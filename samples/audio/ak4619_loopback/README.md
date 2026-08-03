# AK4619 analog loopback

Play a two-tone stimulus out through an AKM AK4619's DAC, capture it back
through the same part's ADC over a physical cable, and report on the console
what came back and whether it was right.

Hardware: a `nucleo_h723zg` and an AKM **AKD4619-A** evaluation board, wired as
`docs/hardware/akd4619-evaluation-board.md` describes. Issue #47.

---

## The one rule

**A passing run proves nothing until the same run has been shown to fail with
the loop cable unplugged.**

The AK4619 can route an ADC's output straight into a DAC inside the package
(register `0x12`). With one of those paths enabled the loop passes with nothing
plugged in and verifies nothing at all. #46 disables them and this application
re-checks and prints register `0x12` before it starts — but a check is a claim,
and the control run is the proof.

So run it **twice**, and keep both consoles:

| Run | Loop cable | Required result |
| --- | --- | --- |
| 1 | `J210` → `J201` + `J202` connected | `RESULT: PASS` |
| 2 | unplugged at both ends | `RESULT: FAIL - the capture is silent` |

A run 2 that passes means the part is looping internally. Stop and read
register `0x12` in the run's own register dump. A run 2 that fails with anything
other than *silent* is also worth recording — it means something is still
reaching the ADC.

---

## 1. Wire it

Follow `docs/hardware/akd4619-evaluation-board.md` — §6 is the full checklist in
one table. The short version, and the six things that are actually easy to get
wrong:

- **Power.** +5.0 V into `J703`, 0 V into `J710` and `J711`. Do **not** power the
  evaluation board from the Nucleo (§5).
- **`SW500` L → H once** after power is applied, then leave it H. The part is in
  power-down until you do, and there is no GPIO that can do it for you (§1.5).
- **`SW502` both sections L** — I2C control, slave address `0x10` (§1.1, §1.2).
- **`PORT601` unplugged** for the whole session, so the board's own USB
  controller stays off SDA and SCL (§1.3).
- **Five I2S wires plus a ground** on `PORT401`, and **two I2C wires plus a
  ground** on `TP601`/`TP602` (§3.2, §1.4). `PB10`+`PC10` are strapped together
  to `PORT401` pin 3, and `PB12`+`PA4` to pin 5.
- **The six-pin selectors at 3-4 (PORT):** `PORT402`, `PORT403`, `PORT405`,
  `PORT406`, `PORT408`. This is what points the codec at the Nucleo instead of
  at the board's own AK4118A.

The loop cable is a 3.5 mm "insert" / Y lead: one TRS plug into `J210` (AOUT1),
two plugs into `J201` (AIN1, Lch) and `J202` (AIN2, Rch). An output jack carries
a stereo pair and an input jack carries one channel — that asymmetry is §4.1 and
it catches everyone once.

---

## 2. Build and flash

From the repository root, in an initialised west workspace:

```sh
west build -b nucleo_h723zg -d build/ak4619 samples/audio/ak4619_loopback \
    -- -DZEPHYR_EXTRA_MODULES=$PWD
west flash -d build/ak4619
```

Console is the ST-LINK VCP (`usart3`, 115200 8N1), the same one Zephyr's boot
banner comes out of.

The application runs once at boot and stops. To repeat it, reset the board.

---

## 3. What the console says

Six numbered sections, in the order the work happens. Sections 1 and 2 are #45
and #46's report — the part answering, the registers it was programmed with, and
the proof that no internal loopback is enabled. Sections 3 to 6 are this ticket.

### A passing run, cable in

```
=== AK4619 analog loopback: play a tone, capture it back (issue #47) ===

--- 1. the codec on the control bus ---
codec node : audio-codec@10
i2c address: 0x10
init check : passed at boot
link check : PASS - the part latched and returned both test patterns

--- 2. the format all four devices are programmed for ---
rate        : 48000 Hz, 2 ch
slot        : 32 bit, BICK 64 fs, MCLK 12288000 Hz (256 fs)
capture     : 24 bit ADC word, MSB-justified in the slot (shift 8)
clock roles : i2s2 drives MCLK/BICK/LRCK; i2s3 and the codec receive them
levels      : DAC -12 half-dB, ADC digital 0 half-dB, MIC gain 0 dB
configure   : PASS

registers, read back from the part:
  0x00 = 0x00  power management (00 = standby, RSTN asserted)
  0x01 = 0x0c  format: TDM/DCF/DSL/BCKP/SDOPH
  0x02 = 0x0c  format: SLOT/DIDL/DODL
  0x03 = 0x00  FS[2:0]: the MCLK ratio the part expects
  0x04 = 0x22  ADC1 analog MIC gain, Lch and Rch
  ...
  0x12 = 0x00  DAC source multiplexers - INTERNAL LOOPBACK
  ...

internal loopback: none. Register 0x12 has both DAC multiplexers on an
SDIN pin, so the only path from DAC to ADC is the cable between J210 and
J201/J202. Unplug it and the capture must go silent.

--- 3. what will be played ---
Lch 1000 Hz, Rch 3000 Hz, both at -3.0 dBFS peak
expected back: -12.0 dBFS RMS per channel, +/- 4.0 dB
pass window  : -16.0 .. -8.0 dBFS RMS
that expectation is the tone, the DAC volume, the MIC amp and the ADC volume
added up - change any of them in Kconfig and this line moves with it.

--- 4. starting, in the only order that works ---
playback (tone_gen -> i2s2): started
capture  (i2s3 -> tone_analyzer): started
codec out of standby: PASS - DAC1 and ADC1 powered, RSTN released

--- 5. what came back ---
first audio 63 ms after the codec left standby. That is an UPPER BOUND on
the loop latency - it contains a whole 960 sample analyzer window and the
transfer blocks in both directions. Record it in §4.6 of the wiring document.

        expected      measured      carries      in band at      tonal
          (RMS)         (RMS)                   1000  3000 Hz
  Lch    -12.0 dBFS    -12.2 dBFS     1000 Hz   0.996 0.001   yes
  Rch    -12.0 dBFS    -11.8 dBFS     3000 Hz   0.002 0.994   yes

windows measured: 12 of 960 samples each

pipeline events:

=== RESULT: PASS ===
Now run it again with the loop cable unplugged. Until that run has failed,
this one has proved nothing (issue #42).

--- 6. stopping ---
codec back in standby
both pipelines joined; the I2S blocks are stopped and the clocks are gone
```

The exact levels will differ — anywhere in the ±4 dB window is a pass, and the
budget behind that number is in `src/loopback_format.h`.

### The control run, cable out

Everything through section 4 is identical. Section 5 and the verdict are not:

```
--- 5. what came back ---
no window ever carried audio.

        expected      measured      carries      in band at      tonal
          (RMS)         (RMS)                   1000  3000 Hz
  Lch    -12.0 dBFS    -99.9 dBFS     silent    0.000 0.000   no
  Rch    -12.0 dBFS    -99.9 dBFS     silent    0.000 0.000   no

windows measured: 96 of 960 samples each

=== RESULT: FAIL - the capture is silent ===
Clocks are running and the ADC is delivering, but there is nothing on the
wire. THIS IS THE EXPECTED RESULT WITH THE LOOP CABLE UNPLUGGED. With the
cable in, check J210 -> J201/J202, that SW500 is H, and that JP202 is 2-3
and JP206 is 1-2.
```

Note what stays the same: the clocks still run, windows still complete, the
capture path still delivers samples. Only the audio is gone. That is what makes
this a control run rather than a second way of failing.

Both runs take about three seconds and stop.

---

## 4. Every verdict, and what to do about it

The console prints the reason under the result line. Summarised:

| `RESULT:` | Means | First thing to check |
| --- | --- | --- |
| `PASS` | The loop is good. | Do the cable-out run. |
| `FAIL - nothing was captured` | No analyzer window ever completed: `i2s3` received no data at all. | `i2s2` is not clocking. MCLK (`PC6` → `PORT401` pin 1), and `PORT402`/`403`/`405` at 3-4. |
| `FAIL - the capture is silent` | Clocks and capture fine, no audio. | **Expected with the cable out.** With it in: the cable, `SW500` = H, `JP202` 2-3, `JP206` 1-2. |
| `FAIL - the channels are swapped` | Both tones came back, each on the wrong channel. | The two input plugs, or the jacks' tip/ring assignment (§4.4, UNRESOLVED-4). Swap the plugs at `J201`/`J202`. |
| `FAIL - the wrong tone came back` | A tone arrived that neither channel expects. | Something else is connected; `J211` picked up. |
| `FAIL - energy came back with no tone in it` | Right level, no sinusoid. | See "if the 32-bit word is the problem" below. |
| `FAIL - the right tone, too quiet` | Loop works, level low. | MIC amp at 0 dB, DAC at −6.0 dB (§4.5), plugs fully in, `JP203` shorted. |
| `FAIL - the right tone, too loud` | Loop works, level high; the ADC is near clipping. | Registers `0x0E`/`0x0F` should read `0x24`, register `0x04` should read `0x22`. |
| `FAIL - the codec never got as far as being configured` | Sections 1–2 failed. | The six causes the console lists — power, `SW500`, `PORT601`, `SW502`, the I2C wires. |

### If the 32-bit word is the problem

`energy came back with no tone in it` has one cause that is not on the
evaluation board at all. Zephyr's STM32 I2S driver configures its DMA with a
fixed 16-bit transfer width whatever `word_size` says
(`drivers/i2s/i2s_stm32.c`), so a 32-bit word crosses to the peripheral as two
halves and the peripheral decides which half is the MSB. If that order is the
opposite of the little-endian one the pipeline's wire seam writes, the loop
still runs at the right rate, still carries the right amount of energy, and
comes back as broadband noise.

Rebuild with 16-bit slots and try again:

```sh
west build -b nucleo_h723zg -d build/ak4619-16 samples/audio/ak4619_loopback \
    -- -DZEPHYR_EXTRA_MODULES=$PWD -DCONFIG_AK4619_LOOPBACK_SLOT_16=y
west flash -d build/ak4619-16
```

That runs BICK at 32 fs instead of 64 fs — both are inside the AK4619's
32…256 fs range for `FS[2:0]` = `000` — keeps MCLK at 256 fs, and moves words the
STM32's DMA is already the right width for. Section 2 of the console will say
`slot: 16 bit, BICK 32 fs`. Everything else, including the pass window, is
unchanged. **If this is what it took, say so on the pull request**: it is a fact
about the toolkit's I2S link, not about this board.

---

## 5. What to record

Per the ticket, on the pull request:

1. The **full console of the cable-in run**.
2. The **full console of the cable-out run**.
3. The **`first audio ... ms` figure** from the passing run, into §4.6 of
   `docs/hardware/akd4619-evaluation-board.md`. No verdict uses it; a later
   sample-exact comparison will need somewhere to start.
4. Anything the board did that the wiring document says it should not — a switch
   that had to be somewhere else, a jumper that was not as shipped, a test point
   that was not where §1.4 says. Those go into the document, and the document
   says the board wins.

---

## 6. How it is put together

Two pipelines, each on its own worker thread, each pacing itself on its own
blocking I2S call:

```
tone_gen  ──►  i2s_out (i2s2, clock CONTROLLER)  ──►  SDIN1 ──► DAC1 ──► J210
                                                                          │
                                                             the loop cable
                                                                          ▼
tone_analyzer  ◄──  i2s_in (i2s3, clock target)  ◄──  SDOUT1 ◄── ADC1 ◄── J201/J202
```

- **Both** are `AUDIO_PIPELINE_DEFINE()`d. The subsystem's built-in stack, frame
  buffer and event slots are single-instance and owned rather than shared, so a
  second pipeline running on them fails at `init` with `-EBUSY`.
- **`i2s2` is the clock controller.** The AK4619 has no PLL and no clock output —
  MCLK, BICK and LRCK are input pins — so the STM32 generates all three or
  nothing is clocked (§2). That is what
  `AUDIO_I2S_OUT_CLK_CONTROLLER_NODE_DEFINE()` is for.
- **Nothing needs the two pipelines aligned.** The analyzer measures the
  magnitude of a frequency component, which does not depend on where its window
  starts, so the unknown loop latency does not enter the verdict.
- **The main thread never blocks on the audio path.** The workers may wait
  forever inside the I2S driver — that wait is how they pace themselves — but
  the thread that prints the verdict polls with a 2 s deadline, which is what
  turns a dead clock into a diagnosis instead of a hang.

Where the numbers live:

| What | Where |
| --- | --- |
| Rate, slot length, MCLK ratio, where the captured sample sits | `src/loopback_format.h` |
| The tone, the expected return level, the pass window and its budget | `src/loopback_format.h` |
| Verdict logic, decibel arithmetic, the hint under each result | `src/loopback_verdict.{h,c}` |
| The codec's registers | `drivers/ak4619.{c,h}` |
| Board switches, jumpers, jacks, gain plan | `docs/hardware/akd4619-evaluation-board.md` |

The verdict logic runs on the host with no hardware at all:

```sh
CI_TEST_PATH=tests/samples/audio/ak4619_loopback ./scripts/ci-test.sh
```
