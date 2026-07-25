/*
 * RIFF/WAVE header codec for the audio pipeline file nodes.
 *
 * Both directions are allocation free and work on a caller supplied byte
 * buffer: a file reader can inspect a short prefix of a file and then seek
 * directly to the audio payload, and a file writer can serialise a header into
 * a stack buffer and hand it to the filesystem in one write.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>

#include <zephyr/audio/audio_wav.h>

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

/*
 * Field offsets inside the canonical header the writer emits. The parser walks
 * the chunk list instead of using these, so they only describe what this
 * module produces, never what it is willing to read.
 */
#define HDR_OFF_RIFF_ID	  0U
#define HDR_OFF_RIFF_SIZE 4U
#define HDR_OFF_WAVE_ID	  8U
#define HDR_OFF_FMT_ID	  12U
#define HDR_OFF_FMT_SIZE  16U
#define HDR_OFF_FMT_BODY  20U
#define HDR_OFF_DATA_ID	  36U
#define HDR_OFF_DATA_SIZE 40U

/* Header bytes counted by the RIFF size field: everything after it. */
#define RIFF_SIZE_OVERHEAD (AUDIO_WAV_MIN_HEADER_SIZE - 8U)

static bool tag_matches(const uint8_t *data, const char *tag)
{
	return memcmp(data, tag, 4) == 0;
}

/*
 * The one definition of a usable stream description, applied to both a header
 * being read and a header about to be written - which is what keeps the writer
 * from ever producing a file this parser would reject.
 */
static bool format_is_usable(uint32_t sample_rate_hz, uint16_t channels, uint16_t bits_per_sample)
{
	if (sample_rate_hz == 0U || channels == 0U) {
		return false;
	}

	if (bits_per_sample == 0U || (bits_per_sample % 8U) != 0U ||
	    bits_per_sample > MAX_BITS_PER_SAMPLE) {
		return false;
	}

	/* block_align is 16 bit on disk. Without this the product would wrap
	 * silently and both halves of the module would agree on a frame size
	 * of zero, while every other tool sees a corrupt header.
	 */
	if (((uint32_t)channels * (bits_per_sample / 8U)) > UINT16_MAX) {
		return false;
	}

	return true;
}

static uint16_t block_align_of(uint16_t channels, uint16_t bits_per_sample)
{
	return (uint16_t)(channels * (bits_per_sample / 8U));
}

static int parse_fmt_chunk(const uint8_t *body, uint32_t size, struct audio_wav_header *out)
{
	if (size < FMT_CHUNK_MIN_SIZE) {
		return -EINVAL;
	}

	out->format_tag = sys_get_le16(&body[FMT_OFF_FORMAT_TAG]);
	out->channels = sys_get_le16(&body[FMT_OFF_CHANNELS]);
	out->sample_rate_hz = sys_get_le32(&body[FMT_OFF_SAMPLE_RATE]);
	out->block_align = sys_get_le16(&body[FMT_OFF_BLOCK_ALIGN]);
	out->bits_per_sample = sys_get_le16(&body[FMT_OFF_BITS_PER_SAMPLE]);

	if (out->format_tag != AUDIO_WAV_FORMAT_PCM) {
		return -ENOTSUP;
	}

	if (!format_is_usable(out->sample_rate_hz, out->channels, out->bits_per_sample)) {
		return -EINVAL;
	}

	if (out->block_align != block_align_of(out->channels, out->bits_per_sample)) {
		return -EINVAL;
	}

	return 0;
}

int audio_wav_read_header(const uint8_t *data, size_t len, struct audio_wav_header *out)
{
	struct audio_wav_header parsed = { 0 };
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

int audio_wav_write_header(uint8_t *buf, size_t len, const struct audio_wav_header *hdr)
{
	uint16_t block_align;

	if (buf == NULL || hdr == NULL || len < AUDIO_WAV_MIN_HEADER_SIZE) {
		return -EINVAL;
	}

	if (!format_is_usable(hdr->sample_rate_hz, hdr->channels, hdr->bits_per_sample)) {
		return -EINVAL;
	}

	if (hdr->data_size > AUDIO_WAV_MAX_DATA_SIZE) {
		/* The RIFF size field counts the payload plus the header bytes
		 * behind it, so anything larger would wrap around and describe
		 * an almost empty file.
		 */
		return -EFBIG;
	}

	block_align = block_align_of(hdr->channels, hdr->bits_per_sample);

	/* Everything goes out little endian explicitly, so a big endian host
	 * produces the same file.
	 */
	memcpy(&buf[HDR_OFF_RIFF_ID], "RIFF", 4);
	sys_put_le32(RIFF_SIZE_OVERHEAD + hdr->data_size, &buf[HDR_OFF_RIFF_SIZE]);
	memcpy(&buf[HDR_OFF_WAVE_ID], "WAVE", 4);

	memcpy(&buf[HDR_OFF_FMT_ID], "fmt ", 4);
	sys_put_le32(FMT_CHUNK_MIN_SIZE, &buf[HDR_OFF_FMT_SIZE]);
	sys_put_le16(hdr->format_tag, &buf[HDR_OFF_FMT_BODY + FMT_OFF_FORMAT_TAG]);
	sys_put_le16(hdr->channels, &buf[HDR_OFF_FMT_BODY + FMT_OFF_CHANNELS]);
	sys_put_le32(hdr->sample_rate_hz, &buf[HDR_OFF_FMT_BODY + FMT_OFF_SAMPLE_RATE]);
	sys_put_le32(hdr->sample_rate_hz * block_align, &buf[HDR_OFF_FMT_BODY + FMT_OFF_BYTE_RATE]);
	sys_put_le16(block_align, &buf[HDR_OFF_FMT_BODY + FMT_OFF_BLOCK_ALIGN]);
	sys_put_le16(hdr->bits_per_sample, &buf[HDR_OFF_FMT_BODY + FMT_OFF_BITS_PER_SAMPLE]);

	memcpy(&buf[HDR_OFF_DATA_ID], "data", 4);
	sys_put_le32(hdr->data_size, &buf[HDR_OFF_DATA_SIZE]);

	return 0;
}
