/*
 * Reference application: a fully statically defined pipeline reading a real
 * WAV file.
 *
 * Nothing in the pipeline is hand-built - AUDIO_PIPELINE_DEFINE() owns the
 * worker thread, its stack and the frame buffer, and the *_NODE_DEFINE() macros
 * own the nodes plus their state and wire the chain at build time.
 *
 * The sample is self-contained on native_sim: it formats an ext2 filesystem on
 * a RAM disk, generates a short 16 bit stereo track on it and plays that
 * through the gain filter into the null sink until the pipeline reports EOF.
 * On real hardware, drop prepare_track() and mount the storage that actually
 * holds the file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zephyr/audio/audio_node.h>
#include <zephyr/audio/audio_nodes.h>
#include <zephyr/audio/audio_pipeline.h>
#include <zephyr/audio/audio_pipeline_events.h>
#include <zephyr/audio/audio_wav.h>

#define TRACK_MOUNT_POINT "/ram"
#define TRACK_PATH TRACK_MOUNT_POINT "/track.wav"

#define TRACK_SAMPLE_RATE_HZ 48000U
#define TRACK_CHANNELS 2U
#define TRACK_BITS_PER_SAMPLE 16U
/* 10 ms of stereo audio. */
#define TRACK_FRAMES 480U

/* Nodes are defined in dataflow order, so each one can point at the node
 * feeding it: reader -> gain (-6 dB) -> null sink.
 */
AUDIO_FILE_READER_NODE_DEFINE(reader, TRACK_PATH);
AUDIO_GAIN_FILTER_NODE_DEFINE(gain, &reader, AUDIO_GAIN_UNITY_Q15 / 2);
AUDIO_NULL_SINK_NODE_DEFINE(sink, &gain);

AUDIO_PIPELINE_DEFINE(pipeline, CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES,
		      CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE, CONFIG_AUDIO_PIPELINE_THREAD_PRIO);

static void print_event(const char *path, const struct audio_pipeline_event *event)
{
	printk("pipeline[%s]: ", path);

	switch (event->type) {
	case AUDIO_PIPELINE_EVENT_EOF:
		printk("EOF\n");
		break;
	case AUDIO_PIPELINE_EVENT_ERROR:
		printk("error %d\n", event->err);
		break;
	case AUDIO_PIPELINE_EVENT_RECONFIG:
		printk("reconfig\n");
		break;
	default:
		printk("unknown event\n");
		break;
	}
}

/*
 * Optional secondary path. It runs on the thread that produced the
 * event - the worker thread for EOF - so it only prints and returns; anything
 * that may block belongs on the control thread, behind the queue read in
 * main().
 */
static void pipeline_event_handler(const struct audio_pipeline_event *event, void *user_data)
{
	ARG_UNUSED(user_data);

	if (!event) {
		return;
	}

	print_event("callback", event);
}

/*
 * Everything below only exists so the sample has a file to read: it formats a
 * RAM disk on first mount and writes a canonical 44 byte RIFF/WAVE header
 * followed by a 16 bit stereo ramp.
 */
static struct fs_mount_t track_mnt = {
	.type = FS_EXT2,
	.mnt_point = TRACK_MOUNT_POINT,
	/* Disk name of the ramdisk0 node in app.overlay. */
	.storage_dev = (void *)"RAM",
	/* No FS_MOUNT_FLAG_NO_FORMAT: the RAM disk comes up zeroed, so this
	 * first mount formats it (CONFIG_FILE_SYSTEM_MKFS).
	 */
	.flags = 0,
};

static int prepare_track(void)
{
	uint8_t hdr[AUDIO_WAV_MIN_HEADER_SIZE];
	uint8_t block[TRACK_CHANNELS * sizeof(int16_t) * 32U];
	struct fs_file_t file;
	uint32_t data_size = TRACK_FRAMES * TRACK_CHANNELS * (TRACK_BITS_PER_SAMPLE / 8U);
	const struct audio_wav_header wav = {
		.sample_rate_hz = TRACK_SAMPLE_RATE_HZ,
		/* The whole track is generated below, so its size is known
		 * before the first byte goes out and the header needs no
		 * back-patching.
		 */
		.data_size = data_size,
		.format_tag = AUDIO_WAV_FORMAT_PCM,
		.channels = TRACK_CHANNELS,
		.bits_per_sample = TRACK_BITS_PER_SAMPLE,
	};
	uint32_t frame = 0;
	ssize_t written;
	int ret;

	ret = fs_mount(&track_mnt);
	if (ret < 0) {
		printk("track: mount failed (%d)\n", ret);
		return ret;
	}

	/* The header comes from the same module the file reader parses with,
	 * so a sample application never spells out the RIFF/WAVE layout.
	 */
	ret = audio_wav_write_header(hdr, sizeof(hdr), &wav);
	if (ret < 0) {
		printk("track: header rejected (%d)\n", ret);
		return ret;
	}

	fs_file_t_init(&file);

	ret = fs_open(&file, TRACK_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (ret < 0) {
		printk("track: open failed (%d)\n", ret);
		return ret;
	}

	written = fs_write(&file, hdr, sizeof(hdr));
	while (written >= 0 && frame < TRACK_FRAMES) {
		size_t i = 0;

		while (i < sizeof(block) && frame < TRACK_FRAMES) {
			/* A triangle sweep, so the gain filter has something
			 * other than silence to scale.
			 */
			int16_t sample = (int16_t)((frame % 256U) * 128) - 16384;

			sys_put_le16((uint16_t)sample, &block[i]);
			sys_put_le16((uint16_t)-sample, &block[i + sizeof(int16_t)]);
			i += TRACK_CHANNELS * sizeof(int16_t);
			frame++;
		}

		written = fs_write(&file, block, i);
	}

	ret = fs_close(&file);
	if (written < 0) {
		printk("track: write failed (%d)\n", (int)written);
		return (int)written;
	}

	printk("track: wrote %u payload bytes to %s\n", data_size, TRACK_PATH);

	return ret;
}

static const struct audio_pipeline_config cfg = {
	.stream = {
		.sample_rate_hz = TRACK_SAMPLE_RATE_HZ,
		.channels = TRACK_CHANNELS,
		/* The container is 32 bit, the track's resolution is 16. */
		.valid_bits_per_sample = TRACK_BITS_PER_SAMPLE,
		.format = AUDIO_SAMPLE_FORMAT_S32_LE,
	},
	/* Same frame size the pipeline was defined with. */
	.frame_samples = CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES,
	.event_cb = pipeline_event_handler,
	.event_user_data = NULL,
};

void main(void)
{
	struct audio_pipeline_event event;
	int ret;

	/* Give the file reader source something to read. */
	ret = prepare_track();
	if (ret < 0) {
		printk("pipeline: no track to play (%d)\n", ret);
		return;
	}

	ret = audio_pipeline_init(&pipeline, &cfg, &sink);
	if (ret < 0) {
		printk("pipeline: init failed (%d)\n", ret);
		return;
	}

	/* start() creates the worker thread and opens the node chain... */
	ret = audio_pipeline_start(&pipeline);
	if (ret < 0) {
		printk("pipeline: start failed (%d)\n", ret);
		return;
	}

	/* ...play() is what actually begins pulling frames. */
	ret = audio_pipeline_play(&pipeline);
	if (ret < 0) {
		printk("pipeline: play failed (%d)\n", ret);
	}

	/* Primary event path: block on the pipeline's queue from
	 * this control thread until the track is over. The worker thread stays
	 * alive across EOF, so a second play() would render another track
	 * without restarting anything.
	 */
	ret = audio_pipeline_get_event(&pipeline, &event, K_SECONDS(5));
	if (ret == 0) {
		print_event("queue", &event);
	} else {
		printk("pipeline: no event within 5 s (%d)\n", ret);
	}

	(void)audio_pipeline_stop(&pipeline);

	/* Only join() ends the worker thread and closes the nodes. */
	(void)audio_pipeline_join(&pipeline);
}
