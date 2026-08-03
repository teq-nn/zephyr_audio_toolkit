/*
 * Tone analyzer sink node.
 *
 * The pass/fail oracle for the loopback bring-up: it measures how much of each
 * channel's energy sits at the frequency that channel was supposed to carry,
 * and turns that into a verdict the application reads as a value (issue #34,
 * manifest §4/§7, spec §4.4/§5.2/§5.3).
 *
 * Why it measures instead of comparing
 * ------------------------------------
 * The link under test has a fixed but unmeasured latency - encoder, framing,
 * decoder and two DMA queues - so comparing the received stream against the
 * transmitted one sample by sample fails even when every part of the system is
 * correct. The magnitude of a frequency component does not depend on where the
 * window starts: for x[n] = A*cos(w*n + phi) the window's response is
 * A*N/2 whatever phi is, because the phase stays in the phase and never
 * reaches the magnitude. **That offset invariance is the load-bearing property
 * of this node**, and it is why the window is closed with a magnitude rather
 * than with a correlation against a reference.
 *
 * How it measures
 * ---------------
 * One Goertzel recurrence per (channel, expected tone) pair, i.e. every
 * expected frequency is measured on every channel. Measuring one frequency per
 * channel would answer "is a tone present here", which a swapped pair of wires
 * also answers with yes; measuring all of them on all of them is what lets the
 * verdict say *swapped* instead of *pass*.
 *
 * The recurrence is s[n] = x[n] + 2*cos(w)*s[n-1] - s[n-2], run over the whole
 * window, and the window is closed with
 * |X|^2 = s1^2 + s2^2 - 2*cos(w)*s1*s2. That identity holds for an arbitrary
 * real w, not only for a whole bin, so the coefficient is derived from the
 * frequency that was asked for rather than from the nearest bin centre: a
 * target between two bins is measured where it actually is instead of reading
 * low and looking like attenuation in the signal path. A window that puts the
 * expected frequencies on whole bins is still worth picking, because there the
 * invariance above is exact rather than nearly so - see the macro's
 * documentation.
 *
 * Alongside the bins each channel accumulates its total energy, so the result
 * is an in-band *fraction*: an absolute magnitude cannot tell a correct tone
 * from a louder wrong one, and a fraction can.
 *
 * Sizing the accumulators
 * -----------------------
 * The recurrence is a marginally stable resonator and its state is much larger
 * than the magnitude it finally yields: with h[n] = sin((n+1)*w)/sin(w) as its
 * impulse response, |s[n]| <= (n+1) * max|x| / |sin w|. Every accumulator here
 * is therefore 64 bit, the input is narrowed to the container's top 16 bits,
 * the window is capped, and open() refuses a frequency closer than one bin to
 * DC or to Nyquist - which bounds 1/|sin w| by the window length. The three
 * together make the bound a constant this file asserts at build time rather
 * than a hope. An int32_t state would wrap long before the magnitude looked
 * wrong, which is the failure mode this arithmetic exists to avoid.
 *
 * Everything is integer arithmetic against a static table. Floating point is
 * deliberately absent, for the same reason as in the generator: an oracle that
 * pulled in cosf() would put an FPU dependency on every target that ever
 * defines one of these nodes.
 *
 * All state lives in the per-instance ::audio_tone_analyzer_state allocated by
 * AUDIO_TONE_ANALYZER_NODE_DEFINE(), so two analyzers share no accumulator and
 * no verdict.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/audio/audio_node.h>
#include <zephyr/audio/audio_nodes.h>

LOG_MODULE_REGISTER(audio_tone_analyzer, LOG_LEVEL_INF);

/*
 * Quarter of a period, sampled at 256 points, as Q30 amplitudes; the 257th
 * entry is the endpoint sin(pi/2), which the mirroring below reads.
 *
 * The generator next door keeps a Q15 table because 16 bits is all its output
 * container carries. Here the table is read once per open() and the number it
 * produces is a *filter coefficient*: 2*cos(w) has to be accurate enough that
 * the resonator resonates at the frequency that was asked for, and near DC the
 * relative precision of cos(w) is much worse than its absolute precision. Q30
 * plus the interpolation below keeps the resulting frequency error under a
 * thousandth of a bin across the whole supported range.
 */
#define TONE_ANALYZER_QUARTER_POINTS 256U

/* Phase bits that select a table point; the rest are interpolated. */
#define TONE_ANALYZER_INDEX_BITS 10U
#define TONE_ANALYZER_FRAC_BITS  (32U - TONE_ANALYZER_INDEX_BITS)

/* round(pi/2 * 2^30): turns the leftover phase bits into radians in Q30. */
#define TONE_ANALYZER_HALF_PI_Q30 1686629713

/* Fixed point of the resonator coefficient: 2*cos(w) in Q24, so |coeff| <= 2^25. */
#define TONE_ANALYZER_COEFF_SHIFT 24
#define TONE_ANALYZER_COEFF_MAX   (INT64_C(2) << TONE_ANALYZER_COEFF_SHIFT)

/* Largest operand the two-term products below are normalised down to. */
#define TONE_ANALYZER_NORM_MAX (INT64_C(1) << 30)

/*
 * Largest recurrence state a supported configuration can reach.
 *
 * |s[n]| <= (n+1) * max|x| / |sin w|, and open() keeps every expected
 * frequency at least one bin away from DC and from Nyquist, so
 * |sin w| >= sin(2*pi/N) >= 6/N for every window this node accepts. That
 * leaves |s| <= N * A * N/6 with N the window and A the narrowed full scale.
 */
#define TONE_ANALYZER_STATE_MAX                                                                    \
	((int64_t)AUDIO_TONE_ANALYZER_MAX_WINDOW * (int64_t)AUDIO_TONE_ANALYZER_MAX_WINDOW *       \
	 (int64_t)AUDIO_TONE_ANALYZER_INPUT_MAX / 6)

/* sin(2*pi/N) >= 6/N is only true from about N = 12; the floor is well past it. */
BUILD_ASSERT(AUDIO_TONE_ANALYZER_MIN_WINDOW >= 16U,
	     "the accumulator bound needs sin(2*pi/N) >= 6/N, which fails for a tiny window");

/* The recurrence: coeff * s stays inside int64_t for the longest window. */
BUILD_ASSERT(TONE_ANALYZER_STATE_MAX < INT64_MAX / TONE_ANALYZER_COEFF_MAX,
	     "a full-scale input over the longest window would overflow the Goertzel state");

/* Closing a window: a^2 + b^2 - ((coeff*a)>>24)*b with |a|,|b| normalised. */
BUILD_ASSERT(TONE_ANALYZER_NORM_MAX < INT64_MAX / (4 * TONE_ANALYZER_NORM_MAX),
	     "closing a window would overflow int64_t");

/* The energy sum, and the fit sums beside it, over the longest window. */
BUILD_ASSERT((int64_t)AUDIO_TONE_ANALYZER_MAX_WINDOW * AUDIO_TONE_ANALYZER_INPUT_MAX *
		     AUDIO_TONE_ANALYZER_INPUT_MAX <
	     INT64_MAX / 4,
	     "a full-scale input over the longest window would overflow the energy sum");

static const int32_t tone_analyzer_quarter_q30[TONE_ANALYZER_QUARTER_POINTS + 1U] = {
	0, 6588356, 13176464, 19764076, 26350943, 32936819,
	39521455, 46104602, 52686014, 59265442, 65842639, 72417357,
	78989349, 85558366, 92124163, 98686491, 105245103, 111799753,
	118350194, 124896179, 131437462, 137973796, 144504935, 151030634,
	157550647, 164064728, 170572633, 177074115, 183568930, 190056834,
	196537583, 203010932, 209476638, 215934457, 222384147, 228825464,
	235258165, 241682010, 248096755, 254502159, 260897982, 267283981,
	273659918, 280025552, 286380643, 292724951, 299058239, 305380268,
	311690799, 317989595, 324276419, 330551034, 336813204, 343062693,
	349299266, 355522689, 361732726, 367929144, 374111709, 380280190,
	386434353, 392573967, 398698801, 404808624, 410903207, 416982319,
	423045732, 429093217, 435124548, 441139496, 447137835, 453119340,
	459083786, 465030947, 470960600, 476872522, 482766489, 488642281,
	494499676, 500338453, 506158392, 511959275, 517740883, 523502998,
	529245404, 534967884, 540670223, 546352205, 552013618, 557654248,
	563273883, 568872310, 574449320, 580004702, 585538248, 591049748,
	596538995, 602005783, 607449906, 612871159, 618269338, 623644239,
	628995660, 634323400, 639627258, 644907034, 650162530, 655393548,
	660599890, 665781362, 670937767, 676068911, 681174602, 686254647,
	691308855, 696337036, 701339000, 706314559, 711263525, 716185713,
	721080937, 725949013, 730789757, 735602987, 740388522, 745146182,
	749875788, 754577161, 759250125, 763894504, 768510122, 773096806,
	777654384, 782182683, 786681534, 791150767, 795590213, 799999706,
	804379079, 808728167, 813046808, 817334838, 821592095, 825818421,
	830013654, 834177638, 838310216, 842411232, 846480531, 850517961,
	854523370, 858496606, 862437520, 866345964, 870221790, 874064853,
	877875009, 881652112, 885396022, 889106597, 892783698, 896427186,
	900036924, 903612776, 907154608, 910662286, 914135678, 917574653,
	920979082, 924348837, 927683790, 930983817, 934248793, 937478595,
	940673101, 943832191, 946955747, 950043650, 953095785, 956112036,
	959092290, 962036435, 964944360, 967815955, 970651112, 973449725,
	976211688, 978936898, 981625251, 984276646, 986890984, 989468165,
	992008094, 994510675, 996975812, 999403415, 1001793390, 1004145648,
	1006460100, 1008736660, 1010975242, 1013175761, 1015338134, 1017462281,
	1019548121, 1021595575, 1023604567, 1025575020, 1027506862, 1029400018,
	1031254418, 1033069992, 1034846671, 1036584389, 1038283080, 1039942680,
	1041563127, 1043144360, 1044686319, 1046188946, 1047652185, 1049075980,
	1050460278, 1051805027, 1053110176, 1054375676, 1055601479, 1056787540,
	1057933813, 1059040255, 1060106826, 1061133483, 1062120190, 1063066909,
	1063973603, 1064840240, 1065666786, 1066453210, 1067199483, 1067905576,
	1068571464, 1069197120, 1069782521, 1070327646, 1070832474, 1071296985,
	1071721163, 1072104991, 1072448455, 1072751542, 1073014240, 1073236540,
	1073418433, 1073559913, 1073660973, 1073721611, 1073741824,
};

static const char *const tone_analyzer_verdict_name[] = {
	[AUDIO_TONE_ANALYZER_VERDICT_NONE] = "none",
	[AUDIO_TONE_ANALYZER_VERDICT_PASS] = "pass",
	[AUDIO_TONE_ANALYZER_VERDICT_SILENT] = "silent",
	[AUDIO_TONE_ANALYZER_VERDICT_SWAPPED] = "swapped",
	[AUDIO_TONE_ANALYZER_VERDICT_WRONG_FREQ] = "wrong frequency",
	[AUDIO_TONE_ANALYZER_VERDICT_NOISE] = "noise",
};

/* -------------------------------------------------------------------------
 * Integer arithmetic the measurement is built from
 * -------------------------------------------------------------------------
 */

static int64_t tone_analyzer_abs64(int64_t value)
{
	return value < 0 ? -value : value;
}

/*
 * sin(2*pi*point/1024) in Q30, @p point taken modulo the table.
 *
 * A quarter is all a sine needs: the other three follow by mirroring the index
 * and the sign. Reading it at point + 256 is cos of the same angle, which is
 * what the interpolation below needs.
 */
static int64_t tone_analyzer_table_sin_q30(uint32_t point)
{
	uint32_t index = point & (TONE_ANALYZER_QUARTER_POINTS - 1U);

	switch ((point / TONE_ANALYZER_QUARTER_POINTS) & 3U) {
	case 0U:
		return tone_analyzer_quarter_q30[index];
	case 1U:
		return tone_analyzer_quarter_q30[TONE_ANALYZER_QUARTER_POINTS - index];
	case 2U:
		return -tone_analyzer_quarter_q30[index];
	default:
		return -tone_analyzer_quarter_q30[TONE_ANALYZER_QUARTER_POINTS - index];
	}
}

/*
 * sin(@p phase) in Q30, @p phase being a fraction of a full turn in 32 bit
 * fixed point - the same phase representation the generator uses.
 *
 * The top ten bits select a table point and the remaining 22 are turned into an
 * angle d and folded in as sin(a + d) = sin a + cos a * d - sin a * d^2/2. The
 * first neglected term is d^3/6 and d is at most one table step, so the result
 * is good to about 4e-9 - some 25 bits, against the 15 a bare table lookup and
 * the 22 a linear interpolation would give. That matters here and nowhere else
 * in the module: this value becomes a resonator coefficient, and near DC a
 * coefficient error turns into a frequency error divided by sin(w).
 */
static int64_t tone_analyzer_sin_q30(uint32_t phase)
{
	uint32_t point = phase >> TONE_ANALYZER_FRAC_BITS;
	uint32_t frac = phase & BIT_MASK(TONE_ANALYZER_FRAC_BITS);
	int64_t sin_a = tone_analyzer_table_sin_q30(point);
	int64_t cos_a = tone_analyzer_table_sin_q30(point + TONE_ANALYZER_QUARTER_POINTS);
	int64_t delta = ((int64_t)frac * TONE_ANALYZER_HALF_PI_Q30) >> 30;
	int64_t half_delta_sq = (delta * delta) >> 31;

	return sin_a + ((cos_a * delta) >> 30) - ((sin_a * half_delta_sq) >> 30);
}

/*
 * Phase increment per sample for @p freq_hz at @p rate_hz: round(f * 2^32/fs),
 * i.e. the angle w as a fraction of a turn. Identical to the generator's, so
 * both ends of a link derive their angle from the same rounding.
 */
static uint32_t tone_analyzer_phase_step(uint32_t freq_hz, uint32_t rate_hz)
{
	uint64_t turns = ((uint64_t)freq_hz << 32) + (uint64_t)(rate_hz / 2U);

	return (uint32_t)(turns / rate_hz);
}

/* Integer square root of @p value, bit by bit; no libm, no floating point. */
static uint32_t tone_analyzer_isqrt(uint64_t value)
{
	uint64_t rem = value;
	uint64_t root = 0U;
	uint64_t bit = UINT64_C(1) << 62;

	while (bit > rem) {
		bit >>= 2;
	}

	while (bit != 0U) {
		if (rem >= root + bit) {
			rem -= root + bit;
			root = (root >> 1) + bit;
		} else {
			root >>= 1;
		}

		bit >>= 2;
	}

	return (uint32_t)root;
}

/*
 * |X|^2 at the end of a window, from the two states the recurrence left and
 * the coefficient it ran with.
 *
 * The states are the large intermediates the file comment bounds; the result is
 * at most (N * A)^2, because it is a plain sum of N samples against a unit
 * vector. The difference between the two is cancellation, so the states are
 * first shifted down to a size whose square and cross product fit - that costs
 * a few of the low bits of a value already 60 bits wide, and only in the
 * configurations where the states are largest, i.e. close to DC or Nyquist.
 */
static int64_t tone_analyzer_power(int64_t s1, int64_t s2, int32_t coeff_q24)
{
	int64_t peak = MAX(tone_analyzer_abs64(s1), tone_analyzer_abs64(s2));
	unsigned int shift = 0U;
	int64_t a;
	int64_t b;
	int64_t power;

	while ((peak >> shift) > TONE_ANALYZER_NORM_MAX) {
		shift++;
	}

	a = s1 >> shift;
	b = s2 >> shift;
	power = a * a + b * b - ((((int64_t)coeff_q24 * a) >> TONE_ANALYZER_COEFF_SHIFT) * b);

	/* Zero in exact arithmetic is the smallest this can be; the truncation
	 * of the shifts above can carry it a few counts past that.
	 */
	if (power < 0) {
		power = 0;
	}

	return power << (2U * shift);
}

/*
 * Energy at the measured frequency as a Q15 fraction of @p energy, the total
 * the channel carried over the window.
 *
 * A sinusoid of amplitude A leaves |X| = A*N/2 and carries N*A^2/2 of energy,
 * so 2*|X|^2/N is the energy in the bin in the same units as the sum of
 * squares beside it, and the quotient is 1.0 for a pure tone. Clamped there:
 * the leakage of a tone's own mirror image can carry the reading a fraction of
 * a percent past unity, and a ratio above one would only ever be noise in a
 * number the application reads as a fraction.
 */
static int32_t tone_analyzer_ratio_q15(int64_t power, int64_t energy, uint32_t window)
{
	int64_t scaled;

	if (energy <= 0) {
		return 0;
	}

	scaled = ((2 * power) / (int64_t)window) * AUDIO_TONE_ANALYZER_UNITY_Q15;
	scaled /= energy;

	return (int32_t)MIN(scaled, (int64_t)AUDIO_TONE_ANALYZER_UNITY_Q15);
}

/*
 * What a one-sinusoid fit leaves behind, as a Q15 fraction of the channel's own
 * energy.
 *
 * Every single sinusoid satisfies x[n-1] + x[n+1] == 2*cos(w)*x[n] exactly, at
 * any frequency and any phase. Fitting the one constant c that minimises
 * sum (y - c*x)^2, with y[n] = x[n-1] + x[n+1], and reporting the residual
 * therefore asks "is this *a* tone" without asking which one - which is what
 * separates a wrong frequency from broadband noise, neither of which puts
 * anything in the bins that were asked about.
 *
 * The residual is sum y^2 - (sum x*y)^2 / sum x^2. Both terms are the size of
 * the energy and they cancel almost exactly for a real tone, so the three sums
 * are normalised together before the products - the ratio is what is being
 * asked for and a common shift leaves it alone.
 */
static int32_t tone_analyzer_residual_q15(int64_t xx, int64_t yy, int64_t xy)
{
	int64_t peak = MAX(MAX(xx, yy), tone_analyzer_abs64(xy));
	unsigned int shift = 0U;
	int64_t residual;

	if (xx <= 0) {
		/* Nothing was fitted: an empty or one-sample window. */
		return 0;
	}

	while ((peak >> shift) > TONE_ANALYZER_NORM_MAX) {
		shift++;
	}

	xx >>= shift;
	yy >>= shift;
	xy >>= shift;

	if (xx <= 0) {
		return 0;
	}

	residual = yy - (xy * xy) / xx;
	if (residual < 0) {
		residual = 0;
	}

	residual = (residual * AUDIO_TONE_ANALYZER_UNITY_Q15) / xx;

	return (int32_t)MIN(residual, (int64_t)INT32_MAX);
}

/* -------------------------------------------------------------------------
 * Folding samples into a window, and closing one
 * -------------------------------------------------------------------------
 */

static void tone_analyzer_reset_window(struct audio_tone_analyzer_state *state)
{
	memset(state->s1, 0, sizeof(state->s1));
	memset(state->s2, 0, sizeof(state->s2));
	memset(state->energy, 0, sizeof(state->energy));
	memset(state->fit_xx, 0, sizeof(state->fit_xx));
	memset(state->fit_yy, 0, sizeof(state->fit_yy));
	memset(state->fit_xy, 0, sizeof(state->fit_xy));
	memset(state->prev1, 0, sizeof(state->prev1));
	memset(state->prev2, 0, sizeof(state->prev2));
	memset(state->history, 0, sizeof(state->history));
	state->filled = 0U;
	state->channel_pos = 0U;
}

/* One sample of one channel into every accumulator that channel owns. */
static void tone_analyzer_sample(struct audio_tone_analyzer_state *state, uint8_t channel,
				 uint8_t tones, int32_t x)
{
	uint8_t tone;

	for (tone = 0U; tone < tones; tone++) {
		int64_t s1 = state->s1[channel][tone];
		int64_t s0 = (int64_t)x +
			     (((int64_t)state->coeff_q24[tone] * s1) >> TONE_ANALYZER_COEFF_SHIFT) -
			     state->s2[channel][tone];

		state->s2[channel][tone] = s1;
		state->s1[channel][tone] = s0;
	}

	state->energy[channel] += (int64_t)x * x;

	/* The fit needs a sample either side of the one it predicts, so it runs
	 * one sample behind and covers the window's interior. Its own sum of
	 * squares is kept separately rather than reusing the energy above,
	 * because a residual normalised by a slightly different sum would not be
	 * zero for a perfect tone.
	 */
	if (state->history[channel] >= 2U) {
		int64_t centre = state->prev1[channel];
		int64_t neighbours = (int64_t)state->prev2[channel] + x;

		state->fit_xx[channel] += centre * centre;
		state->fit_yy[channel] += neighbours * neighbours;
		state->fit_xy[channel] += centre * neighbours;
	} else {
		state->history[channel]++;
	}

	state->prev2[channel] = state->prev1[channel];
	state->prev1[channel] = x;
}

/*
 * The verdict for one closed window.
 *
 * The order of the tests is the contract:
 *
 *  - A pass first, so an analyzer configured with the same frequency on both
 *    channels - where a swap is not observable at all - reports what it can
 *    see rather than an ambiguity.
 *  - Then any silent channel, because a channel carrying nothing is a
 *    different fault from a channel carrying the wrong thing, and it is the
 *    one to fix first.
 *  - Then the swap: every channel carrying its neighbour's tone. v1 runs 1 or
 *    2 channels (spec §5.2), so the rotation is the only permutation there is.
 *  - Then noise before a wrong frequency, because "there is no tone in this"
 *    is the stronger statement of the two.
 */
static enum audio_tone_analyzer_verdict
tone_analyzer_decide(const struct audio_tone_analyzer_result *window, uint8_t channels)
{
	bool pass = true;
	bool swapped = channels > 1U;
	bool any_silent = false;
	bool any_noise = false;
	uint8_t channel;

	for (channel = 0U; channel < channels; channel++) {
		const struct audio_tone_analyzer_channel_result *ch = &window->channel[channel];

		if (ch->in_band_q15[channel] < AUDIO_TONE_ANALYZER_PASS_Q15) {
			pass = false;
		}

		if (channels > 1U &&
		    ch->in_band_q15[(channel + 1U) % channels] < AUDIO_TONE_ANALYZER_PASS_Q15) {
			swapped = false;
		}

		if (ch->silent) {
			any_silent = true;
		} else if (!ch->tonal) {
			any_noise = true;
		}
	}

	if (pass) {
		return AUDIO_TONE_ANALYZER_VERDICT_PASS;
	}

	if (any_silent) {
		return AUDIO_TONE_ANALYZER_VERDICT_SILENT;
	}

	if (swapped) {
		return AUDIO_TONE_ANALYZER_VERDICT_SWAPPED;
	}

	if (any_noise) {
		return AUDIO_TONE_ANALYZER_VERDICT_NOISE;
	}

	return AUDIO_TONE_ANALYZER_VERDICT_WRONG_FREQ;
}

/* Close the window: measure, publish, and start the next one empty. */
static void tone_analyzer_finish(struct audio_tone_analyzer_state *state, uint8_t channels,
				 uint8_t tones)
{
	struct audio_tone_analyzer_result window = {
		.windows = state->result.windows + 1U,
		.window_samples = state->window_samples,
		.channels = channels,
		.tones = tones,
	};
	enum audio_tone_analyzer_verdict previous = state->result.verdict;
	k_spinlock_key_t key;
	uint8_t channel;
	uint8_t tone;

	for (channel = 0U; channel < channels; channel++) {
		struct audio_tone_analyzer_channel_result *ch = &window.channel[channel];
		int64_t energy = state->energy[channel];

		ch->rms = (int32_t)tone_analyzer_isqrt(
			(uint64_t)(energy / (int64_t)state->window_samples));
		ch->silent = ch->rms < AUDIO_TONE_ANALYZER_SILENCE_RMS;
		ch->residual_q15 = tone_analyzer_residual_q15(
			state->fit_xx[channel], state->fit_yy[channel], state->fit_xy[channel]);
		ch->tonal = ch->residual_q15 <= AUDIO_TONE_ANALYZER_TONAL_Q15;
		ch->strongest = -1;

		for (tone = 0U; tone < tones; tone++) {
			int64_t power = tone_analyzer_power(state->s1[channel][tone],
							    state->s2[channel][tone],
							    state->coeff_q24[tone]);

			ch->in_band_q15[tone] =
				tone_analyzer_ratio_q15(power, energy, state->window_samples);

			if (ch->in_band_q15[tone] > 0 &&
			    (ch->strongest < 0 ||
			     ch->in_band_q15[tone] > ch->in_band_q15[ch->strongest])) {
				ch->strongest = (int8_t)tone;
			}
		}
	}

	window.verdict = tone_analyzer_decide(&window, channels);

	/* The only state this node publishes outside the pipeline thread, and
	 * the only place it is written (spec §3.3). A whole window at a time, so
	 * a reader never sees one channel of this window beside one of the last.
	 */
	key = k_spin_lock(&state->lock);
	state->result = window;
	k_spin_unlock(&state->lock, key);

	/* Logged on a change only: a verdict per window is one every few
	 * milliseconds, and a log that scrolls is a log nobody reads. Nothing
	 * depends on this line - the result is a value.
	 */
	if (window.verdict != previous) {
		LOG_INF("window %u: %s", window.windows,
			tone_analyzer_verdict_name[window.verdict]);
	}

	tone_analyzer_reset_window(state);
}

/* -------------------------------------------------------------------------
 * Node operations
 * -------------------------------------------------------------------------
 */

static int tone_analyzer_open(struct audio_node *node)
{
	const struct audio_stream_config *fmt;
	struct audio_tone_analyzer_state *state;
	k_spinlock_key_t key;
	uint8_t tone;

	if (!node) {
		return -EINVAL;
	}

	state = (struct audio_tone_analyzer_state *)node->state;
	if (!state) {
		return -EINVAL;
	}

	/* Reopening starts a fresh measurement: no window in progress and no
	 * verdict from the last run. Done before anything can fail, so a refused
	 * open() leaves nothing behind that a getter could mistake for a result.
	 */
	state->is_open = false;
	tone_analyzer_reset_window(state);
	memset(state->coeff_q24, 0, sizeof(state->coeff_q24));

	key = k_spin_lock(&state->lock);
	memset(&state->result, 0, sizeof(state->result));
	k_spin_unlock(&state->lock, key);

	/* The rate and the channel count come from the binding and nowhere else
	 * (spec §5.2). An analyzer inventing 48 kHz would measure at a frequency
	 * nobody asked about and report the miss as a fault in the link.
	 */
	fmt = node->pipeline_format;
	if (!fmt) {
		LOG_ERR("no pipeline format installed");
		return -EINVAL;
	}

	if (fmt->sample_rate_hz == 0U) {
		LOG_ERR("the bound format carries no sample rate");
		return -EINVAL;
	}

	if (state->window_samples < AUDIO_TONE_ANALYZER_MIN_WINDOW ||
	    state->window_samples > AUDIO_TONE_ANALYZER_MAX_WINDOW) {
		LOG_ERR("a window of %u samples is outside %u..%u", state->window_samples,
			AUDIO_TONE_ANALYZER_MIN_WINDOW, AUDIO_TONE_ANALYZER_MAX_WINDOW);
		return -EINVAL;
	}

	/* One expected frequency per channel, exactly, like the generator at the
	 * other end (spec §5.2: nodes validate, they never adapt). Fewer would
	 * leave a channel unmeasured; more would mean measuring a channel that
	 * does not exist.
	 */
	if (state->tone_count != fmt->channels) {
		LOG_ERR("%u expected tones do not match the pipeline's %u channels",
			state->tone_count, fmt->channels);
		return -ENOTSUP;
	}

	for (tone = 0U; tone < state->tone_count; tone++) {
		uint64_t freq = state->freq_hz[tone];
		uint64_t rate = fmt->sample_rate_hz;
		uint64_t window = state->window_samples;
		uint32_t step;

		/* A window of N samples resolves fs/N, so a frequency closer
		 * than that to DC or to Nyquist cannot be told from the one on
		 * the other side of it - and it is also where the recurrence
		 * state grows without a bound this file can assert. Both halves
		 * of the test are the same statement, which is why there is one
		 * rule rather than two.
		 */
		if (freq * window < rate) {
			LOG_ERR("tone %u: %u Hz is inside the first bin of a %u sample window",
				tone, state->freq_hz[tone], state->window_samples);
			return -EINVAL;
		}

		if (2U * freq * window + 2U * rate > rate * window) {
			LOG_ERR("tone %u: %u Hz is inside the last bin below the %u Hz Nyquist "
				"limit",
				tone, state->freq_hz[tone], fmt->sample_rate_hz / 2U);
			return -EINVAL;
		}

		step = tone_analyzer_phase_step(state->freq_hz[tone], fmt->sample_rate_hz);

		/* 2*cos(w) in Q24. cos is sin a quarter turn along, which is the
		 * whole reason the phase is an angle rather than a frequency.
		 */
		state->coeff_q24[tone] =
			(int32_t)((tone_analyzer_sin_q30(step + (UINT32_C(1) << 30)) + 16) >> 5);
	}

	key = k_spin_lock(&state->lock);
	state->result.window_samples = state->window_samples;
	state->result.channels = fmt->channels;
	state->result.tones = state->tone_count;
	k_spin_unlock(&state->lock, key);

	state->is_open = true;

	LOG_INF("%u tone(s), %u ch at %u Hz, %u sample window (%u Hz per bin)", state->tone_count,
		fmt->channels, fmt->sample_rate_hz, state->window_samples,
		fmt->sample_rate_hz / state->window_samples);

	return 0;
}

static int tone_analyzer_process(struct audio_node *node, struct audio_buffer_view *buf,
				 size_t *out_size)
{
	struct audio_tone_analyzer_state *state;
	uint8_t channels;
	uint8_t tones;
	size_t i;
	int ret;

	if (!node || !buf || !buf->data || !out_size) {
		return -EINVAL;
	}

	state = (struct audio_tone_analyzer_state *)node->state;
	if (!state) {
		return -EINVAL;
	}

	*out_size = 0;

	/* process() before open(), or after close(). The binding is checked with
	 * it because the two are only meaningful together: the pipeline installs
	 * the format before open() and leaves it there until close() (spec §4.1).
	 */
	if (!state->is_open || !node->pipeline_format) {
		LOG_ERR("process() on a closed analyzer");
		return -EBADF;
	}

	ret = audio_node_pull(node, buf, out_size);
	if (ret < 0) {
		return ret;
	}

	if (*out_size == 0U) {
		/* End of stream (manifest §7). Whatever the window had collected
		 * is dropped rather than measured short: a window that never
		 * filled reads low, which would turn a clean end of stream into
		 * a failed verdict.
		 */
		tone_analyzer_reset_window(state);
		return 0;
	}

	/* Read per frame rather than copied at open(): the channel count belongs
	 * to the pipeline (spec §5.2), and a second copy on the node is the one
	 * thing that could ever disagree with it.
	 */
	channels = node->pipeline_format->channels;
	tones = state->tone_count;

	for (i = 0U; i < *out_size; i++) {
		uint8_t channel = state->channel_pos;

		/* spec §5.3's convention, the file writer's narrowing verbatim.
		 * Both sides of every ratio this node reports are narrowed
		 * alike, so nothing a verdict depends on is in the bits below.
		 */
		tone_analyzer_sample(state, channel, tones,
				     buf->data[i] >> AUDIO_TONE_ANALYZER_INPUT_SHIFT);

		/* The interleave position is carried across frames, so a frame
		 * that ends mid sample set - which nothing forbids - does not
		 * transpose every channel of the frames that follow.
		 */
		channel++;
		if (channel < channels) {
			state->channel_pos = channel;
			continue;
		}

		state->channel_pos = 0U;
		state->filled++;

		if (state->filled >= state->window_samples) {
			tone_analyzer_finish(state, channels, tones);
		}
	}

	return 0;
}

static int tone_analyzer_close(struct audio_node *node)
{
	struct audio_tone_analyzer_state *state;

	if (!node) {
		return -EINVAL;
	}

	state = (struct audio_tone_analyzer_state *)node->state;
	if (!state) {
		return -EINVAL;
	}

	/* The last verdict is left where it is: it is what the run was for, and
	 * an application reads it after stopping the pipeline. open() is what
	 * clears it, so a rerun never reports the previous run's result.
	 */
	state->is_open = false;

	return 0;
}

int audio_tone_analyzer_get_result(const struct audio_node *node,
				   struct audio_tone_analyzer_result *result)
{
	struct audio_tone_analyzer_state *state;
	k_spinlock_key_t key;

	if (!node || !result || node->ops != &tone_analyzer_node_ops) {
		return -EINVAL;
	}

	state = (struct audio_tone_analyzer_state *)node->state;
	if (!state) {
		return -EINVAL;
	}

	key = k_spin_lock(&state->lock);
	*result = state->result;
	k_spin_unlock(&state->lock, key);

	return 0;
}

const struct audio_node_ops tone_analyzer_node_ops = {
	.open = tone_analyzer_open,
	.process = tone_analyzer_process,
	.close = tone_analyzer_close,
};
