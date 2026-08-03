# The pipeline format is a contract nodes accept or refuse

**Status:** accepted

A pipeline's stream format is bound once by `audio_pipeline_set_format()`, installed into every node immediately before `open()` (`audio_pipeline_core.c:461`), and is immutable while the chain is open — rebinding underneath it returns `-EBUSY` (`core.c:625`). Each node validates the bound format in its own `open()` and refuses with `-ENOTSUP` what it cannot carry; the pipeline validates only what the pipeline itself owns, and never a limit belonging to a node. There is no negotiation, no capability query, and no adaptation: a format the chain cannot carry fails `audio_pipeline_start()` before any sample moves.

This is the shape `i2s_configure()` already has one level down — a complete config in, `-EINVAL` out, legal only before streaming — which is what makes it the Zephyr-native answer rather than a house invention.

## Considered options

Both alternatives come from the frameworks this module was inspired by, and both were rejected. Evidence is in [`docs/research/format-negotiation-prior-art.md`](../research/format-negotiation-prior-art.md).

**Runtime push, source to sink (Arduino Audio Tools).** A source discovers its format and pushes it downstream at any time; `AudioInfoSupport::setAudioInfo()` returns `void`. That return type is the whole argument against it: with no refusal channel, a node that cannot comply can only adapt or `assert()`, and `I2SStream` does both — it tears down and restarts a live I2S peripheral mid-stream. Refusal is only possible because the format cannot change under an open chain, which is what the `-EBUSY` guard exists to enforce.

**Format as an application-level event (ESP-ADF).** `audio_element_report_info()` emits the format to the *application*, not to the downstream element; `audio_pipeline.c` never reads or checks a format at all. This moves the contradiction out of the library rather than resolving it, and makes every application its own negotiator.

**A global maximum channel count enforced by the pipeline.** Rejected because the ceiling would be a guess: `file_writer` caps at 2 because of WAV, `i2s_out` at 2 because of the wire, `gain_filter` and `null_sink` do not care. `CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES` already argues against a second static channel symbol "to disagree with the bound format."

## Consequences

- **The pipeline cannot report that a chain is unsatisfiable until it opens one.** A source may derive its real format from data it has not read yet — the file reader gets it from a WAV header. So validation is early, but not ahead of `open()`; failure surfaces from `audio_pipeline_start()`, with everything opened so far closed again.
- **A node's refusal is the only place its limits exist.** `audio_pipeline_set_format()` must not restate them, or it becomes a fourth opinion — which is exactly the defect #22 records.
- **The `-EBUSY` rebind guard is load-bearing, not defensive.** Relaxing it to permit a format change under an open chain removes the basis for `-ENOTSUP` and lands this module on Arduino Audio Tools' `assert()`.
- **A format mismatch is an error, not a request for conversion.** v1 ships no resampler and no channel mapper (`file_reader_node.c:161`). If those arrive, the mismatch becomes a missing node rather than a failure, and this ADR needs revisiting.
- Declared per-node capabilities, intersected across a chain before opening, remain compatible with this decision and are #18's work. They would make refusal earlier, not different.
