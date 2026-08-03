/*
 * Tone generator source node.
 *
 * open() turns the configured frequencies into phase increments using the rate
 * the pipeline bound; process() walks one phase accumulator per channel through
 * a sine table and writes the result MSB-aligned into the canonical S32_LE
 * container; close() parks the node (manifest §2/§4/§7, spec §4.2/§5.2/§5.3).
 *
 * It is the stimulus for the loopback bring-up, so the two things it has to get
 * right are frequency and the passage of time:
 *
 *  - The phase is a 32 bit accumulator and the increment is a rounded constant,
 *    so the only frequency error is the rounding of that one constant - half a
 *    phase LSB per sample, about 5.6 microhertz at 48 kHz - and it never turns
 *    into drift. A two-pole resonator would be cheaper per sample but its
 *    amplitude and phase wander, which in a loopback that runs for minutes
 *    shows up as a slow, puzzling failure rather than an obvious one.
 *  - The accumulator carries across frames untouched, so a frame boundary is
 *    not a seam: nothing here restarts a period or rounds a sample count.
 *
 * Everything is integer arithmetic against a static table. Floating point is
 * deliberately absent: a source pulling in sinf() would drag an FPU dependency
 * onto every target that ever defines one of these nodes.
 *
 * All state lives in the per-instance ::audio_tone_gen_state allocated by
 * AUDIO_TONE_GEN_NODE_DEFINE(), so two generators share nothing and two
 * identically configured ones produce the same stream.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/audio/audio_node.h>
#include <zephyr/audio/audio_nodes.h>

LOG_MODULE_REGISTER(audio_tone_gen, LOG_LEVEL_INF);

/*
 * Quarter of a period, sampled at 256 points, as Q15 amplitudes.
 *
 * A quarter is all a sine needs: the other three follow by mirroring the index
 * and the sign, which is why the table stops at 256 and still describes a
 * 1024 point period (~ -60 dBc from the phase quantisation, well under what any
 * analyzer downstream cares about). Storing the whole period instead would cost
 * 2 KiB of flash to save two comparisons.
 *
 * The 257th entry is the endpoint sin(pi/2): the mirror reads index
 * TONE_GEN_QUARTER_POINTS, so the peak has to exist in the table rather than
 * being one step past its end.
 *
 * Peak 32767 rather than 32768 keeps the waveform symmetric around zero, so
 * full-scale output stays inside the container in both directions.
 */
#define TONE_GEN_QUARTER_POINTS 256U

/* Phase bits below the two quadrant bits that select the table entry. */
#define TONE_GEN_INDEX_BITS 8U

static const int16_t tone_gen_quarter_q15[TONE_GEN_QUARTER_POINTS + 1U] = {
	0,     201,   402,   603,   804,   1005,  1206,  1407,  1608,  1809,  2009,  2210,  2410,
	2611,  2811,  3012,  3212,  3412,  3612,  3811,  4011,  4210,  4410,  4609,  4808,  5007,
	5205,  5404,  5602,  5800,  5998,  6195,  6393,  6590,  6786,  6983,  7179,  7375,  7571,
	7767,  7962,  8157,  8351,  8545,  8739,  8933,  9126,  9319,  9512,  9704,  9896,  10087,
	10278, 10469, 10659, 10849, 11039, 11228, 11417, 11605, 11793, 11980, 12167, 12353, 12539,
	12725, 12910, 13094, 13279, 13462, 13645, 13828, 14010, 14191, 14372, 14553, 14732, 14912,
	15090, 15269, 15446, 15623, 15800, 15976, 16151, 16325, 16499, 16673, 16846, 17018, 17189,
	17360, 17530, 17700, 17869, 18037, 18204, 18371, 18537, 18703, 18868, 19032, 19195, 19357,
	19519, 19680, 19841, 20000, 20159, 20317, 20475, 20631, 20787, 20942, 21096, 21250, 21403,
	21554, 21705, 21856, 22005, 22154, 22301, 22448, 22594, 22739, 22884, 23027, 23170, 23311,
	23452, 23592, 23731, 23870, 24007, 24143, 24279, 24413, 24547, 24680, 24811, 24942, 25072,
	25201, 25329, 25456, 25582, 25708, 25832, 25955, 26077, 26198, 26319, 26438, 26556, 26674,
	26790, 26905, 27019, 27133, 27245, 27356, 27466, 27575, 27683, 27790, 27896, 28001, 28105,
	28208, 28310, 28411, 28510, 28609, 28706, 28803, 28898, 28992, 29085, 29177, 29268, 29358,
	29447, 29534, 29621, 29706, 29791, 29874, 29956, 30037, 30117, 30195, 30273, 30349, 30424,
	30498, 30571, 30643, 30714, 30783, 30852, 30919, 30985, 31050, 31113, 31176, 31237, 31297,
	31356, 31414, 31470, 31526, 31580, 31633, 31685, 31736, 31785, 31833, 31880, 31926, 31971,
	32014, 32057, 32098, 32137, 32176, 32213, 32250, 32285, 32318, 32351, 32382, 32412, 32441,
	32469, 32495, 32521, 32545, 32567, 32589, 32609, 32628, 32646, 32663, 32678, 32692, 32705,
	32717, 32728, 32737, 32745, 32752, 32757, 32761, 32765, 32766, 32767,
};

/*
 * Sine of @p phase as a Q15 amplitude, @p phase being a fraction of a full turn
 * in 32 bit fixed point (0x40000000 is a quarter turn).
 *
 * The top two bits pick the quadrant and the next eight the entry inside it;
 * the bits below that are the sub-step position the lookup discards. They are
 * not lost, though - they stay in the accumulator and carry into the next
 * sample, which is what keeps the *long term* frequency exact even though a
 * single sample is quantised.
 */
static int32_t tone_gen_sine_q15(uint32_t phase)
{
	uint32_t index = (phase >> (30U - TONE_GEN_INDEX_BITS)) & (TONE_GEN_QUARTER_POINTS - 1U);

	switch (phase >> 30U) {
	case 0U:
		return tone_gen_quarter_q15[index];
	case 1U:
		return tone_gen_quarter_q15[TONE_GEN_QUARTER_POINTS - index];
	case 2U:
		return -tone_gen_quarter_q15[index];
	default:
		return -tone_gen_quarter_q15[TONE_GEN_QUARTER_POINTS - index];
	}
}

/*
 * One container sample for tone @p tone at its current phase.
 *
 * The Q15 table value is scaled by the configured amplitude and then shifted up
 * by 16, which is the file reader's s32 = s16 << 16 convention (spec §5.3): the
 * generator and the file source therefore hand the pipeline the same kind of
 * value, and a sink narrowing back to 16 bit reproduces the tone exactly.
 *
 * Nothing here can overflow or need clipping. The table peaks at 32767 and the
 * amplitude at 32768, so the product stays below 2^31 before the shift and the
 * scaled value inside [-32767, 32767] after it - the container's extremes are
 * approached, never crossed. The shift up runs in the unsigned domain because
 * left-shifting a negative signed value is not defined by the C standard, while
 * the two's complement result is exactly what the convention asks for.
 */
static int32_t tone_gen_sample(const struct audio_tone_gen_state *state, uint8_t tone)
{
	int32_t value = (tone_gen_sine_q15(state->phase[tone]) * state->amplitude_q15) >> 15;

	return (int32_t)((uint32_t)value << 16);
}

/*
 * Phase increment per sample for @p freq_hz at @p rate_hz: round(f * 2^32 / fs).
 *
 * Rounded rather than truncated so the error is halved and, more importantly,
 * unbiased - a truncated increment is always low, which is a slow flat over the
 * length of a run. The accumulator wraps on exactly one turn, so this constant
 * is the only place a frequency error can enter.
 *
 * The intermediate needs 64 bits; the result does not, because open() has
 * already refused anything at or above Nyquist and f < fs/2 puts the quotient
 * below 2^31.
 */
static uint32_t tone_gen_phase_step(uint32_t freq_hz, uint32_t rate_hz)
{
	uint64_t turns = ((uint64_t)freq_hz << 32) + (uint64_t)(rate_hz / 2U);

	return (uint32_t)(turns / rate_hz);
}

static int tone_gen_open(struct audio_node *node)
{
	const struct audio_stream_config *fmt;
	struct audio_tone_gen_state *state;
	uint8_t tone;

	if (!node) {
		return -EINVAL;
	}

	state = (struct audio_tone_gen_state *)node->state;
	if (!state) {
		return -EINVAL;
	}

	/* Reopening starts the stream over: phase 0 and nothing produced yet.
	 * Done before anything can fail, so a refused open() leaves the node
	 * closed rather than half configured, and two instances opened with the
	 * same configuration are indistinguishable from the first sample on.
	 */
	state->is_open = false;
	state->produced = 0U;
	memset(state->phase, 0, sizeof(state->phase));
	memset(state->phase_step, 0, sizeof(state->phase_step));

	/* The rate and the channel count come from the binding and nowhere else
	 * (spec §5.2), so the absence of one is a caller error rather than a
	 * case to paper over with a default: a generator inventing 48 kHz would
	 * put a tone of the wrong frequency into a pipeline that says otherwise.
	 */
	fmt = node->pipeline_format;
	if (!fmt) {
		LOG_ERR("no pipeline format installed");
		return -EINVAL;
	}

	if (state->amplitude_q15 < 0 || state->amplitude_q15 > AUDIO_TONE_GEN_FULL_SCALE_Q15) {
		LOG_ERR("amplitude %d is outside 0..%d", state->amplitude_q15,
			AUDIO_TONE_GEN_FULL_SCALE_Q15);
		return -EINVAL;
	}

	/* One tone per channel, exactly (spec §5.2: nodes validate, they never
	 * adapt). More tones than channels would mean dropping one, and the
	 * dropped one is what tells a swapped pair of wires apart downstream;
	 * fewer would leave a channel with no frequency at all. The definition
	 * names at most AUDIO_TONE_GEN_MAX_TONES, so this also rules out a
	 * channel count process() could not divide a frame by - zero included.
	 */
	if (state->tone_count != fmt->channels) {
		LOG_ERR("%u tones do not match the pipeline's %u channels", state->tone_count,
			fmt->channels);
		return -ENOTSUP;
	}

	for (tone = 0U; tone < state->tone_count; tone++) {
		/* Above Nyquist the tone aliases down to some other frequency,
		 * which is worse than no tone at all: the analyzer downstream
		 * would report a clean tone nobody asked for. The same test
		 * rejects a zero sample rate, which the increment below divides
		 * by.
		 */
		if (state->freq_hz[tone] == 0U ||
		    (uint64_t)state->freq_hz[tone] * 2U >= (uint64_t)fmt->sample_rate_hz) {
			LOG_ERR("tone %u: %u Hz is not inside 1..%u Hz", tone, state->freq_hz[tone],
				fmt->sample_rate_hz / 2U);
			return -EINVAL;
		}

		state->phase_step[tone] =
			tone_gen_phase_step(state->freq_hz[tone], fmt->sample_rate_hz);
	}

	/* The duration counts total interleaved samples, like every other sample
	 * count in the subsystem (manifest §5), so a value that is not a whole
	 * number of sample sets would end the stream mid frame and shift every
	 * channel of whatever follows. It is a configuration mistake and it is
	 * reported as one.
	 */
	if (state->duration_samples % fmt->channels != 0U) {
		LOG_ERR("a duration of %u samples is not a whole number of %u channel sample sets",
			state->duration_samples, fmt->channels);
		return -EINVAL;
	}

	state->is_open = true;

	LOG_INF("%u tone(s) at %u Hz, %u ch, amplitude %d/%d, %u samples", state->tone_count,
		fmt->sample_rate_hz, fmt->channels, state->amplitude_q15,
		AUDIO_TONE_GEN_FULL_SCALE_Q15, state->duration_samples);

	return 0;
}

static int tone_gen_process(struct audio_node *node, struct audio_buffer_view *buf,
			    size_t *out_size)
{
	struct audio_tone_gen_state *state;
	size_t channels;
	size_t samples;
	size_t i;
	uint8_t tone;

	if (!node || !buf || !buf->data || !out_size) {
		return -EINVAL;
	}

	state = (struct audio_tone_gen_state *)node->state;
	if (!state) {
		return -EINVAL;
	}

	*out_size = 0;

	/* process() before open(), or after close(). The binding is checked with
	 * it because the two are only meaningful together: the pipeline installs
	 * the format before open() and leaves it there until close() (spec §4.1).
	 */
	if (!state->is_open || !node->pipeline_format) {
		LOG_ERR("process() on a closed generator");
		return -EBADF;
	}

	/* Read per frame rather than copied at open(): the channel count belongs
	 * to the pipeline (spec §5.2), and a second copy on the node is the one
	 * thing that could ever disagree with it.
	 */
	channels = node->pipeline_format->channels;

	/* A buffer that cannot hold a single interleaved sample set is a caller
	 * error, not end of stream - reporting EOF here would cut the stimulus
	 * short and look like the source running out.
	 */
	if (buf->capacity < channels) {
		LOG_ERR("buffer of %zu samples is too small for %zu channels", buf->capacity,
			channels);
		return -EINVAL;
	}

	/* A sample set must never straddle two frames, or every following frame
	 * would arrive with its channels swapped.
	 */
	samples = ROUND_DOWN(buf->capacity, channels);

	if (state->duration_samples != 0U) {
		size_t left = (size_t)state->duration_samples - (size_t)state->produced;

		if (left == 0U) {
			/* The configured length is done: EOF (manifest §7). A
			 * duration of zero never gets here, which is how it
			 * runs indefinitely.
			 */
			return 0;
		}

		/* Both the duration and everything produced so far are whole
		 * sample sets, so the remainder is one too and this cannot
		 * truncate a set.
		 */
		samples = MIN(samples, left);
	}

	for (i = 0U; i < samples; i += channels) {
		for (tone = 0U; tone < channels; tone++) {
			buf->data[i + tone] = tone_gen_sample(state, tone);
			state->phase[tone] += state->phase_step[tone];
		}
	}

	state->produced += (uint32_t)samples;
	*out_size = samples;

	return 0;
}

static int tone_gen_close(struct audio_node *node)
{
	struct audio_tone_gen_state *state;

	if (!node) {
		return -EINVAL;
	}

	state = (struct audio_tone_gen_state *)node->state;
	if (!state) {
		return -EINVAL;
	}

	/* The phase is left where it stopped: it is observable after a run and
	 * open() is what resets it, so a reopened generator starts a fresh
	 * stream rather than continuing a stale one.
	 */
	state->is_open = false;

	return 0;
}

const struct audio_node_ops tone_gen_node_ops = {
	.open = tone_gen_open,
	.process = tone_gen_process,
	.close = tone_gen_close,
};
