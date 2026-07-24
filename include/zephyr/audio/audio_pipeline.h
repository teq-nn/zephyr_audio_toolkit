/*
 * Pull-based audio pipeline: public control API.
 *
 * See audio_pipeline_manifest.md §3/§7 and audio_pipeline_spec_v2.md §8.2/§9
 * for the binding lifecycle semantics implemented here.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_AUDIO_PIPELINE_H_
#define ZEPHYR_AUDIO_PIPELINE_H_

#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/types.h>

#include <zephyr/audio/audio_format.h>
#include <zephyr/audio/audio_node.h>
#include <zephyr/audio/audio_pipeline_events.h>

struct audio_pipeline_config {
	struct audio_stream_config stream;
	uint16_t frame_samples;
	audio_pipeline_event_callback_t event_cb;
	void *event_user_data;
};

/**
 * @brief Pipeline instance.
 *
 * The whole struct must be zero-initialised before audio_pipeline_init() is
 * called - static storage or `= {0}` for automatic storage. Everything except
 * the resource fields below is owned by the subsystem; do not touch it while
 * the worker thread is running.
 *
 * Worker thread and frame buffer resources may be supplied by the caller
 * (this is the seam AUDIO_PIPELINE_DEFINE() uses). They come as a unit:
 *
 *  - @p stack NULL  -> the subsystem's built-in stack, stack_size and
 *                      CONFIG_AUDIO_PIPELINE_THREAD_PRIO are installed by
 *                      audio_pipeline_init(). Only one pipeline instance can
 *                      run at a time in that case.
 *  - @p stack set   -> the caller also owns stack_size and priority.
 *
 * Likewise @p frame_buf NULL selects the built-in shared frame buffer, and
 * @p event_slots NULL the built-in shared event queue storage.
 */
struct audio_pipeline {
	/* Topology and configuration, both owned by the caller. */
	const struct audio_pipeline_config *config;
	struct audio_node *sink;

	/* Worker thread resources (see above). */
	struct k_thread thread;
	k_thread_stack_t *stack;
	size_t stack_size;
	int priority;

	/* Shared frame buffer handed down the chain, sized in samples. */
	int32_t *frame_buf;
	size_t frame_capacity;

	/* Event queue (see above): audio_pipeline_init() binds the ring buffer
	 * below to this k_msgq, which is what audio_pipeline_get_event() reads.
	 */
	struct k_msgq event_msgq;
	struct audio_pipeline_event *event_slots;
	size_t event_slot_count;

	/* Released whenever the worker must leave its idle wait. */
	struct k_sem wake;

	bool initialized;
	/* Written by the worker thread on the error path, read by the control
	 * thread, hence volatile - same for playing and quit.
	 */
	volatile bool nodes_open;
	/* Worker thread created and not yet returned; the worker clears it on
	 * the way out so is_running() reflects real liveness.
	 */
	volatile bool running;
	/* Worker thread is pulling frames. */
	volatile bool playing;
	/* Worker thread must return. */
	volatile bool quit;
};

/**
 * @brief Statically define a pipeline instance and all of its resources.
 *
 * File scope only. The macro allocates, per instance:
 *
 *  - the @ref audio_pipeline object (including its @c k_thread and @c k_msgq),
 *  - a dedicated worker thread stack of @p _stack_size bytes,
 *  - a dedicated frame buffer of @p _frame_samples @c int32_t samples,
 *  - dedicated event queue storage for
 *    ::AUDIO_PIPELINE_EVENT_QUEUE_DEPTH events,
 *
 * and ties them together, so two definitions never share storage and their
 * worker threads can run concurrently and raise events independently
 * (manifest §6/§8/§9, spec §6.2). The subsystem allocates nothing on top of
 * that and never calls @c k_malloc.
 *
 * The instance still has to be bound to a configuration and a sink with
 * audio_pipeline_init(); pass a configuration whose @c frame_samples equals
 * @p _frame_samples, because init() clamps the frame capacity to the smaller
 * of the two.
 *
 * The pipeline object itself is not static, so an instance defined in one file
 * can be reached from another through AUDIO_PIPELINE_DECLARE(); its stack and
 * frame buffer are private to the defining file.
 *
 * @param _name          Symbol name of the @ref audio_pipeline instance.
 * @param _frame_samples Frame buffer size in samples.
 * @param _stack_size    Worker thread stack size in bytes.
 * @param _priority      Worker thread priority.
 */
#define AUDIO_PIPELINE_DEFINE(_name, _frame_samples, _stack_size, _priority)               \
	static K_THREAD_STACK_DEFINE(_name##_stack, _stack_size);                          \
	static int32_t _name##_frame_buf[_frame_samples];                                  \
	static struct audio_pipeline_event                                                 \
		_name##_event_slots[AUDIO_PIPELINE_EVENT_QUEUE_DEPTH];                     \
	BUILD_ASSERT(ARRAY_SIZE(_name##_frame_buf) > 0,                                    \
		     "AUDIO_PIPELINE_DEFINE(" #_name "): frame_samples must be positive"); \
	struct audio_pipeline _name = {                                                    \
		.stack = _name##_stack,                                                    \
		.stack_size = K_THREAD_STACK_SIZEOF(_name##_stack),                        \
		.priority = (_priority),                                                   \
		.frame_buf = _name##_frame_buf,                                            \
		.frame_capacity = ARRAY_SIZE(_name##_frame_buf),                           \
		.event_slots = _name##_event_slots,                                        \
		.event_slot_count = ARRAY_SIZE(_name##_event_slots),                       \
	}

/** @brief Declare a pipeline defined with AUDIO_PIPELINE_DEFINE() elsewhere. */
#define AUDIO_PIPELINE_DECLARE(_name) extern struct audio_pipeline _name

/** @brief Check a configuration without binding it to a pipeline. */
bool audio_pipeline_config_is_valid(const struct audio_pipeline_config *config);

/**
 * @brief Bind configuration and topology to a zeroed pipeline instance.
 *
 * Does not touch the nodes and does not create the worker thread. The event
 * queue is (re)initialised here, so a rebound instance never hands the
 * application events left over from its previous life.
 *
 * @retval 0 on success
 * @retval -EINVAL on a NULL argument or an invalid configuration
 * @retval -EBUSY if the worker thread of this instance is still running
 */
int audio_pipeline_init(struct audio_pipeline *pipeline,
			const struct audio_pipeline_config *config,
			struct audio_node *sink);

/**
 * @brief Open the node chain and create the worker thread.
 *
 * Nodes are opened sink first, then walking @c upstream. If a node's open()
 * fails, the nodes already opened are closed again, an
 * ::AUDIO_PIPELINE_EVENT_ERROR is emitted and the failure is returned; no
 * thread is created. The worker starts out idle - use audio_pipeline_play()
 * to begin pulling frames.
 *
 * Idempotent: calling it on a started pipeline with an open chain returns 0.
 * After the error path closed the chain (spec §9.2) another start() reopens it
 * and reuses the existing thread.
 *
 * @retval 0 on success
 * @retval -EINVAL if the pipeline was not initialised
 * @retval -ELOOP if the upstream chain exceeds the supported depth
 * @retval <0 the first node open() error
 */
int audio_pipeline_start(struct audio_pipeline *pipeline);

/**
 * @brief Start (or resume) frame pulling on the worker thread.
 *
 * @retval 0 on success, also when already playing
 * @retval -EINVAL if the pipeline was not initialised
 * @retval -EPERM if no thread is running or the chain is not open
 */
int audio_pipeline_play(struct audio_pipeline *pipeline);

/**
 * @brief Halt frame pulling; the worker thread stays alive and idles.
 *
 * Asynchronous by design: the worker may still complete the frame in flight,
 * because a sink is allowed to block inside process() (spec §3.2) and stop()
 * must not deadlock behind it. Nodes stay open, so audio_pipeline_play() can
 * resume on the same thread.
 *
 * @retval 0 on success
 * @retval -EINVAL if the pipeline was not initialised
 */
int audio_pipeline_stop(struct audio_pipeline *pipeline);

/**
 * @brief Terminate the worker thread and close the node chain.
 *
 * Blocks until the thread has left its loop, so it must not be called from
 * the worker thread itself. Idempotent, and leaves the instance in the state
 * audio_pipeline_init() produced: audio_pipeline_start() can be called again.
 *
 * @retval 0 on success
 * @retval -EINVAL if the pipeline was not initialised
 * @retval <0 the first node close() error (also emitted as an ERROR event)
 */
int audio_pipeline_join(struct audio_pipeline *pipeline);

/** @brief True while the worker thread exists (created, not yet joined). */
bool audio_pipeline_is_running(const struct audio_pipeline *pipeline);

/** @brief True while the worker thread is pulling frames. */
bool audio_pipeline_is_playing(const struct audio_pipeline *pipeline);

/**
 * @brief Pull exactly one frame through the chain.
 *
 * Exposed for tests and for applications that want to drive the pipeline from
 * their own thread instead of using play()/stop().
 *
 * @retval 0 a frame was produced
 * @retval -EPIPE end of stream (the sink saw @c out_size == 0)
 * @retval <0 the node error that aborted the frame
 */
int audio_pipeline_process_frame(struct audio_pipeline *pipeline);

#endif /* ZEPHYR_AUDIO_PIPELINE_H_ */
