#!/usr/bin/env python3
"""Scan the workspace for forbidden axis/legacy markers."""

import re
import sys
from pathlib import Path


CANONICAL_ROOT = Path(__file__).resolve().parent.parent
EXCLUDED_DIRS = {
    ".git",
    "build",
    "cache",
    "release",
    "vcpkg",
    "vcpkg_installed",
    ".claude",
}
SCAN_EXTENSIONS = {
    ".cpp",
    ".cc",
    ".cxx",
    ".c",
    ".hpp",
    ".hh",
    ".h",
    ".md",
    ".txt",
    ".py",
}

PATTERNS = [
    (
        "pattern-1",
        re.compile(r"\bZ\s+is\s+height\b", re.IGNORECASE),
    ),
    (
        "pattern-2",
        re.compile(r"\bworld_z\b.*\bheight\b", re.IGNORECASE),
    ),
    (
        "pattern-3",
        re.compile(r"\bheight\b.*\bworld_z\b", re.IGNORECASE),
    ),
]


def should_skip(path: Path) -> bool:
    for part in path.parts:
        if part in EXCLUDED_DIRS:
            return True
    return False


def scan() -> list[tuple[Path, int, str, str]]:
    matches = []
    for candidate in CANONICAL_ROOT.rglob("*"):
        if not candidate.is_file():
            continue
        if candidate.suffix.lower() not in SCAN_EXTENSIONS:
            continue
        rel = candidate.relative_to(CANONICAL_ROOT)
        if should_skip(rel):
            continue
        try:
            text = candidate.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for lineno, line in enumerate(text.splitlines(), start=1):
            stripped = line.strip()
            for label, pattern in PATTERNS:
                if pattern.search(line):
                    matches.append((rel, lineno, label, stripped))
    return matches


def main() -> int:
    matches = scan()
    if matches:
        print("Legacy axis marker scan failed. Remove or reword the following occurrences:\n")
        for path, lineno, label, snippet in matches:
            print(f"{path}:{lineno} [{label}] {snippet}")
        print()
        print("Run this script after any edits to ensure new code avoids the banned markers.")
        return 1

    print("Legacy axis marker scan passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
