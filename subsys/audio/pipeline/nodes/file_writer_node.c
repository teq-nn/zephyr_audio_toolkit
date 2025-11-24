#include <errno.h>
#include <zephyr/audio/audio_node.h>

static int file_writer_open(struct audio_node *node)
{
	ARG_UNUSED(node);
	return 0;
}

static int file_writer_process(struct audio_node *node, struct audio_buffer_view *buf,
			       size_t *out_size)
{
	int ret;

	if (!node || !buf || !out_size) {
		return -EINVAL;
	}

	if (node->upstream) {
		ret = audio_node_process(node->upstream, buf, out_size);
		if (ret < 0) {
			return ret;
		}
	} else {
		*out_size = 0;
	}

	return 0;
}

static int file_writer_close(struct audio_node *node)
{
	ARG_UNUSED(node);
	return 0;
}

const struct audio_node_ops file_writer_node_ops = {
	.open = file_writer_open,
	.process = file_writer_process,
	.close = file_writer_close,
};
