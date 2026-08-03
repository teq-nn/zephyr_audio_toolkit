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

### Reading from upstream
- A filter and a sink read their upstream through **one shared pull helper**, never by invoking the
  upstream node's `process()` themselves. The helper owns the questions every node would otherwise
  answer for itself: a **missing upstream is a wiring error** (`-ENOTSUP`, never an empty track),
  `-EPIPE` from below is never passed on (§7), and end of stream travels up unchanged.
- The helper does **not** walk the chain: each node still decides *when* and *how often* it pulls,
  which is what keeps a resampler (N in, M out) and a later mixer (several upstreams) possible.

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

### One format, owned by the pipeline:
- Sample rate and channel count are **pipeline-wide**. They are bound once by the application
  through `audio_pipeline_set_format()` and stored in the pipeline, which is their single owner —
  no node and no configuration struct carries a second copy to disagree with.
- The pipeline installs the bound format on every node before it opens that node. A node
  **validates and refuses**; it never adapts and never negotiates with its peers. v1 has no
  resampler, so a source or sink that cannot deliver the bound sample rate or channel count fails
  its `open()` and the whole start fails with it.
- A mismatched chain therefore stops before the first sample moves, instead of producing a file
  whose header describes a stream it does not contain.
- The format is fixed while a run is in progress and may be rebound between runs, only while the
  node chain is closed.

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
- `AUDIO_PIPELINE_DEFINE()` allocates the stack, the frame buffer and the event slots **per
  instance**, so macro-defined pipelines never share storage.
- A hand-rolled (zero-initialised) instance instead falls back on the subsystem's **built-in**
  stack, frame buffer and event slots. There is exactly one of each, and they are **owned, not
  shared**: `audio_pipeline_init()` claims each built-in the instance leaves NULL and refuses a
  second claimant with `-EBUSY`; `audio_pipeline_join()` releases them again, so sequential reuse
  works and `audio_pipeline_start()` reclaims what a join gave back. Concurrency on the built-ins is
  therefore an error the caller sees, not silent memory corruption.

---

## 7. Pipeline Behavior at EOF

- When a source cannot deliver more data (`out_size = 0`), this counts as **EOL / EOF**.
- The frame size is reported **only** through the `out_size` out-parameter; the frame buffer handed
  around describes storage and capacity and carries no size of its own, so there is one place to
  write it and one place to read it.
- Filters forward EOF unchanged.
- The sink raises an **EOF event** (via message queue or callback).
- Audio processing stops, but **the thread keeps running** in idle mode.
- `-EPIPE` is **reserved** for this end-of-stream signal inside the pipeline. No node may report it
  as a failure: every boundary an error can enter through - an upstream node, a filesystem, the sink
  itself - remaps it to `-EIO`, so a broken node can never masquerade as a finished track.

---

## 8. Event Handling

- The pipeline emits events for:
  - `AUDIO_PIPELINE_EVENT_EOF`
  - `AUDIO_PIPELINE_EVENT_ERROR`
  - `AUDIO_PIPELINE_EVENT_RECONFIG`
- Events are exposed via a per-pipeline `k_msgq`, read with `audio_pipeline_get_event()`.
  The queue is the primary path; the optional `event_cb` callback is a secondary one.
- The slot storage behind that queue is per instance for `AUDIO_PIPELINE_DEFINE()` and the
  subsystem's single built-in set for a zero-initialised instance. The built-in set follows the
  claim/release rule of §6, so two instances can never publish into one ring: the second one is
  refused at `init()`/`start()` rather than interleaving its events with the first one's.
- The same rule governs *reading* a released ring. After `join()` an instance keeps delivering the
  events already queued — nothing else has written to them yet — but once the built-in slots are
  claimed by someone else, `audio_pipeline_get_event()` reports `-EPERM` instead of consuming the
  new owner's events, and a restart rebinds the queue rather than reusing a binding that has gone
  stale.
- Resolved delivery contract: the callback is invoked **before** the event is queued, so an
  event becoming visible on the queue means the pipeline has finished reacting to it — chain
  quiesced and callback returned. Depth is `CONFIG_AUDIO_PIPELINE_EVENT_QUEUE_DEPTH`; on
  overflow the **newest** event is dropped, so the worker thread never stalls on a slow
  consumer and the first error in a cascade is preserved.

---

## 9. Memory and API Design

- Nodes are created via `NODE_DEFINE` macros (static allocation).
- Each shipped node is its own Kconfig symbol (`CONFIG_AUDIO_PIPELINE_NODE_*`, all defaulting to
  `n`), and only the enabled ones are compiled, so an image carries the nodes the application
  defines and no others. Dependencies a node needs belong to that node's symbol - the two file
  nodes are what select `FILE_SYSTEM`, not the pipeline.
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
│  ├─ audio_pipeline_events.h
│  └─ audio_wav.h          # RIFF/WAVE header: read and write, one byte layout
├─ subsys/audio/pipeline/
│  ├─ CMakeLists.txt
│  ├─ Kconfig
│  ├─ audio_pipeline_core.c
│  ├─ audio_pipeline_config.c
│  ├─ audio_pipeline_events.c
│  ├─ audio_node_core.c
│  ├─ audio_internal.h
│  ├─ audio_wav.c
│  └─ nodes/
│      ├─ file_reader_node.c
│      ├─ file_writer_node.c
│      ├─ gain_filter_node.c
│      └─ null_sink_node.c
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
   │  ├─ fake_nodes.h        # shared fakes: scripted source, counting sink, read back
   │  ├─ fake_nodes.c
   │  ├─ Kconfig
   │  ├─ prj.conf
   │  ├─ testcase.yaml
   │  ├─ test_roundtrip.c
   │  ├─ test_error_paths.c
   │  ├─ test_lifecycle.c            # spec §8.2/§9 lifecycle
   │  ├─ test_builtin_resources.c    # built-in stack, frame buffer, event slots: claim/release
   │  ├─ test_static_define.c        # DEFINE macros, multi-instance isolation
   │  ├─ test_events.c               # k_msgq event queue
   │  ├─ test_file_reader.c          # WAV source, S16→S32 widening
   │  └─ test_file_writer.c          # WAV sink, S32→S16 truncation
   ├─ no_file_nodes/             # node selection: file nodes off, FILE_SYSTEM stays out
   │  ├─ CMakeLists.txt
   │  ├─ prj.conf
   │  ├─ testcase.yaml
   │  └─ test_no_file_nodes.c
   └─ wav/                       # standalone unit test, no CONFIG_AUDIO_PIPELINE
      ├─ CMakeLists.txt
      ├─ prj.conf
      ├─ testcase.yaml
      └─ test_wav.c              # header write/read round trip, parser errors
```
This document is our shared engineering contract.
