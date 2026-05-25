#!/usr/bin/env python3

import os
import numpy as np
from PIL import Image, ImageFilter

# -----------------------------
# Config
# -----------------------------
NUM_IMAGES = 4

# Portrait canvas
WIDTH = 720 
HEIGHT_MIN_MULT = 3.0
HEIGHT_MAX_MULT = 3.0

# Final opacity multiplier applied as the LAST step (0.0 to 1.0)
FINAL_OPACITY_MULT = 1.0

# Keep portrait canvas size instead of cropping to visible fog bounds
CROP_TO_ALPHA_BOUNDS = False


def smoothstep(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0 + 1e-8), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def noise_layer(h, w, grid, blur_radius, rng):
    gh = max(2, int(np.ceil(h / grid)))
    gw = max(2, int(np.ceil(w / grid)))
    small = rng.random((gh, gw), dtype=np.float32)

    im = Image.fromarray((small * 255.0).astype(np.uint8), mode="L")
    im = im.resize((w, h), resample=Image.BILINEAR)
    if blur_radius > 0.0:
        im = im.filter(ImageFilter.GaussianBlur(radius=blur_radius))

    return np.asarray(im, dtype=np.float32) / 255.0


def bilinear_sample(img, x, y):
    h, w = img.shape
    x = np.clip(x, 0.0, w - 1.001)
    y = np.clip(y, 0.0, h - 1.001)

    x0 = np.floor(x).astype(np.int32)
    y0 = np.floor(y).astype(np.int32)
    x1 = np.clip(x0 + 1, 0, w - 1)
    y1 = np.clip(y0 + 1, 0, h - 1)

    fx = x - x0
    fy = y - y0

    a = img[y0, x0]
    b = img[y0, x1]
    c = img[y1, x0]
    d = img[y1, x1]

    ab = a + (b - a) * fx
    cd = c + (d - c) * fx
    return ab + (cd - ab) * fy


def apply_final_opacity(img_rgba: Image.Image, mult: float) -> Image.Image:
    mult = float(np.clip(mult, 0.0, 1.0))
    if mult >= 0.999999:
        return img_rgba

    a = np.array(img_rgba.getchannel("A"), dtype=np.float32)
    a = np.clip(a * mult, 0.0, 255.0).astype(np.uint8)

    out = img_rgba.copy()
    out.putalpha(Image.fromarray(a, mode="L"))
    return out


def make_fog_image(width, height, seed):
    rng = np.random.default_rng(seed)

    big = noise_layer(height, width, grid=int(max(height, width) / 6), blur_radius=9.0, rng=rng)
    med = noise_layer(height, width, grid=int(max(height, width) / 14), blur_radius=4.5, rng=rng)
    fine = noise_layer(height, width, grid=int(max(height, width) / 38), blur_radius=1.6, rng=rng)

    fog = (0.55 * big + 0.30 * med + 0.15 * fine).astype(np.float32)
    fog = np.clip(fog, 0.0, 1.0)

    warp_a = noise_layer(height, width, grid=int(max(height, width) / 10), blur_radius=5.5, rng=rng)
    warp_b = noise_layer(height, width, grid=int(max(height, width) / 10), blur_radius=5.5, rng=rng)
    warp_a = (warp_a - 0.5) * 2.0
    warp_b = (warp_b - 0.5) * 2.0

    y, x = np.mgrid[0:height, 0:width].astype(np.float32)
    warp_px = 10.0 + rng.random() * 10.0
    xs = x + warp_a * warp_px
    ys = y + warp_b * warp_px
    fog = bilinear_sample(fog, xs, ys).astype(np.float32)
    fog = np.clip(fog, 0.0, 1.0)

    # Bottom-heavy vertical fade
    y01 = (np.arange(height, dtype=np.float32) / max(1, height - 1)).reshape(height, 1)
    vertical_mask = smoothstep(0.03, 1.0, y01)
    vertical_mask = vertical_mask ** 1.85
    fog2 = fog * vertical_mask

    # Portrait-shaped fog body
    cx = (width - 1) * 0.5
    cy = (height - 1) * 0.68

    x_limit = min(cx - 2.0, (width - 1) - cx - 2.0)
    y_limit = min(cy - 2.0, (height - 1) - cy - 2.0)
    x_limit = max(10.0, x_limit)
    y_limit = max(10.0, y_limit)

    bottom_stretch = 0.55

    rx_base = min(x_limit / (1.0 + bottom_stretch), width * (0.42 + 0.06 * rng.random()))
    ry = min(y_limit, height * (0.42 + 0.08 * rng.random()))

    rx_base = max(20.0, rx_base * 0.98)
    ry = max(20.0, ry * 0.98)

    dx = x - cx
    dy = y - cy
    y01_full = y / max(1.0, float(height - 1))
    widen = 1.0 + bottom_stretch * y01_full

    dnoise = noise_layer(height, width, grid=int(max(height, width) / 7), blur_radius=10.0, rng=rng)
    dnoise = (dnoise - 0.5) * 2.0
    wobble = 0.06 * dnoise

    nx = dx / (rx_base * widen + 1e-8)
    ny = dy / (ry + 1e-8)
    r_ell = np.sqrt(nx * nx + ny * ny)

    dist_norm = (1.0 - r_ell) + wobble
    aa = 0.010
    shape_mask = smoothstep(-aa, aa, dist_norm)
    fog3 = fog2 * shape_mask

    fade_to_center = 0.95
    edge_fade = smoothstep(0.0, fade_to_center, np.clip(dist_norm, 0.0, 1.0))
    edge_fade = edge_fade ** 1.15

    final_alpha = np.clip(fog3 * edge_fade, 0.0, 1.0)

    rgb = np.full((height, width, 3), 255, dtype=np.uint8)
    a = (final_alpha * 255.0).astype(np.uint8)
    rgba = np.dstack([rgb, a])

    if CROP_TO_ALPHA_BOUNDS:
        alpha = rgba[:, :, 3]
        rows_have_alpha = (alpha.max(axis=1) > 0)
        cols_have_alpha = (alpha.max(axis=0) > 0)

        if np.any(rows_have_alpha) and np.any(cols_have_alpha):
            top = int(np.where(rows_have_alpha)[0].min())
            bottom = int(np.where(rows_have_alpha)[0].max())
            left = int(np.where(cols_have_alpha)[0].min())
            right = int(np.where(cols_have_alpha)[0].max())
            rgba = rgba[top:bottom + 1, left:right + 1, :]

    return Image.fromarray(rgba, mode="RGBA")


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    master_rng = np.random.default_rng()

    for i in range(1, NUM_IMAGES + 1):
        width = WIDTH
        height = int(round(WIDTH * master_rng.uniform(HEIGHT_MIN_MULT, HEIGHT_MAX_MULT)))
        seed = int(master_rng.integers(0, 2**31 - 1))

        img = make_fog_image(width, height, seed=seed)
        img = img.convert("RGBA")
        img = apply_final_opacity(img, FINAL_OPACITY_MULT)

        out_path = os.path.join(script_dir, f"fog_{i}.png")
        img.save(out_path)


if __name__ == "__main__":
    main()