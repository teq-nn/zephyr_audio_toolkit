#ifndef ZEPHYR_AUDIO_INTERNAL_H_
#define ZEPHYR_AUDIO_INTERNAL_H_

#include <zephyr/kernel.h>

#include <zephyr/audio/audio_pipeline.h>

#define AUDIO_PIPELINE_STACK_SIZE CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE
#define AUDIO_PIPELINE_PRIORITY CONFIG_AUDIO_PIPELINE_THREAD_PRIO
#define AUDIO_PIPELINE_MAX_FRAME_SAMPLES CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES

void audio_pipeline_publish_event(const struct audio_pipeline *pipeline,
				  enum audio_pipeline_event_type type, int err);

#endif /* ZEPHYR_AUDIO_INTERNAL_H_ */
