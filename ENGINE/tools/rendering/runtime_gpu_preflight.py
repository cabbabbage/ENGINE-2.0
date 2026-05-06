#!/usr/bin/env python3
"""Runtime SDL_GPU preflight checks for reproducible local validation."""

from __future__ import annotations

import argparse
import glob
import json
import os
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


REQUIRED_SHADER_VARIANTS = (
    "fullscreen_vertex",
    "sprite_textured",
    "sprite_batched",
    "floor_compose",
    "final_compose",
)


@dataclass
class CheckResult:
    name: str
    ok: bool
    detail: str


def find_tool(names: Iterable[str], workspace: Path) -> str | None:
    for name in names:
        resolved = shutil.which(name)
        if resolved:
            return resolved

    if "cmake" in names:
        bundled = (
            workspace
            / "vcpkg"
            / "downloads"
            / "tools"
            / "cmake-4.2.3-windows"
            / "cmake-4.2.3-windows-x86_64"
            / "bin"
            / "cmake.exe"
        )
        if bundled.exists():
            return str(bundled)

    return None


def check_manifest(workspace: Path) -> CheckResult:
    manifest_path = workspace / "ENGINE" / "runtime" / "rendering" / "shaders" / "runtime_shaders.json"
    if not manifest_path.exists():
        return CheckResult("shader_manifest", False, f"missing {manifest_path}")

    try:
        root = json.loads(manifest_path.read_text(encoding="utf-8"))
    except Exception as exc:  # pragma: no cover - defensive
        return CheckResult("shader_manifest", False, f"invalid JSON: {exc}")

    variants = root.get("variants")
    if not isinstance(variants, dict):
        return CheckResult("shader_manifest", False, "manifest 'variants' must be an object")

    missing = [name for name in REQUIRED_SHADER_VARIANTS if name not in variants]
    if missing:
        return CheckResult("shader_manifest", False, f"missing required variants: {', '.join(missing)}")

    return CheckResult("shader_manifest", True, f"{len(variants)} variants")


def check_variant_payloads(workspace: Path) -> CheckResult:
    shader_dir = workspace / "ENGINE" / "runtime" / "rendering" / "shaders"
    missing_files: list[str] = []
    for variant in REQUIRED_SHADER_VARIANTS:
        for backend, extension in (("dxil", ".dxil"), ("spirv", ".spv")):
            candidate = shader_dir / backend / f"{variant}{extension}"
            if not candidate.exists():
                missing_files.append(str(candidate))
    if missing_files:
        return CheckResult("shader_payloads", False, f"missing files: {len(missing_files)}")
    return CheckResult("shader_payloads", True, "all required DXIL/SPIR-V payload files present")

def check_runtime_graph_bridge_free(workspace: Path) -> CheckResult:
    runtime_render_cpp = workspace / "ENGINE" / "runtime" / "rendering" / "render" / "render.cpp"
    if not runtime_render_cpp.exists():
        return CheckResult("runtime_graph_bridge_free", False, f"missing {runtime_render_cpp}")

    content = runtime_render_cpp.read_text(encoding="utf-8", errors="replace")
    forbidden_tokens = (
        "scene.composite_external",
        "register_external_texture_resource(",
        "extract_gpu_texture_pointer(",
        "SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER",
    )
    for token in forbidden_tokens:
        if token in content:
            return CheckResult(
                "runtime_graph_bridge_free",
                False,
                f"forbidden token present in render.cpp: {token}",
            )
    return CheckResult("runtime_graph_bridge_free", True, "runtime graph has no SDL texture bridge imports")


def check_sdl_packages(workspace: Path) -> CheckResult:
    required = (
        workspace / "vcpkg_installed" / "x64-windows" / "include" / "SDL3" / "SDL.h",
        workspace / "vcpkg_installed" / "x64-windows" / "include" / "SDL3" / "SDL_gpu.h",
        workspace / "vcpkg_installed" / "x64-windows" / "share" / "SDL3" / "SDL3Config.cmake",
    )
    missing = [str(path) for path in required if not path.exists()]
    if missing:
        return CheckResult("sdl3_packages", False, f"missing: {len(missing)}")
    return CheckResult("sdl3_packages", True, "headers + CMake package files detected")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workspace", default=".")
    parser.add_argument("--strict-tools", action="store_true")
    args = parser.parse_args()

    workspace = Path(args.workspace).resolve()

    cmake_path = find_tool(("cmake",), workspace)
    cl_path = find_tool(("cl",), workspace)
    dxc_path = os.environ.get("VIBBLE_DXC") or find_tool(("dxc",), workspace)
    if not dxc_path:
        windows_sdk_candidates = sorted(glob.glob(r"C:\Program Files (x86)\Windows Kits\10\bin\*\x64\dxc.exe"))
        if windows_sdk_candidates:
            dxc_path = windows_sdk_candidates[-1]

    glslc_path = os.environ.get("VIBBLE_GLSLC") or find_tool(("glslc", "glslc.exe"), workspace)
    if not glslc_path:
        bundled_glslc = workspace / "vcpkg" / "installed" / "x64-windows" / "tools" / "shaderc" / "glslc.exe"
        if bundled_glslc.exists():
            glslc_path = str(bundled_glslc)

    checks = [
        CheckResult("cmake", cmake_path is not None, cmake_path or "not found"),
        CheckResult("msvc_cl", cl_path is not None, cl_path or "not found in PATH"),
        check_sdl_packages(workspace),
        check_manifest(workspace),
        check_variant_payloads(workspace),
        check_runtime_graph_bridge_free(workspace),
        CheckResult("dxc", dxc_path is not None, dxc_path or "not found (required for DXIL rebuilds)"),
        CheckResult("glslc", glslc_path is not None, glslc_path or "not found (required for SPIR-V rebuilds)"),
    ]

    hard_fail = False
    soft_fail = False
    for result in checks:
        prefix = "[OK] " if result.ok else "[FAIL] "
        print(f"{prefix}{result.name}: {result.detail}")
        if not result.ok and result.name in {"cmake", "sdl3_packages", "shader_manifest", "shader_payloads"}:
            hard_fail = True
        if not result.ok and result.name in {"dxc", "glslc"}:
            soft_fail = True

    if hard_fail:
        print("Preflight failed: required runtime/build dependencies are missing.")
        return 1
    if args.strict_tools and soft_fail:
        print("Preflight failed (strict tools): missing shader compiler toolchain.")
        return 1

    print("Preflight passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
