/*
 * Board smoke test for the nucleo_h723zg audio target.
 *
 * This suite carries no audio. It answers one question: does the board come
 * up with the peripherals the AES3 loopback needs - two I2S blocks and the
 * I2C control port - and does the devicetree still describe them the way the
 * clock topology requires?
 *
 * The build-time assertions are the part that survives when nobody has the
 * hardware plugged in: they fail the CI build if the overlay ever grows an
 * MCLK output, or if the two directions collapse onto one peripheral.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/ztest.h>

#define I2S_TX_NODE   DT_ALIAS(i2s_tx)
#define I2S_RX_NODE   DT_ALIAS(i2s_rx)
#define CTRL_I2C_NODE DT_ALIAS(audio_ctrl_i2c)

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(I2S_TX_NODE),
	     "the i2s-tx alias must resolve to an enabled node");
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(I2S_RX_NODE),
	     "the i2s-rx alias must resolve to an enabled node");
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(CTRL_I2C_NODE),
	     "the audio-ctrl-i2c alias must resolve to an enabled node");

/*
 * The STM32 I2S driver rejects I2S_DIR_BOTH with -ENOSYS and programs the
 * transfer mode per direction on one set of registers, so the two directions
 * cannot share a peripheral.
 */
BUILD_ASSERT(!DT_SAME_NODE(I2S_TX_NODE, I2S_RX_NODE), "TX and RX must be separate I2S peripherals");

/*
 * The DIX4192 is the clock master. Neither block may drive MCLK, and the
 * absence of mck-enabled is what keeps the devicetree honest about that -
 * the slave selection itself happens at i2s_configure() time through
 * I2S_OPT_FRAME_CLK_TARGET / I2S_OPT_BIT_CLK_TARGET.
 */
BUILD_ASSERT(!DT_PROP(I2S_TX_NODE, mck_enabled),
	     "TX block must not drive MCLK: the codec is the clock master");
BUILD_ASSERT(!DT_PROP(I2S_RX_NODE, mck_enabled),
	     "RX block must not drive MCLK: the codec is the clock master");

static const struct device *const i2s_tx_dev = DEVICE_DT_GET(I2S_TX_NODE);
static const struct device *const i2s_rx_dev = DEVICE_DT_GET(I2S_RX_NODE);
static const struct device *const ctrl_i2c_dev = DEVICE_DT_GET(CTRL_I2C_NODE);

/*
 * Console readout, so a human watching the terminal on real hardware sees the
 * same verdict Twister parses out of the ztest summary.
 */
static void report(const char *role, const struct device *dev)
{
	printk("nucleo_h723zg smoke: %-8s %-16s %s\n", role, dev->name,
	       device_is_ready(dev) ? "ready" : "NOT READY");
}

static void *i2s_smoke_setup(void)
{
	printk("nucleo_h723zg smoke: two slave I2S blocks + control I2C\n");
	report("i2s-tx", i2s_tx_dev);
	report("i2s-rx", i2s_rx_dev);
	report("i2c", ctrl_i2c_dev);

	return NULL;
}

ZTEST_SUITE(board_i2s_smoke, NULL, i2s_smoke_setup, NULL, NULL, NULL);

ZTEST(board_i2s_smoke, test_i2s_tx_device_is_ready)
{
	zassert_true(device_is_ready(i2s_tx_dev), "%s not ready", i2s_tx_dev->name);
}

ZTEST(board_i2s_smoke, test_i2s_rx_device_is_ready)
{
	zassert_true(device_is_ready(i2s_rx_dev), "%s not ready", i2s_rx_dev->name);
}

ZTEST(board_i2s_smoke, test_ctrl_i2c_device_is_ready)
{
	zassert_true(device_is_ready(ctrl_i2c_dev), "%s not ready", ctrl_i2c_dev->name);
}
