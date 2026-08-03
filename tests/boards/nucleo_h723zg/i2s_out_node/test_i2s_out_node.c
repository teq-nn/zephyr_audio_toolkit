/*
 * Board suite for the I2S output sink node on the nucleo_h723zg target.
 *
 * Honest about what a board with nothing wired to it can answer. The node
 * configures itself as a clock *target* (AUDIO_I2S_OUT_TX_OPTIONS), so this
 * suite never pulls a frame and makes no claim about transmitted audio,
 * underrun recovery or pacing - those need a clocked link and belong to the
 * loopback application. Note that the board overlay now makes the transmit
 * block the clock *source* (#43, #44): the peripheral can drive MCLK/BICK/LRCK
 * even though this node does not ask it to. Closing that gap - a sink that
 * configures itself as the controller - belongs to the loopback application
 * too, not here.
 *
 * What it does answer is everything that is decided before the first bit
 * leaves: the build-time assertions below fail CI if the transfer blocks stop
 * satisfying the DMA reachability and cache-alignment rule the board overlay
 * records, if the block size stops tracking the pipeline's frame capacity, or
 * if the clock role ever grows a controller path. The run-time cases then open
 * and close the chain on real silicon, which needs no external clock at all,
 * and read back what the node actually put into the driver.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <zephyr/audio/audio_format.h>
#include <zephyr/audio/audio_i2s_wire.h>
#include <zephyr/audio/audio_node.h>
#include <zephyr/audio/audio_nodes.h>
#include <zephyr/audio/audio_pipeline.h>

#define I2S_TX_NODE DT_ALIAS(i2s_tx)

/*
 * Deliberately not CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES, and deliberately not
 * equal to it: the block arithmetic must follow the capacity this pipeline was
 * defined with, not a Kconfig symbol whose meaning issue #23 may still change.
 * The assertion below is what makes that visible rather than coincidental.
 */
#define FRAME_SAMPLES 256
#define TX_BLOCKS     4

#define SAMPLE_RATE_HZ 48000U
#define CHANNELS       2U
#define VALID_BITS     16U

/* A depth the container describes but the wire seam does not carry. */
#define UNSUPPORTED_BITS 24U

/* ---------------------------------------------------------------------------
 * Build-time assertions
 * ---------------------------------------------------------------------------
 */

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(I2S_TX_NODE),
	     "the i2s-tx alias must resolve to an enabled node");

/*
 * The AK4619 has no clock output, so the transmit block owns the clocks and
 * drives MCLK (docs/hardware/akd4619-evaluation-board.md §2). What this node
 * asks the driver for is a separate question, and AUDIO_I2S_OUT_TX_OPTIONS is
 * still the two target bits - being exactly those two is the stronger claim,
 * since the controller constants are zero and an option word that had lost a
 * bit would silently mean "controller" rather than fail.
 */
BUILD_ASSERT(DT_PROP(I2S_TX_NODE, mck_enabled),
	     "the transmit block must drive MCLK: the codec has no clock output");
BUILD_ASSERT(AUDIO_I2S_OUT_TX_OPTIONS == (I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET),
	     "the sink must configure both clocks as targets and nothing else");

/*
 * Cache maintenance around every transfer acts on whole lines, so a block must
 * be line aligned and line sized (manifest §6). On this part the line is 32.
 */
BUILD_ASSERT(AUDIO_I2S_BLOCK_ALIGN >= CONFIG_DCACHE_LINE_SIZE,
	     "transfer blocks must be aligned to at least one d-cache line");
BUILD_ASSERT((AUDIO_I2S_BLOCK_ALIGN & (AUDIO_I2S_BLOCK_ALIGN - 1)) == 0,
	     "the block alignment must be a power of two");
/* A block that is not a whole number of cache lines lets a flush clobber
 * whatever shares its last line.
 */
BUILD_ASSERT((AUDIO_I2S_BLOCK_BYTES(FRAME_SAMPLES) % AUDIO_I2S_BLOCK_ALIGN) == 0,
	     "a block must be a whole number of cache lines");

/*
 * dma1 lives in the D2 domain and cannot address DTCM, and the slab lands in
 * the image's zephyr,sram. A target that put zephyr,sram in DTCM would need the
 * slab relocated, and the failure would be silent - the transfer completes and
 * moves nothing - so it is caught here instead.
 */
BUILD_ASSERT(DT_REG_ADDR(DT_CHOSEN(zephyr_sram)) != 0x20000000UL,
	     "zephyr,sram is DTCM, which dma1 cannot address");

/* The block has to hold a whole frame of the widest word the container can
 * ever produce, and it has to grow when the frame does.
 */
BUILD_ASSERT(AUDIO_I2S_BLOCK_BYTES(FRAME_SAMPLES) >=
		     (size_t)FRAME_SAMPLES * AUDIO_I2S_WIRE_MAX_WORD_BYTES,
	     "one block must hold one frame");
BUILD_ASSERT(AUDIO_I2S_BLOCK_BYTES(2 * FRAME_SAMPLES) >
		     AUDIO_I2S_BLOCK_BYTES(FRAME_SAMPLES),
	     "the block size must follow the frame capacity, not be a constant");
/* And it follows the capacity this pipeline was defined with rather than the
 * Kconfig symbol, which is why FRAME_SAMPLES differs from it above.
 */
BUILD_ASSERT(AUDIO_I2S_BLOCK_BYTES(FRAME_SAMPLES) !=
		     AUDIO_I2S_BLOCK_BYTES(CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES),
	     "the block size must not come from CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES");

/* ---------------------------------------------------------------------------
 * The chain under test
 * ---------------------------------------------------------------------------
 */

/*
 * A source of digital silence, defined here rather than borrowed: a sink needs
 * an upstream, and every node the module ships that could play one would drag a
 * filesystem onto a board that has no storage.
 */
static int silence_source_open(struct audio_node *node)
{
	ARG_UNUSED(node);
	return 0;
}

static int silence_source_process(struct audio_node *node, struct audio_buffer_view *buf,
				  size_t *out_size)
{
	size_t samples;

	if (!node || !buf || !buf->data || !out_size) {
		return -EINVAL;
	}

	samples = ROUND_DOWN(buf->capacity, CHANNELS);
	memset(buf->data, 0, samples * sizeof(buf->data[0]));
	*out_size = samples;

	return 0;
}

static int silence_source_close(struct audio_node *node)
{
	ARG_UNUSED(node);
	return 0;
}

static const struct audio_node_ops silence_source_ops = {
	.open = silence_source_open,
	.process = silence_source_process,
	.close = silence_source_close,
};

AUDIO_NODE_DEFINE(silence_source, AUDIO_NODE_ROLE_SOURCE, &silence_source_ops, NULL, NULL);

AUDIO_I2S_OUT_NODE_DEFINE(i2s_out, &silence_source, I2S_TX_NODE, FRAME_SAMPLES, TX_BLOCKS);

AUDIO_PIPELINE_DEFINE(test_pipeline, FRAME_SAMPLES, 2048, 5);

static const struct audio_pipeline_config test_config = {
	.frame_samples = FRAME_SAMPLES,
};

static const struct device *const i2s_tx_dev = DEVICE_DT_GET(I2S_TX_NODE);

/* Bind the chain to a format and leave it closed, so every case below starts
 * from the same place whatever order they run in.
 */
static int bind_format(uint8_t valid_bits_per_sample)
{
	const struct audio_stream_config fmt = {
		.sample_rate_hz = SAMPLE_RATE_HZ,
		.channels = CHANNELS,
		.valid_bits_per_sample = valid_bits_per_sample,
		.format = AUDIO_SAMPLE_FORMAT_S32_LE,
	};
	int ret;

	ret = audio_pipeline_init(&test_pipeline, &test_config, &i2s_out);
	if (ret < 0) {
		return ret;
	}

	return audio_pipeline_set_format(&test_pipeline, &fmt);
}

static void *i2s_out_setup(void)
{
	printk("nucleo_h723zg i2s_out: %s, %zu byte blocks x %u\n", i2s_tx_dev->name,
	       (size_t)AUDIO_I2S_BLOCK_BYTES(FRAME_SAMPLES), TX_BLOCKS);

	return NULL;
}

static void i2s_out_after(void *fixture)
{
	ARG_UNUSED(fixture);

	/* Idempotent, so it also cleans up after a case that failed halfway. */
	(void)audio_pipeline_join(&test_pipeline);
}

ZTEST_SUITE(board_i2s_out_node, NULL, i2s_out_setup, NULL, i2s_out_after, NULL);

ZTEST(board_i2s_out_node, test_i2s_out_takes_its_device_from_devicetree)
{
	zassert_equal(i2s_out_state.dev, i2s_tx_dev,
		      "the node must use the devicetree node it was defined with");
	zassert_true(device_is_ready(i2s_out_state.dev), "%s not ready", i2s_out_state.dev->name);
}

ZTEST(board_i2s_out_node, test_i2s_out_owns_transfer_blocks_dma_can_reach)
{
	uintptr_t base = (uintptr_t)i2s_out_slab.buffer;
	size_t span = (size_t)i2s_out_slab.info.num_blocks * i2s_out_slab.info.block_size;

	zassert_equal(i2s_out_slab.info.block_size, AUDIO_I2S_BLOCK_BYTES(FRAME_SAMPLES),
		      "the slab must carry the block size the node reports to the driver");
	zassert_equal(i2s_out_slab.info.num_blocks, TX_BLOCKS);
	zassert_equal(base % AUDIO_I2S_BLOCK_ALIGN, 0U,
		      "the slab is not cache-line aligned, a flush would clobber its neighbour");

	/* The other half of the rule: alignment says nothing about whether dma1
	 * can see the memory at all.
	 */
	zassert_between_inclusive(base, (uintptr_t)DT_REG_ADDR(DT_CHOSEN(zephyr_sram)),
				  (uintptr_t)(DT_REG_ADDR(DT_CHOSEN(zephyr_sram)) +
					      DT_REG_SIZE(DT_CHOSEN(zephyr_sram)) - span),
				  "the transfer blocks are outside the SRAM dma1 can address");
}

ZTEST(board_i2s_out_node, test_i2s_out_configures_the_device_as_a_clock_target)
{
	const struct i2s_config *cfg;

	zassert_ok(bind_format(VALID_BITS));
	zassert_ok(audio_pipeline_start(&test_pipeline));

	cfg = i2s_config_get(i2s_tx_dev, I2S_DIR_TX);
	zassert_not_null(cfg, "the transmit direction was not configured by open()");

	zassert_equal(cfg->options & (I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET),
		      I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET,
		      "the sink must be a target on both clocks, options are 0x%02x", cfg->options);

	/* Everything below came from the bound format, which is the point: the
	 * node stores neither the rate nor the channel count.
	 */
	zassert_equal(cfg->frame_clk_freq, SAMPLE_RATE_HZ);
	zassert_equal(cfg->channels, CHANNELS);
	zassert_equal(cfg->word_size, VALID_BITS);
	zassert_equal(cfg->mem_slab, &i2s_out_slab, "the node must transmit from its own slab");
	zassert_equal(cfg->block_size, AUDIO_I2S_BLOCK_BYTES(FRAME_SAMPLES));

	zassert_ok(audio_pipeline_join(&test_pipeline));
}

ZTEST(board_i2s_out_node, test_i2s_out_reopens_after_close)
{
	/* close() has to stop the device and hand every queued block back, or
	 * the second open() below would find the direction busy.
	 */
	zassert_ok(bind_format(VALID_BITS));
	zassert_ok(audio_pipeline_start(&test_pipeline));
	zassert_ok(audio_pipeline_join(&test_pipeline));

	zassert_ok(bind_format(VALID_BITS));
	zassert_ok(audio_pipeline_start(&test_pipeline), "a closed sink must be reopenable");
	zassert_ok(audio_pipeline_join(&test_pipeline));
}

ZTEST(board_i2s_out_node, test_i2s_out_open_refuses_an_unsupported_depth)
{
	zassert_ok(bind_format(UNSUPPORTED_BITS));

	/* Nodes validate, they do not adapt: a depth the wire seam cannot carry
	 * fails the open rather than reaching the wire as something else.
	 */
	zassert_equal(audio_pipeline_start(&test_pipeline), -ENOTSUP,
		      "a %u bit format must be refused by open()", UNSUPPORTED_BITS);

	/* And it left nothing started, so the next run configures a clean
	 * direction rather than inheriting a half-open one.
	 */
	zassert_ok(bind_format(VALID_BITS));
	zassert_ok(audio_pipeline_start(&test_pipeline));
	zassert_ok(audio_pipeline_join(&test_pipeline));
}
