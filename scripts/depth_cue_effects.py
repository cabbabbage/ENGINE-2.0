#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import re
import sys
from pathlib import Path
from typing import Dict, Tuple

import numpy as np
from PIL import Image


def _clamp(v: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, v))


def _sat_factor(sat_val: float) -> float:
    return _clamp(1.0 + 2.0 * sat_val, 0.0, 3.0)


def _rgb_to_hsv(r: np.ndarray, g: np.ndarray, b: np.ndarray) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    maxc = np.maximum(np.maximum(r, g), b)
    minc = np.minimum(np.minimum(r, g), b)
    v = maxc
    delta = maxc - minc

    s = np.zeros_like(maxc, dtype=np.float32)
    nz = maxc > 0.0
    s[nz] = delta[nz] / maxc[nz]

    h = np.zeros_like(maxc, dtype=np.float32)
    mask = delta > 1.0e-6
    delta_safe = np.where(mask, delta, 1.0)

    r_max = mask & (maxc == r)
    g_max = mask & (maxc == g)
    b_max = mask & (maxc == b)

    h[r_max] = np.mod((g[r_max] - b[r_max]) / delta_safe[r_max], 6.0)
    h[g_max] = ((b[g_max] - r[g_max]) / delta_safe[g_max]) + 2.0
    h[b_max] = ((r[b_max] - g[b_max]) / delta_safe[b_max]) + 4.0
    h = np.mod(h / 6.0, 1.0)
    return h, s, v


def _hsv_to_rgb(h: np.ndarray, s: np.ndarray, v: np.ndarray) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    h6 = h * 6.0
    c = v * s
    x = c * (1.0 - np.abs(np.mod(h6, 2.0) - 1.0))
    m = v - c

    rr = np.zeros_like(h6, dtype=np.float32)
    gg = np.zeros_like(h6, dtype=np.float32)
    bb = np.zeros_like(h6, dtype=np.float32)

    m0 = (h6 >= 0.0) & (h6 < 1.0)
    m1 = (h6 >= 1.0) & (h6 < 2.0)
    m2 = (h6 >= 2.0) & (h6 < 3.0)
    m3 = (h6 >= 3.0) & (h6 < 4.0)
    m4 = (h6 >= 4.0) & (h6 < 5.0)
    m5 = ~(m0 | m1 | m2 | m3 | m4)

    rr[m0], gg[m0], bb[m0] = c[m0], x[m0], 0.0
    rr[m1], gg[m1], bb[m1] = x[m1], c[m1], 0.0
    rr[m2], gg[m2], bb[m2] = 0.0, c[m2], x[m2]
    rr[m3], gg[m3], bb[m3] = 0.0, x[m3], c[m3]
    rr[m4], gg[m4], bb[m4] = x[m4], 0.0, c[m4]
    rr[m5], gg[m5], bb[m5] = c[m5], 0.0, x[m5]

    return rr + m, gg + m, bb + m


def _gaussian_kernel(sigma: float) -> np.ndarray:
    sigma = max(0.01, float(sigma))
    radius = int(np.ceil(3.0 * sigma))
    xs = np.arange(-radius, radius + 1, dtype=np.float32)
    w = np.exp(-(xs * xs) / (2.0 * sigma * sigma))
    s = float(np.sum(w))
    if s < 1.0e-8:
        s = 1.0
    return w / s


def _gaussian_blur_rgba(src: np.ndarray, sigma: float) -> np.ndarray:
    # src: float32 [0,1], HxWx4
    k = _gaussian_kernel(sigma)
    radius = k.shape[0] // 2
    h, w, _ = src.shape

    padded_x = np.pad(src, ((0, 0), (radius, radius), (0, 0)), mode="edge")
    tmp = np.zeros_like(src, dtype=np.float32)
    for idx, weight in enumerate(k):
        tmp += padded_x[:, idx:idx + w, :] * weight

    padded_y = np.pad(tmp, ((radius, radius), (0, 0), (0, 0)), mode="edge")
    out = np.zeros_like(src, dtype=np.float32)
    for idx, weight in enumerate(k):
        out += padded_y[idx:idx + h, :, :] * weight

    return np.clip(out, 0.0, 1.0)


def _unsharp_mask_rgb_preserve_alpha(src: np.ndarray, radius: float, percent: float, threshold_u8: int) -> np.ndarray:
    blurred = _gaussian_blur_rgba(src, max(0.01, radius))
    scale = max(0.0, percent) / 100.0
    thresh = float(threshold_u8) / 255.0

    out = np.array(src, copy=True)
    d = src[:, :, :3] - blurred[:, :, :3]
    keep = np.abs(d) < thresh
    sharpened = np.clip(src[:, :, :3] + d * scale, 0.0, 1.0)
    out[:, :, :3] = np.where(keep, src[:, :, :3], sharpened)
    out[:, :, 3] = src[:, :, 3]
    return out


def _make_centered_transparent_2x_canvas(src: np.ndarray) -> np.ndarray:
    h, w, _ = src.shape
    out_h = max(1, h * 2)
    out_w = max(1, w * 2)
    out = np.zeros((out_h, out_w, 4), dtype=np.float32)
    off_y = (out_h - h) // 2
    off_x = (out_w - w) // 2
    out[off_y:off_y + h, off_x:off_x + w, :] = src
    return out


def _apply_blur_or_sharpen_like_cpp(src: np.ndarray, blur_val: float, is_foreground: bool) -> np.ndarray:
    v = _clamp(float(blur_val), -1.0, 1.0)
    if abs(v) < 1.0e-3:
        return src

    if v > 0.0:
        max_radius = 20.0
        base_radius = v * max_radius
        if base_radius < 1.0:
            base_radius = 1.0

        if is_foreground:
            radius = base_radius * 2.0
            blurred = _gaussian_blur_rgba(src, radius)
            ring_radius = max(1.0, radius * 0.5)
            return _unsharp_mask_rgb_preserve_alpha(blurred, ring_radius, 80.0, 3)

        radius = base_radius * 1.3
        blurred = _gaussian_blur_rgba(src, radius)

        lum = 0.299 * src[:, :, 0] + 0.587 * src[:, :, 1] + 0.114 * src[:, :, 2]
        L = np.rint(lum * 255.0).astype(np.int32)
        mask = np.where(L < 170, 0, np.minimum(255, (L - 170) * 3)).astype(np.float32) / 255.0

        mask_rgba = np.zeros_like(src, dtype=np.float32)
        mask_rgba[:, :, 0] = mask
        mask_rgba[:, :, 1] = mask
        mask_rgba[:, :, 2] = mask
        mask_rgba[:, :, 3] = 1.0

        mask_sigma = max(1.0, radius * 0.8)
        mask_blur = _gaussian_blur_rgba(mask_rgba, mask_sigma)

        bright = np.array(blurred, copy=True)
        bright[:, :, :3] = np.clip(blurred[:, :, :3] * 1.4, 0.0, 1.0)
        bright[:, :, 3] = blurred[:, :, 3]

        m = np.clip(mask_blur[:, :, 0:1], 0.0, 1.0)
        out = np.array(blurred, copy=True)
        out[:, :, :3] = bright[:, :, :3] * m + blurred[:, :, :3] * (1.0 - m)
        out[:, :, 3:4] = bright[:, :, 3:4] * m + blurred[:, :, 3:4] * (1.0 - m)
        return np.clip(out, 0.0, 1.0)

    strength = -v
    radius = 0.7 + strength * 3.3
    percent = 80.0 + strength * 220.0
    return _unsharp_mask_rgb_preserve_alpha(src, radius, percent, 0)


def _apply_color_effects_like_cpp(src: np.ndarray, payload: Dict[str, object], is_foreground: bool) -> np.ndarray:
    out = np.array(src, copy=True)

    brightness = _clamp(float(payload["brightness"]), -1.0, 1.0)
    contrast = _clamp(float(payload["contrast"]), -1.0, 1.0)
    blur = _clamp(float(payload["blur"]), -1.0, 1.0)

    sat_r = _clamp(float(payload["saturation_red"]), -1.0, 1.0)
    sat_g = _clamp(float(payload["saturation_green"]), -1.0, 1.0)
    sat_b = _clamp(float(payload["saturation_blue"]), -1.0, 1.0)
    hue_deg = _clamp(float(payload["hue"]), -180.0, 180.0)
    hue_offset = hue_deg / 360.0

    alpha_mask = out[:, :, 3] > 0.0
    no_color_changes = (
        abs(brightness) < 1.0e-6
        and abs(contrast) < 1.0e-6
        and abs(sat_r) < 1.0e-6
        and abs(sat_g) < 1.0e-6
        and abs(sat_b) < 1.0e-6
        and abs(hue_deg) < 1.0e-6
    )

    if np.any(alpha_mask) and not no_color_changes:
        rgb = out[:, :, :3]
        r = rgb[:, :, 0]
        g = rgb[:, :, 1]
        b = rgb[:, :, 2]

        mask = alpha_mask
        rr = r[mask]
        gg = g[mask]
        bb = b[mask]

        if abs(brightness) > 1.0e-6:
            rr = np.clip(rr + brightness, 0.0, 1.0)
            gg = np.clip(gg + brightness, 0.0, 1.0)
            bb = np.clip(bb + brightness, 0.0, 1.0)

        if abs(contrast) > 1.0e-6:
            c = 1.0 + contrast
            rr = np.clip((rr - 0.5) * c + 0.5, 0.0, 1.0)
            gg = np.clip((gg - 0.5) * c + 0.5, 0.0, 1.0)
            bb = np.clip((bb - 0.5) * c + 0.5, 0.0, 1.0)

        if abs(hue_deg) > 1.0e-6:
            h, s, v = _rgb_to_hsv(rr, gg, bb)
            h = np.mod(h + hue_offset, 1.0)
            rr, gg, bb = _hsv_to_rgb(h, s, v)

        if abs(sat_r) > 1.0e-6 or abs(sat_g) > 1.0e-6 or abs(sat_b) > 1.0e-6:
            gray = (rr + gg + bb) / 3.0
            if abs(sat_r) > 1.0e-6:
                rr = np.clip(gray + (rr - gray) * _sat_factor(sat_r), 0.0, 1.0)
            if abs(sat_g) > 1.0e-6:
                gg = np.clip(gray + (gg - gray) * _sat_factor(sat_g), 0.0, 1.0)
            if abs(sat_b) > 1.0e-6:
                bb = np.clip(gray + (bb - gray) * _sat_factor(sat_b), 0.0, 1.0)

        r2 = np.array(r, copy=True)
        g2 = np.array(g, copy=True)
        b2 = np.array(b, copy=True)
        r2[mask] = rr
        g2[mask] = gg
        b2[mask] = bb
        out[:, :, 0] = r2
        out[:, :, 1] = g2
        out[:, :, 2] = b2

    out = _apply_blur_or_sharpen_like_cpp(out, blur, is_foreground)
    return np.clip(out, 0.0, 1.0)


def _sanitize_request_id(raw: str) -> str:
    s = (raw or "").strip()
    if not s:
        return "request"
    return re.sub(r"[^A-Za-z0-9_.-]", "_", s)


def _repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def _resolve_output_path(layer: str, save_mode: str, request_id: str) -> Path:
    root = _repo_root()
    if save_mode == "preview":
        return root / "cache" / "preview_images" / f"{layer}.png"
    return root / "cache" / "_effects_tmp" / f"{_sanitize_request_id(request_id)}_{layer}.png"


def _atomic_save_rgba_u8(arr_u8: np.ndarray, out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = out_path.with_suffix(out_path.suffix + ".tmp")
    Image.fromarray(arr_u8, mode="RGBA").save(tmp_path, format="PNG", compress_level=3)
    os.replace(tmp_path, out_path)


def _strip_optional_wrapper_quotes(raw: str) -> str:
    if len(raw) >= 2 and raw[0] == raw[-1] and raw[0] in ("'", '"'):
        return raw[1:-1]
    return raw


def _parse_payload(raw_payload: str) -> Dict[str, object]:
    payload_text = _strip_optional_wrapper_quotes(raw_payload.strip())
    try:
        data = json.loads(payload_text)
    except json.JSONDecodeError:
        # Some launchers preserve backslash-escaped quotes from shell-safe payload strings.
        normalized = payload_text.replace('\\"', '"').replace("\\\\", "\\")
        data = json.loads(normalized)
    if not isinstance(data, dict):
        raise ValueError("effects_value must decode to a JSON object")

    required = [
        "layer",
        "contrast",
        "brightness",
        "blur",
        "saturation_red",
        "saturation_green",
        "saturation_blue",
        "hue",
        "save_mode",
        "request_id",
    ]
    for key in required:
        if key not in data:
            raise ValueError(f"effects_value missing required key: {key}")

    layer = str(data["layer"]).strip().lower()
    if layer not in ("foreground", "background"):
        raise ValueError("layer must be 'foreground' or 'background'")
    data["layer"] = layer

    save_mode = str(data["save_mode"]).strip().lower()
    if save_mode not in ("temp", "preview"):
        raise ValueError("save_mode must be 'temp' or 'preview'")
    data["save_mode"] = save_mode

    data["request_id"] = str(data["request_id"])

    for k in [
        "contrast",
        "brightness",
        "blur",
        "saturation_red",
        "saturation_green",
        "saturation_blue",
        "hue",
    ]:
        data[k] = float(data[k])

    return data


def _load_input_rgba(path: Path) -> np.ndarray:
    if not path.exists():
        raise FileNotFoundError(f"input image does not exist: {path}")
    with Image.open(path) as img:
        rgba = img.convert("RGBA")
        arr_u8 = np.asarray(rgba, dtype=np.uint8)
    return arr_u8.astype(np.float32) / 255.0


def main() -> int:
    if len(sys.argv) != 3:
        print("Usage: depth_cue_effects.py <input_image_path> <effects_value_json>", file=sys.stderr)
        return 2

    input_path = Path(sys.argv[1])
    payload = _parse_payload(sys.argv[2])

    src = _load_input_rgba(input_path)
    expanded = _make_centered_transparent_2x_canvas(src)
    out = _apply_color_effects_like_cpp(expanded, payload, payload["layer"] == "foreground")
    out_u8 = np.rint(np.clip(out, 0.0, 1.0) * 255.0).astype(np.uint8)

    output_path = _resolve_output_path(str(payload["layer"]), str(payload["save_mode"]), str(payload["request_id"]))
    _atomic_save_rgba_u8(out_u8, output_path)
    print(str(output_path))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"depth_cue_effects.py failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
