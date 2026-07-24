/*
 * RIFF/WAVE header parser for the audio pipeline file nodes.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_AUDIO_WAV_PARSER_H_
#define ZEPHYR_AUDIO_WAV_PARSER_H_

#include <stddef.h>

#include <zephyr/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Format tag of uncompressed PCM (WAVE_FORMAT_PCM); the only tag accepted in v1. */
#define WAV_PARSER_FORMAT_PCM 0x0001U

/**
 * Size of a canonical RIFF/WAVE header (RIFF + 16 byte fmt + data chunk header).
 *
 * Files carrying extra chunks need a larger prefix; ::WAV_PARSER_HEADER_SCAN_SIZE
 * is a safe default for readers that buffer the header before seeking.
 */
#define WAV_PARSER_MIN_HEADER_SIZE 44U

/** Recommended prefix size a reader should buffer before parsing. */
#define WAV_PARSER_HEADER_SCAN_SIZE 256U

/**
 * Format and payload location extracted from a RIFF/WAVE header.
 *
 * The values describe the on-disk file, not the canonical pipeline format:
 * a source node is responsible for converting @ref bits_per_sample samples
 * to ``AUDIO_SAMPLE_FORMAT_S32_LE``.
 */
struct wav_parser_result {
	/** Sampling frequency in Hz, e.g. 44100 or 48000. */
	uint32_t sample_rate_hz;
	/** Byte offset of the first payload byte of the ``data`` chunk. */
	uint32_t data_offset;
	/** Payload size of the ``data`` chunk in bytes, as declared by the file. */
	uint32_t data_size;
	/** WAVE format tag; always ::WAV_PARSER_FORMAT_PCM on success. */
	uint16_t format_tag;
	/** Number of interleaved channels. */
	uint16_t channels;
	/** Bits per sample of the on-disk payload (8, 16, 24 or 32). */
	uint16_t bits_per_sample;
	/** Bytes per interleaved sample frame (``channels * bits_per_sample / 8``). */
	uint16_t block_align;
};

/**
 * Parse a RIFF/WAVE header from a byte buffer.
 *
 * Walks the RIFF chunk list rather than assuming fixed offsets, so chunks such
 * as ``JUNK`` or ``LIST`` placed before, between or after ``fmt `` and ``data``
 * are skipped. @p data only has to contain the header chunks; the ``data``
 * payload itself may extend past @p len, which lets a file reader parse a short
 * prefix of the file and then seek to ``data_offset``.
 *
 * @param data Buffer holding the beginning of the file. Must not be NULL.
 * @param len  Number of valid bytes in @p data.
 * @param out  Receives the parsed format on success. Must not be NULL. Its
 *             contents are unspecified when the call fails.
 *
 * @retval 0        Header is a valid PCM WAVE header; @p out is populated.
 * @retval -EINVAL  Arguments are NULL, the buffer is truncated, the RIFF/WAVE
 *                  magic is wrong, a required chunk is missing, or a ``fmt ``
 *                  field is degenerate (zero sample rate or channel count, a
 *                  bit depth that is zero, not a multiple of eight or above
 *                  32, or a ``block_align`` inconsistent with the two).
 * @retval -ENOTSUP Header is structurally valid but the format tag is not
 *                  ::WAV_PARSER_FORMAT_PCM.
 */
int wav_parser_read_header(const uint8_t *data, size_t len, struct wav_parser_result *out);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_AUDIO_WAV_PARSER_H_ */
