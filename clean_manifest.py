#!/usr/bin/env python3
"""
Script to remove deprecated fields from manifest.json asset entries.

Deprecated fields identified from codebase analysis:
- Animation level: crop_frames, flipped_source, inherit_source_movement,
                   reverse_source, rnd_start, speed_multiplier, on_end
- Frame level: needs_rebuild, children (legacy child payloads)
- Asset level: animation_children, async_children (legacy child formats)
- Movement entries: embedded legacy child arrays ("children")
"""

import json
import os
from pathlib import Path
from typing import Any, Dict, List


# Deprecated fields to remove at various levels
DEPRECATED_ANIMATION_FIELDS = {
    'crop_frames',
    'flipped_source',
    'inherit_source_movement',
    'reverse_source',
    'rnd_start',
    'speed_multiplier',
    'on_end',
    'flip_vertical_source',
    'flip_movement_horizontal',
    'flip_movement_vertical',
    # NOTE: child_timelines is the new format and must stay; do not strip it here.
    'children',  # Legacy timeline payloads
}

DEPRECATED_FRAME_FIELDS = {
    'needs_rebuild',
    'children',  # Legacy per-frame child payloads
}

DEPRECATED_ASSET_FIELDS = {
    'animation_children',
    'async_children',
}


def _is_legacy_child_list(value: Any) -> bool:
    """Heuristically detect legacy child arrays attached to movement entries."""
    if not isinstance(value, list):
        return False
    if not value:
        return False
    # Movement child payloads are typically lists of lists with 5-6 numeric/bool entries.
    return all(isinstance(item, list) and len(item) >= 5 for item in value)


def clean_movement_entry(entry: Any) -> Any:
    """Strip legacy child payloads from a single movement entry (dict or list)."""
    if isinstance(entry, dict):
        entry.pop('children', None)
        entry.pop('child_timelines', None)  # child_timelines inside movement is legacy/invalid
        return entry

    if isinstance(entry, list):
        # Legacy format often puts child arrays as the last element; drop them.
        while entry and _is_legacy_child_list(entry[-1]):
            entry = entry[:-1]
        return entry

    return entry


def clean_frame(frame: Dict[str, Any]) -> Dict[str, Any]:
    """Remove deprecated fields from a frame object."""
    for field in DEPRECATED_FRAME_FIELDS:
        frame.pop(field, None)
    return frame


def clean_animation(animation: Dict[str, Any]) -> Dict[str, Any]:
    """Remove deprecated fields from an animation object."""
    # Remove deprecated animation-level fields
    for field in DEPRECATED_ANIMATION_FIELDS:
        animation.pop(field, None)

    # Clean frames within animations
    if 'frames' in animation and isinstance(animation['frames'], list):
        animation['frames'] = [clean_frame(frame) for frame in animation['frames']]

    # Clean movement entries (legacy child arrays)
    if 'movement' in animation and isinstance(animation['movement'], list):
        animation['movement'] = [clean_movement_entry(m) for m in animation['movement']]

    # Clean movement paths if they contain frames
    if 'movement_paths' in animation and isinstance(animation['movement_paths'], list):
        for path in animation['movement_paths']:
            if isinstance(path, list):
                for frame in path:
                    if isinstance(frame, dict):
                        clean_frame(frame)

    return animation


def clean_asset(asset: Dict[str, Any]) -> Dict[str, Any]:
    """Remove deprecated fields from an asset object."""
    # Remove deprecated asset-level fields
    for field in DEPRECATED_ASSET_FIELDS:
        asset.pop(field, None)

    # Clean animations
    if 'animations' in asset and isinstance(asset['animations'], dict):
        for anim_name, animation in asset['animations'].items():
            clean_animation(animation)

    return asset


def clean_manifest(manifest: Dict[str, Any]) -> Dict[str, Any]:
    """Remove deprecated fields from entire manifest."""
    if 'assets' in manifest and isinstance(manifest['assets'], dict):
        for asset_name, asset in manifest['assets'].items():
            clean_asset(asset)

    return manifest


def count_deprecated(manifest: Dict[str, Any]) -> Dict[str, int]:
    """Count occurrences of deprecated fields to measure cleanup impact."""
    counts = {
        'animation_fields': 0,
        'frame_fields': 0,
        'asset_fields': 0,
        'movement_children': 0,
    }

    assets = manifest.get('assets')
    if not isinstance(assets, dict):
        return counts

    for asset in assets.values():
        if not isinstance(asset, dict):
            continue

        for field in DEPRECATED_ASSET_FIELDS:
            if field in asset:
                counts['asset_fields'] += 1

        animations = asset.get('animations')
        if not isinstance(animations, dict):
            continue

        for animation in animations.values():
            if not isinstance(animation, dict):
                continue

            for field in DEPRECATED_ANIMATION_FIELDS:
                if field in animation:
                    counts['animation_fields'] += 1

            frames = animation.get('frames')
            if isinstance(frames, list):
                counts['frame_fields'] += sum(
                    1 for frame in frames
                    if isinstance(frame, dict) and any(k in frame for k in DEPRECATED_FRAME_FIELDS)
                )

            movement = animation.get('movement')
            if isinstance(movement, list):
                for entry in movement:
                    if isinstance(entry, dict) and 'children' in entry:
                        counts['movement_children'] += 1
                    elif isinstance(entry, list) and entry and _is_legacy_child_list(entry[-1]):
                        counts['movement_children'] += 1

    return counts


def main():
    """Main entry point."""
    # Find manifest.json in project root
    script_dir = Path(__file__).parent
    manifest_path = script_dir / 'manifest.json'

    if not manifest_path.exists():
        print(f"ERROR: manifest.json not found at {manifest_path}")
        return 1

    print(f"Loading manifest from: {manifest_path}")

    try:
        with open(manifest_path, 'r', encoding='utf-8') as f:
            manifest = json.load(f)
    except json.JSONDecodeError as e:
        print(f"ERROR: Failed to parse manifest.json: {e}")
        return 1
    except Exception as e:
        print(f"ERROR: Failed to read manifest.json: {e}")
        return 1

    # Count deprecated fields before cleaning
    print("\nAnalyzing deprecated fields...")
    before_counts = count_deprecated(manifest)

    # Clean the manifest
    cleaned_manifest = clean_manifest(manifest)

    # Count after cleaning to show delta
    after_counts = count_deprecated(cleaned_manifest)
    removed_counts = {k: before_counts[k] - after_counts[k] for k in before_counts}
    total_removed = sum(removed_counts.values())

    print(f"\nDeprecated fields removed:")
    print(f"  - Asset-level fields: {removed_counts['asset_fields']}")
    print(f"  - Animation-level fields: {removed_counts['animation_fields']}")
    print(f"  - Frame-level fields: {removed_counts['frame_fields']}")
    print(f"  - Movement child payloads: {removed_counts['movement_children']}")
    print(f"  - TOTAL: {total_removed}")

    if total_removed == 0:
        print("\nNo deprecated fields found. Manifest is already clean!")
        return 0

    # Write back the cleaned manifest
    try:
        with open(manifest_path, 'w', encoding='utf-8') as f:
            json.dump(cleaned_manifest, f, indent=2)
        print(f"\nSuccessfully cleaned manifest.json and saved to: {manifest_path}")
        return 0
    except Exception as e:
        print(f"ERROR: Failed to write manifest.json: {e}")
        return 1


if __name__ == '__main__':
    exit(main())
