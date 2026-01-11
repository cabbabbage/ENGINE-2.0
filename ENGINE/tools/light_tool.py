#!/usr/bin/env python3
"""Light cache generation tool driven by manifest content (no rebuild queue)."""

import json
import logging
import math
import multiprocessing
import os
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple

from PIL import Image
import numpy as np

from gpu_status import print_gpu_status

LIGHT_CACHE_VERSION = 3


def _configure_logger() -> logging.Logger:
    logger = logging.getLogger(__name__)
    if not logger.handlers:
        handler = logging.StreamHandler(sys.stderr)
        handler.setFormatter(logging.Formatter("%(levelname)s: %(message)s"))
        logger.addHandler(handler)
        logger.setLevel(logging.ERROR)
    return logger


LOGGER = _configure_logger()

def clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


def _find_repo_root(start_dir: Path) -> Path:
    current = start_dir
    while True:
        candidate = current / "manifest.json"
        if candidate.exists():
            return current
        parent = current.parent
        if parent == current:
            raise FileNotFoundError(
                f"manifest.json not found while searching upward from {start_dir}"
            )
        current = parent


def light_signature(light: "LightDefinition") -> str:
    return (
        f"{light.radius}|"
        f"{light.fall_off}|"
        f"{light.flare}|"
        f"{light.intensity}|"
        f"{light.flicker_speed}|"
        f"{light.flicker_smoothness}"
    )


def compute_fade_exponent(fall_off: int) -> float:
    falloff_norm = clamp(float(fall_off) / 100.0, 0.0, 1.0)
    return 0.6 + 3.4 * falloff_norm


def lround(value: float) -> int:
    # Match std::lround semantics (half away from zero)
    if value >= 0:
        return int(math.floor(value + 0.5))
    return int(math.ceil(value - 0.5))


def _compute_alpha_payload(args: Tuple[int, int]) -> Tuple[Tuple[int, int], bytes, int]:
    radius, fall_off = args
    radius = max(1, int(radius))
    diameter = max(1, radius * 2)
    center = float(diameter) * 0.5
    fade_exponent = compute_fade_exponent(int(fall_off))

    grid = (np.arange(diameter, dtype=np.float32) + 0.5) - center
    dx = grid.reshape(1, -1)
    dy = grid.reshape(-1, 1)
    dist = np.hypot(dx, dy)
    ratio = np.clip(dist / float(radius), 0.0, 1.0)
    base = np.maximum(0.0, 1.0 - ratio)
    alpha = np.power(base, fade_exponent) * 255.0
    alpha_u8 = np.clip(alpha, 0.0, 255.0).astype(np.uint8)
    return (radius, int(fall_off)), alpha_u8.tobytes(), diameter


@dataclass
class LightDefinition:
    intensity: int = 255
    radius: int = 64
    fall_off: int = 50
    flare: int = 0
    flicker_speed: int = 0
    flicker_smoothness: int = 100
    offset_x: int = 0
    offset_z: int = 0
    color: Tuple[int, int, int] = (255, 255, 255)
    in_front: bool = False
    behind: bool = False
    render_to_dark_mask: bool = False
    render_front_and_back_to_asset_alpha_mask: bool = False

    def signature(self) -> str:
        return light_signature(self)

    def cache_json(self) -> Dict[str, Any]:
        return {
            "has_light_source": True,
            "light_intensity": self.intensity,
            "radius": self.radius,
            "fall_off": self.fall_off,
            "flare": self.flare,
            "flicker_speed": self.flicker_speed,
            "flicker_smoothness": self.flicker_smoothness,
            "offset_x": self.offset_x,
            "offset_z": self.offset_z,
            "offset_y": 0,
            "light_color": list(self.color),
            "in_front": self.in_front,
            "behind": self.behind,
            "render_to_dark_mask": self.render_to_dark_mask,
            "render_front_and_back_to_asset_alpha_mask": self.render_front_and_back_to_asset_alpha_mask,
        }


def read_int(src: Dict[str, Any], key: str, fallback: int) -> int:
    try:
        if key in src:
            value = src[key]
            if isinstance(value, bool):
                return 100 if value else 0
            if isinstance(value, (int, float)):
                return int(round(float(value)))
            if isinstance(value, str) and value.strip():
                try:
                    return int(round(float(value.strip())))
                except ValueError:
                    return fallback
    except Exception:
        return fallback
    return fallback


def parse_light_entry(raw: Any) -> Optional[LightDefinition]:
    if not isinstance(raw, dict):
        return None
    if not raw.get("has_light_source", False):
        return None

    radius = max(1, read_int(raw, "radius", 64))

    fall_off_value = max(0, read_int(raw, "fall_off", 50))

    flare = max(0, read_int(raw, "flare", 0))
    intensity = max(1, min(255, read_int(raw, "light_intensity", 255)))
    flicker_speed = max(0, min(100, read_int(raw, "flicker_speed", 0)))
    flicker_smoothness = max(
        0, min(100, read_int(raw, "flicker_smoothness", 100))
    )
    offset_x = read_int(raw, "offset_x", 0)
    offset_z = read_int(raw, "offset_z", read_int(raw, "offset_y", 0))

    color = (255, 255, 255)
    try:
        arr = raw.get("light_color")
        if isinstance(arr, list) and len(arr) >= 3:
            r = max(0, min(255, int(arr[0])))
            g = max(0, min(255, int(arr[1])))
            b = max(0, min(255, int(arr[2])))
            color = (r, g, b)
    except Exception:
        color = (255, 255, 255)

    return LightDefinition(
        intensity=intensity,
        radius=radius,
        fall_off=fall_off_value,
        flare=flare,
        flicker_speed=flicker_speed,
        flicker_smoothness=flicker_smoothness,
        offset_x=offset_x,
        offset_z=offset_z,
        color=color,
        in_front=bool(raw.get("in_front", False)),
        behind=bool(raw.get("behind", False)),
        render_to_dark_mask=bool(raw.get("render_to_dark_mask", False)),
        render_front_and_back_to_asset_alpha_mask=bool(
            raw.get("render_front_and_back_to_asset_alpha_mask", False)
        ),
    )


def build_light_image(light: LightDefinition) -> Image.Image:
    key, alpha_bytes, diameter = _compute_alpha_payload((light.radius, light.fall_off))
    alpha = np.frombuffer(alpha_bytes, dtype=np.uint8).reshape((diameter, diameter))
    rgba = np.zeros((diameter, diameter, 4), dtype=np.uint8)
    rgba[:, :, 0:3] = 255
    rgba[:, :, 3] = alpha
    return Image.fromarray(rgba, mode="RGBA")


@dataclass
class LightAsset:
    name: str
    lighting_entries: List[Dict[str, Any]]
    light_defs: List[LightDefinition]
    flagged_indices: List[int]
    entry_to_light_index: Dict[int, int]


class LightTool:
    def __init__(self, manifest_path: str, cache_root: str) -> None:
        self.manifest_path = Path(manifest_path).absolute()
        self.cache_root = Path(cache_root).absolute()
        self.manifest = self._load_manifest()
        self.any_failures = False
        self.alpha_cache: Dict[Tuple[int, int], np.ndarray] = {}

    def _load_manifest(self) -> Dict[str, Any]:
        try:
            with open(self.manifest_path, "r", encoding="utf-8") as f:
                return json.load(f)
        except Exception as exc:
            LOGGER.error("Failed to read manifest '%s': %s", self.manifest_path, exc)
            sys.exit(1)

    def save_manifest(self) -> None:
        try:
            with open(self.manifest_path, "w", encoding="utf-8") as f:
                json.dump(self.manifest, f, indent=2)
        except Exception as exc:
            LOGGER.error("Failed to write manifest '%s': %s", self.manifest_path, exc)
            sys.exit(1)

    @staticmethod
    def _normalize_lighting_entries(asset_meta: Dict[str, Any]) -> List[Dict[str, Any]]:
        lights = asset_meta.get("lighting_info")
        if isinstance(lights, dict):
            lights = [lights]
        if not isinstance(lights, list):
            lights = []
        normalized: List[Dict[str, Any]] = []
        for entry in lights:
            if not isinstance(entry, dict):
                continue
            entry.setdefault("needs_rebuild", False)
            normalized.append(entry)
        asset_meta["lighting_info"] = normalized
        return normalized

    def _collect_assets(self) -> List[LightAsset]:
        assets_block = self.manifest.get("assets", {})
        if not isinstance(assets_block, dict):
            LOGGER.error("Manifest 'assets' block missing or invalid.")
            return []

        light_assets: List[LightAsset] = []
        for name, entry in assets_block.items():
            if not isinstance(entry, dict):
                continue
            entries = self._normalize_lighting_entries(entry)
            flagged = [idx for idx, light in enumerate(entries) if bool(light.get("needs_rebuild"))]
            if not flagged:
                continue
            parsed_defs: List[LightDefinition] = []
            entry_to_light_index: Dict[int, int] = {}
            for idx, light_entry in enumerate(entries):
                parsed = parse_light_entry(light_entry)
                if parsed:
                    entry_to_light_index[idx] = len(parsed_defs)
                    parsed_defs.append(parsed)
            light_assets.append(
                LightAsset(name, entries, parsed_defs, flagged, entry_to_light_index)
            )
        return light_assets

    def _write_metadata(self, cache_dir: Path, signatures: List[str]) -> None:
        meta = {"version": LIGHT_CACHE_VERSION, "signatures": signatures}
        meta_path = cache_dir / "metadata.json"
        cache_dir.mkdir(parents=True, exist_ok=True)
        with open(meta_path, "w", encoding="utf-8") as f:
            json.dump(meta, f, indent=2)

    @staticmethod
    def _read_metadata(cache_dir: Path) -> Optional[List[str]]:
        meta_path = cache_dir / "metadata.json"
        if not meta_path.exists():
            return None
        try:
            payload = json.loads(meta_path.read_text())
            if payload.get("version") != LIGHT_CACHE_VERSION:
                return None
            signatures = payload.get("signatures")
            if not isinstance(signatures, list):
                return None
            return [str(sig) for sig in signatures]
        except Exception:
            return None

    def _build_alpha_cache(self, assets: Iterable[LightAsset]) -> None:
        keys: List[Tuple[int, int]] = []
        for asset in assets:
            for light in asset.light_defs:
                key = (max(1, light.radius), int(light.fall_off))
                keys.append(key)
        unique_keys = sorted(set(keys))
        if not unique_keys:
            return
        if len(unique_keys) == 1:
            key = unique_keys[0]
            _, alpha_bytes, diameter = _compute_alpha_payload(key)
            alpha = np.frombuffer(alpha_bytes, dtype=np.uint8).reshape((diameter, diameter))
            self.alpha_cache[key] = alpha
            return

        max_workers = max(1, min(len(unique_keys), (multiprocessing.cpu_count() or 1) - 1))
        with multiprocessing.get_context("spawn").Pool(processes=max_workers) as pool:
            for key, alpha_bytes, diameter in pool.map(_compute_alpha_payload, unique_keys):
                alpha = np.frombuffer(alpha_bytes, dtype=np.uint8).reshape((diameter, diameter))
                self.alpha_cache[key] = alpha

    def _clear_light_flags(self, asset: LightAsset) -> None:
        for idx in asset.flagged_indices:
            if 0 <= idx < len(asset.lighting_entries):
                asset.lighting_entries[idx]["needs_rebuild"] = False

    def generate_for_asset(self, asset: LightAsset) -> bool:
        cache_dir = self.cache_root / asset.name / "lights"
        cache_dir.mkdir(parents=True, exist_ok=True)

        if not asset.light_defs:
            if cache_dir.exists():
                shutil.rmtree(cache_dir)
            LOGGER.info("[LightTool] %s has no lights; cleared cache directory.", asset.name)
            self._clear_light_flags(asset)
            return True

        LOGGER.info(
            "[LightTool] Regenerating %s (%d light source%s)",
            asset.name,
            len(asset.light_defs),
            "" if len(asset.light_defs) == 1 else "s",
        )

        signatures = [light.signature() for light in asset.light_defs]
        cached_signatures = self._read_metadata(cache_dir)
        full_rebuild = cached_signatures is None or len(cached_signatures) != len(signatures)

        required_indices: List[int] = []
        if full_rebuild:
            if cache_dir.exists():
                shutil.rmtree(cache_dir)
            cache_dir.mkdir(parents=True, exist_ok=True)
            required_indices = list(range(len(asset.light_defs)))
        else:
            required_set = set()
            for entry_idx in asset.flagged_indices:
                light_idx = asset.entry_to_light_index.get(entry_idx)
                if light_idx is not None:
                    required_set.add(light_idx)
            for idx in range(len(asset.light_defs)):
                if not (cache_dir / f"light_{idx}.png").exists():
                    required_set.add(idx)
            required_indices = sorted(required_set)

        for idx in required_indices:
            light = asset.light_defs[idx]
            key = (max(1, light.radius), int(light.fall_off))
            alpha = self.alpha_cache.get(key)
            if alpha is None:
                img = build_light_image(light)
            else:
                h, w = alpha.shape
                rgba = np.zeros((h, w, 4), dtype=np.uint8)
                rgba[:, :, 0:3] = 255
                rgba[:, :, 3] = alpha
                img = Image.fromarray(rgba, mode="RGBA")
            target = cache_dir / f"light_{idx}.png"
            img.save(target, "PNG", optimize=False)

        self._write_metadata(cache_dir, signatures)
        self._clear_light_flags(asset)
        return True

    def run(self) -> None:
        assets = self._collect_assets()
        if not assets:
            return
        self._build_alpha_cache(assets)

        manifest_changed = False
        for asset in assets:
            success = self.generate_for_asset(asset)
            if success:
                manifest_changed = True
            else:
                self.any_failures = True

        if manifest_changed:
            assets_block = self.manifest.get("assets", {})
            if isinstance(assets_block, dict):
                self.manifest["assets"] = assets_block
            self.save_manifest()
            LOGGER.info("Updated manifest needs_rebuild flags after light rebuilds.")
        else:
            LOGGER.info("No light assets marked for rebuild; nothing to do.")

        if self.any_failures:
            LOGGER.warning("Some light assets could not be regenerated; flags remain set.")


def main() -> None:
    print_gpu_status(False)

    tools_dir = Path(__file__).resolve().parent
    repo_root = _find_repo_root(tools_dir)
    manifest_path = str(repo_root / "manifest.json")
    cache_root = str(repo_root / "cache")

    tool = LightTool(manifest_path, cache_root)
    tool.run()


if __name__ == "__main__":
    main()
