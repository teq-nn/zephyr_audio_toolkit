/*
 * Scriptable I2S controller for the input source suite.
 *
 * The real answers an I2S source has to survive - a read that times out, a
 * driver that fails, an overrun that parks the direction until it is prepared -
 * are states of the device, not of the node, so they can only be exercised by a
 * device that can be put into them on purpose. This is that device: every
 * operation of the API, a state machine that follows the one <zephyr/drivers/
 * i2s.h> documents, and a script the test writes before each case.
 *
 * It allocates the blocks it hands out from the slab the node configured, with
 * K_NO_WAIT: a node that leaks a block therefore fails the very next read with
 * -ENOMEM instead of blocking the suite forever.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AUDIO_TEST_FAKE_I2S_H_
#define AUDIO_TEST_FAKE_I2S_H_

#include <stdbool.h>
#include <stddef.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/types.h>

/** Everything one fake controller knows: its script, its log and its state. */
struct fake_i2s_data {
	/*
	 * Script - what the device does next. Written by the test.
	 */

	/** Returned by every configure() while non-zero. */
	int configure_ret;
	/** Returned by every read() while non-zero, instead of a block. */
	int read_ret;
	/**
	 * True when @ref read_ret is an overrun, i.e. when the failing read
	 * also parks the direction in @c I2S_STATE_ERROR. A PREPARE then clears
	 * both, exactly as recovering on real hardware does.
	 */
	bool read_overruns;
	/** Bytes one read() reports; 0 means the whole configured block. */
	size_t read_bytes;
	/** Value of the next wire word the device produces; counts up. */
	uint16_t next_word;

	/*
	 * Log - what the node did. Read by the test.
	 */

	uint32_t configures;
	uint32_t reads;
	uint32_t starts;
	uint32_t stops;
	uint32_t drops;
	uint32_t prepares;

	/*
	 * Device state, following the API's own state machine.
	 */

	/** Configuration of the receive direction, as the node passed it. */
	struct i2s_config cfg;
	/** True once configure() has succeeded at least once. */
	bool configured;
	/** Current state of the receive direction. */
	enum i2s_state state;
};

/** @brief The scriptable state of @p dev. */
struct fake_i2s_data *fake_i2s_data_get(const struct device *dev);

/** @brief Put @p dev back into its power-on state with an empty script. */
void fake_i2s_reset(const struct device *dev);

#endif /* AUDIO_TEST_FAKE_I2S_H_ */
