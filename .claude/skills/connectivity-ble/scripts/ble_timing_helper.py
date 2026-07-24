#!/usr/bin/env python3
"""Compute BLE timing units for Zephyr configuration work.

Examples:
  python ble_timing_helper.py --adv-ms 100
  python ble_timing_helper.py --conn-ms 30 --conn-ms 50
"""

from __future__ import annotations

import argparse

ADV_STEP_MS = 0.625
ADV_MIN_MS = 20.0
ADV_MAX_MS = 10240.0

CONN_STEP_MS = 1.25
CONN_MIN_MS = 7.5
CONN_MAX_MS = 4000.0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="BLE timing conversion helper")
    parser.add_argument("--adv-ms", type=float, help="Advertising interval in ms")
    parser.add_argument(
        "--conn-ms",
        type=float,
        action="append",
        default=[],
        help="Connection interval in ms (can be passed twice for min/max)",
    )
    return parser.parse_args()


def to_units(ms: float, step_ms: float) -> int:
    return int(round(ms / step_ms))


def main() -> int:
    args = parse_args()

    if args.adv_ms is None and not args.conn_ms:
        print("Provide at least one of --adv-ms or --conn-ms")
        return 2

    if args.adv_ms is not None:
        adv_ms = args.adv_ms
        adv_units = to_units(adv_ms, ADV_STEP_MS)
        in_range = ADV_MIN_MS <= adv_ms <= ADV_MAX_MS
        print("Advertising interval")
        print(f"  ms: {adv_ms}")
        print(f"  units (0.625 ms): {adv_units}")
        print(f"  in spec range [{ADV_MIN_MS}, {ADV_MAX_MS}]: {in_range}")

    if args.conn_ms:
        print("Connection interval(s)")
        for ms in args.conn_ms:
            units = to_units(ms, CONN_STEP_MS)
            in_range = CONN_MIN_MS <= ms <= CONN_MAX_MS
            print(f"  ms: {ms}, units (1.25 ms): {units}, in range: {in_range}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
