#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/audio/audio_pipeline.h>
#include <zephyr/audio/audio_pipeline_events.h>

#include "audio_internal.h"

LOG_MODULE_REGISTER(audio_pipeline_core, LOG_LEVEL_INF);

static K_THREAD_STACK_DEFINE(pipeline_stack, AUDIO_PIPELINE_STACK_SIZE);
static struct k_thread pipeline_thread_data;

static void pipeline_thread(void *p1, void *p2, void *p3)
{
	struct audio_pipeline *pipeline = (struct audio_pipeline *)p1;
	int ret;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (!pipeline || !pipeline->sink) {
		return;
	}

	while (pipeline->running) {
		ret = audio_pipeline_process_frame(pipeline);
		if (ret == -EPIPE) {
			audio_pipeline_publish_event(pipeline, AUDIO_PIPELINE_EVENT_EOF, 0);
			pipeline->running = false;
		} else if (ret < 0) {
			audio_pipeline_publish_event(pipeline, AUDIO_PIPELINE_EVENT_ERROR, ret);
			pipeline->running = false;
		}

		k_yield();
	}
}

int audio_pipeline_init(struct audio_pipeline *pipeline,
			const struct audio_pipeline_config *config,
			struct audio_node *sink)
{
	if (!pipeline || !config || !sink) {
		return -EINVAL;
	}

	if (!audio_pipeline_config_is_valid(config)) {
		return -EINVAL;
	}

	pipeline->config = config;
	pipeline->sink = sink;
	pipeline->running = false;

	return 0;
}

int audio_pipeline_start(struct audio_pipeline *pipeline)
{
	if (!pipeline || !pipeline->sink) {
		return -EINVAL;
	}

	if (pipeline->running) {
		return 0;
	}

	pipeline->running = true;

	k_thread_create(&pipeline_thread_data, pipeline_stack,
			K_THREAD_STACK_SIZEOF(pipeline_stack), pipeline_thread,
			pipeline, NULL, NULL, AUDIO_PIPELINE_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&pipeline_thread_data, "audio_pipeline");

	return 0;
}

int audio_pipeline_stop(struct audio_pipeline *pipeline)
{
	if (!pipeline) {
		return -EINVAL;
	}

	pipeline->running = false;
	return 0;
}

int audio_pipeline_process_frame(struct audio_pipeline *pipeline)
{
	static int32_t frame_buf[AUDIO_PIPELINE_MAX_FRAME_SAMPLES];
	struct audio_buffer_view view = {
		.data = frame_buf,
		.capacity = AUDIO_PIPELINE_MAX_FRAME_SAMPLES,
		.size = 0,
	};
	size_t produced = 0;
	int ret;

	if (!pipeline || !pipeline->sink) {
		return -EINVAL;
	}

	if (!pipeline->sink->ops || !pipeline->sink->ops->process) {
		return -ENOSYS;
	}

	ret = audio_node_process(pipeline->sink, &view, &produced);
	if (ret < 0) {
		return ret;
	}

	if (produced == 0) {
		return -EPIPE;
	}

	view.size = produced;
	return 0;
}
