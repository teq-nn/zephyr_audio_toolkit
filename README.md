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
- `zephyr/module.yml` – Zephyr module metadata; points the build at the root `CMakeLists.txt` and `Kconfig`, and sets `dts_root: .` so `dts/bindings/` is searched by every consumer. Zephyr only discovers modules via `zephyr/module.{yml,yaml}`, so this path is not optional.
- `dts/boards/nucleo_h723zg.overlay` – the canonical board overlay for the hardware target: the
  audio pinout, the TX/RX block split, the clock roles, the AK4619 codec on the control I2C bus and
  the DMA reachability constraint. It lives outside `tests/` and `samples/` so both can include it
  without reaching into the other; each consumer's own `boards/nucleo_h723zg.overlay` is a
  one-line `#include` of it. The wiring it describes is worked out in
  `docs/hardware/akd4619-evaluation-board.md`.
- `dts/bindings/audio/asahi-kasei,ak4619.yaml` – binding for the codec node in that overlay: the
  I2C control interface, the register map's shape, the address derivation, and why there is no
  reset GPIO. It stays here rather than moving under the sample because the board suites include
  the same overlay and would otherwise see an unbound node.
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
  null sink, tone analyzer, tone generator).
- `samples/audio/pipeline_basic/` – reference application (`CMakeLists.txt`, `Kconfig`, `src/main.c`).
- `samples/audio/ak4619_loopback/` – the AKM AK4619 codec on the `nucleo_h723zg` target, and the
  only place a codec driver exists in this tree. `drivers/ak4619.{c,h}` is a real Zephyr
  `audio_codec` driver (`DEVICE_DT_INST_DEFINE`, `<zephyr/audio/codec.h>`) that ships with the
  sample rather than with the toolkit, so nothing under `include/zephyr/audio/` or `subsys/audio/`
  gains a codec dependency and no `AUDIO_PIPELINE_*` symbol mentions it; `drivers/Kconfig` carries
  its own `AK4619_*` symbols. `src/main.c` reports on the console whether the part is really
  answering. Its `boards/nucleo_h723zg.overlay` includes the canonical overlay.
- `tests/samples/audio/ak4619_loopback/` – the codec driver on `native_sim`, against an emulated
  AK4619 (`src/ak4619_emul.c`) that models the register file and addressing rules of the datasheet
  and can be told to misbehave. It covers the reset, the register access and the write/read/verify
  link check, including the cases where the bus ACKs and nothing latches, and where the part is
  absent altogether. No hardware and no I2C peripheral.
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
- `tests/boards/nucleo_h723zg/i2s_smoke/` – board bring-up smoke test: two I2S blocks (i2s2 TX and
  the clock source, i2s3 RX and a clock target) and the control I2C report ready. Its
  `boards/nucleo_h723zg.overlay` includes `dts/boards/nucleo_h723zg.overlay` rather than restating
  it. Twister filters the suite out of `native_sim` runs.
- `tests/boards/nucleo_h723zg/i2s_out_node/` – the I2S output sink on the same target. Its overlay
  includes the canonical one too. Build-time assertions cover the transfer blocks' DMA
  reachability and cache alignment, the clock role and the block-size arithmetic; the run-time
  cases open and close the chain, which needs no external clock. No frame is pulled — the node
  still configures itself as a clock target, so nothing advances until the loopback application
  makes it a controller.
- `tests/boards/nucleo_h723zg/i2s_in_node/` – the same for the I2S input source on `i2s3`. No frame
  is pulled here either: a source that is a clock target blocks in `i2s_read()` until the transmit
  block clocks it, so the node's behaviour is covered by the `native_sim` suite instead.

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
| `CONFIG_AUDIO_PIPELINE_NODE_TONE_ANALYZER` | `AUDIO_TONE_ANALYZER_NODE_DEFINE()` | one expected tone per channel; verdict read with `audio_tone_analyzer_get_result()` |
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

# The AK4619 codec bring-up image, and what CI builds for the hardware target
west build -b nucleo_h723zg -d build/ak4619 samples/audio/ak4619_loopback \
    -- -DZEPHYR_EXTRA_MODULES=$PWD
west flash -d build/ak4619
CI_TEST_PLATFORM=nucleo_h723zg CI_TEST_PATH=samples ./scripts/ci-test.sh
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
