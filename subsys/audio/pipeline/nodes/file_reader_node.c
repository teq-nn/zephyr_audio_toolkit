/*
 * File reader source node.
 *
 * open() opens the file through the Zephyr filesystem API, parses its
 * RIFF/WAVE header with the shared parser and seeks to the payload; process()
 * widens the 16 bit payload into the canonical S32_LE container and reports
 * end of data with out_size == 0; close() releases the handle
 * (manifest §2/§4/§7, spec §5.3/§10.1).
 *
 * All state lives in the per-instance ::audio_file_reader_state allocated by
 * AUDIO_FILE_READER_NODE_DEFINE(), so several readers can run side by side.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <zephyr/audio/audio_node.h>
#include <zephyr/audio/audio_nodes.h>
#include <zephyr/audio/audio_wav.h>

#include "../audio_internal.h"

LOG_MODULE_REGISTER(audio_file_reader, LOG_LEVEL_INF);

/* v1 converts 16 bit PCM; the parser also accepts 8, 24 and 32 bit files. */
#define FILE_READER_BITS_PER_SAMPLE 16U
#define FILE_READER_BYTES_PER_SAMPLE (FILE_READER_BITS_PER_SAMPLE / 8U)

/* Interleaving is pipeline-wide (spec §5.2); v1 is stereo, mono costs nothing. */
#define FILE_READER_MAX_CHANNELS 2U

/* Drop the handle, leaving the node in a well-defined closed state. */
static int file_reader_release(struct audio_file_reader_state *state)
{
	int ret = 0;

	if (state->file_open) {
		ret = fs_close(&state->file);
		/* The handle is gone either way: a failing fs_close() must not
		 * strand the node half open, or close() could never recover.
		 */
		state->file_open = false;
	}

	state->bytes_left = 0;

	return audio_eof_safe_errno(ret);
}

/*
 * Widen @p samples little endian 16 bit samples sitting at the front of @p buf
 * into the S32_LE container the rest of the pipeline works with
 * (spec §5.3: s32 = s16 << 16).
 *
 * Done in place, back to front, so the node needs no scratch buffer at all
 * (manifest §6): sample i is read from byte offset 2*i and written to byte
 * offset 4*i, and 2*i <= 4*i for every i, so a write can never clobber a
 * sample that has not been read yet.
 */
static void file_reader_widen_s16(int32_t *buf, size_t samples)
{
	const uint8_t *raw = (const uint8_t *)buf;
	size_t i = samples;

	while (i-- > 0U) {
		int16_t sample = (int16_t)sys_get_le16(&raw[i * FILE_READER_BYTES_PER_SAMPLE]);

		/* Shifted as unsigned on purpose: left-shifting a negative
		 * signed value is not defined by the C standard, while the
		 * two's complement result below is exactly what the spec asks
		 * for (-1 -> 0xffff0000, -32768 -> INT32_MIN).
		 */
		buf[i] = (int32_t)((uint32_t)(int32_t)sample << 16);
	}
}

static int file_reader_open(struct audio_node *node)
{
	uint8_t header[AUDIO_WAV_HEADER_SCAN_SIZE];
	const struct audio_stream_config *want;
	struct audio_file_reader_state *state;
	struct audio_wav_header wav;
	ssize_t read;
	int ret;

	if (!node) {
		return -EINVAL;
	}

	state = (struct audio_file_reader_state *)node->state;
	if (!state || !state->path || state->path[0] == '\0') {
		return -EINVAL;
	}

	/* Reopening without a close() must not leak the previous handle. */
	(void)file_reader_release(state);

	state->eof = false;
	memset(&state->fmt, 0, sizeof(state->fmt));

	fs_file_t_init(&state->file);

	ret = fs_open(&state->file, state->path, FS_O_READ);
	if (ret < 0) {
		LOG_ERR("%s: open failed (%d)", state->path, ret);
		return audio_eof_safe_errno(ret);
	}

	/* The parser walks the chunk list, so it only needs a prefix of the
	 * file - the payload may extend well past what is buffered here.
	 */
	read = fs_read(&state->file, header, sizeof(header));
	if (read < 0) {
		LOG_ERR("%s: header read failed (%d)", state->path, (int)read);
		ret = audio_eof_safe_errno((int)read);
		goto err_close;
	}

	ret = audio_wav_read_header(header, (size_t)read, &wav);
	if (ret < 0) {
		LOG_ERR("%s: not a usable WAVE file (%d)", state->path, ret);
		goto err_close;
	}

	if (wav.bits_per_sample != FILE_READER_BITS_PER_SAMPLE) {
		LOG_ERR("%s: %u bit PCM is not supported", state->path, wav.bits_per_sample);
		ret = -ENOTSUP;
		goto err_close;
	}

	/* The parser already rejects a zero channel count; re-checking it here
	 * keeps process() free of a division by zero no matter what.
	 */
	if (wav.channels == 0U || wav.channels > FILE_READER_MAX_CHANNELS) {
		LOG_ERR("%s: %u channels are outside the v1 range of 1..%u", state->path,
			wav.channels, FILE_READER_MAX_CHANNELS);
		ret = -ENOTSUP;
		goto err_close;
	}

	/* The container is always S32_LE; the on-disk depth survives as the
	 * effective resolution (spec §5.2). This is the file's *real* format and
	 * it is what the bound format is checked against below.
	 */
	state->fmt.sample_rate_hz = wav.sample_rate_hz;
	state->fmt.channels = (uint8_t)wav.channels;
	state->fmt.valid_bits_per_sample = (uint8_t)wav.bits_per_sample;
	state->fmt.format = AUDIO_SAMPLE_FORMAT_S32_LE;

	/* Nodes validate, they do not adapt (spec §5.2/§10.1): v1 has no
	 * resampler and no channel mapper, so a file whose rate or channel count
	 * disagrees with the pipeline can only be refused. Handing it over
	 * anyway is what produces a track playing at the wrong speed under a
	 * header that describes something else.
	 *
	 * A node opened outside a pipeline has no bound format to disagree with;
	 * the pipeline itself never opens one without it, because
	 * audio_pipeline_start() refuses an unbound pipeline with -ENODATA.
	 */
	want = node->pipeline_format;
	if (want != NULL && (state->fmt.sample_rate_hz != want->sample_rate_hz ||
			     state->fmt.channels != want->channels)) {
		LOG_ERR("%s: %u Hz, %u ch does not match the pipeline's %u Hz, %u ch", state->path,
			state->fmt.sample_rate_hz, state->fmt.channels, want->sample_rate_hz,
			want->channels);
		ret = -ENOTSUP;
		goto err_close;
	}

	ret = fs_seek(&state->file, (off_t)wav.data_offset, FS_SEEK_SET);
	if (ret < 0) {
		LOG_ERR("%s: seek to payload at %u failed (%d)", state->path, wav.data_offset, ret);
		ret = audio_eof_safe_errno(ret);
		goto err_close;
	}

	state->bytes_left = wav.data_size;
	state->file_open = true;

	LOG_INF("%s: %u Hz, %u ch, %u bit, %u payload bytes", state->path, wav.sample_rate_hz,
		wav.channels, wav.bits_per_sample, wav.data_size);

	return 0;

err_close:
	/* A refused file leaves no format behind either: state->fmt is only
	 * meaningful between a successful open() and its close().
	 */
	memset(&state->fmt, 0, sizeof(state->fmt));
	(void)fs_close(&state->file);

	return ret;
}

static int file_reader_process(struct audio_node *node, struct audio_buffer_view *buf,
			       size_t *out_size)
{
	struct audio_file_reader_state *state;
	size_t frame_bytes;
	size_t samples;
	size_t bytes;
	ssize_t read;

	if (!node || !buf || !buf->data || !out_size) {
		return -EINVAL;
	}

	state = (struct audio_file_reader_state *)node->state;
	if (!state) {
		return -EINVAL;
	}

	*out_size = 0;

	if (!state->file_open) {
		/* process() before open(), or after close(). */
		LOG_ERR("process() on a closed reader");
		return -EBADF;
	}

	if (state->eof) {
		return 0;
	}

	/* A buffer that cannot hold a single interleaved sample frame is a
	 * caller error, not end of data - reporting EOF here would silently
	 * truncate the track.
	 */
	if (buf->capacity < state->fmt.channels) {
		LOG_ERR("%s: buffer of %zu samples is too small for %u channels", state->path,
			buf->capacity, state->fmt.channels);
		return -EINVAL;
	}

	/* An interleaved sample frame must never straddle two pipeline frames,
	 * or every following frame would arrive with its channels swapped.
	 */
	frame_bytes = (size_t)state->fmt.channels * FILE_READER_BYTES_PER_SAMPLE;
	bytes = ROUND_DOWN(buf->capacity * FILE_READER_BYTES_PER_SAMPLE, frame_bytes);
	bytes = MIN(bytes, ROUND_DOWN((size_t)state->bytes_left, frame_bytes));

	if (bytes == 0U) {
		/* The declared payload is exhausted: EOF (manifest §7). */
		state->eof = true;
		return 0;
	}

	read = fs_read(&state->file, buf->data, bytes);
	if (read < 0) {
		LOG_ERR("%s: read failed (%d)", state->path, (int)read);
		return audio_eof_safe_errno((int)read);
	}

	state->bytes_left -= (uint32_t)read;

	/* data_size is what the header claims and nothing cross-checks it
	 * against the real file length, so a short read means the data ran out,
	 * not that something broke.
	 */
	if ((size_t)read < bytes) {
		LOG_INF("%s: file ends %zu bytes before the header promised", state->path,
			bytes - (size_t)read);
		state->eof = true;
	}

	/* A payload that stops mid sample frame has no usable tail. */
	samples = ROUND_DOWN((size_t)read, frame_bytes) / FILE_READER_BYTES_PER_SAMPLE;
	if (samples == 0U) {
		state->eof = true;
		return 0;
	}

	file_reader_widen_s16(buf->data, samples);

	*out_size = samples;

	return 0;
}

static int file_reader_close(struct audio_node *node)
{
	struct audio_file_reader_state *state;

	if (!node) {
		return -EINVAL;
	}

	state = (struct audio_file_reader_state *)node->state;
	if (!state) {
		return -EINVAL;
	}

	return file_reader_release(state);
}

const struct audio_node_ops file_reader_node_ops = {
	.open = file_reader_open,
	.process = file_reader_process,
	.close = file_reader_close,
};
