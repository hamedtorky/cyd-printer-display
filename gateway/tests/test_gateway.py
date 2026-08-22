from __future__ import annotations

import httpx
from fastapi.testclient import TestClient
from pathlib import Path

from app.analyzers import MockAnalyzer
from app.config import Settings
from app.main import create_app


def moonraker_response(_: httpx.Request) -> httpx.Response:
    if _.url.path.startswith("/server/files/gcodes/"):
        return httpx.Response(
            200,
            content=(
                b";LAYER:0\n;TYPE:WALL-OUTER\nM82\nG92 E0\n"
                b"G1 X0 Y0 Z0.2 E0\nG1 X20 Y0 E1\nG1 X20 Y20 E2\n"
                b"G1 X0 Y20 E3\nG1 X0 Y0 E4\n"
            ),
        )
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


def build_client(token: str = "", firmware_dir: str = "/missing-firmware") -> TestClient:
    settings = Settings(
        gateway_token=token,
        min_analysis_interval_seconds=10,
        moonraker_url="http://moonraker",
        camera_snapshot_url="http://camera/snapshot",
        firmware_dir=firmware_dir,
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
        assert client.get("/v1/model-preview").status_code == 401
        assert client.get(
            "/v1/assessment", headers={"X-Display-Token": "secret"}
        ).status_code == 200


def test_background_analysis_start() -> None:
    with build_client() as client:
        response = client.post("/v1/analyze/start")
        assert response.status_code == 202
        assert response.json() == {"accepted": True, "status": "started"}


def test_model_preview_is_rgb565() -> None:
    with build_client() as client:
        response = client.get("/v1/model-preview")
        assert response.status_code == 200
        assert response.headers["content-type"] == "application/x-rgb565"
        assert response.headers["x-preview-width"] == "220"
        assert response.headers["x-preview-height"] == "145"
        assert len(response.content) == 220 * 145 * 2


def test_ota_manifest_and_image(tmp_path: Path) -> None:
    firmware = b"test-esp32-firmware"
    (tmp_path / "firmware.bin").write_bytes(firmware)
    (tmp_path / "version.txt").write_text("0.4.0\n", encoding="utf-8")
    with build_client(firmware_dir=str(tmp_path)) as client:
        manifest = client.get("/v1/firmware/manifest")
        assert manifest.status_code == 200
        assert manifest.json() == {
            "version": "0.4.0",
            "size": len(firmware),
            "sha256": "32d893f7584744edc9f241f37070f763e61d590a0007c8ddc0dfde33de16ec78",
            "path": "/v1/firmware/image",
        }
        image = client.get("/v1/firmware/image")
        assert image.status_code == 200
        assert image.content == firmware
