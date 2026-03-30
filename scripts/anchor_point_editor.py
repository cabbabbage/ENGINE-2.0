#!/usr/bin/env python3
import argparse
import copy
import json
import math
import random
from pathlib import Path
import tkinter as tk
from tkinter import messagebox

try:
    from PIL import Image, ImageTk
except ImportError:  # pragma: no cover
    Image = None
    ImageTk = None

DEFAULT_MANIFEST_PATH = Path(__file__).resolve().parents[1] / "manifest.json"

PALETTE = {
    "bg": "#0f1116",
    "panel": "#181c26",
    "nav_bg": "#0f121a",
    "canvas_bg": "#0c0f14",
    "accent": "#57c7ff",
    "muted": "#8b94a6",
    "text": "#f5f7fb",
    "button_bg": "#212737",
    "button_active": "#313a4c",
    "entry_bg": "#10141f",
    "entry_border": "#2c3445",
    "danger": "#d95c5c",
    "hit_box": "#f4b860",
    "attack_box": "#ff6c6c",
}

FONTS = {
    "title": ("Segoe UI", 11, "bold"),
    "section": ("Segoe UI", 10, "bold"),
    "body": ("Segoe UI", 9),
    "label": ("Segoe UI", 9),
    "button": ("Segoe UI", 9, "bold"),
}

ANCHOR_MARKER_RADIUS = 8
CORNER_MARKER_RADIUS = 6
SELECT_RADIUS_PX = 16


def _create_styled_button(parent, text, command, accent=False, **kwargs):
    bg_color = PALETTE["accent"] if accent else PALETTE["button_bg"]
    button = tk.Button(
        parent,
        text=text,
        command=command,
        bg=bg_color,
        fg=PALETTE["text"],
        activebackground=PALETTE["button_active"],
        activeforeground=PALETTE["text"],
        bd=0,
        relief="flat",
        font=FONTS["button"],
        highlightthickness=0,
        pady=4,
        padx=12,
        **kwargs,
    )
    return button


def load_manifest(manifest_path: Path):
    if not manifest_path.exists():
        raise FileNotFoundError(f"Manifest not found: {manifest_path}")
    with manifest_path.open("r", encoding="utf-8") as fh:
        data = json.load(fh)
    if not isinstance(data, dict):
        raise RuntimeError("manifest.json must contain a root object")
    return data


def resolve_asset_key(assets, asset_id):
    if not isinstance(assets, dict):
        return None
    if asset_id in assets:
        return asset_id
    for key, value in assets.items():
        if isinstance(value, dict) and value.get("asset_name") == asset_id:
            return key
    return None


def _read_int(value, default=0):
    try:
        return int(float(str(value).strip()))
    except Exception:
        return default


def _normalize_unique_name(desired_name, existing_names, default_prefix):
    base = str(desired_name or "").strip() or default_prefix
    candidate = base
    suffix = 2
    while candidate in existing_names:
        candidate = f"{base}_{suffix}"
        suffix += 1
    return candidate


def _infer_frame_count(payload):
    if not isinstance(payload, dict):
        return 1
    counts = [1]
    for key in ("number_of_frames",):
        counts.append(max(0, _read_int(payload.get(key, 0), 0)))
    for key in ("movement", "frames", "anchor_points", "hit_boxes", "attack_boxes"):
        value = payload.get(key)
        if isinstance(value, list):
            counts.append(len(value))
    return max(1, max(counts))


def _frame_count_from_source(manifest_root: Path, asset, asset_key, payload):
    if not isinstance(payload, dict):
        return None
    source = payload.get("source")
    if not isinstance(source, dict) or source.get("kind") != "folder":
        return None
    path_value = str(source.get("path") or "").strip()
    asset_dir_value = asset.get("asset_directory") if isinstance(asset, dict) else None
    if asset_dir_value:
        base = Path(asset_dir_value)
        if not base.is_absolute():
            base = manifest_root / base
    else:
        base = manifest_root / "resources" / "assets" / str(asset_key or "")
    folder = base / path_value if path_value else base
    try:
        if not folder.exists() or not folder.is_dir():
            return None
        max_index = -1
        for child in folder.iterdir():
            if child.is_file() and child.suffix.lower() == ".png" and child.stem.isdigit():
                max_index = max(max_index, int(child.stem))
        return max_index + 1 if max_index >= 0 else None
    except Exception:
        return None


def _frame_count_with_disk(payload, manifest_root=None, asset=None, asset_key=None):
    manifest_count = _infer_frame_count(payload)
    disk_count = None
    if manifest_root is not None and asset is not None:
        disk_count = _frame_count_from_source(manifest_root, asset, asset_key, payload)
    if disk_count is not None:
        return max(manifest_count, disk_count)
    return manifest_count


def _normalize_anchor(anchor, fallback_name=None):
    if not isinstance(anchor, dict):
        return None
    name = str(anchor.get("name", "")).strip()
    if not name:
        name = str(fallback_name or "").strip()
    if not name:
        return None
    return {
        "name": name,
        "texture_x": max(0, _read_int(anchor.get("texture_x", 0), 0)),
        "texture_y": max(0, _read_int(anchor.get("texture_y", 0), 0)),
        "depth_offset": _read_int(anchor.get("depth_offset", 0), 0),
    }


def _normalize_anchor_points(payload, frame_count_override=None):
    inferred = _infer_frame_count(payload)
    frame_count = max(frame_count_override or 0, inferred)
    source = payload.get("anchor_points") if isinstance(payload, dict) else None
    out = [[] for _ in range(frame_count)]
    if isinstance(source, list):
        for idx in range(min(frame_count, len(source))):
            frame = source[idx]
            if not isinstance(frame, list):
                continue
            used_names = set()
            normalized_frame = []
            for anchor_idx, anchor in enumerate(frame):
                fallback_name = f"anchor_{anchor_idx + 1}"
                normalized = _normalize_anchor(anchor, fallback_name=fallback_name)
                if not normalized:
                    continue
                normalized_name = _normalize_unique_name(normalized["name"], used_names, fallback_name)
                used_names.add(normalized_name)
                normalized["name"] = normalized_name
                normalized_frame.append(normalized)
            out[idx] = normalized_frame
    return out


def _normalize_corner(corner):
    if not isinstance(corner, dict):
        return {"texture_x": 0, "texture_y": 0}
    return {
        "texture_x": max(0, _read_int(corner.get("texture_x", 0), 0)),
        "texture_y": max(0, _read_int(corner.get("texture_y", 0), 0)),
    }


def _normalize_boxes(payload, key, frame_count_override=None, attack=False):
    inferred = _infer_frame_count(payload)
    frame_count = max(frame_count_override or 0, inferred)
    source = payload.get(key) if isinstance(payload, dict) else None
    out = [[] for _ in range(frame_count)]
    if isinstance(source, list):
        for idx in range(min(frame_count, len(source))):
            frame = source[idx]
            if not isinstance(frame, list):
                continue
            used_names = set()
            normalized = []
            for box_idx, box in enumerate(frame):
                if not isinstance(box, dict):
                    continue
                default_name = ("attack_box" if attack else "hit_box") + f"_{box_idx + 1}"
                name = _normalize_unique_name(box.get("name"), used_names, default_name)
                used_names.add(name)
                corners_in = box.get("corners") if isinstance(box.get("corners"), list) else []
                corners = [_normalize_corner(corners_in[i] if i < len(corners_in) else {}) for i in range(4)]
                item = {
                    "name": name,
                    "extrusion_amount": max(0, _read_int(box.get("extrusion_amount", 0), 0)),
                    "corners": corners,
                }
                if attack:
                    item["damage_amount"] = _read_int(box.get("damage_amount", box.get("damage", 0)), 0)
                normalized.append(item)
            out[idx] = normalized
    return out

class AnchorEditorApp:
    def __init__(self, root, manifest_path: Path, asset_id: str, animation_id: str):
        self.root = root
        self.manifest_path = manifest_path
        self.asset_id = asset_id
        self.animation_id = animation_id
        self.loading = False
        self.current_frame = 0
        self.manifest_root = self.manifest_path.parent
        self._frame_pil_image = None
        self._preview_photo = None
        self._preview_scale = 1.0
        self._preview_offset = (0, 0)
        self._preview_flip = (False, False)
        self._frame_image_size = (0, 0)
        self._dragging = False
        self._dirty_drag = False
        self._entity_map = []

        self.selected_kind = None
        self.selected_index = -1
        self.selected_corner = 0

        self.manifest = load_manifest(self.manifest_path)
        self.asset_key, self.asset = self._resolve_asset()
        self.animations = self.asset.setdefault("animations", {})
        self.payload = self.animations.setdefault(self.animation_id, {})
        if not isinstance(self.payload, dict):
            self.payload = {}
            self.animations[self.animation_id] = self.payload

        frame_count = _frame_count_with_disk(self.payload, self.manifest_root, self.asset, self.asset_key)
        self.anchor_frames = _normalize_anchor_points(self.payload, frame_count_override=frame_count)
        self.hit_box_frames = _normalize_boxes(self.payload, "hit_boxes", frame_count_override=frame_count, attack=False)
        self.attack_box_frames = _normalize_boxes(self.payload, "attack_boxes", frame_count_override=frame_count, attack=True)
        self._ensure_frame_count(frame_count)

        self.root.title(f"Anchor Point Editor - {self.asset_key}:{self.animation_id}")
        self.root.geometry("980x560")
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

        self.frame_label_var = tk.StringVar()
        self.name_var = tk.StringVar()
        self.x_var = tk.StringVar()
        self.y_var = tk.StringVar()
        self.depth_var = tk.StringVar(value="0")
        self.extrusion_var = tk.StringVar(value="0")
        self.damage_var = tk.StringVar(value="0")
        self.detail_title_var = tk.StringVar(value="Selection Details")
        self.detail_note_var = tk.StringVar(value="Select an anchor or a box corner.")

        self._build_ui()
        self._wire_traces()
        self._refresh_frame_ui()
        self._save_manifest()

    def _styled_button(self, parent, text, command, accent=False):
        return _create_styled_button(parent, text, command, accent=accent)

    def _resolve_asset(self):
        assets = self.manifest.setdefault("assets", {})
        if not isinstance(assets, dict):
            raise RuntimeError("manifest.json assets must be an object")
        asset_key = resolve_asset_key(assets, self.asset_id)
        if asset_key is None:
            raise KeyError(f"Asset '{self.asset_id}' not found in manifest")
        asset_value = assets.get(asset_key)
        if not isinstance(asset_value, dict):
            raise RuntimeError(f"Asset '{asset_key}' must be an object")
        return asset_key, asset_value

    def _ensure_frame_count(self, frame_count):
        for data in (self.anchor_frames, self.hit_box_frames, self.attack_box_frames):
            if len(data) < frame_count:
                data.extend([[] for _ in range(frame_count - len(data))])
            elif len(data) > frame_count:
                del data[frame_count:]

    def _frame_count(self):
        return len(self.anchor_frames)

    def _build_ui(self):
        self.root.configure(bg=PALETTE["bg"])
        nav = tk.Frame(self.root, bg=PALETTE["nav_bg"])
        nav.pack(fill="x", padx=8, pady=(8, 4))
        nav_left = tk.Frame(nav, bg=PALETTE["nav_bg"])
        nav_left.pack(side="left")
        nav_right = tk.Frame(nav, bg=PALETTE["nav_bg"])
        nav_right.pack(side="right")
        self._styled_button(nav_left, "<< Prev", self._prev_frame).pack(side="left", padx=(0, 6))
        self._styled_button(nav_left, "Next >>", self._next_frame).pack(side="left")
        tk.Label(nav_left, textvariable=self.frame_label_var, font=FONTS["title"], bg=PALETTE["nav_bg"], fg=PALETTE["accent"], padx=12).pack(side="left")
        self._styled_button(nav_right, "Apply Frame -> All Animations", self._apply_to_all_animations, accent=True).pack(side="right", padx=(0, 4))
        self._styled_button(nav_right, "Apply Frame -> This Animation", self._apply_to_animation, accent=True).pack(side="right", padx=4)
        self._styled_button(nav_right, "Apply Frame -> Next", self._apply_to_next).pack(side="right")

        body = tk.Frame(self.root, bg=PALETTE["bg"])
        body.pack(fill="both", expand=True, padx=8, pady=(0, 8))
        main = tk.Frame(body, bg=PALETTE["panel"])
        main.pack(fill="both", expand=True)

        left = tk.Frame(main, bg=PALETTE["panel"])
        left.pack(side="left", fill="y", padx=(12, 6), pady=12)
        tk.Label(left, text="Entities (current frame)", font=FONTS["section"], fg=PALETTE["text"], bg=PALETTE["panel"]).pack(anchor="w")
        self.entity_list = tk.Listbox(
            left,
            exportselection=False,
            bg=PALETTE["entry_bg"],
            fg=PALETTE["text"],
            selectbackground=PALETTE["accent"],
            selectforeground=PALETTE["text"],
            bd=0,
            highlightthickness=0,
            font=FONTS["body"],
            width=28,
        )
        self.entity_list.pack(fill="both", expand=True, pady=(4, 8))
        self.entity_list.bind("<<ListboxSelect>>", self._on_entity_select)

        btn_col = tk.Frame(left, bg=PALETTE["panel"])
        btn_col.pack(fill="x")
        self._styled_button(btn_col, "Add Anchor Point", self._add_anchor, accent=True).pack(fill="x", pady=(0, 4))
        row = tk.Frame(btn_col, bg=PALETTE["panel"])
        row.pack(fill="x", pady=(0, 4))
        self._styled_button(row, "Add Attack Box", self._add_attack_box).pack(side="left", fill="x", expand=True, padx=(0, 2))
        self._styled_button(row, "Add Hit Box", self._add_hit_box).pack(side="left", fill="x", expand=True, padx=(2, 0))
        row2 = tk.Frame(btn_col, bg=PALETTE["panel"])
        row2.pack(fill="x")
        self._styled_button(row2, "Delete Selected", self._delete_selected).pack(side="left", fill="x", expand=True, padx=(0, 2))
        self._styled_button(row2, "Rnd Anchors", self._randomize_all_anchors).pack(side="left", fill="x", expand=True, padx=(2, 0))

        center = tk.Frame(main, bg=PALETTE["panel"])
        center.pack(side="left", fill="both", expand=True, padx=(0, 6), pady=12)
        tk.Label(center, text="Frame Preview", font=FONTS["section"], fg=PALETTE["text"], bg=PALETTE["panel"]).pack(anchor="w", padx=8)
        self.preview_canvas = tk.Canvas(center, bg=PALETTE["canvas_bg"], highlightthickness=0, bd=0, relief="flat")
        self.preview_canvas.pack(fill="both", expand=True, padx=8, pady=(6, 0))
        self.preview_canvas.bind("<Button-1>", self._on_preview_click)
        self.preview_canvas.bind("<B1-Motion>", self._on_preview_drag)
        self.preview_canvas.bind("<ButtonRelease-1>", self._on_preview_release)
        self.preview_canvas.bind("<Configure>", self._on_preview_configure)

        right = tk.Frame(main, bg=PALETTE["panel"], width=235)
        right.pack(side="left", fill="y", padx=(6, 12), pady=12)
        right.pack_propagate(False)
        tk.Label(right, textvariable=self.detail_title_var, font=FONTS["section"], fg=PALETTE["text"], bg=PALETTE["panel"]).pack(anchor="w")
        self.row_name = self._detail_row(right, "Name", self.name_var)
        self.row_x = self._detail_row(right, "Texture X", self.x_var)
        self.row_y = self._detail_row(right, "Texture Y", self.y_var)
        self.row_depth = self._detail_row(right, "Depth Offset", self.depth_var)
        self.row_extrusion = self._detail_row(right, "Extrusion", self.extrusion_var)
        self.row_damage = self._detail_row(right, "Damage", self.damage_var)
        self.detail_note_label = tk.Label(right, textvariable=self.detail_note_var, font=FONTS["body"], fg=PALETTE["muted"], bg=PALETTE["panel"], wraplength=220, justify="left")
        self.detail_note_label.pack(fill="x", pady=(6, 0))

    def _detail_row(self, parent, label, var):
        row = tk.Frame(parent, bg=PALETTE["panel"])
        lbl = tk.Label(row, text=label, width=11, anchor="w", fg=PALETTE["text"], bg=PALETTE["panel"], font=FONTS["label"])
        lbl.pack(side="left")
        entry = tk.Entry(row, textvariable=var, bg=PALETTE["entry_bg"], fg=PALETTE["text"], insertbackground=PALETTE["text"], bd=1, relief="solid", highlightthickness=0, font=FONTS["body"])
        entry.pack(side="left", fill="x", expand=True)
        row.label_widget = lbl
        row.pack(fill="x", pady=4)
        return row

    def _wire_traces(self):
        self.name_var.trace_add("write", lambda *_: self._on_value_change())
        self.x_var.trace_add("write", lambda *_: self._on_value_change())
        self.y_var.trace_add("write", lambda *_: self._on_value_change())
        self.depth_var.trace_add("write", lambda *_: self._on_value_change())
        self.extrusion_var.trace_add("write", lambda *_: self._on_value_change())
        self.damage_var.trace_add("write", lambda *_: self._on_value_change())

    def _get_source_folder(self):
        source = self.payload.get("source")
        if not isinstance(source, dict) or source.get("kind") != "folder":
            return None
        path_value = str(source.get("path") or "").strip()
        asset_dir_value = self.asset.get("asset_directory")
        if asset_dir_value:
            candidate = Path(asset_dir_value)
            if not candidate.is_absolute():
                candidate = self.manifest_root / candidate
        else:
            candidate = self.manifest_root / "resources" / "assets" / self.asset_key
        return candidate / path_value if path_value else candidate

    def _preview_flip_flags(self):
        if not isinstance(self.payload, dict):
            return (False, False)
        flip_x = bool(self.payload.get("flipped_source")) or bool(self.payload.get("flip_movement_horizontal"))
        flip_y = bool(self.payload.get("flip_vertical_source")) or bool(self.payload.get("flip_movement_vertical"))
        return (flip_x, flip_y)

    def _get_frame_image_path(self, frame_index):
        folder = self._get_source_folder()
        if not folder:
            return None
        return folder / f"{frame_index}.png"

    def _load_frame_image(self):
        self._frame_pil_image = None
        self._frame_image_size = (0, 0)
        self._preview_photo = None
        path = self._get_frame_image_path(self.current_frame)
        if not path or not path.exists() or Image is None or ImageTk is None:
            return
        try:
            image = Image.open(path).convert("RGBA")
        except Exception:
            return
        self._frame_pil_image = image
        self._frame_image_size = (image.width, image.height)

    def _texture_to_canvas(self, texture_x, texture_y):
        scale = self._preview_scale or 1.0
        offset_x, offset_y = self._preview_offset
        width, height = self._frame_image_size
        flip_x, flip_y = self._preview_flip
        w = width if width > 0 else 1
        h = height if height > 0 else 1
        u = (float(texture_x) + 0.5) / float(w)
        v = (float(texture_y) + 0.5) / float(h)
        if flip_x:
            u = 1.0 - u
        if flip_y:
            v = 1.0 - v
        return offset_x + u * float(w) * scale, offset_y + v * float(h) * scale

    def _canvas_to_texture(self, canvas_x, canvas_y, clamp=True):
        scale = self._preview_scale or 1.0
        offset_x, offset_y = self._preview_offset
        width, height = self._frame_image_size
        w = width if width > 0 else 1
        h = height if height > 0 else 1
        u = (canvas_x - offset_x) / (scale * float(w))
        v = (canvas_y - offset_y) / (scale * float(h))
        if clamp:
            u = max(0.0, min(u, 1.0))
            v = max(0.0, min(v, 1.0))
        flip_x, flip_y = self._preview_flip
        if flip_x:
            u = 1.0 - u
        if flip_y:
            v = 1.0 - v
        tx = u * float(w) - 0.5
        ty = v * float(h) - 0.5
        if clamp:
            if width > 0:
                tx = max(0.0, min(tx, float(width - 1)))
            if height > 0:
                ty = max(0.0, min(ty, float(height - 1)))
        return tx, ty

    def _render_preview(self):
        if not hasattr(self, "preview_canvas"):
            return
        width = self.preview_canvas.winfo_width()
        height = self.preview_canvas.winfo_height()
        if width < 10 or height < 10:
            return
        self.preview_canvas.delete("image")
        self.preview_canvas.delete("overlay")
        self.preview_canvas.delete("placeholder")
        self._preview_flip = self._preview_flip_flags()
        if not self._frame_pil_image:
            message = "Image preview unavailable" if Image is not None else "Install Pillow to preview animation frames"
            self.preview_canvas.create_text(width // 2, height // 2, text=message, fill=PALETTE["muted"], font=FONTS["body"], tags=("placeholder",))
            return
        scale = min(width / self._frame_pil_image.width, height / self._frame_pil_image.height)
        if scale <= 0:
            return
        display_w = max(1, int(self._frame_pil_image.width * scale))
        display_h = max(1, int(self._frame_pil_image.height * scale))
        image = self._frame_pil_image
        if scale != 1:
            image = image.resize((display_w, display_h), Image.LANCZOS)
        flip_x, flip_y = self._preview_flip
        transpose_enum = getattr(Image, "Transpose", None)
        if flip_x:
            image = image.transpose(transpose_enum.FLIP_LEFT_RIGHT if transpose_enum else Image.FLIP_LEFT_RIGHT)
        if flip_y:
            image = image.transpose(transpose_enum.FLIP_TOP_BOTTOM if transpose_enum else Image.FLIP_TOP_BOTTOM)
        self._preview_photo = ImageTk.PhotoImage(image)
        offset_x = (width - display_w) // 2
        offset_y = (height - display_h) // 2
        self.preview_canvas.create_image(offset_x, offset_y, image=self._preview_photo, anchor="nw", tags=("image",))
        self._preview_scale = scale
        self._preview_offset = (offset_x, offset_y)
        self._draw_overlays()

    def _draw_box(self, box, kind, idx, color):
        points = []
        for corner in box.get("corners", [])[:4]:
            points.append(self._texture_to_canvas(corner.get("texture_x", 0), corner.get("texture_y", 0)))
        if len(points) != 4:
            return
        selected = self.selected_kind == kind and self.selected_index == idx
        width = 3 if selected else 2
        for i in range(4):
            x1, y1 = points[i]
            x2, y2 = points[(i + 1) % 4]
            self.preview_canvas.create_line(x1, y1, x2, y2, fill=color, width=width, tags=("overlay",))
        for cidx, (cx, cy) in enumerate(points):
            fill = PALETTE["accent"] if selected and cidx == self.selected_corner else color
            self.preview_canvas.create_oval(cx - CORNER_MARKER_RADIUS, cy - CORNER_MARKER_RADIUS, cx + CORNER_MARKER_RADIUS, cy + CORNER_MARKER_RADIUS, fill=fill, outline=PALETTE["canvas_bg"], width=2, tags=("overlay",))

    def _draw_overlays(self):
        if not self._frame_pil_image:
            return
        for idx, anchor in enumerate(self.anchor_frames[self.current_frame]):
            cx, cy = self._texture_to_canvas(anchor["texture_x"], anchor["texture_y"])
            fill = PALETTE["accent"] if self.selected_kind == "anchor" and self.selected_index == idx else PALETTE["button_bg"]
            self.preview_canvas.create_oval(cx - ANCHOR_MARKER_RADIUS, cy - ANCHOR_MARKER_RADIUS, cx + ANCHOR_MARKER_RADIUS, cy + ANCHOR_MARKER_RADIUS, fill=fill, outline=PALETTE["canvas_bg"], width=2, tags=("overlay",))
            self.preview_canvas.create_text(cx, cy - ANCHOR_MARKER_RADIUS - 6, text=anchor["name"], fill=PALETTE["text"], font=("Segoe UI", 8), tags=("overlay",))
        for idx, box in enumerate(self.hit_box_frames[self.current_frame]):
            self._draw_box(box, "hit_box", idx, PALETTE["hit_box"])
        for idx, box in enumerate(self.attack_box_frames[self.current_frame]):
            self._draw_box(box, "attack_box", idx, PALETTE["attack_box"])

    def _current_boxes(self, kind):
        return self.hit_box_frames[self.current_frame] if kind == "hit_box" else self.attack_box_frames[self.current_frame]

    def _set_selection(self, kind=None, index=-1, corner=0):
        self.selected_kind = kind
        self.selected_index = index
        self.selected_corner = max(0, min(3, corner))

    def _refresh_entity_list(self):
        self.entity_list.delete(0, tk.END)
        self._entity_map = []
        for idx, anchor in enumerate(self.anchor_frames[self.current_frame]):
            self.entity_list.insert(tk.END, f"Anchor: {anchor['name']}")
            self._entity_map.append(("anchor", idx))
        for idx, box in enumerate(self.hit_box_frames[self.current_frame]):
            self.entity_list.insert(tk.END, f"Hit Box: {box['name']}")
            self._entity_map.append(("hit_box", idx))
        for idx, box in enumerate(self.attack_box_frames[self.current_frame]):
            self.entity_list.insert(tk.END, f"Attack Box: {box['name']}")
            self._entity_map.append(("attack_box", idx))
        if self.selected_kind:
            for list_index, entry in enumerate(self._entity_map):
                if entry[0] == self.selected_kind and entry[1] == self.selected_index:
                    self.entity_list.selection_set(list_index)
                    self.entity_list.see(list_index)
                    break

    def _show_rows(self, rows):
        all_rows = (self.row_name, self.row_x, self.row_y, self.row_depth, self.row_extrusion, self.row_damage)
        for row in all_rows:
            row.pack_forget()
        for row in rows:
            row.pack(fill="x", pady=4)

    def _load_form(self):
        prev_loading = self.loading
        self.loading = True
        if self.selected_kind == "anchor" and 0 <= self.selected_index < len(self.anchor_frames[self.current_frame]):
            anchor = self.anchor_frames[self.current_frame][self.selected_index]
            self.detail_title_var.set("Anchor Details")
            self.name_var.set(anchor["name"])
            self.x_var.set(str(anchor["texture_x"]))
            self.y_var.set(str(anchor["texture_y"]))
            self.depth_var.set(str(anchor.get("depth_offset", 0)))
            self.extrusion_var.set("0")
            self.damage_var.set("0")
            self.row_x.label_widget.config(text="Texture X")
            self.row_y.label_widget.config(text="Texture Y")
            self._show_rows((self.row_name, self.row_x, self.row_y, self.row_depth))
            self.detail_note_var.set("Anchor fields include depth offset.")
        elif self.selected_kind in ("hit_box", "attack_box") and 0 <= self.selected_index < len(self._current_boxes(self.selected_kind)):
            box = self._current_boxes(self.selected_kind)[self.selected_index]
            corners = box.setdefault("corners", [])
            while len(corners) < 4:
                corners.append({"texture_x": 0, "texture_y": 0})
            corner = corners[self.selected_corner]
            self.detail_title_var.set("Attack Box Details" if self.selected_kind == "attack_box" else "Hit Box Details")
            self.name_var.set(box.get("name", ""))
            self.extrusion_var.set(str(box.get("extrusion_amount", 0)))
            self.x_var.set(str(corner.get("texture_x", 0)))
            self.y_var.set(str(corner.get("texture_y", 0)))
            self.depth_var.set("0")
            self.damage_var.set(str(box.get("damage_amount", 0)))
            self.row_x.label_widget.config(text=f"Corner {self.selected_corner + 1} X")
            self.row_y.label_widget.config(text=f"Corner {self.selected_corner + 1} Y")
            rows = [self.row_name, self.row_extrusion, self.row_x, self.row_y]
            if self.selected_kind == "attack_box":
                rows.append(self.row_damage)
            self._show_rows(rows)
            self.detail_note_var.set("Corner values edit only the selected corner.")
        else:
            self.detail_title_var.set("Selection Details")
            self.name_var.set("")
            self.x_var.set("0")
            self.y_var.set("0")
            self.depth_var.set("0")
            self.extrusion_var.set("0")
            self.damage_var.set("0")
            self._show_rows(())
            self.detail_note_var.set("Select an anchor or a box corner.")
        self.loading = prev_loading

    def _refresh_frame_ui(self):
        self.loading = True
        self.current_frame = max(0, min(self.current_frame, self._frame_count() - 1))
        self.frame_label_var.set(f"Frame {self.current_frame + 1} / {self._frame_count()}")
        self._refresh_entity_list()
        self._load_form()
        self._load_frame_image()
        self._render_preview()
        self.loading = False

    def _save_manifest(self):
        self.payload["anchor_points"] = self.anchor_frames
        self.payload["hit_boxes"] = self.hit_box_frames
        self.payload["attack_boxes"] = self.attack_box_frames
        self.payload["number_of_frames"] = self._frame_count()
        self.payload.pop("hit_geometry", None)
        self.payload.pop("attack_geometry", None)
        with self.manifest_path.open("w", encoding="utf-8") as fh:
            json.dump(self.manifest, fh, indent=2)
            fh.write("\n")

    def _find_nearest(self, canvas_x, canvas_y):
        best = None
        best_dist_sq = SELECT_RADIUS_PX * SELECT_RADIUS_PX
        for idx, anchor in enumerate(self.anchor_frames[self.current_frame]):
            ax, ay = self._texture_to_canvas(anchor["texture_x"], anchor["texture_y"])
            dist_sq = (ax - canvas_x) * (ax - canvas_x) + (ay - canvas_y) * (ay - canvas_y)
            if dist_sq <= best_dist_sq:
                best_dist_sq = dist_sq
                best = ("anchor", idx, 0)
        for kind in ("hit_box", "attack_box"):
            for box_index, box in enumerate(self._current_boxes(kind)):
                for corner_index, corner in enumerate(box.get("corners", [])[:4]):
                    cx, cy = self._texture_to_canvas(corner.get("texture_x", 0), corner.get("texture_y", 0))
                    dist_sq = (cx - canvas_x) * (cx - canvas_x) + (cy - canvas_y) * (cy - canvas_y)
                    if dist_sq <= best_dist_sq:
                        best_dist_sq = dist_sq
                        best = (kind, box_index, corner_index)
        return best

    def _move_selected_to(self, canvas_x, canvas_y):
        tx, ty = self._canvas_to_texture(canvas_x, canvas_y, clamp=True)
        px = max(0, int(round(tx)))
        py = max(0, int(round(ty)))
        if self.selected_kind == "anchor" and 0 <= self.selected_index < len(self.anchor_frames[self.current_frame]):
            anchor = self.anchor_frames[self.current_frame][self.selected_index]
            anchor["texture_x"] = px
            anchor["texture_y"] = py
        elif self.selected_kind in ("hit_box", "attack_box"):
            boxes = self._current_boxes(self.selected_kind)
            if 0 <= self.selected_index < len(boxes):
                corners = boxes[self.selected_index].setdefault("corners", [])
                while len(corners) < 4:
                    corners.append({"texture_x": 0, "texture_y": 0})
                corners[self.selected_corner]["texture_x"] = px
                corners[self.selected_corner]["texture_y"] = py
        else:
            return
        self._dirty_drag = True
        prev_loading = self.loading
        self.loading = True
        self.x_var.set(str(px))
        self.y_var.set(str(py))
        self.loading = prev_loading
        self._render_preview()

    def _on_preview_click(self, event):
        found = self._find_nearest(event.x, event.y)
        if found:
            self._set_selection(found[0], found[1], found[2])
        else:
            self._set_selection()
        self._refresh_entity_list()
        self._load_form()
        self._render_preview()
        self._dragging = self.selected_kind is not None

    def _on_preview_drag(self, event):
        if not self._dragging:
            return
        self._move_selected_to(event.x, event.y)
        self._save_manifest()

    def _on_preview_release(self, _event=None):
        if self._dirty_drag:
            self._save_manifest()
            self._dirty_drag = False
        self._dragging = False

    def _on_preview_configure(self, _event=None):
        self._render_preview()

    def _on_entity_select(self, _event=None):
        if self.loading:
            return
        selection = self.entity_list.curselection()
        if not selection:
            self._set_selection()
            self._load_form()
            self._render_preview()
            return
        kind, index = self._entity_map[selection[0]]
        corner = self.selected_corner if self.selected_kind == kind and self.selected_index == index else 0
        self._set_selection(kind, index, corner)
        self._load_form()
        self._render_preview()

    def _unique_anchor_name(self, desired, skip_index=None):
        existing = {a["name"] for i, a in enumerate(self.anchor_frames[self.current_frame]) if i != skip_index}
        return _normalize_unique_name(desired, existing, "anchor")

    def _unique_box_name(self, kind, desired, skip_index=None):
        existing = {b["name"] for i, b in enumerate(self._current_boxes(kind)) if i != skip_index}
        default_name = "attack_box" if kind == "attack_box" else "hit_box"
        return _normalize_unique_name(desired, existing, default_name)

    def _on_value_change(self):
        if self.loading:
            return
        if self.selected_kind == "anchor" and 0 <= self.selected_index < len(self.anchor_frames[self.current_frame]):
            anchor = self.anchor_frames[self.current_frame][self.selected_index]
            desired_name = (self.name_var.get() or "").strip() or f"anchor_{self.selected_index + 1}"
            unique_name = self._unique_anchor_name(desired_name, skip_index=self.selected_index)
            if unique_name != self.name_var.get():
                prev_loading = self.loading
                self.loading = True
                self.name_var.set(unique_name)
                self.loading = prev_loading
            anchor["name"] = unique_name
            anchor["texture_x"] = max(0, _read_int(self.x_var.get(), anchor.get("texture_x", 0)))
            anchor["texture_y"] = max(0, _read_int(self.y_var.get(), anchor.get("texture_y", 0)))
            anchor["depth_offset"] = _read_int(self.depth_var.get(), anchor.get("depth_offset", 0))
        elif self.selected_kind in ("hit_box", "attack_box"):
            boxes = self._current_boxes(self.selected_kind)
            if 0 <= self.selected_index < len(boxes):
                box = boxes[self.selected_index]
                default_name = f"{'attack' if self.selected_kind == 'attack_box' else 'hit'}_box_{self.selected_index + 1}"
                desired_name = (self.name_var.get() or "").strip() or default_name
                unique_name = self._unique_box_name(self.selected_kind, desired_name, skip_index=self.selected_index)
                if unique_name != self.name_var.get():
                    prev_loading = self.loading
                    self.loading = True
                    self.name_var.set(unique_name)
                    self.loading = prev_loading
                box["name"] = unique_name
                box["extrusion_amount"] = max(0, _read_int(self.extrusion_var.get(), box.get("extrusion_amount", 0)))
                corners = box.setdefault("corners", [])
                while len(corners) < 4:
                    corners.append({"texture_x": 0, "texture_y": 0})
                corner = corners[self.selected_corner]
                corner["texture_x"] = max(0, _read_int(self.x_var.get(), corner.get("texture_x", 0)))
                corner["texture_y"] = max(0, _read_int(self.y_var.get(), corner.get("texture_y", 0)))
                if self.selected_kind == "attack_box":
                    box["damage_amount"] = _read_int(self.damage_var.get(), box.get("damage_amount", 0))
        else:
            return
        self._refresh_entity_list()
        self._load_form()
        self._render_preview()
        self._save_manifest()

    def _add_anchor(self):
        frame = self.anchor_frames[self.current_frame]
        frame.append({
            "name": self._unique_anchor_name(f"anchor_{len(frame) + 1}"),
            "texture_x": 0,
            "texture_y": 0,
            "depth_offset": 0,
        })
        self._set_selection("anchor", len(frame) - 1, 0)
        self._refresh_frame_ui()
        self._save_manifest()

    def _new_box(self, kind):
        default = f"{'attack' if kind == 'attack_box' else 'hit'}_box_{len(self._current_boxes(kind)) + 1}"
        box = {
            "name": self._unique_box_name(kind, default),
            "extrusion_amount": 0,
            "corners": [
                {"texture_x": 0, "texture_y": 0},
                {"texture_x": 16, "texture_y": 0},
                {"texture_x": 16, "texture_y": 16},
                {"texture_x": 0, "texture_y": 16},
            ],
        }
        if kind == "attack_box":
            box["damage_amount"] = 0
        return box

    def _add_hit_box(self):
        frame = self.hit_box_frames[self.current_frame]
        frame.append(self._new_box("hit_box"))
        self._set_selection("hit_box", len(frame) - 1, 0)
        self._refresh_frame_ui()
        self._save_manifest()

    def _add_attack_box(self):
        frame = self.attack_box_frames[self.current_frame]
        frame.append(self._new_box("attack_box"))
        self._set_selection("attack_box", len(frame) - 1, 0)
        self._refresh_frame_ui()
        self._save_manifest()

    def _delete_selected(self):
        if self.selected_kind == "anchor" and 0 <= self.selected_index < len(self.anchor_frames[self.current_frame]):
            self.anchor_frames[self.current_frame].pop(self.selected_index)
        elif self.selected_kind in ("hit_box", "attack_box"):
            boxes = self._current_boxes(self.selected_kind)
            if 0 <= self.selected_index < len(boxes):
                boxes.pop(self.selected_index)
            else:
                return
        else:
            return
        self._set_selection()
        self._refresh_frame_ui()
        self._save_manifest()

    def _randomize_all_anchors(self):
        for anim_id, payload in self.animations.items():
            if not isinstance(payload, dict):
                continue
            frame_count = _frame_count_with_disk(payload, self.manifest_root, self.asset, self.asset_key)
            anchors = _normalize_anchor_points(payload, frame_count_override=frame_count)
            for frame in anchors:
                for anchor in frame:
                    angle = random.uniform(0.0, 2.0 * math.pi)
                    anchor["texture_x"] = max(0, anchor["texture_x"] + int(round(12.0 * math.cos(angle))))
                    anchor["texture_y"] = max(0, anchor["texture_y"] + int(round(12.0 * math.sin(angle))))
            payload["anchor_points"] = anchors
            payload["number_of_frames"] = len(anchors)
            payload.pop("hit_geometry", None)
            payload.pop("attack_geometry", None)
            if anim_id == self.animation_id:
                self.anchor_frames = anchors
                self._ensure_frame_count(frame_count)
        self._save_manifest()
        self._refresh_frame_ui()

    def _frame_snapshot(self, idx):
        return {
            "anchor_points": copy.deepcopy(self.anchor_frames[idx]),
            "hit_boxes": copy.deepcopy(self.hit_box_frames[idx]),
            "attack_boxes": copy.deepcopy(self.attack_box_frames[idx]),
        }

    def _apply_snapshot_to_frame(self, idx, snapshot):
        self.anchor_frames[idx] = copy.deepcopy(snapshot["anchor_points"])
        self.hit_box_frames[idx] = copy.deepcopy(snapshot["hit_boxes"])
        self.attack_box_frames[idx] = copy.deepcopy(snapshot["attack_boxes"])

    def _prev_frame(self):
        if self.current_frame > 0:
            self.current_frame -= 1
            self._set_selection()
            self._refresh_frame_ui()

    def _next_frame(self):
        if self.current_frame + 1 < self._frame_count():
            self.current_frame += 1
            self._set_selection()
            self._refresh_frame_ui()

    def _apply_to_next(self):
        if self.current_frame + 1 >= self._frame_count():
            return
        snapshot = self._frame_snapshot(self.current_frame)
        self._apply_snapshot_to_frame(self.current_frame + 1, snapshot)
        self._save_manifest()

    def _apply_to_animation(self):
        snapshot = self._frame_snapshot(self.current_frame)
        for idx in range(self._frame_count()):
            self._apply_snapshot_to_frame(idx, snapshot)
        self._refresh_frame_ui()
        self._save_manifest()

    def _apply_to_all_animations(self):
        snapshot = self._frame_snapshot(self.current_frame)
        for anim_id, payload in self.animations.items():
            if not isinstance(payload, dict):
                continue
            frame_count = _frame_count_with_disk(payload, self.manifest_root, self.asset, self.asset_key)
            anchors = _normalize_anchor_points(payload, frame_count_override=frame_count)
            hit_boxes = _normalize_boxes(payload, "hit_boxes", frame_count_override=frame_count, attack=False)
            attack_boxes = _normalize_boxes(payload, "attack_boxes", frame_count_override=frame_count, attack=True)
            for idx in range(frame_count):
                anchors[idx] = copy.deepcopy(snapshot["anchor_points"])
                hit_boxes[idx] = copy.deepcopy(snapshot["hit_boxes"])
                attack_boxes[idx] = copy.deepcopy(snapshot["attack_boxes"])
            payload["anchor_points"] = anchors
            payload["hit_boxes"] = hit_boxes
            payload["attack_boxes"] = attack_boxes
            payload["number_of_frames"] = frame_count
            payload.pop("hit_geometry", None)
            payload.pop("attack_geometry", None)
            if anim_id == self.animation_id:
                self.anchor_frames = anchors
                self.hit_box_frames = hit_boxes
                self.attack_box_frames = attack_boxes
        self._refresh_frame_ui()
        self._save_manifest()

    def _on_close(self):
        try:
            self._save_manifest()
        except Exception as exc:
            messagebox.showerror("Save failed", str(exc))
            return
        self.root.destroy()

class AssetSelectionApp:
    def __init__(self, root, manifest_path: Path, default_asset_id=None):
        self.root = root
        self.manifest_path = manifest_path
        self.default_asset_id = default_asset_id
        self._suppress_animation_open = False
        self._ignore_asset_select_event = False

        self.manifest = load_manifest(self.manifest_path)
        assets = self.manifest.setdefault("assets", {})
        if not isinstance(assets, dict):
            raise RuntimeError("manifest.json assets must be an object")
        self.assets = assets
        self.asset_keys = []
        self.animation_keys = []
        self.selected_asset_key = None

        self._build_ui()
        self._populate_asset_list()
        self._refresh_animation_list()

        if self.default_asset_id:
            asset_key = resolve_asset_key(self.assets, self.default_asset_id)
            if asset_key:
                self.root.after(0, lambda key=asset_key: self._select_asset_key(key))

    def _build_ui(self):
        self.root.configure(bg=PALETTE["bg"])
        self.root.title("Anchor Point Editor - Select Asset")
        self.root.geometry("900x540")

        header = tk.Frame(self.root, bg=PALETTE["nav_bg"])
        header.pack(fill="x", padx=8, pady=(8, 4))
        tk.Label(
            header,
            text="Choose an asset to edit anchors, hit boxes, and attack boxes",
            font=FONTS["title"],
            fg=PALETTE["accent"],
            bg=PALETTE["nav_bg"],
        ).pack(anchor="w", padx=12, pady=(8, 0))
        tk.Label(
            header,
            text=f"Manifest: {self.manifest_path}",
            font=FONTS["body"],
            fg=PALETTE["muted"],
            bg=PALETTE["nav_bg"],
        ).pack(anchor="w", padx=12, pady=(0, 8))

        body = tk.Frame(self.root, bg=PALETTE["bg"])
        body.pack(fill="both", expand=True, padx=8, pady=(0, 8))
        main = tk.Frame(body, bg=PALETTE["panel"])
        main.pack(fill="both", expand=True)

        asset_frame = tk.Frame(main, bg=PALETTE["panel"])
        asset_frame.pack(side="left", fill="both", expand=True, padx=(12, 6), pady=12)
        tk.Label(asset_frame, text="Assets", font=FONTS["section"], fg=PALETTE["text"], bg=PALETTE["panel"]).pack(anchor="w", padx=8)
        asset_list_container = tk.Frame(asset_frame, bg=PALETTE["panel"])
        asset_list_container.pack(fill="both", expand=True, pady=(6, 0))
        self.asset_list = tk.Listbox(
            asset_list_container,
            exportselection=False,
            bg=PALETTE["entry_bg"],
            fg=PALETTE["text"],
            selectbackground=PALETTE["accent"],
            selectforeground=PALETTE["text"],
            bd=0,
            highlightthickness=0,
            font=FONTS["body"],
        )
        asset_scroll = tk.Scrollbar(asset_list_container, command=self.asset_list.yview, bg=PALETTE["panel"], troughcolor=PALETTE["nav_bg"])
        self.asset_list.config(yscrollcommand=asset_scroll.set)
        self.asset_list.pack(side="left", fill="both", expand=True)
        asset_scroll.pack(side="right", fill="y")
        self.asset_list.bind("<<ListboxSelect>>", self._on_asset_select)

        animation_frame = tk.Frame(main, bg=PALETTE["panel"])
        animation_frame.pack(side="left", fill="both", expand=True, padx=(6, 12), pady=12)
        tk.Label(animation_frame, text="Animations", font=FONTS["section"], fg=PALETTE["text"], bg=PALETTE["panel"]).pack(anchor="w", padx=8)
        self.selected_asset_label = tk.Label(
            animation_frame,
            text="Selected asset: none",
            font=FONTS["body"],
            fg=PALETTE["muted"],
            bg=PALETTE["panel"],
        )
        self.selected_asset_label.pack(anchor="w", padx=8, pady=(0, 4))
        animation_list_container = tk.Frame(animation_frame, bg=PALETTE["panel"])
        animation_list_container.pack(fill="both", expand=True, pady=(4, 0))
        self.animation_list = tk.Listbox(
            animation_list_container,
            exportselection=False,
            bg=PALETTE["entry_bg"],
            fg=PALETTE["text"],
            selectbackground=PALETTE["accent"],
            selectforeground=PALETTE["text"],
            bd=0,
            highlightthickness=0,
            font=FONTS["body"],
        )
        animation_scroll = tk.Scrollbar(animation_list_container, command=self.animation_list.yview, bg=PALETTE["panel"], troughcolor=PALETTE["nav_bg"])
        self.animation_list.config(yscrollcommand=animation_scroll.set)
        self.animation_list.pack(side="left", fill="both", expand=True)
        animation_scroll.pack(side="right", fill="y")
        self.animation_list.bind("<ButtonRelease-1>", self._on_animation_select)
        self.animation_list.bind("<Return>", self._on_animation_select)
        tk.Label(
            animation_frame,
            text="Click an animation to open the editor.",
            font=FONTS["body"],
            fg=PALETTE["muted"],
            bg=PALETTE["panel"],
        ).pack(anchor="w", padx=8, pady=(6, 0))

        footer = tk.Frame(self.root, bg=PALETTE["bg"])
        footer.pack(fill="x", padx=8, pady=(0, 8))
        tk.Label(
            footer,
            text="Hint: double-click or press Enter on an animation to open the editor.",
            font=FONTS["body"],
            fg=PALETTE["muted"],
            bg=PALETTE["bg"],
        ).pack(anchor="w")

    def _populate_asset_list(self):
        self.asset_list.config(state="normal")
        self.asset_list.delete(0, tk.END)
        self.asset_keys = []
        for key, asset in self.assets.items():
            display = str(key)
            if isinstance(asset, dict):
                asset_name = asset.get("asset_name")
                if asset_name:
                    display = f"{asset_name} ({key})"
            self.asset_list.insert(tk.END, display)
            self.asset_keys.append(key)
        if not self.asset_keys:
            self.asset_list.insert(tk.END, "No assets found in manifest")
            self.asset_list.config(state="disabled")

    def _select_asset_key(self, asset_key):
        if asset_key not in self.asset_keys:
            return
        idx = self.asset_keys.index(asset_key)
        self._ignore_asset_select_event = True
        self.asset_list.selection_clear(0, tk.END)
        self.asset_list.selection_set(idx)
        self.asset_list.see(idx)
        self._ignore_asset_select_event = False
        self._on_asset_select()

    def _on_asset_select(self, _event=None):
        if self._ignore_asset_select_event:
            return
        selection = self.asset_list.curselection()
        if not selection:
            self.selected_asset_key = None
        else:
            self.selected_asset_key = self.asset_keys[selection[0]]
        self._refresh_animation_list()

    def _refresh_animation_list(self):
        self._suppress_animation_open = True
        self.animation_list.config(state="normal")
        self.animation_list.delete(0, tk.END)
        self.animation_list.selection_clear(0, tk.END)
        self.animation_keys = []

        display_asset = "none"
        if self.selected_asset_key:
            asset_value = self.assets.get(self.selected_asset_key)
            if isinstance(asset_value, dict):
                display_asset = asset_value.get("asset_name") or self.selected_asset_key
            else:
                display_asset = self.selected_asset_key
        self.selected_asset_label.config(text=f"Selected asset: {display_asset}")

        if not self.selected_asset_key:
            self.animation_list.insert(tk.END, "Select an asset to load animations")
            self.animation_list.config(state="disabled")
            self._suppress_animation_open = False
            return

        asset = self.assets.get(self.selected_asset_key, {})
        animations = asset.get("animations")
        if isinstance(animations, dict):
            for animation_id in sorted(animations):
                self.animation_keys.append(animation_id)
                self.animation_list.insert(tk.END, animation_id)

        if not self.animation_keys:
            self.animation_list.insert(tk.END, "No animations for this asset")
            self.animation_list.config(state="disabled")

        self._suppress_animation_open = False

    def _on_animation_select(self, _event=None):
        if self._suppress_animation_open:
            return
        selection = self.animation_list.curselection()
        if not selection:
            return
        animation_id = self.animation_keys[selection[0]]
        self._open_anchor_editor(animation_id)
        self.animation_list.selection_clear(0, tk.END)

    def _open_anchor_editor(self, animation_id):
        if not self.selected_asset_key:
            return
        editor_window = tk.Toplevel(self.root)
        editor_window.transient(self.root)
        try:
            AnchorEditorApp(editor_window, self.manifest_path, self.selected_asset_key, animation_id)
        except Exception as exc:
            editor_window.destroy()
            messagebox.showerror("Anchor Editor", str(exc), parent=self.root)
            return
        editor_window.focus_set()


def main():
    parser = argparse.ArgumentParser(description="Anchor/hit-box/attack-box editor for manifest animations")
    parser.add_argument("--manifest", default=str(DEFAULT_MANIFEST_PATH))
    parser.add_argument("--asset")
    parser.add_argument("--animation")
    args = parser.parse_args()

    manifest_path = Path(args.manifest).expanduser()
    root = tk.Tk()
    try:
        if args.asset and args.animation:
            AnchorEditorApp(root, manifest_path, args.asset, args.animation)
        else:
            AssetSelectionApp(root, manifest_path, args.asset)
    except Exception as exc:
        root.withdraw()
        messagebox.showerror("Anchor Editor", str(exc))
        raise SystemExit(1)
    root.mainloop()


if __name__ == "__main__":
    main()
