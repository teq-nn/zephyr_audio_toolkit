#ifndef ZEPHYR_AUDIO_WAV_PARSER_H_
#define ZEPHYR_AUDIO_WAV_PARSER_H_

#include <zephyr/types.h>

struct wav_parser_result {
	uint32_t sample_rate_hz;
	uint16_t channels;
	uint16_t bits_per_sample;
};

int wav_parser_read_header(const uint8_t *data, size_t len,
			   struct wav_parser_result *out);

#endif /* ZEPHYR_AUDIO_WAV_PARSER_H_ */
