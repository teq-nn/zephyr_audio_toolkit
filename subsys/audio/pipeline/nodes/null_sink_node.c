#include <errno.h>
#include <zephyr/sys/util.h>

#include <zephyr/audio/audio_node.h>
#include <zephyr/audio/audio_nodes.h>

static int null_sink_open(struct audio_node *node)
{
	ARG_UNUSED(node);
	return 0;
}

static int null_sink_process(struct audio_node *node, struct audio_buffer_view *buf,
			     size_t *out_size)
{
	if (!node || !buf || !out_size) {
		return -EINVAL;
	}

	/* The whole node: pull a frame and drop it. */
	return audio_node_pull(node, buf, out_size);
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
