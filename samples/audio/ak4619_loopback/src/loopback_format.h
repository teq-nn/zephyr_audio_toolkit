/*
 * The one place the AK4619 loopback's audio format is written down.
 *
 * THE POINT OF THIS FILE
 * ----------------------
 * Three devices have to agree about the same wire: i2s2 (transmit, and the
 * clock source), i2s3 (receive) and the codec. The disagreements are quiet -
 * a bit-depth mismatch shifts the data, a slot mismatch swaps or drops
 * channels, and neither i2s_configure() nor audio_codec_configure() can see
 * the other side to complain. So none of the three gets its own numbers: they
 * all come from here, through ak4619_loopback_codec_cfg() and
 * ak4619_loopback_i2s_cfg(). Issue #46.
 *
 * WHY THESE NUMBERS
 * -----------------
 * 48 kHz, 32-bit words, two channels, I2S frame format. The word size is the
 * one that is not free to choose: Zephyr's STM32 I2S driver derives MCLK from
 * the bit clock, `bit_clk_freq *= 4` for a 32-bit channel length on top of a
 * 64 fs bit clock (drivers/i2s/i2s_stm32.c), so 32-bit words give MCLK = 256 fs
 * and 16-bit words would give 128 fs. 256 fs is the AK4619's FS[2:0] = 000
 * row, legal for 8 kHz..48 kHz; 128 fs is legal only at 192 kHz (datasheet
 * p.31, Table 1). At 48 kHz that is MCLK 12.288 MHz and BICK 3.072 MHz.
 *
 * Clock roles come from #43 §2 and are not negotiable in either direction:
 * the AK4619's MCLK, BICK and LRCK are input pins, so the STM32 sources all
 * three, i2s2 drives them and i2s3 and the codec receive them.
 *
 * WHERE THE CAPTURED SAMPLE SITS
 * ------------------------------
 * The ADC's longest word is 24 bits - DODL has no 32-bit setting (datasheet
 * p.33, Table 7) - while the slot is 32 bits and the STM32 reads 32-bit words.
 * The part puts the word at the start of the slot and pads with zeros (p.34,
 * Figure 12), so a captured sample arrives MSB-justified: value = sample24 <<
 * AK4619_LOOPBACK_CAPTURE_SHIFT, with the low 8 bits clear. Anything measuring
 * the captured amplitude has to know that.
 *
 * WHERE THE LEVELS LIVE
 * ---------------------
 * Not here. The DAC digital volume and the ADC's analog and digital gains are
 * CONFIG_AK4619_DAC_VOLUME_HALF_DB, CONFIG_AK4619_MIC_GAIN_DB and
 * CONFIG_AK4619_ADC_VOLUME_HALF_DB, because they are properties of the codec
 * and the loop cable rather than of the wire format, and the driver programs
 * them itself. The chosen pair is recorded in
 * docs/hardware/akd4619-evaluation-board.md §4.5.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_AUDIO_TOOLKIT_SAMPLES_AK4619_LOOPBACK_FORMAT_H_
#define ZEPHYR_AUDIO_TOOLKIT_SAMPLES_AK4619_LOOPBACK_FORMAT_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/audio/codec.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Sample rate on every one of the three devices, in Hz. */
#define AK4619_LOOPBACK_RATE_HZ 48000U
/** Word and slot length on the wire, in bits. */
#define AK4619_LOOPBACK_WORD_BITS 32U
/** Channels per frame: Lch and Rch. */
#define AK4619_LOOPBACK_CHANNELS 2U
/** MCLK as a multiple of fs. FS[2:0] = 000 (datasheet p.31, Table 1). */
#define AK4619_LOOPBACK_MCLK_RATIO 256U
/** MCLK the STM32 emits and the codec is told to expect, in Hz. */
#define AK4619_LOOPBACK_MCLK_HZ (AK4619_LOOPBACK_RATE_HZ * AK4619_LOOPBACK_MCLK_RATIO)
/** BICK as a multiple of fs: channels x word length. */
#define AK4619_LOOPBACK_BICK_RATIO (AK4619_LOOPBACK_CHANNELS * AK4619_LOOPBACK_WORD_BITS)

/** Valid bits in a captured sample - the ADC cannot emit more (p.33). */
#define AK4619_LOOPBACK_CAPTURE_BITS 24U
/** Left shift the padding puts a captured sample under in its 32-bit slot. */
#define AK4619_LOOPBACK_CAPTURE_SHIFT (AK4619_LOOPBACK_WORD_BITS - AK4619_LOOPBACK_CAPTURE_BITS)

/** Bytes one stereo frame occupies in a transfer block. */
#define AK4619_LOOPBACK_FRAME_BYTES                                                                \
	(AK4619_LOOPBACK_CHANNELS * (AK4619_LOOPBACK_WORD_BITS / 8U))

/**
 * @brief Fill in the audio_codec_cfg the AK4619 is configured from.
 *
 * The codec is a clock target in both directions, so both I2S_OPT_*_TARGET
 * bits are set here and are not a caller's choice: the STM32 already drives
 * MCLK, BICK and LRCK, and the AK4619 could not drive them if it wanted to.
 *
 * @param cfg Destination, overwritten in full.
 * @param route Which converters to bring up. AUDIO_ROUTE_BYPASS is not a
 *              value this part has - see the driver.
 */
static inline void ak4619_loopback_codec_cfg(struct audio_codec_cfg *cfg, audio_route_t route)
{
	*cfg = (struct audio_codec_cfg){
		.mclk_freq = AK4619_LOOPBACK_MCLK_HZ,
		.dai_type = AUDIO_DAI_TYPE_I2S,
		.dai_route = route,
		.dai_cfg.i2s = {
			.word_size = AK4619_LOOPBACK_WORD_BITS,
			.channels = AK4619_LOOPBACK_CHANNELS,
			.format = I2S_FMT_DATA_FORMAT_I2S,
			.options = I2S_OPT_BIT_CLK_TARGET | I2S_OPT_FRAME_CLK_TARGET,
			.frame_clk_freq = AK4619_LOOPBACK_RATE_HZ,
			.mem_slab = NULL,
			.block_size = 0,
			.timeout = 0,
		},
	};
}

/**
 * @brief Fill in the i2s_config one of the two STM32 blocks is configured from.
 *
 * Same format fields as ak4619_loopback_codec_cfg() produces, by construction.
 * What differs is the clock role, which is why @p options is a parameter: i2s2
 * transmits and sources the clocks, so it passes 0; i2s3 receives them and
 * passes I2S_OPT_BIT_CLK_TARGET | I2S_OPT_FRAME_CLK_TARGET. Exactly one of the
 * two may drive BICK and LRCK - they are the same wires (#43 §3.2).
 *
 * @param cfg Destination, overwritten in full.
 * @param options Clock role and any other I2S_OPT_* bits for this block.
 * @param slab Memory slab the direction's blocks come from.
 * @param block_size Transfer block size in bytes.
 * @param timeout Block timeout in milliseconds.
 */
static inline void ak4619_loopback_i2s_cfg(struct i2s_config *cfg, i2s_opt_t options,
					   struct k_mem_slab *slab, size_t block_size,
					   int32_t timeout)
{
	*cfg = (struct i2s_config){
		.word_size = AK4619_LOOPBACK_WORD_BITS,
		.channels = AK4619_LOOPBACK_CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = options,
		.frame_clk_freq = AK4619_LOOPBACK_RATE_HZ,
		.mem_slab = slab,
		.block_size = block_size,
		.timeout = timeout,
	};
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_AUDIO_TOOLKIT_SAMPLES_AK4619_LOOPBACK_FORMAT_H_ */
