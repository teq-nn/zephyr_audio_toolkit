#include <zephyr/audio/audio_pipeline.h>

#include "audio_internal.h"

bool audio_pipeline_config_is_valid(const struct audio_pipeline_config *config)
{
	if (!config) {
		return false;
	}

	if (!config->frame_samples || config->frame_samples > AUDIO_PIPELINE_MAX_FRAME_SAMPLES) {
		return false;
	}

	if (!config->stream.sample_rate_hz || !config->stream.channels) {
		return false;
	}

	return true;
}
