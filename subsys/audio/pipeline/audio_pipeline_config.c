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

	/* The format is not part of the configuration: it is bound separately
	 * with audio_pipeline_set_format(), which does its own validation, and
	 * audio_pipeline_start() refuses a pipeline that has none (spec §5.2).
	 */
	return true;
}
