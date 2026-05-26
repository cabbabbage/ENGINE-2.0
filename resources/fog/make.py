#!/usr/bin/env python3

import json
import math
import os
import queue
import shutil
import threading
import tkinter as tk
from tkinter import ttk, filedialog, colorchooser, messagebox

import numpy as np
from PIL import Image, ImageFilter, ImageTk


# =========================================================
# Helpers
# =========================================================

def clamp01(x):
    return max(0.0, min(1.0, float(x)))


def smoothstep(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0 + 1e-8), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def hex_to_rgb01(hex_color):
    s = str(hex_color).strip()
    if not s.startswith("#"):
        s = "#" + s
    if len(s) != 7:
        raise ValueError(f"Invalid color: {hex_color}")
    r = int(s[1:3], 16)
    g = int(s[3:5], 16)
    b = int(s[5:7], 16)
    return np.array([r, g, b], dtype=np.float32) / 255.0


def ensure_hex_color(s):
    s = str(s).strip()
    if not s.startswith("#"):
        s = "#" + s
    if len(s) != 7:
        raise ValueError(f"Invalid color: {s}")
    int(s[1:], 16)
    return s.upper()


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


def noise_layer(h, w, grid, blur_radius, rng):
    gh = max(2, int(np.ceil(h / grid)))
    gw = max(2, int(np.ceil(w / grid)))

    small = rng.random((gh, gw), dtype=np.float32)

    im = Image.fromarray((small * 255.0).astype(np.uint8), mode="L")
    im = im.resize((w, h), resample=Image.BILINEAR)

    if blur_radius > 0.0:
        im = im.filter(ImageFilter.GaussianBlur(radius=blur_radius))

    return np.asarray(im, dtype=np.float32) / 255.0


def build_gradient_rgb(width, height, params):
    base = hex_to_rgb01(params["base_color"])
    brightness = float(params["brightness"])
    gradient_enabled = bool(params["gradient_enabled"])

    if not gradient_enabled:
        rgb = np.ones((height, width, 3), dtype=np.float32) * base.reshape(1, 1, 3)
        rgb *= brightness
        return np.clip(rgb, 0.0, 1.0)

    start_rgb = hex_to_rgb01(params["gradient_start_color"])
    end_rgb = hex_to_rgb01(params["gradient_end_color"])
    strength = clamp01(params["gradient_strength"])
    grad_type = params["gradient_type"]

    y, x = np.mgrid[0:height, 0:width].astype(np.float32)

    vertical = y / max(1.0, float(height - 1))

    cx = (width - 1) * 0.5
    cy = (height - 1) * 0.68
    dx = x - cx
    dy = y - cy
    radial = np.sqrt(dx * dx + dy * dy)
    radial /= max(1.0, float(radial.max()))
    radial = np.clip(radial, 0.0, 1.0)

    if grad_type == "Vertical":
        t = vertical
    elif grad_type == "Radial":
        t = radial
    elif grad_type == "Vertical + Radial":
        t = 0.5 * vertical + 0.5 * radial
    else:
        t = vertical

    grad_rgb = start_rgb.reshape(1, 1, 3) * (1.0 - t[..., None]) + end_rgb.reshape(1, 1, 3) * t[..., None]
    final_rgb = base.reshape(1, 1, 3) * (1.0 - strength) + grad_rgb * strength
    final_rgb *= brightness

    return np.clip(final_rgb, 0.0, 1.0)


# =========================================================
# Fog generation
# =========================================================

def build_looping_fog_source(width, height, seed, params):
    rng = np.random.default_rng(seed)

    max_motion = max(
        float(params["drift_radius_px"]),
        float(params["secondary_drift_radius_px"]),
        float(params["warp_strength_px"]),
        1.0,
    )

    pad = int(math.ceil(max_motion * 2.5 + 80))
    source_w = width + pad * 2
    source_h = height + pad * 2
    max_dim = max(width, height)

    big = noise_layer(
        source_h,
        source_w,
        grid=max(2, int(max_dim / 6)),
        blur_radius=9.0,
        rng=rng,
    )

    med = noise_layer(
        source_h,
        source_w,
        grid=max(2, int(max_dim / 14)),
        blur_radius=4.5,
        rng=rng,
    )

    fine = noise_layer(
        source_h,
        source_w,
        grid=max(2, int(max_dim / 38)),
        blur_radius=1.6,
        rng=rng,
    )

    fog_source = (0.55 * big + 0.30 * med + 0.15 * fine).astype(np.float32)
    fog_source = np.clip(fog_source, 0.0, 1.0)

    warp_a = noise_layer(
        source_h,
        source_w,
        grid=max(2, int(max_dim / 10)),
        blur_radius=5.5,
        rng=rng,
    )
    warp_b = noise_layer(
        source_h,
        source_w,
        grid=max(2, int(max_dim / 10)),
        blur_radius=5.5,
        rng=rng,
    )

    warp_a = ((warp_a - 0.5) * 2.0).astype(np.float32)
    warp_b = ((warp_b - 0.5) * 2.0).astype(np.float32)

    edge_wobble = noise_layer(
        height,
        width,
        grid=max(2, int(max_dim / 7)),
        blur_radius=10.0,
        rng=rng,
    )
    edge_wobble = ((edge_wobble - 0.5) * 2.0).astype(np.float32)

    return {
        "pad": pad,
        "fog_source": fog_source,
        "warp_a": warp_a,
        "warp_b": warp_b,
        "edge_wobble": edge_wobble,
    }


def make_fog_frame_at_t(width, height, source, t, params):
    angle = 2.0 * math.pi * t
    loop_x = math.cos(angle)
    loop_y = math.sin(angle)

    angle2 = angle * 2.0 + math.pi * 0.35
    loop2_x = math.cos(angle2)
    loop2_y = math.sin(angle2)

    fog_source = source["fog_source"]
    warp_a_source = source["warp_a"]
    warp_b_source = source["warp_b"]
    edge_wobble = source["edge_wobble"]
    pad = source["pad"]

    drift_radius_px = float(params["drift_radius_px"])
    secondary_drift_radius_px = float(params["secondary_drift_radius_px"])
    warp_strength_px = float(params["warp_strength_px"])
    opacity = clamp01(params["opacity"])
    density = max(0.0, float(params["density"]))
    fog_scale = max(0.05, float(params["fog_scale"]))

    y, x = np.mgrid[0:height, 0:width].astype(np.float32)

    base_x = x + pad
    base_y = y + pad

    warp_sample_x = (
        base_x
        + loop_x * secondary_drift_radius_px
        + loop2_x * secondary_drift_radius_px * 0.45
    )
    warp_sample_y = (
        base_y
        + loop_y * secondary_drift_radius_px
        + loop2_y * secondary_drift_radius_px * 0.45
    )

    warp_a = bilinear_sample(warp_a_source, warp_sample_x, warp_sample_y)
    warp_b = bilinear_sample(warp_b_source, warp_sample_x, warp_sample_y)

    sample_x = (
        base_x
        + loop_x * drift_radius_px
        + loop2_x * secondary_drift_radius_px
        + warp_a * warp_strength_px
    )
    sample_y = (
        base_y
        + loop_y * drift_radius_px
        + loop2_y * secondary_drift_radius_px
        + warp_b * warp_strength_px
    )

    fog = bilinear_sample(fog_source, sample_x, sample_y).astype(np.float32)
    fog = np.clip(fog, 0.0, 1.0)

    # Vertical fade
    y01 = (np.arange(height, dtype=np.float32) / max(1, height - 1)).reshape(height, 1)
    vertical_mask = smoothstep(0.03, 1.0, y01)
    vertical_mask = vertical_mask ** 1.85
    fog = fog * vertical_mask

    # Stable fog body shape
    cx = (width - 1) * 0.5
    cy = (height - 1) * 0.68

    x_limit = min(cx - 2.0, (width - 1) - cx - 2.0)
    y_limit = min(cy - 2.0, (height - 1) - cy - 2.0)
    x_limit = max(10.0, x_limit)
    y_limit = max(10.0, y_limit)

    bottom_stretch = 0.55

    rx_base = min(x_limit / (1.0 + bottom_stretch), width * (0.46 * fog_scale))
    ry = min(y_limit, height * (0.46 * fog_scale))

    rx_base = max(20.0, rx_base * 0.98)
    ry = max(20.0, ry * 0.98)

    dx = x - cx
    dy = y - cy
    y01_full = y / max(1.0, float(height - 1))
    widen = 1.0 + bottom_stretch * y01_full

    wobble = 0.06 * edge_wobble

    nx = dx / (rx_base * widen + 1e-8)
    ny = dy / (ry + 1e-8)
    r_ell = np.sqrt(nx * nx + ny * ny)

    dist_norm = (1.0 - r_ell) + wobble
    aa = 0.010
    shape_mask = smoothstep(-aa, aa, dist_norm)

    fog = fog * shape_mask

    fade_to_center = 0.95
    edge_fade = smoothstep(0.0, fade_to_center, np.clip(dist_norm, 0.0, 1.0))
    edge_fade = edge_fade ** 1.15

    final_alpha = np.clip(fog * edge_fade * density, 0.0, 1.0)
    final_alpha *= opacity

    rgb = build_gradient_rgb(width, height, params)
    rgb_u8 = np.clip(rgb * 255.0, 0.0, 255.0).astype(np.uint8)
    alpha_u8 = np.clip(final_alpha * 255.0, 0.0, 255.0).astype(np.uint8)

    rgba = np.dstack([rgb_u8, alpha_u8])

    return Image.fromarray(rgba, mode="RGBA")


# =========================================================
# Tkinter app
# =========================================================

class FogLoopApp:
    def __init__(self, root):
        self.root = root
        self.root.title("Looping Fog Animation Generator")
        self.root.geometry("1350x900")
        self.root.minsize(1200, 800)

        self.preview_after_id = None
        self.preview_photo = None
        self.worker_thread = None
        self.msg_queue = queue.Queue()

        self._build_vars()
        self._build_ui()
        self._bind_visual_updates()

        self.root.after(100, self._poll_queue)
        self._schedule_preview()

    def _build_vars(self):
        script_dir = os.path.dirname(os.path.abspath(__file__))

        self.width_var = tk.StringVar(value="1920")
        self.height_var = tk.StringVar(value="1920")

        self.base_color_var = tk.StringVar(value="#FFFFFF")
        self.brightness_var = tk.StringVar(value="1.0")
        self.opacity_var = tk.StringVar(value="0.55")
        self.density_var = tk.StringVar(value="1.15")
        self.fog_scale_var = tk.StringVar(value="1.0")

        self.gradient_enabled_var = tk.IntVar(value=1)
        self.gradient_type_var = tk.StringVar(value="Vertical")
        self.gradient_start_color_var = tk.StringVar(value="#FFFFFF")
        self.gradient_end_color_var = tk.StringVar(value="#DDE8FF")
        self.gradient_strength_var = tk.StringVar(value="0.35")

        self.drift_radius_var = tk.StringVar(value="90.0")
        self.secondary_drift_radius_var = tk.StringVar(value="35.0")
        self.warp_strength_var = tk.StringVar(value="18.0")

        self.base_seed_var = tk.StringVar(value="12345")

        self.num_animations_var = tk.StringVar(value="3")
        self.frames_per_animation_var = tk.StringVar(value="120")
        self.fps_var = tk.StringVar(value="30")

        self.output_dir_var = tk.StringVar(value=script_dir)
        self.output_name_var = tk.StringVar(value="fog_output")
        self.clear_output_var = tk.IntVar(value=1)

        self.status_var = tk.StringVar(value="Ready.")
        self.progress_var = tk.DoubleVar(value=0.0)

    def _build_ui(self):
        main = ttk.Frame(self.root, padding=10)
        main.pack(fill="both", expand=True)

        left = ttk.Frame(main)
        left.pack(side="left", fill="y")

        right = ttk.Frame(main)
        right.pack(side="left", fill="both", expand=True, padx=(12, 0))

        self._build_controls(left)
        self._build_preview_panel(right)

    def _build_controls(self, parent):
        canvas_frame = ttk.LabelFrame(parent, text="Canvas", padding=10)
        canvas_frame.pack(fill="x", pady=(0, 8))
        self._add_labeled_entry(canvas_frame, "Width", self.width_var, 0)
        self._add_labeled_entry(canvas_frame, "Height", self.height_var, 1)

        appearance_frame = ttk.LabelFrame(parent, text="Appearance", padding=10)
        appearance_frame.pack(fill="x", pady=(0, 8))
        self._add_color_row(appearance_frame, "Fog Color", self.base_color_var, 0)
        self._add_labeled_entry(appearance_frame, "Brightness", self.brightness_var, 1)
        self._add_labeled_entry(appearance_frame, "Opacity", self.opacity_var, 2)
        self._add_labeled_entry(appearance_frame, "Density", self.density_var, 3)
        self._add_labeled_entry(appearance_frame, "Fog Scale", self.fog_scale_var, 4)

        gradient_frame = ttk.LabelFrame(parent, text="Gradient", padding=10)
        gradient_frame.pack(fill="x", pady=(0, 8))

        gradient_enabled_cb = ttk.Checkbutton(
            gradient_frame,
            text="Enable Gradient",
            variable=self.gradient_enabled_var,
            command=self._schedule_preview,
        )
        gradient_enabled_cb.grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 6))

        ttk.Label(gradient_frame, text="Type").grid(row=1, column=0, sticky="w", padx=(0, 8), pady=3)
        gradient_type_combo = ttk.Combobox(
            gradient_frame,
            textvariable=self.gradient_type_var,
            values=["Vertical", "Radial", "Vertical + Radial"],
            state="readonly",
            width=20,
        )
        gradient_type_combo.grid(row=1, column=1, columnspan=2, sticky="ew", pady=3)
        gradient_type_combo.bind("<<ComboboxSelected>>", lambda e: self._schedule_preview())

        self._add_color_row(gradient_frame, "Start Color", self.gradient_start_color_var, 2)
        self._add_color_row(gradient_frame, "End Color", self.gradient_end_color_var, 3)
        self._add_labeled_entry(gradient_frame, "Strength", self.gradient_strength_var, 4)

        motion_frame = ttk.LabelFrame(parent, text="Motion", padding=10)
        motion_frame.pack(fill="x", pady=(0, 8))
        self._add_labeled_entry(motion_frame, "Drift Radius Px", self.drift_radius_var, 0)
        self._add_labeled_entry(motion_frame, "Secondary Drift Px", self.secondary_drift_radius_var, 1)
        self._add_labeled_entry(motion_frame, "Warp Strength Px", self.warp_strength_var, 2)
        self._add_labeled_entry(motion_frame, "Base Seed", self.base_seed_var, 3)

        output_frame = ttk.LabelFrame(parent, text="Output", padding=10)
        output_frame.pack(fill="x", pady=(0, 8))
        self._add_labeled_entry(output_frame, "Animations", self.num_animations_var, 0)
        self._add_labeled_entry(output_frame, "Frames / Animation", self.frames_per_animation_var, 1)
        self._add_labeled_entry(output_frame, "FPS", self.fps_var, 2)

        ttk.Label(output_frame, text="Output Dir").grid(row=3, column=0, sticky="w", padx=(0, 8), pady=3)
        output_dir_entry = ttk.Entry(output_frame, textvariable=self.output_dir_var, width=28)
        output_dir_entry.grid(row=3, column=1, sticky="ew", pady=3)
        browse_btn = ttk.Button(output_frame, text="Browse", command=self._choose_output_dir)
        browse_btn.grid(row=3, column=2, sticky="ew", padx=(6, 0), pady=3)

        self._add_labeled_entry(output_frame, "Folder Name", self.output_name_var, 4)

        clear_cb = ttk.Checkbutton(output_frame, text="Clear Output Folder First", variable=self.clear_output_var)
        clear_cb.grid(row=5, column=0, columnspan=3, sticky="w", pady=(6, 0))

        for frame in [canvas_frame, appearance_frame, gradient_frame, motion_frame, output_frame]:
            frame.columnconfigure(1, weight=1)

    def _build_preview_panel(self, parent):
        preview_frame = ttk.LabelFrame(parent, text="Preview", padding=10)
        preview_frame.pack(fill="both", expand=True)

        self.preview_label = tk.Label(
            preview_frame,
            text="Generating preview...",
            bg="#202020",
            fg="white",
            anchor="center",
            justify="center",
        )
        self.preview_label.pack(fill="both", expand=True)

        bottom = ttk.Frame(parent)
        bottom.pack(fill="x", pady=(8, 0))

        self.progress_bar = ttk.Progressbar(bottom, variable=self.progress_var, maximum=100.0)
        self.progress_bar.pack(fill="x", pady=(0, 8))

        status_label = ttk.Label(bottom, textvariable=self.status_var)
        status_label.pack(fill="x", pady=(0, 8))

        self.generate_button = ttk.Button(bottom, text="Generate Animations", command=self._start_generation)
        self.generate_button.pack(fill="x")

    def _add_labeled_entry(self, parent, label_text, variable, row):
        ttk.Label(parent, text=label_text).grid(row=row, column=0, sticky="w", padx=(0, 8), pady=3)
        entry = ttk.Entry(parent, textvariable=variable, width=16)
        entry.grid(row=row, column=1, columnspan=2, sticky="ew", pady=3)
        return entry

    def _add_color_row(self, parent, label_text, variable, row):
        ttk.Label(parent, text=label_text).grid(row=row, column=0, sticky="w", padx=(0, 8), pady=3)

        entry = ttk.Entry(parent, textvariable=variable, width=16)
        entry.grid(row=row, column=1, sticky="ew", pady=3)

        btn = ttk.Button(parent, text="Pick", command=lambda v=variable: self._pick_color(v))
        btn.grid(row=row, column=2, sticky="ew", padx=(6, 0), pady=3)

        swatch = tk.Label(parent, width=3, relief="solid", borderwidth=1, bg=variable.get())
        swatch.grid(row=row, column=3, sticky="ew", padx=(6, 0), pady=3)

        def update_swatch(*_):
            try:
                swatch.configure(bg=ensure_hex_color(variable.get()))
            except Exception:
                pass

        variable.trace_add("write", update_swatch)
        return entry

    def _pick_color(self, variable):
        initial = variable.get()
        rgb, hx = colorchooser.askcolor(color=initial, parent=self.root)
        if hx:
            variable.set(hx.upper())
            self._schedule_preview()

    def _choose_output_dir(self):
        chosen = filedialog.askdirectory(parent=self.root, initialdir=self.output_dir_var.get() or os.getcwd())
        if chosen:
            self.output_dir_var.set(chosen)

    def _bind_visual_updates(self):
        visual_vars = [
            self.width_var,
            self.height_var,
            self.base_color_var,
            self.brightness_var,
            self.opacity_var,
            self.density_var,
            self.fog_scale_var,
            self.gradient_start_color_var,
            self.gradient_end_color_var,
            self.gradient_strength_var,
            self.gradient_type_var,
            self.drift_radius_var,
            self.secondary_drift_radius_var,
            self.warp_strength_var,
            self.base_seed_var,
        ]

        for var in visual_vars:
            var.trace_add("write", lambda *_: self._schedule_preview())

        self.gradient_enabled_var.trace_add("write", lambda *_: self._schedule_preview())

    def _schedule_preview(self):
        if self.preview_after_id is not None:
            self.root.after_cancel(self.preview_after_id)
        self.preview_after_id = self.root.after(180, self._update_preview)

    def _collect_params(self, show_errors=True):
        try:
            width = max(64, int(self.width_var.get()))
            height = max(64, int(self.height_var.get()))

            params = {
                "width": width,
                "height": height,
                "base_color": ensure_hex_color(self.base_color_var.get()),
                "brightness": float(self.brightness_var.get()),
                "opacity": float(self.opacity_var.get()),
                "density": float(self.density_var.get()),
                "fog_scale": float(self.fog_scale_var.get()),
                "gradient_enabled": bool(self.gradient_enabled_var.get()),
                "gradient_type": self.gradient_type_var.get().strip() or "Vertical",
                "gradient_start_color": ensure_hex_color(self.gradient_start_color_var.get()),
                "gradient_end_color": ensure_hex_color(self.gradient_end_color_var.get()),
                "gradient_strength": float(self.gradient_strength_var.get()),
                "drift_radius_px": float(self.drift_radius_var.get()),
                "secondary_drift_radius_px": float(self.secondary_drift_radius_var.get()),
                "warp_strength_px": float(self.warp_strength_var.get()),
                "base_seed": int(self.base_seed_var.get()),
                "num_animations": max(1, int(self.num_animations_var.get())),
                "frames_per_animation": max(1, int(self.frames_per_animation_var.get())),
                "fps": max(1, int(self.fps_var.get())),
                "output_dir": self.output_dir_var.get().strip(),
                "output_name": self.output_name_var.get().strip() or "fog_output",
                "clear_output": bool(self.clear_output_var.get()),
            }

            return params

        except Exception as exc:
            if show_errors:
                messagebox.showerror("Invalid Input", str(exc), parent=self.root)
            return None

    def _scaled_preview_params(self, params):
        scale = 1.0 / 3.0
        preview_w = max(96, int(round(params["width"] * scale)))
        preview_h = max(96, int(round(params["height"] * scale)))

        preview_params = dict(params)
        preview_params["width"] = preview_w
        preview_params["height"] = preview_h
        preview_params["drift_radius_px"] = params["drift_radius_px"] * scale
        preview_params["secondary_drift_radius_px"] = params["secondary_drift_radius_px"] * scale
        preview_params["warp_strength_px"] = params["warp_strength_px"] * scale
        return preview_params

    def _update_preview(self):
        self.preview_after_id = None
        params = self._collect_params(show_errors=False)
        if params is None:
            return

        try:
            preview_params = self._scaled_preview_params(params)

            source = build_looping_fog_source(
                preview_params["width"],
                preview_params["height"],
                preview_params["base_seed"],
                preview_params,
            )

            img = make_fog_frame_at_t(
                preview_params["width"],
                preview_params["height"],
                source,
                t=0.18,
                params=preview_params,
            )

            max_preview_box_w = 760
            max_preview_box_h = 760
            img.thumbnail((max_preview_box_w, max_preview_box_h), Image.LANCZOS)

            self.preview_photo = ImageTk.PhotoImage(img)
            self.preview_label.configure(image=self.preview_photo, text="")
            self.status_var.set("Preview updated.")

        except Exception as exc:
            self.preview_label.configure(image="", text=f"Preview error:\n{exc}")
            self.status_var.set("Preview error.")

    def _start_generation(self):
        if self.worker_thread and self.worker_thread.is_alive():
            return

        params = self._collect_params(show_errors=True)
        if params is None:
            return

        if not params["output_dir"]:
            messagebox.showerror("Invalid Output", "Please select an output directory.", parent=self.root)
            return

        self.progress_var.set(0.0)
        self.status_var.set("Starting generation...")
        self.generate_button.configure(state="disabled")

        self.worker_thread = threading.Thread(
            target=self._generate_worker,
            args=(params,),
            daemon=True,
        )
        self.worker_thread.start()

    def _generate_worker(self, params):
        try:
            root_output = os.path.join(params["output_dir"], params["output_name"])

            if params["clear_output"] and os.path.isdir(root_output):
                shutil.rmtree(root_output)

            os.makedirs(root_output, exist_ok=True)

            config_path = os.path.join(root_output, "config.json")
            with open(config_path, "w", encoding="utf-8") as f:
                json.dump(params, f, indent=2)

            total_frames = params["num_animations"] * params["frames_per_animation"]
            done = 0

            for anim_index in range(params["num_animations"]):
                anim_seed = params["base_seed"] + anim_index * 100003
                anim_dir = os.path.join(root_output, f"animation_{anim_index + 1:02d}")
                os.makedirs(anim_dir, exist_ok=True)

                source = build_looping_fog_source(
                    params["width"],
                    params["height"],
                    anim_seed,
                    params,
                )

                for frame_index in range(params["frames_per_animation"]):
                    t = frame_index / float(params["frames_per_animation"])
                    img = make_fog_frame_at_t(
                        params["width"],
                        params["height"],
                        source,
                        t=t,
                        params=params,
                    )

                    out_path = os.path.join(anim_dir, f"fog_{frame_index:04d}.png")
                    img.save(out_path)

                    done += 1
                    progress_pct = 100.0 * (done / total_frames)
                    self.msg_queue.put((
                        "progress",
                        progress_pct,
                        f"Generating animation {anim_index + 1}/{params['num_animations']} | "
                        f"frame {frame_index + 1}/{params['frames_per_animation']}"
                    ))

            self.msg_queue.put(("done", root_output))

        except Exception as exc:
            self.msg_queue.put(("error", str(exc)))

    def _poll_queue(self):
        try:
            while True:
                msg = self.msg_queue.get_nowait()
                kind = msg[0]

                if kind == "progress":
                    _, pct, text = msg
                    self.progress_var.set(pct)
                    self.status_var.set(text)

                elif kind == "done":
                    _, root_output = msg
                    self.progress_var.set(100.0)
                    self.status_var.set(f"Done. Output: {root_output}")
                    self.generate_button.configure(state="normal")
                    messagebox.showinfo("Done", f"Animations generated in:\n{root_output}", parent=self.root)

                elif kind == "error":
                    _, error_text = msg
                    self.status_var.set(f"Error: {error_text}")
                    self.generate_button.configure(state="normal")
                    messagebox.showerror("Generation Error", error_text, parent=self.root)

        except queue.Empty:
            pass

        self.root.after(100, self._poll_queue)


# =========================================================
# Main
# =========================================================

def main():
    root = tk.Tk()
    app = FogLoopApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()