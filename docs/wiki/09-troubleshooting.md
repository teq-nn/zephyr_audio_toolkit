# Troubleshooting

Error codes with the fix next to each, build failures, and the log lines worth grepping for.

## Run-time error codes

### From `audio_pipeline_*`

| Code | From | Cause | Fix |
| --- | --- | --- | --- |
| `-EINVAL` | any | NULL argument, or the instance was never initialised (state zero) | call `audio_pipeline_init()` first; the instance must be zero-initialised before it |
| `-EINVAL` | `init()` | `frame_samples` is 0 or above `CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES` | raise the Kconfig symbol or lower `config.frame_samples` |
| `-EINVAL` | `set_format()` | `sample_rate_hz == 0`, `channels == 0`, or `channels > frame_capacity` | a frame must hold at least one interleaved sample set; raise `frame_samples` |
| `-EBUSY` | `init()` | this instance's worker thread is still running | `join()` before rebinding |
| `-EBUSY` | `init()`, `start()` | another instance holds a built-in stack / frame buffer / event slots | define the second pipeline with `AUDIO_PIPELINE_DEFINE()` |
| `-EBUSY` | `set_format()` | the node chain is open — playing *or* merely idle | `join()` first; nodes hold the format until they are closed |
| `-ENODATA` | `start()` | no format bound | call `audio_pipeline_set_format()` |
| `-ELOOP` | `start()` | the upstream chain is deeper than 16, or cyclic | check the `upstream` arguments of your node macros |
| `-ENOTSUP` | `start()` | a node refused the bound format | read the node's log line; see [Node reference](06-node-reference.md) |
| `-EPERM` | `play()` | no worker thread (`INIT`) or the chain is closed (`CLOSED`) | `start()` first; after a node error `start()` reopens the chain |
| `-EPIPE` | `process_frame()` | end of stream — **not** a failure | expected; the sink produced zero samples |
| `-ENOSYS` | `process_frame()` | the sink has no `process` op | your ops table is incomplete |

### From `audio_pipeline_get_event()`

| Code | Cause | Fix |
| --- | --- | --- |
| `-ENOMSG` | queue empty and `timeout` was `K_NO_WAIT` | expected; poll again or use a timeout |
| `-EAGAIN` | the waiting period expired | expected |
| `-EPERM` | the built-in event slots were claimed by another instance after this one was joined | drain the queue *before* `join()`, use the event callback, or give the instance its own slots with `AUDIO_PIPELINE_DEFINE()` |

### From nodes

| Code | Meaning |
| --- | --- |
| `-EBADF` | `process()` was called before `open()` or after `close()` |
| `-ENOTSUP` | the node cannot carry the bound format (rate, channels, depth), or a filter/sink has **no upstream** — a wiring error |
| `-EINVAL` | the frame cannot hold one interleaved sample set, or a produced sample count is not a whole number of sample sets |
| `-ENOSPC` | file writer: the filesystem accepted only part of a write (out of space) |
| `-EFBIG` | file writer: the data chunk would exceed the 32-bit WAV size field |
| `-ENODEV` | I2S: `device_is_ready()` was false |
| `-EIO` | a `-EPIPE` from below, remapped so it cannot be mistaken for end of stream; or an I2S driver that reported success without handing over a block |

## Build errors

### `…_NODE_DEFINE() needs the node it defines: set CONFIG_… =y`

You used a node macro whose Kconfig symbol is off. Node symbols all default to `n`. Add the
symbol the message names to `prj.conf`. The message comes from `AUDIO_NODE_UNAVAILABLE()`,
which exists precisely so the mistake is reported at the line where it was made rather than
as a mangled symbol at link time.

### `frame_samples is the TOTAL interleaved sample count and must hold at least one stereo sample set (>= 2)`

`AUDIO_PIPELINE_DEFINE()` or an I2S node macro was given fewer than 2 samples. The figure is
total across all channels, not per channel.

### `… is not an enabled devicetree node`

The `_node_id` passed to an I2S macro resolves to a node whose `status` is not `okay`. Enable
it in a board overlay. This is a build error on purpose: a chain wired to a peripheral the
board does not have should fail to build, not fail to start.

### `the I2S API needs at least two transfer/receive blocks per queue`

Pass `_blocks >= 2`.

### `AUDIO_TONE_GEN_NODE_DEFINE() takes one frequency per channel` / `… one expected frequency per channel`

Between 1 and 2 frequencies, and at run time their count must equal the bound channel count
(otherwise `open()` fails with `-ENOTSUP`).

### `the window is outside the range the accumulator bound is proved for`

The tone analyzer's window must be 64…4096 samples per channel. The bound on its Goertzel
accumulators is asserted at build time against that range.

## The build never sees the module

Symptoms: `CONFIG_AUDIO_PIPELINE` does not appear in menuconfig, or
`#include <zephyr/audio/audio_pipeline.h>` is not found.

* The module is discovered **only** via `zephyr/module.yml`. A root-level `module.yml` is
  silently ignored.
* Either register the repository in your west manifest, or pass
  `-DZEPHYR_EXTRA_MODULES=<abs path to this repo>` on the build command line.
* Do **not** do both. If the repository is already a west project, adding
  `ZEPHYR_EXTRA_MODULES` registers it twice — `scripts/ci-test.sh` shows the check:

  ```sh
  west list --format='{abspath}' | grep -qxF "$PWD"
  ```

## Behaviour that looks like a bug and is not

**`stop()` returned 0 but a frame still came out.** `stop()` is asynchronous by contract. A
sink is allowed to block inside `process()`, and `stop()` must not deadlock behind it, so the
frame in flight may complete.

**After EOF, `is_running()` is still true.** EOF parks the worker and keeps the chain open.
Only `join()` ends the thread. A second `play()` runs another track with no reopen.

**After a node error, `is_running()` is still true.** The worker closed the chain and parked
(state `CLOSED`). `start()` reopens the chain onto the same thread.

**A second `play()` after EOF produces nothing.** The pipeline is ready, but the *source* is
at end of data. The file reader needs a `close()` and `open()` — i.e. a `join()` and
`start()` — to rewind.

**The ERROR event arrived after everything had already stopped.** By design: an event is
queued last, once the pipeline has finished reacting to it, so an observer never sees a
pipeline that is halfway through publishing.

**Events went missing under load.** The queue drops the **newest** event when full, so the
oldest — and with it the first error, the one that explains the others — survives. Raise
`CONFIG_AUDIO_PIPELINE_EVENT_QUEUE_DEPTH` (1…32, default 4) or drain faster.

**Loud audio came out inverted.** A gain above unity wraps: the container is Q31 with no
headroom and the store back to `int32_t` has no clamp. Issue #39. Keep
`gain_q15 <= AUDIO_GAIN_UNITY_Q15`.

**A second pipeline refuses to start.** There is one built-in stack, frame buffer and event
queue. Use `AUDIO_PIPELINE_DEFINE()` for at least one of them.

**A pipeline that was never joined broke every later one.** An abandoned instance holds its
built-ins for the life of the process. `join()` before an instance goes out of scope.

## Log lines worth grepping for

The subsystem's log modules are `audio_pipeline_core`, `audio_node`, `audio_file_reader`,
`audio_file_writer`, `audio_tone_gen`, `audio_tone_analyzer`, `audio_i2s_in` and
`audio_i2s_out`, all at `LOG_LEVEL_INF`.

| Line | Means |
| --- | --- |
| `no pipeline format bound; call audio_pipeline_set_format() first` | `start()` returned `-ENODATA` |
| `the node chain is open; join() before rebinding the format` | `set_format()` returned `-EBUSY` |
| `a built-in pipeline resource is already owned by another instance…` | use `AUDIO_PIPELINE_DEFINE()` |
| `node open failed (%d)` | which `start()` failure, with the errno |
| `pull from a node that has no upstream` | a filter or sink was defined with `NULL` upstream |
| `remapping the reserved -EPIPE to -EIO` | something below returned the reserved code |
| `event queue full, dropped event %d` | raise the queue depth |
| `the event queue this pipeline points at belongs to another instance…` | `get_event()` returned `-EPERM` |
| `receive overrun, prepared and restarting` / `transmit underrun, prepared and restarting` | the I2S node recovered on its own; if it repeats, the peer is too slow |
| `%s: %u Hz, %u ch does not match the pipeline's %u Hz, %u ch` | the WAV file disagrees with the bound format |

## Getting a clean reproduction

```sh
./scripts/ci-test.sh                                        # everything, native_sim
CI_TEST_PATH=tests/subsys/audio/pipeline ./scripts/ci-test.sh   # one suite
CI_TEST_PLATFORM=native_sim/native/64 ./scripts/ci-test.sh      # 64-bit host
```

Twister writes `twister-out/`; `twister.log` and each case's `handler.log` are what CI
uploads on failure. The suites cover the paths most bug reports land in: lifecycle and state
transitions, event delivery and queue overflow, built-in resource ownership, frame sizing,
both file nodes, both tone nodes, the WAV codec, the I2S wire seam, and the I2S source
against a scriptable fake device.
