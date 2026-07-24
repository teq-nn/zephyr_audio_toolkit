/*
 * Test fixture: a writable filesystem on native_sim plus a RIFF/WAVE file
 * generator, so the file nodes can be exercised against real files
 * (spec §12.1 - everything has to run headless, without audio hardware).
 *
 * The filesystem is ext2 on a RAM disk. Both are in-tree Zephyr code, so the
 * suite needs no extra west module: FAT would pull in `fatfs` and littlefs the
 * `littlefs` module, neither of which is in this repository's west manifest.
 * The RAM disk itself is declared in app.overlay.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AUDIO_PIPELINE_TEST_WAV_FIXTURE_H_
#define AUDIO_PIPELINE_TEST_WAV_FIXTURE_H_

#include <stddef.h>

#include <zephyr/types.h>

/** Mount point of the fixture filesystem; prefix every fixture path with it. */
#define AUDIO_TEST_MOUNT_POINT "/ram"

/** Convenience: build an absolute fixture path from a bare file name. */
#define AUDIO_TEST_PATH(_name) AUDIO_TEST_MOUNT_POINT "/" _name

/** Description of the WAV file to generate. Zero fields take their default. */
struct audio_test_wav_spec {
	/** WAVE format tag; 0 selects PCM (1). */
	uint16_t format_tag;
	/** Channel count; 0 selects 2 (stereo, the v1 pipeline format). */
	uint16_t channels;
	/** Sample rate in Hz; 0 selects 48000. */
	uint32_t sample_rate_hz;
	/** On-disk bit depth; 0 selects 16. */
	uint16_t bits_per_sample;
	/**
	 * Value written into the size field of the ``data`` chunk. 0 selects
	 * @ref payload_len, i.e. an honest header. Set it larger to emulate a
	 * file that promises more payload than it carries - the parser does not
	 * cross-check this, so the reader has to treat the short read as EOF.
	 */
	uint32_t declared_data_size;
	/** Payload bytes, appended verbatim after the ``data`` chunk header. */
	const void *payload;
	/** Number of bytes at @ref payload. */
	size_t payload_len;
};

/**
 * @brief Mount the fixture filesystem, formatting it on first use.
 *
 * Idempotent, so every suite that needs files can simply call it from its
 * @c before hook.
 *
 * @retval 0 on success, a negative errno otherwise.
 */
int audio_test_fs_mount(void);

/** @brief Write @p len bytes verbatim to @p path, truncating an older file. */
int audio_test_write_raw(const char *path, const void *data, size_t len);

/** @brief Write a RIFF/WAVE file described by @p spec to @p path. */
int audio_test_write_wav(const char *path, const struct audio_test_wav_spec *spec);

/**
 * @brief Write a 16 bit PCM WAVE file holding @p count interleaved samples.
 *
 * Shorthand for the common case: PCM, stereo, 48 kHz, honest header.
 */
int audio_test_write_wav_s16(const char *path, const int16_t *samples, size_t count);

#endif /* AUDIO_PIPELINE_TEST_WAV_FIXTURE_H_ */
