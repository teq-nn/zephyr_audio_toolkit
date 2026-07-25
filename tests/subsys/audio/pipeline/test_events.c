/*
 * Queue-based event delivery: audio_pipeline_get_event() (spec §3.3 and §8.3,
 * manifest §8).
 *
 * The suite drives the four publishing sites of the subsystem - start() open
 * failure, worker EOF, worker error and join() close failure - and reads the
 * resulting events back through the message queue from a thread other than the
 * publisher. The optional callback is registered throughout, so every case
 * also proves that the secondary path still fires.
 *
 * The chain is a shared fake source -> shared counting sink (fake_nodes.h).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <zephyr/audio/audio_node.h>
#include <zephyr/audio/audio_pipeline.h>
#include <zephyr/audio/audio_pipeline_events.h>

#include "fake_nodes.h"

#define TEST_EVENT_TIMEOUT K_MSEC(500)
/* Long enough to be unambiguous, short enough not to slow the suite down. */
#define TEST_EMPTY_TIMEOUT K_MSEC(50)

#define TEST_READER_STACK_SIZE 1024

/* The worker thread keeps a pointer to the pipeline, so the fixture outlives
 * the individual test functions.
 */
static struct audio_pipeline test_pipeline;
static struct audio_node test_source;
static struct audio_node test_sink;
static struct audio_fake_source source_state;
static struct audio_fake_sink sink_state;

/* Secondary (callback) path bookkeeping. */
static struct audio_pipeline_event cb_last_event;
static int cb_events;

/* Dedicated consumer thread, used to prove that the queue can be drained from
 * a thread that is neither the publisher nor the control thread.
 */
static K_THREAD_STACK_DEFINE(reader_stack, TEST_READER_STACK_SIZE);
static struct k_thread reader_thread;
static struct audio_pipeline_event reader_event;
static int reader_ret;
static bool reader_created;

static void test_event_cb(const struct audio_pipeline_event *event, void *user_data)
{
	ARG_UNUSED(user_data);

	cb_last_event = *event;
	cb_events++;
}

static const struct audio_pipeline_config test_config = {
	.frame_samples = CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES,
	.event_cb = test_event_cb,
	.event_user_data = NULL,
};

/* Bound after every init(), which clears the binding (spec §8.1). The fakes
 * accept any format, so the values only have to be well formed.
 */
static const struct audio_stream_config test_format = {
	.sample_rate_hz = 48000U,
	.channels = 2U,
	.valid_bits_per_sample = 24U,
	.format = AUDIO_SAMPLE_FORMAT_S32_LE,
};

static void reader_entry(void *p1, void *p2, void *p3)
{
	struct audio_pipeline *pipeline = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	reader_ret = audio_pipeline_get_event(pipeline, &reader_event, K_FOREVER);
}

static void events_before(void *fixture)
{
	ARG_UNUSED(fixture);

	memset(&test_pipeline, 0, sizeof(test_pipeline));
	audio_fake_source_reset(&source_state);
	audio_fake_sink_reset(&sink_state);
	memset(&cb_last_event, 0, sizeof(cb_last_event));
	memset(&reader_event, 0, sizeof(reader_event));
	cb_events = 0;
	reader_ret = -EBUSY;

	test_source = (struct audio_node){
		.role = AUDIO_NODE_ROLE_SOURCE,
		.ops = &audio_fake_source_ops,
		.upstream = NULL,
		.state = &source_state,
	};
	test_sink = (struct audio_node){
		.role = AUDIO_NODE_ROLE_SINK,
		.ops = &audio_fake_sink_ops,
		.upstream = &test_source,
		.state = &sink_state,
	};

	zassert_equal(audio_pipeline_init(&test_pipeline, &test_config, &test_sink), 0,
		      "init failed");
	zassert_equal(audio_pipeline_set_format(&test_pipeline, &test_format), 0,
		      "binding the pipeline format failed");
}

static void events_after(void *fixture)
{
	ARG_UNUSED(fixture);

	/* Never leave a worker thread behind for the next test. */
	sink_state.close_ret = 0;
	source_state.close_ret = 0;
	(void)audio_pipeline_stop(&test_pipeline);
	(void)audio_pipeline_join(&test_pipeline);

	/* A consumer still parked on an empty queue would otherwise survive
	 * into the next test.
	 */
	if (reader_created) {
		k_thread_abort(&reader_thread);
		reader_created = false;
	}
}

ZTEST(audio_pipeline_events, test_get_event_rejects_invalid_arguments)
{
	struct audio_pipeline_event event;
	struct audio_pipeline uninitialised = {0};

	zassert_equal(audio_pipeline_get_event(NULL, &event, K_NO_WAIT), -EINVAL,
		      "get_event(NULL) accepted");
	zassert_equal(audio_pipeline_get_event(&test_pipeline, NULL, K_NO_WAIT), -EINVAL,
		      "get_event without an output slot accepted");
	zassert_equal(audio_pipeline_get_event(&uninitialised, &event, K_NO_WAIT), -EINVAL,
		      "get_event before init() accepted");
}

ZTEST(audio_pipeline_events, test_get_event_no_wait_reports_empty_queue)
{
	struct audio_pipeline_event event;

	/* K_NO_WAIT on an empty queue is the k_msgq "returned without waiting"
	 * case, so the caller can poll without blocking (spec §3.3).
	 */
	zassert_equal(audio_pipeline_get_event(&test_pipeline, &event, K_NO_WAIT), -ENOMSG,
		      "empty queue did not report -ENOMSG");
}

ZTEST(audio_pipeline_events, test_get_event_finite_timeout_expires)
{
	struct audio_pipeline_event event;
	int64_t start = k_uptime_get();
	int ret;

	ret = audio_pipeline_get_event(&test_pipeline, &event, TEST_EMPTY_TIMEOUT);

	zassert_equal(ret, -EAGAIN, "expired timeout did not report -EAGAIN");
	zassert_true(k_uptime_get() - start >= 50, "get_event returned before the timeout");
}

ZTEST(audio_pipeline_events, test_eof_event_arrives_through_the_queue)
{
	struct audio_pipeline_event event;

	source_state.frames_total = 3U;

	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "play failed");

	/* Published by the worker thread, read here on the control thread. */
	zassert_equal(audio_pipeline_get_event(&test_pipeline, &event, TEST_EVENT_TIMEOUT), 0,
		      "no EOF event on the queue");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_EOF, "expected an EOF event");
	zassert_equal(event.err, 0, "EOF must not carry an error code");

	zassert_equal(atomic_get(&sink_state.eof_seen), 1U, "sink did not see EOF");
	zassert_false(audio_pipeline_is_playing(&test_pipeline), "still playing after EOF");

	/* Exactly one event, and the idling worker must not repeat it. */
	zassert_equal(audio_pipeline_get_event(&test_pipeline, &event, TEST_EMPTY_TIMEOUT), -EAGAIN,
		      "EOF event repeated on the queue");
}

ZTEST(audio_pipeline_events, test_eof_event_is_readable_from_a_third_thread)
{
	source_state.frames_total = 2U;

	/* The consumer is neither the publishing worker nor the control thread
	 * that started the pipeline (spec §3.3).
	 */
	k_thread_create(&reader_thread, reader_stack, K_THREAD_STACK_SIZEOF(reader_stack),
			reader_entry, &test_pipeline, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
	reader_created = true;

	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "play failed");

	zassert_equal(k_thread_join(&reader_thread, TEST_EVENT_TIMEOUT), 0,
		      "consumer thread never got its event");
	zassert_equal(reader_ret, 0, "get_event failed on the consumer thread");
	zassert_equal(reader_event.type, AUDIO_PIPELINE_EVENT_EOF, "expected an EOF event");
}

ZTEST(audio_pipeline_events, test_error_event_arrives_after_the_chain_is_quiesced)
{
	struct audio_pipeline_event event;

	source_state.frames_total = 10U;
	source_state.fail_at_frame = 2U;
	source_state.process_ret = -EIO;

	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "play failed");

	zassert_equal(audio_pipeline_get_event(&test_pipeline, &event, TEST_EVENT_TIMEOUT), 0,
		      "no ERROR event on the queue");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_ERROR, "expected an ERROR event");
	zassert_equal(event.err, -EIO, "wrong error code on the queue");

	/* Ordering contract (spec §9.2): the nodes are closed before the event
	 * becomes observable, so the application sees a quiesced pipeline.
	 */
	zassert_equal(atomic_get(&sink_state.close_calls), 1,
		      "sink not closed before the ERROR event");
	zassert_equal(atomic_get(&source_state.close_calls), 1,
		      "source not closed before the ERROR event");
	zassert_false(audio_pipeline_is_playing(&test_pipeline), "still playing after the error");
}

ZTEST(audio_pipeline_events, test_start_open_failure_publishes_error_to_the_queue)
{
	struct audio_pipeline_event event;

	source_state.open_ret = -ENODEV;

	zassert_equal(audio_pipeline_start(&test_pipeline), -ENODEV, "open failure not propagated");

	zassert_equal(audio_pipeline_get_event(&test_pipeline, &event, K_NO_WAIT), 0,
		      "no ERROR event for the failed open");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_ERROR, "expected an ERROR event");
	zassert_equal(event.err, -ENODEV, "wrong error code on the queue");
}

ZTEST(audio_pipeline_events, test_join_close_failure_publishes_error_to_the_queue)
{
	struct audio_pipeline_event event;

	source_state.frames_total = 0U;
	sink_state.close_ret = -EIO;

	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_join(&test_pipeline), -EIO, "close failure not propagated");

	zassert_equal(audio_pipeline_get_event(&test_pipeline, &event, K_NO_WAIT), 0,
		      "no ERROR event for the failed close");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_ERROR, "expected an ERROR event");
	zassert_equal(event.err, -EIO, "wrong error code on the queue");
}

ZTEST(audio_pipeline_events, test_init_purges_stale_events)
{
	struct audio_pipeline_event event;

	source_state.open_ret = -ENODEV;
	zassert_equal(audio_pipeline_start(&test_pipeline), -ENODEV, "open failure not propagated");

	/* Rebinding the instance must not hand the application events from its
	 * previous life.
	 */
	zassert_equal(audio_pipeline_init(&test_pipeline, &test_config, &test_sink), 0,
		      "re-init failed");
	zassert_equal(audio_pipeline_set_format(&test_pipeline, &test_format), 0,
		      "rebinding the format after init() failed");
	zassert_equal(audio_pipeline_get_event(&test_pipeline, &event, K_NO_WAIT), -ENOMSG,
		      "init did not purge the event queue");
}

/*
 * Overflow policy: the queue keeps the oldest AUDIO_PIPELINE_EVENT_QUEUE_DEPTH
 * events and drops the newest ones, so the first failure - the one that
 * explains all the others - always survives.
 */
static const int overflow_errs[] = {
	-EIO, -ENOMEM, -EBUSY, -EACCES, -EFAULT, -ENODEV, -ENOTSUP, -EPERM,
};

BUILD_ASSERT(AUDIO_PIPELINE_EVENT_QUEUE_DEPTH + 2 <= ARRAY_SIZE(overflow_errs),
	     "grow overflow_errs[] to cover the configured queue depth");

ZTEST(audio_pipeline_events, test_full_queue_drops_the_newest_events)
{
	const size_t depth = AUDIO_PIPELINE_EVENT_QUEUE_DEPTH;
	const size_t published = depth + 2U;
	struct audio_pipeline_event event;
	size_t i;

	/* Every failed start() publishes exactly one ERROR event, and each one
	 * carries a distinguishable error code.
	 */
	for (i = 0; i < published; i++) {
		source_state.open_ret = overflow_errs[i];
		zassert_equal(audio_pipeline_start(&test_pipeline), overflow_errs[i],
			      "open failure not propagated");
	}

	zassert_equal(cb_events, (int)published, "callback path lost events");

	for (i = 0; i < depth; i++) {
		zassert_equal(audio_pipeline_get_event(&test_pipeline, &event, K_NO_WAIT), 0,
			      "queue holds fewer events than its depth");
		zassert_equal(event.type, AUDIO_PIPELINE_EVENT_ERROR, "expected an ERROR event");
		zassert_equal(event.err, overflow_errs[i],
			      "queue did not keep the oldest events in order");
	}

	zassert_equal(audio_pipeline_get_event(&test_pipeline, &event, K_NO_WAIT), -ENOMSG,
		      "queue held more events than its depth");
}

ZTEST(audio_pipeline_events, test_callback_still_receives_events_alongside_the_queue)
{
	struct audio_pipeline_event event;

	source_state.frames_total = 1U;

	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "play failed");

	zassert_equal(audio_pipeline_get_event(&test_pipeline, &event, TEST_EVENT_TIMEOUT), 0,
		      "no EOF event on the queue");

	zassert_equal(cb_events, 1, "callback did not see the event");
	zassert_equal(cb_last_event.type, event.type, "callback and queue disagree on the type");
	zassert_equal(cb_last_event.err, event.err, "callback and queue disagree on the error");
}

ZTEST(audio_pipeline_events, test_events_are_delivered_without_a_callback)
{
	static const struct audio_pipeline_config queue_only_config = {
		.frame_samples = CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES,
		.event_cb = NULL,
		.event_user_data = NULL,
	};
	struct audio_pipeline_event event;

	source_state.frames_total = 1U;

	zassert_equal(audio_pipeline_init(&test_pipeline, &queue_only_config, &test_sink), 0,
		      "init without a callback failed");
	zassert_equal(audio_pipeline_set_format(&test_pipeline, &test_format), 0,
		      "binding the pipeline format failed");
	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "play failed");

	zassert_equal(audio_pipeline_get_event(&test_pipeline, &event, TEST_EVENT_TIMEOUT), 0,
		      "queue delivery depends on a registered callback");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_EOF, "expected an EOF event");
	zassert_equal(cb_events, 0, "callback fired although none was registered");
}

ZTEST_SUITE(audio_pipeline_events, NULL, NULL, events_before, events_after, NULL);
