/*
 * Pipeline lifecycle: worker thread, node open/close ordering, EOF and error
 * handling (spec §8.2 and §9, manifest §3 and §7).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/audio/audio_pipeline.h>
#include <zephyr/audio/audio_pipeline_events.h>

#include "audio_internal.h"

LOG_MODULE_REGISTER(audio_pipeline_core, LOG_LEVEL_INF);

/* Built-in resources for the single-instance case. A pipeline that brings its
 * own stack, frame buffer and event slots (AUDIO_PIPELINE_DEFINE) never touches
 * these.
 */
static K_THREAD_STACK_DEFINE(default_pipeline_stack, AUDIO_PIPELINE_STACK_SIZE);
static int32_t default_frame_buf[AUDIO_PIPELINE_MAX_FRAME_SAMPLES];
static struct audio_pipeline_event default_event_slots[AUDIO_PIPELINE_EVENT_QUEUE_DEPTH];

/* There is one of each, so at most one instance may hold them. Ownership is
 * tracked per resource - a pipeline that brings only its own stack still
 * contends for the frame buffer - and NULL means "free". Without this a second
 * zero-initialised pipeline would silently run on the first one's memory.
 *
 * An owner is only ever compared, never dereferenced, so an instance discarded
 * without audio_pipeline_join() leaves a stale identity rather than a dangling
 * read - but it also never gives its built-ins back. Joining before discarding
 * is part of the contract (see audio_pipeline.h).
 */
static struct audio_pipeline *default_stack_owner;
static struct audio_pipeline *default_frame_buf_owner;
static struct audio_pipeline *default_event_slots_owner;
static struct k_spinlock default_owner_lock;

/* True while @p owner is free or already @p pipeline's: re-claiming what an
 * instance holds has to succeed, otherwise start() could not follow init().
 */
static bool claimable_by(const struct audio_pipeline *owner,
			 const struct audio_pipeline *pipeline)
{
	return owner == NULL || owner == pipeline;
}

/*
 * Claim every built-in @p pipeline runs on - the ones it has no storage of its
 * own for, plus the ones an earlier claim already installed - and install the
 * missing ones.
 *
 * All three are checked before any of them is committed, so a refusal leaves
 * both the caller's instance and the current owner untouched.
 *
 * @retval 0 on success, also when the pipeline brings all of its own storage
 * @retval -EBUSY if another instance holds one of the built-ins
 */
static int pipeline_claim_defaults(struct audio_pipeline *pipeline)
{
	bool wants_stack = pipeline->stack == NULL || pipeline->stack == default_pipeline_stack;
	bool wants_frame_buf =
		pipeline->frame_buf == NULL || pipeline->frame_buf == default_frame_buf;
	bool wants_event_slots =
		pipeline->event_slots == NULL || pipeline->event_slots == default_event_slots;
	k_spinlock_key_t key = k_spin_lock(&default_owner_lock);

	if ((wants_stack && !claimable_by(default_stack_owner, pipeline)) ||
	    (wants_frame_buf && !claimable_by(default_frame_buf_owner, pipeline)) ||
	    (wants_event_slots && !claimable_by(default_event_slots_owner, pipeline))) {
		k_spin_unlock(&default_owner_lock, key);
		LOG_ERR("a built-in pipeline resource is already owned by another instance; "
			"define the second pipeline with AUDIO_PIPELINE_DEFINE()");
		return -EBUSY;
	}

	if (wants_stack) {
		default_stack_owner = pipeline;
	}

	if (wants_frame_buf) {
		default_frame_buf_owner = pipeline;
	}

	if (wants_event_slots) {
		default_event_slots_owner = pipeline;
	}

	k_spin_unlock(&default_owner_lock, key);

	/* Only ever installed into an empty field: a re-claim must not undo the
	 * frame capacity clamp init() applied on top of it.
	 *
	 * Thread resources come as a unit: either the caller supplied a stack
	 * (and owns size and priority with it) or the subsystem defaults apply.
	 */
	if (pipeline->stack == NULL) {
		pipeline->stack = default_pipeline_stack;
		pipeline->stack_size = K_THREAD_STACK_SIZEOF(default_pipeline_stack);
		pipeline->priority = AUDIO_PIPELINE_PRIORITY;
	}

	if (pipeline->frame_buf == NULL) {
		pipeline->frame_buf = default_frame_buf;
		pipeline->frame_capacity = ARRAY_SIZE(default_frame_buf);
	}

	if (pipeline->event_slots == NULL) {
		pipeline->event_slots = default_event_slots;
		pipeline->event_slot_count = ARRAY_SIZE(default_event_slots);
	}

	return 0;
}

/*
 * Give back whichever built-ins @p pipeline holds, so the next hand-rolled
 * instance can have them. The instance keeps pointing at them - a joined
 * pipeline is restartable - which is why audio_pipeline_start() claims again.
 */
static void pipeline_release_defaults(struct audio_pipeline *pipeline)
{
	k_spinlock_key_t key = k_spin_lock(&default_owner_lock);

	if (default_stack_owner == pipeline) {
		default_stack_owner = NULL;
	}

	if (default_frame_buf_owner == pipeline) {
		default_frame_buf_owner = NULL;
	}

	if (default_event_slots_owner == pipeline) {
		default_event_slots_owner = NULL;
	}

	k_spin_unlock(&default_owner_lock, key);
}

/*
 * Close the chain from @p first up to (excluding) @p end, walking upstream.
 * Passing NULL as @p end closes the whole chain. Returns the first error but
 * always visits every node, so a failing close() cannot leak the rest.
 */
static int pipeline_close_chain(struct audio_node *first, struct audio_node *end)
{
	struct audio_node *node = first;
	unsigned int depth = 0;
	int first_err = 0;
	int ret;

	while (node != NULL && node != end) {
		if (++depth > AUDIO_PIPELINE_MAX_CHAIN_DEPTH) {
			LOG_ERR("node chain deeper than %d, giving up on close",
				AUDIO_PIPELINE_MAX_CHAIN_DEPTH);
			return (first_err != 0) ? first_err : -ELOOP;
		}

		ret = audio_node_close(node);
		if (ret < 0 && first_err == 0) {
			first_err = ret;
		}

		node = node->upstream;
	}

	return first_err;
}

static int pipeline_close_nodes(struct audio_pipeline *pipeline)
{
	if (!pipeline->nodes_open) {
		return 0;
	}

	pipeline->nodes_open = false;

	return pipeline_close_chain(pipeline->sink, NULL);
}

/*
 * Open the chain sink first, then upstream, so a sink can hand resources to
 * the nodes feeding it. On failure everything opened so far is closed again.
 */
static int pipeline_open_nodes(struct audio_pipeline *pipeline)
{
	struct audio_node *node = pipeline->sink;
	unsigned int depth = 0;
	int ret;

	while (node != NULL) {
		if (++depth > AUDIO_PIPELINE_MAX_CHAIN_DEPTH) {
			LOG_ERR("node chain deeper than %d, refusing to open",
				AUDIO_PIPELINE_MAX_CHAIN_DEPTH);
			(void)pipeline_close_chain(pipeline->sink, node);
			return -ELOOP;
		}

		ret = audio_node_open(node);
		if (ret < 0) {
			LOG_ERR("node open failed (%d)", ret);
			(void)pipeline_close_chain(pipeline->sink, node);
			return ret;
		}

		node = node->upstream;
	}

	pipeline->nodes_open = true;

	return 0;
}

/*
 * The worker outlives EOF, stop() and node errors; only audio_pipeline_join()
 * makes it return (manifest §3, spec §3.1).
 */
static void pipeline_thread(void *p1, void *p2, void *p3)
{
	struct audio_pipeline *pipeline = (struct audio_pipeline *)p1;
	int ret;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (!pipeline->quit) {
		if (!pipeline->playing) {
			/* Idle instead of spinning; play() and join() both
			 * release the semaphore.
			 */
			(void)k_sem_take(&pipeline->wake, K_FOREVER);
			continue;
		}

		ret = audio_pipeline_process_frame(pipeline);
		if (ret == -EPIPE) {
			/* EOF: stop pulling but keep the nodes open so the next
			 * play() can run another track without a reopen.
			 */
			pipeline->playing = false;
			audio_pipeline_publish_event(pipeline, AUDIO_PIPELINE_EVENT_EOF, 0);
		} else if (ret < 0) {
			/* First error wins: quiesce the chain before telling the
			 * application, so the pipeline is fully stopped by the
			 * time the ERROR event is observed.
			 */
			pipeline->playing = false;
			(void)pipeline_close_nodes(pipeline);
			audio_pipeline_publish_event(pipeline, AUDIO_PIPELINE_EVENT_ERROR, ret);
		} else {
			k_yield();
		}
	}

	/* Cleared here rather than in audio_pipeline_join() so that
	 * audio_pipeline_is_running() reports the thread's real liveness. Only
	 * join() ever sets quit, so this cannot race with the control thread.
	 */
	pipeline->running = false;
}

int audio_pipeline_init(struct audio_pipeline *pipeline,
			const struct audio_pipeline_config *config,
			struct audio_node *sink)
{
	int ret;

	if (!pipeline || !config || !sink) {
		return -EINVAL;
	}

	if (!audio_pipeline_config_is_valid(config)) {
		return -EINVAL;
	}

	if (pipeline->initialized && pipeline->running) {
		return -EBUSY;
	}

	/* Before the first write to the instance, so a pipeline refused the
	 * built-ins is handed back untouched rather than half bound.
	 */
	ret = pipeline_claim_defaults(pipeline);
	if (ret < 0) {
		return ret;
	}

	pipeline->config = config;
	pipeline->sink = sink;

	/* The configured frame size governs how much of the buffer is used. */
	pipeline->frame_capacity = MIN(pipeline->frame_capacity, (size_t)config->frame_samples);

	audio_pipeline_event_queue_init(pipeline);

	pipeline->nodes_open = false;
	pipeline->running = false;
	pipeline->playing = false;
	pipeline->quit = false;
	pipeline->initialized = true;

	k_sem_init(&pipeline->wake, 0, 1);

	return 0;
}

int audio_pipeline_start(struct audio_pipeline *pipeline)
{
	int ret;

	if (!pipeline || !pipeline->initialized || !pipeline->sink) {
		return -EINVAL;
	}

	/* An instance that was joined gave its built-ins back, so take them again
	 * before touching them. Deliberately no ERROR event on the refusal: the
	 * event queue is one of the resources this pipeline no longer owns, and
	 * publishing into it would corrupt the new owner's queue.
	 */
	ret = pipeline_claim_defaults(pipeline);
	if (ret < 0) {
		return ret;
	}

	if (!pipeline->nodes_open) {
		ret = pipeline_open_nodes(pipeline);
		if (ret < 0) {
			audio_pipeline_publish_event(pipeline, AUDIO_PIPELINE_EVENT_ERROR, ret);
			return ret;
		}
	}

	if (pipeline->running) {
		return 0;
	}

	pipeline->playing = false;
	pipeline->quit = false;
	k_sem_init(&pipeline->wake, 0, 1);
	pipeline->running = true;

	k_thread_create(&pipeline->thread, pipeline->stack, pipeline->stack_size, pipeline_thread,
			pipeline, NULL, NULL, pipeline->priority, 0, K_NO_WAIT);
	k_thread_name_set(&pipeline->thread, "audio_pipeline");

	return 0;
}

int audio_pipeline_play(struct audio_pipeline *pipeline)
{
	if (!pipeline || !pipeline->initialized) {
		return -EINVAL;
	}

	if (!pipeline->running || !pipeline->nodes_open) {
		return -EPERM;
	}

	if (pipeline->playing) {
		return 0;
	}

	pipeline->playing = true;
	k_sem_give(&pipeline->wake);

	return 0;
}

int audio_pipeline_stop(struct audio_pipeline *pipeline)
{
	if (!pipeline || !pipeline->initialized) {
		return -EINVAL;
	}

	pipeline->playing = false;

	return 0;
}

int audio_pipeline_join(struct audio_pipeline *pipeline)
{
	int ret;

	if (!pipeline || !pipeline->initialized) {
		return -EINVAL;
	}

	if (pipeline->running) {
		pipeline->playing = false;
		pipeline->quit = true;
		k_sem_give(&pipeline->wake);

		(void)k_thread_join(&pipeline->thread, K_FOREVER);
	}

	pipeline->running = false;

	ret = pipeline_close_nodes(pipeline);
	if (ret < 0) {
		audio_pipeline_publish_event(pipeline, AUDIO_PIPELINE_EVENT_ERROR, ret);
	}

	/* Last, so the close error still reaches the event queue the instance was
	 * running on. From here the built-ins are up for grabs again.
	 */
	pipeline_release_defaults(pipeline);

	return ret;
}

bool audio_pipeline_is_running(const struct audio_pipeline *pipeline)
{
	return pipeline != NULL && pipeline->running;
}

bool audio_pipeline_is_playing(const struct audio_pipeline *pipeline)
{
	return pipeline != NULL && pipeline->playing;
}

int audio_pipeline_process_frame(struct audio_pipeline *pipeline)
{
	struct audio_buffer_view view;
	size_t produced = 0;
	int ret;

	if (!pipeline || !pipeline->sink || !pipeline->frame_buf) {
		return -EINVAL;
	}

	if (!pipeline->sink->ops || !pipeline->sink->ops->process) {
		return -ENOSYS;
	}

	view.data = pipeline->frame_buf;
	view.capacity = pipeline->frame_capacity;

	ret = audio_node_process(pipeline->sink, &view, &produced);
	if (ret < 0) {
		/* Only an empty frame ends the stream, never a failing sink:
		 * -EPIPE is this function's own EOF signal, so a sink reporting
		 * it would be read as a finished track (manifest §7).
		 */
		return audio_eof_safe_errno(ret);
	}

	if (produced == 0) {
		return -EPIPE;
	}

	return 0;
}
