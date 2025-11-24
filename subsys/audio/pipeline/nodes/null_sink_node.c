#include <errno.h>
#include <zephyr/audio/audio_node.h>

static int null_sink_open(struct audio_node *node)
{
	ARG_UNUSED(node);
	return 0;
}

static int null_sink_process(struct audio_node *node, struct audio_buffer_view *buf,
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

static int null_sink_close(struct audio_node *node)
{
	ARG_UNUSED(node);
	return 0;
}

const struct audio_node_ops null_sink_node_ops = {
	.open = null_sink_open,
	.process = null_sink_process,
	.close = null_sink_close,
};
