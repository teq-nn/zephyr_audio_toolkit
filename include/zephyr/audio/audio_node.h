#ifndef ZEPHYR_AUDIO_NODE_H_
#define ZEPHYR_AUDIO_NODE_H_

#include <zephyr/types.h>

struct audio_buffer_view {
	int32_t *data;
	size_t capacity;
	size_t size;
};

enum audio_node_role {
	AUDIO_NODE_ROLE_SOURCE,
	AUDIO_NODE_ROLE_FILTER,
	AUDIO_NODE_ROLE_SINK,
};

struct audio_node;

struct audio_node_ops {
	int (*open)(struct audio_node *node);
	int (*process)(struct audio_node *node, struct audio_buffer_view *buf,
		       size_t *out_size);
	int (*close)(struct audio_node *node);
};

struct audio_node {
	enum audio_node_role role;
	const struct audio_node_ops *ops;
	struct audio_node *upstream;
	void *state;
};

int audio_node_open(struct audio_node *node);
int audio_node_process(struct audio_node *node, struct audio_buffer_view *buf,
		       size_t *out_size);
int audio_node_close(struct audio_node *node);

#endif /* ZEPHYR_AUDIO_NODE_H_ */
