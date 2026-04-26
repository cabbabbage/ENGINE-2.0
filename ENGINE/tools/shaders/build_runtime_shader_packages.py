#!/usr/bin/env python3
import argparse
import json
from pathlib import Path

REQUIRED_VARIANTS = [
    "sprite_textured",
    "sprite_batched",
    "light_eval",
    "floor_compose",
    "dark_mask",
    "final_compose",
    "compute_light_binning",
]

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    src = Path(args.input)
    dst = Path(args.output)
    manifest_path = src / "runtime_shaders.json"
    if not manifest_path.exists():
        raise SystemExit(f"missing manifest: {manifest_path}")

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    variants = manifest.get("variants", {})
    for name in REQUIRED_VARIANTS:
        if name not in variants:
            raise SystemExit(f"missing required variant: {name}")
        for key in ("dxil", "spirv"):
            rel = variants[name].get(key, "")
            if not rel:
                raise SystemExit(f"variant {name} missing {key}")
            full = src / rel
            if not full.exists():
                raise SystemExit(f"variant {name} {key} missing file: {full}")

    dst.mkdir(parents=True, exist_ok=True)
    (dst / "runtime_shaders.json").write_text(json.dumps(manifest, sort_keys=True, indent=2), encoding="utf-8")
    for name, entry in variants.items():
        for key in ("dxil", "spirv"):
            rel = Path(entry[key])
            out = dst / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes((src / rel).read_bytes())

    print(f"packaged {len(variants)} shader variants -> {dst}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
