#include <zephyr/ztest.h>

#include <zephyr/audio/audio_pipeline.h>
#include <zephyr/audio/audio_pipeline_events.h>

extern const struct audio_node_ops null_sink_node_ops;

static void test_event_handler(const struct audio_pipeline_event *event, void *user_data)
{
	ARG_UNUSED(event);
	ARG_UNUSED(user_data);
}

ZTEST(audio_pipeline, test_pipeline_start_stop)
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
	struct audio_pipeline pipeline;

	zassert_true(audio_pipeline_config_is_valid(&cfg), "config must be valid");
	zassert_equal(audio_pipeline_init(&pipeline, &cfg, &sink), 0, "init failed");
	zassert_equal(audio_pipeline_start(&pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_stop(&pipeline), 0, "stop failed");
}

ZTEST_SUITE(audio_pipeline, NULL, NULL, NULL, NULL, NULL);
