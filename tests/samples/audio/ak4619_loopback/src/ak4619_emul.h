/*
 * Test-side control of the emulated AK4619 - see ak4619_emul.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_AUDIO_TOOLKIT_TESTS_AK4619_EMUL_H_
#define ZEPHYR_AUDIO_TOOLKIT_TESTS_AK4619_EMUL_H_

#include <stdint.h>

#include <zephyr/drivers/emul.h>

/** Ways the emulated part can be made to lie, all of them things a real bus does. */
enum ak4619_emul_fault {
	/** Behaves like a healthy AK4619. */
	AK4619_EMUL_FAULT_NONE,
	/** ACKs every byte and throws the writes away: the "ACK into the void" case. */
	AK4619_EMUL_FAULT_IGNORE_WRITES,
	/** Reads return 0xFF, which is what an idle open-drain bus reads as. */
	AK4619_EMUL_FAULT_READ_ONES,
	/** Nothing on the bus answers at all. */
	AK4619_EMUL_FAULT_NACK,
};

/** Select the emulated part's behaviour. */
void ak4619_emul_set_fault(const struct emul *target, enum ak4619_emul_fault fault);

/** Refill the register file with the scribble pattern an untouched part holds. */
void ak4619_emul_scribble(const struct emul *target);

/** Read a register out of band, without going through the bus. */
int ak4619_emul_peek(const struct emul *target, uint8_t reg, uint8_t *val);

#endif /* ZEPHYR_AUDIO_TOOLKIT_TESTS_AK4619_EMUL_H_ */
