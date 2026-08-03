/*
 * The AK4619 driver on native_sim, against an emulated part.
 *
 * Issues #45 and #46. Both tickets' acceptance criteria are about behaviour a
 * bench can only show with hardware attached - the part answering, the reset
 * landing, an unpowered board failing rather than passing, and every register
 * of a configuration holding the value the datasheet says it should. All of it
 * is checkable on a host if the thing on the other end of the bus is a model
 * rather than a mock: see ak4619_emul.c, which implements the register file
 * and the addressing rules of datasheet pp.57-60 and can be told to misbehave.
 *
 * What it cannot check is that the configuration sounds like anything. What it
 * can check, and does, is the criterion that decides whether #47's loopback
 * measurement means anything at all: that no internal ADC-to-DAC path is left
 * enabled, so the analog cable is the only way a sample gets from the DAC back
 * to the ADC.
 *
 * Expected register values here are written out from the datasheet's own field
 * tables rather than computed from the driver's macros, for the same reason
 * datasheet_defaults[] is: a test that shares the driver's arithmetic can only
 * prove the driver is self-consistent.
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
#include "loopback_format.h"

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
 * Configuration: the audio interface, the clock mode and the analog paths.
 * Issue #46.
 *
 * The expected register values below are written out from the datasheet's own
 * field tables rather than computed from the driver's macros, for the same
 * reason datasheet_defaults[] is: a test that shares the driver's arithmetic
 * can only prove the driver is self-consistent.
 */

/** The configuration the sample and both STM32 blocks agree on. */
static void configure_the_agreed_format(audio_route_t route)
{
	struct audio_codec_cfg cfg;

	ak4619_loopback_codec_cfg(&cfg, route);
	zassert_ok(audio_codec_configure(present_dev, &cfg),
		   "the format the sample agrees on must be programmable");
}

static uint8_t peek(uint8_t reg)
{
	uint8_t val = 0xFFU;

	zassert_ok(ak4619_emul_peek(present_emul, reg, &val));

	return val;
}

ZTEST(ak4619, test_ak4619_configure_programs_the_serial_interface)
{
	configure_the_agreed_format(AUDIO_ROUTE_PLAYBACK_CAPTURE);

	/*
	 * 48 kHz, stereo, 32-bit words, I2S frame format, MCLK at 256 fs.
	 *
	 * 0x01 = 0x0C: TDM 0, DCF 000 stereo I2S (p.32 Table 2), DSL 11 =
	 *              32-bit slot (p.33 Table 4), BCKP 0 falling, SDOPH 0.
	 * 0x02 = 0x0C: SLOT 0, LRCK edge basis - which stereo mode requires
	 *              (p.32) - DIDL 11 = 32-bit in, DODL 00 = 24-bit out,
	 *              because Table 7 (p.33) has no 32-bit setting at all.
	 * 0x03 = 0x00: FS 000, MCLK 256 fs for 8 kHz..48 kHz (p.31 Table 1).
	 */
	zassert_equal(peek(AK4619_REG_AUDIO_IF_FMT1), 0x0C, "register 0x01 is 0x%02x",
		      peek(AK4619_REG_AUDIO_IF_FMT1));
	zassert_equal(peek(AK4619_REG_AUDIO_IF_FMT2), 0x0C, "register 0x02 is 0x%02x",
		      peek(AK4619_REG_AUDIO_IF_FMT2));
	zassert_equal(peek(AK4619_REG_SYS_CLK), 0x00, "register 0x03 is 0x%02x",
		      peek(AK4619_REG_SYS_CLK));
}

ZTEST(ak4619, test_ak4619_configure_caps_the_capture_word_length)
{
	/*
	 * The one place the two directions cannot be symmetric. DIDL takes
	 * 32-bit; DODL does not exist above 24-bit, so the ADC's word sits at
	 * the start of the 32-bit slot with zero padding after it. #47 has to
	 * shift the captured sample, which is why loopback_format.h says so.
	 */
	configure_the_agreed_format(AUDIO_ROUTE_PLAYBACK_CAPTURE);

	zassert_equal(peek(AK4619_REG_AUDIO_IF_FMT2) & 0x0CU, 0x0CU, "DIDL must be 32-bit");
	zassert_equal(peek(AK4619_REG_AUDIO_IF_FMT2) & 0x03U, 0x00U, "DODL must be 24-bit");
	zassert_equal(AK4619_LOOPBACK_CAPTURE_BITS, AK4619_SDOUT_WORD_BITS_MAX,
		      "the shared format must not promise more capture bits than the ADC emits");
	zassert_equal(AK4619_LOOPBACK_CAPTURE_SHIFT, 8,
		      "a 24-bit word in a 32-bit slot is left-shifted by 8");
}

ZTEST(ak4619, test_ak4619_configure_disables_every_internal_loopback)
{
	uint8_t sel;

	/*
	 * THE criterion of issue #46, and the reason #42 says a passing loop
	 * test means nothing until it has been shown to fail with the cable
	 * out. Register 0x12's two multiplexers can take a DAC's data straight
	 * off an ADC (datasheet p.49, Tables 17 and 18); with either of them on
	 * an SDOUT setting the analog cable is not in the path at all.
	 *
	 * The emulator powers up scribbled with 0x5A, which in this register is
	 * DAC2SEL 10 and DAC1SEL 10 - both on SDOUT1, i.e. the exact failure.
	 * So finding 0x00 here means configure() cleared it rather than
	 * inherited it.
	 */
	ak4619_emul_scribble(present_emul);
	configure_the_agreed_format(AUDIO_ROUTE_PLAYBACK_CAPTURE);

	sel = peek(AK4619_REG_DAC_INPUT_SEL);
	zassert_equal(sel & AK4619_DAC_SEL_LOOPBACK_MASK, 0U,
		      "register 0x12 is 0x%02x: a DAC is being fed from an ADC inside the part",
		      sel);
	zassert_equal(sel, 0x00, "both multiplexers must be on SDIN1, register 0x12 is 0x%02x",
		      sel);
	zassert_ok(ak4619_check_no_internal_loopback(present_dev));

	/* And the de-emphasis filters, which would colour the loop, are off. */
	zassert_equal(peek(AK4619_REG_DAC_DEEMPHASIS), 0x05,
		      "DEM1 and DEM2 must both be 01 (off), register 0x13 is 0x%02x",
		      peek(AK4619_REG_DAC_DEEMPHASIS));
}

ZTEST(ak4619, test_ak4619_configure_reports_a_loopback_it_cannot_clear)
{
	struct audio_codec_cfg cfg;

	/*
	 * A bus that ACKs and drops every write leaves register 0x12 holding
	 * the scribble - two multiplexers on SDOUT1. configure() reads it back
	 * precisely so that this does not become a green test run against a
	 * part that is quietly looping internally.
	 */
	ak4619_emul_scribble(present_emul);
	ak4619_emul_set_fault(present_emul, AK4619_EMUL_FAULT_IGNORE_WRITES);

	ak4619_loopback_codec_cfg(&cfg, AUDIO_ROUTE_PLAYBACK_CAPTURE);
	zassert_equal(audio_codec_configure(present_dev, &cfg), -EIO,
		      "a DAC multiplexer stuck on SDOUT must fail configure()");
}

ZTEST(ak4619, test_ak4619_configure_sets_the_analog_paths)
{
	configure_the_agreed_format(AUDIO_ROUTE_PLAYBACK_CAPTURE);

	/*
	 * ADC1 on AIN1L and AIN1R - the IN1P and IN2P pins that jacks J201 and
	 * J202 reach (#43 §4.3) - which is AD1LSEL = AD1RSEL = 01,
	 * "Single-Ended1" (datasheet p.43, Table 10). ADC2's two fields stay
	 * at the differential default: not in the loop, and powered down.
	 */
	zassert_equal(peek(AK4619_REG_ADC_INPUT_SEL), 0x50, "register 0x0B is 0x%02x",
		      peek(AK4619_REG_ADC_INPUT_SEL));

	/* MIC Gain AMP at CONFIG_AK4619_MIC_GAIN_DB, both channels (p.42). */
	zassert_equal(peek(AK4619_REG_MIC_AMP_GAIN1),
		      (uint8_t)((AK4619_MIC_GAIN_CODE(CONFIG_AK4619_MIC_GAIN_DB) << 4) |
				AK4619_MIC_GAIN_CODE(CONFIG_AK4619_MIC_GAIN_DB)),
		      "register 0x04 is 0x%02x", peek(AK4619_REG_MIC_AMP_GAIN1));

	/* DAC1 at CONFIG_AK4619_DAC_VOLUME_HALF_DB, not at the 0x18 default. */
	zassert_equal(peek(AK4619_REG_DAC1_LCH_VOL),
		      (uint8_t)(0x18 - CONFIG_AK4619_DAC_VOLUME_HALF_DB),
		      "register 0x0E is 0x%02x", peek(AK4619_REG_DAC1_LCH_VOL));
	zassert_equal(peek(AK4619_REG_DAC1_RCH_VOL), peek(AK4619_REG_DAC1_LCH_VOL),
		      "the two DAC1 channels must be at the same level");

	/* ADC digital volume, and mutes and the DC-offset HPF left as chosen. */
	zassert_equal(peek(AK4619_REG_ADC1_LCH_VOL),
		      (uint8_t)(0x30 - CONFIG_AK4619_ADC_VOLUME_HALF_DB),
		      "register 0x06 is 0x%02x", peek(AK4619_REG_ADC1_LCH_VOL));
	zassert_equal(peek(AK4619_REG_ADC_MUTE_HPF), 0x00,
		      "ADC mutes off and both HPFs enabled, register 0x0D is 0x%02x",
		      peek(AK4619_REG_ADC_MUTE_HPF));
	zassert_equal(peek(AK4619_REG_DAC_MUTE_FLT), 0x0A,
		      "DAC mutes off, short-delay sharp roll-off, register 0x14 is 0x%02x",
		      peek(AK4619_REG_DAC_MUTE_FLT));
	zassert_equal(peek(AK4619_REG_RESERVED_0C), 0x00, "register 0x0C must be written as zero");
}

ZTEST(ak4619, test_ak4619_configure_leaves_the_part_in_standby)
{
	/*
	 * "After setting the control register, supply the necessary system
	 * clock (MCLK, BICK, LRCK) and then release the standby state" (p.39).
	 * On this board the clocks come from the STM32 and do not exist yet, so
	 * a configure() that released RSTN would start the part against a dead
	 * MCLK.
	 */
	configure_the_agreed_format(AUDIO_ROUTE_PLAYBACK_CAPTURE);

	zassert_equal(peek(AK4619_REG_PWR_MGMT), AK4619_PWR_MGMT_STANDBY,
		      "register 0x00 is 0x%02x after configure(), want standby",
		      peek(AK4619_REG_PWR_MGMT));
}

/*
 * Configurations the part cannot run. Each one is a way for the two ends of
 * the wire to disagree, and every one of them is silent on a scope.
 */

ZTEST(ak4619, test_ak4619_configure_rejects_a_codec_clock_controller)
{
	struct audio_codec_cfg cfg;

	/*
	 * MCLK, BICK and LRCK are input pins (datasheet p.7) and #43 §2 made
	 * the STM32 the source. A caller that drops the target bits is asking
	 * for two push-pull outputs on one wire.
	 */
	ak4619_loopback_codec_cfg(&cfg, AUDIO_ROUTE_PLAYBACK_CAPTURE);
	cfg.dai_cfg.i2s.options &= ~(I2S_OPT_BIT_CLK_TARGET | I2S_OPT_FRAME_CLK_TARGET);

	zassert_equal(audio_codec_configure(present_dev, &cfg), -ENOTSUP);
}

ZTEST(ak4619, test_ak4619_configure_rejects_an_impossible_mclk_ratio)
{
	struct audio_codec_cfg cfg;

	/* 128 fs is legal only at 192 kHz (datasheet p.31, Table 1). */
	ak4619_loopback_codec_cfg(&cfg, AUDIO_ROUTE_PLAYBACK_CAPTURE);
	cfg.mclk_freq = AK4619_LOOPBACK_RATE_HZ * 128U;
	zassert_equal(audio_codec_configure(present_dev, &cfg), -ENOTSUP);

	/* And a ratio that is not a whole number at all. */
	ak4619_loopback_codec_cfg(&cfg, AUDIO_ROUTE_PLAYBACK_CAPTURE);
	cfg.mclk_freq = 11289600U;
	zassert_equal(audio_codec_configure(present_dev, &cfg), -ENOTSUP);

	/* An unstated MCLK is not a default; the part has no PLL to guess with. */
	ak4619_loopback_codec_cfg(&cfg, AUDIO_ROUTE_PLAYBACK_CAPTURE);
	cfg.mclk_freq = 0U;
	zassert_equal(audio_codec_configure(present_dev, &cfg), -EINVAL);
}

ZTEST(ak4619, test_ak4619_configure_rejects_an_illegal_bit_clock)
{
	struct audio_codec_cfg cfg;

	/*
	 * 20-bit stereo asks for BICK = 40 fs, which is not one of the 32, 48,
	 * 64, 128 or 256 fs the part locks to (datasheet p.31). The word length
	 * itself is legal, which is what makes this worth a test.
	 */
	ak4619_loopback_codec_cfg(&cfg, AUDIO_ROUTE_PLAYBACK_CAPTURE);
	cfg.dai_cfg.i2s.word_size = 20U;
	zassert_equal(audio_codec_configure(present_dev, &cfg), -ENOTSUP);

	/* And a word length off the ladder entirely. */
	ak4619_loopback_codec_cfg(&cfg, AUDIO_ROUTE_PLAYBACK_CAPTURE);
	cfg.dai_cfg.i2s.word_size = 8U;
	zassert_equal(audio_codec_configure(present_dev, &cfg), -ENOTSUP);
}

ZTEST(ak4619, test_ak4619_configure_rejects_a_dai_and_i2s_format_disagreement)
{
	struct audio_codec_cfg cfg;

	/*
	 * The codec is programmed from dai_type and the STM32 block from
	 * i2s.format. If the two disagree the parts are wired to different
	 * conventions and neither driver notices - this is the mismatch issue
	 * #46 is written around.
	 */
	ak4619_loopback_codec_cfg(&cfg, AUDIO_ROUTE_PLAYBACK_CAPTURE);
	cfg.dai_cfg.i2s.format = I2S_FMT_DATA_FORMAT_LEFT_JUSTIFIED;

	zassert_equal(audio_codec_configure(present_dev, &cfg), -EINVAL);
}

ZTEST(ak4619, test_ak4619_configure_rejects_formats_the_part_has_no_row_for)
{
	struct audio_codec_cfg cfg;

	/* Right-justified is absent from Table 2 (datasheet p.32). */
	ak4619_loopback_codec_cfg(&cfg, AUDIO_ROUTE_PLAYBACK_CAPTURE);
	cfg.dai_type = AUDIO_DAI_TYPE_RIGHT_JUSTIFIED;
	cfg.dai_cfg.i2s.format = I2S_FMT_DATA_FORMAT_RIGHT_JUSTIFIED;
	zassert_equal(audio_codec_configure(present_dev, &cfg), -ENOTSUP);

	/* Neither is an LRCK polarity control. */
	ak4619_loopback_codec_cfg(&cfg, AUDIO_ROUTE_PLAYBACK_CAPTURE);
	cfg.dai_cfg.i2s.format |= I2S_FMT_FRAME_CLK_INV;
	zassert_equal(audio_codec_configure(present_dev, &cfg), -ENOTSUP);

	/* Mono has no row either: the stereo modes carry two channels. */
	ak4619_loopback_codec_cfg(&cfg, AUDIO_ROUTE_PLAYBACK_CAPTURE);
	cfg.dai_cfg.i2s.channels = 1U;
	zassert_equal(audio_codec_configure(present_dev, &cfg), -ENOTSUP);
}

ZTEST(ak4619, test_ak4619_configure_rejects_the_bypass_route)
{
	struct audio_codec_cfg cfg;

	/*
	 * The only AK4619 path from an input to an output that skips the
	 * converters is the internal ADC-to-DAC loopback, so accepting
	 * AUDIO_ROUTE_BYPASS would mean enabling the thing #42 forbids. It also
	 * means a zeroed audio_codec_cfg - BYPASS is 0 - cannot configure the
	 * part by accident.
	 */
	ak4619_loopback_codec_cfg(&cfg, AUDIO_ROUTE_BYPASS);
	zassert_equal(audio_codec_configure(present_dev, &cfg), -ENOTSUP);
}

ZTEST(ak4619, test_ak4619_configure_rejects_a_zeroed_config)
{
	struct audio_codec_cfg cfg = {0};

	zassert_true(audio_codec_configure(present_dev, &cfg) < 0,
		     "an all-zero configuration states no clock role and no route");
}

/*
 * Leaving standby, and coming back.
 */

ZTEST(ak4619, test_ak4619_power_up_needs_a_configuration_first)
{
	/*
	 * ak4619_before() has just reset the part, so nothing has configured
	 * it. Releasing RSTN here would start it on whatever the reset defaults
	 * happen to be, against clocks nobody has matched.
	 */
	zassert_equal(ak4619_power_up(present_dev), -EPERM);
	zassert_equal(peek(AK4619_REG_PWR_MGMT), AK4619_PWR_MGMT_STANDBY,
		      "a refused power-up must not have written register 0x00");
}

ZTEST(ak4619, test_ak4619_power_up_releases_the_reset_last)
{
	configure_the_agreed_format(AUDIO_ROUTE_PLAYBACK_CAPTURE);

	zassert_ok(ak4619_power_up(present_dev));

	/*
	 * ADC1 and DAC1 in normal operation with RSTN released - the bottom row
	 * of Table 8 (p.38). ADC2 and DAC2 stay down: their jacks are not in
	 * the loop (#43 §4.1).
	 */
	zassert_equal(peek(AK4619_REG_PWR_MGMT),
		      AK4619_PWR_MGMT_PMAD1 | AK4619_PWR_MGMT_PMDA1 | AK4619_PWR_MGMT_RSTN,
		      "register 0x00 is 0x%02x", peek(AK4619_REG_PWR_MGMT));
}

ZTEST(ak4619, test_ak4619_power_up_follows_the_configured_route)
{
	configure_the_agreed_format(AUDIO_ROUTE_PLAYBACK);
	zassert_ok(ak4619_power_up(present_dev));
	zassert_equal(peek(AK4619_REG_PWR_MGMT), AK4619_PWR_MGMT_PMDA1 | AK4619_PWR_MGMT_RSTN,
		      "playback-only must leave the ADC powered down");

	configure_the_agreed_format(AUDIO_ROUTE_CAPTURE);
	zassert_ok(ak4619_power_up(present_dev));
	zassert_equal(peek(AK4619_REG_PWR_MGMT), AK4619_PWR_MGMT_PMAD1 | AK4619_PWR_MGMT_RSTN,
		      "capture-only must leave the DAC powered down");
}

ZTEST(ak4619, test_ak4619_power_down_returns_to_standby_without_losing_the_format)
{
	configure_the_agreed_format(AUDIO_ROUTE_PLAYBACK_CAPTURE);
	zassert_ok(ak4619_power_up(present_dev));

	zassert_ok(ak4619_power_down(present_dev));

	zassert_equal(peek(AK4619_REG_PWR_MGMT), AK4619_PWR_MGMT_STANDBY);
	zassert_equal(peek(AK4619_REG_AUDIO_IF_FMT1), 0x0C,
		      "only register 0x00 may move - the format has to survive");
	zassert_equal(peek(AK4619_REG_DAC_INPUT_SEL), 0x00,
		      "and so does the routing, loopback included");

	/* And it can go again without being reconfigured. */
	zassert_ok(ak4619_power_up(present_dev));
	zassert_equal(peek(AK4619_REG_PWR_MGMT),
		      AK4619_PWR_MGMT_PMAD1 | AK4619_PWR_MGMT_PMDA1 | AK4619_PWR_MGMT_RSTN);
}

ZTEST(ak4619, test_ak4619_start_output_powers_the_part_up)
{
	/* The audio_codec API's void-returning twin of ak4619_power_up(). */
	configure_the_agreed_format(AUDIO_ROUTE_PLAYBACK_CAPTURE);

	audio_codec_start_output(present_dev);
	zassert_equal(peek(AK4619_REG_PWR_MGMT) & AK4619_PWR_MGMT_RSTN, AK4619_PWR_MGMT_RSTN);

	audio_codec_stop_output(present_dev);
	zassert_equal(peek(AK4619_REG_PWR_MGMT), AK4619_PWR_MGMT_STANDBY);
}

/*
 * Volume and mute: staged by set_property(), written by apply_properties().
 */

ZTEST(ak4619, test_ak4619_set_property_writes_nothing_until_applied)
{
	audio_property_value_t vol = {.vol = -20}; /* -10.0 dB */
	uint8_t before;

	configure_the_agreed_format(AUDIO_ROUTE_PLAYBACK_CAPTURE);
	before = peek(AK4619_REG_DAC1_LCH_VOL);

	zassert_ok(audio_codec_set_property(present_dev, AUDIO_PROPERTY_OUTPUT_VOLUME,
					    AUDIO_CHANNEL_ALL, vol));
	zassert_equal(peek(AK4619_REG_DAC1_LCH_VOL), before,
		      "set_property() must cache, not write");

	zassert_ok(audio_codec_apply_properties(present_dev));
	zassert_equal(peek(AK4619_REG_DAC1_LCH_VOL), 0x18 + 20,
		      "-10.0 dB is 0x18 + 20 on the DAC ladder (datasheet p.51)");
	zassert_equal(peek(AK4619_REG_DAC1_RCH_VOL), 0x18 + 20, "AUDIO_CHANNEL_ALL is both");
}

ZTEST(ak4619, test_ak4619_volume_reaches_one_channel_at_a_time)
{
	audio_property_value_t vol = {.vol = 4}; /* +2.0 dB */

	configure_the_agreed_format(AUDIO_ROUTE_PLAYBACK_CAPTURE);

	zassert_ok(audio_codec_set_property(present_dev, AUDIO_PROPERTY_OUTPUT_VOLUME,
					    AUDIO_CHANNEL_FRONT_RIGHT, vol));
	zassert_ok(audio_codec_apply_properties(present_dev));

	zassert_equal(peek(AK4619_REG_DAC1_RCH_VOL), 0x18 - 4);
	zassert_equal(peek(AK4619_REG_DAC1_LCH_VOL),
		      (uint8_t)(0x18 - CONFIG_AK4619_DAC_VOLUME_HALF_DB),
		      "the left channel must be where configure() put it");
}

ZTEST(ak4619, test_ak4619_input_volume_reaches_the_adc_ladder)
{
	audio_property_value_t vol = {.vol = 12}; /* +6.0 dB */

	configure_the_agreed_format(AUDIO_ROUTE_PLAYBACK_CAPTURE);

	zassert_ok(audio_codec_set_property(present_dev, AUDIO_PROPERTY_INPUT_VOLUME,
					    AUDIO_CHANNEL_ALL, vol));
	zassert_ok(audio_codec_apply_properties(present_dev));

	/* 0x30 is 0 dB on the ADC ladder, 0.5 dB a step (datasheet p.46). */
	zassert_equal(peek(AK4619_REG_ADC1_LCH_VOL), 0x30 - 12);
	zassert_equal(peek(AK4619_REG_ADC1_RCH_VOL), 0x30 - 12);
}

ZTEST(ak4619, test_ak4619_mute_reaches_the_soft_mute_bits)
{
	audio_property_value_t on = {.mute = true};
	audio_property_value_t off = {.mute = false};

	configure_the_agreed_format(AUDIO_ROUTE_PLAYBACK_CAPTURE);

	zassert_ok(audio_codec_set_property(present_dev, AUDIO_PROPERTY_OUTPUT_MUTE,
					    AUDIO_CHANNEL_ALL, on));
	zassert_ok(audio_codec_set_property(present_dev, AUDIO_PROPERTY_INPUT_MUTE,
					    AUDIO_CHANNEL_ALL, on));
	zassert_ok(audio_codec_apply_properties(present_dev));

	zassert_equal(peek(AK4619_REG_DAC_MUTE_FLT) & AK4619_DAC_MUTE_DA1MUTE,
		      AK4619_DAC_MUTE_DA1MUTE, "DA1MUTE must be set");
	zassert_equal(peek(AK4619_REG_ADC_MUTE_HPF) & AK4619_ADC_MUTE_AD1MUTE,
		      AK4619_ADC_MUTE_AD1MUTE, "AD1MUTE must be set");

	/* The filter selection underneath the mute bit has to survive. */
	zassert_equal(peek(AK4619_REG_DAC_MUTE_FLT) & 0x0FU, 0x0AU,
		      "the DAC digital filter bits must not move with the mute");

	zassert_ok(audio_codec_set_property(present_dev, AUDIO_PROPERTY_OUTPUT_MUTE,
					    AUDIO_CHANNEL_ALL, off));
	zassert_ok(audio_codec_apply_properties(present_dev));
	zassert_equal(peek(AK4619_REG_DAC_MUTE_FLT) & AK4619_DAC_MUTE_DA1MUTE, 0U);
}

ZTEST(ak4619, test_ak4619_rejects_properties_it_has_no_register_for)
{
	audio_property_value_t vol = {.vol = 0};

	configure_the_agreed_format(AUDIO_ROUTE_PLAYBACK_CAPTURE);

	/*
	 * A two-channel part has no rear, centre or LFE anything, and the
	 * channel is what set_property() has to refuse - there is no register
	 * to put the value in and nowhere honest to round it to.
	 */
	zassert_equal(audio_codec_set_property(present_dev, AUDIO_PROPERTY_OUTPUT_VOLUME,
					       AUDIO_CHANNEL_REAR_LEFT, vol),
		      -ENOTSUP);
	zassert_equal(audio_codec_set_property(present_dev, AUDIO_PROPERTY_INPUT_VOLUME,
					       AUDIO_CHANNEL_LFE, vol),
		      -ENOTSUP);
}

ZTEST(ak4619, test_ak4619_rejects_a_volume_off_the_ladder)
{
	audio_property_value_t too_loud = {.vol = AK4619_DAC_VOL_MAX_HALF_DB + 1};
	audio_property_value_t too_quiet = {.vol = AK4619_DAC_VOL_MIN_HALF_DB - 1};

	configure_the_agreed_format(AUDIO_ROUTE_PLAYBACK_CAPTURE);

	/*
	 * Refused rather than clamped. A volume that quietly became something
	 * else is how a level measurement across the loop starts lying.
	 */
	zassert_equal(audio_codec_set_property(present_dev, AUDIO_PROPERTY_OUTPUT_VOLUME,
					       AUDIO_CHANNEL_ALL, too_loud),
		      -EINVAL);
	zassert_equal(audio_codec_set_property(present_dev, AUDIO_PROPERTY_OUTPUT_VOLUME,
					       AUDIO_CHANNEL_ALL, too_quiet),
		      -EINVAL);
	zassert_ok(audio_codec_apply_properties(present_dev));
	zassert_equal(peek(AK4619_REG_DAC1_LCH_VOL),
		      (uint8_t)(0x18 - CONFIG_AK4619_DAC_VOLUME_HALF_DB),
		      "a refused volume must leave the staged one alone");
}

/*
 * The analog MIC gain, which is out of band because the audio_codec API's
 * input volume is the ADC's digital ladder, a different stage.
 */

ZTEST(ak4619, test_ak4619_mic_gain_walks_the_three_db_ladder)
{
	configure_the_agreed_format(AUDIO_ROUTE_PLAYBACK_CAPTURE);

	zassert_ok(ak4619_set_mic_gain(present_dev, 1, 12));
	zassert_equal(peek(AK4619_REG_MIC_AMP_GAIN1), 0x66,
		      "+12 dB is code 6 in both nibbles (datasheet p.42, Table 9)");

	zassert_ok(ak4619_set_mic_gain(present_dev, 1, AK4619_MIC_GAIN_MIN_DB));
	zassert_equal(peek(AK4619_REG_MIC_AMP_GAIN1), 0x00, "-6 dB is code 0");

	zassert_ok(ak4619_set_mic_gain(present_dev, 2, AK4619_MIC_GAIN_MAX_DB));
	zassert_equal(peek(AK4619_REG_MIC_AMP_GAIN2), 0xBB, "+27 dB is code 0x0B");

	/* Between two rungs, and off the ends. */
	zassert_equal(ak4619_set_mic_gain(present_dev, 1, 1), -EINVAL);
	zassert_equal(ak4619_set_mic_gain(present_dev, 1, 30), -EINVAL);
	zassert_equal(ak4619_set_mic_gain(present_dev, 3, 0), -EINVAL);
}
