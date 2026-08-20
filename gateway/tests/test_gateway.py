from __future__ import annotations

import httpx
from fastapi.testclient import TestClient

from app.analyzers import MockAnalyzer
from app.config import Settings
from app.main import create_app


def moonraker_response(_: httpx.Request) -> httpx.Response:
    return httpx.Response(
        200,
        json={
            "result": {
                "status": {
                    "print_stats": {
                        "state": "printing",
                        "filename": "cube.gcode",
                        "print_duration": 120,
                    },
                    "virtual_sdcard": {"progress": 0.25},
                    "extruder": {"temperature": 210, "target": 210},
                    "heater_bed": {"temperature": 60, "target": 60},
                    "gcode_move": {"speed_factor": 1, "extrude_factor": 1},
                    "filament_switch_sensor sfs_switch": {"filament_detected": True},
                    "filament_motion_sensor sfs_motion": {"filament_detected": True},
                }
            }
        },
    )


def camera_response(_: httpx.Request) -> httpx.Response:
    return httpx.Response(200, content=b"fake-jpeg", headers={"content-type": "image/jpeg"})


def build_client(token: str = "") -> TestClient:
    settings = Settings(
        gateway_token=token,
        min_analysis_interval_seconds=10,
        moonraker_url="http://moonraker",
        camera_snapshot_url="http://camera/snapshot",
    )
    app = create_app(settings, MockAnalyzer())
    transport = httpx.MockTransport(
        lambda request: moonraker_response(request)
        if request.url.host == "moonraker"
        else camera_response(request)
    )
    app.state.gateway.client = httpx.AsyncClient(transport=transport)
    return TestClient(app)


def test_health_and_mock_analysis() -> None:
    with build_client() as client:
        assert client.get("/health").json()["status"] == "ok"
        response = client.post("/v1/analyze")
        assert response.status_code == 200
        assert response.json()["status"] == "good"
        assert client.get("/v1/assessment").json()["source"] == "mock"


def test_rate_limit() -> None:
    with build_client() as client:
        assert client.post("/v1/analyze").status_code == 200
        response = client.post("/v1/analyze")
        assert response.status_code == 429
        assert int(response.headers["retry-after"]) >= 1


def test_optional_display_token() -> None:
    with build_client("secret") as client:
        assert client.get("/v1/assessment").status_code == 401
        assert client.get(
            "/v1/assessment", headers={"X-Display-Token": "secret"}
        ).status_code == 200


def test_background_analysis_start() -> None:
    with build_client() as client:
        response = client.post("/v1/analyze/start")
        assert response.status_code == 202
        assert response.json() == {"accepted": True, "status": "started"}
