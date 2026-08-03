# Audio Pipeline – Short Spec (Onboarding Digest)

A one-page summary of `audio_pipeline_manifest.md` (the architectural contract) and
`audio_pipeline_spec_v2.md` (the implementation blueprint). It exists so a new contributor or agent
can get the architecture without reading 750+ lines.

**This document is a digest, not a source of truth.** Where it disagrees with the manifest or the
spec, those documents win. Refresh this file whenever either of them changes materially.
Section references below point back into the full documents.

---

## 1. The model in five sentences

The pipeline is **pull-based**: the sink starts every cycle and asks its upstream for data, which
asks its upstream, down to the source (manifest §1, spec §2.2). Nodes are **passive** — they own no
threads and only run when the pipeline's single worker thread calls them (manifest §3, spec §3.1).
Everything is **statically allocated**; the subsystem never calls `k_malloc` (manifest §6,
spec §11.1). Audio moves in fixed-size **frames** of canonical 32-bit little-endian samples
(manifest §4/§5, spec §5). End-of-stream and errors travel back to the application as **events**
while the worker thread stays alive (manifest §7/§8, spec §9).

Call chain per frame:

```text
sink -> filter -> ... -> filter -> source
```

## 2. Roles (manifest §2, spec §4)

| Role | Upstream | Responsibility |
| --- | --- | --- |
| Source | none (`upstream == NULL`) | Produces data (file reader, generator); converts its input format to the canonical S32_LE and writes into the caller-provided buffer. |
| Filter | exactly one | Pulls from upstream, then transforms in place or via a static scratch buffer. Analyzers, decoders, DSP, resamplers. |
| Sink | exactly one | Starts the pull cycle and consumes the result (file writer, hardware sink, test sink); converts back to its target format if needed. |

All three implement the same "opc" op set (spec §4.1):

```c
struct audio_buffer_view {
    int32_t *data;     /* frame buffer, owned by the pipeline */
    size_t   capacity; /* samples the buffer can hold */
};

struct audio_node_ops {
    int (*open)(struct audio_node *node);
    int (*process)(struct audio_node *node, struct audio_buffer_view *buf,
                   size_t *out_size /* samples */);
    int (*close)(struct audio_node *node);
};
```

All three return Zephyr error codes (`0` ok, `< 0` failure). `process()` reports the sample count it
produced in `*out_size` — the only place a frame size is ever written — and `0` there means end of
stream.

Filters and sinks never call an upstream node's `process` op themselves; they read it through the one
pull helper (spec §4.1.1):

```c
int audio_node_pull(struct audio_node *node, struct audio_buffer_view *buf, size_t *out_size);
```

It returns `-ENOTSUP` when the node has no upstream (a wiring error, not an empty track), forwards
end of stream verbatim, and remaps a `-EPIPE` coming from below to `-EIO` so a broken upstream can
never look like a finished one. Nodes still choose *when* and *how often* to pull, which is what
keeps spec §13's resampler and mixer possible.

## 3. Threading (manifest §3, spec §3)

- One worker thread per pipeline, created in `audio_pipeline_start()` via `k_thread_create`.
- The thread loops while `playing` is set, then idles — it survives end-of-track, so multiple tracks
  run without restarting it. Only `audio_pipeline_stop()`/`audio_pipeline_join()` wind it down.
- v1 has **no timer pacing**: frames are processed as fast as data arrives. Real-time sync is the
  sink's job (e.g. a hardware sink blocking in `process()`) — spec §3.2.
- Concurrency rules (spec §3.3): `audio_pipeline_*` is called only from a control thread; nodes are
  called only from the pipeline thread and need no internal locking; the event queue may be read
  from any thread.

## 4. Canonical data format (manifest §4/§5, spec §5)

- Container: `int32_t`, little endian, `AUDIO_SAMPLE_FORMAT_S32_LE`. Used everywhere inside the
  pipeline; filters only ever see 32-bit containers.
- `valid_bits_per_sample` (16/24/32) carries the effective resolution alongside the container.
- v1 supports **1 or 2 channels**, interleaved: `L0, R0, L1, R1, ...`.
- Conversion happens at the edges: sources widen inbound PCM (`s16 << 16`, `s24 << 8`), sinks narrow
  it again (`(int16_t)(s32 >> 16)`).
- Sample rate, channel count, and format are **pipeline-wide**, bound once by the application and
  owned by the pipeline. Fixed for the duration of a run; rebindable between runs.

```c
struct audio_stream_config {
    uint32_t sample_rate_hz;
    uint8_t  channels;              /* v1: 1 or 2 */
    uint8_t  valid_bits_per_sample; /* 16, 24, 32 */
    enum audio_sample_format format;
};
```

### Binding and matching (spec §5.2)

- `audio_pipeline_set_format()` is the **only** way in — `audio_pipeline_config` has no format
  field. `start()` returns `-ENODATA` if nothing was ever bound.
- Legal only while the node chain is closed (before the first `start()`, after `join()`);
  `-EBUSY` otherwise. Cleared by a fresh `init()`.
- The pipeline installs the format on each node as `audio_node.pipeline_format` before calling that
  node's `open()`. The `open`/`process`/`close` signatures are unchanged.
- Nodes **validate, never adapt**: `sample_rate_hz` and `channels` must match exactly or `open()`
  returns `-ENOTSUP`. `valid_bits_per_sample` is enforced per node (v1's file nodes are 16-bit
  only). No resampler exists in v1, so refusing is the only option.
- Control thread only (§3.3), and the worker never reads it — hence no mutex.

## 5. Frames, buffers, and static definition (manifest §5/§6/§9, spec §6)

- Frame size comes from `CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES` (per channel) and governs latency and
  per-cycle workload. One `process()` call per node per frame.
- The pipeline owns one static shared frame buffer; nodes may hold their own static scratch buffers.
- `AUDIO_PIPELINE_DEFINE()` instantiates the pipeline struct, its thread stack
  (`K_THREAD_STACK_DEFINE`), the frame buffer, and the event slots — all per instance, so two
  macro-defined pipelines share nothing. `*_NODE_DEFINE()` macros do the same for nodes and their
  contexts. Users never supply buffer pointers.
- A zero-initialised (hand-rolled) instance instead falls back on the subsystem's single built-in
  stack, frame buffer and event slots. Those are **owned, not shared**: `init()` claims them and
  gives a second claimant `-EBUSY`, `join()` releases them, `start()` reclaims them (also `-EBUSY`
  if they were taken meanwhile). Need two pipelines at once? Use `AUDIO_PIPELINE_DEFINE()` for at
  least one of them. Join such an instance before discarding it — that is the only release — and do
  not pull frames on it between the join and the next successful init/start.
- Reading events in that window is guarded rather than forbidden: what is already queued stays
  readable after `join()`, `audio_pipeline_get_event()` switches to `-EPERM` once another instance
  claims the built-in slots, and `start()` rebinds the queue so a restarted instance never inherits
  the other one's events (spec §6.1/§8.3).

## 6. Lifecycle and API (spec §8)

```c
int audio_pipeline_init(struct audio_pipeline *pl);
int audio_pipeline_set_nodes(struct audio_pipeline *pl, struct audio_node *source,
                             struct audio_node **filters, size_t filter_count,
                             struct audio_node *sink);
int audio_pipeline_set_format(struct audio_pipeline *pl, const struct audio_stream_config *fmt);

int audio_pipeline_start(struct audio_pipeline *pl); /* -ENODATA if no format bound;
                                                        create thread, open() all nodes */
int audio_pipeline_play(struct audio_pipeline *pl);  /* OPEN -> PLAYING */
int audio_pipeline_stop(struct audio_pipeline *pl);  /* PLAYING -> OPEN, thread idles */
int audio_pipeline_join(struct audio_pipeline *pl);  /* optional: end the thread, -> INIT */

int audio_pipeline_get_event(struct audio_pipeline *pl, struct audio_pipeline_event *evt,
                             k_timeout_t timeout);
```

`set_nodes()` links the `upstream` pointers itself: `filters[0].upstream = source`,
`filters[i].upstream = filters[i-1]`, `sink.upstream = last filter or source`.

## 7. EOF and errors (manifest §7/§8, spec §9)

- **EOF**: the source reports `*out_size = 0` and returns `0`. Filters propagate it unchanged. The
  sink detects it and tells the pipeline, which moves `PLAYING` -> `OPEN` and emits
  `AUDIO_PIPELINE_EVENT_EOF`. Processing stops; **the thread keeps running** in idle mode.
- **Error**: any negative return from `open()`, `process()`, or `close()`. `-EPIPE` is reserved for
  the pipeline's own end-of-stream signal, so no node may report it; the pull helper remaps it to
  `-EIO`. On the first error the pipeline stops frame processing, emits
  `AUDIO_PIPELINE_EVENT_ERROR` with the error code, and may `close()` all nodes.
- Event types: `AUDIO_PIPELINE_EVENT_EOF`, `AUDIO_PIPELINE_EVENT_ERROR`,
  `AUDIO_PIPELINE_EVENT_RECONFIG`, delivered via an internal `k_msgq` (optionally via callback).

## 8. Kconfig (spec §7)

| Symbol | Meaning |
| --- | --- |
| `CONFIG_AUDIO_PIPELINE` | Enable the subsystem. |
| `CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES` | Samples per frame, per channel. |
| `CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE` | Worker thread stack size. |
| `CONFIG_AUDIO_PIPELINE_THREAD_PRIO` | Worker thread priority. |
| `CONFIG_AUDIO_PIPELINE_NODE_FILE_READER` | Build the file reader source; selects `FILE_SYSTEM`. |
| `CONFIG_AUDIO_PIPELINE_NODE_FILE_WRITER` | Build the file writer sink; selects `FILE_SYSTEM`. |
| `CONFIG_AUDIO_PIPELINE_NODE_GAIN_FILTER` | Build the gain filter. |
| `CONFIG_AUDIO_PIPELINE_NODE_NULL_SINK` | Build the null sink. |

- **Node symbols all default to `n`** and each one gates its node's source file, its state type and
  its `*_NODE_DEFINE()` macro. Enabling `AUDIO_PIPELINE` alone gives a pipeline with no nodes; an
  application adds one line per node it defines. Using the macro of a node that was not built is a
  build error naming the missing symbol, not a link error.

## 9. Tests (spec §12)

- Everything must run on `native_sim`/QEMU, with no real audio hardware.
- Roundtrip: mount a golden-master WAV, run `file_reader → [filter] → file_writer` to EOF, compare
  the output byte-for-byte.
- Negative paths: corrupted WAV header makes `open()` fail; early EOF still yields a clean EOF event;
  simulated I/O errors yield an ERROR event; a second hand-rolled pipeline asking for the built-in
  resources gets `-EBUSY` while the first one keeps running.

## 10. Layout (manifest §12, spec §14)

```text
zephyr-audio-pipeline/
├─ module.yml, CMakeLists.txt, Kconfig      # Zephyr out-of-tree module glue
├─ include/zephyr/audio/                    # audio_format.h, audio_node.h, audio_pipeline.h,
│                                           # audio_pipeline_events.h, audio_wav.h
├─ subsys/audio/pipeline/                   # core, config, events, node core, audio_internal.h,
│  │                                        # audio_wav.c (RIFF/WAVE header read + write)
│  └─ nodes/                                # file_reader, file_writer, gain_filter, null_sink
├─ samples/audio/pipeline_basic/            # CMakeLists.txt, Kconfig, src/main.c
├─ tests/subsys/audio/pipeline/             # test_roundtrip.c, test_error_paths.c
└─ tests/subsys/audio/wav/                  # test_wav.c, standalone header unit test
```

## 11. Out of scope for v1 (spec §1.3, §13)

No generic float processing (only explicit `s32_to_float` / `float_to_s32` converter nodes later),
no runtime reconfiguration, no multi-input/multi-output nodes (mixer, splitter), no timer-paced
pipeline, no channel counts other than 2. These are the named extension points, not gaps.

---

The implementation is being brought up to this contract issue by issue, so parts of the tree may
still lag the description above. The manifest and spec define the target; the code follows them.
