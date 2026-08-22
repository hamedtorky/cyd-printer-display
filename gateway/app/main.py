from __future__ import annotations

import asyncio
import hashlib
import secrets
import time
from contextlib import asynccontextmanager
from pathlib import Path

import httpx
from fastapi import Depends, FastAPI, Header, HTTPException, status
from fastapi.responses import FileResponse, Response

from .analyzers import Analyzer, create_analyzer
from .config import Settings
from .models import AnalysisStart, Assessment, Health
from .preview import HEIGHT as PREVIEW_HEIGHT
from .preview import WIDTH as PREVIEW_WIDTH
from .preview import PreviewError, render_preview
from .sources import SourceError, fetch_active_gcode, fetch_camera, fetch_telemetry


class GatewayState:
    def __init__(self, settings: Settings, analyzer: Analyzer):
        self.settings = settings
        self.analyzer = analyzer
        self.client = httpx.AsyncClient(timeout=httpx.Timeout(10, connect=3))
        self.latest: Assessment | None = None
        self.last_started_at = 0.0
        self.lock = asyncio.Lock()
        self.preview_lock = asyncio.Lock()
        self.preview_filename = ""
        self.preview_digest = b""
        self.preview_pixels = b""
        self.task: asyncio.Task[None] | None = None

    async def close(self) -> None:
        if self.task and not self.task.done():
            self.task.cancel()
        await self.client.aclose()

    def reserve_analysis(self) -> None:
        now = time.monotonic()
        retry_after = self.settings.min_analysis_interval_seconds - (now - self.last_started_at)
        if retry_after > 0:
            raise HTTPException(
                status_code=status.HTTP_429_TOO_MANY_REQUESTS,
                detail="analysis rate limit active",
                headers={"Retry-After": str(max(1, int(retry_after)))},
            )
        self.last_started_at = now

    async def perform_analysis(self) -> Assessment:
        telemetry, (image, media_type) = await asyncio.gather(
            fetch_telemetry(self.settings, self.client),
            fetch_camera(self.settings, self.client),
        )
        assessment = await self.analyzer.analyze(image, media_type, telemetry)
        self.latest = assessment
        return assessment

    async def perform_background_analysis(self) -> None:
        try:
            await self.perform_analysis()
        except Exception as exc:
            source = "mock" if self.settings.ai_provider == "mock" else (
                "local" if self.settings.ai_base_url else "openai"
            )
            self.latest = Assessment(
                status="unknown",
                summary="Analysis failed",
                recommendation=str(exc)[:180],
                confidence=0,
                source=source,
            )


def create_app(settings: Settings | None = None, analyzer: Analyzer | None = None) -> FastAPI:
    resolved = settings or Settings.from_environment()
    resolved_analyzer = analyzer or create_analyzer(resolved)
    state_holder = GatewayState(resolved, resolved_analyzer)

    @asynccontextmanager
    async def lifespan(_: FastAPI):
        yield
        await state_holder.close()

    app = FastAPI(title="CYD Printer AI Gateway", version="0.1.0", lifespan=lifespan)
    app.state.gateway = state_holder

    def authorize(x_display_token: str = Header(default="")) -> None:
        expected = state_holder.settings.gateway_token
        if expected and not secrets.compare_digest(x_display_token, expected):
            raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="invalid token")

    @app.get("/health", response_model=Health)
    async def health() -> Health:
        return Health(
            provider=state_holder.settings.ai_provider,
            model=state_holder.settings.openai_model,
            analysis_available=state_holder.latest is not None,
            analysis_running=state_holder.task is not None and not state_holder.task.done(),
        )

    @app.get("/v1/assessment", response_model=Assessment | None, dependencies=[Depends(authorize)])
    async def latest_assessment() -> Assessment | None:
        return state_holder.latest

    @app.get("/v1/model-preview", dependencies=[Depends(authorize)])
    async def model_preview() -> Response:
        async with state_holder.preview_lock:
            try:
                filename, gcode = await fetch_active_gcode(
                    state_holder.settings, state_holder.client
                )
                digest = hashlib.blake2s(gcode).digest()
                if (filename != state_holder.preview_filename or
                        digest != state_holder.preview_digest or
                        not state_holder.preview_pixels):
                    state_holder.preview_pixels = await asyncio.to_thread(render_preview, gcode)
                    state_holder.preview_filename = filename
                    state_holder.preview_digest = digest
            except (SourceError, PreviewError) as exc:
                raise HTTPException(
                    status_code=status.HTTP_502_BAD_GATEWAY, detail=str(exc)
                ) from exc
        return Response(
            content=state_holder.preview_pixels,
            media_type="application/x-rgb565",
            headers={
                "X-Preview-Width": str(PREVIEW_WIDTH),
                "X-Preview-Height": str(PREVIEW_HEIGHT),
                "Cache-Control": "no-store",
            },
        )

    def firmware_files() -> tuple[Path, Path]:
        directory = Path(state_holder.settings.firmware_dir)
        return directory / "firmware.bin", directory / "version.txt"

    @app.get("/v1/firmware/manifest", dependencies=[Depends(authorize)])
    async def firmware_manifest() -> dict[str, str | int]:
        image_path, version_path = firmware_files()
        if not image_path.is_file() or not version_path.is_file():
            raise HTTPException(
                status_code=status.HTTP_404_NOT_FOUND, detail="OTA firmware is not published"
            )
        version = version_path.read_text(encoding="utf-8").strip()
        if not version or len(version) > 32:
            raise HTTPException(
                status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
                detail="invalid OTA firmware version",
            )
        image_size = image_path.stat().st_size
        digest = await asyncio.to_thread(
            lambda: hashlib.sha256(image_path.read_bytes()).hexdigest()
        )
        return {
            "version": version,
            "size": image_size,
            "sha256": digest,
            "path": "/v1/firmware/image",
        }

    @app.get("/v1/firmware/image", dependencies=[Depends(authorize)])
    async def firmware_image() -> FileResponse:
        image_path, _ = firmware_files()
        if not image_path.is_file():
            raise HTTPException(
                status_code=status.HTTP_404_NOT_FOUND, detail="OTA firmware is not published"
            )
        return FileResponse(
            image_path,
            media_type="application/octet-stream",
            filename="cyd-printer-display.bin",
            headers={"Cache-Control": "no-store"},
        )

    @app.post("/v1/analyze", response_model=Assessment, dependencies=[Depends(authorize)])
    async def analyze() -> Assessment:
        async with state_holder.lock:
            if state_holder.task and not state_holder.task.done():
                raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="analysis running")
            state_holder.reserve_analysis()
            try:
                assessment = await state_holder.perform_analysis()
            except SourceError as exc:
                raise HTTPException(status_code=status.HTTP_502_BAD_GATEWAY, detail=str(exc)) from exc
            except Exception as exc:
                raise HTTPException(
                    status_code=status.HTTP_502_BAD_GATEWAY,
                    detail=f"analysis provider failed: {exc}",
                ) from exc
            return assessment

    @app.post(
        "/v1/analyze/start",
        response_model=AnalysisStart,
        status_code=status.HTTP_202_ACCEPTED,
        dependencies=[Depends(authorize)],
    )
    async def start_analysis() -> AnalysisStart:
        async with state_holder.lock:
            if state_holder.task and not state_holder.task.done():
                return AnalysisStart(accepted=False, status="running")
            state_holder.reserve_analysis()
            state_holder.task = asyncio.create_task(state_holder.perform_background_analysis())
            return AnalysisStart(accepted=True, status="started")

    return app


app = create_app()
