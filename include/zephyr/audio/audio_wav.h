/*
 * RIFF/WAVE header codec: the one place in the subsystem that knows the byte
 * layout of a WAVE header, in both directions.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_AUDIO_WAV_H_
#define ZEPHYR_AUDIO_WAV_H_

#include <stddef.h>

#include <zephyr/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Format tag of uncompressed PCM (WAVE_FORMAT_PCM); the only tag accepted in v1. */
#define AUDIO_WAV_FORMAT_PCM 0x0001U

/**
 * Size of a canonical RIFF/WAVE header (RIFF + 16 byte fmt + data chunk header).
 *
 * This is exactly what ::audio_wav_write_header emits. Files carrying extra
 * chunks need a larger prefix to parse; ::AUDIO_WAV_HEADER_SCAN_SIZE is a safe
 * default for readers that buffer the header before seeking.
 */
#define AUDIO_WAV_MIN_HEADER_SIZE 44U

/** Recommended prefix size a reader should buffer before parsing. */
#define AUDIO_WAV_HEADER_SCAN_SIZE 256U

/**
 * Largest ``data`` payload a canonical header can describe.
 *
 * Both size fields are 32 bit and the RIFF one also counts the header bytes
 * that follow it, so the payload is capped below UINT32_MAX.
 */
#define AUDIO_WAV_MAX_DATA_SIZE (UINT32_MAX - (AUDIO_WAV_MIN_HEADER_SIZE - 8U))

/**
 * Format and payload location of a RIFF/WAVE header.
 *
 * The same description serves both directions: ::audio_wav_read_header fills
 * every field in, ::audio_wav_write_header reads the format fields and derives
 * the rest.
 *
 * - @ref sample_rate_hz, @ref data_size, @ref format_tag, @ref channels and
 *   @ref bits_per_sample describe the stream and are both read and written.
 * - @ref data_offset and @ref block_align are derived: outputs of a read, and
 *   ignored by a write because they follow from the fields above.
 *
 * The values describe the on-disk file, not the canonical pipeline format: a
 * source node is responsible for converting @ref bits_per_sample samples to
 * ``AUDIO_SAMPLE_FORMAT_S32_LE``.
 */
struct audio_wav_header {
	/** Sampling frequency in Hz, e.g. 44100 or 48000. */
	uint32_t sample_rate_hz;
	/** Payload size of the ``data`` chunk in bytes, as declared by the file. */
	uint32_t data_size;
	/** Derived: byte offset of the first payload byte of the ``data`` chunk. */
	uint32_t data_offset;
	/** WAVE format tag; always ::AUDIO_WAV_FORMAT_PCM on a successful read. */
	uint16_t format_tag;
	/** Number of interleaved channels. */
	uint16_t channels;
	/** Bits per sample of the on-disk payload (8, 16, 24 or 32). */
	uint16_t bits_per_sample;
	/** Derived: bytes per frame (``channels * bits_per_sample / 8``). */
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
 *                  32, a frame that does not fit the 16 bit ``block_align``
 *                  field, or a ``block_align`` inconsistent with the two).
 * @retval -ENOTSUP Header is structurally valid but the format tag is not
 *                  ::AUDIO_WAV_FORMAT_PCM.
 */
int audio_wav_read_header(const uint8_t *data, size_t len, struct audio_wav_header *out);

/**
 * Serialise a canonical RIFF/WAVE header into a byte buffer.
 *
 * Writes exactly ::AUDIO_WAV_MIN_HEADER_SIZE bytes: the RIFF container, a 16
 * byte PCM ``fmt `` chunk and the ``data`` chunk header, with no payload. Every
 * field goes out little endian explicitly, so the same bytes are produced on a
 * big endian host.
 *
 * @ref audio_wav_header.data_size is written verbatim rather than checked
 * against anything the caller has on disk. A writer that only learns the
 * payload size at end of stream therefore emits a zero first and calls this
 * again with the real size once the stream ends.
 *
 * Whatever this call accepts, ::audio_wav_read_header parses back - the one
 * exception being a @ref audio_wav_header.format_tag other than
 * ::AUDIO_WAV_FORMAT_PCM, which is emitted as asked and read back as
 * ``-ENOTSUP``.
 *
 * @param buf Buffer receiving the header. Must not be NULL.
 * @param len Capacity of @p buf; must be at least ::AUDIO_WAV_MIN_HEADER_SIZE.
 * @param hdr Format to serialise. Must not be NULL. @ref
 *            audio_wav_header.data_offset and @ref audio_wav_header.block_align
 *            are ignored; both follow from the rest of the header.
 *
 * @retval 0       Header written; @p buf holds ::AUDIO_WAV_MIN_HEADER_SIZE bytes.
 * @retval -EINVAL Arguments are NULL, @p len is too small, or a format field is
 *                 degenerate - the same rules ::audio_wav_read_header enforces,
 *                 so a header this call emits is never one the parser rejects.
 * @retval -EFBIG  @ref audio_wav_header.data_size exceeds
 *                 ::AUDIO_WAV_MAX_DATA_SIZE, which the 32 bit RIFF size field
 *                 cannot describe.
 */
int audio_wav_write_header(uint8_t *buf, size_t len, const struct audio_wav_header *hdr);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_AUDIO_WAV_H_ */
