/*
 * The loopback's oracle: turn one analyzer window into a verdict a human can
 * act on, and into the level arithmetic that verdict rests on.
 *
 * WHY THIS IS A MODULE AND NOT PART OF main()
 * -------------------------------------------
 * Everything here is arithmetic over a struct, so it is the part of issue #47
 * that can be checked without an evaluation board, a cable or a codec - and it
 * is the part where a mistake is invisible on a console. A level computed 48 dB
 * low because the captured word was taken as right-justified still prints a
 * plausible number and still prints a verdict; only a test that knows what the
 * number should be catches it. tests/samples/audio/ak4619_loopback/ builds this
 * file and does exactly that.
 *
 * WHAT IT DOES NOT DO
 * -------------------
 * Measure. The measuring is the tone analyzer's (#34): it reports, per channel,
 * how much of the channel's energy sits at each expected frequency, an RMS, and
 * whether the channel is tonal or silent. This turns those into decibels and
 * into one of a small set of named outcomes, each of which has a different
 * cause and therefore a different thing for a human to check next.
 *
 * NO FLOATING POINT
 * -----------------
 * Deliberately, like the two nodes it sits between: a verdict that pulled in
 * log10f() would put an FPU dependency on an application whose whole job is to
 * print a line of text. The decibel conversion is an integer log2 with 16
 * fractional bits, scaled by 20*log10(2).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_AUDIO_TOOLKIT_SAMPLES_AK4619_LOOPBACK_VERDICT_H_
#define ZEPHYR_AUDIO_TOOLKIT_SAMPLES_AK4619_LOOPBACK_VERDICT_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/audio/audio_nodes.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Channels this application measures; the analyzer's range (#34). */
#define AK4619_LOOPBACK_MAX_CHANNELS AUDIO_TONE_ANALYZER_MAX_CHANNELS

/**
 * Level reported for a channel carrying nothing at all, in tenths of a decibel.
 *
 * A true zero has no logarithm, and printing "-inf" invites a reader to wonder
 * whether the measurement ran. -99.9 dBFS is below anything this link can
 * produce - the ADC's own noise floor is around -100 dBFS (S/N 100 dB typ,
 * datasheet p.11) - so it reads as "nothing", which is what it means.
 */
#define AK4619_LOOPBACK_DBFS_FLOOR_X10 (-999)

/** What one run made of the loop. */
enum ak4619_loopback_verdict {
	/** Every channel carried its own tone, at the level it should have. */
	AK4619_LOOPBACK_VERDICT_PASS = 0,
	/** No window ever completed: nothing was measured. */
	AK4619_LOOPBACK_VERDICT_NO_WINDOW,
	/** A channel carried no signal at all. The unplugged-cable outcome. */
	AK4619_LOOPBACK_VERDICT_SILENT,
	/** Each channel carried the *other* channel's tone. */
	AK4619_LOOPBACK_VERDICT_SWAPPED,
	/** A tone arrived, but not the one that channel was told to expect. */
	AK4619_LOOPBACK_VERDICT_WRONG_TONE,
	/** Energy arrived with no tone in it. */
	AK4619_LOOPBACK_VERDICT_NOISE,
	/** The right tone on the right channel, too quiet. */
	AK4619_LOOPBACK_VERDICT_LEVEL_LOW,
	/** The right tone on the right channel, too loud. */
	AK4619_LOOPBACK_VERDICT_LEVEL_HIGH,
	/** The analyzer reported something this application does not know. */
	AK4619_LOOPBACK_VERDICT_UNKNOWN,
};

/** What one channel looked like, in the units a console line wants. */
struct ak4619_loopback_channel_report {
	/** RMS level over the window, in tenths of a dB below full scale. */
	int32_t level_dbfs_x10;
	/**
	 * Which expected tone this channel carried, as an index into the
	 * per-channel frequency list, or -1 if it carried none of them.
	 *
	 * Index @c c on channel @c c is correct; any other index is a swap or a
	 * mis-wire, and that is the distinction the whole two-tone stimulus
	 * exists to make.
	 */
	int8_t carries;
	/** Energy fraction at each expected tone, Q15, straight from the node. */
	int32_t in_band_q15[AUDIO_TONE_ANALYZER_MAX_TONES];
	/** True when one sinusoid explains the window (analyzer's fit). */
	bool tonal;
	/** True when the channel is below the analyzer's silence threshold. */
	bool silent;
};

/** The verdict and the per-channel numbers it was reached from. */
struct ak4619_loopback_report {
	enum ak4619_loopback_verdict verdict;
	/** Channels measured. */
	uint8_t channels;
	/** Windows the analyzer completed; 0 means nothing was measured. */
	uint32_t windows;
	/**
	 * Channel the verdict is about, or -1 when it is about all of them.
	 * A level failure names the channel that failed, because "one channel
	 * is 5 dB down" and "both are" have different causes.
	 */
	int8_t channel_at_fault;
	struct ak4619_loopback_channel_report channel[AK4619_LOOPBACK_MAX_CHANNELS];
};

/**
 * @brief Express @p value as decibels relative to @p full_scale, in tenths.
 *
 * Integer throughout: an internal log2 in Q16, scaled by 20*log10(2) = 6.0206
 * dB per octave. Accurate to well under a tenth of a decibel over the whole
 * range of the container, which is two orders of magnitude finer than any
 * threshold here.
 *
 * @param value      Magnitude to express; must not be negative.
 * @param full_scale Reference magnitude; must be positive.
 *
 * @return Tenths of a decibel, at most 0 for a value at or below full scale,
 *         and ::AK4619_LOOPBACK_DBFS_FLOOR_X10 for a value of zero or for an
 *         unusable reference.
 */
int32_t ak4619_loopback_dbfs_x10(int32_t value, int32_t full_scale);

/**
 * @brief Turn one completed analyzer window into a verdict.
 *
 * The order of the checks is the order of the causes, coarsest first, so that
 * the outcome a human is told about is the one that explains the rest: nothing
 * measured, then nothing arriving, then the wrong thing arriving, and only then
 * the right thing at the wrong level. A silent channel is never also reported
 * as "too quiet".
 *
 * @param result Filled by audio_tone_analyzer_get_result().
 * @param report Destination, overwritten in full.
 *
 * @retval 0 on success; the verdict is in @p report.
 * @retval -EINVAL if either pointer is NULL, or @p result names more channels
 *         than ::AK4619_LOOPBACK_MAX_CHANNELS.
 */
int ak4619_loopback_evaluate(const struct audio_tone_analyzer_result *result,
			     struct ak4619_loopback_report *report);

/** @brief One word for a verdict, for the PASS/FAIL line. */
const char *ak4619_loopback_verdict_name(enum ak4619_loopback_verdict verdict);

/**
 * @brief What to check next, for the line under the PASS/FAIL line.
 *
 * Every failure has a different first thing to look at, and a run at a bench at
 * midnight is exactly when nobody wants to go and read the ticket.
 */
const char *ak4619_loopback_verdict_hint(enum ak4619_loopback_verdict verdict);

/** @brief True when the verdict means the loop is good. */
static inline bool ak4619_loopback_passed(const struct ak4619_loopback_report *report)
{
	return report && report->verdict == AK4619_LOOPBACK_VERDICT_PASS;
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_AUDIO_TOOLKIT_SAMPLES_AK4619_LOOPBACK_VERDICT_H_ */
