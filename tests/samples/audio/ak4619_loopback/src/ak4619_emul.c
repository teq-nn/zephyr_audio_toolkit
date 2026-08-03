/*
 * An emulated AK4619 on the native_sim I2C bus.
 *
 * It models what the driver actually depends on and nothing else: the 0x00-
 * 0x14 register file, the two-byte write and random-address read of datasheet
 * pp.57-58, and the internal address counter that auto-increments and rolls
 * over past 0x14. On top of that it can be told to misbehave, which is how the
 * "an ACK is not proof" half of issue #45 gets tested on a host.
 *
 * Deliberately NOT modelled: the reset defaults at power-on. The register file
 * comes up filled with a scribble pattern instead, so a test that finds the
 * datasheet defaults after init has watched the driver put them there. The
 * situation it stands for is real - a warm MCU restart against a codec that is
 * still powered and still configured from the previous run.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT asahi_kasei_ak4619

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/sys/util.h>

#include "ak4619_emul.h"

/* The register file the driver may touch, 0x00..0x14 (datasheet p.60). */
#define AK4619_EMUL_REG_COUNT 0x15U

/* What an untouched register file holds - see the file comment. */
#define AK4619_EMUL_SCRIBBLE 0x5AU

/*
 * The address the overlay gives the node that stands in for a part which is
 * not there. Keying on the address rather than on a devicetree property keeps
 * both nodes on the production binding, which is the point: the driver must
 * not need a test-only property to be testable.
 */
#define AK4619_EMUL_ABSENT_ADDR 0x11U

struct ak4619_emul_data {
	uint8_t regs[AK4619_EMUL_REG_COUNT];
	uint8_t addr_counter;
	enum ak4619_emul_fault fault;
	bool absent;
};

static struct ak4619_emul_data *emul_data(const struct emul *target)
{
	return target->data;
}

void ak4619_emul_set_fault(const struct emul *target, enum ak4619_emul_fault fault)
{
	emul_data(target)->fault = fault;
}

void ak4619_emul_scribble(const struct emul *target)
{
	struct ak4619_emul_data *data = emul_data(target);

	memset(data->regs, AK4619_EMUL_SCRIBBLE, sizeof(data->regs));
}

int ak4619_emul_peek(const struct emul *target, uint8_t reg, uint8_t *val)
{
	if (reg >= AK4619_EMUL_REG_COUNT) {
		return -EINVAL;
	}

	*val = emul_data(target)->regs[reg];

	return 0;
}

/* The internal counter rolls over past 0x14 rather than stalling (p.57). */
static void advance(struct ak4619_emul_data *data)
{
	data->addr_counter = (data->addr_counter + 1U) % AK4619_EMUL_REG_COUNT;
}

static int ak4619_emul_write_msg(struct ak4619_emul_data *data, const struct i2c_msg *msg)
{
	if (msg->len == 0U) {
		return 0;
	}

	/*
	 * The first byte of a write is always the sub-address, whose top bit
	 * is fixed at zero (p.57, Figures 36 and 38); everything after it is
	 * data, walking the counter.
	 */
	data->addr_counter = msg->buf[0] & 0x7FU;

	if (data->addr_counter >= AK4619_EMUL_REG_COUNT) {
		/* Writing 0x15..0x7F is prohibited; the driver must not. */
		return -EIO;
	}

	for (uint32_t i = 1U; i < msg->len; i++) {
		if (data->fault != AK4619_EMUL_FAULT_IGNORE_WRITES) {
			data->regs[data->addr_counter] = msg->buf[i];
		}
		advance(data);
	}

	return 0;
}

static int ak4619_emul_read_msg(struct ak4619_emul_data *data, struct i2c_msg *msg)
{
	for (uint32_t i = 0; i < msg->len; i++) {
		if (data->fault == AK4619_EMUL_FAULT_READ_ONES) {
			msg->buf[i] = 0xFFU;
		} else {
			msg->buf[i] = data->regs[data->addr_counter];
		}
		advance(data);
	}

	return 0;
}

static int ak4619_emul_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs,
				int addr)
{
	struct ak4619_emul_data *data = emul_data(target);

	ARG_UNUSED(addr);

	if (data->absent || data->fault == AK4619_EMUL_FAULT_NACK) {
		/* No ACK: exactly what an unpowered board looks like. */
		return -EIO;
	}

	for (int i = 0; i < num_msgs; i++) {
		int ret;

		if ((msgs[i].flags & I2C_MSG_READ) != 0U) {
			ret = ak4619_emul_read_msg(data, &msgs[i]);
		} else {
			ret = ak4619_emul_write_msg(data, &msgs[i]);
		}

		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

static const struct i2c_emul_api ak4619_emul_api = {
	.transfer = ak4619_emul_transfer,
};

static int ak4619_emul_init(const struct emul *target, const struct device *parent)
{
	struct ak4619_emul_data *data = emul_data(target);

	ARG_UNUSED(parent);

	data->absent = (target->bus.i2c->addr == AK4619_EMUL_ABSENT_ADDR);
	data->fault = AK4619_EMUL_FAULT_NONE;
	data->addr_counter = 0U;
	ak4619_emul_scribble(target);

	return 0;
}

#define AK4619_EMUL_DEFINE(inst)                                                                   \
	static struct ak4619_emul_data ak4619_emul_data_##inst;                                    \
	EMUL_DT_INST_DEFINE(inst, ak4619_emul_init, &ak4619_emul_data_##inst, NULL,                \
			    &ak4619_emul_api, NULL)

DT_INST_FOREACH_STATUS_OKAY(AK4619_EMUL_DEFINE)
