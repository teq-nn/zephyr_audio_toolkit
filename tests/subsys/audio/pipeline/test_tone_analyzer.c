/*
 * Tone analyzer sink node: offset invariance, the four cases it has to tell
 * apart, the channel swap, the accumulator bound and the round trip against
 * the generator (issue #34, manifest §4/§7, spec §4.4/§5.2/§5.3).
 *
 * The case that matters most here is the one an oracle cannot be trusted
 * without: a deliberately mismatched input has to *fail*. Half of this file is
 * therefore about inputs that must not pass - silence, a wrong frequency,
 * broadband noise and a swapped pair of channels - and only the other half
 * about the tone that must.
 *
 * The stimulus is the tone generator next door rather than a table of samples,
 * so the two nodes are exercised against each other and neither can drift into
 * its own idea of what 1 kHz means. A sample offset is produced by pulling
 * samples out of the generator and discarding them before the analyzer is
 * opened, which is exactly the shape of the fault the node exists to be immune
 * to: a link whose latency nobody has measured.
 *
 * The "no floating point, no libm" guardrail is a property of the built object
 * rather than something a case can assert. Re-run it against the node's own
 * object under the Twister output directory:
 *
 *   find <outdir> -name tone_analyzer_node.c.obj -exec nm --undefined-only {} + |
 *           grep -Ei 'sin|cos|sqrt|pow|__aeabi_[fd]|__float|__fix'
 *
 * which must print nothing.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <zephyr/audio/audio_node.h>
#include <zephyr/audio/audio_nodes.h>
#include <zephyr/audio/audio_pipeline.h>
#include <zephyr/audio/audio_pipeline_events.h>

#include "fake_nodes.h"

#define ANA_RATE_HZ  48000U
#define ANA_LEFT_HZ  1000U
#define ANA_RIGHT_HZ 3000U

/*
 * 960 samples at 48 kHz is a 50 Hz bin, which puts 1 kHz on bin 20 and 3 kHz on
 * bin 60 - both whole bins, where the measurement is exactly offset invariant
 * rather than nearly so (see AUDIO_TONE_ANALYZER_NODE_DEFINE()). 1024 samples
 * puts neither on a bin centre, which is the other half of the same case: the
 * invariance has to survive that too, only with a ripple instead of without.
 */
#define ANA_WINDOW      960U
#define ANA_ODD_WINDOW  1024U

/* Frequencies nobody configured, far enough out to be unambiguous. */
#define ANA_WRONG_LEFT_HZ  5000U
#define ANA_WRONG_RIGHT_HZ 7000U

/*
 * The corner the accumulator bound is proved at: the longest supported window,
 * and the lowest frequency it accepts (one bin from DC, i.e. 48000/4096 =
 * 11.72 Hz, so 12 Hz), driven at full scale. That configuration produces the
 * largest Goertzel state a supported configuration can, because the state is
 * bounded by N * max|x| / |sin w| and every factor of it is at its worst.
 */
#define ANA_LONG_WINDOW AUDIO_TONE_ANALYZER_MAX_WINDOW
#define ANA_LOW_HZ      12U

/* Frequencies open() has to refuse: inside the first and the last bin. */
#define ANA_DC_HZ      40U
#define ANA_NYQUIST_HZ 23980U

/* Windows measured per case; two are enough to show a second one starts clean. */
#define ANA_WINDOWS 2U

/* Offsets the invariance cases sweep, and the step between them. */
#define ANA_OFFSET_STEP 37U

/* A tone at a hundredth of full scale: still a tone, 40 dB below a wrong one. */
#define ANA_QUIET_Q15 (AUDIO_TONE_GEN_FULL_SCALE_Q15 / 100)

/* The pipeline case runs whole windows, so no partial one is dropped at EOF. */
#define ANA_CHAIN_FRAME_SAMPLES 64U
#define ANA_CHAIN_WINDOWS       2U
#define ANA_CHAIN_SAMPLES       (ANA_CHAIN_WINDOWS * ANA_WINDOW * 2U)

/* -------------------------------------------------------------------------
 * A source for the two cases the generator cannot produce
 * ----------------------------------------------------------------------
 */

struct ana_fake {
	/** True for broadband noise, false for digital silence. */
	bool noisy;
	/** Linear congruential state; reseeded by open() so runs repeat. */
	uint32_t rng;
};

static int ana_fake_open(struct audio_node *node)
{
	struct ana_fake *fake = node->state;

	fake->rng = 0x12345678U;

	return 0;
}

static int ana_fake_process(struct audio_node *node, struct audio_buffer_view *buf,
			    size_t *out_size)
{
	struct ana_fake *fake = node->state;
	size_t i;

	for (i = 0; i < buf->capacity; i++) {
		if (!fake->noisy) {
			buf->data[i] = 0;
			continue;
		}

		/* The top 16 bits of the generator, MSB aligned like every
		 * other 16 bit payload in the container (spec §5.3): full-scale
		 * broadband noise with no floating point anywhere.
		 */
		fake->rng = fake->rng * 1664525U + 1013904223U;
		buf->data[i] = (int32_t)(fake->rng & 0xffff0000U);
	}

	*out_size = buf->capacity;

	return 0;
}

static int ana_fake_close(struct audio_node *node)
{
	ARG_UNUSED(node);

	return 0;
}

static const struct audio_node_ops ana_fake_ops = {
	.open = ana_fake_open,
	.process = ana_fake_process,
	.close = ana_fake_close,
};

#define ANA_FAKE_DEFINE(_name, _noisy)                                                             \
	static struct ana_fake _name##_state = {                                                   \
		.noisy = (_noisy),                                                                 \
	};                                                                                         \
	AUDIO_NODE_DEFINE(_name, AUDIO_NODE_ROLE_SOURCE, &ana_fake_ops, NULL, &_name##_state)

ANA_FAKE_DEFINE(ana_silence_src, false);
ANA_FAKE_DEFINE(ana_noise_src, true);

/* -------------------------------------------------------------------------
 * The chains under test, every one built through the public macros
 * ----------------------------------------------------------------------
 */

AUDIO_TONE_GEN_NODE_DEFINE(ana_gen, AUDIO_TONE_GEN_FULL_SCALE_Q15, 0, ANA_LEFT_HZ, ANA_RIGHT_HZ);
AUDIO_TONE_ANALYZER_NODE_DEFINE(ana_tone, &ana_gen, ANA_WINDOW, ANA_LEFT_HZ, ANA_RIGHT_HZ);

/* Same stimulus, a window that puts neither tone on a bin centre. */
AUDIO_TONE_GEN_NODE_DEFINE(ana_gen_odd, AUDIO_TONE_GEN_FULL_SCALE_Q15, 0, ANA_LEFT_HZ,
			   ANA_RIGHT_HZ);
AUDIO_TONE_ANALYZER_NODE_DEFINE(ana_odd, &ana_gen_odd, ANA_ODD_WINDOW, ANA_LEFT_HZ, ANA_RIGHT_HZ);

/* Two instances of one configuration, which must share nothing. */
AUDIO_TONE_GEN_NODE_DEFINE(ana_gen_twin, AUDIO_TONE_GEN_FULL_SCALE_Q15, 0, ANA_LEFT_HZ,
			   ANA_RIGHT_HZ);
AUDIO_TONE_ANALYZER_NODE_DEFINE(ana_twin, &ana_gen_twin, ANA_WINDOW, ANA_LEFT_HZ, ANA_RIGHT_HZ);

/* A loud tone at frequencies nobody asked for. */
AUDIO_TONE_GEN_NODE_DEFINE(ana_gen_wrong, AUDIO_TONE_GEN_FULL_SCALE_Q15, 0, ANA_WRONG_LEFT_HZ,
			   ANA_WRONG_RIGHT_HZ);
AUDIO_TONE_ANALYZER_NODE_DEFINE(ana_wrong, &ana_gen_wrong, ANA_WINDOW, ANA_LEFT_HZ, ANA_RIGHT_HZ);

/* The right tones on the wrong channels. */
AUDIO_TONE_GEN_NODE_DEFINE(ana_gen_swapped, AUDIO_TONE_GEN_FULL_SCALE_Q15, 0, ANA_RIGHT_HZ,
			   ANA_LEFT_HZ);
AUDIO_TONE_ANALYZER_NODE_DEFINE(ana_swapped, &ana_gen_swapped, ANA_WINDOW, ANA_LEFT_HZ,
				ANA_RIGHT_HZ);

/* The right tones, 40 dB down. */
AUDIO_TONE_GEN_NODE_DEFINE(ana_gen_quiet, ANA_QUIET_Q15, 0, ANA_LEFT_HZ, ANA_RIGHT_HZ);
AUDIO_TONE_ANALYZER_NODE_DEFINE(ana_quiet, &ana_gen_quiet, ANA_WINDOW, ANA_LEFT_HZ, ANA_RIGHT_HZ);

/* Silence and noise. */
AUDIO_TONE_ANALYZER_NODE_DEFINE(ana_silent, &ana_silence_src, ANA_WINDOW, ANA_LEFT_HZ,
				ANA_RIGHT_HZ);
AUDIO_TONE_ANALYZER_NODE_DEFINE(ana_noise, &ana_noise_src, ANA_WINDOW, ANA_LEFT_HZ, ANA_RIGHT_HZ);

/* The accumulator bound: full scale, longest window, lowest frequency. */
AUDIO_TONE_GEN_NODE_DEFINE(ana_gen_low, AUDIO_TONE_GEN_FULL_SCALE_Q15, 0, ANA_LOW_HZ);
AUDIO_TONE_ANALYZER_NODE_DEFINE(ana_low, &ana_gen_low, ANA_LONG_WINDOW, ANA_LOW_HZ);

/*
 * The same corner 40 dB down: the identical tone, window and frequency, only
 * with the accumulators two orders of magnitude clear of anything they could
 * wrap against. It is the control the full-scale reading above is compared to,
 * because the reading on its own has no absolute value to be held to.
 */
AUDIO_TONE_GEN_NODE_DEFINE(ana_gen_low_quiet, ANA_QUIET_Q15, 0, ANA_LOW_HZ);
AUDIO_TONE_ANALYZER_NODE_DEFINE(ana_low_quiet, &ana_gen_low_quiet, ANA_LONG_WINDOW, ANA_LOW_HZ);

/*
 * One captured window, replayed through the shared scripted source. The
 * generator only ever hands out whole interleaved sample sets, so replaying its
 * output in odd sized pieces is the only way to hand the analyzer a frame that
 * ends in the middle of one - and to end a stream halfway through a window.
 */
static int32_t ana_replay[ANA_WINDOW * 2U];
AUDIO_FAKE_SOURCE_DEFINE(ana_replay_src);
AUDIO_TONE_ANALYZER_NODE_DEFINE(ana_replay_sink, &ana_replay_src, ANA_WINDOW, ANA_LEFT_HZ,
				ANA_RIGHT_HZ);

/* Configurations open() has to refuse. */
AUDIO_TONE_ANALYZER_NODE_DEFINE(ana_dc, &ana_silence_src, ANA_WINDOW, ANA_DC_HZ);
AUDIO_TONE_ANALYZER_NODE_DEFINE(ana_nyquist, &ana_silence_src, ANA_WINDOW, ANA_NYQUIST_HZ);
AUDIO_TONE_ANALYZER_NODE_DEFINE(ana_mono, &ana_silence_src, ANA_WINDOW, ANA_LEFT_HZ);

/* The full chain, driven by the pipeline thread rather than by the test. */
AUDIO_TONE_GEN_NODE_DEFINE(chain_gen, AUDIO_TONE_GEN_FULL_SCALE_Q15, ANA_CHAIN_SAMPLES,
			   ANA_LEFT_HZ, ANA_RIGHT_HZ);
AUDIO_TONE_ANALYZER_NODE_DEFINE(chain_analyzer, &chain_gen, ANA_WINDOW, ANA_LEFT_HZ, ANA_RIGHT_HZ);
AUDIO_PIPELINE_DEFINE(ana_pipeline, ANA_CHAIN_FRAME_SAMPLES, CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE,
		      CONFIG_AUDIO_PIPELINE_THREAD_PRIO);

static const struct audio_stream_config stereo_format = {
	.sample_rate_hz = ANA_RATE_HZ,
	.channels = 2U,
	.valid_bits_per_sample = 16U,
	.format = AUDIO_SAMPLE_FORMAT_S32_LE,
};

static const struct audio_stream_config mono_format = {
	.sample_rate_hz = ANA_RATE_HZ,
	.channels = 1U,
	.valid_bits_per_sample = 16U,
	.format = AUDIO_SAMPLE_FORMAT_S32_LE,
};

/*
 * Nodes the cases drive directly, without a pipeline: a node takes the rate and
 * the channel count from audio_node.pipeline_format (spec §5.2), so the fixture
 * installs by hand what audio_pipeline_start() would install.
 */
static struct audio_node *const stereo_nodes[] = {
	&ana_gen,         &ana_tone,       &ana_gen_odd,     &ana_odd,
	&ana_gen_twin,    &ana_twin,       &ana_gen_wrong,   &ana_wrong,
	&ana_gen_swapped, &ana_swapped,    &ana_gen_quiet,   &ana_quiet,
	&ana_silence_src, &ana_silent,     &ana_noise_src,   &ana_noise,
	&ana_replay_src,  &ana_replay_sink,
};

static struct audio_node *const mono_nodes[] = {&ana_gen_low,   &ana_low, &ana_gen_low_quiet,
						&ana_low_quiet, &ana_dc,  &ana_nyquist};

/* A frame of a few hundred samples has no business on the Ztest stack. */
static int32_t ana_frame[CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES];

static void ana_before(void *fixture)
{
	size_t i;

	ARG_UNUSED(fixture);

	for (i = 0; i < ARRAY_SIZE(stereo_nodes); i++) {
		stereo_nodes[i]->pipeline_format = &stereo_format;
		(void)audio_node_close(stereo_nodes[i]);
	}

	for (i = 0; i < ARRAY_SIZE(mono_nodes); i++) {
		mono_nodes[i]->pipeline_format = &mono_format;
		(void)audio_node_close(mono_nodes[i]);
	}

	/* Mono by default; the case that needs it stereo says so. */
	ana_mono.pipeline_format = &mono_format;
	(void)audio_node_close(&ana_mono);

	/* One case writes this by hand to reach the run-time window check the
	 * definition macro normally catches at build time; restoring it here
	 * rather than there keeps a failed assertion inside that case.
	 */
	((struct audio_tone_analyzer_state *)ana_tone.state)->window_samples = ANA_WINDOW;
}

/* -------------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------
 */

/** @brief Drive @p sink for @p samples total interleaved samples. */
static void ana_run(struct audio_node *sink, size_t samples)
{
	size_t done = 0;

	while (done < samples) {
		struct audio_buffer_view view = {
			.data = ana_frame,
			.capacity = MIN(ARRAY_SIZE(ana_frame), samples - done),
		};
		size_t produced = 0;

		zassert_equal(audio_node_process(sink, &view, &produced), 0,
			      "process failed after %zu samples", done);
		zassert_not_equal(produced, 0U, "the chain stopped after %zu samples", done);

		done += produced;
	}
}

/**
 * @brief Pull @p samples out of @p source and throw them away.
 *
 * The offset an analyzer downstream then sees is the unmeasured latency of the
 * link it exists to be immune to. Done on an already open generator and before
 * the analyzer is opened, so nothing but the starting phase differs between one
 * offset and the next.
 */
static void ana_skip(struct audio_node *source, size_t samples)
{
	size_t done = 0;

	while (done < samples) {
		struct audio_buffer_view view = {
			.data = ana_frame,
			.capacity = MIN(ARRAY_SIZE(ana_frame), samples - done),
		};
		size_t produced = 0;

		zassert_equal(audio_node_process(source, &view, &produced), 0, "generator failed");
		zassert_not_equal(produced, 0U, "the generator stopped early");

		done += produced;
	}
}

/** @brief Pull exactly @p samples out of @p source into @p dst. */
static void ana_capture(struct audio_node *source, int32_t *dst, size_t samples)
{
	size_t done = 0;

	while (done < samples) {
		struct audio_buffer_view view = {
			.data = &dst[done],
			.capacity = samples - done,
		};
		size_t produced = 0;

		zassert_equal(audio_node_process(source, &view, &produced), 0, "generator failed");
		zassert_not_equal(produced, 0U, "the generator stopped early");

		done += produced;
	}
}

/** @brief Open @p source and @p sink, measure @p windows, return the verdict. */
static void ana_measure(struct audio_node *source, struct audio_node *sink, size_t window,
			size_t channels, size_t windows, struct audio_tone_analyzer_result *result)
{
	zassert_equal(audio_node_open(source), 0, "source open failed");
	zassert_equal(audio_node_open(sink), 0, "analyzer open failed");

	ana_run(sink, windows * window * channels);

	zassert_equal(audio_tone_analyzer_get_result(sink, result), 0, "get_result failed");
	zassert_equal(result->windows, windows, "%u windows completed, expected %zu",
		      result->windows, windows);

	zassert_equal(audio_node_close(sink), 0, "analyzer close failed");
	zassert_equal(audio_node_close(source), 0, "source close failed");
}

/* -------------------------------------------------------------------------
 * The verdict: the tone that must pass
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_tone_analyzer, test_sink_reports_the_expected_tone_as_a_pass)
{
	struct audio_tone_analyzer_result result;

	ana_measure(&ana_gen, &ana_tone, ANA_WINDOW, 2U, ANA_WINDOWS, &result);

	zassert_equal(result.verdict, AUDIO_TONE_ANALYZER_VERDICT_PASS,
		      "the configured tone was not a pass, verdict %d", (int)result.verdict);

	/* The result is a value, not a log line: everything the verdict rests on
	 * is readable, per channel and per expected frequency.
	 */
	zassert_equal(result.channels, 2U, "the result does not describe both channels");
	zassert_equal(result.window_samples, ANA_WINDOW, "the result reports the wrong window");

	zassert_true(result.channel[0].in_band_q15[0] > AUDIO_TONE_ANALYZER_PASS_Q15,
		     "left carries %d of %d in band", result.channel[0].in_band_q15[0],
		     AUDIO_TONE_ANALYZER_UNITY_Q15);
	zassert_true(result.channel[1].in_band_q15[1] > AUDIO_TONE_ANALYZER_PASS_Q15,
		     "right carries %d of %d in band", result.channel[1].in_band_q15[1],
		     AUDIO_TONE_ANALYZER_UNITY_Q15);

	/* Every frequency is measured on every channel, which is what makes the
	 * swap case below a swap rather than a pass: the tone that belongs to
	 * the other channel has to be absent here.
	 */
	zassert_true(result.channel[0].in_band_q15[1] < AUDIO_TONE_ANALYZER_PASS_Q15,
		     "left also carries the right channel's tone");
	zassert_true(result.channel[1].in_band_q15[0] < AUDIO_TONE_ANALYZER_PASS_Q15,
		     "right also carries the left channel's tone");

	zassert_equal(result.channel[0].strongest, 0, "left's strongest tone is not its own");
	zassert_equal(result.channel[1].strongest, 1, "right's strongest tone is not its own");
	zassert_true(result.channel[0].tonal, "a pure tone was not called tonal");
	zassert_false(result.channel[0].silent, "a full-scale tone was called silent");
}

/* -------------------------------------------------------------------------
 * Offset invariance, the property the whole approach rests on
 * ----------------------------------------------------------------------
 */

static void ana_assert_offset_invariant(struct audio_node *source, struct audio_node *sink,
					size_t window)
{
	int32_t lowest = INT32_MAX;
	int32_t highest = 0;
	size_t offset;

	for (offset = 0; offset < window; offset += ANA_OFFSET_STEP) {
		struct audio_tone_analyzer_result result;
		size_t channel;

		zassert_equal(audio_node_open(source), 0, "source open failed");
		ana_skip(source, offset * 2U);

		zassert_equal(audio_node_open(sink), 0, "analyzer open failed");
		ana_run(sink, ANA_WINDOWS * window * 2U);
		zassert_equal(audio_tone_analyzer_get_result(sink, &result), 0,
			      "get_result failed");

		/* The verdict, first: the same tone at any offset is the same
		 * answer. A node whose reading depended on where the window
		 * happened to start would fail here at some offset, not at all
		 * of them, which is why this sweeps rather than samples.
		 */
		zassert_equal(result.verdict, AUDIO_TONE_ANALYZER_VERDICT_PASS,
			      "offset %zu turned the verdict into %d", offset,
			      (int)result.verdict);

		for (channel = 0; channel < 2U; channel++) {
			int32_t in_band = result.channel[channel].in_band_q15[channel];

			lowest = MIN(lowest, in_band);
			highest = MAX(highest, in_band);
		}

		zassert_equal(audio_node_close(sink), 0, "analyzer close failed");
		zassert_equal(audio_node_close(source), 0, "source close failed");
	}

	/* And the number behind the verdict has to be steady as well, or the
	 * verdict is only invariant because the threshold is far away. A whole
	 * number of bins makes this exact; a fractional one leaves the ripple of
	 * the tone's own mirror image, which is under a percent for any window
	 * worth using.
	 */
	zassert_true((int64_t)(highest - lowest) * 100 <= (int64_t)highest * 5,
		     "the in-band reading swung from %d to %d across offsets", lowest, highest);
	zassert_true(lowest > AUDIO_TONE_ANALYZER_PASS_Q15, "the in-band reading fell to %d",
		     lowest);
}

ZTEST(audio_pipeline_tone_analyzer, test_sink_verdict_is_offset_invariant)
{
	ana_assert_offset_invariant(&ana_gen, &ana_tone, ANA_WINDOW);
}

ZTEST(audio_pipeline_tone_analyzer, test_sink_verdict_is_offset_invariant_off_bin)
{
	ana_assert_offset_invariant(&ana_gen_odd, &ana_odd, ANA_ODD_WINDOW);
}

/* -------------------------------------------------------------------------
 * The three inputs that must not pass, and the swap
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_tone_analyzer, test_sink_reports_silence)
{
	struct audio_tone_analyzer_result result;

	ana_measure(&ana_silence_src, &ana_silent, ANA_WINDOW, 2U, ANA_WINDOWS, &result);

	zassert_equal(result.verdict, AUDIO_TONE_ANALYZER_VERDICT_SILENT,
		      "silence was reported as %d", (int)result.verdict);
	zassert_true(result.channel[0].silent, "a silent channel was not marked silent");
	zassert_equal(result.channel[0].rms, 0, "digital silence has an RMS of %d",
		      result.channel[0].rms);
	zassert_equal(result.channel[0].strongest, -1, "silence has a strongest tone");
}

ZTEST(audio_pipeline_tone_analyzer, test_sink_reports_a_wrong_frequency)
{
	struct audio_tone_analyzer_result result;

	ana_measure(&ana_gen_wrong, &ana_wrong, ANA_WINDOW, 2U, ANA_WINDOWS, &result);

	/* The criterion this file exists for: an oracle that cannot fail is
	 * worse than no oracle.
	 */
	zassert_not_equal(result.verdict, AUDIO_TONE_ANALYZER_VERDICT_PASS,
			  "a tone at the wrong frequency passed");
	zassert_equal(result.verdict, AUDIO_TONE_ANALYZER_VERDICT_WRONG_FREQ,
		      "a loud wrong tone was reported as %d", (int)result.verdict);

	/* Loud, and still empty where it was supposed to be: the in-band
	 * fraction is what separates a correct tone from a louder wrong one,
	 * and an absolute magnitude could not.
	 */
	zassert_false(result.channel[0].silent, "the wrong tone was not even loud");
	zassert_true(result.channel[0].in_band_q15[0] < AUDIO_TONE_ANALYZER_PASS_Q15,
		     "5 kHz read as 1 kHz: %d of %d in band", result.channel[0].in_band_q15[0],
		     AUDIO_TONE_ANALYZER_UNITY_Q15);
	zassert_true(result.channel[0].tonal, "a sine was not recognised as a tone");
}

ZTEST(audio_pipeline_tone_analyzer, test_sink_reports_broadband_noise)
{
	struct audio_tone_analyzer_result result;

	ana_measure(&ana_noise_src, &ana_noise, ANA_WINDOW, 2U, ANA_WINDOWS, &result);

	zassert_not_equal(result.verdict, AUDIO_TONE_ANALYZER_VERDICT_PASS,
			  "full-scale noise passed as a tone");
	zassert_equal(result.verdict, AUDIO_TONE_ANALYZER_VERDICT_NOISE,
		      "noise was reported as %d", (int)result.verdict);

	/* Noise is loud, spread over every bin, and nothing a one-sinusoid fit
	 * can predict - which is what tells it from the wrong frequency above.
	 */
	zassert_false(result.channel[0].silent, "full-scale noise was called silent");
	zassert_false(result.channel[0].tonal, "noise was called tonal, residual %d",
		      result.channel[0].residual_q15);
	zassert_true(result.channel[0].in_band_q15[0] < AUDIO_TONE_ANALYZER_PASS_Q15,
		     "noise filled the expected bin: %d of %d", result.channel[0].in_band_q15[0],
		     AUDIO_TONE_ANALYZER_UNITY_Q15);
}

ZTEST(audio_pipeline_tone_analyzer, test_sink_reports_a_channel_swap)
{
	struct audio_tone_analyzer_result result;

	ana_measure(&ana_gen_swapped, &ana_swapped, ANA_WINDOW, 2U, ANA_WINDOWS, &result);

	/* Both tones are present and both are correct - they are on the wrong
	 * wires. Measuring one frequency per channel would report this as a
	 * pass, which is why every frequency is measured on every channel.
	 */
	zassert_equal(result.verdict, AUDIO_TONE_ANALYZER_VERDICT_SWAPPED,
		      "a swapped pair was reported as %d", (int)result.verdict);
	zassert_equal(result.channel[0].strongest, 1, "left's strongest tone is not the right's");
	zassert_equal(result.channel[1].strongest, 0, "right's strongest tone is not the left's");
	zassert_true(result.channel[0].in_band_q15[1] > AUDIO_TONE_ANALYZER_PASS_Q15,
		     "left does not carry the right channel's tone");
}

ZTEST(audio_pipeline_tone_analyzer, test_sink_prefers_a_quiet_right_tone_to_a_loud_wrong_one)
{
	struct audio_tone_analyzer_result quiet;
	struct audio_tone_analyzer_result loud;

	ana_measure(&ana_gen_quiet, &ana_quiet, ANA_WINDOW, 2U, ANA_WINDOWS, &quiet);
	ana_measure(&ana_gen_wrong, &ana_wrong, ANA_WINDOW, 2U, ANA_WINDOWS, &loud);

	/* 40 dB apart in level, and the verdicts go the other way round. The
	 * reported energy is in-band against total for exactly this reason.
	 */
	zassert_true(quiet.channel[0].rms < loud.channel[0].rms / 10,
		     "the quiet tone (%d) is not much quieter than the wrong one (%d)",
		     quiet.channel[0].rms, loud.channel[0].rms);
	zassert_equal(quiet.verdict, AUDIO_TONE_ANALYZER_VERDICT_PASS,
		      "a quiet correct tone did not pass");
	zassert_not_equal(loud.verdict, AUDIO_TONE_ANALYZER_VERDICT_PASS,
			  "a loud wrong tone passed");
}

/* -------------------------------------------------------------------------
 * The accumulator bound
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_tone_analyzer, test_sink_full_scale_does_not_overflow_the_accumulators)
{
	struct audio_tone_analyzer_result loud;
	struct audio_tone_analyzer_result quiet;
	int64_t state_max;
	int32_t lowest;
	int32_t highest;

	/*
	 * The arithmetic first, as the node states it: the Goertzel state is
	 * bounded by N * max|x| / |sin w|, and open() keeps every frequency at
	 * least one bin from DC and Nyquist, so |sin w| >= sin(2*pi/N) >= 6/N.
	 * The recurrence multiplies that state by 2*cos(w) in Q24, and that
	 * product is what has to stay inside int64_t.
	 */
	state_max = (int64_t)AUDIO_TONE_ANALYZER_MAX_WINDOW *
		    (int64_t)AUDIO_TONE_ANALYZER_MAX_WINDOW *
		    (int64_t)AUDIO_TONE_ANALYZER_INPUT_MAX / 6;

	zassert_true(state_max < INT64_MAX / (INT64_C(2) << 24),
		     "the bound on the recurrence state does not fit int64_t");

	/*
	 * Then the measurement, at the configuration that reaches it: full
	 * scale, the longest window the node supports and the lowest frequency
	 * it accepts there. A wrapped accumulator does not announce itself - it
	 * reports a magnitude that is merely wrong - so the fault has to be
	 * caught by a property of the reading rather than by the reading itself.
	 *
	 * It cannot be caught by an absolute in-band fraction, and it is worth
	 * saying why, because "a full-scale tone must read full scale" is the
	 * obvious thing to write here and it is wrong. 12 Hz over 4096 samples
	 * at 48 kHz is 1.024 cycles per window - deliberately just off a bin
	 * centre, because open() refuses anything closer to DC, and a whole
	 * number of bins is not reachable at an integer frequency this low. The
	 * tone's own negative-frequency image therefore leaks into the bin, and
	 * the in-band fraction lands about 2 % under unity in exact arithmetic
	 * before a single bit is rounded. That is scalloping loss, not lost
	 * bits, and no implementation can be asked to beat it.
	 *
	 * The property that does separate the two is proportionality. The
	 * reading is a ratio of energies, so scaling the input scales numerator
	 * and denominator alike and the fraction does not move - scalloping loss
	 * included, because it is a property of the frequency and the window and
	 * not of the level. A wrapped accumulator has no such invariance: a
	 * magnitude folded back through int64_t bears no fixed relation to the
	 * energy it is divided by, so it moves the loud reading and leaves the
	 * quiet one alone. Measuring the same tone twice, once at full scale and
	 * once at a hundredth of it where nothing can wrap, therefore asks the
	 * overflow question directly and cancels everything that is not overflow.
	 */
	ana_measure(&ana_gen_low, &ana_low, ANA_LONG_WINDOW, 1U, 1U, &loud);
	ana_measure(&ana_gen_low_quiet, &ana_low_quiet, ANA_LONG_WINDOW, 1U, 1U, &quiet);

	zassert_equal(loud.verdict, AUDIO_TONE_ANALYZER_VERDICT_PASS,
		      "full scale over the longest window was reported as %d", (int)loud.verdict);
	zassert_equal(quiet.verdict, AUDIO_TONE_ANALYZER_VERDICT_PASS,
		      "the same tone 40 dB down was reported as %d, so it is no control",
		      (int)quiet.verdict);

	lowest = MIN(loud.channel[0].in_band_q15[0], quiet.channel[0].in_band_q15[0]);
	highest = MAX(loud.channel[0].in_band_q15[0], quiet.channel[0].in_band_q15[0]);

	/* 1 % of the reading, which is not a round number picked for looking
	 * safe. What is left between the two once the ratio has cancelled the
	 * amplitude is the quantisation the quiet stimulus carries, 40 dB closer
	 * to the least significant bit than the loud one: measured, the two sit
	 * about 0.05 % apart, so this leaves a factor of twenty. On the other
	 * side, the mildest wrap there is - one bit too few in the recurrence
	 * state at this corner - moves the full-scale reading by around 2 %,
	 * because the state is some thirty-six bits wide here and folding its
	 * top bit away changes the magnitude by a lot more than the ratio's
	 * own noise. 1 % sits between those two with room either way.
	 */
	zassert_true((int64_t)(highest - lowest) * 100 <= (int64_t)highest,
		     "the same tone read %d of %d in band at full scale and %d at a hundredth "
		     "of it; the fraction is amplitude invariant unless an accumulator wrapped",
		     loud.channel[0].in_band_q15[0], AUDIO_TONE_ANALYZER_UNITY_Q15,
		     quiet.channel[0].in_band_q15[0]);

	zassert_true(loud.channel[0].rms > 20000, "a full-scale tone has an RMS of only %d",
		     loud.channel[0].rms);
	zassert_true(quiet.channel[0].rms < loud.channel[0].rms / 10,
		     "the control (RMS %d) is not much quieter than full scale (RMS %d), so it "
		     "does not put the accumulators anywhere they could not wrap",
		     quiet.channel[0].rms, loud.channel[0].rms);
}

/* -------------------------------------------------------------------------
 * open(): the configuration has to agree with the bound format
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_tone_analyzer, test_sink_requires_a_bound_format)
{
	struct audio_tone_analyzer_state *state = ana_tone.state;
	int ret;

	ana_tone.pipeline_format = NULL;

	ret = audio_node_open(&ana_tone);
	zassert_equal(ret, -EINVAL, "an analyzer without a bound format must fail, got %d", ret);
	zassert_not_equal(ret, -EPIPE, "a node must never return -EPIPE; that means EOF");
	zassert_false(state->is_open, "a failed open() left the node open");
}

ZTEST(audio_pipeline_tone_analyzer, test_sink_rejects_a_tone_count_the_format_disagrees_with)
{
	int ret;

	/* One expectation into a stereo pipeline: the second channel would go
	 * unmeasured, and an unmeasured channel is exactly where a fault hides.
	 */
	ana_mono.pipeline_format = &stereo_format;

	ret = audio_node_open(&ana_mono);
	zassert_equal(ret, -ENOTSUP, "1 tone into 2 channels must be refused, got %d", ret);
}

ZTEST(audio_pipeline_tone_analyzer, test_sink_rejects_a_frequency_it_cannot_resolve)
{
	int ret;

	/* A window of N samples resolves fs/N. Inside the first bin a tone
	 * cannot be told from DC, and it is also where the recurrence state
	 * grows past the bound the node asserts.
	 */
	ret = audio_node_open(&ana_dc);
	zassert_equal(ret, -EINVAL, "a tone inside the first bin must be refused, got %d", ret);

	ret = audio_node_open(&ana_nyquist);
	zassert_equal(ret, -EINVAL, "a tone inside the last bin must be refused, got %d", ret);
}

ZTEST(audio_pipeline_tone_analyzer, test_sink_rejects_a_window_outside_the_proved_range)
{
	struct audio_tone_analyzer_state *state = ana_tone.state;
	uint32_t window = state->window_samples;
	int ret;

	/* The definition macro asserts this at build time, so reaching it needs
	 * the state written by hand - but the run-time check is what keeps the
	 * accumulator bound true for a window that arrived some other way.
	 */
	state->window_samples = AUDIO_TONE_ANALYZER_MIN_WINDOW - 1U;
	ret = audio_node_open(&ana_tone);
	zassert_equal(ret, -EINVAL, "a window under the floor must be refused, got %d", ret);

	state->window_samples = AUDIO_TONE_ANALYZER_MAX_WINDOW + 1U;
	ret = audio_node_open(&ana_tone);
	zassert_equal(ret, -EINVAL, "a window over the ceiling must be refused, got %d", ret);

	state->window_samples = window;
}

/* -------------------------------------------------------------------------
 * Instances, frames and misuse
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_tone_analyzer, test_sink_instances_share_no_state)
{
	struct audio_tone_analyzer_state *a = ana_tone.state;
	struct audio_tone_analyzer_state *b = ana_twin.state;
	struct audio_tone_analyzer_result result;

	zassert_not_equal(a, b, "two instances share one state object");

	zassert_equal(audio_node_open(&ana_twin), 0, "open failed");
	ana_measure(&ana_gen, &ana_tone, ANA_WINDOW, 2U, ANA_WINDOWS, &result);

	/* Measuring on one instance must leave the other exactly where its own
	 * open() left it - no window filled, no verdict formed.
	 */
	zassert_equal(audio_tone_analyzer_get_result(&ana_twin, &result), 0, "get_result failed");
	zassert_equal(result.windows, 0U, "measuring on one instance advanced the other");
	zassert_equal(result.verdict, AUDIO_TONE_ANALYZER_VERDICT_NONE,
		      "measuring on one instance gave the other a verdict");

	zassert_equal(audio_node_close(&ana_twin), 0, "close failed");
}

ZTEST(audio_pipeline_tone_analyzer, test_sink_window_spans_frames)
{
	struct audio_tone_analyzer_result result;
	size_t done = 0;

	zassert_equal(audio_node_open(&ana_gen), 0, "open failed");
	zassert_equal(audio_node_open(&ana_tone), 0, "open failed");

	/* Frames of 30 samples against a window of 960 per channel: the window
	 * closes in the middle of a frame, over and over, and the frame size
	 * divides neither the window nor the run. A frame is a transport detail
	 * and the measurement has to be blind to it.
	 */
	while (done < ANA_WINDOWS * ANA_WINDOW * 2U) {
		struct audio_buffer_view view = {
			.data = ana_frame,
			.capacity = 30U,
		};
		size_t produced = 0;

		zassert_equal(audio_node_process(&ana_tone, &view, &produced), 0, "process failed");
		done += produced;
	}

	zassert_equal(audio_tone_analyzer_get_result(&ana_tone, &result), 0, "get_result failed");
	zassert_equal(result.verdict, AUDIO_TONE_ANALYZER_VERDICT_PASS,
		      "an odd frame size turned the verdict into %d", (int)result.verdict);

	zassert_equal(audio_node_close(&ana_tone), 0, "close failed");
	zassert_equal(audio_node_close(&ana_gen), 0, "close failed");
}

ZTEST(audio_pipeline_tone_analyzer, test_sink_carries_the_interleave_across_frames)
{
	struct audio_fake_source *script = ana_replay_src.state;
	struct audio_tone_analyzer_result result;
	size_t produced;

	/* One window of the generator's own output, replayed 37 samples at a
	 * time: every other frame therefore ends in the middle of a stereo
	 * sample set. An analyzer that restarted at channel 0 on each frame
	 * would transpose the two channels from there on, which at these two
	 * frequencies is the difference between a pass and a swap.
	 */
	zassert_equal(audio_node_open(&ana_gen), 0, "open failed");
	ana_capture(&ana_gen, ana_replay, ARRAY_SIZE(ana_replay));
	zassert_equal(audio_node_close(&ana_gen), 0, "close failed");

	audio_fake_source_reset(script);
	script->samples = ana_replay;
	script->sample_count = ARRAY_SIZE(ana_replay);
	script->chunk = 37U;

	zassert_equal(audio_node_open(&ana_replay_src), 0, "open failed");
	zassert_equal(audio_node_open(&ana_replay_sink), 0, "open failed");

	do {
		struct audio_buffer_view view = {
			.data = ana_frame,
			.capacity = ARRAY_SIZE(ana_frame),
		};

		produced = 0;
		zassert_equal(audio_node_process(&ana_replay_sink, &view, &produced), 0,
			      "process failed");
	} while (produced != 0U);

	zassert_equal(audio_tone_analyzer_get_result(&ana_replay_sink, &result), 0,
		      "get_result failed");
	zassert_equal(result.windows, 1U, "%u windows closed, expected 1", result.windows);
	zassert_equal(result.verdict, AUDIO_TONE_ANALYZER_VERDICT_PASS,
		      "frames ending mid sample set turned the verdict into %d",
		      (int)result.verdict);

	zassert_equal(audio_node_close(&ana_replay_sink), 0, "close failed");
	zassert_equal(audio_node_close(&ana_replay_src), 0, "close failed");
}

ZTEST(audio_pipeline_tone_analyzer, test_sink_drops_a_partial_window_at_eof)
{
	struct audio_fake_source *script = ana_replay_src.state;
	struct audio_tone_analyzer_result result;
	size_t produced;

	/* Half a window, then end of stream. A window that never filled reads
	 * low, so measuring it would turn a clean end of stream into a failed
	 * verdict - the one way an oracle can fail that nobody would suspect.
	 */
	audio_fake_source_reset(script);
	script->samples = ana_replay;
	script->sample_count = ANA_WINDOW;

	zassert_equal(audio_node_open(&ana_replay_src), 0, "open failed");
	zassert_equal(audio_node_open(&ana_replay_sink), 0, "open failed");

	do {
		struct audio_buffer_view view = {
			.data = ana_frame,
			.capacity = ARRAY_SIZE(ana_frame),
		};

		produced = 0;
		zassert_equal(audio_node_process(&ana_replay_sink, &view, &produced), 0,
			      "process failed");
	} while (produced != 0U);

	zassert_equal(audio_tone_analyzer_get_result(&ana_replay_sink, &result), 0,
		      "get_result failed");
	zassert_equal(result.windows, 0U, "a partial window was measured anyway");
	zassert_equal(result.verdict, AUDIO_TONE_ANALYZER_VERDICT_NONE,
		      "a partial window produced a verdict");

	/* EOF stays EOF (manifest §7) rather than starting a new window. */
	{
		struct audio_buffer_view view = {
			.data = ana_frame,
			.capacity = ARRAY_SIZE(ana_frame),
		};

		produced = 1;
		zassert_equal(audio_node_process(&ana_replay_sink, &view, &produced), 0,
			      "EOF must return 0");
		zassert_equal(produced, 0U, "the sink restarted after end of stream");
	}

	zassert_equal(audio_node_close(&ana_replay_sink), 0, "close failed");
	zassert_equal(audio_node_close(&ana_replay_src), 0, "close failed");
}

ZTEST(audio_pipeline_tone_analyzer, test_sink_process_without_open_fails)
{
	struct audio_buffer_view view = {
		.data = ana_frame,
		.capacity = ARRAY_SIZE(ana_frame),
	};
	size_t produced = 1;
	int ret;

	ret = audio_node_process(&ana_tone, &view, &produced);
	zassert_true(ret < 0, "process() without open() must fail");
	zassert_not_equal(ret, -EPIPE, "a node must never return -EPIPE; that means EOF");
	zassert_equal(produced, 0U, "a failing process() must not claim samples");
}

ZTEST(audio_pipeline_tone_analyzer, test_sink_result_refuses_a_node_of_another_kind)
{
	struct audio_tone_analyzer_result result;

	/* The getter is public API and takes an audio_node, so it has to say no
	 * to one that is not an analyzer rather than read another node's state
	 * as if it were a verdict.
	 */
	zassert_equal(audio_tone_analyzer_get_result(&ana_gen, &result), -EINVAL,
		      "the getter accepted a generator");
	zassert_equal(audio_tone_analyzer_get_result(&ana_tone, NULL), -EINVAL,
		      "the getter accepted a NULL result");
	zassert_equal(audio_tone_analyzer_get_result(NULL, &result), -EINVAL,
		      "the getter accepted a NULL node");
}

/* -------------------------------------------------------------------------
 * The round trip, driven by a real pipeline
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_tone_analyzer, test_sink_passes_the_generator_in_a_full_chain)
{
	static const struct audio_pipeline_config cfg = {
		.frame_samples = ANA_CHAIN_FRAME_SAMPLES,
		.event_cb = NULL,
		.event_user_data = NULL,
	};
	struct audio_tone_analyzer_result result;
	struct audio_pipeline_event event;

	/*
	 * The whole point of the pair: the generator's own configured
	 * frequencies, through a real pipeline on a real worker thread, come
	 * back as a pass. Nothing here says 1 kHz twice - the analyzer is
	 * defined with the frequencies the generator was defined with, which is
	 * what an application does.
	 */
	zassert_equal(audio_pipeline_init(&ana_pipeline, &cfg, &chain_analyzer), 0, "init failed");
	zassert_equal(audio_pipeline_set_format(&ana_pipeline, &stereo_format), 0,
		      "binding the pipeline format failed");
	zassert_equal(audio_pipeline_start(&ana_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&ana_pipeline), 0, "play failed");

	zassert_equal(audio_pipeline_get_event(&ana_pipeline, &event, K_SECONDS(2)), 0,
		      "no event within 2 s");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_EOF, "expected a clean EOF, got type %d "
		      "err %d", (int)event.type, event.err);

	/* Read from the test thread while the worker thread owns the node: the
	 * result is published under a lock for exactly this.
	 */
	zassert_equal(audio_tone_analyzer_get_result(&chain_analyzer, &result), 0,
		      "get_result failed");
	zassert_equal(result.windows, ANA_CHAIN_WINDOWS, "%u windows completed, expected %u",
		      result.windows, ANA_CHAIN_WINDOWS);
	zassert_equal(result.verdict, AUDIO_TONE_ANALYZER_VERDICT_PASS,
		      "the generator into the analyzer reported %d", (int)result.verdict);

	zassert_equal(audio_pipeline_join(&ana_pipeline), 0, "join failed");

	/* And the verdict survives the chain being closed, because it is what
	 * the run was for.
	 */
	zassert_equal(audio_tone_analyzer_get_result(&chain_analyzer, &result), 0,
		      "get_result failed");
	zassert_equal(result.verdict, AUDIO_TONE_ANALYZER_VERDICT_PASS,
		      "close() threw the verdict away");
}

ZTEST_SUITE(audio_pipeline_tone_analyzer, NULL, NULL, ana_before, NULL, NULL);
