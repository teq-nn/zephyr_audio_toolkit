/*
 * I2S wire format codec: the one place in the subsystem that knows how the
 * canonical S32_LE container maps onto the words an I2S peripheral moves, in
 * both directions.
 *
 * This is the container-to-wire seam of the I2S link, and it is deliberately
 * shared rather than owned by one node: the I2S output sink narrows containers
 * into the block a driver transmits, and the I2S input source widens a received
 * block back into containers. Neither spells the layout out itself, so the two
 * ends of one link cannot drift apart - the same reason the two file nodes
 * share audio_wav.h.
 *
 * Allocation free, driver free and endianness explicit, so the arithmetic is
 * testable on a host that has no I2S device at all.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_AUDIO_I2S_WIRE_H_
#define ZEPHYR_AUDIO_I2S_WIRE_H_

#include <stddef.h>

#include <zephyr/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bytes the widest wire word this module can ever emit occupies.
 *
 * The container is 32 bit, so no wire word can be wider than that however many
 * depths this module grows to support. It is spelled out because a transfer
 * block has to be sized at build time while the depth is only known once a
 * format is bound: a caller sizing storage multiplies by this rather than by
 * the depth it happens to use today, and adding a depth here then needs no
 * change at its definition sites.
 */
#define AUDIO_I2S_WIRE_MAX_WORD_BYTES 4U

/**
 * Wire words one bound format maps onto.
 *
 * Two answers that always have to agree - what to tell the driver, and how much
 * room a block needs - so they are produced together rather than derived twice.
 */
struct audio_i2s_wire_format {
	/** Word width on the wire in bits; this is @c i2s_config.word_size. */
	uint8_t word_bits;
	/** Bytes one wire word occupies inside a transfer block. */
	uint8_t word_bytes;
};

/**
 * Describe the wire words a container of @p valid_bits_per_sample maps onto.
 *
 * This is also the single gate on the depths the link supports: everything else
 * in this module refuses exactly what this call refuses, so a node validates a
 * bound format by asking here once in @c open() (spec §5.2 - nodes validate,
 * they do not adapt).
 *
 * Two depths are carried, 16 and 32 bit, and each exists because something at
 * the other end of a link needs it:
 *
 *  - **16 bit** is what the file nodes speak, so a file played out over I2S and
 *    a capture written back to a file need no conversion beyond this one.
 *  - **32 bit** is what a codec with a 32 bit slot needs. The AK4619 bring-up
 *    (samples/audio/ak4619_loopback, issue #47) runs a 32 bit slot so that the
 *    part's 24 bit ADC word arrives MSB justified inside it, and the slot
 *    length is what @c i2s_config.word_size sets. At 32 bit the mapping is the
 *    identity - the canonical container already *is* an MSB aligned 32 bit
 *    value (spec §5.3) - which is why this depth adds arithmetic that cannot
 *    round, clip or shift anything.
 *
 * 24 bit stays refused. It is the one depth whose word does not fill a whole
 * number of bytes the same way in every driver, and nothing in this tree needs
 * it: a part whose converter is 24 bit is carried in a 32 bit slot instead.
 *
 * WHAT A 32 BIT WORD ASSUMES ABOUT THE DRIVER, AND WHY IT IS WRITTEN DOWN HERE
 * ---------------------------------------------------------------------------
 * A wire word is stored little endian, i.e. in the memory order the CPU itself
 * would use, and the block is handed to the driver as bytes. That is right for
 * any driver whose DMA moves words as wide as the word, and for any FIFO that
 * is byte addressed.
 *
 * It is an *assumption* for one case worth naming: Zephyr's STM32 I2S driver
 * configures its DMA with a fixed 16 bit transfer width whatever
 * @c i2s_config.word_size says (@c drivers/i2s/i2s_stm32.c, read at v4.4.1), so
 * a 32 bit word reaches the peripheral as two halves and the peripheral decides
 * which half is the MSB. If that order is the opposite of this one, a link
 * still runs at the right rate and still carries energy - what comes back is
 * the two halves swapped, which reads as broadband noise rather than as
 * silence. That signature is deliberately distinguishable, and the AK4619
 * loopback sample reports it as its own diagnosis.
 *
 * @param valid_bits_per_sample Effective resolution carried in the container,
 *                              i.e. @c audio_stream_config.valid_bits_per_sample.
 * @param out                   Receives the wire description. Must not be NULL.
 *
 * @retval 0        @p out is populated.
 * @retval -EINVAL  @p out is NULL.
 * @retval -ENOTSUP The depth is not one this link can carry.
 */
int audio_i2s_wire_format_get(uint8_t valid_bits_per_sample, struct audio_i2s_wire_format *out);

/**
 * Narrow @p count canonical container samples into wire words.
 *
 * The samples stay in the order they arrive, so the interleaving the pipeline
 * uses is the interleaving that reaches the wire; the caller is responsible for
 * only handing over whole interleaved sample sets.
 *
 * For a 16 bit link the rule is the top half of the container, exactly as the
 * file writer narrows to 16 bit PCM (spec §5.3): truncation towards negative
 * infinity, no rounding bias and no clipping - a 32 bit value shifted down by
 * 16 always lands inside [-32768, 32767], so clamping cannot be needed, and the
 * result is the exact inverse of ::audio_i2s_wire_to_container.
 *
 * For a 32 bit link the container sample *is* the wire word, stored little
 * endian. Nothing is discarded, so the round trip is bit identical for every
 * value the container can hold rather than only for those that survive a
 * narrowing.
 *
 * @param valid_bits_per_sample Effective resolution of @p samples.
 * @param samples               Container samples to narrow. Must not be NULL.
 * @param count                 Number of samples in @p samples.
 * @param wire                  Block receiving the words. Must not be NULL and
 *                              needs no alignment: every word goes out byte
 *                              wise and little endian.
 * @param len                   Capacity of @p wire in bytes. Bytes past the
 *                              words written are left untouched.
 *
 * @retval 0        @p count words were written to @p wire.
 * @retval -EINVAL  A pointer is NULL, or @p len cannot hold @p count words.
 * @retval -ENOTSUP The depth is not one this link can carry.
 */
int audio_i2s_wire_from_container(uint8_t valid_bits_per_sample, const int32_t *samples,
				  size_t count, uint8_t *wire, size_t len);

/**
 * Widen wire words back into @p count canonical container samples.
 *
 * The inverse of ::audio_i2s_wire_from_container, and the direction the I2S
 * input source node uses. For a 16 bit link this is the widening spec §5.3
 * prescribes for every source, @c s32 = @c s16 << 16, so a wire word that made
 * a round trip through the pipeline arrives back bit identical. For a 32 bit
 * link the word is the container sample already and is read back little endian.
 *
 * @param valid_bits_per_sample Effective resolution to produce.
 * @param wire                  Block holding the received words. Must not be
 *                              NULL and needs no alignment.
 * @param len                   Valid bytes in @p wire.
 * @param samples               Receives the container samples. Must not be
 *                              NULL, and may not overlap @p wire.
 * @param count                 Number of samples to produce.
 *
 * @retval 0        @p count samples were written to @p samples.
 * @retval -EINVAL  A pointer is NULL, or @p len holds fewer than @p count words.
 * @retval -ENOTSUP The depth is not one this link can carry.
 */
int audio_i2s_wire_to_container(uint8_t valid_bits_per_sample, const uint8_t *wire, size_t len,
				int32_t *samples, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_AUDIO_I2S_WIRE_H_ */
