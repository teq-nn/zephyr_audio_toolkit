/*
 * Static definition macros for the nodes shipped with the subsystem
 * (manifest §9, spec §11.2).
 *
 * Every macro allocates the node's private state itself, so an application
 * never passes buffer pointers and two instances of the same node type can
 * never share state.
 *
 * Each node is a Kconfig symbol of its own (CONFIG_AUDIO_PIPELINE_NODE_*) and
 * only the enabled ones are compiled, so everything a node owns - its state
 * type, its ops object and its *_NODE_DEFINE() macro - is declared here under
 * the matching guard. Using the macro of a node that was not built is a
 * configuration mistake, and this header reports it as one: see
 * AUDIO_NODE_UNAVAILABLE() below.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_AUDIO_NODES_H_
#define ZEPHYR_AUDIO_NODES_H_

#include <stdbool.h>
#include <zephyr/sys/util_macro.h>
#include <zephyr/toolchain.h>
#include <zephyr/types.h>

#include <zephyr/audio/audio_format.h>
#include <zephyr/audio/audio_node.h>

#if defined(CONFIG_AUDIO_PIPELINE_NODE_FILE_READER) || \
	defined(CONFIG_AUDIO_PIPELINE_NODE_FILE_WRITER)
#include <zephyr/fs/fs.h>
#endif

/**
 * @brief Stand in for the *_NODE_DEFINE() of a node that was not built.
 *
 * A disabled node has no ops object, so its macro cannot expand to a working
 * definition. Leaving the macro undefined would report the mistake as an
 * implicit-declaration warning, and expanding it to nothing would report it at
 * link time naming a mangled state symbol - neither says which Kconfig symbol
 * turns the node back on.
 *
 * This expands to a placeholder node with no ops - so the rest of the
 * translation unit still compiles and the build reports one error rather than a
 * cascade - followed by a failing BUILD_ASSERT that names the macro and that
 * symbol. The placeholder is never reachable: the assertion fails first.
 *
 * @param _name   Symbol name the caller asked for.
 * @param _role   Role the node would have had (::audio_node_role).
 * @param _macro  Name of the macro that was used, as a string literal.
 * @param _symbol Kconfig symbol that builds the node, without the CONFIG_
 *                prefix, as a string literal.
 */
#define AUDIO_NODE_UNAVAILABLE(_name, _role, _macro, _symbol)                    \
	struct audio_node _name = {                                              \
		.role = (_role),                                                 \
	};                                                                       \
	BUILD_ASSERT(0, _macro "() needs the node it defines: set CONFIG_"       \
			_symbol "=y")

/* -------------------------------------------------------------------------
 * File reader source node
 * -------------------------------------------------------------------------
 */

#ifdef CONFIG_AUDIO_PIPELINE_NODE_FILE_READER

/** @brief Per-instance state of the file reader source node. */
struct audio_file_reader_state {
	/** Source file, owned by the definition macro. */
	const char *path;
	/** Set once the source has run out of data. */
	bool eof;

	/*
	 * Everything below belongs to the node implementation. It is only
	 * meaningful between a successful open() and the matching close(), and
	 * an application must treat it as read-only.
	 */

	/** Handle of @ref path while the node is open. */
	struct fs_file_t file;
	/**
	 * Format the node delivers: the container is always
	 * ::AUDIO_SAMPLE_FORMAT_S32_LE, while @c valid_bits_per_sample carries
	 * the on-disk resolution of the WAV payload (spec §5.2/§5.3).
	 * Populated by open() from the parsed header, which is also what open()
	 * checks against the pipeline's bound format: a file that disagrees is
	 * refused with @c -ENOTSUP rather than converted (spec §10.1).
	 */
	struct audio_stream_config fmt;
	/** Payload bytes the parsed @c data chunk still promises. */
	uint32_t bytes_left;
	/** True while @ref file holds an open handle. */
	bool file_open;
};

extern const struct audio_node_ops file_reader_node_ops;

/**
 * @brief Statically define a file reader source node.
 *
 * File scope only. Allocates the node and its ::audio_file_reader_state.
 * Needs @kconfig{CONFIG_AUDIO_PIPELINE_NODE_FILE_READER}.
 *
 * @param _name Symbol name of the @ref audio_node instance.
 * @param _path Path of the file the source reads from.
 */
#define AUDIO_FILE_READER_NODE_DEFINE(_name, _path)                                   \
	static struct audio_file_reader_state _name##_state = {                       \
		.path = (_path),                                                      \
	};                                                                            \
	AUDIO_NODE_DEFINE(_name, AUDIO_NODE_ROLE_SOURCE, &file_reader_node_ops, NULL, \
			  &_name##_state)

#else /* CONFIG_AUDIO_PIPELINE_NODE_FILE_READER */

#define AUDIO_FILE_READER_NODE_DEFINE(_name, _path)                          \
	AUDIO_NODE_UNAVAILABLE(_name, AUDIO_NODE_ROLE_SOURCE,                \
			       "AUDIO_FILE_READER_NODE_DEFINE",              \
			       "AUDIO_PIPELINE_NODE_FILE_READER")

#endif /* CONFIG_AUDIO_PIPELINE_NODE_FILE_READER */

/* -------------------------------------------------------------------------
 * File writer sink node
 * -------------------------------------------------------------------------
 */

/**
 * @brief Samples the file writer narrows per filesystem write.
 *
 * Only sizes the per-instance conversion scratch buffer; a frame larger than
 * this is written in several chunks, so it does not cap the frame size.
 */
#define AUDIO_FILE_WRITER_CHUNK_SAMPLES 64U

#ifdef CONFIG_AUDIO_PIPELINE_NODE_FILE_WRITER

/** @brief Per-instance state of the file writer sink node. */
struct audio_file_writer_state {
	/** Destination file, owned by the definition macro. */
	const char *path;
	/**
	 * Format the sink wrote to disk, as a copy of the pipeline's bound
	 * format (spec §10.2). Populated by open() from
	 * @c audio_node.pipeline_format and read-only to the application: it is
	 * an observation, not an input, and the node resolves no defaults of its
	 * own - a sink guessing a rate or a channel count is the mislabelling
	 * the top-down binding exists to prevent.
	 *
	 * The container field describes the *pipeline* side and is always
	 * ::AUDIO_SAMPLE_FORMAT_S32_LE; @c valid_bits_per_sample is the on-disk
	 * resolution and v1 supports 16 only (spec §5.2/§5.3), so open() refuses
	 * anything else with @c -ENOTSUP.
	 */
	struct audio_stream_config fmt;

	/*
	 * Everything below belongs to the node implementation. It is only
	 * meaningful between a successful open() and the matching close(), and
	 * an application must treat it as read-only.
	 */

	/** Handle of @ref path while the node is open. */
	struct fs_file_t file;
	/** Payload bytes appended to the @c data chunk so far. */
	uint32_t data_bytes;
	/** True while @ref file holds an open handle. */
	bool file_open;
	/** Set while the sizes on disk are older than @ref data_bytes. */
	bool header_stale;
	/** Scratch space for the S32 -> S16 conversion, never read by callers. */
	uint8_t chunk[AUDIO_FILE_WRITER_CHUNK_SAMPLES * sizeof(int16_t)];
};

extern const struct audio_node_ops file_writer_node_ops;

/**
 * @brief Statically define a file writer sink node.
 *
 * File scope only. Allocates the node and its ::audio_file_writer_state.
 * Needs @kconfig{CONFIG_AUDIO_PIPELINE_NODE_FILE_WRITER}.
 *
 * @param _name     Symbol name of the @ref audio_node instance.
 * @param _upstream Pointer to the upstream node.
 * @param _path     Path of the file the sink writes to.
 */
#define AUDIO_FILE_WRITER_NODE_DEFINE(_name, _upstream, _path)                             \
	static struct audio_file_writer_state _name##_state = {                            \
		.path = (_path),                                                           \
	};                                                                                 \
	AUDIO_NODE_DEFINE(_name, AUDIO_NODE_ROLE_SINK, &file_writer_node_ops, (_upstream), \
			  &_name##_state)

#else /* CONFIG_AUDIO_PIPELINE_NODE_FILE_WRITER */

#define AUDIO_FILE_WRITER_NODE_DEFINE(_name, _upstream, _path)               \
	AUDIO_NODE_UNAVAILABLE(_name, AUDIO_NODE_ROLE_SINK,                  \
			       "AUDIO_FILE_WRITER_NODE_DEFINE",              \
			       "AUDIO_PIPELINE_NODE_FILE_WRITER")

#endif /* CONFIG_AUDIO_PIPELINE_NODE_FILE_WRITER */

/* -------------------------------------------------------------------------
 * Gain filter node
 * -------------------------------------------------------------------------
 */

/** @brief Q15 unity gain: sample * AUDIO_GAIN_UNITY_Q15 >> 15 == sample. */
#define AUDIO_GAIN_UNITY_Q15 32768

#ifdef CONFIG_AUDIO_PIPELINE_NODE_GAIN_FILTER

/** @brief Per-instance state of the gain filter node. */
struct audio_gain_filter_state {
	/** Gain in Q15; 0 is treated as unity gain by open(). */
	int32_t gain_q15;
};

extern const struct audio_node_ops gain_filter_node_ops;

/**
 * @brief Statically define a gain filter node.
 *
 * File scope only. Allocates the node and its ::audio_gain_filter_state.
 * Needs @kconfig{CONFIG_AUDIO_PIPELINE_NODE_GAIN_FILTER}.
 *
 * @param _name     Symbol name of the @ref audio_node instance.
 * @param _upstream Pointer to the upstream node.
 * @param _gain_q15 Gain in Q15 (::AUDIO_GAIN_UNITY_Q15 is 1.0).
 */
#define AUDIO_GAIN_FILTER_NODE_DEFINE(_name, _upstream, _gain_q15)                           \
	static struct audio_gain_filter_state _name##_state = {                              \
		.gain_q15 = (_gain_q15),                                                     \
	};                                                                                   \
	AUDIO_NODE_DEFINE(_name, AUDIO_NODE_ROLE_FILTER, &gain_filter_node_ops, (_upstream), \
			  &_name##_state)

#else /* CONFIG_AUDIO_PIPELINE_NODE_GAIN_FILTER */

#define AUDIO_GAIN_FILTER_NODE_DEFINE(_name, _upstream, _gain_q15)           \
	AUDIO_NODE_UNAVAILABLE(_name, AUDIO_NODE_ROLE_FILTER,                \
			       "AUDIO_GAIN_FILTER_NODE_DEFINE",              \
			       "AUDIO_PIPELINE_NODE_GAIN_FILTER")

#endif /* CONFIG_AUDIO_PIPELINE_NODE_GAIN_FILTER */

/* -------------------------------------------------------------------------
 * Null sink node
 * -------------------------------------------------------------------------
 */

#ifdef CONFIG_AUDIO_PIPELINE_NODE_NULL_SINK

extern const struct audio_node_ops null_sink_node_ops;

/**
 * @brief Statically define a null sink node.
 *
 * File scope only. The node discards every frame and keeps no state.
 * Needs @kconfig{CONFIG_AUDIO_PIPELINE_NODE_NULL_SINK}.
 *
 * @param _name     Symbol name of the @ref audio_node instance.
 * @param _upstream Pointer to the upstream node.
 */
#define AUDIO_NULL_SINK_NODE_DEFINE(_name, _upstream) \
	AUDIO_NODE_DEFINE(_name, AUDIO_NODE_ROLE_SINK, &null_sink_node_ops, (_upstream), NULL)

#else /* CONFIG_AUDIO_PIPELINE_NODE_NULL_SINK */

#define AUDIO_NULL_SINK_NODE_DEFINE(_name, _upstream)                        \
	AUDIO_NODE_UNAVAILABLE(_name, AUDIO_NODE_ROLE_SINK,                  \
			       "AUDIO_NULL_SINK_NODE_DEFINE",                \
			       "AUDIO_PIPELINE_NODE_NULL_SINK")

#endif /* CONFIG_AUDIO_PIPELINE_NODE_NULL_SINK */

/* -------------------------------------------------------------------------
 * Tone generator source node
 * -------------------------------------------------------------------------
 */

/** @brief Q15 full scale: table value * AUDIO_TONE_GEN_FULL_SCALE_Q15 >> 15. */
#define AUDIO_TONE_GEN_FULL_SCALE_Q15 32768

/**
 * @brief Tones one tone generator definition can name.
 *
 * The v1 channel range (spec §5.2), because a definition names exactly one
 * frequency per channel.
 */
#define AUDIO_TONE_GEN_MAX_TONES 2

#ifdef CONFIG_AUDIO_PIPELINE_NODE_TONE_GEN

/** @brief Per-instance state of the tone generator source node. */
struct audio_tone_gen_state {
	/**
	 * Frequency of each channel's tone in Hz, in channel order, owned by
	 * the definition macro. Per channel rather than per node on purpose:
	 * left and right carrying different tones is how an analyzer downstream
	 * tells a swapped pair of wires from a correct one.
	 */
	uint32_t freq_hz[AUDIO_TONE_GEN_MAX_TONES];
	/**
	 * Frequencies the definition named, i.e. how many entries of
	 * @ref freq_hz are configuration rather than padding.
	 *
	 * Not a channel count: the channel count is the pipeline's and is read
	 * from @c audio_node.pipeline_format (spec §5.2). This is what open()
	 * checks *against* it, and a definition naming a different number of
	 * tones than the pipeline carries is refused rather than adapted.
	 */
	uint8_t tone_count;
	/**
	 * Peak amplitude as a fraction of full scale in Q15;
	 * ::AUDIO_TONE_GEN_FULL_SCALE_Q15 is full scale and 0 is silence.
	 */
	int32_t amplitude_q15;
	/**
	 * Samples to produce before the stream ends, counted as TOTAL
	 * interleaved samples across all channels like every other sample count
	 * in the subsystem (manifest §5), so it has to be a whole number of
	 * interleaved sample sets.
	 *
	 * 0 runs indefinitely, which is what a loopback under test wants: it is
	 * stopped by the application, not by the stimulus running out.
	 */
	uint32_t duration_samples;

	/*
	 * Everything below belongs to the node implementation. It is only
	 * meaningful between a successful open() and the matching close(), and
	 * an application must treat it as read-only.
	 */

	/** Phase of each tone as a fraction of a turn in 32 bit fixed point. */
	uint32_t phase[AUDIO_TONE_GEN_MAX_TONES];
	/** Phase added per sample, derived from the bound rate by open(). */
	uint32_t phase_step[AUDIO_TONE_GEN_MAX_TONES];
	/** Samples produced since open(), against @ref duration_samples. */
	uint32_t produced;
	/** True between a successful open() and its close(). */
	bool is_open;
};

extern const struct audio_node_ops tone_gen_node_ops;

/**
 * @brief Statically define a tone generator source node.
 *
 * File scope only. Allocates the node and its ::audio_tone_gen_state.
 * Needs @kconfig{CONFIG_AUDIO_PIPELINE_NODE_TONE_GEN}.
 *
 * The frequencies are variadic because how many there are is the statement the
 * definition makes about the pipeline it belongs in: one per channel, in
 * channel order. open() refuses a pipeline whose channel count says otherwise
 * rather than dropping or inventing a tone.
 *
 * @param _name             Symbol name of the @ref audio_node instance.
 * @param _amplitude_q15    Peak amplitude in Q15
 *                          (::AUDIO_TONE_GEN_FULL_SCALE_Q15 is full scale).
 * @param _duration_samples Total interleaved samples to produce, 0 for an
 *                          endless stream.
 * @param ...               One frequency in Hz per channel, at most
 *                          ::AUDIO_TONE_GEN_MAX_TONES of them.
 */
#define AUDIO_TONE_GEN_NODE_DEFINE(_name, _amplitude_q15, _duration_samples, ...)                  \
	BUILD_ASSERT(NUM_VA_ARGS(__VA_ARGS__) >= 1 &&                                              \
			     NUM_VA_ARGS(__VA_ARGS__) <= AUDIO_TONE_GEN_MAX_TONES,                 \
		     "AUDIO_TONE_GEN_NODE_DEFINE() takes one frequency per "                       \
		     "channel");                                                                   \
	static struct audio_tone_gen_state _name##_state = {                                       \
		.freq_hz = {__VA_ARGS__},                                                          \
		.tone_count = NUM_VA_ARGS(__VA_ARGS__),                                            \
		.amplitude_q15 = (_amplitude_q15),                                                 \
		.duration_samples = (_duration_samples),                                           \
	};                                                                                         \
	AUDIO_NODE_DEFINE(_name, AUDIO_NODE_ROLE_SOURCE, &tone_gen_node_ops, NULL, &_name##_state)

#else /* CONFIG_AUDIO_PIPELINE_NODE_TONE_GEN */

#define AUDIO_TONE_GEN_NODE_DEFINE(_name, _amplitude_q15, _duration_samples, ...)                  \
	AUDIO_NODE_UNAVAILABLE(_name, AUDIO_NODE_ROLE_SOURCE, "AUDIO_TONE_GEN_NODE_DEFINE",        \
			       "AUDIO_PIPELINE_NODE_TONE_GEN")

#endif /* CONFIG_AUDIO_PIPELINE_NODE_TONE_GEN */

#endif /* ZEPHYR_AUDIO_NODES_H_ */
