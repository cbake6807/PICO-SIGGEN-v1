# Gated Pulse

A gated pulse-train generator on a Raspberry Pi Pico 2 W, built for water-fuel-
cell bench work but useful for anything that wants precisely-timed bursts.

Pulse timing is generated entirely in PIO state machines, so widths land on
exact integer clocks — a 1.000 µs pulse is **150 clocks at 150 MHz**, not
"about a microsecond". PIO is independent hardware: WiFi, HTTP, USB and the
encoders cannot perturb the output. That is not a claim, it is measured — the
output width does not move by a single sample while the board is serving its
web UI to four clients at once.

```
                      ┌──────────────┐
   ext sig gen ─ GP3 ─┤              ├─ GP5   the gated pulse train
                      │  PIO gating  ├─ GP4   gate / burst marker
   int carrier ─ GP2 ─┤              ├─ GP18/19/20  channels
   (also a PIO SM)    │              ├─ GP22  cycle marker
                      └──────────────┘
```

**No signal generator needed** — it makes its own carrier. **No computer
needed** — three encoders and two buttons make it a complete instrument.

---

## What it does

| | |
|---|---|
| **Bursts** | N pulses on, N muted, repeating — or a timed gate window |
| **Elongation** | each pulse in a burst compounding off the last |
| **Sequences** | an explicit per-pulse width table, not just a ratio |
| **Train ring** | a per-burst amplitude envelope spanning many bursts |
| **Channels** | rotate the train across 3 outputs, or fire them together |
| **Sweep** | step T1 across bursts to hunt a resonance |
| **Persistence** | settings survive power-off; can start pulsing on power-on |
| **Web UI** | optional — served by the board itself over WiFi |

---

## 1. What you need

| Qty | Part | Notes |
|---|---|---|
| 1 | **Raspberry Pi Pico 2 W** | what it is developed on |
| 3 | rotary encoder, EC11 with push switch | the common 20-detent module |
| 2 | momentary push button | output enable, output mode |
| 1 | SPDT toggle switch | TIME / COUNT |
| 1 | LED + 330 Ω | status |

Breakout modules with the resistors fitted work fine, and so do bare EC11s —
every input uses the RP2350's internal pull-up, so there is nothing else to add.

> A non-W **Pico 2** works for everything except WiFi (build with
> `FQBN=rp2040:rp2040:rpipico2`), but that combination is untested here.

---

## 2. Flash it

### Install the toolchain, once

Install [arduino-cli](https://arduino.github.io/arduino-cli/latest/installation/),
then add the RP2040/RP2350 core:

```bash
arduino-cli config init
arduino-cli config set board_manager.additional_urls https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
arduino-cli core update-index
arduino-cli core install rp2040:rp2040
```

Each command is a single line, so these work as-is in Windows Command Prompt
and PowerShell as well as macOS/Linux shells.

That core is the community RP2040/RP2350 one by earlephilhower — the official
Arduino Mbed core will **not** build this.

### Build and upload

**First flash only:** hold the **BOOTSEL** button while plugging in the USB
cable. The board appears as a USB drive, which is how the uploader reaches a
board with no firmware on it. After that, uploads reset it automatically and
you never touch BOOTSEL again.

```bash
arduino-cli compile --fqbn rp2040:rp2040:rpipico2w GatedPulsePico
arduino-cli upload  --fqbn rp2040:rp2040:rpipico2w --port PORT GatedPulsePico
```

Find `PORT` with `arduino-cli board list`:

| OS | looks like |
|---|---|
| Windows | `COM4` |
| macOS | `/dev/cu.usbmodem14201` |
| Linux | `/dev/ttyACM0` |

On macOS and Linux, `make flash` does both and finds the port itself.

### If it goes wrong

**"No such file or directory" / port missing.** The board only shows a serial
port once it is running firmware. On a blank board, use BOOTSEL.

**Upload hangs or "device not found".** Hold BOOTSEL, replug, and check for a
drive called `RP2350`. If it appears, the board is fine — the uploader just
could not reset it.

**"Multiple libraries found" or a core error.** You have the Arduino Mbed core
installed too. Be explicit with the full FQBN above.

**Linux permission denied on `/dev/ttyACM0`.** Add yourself to `dialout`:
`sudo usermod -aG dialout $USER`, then log out and back in.

**It compiles but nothing comes out.** Output starts disabled unless you
configure otherwise — press the GP9 button, or send `E 1`. See §6.

---

## 3. Wire it

Full detail, including the header diagram and the orientation cue, is in
**[HARDWARE.md](HARDWARE.md)**. The short version:

| GP | Board pin | Function |
|---|---|---|
| **GP2** | 4 | internal carrier — also a plain square-wave tap |
| **GP3** | 5 | external signal input — **3.3 V ONLY** |
| **GP4** | 6 | gate / burst marker |
| **GP5** | 7 | **the gated pulse train** — the main output |
| GP6 / GP7 / GP8 | 9 / 10 / 11 | encoder 1 A / B / push |
| GP9 | 12 | output enable button |
| GP10 | 14 | TIME / COUNT toggle |
| GP11 / GP12 / GP13 | 15 / 16 / 17 | encoder 2 A / B / push |
| GP14 / GP16 / GP17 | 19 / 21 / 22 | encoder 3 A / B / push |
| GP15 | 20 | status LED (+ resistor) |
| GP18 / GP19 / GP20 | 24 / 25 / 26 | channel outputs |
| **GP21** | 27 | output-mode button |
| **GP22** | 29 | cycle marker |

Free for your own use: GP0, GP1 (I²C), GP26–GP28 (the only ADCs).

**Everything is active-low with an internal pull-up.** Encoder commons and one
side of every switch go to **GND**; the other side goes to the GPIO. No external
resistors. Both panel buttons are conveniently placed — GP21 (pin 27) and GP22
(pin 29) sit either side of the ground at pin 28.

Pull-*ups* throughout is deliberate rather than a preference: RP2350 erratum E9
affects pads using the internal pull-down, so the firmware avoids them entirely.

> ### GP3 is not 5 V tolerant
> Nothing on the RP2350 is. A bench function generator at 10 Vpp, or a 5 V TTL
> source, destroys the chip the first time you plug it in. If you bring GP3 out
> to a connector, buffer it — a 74LVC1G17 at 3.3 V, or at minimum a series 1 kΩ
> with a BAT54S clamp to 3V3/GND.

---

## 4. Drive it from the panel

**Encoder 1 — burst structure.** Press to switch which value it edits.

| Mode | Turning it |
|---|---|
| COUNT | **ON pulses** per burst — exactly 1 per click, no acceleration |
| COUNT (pressed) | **OFF pulses** — accelerates when you spin it |
| TIME | gate **period** |
| TIME (pressed) | gate **duty** |

ON pulses stays linear on purpose. It is a count you dial to an exact number —
"give me 7" — and an acceleration multiplier makes that target unhittable.

**Encoders 2 and 3 — the carrier.** Encoder 2's push swaps what both mean:

| | Encoder 2 | Encoder 3 |
|---|---|---|
| freq+duty mode | carrier frequency | carrier duty |
| T1/T2 mode | **T1** (pulse width) | **T2** (space) |

Both are multiplicative, so one control spans the whole range: **0.1 %** per
click turning slowly, 2 % at a medium pace, 50 % on a fast flick — a decade in
about six clicks. At 25 kHz the slow tier is 25 Hz per click.

**Buttons**

| Control | Does |
|---|---|
| GP9 | output on / off |
| GP10 toggle | TIME mode (low) or COUNT mode (high) |
| **GP21** | output mode: single → offset → sync |
| **Encoder 3 press** | elongation on / off |
| **Encoder 3 hold** (0.7 s) | sweep on / off |

These are **three independent axes** — elongation with offset, sweep with sync,
or all three at once are all reachable. The status LED blinks the position back
at you (1 / 2 / 3), because a standalone box has no display and otherwise the
only way to know which mode a press landed on is to plug in USB.

---

## 5. Drive it from a browser (optional)

Copy the example and fill it in:

```bash
cp GatedPulsePico/secrets.h.example GatedPulsePico/secrets.h
```

```c
#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"
#define MDNS_NAME "gatedpulse"   // -> http://gatedpulse.local/
```

Reflash. The board prints its IP to USB serial on boot, and serves its own UI
there — no software to install on the phone or PC, and nothing is fetched from
the internet, so it works on an isolated workshop network.

The page has vertical faders for frequency, duty, T1 and T2 (all four
cross-linked — move one and the others follow), pulses per burst, elongation
ratio, and a fader per burst for the train ring. Tabs cover carrier, burst,
channels, shape, train, panel and system.

Two things there are worth knowing about before you need them:

**`known state`** in the header — one click, no dialog. 100 kHz / 50 %, 10 on /
90 off, internal carrier, elongation and sweep and seq and train all off,
**offset ×3**, output on. When you have been turning knobs for an hour and
nothing comes out, press this before debugging anything.

It deliberately lands on offset ×3 rather than single, so GP18–20 *and* the
GP22 cycle marker all come alive — single mode idles four of the five pins you
are most likely to have a probe on, which makes a reset look like a failure.

**The `panel` tab** is a live wiring check, described in §7 below. The activity
log it feeds sits at the bottom of *every* tab, so you can watch a knob move the
parameter you expected while looking at that parameter.

**`secrets.h` is optional and gitignored.** Without it the firmware still builds
and runs — it just stays offline, which is the standalone case above. The
include is guarded by `__has_include`, so a missing file is not a build error.
It is a real difference, not a token one: **141 KB offline versus 440 KB with
WiFi**, since the network stack, web server, mDNS and the UI all compile out.

> No authentication. Anyone on your network can drive the generator. That is
> the right trade for a bench instrument on an isolated network and the wrong
> one anywhere else — do not port-forward it.

---

## 6. Drive it from a terminal

USB serial at any baud. `?` prints the full list. Serial reaches every feature,
including several the knobs do not expose.

```
P              print state              J     state as one line of JSON
DIAG           bring-up diagnostic      ?     help
INPUTS         live front-panel pin levels (wiring check)

M 1 / M 0      COUNT mode / TIME mode
E 1 / E 0      output on / off
ON <n>         pulses passed per burst  OFF <n>   pulses muted per burst
SHOT           fire exactly ONE burst   RUN <ms>  output for ms, silent either side
STOP           end a RUN / disable now
LOCK 0|1       HV enclosure interlock on GP26 (bare LOCK reports state)
ARM            clear a tripped interlock (refused unless GP26 is closed)

SRC INT|EXT    internal carrier, or the GP3 input
C <Hz>         carrier frequency        CD <pct>  carrier duty
T <us>         TIME: gate period        G <Hz>    TIME: gate frequency
D <pct>        TIME: gate duty          W <us>    TIME: gate open width
I              invert gate              V         invert pulse polarity

ELONGATE <r>   each pulse grows, total span r across the burst
ELONGATE PHI   golden-ratio growth per pulse
ELONGATE OFF   flat train of the base pulses
SEQ 1,1 2,1 4,1      explicit per-pulse widths, t1_us,t2_us[,amp]
SEQ / SEQ OFF        print the table / drop it
TRAIN 100 75 50 25   per-burst amplitude ring, % of base T1
TRAIN / TRAIN OFF    print the ring / every burst identical
RAMP1 <step_us> <bursts> <limit_us> [STOP|WRAP]   sweep T1 across bursts
RAMP2 ...            same, for T2        RAMP1|2 OFF   cancel, holding value

PHASE ROT <n>  rotate across n channels, one burst each
PHASE SYNC <n> all n channels fire together every burst
PHASE OFF      single output on GP5      PHASE DUMP    print the DMA tables
SHAPE FD|T12   what encoders 2 and 3 edit

SAVE / LOAD / FORGET      store, restore, erase settings in flash
AUTOSAVE 0|1              auto-store ~5 s after the last change (default on)
BOOT RUN|OFF|SAVED        what the output does at power-on
R              restore defaults          REBOOT   restart the board
```

Over WiFi the same commands work as `http://<board>/cmd?c=<command>`, and
`http://<board>/state` returns the JSON.

---

## 7. The features, in more detail

**Elongation** reshapes pulses *within* one burst — each pulse compounds off the
last by a fixed factor, so `ELONGATE 8` spans 8× from first to last. Pulse 1
stays at the current T1/T2.

**SEQ** replaces that ratio with an explicit table when you want a shape a ratio
cannot describe. Up to 100 steps.

**TRAIN** is a ring of amplitudes applied *across* bursts — burst 1 at 100 %,
burst 2 at 75 %, and so on, repeating. It composes with the two above: TRAIN
scales, they shape. For an inductive primary the current at turn-off is
`i = V·T1/L`, so narrowing every T1 in a burst by the same fraction attenuates
that whole burst without changing its shape. T2 is deliberately *not* scaled, so
the burst occupies the same time on screen at every amplitude and your scope
framing does not move as the ring cycles.

**Channels.** In *offset* mode GP18, GP19 and GP20 take one burst each in turn,
separated by the off-time. In *sync* mode all three fire together. GP5 carries
every pulse in every mode, so it stays a valid monitor whichever channel is live.

**The cycle marker (GP22)** exists because GP4 pulses on *every* burst, so in
offset mode a scope has no way to tell burst 1 from burst 3 and the trace walks.
GP22 is high for the whole of burst 1 of the pattern and low for the rest —
**trigger on it with holdoff off** and the pattern sits still. It rides the same
DMA word that selects the channel, so it cannot drift from the burst it marks by
even a clock. It idles low when the pattern is only one burst long.

**Persistence.** Settings are stored in a flash sector with a CRC and a version
stamp, at a fixed address so they survive reflashing. `BOOT RUN` starts the
output at power-on — for a box that lives in a rig and should come back up
pulsing after a blip. Autosave writes ~5 s after the last change, which is
worth knowing: **a ring you armed once will come back every power-on**, so
clear what you do not want persisted.

---

## 7b. When an encoder does not work

Open the **panel** tab. It shows every input's live level and logs three
separate things, which is what lets you tell the failures apart:

| what you see | what it means |
|---|---|
| pin never changes when you turn the knob | dry joint, wrong pin, or the common is not grounded |
| A and B both toggle but no `STEP` line | A and B are swapped on that encoder |
| clean edges but `STEP` jumps by two | contact bounce — 100 nF from each pin to ground |
| pin stuck **LOW** untouched | shorted to ground |
| pin stuck **HIGH** with the contact closed | common not connected to GND |

All inputs idle HIGH — they are pulled up internally and the contacts pull them
down, so LOW means "made".

The log is deliberately **not** debounced. Bounce is one of the things you are
looking for, and a filtered log hides the noisy contact that is causing the
miscounts.

The most useful test is boring and systematic: turn each knob one detent left,
then one right, then press it, touching nothing else. That turns a wall of
edges into three comparable groups, and a pin that never appears at all — or
one that appears when it should not — stands out immediately.

`INPUTS` over serial prints the same pin table with both GP and board numbers,
so this works with no WiFi.

## 8. Safety

The Pico's GPIO is **3.3 V and not 5 V tolerant**. Buffer anything you bring in
from outside the box.

If you are driving anything through a step-up transformer, read
**[AMPLITUDE_SEQUENCING.md](AMPLITUDE_SEQUENCING.md)** first. Rectified and
filtered high voltage stores real energy — a 100 µF cap at 460 V holds about
10 J, which stays lethal long after the power is off, and wants a bleeder
resistor and a meter check before you touch anything.

If you drive multiple taps of a single winding, use a one-hot decoder rather
than independent enables. Two taps conducting simultaneously shorts the turns
between them, and one-hot in hardware makes that combination unreachable no
matter what the firmware asks for.

Nothing here is safety-rated. See [LICENSE](LICENSE).

---

## 9. Repo layout

```
GatedPulsePico/
  GatedPulsePico.ino     the firmware
  webui.h                the onboard web UI (PROGMEM HTML)
  *.pio                  readable PIO sources — see below
  secrets.h.example      copy to secrets.h for WiFi
HARDWARE.md              pin map, wiring, what every knob does
AMPLITUDE_SEQUENCING.md  driving a VIC, amplitude control, HV parts
docs/DESIGN_NOTES.md     why the firmware is built this way; PIO gotchas
tools/pioverify.py       proves the PIO encodings match their sources
```

## 10. Working on the firmware

`arduino-cli` does not run `pioasm` during a sketch build, so the sketch carries
the assembled PIO words as C arrays and the `.pio` files are the readable
source. They can therefore drift, which would be a waveform bug you chase on a
scope instead of in a diff. So after touching either:

```bash
make pioverify
```

It re-assembles every `.pio` and diffs the result against the matching
`*_insns[]` array:

```
  elongate       OK     22 instructions
  gate_gen       OK     10 instructions
  gating         OK     26 instructions
PIO verification: all programs match
```

Two things in the timing are worth knowing before you change the PIO:

- Every phase count is compensated for loop overhead, derived
  instruction-by-instruction rather than measured — `hi = pushed+4`,
  `lo = pushed+9`, `mute = pushed+11`. Insert an instruction into one of those
  windows and the corresponding constant moves.
- `out pins, n` does **not** leave the pins above `n` alone. The pin-write mask
  comes from the group's configured `OUT_COUNT`, not the instruction's bit
  count, so a narrow OUT zero-fills the rest of the group. This contradicts the
  natural reading of the instruction set and cost a bench session to find; see
  [docs/DESIGN_NOTES.md](docs/DESIGN_NOTES.md).

## License

MIT — see [LICENSE](LICENSE).
