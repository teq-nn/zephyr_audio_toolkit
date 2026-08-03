/*
 * AK4619 bring-up: report on the console whether the codec on the control bus
 * is really answering.
 *
 * Issue #45. This is the demonstrable half of the driver skeleton: it says out
 * loud what happened at init, and re-runs the write/read/verify so the verdict
 * on screen is produced now rather than remembered from boot. The tone path -
 * play out through the DAC, capture back through the ADC, report a verdict -
 * is #47's, and lands in this same directory once #46 has configured the audio
 * interface.
 *
 * Wiring, switch positions and jumpers: docs/hardware/akd4619-evaluation-board.md.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "ak4619.h"

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

int main(void)
{
	int ret;

	printk("\n=== AK4619 bring-up (issue #45) ===\n");
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

	printk("RESULT: PASS - the AK4619 latched and returned both test patterns\n");
	printk("The part is in the datasheet's standby state (registers at their\n");
	printk("reset defaults, RSTN asserted). Audio format, clocking and the\n");
	printk("analog paths are issue #46; the tone loopback is issue #47.\n");

	return 0;
}
