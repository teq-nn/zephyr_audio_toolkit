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

void audio_pipeline_event_queue_init(struct audio_pipeline *pipeline)
{
	/* The slots are the instance's own (AUDIO_PIPELINE_DEFINE) or the
	 * built-in ones audio_pipeline_init() claimed on its behalf; either way
	 * they are installed by the time this runs.
	 *
	 * Also purges whatever a previous life of this instance left behind.
	 */
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
	if (audio_pipeline_state_get(pipeline) != AUDIO_PIPELINE_STATE_UNINIT &&
	    k_msgq_put(&pipeline->event_msgq, &evt, K_NO_WAIT) != 0) {
		LOG_WRN("event queue full, dropped event %d (err %d)", (int)type, err);
	}
}

int audio_pipeline_get_event(struct audio_pipeline *pipeline,
			     struct audio_pipeline_event *event, k_timeout_t timeout)
{
	if (!pipeline || !event ||
	    audio_pipeline_state_get(pipeline) == AUDIO_PIPELINE_STATE_UNINIT) {
		return -EINVAL;
	}

	return k_msgq_get(&pipeline->event_msgq, event, timeout);
}
