/*
 * Two suites:
 *
 *  - audio_pipeline: the argument/state guards of the control API plus a
 *    start/play/stop/join smoke test. These predate the file nodes and do not
 *    touch the filesystem.
 *  - audio_pipeline_negative: the negative test strategy of spec §12.3 driven
 *    end to end through the worker thread - a corrupt header aborting start(),
 *    a truncated file ending as a clean EOF, a simulated I/O error surfacing as
 *    an ERROR event, the reserved -EPIPE arriving as an -EIO ERROR whether it
 *    comes from a source below a filter or from the sink itself, and the two
 *    file-writer overflow guards (-EFBIG, -ENOSPC).
 *
 * Everything runs headless on native_sim, without audio hardware (spec §12.1).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <zephyr/audio/audio_node.h>
#include <zephyr/audio/audio_nodes.h>
#include <zephyr/audio/audio_pipeline.h>
#include <zephyr/audio/audio_pipeline_events.h>
#include <zephyr/audio/audio_wav.h>

#include "fake_nodes.h"
#include "wav_fixture.h"

/* =========================================================================
 * Suite 1: audio_pipeline - control API guards and a lifecycle smoke test.
 * ======================================================================
 */

static void test_event_handler(const struct audio_pipeline_event *event, void *user_data)
{
	ARG_UNUSED(event);
	ARG_UNUSED(user_data);
}

ZTEST(audio_pipeline, test_invalid_config_rejected)
{
	struct audio_pipeline_config cfg = { 0 };

	zassert_false(audio_pipeline_config_is_valid(&cfg), "invalid config accepted");
}

ZTEST(audio_pipeline, test_lifecycle_rejects_null_pipeline)
{
	zassert_equal(audio_pipeline_start(NULL), -EINVAL, "start(NULL) accepted");
	zassert_equal(audio_pipeline_play(NULL), -EINVAL, "play(NULL) accepted");
	zassert_equal(audio_pipeline_stop(NULL), -EINVAL, "stop(NULL) accepted");
	zassert_equal(audio_pipeline_join(NULL), -EINVAL, "join(NULL) accepted");
	zassert_false(audio_pipeline_is_running(NULL), "is_running(NULL) true");
	zassert_false(audio_pipeline_is_playing(NULL), "is_playing(NULL) true");
}

ZTEST(audio_pipeline, test_lifecycle_rejects_uninitialised_pipeline)
{
	struct audio_pipeline pipeline = {0};

	zassert_equal(audio_pipeline_start(&pipeline), -EINVAL, "start before init accepted");
	zassert_equal(audio_pipeline_play(&pipeline), -EINVAL, "play before init accepted");
	zassert_equal(audio_pipeline_stop(&pipeline), -EINVAL, "stop before init accepted");
	zassert_equal(audio_pipeline_join(&pipeline), -EINVAL, "join before init accepted");
}

ZTEST(audio_pipeline, test_pipeline_start_play_stop_join)
{
	struct audio_fake_source source_state = { .frames_total = 0U };
	struct audio_node source = {
		.role = AUDIO_NODE_ROLE_SOURCE,
		.ops = &audio_fake_source_ops,
		.upstream = NULL,
		.state = &source_state,
	};
	struct audio_node sink = {
		.role = AUDIO_NODE_ROLE_SINK,
		.ops = &null_sink_node_ops,
		.upstream = &source,
		.state = NULL,
	};
	struct audio_pipeline_config cfg = {
		.stream = {
			.sample_rate_hz = 44100U,
			.channels = 2U,
			.valid_bits_per_sample = 24U,
			.format = AUDIO_SAMPLE_FORMAT_S32_LE,
		},
		.frame_samples = CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES,
		.event_cb = test_event_handler,
		.event_user_data = NULL,
	};
	struct audio_pipeline pipeline = {0};

	zassert_true(audio_pipeline_config_is_valid(&cfg), "config must be valid");
	zassert_equal(audio_pipeline_init(&pipeline, &cfg, &sink), 0, "init failed");
	zassert_equal(audio_pipeline_start(&pipeline), 0, "start failed");
	zassert_true(audio_pipeline_is_running(&pipeline), "worker thread missing");

	/* A source scripted to zero frames ends the stream on the first pull;
	 * the thread survives the EOF.
	 */
	zassert_equal(audio_pipeline_play(&pipeline), 0, "play failed");
	k_msleep(20);
	zassert_true(audio_pipeline_is_running(&pipeline), "EOF killed the worker thread");
	zassert_false(audio_pipeline_is_playing(&pipeline), "still playing after EOF");

	zassert_equal(audio_pipeline_stop(&pipeline), 0, "stop failed");
	zassert_equal(audio_pipeline_join(&pipeline), 0, "join failed");
	zassert_false(audio_pipeline_is_running(&pipeline), "worker thread still running");
}

/* One of each shape that reads from upstream - a shipped sink, a shipped filter
 * and the shared test fake - all wired without one.
 */
AUDIO_NULL_SINK_NODE_DEFINE(orphan_null_sink, NULL);
AUDIO_GAIN_FILTER_NODE_DEFINE(orphan_gain_filter, NULL, AUDIO_GAIN_UNITY_Q15);
AUDIO_FAKE_SINK_DEFINE(orphan_fake_sink, NULL);

ZTEST(audio_pipeline, test_sink_and_filter_without_upstream_report_enotsup)
{
	struct audio_node *const orphans[] = {
		&orphan_null_sink,
		&orphan_gain_filter,
		&orphan_fake_sink,
	};
	int32_t buf[8];
	struct audio_buffer_view view = {
		.data = buf,
		.capacity = ARRAY_SIZE(buf),
	};
	size_t i;

	/* Spec §4.3/§4.4: a filter and a sink have an upstream, so a missing one
	 * is a wiring error - and the same one for every node, whoever wrote it.
	 * Reporting a clean EOF instead would swallow the track in silence.
	 */
	for (i = 0; i < ARRAY_SIZE(orphans); i++) {
		size_t produced = 1;
		int ret;

		zassert_equal(audio_node_open(orphans[i]), 0, "node %zu: open failed", i);

		ret = audio_node_process(orphans[i], &view, &produced);
		zassert_equal(ret, -ENOTSUP, "node %zu: expected -ENOTSUP without upstream, got %d",
			      i, ret);
		zassert_equal(produced, 0U, "node %zu: a failing process() claimed samples", i);

		zassert_equal(audio_node_close(orphans[i]), 0, "node %zu: close failed", i);
	}
}

ZTEST_SUITE(audio_pipeline, NULL, NULL, NULL, NULL, NULL);

/* =========================================================================
 * Suite 2: audio_pipeline_negative - spec §12.3 end to end.
 * ======================================================================
 */

#define NEG_FRAME_SAMPLES 16

static const struct audio_pipeline_config neg_config = {
	.stream = {
		.sample_rate_hz = 48000U,
		.channels = 2U,
		.valid_bits_per_sample = 16U,
		.format = AUDIO_SAMPLE_FORMAT_S32_LE,
	},
	.frame_samples = NEG_FRAME_SAMPLES,
	.event_cb = NULL,
	.event_user_data = NULL,
};

/* Corrupt header -> reader open() fails -> ERROR. */
AUDIO_FILE_READER_NODE_DEFINE(neg_bad_reader, AUDIO_TEST_PATH("neg_bad.wav"));
AUDIO_FILE_WRITER_NODE_DEFINE(neg_bad_writer, &neg_bad_reader, AUDIO_TEST_PATH("neg_bad_out.wav"));
AUDIO_PIPELINE_DEFINE(neg_bad_pipeline, NEG_FRAME_SAMPLES,
		      CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE, CONFIG_AUDIO_PIPELINE_THREAD_PRIO);

/* Truncated file -> reader short read -> clean EOF, no ERROR. */
AUDIO_FILE_READER_NODE_DEFINE(neg_trunc_reader, AUDIO_TEST_PATH("neg_trunc.wav"));
AUDIO_FILE_WRITER_NODE_DEFINE(neg_trunc_writer, &neg_trunc_reader,
			      AUDIO_TEST_PATH("neg_trunc_out.wav"));
AUDIO_PIPELINE_DEFINE(neg_trunc_pipeline, NEG_FRAME_SAMPLES,
		      CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE, CONFIG_AUDIO_PIPELINE_THREAD_PRIO);

/* Simulated I/O error mid-stream -> ERROR carrying the code. The shared fake
 * source delivers a few good frames and then fails, so the fault lands at a
 * fixed frame instead of depending on the timing of a real disk error.
 */
AUDIO_FAKE_SOURCE_DEFINE(neg_fault_source);
AUDIO_FILE_WRITER_NODE_DEFINE(neg_fault_writer, &neg_fault_source,
			      AUDIO_TEST_PATH("neg_fault_out.wav"));
AUDIO_PIPELINE_DEFINE(neg_fault_pipeline, NEG_FRAME_SAMPLES,
		      CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE, CONFIG_AUDIO_PIPELINE_THREAD_PRIO);

/* A source failing with the reserved EOF code, seen through a filter: neither
 * the filter nor the sink may pass -EPIPE on, or the application would be told
 * the track finished cleanly.
 */
AUDIO_FAKE_SOURCE_DEFINE(neg_epipe_source);
AUDIO_GAIN_FILTER_NODE_DEFINE(neg_epipe_filter, &neg_epipe_source, AUDIO_GAIN_UNITY_Q15);
AUDIO_NULL_SINK_NODE_DEFINE(neg_epipe_sink, &neg_epipe_filter);
AUDIO_PIPELINE_DEFINE(neg_epipe_pipeline, NEG_FRAME_SAMPLES,
		      CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE, CONFIG_AUDIO_PIPELINE_THREAD_PRIO);

/* The same reserved code, one level further down the chain: a sink failing with
 * -EPIPE in its own body, where no pull can intercept it.
 */
AUDIO_FAKE_SOURCE_DEFINE(neg_sink_epipe_source);
AUDIO_FAKE_SINK_DEFINE(neg_sink_epipe_sink, &neg_sink_epipe_source);
AUDIO_PIPELINE_DEFINE(neg_sink_epipe_pipeline, NEG_FRAME_SAMPLES,
		      CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE, CONFIG_AUDIO_PIPELINE_THREAD_PRIO);

/* -EFBIG guard: one frame past a data chunk that is already at the 32 bit
 * ceiling. The source is scripted to hand out one stereo frame per call and
 * never to end, so the guard - not the end of the stream - is what stops it.
 */
AUDIO_FAKE_SOURCE_DEFINE(neg_efbig_source);
AUDIO_FILE_WRITER_NODE_DEFINE(neg_efbig_writer, &neg_efbig_source,
			      AUDIO_TEST_PATH("neg_efbig.wav"));

static void negative_before(void *fixture)
{
	ARG_UNUSED(fixture);

	zassert_equal(audio_test_fs_mount(), 0, "fixture filesystem did not mount");
}

/* -------------------------------------------------------------------------
 * Corrupted WAV header -> open() fails and an ERROR event is observed.
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_negative, test_source_corrupt_header_emits_error_event)
{
	static const uint8_t garbage[64] = {'N', 'O', 'T', 'A', 'W', 'A', 'V', 'E'};
	struct audio_pipeline_event event;
	int ret;

	zassert_equal(audio_test_write_raw(AUDIO_TEST_PATH("neg_bad.wav"), garbage, sizeof(garbage)),
		      0, "could not write the corrupt fixture");

	zassert_equal(audio_pipeline_init(&neg_bad_pipeline, &neg_config, &neg_bad_writer), 0,
		      "init failed");

	/* start() opens the chain sink-first, then upstream; the reader's open()
	 * rejects the header, so start() fails and no worker thread is created.
	 */
	ret = audio_pipeline_start(&neg_bad_pipeline);
	zassert_true(ret < 0, "start() must fail on a corrupt source header");
	zassert_equal(ret, -EINVAL, "a corrupt header must surface as -EINVAL, got %d", ret);
	zassert_false(audio_pipeline_is_running(&neg_bad_pipeline), "a thread was created anyway");

	zassert_equal(audio_pipeline_get_event(&neg_bad_pipeline, &event, K_NO_WAIT), 0,
		      "no ERROR event was published");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_ERROR, "expected an ERROR event, got %d",
		      (int)event.type);
	zassert_equal(event.err, ret, "the event carries %d, start() returned %d", event.err, ret);
	zassert_not_equal(event.err, -EPIPE, "an ERROR event must not carry the EOF code");
}

/* -------------------------------------------------------------------------
 * Early EOF (truncated file) -> a clean EOF event, no ERROR.
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_negative, test_source_truncated_file_reports_clean_eof)
{
	/* The header promises 8 KiB of payload; the file carries eight samples.
	 * The reader must treat the short read as end of stream, not an error.
	 */
	int16_t payload[8] = {1, -1, 2, -2, 3, -3, 4, -4};
	struct audio_test_wav_spec spec = {
		.declared_data_size = 8192U,
		.payload = payload,
		.payload_len = sizeof(payload),
	};
	struct audio_pipeline_event event;

	zassert_equal(audio_test_write_wav(AUDIO_TEST_PATH("neg_trunc.wav"), &spec), 0,
		      "could not write the truncated fixture");

	zassert_equal(audio_pipeline_init(&neg_trunc_pipeline, &neg_config, &neg_trunc_writer), 0,
		      "init failed");
	zassert_equal(audio_pipeline_start(&neg_trunc_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&neg_trunc_pipeline), 0, "play failed");

	zassert_equal(audio_pipeline_get_event(&neg_trunc_pipeline, &event, K_SECONDS(2)), 0,
		      "no event within 2 s");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_EOF,
		      "a truncated file must end as a clean EOF, got type %d err %d",
		      (int)event.type, event.err);
	zassert_equal(event.err, 0, "the EOF event carries an error");

	/* Nothing may follow the EOF - least of all an ERROR. */
	zassert_equal(audio_pipeline_get_event(&neg_trunc_pipeline, &event, K_NO_WAIT), -ENOMSG,
		      "an event followed the clean EOF");

	zassert_equal(audio_pipeline_join(&neg_trunc_pipeline), 0, "join failed");

	/* The eight samples that were there did make it through to the sink: read
	 * the finalised output back and check its data chunk. close() has since
	 * reset the writer's own counter, so the file on disk is the source of
	 * truth here.
	 */
	{
		static uint8_t trunc_buf[128];
		struct audio_wav_header wav;
		size_t read;

		read = audio_test_read_file(AUDIO_TEST_PATH("neg_trunc_out.wav"), trunc_buf,
					    sizeof(trunc_buf));

		zassert_equal(audio_wav_read_header(trunc_buf, read, &wav), 0,
			      "the sink left an unparsable file");
		zassert_equal(wav.data_size, sizeof(payload),
			      "the writer stored %u bytes, expected %u", wav.data_size,
			      (unsigned int)sizeof(payload));
		zassert_equal(read, AUDIO_WAV_MIN_HEADER_SIZE + sizeof(payload),
			      "output file is the wrong length");
	}
}

/* -------------------------------------------------------------------------
 * Simulated I/O error during processing -> ERROR event carrying the code.
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_negative, test_processing_error_emits_error_event_with_code)
{
	struct audio_pipeline_event event;

	/* One good frame, then a filesystem-style failure. -EIO is what both file
	 * nodes remap a stray -EPIPE to, so it is the natural stand-in for a disk
	 * that stops accepting data mid-stream (spec §12.3).
	 */
	audio_fake_source_reset(&neg_fault_source_state);
	neg_fault_source_state.frames_total = AUDIO_FAKE_ENDLESS;
	neg_fault_source_state.fail_at_frame = 2U; /* one good frame, then fail */
	neg_fault_source_state.process_ret = -EIO;

	zassert_equal(audio_pipeline_init(&neg_fault_pipeline, &neg_config, &neg_fault_writer), 0,
		      "init failed");
	zassert_equal(audio_pipeline_start(&neg_fault_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&neg_fault_pipeline), 0, "play failed");

	zassert_equal(audio_pipeline_get_event(&neg_fault_pipeline, &event, K_SECONDS(2)), 0,
		      "no event within 2 s");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_ERROR,
		      "a mid-stream failure must raise an ERROR, got type %d", (int)event.type);
	zassert_equal(event.err, -EIO, "the ERROR event must carry the failing code, got %d",
		      event.err);
	zassert_not_equal(event.err, -EPIPE, "an ERROR event must not carry the EOF code");
	zassert_false(audio_pipeline_is_playing(&neg_fault_pipeline), "still playing after an error");

	zassert_equal(audio_pipeline_join(&neg_fault_pipeline), 0, "join failed");
}

/* -------------------------------------------------------------------------
 * A source failing with -EPIPE, two nodes below the application.
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_negative, test_source_epipe_through_filter_is_not_an_eof)
{
	struct audio_pipeline_event event;

	/* -EPIPE is the pipeline's own end-of-stream code, so a node that
	 * forwarded it would turn a third-party source's failure into a clean
	 * EOF. The chain here is source -> filter -> sink: the remap has to
	 * happen wherever the failure enters, not only at the sink.
	 */
	audio_fake_source_reset(&neg_epipe_source_state);
	neg_epipe_source_state.frames_total = AUDIO_FAKE_ENDLESS;
	neg_epipe_source_state.fail_at_frame = 2U; /* one good frame, then fail */
	neg_epipe_source_state.process_ret = -EPIPE;

	zassert_equal(audio_pipeline_init(&neg_epipe_pipeline, &neg_config, &neg_epipe_sink), 0,
		      "init failed");
	zassert_equal(audio_pipeline_start(&neg_epipe_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&neg_epipe_pipeline), 0, "play failed");

	zassert_equal(audio_pipeline_get_event(&neg_epipe_pipeline, &event, K_SECONDS(2)), 0,
		      "no event within 2 s");
	zassert_not_equal(event.type, AUDIO_PIPELINE_EVENT_EOF,
			  "a -EPIPE from the source surfaced as a clean EOF");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_ERROR,
		      "a mid-stream failure must raise an ERROR, got type %d", (int)event.type);
	zassert_equal(event.err, -EIO, "the reserved -EPIPE must arrive remapped to -EIO, got %d",
		      event.err);
	zassert_false(audio_pipeline_is_playing(&neg_epipe_pipeline),
		      "still playing after an error");

	zassert_equal(audio_pipeline_join(&neg_epipe_pipeline), 0, "join failed");
}

ZTEST(audio_pipeline_negative, test_sink_epipe_is_not_an_eof)
{
	struct audio_pipeline_event event;

	/* The last boundary: the sink itself fails with the reserved code. No
	 * pull sits above it, so the pipeline has to hold the invariant.
	 */
	audio_fake_source_reset(&neg_sink_epipe_source_state);
	audio_fake_sink_reset(&neg_sink_epipe_sink_state);
	neg_sink_epipe_source_state.frames_total = AUDIO_FAKE_ENDLESS;
	neg_sink_epipe_sink_state.fail_at_frame = 2U; /* one good frame, then fail */
	neg_sink_epipe_sink_state.process_ret = -EPIPE;

	zassert_equal(audio_pipeline_init(&neg_sink_epipe_pipeline, &neg_config,
					  &neg_sink_epipe_sink),
		      0, "init failed");
	zassert_equal(audio_pipeline_start(&neg_sink_epipe_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&neg_sink_epipe_pipeline), 0, "play failed");

	zassert_equal(audio_pipeline_get_event(&neg_sink_epipe_pipeline, &event, K_SECONDS(2)), 0,
		      "no event within 2 s");
	zassert_not_equal(event.type, AUDIO_PIPELINE_EVENT_EOF,
			  "a -EPIPE from the sink surfaced as a clean EOF");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_ERROR,
		      "a failing sink must raise an ERROR, got type %d", (int)event.type);
	zassert_equal(event.err, -EIO, "the reserved -EPIPE must arrive remapped to -EIO, got %d",
		      event.err);

	zassert_equal(audio_pipeline_join(&neg_sink_epipe_pipeline), 0, "join failed");
}

/* -------------------------------------------------------------------------
 * Writer -EFBIG guard: a data chunk that would exceed the 32 bit size field.
 *
 * White-box: writing four more gigabytes is not possible on a RAM disk, so the
 * accumulated byte count is pushed to the ceiling by hand and a single frame is
 * then enough to trip the guard. It exercises exactly the branch in
 * file_writer_process() that no organic run can reach.
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_negative, test_sink_rejects_data_chunk_overflow)
{
	int32_t buf[NEG_FRAME_SAMPLES];
	struct audio_buffer_view view = {
		.data = buf,
		.capacity = ARRAY_SIZE(buf),
	};
	struct audio_file_writer_state *state = neg_efbig_writer.state;
	size_t produced = 1;
	int ret;

	audio_fake_source_reset(&neg_efbig_source_state);
	neg_efbig_source_state.frames_total = AUDIO_FAKE_ENDLESS;
	neg_efbig_source_state.chunk = 2U; /* one stereo frame per call */

	zassert_equal(audio_node_open(&neg_efbig_writer), 0, "open failed");

	/* Two bytes of headroom below the 32 bit ceiling: the RIFF overhead is 36
	 * bytes, so the guard fires when data_bytes + frame_bytes would pass
	 * UINT32_MAX - 36. One stereo frame is four bytes, comfortably over two.
	 */
	state->data_bytes = UINT32_MAX - 36U - 2U;

	ret = audio_node_process(&neg_efbig_writer, &view, &produced);
	zassert_equal(ret, -EFBIG, "a data chunk past the 32 bit field must be rejected, got %d",
		      ret);
	zassert_not_equal(ret, -EPIPE, "a sink must never return -EPIPE; that means EOF");
	zassert_equal(produced, 0U, "a failing process() must not claim samples");
	zassert_equal(state->data_bytes, UINT32_MAX - 36U - 2U,
		      "the rejected frame must not have been counted");

	/* Reset before close() so finalisation does not try to stamp a bogus
	 * multi-gigabyte size into the header.
	 */
	state->data_bytes = 0;
	zassert_equal(audio_node_close(&neg_efbig_writer), 0, "close failed");
}

/*
 * The writer's -ENOSPC short-write branch is intentionally not exercised here.
 * The only way to reach it is a genuinely full filesystem, and filling the ext2
 * RAM disk faults Zephyr's ext2 driver: a native_sim run that fills the disk and
 * then writes crashes with SIGSEGV (twister rc=-11) inside the filesystem layer,
 * not in this subsystem. There is no white-box seam to force a short fs_write()
 * the way the -EFBIG guard can be forced through data_bytes. See the ticket
 * report: -ENOSPC remains unverified for that reason.
 */

ZTEST_SUITE(audio_pipeline_negative, NULL, NULL, negative_before, NULL, NULL);
