# Audio Pipeline

A pull-based audio pipeline for Zephyr: a chain of nodes behind one worker
thread, using static memory only. This file is the glossary — the vocabulary to
use in code, tests, commits and issues. Behaviour lives in the code; decisions
live in `docs/adr/`.

## Language

**Node**:
One processing step in a chain, with a role and a set of ops (`open`, `process`,
`close`).
_Avoid_: stage, block, element, unit

**Source**:
A node with no upstream, which produces samples.
_Avoid_: producer, input, generator (a generator is one *kind* of source)

**Filter**:
A node with exactly one upstream, which reads samples and transforms them.
_Avoid_: processor, transform, effect, DSP block

**Sink**:
A node that consumes the final samples and is the start point of the pull cycle.
_Avoid_: consumer, output, destination

**Pipeline**:
The object binding a node chain to a worker thread, a frame buffer and an event
queue. One sink, reached through its chain of upstreams.
_Avoid_: graph, chain (a *chain* is the nodes alone, without the thread and
storage), stream

**Upstream**:
The node a filter or sink reads from. Direction is always named from the reader's
side; there is no "downstream" — no node knows what reads it.
_Avoid_: parent, previous, source (a source is a role, not a position)

**Pull**:
A read of one frame from a node's upstream, always through `audio_node_pull()`.
_Avoid_: fetch, request, poll

**Frame**:
The unit of work for one pipeline iteration: up to `frame_samples` interleaved
samples in the shared buffer. Every node is invoked at most once per frame.
_Avoid_: block, chunk, buffer (the *buffer* is the storage; the *frame* is what
occupies it), period

**Container**:
The `int32_t` every sample occupies inside the pipeline, regardless of the
resolution of the data it carries.
_Avoid_: sample format, word, type

**Valid bits**:
The effective resolution of the data sitting in the container — 16 for a 16-bit
source, whatever the container's width. Distinct from the container.
_Avoid_: bit depth, precision, sample size

**Track**:
One finite run of samples from a source, from `play()` to end of stream. A
pipeline can play several tracks in sequence on the same worker thread.
_Avoid_: stream, file, song, clip

**End of stream**:
A source having no more samples, reported as a successful return with
`out_size == 0` and surfaced as `AUDIO_PIPELINE_EVENT_EOF`. It is not an error.
_Avoid_: EOL, end of life, end of track, EOS

**Wiring error**:
A filter or sink defined with no upstream — a build-time mistake, reported as
`-ENOTSUP`. Never confused with end of stream.
_Avoid_: misconfiguration, unconnected node
