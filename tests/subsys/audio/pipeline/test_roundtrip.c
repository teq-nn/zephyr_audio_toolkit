#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <zephyr/audio/audio_nodes.h>
#include <zephyr/audio/audio_pipeline.h>
#include <zephyr/audio/audio_pipeline_events.h>

static void test_event_handler(const struct audio_pipeline_event *event, void *user_data)
{
	ARG_UNUSED(event);
	ARG_UNUSED(user_data);
}

ZTEST(audio_pipeline, test_pipeline_start_play_stop_join)
{
	struct audio_node sink = {
		.role = AUDIO_NODE_ROLE_SINK,
		.ops = &null_sink_node_ops,
		.upstream = NULL,
		.state = NULL,
	};
	struct audio_pipeline_config cfg = {
		.stream = {
			.sample_rate_hz = 44100U,
			.channels = 2U,
			.valid_bits_per_sample = 24U,
			.format = AUDIO_SAMPLE_FORMAT_S32_LE,
		},
		.frame_samples = CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES,
		.event_cb = test_event_handler,
		.event_user_data = NULL,
	};
	struct audio_pipeline pipeline = {0};

	zassert_true(audio_pipeline_config_is_valid(&cfg), "config must be valid");
	zassert_equal(audio_pipeline_init(&pipeline, &cfg, &sink), 0, "init failed");
	zassert_equal(audio_pipeline_start(&pipeline), 0, "start failed");
	zassert_true(audio_pipeline_is_running(&pipeline), "worker thread missing");

	/* A sink without upstream reports EOF right away; the thread survives. */
	zassert_equal(audio_pipeline_play(&pipeline), 0, "play failed");
	k_msleep(20);
	zassert_true(audio_pipeline_is_running(&pipeline), "EOF killed the worker thread");
	zassert_false(audio_pipeline_is_playing(&pipeline), "still playing after EOF");

	zassert_equal(audio_pipeline_stop(&pipeline), 0, "stop failed");
	zassert_equal(audio_pipeline_join(&pipeline), 0, "join failed");
	zassert_false(audio_pipeline_is_running(&pipeline), "worker thread still running");
}

ZTEST_SUITE(audio_pipeline, NULL, NULL, NULL, NULL, NULL);
