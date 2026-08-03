# Zephyr Audio Toolkit Module

This repository houses an out-of-tree Zephyr module (`zephyr-audio-pipeline`) that provides a
pull-based audio pipeline subsystem: sources, filters, and sinks chained behind a single worker
thread, with static memory only. Architectural intent lives alongside the code so the module can
evolve without drifting from the authored contract.

New here? Read `audio_pipeline_spec_short.md` first — it is the one-page digest of the contract.

## Layout

The tree follows the layout prescribed by manifest §12 and spec §14.

- `audio_pipeline_manifest.md` – the architectural contract; update it first when changing thread,
  buffer, or role semantics.
- `audio_pipeline_spec_v2.md` – the implementation blueprint (API shapes, Kconfig, EOF/error rules).
- `audio_pipeline_spec_short.md` – the onboarding digest of the two documents above; refresh it
  whenever the manifest or spec changes materially.
- `AGENTS.md` – contributor and agent guidelines (structure, style, testing, review).
- `zephyr/module.yml` – Zephyr module metadata; points the build at the root `CMakeLists.txt` and `Kconfig`. Zephyr only discovers modules via `zephyr/module.{yml,yaml}`, so this path is not optional.
- `CMakeLists.txt` – module root; adds `subsys/audio/pipeline`.
- `Kconfig` – module root menu; `rsource`s the subsystem Kconfig.
- `include/zephyr/audio/` – public headers other Zephyr applications include:
  `audio_format.h`, `audio_node.h`, `audio_nodes.h`, `audio_pipeline.h`, `audio_pipeline_events.h`,
  `audio_wav.h` (reads *and* writes RIFF/WAVE headers; the only place that knows the byte layout).
- `subsys/audio/pipeline/` – the implementation: `audio_pipeline_core.c`, `audio_pipeline_config.c`,
  `audio_pipeline_events.c`, `audio_node_core.c`, `audio_wav.c`, the private `audio_internal.h`,
  plus `nodes/` (file reader, file writer, gain filter, null sink).
- `samples/audio/pipeline_basic/` – reference application (`CMakeLists.txt`, `Kconfig`, `src/main.c`).
- `tests/subsys/audio/pipeline/` – Ztest suites (`test_roundtrip.c`, `test_error_paths.c`).
- `tests/subsys/audio/wav/` – standalone WAV header unit test (`test_wav.c`), no pipeline needed.
- `tests/boards/nucleo_h723zg/i2s_smoke/` – board bring-up smoke test: two I2S blocks (i2s2 TX,
  i2s3 RX, both clock slaves) and the control I2C report ready. Its
  `boards/nucleo_h723zg.overlay` is the canonical board overlay for the hardware target — the
  audio pinout, the TX/RX block split and the DMA reachability constraint live there, and hardware
  work reuses it instead of restating it. Twister filters the suite out of `native_sim` runs.

Headers are installed under the `zephyr/audio/` namespace, so applications include them as
`#include <zephyr/audio/audio_pipeline.h>`.

## Using the module

The repository is a Zephyr module, not a standalone application. Make it visible to your workspace
in one of two ways:

- Add it to your west manifest (`west.yml`) as a project, so `west` registers it automatically, or
- Point the build at it explicitly: `-DZEPHYR_EXTRA_MODULES=<abs path to this repo>`.

Then enable it with `CONFIG_AUDIO_PIPELINE=y` in your application's `prj.conf`.

## Build & Test

Commands below assume you are at the repository root inside an initialised west workspace.

```sh
# Build the sample for the host simulator
west build -b native_sim -d build/sample samples/audio/pipeline_basic \
    -- -DZEPHYR_EXTRA_MODULES=$PWD

# Same sample on hardware, then flash it
west build -b nrf5340_audio_dk/nrf5340/cpuapp -d build/hw samples/audio/pipeline_basic \
    -- -DZEPHYR_EXTRA_MODULES=$PWD
west flash -d build/hw

# Run the test suites headlessly
west twister -T tests -p native_sim -x=ZEPHYR_EXTRA_MODULES=$PWD

# Board bring-up: build the smoke test for the hardware target...
west twister -T tests -p nucleo_h723zg
# ...and run it on an attached board
west twister -T tests -p nucleo_h723zg --device-testing --hardware-map map.yaml
```

`west.yml` pins Zephyr and clones only `picolibc`, `hal_stm32` and `cmsis_6`, which is exactly what
the `native_sim` and `nucleo_h723zg` targets need. A workspace made with `west init -l` against this
repository builds both without anything added by hand.

If the module is registered in the west manifest, the `-DZEPHYR_EXTRA_MODULES` / `-x` arguments can
be dropped. Swap `-p native_sim` for `-p <BOARD>` when coverage must include driver-backed sinks.

## Contributing

Follow Zephyr's K&R style (tabs, snake_case, braces on the same line) and run `checkpatch.pl
--strict` before submitting patches that touch code. Keep the manifest, spec, and digest
synchronized with any code change — see `AGENTS.md` for the full guidelines.
