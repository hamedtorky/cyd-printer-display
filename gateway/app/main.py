from __future__ import annotations

import asyncio
import secrets
import time
from contextlib import asynccontextmanager

import httpx
from fastapi import Depends, FastAPI, Header, HTTPException, status

from .analyzers import Analyzer, create_analyzer
from .config import Settings
from .models import AnalysisStart, Assessment, Health
from .sources import SourceError, fetch_camera, fetch_telemetry


class GatewayState:
    def __init__(self, settings: Settings, analyzer: Analyzer):
        self.settings = settings
        self.analyzer = analyzer
        self.client = httpx.AsyncClient(timeout=httpx.Timeout(10, connect=3))
        self.latest: Assessment | None = None
        self.last_started_at = 0.0
        self.lock = asyncio.Lock()
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
