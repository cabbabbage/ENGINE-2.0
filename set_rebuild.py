#!/usr/bin/env python3

import json
import sys
from pathlib import Path

def set_all_needs_rebuild(manifest_path):
    with open(manifest_path, 'r') as f:
        manifest = json.load(f)

    assets = manifest.get('assets', {})
    changed = False

    for asset_name, asset_meta in assets.items():
        if not isinstance(asset_meta, dict):
            continue
        animations = asset_meta.get('animations', {})
        if isinstance(animations, dict):
            for anim_name, anim_meta in animations.items():
                if not isinstance(anim_meta, dict):
                    continue
                frames = anim_meta.get('frames', [])
                if isinstance(frames, list):
                    for frame in frames:
                        if isinstance(frame, dict) and 'needs_rebuild' in frame:
                            if not frame['needs_rebuild']:
                                frame['needs_rebuild'] = True
                                changed = True

    if changed:
        with open(manifest_path, 'w') as f:
            json.dump(manifest, f, indent=2)
        print("Set needs_rebuild to true for all frames.")
    else:
        print("No changes needed.")

if __name__ == "__main__":
    repo_root = Path(__file__).resolve().parent
    manifest_path = repo_root / "manifest.json"
    set_all_needs_rebuild(str(manifest_path))
