#!/usr/bin/env python3

import os
import numpy as np
from PIL import Image, ImageFilter

# Output size
WIDTH = 1080
HEIGHT = 1080

# Output folder = folder this script lives in
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUTPUT_FOLDER = SCRIPT_DIR

NUM_IMAGES = 10

# 30% fully transparent padding around the edges
PAD_FRAC = 0.30

# Make fog "more white, less grey"
# Increase contrast so more pixels become either near-0 alpha or near-255 alpha
ALPHA_GAMMA = 2.4   # higher => pushes mid alphas down (less grey haze)
ALPHA_GAIN = 1.35   # overall strength (before clipping)


def _soft_edge_mask_with_padding(width: int, height: int, pad_frac: float, power: float = 2.2) -> np.ndarray:
    pad_frac = float(np.clip(pad_frac, 0.0, 0.95))

    x = np.linspace(-1.0, 1.0, width, dtype=np.float32)
    y = np.linspace(-1.0, 1.0, height, dtype=np.float32)
    xx, yy = np.meshgrid(x, y)

    r = np.sqrt(xx * xx + yy * yy)
    r /= np.sqrt(2.0)

    inner = 1.0 - pad_frac
    t = np.clip(r / max(inner, 1e-6), 0.0, 1.0)
    mask = (1.0 - t) ** power
    mask[r >= inner] = 0.0
    return mask


def generate_fog_alpha(width: int, height: int) -> np.ndarray:
    """
    Generates an alpha-only fog field (white color, varying alpha).
    The shaping below intentionally reduces mid-alpha "grey haze" by
    pushing values toward 0 or higher opacity.
    """
    rng = np.random.default_rng()

    # Noise layers
    noise1 = rng.random((height, width), dtype=np.float32)
    img1 = Image.fromarray((noise1 * 255).astype(np.uint8), mode="L").filter(
        ImageFilter.GaussianBlur(radius=18)
    )

    noise2 = rng.random((height, width), dtype=np.float32)
    img2 = Image.fromarray((noise2 * 255).astype(np.uint8), mode="L").filter(
        ImageFilter.GaussianBlur(radius=7)
    )

    noise3 = rng.random((height, width), dtype=np.float32)
    img3 = Image.fromarray((noise3 * 255).astype(np.uint8), mode="L").filter(
        ImageFilter.GaussianBlur(radius=2.5)
    )

    a1 = np.array(img1, dtype=np.float32)
    a2 = np.array(img2, dtype=np.float32)
    a3 = np.array(img3, dtype=np.float32)

    # Combine (slightly bias toward bigger shapes so it reads as puffs, not haze)
    alpha = 0.65 * a1 + 0.25 * a2 + 0.10 * a3

    # Normalize to 0..1
    alpha -= alpha.min()
    mx = alpha.max()
    if mx > 0:
        alpha /= mx

    # Contrast shaping: reduce mid values (the "grey" look)
    # 0..1 -> apply gain then gamma
    alpha = np.clip(alpha * ALPHA_GAIN, 0.0, 1.0)
    alpha = alpha ** ALPHA_GAMMA

    # Fade out with fully transparent padding
    edge_mask = _soft_edge_mask_with_padding(width, height, PAD_FRAC, power=2.2)
    alpha *= edge_mask

    # Back to 0..255
    alpha = (alpha * 255.0).clip(0, 255).astype(np.uint8)
    return alpha


def build_fog_texture(width: int, height: int, index: int) -> None:
    alpha = generate_fog_alpha(width, height)

    # Pure white RGB everywhere, only alpha varies
    rgb = np.full((height, width), 255, dtype=np.uint8)
    rgba = np.dstack([rgb, rgb, rgb, alpha])

    img = Image.fromarray(rgba, mode="RGBA")

    os.makedirs(OUTPUT_FOLDER, exist_ok=True)
    out_path = os.path.join(OUTPUT_FOLDER, f"fog_{index}.png")
    img.save(out_path, format="PNG")
    print(f"Saved {out_path}")


def main():
    for i in range(1, NUM_IMAGES + 1):
        build_fog_texture(WIDTH, HEIGHT, i)


if __name__ == "__main__":
    main()
