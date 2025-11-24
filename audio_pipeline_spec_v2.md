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
- No dynamic runtime reconfiguration of the pipeline (format & structure are static in v1).
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

---

## 4. Role Model: Source, Filter, Sink

### 4.1 Common node type

```c
enum audio_node_role {
    AUDIO_NODE_ROLE_SOURCE,
    AUDIO_NODE_ROLE_FILTER,
    AUDIO_NODE_ROLE_SINK,
};

struct audio_node_ops {
    int (*open)(struct audio_node *node);
    ssize_t (*process)(struct audio_node *node,
                       int32_t *buf,
                       size_t capacity,   /* in samples */
                       size_t *out_size); /* in samples */
    int (*close)(struct audio_node *node);
};

struct audio_node {
    enum audio_node_role role;
    const struct audio_node_ops *ops;
    struct audio_node *upstream; /* NULL for sources */
    void *context;               /* implementation-specific state */
};
```

**Naming convention:**  
The function pointers in `audio_node_ops` are named **`open`**, **`process`**, **`close`** (“opc”), as requested.

**Error codes:**  
- `open()` and `close()` return `int` with Zephyr error codes (`0` on success, `< 0` on failure; e.g., `-EINVAL`, `-EIO`, `-ENOMEM`).
- `process()` returns `ssize_t`:
  - `>= 0`: number of samples actually produced (mirrored in `out_size`),
  - `< 0`: error code (e.g., `-EIO`).

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
  - first calls `upstream->ops->process(...)`,
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
  - calls `upstream->ops->process(...)`,
  - processes or consumes the data (e.g., writes to a file),
  - the pipeline mostly cares about EOF/errors:
    - If `*out_size == 0`: end-of-stream detected.

Note:
- Unlike many frameworks, the sink does not have to write into `buf`; it uses it as a transient transport buffer.

---

## 5. Data Format

### 5.1 Canonical internal format

- Data type: `int32_t`
- Endianness: Little Endian
- Enum value: `AUDIO_SAMPLE_FORMAT_S32_LE`
- All nodes work internally with 32-bit samples.

### 5.2 Valid bits per sample

```c
struct audio_format {
    uint32_t sample_rate;           /* Hz, e.g. 44100, 48000 */
    uint8_t  channels;              /* v1: always 2 */
    uint8_t  valid_bits_per_sample; /* e.g. 16, 24, 32 */
    enum audio_sample_format format;/* internal: AUDIO_SAMPLE_FORMAT_S32_LE */
};
```

- In v1 `channels` is fixed to **2** (stereo).  
- Samples are **interleaved** in the buffer:

```text
buf: L0, R0, L1, R1, L2, R2, ...
```

- `valid_bits_per_sample` describes the effective resolution:
  - e.g., `16` for data converted from 16-bit PCM,
  - `24` or `32` for higher resolution.
- Sample rate and channel count are pipeline-wide settings defined once with the pipeline format.

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

    struct audio_format format;

    struct k_thread thread;
    k_thread_stack_t *stack;
    size_t stack_size;
    int priority;

    int32_t *frame_buffer;
    size_t frame_capacity; /* in samples */

    struct k_msgq *event_queue;

    bool initialized;
    bool running;
    bool playing;
};
```

### 6.2 Static definition

The pipeline is defined statically via a macro, e.g.:

```c
AUDIO_PIPELINE_DEFINE(my_pipeline,
    .frame_samples = CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES,
    .stack_size    = CONFIG_AUDIO_PIPELINE_STACK_SIZE,
    .priority      = CONFIG_AUDIO_PIPELINE_PRIORITY);
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

config AUDIO_PIPELINE_STACK_SIZE
    int "Stack size for pipeline worker thread"
    default 2048

config AUDIO_PIPELINE_PRIORITY
    int "Priority of pipeline worker thread"
    default 5
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
                              const struct audio_format *fmt);
```

Agreements:

- `audio_pipeline_init()`:
  - sets internal flags,
  - initializes the event queue,
  - may be called only once per pipeline instance.
- `audio_pipeline_set_nodes()`:
  - assigns `source`, filter list, and `sink`,
  - internally links `upstream` pointers (Filter[i].upstream = (i==0 ? source : Filter[i-1]); sink.upstream = last filter or source).
- `audio_pipeline_set_format()`:
  - sets the `audio_format` valid for the whole pipeline.  
  - v1: format is static for the entire runtime.

### 8.2 Lifecycle

```c
int audio_pipeline_start(struct audio_pipeline *pl);
int audio_pipeline_play(struct audio_pipeline *pl);
int audio_pipeline_stop(struct audio_pipeline *pl); /* stops playing */
int audio_pipeline_join(struct audio_pipeline *pl); /* optional: wait for thread end */
```

Recommended behavior:

- `audio_pipeline_start()`:
  - creates the thread (if not already existing),
  - opens all nodes via `node->ops->open`.
- `audio_pipeline_play()`:
  - sets `pl->playing = true`,
  - pipeline thread begins pulling frames.
- `audio_pipeline_stop()`:
  - sets `pl->playing = false`,
  - thread stays alive but idles/waits.
- `audio_pipeline_join()`:
  - optional: ends thread (cleanup scenarios).

### 8.3 Events

```c
enum audio_pipeline_event_type {
    AUDIO_PIPELINE_EVENT_EOF,
    AUDIO_PIPELINE_EVENT_ERROR,
    AUDIO_PIPELINE_EVENT_RECONFIG,
};

struct audio_pipeline_event {
    enum audio_pipeline_event_type type;
    int error; /* optional: error code for ERROR */
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
struct file_reader_context {
    struct fs_file_t file;
    struct audio_format fmt;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t bytes_read;
    bool eof;
};
```

- `open()`:
  - open file,
  - parse header,
  - set `fmt`,
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

Implementation mirrors the reader, in reverse.

---

## 11. Memory & Module Structure

### 11.1 No dynamic allocation

- Within the subsystem there is **no** dynamic allocation (`k_malloc`, `k_calloc`, `k_free`).
- All structures (pipeline, nodes, contexts, buffers) are static or user-provided.

### 11.2 Definition macros

- `AUDIO_PIPELINE_DEFINE(name, ...)`  
  - allocates pipeline + thread stack + frame buffer.
- `FILE_READER_NODE_DEFINE(name, path)`  
  - statically allocates `struct audio_node` and `struct file_reader_context`.

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
├─ module.yml
├─ CMakeLists.txt
├─ Kconfig
├─ include/
│  └─ zephyr/
│     └─ audio/
│        ├─ audio_format.h
│        ├─ audio_node.h
│        ├─ audio_pipeline.h
│        └─ audio_pipeline_events.h
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
│        ├─ nodes/
│        │   ├─ file_reader_node.c
│        │   ├─ file_writer_node.c
│        │   ├─ gain_filter_node.c
│        │   └─ null_sink_node.c
│        └─ util/
│            ├─ wav_parser.c
│            └─ wav_parser.h
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
