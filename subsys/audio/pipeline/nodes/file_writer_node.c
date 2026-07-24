/*
 * File writer sink node.
 *
 * open() creates the output file through the Zephyr filesystem API and writes a
 * canonical 44 byte RIFF/WAVE header; process() pulls a frame from upstream,
 * narrows the canonical S32_LE container back to 16 bit PCM and appends it to
 * the data chunk; close() back-patches the two size fields so the file is a
 * valid WAV (manifest §2/§4/§7, spec §5.3/§10.2).
 *
 * Sizes are not known until the stream ends, so the header written by open()
 * declares an *empty* data chunk (RIFF size 36, data size 0) and the real sizes
 * are patched in afterwards. The patch happens on end of stream as well as in
 * close(), so a file is valid as soon as the pipeline reports EOF. A run that
 * dies without either - a crash, a reset, a filesystem that stops accepting
 * writes - therefore leaves a structurally valid header that claims no payload:
 * a reader sees an empty track and stops immediately instead of replaying
 * whatever bytes happen to follow.
 *
 * All state lives in the per-instance ::audio_file_writer_state allocated by
 * AUDIO_FILE_WRITER_NODE_DEFINE(), so several writers can run side by side.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <zephyr/audio/audio_node.h>
#include <zephyr/audio/audio_nodes.h>
#include <zephyr/audio/audio_wav.h>

LOG_MODULE_REGISTER(audio_file_writer, LOG_LEVEL_INF);

/* v1 writes 16 bit PCM; the spec's 24 bit path is not built yet. */
#define FILE_WRITER_BITS_PER_SAMPLE 16U
#define FILE_WRITER_BYTES_PER_SAMPLE (FILE_WRITER_BITS_PER_SAMPLE / 8U)

/* Interleaving is pipeline-wide (spec §5.2); v1 is stereo, mono costs nothing. */
#define FILE_WRITER_MAX_CHANNELS 2U

/* Applied to every zero field of the caller's format, so a writer defined with
 * AUDIO_FILE_WRITER_NODE_DEFINE() and never touched again produces the v1
 * pipeline format.
 */
#define FILE_WRITER_DEFAULT_RATE_HZ 48000U
#define FILE_WRITER_DEFAULT_CHANNELS 2U

/*
 * The pipeline reserves -EPIPE for "end of stream": audio_pipeline_process_frame()
 * returns it when the sink reports out_size == 0, and the worker thread turns
 * that into a clean EOF event. A real failure must therefore never leave this
 * node as -EPIPE, or a broken file - or a broken upstream - would look like a
 * finished track.
 */
static int file_writer_errno(int err)
{
	if (err == -EPIPE) {
		LOG_WRN("remapping -EPIPE to -EIO");
		return -EIO;
	}

	return err;
}

/*
 * Serialise a canonical RIFF/WAVE header declaring @p data_bytes of payload
 * into the @p len bytes at @p header.
 *
 * The byte layout belongs to the WAV module, which is also what the reader
 * parses with, so the two sides of a file round trip cannot drift apart. The
 * node only supplies the stream description - kept in one place here because
 * both open() and the finalise path have to describe the same stream.
 */
static int file_writer_build_header(const struct audio_file_writer_state *state,
				    uint32_t data_bytes, uint8_t *header, size_t len)
{
	const struct audio_wav_header hdr = {
		.sample_rate_hz = state->fmt.sample_rate_hz,
		.data_size = data_bytes,
		.format_tag = AUDIO_WAV_FORMAT_PCM,
		.channels = state->fmt.channels,
		.bits_per_sample = FILE_WRITER_BITS_PER_SAMPLE,
	};

	return audio_wav_write_header(header, len, &hdr);
}

/* All or nothing: a filesystem that accepts only part of the buffer is out of
 * space, and a half written sample frame is worse than a failed frame.
 */
static int file_writer_write_all(struct audio_file_writer_state *state, const void *data,
				 size_t len)
{
	ssize_t written = fs_write(&state->file, data, len);

	if (written < 0) {
		LOG_ERR("%s: write of %zu bytes failed (%d)", state->path, len, (int)written);
		return file_writer_errno((int)written);
	}

	if ((size_t)written != len) {
		LOG_ERR("%s: only %zd of %zu bytes written, out of space", state->path, written,
			len);
		return -ENOSPC;
	}

	return 0;
}

/*
 * Patch the RIFF and data chunk sizes to match what has actually been written
 * and leave the position at the end of the file, so appending can continue.
 *
 * Cheap to call repeatedly: without new payload since the last patch there is
 * nothing to do.
 */
static int file_writer_finalize(struct audio_file_writer_state *state)
{
	uint8_t header[AUDIO_WAV_MIN_HEADER_SIZE];
	int ret;

	if (!state->file_open || !state->header_stale) {
		return 0;
	}

	ret = file_writer_build_header(state, state->data_bytes, header, sizeof(header));
	if (ret < 0) {
		/* open() already serialised this very format, so the only way
		 * here is a payload the size fields cannot describe - which
		 * process() refuses to produce.
		 */
		LOG_ERR("%s: %u payload bytes cannot be described (%d)", state->path,
			state->data_bytes, ret);
		return ret;
	}

	ret = fs_seek(&state->file, 0, FS_SEEK_SET);
	if (ret < 0) {
		LOG_ERR("%s: seek to the header failed (%d)", state->path, ret);
		return file_writer_errno(ret);
	}

	ret = file_writer_write_all(state, header, sizeof(header));
	if (ret < 0) {
		return ret;
	}

	/* The sizes are the only thing that makes the file readable at all, so
	 * push them out rather than leaving them in a cache. A filesystem
	 * without sync support is not an error.
	 */
	ret = fs_sync(&state->file);
	if (ret < 0 && ret != -ENOTSUP) {
		LOG_ERR("%s: sync failed (%d)", state->path, ret);
		return file_writer_errno(ret);
	}

	ret = fs_seek(&state->file, 0, FS_SEEK_END);
	if (ret < 0) {
		LOG_ERR("%s: seek back to the end failed (%d)", state->path, ret);
		return file_writer_errno(ret);
	}

	state->header_stale = false;

	return 0;
}

/*
 * Finalise and drop the handle, leaving the node in a well-defined closed
 * state. Reports the first failure but always releases the handle: a failing
 * finalise must not strand the node half open, or close() could never recover.
 */
static int file_writer_release(struct audio_file_writer_state *state)
{
	int ret = 0;
	int err;

	if (state->file_open) {
		ret = file_writer_finalize(state);

		err = fs_close(&state->file);
		if (ret == 0) {
			ret = file_writer_errno(err);
		}

		state->file_open = false;
	}

	state->data_bytes = 0;
	state->header_stale = false;

	return ret;
}

/*
 * Narrow @p count canonical S32_LE container samples to little endian 16 bit
 * PCM (spec §5.3: the inverse of the source's s32 = s16 << 16).
 *
 * The rule is a plain arithmetic shift down by 16, i.e. the top 16 bits of the
 * container are kept:
 *
 *  - Rounding is truncation towards negative infinity, not round to nearest.
 *    Round to nearest would need a +0x8000 bias that overflows int32_t just
 *    below INT32_MAX and pushes full-scale positive out of the int16 range,
 *    which is exactly the asymmetry that makes naive narrowing buggy. Plain
 *    truncation is also the exact inverse of the reader's widening, so a
 *    file -> pipeline -> file round trip is bit identical.
 *  - No clipping is needed, and none is possible: a 32 bit value shifted down
 *    by 16 always lands inside [-32768, 32767]. INT32_MIN becomes -32768 and
 *    INT32_MAX becomes 32767, where a naive (int16_t) cast of the low half
 *    would have produced 0 and -1.
 *
 * The shift runs in the unsigned domain and the result is stored as raw bytes,
 * so the conversion has no implementation defined behaviour at all: the two's
 * complement bit pattern of an arithmetic >> 16 is the top half of the
 * container, whatever the host does with signed shifts.
 */
static void file_writer_narrow_s16(const int32_t *samples, size_t count, uint8_t *dst)
{
	size_t i;

	for (i = 0; i < count; i++) {
		sys_put_le16((uint16_t)((uint32_t)samples[i] >> 16),
			     &dst[i * FILE_WRITER_BYTES_PER_SAMPLE]);
	}
}

static int file_writer_open(struct audio_node *node)
{
	uint8_t header[AUDIO_WAV_MIN_HEADER_SIZE];
	struct audio_file_writer_state *state;
	int ret;

	if (!node) {
		return -EINVAL;
	}

	state = (struct audio_file_writer_state *)node->state;
	if (!state || !state->path || state->path[0] == '\0') {
		return -EINVAL;
	}

	/* Reopening without a close() must not leak the previous handle. */
	(void)file_writer_release(state);

	/* Resolve the defaults in place, so the format the node actually wrote
	 * stays observable after open() (spec §5.2).
	 */
	if (state->fmt.sample_rate_hz == 0U) {
		state->fmt.sample_rate_hz = FILE_WRITER_DEFAULT_RATE_HZ;
	}
	if (state->fmt.channels == 0U) {
		state->fmt.channels = FILE_WRITER_DEFAULT_CHANNELS;
	}
	if (state->fmt.valid_bits_per_sample == 0U) {
		state->fmt.valid_bits_per_sample = FILE_WRITER_BITS_PER_SAMPLE;
	}
	/* The container handed to process() is canonical by definition. */
	state->fmt.format = AUDIO_SAMPLE_FORMAT_S32_LE;

	if (state->fmt.valid_bits_per_sample != FILE_WRITER_BITS_PER_SAMPLE) {
		LOG_ERR("%s: %u bit output is not supported", state->path,
			state->fmt.valid_bits_per_sample);
		return -ENOTSUP;
	}

	if (state->fmt.channels > FILE_WRITER_MAX_CHANNELS) {
		LOG_ERR("%s: %u channels are outside the v1 range of 1..%u", state->path,
			state->fmt.channels, FILE_WRITER_MAX_CHANNELS);
		return -ENOTSUP;
	}

	/* Placeholder sizes: an empty but valid file until the stream ends.
	 * Serialised before the file is created, so a format the WAV module
	 * refuses leaves no truncated file behind at all.
	 */
	ret = file_writer_build_header(state, 0, header, sizeof(header));
	if (ret < 0) {
		LOG_ERR("%s: %u Hz, %u ch is not a writable WAVE format (%d)", state->path,
			state->fmt.sample_rate_hz, state->fmt.channels, ret);
		return ret;
	}

	fs_file_t_init(&state->file);

	/* FS_O_TRUNC: a shorter track must not leave the tail of an older one
	 * behind, where it would be counted by nothing but still occupy the
	 * file.
	 */
	ret = fs_open(&state->file, state->path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (ret < 0) {
		LOG_ERR("%s: create failed (%d)", state->path, ret);
		return file_writer_errno(ret);
	}

	state->file_open = true;
	state->data_bytes = 0;
	state->header_stale = false;

	ret = file_writer_write_all(state, header, sizeof(header));
	if (ret < 0) {
		(void)fs_close(&state->file);
		state->file_open = false;
		return ret;
	}

	LOG_INF("%s: %u Hz, %u ch, %u bit", state->path, state->fmt.sample_rate_hz,
		state->fmt.channels, FILE_WRITER_BITS_PER_SAMPLE);

	return 0;
}

static int file_writer_process(struct audio_node *node, struct audio_buffer_view *buf,
			       size_t *out_size)
{
	struct audio_file_writer_state *state;
	size_t produced = 0;
	size_t offset;
	size_t bytes;
	int ret;

	if (!node || !buf || !buf->data || !out_size) {
		return -EINVAL;
	}

	state = (struct audio_file_writer_state *)node->state;
	if (!state) {
		return -EINVAL;
	}

	*out_size = 0;
	buf->size = 0;

	if (!state->file_open) {
		/* process() before open(), or after close(). Reporting EOF here
		 * would silently swallow the whole track.
		 */
		LOG_ERR("process() on a closed writer");
		return -EBADF;
	}

	if (node->upstream) {
		ret = audio_node_process(node->upstream, buf, &produced);
		if (ret < 0) {
			/* Passed through, but never as -EPIPE: an upstream error
			 * must not reach the pipeline looking like end of data.
			 */
			return file_writer_errno(ret);
		}
	}

	if (produced == 0U) {
		/* End of stream (manifest §7): nothing to append. Finalise here
		 * as well as in close(), so an application that waits for the
		 * EOF event already has a valid file in hand.
		 */
		return file_writer_finalize(state);
	}

	/* An interleaved sample frame must never be split across two writes, or
	 * the channels would stay swapped for the rest of the file.
	 */
	if ((produced % state->fmt.channels) != 0U) {
		LOG_ERR("%s: %zu samples do not fill whole %u channel frames", state->path,
			produced, state->fmt.channels);
		return -EINVAL;
	}

	bytes = produced * FILE_WRITER_BYTES_PER_SAMPLE;
	if (bytes > (size_t)AUDIO_WAV_MAX_DATA_SIZE - state->data_bytes) {
		/* Both size fields are 32 bit, so this is as much as a WAV file
		 * can describe.
		 */
		LOG_ERR("%s: data chunk would exceed the 32 bit size field", state->path);
		return -EFBIG;
	}

	for (offset = 0; offset < produced; offset += AUDIO_FILE_WRITER_CHUNK_SAMPLES) {
		size_t chunk = MIN((size_t)AUDIO_FILE_WRITER_CHUNK_SAMPLES, produced - offset);

		file_writer_narrow_s16(&buf->data[offset], chunk, state->chunk);

		ret = file_writer_write_all(state, state->chunk,
					    chunk * FILE_WRITER_BYTES_PER_SAMPLE);
		if (ret < 0) {
			/* data_bytes only counts what the filesystem confirmed,
			 * so the header stays truthful about the payload even
			 * after a failed frame.
			 */
			return ret;
		}

		state->data_bytes += (uint32_t)(chunk * FILE_WRITER_BYTES_PER_SAMPLE);
		state->header_stale = true;
	}

	/* The sink consumed the frame; the pipeline only cares that it was not
	 * empty (spec §4.4).
	 */
	buf->size = produced;
	*out_size = produced;

	return 0;
}

static int file_writer_close(struct audio_node *node)
{
	struct audio_file_writer_state *state;

	if (!node) {
		return -EINVAL;
	}

	state = (struct audio_file_writer_state *)node->state;
	if (!state) {
		return -EINVAL;
	}

	return file_writer_release(state);
}

const struct audio_node_ops file_writer_node_ops = {
	.open = file_writer_open,
	.process = file_writer_process,
	.close = file_writer_close,
};
