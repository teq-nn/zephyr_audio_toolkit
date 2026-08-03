/*
 * Behaviour suite for the I2S input source node.
 *
 * Everything a capture source can get wrong is a property of what the driver
 * does to it - a read that times out, a driver that fails, an overrun that
 * parks the direction - so the suite drives a scriptable I2S controller
 * (fake_i2s.c) instead of hardware and runs on native_sim with no I2S
 * peripheral at all.
 *
 * The claim it exists to protect is the one the pipeline cannot recover from:
 * *a live input never ends*. out_size == 0 and -EPIPE mean "the track finished"
 * (manifest §7), so a failing read that answered with an empty frame would park
 * the worker and report a broken wire as a clean end of stream. Several cases
 * below therefore assert not only that a failure is reported, but that it is
 * reported as a failure.
 *
 * The second claim is ownership: every block i2s_read() hands over has to go
 * back to the slab, on every path. The fake allocates with K_NO_WAIT, so a leak
 * shows up as the next read failing rather than as a hang, and the cases check
 * the slab is whole again afterwards.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <zephyr/audio/audio_format.h>
#include <zephyr/audio/audio_i2s_wire.h>
#include <zephyr/audio/audio_node.h>
#include <zephyr/audio/audio_nodes.h>
#include <zephyr/audio/audio_pipeline.h>

#include "fake_i2s.h"

#define FAKE_I2S_A DT_NODELABEL(fake_i2s_a)
#define FAKE_I2S_B DT_NODELABEL(fake_i2s_b)

/*
 * Deliberately not CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES, and deliberately
 * smaller: the block arithmetic must follow the capacity this pipeline was
 * defined with, and a block that holds more than one frame is what makes the
 * draining case below meaningful.
 */
#define FRAME_SAMPLES 32
#define RX_BLOCKS     2

#define SAMPLE_RATE_HZ 48000U
#define CHANNELS       2U
#define VALID_BITS     16U

/* A depth the container describes but the wire seam does not carry. */
#define UNSUPPORTED_BITS 24U

/* Bytes one block holds, and the words that fit into it at the bound depth. */
#define BLOCK_BYTES AUDIO_I2S_BLOCK_BYTES(FRAME_SAMPLES)
#define BLOCK_WORDS (BLOCK_BYTES / (VALID_BITS / 8U))

/* Frames it takes to drain one block, which has to be more than one for the
 * draining case to say anything.
 */
#define FRAMES_PER_BLOCK (BLOCK_WORDS / FRAME_SAMPLES)

/* ---------------------------------------------------------------------------
 * Build-time assertions
 * ---------------------------------------------------------------------------
 */

/*
 * The codec owns the clocks. Being exactly the two target bits is the stronger
 * claim - the controller constants are zero, so an option word that had lost a
 * bit would silently mean "controller" rather than fail.
 */
BUILD_ASSERT(AUDIO_I2S_IN_RX_OPTIONS == (I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET),
	     "the source must configure both clocks as targets and nothing else");

BUILD_ASSERT(BLOCK_BYTES >= (size_t)FRAME_SAMPLES * AUDIO_I2S_WIRE_MAX_WORD_BYTES,
	     "one block must hold one frame of the widest word the container can produce");
BUILD_ASSERT(BLOCK_BYTES != AUDIO_I2S_BLOCK_BYTES(CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES),
	     "the block size must not come from CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES");
BUILD_ASSERT(FRAMES_PER_BLOCK >= 2,
	     "the draining case needs a block that outlasts a single frame");

/* ---------------------------------------------------------------------------
 * The nodes under test
 * ---------------------------------------------------------------------------
 */

AUDIO_I2S_IN_NODE_DEFINE(i2s_in_a, FAKE_I2S_A, FRAME_SAMPLES, RX_BLOCKS);
AUDIO_I2S_IN_NODE_DEFINE(i2s_in_b, FAKE_I2S_B, FRAME_SAMPLES, RX_BLOCKS);

static const struct device *const dev_a = DEVICE_DT_GET(FAKE_I2S_A);
static const struct device *const dev_b = DEVICE_DT_GET(FAKE_I2S_B);

/* Formats live here rather than on the stack: the pipeline installs a *pointer*
 * on the node and leaves it there for as long as the node is open.
 */
static struct audio_stream_config format_a;
static struct audio_stream_config format_b;

static int32_t frame_storage[FRAME_SAMPLES];

/* Bind @p node to a format and open it, the way a pipeline would. */
static int open_source(struct audio_node *node, struct audio_stream_config *fmt,
		       uint32_t sample_rate_hz, uint8_t channels, uint8_t valid_bits_per_sample)
{
	fmt->sample_rate_hz = sample_rate_hz;
	fmt->channels = channels;
	fmt->valid_bits_per_sample = valid_bits_per_sample;
	fmt->format = AUDIO_SAMPLE_FORMAT_S32_LE;

	node->pipeline_format = fmt;

	return audio_node_open(node);
}

static int pull_frame(struct audio_node *node, size_t *out_size)
{
	struct audio_buffer_view view = {
		.data = frame_storage,
		.capacity = ARRAY_SIZE(frame_storage),
	};

	/* Deliberately not cleared here: reporting no samples is the pipeline's
	 * end-of-stream signal, so whether a failing process() leaves it at zero
	 * is the node's claim to make and this suite's to check.
	 */
	return audio_node_process(node, &view, out_size);
}

/* The container sample a wire word of @p word must widen to (spec §5.3). */
static int32_t widened(uint16_t word)
{
	return (int32_t)((uint32_t)word << 16);
}

/* ---------------------------------------------------------------------------
 * A sink that counts frames, for the cases that run a whole pipeline
 * ---------------------------------------------------------------------------
 */

struct capture_state {
	uint32_t frames;
	size_t last_size;
};

static struct capture_state capture_state_inst;

static int capture_open(struct audio_node *node)
{
	struct capture_state *state = node->state;

	state->frames = 0U;
	state->last_size = 0U;

	return 0;
}

static int capture_process(struct audio_node *node, struct audio_buffer_view *buf, size_t *out_size)
{
	struct capture_state *state = node->state;
	int ret = audio_node_pull(node, buf, out_size);

	if (ret < 0) {
		return ret;
	}

	state->frames++;
	state->last_size = *out_size;

	return 0;
}

static int capture_close(struct audio_node *node)
{
	ARG_UNUSED(node);

	return 0;
}

static const struct audio_node_ops capture_ops = {
	.open = capture_open,
	.process = capture_process,
	.close = capture_close,
};

AUDIO_NODE_DEFINE(capture_sink, AUDIO_NODE_ROLE_SINK, &capture_ops, &i2s_in_a,
		  &capture_state_inst);

AUDIO_PIPELINE_DEFINE(test_pipeline, FRAME_SAMPLES, 2048, 5);

static const struct audio_pipeline_config test_config = {
	.frame_samples = FRAME_SAMPLES,
};

/* ---------------------------------------------------------------------------
 * Fixture
 * ---------------------------------------------------------------------------
 */

static void i2s_in_before(void *fixture)
{
	ARG_UNUSED(fixture);

	i2s_in_a.pipeline_format = NULL;
	i2s_in_b.pipeline_format = NULL;

	fake_i2s_reset(dev_a);
	fake_i2s_reset(dev_b);

	memset(frame_storage, 0, sizeof(frame_storage));
	memset(&capture_state_inst, 0, sizeof(capture_state_inst));
}

static void i2s_in_after(void *fixture)
{
	ARG_UNUSED(fixture);

	/* Idempotent, so it also cleans up after a case that failed halfway.
	 * The pipeline goes first: joining closes the chain, and closing a node
	 * twice is a no-op.
	 */
	(void)audio_pipeline_join(&test_pipeline);
	(void)audio_node_close(&i2s_in_a);
	(void)audio_node_close(&i2s_in_b);

	zassert_equal(k_mem_slab_num_free_get(&i2s_in_a_slab), RX_BLOCKS,
		      "a closed source still holds %u of its %u blocks",
		      RX_BLOCKS - k_mem_slab_num_free_get(&i2s_in_a_slab), RX_BLOCKS);
	zassert_equal(k_mem_slab_num_free_get(&i2s_in_b_slab), RX_BLOCKS,
		      "a closed source still holds %u of its %u blocks",
		      RX_BLOCKS - k_mem_slab_num_free_get(&i2s_in_b_slab), RX_BLOCKS);
}

ZTEST_SUITE(audio_i2s_in_node, NULL, NULL, i2s_in_before, i2s_in_after, NULL);

/* ---------------------------------------------------------------------------
 * Definition and configuration
 * ---------------------------------------------------------------------------
 */

ZTEST(audio_i2s_in_node, test_i2s_in_takes_its_device_from_devicetree)
{
	zassert_equal(i2s_in_a_state.dev, dev_a,
		      "the node must use the devicetree node it was defined with");
	zassert_true(device_is_ready(i2s_in_a_state.dev), "%s not ready", i2s_in_a_state.dev->name);
	zassert_equal(i2s_in_a_state.block_bytes, BLOCK_BYTES,
		      "the block size must follow the frame capacity the macro was given");
}

ZTEST(audio_i2s_in_node, test_i2s_in_instances_share_no_storage)
{
	size_t produced_a = 0;
	size_t produced_b = 0;

	zassert_not_equal(&i2s_in_a_state, &i2s_in_b_state, "two nodes share one state");
	zassert_not_equal(i2s_in_a_state.slab, i2s_in_b_state.slab, "two nodes share one slab");
	zassert_not_equal(i2s_in_a_state.dev, i2s_in_b_state.dev, "two nodes share one device");

	zassert_ok(open_source(&i2s_in_a, &format_a, SAMPLE_RATE_HZ, CHANNELS, VALID_BITS));
	zassert_ok(open_source(&i2s_in_b, &format_b, SAMPLE_RATE_HZ, CHANNELS, VALID_BITS));

	/* Each device receives into the slab of the node that configured it,
	 * which is the whole point of allocating one per instance.
	 */
	zassert_equal(fake_i2s_data_get(dev_a)->cfg.mem_slab, &i2s_in_a_slab);
	zassert_equal(fake_i2s_data_get(dev_b)->cfg.mem_slab, &i2s_in_b_slab);

	zassert_ok(pull_frame(&i2s_in_a, &produced_a));
	zassert_ok(pull_frame(&i2s_in_b, &produced_b));
	zassert_true(produced_a > 0U);
	zassert_true(produced_b > 0U);

	/* Closing one source must not disturb the other's stream. */
	zassert_ok(audio_node_close(&i2s_in_a));
	zassert_ok(pull_frame(&i2s_in_b, &produced_b));
	zassert_true(produced_b > 0U, "closing one instance stopped another");

	zassert_ok(audio_node_close(&i2s_in_b));
}

ZTEST(audio_i2s_in_node, test_i2s_in_configures_the_device_as_a_clock_target)
{
	const struct i2s_config *cfg;

	zassert_ok(open_source(&i2s_in_a, &format_a, SAMPLE_RATE_HZ, CHANNELS, VALID_BITS));

	cfg = i2s_config_get(dev_a, I2S_DIR_RX);
	zassert_not_null(cfg, "the receive direction was not configured by open()");

	zassert_equal(cfg->options & (I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET),
		      I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET,
		      "the source must be a target on both clocks, options are 0x%02x",
		      cfg->options);

	/* Everything below came from the bound format, which is the point: the
	 * node stores neither the rate nor the channel count.
	 */
	zassert_equal(cfg->frame_clk_freq, SAMPLE_RATE_HZ);
	zassert_equal(cfg->channels, CHANNELS);
	zassert_equal(cfg->word_size, VALID_BITS);
	zassert_equal(cfg->format, I2S_FMT_DATA_FORMAT_I2S);
	zassert_equal(cfg->mem_slab, &i2s_in_a_slab, "the node must receive into its own slab");
	zassert_equal(cfg->block_size, BLOCK_BYTES);
	zassert_equal(cfg->timeout, SYS_FOREVER_MS,
		      "the blocking read is the pacing mechanism, not a bug");

	/* Configured, and deliberately not started: open() leaves no stream
	 * running behind it.
	 */
	zassert_equal(fake_i2s_data_get(dev_a)->state, I2S_STATE_READY);
	zassert_equal(fake_i2s_data_get(dev_a)->starts, 0U, "open() must not start the device");
}

ZTEST(audio_i2s_in_node, test_i2s_in_follows_the_bound_format_across_opens)
{
	/* The rate and the channel count live in the pipeline's format, so a
	 * second run at a different rate has to reach the driver as that rate -
	 * a node caching either would configure the first one again.
	 */
	zassert_ok(open_source(&i2s_in_a, &format_a, SAMPLE_RATE_HZ, CHANNELS, VALID_BITS));
	zassert_equal(i2s_config_get(dev_a, I2S_DIR_RX)->frame_clk_freq, SAMPLE_RATE_HZ);
	zassert_ok(audio_node_close(&i2s_in_a));

	zassert_ok(open_source(&i2s_in_a, &format_a, SAMPLE_RATE_HZ / 2U, CHANNELS, VALID_BITS));
	zassert_equal(i2s_config_get(dev_a, I2S_DIR_RX)->frame_clk_freq, SAMPLE_RATE_HZ / 2U,
		      "the node must configure the rate the pipeline bound, not a stored one");

	zassert_ok(audio_node_close(&i2s_in_a));
}

/* ---------------------------------------------------------------------------
 * open() refuses cleanly
 * ---------------------------------------------------------------------------
 */

ZTEST(audio_i2s_in_node, test_i2s_in_open_refuses_an_unsupported_depth)
{
	zassert_equal(open_source(&i2s_in_a, &format_a, SAMPLE_RATE_HZ, CHANNELS, UNSUPPORTED_BITS),
		      -ENOTSUP, "a %u bit format must be refused by open()", UNSUPPORTED_BITS);

	/* Nodes validate, they do not adapt: nothing was configured and nothing
	 * was started, so the next open() finds a clean device.
	 */
	zassert_equal(fake_i2s_data_get(dev_a)->state, I2S_STATE_NOT_READY);
	zassert_equal(fake_i2s_data_get(dev_a)->configures, 0U);
	zassert_equal(fake_i2s_data_get(dev_a)->starts, 0U);

	zassert_ok(open_source(&i2s_in_a, &format_a, SAMPLE_RATE_HZ, CHANNELS, VALID_BITS));
	zassert_ok(audio_node_close(&i2s_in_a));
}

ZTEST(audio_i2s_in_node, test_i2s_in_open_refuses_a_channel_count_the_frame_cannot_carry)
{
	zassert_equal(open_source(&i2s_in_a, &format_a, SAMPLE_RATE_HZ, 1U, VALID_BITS), -ENOTSUP,
		      "an I2S frame carries two words, so mono must be refused");
	zassert_equal(fake_i2s_data_get(dev_a)->configures, 0U);
}

ZTEST(audio_i2s_in_node, test_i2s_in_open_fails_when_the_device_cannot_be_configured)
{
	size_t produced = 0;

	fake_i2s_data_get(dev_a)->configure_ret = -EINVAL;

	zassert_equal(open_source(&i2s_in_a, &format_a, SAMPLE_RATE_HZ, CHANNELS, VALID_BITS),
		      -EINVAL, "a device that refuses the format must fail the open()");

	/* Nothing started, nothing left half open. */
	zassert_equal(fake_i2s_data_get(dev_a)->state, I2S_STATE_NOT_READY);
	zassert_equal(fake_i2s_data_get(dev_a)->starts, 0U);

	/* And a process() on it is a closed source, not an ended one. */
	zassert_equal(pull_frame(&i2s_in_a, &produced), -EBADF);
	zassert_equal(produced, 0U);

	/* Once the device accepts the format again, the same node opens. */
	fake_i2s_data_get(dev_a)->configure_ret = 0;
	zassert_ok(open_source(&i2s_in_a, &format_a, SAMPLE_RATE_HZ, CHANNELS, VALID_BITS));
	zassert_ok(audio_node_close(&i2s_in_a));
}

ZTEST(audio_i2s_in_node, test_i2s_in_open_refuses_a_device_that_is_not_ready)
{
	/* A device whose init failed is the case a board hits when the
	 * peripheral is missing or misconfigured, and the only way to produce
	 * it on a host is to say so through the device's own readiness flag -
	 * which is exactly what device_is_ready() reads. Restored below, so the
	 * rest of the suite still has a working controller.
	 */
	struct device_state *dev_state = dev_a->state;
	bool initialized = dev_state->initialized;

	dev_state->initialized = false;
	zassert_false(device_is_ready(dev_a), "the device should be reporting itself unusable");

	zassert_equal(open_source(&i2s_in_a, &format_a, SAMPLE_RATE_HZ, CHANNELS, VALID_BITS),
		      -ENODEV, "a device that is not ready must fail the open()");
	zassert_equal(fake_i2s_data_get(dev_a)->configures, 0U,
		      "an unusable device must not be configured");

	dev_state->initialized = initialized;
	zassert_true(device_is_ready(dev_a));
}

ZTEST(audio_i2s_in_node, test_i2s_in_open_needs_a_bound_format)
{
	i2s_in_a.pipeline_format = NULL;

	zassert_equal(audio_node_open(&i2s_in_a), -EINVAL,
		      "a source must not invent a format the pipeline did not bind");
	zassert_equal(fake_i2s_data_get(dev_a)->configures, 0U);
}

ZTEST(audio_i2s_in_node, test_i2s_in_process_before_open_is_not_end_of_stream)
{
	size_t produced = SIZE_MAX;

	zassert_equal(pull_frame(&i2s_in_a, &produced), -EBADF,
		      "process() on a closed source must fail, not report EOF");
	zassert_equal(produced, 0U, "a failing process() must report no samples");
}

/* ---------------------------------------------------------------------------
 * Reading, widening and draining
 * ---------------------------------------------------------------------------
 */

ZTEST(audio_i2s_in_node, test_i2s_in_widens_wire_words_through_the_shared_seam)
{
	size_t produced = 0;
	size_t i;

	zassert_ok(open_source(&i2s_in_a, &format_a, SAMPLE_RATE_HZ, CHANNELS, VALID_BITS));

	zassert_ok(pull_frame(&i2s_in_a, &produced));
	zassert_equal(produced, FRAME_SAMPLES, "a whole frame fits in one block");

	/* The fake counts its words up from zero, so the frame is the seam's
	 * widening of 0, 1, 2, ... - the same arithmetic the sink narrows with
	 * (spec §5.3), which is what keeps a loopback bit identical.
	 */
	for (i = 0; i < produced; i++) {
		zassert_equal(frame_storage[i], widened((uint16_t)i),
			      "sample %zu is 0x%08x, not the widened wire word 0x%04x", i,
			      (unsigned int)frame_storage[i], (unsigned int)i);
	}

	/* The device had to be started before it could deliver anything. */
	zassert_equal(fake_i2s_data_get(dev_a)->starts, 1U);
	zassert_equal(fake_i2s_data_get(dev_a)->state, I2S_STATE_RUNNING);

	zassert_ok(audio_node_close(&i2s_in_a));
}

ZTEST(audio_i2s_in_node, test_i2s_in_drains_one_block_across_several_frames)
{
	size_t produced = 0;
	uint32_t frame;

	zassert_ok(open_source(&i2s_in_a, &format_a, SAMPLE_RATE_HZ, CHANNELS, VALID_BITS));

	/* A block holds more than one frame, and the surplus is delivered
	 * rather than dropped: FRAMES_PER_BLOCK frames come out of one read.
	 */
	for (frame = 0U; frame < FRAMES_PER_BLOCK; frame++) {
		zassert_ok(pull_frame(&i2s_in_a, &produced));
		zassert_equal(produced, FRAME_SAMPLES, "frame %u came up short", frame);
		zassert_equal(fake_i2s_data_get(dev_a)->reads, 1U,
			      "frame %u went back to the driver instead of draining the block",
			      frame);
	}

	/* The drained block went back before the next one was asked for. */
	zassert_equal(k_mem_slab_num_free_get(&i2s_in_a_slab), RX_BLOCKS,
		      "the drained block was not returned to the slab");

	zassert_ok(pull_frame(&i2s_in_a, &produced));
	zassert_equal(fake_i2s_data_get(dev_a)->reads, 2U, "a drained block must be refilled");
	/* Word numbering continues, so nothing was lost between the blocks. */
	zassert_equal(frame_storage[0], widened((uint16_t)(FRAMES_PER_BLOCK * FRAME_SAMPLES)),
		      "samples went missing at the block boundary");

	zassert_ok(audio_node_close(&i2s_in_a));
}

/* ---------------------------------------------------------------------------
 * Failure is never end of stream
 * ---------------------------------------------------------------------------
 */

ZTEST(audio_i2s_in_node, test_i2s_in_read_timeout_is_an_error_not_end_of_stream)
{
	size_t produced = SIZE_MAX;
	int ret;

	zassert_ok(open_source(&i2s_in_a, &format_a, SAMPLE_RATE_HZ, CHANNELS, VALID_BITS));

	/* A blocking read that timed out. The wire is live, so this is a
	 * transport failure; answering it with an empty frame would tell the
	 * pipeline the track finished (manifest §7).
	 */
	fake_i2s_data_get(dev_a)->read_ret = -EAGAIN;

	ret = pull_frame(&i2s_in_a, &produced);
	zassert_true(ret < 0, "a read timeout was reported as success");
	zassert_not_equal(ret, -EPIPE, "-EPIPE is reserved for end of stream");
	zassert_equal(ret, -EAGAIN, "the driver's error must reach the pipeline unchanged");
	zassert_equal(produced, 0U, "a failing process() must report no samples");

	zassert_ok(audio_node_close(&i2s_in_a));
}

ZTEST(audio_i2s_in_node, test_i2s_in_driver_failure_is_an_error_not_end_of_stream)
{
	size_t produced = SIZE_MAX;
	int ret;

	zassert_ok(open_source(&i2s_in_a, &format_a, SAMPLE_RATE_HZ, CHANNELS, VALID_BITS));

	/* An -EIO that is not an overrun: the prepare-and-restart attempt is
	 * refused, and the original failure is what the pipeline sees.
	 */
	fake_i2s_data_get(dev_a)->read_ret = -EIO;

	ret = pull_frame(&i2s_in_a, &produced);
	zassert_equal(ret, -EIO, "a driver failure must be reported as one");
	zassert_equal(produced, 0U);
	zassert_equal(fake_i2s_data_get(dev_a)->prepares, 1U,
		      "a failing read must be tried as an overrun once");

	zassert_ok(audio_node_close(&i2s_in_a));
}

ZTEST(audio_i2s_in_node, test_i2s_in_failing_reads_return_every_block)
{
	const uint32_t attempts = 16U;
	size_t produced = 0;
	uint32_t i;

	zassert_ok(open_source(&i2s_in_a, &format_a, SAMPLE_RATE_HZ, CHANNELS, VALID_BITS));

	/* Fill and drain one block first, so the failures below run against a
	 * slab that has already handed a block out and taken it back.
	 */
	for (i = 0U; i < FRAMES_PER_BLOCK; i++) {
		zassert_ok(pull_frame(&i2s_in_a, &produced));
	}

	fake_i2s_data_get(dev_a)->read_ret = -EAGAIN;

	for (i = 0U; i < attempts; i++) {
		zassert_true(pull_frame(&i2s_in_a, &produced) < 0, "attempt %u did not fail", i);
		zassert_equal(produced, 0U);
	}

	/* The invisible failure this guards against: a block kept on an error
	 * path is only noticed when the slab runs dry, long after the leak.
	 */
	zassert_equal(k_mem_slab_num_free_get(&i2s_in_a_slab), RX_BLOCKS,
		      "%u failing reads left %u of %u blocks outstanding", attempts,
		      RX_BLOCKS - k_mem_slab_num_free_get(&i2s_in_a_slab), RX_BLOCKS);

	/* And the source still works once the device delivers again. */
	fake_i2s_data_get(dev_a)->read_ret = 0;
	zassert_ok(pull_frame(&i2s_in_a, &produced));
	zassert_true(produced > 0U);

	zassert_ok(audio_node_close(&i2s_in_a));
}

ZTEST(audio_i2s_in_node, test_i2s_in_returns_a_block_it_cannot_use)
{
	size_t produced = SIZE_MAX;

	zassert_ok(open_source(&i2s_in_a, &format_a, SAMPLE_RATE_HZ, CHANNELS, VALID_BITS));

	/* Half an interleaved sample set: the block was handed over, so it has
	 * to be handed back even though nothing in it can be used.
	 */
	fake_i2s_data_get(dev_a)->read_bytes = VALID_BITS / 8U;

	zassert_equal(pull_frame(&i2s_in_a, &produced), -EIO,
		      "a block with no whole sample set must be an error, not EOF");
	zassert_equal(produced, 0U);
	zassert_equal(k_mem_slab_num_free_get(&i2s_in_a_slab), RX_BLOCKS,
		      "the unusable block was not returned to the slab");

	zassert_ok(audio_node_close(&i2s_in_a));
}

ZTEST(audio_i2s_in_node, test_i2s_in_recovers_from_an_overrun)
{
	struct fake_i2s_data *data = fake_i2s_data_get(dev_a);
	size_t produced = 0;

	zassert_ok(open_source(&i2s_in_a, &format_a, SAMPLE_RATE_HZ, CHANNELS, VALID_BITS));

	zassert_ok(pull_frame(&i2s_in_a, &produced));
	zassert_true(produced > 0U);

	/* An overrun parks the direction in I2S_STATE_ERROR, where it stays
	 * until it is prepared - a node that ignored that wedges for good.
	 */
	data->read_ret = -EIO;
	data->read_overruns = true;

	/* Drain the block still in hand so the next frame goes to the driver. */
	while (produced > 0U && k_mem_slab_num_free_get(&i2s_in_a_slab) < RX_BLOCKS) {
		zassert_ok(pull_frame(&i2s_in_a, &produced));
	}

	zassert_ok(pull_frame(&i2s_in_a, &produced),
		   "an overrun must be recovered, not reported as a dead stream");
	zassert_equal(produced, FRAME_SAMPLES);
	zassert_equal(data->prepares, 1U, "the direction was not prepared after the overrun");
	zassert_true(data->starts >= 2U, "a prepared direction has to be started again");
	zassert_equal(data->state, I2S_STATE_RUNNING);

	/* And it keeps running afterwards. */
	zassert_ok(pull_frame(&i2s_in_a, &produced));
	zassert_true(produced > 0U, "the node stayed wedged after recovering");

	zassert_ok(audio_node_close(&i2s_in_a));
}

/* ---------------------------------------------------------------------------
 * close() and reopen
 * ---------------------------------------------------------------------------
 */

ZTEST(audio_i2s_in_node, test_i2s_in_close_returns_an_outstanding_block)
{
	size_t produced = 0;

	zassert_ok(open_source(&i2s_in_a, &format_a, SAMPLE_RATE_HZ, CHANNELS, VALID_BITS));

	/* One frame out of a block that holds several, so the node is still
	 * holding it when close() runs. i2s_read() passed ownership, so the
	 * driver's DROP cannot return this one - the node has to.
	 */
	zassert_ok(pull_frame(&i2s_in_a, &produced));
	zassert_equal(k_mem_slab_num_free_get(&i2s_in_a_slab), RX_BLOCKS - 1,
		      "the node should still be draining a block");

	zassert_ok(audio_node_close(&i2s_in_a));
	zassert_equal(k_mem_slab_num_free_get(&i2s_in_a_slab), RX_BLOCKS,
		      "close() left a block outstanding");
	zassert_equal(fake_i2s_data_get(dev_a)->drops, 1U, "close() must stop the device");
	zassert_equal(fake_i2s_data_get(dev_a)->state, I2S_STATE_READY);
}

ZTEST(audio_i2s_in_node, test_i2s_in_reopens_after_close)
{
	size_t produced = 0;

	zassert_ok(open_source(&i2s_in_a, &format_a, SAMPLE_RATE_HZ, CHANNELS, VALID_BITS));
	zassert_ok(pull_frame(&i2s_in_a, &produced));
	zassert_ok(audio_node_close(&i2s_in_a));

	zassert_ok(open_source(&i2s_in_a, &format_a, SAMPLE_RATE_HZ, CHANNELS, VALID_BITS),
		   "a closed source must be reopenable");
	zassert_ok(pull_frame(&i2s_in_a, &produced), "a reopened source must deliver again");
	zassert_true(produced > 0U);

	zassert_ok(audio_node_close(&i2s_in_a));
}

/* ---------------------------------------------------------------------------
 * In a whole pipeline
 * ---------------------------------------------------------------------------
 */

ZTEST(audio_i2s_in_node, test_i2s_in_never_reports_end_of_stream)
{
	const uint32_t frames = 4U * FRAMES_PER_BLOCK;
	uint32_t frame;

	format_a.sample_rate_hz = SAMPLE_RATE_HZ;
	format_a.channels = CHANNELS;
	format_a.valid_bits_per_sample = VALID_BITS;
	format_a.format = AUDIO_SAMPLE_FORMAT_S32_LE;

	zassert_ok(audio_pipeline_init(&test_pipeline, &test_config, &capture_sink));
	zassert_ok(audio_pipeline_set_format(&test_pipeline, &format_a));
	zassert_ok(audio_pipeline_start(&test_pipeline));

	/* Long enough to reuse every block in the slab several times over: a
	 * source that stopped returning blocks would run the slab dry here.
	 */
	for (frame = 0U; frame < frames; frame++) {
		zassert_equal(audio_pipeline_process_frame(&test_pipeline), 0,
			      "frame %u did not reach the sink", frame);
	}

	zassert_equal(capture_state_inst.frames, frames);
	zassert_equal(capture_state_inst.last_size, FRAME_SAMPLES);

	zassert_ok(audio_pipeline_join(&test_pipeline));
}

ZTEST(audio_i2s_in_node, test_i2s_in_failure_reaches_the_pipeline_as_an_error)
{
	int ret;

	format_a.sample_rate_hz = SAMPLE_RATE_HZ;
	format_a.channels = CHANNELS;
	format_a.valid_bits_per_sample = VALID_BITS;
	format_a.format = AUDIO_SAMPLE_FORMAT_S32_LE;

	zassert_ok(audio_pipeline_init(&test_pipeline, &test_config, &capture_sink));
	zassert_ok(audio_pipeline_set_format(&test_pipeline, &format_a));
	zassert_ok(audio_pipeline_start(&test_pipeline));

	zassert_equal(audio_pipeline_process_frame(&test_pipeline), 0);

	/* Drain whatever the node still holds, then break the wire. */
	fake_i2s_data_get(dev_a)->read_ret = -EAGAIN;

	do {
		ret = audio_pipeline_process_frame(&test_pipeline);
	} while (ret == 0);

	/* -EPIPE is what audio_pipeline_process_frame() returns for a finished
	 * track, and it is exactly what a failing capture must never produce:
	 * the worker would park and the application would be told the stream
	 * ended cleanly.
	 */
	zassert_not_equal(ret, -EPIPE, "a failing capture was reported as end of stream");
	zassert_equal(ret, -EAGAIN, "the transport failure must reach the application");

	zassert_ok(audio_pipeline_join(&test_pipeline));
}
