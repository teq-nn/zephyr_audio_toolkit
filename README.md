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
  `audio_wav.h` (reads *and* writes RIFF/WAVE headers; the only place that knows the byte layout),
  `audio_i2s_wire.h` (maps the canonical container to I2S wire words and back; shared by the I2S
  sink and the I2S source, so the two ends of a link cannot drift apart).
- `subsys/audio/pipeline/` – the implementation: `audio_pipeline_core.c`, `audio_pipeline_config.c`,
  `audio_pipeline_events.c`, `audio_node_core.c`, `audio_wav.c`, `audio_i2s_wire.c`, the private
  `audio_internal.h`, plus `nodes/` (file reader, file writer, gain filter, I2S input, I2S output,
  null sink, tone generator).
- `samples/audio/pipeline_basic/` – reference application (`CMakeLists.txt`, `Kconfig`, `src/main.c`).
- `tests/subsys/audio/pipeline/` – Ztest suites (`test_roundtrip.c`, `test_error_paths.c`); enables
  every shipped node.
- `tests/subsys/audio/no_file_nodes/` – the other end of the node selection range: only the gain
  filter and the null sink are built, and the suite checks that neither `CONFIG_FILE_SYSTEM` nor
  `CONFIG_I2S` reaches the generated configuration while a pipeline of the remaining nodes still
  runs.
- `tests/subsys/audio/wav/` – standalone WAV header unit test (`test_wav.c`), no pipeline needed.
- `tests/subsys/audio/i2s_wire/` – unit test for the container-to-wire seam the I2S nodes share
  (`test_i2s_wire.c`); pure arithmetic, so it runs on `native_sim` with no I2S device.
- `tests/subsys/audio/i2s_in_node/` – behaviour suite for the I2S input source, driven by a
  scriptable I2S device (`fake_i2s.c`) declared in the suite's own overlay and binding, so a read
  timeout, a driver failure and an RX overrun can be produced on `native_sim`. It is where the
  rule that a live source never reports end of stream, and that every block goes back to the slab,
  are actually checked.
- `tests/boards/nucleo_h723zg/i2s_smoke/` – board bring-up smoke test: two I2S blocks (i2s2 TX,
  i2s3 RX, both clock slaves) and the control I2C report ready. Its
  `boards/nucleo_h723zg.overlay` is the canonical board overlay for the hardware target — the
  audio pinout, the TX/RX block split and the DMA reachability constraint live there, and hardware
  work reuses it instead of restating it. Twister filters the suite out of `native_sim` runs.
- `tests/boards/nucleo_h723zg/i2s_out_node/` – the I2S output sink on the same target. Its overlay
  includes the smoke test's rather than copying it. Build-time assertions cover the transfer
  blocks' DMA reachability and cache alignment, the clock role and the block-size arithmetic; the
  run-time cases open and close the chain, which needs no external clock. No frame is pulled — a
  clock target only advances while the codec clocks it.
- `tests/boards/nucleo_h723zg/i2s_in_node/` – the same for the I2S input source on `i2s3`. No frame
  is pulled here either: a source that is a clock target would block in `i2s_read()` until a master
  appears, so the node's behaviour is covered by the `native_sim` suite instead.

Headers are installed under the `zephyr/audio/` namespace, so applications include them as
`#include <zephyr/audio/audio_pipeline.h>`.

## Using the module

The repository is a Zephyr module, not a standalone application. Make it visible to your workspace
in one of two ways:

- Add it to your west manifest (`west.yml`) as a project, so `west` registers it automatically, or
- Point the build at it explicitly: `-DZEPHYR_EXTRA_MODULES=<abs path to this repo>`.

Then enable it with `CONFIG_AUDIO_PIPELINE=y` in your application's `prj.conf`, plus one symbol per
node the application defines - they all default to `n`, so an image links the nodes it names and
nothing else:

| Symbol | Node | Notes |
| --- | --- | --- |
| `CONFIG_AUDIO_PIPELINE_NODE_FILE_READER` | `AUDIO_FILE_READER_NODE_DEFINE()` | selects `FILE_SYSTEM` |
| `CONFIG_AUDIO_PIPELINE_NODE_FILE_WRITER` | `AUDIO_FILE_WRITER_NODE_DEFINE()` | selects `FILE_SYSTEM` |
| `CONFIG_AUDIO_PIPELINE_NODE_GAIN_FILTER` | `AUDIO_GAIN_FILTER_NODE_DEFINE()` | |
| `CONFIG_AUDIO_PIPELINE_NODE_I2S_IN` | `AUDIO_I2S_IN_NODE_DEFINE()` | selects `I2S`; device from devicetree, slave only; a live source never reports EOF |
| `CONFIG_AUDIO_PIPELINE_NODE_I2S_OUT` | `AUDIO_I2S_OUT_NODE_DEFINE()` | selects `I2S`; device and clock role come from devicetree, slave only |
| `CONFIG_AUDIO_PIPELINE_NODE_NULL_SINK` | `AUDIO_NULL_SINK_NODE_DEFINE()` | |
| `CONFIG_AUDIO_PIPELINE_NODE_TONE_GEN` | `AUDIO_TONE_GEN_NODE_DEFINE()` | one tone per channel |

Using a `*_NODE_DEFINE()` macro whose symbol is off is a build error naming the symbol that fixes
it, so a missing line here is reported where it was made rather than at link time.

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
