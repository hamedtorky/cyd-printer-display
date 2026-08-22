#!/usr/bin/env python3
"""Render Cura G-code extrusion paths and emit a compact ESP32 RGB565 preview."""

from __future__ import annotations

import argparse
import math
import re
from collections import defaultdict
from pathlib import Path

from PIL import Image, ImageDraw


MOVE = re.compile(r"([XYZEF])(-?\d+(?:\.\d+)?)")
VISIBLE_FEATURES = {"WALL-OUTER", "WALL-INNER", "SKIN"}


def parse_segments(path: Path):
    position = {"X": 0.0, "Y": 0.0, "Z": 0.0, "E": 0.0}
    absolute_extrusion = True
    feature = ""
    started = False
    layers: dict[float, list[tuple[float, float, float, float]]] = defaultdict(list)

    with path.open("r", encoding="utf-8", errors="ignore") as source:
        for raw_line in source:
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
                values = {key: float(value) for key, value in MOVE.findall(line)}
                position.update(values)
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
            if started and moved_xy and extrusion_delta > 0 and feature in VISIBLE_FEATURES:
                z = round(next_position["Z"], 3)
                layers[z].append(
                    (position["X"], position["Y"], next_position["X"], next_position["Y"])
                )
            position = next_position
    return layers


def render(layers, width: int, height: int) -> Image.Image:
    if not layers:
        raise RuntimeError("no visible extrusion paths found")

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
                (
                    x1 - y1,
                    (x1 + y1) * 0.42 - z * 3.2,
                    x2 - y2,
                    (x2 + y2) * 0.42 - z * 3.2,
                    z,
                )
            )

    min_x = min(min(item[0], item[2]) for item in projected)
    max_x = max(max(item[0], item[2]) for item in projected)
    min_y = min(min(item[1], item[3]) for item in projected)
    max_y = max(max(item[1], item[3]) for item in projected)
    scale = min((width - 12) / max(max_x - min_x, 1), (height - 12) / max(max_y - min_y, 1))
    oversample = 3
    canvas = Image.new("RGB", (width * oversample, height * oversample), (0, 0, 0))
    draw = ImageDraw.Draw(canvas)

    def point(x, y):
        px = (6 + (x - min_x) * scale) * oversample
        py = (height - 6 - (y - min_y) * scale) * oversample
        return px, py

    for x1, y1, x2, y2, z in projected:
        fraction = z / max(z_max, 0.001)
        color = (
            255,
            int(45 + 205 * fraction),
            int(20 + 25 * fraction),
        )
        draw.line((*point(x1, y1), *point(x2, y2)), fill=color, width=2 * oversample)

    return canvas.resize((width, height), Image.Resampling.LANCZOS)


def rgb565(red: int, green: int, blue: int) -> int:
    return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)


def write_header(image: Image.Image, output: Path) -> None:
    pixels = [rgb565(*pixel) for pixel in image.getdata()]
    lines = []
    for start in range(0, len(pixels), 16):
        values = ", ".join(f"0x{value:04X}" for value in pixels[start : start + 16])
        lines.append(f"  {values},")
    output.write_text(
        "#pragma once\n\n"
        "#include <Arduino.h>\n\n"
        f"constexpr uint16_t kModelPreviewWidth = {image.width};\n"
        f"constexpr uint16_t kModelPreviewHeight = {image.height};\n"
        "const uint16_t kModelPreview[] PROGMEM = {\n"
        + "\n".join(lines)
        + "\n};\n",
        encoding="utf-8",
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("gcode", type=Path)
    parser.add_argument("--png", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--width", type=int, default=220)
    parser.add_argument("--height", type=int, default=145)
    arguments = parser.parse_args()

    layers = parse_segments(arguments.gcode)
    image = render(layers, arguments.width, arguments.height)
    arguments.png.parent.mkdir(parents=True, exist_ok=True)
    arguments.header.parent.mkdir(parents=True, exist_ok=True)
    image.save(arguments.png)
    write_header(image, arguments.header)
    segment_count = sum(len(segments) for segments in layers.values())
    print(f"Rendered {segment_count} segments across {len(layers)} layers")


if __name__ == "__main__":
    main()
