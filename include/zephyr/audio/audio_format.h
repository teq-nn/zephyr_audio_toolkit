#ifndef ZEPHYR_AUDIO_FORMAT_H_
#define ZEPHYR_AUDIO_FORMAT_H_

#include <zephyr/types.h>

enum audio_sample_format {
	AUDIO_SAMPLE_FORMAT_S32_LE,
};

struct audio_stream_config {
	uint32_t sample_rate_hz;
	uint8_t channels;
	uint8_t valid_bits_per_sample;
	enum audio_sample_format format;
};

#endif /* ZEPHYR_AUDIO_FORMAT_H_ */
