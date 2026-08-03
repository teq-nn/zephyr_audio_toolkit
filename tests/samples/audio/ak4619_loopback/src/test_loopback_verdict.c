/*
 * The loopback's oracle on a host: the arithmetic that decides PASS or FAIL,
 * checked without an evaluation board, a cable or a codec.
 *
 * WHY THIS SUITE EXISTS
 * ---------------------
 * Everything the console prints at the end of a hardware run comes out of
 * src/loopback_verdict.c, and every mistake in it produces a plausible-looking
 * console. A level computed 48 dB low because the captured word was read as if
 * it were right-justified still prints a number, still prints a verdict, and
 * still looks like a wiring problem to whoever is holding the cable. The cases
 * below are the ones that cannot be told apart on a bench.
 *
 * The one that matters most is
 * test_loopback_verdict_fails_a_capture_that_ignored_the_slot_padding: it feeds
 * in exactly the samples the AK4619's ADC produces, once placed correctly in
 * the 32-bit slot and once placed as a naive reader would place them, and
 * requires the two to reach opposite verdicts.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "loopback_format.h"
#include "loopback_verdict.h"

/*
 * These cases carry numbers worked out by hand from the gain plan of
 * docs/hardware/akd4619-evaluation-board.md §4.5, so they are only correct for
 * that plan. Asserted rather than skipped: a change to the levels or to the
 * slot length has to be followed here, and a build error says so where a
 * skipped case would not.
 */
BUILD_ASSERT(AK4619_LOOPBACK_CAPTURE_BITS == 24U,
	     "the numbers below assume the 24-bit ADC word of a 32-bit slot");
BUILD_ASSERT(AK4619_LOOPBACK_EXPECTED_RMS_DBFS_X10 == -120,
	     "the numbers below assume the -12.0 dBFS RMS the §4.5 gain plan produces");
BUILD_ASSERT(AK4619_LOOPBACK_TONE_DBFS_X10 == -30,
	     "the numbers below assume a -3.0 dBFS peak stimulus");

/** Full scale of the ADC's own output word. */
#define ADC_FULL_SCALE (1 << (AK4619_LOOPBACK_CAPTURE_BITS - 1U))

/*
 * The peak the ADC should produce with the loop closed.
 *
 * The tone is -3.0 dBFS peak and the loop's gain is -6.0 dB (§4.5), so the
 * captured peak is -9.0 dBFS of the ADC's own full scale:
 * 2^23 * 10^(-9.02/20) = 8388608 * 0.35481 = 2976427.
 */
#define ADC_PEAK_GOOD 2976427

/** Q16 of 1/sqrt(2): a sine's RMS relative to its peak. */
#define RMS_OF_PEAK_Q16 46341

/*
 * The RMS the analyzer reports for a sine of @p container_peak in the canonical
 * container.
 *
 * Two narrowings, in the order the real path applies them: the analyzer shifts
 * every sample down by AUDIO_TONE_ANALYZER_INPUT_SHIFT before it measures
 * anything, and what it publishes is an RMS rather than a peak.
 */
static int32_t analyzer_rms_for_container_peak(int32_t container_peak)
{
	int64_t narrowed = (int64_t)(container_peak >> AUDIO_TONE_ANALYZER_INPUT_SHIFT);

	return (int32_t)((narrowed * RMS_OF_PEAK_Q16) >> 16);
}

/*
 * A result of the shape the tone analyzer publishes for a healthy stereo
 * loopback, with each channel's RMS set by the caller.
 */
static void make_result(struct audio_tone_analyzer_result *result,
			enum audio_tone_analyzer_verdict verdict, int32_t rms_lch, int32_t rms_rch)
{
	const int32_t rms[2] = {rms_lch, rms_rch};
	uint8_t ch;

	memset(result, 0, sizeof(*result));
	result->verdict = verdict;
	result->windows = 5;
	result->window_samples = AK4619_LOOPBACK_WINDOW_SAMPLES;
	result->channels = 2;
	result->tones = 2;

	for (ch = 0; ch < 2U; ch++) {
		/* Each channel carries its own tone and nothing else: nearly all
		 * of its energy in its own bin, a clean one-sinusoid fit.
		 */
		result->channel[ch].in_band_q15[ch] = AUDIO_TONE_ANALYZER_UNITY_Q15 - 100;
		result->channel[ch].in_band_q15[1U - ch] = 30;
		result->channel[ch].rms = rms[ch];
		result->channel[ch].residual_q15 = 4;
		result->channel[ch].strongest = (int8_t)ch;
		result->channel[ch].tonal = true;
		result->channel[ch].silent = rms[ch] < AUDIO_TONE_ANALYZER_SILENCE_RMS;
	}
}

ZTEST_SUITE(ak4619_loopback_verdict, NULL, NULL, NULL, NULL, NULL);

ZTEST(ak4619_loopback_verdict, test_loopback_verdict_converts_known_ratios_to_decibels)
{
	const int32_t full = AUDIO_TONE_ANALYZER_INPUT_MAX;

	zassert_equal(ak4619_loopback_dbfs_x10(full, full), 0, "full scale is 0 dBFS");
	zassert_equal(ak4619_loopback_dbfs_x10(full / 2, full), -60, "half is -6.0 dBFS");
	zassert_equal(ak4619_loopback_dbfs_x10(full / 4, full), -120, "a quarter is -12.0 dBFS");
	zassert_equal(ak4619_loopback_dbfs_x10(full / 1024, full), -602,
		      "1/1024 is -60.2 dBFS, which is where a 10-bit error would land");

	/* Zero has no logarithm, and an unusable reference is not a level. */
	zassert_equal(ak4619_loopback_dbfs_x10(0, full), AK4619_LOOPBACK_DBFS_FLOOR_X10);
	zassert_equal(ak4619_loopback_dbfs_x10(-1, full), AK4619_LOOPBACK_DBFS_FLOOR_X10);
	zassert_equal(ak4619_loopback_dbfs_x10(full, 0), AK4619_LOOPBACK_DBFS_FLOOR_X10);
}

ZTEST(ak4619_loopback_verdict, test_loopback_verdict_amplitude_matches_the_level_it_claims)
{
	/*
	 * AK4619_LOOPBACK_TONE_AMPLITUDE_Q15 and AK4619_LOOPBACK_TONE_DBFS_X10
	 * are two statements of one thing: the Q15 value is what the tone
	 * generator is given, the decibel value is what the pass window is built
	 * from. Nothing in the application compares them, so this does.
	 */
	zassert_equal(ak4619_loopback_dbfs_x10(AK4619_LOOPBACK_TONE_AMPLITUDE_Q15,
					       AUDIO_TONE_ANALYZER_INPUT_MAX),
		      AK4619_LOOPBACK_TONE_DBFS_X10,
		      "the tone's Q15 amplitude and its stated level have drifted apart");
}

ZTEST(ak4619_loopback_verdict, test_loopback_verdict_expectation_follows_the_programmed_gains)
{
	/* The expectation is the tone, plus the three gains the driver writes to
	 * the part, plus the RMS-of-a-sine conversion - and nothing else. Stated
	 * again here so that a change to any of them has to be deliberate.
	 */
	zassert_equal(AK4619_LOOPBACK_LOOP_GAIN_DBFS_X10, -60,
		      "the §4.5 gain plan is DAC -6.0 dB, MIC amp 0 dB, ADC digital 0 dB");
	zassert_equal(AK4619_LOOPBACK_EXPECTED_RMS_DBFS_X10, -120);
	zassert_equal(AK4619_LOOPBACK_LEVEL_TOLERANCE_X10, 40);
}

ZTEST(ak4619_loopback_verdict, test_loopback_verdict_passes_a_good_window)
{
	struct audio_tone_analyzer_result result;
	struct ak4619_loopback_report report;
	int32_t rms = analyzer_rms_for_container_peak(
		ak4619_loopback_capture_to_container(ADC_PEAK_GOOD));

	make_result(&result, AUDIO_TONE_ANALYZER_VERDICT_PASS, rms, rms);

	zassert_ok(ak4619_loopback_evaluate(&result, &report));
	zassert_equal(report.verdict, AK4619_LOOPBACK_VERDICT_PASS, "verdict was %s",
		      ak4619_loopback_verdict_name(report.verdict));
	zassert_true(ak4619_loopback_passed(&report));
	zassert_equal(report.channel_at_fault, -1, "a pass blames no channel");
	zassert_equal(report.channels, 2);
	zassert_equal(report.windows, 5);

	/* And the level it reports is the one the gain plan predicts, to the
	 * tenth of a decibel the console prints.
	 */
	zassert_within(report.channel[0].level_dbfs_x10, AK4619_LOOPBACK_EXPECTED_RMS_DBFS_X10, 2,
		       "Lch measured %d, expected %d", report.channel[0].level_dbfs_x10,
		       AK4619_LOOPBACK_EXPECTED_RMS_DBFS_X10);
	zassert_equal(report.channel[0].carries, 0, "Lch must carry the first tone");
	zassert_equal(report.channel[1].carries, 1, "Rch must carry the second tone");
}

ZTEST(ak4619_loopback_verdict, test_loopback_verdict_fails_a_capture_that_ignored_the_slot_padding)
{
	struct audio_tone_analyzer_result result;
	struct ak4619_loopback_report report;
	int32_t rms_placed;
	int32_t rms_naive;

	/*
	 * The same ADC output word, placed two ways.
	 *
	 * Correct: the part puts its 24-bit word at the start of the 32-bit slot
	 * and pads with zeros, so the word ends at the top of the container.
	 * Naive: treat the slot value as a right-justified 24-bit number.
	 *
	 * The two differ by 8 bits of slot padding plus the 16 the wire seam
	 * would otherwise apply - 48 dB - which is the whole difference between
	 * a working loop and one that looks broken.
	 */
	rms_placed = analyzer_rms_for_container_peak(
		ak4619_loopback_capture_to_container(ADC_PEAK_GOOD));
	rms_naive = analyzer_rms_for_container_peak(ADC_PEAK_GOOD);

	zassert_true(rms_placed > rms_naive * 100,
		     "the two placements must be orders of magnitude apart, not adjacent");

	make_result(&result, AUDIO_TONE_ANALYZER_VERDICT_PASS, rms_naive, rms_naive);
	zassert_ok(ak4619_loopback_evaluate(&result, &report));

	/* The analyzer is perfectly happy: the tone is on the right channel and
	 * it is tonal. Only the level says the sample was never put where the
	 * part actually puts it.
	 */
	zassert_equal(report.verdict, AK4619_LOOPBACK_VERDICT_LEVEL_LOW,
		      "a 48 dB placement error must fail on level, not pass quietly (%s)",
		      ak4619_loopback_verdict_name(report.verdict));
	zassert_equal(report.channel_at_fault, 0, "the first channel out of range is named");
}

ZTEST(ak4619_loopback_verdict, test_loopback_verdict_reports_a_quiet_loop_and_names_the_channel)
{
	struct audio_tone_analyzer_result result;
	struct ak4619_loopback_report report;
	int32_t good = analyzer_rms_for_container_peak(
		ak4619_loopback_capture_to_container(ADC_PEAK_GOOD));

	/* One channel 6 dB down: inside no tolerance, and a different fault from
	 * both channels being down - one bad contact rather than a gain plan
	 * that was not programmed.
	 */
	make_result(&result, AUDIO_TONE_ANALYZER_VERDICT_PASS, good, good / 2);

	zassert_ok(ak4619_loopback_evaluate(&result, &report));
	zassert_equal(report.verdict, AK4619_LOOPBACK_VERDICT_LEVEL_LOW);
	zassert_equal(report.channel_at_fault, 1, "the right channel is the one at fault");
}

ZTEST(ak4619_loopback_verdict, test_loopback_verdict_reports_a_loud_loop)
{
	struct audio_tone_analyzer_result result;
	struct ak4619_loopback_report report;
	int32_t good = analyzer_rms_for_container_peak(
		ak4619_loopback_capture_to_container(ADC_PEAK_GOOD));

	/* 6 dB up, which on this loop means the ADC is a few dB from clipping. */
	make_result(&result, AUDIO_TONE_ANALYZER_VERDICT_PASS, good * 2, good * 2);

	zassert_ok(ak4619_loopback_evaluate(&result, &report));
	zassert_equal(report.verdict, AK4619_LOOPBACK_VERDICT_LEVEL_HIGH);
	zassert_equal(report.channel_at_fault, 0);
}

ZTEST(ak4619_loopback_verdict, test_loopback_verdict_accepts_the_whole_tolerance_band)
{
	struct audio_tone_analyzer_result result;
	struct ak4619_loopback_report report;
	int32_t good = analyzer_rms_for_container_peak(
		ak4619_loopback_capture_to_container(ADC_PEAK_GOOD));

	/* 3 dB either way is inside the 4 dB budget, so neither end of a
	 * worst-case pairing of the two full-scale tolerances fails.
	 */
	make_result(&result, AUDIO_TONE_ANALYZER_VERDICT_PASS, (int32_t)(((int64_t)good * 7) / 10),
		    (int32_t)(((int64_t)good * 14) / 10));

	zassert_ok(ak4619_loopback_evaluate(&result, &report));
	zassert_equal(report.verdict, AK4619_LOOPBACK_VERDICT_PASS,
		      "+-3 dB has to stay inside the window (%s)",
		      ak4619_loopback_verdict_name(report.verdict));
}

ZTEST(ak4619_loopback_verdict, test_loopback_verdict_reports_silence_as_silence)
{
	struct audio_tone_analyzer_result result;
	struct ak4619_loopback_report report;

	/* The unplugged-cable outcome: clocks running, ADC delivering, nothing
	 * on the wire. It must never be reported as a level failure - "too
	 * quiet" would send a human looking at gain registers.
	 */
	make_result(&result, AUDIO_TONE_ANALYZER_VERDICT_SILENT, 0, 0);

	zassert_ok(ak4619_loopback_evaluate(&result, &report));
	zassert_equal(report.verdict, AK4619_LOOPBACK_VERDICT_SILENT);
	zassert_false(ak4619_loopback_passed(&report));
	zassert_equal(report.channel[0].level_dbfs_x10, AK4619_LOOPBACK_DBFS_FLOOR_X10,
		      "a silent channel reports the floor rather than an invented number");
}

ZTEST(ak4619_loopback_verdict, test_loopback_verdict_reports_a_swap_as_a_swap)
{
	struct audio_tone_analyzer_result result;
	struct ak4619_loopback_report report;
	int32_t good = analyzer_rms_for_container_peak(
		ak4619_loopback_capture_to_container(ADC_PEAK_GOOD));

	make_result(&result, AUDIO_TONE_ANALYZER_VERDICT_SWAPPED, good, good);
	/* Each channel carries the other's tone, at the right level. */
	result.channel[0].strongest = 1;
	result.channel[1].strongest = 0;

	zassert_ok(ak4619_loopback_evaluate(&result, &report));
	zassert_equal(report.verdict, AK4619_LOOPBACK_VERDICT_SWAPPED,
		      "a swap must be a swap, not a generic failure");
	zassert_equal(report.channel[0].carries, 1, "the report carries what each channel had");
	zassert_equal(report.channel[1].carries, 0);
}

ZTEST(ak4619_loopback_verdict, test_loopback_verdict_distinguishes_noise_from_a_wrong_tone)
{
	struct audio_tone_analyzer_result result;
	struct ak4619_loopback_report report;
	int32_t good = analyzer_rms_for_container_peak(
		ak4619_loopback_capture_to_container(ADC_PEAK_GOOD));

	/* Energy with no single sinusoid in it: the signature of a word carried
	 * to the peripheral in the wrong order, which is a host problem and not
	 * a wiring one.
	 */
	make_result(&result, AUDIO_TONE_ANALYZER_VERDICT_NOISE, good, good);
	result.channel[0].tonal = false;
	result.channel[1].tonal = false;

	zassert_ok(ak4619_loopback_evaluate(&result, &report));
	zassert_equal(report.verdict, AK4619_LOOPBACK_VERDICT_NOISE);
	zassert_equal(report.channel_at_fault, 0);

	/* A tone that is simply not the expected one is a different fault. */
	make_result(&result, AUDIO_TONE_ANALYZER_VERDICT_WRONG_FREQ, good, good);
	result.channel[0].strongest = -1;

	zassert_ok(ak4619_loopback_evaluate(&result, &report));
	zassert_equal(report.verdict, AK4619_LOOPBACK_VERDICT_WRONG_TONE);
	zassert_equal(report.channel[0].carries, -1);
}

ZTEST(ak4619_loopback_verdict, test_loopback_verdict_reports_no_window_before_the_first_one)
{
	struct audio_tone_analyzer_result result;
	struct ak4619_loopback_report report;

	/* A zeroed result is what audio_tone_analyzer_get_result() returns
	 * before anything has been measured. Reading it as silence would blame
	 * the cable for a capture that never ran.
	 */
	memset(&result, 0, sizeof(result));
	result.channels = 2;

	zassert_ok(ak4619_loopback_evaluate(&result, &report));
	zassert_equal(report.verdict, AK4619_LOOPBACK_VERDICT_NO_WINDOW);
	zassert_equal(report.windows, 0);

	/* And a completed window whose verdict is still NONE is the same case. */
	make_result(&result, AUDIO_TONE_ANALYZER_VERDICT_NONE, 1000, 1000);
	zassert_ok(ak4619_loopback_evaluate(&result, &report));
	zassert_equal(report.verdict, AK4619_LOOPBACK_VERDICT_NO_WINDOW);
}

ZTEST(ak4619_loopback_verdict, test_loopback_verdict_refuses_what_it_cannot_evaluate)
{
	struct audio_tone_analyzer_result result;
	struct ak4619_loopback_report report;

	make_result(&result, AUDIO_TONE_ANALYZER_VERDICT_PASS, 1000, 1000);

	zassert_equal(ak4619_loopback_evaluate(NULL, &report), -EINVAL);
	zassert_equal(ak4619_loopback_evaluate(&result, NULL), -EINVAL);

	/* More channels than there is room to report on is refused rather than
	 * truncated: a report about two of four channels is not a verdict.
	 */
	result.channels = AK4619_LOOPBACK_MAX_CHANNELS + 1U;
	zassert_equal(ak4619_loopback_evaluate(&result, &report), -EINVAL);
}

ZTEST(ak4619_loopback_verdict, test_loopback_verdict_has_a_name_and_a_hint_for_every_outcome)
{
	/* The console prints both for whatever verdict comes out, so a verdict
	 * added without its text would print a default that helps nobody.
	 */
	for (int v = AK4619_LOOPBACK_VERDICT_PASS; v <= AK4619_LOOPBACK_VERDICT_UNKNOWN; v++) {
		const char *name = ak4619_loopback_verdict_name((enum ak4619_loopback_verdict)v);
		const char *hint = ak4619_loopback_verdict_hint((enum ak4619_loopback_verdict)v);

		zassert_not_null(name);
		zassert_not_null(hint);
		zassert_true(strlen(name) > 0, "verdict %d has no name", v);
		zassert_true(strlen(hint) > 0, "verdict %d has no hint", v);

		/* Every outcome but the first has to read as a failure on the
		 * one line a human looks at.
		 */
		if (v == AK4619_LOOPBACK_VERDICT_PASS) {
			zassert_equal(strncmp(name, "PASS", 4), 0);
		} else {
			zassert_equal(strncmp(name, "FAIL", 4), 0, "verdict %d does not say FAIL",
				      v);
		}
	}
}
