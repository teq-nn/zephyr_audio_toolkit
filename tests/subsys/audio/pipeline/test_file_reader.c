/*
 * File reader source node: header validation, S16 -> S32_LE conversion,
 * partial final frames and EOF.
 *
 * Every case runs against a real file on the fixture filesystem, so the node
 * is exercised through the Zephyr filesystem API rather than a mock.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <zephyr/audio/audio_node.h>
#include <zephyr/audio/audio_nodes.h>
#include <zephyr/audio/audio_pipeline.h>
#include <zephyr/audio/audio_pipeline_events.h>

#include "wav_fixture.h"

/* One node per fixture file, all defined through the public macro so the test
 * sees exactly what an application sees.
 */
AUDIO_FILE_READER_NODE_DEFINE(pcm_reader, AUDIO_TEST_PATH("pcm.wav"));
AUDIO_FILE_READER_NODE_DEFINE(missing_reader, AUDIO_TEST_PATH("nope.wav"));
AUDIO_FILE_READER_NODE_DEFINE(garbage_reader, AUDIO_TEST_PATH("garbage.wav"));
AUDIO_FILE_READER_NODE_DEFINE(adpcm_reader, AUDIO_TEST_PATH("adpcm.wav"));
AUDIO_FILE_READER_NODE_DEFINE(eight_bit_reader, AUDIO_TEST_PATH("u8.wav"));
AUDIO_FILE_READER_NODE_DEFINE(lying_reader, AUDIO_TEST_PATH("lying.wav"));
AUDIO_FILE_READER_NODE_DEFINE(long_reader, AUDIO_TEST_PATH("long.wav"));
AUDIO_FILE_READER_NODE_DEFINE(chain_reader, AUDIO_TEST_PATH("chain.wav"));

AUDIO_GAIN_FILTER_NODE_DEFINE(chain_gain, &chain_reader, AUDIO_GAIN_UNITY_Q15);
AUDIO_NULL_SINK_NODE_DEFINE(chain_sink, &chain_gain);

#define CHAIN_FRAME_SAMPLES 16

AUDIO_PIPELINE_DEFINE(chain_pipeline, CHAIN_FRAME_SAMPLES,
		      CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE, CONFIG_AUDIO_PIPELINE_THREAD_PRIO);

/*
 * Interesting 16 bit values, deliberately including both signed extremes and
 * a negative mid-scale value: sign extension is where an S16 -> S32 widening
 * goes wrong first.
 */
static const int16_t known_samples[] = {
	0, -1, 1, 32767, -32768, 0x1234, -0x1234, -256, 255, 0x7ffe, -0x7fff, 4,
};

/* The container value is the 16 bit sample shifted up by 16. */
static int32_t expected_s32(int16_t sample)
{
	return (int32_t)((uint32_t)(int32_t)sample << 16);
}

static void reader_before(void *fixture)
{
	ARG_UNUSED(fixture);

	zassert_equal(audio_test_fs_mount(), 0, "fixture filesystem did not mount");
}

/* -------------------------------------------------------------------------
 * open(): header validation
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_file_reader, test_source_rejects_missing_file)
{
	/* -ENOENT comes straight from the filesystem, and crucially it is not
	 * -EPIPE, which the pipeline reads as end of stream.
	 */
	zassert_equal(audio_node_open(&missing_reader), -ENOENT,
		      "opening an absent file must fail with -ENOENT");
}

ZTEST(audio_pipeline_file_reader, test_source_rejects_corrupt_header)
{
	static const uint8_t garbage[64] = {'N', 'O', 'T', 'A', 'W', 'A', 'V'};

	zassert_equal(audio_test_write_raw(AUDIO_TEST_PATH("garbage.wav"), garbage,
					   sizeof(garbage)),
		      0, "could not write the fixture");

	zassert_equal(audio_node_open(&garbage_reader), -EINVAL,
		      "a structurally corrupt header must be rejected");
}

ZTEST(audio_pipeline_file_reader, test_source_rejects_non_pcm)
{
	static const uint8_t payload[8] = {0};
	struct audio_test_wav_spec spec = {
		.format_tag = 2U, /* WAVE_FORMAT_ADPCM */
		.payload = payload,
		.payload_len = sizeof(payload),
	};

	zassert_equal(audio_test_write_wav(AUDIO_TEST_PATH("adpcm.wav"), &spec), 0,
		      "could not write the fixture");

	zassert_equal(audio_node_open(&adpcm_reader), -ENOTSUP,
		      "a non-PCM file must be rejected with -ENOTSUP");
}

ZTEST(audio_pipeline_file_reader, test_source_rejects_unsupported_depth)
{
	static const uint8_t payload[8] = {0};
	struct audio_test_wav_spec spec = {
		.bits_per_sample = 8U,
		.payload = payload,
		.payload_len = sizeof(payload),
	};

	zassert_equal(audio_test_write_wav(AUDIO_TEST_PATH("u8.wav"), &spec), 0,
		      "could not write the fixture");

	zassert_equal(audio_node_open(&eight_bit_reader), -ENOTSUP,
		      "v1 converts 16 bit PCM only");
}

ZTEST(audio_pipeline_file_reader, test_source_publishes_parsed_format)
{
	struct audio_file_reader_state *state = pcm_reader.state;
	struct audio_test_wav_spec spec = {
		.sample_rate_hz = 44100U,
		.payload = known_samples,
		.payload_len = sizeof(known_samples),
	};

	zassert_equal(audio_test_write_wav(AUDIO_TEST_PATH("pcm.wav"), &spec), 0,
		      "could not write the fixture");
	zassert_equal(audio_node_open(&pcm_reader), 0, "open failed");

	zassert_equal(state->fmt.sample_rate_hz, 44100U, "sample rate not taken from the header");
	zassert_equal(state->fmt.channels, 2U, "channel count not taken from the header");
	/* The container is 32 bit, the resolution stays 16. */
	zassert_equal(state->fmt.valid_bits_per_sample, 16U, "valid_bits_per_sample is wrong");
	zassert_equal(state->fmt.format, AUDIO_SAMPLE_FORMAT_S32_LE, "container must be S32_LE");
	zassert_false(state->eof, "a fresh reader must not start at EOF");

	zassert_equal(audio_node_close(&pcm_reader), 0, "close failed");
}

/* -------------------------------------------------------------------------
 * process(): conversion, partial frames, EOF
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_file_reader, test_source_converts_s16_to_s32)
{
	int32_t buf[ARRAY_SIZE(known_samples) + 4];
	struct audio_buffer_view view = {
		.data = buf,
		.capacity = ARRAY_SIZE(buf),
	};
	size_t produced = 0;
	size_t i;

	zassert_equal(audio_test_write_wav_s16(AUDIO_TEST_PATH("pcm.wav"), known_samples,
					       ARRAY_SIZE(known_samples)),
		      0, "could not write the fixture");
	zassert_equal(audio_node_open(&pcm_reader), 0, "open failed");

	memset(buf, 0x5a, sizeof(buf));
	zassert_equal(audio_node_process(&pcm_reader, &view, &produced), 0, "process failed");
	zassert_equal(produced, ARRAY_SIZE(known_samples), "wrong sample count");

	for (i = 0; i < ARRAY_SIZE(known_samples); i++) {
		zassert_equal(buf[i], expected_s32(known_samples[i]),
			      "sample %zu: %d -> 0x%08x, expected 0x%08x", i, known_samples[i],
			      (unsigned int)buf[i], (unsigned int)expected_s32(known_samples[i]));
	}

	/* Beyond out_size the reader must not have touched the buffer. */
	for (i = ARRAY_SIZE(known_samples); i < ARRAY_SIZE(buf); i++) {
		zassert_equal(buf[i], (int32_t)0x5a5a5a5a, "reader wrote past out_size");
	}

	zassert_equal(audio_node_close(&pcm_reader), 0, "close failed");
}

ZTEST(audio_pipeline_file_reader, test_source_reports_eof)
{
	int32_t buf[8];
	struct audio_buffer_view view = {
		.data = buf,
		.capacity = ARRAY_SIZE(buf),
	};
	struct audio_file_reader_state *state = pcm_reader.state;
	size_t produced = 0;

	/* Exactly one full frame of payload, so the frame after it is EOF. */
	zassert_equal(audio_test_write_wav_s16(AUDIO_TEST_PATH("pcm.wav"), known_samples,
					       ARRAY_SIZE(buf)),
		      0, "could not write the fixture");
	zassert_equal(audio_node_open(&pcm_reader), 0, "open failed");

	zassert_equal(audio_node_process(&pcm_reader, &view, &produced), 0, "process failed");
	zassert_equal(produced, ARRAY_SIZE(buf), "first frame is short");

	/* EOF is out_size == 0 with a *successful* return. */
	zassert_equal(audio_node_process(&pcm_reader, &view, &produced), 0, "EOF must return 0");
	zassert_equal(produced, 0U, "EOF must report out_size == 0");
	zassert_true(state->eof, "eof flag not latched");

	/* And it stays at EOF instead of looping around. */
	zassert_equal(audio_node_process(&pcm_reader, &view, &produced), 0, "EOF must return 0");
	zassert_equal(produced, 0U, "reader restarted after EOF");

	zassert_equal(audio_node_close(&pcm_reader), 0, "close failed");
}

ZTEST(audio_pipeline_file_reader, test_source_delivers_partial_final_frame)
{
	int32_t buf[8];
	struct audio_buffer_view view = {
		.data = buf,
		.capacity = ARRAY_SIZE(buf),
	};
	/* 12 samples against a capacity of 8: one full frame, then four. */
	const size_t total = 12;
	size_t produced = 0;
	size_t i;

	zassert_equal(audio_test_write_wav_s16(AUDIO_TEST_PATH("long.wav"), known_samples, total),
		      0, "could not write the fixture");
	zassert_equal(audio_node_open(&long_reader), 0, "open failed");

	zassert_equal(audio_node_process(&long_reader, &view, &produced), 0, "process failed");
	zassert_equal(produced, ARRAY_SIZE(buf), "first frame must be full");
	for (i = 0; i < produced; i++) {
		zassert_equal(buf[i], expected_s32(known_samples[i]), "frame 0 sample %zu", i);
	}

	zassert_equal(audio_node_process(&long_reader, &view, &produced), 0, "process failed");
	zassert_equal(produced, total - ARRAY_SIZE(buf), "partial final frame is wrong");
	for (i = 0; i < produced; i++) {
		zassert_equal(buf[i], expected_s32(known_samples[ARRAY_SIZE(buf) + i]),
			      "frame 1 sample %zu", i);
	}

	zassert_equal(audio_node_process(&long_reader, &view, &produced), 0, "EOF must return 0");
	zassert_equal(produced, 0U, "EOF must report out_size == 0");

	zassert_equal(audio_node_close(&long_reader), 0, "close failed");
}

ZTEST(audio_pipeline_file_reader, test_source_treats_short_read_as_eof)
{
	int32_t buf[16];
	struct audio_buffer_view view = {
		.data = buf,
		.capacity = ARRAY_SIZE(buf),
	};
	struct audio_test_wav_spec spec = {
		/* The header promises 4 KiB, the file carries 8 samples. The
		 * parser does not cross-check this, so the reader must report a
		 * clean EOF rather than an error.
		 */
		.declared_data_size = 4096U,
		.payload = known_samples,
		.payload_len = 8U * sizeof(int16_t),
	};
	size_t produced = 0;

	zassert_equal(audio_test_write_wav(AUDIO_TEST_PATH("lying.wav"), &spec), 0,
		      "could not write the fixture");
	zassert_equal(audio_node_open(&lying_reader), 0, "open failed");

	zassert_equal(audio_node_process(&lying_reader, &view, &produced), 0,
		      "a short read must not be an error");
	zassert_equal(produced, 8U, "wrong sample count from a truncated file");

	zassert_equal(audio_node_process(&lying_reader, &view, &produced), 0, "EOF must return 0");
	zassert_equal(produced, 0U, "truncated payload must end the stream");

	zassert_equal(audio_node_close(&lying_reader), 0, "close failed");
}

/* -------------------------------------------------------------------------
 * close(), and misuse
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_file_reader, test_source_process_without_open_fails)
{
	int32_t buf[4];
	struct audio_buffer_view view = {
		.data = buf,
		.capacity = ARRAY_SIZE(buf),
	};
	size_t produced = 1;
	int ret;

	ret = audio_node_process(&pcm_reader, &view, &produced);
	zassert_true(ret < 0, "process() without open() must fail");
	zassert_not_equal(ret, -EPIPE, "a reader must never return -EPIPE; that means EOF");
	zassert_equal(produced, 0U, "a failing process() must not claim samples");
}

ZTEST(audio_pipeline_file_reader, test_source_rejects_undersized_buffer)
{
	int32_t buf[1];
	struct audio_buffer_view view = {
		.data = buf,
		.capacity = ARRAY_SIZE(buf),
	};
	struct audio_file_reader_state *state = pcm_reader.state;
	size_t produced = 1;

	zassert_equal(audio_test_write_wav_s16(AUDIO_TEST_PATH("pcm.wav"), known_samples,
					       ARRAY_SIZE(known_samples)),
		      0, "could not write the fixture");
	zassert_equal(audio_node_open(&pcm_reader), 0, "open failed");

	/* One sample of room for a stereo file: an error, not EOF - reporting
	 * EOF would truncate the track without anyone noticing.
	 */
	zassert_equal(audio_node_process(&pcm_reader, &view, &produced), -EINVAL,
		      "a buffer smaller than one sample frame must be rejected");
	zassert_equal(produced, 0U, "a failing process() must not claim samples");
	zassert_false(state->eof, "an undersized buffer must not end the stream");

	zassert_equal(audio_node_close(&pcm_reader), 0, "close failed");
}

ZTEST(audio_pipeline_file_reader, test_source_close_releases_handle_and_allows_reopen)
{
	int32_t buf[8];
	struct audio_buffer_view view = {
		.data = buf,
		.capacity = ARRAY_SIZE(buf),
	};
	struct audio_file_reader_state *state = pcm_reader.state;
	size_t produced = 0;
	unsigned int i;

	zassert_equal(audio_test_write_wav_s16(AUDIO_TEST_PATH("pcm.wav"), known_samples,
					       ARRAY_SIZE(known_samples)),
		      0, "could not write the fixture");

	/* CONFIG_EXT2_MAX_FILES caps the number of live inodes, so a leaking
	 * close() shows up as an open() failure after a few rounds.
	 */
	for (i = 0; i < 32U; i++) {
		zassert_equal(audio_node_open(&pcm_reader), 0, "open failed in round %u", i);
		zassert_true(state->file_open, "open() did not record the handle");

		zassert_equal(audio_node_process(&pcm_reader, &view, &produced), 0,
			      "process failed in round %u", i);
		zassert_equal(produced, ARRAY_SIZE(buf), "round %u produced %zu samples", i,
			      produced);
		zassert_equal(buf[0], expected_s32(known_samples[0]),
			      "round %u did not restart at the first sample", i);

		zassert_equal(audio_node_close(&pcm_reader), 0, "close failed in round %u", i);
		zassert_false(state->file_open, "close() did not release the handle");
	}

	zassert_true(audio_node_process(&pcm_reader, &view, &produced) < 0,
		     "process() after close() must fail");
}

/* -------------------------------------------------------------------------
 * End to end: file -> gain -> null sink, EOF event (issue #7 AC 4)
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_file_reader, test_source_drives_pipeline_to_eof_event)
{
	static const struct audio_pipeline_config cfg = {
		.stream = {
			.sample_rate_hz = 48000U,
			.channels = 2U,
			.valid_bits_per_sample = 16U,
			.format = AUDIO_SAMPLE_FORMAT_S32_LE,
		},
		.frame_samples = CHAIN_FRAME_SAMPLES,
		.event_cb = NULL,
		.event_user_data = NULL,
	};
	struct audio_pipeline_event event;
	int16_t payload[CHAIN_FRAME_SAMPLES * 3];
	size_t i;

	for (i = 0; i < ARRAY_SIZE(payload); i++) {
		payload[i] = (int16_t)(i * 137) - 4096;
	}

	zassert_equal(audio_test_write_wav_s16(AUDIO_TEST_PATH("chain.wav"), payload,
					       ARRAY_SIZE(payload)),
		      0, "could not write the fixture");

	zassert_equal(audio_pipeline_init(&chain_pipeline, &cfg, &chain_sink), 0, "init failed");
	zassert_equal(audio_pipeline_start(&chain_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&chain_pipeline), 0, "play failed");

	zassert_equal(audio_pipeline_get_event(&chain_pipeline, &event, K_SECONDS(2)), 0,
		      "no event within 2 s");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_EOF, "expected a clean EOF, got type %d "
		      "err %d", (int)event.type, event.err);
	zassert_equal(event.err, 0, "EOF event carries an error");
	zassert_false(audio_pipeline_is_playing(&chain_pipeline), "still playing after EOF");
	zassert_true(audio_pipeline_is_running(&chain_pipeline), "EOF killed the worker thread");

	zassert_equal(audio_pipeline_join(&chain_pipeline), 0, "join failed");
}

ZTEST_SUITE(audio_pipeline_file_reader, NULL, NULL, reader_before, NULL, NULL);
