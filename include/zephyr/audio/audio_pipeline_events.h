#ifndef ZEPHYR_AUDIO_PIPELINE_EVENTS_H_
#define ZEPHYR_AUDIO_PIPELINE_EVENTS_H_

#include <zephyr/types.h>

enum audio_pipeline_event_type {
	AUDIO_PIPELINE_EVENT_EOF = 0,
	AUDIO_PIPELINE_EVENT_ERROR,
	AUDIO_PIPELINE_EVENT_RECONFIG,
};

struct audio_pipeline_event {
	enum audio_pipeline_event_type type;
	int err;
};

typedef void (*audio_pipeline_event_callback_t)(
	const struct audio_pipeline_event *event, void *user_data);

#endif /* ZEPHYR_AUDIO_PIPELINE_EVENTS_H_ */
