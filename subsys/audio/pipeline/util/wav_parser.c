#include <errno.h>
#include <zephyr/audio/audio_pipeline.h>

#include "wav_parser.h"

int wav_parser_read_header(const uint8_t *data, size_t len,
			   struct wav_parser_result *out)
{
	if (!data || !out || len < 12) {
		return -EINVAL;
	}

	/* Placeholder implementation: the real parser should inspect the RIFF/WAVE headers. */
	out->sample_rate_hz = 48000U;
	out->channels = 2U;
	out->bits_per_sample = 16U;

	return -ENOSYS;
}
