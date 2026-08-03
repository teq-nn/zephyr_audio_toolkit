/*
 * Pipeline lifecycle: worker thread, node open/close ordering, EOF and error
 * handling (spec §8.2 and §9, manifest §3 and §7).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zephyr/audio/audio_pipeline.h>
#include <zephyr/audio/audio_pipeline_events.h>

#include "audio_internal.h"

LOG_MODULE_REGISTER(audio_pipeline_core, LOG_LEVEL_INF);

/* What can be asked of the lifecycle state (::audio_pipeline_state). One per
 * entry point that moves it, plus the two moves the worker thread makes.
 */
enum pipeline_trigger {
	PIPELINE_TRIGGER_INIT,
	PIPELINE_TRIGGER_START,
	PIPELINE_TRIGGER_PLAY,
	PIPELINE_TRIGGER_STOP,
	/* The worker reached end of stream. */
	PIPELINE_TRIGGER_EOF,
	/* The node chain came down under a live worker thread. */
	PIPELINE_TRIGGER_CLOSE,
	PIPELINE_TRIGGER_JOIN,
};

/*
 * The lifecycle transition table: the one place a legal move is written down.
 *
 * A (trigger, from) pair that is not listed here is not a legal move, and
 * pipeline_transition() refuses it rather than storing a state nobody meant.
 * That is what keeps the illegal combinations of the five booleans this
 * replaced - playing without a worker thread, an open chain on an
 * uninitialised instance - from being expressible at all.
 *
 * Read it as the contract in spec §8.2; the guard clauses in the entry points
 * below only translate a refusal into the errno that entry point documents.
 */
static const struct {
	enum pipeline_trigger trigger;
	enum audio_pipeline_state from;
	enum audio_pipeline_state to;
} pipeline_transitions[] = {
	/* audio_pipeline_init() binds an instance that has no worker thread.
	 * One that has is refused - rebinding underneath a running worker is
	 * the -EBUSY the API documents.
	 */
	{ PIPELINE_TRIGGER_INIT, AUDIO_PIPELINE_STATE_UNINIT, AUDIO_PIPELINE_STATE_INIT },
	{ PIPELINE_TRIGGER_INIT, AUDIO_PIPELINE_STATE_INIT, AUDIO_PIPELINE_STATE_INIT },

	/* audio_pipeline_start() opens the chain, and from INIT creates the
	 * thread as well; from CLOSED it only reopens the chain, because the
	 * worker of the run a node error ended is still there. Both are states
	 * the worker never leaves on its own, which is what lets start() decide
	 * on one earlier read of them.
	 *
	 * There is no row from OPEN or PLAYING on purpose: start() has nothing
	 * left to do there and moves nothing, which is what makes it idempotent.
	 * Writing an identity row instead would let a start() racing a node
	 * error re-declare a chain the worker has just closed as open.
	 */
	{ PIPELINE_TRIGGER_START, AUDIO_PIPELINE_STATE_INIT, AUDIO_PIPELINE_STATE_OPEN },
	{ PIPELINE_TRIGGER_START, AUDIO_PIPELINE_STATE_CLOSED, AUDIO_PIPELINE_STATE_OPEN },

	/* audio_pipeline_play() needs an open chain under a live worker. INIT
	 * has no thread and CLOSED has no chain, so both are the -EPERM the
	 * API documents.
	 */
	{ PIPELINE_TRIGGER_PLAY, AUDIO_PIPELINE_STATE_OPEN, AUDIO_PIPELINE_STATE_PLAYING },
	{ PIPELINE_TRIGGER_PLAY, AUDIO_PIPELINE_STATE_PLAYING, AUDIO_PIPELINE_STATE_PLAYING },

	/* audio_pipeline_stop() is legal in every initialised state, which is
	 * why it returns 0 on a pipeline that was not playing.
	 */
	{ PIPELINE_TRIGGER_STOP, AUDIO_PIPELINE_STATE_PLAYING, AUDIO_PIPELINE_STATE_OPEN },
	{ PIPELINE_TRIGGER_STOP, AUDIO_PIPELINE_STATE_OPEN, AUDIO_PIPELINE_STATE_OPEN },
	{ PIPELINE_TRIGGER_STOP, AUDIO_PIPELINE_STATE_INIT, AUDIO_PIPELINE_STATE_INIT },
	{ PIPELINE_TRIGGER_STOP, AUDIO_PIPELINE_STATE_CLOSED, AUDIO_PIPELINE_STATE_CLOSED },

	/* End of stream stops the pulling and keeps the chain open, so the next
	 * play() can run another track without a reopen (manifest §7). Only
	 * from PLAYING: a stop() that got in first has already done it.
	 */
	{ PIPELINE_TRIGGER_EOF, AUDIO_PIPELINE_STATE_PLAYING, AUDIO_PIPELINE_STATE_OPEN },

	/* A node error takes the chain down under the live worker (spec §9.2).
	 * Listed from OPEN as well, because audio_pipeline_stop() may have run
	 * between the frame that failed and this move.
	 */
	{ PIPELINE_TRIGGER_CLOSE, AUDIO_PIPELINE_STATE_PLAYING, AUDIO_PIPELINE_STATE_CLOSED },
	{ PIPELINE_TRIGGER_CLOSE, AUDIO_PIPELINE_STATE_OPEN, AUDIO_PIPELINE_STATE_CLOSED },

	/* audio_pipeline_join() has waited for the worker to return by the time
	 * it applies this, so every state that could hold one ends up back at
	 * INIT. The from-state is what tells join() whether the chain is still
	 * open and has to be closed. INIT -> INIT is what makes join()
	 * idempotent.
	 */
	{ PIPELINE_TRIGGER_JOIN, AUDIO_PIPELINE_STATE_PLAYING, AUDIO_PIPELINE_STATE_INIT },
	{ PIPELINE_TRIGGER_JOIN, AUDIO_PIPELINE_STATE_OPEN, AUDIO_PIPELINE_STATE_INIT },
	{ PIPELINE_TRIGGER_JOIN, AUDIO_PIPELINE_STATE_CLOSED, AUDIO_PIPELINE_STATE_INIT },
	{ PIPELINE_TRIGGER_JOIN, AUDIO_PIPELINE_STATE_INIT, AUDIO_PIPELINE_STATE_INIT },
};

/*
 * Look (@p trigger, @p from) up in the transition table without applying it.
 *
 * @retval >=0 the ::audio_pipeline_state the move leads to
 * @retval -EPERM the move is not legal
 */
static int pipeline_transition_target(enum pipeline_trigger trigger,
				      enum audio_pipeline_state from)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(pipeline_transitions); i++) {
		if (pipeline_transitions[i].trigger == trigger &&
		    pipeline_transitions[i].from == from) {
			return (int)pipeline_transitions[i].to;
		}
	}

	return -EPERM;
}

/*
 * Apply @p trigger to @p pipeline's lifecycle state.
 *
 * The state is moved with a compare-and-swap rather than a plain store, and
 * that is the point of the exercise: the worker thread and the control thread
 * both move it, so a store would let one silently overwrite the other. When
 * audio_pipeline_stop() and the worker's end-of-stream move land together, the
 * loser of the CAS re-reads and looks the move up again against the state that
 * actually won - and finds it is no longer legal, instead of undoing it.
 *
 * @param from If non-NULL, receives the state the move started from.
 *
 * @retval 0 the state now holds the target of the move
 * @retval -EPERM @p trigger is not legal in the state @p pipeline was in
 */
static int pipeline_transition(struct audio_pipeline *pipeline, enum pipeline_trigger trigger,
			       enum audio_pipeline_state *from)
{
	enum audio_pipeline_state state;
	atomic_val_t target;

	for (;;) {
		state = audio_pipeline_state_get(pipeline);

		target = (atomic_val_t)pipeline_transition_target(trigger, state);
		if (target < 0) {
			return (int)target;
		}

		if (atomic_cas(&pipeline->state, (atomic_val_t)state, target)) {
			if (from != NULL) {
				*from = state;
			}

			return 0;
		}
	}
}

/* True in the states that hold a worker thread - what the "running" boolean
 * used to answer, and what audio_pipeline_is_running() reports.
 */
static bool pipeline_state_has_worker(enum audio_pipeline_state state)
{
	return state == AUDIO_PIPELINE_STATE_OPEN || state == AUDIO_PIPELINE_STATE_PLAYING ||
	       state == AUDIO_PIPELINE_STATE_CLOSED;
}

/* True in the states that hold an open node chain - what the "nodes_open"
 * boolean used to answer.
 */
static bool pipeline_state_has_open_chain(enum audio_pipeline_state state)
{
	return state == AUDIO_PIPELINE_STATE_OPEN || state == AUDIO_PIPELINE_STATE_PLAYING;
}

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
 * read. Two consequences are accepted rather than fixed, because both would
 * need the owner to be inspected - which is exactly what must not happen to a
 * pointer that may name an object that is gone (issue #20):
 *
 *  - An instance that is initialised and then abandoned holds its built-ins for
 *    the lifetime of the process, so every later hand-rolled instance gets
 *    -EBUSY. Joining before discarding is part of the contract (see
 *    audio_pipeline.h); there is no deinit, and reclaiming from a "dead" owner
 *    would mean asking that owner whether it is still alive.
 *  - Ownership is by pointer identity, so a new instance in the storage of a
 *    discarded one inherits its claim. It cannot corrupt anything: the
 *    inheriting instance re-initialises the ring and refreshes the epoch below
 *    on its own init(), and the instance it inherits from no longer exists.
 *
 * Neither is a corruption path; both are lockout/identity effects, and the
 * cheapest cure for the first one is the caller's own join().
 */
static struct audio_pipeline *default_stack_owner;
static struct audio_pipeline *default_frame_buf_owner;
static struct audio_pipeline *default_event_slots_owner;
static struct k_spinlock default_owner_lock;

/*
 * How often the built-in event slots have changed hands, which is what tells a
 * stale k_msgq binding from a live one (issue #20).
 *
 * Releasing the slots does not invalidate the binding of the instance that
 * released them - the ring still holds that instance's own events, which is why
 * the ERROR event audio_pipeline_join() publishes on a failing close() stays
 * readable. Handing them to *another* instance does: that instance calls
 * k_msgq_init() on the same storage, so head, tail and used_msgs are reset
 * behind the back of every other control block pointing at it. From that moment
 * the previous binding describes a ring that has moved on, and reading through
 * it would consume the new owner's events.
 *
 * Counted rather than derived from the owner pointer because "free again" is
 * not the same as "untouched": an instance may claim, run and release the slots
 * while the previous binding still exists.
 *
 * Written under default_owner_lock; audio_pipeline.event_slots_epoch carries
 * the value an instance was given when it claimed. Zero is no valid epoch - the
 * first claim bumps this to 1 - so a zero-initialised instance never looks
 * current by accident.
 */
static uint32_t default_event_slots_epoch;

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
		if (default_event_slots_owner != pipeline) {
			/* The slots change hands, so every binding to them that
			 * is not this one is stale from here on.
			 */
			default_event_slots_epoch++;
		}

		default_event_slots_owner = pipeline;
		pipeline->event_slots_epoch = default_event_slots_epoch;
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

bool audio_pipeline_event_queue_is_current(const struct audio_pipeline *pipeline)
{
	k_spinlock_key_t key;
	bool current;

	if (pipeline->event_slots != default_event_slots) {
		/* Caller-owned storage (AUDIO_PIPELINE_DEFINE) or nothing bound
		 * yet: no other instance can ever take it, so the binding holds
		 * for the life of the instance.
		 */
		return true;
	}

	key = k_spin_lock(&default_owner_lock);
	current = (pipeline->event_slots_epoch == default_event_slots_epoch);
	k_spin_unlock(&default_owner_lock, key);

	return current;
}

/*
 * Give back whichever built-ins @p pipeline holds, so the next hand-rolled
 * instance can have them. The instance keeps pointing at them - a joined
 * pipeline is restartable - which is why audio_pipeline_start() claims again.
 *
 * The event slot epoch is deliberately not bumped here: until another instance
 * claims them, the ring still holds nothing but this instance's own events, and
 * draining them after audio_pipeline_join() - including the ERROR event a
 * failing close() just published - stays legal.
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

/*
 * Take the node chain down under a live worker thread and record it in the
 * state, at most once.
 *
 * The state move comes first and doubles as the guard: only the caller that
 * wins the transition out of a chain-open state runs close(), so no node can
 * be closed twice. It is a CAS loop rather than a plain store because
 * audio_pipeline_stop() may move PLAYING -> OPEN while the failing frame is
 * still unwinding.
 */
static int pipeline_close_nodes(struct audio_pipeline *pipeline)
{
	if (pipeline_transition(pipeline, PIPELINE_TRIGGER_CLOSE, NULL) < 0) {
		return 0;
	}

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

		/* Top-down format binding (spec §5.2): every node is handed the
		 * pipeline's format immediately before it is opened, and the
		 * node validates it in open(). Installing it here rather than
		 * ahead of the walk keeps the two steps adjacent, so a node can
		 * never be opened without one.
		 */
		node->pipeline_format = &pipeline->format;

		ret = audio_node_open(node);
		if (ret < 0) {
			LOG_ERR("node open failed (%d)", ret);
			(void)pipeline_close_chain(pipeline->sink, node);
			return ret;
		}

		node = node->upstream;
	}

	/* Deliberately no state move here: audio_pipeline_start() makes it once
	 * the worker thread exists too, so the state never claims an open chain
	 * on a pipeline that has no thread to drive it.
	 */
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

	while (!atomic_get(&pipeline->quit_request)) {
		if (audio_pipeline_state_get(pipeline) != AUDIO_PIPELINE_STATE_PLAYING) {
			/* Idle instead of spinning; play() and join() both
			 * release the semaphore.
			 */
			(void)k_sem_take(&pipeline->wake, K_FOREVER);
			continue;
		}

		ret = audio_pipeline_process_frame(pipeline);
		if (ret == -EPIPE) {
			/* EOF: stop pulling but keep the nodes open so the next
			 * play() can run another track without a reopen. The
			 * move is refused if audio_pipeline_stop() already left
			 * PLAYING, and the event is published either way.
			 */
			(void)pipeline_transition(pipeline, PIPELINE_TRIGGER_EOF, NULL);
			audio_pipeline_publish_event(pipeline, AUDIO_PIPELINE_EVENT_EOF, 0);
		} else if (ret < 0) {
			/* First error wins: quiesce the chain before telling the
			 * application, so the pipeline is fully stopped by the
			 * time the ERROR event is observed.
			 */
			(void)pipeline_close_nodes(pipeline);
			audio_pipeline_publish_event(pipeline, AUDIO_PIPELINE_EVENT_ERROR, ret);
		} else {
			k_yield();
		}
	}

	/* Deliberately no state move on the way out. The state the worker
	 * leaves behind still says whether the node chain is open, and
	 * audio_pipeline_join() - the only writer of quit_request, and parked
	 * in k_thread_join() while this returns - needs that to decide whether
	 * to close it. join() makes the move to INIT once this thread is gone;
	 * spec §3.3 confines the API to one control thread, so no caller can
	 * observe the gap.
	 */
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

	/* Asked of the table rather than applied, because the claim below may
	 * still refuse and has to leave the instance untouched. The table has
	 * no INIT move out of the states that hold a worker thread, so an
	 * instance whose worker is still alive cannot be rebound.
	 */
	if (pipeline_transition_target(PIPELINE_TRIGGER_INIT,
				       audio_pipeline_state_get(pipeline)) < 0) {
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

	/* A rebind must not inherit the format of the instance's previous life:
	 * that would be a default nobody chose (spec §8.1). start() reports
	 * -ENODATA until audio_pipeline_set_format() is called again.
	 */
	memset(&pipeline->format, 0, sizeof(pipeline->format));
	pipeline->format_bound = false;

	atomic_clear(&pipeline->quit_request);
	k_sem_init(&pipeline->wake, 0, 1);

	/* Last, so the instance only counts as initialised once everything it
	 * needs is in place: this is the move that stops the lifecycle entry
	 * points and audio_pipeline_get_event() returning -EINVAL.
	 */
	(void)pipeline_transition(pipeline, PIPELINE_TRIGGER_INIT, NULL);

	return 0;
}

int audio_pipeline_set_format(struct audio_pipeline *pipeline,
			      const struct audio_stream_config *fmt)
{
	if (!pipeline || !fmt ||
	    audio_pipeline_state_get(pipeline) == AUDIO_PIPELINE_STATE_UNINIT) {
		return -EINVAL;
	}

	/* A format no node could ever satisfy is refused here rather than at the
	 * first open(), where it would look like a node defect (spec §8.1).
	 */
	if (fmt->sample_rate_hz == 0U || fmt->channels == 0U) {
		LOG_ERR("a pipeline format needs a sample rate and a channel count");
		return -EINVAL;
	}

	/* frame_samples counts TOTAL interleaved samples across all channels
	 * (issue #23), so this is where that count and the channel count first
	 * meet: a frame that cannot hold one interleaved sample set is not a
	 * short frame, it is a frame no node can put a single instant of audio
	 * into. CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES has a compile-time floor for
	 * the channel counts the shipped nodes accept; the exact test needs the
	 * bound format and therefore lives here.
	 */
	if (pipeline->frame_capacity < (size_t)fmt->channels) {
		LOG_ERR("a frame of %zu samples cannot hold one interleaved sample set for "
			"%u channels",
			pipeline->frame_capacity, fmt->channels);
		return -EINVAL;
	}

	/* Only while the chain is closed (spec §5.2): the nodes read the format
	 * in open() and hold it until they are closed, so swapping it underneath
	 * an open chain would leave them running on a stale one. "Not playing"
	 * would not be tight enough - nodes stay open across EOF and stop().
	 */
	if (pipeline_state_has_open_chain(audio_pipeline_state_get(pipeline))) {
		LOG_ERR("the node chain is open; join() before rebinding the format");
		return -EBUSY;
	}

	pipeline->format = *fmt;
	pipeline->format_bound = true;

	return 0;
}

int audio_pipeline_start(struct audio_pipeline *pipeline)
{
	enum audio_pipeline_state state;
	bool event_queue_current;
	bool create_thread;
	int ret;

	if (!pipeline || !pipeline->sink) {
		return -EINVAL;
	}

	/* One read of the state drives the whole call: nothing but this control
	 * thread can leave INIT or CLOSED, and from OPEN or PLAYING there is
	 * neither a chain to open nor a thread to create.
	 */
	state = audio_pipeline_state_get(pipeline);
	if (state == AUDIO_PIPELINE_STATE_UNINIT) {
		return -EINVAL;
	}

	/* Before anything is claimed and before a thread exists, so a pipeline
	 * nobody bound a format to leaves no trace at all. -ENODATA rather than
	 * -EINVAL keeps "you never called audio_pipeline_set_format()" apart
	 * from "your configuration is wrong" (spec §8.2).
	 */
	if (!pipeline->format_bound) {
		LOG_ERR("no pipeline format bound; call audio_pipeline_set_format() first");
		return -ENODATA;
	}

	/* Read before the claim below refreshes the answer: it says whether the
	 * event queue this instance carries still describes the storage it
	 * points at, or whether another instance held the built-in slots in the
	 * meantime and reset the ring through its own control block.
	 */
	event_queue_current = audio_pipeline_event_queue_is_current(pipeline);

	/* An instance that was joined gave its built-ins back, so take them again
	 * before touching them. Deliberately no ERROR event on the refusal: the
	 * event queue is one of the resources this pipeline no longer owns, and
	 * publishing into it would corrupt the new owner's queue.
	 */
	ret = pipeline_claim_defaults(pipeline);
	if (ret < 0) {
		return ret;
	}

	if (!event_queue_current) {
		/* The queue is bound in init(), not here, so a restart across
		 * another owner's life has to rebind rather than assume the old
		 * binding is still good (issue #20). Done as early as this - the
		 * first thing after the slots are ours again - so nothing can
		 * publish through the stale binding, and it purges the events the
		 * intervening owner left in the storage, which are not ours to
		 * deliver.
		 */
		audio_pipeline_event_queue_init(pipeline);
	}

	if (pipeline_state_has_open_chain(state)) {
		/* Already started: no chain to open and no thread to create, so
		 * the state is left exactly as it is. Touching it here would
		 * mean racing the worker for a value it owns while it runs.
		 */
		return 0;
	}

	ret = pipeline_open_nodes(pipeline);
	if (ret < 0) {
		/* The state was not moved, so the instance is still where it
		 * started: INIT with no thread, or CLOSED with the thread of the
		 * run a node error ended.
		 */
		audio_pipeline_publish_event(pipeline, AUDIO_PIPELINE_EVENT_ERROR, ret);
		return ret;
	}

	create_thread = !pipeline_state_has_worker(state);
	if (create_thread) {
		/* Both reset before the thread exists, so its first pass sees a
		 * fresh wake semaphore and no exit request left by an earlier
		 * audio_pipeline_join().
		 */
		atomic_clear(&pipeline->quit_request);
		k_sem_init(&pipeline->wake, 0, 1);
	}

	/* Before the thread is created, so the worker never reads a state that
	 * says there is no worker. The move cannot be lost: INIT and CLOSED are
	 * the two states the worker never leaves on its own.
	 */
	(void)pipeline_transition(pipeline, PIPELINE_TRIGGER_START, NULL);

	if (create_thread) {
		k_thread_create(&pipeline->thread, pipeline->stack, pipeline->stack_size,
				pipeline_thread, pipeline, NULL, NULL, pipeline->priority, 0,
				K_NO_WAIT);
		k_thread_name_set(&pipeline->thread, "audio_pipeline");
	}

	return 0;
}

int audio_pipeline_play(struct audio_pipeline *pipeline)
{
	enum audio_pipeline_state from = AUDIO_PIPELINE_STATE_UNINIT;

	if (!pipeline || audio_pipeline_state_get(pipeline) == AUDIO_PIPELINE_STATE_UNINIT) {
		return -EINVAL;
	}

	/* Only an open chain under a live worker can be played. INIT has no
	 * thread and CLOSED no chain, and the table refusing both is what
	 * produces the documented -EPERM.
	 */
	if (pipeline_transition(pipeline, PIPELINE_TRIGGER_PLAY, &from) < 0) {
		return -EPERM;
	}

	if (from == AUDIO_PIPELINE_STATE_OPEN) {
		/* It was idling on the semaphore. An instance that was already
		 * playing must not be handed a spare count, or the next idle
		 * wait returns at once.
		 */
		k_sem_give(&pipeline->wake);
	}

	return 0;
}

int audio_pipeline_stop(struct audio_pipeline *pipeline)
{
	if (!pipeline || audio_pipeline_state_get(pipeline) == AUDIO_PIPELINE_STATE_UNINIT) {
		return -EINVAL;
	}

	/* Legal in every initialised state, so a pipeline that was not playing
	 * still gets a 0 (spec §8.2). The worker may be mid-frame; stop() is
	 * asynchronous by contract and does not wait for it.
	 */
	(void)pipeline_transition(pipeline, PIPELINE_TRIGGER_STOP, NULL);

	return 0;
}

int audio_pipeline_join(struct audio_pipeline *pipeline)
{
	enum audio_pipeline_state state;
	/* Not open, so a refused move below closes nothing - the safe way for
	 * the fallback to be wrong. The table makes every initialised state a
	 * legal starting point, so it never is.
	 */
	enum audio_pipeline_state from = AUDIO_PIPELINE_STATE_UNINIT;
	int ret = 0;

	if (!pipeline) {
		return -EINVAL;
	}

	state = audio_pipeline_state_get(pipeline);
	if (state == AUDIO_PIPELINE_STATE_UNINIT) {
		return -EINVAL;
	}

	if (pipeline_state_has_worker(state)) {
		/* The exit request is not a state (see struct audio_pipeline):
		 * setting it cannot be undone by a move the worker makes on its
		 * way through this last frame.
		 */
		atomic_set(&pipeline->quit_request, 1);
		k_sem_give(&pipeline->wake);

		(void)k_thread_join(&pipeline->thread, K_FOREVER);
	}

	/* The worker has returned, so nothing races this move. It does both
	 * jobs at once: it takes the instance back to INIT, and its from-state
	 * says whether the chain is still open. After a node error the worker
	 * closed it already, and closing again would call close() on every node
	 * a second time.
	 */
	(void)pipeline_transition(pipeline, PIPELINE_TRIGGER_JOIN, &from);

	if (pipeline_state_has_open_chain(from)) {
		ret = pipeline_close_chain(pipeline->sink, NULL);
		if (ret < 0) {
			audio_pipeline_publish_event(pipeline, AUDIO_PIPELINE_EVENT_ERROR, ret);
		}
	}

	/* Last, so the close error still reaches the event queue the instance was
	 * running on. From here the built-ins are up for grabs again.
	 */
	pipeline_release_defaults(pipeline);

	return ret;
}

bool audio_pipeline_is_running(const struct audio_pipeline *pipeline)
{
	return pipeline != NULL && pipeline_state_has_worker(audio_pipeline_state_get(pipeline));
}

bool audio_pipeline_is_playing(const struct audio_pipeline *pipeline)
{
	return pipeline != NULL &&
	       audio_pipeline_state_get(pipeline) == AUDIO_PIPELINE_STATE_PLAYING;
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
