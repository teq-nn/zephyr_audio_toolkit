# Zephyr Audio Toolkit Module

This repository houses an out-of-tree Zephyr module (`zephyr-audio-pipeline`) that provides a
pull-based audio pipeline subsystem: sources, filters, and sinks chained behind a single worker
thread, with static memory only.

## Concepts

A **pipeline** owns one worker thread and one frame buffer. Hanging off it is a chain of **nodes**,
each with a role: a **source** produces samples, a **filter** transforms them, a **sink** consumes
them.

The **sink** drives. It asks its upstream for a **frame** of samples, that node asks *its* upstream,
and so on back to the source — so data is pulled through the chain, never pushed into it. Nothing is
buffered between nodes and nothing is queued, which is what lets the whole pipeline share a single
statically allocated frame buffer. Nodes are passive: they run only when the worker thread invokes
them, one frame at a time, so none of them needs to be thread-safe.

A source with nothing left reports **end of stream** — a success, not an error — and the worker goes
idle without tearing anything down, ready to play another track on the same thread.

Every sample travels in a 32-bit **container**, whatever its original resolution; a source widens
what it reads on the way in and a sink narrows it on the way out.

`CONTEXT.md` defines these terms precisely and is the vocabulary to use when writing code here.
`docs/adr/` records why the design is shaped this way.

## Where to start reading

The public headers in `include/zephyr/audio/` are the specification — start with
`audio_pipeline.h` for the lifecycle and `audio_node.h` for what a node must implement. Then read
`samples/audio/pipeline_basic/src/main.c`, which builds a complete reader → gain → sink pipeline and
is the shortest end-to-end example of the API. The Ztest suites under `tests/` are the executable
contract for the behaviours the headers describe.

There is deliberately no separate specification document: the code is the spec, and a prose copy of
it would only drift. See `AGENTS.md` for the reasoning and for the contributor guidelines.

## Using the module

The repository is a Zephyr module, not a standalone application. Make it visible to your workspace
in one of two ways:

- Add it to your west manifest (`west.yml`) as a project, so `west` registers it automatically, or
- Point the build at it explicitly: `-DZEPHYR_EXTRA_MODULES=<abs path to this repo>`.

Then enable it with `CONFIG_AUDIO_PIPELINE=y` in your application's `prj.conf`.

Headers are installed under the `zephyr/audio/` namespace, so applications include them as
`#include <zephyr/audio/audio_pipeline.h>`.

Zephyr only discovers modules through `zephyr/module.{yml,yaml}`, so that path is not optional; it
points the build at the root `CMakeLists.txt` and `Kconfig`.

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
```

If the module is registered in the west manifest, the `-DZEPHYR_EXTRA_MODULES` / `-x` arguments can
be dropped. Swap `-p native_sim` for `-p <BOARD>` when coverage must include driver-backed sinks.

## Contributing

Follow Zephyr's K&R style (tabs, snake_case, braces on the same line) and run `checkpatch.pl
--strict` before submitting patches that touch code. See `AGENTS.md` for the full guidelines.
