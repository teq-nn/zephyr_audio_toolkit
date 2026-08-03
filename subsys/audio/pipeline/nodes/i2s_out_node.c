/*
 * I2S output sink node.
 *
 * open() configures the transmit direction of a Zephyr I2S device from the
 * pipeline's bound format and leaves it stopped; process() pulls a frame,
 * narrows it into transfer blocks through the shared wire seam and hands them
 * to the driver; close() drops the stream and returns every queued block
 * (manifest §2/§4/§6, spec §4.4/§5.3).
 *
 * WHY THE FRAME IS COPIED
 * -----------------------
 * The Zephyr I2S API is mem_slab based: the caller allocates a block, fills it
 * and i2s_write() takes ownership of it until the transfer completes. The
 * pipeline hands a node a *borrowed* frame buffer it does not own and reuses for
 * the next frame (spec §4.1), so the two ownership models cannot be bridged by
 * passing the pipeline's buffer down - the copy at this boundary is the seam
 * between them, not an oversight.
 *
 * WHY BLOCKING IS CORRECT HERE
 * ----------------------------
 * Both the block allocation and i2s_write() wait forever, and that wait is what
 * paces the whole pull chain against the codec's clock: the sink runs exactly as
 * fast as the wire drains. Manifest §3.2 permits a sink to block inside
 * process(), and audio_pipeline_stop() is asynchronous precisely so it does not
 * deadlock behind one. A timeout here would turn a slow consumer into dropped
 * audio, which is the one failure a sink must not invent.
 *
 * THE CLOCK ROLE IS THE DEFINITION SITE'S
 * ---------------------------------------
 * open() configures the direction as a clock target or as the clock controller
 * according to the macro that defined the node, and that is the whole of the
 * difference between the two. It is a definition-time choice rather than a
 * Kconfig symbol because it belongs to one wiring: a board whose codec cannot
 * generate MCLK, BICK or LRCK needs the host to, and a board whose codec does
 * needs the host not to. See ::AUDIO_I2S_OUT_TX_CLK_CONTROLLER_OPTIONS.
 *
 * All state lives in the per-instance ::audio_i2s_out_state allocated by
 * AUDIO_I2S_OUT_NODE_DEFINE() or AUDIO_I2S_OUT_CLK_CONTROLLER_NODE_DEFINE(),
 * which allocates the transfer blocks with it, so several sinks can run side by
 * side.
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

LOG_MODULE_REGISTER(audio_i2s_out, LOG_LEVEL_INF);

/*
 * The Philips I2S frame carries two words by definition, and the drivers behind
 * this API say so too: they ignore i2s_config.channels for that standard and
 * always clock two words per frame. A mono pipeline would therefore be accepted
 * and then transmitted at half the rate it describes, so it is refused instead
 * (spec §5.2: which formats a node accepts is a property of that node).
 */
#define I2S_OUT_CHANNELS 2U

/* Blocking is the pacing mechanism; see the file comment. */
#define I2S_OUT_QUEUE_TIMEOUT SYS_FOREVER_MS

/*
 * Stop the transmit direction and give every block the driver still holds back
 * to the slab, leaving the node in a well-defined closed state.
 *
 * DROP rather than DRAIN: draining waits for the queue to play out, and where
 * this node is a clock target - the default role - a master that has stopped
 * makes that wait endless. DROP is also the one trigger legal from every state
 * but NOT_READY, so the same call closes a running stream and one that has
 * underrun, in either clock role.
 */
static int i2s_out_release(struct audio_i2s_out_state *state)
{
	int ret = 0;

	if (state->configured) {
		ret = i2s_trigger(state->dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
		if (ret < 0) {
			LOG_ERR("%s: stopping the transmit direction failed (%d)", state->dev->name,
				ret);
		}

		/* The direction is left configured-but-stopped either way: a
		 * failing DROP must not strand the node half open, or close()
		 * could never recover and the next open() would inherit it.
		 */
		state->configured = false;
	}

	state->started = false;

	return audio_eof_safe_errno(ret);
}

/*
 * Bring a direction that has underrun back to one that accepts blocks again.
 *
 * A TX underrun is a *state*, not an event: the driver parks the direction in
 * I2S_STATE_ERROR and rejects every write from then on, and I2S_TRIGGER_PREPARE
 * is the only way out. PREPARE is in turn legal from that state only, so its
 * return value doubles as the test - 0 means "this was an underrun and the
 * direction is ready again", and an error means the write failed for some other
 * reason and must be reported as it was.
 *
 * Without this a node wedges permanently on its first underrun, which during
 * bring-up is the likeliest failure of all.
 */
static int i2s_out_recover(struct audio_i2s_out_state *state)
{
	int ret = i2s_trigger(state->dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);

	if (ret < 0) {
		return ret;
	}

	/* PREPARE emptied the queue and left the direction stopped, so the next
	 * block has to start the transmission again.
	 */
	state->started = false;

	LOG_WRN("%s: transmit underrun, prepared and restarting", state->dev->name);

	return 0;
}

/* Start the transmit direction once there is something queued for it: the API
 * requires a block in hand at START, and starting an empty queue is an
 * underrun by construction.
 */
static int i2s_out_start(struct audio_i2s_out_state *state)
{
	int ret;

	if (state->started) {
		return 0;
	}

	ret = i2s_trigger(state->dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret < 0) {
		LOG_ERR("%s: starting the transmit direction failed (%d)", state->dev->name, ret);
		return audio_eof_safe_errno(ret);
	}

	state->started = true;

	return 0;
}

/*
 * Hand @p block to the driver, which owns it from the moment the write
 * succeeds and frees it once the transfer completes.
 *
 * A failed write is not final: it is first taken as "we may have underrun" and
 * answered with prepare-and-restart, and only reported once the retry fails too.
 * The block stays the node's own across that retry - a write that failed queued
 * nothing - so recovery costs no block and drops no samples.
 */
static int i2s_out_submit(struct audio_i2s_out_state *state, void *block, size_t bytes)
{
	int ret = i2s_write(state->dev, block, bytes);

	if (ret < 0) {
		int err = i2s_out_recover(state);

		if (err == 0) {
			ret = i2s_write(state->dev, block, bytes);
		}

		if (ret < 0) {
			LOG_ERR("%s: writing %zu bytes failed (%d)", state->dev->name, bytes, ret);
			k_mem_slab_free(state->slab, block);
			return audio_eof_safe_errno(ret);
		}
	}

	return i2s_out_start(state);
}

/* Copy @p count container samples into a fresh transfer block and queue it. */
static int i2s_out_send(struct audio_i2s_out_state *state, uint8_t valid_bits_per_sample,
			const int32_t *samples, size_t count, size_t word_bytes)
{
	void *block;
	int ret;

	/* K_FOREVER, and safe from the first frame on: the slab always has a
	 * free block before the transmission is started, and once it is started
	 * the driver keeps returning them.
	 */
	ret = k_mem_slab_alloc(state->slab, &block, K_FOREVER);
	if (ret < 0) {
		LOG_ERR("%s: no transfer block available (%d)", state->dev->name, ret);
		return audio_eof_safe_errno(ret);
	}

	ret = audio_i2s_wire_from_container(valid_bits_per_sample, samples, count, block,
					    state->block_bytes);
	if (ret < 0) {
		/* open() validated the depth and the block size against the
		 * bound format, so this is unreachable unless the format changed
		 * underneath an open node.
		 */
		LOG_ERR("%s: %zu samples do not convert into a %zu byte block (%d)",
			state->dev->name, count, state->block_bytes, ret);
		k_mem_slab_free(state->slab, block);
		return ret;
	}

	return i2s_out_submit(state, block, count * word_bytes);
}

static int i2s_out_open(struct audio_node *node)
{
	const struct audio_stream_config *fmt;
	struct audio_i2s_out_state *state;
	struct audio_i2s_wire_format wire;
	struct i2s_config cfg = {0};
	int ret;

	if (!node) {
		return -EINVAL;
	}

	state = (struct audio_i2s_out_state *)node->state;
	if (!state || !state->dev || !state->slab || state->block_bytes == 0U) {
		return -EINVAL;
	}

	/* Reopening without a close() must not leave the previous stream
	 * running on the same device.
	 */
	(void)i2s_out_release(state);

	if (!device_is_ready(state->dev)) {
		LOG_ERR("%s: device is not ready", state->dev->name);
		return -ENODEV;
	}

	/* The output format comes from the pipeline and nowhere else
	 * (spec §5.2): sample rate and channel count are pipeline-wide and this
	 * node keeps no copy of them to disagree with. A format is always bound
	 * before start() runs, so the absence of one is a caller error rather
	 * than a case to paper over with a default.
	 */
	fmt = node->pipeline_format;
	if (!fmt) {
		LOG_ERR("%s: no pipeline format installed", state->dev->name);
		return -EINVAL;
	}

	if (fmt->channels != I2S_OUT_CHANNELS) {
		LOG_ERR("%s: %u channels cannot be carried by an I2S frame of %u words",
			state->dev->name, fmt->channels, I2S_OUT_CHANNELS);
		return -ENOTSUP;
	}

	/* Nodes validate, they do not adapt (spec §5.2): the wire seam is the
	 * single gate on the depths this link carries, so a bound format it
	 * refuses is refused here rather than transmitted as something else.
	 */
	ret = audio_i2s_wire_format_get(fmt->valid_bits_per_sample, &wire);
	if (ret < 0) {
		LOG_ERR("%s: %u bit output is not supported (%d)", state->dev->name,
			fmt->valid_bits_per_sample, ret);
		return ret;
	}

	/* A block that cannot hold one interleaved sample set would split every
	 * frame across the channel boundary; process() relies on this holding.
	 */
	if (state->block_bytes < (size_t)wire.word_bytes * fmt->channels) {
		LOG_ERR("%s: a %zu byte block is too small for one %u channel sample set",
			state->dev->name, state->block_bytes, fmt->channels);
		return -EINVAL;
	}

	cfg.word_size = wire.word_bits;
	cfg.channels = fmt->channels;
	cfg.format = I2S_FMT_DATA_FORMAT_I2S;
	/* The definition macro chose the role and this is the only place it is
	 * turned into option bits. Both constants are named rather than written
	 * out, because the controller one is zero and a bare 0 here would read
	 * as "no options" instead of as a decision.
	 */
	cfg.options = state->clk_controller ? AUDIO_I2S_OUT_TX_CLK_CONTROLLER_OPTIONS
					   : AUDIO_I2S_OUT_TX_OPTIONS;
	cfg.frame_clk_freq = fmt->sample_rate_hz;
	cfg.mem_slab = state->slab;
	cfg.block_size = state->block_bytes;
	cfg.timeout = I2S_OUT_QUEUE_TIMEOUT;

	ret = i2s_configure(state->dev, I2S_DIR_TX, &cfg);
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

	LOG_INF("%s: %u Hz, %u ch, %u bit, %zu byte blocks, clock %s", state->dev->name,
		fmt->sample_rate_hz, fmt->channels, wire.word_bits, state->block_bytes,
		state->clk_controller ? "controller" : "target");

	return 0;
}

static int i2s_out_process(struct audio_node *node, struct audio_buffer_view *buf, size_t *out_size)
{
	const struct audio_stream_config *fmt;
	struct audio_i2s_out_state *state;
	struct audio_i2s_wire_format wire;
	size_t produced = 0;
	size_t block_samples;
	size_t offset;
	int ret;

	if (!node || !buf || !buf->data || !out_size) {
		return -EINVAL;
	}

	state = (struct audio_i2s_out_state *)node->state;
	if (!state) {
		return -EINVAL;
	}

	*out_size = 0;

	if (!state->configured) {
		/* process() before open(), or after close(). Reporting EOF here
		 * would silently swallow the whole track.
		 */
		LOG_ERR("process() on a closed I2S sink");
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

	ret = audio_node_pull(node, buf, &produced);
	if (ret < 0) {
		return ret;
	}

	if (produced == 0U) {
		/* End of stream (manifest §7): nothing left to transmit. The
		 * blocks already queued play out on their own and close() drops
		 * whatever the wire has not consumed.
		 */
		return 0;
	}

	/* An interleaved sample set must never be split across two blocks, or
	 * the channels would stay swapped for the rest of the stream.
	 */
	if ((produced % fmt->channels) != 0U) {
		LOG_ERR("%s: %zu samples do not fill whole %u channel sample sets",
			state->dev->name, produced, fmt->channels);
		return -EINVAL;
	}

	/* Blocks are sized from the frame capacity, so one block normally
	 * carries the whole frame. A pipeline handing over a larger frame than
	 * the definition site promised is still transmitted correctly, in
	 * several blocks, rather than refused halfway through a stream.
	 */
	block_samples = ROUND_DOWN(state->block_bytes / wire.word_bytes, fmt->channels);
	if (block_samples == 0U) {
		/* open() refused a block this small, so getting here means the
		 * format changed underneath an open node. Reported rather than
		 * clamped: the loop below would otherwise never advance.
		 */
		LOG_ERR("%s: a %zu byte block holds no %u channel sample set", state->dev->name,
			state->block_bytes, fmt->channels);
		return -EINVAL;
	}

	for (offset = 0; offset < produced; offset += block_samples) {
		size_t chunk = MIN(block_samples, produced - offset);

		ret = i2s_out_send(state, fmt->valid_bits_per_sample, &buf->data[offset], chunk,
				   wire.word_bytes);
		if (ret < 0) {
			return ret;
		}
	}

	/* The sink consumed the frame; the pipeline only cares that it was not
	 * empty (spec §4.4).
	 */
	*out_size = produced;

	return 0;
}

static int i2s_out_close(struct audio_node *node)
{
	struct audio_i2s_out_state *state;

	if (!node) {
		return -EINVAL;
	}

	state = (struct audio_i2s_out_state *)node->state;
	if (!state) {
		return -EINVAL;
	}

	return i2s_out_release(state);
}

const struct audio_node_ops i2s_out_node_ops = {
	.open = i2s_out_open,
	.process = i2s_out_process,
	.close = i2s_out_close,
};
