/*
 * AK4619 bring-up: report on the console whether the codec on the control bus
 * is really answering, then program the audio interface, the clock mode and
 * the analog paths and read every one of those registers back.
 *
 * Issues #45 and #46. This is the demonstrable half of the driver: it says out
 * loud what happened at init, re-runs the write/read/verify so the verdict on
 * screen is produced now rather than remembered from boot, and prints the
 * register block the configuration actually landed - including register 0x12,
 * the one that decides whether the loop test can be fooled by an internal path
 * with no cable attached.
 *
 * WHAT IT DELIBERATELY DOES NOT DO
 * --------------------------------
 * Take the part out of standby. The datasheet's order is "After setting the
 * control register, supply the necessary system clock (MCLK, BICK, LRCK) and
 * then release the standby state" (p.39), and on this board the clocks come
 * from the STM32's i2s2 block (#43 §2), which nothing here starts. So this
 * image configures and stops. Issue #47 adds the tone: start i2s2, start i2s3,
 * call ak4619_power_up(), play, capture, report.
 *
 * Wiring, switch positions and jumpers: docs/hardware/akd4619-evaluation-board.md.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "ak4619.h"
#include "loopback_format.h"

#ifndef CONFIG_AK4619
#error "this sample exists to demonstrate the AK4619 driver - CONFIG_AK4619 must be y"
#endif

/*
 * dts/boards/nucleo_h723zg.overlay aliases audio-codec to the AK4619 node on
 * i2c1. DEVICE_DT_GET() on it is a build-time reference: if the alias is
 * missing the build fails here rather than the application discovering it at
 * runtime.
 */
#define CODEC_NODE DT_ALIAS(audio_codec)

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(CODEC_NODE),
	     "the audio-codec alias must point at an enabled node");

static const struct device *const codec = DEVICE_DT_GET(CODEC_NODE);

/* The registers the configuration owns, with what each one decides. */
static const struct {
	uint8_t reg;
	const char *what;
} configured_registers[] = {
	{AK4619_REG_PWR_MGMT, "power management (00 = standby, RSTN asserted)"},
	{AK4619_REG_AUDIO_IF_FMT1, "format: TDM/DCF/DSL/BCKP/SDOPH"},
	{AK4619_REG_AUDIO_IF_FMT2, "format: SLOT/DIDL/DODL"},
	{AK4619_REG_SYS_CLK, "FS[2:0]: the MCLK ratio the part expects"},
	{AK4619_REG_MIC_AMP_GAIN1, "ADC1 analog MIC gain, Lch and Rch"},
	{AK4619_REG_ADC1_LCH_VOL, "ADC1 Lch digital volume"},
	{AK4619_REG_ADC1_RCH_VOL, "ADC1 Rch digital volume"},
	{AK4619_REG_ADC_INPUT_SEL, "ADC input select (single-ended AIN1L/AIN1R)"},
	{AK4619_REG_ADC_MUTE_HPF, "ADC mutes and DC-offset HPF"},
	{AK4619_REG_DAC1_LCH_VOL, "DAC1 Lch digital volume"},
	{AK4619_REG_DAC1_RCH_VOL, "DAC1 Rch digital volume"},
	{AK4619_REG_DAC_INPUT_SEL, "DAC source multiplexers - INTERNAL LOOPBACK"},
	{AK4619_REG_DAC_DEEMPHASIS, "DAC de-emphasis (05 = off on both)"},
	{AK4619_REG_DAC_MUTE_FLT, "DAC mutes and digital filter"},
};

static void dump_configuration(void)
{
	printk("\nregisters, read back from the part:\n");

	for (size_t i = 0; i < ARRAY_SIZE(configured_registers); i++) {
		uint8_t val = 0;
		int ret = ak4619_reg_read(codec, configured_registers[i].reg, &val);

		if (ret < 0) {
			printk("  0x%02x  <read failed, %d>\n", configured_registers[i].reg, ret);
			continue;
		}

		printk("  0x%02x = 0x%02x  %s\n", configured_registers[i].reg, val,
		       configured_registers[i].what);
	}
}

int main(void)
{
	struct audio_codec_cfg cfg;
	int ret;

	printk("\n=== AK4619 bring-up and configuration (issues #45, #46) ===\n");
	printk("codec node : %s\n", DT_NODE_FULL_NAME(CODEC_NODE));
	printk("i2c address: 0x%02x\n", (unsigned int)DT_REG_ADDR(CODEC_NODE));

	if (!device_is_ready(codec)) {
		printk("RESULT: FAIL - the codec did not initialise\n");
		printk("The driver logged why. The usual causes, in order:\n");
		printk("  1. the evaluation board is not powered (+5 V at J703)\n");
		printk("  2. SW500 was never taken L then H, so PDN is still low\n");
		printk("  3. PORT601 is plugged in and the on-board PIC owns the bus\n");
		printk("  4. SW502-1 is H, selecting SPI instead of I2C\n");
		printk("  5. SW502-2 is H, so the part answers at 0x11, not 0x10\n");
		printk("  6. SCL/SDA are not on TP601/TP602, or there is no common ground\n");
		return 0;
	}

	/*
	 * device_is_ready() already implies the init-time check passed - init
	 * returns an error if it did not - but running it again is what makes
	 * this a live report rather than a recollection, and it is how a
	 * disconnected wire shows up after boot.
	 */
	printk("init check : %s\n", ak4619_is_linked(codec) ? "passed at boot" : "not verified");

	ret = ak4619_link_check(codec);
	if (ret < 0) {
		printk("RESULT: FAIL - write/read/verify failed now (%d)\n", ret);
		printk("An address ACK is not proof; nothing latched the value written.\n");
		return 0;
	}

	printk("link check : PASS - the part latched and returned both test patterns\n");

	/*
	 * One definition, two consumers: the same helper fills in the
	 * i2s_config that #47 will hand to i2s_configure() for both STM32
	 * blocks. That is what stops the codec and the I2S side drifting apart
	 * into a mismatch neither driver can see.
	 */
	ak4619_loopback_codec_cfg(&cfg, AUDIO_ROUTE_PLAYBACK_CAPTURE);

	printk("\nagreed format: %u Hz, %u ch x %u bit, I2S, MCLK %u Hz (%u fs), BICK %u fs\n",
	       AK4619_LOOPBACK_RATE_HZ, AK4619_LOOPBACK_CHANNELS, AK4619_LOOPBACK_WORD_BITS,
	       AK4619_LOOPBACK_MCLK_HZ, AK4619_LOOPBACK_MCLK_RATIO, AK4619_LOOPBACK_BICK_RATIO);
	printk("clock roles : STM32 i2s2 sources MCLK/BICK/LRCK; the codec is a target\n");
	printk("levels      : DAC %d half-dB, ADC digital %d half-dB, MIC gain %d dB\n",
	       CONFIG_AK4619_DAC_VOLUME_HALF_DB, CONFIG_AK4619_ADC_VOLUME_HALF_DB,
	       CONFIG_AK4619_MIC_GAIN_DB);

	ret = audio_codec_configure(codec, &cfg);
	if (ret < 0) {
		printk("RESULT: FAIL - configure() refused this configuration (%d)\n", ret);
		printk("The driver logged which field it could not program.\n");
		return 0;
	}

	dump_configuration();

	ret = ak4619_check_no_internal_loopback(codec);
	if (ret < 0) {
		printk("\nRESULT: FAIL - an internal ADC-to-DAC path is enabled (%d)\n", ret);
		printk("A loopback test would pass with the cable unplugged. See issue #42.\n");
		return 0;
	}

	printk("\nno internal loopback: register 0x%02x has both DAC multiplexers on an SDIN\n",
	       AK4619_REG_DAC_INPUT_SEL);
	printk("pin, so the only path from DAC to ADC is the cable between J210 and\n");
	printk("J201/J202. Unplug it and the capture must go silent.\n");

	printk("\nRESULT: PASS - configured, and still in standby as the datasheet requires\n");
	printk("The part leaves standby only once MCLK, BICK and LRCK are running (p.39):\n");
	printk("start i2s2, start i2s3, then call ak4619_power_up(). That, and the tone,\n");
	printk("are issue #47.\n");

	return 0;
}
