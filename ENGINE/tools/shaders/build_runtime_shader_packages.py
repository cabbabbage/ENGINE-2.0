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


def fnv1a64(data: bytes) -> int:
    hash_value = 0xCBF29CE484222325
    for byte in data:
        hash_value ^= byte
        hash_value = (hash_value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return hash_value


def ensure_magic(name: str, backend: str, payload: bytes) -> None:
    if backend == "spirv":
        if len(payload) < 4 or payload[:4] != b"\x03\x02\x23\x07":
            raise SystemExit(f"variant {name} {backend} has invalid SPIR-V magic")
    elif backend == "dxil":
        if len(payload) < 4 or payload[:4] != b"DXBC":
            raise SystemExit(f"variant {name} {backend} has invalid DXIL container magic")


def normalize_entry(raw_entry: object, backend: str) -> tuple[str, str, str]:
    if isinstance(raw_entry, str):
        return raw_entry, "main", "auto"
    if isinstance(raw_entry, dict):
        path = str(raw_entry.get("path", "")).strip()
        entrypoint = str(raw_entry.get("entrypoint", "main")).strip() or "main"
        stage = str(raw_entry.get("stage", "auto")).strip() or "auto"
        return path, entrypoint, stage
    raise SystemExit(f"shader backend entry must be string/object for {backend}")


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

    source_manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    variants = source_manifest.get("variants", {})
    if not isinstance(variants, dict):
        raise SystemExit("manifest variants must be an object")

    packaged_manifest: dict[str, object] = {
        "manifest_version": 2,
        "build_tool_version": 2,
        "variants": {},
    }

    for name in REQUIRED_VARIANTS:
        if name not in variants:
            raise SystemExit(f"missing required variant: {name}")

    for name, entry in sorted(variants.items()):
        if not isinstance(entry, dict):
            raise SystemExit(f"variant entry must be object: {name}")

        dxil_rel, dxil_entrypoint, dxil_stage = normalize_entry(entry.get("dxil", ""), "dxil")
        spirv_rel, spirv_entrypoint, spirv_stage = normalize_entry(entry.get("spirv", ""), "spirv")

        if not dxil_rel or not spirv_rel:
            raise SystemExit(f"variant {name} must provide both dxil and spirv")

        dxil_src = src / dxil_rel
        spirv_src = src / spirv_rel
        if not dxil_src.exists():
            raise SystemExit(f"variant {name} dxil missing file: {dxil_src}")
        if not spirv_src.exists():
            raise SystemExit(f"variant {name} spirv missing file: {spirv_src}")

        dxil_bytes = dxil_src.read_bytes()
        spirv_bytes = spirv_src.read_bytes()
        ensure_magic(name, "dxil", dxil_bytes)
        ensure_magic(name, "spirv", spirv_bytes)

        packaged_manifest["variants"][name] = {
            "dxil": {
                "path": dxil_rel,
                "entrypoint": dxil_entrypoint,
                "stage": dxil_stage,
                "hash_fnv1a64": f"0x{fnv1a64(dxil_bytes):016x}",
                "file_size_bytes": len(dxil_bytes),
            },
            "spirv": {
                "path": spirv_rel,
                "entrypoint": spirv_entrypoint,
                "stage": spirv_stage,
                "hash_fnv1a64": f"0x{fnv1a64(spirv_bytes):016x}",
                "file_size_bytes": len(spirv_bytes),
            },
        }

    dst.mkdir(parents=True, exist_ok=True)
    (dst / "runtime_shaders.json").write_text(
        json.dumps(packaged_manifest, sort_keys=True, indent=2),
        encoding="utf-8",
    )

    for name, entry in packaged_manifest["variants"].items():
        dxil_rel = Path(entry["dxil"]["path"])
        spirv_rel = Path(entry["spirv"]["path"])

        dxil_out = dst / dxil_rel
        spirv_out = dst / spirv_rel
        dxil_out.parent.mkdir(parents=True, exist_ok=True)
        spirv_out.parent.mkdir(parents=True, exist_ok=True)
        dxil_out.write_bytes((src / dxil_rel).read_bytes())
        spirv_out.write_bytes((src / spirv_rel).read_bytes())

    print(f"packaged {len(packaged_manifest['variants'])} shader variants -> {dst}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
