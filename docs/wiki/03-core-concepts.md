# Core concepts

Seven ideas carry the whole subsystem. Once these are in place, the API reads as the
obvious consequence of them.

1. [The chain is wired at build time](#1-the-chain-is-wired-at-build-time)
2. [Data flows by pull](#2-data-flows-by-pull)
3. [One frame buffer, borrowed](#3-one-frame-buffer-borrowed)
4. [One sample container](#4-one-sample-container-s32_le-left-justified)
5. [One format, bound top-down](#5-one-format-bound-top-down)
6. [End of stream is not an error](#6-end-of-stream-is-not-an-error)
7. [One worker thread, one control thread](#7-one-worker-thread-one-control-thread)

## 1. The chain is wired at build time

A node is a `struct audio_node`: a role, three ops, an `upstream` pointer, a `state`
pointer and the format the pipeline installed.

```c
struct audio_node {
	enum audio_node_role role;          /* SOURCE | FILTER | SINK */
	const struct audio_node_ops *ops;   /* open / process / close  */
	struct audio_node *upstream;        /* NULL for a source       */
	void *state;                        /* the node's private data */
	const struct audio_stream_config *pipeline_format;
};
```

The `*_NODE_DEFINE()` macros allocate the node *and* its state and set `upstream` from the
argument you pass, so nodes must be defined in dataflow order — source first, sink last:

```c
AUDIO_FILE_READER_NODE_DEFINE(reader, "/ram/track.wav");
AUDIO_GAIN_FILTER_NODE_DEFINE(gain, &reader, AUDIO_GAIN_UNITY_Q15 / 2);
AUDIO_NULL_SINK_NODE_DEFINE(sink, &gain);
```

The pipeline is then given **only the sink**. Everything upstream is reached by walking
`->upstream`, which is also why the walk is bounded: `AUDIO_PIPELINE_MAX_CHAIN_DEPTH` is
16, and a deeper (or cyclic) chain is refused with `-ELOOP` instead of looping forever.

There is no run-time `connect()` call and no graph object. A chain the board cannot
support fails to build rather than failing to start.

## 2. Data flows by pull

The worker thread calls `process()` on the **sink only**. Every sink and filter reaches its
upstream through one function:

```c
int audio_node_pull(struct audio_node *node, struct audio_buffer_view *buf,
		    size_t *out_size);   /* pass *yourself* as node */
```

`audio_node_pull()` is the single implementation of the pull contract, and it owns three
decisions so they cannot drift from node to node:

* **Wiring policy.** A filter or sink with no upstream is a wiring error — `-ENOTSUP`,
  never an empty track (which would silently swallow the stream).
* **The reserved code.** `-EPIPE` arriving from below is remapped to `-EIO`, so a broken
  upstream can never reach the application looking like a finished one.
* **End of stream.** `*out_size == 0` with a return of `0`, forwarded verbatim; `*out_size`
  is `0` on every failure too.

How often a node pulls stays its own business: a future resampler may pull several times
per frame, a mixer once per upstream.

## 3. One frame buffer, borrowed

A pipeline owns exactly one frame buffer. It is handed down the chain as a view:

```c
struct audio_buffer_view {
	int32_t *data;     /* frame storage, owned by the PIPELINE */
	size_t capacity;   /* samples it can hold */
};
```

The buffer is **borrowed** for the duration of one `process()` call and reused for the next
frame. Nodes read and write it in place — the gain filter multiplies in place, the file
reader widens 16-bit payload to 32-bit in place, back to front, so it needs no scratch
buffer at all.

Two consequences worth internalising:

* A node that must hand memory to a driver that takes ownership (the I2S sink) has to
  **copy**. That copy is the seam between two ownership models, not an oversight.
* `capacity` and `out_size` count **total interleaved samples across all channels**, never
  per channel. `frame_samples = 128` at two channels is 64 sample pairs. Latency per
  iteration is `frame_samples / (sample_rate_hz * channels)` — 128 total samples at 48 kHz
  stereo is ~1.33 ms.

Nodes must never split an interleaved sample set across two frames; every shipped node
rounds down to whole sample sets for exactly this reason. A set that straddled a frame
boundary would leave every following frame with its channels transposed.

## 4. One sample container: S32_LE, left-justified

`AUDIO_SAMPLE_FORMAT_S32_LE` is the only value the format enum carries, and it means a
signed 32-bit integer holding a **left-justified, full-scale (Q31)** value.

A source normalises its wire depth into that container by shifting up:

```c
s32 = (int32_t)((uint32_t)(int32_t)s16 << 16);   /* file reader, I2S wire, tone gen */
```

and a sink narrows by shifting back down by 16 — plain truncation, which is the exact
inverse, so a file → pipeline → file round trip is bit identical.

`valid_bits_per_sample` in the format describes the **wire**, not the container. A node
reads it to decide what it can carry (the file writer and the I2S link accept 16 in v1),
never to discover what scale the samples are at. The samples are always Q31.

Why 32 bits: one container at one fixed scale removes per-depth code paths from every
filter. A Q15 coefficient means the same thing whether the audio arrived as 16, 24 or
32 bit. It is explicitly **not** about headroom — a full-scale 16-bit input already sits
near `INT32_MAX` after its shift. Headroom lives in the filter's intermediate: the gain
filter widens to `int64_t`, applies the gain and shifts back.

> ⚠️ **Known defect:** that store back to `int32_t` has no clamp, so a gain **above** unity
> on a near-full-scale sample wraps rather than clips — loud positive becomes loud
> negative. Tracked in issue #39. Keep `gain_q15 <= AUDIO_GAIN_UNITY_Q15` until it is
> fixed. Full reasoning in
> [ADR 0002](../adr/0002-internal-container-is-left-justified-q31-int32.md).

## 5. One format, bound top-down

```c
struct audio_stream_config {
	uint32_t sample_rate_hz;
	uint8_t channels;
	uint8_t valid_bits_per_sample;
	enum audio_sample_format format;
};
```

The rules, all enforced in code:

* The format is **not** part of `struct audio_pipeline_config`. It is bound at run time by
  `audio_pipeline_set_format()` **and nowhere else**, so there is never a second place to
  look for the format a run is using.
* `audio_pipeline_start()` installs it into `node->pipeline_format` **immediately before**
  opening that node, and leaves it there until the node is closed.
* A node **validates** the format in its own `open()` and refuses with `-ENOTSUP` what it
  cannot carry. v1 has no resampler and no channel mapper: a node can match or refuse,
  never adapt.
* Rebinding while the chain is open is `-EBUSY`. Nodes hold the format across EOF and
  `stop()`, so "not playing" would not be tight enough; only `join()` reopens the window.
* Starting without a bound format is `-ENODATA` — deliberately distinct from the `-EINVAL`
  of a malformed configuration, so "you forgot `set_format()`" cannot be confused with
  "your config is wrong".

The pipeline itself validates only what the pipeline owns: a non-zero rate and channel
count, and that one interleaved sample set fits the frame buffer
(`frame_capacity >= channels`). A node's limits live in that node's `open()` and nowhere
else — restating them in the pipeline would create a second opinion that can disagree.
This is [ADR 0001](../adr/0001-pipeline-format-is-a-contract-nodes-refuse.md).

## 6. End of stream is not an error

There is exactly one way to say "the stream finished":

> a `process()` that returns `0` with `*out_size == 0`.

That travels up the chain unchanged. When the **sink** produces zero samples,
`audio_pipeline_process_frame()` turns it into `-EPIPE` for its caller — the worker then
stops pulling, keeps the chain open and publishes `AUDIO_PIPELINE_EVENT_EOF`. A second
`play()` runs another track without reopening anything.

Because `-EPIPE` is reserved for that signal, any `-EPIPE` entering the subsystem from
outside is remapped to `-EIO` by `audio_eof_safe_errno()`. Every boundary an error can
arrive through funnels through that one function: `audio_node_pull()`, the filesystem calls
in the file nodes, the I2S calls, and the sink's return in `process_frame()`. A failing
node can therefore never be mistaken for a finished one.

The mirror image matters just as much: a node must never report an empty frame for a
condition that is not end of stream. A too-small buffer is `-EINVAL`, a `process()` before
`open()` is `-EBADF`, and the I2S source — a live input that has no end at all — answers a
read timeout with an error, because an empty frame there would report a broken wire as a
clean end of stream.

## 7. One worker thread, one control thread

* **One worker thread per pipeline.** It is created by `start()`, survives EOF, `stop()`
  and node errors, and is ended only by `join()`. While not playing it blocks on a
  semaphore rather than spinning; after each successful frame it calls `k_yield()`.
* **The control API is confined to one control thread** (`init`, `set_format`, `start`,
  `play`, `stop`, `join`). That confinement is why the bound format needs no lock: it is
  written by `set_format()` and read by `open()` on the same thread.
* **Nodes may block inside `process()`.** That is how a hardware sink paces the entire pull
  chain against the codec's clock. It is also why `audio_pipeline_stop()` is asynchronous
  by contract: it must not deadlock behind a blocking sink, so the frame in flight may
  still complete.
* **Events cross threads.** `audio_pipeline_get_event()` is plain `k_msgq` semantics and may
  be read from any thread. The optional callback runs on the *publishing* thread — the
  worker for EOF and processing errors, the control thread for open/close failures — and
  must not block.
* **Everything is static.** The subsystem never calls `k_malloc()`. `AUDIO_PIPELINE_DEFINE()`
  allocates the stack, frame buffer and event slots per instance; a zero-initialised
  instance instead claims the subsystem's single set of built-ins, and a second claimant
  gets `-EBUSY` rather than silently sharing them.

One state field, an `atomic_t`, encodes all of it — initialised, chain open, thread alive,
pulling. See [Pipeline lifecycle](05-pipeline-lifecycle.md).

## Where to go next

* Why these choices, and what was rejected: [Architecture](04-architecture.md).
* The state machine and every errno: [Pipeline lifecycle](05-pipeline-lifecycle.md).
* What the shipped nodes make of these rules: [Node reference](06-node-reference.md).
