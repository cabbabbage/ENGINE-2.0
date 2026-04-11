#!/usr/bin/env python3
"""Hard-cutover migration for animation data inversion flags.

This script:
- Removes legacy inversion fields.
- Removes legacy inversion entries inside `derived_modifiers`.
- Ensures animation payloads have `invert_x`, `invert_y`, `invert_z` set to `false`.

Usage:
    python scripts/migrate_animation_inversion_xyz.py [path/to/manifest.json]
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


LEGACY_TOP_LEVEL_KEYS = {
    "flipped_source",
    "flip_vertical_source",
    "flip_movement_horizontal",
    "flip_movement_vertical",
}

LEGACY_DERIVED_MODIFIER_KEYS = {
    "flipX",
    "flipY",
    "flipMovementX",
    "flipMovementY",
}


def looks_like_animation_payload(node: dict[str, Any]) -> bool:
    source = node.get("source")
    if isinstance(source, dict) and isinstance(source.get("kind"), str):
        return True
    if "number_of_frames" in node and any(
        key in node for key in ("movement", "anchor_points", "hit_boxes", "attack_boxes")
    ):
        return True
    return False


def migrate_node(node: Any, stats: dict[str, int]) -> None:
    if isinstance(node, list):
        for item in node:
            migrate_node(item, stats)
        return

    if not isinstance(node, dict):
        return

    for key in tuple(node.keys()):
        if key in LEGACY_TOP_LEVEL_KEYS:
            node.pop(key, None)
            stats["legacy_top_level_removed"] += 1

    derived_modifiers = node.get("derived_modifiers")
    if isinstance(derived_modifiers, dict):
        for key in tuple(derived_modifiers.keys()):
            if key in LEGACY_DERIVED_MODIFIER_KEYS:
                derived_modifiers.pop(key, None)
                stats["legacy_derived_removed"] += 1

    if looks_like_animation_payload(node):
        for key in ("invert_x", "invert_y", "invert_z"):
            prev = node.get(key, None)
            node[key] = False
            if prev is None:
                stats["invert_keys_added"] += 1
            elif prev is not False:
                stats["invert_keys_forced_false"] += 1

    for value in node.values():
        migrate_node(value, stats)


def main() -> int:
    parser = argparse.ArgumentParser(description="Migrate animation inversion flags to invert_x/y/z.")
    parser.add_argument(
        "manifest",
        nargs="?",
        default="manifest.json",
        help="Path to the manifest JSON file (default: manifest.json).",
    )
    args = parser.parse_args()

    manifest_path = Path(args.manifest)
    if not manifest_path.is_file():
        raise FileNotFoundError(f"Manifest not found: {manifest_path}")

    raw = manifest_path.read_text(encoding="utf-8")
    data = json.loads(raw)

    stats = {
        "legacy_top_level_removed": 0,
        "legacy_derived_removed": 0,
        "invert_keys_added": 0,
        "invert_keys_forced_false": 0,
    }
    migrate_node(data, stats)

    manifest_path.write_text(
        json.dumps(data, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )

    print(
        "Migration complete:",
        f"removed_top_level={stats['legacy_top_level_removed']}",
        f"removed_derived={stats['legacy_derived_removed']}",
        f"added_invert={stats['invert_keys_added']}",
        f"forced_invert_false={stats['invert_keys_forced_false']}",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
