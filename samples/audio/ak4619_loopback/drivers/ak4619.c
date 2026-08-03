/*
 * AKM AK4619VN audio codec driver: I2C register access, register-interface
 * reset, and a link check that does not take an ACK for an answer.
 *
 * Issue #45. Scope is the skeleton: the part comes up on the bus, is reset
 * into the datasheet's standby state, and proves over the console that it is
 * really there. The audio surface of <zephyr/audio/codec.h> - configure(),
 * routing and the volume controls - is #46's, and is stubbed here so the
 * seam is visible rather than implied.
 *
 * WHERE THIS FILE LIVES
 * ---------------------
 * With the sample, not in subsys/. Issue #42 is explicit that nothing under
 * include/zephyr/audio/ or subsys/audio/ gains a codec dependency, and that no
 * AUDIO_PIPELINE_* Kconfig symbol mentions the codec. It is still written as a
 * real Zephyr driver - DEVICE_DT_INST_DEFINE() against the audio_codec API -
 * so promoting it into drivers/audio/ later is a move, not a rewrite.
 *
 * Page citations are to the AK4619VN datasheet, 200900082-E-00 2021/06, and to
 * the AKD4619-A evaluation manual, KM134006 2023/05. The board-side reasoning
 * they support is written up in docs/hardware/akd4619-evaluation-board.md.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT asahi_kasei_ak4619

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "ak4619.h"

LOG_MODULE_REGISTER(ak4619, CONFIG_AK4619_LOG_LEVEL);

/*
 * The device has to come up after the I2C controller it hangs off, which on
 * every in-tree controller means CONFIG_I2C_INIT_PRIORITY. Asserting it beats
 * documenting it: a board that lowers the codec's priority below its bus gets
 * a build error rather than an -ENODEV at boot.
 */
BUILD_ASSERT(CONFIG_AK4619_INIT_PRIORITY > CONFIG_I2C_INIT_PRIORITY,
	     "the AK4619 must initialise after its I2C controller");

/*
 * Reset defaults, register 0x00 through 0x14 (datasheet p.60).
 *
 * This table *is* the reset: with PDN unreachable on the evaluation board
 * (evaluation manual p.26, Figure 26; the pin is driven by the SN74LVC2G14 at
 * U502 and would fight a host GPIO), writing it back is the only way to
 * reproduce what a PDN "L" -> "H" edge does. Keep it in datasheet order so it
 * can be checked against the table by eye.
 */
static const uint8_t ak4619_reg_defaults[AK4619_REG_COUNT] = {
	[AK4619_REG_PWR_MGMT] = 0x00U,       [AK4619_REG_AUDIO_IF_FMT1] = 0x0CU,
	[AK4619_REG_AUDIO_IF_FMT2] = 0x0CU,  [AK4619_REG_SYS_CLK] = 0x00U,
	[AK4619_REG_MIC_AMP_GAIN1] = 0x22U,  [AK4619_REG_MIC_AMP_GAIN2] = 0x22U,
	[AK4619_REG_ADC1_LCH_VOL] = 0x30U,   [AK4619_REG_ADC1_RCH_VOL] = 0x30U,
	[AK4619_REG_ADC2_LCH_VOL] = 0x30U,   [AK4619_REG_ADC2_RCH_VOL] = 0x30U,
	[AK4619_REG_ADC_FILTER] = 0x00U,     [AK4619_REG_ADC_INPUT_SEL] = 0x00U,
	[AK4619_REG_RESERVED_0C] = 0x00U,    [AK4619_REG_ADC_MUTE_HPF] = 0x00U,
	[AK4619_REG_DAC1_LCH_VOL] = 0x18U,   [AK4619_REG_DAC1_RCH_VOL] = 0x18U,
	[AK4619_REG_DAC2_LCH_VOL] = 0x18U,   [AK4619_REG_DAC2_RCH_VOL] = 0x18U,
	[AK4619_REG_DAC_INPUT_SEL] = 0x04U,  [AK4619_REG_DAC_DEEMPHASIS] = 0x05U,
	[AK4619_REG_DAC_MUTE_FLT] = 0x0AU,
};

/*
 * The register the link check scribbles on, and the two patterns it uses.
 *
 * ADC1 Lch digital volume is eight fully read/write bits with no reserved
 * fields (datasheet p.63), and the part is in standby with RSTN asserted while
 * the check runs, so nothing is listening to it. 0xA5 and 0x5A are bitwise
 * complements: a bus stuck high reads 0xFF and a bus stuck low reads 0x00, so
 * neither pattern can be matched by accident, and a device that ACKs
 * everything and returns a constant fails on one of the two whatever the
 * constant is.
 */
#define AK4619_LINK_CHECK_REG      AK4619_REG_ADC1_LCH_VOL
#define AK4619_LINK_CHECK_PATTERN0 0xA5U
#define AK4619_LINK_CHECK_PATTERN1 0x5AU

struct ak4619_config {
	struct i2c_dt_spec i2c;
};

struct ak4619_data {
	/* Set once the init-time link check has passed. */
	bool linked;
};

/* Registers 0x15..0x7F are prohibited (datasheet p.60). */
static bool ak4619_reg_in_range(uint8_t reg)
{
	return reg <= AK4619_REG_LAST;
}

int ak4619_reg_write(const struct device *dev, uint8_t reg, uint8_t val)
{
	const struct ak4619_config *cfg = dev->config;
	int ret;

	if (!ak4619_reg_in_range(reg)) {
		LOG_ERR("register 0x%02x is outside the writable map 0x00..0x%02x", reg,
			AK4619_REG_LAST);
		return -EINVAL;
	}

	ret = i2c_reg_write_byte_dt(&cfg->i2c, reg, val);
	if (ret < 0) {
		LOG_ERR("write of 0x%02x to register 0x%02x failed (%d)", val, reg, ret);
		return ret;
	}

	LOG_DBG("reg 0x%02x <- 0x%02x", reg, val);

	return 0;
}

int ak4619_reg_read(const struct device *dev, uint8_t reg, uint8_t *val)
{
	const struct ak4619_config *cfg = dev->config;
	int ret;

	if (!ak4619_reg_in_range(reg) || val == NULL) {
		return -EINVAL;
	}

	ret = i2c_reg_read_byte_dt(&cfg->i2c, reg, val);
	if (ret < 0) {
		LOG_ERR("read of register 0x%02x failed (%d)", reg, ret);
		return ret;
	}

	LOG_DBG("reg 0x%02x -> 0x%02x", reg, *val);

	return 0;
}

int ak4619_reg_update(const struct device *dev, uint8_t reg, uint8_t mask, uint8_t val)
{
	uint8_t old;
	uint8_t updated;
	int ret;

	ret = ak4619_reg_read(dev, reg, &old);
	if (ret < 0) {
		return ret;
	}

	updated = (old & ~mask) | (val & mask);
	if (updated == old) {
		return 0;
	}

	return ak4619_reg_write(dev, reg, updated);
}

int ak4619_reg_write_burst(const struct device *dev, uint8_t start_reg, const uint8_t *vals,
			   size_t count)
{
	const struct ak4619_config *cfg = dev->config;
	uint8_t buf[1 + AK4619_REG_COUNT];
	int ret;

	if (vals == NULL || count == 0U) {
		return -EINVAL;
	}

	/*
	 * A run that walks past 0x14 does not fail on the wire: the internal
	 * address counter rolls over to 0x00 (datasheet p.57) and the tail of
	 * the burst quietly overwrites the power management register. Catch it
	 * here instead.
	 */
	if (!ak4619_reg_in_range(start_reg) ||
	    count > (size_t)(AK4619_REG_LAST - start_reg) + 1U) {
		LOG_ERR("burst of %zu from register 0x%02x leaves the map 0x00..0x%02x", count,
			start_reg, AK4619_REG_LAST);
		return -EINVAL;
	}

	buf[0] = start_reg;
	memcpy(&buf[1], vals, count);

	ret = i2c_write_dt(&cfg->i2c, buf, count + 1U);
	if (ret < 0) {
		LOG_ERR("burst write of %zu registers from 0x%02x failed (%d)", count, start_reg,
			ret);
		return ret;
	}

	LOG_DBG("regs 0x%02x..0x%02x <- %zu bytes", start_reg,
		(unsigned int)(start_reg + count - 1U), count);

	return 0;
}

int ak4619_reset(const struct device *dev)
{
	int ret;

	/*
	 * Standby first. Every register below is a format, clock or routing
	 * register, and the datasheet is explicit that those change during
	 * reset, not during normal operation: "The system clock should be
	 * changed during standby, reset state or power-down (PDN pin = 'L')
	 * mode. It is possible to change the register values during the reset
	 * state. Release RSTN bit to '1' after the system clock is stable
	 * during reset state." (p.41). Clearing PMADx/PMDAx/RSTN together is
	 * the standby row of Table 8 (p.38).
	 */
	ret = ak4619_reg_write(dev, AK4619_REG_PWR_MGMT, AK4619_PWR_MGMT_STANDBY);
	if (ret < 0) {
		return ret;
	}

	/*
	 * Then the rest of the map in one transaction, which is what the
	 * part's address auto-increment is for. Register 0x00 is deliberately
	 * not part of this burst: it has already been written, and doing it
	 * separately means a failure says which half of the reset failed.
	 */
	ret = ak4619_reg_write_burst(dev, AK4619_REG_AUDIO_IF_FMT1,
				     &ak4619_reg_defaults[AK4619_REG_AUDIO_IF_FMT1],
				     AK4619_REG_COUNT - 1U);
	if (ret < 0) {
		return ret;
	}

	LOG_DBG("registers 0x00..0x%02x restored to their reset defaults", AK4619_REG_LAST);

	return 0;
}

int ak4619_link_check(const struct device *dev)
{
	static const uint8_t patterns[] = {AK4619_LINK_CHECK_PATTERN0, AK4619_LINK_CHECK_PATTERN1};
	int ret;

	ARRAY_FOR_EACH(patterns, i) {
		uint8_t readback;

		ret = ak4619_reg_write(dev, AK4619_LINK_CHECK_REG, patterns[i]);
		if (ret < 0) {
			return ret;
		}

		ret = ak4619_reg_read(dev, AK4619_LINK_CHECK_REG, &readback);
		if (ret < 0) {
			return ret;
		}

		if (readback != patterns[i]) {
			LOG_ERR("register 0x%02x read back 0x%02x after writing 0x%02x",
				AK4619_LINK_CHECK_REG, readback, patterns[i]);
			LOG_ERR("the bus ACKed but nothing latched - the AK4619 is not the device "
				"answering at this address");
			return -EIO;
		}
	}

	/* Put the scratch register back where the reset left it. */
	return ak4619_reg_write(dev, AK4619_LINK_CHECK_REG,
				ak4619_reg_defaults[AK4619_LINK_CHECK_REG]);
}

bool ak4619_is_linked(const struct device *dev)
{
	const struct ak4619_data *data = dev->data;

	return data->linked;
}

/*
 * Wait for the part to start answering at all.
 *
 * Two things have to have happened before a register access can work, and the
 * driver can observe neither of them:
 *
 *   * the supplies have ramped and PDN has gone "H" - on the AKD4619-A that is
 *     a human throwing SW500, which the manual says to do "just after Power
 *     supplied" (evaluation manual p.27, item 3), so it may land either side
 *     of the MCU's own boot;
 *   * 10 ms have passed since that edge, which is when the internal PDN signal
 *     is guaranteed to have risen (datasheet p.31 and p.40, note (3)).
 *
 * So: sleep the mandatory 10 ms, then poll for an ACK for a bounded time and
 * give up with a message rather than blocking forever. A read is used for the
 * poll because it changes nothing if the part is halfway through coming up.
 */
static int ak4619_wait_for_ack(const struct device *dev)
{
	const struct ak4619_config *cfg = dev->config;
	int elapsed_ms = 0;
	uint8_t scratch;
	int ret;

	k_msleep(AK4619_PDN_TO_REG_ACCESS_MS);

	while (true) {
		ret = i2c_reg_read_byte_dt(&cfg->i2c, AK4619_REG_PWR_MGMT, &scratch);
		if (ret == 0) {
			return 0;
		}

		if (elapsed_ms >= CONFIG_AK4619_PROBE_TIMEOUT_MS) {
			break;
		}

		k_msleep(AK4619_PDN_TO_REG_ACCESS_MS);
		elapsed_ms += AK4619_PDN_TO_REG_ACCESS_MS;
	}

	LOG_ERR("no answer from an AK4619 at 0x%02x on %s after %d ms (%d)", cfg->i2c.addr,
		cfg->i2c.bus->name, CONFIG_AK4619_PROBE_TIMEOUT_MS, ret);
	LOG_ERR("check: board powered (+5 V at J703), SW500 driven L then H, SW502-1 = L for I2C, "
		"SW502-2 = L for address 0x10, PORT601 unplugged so the on-board PIC is off the "
		"bus, SCL/SDA on TP601/TP602 with a common ground");
	LOG_ERR("see section 1 of docs/hardware/akd4619-evaluation-board.md");

	return ret;
}

static int ak4619_init(const struct device *dev)
{
	const struct ak4619_config *cfg = dev->config;
	struct ak4619_data *data = dev->data;
	int ret;

	data->linked = false;

	if (!device_is_ready(cfg->i2c.bus)) {
		LOG_ERR("I2C bus %s is not ready", cfg->i2c.bus->name);
		return -ENODEV;
	}

	/*
	 * k_msleep() below the call is why this driver initialises at
	 * POST_KERNEL and not earlier: PRE_KERNEL_* runs before the scheduler
	 * and may not sleep.
	 */
	ret = ak4619_wait_for_ack(dev);
	if (ret < 0) {
		return ret;
	}

	ret = ak4619_reset(dev);
	if (ret < 0) {
		LOG_ERR("could not reset the codec into its default state (%d)", ret);
		return ret;
	}

	/*
	 * Reset first, then prove. The reset gets the part into a state where
	 * the scratch register is doing nothing, and a link check that fails
	 * leaves the map at its defaults rather than half-written.
	 */
	ret = ak4619_link_check(dev);
	if (ret < 0) {
		return ret;
	}

	data->linked = true;

	LOG_INF("AK4619 at 0x%02x on %s: reset to defaults, write/read/verify passed "
		"(0x%02x and 0x%02x read back from register 0x%02x)",
		cfg->i2c.addr, cfg->i2c.bus->name, AK4619_LINK_CHECK_PATTERN0,
		AK4619_LINK_CHECK_PATTERN1, AK4619_LINK_CHECK_REG);

	return 0;
}

/*
 * The audio_codec API surface, and the seam for #46.
 *
 * configure(), set_property() and apply_properties() are mandatory members of
 * the API, so they exist; they report -ENOSYS rather than pretending, because
 * the audio interface format, the clocking and the analog paths are #46's
 * ticket and a driver that silently accepted a configuration it does not
 * apply would be worse than one that refuses. start_output()/stop_output()
 * return void and so can only warn.
 */
static int ak4619_configure(const struct device *dev, struct audio_codec_cfg *cfg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cfg);

	LOG_WRN("configure() is not implemented yet - see issue #46");

	return -ENOSYS;
}

static void ak4619_start_output(const struct device *dev)
{
	ARG_UNUSED(dev);

	LOG_WRN("start_output() is not implemented yet - see issue #46");
}

static void ak4619_stop_output(const struct device *dev)
{
	ARG_UNUSED(dev);

	LOG_WRN("stop_output() is not implemented yet - see issue #46");
}

static int ak4619_set_property(const struct device *dev, audio_property_t property,
			       audio_channel_t channel, audio_property_value_t val)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(property);
	ARG_UNUSED(channel);
	ARG_UNUSED(val);

	LOG_WRN("set_property() is not implemented yet - see issue #46");

	return -ENOSYS;
}

static int ak4619_apply_properties(const struct device *dev)
{
	ARG_UNUSED(dev);

	LOG_WRN("apply_properties() is not implemented yet - see issue #46");

	return -ENOSYS;
}

/*
 * Zephyr v4.4.1 - the revision west.yml pins - still names this struct
 * `audio_codec_api` and in-tree codec drivers still define it by hand;
 * DEVICE_API(audio_codec, ...) needs the `audio_codec_driver_api` tag that
 * arrived after it. Switch to the macro when the pin moves.
 */
static const struct audio_codec_api ak4619_driver_api = {
	.configure = ak4619_configure,
	.start_output = ak4619_start_output,
	.stop_output = ak4619_stop_output,
	.set_property = ak4619_set_property,
	.apply_properties = ak4619_apply_properties,
};

#define AK4619_DEFINE(inst)                                                                        \
	static struct ak4619_data ak4619_data_##inst;                                              \
	static const struct ak4619_config ak4619_config_##inst = {                                 \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, ak4619_init, NULL, &ak4619_data_##inst,                        \
			      &ak4619_config_##inst, POST_KERNEL, CONFIG_AK4619_INIT_PRIORITY,     \
			      &ak4619_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AK4619_DEFINE)
