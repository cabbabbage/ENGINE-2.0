#!/usr/bin/env python3
"""OpenGL runtime preflight checks for reproducible local validation."""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path


@dataclass
class CheckResult:
    name: str
    ok: bool
    detail: str


def check_manifest(workspace: Path) -> CheckResult:
    manifest_path = workspace / "ENGINE" / "runtime" / "rendering" / "shaders" / "runtime_shaders.json"
    if not manifest_path.exists():
        return CheckResult("shader_manifest", False, f"missing {manifest_path}")

    try:
        root = json.loads(manifest_path.read_text(encoding="utf-8"))
    except Exception as exc:  # pragma: no cover
        return CheckResult("shader_manifest", False, f"invalid JSON: {exc}")

    variants = root.get("variants")
    if not isinstance(variants, dict):
        return CheckResult("shader_manifest", False, "manifest 'variants' must be an object")

    return CheckResult("shader_manifest", True, f"{len(variants)} variants")


def check_opengl_runtime_sources(workspace: Path) -> CheckResult:
    runtime_header = workspace / "ENGINE" / "runtime" / "rendering" / "render" / "opengl_runtime_renderer.hpp"
    if not runtime_header.exists():
        return CheckResult("opengl_runtime", False, f"missing {runtime_header}")
    return CheckResult("opengl_runtime", True, "OpenGLRuntimeRenderer source present")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workspace", default=".")
    args = parser.parse_args()
    workspace = Path(args.workspace).resolve()

    checks = [check_manifest(workspace), check_opengl_runtime_sources(workspace)]
    failed = False
    for result in checks:
        print(f"{'[OK]' if result.ok else '[FAIL]'} {result.name}: {result.detail}")
        failed = failed or not result.ok

    if failed:
        print("Preflight failed.")
        return 1
    print("Preflight passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
