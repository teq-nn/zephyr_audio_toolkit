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
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/types.h>

#include <zephyr/audio/audio_format.h>
#include <zephyr/audio/audio_node.h>
#include <zephyr/audio/audio_pipeline_events.h>

/**
 * @brief Static configuration of a pipeline instance.
 *
 * Carries no format: the pipeline format is bound at run time with
 * audio_pipeline_set_format() and nowhere else, so there is never a second
 * place to look for the format a run is using (spec §5.2/§8.1).
 */
struct audio_pipeline_config {
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
 *                      audio_pipeline_init().
 *  - @p stack set   -> the caller also owns stack_size and priority.
 *
 * Likewise @p frame_buf NULL selects the built-in frame buffer, and
 * @p event_slots NULL the built-in event queue storage.
 *
 * There is exactly one of each built-in, so at most one instance may hold them:
 * audio_pipeline_init() and audio_pipeline_start() refuse a second claimant
 * with @c -EBUSY instead of letting two pipelines share one stack, one frame
 * buffer and one event queue. audio_pipeline_join() hands them back, so
 * init -> join -> init passes them on to the next instance. Two pipelines that
 * must run at the same time therefore need AUDIO_PIPELINE_DEFINE() (or their
 * own storage) for at least one of them.
 */
struct audio_pipeline {
	/* Topology and configuration, both owned by the caller. */
	const struct audio_pipeline_config *config;
	struct audio_node *sink;

	/* The one format this pipeline runs at, owned by the pipeline and
	 * installed on every node before that node is opened (spec §5.2).
	 * Written only by audio_pipeline_set_format(); @p format_bound stays
	 * false until it has been, which is what audio_pipeline_start() refuses
	 * on. audio_pipeline_init() clears both again.
	 */
	struct audio_stream_config format;
	bool format_bound;

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

	/* The one lifecycle state of this instance. It replaces the five
	 * booleans this struct used to carry - initialised, node chain open,
	 * worker thread alive, worker pulling - so that the combinations which
	 * were expressible but never reachable no longer exist: every one of
	 * those facts is now a function of this single value, and every legal
	 * move between values lives in one transition table in
	 * audio_pipeline_core.c.
	 *
	 * The values are enum audio_pipeline_state, private to the subsystem
	 * (subsys/audio/pipeline/audio_internal.h). Zero is "not initialised",
	 * which is what makes a zeroed instance uninitialised by construction.
	 * Read it from outside through audio_pipeline_is_running() and
	 * audio_pipeline_is_playing().
	 *
	 * @c atomic_t rather than a volatile bool: the worker thread writes it
	 * as well as the control thread, and @c volatile is neither a memory
	 * barrier nor a Zephyr cross-thread primitive.
	 */
	atomic_t state;

	/* Worker thread exit request, and deliberately *not* a state value.
	 *
	 * "Leave the loop" is a request rather than a place: it applies to
	 * every state that holds a worker thread, so folding it into the state
	 * would double the enum and reintroduce exactly the unreachable
	 * combinations the state above removes. Keeping it apart is also what
	 * lets the worker compare-and-swap its own moves without ever
	 * overwriting a pending audio_pipeline_join().
	 *
	 * Set only by audio_pipeline_join(), cleared by audio_pipeline_init()
	 * and by the audio_pipeline_start() that creates the thread.
	 */
	atomic_t quit_request;
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
 * For every resource field left NULL this claims the matching built-in (see
 * @ref audio_pipeline) and installs it. A refused claim writes nothing: a fresh
 * instance stays zeroed and uninitialised, and an instance that was joined out
 * of its built-ins keeps the configuration and pointers of its previous life
 * rather than being rebound to the new caller's.
 *
 * The bound format is cleared (spec §8.1): re-initialising rebinds the instance
 * to a new configuration and sink, and a format carried over from its previous
 * life would be a stale default nobody chose. audio_pipeline_start() reports
 * @c -ENODATA until audio_pipeline_set_format() is called again.
 *
 * @retval 0 on success
 * @retval -EINVAL on a NULL argument or an invalid configuration
 * @retval -EBUSY if the worker thread of this instance is still running, or if
 *         another instance holds a built-in resource this one needs
 */
int audio_pipeline_init(struct audio_pipeline *pipeline,
			const struct audio_pipeline_config *config,
			struct audio_node *sink);

/**
 * @brief Bind the one format this pipeline runs at.
 *
 * @p fmt is copied into pipeline-owned storage, so the caller's struct may be a
 * temporary. This is the *only* way to bind a format: sample rate and channel
 * count are pipeline-wide, declared top-down by the application, and no node
 * infers them from its peers (spec §5.2).
 *
 * audio_pipeline_start() installs the bound format on every node before it
 * opens that node, and a node that cannot deliver or accept it fails its open()
 * - v1 has no resampler and no channel mapper, so a mismatch is refused rather
 * than converted.
 *
 * Legal only while the node chain is closed: before the first
 * audio_pipeline_start(), or after audio_pipeline_join(). Nodes read the format
 * in open() and hold it until they are closed, so replacing it underneath an
 * open chain would leave them with a stale one.
 *
 * Control thread only, like every other @c audio_pipeline_* entry point
 * (spec §3.3). The worker thread never reads the bound format, which is why
 * this needs no lock.
 *
 * @param pipeline Initialised pipeline instance.
 * @param fmt      Format to copy in; @c sample_rate_hz and @c channels must be
 *                 non-zero.
 *
 * @retval 0 on success
 * @retval -EINVAL on a NULL argument, on a pipeline that is not initialised, or
 *         on a format no node could satisfy
 * @retval -EBUSY while the node chain is open, whether the pipeline is playing
 *         or merely idle
 */
int audio_pipeline_set_format(struct audio_pipeline *pipeline,
			      const struct audio_stream_config *fmt);

/**
 * @brief Open the node chain and create the worker thread.
 *
 * The bound format is installed on each node immediately before that node is
 * opened (spec §5.2), so a node validates it from @c audio_node.pipeline_format
 * inside its own open(). A pipeline with no bound format is refused with
 * @c -ENODATA before anything is claimed and before a thread exists - distinct
 * from the @c -EINVAL of a malformed configuration, so "you never called
 * audio_pipeline_set_format()" cannot be confused with "your configuration is
 * wrong".
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
 * An instance running on the built-in resources reclaims them here, because
 * audio_pipeline_join() released them. No ERROR event is published if that
 * fails - the event queue is one of the resources in question.
 *
 * @retval 0 on success
 * @retval -EINVAL if the pipeline was not initialised
 * @retval -ENODATA if no format was bound with audio_pipeline_set_format()
 * @retval -EBUSY if another instance has taken over a built-in resource this
 *         one was joined out of
 * @retval -ELOOP if the upstream chain exceeds the supported depth
 * @retval -ENOTSUP if a node cannot deliver or accept the bound format
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
 * Any built-in resource this instance holds is released, so the next
 * hand-rolled pipeline can claim it. The restart above then reclaims it, and
 * fails with @c -EBUSY if another instance got there first. Between the join
 * and that successful reclaim the instance owns nothing, even though it still
 * points at the built-ins: do not call audio_pipeline_process_frame() or
 * audio_pipeline_get_event() on it in that window, or it will read and write
 * storage the new owner is using. An instance running on the built-ins must
 * therefore be joined before it goes out of scope - nothing else gives them
 * back.
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
