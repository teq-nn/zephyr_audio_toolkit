/*
 * RIFF/WAVE header parser for the audio pipeline file nodes.
 *
 * The parser is allocation free and works on a caller supplied byte buffer so
 * that a file reader can inspect a short prefix of a file and then seek
 * directly to the audio payload.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>

#include "wav_parser.h"

/* "RIFF" + size + "WAVE" */
#define RIFF_HEADER_SIZE 12U
/* Four character chunk id + 32 bit chunk size */
#define CHUNK_HEADER_SIZE 8U
/* Size of the PCM flavour of the "fmt " chunk body */
#define FMT_CHUNK_MIN_SIZE 16U
/* Highest bit depth the canonical S32_LE container can hold */
#define MAX_BITS_PER_SAMPLE 32U

/* Field offsets inside the "fmt " chunk body */
#define FMT_OFF_FORMAT_TAG	0U
#define FMT_OFF_CHANNELS	2U
#define FMT_OFF_SAMPLE_RATE	4U
#define FMT_OFF_BYTE_RATE	8U
#define FMT_OFF_BLOCK_ALIGN	12U
#define FMT_OFF_BITS_PER_SAMPLE 14U

static bool tag_matches(const uint8_t *data, const char *tag)
{
	return memcmp(data, tag, 4) == 0;
}

static int parse_fmt_chunk(const uint8_t *body, uint32_t size, struct wav_parser_result *out)
{
	uint16_t expected_block_align;

	if (size < FMT_CHUNK_MIN_SIZE) {
		return -EINVAL;
	}

	out->format_tag = sys_get_le16(&body[FMT_OFF_FORMAT_TAG]);
	out->channels = sys_get_le16(&body[FMT_OFF_CHANNELS]);
	out->sample_rate_hz = sys_get_le32(&body[FMT_OFF_SAMPLE_RATE]);
	out->block_align = sys_get_le16(&body[FMT_OFF_BLOCK_ALIGN]);
	out->bits_per_sample = sys_get_le16(&body[FMT_OFF_BITS_PER_SAMPLE]);

	if (out->format_tag != WAV_PARSER_FORMAT_PCM) {
		return -ENOTSUP;
	}

	if (out->sample_rate_hz == 0U || out->channels == 0U) {
		return -EINVAL;
	}

	if (out->bits_per_sample == 0U || (out->bits_per_sample % 8U) != 0U ||
	    out->bits_per_sample > MAX_BITS_PER_SAMPLE) {
		return -EINVAL;
	}

	expected_block_align = (uint16_t)(out->channels * (out->bits_per_sample / 8U));
	if (out->block_align != expected_block_align) {
		return -EINVAL;
	}

	return 0;
}

int wav_parser_read_header(const uint8_t *data, size_t len, struct wav_parser_result *out)
{
	struct wav_parser_result parsed = { 0 };
	bool have_fmt = false;
	bool have_data = false;
	size_t pos;

	if (data == NULL || out == NULL) {
		return -EINVAL;
	}

	if (len < RIFF_HEADER_SIZE) {
		return -EINVAL;
	}

	if (!tag_matches(&data[0], "RIFF") || !tag_matches(&data[8], "WAVE")) {
		return -EINVAL;
	}

	/*
	 * Walk the chunk list instead of assuming the canonical layout: files
	 * routinely carry "JUNK", "LIST" or "fact" chunks around "fmt " and
	 * "data".
	 */
	pos = RIFF_HEADER_SIZE;
	while ((len - pos) >= CHUNK_HEADER_SIZE) {
		const uint8_t *id = &data[pos];
		uint32_t chunk_size = sys_get_le32(&data[pos + 4]);
		size_t body = pos + CHUNK_HEADER_SIZE;
		size_t available = len - body;
		uint64_t advance;

		if (tag_matches(id, "fmt ")) {
			int err;

			if (chunk_size > available) {
				return -EINVAL;
			}

			err = parse_fmt_chunk(&data[body], chunk_size, &parsed);
			if (err != 0) {
				return err;
			}

			have_fmt = true;
		} else if (tag_matches(id, "data")) {
			parsed.data_offset = (uint32_t)body;
			parsed.data_size = chunk_size;
			have_data = true;

			/*
			 * The payload may extend past the buffer, so stop here
			 * as soon as everything needed has been collected.
			 */
			if (have_fmt) {
				break;
			}
		}

		/* Chunks are padded to an even length. */
		advance = (uint64_t)chunk_size + (chunk_size & 1U);
		if (advance > (uint64_t)available) {
			/* Remainder is not buffered; nothing left to walk. */
			break;
		}

		pos = body + (size_t)advance;
	}

	if (!have_fmt || !have_data) {
		return -EINVAL;
	}

	*out = parsed;

	return 0;
}
