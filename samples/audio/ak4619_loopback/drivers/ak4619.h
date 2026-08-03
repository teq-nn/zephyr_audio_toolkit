/*
 * AKM AK4619VN audio codec: register map and the sample-local driver's
 * out-of-band API.
 *
 * The audio behaviour of this driver is reached through Zephyr's
 * <zephyr/audio/codec.h> API, which is what DEVICE_DT_INST_DEFINE() publishes.
 * What is declared here is the part that has no home in that API: raw register
 * access, the register-interface reset, the write/read/verify link check, the
 * standby exit that has to happen after the host's clocks start, the analog
 * MIC gain, and the check that no internal loopback is enabled.
 *
 * The register bit definitions below are the field tables of datasheet
 * sections 9.2 to 9.10, in register order.
 *
 * The header is private to samples/audio/ak4619_loopback/. Nothing under
 * include/zephyr/audio/ or subsys/audio/ may include it - see issue #42.
 *
 * Page citations are to the AK4619VN datasheet, 200900082-E-00 2021/06.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_AUDIO_TOOLKIT_SAMPLES_AK4619_H_
#define ZEPHYR_AUDIO_TOOLKIT_SAMPLES_AK4619_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Register map (datasheet p.60, Table "Register Map Table").
 *
 * The whole map is 0x00..0x14. Addresses 0x15..0x7F are prohibited, and the
 * part's internal address counter rolls over to 0x00 past 0x14 during a
 * multi-byte transfer (p.57), so a burst that walks off the end silently
 * overwrites the power management register. ak4619_reg_*() enforce the range.
 */
#define AK4619_REG_PWR_MGMT       0x00U /* Power Management */
#define AK4619_REG_AUDIO_IF_FMT1  0x01U /* Audio I/F Format */
#define AK4619_REG_AUDIO_IF_FMT2  0x02U /* Audio I/F Format, extended */
#define AK4619_REG_SYS_CLK        0x03U /* System Clock Setting */
#define AK4619_REG_MIC_AMP_GAIN1  0x04U /* MIC AMP Gain, ADC1 */
#define AK4619_REG_MIC_AMP_GAIN2  0x05U /* MIC AMP Gain, ADC2 */
#define AK4619_REG_ADC1_LCH_VOL   0x06U /* ADC1 Lch Digital Volume */
#define AK4619_REG_ADC1_RCH_VOL   0x07U /* ADC1 Rch Digital Volume */
#define AK4619_REG_ADC2_LCH_VOL   0x08U /* ADC2 Lch Digital Volume */
#define AK4619_REG_ADC2_RCH_VOL   0x09U /* ADC2 Rch Digital Volume */
#define AK4619_REG_ADC_FILTER     0x0AU /* ADC Digital Filter Setting */
#define AK4619_REG_ADC_INPUT_SEL  0x0BU /* ADC Analog Input Setting */
#define AK4619_REG_RESERVED_0C    0x0CU /* Reserved, all bits must be 0 */
#define AK4619_REG_ADC_MUTE_HPF   0x0DU /* ADC Mute & HPF Control */
#define AK4619_REG_DAC1_LCH_VOL   0x0EU /* DAC1 Lch Digital Volume */
#define AK4619_REG_DAC1_RCH_VOL   0x0FU /* DAC1 Rch Digital Volume */
#define AK4619_REG_DAC2_LCH_VOL   0x10U /* DAC2 Lch Digital Volume */
#define AK4619_REG_DAC2_RCH_VOL   0x11U /* DAC2 Rch Digital Volume */
#define AK4619_REG_DAC_INPUT_SEL  0x12U /* DAC Input Select Setting */
#define AK4619_REG_DAC_DEEMPHASIS 0x13U /* DAC De-Emphasis Setting */
#define AK4619_REG_DAC_MUTE_FLT   0x14U /* DAC Mute & Filter Setting */

/** Highest writable register address (datasheet p.60). */
#define AK4619_REG_LAST AK4619_REG_DAC_MUTE_FLT
/** Number of registers in the map, 0x00..0x14 inclusive. */
#define AK4619_REG_COUNT (AK4619_REG_LAST + 1U)

/*
 * Power Management, register 0x00 (datasheet p.61).
 *
 * These are what the power state table (p.38, Table 8) is expressed in. The
 * four power-management bits choose which converters run; RSTN is the one that
 * has to go last, after the host's clocks are stable (p.41).
 */
#define AK4619_PWR_MGMT_PMAD2 BIT(5) /* ADC2 power management */
#define AK4619_PWR_MGMT_PMAD1 BIT(4) /* ADC1 power management */
#define AK4619_PWR_MGMT_PMDA2 BIT(2) /* DAC2 power management */
#define AK4619_PWR_MGMT_PMDA1 BIT(1) /* DAC1 power management */
#define AK4619_PWR_MGMT_RSTN  BIT(0) /* 0: internal reset, 1: normal */

/**
 * Standby: every block powered down, internal reset asserted, only the
 * reference generator and the LDO running (datasheet p.38 Table 8, p.39).
 * This is both the part's reset default and the state ak4619_reset() leaves
 * it in.
 */
#define AK4619_PWR_MGMT_STANDBY 0x00U

/*
 * Audio I/F Format, register 0x01 (datasheet p.61; the field tables it points
 * at are Table 2 on p.32 and Tables 4 and 5 on p.33).
 */
#define AK4619_IF1_TDM      BIT(7)          /* 0: stereo/PCM, 1: TDM */
#define AK4619_IF1_DCF_MASK GENMASK(6, 4)   /* DCF[2:0], interface format */
#define AK4619_IF1_DSL_MASK GENMASK(3, 2)   /* DSL[1:0], slot length */
#define AK4619_IF1_BCKP     BIT(1)          /* 0: BICK falling edge, 1: rising */
#define AK4619_IF1_SDOPH    BIT(0)          /* 0: slow SDOUT drive, 1: fast */

/* DCF[2:0] values, TDM bit = 0 (datasheet p.32, Table 2). */
#define AK4619_DCF_STEREO_I2S 0x0U /* I2S compatible (default) */
#define AK4619_DCF_STEREO_MSB 0x5U /* MSB justified, a.k.a. left justified */
#define AK4619_DCF_PCM_SHORT  0x6U /* PCM short frame */
#define AK4619_DCF_PCM_LONG   0x7U /* PCM long frame */

/*
 * Audio I/F Format, register 0x02 (datasheet p.62; Tables 6 and 7 on p.33).
 */
#define AK4619_IF2_SLOT      BIT(4)        /* 0: LRCK edge basis, 1: slot length */
#define AK4619_IF2_DIDL_MASK GENMASK(3, 2) /* SDIN1/2 word length */
#define AK4619_IF2_DODL_MASK GENMASK(1, 0) /* SDOUT1/2 word length */

/*
 * Length encoding, shared by DSL[1:0] (slot), DIDL[1:0] (SDIN word) and
 * DODL[1:0] (SDOUT word) - Tables 4, 6 and 7 on p.33 are the same four rows.
 *
 * With one hole: DODL = 11 is "N/A". The ADC cannot emit a 32-bit word,
 * whatever the slot length is - see AK4619_SDOUT_WORD_BITS_MAX.
 */
#define AK4619_DL_24BIT 0x0U
#define AK4619_DL_20BIT 0x1U
#define AK4619_DL_16BIT 0x2U
#define AK4619_DL_32BIT 0x3U /* the DSL and DIDL default */

/**
 * Longest word the ADC can put on SDOUT, in bits (datasheet p.33, Table 7).
 *
 * A 32-bit slot still works - the word sits at the start of the slot and the
 * part sends zeros in the padding bits (p.34, Figure 12) - so a 32-bit host
 * receives the sample MSB-justified with its low 8 bits clear.
 */
#define AK4619_SDOUT_WORD_BITS_MAX 24U

/*
 * System Clock Setting, register 0x03 (datasheet p.62; Table 1 on p.31).
 *
 * The AK4619 has no PLL. FS[2:0] does not divide anything: it tells the part
 * which MCLK-to-fs ratio the host is feeding it, and a wrong value is a part
 * that mis-times its own filters against clocks that look fine on a scope.
 */
#define AK4619_SYS_CLK_FS_MASK GENMASK(2, 0)

#define AK4619_FS_256FS_8K_48K 0x0U /* MCLK 256 fs,  8 kHz..48 kHz (default) */
#define AK4619_FS_256FS_96K    0x1U /* MCLK 256 fs, fs = 96 kHz */
#define AK4619_FS_384FS_8K_48K 0x2U /* MCLK 384 fs,  8 kHz..48 kHz */
#define AK4619_FS_512FS_8K_48K 0x3U /* MCLK 512 fs,  8 kHz..48 kHz */
#define AK4619_FS_128FS_192K   0x4U /* MCLK 128 fs, fs = 192 kHz */

/*
 * MIC AMP Gain, registers 0x04 (ADC1) and 0x05 (ADC2) (datasheet p.63; the
 * gain table is Table 9 on p.42).
 *
 * Every analog input reaches its ADC through this amplifier (p.43, Figure 23),
 * so it is the first place a line-level signal can be clipped: -6 dB to +27 dB
 * in 3 dB steps, code = (dB + 6) / 3.
 */
#define AK4619_MIC_GAIN_L_MASK GENMASK(7, 4)
#define AK4619_MIC_GAIN_R_MASK GENMASK(3, 0)

#define AK4619_MIC_GAIN_MIN_DB  (-6)
#define AK4619_MIC_GAIN_MAX_DB  27
#define AK4619_MIC_GAIN_STEP_DB 3
#define AK4619_MIC_GAIN_CODE(db)                                                                   \
	((uint8_t)(((db) - AK4619_MIC_GAIN_MIN_DB) / AK4619_MIC_GAIN_STEP_DB))

/*
 * ADC and DAC digital volume, registers 0x06..0x09 and 0x0E..0x11.
 *
 * Both are 256-step ladders in 0.5 dB decrements from a code that means 0 dB
 * (datasheet p.45 Table 11 and p.46 Table 14 for the ADC, p.50 Table 19 and
 * p.51 Table 22 for the DAC), and 0xFF is mute at both ends. So the level is
 * carried in half-decibels everywhere in this driver and in the audio_codec
 * volume property, which is also what Zephyr's tlv320dac driver does for the
 * same reason.
 */
#define AK4619_ADC_VOL_0DB  0x30U
#define AK4619_DAC_VOL_0DB  0x18U
#define AK4619_VOL_MUTE     0xFFU
#define AK4619_VOL_MIN_CODE 0xFEU

/** ADC digital volume range, in half-decibels: +24.0 dB .. -103.0 dB. */
#define AK4619_ADC_VOL_MAX_HALF_DB 48
#define AK4619_ADC_VOL_MIN_HALF_DB (-206)
/** DAC digital volume range, in half-decibels: +12.0 dB .. -115.0 dB. */
#define AK4619_DAC_VOL_MAX_HALF_DB 24
#define AK4619_DAC_VOL_MIN_HALF_DB (-230)

/** Register code for an ADC digital volume expressed in half-decibels. */
#define AK4619_ADC_VOL_CODE(half_db) ((uint8_t)(AK4619_ADC_VOL_0DB - (half_db)))
/** Register code for a DAC digital volume expressed in half-decibels. */
#define AK4619_DAC_VOL_CODE(half_db) ((uint8_t)(AK4619_DAC_VOL_0DB - (half_db)))

/*
 * ADC Digital Filter Setting, register 0x0A (datasheet p.64).
 */
#define AK4619_ADC_FILTER_AD2VO BIT(6) /* ADC2 voice filter */
#define AK4619_ADC_FILTER_AD2SD BIT(5)
#define AK4619_ADC_FILTER_AD2SL BIT(4)
#define AK4619_ADC_FILTER_AD1VO BIT(2) /* ADC1 voice filter */
#define AK4619_ADC_FILTER_AD1SD BIT(1)
#define AK4619_ADC_FILTER_AD1SL BIT(0)

/*
 * ADC Analog Input Setting, register 0x0B (datasheet p.64; Table 10 on p.43).
 * Each field picks which pins reach one ADC channel's amplifier.
 */
#define AK4619_ADC_IN_AD1L_MASK GENMASK(7, 6)
#define AK4619_ADC_IN_AD1R_MASK GENMASK(5, 4)
#define AK4619_ADC_IN_AD2L_MASK GENMASK(3, 2)
#define AK4619_ADC_IN_AD2R_MASK GENMASK(1, 0)

#define AK4619_ADC_IN_DIFFERENTIAL  0x0U /* INxP / INxN (default) */
#define AK4619_ADC_IN_SINGLE_ENDED1 0x1U /* AIN1L/R, AIN4L/R */
#define AK4619_ADC_IN_SINGLE_ENDED2 0x2U /* AIN2L/R, AIN5L/R */
#define AK4619_ADC_IN_PSEUDO_DIFF   0x3U /* AIN3L/R + GND3L/R */

/*
 * ADC Mute & HPF Control, register 0x0D (datasheet p.65).
 *
 * The HPF bits are named for what turns them *off*: AD1HPFN = 0 leaves the
 * DC-offset-cancelling high pass filter enabled, which is the default.
 */
#define AK4619_ADC_MUTE_ATSPAD  BIT(7)
#define AK4619_ADC_MUTE_AD2MUTE BIT(6)
#define AK4619_ADC_MUTE_AD1MUTE BIT(5)
#define AK4619_ADC_MUTE_AD2HPFN BIT(2)
#define AK4619_ADC_MUTE_AD1HPFN BIT(1)

/*
 * DAC Input Select Setting, register 0x12 - THE INTERNAL LOOPBACK REGISTER.
 *
 * Each DAC sits behind a 4:1 multiplexer that can take its data either from an
 * SDIN pin or straight off an ADC's SDOUT, without the sample ever leaving the
 * package (datasheet p.49, Figure 29 and Tables 17 and 18). The second case is
 * a digital loopback inside the part, and issue #42 is explicit about why it
 * matters: with one of them enabled the loopback sample passes with no cable
 * plugged in and verifies nothing.
 *
 * The encoding makes the check cheap. In both fields the high bit is "source
 * is an SDOUT", so AK4619_DAC_SEL_LOOPBACK_MASK is every bit of register 0x12
 * that can select an internal path, and the part is provably loop-free exactly
 * when they read back clear.
 */
#define AK4619_DAC_SEL_DAC2_MASK GENMASK(3, 2)
#define AK4619_DAC_SEL_DAC1_MASK GENMASK(1, 0)

#define AK4619_DAC_SEL_SDIN1  0x0U /* external, from the SDIN1 pin */
#define AK4619_DAC_SEL_SDIN2  0x1U /* external, from the SDIN2 pin */
#define AK4619_DAC_SEL_SDOUT1 0x2U /* INTERNAL: ADC1 -> DAC, no pins involved */
#define AK4619_DAC_SEL_SDOUT2 0x3U /* INTERNAL: ADC2 -> DAC, no pins involved */

/** Every bit of register 0x12 that selects an internal ADC-to-DAC path. */
#define AK4619_DAC_SEL_LOOPBACK_MASK (BIT(3) | BIT(1))

/** Both DAC multiplexers on SDIN1: no internal loopback anywhere in the part. */
#define AK4619_DAC_SEL_EXTERNAL_ONLY 0x00U

/*
 * DAC De-Emphasis Setting, register 0x13 (datasheet p.66). "01" is off, and
 * off is the default - but a de-emphasis filter left on would tilt the loop's
 * high end, so the driver writes it rather than assuming it.
 */
#define AK4619_DEM2_MASK GENMASK(3, 2)
#define AK4619_DEM1_MASK GENMASK(1, 0)
#define AK4619_DEM_OFF   0x1U

/*
 * DAC Mute & Filter Setting, register 0x14 (datasheet p.66).
 */
#define AK4619_DAC_MUTE_ATSPDA  BIT(7)
#define AK4619_DAC_MUTE_DA2MUTE BIT(5)
#define AK4619_DAC_MUTE_DA1MUTE BIT(4)
#define AK4619_DAC_FILTER_DA2SD BIT(3)
#define AK4619_DAC_FILTER_DA2SL BIT(2)
#define AK4619_DAC_FILTER_DA1SD BIT(1)
#define AK4619_DAC_FILTER_DA1SL BIT(0)

/**
 * Time the analog input coupling capacitors need to charge before an ADC may
 * be powered up, in milliseconds.
 *
 * "Wait about 100 ms to fully charge, then power up ADC1/2 with PMAD1/2 bit =
 * '0' -> '1'. If the wait time is less than 100 ms, a pop noise may appear
 * immediately after the ADC start-up." (datasheet p.42, and note (4) to the
 * power-up sequence on p.40). The clock starts at the PDN edge, which this
 * driver cannot see; it counts from its own init instead, which is at least
 * 10 ms later and therefore conservative.
 */
#define AK4619_ANALOG_CHARGE_MS 100

/**
 * Time the host must wait after the PDN pin rises before touching a control
 * register, in milliseconds.
 *
 * "control register write/read operations can be performed 10 ms after PDN is
 * deasserted" (datasheet p.31); the same 10 ms appears as note (3) to the
 * power-up sequence (p.40) as the maximum delay of the internal PDN signal.
 */
#define AK4619_PDN_TO_REG_ACCESS_MS 10

/**
 * @brief Write one control register.
 *
 * A two-byte I2C write: sub-address then data (datasheet p.57, Figure 36).
 *
 * @param dev AK4619 device.
 * @param reg Register address, 0x00..0x14.
 * @param val Value to write.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p reg is outside the writable range.
 * @retval -errno from the I2C driver otherwise.
 */
int ak4619_reg_write(const struct device *dev, uint8_t reg, uint8_t val);

/**
 * @brief Read one control register.
 *
 * A random-address read: sub-address write, repeated start, one data byte
 * (datasheet p.58, Figure 41).
 *
 * @param dev AK4619 device.
 * @param reg Register address, 0x00..0x14.
 * @param val Destination for the value read.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p reg is outside the readable range or @p val is NULL.
 * @retval -errno from the I2C driver otherwise.
 */
int ak4619_reg_read(const struct device *dev, uint8_t reg, uint8_t *val);

/**
 * @brief Read-modify-write the bits of @p mask in one register.
 *
 * @param dev AK4619 device.
 * @param reg Register address, 0x00..0x14.
 * @param mask Bits to change.
 * @param val New values for those bits; bits outside @p mask are ignored.
 *
 * @retval 0 on success, negative errno otherwise.
 */
int ak4619_reg_update(const struct device *dev, uint8_t reg, uint8_t mask, uint8_t val);

/**
 * @brief Write consecutive registers in one I2C transaction.
 *
 * The part auto-increments its internal address counter after every data byte
 * (datasheet p.57), so @p count bytes starting at @p start_reg land in
 * @p start_reg .. @p start_reg + @p count - 1.
 *
 * @param dev AK4619 device.
 * @param start_reg First register address.
 * @param vals Values to write, @p count of them.
 * @param count Number of registers; the run must end at or before 0x14.
 *
 * @retval 0 on success.
 * @retval -EINVAL if the run leaves the writable range, or @p vals is NULL.
 * @retval -errno from the I2C driver otherwise.
 */
int ak4619_reg_write_burst(const struct device *dev, uint8_t start_reg, const uint8_t *vals,
			   size_t count);

/**
 * @brief Reset the part to its power-on register defaults over the bus.
 *
 * The hardware reset - a "L" -> "H" edge on PDN - is not available on the
 * AKD4619-A evaluation board, where PDN is driven by a push-pull buffer off
 * the SW500 toggle (see docs/hardware/akd4619-evaluation-board.md §1.5). So
 * the driver reproduces it through the register interface, which is also what
 * AKM's own control software offers as its "Write Default" button (evaluation
 * manual p.28, item (5)):
 *
 *   1. Register 0x00 <- 0x00. Standby: ADCs and DACs powered down and RSTN
 *      asserted, so every subsequent write lands while the digital blocks are
 *      held in reset. The datasheet requires exactly that ordering - "It is
 *      possible to change the register values during the reset state. Release
 *      RSTN bit to '1' after the system clock is stable" (p.41).
 *   2. Registers 0x01..0x14 <- their reset defaults (p.60), in one burst.
 *
 * Leaves the part in the standby state of Table 8 (p.38). It does not start
 * any clock and does not power any block up - ak4619_power_up() does that,
 * after audio_codec_configure() has programmed format, clocking and routing.
 * A reset therefore discards any configuration, and the driver stops
 * considering itself configured until audio_codec_configure() runs again.
 *
 * @param dev AK4619 device.
 *
 * @retval 0 on success, negative errno from the I2C driver otherwise.
 */
int ak4619_reset(const struct device *dev);

/**
 * @brief Prove the codec is answering, not just ACKing.
 *
 * An I2C write to a floating bus, or to some other device that happens to
 * ACK 0x10, looks exactly like success at the ACK level, and the AK4619 has no
 * device-ID register to fall back on. So this writes two bit-complementary
 * patterns to a known-writable register, reads each back and compares, then
 * restores the register's reset default.
 *
 * Runs once at init - its result is logged, and a failure fails init so the
 * device never reports ready - and can be re-run afterwards to report the link
 * state on demand.
 *
 * It is destructive to one register: ADC1 Lch digital volume is left at its
 * reset default, not at whatever it held before. Harmless while the part is in
 * standby; do not call it once audio_codec_configure() has programmed a
 * capture gain, or re-apply the properties afterwards.
 *
 * @param dev AK4619 device.
 *
 * @retval 0 the part read back both patterns.
 * @retval -EIO the part ACKed but did not latch, or the bus is stuck.
 * @retval -errno from the I2C driver if the transfer itself failed.
 */
int ak4619_link_check(const struct device *dev);

/**
 * @brief Whether the link check passed at init.
 *
 * @param dev AK4619 device.
 * @return true if init completed with a verified link.
 */
bool ak4619_is_linked(const struct device *dev);

/**
 * @brief Leave standby: power the converters the route asked for and release
 *        the internal reset.
 *
 * THE CLOCKS MUST ALREADY BE RUNNING. The datasheet gives the order without
 * room for interpretation - "After setting the control register, supply the
 * necessary system clock (MCLK, BICK, LRCK) and then release the standby
 * state" (p.39) - and on this board the clocks do not exist until the STM32's
 * i2s2 block starts, because #43 made the STM32 the clock source. So the
 * sequence is: audio_codec_configure(), start i2s2, start i2s3, then this.
 *
 * Two writes to register 0x00, in the order the datasheet's Table 8 (p.38)
 * and power-up sequence (p.39) describe: the PMADx/PMDAx bits for the
 * configured route first, then RSTN on top. Before the first one it sleeps out
 * whatever is left of AK4619_ANALOG_CHARGE_MS since init, so the input
 * coupling capacitors are charged and the ADC does not start with a pop.
 *
 * This is the out-of-band twin of the audio_codec API's start_output(), which
 * returns void and so cannot tell a caller why nothing came out of the DAC.
 * Prefer it.
 *
 * @param dev AK4619 device.
 *
 * @retval 0 on success.
 * @retval -EPERM if audio_codec_configure() has not run: the part would leave
 *         reset with an interface and a clock mode nobody chose.
 * @retval -errno from the I2C driver otherwise.
 */
int ak4619_power_up(const struct device *dev);

/**
 * @brief Return to standby: converters powered down, RSTN asserted.
 *
 * The state of Table 8's standby row (datasheet p.38), which is also where
 * ak4619_reset() leaves the part. Format, clocking and analog routing survive:
 * only register 0x00 is written, so ak4619_power_up() can be called again
 * without reconfiguring. Pop noise may occur on the RSTN edge (p.41, note 3).
 *
 * @param dev AK4619 device.
 *
 * @retval 0 on success, negative errno from the I2C driver otherwise.
 */
int ak4619_power_down(const struct device *dev);

/**
 * @brief Re-read register 0x12 and prove no internal loopback is enabled.
 *
 * audio_codec_configure() already writes the multiplexers to an external
 * source and checks the read-back, so this is for a caller that wants to say
 * so on a console, or to re-check after something else has had the bus.
 *
 * @param dev AK4619 device.
 *
 * @retval 0 both DAC multiplexers take their data from an SDIN pin.
 * @retval -EIO an internal ADC-to-DAC path is enabled, which would make a
 *         loopback test pass with the cable unplugged.
 * @retval -errno from the I2C driver if the read failed.
 */
int ak4619_check_no_internal_loopback(const struct device *dev);

/**
 * @brief Set the analog MIC Gain AMP gain for one ADC, both channels.
 *
 * The amplifier ahead of every ADC input (datasheet p.43, Figure 23), -6 dB to
 * +27 dB in 3 dB steps (p.42, Table 9). audio_codec_configure() programs
 * CONFIG_AK4619_MIC_GAIN_DB into it; this is the bench override, and the
 * reason it is out of band is that the audio_codec API's input volume is the
 * ADC's *digital* volume, a different stage with a different range.
 *
 * @param dev AK4619 device.
 * @param adc 1 for ADC1, 2 for ADC2.
 * @param gain_db Gain in dB: -6..+27, a multiple of 3.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p adc or @p gain_db is outside the part's range.
 * @retval -errno from the I2C driver otherwise.
 */
int ak4619_set_mic_gain(const struct device *dev, uint8_t adc, int gain_db);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_AUDIO_TOOLKIT_SAMPLES_AK4619_H_ */
