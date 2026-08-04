# Writing your own node

A node is three function pointers and a state struct. This article is the contract you have
to honour, a working template for each role, and the mistakes that produce the most
confusing failures.

## The contract

```c
struct audio_node_ops {
	int (*open)(struct audio_node *node);
	int (*process)(struct audio_node *node, struct audio_buffer_view *buf, size_t *out_size);
	int (*close)(struct audio_node *node);
};
```

* **`open()`** — acquire resources and **validate the bound format** in
  `node->pipeline_format`. Called by `audio_pipeline_start()`, on the control thread, with
  the format already installed. Return `< 0` to refuse; the pipeline then closes everything
  it had already opened and fails `start()`. `-ENOTSUP` is the code for "I cannot carry this
  format". Omitting the op is legal and counts as success.
* **`process()`** — produce (source), transform (filter) or consume (sink) one frame.
  Called on the worker thread. Omitting it is `-ENOSYS`.
* **`close()`** — release everything, and leave the node in a state a later `open()` can
  recover from **even if the release failed**. Called sink-first, walking upstream. Omitting
  it is legal.

Everything the node needs travels on `struct audio_node`: `state` (its private data),
`upstream` (the node feeding it) and `pipeline_format` (the bound format, valid from just
before `open()` until `close()`).

## The rules that are not negotiable

1. **`*out_size == 0` means end of stream, and nothing else.** Never report an empty frame
   for a buffer that was too small (`-EINVAL`), for a `process()` before `open()`
   (`-EBADF`), or for a transport that failed (a real errno). Every shipped node spells this
   out in a comment because it is the one mistake that silently truncates audio.
2. **Never return `-EPIPE`.** It is the pipeline's own EOF signal. If you call into
   something that might produce it, funnel the result through the same rule the subsystem
   uses internally: map `-EPIPE` to `-EIO`.
3. **Set `*out_size` before anything can fail**, and to `0` on every error path, so a caller
   that reads it after a failure never sees a stale count.
4. **Reach upstream only through `audio_node_pull(node, buf, out_size)`**, passing
   *yourself* as `node`. That function owns the wiring policy (`-ENOTSUP` for a missing
   upstream), the `-EPIPE` remap and the EOF forwarding. Calling
   `node->upstream->ops->process()` directly re-implements all three, badly.
5. **Never split an interleaved sample set across two frames.** Round down to whole sample
   sets (`ROUND_DOWN(buf->capacity, channels)`); a set that straddled a frame boundary would
   transpose the channels of every following frame.
6. **Do not cache the sample rate or the channel count in your state.** Read them from
   `node->pipeline_format` where you need them. A second copy is the one thing that can
   disagree with the pipeline.
7. **Validate, do not adapt.** v1 has no resampler and no channel mapper. If you cannot
   carry the bound format, refuse it in `open()` with `-ENOTSUP`.
8. **Allocate statically, in your `*_NODE_DEFINE()` macro.** The subsystem never calls
   `k_malloc()`, and an application should never have to pass you a buffer pointer.
9. **`open()` must be re-entrant against a missing `close()`.** Every shipped node calls its
   own release helper first, so a reopen cannot leak the previous handle, block or stream.
10. **Blocking inside `process()` is allowed** — that is how a hardware sink paces the
    chain. `audio_pipeline_stop()` is asynchronous precisely so it does not deadlock behind
    you. Do not block in an event callback.

## Template: a filter

```c
#include <errno.h>
#include <zephyr/sys/util.h>

#include <zephyr/audio/audio_node.h>

struct my_filter_state {
	int32_t coeff_q15;   /* configuration, owned by the macro   */
	int32_t history;     /* implementation, valid while open    */
};

static int my_filter_open(struct audio_node *node)
{
	struct my_filter_state *state = node->state;

	if (!state || !node->pipeline_format) {
		return -EINVAL;
	}

	/* Validate what this node actually cares about; refuse the rest. */
	if (node->pipeline_format->channels > 2U) {
		return -ENOTSUP;
	}

	state->history = 0;

	return 0;
}

static int my_filter_process(struct audio_node *node, struct audio_buffer_view *buf,
			     size_t *out_size)
{
	struct my_filter_state *state = node->state;
	size_t i;
	int ret;

	if (!node || !buf || !buf->data || !out_size || !state) {
		return -EINVAL;
	}

	/* The one way to reach upstream. Forwards EOF as (0, *out_size == 0). */
	ret = audio_node_pull(node, buf, out_size);
	if (ret < 0 || *out_size == 0U) {
		return ret;
	}

	for (i = 0; i < *out_size; i++) {
		int64_t sample = buf->data[i];

		sample = (sample * state->coeff_q15) >> 15;
		/* Clamp before storing back: the container is Q31 and has no
		 * headroom, so anything above unity has to be saturated here.
		 */
		buf->data[i] = (int32_t)CLAMP(sample, INT32_MIN, INT32_MAX);
	}

	return 0;
}

static int my_filter_close(struct audio_node *node)
{
	ARG_UNUSED(node);
	return 0;
}

const struct audio_node_ops my_filter_node_ops = {
	.open = my_filter_open,
	.process = my_filter_process,
	.close = my_filter_close,
};
```

And the definition macro that goes with it, in your header:

```c
#define MY_FILTER_NODE_DEFINE(_name, _upstream, _coeff_q15)                            \
	static struct my_filter_state _name##_state = {                                \
		.coeff_q15 = (_coeff_q15),                                             \
	};                                                                             \
	AUDIO_NODE_DEFINE(_name, AUDIO_NODE_ROLE_FILTER, &my_filter_node_ops,          \
			  (_upstream), &_name##_state)
```

`AUDIO_NODE_DEFINE()` is public, so an out-of-tree node needs nothing from the subsystem's
private headers.

## Template: a source

A source has no upstream, so it never pulls; it fills the frame and reports how much.

```c
static int my_source_process(struct audio_node *node, struct audio_buffer_view *buf,
			     size_t *out_size)
{
	struct my_source_state *state = node->state;
	size_t channels;
	size_t samples;

	if (!node || !buf || !buf->data || !out_size) {
		return -EINVAL;
	}

	*out_size = 0;                      /* before anything can fail */

	if (!state->is_open) {
		return -EBADF;              /* not "end of stream" */
	}

	channels = node->pipeline_format->channels;

	if (buf->capacity < channels) {
		return -EINVAL;             /* not "end of stream" either */
	}

	if (state->done) {
		return 0;                   /* THIS is end of stream */
	}

	samples = ROUND_DOWN(buf->capacity, channels);
	/* … fill buf->data[0 .. samples-1] … */

	*out_size = samples;

	return 0;
}
```

A **live** source (a microphone, an I2S input) has no end of stream at all: every failure
path returns an error, because an empty frame would report a broken wire as a finished
track.

## Template: a sink

A sink pulls, consumes and reports what it consumed. The pipeline only cares that
`*out_size` was non-zero:

```c
static int my_sink_process(struct audio_node *node, struct audio_buffer_view *buf,
			   size_t *out_size)
{
	size_t produced = 0;
	int ret;

	if (!node || !buf || !buf->data || !out_size) {
		return -EINVAL;
	}

	*out_size = 0;

	ret = audio_node_pull(node, buf, &produced);
	if (ret < 0) {
		return ret;
	}

	if (produced == 0U) {
		return 0;                   /* forward EOF; consume nothing */
	}

	if ((produced % node->pipeline_format->channels) != 0U) {
		return -EINVAL;             /* refuse a split sample set */
	}

	/* … consume buf->data[0 .. produced-1] … */

	*out_size = produced;

	return 0;
}
```

## Publishing state to the application

If your node produces a *result* rather than audio, publish it as a **value**, not as a log
line — an application or a test has to be able to act on it. The tone analyzer is the
worked example: it keeps a `struct k_spinlock` in its state, writes a whole result under it
at the end of each window, and offers a getter that copies it out under the same lock, so a
reader on another thread never sees half of one window and half of the next.

Anything else in your state stays confined to the pipeline thread and needs no lock.

## Testing it

Put a suite under `tests/subsys/audio/<your_node>/` with a `testcase.yaml` and a `prj.conf`,
and make it run on `native_sim` with no hardware. Two patterns from the existing suites are
worth copying:

* **Fake peers.** `tests/subsys/audio/pipeline/fake_nodes.h` provides a scripted source
  ("produce N frames, optionally failing at frame K") and a counting sink, so a new suite
  needs no node implementation of its own to exercise a filter or a sink.
* **Fake drivers.** `tests/subsys/audio/i2s_in_node/` declares its own I2S device
  (`fake_i2s.c`) in the suite's overlay and binding, which is how a read timeout, a driver
  failure and an RX overrun are produced on a host with no I2S peripheral.

Name cases `test_<role>_<behaviour>` (`test_sink_reports_eof`), and cover at least: a
refused format, a `process()` before `open()`, end of stream, a short frame, and a reopen
after a failed `close()`.

Run them with `./scripts/ci-test.sh`.
