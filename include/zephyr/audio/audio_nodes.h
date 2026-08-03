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
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>
#include <zephyr/types.h>

#include <zephyr/audio/audio_format.h>
#include <zephyr/audio/audio_i2s_wire.h>
#include <zephyr/audio/audio_node.h>

#if defined(CONFIG_AUDIO_PIPELINE_NODE_FILE_READER) || \
	defined(CONFIG_AUDIO_PIPELINE_NODE_FILE_WRITER)
#include <zephyr/fs/fs.h>
#endif

#ifdef CONFIG_AUDIO_PIPELINE_NODE_I2S_OUT
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
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
 * I2S output sink node
 * -------------------------------------------------------------------------
 */

/**
 * @brief Alignment and size granularity of one I2S transfer block.
 *
 * Static allocation says how much memory a node gets, not where or how it is
 * shaped, and a DMA-driven I2S driver cares about both (manifest §6). The
 * drivers this node targets do their own cache maintenance around every
 * transfer - @c sys_cache_data_flush_range() before TX - and that acts on whole
 * cache lines, so a block that is not line aligned *and* line sized lets a
 * flush clobber whatever shares its first or last line.
 *
 * The floor of 32 is the Cortex-M7 line of the first hardware target. It is
 * kept even where the line is smaller or where there is no cache at all,
 * because over-alignment costs a few bytes of padding while under-alignment
 * costs silent corruption, and a target-dependent block size would make the
 * build-time arithmetic below unauditable.
 *
 * Where the block *lives* is the other half of the rule and is not expressible
 * here: the slab lands in @c .noinit, i.e. in the image's @c zephyr,sram, which
 * on the nucleo_h723zg target is the AXI SRAM that dma1 can address. A target
 * that puts @c zephyr,sram somewhere its I2S DMA cannot reach - DTCM on an
 * STM32H7 - needs the slab relocated, not merely realigned.
 */
#if defined(CONFIG_DCACHE_LINE_SIZE) && (CONFIG_DCACHE_LINE_SIZE > 32)
#define AUDIO_I2S_OUT_BLOCK_ALIGN CONFIG_DCACHE_LINE_SIZE
#else
#define AUDIO_I2S_OUT_BLOCK_ALIGN 32
#endif

/**
 * @brief Bytes one transfer block needs for @p _frame_samples container samples.
 *
 * Sized from the frame capacity the pipeline hands the node and never from
 * @kconfig{CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES}: the block has to hold what
 * arrives in a frame, and whether that Kconfig symbol counts total or
 * per-channel samples is not this node's question to answer (issue #23). The
 * application passes the same figure it gave AUDIO_PIPELINE_DEFINE(), so a
 * change of meaning there moves both allocations together.
 *
 * The width factor is ::AUDIO_I2S_WIRE_MAX_WORD_BYTES rather than the width the
 * bound format happens to use, because the format is bound at run time and this
 * has to be a constant. Sizing for the widest word the container can ever
 * produce is what keeps every definition site correct when the wire seam grows
 * a depth.
 */
#define AUDIO_I2S_OUT_BLOCK_BYTES(_frame_samples)                                                  \
	ROUND_UP((size_t)(_frame_samples) * AUDIO_I2S_WIRE_MAX_WORD_BYTES,                         \
		 AUDIO_I2S_OUT_BLOCK_ALIGN)

#ifdef CONFIG_AUDIO_PIPELINE_NODE_I2S_OUT

/**
 * @brief Clock role the sink configures, fixed at target (slave).
 *
 * The codec is the clock master for the systems this node is built for; it owns
 * MCLK, BCK and LRCK, and a second device driving any of them is contention on
 * a shared wire. There is deliberately no counterpart to this macro and no
 * Kconfig option beside it: @c I2S_OPT_BIT_CLK_CONTROLLER and
 * @c I2S_OPT_FRAME_CLK_CONTROLLER are zero bits, so "controller" is not a value
 * the node could pass by accident - it is the absence of the two below, and
 * they are always present.
 *
 * Named here rather than left inside the node body so that a board suite can
 * assert the clock role at build time.
 */
#define AUDIO_I2S_OUT_TX_OPTIONS (I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET)

/** @brief Per-instance state of the I2S output sink node. */
struct audio_i2s_out_state {
	/** I2S device, resolved from devicetree by the definition macro. */
	const struct device *dev;
	/**
	 * Transfer blocks the driver takes ownership of, owned by the definition
	 * macro. Per instance, so two sinks never hand a driver the same memory.
	 */
	struct k_mem_slab *slab;
	/** Bytes in one @ref slab block, owned by the definition macro. */
	size_t block_bytes;

	/*
	 * Everything below belongs to the node implementation. It is only
	 * meaningful between a successful open() and the matching close(), and
	 * an application must treat it as read-only.
	 *
	 * There is deliberately no sample rate and no channel count here: they
	 * are pipeline-wide, owned by the pipeline, and read from
	 * audio_node.pipeline_format wherever they are needed (manifest §4).
	 */

	/** True once open() has configured the transmit direction. */
	bool configured;
	/** True while the transmit direction has been started and not stopped. */
	bool started;
};

extern const struct audio_node_ops i2s_out_node_ops;

/**
 * @brief Statically define an I2S output sink node.
 *
 * File scope only. Allocates the node, its ::audio_i2s_out_state **and its
 * @c k_mem_slab**, so two instances never share transfer blocks.
 * Needs @kconfig{CONFIG_AUDIO_PIPELINE_NODE_I2S_OUT}.
 *
 * The device comes from devicetree rather than from a name looked up at run
 * time, so a chain wired to a peripheral the board does not have fails to build
 * instead of failing to start.
 *
 * @param _name          Symbol name of the @ref audio_node instance.
 * @param _upstream      Pointer to the upstream node.
 * @param _node_id       Devicetree node identifier of the I2S device, e.g.
 *                       @c DT_ALIAS(i2s_tx). Must be @c okay.
 * @param _frame_samples Frame capacity the pipeline hands this node, in total
 *                       interleaved samples - the same figure passed to
 *                       AUDIO_PIPELINE_DEFINE(). Sizes the transfer blocks; a
 *                       frame larger than one block is still transmitted, in
 *                       several blocks.
 * @param _blocks        Transfer blocks to allocate. The I2S API needs at least
 *                       two per queue; more of them buys tolerance against a
 *                       late producer at the cost of latency.
 */
#define AUDIO_I2S_OUT_NODE_DEFINE(_name, _upstream, _node_id, _frame_samples, _blocks)             \
	BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(_node_id),                                            \
		     "AUDIO_I2S_OUT_NODE_DEFINE(" #_name "): " #_node_id                           \
		     " is not an enabled devicetree node");                                        \
	BUILD_ASSERT((_frame_samples) >= 2,                                                        \
		     "AUDIO_I2S_OUT_NODE_DEFINE(" #_name "): frame_samples is the TOTAL "          \
		     "interleaved sample count and must hold at least one stereo sample "          \
		     "set (>= 2), like AUDIO_PIPELINE_DEFINE()");                                  \
	BUILD_ASSERT((_blocks) >= 2,                                                               \
		     "AUDIO_I2S_OUT_NODE_DEFINE(" #_name "): the I2S API needs at least "          \
		     "two transfer blocks per queue");                                             \
	K_MEM_SLAB_DEFINE_STATIC(_name##_slab, AUDIO_I2S_OUT_BLOCK_BYTES(_frame_samples),          \
				 (_blocks), AUDIO_I2S_OUT_BLOCK_ALIGN);                            \
	static struct audio_i2s_out_state _name##_state = {                                        \
		.dev = DEVICE_DT_GET(_node_id),                                                    \
		.slab = &_name##_slab,                                                             \
		.block_bytes = AUDIO_I2S_OUT_BLOCK_BYTES(_frame_samples),                          \
	};                                                                                         \
	AUDIO_NODE_DEFINE(_name, AUDIO_NODE_ROLE_SINK, &i2s_out_node_ops, (_upstream),             \
			  &_name##_state)

#else /* CONFIG_AUDIO_PIPELINE_NODE_I2S_OUT */

#define AUDIO_I2S_OUT_NODE_DEFINE(_name, _upstream, _node_id, _frame_samples, _blocks)             \
	AUDIO_NODE_UNAVAILABLE(_name, AUDIO_NODE_ROLE_SINK, "AUDIO_I2S_OUT_NODE_DEFINE",           \
			       "AUDIO_PIPELINE_NODE_I2S_OUT")

#endif /* CONFIG_AUDIO_PIPELINE_NODE_I2S_OUT */

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

#endif /* ZEPHYR_AUDIO_NODES_H_ */
