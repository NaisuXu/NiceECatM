# Copyright (C) 2026 Naisu Xu
# SPDX-License-Identifier: GPL-3.0-only

"""Optional artwork tool: regenerate the checked-in ICO with Pillow.

Normal CMake builds use NiceECatM.ico directly and require no Python packages.
Geometry matches NiceECatM.svg and the existing in-app network mark.
"""

from pathlib import Path

from PIL import Image, ImageDraw


def render(size):
    scale = 4
    image = Image.new("RGBA", (size * scale, size * scale))
    draw = ImageDraw.Draw(image)
    unit = size * scale / 256

    def box(bounds, radius, fill=None, outline=None, width=1):
        draw.rounded_rectangle(
            tuple(round(n * unit) for n in bounds),
            radius=round(radius * unit),
            fill=fill,
            outline=outline,
            width=max(1, round(width * unit)),
        )

    box((8, 8, 248, 248), 48, fill="#0c8f99")
    for points in (((128, 103), (128, 141)), ((75, 141), (181, 141)),
                   ((75, 141), (75, 168)), ((181, 141), (181, 168))):
        draw.line([(round(x * unit), round(y * unit)) for x, y in points],
                  fill="white", width=max(1, round(8 * unit)))
    for bounds, radius in (((75, 56, 181, 103), 8),
                           ((55, 168, 95, 195), 5), ((161, 168, 201, 195), 5)):
        box(bounds, radius, outline="white", width=8)
    return image.resize((size, size), Image.Resampling.LANCZOS)


if __name__ == "__main__":
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    images = [render(size) for size in sizes]
    images[-1].save(Path(__file__).with_name("NiceECatM.ico"),
                    sizes=[(size, size) for size in sizes], append_images=images[:-1])
