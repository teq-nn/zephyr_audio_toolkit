# AKD4619-A evaluation board: configuration and wiring for `nucleo_h723zg`

The board settings a human needs in order to wire an AKM AKD4619-A evaluation
board to a `nucleo_h723zg` and leave the Nucleo — not the board's own USB
controller — in charge of the codec. Written for #43; #44, #45, #46 and #47
build on the decisions recorded here.

## Sources

| | Document | Identity |
| --- | --- | --- |
| **manual** | AKD4619-A AK4619 Evaluation Board Rev.0 | `<KM134006>` 2023/05, 64 numbered pages plus 7 schematic sheets on pages 65–71 |
| **datasheet** | AK4619VN 4ch 32-bit 192 kHz audio CODEC | `200900082-E-00` 2021/06 |

Every designator below is quoted from one of those two documents and carries
its page. Printed page numbers and PDF page numbers coincide in both. Where a
fact comes from a schematic sheet rather than the body text this is said
explicitly, because the body text is what AKM maintains — sheet-derived facts
are marked **[schematic]** and belong on the verify-on-board list.

Nothing here has been checked against physical hardware. Where the board
disagrees with this document, the board wins; record the disagreement here.

## The decisions, in one place

| Question | Answer | Where it comes from |
| --- | --- | --- |
| Control interface | I2C, `SW502-1` (SW-I2CN) = **L** | manual p.26, Table 19 |
| 7-bit I2C slave address | **0x10** (`0010000`), CAD = 0 via `SW502-2` = L | datasheet p.57; manual p.26 |
| USB controller kept off SDA/SCL by | leaving `PORT601` **unplugged** | manual p.70 **[schematic]** |
| **Clock mastering** | **STM32 as clock source** | manual pp.24–25 + p.68; datasheet p.6 |
| Digital audio path | `PORT401` (INOUT-PORT), 14-pin header | manual p.25, Tables 17/18 |
| Optical/coax path | not used | issue #43 |
| DAC out → ADC in loop | `J210` → `J201` (+ `J202` for a stereo loop) | manual p.4, p.66 |
| Attenuation in the loop cable | none needed; drive the DAC at ≤ −3 dBFS | datasheet pp.11–12 |
| Board power | +5.0 V into `J703` (REG); all rail jumpers at their defaults | manual pp.6–8, p.37 |

## 1. Control mode

The board is normally driven from a Windows application over `PORT601`, the
USB-B port, which reaches the codec through a PIC18F4550 (`U602`) (manual p.5
item (9), p.27). Two things have to be true for the Nucleo to own the bus
instead.

### 1.1 Select I2C at the codec

`SW502` is a two-section DIP switch (manual p.26, Figure 25):

| Section | Name | Function | Setting for this test |
| --- | --- | --- | --- |
| `SW502-1` | SW-I2CN | control interface, H: SPI, L: I2C | **L** (default) |
| `SW502-2` | SW-CAD | I2C slave address LSB | **L** (default) |

Both defaults are L (manual p.26, Table 19), so a board as shipped is already
correct. Section 6.3 (5) of the manual calls the second section "the DIP switch
Level of SW502 CAD" (p.28), which is what ties the switch names in Table 19 to
the `SW502` designator in Figure 25.

`SW-I2CN` does more than tell the codec which protocol to speak: it is the
select input of `U501`, a `TC7MBL3257CFT` 4-bit 2:1 bus switch sitting between
the PIC and the codec's four control pins (manual p.69 **[schematic]**):

| `U501` | B1 input (S = L, I2C) | B2 input (S = H, SPI) | A output → codec pin |
| --- | --- | --- | --- |
| 1 | `PCIF-SDA/SO` | `PCIF-SDA/SO` | `SDA/SO` (pin 30) via `R501` 51 Ω |
| 2 | `PCIF-SCL/SCLK` | `PCIF-SCL/SCLK` | `SCL/SCLK` (pin 28) via `R502` 51 Ω |
| 3 | `SW-CAD` | `PCIF-CSN` | `CAD/CSN` (pin 27) via `R503` 51 Ω |
| 4 | `DGND` | `PCIF-SI` | `SI` (pin 29) via `R504` 51 Ω |

Read that table carefully: **`U501` does not disconnect the PIC from SDA/SCL in
either position.** It only decides whether the codec's `CAD/CSN` pin is strapped
by `SW-CAD` or driven as an SPI chip select. `OE` is tied to `DGND`, so the
switch is permanently enabled. Isolating the on-board controller is therefore a
separate problem, solved in 1.3.

### 1.2 I2C slave address

The AK4619's seven address bits are `0010 00 CAD`, with the CAD bit taken from
the `CAD/CSN` pin (datasheet p.57, Figure 37: "The most significant seven bits
of the slave address are fixed as `0010000`").

With `SW502-2` = L, `CAD` = 0 and the **7-bit slave address is `0x10`**
(write byte `0x20`, read byte `0x21`). Setting `SW502-2` = H gives `0x11`.
`SW-CAD` reaches the codec only while `SW-I2CN` = L (§1.1), so the address is
only defined in I2C mode.

### 1.3 Keeping the on-board controller off SDA and SCL

The manual documents no external-control mode at all, and brings no I2C signal
to a header. The answer below is read off schematic sheet 6 (manual p.70) and
is **[schematic]**-derived:

- `U602` (PIC18F4550) is bus-powered: its `VDD0`/`VDD1` pins sit on the `VUSB`
  rail that comes in on `PORT601` pin 1.
- The PIC does not reach the codec's bus directly. It drives `SCL2`/`SDA2` of
  two `PCA9306DP1` translators, `U601` (SCL/SDA) and `U604` (CSN/SI). Their
  `EN` pins are pulled to that same `VUSB` rail through `R601`/`R611`, 100 kΩ.
- A `PCA9306` with `EN` low holds its pass transistors off, so with no USB cable
  the PIC is both unpowered and disconnected.

**Setting: leave `PORT601` unplugged for the whole test.** Do not run the AKM
control software against a board the Nucleo is driving.

A second, belt-and-braces option exists but is documented only by the silkscreen
legend on sheet 6 (`JP601`, "1:VDD 2:MCLR 3:PGD 4:PGC 5:GND"): grounding
`JP601` pin 2 holds the PIC in reset. Treat it as unverified.

### 1.4 Where the Nucleo attaches to the bus

There is no I2C header. The bus is reachable at test points; both pairs sit on
the same net once `SW-I2CN` = L, and both are **[schematic]**:

| Signal | At the PIC-IF block (manual p.70) | At the codec (manual p.65) |
| --- | --- | --- |
| SCL | `TP601` (`SCL/SCLK`) | `TP126` (`SCL/SCLK`) |
| SDA | `TP602` (`SDA/SO`) | `TP128` (`SDA/SO`) |
| GND | `DGND` | `TP134` (`DGND`) |

`TP601`/`TP602` are on the near side of `U501`; `TP126`/`TP128` are past `U501`
and the 51 Ω series resistors, right at the codec. Either works — prefer
`TP601`/`TP602`, which is the net that carries the pull-ups.

**Pull-ups are already on the board:** `R607` and `R608`, 10 kΩ to TVDD, on the
`PCIF-SCL/SCLK` and `PCIF-SDA/SO` nets (manual p.70 **[schematic]**). Do not
add more, and do not enable the STM32's internal pull-ups on top of them
without measuring. With TVDD at 3.3 V (§5) the bus is a 3.3 V bus, directly
compatible with the Nucleo. The codec is a fast-mode part, 400 kHz max
(datasheet p.57); 100 kHz is the safer starting point over flying leads.

### 1.5 Reset

`SW500` is a manual toggle driving the codec's `PDN` pin through `U502`, an
`SN74LVC2G14` (manual p.26, Figure 26 and Table 20; p.69 **[schematic]**):
L = power-down and register initialise (default), H = normal operation. The
sequence the manual gives is to bring `SW500` from L to H once, just after
power is applied, and leave it H for the whole session (manual p.27, item 3).

**`PDN` is not available to the Nucleo.** It is driven by a push-pull buffer
output, so a Nucleo GPIO wired to it would fight `U502`. #45 must reset the
part through the register map instead of through `PDN`; the AKM software
exposes the same thing as its `[RSTN]` button (manual p.29, item (12)).

`SW501` is the matching toggle for the AK4118A (`U300`). Set it **L**
(power-down) — see §2.

## 2. Clock mastering: **STM32 as clock source**

**Decision: the STM32 is the clock source. `i2s2` generates MCLK, BICK and
LRCK; the AKD4619 receives all three.** #44 implements this.

Three facts force it, and no board setting can undo them:

1. **The AK4619 cannot master anything.** `LRCK` (pin 6), `BICK` (pin 7) and
   `MCLK` (pin 8) are all direction `I` — "Audio Serial Frame Sync Clock Pin",
   "Audio Serial Data Clock Pin", "External Master Clock Input Pin"
   (datasheet p.7). The part has no PLL and no clock output.
2. **The board's own clock generator cannot reach the Nucleo.** The AK4118A
   (`U300`) has the 24.576 MHz crystal `X300` and can output `MCKO1`, `BICK`
   and `LRCK` (manual p.19, Table 9; p.20, Table 11) — but those reach the
   codec only through the `DIO` position of the `PORT402`/`PORT403`/`PORT405`
   selectors (manual p.21, Table 12). They are not routed to `PORT401`.
3. **`PORT401`'s clock pins are inputs, in hardware.** Tables 16 and 18 give
   `MCLK` (pin 1), `BICK` (pin 3) and `LRCK` (pin 5) as direction *Input*
   (manual pp.24–25). Sheet 4 shows why this is not a convention but a
   strapping: the clock lines pass through `U401`, a `74AVC4T245PW`, whose
   `1DIR`/`2DIR` pins are tied to `D3318V` — and the sheet's own legend reads
   "OE=L DIR=H : A=in B=A", A being the `PORT401` side (manual p.68
   **[schematic]**). The buffer physically cannot drive clocks outward.

So the only topology in which the Nucleo and the codec share a clock is the one
where the Nucleo generates it. Selecting `DIO` for the clocks would leave the
codec clocked by the AK4118A and the STM32 clocked by nothing.

Consequences for the rest of the batch:

- The `nucleo_h723zg` overlay's "WHY SLAVE" paragraph no longer holds. #44 must
  make `i2s2` a **clock master** — `i2s_configure()` without
  `I2S_OPT_FRAME_CLK_TARGET`/`I2S_OPT_BIT_CLK_TARGET` — add `mck-enabled` to
  the `i2s2` node and claim an MCLK pin (§3.2).
- `i2s3` stays a **clock target** on the same BICK and LRCK wires. Exactly one
  of the two blocks may drive them; if #44 makes both masters, two push-pull
  outputs fight on every strap.
- Start order inverts relative to #36. There, the DIX had to be clocking before
  the STM32 blocks started. Here the clocks do not exist until `i2s2` starts, so
  the order is: `SW500` to H → configure the codec over I2C → start `i2s2` →
  start `i2s3`.
- Zephyr's STM32 I2S driver emits MCLK at **256 fs** — `bit_clk_freq *= 4` for a
  32-bit channel length on top of a 64 fs bit clock (`drivers/i2s/i2s_stm32.c`,
  read at Zephyr `main`). At 48 kHz that is MCLK 12.288 MHz, BICK 3.072 MHz,
  inside the AK4619's 2.027–24.822 MHz MCLK range (datasheet p.23) and matching
  its **default** `FS[2:0]` = `000` (MCLK 256 fs, BICK 32–256 fs, 8 kHz ≤ fs ≤
  48 kHz — datasheet p.31, Table 1). #46 gets the easy case.
- MCLK, BICK and LRCK must be synchronous with each other (datasheet p.31);
  one STM32 block generating all three satisfies that by construction.

Because the AK4118A is out of the path entirely, set `SW501` = L to keep it
powered down, and leave `SW300`, `JP300`, `JP301` and the four optical/coaxial
connectors as shipped (manual p.19, Tables 8 and 9). None of them is in the
signal path for this test.

## 3. Digital audio interface

### 3.1 `PORT401` (INOUT-PORT), 14-pin header

Signals on odd pins, ground on even pins (manual p.25, Table 18; even-pin
grounding from sheet 4, manual p.68 **[schematic]**):

| Pin | Signal | Direction (board's view) | Pin | Signal |
| --- | --- | --- | --- | --- |
| 1 | `MCLK` | Input | 2 | `DGND` |
| 3 | `BICK` | Input | 4 | `DGND` |
| 5 | `LRCK` | Input | 6 | `DGND` |
| 7 | `SDIN2` | Input | 8 | `DGND` |
| 9 | `SDIN1` | Input | 10 | `DGND` |
| 11 | `SDOUT2` | Output | 12 | `DGND` |
| 13 | `SDOUT1` | Output | 14 | `DGND` |

`SDIN1` is the DAC's serial input and `SDOUT1` the ADC's serial output — the
pairing every evaluation mode in the manual uses (manual p.9, Table 6).
`SDIN2`/`SDOUT2` are unused here.

The `PORT401` side of the buffers runs at `D3318V`, which `JP706` selects as
+3.3 V by default (manual p.7, Table 4) — the right level for the Nucleo.

### 3.2 Mapping against the STM32 pins the overlay already uses

Existing overlay: `tests/boards/nucleo_h723zg/i2s_smoke/boards/nucleo_h723zg.overlay`.

| `PORT401` pin | Signal | STM32 pin | pinctrl group | Block | Status |
| --- | --- | --- | --- | --- | --- |
| 1 | MCLK | `PC6` | `i2s2_mck_pc6` | i2s2 | **must be added by #44** |
| 3 | BICK | `PB10` and `PC10`, strapped | `i2s2_ck_pb10`, `i2s3_ck_pc10` | i2s2 drives, i2s3 receives | already in overlay |
| 5 | LRCK | `PB12` and `PA4`, strapped | `i2s2_ws_pb12`, `i2s3_ws_pa4` | i2s2 drives, i2s3 receives | already in overlay |
| 9 | SDIN1 | `PB15` | `spi2_mosi_pb15` | i2s2 TX data | already in overlay |
| 13 | SDOUT1 | `PC11` | `spi3_miso_pc11` | i2s3 RX data | already in overlay |
| 2/4/6/8/10/12/14 | DGND | any Nucleo GND | — | — | §5 |

Five signal wires and at least one ground. The two strapped pairs already exist
as straps in the overlay's wiring description; what changes is that `PB10` and
`PB12` become outputs.

**On the MCLK pin.** `i2s2_mck_pc6` is the only MCK group `i2s2` has on this
part, and `i2s3`'s only one is `i2s3_mck_pc7`
(`hal_stm32/dts/st/h7/stm32h723zgtx-pinctrl.dtsi`, read at `main`). `PC6` is not
claimed by any enabled node in `boards/st/nucleo_h723zg/nucleo_h723zg.dts` (read
at Zephyr `main`), so the overlay's pin-budget paragraph gains `PC6` without
losing anything. Its ZIO connector position must be read off the board silkscreen
or UM2407 — this document does not assert it.

## 4. Analog signal path and the loop cable

### 4.1 Jacks

All six are 3.5 mm stereo mini jacks (manual p.1, p.4 item (2)):

| Jack | Silk | Carries | Codec pins |
| --- | --- | --- | --- |
| `J201` | AIN1 | ADC1 **Lch** input | `IN1P` / `IN1N` |
| `J202` | AIN2 | ADC1 **Rch** input | `IN2P` / `IN2N` |
| `J203` | AIN3 | ADC2 Lch input | `IN3P` / `IN3N` |
| `J204` | AIN4 | ADC2 Rch input | `IN4P` / `IN4N` |
| `J210` | AOUT1 | DAC1 **stereo** output | `AOUT1L` / `AOUT1R` |
| `J211` | AOUT2 | DAC2 stereo output | `AOUT2L` / `AOUT2R` |

The asymmetry matters and is easy to get wrong: **an output jack carries a
stereo pair, an input jack carries one channel** (its P and N halves). Figure 16
labels the four input jacks `IN1P/N` … `IN4P/N` and the two output jacks
`AOUT1L/R`, `AOUT2L/R` (manual p.17). A stereo loop therefore needs two input
jacks — which is why every evaluation mode in Table 6 lists "AIN1 J201, AIN2
J202" for one stereo ADC pair (manual p.9).

Jack contacts, from sheet 2 (manual p.66 **[schematic]**):

| Jack | pin 1 | pin 2 | pin 5 |
| --- | --- | --- | --- |
| `J201`…`J204` | sleeve → `JP203`/`JP210` node → AGND | `AINP` | `AINN` |
| `J210`, `J211` | sleeve → AGND via `R210`/`R211` 0 Ω | `AOUTL` | `AOUTR` |

The manual never says which contact — tip or ring — is pin 2 and which is pin 5.
See UNRESOLVED-4.

### 4.2 Is the ADC input behind the microphone amplifier?

Yes. Every analog input passes through the MIC Gain Amp before its ADC
(datasheet p.43, Figure 23), gain `MGN*[3:0]`, −6 dB to +27 dB (datasheet
p.10), **default `2h` = 0 dB** (datasheet p.42, Table 9; register 04h/05h reset
value `22h`, p.60). At 0 dB the input is a line-level input:
full scale 2.83 Vpp typ, input impedance 25 kΩ typ (datasheet p.11). #46 must
leave the MIC gain at 0 dB; every dB added there costs a dB of loop headroom.

### 4.3 Input jumpers

Ten jumpers configure the analog inputs (manual p.18, Table 7). For a
single-ended stereo loop into ADC1:

| Jumper | Selects | Setting for this test | Default |
| --- | --- | --- | --- |
| `JP202` | `IN1P` / `AIN1L` / `AIN3L` | **2-3** (signal from `J201` pin 2) | 2-3 |
| `JP201` | `IN1N` / `AIN2L` / `GND3L` | **Open** (unused input left open) | 2-3 |
| `JP206` | `IN2P` / `AIN1R` / `AIN3R` | **1-2** (signal from `J202` pin 2) | 1-2 |
| `JP207` | `IN2N` / `AIN2R` / `GND3R` | **Open** | 2-3 |
| `JP203` | jack signal GND ↔ board GND, `J201`/`J202` | **Short** | Short |
| `JP208`, `JP209`, `JP213`, `JP214`, `JP210` | ADC2 inputs, `J203`/`J204` | leave at defaults; not in the path | as shipped |

That pairs with `AD1LSEL[1:0]` = `AD1RSEL[1:0]` = `01`, "Single-Ended1", which
routes `AIN1L` and `AIN1R` — the `IN1P` and `IN2P` pins — into ADC1
(datasheet p.43, Table 10). The datasheet's instruction for this mode is that
"unused input pins should be left open" (p.44), which is why `JP201` and `JP207`
come out rather than staying at their differential default.

Fallback if the loop hums: pseudo-differential. Put `JP201` and `JP207` at
**1-2** (signal GND, per Table 7) and set `AD1LSEL`/`AD1RSEL` = `11`
(`AIN3L`/`GND3L`, `AIN3R`/`GND3R`). The jack sleeve then becomes the reference
the ADC subtracts.

### 4.4 The loop cable

DAC1 out on `J210` → ADC1 in on `J201` (Lch) and `J202` (Rch). One 3.5 mm TRS
plug at the board's output, two plugs at its inputs — an "insert"/Y cable:

```
J210 (AOUT1)  tip  ── AOUT1L ──►  J201 tip   (AINP → IN1P = AIN1L, ADC1 Lch)
              ring ── AOUT1R ──►  J202 tip   (AINP → IN2P = AIN1R, ADC1 Rch)
              sleeve ──────────►  J201 sleeve, J202 sleeve
```

A mono variant — a plain TRS-to-TS lead from `J210` to `J201` — is a legitimate
smaller test: it verifies DAC Lch → ADC1 Lch only, and a TS plug shorts the ring
contact to sleeve, which is harmless with `JP201` open. #47's channel-swap check
needs the stereo version.

**Attenuation: none needed, but there is no headroom either.** The DAC's
full-scale output and the ADC's single-ended full-scale input are the same
number and track the same rail: 2.55 / **2.83** / 3.11 Vpp min/typ/max, both
specified as 0.858 × AVDD (datasheet p.11 note *14, p.12 note *15). A 0 dBFS
tone therefore lands exactly at ADC full scale, and part-to-part spread on the
two ends is ±10%. Drive the DAC at **≤ −3 dBFS** (or back the DAC digital
volume off by 3 dB) and the loop has margin without an attenuator.

Series and load parts in the path, all on-board: `R224`–`R227` 220 Ω in series
with the outputs, `R220`–`R223` 10 kΩ to AGND, and 1 µF DC-blocking capacitors
at both ends (`C209`–`C212` out, `C201`–`C208` in) (manual p.66
**[schematic]**). Against a 25 kΩ input impedance the 220 Ω costs under
0.1 dB, and the two capacitors put the high-pass corner near 13 Hz — the manual
notes the same effect on its own low-frequency plots (p.62). A 1 kHz test tone
is unaffected.

## 5. Power and ground

Powered from its own supply. **Do not attempt to power the AKD4619-A from the
Nucleo** (issue #43; and see Note 3, manual p.8: "Each supply line should be
distributed independently from the power supply unit").

Simplest configuration, and the one the manual's own measurements use
("Power Supply: AVDD = VREFH = TVDD = +3.3V(Regulator)", manual p.37): feed the
regulator input only and leave every rail jumper at its default, which routes
all four rails from the on-board regulators.

| Jack | Colour | Supply | Function |
| --- | --- | --- | --- |
| `J703` | Red | **+5.0 V** | REG — input to the on-board regulators |
| `J710` | Black | 0 V | AGND, "should always be connected" (manual p.6) |
| `J711` | Black | 0 V | DGND, "should always be connected" (manual p.7) |
| `J701` | Yellow | not used | AVDD, only if `JP701`/`JP702` are moved to CON |
| `J730` | Yellow | not used | VREFH, only if `JP730`/`JP731` are moved to CON |
| `J702` | Yellow | not used | TVDD, only if `JP703`/`JP705` are moved to CON |
| `J731` | Black | not used | VREFL, only if `JP732` is moved to CON |

Rail jumpers — all at their manual defaults (manual pp.6–8, Tables 1–5):

| Jumper | Default | Result |
| --- | --- | --- |
| `JP701` AVDD-SEL1 | 1-2 (REG) | AVDD from regulator |
| `JP702` AVDD-SEL2 | 1-2 (REG) | AVDD source = regulator, 3.3 V |
| `JP730` VREFH-SEL1 | 1-2 (REG) | VREFH from regulator |
| `JP731` VREFH-SEL2 | 3-2 (REG3.3) | VREFH = 3.3 V |
| `JP732` VREFL-SEL1 | 3-2 (AGND) | VREFL = AGND |
| `JP703` TVDD-SEL1 | 1-2 (REG) | TVDD from regulator |
| `JP704` TVDD-SEL2 | 1-2 (+3.3V) | TVDD = 3.3 V — required for a 3.3 V Nucleo |
| `JP705` TVDD-SEL3 | 1-2 (REG) | TVDD source = regulator |
| `JP706` D3318V-SEL1 | 1-2 (+3.3V) | buffer/`PORT401` level = 3.3 V |
| `JP710` GND-SEL | Short | AGND and DGND common |

`JP704` and `JP706` are the two that must not be at +1.8 V: they set the codec's
digital I/O level and the `PORT401` buffer level respectively, and the Nucleo's
GPIOs are 3.3 V.

### Ground reference between the two boards

Two ground wires, one per bundle, both to `DGND` on the AKD4619 side:

| Bundle | AKD4619 side | Nucleo side |
| --- | --- | --- |
| I2S (5 signals) | any even pin of `PORT401`, pins 2–14 (manual p.68 **[schematic]**) | any GND pin on the ZIO/Morpho headers, near the I2S pins |
| I2C (2 signals) | `DGND` next to `TP601`/`TP602`, or `TP134` at the codec (manual p.65, p.70 **[schematic]**) | GND next to `PB8`/`PB9` |

`JP710` shorted (above) is what makes the board's AGND and DGND one reference,
so the analog loop and the digital ground agree. The supply return at
`J710`/`J711` is a separate, heavier path and is not a substitute for the signal
grounds.

## 6. Full settings checklist

Everything on the board, in one pass. "As shipped" means the manual's documented
default and no action needed.

| Designator | Kind | Setting | Why |
| --- | --- | --- | --- |
| `SW500` | toggle | **L → H once after power-up, then leave H** | codec out of power-down (manual p.26) |
| `SW501` | toggle | **L** | AK4118A powered down, out of the path (§2) |
| `SW502-1` SW-I2CN | DIP | **L** | I2C control (§1.1) |
| `SW502-2` SW-CAD | DIP | **L** | slave address `0x10` (§1.2) |
| `SW300` 1-5 | DIP | as shipped | AK4118A format/clock; not in the path |
| `PORT402` MCLK-SEL | 6-pin | **3-4 (PORT)** | MCLK from `PORT401` (manual p.22, Table 13) |
| `PORT403` BICK-SEL | 6-pin | **3-4 (PORT)** | BICK from `PORT401` |
| `PORT405` LRCK-SEL | 6-pin | **3-4 (PORT)** | LRCK from `PORT401` |
| `PORT406` SDOUT1-SEL | 6-pin | **3-4 (PORT)** | ADC data out to `PORT401` |
| `PORT408` SDIN1-SEL | 6-pin | **3-4 (PORT)** | DAC data in from `PORT401` |
| `PORT407` SDOUT2-SEL | 6-pin | **Open** (as shipped) | SDOUT2 unused |
| `PORT409` SDIN2-SEL | 6-pin | **5-6 (GND)** (as shipped) | SDIN2 tied low, not floating |
| `PORT401` | 14-pin | 5 signals + ground to the Nucleo | §3 |
| `PORT601` | USB-B | **unplugged** | isolates the on-board controller (§1.3) |
| `JP201` | 3-pin | **Open** | `IN1N` unused (§4.3) |
| `JP202` | 3-pin | **2-3** | `J201` → `IN1P` |
| `JP206` | 3-pin | **1-2** | `J202` → `IN2P` |
| `JP207` | 3-pin | **Open** | `IN2N` unused |
| `JP203` | 2-pin | **Short** | `J201`/`J202` signal GND to board GND |
| `JP208`, `JP209`, `JP213`, `JP214` | 3-pin | as shipped | ADC2 inputs, unused |
| `JP210` | 2-pin | as shipped | `J203`/`J204` signal GND, unused |
| `JP300` RX-SEL, `JP301` TX-SEL | 3-pin | as shipped (OPT) | AK4118A optical/coax, unused |
| `JP601` | 5-pin | **untouched** | PIC ICSP header (§1.3) |
| `JP701`…`JP710`, `JP730`…`JP732` | power | **all as shipped** | §5 |
| `J703` | jack | **+5.0 V in** | board supply |
| `J710`, `J711` | jack | **0 V** | supply return |
| `J701`, `J702`, `J730`, `J731` | jack | unused | §5 |
| `J210` | 3.5 mm | **loop cable out** | DAC1 → ADC1 |
| `J201`, `J202` | 3.5 mm | **loop cable in** | ADC1 Lch, Rch |
| `J203`, `J204`, `J211` | 3.5 mm | unused | — |
| `PORT300`, `PORT301`, `PORT303`, `PORT304` | optical/coax | unused | not this batch's path |
| `TP601`/`TP602` or `TP126`/`TP128` | test point | **I2C to the Nucleo** | §1.4 |

## 7. What #44 has to change in the overlay

1. Add `&i2s2_mck_pc6` to `i2s2`'s `pinctrl-0` and `mck-enabled;` to the `i2s2`
   node. `i2s3` gains neither.
2. Rewrite the "WHY SLAVE" paragraph: `i2s2` is now the clock master and `i2s3`
   the target, for the reasons in §2. The runtime half of that — the
   `I2S_OPT_*_TARGET` flags — changes only for the TX direction.
3. Re-derive the pin budget in the header comment: the claim "no `i2sN_mck_*`
   pin is claimed" is now false, and `PC6` joins the list. `PC6` and `PC7` are
   both free on this board (checked against `nucleo_h723zg.dts` at Zephyr
   `main`); nothing else in the budget moves.
4. `tests/boards/nucleo_h723zg/i2s_smoke/` asserts the absence of `mck-enabled`
   at build time. That assertion is now wrong for `i2s2` and has to move or
   invert.
5. Nothing changes for I2C: `audio-ctrl-i2c = &i2c1` on `PB8`/`PB9` already
   exists, and the codec node gets `reg = <0x10>`.

## UNRESOLVED

Called out rather than inferred, in the manner of #36's two unextractable
tables.

1. **The manual documents no external-I2C control mode.** Sections 5.1.2.13 and
   6.1 describe only USB control from the AKM Windows application. Everything in
   §1.3 and §1.4 — that the PIC is bus-powered, that the `PCA9306` `EN` pins
   follow `VUSB`, that `TP601`/`TP602` are the attach points — is read off
   schematic sheets 5 and 6 (manual pp.69–70), not from the body text. Before
   trusting a quiet bus, put a scope on SDA and SCL with the USB unplugged and
   confirm nothing is driving them, then confirm the codec ACKs at `0x10`.
2. **Which physical DIP position of `SW502` is "H".** Figure 25 (manual p.26)
   shows an `ON` legend above sections 1 and 2 but never says whether `ON`
   is H or L; sheet 5 shows each section selecting between TVDD and a 10 kΩ
   pull-down. Table 19 gives both defaults as L, so a board as shipped should
   need no change — but if the board is not as shipped, the level has to be
   measured rather than read off the silkscreen.
3. **Whether the `PORT401` even pins are all ground.** Table 18 (manual p.25)
   lists only the odd, signal pins. Sheet 4 shows pins 2–14 tied to `DGND`
   (manual p.68), which is why §3.1 states it — confirm with a meter before
   relying on any particular even pin.
4. **Tip/ring assignment of the 3.5 mm jacks.** The schematic gives contact
   numbers (`J210` pin 2 = `AOUTL`, pin 5 = `AOUTR`, pin 1 = sleeve; `J201`
   pin 2 = `AINP`, pin 5 = `AINN`, pin 1 = sleeve) but never maps those numbers
   to tip and ring. The five-contact body suggests sleeve/tip/ring for pins
   1/2/5 with 3 and 4 as switch contacts, which is not stated anywhere in the
   manual. Confirm with a plug and a meter before wiring the Y-cable; a swap
   here shows up as a channel swap, which #47 reports as a swap rather than a
   generic failure.
5. **Contradiction inside the manual about `JP203`/`JP210`.** Table 7 (manual
   p.18) describes both as "Select connection of signal GND and board GND.
   Short: Common GND, Open: Separate". Sheet 2 (manual p.66) annotates the same
   two jumpers "Short: Diff / SingleEnded, Open: Pseudo". They describe the same
   switch — the jack sleeve node to AGND — from two directions, and the board
   settles it. §4.3 follows Table 7's reading.
6. **The connector position of `PC6` on the Nucleo.** Established as free and as
   `i2s2`'s only MCK pin, but this document does not say which ZIO header pin it
   lands on; read it off UM2407 or the silkscreen when wiring MCLK.

## Open item that is not a manual question

The 3.5 mm Y-cable in §4.4 (one TRS male to two TS/TRS males) is a stock
"insert cable" but has to be on hand; a passing loop test needs it before
anything else in the batch can be validated on hardware.
