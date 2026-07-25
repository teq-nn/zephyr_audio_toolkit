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
 * Map @p err onto a code that cannot be mistaken for end of stream, i.e. away
 * from -EPIPE.
 *
 * -EPIPE is how audio_pipeline_process_frame() tells the worker thread that the
 * stream ended, so a failure carrying it would reach the application as a clean
 * EOF. Every boundary an error can enter the subsystem through funnels its
 * result through here - an upstream node in audio_node_pull(), a filesystem in
 * the file nodes, the sink in audio_pipeline_process_frame() - so the rule has
 * exactly one implementation.
 *
 * @return @p err, or -EIO if @p err was -EPIPE.
 */
int audio_eof_safe_errno(int err);

/*
 * Publish one event on the pipeline's queue and, if one is registered, to the
 * callback. Never blocks, so it is safe to call from the worker thread.
 */
void audio_pipeline_publish_event(struct audio_pipeline *pipeline,
				  enum audio_pipeline_event_type type, int err);

/* Bind the instance's event slots to its k_msgq. The slots must already be
 * installed - audio_pipeline_init() claims the built-in ones for an instance
 * that brought none of its own before it calls this.
 */
void audio_pipeline_event_queue_init(struct audio_pipeline *pipeline);

#endif /* ZEPHYR_AUDIO_INTERNAL_H_ */
