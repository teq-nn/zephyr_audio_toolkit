# Quick Start: your first pipeline

Goal: your own application, running a stereo test tone through a gain filter into a null
sink. No filesystem, no hardware, no devicetree — it builds and runs on `native_sim` and on
any board. It is the shortest chain that is still a real pipeline.

## The five steps

Every application does the same five things:

1. **Enable** the module and the nodes it uses, in `prj.conf`.
2. **Define** the nodes in dataflow order and the pipeline, at file scope.
3. **Bind** a configuration and the sink with `audio_pipeline_init()`.
4. **Bind the format** with `audio_pipeline_set_format()`, then `start()` and `play()`.
5. **Wait** for an event, then `stop()` and `join()`.

## 1. `prj.conf`

```conf
CONFIG_AUDIO_PIPELINE=y

# One symbol per node this application defines. They all default to n, so the
# image links exactly these three.
CONFIG_AUDIO_PIPELINE_NODE_TONE_GEN=y
CONFIG_AUDIO_PIPELINE_NODE_GAIN_FILTER=y
CONFIG_AUDIO_PIPELINE_NODE_NULL_SINK=y
```

## 2. `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(my_pipeline)

target_sources(app PRIVATE src/main.c)
```

## 3. `src/main.c`

```c
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/audio/audio_node.h>
#include <zephyr/audio/audio_nodes.h>
#include <zephyr/audio/audio_pipeline.h>
#include <zephyr/audio/audio_pipeline_events.h>

#define RATE_HZ   48000U
#define CHANNELS  2U
/* Sample counts are ALWAYS total across all channels: this is one second of
 * stereo, not two.
 */
#define DURATION_SAMPLES (RATE_HZ * CHANNELS)

/* Nodes are wired at build time, so they are defined in dataflow order and each
 * one points at the node feeding it. A source takes no upstream.
 *
 * One frequency per channel: 1 kHz left, 3 kHz right.
 */
AUDIO_TONE_GEN_NODE_DEFINE(tone, AUDIO_TONE_GEN_FULL_SCALE_Q15 / 2, DURATION_SAMPLES,
			   1000U, 3000U);
AUDIO_GAIN_FILTER_NODE_DEFINE(gain, &tone, AUDIO_GAIN_UNITY_Q15 / 2);   /* -6 dB */
AUDIO_NULL_SINK_NODE_DEFINE(sink, &gain);

/* The pipeline owns its worker thread, its stack, its frame buffer and its event
 * queue storage. frame_samples is the TOTAL interleaved sample count, so at two
 * channels the default 128 is 64 sample pairs per frame (~1.33 ms at 48 kHz).
 */
AUDIO_PIPELINE_DEFINE(pipeline, CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES,
		      CONFIG_AUDIO_PIPELINE_THREAD_STACK_SIZE,
		      CONFIG_AUDIO_PIPELINE_THREAD_PRIO);

static const struct audio_pipeline_config cfg = {
	/* Must match what AUDIO_PIPELINE_DEFINE() allocated: init() clamps the
	 * frame capacity to the smaller of the two.
	 */
	.frame_samples = CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES,
	.event_cb = NULL,          /* the queue below is the primary path */
	.event_user_data = NULL,
};

/* The one format the whole chain runs at. The application declares it; no node
 * infers it from its peers.
 */
static const struct audio_stream_config format = {
	.sample_rate_hz = RATE_HZ,
	.channels = CHANNELS,
	/* The container is always 32 bit; this is the effective resolution the
	 * stream carries (see article 3).
	 */
	.valid_bits_per_sample = 16,
	.format = AUDIO_SAMPLE_FORMAT_S32_LE,
};

int main(void)
{
	struct audio_pipeline_event event;
	int ret;

	ret = audio_pipeline_init(&pipeline, &cfg, &sink);   /* the SINK, not the source */
	if (ret < 0) {
		printk("init failed (%d)\n", ret);
		return 0;
	}

	ret = audio_pipeline_set_format(&pipeline, &format);
	if (ret < 0) {
		printk("format rejected (%d)\n", ret);
		return 0;
	}

	/* start() opens the chain (sink first, then upstream) and creates the
	 * worker thread. The worker starts out idle.
	 */
	ret = audio_pipeline_start(&pipeline);
	if (ret < 0) {
		printk("start failed (%d)\n", ret);
		return 0;
	}

	/* play() is what actually begins pulling frames. */
	ret = audio_pipeline_play(&pipeline);
	if (ret < 0) {
		printk("play failed (%d)\n", ret);
		return 0;
	}

	/* Block until the generator's configured duration runs out. */
	ret = audio_pipeline_get_event(&pipeline, &event, K_SECONDS(5));
	if (ret == 0 && event.type == AUDIO_PIPELINE_EVENT_EOF) {
		printk("end of stream\n");
	} else {
		printk("no EOF (%d)\n", ret);
	}

	(void)audio_pipeline_stop(&pipeline);
	(void)audio_pipeline_join(&pipeline);   /* only join() ends the thread */

	return 0;
}
```

## 4. Build and run

```sh
west build -b native_sim -d build/my my_app
west build -d build/my -t run
```

## What each call actually does

| Call | Effect | Refuses when |
| --- | --- | --- |
| `audio_pipeline_init()` | Binds config + sink, claims resources, (re)initialises the event queue, clears any bound format | `-EINVAL` bad args/config, `-EBUSY` worker still alive or a built-in resource is taken |
| `audio_pipeline_set_format()` | Copies the format into pipeline-owned storage | `-EINVAL` zero rate/channels or more channels than a frame holds, `-EBUSY` chain is open |
| `audio_pipeline_start()` | Opens every node sink-first, installing the format on each, then creates the worker thread | `-ENODATA` no format bound, `-ENOTSUP` a node refuses the format, `-ELOOP` chain too deep |
| `audio_pipeline_play()` | Wakes the worker; it starts pulling frames | `-EPERM` no thread, or the chain is closed |
| `audio_pipeline_stop()` | Asks the worker to stop pulling. Asynchronous — the frame in flight may finish | never, on an initialised pipeline |
| `audio_pipeline_join()` | Ends the worker thread, closes the chain, releases built-in resources | `-EINVAL` uninitialised; returns the first `close()` error |

Full semantics and every error code: [Pipeline lifecycle](05-pipeline-lifecycle.md).

## Things that bite newcomers

* **`init()` takes the sink.** The chain is walked backwards from there via `->upstream`,
  so the source is never passed to the pipeline.
* **Sample counts are total, never per channel.** `frame_samples`, the generator's
  `duration_samples`, `out_size` — all of them count interleaved samples across all
  channels. At two channels, 128 means 64 sample pairs.
* **`start()` does not play.** It opens the chain and creates the thread; `play()` starts
  the pulling. This split is what lets you open a chain and arm it before the first sample
  moves.
* **`join()` is not optional** if your pipeline runs on the built-in resources (i.e. was
  *not* defined with `AUDIO_PIPELINE_DEFINE()`). An abandoned instance holds them for the
  life of the process and every later hand-rolled pipeline gets `-EBUSY`.
* **The tone generator wants one frequency per channel, exactly.** Two channels, two
  frequencies; a mismatch is `-ENOTSUP` out of `start()`, not a silently dropped tone.

## Variations

* **Write a WAV file instead:** swap the null sink for
  `AUDIO_FILE_WRITER_NODE_DEFINE(sink, &gain, "/lfs/out.wav")`, set
  `CONFIG_AUDIO_PIPELINE_NODE_FILE_WRITER=y`, and mount a filesystem first. The writer
  needs `valid_bits_per_sample == 16`.
* **Play a WAV file:** replace the tone generator with
  `AUDIO_FILE_READER_NODE_DEFINE(src, "/lfs/track.wav")`. The reader refuses a file whose
  sample rate or channel count disagrees with the bound format.
* **Check the audio instead of discarding it:** replace the null sink with the tone
  analyzer and read its verdict as a value —
  see [Node reference → tone analyzer](06-node-reference.md#tone-analyzer-sink).
