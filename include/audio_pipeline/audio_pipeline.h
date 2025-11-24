/*
 * Zephyr Audio Pipeline – public API
 *
 * First spin implementation closely aligned with the authored manifest and
 * spec. A lightweight host-usable compatibility layer is provided for the
 * Zephyr primitives used by the subsystem so the library and tests can run
 * without a full RTOS environment.
 */

#ifndef AUDIO_PIPELINE_AUDIO_PIPELINE_H_
#define AUDIO_PIPELINE_AUDIO_PIPELINE_H_

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_PIPELINE_CHANNELS	2
#define AUDIO_PIPELINE_EVENT_QUEUE_LEN	8

/* Minimal Zephyr-style timeout helpers for host execution. */
typedef int k_timeout_t;

#define K_NO_WAIT	(0)
#define K_FOREVER	(-1)

static inline k_timeout_t K_MSEC(int ms)
{
	return ms;
}

/* Lightweight k_msgq stand-in for host builds. */
struct k_msgq {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	char *buffer;
	size_t msg_size;
	size_t max_msgs;
	size_t read_idx;
	size_t write_idx;
	size_t count;
};

int k_msgq_init(struct k_msgq *q, char *buffer, size_t msg_size, size_t max_msgs);
int k_msgq_put(struct k_msgq *q, const void *data, k_timeout_t timeout);
int k_msgq_get(struct k_msgq *q, void *data, k_timeout_t timeout);

struct k_thread {
	pthread_t thread;
};

typedef char k_thread_stack_t;

enum audio_sample_format {
	AUDIO_SAMPLE_FORMAT_S32_LE,
};

struct audio_format {
	uint32_t sample_rate;
	uint8_t channels;
	uint8_t valid_bits_per_sample;
	enum audio_sample_format format;
};

enum audio_node_role {
	AUDIO_NODE_ROLE_SOURCE,
	AUDIO_NODE_ROLE_FILTER,
	AUDIO_NODE_ROLE_SINK,
};

struct audio_node;

struct audio_node_ops {
	int (*open)(struct audio_node *node);
	ssize_t (*process)(struct audio_node *node,
			   int32_t *buf,
			   size_t capacity,
			   size_t *out_size);
	int (*close)(struct audio_node *node);
};

struct audio_node {
	enum audio_node_role role;
	const struct audio_node_ops *ops;
	struct audio_node *upstream;
	void *context;
};

enum audio_pipeline_event_type {
	AUDIO_PIPELINE_EVENT_EOF,
	AUDIO_PIPELINE_EVENT_ERROR,
	AUDIO_PIPELINE_EVENT_RECONFIG,
};

struct audio_pipeline_event {
	enum audio_pipeline_event_type type;
	int error;
};

struct audio_pipeline {
	struct audio_node *source;
	struct audio_node *sink;
	struct audio_node **filters;
	size_t filter_count;

	struct audio_format format;

	struct k_thread thread;
	k_thread_stack_t *stack;
	size_t stack_size;
	int priority;

	int32_t *frame_buffer;
	size_t frame_capacity;

	struct k_msgq *event_queue;
	char *event_buffer;
	size_t event_queue_len;

	bool initialized;
	bool thread_started;
	bool running;
	bool playing;
	bool stop_request;

	pthread_mutex_t state_lock;
	pthread_cond_t state_cond;
};

#define AUDIO_PIPELINE_DEFINE(name, frame_samples, stack_sz, prio) 	static k_thread_stack_t name##_stack[(stack_sz)]; 	static int32_t name##_frame_buf[(frame_samples) * AUDIO_PIPELINE_CHANNELS]; 	static char name##_event_buf[AUDIO_PIPELINE_EVENT_QUEUE_LEN * sizeof(struct audio_pipeline_event)]; 	static struct k_msgq name##_event_q; 	struct audio_pipeline name = { 		.stack = name##_stack, 		.stack_size = (stack_sz), 		.priority = (prio), 		.frame_buffer = name##_frame_buf, 		.frame_capacity = (frame_samples) * AUDIO_PIPELINE_CHANNELS, 		.event_queue = &name##_event_q, 		.event_buffer = name##_event_buf, 		.event_queue_len = AUDIO_PIPELINE_EVENT_QUEUE_LEN, 	}

int audio_pipeline_init(struct audio_pipeline *pl);
int audio_pipeline_set_nodes(struct audio_pipeline *pl,
			     struct audio_node *source,
			     struct audio_node **filters,
			     size_t filter_count,
			     struct audio_node *sink);
int audio_pipeline_set_format(struct audio_pipeline *pl, const struct audio_format *fmt);
int audio_pipeline_start(struct audio_pipeline *pl);
int audio_pipeline_play(struct audio_pipeline *pl);
int audio_pipeline_stop(struct audio_pipeline *pl);
int audio_pipeline_join(struct audio_pipeline *pl);
int audio_pipeline_get_event(struct audio_pipeline *pl,
			  struct audio_pipeline_event *evt,
			  k_timeout_t timeout);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_PIPELINE_AUDIO_PIPELINE_H_ */
