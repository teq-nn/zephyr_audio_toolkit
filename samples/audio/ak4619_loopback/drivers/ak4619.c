/*
 * AKM AK4619VN audio codec driver: I2C register access, register-interface
 * reset, and a link check that does not take an ACK for an answer.
 *
 * Issues #45 and #46. #45 brought the part up on the bus, reset it into the
 * datasheet's standby state and proved over the console that it was really
 * there. #46 added the second half, from ak4619_configure() down: the serial
 * audio interface, the clock mode, the ADC and DAC analog paths, the volume
 * and mute properties, and the power-up that leaves standby once the clocks
 * the STM32 sources are running.
 *
 * NO INTERNAL LOOPBACK, EVER
 * --------------------------
 * The part can route an ADC straight into a DAC through the DAC source
 * multiplexers of register 0x12 (datasheet p.49). Every configuration this
 * driver programs puts both multiplexers on an SDIN pin and then reads the
 * register back to prove it, because issue #42 is built on a loopback test
 * whose whole value is that it fails with the cable unplugged.
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

/*
 * The three levels audio_codec_configure() programs, checked here rather than
 * left to a runtime surprise. The MIC Gain AMP is a 3 dB ladder (datasheet
 * p.42, Table 9); a value between two rungs would silently round.
 */
BUILD_ASSERT((CONFIG_AK4619_MIC_GAIN_DB - AK4619_MIC_GAIN_MIN_DB) % AK4619_MIC_GAIN_STEP_DB == 0,
	     "CONFIG_AK4619_MIC_GAIN_DB must be a multiple of the part's 3 dB step");
BUILD_ASSERT(CONFIG_AK4619_DAC_VOLUME_HALF_DB <= AK4619_DAC_VOL_MAX_HALF_DB &&
		     CONFIG_AK4619_DAC_VOLUME_HALF_DB >= AK4619_DAC_VOL_MIN_HALF_DB,
	     "CONFIG_AK4619_DAC_VOLUME_HALF_DB is outside the DAC volume ladder");
BUILD_ASSERT(CONFIG_AK4619_ADC_VOLUME_HALF_DB <= AK4619_ADC_VOL_MAX_HALF_DB &&
		     CONFIG_AK4619_ADC_VOLUME_HALF_DB >= AK4619_ADC_VOL_MIN_HALF_DB,
	     "CONFIG_AK4619_ADC_VOLUME_HALF_DB is outside the ADC volume ladder");

struct ak4619_config {
	struct i2c_dt_spec i2c;
};

/*
 * Volume and mute, staged by set_property() and written by apply_properties().
 *
 * The audio_codec API is explicit that the two are separate steps - properties
 * are cached and then applied - so set_property() touches no register. That is
 * not only contract-following: it means a caller can move both channels and
 * the mute together without the part passing through a state nobody asked for.
 *
 * Index 0 is Lch and index 1 is Rch, for ADC1 and DAC1. ADC2 and DAC2 are not
 * in this board's signal path (#43 §4) and are powered down.
 */
struct ak4619_props {
	uint8_t dac_vol[2];
	uint8_t adc_vol[2];
	bool dac_mute;
	bool adc_mute;
};

struct ak4619_data {
	/* Set once the init-time link check has passed. */
	bool linked;
	/* Set once configure() has programmed format, clocking and routing. */
	bool configured;
	/*
	 * Uptime past which the analog input coupling capacitors are charged
	 * and an ADC may be powered up without a pop (datasheet p.42).
	 */
	int64_t analog_ready_uptime;
	/* PMADx/PMDAx for the configured route. RSTN is added after them. */
	uint8_t route_pwr;
	struct ak4619_props props;
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
	struct ak4619_data *data = dev->data;
	int ret;

	/*
	 * A reset puts the format, clock and routing registers back to values
	 * nobody chose, so whatever configure() programmed is gone. Saying so
	 * here is what stops ak4619_power_up() from releasing the reset onto a
	 * configuration that no longer exists.
	 */
	data->configured = false;

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

	/*
	 * The coupling capacitors started charging at the PDN edge, which is a
	 * human throwing SW500 and is not observable from here. Init is at
	 * least the mandatory 10 ms past it (p.40 note (3)), so counting the
	 * 100 ms from now rather than from the edge can only overshoot.
	 */
	data->analog_ready_uptime = k_uptime_get() + AK4619_ANALOG_CHARGE_MS;

	LOG_INF("AK4619 at 0x%02x on %s: reset to defaults, write/read/verify passed "
		"(0x%02x and 0x%02x read back from register 0x%02x)",
		cfg->i2c.addr, cfg->i2c.bus->name, AK4619_LINK_CHECK_PATTERN0,
		AK4619_LINK_CHECK_PATTERN1, AK4619_LINK_CHECK_REG);

	return 0;
}

/*
 * ---------------------------------------------------------------------------
 * Configuration: the audio interface, the clock mode, and the analog paths.
 * Issue #46.
 * ---------------------------------------------------------------------------
 *
 * Everything below runs with the part in the standby state of Table 8 (p.38)
 * and leaves it there. That is not tidiness, it is the datasheet's sequence:
 * "The system clock should be changed during standby, reset state or
 * power-down mode. It is possible to change the register values during the
 * reset state. Release RSTN bit to '1' after the system clock is stable"
 * (p.41), and "After setting the control register, supply the necessary system
 * clock (MCLK, BICK, LRCK) and then release the standby state" (p.39).
 *
 * On this board the clocks do not exist when configure() runs - the STM32 is
 * the clock source (#43 §2) and its i2s2 block has not started yet - so
 * releasing the reset here would release it against a dead MCLK. That is what
 * ak4619_power_up() is for, and why it is a separate call.
 */

/** DCF[2:0] and the SLOT bit for one audio interface format. */
struct ak4619_format {
	/*
	 * There has to be a flag: I2S_FMT_DATA_FORMAT_I2S is zero, so a zeroed
	 * row - which is what a gap in the table below is - is indistinguishable
	 * from the one format this driver most needs to support.
	 */
	bool supported;
	uint8_t dcf;
	bool slot_length_basis;
	i2s_fmt_t i2s_fmt;
};

/*
 * The formats this driver programs, keyed by audio_dai_type_t.
 *
 * Table 2 (p.32) lists twelve rows; these are the four that need neither TDM
 * nor a slot layout the Zephyr I2S API cannot express. TDM128/TDM256 are real
 * on this part but would need a channel count and a slot map that
 * struct i2s_config has nowhere to put, so they are refused rather than
 * guessed at.
 *
 * i2s_fmt is the matching I2S_FMT_DATA_FORMAT_* value. It is here so that
 * configure() can catch a caller whose audio_codec_cfg says one thing in
 * dai_type and another in the struct i2s_config it hands to i2s_configure():
 * that disagreement is precisely the quiet failure issue #46 is about, and it
 * is invisible on a scope.
 */
static const struct ak4619_format ak4619_formats[] = {
	[AUDIO_DAI_TYPE_I2S] = {true, AK4619_DCF_STEREO_I2S, false, I2S_FMT_DATA_FORMAT_I2S},
	[AUDIO_DAI_TYPE_LEFT_JUSTIFIED] = {true, AK4619_DCF_STEREO_MSB, false,
					   I2S_FMT_DATA_FORMAT_LEFT_JUSTIFIED},
	[AUDIO_DAI_TYPE_PCMA] = {true, AK4619_DCF_PCM_SHORT, true, I2S_FMT_DATA_FORMAT_PCM_SHORT},
	[AUDIO_DAI_TYPE_PCMB] = {true, AK4619_DCF_PCM_LONG, true, I2S_FMT_DATA_FORMAT_PCM_LONG},
};

/** DSL/DIDL/DODL share one encoding (datasheet p.33, Tables 4, 6 and 7). */
static int ak4619_word_length_code(uint8_t bits, uint8_t *code)
{
	switch (bits) {
	case 16:
		*code = AK4619_DL_16BIT;
		return 0;
	case 20:
		*code = AK4619_DL_20BIT;
		return 0;
	case 24:
		*code = AK4619_DL_24BIT;
		return 0;
	case 32:
		*code = AK4619_DL_32BIT;
		return 0;
	default:
		return -ENOTSUP;
	}
}

/**
 * FS[2:0] from the MCLK-to-fs ratio and the sample rate (datasheet p.31,
 * Table 1).
 *
 * The part derives nothing: this register only tells it what it is being fed.
 * A ratio the table does not list is a configuration the AK4619 cannot run at
 * all, which is why it comes back as -ENOTSUP with both numbers in the log
 * rather than as a nearest match.
 */
static int ak4619_sys_clk_code(uint32_t mclk_freq, uint32_t rate, uint8_t *code)
{
	uint32_t ratio;

	if (rate == 0U || mclk_freq == 0U) {
		return -EINVAL;
	}

	if ((mclk_freq % rate) != 0U) {
		LOG_ERR("MCLK %u Hz is not an integer multiple of fs %u Hz", mclk_freq, rate);
		return -ENOTSUP;
	}

	ratio = mclk_freq / rate;

	if (rate <= 48000U) {
		switch (ratio) {
		case 256U:
			*code = AK4619_FS_256FS_8K_48K;
			return 0;
		case 384U:
			*code = AK4619_FS_384FS_8K_48K;
			return 0;
		case 512U:
			*code = AK4619_FS_512FS_8K_48K;
			return 0;
		default:
			break;
		}
	} else if (rate == 96000U && ratio == 256U) {
		*code = AK4619_FS_256FS_96K;
		return 0;
	} else if (rate == 192000U && ratio == 128U) {
		*code = AK4619_FS_128FS_192K;
		return 0;
	}

	LOG_ERR("no FS[2:0] setting for fs %u Hz at MCLK %u fs (Table 1, datasheet p.31)", rate,
		ratio);

	return -ENOTSUP;
}

/**
 * Translate one audio_codec_cfg into registers 0x01..0x03.
 *
 * Every rejection here is a configuration the part would either refuse or,
 * worse, accept and mis-time. None of them is silently corrected.
 */
static int ak4619_build_interface(const struct audio_codec_cfg *cfg, uint8_t regs[3])
{
	const struct i2s_config *i2s = &cfg->dai_cfg.i2s;
	const struct ak4619_format *fmt;
	uint8_t slot_code;
	uint8_t in_code;
	uint8_t out_code;
	uint8_t fs_code;
	uint32_t bick_ratio;
	uint8_t if1;
	uint8_t if2;
	int ret;

	if ((size_t)cfg->dai_type >= ARRAY_SIZE(ak4619_formats) ||
	    !ak4619_formats[cfg->dai_type].supported) {
		LOG_ERR("dai_type %d is not one of the stereo or PCM rows of Table 2 (p.32); the "
			"AK4619's TDM128/TDM256 modes need a slot map struct i2s_config cannot "
			"carry",
			(int)cfg->dai_type);
		return -ENOTSUP;
	}
	fmt = &ak4619_formats[cfg->dai_type];

	/*
	 * dai_type and i2s.format describe the same wire. The codec is
	 * programmed from the first and the STM32 block from the second, so a
	 * disagreement between them is two devices with different ideas about
	 * where the MSB is - and neither driver would report it.
	 */
	if ((i2s->format & I2S_FMT_DATA_FORMAT_MASK) != fmt->i2s_fmt) {
		LOG_ERR("dai_type %d asks for one frame format and i2s.format 0x%02x for another; "
			"the codec and the I2S block would disagree silently",
			(int)cfg->dai_type, i2s->format);
		return -EINVAL;
	}

	if ((i2s->format & I2S_FMT_FRAME_CLK_INV) != 0U) {
		LOG_ERR("the AK4619 has no LRCK polarity control - there is no bit for "
			"I2S_FMT_FRAME_CLK_INV");
		return -ENOTSUP;
	}

	/*
	 * THE CLOCK ROLE. LRCK, BICK and MCLK are input pins on this part
	 * (datasheet p.7) - it has no PLL and no clock output, and #43 §2
	 * settled the topology accordingly: i2s2 drives all three. So the codec
	 * is a target in both directions, and a caller that has not said so is
	 * a caller about to put two drivers on the same wire. Note that
	 * I2S_OPT_*_CONTROLLER are zero bits, so this is the only way round the
	 * test can be written.
	 */
	if ((i2s->options & (I2S_OPT_BIT_CLK_TARGET | I2S_OPT_FRAME_CLK_TARGET)) !=
	    (I2S_OPT_BIT_CLK_TARGET | I2S_OPT_FRAME_CLK_TARGET)) {
		LOG_ERR("options 0x%02x makes the codec a clock controller; the AK4619 cannot "
			"drive MCLK, BICK or LRCK (datasheet p.7) and the STM32 already does",
			i2s->options);
		return -ENOTSUP;
	}

	if (i2s->channels != 2U) {
		LOG_ERR("%u channels: the stereo and PCM rows of Table 2 carry exactly two",
			i2s->channels);
		return -ENOTSUP;
	}

	ret = ak4619_word_length_code(i2s->word_size, &slot_code);
	if (ret < 0) {
		LOG_ERR("word size %u is not 16, 20, 24 or 32 bits (datasheet p.33)",
			i2s->word_size);
		return ret;
	}
	in_code = slot_code;

	/*
	 * BICK carries channels x word_size bits per frame, and Table 1 (p.31)
	 * lists the ratios the part will lock to. 20-bit stereo asks for 40 fs,
	 * which is not one of them - the failure would be a part clocking data
	 * it cannot frame.
	 */
	bick_ratio = (uint32_t)i2s->channels * i2s->word_size;
	if (bick_ratio != 32U && bick_ratio != 48U && bick_ratio != 64U && bick_ratio != 128U &&
	    bick_ratio != 256U) {
		LOG_ERR("%u channels x %u bits gives BICK = %u fs; the AK4619 accepts 32, 48, 64, "
			"128 or 256 fs (datasheet p.31)",
			i2s->channels, i2s->word_size, bick_ratio);
		return -ENOTSUP;
	}

	/*
	 * DODL has no 32-bit setting: the ADC's longest word is 24 bits
	 * (datasheet p.33, Table 7). A 32-bit slot is still legal - the part
	 * pads the remainder with zeros (p.34, Figure 12) - so the host reads
	 * the sample MSB-justified with its low 8 bits clear. Said out loud
	 * because issue #47 has to know where the sample sits before it can
	 * measure its amplitude.
	 */
	out_code = slot_code;
	if (i2s->word_size > AK4619_SDOUT_WORD_BITS_MAX) {
		(void)ak4619_word_length_code(AK4619_SDOUT_WORD_BITS_MAX, &out_code);
		LOG_INF("capture word length capped at %u bits (DODL has no 32-bit setting, "
			"datasheet p.33 Table 7): samples arrive MSB-justified in the %u-bit slot "
			"with the low %u bits zero",
			AK4619_SDOUT_WORD_BITS_MAX, i2s->word_size,
			i2s->word_size - AK4619_SDOUT_WORD_BITS_MAX);
	}

	ret = ak4619_sys_clk_code(cfg->mclk_freq, i2s->frame_clk_freq, &fs_code);
	if (ret < 0) {
		if (ret == -EINVAL) {
			LOG_ERR("mclk_freq and frame_clk_freq must both be stated: the AK4619 has "
				"no PLL and cannot infer either one");
		}
		return ret;
	}

	if1 = (uint8_t)(FIELD_PREP(AK4619_IF1_DCF_MASK, fmt->dcf) |
			FIELD_PREP(AK4619_IF1_DSL_MASK, slot_code));
	if ((i2s->format & I2S_FMT_BIT_CLK_INV) != 0U) {
		if1 |= AK4619_IF1_BCKP;
	}

	if2 = (uint8_t)(FIELD_PREP(AK4619_IF2_DIDL_MASK, in_code) |
			FIELD_PREP(AK4619_IF2_DODL_MASK, out_code));
	if (fmt->slot_length_basis) {
		if2 |= AK4619_IF2_SLOT;
	}

	regs[0] = if1;
	regs[1] = if2;
	regs[2] = (uint8_t)FIELD_PREP(AK4619_SYS_CLK_FS_MASK, fs_code);

	return 0;
}

/** PMADx/PMDAx for a route, without RSTN (datasheet p.38, Table 8). */
static int ak4619_route_power(audio_route_t route, uint8_t *pwr)
{
	switch (route) {
	case AUDIO_ROUTE_PLAYBACK:
		*pwr = AK4619_PWR_MGMT_PMDA1;
		return 0;
	case AUDIO_ROUTE_CAPTURE:
		*pwr = AK4619_PWR_MGMT_PMAD1;
		return 0;
	case AUDIO_ROUTE_PLAYBACK_CAPTURE:
		*pwr = AK4619_PWR_MGMT_PMDA1 | AK4619_PWR_MGMT_PMAD1;
		return 0;
	case AUDIO_ROUTE_BYPASS:
	default:
		/*
		 * There is no analog bypass in this part - the only path from
		 * an input to an output that does not pass through both
		 * converters is the DAC source multiplexer's SDOUT setting
		 * (p.49), which is the internal loopback issue #42 forbids. So
		 * "bypass" is refused rather than approximated.
		 */
		LOG_ERR("AUDIO_ROUTE_BYPASS has no implementation on the AK4619: the only "
			"input-to-output path that skips the converters is the internal "
			"ADC-to-DAC loopback, which this board's test must not enable");
		return -ENOTSUP;
	}
}

static void ak4619_reset_props(struct ak4619_props *props)
{
	props->dac_vol[0] = AK4619_DAC_VOL_CODE(CONFIG_AK4619_DAC_VOLUME_HALF_DB);
	props->dac_vol[1] = props->dac_vol[0];
	props->adc_vol[0] = AK4619_ADC_VOL_CODE(CONFIG_AK4619_ADC_VOLUME_HALF_DB);
	props->adc_vol[1] = props->adc_vol[0];
	props->dac_mute = false;
	props->adc_mute = false;
}

int ak4619_check_no_internal_loopback(const struct device *dev)
{
	uint8_t sel;
	int ret;

	ret = ak4619_reg_read(dev, AK4619_REG_DAC_INPUT_SEL, &sel);
	if (ret < 0) {
		return ret;
	}

	if ((sel & AK4619_DAC_SEL_LOOPBACK_MASK) != 0U) {
		LOG_ERR("register 0x%02x reads 0x%02x: a DAC source multiplexer is on an SDOUT, "
			"i.e. an ADC is feeding a DAC inside the part (datasheet p.49)",
			AK4619_REG_DAC_INPUT_SEL, sel);
		LOG_ERR("a loopback test would pass with the cable unplugged - see issue #42");
		return -EIO;
	}

	return 0;
}

static int ak4619_configure(const struct device *dev, struct audio_codec_cfg *cfg)
{
	struct ak4619_data *data = dev->data;
	uint8_t interface[3];
	uint8_t analog[AK4619_REG_LAST - AK4619_REG_MIC_AMP_GAIN1 + 1U];
	uint8_t route_pwr;
	int ret;

	if (cfg == NULL) {
		return -EINVAL;
	}

	data->configured = false;

	ret = ak4619_build_interface(cfg, interface);
	if (ret < 0) {
		return ret;
	}

	ret = ak4619_route_power(cfg->dai_route, &route_pwr);
	if (ret < 0) {
		return ret;
	}

	ak4619_reset_props(&data->props);

	/*
	 * Standby first, unconditionally. Registers 0x01..0x03 are the ones
	 * p.41 says to change under reset, and the caller may be reconfiguring
	 * a part that ak4619_power_up() has already taken out of it.
	 */
	ret = ak4619_reg_write(dev, AK4619_REG_PWR_MGMT, AK4619_PWR_MGMT_STANDBY);
	if (ret < 0) {
		return ret;
	}

	ret = ak4619_reg_write_burst(dev, AK4619_REG_AUDIO_IF_FMT1, interface,
				     ARRAY_SIZE(interface));
	if (ret < 0) {
		return ret;
	}

	/*
	 * The analog half, 0x04..0x14 in one burst. Written as a block rather
	 * than as read-modify-writes because every one of these registers has a
	 * value this configuration wants - including the ones whose wanted
	 * value happens to equal the reset default. Issue #46 asks for exactly
	 * that: state the mute, the de-emphasis and above all the DAC source
	 * multiplexers, do not inherit them.
	 */
	analog[AK4619_REG_MIC_AMP_GAIN1 - AK4619_REG_MIC_AMP_GAIN1] = (uint8_t)(
		FIELD_PREP(AK4619_MIC_GAIN_L_MASK, AK4619_MIC_GAIN_CODE(CONFIG_AK4619_MIC_GAIN_DB)) |
		FIELD_PREP(AK4619_MIC_GAIN_R_MASK, AK4619_MIC_GAIN_CODE(CONFIG_AK4619_MIC_GAIN_DB)));
	analog[AK4619_REG_MIC_AMP_GAIN2 - AK4619_REG_MIC_AMP_GAIN1] =
		analog[AK4619_REG_MIC_AMP_GAIN1 - AK4619_REG_MIC_AMP_GAIN1];

	analog[AK4619_REG_ADC1_LCH_VOL - AK4619_REG_MIC_AMP_GAIN1] = data->props.adc_vol[0];
	analog[AK4619_REG_ADC1_RCH_VOL - AK4619_REG_MIC_AMP_GAIN1] = data->props.adc_vol[1];
	analog[AK4619_REG_ADC2_LCH_VOL - AK4619_REG_MIC_AMP_GAIN1] = data->props.adc_vol[0];
	analog[AK4619_REG_ADC2_RCH_VOL - AK4619_REG_MIC_AMP_GAIN1] = data->props.adc_vol[1];

	/* Audio sharp roll-off on both ADCs; the voice filter is 16 kHz work. */
	analog[AK4619_REG_ADC_FILTER - AK4619_REG_MIC_AMP_GAIN1] = 0x00U;

	/*
	 * ADC1 on the single-ended AIN1L/AIN1R pins - the IN1P and IN2P pins
	 * that jacks J201 and J202 reach with JP202 at 2-3 and JP206 at 1-2
	 * (#43 §4.3, datasheet p.43 Table 10). ADC2's fields stay at the
	 * differential default: its jacks are not in the loop and it is
	 * powered down.
	 */
	analog[AK4619_REG_ADC_INPUT_SEL - AK4619_REG_MIC_AMP_GAIN1] =
		(uint8_t)(FIELD_PREP(AK4619_ADC_IN_AD1L_MASK, AK4619_ADC_IN_SINGLE_ENDED1) |
			  FIELD_PREP(AK4619_ADC_IN_AD1R_MASK, AK4619_ADC_IN_SINGLE_ENDED1) |
			  FIELD_PREP(AK4619_ADC_IN_AD2L_MASK, AK4619_ADC_IN_DIFFERENTIAL) |
			  FIELD_PREP(AK4619_ADC_IN_AD2R_MASK, AK4619_ADC_IN_DIFFERENTIAL));

	/* "Write '0' data on each bit" (datasheet p.64). */
	analog[AK4619_REG_RESERVED_0C - AK4619_REG_MIC_AMP_GAIN1] = 0x00U;

	/* Mutes off, DC-offset HPF on for both ADCs (HPFN = 0 means enabled). */
	analog[AK4619_REG_ADC_MUTE_HPF - AK4619_REG_MIC_AMP_GAIN1] = 0x00U;

	analog[AK4619_REG_DAC1_LCH_VOL - AK4619_REG_MIC_AMP_GAIN1] = data->props.dac_vol[0];
	analog[AK4619_REG_DAC1_RCH_VOL - AK4619_REG_MIC_AMP_GAIN1] = data->props.dac_vol[1];
	analog[AK4619_REG_DAC2_LCH_VOL - AK4619_REG_MIC_AMP_GAIN1] = data->props.dac_vol[0];
	analog[AK4619_REG_DAC2_RCH_VOL - AK4619_REG_MIC_AMP_GAIN1] = data->props.dac_vol[1];

	/*
	 * EVERY INTERNAL LOOPBACK OFF. Both multiplexers on SDIN1, so neither
	 * of the two SDOUT settings that would carry an ADC straight back into
	 * a DAC is selected (datasheet p.49, Tables 17 and 18). The read-back
	 * below turns that from a write into a fact.
	 */
	analog[AK4619_REG_DAC_INPUT_SEL - AK4619_REG_MIC_AMP_GAIN1] =
		(uint8_t)(FIELD_PREP(AK4619_DAC_SEL_DAC1_MASK, AK4619_DAC_SEL_SDIN1) |
			  FIELD_PREP(AK4619_DAC_SEL_DAC2_MASK, AK4619_DAC_SEL_SDIN1));

	/* De-emphasis off on both DACs: it would tilt the loop's high end. */
	analog[AK4619_REG_DAC_DEEMPHASIS - AK4619_REG_MIC_AMP_GAIN1] =
		(uint8_t)(FIELD_PREP(AK4619_DEM1_MASK, AK4619_DEM_OFF) |
			  FIELD_PREP(AK4619_DEM2_MASK, AK4619_DEM_OFF));

	/* Mutes off, short-delay sharp roll-off on both DACs (p.66, Table 23). */
	analog[AK4619_REG_DAC_MUTE_FLT - AK4619_REG_MIC_AMP_GAIN1] =
		AK4619_DAC_FILTER_DA1SD | AK4619_DAC_FILTER_DA2SD;

	ret = ak4619_reg_write_burst(dev, AK4619_REG_MIC_AMP_GAIN1, analog, ARRAY_SIZE(analog));
	if (ret < 0) {
		return ret;
	}

	/*
	 * Prove the loopback multiplexers rather than trusting the write. This
	 * is the one register whose value the whole loopback test depends on,
	 * and a bus that ACKs without latching is a failure mode this driver
	 * has already met once (see ak4619_link_check()).
	 */
	ret = ak4619_check_no_internal_loopback(dev);
	if (ret < 0) {
		return ret;
	}

	data->route_pwr = route_pwr;
	data->configured = true;

	LOG_INF("AK4619 configured: dai_type %d, %u ch x %u bit, fs %u Hz, MCLK %u Hz (%u fs), "
		"BICK %u fs, codec is a clock target",
		(int)cfg->dai_type, cfg->dai_cfg.i2s.channels, cfg->dai_cfg.i2s.word_size,
		cfg->dai_cfg.i2s.frame_clk_freq, cfg->mclk_freq,
		cfg->mclk_freq / cfg->dai_cfg.i2s.frame_clk_freq,
		(unsigned int)cfg->dai_cfg.i2s.channels * cfg->dai_cfg.i2s.word_size);
	LOG_INF("registers 0x01=0x%02x 0x02=0x%02x 0x03=0x%02x, DAC volume %d.%u dB, MIC gain "
		"%d dB, internal loopback off (0x12=0x%02x)",
		interface[0], interface[1], interface[2], CONFIG_AK4619_DAC_VOLUME_HALF_DB / 2,
		(CONFIG_AK4619_DAC_VOLUME_HALF_DB % 2) == 0 ? 0U : 5U, CONFIG_AK4619_MIC_GAIN_DB,
		analog[AK4619_REG_DAC_INPUT_SEL - AK4619_REG_MIC_AMP_GAIN1]);
	LOG_INF("still in standby: start MCLK/BICK/LRCK, then call ak4619_power_up() (p.39)");

	return 0;
}

int ak4619_power_up(const struct device *dev)
{
	struct ak4619_data *data = dev->data;
	int64_t remaining;
	int ret;

	if (!data->configured) {
		LOG_ERR("configure() has not run: releasing the reset now would start the part on "
			"an interface and a clock mode nobody chose");
		return -EPERM;
	}

	/*
	 * Finish the coupling capacitors' charge before the ADC comes up
	 * (datasheet p.42). Nearly always already elapsed - it is 100 ms from
	 * init and the clocks have had to start since - so this usually sleeps
	 * for nothing at all.
	 */
	remaining = data->analog_ready_uptime - k_uptime_get();
	if (remaining > 0) {
		LOG_DBG("waiting %d ms for the analog input coupling capacitors",
			(int)remaining);
		k_msleep((int32_t)remaining);
	}

	/* Power management bits first, then RSTN on top (datasheet p.39). */
	ret = ak4619_reg_write(dev, AK4619_REG_PWR_MGMT, data->route_pwr);
	if (ret < 0) {
		return ret;
	}

	ret = ak4619_reg_write(dev, AK4619_REG_PWR_MGMT, data->route_pwr | AK4619_PWR_MGMT_RSTN);
	if (ret < 0) {
		return ret;
	}

	LOG_INF("AK4619 in normal operation: register 0x00 = 0x%02x",
		(unsigned int)(data->route_pwr | AK4619_PWR_MGMT_RSTN));

	return 0;
}

int ak4619_power_down(const struct device *dev)
{
	return ak4619_reg_write(dev, AK4619_REG_PWR_MGMT, AK4619_PWR_MGMT_STANDBY);
}

int ak4619_set_mic_gain(const struct device *dev, uint8_t adc, int gain_db)
{
	uint8_t reg;
	uint8_t code;

	switch (adc) {
	case 1U:
		reg = AK4619_REG_MIC_AMP_GAIN1;
		break;
	case 2U:
		reg = AK4619_REG_MIC_AMP_GAIN2;
		break;
	default:
		return -EINVAL;
	}

	if (gain_db < AK4619_MIC_GAIN_MIN_DB || gain_db > AK4619_MIC_GAIN_MAX_DB ||
	    ((gain_db - AK4619_MIC_GAIN_MIN_DB) % AK4619_MIC_GAIN_STEP_DB) != 0) {
		LOG_ERR("MIC gain %d dB is not a rung of the -6..+27 dB, 3 dB ladder (datasheet "
			"p.42, Table 9)",
			gain_db);
		return -EINVAL;
	}

	code = AK4619_MIC_GAIN_CODE(gain_db);

	return ak4619_reg_write(dev, reg,
				(uint8_t)(FIELD_PREP(AK4619_MIC_GAIN_L_MASK, code) |
					  FIELD_PREP(AK4619_MIC_GAIN_R_MASK, code)));
}

/*
 * start_output() and stop_output() return void, so they cannot say why nothing
 * came out. They log, and ak4619_power_up()/ak4619_power_down() are what an
 * application that wants to know should call.
 *
 * On this part they are not output-only: ADC and DAC share RSTN, so the pair
 * moves whichever converters the configured route asked for.
 */
static void ak4619_start_output(const struct device *dev)
{
	int ret = ak4619_power_up(dev);

	if (ret < 0) {
		LOG_ERR("could not take the AK4619 out of standby (%d)", ret);
	}
}

static void ak4619_stop_output(const struct device *dev)
{
	int ret = ak4619_power_down(dev);

	if (ret < 0) {
		LOG_ERR("could not return the AK4619 to standby (%d)", ret);
	}
}

/**
 * Which of the two staged channel slots a channel identifier means: @p first
 * is 0 for Lch and 1 for Rch, @p count is 1 or 2.
 *
 * @retval 0 on success, -ENOTSUP for a channel this part does not have.
 */
static int ak4619_channel_slots(audio_channel_t channel, uint8_t *first, uint8_t *count)
{
	switch (channel) {
	case AUDIO_CHANNEL_FRONT_LEFT:
	case AUDIO_CHANNEL_HEADPHONE_LEFT:
		*first = 0U;
		*count = 1U;
		return 0;
	case AUDIO_CHANNEL_FRONT_RIGHT:
	case AUDIO_CHANNEL_HEADPHONE_RIGHT:
		*first = 1U;
		*count = 1U;
		return 0;
	case AUDIO_CHANNEL_ALL:
		*first = 0U;
		*count = 2U;
		return 0;
	default:
		return -ENOTSUP;
	}
}

/*
 * Volume is carried in half-decibels, the resolution of both ladders, and is
 * clamped nowhere: a level the part cannot reach comes back as -EINVAL with
 * the range in the message, because a volume that quietly became something
 * else is exactly the kind of thing that makes a loopback measurement lie.
 */
static int ak4619_stage_volume(uint8_t *slots, int half_db, audio_channel_t channel, int min,
			       int max, uint8_t code)
{
	uint8_t first;
	uint8_t count;
	int ret;

	ret = ak4619_channel_slots(channel, &first, &count);
	if (ret < 0) {
		LOG_ERR("channel %d is not one this part has", (int)channel);
		return ret;
	}

	if (half_db < min || half_db > max) {
		LOG_ERR("volume %d half-dB is outside %d..%d half-dB", half_db, min, max);
		return -EINVAL;
	}

	for (uint8_t i = 0U; i < count; i++) {
		slots[first + i] = code;
	}

	return 0;
}

static int ak4619_set_property(const struct device *dev, audio_property_t property,
			       audio_channel_t channel, audio_property_value_t val)
{
	struct ak4619_data *data = dev->data;

	switch (property) {
	case AUDIO_PROPERTY_OUTPUT_VOLUME:
		return ak4619_stage_volume(data->props.dac_vol, val.vol, channel,
					   AK4619_DAC_VOL_MIN_HALF_DB, AK4619_DAC_VOL_MAX_HALF_DB,
					   AK4619_DAC_VOL_CODE(val.vol));
	case AUDIO_PROPERTY_INPUT_VOLUME:
		return ak4619_stage_volume(data->props.adc_vol, val.vol, channel,
					   AK4619_ADC_VOL_MIN_HALF_DB, AK4619_ADC_VOL_MAX_HALF_DB,
					   AK4619_ADC_VOL_CODE(val.vol));
	case AUDIO_PROPERTY_OUTPUT_MUTE:
		data->props.dac_mute = val.mute;
		return 0;
	case AUDIO_PROPERTY_INPUT_MUTE:
		data->props.adc_mute = val.mute;
		return 0;
	default:
		/* No equaliser and no tone control anywhere in the map. */
		LOG_ERR("property %d has no AK4619 register behind it", (int)property);
		return -ENOTSUP;
	}
}

static int ak4619_apply_properties(const struct device *dev)
{
	struct ak4619_data *data = dev->data;
	uint8_t adc_vol[2] = {data->props.adc_vol[0], data->props.adc_vol[1]};
	uint8_t dac_vol[2] = {data->props.dac_vol[0], data->props.dac_vol[1]};
	int ret;

	ret = ak4619_reg_write_burst(dev, AK4619_REG_ADC1_LCH_VOL, adc_vol, ARRAY_SIZE(adc_vol));
	if (ret < 0) {
		return ret;
	}

	ret = ak4619_reg_write_burst(dev, AK4619_REG_DAC1_LCH_VOL, dac_vol, ARRAY_SIZE(dac_vol));
	if (ret < 0) {
		return ret;
	}

	ret = ak4619_reg_update(dev, AK4619_REG_ADC_MUTE_HPF, AK4619_ADC_MUTE_AD1MUTE,
				data->props.adc_mute ? AK4619_ADC_MUTE_AD1MUTE : 0U);
	if (ret < 0) {
		return ret;
	}

	return ak4619_reg_update(dev, AK4619_REG_DAC_MUTE_FLT, AK4619_DAC_MUTE_DA1MUTE,
				 data->props.dac_mute ? AK4619_DAC_MUTE_DA1MUTE : 0U);
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
