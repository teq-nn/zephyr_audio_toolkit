# v1 excludes runtime reconfiguration and multi-port nodes

Two capabilities are deliberately absent from v1, so that a reader finding them
missing knows it was a choice rather than an oversight:

- **No dynamic runtime reconfiguration.** Format and topology are fixed at build
  time — topology by the `*_NODE_DEFINE` macros' `_upstream` argument, sizing by
  Kconfig and the `AUDIO_PIPELINE_DEFINE` arguments. Reconfiguring a running
  pipeline would mean re-sizing static storage, which cannot be done without
  either dynamic allocation or worst-case over-allocation.
- **No multi-input or multi-output nodes.** A node has at most one upstream, so
  mixers and splitters are out. `audio_node_pull()` was written not to preclude
  them (see ADR-0002), but nothing in v1 implements the multi-link node model
  they need.

Both are extension points, not permanent prohibitions. Adding either is a new
decision that supersedes this one; neither should be added incidentally.

Sample-format questions — the width of the internal container and whether float
processing is ever supported — are **not settled here** and are tracked
separately.
