#include <errno.h>
#include <string.h>
#include <zephyr/sys/util.h>

#include <zephyr/audio/audio_node.h>

#include "../audio_internal.h"

static bool eof_sent;

static int file_reader_open(struct audio_node *node)
{
	ARG_UNUSED(node);
	eof_sent = false;
	return 0;
}

static int file_reader_process(struct audio_node *node, struct audio_buffer_view *buf,
			       size_t *out_size)
{
	size_t samples;

	ARG_UNUSED(node);

	if (!buf || !out_size) {
		return -EINVAL;
	}

	if (eof_sent) {
		*out_size = 0;
		return 0;
	}

	samples = MIN((size_t)AUDIO_PIPELINE_MAX_FRAME_SAMPLES, buf->capacity);
	memset(buf->data, 0, samples * sizeof(int32_t));
	*out_size = samples;
	eof_sent = true;

	return 0;
}

static int file_reader_close(struct audio_node *node)
{
	ARG_UNUSED(node);
	return 0;
}

const struct audio_node_ops file_reader_node_ops = {
	.open = file_reader_open,
	.process = file_reader_process,
	.close = file_reader_close,
};
