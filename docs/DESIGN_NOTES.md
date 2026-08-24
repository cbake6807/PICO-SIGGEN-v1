# Design notes

Why the firmware is built the way it is, and the hardware findings that cost a
bench session each. Read this before changing the PIO programs.

For how to *use* the thing, see [../README.md](../README.md) and
[../HARDWARE.md](../HARDWARE.md).

---

## Two carrier-shaping features that are easy to confuse

| | scope | what it changes |
|---|---|---|
| **Elongation** | *within* one burst | each pulse compounds off the last, so pulse N is `ratio` times pulse 1 |
| **Sweep** (`RAMP1`/`RAMP2`) | *across* bursts | steps the base T1 or T2 every N bursts, to hunt a resonance |

They compose. Elongation reshapes the pulses inside a burst; sweep moves the
base those pulses are built from. A third axis, `TRAIN`, scales whole bursts.

## Why elongation needed its own PIO program

The normal path is two state machines: one free-running carrier, and a separate
one that counts its pulses and gates them. That works because the gating SM only
ever has to decide "pass this pulse or mute it" — it never needs to know *which*
pulse it is looking at.

Elongation does need to know. Pulse 3 of a burst has a different width from
pulse 2, so something has to keep a per-pulse table and stay aligned to the
burst boundary. Splitting that across two SMs would mean keeping a counter in
one machine synchronised with a table index in another, through every start,
stop and parameter change.

`elongate.pio` instead gives **one** state machine ownership of the whole burst:
the on-phase pulses come one at a time from a DMA-streamed table, and the
off-phase is a plain countdown. "Pulse 1 of this burst" is then simply wherever
its own program counter loops back to, so no cross-SM sync exists to drift.

The table is re-armed each burst by the DMA's own completion IRQ, which does
nothing but move a pointer — safe to run in an interrupt, and the reason a ring
of bursts can span many bursts without the CPU touching the waveform.

## Timing: every phase count is compensated, and the constants are load-bearing

PIO instructions each take one cycle, so the loop overhead around a countdown is
part of the width. The firmware pushes `desired - overhead`, with the overhead
derived instruction-by-instruction against the assembled listing:

```
hi_cycles   = pushed + 4     set pins,3 .. set pins,1
lo_cycles   = pushed + 9     set pins,1 .. the next set pins,3, including the
                             closing jmp x-- and the next pulse's setup
mute_cycles = pushed + 11    set pins,0 .. the next burst's set pins,3, past
                             the jmp and the whole burst_start preamble
```

Each is measured "until the pins next actually move", so loop-exit and the
following setup instructions belong to the phase *before* them.

**Insert an instruction into one of those windows and the corresponding constant
moves.** When the cycle-marker gap mask was added, two instructions landed in
the burst_start preamble — inside the window the mute is measured over — and the
mute constant went from 9 to 11. `hi` and `lo` were untouched because nothing
was inserted between their boundary instructions.

Re-derive against the listing. Do not measure and fudge: an exactly 1.000 µs T1
is 150 integer clocks at 150 MHz, and that exactness is the reason for the whole
PIO approach.

## `out pins, n` does not leave the upper pins alone

The finding that cost the most, because it contradicts the natural reading of
the instruction set:

> **The pin-write mask comes from the OUT group's configured `OUT_COUNT`, not
> from the instruction's bit count.** A narrow OUT zero-fills the rest of the
> group.

`out pins, 3` on a 5-wide group clears GP21 and GP22 too.

The cycle marker was first implemented by making the two OUT instructions
different widths — a 5-bit write before each pulse to raise channel and marker,
a 3-bit write after it to drop the channel while leaving the marker latched.
That silently does nothing, and the symptom is subtle: the marker appears, but
follows the pulses instead of spanning the burst.

Measured, not inferred. At 100 kHz / ON 10 / OFF 90 / 3 channels the pattern is
three bursts, so a latched marker reads ~33 % duty and a per-pulse one ~1.7 %:

```
chan   GP18 : 1.0% of 50ms high
chan   GP19 : 2.0% of 50ms high
chan   GP20 : 2.0% of 50ms high
cycle  GP22 : 1.0% of 50ms high     <- latch failed; tracking the channel
```

**The fix**: carry the between-pulses value in a register instead of relying on
write width. Each burst parks a "gap mask" in ISR — the cycle bit alone, or 0 —
and the low phase does `mov osr, isr` + a full-width OUT, replacing
`mov osr, null`. Same instruction count in the loop, so `hi` and `lo` do not
move; only the mute constant does, per the section above.

## Measuring a pin: duty, not a single sample

`DIAG` reports each output's percentage of a 200 ms window rather than its level.
A single `gpio_get()` cannot tell "low at this instant" from "dead", and that
ambiguity is expensive at the bench — an early round of single-sample polling
produced two contradictory results before the duty readout settled it.

The window is a whole number of patterns only by luck, so the figure is
quantised by how many bursts fall inside it. At 100 kHz / ON 10 / OFF 90 a 50 ms
window read a true 33 % as either 20 % or 40 %; 200 ms keeps that error small
enough not to mislead.

## The 100-pulse cap

`ELONG_MAX_PULSES` is 100. In any mode that runs the elongation engine — that is,
elongation, `SEQ`, `TRAIN` or channel output — a burst longer than that is
clamped. The normal carrier+gate path has no such limit.

The state readout reports the **effective** burst length and flags the clamp
(`burst_clamped`, `burst_requested`), because an instrument that quietly emits
100 pulses while reporting 150 is worse than one with a documented limit.

It was 25 for a long time, and the binding constraint was never the PIO or the
DMA — it was that the same constant sizes the persisted `seqSteps[]` array,
which had to fit a 512-byte settings budget. That budget was raised to 2 KB,
which is still inside the one 4 KB flash sector the core erases and rewrites
whole, so it costs no extra wear and no extra write time. Both limits moved
together, which is why a 100-pulse burst and a 100-step `SEQ` are the same
number rather than two you have to remember separately.

`static_assert(sizeof(Settings) <= CFG_EE_SIZE)` guards it now. Nothing at
runtime notices the struct outgrowing the buffer — `cfgWrite()` memcpy's
`sizeof(s)` into it — so the failure mode was a silent overwrite past the end,
surfacing later as corrupted settings rather than as a build error.

## Scaling past three channels

Not a timing question — the channel code travels as data in the same DMA word as
the timing, so the state machine runs the identical program whether there are 3
codes or 31. What actually binds, in order:

1. **Consecutive GPIOs.** A PIO OUT group must be consecutive. On a Pico 2 W,
   GP23/24/25 belong to the CYW43 WiFi chip, so the longest free run is
   **GP18–GP22, five pins**. More than five direct channel pins is impossible on
   this board.
2. **Therefore a decoder.** Four code bits into a 74HC154 (4-to-16) gives 15
   one-hot outputs, at the cost of moving the mode button off GP21 — which also
   removes the wasted bit 3 that exists today only because that button sits in
   the middle of the group. Offset mode would then emit the binary code `k+1`
   rather than today's one-hot `1 << k`; leave Y0 unconnected so code 0 stays
   parked.
3. **Ring memory.** `ring_len = lcm(train_len, channels)` against `RING_MAX`.
   Nine channels with a four-level train needs 36 entries, with eight levels 72.
   Cheap in RAM, but it silently clamps if the constant is not raised.

One-hot matters for different reasons in different topologies. Taps on a single
winding: two conducting at once shorts the turns between them, so a decoder is a
hardware safety interlock. Separate cells fired in sequence: no such hazard, but
two firing at once doubles the draw on the shared rail and defeats the point of
sequencing.

## Pull-ups everywhere

Every input uses the internal pull-up and switches to ground. That is not a
style preference: RP2350 erratum E9 affects pads using the internal pull-down,
so the firmware avoids them entirely.
