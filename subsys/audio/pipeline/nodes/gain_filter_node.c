#include <errno.h>
#include <zephyr/audio/audio_node.h>

struct gain_filter_state {
	int32_t gain_q15;
};

static int gain_filter_open(struct audio_node *node)
{
	struct gain_filter_state *state = (struct gain_filter_state *)node->state;

	if (!state) {
		return -EINVAL;
	}

	if (state->gain_q15 == 0) {
		state->gain_q15 = 32768;
	}

	return 0;
}

static int gain_filter_process(struct audio_node *node, struct audio_buffer_view *buf,
			       size_t *out_size)
{
	struct gain_filter_state *state = (struct gain_filter_state *)node->state;
	size_t i;
	int ret;

	if (!state || !node || !buf || !out_size) {
		return -EINVAL;
	}

	if (!node->upstream) {
		return -ENOTSUP;
	}

	ret = audio_node_process(node->upstream, buf, out_size);
	if (ret < 0 || *out_size == 0) {
		return ret;
	}

	for (i = 0; i < *out_size; i++) {
		int64_t sample = buf->data[i];

		sample *= state->gain_q15;
		sample >>= 15;
		buf->data[i] = (int32_t)sample;
	}

	return 0;
}

static int gain_filter_close(struct audio_node *node)
{
	ARG_UNUSED(node);
	return 0;
}

const struct audio_node_ops gain_filter_node_ops = {
	.open = gain_filter_open,
	.process = gain_filter_process,
	.close = gain_filter_close,
};
