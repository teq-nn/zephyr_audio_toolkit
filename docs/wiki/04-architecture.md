# Architecture and why it is shaped this way

[Core concepts](03-core-concepts.md) says *what* the rules are. This article says *why*,
and what each decision costs. Every claim below is anchored in a file you can open.

## 1. Pull, not push

The worker calls `process()` on the sink; the sink pulls from its upstream. Nothing pushes.

*What it buys.* The sink is the only node that knows how fast the stream must run, because
it is the one attached to the clock — an I2S peripheral, a filesystem, a codec. In a pull
chain that pacing falls out for free: `i2s_write()` blocks until the wire has room, the
whole chain blocks with it, and the pipeline runs at exactly the rate the hardware
consumes. A push chain would need a rate-matching buffer at every boundary.

*What it costs.* A node that wants data from several upstreams (a mixer) has to pull each
one itself, and the single shared frame buffer means it has to do so serially. That is
allowed — "when and how often a node pulls stays the node's own business" — but v1 ships no
such node.

*Where.* `pipeline_thread()` and `audio_pipeline_process_frame()` in
`audio_pipeline_core.c`; `audio_node_pull()` in `audio_node_core.c`.

## 2. Static memory only, allocated by macros

The subsystem never calls `k_malloc()`. `AUDIO_PIPELINE_DEFINE()` allocates the thread
stack, the frame buffer and the event queue storage; each `*_NODE_DEFINE()` allocates its
node's private state, and the I2S macros allocate a `k_mem_slab` of transfer blocks per
instance.

*What it buys.* Memory is decided at link time and visible in the map file — the property
an audio path needs most, because a failed allocation mid-stream has no graceful answer.
It also makes two instances of a node structurally unable to share state.

*What it costs.* Sizes are compile-time constants. A frame buffer is as large as the
largest format you intend to run, and you pay for it whether you run that format or not.
The application must also pass consistent numbers in two places (the pipeline's
`frame_samples` and the I2S nodes' `_frame_samples`), which the macros guard with
`BUILD_ASSERT`s but cannot infer.

*Where.* `AUDIO_PIPELINE_DEFINE()` in `audio_pipeline.h`; the node macros in
`audio_nodes.h`.

## 3. One worker thread per pipeline, and it outlives everything but `join()`

The thread is created by the first `start()` and returns only when `join()` sets
`quit_request`. EOF parks it. A node error parks it. `stop()` parks it.

*What it buys.* A restart after EOF or after a node error costs no thread creation, and
`audio_pipeline_is_running()` keeps meaning "there is a worker" across all of them. Keeping
`quit_request` *outside* the state enum is deliberate: "leave the loop" applies to every
state that holds a thread, so folding it in would double the enum and let the worker's own
compare-and-swap overwrite a pending `join()`.

*What it costs.* An idle pipeline still owns a thread and its stack. If that matters,
`join()` it and `start()` again later — the instance is restartable by design.

*Where.* `pipeline_thread()`, `audio_pipeline_join()` in `audio_pipeline_core.c`; the
`quit_request` comment in `audio_pipeline.h`.

## 4. One lifecycle state, one transition table

`struct audio_pipeline` used to carry five booleans — initialised, chain open, thread
alive, worker pulling. They are now one `atomic_t` holding an `enum audio_pipeline_state`,
and every legal move lives in a single table:

```c
static const struct {
	enum pipeline_trigger trigger;
	enum audio_pipeline_state from;
	enum audio_pipeline_state to;
} pipeline_transitions[] = { … };
```

*What it buys.* The combinations that were expressible but never reachable — playing
without a thread, an open chain on an uninitialised instance — no longer exist. A
`(trigger, from)` pair that is not in the table is refused, and the guard clauses in the
entry points only translate that refusal into the errno each one documents. Moves are
applied with `atomic_cas()` in a loop, not a store, because the worker thread and the
control thread both write the state: when `stop()` and the worker's EOF land together, the
loser re-reads and discovers its move is no longer legal instead of undoing the winner's.

*What it costs.* Adding a lifecycle behaviour means adding rows, not `if`s — which is the
point, but it does mean the table is the API's real specification and has to be read to
predict an edge case.

*Where.* `pipeline_transitions[]`, `pipeline_transition()` in `audio_pipeline_core.c`;
`enum audio_pipeline_state` in `audio_internal.h`. Rendered as a diagram in
[Pipeline lifecycle](05-pipeline-lifecycle.md).

## 5. The format is a contract nodes accept or refuse

Bound once by the application, installed top-down, immutable while the chain is open, and
validated by each node in its own `open()`. No negotiation, no capability query, no
adaptation. This is [ADR 0001](../adr/0001-pipeline-format-is-a-contract-nodes-refuse.md),
and the ADR is worth reading in full because it names what was rejected:

* **Runtime push, source to sink** (Arduino Audio Tools). `setAudioInfo()` returns `void` —
  with no refusal channel, a node that cannot comply can only adapt or `assert()`, and
  `I2SStream` tears down and restarts a live I2S peripheral mid-stream. Refusal is only
  possible *because* the format cannot change under an open chain, which is exactly what
  the `-EBUSY` rebind guard enforces.
* **Format as an application-level event** (ESP-ADF). `audio_pipeline.c` there never reads
  or checks a format at all; every application becomes its own negotiator.
* **A global maximum channel count in the pipeline.** The ceiling would be a guess: the
  file writer caps at 2 because of WAV, the I2S sink at 2 because of the wire, the gain
  filter and null sink do not care.

*What it costs.* The pipeline cannot tell you a chain is unsatisfiable until it opens one —
a source may only learn its real format from data it has not read yet, as the file reader
does from a WAV header. So failure surfaces from `audio_pipeline_start()`, with everything
already opened closed again.

## 6. One canonical container

Left-justified Q31 `int32_t`, always. [ADR 0002](../adr/0002-internal-container-is-left-justified-q31-int32.md)
argues it, rejects right-justification with guard bits (depth-dependent shifts return in a
subtler form), rejects a 16-bit container (forces the wire depth into the container) and
leaves `float` as an explicit extension point rather than a closed door.

The honest consequence is recorded there too: nothing saturates on the way down, so a gain
above unity wraps. That is a real defect (#39), not a design tolerance.

## 7. Node dependencies belong to the node's Kconfig symbol

`CONFIG_AUDIO_PIPELINE` brings in the core only: thread, frame buffer, event queue, node
dispatch. Every node is its own symbol defaulting to `n`, and the heavy dependencies hang
off the node that needs them — `FILE_SYSTEM` is selected by the two file nodes,
`I2S` by the two I2S nodes.

*What it buys.* An image links the nodes the application names and nothing else. A target
with no storage — the first hardware targets of this module have none — never pays for a
filesystem dispatch layer. `tests/subsys/audio/no_file_nodes/` asserts this against the
*generated* `autoconf.h`, so the property is tested rather than asserted in prose.

*What it costs.* Every application must list its nodes in `prj.conf`. The cost of
forgetting is made cheap on purpose: a `*_NODE_DEFINE()` whose symbol is off expands to
`AUDIO_NODE_UNAVAILABLE()`, a placeholder node plus a failing `BUILD_ASSERT` that names the
macro *and* the Kconfig symbol that fixes it. One error at the line where the mistake was
made, rather than a mangled symbol at link time.

## 8. Shared seams, so two ends of a link cannot drift apart

Two modules exist purely to be shared:

* `audio_wav.[ch]` — the only place that knows the RIFF/WAVE byte layout, in both
  directions. The file reader parses with it, the file writer emits with it, and the sample
  application builds its test track with it. Whatever `audio_wav_write_header()` accepts,
  `audio_wav_read_header()` parses back.
* `audio_i2s_wire.[ch]` — the only place that knows how the container maps onto I2S words,
  in both directions, plus the single gate on which depths the link supports. Both I2S
  nodes call `audio_i2s_wire_format_get()` in `open()` to validate the bound format.

Both are allocation-free, driver-free and endianness-explicit, so their arithmetic is unit
tested on a host with no hardware (`tests/subsys/audio/wav/`,
`tests/subsys/audio/i2s_wire/`).

## 9. Built-in resources are owned, not shared

A zero-initialised `struct audio_pipeline` gets the subsystem's single built-in stack,
frame buffer and event slots. Ownership is tracked **per resource** under a spinlock, and a
second claimant is refused with `-EBUSY` — with a log line that tells you to use
`AUDIO_PIPELINE_DEFINE()` for the second pipeline. `join()` hands the resources back, so
`init → join → init` passes them on.

The subtle part is the event queue. Releasing the slots does *not* invalidate the releasing
instance's binding — the ring still holds its own events, which is why the ERROR event a
failing `close()` publishes stays readable after `join()`. Handing them to *another*
instance does invalidate it, because that instance calls `k_msgq_init()` on the same
storage. An epoch counter records how often the slots changed hands, and
`audio_pipeline_get_event()` returns `-EPERM` rather than consuming the new owner's events.

Two accepted (documented, not hidden) consequences: an instance that is initialised and
then abandoned holds its built-ins for the life of the process, and ownership is by pointer
identity, so a new instance in the storage of a discarded one inherits its claim. Both are
lockout/identity effects, never corruption — fixing them would mean dereferencing an owner
pointer that may name an object that is gone.

*Where.* `pipeline_claim_defaults()`, `pipeline_release_defaults()`,
`audio_pipeline_event_queue_is_current()` in `audio_pipeline_core.c`.

## 10. The node interface is three ops, and everything else rides on the object

```c
struct audio_node_ops {
	int (*open)(struct audio_node *node);
	int (*process)(struct audio_node *node, struct audio_buffer_view *buf, size_t *out_size);
	int (*close)(struct audio_node *node);
};
```

Anything the pipeline has to tell a node — the format, the upstream, the private state —
travels on `struct audio_node` rather than through the op signatures. Adding a fact the
pipeline must communicate is therefore a field, not a signature change that breaks every
node in and out of tree.

`audio_node_open()` and `audio_node_close()` treat a missing op as success (a node need not
implement them); `audio_node_process()` treats a missing `process` as `-ENOSYS`, because a
node that cannot produce a frame is not a node.

## 11. Counts are total interleaved samples, everywhere

`frame_samples`, `frame_capacity`, `out_size`, the tone generator's `duration_samples`, the
I2S macros' `_frame_samples` — all total across channels, never per channel. The reason is
that the channel count is bound at *run* time, so a per-channel figure could not be turned
into a buffer size at compile time without a second static channel symbol to disagree with
the bound format.

Where the two finally meet is one place: `audio_pipeline_set_format()` refuses a format
whose `channels` exceed the frame capacity, because such a frame could not carry a single
interleaved sample set. The Kconfig floor of 2 on `AUDIO_PIPELINE_FRAME_SAMPLES` covers the
compile-time half of the same rule.
