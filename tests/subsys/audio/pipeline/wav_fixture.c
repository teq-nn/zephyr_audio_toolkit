/*
 * Implementation of the filesystem / WAV fixture. See wav_fixture.h for why
 * ext2 on a RAM disk is the backing store.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "wav_fixture.h"

/* Disk name of the ramdisk0 node in app.overlay. */
#define AUDIO_TEST_DISK_NAME "RAM"

static struct fs_mount_t audio_test_mnt = {
	.type = FS_EXT2,
	.mnt_point = AUDIO_TEST_MOUNT_POINT,
	.storage_dev = (void *)AUDIO_TEST_DISK_NAME,
	/* No FS_MOUNT_FLAG_NO_FORMAT: the RAM disk comes up zeroed, so the
	 * first mount formats it (CONFIG_FILE_SYSTEM_MKFS).
	 */
	.flags = 0,
};

int audio_test_fs_mount(void)
{
	static bool mounted;
	int ret;

	if (mounted) {
		return 0;
	}

	ret = fs_mount(&audio_test_mnt);
	if (ret < 0) {
		return ret;
	}

	mounted = true;

	return 0;
}

int audio_test_write_raw(const char *path, const void *data, size_t len)
{
	struct fs_file_t file;
	ssize_t written;
	int ret;

	fs_file_t_init(&file);

	ret = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (ret < 0) {
		return ret;
	}

	written = (len > 0U) ? fs_write(&file, data, len) : 0;

	ret = fs_close(&file);
	if (written < 0) {
		return (int)written;
	}
	if ((size_t)written != len) {
		return -EIO;
	}

	return ret;
}

int audio_test_write_wav(const char *path, const struct audio_test_wav_spec *spec)
{
	/* 44 byte canonical header plus the largest payload any test needs. */
	uint8_t buf[44 + 1024];
	uint16_t format_tag = (spec->format_tag != 0U) ? spec->format_tag : 1U;
	uint16_t channels = (spec->channels != 0U) ? spec->channels : 2U;
	uint32_t rate = (spec->sample_rate_hz != 0U) ? spec->sample_rate_hz : 48000U;
	uint16_t bits = (spec->bits_per_sample != 0U) ? spec->bits_per_sample : 16U;
	uint32_t declared = (spec->declared_data_size != 0U) ? spec->declared_data_size
							     : (uint32_t)spec->payload_len;
	uint16_t block_align = (uint16_t)(channels * (bits / 8U));
	size_t len = 44U + spec->payload_len;

	if (len > sizeof(buf)) {
		return -ENOMEM;
	}

	memcpy(&buf[0], "RIFF", 4);
	sys_put_le32((uint32_t)(36U + declared), &buf[4]);
	memcpy(&buf[8], "WAVE", 4);

	memcpy(&buf[12], "fmt ", 4);
	sys_put_le32(16U, &buf[16]);
	sys_put_le16(format_tag, &buf[20]);
	sys_put_le16(channels, &buf[22]);
	sys_put_le32(rate, &buf[24]);
	sys_put_le32(rate * block_align, &buf[28]);
	sys_put_le16(block_align, &buf[32]);
	sys_put_le16(bits, &buf[34]);

	memcpy(&buf[36], "data", 4);
	sys_put_le32(declared, &buf[40]);

	if (spec->payload_len > 0U) {
		memcpy(&buf[44], spec->payload, spec->payload_len);
	}

	return audio_test_write_raw(path, buf, len);
}

int audio_test_write_wav_s16(const char *path, const int16_t *samples, size_t count)
{
	uint8_t payload[512];
	struct audio_test_wav_spec spec = {
		.payload = payload,
		.payload_len = count * sizeof(int16_t),
	};
	size_t i;

	if (spec.payload_len > sizeof(payload)) {
		return -ENOMEM;
	}

	/* Serialise explicitly so the fixture produces a little endian file on
	 * a big endian host as well.
	 */
	for (i = 0; i < count; i++) {
		sys_put_le16((uint16_t)samples[i], &payload[i * sizeof(int16_t)]);
	}

	return audio_test_write_wav(path, &spec);
}
