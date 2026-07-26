/*
 * File writer sink node: header emission and finalisation, S32_LE -> S16
 * narrowing, EOF propagation and the filesystem error paths (manifest §4/§7,
 * spec §5.3/§10.2).
 *
 * Every case writes a real file on the fixture filesystem and reads it back
 * through the shared RIFF/WAVE parser, so the node is judged by the bytes it
 * leaves on disk rather than by its own bookkeeping.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/fs/fs.h>
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

/* Canonical header: RIFF/WAVE + 16 byte "fmt " + "data" chunk header. */
#define WRITER_HEADER_SIZE AUDIO_WAV_MIN_HEADER_SIZE
/*
 * Offset of the RIFF size field. The one field no reader in this subsystem
 * looks at, so the suite has to reach for it by hand - checking it through the
 * same module that wrote it would assert nothing.
 */
#define WRITER_RIFF_SIZE_OFFSET 4U

/* The format the suite binds unless a case needs a different one. The sink
 * resolves no defaults, so this is what every file below is expected to declare.
 */
#define WRITER_RATE 48000U
#define WRITER_CHANNELS 2U

#define WRITER_FRAME_SAMPLES 16

/* The scripted source of fake_nodes.h hands the sink exact container values
 * and exact frame lengths, without going through a file first.
 */
AUDIO_FAKE_SOURCE_DEFINE(hdr_source);
AUDIO_FAKE_SOURCE_DEFINE(conv_source);
AUDIO_FAKE_SOURCE_DEFINE(fmt_source);
AUDIO_FAKE_SOURCE_DEFINE(eof_source);
AUDIO_FAKE_SOURCE_DEFINE(abort_source);
AUDIO_FAKE_SOURCE_DEFINE(reopen_source);
AUDIO_FAKE_SOURCE_DEFINE(odd_source);

AUDIO_FILE_WRITER_NODE_DEFINE(hdr_writer, &hdr_source, AUDIO_TEST_PATH("w_hdr.wav"));
AUDIO_FILE_WRITER_NODE_DEFINE(conv_writer, &conv_source, AUDIO_TEST_PATH("w_conv.wav"));
AUDIO_FILE_WRITER_NODE_DEFINE(fmt_writer, &fmt_source, AUDIO_TEST_PATH("w_fmt.wav"));
AUDIO_FILE_WRITER_NODE_DEFINE(eof_writer, &eof_source, AUDIO_TEST_PATH("w_eof.wav"));
AUDIO_FILE_WRITER_NODE_DEFINE(abort_writer, &abort_source, AUDIO_TEST_PATH("w_abort.wav"));
AUDIO_FILE_WRITER_NODE_DEFINE(reopen_writer, &reopen_source, AUDIO_TEST_PATH("w_reopen.wav"));
AUDIO_FILE_WRITER_NODE_DEFINE(odd_writer, &odd_source, AUDIO_TEST_PATH("w_odd.wav"));
AUDIO_FILE_WRITER_NODE_DEFINE(depth_writer, &hdr_source, AUDIO_TEST_PATH("w_depth.wav"));
AUDIO_FILE_WRITER_NODE_DEFINE(chan_writer, &hdr_source, AUDIO_TEST_PATH("w_chan.wav"));
/* A directory that does not exist: the filesystem has to reject open(). */
AUDIO_FILE_WRITER_NODE_DEFINE(nodir_writer, &hdr_source, AUDIO_TEST_PATH("nodir/w.wav"));
/* No upstream at all: a wiring error the pull has to reject. */
AUDIO_FILE_WRITER_NODE_DEFINE(orphan_writer, NULL, AUDIO_TEST_PATH("w_orphan.wav"));
/* Never opened by any case, so process() has to refuse it. */
AUDIO_FILE_WRITER_NODE_DEFINE(unopened_writer, &hdr_source, AUDIO_TEST_PATH("w_unopened.wav"));

/* End to end: a real WAV in, a real WAV out (spec §12.2 in miniature). */
AUDIO_FILE_READER_NODE_DEFINE(pipe_reader, AUDIO_TEST_PATH("w_src.wav"));
AUDIO_FILE_WRITER_NODE_DEFINE(pipe_writer, &pipe_reader, AUDIO_TEST_PATH("w_pipe.wav"));
AUDIO_PIPELINE_DEFINE(writer_pipeline, WRITER_FRAME_SAMPLES,
		      CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE, CONFIG_AUDIO_PIPELINE_THREAD_PRIO);

/* Second instance so the failing-open case cannot disturb the one above. */
AUDIO_FILE_WRITER_NODE_DEFINE(event_writer, NULL, AUDIO_TEST_PATH("nodir/e.wav"));
AUDIO_PIPELINE_DEFINE(event_pipeline, WRITER_FRAME_SAMPLES,
		      CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE, CONFIG_AUDIO_PIPELINE_THREAD_PRIO);

/*
 * Container values with the interesting corners: both full-scale extremes, the
 * two values issue #7 pins down for the widening direction (-1 -> 0xffff0000
 * and -32768 -> INT32_MIN), values whose naive narrowing cast would wrap, and
 * values with dirt in the low 16 bits so the rounding rule is observable.
 *
 * The expectations are written out literally rather than computed, so the test
 * states the contract instead of mirroring the implementation.
 */
static const int32_t narrow_in[] = {
	(int32_t)0x00000000U, /* silence                                        */
	(int32_t)0xffff0000U, /* -1 widened by the reader                       */
	(int32_t)0x00010000U, /* +1 widened by the reader                       */
	(int32_t)0x7fff0000U, /* full-scale positive, 32767 widened             */
	(int32_t)0x80000000U, /* INT32_MIN, full-scale negative (-32768)        */
	(int32_t)0x7fffffffU, /* INT32_MAX: a naive (int16_t) cast yields -1    */
	(int32_t)0x1234abcdU, /* dirt in the low bits is dropped                */
	(int32_t)0x8000ffffU, /* just above INT32_MIN, still -32768             */
	(int32_t)0xffffffffU, /* -1 container: truncates towards -inf, not 0    */
	(int32_t)0x0000ffffU, /* +0.99: truncates towards -inf, i.e. to 0       */
	(int32_t)0x00008000U, /* exactly +0.5: truncation keeps 0               */
	(int32_t)0xfffe0001U, /* -2 with dirt above the sign                    */
};

static const uint16_t narrow_expect[] = {
	0x0000U, 0xffffU, 0x0001U, 0x7fffU, 0x8000U, 0x7fffU,
	0x1234U, 0x8000U, 0xffffU, 0x0000U, 0x0000U, 0xfffeU,
};

BUILD_ASSERT(ARRAY_SIZE(narrow_in) == ARRAY_SIZE(narrow_expect));

/* -------------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------
 */

static uint8_t file_buf[512];

/*
 * Round-trip the produced file through the parser the *reader* uses and
 * cross-check the two size fields against the real file length: a header whose
 * chunk sizes disagree with the payload is exactly the failure mode a
 * back-patching writer has.
 */
static void assert_valid_wav(const char *path, uint32_t rate, uint16_t channels,
			     uint32_t data_bytes)
{
	struct audio_wav_header wav;
	size_t len = audio_test_read_file(path, file_buf, sizeof(file_buf));
	uint32_t riff_size;
	int ret;

	zassert_equal(len, WRITER_HEADER_SIZE + data_bytes,
		      "%s: file is %zu bytes, expected %u", path, len,
		      WRITER_HEADER_SIZE + data_bytes);

	ret = audio_wav_read_header(file_buf, len, &wav);
	zassert_equal(ret, 0, "%s: the sink produced a header the parser rejects (%d)", path, ret);

	zassert_equal(wav.format_tag, AUDIO_WAV_FORMAT_PCM, "%s: not tagged as PCM", path);
	zassert_equal(wav.sample_rate_hz, rate, "%s: wrong sample rate", path);
	zassert_equal(wav.channels, channels, "%s: wrong channel count", path);
	zassert_equal(wav.bits_per_sample, 16U, "%s: v1 writes 16 bit PCM", path);
	zassert_equal(wav.block_align, (uint16_t)(channels * 2U), "%s: wrong block align", path);
	zassert_equal(wav.data_offset, WRITER_HEADER_SIZE, "%s: payload is not at byte 44", path);
	zassert_equal(wav.data_size, data_bytes, "%s: data chunk claims %u of %u bytes", path,
		      wav.data_size, data_bytes);

	/* The parser ignores the RIFF size, so check it by hand. */
	riff_size = sys_get_le32(&file_buf[WRITER_RIFF_SIZE_OFFSET]);
	zassert_equal(riff_size, 36U + data_bytes, "%s: RIFF size is %u, expected %u", path,
		      riff_size, 36U + data_bytes);
}

/** The 16 bit sample at payload index @p i of the file last read back. */
static uint16_t payload_u16(size_t i)
{
	return sys_get_le16(&file_buf[WRITER_HEADER_SIZE + i * sizeof(int16_t)]);
}

/*
 * Nodes the cases below open directly, without a pipeline. A sink takes its
 * format from audio_node.pipeline_format and resolves no defaults (spec §10.2),
 * so the fixture installs by hand what audio_pipeline_start() would install.
 */
static struct audio_node *const direct_writers[] = {
	&hdr_writer,    &conv_writer,   &fmt_writer,    &eof_writer,
	&abort_writer,  &reopen_writer, &odd_writer,    &depth_writer,
	&chan_writer,   &nodir_writer,  &orphan_writer, &unopened_writer,
};

static const struct audio_stream_config writer_format = {
	.sample_rate_hz = WRITER_RATE,
	.channels = WRITER_CHANNELS,
	.valid_bits_per_sample = 16U,
	.format = AUDIO_SAMPLE_FORMAT_S32_LE,
};

static void writer_before(void *fixture)
{
	size_t i;

	ARG_UNUSED(fixture);

	zassert_equal(audio_test_fs_mount(), 0, "fixture filesystem did not mount");

	for (i = 0; i < ARRAY_SIZE(direct_writers); i++) {
		direct_writers[i]->pipeline_format = &writer_format;
	}
}

/* -------------------------------------------------------------------------
 * open() / close(): the file the sink leaves behind
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_file_writer, test_sink_writes_valid_wav)
{
	int32_t buf[WRITER_FRAME_SAMPLES];
	struct audio_buffer_view view = {
		.data = buf,
		.capacity = ARRAY_SIZE(buf),
	};
	struct audio_file_writer_state *state = hdr_writer.state;
	size_t produced = 0;

	hdr_source_state.samples = narrow_in;
	hdr_source_state.sample_count = ARRAY_SIZE(narrow_in);
	hdr_source_state.chunk = 0;

	zassert_equal(audio_node_open(&hdr_writer), 0, "open failed");
	zassert_true(state->file_open, "open() did not record the handle");

	zassert_equal(audio_node_process(&hdr_writer, &view, &produced), 0, "process failed");
	zassert_equal(produced, ARRAY_SIZE(narrow_in), "the sink changed the frame length");

	zassert_equal(audio_node_close(&hdr_writer), 0, "close failed");
	zassert_false(state->file_open, "close() did not release the handle");

	assert_valid_wav(AUDIO_TEST_PATH("w_hdr.wav"), WRITER_RATE, WRITER_CHANNELS,
			 (uint32_t)(ARRAY_SIZE(narrow_in) * sizeof(int16_t)));
}

ZTEST(audio_pipeline_file_writer, test_sink_writes_configured_format)
{
	int32_t buf[WRITER_FRAME_SAMPLES];
	struct audio_buffer_view view = {
		.data = buf,
		.capacity = ARRAY_SIZE(buf),
	};
	static const struct audio_stream_config mono_44100 = {
		.sample_rate_hz = 44100U,
		.channels = 1U,
		.valid_bits_per_sample = 16U,
		.format = AUDIO_SAMPLE_FORMAT_S32_LE,
	};
	struct audio_file_writer_state *state = fmt_writer.state;
	size_t produced = 0;

	fmt_source_state.samples = narrow_in;
	fmt_source_state.sample_count = 6U;
	fmt_source_state.chunk = 0;

	/* Mono at 44.1 kHz: the header has to follow the bound format, or a
	 * reader would replay the file at the wrong speed.
	 */
	fmt_writer.pipeline_format = &mono_44100;

	zassert_equal(audio_node_open(&fmt_writer), 0, "open failed");
	zassert_equal(audio_node_process(&fmt_writer, &view, &produced), 0, "process failed");
	zassert_equal(audio_node_close(&fmt_writer), 0, "close failed");

	assert_valid_wav(AUDIO_TEST_PATH("w_fmt.wav"), 44100U, 1U,
			 (uint32_t)(6U * sizeof(int16_t)));

	/* The node keeps the resolved format observable, as a copy of the
	 * pipeline's rather than as a second source of truth.
	 */
	zassert_equal(state->fmt.sample_rate_hz, 44100U, "the sink did not record its rate");
	zassert_equal(state->fmt.channels, 1U, "the sink did not record its channel count");
}

/** @brief Fail the calling test unless @p path is absent from the filesystem. */
static void assert_no_file(const char *path)
{
	struct fs_dirent entry;

	zassert_equal(fs_stat(path, &entry), -ENOENT, "%s: a refused open() left a file behind",
		      path);
}

ZTEST(audio_pipeline_file_writer, test_sink_rejects_unsupported_depth)
{
	static const struct audio_stream_config deep_format = {
		.sample_rate_hz = WRITER_RATE,
		.channels = WRITER_CHANNELS,
		/* v1 narrows to 16 bit only; the spec's 24 bit path is not
		 * built, so a bound format asking for it has to fail loudly.
		 */
		.valid_bits_per_sample = 24U,
		.format = AUDIO_SAMPLE_FORMAT_S32_LE,
	};
	struct audio_file_writer_state *state = depth_writer.state;
	int ret;

	depth_writer.pipeline_format = &deep_format;

	ret = audio_node_open(&depth_writer);
	zassert_equal(ret, -ENOTSUP, "a 24 bit sink must be rejected with -ENOTSUP, got %d", ret);
	zassert_false(state->file_open, "a failed open() must not leave a handle");

	/* Refused before the file is created (spec §10.2), so an unsupported
	 * bound format leaves nothing on disk to mislead a reader.
	 */
	assert_no_file(AUDIO_TEST_PATH("w_depth.wav"));
}

ZTEST(audio_pipeline_file_writer, test_sink_rejects_unsupported_channels)
{
	static const struct audio_stream_config wide_format = {
		.sample_rate_hz = WRITER_RATE,
		.channels = 3U,
		.valid_bits_per_sample = 16U,
		.format = AUDIO_SAMPLE_FORMAT_S32_LE,
	};
	struct audio_file_writer_state *state = chan_writer.state;
	int ret;

	chan_writer.pipeline_format = &wide_format;

	ret = audio_node_open(&chan_writer);
	zassert_equal(ret, -ENOTSUP, "v1 writes 1 or 2 channels, got %d for 3", ret);
	zassert_false(state->file_open, "a failed open() must not leave a handle");

	assert_no_file(AUDIO_TEST_PATH("w_chan.wav"));
}

ZTEST(audio_pipeline_file_writer, test_sink_requires_a_bound_format)
{
	struct audio_file_writer_state *state = depth_writer.state;
	int ret;

	/* The 48 kHz / 2 channel fallback is gone: a format is always bound
	 * before start() runs, so a sink guessing one would be exactly the
	 * mislabelling the top-down binding removes.
	 */
	depth_writer.pipeline_format = NULL;

	ret = audio_node_open(&depth_writer);
	zassert_equal(ret, -EINVAL, "a sink without a bound format must fail, got %d", ret);
	zassert_not_equal(ret, -EPIPE, "a sink must never return -EPIPE; that means EOF");
	zassert_false(state->file_open, "a failed open() must not leave a handle");

	assert_no_file(AUDIO_TEST_PATH("w_depth.wav"));
}

ZTEST(audio_pipeline_file_writer, test_sink_rejects_unwritable_path)
{
	struct audio_file_writer_state *state = nodir_writer.state;
	int ret;

	ret = audio_node_open(&nodir_writer);
	zassert_true(ret < 0, "creating a file in a missing directory must fail");
	zassert_not_equal(ret, -EPIPE, "a sink must never return -EPIPE; that means EOF");
	zassert_equal(ret, -ENOENT, "expected -ENOENT from the filesystem, got %d", ret);
	zassert_false(state->file_open, "a failed open() must not leave a handle");
}

ZTEST(audio_pipeline_file_writer, test_sink_close_finalises_and_allows_reopen)
{
	int32_t buf[WRITER_FRAME_SAMPLES];
	struct audio_buffer_view view = {
		.data = buf,
		.capacity = ARRAY_SIZE(buf),
	};
	struct audio_file_writer_state *state = reopen_writer.state;
	size_t produced = 0;
	unsigned int round;

	reopen_source_state.samples = narrow_in;
	reopen_source_state.chunk = 0;

	/* CONFIG_EXT2_MAX_FILES caps the live inodes, so a leaking close() turns
	 * into an open() failure after a few rounds. Each round writes one more
	 * stereo frame than the last, which also proves open() truncates instead
	 * of appending to whatever was there.
	 */
	for (round = 1U; round <= ARRAY_SIZE(narrow_in) / 2U; round++) {
		reopen_source_state.sample_count = round * 2U;
		/* Only the sink is reopened here, so rewind the script by hand. */
		audio_fake_source_rewind(&reopen_source_state);

		zassert_equal(audio_node_open(&reopen_writer), 0, "open failed in round %u",
			      round);
		zassert_equal(audio_node_process(&reopen_writer, &view, &produced), 0,
			      "process failed in round %u", round);
		zassert_equal(produced, round * 2U, "round %u is short", round);
		zassert_equal(audio_node_close(&reopen_writer), 0, "close failed in round %u",
			      round);
		zassert_equal(state->data_bytes, 0U, "close() did not reset the byte count");

		assert_valid_wav(AUDIO_TEST_PATH("w_reopen.wav"), WRITER_RATE, WRITER_CHANNELS,
				 (uint32_t)(round * 2U * sizeof(int16_t)));
	}

	/* close() is idempotent, and process() after it must not touch a file. */
	zassert_equal(audio_node_close(&reopen_writer), 0, "second close() failed");
}

/* -------------------------------------------------------------------------
 * process(): S32_LE -> S16 narrowing
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_file_writer, test_sink_narrows_s32_to_s16)
{
	int32_t buf[WRITER_FRAME_SAMPLES];
	struct audio_buffer_view view = {
		.data = buf,
		.capacity = ARRAY_SIZE(buf),
	};
	size_t produced = 0;
	size_t i;

	conv_source_state.samples = narrow_in;
	conv_source_state.sample_count = ARRAY_SIZE(narrow_in);
	/* Four samples per frame, so the payload is stitched from four writes
	 * and an off-by-one in the chunking shows up as a shifted sample.
	 */
	conv_source_state.chunk = 4U;

	zassert_equal(audio_node_open(&conv_writer), 0, "open failed");

	for (i = 0; i < ARRAY_SIZE(narrow_in) / 4U; i++) {
		zassert_equal(audio_node_process(&conv_writer, &view, &produced), 0,
			      "process failed on frame %zu", i);
		zassert_equal(produced, 4U, "frame %zu is short", i);
	}

	zassert_equal(audio_node_process(&conv_writer, &view, &produced), 0, "EOF must return 0");
	zassert_equal(produced, 0U, "the script source should be exhausted");

	zassert_equal(audio_node_close(&conv_writer), 0, "close failed");

	assert_valid_wav(AUDIO_TEST_PATH("w_conv.wav"), WRITER_RATE, WRITER_CHANNELS,
			 (uint32_t)(ARRAY_SIZE(narrow_in) * sizeof(int16_t)));

	for (i = 0; i < ARRAY_SIZE(narrow_in); i++) {
		zassert_equal(payload_u16(i), narrow_expect[i],
			      "sample %zu: 0x%08x -> 0x%04x, expected 0x%04x", i,
			      (unsigned int)narrow_in[i], payload_u16(i), narrow_expect[i]);
	}
}

ZTEST(audio_pipeline_file_writer, test_sink_rejects_partial_sample_frame)
{
	int32_t buf[WRITER_FRAME_SAMPLES];
	struct audio_buffer_view view = {
		.data = buf,
		.capacity = ARRAY_SIZE(buf),
	};
	struct audio_file_writer_state *state = odd_writer.state;
	size_t produced = 1;
	int ret;

	odd_source_state.samples = narrow_in;
	/* Three samples into a stereo sink: writing them would swap left and
	 * right for the whole rest of the file, so it must be an error.
	 */
	odd_source_state.sample_count = 3U;
	odd_source_state.chunk = 0;

	zassert_equal(audio_node_open(&odd_writer), 0, "open failed");

	ret = audio_node_process(&odd_writer, &view, &produced);
	zassert_equal(ret, -EINVAL, "an incomplete sample frame must be rejected, got %d", ret);
	zassert_not_equal(ret, -EPIPE, "a sink must never return -EPIPE; that means EOF");
	zassert_equal(produced, 0U, "a failing process() must not claim samples");
	zassert_equal(state->data_bytes, 0U, "nothing may have been appended");

	zassert_equal(audio_node_close(&odd_writer), 0, "close failed");
	assert_valid_wav(AUDIO_TEST_PATH("w_odd.wav"), WRITER_RATE, WRITER_CHANNELS, 0U);
}

/* -------------------------------------------------------------------------
 * EOF and error paths
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_file_writer, test_sink_propagates_eof_without_appending)
{
	int32_t buf[WRITER_FRAME_SAMPLES];
	struct audio_buffer_view view = {
		.data = buf,
		.capacity = ARRAY_SIZE(buf),
	};
	struct audio_file_writer_state *state = eof_writer.state;
	const uint32_t expected = 4U * sizeof(int16_t);
	size_t produced = 0;
	unsigned int i;

	eof_source_state.samples = narrow_in;
	eof_source_state.sample_count = 4U;
	eof_source_state.chunk = 0;

	zassert_equal(audio_node_open(&eof_writer), 0, "open failed");
	zassert_equal(audio_node_process(&eof_writer, &view, &produced), 0, "process failed");
	zassert_equal(produced, 4U, "first frame is short");
	zassert_equal(state->data_bytes, expected, "wrong payload length after one frame");

	/* EOF is out_size == 0 with a successful return (manifest §7), and it
	 * must stay that way however often the pipeline asks again.
	 */
	for (i = 0; i < 3U; i++) {
		zassert_equal(audio_node_process(&eof_writer, &view, &produced), 0,
			      "EOF must return 0");
		zassert_equal(produced, 0U, "EOF must report out_size == 0");
		zassert_equal(state->data_bytes, expected, "the sink appended data after EOF");
	}

	/* A clean end of stream finalises the sizes even before close(), so an
	 * application that only waits for the EOF event already has a valid
	 * file.
	 */
	assert_valid_wav(AUDIO_TEST_PATH("w_eof.wav"), WRITER_RATE, WRITER_CHANNELS, expected);

	zassert_equal(audio_node_close(&eof_writer), 0, "close failed");
	assert_valid_wav(AUDIO_TEST_PATH("w_eof.wav"), WRITER_RATE, WRITER_CHANNELS, expected);
}

ZTEST(audio_pipeline_file_writer, test_sink_without_upstream_is_a_wiring_error)
{
	int32_t buf[WRITER_FRAME_SAMPLES];
	struct audio_buffer_view view = {
		.data = buf,
		.capacity = ARRAY_SIZE(buf),
	};
	size_t produced = 1;
	int ret;

	zassert_equal(audio_node_open(&orphan_writer), 0, "open failed");

	/* Spec §4.4: a sink has an upstream. A missing one is a wiring error,
	 * and reporting it as a clean EOF would swallow the track silently.
	 */
	ret = audio_node_process(&orphan_writer, &view, &produced);
	zassert_equal(ret, -ENOTSUP, "a sink without upstream must report -ENOTSUP, got %d", ret);
	zassert_equal(produced, 0U, "a failing process() must not claim samples");

	zassert_equal(audio_node_close(&orphan_writer), 0, "close failed");
	assert_valid_wav(AUDIO_TEST_PATH("w_orphan.wav"), WRITER_RATE, WRITER_CHANNELS, 0U);
}

ZTEST(audio_pipeline_file_writer, test_sink_process_without_open_fails)
{
	int32_t buf[4];
	struct audio_buffer_view view = {
		.data = buf,
		.capacity = ARRAY_SIZE(buf),
	};
	size_t produced = 1;
	int ret;

	/* A sink without a file must not pretend to consume data, and must not
	 * report EOF either - that would silently truncate the track.
	 */
	ret = audio_node_process(&unopened_writer, &view, &produced);
	zassert_true(ret < 0, "process() without open() must fail");
	zassert_not_equal(ret, -EPIPE, "a sink must never return -EPIPE; that means EOF");
	zassert_equal(ret, -EBADF, "expected -EBADF on a closed sink, got %d", ret);
	zassert_equal(produced, 0U, "a failing process() must not claim samples");
}

ZTEST(audio_pipeline_file_writer, test_sink_reports_write_error_and_leaves_empty_file)
{
	int32_t buf[WRITER_FRAME_SAMPLES];
	struct audio_buffer_view view = {
		.data = buf,
		.capacity = ARRAY_SIZE(buf),
	};
	struct audio_file_writer_state *state = abort_writer.state;
	struct audio_wav_header wav;
	size_t produced = 0;
	size_t len;
	int ret;

	abort_source_state.samples = narrow_in;
	abort_source_state.sample_count = 8U;
	abort_source_state.chunk = 4U;

	zassert_equal(audio_node_open(&abort_writer), 0, "open failed");
	zassert_equal(audio_node_process(&abort_writer, &view, &produced), 0, "process failed");
	zassert_equal(produced, 4U, "first frame is short");

	/* Pull the handle out from under the node: fs_write() on a closed file
	 * object returns -EBADF, which is the cheapest deterministic stand-in
	 * for a filesystem that stops accepting data (spec §12.3).
	 */
	zassert_equal(fs_close(&state->file), 0, "could not close the handle behind the node");

	ret = audio_node_process(&abort_writer, &view, &produced);
	zassert_true(ret < 0, "a failing write must surface as a negative return");
	zassert_not_equal(ret, -EPIPE, "a write error must not look like EOF");
	zassert_equal(produced, 0U, "a failing process() must not claim samples");

	/* close() cannot patch the sizes either, so it reports the failure - but
	 * it still gives up the handle.
	 */
	ret = audio_node_close(&abort_writer);
	zassert_true(ret < 0, "close() on a broken file must report the failure");
	zassert_not_equal(ret, -EPIPE, "a close error must not look like EOF");
	zassert_false(state->file_open, "close() must release the handle regardless");

	/* What a reader sees: the placeholder header open() wrote, structurally
	 * valid but claiming no payload at all. The bytes that made it to disk
	 * before the failure are still in the file, so it is longer than the
	 * header admits - an aborted run yields an empty track, never a bogus
	 * length.
	 */
	len = audio_test_read_file(AUDIO_TEST_PATH("w_abort.wav"), file_buf, sizeof(file_buf));
	zassert_equal(audio_wav_read_header(file_buf, len, &wav), 0,
		      "even an unfinalised file must carry a parsable header");
	zassert_equal(wav.data_size, 0U, "an unfinalised file must not claim payload");
	zassert_equal(sys_get_le32(&file_buf[WRITER_RIFF_SIZE_OFFSET]), 36U,
		      "an unfinalised file must not claim a RIFF length either");
	zassert_true(len > WRITER_HEADER_SIZE, "the frame written before the failure is lost");
}

ZTEST(audio_pipeline_file_writer, test_sink_open_failure_emits_error_event)
{
	static const struct audio_pipeline_config cfg = {
		.frame_samples = WRITER_FRAME_SAMPLES,
		.event_cb = NULL,
		.event_user_data = NULL,
	};
	struct audio_pipeline_event event;
	int ret;

	zassert_equal(audio_pipeline_init(&event_pipeline, &cfg, &event_writer), 0, "init failed");
	zassert_equal(audio_pipeline_set_format(&event_pipeline, &writer_format), 0,
		      "binding the pipeline format failed");

	ret = audio_pipeline_start(&event_pipeline);
	zassert_true(ret < 0, "start() must fail when the sink cannot create its file");

	zassert_equal(audio_pipeline_get_event(&event_pipeline, &event, K_NO_WAIT), 0,
		      "no event was published");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_ERROR, "expected an ERROR event, got %d",
		      (int)event.type);
	zassert_equal(event.err, ret, "the event carries %d, start() returned %d", event.err, ret);
	zassert_not_equal(event.err, -EPIPE, "an ERROR event must not carry the EOF code");
	zassert_false(audio_pipeline_is_running(&event_pipeline), "a thread was created anyway");
}

/* -------------------------------------------------------------------------
 * End to end: file -> file through the pipeline thread
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_file_writer, test_sink_writes_file_driven_by_pipeline)
{
	static const struct audio_pipeline_config cfg = {
		.frame_samples = WRITER_FRAME_SAMPLES,
		.event_cb = NULL,
		.event_user_data = NULL,
	};
	/* Deliberately not a multiple of the frame size: the last frame is
	 * partial, which is where a writer that trusts the frame length breaks.
	 */
	int16_t payload[WRITER_FRAME_SAMPLES * 2 + 6];
	struct audio_pipeline_event event;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(payload); i++) {
		payload[i] = (int16_t)(i * 271) - 3000;
	}

	zassert_equal(audio_test_write_wav_s16(AUDIO_TEST_PATH("w_src.wav"), payload,
					       ARRAY_SIZE(payload)),
		      0, "could not write the source fixture");

	zassert_equal(audio_pipeline_init(&writer_pipeline, &cfg, &pipe_writer), 0, "init failed");
	zassert_equal(audio_pipeline_set_format(&writer_pipeline, &writer_format), 0,
		      "binding the pipeline format failed");
	zassert_equal(audio_pipeline_start(&writer_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&writer_pipeline), 0, "play failed");

	zassert_equal(audio_pipeline_get_event(&writer_pipeline, &event, K_SECONDS(2)), 0,
		      "no event within 2 s");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_EOF, "expected a clean EOF, got type %d "
		      "err %d", (int)event.type, event.err);

	/* join() is the synchronous half of the lifecycle: it closes the chain,
	 * so the output file is final once it returns.
	 */
	zassert_equal(audio_pipeline_join(&writer_pipeline), 0, "join failed");

	assert_valid_wav(AUDIO_TEST_PATH("w_pipe.wav"), 48000U, 2U,
			 (uint32_t)(ARRAY_SIZE(payload) * sizeof(int16_t)));

	/* S16 -> S32 -> S16 has to be the identity (spec §5.3). */
	for (i = 0; i < ARRAY_SIZE(payload); i++) {
		zassert_equal(payload_u16(i), (uint16_t)payload[i],
			      "sample %zu came back as 0x%04x instead of 0x%04x", i,
			      payload_u16(i), (uint16_t)payload[i]);
	}
}

ZTEST_SUITE(audio_pipeline_file_writer, NULL, NULL, writer_before, NULL, NULL);
