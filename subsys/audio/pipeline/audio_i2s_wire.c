/*
 * I2S wire format codec (spec §10.5).
 *
 * The whole module is one mapping stated twice, once per direction, plus the
 * single description of which depths that mapping exists for. Keeping the three
 * together is the point: a depth added to audio_i2s_wire_format_get() that
 * neither conversion implements would be a link that configures itself and then
 * transmits noise.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/sys/byteorder.h>

#include <zephyr/audio/audio_i2s_wire.h>

/* v1 carries 16 bit words; see the header for why wider words wait for
 * hardware.
 */
#define I2S_WIRE_BITS_PER_WORD  16U
#define I2S_WIRE_BYTES_PER_WORD (I2S_WIRE_BITS_PER_WORD / 8U)

int audio_i2s_wire_format_get(uint8_t valid_bits_per_sample, struct audio_i2s_wire_format *out)
{
	if (!out) {
		return -EINVAL;
	}

	if (valid_bits_per_sample != I2S_WIRE_BITS_PER_WORD) {
		return -ENOTSUP;
	}

	out->word_bits = I2S_WIRE_BITS_PER_WORD;
	out->word_bytes = I2S_WIRE_BYTES_PER_WORD;

	return 0;
}

int audio_i2s_wire_from_container(uint8_t valid_bits_per_sample, const int32_t *samples,
				  size_t count, uint8_t *wire, size_t len)
{
	struct audio_i2s_wire_format fmt;
	size_t i;
	int ret;

	if (!samples || !wire) {
		return -EINVAL;
	}

	ret = audio_i2s_wire_format_get(valid_bits_per_sample, &fmt);
	if (ret < 0) {
		return ret;
	}

	/* Written as a division so the multiplication that would overflow a
	 * size_t on a long block never happens.
	 */
	if (count > len / fmt.word_bytes) {
		return -EINVAL;
	}

	for (i = 0; i < count; i++) {
		/* Shifted in the unsigned domain and stored as raw bytes, so the
		 * conversion has no implementation defined behaviour at all: the
		 * two's complement bit pattern of an arithmetic >> 16 is the top
		 * half of the container, whatever the host does with signed
		 * shifts.
		 */
		sys_put_le16((uint16_t)((uint32_t)samples[i] >> 16),
			     &wire[i * I2S_WIRE_BYTES_PER_WORD]);
	}

	return 0;
}

int audio_i2s_wire_to_container(uint8_t valid_bits_per_sample, const uint8_t *wire, size_t len,
				int32_t *samples, size_t count)
{
	struct audio_i2s_wire_format fmt;
	size_t i;
	int ret;

	if (!wire || !samples) {
		return -EINVAL;
	}

	ret = audio_i2s_wire_format_get(valid_bits_per_sample, &fmt);
	if (ret < 0) {
		return ret;
	}

	if (count > len / fmt.word_bytes) {
		return -EINVAL;
	}

	for (i = 0; i < count; i++) {
		int16_t word = (int16_t)sys_get_le16(&wire[i * I2S_WIRE_BYTES_PER_WORD]);

		/* Shifted as unsigned on purpose: left-shifting a negative
		 * signed value is not defined by the C standard, while the two's
		 * complement result below is exactly what spec §5.3 asks for
		 * (-1 -> 0xffff0000, -32768 -> INT32_MIN).
		 */
		samples[i] = (int32_t)((uint32_t)(int32_t)word << 16);
	}

	return 0;
}
