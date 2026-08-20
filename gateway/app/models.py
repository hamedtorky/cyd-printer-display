from __future__ import annotations

from datetime import datetime, timezone
from typing import Literal

from pydantic import BaseModel, Field


class PrinterTelemetry(BaseModel):
    print_state: str = "unknown"
    filename: str = ""
    progress: float = Field(default=0, ge=0, le=1)
    print_duration_seconds: float = Field(default=0, ge=0)
    nozzle_temperature: float = 0
    nozzle_target: float = 0
    bed_temperature: float = 0
    bed_target: float = 0
    speed_percent: int = 100
    flow_percent: int = 100
    filament_present: bool | None = None
    filament_motion_ok: bool | None = None


class ModelAssessment(BaseModel):
    status: Literal["good", "warning", "critical", "unknown"]
    summary: str = Field(max_length=100)
    recommendation: str = Field(max_length=180)
    confidence: float = Field(ge=0, le=1)


class Assessment(ModelAssessment):
    analyzed_at: datetime = Field(default_factory=lambda: datetime.now(timezone.utc))
    source: Literal["openai", "mock", "local"] = "mock"


class Health(BaseModel):
    status: Literal["ok"] = "ok"
    provider: str
    model: str
    analysis_available: bool
    analysis_running: bool = False


class AnalysisStart(BaseModel):
    accepted: bool
    status: Literal["started", "running"]
