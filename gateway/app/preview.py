from __future__ import annotations

import base64
import io
import math
import re
from collections import defaultdict

from PIL import Image, ImageDraw


WIDTH = 220
HEIGHT = 145
MOVE = re.compile(r"([XYZEF])(-?\d+(?:\.\d+)?)")
THUMBNAIL = re.compile(
    rb";\s*thumbnail(?:_JPG)?\s+begin\s+\d+x\d+\s+\d+\s*\r?\n"
    rb"(?P<data>(?:;[^\r\n]*\r?\n)+?)"
    rb";\s*thumbnail(?:_JPG)?\s+end",
    re.IGNORECASE,
)
VISIBLE_FEATURES = {"WALL-OUTER", "WALL-INNER", "SKIN"}


class PreviewError(RuntimeError):
    pass


def _embedded_thumbnail(gcode: bytes) -> Image.Image | None:
    match = THUMBNAIL.search(gcode)
    if not match:
        return None
    encoded = b"".join(
        line.lstrip()[1:].strip() for line in match.group("data").splitlines() if line.lstrip()
    )
    try:
        image = Image.open(io.BytesIO(base64.b64decode(encoded, validate=True)))
        image.load()
        return image.convert("RGBA")
    except Exception:
        return None


def _parse_segments(gcode: bytes):
    position = {"X": 0.0, "Y": 0.0, "Z": 0.0, "E": 0.0}
    absolute_extrusion = True
    feature = ""
    started = False
    layers: dict[float, list[tuple[float, float, float, float]]] = defaultdict(list)

    for raw_line in gcode.decode("utf-8", errors="ignore").splitlines():
        line = raw_line.strip()
        if line.startswith(";LAYER:"):
            started = True
            continue
        if line.startswith(";TYPE:"):
            feature = line[6:].strip()
            continue
        if line.startswith("M82"):
            absolute_extrusion = True
            continue
        if line.startswith("M83"):
            absolute_extrusion = False
            continue
        if line.startswith("G92"):
            position.update({key: float(value) for key, value in MOVE.findall(line)})
            continue
        if not (line.startswith("G0 ") or line.startswith("G1 ")):
            continue

        values = {key: float(value) for key, value in MOVE.findall(line)}
        next_position = position.copy()
        for axis in ("X", "Y", "Z"):
            if axis in values:
                next_position[axis] = values[axis]
        extrusion_delta = 0.0
        if "E" in values:
            if absolute_extrusion:
                extrusion_delta = values["E"] - position["E"]
                next_position["E"] = values["E"]
            else:
                extrusion_delta = values["E"]
                next_position["E"] = position["E"] + values["E"]

        moved_xy = next_position["X"] != position["X"] or next_position["Y"] != position["Y"]
        visible = not feature or feature in VISIBLE_FEATURES
        if started and moved_xy and extrusion_delta > 0 and visible:
            layers[round(next_position["Z"], 3)].append(
                (position["X"], position["Y"], next_position["X"], next_position["Y"])
            )
        position = next_position
    return layers


def _render_paths(gcode: bytes) -> Image.Image:
    layers = _parse_segments(gcode)
    if not layers:
        raise PreviewError("no visible extrusion paths found")

    z_values = sorted(layers)
    layer_step = max(1, math.ceil(len(z_values) / 48))
    selected = z_values[::layer_step]
    if selected[-1] != z_values[-1]:
        selected.append(z_values[-1])

    projected = []
    z_max = max(z_values)
    for z in selected:
        for x1, y1, x2, y2 in layers[z]:
            projected.append(
                (x1 - y1, (x1 + y1) * 0.42 - z * 3.2,
                 x2 - y2, (x2 + y2) * 0.42 - z * 3.2, z)
            )

    min_x = min(min(item[0], item[2]) for item in projected)
    max_x = max(max(item[0], item[2]) for item in projected)
    min_y = min(min(item[1], item[3]) for item in projected)
    max_y = max(max(item[1], item[3]) for item in projected)
    scale = min((WIDTH - 12) / max(max_x - min_x, 1), (HEIGHT - 12) / max(max_y - min_y, 1))
    oversample = 3
    canvas = Image.new("RGB", (WIDTH * oversample, HEIGHT * oversample), "black")
    draw = ImageDraw.Draw(canvas)

    def point(x: float, y: float) -> tuple[float, float]:
        return ((6 + (x - min_x) * scale) * oversample,
                (HEIGHT - 6 - (y - min_y) * scale) * oversample)

    for x1, y1, x2, y2, z in projected:
        fraction = z / max(z_max, 0.001)
        color = (255, int(45 + 205 * fraction), int(20 + 25 * fraction))
        draw.line((*point(x1, y1), *point(x2, y2)), fill=color, width=2 * oversample)
    return canvas.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)


def _fit(image: Image.Image) -> Image.Image:
    converted = image.convert("RGB")
    converted.thumbnail((WIDTH - 4, HEIGHT - 4), Image.Resampling.LANCZOS)
    canvas = Image.new("RGB", (WIDTH, HEIGHT), "black")
    canvas.paste(converted, ((WIDTH - converted.width) // 2, (HEIGHT - converted.height) // 2))
    return canvas


def _rgb565_little_endian(image: Image.Image) -> bytes:
    output = bytearray(WIDTH * HEIGHT * 2)
    for index, (red, green, blue) in enumerate(image.getdata()):
        value = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
        output[index * 2] = value & 0xFF
        output[index * 2 + 1] = value >> 8
    return bytes(output)


def render_preview(gcode: bytes) -> bytes:
    thumbnail = _embedded_thumbnail(gcode)
    image = _fit(thumbnail) if thumbnail is not None else _render_paths(gcode)
    return _rgb565_little_endian(image)
