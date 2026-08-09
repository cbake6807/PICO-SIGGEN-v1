# GatedPulsePico — front panel + elongation, bring-up notes

The whole project (this used to live at `~/tripleandseq/dualtripleseq`) now
lives right here — `GatedPulsePico/` has the actual `.ino`/`.pio` files and
their own README, `tools/` has the dashboard/MCP server. This file is a
standalone summary + tomorrow's bring-up checklist sitting at the repo root.

Status as of tonight: **everything below compiles clean, `make pioverify`
passes, and the math has been cycle-simulated in Python — but none of it has
been flashed or run on real hardware yet.** The Pico was powered down before
any of this got tested live. First bring-up is tomorrow.

---

## 1. What's new: three rotary encoders

The front panel went from one encoder to three. Pin table (GPIO number, which
is **not** the same as physical header pin position — see gotcha below):

| Encoder | CLK | DT | SW | Controls |
|---|---|---|---|---|
| 1 (existing) | GP6 | GP7 | GP8 | Gate ON/OFF pulses (COUNT mode) or PERIOD/DUTY (TIME mode). SW press cycles which one the knob edits. |
| 2 (new) | GP11 | GP12 | GP13 | Carrier **frequency**, or **T1 width** if in T1/T2 edit mode. SW press toggles freq+duty ↔ T1/T2-independent editing (`SHAPE` command). |
| 3 (new) | GP14 | GP16 | GP17 | Carrier **duty**, or **T2 space** if in T1/T2 edit mode. SW press arms/disarms elongation (below). |

**Gotcha already hit once tonight:** GPIO number ≠ physical pin position on
the header. GP6 is physically pin **9**, not pin 6 (pin 6 is GP4 — gate out).
Easy to miscount past the GND pins (3/8/13/18/23/28/33/38). Read the `GP`
label, don't count holes.

KY-040 wiring: CLK→A, DT→B, SW→SW, **VCC→3V3 (not 5V — RP2350 GPIO is not 5V
tolerant)**, GND→GND.

**Verified tonight**: after fixing a wiring mix-up (turning the knob was
spuriously triggering the button — traced to a bad connection, not a firmware
bug), both new encoders were confirmed working with `ENCLOG 1` live-logging
raw quadrature transitions and steps to serial.

If a knob still seems dead tomorrow: `ENCLOG 1`, watch serial while turning
it. No `enc2 raw:`/`enc3 raw:` lines at all = wiring/GND problem on that
encoder specifically, not firmware. Turn back off (`ENCLOG 0`) once done —
it's chatty.

---

## 2. Two carrier-shaping features — don't confuse them

Both operate on the internal carrier's T1 (on-width) / T2 (space), but at
completely different timescales:

- **Sweep** (`RAMP1`/`RAMP2`) — steps T1 or T2 by a fixed amount every N
  *bursts*, over seconds. A slow resonance sweep. **Flashed and working**
  from earlier in this session.
- **Elongation** (`ELONGATE`) — each pulse *within one burst* compounds off
  the last. **Flashed and verified on the scope** (2026-07-31).

They were originally both called "elongation" in the firmware/UI — that's
been cleaned up. `RAMP1_active`/`ramp2_active` in the JSON status are the
sweep; `elongation`/`elong_ratio`/`elong_factor` are the per-pulse thing.

---

## 3. Elongation — how it works and why it needed new PIO code

**What it does**: in COUNT mode with the internal carrier, pulse 1 of a burst
is whatever T1/T2 the knobs are set to (the "base"). Pulse 2 = pulse 1 ×
factor. Pulse 3 = pulse 2 × factor. And so on, T1 and T2 independently, up to
**25 pulses per burst** (your call — anything more was deemed pointless).
Factor 0.1–0.99 shrinks pulse-over-pulse, >1 grows.

**Why not just reuse the sweep code**: the sweep works because the CPU can
poll and nudge the carrier between bursts — bursts happen slowly enough
(every N bursts, seconds) for `loop()` to keep up. Elongation needs a
*different* width on literally every pulse, which can be hundreds of kHz —
far too fast for the CPU loop. Also, the normal design deliberately keeps the
carrier (free-running) and the gate (a separate counting SM) decoupled — the
gate has no idea what the carrier is doing, it just passes through whatever
arrives. That's fine for a flat carrier, but "pulse 1 of *this burst*" is
meaningless without something resetting exactly at burst boundaries, and two
independent SMs can't do that without real synchronization.

**The fix**: a brand-new PIO program (`elongate.pio`) that owns pulse
generation *and* the burst/mute cycle in one self-contained state machine.
Every burst it's fed a small table via DMA — pulse count, then each pulse's
(high, low) cycle counts, then the mute duration — and because it's all one
program, "pulse 1" is naturally wherever its own program counter loops back
to. No cross-SM sync needed. The table is rebuilt (in C, on the CPU) whenever
a relevant parameter changes, and DMA re-arms itself automatically each burst
via its own completion interrupt — zero per-pulse CPU involvement, same
"PIO owns the waveform" guarantee as the rest of this design.

**Two real bugs were caught before flashing**, by writing a cycle-accurate
Python simulation of the PIO program against the actual table contents:

1. The DMA table had the mute-duration value in the wrong position (2nd
   instead of last) — would have fed pulse 1's high-time the mute count
   instead, corrupting every single burst.
2. The mute-duration cycle-count formula was off by 2 cycles (a fixed
   instruction-overhead miscount).

Both are fixed now and the simulation matches exactly across multiple
repeated bursts. Worth mentioning because this is exactly the kind of bug
that's very hard to spot on a scope trace but obvious once you count
instructions — if the *shape* of the elongation looks subtly wrong tomorrow
(pulse 1 doesn't match the base T1/T2 you set, or the gap between bursts
looks off), that's the first place to re-check, even though the simulation
should already have caught it.

### Commands

```
ELONGATE <ratio>      arm it — e.g. ELONGATE 8 (grow 8x) or ELONGATE 0.2 (shrink)
ELONGATE PHI          golden-ratio growth per pulse at the current pulse count
ELONGATE OFF          disarm — reverts to a flat train of the base pulses
```

The argument is the **total span across the burst** — last pulse ÷ first
pulse — not the per-pulse multiplier. The firmware derives the per-pulse
figure from it and the pulse count (`ratio^(1/(N-1))`), and reports both.

That indirection is the whole point, because the compounding is exponential
in `ON`. A fixed per-pulse step that looks good at 10 pulses is unusable at
25: φ per pulse is a pleasant 76× span over 10 pulses and a nonsense 103,682×
over 25, which saturates the 32-bit cycle counters and flattens the tail into
a row of identically clamped pulses. Pinning the *total* instead makes the
burst's shape independent of how many pulses are in it — dial "8× across the
burst" and it reads the same at N=5 and N=25, which is what you want when
sweeping pulse count against a load.

Verified on the bench at 100 kHz base (T1=T2=5 µs), `ON 10`, `ELONGATE 8`:
per-pulse 1.2599, burst on-phase measured **350 µs** against 349 µs predicted.
Changing to `ON 25` held the ratio at 8× and re-derived 1.0905/pulse.

**On Fibonacci:** it isn't implemented separately because it isn't a
different curve. F(n+1)/F(n) converges to φ — within 1% by the 5th term,
0.01% by the 10th — so a Fibonacci burst and `ELONGATE PHI` are the same
waveform past the first few pulses, except Fibonacci has no knob to turn.

Also settable from the dashboard (Pulse control card — ratio box + arm
button) or the ENC3 push button, which re-arms with whatever ratio was last
used (default **8** the very first time, before anything's ever been set).

### Constraints

- **COUNT mode + internal carrier only.** Switching to TIME mode, or `SRC
  EXT`, auto-disarms it (there's no well-defined "pulse 1 of this burst" for
  an external, unpredictable signal).
- **25 pulses max per burst.** If `ON` is set higher than that while
  elongation is armed, only the first 25 compound; ideally don't set `ON`
  above 25 while it's on. The ratio is spread across those 25, not across
  the full `ON` count — `elong_pulses` in the JSON reports which number the
  derived per-pulse factor was actually computed from.
- **GP2 (carrier tap) goes idle while elongation is armed** — it's not
  driven at all, since elongation bypasses the normal free-running carrier
  entirely. If you have a probe on GP2 expecting to see something, that's
  expected, not a fault.
- `R` (defaults) and `SHOT` both compose correctly with it (SHOT parks in
  mute forever after one burst, same trick the normal path already uses).

### Chart

The dashboard's "digital reproduction" chart now draws the actual compounding
shape (mirrors the firmware math) instead of a flat repeat, whenever
elongation is active — so once it's flashed, arming it should visibly show
the ramp on the reproduction chart even before touching the real scope.

---

## 4. Tomorrow's bring-up checklist

1. `make pico-flash` (port will likely re-enumerate — check
   `ls /dev/cu.usbmodem*` first, don't assume the old path).
2. `J` or `P` over serial — confirm it boots and reports `"shape"`,
   `"elongation"`, `"elong_factor"` fields (proves the new firmware is
   actually running, not a stale image).
3. Re-confirm the two new encoders still turn/click correctly post-flash
   (`ENCLOG 1` if anything seems off) — the wiring fix from tonight should
   hold, but worth a 30-second re-check after a fresh flash/reset.
4. **Elongation first test — do this on the bench before trusting the scope
   reproduction chart**:
   - `M 1` (COUNT mode), `SRC INT`, set a slow-ish base carrier (e.g. `C 1000`
     for 1 kHz, `CD 20` for 20% duty) and a small `ON` count (e.g. `ON 5`) so
     the burst is easy to read on screen.
   - `ELONGATE 1.2` — arm with a very obvious 20%-per-pulse growth factor.
   - `SHOT` — fire one burst, scope on GP5 single-shot triggered on GP4.
   - **Check**: 5 visibly growing pulses, pulse 1 matching the base width you
     set. Compare against the carrier Hz/duty reported under `source` in `P`'s
     output — that's your base T1/T2.
   - Then `ELONGATE OFF`, `SHOT` again — burst should go back to 5 identical
     flat pulses.
   - Then try `ELONGATE 0.8` to confirm shrinking works too.
5. If any of the above looks wrong, the two bugs already fixed in section 3
   are the most likely place — re-derive against `elongate.pio`'s comments,
   which spell out the exact pull order and per-phase cycle-overhead
   constants.

---

## 5. Also fixed tonight (dashboard/scope side, already deployed — no flash needed)

- **Scope channel map was backwards**: C2/C4 roles were swapped (burst_marker
  vs carrier) relative to what's actually wired. Fixed in
  `tools/runtime_channels.json`. This is also why the reproduction chart's
  burst_marker trace was invisible before — it was using the wrong channel's
  (wildly different) volts/div and offset.
- **Chart wasn't live-updating** when a parameter changed via the physical
  encoders (only the number fields were; the chart only redrew on
  user-initiated dashboard actions). Fixed by fingerprinting the
  shape-relevant fields and scheduling a redraw when they change on the
  periodic poll, regardless of what changed them.
- Investigated a scope freeze that needed a power-cycle to clear. Found and
  fixed one real locking gap (`scope_geometry()` wasn't taking `SCOPE_LOCK`),
  though on closer inspection its only caller already locked around it, so
  that wasn't conclusively *the* cause — no confirmed root cause yet. If it
  recurs, worth checking whether anything else (another script, a browser tab
  open straight to the scope's own IP) might be talking to it at the same
  time, since the in-process locking already checked out clean otherwise.

---

## 6. Command reference (everything new, for quick lookup)

```
SHAPE FD|T12         carrier knobs (ENC2/ENC3) edit freq+duty, or T1/T2 independently
ENCLOG <0|1>          live-print every encoder edge/step to serial (debug only)
RAMP1 <step_us> <bursts> <limit_us> [STOP|WRAP]     sweep T1 across bursts
RAMP2 ...             same, for T2
RAMP1|2 OFF           cancel that sweep, holding its current value
ELONGATE <ratio>      arm compounding, total span last/first (COUNT+INT only)
ELONGATE PHI          golden-ratio growth per pulse at the current count
ELONGATE OFF          disarm — flat train of the base pulses
```

---

## 7. Cycle marker on GP22

**Why.** GP4 goes high for every burst, so in offset mode (three bursts per
pattern) it cannot tell you *which* burst is on screen and the trace walks. The
workaround was triggering off one channel with holdoff, which has to be retuned
whenever the pattern length changes. GP22 marks burst 1 of the pattern and
nothing else, giving a stable trigger with holdoff off.

**How it is wired in.** The marker rides the same DMA word that selects the
channel, on the same state machine, so it cannot drift from the burst it marks
by even a clock. Reaching GP22 from the OUT group's base of GP18 means a
five-wide group, and bit 3 of that group lands on GP21 — the mode button. That
bit is simply never driven: GP21's pad function stays SIO, so the PIO block is
not connected to it and writes there go nowhere. Costs one wasted bit in a
32-bit word.

### The finding worth keeping

The first attempt made the two OUT instructions different widths — a 5-bit write
before each pulse to raise channel and marker, a 3-bit write after it to drop
the channel while leaving the marker latched. **That does not work, and the
reason contradicts the obvious reading of the instruction set:**

> `out pins, n` does NOT leave the pins above `n` alone. The pin-write mask
> comes from the group's configured OUT_COUNT, not from the instruction's bit
> count, so a narrow OUT zero-fills the rest of the group.

`out pins, 3` on a 5-wide group clears GP21 and GP22 too.

Measured, not inferred. At C 10000 / ON 10 / OFF 90 / PHASE ROT 3 the pattern is
three 10 ms bursts, so a latched marker reads ~33 % duty and a per-pulse one
~1.7 %:

```
chan   GP18 : 1.0% of 50ms high
chan   GP19 : 2.0% of 50ms high
chan   GP20 : 2.0% of 50ms high
cycle  GP22 : 1.0% of 50ms high     <- latch failed; tracking the channel
```

That is `DIAG`'s per-pin duty readout, added for exactly this question. A single
`gpio_get()` cannot distinguish "low right now" from "dead", and an earlier
round of single-sample polling produced two contradictory results before the
duty readout settled it. The window is a whole number of patterns only by luck,
so short windows quantise badly — 50 ms read a true 33 % as either 20 % or 40 %,
which is why it is now 200 ms.

### The fix

Hold the between-pulses value in a register instead of relying on write width:

- `burst_start` pulls one extra word into ISR — the gap mask, which is the cycle
  bit alone, or 0 on every other burst
- the low phase does `mov osr, isr` + a full-width `out pins, 5`, replacing
  `mov osr, null` + `out pins, 3`

Same instruction count in the loop, so **hi and lo are unchanged** and an exactly
1.000 us T1 still lands on 150 integer clocks. Two knock-on costs, both handled:

1. the program grew 20 -> 22 instructions, and the two new burst_start
   instructions sit between the mute loop and the next burst's first rising
   edge — inside the window the mute is measured over. Mute therefore moved from
   `pushed = desired - 9` to `desired - 11`. Re-derived instruction-by-
   instruction against the listing, not guessed.
2. the table gained a word per burst: `N-1, gapMask, (mask, hi, lo) x N, mute`.
   `ELONG_WORDS = 3 + 3*ELONG_MAX_PULSES` already covered 3N+3 exactly. The
   `PHASE DUMP` offsets shifted by one and were updated with it.

Confirmed on the board — same settings as the failing measurement above:

```
cycle  GP22 : 40.0% of 50ms high    <- latched across the whole burst
```

(40 % rather than 33 % is the 50 ms window quantisation described above, not an
error; `PHASE DUMP` independently shows burst 1 flagged `*` and bursts 2-3
clean.) Scope confirmation with a probe on pin 29 is still outstanding.

**Also fixed along the way.** Falling through to the normal carrier+gate path
now parks the OUT group (`ampParkPins()`). That path never drives GP18-20, so
whatever the elongation engine last latched used to stay asserted after a mode
change — one decoder output, and therefore one high-side leg's opto, held on
with nothing running that would ever rewrite it.

**A red herring, recorded so it is not chased twice.** Midway through, the rig
appeared to fire "one burst on one channel only when a knob is turned". Nothing
to do with any of the above: the carrier had been wound down to 14.6 Hz with OFF
at 648, making one burst ~0.34 s followed by ~44 s of silence. Every parameter
change rebuilds the ring and restarts it at index 0, which is always channel 1 —
so turning a knob fires exactly one channel-1 burst and then goes quiet. Check
`P` for the carrier frequency before suspecting the ring.

### Scaling past 3 channels

Not a timing question: the channel code is data in the same DMA word as the
timing, so the state machine runs the identical 22 instructions whether there
are 3 codes or 31. What binds, in order:

1. **Consecutive GPIOs.** A PIO OUT group must be consecutive. On a Pico 2 W
   GP23/24/25 belong to the CYW43 WiFi chip, so the longest free run is
   **GP18-GP22, five pins**. More than five direct channel pins is impossible on
   this board.
2. **Therefore a decoder.** Four code bits into a 74HC154 (4-to-16) gives 15
   one-hot outputs. That means moving the mode button off GP21 to GP26, which
   also deletes the wasted bit 3. Offset mode would emit the binary code `k+1`
   rather than today's one-hot `1 << k`; leave Y0 unconnected so code 0 stays
   parked.
3. **Ring memory.** `ringLen = lcm(trainLen, channels)` against `RING_MAX = 24`.
   Nine channels with a 4-level train needs 36, with 8 levels needs 72 — 22 KB
   at `RING_MAX 72`, against 441 KB free. Cheap, but it silently clamps if the
   constant is not raised.

Note the two topologies want one-hot for *different* reasons. Taps on a single
winding: two conducting at once shorts the turns between them, so the decoder is
a hardware safety interlock. Separate VIC/WFC cells fired sequentially to
restrict current: no such hazard, but two firing at once doubles the draw on the
shared rail and defeats the point of sequencing.
