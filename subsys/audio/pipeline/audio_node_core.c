#include <errno.h>
#include <zephyr/audio/audio_node.h>

int audio_node_open(struct audio_node *node)
{
	if (!node || !node->ops || !node->ops->open) {
		return 0;
	}

	return node->ops->open(node);
}

int audio_node_process(struct audio_node *node, struct audio_buffer_view *buf,
		       size_t *out_size)
{
	if (!node || !node->ops || !node->ops->process) {
		return -ENOSYS;
	}

	if (!buf || !out_size) {
		return -EINVAL;
	}

	return node->ops->process(node, buf, out_size);
}

int audio_node_close(struct audio_node *node)
{
	if (!node || !node->ops || !node->ops->close) {
		return 0;
	}

	return node->ops->close(node);
}
