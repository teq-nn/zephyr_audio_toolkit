#ifndef ZEPHYR_AUDIO_NODE_H_
#define ZEPHYR_AUDIO_NODE_H_

#include <zephyr/types.h>

/**
 * @brief The frame buffer a node is handed for the duration of one process()
 *        call.
 *
 * Describes the storage and nothing else: how much of it a call filled is
 * reported once, through the @c out_size out-parameter of
 * ::audio_node_ops.process.
 */
struct audio_buffer_view {
	/** Frame storage, owned by the pipeline. */
	int32_t *data;
	/** Samples @ref data can hold. */
	size_t capacity;
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
 * @brief Read one frame from @p node's upstream.
 *
 * The single implementation of the pull contract: every filter and every sink
 * reads its upstream through this function, passing *itself* as
 * @p node. Nothing else may invoke an upstream node's process op, or the three
 * decisions below would drift apart again from node to node.
 *
 * It owns:
 *  - the wiring policy - a filter or a sink without an upstream is a wiring
 *    error, reported as @c -ENOTSUP and never as an empty track, which would
 *    silently swallow the stream;
 *  - the reserved code - @c -EPIPE means "end of stream" to the pipeline, so a
 *    @c -EPIPE arriving from below is remapped to @c -EIO and a broken upstream
 *    can never reach the application looking like a finished one;
 *  - end of stream - @p out_size is @c 0 with a return of @c 0, forwarded
 *    verbatim, and @c 0 on every failure as well.
 *
 * When and how often a node pulls stays the node's own business: a resampler
 * may pull several times per frame, a mixer once per upstream.
 *
 * @param node     Node doing the pulling, i.e. the caller itself.
 * @param buf      Frame buffer handed to the upstream node.
 * @param out_size Samples the upstream node produced; @c 0 at end of stream.
 *
 * @retval 0 on success, end of stream included
 * @retval -EINVAL if @p node, @p buf or @p out_size is NULL
 * @retval -ENOTSUP if @p node has no upstream
 * @retval -errno as reported by the upstream node, never @c -EPIPE
 */
int audio_node_pull(struct audio_node *node, struct audio_buffer_view *buf,
		    size_t *out_size);

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
