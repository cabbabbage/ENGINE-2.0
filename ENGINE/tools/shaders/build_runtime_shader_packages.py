#!/usr/bin/env python3
from __future__ import annotations

import argparse
import glob
import json
import os
import shutil
import subprocess
from pathlib import Path

REQUIRED_VARIANTS = [
    "fullscreen_vertex",
    "sprite_textured",
    "sprite_batched",
    "light_eval",
    "floor_compose",
    "dark_mask",
    "final_compose",
    "compute_light_binning",
]

STAGE_HINTS = {
    "fullscreen_vertex": "vertex",
    "sprite_textured": "fragment",
    "sprite_batched": "fragment",
    "light_eval": "fragment",
    "floor_compose": "fragment",
    "dark_mask": "fragment",
    "final_compose": "fragment",
    "compute_light_binning": "compute",
}

VALID_STAGES = {"auto", "vertex", "fragment", "compute"}


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


def normalize_entry(raw_entry: object, backend: str, default_stage: str) -> tuple[str, str, str]:
    if isinstance(raw_entry, str):
        return raw_entry, "main", default_stage
    if isinstance(raw_entry, dict):
        path = str(raw_entry.get("path", "")).strip()
        entrypoint = str(raw_entry.get("entrypoint", "main")).strip() or "main"
        stage = str(raw_entry.get("stage", default_stage)).strip().lower() or default_stage
        if stage == "auto" and default_stage != "auto":
            stage = default_stage
        if stage not in VALID_STAGES:
            raise SystemExit(f"shader backend entry has invalid stage '{stage}' for {backend}")
        return path, entrypoint, stage
    raise SystemExit(f"shader backend entry must be string/object for {backend}")


def find_dxc() -> str | None:
    env_override = os.environ.get("VIBBLE_DXC", "").strip()
    if env_override and Path(env_override).exists():
        return env_override

    from_path = shutil.which("dxc")
    if from_path:
        return from_path

    windows_sdk_glob = r"C:\Program Files (x86)\Windows Kits\10\bin\*\x64\dxc.exe"
    candidates = sorted(glob.glob(windows_sdk_glob))
    if candidates:
        return candidates[-1]
    return None


def find_glslc(src_root: Path) -> str | None:
    env_override = os.environ.get("VIBBLE_GLSLC", "").strip()
    if env_override and Path(env_override).exists():
        return env_override

    from_path = shutil.which("glslc")
    if from_path:
        return from_path

    repo_root = src_root.parents[3] if len(src_root.parents) >= 4 else src_root
    bundled = repo_root / "vcpkg" / "installed" / "x64-windows" / "tools" / "shaderc" / "glslc.exe"
    if bundled.exists():
        return str(bundled)
    return None


def dxc_profile_for_stage(stage: str) -> str:
    if stage == "vertex":
        return "vs_6_0"
    if stage == "fragment":
        return "ps_6_0"
    if stage == "compute":
        return "cs_6_0"
    raise SystemExit(f"cannot derive shader profile for stage '{stage}'")


def run_dxc(dxc: str, source: Path, profile: str, entrypoint: str, output: Path, spirv: bool) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [dxc, "-T", profile, "-E", entrypoint, "-Fo", str(output), str(source)]
    if spirv:
        command[1:1] = ["-spirv", "-fspv-target-env=vulkan1.2"]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        stderr = result.stderr.strip()
        stdout = result.stdout.strip()
        message = stderr or stdout or f"dxc exited with {result.returncode}"
        raise SystemExit(f"dxc failed for {source.name} ({profile}, spirv={spirv}): {message}")


def glsl_extension_for_stage(stage: str) -> str:
    if stage == "vertex":
        return "vert"
    if stage == "fragment":
        return "frag"
    if stage == "compute":
        return "comp"
    raise SystemExit(f"cannot derive GLSL extension for stage '{stage}'")


def run_glslc(glslc: str, source: Path, output: Path, stage: str) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [glslc, "-fshader-stage=" + stage, str(source), "-o", str(output), "--target-env=vulkan1.2"]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        stderr = result.stderr.strip()
        stdout = result.stdout.strip()
        message = stderr or stdout or f"glslc exited with {result.returncode}"
        raise SystemExit(f"glslc failed for {source.name}: {message}")


def compile_or_copy_variant(
    dxc_path: str | None,
    glslc_path: str | None,
    src_root: Path,
    dst_root: Path,
    variant_name: str,
    dxil_rel: str,
    spirv_rel: str,
    stage: str,
    entrypoint: str,
    require_compile: bool,
) -> tuple[bytes, bytes]:
    dxil_out = dst_root / dxil_rel
    spirv_out = dst_root / spirv_rel
    dxil_out.parent.mkdir(parents=True, exist_ok=True)
    spirv_out.parent.mkdir(parents=True, exist_ok=True)

    if dxc_path:
        source_path = src_root / "src" / "hlsl" / f"{variant_name}.hlsl"
        if not source_path.exists():
            raise SystemExit(f"missing shader source file for {variant_name}: {source_path}")
        profile = dxc_profile_for_stage(stage)
        run_dxc(dxc_path, source_path, profile, entrypoint, dxil_out, spirv=False)
    else:
        if require_compile:
            raise SystemExit("dxc was not found and --require-compile was set")
        dxil_src = src_root / dxil_rel
        if not dxil_src.exists():
            raise SystemExit(f"missing prebuilt DXIL binary for {variant_name} and no dxc compiler available")
        dxil_out.write_bytes(dxil_src.read_bytes())

    if glslc_path:
        glsl_stage_extension = glsl_extension_for_stage(stage)
        glsl_source = src_root / "src" / "glsl" / f"{variant_name}.{glsl_stage_extension}.glsl"
        if not glsl_source.exists():
            raise SystemExit(f"missing GLSL source file for {variant_name}: {glsl_source}")
        run_glslc(glslc_path, glsl_source, spirv_out, stage=stage)
    else:
        if require_compile:
            raise SystemExit("glslc was not found and --require-compile was set")
        spirv_src = src_root / spirv_rel
        if not spirv_src.exists():
            raise SystemExit(f"missing prebuilt SPIR-V binary for {variant_name} and no glslc compiler available")
        spirv_out.write_bytes(spirv_src.read_bytes())

    dxil_bytes = dxil_out.read_bytes()
    spirv_bytes = spirv_out.read_bytes()
    ensure_magic(variant_name, "dxil", dxil_bytes)
    ensure_magic(variant_name, "spirv", spirv_bytes)
    return dxil_bytes, spirv_bytes


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--require-compile", action="store_true")
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

    for name in REQUIRED_VARIANTS:
        if name not in variants:
            raise SystemExit(f"missing required variant: {name}")

    dxc_path = find_dxc()
    glslc_path = find_glslc(src)
    if dxc_path:
        print(f"using dxc compiler: {dxc_path}")
    else:
        print("dxc compiler not found; using prebuilt DXIL binaries from source tree")
    if glslc_path:
        print(f"using glslc compiler: {glslc_path}")
    else:
        print("glslc compiler not found; using prebuilt SPIR-V binaries from source tree")

    packaged_manifest: dict[str, object] = {
        "manifest_version": 2,
        "build_tool_version": 2,
        "variants": {},
    }

    dst.mkdir(parents=True, exist_ok=True)

    for name, entry in sorted(variants.items()):
        if not isinstance(entry, dict):
            raise SystemExit(f"variant entry must be object: {name}")
        default_stage = STAGE_HINTS.get(name, "auto")
        dxil_rel, dxil_entrypoint, dxil_stage = normalize_entry(entry.get("dxil", ""), "dxil", default_stage)
        spirv_rel, spirv_entrypoint, spirv_stage = normalize_entry(entry.get("spirv", ""), "spirv", default_stage)
        if not dxil_rel or not spirv_rel:
            raise SystemExit(f"variant {name} must provide both dxil and spirv")
        if dxil_stage != spirv_stage:
            raise SystemExit(f"variant {name} has stage mismatch between dxil ({dxil_stage}) and spirv ({spirv_stage})")
        if dxil_entrypoint != spirv_entrypoint:
            raise SystemExit(
                f"variant {name} has entrypoint mismatch between dxil ({dxil_entrypoint}) and spirv ({spirv_entrypoint})"
            )

        dxil_bytes, spirv_bytes = compile_or_copy_variant(
            dxc_path=dxc_path,
            glslc_path=glslc_path,
            src_root=src,
            dst_root=dst,
            variant_name=name,
            dxil_rel=dxil_rel,
            spirv_rel=spirv_rel,
            stage=dxil_stage,
            entrypoint=dxil_entrypoint,
            require_compile=args.require_compile,
        )

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

    (dst / "runtime_shaders.json").write_text(
        json.dumps(packaged_manifest, sort_keys=True, indent=2),
        encoding="utf-8",
    )

    print(f"packaged {len(packaged_manifest['variants'])} shader variants -> {dst}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
