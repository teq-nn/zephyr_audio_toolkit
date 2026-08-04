# Zephyr Audio Toolkit — Wiki

A pull-based audio pipeline for Zephyr: sources, filters and sinks chained behind a
single worker thread, with static memory only and no `k_malloc()` anywhere.

This wiki is the *task-oriented* entry point. It is written for someone who has never
used the module and wants a running pipeline today, and it explains the concepts and the
architectural decisions well enough that the code stops being surprising.

Everything here was derived from the source under `include/zephyr/audio/` and
`subsys/audio/pipeline/`. Where this wiki and a comment disagree, the code wins — and the
wiki is the thing that needs fixing.

## Read in this order

| # | Article | What you get |
| --- | --- | --- |
| 1 | [Quick Start: run the sample](01-quickstart-native-sim.md) | A workspace, a build and a pipeline running on your host in ~10 minutes |
| 2 | [Quick Start: your first pipeline](02-quickstart-first-pipeline.md) | Your own application, from `prj.conf` to EOF, with no hardware and no filesystem |
| 3 | [Core concepts](03-core-concepts.md) | Pull model, frames, the sample container, the format contract, EOF vs. error |
| 4 | [Architecture and why it is shaped this way](04-architecture.md) | The decisions behind the API, and what each one buys |
| 5 | [Pipeline lifecycle and control API](05-pipeline-lifecycle.md) | The state machine, every entry point, every error code |
| 6 | [Node reference](06-node-reference.md) | The eight shipped nodes: macros, accepted formats, failure modes |
| 7 | [Writing your own node](07-writing-a-node.md) | The three-op contract and the rules a node must not break |
| 8 | [I2S and hardware bring-up](08-i2s-and-hardware.md) | Clock roles, transfer blocks, DMA/cache constraints, loopback testing |
| 9 | [Troubleshooting](09-troubleshooting.md) | Error codes and build failures, with the fix next to each |

If you only have five minutes: read [Core concepts](03-core-concepts.md) and skim the
listing in [Quick Start: your first pipeline](02-quickstart-first-pipeline.md).

## What the module is, in one screen

```
 ┌── control thread (your code) ───────────────────────────────────────────┐
 │  audio_pipeline_init()  → set_format() → start() → play() … join()      │
 └────────────────────────┬───────────────────────────────────────────────┘
                          │  state (atomic), event queue (k_msgq)
 ┌────────────────────────┴───────────────────────────────────────────────┐
 │ worker thread, one per pipeline                                         │
 │   while playing:  audio_pipeline_process_frame()                        │
 │                        │                                                │
 │                        ▼   calls process() on the SINK only             │
 │        ┌────────┐   pull   ┌────────┐   pull   ┌────────┐               │
 │        │ source │ ◄─────── │ filter │ ◄─────── │  sink  │               │
 │        └────────┘          └────────┘          └────────┘               │
 │              one shared int32_t frame buffer, passed by reference       │
 └─────────────────────────────────────────────────────────────────────────┘
```

* The chain is **wired at build time** by the `*_NODE_DEFINE()` macros; the pipeline only
  ever holds a pointer to the **sink** and walks `->upstream` from there.
* Data flows by **pull**: the worker calls the sink's `process()`, and each sink or filter
  reaches upstream through `audio_node_pull()`.
* One **format** — sample rate, channel count, resolution — is bound by the application
  and installed on every node before it is opened. A node that cannot carry it fails its
  `open()`; nothing is converted behind your back.
* Every buffer is **static**. `AUDIO_PIPELINE_DEFINE()` and the node macros own all of it.

## Related documents in this repository

The wiki is deliberately practical. The normative documents live elsewhere and win on
conflict:

* `audio_pipeline_manifest.md` — the architectural contract (threads, buffers, roles).
* `audio_pipeline_spec_v2.md` — the implementation blueprint (API shapes, Kconfig, EOF and
  error rules). Section numbers quoted in the source (`spec §5.2`, `manifest §7`, …) point
  here.
* `audio_pipeline_spec_short.md` — the one-page digest of both.
* `docs/adr/` — architecture decision records; two exist and both are load bearing:
  [0001 the format is a contract nodes refuse](../adr/0001-pipeline-format-is-a-contract-nodes-refuse.md)
  and [0002 the container is left-justified Q31](../adr/0002-internal-container-is-left-justified-q31-int32.md).
* `AGENTS.md` — contributor guidelines, style, testing expectations.
* `README.md` — the file tree and the build/test entry points.
