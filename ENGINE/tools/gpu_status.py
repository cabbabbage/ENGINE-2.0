#!/usr/bin/env python3
"""Shared GPU status helpers for tools."""

from __future__ import annotations


def detect_torch_gpu() -> bool:
    try:
        import torch

        return bool(torch.cuda.is_available())
    except Exception:
        return False


def print_gpu_status(enabled: bool) -> None:
    status = "GPU ACCELERATION: ENABLED" if enabled else "GPU ACCELERATION: DISABLED"
    print(status, flush=True)
