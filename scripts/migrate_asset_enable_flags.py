#!/usr/bin/env python3
"""Backfill asset enable flags and normalize floor-box boundary invariant.

This migration:
- Backfills missing/non-boolean asset flags:
  - movement_enabled
  - hitbox_enabled
  - attack_box_enabled
  - floor_boxes_enabled
- Uses existing payload presence as inference source:
  - movement/hitbox/attack: animation-scoped payload presence
  - floor_boxes_enabled: asset-level floor_boxes presence
- Preserves existing payloads (does not strip movement/hit/attack/floor payload keys).
- Enforces floor-box single-boundary invariant deterministically:
  first `is_boundary=true` entry wins; later `true` entries are set to `false`.

The migration is idempotent: re-running after first successful pass is a no-op.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


FLAG_MOVEMENT = "movement_enabled"
FLAG_HITBOX = "hitbox_enabled"
FLAG_ATTACK = "attack_box_enabled"
FLAG_FLOOR = "floor_boxes_enabled"


def has_animation_payload_key(asset: dict[str, Any], key: str) -> bool:
    animations = asset.get("animations")
    if not isinstance(animations, dict):
        return False

    for animation in animations.values():
        if not isinstance(animation, dict):
            continue
        if key not in animation:
            continue
        value = animation[key]
        if isinstance(value, list):
            if len(value) > 0:
                return True
            continue
        if value is not None:
            return True
    return False


def infer_movement_enabled(asset: dict[str, Any]) -> bool:
    return has_animation_payload_key(asset, "movement") or has_animation_payload_key(asset, "movement_paths")


def infer_hitbox_enabled(asset: dict[str, Any]) -> bool:
    return has_animation_payload_key(asset, "hit_boxes")


def infer_attack_enabled(asset: dict[str, Any]) -> bool:
    return has_animation_payload_key(asset, "attack_boxes")


def infer_floor_boxes_enabled(asset: dict[str, Any]) -> bool:
    floor_boxes = asset.get("floor_boxes")
    return isinstance(floor_boxes, list) and len(floor_boxes) > 0


def normalize_single_boundary(asset: dict[str, Any]) -> int:
    """Normalize floor_boxes so at most one is_boundary=true exists.

    Returns number of entries demoted from true -> false.
    """
    floor_boxes = asset.get("floor_boxes")
    if not isinstance(floor_boxes, list):
        return 0

    boundary_seen = False
    demoted = 0
    for entry in floor_boxes:
        if not isinstance(entry, dict):
            continue
        if entry.get("is_boundary") is True:
            if boundary_seen:
                entry["is_boundary"] = False
                demoted += 1
            else:
                boundary_seen = True
    return demoted


def set_or_normalize_flag(
    asset: dict[str, Any], flag: str, inferred_value: bool, stats: dict[str, int], touched: list[bool]
) -> None:
    if flag not in asset:
        asset[flag] = inferred_value
        stats["flags_added"] += 1
        touched[0] = True
        return

    if not isinstance(asset[flag], bool):
        asset[flag] = inferred_value
        stats["flags_normalized"] += 1
        touched[0] = True


def migrate_manifest(manifest: dict[str, Any]) -> dict[str, int]:
    stats = {
        "assets_total": 0,
        "assets_touched": 0,
        "flags_added": 0,
        "flags_normalized": 0,
        "boundary_conflicts_normalized": 0,
    }

    assets = manifest.get("assets")
    if not isinstance(assets, dict):
        return stats

    for asset in assets.values():
        if not isinstance(asset, dict):
            continue

        stats["assets_total"] += 1
        touched = [False]

        set_or_normalize_flag(asset, FLAG_MOVEMENT, infer_movement_enabled(asset), stats, touched)
        set_or_normalize_flag(asset, FLAG_HITBOX, infer_hitbox_enabled(asset), stats, touched)
        set_or_normalize_flag(asset, FLAG_ATTACK, infer_attack_enabled(asset), stats, touched)
        set_or_normalize_flag(asset, FLAG_FLOOR, infer_floor_boxes_enabled(asset), stats, touched)

        demoted = normalize_single_boundary(asset)
        if demoted > 0:
            stats["boundary_conflicts_normalized"] += demoted
            touched[0] = True

        if touched[0]:
            stats["assets_touched"] += 1

    return stats


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "manifest.json",
        help="Path to manifest.json (default: repo_root/manifest.json)",
    )
    args = parser.parse_args()

    manifest_path: Path = args.manifest
    if not manifest_path.exists():
        print(f"Manifest not found: {manifest_path}")
        return 1

    with manifest_path.open("r", encoding="utf-8") as fh:
        manifest = json.load(fh)

    if not isinstance(manifest, dict):
        print(f"Manifest root is not an object: {manifest_path}")
        return 1

    stats = migrate_manifest(manifest)

    if stats["assets_touched"] > 0:
        with manifest_path.open("w", encoding="utf-8", newline="\n") as fh:
            json.dump(manifest, fh, indent=2)
            fh.write("\n")

    print(
        "Migration summary:",
        f"assets_total={stats['assets_total']}",
        f"assets_touched={stats['assets_touched']}",
        f"flags_added={stats['flags_added']}",
        f"flags_normalized={stats['flags_normalized']}",
        f"boundary_conflicts_normalized={stats['boundary_conflicts_normalized']}",
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
