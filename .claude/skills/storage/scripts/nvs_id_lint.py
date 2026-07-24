#!/usr/bin/env python3
"""Detect duplicate NVS ID macro values across source/header files.

Looks for patterns like:
  #define NVS_ID_FOO 0x0001
"""

from __future__ import annotations

import argparse
import re
from collections import defaultdict
from pathlib import Path

DEFINE_RE = re.compile(
    r"^\s*#\s*define\s+(NVS_ID_[A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+|\d+)\b"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Find duplicate NVS ID values")
    parser.add_argument(
        "paths",
        nargs="+",
        help="Files or directories to scan recursively (.h/.c/.hpp/.cpp)",
    )
    return parser.parse_args()


def iter_files(paths: list[str]) -> list[Path]:
    files: list[Path] = []
    allowed = {".h", ".c", ".hpp", ".cpp"}

    for raw in paths:
        path = Path(raw)
        if path.is_file() and path.suffix in allowed:
            files.append(path)
            continue

        if path.is_dir():
            for candidate in path.rglob("*"):
                if candidate.is_file() and candidate.suffix in allowed:
                    files.append(candidate)

    return sorted(set(files))


def parse_value(text: str) -> int:
    return int(text, 16) if text.lower().startswith("0x") else int(text, 10)


def main() -> int:
    args = parse_args()
    files = iter_files(args.paths)

    if not files:
        print("No matching source files found.")
        return 2

    by_value: dict[int, list[tuple[str, Path, int]]] = defaultdict(list)

    for file_path in files:
        for line_no, line in enumerate(
            file_path.read_text(encoding="utf-8", errors="replace").splitlines(),
            start=1,
        ):
            match = DEFINE_RE.match(line)
            if not match:
                continue

            name = match.group(1)
            value = parse_value(match.group(2))
            by_value[value].append((name, file_path, line_no))

    duplicates = {value: entries for value, entries in by_value.items() if len(entries) > 1}

    if not duplicates:
        print(f"No duplicate NVS IDs found in {len(files)} files.")
        return 0

    print("Duplicate NVS ID values found:")
    for value in sorted(duplicates):
        print(f"\n0x{value:04X}")
        for name, file_path, line_no in duplicates[value]:
            print(f"  {name} -> {file_path}:{line_no}")

    return 1


if __name__ == "__main__":
    raise SystemExit(main())
