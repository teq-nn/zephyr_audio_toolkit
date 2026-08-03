/*
 * I2S input source node.
 *
 * open() configures the receive direction of a Zephyr I2S device from the
 * pipeline's bound format and leaves it stopped; process() takes a received
 * block from the driver, widens it into the canonical container through the
 * shared wire seam and hands the samples to the chain; close() drops the stream
 * and returns every block the node still holds (manifest §2/§4/§6/§7,
 * spec §4.2/§5.3/§10.5).
 *
 * A LIVE INPUT NEVER ENDS
 * -----------------------
 * The pipeline reserves out_size == 0 - and the -EPIPE that carries it - for a
 * track that finished: the sink raises EOF and the worker parks (manifest §7).
 * An I2S input has no end. The codec clocks continuously, so a read that
 * produced nothing means the transport failed, not that the music stopped, and
 * every failure path below therefore returns an error and leaves *out_size at
 * zero. A node that answered a read timeout with an empty frame would report a
 * broken wire as a clean end of stream, which is the one failure this node must
 * not invent.
 *
 * WHY THE BLOCK IS COPIED, AND WHY IT IS ALWAYS GIVEN BACK
 * -------------------------------------------------------
 * The Zephyr I2S API is mem_slab based: i2s_read() hands over ownership of a
 * block the driver filled, and the caller has to free it back to the slab. The
 * pipeline's frame buffer is borrowed storage owned by the pipeline (spec §4.1),
 * so the two ownership models cannot be bridged by passing the driver's block up
 * the chain - the copy at this boundary is the seam between them.
 *
 * A block that is not returned is invisible until the slab runs dry, at which
 * point capture stops for a reason that looks nothing like a leak. There is
 * therefore exactly one place a block is released, i2s_in_block_release(), and
 * every path that can lose interest in a block - a drained block, a conversion
 * failure, close() - goes through it.
 *
 * WHY BLOCKING IS CORRECT HERE
 * ----------------------------
 * i2s_read() waits for the next block forever, and that wait is what paces the
 * capture chain against the codec's clock: the source produces exactly as fast
 * as the wire fills. Polling would replace a wait with a spin and a timeout with
 * dropped audio, and audio_pipeline_stop() is asynchronous precisely so it does
 * not deadlock behind a blocking node (manifest §3.2).
 *
 * All state lives in the per-instance ::audio_i2s_in_state allocated by
 * AUDIO_I2S_IN_NODE_DEFINE(), which allocates the receive blocks with it, so
 * several sources can run side by side.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/audio/audio_i2s_wire.h>
#include <zephyr/audio/audio_node.h>
#include <zephyr/audio/audio_nodes.h>

#include "../audio_internal.h"

LOG_MODULE_REGISTER(audio_i2s_in, LOG_LEVEL_INF);

/*
 * The Philips I2S frame carries two words by definition, and the drivers behind
 * this API say so too: they ignore i2s_config.channels for that standard and
 * always clock two words per frame. A mono pipeline would therefore be accepted
 * and then filled at half the rate it describes, so it is refused instead
 * (spec §5.2: which formats a node accepts is a property of that node).
 */
#define I2S_IN_CHANNELS 2U

/* Blocking is the pacing mechanism; see the file comment. */
#define I2S_IN_QUEUE_TIMEOUT SYS_FOREVER_MS

/*
 * Give the block the node is holding back to the slab.
 *
 * The one release path, and idempotent, so an error path can call it without
 * first working out whether the block was ever taken.
 */
static void i2s_in_block_release(struct audio_i2s_in_state *state)
{
	if (state->block) {
		k_mem_slab_free(state->slab, state->block);
		state->block = NULL;
	}

	state->block_valid = 0U;
	state->block_used = 0U;
}

/*
 * Stop the receive direction and return everything to the slab, leaving the
 * node in a well-defined closed state.
 *
 * DROP rather than STOP: STOP is legal from RUNNING only and finishes the block
 * in flight, and this node is a clock target - if the master has stopped, that
 * wait never ends. DROP is legal from every state but NOT_READY, so the same
 * call closes a running stream and one that has overrun, and it discards the
 * queue the driver still owns.
 *
 * The block this node took from the driver is *not* in that queue: i2s_read()
 * passed ownership, so DROP cannot free it and close() would leak it. That is
 * why the release below is unconditional.
 */
static int i2s_in_release(struct audio_i2s_in_state *state)
{
	int ret = 0;

	if (state->configured) {
		ret = i2s_trigger(state->dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
		if (ret < 0) {
			LOG_ERR("%s: stopping the receive direction failed (%d)", state->dev->name,
				ret);
		}

		/* The direction is left configured-but-stopped either way: a
		 * failing DROP must not strand the node half open, or close()
		 * could never recover and the next open() would inherit it.
		 */
		state->configured = false;
	}

	state->started = false;

	i2s_in_block_release(state);

	return audio_eof_safe_errno(ret);
}

/* Start the receive direction. Unlike the transmit side this needs nothing in
 * hand - the driver allocates the blocks it fills - so it is what turns a
 * configured direction into one that produces data.
 */
static int i2s_in_start(struct audio_i2s_in_state *state)
{
	int ret;

	if (state->started) {
		return 0;
	}

	ret = i2s_trigger(state->dev, I2S_DIR_RX, I2S_TRIGGER_START);
	if (ret < 0) {
		LOG_ERR("%s: starting the receive direction failed (%d)", state->dev->name, ret);
		return audio_eof_safe_errno(ret);
	}

	state->started = true;

	return 0;
}

/*
 * Bring a direction that has overrun back to one that delivers blocks again.
 *
 * An RX overrun is a *state*, not an event: the driver parks the direction in
 * I2S_STATE_ERROR, lets the reader drain what is still valid and then refuses
 * every further read, and I2S_TRIGGER_PREPARE is the only way out. PREPARE is in
 * turn legal from that state only, so its return value doubles as the test - 0
 * means "this was an overrun and the direction is ready again", and an error
 * means the read failed for some other reason and must be reported as it was.
 *
 * PREPARE leaves the direction stopped, so reception has to be started again;
 * without that a node wedges on its first overrun, which during bring-up - a
 * consumer that is briefly too slow - is the likeliest failure of all.
 */
static int i2s_in_recover(struct audio_i2s_in_state *state)
{
	int ret = i2s_trigger(state->dev, I2S_DIR_RX, I2S_TRIGGER_PREPARE);

	if (ret < 0) {
		return ret;
	}

	state->started = false;

	LOG_WRN("%s: receive overrun, prepared and restarting", state->dev->name);

	return i2s_in_start(state);
}

/*
 * Take the next block from the driver into @p state.
 *
 * A failed read is not final: it is first taken as "we may have overrun" and
 * answered with prepare-and-restart, and only reported once the retry fails too.
 * Whatever the outcome, this either leaves a block in @p state or none at all -
 * a read that failed handed nothing over, so there is nothing to release.
 */
static int i2s_in_fetch(struct audio_i2s_in_state *state)
{
	void *block = NULL;
	size_t bytes = 0;
	int ret;

	ret = i2s_in_start(state);
	if (ret < 0) {
		return ret;
	}

	ret = i2s_read(state->dev, &block, &bytes);
	if (ret < 0) {
		int err = i2s_in_recover(state);

		if (err == 0) {
			ret = i2s_read(state->dev, &block, &bytes);
		}

		if (ret < 0) {
			/* Never end of stream (manifest §7): a timeout, a
			 * driver error and an overrun the retry could not clear
			 * are all transport failures, and the pipeline has to
			 * see them as failures.
			 */
			LOG_ERR("%s: receiving a block failed (%d)", state->dev->name, ret);
			return audio_eof_safe_errno(ret);
		}
	}

	if (!block) {
		/* A driver reporting success without a block would otherwise
		 * turn into an empty frame, i.e. into a clean EOF.
		 */
		LOG_ERR("%s: the driver reported a block it did not hand over", state->dev->name);
		return -EIO;
	}

	/* The driver may report fewer bytes than the block holds, but never
	 * more; trusting a larger figure would read past the block.
	 */
	state->block = block;
	state->block_valid = MIN(bytes, state->block_bytes);
	state->block_used = 0U;

	return 0;
}

static int i2s_in_open(struct audio_node *node)
{
	const struct audio_stream_config *fmt;
	struct audio_i2s_in_state *state;
	struct audio_i2s_wire_format wire;
	struct i2s_config cfg = {0};
	size_t sample_set_bytes;
	int ret;

	if (!node) {
		return -EINVAL;
	}

	state = (struct audio_i2s_in_state *)node->state;
	if (!state || !state->dev || !state->slab || state->block_bytes == 0U) {
		return -EINVAL;
	}

	/* Reopening without a close() must not leave the previous stream
	 * running on the same device, nor its block outstanding.
	 */
	(void)i2s_in_release(state);

	if (!device_is_ready(state->dev)) {
		LOG_ERR("%s: device is not ready", state->dev->name);
		return -ENODEV;
	}

	/* The format comes from the pipeline and nowhere else (spec §5.2):
	 * sample rate and channel count are pipeline-wide and this node keeps no
	 * copy of them to disagree with. A format is always bound before start()
	 * runs, so the absence of one is a caller error rather than a case to
	 * paper over with a default.
	 */
	fmt = node->pipeline_format;
	if (!fmt) {
		LOG_ERR("%s: no pipeline format installed", state->dev->name);
		return -EINVAL;
	}

	if (fmt->channels != I2S_IN_CHANNELS) {
		LOG_ERR("%s: %u channels cannot be carried by an I2S frame of %u words",
			state->dev->name, fmt->channels, I2S_IN_CHANNELS);
		return -ENOTSUP;
	}

	/* Nodes validate, they do not adapt (spec §5.2): the wire seam is the
	 * single gate on the depths this link carries, so a bound format it
	 * refuses is refused here rather than received as something else.
	 */
	ret = audio_i2s_wire_format_get(fmt->valid_bits_per_sample, &wire);
	if (ret < 0) {
		LOG_ERR("%s: %u bit input is not supported (%d)", state->dev->name,
			fmt->valid_bits_per_sample, ret);
		return ret;
	}

	sample_set_bytes = (size_t)wire.word_bytes * fmt->channels;

	/* A block that cannot hold one interleaved sample set would carry no
	 * usable frame at all; process() relies on this holding.
	 */
	if (state->block_bytes < sample_set_bytes) {
		LOG_ERR("%s: a %zu byte block is too small for one %u channel sample set",
			state->dev->name, state->block_bytes, fmt->channels);
		return -EINVAL;
	}

	cfg.word_size = wire.word_bits;
	cfg.channels = fmt->channels;
	cfg.format = I2S_FMT_DATA_FORMAT_I2S;
	cfg.options = AUDIO_I2S_IN_RX_OPTIONS;
	cfg.frame_clk_freq = fmt->sample_rate_hz;
	cfg.mem_slab = state->slab;
	/*
	 * Whole interleaved sample sets only, which is what keeps the channels
	 * of one block from shifting into the next. The blocks are sized for the
	 * widest word the container can ever produce, so at a narrower depth the
	 * slab block is larger than the driver is asked to fill; the surplus
	 * stays unused rather than becoming half a sample set. With the depths
	 * v1 carries this rounds nothing off - it is the invariant that is
	 * stated here, not an adjustment.
	 */
	cfg.block_size = ROUND_DOWN(state->block_bytes, sample_set_bytes);
	cfg.timeout = I2S_IN_QUEUE_TIMEOUT;

	ret = i2s_configure(state->dev, I2S_DIR_RX, &cfg);
	if (ret < 0) {
		/* Nothing was started, and the direction stays as the driver
		 * left it: an open() that fails leaves no stream behind.
		 */
		LOG_ERR("%s: %u Hz, %u ch, %u bit is not configurable (%d)", state->dev->name,
			fmt->sample_rate_hz, fmt->channels, wire.word_bits, ret);
		return audio_eof_safe_errno(ret);
	}

	state->configured = true;
	state->started = false;

	LOG_INF("%s: %u Hz, %u ch, %u bit, %zu byte blocks", state->dev->name, fmt->sample_rate_hz,
		fmt->channels, wire.word_bits, (size_t)cfg.block_size);

	return 0;
}

static int i2s_in_process(struct audio_node *node, struct audio_buffer_view *buf, size_t *out_size)
{
	const struct audio_stream_config *fmt;
	struct audio_i2s_in_state *state;
	struct audio_i2s_wire_format wire;
	size_t available;
	size_t samples;
	int ret;

	if (!node || !buf || !buf->data || !out_size) {
		return -EINVAL;
	}

	state = (struct audio_i2s_in_state *)node->state;
	if (!state) {
		return -EINVAL;
	}

	*out_size = 0;

	if (!state->configured) {
		/* process() before open(), or after close(). Reporting EOF here
		 * would look like a capture that finished before it began.
		 */
		LOG_ERR("process() on a closed I2S source");
		return -EBADF;
	}

	/* The pipeline leaves the bound format installed for as long as the node
	 * is open, which is why the node needs no copy of it (manifest §4).
	 */
	fmt = node->pipeline_format;
	if (!fmt) {
		return -EINVAL;
	}

	ret = audio_i2s_wire_format_get(fmt->valid_bits_per_sample, &wire);
	if (ret < 0) {
		return ret;
	}

	/* A frame that cannot hold one interleaved sample set is a caller error,
	 * not end of stream: nothing would fit and reporting zero samples would
	 * end the capture instead.
	 */
	if (buf->capacity < fmt->channels) {
		LOG_ERR("%s: a frame of %zu samples is too small for %u channels",
			state->dev->name, buf->capacity, fmt->channels);
		return -EINVAL;
	}

	if (!state->block) {
		ret = i2s_in_fetch(state);
		if (ret < 0) {
			return ret;
		}
	}

	/* A block can carry more than one frame - it is sized for the widest
	 * word the container can produce - so it is drained across as many
	 * frames as it takes rather than truncated to one.
	 */
	available = ROUND_DOWN((state->block_valid - state->block_used) / wire.word_bytes,
			       fmt->channels);
	if (available == 0U) {
		/* open() asked the driver for whole sample sets and refused a
		 * block too small for one, so a block that carries none is the
		 * driver contradicting that. Reported rather than answered with
		 * an empty frame, and the block goes back either way.
		 */
		LOG_ERR("%s: a block of %zu bytes carries no whole %u channel sample set",
			state->dev->name, state->block_valid, fmt->channels);
		i2s_in_block_release(state);
		return -EIO;
	}

	/* An interleaved sample set must never straddle two frames, or every
	 * following frame would arrive with its channels swapped.
	 */
	samples = MIN(ROUND_DOWN(buf->capacity, fmt->channels), available);

	ret = audio_i2s_wire_to_container(fmt->valid_bits_per_sample,
					  (const uint8_t *)state->block + state->block_used,
					  state->block_valid - state->block_used, buf->data,
					  samples);
	if (ret < 0) {
		/* open() validated the depth against the bound format, so this
		 * is unreachable unless the format changed underneath an open
		 * node - and even then the block is not lost.
		 */
		LOG_ERR("%s: %zu wire words do not widen into container samples (%d)",
			state->dev->name, samples, ret);
		i2s_in_block_release(state);
		return ret;
	}

	state->block_used += samples * wire.word_bytes;

	/* Handed back as soon as what is left cannot fill another sample set,
	 * so a block is owned for exactly as long as it carries audio.
	 */
	if ((state->block_valid - state->block_used) < (size_t)wire.word_bytes * fmt->channels) {
		i2s_in_block_release(state);
	}

	/* Always more than zero: a source that reported no samples would be
	 * telling the pipeline the stream ended (manifest §7).
	 */
	*out_size = samples;

	return 0;
}

static int i2s_in_close(struct audio_node *node)
{
	struct audio_i2s_in_state *state;

	if (!node) {
		return -EINVAL;
	}

	state = (struct audio_i2s_in_state *)node->state;
	if (!state) {
		return -EINVAL;
	}

	return i2s_in_release(state);
}

const struct audio_node_ops i2s_in_node_ops = {
	.open = i2s_in_open,
	.process = i2s_in_process,
	.close = i2s_in_close,
};
