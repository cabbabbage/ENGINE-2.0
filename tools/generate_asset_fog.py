#!/usr/bin/env python3
"""Generate per-asset fog textures with a vertical alpha gradient."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Tuple

from PIL import Image


def _parse_size(value: str) -> Tuple[int, int]:
    parts = value.lower().replace("x", " ").split()
    if len(parts) != 2:
        raise ValueError("Size must be formatted as WIDTHxHEIGHT.")
    return int(parts[0]), int(parts[1])


def build_fog_alpha(width: int, height: int) -> Image.Image:
    if width <= 0 or height <= 0:
        raise ValueError("Width and height must be positive.")
    if height == 1:
        alpha = [255]
    else:
        alpha = [int(round(255 * (1.0 - (y / (height - 1))))) for y in range(height)]
    gradient = Image.new("L", (1, height))
    gradient.putdata(alpha)
    return gradient.resize((width, height), resample=Image.NEAREST)


def generate_fog_image(width: int, height: int) -> Image.Image:
    fog = Image.new("RGBA", (width, height), (255, 255, 255, 0))
    fog.putalpha(build_fog_alpha(width, height))
    return fog


def generate_fog_texture(width: int, height: int, output_path: Path) -> None:
    fog = generate_fog_image(width, height)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fog.save(output_path, "PNG", optimize=False)


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate a white fog texture with a top-to-bottom alpha gradient.")
    parser.add_argument("output", type=Path, help="Output PNG path.")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--size", type=_parse_size, help="Texture size as WIDTHxHEIGHT.")
    group.add_argument("--source", type=Path, help="Source image to match dimensions.")
    args = parser.parse_args()

    if args.size:
        width, height = args.size
    else:
        with Image.open(args.source) as img:
            width, height = img.size

    generate_fog_texture(width, height, args.output)


if __name__ == "__main__":
    main()
