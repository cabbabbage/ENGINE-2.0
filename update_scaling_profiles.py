#!/usr/bin/env python3

import json
from pathlib import Path

def update_scaling_profiles(manifest_path):
    with open(manifest_path, 'r') as f:
        manifest = json.load(f)

    assets = manifest.get('assets', {})
    changed = False

    for asset_name, asset_meta in assets.items():
        if not isinstance(asset_meta, dict):
            continue
        scaling_profile = asset_meta.get('scaling_profile')
        if not isinstance(scaling_profile, dict):
            continue
        recommended_steps = scaling_profile.get('recommended_steps')
        if not isinstance(recommended_steps, list):
            continue
        if recommended_steps and recommended_steps[0] == 1.0:
            recommended_steps.pop(0)
            changed = True
            print(f"Updated {asset_name} recommended_steps to {recommended_steps}")

    if changed:
        with open(manifest_path, 'w') as f:
            json.dump(manifest, f, indent=2)
        print("Updated manifest scaling profiles.")
    else:
        print("No changes needed.")

if __name__ == "__main__":
    repo_root = Path(__file__).resolve().parent
    manifest_path = repo_root / "manifest.json"
    update_scaling_profiles(str(manifest_path))
