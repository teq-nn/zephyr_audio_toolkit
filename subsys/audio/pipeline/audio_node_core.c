/*
 * Node dispatch plus the one implementation of the pull contract every filter
 * and sink reads its upstream through (spec §4.1.1).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/logging/log.h>

#include <zephyr/audio/audio_node.h>

#include "audio_internal.h"

LOG_MODULE_REGISTER(audio_node, LOG_LEVEL_INF);

int audio_eof_safe_errno(int err)
{
	if (err == -EPIPE) {
		LOG_WRN("remapping the reserved -EPIPE to -EIO");
		return -EIO;
	}

	return err;
}

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

int audio_node_pull(struct audio_node *node, struct audio_buffer_view *buf,
		    size_t *out_size)
{
	int ret;

	if (!node || !buf || !out_size) {
		return -EINVAL;
	}

	/* Set before anything can fail: a caller that only looks at out_size
	 * after an error must not see a stale sample count.
	 */
	*out_size = 0;

	if (!node->upstream) {
		/* Spec §4.3/§4.4: a filter and a sink have an upstream. A
		 * missing one is a wiring error, not an empty track.
		 */
		LOG_ERR("pull from a node that has no upstream");
		return -ENOTSUP;
	}

	ret = audio_node_process(node->upstream, buf, out_size);
	if (ret < 0) {
		*out_size = 0;
		return audio_eof_safe_errno(ret);
	}

	return 0;
}
