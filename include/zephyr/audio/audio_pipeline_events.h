/*
 * Pipeline events: the queue-based poll API and the optional callback.
 *
 * See audio_pipeline_manifest.md §8 and audio_pipeline_spec_v2.md §3.3/§8.3.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_AUDIO_PIPELINE_EVENTS_H_
#define ZEPHYR_AUDIO_PIPELINE_EVENTS_H_

#include <zephyr/kernel.h>
#include <zephyr/types.h>

/**
 * @brief Number of events a pipeline's message queue can hold.
 *
 * The storage behind it is static per instance: AUDIO_PIPELINE_DEFINE()
 * allocates it, and audio_pipeline_init() falls back to the subsystem's
 * built-in slots for a zero-initialised instance.
 */
#define AUDIO_PIPELINE_EVENT_QUEUE_DEPTH CONFIG_AUDIO_PIPELINE_EVENT_QUEUE_DEPTH

enum audio_pipeline_event_type {
	AUDIO_PIPELINE_EVENT_EOF = 0,
	AUDIO_PIPELINE_EVENT_ERROR,
	AUDIO_PIPELINE_EVENT_RECONFIG,
};

struct audio_pipeline_event {
	enum audio_pipeline_event_type type;
	int err;
};

/**
 * @brief Optional secondary event path.
 *
 * Invoked from the thread that produced the event - the worker thread for EOF
 * and processing errors, the control thread for open/close failures - so it
 * must not block. It runs *before* the event reaches the queue, so a queue
 * reader that has already seen an event knows the callback has returned.
 *
 * The queue is the primary interface; a callback is only worth registering when
 * an event has to be observed synchronously on the publishing thread.
 */
typedef void (*audio_pipeline_event_callback_t)(
	const struct audio_pipeline_event *event, void *user_data);

struct audio_pipeline;

/**
 * @brief Fetch the next event from a pipeline's event queue.
 *
 * Plain @c k_msgq semantics (spec §3.3): the queue may be drained from any
 * thread, events arrive in the order they were published, and @p timeout may
 * be @c K_NO_WAIT, @c K_FOREVER or any finite duration.
 *
 * A full queue drops the *newest* event, so the oldest entries - and with them
 * the first error, the one that explains all the others - are the ones that
 * survive.
 *
 * An event is queued last, once the pipeline has finished reacting to it: on
 * the error path the node chain is already closed (spec §9.2) and any
 * registered callback has already returned by the time this call hands the
 * event over.
 *
 * @param pipeline Pipeline to read from; must have been initialised.
 * @param event    Receives the event on success.
 * @param timeout  Waiting period.
 *
 * @retval 0 an event was written to @p event
 * @retval -EINVAL on a NULL argument or an uninitialised pipeline
 * @retval -ENOMSG the queue was empty and @p timeout was @c K_NO_WAIT
 * @retval -EAGAIN the waiting period expired with no event
 */
int audio_pipeline_get_event(struct audio_pipeline *pipeline,
			     struct audio_pipeline_event *event, k_timeout_t timeout);

#endif /* ZEPHYR_AUDIO_PIPELINE_EVENTS_H_ */
