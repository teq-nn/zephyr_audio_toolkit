/*
 * Node selection: what an image gets when the nodes with dependencies are
 * switched off.
 *
 * Every node the subsystem ships is its own Kconfig symbol, and a node's
 * dependencies belong to that symbol: the two file nodes are the only ones that
 * select FILE_SYSTEM, and the two I2S nodes are the only ones that select I2S.
 * This suite is the other end of that range from tests/subsys/audio/pipeline:
 * here only the gain filter and the null sink are built, and the suite checks
 * both halves of the claim - that neither subsystem is dragged into the image,
 * and that a pipeline made of the remaining nodes still runs.
 *
 * The chain is driven by a scripted source defined in this file rather than by
 * the shared fakes next door, so the suite depends on nothing that could quietly
 * pull a node back in.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include <zephyr/audio/audio_node.h>
#include <zephyr/audio/audio_nodes.h>
#include <zephyr/audio/audio_pipeline.h>

/*
 * The load-bearing assertion of the whole change, and the reason it is a
 * BUILD_ASSERT: CONFIG_FILE_SYSTEM reaches this file from the generated
 * autoconf.h, i.e. from the .config this application produced, so it fails on
 * what the build actually configured rather than on what the Kconfig source
 * looks like.
 */
BUILD_ASSERT(!IS_ENABLED(CONFIG_FILE_SYSTEM),
	     "prj.conf enables no file node, so nothing may select FILE_SYSTEM");
BUILD_ASSERT(!IS_ENABLED(CONFIG_I2S),
	     "prj.conf enables no I2S node, so nothing may select I2S");

#define TEST_SOURCE_FRAMES 3U
#define TEST_SOURCE_SAMPLE 1000
#define TEST_CAPTURE_SAMPLES 64U

/* Scripted source: TEST_SOURCE_FRAMES frames of a constant, then end of
 * stream. Filling the whole frame keeps the arithmetic below independent of
 * CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES.
 */
struct test_source_state {
	uint32_t frames_left;
};

static int test_source_open(struct audio_node *node)
{
	struct test_source_state *state = node->state;

	state->frames_left = TEST_SOURCE_FRAMES;

	return 0;
}

static int test_source_process(struct audio_node *node, struct audio_buffer_view *buf,
			       size_t *out_size)
{
	struct test_source_state *state = node->state;
	size_t i;

	if (state->frames_left == 0U) {
		*out_size = 0U;
		return 0;
	}

	for (i = 0; i < buf->capacity; i++) {
		buf->data[i] = TEST_SOURCE_SAMPLE;
	}

	state->frames_left--;
	*out_size = buf->capacity;

	return 0;
}

static int test_source_close(struct audio_node *node)
{
	ARG_UNUSED(node);
	return 0;
}

static const struct audio_node_ops test_source_ops = {
	.open = test_source_open,
	.process = test_source_process,
	.close = test_source_close,
};

/* Capture sink: a null sink that keeps the head of the frame, so a test can
 * see what the gain filter produced.
 */
struct test_capture_state {
	int32_t samples[TEST_CAPTURE_SAMPLES];
	size_t captured;
	uint32_t frames;
};

static int test_capture_open(struct audio_node *node)
{
	struct test_capture_state *state = node->state;

	state->captured = 0U;
	state->frames = 0U;

	return 0;
}

static int test_capture_process(struct audio_node *node, struct audio_buffer_view *buf,
				size_t *out_size)
{
	struct test_capture_state *state = node->state;
	int ret;

	ret = audio_node_pull(node, buf, out_size);
	if (ret < 0 || *out_size == 0U) {
		return ret;
	}

	state->captured = MIN(*out_size, ARRAY_SIZE(state->samples));
	memcpy(state->samples, buf->data, state->captured * sizeof(state->samples[0]));
	state->frames++;

	return 0;
}

static int test_capture_close(struct audio_node *node)
{
	ARG_UNUSED(node);
	return 0;
}

static const struct audio_node_ops test_capture_ops = {
	.open = test_capture_open,
	.process = test_capture_process,
	.close = test_capture_close,
};

static struct audio_pipeline test_pipeline;
static struct test_source_state source_state;
static struct test_capture_state capture_state;

static struct audio_node test_source = {
	.role = AUDIO_NODE_ROLE_SOURCE,
	.ops = &test_source_ops,
	.upstream = NULL,
	.state = &source_state,
};

AUDIO_GAIN_FILTER_NODE_DEFINE(test_gain, &test_source, AUDIO_GAIN_UNITY_Q15 / 2);
AUDIO_NULL_SINK_NODE_DEFINE(test_null_sink, &test_gain);

static struct audio_node test_capture_sink = {
	.role = AUDIO_NODE_ROLE_SINK,
	.ops = &test_capture_ops,
	.upstream = &test_gain,
	.state = &capture_state,
};

static const struct audio_pipeline_config test_config = {
	.frame_samples = CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES,
	.event_cb = NULL,
	.event_user_data = NULL,
};

static const struct audio_stream_config test_format = {
	.sample_rate_hz = 48000U,
	.channels = 2U,
	.valid_bits_per_sample = 16U,
	.format = AUDIO_SAMPLE_FORMAT_S32_LE,
};

static void no_file_nodes_before(void *fixture)
{
	ARG_UNUSED(fixture);

	memset(&test_pipeline, 0, sizeof(test_pipeline));
	memset(&source_state, 0, sizeof(source_state));
	memset(&capture_state, 0, sizeof(capture_state));
}

static void no_file_nodes_after(void *fixture)
{
	ARG_UNUSED(fixture);

	(void)audio_pipeline_stop(&test_pipeline);
	(void)audio_pipeline_join(&test_pipeline);
}

/* The runtime half of the BUILD_ASSERT above: reported as a test case so the
 * claim shows up in the Twister report rather than only in a green build.
 */
ZTEST(audio_pipeline_no_file_nodes, test_config_file_system_stays_out)
{
	zassert_false(IS_ENABLED(CONFIG_FILE_SYSTEM),
		      "FILE_SYSTEM is set although no file node is enabled");
}

/* The same claim for the other subsystem a node selects, so an image that wires
 * up neither I2S node carries no I2S driver layer either.
 */
ZTEST(audio_pipeline_no_file_nodes, test_config_i2s_stays_out)
{
	zassert_false(IS_ENABLED(CONFIG_I2S), "I2S is set although no I2S node is enabled");
}

ZTEST(audio_pipeline_no_file_nodes, test_sink_runs_without_file_nodes)
{
	uint32_t frame;

	zassert_equal(audio_pipeline_init(&test_pipeline, &test_config, &test_null_sink), 0,
		      "init failed");
	zassert_equal(audio_pipeline_set_format(&test_pipeline, &test_format), 0,
		      "binding the pipeline format failed");
	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");

	for (frame = 0U; frame < TEST_SOURCE_FRAMES; frame++) {
		zassert_equal(audio_pipeline_process_frame(&test_pipeline), 0,
			      "frame %u did not reach the sink", frame);
	}

	zassert_equal(audio_pipeline_process_frame(&test_pipeline), -EPIPE,
		      "the drained source did not report end of stream");
	zassert_equal(source_state.frames_left, 0U, "the source was not drained");
}

ZTEST(audio_pipeline_no_file_nodes, test_filter_scales_without_file_nodes)
{
	size_t i;

	zassert_equal(audio_pipeline_init(&test_pipeline, &test_config, &test_capture_sink), 0,
		      "init failed");
	zassert_equal(audio_pipeline_set_format(&test_pipeline, &test_format), 0,
		      "binding the pipeline format failed");
	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");

	zassert_equal(audio_pipeline_process_frame(&test_pipeline), 0, "no frame produced");
	zassert_equal(capture_state.frames, 1U, "the sink saw no frame");
	zassert_true(capture_state.captured > 0U, "the sink captured nothing");

	for (i = 0; i < capture_state.captured; i++) {
		zassert_equal(capture_state.samples[i], TEST_SOURCE_SAMPLE / 2,
			      "sample %zu was not halved by the gain filter", i);
	}
}

ZTEST_SUITE(audio_pipeline_no_file_nodes, NULL, NULL, no_file_nodes_before, no_file_nodes_after,
	    NULL);
