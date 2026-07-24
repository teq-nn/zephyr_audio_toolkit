#ifndef ZEPHYR_AUDIO_INTERNAL_H_
#define ZEPHYR_AUDIO_INTERNAL_H_

#include <zephyr/kernel.h>

#include <zephyr/audio/audio_pipeline.h>

#define AUDIO_PIPELINE_STACK_SIZE CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE
#define AUDIO_PIPELINE_PRIORITY CONFIG_AUDIO_PIPELINE_THREAD_PRIO
#define AUDIO_PIPELINE_MAX_FRAME_SAMPLES CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES

/* Upper bound on the sink -> upstream walk. A malformed (e.g. cyclic) chain
 * must not turn open/close into an endless loop.
 */
#define AUDIO_PIPELINE_MAX_CHAIN_DEPTH 16

/*
 * Publish one event on the pipeline's queue and, if one is registered, to the
 * callback. Never blocks, so it is safe to call from the worker thread.
 */
void audio_pipeline_publish_event(struct audio_pipeline *pipeline,
				  enum audio_pipeline_event_type type, int err);

/* Bind the instance's (or the built-in) event slots to its k_msgq. */
void audio_pipeline_event_queue_init(struct audio_pipeline *pipeline);

#endif /* ZEPHYR_AUDIO_INTERNAL_H_ */
