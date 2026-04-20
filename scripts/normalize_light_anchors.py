#!/usr/bin/env python3
import argparse
import copy
import json
from pathlib import Path
from typing import Any, Dict, List, Tuple


def _sanitize_anchor_entry(anchor: Dict[str, Any]) -> None:
    anchor.pop("light_enabled", None)
    light = anchor.get("light")
    if isinstance(light, dict):
        light.pop("light_enabled", None)


def _dedupe_frame_anchors(frame_anchors: List[Any]) -> Tuple[List[Dict[str, Any]], int]:
    deduped: List[Dict[str, Any]] = []
    seen = set()
    dropped = 0
    for entry in frame_anchors:
        if not isinstance(entry, dict):
            dropped += 1
            continue
        name = entry.get("name")
        if not isinstance(name, str) or not name:
            dropped += 1
            continue
        if name in seen:
            dropped += 1
            continue
        seen.add(name)
        _sanitize_anchor_entry(entry)
        deduped.append(entry)
    return deduped, dropped


def normalize_animation_anchor_lights(animation_payload: Dict[str, Any]) -> Dict[str, int]:
    stats = {
        "anchors_deduped": 0,
        "light_propagations": 0,
        "lights_removed": 0,
        "light_enabled_removed": 0,
    }

    anchor_points = animation_payload.get("anchor_points")
    if not isinstance(anchor_points, list):
        return stats

    normalized_frames: List[List[Dict[str, Any]]] = []
    for frame in anchor_points:
        if isinstance(frame, list):
            deduped, dropped = _dedupe_frame_anchors(frame)
            stats["anchors_deduped"] += dropped
            normalized_frames.append(deduped)
        else:
            normalized_frames.append([])

    frame0_light_by_name: Dict[str, Dict[str, Any]] = {}
    if normalized_frames:
        for anchor in normalized_frames[0]:
            light = anchor.get("light")
            if isinstance(light, dict):
                frame0_light_by_name[anchor["name"]] = copy.deepcopy(light)

    for frame in normalized_frames:
        for anchor in frame:
            before_has_light_enabled = "light_enabled" in anchor
            _sanitize_anchor_entry(anchor)
            if before_has_light_enabled:
                stats["light_enabled_removed"] += 1
            light = anchor.get("light")
            if isinstance(light, dict) and "light_enabled" in light:
                stats["light_enabled_removed"] += 1

            name = anchor["name"]
            canonical_light = frame0_light_by_name.get(name)
            if canonical_light is None:
                if "light" in anchor:
                    anchor.pop("light", None)
                    stats["lights_removed"] += 1
                continue

            if anchor.get("light") != canonical_light:
                anchor["light"] = copy.deepcopy(canonical_light)
                stats["light_propagations"] += 1

    animation_payload["anchor_points"] = normalized_frames
    return stats


def normalize_manifest(manifest: Dict[str, Any]) -> Dict[str, int]:
    total = {
        "assets_touched": 0,
        "animations_touched": 0,
        "anchors_deduped": 0,
        "light_propagations": 0,
        "lights_removed": 0,
        "light_enabled_removed": 0,
    }

    assets = manifest.get("assets")
    if not isinstance(assets, dict):
        return total

    for asset_payload in assets.values():
        if not isinstance(asset_payload, dict):
            continue
        animations = asset_payload.get("animations")
        if not isinstance(animations, dict):
            continue

        asset_touched = False
        for animation_payload in animations.values():
            if not isinstance(animation_payload, dict):
                continue
            stats = normalize_animation_anchor_lights(animation_payload)
            if any(v > 0 for v in stats.values()):
                asset_touched = True
                total["animations_touched"] += 1
            total["anchors_deduped"] += stats["anchors_deduped"]
            total["light_propagations"] += stats["light_propagations"]
            total["lights_removed"] += stats["lights_removed"]
            total["light_enabled_removed"] += stats["light_enabled_removed"]

        if asset_touched:
            total["assets_touched"] += 1

    return total


def main() -> None:
    parser = argparse.ArgumentParser(description="Normalize manifest anchor light data.")
    parser.add_argument("manifest", type=Path, help="Path to manifest.json")
    args = parser.parse_args()

    manifest_path = args.manifest
    with manifest_path.open("r", encoding="utf-8") as f:
        manifest = json.load(f)

    stats = normalize_manifest(manifest)

    with manifest_path.open("w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
        f.write("\n")

    print(json.dumps(stats, indent=2))


if __name__ == "__main__":
    main()
