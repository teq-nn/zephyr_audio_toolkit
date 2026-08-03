/*
 * The loopback's oracle. See loopback_verdict.h for why it is a module.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include "loopback_format.h"
#include "loopback_verdict.h"

/* Fractional bits of the internal logarithm. */
#define LOG2_FRAC_BITS 16
#define LOG2_ONE       (1U << LOG2_FRAC_BITS)

/*
 * 20*log10(2) in tenths of a decibel, scaled by 1000: one octave is 6.0206 dB,
 * i.e. 60.206 tenths. Kept as an integer numerator over a matching denominator
 * so the conversion needs no floating point and no table.
 */
#define DB_X10_PER_OCTAVE_NUM 60206
#define DB_X10_PER_OCTAVE_DEN 1000

/*
 * log2(@p value) in Q16, for value >= 1.
 *
 * The integer part is the position of the highest set bit. The fractional part
 * comes from repeatedly squaring the mantissa: squaring doubles the logarithm,
 * so each squaring that pushes the mantissa past 2 contributes a 1 bit and is
 * followed by a halving that brings it back into [1, 2). Sixteen rounds give
 * sixteen fractional bits, which is about 1e-5 of an octave - far finer than
 * the tenth of a decibel this is eventually rounded to.
 */
static uint32_t loopback_log2_q16(uint32_t value)
{
	uint32_t msb = 0;
	uint32_t mantissa;
	uint32_t frac = 0;
	uint32_t i;
	uint32_t v;

	if (value == 0U) {
		return 0U;
	}

	for (v = value; v > 1U; v >>= 1) {
		msb++;
	}

	/* mantissa = value / 2^msb, in Q16, so it lands in [1.0, 2.0). */
	if (msb <= (uint32_t)LOG2_FRAC_BITS) {
		mantissa = value << ((uint32_t)LOG2_FRAC_BITS - msb);
	} else {
		mantissa = value >> (msb - (uint32_t)LOG2_FRAC_BITS);
	}

	for (i = 0; i < (uint32_t)LOG2_FRAC_BITS; i++) {
		uint64_t squared = (uint64_t)mantissa * (uint64_t)mantissa;

		/* Q16 * Q16 is Q32; back to Q16, so still under 4.0. */
		mantissa = (uint32_t)(squared >> LOG2_FRAC_BITS);

		frac <<= 1;
		if (mantissa >= (2U * LOG2_ONE)) {
			mantissa >>= 1;
			frac |= 1U;
		}
	}

	return (msb << LOG2_FRAC_BITS) | frac;
}

int32_t ak4619_loopback_dbfs_x10(int32_t value, int32_t full_scale)
{
	int64_t octaves_q16;
	uint64_t magnitude;
	uint64_t scaled;
	bool below;

	if (value <= 0 || full_scale <= 0) {
		return AK4619_LOOPBACK_DBFS_FLOOR_X10;
	}

	octaves_q16 = (int64_t)loopback_log2_q16((uint32_t)value) -
		      (int64_t)loopback_log2_q16((uint32_t)full_scale);

	/* Sign is taken out before the scaling: C leaves the rounding of a
	 * negative division to the implementation, and a threshold comparison
	 * must not depend on that.
	 */
	below = octaves_q16 < 0;
	magnitude = below ? (uint64_t)-octaves_q16 : (uint64_t)octaves_q16;

	scaled = magnitude * (uint64_t)DB_X10_PER_OCTAVE_NUM;
	/* Round to nearest tenth rather than truncating towards zero. */
	scaled += ((uint64_t)LOG2_ONE * (uint64_t)DB_X10_PER_OCTAVE_DEN) / 2U;
	scaled /= ((uint64_t)LOG2_ONE * (uint64_t)DB_X10_PER_OCTAVE_DEN);

	return below ? -(int32_t)scaled : (int32_t)scaled;
}

/*
 * Copy the analyzer's per-channel numbers across and convert the one that needs
 * converting. Nothing is decided here: the decisions are in the caller, in the
 * order the header describes.
 */
static void loopback_fill_channels(const struct audio_tone_analyzer_result *result,
				   struct ak4619_loopback_report *report)
{
	uint8_t ch;

	for (ch = 0; ch < result->channels; ch++) {
		const struct audio_tone_analyzer_channel_result *src = &result->channel[ch];
		struct ak4619_loopback_channel_report *dst = &report->channel[ch];

		/* The analyzer measures a value it has already narrowed to 16
		 * bits (AUDIO_TONE_ANALYZER_INPUT_SHIFT), so full scale in its
		 * domain is AUDIO_TONE_ANALYZER_INPUT_MAX and not INT32_MAX.
		 * Referencing the wrong one would report every level 90 dB low.
		 */
		dst->level_dbfs_x10 =
			ak4619_loopback_dbfs_x10(src->rms, AUDIO_TONE_ANALYZER_INPUT_MAX);
		dst->carries = src->strongest;
		dst->tonal = src->tonal;
		dst->silent = src->silent;
		memcpy(dst->in_band_q15, src->in_band_q15, sizeof(dst->in_band_q15));
	}
}

/*
 * The level test, run only once the analyzer has said the right tone is on the
 * right channel. Returns the failing verdict and names the channel, or PASS.
 */
static enum ak4619_loopback_verdict loopback_check_levels(struct ak4619_loopback_report *report)
{
	const int32_t low = AK4619_LOOPBACK_EXPECTED_RMS_DBFS_X10 -
			    AK4619_LOOPBACK_LEVEL_TOLERANCE_X10;
	const int32_t high = AK4619_LOOPBACK_EXPECTED_RMS_DBFS_X10 +
			     AK4619_LOOPBACK_LEVEL_TOLERANCE_X10;
	uint8_t ch;

	for (ch = 0; ch < report->channels; ch++) {
		int32_t level = report->channel[ch].level_dbfs_x10;

		if (level < low) {
			report->channel_at_fault = (int8_t)ch;
			return AK4619_LOOPBACK_VERDICT_LEVEL_LOW;
		}

		if (level > high) {
			report->channel_at_fault = (int8_t)ch;
			return AK4619_LOOPBACK_VERDICT_LEVEL_HIGH;
		}
	}

	return AK4619_LOOPBACK_VERDICT_PASS;
}

/* The channel a non-level failure is about, or -1 when every channel shows it. */
static int8_t loopback_first_odd_channel(const struct ak4619_loopback_report *report)
{
	uint8_t ch;

	for (ch = 0; ch < report->channels; ch++) {
		if (report->channel[ch].silent || !report->channel[ch].tonal ||
		    report->channel[ch].carries != (int8_t)ch) {
			return (int8_t)ch;
		}
	}

	return -1;
}

int ak4619_loopback_evaluate(const struct audio_tone_analyzer_result *result,
			     struct ak4619_loopback_report *report)
{
	if (!result || !report) {
		return -EINVAL;
	}

	if (result->channels > AK4619_LOOPBACK_MAX_CHANNELS) {
		return -EINVAL;
	}

	memset(report, 0, sizeof(*report));
	report->channels = result->channels;
	report->windows = result->windows;
	report->channel_at_fault = -1;

	/* Before the first window completes there is no measurement to judge,
	 * and the analyzer's zeroed result would otherwise read as silence -
	 * which is a different failure with a different cause.
	 */
	if (result->windows == 0U || result->verdict == AUDIO_TONE_ANALYZER_VERDICT_NONE) {
		report->verdict = AK4619_LOOPBACK_VERDICT_NO_WINDOW;
		return 0;
	}

	loopback_fill_channels(result, report);

	switch (result->verdict) {
	case AUDIO_TONE_ANALYZER_VERDICT_SILENT:
		report->verdict = AK4619_LOOPBACK_VERDICT_SILENT;
		report->channel_at_fault = loopback_first_odd_channel(report);
		break;
	case AUDIO_TONE_ANALYZER_VERDICT_SWAPPED:
		report->verdict = AK4619_LOOPBACK_VERDICT_SWAPPED;
		break;
	case AUDIO_TONE_ANALYZER_VERDICT_WRONG_FREQ:
		report->verdict = AK4619_LOOPBACK_VERDICT_WRONG_TONE;
		report->channel_at_fault = loopback_first_odd_channel(report);
		break;
	case AUDIO_TONE_ANALYZER_VERDICT_NOISE:
		report->verdict = AK4619_LOOPBACK_VERDICT_NOISE;
		report->channel_at_fault = loopback_first_odd_channel(report);
		break;
	case AUDIO_TONE_ANALYZER_VERDICT_PASS:
		report->verdict = loopback_check_levels(report);
		break;
	default:
		report->verdict = AK4619_LOOPBACK_VERDICT_UNKNOWN;
		break;
	}

	return 0;
}

const char *ak4619_loopback_verdict_name(enum ak4619_loopback_verdict verdict)
{
	switch (verdict) {
	case AK4619_LOOPBACK_VERDICT_PASS:
		return "PASS";
	case AK4619_LOOPBACK_VERDICT_NO_WINDOW:
		return "FAIL - nothing was captured";
	case AK4619_LOOPBACK_VERDICT_SILENT:
		return "FAIL - the capture is silent";
	case AK4619_LOOPBACK_VERDICT_SWAPPED:
		return "FAIL - the channels are swapped";
	case AK4619_LOOPBACK_VERDICT_WRONG_TONE:
		return "FAIL - the wrong tone came back";
	case AK4619_LOOPBACK_VERDICT_NOISE:
		return "FAIL - energy came back with no tone in it";
	case AK4619_LOOPBACK_VERDICT_LEVEL_LOW:
		return "FAIL - the right tone, too quiet";
	case AK4619_LOOPBACK_VERDICT_LEVEL_HIGH:
		return "FAIL - the right tone, too loud";
	case AK4619_LOOPBACK_VERDICT_UNKNOWN:
	default:
		return "FAIL - the analyzer reported something unrecognised";
	}
}

const char *ak4619_loopback_verdict_hint(enum ak4619_loopback_verdict verdict)
{
	switch (verdict) {
	case AK4619_LOOPBACK_VERDICT_PASS:
		return "Now run it again with the loop cable unplugged. Until that run has "
		       "failed, this one has proved nothing (issue #42).";
	case AK4619_LOOPBACK_VERDICT_NO_WINDOW:
		return "The capture pipeline never completed a window, so i2s3 received no "
		       "data at all. Check that i2s2 is really clocking: BICK on PORT401 pin 3 "
		       "and LRCK on pin 5 must be running, and PORT402/403/405 must be at 3-4 "
		       "(PORT). This is also what a missing MCLK wire (PC6 -> PORT401 pin 1) "
		       "looks like.";
	case AK4619_LOOPBACK_VERDICT_SILENT:
		return "Clocks are running and the ADC is delivering, but there is nothing on "
		       "the wire. THIS IS THE EXPECTED RESULT WITH THE LOOP CABLE UNPLUGGED. "
		       "With the cable in, check J210 -> J201/J202, that SW500 is H, and that "
		       "JP202 is 2-3 and JP206 is 1-2.";
	case AK4619_LOOPBACK_VERDICT_SWAPPED:
		return "Both tones came back, each on the other channel. Either the Y-cable's "
		       "two input plugs are the other way round, or the tip/ring assignment of "
		       "the 3.5 mm jacks is the opposite of the one assumed (#43 "
		       "UNRESOLVED-4). Swap the two plugs at J201/J202 and run it again.";
	case AK4619_LOOPBACK_VERDICT_WRONG_TONE:
		return "A tone arrived that neither channel was supposed to carry there. Check "
		       "that only one signal source is connected, and that the loop cable is "
		       "not picking up the other DAC output (J211).";
	case AK4619_LOOPBACK_VERDICT_NOISE:
		return "Energy at the right level but no single tone in it. The most likely "
		       "cause on this hardware is the 32-bit word being carried to the "
		       "peripheral as two 16-bit halves in the opposite order - rebuild with "
		       "-DCONFIG_AK4619_LOOPBACK_SLOT_16=y and try again. Failing that: a "
		       "BICK/LRCK strap that is not shared by both blocks, or an analog input "
		       "left floating (JP201/JP207 must be open).";
	case AK4619_LOOPBACK_VERDICT_LEVEL_LOW:
		return "The loop works and is quieter than the gain plan says it should be. "
		       "Check the MIC amp is at 0 dB and the DAC volume at -6.0 dB (§4.5), "
		       "that the cable is a signal path and not a partly inserted plug, and "
		       "that JP203 is shorted.";
	case AK4619_LOOPBACK_VERDICT_LEVEL_HIGH:
		return "The loop works and is louder than the gain plan says it should be, "
		       "which risks clipping the ADC. Check the DAC digital volume actually "
		       "landed at -6.0 dB (registers 0x0E/0x0F = 0x24) and that the MIC amp "
		       "is not above 0 dB (register 0x04 = 0x22).";
	case AK4619_LOOPBACK_VERDICT_UNKNOWN:
	default:
		return "The analyzer produced a verdict this application does not know about, "
		       "which means the two have drifted apart. Report it with the numbers "
		       "above.";
	}
}
