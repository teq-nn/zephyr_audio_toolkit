/*
 * Lifecycle tests for the pipeline worker thread (spec §8.2 / §9,
 * manifest §3 and §7).
 *
 * The chain under test is always source -> filter -> sink, built from the
 * shared fake nodes (fake_nodes.h) so the suite runs on native_sim without
 * audio hardware (spec §12.1).
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
	.frame_samples = CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES,
	.event_cb = test_event_cb,
	.event_user_data = NULL,
};

/* The format the suite binds after every init(); the fakes accept anything, so
 * its only job is to be recognisable where a node reports what it was handed.
 */
static const struct audio_stream_config test_format = {
	.sample_rate_hz = 48000U,
	.channels = 2U,
	.valid_bits_per_sample = 24U,
	.format = AUDIO_SAMPLE_FORMAT_S32_LE,
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
	/* init() clears the binding, so every test starts from a bound pipeline
	 * unless it deliberately unbinds one (spec §8.1).
	 */
	zassert_equal(audio_pipeline_set_format(&test_pipeline, &test_format), 0,
		      "binding the pipeline format failed");
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

	/* The worker exists but must not have pulled anything: it pulls only
	 * while the pipeline is playing, and nothing but play() enters that
	 * state. That is the whole guarantee, and it holds the moment start()
	 * returns - sleeping first and counting frames afterwards could only
	 * ever miss a defect on a slow host, never catch one.
	 */
	zassert_true(audio_pipeline_is_running(&test_pipeline), "thread not running");
	zassert_false(audio_pipeline_is_playing(&test_pipeline), "start must not play");
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
	source_state.frames_total = AUDIO_FAKE_ENDLESS;
	sink_state.frame_sem = &frame_sem;
	sink_state.gate_sem = &gate_sem;

	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "play failed");

	/* Wait until the sink is parked inside process() on the gate. */
	zassert_equal(k_sem_take(&frame_sem, TEST_FRAME_TIMEOUT), 0, "no frame processed");
	zassert_true(audio_pipeline_is_playing(&test_pipeline), "not playing");

	zassert_equal(audio_pipeline_stop(&test_pipeline), 0, "stop failed");

	/* The transition is complete when stop() returns - it is the caller's
	 * own move, not something the worker gets around to - so there is
	 * nothing to wait for here. The worker may still be finishing the frame
	 * the sink is parked on, which is exactly why this asserts the state
	 * rather than a frame count.
	 */
	zassert_false(audio_pipeline_is_playing(&test_pipeline), "still playing after stop()");
	zassert_true(audio_pipeline_is_running(&test_pipeline), "stop() killed the thread");

	/* Release the parked sink so the in-flight frame completes. Its give of
	 * frame_sem was consumed above, so the next successful take() can only
	 * come from a frame pulled after this point - and while the pipeline is
	 * stopped there is none, which is what makes the replay assertion below
	 * mean something without a second frame counter.
	 */
	k_sem_give(&gate_sem);

	/* Playback resumes on the same thread. */
	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "replay after stop failed");
	zassert_true(audio_pipeline_is_playing(&test_pipeline), "play() did not resume playback");
	zassert_equal(k_sem_take(&frame_sem, TEST_FRAME_TIMEOUT), 0, "no frame after replay");

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

	/* The worker leaves the playing state *before* it publishes, so the
	 * event having arrived is itself proof that the transition happened -
	 * there is no settling time to wait out. The thread's liveness is a
	 * state question too: nothing but a join request makes it return.
	 */
	zassert_false(audio_pipeline_is_playing(&test_pipeline), "still playing after EOF");
	zassert_true(audio_pipeline_is_running(&test_pipeline), "EOF terminated the thread");

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

	/* Second track on the very same thread (manifest §3). */
	audio_fake_source_rewind(&source_state);

	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "replay failed");
	zassert_equal(k_sem_take(&event_sem, TEST_EVENT_TIMEOUT), 0, "no second EOF event");
	zassert_equal(eof_events, 2, "expected second EOF");

	zassert_equal(atomic_get(&sink_state.frames_seen), 4U,
		      "second track did not process frames");
	zassert_equal(atomic_get(&source_state.open_calls), 1,
		      "nodes reopened for the second track");

	/* Both tracks ran on the one thread, and it is still there: the second
	 * EOF above could not have been published otherwise.
	 */
	zassert_true(audio_pipeline_is_running(&test_pipeline), "thread died between tracks");
	zassert_false(audio_pipeline_is_playing(&test_pipeline), "still playing after track two");
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

	/* The worker closes the chain and leaves the playing state before it
	 * publishes, so the ERROR event having arrived settles all of this - no
	 * sleep can make it truer. The thread itself survives the error
	 * (spec §3.1); only a join request ends it.
	 */
	zassert_false(audio_pipeline_is_playing(&test_pipeline), "still playing after error");
	zassert_true(audio_pipeline_is_running(&test_pipeline), "error terminated the thread");
	zassert_equal(atomic_get(&sink_state.frames_seen), 1U,
		      "processing did not stop at the first error");

	/* Nodes are closed on the error path, so playback cannot resume until
	 * start() reopens the chain.
	 */
	zassert_equal(atomic_get(&sink_state.close_calls), 1, "sink not closed after error");
	zassert_equal(atomic_get(&filter_state.close_calls), 1, "filter not closed after error");
	zassert_equal(atomic_get(&source_state.close_calls), 1, "source not closed after error");
	zassert_equal(audio_pipeline_play(&test_pipeline), -EPERM, "play() after error allowed");

	/* start() reopens the chain onto the thread that is still there, which
	 * is what separates the state a node error leaves behind from the one
	 * join() leaves behind: the latter has no thread to reopen onto.
	 */
	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "restart after error failed");
	zassert_equal(atomic_get(&sink_state.open_calls), 2, "restart did not reopen the chain");
	zassert_equal(atomic_get(&source_state.open_calls), 2, "restart did not reopen the source");
	zassert_true(audio_pipeline_is_running(&test_pipeline), "the worker did not survive");
	zassert_false(audio_pipeline_is_playing(&test_pipeline), "the reopen resumed playback");
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

ZTEST(audio_pipeline_lifecycle, test_init_refuses_a_live_worker_thread)
{
	source_state.frames_total = 1U;

	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");

	/* Rebinding an instance whose worker thread is still alive is the one
	 * refusal init() owns by itself: there is no way out of a state that
	 * holds a thread except through join().
	 */
	zassert_equal(audio_pipeline_init(&test_pipeline, &test_config, &test_sink), -EBUSY,
		      "init() rebound a pipeline whose worker thread is alive");

	/* And it wrote nothing on the way out: the thread is still there, the
	 * chain is still open, and the format the fixture bound is still live -
	 * a successful init() would have cleared it and left start() with
	 * -ENODATA.
	 */
	zassert_true(audio_pipeline_is_running(&test_pipeline), "the refusal ended the thread");
	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "the refusal closed the chain");
	zassert_equal(k_sem_take(&event_sem, TEST_EVENT_TIMEOUT), 0, "no EOF event");
	zassert_equal(eof_events, 1, "the run after the refused init() never reached EOF");
}

ZTEST(audio_pipeline_lifecycle, test_stop_is_accepted_in_every_initialised_state)
{
	source_state.frames_total = AUDIO_FAKE_ENDLESS;
	sink_state.frame_sem = &frame_sem;
	sink_state.gate_sem = &gate_sem;
	/* Fails before it pulls, so the second frame ends the run with an error
	 * instead of data.
	 */
	sink_state.fail_at_frame = 2U;
	sink_state.process_ret = -EIO;

	/* Bound to a configuration and a sink, no worker thread. */
	zassert_equal(audio_pipeline_stop(&test_pipeline), 0, "stop() before start() refused");
	zassert_false(audio_pipeline_is_running(&test_pipeline), "stop() invented a thread");

	/* Worker thread up, chain open, idle. */
	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_stop(&test_pipeline), 0, "stop() on an idle pipeline refused");
	zassert_true(audio_pipeline_is_running(&test_pipeline), "stop() ended the thread");
	zassert_false(audio_pipeline_is_playing(&test_pipeline), "stop() started playback");

	/* Pulling frames: the sink is parked inside process() on the gate. */
	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "play failed");
	zassert_equal(k_sem_take(&frame_sem, TEST_FRAME_TIMEOUT), 0, "no frame processed");
	zassert_true(audio_pipeline_is_playing(&test_pipeline), "not playing");
	zassert_equal(audio_pipeline_stop(&test_pipeline), 0, "stop() while playing refused");
	zassert_false(audio_pipeline_is_playing(&test_pipeline), "still playing after stop()");

	/* Chain torn down by a node error, worker thread still parked on it.
	 * This is the state issue #17's three-state sketch has no name for: it
	 * folds the error path into "no worker thread", which would make
	 * audio_pipeline_is_running() start answering false here.
	 */
	k_sem_give(&gate_sem);
	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "replay before the error failed");
	zassert_equal(k_sem_take(&event_sem, TEST_EVENT_TIMEOUT), 0, "no ERROR event");
	zassert_equal(error_events, 1, "expected the scripted node error");

	zassert_true(audio_pipeline_is_running(&test_pipeline), "the error ended the thread");
	zassert_equal(audio_pipeline_play(&test_pipeline), -EPERM, "play() on a closed chain");
	zassert_equal(audio_pipeline_stop(&test_pipeline), 0, "stop() after a node error refused");

	/* The thread outlives all of it (spec §3.1), so init() stays refused
	 * throughout - including here, where the chain is already closed.
	 */
	zassert_equal(audio_pipeline_init(&test_pipeline, &test_config, &test_sink), -EBUSY,
		      "init() rebound a pipeline that still has a worker thread");
}

/* -------------------------------------------------------------------------
 * The bound pipeline format (spec §5.2, §8.1 and §8.2)
 * ----------------------------------------------------------------------
 */

/* Deliberately different from test_format in every field that matters, so a
 * rebind cannot pass by writing the value that was already there.
 */
static const struct audio_stream_config other_format = {
	.sample_rate_hz = 44100U,
	.channels = 1U,
	.valid_bits_per_sample = 16U,
	.format = AUDIO_SAMPLE_FORMAT_S32_LE,
};

ZTEST(audio_pipeline_lifecycle, test_set_format_rejects_invalid_arguments)
{
	struct audio_pipeline uninitialised = {0};
	struct audio_stream_config degenerate;

	zassert_equal(audio_pipeline_set_format(NULL, &test_format), -EINVAL,
		      "set_format(NULL) accepted");
	zassert_equal(audio_pipeline_set_format(&test_pipeline, NULL), -EINVAL,
		      "set_format without a format accepted");
	zassert_equal(audio_pipeline_set_format(&uninitialised, &test_format), -EINVAL,
		      "set_format before init() accepted");

	/* A format no node could ever satisfy is refused here, not at the first
	 * open() where it would look like a node defect.
	 */
	degenerate = test_format;
	degenerate.sample_rate_hz = 0U;
	zassert_equal(audio_pipeline_set_format(&test_pipeline, &degenerate), -EINVAL,
		      "a zero sample rate was accepted");

	degenerate = test_format;
	degenerate.channels = 0U;
	zassert_equal(audio_pipeline_set_format(&test_pipeline, &degenerate), -EINVAL,
		      "a zero channel count was accepted");
}

ZTEST(audio_pipeline_lifecycle, test_start_without_a_bound_format_reports_enodata)
{
	/* A fresh init() drops the binding the fixture made, so this is a
	 * pipeline nobody ever gave a format to.
	 */
	zassert_equal(audio_pipeline_init(&test_pipeline, &test_config, &test_sink), 0,
		      "re-init failed");

	/* -ENODATA, not -EINVAL: "you never called set_format()" is a different
	 * mistake from "your configuration is wrong" (spec §8.2).
	 */
	zassert_equal(audio_pipeline_start(&test_pipeline), -ENODATA,
		      "start() invented a format instead of refusing");

	/* Refused before anything was claimed or created, so nothing is left to
	 * clean up - and no ERROR event either.
	 */
	zassert_false(audio_pipeline_is_running(&test_pipeline), "a worker thread was created");
	zassert_equal(atomic_get(&sink_state.open_calls), 0, "the chain was opened anyway");
	zassert_equal(atomic_get(&source_state.open_calls), 0, "the chain was opened anyway");
	zassert_equal(error_events, 0, "the refusal published an ERROR event");
	zassert_equal(audio_pipeline_play(&test_pipeline), -EPERM,
		      "play() ran on a chain that was never opened");
}

ZTEST(audio_pipeline_lifecycle, test_init_clears_the_bound_format)
{
	source_state.frames_total = 1U;

	/* The fixture bound a format, so this run is legal. */
	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_join(&test_pipeline), 0, "join failed");

	/* Re-initialising rebinds the instance to a new configuration and sink,
	 * and a format carried over would be a stale default nobody chose.
	 */
	zassert_equal(audio_pipeline_init(&test_pipeline, &test_config, &test_sink), 0,
		      "re-init failed");
	zassert_equal(audio_pipeline_start(&test_pipeline), -ENODATA,
		      "the previous binding survived init()");

	/* Binding again is all it takes to make the instance runnable. */
	zassert_equal(audio_pipeline_set_format(&test_pipeline, &other_format), 0,
		      "rebinding after init() was refused");
	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start after rebinding failed");
}

ZTEST(audio_pipeline_lifecycle, test_set_format_rejects_an_open_chain)
{
	const struct audio_stream_config *reopened;

	source_state.frames_total = 2U;

	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");

	/* The chain is open but idle: still -EBUSY, because the nodes read the
	 * format in open() and hold it until they are closed. "Not playing" is
	 * not tight enough.
	 */
	zassert_equal(audio_pipeline_set_format(&test_pipeline, &other_format), -EBUSY,
		      "rebinding an open but idle chain was allowed");

	zassert_equal(audio_pipeline_play(&test_pipeline), 0, "play failed");
	zassert_equal(k_sem_take(&event_sem, TEST_EVENT_TIMEOUT), 0, "no EOF event");
	zassert_equal(last_event.type, AUDIO_PIPELINE_EVENT_EOF, "expected EOF event");

	/* Nodes stay open across EOF (manifest §3), so the answer does not
	 * change once the track has finished either.
	 */
	zassert_equal(audio_pipeline_set_format(&test_pipeline, &other_format), -EBUSY,
		      "EOF made the rebind legal although the nodes are still open");

	/* join() is what closes the chain, and that is what makes it legal. */
	zassert_equal(audio_pipeline_join(&test_pipeline), 0, "join failed");
	zassert_equal(audio_pipeline_set_format(&test_pipeline, &other_format), 0,
		      "rebinding after join() was refused");

	/* The rebind really took: the next run hands the nodes the new format. */
	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "restart failed");
	reopened = atomic_ptr_get(&sink_state.seen_format);
	zassert_not_null(reopened, "the sink was reopened without a format");
	zassert_equal(reopened->sample_rate_hz, other_format.sample_rate_hz,
		      "the sink was reopened with the old sample rate");
	zassert_equal(reopened->channels, other_format.channels,
		      "the sink was reopened with the old channel count");
}

ZTEST(audio_pipeline_lifecycle, test_start_installs_the_bound_format_on_every_node)
{
	const struct audio_stream_config *seen;

	source_state.frames_total = 1U;

	zassert_equal(audio_pipeline_start(&test_pipeline), 0, "start failed");

	/* Each fake snapshots audio_node.pipeline_format inside its own open(),
	 * so a NULL here means the node was opened before the pipeline installed
	 * the format - the ordering spec §5.2 forbids. Every node is checked,
	 * not only the sink: the walk is sink-first then upstream, so a format
	 * installed at the wrong point in the loop would miss one end.
	 */
	seen = atomic_ptr_get(&sink_state.seen_format);
	zassert_not_null(seen, "the sink was opened without a format");
	zassert_equal_ptr(atomic_ptr_get(&filter_state.seen_format), seen,
			  "the filter was opened without the pipeline's format");
	zassert_equal_ptr(atomic_ptr_get(&source_state.seen_format), seen,
			  "the source was opened without the pipeline's format");

	/* It carries what the application bound... */
	zassert_equal(seen->sample_rate_hz, test_format.sample_rate_hz, "wrong sample rate");
	zassert_equal(seen->channels, test_format.channels, "wrong channel count");
	zassert_equal(seen->valid_bits_per_sample, test_format.valid_bits_per_sample,
		      "wrong valid bits");
	zassert_equal(seen->format, test_format.format, "wrong container format");

	/* ...out of storage the pipeline owns, not out of the caller's struct,
	 * which set_format() is free to treat as a temporary.
	 */
	zassert_true(seen != &test_format, "the pipeline kept a pointer to the caller's struct");

	/* And the field is still installed while the node is open. */
	zassert_equal_ptr(test_sink.pipeline_format, seen, "the sink lost its format after open()");
	zassert_equal_ptr(test_source.pipeline_format, seen,
			  "the source lost its format after open()");
}

ZTEST_SUITE(audio_pipeline_lifecycle, NULL, NULL, lifecycle_before, lifecycle_after, NULL);
