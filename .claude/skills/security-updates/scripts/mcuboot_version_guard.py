#!/usr/bin/env python3
"""Check monotonic firmware version progression for update pipelines.

Example:
  python mcuboot_version_guard.py --current 1.2.3 --candidate 1.2.4
"""

from __future__ import annotations

import argparse
import re

VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Enforce monotonic firmware versions")
    parser.add_argument("--current", required=True, help="Current deployed semantic version")
    parser.add_argument("--candidate", required=True, help="Candidate update semantic version")
    return parser.parse_args()


def parse_version(text: str) -> tuple[int, int, int]:
    match = VERSION_RE.match(text.strip())
    if not match:
        raise ValueError(f"invalid semantic version '{text}', expected MAJOR.MINOR.PATCH")
    return tuple(int(part) for part in match.groups())


def main() -> int:
    args = parse_args()

    try:
        current = parse_version(args.current)
        candidate = parse_version(args.candidate)
    except ValueError as exc:
        print(f"error: {exc}")
        return 2

    if candidate <= current:
        print(
            "Version guard failed: "
            f"candidate {args.candidate} must be greater than current {args.current}."
        )
        return 1

    print(
        "Version guard passed: "
        f"candidate {args.candidate} is newer than current {args.current}."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
