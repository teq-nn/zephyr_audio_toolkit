# The sink drives the pipeline; data is pulled, not pushed

A push model, where a source produces a frame and hands it downstream, needs
somewhere to put frames the next node is not ready for — a queue per link, which
this subsystem cannot have because it allocates nothing dynamically. Pulling
instead lets the sink ask for exactly as much as it can take, so the whole
pipeline runs on **one shared static frame buffer** and needs no inter-node
messaging at all. Nodes are passive: they act only when invoked, from the single
worker thread, which is why none of them needs internal thread safety.

The pull is deliberately *not* a chain walk. Each node decides when and how
often it reads its own upstream, so a node that consumes N frames to produce M —
a resampler — remains expressible without changing the model.

_Rationale reconstructed from the code and the design's consequences; the
original decision was not recorded at the time it was made._
