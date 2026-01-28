#!/usr/bin/env python3
"""
Script to remove deprecated fields from manifest.json asset entries.

Deprecated fields identified from codebase analysis:
- Animation level: crop_frames, flipped_source, inherit_source_movement,
                   reverse_source, rnd_start, speed_multiplier, on_end
- Frame level: needs_rebuild
- Asset level: animation_children, async_children (legacy child formats)
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
    'child_timelines',  # Timeline entries (new format)
    'children',         # Timeline entries (legacy format)
}

DEPRECATED_FRAME_FIELDS = {
    'needs_rebuild',
}

DEPRECATED_ASSET_FIELDS = {
    'animation_children',
    'async_children',
}


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


def count_removals(original: Dict[str, Any], cleaned: Dict[str, Any]) -> Dict[str, int]:
    """Count removed deprecated fields for reporting."""
    counts = {
        'animation_fields': 0,
        'frame_fields': 0,
        'asset_fields': 0,
    }

    if 'assets' not in original:
        return counts

    for asset_name, asset in original['assets'].items():
        # Count asset-level removals
        for field in DEPRECATED_ASSET_FIELDS:
            if field in asset:
                counts['asset_fields'] += 1

        # Count animation-level removals
        if 'animations' in asset and isinstance(asset['animations'], dict):
            for anim_name, animation in asset['animations'].items():
                for field in DEPRECATED_ANIMATION_FIELDS:
                    if field in animation:
                        counts['animation_fields'] += 1

                # Count frame-level removals
                if 'frames' in animation and isinstance(animation['frames'], list):
                    counts['frame_fields'] += len([f for f in animation['frames']
                                                   if isinstance(f, dict) and any(k in f for k in DEPRECATED_FRAME_FIELDS)])

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

    # Count removals before cleaning
    print("\nAnalyzing deprecated fields...")
    removal_counts = count_removals(manifest, manifest)

    # Clean the manifest
    cleaned_manifest = clean_manifest(manifest)

    # Report results
    total_removed = sum(removal_counts.values())
    print(f"\nDeprecated fields found and removed:")
    print(f"  - Asset-level fields: {removal_counts['asset_fields']}")
    print(f"  - Animation-level fields: {removal_counts['animation_fields']}")
    print(f"  - Frame-level fields: {removal_counts['frame_fields']}")
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
