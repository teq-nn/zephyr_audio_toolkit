#!/usr/bin/env python3
"""Summarize Zephyr runtime logs by level and module.

Example:
  python log_summary.py --input app.log
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

LOG_RE = re.compile(
    r"^\s*<(?P<level>dbg|inf|wrn|err)>\s*(?:(?P<module>[^:]+):)?",
    re.IGNORECASE,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Summarize Zephyr log output")
    parser.add_argument("--input", required=True, help="Path to log text file")
    parser.add_argument(
        "--top-modules",
        type=int,
        default=10,
        help="Number of top modules to print (default: 10)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    path = Path(args.input)
    if not path.exists():
        print(f"error: input file not found: {path}")
        return 2

    level_counts: Counter[str] = Counter()
    module_counts: Counter[str] = Counter()
    parsed = 0

    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = LOG_RE.match(line)
        if not match:
            continue

        parsed += 1
        level = match.group("level").lower()
        module = (match.group("module") or "unknown").strip()

        level_counts[level] += 1
        module_counts[module] += 1

    if parsed == 0:
        print("No Zephyr log lines were detected.")
        return 1

    print("Level counts:")
    for level in ("err", "wrn", "inf", "dbg"):
        print(f"  {level:>3}: {level_counts[level]}")

    print("\nTop modules:")
    for module, count in module_counts.most_common(max(args.top_modules, 1)):
        print(f"  {module}: {count}")

    print(f"\nParsed log lines: {parsed}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
