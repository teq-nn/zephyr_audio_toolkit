/*
 * Unit tests for the RIFF/WAVE header parser.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "wav_parser.h"

/* Canonical 16-bit stereo 44.1 kHz PCM payload used by the happy-path cases. */
#define TEST_SAMPLE_RATE 44100U
#define TEST_CHANNELS	 2U
#define TEST_BITS	 16U
#define TEST_DATA_BYTES	 8U

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
	wb_fmt(b, WAV_PARSER_FORMAT_PCM, TEST_CHANNELS, TEST_SAMPLE_RATE, TEST_BITS);
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

static size_t build_valid_wav(struct wav_builder *b)
{
	size_t data_offset;

	wb_start(b);
	wb_fmt_default(b);
	data_offset = wb_data(b, TEST_DATA_BYTES);
	wb_finish(b);

	return data_offset;
}

static int parse(struct wav_builder *b, struct wav_parser_result *out)
{
	memset(out, 0, sizeof(*out));

	return wav_parser_read_header(b->buf, b->len, out);
}

ZTEST(wav_parser, test_wav_parser_accepts_16bit_stereo_pcm)
{
	struct wav_parser_result res;
	struct wav_builder b;
	size_t data_offset = build_valid_wav(&b);

	zassert_equal(parse(&b, &res), 0, "valid header rejected");
	zassert_equal(res.sample_rate_hz, TEST_SAMPLE_RATE, "wrong sample rate");
	zassert_equal(res.channels, TEST_CHANNELS, "wrong channel count");
	zassert_equal(res.bits_per_sample, TEST_BITS, "wrong bit depth");
	zassert_equal(res.format_tag, WAV_PARSER_FORMAT_PCM, "wrong format tag");
	zassert_equal(res.block_align, TEST_CHANNELS * (TEST_BITS / 8U), "wrong block align");
	zassert_equal(res.data_offset, (uint32_t)data_offset, "wrong data offset");
	zassert_equal(res.data_size, TEST_DATA_BYTES, "wrong data size");
}

ZTEST(wav_parser, test_wav_parser_walks_chunks_in_any_order)
{
	struct wav_parser_result res;
	struct wav_builder b;
	size_t data_offset;

	wb_start(&b);
	wb_chunk(&b, "JUNK", 12U);
	wb_fmt_default(&b);
	wb_chunk(&b, "LIST", 10U);
	data_offset = wb_data(&b, TEST_DATA_BYTES);
	wb_finish(&b);

	zassert_equal(parse(&b, &res), 0, "extra chunks rejected");
	zassert_equal(res.sample_rate_hz, TEST_SAMPLE_RATE, "wrong sample rate");
	zassert_equal(res.data_offset, (uint32_t)data_offset, "chunk walk found wrong offset");
	zassert_equal(res.data_size, TEST_DATA_BYTES, "wrong data size");
}

ZTEST(wav_parser, test_wav_parser_skips_odd_sized_chunk_padding)
{
	struct wav_parser_result res;
	struct wav_builder b;
	size_t data_offset;

	wb_start(&b);
	wb_fmt_default(&b);
	wb_chunk(&b, "LIST", 7U); /* odd payload -> one pad byte */
	data_offset = wb_data(&b, TEST_DATA_BYTES);
	wb_finish(&b);

	zassert_equal(parse(&b, &res), 0, "odd sized chunk rejected");
	zassert_equal(res.data_offset, (uint32_t)data_offset, "pad byte not skipped");
}

ZTEST(wav_parser, test_wav_parser_accepts_header_only_prefix)
{
	struct wav_parser_result res;
	struct wav_builder b;
	size_t data_offset = build_valid_wav(&b);

	/* A file reader only buffers the header; the payload is read later. */
	b.len = data_offset;

	zassert_equal(parse(&b, &res), 0, "header-only prefix rejected");
	zassert_equal(res.data_offset, (uint32_t)data_offset, "wrong data offset");
	zassert_equal(res.data_size, TEST_DATA_BYTES, "wrong data size");
}

ZTEST(wav_parser, test_wav_parser_rejects_null_arguments)
{
	struct wav_parser_result res;
	struct wav_builder b;

	(void)build_valid_wav(&b);

	zassert_equal(wav_parser_read_header(NULL, b.len, &res), -EINVAL, "NULL data accepted");
	zassert_equal(wav_parser_read_header(b.buf, b.len, NULL), -EINVAL, "NULL result accepted");
}

ZTEST(wav_parser, test_wav_parser_rejects_bad_riff_magic)
{
	struct wav_parser_result res;
	struct wav_builder b;

	(void)build_valid_wav(&b);
	memcpy(&b.buf[0], "RIFX", 4);

	zassert_equal(parse(&b, &res), -EINVAL, "bad RIFF magic accepted");
}

ZTEST(wav_parser, test_wav_parser_rejects_bad_wave_magic)
{
	struct wav_parser_result res;
	struct wav_builder b;

	(void)build_valid_wav(&b);
	memcpy(&b.buf[8], "AVI ", 4);

	zassert_equal(parse(&b, &res), -EINVAL, "bad WAVE magic accepted");
}

ZTEST(wav_parser, test_wav_parser_rejects_truncated_container)
{
	struct wav_parser_result res;
	struct wav_builder b;

	(void)build_valid_wav(&b);

	for (size_t len = 0; len < 12U; len++) {
		b.len = len;
		zassert_equal(parse(&b, &res), -EINVAL, "truncated container accepted");
	}
}

ZTEST(wav_parser, test_wav_parser_rejects_truncated_chunk_header)
{
	struct wav_parser_result res;
	struct wav_builder b;

	(void)build_valid_wav(&b);
	b.len = 12U + 4U; /* half of the "fmt " chunk header */

	zassert_equal(parse(&b, &res), -EINVAL, "truncated chunk header accepted");
}

ZTEST(wav_parser, test_wav_parser_rejects_truncated_fmt_payload)
{
	struct wav_parser_result res;
	struct wav_builder b;

	(void)build_valid_wav(&b);
	b.len = 12U + 8U + 8U; /* fmt chunk header plus half of its payload */

	zassert_equal(parse(&b, &res), -EINVAL, "truncated fmt payload accepted");
}

ZTEST(wav_parser, test_wav_parser_rejects_short_fmt_chunk)
{
	struct wav_parser_result res;
	struct wav_builder b;

	wb_start(&b);
	wb_chunk(&b, "fmt ", 12U); /* below the 16 byte PCM fmt body */
	(void)wb_data(&b, TEST_DATA_BYTES);
	wb_finish(&b);

	zassert_equal(parse(&b, &res), -EINVAL, "short fmt chunk accepted");
}

ZTEST(wav_parser, test_wav_parser_rejects_missing_fmt_chunk)
{
	struct wav_parser_result res;
	struct wav_builder b;

	wb_start(&b);
	wb_chunk(&b, "LIST", 8U);
	(void)wb_data(&b, TEST_DATA_BYTES);
	wb_finish(&b);

	zassert_equal(parse(&b, &res), -EINVAL, "missing fmt chunk accepted");
}

ZTEST(wav_parser, test_wav_parser_rejects_missing_data_chunk)
{
	struct wav_parser_result res;
	struct wav_builder b;

	wb_start(&b);
	wb_fmt_default(&b);
	wb_chunk(&b, "LIST", 8U);
	wb_finish(&b);

	zassert_equal(parse(&b, &res), -EINVAL, "missing data chunk accepted");
}

ZTEST(wav_parser, test_wav_parser_rejects_oversized_chunk_size)
{
	struct wav_parser_result res;
	struct wav_builder b;

	/* A chunk claiming a size beyond the buffer must not be walked past. */
	wb_start(&b);
	wb_fmt_default(&b);
	wb_tag(&b, "LIST");
	wb_u32(&b, 0xfffffff0U);
	wb_finish(&b);

	zassert_equal(parse(&b, &res), -EINVAL, "oversized chunk accepted");
}

ZTEST(wav_parser, test_wav_parser_rejects_non_pcm_format_tag)
{
	struct wav_parser_result res;
	struct wav_builder b;
	static const uint16_t tags[] = { 0x0000U, 0x0003U, 0x0006U, 0xfffeU };

	for (size_t i = 0; i < ARRAY_SIZE(tags); i++) {
		wb_start(&b);
		wb_fmt(&b, tags[i], TEST_CHANNELS, TEST_SAMPLE_RATE, TEST_BITS);
		(void)wb_data(&b, TEST_DATA_BYTES);
		wb_finish(&b);

		zassert_equal(parse(&b, &res), -ENOTSUP, "non-PCM format tag accepted");
	}
}

ZTEST(wav_parser, test_wav_parser_rejects_degenerate_fmt_fields)
{
	struct wav_parser_result res;
	struct wav_builder b;

	wb_start(&b);
	wb_fmt(&b, WAV_PARSER_FORMAT_PCM, 0U, TEST_SAMPLE_RATE, TEST_BITS);
	(void)wb_data(&b, TEST_DATA_BYTES);
	wb_finish(&b);
	zassert_equal(parse(&b, &res), -EINVAL, "zero channels accepted");

	wb_start(&b);
	wb_fmt(&b, WAV_PARSER_FORMAT_PCM, TEST_CHANNELS, 0U, TEST_BITS);
	(void)wb_data(&b, TEST_DATA_BYTES);
	wb_finish(&b);
	zassert_equal(parse(&b, &res), -EINVAL, "zero sample rate accepted");

	wb_start(&b);
	wb_fmt(&b, WAV_PARSER_FORMAT_PCM, TEST_CHANNELS, TEST_SAMPLE_RATE, 0U);
	(void)wb_data(&b, TEST_DATA_BYTES);
	wb_finish(&b);
	zassert_equal(parse(&b, &res), -EINVAL, "zero bit depth accepted");

	wb_start(&b);
	wb_fmt(&b, WAV_PARSER_FORMAT_PCM, TEST_CHANNELS, TEST_SAMPLE_RATE, 12U);
	(void)wb_data(&b, TEST_DATA_BYTES);
	wb_finish(&b);
	zassert_equal(parse(&b, &res), -EINVAL, "non byte aligned bit depth accepted");
}

ZTEST(wav_parser, test_wav_parser_rejects_inconsistent_block_align)
{
	struct wav_parser_result res;
	struct wav_builder b;

	(void)build_valid_wav(&b);
	/* block_align sits 12 bytes into the fmt body, which starts at offset 20. */
	b.buf[20 + 12] = 3U;

	zassert_equal(parse(&b, &res), -EINVAL, "inconsistent block align accepted");
}

ZTEST(wav_parser, test_wav_parser_rejects_oversized_bit_depth)
{
	struct wav_parser_result res;
	struct wav_builder b;

	wb_start(&b);
	wb_fmt(&b, WAV_PARSER_FORMAT_PCM, TEST_CHANNELS, TEST_SAMPLE_RATE, 64U);
	(void)wb_data(&b, TEST_DATA_BYTES);
	wb_finish(&b);

	zassert_equal(parse(&b, &res), -EINVAL, "64-bit sample depth accepted");
}

ZTEST(wav_parser, test_wav_parser_accepts_24bit_mono)
{
	struct wav_parser_result res;
	struct wav_builder b;
	size_t data_offset;

	wb_start(&b);
	wb_fmt(&b, WAV_PARSER_FORMAT_PCM, 1U, 48000U, 24U);
	data_offset = wb_data(&b, 9U);
	wb_finish(&b);

	zassert_equal(parse(&b, &res), 0, "24-bit mono header rejected");
	zassert_equal(res.sample_rate_hz, 48000U, "wrong sample rate");
	zassert_equal(res.channels, 1U, "wrong channel count");
	zassert_equal(res.bits_per_sample, 24U, "wrong bit depth");
	zassert_equal(res.block_align, 3U, "wrong block align");
	zassert_equal(res.data_offset, (uint32_t)data_offset, "wrong data offset");
	zassert_equal(res.data_size, 9U, "wrong data size");
}

ZTEST_SUITE(wav_parser, NULL, NULL, NULL, NULL, NULL);
