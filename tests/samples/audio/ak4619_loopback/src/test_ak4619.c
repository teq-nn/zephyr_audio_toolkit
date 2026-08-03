/*
 * The AK4619 driver skeleton on native_sim, against an emulated part.
 *
 * Issue #45's acceptance criteria are about behaviour a bench can only show
 * with hardware attached - the part answering, the reset landing, an unpowered
 * board failing rather than passing. All three are checkable on a host if the
 * thing on the other end of the bus is a model rather than a mock: see
 * ak4619_emul.c, which implements the register file and the addressing rules
 * of datasheet pp.57-60 and can be told to misbehave.
 *
 * The reset-default values below are written out from the datasheet's register
 * map table (p.60) rather than shared with the driver's table on purpose. If
 * the two ever disagree, that disagreement is the finding.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/ztest.h>

#include "ak4619.h"
#include "ak4619_emul.h"

#ifndef CONFIG_AK4619
#error "the suite compiles the driver it tests - CONFIG_AK4619 must be y"
#endif

#define PRESENT_NODE DT_NODELABEL(ak4619_present)
#define ABSENT_NODE  DT_NODELABEL(ak4619_absent)

static const struct device *const present_dev = DEVICE_DT_GET(PRESENT_NODE);
static const struct emul *const present_emul = EMUL_DT_GET(PRESENT_NODE);
static const struct device *const absent_dev = DEVICE_DT_GET(ABSENT_NODE);

/* AK4619VN datasheet 200900082-E-00, p.60, "Register Map Table". */
static const uint8_t datasheet_defaults[] = {
	0x00, 0x0C, 0x0C, 0x00, 0x22, 0x22, 0x30, 0x30, 0x30, 0x30, 0x00,
	0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0x18, 0x04, 0x05, 0x0A,
};

BUILD_ASSERT(ARRAY_SIZE(datasheet_defaults) == AK4619_REG_COUNT,
	     "the register map is 0x00..0x14 inclusive");

static void ak4619_before(void *fixture)
{
	ARG_UNUSED(fixture);

	ak4619_emul_set_fault(present_emul, AK4619_EMUL_FAULT_NONE);
	zassert_ok(ak4619_reset(present_dev), "could not return the part to defaults");
}

ZTEST_SUITE(ak4619, NULL, NULL, ak4619_before, NULL, NULL);

/*
 * Initialisation.
 */

ZTEST(ak4619, test_ak4619_present_part_becomes_ready)
{
	zassert_true(device_is_ready(present_dev), "a part that answers must initialise");
	zassert_true(ak4619_is_linked(present_dev), "init must record a verified link");
}

ZTEST(ak4619, test_ak4619_absent_part_never_reports_ready)
{
	/*
	 * The unpowered-board criterion. The emulator at 0x11 NACKs, so init
	 * polls, times out, logs why, and fails - and the application sees a
	 * device that is not ready rather than one that pretends.
	 */
	zassert_false(device_is_ready(absent_dev),
		      "a part that never ACKs must not report itself ready");
	zassert_false(ak4619_is_linked(absent_dev), "and must not claim a verified link");
}

ZTEST(ak4619, test_ak4619_init_restores_the_reset_defaults)
{
	/*
	 * The emulator powers up with a scribble pattern, not with defaults,
	 * so finding the defaults here means the driver wrote all 21 of them.
	 * ak4619_before() re-runs the reset, which is the same code path init
	 * took.
	 */
	for (uint8_t reg = 0; reg < AK4619_REG_COUNT; reg++) {
		uint8_t val = 0xFFU;

		zassert_ok(ak4619_emul_peek(present_emul, reg, &val));
		zassert_equal(val, datasheet_defaults[reg],
			      "register 0x%02x is 0x%02x, datasheet default is 0x%02x", reg, val,
			      datasheet_defaults[reg]);
	}
}

ZTEST(ak4619, test_ak4619_reset_leaves_the_part_in_standby)
{
	uint8_t pwr;

	/*
	 * Standby is the row of Table 8 (p.38) with every power-management bit
	 * and RSTN clear. Anything else and #46 would be programming format
	 * and clock registers on a running part, which p.41 forbids.
	 */
	zassert_ok(ak4619_reg_read(present_dev, AK4619_REG_PWR_MGMT, &pwr));
	zassert_equal(pwr, AK4619_PWR_MGMT_STANDBY, "power management is 0x%02x, want standby",
		      pwr);
	zassert_equal(pwr & AK4619_PWR_MGMT_RSTN, 0, "RSTN must be asserted after a reset");
}

/*
 * Register access.
 */

ZTEST(ak4619, test_ak4619_reg_write_read_round_trip)
{
	uint8_t val = 0;

	zassert_ok(ak4619_reg_write(present_dev, AK4619_REG_DAC1_LCH_VOL, 0xC3));
	zassert_ok(ak4619_reg_read(present_dev, AK4619_REG_DAC1_LCH_VOL, &val));
	zassert_equal(val, 0xC3);
}

ZTEST(ak4619, test_ak4619_reg_update_leaves_other_bits_alone)
{
	uint8_t val = 0;

	zassert_ok(ak4619_reg_write(present_dev, AK4619_REG_PWR_MGMT, 0x00));
	zassert_ok(ak4619_reg_update(present_dev, AK4619_REG_PWR_MGMT, AK4619_PWR_MGMT_PMAD1,
				     AK4619_PWR_MGMT_PMAD1));
	zassert_ok(ak4619_reg_update(present_dev, AK4619_REG_PWR_MGMT, AK4619_PWR_MGMT_RSTN,
				     AK4619_PWR_MGMT_RSTN));

	zassert_ok(ak4619_reg_read(present_dev, AK4619_REG_PWR_MGMT, &val));
	zassert_equal(val, AK4619_PWR_MGMT_PMAD1 | AK4619_PWR_MGMT_RSTN,
		      "read 0x%02x, the second update dropped the first", val);
}

ZTEST(ak4619, test_ak4619_burst_write_walks_consecutive_registers)
{
	static const uint8_t vals[] = {0x11, 0x22, 0x33};
	uint8_t val = 0;

	zassert_ok(ak4619_reg_write_burst(present_dev, AK4619_REG_MIC_AMP_GAIN1, vals,
					  ARRAY_SIZE(vals)));

	for (size_t i = 0; i < ARRAY_SIZE(vals); i++) {
		zassert_ok(ak4619_emul_peek(present_emul, AK4619_REG_MIC_AMP_GAIN1 + i, &val));
		zassert_equal(val, vals[i], "register 0x%02x is 0x%02x, wrote 0x%02x",
			      (unsigned int)(AK4619_REG_MIC_AMP_GAIN1 + i), val, vals[i]);
	}
}

ZTEST(ak4619, test_ak4619_rejects_registers_outside_the_map)
{
	static const uint8_t tail[] = {0x00, 0x00};
	uint8_t val = 0;

	/* 0x15..0x7F is prohibited (p.60). */
	zassert_equal(ak4619_reg_write(present_dev, AK4619_REG_LAST + 1U, 0x00), -EINVAL);
	zassert_equal(ak4619_reg_read(present_dev, AK4619_REG_LAST + 1U, &val), -EINVAL);

	/*
	 * And a burst that would walk off the end, which on the wire does not
	 * fail at all: the address counter rolls to 0x00 (p.57) and the tail
	 * of the transfer overwrites power management.
	 */
	zassert_equal(ak4619_reg_write_burst(present_dev, AK4619_REG_LAST, tail, ARRAY_SIZE(tail)),
		      -EINVAL);
	zassert_ok(ak4619_emul_peek(present_emul, AK4619_REG_PWR_MGMT, &val));
	zassert_equal(val, AK4619_PWR_MGMT_STANDBY, "the rejected burst still reached 0x00");
}

/*
 * The link check: an ACK is not proof.
 */

ZTEST(ak4619, test_ak4619_link_check_passes_against_a_real_part)
{
	zassert_ok(ak4619_link_check(present_dev));
}

ZTEST(ak4619, test_ak4619_link_check_restores_the_scratch_register)
{
	uint8_t val = 0;

	zassert_ok(ak4619_link_check(present_dev));
	zassert_ok(ak4619_emul_peek(present_emul, AK4619_REG_ADC1_LCH_VOL, &val));
	zassert_equal(val, datasheet_defaults[AK4619_REG_ADC1_LCH_VOL],
		      "the check left 0x%02x behind in its scratch register", val);
}

ZTEST(ak4619, test_ak4619_link_check_catches_a_bus_that_only_acks)
{
	/*
	 * Every byte is acknowledged and every write is dropped - the failure
	 * mode issue #45 names: a floating bus, or the evaluation board's own
	 * USB controller still holding SDA and SCL.
	 */
	ak4619_emul_set_fault(present_emul, AK4619_EMUL_FAULT_IGNORE_WRITES);
	zassert_equal(ak4619_link_check(present_dev), -EIO,
		      "an ACK with nothing latched must not read as success");
}

ZTEST(ak4619, test_ak4619_link_check_catches_an_idle_bus_reading_ones)
{
	ak4619_emul_set_fault(present_emul, AK4619_EMUL_FAULT_READ_ONES);
	zassert_equal(ak4619_link_check(present_dev), -EIO,
		      "0xFF on every read is an idle open-drain bus, not a codec");
}

ZTEST(ak4619, test_ak4619_link_check_reports_a_transfer_failure)
{
	ak4619_emul_set_fault(present_emul, AK4619_EMUL_FAULT_NACK);
	zassert_true(ak4619_link_check(present_dev) < 0, "a NACK must surface as an error");
}

/*
 * The seam for #46: the audio half of the codec API exists and refuses,
 * rather than accepting a configuration it does not apply.
 */

ZTEST(ak4619, test_ak4619_audio_api_is_not_implemented_yet)
{
	struct audio_codec_cfg cfg = {0};
	audio_property_value_t vol = {.vol = 0};

	zassert_equal(audio_codec_configure(present_dev, &cfg), -ENOSYS);
	zassert_equal(audio_codec_set_property(present_dev, AUDIO_PROPERTY_OUTPUT_VOLUME,
					       AUDIO_CHANNEL_ALL, vol),
		      -ENOSYS);
	zassert_equal(audio_codec_apply_properties(present_dev), -ENOSYS);
}
