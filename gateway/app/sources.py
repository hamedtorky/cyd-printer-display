from __future__ import annotations

import httpx
from urllib.parse import quote

from .config import Settings
from .models import PrinterTelemetry


class SourceError(RuntimeError):
    pass


async def fetch_active_gcode(settings: Settings, client: httpx.AsyncClient) -> tuple[str, bytes]:
    headers = {"X-Api-Key": settings.moonraker_api_key} if settings.moonraker_api_key else {}
    try:
        status_response = await client.get(
            f"{settings.moonraker_url}/printer/objects/query",
            params={"print_stats": "state,filename"},
            headers=headers,
        )
        status_response.raise_for_status()
        filename = status_response.json()["result"]["status"]["print_stats"]["filename"]
        if not filename:
            raise SourceError("no active G-code file")

        endpoint = f"{settings.moonraker_url}/server/files/gcodes/{quote(filename, safe='/')}"
        async with client.stream("GET", endpoint, headers=headers) as response:
            response.raise_for_status()
            chunks: list[bytes] = []
            size = 0
            async for chunk in response.aiter_bytes():
                size += len(chunk)
                if size > settings.max_gcode_bytes:
                    raise SourceError("G-code exceeds MAX_GCODE_BYTES")
                chunks.append(chunk)
        gcode = b"".join(chunks)
        if not gcode:
            raise SourceError("active G-code file is empty")
        return filename, gcode
    except SourceError:
        raise
    except (httpx.HTTPError, KeyError, TypeError, ValueError) as exc:
        raise SourceError(f"active G-code request failed: {exc}") from exc


async def fetch_camera(settings: Settings, client: httpx.AsyncClient) -> tuple[bytes, str]:
    try:
        async with client.stream("GET", settings.camera_snapshot_url) as response:
            response.raise_for_status()
            content_type = response.headers.get("content-type", "image/jpeg").split(";", 1)[0]
            if not content_type.startswith("image/"):
                raise SourceError(f"camera returned {content_type}, not an image")
            chunks: list[bytes] = []
            size = 0
            async for chunk in response.aiter_bytes():
                size += len(chunk)
                if size > settings.max_image_bytes:
                    raise SourceError("camera image exceeds MAX_IMAGE_BYTES")
                chunks.append(chunk)
            image = b"".join(chunks)
            if not image:
                raise SourceError("camera returned an empty image")
            return image, content_type
    except httpx.HTTPError as exc:
        raise SourceError(f"camera request failed: {exc}") from exc


async def fetch_telemetry(settings: Settings, client: httpx.AsyncClient) -> PrinterTelemetry:
    endpoint = f"{settings.moonraker_url}/printer/objects/query"
    params = {
        "print_stats": "state,filename,print_duration",
        "virtual_sdcard": "progress",
        "extruder": "temperature,target",
        "heater_bed": "temperature,target",
        "gcode_move": "speed_factor,extrude_factor",
        "filament_switch_sensor sfs_switch": "filament_detected",
        "filament_motion_sensor sfs_motion": "filament_detected",
    }
    headers = {"X-Api-Key": settings.moonraker_api_key} if settings.moonraker_api_key else {}
    try:
        response = await client.get(endpoint, params=params, headers=headers)
        response.raise_for_status()
        status = response.json()["result"]["status"]
    except (httpx.HTTPError, KeyError, TypeError, ValueError) as exc:
        raise SourceError(f"Moonraker request failed: {exc}") from exc

    print_stats = status.get("print_stats", {})
    virtual_sdcard = status.get("virtual_sdcard", {})
    extruder = status.get("extruder", {})
    bed = status.get("heater_bed", {})
    move = status.get("gcode_move", {})
    switch = status.get("filament_switch_sensor sfs_switch", {})
    motion = status.get("filament_motion_sensor sfs_motion", {})
    return PrinterTelemetry(
        print_state=print_stats.get("state", "unknown"),
        filename=print_stats.get("filename", ""),
        progress=virtual_sdcard.get("progress", 0),
        print_duration_seconds=print_stats.get("print_duration", 0),
        nozzle_temperature=extruder.get("temperature", 0),
        nozzle_target=extruder.get("target", 0),
        bed_temperature=bed.get("temperature", 0),
        bed_target=bed.get("target", 0),
        speed_percent=round(move.get("speed_factor", 1) * 100),
        flow_percent=round(move.get("extrude_factor", 1) * 100),
        filament_present=switch.get("filament_detected"),
        filament_motion_ok=motion.get("filament_detected"),
    )
