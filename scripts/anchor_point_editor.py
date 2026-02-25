#!/usr/bin/env python3
import argparse
import copy
import json
from pathlib import Path
import tkinter as tk
from tkinter import messagebox

DEFAULT_MANIFEST_PATH = Path(__file__).resolve().parents[1] / "manifest.json"


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


def _infer_frame_count(payload):
    if not isinstance(payload, dict):
        return 1
    counts = [1]
    for key in ("number_of_frames",):
        counts.append(max(0, _read_int(payload.get(key, 0), 0)))
    for key in ("movement", "frames", "anchor_points"):
        value = payload.get(key)
        if isinstance(value, list):
            counts.append(len(value))
    return max(1, max(counts))


def _normalize_anchor(anchor):
    if not isinstance(anchor, dict):
        return None
    name = str(anchor.get("name", "")).strip()
    if not name:
        return None
    return {
        "name": name,
        "texture_x": max(0, _read_int(anchor.get("texture_x", 0), 0)),
        "texture_y": max(0, _read_int(anchor.get("texture_y", 0), 0)),
        "in_front": bool(anchor.get("in_front", True)),
    }


def _normalize_anchor_points(payload):
    frame_count = _infer_frame_count(payload)
    source = payload.get("anchor_points") if isinstance(payload, dict) else None
    out = [[] for _ in range(frame_count)]
    if isinstance(source, list):
        for idx in range(min(frame_count, len(source))):
            frame = source[idx]
            if not isinstance(frame, list):
                continue
            dedupe = set()
            for anchor in frame:
                normalized = _normalize_anchor(anchor)
                if not normalized or normalized["name"] in dedupe:
                    continue
                dedupe.add(normalized["name"])
                out[idx].append(normalized)
    return out


class AnchorEditorApp:
    def __init__(self, root, manifest_path: Path, asset_id: str, animation_id: str):
        self.root = root
        self.manifest_path = manifest_path
        self.asset_id = asset_id
        self.animation_id = animation_id
        self.loading = False
        self.current_frame = 0
        self.current_anchor = -1

        self.manifest = load_manifest(self.manifest_path)
        self.asset_key, self.asset = self._resolve_asset()
        self.animations = self.asset.setdefault("animations", {})
        self.payload = self.animations.setdefault(self.animation_id, {})
        if not isinstance(self.payload, dict):
            self.payload = {}
            self.animations[self.animation_id] = self.payload
        self.frames = _normalize_anchor_points(self.payload)
        self.payload["anchor_points"] = self.frames

        self.root.title(f"Anchor Point Editor - {self.asset_key}:{self.animation_id}")
        self.root.geometry("860x520")
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

        self.frame_label_var = tk.StringVar()
        self.anchor_name_var = tk.StringVar()
        self.anchor_x_var = tk.StringVar()
        self.anchor_y_var = tk.StringVar()
        self.anchor_in_front_var = tk.BooleanVar(value=True)

        self._build_ui()
        self._wire_traces()
        self._refresh_frame_ui()
        self._save_manifest()

    def _build_ui(self):
        nav = tk.Frame(self.root)
        nav.pack(fill="x", padx=8, pady=6)
        tk.Button(nav, text="<< Prev", command=self._prev_frame).pack(side="left")
        tk.Button(nav, text="Next >>", command=self._next_frame).pack(side="left", padx=(6, 12))
        tk.Label(nav, textvariable=self.frame_label_var, font=("Arial", 11, "bold")).pack(side="left")
        tk.Button(nav, text="Apply Frame -> Next", command=self._apply_to_next).pack(side="right")
        tk.Button(nav, text="Apply Frame -> This Animation", command=self._apply_to_animation).pack(side="right", padx=6)
        tk.Button(nav, text="Apply Frame -> All Animations", command=self._apply_to_all_animations).pack(side="right", padx=6)

        body = tk.Frame(self.root)
        body.pack(fill="both", expand=True, padx=8, pady=6)

        left = tk.Frame(body)
        left.pack(side="left", fill="both", expand=True)

        tk.Label(left, text="Anchors (current frame)", font=("Arial", 10, "bold")).pack(anchor="w")
        self.anchor_list = tk.Listbox(left, exportselection=False)
        self.anchor_list.pack(fill="both", expand=True, pady=(4, 8))
        self.anchor_list.bind("<<ListboxSelect>>", self._on_anchor_select)

        list_buttons = tk.Frame(left)
        list_buttons.pack(fill="x")
        tk.Button(list_buttons, text="Add Anchor", command=self._add_anchor).pack(side="left")
        tk.Button(list_buttons, text="Delete Anchor", command=self._delete_anchor).pack(side="left", padx=6)

        right = tk.Frame(body)
        right.pack(side="left", fill="y", padx=(12, 0))

        def row(label, var):
            r = tk.Frame(right)
            r.pack(fill="x", pady=3)
            tk.Label(r, text=label, width=10, anchor="w").pack(side="left")
            entry = tk.Entry(r, textvariable=var, width=24)
            entry.pack(side="left")
            return entry

        row("Name", self.anchor_name_var)
        row("Texture X", self.anchor_x_var)
        row("Texture Y", self.anchor_y_var)
        tk.Checkbutton(right, text="In Front", variable=self.anchor_in_front_var, command=self._on_value_change).pack(anchor="w", pady=(8, 0))

    def _wire_traces(self):
        self.anchor_name_var.trace_add("write", lambda *_: self._on_value_change())
        self.anchor_x_var.trace_add("write", lambda *_: self._on_value_change())
        self.anchor_y_var.trace_add("write", lambda *_: self._on_value_change())

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

    def _save_manifest(self):
        self.payload["anchor_points"] = self.frames
        with self.manifest_path.open("w", encoding="utf-8") as fh:
            json.dump(self.manifest, fh, indent=2)
            fh.write("\n")

    def _frame_count(self):
        return len(self.frames)

    def _refresh_frame_ui(self):
        self.loading = True
        self.current_frame = max(0, min(self.current_frame, self._frame_count() - 1))
        self.frame_label_var.set(f"Frame {self.current_frame + 1} / {self._frame_count()}")

        self.anchor_list.delete(0, tk.END)
        anchors = self.frames[self.current_frame]
        for idx, anchor in enumerate(anchors):
            self.anchor_list.insert(tk.END, f"{idx + 1}. {anchor['name']}")

        if anchors:
            self.current_anchor = max(0, min(self.current_anchor, len(anchors) - 1))
            self.anchor_list.selection_set(self.current_anchor)
            self._load_anchor_form()
        else:
            self.current_anchor = -1
            self.anchor_name_var.set("")
            self.anchor_x_var.set("0")
            self.anchor_y_var.set("0")
            self.anchor_in_front_var.set(True)
        self.loading = False

    def _load_anchor_form(self):
        if self.current_anchor < 0:
            return
        anchor = self.frames[self.current_frame][self.current_anchor]
        self.anchor_name_var.set(anchor["name"])
        self.anchor_x_var.set(str(anchor["texture_x"]))
        self.anchor_y_var.set(str(anchor["texture_y"]))
        self.anchor_in_front_var.set(bool(anchor["in_front"]))

    def _on_anchor_select(self, _event=None):
        if self.loading:
            return
        sel = self.anchor_list.curselection()
        self.current_anchor = sel[0] if sel else -1
        if self.current_anchor >= 0:
            self.loading = True
            self._load_anchor_form()
            self.loading = False

    def _on_value_change(self):
        if self.loading or self.current_anchor < 0:
            return
        frame = self.frames[self.current_frame]
        if self.current_anchor >= len(frame):
            return
        anchor = frame[self.current_anchor]
        anchor["name"] = self.anchor_name_var.get().strip() or f"anchor_{self.current_anchor + 1}"
        anchor["texture_x"] = max(0, _read_int(self.anchor_x_var.get(), anchor.get("texture_x", 0)))
        anchor["texture_y"] = max(0, _read_int(self.anchor_y_var.get(), anchor.get("texture_y", 0)))
        anchor["in_front"] = bool(self.anchor_in_front_var.get())
        self.anchor_list.delete(self.current_anchor)
        self.anchor_list.insert(self.current_anchor, f"{self.current_anchor + 1}. {anchor['name']}")
        self.anchor_list.selection_set(self.current_anchor)
        self._save_manifest()

    def _add_anchor(self):
        frame = self.frames[self.current_frame]
        new_anchor = {
            "name": f"anchor_{len(frame) + 1}",
            "texture_x": 0,
            "texture_y": 0,
            "in_front": True,
        }
        frame.append(new_anchor)
        self.current_anchor = len(frame) - 1
        self._refresh_frame_ui()
        self._save_manifest()

    def _delete_anchor(self):
        if self.current_anchor < 0:
            return
        frame = self.frames[self.current_frame]
        if self.current_anchor >= len(frame):
            return
        frame.pop(self.current_anchor)
        if self.current_anchor >= len(frame):
            self.current_anchor = len(frame) - 1
        self._refresh_frame_ui()
        self._save_manifest()

    def _prev_frame(self):
        if self.current_frame > 0:
            self.current_frame -= 1
            self.current_anchor = -1
            self._refresh_frame_ui()

    def _next_frame(self):
        if self.current_frame + 1 < self._frame_count():
            self.current_frame += 1
            self.current_anchor = -1
            self._refresh_frame_ui()

    def _apply_to_next(self):
        if self.current_frame + 1 >= self._frame_count():
            return
        self.frames[self.current_frame + 1] = copy.deepcopy(self.frames[self.current_frame])
        self._save_manifest()

    def _apply_to_animation(self):
        source = copy.deepcopy(self.frames[self.current_frame])
        for idx in range(self._frame_count()):
            self.frames[idx] = copy.deepcopy(source)
        self._refresh_frame_ui()
        self._save_manifest()

    def _apply_to_all_animations(self):
        source = copy.deepcopy(self.frames[self.current_frame])
        for anim_id, payload in self.animations.items():
            if not isinstance(payload, dict):
                continue
            frame_count = _infer_frame_count(payload)
            anchors = _normalize_anchor_points(payload)
            if len(anchors) < frame_count:
                anchors.extend([[] for _ in range(frame_count - len(anchors))])
            if self.current_frame >= len(anchors):
                anchors.extend([[] for _ in range(self.current_frame - len(anchors) + 1)])
            anchors[self.current_frame] = copy.deepcopy(source)
            payload["anchor_points"] = anchors
            if anim_id == self.animation_id:
                self.frames = anchors
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
        self.root.title("Anchor Point Editor - Select Asset")
        self.root.geometry("720x460")

        header = tk.Frame(self.root)
        header.pack(fill="x", padx=8, pady=(8, 4))
        tk.Label(header, text="Choose an asset to view its animations", font=("Arial", 11, "bold")).pack(anchor="w")
        tk.Label(header, text=f"Manifest: {self.manifest_path}", fg="gray").pack(anchor="w")

        body = tk.Frame(self.root)
        body.pack(fill="both", expand=True, padx=8, pady=6)

        asset_frame = tk.Frame(body)
        asset_frame.pack(side="left", fill="both", expand=True, padx=(0, 6))
        tk.Label(asset_frame, text="Assets", font=("Arial", 10, "bold")).pack(anchor="w")
        asset_list_container = tk.Frame(asset_frame)
        asset_list_container.pack(fill="both", expand=True, pady=(4, 0))
        self.asset_list = tk.Listbox(asset_list_container, exportselection=False)
        asset_scroll = tk.Scrollbar(asset_list_container, command=self.asset_list.yview)
        self.asset_list.config(yscrollcommand=asset_scroll.set)
        self.asset_list.pack(side="left", fill="both", expand=True)
        asset_scroll.pack(side="right", fill="y")
        self.asset_list.bind("<<ListboxSelect>>", self._on_asset_select)

        animation_frame = tk.Frame(body)
        animation_frame.pack(side="left", fill="both", expand=True)
        tk.Label(animation_frame, text="Animations", font=("Arial", 10, "bold")).pack(anchor="w")
        animation_list_container = tk.Frame(animation_frame)
        animation_list_container.pack(fill="both", expand=True, pady=(4, 0))
        self.animation_list = tk.Listbox(animation_list_container, exportselection=False)
        animation_scroll = tk.Scrollbar(animation_list_container, command=self.animation_list.yview)
        self.animation_list.config(yscrollcommand=animation_scroll.set)
        self.animation_list.pack(side="left", fill="both", expand=True)
        animation_scroll.pack(side="right", fill="y")
        self.animation_list.bind("<ButtonRelease-1>", self._on_animation_select)
        self.animation_list.bind("<Return>", self._on_animation_select)
        tk.Label(animation_frame, text="Click an animation to open the editor", fg="gray").pack(anchor="w", pady=(6, 0))

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
    parser = argparse.ArgumentParser(description="Anchor point editor for manifest animations")
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
