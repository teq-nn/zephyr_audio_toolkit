/*
 * Static definition macros and multi-instance isolation.
 *
 * Everything here is built from AUDIO_PIPELINE_DEFINE(), AUDIO_NODE_DEFINE()
 * and the per-node *_NODE_DEFINE() macros, so the suite fails if any of them
 * ever shares static storage between instances.
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
#include <zephyr/audio/audio_nodes.h>
#include <zephyr/audio/audio_pipeline.h>
#include <zephyr/audio/audio_pipeline_events.h>

#include "fake_nodes.h"
#include "wav_fixture.h"

/* Two pipelines deliberately get different frame sizes: a shared frame buffer
 * would have to pick one of them.
 */
#define PIPELINE_A_FRAME_SAMPLES 32
#define PIPELINE_B_FRAME_SAMPLES 16

#define CONCURRENT_FRAMES 24U

#define TEST_EVENT_TIMEOUT K_MSEC(2000)

/*
 * Two statically wired chains, source -> sink, one per pipeline, built from the
 * shared fakes. Cross-talk between the pipelines is what the probes make
 * visible: each source stamps every sample with its own pattern and each sink
 * verifies that nothing else wrote into the frame it was handed. The DECLARE
 * macro is what another translation unit would use to reach a node; spelling it
 * out here as well keeps that form covered by the build.
 */
AUDIO_NODE_DECLARE(probe_sink_a);
AUDIO_FAKE_SOURCE_DEFINE(probe_source_a);
AUDIO_FAKE_SINK_DEFINE(probe_sink_a, &probe_source_a);

AUDIO_FAKE_SOURCE_DEFINE(probe_source_b);
AUDIO_FAKE_SINK_DEFINE(probe_sink_b, &probe_source_b);

/* Lock-step barriers: released by one pipeline, waited on by the other. */
static struct k_sem probe_a_filled;
static struct k_sem probe_b_filled;
static struct k_sem probe_a_eof;
static struct k_sem probe_b_eof;

static void probe_event_cb(const struct audio_pipeline_event *event, void *user_data)
{
	struct k_sem *eof = user_data;

	if (event->type == AUDIO_PIPELINE_EVENT_EOF) {
		k_sem_give(eof);
	}
}

static const struct audio_pipeline_config config_a = {
	.stream = {
		.sample_rate_hz = 48000U,
		.channels = 2U,
		.valid_bits_per_sample = 24U,
		.format = AUDIO_SAMPLE_FORMAT_S32_LE,
	},
	.frame_samples = PIPELINE_A_FRAME_SAMPLES,
	.event_cb = probe_event_cb,
	.event_user_data = &probe_a_eof,
};

static const struct audio_pipeline_config config_b = {
	.stream = {
		.sample_rate_hz = 48000U,
		.channels = 2U,
		.valid_bits_per_sample = 24U,
		.format = AUDIO_SAMPLE_FORMAT_S32_LE,
	},
	.frame_samples = PIPELINE_B_FRAME_SAMPLES,
	.event_cb = probe_event_cb,
	.event_user_data = &probe_b_eof,
};

AUDIO_PIPELINE_DECLARE(pipeline_a);
AUDIO_PIPELINE_DEFINE(pipeline_a, PIPELINE_A_FRAME_SAMPLES,
		      CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE, CONFIG_AUDIO_PIPELINE_THREAD_PRIO);
AUDIO_PIPELINE_DEFINE(pipeline_b, PIPELINE_B_FRAME_SAMPLES,
		      CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE, CONFIG_AUDIO_PIPELINE_THREAD_PRIO);

/* A hand-rolled instance, used to prove that the macro instances do not fall
 * back to the subsystem's built-in single-instance resources.
 */
static struct audio_pipeline builtin_pipeline;

/* Node instances for the per-node state tests: an endless source of one
 * constant container value, so a filter's own arithmetic is all that changes
 * between two instances.
 */
AUDIO_FAKE_SOURCE_DEFINE(const_source);
AUDIO_GAIN_FILTER_NODE_DEFINE(gain_half, &const_source, AUDIO_GAIN_UNITY_Q15 / 2);
AUDIO_GAIN_FILTER_NODE_DEFINE(gain_unity, &const_source, AUDIO_GAIN_UNITY_Q15);
AUDIO_NULL_SINK_NODE_DEFINE(half_sink, &gain_half);

/* Two readers on two real files: the reader opens what its own state points at
 * (see wav_fixture.h for how the filesystem is provided).
 */
AUDIO_FILE_READER_NODE_DEFINE(reader_a, AUDIO_TEST_PATH("a.wav"));
AUDIO_FILE_READER_NODE_DEFINE(reader_b, AUDIO_TEST_PATH("b.wav"));

/* Exactly one frame of the 8 sample buffer the reader case below uses, so the
 * frame after the first one is EOF.
 */
static const int16_t reader_payload[8] = {1, -1, 2, -2, 3, -3, 4, -4};

/* Wipe one probe chain and give the source and the sink a shared pattern, so a
 * frame written by the peer pipeline cannot pass the sink's check.
 */
static void probe_reset(struct audio_fake_source *src, struct audio_fake_sink *sink,
			int32_t pattern, size_t expected_capacity)
{
	audio_fake_source_reset(src);
	audio_fake_sink_reset(sink);

	src->pattern = pattern;

	sink->check_pattern = true;
	sink->expect_pattern = pattern;
	sink->expect_capacity = expected_capacity;
}

static void static_define_before(void *fixture)
{
	ARG_UNUSED(fixture);

	/* The macro instances must NOT be memset here: the macro is what fills
	 * their resource fields, and wiping them would hand the pipelines back
	 * to the built-in defaults.
	 */
	probe_reset(&probe_source_a_state, &probe_sink_a_state, 0x0a0a0a0a,
		    PIPELINE_A_FRAME_SAMPLES);
	probe_reset(&probe_source_b_state, &probe_sink_b_state, 0x0b0b0b0b,
		    PIPELINE_B_FRAME_SAMPLES);

	audio_fake_source_reset(&const_source_state);
	const_source_state.frames_total = AUDIO_FAKE_ENDLESS;

	k_sem_init(&probe_a_filled, 0, CONCURRENT_FRAMES + 1U);
	k_sem_init(&probe_b_filled, 0, CONCURRENT_FRAMES + 1U);
	k_sem_init(&probe_a_eof, 0, 4);
	k_sem_init(&probe_b_eof, 0, 4);
}

static void static_define_after(void *fixture)
{
	ARG_UNUSED(fixture);

	/* Never leave a worker thread behind; release both barriers first in
	 * case a source is still parked on one.
	 */
	(void)audio_pipeline_stop(&pipeline_a);
	(void)audio_pipeline_stop(&pipeline_b);
	k_sem_give(&probe_a_filled);
	k_sem_give(&probe_b_filled);
	(void)audio_pipeline_join(&pipeline_a);
	(void)audio_pipeline_join(&pipeline_b);

	/* The hand-rolled instance holds the subsystem's built-in stack, frame
	 * buffer and event slots once a test has initialised it; join() is what
	 * hands them back to the next hand-rolled pipeline.
	 */
	(void)audio_pipeline_join(&builtin_pipeline);
}

ZTEST(audio_pipeline_static_define, test_pipeline_define_allocates_per_instance_resources)
{
	const int32_t *buf_a = pipeline_a.frame_buf;
	const int32_t *buf_b = pipeline_b.frame_buf;
	bool buffers_disjoint = buf_a + PIPELINE_A_FRAME_SAMPLES <= buf_b ||
				buf_b + PIPELINE_B_FRAME_SAMPLES <= buf_a;

	zassert_equal(audio_pipeline_init(&pipeline_a, &config_a, &probe_sink_a), 0,
		      "init A failed");
	zassert_equal(audio_pipeline_init(&pipeline_b, &config_b, &probe_sink_b), 0,
		      "init B failed");

	/* A zeroed instance still gets the built-in single-instance resources,
	 * and those are not the ones the macro handed out.
	 */
	memset(&builtin_pipeline, 0, sizeof(builtin_pipeline));
	zassert_equal(audio_pipeline_init(&builtin_pipeline, &config_a, &probe_sink_a), 0,
		      "init of the built-in instance failed");

	zassert_not_null(pipeline_a.stack, "macro left the stack unset");
	zassert_not_null(pipeline_b.stack, "macro left the stack unset");
	zassert_true(pipeline_a.stack != pipeline_b.stack, "instances share a thread stack");
	/* Both stacks were declared with the same Kconfig size as the built-in
	 * one, so the usable sizes have to match exactly.
	 */
	zassert_equal(pipeline_a.stack_size, builtin_pipeline.stack_size, "wrong stack size A");
	zassert_equal(pipeline_b.stack_size, builtin_pipeline.stack_size, "wrong stack size B");
	zassert_equal(pipeline_a.priority, CONFIG_AUDIO_PIPELINE_THREAD_PRIO, "wrong priority");

	zassert_not_null(pipeline_a.frame_buf, "macro left the frame buffer unset");
	zassert_not_null(pipeline_b.frame_buf, "macro left the frame buffer unset");
	zassert_true(pipeline_a.frame_buf != pipeline_b.frame_buf,
		     "instances share the frame buffer");
	zassert_true(buffers_disjoint, "frame buffers overlap");

	/* Per-instance sizing survives init(), which only clamps to the
	 * configured frame size.
	 */
	zassert_equal(pipeline_a.frame_capacity, PIPELINE_A_FRAME_SAMPLES, "wrong capacity A");
	zassert_equal(pipeline_b.frame_capacity, PIPELINE_B_FRAME_SAMPLES, "wrong capacity B");

	zassert_true(&pipeline_a.thread != &pipeline_b.thread, "instances share a thread struct");

	zassert_not_null(builtin_pipeline.stack, "built-in stack missing");
	zassert_true(builtin_pipeline.stack != pipeline_a.stack,
		     "macro instance fell back to the built-in stack");
	zassert_true(builtin_pipeline.frame_buf != pipeline_a.frame_buf,
		     "macro instance fell back to the built-in frame buffer");
}

ZTEST(audio_pipeline_static_define, test_pipeline_define_allocates_per_instance_event_queue)
{
	const struct audio_pipeline_event *slots_a = pipeline_a.event_slots;
	const struct audio_pipeline_event *slots_b = pipeline_b.event_slots;
	bool slots_disjoint;

	zassert_equal(audio_pipeline_init(&pipeline_a, &config_a, &probe_sink_a), 0,
		      "init A failed");
	zassert_equal(audio_pipeline_init(&pipeline_b, &config_b, &probe_sink_b), 0,
		      "init B failed");

	memset(&builtin_pipeline, 0, sizeof(builtin_pipeline));
	zassert_equal(audio_pipeline_init(&builtin_pipeline, &config_a, &probe_sink_a), 0,
		      "init of the built-in instance failed");

	zassert_not_null(slots_a, "macro left the event slots unset");
	zassert_not_null(slots_b, "macro left the event slots unset");
	zassert_equal(pipeline_a.event_slot_count, AUDIO_PIPELINE_EVENT_QUEUE_DEPTH,
		      "wrong event queue depth A");
	zassert_equal(pipeline_b.event_slot_count, AUDIO_PIPELINE_EVENT_QUEUE_DEPTH,
		      "wrong event queue depth B");

	slots_disjoint = slots_a + pipeline_a.event_slot_count <= slots_b ||
			 slots_b + pipeline_b.event_slot_count <= slots_a;
	zassert_true(slots_a != slots_b, "instances share their event queue storage");
	zassert_true(slots_disjoint, "event queue storages overlap");

	/* Each instance drives its own k_msgq, and neither of them fell back to
	 * the subsystem's built-in slots.
	 */
	zassert_true(&pipeline_a.event_msgq != &pipeline_b.event_msgq,
		     "instances share a k_msgq object");
	zassert_true(builtin_pipeline.event_slots != slots_a,
		     "macro instance fell back to the built-in event slots");
	zassert_true(builtin_pipeline.event_slots != slots_b,
		     "macro instance fell back to the built-in event slots");
}

ZTEST(audio_pipeline_static_define, test_pipeline_define_keeps_events_per_instance)
{
	struct audio_pipeline_event event;

	zassert_equal(audio_pipeline_init(&pipeline_a, &config_a, &probe_sink_a), 0,
		      "init A failed");
	zassert_equal(audio_pipeline_init(&pipeline_b, &config_b, &probe_sink_b), 0,
		      "init B failed");

	/* Distinguishable events on purpose: two identical EOFs would still
	 * look right if both instances wrote into one ring buffer, but two
	 * different error codes cannot.
	 */
	probe_sink_a_state.open_ret = -EIO;
	probe_sink_b_state.open_ret = -ENOMEM;

	zassert_equal(audio_pipeline_start(&pipeline_a), -EIO, "A did not report its open error");
	zassert_equal(audio_pipeline_start(&pipeline_b), -ENOMEM,
		      "B did not report its open error");

	zassert_equal(audio_pipeline_get_event(&pipeline_a, &event, K_NO_WAIT), 0,
		      "pipeline A queued no ERROR event");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_ERROR, "wrong event type on queue A");
	zassert_equal(event.err, -EIO, "pipeline A read pipeline B's event");
	zassert_equal(audio_pipeline_get_event(&pipeline_a, &event, K_NO_WAIT), -ENOMSG,
		      "queue A also received pipeline B's event");

	zassert_equal(audio_pipeline_get_event(&pipeline_b, &event, K_NO_WAIT), 0,
		      "pipeline B queued no ERROR event");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_ERROR, "wrong event type on queue B");
	zassert_equal(event.err, -ENOMEM, "pipeline B read pipeline A's event");
	zassert_equal(audio_pipeline_get_event(&pipeline_b, &event, K_NO_WAIT), -ENOMSG,
		      "queue B also received pipeline A's event");

	/* Same story for the worker thread's EOF events: one per instance. */
	probe_sink_a_state.open_ret = 0;
	probe_sink_b_state.open_ret = 0;
	probe_source_a_state.frames_total = 2U;
	probe_source_b_state.frames_total = 4U;

	zassert_equal(audio_pipeline_start(&pipeline_a), 0, "restart A failed");
	zassert_equal(audio_pipeline_start(&pipeline_b), 0, "restart B failed");
	zassert_equal(audio_pipeline_play(&pipeline_a), 0, "play A failed");
	zassert_equal(audio_pipeline_play(&pipeline_b), 0, "play B failed");

	zassert_equal(audio_pipeline_get_event(&pipeline_a, &event, TEST_EVENT_TIMEOUT), 0,
		      "pipeline A never queued its EOF");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_EOF, "wrong event type on queue A");
	zassert_equal(audio_pipeline_get_event(&pipeline_b, &event, TEST_EVENT_TIMEOUT), 0,
		      "pipeline B never queued its EOF");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_EOF, "wrong event type on queue B");

	zassert_equal(audio_pipeline_get_event(&pipeline_a, &event, K_NO_WAIT), -ENOMSG,
		      "queue A saw more than its own EOF");
	zassert_equal(audio_pipeline_get_event(&pipeline_b, &event, K_NO_WAIT), -ENOMSG,
		      "queue B saw more than its own EOF");

	zassert_equal(atomic_get(&probe_sink_a_state.frames_seen), 2U, "pipeline A lost frames");
	zassert_equal(atomic_get(&probe_sink_b_state.frames_seen), 4U, "pipeline B lost frames");
}

ZTEST(audio_pipeline_static_define, test_pipeline_define_processes_on_its_own_buffer)
{
	probe_source_a_state.frames_total = 1U;

	zassert_equal(audio_pipeline_init(&pipeline_a, &config_a, &probe_sink_a), 0,
		      "init A failed");
	zassert_equal(audio_pipeline_process_frame(&pipeline_a), 0, "frame not produced");

	zassert_equal_ptr(atomic_ptr_get(&probe_sink_a_state.seen_buf), pipeline_a.frame_buf,
			  "chain did not run on the instance's own frame buffer");
	zassert_equal(atomic_get(&probe_sink_a_state.frames_seen), 1U, "sink saw no frame");
	zassert_equal(atomic_get(&probe_sink_a_state.corrupt_frames), 0U,
		      "frame content corrupted");
	zassert_equal(atomic_get(&probe_sink_a_state.wrong_capacity), 0U,
		      "sink saw the wrong frame capacity");
}

ZTEST(audio_pipeline_static_define, test_two_pipelines_run_concurrently)
{
	struct audio_fake_sink *sink_a = &probe_sink_a_state;
	struct audio_fake_sink *sink_b = &probe_sink_b_state;

	probe_source_a_state.frames_total = CONCURRENT_FRAMES;
	probe_source_b_state.frames_total = CONCURRENT_FRAMES;
	/* Each source releases its own barrier and then waits on the peer's, so
	 * both workers are pinned to the same frame.
	 */
	probe_source_a_state.frame_sem = &probe_a_filled;
	probe_source_a_state.peer_sem = &probe_b_filled;
	probe_source_b_state.frame_sem = &probe_b_filled;
	probe_source_b_state.peer_sem = &probe_a_filled;

	zassert_equal(audio_pipeline_init(&pipeline_a, &config_a, &probe_sink_a), 0,
		      "init A failed");
	zassert_equal(audio_pipeline_init(&pipeline_b, &config_b, &probe_sink_b), 0,
		      "init B failed");

	zassert_equal(audio_pipeline_start(&pipeline_a), 0, "start A failed");
	zassert_equal(audio_pipeline_start(&pipeline_b), 0, "start B failed");

	/* Both workers exist before either of them runs (the ztest thread is
	 * cooperative), so the barrier in the source really does force the two
	 * pipelines through their frames in lock-step.
	 */
	zassert_equal(audio_pipeline_play(&pipeline_a), 0, "play A failed");
	zassert_equal(audio_pipeline_play(&pipeline_b), 0, "play B failed");

	zassert_equal(k_sem_take(&probe_a_eof, TEST_EVENT_TIMEOUT), 0, "pipeline A never hit EOF");
	zassert_equal(k_sem_take(&probe_b_eof, TEST_EVENT_TIMEOUT), 0, "pipeline B never hit EOF");

	zassert_equal(atomic_get(&probe_source_a_state.peer_timeouts), 0U,
		      "pipeline A did not run alongside B");
	zassert_equal(atomic_get(&probe_source_b_state.peer_timeouts), 0U,
		      "pipeline B did not run alongside A");

	zassert_equal(atomic_get(&sink_a->frames_seen), CONCURRENT_FRAMES,
		      "pipeline A lost frames");
	zassert_equal(atomic_get(&sink_b->frames_seen), CONCURRENT_FRAMES,
		      "pipeline B lost frames");
	zassert_equal(atomic_get(&sink_a->corrupt_frames), 0U, "pipeline B wrote into A's frames");
	zassert_equal(atomic_get(&sink_b->corrupt_frames), 0U, "pipeline A wrote into B's frames");
	zassert_equal(atomic_get(&sink_a->wrong_capacity), 0U, "pipeline A ran with B's capacity");
	zassert_equal(atomic_get(&sink_b->wrong_capacity), 0U, "pipeline B ran with A's capacity");

	zassert_true(atomic_ptr_get(&sink_a->worker) != atomic_ptr_get(&sink_b->worker),
		     "both pipelines ran on one thread");
	zassert_true(atomic_ptr_get(&sink_a->worker) != k_current_get(),
		     "pipeline A ran on the test thread");

	zassert_true(audio_pipeline_is_running(&pipeline_a), "EOF killed worker A");
	zassert_true(audio_pipeline_is_running(&pipeline_b), "EOF killed worker B");

	zassert_equal(audio_pipeline_join(&pipeline_a), 0, "join A failed");
	zassert_true(audio_pipeline_is_running(&pipeline_b), "join A took worker B down");
	zassert_equal(audio_pipeline_join(&pipeline_b), 0, "join B failed");
}

ZTEST(audio_pipeline_static_define, test_node_define_wires_the_chain_statically)
{
	zassert_equal(probe_sink_a.role, AUDIO_NODE_ROLE_SINK, "wrong sink role");
	zassert_equal_ptr(probe_sink_a.ops, &audio_fake_sink_ops, "wrong sink ops");
	zassert_equal_ptr(probe_sink_a.upstream, &probe_source_a, "sink not wired to its source");
	zassert_equal_ptr(probe_source_a.upstream, NULL, "a source must have no upstream");
	zassert_equal_ptr(probe_sink_a.state, &probe_sink_a_state, "wrong sink state");

	zassert_equal(gain_half.role, AUDIO_NODE_ROLE_FILTER, "wrong filter role");
	zassert_equal_ptr(gain_half.upstream, &const_source, "filter not wired to its source");
	zassert_equal(half_sink.role, AUDIO_NODE_ROLE_SINK, "wrong sink role");
	zassert_equal_ptr(half_sink.upstream, &gain_half, "sink not wired to its filter");

	zassert_equal(reader_a.role, AUDIO_NODE_ROLE_SOURCE, "wrong reader role");
	zassert_equal_ptr(reader_a.upstream, NULL, "a source must have no upstream");
}

ZTEST(audio_pipeline_static_define, test_node_define_gain_state_is_per_instance)
{
	int32_t buf[8];
	struct audio_buffer_view view = {
		.data = buf,
		.capacity = ARRAY_SIZE(buf),
	};
	size_t produced = 0;

	zassert_true(gain_half.state != gain_unity.state, "gain filters share their state");

	const_source_state.pattern = 1000;

	zassert_equal(audio_node_open(&gain_half), 0, "open half failed");
	zassert_equal(audio_node_open(&gain_unity), 0, "open unity failed");

	zassert_equal(audio_node_process(&gain_half, &view, &produced), 0, "half process failed");
	zassert_equal(produced, ARRAY_SIZE(buf), "wrong frame size");
	zassert_equal(buf[0], 500, "half-gain instance did not apply its own gain");

	zassert_equal(audio_node_process(&gain_unity, &view, &produced), 0, "unity process failed");
	zassert_equal(buf[0], 1000, "unity instance did not apply its own gain");

	/* The second instance must not have rewritten the first one's gain. */
	zassert_equal(audio_node_process(&gain_half, &view, &produced), 0, "half reprocess failed");
	zassert_equal(buf[0], 500, "gain state leaked between instances");

	zassert_equal(audio_node_close(&gain_half), 0, "close half failed");
	zassert_equal(audio_node_close(&gain_unity), 0, "close unity failed");
}

ZTEST(audio_pipeline_static_define, test_node_define_reader_state_is_per_instance)
{
	int32_t buf[8];
	struct audio_buffer_view view = {
		.data = buf,
		.capacity = ARRAY_SIZE(buf),
	};
	size_t produced = 0;
	struct audio_file_reader_state *state_a = reader_a.state;
	struct audio_file_reader_state *state_b = reader_b.state;

	zassert_not_null(state_a, "reader A has no state");
	zassert_not_null(state_b, "reader B has no state");
	zassert_true(state_a != state_b, "reader instances share their state");
	zassert_str_equal(state_a->path, AUDIO_TEST_PATH("a.wav"),
			  "reader A did not keep its own path");
	zassert_str_equal(state_b->path, AUDIO_TEST_PATH("b.wav"),
			  "reader B did not keep its own path");

	zassert_equal(audio_test_fs_mount(), 0, "fixture filesystem did not mount");
	zassert_equal(audio_test_write_wav_s16(AUDIO_TEST_PATH("a.wav"), reader_payload,
					       ARRAY_SIZE(reader_payload)),
		      0, "could not write reader A's file");
	zassert_equal(audio_test_write_wav_s16(AUDIO_TEST_PATH("b.wav"), reader_payload,
					       ARRAY_SIZE(reader_payload)),
		      0, "could not write reader B's file");

	/* Both readers are opened up front, so running one of them to EOF must
	 * not be observable on the other.
	 */
	zassert_equal(audio_node_open(&reader_a), 0, "open A failed");
	zassert_equal(audio_node_open(&reader_b), 0, "open B failed");

	zassert_equal(audio_node_process(&reader_a, &view, &produced), 0, "A process failed");
	zassert_true(produced > 0U, "reader A produced no data");
	zassert_equal(audio_node_process(&reader_a, &view, &produced), 0, "A process failed");
	zassert_equal(produced, 0U, "reader A did not report EOF");

	zassert_equal(audio_node_process(&reader_b, &view, &produced), 0, "B process failed");
	zassert_true(produced > 0U, "reader B inherited the EOF flag of reader A");
	zassert_equal(audio_node_process(&reader_b, &view, &produced), 0, "B process failed");
	zassert_equal(produced, 0U, "reader B did not report EOF");

	zassert_equal(audio_node_close(&reader_a), 0, "close A failed");
	zassert_equal(audio_node_close(&reader_b), 0, "close B failed");
}

ZTEST_SUITE(audio_pipeline_static_define, NULL, NULL, static_define_before, static_define_after,
	    NULL);
