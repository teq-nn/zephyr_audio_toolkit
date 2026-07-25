#ifndef ZEPHYR_AUDIO_INTERNAL_H_
#define ZEPHYR_AUDIO_INTERNAL_H_

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <zephyr/audio/audio_pipeline.h>

#define AUDIO_PIPELINE_STACK_SIZE CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE
#define AUDIO_PIPELINE_PRIORITY CONFIG_AUDIO_PIPELINE_THREAD_PRIO
#define AUDIO_PIPELINE_MAX_FRAME_SAMPLES CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES

/* Upper bound on the sink -> upstream walk. A malformed (e.g. cyclic) chain
 * must not turn open/close into an endless loop.
 */
#define AUDIO_PIPELINE_MAX_CHAIN_DEPTH 16

/*
 * The one lifecycle state of a pipeline instance (spec §8.2).
 *
 * Everything the control API used to keep in a separate boolean - initialised,
 * node chain open, worker thread alive, worker pulling - is a function of this
 * value, so a pipeline can no longer be playing without a thread or hold an
 * open chain while uninitialised. The legal moves between the values live in
 * exactly one place: the transition table in audio_pipeline_core.c.
 *
 *   UNINIT   Nothing bound yet. Zero, so a zero-initialised instance is
 *            uninitialised by construction and every entry point refuses it
 *            with -EINVAL.
 *   INIT     Bound to a configuration and a sink. No worker thread, node chain
 *            closed. This is also where audio_pipeline_join() leaves an
 *            instance, which is why a joined pipeline can be started again.
 *   OPEN     Worker thread alive, node chain open, not pulling frames.
 *   PLAYING  Worker thread pulling frames.
 *   CLOSED   Worker thread alive, node chain closed. Where a node error leaves
 *            the pipeline: the worker tears the chain down and parks, and
 *            audio_pipeline_start() reopens the chain onto the same thread
 *            (spec §9.2).
 *
 * CLOSED is the one state issue #17's sketch does not name - it folds the node
 * error path into Init. It cannot be Init: Init means "no worker thread", and
 * spec §3.1 keeps the worker alive across a node error, which
 * audio_pipeline_is_running() has always reported. Merging the two would
 * change that public answer, so the state was added instead.
 */
enum audio_pipeline_state {
	AUDIO_PIPELINE_STATE_UNINIT = 0,
	AUDIO_PIPELINE_STATE_INIT,
	AUDIO_PIPELINE_STATE_OPEN,
	AUDIO_PIPELINE_STATE_PLAYING,
	AUDIO_PIPELINE_STATE_CLOSED,
};

/* Read the lifecycle state of @p pipeline. */
static inline enum audio_pipeline_state
audio_pipeline_state_get(const struct audio_pipeline *pipeline)
{
	return (enum audio_pipeline_state)atomic_get(&pipeline->state);
}

/*
 * Map @p err onto a code that cannot be mistaken for end of stream, i.e. away
 * from -EPIPE (manifest §7, spec §9.2).
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
