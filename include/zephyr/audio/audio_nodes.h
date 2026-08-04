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
#include <zephyr/sys/util_macro.h>
#include <zephyr/toolchain.h>
#include <zephyr/types.h>

#include <zephyr/audio/audio_format.h>
#include <zephyr/audio/audio_i2s_wire.h>
#include <zephyr/audio/audio_node.h>

#if defined(CONFIG_AUDIO_PIPELINE_NODE_FILE_READER) || \
	defined(CONFIG_AUDIO_PIPELINE_NODE_FILE_WRITER)
#include <zephyr/fs/fs.h>
#endif

#if defined(CONFIG_AUDIO_PIPELINE_NODE_I2S_IN) || defined(CONFIG_AUDIO_PIPELINE_NODE_I2S_OUT)
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#endif

/* The tone analyzer publishes a completed window to whichever thread asks for
 * it, which is the one seam in the node set that is not confined to the
 * pipeline thread, so its state carries a lock (spec §3.3).
 */
#ifdef CONFIG_AUDIO_PIPELINE_NODE_TONE_ANALYZER
#include <zephyr/spinlock.h>
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
 * I2S transfer blocks, shared by both I2S nodes
 * -------------------------------------------------------------------------
 */

/**
 * @brief Alignment and size granularity of one I2S transfer block.
 *
 * Static allocation says how much memory a node gets, not where or how it is
 * shaped, and a DMA-driven I2S driver cares about both (manifest §6). The
 * drivers these nodes target do their own cache maintenance around every
 * transfer - @c sys_cache_data_flush_range() before TX,
 * @c sys_cache_data_invd_range() after RX - and that acts on whole cache lines,
 * so a block that is not line aligned *and* line sized lets a flush or an
 * invalidate clobber whatever shares its first or last line.
 *
 * The rule is identical in both directions, which is why it is stated once here
 * rather than per node: an invalidate that discards a neighbour's dirty line on
 * the receive side is the same defect as a flush that overwrites it on the
 * transmit side.
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
#define AUDIO_I2S_BLOCK_ALIGN CONFIG_DCACHE_LINE_SIZE
#else
#define AUDIO_I2S_BLOCK_ALIGN 32
#endif

/**
 * @brief Bytes one transfer block needs for @p _frame_samples container samples.
 *
 * Sized from the frame capacity the pipeline hands the node and never from
 * @kconfig{CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES}: the block has to hold what
 * arrives in a frame, and whether that Kconfig symbol counts total or
 * per-channel samples is not a node's question to answer (issue #23). The
 * application passes the same figure it gave AUDIO_PIPELINE_DEFINE(), so a
 * change of meaning there moves both allocations together.
 *
 * The width factor is ::AUDIO_I2S_WIRE_MAX_WORD_BYTES rather than the width the
 * bound format happens to use, because the format is bound at run time and this
 * has to be a constant. Sizing for the widest word the container can ever
 * produce is what keeps every definition site correct when the wire seam grows
 * a depth.
 *
 * On the receive side the same figure is an upper bound rather than an exact
 * fit: a block sized for the widest word carries more of the narrower ones than
 * one frame can hold, so the source drains a block across as many frames as it
 * takes instead of dropping the surplus.
 */
#define AUDIO_I2S_BLOCK_BYTES(_frame_samples)                                                      \
	ROUND_UP((size_t)(_frame_samples) * AUDIO_I2S_WIRE_MAX_WORD_BYTES, AUDIO_I2S_BLOCK_ALIGN)

/* -------------------------------------------------------------------------
 * I2S input source node
 * -------------------------------------------------------------------------
 */

#ifdef CONFIG_AUDIO_PIPELINE_NODE_I2S_IN

/**
 * @brief Clock role the source configures, fixed at target (slave).
 *
 * Stated separately from the transmit side because the two directions are
 * configured by two independent @c i2s_configure() calls - on this hardware
 * even on two different peripherals - so each one carries its own assertion.
 *
 * Unlike the transmit side there is no controller counterpart, because nothing
 * needs one yet: on a board where the host generates the clocks, one direction
 * generates them and the other receives them, and the transmit block is the one
 * that has the MCLK pin. Add the counterpart when a board arrives that inverts
 * that - the change is the sink's @c clk_controller field and its second macro,
 * mirrored - rather than passing a bare 0 from a definition site.
 *
 * @c I2S_OPT_BIT_CLK_CONTROLLER and @c I2S_OPT_FRAME_CLK_CONTROLLER are zero
 * bits, so "controller" is not a value this node could pass by accident; it is
 * the absence of the two below, and they are always present. There is
 * deliberately no Kconfig option to drop them.
 */
#define AUDIO_I2S_IN_RX_OPTIONS (I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET)

/** @brief Per-instance state of the I2S input source node. */
struct audio_i2s_in_state {
	/** I2S device, resolved from devicetree by the definition macro. */
	const struct device *dev;
	/**
	 * Receive blocks the driver fills, owned by the definition macro. Per
	 * instance, so two sources never take blocks from the same slab.
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

	/** True once open() has configured the receive direction. */
	bool configured;
	/** True while the receive direction has been started and not stopped. */
	bool started;
	/**
	 * Block taken from @ref slab and not yet handed back, or NULL.
	 *
	 * A block holds whatever the driver was told to receive, which may be
	 * more than one frame carries, so it is drained across several
	 * process() calls and only then returned. It is the node's own property
	 * for as long as it is set here - i2s_read() passed ownership - which is
	 * why close() and every error path release it explicitly.
	 */
	void *block;
	/** Valid bytes in @ref block, as reported by the driver. */
	size_t block_valid;
	/** Bytes of @ref block already widened into frames. */
	size_t block_used;
};

extern const struct audio_node_ops i2s_in_node_ops;

/**
 * @brief Statically define an I2S input source node.
 *
 * File scope only. Allocates the node, its ::audio_i2s_in_state **and its
 * @c k_mem_slab**, so two instances never receive into the same memory.
 * Needs @kconfig{CONFIG_AUDIO_PIPELINE_NODE_I2S_IN}.
 *
 * The device comes from devicetree rather than from a name looked up at run
 * time, so a chain wired to a peripheral the board does not have fails to build
 * instead of failing to start.
 *
 * A source has no upstream, so unlike the sink macro this one takes none.
 *
 * @param _name          Symbol name of the @ref audio_node instance.
 * @param _node_id       Devicetree node identifier of the I2S device, e.g.
 *                       @c DT_ALIAS(i2s_rx). Must be @c okay.
 * @param _frame_samples Frame capacity the pipeline hands this node, in total
 *                       interleaved samples - the same figure passed to
 *                       AUDIO_PIPELINE_DEFINE(). Sizes the receive blocks; a
 *                       block holding more than one frame is drained across
 *                       several frames rather than truncated.
 * @param _blocks        Receive blocks to allocate. The I2S API needs at least
 *                       two per queue; more of them buys tolerance against a
 *                       late consumer - an overrun - at the cost of latency.
 */
#define AUDIO_I2S_IN_NODE_DEFINE(_name, _node_id, _frame_samples, _blocks)                         \
	BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(_node_id),                                            \
		     "AUDIO_I2S_IN_NODE_DEFINE(" #_name "): " #_node_id                            \
		     " is not an enabled devicetree node");                                        \
	BUILD_ASSERT((_frame_samples) >= 2,                                                        \
		     "AUDIO_I2S_IN_NODE_DEFINE(" #_name "): frame_samples is the TOTAL "           \
		     "interleaved sample count and must hold at least one stereo sample "          \
		     "set (>= 2), like AUDIO_PIPELINE_DEFINE()");                                  \
	BUILD_ASSERT((_blocks) >= 2,                                                               \
		     "AUDIO_I2S_IN_NODE_DEFINE(" #_name "): the I2S API needs at least "           \
		     "two receive blocks per queue");                                              \
	K_MEM_SLAB_DEFINE_STATIC(_name##_slab, AUDIO_I2S_BLOCK_BYTES(_frame_samples), (_blocks),   \
				 AUDIO_I2S_BLOCK_ALIGN);                                           \
	static struct audio_i2s_in_state _name##_state = {                                         \
		.dev = DEVICE_DT_GET(_node_id),                                                    \
		.slab = &_name##_slab,                                                             \
		.block_bytes = AUDIO_I2S_BLOCK_BYTES(_frame_samples),                              \
	};                                                                                         \
	AUDIO_NODE_DEFINE(_name, AUDIO_NODE_ROLE_SOURCE, &i2s_in_node_ops, NULL, &_name##_state)

#else /* CONFIG_AUDIO_PIPELINE_NODE_I2S_IN */

#define AUDIO_I2S_IN_NODE_DEFINE(_name, _node_id, _frame_samples, _blocks)                         \
	AUDIO_NODE_UNAVAILABLE(_name, AUDIO_NODE_ROLE_SOURCE, "AUDIO_I2S_IN_NODE_DEFINE",          \
			       "AUDIO_PIPELINE_NODE_I2S_IN")

#endif /* CONFIG_AUDIO_PIPELINE_NODE_I2S_IN */

/* -------------------------------------------------------------------------
 * I2S output sink node
 * -------------------------------------------------------------------------
 */

#ifdef CONFIG_AUDIO_PIPELINE_NODE_I2S_OUT

/**
 * @brief Clock role the sink configures by default: target (slave).
 *
 * A codec that owns MCLK, BCK and LRCK is the common case, and it is the only
 * one AUDIO_I2S_OUT_NODE_DEFINE() produces - a second device driving any of
 * those wires is contention on a strap. Named here rather than left inside the
 * node body so that a board suite can assert the clock role at build time.
 */
#define AUDIO_I2S_OUT_TX_OPTIONS (I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET)

/**
 * @brief Clock role AUDIO_I2S_OUT_CLK_CONTROLLER_NODE_DEFINE() configures.
 *
 * The other case, and it is a property of the *board*, not of the codec: some
 * parts have no clock output at all. The AK4619 is one - its MCLK, BICK and
 * LRCK are input pins and it has no PLL (datasheet p.7) - so on the AKD4619
 * evaluation board the STM32 has to generate all three or nothing is clocked
 * (docs/hardware/akd4619-evaluation-board.md §2). A node that can only be a
 * target cannot be used on such a board at all: the STM32 I2S driver enables
 * its MCLK output only for a direction that is a controller
 * (@c drivers/i2s/i2s_stm32.c), so a target-configured transmitter emits no
 * clock, both blocks then wait on a clock nobody drives, and the symptom is a
 * pipeline that appears to hang.
 *
 * @c I2S_OPT_BIT_CLK_CONTROLLER and @c I2S_OPT_FRAME_CLK_CONTROLLER are zero
 * bits, so "controller" is the *absence* of the target bits rather than a value
 * a node could pass by accident. Spelling it as a named constant is what lets a
 * definition site and a board suite say which role was chosen instead of
 * passing a bare 0 around - and what keeps the choice a definition-time
 * decision rather than a Kconfig symbol, since it belongs to one wiring and not
 * to an image.
 *
 * This is a clock role and nothing else. The node still knows no codec, no
 * register map and no vendor, which is the boundary issue #42 draws.
 */
#define AUDIO_I2S_OUT_TX_CLK_CONTROLLER_OPTIONS                                                    \
	(I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER)

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
	/**
	 * True when open() must configure this direction as the clock
	 * controller (::AUDIO_I2S_OUT_TX_CLK_CONTROLLER_OPTIONS) rather than as
	 * a target (::AUDIO_I2S_OUT_TX_OPTIONS). Owned by the definition macro.
	 *
	 * A @c bool rather than the option bits themselves, and false rather
	 * than true, so that zero keeps its historical meaning: a state left
	 * unset by anything that is not one of the two macros still configures
	 * the target role the sink has always configured, and the controller
	 * role can only be reached by asking for it by name.
	 */
	bool clk_controller;

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
 * This one configures the direction as a clock **target**
 * (::AUDIO_I2S_OUT_TX_OPTIONS). Where the board needs the peripheral to
 * generate the clocks instead, use
 * AUDIO_I2S_OUT_CLK_CONTROLLER_NODE_DEFINE() - same node, same parameters, the
 * other role.
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
	Z_AUDIO_I2S_OUT_NODE_DEFINE(_name, _upstream, _node_id, _frame_samples, _blocks, false,    \
				    "AUDIO_I2S_OUT_NODE_DEFINE")

/**
 * @brief Statically define an I2S output sink node that drives the clocks.
 *
 * Identical to AUDIO_I2S_OUT_NODE_DEFINE() in every respect except the clock
 * role: @c open() configures this direction with
 * ::AUDIO_I2S_OUT_TX_CLK_CONTROLLER_OPTIONS, so the peripheral generates BCK
 * and LRCK - and, where the devicetree node says @c mck-enabled, MCLK.
 *
 * Use it when the part on the other end cannot generate them, which is a fact
 * about the board rather than about this node. Exactly one device may drive a
 * given BCK/LRCK pair: a second controller on the same strap is two push-pull
 * outputs fighting, and the strap, not the software, decides who wins.
 *
 * The parameters are those of AUDIO_I2S_OUT_NODE_DEFINE().
 */
#define AUDIO_I2S_OUT_CLK_CONTROLLER_NODE_DEFINE(_name, _upstream, _node_id, _frame_samples,       \
						 _blocks)                                          \
	Z_AUDIO_I2S_OUT_NODE_DEFINE(_name, _upstream, _node_id, _frame_samples, _blocks, true,     \
				    "AUDIO_I2S_OUT_CLK_CONTROLLER_NODE_DEFINE")

/* Shared body of the two definition macros above; not API. @p _macro is the
 * name the caller used, so a failed assertion names the macro that was written
 * rather than this one.
 */
#define Z_AUDIO_I2S_OUT_NODE_DEFINE(_name, _upstream, _node_id, _frame_samples, _blocks,           \
				    _controller, _macro)                                           \
	BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(_node_id),                                            \
		     _macro "(" #_name "): " #_node_id " is not an enabled devicetree node");      \
	BUILD_ASSERT((_frame_samples) >= 2,                                                        \
		     _macro "(" #_name "): frame_samples is the TOTAL "                            \
			    "interleaved sample count and must hold at least one stereo sample "   \
			    "set (>= 2), like AUDIO_PIPELINE_DEFINE()");                           \
	BUILD_ASSERT((_blocks) >= 2, _macro "(" #_name "): the I2S API needs at least "            \
					    "two transfer blocks per queue");                      \
	K_MEM_SLAB_DEFINE_STATIC(_name##_slab, AUDIO_I2S_BLOCK_BYTES(_frame_samples), (_blocks),   \
				 AUDIO_I2S_BLOCK_ALIGN);                                           \
	static struct audio_i2s_out_state _name##_state = {                                        \
		.dev = DEVICE_DT_GET(_node_id),                                                    \
		.slab = &_name##_slab,                                                             \
		.block_bytes = AUDIO_I2S_BLOCK_BYTES(_frame_samples),                              \
		.clk_controller = (_controller),                                                   \
	};                                                                                         \
	AUDIO_NODE_DEFINE(_name, AUDIO_NODE_ROLE_SINK, &i2s_out_node_ops, (_upstream),             \
			  &_name##_state)

#else /* CONFIG_AUDIO_PIPELINE_NODE_I2S_OUT */

#define AUDIO_I2S_OUT_NODE_DEFINE(_name, _upstream, _node_id, _frame_samples, _blocks)             \
	AUDIO_NODE_UNAVAILABLE(_name, AUDIO_NODE_ROLE_SINK, "AUDIO_I2S_OUT_NODE_DEFINE",           \
			       "AUDIO_PIPELINE_NODE_I2S_OUT")

#define AUDIO_I2S_OUT_CLK_CONTROLLER_NODE_DEFINE(_name, _upstream, _node_id, _frame_samples,       \
						 _blocks)                                          \
	AUDIO_NODE_UNAVAILABLE(_name, AUDIO_NODE_ROLE_SINK,                                        \
			       "AUDIO_I2S_OUT_CLK_CONTROLLER_NODE_DEFINE",                         \
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

/* -------------------------------------------------------------------------
 * Tone analyzer sink node
 * -------------------------------------------------------------------------
 */

/**
 * @brief Tones one tone analyzer definition can name, i.e. the channel range
 *        it can measure.
 *
 * The v1 channel range (spec §5.2), because a definition names exactly one
 * expected frequency per channel, the same pairing the tone generator makes at
 * the other end of the link.
 */
#define AUDIO_TONE_ANALYZER_MAX_TONES 2

/** @brief Channels one tone analyzer can measure - one expected tone each. */
#define AUDIO_TONE_ANALYZER_MAX_CHANNELS AUDIO_TONE_ANALYZER_MAX_TONES

/**
 * @brief Shortest and longest integration window, in samples per channel.
 *
 * The window is what the measurement is averaged over, and both ends of the
 * range are load bearing:
 *
 *  - The floor keeps the bin narrow enough to mean something (a window of N
 *    samples resolves @c sample_rate_hz / N) and it is what makes the
 *    accumulator bound below provable - see AUDIO_TONE_ANALYZER_STATE_MAX.
 *  - The ceiling is what that same bound is computed from. A longer window
 *    would not overflow silently, it would fail the build assertion in
 *    tone_analyzer_node.c.
 */
#define AUDIO_TONE_ANALYZER_MIN_WINDOW 64U
#define AUDIO_TONE_ANALYZER_MAX_WINDOW 4096U

/**
 * @brief Right shift applied to every container sample before it is measured,
 *        and the bound that follows from it.
 *
 * The canonical container is S32_LE carrying an MSB-aligned value (spec §5.3),
 * so the top 16 bits are the sample and what is below them is at least 90 dB
 * under any tone this node is asked about. Measuring the narrowed value keeps
 * every accumulator bound an order of magnitude away from its type's limit,
 * and it costs nothing that a verdict could depend on: the node reports
 * *ratios* of energies, and both sides of every ratio are narrowed alike.
 */
#define AUDIO_TONE_ANALYZER_INPUT_SHIFT 16
#define AUDIO_TONE_ANALYZER_INPUT_MAX   32768

/** @brief Q15 one, i.e. the full-scale value of every ratio reported below. */
#define AUDIO_TONE_ANALYZER_UNITY_Q15 32768

/**
 * @brief In-band energy fraction a channel needs before its tone counts as
 *        present.
 *
 * A pure tone at the expected frequency reads at most
 * ::AUDIO_TONE_ANALYZER_UNITY_Q15, and reaches it only when that frequency
 * falls on a bin centre, i.e. when @c freq_hz * window is a whole multiple of
 * @c sample_rate_hz. Off a bin centre the tone's own negative-frequency image
 * leaks into the bin and the reading falls short by the scalloping loss of the
 * offset - a fraction of a percent for a tone a few bins up, and a couple of
 * percent in the first bin the node accepts at all. White noise of the same
 * total energy reads @c 2/window of it, and a tone at some other frequency
 * reads its leakage into the bin, which is smaller still.
 * Half is therefore not a knife edge between the two - it is the middle of a
 * gap of two orders of magnitude, and it leaves a real signal room for the
 * hum, the noise floor and the quantisation a link adds on the way.
 */
#define AUDIO_TONE_ANALYZER_PASS_Q15 (AUDIO_TONE_ANALYZER_UNITY_Q15 / 2)

/**
 * @brief RMS below which a channel is called silent, in the narrowed 16 bit
 *        domain of ::AUDIO_TONE_ANALYZER_INPUT_SHIFT.
 *
 * -66 dBFS. A muted link reads 0, a live one carrying nothing but its own
 * noise floor reads a few counts, and any stimulus worth measuring reads
 * thousands. Silence is reported as itself rather than as a missing tone
 * because the two have different causes: no clock and no signal against a
 * signal that arrived wrong.
 */
#define AUDIO_TONE_ANALYZER_SILENCE_RMS 16

/**
 * @brief Prediction residual, as a Q15 fraction of the signal's own energy,
 *        below which a channel counts as tonal.
 *
 * Any single sinusoid obeys @c x[n-1] + x[n+1] == 2*cos(w)*x[n] exactly, at
 * every frequency and every phase, so fitting one constant to that relation
 * over the window and looking at what is left over separates "one tone" from
 * "many tones or none" without knowing which tone it is. A sinusoid leaves
 * only its own quantisation behind (parts per billion of its energy);
 * broadband noise leaves about twice its energy. 5 % is between them with
 * seven orders of magnitude to spare on the tonal side.
 */
#define AUDIO_TONE_ANALYZER_TONAL_Q15 (AUDIO_TONE_ANALYZER_UNITY_Q15 / 20)

/** @brief What the analyzer made of the last completed window. */
enum audio_tone_analyzer_verdict {
	/** No window has completed since open(); nothing has been measured. */
	AUDIO_TONE_ANALYZER_VERDICT_NONE = 0,
	/** Every channel carries its own expected tone. */
	AUDIO_TONE_ANALYZER_VERDICT_PASS,
	/** At least one channel carries no signal at all. */
	AUDIO_TONE_ANALYZER_VERDICT_SILENT,
	/** Every channel carries another channel's expected tone. */
	AUDIO_TONE_ANALYZER_VERDICT_SWAPPED,
	/** A tone arrived, but not one that was expected on that channel. */
	AUDIO_TONE_ANALYZER_VERDICT_WRONG_FREQ,
	/** Energy arrived without a tone in it. */
	AUDIO_TONE_ANALYZER_VERDICT_NOISE,
};

/** @brief What one channel looked like over the last completed window. */
struct audio_tone_analyzer_channel_result {
	/**
	 * Energy this channel carried at each of the expected frequencies, as a
	 * Q15 fraction of the channel's total energy over the window
	 * (::AUDIO_TONE_ANALYZER_UNITY_Q15 is all of it).
	 *
	 * Indexed by *tone*, not by channel, and every tone is measured on
	 * every channel: entry @c c of channel @c c is the tone that channel
	 * should carry, and the others are what tells a swapped pair of wires
	 * from a silent one. Reported against the total rather than as an
	 * absolute level on purpose - an absolute magnitude cannot tell a
	 * correct tone from a louder wrong one, and a fraction can.
	 */
	int32_t in_band_q15[AUDIO_TONE_ANALYZER_MAX_TONES];
	/** RMS over the window, in the narrowed 16 bit domain (0..32767). */
	int32_t rms;
	/**
	 * Residual left by the one-sinusoid fit, as a Q15 fraction of the
	 * channel's energy; see ::AUDIO_TONE_ANALYZER_TONAL_Q15. Saturates at
	 * @c INT32_MAX.
	 */
	int32_t residual_q15;
	/**
	 * Expected tone with the largest @ref in_band_q15, or -1 if the channel
	 * carried none of them.
	 */
	int8_t strongest;
	/** True when @ref residual_q15 is at or below the tonal threshold. */
	bool tonal;
	/** True when @ref rms is below ::AUDIO_TONE_ANALYZER_SILENCE_RMS. */
	bool silent;
};

/**
 * @brief The analyzer's verdict and the measurements behind it.
 *
 * Filled by audio_tone_analyzer_get_result(). Everything an application or a
 * test needs is a value here: the node logs a verdict when it changes, but no
 * decision depends on the log.
 */
struct audio_tone_analyzer_result {
	/** Verdict for the last completed window. */
	enum audio_tone_analyzer_verdict verdict;
	/** Windows completed since open(); 0 means nothing was measured yet. */
	uint32_t windows;
	/** Length of the window that was measured, in samples per channel. */
	uint32_t window_samples;
	/** Channels measured, i.e. the bound format's channel count. */
	uint8_t channels;
	/** Expected frequencies the definition named, one per channel. */
	uint8_t tones;
	/** Per-channel measurements, in channel order. */
	struct audio_tone_analyzer_channel_result channel[AUDIO_TONE_ANALYZER_MAX_CHANNELS];
};

#ifdef CONFIG_AUDIO_PIPELINE_NODE_TONE_ANALYZER

/** @brief Per-instance state of the tone analyzer sink node. */
struct audio_tone_analyzer_state {
	/**
	 * Frequency each channel is expected to carry, in Hz and in channel
	 * order, owned by the definition macro. Per channel rather than per
	 * node because that is what makes a channel swap a distinguishable
	 * result instead of an undetected pass.
	 */
	uint32_t freq_hz[AUDIO_TONE_ANALYZER_MAX_TONES];
	/**
	 * Frequencies the definition named, i.e. how many entries of
	 * @ref freq_hz are configuration rather than padding.
	 *
	 * Not a channel count: the channel count is the pipeline's and is read
	 * from @c audio_node.pipeline_format (spec §5.2). This is what open()
	 * checks *against* it.
	 */
	uint8_t tone_count;
	/**
	 * Integration window in samples per channel, owned by the definition
	 * macro; ::AUDIO_TONE_ANALYZER_MIN_WINDOW to
	 * ::AUDIO_TONE_ANALYZER_MAX_WINDOW.
	 */
	uint32_t window_samples;

	/*
	 * Everything below belongs to the node implementation. It is only
	 * meaningful between a successful open() and the matching close(), and
	 * an application must treat it as read-only - @ref result through
	 * audio_tone_analyzer_get_result() rather than by reaching in here.
	 *
	 * There is deliberately no sample rate and no channel count here: they
	 * are pipeline-wide, owned by the pipeline, and read from
	 * audio_node.pipeline_format wherever they are needed (manifest §4).
	 */

	/** Guards @ref result against the reader in another thread. */
	struct k_spinlock lock;
	/** Last completed window, published under @ref lock. */
	struct audio_tone_analyzer_result result;
	/** 2*cos(w) per expected tone in Q24, derived from the bound rate. */
	int32_t coeff_q24[AUDIO_TONE_ANALYZER_MAX_TONES];
	/** Goertzel state, one recurrence per channel and expected tone. */
	int64_t s1[AUDIO_TONE_ANALYZER_MAX_CHANNELS][AUDIO_TONE_ANALYZER_MAX_TONES];
	int64_t s2[AUDIO_TONE_ANALYZER_MAX_CHANNELS][AUDIO_TONE_ANALYZER_MAX_TONES];
	/** Sum of x[n]^2 over the window, per channel. */
	int64_t energy[AUDIO_TONE_ANALYZER_MAX_CHANNELS];
	/** One-sinusoid fit: sums of x*x, y*y and x*y with y[n] = x[n-1]+x[n+1]. */
	int64_t fit_xx[AUDIO_TONE_ANALYZER_MAX_CHANNELS];
	int64_t fit_yy[AUDIO_TONE_ANALYZER_MAX_CHANNELS];
	int64_t fit_xy[AUDIO_TONE_ANALYZER_MAX_CHANNELS];
	/** The two previous samples of each channel, for the fit above. */
	int32_t prev1[AUDIO_TONE_ANALYZER_MAX_CHANNELS];
	int32_t prev2[AUDIO_TONE_ANALYZER_MAX_CHANNELS];
	/** Samples of each channel already in @ref prev1 / @ref prev2, up to 2. */
	uint8_t history[AUDIO_TONE_ANALYZER_MAX_CHANNELS];
	/** Sample sets folded into the window so far. */
	uint32_t filled;
	/** Position inside the interleaved sample set, carried across frames. */
	uint8_t channel_pos;
	/** True between a successful open() and its close(). */
	bool is_open;
};

extern const struct audio_node_ops tone_analyzer_node_ops;

/**
 * @brief Read the analyzer's verdict and the measurements behind it.
 *
 * The result is the node's product, so it is a value an application reads
 * rather than a line it has to parse out of a log: the loopback application
 * needs the verdict to decide, and a Ztest needs it to assert.
 *
 * Safe to call from any thread and at any time, including while the pipeline
 * is running: the node publishes a completed window under a spinlock and this
 * copies it out under the same one, so a reader never sees half of one window
 * and half of the next. It is the only part of the node that is not confined
 * to the pipeline thread (spec §3.3); the three ops still are.
 *
 * @param node   Node defined with AUDIO_TONE_ANALYZER_NODE_DEFINE().
 * @param result Filled with the last completed window. Before the first one
 *               completes this reports ::AUDIO_TONE_ANALYZER_VERDICT_NONE and
 *               a @c windows count of 0.
 *
 * @retval 0 on success
 * @retval -EINVAL if @p node or @p result is NULL, or @p node is not a tone
 *         analyzer
 */
int audio_tone_analyzer_get_result(const struct audio_node *node,
				   struct audio_tone_analyzer_result *result);

/**
 * @brief Statically define a tone analyzer sink node.
 *
 * File scope only. Allocates the node and its ::audio_tone_analyzer_state, so
 * two analyzers share no accumulator and no verdict.
 * Needs @kconfig{CONFIG_AUDIO_PIPELINE_NODE_TONE_ANALYZER}.
 *
 * The frequencies are variadic and there is one per channel, in channel order,
 * exactly as in AUDIO_TONE_GEN_NODE_DEFINE(): the two macros describe the two
 * ends of the same link, and open() refuses a pipeline whose channel count says
 * otherwise rather than dropping or inventing an expectation.
 *
 * The window is a definition-time figure rather than a Kconfig symbol because
 * it is a property of what is being measured, not of the image: it sets the bin
 * width (@c sample_rate_hz / @p _window_samples) and therefore how far apart
 * two frequencies have to be before the analyzer can tell them apart. Pick one
 * where the expected frequencies land on whole bins - 960 samples at 48 kHz
 * puts 1 kHz on bin 20 and 3 kHz on bin 60 - and the measurement is exactly
 * offset invariant instead of merely nearly so.
 *
 * @param _name           Symbol name of the @ref audio_node instance.
 * @param _upstream       Pointer to the upstream node.
 * @param _window_samples Integration window in samples per channel,
 *                        ::AUDIO_TONE_ANALYZER_MIN_WINDOW to
 *                        ::AUDIO_TONE_ANALYZER_MAX_WINDOW.
 * @param ...             One expected frequency in Hz per channel, at most
 *                        ::AUDIO_TONE_ANALYZER_MAX_TONES of them.
 */
#define AUDIO_TONE_ANALYZER_NODE_DEFINE(_name, _upstream, _window_samples, ...)                    \
	BUILD_ASSERT(NUM_VA_ARGS(__VA_ARGS__) >= 1 &&                                              \
			     NUM_VA_ARGS(__VA_ARGS__) <= AUDIO_TONE_ANALYZER_MAX_TONES,            \
		     "AUDIO_TONE_ANALYZER_NODE_DEFINE() takes one expected frequency per "         \
		     "channel");                                                                   \
	BUILD_ASSERT((_window_samples) >= AUDIO_TONE_ANALYZER_MIN_WINDOW &&                        \
			     (_window_samples) <= AUDIO_TONE_ANALYZER_MAX_WINDOW,                  \
		     "AUDIO_TONE_ANALYZER_NODE_DEFINE(" #_name "): the window is outside the "     \
		     "range the accumulator bound is proved for");                                 \
	static struct audio_tone_analyzer_state _name##_state = {                                  \
		.freq_hz = {__VA_ARGS__},                                                          \
		.tone_count = NUM_VA_ARGS(__VA_ARGS__),                                            \
		.window_samples = (_window_samples),                                               \
	};                                                                                         \
	AUDIO_NODE_DEFINE(_name, AUDIO_NODE_ROLE_SINK, &tone_analyzer_node_ops, (_upstream),       \
			  &_name##_state)

#else /* CONFIG_AUDIO_PIPELINE_NODE_TONE_ANALYZER */

#define AUDIO_TONE_ANALYZER_NODE_DEFINE(_name, _upstream, _window_samples, ...)                    \
	AUDIO_NODE_UNAVAILABLE(_name, AUDIO_NODE_ROLE_SINK, "AUDIO_TONE_ANALYZER_NODE_DEFINE",     \
			       "AUDIO_PIPELINE_NODE_TONE_ANALYZER")

#endif /* CONFIG_AUDIO_PIPELINE_NODE_TONE_ANALYZER */

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
