#include <zephyr/audio/audio_pipeline_events.h>

#include "audio_internal.h"

void audio_pipeline_publish_event(const struct audio_pipeline *pipeline,
				  enum audio_pipeline_event_type type, int err)
{
	if (!pipeline || !pipeline->config || !pipeline->config->event_cb) {
		return;
	}

	struct audio_pipeline_event evt = {
		.type = type,
		.err = err,
	};

	pipeline->config->event_cb(&evt, pipeline->config->event_user_data);
}
