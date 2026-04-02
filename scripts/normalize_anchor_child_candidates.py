#!/usr/bin/env python3
"""Normalize anchor-point child candidates to canonical {name, chance} arrays.

Canonical shape:
  anchor_point_child_candidates: [
    {
      "anchor_point_name": "<anchor>",
      "candidates": {
        "candidates": [
          {"name": "<asset-or-#tag>", "chance": <number>}
        ]
      }
    }
  ]
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any

DEFAULT_MISSING_CHANCE = 100.0
EPSILON = 1e-9


def trim_text(value: str) -> str:
    return value.strip()


def ensure_tag_prefix(value: str) -> str:
    value = trim_text(value)
    if value and not value.startswith("#"):
        return "#" + value
    return value


def is_integral_number(value: float) -> bool:
    return math.isfinite(value) and abs(value - round(value)) <= EPSILON


def clamp_non_negative(value: float) -> float:
    if not math.isfinite(value) or value < 0.0:
        return 0.0
    return value


def parse_number_like(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        text = trim_text(value)
        if not text:
            return None
        try:
            return float(text)
        except ValueError:
            return None
    return None


def read_candidate_chance(candidate: dict[str, Any]) -> tuple[bool, float]:
    if "chance" in candidate:
        parsed = parse_number_like(candidate.get("chance"))
        return True, 0.0 if parsed is None else parsed
    if "weight" in candidate:
        parsed = parse_number_like(candidate.get("weight"))
        return True, 0.0 if parsed is None else parsed
    return False, 0.0


def normalize_candidate_name(candidate: Any, legacy_key: str = "") -> str:
    name = trim_text(legacy_key)
    explicit_tag_name = ""
    tag_flag = False

    if isinstance(candidate, dict):
        if isinstance(candidate.get("name"), str):
            name = trim_text(candidate["name"])
        elif isinstance(candidate.get("asset_name"), str):
            name = trim_text(candidate["asset_name"])

        if isinstance(candidate.get("tag_name"), str):
            explicit_tag_name = trim_text(candidate["tag_name"])

        tag_value = candidate.get("tag")
        if isinstance(tag_value, bool):
            tag_flag = tag_value
        elif isinstance(tag_value, str):
            explicit_tag_name = trim_text(tag_value)

    if explicit_tag_name:
        name = ensure_tag_prefix(explicit_tag_name)
    elif tag_flag and name:
        name = ensure_tag_prefix(name)

    return name


def normalize_single_candidate(candidate: Any, legacy_key: str = "") -> dict[str, Any]:
    name = normalize_candidate_name(candidate, legacy_key)

    had_explicit_weight = False
    chance = 0.0
    if isinstance(candidate, dict):
        had_explicit_weight, chance = read_candidate_chance(candidate)
    else:
        parsed = parse_number_like(candidate)
        if parsed is not None:
            had_explicit_weight = True
            chance = parsed

    if not had_explicit_weight:
        chance = DEFAULT_MISSING_CHANCE if name else 0.0

    chance = clamp_non_negative(chance)
    normalized = {"name": name if name else "null"}
    if is_integral_number(chance):
        normalized["chance"] = int(round(chance))
    else:
        normalized["chance"] = chance
    return normalized


def normalize_candidate_payload(payload: Any) -> dict[str, Any]:
    normalized_candidates: list[dict[str, Any]] = []

    if isinstance(payload, dict):
        nested = payload.get("candidates")
        if isinstance(nested, list):
            for entry in nested:
                normalized_candidates.append(normalize_single_candidate(entry))
        else:
            for key, value in payload.items():
                normalized_candidates.append(normalize_single_candidate(value, str(key)))
    elif isinstance(payload, list):
        for entry in payload:
            normalized_candidates.append(normalize_single_candidate(entry))
    elif payload is not None:
        normalized_candidates.append(normalize_single_candidate(payload))

    return {"candidates": normalized_candidates}


def normalize_anchor_entries(entries: Any) -> list[dict[str, Any]]:
    if not isinstance(entries, list):
        return []

    normalized_entries: list[dict[str, Any]] = []
    seen_anchor_names: set[str] = set()
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        anchor_name_raw = entry.get("anchor_point_name")
        if not isinstance(anchor_name_raw, str):
            continue
        anchor_name = trim_text(anchor_name_raw)
        if not anchor_name or anchor_name in seen_anchor_names:
            continue
        seen_anchor_names.add(anchor_name)

        normalized_entries.append(
            {
                "anchor_point_name": anchor_name,
                "candidates": normalize_candidate_payload(entry.get("candidates", {})),
            }
        )

    return normalized_entries


def normalize_manifest(manifest: dict[str, Any]) -> bool:
    assets = manifest.get("assets")
    if not isinstance(assets, dict):
        return False

    changed = False
    for asset in assets.values():
        if not isinstance(asset, dict):
            continue
        if "anchor_point_child_candidates" not in asset:
            continue

        before = asset.get("anchor_point_child_candidates")
        after = normalize_anchor_entries(before)
        if before != after:
            asset["anchor_point_child_candidates"] = after
            changed = True

    return changed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "manifest.json",
        help="Path to manifest.json (default: repo_root/manifest.json)",
    )
    args = parser.parse_args()

    manifest_path: Path = args.manifest
    if not manifest_path.exists():
        print(f"Manifest not found: {manifest_path}")
        return 1

    with manifest_path.open("r", encoding="utf-8") as fh:
        manifest = json.load(fh)

    if not isinstance(manifest, dict):
        print(f"Manifest root is not an object: {manifest_path}")
        return 1

    changed = normalize_manifest(manifest)

    if changed:
        with manifest_path.open("w", encoding="utf-8", newline="\n") as fh:
            json.dump(manifest, fh, indent=2)
            fh.write("\n")
        print(f"Normalized anchor child candidates: {manifest_path}")
    else:
        print(f"No anchor child candidate changes needed: {manifest_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

