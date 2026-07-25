/*
 * The built-in stack, frame buffer and event slots a zero-initialised pipeline
 * falls back on (manifest §6/§8, spec §6.1/§8.1).
 *
 * There is exactly one of each in the subsystem, so at most one hand-rolled
 * instance can hold them. This suite pins the claim/release protocol that makes
 * that loud instead of silent: init() refuses a second claimant with -EBUSY,
 * join() hands the built-ins back, and start() will not pick up resources the
 * instance has since given away.
 *
 * The event queue needs the same care on the way *out*, because
 * audio_pipeline_get_event() is reachable long after join() gave the slots back
 * (issue #20): once another instance has claimed them the read is refused, and a
 * restart rebinds the queue instead of trusting the binding init() made.
 *
 * The two instances are deliberately distinguishable - different frame sizes,
 * different sample patterns, different frame counts - so a test cannot pass
 * merely because two pipelines running on one buffer happen to look alike.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include <zephyr/audio/audio_node.h>
#include <zephyr/audio/audio_pipeline.h>
#include <zephyr/audio/audio_pipeline_events.h>

#include "fake_nodes.h"

/* Different on purpose: a shared frame buffer would have to pick one of them,
 * and the sinks check the capacity they are handed.
 */
#define FIRST_FRAME_SAMPLES 32
#define SECOND_FRAME_SAMPLES 16

/* Likewise different, so a frame count cannot match by accident. */
#define FIRST_FRAMES 3U
#define SECOND_FRAMES 7U

#define FIRST_PATTERN 0x11111111
#define SECOND_PATTERN 0x22222222

/* Scripted open() failures, one per chain and different from each other, so an
 * ERROR event names the instance that produced it. Neither collides with a code
 * the event API returns on its own (-EINVAL, -EPERM, -ENOMSG, -EAGAIN).
 */
#define FIRST_OPEN_ERR (-EACCES)
#define SECOND_OPEN_ERR (-ENOSPC)

#define TEST_EVENT_TIMEOUT K_MSEC(2000)

AUDIO_FAKE_SOURCE_DEFINE(first_source);
AUDIO_FAKE_SINK_DEFINE(first_sink, &first_source);

AUDIO_FAKE_SOURCE_DEFINE(second_source);
AUDIO_FAKE_SINK_DEFINE(second_sink, &second_source);

/* Hand-rolled instances: no AUDIO_PIPELINE_DEFINE, so both want the built-ins. */
static struct audio_pipeline first_pipeline;
static struct audio_pipeline second_pipeline;

/* A third one for the per-resource test: it brings its own frame buffer and
 * event slots, so the built-in stack is the only thing it asks for.
 */
static struct audio_pipeline stack_only_pipeline;
static int32_t own_frame_buf[SECOND_FRAME_SAMPLES];
static struct audio_pipeline_event own_event_slots[AUDIO_PIPELINE_EVENT_QUEUE_DEPTH];

/* ... and the thread resources the same test hands to the first pipeline, so
 * that one asks for everything except the stack.
 */
static K_THREAD_STACK_DEFINE(own_stack, CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE);

static const struct audio_pipeline_config first_config = {
	.frame_samples = FIRST_FRAME_SAMPLES,
};

static const struct audio_pipeline_config second_config = {
	.frame_samples = SECOND_FRAME_SAMPLES,
};

/* The format every instance here runs at; the fakes accept anything, so the
 * suite only needs one. init() clears the binding (spec §8.1), so a successful
 * init() is always followed by a bind_format().
 */
static const struct audio_stream_config builtin_format = {
	.sample_rate_hz = 48000U,
	.channels = 2U,
	.valid_bits_per_sample = 24U,
	.format = AUDIO_SAMPLE_FORMAT_S32_LE,
};

static void bind_format(struct audio_pipeline *pipeline)
{
	zassert_equal(audio_pipeline_set_format(pipeline, &builtin_format), 0,
		      "binding the pipeline format failed");
}

/* Wipe one chain and give its source and sink a shared pattern, so a frame
 * written by the other chain cannot pass the sink's check.
 */
static void chain_reset(struct audio_fake_source *src, struct audio_fake_sink *sink,
			int32_t pattern, size_t expected_capacity)
{
	audio_fake_source_reset(src);
	audio_fake_sink_reset(sink);

	src->pattern = pattern;

	sink->check_pattern = true;
	sink->expect_pattern = pattern;
	sink->expect_capacity = expected_capacity;
}

static void builtin_resources_before(void *fixture)
{
	ARG_UNUSED(fixture);

	memset(&first_pipeline, 0, sizeof(first_pipeline));
	memset(&second_pipeline, 0, sizeof(second_pipeline));

	memset(&stack_only_pipeline, 0, sizeof(stack_only_pipeline));
	stack_only_pipeline.frame_buf = own_frame_buf;
	stack_only_pipeline.frame_capacity = ARRAY_SIZE(own_frame_buf);
	stack_only_pipeline.event_slots = own_event_slots;
	stack_only_pipeline.event_slot_count = ARRAY_SIZE(own_event_slots);

	chain_reset(&first_source_state, &first_sink_state, FIRST_PATTERN, FIRST_FRAME_SAMPLES);
	chain_reset(&second_source_state, &second_sink_state, SECOND_PATTERN,
		    SECOND_FRAME_SAMPLES);
}

static void builtin_resources_after(void *fixture)
{
	ARG_UNUSED(fixture);

	/* Both joins matter for the suites that follow: whichever instance ended
	 * up holding the built-ins has to hand them back here.
	 */
	(void)audio_pipeline_join(&first_pipeline);
	(void)audio_pipeline_join(&second_pipeline);
	(void)audio_pipeline_join(&stack_only_pipeline);
}

ZTEST(audio_pipeline_builtin_resources, test_builtin_resources_refuse_a_second_claimant)
{
	zassert_equal(audio_pipeline_init(&first_pipeline, &first_config, &first_sink), 0,
		      "the first hand-rolled pipeline was refused");
	bind_format(&first_pipeline);
	zassert_not_null(first_pipeline.stack, "no built-in stack installed");
	zassert_not_null(first_pipeline.frame_buf, "no built-in frame buffer installed");
	zassert_not_null(first_pipeline.event_slots, "no built-in event slots installed");

	zassert_equal(audio_pipeline_init(&second_pipeline, &second_config, &second_sink), -EBUSY,
		      "a second hand-rolled pipeline silently took the built-ins as well");

	/* A refused init() must leave its instance exactly as it found it, or the
	 * caller is left holding resources it does not own.
	 */
	zassert_is_null(second_pipeline.stack, "the refused instance kept a stack");
	zassert_is_null(second_pipeline.frame_buf, "the refused instance kept a frame buffer");
	zassert_is_null(second_pipeline.event_slots, "the refused instance kept event slots");
	/* Asked through the API rather than of a struct field: "not
	 * initialised" is a lifecycle state, and audio_pipeline_stop() refusing
	 * with -EINVAL is the only thing it means from outside. Every other
	 * initialised state answers 0.
	 */
	zassert_equal(audio_pipeline_stop(&second_pipeline), -EINVAL,
		      "the refused instance was marked initialised");
}

ZTEST(audio_pipeline_builtin_resources, test_builtin_resources_allow_the_owner_to_reinitialise)
{
	zassert_equal(audio_pipeline_init(&first_pipeline, &first_config, &first_sink), 0,
		      "the first hand-rolled pipeline was refused");
	bind_format(&first_pipeline);

	/* Rebinding an instance to a new configuration or sink is a supported
	 * move, so the owner must not lock itself out of its own built-ins.
	 */
	zassert_equal(audio_pipeline_init(&first_pipeline, &first_config, &first_sink), 0,
		      "the owner was refused the built-ins it already holds");
	bind_format(&first_pipeline);

	zassert_equal(audio_pipeline_init(&second_pipeline, &second_config, &second_sink), -EBUSY,
		      "a rebind released the built-ins to another instance");
}

ZTEST(audio_pipeline_builtin_resources, test_builtin_resources_are_claimed_per_resource)
{
	/* This one brings its own thread resources, so it asks for the built-in
	 * frame buffer and event slots but never for the built-in stack.
	 */
	first_pipeline.stack = own_stack;
	first_pipeline.stack_size = K_THREAD_STACK_SIZEOF(own_stack);
	first_pipeline.priority = CONFIG_AUDIO_PIPELINE_THREAD_PRIO;

	zassert_equal(audio_pipeline_init(&first_pipeline, &first_config, &first_sink), 0,
		      "the first hand-rolled pipeline was refused");
	bind_format(&first_pipeline);
	zassert_equal_ptr(first_pipeline.stack, own_stack, "init() replaced the caller's stack");

	/* Wants all three; two of them are taken, so it is refused. */
	zassert_equal(audio_pipeline_init(&second_pipeline, &second_config, &second_sink), -EBUSY,
		      "the built-in frame buffer was handed out twice");

	/* Wants only the built-in stack, which nobody claimed - so it gets it.
	 * A guard tracking one flag for all three would refuse this.
	 */
	zassert_equal(audio_pipeline_init(&stack_only_pipeline, &second_config, &second_sink), 0,
		      "an unclaimed built-in was refused because another one was taken");
	bind_format(&stack_only_pipeline);
	zassert_not_null(stack_only_pipeline.stack, "no built-in stack installed");
	zassert_true(stack_only_pipeline.stack != own_stack,
		     "the caller's own stack was handed to another instance");
	zassert_equal_ptr(stack_only_pipeline.frame_buf, own_frame_buf,
			  "init() replaced the caller's own frame buffer");
	zassert_equal_ptr(stack_only_pipeline.event_slots, own_event_slots,
			  "init() replaced the caller's own event slots");
}

ZTEST(audio_pipeline_builtin_resources, test_builtin_resources_keep_the_first_claimant_working)
{
	struct audio_pipeline_event event;
	const int32_t *frame_buf;
	const struct audio_pipeline_event *slots;

	first_source_state.frames_total = FIRST_FRAMES;
	second_source_state.frames_total = SECOND_FRAMES;

	zassert_equal(audio_pipeline_init(&first_pipeline, &first_config, &first_sink), 0,
		      "the first hand-rolled pipeline was refused");
	bind_format(&first_pipeline);
	frame_buf = first_pipeline.frame_buf;
	slots = first_pipeline.event_slots;

	zassert_equal(audio_pipeline_init(&second_pipeline, &second_config, &second_sink), -EBUSY,
		      "a second hand-rolled pipeline silently took the built-ins as well");

	/* The refusal must not have moved anything out from under the owner. */
	zassert_equal_ptr(first_pipeline.frame_buf, frame_buf, "the owner lost its frame buffer");
	zassert_equal_ptr(first_pipeline.event_slots, slots, "the owner lost its event slots");
	zassert_equal(first_pipeline.frame_capacity, FIRST_FRAME_SAMPLES,
		      "the owner was resized to the refused pipeline's frame");

	/* And it still plays a whole track, on its own buffer and at its own
	 * frame size - the sink rejects both the other chain's pattern and the
	 * other chain's capacity.
	 */
	zassert_equal(audio_pipeline_start(&first_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&first_pipeline), 0, "play failed");

	zassert_equal(audio_pipeline_get_event(&first_pipeline, &event, TEST_EVENT_TIMEOUT), 0,
		      "no EOF event within the timeout");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_EOF, "expected an EOF event, got type %d",
		      (int)event.type);
	zassert_equal(event.err, 0, "the EOF event carries an error");
	zassert_equal(audio_pipeline_get_event(&first_pipeline, &event, K_NO_WAIT), -ENOMSG,
		      "a second instance published into the built-in event queue");

	zassert_equal(atomic_get(&first_sink_state.frames_seen), FIRST_FRAMES,
		      "the owner lost frames");
	zassert_equal(atomic_get(&first_sink_state.corrupt_frames), 0U,
		      "something else wrote into the owner's frames");
	zassert_equal(atomic_get(&first_sink_state.wrong_capacity), 0U,
		      "the owner ran at the wrong frame capacity");
	zassert_equal_ptr(atomic_ptr_get(&first_sink_state.seen_buf), frame_buf,
			  "the owner's chain ran on a different frame buffer");

	/* The refused chain was never opened, let alone run. */
	zassert_equal(atomic_get(&second_source_state.open_calls), 0,
		      "the refused pipeline opened its chain");
	zassert_equal(atomic_get(&second_sink_state.frames_seen), 0U,
		      "the refused pipeline processed frames");
}

ZTEST(audio_pipeline_builtin_resources, test_builtin_resources_return_to_the_pool_on_join)
{
	struct audio_pipeline_event event;
	const k_thread_stack_t *stack;
	const int32_t *frame_buf;
	const struct audio_pipeline_event *slots;

	second_source_state.frames_total = SECOND_FRAMES;

	zassert_equal(audio_pipeline_init(&first_pipeline, &first_config, &first_sink), 0,
		      "the first hand-rolled pipeline was refused");
	bind_format(&first_pipeline);
	stack = first_pipeline.stack;
	frame_buf = first_pipeline.frame_buf;
	slots = first_pipeline.event_slots;

	zassert_equal(audio_pipeline_start(&first_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_join(&first_pipeline), 0, "join failed");

	/* Sequential reuse: the next hand-rolled instance gets the very same
	 * three objects the joined one gave back.
	 */
	zassert_equal(audio_pipeline_init(&second_pipeline, &second_config, &second_sink), 0,
		      "join() did not release the built-ins");
	bind_format(&second_pipeline);
	zassert_equal_ptr(second_pipeline.stack, stack, "a second built-in stack appeared");
	zassert_equal_ptr(second_pipeline.frame_buf, frame_buf,
			  "a second built-in frame buffer appeared");
	zassert_equal_ptr(second_pipeline.event_slots, slots,
			  "a second set of built-in event slots appeared");
	zassert_equal(second_pipeline.frame_capacity, SECOND_FRAME_SAMPLES,
		      "the new owner inherited the previous frame capacity");

	/* The instance that handed them back must not quietly pick them up
	 * again, and must not publish into the new owner's queue while failing.
	 */
	zassert_equal(audio_pipeline_start(&first_pipeline), -EBUSY,
		      "a released pipeline restarted on resources it no longer owns");
	zassert_false(audio_pipeline_is_running(&first_pipeline),
		      "the refused restart created a worker thread");
	zassert_equal(atomic_get(&first_sink_state.open_calls), 1,
		      "the refused restart reopened the chain");

	/* Meanwhile the new owner runs its own track undisturbed. */
	zassert_equal(audio_pipeline_start(&second_pipeline), 0, "start of the new owner failed");
	zassert_equal(audio_pipeline_play(&second_pipeline), 0, "play of the new owner failed");

	zassert_equal(audio_pipeline_get_event(&second_pipeline, &event, TEST_EVENT_TIMEOUT), 0,
		      "no EOF event within the timeout");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_EOF, "expected an EOF event, got type %d",
		      (int)event.type);
	zassert_equal(event.err, 0, "the refused restart leaked an error into this queue");

	zassert_equal(atomic_get(&second_sink_state.frames_seen), SECOND_FRAMES,
		      "the new owner lost frames");
	zassert_equal(atomic_get(&second_sink_state.corrupt_frames), 0U,
		      "something else wrote into the new owner's frames");
	zassert_equal(atomic_get(&second_sink_state.wrong_capacity), 0U,
		      "the new owner ran at the previous owner's frame capacity");
}

ZTEST(audio_pipeline_builtin_resources, test_builtin_resources_refuse_an_event_read_after_takeover)
{
	struct audio_pipeline_event event = {0};
	int ret;

	first_source_state.frames_total = FIRST_FRAMES;

	/* The first instance runs a whole track and drains its own queue, so
	 * anything it reads from here on can only have come from somewhere else.
	 */
	zassert_equal(audio_pipeline_init(&first_pipeline, &first_config, &first_sink), 0,
		      "the first hand-rolled pipeline was refused");
	bind_format(&first_pipeline);
	zassert_equal(audio_pipeline_start(&first_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&first_pipeline), 0, "play failed");

	zassert_equal(audio_pipeline_get_event(&first_pipeline, &event, TEST_EVENT_TIMEOUT), 0,
		      "no EOF event within the timeout");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_EOF, "expected an EOF event, got type %d",
		      (int)event.type);
	zassert_equal(audio_pipeline_get_event(&first_pipeline, &event, K_NO_WAIT), -ENOMSG,
		      "the first pipeline's queue was not empty");

	zassert_equal(audio_pipeline_join(&first_pipeline), 0, "join failed");

	/* Still readable while the slots are merely free: they hold nothing but
	 * this instance's own events, which is what makes draining after a join
	 * - the ERROR event of a failing close() included - a legal pattern.
	 */
	zassert_equal(audio_pipeline_get_event(&first_pipeline, &event, K_NO_WAIT), -ENOMSG,
		      "a joined pipeline lost its queue before anyone else claimed it");

	/* The second instance takes the very storage the first one still points
	 * at and leaves an event only it could have produced: an ERROR carrying
	 * its own open() failure, not the EOF the first chain publishes.
	 */
	second_source_state.open_ret = SECOND_OPEN_ERR;

	zassert_equal(audio_pipeline_init(&second_pipeline, &second_config, &second_sink), 0,
		      "join() did not release the built-in event slots");
	bind_format(&second_pipeline);
	zassert_equal_ptr(second_pipeline.event_slots, first_pipeline.event_slots,
			  "the two instances did not end up on the same event storage");
	zassert_equal(audio_pipeline_start(&second_pipeline), SECOND_OPEN_ERR,
		      "the scripted open() failure did not reach start()");

	/* Reading the joined instance must not reach the new owner's ring ... */
	ret = audio_pipeline_get_event(&first_pipeline, &event, K_NO_WAIT);
	zassert_equal(ret, -EPERM,
		      "a joined pipeline read the queue's new owner: got %d (type %d, err %d)", ret,
		      (int)event.type, event.err);

	/* ... and must not have consumed from it either, which is the half a
	 * plain "return an error" guard would miss.
	 */
	zassert_equal(audio_pipeline_get_event(&second_pipeline, &event, K_NO_WAIT), 0,
		      "the new owner's event went missing");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_ERROR,
		      "expected an ERROR event, got type %d", (int)event.type);
	zassert_equal(event.err, SECOND_OPEN_ERR, "the new owner's event carries err %d",
		      event.err);
}

ZTEST(audio_pipeline_builtin_resources, test_builtin_resources_rebind_the_event_queue_on_restart)
{
	struct audio_pipeline_event event = {0};
	int ret;

	first_source_state.frames_total = FIRST_FRAMES;

	zassert_equal(audio_pipeline_init(&first_pipeline, &first_config, &first_sink), 0,
		      "the first hand-rolled pipeline was refused");
	bind_format(&first_pipeline);

	/* Leave an event of this instance's own in the queue and never drain it:
	 * a control block that survives a takeover would deliver whatever sits
	 * at its stale read position after the next owner has reset the ring.
	 */
	first_source_state.open_ret = FIRST_OPEN_ERR;
	zassert_equal(audio_pipeline_start(&first_pipeline), FIRST_OPEN_ERR,
		      "the scripted open() failure did not reach start()");
	first_source_state.open_ret = 0;

	zassert_equal(audio_pipeline_join(&first_pipeline), 0, "join failed");

	/* The second instance claims the slots in between, re-initialises the
	 * ring through a control block of its own, leaves a distinguishable
	 * event behind and gives the slots back again.
	 */
	second_source_state.open_ret = SECOND_OPEN_ERR;

	zassert_equal(audio_pipeline_init(&second_pipeline, &second_config, &second_sink), 0,
		      "join() did not release the built-in event slots");
	bind_format(&second_pipeline);
	zassert_equal(audio_pipeline_start(&second_pipeline), SECOND_OPEN_ERR,
		      "the scripted open() failure did not reach start()");
	zassert_equal(audio_pipeline_join(&second_pipeline), 0, "join of the new owner failed");

	/* init -> start -> join -> start: the restart takes the built-ins back
	 * and must rebind the queue, because init() bound it a whole ownership
	 * ago.
	 */
	zassert_equal(audio_pipeline_start(&first_pipeline), 0, "the restart was refused");

	ret = audio_pipeline_get_event(&first_pipeline, &event, K_NO_WAIT);
	zassert_equal(ret, -ENOMSG,
		      "the restarted pipeline inherited a queued event: got %d (type %d, err %d)",
		      ret, (int)event.type, event.err);

	/* And the rebound queue still works - a restart that disabled the queue
	 * instead of rebinding it would stop here.
	 */
	zassert_equal(audio_pipeline_play(&first_pipeline), 0, "play failed");
	zassert_equal(audio_pipeline_get_event(&first_pipeline, &event, TEST_EVENT_TIMEOUT), 0,
		      "no EOF event within the timeout after the restart");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_EOF, "expected an EOF event, got type %d",
		      (int)event.type);
	zassert_equal(event.err, 0, "the EOF event carries an error");

	zassert_equal(atomic_get(&first_sink_state.frames_seen), FIRST_FRAMES,
		      "the restarted pipeline lost frames");
	zassert_equal(atomic_get(&first_sink_state.corrupt_frames), 0U,
		      "something else wrote into the restarted pipeline's frames");
}

ZTEST_SUITE(audio_pipeline_builtin_resources, NULL, NULL, builtin_resources_before,
	    builtin_resources_after, NULL);
