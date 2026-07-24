#!/usr/bin/env python3
"""Estimate average current and battery life from state duty-cycle data.

CSV format:
  state,current_ma,duty_cycle_percent
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


class InputError(Exception):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Estimate battery life from power states")
    parser.add_argument("--csv", required=True, help="Path to state budget CSV")
    parser.add_argument(
        "--battery-mah",
        type=float,
        required=True,
        help="Battery capacity in mAh",
    )
    return parser.parse_args()


def load_rows(path: Path) -> list[tuple[str, float, float]]:
    if not path.exists():
        raise InputError(f"file not found: {path}")

    rows: list[tuple[str, float, float]] = []
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        required = {"state", "current_ma", "duty_cycle_percent"}
        missing = required - set(reader.fieldnames or [])
        if missing:
            raise InputError(f"missing CSV columns: {', '.join(sorted(missing))}")

        for line_no, row in enumerate(reader, start=2):
            state = (row.get("state") or "").strip()
            if not state:
                raise InputError(f"line {line_no}: empty state")

            try:
                current = float((row.get("current_ma") or "").strip())
                duty = float((row.get("duty_cycle_percent") or "").strip())
            except ValueError as exc:
                raise InputError(f"line {line_no}: invalid numeric value") from exc

            if current < 0:
                raise InputError(f"line {line_no}: current_ma must be >= 0")
            if duty < 0 or duty > 100:
                raise InputError(f"line {line_no}: duty_cycle_percent must be in [0, 100]")

            rows.append((state, current, duty))

    return rows


def main() -> int:
    args = parse_args()

    try:
        rows = load_rows(Path(args.csv))
    except InputError as exc:
        print(f"error: {exc}")
        return 2

    duty_sum = sum(duty for _, _, duty in rows)
    if duty_sum <= 0:
        print("error: total duty cycle must be > 0")
        return 2

    average_current = sum(current * (duty / 100.0) for _, current, duty in rows)
    if average_current <= 0:
        print("error: average current must be > 0")
        return 2

    hours = args.battery_mah / average_current
    days = hours / 24.0

    print("State contributions:")
    for state, current, duty in rows:
        contribution = current * (duty / 100.0)
        print(f"  {state}: {contribution:.3f} mA ({duty:.2f}% @ {current:.3f} mA)")

    print(f"\nDuty total: {duty_sum:.2f}%")
    print(f"Average current: {average_current:.3f} mA")
    print(f"Estimated battery life: {hours:.2f} h ({days:.2f} days)")

    if abs(duty_sum - 100.0) > 0.5:
        print("warning: duty-cycle total is not near 100%; verify measurement model")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
