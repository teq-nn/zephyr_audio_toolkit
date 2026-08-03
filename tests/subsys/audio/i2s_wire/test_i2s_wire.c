/*
 * Unit test for the container-to-wire seam shared by the I2S nodes.
 *
 * The seam is the part of an I2S link that is pure arithmetic, so it is also
 * the only part that can be checked without a peripheral. Everything here runs
 * on native_sim with no I2S device, no driver and no node built - which is why
 * the seam lives beside the WAV codec rather than inside the sink.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <zephyr/audio/audio_i2s_wire.h>

/* The narrow depth: the one the file nodes speak. Named WIRE_BITS because every
 * case below that is not about a particular depth uses it.
 */
#define WIRE_BITS 16U

/* The wide depth: a 32 bit slot, which is what a codec whose converter word is
 * narrower than its slot needs (issue #47).
 */
#define WIRE_BITS_WIDE 32U

/* A depth the container can hold but the link deliberately does not carry: a
 * three byte word.
 */
#define UNSUPPORTED_BITS 24U

/* Full scale in both directions plus the values a naive (int16_t) cast of the
 * low half gets wrong: INT32_MAX would become 0 and INT32_MIN would become -1.
 */
static const int32_t container_samples[] = {
	0,
	INT32_MAX,
	INT32_MIN,
	(int32_t)((uint32_t)1 << 16),
	(int32_t)0xffff0000,
	(int32_t)0x1234abcd,
};

static const uint16_t expected_words[] = {
	0x0000, 0x7fff, 0x8000, 0x0001, 0xffff, 0x1234,
};

BUILD_ASSERT(ARRAY_SIZE(container_samples) == ARRAY_SIZE(expected_words),
	     "every sample needs the word it must narrow to");

ZTEST_SUITE(audio_i2s_wire, NULL, NULL, NULL, NULL, NULL);

ZTEST(audio_i2s_wire, test_i2s_wire_describes_the_word_the_driver_needs)
{
	struct audio_i2s_wire_format fmt = {0};

	zassert_ok(audio_i2s_wire_format_get(WIRE_BITS, &fmt));
	zassert_equal(fmt.word_bits, WIRE_BITS, "word_bits is what i2s_config.word_size gets");
	zassert_equal(fmt.word_bytes, WIRE_BITS / 8U, "%u bit words occupy %u bytes", WIRE_BITS,
		      WIRE_BITS / 8U);
	/* Block sizing multiplies by AUDIO_I2S_WIRE_MAX_WORD_BYTES, so a word
	 * wider than that would silently outgrow every block already defined.
	 */
	zassert_true(fmt.word_bytes <= AUDIO_I2S_WIRE_MAX_WORD_BYTES, "the word is too wide");
}

ZTEST(audio_i2s_wire, test_i2s_wire_narrows_container_to_wire_words)
{
	uint8_t wire[ARRAY_SIZE(container_samples) * sizeof(uint16_t)];
	size_t i;

	memset(wire, 0xa5, sizeof(wire));

	zassert_ok(audio_i2s_wire_from_container(
		WIRE_BITS, container_samples, ARRAY_SIZE(container_samples), wire, sizeof(wire)));

	for (i = 0; i < ARRAY_SIZE(expected_words); i++) {
		zassert_equal(sys_get_le16(&wire[i * sizeof(uint16_t)]), expected_words[i],
			      "sample %zu narrowed to 0x%04x instead of 0x%04x", i,
			      sys_get_le16(&wire[i * sizeof(uint16_t)]), expected_words[i]);
	}
}

ZTEST(audio_i2s_wire, test_i2s_wire_widens_wire_words_to_container)
{
	uint8_t wire[ARRAY_SIZE(expected_words) * sizeof(uint16_t)];
	int32_t samples[ARRAY_SIZE(expected_words)];
	size_t i;

	for (i = 0; i < ARRAY_SIZE(expected_words); i++) {
		sys_put_le16(expected_words[i], &wire[i * sizeof(uint16_t)]);
	}

	zassert_ok(audio_i2s_wire_to_container(WIRE_BITS, wire, sizeof(wire), samples,
					       ARRAY_SIZE(samples)));

	for (i = 0; i < ARRAY_SIZE(samples); i++) {
		/* The container keeps the top half and clears the rest, which is
		 * what makes the two directions exact inverses of each other.
		 */
		zassert_equal(samples[i], (int32_t)((uint32_t)expected_words[i] << 16),
			      "word %zu widened to 0x%08x", i, (unsigned int)samples[i]);
	}
}

ZTEST(audio_i2s_wire, test_i2s_wire_round_trips_the_wire_representation)
{
	uint8_t wire[ARRAY_SIZE(container_samples) * sizeof(uint16_t)];
	int32_t back[ARRAY_SIZE(container_samples)];
	int32_t again[ARRAY_SIZE(container_samples)];
	uint8_t wire_again[sizeof(wire)];

	zassert_ok(audio_i2s_wire_from_container(
		WIRE_BITS, container_samples, ARRAY_SIZE(container_samples), wire, sizeof(wire)));
	zassert_ok(
		audio_i2s_wire_to_container(WIRE_BITS, wire, sizeof(wire), back, ARRAY_SIZE(back)));
	zassert_ok(audio_i2s_wire_from_container(WIRE_BITS, back, ARRAY_SIZE(back), wire_again,
						 sizeof(wire_again)));
	zassert_ok(audio_i2s_wire_to_container(WIRE_BITS, wire_again, sizeof(wire_again), again,
					       ARRAY_SIZE(again)));

	/* Narrowing is lossy once, never twice: the second trip has to be the
	 * identity, or a sink and a source on the same link would drift apart
	 * frame by frame.
	 */
	zassert_mem_equal(wire_again, wire, sizeof(wire), "the wire form is not stable");
	zassert_mem_equal(again, back, sizeof(back), "the container form is not stable");
}

ZTEST(audio_i2s_wire, test_i2s_wire_leaves_the_rest_of_the_block_untouched)
{
	uint8_t wire[8];

	memset(wire, 0xa5, sizeof(wire));

	zassert_ok(
		audio_i2s_wire_from_container(WIRE_BITS, container_samples, 2, wire, sizeof(wire)));

	/* The sink hands the driver a block that is longer than the samples it
	 * carries, so the tail must stay whatever it was rather than be zeroed
	 * at a cost the pacing budget never accounted for.
	 */
	zassert_equal(wire[4], 0xa5, "the tail of the block was rewritten");
	zassert_equal(wire[7], 0xa5, "the tail of the block was rewritten");
}

ZTEST(audio_i2s_wire, test_i2s_wire_describes_a_wide_word)
{
	struct audio_i2s_wire_format fmt = {0};

	zassert_ok(audio_i2s_wire_format_get(WIRE_BITS_WIDE, &fmt));
	zassert_equal(fmt.word_bits, WIRE_BITS_WIDE);
	zassert_equal(fmt.word_bytes, WIRE_BITS_WIDE / 8U);
	/* The block sizing constant has to cover the widest word or every block
	 * already defined would be short by half.
	 */
	zassert_equal(fmt.word_bytes, AUDIO_I2S_WIRE_MAX_WORD_BYTES,
		      "a 32 bit word is the widest the container can produce");
}

ZTEST(audio_i2s_wire, test_i2s_wire_carries_a_wide_word_unchanged)
{
	uint8_t wire[ARRAY_SIZE(container_samples) * sizeof(uint32_t)];
	int32_t back[ARRAY_SIZE(container_samples)];
	size_t i;

	memset(wire, 0xa5, sizeof(wire));

	zassert_ok(audio_i2s_wire_from_container(WIRE_BITS_WIDE, container_samples,
						 ARRAY_SIZE(container_samples), wire,
						 sizeof(wire)));

	/* Little endian, i.e. the memory order the CPU itself uses. A driver
	 * whose DMA is as wide as the word sees exactly the container sample.
	 */
	for (i = 0; i < ARRAY_SIZE(container_samples); i++) {
		zassert_equal((int32_t)sys_get_le32(&wire[i * sizeof(uint32_t)]),
			      container_samples[i], "sample %zu changed on the way to the wire", i);
	}

	zassert_ok(audio_i2s_wire_to_container(WIRE_BITS_WIDE, wire, sizeof(wire), back,
					       ARRAY_SIZE(back)));

	/* Nothing is discarded at this depth, so the round trip is bit
	 * identical for every value - including the two full scales, which a
	 * narrowing link can only carry approximately.
	 */
	zassert_mem_equal(back, container_samples, sizeof(container_samples),
			  "a 32 bit link lost information it had no place to lose it in");
}

ZTEST(audio_i2s_wire, test_i2s_wire_keeps_the_two_depths_apart)
{
	uint8_t wire[2 * sizeof(uint32_t)];
	int32_t samples[2] = {(int32_t)0x12345678, (int32_t)0xfedcba98};
	int32_t back[2] = {0};

	zassert_ok(audio_i2s_wire_from_container(WIRE_BITS_WIDE, samples, ARRAY_SIZE(samples), wire,
						 sizeof(wire)));

	/* Reading a 32 bit block back as 16 bit words must not quietly succeed
	 * with half the samples: the two depths describe different blocks, and
	 * a link that mixed them would drift a channel per word.
	 */
	zassert_ok(audio_i2s_wire_to_container(WIRE_BITS, wire, sizeof(wire), back,
					       ARRAY_SIZE(back)));
	zassert_not_equal(back[0], samples[0],
			  "a 16 bit read of a 32 bit block cannot reproduce the samples");

	/* And a block sized for narrow words cannot hold the same count of wide
	 * ones - that is refused rather than truncated.
	 */
	zassert_equal(audio_i2s_wire_from_container(WIRE_BITS_WIDE, samples, ARRAY_SIZE(samples),
						    wire, ARRAY_SIZE(samples) * sizeof(uint16_t)),
		      -EINVAL);
}

ZTEST(audio_i2s_wire, test_i2s_wire_refuses_an_unsupported_depth)
{
	struct audio_i2s_wire_format fmt = {0};
	uint8_t wire[8] = {0};
	int32_t samples[2] = {0};

	zassert_equal(audio_i2s_wire_format_get(UNSUPPORTED_BITS, &fmt), -ENOTSUP);
	zassert_equal(audio_i2s_wire_format_get(0, &fmt), -ENOTSUP);

	/* Every entry point gates on the same description, so a depth the
	 * format call refuses can never be converted through a back door.
	 */
	zassert_equal(audio_i2s_wire_from_container(UNSUPPORTED_BITS, samples, ARRAY_SIZE(samples),
						    wire, sizeof(wire)),
		      -ENOTSUP);
	zassert_equal(audio_i2s_wire_to_container(UNSUPPORTED_BITS, wire, sizeof(wire), samples,
						  ARRAY_SIZE(samples)),
		      -ENOTSUP);
}

ZTEST(audio_i2s_wire, test_i2s_wire_refuses_a_block_that_is_too_small)
{
	uint8_t wire[3] = {0};
	int32_t samples[2] = {0};

	/* Two 16 bit words need four bytes; three is a truncated frame, not a
	 * short write to be reported afterwards.
	 */
	zassert_equal(audio_i2s_wire_from_container(WIRE_BITS, samples, ARRAY_SIZE(samples), wire,
						    sizeof(wire)),
		      -EINVAL);
	zassert_equal(audio_i2s_wire_to_container(WIRE_BITS, wire, sizeof(wire), samples,
						  ARRAY_SIZE(samples)),
		      -EINVAL);

	/* A count of zero always fits, including into nothing at all. */
	zassert_ok(audio_i2s_wire_from_container(WIRE_BITS, samples, 0, wire, 0));
	zassert_ok(audio_i2s_wire_to_container(WIRE_BITS, wire, 0, samples, 0));
}

ZTEST(audio_i2s_wire, test_i2s_wire_refuses_null_arguments)
{
	uint8_t wire[4] = {0};
	int32_t samples[2] = {0};

	zassert_equal(audio_i2s_wire_format_get(WIRE_BITS, NULL), -EINVAL);
	zassert_equal(audio_i2s_wire_from_container(WIRE_BITS, NULL, ARRAY_SIZE(samples), wire,
						    sizeof(wire)),
		      -EINVAL);
	zassert_equal(audio_i2s_wire_from_container(WIRE_BITS, samples, ARRAY_SIZE(samples), NULL,
						    sizeof(wire)),
		      -EINVAL);
	zassert_equal(audio_i2s_wire_to_container(WIRE_BITS, NULL, sizeof(wire), samples,
						  ARRAY_SIZE(samples)),
		      -EINVAL);
	zassert_equal(audio_i2s_wire_to_container(WIRE_BITS, wire, sizeof(wire), NULL,
						  ARRAY_SIZE(samples)),
		      -EINVAL);
}
