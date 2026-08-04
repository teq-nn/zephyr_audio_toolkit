# I2S and hardware bring-up

The two I2S nodes are where the pipeline meets a driver with its own buffer ownership, its
own clocking and its own failure states. This article covers what you have to get right
before the first bit leaves the pin.

## What the I2S nodes accept, in one table

| Property | v1 | Enforced where |
| --- | --- | --- |
| Channels | exactly **2** | `open()`, `-ENOTSUP` |
| Wire depth | **16 bit** (`valid_bits_per_sample == 16`) | `audio_i2s_wire_format_get()`, `-ENOTSUP` |
| Data format | Philips I2S (`I2S_FMT_DATA_FORMAT_I2S`) | `open()` |
| Clock role | **target (slave) on both BCK and LRCK**, always | `AUDIO_I2S_{IN_RX,OUT_TX}_OPTIONS` |
| Device | from devicetree, must be `okay` | `BUILD_ASSERT` in the macro |
| Blocks per queue | `>= 2` | `BUILD_ASSERT` in the macro |
| Read/write timeout | `SYS_FOREVER_MS` | `open()` |

Mono is refused rather than accepted-and-halved: the Philips I2S frame carries two words by
definition, and the drivers behind this API ignore `i2s_config.channels` for that format, so
a mono pipeline would be clocked at half the rate it describes.

Wider words are not merely unimplemented arithmetic — on the STM32 I2S register file a 24 or
32 bit word is moved as two 16-bit halves in an order the container does not describe, and
that order cannot be settled without hardware to verify it against.

## Slave only, and why there is no option

The codec is the clock master on the systems this module is built for: it owns MCLK, BCK and
LRCK, and a second device driving any of them is contention on a shared wire. So both nodes
always pass:

```c
#define AUDIO_I2S_IN_RX_OPTIONS  (I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET)
#define AUDIO_I2S_OUT_TX_OPTIONS (I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET)
```

There is deliberately no Kconfig option beside them. `I2S_OPT_BIT_CLK_CONTROLLER` and
`I2S_OPT_FRAME_CLK_CONTROLLER` are **zero bits**, so "controller" is not a value a node could
pass by accident — it is the *absence* of the two above. That is also why the board suite
asserts the option word is exactly those two bits: an option word that had lost one would
silently mean "controller" instead of failing.

The practical consequence during bring-up: **nothing moves until the master clocks you.** A
capture node blocks in `i2s_read()` until a master appears, and a transmit node's queued
blocks sit there. That is why the board suites open and close the chain but never pull a
frame.

## Transfer blocks: size, alignment, location

Both macros allocate a private `k_mem_slab`:

```c
#define AUDIO_I2S_BLOCK_BYTES(_frame_samples) \
	ROUND_UP((size_t)(_frame_samples) * AUDIO_I2S_WIRE_MAX_WORD_BYTES, AUDIO_I2S_BLOCK_ALIGN)
```

Three decisions are baked in:

* **Sized from the frame capacity you pass**, not from
  `CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES`. Pass the *same* figure you gave
  `AUDIO_PIPELINE_DEFINE()`, so both allocations move together.
* **Width factor is `AUDIO_I2S_WIRE_MAX_WORD_BYTES` (4), not the depth in use.** The format
  is bound at run time and this has to be a compile-time constant, so blocks are sized for
  the widest word the container can ever produce. On the receive side that makes a block an
  *upper* bound: it may carry more than one frame, and the source drains it across as many
  frames as it takes rather than dropping the surplus.
* **Alignment is the d-cache line, floored at 32 bytes**
  (`AUDIO_I2S_BLOCK_ALIGN = max(CONFIG_DCACHE_LINE_SIZE, 32)`).

That last one is not cosmetic. A DMA-driven I2S driver does its own cache maintenance around
every transfer — `sys_cache_data_flush_range()` before TX, `sys_cache_data_invd_range()`
after RX — and those act on **whole cache lines**. A block that is not line aligned *and*
line sized lets a flush or an invalidate clobber whatever shares its first or last line. The
rule is identical in both directions, which is why it is stated once for both nodes.

> **Where the block lives is the other half of the rule, and the macro cannot express it.**
> The slab lands in `.noinit`, i.e. in the image's `zephyr,sram`. On the `nucleo_h723zg`
> target that is the AXI SRAM at `0x24000000`, which `dma1` can address. A target that puts
> `zephyr,sram` somewhere its I2S DMA cannot reach — DTCM at `0x20000000` on an STM32H7 —
> needs the slab **relocated**, not merely realigned. The failure is silent: the transfer
> completes and moves nothing.

More blocks buy tolerance against a late peer — an overrun on RX, an underrun on TX — at the
cost of latency. Two is the API minimum; four is a reasonable starting point.

## Buffer ownership, and why there is a copy

The Zephyr I2S API is `mem_slab` based:

* `i2s_read()` **hands you** a block the driver filled; you must free it back to the slab.
* `i2s_write()` **takes** a block from you and owns it until the transfer completes.

The pipeline's frame buffer is borrowed storage the pipeline owns and reuses for the next
frame. The two ownership models cannot be bridged by passing pointers, so each node copies
once at that boundary. That copy is the seam, not an oversight.

On the receive side there is exactly **one** release path (`i2s_in_block_release()`), and
every path that can lose interest in a block — drained, conversion failure, `close()` — goes
through it. A block that is not returned is invisible until the slab runs dry, at which
point capture stops for a reason that looks nothing like a leak.

## Blocking is the pacing mechanism

Both nodes configure `cfg.timeout = SYS_FOREVER_MS`, and the sink also allocates its block
with `K_FOREVER`. That is deliberate:

* On TX, `i2s_write()` waits until the wire has room, so the sink — and with it the whole
  pull chain — runs exactly as fast as the codec drains it.
* On RX, `i2s_read()` waits for the next block, so the source produces exactly as fast as
  the wire fills.

A timeout would turn a slow peer into dropped audio. `audio_pipeline_stop()` is asynchronous
precisely so it does not deadlock behind a blocking node; the frame in flight completes
first.

## Overrun and underrun are states, not events

When a Zephyr I2S direction under- or overruns, the driver parks it in `I2S_STATE_ERROR` and
refuses every further transfer. `I2S_TRIGGER_PREPARE` is the only way out — and it is legal
*only* from that state, which is what makes its return value double as the test:

```
write/read fails ──► PREPARE ──► 0   → it was an under/overrun, direction ready, retry once
                            └──► <0  → it failed for some other reason, report it as it was
```

Both nodes do this automatically and log a warning. Without it, a node wedges permanently on
its first underrun — the likeliest failure of all during bring-up.

`close()` uses `I2S_TRIGGER_DROP`, never `DRAIN` or `STOP`: draining waits for the queue to
play out and stopping finishes the block in flight, and a clock *target* whose master has
stopped would wait forever. `DROP` is legal from every state but `NOT_READY`, so the same
call closes a running stream and one that has errored. The direction is left
configured-but-stopped even if the trigger fails, so a failing `close()` cannot strand the
node half open.

## Wiring it up

```c
/* Devicetree aliases keep the application off board-specific node labels. */
AUDIO_I2S_IN_NODE_DEFINE(capture, DT_ALIAS(i2s_rx), FRAME_SAMPLES, 4);
AUDIO_GAIN_FILTER_NODE_DEFINE(gain, &capture, AUDIO_GAIN_UNITY_Q15 / 2);
AUDIO_I2S_OUT_NODE_DEFINE(playback, &gain, DT_ALIAS(i2s_tx), FRAME_SAMPLES, 4);

AUDIO_PIPELINE_DEFINE(pipeline, FRAME_SAMPLES, 2048, 5);
```

```conf
CONFIG_AUDIO_PIPELINE=y
CONFIG_AUDIO_PIPELINE_NODE_I2S_IN=y
CONFIG_AUDIO_PIPELINE_NODE_I2S_OUT=y
CONFIG_AUDIO_PIPELINE_NODE_GAIN_FILTER=y
```

`CONFIG_I2S` is not spelled out: the node symbols select it. You still enable the SoC's I2S
driver and describe the peripheral in devicetree.

## The reference board overlay

`tests/boards/nucleo_h723zg/i2s_smoke/boards/nucleo_h723zg.overlay` is the canonical
description of the hardware target, and hardware work is expected to `#include` it rather
than re-derive the pin choices. It records, with reasoning:

* **Two blocks, one per direction** — the `st,stm32h7-i2s` driver returns `-ENOSYS` for
  `I2S_DIR_BOTH`, so `i2s2` transmits and `i2s3` receives.
* **No `mck-enabled` on either block** — the codec owns MCLK. Master/slave itself is a
  *runtime* property of the STM32 driver (the option bits above); the missing `mck-enabled`
  is what keeps the devicetree consistent with that choice, and the board suite asserts it.
* **Pin choices**, including the non-obvious one: data pins come from the SPI pinctrl groups
  (`spiN_mosi_*`, `spiN_miso_*`), because the STM32H723ZG pinctrl file only defines
  `i2sN_ck_*`, `i2sN_ws_*` and `i2sN_mck_*`.
* **DMA reachability** — which dmamux channels the defaults use, why `dma1` is enabled and
  `dma2` is not, and that `dma1` cannot address DTCM.

Aliases it defines: `i2s-tx`, `i2s-rx`, `audio-ctrl-i2c`.

## Bringing a link up, in order

1. **Smoke test the board.** `tests/boards/nucleo_h723zg/i2s_smoke/` checks that both I2S
   devices and the control I2C report ready.
   ```sh
   west twister -T tests -p nucleo_h723zg --device-testing --hardware-map map.yaml
   ```
2. **Open and close the chain on silicon.** The `i2s_out_node` and `i2s_in_node` board
   suites do exactly this, and their build-time assertions are the real payload: DMA
   reachability, cache alignment, block-size arithmetic and the clock role all fail CI
   rather than the wire.
3. **Prove the transport with a known signal.** Chain the tone generator into the I2S sink
   at one end, and the I2S source into the tone analyzer at the other:

   ```c
   /* Transmit side */
   AUDIO_TONE_GEN_NODE_DEFINE(tone, AUDIO_TONE_GEN_FULL_SCALE_Q15 / 2, 0, 1000U, 3000U);
   AUDIO_I2S_OUT_NODE_DEFINE(tx, &tone, DT_ALIAS(i2s_tx), FRAME_SAMPLES, 4);

   /* Receive side: 960 samples at 48 kHz puts both tones on whole bins */
   AUDIO_I2S_IN_NODE_DEFINE(rx, DT_ALIAS(i2s_rx), FRAME_SAMPLES, 4);
   AUDIO_TONE_ANALYZER_NODE_DEFINE(check, &rx, 960U, 1000U, 3000U);
   ```

   A duration of `0` runs the stimulus indefinitely — a loopback under test is stopped by
   the application, not by the stimulus running out. Then poll the verdict:

   ```c
   struct audio_tone_analyzer_result r;

   audio_tone_analyzer_get_result(&check, &r);
   /* r.verdict: PASS | SILENT | SWAPPED | NOISE | WRONG_FREQ */
   ```

   Different frequencies per channel are what turn "a tone arrived" into "the *left* tone
   arrived on the left". `SILENT` means no clock or no signal; `SWAPPED` means the pair is
   crossed; `NOISE` means energy without a tone in it. See
   [Node reference → tone analyzer](06-node-reference.md#tone-analyzer-sink).

4. **Insert the real source and sink** once the transport is proven.

## When something does not move

| Symptom | Look at |
| --- | --- |
| `open()` returns `-ENODEV` | the devicetree node is `okay` but the driver is not enabled, or the device failed init |
| `open()` returns `-ENOTSUP` | channel count is not 2, or `valid_bits_per_sample` is not 16 |
| `open()` returns `-EINVAL` from the node | the block is too small for one interleaved sample set — check the `frame_samples` you passed the macro |
| `i2s_configure()` fails | the rate the driver can derive from its clock tree; STM32 blocks cannot hit every `frame_clk_freq` |
| Everything opens, nothing is transferred | no clock master, or the transfer blocks are in memory the DMA cannot address |
| Capture stops after a few seconds | a leaked block, or a consumer too slow and the overrun retry failing — check for the *"receive overrun, prepared and restarting"* warning |
| Transfer completes but the audio is silent/garbled | cache line alignment of the slab, or a buffer in DTCM |
