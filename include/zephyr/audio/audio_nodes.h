/*
 * Static definition macros for the nodes shipped with the subsystem.
 *
 * Every macro allocates the node's private state itself, so an application
 * never passes buffer pointers and two instances of the same node type can
 * never share state.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_AUDIO_NODES_H_
#define ZEPHYR_AUDIO_NODES_H_

#include <stdbool.h>
#include <zephyr/fs/fs.h>
#include <zephyr/types.h>

#include <zephyr/audio/audio_format.h>
#include <zephyr/audio/audio_node.h>

/** @brief Q15 unity gain: sample * AUDIO_GAIN_UNITY_Q15 >> 15 == sample. */
#define AUDIO_GAIN_UNITY_Q15 32768

/** @brief Per-instance state of the gain filter node. */
struct audio_gain_filter_state {
	/** Gain in Q15; 0 is treated as unity gain by open(). */
	int32_t gain_q15;
};

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
	 * the on-disk resolution of the WAV payload.
	 * Populated by open() from the parsed header.
	 */
	struct audio_stream_config fmt;
	/** Payload bytes the parsed @c data chunk still promises. */
	uint32_t bytes_left;
	/** True while @ref file holds an open handle. */
	bool file_open;
};

/**
 * @brief Samples the file writer narrows per filesystem write.
 *
 * Only sizes the per-instance conversion scratch buffer; a frame larger than
 * this is written in several chunks, so it does not cap the frame size.
 */
#define AUDIO_FILE_WRITER_CHUNK_SAMPLES 64U

/** @brief Per-instance state of the file writer sink node. */
struct audio_file_writer_state {
	/** Destination file, owned by the definition macro. */
	const char *path;
	/**
	 * Format the sink writes to disk. Optional: every zero field takes its
	 * default in open() (48000 Hz, 2 channels, 16 bit), so an application
	 * that is happy with the v1 defaults leaves the whole struct alone.
	 *
	 * The container field describes the *pipeline* side and is always
	 * ::AUDIO_SAMPLE_FORMAT_S32_LE; @c valid_bits_per_sample is the on-disk
	 * resolution and v1 supports 16 only. Set it before
	 * open(); the node does not look at it again afterwards.
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

extern const struct audio_node_ops file_reader_node_ops;
extern const struct audio_node_ops file_writer_node_ops;
extern const struct audio_node_ops gain_filter_node_ops;
extern const struct audio_node_ops null_sink_node_ops;

/**
 * @brief Statically define a file reader source node.
 *
 * File scope only. Allocates the node and its ::audio_file_reader_state.
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

/**
 * @brief Statically define a gain filter node.
 *
 * File scope only. Allocates the node and its ::audio_gain_filter_state.
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

/**
 * @brief Statically define a file writer sink node.
 *
 * File scope only. Allocates the node and its ::audio_file_writer_state.
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

/**
 * @brief Statically define a null sink node.
 *
 * File scope only. The node discards every frame and keeps no state.
 *
 * @param _name     Symbol name of the @ref audio_node instance.
 * @param _upstream Pointer to the upstream node.
 */
#define AUDIO_NULL_SINK_NODE_DEFINE(_name, _upstream) \
	AUDIO_NODE_DEFINE(_name, AUDIO_NODE_ROLE_SINK, &null_sink_node_ops, (_upstream), NULL)

#endif /* ZEPHYR_AUDIO_NODES_H_ */
