/*
 * Tone generator source node: format binding, frequency accuracy, phase
 * behaviour over long runs, container alignment and duration (issue #30,
 * manifest §4/§7, spec §4.2/§5.2/§5.3).
 *
 * The cases that matter here are the ones a short run cannot show: a generator
 * whose frequency is a fraction of a percent off, or whose phase creeps, looks
 * perfect for a few frames and then fails a loopback that runs for minutes. The
 * long cases below therefore simulate ten seconds of samples and measure the
 * result, rather than checking a handful of values.
 *
 * The "no floating point, no libm" guardrail is a property of the built object
 * rather than something a case can assert. Re-run it against the node's own
 * object under the Twister output directory:
 *
 *   find <outdir> -name tone_gen_node.c.obj -exec nm --undefined-only {} + |
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

#define TONE_RATE_HZ  48000U
#define TONE_LEFT_HZ  1000U
#define TONE_RIGHT_HZ 3000U

/* Ten seconds of one channel: the shortest run that can show a slow drift. */
#define TONE_LONG_SAMPLES (10U * TONE_RATE_HZ)

/* One second per channel, which is enough to measure a tone to well inside the
 * tolerance below without spending ten on it.
 */
#define TONE_SECOND_SAMPLES TONE_RATE_HZ

/*
 * Stated frequency tolerance: 100 ppm, i.e. 0.01 %.
 *
 * The design bound is far tighter - the phase increment is rounded once, so the
 * error is at most fs / 2^33, some 5.6 microhertz - and what is left is the
 * measurement: a zero crossing is only located to about a sample, so a run has
 * to be long before a tighter number would mean anything. 100 ppm sits above
 * the measurement noise and still fails any generator that gets its increment
 * wrong.
 */
#define TONE_FREQ_TOLERANCE_PPM 100

/*
 * Stated phase tolerance: a thousandth of a turn after ten seconds.
 *
 * A phase accumulator can only be off by the rounding of its increment, at most
 * half a phase LSB per sample, which after 480000 samples is 0.006 % of a turn.
 * A resonator recurrence - the cheaper alternative this node rejects - wanders
 * orders of magnitude further over the same run.
 */
#define TONE_PHASE_TOLERANCE (UINT32_MAX / 1000U)

/* Full scale in the container: the peak table value, MSB-aligned. */
#define TONE_FULL_SCALE ((int32_t)(32767 << 16))

/* Periods the full-scale case looks at, and the samples that takes. */
#define TONE_PEAK_PERIODS 100U
#define TONE_PEAK_SAMPLES (TONE_PEAK_PERIODS * TONE_RATE_HZ / TONE_LEFT_HZ)

/* Duration cases: 50 stereo sample sets, delivered in frames of 32. */
#define TONE_DURATION_SAMPLES 100U
#define TONE_DURATION_FRAME   32U

/* Samples the seam and determinism cases compare, per instance. */
#define TONE_COMPARE_SAMPLES 2048U

/* Frames the pipeline case runs before the configured duration ends it. */
#define TONE_CHAIN_FRAME_SAMPLES 32U
#define TONE_CHAIN_FRAMES        3U
#define TONE_CHAIN_SAMPLES       (TONE_CHAIN_FRAMES * TONE_CHAIN_FRAME_SAMPLES)

/*
 * Every node is defined through the public macro, so the cases see exactly what
 * an application sees. tone_a and tone_b are deliberately identical: two
 * instances of one configuration have to be indistinguishable.
 */
AUDIO_TONE_GEN_NODE_DEFINE(tone_a, AUDIO_TONE_GEN_FULL_SCALE_Q15, 0, TONE_LEFT_HZ, TONE_RIGHT_HZ);
AUDIO_TONE_GEN_NODE_DEFINE(tone_b, AUDIO_TONE_GEN_FULL_SCALE_Q15, 0, TONE_LEFT_HZ, TONE_RIGHT_HZ);
AUDIO_TONE_GEN_NODE_DEFINE(tone_mono, AUDIO_TONE_GEN_FULL_SCALE_Q15, 0, TONE_LEFT_HZ);
AUDIO_TONE_GEN_NODE_DEFINE(tone_dur, AUDIO_TONE_GEN_FULL_SCALE_Q15, TONE_DURATION_SAMPLES,
			   TONE_LEFT_HZ, TONE_RIGHT_HZ);
AUDIO_TONE_GEN_NODE_DEFINE(tone_odd, AUDIO_TONE_GEN_FULL_SCALE_Q15, TONE_DURATION_SAMPLES + 1U,
			   TONE_LEFT_HZ, TONE_RIGHT_HZ);
AUDIO_TONE_GEN_NODE_DEFINE(tone_loud, AUDIO_TONE_GEN_FULL_SCALE_Q15 + 1, 0, TONE_LEFT_HZ);
AUDIO_TONE_GEN_NODE_DEFINE(tone_nyquist, AUDIO_TONE_GEN_FULL_SCALE_Q15, 0, TONE_RATE_HZ / 2U);

/* Full chain: the generator ends the stream, the null sink reports it. */
AUDIO_TONE_GEN_NODE_DEFINE(chain_tone, AUDIO_TONE_GEN_FULL_SCALE_Q15, TONE_CHAIN_SAMPLES,
			   TONE_LEFT_HZ, TONE_RIGHT_HZ);
AUDIO_NULL_SINK_NODE_DEFINE(chain_tone_sink, &chain_tone);
AUDIO_PIPELINE_DEFINE(tone_pipeline, TONE_CHAIN_FRAME_SAMPLES,
		      CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE, CONFIG_AUDIO_PIPELINE_THREAD_PRIO);

static const struct audio_stream_config stereo_format = {
	.sample_rate_hz = TONE_RATE_HZ,
	.channels = 2U,
	.valid_bits_per_sample = 16U,
	.format = AUDIO_SAMPLE_FORMAT_S32_LE,
};

static const struct audio_stream_config mono_format = {
	.sample_rate_hz = TONE_RATE_HZ,
	.channels = 1U,
	.valid_bits_per_sample = 16U,
	.format = AUDIO_SAMPLE_FORMAT_S32_LE,
};

/*
 * Nodes the cases open directly, without a pipeline. A source takes the rate
 * and the channel count from audio_node.pipeline_format (spec §5.2), so the
 * fixture installs by hand what audio_pipeline_start() would install - one tone
 * per channel, which is the pairing the node insists on.
 */
static struct audio_node *const stereo_nodes[] = {&tone_a, &tone_b, &tone_dur, &tone_odd};
static struct audio_node *const mono_nodes[] = {&tone_mono, &tone_loud, &tone_nyquist};

/* Buffers are static: a frame of a few thousand samples has no business on the
 * Ztest stack.
 */
static int32_t single_buf[TONE_COMPARE_SAMPLES];
static int32_t chunked_buf[TONE_COMPARE_SAMPLES];
static int32_t frame_buf[CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES];

static void tone_before(void *fixture)
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
}

/* -------------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------
 */

/**
 * @brief Pull exactly @p samples samples out of @p node into @p dst.
 *
 * @p chunk caps what one process() call may produce, which is how the cases
 * below vary the frame size without varying anything else.
 */
static void tone_generate(struct audio_node *node, int32_t *dst, size_t samples, size_t chunk)
{
	size_t done = 0;

	while (done < samples) {
		struct audio_buffer_view view = {
			.data = &dst[done],
			.capacity = MIN(chunk, samples - done),
		};
		size_t produced = 0;

		zassert_equal(audio_node_process(node, &view, &produced), 0,
			      "process failed after %zu samples", done);
		zassert_not_equal(produced, 0U, "an endless generator stopped after %zu samples",
				  done);

		done += produced;
	}
}

/** @brief What one channel of a generated stream looks like. */
struct tone_scan {
	/** Samples of this channel seen so far. */
	size_t index;
	/** Rising zero crossings, i.e. one per period. */
	size_t crossings;
	/** Index of the first rising crossing. */
	size_t first_cross;
	/** Index of the last rising crossing. */
	size_t last_cross;
	/** Largest sample seen. */
	int32_t peak;
	/** Smallest sample seen. */
	int32_t trough;
	/** Largest sample of the first window of the run. */
	int32_t early_peak;
	/** Largest sample of the last window of the run. */
	int32_t late_peak;
	/** Samples sitting exactly on @ref peak, i.e. candidates for a flat top. */
	size_t peak_hits;
	/** Previous sample of this channel, carried across frame boundaries. */
	int32_t prev;
	/** False until @ref prev holds a sample. */
	bool have_prev;
};

static void tone_scan_init(struct tone_scan *scan)
{
	memset(scan, 0, sizeof(*scan));
	scan->peak = INT32_MIN;
	scan->trough = INT32_MAX;
	scan->early_peak = INT32_MIN;
	scan->late_peak = INT32_MIN;
}

/*
 * Fold one frame of channel @p channel into @p scan.
 *
 * @p window is the length of the leading and trailing stretches whose peaks are
 * compared, so an amplitude that decays over a run cannot hide behind the
 * overall peak; @p total is the length of the whole run, per channel.
 */
static void tone_scan_frame(struct tone_scan *scan, const int32_t *buf, size_t samples,
			    size_t channels, size_t channel, size_t window, size_t total)
{
	size_t i;

	for (i = channel; i < samples; i += channels) {
		int32_t sample = buf[i];

		if (scan->have_prev && scan->prev < 0 && sample >= 0) {
			if (scan->crossings == 0U) {
				scan->first_cross = scan->index;
			}

			scan->last_cross = scan->index;
			scan->crossings++;
		}

		if (sample > scan->peak) {
			scan->peak = sample;
			scan->peak_hits = 1U;
		} else if (sample == scan->peak) {
			scan->peak_hits++;
		}

		scan->trough = MIN(scan->trough, sample);

		if (scan->index < window) {
			scan->early_peak = MAX(scan->early_peak, sample);
		}

		if (scan->index + window >= total) {
			scan->late_peak = MAX(scan->late_peak, sample);
		}

		scan->prev = sample;
		scan->have_prev = true;
		scan->index++;
	}
}

/** @brief Absolute value of @p value. */
static int64_t tone_abs64(int64_t value)
{
	return value < 0 ? -value : value;
}

/*
 * Fail the calling test unless the tone @p scan measured is @p freq_hz within
 * TONE_FREQ_TOLERANCE_PPM.
 *
 * Measured across the span between the first and the last rising crossing
 * rather than across the whole run, so a partial period at either end cannot be
 * mistaken for a frequency error. The comparison is a cross-multiplication
 * because a ratio of integers is exactly what is being asserted - and because
 * this file has no more business with floating point than the node does.
 */
static void tone_assert_frequency(const struct tone_scan *scan, uint32_t freq_hz)
{
	int64_t cycles;
	int64_t span;
	int64_t ref;
	int64_t err;

	zassert_true(scan->crossings >= 2U, "only %zu zero crossings to measure with",
		     scan->crossings);

	cycles = (int64_t)scan->crossings - 1;
	span = (int64_t)scan->last_cross - (int64_t)scan->first_cross;

	/* cycles / span == freq_hz / rate, cross-multiplied. */
	ref = (int64_t)freq_hz * span;
	err = tone_abs64(cycles * (int64_t)TONE_RATE_HZ - ref);

	zassert_true(err * 1000000 <= ref * TONE_FREQ_TOLERANCE_PPM,
		     "%u Hz requested, %lld cycles in %lld samples is off by more than %d ppm",
		     freq_hz, (long long)cycles, (long long)span, TONE_FREQ_TOLERANCE_PPM);
}

/*
 * Run @p node for @p samples samples per channel and scan every channel into
 * @p scans, which holds one entry per channel.
 *
 * The samples are consumed frame by frame and never all held at once: ten
 * seconds of them is megabytes, and nothing here needs to look backwards.
 */
static void tone_run(struct audio_node *node, size_t channels, size_t samples,
		     struct tone_scan *scans)
{
	size_t total = samples * channels;
	size_t done = 0;
	size_t channel;

	for (channel = 0; channel < channels; channel++) {
		tone_scan_init(&scans[channel]);
	}

	while (done < total) {
		struct audio_buffer_view view = {
			.data = frame_buf,
			.capacity = MIN(ARRAY_SIZE(frame_buf), total - done),
		};
		size_t produced = 0;

		zassert_equal(audio_node_process(node, &view, &produced), 0,
			      "process failed after %zu samples", done);
		zassert_not_equal(produced, 0U, "the generator stopped after %zu samples", done);

		for (channel = 0; channel < channels; channel++) {
			tone_scan_frame(&scans[channel], frame_buf, produced, channels, channel,
					TONE_RATE_HZ / 10U, samples);
		}

		done += produced;
	}
}

/* -------------------------------------------------------------------------
 * open(): the configuration has to agree with the bound format
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_tone_gen, test_source_requires_a_bound_format)
{
	struct audio_tone_gen_state *state = tone_mono.state;
	int ret;

	/* The rate is the pipeline's (spec §5.2). A generator falling back on
	 * 48 kHz would put a tone of the wrong frequency into a pipeline that
	 * says something else, which is the mislabelling the binding removes.
	 */
	tone_mono.pipeline_format = NULL;

	ret = audio_node_open(&tone_mono);
	zassert_equal(ret, -EINVAL, "a source without a bound format must fail, got %d", ret);
	zassert_not_equal(ret, -EPIPE, "a source must never return -EPIPE; that means EOF");
	zassert_false(state->is_open, "a failed open() left the node open");
}

ZTEST(audio_pipeline_tone_gen, test_source_rejects_more_tones_than_channels)
{
	struct audio_tone_gen_state *state = tone_a.state;
	int ret;

	/* Two tones into a mono pipeline. Dropping the second one would be the
	 * adaptation spec §5.2 forbids, and it would silently remove the very
	 * thing a channel swap is detected by.
	 */
	tone_a.pipeline_format = &mono_format;

	ret = audio_node_open(&tone_a);
	zassert_equal(ret, -ENOTSUP, "2 tones into 1 channel must be refused, got %d", ret);
	zassert_false(state->is_open, "a failed open() left the node open");
}

ZTEST(audio_pipeline_tone_gen, test_source_rejects_fewer_tones_than_channels)
{
	struct audio_tone_gen_state *state = tone_mono.state;
	int ret;

	/* The other half of the same rule: a channel with no frequency would
	 * have to be filled with something the definition never asked for.
	 */
	tone_mono.pipeline_format = &stereo_format;

	ret = audio_node_open(&tone_mono);
	zassert_equal(ret, -ENOTSUP, "1 tone into 2 channels must be refused, got %d", ret);
	zassert_false(state->is_open, "a failed open() left the node open");
}

ZTEST(audio_pipeline_tone_gen, test_source_rejects_a_frequency_at_nyquist)
{
	int ret = audio_node_open(&tone_nyquist);

	/* At and above Nyquist the tone aliases to something else entirely, so
	 * an analyzer downstream would report a clean tone nobody configured.
	 */
	zassert_equal(ret, -EINVAL, "a tone at fs/2 must be refused, got %d", ret);
}

ZTEST(audio_pipeline_tone_gen, test_source_rejects_an_amplitude_above_full_scale)
{
	int ret = audio_node_open(&tone_loud);

	zassert_equal(ret, -EINVAL, "an amplitude above full scale must be refused, got %d", ret);
}

ZTEST(audio_pipeline_tone_gen, test_source_rejects_a_partial_sample_set_duration)
{
	int ret = audio_node_open(&tone_odd);

	/* An odd duration on a stereo pipeline would end the stream mid sample
	 * set and shift the channels of everything that followed.
	 */
	zassert_equal(ret, -EINVAL,
		      "a duration that is not whole sample sets must be refused, got %d", ret);
}

/* -------------------------------------------------------------------------
 * The samples themselves: container alignment, full scale, per-channel tones
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_tone_gen, test_source_lands_msb_aligned_in_the_container)
{
	size_t i;

	zassert_equal(audio_node_open(&tone_a), 0, "open failed");

	tone_generate(&tone_a, single_buf, TONE_COMPARE_SAMPLES, ARRAY_SIZE(frame_buf));

	for (i = 0; i < TONE_COMPARE_SAMPLES; i++) {
		int16_t narrowed = (int16_t)(single_buf[i] >> 16);

		/* spec §5.3: the file reader's s32 = s16 << 16 convention, so a
		 * sink narrowing back to 16 bit reproduces the tone exactly.
		 */
		zassert_equal(single_buf[i] & 0xffff, 0, "sample %zu (0x%08x) is not MSB-aligned",
			      i, (unsigned int)single_buf[i]);
		zassert_equal(single_buf[i], (int32_t)((uint32_t)(int32_t)narrowed << 16),
			      "sample %zu does not survive a 16 bit round trip", i);
	}

	zassert_equal(audio_node_close(&tone_a), 0, "close failed");
}

ZTEST(audio_pipeline_tone_gen, test_source_full_scale_neither_overflows_nor_clips)
{
	struct tone_scan scan;

	zassert_equal(audio_node_open(&tone_mono), 0, "open failed");

	/* A hundred periods at full scale: enough that the peak is reached, and
	 * enough that a flat top would be unmistakable.
	 */
	tone_run(&tone_mono, 1U, TONE_PEAK_SAMPLES, &scan);

	zassert_true(scan.peak <= TONE_FULL_SCALE, "peak 0x%08x is past full scale",
		     (unsigned int)scan.peak);
	zassert_true(scan.trough >= -TONE_FULL_SCALE, "trough 0x%08x is past full scale",
		     (unsigned int)scan.trough);

	/* Full scale must actually be reached, or the amplitude is not the one
	 * configured; the symmetric trough is what rules out a wrapped extreme,
	 * which would show up as a peak on the wrong side.
	 */
	zassert_true(scan.peak > TONE_FULL_SCALE - (TONE_FULL_SCALE / 1000),
		     "peak 0x%08x is well below full scale", (unsigned int)scan.peak);
	zassert_true(scan.trough < -TONE_FULL_SCALE + (TONE_FULL_SCALE / 1000),
		     "trough 0x%08x is well above full scale", (unsigned int)scan.trough);

	/* A clipped sine sits on its peak for a stretch of every period; an
	 * unclipped one passes through it once at most.
	 */
	zassert_true(scan.peak_hits <= 2U * TONE_PEAK_PERIODS,
		     "%zu samples sit exactly on the peak in %u periods, which is a flat top",
		     scan.peak_hits, TONE_PEAK_PERIODS);

	zassert_equal(audio_node_close(&tone_mono), 0, "close failed");
}

ZTEST(audio_pipeline_tone_gen, test_source_gives_each_channel_its_own_frequency)
{
	struct tone_scan scans[2];

	zassert_equal(audio_node_open(&tone_a), 0, "open failed");

	tone_run(&tone_a, 2U, TONE_SECOND_SAMPLES, scans);

	/* A different tone per channel is the whole point: it is what lets an
	 * analyzer downstream tell a swapped pair of wires from a correct one.
	 */
	tone_assert_frequency(&scans[0], TONE_LEFT_HZ);
	tone_assert_frequency(&scans[1], TONE_RIGHT_HZ);

	zassert_true(scans[1].crossings > 2U * scans[0].crossings,
		     "the channels carry the same tone: %zu against %zu periods",
		     scans[0].crossings, scans[1].crossings);

	zassert_equal(audio_node_close(&tone_a), 0, "close failed");
}

/* -------------------------------------------------------------------------
 * The long run: frequency accuracy and phase over ten seconds
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_tone_gen, test_source_generates_the_requested_frequency)
{
	struct tone_scan scan;

	zassert_equal(audio_node_open(&tone_mono), 0, "open failed");

	tone_run(&tone_mono, 1U, TONE_LONG_SAMPLES, &scan);
	tone_assert_frequency(&scan, TONE_LEFT_HZ);

	zassert_equal(audio_node_close(&tone_mono), 0, "close failed");
}

ZTEST(audio_pipeline_tone_gen, test_source_phase_does_not_drift_over_ten_seconds)
{
	struct audio_tone_gen_state *state = tone_mono.state;
	struct tone_scan scan;
	uint32_t ideal;
	int32_t error;

	zassert_equal(audio_node_open(&tone_mono), 0, "open failed");

	tone_run(&tone_mono, 1U, TONE_LONG_SAMPLES, &scan);

	/* Anchors the phase check below: an accumulator that landed on the right
	 * phase by accident, having spent the run at some other frequency, is
	 * not a generator that held its phase.
	 */
	tone_assert_frequency(&scan, TONE_LEFT_HZ);

	/*
	 * Where the phase should be after the run, computed the exact way
	 * rather than the node's way: samples * f * 2^32 / fs in 64 bit
	 * integers, truncated to one turn. The difference is read as a signed
	 * 32 bit value, so a wrap either side of zero comes out as the short
	 * way round rather than as a full turn of error.
	 */
	ideal = (uint32_t)((((uint64_t)TONE_LONG_SAMPLES * TONE_LEFT_HZ) << 32) / TONE_RATE_HZ);
	error = (int32_t)(state->phase[0] - ideal);

	zassert_true(tone_abs64(error) <= (int64_t)TONE_PHASE_TOLERANCE,
		     "phase is off by %lld of 2^32 after %u samples, tolerance is %u",
		     (long long)error, TONE_LONG_SAMPLES, TONE_PHASE_TOLERANCE);

	/* The amplitude has to hold up as well: a resonator recurrence keeps
	 * its frequency far better than its level, and a stimulus that fades
	 * out is a loopback failure nobody can explain.
	 */
	zassert_equal(scan.late_peak, scan.early_peak,
		      "the peak fell from 0x%08x to 0x%08x over the run",
		      (unsigned int)scan.early_peak, (unsigned int)scan.late_peak);

	zassert_equal(audio_node_close(&tone_mono), 0, "close failed");
}

/* -------------------------------------------------------------------------
 * Frames are a transport detail: seams, determinism, reopen
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_tone_gen, test_source_has_no_seam_between_frames)
{
	size_t i;

	zassert_equal(audio_node_open(&tone_a), 0, "open failed");
	zassert_equal(audio_node_open(&tone_b), 0, "open failed");

	/* One long call against many short ones. The phase carries across a
	 * frame boundary or it does not, and concatenating the short frames is
	 * what shows it: a generator that restarted a period, rounded a sample
	 * count or dropped the sub-step remainder would diverge at the seam.
	 */
	tone_generate(&tone_a, single_buf, TONE_COMPARE_SAMPLES, TONE_COMPARE_SAMPLES);
	tone_generate(&tone_b, chunked_buf, TONE_COMPARE_SAMPLES, 30U);

	for (i = 0; i < TONE_COMPARE_SAMPLES; i++) {
		zassert_equal(single_buf[i], chunked_buf[i],
			      "sample %zu differs across the seam: 0x%08x against 0x%08x", i,
			      (unsigned int)single_buf[i], (unsigned int)chunked_buf[i]);
	}

	zassert_equal(audio_node_close(&tone_a), 0, "close failed");
	zassert_equal(audio_node_close(&tone_b), 0, "close failed");
}

ZTEST(audio_pipeline_tone_gen, test_source_instances_share_no_state)
{
	struct audio_tone_gen_state *a = tone_a.state;
	struct audio_tone_gen_state *b = tone_b.state;
	size_t i;

	zassert_not_equal(a, b, "two instances share one state object");

	/* Identical configuration, so the two streams must be identical - and
	 * running one must leave the other exactly where its open() left it.
	 */
	zassert_equal(audio_node_open(&tone_b), 0, "open failed");
	zassert_equal(audio_node_open(&tone_a), 0, "open failed");

	tone_generate(&tone_a, single_buf, TONE_COMPARE_SAMPLES, ARRAY_SIZE(frame_buf));

	zassert_equal(b->phase[0], 0U, "generating from one instance advanced the other");
	zassert_equal(b->produced, 0U, "generating from one instance advanced the other");

	tone_generate(&tone_b, chunked_buf, TONE_COMPARE_SAMPLES, ARRAY_SIZE(frame_buf));

	for (i = 0; i < TONE_COMPARE_SAMPLES; i++) {
		zassert_equal(single_buf[i], chunked_buf[i],
			      "identically configured instances differ at sample %zu", i);
	}

	zassert_equal(audio_node_close(&tone_a), 0, "close failed");
	zassert_equal(audio_node_close(&tone_b), 0, "close failed");
}

ZTEST(audio_pipeline_tone_gen, test_source_restarts_the_stream_on_reopen)
{
	size_t i;

	zassert_equal(audio_node_open(&tone_a), 0, "open failed");
	tone_generate(&tone_a, single_buf, TONE_COMPARE_SAMPLES, ARRAY_SIZE(frame_buf));
	zassert_equal(audio_node_close(&tone_a), 0, "close failed");

	/* A reopened generator starts a fresh stream rather than continuing a
	 * stale phase, which is what makes a rerun reproducible.
	 */
	zassert_equal(audio_node_open(&tone_a), 0, "reopen failed");
	tone_generate(&tone_a, chunked_buf, TONE_COMPARE_SAMPLES, ARRAY_SIZE(frame_buf));

	for (i = 0; i < TONE_COMPARE_SAMPLES; i++) {
		zassert_equal(single_buf[i], chunked_buf[i], "the reopened run differs at %zu", i);
	}

	zassert_equal(audio_node_close(&tone_a), 0, "close failed");
}

/* -------------------------------------------------------------------------
 * Duration: zero runs on, a configured length ends the stream
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_tone_gen, test_source_runs_indefinitely_with_a_zero_duration)
{
	struct audio_buffer_view view = {
		.data = frame_buf,
		.capacity = ARRAY_SIZE(frame_buf),
	};
	unsigned int frame;

	zassert_equal(audio_node_open(&tone_a), 0, "open failed");

	/* Well past any plausible internal counter: a stimulus for a loopback
	 * is stopped by the application, never by itself.
	 */
	for (frame = 0U; frame < 2000U; frame++) {
		size_t produced = 0;

		zassert_equal(audio_node_process(&tone_a, &view, &produced), 0,
			      "process failed in frame %u", frame);
		zassert_equal(produced, ARRAY_SIZE(frame_buf), "frame %u is short", frame);
	}

	zassert_equal(audio_node_close(&tone_a), 0, "close failed");
}

ZTEST(audio_pipeline_tone_gen, test_source_produces_exactly_the_configured_duration)
{
	struct audio_buffer_view view = {
		.data = frame_buf,
		.capacity = TONE_DURATION_FRAME,
	};
	size_t total = 0;
	size_t produced = 0;

	zassert_equal(audio_node_open(&tone_dur), 0, "open failed");

	/* The duration is not a multiple of the frame, so the last frame is a
	 * short one - and it has to be short rather than rounded either way.
	 */
	do {
		zassert_equal(audio_node_process(&tone_dur, &view, &produced), 0, "process failed");
		total += produced;
		zassert_true(total <= TONE_DURATION_SAMPLES, "%zu samples past the configured %u",
			     total, TONE_DURATION_SAMPLES);
	} while (produced != 0U);

	zassert_equal(total, TONE_DURATION_SAMPLES, "produced %zu samples, configured %u", total,
		      TONE_DURATION_SAMPLES);

	/* EOF is out_size == 0 with a successful return (manifest §7), and it
	 * stays that way instead of looping around.
	 */
	zassert_equal(audio_node_process(&tone_dur, &view, &produced), 0, "EOF must return 0");
	zassert_equal(produced, 0U, "the generator restarted after its duration");

	zassert_equal(audio_node_close(&tone_dur), 0, "close failed");
}

/* -------------------------------------------------------------------------
 * Misuse, and the node inside a real chain
 * ----------------------------------------------------------------------
 */

ZTEST(audio_pipeline_tone_gen, test_source_process_without_open_fails)
{
	struct audio_buffer_view view = {
		.data = frame_buf,
		.capacity = ARRAY_SIZE(frame_buf),
	};
	size_t produced = 1;
	int ret;

	ret = audio_node_process(&tone_a, &view, &produced);
	zassert_true(ret < 0, "process() without open() must fail");
	zassert_not_equal(ret, -EPIPE, "a source must never return -EPIPE; that means EOF");
	zassert_equal(produced, 0U, "a failing process() must not claim samples");
}

ZTEST(audio_pipeline_tone_gen, test_source_rejects_undersized_buffer)
{
	struct audio_buffer_view view = {
		.data = frame_buf,
		.capacity = 1U,
	};
	size_t produced = 1;

	zassert_equal(audio_node_open(&tone_a), 0, "open failed");

	/* One sample of room for a stereo tone: an error, not EOF - EOF here
	 * would look like the stimulus running out.
	 */
	zassert_equal(audio_node_process(&tone_a, &view, &produced), -EINVAL,
		      "a buffer smaller than one sample set must be rejected");
	zassert_equal(produced, 0U, "a failing process() must not claim samples");

	zassert_equal(audio_node_close(&tone_a), 0, "close failed");
}

ZTEST(audio_pipeline_tone_gen, test_source_drives_pipeline_to_eof_event)
{
	static const struct audio_pipeline_config cfg = {
		.frame_samples = TONE_CHAIN_FRAME_SAMPLES,
		.event_cb = NULL,
		.event_user_data = NULL,
	};
	struct audio_tone_gen_state *state = chain_tone.state;
	struct audio_pipeline_event event;

	zassert_equal(audio_pipeline_init(&tone_pipeline, &cfg, &chain_tone_sink), 0,
		      "init failed");
	zassert_equal(audio_pipeline_set_format(&tone_pipeline, &stereo_format), 0,
		      "binding the pipeline format failed");
	zassert_equal(audio_pipeline_start(&tone_pipeline), 0, "start failed");
	zassert_equal(audio_pipeline_play(&tone_pipeline), 0, "play failed");

	zassert_equal(audio_pipeline_get_event(&tone_pipeline, &event, K_SECONDS(2)), 0,
		      "no event within 2 s");
	zassert_equal(event.type, AUDIO_PIPELINE_EVENT_EOF,
		      "expected a clean EOF, got type %d err %d", (int)event.type, event.err);
	zassert_equal(event.err, 0, "EOF event carries an error");
	zassert_equal(state->produced, TONE_CHAIN_SAMPLES,
		      "the chain pulled %u samples, configured %u", state->produced,
		      TONE_CHAIN_SAMPLES);

	zassert_equal(audio_pipeline_join(&tone_pipeline), 0, "join failed");
}

ZTEST_SUITE(audio_pipeline_tone_gen, NULL, NULL, tone_before, NULL, NULL);
