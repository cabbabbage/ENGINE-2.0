#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
from typing import Any

DEFAULT_ROOM_SIZE = 9
DEFAULT_TRAIL_SIZE = 5
LEGACY_ROOM_KEYS = {
    "geometry",
    "radius",
    "min_radius",
    "max_radius",
    "width",
    "height",
    "min_width",
    "max_width",
    "min_height",
    "max_height",
}


def migrate_room_entry(entry: Any, default_size: int) -> bool:
    if not isinstance(entry, dict):
        return False
    changed = False
    if entry.get("size") != default_size:
        entry["size"] = default_size
        changed = True
    for key in LEGACY_ROOM_KEYS:
        if key in entry:
            del entry[key]
            changed = True
    return changed


def migrate_file(path: Path) -> tuple[bool, int]:
    try:
        original_text = path.read_text(encoding="utf-8")
        data = json.loads(original_text)
    except Exception:
        return False, 0

    if not isinstance(data, dict):
        return False, 0

    changed = False
    migrated_entries = 0
    for section_name, default_size in (("rooms_data", DEFAULT_ROOM_SIZE), ("trails_data", DEFAULT_TRAIL_SIZE)):
        section = data.get(section_name)
        if not isinstance(section, dict):
            continue
        for _, room_entry in section.items():
            if migrate_room_entry(room_entry, default_size):
                changed = True
            if isinstance(room_entry, dict):
                migrated_entries += 1

    if not changed:
        return False, migrated_entries

    updated_text = json.dumps(data, indent=2, ensure_ascii=False) + "\n"
    path.write_text(updated_text, encoding="utf-8")
    return True, migrated_entries


def iter_json_files(roots: list[Path]):
    for root in roots:
        if root.is_file() and root.suffix.lower() == ".json":
            yield root
            continue
        if root.is_dir():
            yield from root.rglob("*.json")


def main() -> int:
    parser = argparse.ArgumentParser(description="Migrate room/trail geometry fields to a single size field.")
    parser.add_argument(
        "paths",
        nargs="*",
        default=["content", "ENGINE"],
        help="Files or directories to scan recursively for JSON map data.",
    )
    args = parser.parse_args()

    roots = [Path(p).resolve() for p in args.paths]
    scanned = 0
    changed_files = 0
    migrated_entries = 0

    for json_file in iter_json_files(roots):
        scanned += 1
        changed, entry_count = migrate_file(json_file)
        migrated_entries += entry_count
        if changed:
            changed_files += 1
            print(f"updated: {json_file}")

    print(
        f"summary: scanned_files={scanned} changed_files={changed_files} migrated_room_entries={migrated_entries}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
