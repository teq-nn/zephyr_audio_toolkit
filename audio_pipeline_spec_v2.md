# Zephyr Audio Pipeline – Software Specification (v1)

This document describes the architecture and behavior of an audio pipeline subsystem for Zephyr.  
It is written so an external developer can implement it without additional context.

---

## 1. Goals

### 1.1 Purpose

The subsystem provides a **lightweight audio pipeline** that chains audio data across sources, filters, and sinks.

### 1.2 Design goals

- **Pull-based dataflow model**
- **One worker thread per pipeline**
- **Static memory allocation** (no `k_malloc` in the subsystem)
- **Canonical internal sample format: 32-bit signed, Little Endian**
- **Clear roles**: source, filter, sink
- **Deterministic latency via global frame size**
- **Zephyr-compliant error codes and coding style**

### 1.3 Non-goals (v1)

- No generic support for float processing (extensibility only).
- No dynamic runtime reconfiguration of the pipeline *while it runs*. Structure is static in v1;
  the format is fixed for the duration of a run and may be rebound between runs, while the node
  chain is closed (§5.2).
- No multi-input or multi-output nodes (no mixer/splitter in v1).

---

## 2. Core Architecture

### 2.1 Components

- **Audio pipeline (`audio_pipeline`)**  
  - Holds an ordered list of nodes (source → filter... → sink).  
  - Owns its own worker thread.  
  - Responsible for:
    - Lifecycle (open/close) of all nodes,
    - Start/stop of processing,
    - Event generation (EOF, errors).

- **Nodes (`audio_node`)**  
  - Implement audio functionality.  
  - Classified into three roles:
    - **Source**: produces data
    - **Filter**: transforms data
    - **Sink**: consumes data

- **Event system**  
  - Reports key states (EOF, errors, reconfigure) to the application.

### 2.2 Dataflow model (pull)

- The **sink** initiates dataflow by requesting data from its upstream.
- Each filter calls its upstream in turn, until a source is served.
- Data is processed in fixed **frames** with a globally defined size.

Sequence (simplified):

```text
Pipeline thread:
    while (playing) {
        frame = pull_frame_from_sink();
        if (frame_size == 0) {
            signal EOF;
            playing = false;
        }
    }
```

Call chain (pull):

```
sink -> filter -> filter -> source
```

---

## 3. Threading & Execution

### 3.1 Pipeline thread

- The pipeline creates a **worker thread** via `k_thread_create` in `audio_pipeline_start()`.
- This thread:
  - loops frame processing while an internal `playing` flag is set,
  - then idles/waits (thread stays alive),
  - is only terminated by `audio_pipeline_stop()`/`audio_pipeline_join()`.

### 3.2 Timing model (v1)

- In v1 there is **no explicit timer pacing** inside the pipeline.
- The pipeline thread processes frames **as fast as possible** while:
  - `playing == true`, and
  - nodes still provide data.
- Real-time sync (e.g., I2S output) is sink responsibility:
  - A hardware sink may block in `process()` or pace itself internally.
- For file-based tests (file→file, file→memory) this best-effort loop is sufficient and simple.

> Note: Timer-based scheduling can be added later but is explicitly *not* part of v1.

### 3.3 Concurrency rules

- **Pipeline API (`audio_pipeline_*`)**  
  - May only be called from a **control thread** (e.g., main thread or dedicated control task).
- **Nodes (`audio_node`)**  
  - Are invoked only by the **pipeline thread**.  
  - Nodes are **not reentrant** and need no internal thread safety.
- **Event queue**  
  - May be read by the application from any thread (Zephyr-style `k_msgq` semantics).
- **Pipeline format**  
  - Written only by `audio_pipeline_set_format()`, i.e. from the control thread, and only while the
    node chain is closed (§5.2, §8.1).
  - Read by a node in its `open()`, which the control thread also drives, inside
    `audio_pipeline_start()`. The worker thread never reads it.
  - Both accesses are therefore already serialised by this rule, which is why the bound format needs
    no mutex. A future revision that relaxes control-thread exclusivity would have to add one.

---

## 4. Role Model: Source, Filter, Sink

### 4.1 Common node type

```c
enum audio_node_role {
    AUDIO_NODE_ROLE_SOURCE,
    AUDIO_NODE_ROLE_FILTER,
    AUDIO_NODE_ROLE_SINK,
};

struct audio_buffer_view {
    int32_t *data;     /* frame buffer, owned by the pipeline */
    size_t   capacity; /* samples the buffer can hold */
};

struct audio_node_ops {
    int (*open)(struct audio_node *node);
    int (*process)(struct audio_node *node,
                   struct audio_buffer_view *buf,
                   size_t *out_size); /* in samples */
    int (*close)(struct audio_node *node);
};

struct audio_node {
    enum audio_node_role role;
    const struct audio_node_ops *ops;
    struct audio_node *upstream; /* NULL for sources */
    void *state;                 /* implementation-specific state */

    /* Pipeline format, installed by the pipeline before open() (§5.2).
     * Owned by the pipeline; NULL until the node is opened. */
    const struct audio_stream_config *pipeline_format;
};
```

**The format reaches nodes through the node object, not through `open()`.**
`open()`, `process()` and `close()` keep the signatures above. The pipeline writes
`pipeline_format` on every node in the chain immediately before it calls that node's
`open()`, so a node reads its pipeline's format as `node->pipeline_format` and needs no
route back to the pipeline object. The field is read-only to the node and stable for as
long as the node is open (§5.2).

Adding a parameter to `open()` was rejected: the format is a property of the node's
binding, not of the act of opening, and threading it through the op signature would churn
every node and every fake for a value most nodes ignore.

**Naming convention:**  
The function pointers in `audio_node_ops` are named **`open`**, **`process`**, **`close`** (“opc”), as requested.

**Error codes:**  
- `open()`, `process()` and `close()` return `int` with Zephyr error codes (`0` on success, `< 0` on failure; e.g., `-EINVAL`, `-EIO`, `-ENOMEM`).
- On success `process()` reports the number of samples actually produced in `*out_size`; `0` means end of stream.

**One size, one place:**
The frame size travels **only** through `*out_size`. `audio_buffer_view` describes the buffer (`data`, `capacity`) and nothing else, so a node has exactly one field to write and a caller exactly one field to read.

### 4.1.1 Pulling from upstream

Reading a frame from upstream has exactly one implementation:

```c
int audio_node_pull(struct audio_node *node,
                    struct audio_buffer_view *buf,
                    size_t *out_size);
```

Every filter and every sink reads its upstream through `audio_node_pull()` and passes **itself** as `node`; no node invokes an upstream node's `process` op (directly or through `audio_node_process()`). The helper owns the three decisions that used to be repeated per node:

- **No upstream is a wiring error.** §4.3 and §4.4 require a filter and a sink to have an upstream, so `node->upstream == NULL` returns `-ENOTSUP` — never a clean end of stream, which would silently swallow the track.
- **`-EPIPE` never escapes upwards.** `-EPIPE` is the pipeline's reserved end-of-stream code (§9). A `-EPIPE` arriving from below is remapped to `-EIO`, so a broken upstream can never reach the application as a finished track.
- **End of stream is forwarded verbatim.** `*out_size == 0` with return `0` travels up the chain unchanged; `*out_size` is `0` on every failure.

Nodes keep control of **when** and **how often** they pull: a resampler may pull several times per frame and a mixer once per upstream (§13).

### 4.2 Source role

- `role == AUDIO_NODE_ROLE_SOURCE`
- `upstream == NULL`
- `process()`:
  - reads data from a source (file, generator, etc.),
  - fills `buf` with up to `capacity` samples,
  - writes the actual sample count to `*out_size`.

EOF convention:
- At end of data: `*out_size = 0`, `process()` returns `0`.

### 4.3 Filter role

- `role == AUDIO_NODE_ROLE_FILTER`
- `upstream != NULL`
- `process()`:
  - first calls `audio_node_pull(node, buf, out_size)` (§4.1.1),
  - processes the delivered samples (in-place or using scratch),
  - writes the resulting sample count back to `*out_size`.

EOF behavior:
- When upstream delivers `out_size == 0`, the filter must:
  - set `*out_size = 0`,
  - return `0`,
  - produce no further data.

### 4.4 Sink role

- `role == AUDIO_NODE_ROLE_SINK`
- `upstream != NULL`
- `process()`:
  - calls `audio_node_pull(node, buf, out_size)` (§4.1.1),
  - processes or consumes the data (e.g., writes to a file),
  - the pipeline mostly cares about EOF/errors:
    - If `*out_size == 0`: end-of-stream detected.
    - A sink defined without an upstream is a wiring error: the pull reports `-ENOTSUP`.

Note:
- Unlike many frameworks, the sink does not have to write into `buf`; it uses it as a transient transport buffer.

---

## 5. Data Format

### 5.1 Canonical internal format

- Data type: `int32_t`
- Endianness: Little Endian
- Enum value: `AUDIO_SAMPLE_FORMAT_S32_LE`
- All nodes work internally with 32-bit samples.

### 5.2 The pipeline format

```c
struct audio_stream_config {
    uint32_t sample_rate_hz;        /* Hz, e.g. 44100, 48000 */
    uint8_t  channels;              /* v1: 1 or 2 */
    uint8_t  valid_bits_per_sample; /* e.g. 16, 24, 32 */
    enum audio_sample_format format;/* internal: AUDIO_SAMPLE_FORMAT_S32_LE */
};
```

- Samples are **interleaved** in the buffer:

```text
buf: L0, R0, L1, R1, L2, R2, ...
```

- `valid_bits_per_sample` describes the effective resolution:
  - e.g., `16` for data converted from 16-bit PCM,
  - `24` or `32` for higher resolution.

#### Binding: one format, declared top-down

Sample rate and channel count are **pipeline-wide** and are declared by the application
before the chain opens. The pipeline is the single authority; nodes do not negotiate with
each other and no node infers the format from its peers.

- The format is bound with `audio_pipeline_set_format()` (§8.1) and lives in storage the
  **pipeline owns**. There is no format field on `audio_pipeline_config`: one setter is
  the only way in, so there is never a second place to look.
- `audio_pipeline_start()` refuses a pipeline with no bound format (`-ENODATA`) rather
  than inventing a default.
- Immediately before it calls a node's `open()`, the pipeline installs the bound format on
  that node as `audio_node.pipeline_format` (§4.1).

#### Matching: nodes validate, they do not adapt

Every node checks the bound format in `open()` against what it can actually deliver or
accept, and **fails the open** when it cannot comply. v1 has no resampler and no channel
mapper (§1.3, §13), so a node can only match or refuse.

- **`sample_rate_hz` and `channels` must match exactly.** A source whose real, on-disk
  format disagrees with the bound format returns `-ENOTSUP` from `open()`; so does a sink
  that cannot emit the bound format. Because every node is checked against the same bound
  format, source and sink agree transitively — the pipeline performs no separate
  source-versus-sink comparison.
- **`valid_bits_per_sample` is enforced per node**, not by a pipeline-wide equality check.
  It describes the resolution carried inside the canonical S32_LE container, and which
  depths a node supports is a property of that node. v1's file reader and file writer both
  support 16-bit only, so a bound format asking for 24-bit fails loudly at `open()` (§5.3).

This is what makes a mismatch impossible to observe as a silently mislabelled file: a
44.1 kHz mono track can only reach a 44.1 kHz mono sink, because both were checked against
the same bound format before either produced a sample.

#### Reconfiguration

The bound format may be replaced between runs, but **only while the node chain is closed** —
before the first `audio_pipeline_start()`, or after `audio_pipeline_join()`. Nodes read the
format in `open()` and keep using it for as long as they are open, so changing it underneath
an open chain would leave nodes holding a stale format. `set_format()` returns `-EBUSY`
otherwise (§8.1).

This lifts the v1 restriction that the format is static for the entire runtime: it is static
for the duration of a run, and rebindable between runs.

### 5.3 Format conversion

- Sources handle conversion from their input format to S32_LE:
  - 16-bit PCM → `int32_t s32 = (int32_t)s16 << 16;`
  - 24-bit PCM → `int32_t s32 = (int32_t)s24 << 8;`
- Sinks convert S32_LE back to their target format:
  - e.g., `int16_t s16 = (int16_t)(s32 >> 16);`

Filters expect and produce S32_LE only.

---

## 6. Pipeline Object

### 6.1 Structure

One possible design for `struct audio_pipeline` (details may vary but should include these elements):

```c
struct audio_pipeline {
    struct audio_node *source;
    struct audio_node *sink;
    struct audio_node **filters;
    size_t filter_count;

    /* Bound format, owned by the pipeline and installed on every node before
     * open() (§5.2). Written only by audio_pipeline_set_format(). */
    struct audio_stream_config format;
    bool format_bound;

    struct k_thread thread;
    k_thread_stack_t *stack;
    size_t stack_size;
    int priority;

    int32_t *frame_buffer;
    size_t frame_capacity; /* in samples */

    /* Event queue is embedded, not a pointer to an external msgq: the
     * AUDIO_PIPELINE_DEFINE macro owns the slot storage so that two
     * macro-defined pipelines cannot share one queue. Zero-initialised
     * instances fall back to built-in slots in audio_pipeline_init(). */
    struct k_msgq event_msgq;
    struct audio_pipeline_event *event_slots;
    size_t event_slot_count;

    bool initialized;
    bool running;
    bool playing;
};
```

#### Built-in resources and their ownership

`stack`, `frame_buf` and `event_slots` are the caller's seam. Left NULL they select the
subsystem's built-in objects — one thread stack, one frame buffer, one set of event slots, all
file-scope statics in the core.

Because there is only one of each, they are **claimed**, not shared:

- `audio_pipeline_init()` claims every built-in the instance leaves NULL and installs it. If
  another instance already holds one of them, init fails with `-EBUSY` and writes nothing: a fresh
  instance stays zeroed and not `initialized`, and an instance that was joined out of its built-ins
  keeps its previous configuration and pointers instead of being rebound.
- Ownership is tracked per resource, so an instance that brings its own stack but no frame buffer
  contends only for the frame buffer.
- Re-initialising an instance that already owns a built-in succeeds — rebinding a pipeline to a new
  configuration or sink must not lock it out of its own storage.
- `audio_pipeline_join()` releases whatever the instance holds, which is what makes
  init → join → init hand the built-ins on to the next instance.
- `audio_pipeline_start()` claims again, because a joined instance still points at resources it has
  given back. It returns `-EBUSY` if they were taken in the meantime, and publishes **no** ERROR
  event in that case — the event queue is one of the resources it no longer owns.

Two pipelines that must run concurrently therefore need `AUDIO_PIPELINE_DEFINE()` (or caller-owned
storage) for at least one of them; that path allocates per instance and never touches the built-ins.

Two obligations fall on the caller, because the guard covers the entry points that *take* the
resources, not every use of them:

- An instance running on the built-ins must be joined before it is discarded. `join()` is the only
  release; there is no deinit, and an instance that is initialised and then abandoned holds them
  for the lifetime of the process.
- Between a `join()` and the next successful `init()`/`start()` the instance still points at the
  built-ins but owns none of them. `audio_pipeline_process_frame()` and
  `audio_pipeline_get_event()` must not be called in that window — they would read and write the
  new owner's frame buffer and event ring.

### 6.2 Static definition

The pipeline is defined statically via a macro, e.g.:

```c
AUDIO_PIPELINE_DEFINE(my_pipeline,
    .frame_samples = CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES,
    .stack_size    = CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE,
    .priority      = CONFIG_AUDIO_PIPELINE_THREAD_PRIO);
```

The macro:
- instantiates `struct audio_pipeline my_pipeline`,
- creates `K_THREAD_STACK_DEFINE` for the pipeline thread,
- creates a static frame buffer:
  ```c
  static int32_t my_pipeline_frame_buf[CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES * 2];
  ```
  (2 = channels),
- ties these to the pipeline struct.

---

## 7. Kconfig Options

Minimal set of configuration options:

```kconfig
config AUDIO_PIPELINE
    bool "Audio processing pipeline framework"

config AUDIO_PIPELINE_FRAME_SAMPLES
    int "Samples per frame (per channel)"
    default 64
    range 8 1024

config AUDIO_PIPELINE_THREAD_STACK_SIZE
    int "Stack size for pipeline worker thread"
    default 2048

config AUDIO_PIPELINE_THREAD_PRIO
    int "Priority of pipeline worker thread"
    default 5

config AUDIO_PIPELINE_EVENT_QUEUE_DEPTH
    int "Number of events buffered per pipeline"
    default 4
    range 1 32
```

---

## 8. Pipeline API

### 8.1 Initialization & configuration

```c
int audio_pipeline_init(struct audio_pipeline *pl);

int audio_pipeline_set_nodes(struct audio_pipeline *pl,
                             struct audio_node *source,
                             struct audio_node **filters,
                             size_t filter_count,
                             struct audio_node *sink);

int audio_pipeline_set_format(struct audio_pipeline *pl,
                              const struct audio_stream_config *fmt);
```

Agreements:

- `audio_pipeline_init()`:
  - sets internal flags,
  - claims and installs the built-in stack, frame buffer and event slots for every resource field
    the instance left NULL (§6.1), returning `-EBUSY` when another instance holds one of them,
  - initializes the event queue,
  - may be called again on the same instance to rebind it (that is not a second claim).
- `audio_pipeline_set_nodes()`:
  - assigns `source`, filter list, and `sink`,
  - internally links `upstream` pointers (Filter[i].upstream = (i==0 ? source : Filter[i-1]); sink.upstream = last filter or source).
- `audio_pipeline_set_format()`:
  - copies `fmt` into pipeline-owned storage, where it becomes the one format valid for the whole
    pipeline (§5.2). The caller's struct is not retained and may be a temporary.
  - is the **only** way to bind a format. `audio_pipeline_config` carries no format field, so an
    application cannot declare one statically and then wonder which of the two is live.
  - must be called after `audio_pipeline_init()` — it writes instance storage that `init()` binds —
    and returns `-EINVAL` on a pipeline that is not initialised, or on a `fmt` a node could never
    satisfy (zero `sample_rate_hz`, zero `channels`).
  - is legal only while the **node chain is closed**: before the first `start()`, or after
    `join()`. It returns `-EBUSY` while the nodes are open, whether the pipeline is playing or
    merely idle, because nodes read the format at `open()` and hold it until they are closed.
  - is subject to §3.3 like every other `audio_pipeline_*` entry point: control thread only. That
    is what makes a lock unnecessary — the worker thread never reads the bound format, it only
    walks nodes that were handed it at open time.
  - is cleared by a subsequent `audio_pipeline_init()`. Re-initialising rebinds the instance to a
    new configuration and sink, and a format carried over from the previous binding would be a
    stale default nobody chose; `start()` then reports `-ENODATA` until a format is set again.

### 8.2 Lifecycle

```c
int audio_pipeline_start(struct audio_pipeline *pl);
int audio_pipeline_play(struct audio_pipeline *pl);
int audio_pipeline_stop(struct audio_pipeline *pl); /* stops playing */
int audio_pipeline_join(struct audio_pipeline *pl); /* optional: wait for thread end */
```

Recommended behavior:

- `audio_pipeline_start()`:
  - refuses a pipeline with **no bound format** (§5.2) with `-ENODATA`, before it claims anything
    or creates a thread. The code is distinct from the `-EINVAL` of a malformed configuration so
    that "you never called `set_format()`" is not confused with "your configuration is wrong",
  - reclaims the built-in resources the instance runs on (§6.1), failing with `-EBUSY` and without
    an ERROR event if another instance took them after a join,
  - creates the thread (if not already existing),
  - installs the bound format on each node (§4.1) and opens all nodes via `node->ops->open`. A node
    that cannot accept the format fails its open, which fails `start()` (§5.2).
- `audio_pipeline_play()`:
  - sets `pl->playing = true`,
  - pipeline thread begins pulling frames.
- `audio_pipeline_stop()`:
  - sets `pl->playing = false`,
  - thread stays alive but idles/waits.
- `audio_pipeline_join()`:
  - optional: ends thread (cleanup scenarios),
  - releases any built-in resource the instance holds (§6.1), after the closing errors have been
    published, so the next hand-rolled pipeline can claim it.

### 8.3 Events

```c
enum audio_pipeline_event_type {
    AUDIO_PIPELINE_EVENT_EOF,
    AUDIO_PIPELINE_EVENT_ERROR,
    AUDIO_PIPELINE_EVENT_RECONFIG,
};

struct audio_pipeline_event {
    enum audio_pipeline_event_type type;
    int err; /* optional: error code for ERROR */
};
```

API (example):

```c
int audio_pipeline_get_event(struct audio_pipeline *pl,
                             struct audio_pipeline_event *evt,
                             k_timeout_t timeout);
```

Behavior:

- EOF: As soon as a sink receives `out_size == 0`, an `AUDIO_PIPELINE_EVENT_EOF` is generated.
- ERROR: If any node returns < 0 from `process()` or `open()`/`close()`, the pipeline generates `AUDIO_PIPELINE_EVENT_ERROR` and sets `evt.error` accordingly.

---

## 9. EOF & Error Behavior

### 9.1 EOF

- Source signals EOF with `out_size == 0` and return value `0`.
- Filters propagate this state unchanged.
- Sink detects EOF and informs the pipeline.
- Pipeline:
  - sets `playing = false`,
  - generates `AUDIO_PIPELINE_EVENT_EOF`.

### 9.2 Errors

- Any negative return value from `open()`, `process()`, `close()` is an error.
- `-EPIPE` is **reserved** for end of stream: `audio_pipeline_process_frame()` returns it when the sink reports `out_size == 0`, and the worker thread turns that into a clean EOF event. No node may report `-EPIPE` as a failure. `audio_node_pull()` (§4.1.1) enforces this on the upstream boundary; a node that talks to a filesystem remaps its own `-EPIPE` to `-EIO` the same way.
- On the first error:
  - pipeline stops further frame processing (`playing = false`),
  - generates `AUDIO_PIPELINE_EVENT_ERROR`,
  - optionally invokes `close()` on all nodes.

---

## 10. Example Nodes (v1)

### 10.1 File reader node (source)

- Task:
  - Opens a WAV file (e.g., via RAMFS or LittleFS),
  - parses the header (PCM, stereo, 16-bit),
  - delivers S16 data as S32_LE into the pipeline.

- Context struct (example):

```c
/* Implemented as struct audio_file_reader_state in
 * include/zephyr/audio/audio_nodes.h - the sketch below predates it. */
struct audio_file_reader_state {
    const char *path;       /* set by AUDIO_FILE_READER_NODE_DEFINE */
    struct fs_file_t file;
    struct audio_format fmt;
    size_t bytes_left;      /* remaining declared payload */
    bool file_open;
    bool eof;
};
```

- `open()`:
  - open file,
  - parse header,
  - set `fmt` from the parsed header — this is the file's **real** format, and it is what
    the node validates against `node->pipeline_format` (§4.1),
  - **reject a file that disagrees with the bound format** (§5.2): a `sample_rate_hz` or
    `channels` other than the pipeline's returns `-ENOTSUP` and the file is closed again.
    v1 has no resampler, so the reader cannot convert a 44.1 kHz file for a 48 kHz
    pipeline — it can only refuse it,
  - `bytes_read = 0`, `eof = false`.
- `process()`:
  - reads `capacity` * 2 (channels) * 2 (bytes per sample) from file,
  - converts `int16_t` → `int32_t` into `buf`,
  - sets `*out_size` (in samples, not bytes).
- `close()`:
  - close file.

### 10.2 File writer node (sink)

- Task:
  - Accepts S32_LE,
  - converts to desired output format (e.g., 16-bit PCM),
  - writes to file.

Implementation mirrors the reader, in reverse. Two decisions are contract:

- **Header sizes: placeholder, then patch.** `open()` serialises a canonical
  44-byte header through the WAV module (§10.3) declaring an empty chunk (RIFF
  size 36, `data` size 0) *before* it creates the file, so a format the module
  refuses leaves no truncated file behind. The real sizes are
  patched by seeking to 0, rewriting the header, `fs_sync()`, then seeking back to
  end. This happens on **end of stream as well as in `close()`**, so the file is
  already valid when the pipeline reports EOF — before `join()`. `data_size` counts
  only bytes the filesystem confirmed, so it can never exceed the payload on disk.
  An aborted run therefore leaves a structurally valid header declaring an empty
  track: a reader sees immediate EOF, never a bogus length.
- **The output format comes from the pipeline, not from the node.** `open()` reads
  `node->pipeline_format` (§4.1) and writes exactly that into the WAV header, so the
  header can never describe a stream different from the one the pipeline carries. The
  node resolves **no defaults**: there is no 48 kHz/2-channel fallback, because a format
  is always bound before `start()` runs (§5.2) and a sink guessing one would be the very
  mislabelling this seam exists to prevent. `audio_file_writer_state.fmt` keeps the
  resolved format observable after `open()`, but it is now a copy of the pipeline's
  format rather than an independent source of truth.
- **The writer refuses what it cannot emit.** v1 writes 16-bit PCM into at most two
  channels, so `open()` returns `-ENOTSUP` for a bound format with
  `valid_bits_per_sample != 16` or `channels > 2` (§5.2), before it creates the file.

Conversion is **truncation toward negative infinity** — keep the top 16 bits,
`(uint16_t)((uint32_t)sample >> 16)`. No rounding bias and no clipping: a 32-bit
value shifted down by 16 always lands in `[-32768, 32767]`, so clamping cannot be
needed. Round-to-nearest is rejected deliberately — the `+0x8000` bias overflows
`int32_t` just below `INT32_MAX` and pushes full scale out of the int16 range.
Truncation is also the exact inverse of the reader's `s16 << 16`, which makes the
roundtrip bit-identical.

### 10.3 WAV header module (shared)

Both file nodes delegate the RIFF/WAVE byte layout to one module,
`include/zephyr/audio/audio_wav.h` + `subsys/audio/pipeline/audio_wav.c`. It is
public API, allocation free and filesystem free: it only maps a byte buffer to
and from a `struct audio_wav_header`.

```c
int audio_wav_read_header(const uint8_t *data, size_t len, struct audio_wav_header *out);
int audio_wav_write_header(uint8_t *buf, size_t len, const struct audio_wav_header *hdr);
```

- **One record, both directions.** `sample_rate_hz`, `data_size`, `format_tag`,
  `channels` and `bits_per_sample` describe the stream and are read and written;
  `data_offset` and `block_align` are derived — outputs of a read, ignored by a
  write.
- **The reader walks the chunk list**, so `JUNK`/`LIST`/`fact` chunks around
  `fmt ` and `data` are skipped and a short prefix of the file is enough
  (`AUDIO_WAV_HEADER_SCAN_SIZE`). The writer emits only the canonical
  `AUDIO_WAV_MIN_HEADER_SIZE` (44) byte form with no payload.
- **Both halves share one definition of a usable format**, so the writer can
  never emit a header the reader rejects: `-EINVAL` for a degenerate `fmt `
  field, `-EFBIG` for a payload past `AUDIO_WAV_MAX_DATA_SIZE`. The one
  asymmetry is deliberate — a `format_tag` other than `AUDIO_WAV_FORMAT_PCM` is
  serialised verbatim and read back as `-ENOTSUP`, which is what lets a test
  produce a non-PCM file without spelling out field offsets.
- **Nothing outside this module derives the layout.** The file writer sink
  supplies the stream description and the module lays out the bytes; the same
  holds for the reference sample and the test fixture.

---

## 11. Memory & Module Structure

### 11.1 No dynamic allocation

- Within the subsystem there is **no** dynamic allocation (`k_malloc`, `k_calloc`, `k_free`).
- All structures (pipeline, nodes, contexts, buffers) are static or user-provided.

### 11.2 Definition macros

- `AUDIO_PIPELINE_DEFINE(name, ...)`  
  - allocates pipeline + thread stack + frame buffer.
- `AUDIO_FILE_READER_NODE_DEFINE(name, path)`  
  - statically allocates `struct audio_node` and `struct audio_file_reader_state`.
  - Macros carry the `AUDIO_` prefix per AGENTS.md; see `audio_nodes.h` for the full set.

Concrete macros can be refined during implementation but must honor this principle.

---

## 12. Test Strategy

### 12.1 Goal

- Tests should run entirely on **QEMU**, without real audio hardware.
- Focus on:
  - Correct data propagation,
  - EOF behavior,
  - Error paths.

### 12.2 Roundtrip test

Example setup:

```text
[file_reader_source] -> [optional filter] -> [file_writer_sink]
```

Test steps:

1. Mount a known WAV file (golden master) in RAMFS.
2. Run the pipeline until EOF.
3. Compare output file against the golden master:
   - File size identical,
   - Byte-for-byte identical.

### 12.3 Negative tests

- Corrupted WAV header → `open()` must fail.
- Early EOF → pipeline must emit a clean EOF event.
- Simulated I/O errors → ERROR event.
- A second hand-rolled pipeline claiming the built-in resources (§6.1) → `-EBUSY` from `init()`,
  the first claimant unaffected, and the built-ins reusable after its `join()`. The two instances
  must be distinguishable (different frame sizes, sample patterns and frame counts), otherwise the
  test would still pass if the guard were removed.

---

## 13. Extension Points for Later Versions

- **Float DSP**:
  - Introduce converter nodes `s32_to_float`, `float_to_s32`.
- **Multi-channel support**:
  - Lift the 2-channel restriction.
- **Mixer/Splitter**:
  - Extend the node model to multiple upstream/downstream links.
- **Timer-paced pipeline**:
  - Optional mode that emits frames in real time based on sample rate.

---

## 14 Project Structure

```

zephyr-audio-pipeline/
├─ zephyr/module.yml      # module manifest; Zephyr only looks here
├─ CMakeLists.txt
├─ Kconfig
├─ include/
│  └─ zephyr/
│     └─ audio/
│        ├─ audio_format.h
│        ├─ audio_node.h
│        ├─ audio_nodes.h        # per-node state types, ops externs, node DEFINE macros
│        ├─ audio_pipeline.h
│        ├─ audio_pipeline_events.h
│        └─ audio_wav.h          # RIFF/WAVE header: read and write, one byte layout
├─ subsys/
│  └─ audio/
│     └─ pipeline/
│        ├─ CMakeLists.txt
│        ├─ Kconfig
│        ├─ audio_pipeline_core.c
│        ├─ audio_pipeline_config.c
│        ├─ audio_pipeline_events.c
│        ├─ audio_node_core.c
│        ├─ audio_internal.h
│        ├─ audio_wav.c
│        └─ nodes/
│            ├─ file_reader_node.c
│            ├─ file_writer_node.c
│            ├─ gain_filter_node.c
│            └─ null_sink_node.c
├─ samples/
│  └─ audio/
│     └─ pipeline_basic/
│        ├─ CMakeLists.txt
│        ├─ Kconfig
│        └─ src/main.c
└─ tests/
   └─ subsys/
      └─ audio/
         └─ pipeline/
            ├─ CMakeLists.txt
            ├─ Kconfig
            ├─ test_roundtrip.c
            └─ test_error_paths.c

```


## 15. Summary

This specification defines:

- A pull-based audio pipeline system,
- with one worker thread per pipeline,
- static memory allocation,
- 32-bit internal sample format (S32_LE),
- clear roles (source/filter/sink),
- prescribed EOF and error behavior,
- Zephyr-style API and error codes,
- and a simple yet extensible event system.

It forms the complete technical contract that the implementation must follow.
