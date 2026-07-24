# Project Skills

Vendored agent skills for Claude Code (and compatible agents). Each subdirectory
contains one skill (`<name>/SKILL.md`). They are picked up automatically in
sessions started from this repository and can be invoked with `/<skill-name>`.

## Sources

- **Zephyr agent skills** — https://github.com/beriberikix/zephyr-agent-skills
  (Apache-2.0, see `LICENSE-zephyr-agent-skills`). Zephyr RTOS domain knowledge:
  `zephyr-foundations`, `zephyr-module`, `build-system`, `devicetree`,
  `kernel-basics`, `kernel-services`, `testing-debugging`, `native-sim`,
  `board-bringup`, `hardware-io`, `storage`, `multicore`, `power-performance`,
  `connectivity-ble`, `connectivity-ip`, `connectivity-usb-can`, `iot-protocols`,
  `industrial`, `security-updates`, `specialized`, `zephyr-index`.

- **Matt Pocock's skills** — https://github.com/mattpocock/skills
  (MIT, see `LICENSE-mattpocock-skills`). Engineering-process skills from the
  `engineering`, `productivity`, and `misc` categories: `implement`, `tdd`,
  `code-review`, `codebase-design`, `diagnosing-bugs`, `domain-modeling`,
  `research`, `prototype`, `to-spec`, `to-tickets`, `triage`, `wayfinder`,
  `grilling`, `grill-me`, `grill-with-docs`, `handoff`, `teach`,
  `writing-great-skills`, and others. The upstream `deprecated`, `in-progress`,
  and `personal` categories were not vendored.

To update, re-copy the skill directories from the upstream repositories.
