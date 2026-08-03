/*
 * Frame sizing: CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES and the _frame_samples
 * argument of AUDIO_PIPELINE_DEFINE() count TOTAL interleaved samples across
 * all channels, never samples per channel (issue #23, manifest §5/§6).
 *
 * The distinction is invisible in a mono run and worth exactly a factor of
 * "channels" in every other one, so it is pinned here on the two places it is
 * observable: the storage the macro allocates, and the capacity the chain is
 * handed. The complement of that decision is checked as well - a format with
 * more channels than the frame has samples cannot describe one interleaved
 * sample set, and audio_pipeline_set_format() refuses it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <zephyr/audio/audio_format.h>
#include <zephyr/audio/audio_node.h>
#include <zephyr/audio/audio_pipeline.h>

#include "fake_nodes.h"

/* One frame of 16 total samples run as stereo, i.e. 8 sample pairs. Both
 * readings of the symbol are expressible at these numbers, and they differ, so
 * an accidental per-channel allocation would show up as 32 samples of storage.
 */
#define SIZING_FRAME_SAMPLES 16
#define SIZING_CHANNELS      2U
#define SIZING_SAMPLE_SETS   (SIZING_FRAME_SAMPLES / SIZING_CHANNELS)

/* The floor CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES allows: exactly one stereo
 * sample set and nothing more.
 */
#define MINIMAL_FRAME_SAMPLES 2

AUDIO_FAKE_SOURCE_DEFINE(sizing_source);
AUDIO_FAKE_SINK_DEFINE(sizing_sink, &sizing_source);

AUDIO_FAKE_SOURCE_DEFINE(minimal_source);
AUDIO_FAKE_SINK_DEFINE(minimal_sink, &minimal_source);

AUDIO_PIPELINE_DEFINE(sizing_pipeline, SIZING_FRAME_SAMPLES,
		      CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE, CONFIG_AUDIO_PIPELINE_THREAD_PRIO);
AUDIO_PIPELINE_DEFINE(minimal_pipeline, MINIMAL_FRAME_SAMPLES,
		      CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE, CONFIG_AUDIO_PIPELINE_THREAD_PRIO);

/* The same assertion the suite makes at run time, made where it costs nothing:
 * the macro multiplies its argument by sizeof(int32_t) and by nothing else.
 */
BUILD_ASSERT(sizeof(sizing_pipeline_frame_buf) == SIZING_FRAME_SAMPLES * sizeof(int32_t),
	     "AUDIO_PIPELINE_DEFINE() allocated something other than the total sample count");
BUILD_ASSERT(sizeof(minimal_pipeline_frame_buf) == MINIMAL_FRAME_SAMPLES * sizeof(int32_t),
	     "AUDIO_PIPELINE_DEFINE() allocated something other than the total sample count");

static const struct audio_pipeline_config sizing_config = {
	.frame_samples = SIZING_FRAME_SAMPLES,
};

static const struct audio_pipeline_config minimal_config = {
	.frame_samples = MINIMAL_FRAME_SAMPLES,
};

static const struct audio_stream_config stereo_format = {
	.sample_rate_hz = 48000U,
	.channels = SIZING_CHANNELS,
	.valid_bits_per_sample = 24U,
	.format = AUDIO_SAMPLE_FORMAT_S32_LE,
};

static void frame_sizing_before(void *fixture)
{
	ARG_UNUSED(fixture);

	/* The pipeline instances are macro-defined, so they must not be wiped:
	 * the macro is what filled their resource fields.
	 */
	audio_fake_source_reset(&sizing_source_state);
	audio_fake_sink_reset(&sizing_sink_state);
	audio_fake_source_reset(&minimal_source_state);
	audio_fake_sink_reset(&minimal_sink_state);
}

static void frame_sizing_after(void *fixture)
{
	ARG_UNUSED(fixture);

	(void)audio_pipeline_join(&sizing_pipeline);
	(void)audio_pipeline_join(&minimal_pipeline);
}

ZTEST(audio_pipeline_frame_sizing, test_pipeline_allocates_total_interleaved_samples)
{
	zassert_equal(sizeof(sizing_pipeline_frame_buf),
		      (size_t)SIZING_FRAME_SAMPLES * sizeof(int32_t),
		      "frame buffer is not frame_samples int32_t");
	zassert_equal(ARRAY_SIZE(sizing_pipeline_frame_buf), (size_t)SIZING_FRAME_SAMPLES,
		      "frame buffer holds something other than frame_samples samples");

	/* The per-channel reading, spelled out so it cannot creep back in: it
	 * would have made this buffer twice as large for the stereo format the
	 * suite binds below.
	 */
	zassert_not_equal(ARRAY_SIZE(sizing_pipeline_frame_buf),
			  (size_t)SIZING_FRAME_SAMPLES * SIZING_CHANNELS,
			  "frame buffer was allocated per channel");

	zassert_equal(audio_pipeline_init(&sizing_pipeline, &sizing_config, &sizing_sink), 0,
		      "init failed");
	zassert_equal(audio_pipeline_set_format(&sizing_pipeline, &stereo_format), 0,
		      "binding the stereo format failed");

	zassert_equal_ptr(sizing_pipeline.frame_buf, sizing_pipeline_frame_buf,
			  "init() moved the pipeline off its own frame buffer");
	zassert_equal(sizing_pipeline.frame_capacity, (size_t)SIZING_FRAME_SAMPLES,
		      "frame capacity is not the total sample count");
	zassert_equal(sizing_pipeline.frame_capacity / SIZING_CHANNELS,
		      (size_t)SIZING_SAMPLE_SETS, "wrong number of interleaved sample sets");
}

ZTEST(audio_pipeline_frame_sizing, test_pipeline_hands_the_chain_the_total_frame_capacity)
{
	sizing_source_state.frames_total = 1U;
	sizing_source_state.pattern = 0x5a5a5a5a;

	sizing_sink_state.check_pattern = true;
	sizing_sink_state.expect_pattern = 0x5a5a5a5a;
	/* The whole interleaved frame, not one channel's share of it. */
	sizing_sink_state.expect_capacity = SIZING_FRAME_SAMPLES;

	zassert_equal(audio_pipeline_init(&sizing_pipeline, &sizing_config, &sizing_sink), 0,
		      "init failed");
	zassert_equal(audio_pipeline_set_format(&sizing_pipeline, &stereo_format), 0,
		      "binding the stereo format failed");

	zassert_equal(audio_pipeline_process_frame(&sizing_pipeline), 0, "frame not produced");

	zassert_equal(atomic_get(&sizing_sink_state.frames_seen), 1U, "sink saw no frame");
	zassert_equal(atomic_get(&sizing_sink_state.wrong_capacity), 0U,
		      "chain ran on a capacity other than the total frame size");
	zassert_equal(atomic_get(&sizing_sink_state.corrupt_frames), 0U, "frame content corrupted");
}

ZTEST(audio_pipeline_frame_sizing, test_pipeline_set_format_rejects_more_channels_than_a_frame)
{
	struct audio_stream_config too_many_channels = stereo_format;

	too_many_channels.channels = (uint8_t)(MINIMAL_FRAME_SAMPLES + 1);

	zassert_equal(audio_pipeline_init(&minimal_pipeline, &minimal_config, &minimal_sink), 0,
		      "init failed");

	/* One interleaved sample set of 3 channels does not fit a 2 sample
	 * frame, so no node could ever deliver this format.
	 */
	zassert_equal(audio_pipeline_set_format(&minimal_pipeline, &too_many_channels), -EINVAL,
		      "a format wider than the frame was accepted");

	/* A refused bind must leave no format behind: start() still reports
	 * "nobody bound one" rather than running on the rejected value.
	 */
	zassert_equal(audio_pipeline_start(&minimal_pipeline), -ENODATA,
		      "the refused format was bound anyway");

	/* Exactly one stereo sample set is the smallest frame that works, and
	 * it does work.
	 */
	zassert_equal(audio_pipeline_set_format(&minimal_pipeline, &stereo_format), 0,
		      "the smallest legal stereo frame was refused");
	zassert_equal(minimal_pipeline.frame_capacity, (size_t)MINIMAL_FRAME_SAMPLES,
		      "wrong frame capacity");
}

ZTEST_SUITE(audio_pipeline_frame_sizing, NULL, NULL, frame_sizing_before, frame_sizing_after, NULL);
