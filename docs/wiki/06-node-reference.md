# Node reference

Eight nodes ship with the module. Each is its own Kconfig symbol, defaulting to `n`, and
each is reachable only through its `*_NODE_DEFINE()` macro.

| Node | Role | Kconfig symbol (`CONFIG_AUDIO_PIPELINE_NODE_…`) | Pulls in |
| --- | --- | --- | --- |
| [File reader](#file-reader-source) | source | `FILE_READER` | `FILE_SYSTEM` |
| [File writer](#file-writer-sink) | sink | `FILE_WRITER` | `FILE_SYSTEM` |
| [Gain filter](#gain-filter) | filter | `GAIN_FILTER` | — |
| [I2S input](#i2s-input-source) | source | `I2S_IN` | `I2S` |
| [I2S output](#i2s-output-sink) | sink | `I2S_OUT` | `I2S` |
| [Null sink](#null-sink) | sink | `NULL_SINK` | — |
| [Tone analyzer](#tone-analyzer-sink) | sink | `TONE_ANALYZER` | — |
| [Tone generator](#tone-generator-source) | source | `TONE_GEN` | — |

Rules that hold for **all** of them:

* Every macro is **file scope only** and allocates the node plus its private state. Two
  instances never share state.
* Everything below the "implementation" line in each state struct is meaningful only
  between a successful `open()` and its `close()`, and is **read-only to the application**.
* No node keeps its own sample rate or channel count. Both are pipeline-wide and read from
  `node->pipeline_format` wherever they are needed.
* `process()` before `open()` (or after `close()`) is `-EBADF`, never an empty frame.
* A frame too small for one interleaved sample set is `-EINVAL`, never end of stream.

---

## File reader (source)

```c
AUDIO_FILE_READER_NODE_DEFINE(name, path);
```

Reads a RIFF/WAVE file through the Zephyr filesystem API and widens its 16-bit payload into
the canonical container.

**`open()`** opens the file, reads a `AUDIO_WAV_HEADER_SCAN_SIZE` (256 byte) prefix, parses
it with the shared WAV codec — which walks the chunk list, so `JUNK`/`LIST` chunks are
skipped — and seeks to the payload. It then checks, in order:

| Check | Failure |
| --- | --- |
| valid PCM WAVE header | `-EINVAL` (structure) / `-ENOTSUP` (not PCM) |
| `bits_per_sample == 16` | `-ENOTSUP` — v1 converts 16-bit only |
| `1 <= channels <= 2` | `-ENOTSUP` |
| file's rate **and** channel count equal the bound format's | `-ENOTSUP` |

Note what is *not* checked: the pipeline's `valid_bits_per_sample`. The node's gate is the
file's own depth. A refused file leaves no handle and no format behind.

**`process()`** reads whole interleaved sample sets only, widens in place (`s32 = s16 << 16`,
back to front, so no scratch buffer is needed) and reports the sample count. End of stream
(`*out_size == 0`) when the declared payload is exhausted, when the file is shorter than the
header promised, or when what is left cannot fill one sample set.

**Observable state:** `state->fmt` is the file's real format after a successful `open()`;
`state->eof` says whether the source has run out.

**Application must:** mount a filesystem first. `select FILE_SYSTEM` only pulls in the
dispatch layer — you still choose ext2/FAT/littlefs and mount it.

---

## File writer (sink)

```c
AUDIO_FILE_WRITER_NODE_DEFINE(name, upstream, path);
```

Narrows the container back to 16-bit PCM and appends it to a RIFF/WAVE file.

**`open()`** requires an installed pipeline format (`-EINVAL` without one), copies it, and
refuses anything but `valid_bits_per_sample == 16` (`-ENOTSUP`) or more than 2 channels
(`-ENOTSUP`). It creates the file with `FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC` and writes a
44-byte header declaring an **empty** data chunk.

**Sizes are back-patched**, because they are not known until the stream ends. The patch
happens on end of stream *as well as* in `close()`, so the file is valid as soon as the
pipeline reports EOF. A run that dies without either leaves a structurally valid header
claiming no payload — a reader sees an empty track and stops, rather than replaying
whatever bytes follow.

**`process()`** pulls, then:

* `*out_size == 0` from upstream → finalise the header and propagate EOF;
* `produced % channels != 0` → `-EINVAL` (a split sample set would transpose the rest of the
  file);
* payload beyond `AUDIO_WAV_MAX_DATA_SIZE` → `-EFBIG` (both size fields are 32-bit);
* a short filesystem write → `-ENOSPC`. `data_bytes` counts only what the filesystem
  confirmed, so the header stays truthful even after a failed frame.

Conversion runs through a per-instance scratch buffer of
`AUDIO_FILE_WRITER_CHUNK_SAMPLES` (64) samples; a larger frame is simply written in several
chunks, so this does **not** cap the frame size.

**Narrowing rule:** arithmetic `>> 16` — truncation toward negative infinity, no rounding
bias, no clipping needed. It is the exact inverse of the reader's widening, so
file → pipeline → file is bit identical.

---

## Gain filter

```c
AUDIO_GAIN_FILTER_NODE_DEFINE(name, upstream, gain_q15);   /* AUDIO_GAIN_UNITY_Q15 == 32768 */
```

Pulls a frame and scales every sample by a Q15 gain: widen to `int64_t`, multiply, `>> 15`,
store back. A `gain_q15` of `0` is treated as **unity** by `open()`, so an unconfigured
filter passes audio through rather than muting it. No dependencies, no state beyond the
gain.

> ⚠️ The store back to `int32_t` has **no clamp**. A gain above unity on a near-full-scale
> sample wraps — loud positive becomes loud negative. Tracked in issue #39; see
> [ADR 0002](../adr/0002-internal-container-is-left-justified-q31-int32.md). Attenuation
> (`gain_q15 <= 32768`) is safe.

---

## Null sink

```c
AUDIO_NULL_SINK_NODE_DEFINE(name, upstream);
```

Pulls a frame and drops it. No state at all. Useful to terminate a chain while bringing a
source or filter up, and it accepts every format because it looks at none.

---

## Tone generator (source)

```c
AUDIO_TONE_GEN_NODE_DEFINE(name, amplitude_q15, duration_samples, freq_hz…);
```

Synthesises one sine per channel from an integer phase accumulator and a static quarter-wave
table. Integer arithmetic throughout — deliberately no `sinf()`, so no FPU dependency
follows the node onto a target.

| Parameter | Meaning |
| --- | --- |
| `amplitude_q15` | peak amplitude, `0` … `AUDIO_TONE_GEN_FULL_SCALE_Q15` (32768 = full scale) |
| `duration_samples` | **total** interleaved samples to produce; `0` runs indefinitely |
| `freq_hz…` | one frequency per channel, in channel order, at most `AUDIO_TONE_GEN_MAX_TONES` (2) |

**`open()`** refuses, with the pipeline's bound format in hand:

| Condition | Failure |
| --- | --- |
| no installed format | `-EINVAL` |
| amplitude outside `0…32768` | `-EINVAL` |
| number of frequencies ≠ channel count | `-ENOTSUP` |
| a frequency of 0, or at/above Nyquist (`2·f >= fs`) | `-EINVAL` — an aliased tone would look like a clean tone nobody asked for |
| `duration_samples % channels != 0` | `-EINVAL` |

Reopening starts the stream over: phase 0, nothing produced. Two identically configured
instances are indistinguishable from the first sample on.

**Accuracy.** The phase increment is `round(f · 2³² / fs)`, computed once, so the only
frequency error is that single rounding — about 5.6 µHz at 48 kHz — and it never becomes
drift. The accumulator carries across frames untouched, so a frame boundary is not a seam.

**Why one frequency *per channel*:** different tones left and right are what let an analyzer
at the far end tell a swapped pair of wires from a correct one.

---

## Tone analyzer (sink)

```c
AUDIO_TONE_ANALYZER_NODE_DEFINE(name, upstream, window_samples, expected_freq_hz…);

int audio_tone_analyzer_get_result(const struct audio_node *node,
				   struct audio_tone_analyzer_result *result);
```

The pass/fail oracle for a loopback. It measures how much of each channel's energy sits at
the frequency that channel was told to expect, and publishes a **verdict you read as a
value** — nothing depends on parsing a log line.

**Why measure instead of compare:** the link under test has a fixed but unmeasured latency
(encoder, framing, decoder, two DMA queues), so a sample-by-sample comparison fails even
when everything is correct. The magnitude of a frequency component does not depend on where
the window starts. That offset invariance is the load-bearing property of this node.

**How:** one Goertzel recurrence per (channel, expected tone) pair — every expected
frequency is measured on **every** channel, which is what lets the verdict say *swapped*
instead of *pass*. Each channel also accumulates total energy, so results are in-band
*fractions*: an absolute magnitude cannot tell a correct tone from a louder wrong one.

**`open()`** refuses:

| Condition | Failure |
| --- | --- |
| no installed format, or `sample_rate_hz == 0` | `-EINVAL` |
| window outside `AUDIO_TONE_ANALYZER_MIN_WINDOW`…`MAX_WINDOW` (64…4096, per channel) | `-EINVAL` |
| number of expected frequencies ≠ channel count | `-ENOTSUP` |
| a frequency inside the first bin (`f · N < fs`) or the last bin below Nyquist | `-EINVAL` |

Window choice matters: a window of N samples resolves `fs / N`. Pick one where the expected
frequencies land on whole bins — 960 samples at 48 kHz puts 1 kHz on bin 20 and 3 kHz on
bin 60 — and the measurement is *exactly* offset invariant instead of merely nearly so.

**Verdicts**, decided in this order (the order is the contract):

| Verdict | Meaning |
| --- | --- |
| `PASS` | every channel carries its own expected tone (in-band fraction ≥ ½) |
| `SILENT` | at least one channel carries no signal (RMS < 16 in the narrowed 16-bit domain, ≈ −66 dBFS) |
| `SWAPPED` | every channel carries its neighbour's expected tone |
| `NOISE` | energy arrived without a tone in it (one-sinusoid fit residual > 5 %) |
| `WRONG_FREQ` | a tone arrived, but not one expected on that channel |
| `NONE` | no window has completed since `open()` |

`PASS` is tested first so that an analyzer configured with the same frequency on both
channels — where a swap is not observable at all — reports what it can see rather than an
ambiguity. `SILENT` comes before `SWAPPED` because a dead channel is a different fault and
the one to fix first.

**Reading the result** is safe from any thread at any time, including while the pipeline
runs: a completed window is published under a spinlock and copied out under the same one, so
a reader never sees half of one window and half of the next. This is the only part of the
node not confined to the pipeline thread.

```c
struct audio_tone_analyzer_result r;

audio_tone_analyzer_get_result(&analyzer, &r);
if (r.windows > 0 && r.verdict == AUDIO_TONE_ANALYZER_VERDICT_PASS) { … }
```

Per channel you also get `in_band_q15[tone]` (Q15 fraction of the channel's energy at each
expected frequency — indexed by *tone*, not channel), `rms`, `residual_q15`, `strongest`,
`tonal` and `silent`.

**Lifecycle detail:** end of stream drops the partially filled window rather than measuring
it short (a short window reads low and would turn a clean EOF into a failed verdict).
`close()` leaves the last verdict in place — it is what the run was for; `open()` clears it,
so a rerun never reports the previous run's result.

---

## I2S input (source)

```c
AUDIO_I2S_IN_NODE_DEFINE(name, node_id, frame_samples, blocks);
```

Captures from a Zephyr I2S device. See [I2S and hardware bring-up](08-i2s-and-hardware.md)
for the sizing, clocking and DMA rules; the summary:

| Parameter | Meaning |
| --- | --- |
| `node_id` | devicetree node of the I2S device, e.g. `DT_ALIAS(i2s_rx)`. Must be `okay` — checked with `BUILD_ASSERT`, so a chain wired to a peripheral the board lacks fails to **build** |
| `frame_samples` | the same total-sample figure passed to `AUDIO_PIPELINE_DEFINE()`; sizes the receive blocks |
| `blocks` | receive blocks to allocate, `>= 2` (the I2S API's minimum); more buys tolerance against a late consumer at the cost of latency |

**`open()`** requires the device to be ready (`-ENODEV`), exactly **2 channels**
(`-ENOTSUP` — the Philips I2S frame carries two words by definition and the drivers ignore
`i2s_config.channels` for that format), and a depth the wire seam supports —
16 bit in v1 (`-ENOTSUP`). It configures RX as a **clock target (slave) on both clocks**;
there is deliberately no option to make it a controller.

**`process()`** takes a block from the driver, widens it through the shared wire codec and
hands samples up. A block may carry more than one frame, so it is drained across as many
frames as it takes and returned to the slab as soon as what is left cannot fill another
sample set. There is exactly one release path, so no path can leak a block.

> **A live source never reports end of stream.** The codec clocks continuously, so a read
> that produced nothing means the transport failed. A read timeout, a driver error and an
> unrecoverable overrun are all reported as errors with `*out_size == 0` — never as an empty
> frame, which the pipeline would read as a finished track.

**Overrun recovery** is automatic: a failed read is first taken as "we may have overrun" and
answered with `I2S_TRIGGER_PREPARE` plus a restart, and only reported once the retry fails
too.

---

## I2S output (sink)

```c
AUDIO_I2S_OUT_NODE_DEFINE(name, upstream, node_id, frame_samples, blocks);
```

Transmits through a Zephyr I2S device. Same devicetree, channel-count, depth and clock-role
rules as the input node — 2 channels, 16-bit wire, clock target on both clocks, `blocks >= 2`.

**`process()`** pulls a frame, copies it into transfer blocks through the shared wire codec
and hands them to the driver, which owns each block until its transfer completes. The copy
is unavoidable: the pipeline's frame buffer is borrowed storage it reuses, and `i2s_write()`
takes ownership.

Blocking is the pacing mechanism — both the slab allocation and `i2s_write()` wait forever,
so the sink (and with it the whole pull chain) runs exactly as fast as the wire drains. A
timeout here would turn a slow consumer into dropped audio.

**Underrun recovery** mirrors the input node: `I2S_TRIGGER_PREPARE`, then retry the write.
The block stays the node's own across that retry, so recovery costs no block and drops no
samples.

On end of stream the sink returns cleanly; queued blocks play out on their own and `close()`
drops whatever the wire has not consumed (`I2S_TRIGGER_DROP`, not `DRAIN` — draining would
wait forever if the clock master has stopped).
