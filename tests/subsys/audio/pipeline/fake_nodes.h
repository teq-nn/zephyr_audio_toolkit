/*
 * Shared fake nodes for the pipeline test suites: one scripted source, one
 * counting sink and a file read-back helper.
 *
 * Every suite used to carry its own near-identical copies of these; they live
 * here now so a new test needs no node implementation of its own for the two
 * common shapes - "produce N frames, optionally failing at frame K" and "count
 * what arrives".
 *
 * Threading: the pipeline worker thread calls into these nodes while the ztest
 * thread reads their bookkeeping. Every counter the nodes write is therefore an
 * ::atomic_t (or ::atomic_ptr_t) rather than plain memory; read them with
 * atomic_get() / atomic_ptr_get(). The script fields are the other way round -
 * the ztest thread sets them before the run and the nodes only read them.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AUDIO_PIPELINE_TEST_FAKE_NODES_H_
#define AUDIO_PIPELINE_TEST_FAKE_NODES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <zephyr/audio/audio_node.h>

/** Frame budget of a source that never reaches end of stream. */
#define AUDIO_FAKE_ENDLESS SIZE_MAX

/**
 * Every semaphore the fakes wait on is taken with this timeout rather than
 * K_FOREVER, so a broken pipeline fails an assertion instead of hanging the
 * whole suite. The expired wait is counted, never silently swallowed.
 */
#define AUDIO_FAKE_SEM_TIMEOUT K_MSEC(500)

/**
 * @brief State of the shared scripted source (::audio_fake_source_ops).
 *
 * Zero-initialised, the source reports end of stream on its very first frame.
 */
struct audio_fake_source {
	/* --- Script: set by the test thread before the run. --------------- */

	/**
	 * Frames to deliver before reporting EOF; ::AUDIO_FAKE_ENDLESS never
	 * ends. Ignored when @ref samples is set.
	 */
	size_t frames_total;
	/**
	 * 1-based frame at which process() fails with @ref process_ret;
	 * 0 disables the failure injection.
	 */
	size_t fail_at_frame;
	/** Error returned at @ref fail_at_frame. */
	int process_ret;
	/** Value open() returns; non-zero makes the chain fail to open. */
	int open_ret;
	/** Value close() returns. */
	int close_ret;
	/** Container value stamped into every sample of a produced frame. */
	int32_t pattern;
	/**
	 * Literal payload handed out in @ref chunk sized pieces. When set it
	 * replaces @ref pattern and the stream ends once @ref sample_count
	 * samples have been delivered.
	 */
	const int32_t *samples;
	/** Number of samples at @ref samples. */
	size_t sample_count;
	/** Samples per process() call; 0 means "fill the buffer". */
	size_t chunk;
	/** Given once per produced frame; NULL disables. */
	struct k_sem *frame_sem;
	/**
	 * Taken once per produced frame, right after @ref frame_sem is given,
	 * so two chains can be forced through their frames in lock-step. An
	 * expired wait bumps @ref peer_timeouts. NULL disables.
	 */
	struct k_sem *peer_sem;

	/* --- Bookkeeping: written by whichever thread drives the chain. --- */

	atomic_t open_calls;
	atomic_t close_calls;
	/** Frames handed out since the last open() or rewind. */
	atomic_t frames_done;
	/** Samples of @ref samples handed out since the last open() or rewind. */
	atomic_t samples_done;
	/** Times @ref peer_sem expired instead of being released. */
	atomic_t peer_timeouts;
};

/**
 * @brief State of the shared counting sink (::audio_fake_sink_ops).
 *
 * The ops pull the upstream chain and count what comes back, which is exactly
 * what a pass-through filter does as well - a node using them may therefore
 * take either ::AUDIO_NODE_ROLE_SINK or ::AUDIO_NODE_ROLE_FILTER.
 *
 * The pull goes through audio_node_pull() like a shipped node's does, so a fake
 * wired without an upstream reports @c -ENOTSUP rather than end of stream.
 */
struct audio_fake_sink {
	/* --- Script: set by the test thread before the run. --------------- */

	/** Value open() returns; non-zero makes the chain fail to open. */
	int open_ret;
	/** Value close() returns. */
	int close_ret;
	/**
	 * 1-based frame at which process() fails with @ref process_ret before it
	 * pulls; 0 disables the failure injection. Models a node that breaks in
	 * its own body rather than below.
	 */
	size_t fail_at_frame;
	/** Error returned at @ref fail_at_frame. */
	int process_ret;
	/** Check every sample of a frame against @ref expect_pattern. */
	bool check_pattern;
	/** Container value every sample must carry when @ref check_pattern. */
	int32_t expect_pattern;
	/** Frame capacity every call must be handed; 0 disables the check. */
	size_t expect_capacity;
	/** Given once per consumed frame; NULL disables. */
	struct k_sem *frame_sem;
	/**
	 * Taken once per consumed frame, right after @ref frame_sem is given,
	 * so a test can park the chain inside process(). An expired wait bumps
	 * @ref gate_timeouts. NULL disables.
	 */
	struct k_sem *gate_sem;

	/* --- Bookkeeping: written by whichever thread drives the chain. --- */

	atomic_t open_calls;
	atomic_t close_calls;
	/** Frames carrying samples. */
	atomic_t frames_seen;
	/** Frames reporting end of stream. */
	atomic_t eof_seen;
	/** Frames holding a sample other than @ref expect_pattern. */
	atomic_t corrupt_frames;
	/** Calls handed a capacity other than @ref expect_capacity. */
	atomic_t wrong_capacity;
	/** Times @ref gate_sem expired instead of being released. */
	atomic_t gate_timeouts;
	/** Frame buffer the most recent call was handed (@c int32_t *). */
	atomic_ptr_t seen_buf;
	/** Thread that made the most recent call (@c k_tid_t). */
	atomic_ptr_t worker;
};

/** Ops of the scripted source; drives ::audio_fake_source. */
extern const struct audio_node_ops audio_fake_source_ops;

/** Ops of the counting sink/filter; drives ::audio_fake_sink. */
extern const struct audio_node_ops audio_fake_sink_ops;

/** @brief Statically define a scripted source and its @c <name>_state. */
#define AUDIO_FAKE_SOURCE_DEFINE(_name)                                                            \
	static struct audio_fake_source _name##_state;                                             \
	AUDIO_NODE_DEFINE(_name, AUDIO_NODE_ROLE_SOURCE, &audio_fake_source_ops, NULL,             \
			  &_name##_state)

/** @brief Statically define a counting sink and its @c <name>_state. */
#define AUDIO_FAKE_SINK_DEFINE(_name, _upstream)                                                   \
	static struct audio_fake_sink _name##_state;                                               \
	AUDIO_NODE_DEFINE(_name, AUDIO_NODE_ROLE_SINK, &audio_fake_sink_ops, (_upstream),          \
			  &_name##_state)

/** @brief Statically define a counting pass-through filter and its state. */
#define AUDIO_FAKE_FILTER_DEFINE(_name, _upstream)                                                 \
	static struct audio_fake_sink _name##_state;                                               \
	AUDIO_NODE_DEFINE(_name, AUDIO_NODE_ROLE_FILTER, &audio_fake_sink_ops, (_upstream),        \
			  &_name##_state)

/** @brief Clear both the script and the counters of @p src. */
void audio_fake_source_reset(struct audio_fake_source *src);

/** @brief Clear both the script and the counters of @p sink. */
void audio_fake_sink_reset(struct audio_fake_sink *sink);

/**
 * @brief Restart @p src at its first frame, keeping the script.
 *
 * open() does this on its own; a test only needs it to replay a source whose
 * node is not reopened in between.
 */
void audio_fake_source_rewind(struct audio_fake_source *src);

/**
 * @brief Read @p path in full into @p buf.
 *
 * Fails the calling test if the file cannot be opened, read or closed, or if it
 * does not fit @p cap bytes with room to spare.
 *
 * @return Number of bytes read.
 */
size_t audio_test_read_file(const char *path, void *buf, size_t cap);

#endif /* AUDIO_PIPELINE_TEST_FAKE_NODES_H_ */
