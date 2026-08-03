/*
 * AK4619 analog loopback: play a tone out through the codec's DAC, capture it
 * back through the codec's ADC over a physical cable, and say on the console
 * whether the loop is good.
 *
 * Issue #47, the ticket the whole #42 batch exists for. #45 brought the part up
 * on the control bus and #46 programmed its interface, clocking and analog
 * paths; this adds the clocks, the audio, and a verdict.
 *
 * WHAT PROVES ANYTHING, AND WHAT DOES NOT
 * ---------------------------------------
 * A passing run means nothing on its own. The AK4619 can route an ADC's output
 * straight into a DAC inside the package (register 0x12), and a configuration
 * regression there would produce a test that passes with no cable attached and
 * verifies nothing at all. #46 disables those paths and this application
 * re-checks them before it starts - but the check that actually settles it is
 * the human's: **run it once with the loop cable in and once with it out, and
 * keep both consoles.** Until the second run has failed, the first has proved
 * nothing. README.md is the procedure.
 *
 * WHY TWO PIPELINES
 * -----------------
 * tone_gen -> i2s_out on one, i2s_in -> tone_analyzer on the other. Each paces
 * itself on its own blocking I2S call, so neither has to know about the other,
 * and the analyzer measures the magnitude of a frequency component, which does
 * not depend on where its window starts - so nothing depends on the two staying
 * aligned either.
 *
 * Both are AUDIO_PIPELINE_DEFINE()d. The subsystem's built-in worker stack,
 * frame buffer and event slots are single-instance and owned rather than shared
 * (manifest §6): audio_pipeline_init() claims each built-in an instance leaves
 * NULL and refuses a second claimant with -EBUSY. Only one of the two would
 * strictly have to bring its own, but a hand-rolled instance that fails at
 * init is a failure with no symptom worth debugging, and two definitions cost
 * two stacks either way.
 *
 * ORDER, AND WHY IT IS THIS WAY ROUND
 * -----------------------------------
 * On this board the STM32 is the clock source (#43 §2): the AK4619's MCLK, BICK
 * and LRCK are input pins and it has no PLL, so nothing is clocked until i2s2
 * starts. The datasheet is equally firm the other way - "After setting the
 * control register, supply the necessary system clock (MCLK, BICK, LRCK) and
 * then release the standby state" (p.39). Both constraints are satisfied by one
 * order and only one:
 *
 *   1. configure the codec over I2C, and confirm it (the part stays in standby)
 *   2. start the playback pipeline: i2s2 begins clocking MCLK, BICK and LRCK
 *   3. start the capture pipeline: i2s3 locks to those clocks
 *   4. ak4619_power_up(): the converters come out of standby into a live clock
 *
 * #36's DIX bring-up ran the inverse of steps 1-2 because there the codec was
 * the clock source. Here it cannot be one.
 *
 * Wiring, switch positions and jumpers: docs/hardware/akd4619-evaluation-board.md.
 * The measurement thresholds: src/loopback_format.h and src/loopback_verdict.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/audio/audio_nodes.h>
#include <zephyr/audio/audio_pipeline.h>
#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "ak4619.h"
#include "loopback_format.h"
#include "loopback_verdict.h"

#ifndef CONFIG_AK4619
#error "this sample exists to demonstrate the AK4619 driver - CONFIG_AK4619 must be y"
#endif

/*
 * dts/boards/nucleo_h723zg.overlay aliases audio-codec to the AK4619 node on
 * i2c1, i2s-tx to i2s2 and i2s-rx to i2s3. DEVICE_DT_GET() on them is a
 * build-time reference: a missing alias fails the build here rather than being
 * discovered at run time.
 */
#define CODEC_NODE  DT_ALIAS(audio_codec)
#define I2S_TX_NODE DT_ALIAS(i2s_tx)
#define I2S_RX_NODE DT_ALIAS(i2s_rx)

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(CODEC_NODE),
	     "the audio-codec alias must point at an enabled node");

static const struct device *const codec = DEVICE_DT_GET(CODEC_NODE);

/*
 * Frame size, in TOTAL interleaved samples across both channels (manifest §5),
 * and the transfer blocks each direction gets.
 *
 * 256 total samples is 128 stereo sample sets, 2.67 ms at 48 kHz - short enough
 * that four blocks of it are a latency worth reporting rather than a delay, and
 * long enough that neither pipeline thread wakes up more than ~375 times a
 * second. The block that carries it is ROUND_UP(256 * 4, 32) = 1024 bytes, so
 * four blocks per direction is 8 KiB of transfer buffer in total.
 */
#define FRAME_SAMPLES 256U
#define TX_BLOCKS     4U
#define RX_BLOCKS     4U

/*
 * Each pipeline's own worker thread. Priority 5 is below the main thread's, so
 * the report below runs whenever it has something to say and the two workers
 * have the CPU the rest of the time; they spend nearly all of it blocked in the
 * I2S driver anyway.
 */
#define PIPELINE_STACK_SIZE 2048
#define PIPELINE_PRIORITY   5

/*
 * How long to wait for the capture to produce something, and how long to let it
 * settle once it has.
 *
 * The timeout is what makes a dead clock a diagnosable failure instead of a
 * hang: the two worker threads may block forever inside the I2S driver - that
 * is how they pace themselves - but this thread never does, so a run against a
 * board that is powered but not clocking still reaches its verdict.
 *
 * 2 s is many hundreds of 20 ms windows, so reaching the timeout means nothing
 * arrived rather than that something was slow. The settle wait covers the DAC's
 * soft-start and the ADC's DC-offset high-pass filter, neither of which is
 * instantaneous after RSTN is released.
 */
#define CAPTURE_TIMEOUT_MS 2000
#define CAPTURE_SETTLE_MS  250
#define CAPTURE_POLL_MS    5

/* Playback: a two-tone stimulus into i2s2, which drives the clocks. */
AUDIO_TONE_GEN_NODE_DEFINE(tone_source, AK4619_LOOPBACK_TONE_AMPLITUDE_Q15, 0,
			   AK4619_LOOPBACK_TONE_LCH_HZ, AK4619_LOOPBACK_TONE_RCH_HZ);
AUDIO_I2S_OUT_CLK_CONTROLLER_NODE_DEFINE(i2s_sink, &tone_source, I2S_TX_NODE, FRAME_SAMPLES,
					 TX_BLOCKS);
AUDIO_PIPELINE_DEFINE(playback_pipeline, FRAME_SAMPLES, PIPELINE_STACK_SIZE, PIPELINE_PRIORITY);

/* Capture: i2s3 into the analyzer, which is told to expect the same two tones
 * on the same two channels. Every tone is measured on every channel, which is
 * what turns a swapped pair of wires into a reported swap instead of a pass.
 */
AUDIO_I2S_IN_NODE_DEFINE(i2s_source, I2S_RX_NODE, FRAME_SAMPLES, RX_BLOCKS);
AUDIO_TONE_ANALYZER_NODE_DEFINE(analyzer, &i2s_source, AK4619_LOOPBACK_WINDOW_SAMPLES,
				AK4619_LOOPBACK_TONE_LCH_HZ, AK4619_LOOPBACK_TONE_RCH_HZ);
AUDIO_PIPELINE_DEFINE(capture_pipeline, FRAME_SAMPLES, PIPELINE_STACK_SIZE, PIPELINE_PRIORITY);

static const struct audio_pipeline_config playback_config = {
	.frame_samples = FRAME_SAMPLES,
};

static const struct audio_pipeline_config capture_config = {
	.frame_samples = FRAME_SAMPLES,
};

/*
 * The one format both pipelines are bound to, and the fourth description of the
 * same wire after the codec's, i2s2's and i2s3's.
 *
 * valid_bits_per_sample is what the I2S nodes turn into i2s_config.word_size
 * through the shared wire seam, so this field and the codec's word_size are the
 * same number by construction - which is the whole reason both come from
 * loopback_format.h.
 */
static const struct audio_stream_config stream_format = {
	.sample_rate_hz = AK4619_LOOPBACK_RATE_HZ,
	.channels = AK4619_LOOPBACK_CHANNELS,
	.valid_bits_per_sample = AK4619_LOOPBACK_WORD_BITS,
	.format = AUDIO_SAMPLE_FORMAT_S32_LE,
};

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

/** Channel names, in channel order, for the report. */
static const char *const channel_name[AK4619_LOOPBACK_MAX_CHANNELS] = {"Lch", "Rch"};

/** The frequency each channel is supposed to carry, in the same order. */
static const uint32_t channel_tone_hz[AK4619_LOOPBACK_MAX_CHANNELS] = {
	AK4619_LOOPBACK_TONE_LCH_HZ,
	AK4619_LOOPBACK_TONE_RCH_HZ,
};

BUILD_ASSERT(AK4619_LOOPBACK_CHANNELS <= AK4619_LOOPBACK_MAX_CHANNELS,
	     "every channel needs a name and an expected tone in the two tables above");

/* Tenths of a decibel as text. Printed rather than converted to a float because
 * an image that formats one float pulls the whole soft-float printf in.
 */
#define DB_TEXT_LEN 16

static const char *db_text(char *buf, size_t len, int32_t x10)
{
	int32_t whole = x10 / 10;
	uint32_t frac = (uint32_t)((x10 < 0) ? -(x10 % 10) : (x10 % 10));

	/* -0.5 dB has a whole part of 0, so the sign has to be carried
	 * separately or it would print as 0.5.
	 */
	if (x10 < 0 && whole == 0) {
		(void)snprintk(buf, len, "-0.%u", frac);
	} else {
		(void)snprintk(buf, len, "%d.%u", whole, frac);
	}

	return buf;
}

/* A Q15 fraction as 0.000 .. 1.000, for the in-band energy columns. */
#define FRACTION_TEXT_LEN 8

static const char *fraction_text(char *buf, size_t len, int32_t q15)
{
	uint32_t milli = (uint32_t)(((int64_t)q15 * 1000) / AUDIO_TONE_ANALYZER_UNITY_Q15);

	(void)snprintk(buf, len, "%u.%03u", milli / 1000U, milli % 1000U);

	return buf;
}

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

/*
 * Bring the codec up and program it, leaving it configured and in standby.
 *
 * Every failure here is reported with the board-side thing to check, because
 * this is where a board that is unpowered, unplugged from its supply or still
 * held in power-down shows up - and it shows up before anything can block on a
 * clock that will never run.
 */
static bool bring_up_codec(void)
{
	struct audio_codec_cfg cfg;
	int ret;

	printk("\n--- 1. the codec on the control bus ---\n");
	printk("codec node : %s\n", DT_NODE_FULL_NAME(CODEC_NODE));
	printk("i2c address: 0x%02x\n", (unsigned int)DT_REG_ADDR(CODEC_NODE));

	if (!device_is_ready(codec)) {
		printk("codec      : FAIL - the codec did not initialise\n");
		printk("The driver logged why. The usual causes, in order:\n");
		printk("  1. the evaluation board is not powered (+5 V at J703)\n");
		printk("  2. SW500 was never taken L then H, so PDN is still low\n");
		printk("  3. PORT601 is plugged in and the on-board PIC owns the bus\n");
		printk("  4. SW502-1 is H, selecting SPI instead of I2C\n");
		printk("  5. SW502-2 is H, so the part answers at 0x11, not 0x10\n");
		printk("  6. SCL/SDA are not on TP601/TP602, or there is no common ground\n");
		return false;
	}

	printk("init check : %s\n", ak4619_is_linked(codec) ? "passed at boot" : "not verified");

	ret = ak4619_link_check(codec);
	if (ret < 0) {
		printk("link check : FAIL - write/read/verify failed now (%d)\n", ret);
		printk("An address ACK is not proof; nothing latched the value written.\n");
		return false;
	}

	printk("link check : PASS - the part latched and returned both test patterns\n");

	printk("\n--- 2. the format all four devices are programmed for ---\n");
	printk("rate        : %u Hz, %u ch\n", AK4619_LOOPBACK_RATE_HZ, AK4619_LOOPBACK_CHANNELS);
	printk("slot        : %u bit, BICK %u fs, MCLK %u Hz (%u fs)\n", AK4619_LOOPBACK_WORD_BITS,
	       AK4619_LOOPBACK_BICK_RATIO, AK4619_LOOPBACK_MCLK_HZ, AK4619_LOOPBACK_MCLK_RATIO);
	printk("capture     : %u bit ADC word, MSB-justified in the slot (shift %u)\n",
	       AK4619_LOOPBACK_CAPTURE_BITS, AK4619_LOOPBACK_CAPTURE_SHIFT);
	printk("clock roles : i2s2 drives MCLK/BICK/LRCK; i2s3 and the codec receive them\n");
	printk("levels      : DAC %d half-dB, ADC digital %d half-dB, MIC gain %d dB\n",
	       CONFIG_AK4619_DAC_VOLUME_HALF_DB, CONFIG_AK4619_ADC_VOLUME_HALF_DB,
	       CONFIG_AK4619_MIC_GAIN_DB);

	ak4619_loopback_codec_cfg(&cfg, AUDIO_ROUTE_PLAYBACK_CAPTURE);

	ret = audio_codec_configure(codec, &cfg);
	if (ret < 0) {
		printk("configure   : FAIL - the driver refused this configuration (%d)\n", ret);
		printk("It logged which field it could not program.\n");
		return false;
	}

	printk("configure   : PASS\n");

	dump_configuration();

	ret = ak4619_check_no_internal_loopback(codec);
	if (ret < 0) {
		printk("\ninternal loopback: FAIL - an ADC-to-DAC path is enabled inside the "
		       "part (%d)\n",
		       ret);
		printk("This run would pass with the cable unplugged and would verify "
		       "nothing. See issue #42.\n");
		return false;
	}

	printk("\ninternal loopback: none. Register 0x%02x has both DAC multiplexers on an\n",
	       AK4619_REG_DAC_INPUT_SEL);
	printk("SDIN pin, so the only path from DAC to ADC is the cable between J210 and\n");
	printk("J201/J202. Unplug it and the capture must go silent.\n");

	return true;
}

/* Bind, open and start playing one pipeline. Reported by name so a failure says
 * which of the two it was.
 */
static int start_pipeline(struct audio_pipeline *pipeline,
			  const struct audio_pipeline_config *config, struct audio_node *sink,
			  const char *what)
{
	int ret;

	ret = audio_pipeline_init(pipeline, config, sink);
	if (ret < 0) {
		printk("%s: FAIL - init (%d)\n", what, ret);
		return ret;
	}

	ret = audio_pipeline_set_format(pipeline, &stream_format);
	if (ret < 0) {
		printk("%s: FAIL - set_format (%d)\n", what, ret);
		return ret;
	}

	/* start() opens the chain, which is where each I2S node configures its
	 * direction. A format the node cannot carry is refused here, with the
	 * node's own log line saying which field it was.
	 */
	ret = audio_pipeline_start(pipeline);
	if (ret < 0) {
		printk("%s: FAIL - start (%d)\n", what, ret);
		return ret;
	}

	ret = audio_pipeline_play(pipeline);
	if (ret < 0) {
		printk("%s: FAIL - play (%d)\n", what, ret);
		return ret;
	}

	printk("%s: started\n", what);

	return 0;
}

/* Drain and print whatever a pipeline had to say, so a node error that only
 * showed up as a stopped worker is on the console next to the verdict.
 */
static void report_events(struct audio_pipeline *pipeline, const char *what)
{
	struct audio_pipeline_event event;

	while (audio_pipeline_get_event(pipeline, &event, K_NO_WAIT) == 0) {
		switch (event.type) {
		case AUDIO_PIPELINE_EVENT_ERROR:
			printk("  %s: ERROR event, %d\n", what, event.err);
			break;
		case AUDIO_PIPELINE_EVENT_EOF:
			printk("  %s: EOF event - a live link should never report one\n", what);
			break;
		default:
			printk("  %s: event %d\n", what, (int)event.type);
			break;
		}
	}
}

/*
 * Watch the analyzer until it has something settled to say, or until the
 * timeout says nothing is coming.
 *
 * @param first_audio_ms Receives the delay from the codec leaving standby to the
 *                       first window that was not silent, or -1 if none was.
 *                       That figure is an upper bound on the loop's latency - it
 *                       contains one whole 20 ms window, the transfer blocks in
 *                       both directions and the converters' own group delay -
 *                       and no verdict uses it. It is recorded because a later
 *                       sample-exact comparison will need somewhere to start.
 */
static void await_measurement(struct audio_tone_analyzer_result *result, int64_t *first_audio_ms)
{
	int64_t started = k_uptime_get();
	int64_t elapsed = 0;

	*first_audio_ms = -1;

	while (elapsed < CAPTURE_TIMEOUT_MS) {
		(void)audio_tone_analyzer_get_result(&analyzer, result);

		if (result->windows > 0U && result->verdict != AUDIO_TONE_ANALYZER_VERDICT_NONE &&
		    result->verdict != AUDIO_TONE_ANALYZER_VERDICT_SILENT &&
		    *first_audio_ms < 0) {
			*first_audio_ms = elapsed;
		}

		/* Leave early only once something arrived *and* it has had time
		 * to settle; otherwise run the timeout out, which is what the
		 * unplugged control run does.
		 */
		if (*first_audio_ms >= 0 && elapsed >= CAPTURE_SETTLE_MS) {
			break;
		}

		k_msleep(CAPTURE_POLL_MS);
		elapsed = k_uptime_get() - started;
	}

	(void)audio_tone_analyzer_get_result(&analyzer, result);
}

static void print_measurement(const struct ak4619_loopback_report *report, int64_t first_audio_ms)
{
	char level[DB_TEXT_LEN];
	char in_band_a[FRACTION_TEXT_LEN];
	char in_band_b[FRACTION_TEXT_LEN];
	uint8_t ch;

	printk("\n--- 5. what came back ---\n");

	if (first_audio_ms >= 0) {
		printk("first audio %d ms after the codec left standby. That is an UPPER "
		       "BOUND on\n",
		       (int)first_audio_ms);
		printk("the loop latency - it contains a whole %u sample analyzer window and "
		       "the\n",
		       AK4619_LOOPBACK_WINDOW_SAMPLES);
		printk("transfer blocks in both directions. Record it in §4.6 of the wiring "
		       "document.\n\n");
	} else {
		printk("no window ever carried audio.\n\n");
	}

	printk("        expected      measured      carries      in band at      tonal\n");
	printk("          (RMS)         (RMS)                  %5u %5u Hz\n",
	       AK4619_LOOPBACK_TONE_LCH_HZ, AK4619_LOOPBACK_TONE_RCH_HZ);

	for (ch = 0; ch < report->channels; ch++) {
		const struct ak4619_loopback_channel_report *c = &report->channel[ch];

		printk("  %s   %6s dBFS", channel_name[ch],
		       db_text(level, sizeof(level), AK4619_LOOPBACK_EXPECTED_RMS_DBFS_X10));
		printk("  %6s dBFS", db_text(level, sizeof(level), c->level_dbfs_x10));

		if (c->silent) {
			printk("     silent  ");
		} else if (c->carries < 0) {
			printk("     none    ");
		} else {
			printk("   %6u Hz ", channel_tone_hz[c->carries]);
		}

		/* Two buffers, because both conversions happen before either is
		 * printed and one buffer would print the second value twice.
		 */
		printk("  %5s %5s",
		       fraction_text(in_band_a, sizeof(in_band_a), c->in_band_q15[0]),
		       fraction_text(in_band_b, sizeof(in_band_b), c->in_band_q15[1]));
		printk("   %s\n", c->tonal ? "yes" : "no");
	}

	printk("\nwindows measured: %u of %u samples each\n", report->windows,
	       AK4619_LOOPBACK_WINDOW_SAMPLES);

	if (report->channel_at_fault >= 0) {
		printk("the channel at fault is %s\n", channel_name[report->channel_at_fault]);
	}
}

int main(void)
{
	struct audio_tone_analyzer_result result = {0};
	struct ak4619_loopback_report report = {0};
	int64_t first_audio_ms = -1;
	char level[DB_TEXT_LEN];
	int ret;

	printk("\n=== AK4619 analog loopback: play a tone, capture it back (issue #47) ===\n");

	if (!bring_up_codec()) {
		printk("\n=== RESULT: FAIL - the codec never got as far as being configured "
		       "===\n");
		return 0;
	}

	printk("\n--- 3. what will be played ---\n");
	printk("%s %u Hz, %s %u Hz, both at %s dBFS peak\n", channel_name[0],
	       AK4619_LOOPBACK_TONE_LCH_HZ, channel_name[1], AK4619_LOOPBACK_TONE_RCH_HZ,
	       db_text(level, sizeof(level), AK4619_LOOPBACK_TONE_DBFS_X10));
	printk("expected back: %s dBFS RMS per channel",
	       db_text(level, sizeof(level), AK4619_LOOPBACK_EXPECTED_RMS_DBFS_X10));
	printk(", +/- %s dB\n", db_text(level, sizeof(level), AK4619_LOOPBACK_LEVEL_TOLERANCE_X10));
	printk("pass window  : %s", db_text(level, sizeof(level),
					    AK4619_LOOPBACK_EXPECTED_RMS_DBFS_X10 -
						    AK4619_LOOPBACK_LEVEL_TOLERANCE_X10));
	printk(" .. %s dBFS RMS\n", db_text(level, sizeof(level),
					    AK4619_LOOPBACK_EXPECTED_RMS_DBFS_X10 +
						    AK4619_LOOPBACK_LEVEL_TOLERANCE_X10));
	printk("that expectation is the tone, the DAC volume, the MIC amp and the ADC volume\n");
	printk("added up - change any of them in Kconfig and this line moves with it.\n");

	printk("\n--- 4. starting, in the only order that works ---\n");

	/* Step 2: the transmit block is the clock controller, so this is what
	 * makes MCLK, BICK and LRCK exist at all.
	 */
	ret = start_pipeline(&playback_pipeline, &playback_config, &i2s_sink,
			     "playback (tone_gen -> i2s2)");
	if (ret < 0) {
		printk("\n=== RESULT: FAIL - the playback pipeline would not start ===\n");
		return 0;
	}

	/* Step 3: give the transmit side a moment to have actually queued a
	 * block and triggered START before the receive side goes looking for
	 * the clocks it is a target on.
	 */
	k_msleep(20);

	ret = start_pipeline(&capture_pipeline, &capture_config, &analyzer,
			     "capture  (i2s3 -> tone_analyzer)");
	if (ret < 0) {
		printk("\n=== RESULT: FAIL - the capture pipeline would not start ===\n");
		return 0;
	}

	/* Step 4: and only now may the converters leave standby. */
	ret = ak4619_power_up(codec);
	if (ret < 0) {
		printk("codec power-up: FAIL (%d)\n", ret);
		printk("\n=== RESULT: FAIL - the part would not leave standby ===\n");
		return 0;
	}

	printk("codec out of standby: PASS - DAC1 and ADC1 powered, RSTN released\n");

	await_measurement(&result, &first_audio_ms);

	ret = ak4619_loopback_evaluate(&result, &report);
	if (ret < 0) {
		printk("\n=== RESULT: FAIL - the analyzer result could not be evaluated (%d) "
		       "===\n",
		       ret);
		return 0;
	}

	print_measurement(&report, first_audio_ms);

	printk("\npipeline events:\n");
	report_events(&playback_pipeline, "playback");
	report_events(&capture_pipeline, "capture ");

	/* The verdict goes out before the teardown, so it is on the console even
	 * if a worker stuck on a clock that never came makes the teardown below
	 * take forever.
	 */
	printk("\n=== RESULT: %s ===\n", ak4619_loopback_verdict_name(report.verdict));
	printk("%s\n", ak4619_loopback_verdict_hint(report.verdict));

	printk("\n--- 6. stopping ---\n");
	(void)audio_pipeline_stop(&capture_pipeline);
	(void)audio_pipeline_stop(&playback_pipeline);
	(void)ak4619_power_down(codec);
	printk("codec back in standby\n");

	if (report.verdict == AK4619_LOOPBACK_VERDICT_NO_WINDOW) {
		/* i2s_read() waits forever by design - that wait is how the
		 * source paces itself - so a receive direction that never got a
		 * clock is still inside it, and join() would wait with it. The
		 * verdict is already printed; say why the image stops here
		 * rather than appearing to hang one line later.
		 */
		printk("not joining the pipelines: the capture worker is still blocked in\n");
		printk("i2s_read() waiting for a bit clock that never arrived, and join() "
		       "would\n");
		printk("wait with it. Reset the board.\n");
		return 0;
	}

	(void)audio_pipeline_join(&capture_pipeline);
	(void)audio_pipeline_join(&playback_pipeline);
	printk("both pipelines joined; the I2S blocks are stopped and the clocks are gone\n");

	return 0;
}
