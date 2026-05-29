#!/usr/bin/env python3
"""
Tileable atmospheric dust / glitter texture generator.

Place this file at:
    resources/dust.py

Generated output goes to:
    resources/dust/

Dependencies:
    pip install pillow
"""

from __future__ import annotations

import math
import random
import time
import tkinter as tk
from dataclasses import dataclass
from pathlib import Path
from tkinter import messagebox, ttk
from typing import Tuple

from PIL import Image, ImageDraw, ImageFilter, ImageTk


Color = Tuple[int, int, int]


@dataclass
class DustSettings:
    tile_size: int = 512
    seed: int = 1337

    dust_count: int = 420
    glitter_count: int = 36

    min_radius: float = 0.45
    max_radius: float = 2.20
    glitter_min_radius: float = 0.85
    glitter_max_radius: float = 3.20

    dust_opacity: float = 0.105
    glitter_opacity: float = 0.180

    blur_radius: float = 0.70
    glow_radius: float = 2.40

    dust_brightness: float = 0.78
    glitter_brightness: float = 1.00

    blue_tint: float = 0.16
    warm_tint: float = 0.03

    top_density: float = 0.70
    bottom_density: float = 1.00
    center_fade: float = 0.18

    flow_angle_deg: float = -18.0
    trail_length: float = 6.0
    trail_opacity: float = 0.055
    trail_steps: int = 5
    drift_pixels_per_loop: float = 72.0

    shimmer_variation: float = 0.35
    alpha_noise: float = 0.18

    preview_scale: float = 0.75
    preview_tile_repeat: int = 2

    frame_count: int = 48
    animation_count: int = 1
    output_name: str = "dust"


def clamp(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))


def smoothstep(value: float) -> float:
    t = clamp(value, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def parse_int(var: tk.StringVar | None, fallback: int, lo: int, hi: int) -> int:
    if var is None:
        return fallback
    try:
        parsed = int(float(var.get()))
    except Exception:
        parsed = fallback
    return max(lo, min(hi, parsed))


def parse_float(var: tk.StringVar | None, fallback: float, lo: float, hi: float) -> float:
    if var is None:
        return fallback
    try:
        parsed = float(var.get())
    except Exception:
        parsed = fallback
    return max(lo, min(hi, parsed))


def parse_string(var: tk.StringVar | None, fallback: str) -> str:
    if var is None:
        return fallback
    value = var.get().strip()
    return value if value else fallback


def color_from_tint(base: float, blue_tint: float, warm_tint: float) -> Color:
    base_i = int(clamp(base, 0.0, 1.0) * 255.0)

    r = base_i * (1.0 + warm_tint * 0.45)
    g = base_i * (1.0 + warm_tint * 0.10)
    b = base_i * (1.0 + blue_tint * 0.55)

    return (
        int(clamp(r, 0.0, 255.0)),
        int(clamp(g, 0.0, 255.0)),
        int(clamp(b, 0.0, 255.0)),
    )


def wrap_positions(x: float, y: float, size: int, radius: float) -> list[tuple[float, float]]:
    positions = [(x, y)]

    near_left = x - radius < 0.0
    near_right = x + radius >= size
    near_top = y - radius < 0.0
    near_bottom = y + radius >= size

    if near_left:
        positions.append((x + size, y))
    if near_right:
        positions.append((x - size, y))
    if near_top:
        positions.append((x, y + size))
    if near_bottom:
        positions.append((x, y - size))

    if near_left and near_top:
        positions.append((x + size, y + size))
    if near_left and near_bottom:
        positions.append((x + size, y - size))
    if near_right and near_top:
        positions.append((x - size, y + size))
    if near_right and near_bottom:
        positions.append((x - size, y - size))

    return positions


def draw_soft_particle(
    image: Image.Image,
    x: float,
    y: float,
    radius: float,
    color: Color,
    alpha: float,
    size: int,
) -> None:
    if radius <= 0.0 or alpha <= 0.0:
        return

    draw = ImageDraw.Draw(image, "RGBA")
    alpha_i = int(clamp(alpha, 0.0, 1.0) * 255.0)
    if alpha_i <= 0:
        return

    wrap_radius = radius + 8.0
    for px, py in wrap_positions(x, y, size, wrap_radius):
        draw.ellipse(
            (
                px - radius,
                py - radius,
                px + radius,
                py + radius,
            ),
            fill=(color[0], color[1], color[2], alpha_i),
        )


def draw_trail(
    image: Image.Image,
    x: float,
    y: float,
    radius: float,
    color: Color,
    alpha: float,
    size: int,
    flow_angle_deg: float,
    trail_length: float,
    trail_steps: int,
) -> None:
    if trail_steps <= 0 or trail_length <= 0.0 or alpha <= 0.0:
        return

    angle = math.radians(flow_angle_deg)
    dx = math.cos(angle)
    dy = math.sin(angle)

    for step in range(1, trail_steps + 1):
        t = step / float(trail_steps)
        px = (x - dx * trail_length * t) % size
        py = (y - dy * trail_length * t) % size
        local_alpha = alpha * (1.0 - t) * 0.70
        local_radius = max(0.20, radius * (1.0 - t * 0.35))
        draw_soft_particle(image, px, py, local_radius, color, local_alpha, size)


def vertical_density_weight(y: float, size: int, top_density: float, bottom_density: float) -> float:
    t = clamp(y / max(1.0, float(size)), 0.0, 1.0)
    return top_density + (bottom_density - top_density) * smoothstep(t)


def center_fade_weight(x: float, y: float, size: int, center_fade: float) -> float:
    if center_fade <= 0.0:
        return 1.0

    cx = size * 0.5
    cy = size * 0.5
    dx = (x - cx) / max(1.0, cx)
    dy = (y - cy) / max(1.0, cy)
    d = clamp(math.sqrt(dx * dx + dy * dy), 0.0, 1.0)

    return 1.0 - center_fade * (1.0 - smoothstep(d))


def particle_position_with_drift(
    x: float,
    y: float,
    size: int,
    phase01: float,
    flow_angle_deg: float,
    drift_pixels_per_loop: float,
    individual_speed: float,
) -> tuple[float, float]:
    angle = math.radians(flow_angle_deg)
    drift = drift_pixels_per_loop * phase01 * individual_speed

    px = (x + math.cos(angle) * drift) % size
    py = (y + math.sin(angle) * drift) % size

    return px, py


def make_tile(settings: DustSettings, phase01: float = 0.0) -> Image.Image:
    size = int(settings.tile_size)
    phase01 = phase01 % 1.0
    phase_radians = phase01 * math.tau

    rng = random.Random(settings.seed)

    glow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    sharp = Image.new("RGBA", (size, size), (0, 0, 0, 0))

    dust_color = color_from_tint(settings.dust_brightness, settings.blue_tint, settings.warm_tint)
    glitter_color = color_from_tint(settings.glitter_brightness, settings.blue_tint * 1.65, settings.warm_tint)

    min_radius = min(settings.min_radius, settings.max_radius)
    max_radius = max(settings.min_radius, settings.max_radius)

    glitter_min_radius = min(settings.glitter_min_radius, settings.glitter_max_radius)
    glitter_max_radius = max(settings.glitter_min_radius, settings.glitter_max_radius)

    for _ in range(settings.dust_count):
        base_x = rng.random() * size
        base_y = rng.random() * size
        individual_speed = rng.uniform(0.65, 1.35)

        x, y = particle_position_with_drift(
            base_x,
            base_y,
            size,
            phase01,
            settings.flow_angle_deg,
            settings.drift_pixels_per_loop,
            individual_speed,
        )

        radius = rng.uniform(min_radius, max_radius)

        density = vertical_density_weight(y, size, settings.top_density, settings.bottom_density)
        center = center_fade_weight(x, y, size, settings.center_fade)
        noise = 1.0 + rng.uniform(-settings.alpha_noise, settings.alpha_noise)

        shimmer_phase = rng.random() * math.tau
        shimmer = 1.0 + math.sin(phase_radians + shimmer_phase) * settings.shimmer_variation * 0.18

        alpha = settings.dust_opacity * density * center * noise * shimmer
        alpha = clamp(alpha, 0.0, 1.0)

        draw_trail(
            glow,
            x,
            y,
            radius * 0.85,
            dust_color,
            settings.trail_opacity * alpha,
            size,
            settings.flow_angle_deg,
            settings.trail_length,
            settings.trail_steps,
        )
        draw_soft_particle(glow, x, y, radius * 1.20, dust_color, alpha * 0.42, size)
        draw_soft_particle(sharp, x, y, radius * 0.58, dust_color, alpha * 0.70, size)

    for _ in range(settings.glitter_count):
        base_x = rng.random() * size
        base_y = rng.random() * size
        individual_speed = rng.uniform(0.45, 1.15)

        x, y = particle_position_with_drift(
            base_x,
            base_y,
            size,
            phase01,
            settings.flow_angle_deg,
            settings.drift_pixels_per_loop * 0.75,
            individual_speed,
        )

        radius = rng.uniform(glitter_min_radius, glitter_max_radius)

        density = vertical_density_weight(y, size, settings.top_density, settings.bottom_density)
        center = center_fade_weight(x, y, size, settings.center_fade)

        shimmer_phase = rng.random() * math.tau
        shimmer = 1.0 + math.sin(phase_radians * 1.75 + shimmer_phase) * settings.shimmer_variation
        noise = 1.0 + rng.uniform(-settings.alpha_noise, settings.alpha_noise)

        alpha = settings.glitter_opacity * density * center * shimmer * noise
        alpha = clamp(alpha, 0.0, 1.0)

        draw_trail(
            glow,
            x,
            y,
            radius * 0.65,
            glitter_color,
            settings.trail_opacity * alpha * 0.85,
            size,
            settings.flow_angle_deg,
            settings.trail_length * 1.25,
            settings.trail_steps,
        )
        draw_soft_particle(glow, x, y, radius * 1.55, glitter_color, alpha * 0.52, size)
        draw_soft_particle(sharp, x, y, radius * 0.46, glitter_color, alpha, size)

    if settings.glow_radius > 0.0:
        glow = glow.filter(ImageFilter.GaussianBlur(settings.glow_radius))

    if settings.blur_radius > 0.0:
        sharp = sharp.filter(ImageFilter.GaussianBlur(settings.blur_radius))

    result = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    result.alpha_composite(glow)
    result.alpha_composite(sharp)
    return result


def make_preview_image(tile: Image.Image, repeat: int, preview_scale: float) -> Image.Image:
    repeat = max(1, min(4, repeat))
    width, height = tile.size

    preview = Image.new("RGBA", (width * repeat, height * repeat), (10, 13, 18, 255))
    draw = ImageDraw.Draw(preview, "RGBA")

    cell = max(16, width // 8)
    for y in range(0, preview.height, cell):
        for x in range(0, preview.width, cell):
            if ((x // cell) + (y // cell)) % 2 == 0:
                draw.rectangle((x, y, x + cell, y + cell), fill=(16, 20, 28, 255))

    for ty in range(repeat):
        for tx in range(repeat):
            preview.alpha_composite(tile, (tx * width, ty * height))

    if abs(preview_scale - 1.0) > 0.001:
        new_width = max(1, int(preview.width * preview_scale))
        new_height = max(1, int(preview.height * preview_scale))
        preview = preview.resize((new_width, new_height), Image.Resampling.LANCZOS)

    return preview


class DustGeneratorUI:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("Atmospheric Dust Tile Generator")
        self.root.geometry("1240x880")

        self.script_dir = Path(__file__).resolve().parent
        self.output_dir = self.script_dir / "dust"
        self.output_dir.mkdir(parents=True, exist_ok=True)

        self.vars: dict[str, tk.StringVar] = {}
        self.preview_photo: ImageTk.PhotoImage | None = None
        self.current_tile: Image.Image | None = None

        self.suppress_preview = True
        self.preview_pending = False
        self.animation_running = False
        self.animation_phase = 0.0
        self.status_var = tk.StringVar(value="Building UI...")

        self._build_ui()

        self.suppress_preview = False
        self.apply_preset("cinematic")
        self.schedule_preview(immediate=True)

    def _var(self, key: str, value: str) -> tk.StringVar:
        var = tk.StringVar(value=value)
        var.trace_add("write", lambda *_: self.schedule_preview())
        self.vars[key] = var
        return var

    def _build_ui(self) -> None:
        root_frame = ttk.Frame(self.root, padding=10)
        root_frame.pack(fill=tk.BOTH, expand=True)

        left = ttk.Frame(root_frame)
        left.pack(side=tk.LEFT, fill=tk.Y, padx=(0, 10))

        right = ttk.Frame(root_frame)
        right.pack(side=tk.RIGHT, fill=tk.BOTH, expand=True)

        ttk.Label(left, text="Dust Tile Settings", font=("Segoe UI", 14, "bold")).pack(anchor=tk.W, pady=(0, 8))

        preset_frame = ttk.LabelFrame(left, text="Presets", padding=8)
        preset_frame.pack(fill=tk.X, pady=(0, 8))

        ttk.Button(preset_frame, text="Cinematic", command=lambda: self.apply_preset("cinematic")).pack(side=tk.LEFT, padx=2)
        ttk.Button(preset_frame, text="Subtle", command=lambda: self.apply_preset("subtle")).pack(side=tk.LEFT, padx=2)
        ttk.Button(preset_frame, text="Glittery", command=lambda: self.apply_preset("glittery")).pack(side=tk.LEFT, padx=2)
        ttk.Button(preset_frame, text="Dense Far Dust", command=lambda: self.apply_preset("dense")).pack(side=tk.LEFT, padx=2)

        controls_outer = ttk.Frame(left)
        controls_outer.pack(fill=tk.BOTH, expand=True)

        controls_canvas = tk.Canvas(controls_outer, width=420, highlightthickness=0)
        controls_scroll = ttk.Scrollbar(controls_outer, orient=tk.VERTICAL, command=controls_canvas.yview)
        controls_frame = ttk.Frame(controls_canvas)

        controls_frame.bind(
            "<Configure>",
            lambda _event: controls_canvas.configure(scrollregion=controls_canvas.bbox("all")),
        )

        controls_canvas.create_window((0, 0), window=controls_frame, anchor="nw")
        controls_canvas.configure(yscrollcommand=controls_scroll.set)

        controls_canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        controls_scroll.pack(side=tk.RIGHT, fill=tk.Y)

        def wheel(event: tk.Event) -> None:
            controls_canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")

        controls_canvas.bind("<Enter>", lambda _event: controls_canvas.bind_all("<MouseWheel>", wheel))
        controls_canvas.bind("<Leave>", lambda _event: controls_canvas.unbind_all("<MouseWheel>"))

        self._add_entry(controls_frame, "Tile Size", "tile_size", "512", "128 to 2048")
        self._add_entry(controls_frame, "Seed", "seed", "1337", "0 to 9999999")
        self._add_entry(controls_frame, "Dust Count", "dust_count", "420", "0 to 5000")
        self._add_entry(controls_frame, "Glitter Count", "glitter_count", "36", "0 to 1000")

        self._add_slider(controls_frame, "Dust Min Radius", "min_radius", "0.45", 0.05, 8.0)
        self._add_slider(controls_frame, "Dust Max Radius", "max_radius", "2.20", 0.05, 12.0)
        self._add_slider(controls_frame, "Glitter Min Radius", "glitter_min_radius", "0.85", 0.05, 10.0)
        self._add_slider(controls_frame, "Glitter Max Radius", "glitter_max_radius", "3.20", 0.05, 16.0)

        self._add_slider(controls_frame, "Dust Opacity", "dust_opacity", "0.105", 0.0, 1.0)
        self._add_slider(controls_frame, "Glitter Opacity", "glitter_opacity", "0.180", 0.0, 1.0)
        self._add_slider(controls_frame, "Blur Radius", "blur_radius", "0.70", 0.0, 8.0)
        self._add_slider(controls_frame, "Glow Radius", "glow_radius", "2.40", 0.0, 16.0)

        self._add_slider(controls_frame, "Dust Brightness", "dust_brightness", "0.78", 0.0, 1.0)
        self._add_slider(controls_frame, "Glitter Brightness", "glitter_brightness", "1.00", 0.0, 1.0)
        self._add_slider(controls_frame, "Blue Tint", "blue_tint", "0.16", 0.0, 1.0)
        self._add_slider(controls_frame, "Warm Tint", "warm_tint", "0.03", 0.0, 1.0)

        self._add_slider(controls_frame, "Top Density", "top_density", "0.70", 0.0, 2.0)
        self._add_slider(controls_frame, "Bottom Density", "bottom_density", "1.00", 0.0, 2.0)
        self._add_slider(controls_frame, "Center Fade", "center_fade", "0.18", 0.0, 1.0)

        self._add_slider(controls_frame, "Flow Angle", "flow_angle_deg", "-18.0", -180.0, 180.0)
        self._add_slider(controls_frame, "Trail Length", "trail_length", "6.0", 0.0, 64.0)
        self._add_slider(controls_frame, "Trail Opacity", "trail_opacity", "0.055", 0.0, 1.0)
        self._add_entry(controls_frame, "Trail Steps", "trail_steps", "5", "0 to 64")
        self._add_slider(controls_frame, "Drift Pixels Per Loop", "drift_pixels_per_loop", "72.0", 0.0, 512.0)

        self._add_slider(controls_frame, "Shimmer Variation", "shimmer_variation", "0.35", 0.0, 1.0)
        self._add_slider(controls_frame, "Alpha Noise", "alpha_noise", "0.18", 0.0, 1.0)

        self._add_slider(controls_frame, "Preview Scale", "preview_scale", "0.75", 0.10, 2.0)
        self._add_entry(controls_frame, "Preview Tile Repeat", "preview_tile_repeat", "2", "1 to 4")

        self._add_entry(controls_frame, "Frame Count", "frame_count", "48", "1 to 600")
        self._add_entry(controls_frame, "Animation Count", "animation_count", "1", "1 to 64")
        self._add_entry(controls_frame, "Output Name", "output_name", "dust", "folder / file prefix")

        action_frame = ttk.Frame(right)
        action_frame.pack(fill=tk.X, pady=(0, 8))

        ttk.Button(action_frame, text="Regenerate Preview", command=lambda: self.schedule_preview(immediate=True)).pack(side=tk.LEFT, padx=3)
        ttk.Button(action_frame, text="Random Seed", command=self.random_seed).pack(side=tk.LEFT, padx=3)
        ttk.Button(action_frame, text="Animate Preview", command=self.toggle_animation).pack(side=tk.LEFT, padx=3)
        ttk.Button(action_frame, text="Save Tile", command=self.save_tile).pack(side=tk.LEFT, padx=3)
        ttk.Button(action_frame, text="Save 4 Tiles", command=lambda: self.save_tile_variants(4)).pack(side=tk.LEFT, padx=3)
        ttk.Button(action_frame, text="Save Animation Frames", command=self.save_animation_frames).pack(side=tk.LEFT, padx=3)
        ttk.Button(action_frame, text="Save Many Animations", command=self.save_many_animations).pack(side=tk.LEFT, padx=3)

        ttk.Label(right, textvariable=self.status_var).pack(anchor=tk.W, pady=(0, 4))

        preview_frame = ttk.LabelFrame(right, text="Preview", padding=8)
        preview_frame.pack(fill=tk.BOTH, expand=True)

        self.preview_label = ttk.Label(preview_frame)
        self.preview_label.pack(fill=tk.BOTH, expand=True)

    def _add_entry(self, parent: ttk.Frame, label: str, key: str, default: str, hint: str) -> None:
        frame = ttk.Frame(parent)
        frame.pack(fill=tk.X, pady=4)

        ttk.Label(frame, text=label, width=24).pack(side=tk.LEFT)

        entry = ttk.Entry(frame, textvariable=self._var(key, default), width=12)
        entry.pack(side=tk.LEFT, padx=(5, 4))

        ttk.Label(frame, text=hint).pack(side=tk.LEFT)

    def _add_slider(self, parent: ttk.Frame, label: str, key: str, default: str, lo: float, hi: float) -> None:
        frame = ttk.Frame(parent)
        frame.pack(fill=tk.X, pady=5)

        ttk.Label(frame, text=label, width=24).pack(side=tk.LEFT)

        var = self._var(key, default)

        scale = ttk.Scale(
            frame,
            from_=lo,
            to=hi,
            orient=tk.HORIZONTAL,
            command=lambda value, v=var: v.set(f"{float(value):.4f}"),
        )
        scale.set(float(default))
        scale.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(5, 4))

        ttk.Entry(frame, textvariable=var, width=8).pack(side=tk.LEFT)

    def apply_preset(self, name: str) -> None:
        presets: dict[str, dict[str, str]] = {
            "cinematic": {
                "tile_size": "512",
                "dust_count": "420",
                "glitter_count": "36",
                "min_radius": "0.45",
                "max_radius": "2.20",
                "glitter_min_radius": "0.85",
                "glitter_max_radius": "3.20",
                "dust_opacity": "0.105",
                "glitter_opacity": "0.180",
                "blur_radius": "0.70",
                "glow_radius": "2.40",
                "dust_brightness": "0.78",
                "glitter_brightness": "1.00",
                "blue_tint": "0.16",
                "warm_tint": "0.03",
                "top_density": "0.70",
                "bottom_density": "1.00",
                "center_fade": "0.18",
                "flow_angle_deg": "-18.0",
                "trail_length": "6.0",
                "trail_opacity": "0.055",
                "trail_steps": "5",
                "drift_pixels_per_loop": "72.0",
                "shimmer_variation": "0.35",
                "alpha_noise": "0.18",
                "frame_count": "48",
                "animation_count": "1",
                "output_name": "dust",
            },
            "subtle": {
                "dust_count": "280",
                "glitter_count": "18",
                "dust_opacity": "0.055",
                "glitter_opacity": "0.105",
                "glow_radius": "1.75",
                "trail_length": "3.5",
                "trail_opacity": "0.025",
                "drift_pixels_per_loop": "48.0",
                "shimmer_variation": "0.20",
            },
            "glittery": {
                "dust_count": "360",
                "glitter_count": "92",
                "dust_opacity": "0.080",
                "glitter_opacity": "0.290",
                "glitter_min_radius": "0.60",
                "glitter_max_radius": "4.30",
                "glow_radius": "3.20",
                "blue_tint": "0.28",
                "trail_length": "8.5",
                "trail_opacity": "0.080",
                "drift_pixels_per_loop": "88.0",
                "shimmer_variation": "0.62",
            },
            "dense": {
                "dust_count": "980",
                "glitter_count": "48",
                "min_radius": "0.25",
                "max_radius": "1.25",
                "dust_opacity": "0.062",
                "glitter_opacity": "0.135",
                "blur_radius": "0.50",
                "glow_radius": "1.80",
                "top_density": "0.95",
                "bottom_density": "1.20",
                "trail_length": "4.0",
                "trail_opacity": "0.035",
                "drift_pixels_per_loop": "56.0",
            },
        }

        was_suppressed = self.suppress_preview
        self.suppress_preview = True

        for key, value in presets.get(name, {}).items():
            if key in self.vars:
                self.vars[key].set(value)

        self.suppress_preview = was_suppressed
        self.schedule_preview(immediate=True)

    def read_settings(self) -> DustSettings:
        return DustSettings(
            tile_size=parse_int(self.vars.get("tile_size"), 512, 128, 2048),
            seed=parse_int(self.vars.get("seed"), 1337, 0, 9999999),
            dust_count=parse_int(self.vars.get("dust_count"), 420, 0, 5000),
            glitter_count=parse_int(self.vars.get("glitter_count"), 36, 0, 1000),
            min_radius=parse_float(self.vars.get("min_radius"), 0.45, 0.01, 64.0),
            max_radius=parse_float(self.vars.get("max_radius"), 2.20, 0.01, 64.0),
            glitter_min_radius=parse_float(self.vars.get("glitter_min_radius"), 0.85, 0.01, 64.0),
            glitter_max_radius=parse_float(self.vars.get("glitter_max_radius"), 3.20, 0.01, 64.0),
            dust_opacity=parse_float(self.vars.get("dust_opacity"), 0.105, 0.0, 1.0),
            glitter_opacity=parse_float(self.vars.get("glitter_opacity"), 0.180, 0.0, 1.0),
            blur_radius=parse_float(self.vars.get("blur_radius"), 0.70, 0.0, 64.0),
            glow_radius=parse_float(self.vars.get("glow_radius"), 2.40, 0.0, 64.0),
            dust_brightness=parse_float(self.vars.get("dust_brightness"), 0.78, 0.0, 1.0),
            glitter_brightness=parse_float(self.vars.get("glitter_brightness"), 1.0, 0.0, 1.0),
            blue_tint=parse_float(self.vars.get("blue_tint"), 0.16, 0.0, 1.0),
            warm_tint=parse_float(self.vars.get("warm_tint"), 0.03, 0.0, 1.0),
            top_density=parse_float(self.vars.get("top_density"), 0.70, 0.0, 2.0),
            bottom_density=parse_float(self.vars.get("bottom_density"), 1.0, 0.0, 2.0),
            center_fade=parse_float(self.vars.get("center_fade"), 0.18, 0.0, 1.0),
            flow_angle_deg=parse_float(self.vars.get("flow_angle_deg"), -18.0, -180.0, 180.0),
            trail_length=parse_float(self.vars.get("trail_length"), 6.0, 0.0, 128.0),
            trail_opacity=parse_float(self.vars.get("trail_opacity"), 0.055, 0.0, 1.0),
            trail_steps=parse_int(self.vars.get("trail_steps"), 5, 0, 64),
            drift_pixels_per_loop=parse_float(self.vars.get("drift_pixels_per_loop"), 72.0, 0.0, 4096.0),
            shimmer_variation=parse_float(self.vars.get("shimmer_variation"), 0.35, 0.0, 1.0),
            alpha_noise=parse_float(self.vars.get("alpha_noise"), 0.18, 0.0, 1.0),
            preview_scale=parse_float(self.vars.get("preview_scale"), 0.75, 0.05, 4.0),
            preview_tile_repeat=parse_int(self.vars.get("preview_tile_repeat"), 2, 1, 4),
            frame_count=parse_int(self.vars.get("frame_count"), 48, 1, 600),
            animation_count=parse_int(self.vars.get("animation_count"), 1, 1, 64),
            output_name=parse_string(self.vars.get("output_name"), "dust"),
        )

    def schedule_preview(self, immediate: bool = False) -> None:
        if self.suppress_preview:
            return

        if immediate:
            self.preview_pending = False
            self.update_preview()
            return

        if self.preview_pending:
            return

        self.preview_pending = True
        self.root.after(160, self._deferred_preview)

    def _deferred_preview(self) -> None:
        self.preview_pending = False
        self.update_preview()

    def update_preview(self) -> None:
        if self.suppress_preview:
            return

        try:
            settings = self.read_settings()
            tile = make_tile(settings, self.animation_phase)
            self.current_tile = tile

            preview = make_preview_image(
                tile,
                settings.preview_tile_repeat,
                settings.preview_scale,
            )

            self.preview_photo = ImageTk.PhotoImage(preview)
            self.preview_label.configure(image=self.preview_photo)

            self.status_var.set(
                f"Preview ready. Output folder: {self.output_dir}"
            )
        except Exception as exc:
            self.status_var.set(f"Preview failed: {exc}")

    def animate_tick(self) -> None:
        if not self.animation_running:
            return

        self.animation_phase = (self.animation_phase + 1.0 / 48.0) % 1.0
        self.update_preview()
        self.root.after(90, self.animate_tick)

    def toggle_animation(self) -> None:
        self.animation_running = not self.animation_running
        if self.animation_running:
            self.animate_tick()

    def random_seed(self) -> None:
        if "seed" in self.vars:
            self.vars["seed"].set(str(random.randint(0, 9999999)))
        self.schedule_preview(immediate=True)

    def save_tile(self) -> None:
        try:
            settings = self.read_settings()
            tile = make_tile(settings, 0.0)

            path = self.output_dir / f"{settings.output_name}_tile_{settings.seed}_{settings.tile_size}.png"
            tile.save(path)

            self.status_var.set(f"Saved tile: {path}")
            messagebox.showinfo("Saved", f"Saved tile:\n{path}")
        except Exception as exc:
            messagebox.showerror("Save failed", str(exc))

    def save_tile_variants(self, count: int) -> None:
        try:
            settings = self.read_settings()
            saved: list[Path] = []

            for index in range(count):
                variant = DustSettings(**settings.__dict__)
                variant.seed = settings.seed + index * 7919
                tile = make_tile(variant, 0.0)

                path = self.output_dir / f"{settings.output_name}_tile_{index:02d}_{settings.tile_size}.png"
                tile.save(path)
                saved.append(path)

            self.status_var.set(f"Saved {len(saved)} tile variants.")
            messagebox.showinfo("Saved", f"Saved {len(saved)} tile variants to:\n{self.output_dir}")
        except Exception as exc:
            messagebox.showerror("Save variants failed", str(exc))

    def save_animation_frames(self) -> None:
        try:
            settings = self.read_settings()
            folder = self.output_dir / f"{settings.output_name}_anim_{settings.seed}_{settings.tile_size}_{settings.frame_count}f"
            folder.mkdir(parents=True, exist_ok=True)

            for frame in range(settings.frame_count):
                phase01 = frame / float(settings.frame_count)
                tile = make_tile(settings, phase01)
                tile.save(folder / f"frame_{frame:04d}.png")

                if frame % 4 == 0:
                    self.status_var.set(f"Saving frame {frame + 1}/{settings.frame_count}...")
                    self.root.update_idletasks()

            self.status_var.set(f"Saved animation frames: {folder}")
            messagebox.showinfo("Saved", f"Saved {settings.frame_count} frames to:\n{folder}")
        except Exception as exc:
            messagebox.showerror("Save animation failed", str(exc))

    def save_many_animations(self) -> None:
        try:
            settings = self.read_settings()
            root_folder = self.output_dir / f"{settings.output_name}_animations"
            root_folder.mkdir(parents=True, exist_ok=True)

            total = settings.animation_count * settings.frame_count
            done = 0

            for anim_index in range(settings.animation_count):
                variant = DustSettings(**settings.__dict__)
                variant.seed = settings.seed + anim_index * 7919

                folder = root_folder / f"{settings.output_name}_anim_{anim_index:02d}_{variant.tile_size}_{variant.frame_count}f"
                folder.mkdir(parents=True, exist_ok=True)

                for frame in range(variant.frame_count):
                    phase01 = frame / float(variant.frame_count)
                    tile = make_tile(variant, phase01)
                    tile.save(folder / f"frame_{frame:04d}.png")

                    done += 1
                    if done % 4 == 0:
                        self.status_var.set(f"Saving {done}/{total} frames...")
                        self.root.update_idletasks()

            self.status_var.set(f"Saved {settings.animation_count} animations to {root_folder}")
            messagebox.showinfo(
                "Saved",
                f"Saved {settings.animation_count} animations to:\n{root_folder}",
            )
        except Exception as exc:
            messagebox.showerror("Save many animations failed", str(exc))


def main() -> None:
    root = tk.Tk()
    DustGeneratorUI(root)
    root.mainloop()


if __name__ == "__main__":
    main()