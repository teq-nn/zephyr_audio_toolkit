/*
 * Scriptable I2S controller for the input source suite; see fake_i2s.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT vnd_i2s_fake

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "fake_i2s.h"

struct fake_i2s_data *fake_i2s_data_get(const struct device *dev)
{
	return (struct fake_i2s_data *)dev->data;
}

void fake_i2s_reset(const struct device *dev)
{
	struct fake_i2s_data *data = fake_i2s_data_get(dev);

	memset(data, 0, sizeof(*data));
	data->state = I2S_STATE_NOT_READY;
}

static int fake_i2s_configure(const struct device *dev, enum i2s_dir dir,
			      const struct i2s_config *cfg)
{
	struct fake_i2s_data *data = fake_i2s_data_get(dev);

	data->configures++;

	/* The node only ever configures the direction it uses; a suite that
	 * saw the other one would be watching the wrong node.
	 */
	if (dir != I2S_DIR_RX) {
		return -ENOSYS;
	}

	if (data->configure_ret != 0) {
		/* A refused configuration changes nothing, so a node that
		 * ignored the error would find a device it cannot read from.
		 */
		return data->configure_ret;
	}

	data->cfg = *cfg;
	data->configured = true;
	data->state = I2S_STATE_READY;

	return 0;
}

static const struct i2s_config *fake_i2s_config_get(const struct device *dev, enum i2s_dir dir)
{
	struct fake_i2s_data *data = fake_i2s_data_get(dev);

	if (dir != I2S_DIR_RX || !data->configured) {
		return NULL;
	}

	return &data->cfg;
}

static int fake_i2s_trigger(const struct device *dev, enum i2s_dir dir, enum i2s_trigger_cmd cmd)
{
	struct fake_i2s_data *data = fake_i2s_data_get(dev);

	if (dir != I2S_DIR_RX) {
		return -ENOSYS;
	}

	switch (cmd) {
	case I2S_TRIGGER_START:
		data->starts++;
		/* START is legal from READY only. */
		if (data->state != I2S_STATE_READY) {
			return -EIO;
		}
		data->state = I2S_STATE_RUNNING;
		return 0;
	case I2S_TRIGGER_STOP:
	case I2S_TRIGGER_DRAIN:
		data->stops++;
		if (data->state != I2S_STATE_RUNNING) {
			return -EIO;
		}
		data->state = I2S_STATE_READY;
		return 0;
	case I2S_TRIGGER_DROP:
		data->drops++;
		/* Legal from every state but NOT_READY, which is what lets one
		 * close() path serve a running stream and an overrun one.
		 */
		if (data->state == I2S_STATE_NOT_READY) {
			return -EIO;
		}
		data->state = I2S_STATE_READY;
		return 0;
	case I2S_TRIGGER_PREPARE:
		data->prepares++;
		/* Legal from ERROR only, which is what makes its return value a
		 * usable test for "was this an overrun?".
		 */
		if (data->state != I2S_STATE_ERROR) {
			return -EIO;
		}
		data->state = I2S_STATE_READY;
		/* Recovering clears the overrun; the next read delivers again. */
		data->read_ret = 0;
		data->read_overruns = false;
		return 0;
	default:
		return -EINVAL;
	}
}

static int fake_i2s_read(const struct device *dev, void **mem_block, size_t *size)
{
	struct fake_i2s_data *data = fake_i2s_data_get(dev);
	uint8_t *block;
	size_t bytes;
	size_t i;
	int ret;

	data->reads++;

	if (data->state == I2S_STATE_NOT_READY) {
		return -EIO;
	}

	if (data->read_ret != 0) {
		int scripted = data->read_ret;

		if (data->read_overruns) {
			/* An overrun is a state: every further read is refused
			 * until the direction is prepared.
			 */
			data->state = I2S_STATE_ERROR;
		}

		return scripted;
	}

	if (data->state != I2S_STATE_RUNNING) {
		/* Reading a direction nobody started delivers nothing; a node
		 * that skipped its START would see this rather than audio.
		 */
		return -EIO;
	}

	/* K_NO_WAIT on purpose: the slab is the node's, and a node that failed
	 * to hand a block back must fail the suite instead of blocking it.
	 */
	ret = k_mem_slab_alloc(data->cfg.mem_slab, (void **)&block, K_NO_WAIT);
	if (ret < 0) {
		return -ENOMEM;
	}

	bytes = (data->read_bytes != 0U) ? data->read_bytes : data->cfg.block_size;
	bytes = MIN(bytes, data->cfg.block_size);

	for (i = 0; i + sizeof(uint16_t) <= bytes; i += sizeof(uint16_t)) {
		sys_put_le16(data->next_word, &block[i]);
		data->next_word++;
	}

	*mem_block = block;
	*size = bytes;

	return 0;
}

static int fake_i2s_write(const struct device *dev, void *mem_block, size_t size)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(mem_block);
	ARG_UNUSED(size);

	/* This device exists for the input source; nothing here transmits. */
	return -ENOSYS;
}

static DEVICE_API(i2s, fake_i2s_api) = {
	.configure = fake_i2s_configure,
	.config_get = fake_i2s_config_get,
	.trigger = fake_i2s_trigger,
	.read = fake_i2s_read,
	.write = fake_i2s_write,
};

static int fake_i2s_init(const struct device *dev)
{
	fake_i2s_reset(dev);

	return 0;
}

#define FAKE_I2S_DEFINE(inst)                                                                      \
	static struct fake_i2s_data fake_i2s_data_##inst;                                          \
	DEVICE_DT_INST_DEFINE(inst, fake_i2s_init, NULL, &fake_i2s_data_##inst, NULL, POST_KERNEL, \
			      CONFIG_I2S_INIT_PRIORITY, &fake_i2s_api);

DT_INST_FOREACH_STATUS_OKAY(FAKE_I2S_DEFINE)
