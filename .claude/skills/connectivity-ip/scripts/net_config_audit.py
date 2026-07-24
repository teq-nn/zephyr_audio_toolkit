#!/usr/bin/env python3
"""Audit Zephyr networking Kconfig selections for common IP profiles.

Example:
  python net_config_audit.py --conf prj.conf
"""

from __future__ import annotations

import argparse
from pathlib import Path

KEYS = [
    "CONFIG_NETWORKING",
    "CONFIG_NET_IPV4",
    "CONFIG_NET_IPV6",
    "CONFIG_NET_UDP",
    "CONFIG_NET_TCP",
    "CONFIG_COAP",
    "CONFIG_MQTT_LIB",
    "CONFIG_LWM2M",
    "CONFIG_DNS_RESOLVER",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Audit Zephyr network config")
    parser.add_argument("--conf", required=True, help="Path to prj.conf-style file")
    return parser.parse_args()


def load_flags(conf_path: Path) -> dict[str, str]:
    flags: dict[str, str] = {}
    for raw in conf_path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        flags[key.strip()] = value.strip()
    return flags


def enabled(flags: dict[str, str], key: str) -> bool:
    return flags.get(key) == "y"


def main() -> int:
    args = parse_args()
    conf_path = Path(args.conf)
    if not conf_path.exists():
        print(f"error: file not found: {conf_path}")
        return 2

    flags = load_flags(conf_path)

    print("Selected network features:")
    for key in KEYS:
        state = "enabled" if enabled(flags, key) else "disabled"
        print(f"  {key}: {state}")

    print("\nProfile hints:")
    if enabled(flags, "CONFIG_COAP") and enabled(flags, "CONFIG_NET_UDP"):
        print("  - CoAP/UDP path is configured.")
    if enabled(flags, "CONFIG_MQTT_LIB") and enabled(flags, "CONFIG_NET_TCP"):
        print("  - MQTT/TCP path is configured.")
    if enabled(flags, "CONFIG_LWM2M"):
        print("  - LwM2M path is configured.")
    if enabled(flags, "CONFIG_NET_IPV4") and not enabled(flags, "CONFIG_NET_IPV6"):
        print("  - IPv4-only profile may reduce footprint.")
    if enabled(flags, "CONFIG_NET_IPV6") and not enabled(flags, "CONFIG_NET_IPV4"):
        print("  - IPv6-only profile selected.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
