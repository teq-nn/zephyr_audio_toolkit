/*
 * Lifecycle tests for the pipeline worker thread.
 *
 * The chain under test is always source -> filter -> sink, built from the
 * shared fake nodes (fake_nodes.h) so the suite runs on native_sim without
 * audio hardware.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include <zephyr/audio/audio_pipeline.h>
#include <zephyr/audio/audio_pipeline_events.h>

#include "fake_nodes.h"

#define TEST_EVENT_TIMEOUT K_MSEC(500)
#define TEST_FRAME_TIMEOUT K_MSEC(500)

/* The worker thread holds a pointer to the pipeline, so the fixture has to
 * outlive the individual test functions.
 */
static struct audio_pipeline test_pipeline;
static struct audio_node test_source;
static struct audio_node test_filter;
static struct audio_node test_sink;
static struct audio_fake_source source_state;
static struct audio_fake_sink filter_state;
static struct audio_fake_sink sink_state;

static struct k_sem event_sem;
static struct k_sem frame_sem;
static struct k_sem gate_sem;
static struct audio_pipeline_event last_event;
static int eof_events;
static int error_events;

static void test_event_cb(const struct audio_pipeline_event *event, void *user_data)
{
	ARG_UNUSED(user_data);

	last_event = *event;

	switch (event->type) {
	case AUDIO_PIPELINE_EVENT_EOF:
		eof_events++;
		break;
	case AUDIO_PIPELINE_EVENT_ERROR:
		error_events++;
		break;
	default:
		break;
	}

	k_sem_give(&event_sem);
}

static const struct audio_pipeline_config test_config = {
	.stream = {
		.sample_rate_hz = 48000U,
		.channels = 2U,
		.valid_bits_per_sample = 24U,
		.format = AUDIO_SAMPLE_FORMAT_S32_LE,
	},
	.frame_samples = CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES,
	.event_cb = test_event_cb,
	.event_user_data = NULL,
};

static void lifecycle_before(void *fixture)
{
	ARG_UNUSED(fixture);

	memset(&test_pipeline, 0, sizeof(test_pipeline));
	audio_fake_source_reset(&source_state);
	audio_fake_sink_reset(&filter_state);
	audio_fake_sink_reset(&sink_state);

	test_source = (struct audio_node){
		.role = AUDIO_NODE_ROLE_SOURCE,
		.ops = &audio_fake_source_ops,
		.upstream = NULL,
		.state = &source_state,
	};
	test_filter = (struct audio_node){
		.role = AUDIO_NODE_ROLE_FILTER,
		.ops = &audio_fake_sink_ops,
		.upstream = &test_source,
		.state = &filter_state,
	};
	test_sink = (struct audio_node){
		.role = AUDIO_NODE_ROLE_SINK,
		.ops = &audio_fake_sink_ops,
		.upstream = &test_filter,
		.state = &sink_state,
	};

	k_sem_init(&event_sem, 0, 16);
	k_sem_init(&frame_sem, 0, 16);
	k_sem_init(&gate_sem, 0, 16);
	eof_events = 0;
	error_events = 0;
	memset(&last_event, 0, sizeof(last_event));

	zassert_equal(audio_pipeline_init(&test_pipeline, &test_config, &test_sink), 0,
		      "init failed");
}

static void lifecycle_after(void *fixture)
{
	ARG_UNUSED(fixture);

	/* Never leave a worker thread behind for the next test; release the
	 * gate first in case the sink is still parked on it.
	 */
	k_sem_give(&gate_sem);
	(void)audio_pipeline_stop(&test_pipeline);
	k_sem_give(&gate_sem);
	(void)audio_pipeline_join(&test_pipeline);
}

ZTEST(audio_pipeline_lifecycle, test_start_opens_all_nodes)
{
	source_state.frames_total = 1U;

	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");

	zassert_equal(atomic_get(&source_state.open_calls), 1, "source not opened");
	zassert_equal(atomic_get(&filter_state.open_calls), 1, "filter not opened");
	zassert_equal(atomic_get(&sink_state.open_calls), 1, "sink not opened");

	zassert_true(audio_pipeline_is_running(&test_pipeline), "thread not running");
	zassert_false(audio_pipeline_is_playing(&test_pipeline), "start must not play");

	/* No play() yet, so the chain must stay untouched. */
	k_msleep(20);
	zassert_equal(atomic_get(&sink_state.frames_seen), 0U, "frames pulled before play()");

	zassert_equal(audio_pipeline_join(&test_pipeline), 0, "join failed");

	zassert_equal(atomic_get(&source_state.close_calls), 1, "source not closed");
	zassert_equal(atomic_get(&filter_state.close_calls), 1, "filter not closed");
	zassert_equal(atomic_get(&sink_state.close_calls), 1, "sink not closed");
	zassert_false(audio_pipeline_is_running(&test_pipeline), "thread still running");
}

ZTEST(audio_pipeline_lifecycle, test_start_propagates_open_failure)
{
	source_state.open_ret = -EIO;

	zassert_equal(audio_pipeline_start(&test_pipeline), -EIO, "open failure not propagated");

	zassert_equal(error_events, 1, "no ERROR event for failed open");
	zassert_equal(last_event.type, AUDIO_PIPELINE_EVENT_ERROR, "wrong event type");
	zassert_equal(last_event.err, -EIO, "wrong error code");

	zassert_false(audio_pipeline_is_running(&test_pipeline), "thread started despite failure");

	/* Nodes opened before the failure must be unwound; the failing node
	 * itself is not closed.
	 */
	zassert_equal(atomic_get(&sink_state.close_calls), 1, "sink not unwound");
	zassert_equal(atomic_get(&filter_state.close_calls), 1, "filter not unwound");
	zassert_equal(atomic_get(&source_state.close_calls), 0, "failing node must not be closed");

	zassert_equal(audio_pipeline_play(&test_pipeline), -EPERM, "play() must reject closed chain");
}

ZTEST(audio_pipeline_lifecycle, test_play_requires_start)
{
	zassert_equal(audio_pipeline_play(&test_pipeline), -EPERM, "play() before start() allowed");
	zassert_equal(atomic_get(&sink_state.frames_seen), 0U,
		      "frames pulled without a worker thread");
}

ZTEST(audio_pipeline_lifecycle, test_stop_halts_playback_thread_idles)
{
	size_t frames;

	source_state.frames_total = AUDIO_FAKE_ENDLESS;
	sink_state.frame_sem = &frame_sem;
	sink_state.gate_sem = &gate_sem;

	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "play failed");

	/* Wait until the sink is parked inside process() on the gate. */
	zassert_equal(k_sem_take(&frame_sem, TEST_FRAME_TIMEOUT), 0, "no frame processed");
	zassert_true(audio_pipeline_is_playing(&test_pipeline), "not playing");

	zassert_equal(audio_pipeline_stop(&test_pipeline), 0, "stop failed");
	k_sem_give(&gate_sem);

	/* At most the in-flight frame completes, then the worker idles. */
	k_msleep(20);
	frames = (size_t)atomic_get(&sink_state.frames_seen);

	zassert_true(audio_pipeline_is_running(&test_pipeline), "stop() killed the thread");
	zassert_false(audio_pipeline_is_playing(&test_pipeline), "still playing after stop()");

	k_msleep(20);
	zassert_equal((size_t)atomic_get(&sink_state.frames_seen), frames,
		      "frames pulled while stopped");

	/* Playback resumes on the same thread. */
	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "replay after stop failed");
	zassert_equal(k_sem_take(&frame_sem, TEST_FRAME_TIMEOUT), 0, "no frame after replay");
	zassert_true((size_t)atomic_get(&sink_state.frames_seen) > frames, "sink saw no new frame");

	zassert_equal(audio_pipeline_stop(&test_pipeline), 0, "stop failed");
	k_sem_give(&gate_sem);
}

ZTEST(audio_pipeline_lifecycle, test_eof_keeps_thread_alive)
{
	source_state.frames_total = 3U;

	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "play failed");

	zassert_equal(k_sem_take(&event_sem, TEST_EVENT_TIMEOUT), 0, "no EOF event");
	zassert_equal(last_event.type, AUDIO_PIPELINE_EVENT_EOF, "expected EOF event");
	zassert_equal(eof_events, 1, "expected exactly one EOF event");
	zassert_equal(error_events, 0, "EOF must not raise ERROR");

	zassert_equal(atomic_get(&sink_state.frames_seen), 3U, "wrong frame count");
	zassert_equal(atomic_get(&sink_state.eof_seen), 1U, "sink did not see EOF");
	zassert_equal(atomic_get(&filter_state.eof_seen), 1U, "filter did not forward EOF");

	zassert_false(audio_pipeline_is_playing(&test_pipeline), "still playing after EOF");

	/* Let the worker settle: the event fires from inside its loop, so its
	 * liveness can only be judged once it has had time to leave or idle.
	 * The thread must idle instead of spinning on the exhausted source.
	 */
	k_msleep(20);
	zassert_true(audio_pipeline_is_running(&test_pipeline), "EOF terminated the thread");
	zassert_equal(atomic_get(&sink_state.frames_seen), 3U, "kept pulling after EOF");
	zassert_equal(eof_events, 1, "EOF event repeated while idle");

	/* Nodes stay open across EOF so the next track needs no reopen. */
	zassert_equal(atomic_get(&source_state.close_calls), 0, "source closed on EOF");
	zassert_equal(atomic_get(&sink_state.close_calls), 0, "sink closed on EOF");
}

ZTEST(audio_pipeline_lifecycle, test_replay_after_eof)
{
	source_state.frames_total = 2U;

	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "play failed");
	zassert_equal(k_sem_take(&event_sem, TEST_EVENT_TIMEOUT), 0, "no EOF event");
	zassert_equal(eof_events, 1, "expected first EOF");

	/* Second track on the very same thread. */
	audio_fake_source_rewind(&source_state);

	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "replay failed");
	zassert_equal(k_sem_take(&event_sem, TEST_EVENT_TIMEOUT), 0, "no second EOF event");
	zassert_equal(eof_events, 2, "expected second EOF");

	zassert_equal(atomic_get(&sink_state.frames_seen), 4U,
		      "second track did not process frames");
	zassert_equal(atomic_get(&source_state.open_calls), 1,
		      "nodes reopened for the second track");

	k_msleep(20);
	zassert_true(audio_pipeline_is_running(&test_pipeline), "thread died between tracks");
}

ZTEST(audio_pipeline_lifecycle, test_node_error_stops_and_closes_nodes)
{
	source_state.frames_total = 10U;
	source_state.fail_at_frame = 2U;
	source_state.process_ret = -EIO;

	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "play failed");

	zassert_equal(k_sem_take(&event_sem, TEST_EVENT_TIMEOUT), 0, "no ERROR event");
	zassert_equal(last_event.type, AUDIO_PIPELINE_EVENT_ERROR, "expected ERROR event");
	zassert_equal(last_event.err, -EIO, "wrong error code");
	zassert_equal(error_events, 1, "expected exactly one ERROR event");
	zassert_equal(eof_events, 0, "error must not raise EOF");

	zassert_false(audio_pipeline_is_playing(&test_pipeline), "still playing after error");
	zassert_equal(atomic_get(&sink_state.frames_seen), 1U,
		      "processing did not stop at the first error");

	/* Nodes are closed on the error path, so playback cannot resume until
	 * start() reopens the chain.
	 */
	zassert_equal(atomic_get(&sink_state.close_calls), 1, "sink not closed after error");
	zassert_equal(atomic_get(&filter_state.close_calls), 1, "filter not closed after error");
	zassert_equal(atomic_get(&source_state.close_calls), 1, "source not closed after error");
	zassert_equal(audio_pipeline_play(&test_pipeline), -EPERM, "play() after error allowed");

	k_msleep(20);
	zassert_true(audio_pipeline_is_running(&test_pipeline), "error terminated the thread");
}

ZTEST(audio_pipeline_lifecycle, test_join_terminates_thread_and_start_restarts)
{
	source_state.frames_total = 1U;

	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_join(&test_pipeline), 0, "join failed");
	zassert_false(audio_pipeline_is_running(&test_pipeline), "join did not end the thread");

	/* join() is idempotent. */
	zassert_equal(audio_pipeline_join(&test_pipeline), 0, "second join failed");
	zassert_equal(atomic_get(&sink_state.close_calls), 1, "close called twice");

	/* A joined pipeline can be started again. */
	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "restart failed");
	zassert_equal(atomic_get(&sink_state.open_calls), 2, "restart did not reopen the chain");
	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "play after restart failed");
	zassert_equal(k_sem_take(&event_sem, TEST_EVENT_TIMEOUT), 0, "no EOF after restart");
	zassert_equal(eof_events, 1, "expected EOF after restart");
}

ZTEST_SUITE(audio_pipeline_lifecycle, NULL, NULL, lifecycle_before, lifecycle_after, NULL);
