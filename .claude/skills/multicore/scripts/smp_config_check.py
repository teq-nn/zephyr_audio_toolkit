#!/usr/bin/env python3
"""Check Zephyr SMP-related config consistency from a prj.conf file."""

from __future__ import annotations

import argparse
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate SMP-related Kconfig selections")
    parser.add_argument("--conf", required=True, help="Path to prj.conf-style file")
    return parser.parse_args()


def read_flags(path: Path) -> dict[str, str]:
    flags: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        flags[key.strip()] = value.strip()
    return flags


def main() -> int:
    args = parse_args()
    conf = Path(args.conf)
    if not conf.exists():
        print(f"error: file not found: {conf}")
        return 2

    flags = read_flags(conf)

    smp_enabled = flags.get("CONFIG_SMP") == "y"
    cpus = flags.get("CONFIG_MP_NUM_CPUS")

    print(f"CONFIG_SMP: {'enabled' if smp_enabled else 'disabled'}")
    print(f"CONFIG_MP_NUM_CPUS: {cpus if cpus is not None else 'unset'}")

    if smp_enabled and cpus is None:
        print("warning: CONFIG_SMP enabled without CONFIG_MP_NUM_CPUS explicitly set")
        return 1

    if smp_enabled and cpus is not None:
        try:
            ncpu = int(cpus)
        except ValueError:
            print(f"error: CONFIG_MP_NUM_CPUS must be integer, got '{cpus}'")
            return 2

        if ncpu < 2:
            print("warning: CONFIG_SMP enabled but CONFIG_MP_NUM_CPUS < 2")
            return 1

    print("SMP config check completed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
