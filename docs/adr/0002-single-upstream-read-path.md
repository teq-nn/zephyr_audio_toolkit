# `audio_node_pull()` is the only way to read upstream

Every node that read from its upstream once carried its own copy of the pull,
and the copies disagreed: with no upstream the gain filter returned `-ENOTSUP`
while the null sink and the file writer reported a clean end of stream, and only
the file nodes kept the reserved `-EPIPE` out of their result — so a third-party
source failing with `-EPIPE` reached the application looking like a finished
track. `audio_node_pull()` is now the single implementation, owning the wiring
policy (`-ENOTSUP` for a filter or sink with no upstream), the `-EPIPE` remap,
and zero-size end-of-stream forwarding.

**No node may invoke an upstream node's `process()` op directly.** Doing so
bypasses all three decisions and is how they drifted apart the first time. The
helper does not walk the chain, so nodes still control when and how often they
pull.
