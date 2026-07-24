#!/usr/bin/env python3
"""Scan native_sim logs for common crash and warning signatures.

Example:
  python native_log_scan.py --log build.log
"""

from __future__ import annotations

import argparse
from pathlib import Path

DEFAULT_PATTERNS = [
    "ASSERTION FAIL",
    "Kernel panic",
    "Segmentation fault",
    "Hard fault",
    "stack overflow",
    "z_fatal_error",
    "ERROR",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Scan native_sim logs for failure signatures")
    parser.add_argument("--log", required=True, help="Path to log file")
    parser.add_argument(
        "--pattern",
        action="append",
        default=[],
        help="Additional case-insensitive pattern to search for",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    log_path = Path(args.log)
    if not log_path.exists():
        print(f"error: file not found: {log_path}")
        return 2

    lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
    patterns = DEFAULT_PATTERNS + args.pattern

    hits: list[tuple[str, int, str]] = []
    for idx, line in enumerate(lines, start=1):
        low = line.lower()
        for pattern in patterns:
            if pattern.lower() in low:
                hits.append((pattern, idx, line.strip()))
                break

    if not hits:
        print(f"No failure signatures found in {log_path} ({len(lines)} lines scanned).")
        return 0

    print(f"Potential failure signatures in {log_path}:")
    for pattern, line_no, text in hits:
        print(f"- line {line_no} [{pattern}]: {text}")

    return 1


if __name__ == "__main__":
    raise SystemExit(main())
