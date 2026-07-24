/*
 * Event delivery: the per-instance k_msgq that audio_pipeline_get_event()
 * reads, plus the optional callback (spec §3.3/§8.3, manifest §8).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/audio/audio_pipeline.h>
#include <zephyr/audio/audio_pipeline_events.h>

#include "audio_internal.h"

LOG_MODULE_DECLARE(audio_pipeline_core, LOG_LEVEL_INF);

/* Built-in event slots for the single-instance case, the counterpart of the
 * built-in stack and frame buffer in audio_pipeline_core.c. A pipeline that
 * brings its own slots (AUDIO_PIPELINE_DEFINE) never touches these.
 */
static struct audio_pipeline_event default_event_slots[AUDIO_PIPELINE_EVENT_QUEUE_DEPTH];

void audio_pipeline_event_queue_init(struct audio_pipeline *pipeline)
{
	if (pipeline->event_slots == NULL) {
		pipeline->event_slots = default_event_slots;
		pipeline->event_slot_count = ARRAY_SIZE(default_event_slots);
	}

	/* Also purges whatever a previous life of this instance left behind. */
	k_msgq_init(&pipeline->event_msgq, (char *)pipeline->event_slots,
		    sizeof(struct audio_pipeline_event), (uint32_t)pipeline->event_slot_count);
}

void audio_pipeline_publish_event(struct audio_pipeline *pipeline,
				  enum audio_pipeline_event_type type, int err)
{
	struct audio_pipeline_event evt = {
		.type = type,
		.err = err,
	};

	if (!pipeline) {
		return;
	}

	/* Callback first, queue second, and deliberately so: a queue reader may
	 * well have a higher priority than the publishing worker thread, so it
	 * runs the moment the message lands. Publishing to the queue last is
	 * what makes the event mean "the pipeline is done with this" - chain
	 * quiesced on the error path (spec §9.2) and the callback already
	 * invoked - instead of "the pipeline is halfway through publishing".
	 */
	if (pipeline->config && pipeline->config->event_cb) {
		pipeline->config->event_cb(&evt, pipeline->config->event_user_data);
	}

	/* Never a blocking put(): the worker must not stall because the
	 * application is slow to drain the queue. A full queue therefore drops
	 * the newest event and keeps the oldest ones, which is where the first
	 * error - the one that explains all the others - sits.
	 */
	if (pipeline->initialized && k_msgq_put(&pipeline->event_msgq, &evt, K_NO_WAIT) != 0) {
		LOG_WRN("event queue full, dropped event %d (err %d)", (int)type, err);
	}
}

int audio_pipeline_get_event(struct audio_pipeline *pipeline,
			     struct audio_pipeline_event *event, k_timeout_t timeout)
{
	if (!pipeline || !event || !pipeline->initialized) {
		return -EINVAL;
	}

	return k_msgq_get(&pipeline->event_msgq, event, timeout);
}
