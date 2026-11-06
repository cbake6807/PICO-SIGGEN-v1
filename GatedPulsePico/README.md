# GatedPulsePico — networked gated pulse-train generator (Pico 2 W)

Port of [`GatedPulse`](../GatedPulse) (Arduino Nano) to a Raspberry Pi Pico 2 W.
Same idea — gate a square wave into bursts — but PIO does the gating in
hardware, so the Nano's software-AND compromise is gone and no 74HC08 is needed.

It can also generate its own carrier, so **no signal generator is required**.

```
  ext sig gen ──► GP3 ─┐
                       ├─► PIO gating SM ──► GP5   gated pulse train
  int carrier ──► GP2 ─┘        ▲            GP4   gate / burst marker
  (PIO gate_gen)                │
                                └─ COUNT: SM counts input pulses
                                   TIME : second gate_gen SM drives GP4
```

## Why the port was worth it

| | edge error | blind time | extra parts |
|---|---|---|---|
| Nano, software AND | 562 ns worst case | 1.7% | none |
| Nano + 74HC08 | ~10 ns | 0 | one chip |
| **Pico 2 W, PIO** | **~20 ns constant delay, ±6.7 ns jitter** | **0** | **none** |

PIO is independent hardware. WiFi, HTTP, USB and the encoder cannot perturb the
output — on the Nano, the main loop *was* the waveform.

> **3.3 V ONLY.** RP2350 GPIO is **not** 5 V tolerant, unlike the Nano this
> replaces. A 5 V sig-gen output **will damage GP3**. Use a divider (10k series
> + 20k to ground) or a 74LVC1G17 buffer on 3V3.

## Two gate modes

**COUNT** (GP10 open/HIGH, default) — the PIO SM counts input pulses: `ON`
pulses pass, `OFF` pulses are muted. It only raises the output after a rising
edge and lowers it after the matching falling edge, so **every burst is exactly
`ON` whole pulses, no runts, at any frequency**, and the burst rate tracks the
input. Counters are 32-bit (the Nano's 65536 ceiling is gone).

**TIME** (GP10 grounded/LOW) — a second PIO state machine generates a gate on
GP4 with independent 32-bit high/low counts, and the gating SM ANDs it with the
input via `JMP_PIN`. Range 1 µs to 60 s while still resolving 6.67 ns; the
Nano's Timer1 stopped at 4.19 s. Also runt-free: a gate edge landing mid-pulse
defers to the next whole pulse.

## Pins

| Pin | Function |
|---|---|
| GP2 | carrier out (internal source; also a plain sig-gen output to tap) — **idle while elongation is armed**, see below |
| GP3 | **SIG IN** — external square wave, **3.3 V max** |
| GP4 | gate / burst marker — **wire this to the scope trigger** |
| GP5 | **GATED OUT** — the burst train |
| GP6/GP7 | encoder 1 (CLK/DT) — gate ON/OFF pulses, or PERIOD/DUTY in TIME mode |
| GP8 | encoder 1 push (SW) — select which of the above the knob edits |
| GP9 | push — output enable/disable |
| GP10 | toggle — LOW = TIME, HIGH = COUNT |
| GP11/GP12 | encoder 2 (CLK/DT) — carrier frequency, or T1 width in T1/T2 mode |
| GP13 | encoder 2 push (SW) — toggle freq+duty vs T1/T2-independent carrier editing |
| GP14/GP16 | encoder 3 (CLK/DT) — carrier duty, or T2 space in T1/T2 mode |
| GP15 | status LED + resistor |
| GP17 | encoder 3 push (SW) — arm/disarm elongation (below) |

All inputs use internal pull-**ups**; RP2350 erratum E9 affects pads using the
internal pull-down. The onboard LED is not a GPIO on a Pico 2 W (it hangs off
the CYW43 chip), hence the external one.

**GPIO number ≠ physical header pin position** — e.g. GP6 is the pad labelled
`GP6`, but it sits at physical pin **9** on the header, not pin 6 (pin 6 is
GP4). Easy to miscount past the GND pins (3/8/13/18/23/28/33/38) when eyeballing
the board instead of reading the silkscreen. GP15 is skipped in the encoder 3
run (14 → 16) because it's the status LED; GP23/24/25/29 are avoided even
though this board doesn't break them out, since on a plain Pico W they're
wired straight to the CYW43 radio.

KY-040 wiring: CLK→A, DT→B, SW→SW, **VCC→3V3 (not 5V — see the warning
above)**, GND→GND. The module's own pull-ups and the RP2350's internal ones
just stack harmlessly.

## Three ways to drive it

Encoder, USB serial, and HTTP all share one command interpreter.

| Cmd | Action |
|---|---|
| `P` / `J` | state, human / one line of JSON |
| `DIAG` | bring-up diagnostic |
| `ON <n>` / `OFF <n>` | COUNT: pulses passed / muted per burst |
| `T <us>` / `G <Hz>` | TIME: gate period / frequency |
| `D <pct>` / `W <us>` | TIME: gate duty / open width |
| `SRC EXT\|INT` | gate the GP3 input, or the internal carrier |
| `C <Hz>` / `CD <pct>` | carrier frequency (implies `SRC INT`) / duty |
| `RUN <ms>` | silent → output for `<ms>` → silent |
| `SHOT` | fire exactly **one burst**, then stop (COUNT mode) |
| `STOP` | end a run / disable now |
| `M <0\|1>` `E <0\|1>` `I` `V` `R` | mode, enable, invert gate, invert pulses, defaults |

HTTP: `GET /state` (JSON), `GET /cmd?c=<command>`, `GET /` (web UI).
mDNS as `gatedpulse.local`. WiFi credentials go in `secrets.h` (gitignored —
copy `secrets.h.example`). Without it the firmware still runs, just offline.

## Why `RUN <ms>` matters

It is what makes a scope capture correspond to a known experiment:

1. Generator is silent.
2. Arm the scope **single-shot**, triggered on the GP4 burst marker.
3. `RUN 250`.
4. The first burst edge triggers the capture.

Since nothing is emitted before the run, the record **cannot** contain anything
else. No post-hoc isolation needed. `run_experiment` in the MCP server does
exactly this sequence (`single_burst: true` uses `SHOT`).

`SHOT` fires exactly one burst by setting the mute count to its 32-bit maximum,
so the state machine emits its `ON` pulses and then parks in the mute loop for
4.29e9 input pulses — over a day at 10 kHz. The burst is bounded by the PIO
rather than by the CPU racing to halt it, so it can never be truncated mid-pulse.

**No leading off-time.** The SM enters the program at the `burst` label, so the
first burst begins immediately rather than after an `OFF` period. This is
deliberate: a triggered capture wants the burst on the trigger edge, not
`off/carrier` seconds later. Every subsequent cycle has the full off-time.

### Verified on hardware
Flashed to a Pico 2 W and checked against a Siglent SDS1104X-E:
- PIO resources were claimed on **pio2 sm0** (not pio0) — hardcoding the block
  would have failed, which is why claiming is dynamic.
- Burst rate tracks `carrier/(on+off)` exactly: 20 kHz with 5 on / 25 off gave
  667 Hz at 16.67 % duty.
- `SHOT` produced a single 10-pulse burst; the marker measured 1 ms wide with
  no repetition rate at all, because there is only one pulse.

## PIO notes

- Both gating engines live in **one** program (`gating.pio`, 26 instructions,
  COUNT entry +0, TIME entry +17) so dynamic PIO claiming can never split them
  across blocks.
- Resources are claimed with `pio_claim_free_sm_and_add_program`, **not**
  hardcoded to `pio0`/`pio1`. On a Pico W the CYW43 WiFi chip is itself driven
  by a PIO state machine allocated the same way — hardcoding would risk
  silently fighting it. RP2350 has three PIO blocks, so this always succeeds.
- The sketch carries hand-maintained instruction encodings because arduino-cli
  does not run pioasm. `make pioverify` re-assembles the `.pio` sources and
  diffs them, so the two cannot drift.

## Build

```
make pico          # compile
make pico-flash    # compile + upload
make pioverify     # prove the PIO arrays match the .pio sources
```

## Host tooling — see [../tools](../tools)

`tools/gatedpulse.py` (HTTP client + CLI), `tools/siglent.py` (scope over
SCPI), `tools/mcp_server.py` (MCP server exposing both).
