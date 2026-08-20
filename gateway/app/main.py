from __future__ import annotations

import asyncio
import secrets
import time
from contextlib import asynccontextmanager

import httpx
from fastapi import Depends, FastAPI, Header, HTTPException, status

from .analyzers import Analyzer, create_analyzer
from .config import Settings
from .models import Assessment, Health
from .sources import SourceError, fetch_camera, fetch_telemetry


class GatewayState:
    def __init__(self, settings: Settings, analyzer: Analyzer):
        self.settings = settings
        self.analyzer = analyzer
        self.client = httpx.AsyncClient(timeout=httpx.Timeout(10, connect=3))
        self.latest: Assessment | None = None
        self.last_started_at = 0.0
        self.lock = asyncio.Lock()

    async def close(self) -> None:
        await self.client.aclose()


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
        )

    @app.get("/v1/assessment", response_model=Assessment | None, dependencies=[Depends(authorize)])
    async def latest_assessment() -> Assessment | None:
        return state_holder.latest

    @app.post("/v1/analyze", response_model=Assessment, dependencies=[Depends(authorize)])
    async def analyze() -> Assessment:
        async with state_holder.lock:
            now = time.monotonic()
            retry_after = state_holder.settings.min_analysis_interval_seconds - (
                now - state_holder.last_started_at
            )
            if retry_after > 0:
                raise HTTPException(
                    status_code=status.HTTP_429_TOO_MANY_REQUESTS,
                    detail="analysis rate limit active",
                    headers={"Retry-After": str(max(1, int(retry_after)))},
                )
            state_holder.last_started_at = now
            try:
                telemetry, (image, media_type) = await asyncio.gather(
                    fetch_telemetry(state_holder.settings, state_holder.client),
                    fetch_camera(state_holder.settings, state_holder.client),
                )
                assessment = await state_holder.analyzer.analyze(image, media_type, telemetry)
            except SourceError as exc:
                raise HTTPException(status_code=status.HTTP_502_BAD_GATEWAY, detail=str(exc)) from exc
            except Exception as exc:
                raise HTTPException(
                    status_code=status.HTTP_502_BAD_GATEWAY,
                    detail=f"analysis provider failed: {exc}",
                ) from exc
            state_holder.latest = assessment
            return assessment

    return app


app = create_app()
