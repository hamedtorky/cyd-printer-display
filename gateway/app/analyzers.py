from __future__ import annotations

import asyncio
import base64
from typing import Protocol

from openai import OpenAI

from .config import Settings
from .models import Assessment, ModelAssessment, PrinterTelemetry


class Analyzer(Protocol):
    async def analyze(
        self, image: bytes, media_type: str, telemetry: PrinterTelemetry
    ) -> Assessment: ...


class MockAnalyzer:
    async def analyze(
        self, image: bytes, media_type: str, telemetry: PrinterTelemetry
    ) -> Assessment:
        return Assessment(
            status="good",
            summary="Mock analysis: print appearance is acceptable",
            recommendation="Configure OPENAI_API_KEY and AI_PROVIDER=openai for camera analysis.",
            confidence=0.75,
            source="mock",
        )


class OpenAIAnalyzer:
    def __init__(self, settings: Settings):
        if not settings.openai_api_key:
            raise ValueError("OPENAI_API_KEY is required when AI_PROVIDER=openai")
        kwargs: dict[str, str] = {"api_key": settings.openai_api_key}
        if settings.ai_base_url:
            kwargs["base_url"] = settings.ai_base_url
        self.client = OpenAI(**kwargs)
        self.model = settings.openai_model
        self.source = "local" if settings.ai_base_url else "openai"

    async def analyze(
        self, image: bytes, media_type: str, telemetry: PrinterTelemetry
    ) -> Assessment:
        encoded = base64.b64encode(image).decode("ascii")
        telemetry_json = telemetry.model_dump_json()

        def request() -> ModelAssessment:
            response = self.client.responses.parse(
                model=self.model,
                store=False,
                instructions=(
                    "You inspect FDM 3D-printer images. Be conservative: report only visible "
                    "evidence, distinguish uncertainty, and never instruct automatic machine "
                    "movement or heater changes. Keep advice short enough for a 320x240 display."
                ),
                input=[
                    {
                        "role": "user",
                        "content": [
                            {
                                "type": "input_text",
                                "text": (
                                    "Assess the current print for adhesion failure, spaghetti, "
                                    "layer shift, under-extrusion, over-extrusion, or obvious normal "
                                    f"operation. Printer telemetry: {telemetry_json}"
                                ),
                            },
                            {
                                "type": "input_image",
                                "image_url": f"data:{media_type};base64,{encoded}",
                                "detail": "low",
                            },
                        ],
                    }
                ],
                text_format=ModelAssessment,
            )
            if response.output_parsed is None:
                raise RuntimeError("model returned no structured assessment")
            return response.output_parsed

        result = await asyncio.to_thread(request)
        return Assessment(**result.model_dump(), source=self.source)


def create_analyzer(settings: Settings) -> Analyzer:
    if settings.ai_provider == "mock":
        return MockAnalyzer()
    if settings.ai_provider == "openai":
        return OpenAIAnalyzer(settings)
    raise ValueError("AI_PROVIDER must be 'mock' or 'openai'")

