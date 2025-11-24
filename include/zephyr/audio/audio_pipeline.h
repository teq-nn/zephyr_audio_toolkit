#ifndef ZEPHYR_AUDIO_PIPELINE_H_
#define ZEPHYR_AUDIO_PIPELINE_H_

#include <stdbool.h>
#include <zephyr/types.h>

#include <zephyr/audio/audio_format.h>
#include <zephyr/audio/audio_node.h>
#include <zephyr/audio/audio_pipeline_events.h>

struct audio_pipeline;

struct audio_pipeline_config {
	struct audio_stream_config stream;
	uint16_t frame_samples;
	audio_pipeline_event_callback_t event_cb;
	void *event_user_data;
};

struct audio_pipeline {
	const struct audio_pipeline_config *config;
	struct audio_node *sink;
	bool running;
};

bool audio_pipeline_config_is_valid(const struct audio_pipeline_config *config);
int audio_pipeline_init(struct audio_pipeline *pipeline,
			const struct audio_pipeline_config *config,
			struct audio_node *sink);
int audio_pipeline_start(struct audio_pipeline *pipeline);
int audio_pipeline_stop(struct audio_pipeline *pipeline);
int audio_pipeline_process_frame(struct audio_pipeline *pipeline);

#endif /* ZEPHYR_AUDIO_PIPELINE_H_ */
