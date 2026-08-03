# Format negotiation: prior art

Background reading for #22 (channel count enforced three different ways) and, by
extension, #18 (format negotiation). The question behind both: **who owns the
stream format, and what happens when two parts of a chain disagree about it?**

Sources are primary — headers and implementation read from the projects
themselves, not from secondary write-ups. Read at the versions current on
2026-08-03.

## Summary

| | Format lives in | Declared capabilities | Refusal possible | Resolved when |
| --- | --- | --- | --- | --- |
| Arduino Audio Tools | pushed node to node | none | no (`void` return) | any time, at runtime |
| ESP-ADF | nowhere; event to the app | none | n/a — app's problem | app decides |
| Zephyr `i2s` | caller's `struct i2s_config` | none | yes (`-EINVAL`) | `i2s_configure()`, before streaming |
| Zephyr `dmic` | caller's `struct dmic_cfg`, amended by driver | none, but driver reports *actual* | yes, plus partial grant | `dmic_configure()`, before streaming |
| **this module** | `pipeline->format`, bound top-down | none | yes (`-ENOTSUP` from `open()`) | `audio_pipeline_start()` |

Neither of the two audio frameworks this module took inspiration from has a
pipeline-level format at all. Both are runtime-push: a source discovers the
format and everyone downstream adapts. Zephyr's own audio driver APIs are the
opposite, and are much closer to what this module already does.

## Arduino Audio Tools (pschatzmann)

`src/AudioTools/CoreAudio/AudioTypes.h`:

```cpp
struct AudioInfo {
  sample_rate_t sample_rate = DEFAULT_SAMPLE_RATE;
  uint16_t channels = DEFAULT_CHANNELS;
  uint8_t bits_per_sample = DEFAULT_BITS_PER_SAMPLE;
  ...
};

class AudioInfoSupport {
 public:
  /// Defines the input AudioInfo
  virtual void setAudioInfo(AudioInfo info) = 0;
  /// provides the actual input AudioInfo
  virtual AudioInfo audioInfo() = 0;
  /// provides the actual output AudioInfo: this is usually the same as
  /// audioInfo() unless we use a transforming stream
  virtual AudioInfo audioInfoOut() { return audioInfo(); }
};
```

Three observations:

1. **`setAudioInfo()` returns `void`.** There is no refusal channel anywhere in
   the design. A node cannot decline a format it does not support.
2. **Format propagates at runtime**, by push. `AudioInfoSource::notifyAudioChange()`
   walks a subscriber vector and calls `setAudioInfo()` on each, whenever a source
   discovers or changes its format.
3. **Nothing declares what it supports.** Grepping `src/AudioTools/CoreAudio/*.h`
   for `isSupported`, `supportedFormats`, or `supports(` returns no matches.

What a node does when it cannot comply is therefore a per-node improvisation.
`AudioI2S/I2SStream.h` shows the house style:

```cpp
virtual void setAudioInfo(AudioInfo info) {
  TRACEI();
  assert(info.sample_rate != 0);
  assert(info.channels != 0);
  assert(info.bits_per_sample != 0);
  AudioStream::setAudioInfo(info);
  if (is_active) {
    if (!p_driver->setAudioInfo(info)) {
      I2SConfig current_cfg = p_driver->config();
      if (!info.equals(current_cfg)) {
        LOGI("restarting i2s");
        ...
        p_driver->end();
        current_cfg.copyFrom(info);
        p_driver->begin(current_cfg);
      }
```

Adapt, or `assert()`. Those are the only two outcomes a `void` return leaves
available. Note the cost: an unsupported format tears down and restarts a live
I2S peripheral mid-stream.

The compensating idea, and the genuinely portable one: **a mismatch is not an
error, it is a missing node.** `AudioStreamsConverter.h` supplies
`ChannelFormatConverterStream`, `NumberFormatConverterStream` and
`FormatConverterStream`; the application inserts one where two neighbours
disagree.

## ESP-ADF (Espressif)

`components/audio_pipeline/include/audio_element.h`:

```c
typedef struct {
    int sample_rates;   /*!< Sample rates in Hz */
    int channels;       /*!< Number of audio channel, mono is 1, stereo is 2 */
    int bits;           /*!< Bit wide (8, 16, 24, 32 bits) */
    int bps;            /*!< Bit per second */
    ...
} audio_element_info_t;
```

Flat values, no ranges, no supported-set. `audio_element_report_info()`
(`audio_element.c:630`) packages the current info as a message:

```c
msg.cmd = AEL_MSG_CMD_REPORT_MUSIC_INFO;
```

That message goes to the element's **event listener** — the application — not to
the downstream element. `audio_pipeline.c` contains no reference to `getinfo`,
`setinfo`, or `music_info` at all: the pipeline object never reads, propagates,
or checks a format.

So ESP-ADF's answer to "who negotiates?" is *the application does*. The canonical
pattern is a decoder reporting `AEL_MSG_CMD_REPORT_MUSIC_INFO` and the app event
loop calling `i2s_stream_set_clk()` on the sink to match. Elements that disagree
without an app watching produce mislabelled audio, not an error.

## Zephyr's own audio APIs

This is the closer relative, and it points the other way.

### `i2s_configure()` — validate and refuse

`include/zephyr/drivers/i2s.h`:

```
 * The function can be called in NOT_READY or READY state only. If executed
 * successfully the function will change the interface state to READY.
 ...
 * @retval 0 on success.
 * @retval -EINVAL Invalid argument.
 * @retval -ENOSYS I2S_DIR_BOTH value is not supported.
```

The caller states a full `struct i2s_config`; the driver checks it against what
the hardware can do and returns an error if it cannot. There is no capability
query to ask first, and no partial acceptance. Critically, **configuration is a
state-machine-guarded operation**: it is legal only before streaming, never
underneath a running stream.

### `dmic_configure()` — request and actual

`include/zephyr/audio/dmic.h` is the interesting one, because it *does*
negotiate, in a shape neither audio framework uses:

```c
struct pdm_chan_cfg {
	uint32_t req_chan_map_lo;	/**< Channels 0 to 7 */
	uint32_t req_chan_map_hi;	/**< Channels 8 to 15 */
	uint32_t act_chan_map_lo;	/**< Channels 0 to 7 */
	uint32_t act_chan_map_hi;	/**< Channels 8 to 15 */
	/** Requested number of channels */
	uint8_t req_num_chan;
	/** Actual number of channels that the driver could configure */
	uint8_t act_num_chan;
	uint8_t req_num_streams;
	uint8_t act_num_streams;
};
```

The caller writes `req_*`; the driver writes back `act_*`. One call, no
round-trip, no capability enumeration — and the answer can be *partial* rather
than yes/no. The caller is expected to read `act_num_chan` and cope.

This is the Zephyr idiom for exactly the problem #22 raises: a channel count a
peripheral may not be able to honour. It resolves once, at configure time,
before any data moves.

## Where this module sits

`audio_pipeline_core.c:461` binds `pipeline->format` into every node immediately
before `open()`, and `open()` validates and may return `-ENOTSUP`;
`audio_pipeline_set_format()` refuses with `-EBUSY` while the chain is open
(`core.c:625`). That is the `i2s_configure()` model, lifted to a chain: one
format, resolved before streaming, refusable, immutable while running.

Two consequences worth stating plainly.

**The `-EBUSY` guard is what makes refusal possible.** Arduino Audio Tools shows
what a design costs once a source may push format after open: refusal becomes
structurally impossible and `assert()` is the fallback. Keeping the format
immutable across an open chain is what keeps `-ENOTSUP` meaningful.

**#22 is a bug this module's design makes available and the others do not.** The
defect is a central authority (`set_format()` accepting 1–255 channels,
`core.c:602`) that is weaker than the private rules the nodes actually enforce
(`file_reader_node.c` 1–2, `file_writer_node.c` ≤2). Arduino Audio Tools and
ESP-ADF cannot have this bug, because neither has a central rule to contradict.
The fix therefore has to come from within the model, not from either framework.

The same shape exists on the bit-depth axis and is currently worse:
`set_format()` never validates `valid_bits_per_sample` at all, and
`file_reader_node.c:167` compares the file against the bound format on sample
rate and channels but **not** depth — so a 16-bit file opens cleanly into a
24-bit-bound pipeline, and whether anything notices depends on which sink is in
the chain.

## What is worth borrowing

- **From `dmic_configure()`: request/actual.** If a node cannot honour the bound
  format exactly, a single resolved answer beats both a hard refusal and a silent
  adaptation. This is the Zephyr-native shape for #18.
- **From Arduino Audio Tools: a mismatch is a missing node.** `file_reader_node.c:161`
  already says so — *"v1 has no resampler and no channel mapper, so a file whose
  rate or channel count disagrees with the pipeline can only be refused"*. It
  names the node that would make the mismatch legal. That is the extension seam.
- **From ESP-ADF: the pipeline need not hold a format opinion.** Adopting that
  would delete `set_format()`'s channel check and leave nodes as the sole
  authority — the cheapest possible resolution of #22, at the cost of never being
  able to reject a chain before opening it.

## Not borrowed

- Runtime format push (both frameworks). Incompatible with `-EBUSY`-guarded
  binding, and it is what forces `assert()`-on-mismatch.
- Format as an application-level event (ESP-ADF). It moves the contradiction out
  of the library rather than resolving it.

## Sources

- Arduino Audio Tools — <https://github.com/pschatzmann/arduino-audio-tools>
  (`src/AudioTools/CoreAudio/AudioTypes.h`, `AudioI2S/I2SStream.h`,
  `AudioStreamsConverter.h`)
- ESP-ADF — <https://github.com/espressif/esp-adf>
  (`components/audio_pipeline/include/audio_element.h`,
  `components/audio_pipeline/audio_element.c`,
  `components/audio_pipeline/audio_pipeline.c`)
- Zephyr — <https://github.com/zephyrproject-rtos/zephyr>
  (`include/zephyr/drivers/i2s.h`, `include/zephyr/audio/dmic.h`)
- ESP-GMF — <https://github.com/espressif/esp-gmf> — successor framework; v0.7
  adds "element caps" capability support. Not read in depth; worth revisiting if
  #18 heads toward declared capabilities.
