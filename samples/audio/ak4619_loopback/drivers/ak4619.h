/*
 * AKM AK4619VN audio codec: register map and the sample-local driver's
 * out-of-band API.
 *
 * The audio behaviour of this driver is reached through Zephyr's
 * <zephyr/audio/codec.h> API, which is what DEVICE_DT_INST_DEFINE() publishes.
 * What is declared here is the part that has no home in that API: raw register
 * access, the register-interface reset, and the write/read/verify link check.
 * Issue #46 builds the audio interface, clocking and analog routing on top of
 * these, so they are the seam between the two tickets.
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
 * These are the only bit definitions this ticket needs: they are what the
 * power state table (p.38, Table 8) is expressed in. The format, clock and
 * routing registers are #46's, and their bit definitions belong with the code
 * that writes them.
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
 * any clock and does not power any block up: #46 does that, after it has
 * programmed format, clocking and routing.
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
 * standby; do not call it once #46 has programmed a capture gain.
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

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_AUDIO_TOOLKIT_SAMPLES_AK4619_H_ */
