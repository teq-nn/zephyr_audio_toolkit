#include <zephyr/audio/audio_pipeline.h>

#include "audio_internal.h"

bool audio_pipeline_config_is_valid(const struct audio_pipeline_config *config)
{
	if (!config) {
		return false;
	}

	/* frame_samples is the TOTAL number of interleaved samples in a frame,
	 * across all channels (issue #23). The channel count is not part of the
	 * configuration - it is bound later with audio_pipeline_set_format() -
	 * so all that can be checked here is that the frame exists at all and
	 * fits the buffer the subsystem was built for. Whether it holds one
	 * interleaved sample set is a question only the bound format can answer,
	 * and set_format() answers it.
	 */
	if (!config->frame_samples || config->frame_samples > AUDIO_PIPELINE_MAX_FRAME_SAMPLES) {
		return false;
	}

	/* The format is not part of the configuration: it is bound separately
	 * with audio_pipeline_set_format(), which does its own validation, and
	 * audio_pipeline_start() refuses a pipeline that has none (spec §5.2).
	 */
	return true;
}
