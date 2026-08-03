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

/* The two depths this link carries; see the header for why each one exists and
 * why 24 bit is not among them.
 */
#define I2S_WIRE_BITS_NARROW 16U
#define I2S_WIRE_BITS_WIDE   32U

/* How far a container sample is shifted to become a narrow wire word. The wide
 * word needs no shift at all - the container already is one (spec §5.3).
 */
#define I2S_WIRE_NARROW_SHIFT 16U

int audio_i2s_wire_format_get(uint8_t valid_bits_per_sample, struct audio_i2s_wire_format *out)
{
	if (!out) {
		return -EINVAL;
	}

	if (valid_bits_per_sample != I2S_WIRE_BITS_NARROW &&
	    valid_bits_per_sample != I2S_WIRE_BITS_WIDE) {
		return -ENOTSUP;
	}

	out->word_bits = valid_bits_per_sample;
	out->word_bytes = (uint8_t)(valid_bits_per_sample / 8U);

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
		if (fmt.word_bits == I2S_WIRE_BITS_NARROW) {
			sys_put_le16((uint16_t)((uint32_t)samples[i] >> I2S_WIRE_NARROW_SHIFT),
				     &wire[i * fmt.word_bytes]);
		} else {
			sys_put_le32((uint32_t)samples[i], &wire[i * fmt.word_bytes]);
		}
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
		if (fmt.word_bits == I2S_WIRE_BITS_NARROW) {
			int16_t word = (int16_t)sys_get_le16(&wire[i * fmt.word_bytes]);

			/* Shifted as unsigned on purpose: left-shifting a
			 * negative signed value is not defined by the C
			 * standard, while the two's complement result below is
			 * exactly what spec §5.3 asks for (-1 -> 0xffff0000,
			 * -32768 -> INT32_MIN).
			 */
			samples[i] = (int32_t)((uint32_t)(int32_t)word << I2S_WIRE_NARROW_SHIFT);
		} else {
			samples[i] = (int32_t)sys_get_le32(&wire[i * fmt.word_bytes]);
		}
	}

	return 0;
}
