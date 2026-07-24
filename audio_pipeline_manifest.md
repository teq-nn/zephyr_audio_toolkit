# Audio Pipeline Manifest (Zephyr-Compatible)

This manifest captures all architectural decisions we agreed on.  
It acts as the binding engineering contract for ongoing development.

---

## 1. Core Principles

- The system uses a **pull model**.
- Data flow is always initiated by the **sink**, which requests samples from its upstream node.
- Every node (source, filter, sink) implements a passive `process()` function that only runs when invoked by the pipeline thread.
- There are **no per-node tasks**.

---

## 2. Roles: Source, Filter, Sink

### Source  
- Has **no upstream**.  
- Produces data (e.g., file reader, generator).  
- Needs no input buffer, writes into the caller-provided buffer.

### Filter  
- Has **exactly one upstream**.  
- Reads data from upstream and processes it in-place or with an internal scratch buffer.  
- Can be an analyzer, decoder, DSP, resampler, etc.

### Sink  
- Is the **start point of the pull cycle**.  
- Invokes the entire chain of upstream nodes.  
- Consumes final data (e.g., file writer, hardware sink, test sink).

---

## 3. Thread Model

- The pipeline owns **one worker thread** that runs in the background.
- This thread is created via `audio_pipeline_start()`.
- The thread persists even when a track / source ends.
- Multiple tracks can be processed without restarting the thread.

---

## 4. Data Format (Canonical)

### Internal container format:
- **int32_t** per sample  
- Little Endian  
- Enum: `AUDIO_SAMPLE_FORMAT_S32_LE`  
- This format is **global and always used** inside the pipeline.

### Valid bits:
- The actual resolution (e.g., 16, 24, 32 bits) is stored in `valid_bits_per_sample`.
- Filters always see 32-bit container values.

### Conversion:
- All sources convert inbound formats (e.g., WAV 16-bit) → 32-bit.  
- All sinks convert back if needed.

---

## 5. Frame Size & Timing

- Global frame size is defined via **Kconfig**:  
  `CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES`
- This size governs latency and workload per processing cycle.
- The pipeline calls `process()` once per node per frame.

---

## 6. Buffer Strategy

- **No dynamic allocations (`k_malloc`) inside the subsystem.**
- All pipeline and node structures are **static**, created via macros.
- The pipeline thread uses a **shared frame buffer**, also static.
- Nodes may have **internal scratch buffers**, also static (via DEFINE macros).

---

## 7. Pipeline Behavior at EOF

- When a source cannot deliver more data (`out_size = 0`), this counts as **EOL / EOF**.
- Filters forward EOF unchanged.
- The sink raises an **EOF event** (via message queue or callback).
- Audio processing stops, but **the thread keeps running** in idle mode.

---

## 8. Event Handling

- The pipeline emits events for:
  - `AUDIO_PIPELINE_EVENT_EOF`
  - `AUDIO_PIPELINE_EVENT_ERROR`
  - `AUDIO_PIPELINE_EVENT_RECONFIG`
- Events are exposed via a per-pipeline `k_msgq`, read with `audio_pipeline_get_event()`.
  The queue is the primary path; the optional `event_cb` callback is a secondary one.
- Resolved delivery contract: the callback is invoked **before** the event is queued, so an
  event becoming visible on the queue means the pipeline has finished reacting to it — chain
  quiesced and callback returned. Depth is `CONFIG_AUDIO_PIPELINE_EVENT_QUEUE_DEPTH`; on
  overflow the **newest** event is dropped, so the worker thread never stalls on a slow
  consumer and the first error in a cascade is preserved.

---

## 9. Memory and API Design

- Nodes are created via `NODE_DEFINE` macros (static allocation).
- The pipeline is created via `AUDIO_PIPELINE_DEFINE()` (static thread, buffer, context).
- The user does **not** need to supply buffer pointers.
- Everything is fully statically allocated and deterministic.

---

## 10. Extensibility

- Float support is possible but only via **explicit converter nodes** (`float_to_s32`, `s32_to_float`).
- Format changes at runtime happen only explicitly.
- Future multi-input or multi-output nodes (mixers, splitters) fit this model.

---

## 11. Summary

This manifest fully defines the audio pipeline architecture:

- Pull model  
- One pipeline thread  
- Nodes are passive, no own threads  
- Static memory strategy  
- Canonical 32-bit sample format  
- Global frame size  
- EOL propagates cleanly, thread stays alive  
- Event system via message queue  
- API and internals are deterministic and MCU-friendly  

---

## 12. Project Structure

The implementation follows the layout defined in the specification:

```
zephyr-audio-pipeline/
├─ zephyr/module.yml      # module manifest; Zephyr only looks here
├─ CMakeLists.txt
├─ Kconfig
├─ include/zephyr/audio/
│  ├─ audio_format.h
│  ├─ audio_node.h
│  ├─ audio_nodes.h        # per-node state types, ops externs, node DEFINE macros
│  ├─ audio_pipeline.h
│  └─ audio_pipeline_events.h
├─ subsys/audio/pipeline/
│  ├─ CMakeLists.txt
│  ├─ Kconfig
│  ├─ audio_pipeline_core.c
│  ├─ audio_pipeline_config.c
│  ├─ audio_pipeline_events.c
│  ├─ audio_node_core.c
│  ├─ audio_internal.h
│  ├─ nodes/
│  │   ├─ file_reader_node.c
│  │   ├─ file_writer_node.c
│  │   ├─ gain_filter_node.c
│  │   └─ null_sink_node.c
│  └─ util/
│      ├─ wav_parser.c
│      └─ wav_parser.h
├─ samples/audio/pipeline_basic/
│  ├─ CMakeLists.txt
│  ├─ Kconfig
│  ├─ prj.conf
│  ├─ app.overlay            # zephyr,ram-disk for the generated track
│  └─ src/main.c
└─ tests/subsys/audio/
   ├─ pipeline/
   │  ├─ CMakeLists.txt
   │  ├─ app.overlay         # zephyr,ram-disk backing the ext2 fixture mount
   │  ├─ wav_fixture.h       # shared fixture: mount, raw writer, WAV generator
   │  ├─ wav_fixture.c
   │  ├─ Kconfig
   │  ├─ prj.conf
   │  ├─ testcase.yaml
   │  ├─ test_roundtrip.c
   │  ├─ test_error_paths.c
   │  ├─ test_lifecycle.c        # spec §8.2/§9 lifecycle
   │  ├─ test_static_define.c    # DEFINE macros, multi-instance isolation
   │  ├─ test_events.c           # k_msgq event queue
   │  └─ test_file_reader.c      # WAV source, S16→S32 widening
   └─ wav_parser/                # standalone unit test, no CONFIG_AUDIO_PIPELINE
      ├─ CMakeLists.txt
      ├─ prj.conf
      ├─ testcase.yaml
      └─ test_wav_parser.c
```
This document is our shared engineering contract.
