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
struct audio_node_ops {
    int     (*open)(struct audio_node *node);
    ssize_t (*process)(struct audio_node *node, int32_t *buf,
                       size_t capacity /* samples */, size_t *out_size /* samples */);
    int     (*close)(struct audio_node *node);
};
```

`open()`/`close()` return Zephyr error codes (`0` ok, `< 0` failure). `process()` returns the sample
count produced (mirrored in `*out_size`) or a negative error code.

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
- v1 is fixed at **2 channels**, interleaved: `L0, R0, L1, R1, ...`.
- Conversion happens at the edges: sources widen inbound PCM (`s16 << 16`, `s24 << 8`), sinks narrow
  it again (`(int16_t)(s32 >> 16)`).
- Sample rate, channel count, and format are pipeline-wide and static for the whole runtime in v1.

```c
struct audio_format {
    uint32_t sample_rate;
    uint8_t  channels;              /* v1: always 2 */
    uint8_t  valid_bits_per_sample; /* 16, 24, 32 */
    enum audio_sample_format format;
};
```

## 5. Frames, buffers, and static definition (manifest §5/§6/§9, spec §6)

- Frame size comes from `CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES` (per channel) and governs latency and
  per-cycle workload. One `process()` call per node per frame.
- The pipeline owns one static shared frame buffer; nodes may hold their own static scratch buffers.
- `AUDIO_PIPELINE_DEFINE()` instantiates the pipeline struct, its thread stack
  (`K_THREAD_STACK_DEFINE`), and the frame buffer. `*_NODE_DEFINE()` macros do the same for nodes and
  their contexts. Users never supply buffer pointers.

## 6. Lifecycle and API (spec §8)

```c
int audio_pipeline_init(struct audio_pipeline *pl);
int audio_pipeline_set_nodes(struct audio_pipeline *pl, struct audio_node *source,
                             struct audio_node **filters, size_t filter_count,
                             struct audio_node *sink);
int audio_pipeline_set_format(struct audio_pipeline *pl, const struct audio_format *fmt);

int audio_pipeline_start(struct audio_pipeline *pl); /* create thread, open() all nodes */
int audio_pipeline_play(struct audio_pipeline *pl);  /* playing = true  */
int audio_pipeline_stop(struct audio_pipeline *pl);  /* playing = false, thread idles */
int audio_pipeline_join(struct audio_pipeline *pl);  /* optional: end the thread */

int audio_pipeline_get_event(struct audio_pipeline *pl, struct audio_pipeline_event *evt,
                             k_timeout_t timeout);
```

`set_nodes()` links the `upstream` pointers itself: `filters[0].upstream = source`,
`filters[i].upstream = filters[i-1]`, `sink.upstream = last filter or source`.

## 7. EOF and errors (manifest §7/§8, spec §9)

- **EOF**: the source reports `*out_size = 0` and returns `0`. Filters propagate it unchanged. The
  sink detects it and tells the pipeline, which clears `playing` and emits
  `AUDIO_PIPELINE_EVENT_EOF`. Processing stops; **the thread keeps running** in idle mode.
- **Error**: any negative return from `open()`, `process()`, or `close()`. On the first error the
  pipeline stops frame processing, emits `AUDIO_PIPELINE_EVENT_ERROR` with the error code, and may
  `close()` all nodes.
- Event types: `AUDIO_PIPELINE_EVENT_EOF`, `AUDIO_PIPELINE_EVENT_ERROR`,
  `AUDIO_PIPELINE_EVENT_RECONFIG`, delivered via an internal `k_msgq` (optionally via callback).

## 8. Kconfig (spec §7)

| Symbol | Meaning |
| --- | --- |
| `CONFIG_AUDIO_PIPELINE` | Enable the subsystem. |
| `CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES` | Samples per frame, per channel. |
| `CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE` | Worker thread stack size. |
| `CONFIG_AUDIO_PIPELINE_THREAD_PRIO` | Worker thread priority. |

## 9. Tests (spec §12)

- Everything must run on `native_sim`/QEMU, with no real audio hardware.
- Roundtrip: mount a golden-master WAV, run `file_reader → [filter] → file_writer` to EOF, compare
  the output byte-for-byte.
- Negative paths: corrupted WAV header makes `open()` fail; early EOF still yields a clean EOF event;
  simulated I/O errors yield an ERROR event.

## 10. Layout (manifest §12, spec §14)

```text
zephyr-audio-pipeline/
├─ module.yml, CMakeLists.txt, Kconfig      # Zephyr out-of-tree module glue
├─ include/zephyr/audio/                    # audio_format.h, audio_node.h,
│                                           # audio_pipeline.h, audio_pipeline_events.h
├─ subsys/audio/pipeline/                   # core, config, events, node core, audio_internal.h
│  ├─ nodes/                                # file_reader, file_writer, gain_filter, null_sink
│  └─ util/                                 # wav_parser
├─ samples/audio/pipeline_basic/            # CMakeLists.txt, Kconfig, src/main.c
└─ tests/subsys/audio/pipeline/             # test_roundtrip.c, test_error_paths.c
```

## 11. Out of scope for v1 (spec §1.3, §13)

No generic float processing (only explicit `s32_to_float` / `float_to_s32` converter nodes later),
no runtime reconfiguration, no multi-input/multi-output nodes (mixer, splitter), no timer-paced
pipeline, no channel counts other than 2. These are the named extension points, not gaps.

---

The implementation is being brought up to this contract issue by issue, so parts of the tree may
still lag the description above. The manifest and spec define the target; the code follows them.
