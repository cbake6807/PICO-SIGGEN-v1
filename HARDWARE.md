# Hardware — building the standalone box

The board is a complete instrument on its own. Flash it, wire three encoders and
two switches, and you have a gated pulse generator with a front panel — no
computer, no WiFi, nothing else to install. The optional WiFi web UI below
layers on top of that; it does not replace any of it.

## Minimum build

| Qty | Part | Notes |
|---|---|---|
| 1 | **Raspberry Pi Pico 2 W** | what it is developed on |
| 3 | rotary encoder, EC11 type with push switch | the common 20-detent module |
| 2 | momentary push button | output enable, output mode |
| 1 | SPDT toggle switch | TIME / COUNT mode |
| 1 | LED + 330 Ω | status |

That's it. Encoder breakout modules with the resistors already on them work
fine; so do bare EC11s, because every input uses the RP2350's internal pull-up.

## Pin map

Two numbers matter and they are not the same one. **GP** is what the firmware
and every datasheet call the pin; **board pin** is what you count along the
header when you are actually soldering. Both are given below — mixing them up
is the single easiest mistake to make here.

| GP | Board pin | Function | Direction |
|---|---|---|---|
| **GP2** | 4 | internal carrier — also a plain square-wave output to tap | out |
| **GP3** | 5 | external signal input — **3.3 V ONLY** | in |
| **GP4** | 6 | gate / burst marker, high for every burst | out |
| **GP5** | 7 | **the gated pulse train** — the main output | out |
| GP6 / GP7 | 9 / 10 | encoder 1 A / B | in |
| GP8 | 11 | encoder 1 push | in |
| GP9 | 12 | output enable button | in |
| GP10 | 14 | TIME / COUNT toggle | in |
| GP11 / GP12 | 15 / 16 | encoder 2 A / B | in |
| GP13 | 17 | encoder 2 push | in |
| GP14 / GP16 | 19 / 21 | encoder 3 A / B | in |
| GP15 | 20 | status LED (+ resistor) | out |
| GP17 | 22 | encoder 3 push | in |
| GP18–GP20 | 24 / 25 / 26 | channel outputs (offset & sync modes), or amplitude / tap select | out |
| **GP21** | **27** | **output-mode button** — single / offset / sync | in |
| **GP22** | **29** | **cycle marker** — burst 1 of the pattern | out |
| **GP26** | **31** | **HV enclosure interlock** — closed to ground = safe | in |

Free for your own use: GP0, GP1 (I²C), GP27 / GP28 (ADC).

## Selecting channels, and what the pins do in sweep mode

`PHASE CH 2 3` picks which of GP18/19/20 take part; a bare `PHASE 2` still means
"the first two". **Offset** rotates across only the selected channels, **sync**
fires only those together, and a deselected channel is held low at the pad in
every mode — worth knowing if you ever lose a driver and want to keep running on
the survivors. At least one channel always stays selected.

`FSWEEP` is a different animal from the gated modes. It sweeps a continuous
50 % square from ~0 up to a ceiling (default 10 kHz) and back, with no gating at
all, and it changes what two pins mean:

| Pin | In gated modes | In `FSWEEP` |
|---|---|---|
| **GP4** | burst marker | **sweep marker** — high on the way up, low on the way down |
| **GP5** | the pulse train | the continuous swept tone |
| **GP18–20** | channels, rotated or synced | all selected channels together, same tone |
| **GP2** | internal carrier monitor | mirrors the swept tone |

GP4 stays your scope trigger either way; in sweep mode its level also tells you
which direction you are watching. **Encoder 2** — normally carrier frequency —
becomes the sweep speed while a sweep is running, since there is no fixed
frequency to set.

Three channels conducting at once is three times the draw on the shared rail.
If they are taps of one winding rather than separate cells, simultaneous
conduction shorts the turns between them; the one-hot decoder that normally
makes that unreachable is bypassed in this mode.

## The interlock on GP26

Off by default. Turn it on from the **sys** tab or with `LOCK 1`, and only once
something is actually wired, because an unwired input reads open and cuts the
output — which is the entire point.

Wire a normally-closed switch, a reed, or a Hall sensor between **GP26 (pin 31)
and GND (pin 33)**. Closed to ground is the safe state, and that polarity is
chosen so every failure lands on "unsafe": a cut wire, a dead sensor, or a
connector never plugged in all float to the internal pull-up and read open. The
opposite polarity would report a closed lid for a disconnected cable.

Trips **latch**. Closing the lid does not restart the output; you have to press
**ARM** (or send `ARM`), and the firmware refuses that until the input has been
steadily closed for 250 ms, so an intermittent contact cannot be armed into. A
reboot always comes up tripped — whatever the enclosure was doing when power
went away is not knowable afterwards.

Using an **A3144 / KY-003** Hall module for this: lift its onboard pull-up and
LED and let the Pico's internal pull-up do the work, so the open-collector
output swings 0–3.3 V. Do not feed the module's 5 V output to GP26 through a
divider — a divider to ground reads *low* when the cable is unplugged, which
inverts the fail-safe behaviour and is exactly the failure this input exists to
catch. Add 1 nF at the pin if the run passes near the coils.

**This is a reminder, not a guard.** A magnet defeats it in seconds. The bleeder
resistor and a meter check across the capacitor stay mandatory.

Ground is at board pins 3, 8, 13, 18, 23, 28 and 38 — every switch has one
within a pin or two of itself. The two panel buttons are especially convenient:
**GP21 at pin 27 and GP22 at pin 29 both sit next to the ground at pin 28.**

Held component-side up with the **USB connector at the top** — pin 1 is the
first pin on the left, right beside the USB:

```
        ┌───USB────┐
GP0   1 │•        •│ 40  VBUS
GP1   2 │•        •│ 39  VSYS
GND   3 │•        •│ 38  GND
GP2   4 │• carrier•│ 37  3V3_EN
GP3   5 │• sig in •│ 36  3V3 OUT
GP4   6 │• gate   •│ 35  ADC_VREF
GP5   7 │• OUTPUT •│ 34  GP28
GND   8 │•        •│ 33  AGND        <-- interlock grounds here
GP6   9 │• enc1 A •│ 32  GP27
GP7  10 │• enc1 B •│ 31  GP26   INTERLOCK    <-- IN (low = safe)
GP8  11 │• enc1 sw•│ 30  RUN
GP9  12 │• out en •│ 29  GP22   cycle marker  <-- OUT
GND  13 │•        •│ 28  GND         <-- both buttons ground here
GP10 14 │• T/C sw •│ 27  GP21   mode button   <-- IN
GP11 15 │• enc2 A •│ 26  GP20   channel 3
GP12 16 │• enc2 B •│ 25  GP19   channel 2
GP13 17 │• enc2 sw•│ 24  GP18   channel 1
GND  18 │•        •│ 23  GND
GP14 19 │• enc3 A •│ 22  GP17   enc3 sw
GP15 20 │• LED    •│ 21  GP16   enc3 B
        └──────────┘
```

If you are holding the board with the USB pointing away from you, flip this
top-for-bottom — the fastest sanity check is the debug/BOOTSEL button and the
onboard LED, which sit at the USB end.

## Wiring

**Everything is active-low with an internal pull-up.** Encoder common pins and
one side of every switch go to **GND**; the other side goes to the GPIO. No
external resistors.

```
   encoder                 button / toggle
   ┌───────┐
   │  A ───┼── GPIO           ┌─────┐
   │ GND ──┼── GND      GPIO ─┤     ├─ GND
   │  B ───┼── GPIO           └─────┘
   │ SW ───┼── GPIO
   │ GND ──┼── GND
   └───────┘
```

Pull-**ups** throughout is deliberate, not a preference: RP2350 erratum E9
affects pads using the internal pull-down, so the firmware avoids them entirely.

**Do not power encoder modules from VBUS (pin 40).** Bare EC11s need no supply
at all -- they are switches, and the internal pull-ups do the rest. Breakout
modules have their own pull-ups tied to `VCC`, so whatever you feed `VCC` is
what appears on the GPIO when the contact is open: 5 V from VBUS puts 5 V on a
3.3 V-only pin. Use **3V3 OUT, pin 36**, or leave `VCC` unconnected entirely
(only an onboard LED stops working). VBUS is also dead whenever the board runs
from VSYS instead of USB, so a panel wired to it works on the bench and goes
silent in the enclosure.

**GP3 is not 5 V tolerant.** Nothing on the RP2350 is. A bench function
generator at 10 Vpp, or a 5 V TTL source, destroys the chip the first time you
plug it in. If you bring GP3 out to a connector, put a buffer behind it — a
74LVC1G17 run at 3.3 V, or at minimum a series 1 kΩ with a BAT54S clamp to
3V3/GND.

## What the controls do

**Encoder 1** — burst structure.
Press to switch which value it edits.

| Mode | Turning it |
|---|---|
| COUNT | **ON pulses** per burst — exactly 1 per click, no acceleration |
| COUNT (pressed) | **OFF pulses** — accelerates when you spin it |
| TIME | gate **period** |
| TIME (pressed) | gate **duty** |

ON pulses stays linear on purpose. It is a count you dial to an exact number —
"give me 7" — and an acceleration multiplier makes that target unhittable.

**Encoder 2 / Encoder 3** — the carrier.
Encoder 2's push toggles what both of them mean:

| | Encoder 2 | Encoder 3 |
|---|---|---|
| freq+duty mode | carrier frequency | carrier duty |
| T1/T2 mode | **T1** (pulse width) | **T2** (space) |

Both are multiplicative, so they work across the whole range: **0.1 %** per
click turning slowly, 2 % at a medium pace, 50 % on a fast flick — a decade in
about six clicks. At 25 kHz the slow tier is 25 Hz per click.

**Encoder 3's push** toggles elongation on and off — each pulse in a burst
compounding off the last.

**GP9 button** — output on/off.
**GP10 toggle** — TIME mode (low) or COUNT mode (high).

**GP21 button** (board pin 27, ground at 28) — cycles the output mode:
**single → offset → sync**. The status LED blinks the position back at you
(1, 2 or 3), since a standalone box has no display and otherwise the only way to
know is to plug in USB.

| Mode | What comes out |
|---|---|
| single | one train on GP5; GP18–20 idle |
| offset | GP18, GP19, GP20 take one burst each in turn, separated by the off-time |
| sync | all three fire the same burst together, every burst |

These are three independent axes, so any combination works — elongation with
offset, sweep with sync, all three at once:

| Control | Axis |
|---|---|
| GP21 press | output: single / offset / sync |
| Encoder 3 press | elongation on / off |
| Encoder 3 **hold** | sweep on / off (re-arms whatever `RAMP1` was last given) |

## Outputs

**GP5** (pin 7) is the pulse train — the one you drive things with. It carries
every pulse in every mode, so it stays a valid monitor no matter which channel
is live.

**GP4** (pin 6) goes high for the whole of *every* burst.

**GP18, GP19, GP20** (pins 24, 25, 26) are the channels. In offset mode each one
carries one burst in turn; in sync mode all three carry the same burst.

**GP22** (pin 29) is the cycle marker, and it exists because GP4 cannot tell you
*which* burst you are looking at. In offset mode the pattern is three bursts
long, GP4 pulses on all three identically, and a scope triggering on it has no
way to stay put — hence the usual workaround of triggering off one channel with
holdoff, which has to be retuned every time the pattern length changes.

GP22 goes high for the whole of burst 1 and stays low for the rest of the
pattern, so **trigger on its rising edge with holdoff off** and the pattern sits
still. It is generated from the same DMA word that picks the channel, on the
same state machine, so it cannot drift from the burst it marks by even a clock.

It is idle (low) whenever the pattern is only one burst long — single mode, or
sync with no train ring — since there is then nothing to distinguish.

**GP2** (pin 4) is the raw carrier, useful as a plain signal-generator tap or
for checking what the gate is chopping.

Series 33–100 Ω at each output is worth adding: it damps ringing on a coax run
and survives a shorted load.

## Serial

USB serial at any baud, `?` for help. This works with no WiFi at all — a
terminal is enough to reach every feature the knobs expose and several they
don't (`SEQ`, `TRAIN`, `RAMP1/2`, `INPUTS`).

## Optional: amplitude / tap select

GP18–GP20 carry a 3-bit code, emitted in the same DMA stream as the pulse
timing, changing during the gap *before* each pulse so it has settling time.
Nothing needs to be connected — with the pins unwired they just toggle into open
air and every other feature works normally.

To use it, feed them into a **74HC238** (3-to-8, active-high outputs) and gate
its enable with GP5. One decoder output per drive leg, and **leave Y0
unconnected** — the firmware pushes code 0 to park deselected between bursts,
which only means "off" if nothing hangs on Y0.

A decoder rather than three independent enables because two taps of one winding
conducting simultaneously shorts the turns between them. One-hot in hardware
makes that combination unreachable no matter what the firmware asks for.

See [AMPLITUDE_SEQUENCING.md](AMPLITUDE_SEQUENCING.md) for the drive legs.

## Optional: WiFi

Copy `GatedPulsePico/secrets.h.example` to `secrets.h` and fill it in. Without
that file the firmware still builds and runs — it just stays offline, which is
the standalone case above. The include is guarded by `__has_include`, so a
missing `secrets.h` is not a compile error.

With WiFi it serves its own web UI and a JSON endpoint at the address it prints
to serial on boot — faders for the carrier and the train ring, a live front-panel
wiring checker, and every command the serial console takes. Nothing to install
on the phone or PC, and nothing is fetched from the internet, so it works on an
isolated workshop network.
