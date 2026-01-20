#!/usr/bin/env python3

import os
import numpy as np
from PIL import Image, ImageFilter
import math
# Height is fixed
HEIGHT = 720

# Width is random: between 1x and 2x the height (inclusive)
WIDTH_MIN = HEIGHT
WIDTH_MAX = HEIGHT * 2

# Output folder = folder this script lives in
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUTPUT_FOLDER = SCRIPT_DIR

NUM_IMAGES = 10

# Fog shaping
ALPHA_GAMMA = 2.2   # higher => less mid haze
ALPHA_GAIN = 1.35   # overall strength

# Edge fade: keep full opacity inside the inner 90% region,
# fade to 0 at the edges across the outer 5% per side.
EDGE_FULL_PAD_FRAC = 0.05  # 5% per side


def _random_warp_field(h: int, w: int, rng: np.random.Generator) -> tuple[np.ndarray, np.ndarray]:
    """Smooth random displacement field (dx, dy) in pixels."""
    base = rng.random((h, w), dtype=np.float32)
    img = Image.fromarray((base * 255).astype(np.uint8), mode="L").filter(
        ImageFilter.GaussianBlur(radius=max(12, min(h, w) / 30))
    )
    f = np.array(img, dtype=np.float32) / 255.0

    base2 = rng.random((h, w), dtype=np.float32)
    img2 = Image.fromarray((base2 * 255).astype(np.uint8), mode="L").filter(
        ImageFilter.GaussianBlur(radius=max(14, min(h, w) / 26))
    )
    g = np.array(img2, dtype=np.float32) / 255.0

    amp = float(rng.uniform(10.0, 28.0))
    dx = (f * 2.0 - 1.0) * amp
    dy = (g * 2.0 - 1.0) * amp
    return dx, dy


def _apply_displacement(field: np.ndarray, dx: np.ndarray, dy: np.ndarray) -> np.ndarray:
    """Bilinear sampling using pixel displacement dx/dy (field float32 0..1)."""
    h, w = field.shape
    yy, xx = np.meshgrid(
        np.arange(h, dtype=np.float32),
        np.arange(w, dtype=np.float32),
        indexing="ij",
    )

    sx = np.clip(xx + dx, 0.0, w - 1.001)
    sy = np.clip(yy + dy, 0.0, h - 1.001)

    x0 = np.floor(sx).astype(np.int32)
    y0 = np.floor(sy).astype(np.int32)
    x1 = np.clip(x0 + 1, 0, w - 1)
    y1 = np.clip(y0 + 1, 0, h - 1)

    fx = sx - x0
    fy = sy - y0

    a = field[y0, x0]
    b = field[y0, x1]
    c = field[y1, x0]
    d = field[y1, x1]

    ab = a * (1.0 - fx) + b * fx
    cd = c * (1.0 - fx) + d * fx
    return ab * (1.0 - fy) + cd * fy


def _vertical_falloff(height: int, width: int, rng: np.random.Generator) -> np.ndarray:
    """
    Stronger fog at bottom, fading upward.
    Adds slight horizontal waviness so it isn't a perfect straight gradient.
    """
    y = np.linspace(0.0, 1.0, height, dtype=np.float32)[:, None]  # 0 top, 1 bottom
    base = y ** float(rng.uniform(1.6, 2.6))

    x = np.linspace(0.0, 1.0, width, dtype=np.float32)[None, :]
    wav = 0.10 * np.sin((x * rng.uniform(6.0, 14.0) + rng.uniform(0.0, 6.28318)) * 2.0 * np.pi)
    return np.clip(base + wav, 0.0, 1.0)


def _edge_fade_mask(width: int, height: int, full_pad_frac: float) -> np.ndarray:
    """
    1.0 in the inner region, fades to 0.0 at the image borders.
    full_pad_frac is the fraction per side that stays at full strength
    before the fade begins.
    """
    full_pad_frac = float(np.clip(full_pad_frac, 0.0, 0.49))

    # Coordinate in [0..1]
    x = np.linspace(0.0, 1.0, width, dtype=np.float32)[None, :]
    y = np.linspace(0.0, 1.0, height, dtype=np.float32)[:, None]

    # Distance to nearest edge in normalized units
    dx = np.minimum(x, 1.0 - x)
    dy = np.minimum(y, 1.0 - y)

    # Start fading when we enter the outer band
    # inner boundary distance in normalized units:
    # for 5% padding, inner boundary is at 0.05 from each side
    inner = full_pad_frac
    outer = 0.0

    def ramp(d: np.ndarray) -> np.ndarray:
        # d in [0..0.5], want:
        # d >= inner -> 1
        # d <= outer -> 0
        t = (d - outer) / max(inner - outer, 1e-6)
        return np.clip(t, 0.0, 1.0)

    mx = ramp(dx)
    my = ramp(dy)

    # Multiply so corners also fade naturally
    m = mx * my

    # Smooth the curve so the fade looks soft
    m = m ** 1.8
    return m.astype(np.float32)


def generate_fog_alpha(width: int, height: int, rng: np.random.Generator) -> np.ndarray:
    """
    Alpha-only fog field (white color, varying alpha).
    Random internal shape (warped), bottom-heavy, and edge-faded.
    """
    # Big shapes
    n1 = rng.random((height, width), dtype=np.float32)
    img1 = Image.fromarray((n1 * 255).astype(np.uint8), mode="L").filter(
        ImageFilter.GaussianBlur(radius=rng.uniform(20.0, 34.0))
    )
    a1 = np.array(img1, dtype=np.float32) / 255.0

    # Medium shapes
    n2 = rng.random((height, width), dtype=np.float32)
    img2 = Image.fromarray((n2 * 255).astype(np.uint8), mode="L").filter(
        ImageFilter.GaussianBlur(radius=rng.uniform(8.0, 14.0))
    )
    a2 = np.array(img2, dtype=np.float32) / 255.0

    # Fine detail
    n3 = rng.random((height, width), dtype=np.float32)
    img3 = Image.fromarray((n3 * 255).astype(np.uint8), mode="L").filter(
        ImageFilter.GaussianBlur(radius=rng.uniform(2.0, 4.0))
    )
    a3 = np.array(img3, dtype=np.float32) / 255.0

    # Mix
    alpha = 0.62 * a1 + 0.26 * a2 + 0.12 * a3

    # Warp to reduce circular look
    dx, dy = _random_warp_field(height, width, rng)
    alpha = _apply_displacement(alpha.astype(np.float32), dx, dy)

    # Wispy modulation
    streak = rng.random((height, width), dtype=np.float32)
    streak_img = Image.fromarray((streak * 255).astype(np.uint8), mode="L").filter(
        ImageFilter.GaussianBlur(radius=rng.uniform(6.0, 12.0))
    )
    streak_f = np.array(streak_img, dtype=np.float32) / 255.0
    dx2, dy2 = _random_warp_field(height, width, rng)
    streak_f = _apply_displacement(streak_f.astype(np.float32), dx2 * 0.6, dy2 * 0.6)
    alpha *= (0.75 + 0.5 * streak_f)

    # Normalize 0..1
    alpha -= float(alpha.min())
    mx = float(alpha.max())
    if mx > 1e-8:
        alpha /= mx

    # Bottom-heavy fade
    alpha *= _vertical_falloff(height, width, rng)

    # Edge fade so it doesn't look cut off
    alpha *= _edge_fade_mask(width, height, EDGE_FULL_PAD_FRAC)

    # Contrast shaping
    alpha = np.clip(alpha * ALPHA_GAIN, 0.0, 1.0)
    alpha = alpha ** ALPHA_GAMMA

    return (alpha * 255.0).clip(0, 255).astype(np.uint8)


def build_fog_texture(width: int, height: int, index: int, rng: np.random.Generator) -> None:
    alpha = generate_fog_alpha(width, height, rng)

    rgb = np.full((height, width), 255, dtype=np.uint8)
    rgba = np.dstack([rgb, rgb, rgb, (alpha//10)])

    img = Image.fromarray(rgba, mode="RGBA")

    os.makedirs(OUTPUT_FOLDER, exist_ok=True)
    out_path = os.path.join(OUTPUT_FOLDER, f"fog_{index}.png")
    img.save(out_path, format="PNG")
    print(f"Saved {out_path}  ({width}x{height})")


def main():
    rng = np.random.default_rng()

    for i in range(1, NUM_IMAGES + 1):
        w = int(rng.integers(WIDTH_MIN, WIDTH_MAX + 1))
        build_fog_texture(w, HEIGHT, i, rng)


if __name__ == "__main__":
    main()
