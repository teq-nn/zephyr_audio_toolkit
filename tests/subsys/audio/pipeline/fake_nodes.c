/*
 * Implementation of the shared fake nodes. See fake_nodes.h for the contract.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <zephyr/audio/audio_node.h>

#include "fake_nodes.h"

/* -------------------------------------------------------------------------
 * Scripted source
 * ----------------------------------------------------------------------
 */

static int fake_source_open(struct audio_node *node)
{
	struct audio_fake_source *state = node->state;

	atomic_inc(&state->open_calls);
	/* Captured here rather than read from the node afterwards: the contract
	 * is that the format is installed *before* open() runs (spec §5.2), and
	 * only a snapshot taken inside open() can tell the two apart.
	 */
	atomic_ptr_set(&state->seen_format, (void *)node->pipeline_format);
	audio_fake_source_rewind(state);

	return state->open_ret;
}

static int fake_source_close(struct audio_node *node)
{
	struct audio_fake_source *state = node->state;

	atomic_inc(&state->close_calls);

	return state->close_ret;
}

/** Samples this call may deliver; 0 means the stream has ended. */
static size_t fake_source_available(const struct audio_fake_source *state, size_t capacity)
{
	size_t avail;

	if (state->samples) {
		avail = state->sample_count - (size_t)atomic_get(&state->samples_done);
	} else if ((size_t)atomic_get(&state->frames_done) >= state->frames_total) {
		avail = 0U;
	} else {
		avail = capacity;
	}

	avail = MIN(avail, capacity);

	if (state->chunk != 0U) {
		avail = MIN(avail, state->chunk);
	}

	return avail;
}

static int fake_source_process(struct audio_node *node, struct audio_buffer_view *buf,
			       size_t *out_size)
{
	struct audio_fake_source *state = node->state;
	size_t n;
	size_t i;

	if (state->fail_at_frame != 0U &&
	    (size_t)atomic_get(&state->frames_done) + 1U == state->fail_at_frame) {
		*out_size = 0;
		return state->process_ret;
	}

	n = fake_source_available(state, buf->capacity);
	if (n == 0U) {
		/* End of stream: out_size == 0 with a successful return. */
		*out_size = 0;
		return 0;
	}

	if (state->samples) {
		size_t pos = (size_t)atomic_get(&state->samples_done);

		memcpy(buf->data, &state->samples[pos], n * sizeof(int32_t));
		atomic_add(&state->samples_done, (atomic_val_t)n);
	} else {
		for (i = 0; i < n; i++) {
			buf->data[i] = state->pattern;
		}
	}

	*out_size = n;
	atomic_inc(&state->frames_done);

	/* Hand over to a peer chain in the middle of the frame, so two
	 * pipelines can be pinned to the same frame instead of only overlapping
	 * under lucky timing.
	 */
	if (state->frame_sem) {
		k_sem_give(state->frame_sem);
	}

	if (state->peer_sem && k_sem_take(state->peer_sem, AUDIO_FAKE_SEM_TIMEOUT) != 0) {
		atomic_inc(&state->peer_timeouts);
	}

	return 0;
}

const struct audio_node_ops audio_fake_source_ops = {
	.open = fake_source_open,
	.process = fake_source_process,
	.close = fake_source_close,
};

/* -------------------------------------------------------------------------
 * Counting sink (also used for pass-through filters)
 * ----------------------------------------------------------------------
 */

static int fake_sink_open(struct audio_node *node)
{
	struct audio_fake_sink *state = node->state;

	atomic_inc(&state->open_calls);
	/* See fake_source_open(): a snapshot taken inside open() is what proves
	 * the pipeline installed the format before it called the node.
	 */
	atomic_ptr_set(&state->seen_format, (void *)node->pipeline_format);

	return state->open_ret;
}

static int fake_sink_close(struct audio_node *node)
{
	struct audio_fake_sink *state = node->state;

	atomic_inc(&state->close_calls);

	return state->close_ret;
}

static int fake_sink_process(struct audio_node *node, struct audio_buffer_view *buf,
			     size_t *out_size)
{
	struct audio_fake_sink *state = node->state;
	size_t i;
	int ret;

	atomic_ptr_set(&state->worker, k_current_get());
	atomic_ptr_set(&state->seen_buf, buf->data);

	if (state->expect_capacity != 0U && buf->capacity != state->expect_capacity) {
		atomic_inc(&state->wrong_capacity);
	}

	if (state->fail_at_frame != 0U &&
	    (size_t)atomic_get(&state->frames_seen) + 1U == state->fail_at_frame) {
		*out_size = 0;
		return state->process_ret;
	}

	/* Through the one pull helper, exactly like a shipped node: a fake that
	 * called the upstream op itself could pass a chain the real nodes reject.
	 */
	ret = audio_node_pull(node, buf, out_size);
	if (ret < 0) {
		return ret;
	}

	if (*out_size == 0U) {
		atomic_inc(&state->eof_seen);
		return 0;
	}

	if (state->check_pattern) {
		for (i = 0; i < *out_size; i++) {
			if (buf->data[i] != state->expect_pattern) {
				atomic_inc(&state->corrupt_frames);
				break;
			}
		}
	}

	atomic_inc(&state->frames_seen);

	if (state->frame_sem) {
		k_sem_give(state->frame_sem);
	}

	if (state->gate_sem && k_sem_take(state->gate_sem, AUDIO_FAKE_SEM_TIMEOUT) != 0) {
		atomic_inc(&state->gate_timeouts);
	}

	return 0;
}

const struct audio_node_ops audio_fake_sink_ops = {
	.open = fake_sink_open,
	.process = fake_sink_process,
	.close = fake_sink_close,
};

/* -------------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------
 */

void audio_fake_source_reset(struct audio_fake_source *src)
{
	memset(src, 0, sizeof(*src));
}

void audio_fake_sink_reset(struct audio_fake_sink *sink)
{
	memset(sink, 0, sizeof(*sink));
}

void audio_fake_source_rewind(struct audio_fake_source *src)
{
	atomic_set(&src->frames_done, 0);
	atomic_set(&src->samples_done, 0);
}

size_t audio_test_read_file(const char *path, void *buf, size_t cap)
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
