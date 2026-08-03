/*
 * The one place the AK4619 loopback's audio format and stimulus are written
 * down.
 *
 * THE POINT OF THIS FILE
 * ----------------------
 * Three devices have to agree about the same wire: i2s2 (transmit, and the
 * clock controller), i2s3 (receive) and the codec. The disagreements are quiet -
 * a bit-depth mismatch shifts the data, a slot mismatch swaps or drops
 * channels, and neither i2s_configure() nor audio_codec_configure() can see
 * the other side to complain. So none of the three gets its own numbers: they
 * all come from here, through ak4619_loopback_codec_cfg() and
 * ak4619_loopback_i2s_cfg(). Issue #46.
 *
 * Issue #47 added the second half: what is played, where the captured sample
 * sits, and therefore what level the measurement should find. Those follow from
 * the same numbers and are derived here rather than written out again beside
 * the code that checks them.
 *
 * WHY THESE NUMBERS
 * -----------------
 * 48 kHz, two channels, I2S frame format, MCLK 256 fs. The MCLK ratio is the
 * one that is not free: the AK4619 has no PLL and FS[2:0] only tells it what it
 * is being fed, and 256 fs is its FS[2:0] = 000 row, legal for 8 kHz..48 kHz
 * (datasheet p.31, Table 1). At 48 kHz that is MCLK 12.288 MHz.
 *
 * Zephyr's STM32 I2S driver delivers 256 fs at *either* slot length, because it
 * compensates for the peripheral's own master-clock divider: `bit_clk_freq *=
 * channel_length == 16U ? 4U * 2U : 4U` (drivers/i2s/i2s_stm32.c, read at
 * v4.4.1, the revision pinned in west.yml). A 16-bit channel length gives a
 * 32 fs bit clock times 8; a 32-bit one gives 64 fs times 4. Both land on
 * 256 fs, so the slot length is a free choice as far as the codec's clock
 * register is concerned - it only moves BICK, and 32 fs and 64 fs are both
 * inside the 32..256 fs the same table allows.
 *
 * (An earlier revision of this file and of docs/hardware §2 said 16-bit words
 * would give 128 fs. That read only the 32-bit branch of the expression above.
 * Corrected by #47; nothing else depended on it, because 32-bit slots are still
 * the default - for the reason below rather than for a clock ratio.)
 *
 * Slot length defaults to 32 bits, which is the part's own default and the only
 * one that carries the ADC's full 24-bit word. CONFIG_AK4619_LOOPBACK_SLOT_16
 * selects 16-bit slots instead; the reason that escape hatch exists is in
 * Kconfig.loopback and in the sample's README, and it is about the host's DMA,
 * not about the codec.
 *
 * Clock roles come from #43 §2 and are not negotiable in either direction:
 * the AK4619's MCLK, BICK and LRCK are input pins, so the STM32 sources all
 * three, i2s2 drives them and i2s3 and the codec receive them.
 *
 * WHERE THE CAPTURED SAMPLE SITS
 * ------------------------------
 * The ADC's longest word is 24 bits - DODL has no 32-bit setting (datasheet
 * p.33, Table 7) - so in a 32-bit slot the part puts its word at the start of
 * the slot and pads with zeros (p.34, Figure 12) and the host reads it
 * MSB-justified with the low 8 bits clear. In a 16-bit slot the word fills the
 * slot exactly.
 *
 * Either way the ADC word ends up at the *top* of the canonical 32-bit
 * container, which is what ak4619_loopback_capture_to_container() states once
 * and everything measuring an amplitude goes through. Getting this wrong is not
 * a rounding error: reading the 24-bit word as if it were right-justified
 * understates the level by 48 dB, which turns a good loop into a failed one.
 *
 * WHERE THE LEVELS LIVE
 * ---------------------
 * The codec's three gain stages are CONFIG_AK4619_DAC_VOLUME_HALF_DB,
 * CONFIG_AK4619_MIC_GAIN_DB and CONFIG_AK4619_ADC_VOLUME_HALF_DB, because they
 * are properties of the codec and the loop cable rather than of the wire
 * format, and the driver programs them itself. The chosen set is recorded in
 * docs/hardware/akd4619-evaluation-board.md §4.5.
 *
 * What is here is what the *application* adds on top - the tone's own amplitude
 * - and the sum of all four, which is the level the capture should come back
 * at. Deriving the expectation from the same symbols the driver programs is the
 * point: change a gain in Kconfig and the threshold moves with it instead of
 * silently becoming wrong.
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
#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Sample rate on every one of the three devices, in Hz. */
#define AK4619_LOOPBACK_RATE_HZ 48000U

/** Word and slot length on the wire, in bits (Kconfig.loopback). */
#ifdef CONFIG_AK4619_LOOPBACK_SLOT_16
#define AK4619_LOOPBACK_WORD_BITS 16U
#else
#define AK4619_LOOPBACK_WORD_BITS 32U
#endif

/** Channels per frame: Lch and Rch. */
#define AK4619_LOOPBACK_CHANNELS 2U
/** MCLK as a multiple of fs. FS[2:0] = 000 (datasheet p.31, Table 1). */
#define AK4619_LOOPBACK_MCLK_RATIO 256U
/** MCLK the STM32 emits and the codec is told to expect, in Hz. */
#define AK4619_LOOPBACK_MCLK_HZ (AK4619_LOOPBACK_RATE_HZ * AK4619_LOOPBACK_MCLK_RATIO)
/** BICK as a multiple of fs: channels x slot length. 64 fs, or 32 fs at 16 bit. */
#define AK4619_LOOPBACK_BICK_RATIO (AK4619_LOOPBACK_CHANNELS * AK4619_LOOPBACK_WORD_BITS)

/**
 * Valid bits in a captured sample: the ADC cannot emit more than 24 (p.33), and
 * it cannot emit more than the slot holds either.
 */
#define AK4619_LOOPBACK_CAPTURE_BITS                                                               \
	(AK4619_LOOPBACK_WORD_BITS < 24U ? AK4619_LOOPBACK_WORD_BITS : 24U)
/** Left shift the slot padding puts a captured sample under, inside its slot. */
#define AK4619_LOOPBACK_CAPTURE_SHIFT (AK4619_LOOPBACK_WORD_BITS - AK4619_LOOPBACK_CAPTURE_BITS)

/** Bits in the canonical pipeline container (spec §5.3). */
#define AK4619_LOOPBACK_CONTAINER_BITS 32U

/** Bytes one stereo frame occupies in a transfer block. */
#define AK4619_LOOPBACK_FRAME_BYTES                                                                \
	(AK4619_LOOPBACK_CHANNELS * (AK4619_LOOPBACK_WORD_BITS / 8U))

/**
 * @brief Place an ADC output word where it lands in the canonical container.
 *
 * The one statement of where a captured sample sits, and the only arithmetic in
 * this application that a 48 dB error can hide in.
 *
 * Two shifts compose into it and they are deliberately not written separately
 * at any call site:
 *
 *  1. The part puts its @ref AK4619_LOOPBACK_CAPTURE_BITS word at the *start*
 *     of the slot and pads the rest with zeros, so the slot value is
 *     @c word << AK4619_LOOPBACK_CAPTURE_SHIFT (datasheet p.34, Figure 12).
 *  2. The I2S wire seam carries a slot into the container MSB-aligned, so a
 *     32-bit slot passes through unchanged and a 16-bit one is shifted up by 16
 *     (spec §5.3).
 *
 * Their sum is the same in both cases - the ADC word ends at the top of the
 * container - which is why this is one shift of
 * @c AK4619_LOOPBACK_CONTAINER_BITS - @c AK4619_LOOPBACK_CAPTURE_BITS rather
 * than a case analysis.
 *
 * @param adc_word Signed ADC output word, @ref AK4619_LOOPBACK_CAPTURE_BITS
 *                 wide.
 * @return The container sample the pipeline sees.
 */
static inline int32_t ak4619_loopback_capture_to_container(int32_t adc_word)
{
	/* Shifted through the unsigned domain: left-shifting a negative signed
	 * value is not defined by the C standard, and the two's complement
	 * result is what is wanted.
	 */
	return (int32_t)((uint32_t)adc_word
			 << (AK4619_LOOPBACK_CONTAINER_BITS - AK4619_LOOPBACK_CAPTURE_BITS));
}

/*
 * ---------------------------------------------------------------------------
 * The stimulus, and the level it should come back at
 * ---------------------------------------------------------------------------
 */

/**
 * Peak amplitude of the played tone, as a Q15 fraction of full scale, and the
 * same figure in tenths of a decibel.
 *
 * -3.0 dBFS, which is the ceiling §4.4 of the wiring document sets: the DAC's
 * full-scale output and the ADC's full-scale input are the same number and
 * carry a +-10% spread each, so a 0 dBFS tone would land exactly at the ADC's
 * clipping point with the tolerances free to push it over.
 *
 * The two constants describe one thing and are checked against each other on
 * the host - 32768 * 10^(-3.0/20) = 23198, and 23170 is 1/sqrt(2) of full
 * scale, which is -3.01 dBFS and rounds to the same tenth. The Q15 value is
 * what the tone generator takes; the decibel value is what the expectation
 * below is built from.
 */
#define AK4619_LOOPBACK_TONE_AMPLITUDE_Q15 23170
#define AK4619_LOOPBACK_TONE_DBFS_X10      (-30)

/** Frequency each channel carries, in Hz, in channel order. */
#define AK4619_LOOPBACK_TONE_LCH_HZ 1000U
#define AK4619_LOOPBACK_TONE_RCH_HZ 3000U

/**
 * Analyzer integration window, in samples per channel.
 *
 * 960 at 48 kHz is a 50 Hz bin, so both tones land on whole bins - 1 kHz on bin
 * 20 and 3 kHz on bin 60 - and the measurement is exactly offset invariant
 * rather than nearly so. One window is 20 ms.
 */
#define AK4619_LOOPBACK_WINDOW_SAMPLES 960U

/**
 * The gain the loop applies to the played tone, in tenths of a decibel.
 *
 * Read off the same Kconfig symbols the driver programs into the part, so this
 * cannot drift away from what was actually written to registers 0x04, 0x06/0x07
 * and 0x0E/0x0F. The analog path between the DAC pin and the ADC pin is unity
 * by design (wiring document §4.4: both full scales are 0.858 x AVDD, and the
 * 220 ohm series resistors cost under 0.1 dB into 25 kohm).
 *
 * The digital volumes are in half-decibels, hence x5 to reach tenths; the MIC
 * amplifier's gain is in whole decibels, hence x10.
 */
#define AK4619_LOOPBACK_LOOP_GAIN_DBFS_X10                                                         \
	((CONFIG_AK4619_DAC_VOLUME_HALF_DB * 5) + (CONFIG_AK4619_ADC_VOLUME_HALF_DB * 5) +          \
	 (CONFIG_AK4619_MIC_GAIN_DB * 10))

/**
 * A sine's RMS relative to its peak, in tenths of a decibel: 1/sqrt(2).
 *
 * The analyzer reports RMS and everything above is stated as a peak, so this is
 * the one conversion between them.
 */
#define AK4619_LOOPBACK_SINE_RMS_DBFS_X10 (-30)

/**
 * The level the analyzer should measure on each channel, in tenths of a
 * decibel below full scale, RMS.
 *
 * With the defaults of §4.5 - tone -3.0 dBFS peak, DAC -6.0 dB, MIC amp 0 dB,
 * ADC digital 0 dB - this is -12.0 dBFS RMS, i.e. -9.0 dBFS peak.
 */
#define AK4619_LOOPBACK_EXPECTED_RMS_DBFS_X10                                                      \
	(AK4619_LOOPBACK_TONE_DBFS_X10 + AK4619_LOOPBACK_LOOP_GAIN_DBFS_X10 +                       \
	 AK4619_LOOPBACK_SINE_RMS_DBFS_X10)

/**
 * How far the measured level may sit from the expectation before the run fails,
 * in tenths of a decibel.
 *
 * 4.0 dB, and it is a budget rather than a round number:
 *
 *   1.7 dB  the two full scales, worst pairing. Both the DAC output and the
 *           single-ended ADC input are specified 2.55 / 2.83 / 3.11 Vpp
 *           min/typ/max (datasheet p.11 note *14, p.12 note *15), so a DAC at
 *           its maximum into an ADC at its minimum is 1.7 dB the wrong way.
 *   0.5 dB  the gain stages' own accuracy - two digital volume ladders and the
 *           MIC amplifier's 3 dB steps.
 *   0.1 dB  the 220 ohm series resistors into 25 kohm (wiring document §4.4).
 *   0.3 dB  the cable, its contacts, and the analyzer's window quantisation.
 *   ------
 *   2.6 dB  and 4.0 leaves 1.4 dB of slack.
 *
 * The window it defines is -16.0 .. -8.0 dBFS RMS. That is nowhere near the two
 * failures it has to stay clear of: an unplugged loop reads its own noise floor,
 * tens of decibels below, and a path stuck at full scale reads -3.0.
 */
#define AK4619_LOOPBACK_LEVEL_TOLERANCE_X10 40

/*
 * ---------------------------------------------------------------------------
 * Configuration builders
 * ---------------------------------------------------------------------------
 */

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
 * The application itself no longer calls i2s_configure(): the pipeline's I2S
 * nodes do, from the bound pipeline format. This stays because it is what makes
 * the two descriptions comparable, and the sample's Ztest suite asserts that
 * the format the nodes will produce is the one the codec was programmed for.
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
