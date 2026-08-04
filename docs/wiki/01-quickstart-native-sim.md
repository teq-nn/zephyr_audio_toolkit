# Quick Start: run the sample on your host

Goal: a pipeline running on `native_sim`, playing a real WAV file through a gain filter,
without any audio hardware. Roughly ten minutes, most of it spent cloning Zephyr.

> **Next:** [Quick Start: your first pipeline](02-quickstart-first-pipeline.md) builds the
> same thing from scratch in your own application.

## 1. Prerequisites

* Python 3, `pip`, `cmake >= 3.20.5`, `ninja`, and the Zephyr SDK host tools —
  see [Zephyr's getting started guide](https://docs.zephyrproject.org/latest/develop/getting_started/).
* `native_sim` compiles with the **host** gcc, so no cross toolchain is required.
* The default `native_sim` target is 32-bit and needs the 32-bit host C library:

  ```sh
  sudo apt-get install gcc-multilib g++-multilib libc6-dev-i386
  ```

  Or skip that and use the 64-bit variant everywhere: `-b native_sim/native/64`.

These are the prerequisites recorded in `scripts/ci-test.sh`, which is what CI runs.

## 2. Create a workspace

This repository can act as the west manifest repository. `west.yml` pins Zephyr (v4.4.1 at
the time of writing) and clones only `picolibc`, `hal_stm32` and `cmsis_6` — exactly what
the `native_sim` and `nucleo_h723zg` targets need.

```sh
pip install west
git clone https://github.com/teq-nn/zephyr_audio_toolkit
west init -l zephyr_audio_toolkit
west update
west packages pip --install      # Zephyr's own Python requirements
```

You end up with `zephyr/` next to `zephyr_audio_toolkit/` inside one workspace. Because
this repository is a west project there, Zephyr discovers the module on its own — no
`ZEPHYR_EXTRA_MODULES` needed.

*Already have a workspace?* Drop the repository in and either add it to your manifest, or
pass `-DZEPHYR_EXTRA_MODULES=<abs path to this repo>` on every build. Both are described
in [Troubleshooting → the module is invisible](09-troubleshooting.md#the-build-never-sees-the-module).

## 3. Build and run the sample

From the repository root:

```sh
west build -b native_sim -d build/sample samples/audio/pipeline_basic
west build -d build/sample -t run
```

(Add `-- -DZEPHYR_EXTRA_MODULES=$PWD` to the first command if the module is not a west
project in your workspace.)

The sample is self-contained: it formats an ext2 filesystem on a RAM disk declared in
`app.overlay`, writes a short 16-bit stereo track to `/ram/track.wav`, and plays that
through a −6 dB gain filter into a null sink until the pipeline reports end of stream.

Expected output, modulo your logging configuration:

```
track: wrote 1920 payload bytes to /ram/track.wav
[00:00:00.000,000] <inf> audio_file_reader: /ram/track.wav: 48000 Hz, 2 ch, 16 bit, 1920 payload bytes
pipeline[callback]: EOF
pipeline[queue]: EOF
```

Two EOF lines, and that is the point: the same event reaches you twice, once through the
optional callback on the worker thread and once through the queue on your control thread.
See [Events](05-pipeline-lifecycle.md#events).

## 4. Run the test suites

```sh
./scripts/ci-test.sh
```

That script is the single place the test invocation is spelled out — CI runs exactly this,
so a local run cannot drift from a CI run. It wraps:

```sh
west twister -T tests -p native_sim --outdir twister-out --inline-logs --verbose
```

Useful variants:

```sh
CI_TEST_PLATFORM=native_sim/native/64 ./scripts/ci-test.sh
CI_TEST_PATH=tests/subsys/audio/pipeline ./scripts/ci-test.sh
./scripts/ci-test.sh --no-clean
```

The suites under `tests/subsys/` all run on `native_sim` with no audio hardware — the I2S
source suite brings its own scriptable fake I2S device. The suites under
`tests/boards/nucleo_h723zg/` are pinned to that board and are filtered out of a
`native_sim` run.

## 5. What to read next

* The sample's `src/main.c` is 260 well-commented lines and is the reference application;
  everything in it is explained in [Pipeline lifecycle](05-pipeline-lifecycle.md).
* Its `prj.conf` shows the shape of every application config: `CONFIG_AUDIO_PIPELINE=y`
  plus one symbol per node you define.

```conf
CONFIG_AUDIO_PIPELINE=y

CONFIG_AUDIO_PIPELINE_NODE_FILE_READER=y
CONFIG_AUDIO_PIPELINE_NODE_GAIN_FILTER=y
CONFIG_AUDIO_PIPELINE_NODE_NULL_SINK=y
```

Every node symbol defaults to `n`, so an image links the nodes you name and nothing else.
Using a `*_NODE_DEFINE()` macro whose symbol is off is a build error that names the symbol
to set — see [Troubleshooting → build errors](09-troubleshooting.md#build-errors).

## Building for real hardware

```sh
west build -b nrf5340_audio_dk/nrf5340/cpuapp -d build/hw samples/audio/pipeline_basic
west flash -d build/hw
```

The sample's `prepare_track()` exists only to give the file reader something to read on a
host; on a target with real storage, drop it and mount the filesystem that holds your file.
For I2S targets, start at [I2S and hardware bring-up](08-i2s-and-hardware.md).
