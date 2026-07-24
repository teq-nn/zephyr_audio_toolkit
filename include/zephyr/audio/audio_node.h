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

/**
 * @brief Statically define and wire an audio node.
 *
 * File scope only. The chain is wired at build time, so nodes have to be
 * defined in dataflow order - source first, sink last - and @p _upstream is
 * simply @c &previous_node (NULL for a source).
 *
 * The node object itself is not static, so a node defined in one file can be
 * reached from another through AUDIO_NODE_DECLARE(). Per-node state is
 * allocated by the node-specific macros in @c <zephyr/audio/audio_nodes.h>.
 *
 * @param _name     Symbol name of the @ref audio_node instance.
 * @param _role     Node role (::audio_node_role).
 * @param _ops      Pointer to the node's @ref audio_node_ops.
 * @param _upstream Pointer to the upstream node, NULL for a source.
 * @param _state    Pointer to the node's private state, NULL if it has none.
 */
#define AUDIO_NODE_DEFINE(_name, _role, _ops, _upstream, _state) \
	struct audio_node _name = {                              \
		.role = (_role),                                 \
		.ops = (_ops),                                   \
		.upstream = (_upstream),                         \
		.state = (_state),                               \
	}

/** @brief Declare a node defined with AUDIO_NODE_DEFINE() in another file. */
#define AUDIO_NODE_DECLARE(_name) extern struct audio_node _name

#endif /* ZEPHYR_AUDIO_NODE_H_ */
