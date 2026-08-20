from __future__ import annotations

import os
from dataclasses import dataclass


@dataclass(frozen=True)
class Settings:
    openai_api_key: str = ""
    openai_model: str = "gpt-5.6-luna"
    ai_provider: str = "mock"
    ai_base_url: str = ""
    moonraker_url: str = "http://127.0.0.1:7125"
    moonraker_api_key: str = ""
    camera_snapshot_url: str = "http://127.0.0.1:8080/?action=snapshot"
    gateway_token: str = ""
    min_analysis_interval_seconds: int = 60
    max_image_bytes: int = 5_000_000

    @classmethod
    def from_environment(cls) -> "Settings":
        return cls(
            openai_api_key=os.getenv("OPENAI_API_KEY", ""),
            openai_model=os.getenv("OPENAI_MODEL", "gpt-5.6-luna"),
            ai_provider=os.getenv("AI_PROVIDER", "mock").lower(),
            ai_base_url=os.getenv("AI_BASE_URL", ""),
            moonraker_url=os.getenv("MOONRAKER_URL", "http://127.0.0.1:7125").rstrip("/"),
            moonraker_api_key=os.getenv("MOONRAKER_API_KEY", ""),
            camera_snapshot_url=os.getenv(
                "CAMERA_SNAPSHOT_URL", "http://127.0.0.1:8080/?action=snapshot"
            ),
            gateway_token=os.getenv("GATEWAY_TOKEN", ""),
            min_analysis_interval_seconds=max(
                10, int(os.getenv("MIN_ANALYSIS_INTERVAL_SECONDS", "60"))
            ),
            max_image_bytes=max(100_000, int(os.getenv("MAX_IMAGE_BYTES", "5000000"))),
        )

