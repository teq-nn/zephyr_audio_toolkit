#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/audio/audio_pipeline.h>
#include <zephyr/audio/audio_pipeline_events.h>

extern const struct audio_node_ops null_sink_node_ops;

static void pipeline_event_handler(const struct audio_pipeline_event *event, void *user_data)
{
	ARG_UNUSED(user_data);

	if (!event) {
		return;
	}

	switch (event->type) {
	case AUDIO_PIPELINE_EVENT_EOF:
		printk("pipeline: EOF\n");
		break;
	case AUDIO_PIPELINE_EVENT_ERROR:
		printk("pipeline: error %d\n", event->err);
		break;
	case AUDIO_PIPELINE_EVENT_RECONFIG:
		printk("pipeline: reconfig\n");
		break;
	default:
		printk("pipeline: unknown event\n");
		break;
	}
}

void main(void)
{
	static struct audio_node sink = {
		.role = AUDIO_NODE_ROLE_SINK,
		.ops = &null_sink_node_ops,
		.upstream = NULL,
		.state = NULL,
	};
	static const struct audio_pipeline_config cfg = {
		.stream = {
			.sample_rate_hz = 48000U,
			.channels = 2U,
			.valid_bits_per_sample = 24U,
			.format = AUDIO_SAMPLE_FORMAT_S32_LE,
		},
		.frame_samples = CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES,
		.event_cb = pipeline_event_handler,
		.event_user_data = NULL,
	};
	static struct audio_pipeline pipeline;

	if (audio_pipeline_init(&pipeline, &cfg, &sink) == 0) {
		(void)audio_pipeline_start(&pipeline);
		k_msleep(20);
		(void)audio_pipeline_stop(&pipeline);
	} else {
		printk("pipeline: init failed\n");
	}
}
