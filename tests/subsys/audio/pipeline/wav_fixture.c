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

#include <zephyr/audio/audio_wav.h>

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
	/* Canonical header plus the largest payload any test needs. */
	uint8_t buf[AUDIO_WAV_MIN_HEADER_SIZE + 1024];
	const struct audio_wav_header hdr = {
		.sample_rate_hz = (spec->sample_rate_hz != 0U) ? spec->sample_rate_hz : 48000U,
		.data_size = (spec->declared_data_size != 0U) ? spec->declared_data_size
							     : (uint32_t)spec->payload_len,
		.format_tag = (spec->format_tag != 0U) ? spec->format_tag : AUDIO_WAV_FORMAT_PCM,
		.channels = (spec->channels != 0U) ? spec->channels : 2U,
		.bits_per_sample = (spec->bits_per_sample != 0U) ? spec->bits_per_sample : 16U,
	};
	size_t len = AUDIO_WAV_MIN_HEADER_SIZE + spec->payload_len;
	int ret;

	if (len > sizeof(buf)) {
		return -ENOMEM;
	}

	/* The fixture describes the file it wants and lets the WAV module lay
	 * out the bytes, so a test file is exactly what the subsystem itself
	 * would have produced - down to the last field.
	 */
	ret = audio_wav_write_header(buf, sizeof(buf), &hdr);
	if (ret < 0) {
		return ret;
	}

	if (spec->payload_len > 0U) {
		memcpy(&buf[AUDIO_WAV_MIN_HEADER_SIZE], spec->payload, spec->payload_len);
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
