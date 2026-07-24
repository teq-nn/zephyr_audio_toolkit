#!/usr/bin/env python3
"""Lint Zephyr Zbus channel declarations for duplicate names.

Searches C/H files for `ZBUS_CHAN_DEFINE(name, ...)` and reports duplicates.
"""

from __future__ import annotations

import argparse
import re
from collections import defaultdict
from pathlib import Path

PATTERN = re.compile(r"\bZBUS_CHAN_DEFINE\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Detect duplicate Zbus channel names")
    parser.add_argument(
        "paths",
        nargs="+",
        help="Files or directories to scan (.c/.h/.cpp/.hpp)",
    )
    return parser.parse_args()


def iter_files(inputs: list[str]) -> list[Path]:
    allowed = {".c", ".h", ".cpp", ".hpp"}
    files: list[Path] = []

    for raw in inputs:
        path = Path(raw)
        if path.is_file() and path.suffix in allowed:
            files.append(path)
            continue
        if path.is_dir():
            for candidate in path.rglob("*"):
                if candidate.is_file() and candidate.suffix in allowed:
                    files.append(candidate)

    return sorted(set(files))


def main() -> int:
    args = parse_args()
    files = iter_files(args.paths)

    if not files:
        print("No supported source files found.")
        return 2

    found: dict[str, list[tuple[Path, int]]] = defaultdict(list)

    for file_path in files:
        lines = file_path.read_text(encoding="utf-8", errors="replace").splitlines()
        for line_no, line in enumerate(lines, start=1):
            match = PATTERN.search(line)
            if not match:
                continue
            found[match.group(1)].append((file_path, line_no))

    duplicates = {name: refs for name, refs in found.items() if len(refs) > 1}

    if not duplicates:
        print(f"No duplicate Zbus channel names found in {len(files)} files.")
        return 0

    print("Duplicate Zbus channel names detected:")
    for name in sorted(duplicates):
        print(f"- {name}")
        for file_path, line_no in duplicates[name]:
            print(f"  {file_path}:{line_no}")

    return 1


if __name__ == "__main__":
    raise SystemExit(main())
