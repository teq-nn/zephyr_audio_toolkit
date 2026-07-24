/*
 * Lifecycle tests for the pipeline worker thread (spec §8.2 / §9,
 * manifest §3 and §7).
 *
 * The chain under test is always source -> filter -> sink, built from the fake
 * nodes below so the suite runs on native_sim without audio hardware
 * (spec §12.1).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <zephyr/audio/audio_pipeline.h>
#include <zephyr/audio/audio_pipeline_events.h>

#define TEST_EVENT_TIMEOUT K_MSEC(500)
#define TEST_FRAME_TIMEOUT K_MSEC(500)

/* The fake sink blocks on the gate with a timeout rather than K_FOREVER so a
 * broken pipeline fails the assertions instead of hanging the whole suite.
 */
#define TEST_GATE_TIMEOUT K_MSEC(500)

struct fake_node_state {
	/* open()/close() bookkeeping, shared by all three roles. */
	int open_calls;
	int close_calls;
	int open_ret;
	int close_ret;

	/* Source behaviour. */
	size_t frames_total;
	size_t frames_done;
	size_t fail_at_frame; /* 1-based; 0 disables the failure injection */
	int process_ret;

	/* Filter/sink behaviour. */
	size_t frames_seen;
	size_t eof_seen;

	/* Optional lock-step handshake used by the stop() test. */
	struct k_sem *frame_sem;
	struct k_sem *gate_sem;
};

static int fake_open(struct audio_node *node)
{
	struct fake_node_state *state = node->state;

	state->open_calls++;

	return state->open_ret;
}

static int fake_close(struct audio_node *node)
{
	struct fake_node_state *state = node->state;

	state->close_calls++;

	return state->close_ret;
}

static int fake_source_process(struct audio_node *node, struct audio_buffer_view *buf,
			       size_t *out_size)
{
	struct fake_node_state *state = node->state;

	if (state->fail_at_frame != 0U && state->frames_done + 1U == state->fail_at_frame) {
		return state->process_ret;
	}

	if (state->frames_done >= state->frames_total) {
		*out_size = 0;
		return 0;
	}

	memset(buf->data, 0, buf->capacity * sizeof(int32_t));
	*out_size = buf->capacity;
	state->frames_done++;

	return 0;
}

static int fake_filter_process(struct audio_node *node, struct audio_buffer_view *buf,
			       size_t *out_size)
{
	struct fake_node_state *state = node->state;
	int ret;

	ret = audio_node_process(node->upstream, buf, out_size);
	if (ret < 0) {
		return ret;
	}

	if (*out_size == 0U) {
		state->eof_seen++;
	} else {
		state->frames_seen++;
	}

	return 0;
}

static int fake_sink_process(struct audio_node *node, struct audio_buffer_view *buf,
			     size_t *out_size)
{
	struct fake_node_state *state = node->state;
	int ret;

	ret = audio_node_process(node->upstream, buf, out_size);
	if (ret < 0) {
		return ret;
	}

	if (*out_size == 0U) {
		state->eof_seen++;
		return 0;
	}

	state->frames_seen++;

	if (state->frame_sem) {
		k_sem_give(state->frame_sem);
	}

	if (state->gate_sem) {
		(void)k_sem_take(state->gate_sem, TEST_GATE_TIMEOUT);
	}

	return 0;
}

static const struct audio_node_ops fake_source_ops = {
	.open = fake_open,
	.process = fake_source_process,
	.close = fake_close,
};

static const struct audio_node_ops fake_filter_ops = {
	.open = fake_open,
	.process = fake_filter_process,
	.close = fake_close,
};

static const struct audio_node_ops fake_sink_ops = {
	.open = fake_open,
	.process = fake_sink_process,
	.close = fake_close,
};

/* The worker thread holds a pointer to the pipeline, so the fixture has to
 * outlive the individual test functions.
 */
static struct audio_pipeline test_pipeline;
static struct audio_node test_source;
static struct audio_node test_filter;
static struct audio_node test_sink;
static struct fake_node_state source_state;
static struct fake_node_state filter_state;
static struct fake_node_state sink_state;

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
	memset(&source_state, 0, sizeof(source_state));
	memset(&filter_state, 0, sizeof(filter_state));
	memset(&sink_state, 0, sizeof(sink_state));

	test_source = (struct audio_node){
		.role = AUDIO_NODE_ROLE_SOURCE,
		.ops = &fake_source_ops,
		.upstream = NULL,
		.state = &source_state,
	};
	test_filter = (struct audio_node){
		.role = AUDIO_NODE_ROLE_FILTER,
		.ops = &fake_filter_ops,
		.upstream = &test_source,
		.state = &filter_state,
	};
	test_sink = (struct audio_node){
		.role = AUDIO_NODE_ROLE_SINK,
		.ops = &fake_sink_ops,
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

	zassert_equal(source_state.open_calls, 1, "source not opened");
	zassert_equal(filter_state.open_calls, 1, "filter not opened");
	zassert_equal(sink_state.open_calls, 1, "sink not opened");

	zassert_true(audio_pipeline_is_running(&test_pipeline), "thread not running");
	zassert_false(audio_pipeline_is_playing(&test_pipeline), "start must not play");

	/* No play() yet, so the chain must stay untouched. */
	k_msleep(20);
	zassert_equal(sink_state.frames_seen, 0U, "frames pulled before play()");

	zassert_equal(audio_pipeline_join(&test_pipeline), 0, "join failed");

	zassert_equal(source_state.close_calls, 1, "source not closed");
	zassert_equal(filter_state.close_calls, 1, "filter not closed");
	zassert_equal(sink_state.close_calls, 1, "sink not closed");
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
	zassert_equal(sink_state.close_calls, 1, "sink not unwound");
	zassert_equal(filter_state.close_calls, 1, "filter not unwound");
	zassert_equal(source_state.close_calls, 0, "failing node must not be closed");

	zassert_equal(audio_pipeline_play(&test_pipeline), -EPERM, "play() must reject closed chain");
}

ZTEST(audio_pipeline_lifecycle, test_play_requires_start)
{
	zassert_equal(audio_pipeline_play(&test_pipeline), -EPERM, "play() before start() allowed");
	zassert_equal(sink_state.frames_seen, 0U, "frames pulled without a worker thread");
}

ZTEST(audio_pipeline_lifecycle, test_stop_halts_playback_thread_idles)
{
	size_t frames;

	source_state.frames_total = SIZE_MAX;
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
	frames = sink_state.frames_seen;

	zassert_true(audio_pipeline_is_running(&test_pipeline), "stop() killed the thread");
	zassert_false(audio_pipeline_is_playing(&test_pipeline), "still playing after stop()");

	k_msleep(20);
	zassert_equal(sink_state.frames_seen, frames, "frames pulled while stopped");

	/* Playback resumes on the same thread. */
	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "replay after stop failed");
	zassert_equal(k_sem_take(&frame_sem, TEST_FRAME_TIMEOUT), 0, "no frame after replay");
	zassert_true(sink_state.frames_seen > frames, "sink saw no new frame");

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

	zassert_equal(sink_state.frames_seen, 3U, "wrong frame count");
	zassert_equal(sink_state.eof_seen, 1U, "sink did not see EOF");
	zassert_equal(filter_state.eof_seen, 1U, "filter did not forward EOF");

	zassert_false(audio_pipeline_is_playing(&test_pipeline), "still playing after EOF");

	/* Let the worker settle: the event fires from inside its loop, so its
	 * liveness can only be judged once it has had time to leave or idle.
	 * The thread must idle instead of spinning on the exhausted source.
	 */
	k_msleep(20);
	zassert_true(audio_pipeline_is_running(&test_pipeline), "EOF terminated the thread");
	zassert_equal(sink_state.frames_seen, 3U, "kept pulling after EOF");
	zassert_equal(eof_events, 1, "EOF event repeated while idle");

	/* Nodes stay open across EOF so the next track needs no reopen. */
	zassert_equal(source_state.close_calls, 0, "source closed on EOF");
	zassert_equal(sink_state.close_calls, 0, "sink closed on EOF");
}

ZTEST(audio_pipeline_lifecycle, test_replay_after_eof)
{
	source_state.frames_total = 2U;

	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "play failed");
	zassert_equal(k_sem_take(&event_sem, TEST_EVENT_TIMEOUT), 0, "no EOF event");
	zassert_equal(eof_events, 1, "expected first EOF");

	/* Second track on the very same thread (manifest §3). */
	source_state.frames_done = 0U;

	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "replay failed");
	zassert_equal(k_sem_take(&event_sem, TEST_EVENT_TIMEOUT), 0, "no second EOF event");
	zassert_equal(eof_events, 2, "expected second EOF");

	zassert_equal(sink_state.frames_seen, 4U, "second track did not process frames");
	zassert_equal(source_state.open_calls, 1, "nodes reopened for the second track");

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
	zassert_equal(sink_state.frames_seen, 1U, "processing did not stop at the first error");

	/* Nodes are closed on the error path, so playback cannot resume until
	 * start() reopens the chain.
	 */
	zassert_equal(sink_state.close_calls, 1, "sink not closed after error");
	zassert_equal(filter_state.close_calls, 1, "filter not closed after error");
	zassert_equal(source_state.close_calls, 1, "source not closed after error");
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
	zassert_equal(sink_state.close_calls, 1, "close called twice");

	/* A joined pipeline can be started again. */
	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "restart failed");
	zassert_equal(sink_state.open_calls, 2, "restart did not reopen the chain");
	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "play after restart failed");
	zassert_equal(k_sem_take(&event_sem, TEST_EVENT_TIMEOUT), 0, "no EOF after restart");
	zassert_equal(eof_events, 1, "expected EOF after restart");
}

ZTEST_SUITE(audio_pipeline_lifecycle, NULL, NULL, lifecycle_before, lifecycle_after, NULL);
