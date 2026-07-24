/*
 * Unit tests for the RIFF/WAVE header module.
 *
 * The suite has two halves. The first one is the round trip: everything
 * audio_wav_write_header() emits, audio_wav_read_header() must parse back
 * unchanged - no filesystem, no pipeline, just bytes. The second one is the
 * parser's own error handling, driven by headers the writer deliberately
 * cannot produce: foreign chunks, truncations, missing chunks and degenerate
 * "fmt " fields. Only those still need the little builder DSL below; every
 * header that is merely *valid* now comes out of the writer instead.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <zephyr/audio/audio_wav.h>

/* Canonical 16-bit stereo 44.1 kHz PCM payload used by the happy-path cases. */
#define TEST_SAMPLE_RATE 44100U
#define TEST_CHANNELS	 2U
#define TEST_BITS	 16U
#define TEST_DATA_BYTES	 8U

/*
 * Offsets into the canonical header the writer emits, used only to corrupt it
 * on purpose. Nothing here derives a *valid* header by hand.
 */
#define HDR_OFF_RIFF_MAGIC  0U
#define HDR_OFF_WAVE_MAGIC  8U
#define HDR_OFF_BLOCK_ALIGN 32U

/* -------------------------------------------------------------------------
 * Write -> read round trip
 * ----------------------------------------------------------------------
 */

/** Serialise @p hdr into @p buf, failing the test if the writer refuses. */
static void write_header(uint8_t *buf, size_t cap, const struct audio_wav_header *hdr)
{
	int ret = audio_wav_write_header(buf, cap, hdr);

	zassert_equal(ret, 0, "the writer rejected a header it must accept (%d)", ret);
}

/** Emit the canonical 16 bit stereo header the reader cases start from. */
static void write_valid_header(uint8_t *buf, size_t cap)
{
	const struct audio_wav_header hdr = {
		.sample_rate_hz = TEST_SAMPLE_RATE,
		.data_size = TEST_DATA_BYTES,
		.format_tag = AUDIO_WAV_FORMAT_PCM,
		.channels = TEST_CHANNELS,
		.bits_per_sample = TEST_BITS,
	};

	write_header(buf, cap, &hdr);
}

ZTEST(audio_wav, test_wav_round_trips_every_supported_format)
{
	/* Every bit depth and channel count v1 accepts, plus the edges of the
	 * declared payload size.
	 */
	static const struct audio_wav_header cases[] = {
		{ .sample_rate_hz = TEST_SAMPLE_RATE, .data_size = TEST_DATA_BYTES,
		  .channels = TEST_CHANNELS, .bits_per_sample = TEST_BITS },
		{ .sample_rate_hz = 8000U, .data_size = 0U, .channels = 1U, .bits_per_sample = 8U },
		{ .sample_rate_hz = 44100U, .data_size = 4U, .channels = 2U,
		  .bits_per_sample = 8U },
		{ .sample_rate_hz = 16000U, .data_size = 32U, .channels = 1U,
		  .bits_per_sample = 16U },
		{ .sample_rate_hz = 48000U, .data_size = 1024U, .channels = 2U,
		  .bits_per_sample = 16U },
		{ .sample_rate_hz = 48000U, .data_size = 9U, .channels = 1U,
		  .bits_per_sample = 24U },
		{ .sample_rate_hz = 96000U, .data_size = 48U, .channels = 2U,
		  .bits_per_sample = 32U },
		{ .sample_rate_hz = 192000U, .data_size = AUDIO_WAV_MAX_DATA_SIZE, .channels = 2U,
		  .bits_per_sample = 24U },
	};
	uint8_t buf[AUDIO_WAV_MIN_HEADER_SIZE];
	struct audio_wav_header out;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct audio_wav_header hdr = cases[i];

		hdr.format_tag = AUDIO_WAV_FORMAT_PCM;
		write_header(buf, sizeof(buf), &hdr);

		zassert_equal(audio_wav_read_header(buf, sizeof(buf), &out), 0,
			      "case %zu: the module cannot read its own header", i);
		zassert_equal(out.sample_rate_hz, hdr.sample_rate_hz, "case %zu: wrong rate", i);
		zassert_equal(out.channels, hdr.channels, "case %zu: wrong channel count", i);
		zassert_equal(out.bits_per_sample, hdr.bits_per_sample, "case %zu: wrong depth", i);
		zassert_equal(out.format_tag, AUDIO_WAV_FORMAT_PCM, "case %zu: wrong tag", i);
		zassert_equal(out.data_size, hdr.data_size, "case %zu: wrong data size", i);
		/* The derived pair: the writer computes them, the reader gets
		 * them back out of the bytes.
		 */
		zassert_equal(out.block_align, hdr.channels * (hdr.bits_per_sample / 8U),
			      "case %zu: wrong block align", i);
		zassert_equal(out.data_offset, AUDIO_WAV_MIN_HEADER_SIZE,
			      "case %zu: payload is not at byte 44", i);
	}
}

ZTEST(audio_wav, test_wav_writes_exactly_the_canonical_header)
{
	uint8_t buf[AUDIO_WAV_MIN_HEADER_SIZE + 16U];
	size_t i;

	memset(buf, 0xa5, sizeof(buf));
	write_valid_header(buf, sizeof(buf));

	/* A caller appending its payload right behind the header would lose the
	 * first samples if the module ever wrote past what it promises.
	 */
	for (i = AUDIO_WAV_MIN_HEADER_SIZE; i < sizeof(buf); i++) {
		zassert_equal(buf[i], 0xa5U, "the writer touched byte %zu, past the header", i);
	}
}

ZTEST(audio_wav, test_wav_writes_a_payload_size_it_has_not_seen)
{
	const struct audio_wav_header hdr = {
		.sample_rate_hz = TEST_SAMPLE_RATE,
		/* The header goes out before the payload exists, so the declared
		 * size is the caller's word - here, 4 KiB nobody has written.
		 */
		.data_size = 4096U,
		.format_tag = AUDIO_WAV_FORMAT_PCM,
		.channels = TEST_CHANNELS,
		.bits_per_sample = TEST_BITS,
	};
	uint8_t buf[AUDIO_WAV_MIN_HEADER_SIZE];
	struct audio_wav_header out;

	write_header(buf, sizeof(buf), &hdr);

	zassert_equal(audio_wav_read_header(buf, sizeof(buf), &out), 0,
		      "a header without its payload must still parse");
	zassert_equal(out.data_size, 4096U, "declared data size did not survive");
	zassert_equal(out.data_offset, AUDIO_WAV_MIN_HEADER_SIZE, "wrong data offset");
}

ZTEST(audio_wav, test_wav_writes_a_foreign_format_tag_verbatim)
{
	static const uint16_t tags[] = { 0x0000U, 0x0002U, 0x0003U, 0x0006U, 0xfffeU };
	uint8_t buf[AUDIO_WAV_MIN_HEADER_SIZE];
	struct audio_wav_header out;
	size_t i;

	/* The module serialises the tag it is given; the reader is the one that
	 * draws the line: structurally fine, but not PCM.
	 */
	for (i = 0; i < ARRAY_SIZE(tags); i++) {
		struct audio_wav_header hdr = {
			.sample_rate_hz = TEST_SAMPLE_RATE,
			.data_size = TEST_DATA_BYTES,
			.format_tag = tags[i],
			.channels = TEST_CHANNELS,
			.bits_per_sample = TEST_BITS,
		};

		write_header(buf, sizeof(buf), &hdr);

		zassert_equal(audio_wav_read_header(buf, sizeof(buf), &out), -ENOTSUP,
			      "tag 0x%04x was not reported as unsupported", tags[i]);
	}
}

ZTEST(audio_wav, test_wav_write_rejects_bad_arguments)
{
	const struct audio_wav_header hdr = {
		.sample_rate_hz = TEST_SAMPLE_RATE,
		.data_size = TEST_DATA_BYTES,
		.format_tag = AUDIO_WAV_FORMAT_PCM,
		.channels = TEST_CHANNELS,
		.bits_per_sample = TEST_BITS,
	};
	uint8_t buf[AUDIO_WAV_MIN_HEADER_SIZE];

	zassert_equal(audio_wav_write_header(NULL, sizeof(buf), &hdr), -EINVAL,
		      "NULL buffer accepted");
	zassert_equal(audio_wav_write_header(buf, sizeof(buf), NULL), -EINVAL,
		      "NULL header accepted");
	zassert_equal(audio_wav_write_header(buf, AUDIO_WAV_MIN_HEADER_SIZE - 1U, &hdr), -EINVAL,
		      "a buffer one byte short of the header accepted");
}

ZTEST(audio_wav, test_wav_write_rejects_headers_the_reader_would_reject)
{
	static const struct audio_wav_header cases[] = {
		{ .sample_rate_hz = 0U, .channels = 2U, .bits_per_sample = 16U },
		{ .sample_rate_hz = TEST_SAMPLE_RATE, .channels = 0U, .bits_per_sample = 16U },
		{ .sample_rate_hz = TEST_SAMPLE_RATE, .channels = 2U, .bits_per_sample = 0U },
		{ .sample_rate_hz = TEST_SAMPLE_RATE, .channels = 2U, .bits_per_sample = 12U },
		{ .sample_rate_hz = TEST_SAMPLE_RATE, .channels = 2U, .bits_per_sample = 64U },
		/* A frame of 65536 bytes: one more than block_align can hold, and
		 * the product wraps to zero if nothing checks it.
		 */
		{ .sample_rate_hz = TEST_SAMPLE_RATE, .channels = 16384U, .bits_per_sample = 32U },
	};
	uint8_t buf[AUDIO_WAV_MIN_HEADER_SIZE];
	size_t i;

	/* The two halves of the module agree on what a usable header is, so the
	 * writer can never leave behind a file the reader refuses to open.
	 */
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct audio_wav_header hdr = cases[i];

		hdr.format_tag = AUDIO_WAV_FORMAT_PCM;

		zassert_equal(audio_wav_write_header(buf, sizeof(buf), &hdr), -EINVAL,
			      "case %zu: a degenerate format was serialised", i);
	}
}

ZTEST(audio_wav, test_wav_write_rejects_payload_beyond_the_size_field)
{
	struct audio_wav_header hdr = {
		.sample_rate_hz = TEST_SAMPLE_RATE,
		.data_size = AUDIO_WAV_MAX_DATA_SIZE + 1U,
		.format_tag = AUDIO_WAV_FORMAT_PCM,
		.channels = TEST_CHANNELS,
		.bits_per_sample = TEST_BITS,
	};
	uint8_t buf[AUDIO_WAV_MIN_HEADER_SIZE];

	/* One byte more than the RIFF size field can describe: wrapping it would
	 * produce a header claiming a payload of almost nothing.
	 */
	zassert_equal(audio_wav_write_header(buf, sizeof(buf), &hdr), -EFBIG,
		      "an undescribable payload size was accepted");

	hdr.data_size = AUDIO_WAV_MAX_DATA_SIZE;
	zassert_equal(audio_wav_write_header(buf, sizeof(buf), &hdr), 0,
		      "the largest describable payload was rejected");
}

/* -------------------------------------------------------------------------
 * Reader: the headers the writer refuses to produce
 * ----------------------------------------------------------------------
 */

struct wav_builder {
	uint8_t buf[256];
	size_t len;
};

static void wb_u8(struct wav_builder *b, uint8_t v)
{
	zassert_true(b->len < sizeof(b->buf), "builder overflow");
	b->buf[b->len++] = v;
}

static void wb_u16(struct wav_builder *b, uint16_t v)
{
	wb_u8(b, (uint8_t)(v & 0xffU));
	wb_u8(b, (uint8_t)((v >> 8) & 0xffU));
}

static void wb_u32(struct wav_builder *b, uint32_t v)
{
	wb_u16(b, (uint16_t)(v & 0xffffU));
	wb_u16(b, (uint16_t)((v >> 16) & 0xffffU));
}

static void wb_tag(struct wav_builder *b, const char *tag)
{
	for (size_t i = 0; i < 4U; i++) {
		wb_u8(b, (uint8_t)tag[i]);
	}
}

static void wb_fill(struct wav_builder *b, uint8_t value, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		wb_u8(b, value);
	}
}

/* Writes the RIFF container header; the size field is patched by wb_finish(). */
static void wb_start(struct wav_builder *b)
{
	memset(b, 0, sizeof(*b));
	wb_tag(b, "RIFF");
	wb_u32(b, 0U);
	wb_tag(b, "WAVE");
}

static void wb_fmt(struct wav_builder *b, uint16_t format_tag, uint16_t channels,
		   uint32_t sample_rate, uint16_t bits)
{
	uint16_t block_align = (uint16_t)(channels * (bits / 8U));

	wb_tag(b, "fmt ");
	wb_u32(b, 16U);
	wb_u16(b, format_tag);
	wb_u16(b, channels);
	wb_u32(b, sample_rate);
	wb_u32(b, sample_rate * block_align);
	wb_u16(b, block_align);
	wb_u16(b, bits);
}

static void wb_fmt_default(struct wav_builder *b)
{
	wb_fmt(b, AUDIO_WAV_FORMAT_PCM, TEST_CHANNELS, TEST_SAMPLE_RATE, TEST_BITS);
}

/* Adds a filler chunk of @p payload_len bytes, exercising the chunk walker. */
static void wb_chunk(struct wav_builder *b, const char *tag, size_t payload_len)
{
	wb_tag(b, tag);
	wb_u32(b, (uint32_t)payload_len);
	wb_fill(b, 0xa5U, payload_len);
	if ((payload_len % 2U) != 0U) {
		wb_u8(b, 0U);
	}
}

/* Returns the byte offset the payload of the data chunk will land on. */
static size_t wb_data(struct wav_builder *b, size_t payload_len)
{
	size_t offset;

	wb_tag(b, "data");
	wb_u32(b, (uint32_t)payload_len);
	offset = b->len;
	wb_fill(b, 0x5aU, payload_len);

	return offset;
}

static void wb_finish(struct wav_builder *b)
{
	uint32_t riff_size = (uint32_t)(b->len - 8U);

	b->buf[4] = (uint8_t)(riff_size & 0xffU);
	b->buf[5] = (uint8_t)((riff_size >> 8) & 0xffU);
	b->buf[6] = (uint8_t)((riff_size >> 16) & 0xffU);
	b->buf[7] = (uint8_t)((riff_size >> 24) & 0xffU);
}

/** Parse whatever the builder has assembled so far. */
static int read_built_header(struct wav_builder *b, struct audio_wav_header *out)
{
	memset(out, 0, sizeof(*out));

	return audio_wav_read_header(b->buf, b->len, out);
}

ZTEST(audio_wav, test_wav_walks_chunks_in_any_order)
{
	struct audio_wav_header res;
	struct wav_builder b;
	size_t data_offset;

	wb_start(&b);
	wb_chunk(&b, "JUNK", 12U);
	wb_fmt_default(&b);
	wb_chunk(&b, "LIST", 10U);
	data_offset = wb_data(&b, TEST_DATA_BYTES);
	wb_finish(&b);

	zassert_equal(read_built_header(&b, &res), 0, "extra chunks rejected");
	zassert_equal(res.sample_rate_hz, TEST_SAMPLE_RATE, "wrong sample rate");
	zassert_equal(res.data_offset, (uint32_t)data_offset, "chunk walk found wrong offset");
	zassert_equal(res.data_size, TEST_DATA_BYTES, "wrong data size");
}

ZTEST(audio_wav, test_wav_skips_odd_sized_chunk_padding)
{
	struct audio_wav_header res;
	struct wav_builder b;
	size_t data_offset;

	wb_start(&b);
	wb_fmt_default(&b);
	wb_chunk(&b, "LIST", 7U); /* odd payload -> one pad byte */
	data_offset = wb_data(&b, TEST_DATA_BYTES);
	wb_finish(&b);

	zassert_equal(read_built_header(&b, &res), 0, "odd sized chunk rejected");
	zassert_equal(res.data_offset, (uint32_t)data_offset, "pad byte not skipped");
}

ZTEST(audio_wav, test_wav_rejects_null_arguments)
{
	uint8_t buf[AUDIO_WAV_MIN_HEADER_SIZE];
	struct audio_wav_header res;

	write_valid_header(buf, sizeof(buf));

	zassert_equal(audio_wav_read_header(NULL, sizeof(buf), &res), -EINVAL,
		      "NULL data accepted");
	zassert_equal(audio_wav_read_header(buf, sizeof(buf), NULL), -EINVAL,
		      "NULL result accepted");
}

ZTEST(audio_wav, test_wav_rejects_bad_riff_magic)
{
	uint8_t buf[AUDIO_WAV_MIN_HEADER_SIZE];
	struct audio_wav_header res;

	write_valid_header(buf, sizeof(buf));
	memcpy(&buf[HDR_OFF_RIFF_MAGIC], "RIFX", 4);

	zassert_equal(audio_wav_read_header(buf, sizeof(buf), &res), -EINVAL,
		      "bad RIFF magic accepted");
}

ZTEST(audio_wav, test_wav_rejects_bad_wave_magic)
{
	uint8_t buf[AUDIO_WAV_MIN_HEADER_SIZE];
	struct audio_wav_header res;

	write_valid_header(buf, sizeof(buf));
	memcpy(&buf[HDR_OFF_WAVE_MAGIC], "AVI ", 4);

	zassert_equal(audio_wav_read_header(buf, sizeof(buf), &res), -EINVAL,
		      "bad WAVE magic accepted");
}

ZTEST(audio_wav, test_wav_rejects_truncated_container)
{
	uint8_t buf[AUDIO_WAV_MIN_HEADER_SIZE];
	struct audio_wav_header res;
	size_t len;

	write_valid_header(buf, sizeof(buf));

	for (len = 0; len < 12U; len++) {
		zassert_equal(audio_wav_read_header(buf, len, &res), -EINVAL,
			      "truncated container accepted");
	}
}

ZTEST(audio_wav, test_wav_rejects_truncated_chunk_header)
{
	uint8_t buf[AUDIO_WAV_MIN_HEADER_SIZE];
	struct audio_wav_header res;

	write_valid_header(buf, sizeof(buf));

	/* Half of the "fmt " chunk header. */
	zassert_equal(audio_wav_read_header(buf, 12U + 4U, &res), -EINVAL,
		      "truncated chunk header accepted");
}

ZTEST(audio_wav, test_wav_rejects_truncated_fmt_payload)
{
	uint8_t buf[AUDIO_WAV_MIN_HEADER_SIZE];
	struct audio_wav_header res;

	write_valid_header(buf, sizeof(buf));

	/* The fmt chunk header plus half of its payload. */
	zassert_equal(audio_wav_read_header(buf, 12U + 8U + 8U, &res), -EINVAL,
		      "truncated fmt payload accepted");
}

ZTEST(audio_wav, test_wav_rejects_short_fmt_chunk)
{
	struct audio_wav_header res;
	struct wav_builder b;

	wb_start(&b);
	wb_chunk(&b, "fmt ", 12U); /* below the 16 byte PCM fmt body */
	(void)wb_data(&b, TEST_DATA_BYTES);
	wb_finish(&b);

	zassert_equal(read_built_header(&b, &res), -EINVAL, "short fmt chunk accepted");
}

ZTEST(audio_wav, test_wav_rejects_missing_fmt_chunk)
{
	struct audio_wav_header res;
	struct wav_builder b;

	wb_start(&b);
	wb_chunk(&b, "LIST", 8U);
	(void)wb_data(&b, TEST_DATA_BYTES);
	wb_finish(&b);

	zassert_equal(read_built_header(&b, &res), -EINVAL, "missing fmt chunk accepted");
}

ZTEST(audio_wav, test_wav_rejects_missing_data_chunk)
{
	struct audio_wav_header res;
	struct wav_builder b;

	wb_start(&b);
	wb_fmt_default(&b);
	wb_chunk(&b, "LIST", 8U);
	wb_finish(&b);

	zassert_equal(read_built_header(&b, &res), -EINVAL, "missing data chunk accepted");
}

ZTEST(audio_wav, test_wav_rejects_oversized_chunk_size)
{
	struct audio_wav_header res;
	struct wav_builder b;

	/* A chunk claiming a size beyond the buffer must not be walked past. */
	wb_start(&b);
	wb_fmt_default(&b);
	wb_tag(&b, "LIST");
	wb_u32(&b, 0xfffffff0U);
	wb_finish(&b);

	zassert_equal(read_built_header(&b, &res), -EINVAL, "oversized chunk accepted");
}

ZTEST(audio_wav, test_wav_rejects_degenerate_fmt_fields)
{
	struct audio_wav_header res;
	struct wav_builder b;

	wb_start(&b);
	wb_fmt(&b, AUDIO_WAV_FORMAT_PCM, 0U, TEST_SAMPLE_RATE, TEST_BITS);
	(void)wb_data(&b, TEST_DATA_BYTES);
	wb_finish(&b);
	zassert_equal(read_built_header(&b, &res), -EINVAL, "zero channels accepted");

	wb_start(&b);
	wb_fmt(&b, AUDIO_WAV_FORMAT_PCM, TEST_CHANNELS, 0U, TEST_BITS);
	(void)wb_data(&b, TEST_DATA_BYTES);
	wb_finish(&b);
	zassert_equal(read_built_header(&b, &res), -EINVAL, "zero sample rate accepted");

	wb_start(&b);
	wb_fmt(&b, AUDIO_WAV_FORMAT_PCM, TEST_CHANNELS, TEST_SAMPLE_RATE, 0U);
	(void)wb_data(&b, TEST_DATA_BYTES);
	wb_finish(&b);
	zassert_equal(read_built_header(&b, &res), -EINVAL, "zero bit depth accepted");

	wb_start(&b);
	wb_fmt(&b, AUDIO_WAV_FORMAT_PCM, TEST_CHANNELS, TEST_SAMPLE_RATE, 12U);
	(void)wb_data(&b, TEST_DATA_BYTES);
	wb_finish(&b);
	zassert_equal(read_built_header(&b, &res), -EINVAL, "non byte aligned bit depth accepted");

	/* 16384 channels of 32 bit make a 65536 byte frame, which the 16 bit
	 * block_align field can only hold as zero.
	 */
	wb_start(&b);
	wb_fmt(&b, AUDIO_WAV_FORMAT_PCM, 16384U, TEST_SAMPLE_RATE, 32U);
	(void)wb_data(&b, TEST_DATA_BYTES);
	wb_finish(&b);
	zassert_equal(read_built_header(&b, &res), -EINVAL, "a frame beyond block_align accepted");
}

ZTEST(audio_wav, test_wav_rejects_inconsistent_block_align)
{
	uint8_t buf[AUDIO_WAV_MIN_HEADER_SIZE];
	struct audio_wav_header res;

	write_valid_header(buf, sizeof(buf));
	buf[HDR_OFF_BLOCK_ALIGN] = 3U;

	zassert_equal(audio_wav_read_header(buf, sizeof(buf), &res), -EINVAL,
		      "inconsistent block align accepted");
}

ZTEST(audio_wav, test_wav_rejects_oversized_bit_depth)
{
	struct audio_wav_header res;
	struct wav_builder b;

	wb_start(&b);
	wb_fmt(&b, AUDIO_WAV_FORMAT_PCM, TEST_CHANNELS, TEST_SAMPLE_RATE, 64U);
	(void)wb_data(&b, TEST_DATA_BYTES);
	wb_finish(&b);

	zassert_equal(read_built_header(&b, &res), -EINVAL, "64-bit sample depth accepted");
}

ZTEST_SUITE(audio_wav, NULL, NULL, NULL, NULL, NULL);
