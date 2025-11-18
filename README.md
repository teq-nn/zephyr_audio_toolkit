# Zephyr Audio Toolkit Module

This repository houses an out-of-tree module that plugs into Zephyr builds, providing reusable audio pipeline components. Architectural intent lives alongside the code so the module can evolve without drifting from the authored contract.

## Layout

- `audio_pipeline_manifest.md`, `audio_pipeline_spec_v2.md`, `audio_pipeline_spec_short.md` – canonical design docs; update the manifest first when changing thread, buffer, or role semantics, then mirror API edits in the spec and digest.
- `zephyr/` – integration glue for Zephyr (e.g., `module.yml`, Kconfig fragments, top-level `CMakeLists.txt`).
- `src/` – Zephyr sources for pipeline nodes, helpers, and runtime glue.
- `include/audio_pipeline/` – public headers that other Zephyr applications ingest.
- `tests/` – Twister scenarios plus fixtures; follow the `test_<role>_<behavior>` naming convention.
- `samples/` – reference applications or smoke tests runnable with `west build` and `west flash`.
- `doc/` – supplementary documentation beyond the manifest/spec trio.

## Build & Test

Common commands once sources land:

- `west build -b nrf5340_audio src` – build the module against Zephyr (swap `-b` per target).
- `west flash` – deploy onto hardware after host-based validation.
- `west twister -T tests` – run headless test suites; add `-p <BOARD>` for driver-backed coverage.

Follow Zephyr’s K&R style (tabs, snake_case, braces on same line) and run `checkpatch.pl --strict` and `clang-format -style=file` before submitting patches that touch code. Keep specs, manifest, and docs synchronized with any code changes.
