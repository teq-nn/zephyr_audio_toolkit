/*
 * Roundtrip test suite (spec §12.2): a golden WAV file on a RAM filesystem is
 * pushed through file_reader -> [optional gain filter] -> file_writer until the
 * pipeline reports EOF, then the output file is compared against the golden
 * master. A unity-gain run must reproduce the master byte for byte; a
 * non-unity-gain run must produce a file of the same shape whose samples are
 * demonstrably transformed, which is what stops a silent pass-through pipeline
 * from faking the roundtrip.
 *
 * Everything runs headless on native_sim, without audio hardware (spec §12.1).
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
#include "wav_parser.h"

#define RT_FRAME_SAMPLES 16

/* Half gain in Q15: sample * 16384 >> 15 == sample / 2. Chosen because a 16 bit
 * roundtrip through it collapses to an arithmetic sample >> 1 for every input
 * value (worked out in the header comment of test_roundtrip_gain_transforms),
 * so the expectations can be written independently of the filter internals.
 */
#define RT_HALF_GAIN_Q15 16384

/*
 * 45 stereo frames = 90 interleaved samples. A whole number of stereo frames,
 * so nothing is dropped and the file lengths can match exactly, but not a
 * multiple of the 16-sample pipeline frame (8 stereo frames), so the last
 * pipeline frame is partial - the case a writer that trusts the frame length
 * gets wrong.
 */
#define RT_STEREO_FRAMES 45U
#define RT_SAMPLE_COUNT (RT_STEREO_FRAMES * 2U)
#define RT_PAYLOAD_BYTES (RT_SAMPLE_COUNT * sizeof(int16_t))
#define RT_FILE_BYTES (WAV_PARSER_MIN_HEADER_SIZE + RT_PAYLOAD_BYTES)

/* file_reader source -> file_writer sink: the plain roundtrip. */
AUDIO_FILE_READER_NODE_DEFINE(rt_reader, AUDIO_TEST_PATH("rt_golden.wav"));
AUDIO_FILE_WRITER_NODE_DEFINE(rt_writer, &rt_reader, AUDIO_TEST_PATH("rt_out.wav"));
AUDIO_PIPELINE_DEFINE(rt_pipeline, RT_FRAME_SAMPLES,
		      CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE, CONFIG_AUDIO_PIPELINE_THREAD_PRIO);

/* file_reader -> half-gain filter -> file_writer: the transform guard. */
AUDIO_FILE_READER_NODE_DEFINE(rt_gain_reader, AUDIO_TEST_PATH("rt_golden.wav"));
AUDIO_GAIN_FILTER_NODE_DEFINE(rt_gain_filter, &rt_gain_reader, RT_HALF_GAIN_Q15);
AUDIO_FILE_WRITER_NODE_DEFINE(rt_gain_writer, &rt_gain_filter, AUDIO_TEST_PATH("rt_gain.wav"));
AUDIO_PIPELINE_DEFINE(rt_gain_pipeline, RT_FRAME_SAMPLES,
		      CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE, CONFIG_AUDIO_PIPELINE_THREAD_PRIO);

static const struct audio_pipeline_config rt_config = {
	.stream = {
		.sample_rate_hz = 48000U,
		.channels = 2U,
		.valid_bits_per_sample = 16U,
		.format = AUDIO_SAMPLE_FORMAT_S32_LE,
	},
	.frame_samples = RT_FRAME_SAMPLES,
	.event_cb = NULL,
	.event_user_data = NULL,
};

/* The golden payload, generated once per test. Interesting corners live at the
 * front so the extremes are covered even though the tail is a plain ramp.
 */
static int16_t golden_samples[RT_SAMPLE_COUNT];

static uint8_t golden_buf[RT_FILE_BYTES + 32];
static uint8_t out_buf[RT_FILE_BYTES + 32];

static void build_golden_samples(void)
{
	static const int16_t corners[] = {
		0, -1, 1, 32767, -32768, 5, -5, 0x1234, -0x1234, 256,
	};
	size_t i;

	BUILD_ASSERT(ARRAY_SIZE(corners) < RT_SAMPLE_COUNT);

	for (i = 0; i < ARRAY_SIZE(corners); i++) {
		golden_samples[i] = corners[i];
	}
	for (; i < RT_SAMPLE_COUNT; i++) {
		/* A deterministic but wide-ranging ramp, biased negative so both
		 * signs are well represented in the tail as well.
		 */
		golden_samples[i] = (int16_t)((int)(i * 613) - 20000);
	}
}

/** Read @p path in full into @p buf; fails the test if it does not fit. */
static size_t read_file(const char *path, uint8_t *buf, size_t cap)
{
	struct fs_file_t file;
	ssize_t read;
	int ret;

	memset(buf, 0, cap);
	fs_file_t_init(&file);

	ret = fs_open(&file, path, FS_O_READ);
	zassert_equal(ret, 0, "%s: could not be opened for read back (%d)", path, ret);

	read = fs_read(&file, buf, cap);
	zassert_true(read >= 0, "%s: read back failed (%d)", path, (int)read);
	zassert_true((size_t)read < cap, "%s: file does not fit the read buffer", path);

	zassert_equal(fs_close(&file), 0, "%s: close after read back failed", path);

	return (size_t)read;
}

/* Drive a pipeline from a golden source until it reports a clean EOF, then join
 * so the sink's output file is finalised. Fails the test on any other outcome.
 */
static void run_to_eof(struct audio_pipeline *pipeline, struct audio_node *sink)
{
	struct audio_pipeline_event event;

	zassert_equal(audio_pipeline_init(pipeline, &rt_config, sink), 0, "init failed");
	zassert_equal(audio_pipeline_start(pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(pipeline), 0, "play failed");

	zassert_equal(audio_pipeline_get_event(pipeline, &event, K_SECONDS(2)), 0,
		      "no event within 2 s");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_EOF,
		      "expected a clean EOF, got type %d err %d", (int)event.type, event.err);
	zassert_equal(event.err, 0, "the EOF event carries an error");

	/* No ERROR must sneak in behind the EOF. */
	zassert_equal(audio_pipeline_get_event(pipeline, &event, K_NO_WAIT), -ENOMSG,
		      "a second event followed the EOF");

	zassert_equal(audio_pipeline_join(pipeline), 0, "join failed");
}

static void roundtrip_before(void *fixture)
{
	ARG_UNUSED(fixture);

	zassert_equal(audio_test_fs_mount(), 0, "fixture filesystem did not mount");

	build_golden_samples();
	zassert_equal(audio_test_write_wav_s16(AUDIO_TEST_PATH("rt_golden.wav"), golden_samples,
					       RT_SAMPLE_COUNT),
		      0, "could not write the golden master");
}

/* -------------------------------------------------------------------------
 * spec §12.2: byte-for-byte roundtrip
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_roundtrip, test_roundtrip_reproduces_golden_master)
{
	size_t golden_len;
	size_t out_len;

	run_to_eof(&rt_pipeline, &rt_writer);

	golden_len = read_file(AUDIO_TEST_PATH("rt_golden.wav"), golden_buf, sizeof(golden_buf));
	out_len = read_file(AUDIO_TEST_PATH("rt_out.wav"), out_buf, sizeof(out_buf));

	/* The whole point of the ticket: identical size, then identical bytes. */
	zassert_equal(golden_len, RT_FILE_BYTES, "golden master is %zu bytes, expected %u",
		      golden_len, (unsigned int)RT_FILE_BYTES);
	zassert_equal(out_len, golden_len, "output is %zu bytes, golden is %zu", out_len,
		      golden_len);
	zassert_mem_equal(out_buf, golden_buf, golden_len,
			  "reader -> unity -> writer is not byte-for-byte identical");
}

/* -------------------------------------------------------------------------
 * ticket AC 2: a non-unity gain must actually transform the samples
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_roundtrip, test_roundtrip_gain_transforms_samples)
{
	struct wav_parser_result wav;
	size_t golden_len;
	size_t out_len;
	size_t i;

	run_to_eof(&rt_gain_pipeline, &rt_gain_writer);

	golden_len = read_file(AUDIO_TEST_PATH("rt_golden.wav"), golden_buf, sizeof(golden_buf));
	out_len = read_file(AUDIO_TEST_PATH("rt_gain.wav"), out_buf, sizeof(out_buf));

	/* Same shape as the master: a valid PCM WAV of identical length. */
	zassert_equal(out_len, golden_len, "half-gain output changed the file length");
	zassert_equal(wav_parser_read_header(out_buf, out_len, &wav), 0,
		      "half-gain output is not a parsable WAV");
	zassert_equal(wav.channels, 2U, "channel count changed");
	zassert_equal(wav.sample_rate_hz, 48000U, "sample rate changed");
	zassert_equal(wav.data_size, RT_PAYLOAD_BYTES, "payload length changed");

	/* Different content: this is the guard against a pass-through pipeline
	 * quietly reproducing the master and calling it a roundtrip. The golden
	 * payload holds non-zero samples, so halving them must change some bytes.
	 */
	zassert_true(memcmp(out_buf, golden_buf, out_len) != 0,
		     "half gain produced a byte-identical copy of the master");

	/*
	 * And the exact transform. For gain 0.5 the whole chain collapses to an
	 * arithmetic right shift by one:
	 *
	 *   widen:    c   = (int32_t)s16 << 16
	 *   gain:     c'  = (c * 16384) >> 15 == c >> 1  (c's low 16 bits are 0,
	 *                                                 so no rounding is lost)
	 *   narrow:   s16'= (uint32_t)c' >> 16
	 *
	 * which is exactly s16 >> 1 for every value, including the awkward ones
	 * (-1 -> -1, 32767 -> 16383, -32768 -> -16384). Written from the spec's
	 * conversion rules, not copied from the gain node.
	 */
	for (i = 0; i < RT_SAMPLE_COUNT; i++) {
		uint16_t got = sys_get_le16(&out_buf[WAV_PARSER_MIN_HEADER_SIZE +
						     i * sizeof(int16_t)]);
		uint16_t expect = (uint16_t)(int16_t)(golden_samples[i] >> 1);

		zassert_equal(got, expect,
			      "sample %zu: %d halved came back as 0x%04x, expected 0x%04x", i,
			      golden_samples[i], got, expect);
	}
}

ZTEST_SUITE(audio_pipeline_roundtrip, NULL, NULL, roundtrip_before, NULL, NULL);
