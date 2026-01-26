#!/usr/bin/env python3

import os
import numpy as np
from PIL import Image, ImageFilter

# -----------------------------
# Config
# -----------------------------
NUM_IMAGES = 10
HEIGHT = 720 * 3
WIDTH_MIN_MULT = 1.0
WIDTH_MAX_MULT = 2.0

# When true, generate solid random-color test rectangles instead of fog
CREATE_TEST_IMAGES = False

# Final opacity multiplier applied as the LAST step (0.0 to 1.0)
# Example: 0.75 = reduce final opacity to 75%
FINAL_OPACITY_MULT = 0.1


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


def make_test_image(width, height, seed):
    rng = np.random.default_rng(seed)

    # Solid random RGB, full alpha
    color = rng.integers(0, 256, size=(3,), dtype=np.uint8)
    rgb = np.full((height, width, 3), color, dtype=np.uint8)
    a = np.full((height, width, 1), 255, dtype=np.uint8)

    rgba = np.concatenate([rgb, a], axis=2)
    return Image.fromarray(rgba, mode="RGBA")


def make_fog_image(width, height, seed):
    rng = np.random.default_rng(seed)

    # Step 1: Create the raw rectangular fog field (no fades, no geometry)
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

    # Step 2: Apply a vertical opacity gradient (bottom full, fades upward)
    y01 = (np.arange(height, dtype=np.float32) / max(1, height - 1)).reshape(height, 1)  # 0 top, 1 bottom
    vertical_mask = smoothstep(0.03, 1.0, y01)
    vertical_mask = vertical_mask ** 1.85
    fog2 = fog * vertical_mask

    # Step 3: Apply the random curved shape mask (sideways oval, wider at bottom, slightly distorted, stays in bounds)
    cx = (width - 1) * 0.5
    cy = (height - 1) * 0.62

    x_limit = min(cx - 2.0, (width - 1) - cx - 2.0)
    y_limit = min(cy - 2.0, (height - 1) - cy - 2.0)
    x_limit = max(10.0, x_limit)
    y_limit = max(10.0, y_limit)

    bottom_stretch = 0.70
    max_widen = 1.0 + bottom_stretch * 1.0

    rx_base = min(x_limit / max_widen, min(width, height) * (0.58 + 0.06 * rng.random()))
    ry = min(y_limit, min(width, height) * (0.33 + 0.05 * rng.random()))

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

    # Step 4: Apply a gradient fade around the silhouette edge (soft perimeter, long fade inward)
    fade_to_center = 0.95
    edge_fade = smoothstep(0.0, fade_to_center, np.clip(dist_norm, 0.0, 1.0))
    edge_fade = edge_fade ** 1.15

    final_alpha = np.clip(fog3 * edge_fade, 0.0, 1.0)

    rgb = np.full((height, width, 3), 255, dtype=np.uint8)
    a = (final_alpha * 255.0).astype(np.uint8)
    rgba = np.dstack([rgb, a])

    # Final: crop away all fully transparent rows/cols on every side (tight bbox around alpha>0)
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
        w = int(round(HEIGHT * master_rng.uniform(WIDTH_MIN_MULT, WIDTH_MAX_MULT)))
        seed = int(master_rng.integers(0, 2**31 - 1))

        if CREATE_TEST_IMAGES:
            img = make_test_image(w, HEIGHT, seed=seed)
        else:
            img = make_fog_image(w, HEIGHT, seed=seed)

        # Darken the image by 25% BEFORE applying final opacity
        img = img.convert("RGBA")
        arr = np.array(img, dtype=np.float32)
        arr[..., :3] *= 0.7  # darken RGB by 25%
        arr[..., :3] = np.clip(arr[..., :3], 0, 255)
        img = Image.fromarray(arr.astype(np.uint8), mode="RGBA")

        # LAST STEP: reduce opacity for the final image (applies to both test + fog)
        img = apply_final_opacity(img, FINAL_OPACITY_MULT)

        out_path = os.path.join(script_dir, f"fog_{i}.png")
        img.save(out_path)


if __name__ == "__main__":
    main()
