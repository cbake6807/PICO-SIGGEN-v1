// GatedPulsePico — networked gated pulse-train generator, Raspberry Pi Pico 2 W
//
// Port of ../GatedPulse (Arduino Nano). Same knob/serial UX, but the RP2350
// changes the design: PIO does the gating in hardware, so the AVR's
// software-AND compromise is gone and no 74HC08 is needed.
//
//   ext sig gen ──► GP3 ─┐
//                        ├─► PIO gating SM ──► GP5   gated pulse train
//   int carrier ──► GP2 ─┘        ▲            GP4   gate / burst marker
//   (PIO gate_gen)                │
//                                 └─ COUNT: SM counts input pulses
//                                    TIME : second gate_gen SM drives GP4
//
// Edge fidelity, measured in clocks rather than guessed:
//   Nano software AND     562 ns worst-case error, 1.7 % blind time
//   Nano + 74HC08         ~10 ns, extra chip
//   Pico 2 W PIO          ~20 ns constant delay, +/-6.7 ns jitter, no extra parts
//
// PIO runs independently of the CPU, so WiFi, HTTP, USB and the encoder cannot
// perturb the output. On the Nano every one of those punched holes in it.
//
// Three ways to drive it, all sharing one command interpreter:
//   - rotary encoder + buttons
//   - USB serial (type ? for help)
//   - HTTP on the LAN:  GET /state          -> JSON
//                       GET /cmd?c=<command> -> runs any command, returns text
//                       GET /                -> small web UI
// tools/gatedpulse.py and tools/mcp_server.py on the host wrap the HTTP API.
//
// !! 3.3 V ONLY !! RP2350 GPIO is NOT 5 V tolerant, unlike the Nano this
// replaces. A 5 V sig-gen output WILL damage GP3. Use a divider (10k series +
// 20k to ground) or a 74LVC1G17 buffer on 3V3. See README.

#include <Arduino.h>
#include <stdio.h>            // sscanf, for parsing RAMP1/RAMP2's multi-value argument
#include <hardware/pio.h>
#include <hardware/pwm.h>
#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/dma.h>
#include <hardware/irq.h>
#include <string.h>           // memcpy/memset, for the settings blob
#include <EEPROM.h>           // emulated in one flash sector -- see cfgWrite()

// Forward-declared for arduino-cli's auto-generated prototypes. It inserts the
// whole block immediately above the file's FIRST function definition, so any
// struct a function signature mentions has to be declared above that point --
// and where that point lands moves whenever a helper is added near the top.
// Declaring every struct that appears in a signature here makes the build
// independent of that, rather than quietly breaking on the next edit. A
// reference to an incomplete type is perfectly legal in a declaration.
struct Settings;
struct RampCfg;
struct ElongCfg;
struct SeqStep;
#include <WiFi.h>
#include <WebServer.h>
#include <LEAmDNS.h>

// WiFi credentials live in secrets.h, which is gitignored. Without it the
// firmware still builds and runs — it just stays offline.
#if __has_include("secrets.h")
  #include "secrets.h"
#endif
#ifndef WIFI_SSID
  #define WIFI_SSID ""
  #define WIFI_PASS ""
#endif
#ifndef MDNS_NAME
  #define MDNS_NAME "gatedpulse"
#endif

// ---------- Pins ----------
// GP3 is odd on purpose: PWM channel B is the one with an edge-count input
// mode, and channel B lives on odd GPIOs. GP4/GP5 are adjacent on purpose so
// one SET pin group can drive both in COUNT mode.
static const uint PIN_CARRIER    = 2;   // internally generated carrier (SRC INT)
static const uint PIN_SIG_IN     = 3;   // external sig-gen square wave (3.3 V!)
static const uint PIN_GATE_OUT   = 4;   // gate / burst marker — scope trigger
static const uint PIN_GATED_OUT  = 5;   // the gated pulse train
static const uint PIN_ENC_A      = 6;
static const uint PIN_ENC_B      = 7;
static const uint PIN_ENC_SW     = 8;   // press: select which param the knob edits
static const uint PIN_DISABLE    = 9;   // press: output on/off
static const uint PIN_MODE_SW    = 10;  // toggle: LOW = TIME, HIGH = COUNT
static const uint PIN_STATUS_LED = 15;  // external LED + resistor

// HV enclosure interlock. Board pin 31, with AGND right beside it at pin 33,
// so the whole connection is a two-pin header.
//
// Polarity is chosen so every way this can break lands on "unsafe":
//   closed (safe)  = pin pulled to GND by the sensor or switch
//   open           = pin floats to the internal pull-up
// A cut wire, an unpowered sensor, or nothing plugged in at all reads HIGH,
// which is the open case. The alternative polarity would report "lid closed"
// for a disconnected cable, which is exactly the failure a safety input exists
// to catch. Same pull-up-and-switch-to-ground convention as every other input
// here (RP2350 erratum E9 rules out the pull-downs).
static const uint PIN_INTERLOCK  = 26;  // board pin 31, GND at pin 33

// Second and third encoders: independent carrier controls. GP15 is skipped
// (status LED) and GP23/24/25/29 are avoided even though this board doesn't
// break them out at all -- on the plain Pico W they are wired straight to the
// CYW43 radio, so staying off that range is a cheap habit to keep.
static const uint PIN_ENC2_A     = 11;  // carrier frequency, or T1 width in T12 mode
static const uint PIN_ENC2_B     = 12;
static const uint PIN_ENC2_SW    = 13;  // press: toggle freq+duty vs T1/T2-independent editing
static const uint PIN_ENC3_A     = 14;  // carrier duty, or T2 space in T12 mode
static const uint PIN_ENC3_B     = 16;
static const uint PIN_ENC3_SW    = 17;  // press: toggle T1/T2 elongation ramp on/off

// Amplitude / tap select: 3 consecutive pins driven by the elongation SM's OUT
// group, intended for a 74HC138-style one-hot decoder feeding one opto +
// high-side leg per level. A decoder rather than three independent enables on
// purpose: two taps of one winding conducting at once shorts the turns between
// them, and one-hot in hardware makes that combination unreachable no matter
// what the firmware asks for. Harmless when nothing is wired -- the pins just
// toggle into open air.
// Dedicated panel button for the output mode (single / offset / sync). It gets
// its own pin because the modes are INDEPENDENT axes -- you may well want
// elongation with offset, or sweep with sync -- and folding them into one
// cycling control made combinations unreachable.
static const uint     PIN_PHASE_SW = 21;   // board pin 27, GND at pin 28

// ---- front-panel input map + event log ------------------------------------
// Wiring an encoder panel is the one job where "it does nothing" has half a
// dozen indistinguishable causes: A and B swapped, the common not grounded, a
// joint that never took, the wrong board pin counted from the wrong end. The
// raw edge log separates them in seconds -- if a pin never toggles it is the
// wiring, and if it toggles but no step is ever decoded it is the pairing.
//
// Board pin numbers live here too, because that is the number you count on the
// header while probing, and having to cross-reference GP-to-board in the
// middle of debugging is how the wrong pin gets blamed.
struct InputPin { uint gp; uint8_t board; const char *name; };
static const InputPin INPUTS[] = {
  { PIN_ENC_A,     9, "enc1 A"  }, { PIN_ENC_B,   10, "enc1 B"  },
  { PIN_ENC_SW,   11, "enc1 sw" }, { PIN_DISABLE, 12, "out en"  },
  { PIN_MODE_SW,  14, "T/C sw"  }, { PIN_ENC2_A,  15, "enc2 A"  },
  { PIN_ENC2_B,   16, "enc2 B"  }, { PIN_ENC2_SW, 17, "enc2 sw" },
  { PIN_ENC3_A,   19, "enc3 A"  }, { PIN_ENC3_B,  21, "enc3 B"  },
  { PIN_ENC3_SW,  22, "enc3 sw" }, { PIN_PHASE_SW, 27, "mode sw" },
};

// Kinds: 0 = raw pin edge, 1 = decoded detent step.
// kind 2 carries `msg` -- what the firmware DECIDED the input meant. Raw
// edges tell you the wiring is sound; this tells you the knob you turned moved
// the parameter you expected, which is a different failure and a commoner one.
struct InEvent { uint32_t ms; uint8_t gp; uint8_t level; uint8_t kind;
                 int8_t step; char msg[28]; };
static const uint16_t INLOG_MAX = 128;
static InEvent  inLog[INLOG_MAX];
static uint32_t inLogSeq = 0;          // total ever logged; the UI polls "since"

static void inLogPush(uint8_t gp, uint8_t level, uint8_t kind, int8_t step) {
  InEvent &e = inLog[inLogSeq % INLOG_MAX];
  e.ms = millis(); e.gp = gp; e.level = level; e.kind = kind; e.step = step;
  e.msg[0] = 0;
  inLogSeq++;
}

// What the panel just did, in words. Called from the apply/toggle paths where
// the firmware already knows the outcome, so the log cannot drift from what
// actually happened.
static void inLogAction(const char *fmt, ...) {
  InEvent &e = inLog[inLogSeq % INLOG_MAX];
  e.ms = millis(); e.gp = 0; e.level = 0; e.kind = 2; e.step = 0;
  va_list ap; va_start(ap, fmt);
  vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
  va_end(ap);
  inLogSeq++;
}

// Every input, every loop. Deliberately NOT debounced and NOT filtered: bounce
// is one of the things you are looking for, and a "clean" log would hide the
// noisy contact that is actually causing the miscounts.
static void pollInputWatch() {
  static uint32_t last = 0;
  static bool     primed = false;
  uint32_t m = 0;
  for (uint i = 0; i < count_of(INPUTS); i++)
    if (gpio_get(INPUTS[i].gp)) m |= (1u << i);
  if (!primed) { last = m; primed = true; return; }
  uint32_t diff = m ^ last;
  if (!diff) return;
  for (uint i = 0; i < count_of(INPUTS); i++)
    if (diff & (1u << i)) inLogPush((uint8_t)INPUTS[i].gp, (m >> i) & 1u, 0, 0);
  last = m;
}

// Runtime-settable (PHASEPIN), not a constant: every panel is wired
// differently, and the base only has to satisfy "three consecutive free GPIOs".
// Baking it in meant anyone whose encoders landed here had to edit and rebuild.
static uint           ampBase    = 18;  // default GP18/19/20 = A0/A1/A2
static const uint     AMP_BITS   = 3;
static const uint32_t AMP_MASK   = (1u << AMP_BITS) - 1;   // codes 0..7

// Cycle marker: high for the whole of burst 1 of the repeating pattern, low for
// every other burst. It exists because GP4 pulses on EVERY burst, so in offset
// mode a scope has no way to tell burst 1 from burst 3 and the trace walks --
// the workaround was triggering off one channel with holdoff, which is fragile
// and drifts the moment the ring length changes.
//
// It rides the same OUT group as the channel bits rather than a second state
// machine, so it is generated from the same DMA word that picks the channel and
// cannot drift from it by even a clock. PIO pin groups are consecutive, so
// reaching GP22 from base GP18 means a 5-wide group, and bit 3 lands on GP21 --
// the mode button. That bit is simply never driven: GP21's function stays SIO,
// so the PIO block is not connected to that pad and its writes there go
// nowhere. Costs one wasted bit in the pushed word and nothing at runtime.
static const uint     CYCLE_BIT  = 4;                       // bit 3 = GP21 hole
static const uint     OUT_BITS   = CYCLE_BIT + 1;           // GP18..GP22
static const uint32_t CYCLE_MASK = 1u << CYCLE_BIT;
static inline uint    cyclePin()  { return ampBase + CYCLE_BIT; }   // GP22

// Named PIN_STATUS_LED, not PIN_LED: the rpipico2w variant already defines
// PIN_LED as 64, because the onboard LED hangs off the CYW43 WiFi chip rather
// than a GPIO. That one is used for link status once WiFi is up.

// ============================================================================
// PIO programs
// ============================================================================
//
// Hand-maintained encodings — arduino-cli does not run pioasm during a sketch
// build. The .pio sources next to this file are the readable original, and
// `make pioverify` re-assembles them and diffs against these arrays, so the
// two cannot silently drift.

// ---- Combined gating program (gating.pio) --------------------------------
//
// Both engines live in ONE program so dynamic PIO claiming can never split
// them across blocks. COUNT entry = offset+0, TIME entry = offset+17.
//
// The SET pin group differs per mode and is configured on the state machine:
//   COUNT: base GP4, count 2  -> bit0 = burst marker, bit1 = gated output
//   TIME : base GP5, count 1  -> bit0 = gated output
//
// COUNT half: passes ON input pulses then mutes OFF input pulses, forever, with
// no CPU involvement. It only ever raises the output after a rising edge and
// lowers it after the matching falling edge, so every burst is exactly ON
// *whole* pulses — no runts, at any input frequency.
//
// TIME half: ANDs the input with a gate level read through JMP_PIN. The `wait 0`
// before each pulse is what keeps it runt-free — a gate opening mid-pulse skips
// the remainder and starts clean on the next rising edge. Gate closure is
// likewise deferred to the end of the pulse in flight, at most one input period.
static const uint16_t gating_insns[] = {
  0x80a0,  //  0: pull block          OSR = OFF-1
  0xa0c7,  //  1: mov  isr, osr       stash OFF
  0x80a0,  //  2: pull block          OSR = ON-1 (stays here)
  0x2020,  //  3: wait 0 pin 0        sync: begin from a low input
  0xa027,  //  4: mov  x, osr       burst: reload ON count
  0xe001,  //  5: set  pins, 1        marker high, output low
  0x20a0,  //  6: wait 1 pin 0      pass:
  0xe003,  //  7: set  pins, 3        output follows input
  0x2020,  //  8: wait 0 pin 0
  0xe001,  //  9: set  pins, 1
  0x0046,  // 10: jmp  x--, 6
  0xe000,  // 11: set  pins, 0        burst over, marker low
  0xa046,  // 12: mov  y, isr         reload OFF count
  0x20a0,  // 13: wait 1 pin 0      mute:
  0x2020,  // 14: wait 0 pin 0
  0x008d,  // 15: jmp  y--, 13
  0x0004,  // 16: jmp  4
  0x00d4,  // 17: jmp  pin, 20     tloop:   <-- TIME entry
  0xe000,  // 18: set  pins, 0        gate shut, hold low
  0x0011,  // 19: jmp  17
  0x2020,  // 20: wait 0 pin 0     topen:
  0x20a0,  // 21: wait 1 pin 0
  0xe001,  // 22: set  pins, 1
  0x2020,  // 23: wait 0 pin 0
  0xe000,  // 24: set  pins, 0
  0x0011,  // 25: jmp  17
};
static const pio_program_t gating_program = {
  .instructions = gating_insns,
  .length       = count_of(gating_insns),
  .origin       = -1,
};
static const uint GATING_ENTRY_COUNT = 0;
static const uint GATING_ENTRY_TIME  = 17;

// ---- Square-wave generator (gate_gen.pio) --------------------------------
//
// Independent 32-bit high and low counts. Replaces the AVR's Timer1: that
// topped out at a 4.19 s period, this reaches hours while still resolving
// 6.67 ns at clkdiv 1. Two instances run: the TIME-mode gate, and the internal
// carrier. Fixed loop overhead is compensated on the CPU side —
// high = N + 3 cycles, low = M + 4 cycles (see genCycles()).
static const uint16_t gate_gen_insns[] = {
  0x80a0,  //  0: pull block          OSR = low-1
  0xa0c7,  //  1: mov  isr, osr
  0x80a0,  //  2: pull block          OSR = high-1
  0xa027,  //  3: mov  x, osr       cycle:
  0xe001,  //  4: set  pins, 1
  0x0045,  //  5: jmp  x--, 5      hi:
  0xa046,  //  6: mov  y, isr
  0xe000,  //  7: set  pins, 0
  0x0088,  //  8: jmp  y--, 8      lo:
  0x0003,  //  9: jmp  3
};
static const pio_program_t gate_gen_program = {
  .instructions = gate_gen_insns,
  .length       = count_of(gate_gen_insns),
  .origin       = -1,
};

// ---- Elongation burst generator (elongate.pio) ----------------------------
//
// Unlike gate_gen (a free-running carrier, gated by a SEPARATE counting SM
// that has no idea what the carrier is doing), this ONE state machine owns a
// whole COUNT-mode burst itself: on-phase pulses fed one at a time from a
// small DMA-streamed table, off-phase a plain countdown. "Pulse 1 of this
// burst" is naturally wherever its own program counter loops back to, so no
// cross-SM sync is needed to keep the shape locked to burst boundaries.
//
// SET pin group: base GP4, count 2 -> bit0 = marker (GP4), bit1 = output (GP5),
// same layout as the normal COUNT-mode gating program.
//
// OUT pin group: base GP18, count 5 -> bits 0..2 = channel / tap select
// (GP18-20), bit 3 unused (GP21, left on SIO for the mode button), bit 4 = the
// cycle marker (GP22).
//
// The channel has to drop between pulses (each channel carries the real pulse
// train, not a burst-wide envelope) while the cycle marker has to stay up
// across the whole burst. Both live in the same OUT group, so one write cannot
// do both -- and a NARROWER write does not help, which is the trap here:
//
//   `out pins, 3` on a 5-wide group does NOT leave GP21/GP22 untouched.
//
// The pin-write mask comes from the group's configured OUT_COUNT, not from the
// instruction's bit count, so a short OUT zero-fills the rest of the group.
// This was measured on the bench (GP22 read 1.0% duty where a latched marker
// would read 33%) after an earlier version of this program relied on the
// opposite, and it is worth knowing because it contradicts the natural reading
// of the instruction set.
//
// So the between-pulses value is carried in a register instead. Each burst
// parks a "gap mask" in ISR -- the cycle bit alone, or 0 -- and the low phase
// does `mov osr, isr` + a FULL-WIDTH out. Same instruction count as the
// `mov osr, null` it replaces, so no timing derivation below moves.
//
// Every burst the TX FIFO is fed (DMA, one linear transfer re-armed on
// completion by the DMA's own IRQ -- see elongDmaIrqHandler()), in EXACTLY
// the order the program below pulls it:
//   N-1, the gap mask, then N triples of
//   (select code, hi_cycles-1, lo_cycles-1), then
//   mute_cycles-1 LAST (pulled once, after the on-phase loop below exits --
//   not second, or pulse 1 gets fed the mute count instead of its own
//   hi-phase).
// The select lane is pulled at the START of each low phase rather than just
// before the next rising edge, so it has the whole of T2 to settle -- an opto
// downstream of the decoder needs microseconds, not the ~27 ns four clocks
// would buy.
// Loop overhead, worked out instruction-by-instruction against the listing
// below (each is "until the pins next actually move", so loop-exit and the
// following setup instructions count):
//   hi_cycles   = pushed+4    set pins,3 .. set pins,1
//   lo_cycles   = pushed+9    set pins,1 .. the next set pins,3, including the
//                             closing `jmp x--` and the next pulse's setup
//   mute_cycles = pushed+11   set pins,0 .. the next burst's set pins,3, past
//                             instr21's jmp and the whole burst_start preamble
// Only the mute constant moved when the gap mask was added (9 -> 11): its two
// new instructions sit in the burst_start preamble, inside the window the mute
// is measured over. hi and lo are UNCHANGED -- nothing was inserted between
// `set pins,3` and `set pins,1`, and the low phase swapped one MOV for another
// -- so an exactly 1.000 us T1 still lands on 150 integer clocks.
static const uint16_t elongate_insns[] = {
  0x80a0,  //  0: pull block          OSR = N-1                   burst_start:
  0xa027,  //  1: mov  x, osr
  0x80a0,  //  2: pull block          OSR = gap mask (cycle bit alone, or 0)
  0xa0c7,  //  3: mov  isr, osr         parked for every gap in this burst
  0x80a0,  //  4: pull block          OSR = channel mask | cycle   pulse:
  0x6005,  //  5: out  pins, 5          channel HIGH (before the pulse)
  0x80a0,  //  6: pull block          OSR = hi-1
  0xa047,  //  7: mov  y, osr
  0xe003,  //  8: set  pins, 3
  0x0089,  //  9: jmp  y--, 9       hi:
  0x80a0,  // 10: pull block          OSR = lo-1
  0xa047,  // 11: mov  y, osr
  0xe001,  // 12: set  pins, 1
  0xa0e6,  // 13: mov  osr, isr        the gap mask, not zero -- holds GP22
  0x6005,  // 14: out  pins, 5         channel LOW, marker unchanged
  0x008f,  // 15: jmp  y--, 15      lo:
  0x0044,  // 16: jmp  x--, 4         more pulses this burst?
  0x80a0,  // 17: pull block          OSR = mute-1
  0xa047,  // 18: mov  y, osr
  0xe000,  // 19: set  pins, 0
  0x0094,  // 20: jmp  y--, 20      mute:
  0x0000,  // 21: jmp  0
};
static const pio_program_t elongate_program = {
  .instructions = elongate_insns,
  .length       = count_of(elongate_insns),
  .origin       = -1,
};

// PIO resources are claimed cooperatively rather than hardcoded: on a Pico W
// the CYW43 WiFi chip is itself driven by a PIO state machine, allocated with
// pio_claim_free_sm_and_add_program_for_gpio_range(). Hardcoding pio0/pio1
// would risk silently fighting it. RP2350 has three PIO blocks, so asking the
// SDK for free ones always succeeds here regardless of init order.
static PIO  pioGate = nullptr;  static uint smGate = 0;  static uint offGating = 0;
static PIO  pioGen  = nullptr;  static uint smGen  = 0;  static uint offGen    = 0;
static uint smCarrier = 0;
static bool pioReady  = false;

static PIO  pioElong = nullptr; static uint smElong = 0; static uint offElong = 0;
static bool elongPioReady = false;
// Declared up here because applyGate() clears it when it routes to the normal
// carrier+gate path, and that sits well above the ring itself.
static uint32_t ringLen = 0;                // 0 = no ring, single table
static int  elongDmaChan  = -1;

// ============================================================================
// Parameters
// ============================================================================

enum GateMode : uint8_t { MODE_TIME = 0, MODE_COUNT = 1 };
static GateMode gateMode = MODE_COUNT;

// Where the pulses being gated come from. INT runs a second copy of gate_gen
// as a carrier oscillator on GP2 and gates that, so the box is a complete burst
// generator with nothing attached. The gating engine cannot tell the difference
// — it just reads a different input pin.
enum SigSource : uint8_t { SRC_EXT = 0, SRC_INT = 1 };
static SigSource sigSource = SRC_EXT;

// What the two carrier encoders (GP11/12 and GP14/16) currently edit. Same
// two representations the dashboard's fd/t12 toggle already offers -- this
// just brings that choice to the front panel, via the ENC2 pushbutton.
enum ShapeMode : uint8_t { SHAPE_FD = 0, SHAPE_T12 = 1 };
static ShapeMode shapeMode = SHAPE_FD;

// COUNT mode. PIO counters are 32-bit, so the Nano's 65536-total ceiling is
// gone; 1e9 is a UI sanity limit, not a hardware one.
static uint32_t onCount  = 10;
static uint32_t offCount = 90;
static const uint32_t COUNT_MIN = 1;
static const uint32_t COUNT_MAX = 1000000000UL;

// TIME mode. Nanoseconds needs 64 bits now that the range reaches 60 s.
static uint64_t gatePeriodNs = 10000000ULL;   // 10 ms -> 100 Hz
static uint32_t gateDutyPpm  = 100000UL;      // 10 %
static const uint64_t PERIOD_NS_MIN = 1000ULL;           // 1 us
static const uint64_t PERIOD_NS_MAX = 60000000000ULL;    // 60 s
static const uint32_t DUTY_PPM_MIN  = 100UL;             // 0.01 %
static const uint32_t DUTY_PPM_MAX  = 999900UL;          // 99.99 %

static uint64_t carrierPeriodNs = 10000ULL;   // 10 us -> 100 kHz
static uint32_t carrierDutyPpm  = 500000UL;   // 50 %
static const uint64_t CARRIER_NS_MIN = 200ULL;           // 5 MHz
static const uint64_t CARRIER_NS_MAX = 1000000000ULL;    // 1 Hz

// T1/T2 elongation ramp. Auto-steps the carrier's own width (T1) and/or space
// (T2) by a fixed amount every N bursts, holding or wrapping back to the start
// once a limit is reached -- e.g. to sweep toward a resonance/breakdown point
// repeatably instead of hand-tuning into it every time. Independent of COUNT
// vs TIME gate mode: this only ever touches carrierPeriodNs/carrierDutyPpm,
// the same state C/CD already own, so it needs no PIO changes at all -- it
// rides the same live-reconfigure path a manual carrier edit already uses.
struct RampCfg {
  bool     active        = false;
  bool     wrap          = false;   // false = hold at the limit, true = restart at startNs
  int64_t  stepNs        = 0;       // signed: +grow, -shrink
  uint32_t burstsPerStep = 1;
  uint64_t limitNs       = 0;       // stepping stops/wraps once reached (direction = sign of stepNs)
  uint64_t startNs       = 0;       // T1/T2 value when armed -- the wrap target
  uint64_t nextStepAtUs  = 0;       // absolute time_us_64() deadline for the next step
};
// What the mode knob arms when it lands on SWEEP. Seeded with a usable default
// so the button does something sensible on a fresh board, then overwritten by
// whatever RAMP1 was last given -- the knob should re-arm the sweep you set up,
// not a canned one.
static int64_t  sweepStepNs  = 200;      // 0.2 us per step
static uint32_t sweepBursts  = 50;
static uint64_t sweepLimitNs = 9000;     // 9 us
static bool     sweepWrap    = true;

static RampCfg ramp1;   // T1 = carrier ON width
static RampCfg ramp2;   // T2 = carrier OFF width (space)

// Elongation: each pulse in a COUNT-mode burst compounds off the last, T1 and
// T2 independently -- pulse 1 is exactly the base T1/T2 the ENC2/ENC3 knobs
// set. Distinct from RAMP1/RAMP2 above: those step the carrier BETWEEN bursts
// over many seconds; this reshapes pulses WITHIN a single burst, which needs
// the dedicated elongate_program SM rather than the normal carrier+gate pair
// -- see elongArm()/elongStart() near applyGate().
//
// The stored knob is `ratio`: the TOTAL span across the burst (last pulse /
// first pulse), NOT the per-pulse multiplier. The per-pulse factor is derived
// from it and the pulse count. That distinction is the whole point, because
// the compounding is exponential in onCount: a fixed per-pulse 1.618 is a
// pleasant 76x span over 10 pulses and an unusable 103,682x over 25, which
// saturates elongNsToCycles() and flattens the tail into a row of identically
// clamped pulses. Holding the TOTAL span fixed instead makes the burst's
// shape independent of how many pulses are in it -- dial "10x across the
// burst" and it reads the same at N=5 and at N=25, which is what you actually
// want when sweeping pulse count against a load.
struct ElongCfg {
  bool   active = false;
  double ratio  = 8.0;    // default if armed before ever being configured
};
static ElongCfg elong;
// Was 25, and the binding constraint was never the PIO or the DMA -- it was
// that this same constant sizes the persisted seqSteps[] array, which had to
// fit the 512-byte settings budget. Raising the budget (still one flash
// sector) lifts both together, so a 100-pulse burst and a 100-step SEQ table
// stay the same number rather than becoming two limits to remember.
//
// Costs, both checked: the burst ring grows to RING_MAX * (3 + 3*N) words
// (24 * 303 * 4 = 29 KB of RAM, against ~436 KB free), and Settings grows to
// ~1.3 KB, guarded by the static_assert next to CFG_EE_SIZE.
static const uint32_t ELONG_MAX_PULSES = 100;
static const double   ELONG_RATIO_MIN  = 0.01;
static const double   ELONG_RATIO_MAX  = 1000.0;
static const double   ELONG_PHI        = 1.6180339887498949;

static bool outputEnabled = true;
static bool gateInvert    = false;   // swap the open/closed phases
static bool pulseInvert   = false;   // hardware pad inverter on the output

// ---- HV interlock ---------------------------------------------------------
// Two separate things, deliberately not merged:
//   outputEnabled     what the operator asked for
//   interlockTripped  what the enclosure says it is allowed to do
// Folding the interlock into outputEnabled would let a trip get written to
// flash as an intent, so re-arming would silently restore a state the operator
// never chose -- and worse, the saved "output off" would look like a setting
// rather than a fault.
//
// Latching on purpose. An interlock that clears itself the moment the lid
// shuts brings HV back up with nobody's hand on a control; the operator has to
// say ARM. That is the whole difference between an interlock and a switch.
static bool     interlockEnabled  = false;   // persisted; off unless wired
static bool     interlockTripped  = false;   // runtime only, never saved
static uint32_t interlockOpenAt   = 0;       // first HIGH sample of a trip
static uint32_t interlockShutAt   = 0;       // start of the current closed run
static uint32_t interlockTrips    = 0;       // since boot, for the readout

// Asymmetric on purpose: 2 ms to trip rejects a dV/dt spike coupled off the
// coils without meaningfully delaying a real one (a lid cannot move in 2 ms),
// while 250 ms of *continuous* closure before ARM is accepted stops a bouncing
// or intermittent contact from being re-armed into.
static const uint32_t INTERLOCK_TRIP_MS = 2;
static const uint32_t INTERLOCK_SHUT_MS = 250;

static inline bool interlockClosed() { return gpio_get(PIN_INTERLOCK) == 0; }

// The one question the waveform path asks. Everything that used to test
// outputEnabled directly must test this instead, or the interlock is advice.
static inline bool outputLive() { return outputEnabled && !interlockTripped; }

// Live encoder debug -- off by default, toggled with ENCLOG. Declared up
// here (not down with the rest of the encoder code) because the ENCLOG
// command handler needs it and runCommand() comes before that section.
static bool encLogEnabled = false;

static uint8_t selParam = 0;         // 0 = ON/PERIOD, 1 = OFF/DUTY

// One-shot run window. `RUN <ms>` enables the output for exactly this long and
// then shuts it off again, which is what makes a scope capture correspond to a
// known experiment rather than to whenever you happened to press the button.
// Arm the scope single-shot on the GP4 burst marker first: the output is
// silent before RUN, so the scope cannot trigger on anything else.
static uint32_t runUntilMs = 0;      // 0 = not in a timed run
static uint32_t runLenMs   = 0;

// SHOT: emit exactly ONE burst, then stop. Rather than have the CPU chase the
// PIO to halt it mid-stream — which could truncate a pulse — this sets the mute
// count to its 32-bit maximum. The state machine emits its ON pulses, enters the
// mute loop, and parks there for 4.29e9 input pulses (over a day at 10 kHz), so
// the single burst is bounded by the PIO itself and is exactly as clean as a
// free-running one. Firing again is just another restart.
static bool shotMode = false;

static uint32_t achGenDiv = 1, achHiCyc = 0, achLoCyc = 0;
static uint32_t achCarDiv = 1, achCarHi = 0, achCarLo = 0;

static float measuredHz = 0.0f;      // EXT only; INT is computed exactly

// Frequency of whatever is actually feeding the gate.
static double sourceHz() {
  if (sigSource == SRC_INT) {
    if (!achCarHi && !achCarLo) return 0.0;
    double tick = (double)achCarDiv / (double)clock_get_hz(clk_sys);
    return 1.0 / (tick * (double)(achCarHi + achCarLo));
  }
  return (double)measuredHz;
}

// ============================================================================
// Gate engine
// ============================================================================

static void gateStop() {
  if (!pioReady) return;
  pio_sm_set_enabled(pioGate, smGate,    false);
  pio_sm_set_enabled(pioGen,  smGen,     false);
  pio_sm_set_enabled(pioGen,  smCarrier, false);
  if (elongPioReady) {
    pio_sm_set_enabled(pioElong, smElong, false);
    dma_channel_abort(elongDmaChan);
  }
}

// Hand every driven pin back to SIO and park it low. The amplitude select goes
// with them: leaving the last code latched would hold one decoder output — and
// therefore one high-side leg's opto — asserted after the output was stopped.
// Code 0 is the deselected state, so parking low deselects.
static void parkPins(const uint *pins, size_t n) {
  for (size_t i = 0; i < n; i++) {
    gpio_set_function(pins[i], GPIO_FUNC_SIO);
    gpio_set_dir(pins[i], GPIO_OUT);
    gpio_put(pins[i], 0);
  }
}

// Just the OUT group. Split out from gateParkPins() because the normal
// carrier+gate path needs it too: that path never drives these pins, so without
// it whatever the elongation engine last latched stays asserted after a mode
// change -- one decoder output (and therefore one high-side leg's opto) held
// on, and a cycle marker stuck high, with nothing running that would ever
// rewrite them.
static void ampParkPins() {
  const uint pins[] = { ampBase, ampBase + 1, ampBase + 2, cyclePin() };
  parkPins(pins, count_of(pins));
}

static void gateParkPins() {
  const uint pins[] = { PIN_GATED_OUT, PIN_GATE_OUT, PIN_CARRIER };
  parkPins(pins, count_of(pins));
  ampParkPins();
}

// Cycle counts for one gate_gen instance: pick a clock divider that keeps both
// half-periods inside a 32-bit counter, then split the period by duty. Shared
// by the TIME-mode gate and the internal carrier — same program, same maths.
static void genCycles(uint64_t periodNs, uint32_t dutyPpm, bool invert,
                      uint32_t *div, uint32_t *hi, uint32_t *lo) {
  uint64_t fsys  = clock_get_hz(clk_sys);
  uint64_t total = (periodNs * fsys) / 1000000000ULL;
  uint32_t d     = 1;
  while (total > 2000000000ULL) { d <<= 1; total >>= 1; }
  if (total < 7) total = 7;                       // 3 high + 4 low minimum

  uint64_t h = (total * dutyPpm) / 1000000ULL;
  if (invert) h = total - h;
  if (h < 3)         h = 3;
  if (h > total - 4) h = total - 4;

  *div = d;
  *hi  = (uint32_t)h;
  *lo  = (uint32_t)(total - h);
}

// Bring up one gate_gen state machine on `pin`. FIFO pushes must follow
// pio_sm_init(), which clears the FIFOs.
static void startGen(uint sm, uint pin, uint32_t div, uint32_t hi, uint32_t lo) {
  pio_gpio_init(pioGen, pin);
  pio_sm_set_consecutive_pindirs(pioGen, sm, pin, 1, true);

  pio_sm_config g = pio_get_default_sm_config();
  sm_config_set_wrap(&g, offGen, offGen + gate_gen_program.length - 1);
  sm_config_set_set_pins(&g, pin, 1);
  sm_config_set_clkdiv_int_frac(&g, div, 0);

  pio_sm_init(pioGen, sm, offGen, &g);
  pio_sm_put_blocking(pioGen, sm, lo - 4);   // consumed by pull #1
  pio_sm_put_blocking(pioGen, sm, hi - 3);   // consumed by pull #2
  pio_sm_set_enabled(pioGen, sm, true);
}

// Defined down in the settings section; applyGate() is the one funnel every
// waveform change goes through, so it is where a save gets armed.
static void cfgTouch();

static void applyGate() {
  if (!pioReady) return;
  cfgTouch();                 // arm a deferred save; the write happens once quiet
  gateStop();

  // gateParkPins() takes the channel pins and the cycle marker down with it,
  // so a trip leaves nothing driven anywhere.
  if (!outputLive()) {
    gateParkPins();
    return;
  }

  // Elongation owns pulse generation itself (a burst's worth of T1/T2 pairs
  // it streams from its own table), so it fully replaces the normal
  // carrier+gate pair rather than layering on top of them -- meaningless
  // outside COUNT+INT, where "pulse 1..N of this burst" is well-defined.
  // PHASE lives on the elongation state machine -- it is the only one with an
  // OUT group and a per-burst DMA table. So the engine runs whenever EITHER is
  // wanted. With elongation off it simply emits a flat train, which is the
  // same waveform the normal carrier+gate path would have produced; before this
  // check included phaseMode, switching elongation off dropped back to that
  // path and the three channel pins went dead.
  if ((elong.active || phaseOn()) && elongPioReady
      && sigSource == SRC_INT && gateMode == MODE_COUNT) {
    elongStart();
    return;
  }
  // Falling through to the normal carrier+gate path: the ring belongs to the
  // elongation engine, so leave no stale length behind for the status readout
  // to report a pattern that is not running -- and no stale levels on the pins
  // only that engine drives.
  ringLen = 0;
  ampParkPins();

  // Internal carrier first, so the gating SM has something to read the instant
  // it starts. Its pin doubles as a plain sig-gen output you can tap.
  if (sigSource == SRC_INT) {
    uint32_t cdiv, chi, clo;
    genCycles(carrierPeriodNs, carrierDutyPpm, false, &cdiv, &chi, &clo);
    achCarDiv = cdiv; achCarHi = chi; achCarLo = clo;
    startGen(smCarrier, PIN_CARRIER, cdiv, chi, clo);
  } else {
    achCarDiv = 0; achCarHi = 0; achCarLo = 0;
    gpio_set_function(PIN_CARRIER, GPIO_FUNC_SIO);
    gpio_set_dir(PIN_CARRIER, GPIO_OUT);
    gpio_put(PIN_CARRIER, 0);
  }
  const uint inPin = (sigSource == SRC_INT) ? PIN_CARRIER : PIN_SIG_IN;

  pio_sm_config c = pio_get_default_sm_config();
  sm_config_set_wrap(&c, offGating, offGating + gating_program.length - 1);
  sm_config_set_in_pins(&c, inPin);
  sm_config_set_clkdiv_int_frac(&c, 1, 0);   // full sys clock, lowest latency

  if (gateMode == MODE_COUNT) {
    uint32_t on  = onCount  < COUNT_MIN ? COUNT_MIN : onCount;
    uint32_t off = shotMode ? 0xFFFFFFFFUL
                            : (offCount < COUNT_MIN ? COUNT_MIN : offCount);
    if (gateInvert && !shotMode) { uint32_t t = on; on = off; off = t; }
    achHiCyc = on; achLoCyc = off; achGenDiv = 0;

    // One SET group covers both pins: bit0 = GP4 marker, bit1 = GP5 output.
    pio_gpio_init(pioGate, PIN_GATE_OUT);
    pio_gpio_init(pioGate, PIN_GATED_OUT);
    pio_sm_set_consecutive_pindirs(pioGate, smGate, PIN_GATE_OUT, 2, true);
    sm_config_set_set_pins(&c, PIN_GATE_OUT, 2);

    pio_sm_init(pioGate, smGate, offGating + GATING_ENTRY_COUNT, &c);
    pio_sm_put_blocking(pioGate, smGate, off - 1);   // pull #1
    pio_sm_put_blocking(pioGate, smGate, on  - 1);   // pull #2
    pio_sm_set_enabled(pioGate, smGate, true);
  } else {
    uint32_t div, hi, lo;
    genCycles(gatePeriodNs, gateDutyPpm, gateInvert, &div, &hi, &lo);
    achGenDiv = div; achHiCyc = hi; achLoCyc = lo;

    // Gate generator owns GP4 here, so the gating SM must not drive it: its
    // SET group is GP5 alone.
    startGen(smGen, PIN_GATE_OUT, div, hi, lo);

    pio_gpio_init(pioGate, PIN_GATED_OUT);
    pio_sm_set_consecutive_pindirs(pioGate, smGate, PIN_GATED_OUT, 1, true);
    sm_config_set_set_pins(&c, PIN_GATED_OUT, 1);
    sm_config_set_jmp_pin(&c, PIN_GATE_OUT);

    pio_sm_init(pioGate, smGate, offGating + GATING_ENTRY_TIME, &c);
    pio_sm_set_enabled(pioGate, smGate, true);
  }

  // Must come last: gpio_set_function() (inside pio_gpio_init) zeroes every
  // CTRL field except FUNCSEL, so setting the pad inverter any earlier would be
  // silently wiped. The pad's own inverter costs zero PIO instructions.
  gpio_set_outover(PIN_GATED_OUT,
                   pulseInvert ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
}

// ---- Elongation: per-pulse compounding within one burst -------------------

// N-1, the gap mask, then N (select, hi-1, lo-1) triples, then mute-1 -- 3N+3
// words, which is what the expression below spells out. See elongate_insns
// above for the exact per-phase overhead the -1s compensate for.
static const uint32_t ELONG_WORDS = 3 + 3 * ELONG_MAX_PULSES;
static uint32_t elongTable[ELONG_WORDS];
static volatile uint32_t elongTableLen = 0;

// ---- Per-train amplitude ring ---------------------------------------------
//
// A ring of complete burst tables, one per amplitude level, advanced by the
// DMA's own completion IRQ. Burst 1 fires table 0, burst 2 table 1, and so on,
// wrapping -- so an envelope can span a whole TRAIN of bursts instead of
// resetting every burst the way a single table does.
//
// Amplitude here is width: for an inductive primary i = V*T1/L, so scaling
// every T1 in a burst scales the energy that burst delivers. That needs no
// hardware at all -- unlike tap select, which needs the decoder and legs.
//
// The tables are built ONCE at arm time, never in the ISR. Building one calls
// pow() and does floating point, which has no business in an interrupt that
// fires every burst; the handler does nothing but move a pointer.
//
// Every table in the ring holds the same pulse count, so they are all the same
// length. That is what lets the ISR swap only the read address, exactly as the
// single-table path has always done -- no transfer-count reload, no change to
// a DMA pattern that already works.
static const uint32_t TRAIN_MAX = 8;
// The ring now carries TWO per-burst sequences -- amplitude level and output
// channel -- so its length is the lcm of the two, not just the level count.
// lcm(8,3) = 24 is the worst case, hence the separate cap.
static const uint32_t RING_MAX  = 24;
static uint32_t elongRing[RING_MAX][ELONG_WORDS];
static uint8_t  trainAmp[TRAIN_MAX];        // percent of base T1, 1..100
static uint32_t trainLen = 0;               // 0 = ring off, single table
static volatile uint32_t trainIdx = 0;      // which table the NEXT burst uses

// ---- Output channel rotation (PHASE) --------------------------------------
//
// Fires one burst on GP18, the next on GP19, the next on GP20, then repeats --
// each separated by the normal off-time, because the mute is simply part of
// every burst's table. The three lines are the outputs themselves, driven by
// the same DMA stream as the timing, so no external logic is needed.
//
// PH_OFF hands the OUT lane back to SEQ's per-pulse codes for tap select. The
// two uses share three pins and are mutually exclusive.
//
//   PH_ROTATE  one burst per channel, advancing -- CH1, gap, CH2, gap, CH3...
//   PH_SYNC    all channels fire together on every burst
enum PhaseMode : uint8_t { PH_OFF = 0, PH_ROTATE = 1, PH_SYNC = 2 };
static uint8_t  phaseMode = PH_OFF;
static uint32_t phaseLen  = 3;              // channels in use, 1..AMP_BITS
static inline bool phaseOn() { return phaseMode != PH_OFF; }



// ---- Arbitrary per-pulse sequence -----------------------------------------
//
// The PIO never cared where the numbers came from -- it just reads a table --
// so an explicit list of steps costs nothing but the parsing. `seqLen == 0`
// means "no table loaded", and elongation falls back to the geometric ratio it
// always used. Sharing ELONG_MAX_PULSES rather than inventing a second cap:
// the PIO table is the same table, so the same limit is the real one.
struct SeqStep {
  uint32_t t1_ns;
  uint32_t t2_ns;
  uint8_t  amp;      // 0..AMP_MASK, driven on GP18-20 during the PRIOR gap
};
static SeqStep  seqSteps[ELONG_MAX_PULSES];
static uint32_t seqLen = 0;

// ============================================================================
// Persistent settings — survive a power cycle
// ============================================================================
//
// The point is standalone use: wire up the knobs, dial in a train, unplug it,
// plug it back in tomorrow and it is still doing that. No serial, no dashboard.
//
// Stored in one emulated-EEPROM sector (really a 4 KB flash sector the core
// erases and rewrites whole). Two consequences shape everything below:
//
//   1. WRITING STOPS INTERRUPTS. EEPROM.commit() calls noInterrupts() and then
//      erases a sector, which is tens of milliseconds. The normal carrier+gate
//      path does not care at all -- it is pure PIO and needs no CPU. But the
//      elongation path re-arms its DMA from an IRQ, so a save while elongation
//      is running leaves the state machine waiting on an empty FIFO until
//      interrupts come back: a brief gap in the output, not a hang. Hence the
//      deferred, change-gated save below rather than writing on every click.
//
//   2. FLASH WEARS OUT. ~100k erase cycles. Saving per encoder detent would
//      burn that in an afternoon, so a save happens at most once per editing
//      session and only if the bytes actually differ.
static const uint32_t CFG_MAGIC    = 0x47504C53UL;   // 'GPLS'
static const uint16_t CFG_VERSION  = 4;   // 4: ELONG_MAX_PULSES 25 -> 100
// Still one 4 KB flash sector -- the core erases and rewrites the whole sector
// either way, so this costs no extra wear and no extra write time, only a
// larger staging buffer during begin()/end().
static const uint32_t CFG_EE_SIZE  = 2048;
static const uint32_t CFG_SETTLE_MS = 5000;          // quiet time before saving

// Boot behaviour for the output enable, independent of the rest of the state.
// SAVED is what a bench instrument normally does; RUN is for a box that lives
// in a rig and should come straight back up pulsing after a power blip.
enum BootMode : uint8_t { BOOT_SAVED = 0, BOOT_RUN = 1, BOOT_OFF = 2 };

struct __attribute__((packed)) Settings {
  uint32_t magic;
  uint16_t version;
  uint16_t size;

  uint8_t  gateMode, sigSource, shapeMode, selParam;
  uint8_t  gateInvert, pulseInvert, outputEnabled, bootMode;
  uint8_t  autosave, elongActive, trainLen, seqLen;
  // interlockEnabled claims one of the two spare pad bytes rather than growing
  // the struct, so CFG_VERSION does not move and existing saved settings still
  // load. Old blobs zeroed the padding, and zero here means "interlock off" --
  // which is the right thing to inherit on a rig that has no sensor wired.
  // The *tripped* state is deliberately not persisted: a fault is not a
  // setting, and HV must never come back armed after a power cycle.
  uint8_t  phaseLen, phaseMode, interlockEnabled, _pad[1];

  uint32_t onCount, offCount;
  uint64_t gatePeriodNs;
  uint32_t gateDutyPpm;
  uint64_t carrierPeriodNs;
  uint32_t carrierDutyPpm;
  double   elongRatio;

  uint8_t  trainAmp[TRAIN_MAX];
  SeqStep  seqSteps[ELONG_MAX_PULSES];

  uint32_t crc;                                     // must stay LAST
};

// The check that was missing when ELONG_MAX_PULSES was 25. Nothing at runtime
// notices Settings outgrowing the budget -- cfgWrite() memcpy's sizeof(s) into
// a CFG_EE_SIZE buffer, so an overflow is a silent stomp past the end of it,
// and the symptom would be corrupted settings rather than a build error.
// Raise CFG_EE_SIZE (up to 4096, one sector) if this ever fires.
static_assert(sizeof(Settings) <= CFG_EE_SIZE,
              "Settings no longer fits CFG_EE_SIZE -- raise it (max 4096)");

static bool     cfgAutosave = true;
static uint8_t  cfgBootMode = BOOT_SAVED;
static uint32_t cfgDirtyAt  = 0;      // 0 = clean
static bool     cfgReady    = false;  // suppresses dirty marks during boot/apply
static uint32_t cfgLastCrc  = 0;      // what is already in flash

static uint32_t cfgCrc32(const uint8_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFFUL;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320UL & (-(int32_t)(c & 1)));
  }
  return ~c;
}

static void cfgCapture(Settings &s) {
  memset(&s, 0, sizeof(s));           // zero the padding too, or the CRC of two
  s.magic   = CFG_MAGIC;              // identical states can differ
  s.version = CFG_VERSION;
  s.size    = (uint16_t)sizeof(Settings);

  s.gateMode      = (uint8_t)gateMode;
  s.sigSource     = (uint8_t)sigSource;
  s.shapeMode     = (uint8_t)shapeMode;
  s.selParam      = selParam;
  s.gateInvert    = gateInvert ? 1 : 0;
  s.pulseInvert   = pulseInvert ? 1 : 0;
  s.outputEnabled = outputEnabled ? 1 : 0;
  s.bootMode      = cfgBootMode;
  s.autosave      = cfgAutosave ? 1 : 0;
  s.elongActive   = elong.active ? 1 : 0;
  s.trainLen      = (uint8_t)trainLen;
  s.phaseLen      = (uint8_t)phaseLen;
  s.phaseMode     = phaseMode;
  s.interlockEnabled = interlockEnabled ? 1 : 0;
  s.seqLen        = (uint8_t)seqLen;

  s.onCount        = onCount;
  s.offCount       = offCount;
  s.gatePeriodNs   = gatePeriodNs;
  s.gateDutyPpm    = gateDutyPpm;
  s.carrierPeriodNs= carrierPeriodNs;
  s.carrierDutyPpm = carrierDutyPpm;
  s.elongRatio     = elong.ratio;

  for (uint32_t i = 0; i < TRAIN_MAX; i++) s.trainAmp[i] = trainAmp[i];
  for (uint32_t i = 0; i < ELONG_MAX_PULSES; i++) s.seqSteps[i] = seqSteps[i];

  s.crc = cfgCrc32((const uint8_t *)&s, sizeof(Settings) - sizeof(uint32_t));
}

// Everything here is range-checked on the way in. A corrupted or half-written
// blob that passed the CRC by luck must not be able to command a wild output --
// this is a generator wired to a cell, and "trust the flash" is not a policy.
static void cfgApply(const Settings &s) {
  gateMode    = s.gateMode ? MODE_COUNT : MODE_TIME;
  sigSource   = s.sigSource ? SRC_INT : SRC_EXT;
  shapeMode   = s.shapeMode ? SHAPE_T12 : SHAPE_FD;
  selParam    = s.selParam ? 1 : 0;
  gateInvert  = s.gateInvert;
  pulseInvert = s.pulseInvert;
  cfgBootMode = (s.bootMode <= BOOT_OFF) ? s.bootMode : BOOT_SAVED;
  cfgAutosave = s.autosave ? true : false;

  onCount  = (s.onCount  < COUNT_MIN) ? COUNT_MIN
           : (s.onCount  > COUNT_MAX) ? COUNT_MAX : s.onCount;
  offCount = (s.offCount < COUNT_MIN) ? COUNT_MIN
           : (s.offCount > COUNT_MAX) ? COUNT_MAX : s.offCount;

  gatePeriodNs = (s.gatePeriodNs < PERIOD_NS_MIN) ? PERIOD_NS_MIN
               : (s.gatePeriodNs > PERIOD_NS_MAX) ? PERIOD_NS_MAX : s.gatePeriodNs;
  carrierPeriodNs = (s.carrierPeriodNs < CARRIER_NS_MIN) ? CARRIER_NS_MIN
                  : (s.carrierPeriodNs > CARRIER_NS_MAX) ? CARRIER_NS_MAX
                  : s.carrierPeriodNs;
  gateDutyPpm = (s.gateDutyPpm < DUTY_PPM_MIN) ? DUTY_PPM_MIN
              : (s.gateDutyPpm > DUTY_PPM_MAX) ? DUTY_PPM_MAX : s.gateDutyPpm;
  carrierDutyPpm = (s.carrierDutyPpm < DUTY_PPM_MIN) ? DUTY_PPM_MIN
                 : (s.carrierDutyPpm > DUTY_PPM_MAX) ? DUTY_PPM_MAX : s.carrierDutyPpm;

  elong.ratio = (s.elongRatio >= ELONG_RATIO_MIN && s.elongRatio <= ELONG_RATIO_MAX)
              ? s.elongRatio : 8.0;
  elong.active = s.elongActive && elongPioReady;

  trainLen = (s.trainLen <= TRAIN_MAX) ? s.trainLen : 0;
  phaseLen  = (s.phaseLen >= 1 && s.phaseLen <= AMP_BITS) ? s.phaseLen : 3;
  phaseMode = (s.phaseMode <= PH_SYNC) ? s.phaseMode : PH_OFF;

  // Restored armed-but-tripped, never armed-and-live. Whatever the enclosure
  // was doing when power went away is not knowable now, so the operator has to
  // look at it and say ARM.
  interlockEnabled = s.interlockEnabled ? true : false;
  interlockTripped = interlockEnabled;
  for (uint32_t i = 0; i < TRAIN_MAX; i++)
    trainAmp[i] = (s.trainAmp[i] >= 1 && s.trainAmp[i] <= 100) ? s.trainAmp[i] : 100;

  seqLen = (s.seqLen <= ELONG_MAX_PULSES) ? s.seqLen : 0;
  for (uint32_t i = 0; i < ELONG_MAX_PULSES; i++) {
    seqSteps[i] = s.seqSteps[i];
    seqSteps[i].amp &= AMP_MASK;
  }
  // Validate only the steps actually in use. The slots past seqLen are zero
  // by construction (cfgCapture memsets the whole blob), so validating the
  // full array treated that zero padding as corruption and threw the table
  // away -- which meant any SEQ shorter than the maximum silently failed to
  // survive a power cycle, reported afterwards as an innocent "SEQ off".
  for (uint32_t i = 0; i < seqLen; i++) {
    if (!seqSteps[i].t1_ns || !seqSteps[i].t2_ns) { seqLen = 0; break; }
  }

  switch (cfgBootMode) {
    case BOOT_RUN: outputEnabled = true;  break;
    case BOOT_OFF: outputEnabled = false; break;
    default:       outputEnabled = s.outputEnabled ? true : false;
  }
}

static bool cfgWrite(Print &o) {
  Settings s;
  cfgCapture(s);
  if (s.crc == cfgLastCrc) { o.println("settings unchanged -- nothing written"); return true; }

  EEPROM.begin(CFG_EE_SIZE);
  uint8_t *dst = EEPROM.getDataPtr();
  memcpy(dst, &s, sizeof(s));
  const bool ok = EEPROM.commit();
  EEPROM.end();
  if (ok) cfgLastCrc = s.crc;
  o.println(ok ? "settings saved" : "ERROR: settings write failed");
  return ok;
}

// Returns true if a valid blob was found AND applied. Caller decides what to do
// with false -- on boot that means "keep the compiled-in defaults", which is
// exactly right for a board that has never been saved to.
static bool cfgRead(bool apply) {
  EEPROM.begin(CFG_EE_SIZE);
  Settings s;
  memcpy(&s, EEPROM.getConstDataPtr(), sizeof(s));
  EEPROM.end();

  if (s.magic != CFG_MAGIC) return false;
  if (s.version != CFG_VERSION) return false;    // layout changed: ignore, do not guess
  if (s.size != sizeof(Settings)) return false;
  const uint32_t want = cfgCrc32((const uint8_t *)&s, sizeof(Settings) - sizeof(uint32_t));
  if (want != s.crc) return false;

  cfgLastCrc = s.crc;
  if (apply) { cfgReady = false; cfgApply(s); cfgReady = true; }
  return true;
}

// Called from applyGate(), i.e. after anything that changes the waveform. Only
// arms a timer -- the write itself happens once the knobs go quiet, so a sweep
// across a decade is one save rather than sixty.
static void cfgTouch() {
  if (cfgReady && cfgAutosave) cfgDirtyAt = millis();
}

static void cfgPoll(Print &o) {
  if (!cfgDirtyAt || !cfgAutosave) return;
  if (millis() - cfgDirtyAt < CFG_SETTLE_MS) return;
  cfgDirtyAt = 0;
  Settings s;
  cfgCapture(s);
  if (s.crc == cfgLastCrc) return;               // nothing really changed
  EEPROM.begin(CFG_EE_SIZE);
  memcpy(EEPROM.getDataPtr(), &s, sizeof(s));
  if (EEPROM.commit()) cfgLastCrc = s.crc;
  EEPROM.end();
  (void)o;
}


// elongate_program always runs at clkdiv 1 (full sys clock, matching the
// gating SM's own "lowest latency" choice) rather than picking a divider the
// way genCycles() does for the free-running carrier. That caps any single
// phase at 0xFFFFFFFF cycles (~28.6 s at 150 MHz) -- fine for a feature whose
// whole point is a handful of pulses within one burst; a multi-second mute
// between elongated bursts is already well outside what this is for.
static uint32_t elongNsToCycles(uint64_t ns) {
  uint64_t fsys = clock_get_hz(clk_sys);
  uint64_t c = (ns * fsys) / 1000000000ULL;
  if (c > 0xFFFFFFFFULL) c = 0xFFFFFFFFULL;
  return (uint32_t)c;
}

// Pulses this burst will actually generate: onCount, floored at 1 and capped
// at what the table can hold. Both the width math and the reported per-pulse
// factor have to agree on this number or the two disagree at onCount > 25.
static uint32_t elongPulseCount() {
  uint32_t n = onCount;
  if (n < 1) n = 1;
  if (n > ELONG_MAX_PULSES) n = ELONG_MAX_PULSES;
  return n;
}

// Per-pulse multiplier that spreads elong.ratio evenly across the burst on a
// log scale, so pulse N lands at exactly `ratio` times pulse 1. A one-pulse
// burst has no span to divide up, so it stays flat.
static double elongPerPulseFactor() {
  if (!elong.active) return 1.0;     // engine may be up purely for PHASE
  uint32_t n = elongPulseCount();
  if (n < 2) return 1.0;
  return pow(elong.ratio, 1.0 / (double)(n - 1));
}

// Rebuilds the per-burst table from the current base T1/T2 (whatever ENC2/
// ENC3 last set -- elongArm() forces SHAPE_T12 so those knobs edit exactly
// this), the derived per-pulse factor, and onCount capped at ELONG_MAX_PULSES.
// Pulse 1 is the base pulse; each pulse after that is the previous one's
// width times that factor, T1 and T2 independently. Mute duration is offCount
// "carrier-equivalent" cycles at the BASE (unelongated) period -- offCount is
// a pulse count everywhere else in this sketch, so this keeps that meaning
// even though elongation generates its own pulses rather than counting an
// input. SHOT mode parks in mute forever, same trick the normal COUNT path
// uses via off=0xFFFFFFFF.
// Builds ONE burst's table into `dst`, with every T1 scaled by `t1Scale`.
// Returns the word count written.
//
// The scale is what makes per-train amplitude work. For an inductive primary
// the current at turn-off is V*T1/L, so narrowing every pulse in a burst by the
// same fraction attenuates that whole burst without touching its shape -- the
// elongation ratio, or a loaded SEQ envelope, still does exactly what it did,
// just at a lower level. T2 is deliberately NOT scaled: keeping the spacing put
// means the burst occupies the same time on screen at every amplitude, so the
// scope framing does not move as the ring cycles.
static uint32_t elongBuildInto(uint32_t *dst, double t1Scale, int32_t chanMask,
                               bool cycleMark) {
  // chanMask >= 0 pins every pulse in this burst to one channel code -- that is
  // PHASE mode, where the code IS the output. chanMask < 0 falls back to the
  // per-pulse codes a SEQ table carries, which is the tap-select case.
  //
  // cycleMark rides in the same word (bit 4 -> GP22) rather than being pushed
  // separately: it must land on exactly the same clock edge as the channel it
  // marks, and sharing the word is the only way to guarantee that.
  const bool table  = (seqLen > 0);
  uint32_t   n      = table ? seqLen : elongPulseCount();
  double     factor = table ? 1.0    : elongPerPulseFactor();

  double t1 = (double)currentT1Ns() * t1Scale;
  double t2 = (double)currentT2Ns();
  double baseNs = (double)currentT1Ns() + t2;    // unscaled: sets the mute

  // Order MUST match the program's own pull sequence: N, the gap mask, then N
  // triples of (channel mask, hi, lo), then mute LAST -- pulled once after the
  // on-phase loop exits, not second, or pulse 1 gets fed the mute count.
  uint32_t idx = 0;
  dst[idx++] = n - 1;
  // What the OUT group holds between pulses. The channel bits are 0 there (each
  // channel carries the real train, so it has to drop in the gaps) and the
  // marker is not, which is the whole reason this is a separate word rather
  // than the `mov osr, null` it replaced.
  dst[idx++] = cycleMark ? CYCLE_MASK : 0u;

  for (uint32_t i = 0; i < n; i++) {
    double hiNs = t1, loNs = t2;
    if (table) {
      hiNs = (double)seqSteps[i].t1_ns * t1Scale;   // the ring scales SEQ too
      loNs = (double)seqSteps[i].t2_ns;
    }
    uint32_t mask = (chanMask >= 0) ? ((uint32_t)chanMask & AMP_MASK)
                  : (table ? (uint32_t)(seqSteps[i].amp & AMP_MASK) : 0u);
    if (cycleMark) mask |= CYCLE_MASK;
    uint32_t hiCyc = elongNsToCycles((uint64_t)hiNs);
    uint32_t loCyc = elongNsToCycles((uint64_t)loNs);
    if (hiCyc < 4) hiCyc = 4;
    if (loCyc < 9) loCyc = 9;      // lo grew to +9 with the channel-clear pair
    dst[idx++] = mask;
    dst[idx++] = hiCyc - 4;        // hi is still +4: nothing was added inside it
    dst[idx++] = loCyc - 9;

    if (!table) { t1 *= factor; t2 *= factor; }
  }

  uint32_t muteCyc;
  if (shotMode) {
    muteCyc = 0xFFFFFFFFUL;
  } else {
    // A loaded table has no single base period to scale offCount by, so the
    // mute is offCount times the FIRST step. That keeps offCount meaning
    // "pulses of silence" everywhere rather than quietly becoming a duration.
    if (table) baseNs = (double)seqSteps[0].t1_ns + (double)seqSteps[0].t2_ns;
    muteCyc = elongNsToCycles((uint64_t)(baseNs * (double)offCount));
    if (muteCyc < 11) muteCyc = 11;
  }
  dst[idx++] = muteCyc - 11;   // 11, not 9: the gap-mask preamble is inside the
  return idx;                  // window the mute is measured over
}

// Rebuilds everything the DMA will read: the whole ring if one is armed,
// otherwise the single table. Every caller that changes what a burst should
// look like goes through here, so there is one place that knows which mode is
// live rather than a check at each call site.
static uint32_t cfgGcd(uint32_t a, uint32_t b) { while (b) { uint32_t t = a % b; a = b; b = t; } return a; }

static void elongBuildAll() {
  const uint32_t tl = trainLen ? trainLen : 1;
  // SYNC fires every channel on every burst, so it adds no per-burst variation
  // and contributes 1 to the ring length -- only ROTATE walks the channels.
  const uint32_t pl = (phaseMode == PH_ROTATE) ? (phaseLen ? phaseLen : 1) : 1;

  if (!trainLen && !phaseOn()) {         // neither sequence armed: one table
    ringLen = 0;
    // No cycle marker: with one table every burst is identical, so there is no
    // "burst 1" to point at. Marking them all would leave GP22 stuck high --
    // the marker only ever falls when a LATER burst rewrites it low.
    elongTableLen = elongBuildInto(elongTable, 1.0, -1, false);
    return;
  }
  // Both sequences advance once per burst but at different rates, so the
  // pattern only repeats after lcm(levels, channels) bursts. Build that many
  // tables and the IRQ needs nothing but an index.
  uint32_t n = (tl / cfgGcd(tl, pl)) * pl;
  if (n > RING_MAX) n = (tl > RING_MAX) ? RING_MAX : tl;   // pathological only
  ringLen = n;
  for (uint32_t k = 0; k < n; k++) {
    const double  scale = trainLen ? (double)trainAmp[k % tl] / 100.0 : 1.0;
    int32_t mask = -1;                                   // -1 = SEQ tap codes
    if (phaseMode == PH_ROTATE)    mask = (int32_t)(1u << (k % pl));
    else if (phaseMode == PH_SYNC) mask = (int32_t)((1u << phaseLen) - 1u);
    // Only burst 1, and only when the pattern is actually longer than one burst
    // -- a 1-long ring has nothing to distinguish, and see the note above about
    // a marker that is never rewritten low.
    elongTableLen = elongBuildInto(elongRing[k], scale, mask, n > 1 && k == 0);
  }
  trainIdx = 0;
}

// Fires once per burst (when the linear transfer completes, i.e. after the
// mute-count word has been consumed) -- points the DMA at the table the NEXT
// burst should use. Without a ring that is the same table every time; with one
// it advances and wraps, which is what makes an envelope span a train of bursts
// rather than resetting at every burst boundary.
//
// Pointer arithmetic only. The tables are prebuilt, so nothing here computes a
// width, and this stays safe to run in an interrupt that fires once per burst.
static void elongDmaIrqHandler() {
  dma_hw->ints0 = 1u << elongDmaChan;
  const uint32_t *next = elongTable;
  if (ringLen) {
    uint32_t i = trainIdx + 1;
    if (i >= ringLen) i = 0;
    trainIdx = i;
    next = elongRing[i];
  }
  dma_channel_set_trans_count(elongDmaChan, elongTableLen, false);
  dma_channel_set_read_addr(elongDmaChan, next, true);
}

static void elongStart() {
  elongBuildAll();

  pio_gpio_init(pioElong, PIN_GATE_OUT);
  pio_gpio_init(pioElong, PIN_GATED_OUT);
  pio_sm_set_consecutive_pindirs(pioElong, smElong, PIN_GATE_OUT, 2, true);

  // Amplitude select and the cycle marker share one OUT group; SET and OUT are
  // independent pin groups on one SM, so this needs no extra state machine.
  // Bit 3 (ampBase+3 = GP21) is skipped on purpose -- handing that pad to PIO
  // would take it away from the mode button. Its pindir is still set here, but
  // a pindir on a pad whose function is SIO does nothing.
  for (uint i = 0; i < AMP_BITS; i++) pio_gpio_init(pioElong, ampBase + i);
  pio_gpio_init(pioElong, cyclePin());
  pio_sm_set_consecutive_pindirs(pioElong, smElong, ampBase, OUT_BITS, true);

  pio_sm_config c = pio_get_default_sm_config();
  sm_config_set_wrap(&c, offElong, offElong + elongate_program.length - 1);
  sm_config_set_set_pins(&c, PIN_GATE_OUT, 2);
  sm_config_set_out_pins(&c, ampBase, OUT_BITS);
  // Shift right so `out pins, n` takes the low n bits of the pushed word, and
  // NO autopull -- every pull in this program is explicit and ordered, and an
  // autopull firing between them would desynchronise the whole table.
  sm_config_set_out_shift(&c, true, false, 32);
  // ISR holds the gap mask for a whole burst, so autopush must stay off -- it
  // is a scratch register here, not an input path, and nothing ever reads the
  // RX FIFO. (MOV ISR does not trigger autopush anyway, which only fires after
  // an IN; this is belt-and-braces against a future IN being added.)
  sm_config_set_in_shift(&c, true, false, 32);
  sm_config_set_clkdiv_int_frac(&c, 1, 0);

  pio_sm_init(pioElong, smElong, offElong, &c);
  pio_sm_set_enabled(pioElong, smElong, true);

  dma_channel_config dc = dma_channel_get_default_config(elongDmaChan);
  channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
  channel_config_set_read_increment(&dc, true);
  channel_config_set_write_increment(&dc, false);
  channel_config_set_dreq(&dc, pio_get_dreq(pioElong, smElong, true));
  dma_channel_configure(elongDmaChan, &dc, &pioElong->txf[smElong],
                        ringLen ? elongRing[0] : elongTable,
                        elongTableLen, true);               // trigger now

  // Same reasoning as the normal path: gpio_set_function() inside
  // pio_gpio_init() above just wiped this, so it must be set after.
  gpio_set_outover(PIN_GATED_OUT,
                   pulseInvert ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
}

// Arms elongation: forces the preconditions it needs (COUNT+INT) and the
// carrier-edit mode that makes ENC2/ENC3 mean "base T1/T2", then lets
// applyGate() notice elong.active and route to elongStart(). Re-arming
// (button pressed again, or a param changed while already active) just
// rebuilds the table with whatever's current -- there is no separate "was it
// ever configured" state to track, unlike the old ramp button.
// Preconditions the elongation state machine needs, without asserting that you
// want elongation SHAPING. PHASE uses this: it needs the engine, not the ratio.
static void phaseArm() {
  if (!elongPioReady) return;
  shapeMode = SHAPE_T12;
  sigSource = SRC_INT;
  gateMode  = MODE_COUNT;
  applyGate();
}

static void elongArm() {
  if (!elongPioReady) return;
  elong.active = true;
  shapeMode     = SHAPE_T12;
  sigSource     = SRC_INT;
  gateMode      = MODE_COUNT;
  applyGate();
}

// Reverts to a flat train of the base pulses -- i.e. exactly the normal
// carrier+gate path, since that's what already runs whenever elong.active is
// false. No separate "flat mode" to implement.
static void elongDisarm() {
  elong.active = false;
  applyGate();          // PHASE keeps the engine up; the train just goes flat
}

// ---- T1/T2 sweep ramp (RAMP1/RAMP2) ----------------------------------------
// Steps the carrier's own T1/T2 between BURSTS over many seconds -- a
// resonance sweep, distinct from the per-pulse elongation above which
// reshapes pulses WITHIN one burst. Kept under its original name/commands.

// carrierPeriodNs * dutyPpm maxes out around 1e9 * 1e6 = 1e15, comfortably
// inside uint64_t (~1.8e19) -- plain 64-bit math, no overflow risk.
static uint64_t currentT1Ns() { return (carrierPeriodNs * (uint64_t)carrierDutyPpm) / 1000000ULL; }
static uint64_t currentT2Ns() { return carrierPeriodNs - currentT1Ns(); }

static void applyT1T2(uint64_t t1Ns, uint64_t t2Ns) {
  uint64_t period = t1Ns + t2Ns;
  if (period < CARRIER_NS_MIN) period = CARRIER_NS_MIN;
  if (period > CARRIER_NS_MAX) period = CARRIER_NS_MAX;
  carrierPeriodNs = period;
  uint32_t ppm = (uint32_t)((t1Ns * 1000000ULL) / period);
  if (ppm < DUTY_PPM_MIN) ppm = DUTY_PPM_MIN;
  if (ppm > DUTY_PPM_MAX) ppm = DUTY_PPM_MAX;
  carrierDutyPpm = ppm;
  applyGate();
}

// How long one full burst takes, in ns -- the unit "every N bursts" counts in.
// COUNT mode computes it from the carrier directly (integer, exact, no float
// round-trip); TIME mode's burst period is the gate generator's own, set by a
// separate PIO SM and independent of the carrier's own mark/space.
static uint64_t burstPeriodNs() {
  if (gateMode == MODE_COUNT) {
    uint64_t total = (uint64_t)onCount + offCount;
    return total * carrierPeriodNs;
  }
  return gatePeriodNs;
}

// Multiply before dividing: at a fast carrier (period in the hundreds of ns)
// with a large bursts-per-step, dividing burstPeriodNs by 1000 first can floor
// to zero and silently step every poll instead of waiting the intended count.
static uint64_t rampIntervalUs(uint32_t burstsPerStep) {
  uint64_t bp = burstPeriodNs();
  if (!bp) return 100000ULL;            // no burst rate yet -- fall back to 100 ms
  uint64_t us = (bp * (uint64_t)burstsPerStep) / 1000ULL;
  return us ? us : 1ULL;
}

// Arms a ramp with an explicit configuration -- shared by the RAMP1/RAMP2
// command (fresh values typed in) and the elongation pushbutton (re-arming
// with whatever was configured last). Capturing startNs from the CURRENT
// T1/T2 here, not from whatever the caller remembers, is what makes a wrap
// restart from wherever the value actually sits instead of drifting from a
// stale snapshot.
static void armRamp(RampCfg &r, bool isT1, int64_t stepNs, uint32_t bursts,
                    uint64_t limitNs, bool wrap) {
  sigSource = SRC_INT;         // ramping the carrier implies SRC INT, same as C/CD
  uint64_t t1 = currentT1Ns(), t2 = currentT2Ns();
  r.startNs       = isT1 ? t1 : t2;
  r.stepNs        = stepNs;
  r.burstsPerStep = bursts;
  r.limitNs       = limitNs;
  r.wrap          = wrap;
  r.active        = true;
  r.nextStepAtUs  = time_us_64() + rampIntervalUs(bursts);
}

static void rampStep(RampCfg &r, bool isT1) {
  uint64_t t1 = currentT1Ns(), t2 = currentT2Ns();
  uint64_t &target = isT1 ? t1 : t2;
  int64_t next = (int64_t)target + r.stepNs;
  bool reachedLimit = (r.stepNs >= 0) ? (next >= (int64_t)r.limitNs)
                                      : (next <= (int64_t)r.limitNs);
  if (reachedLimit) {
    if (r.wrap) target = r.startNs;
    else { target = r.limitNs; r.active = false; }   // hold at the limit; done
  } else {
    target = (uint64_t)next;
  }
  applyT1T2(t1, t2);
}

// Called from loop(). Nothing here is timing-critical -- a step landing a
// fraction late just makes that step's dwell one poll-cycle longer, which is
// invisible against a "every N bursts" cadence.
static void pollRamp() {
  uint64_t now = time_us_64();
  if (ramp1.active && (int64_t)(now - ramp1.nextStepAtUs) >= 0) {
    rampStep(ramp1, true);
    ramp1.nextStepAtUs = now + rampIntervalUs(ramp1.burstsPerStep);
  }
  if (ramp2.active && (int64_t)(now - ramp2.nextStepAtUs) >= 0) {
    rampStep(ramp2, false);
    ramp2.nextStepAtUs = now + rampIntervalUs(ramp2.burstsPerStep);
  }
}

// ============================================================================
// Input frequency counter (PWM slice, hardware)
// ============================================================================
//
// PWM channel B can clock its slice from rising edges on its pin instead of
// from sys_clk, making the slice a free 16-bit pulse counter. The CPU only
// reads a register occasionally — no per-edge interrupt, which is what made
// this awkward on the AVR. GPIO inputs reach every peripheral at once
// regardless of FUNCSEL, so PIO still sees GP3 while the pad is set to PWM.

static uint     freqSlice    = 0;
static uint16_t freqLast     = 0;
static uint32_t freqAccum    = 0;
static uint32_t freqWinStart = 0;

static void freqCounterInit() {
  gpio_set_function(PIN_SIG_IN, GPIO_FUNC_PWM);
  freqSlice = pwm_gpio_to_slice_num(PIN_SIG_IN);

  pwm_config cfg = pwm_get_default_config();
  pwm_config_set_clkdiv_mode(&cfg, PWM_DIV_B_RISING);
  pwm_config_set_clkdiv(&cfg, 1.0f);
  pwm_config_set_wrap(&cfg, 65535);
  pwm_init(freqSlice, &cfg, true);

  freqLast     = (uint16_t)pwm_get_counter(freqSlice);
  freqWinStart = millis();
}

// Sampled every 50 ms so the 16-bit counter cannot wrap below 1.31 MHz, then
// averaged over 500 ms for ~2 Hz resolution.
static void freqPoll() {
  static uint32_t lastSample = 0;
  uint32_t now = millis();
  if (now - lastSample < 50) return;
  lastSample = now;

  uint16_t c = (uint16_t)pwm_get_counter(freqSlice);
  freqAccum += (uint16_t)(c - freqLast);
  freqLast = c;

  uint32_t win = now - freqWinStart;
  if (win >= 500) {
    measuredHz   = (float)freqAccum * 1000.0f / (float)win;
    freqAccum    = 0;
    freqWinStart = now;
  }
}

// ============================================================================
// Readout — everything writes to a Print so serial and HTTP share one path
// ============================================================================

// Accumulates Print output into a String for the HTTP handlers.
class StringPrint : public Print {
public:
  String s;
  size_t write(uint8_t c) override { s += (char)c; return 1; }
  size_t write(const uint8_t *b, size_t n) override {
    s.concat((const char *)b, n); return n;
  }
};

static void printU64(Print &o, uint64_t v) {
  char b[24];
  snprintf(b, sizeof(b), "%llu", (unsigned long long)v);
  o.print(b);
}

static void printState(Print &o) {
  uint64_t fsys = clock_get_hz(clk_sys);

  o.println("=== GATED PULSE (Pico 2 W / PIO) ===");
  o.print  ("mode      : ");
  o.println(gateMode == MODE_COUNT ? "COUNT (PIO counts input pulses)"
                                   : "TIME  (PIO gate generator)");
  o.print  ("output    : ");
  o.print  (outputEnabled ? "enabled" : "DISABLED");
  if (interlockTripped) o.print(" (INTERLOCK OPEN - nothing driven)");
  o.print  ("   gate: ");
  o.print  (gateInvert ? "inverted" : "normal");
  o.print  ("   pulses: ");
  o.println(pulseInvert ? "active-low" : "active-high");
  o.print  ("editing   : ");
  if (gateMode == MODE_COUNT) o.println(selParam ? "OFF pulses" : "ON pulses");
  else                        o.println(selParam ? "DUTY"       : "PERIOD");
  o.print  ("carrier ctl: ");
  o.print  (shapeMode == SHAPE_T12 ? "T1/T2 independent" : "freq/duty");
  o.print  ("   elongation: ");
  if (elong.active) {
    o.print("ON, ratio "); o.print(elong.ratio, 4);
    o.print("x over "); o.print(elongPulseCount()); o.print(" pulses (");
    o.print(elongPerPulseFactor(), 4); o.println("/pulse)");
  } else {
    o.println("off");
  }
  o.print  ("   sweep    : ");
  o.println((ramp1.active || ramp2.active) ? "ON" : "off");

  o.print("source    : ");
  if (sigSource == SRC_INT) {
    o.print("INTERNAL carrier on GP2 — ");
    o.print(sourceHz(), 3); o.print(" Hz, ");
    o.print((double)carrierDutyPpm / 10000.0, 3); o.println(" % duty");
    o.print("  PIO car : clkdiv "); o.print(achCarDiv);
    o.print(" hi="); o.print(achCarHi);
    o.print(" lo="); o.print(achCarLo);
    o.print("  (asked "); o.print(1e9 / (double)carrierPeriodNs, 3);
    o.println(" Hz)");
    if (ramp1.active) {
      o.print("  T1 ramp: now "); o.print(currentT1Ns() / 1000.0, 3);
      o.print(" us, step "); o.print(ramp1.stepNs / 1000.0, 3);
      o.print(" us / "); o.print(ramp1.burstsPerStep); o.print(" burst(s), limit ");
      o.print(ramp1.limitNs / 1000.0, 3);
      o.println(ramp1.wrap ? " us, WRAP" : " us, STOP");
    }
    if (ramp2.active) {
      o.print("  T2 ramp: now "); o.print(currentT2Ns() / 1000.0, 3);
      o.print(" us, step "); o.print(ramp2.stepNs / 1000.0, 3);
      o.print(" us / "); o.print(ramp2.burstsPerStep); o.print(" burst(s), limit ");
      o.print(ramp2.limitNs / 1000.0, 3);
      o.println(ramp2.wrap ? " us, WRAP" : " us, STOP");
    }
  } else {
    o.println("EXTERNAL sig gen on GP3");
  }

  if (gateMode == MODE_COUNT) {
    uint64_t total = (uint64_t)onCount + offCount;
    double   shz   = sourceHz();
    o.print("ON  pulses: "); o.print(onCount);
    if (effectiveBurstPulses() != onCount) {
      o.print("  ** only "); o.print(effectiveBurstPulses());
      o.print(" emitted -- the channel/elongation engine streams a table capped at ");
      o.print(ELONG_MAX_PULSES); o.print(" pulses");
    }
    o.println();
    o.print("OFF pulses: "); o.println(offCount);
    o.print("period    : "); printU64(o, total); o.println(" input pulses");
    o.print("duty      : ");
    o.print(100.0 * (double)onCount / (double)total, 4); o.println(" %");
    if (shz > 0) {
      o.print("burst rate: "); o.print(shz / (double)total, 4); o.println(" Hz");
      o.print("burst len : "); o.print((double)onCount * 1e6 / shz, 3); o.println(" us");
    }
  } else {
    double perUs = (double)gatePeriodNs / 1000.0;
    double duty  = (double)gateDutyPpm / 10000.0;
    double shz   = sourceHz();
    o.print("period    : "); o.print(perUs, 3);
    o.print(" us  ("); o.print(1e9 / (double)gatePeriodNs, 4); o.println(" Hz)");
    o.print("duty      : "); o.print(duty, 4); o.println(" %");
    o.print("open width: "); o.print(perUs * duty / 100.0, 3); o.println(" us");

    double tickNs = 1e9 * (double)achGenDiv / (double)fsys;
    double achPer = (double)(achHiCyc + achLoCyc) * tickNs;
    o.print("PIO gate  : clkdiv "); o.print(achGenDiv);
    o.print("  hi="); o.print(achHiCyc);
    o.print("  lo="); o.print(achLoCyc);
    o.print("  tick="); o.print(tickNs, 3); o.println(" ns");
    o.print("achieved  : "); o.print(achPer / 1000.0, 4);
    o.print(" us period, ");
    o.print(100.0 * achHiCyc / (double)(achHiCyc + achLoCyc), 4);
    o.println(" % duty");
    if (shz > 0) {
      o.print("pulses/brst: ~"); o.println(perUs * duty / 100.0 * shz / 1e6, 2);
    }
  }

  o.print("input freq: ");
  if (sigSource == SRC_INT) { o.print(sourceHz(), 3); o.println(" Hz (internal, exact)"); }
  else if (measuredHz > 0)  { o.print(measuredHz, 1); o.println(" Hz (hardware counter)"); }
  else                      { o.println("no input detected on GP3"); }

  o.print("pio       : gating pio"); o.print(pio_get_index(pioGate));
  o.print(" sm"); o.print(smGate); o.print(" @"); o.print(offGating);
  o.print(",  gen pio"); o.print(pio_get_index(pioGen));
  o.print(" sm"); o.print(smGen); o.print("/sm"); o.print(smCarrier);
  o.print(" @"); o.println(offGen);
  o.print("sys clock : "); printU64(o, fsys / 1000000); o.println(" MHz");
  o.print("wifi      : ");
  if (WiFi.status() == WL_CONNECTED) {
    o.print(WiFi.SSID()); o.print("  http://"); o.print(WiFi.localIP());
    o.print("/  ("); o.print(MDNS_NAME); o.println(".local)");
  } else {
    o.println(strlen(WIFI_SSID) ? "not connected" : "disabled (no secrets.h)");
  }
}

// True when the elongation state machine -- not the normal carrier+gate pair --
// is the thing generating pulses. Mirrors the routing test in applyGate().
// It matters for REPORTING because that engine streams a prebuilt table capped
// at ELONG_MAX_PULSES, so a longer burst is silently shortened.
static inline bool elongEngineActive() {
  return (elong.active || phaseOn()) && elongPioReady
      && sigSource == SRC_INT && gateMode == MODE_COUNT;
}

// The burst length actually emitted, which is NOT always the one you asked
// for. Reporting onCount unconditionally meant `ON 40` in a phase mode showed
// 40 everywhere while the hardware emitted 25 -- an instrument disagreeing
// with itself, which is worse than one with a limit.
static inline uint32_t effectiveBurstPulses() {
  if (seqLen > 0 && elongEngineActive()) return seqLen;
  return elongEngineActive() ? elongPulseCount() : onCount;
}

// ---- Machine-readable status ---------------------------------------------
// One line of JSON so host tooling never has to scrape the human readout.
static void printJson(Print &o) {
  double   shz   = sourceHz();
  uint64_t total = (uint64_t)onCount + offCount;

  o.print("{\"ok\":1");
  o.print(",\"mode\":\"");  o.print(gateMode == MODE_COUNT ? "COUNT" : "TIME");
  o.print("\",\"src\":\""); o.print(sigSource == SRC_INT ? "INT" : "EXT");
  o.print("\",\"enabled\":");    o.print(outputEnabled ? 1 : 0);
  // "enabled" stays the operator's intent; "live" is whether anything is
  // actually coming out. Host tooling that arms a scope wants live.
  o.print(",\"live\":");         o.print(outputLive() ? 1 : 0);
  o.print(",\"interlock\":");    o.print(interlockEnabled ? 1 : 0);
  o.print(",\"interlock_closed\":"); o.print(interlockClosed() ? 1 : 0);
  o.print(",\"interlock_tripped\":"); o.print(interlockTripped ? 1 : 0);
  o.print(",\"interlock_trips\":");   o.print(interlockTrips);
  o.print(",\"interlock_pin\":");     o.print(PIN_INTERLOCK);
  o.print(",\"gate_invert\":");  o.print(gateInvert ? 1 : 0);
  o.print(",\"pulse_invert\":"); o.print(pulseInvert ? 1 : 0);
  o.print(",\"editing\":\"");
  if (gateMode == MODE_COUNT) o.print(selParam ? "OFF" : "ON");
  else                        o.print(selParam ? "DUTY" : "PERIOD");

  o.print("\",\"shape\":\""); o.print(shapeMode == SHAPE_T12 ? "T12" : "FD");
  o.print("\",\"elongation\":"); o.print(elong.active ? 1 : 0);
  o.print(",\"elong_ratio\":"); o.print(elong.ratio, 4);
  // Still published, and still means exactly what it always did -- the
  // per-pulse multiplier. It is now derived rather than stored, so the
  // consumers that mirror elongBuildInto() (the MCP server's predicted
  // waveform, the dashboard readout) keep composing without knowing the knob
  // moved up a level.
  o.print(",\"elong_factor\":"); o.print(elongPerPulseFactor(), 6);
  o.print(",\"elong_pulses\":"); o.print(elongPulseCount());
  o.print(",\"sweep\":"); o.print((ramp1.active || ramp2.active) ? 1 : 0);
  // The loaded envelope, if any. Emitted in full rather than as a length:
  // every consumer that predicts the waveform (the MCP server's expected
  // trace, the dashboard chart) needs the actual widths, and a table is the
  // one case where they cannot be derived from ratio + count.
  // Whether the elongation state machine actually got claimed. ELONGATE, SEQ
  // and TRAIN all silently do nothing without it, and "nothing happened" is a
  // miserable thing to debug from the outside.
  o.print(",\"elong_pio\":"); o.print(elongPioReady ? 1 : 0);
  o.print(",\"autosave\":"); o.print(cfgAutosave ? 1 : 0);
  o.print(",\"boot_mode\":\"");
  o.print(cfgBootMode == BOOT_RUN ? "RUN" : cfgBootMode == BOOT_OFF ? "OFF" : "SAVED");
  o.print("\",\"saved\":"); o.print(cfgLastCrc ? 1 : 0);
  o.print(",\"phase_len\":"); o.print(phaseLen);
  o.print(",\"phase_mode\":\"");
  o.print(phaseMode == PH_SYNC ? "SYNC" : phaseMode == PH_ROTATE ? "ROTATE" : "OFF");
  o.print("\"");
  o.print(",\"ring_len\":"); o.print(ringLen);
  o.print(",\"cycle_pin\":"); o.print(cyclePin());
  o.print(",\"cycle_on\":");  o.print(ringLen > 1 ? 1 : 0);
  o.print(",\"train_len\":"); o.print(trainLen);
  if (trainLen) {
    o.print(",\"train\":[");
    for (uint32_t i = 0; i < trainLen; i++) {
      if (i) o.print(",");
      o.print(trainAmp[i]);
    }
    o.print("]");
  }
  o.print(",\"seq_len\":"); o.print(seqLen);
  if (seqLen) {
    o.print(",\"seq\":[");
    for (uint32_t i = 0; i < seqLen; i++) {
      if (i) o.print(",");
      o.print("{\"t1_ns\":"); o.print(seqSteps[i].t1_ns);
      o.print(",\"t2_ns\":"); o.print(seqSteps[i].t2_ns);
      o.print(",\"amp\":");   o.print(seqSteps[i].amp);
      o.print("}");
    }
    o.print("]");
  }

  o.print(",\"on\":");  o.print(onCount);
  o.print(",\"off\":");   o.print(offCount);
  o.print(",\"period_ns\":");           printU64(o, gatePeriodNs);
  o.print(",\"duty_ppm\":");            o.print(gateDutyPpm);
  o.print(",\"carrier_ns\":");          printU64(o, carrierPeriodNs);
  o.print(",\"carrier_duty_ppm\":");    o.print(carrierDutyPpm);

  o.print(",\"source_hz\":");   o.print(shz, 4);
  o.print(",\"measured_hz\":"); o.print(measuredHz, 2);
  o.print(",\"sys_hz\":");      printU64(o, clock_get_hz(clk_sys));

  o.print(",\"gate_div\":"); o.print(achGenDiv);
  o.print(",\"gate_hi\":");  o.print(achHiCyc);
  o.print(",\"gate_lo\":");  o.print(achLoCyc);
  o.print(",\"car_div\":");  o.print(achCarDiv);
  o.print(",\"car_hi\":");   o.print(achCarHi);
  o.print(",\"car_lo\":");   o.print(achCarLo);

  // Derived, so callers do not duplicate the maths.
  if (gateMode == MODE_COUNT) {
    o.print(",\"burst_pulses\":"); o.print(effectiveBurstPulses());
    if (effectiveBurstPulses() != onCount) {
      o.print(",\"burst_clamped\":1,\"burst_requested\":"); o.print(onCount);
    }
    if (shz > 0) {
      o.print(",\"burst_hz\":"); o.print(shz / (double)total, 6);
      o.print(",\"burst_us\":"); o.print((double)onCount * 1e6 / shz, 4);
    }
  } else {
    double openUs = (double)gatePeriodNs / 1000.0 * (double)gateDutyPpm / 1e6;
    o.print(",\"burst_hz\":"); o.print(1e9 / (double)gatePeriodNs, 6);
    o.print(",\"burst_us\":"); o.print(openUs, 4);
    if (shz > 0) { o.print(",\"burst_pulses\":"); o.print(openUs * shz / 1e6, 2); }
  }

  o.print(",\"ramp1_active\":"); o.print(ramp1.active ? 1 : 0);
  if (ramp1.active) {
    o.print(",\"ramp1_t1_ns\":");    printU64(o, currentT1Ns());
    o.print(",\"ramp1_step_ns\":");  o.print((double)ramp1.stepNs, 0);
    o.print(",\"ramp1_bursts\":");   o.print(ramp1.burstsPerStep);
    o.print(",\"ramp1_limit_ns\":"); printU64(o, ramp1.limitNs);
    o.print(",\"ramp1_wrap\":");     o.print(ramp1.wrap ? 1 : 0);
  }
  o.print(",\"ramp2_active\":"); o.print(ramp2.active ? 1 : 0);
  if (ramp2.active) {
    o.print(",\"ramp2_t2_ns\":");    printU64(o, currentT2Ns());
    o.print(",\"ramp2_step_ns\":");  o.print((double)ramp2.stepNs, 0);
    o.print(",\"ramp2_bursts\":");   o.print(ramp2.burstsPerStep);
    o.print(",\"ramp2_limit_ns\":"); printU64(o, ramp2.limitNs);
    o.print(",\"ramp2_wrap\":");     o.print(ramp2.wrap ? 1 : 0);
  }

  o.print(",\"shot\":");       o.print(shotMode ? 1 : 0);
  o.print(",\"running\":");    o.print(runUntilMs ? 1 : 0);
  o.print(",\"run_len_ms\":");  o.print(runLenMs);
  o.print(",\"run_left_ms\":");
  o.print(runUntilMs ? (int32_t)(runUntilMs - millis()) : 0);
  o.print(",\"wifi\":");
  if (WiFi.status() == WL_CONNECTED) {
    o.print("\""); o.print(WiFi.localIP()); o.print("\"");
  } else {
    o.print("null");
  }
  o.println("}");
}

// ---- Bring-up diagnostic --------------------------------------------------
// Answers the questions you actually have when nothing comes out: is there a
// signal on the pin, is the PWM counter seeing it, and is the gating state
// machine running or parked on a wait?
// Percentage of a 200 ms window a pin spends high. One gpio_get() cannot tell
// "low at this instant" from "dead", which is exactly the ambiguity that costs
// an hour at the bench: a channel that is rotating reads its share, one that is
// stuck reads 0 or 100.
//
// The window is a whole number of patterns only by luck, so the figure is
// quantised by however many bursts fall inside it -- at 10 kHz / ON 10 / OFF 90
// a 50 ms window read a true 33% as either 20% or 40%. 200 ms keeps that error
// small enough to not mislead while staying an interactive command's worth of
// blocking (four pins, so ~0.8 s).
static void printPinDuty(Print &o, const char *label, uint pin) {
  uint32_t hi = 0, n = 0;
  uint32_t t0 = micros();
  while ((uint32_t)(micros() - t0) < 200000) { if (gpio_get(pin)) hi++; n++; }
  o.print(label); o.print(" GP"); o.print(pin); o.print(" : ");
  o.print(n ? (100.0 * hi) / n : 0.0, 1); o.println("% of 200ms high");
}

static void printDiag(Print &o) {
  const uint inPin = (sigSource == SRC_INT) ? PIN_CARRIER : PIN_SIG_IN;

  uint32_t edges = 0, hi = 0, samples = 0;
  bool prev = gpio_get(inPin);
  uint32_t t0 = micros();
  while ((uint32_t)(micros() - t0) < 5000) {
    bool c = gpio_get(inPin);
    if (c && !prev) edges++;
    if (c) hi++;
    samples++;
    prev = c;
  }
  uint32_t elapsed = micros() - t0;

  uint16_t p0 = (uint16_t)pwm_get_counter(freqSlice);
  uint32_t pc0 = pio_sm_get_pc(pioGate, smGate);
  delayMicroseconds(5000);
  uint16_t pwmDelta = (uint16_t)((uint16_t)pwm_get_counter(freqSlice) - p0);
  uint32_t pc1 = pio_sm_get_pc(pioGate, smGate);

  o.println("=== DIAG ===");
  o.print("input pin      : GP"); o.print(inPin);
  o.println(sigSource == SRC_INT ? "  (internal carrier)" : "  (external)");
  o.print("  CPU-sampled  : "); o.print(edges);
  o.print(" rising edges in "); o.print(elapsed); o.print(" us  -> ~");
  o.print(elapsed ? (double)edges * 1e6 / elapsed : 0.0, 1); o.println(" Hz");
  o.print("  duty seen    : ");
  o.print(samples ? 100.0 * hi / samples : 0.0, 1); o.println(" %");
  o.print("  PWM counter  : "); o.print(pwmDelta);
  o.println(" edges in 5000 us on GP3");
  o.println("  (PWM should track GP3 even though PIO also reads it — RP2350");
  o.println("   routes each pad's input to every peripheral at once.)");

  o.print("gating SM pc   : "); o.print(pc0); o.print(" then "); o.print(pc1);
  o.print("   (program base "); o.print(offGating);
  o.print(", COUNT entry +"); o.print(GATING_ENTRY_COUNT);
  o.print(", TIME entry +");  o.print(GATING_ENTRY_TIME); o.println(")");
  o.println("  A pc that never moves means the SM is parked on a wait — i.e. it");
  o.println("  is not seeing edges on its input pin.");
  o.print("gate pin GP4   : "); o.println(gpio_get(PIN_GATE_OUT) ? "high" : "low");
  o.print("out  pin GP5   : "); o.println(gpio_get(PIN_GATED_OUT) ? "high" : "low");
  printPinDuty(o, "chan  ", ampBase);
  printPinDuty(o, "chan  ", ampBase + 1);
  printPinDuty(o, "chan  ", ampBase + 2);
  printPinDuty(o, "cycle ", cyclePin());
}

static void printHelp(Print &o) {
  o.println("=== COMMANDS (case-insensitive) ===");
  o.println("  P              print state");
  o.println("  J              state as one line of JSON");
  o.println("  DIAG           bring-up diagnostic");
  o.println("  INPUTS         live front-panel pin levels (wiring check)");
  o.println("  ON <n>         COUNT mode: pulses passed per burst");
  o.println("  OFF <n>        COUNT mode: pulses muted per burst");
  o.println("  T <us>         TIME mode: gate period, microseconds");
  o.println("  G <Hz>         TIME mode: gate frequency");
  o.println("  D <pct>        TIME mode: gate duty, percent");
  o.println("  W <us>         TIME mode: gate open width -> sets duty");
  o.println("  SRC EXT|INT    gate the GP3 sig gen, or the internal carrier");
  o.println("  C <Hz>         internal carrier frequency (implies SRC INT)");
  o.println("  CD <pct>       internal carrier duty");
  o.println("  RAMP1 <step_us> <bursts> <limit_us> [STOP|WRAP]");
  o.println("                 auto-step carrier T1 (width) every N bursts");
  o.println("  RAMP2 ...      same, for T2 (space)");
  o.println("  RAMP1|2 OFF    cancel that ramp, holding its current value");
  o.println("  ELONGATE <r>   COUNT+INT: total span across the burst, last/first");
  o.println("                 (>1 grows, <1 shrinks); pulse 1 = current T1/T2 base");
  o.println("  ELONGATE PHI   golden-ratio growth per pulse at the current count");
  o.println("  ELONGATE OFF   flat train of the base pulses (needs COUNT+INT)");
  o.println("  SEQ <steps>    arbitrary per-pulse envelope, replaces the ratio:");
  o.println("                 each step is t1_us,t2_us[,amp], space separated");
  o.println("                 e.g. SEQ 1,1,0 2,1,1 4,1,2   (amp 0-7 -> GP18-20)");
  o.println("  SEQ            print the loaded table");
  o.println("  SEQ OFF        drop the table, back to ELONGATE's ratio");
  o.println("  TRAIN <levels> per-BURST amplitude ring, percent of base T1:");
  o.println("                 e.g. TRAIN 100 75 50 25  -> 4 bursts, then repeat");
  o.println("                 composes with ELONGATE/SEQ -- it scales, they shape");
  o.println("  TRAIN          print the ring");
  o.println("  TRAIN OFF      every burst identical again");
  o.println("  PHASE ROT <n>  rotate across n pins, one burst each");
  o.println("  PHASE SYNC <n> all n pins fire together every burst");
  o.println("  PHASE OFF      single output on GP5");
  o.println("  SHAPE FD|T12   carrier knobs edit freq/duty, or T1/T2 independently");
  o.println("  ENCLOG <0|1>   live-print every encoder edge/step to serial (debug)");
  o.println("  M <0|1>        mode 0=TIME 1=COUNT (until GP10 is flipped)");
  o.println("  E <0|1>        output disable/enable");
  o.println("  I              invert gate (swap open/closed phases)");
  o.println("  V              invert output pulse polarity (pad inverter)");
  o.println("  RUN <ms>       silent -> output for <ms> -> silent (one shot)");
  o.println("  SHOT           fire exactly ONE burst, then stop (COUNT mode)");
  o.println("  STOP           end a RUN / disable output now");
  o.println("  LOCK <0|1>     HV enclosure interlock on GP26 (bare = report)");
  o.println("  ARM            clear a tripped interlock (needs GP26 closed)");
  o.println("  R              restore defaults");
  o.println("  SAVE           store the current state in flash");
  o.println("  REBOOT         restart the board");
  o.println("  LOAD           restore it        FORGET  erase it");
  o.println("  AUTOSAVE 0|1   auto-store ~5 s after the last change (default on)");
  o.println("  BOOT RUN|OFF|SAVED   what the output does at power-on");
  o.println("  ?              this help");
  o.println("Input frequency is measured continuously in hardware.");
  o.println("HTTP: GET /state, GET /cmd?c=<command>, GET / for the web UI.");
}

static void printPhase(Print &o) {
  if (!phaseOn()) { o.println("PHASE: off -- single output on GP5"); return; }
  o.print("PHASE ");
  o.print(phaseMode == PH_SYNC ? "SYNC " : "ROTATE ");
  o.print(phaseLen); o.print(" channels on GP"); o.print(ampBase);
  if (phaseLen > 1) { o.print("-GP"); o.print(ampBase + phaseLen - 1); }
  o.println(phaseMode == PH_SYNC ? " -- all together every burst"
                                 : " -- one burst each, rotating");
  if (ringLen > 1) {
    o.print("  pattern repeats every "); o.print(ringLen); o.println(" bursts");
    o.print("  cycle marker on GP"); o.print(cyclePin());
    o.println(" -- high for burst 1 only, trigger on it");
  } else {
    o.print("  cycle marker GP"); o.print(cyclePin());
    o.println(" idle -- pattern is one burst, nothing to mark");
  }
  if (!elong.active) o.println("  (elongation off -- flat pulses)");
}

// ============================================================================
// Command interpreter — shared by serial and HTTP
// ============================================================================

static void runCommand(char *s, Print &o) {
  while (*s == ' ' || *s == '\t') s++;
  for (char *p = s; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;

  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
  if (!n) return;

  // Split the verb from its argument first, so a trailing space or extra
  // spacing still parses and the aliases below can repoint s at a literal.
  char *arg = s;
  while (*arg && *arg != ' ' && *arg != '\t') arg++;
  while (*arg == ' ' || *arg == '\t') *arg++ = 0;
  bool   hasArg = (*arg != 0);
  double val    = hasArg ? atof(arg) : 0.0;

  // PulseCounter-era names, kept working.
  if      (!strcmp(s, "SET_ON_COUNT"))  s = (char *)"ON";
  else if (!strcmp(s, "SET_OFF_COUNT")) s = (char *)"OFF";
  else if (!strcmp(s, "SET_MODE"))      s = (char *)"M";
  else if (!strcmp(s, "SET_ENABLE"))    s = (char *)"E";
  else if (!strcmp(s, "GET_STATUS") ||
           !strcmp(s, "STATUS"))        s = (char *)"P";
  else if (!strcmp(s, "HELP"))          s = (char *)"?";

  if (!strcmp(s, "?"))    { printHelp(o);  return; }
  if (!strcmp(s, "P"))    { printState(o); return; }
  if (!strcmp(s, "J"))    { printJson(o);  return; }
  if (!strcmp(s, "DIAG")) { printDiag(o);  return; }
  if (!strcmp(s, "INPUTS")) { printInputs(o); return; }

  if (!strcmp(s, "SRC")) {
    if      (hasArg && (!strcmp(arg, "INT") || !strcmp(arg, "1"))) sigSource = SRC_INT;
    else if (hasArg && (!strcmp(arg, "EXT") || !strcmp(arg, "0"))) {
      sigSource = SRC_EXT;
      elong.active = false;   // elongation needs INT -- an external signal has no table to follow
    }
    else { o.println("ERROR: SRC EXT or SRC INT"); return; }
    applyGate(); printState(o); return;
  }

  // ---- persistence -------------------------------------------------------
  if (!strcmp(s, "SAVE")) { cfgWrite(o); return; }

  // Restart the board. Useful on a headless standalone box, and the only way to
  // check what it actually comes up doing without reaching for the USB lead.
  if (!strcmp(s, "REBOOT")) {
    o.println("rebooting...");
    o.flush();
    delay(120);              // let the reply reach serial/HTTP before we go
    rp2040.reboot();
    return;
  }

  if (!strcmp(s, "LOAD")) {
    if (!cfgRead(true)) { o.println("no valid saved settings"); return; }
    applyGate();
    cfgDirtyAt = 0;                        // do not re-save what we just read
    o.println("settings restored");
    printState(o);
    return;
  }

  if (!strcmp(s, "FORGET")) {
    // Zero the magic rather than erasing artfully: the next cfgRead() rejects
    // it and boots to defaults, which is the whole meaning of "forget".
    EEPROM.begin(CFG_EE_SIZE);
    memset(EEPROM.getDataPtr(), 0, CFG_EE_SIZE);
    const bool ok = EEPROM.commit();
    EEPROM.end();
    cfgLastCrc = 0;
    o.println(ok ? "saved settings erased -- next boot uses defaults"
                 : "ERROR: erase failed");
    return;
  }

  if (!strcmp(s, "AUTOSAVE")) {
    if (hasArg) cfgAutosave = (val != 0.0);
    o.print("autosave "); o.println(cfgAutosave ? "on" : "off");
    if (cfgAutosave)
      o.println("  writes once, ~5 s after the last change, only if it differs");
    return;
  }

  // What the output enable does at power-on. RUN is the standalone case: a box
  // in a rig that should come straight back up pulsing after a power blip.
  if (!strcmp(s, "BOOT")) {
    if (hasArg) {
      if      (!strcmp(arg, "RUN"))   cfgBootMode = BOOT_RUN;
      else if (!strcmp(arg, "OFF"))   cfgBootMode = BOOT_OFF;
      else if (!strcmp(arg, "SAVED")) cfgBootMode = BOOT_SAVED;
      else { o.println("ERROR: BOOT RUN | OFF | SAVED"); return; }
      cfgTouch();
    }
    o.print("boot mode: ");
    o.println(cfgBootMode == BOOT_RUN ? "RUN (always start pulsing)"
            : cfgBootMode == BOOT_OFF ? "OFF (always start silent)"
                                      : "SAVED (whatever it was when stored)");
    return;
  }

  // Rotate the output across N pins, one burst each. GP18 fires a burst, then
  // the normal off-time, then GP19, then GP20, then back to GP18. The pins ARE
  // the outputs -- no decoder, no gates.
  //
  //   PHASE 3     three channels on GP18/19/20
  //   PHASE OFF   single output on GP5 as usual
  //
  // GP5 keeps carrying every pulse regardless, so it stays a valid scope
  // trigger and monitor no matter which channel is live.
  if (!strcmp(s, "PHASE")) {
    // Print what is actually in the DMA tables. Without this, "no output" and
    // "output you cannot see" look identical from the bench, and we spent a
    // while guessing which one we had.
    if (hasArg && !strcmp(arg, "DUMP")) {
      printPhase(o);
      const uint32_t nt = ringLen ? ringLen : 1;
      for (uint32_t k = 0; k < nt; k++) {
        // Layout: [0]=N-1, [1]=gap mask, then (mask, hi, lo) triples from [2].
        const uint32_t *t = ringLen ? elongRing[k] : elongTable;
        const uint32_t n = t[0] + 1;
        o.print("  burst "); o.print(k + 1);
        o.print((t[1] & CYCLE_MASK) ? " *: " : "  : ");
        for (uint32_t i = 0; i < n; i++) {
          const uint32_t m = t[2 + i * 3];
          o.print("[");
          for (int b = AMP_BITS - 1; b >= 0; b--) o.print((m >> b) & 1);
          o.print("] ");
        }
        o.print(" T1=");
        o.print((t[3] + 4) / 150.0, 2);   // 150 MHz -> us
        o.print("us T2=");
        o.print((t[4] + 9) / 150.0, 2);
        o.println("us");
      }
      o.print("  bits are GP");    o.print(ampBase + 2);
      o.print(" GP");              o.print(ampBase + 1);
      o.print(" GP");              o.print(ampBase);
      o.println(", left to right, one group per pulse");
      o.print("  * = cycle marker, GP");  o.print(cyclePin());
      o.println(" high for that whole burst");
      return;
    }
    if (!hasArg) { printPhase(o); return; }
    if (!strcmp(arg, "OFF")) {
      phaseMode = PH_OFF; applyGate(); printPhase(o); return;
    }
    if (!elongPioReady) {
      o.println("ERROR: elongation PIO unavailable -- PHASE needs it"); return;
    }
    // "PHASE 3" keeps meaning rotate, so existing use and saved settings stay
    // valid; SYNC/ROT are the explicit spellings.
    const char *m = arg;
    uint8_t mode = PH_ROTATE;
    if (!strncmp(arg, "SYNC", 4))     { mode = PH_SYNC;   m = arg + 4; }
    else if (!strncmp(arg, "ROT", 3)) { mode = PH_ROTATE; m = arg + 3; }
    while (*m == ' ' || *m == '\t') m++;
    uint32_t n = *m ? (uint32_t)atoi(m) : phaseLen;
    if (n < 1 || n > AMP_BITS) {
      o.print("ERROR: PHASE [ROT|SYNC] 1.."); o.print(AMP_BITS);
      o.println(", or PHASE OFF"); return;
    }
    phaseLen = n; phaseMode = mode;
    phaseArm();                 // engine up, elongation left exactly as it was
    printPhase(o);
    return;
  }


  // Per-TRAIN amplitude: a ring of burst tables, one per level, advanced by the
  // DMA IRQ at every burst boundary. Levels are a percentage of the base T1,
  // and since i = V*T1/L on an inductive primary, narrowing the pulses
  // attenuates that burst. Needs no hardware -- this is the width-as-amplitude
  // route, distinct from tap select.
  //
  //   TRAIN 100 75 50 25    four bursts, stepping down, then repeating
  //   TRAIN OFF             every burst identical again
  //
  // Composes with everything below it: whatever shapes a burst -- ELONGATE's
  // ratio or a SEQ envelope -- still applies, the ring just scales it.
  if (!strcmp(s, "TRAIN")) {
    if (hasArg && !strcmp(arg, "OFF")) {
      trainLen = 0;
      applyGate();
      o.println("TRAIN off -- every burst identical");
      return;
    }
    if (!hasArg) {
      if (!trainLen) { o.println("TRAIN: off (every burst identical)"); return; }
      o.print("TRAIN ring of "); o.print(trainLen); o.print(": ");
      for (uint32_t i = 0; i < trainLen; i++) {
        if (i) o.print(" -> ");
        o.print(trainAmp[i]); o.print("%");
      }
      o.println();
      return;
    }
    // Scratch first: a bad level partway through must not leave a half-written
    // ring for the ISR to walk into.
    uint8_t  tmp[TRAIN_MAX];
    uint32_t cnt = 0;
    char *save = nullptr;
    for (char *tok = strtok_r(arg, " \t", &save); tok;
         tok = strtok_r(nullptr, " \t", &save)) {
      if (cnt >= TRAIN_MAX) {
        o.print("ERROR: TRAIN takes at most "); o.print(TRAIN_MAX);
        o.println(" levels"); return;
      }
      int pct = atoi(tok);
      if (pct < 1 || pct > 100) {
        o.print("ERROR: level '"); o.print(tok);
        o.println("' out of range -- want 1..100 (percent of base T1)"); return;
      }
      tmp[cnt++] = (uint8_t)pct;
    }
    if (!cnt) { o.println("ERROR: TRAIN needs at least one level"); return; }
    if (!elongPioReady) {
      o.println("ERROR: elongation PIO unavailable -- TRAIN needs it");
      return;
    }
    for (uint32_t i = 0; i < cnt; i++) trainAmp[i] = tmp[i];
    trainLen = cnt;
    elongArm();                 // the ring lives on the elongation path
    o.print("TRAIN armed: "); o.print(trainLen);
    o.println(" bursts per cycle");
    return;
  }

  // Arbitrary per-pulse envelope: an explicit list of steps instead of the
  // geometric ratio. Each step is `t1_us,t2_us[,amp]`, steps separated by
  // spaces, amp being the 0..7 select code driven on GP18-20 during the gap
  // BEFORE that pulse fires.
  //
  //   SEQ 1,1,0 2,1,1 4,1,2 8,1,3     four pulses, widening, climbing taps
  //   SEQ OFF                          back to ELONGATE's ratio
  //
  // Widths land on integer PIO clocks, so what you ask for is what comes out
  // to within one 6.67 ns tick -- the same guarantee the 1.000 us step-charge
  // pulse relies on.
  if (!strcmp(s, "SEQ")) {
    if (hasArg && !strcmp(arg, "OFF")) {
      seqLen = 0;
      applyGate();
      o.println("SEQ off -- elongation back to ratio mode");
      return;
    }
    if (!hasArg) {
      if (!seqLen) { o.println("SEQ: no table loaded (ratio mode)"); return; }
      o.print("SEQ "); o.print(seqLen); o.println(" steps:");
      for (uint32_t i = 0; i < seqLen; i++) {
        o.print("  "); o.print(i + 1);
        o.print(": T1 "); o.print(seqSteps[i].t1_ns / 1000.0, 3);
        o.print(" us  T2 "); o.print(seqSteps[i].t2_ns / 1000.0, 3);
        o.print(" us  amp "); o.println(seqSteps[i].amp);
      }
      return;
    }
    // Parse into a scratch buffer first: a bad step halfway through must not
    // leave the live table half-overwritten while the PIO is reading it.
    SeqStep tmp[ELONG_MAX_PULSES];
    uint32_t cnt = 0;
    char *save = nullptr;
    for (char *tok = strtok_r(arg, " \t", &save); tok;
         tok = strtok_r(nullptr, " \t", &save)) {
      if (cnt >= ELONG_MAX_PULSES) {
        o.print("ERROR: SEQ takes at most "); o.print(ELONG_MAX_PULSES);
        o.println(" steps"); return;
      }
      double a = 0, b = 0; int amp = 0;
      int got = sscanf(tok, "%lf,%lf,%d", &a, &b, &amp);
      if (got < 2) {
        o.print("ERROR: bad step '"); o.print(tok);
        o.println("' -- want t1_us,t2_us[,amp]"); return;
      }
      if (a <= 0 || b <= 0) {
        o.println("ERROR: T1 and T2 must both be > 0"); return;
      }
      if (amp < 0 || amp > (int)AMP_MASK) {
        o.print("ERROR: amp must be 0.."); o.println(AMP_MASK); return;
      }
      tmp[cnt].t1_ns = (uint32_t)(a * 1000.0 + 0.5);
      tmp[cnt].t2_ns = (uint32_t)(b * 1000.0 + 0.5);
      tmp[cnt].amp   = (uint8_t)amp;
      cnt++;
    }
    if (!cnt) { o.println("ERROR: SEQ needs at least one step"); return; }
    for (uint32_t i = 0; i < cnt; i++) seqSteps[i] = tmp[i];
    seqLen = cnt;
    if (!elongPioReady) {
      o.println("ERROR: elongation PIO unavailable -- SEQ stored but not running");
      return;
    }
    elongArm();                 // a table implies the elongation path
    o.print("SEQ armed: "); o.print(seqLen); o.println(" pulses per burst");
    return;
  }

  // Each pulse in a COUNT-mode burst compounds off the last, T1 and T2
  // independently -- pulse 1 is the base ENC2/ENC3 set. The argument is the
  // TOTAL span across the burst (last/first): >1 grows, <1 shrinks, and the
  // per-pulse multiplier is derived from it and the pulse count (see
  // ElongCfg). ELONGATE OFF reverts to a flat train of the base pulses (the
  // normal carrier+gate path).
  if (!strcmp(s, "ELONGATE")) {
    // Any ELONGATE drops a loaded sequence. Asking for a ratio while a table is
    // armed is unambiguous -- you want the ratio -- and leaving the table in
    // place would silently ignore the command, since elongBuildInto() prefers
    // the table whenever one exists.
    seqLen = 0;
    if (hasArg && !strcmp(arg, "OFF")) {
      elongDisarm();
      o.println("elongation OFF -- flat base train");
      return;
    }
    // Golden-ratio growth per pulse, converted to the total span it implies
    // at the CURRENT pulse count. Stored as a total like every other setting
    // so it stays put if onCount later changes -- asking for "phi" is a way
    // of picking a number, not a third mode the rest of the code has to know
    // about. Fibonacci would land within a fraction of a percent of this past
    // its first few terms, which is why it is not separately implemented.
    if (hasArg && !strcmp(arg, "PHI")) {
      uint32_t n     = elongPulseCount();
      double   want  = pow(ELONG_PHI, (double)(n - 1));
      elong.ratio    = (want > ELONG_RATIO_MAX) ? ELONG_RATIO_MAX : want;
      elongArm();
      o.print("elongation ON: PHI over "); o.print(n);
      o.print(" pulses -> ratio "); o.print(elong.ratio, 4); o.print("x");
      if (want > ELONG_RATIO_MAX) {
        o.print(" (clamped from "); o.print(want, 0);
        o.print("x -- phi compounds past what one burst can hold at this pulse count)");
      }
      o.print(", "); o.print(elongPerPulseFactor(), 4); o.println("/pulse");
      return;
    }
    if (!hasArg || val < ELONG_RATIO_MIN || val > ELONG_RATIO_MAX) {
      o.println("ERROR: ELONGATE <total ratio 0.01..1000, e.g. 8 grows / 0.2 shrinks>,");
      o.println("       ELONGATE PHI, or ELONGATE OFF");
      return;
    }
    elong.ratio = val;
    elongArm();
    o.print("elongation ON: ratio "); o.print(elong.ratio, 4);
    o.print("x over "); o.print(elongPulseCount()); o.print(" pulses, ");
    o.print(elongPerPulseFactor(), 4); o.println("/pulse");
    return;
  }

  // What the two carrier encoders (ENC2/ENC3) currently edit -- also settable
  // from the dashboard's freq+duty / T1+T2 toggle, so a click there and a
  // press of the ENC2 button on the front panel stay in lockstep either way.
  if (!strcmp(s, "SHAPE")) {
    if      (hasArg && !strcmp(arg, "FD"))  shapeMode = SHAPE_FD;
    else if (hasArg && !strcmp(arg, "T12")) shapeMode = SHAPE_T12;
    else { o.println("ERROR: SHAPE FD or SHAPE T12"); return; }
    printState(o); return;
  }

  // Live-prints every raw quadrature/button edge and every fired step to
  // Serial -- turn this on, then turn a knob or press its button, to see
  // whether anything is reaching the firmware at all. Leave it off otherwise:
  // it is debug-only chatter, not something host tooling should parse.
  if (!strcmp(s, "ENCLOG")) {
    if (!hasArg) { o.println("ERROR: ENCLOG 0 or ENCLOG 1"); return; }
    encLogEnabled = ((int)val != 0);
    o.println(encLogEnabled ? "encoder logging ON" : "encoder logging OFF");
    return;
  }

  // Asking for a carrier frequency and then wondering why nothing changed
  // would be a silly trap to leave, so C implies SRC INT.
  if (!strcmp(s, "C")) {
    if (!hasArg || val <= 0.0) { o.println("ERROR: need a positive frequency"); return; }
    double ns = 1e9 / val;
    if (ns < (double)CARRIER_NS_MIN) ns = (double)CARRIER_NS_MIN;
    if (ns > (double)CARRIER_NS_MAX) ns = (double)CARRIER_NS_MAX;
    carrierPeriodNs = (uint64_t)ns;
    sigSource = SRC_INT;
    applyGate(); printState(o); return;
  }

  if (!strcmp(s, "CD")) {
    if (!hasArg || val <= 0.0) { o.println("ERROR: need a positive percent"); return; }
    double ppm = val * 10000.0;
    if (ppm < DUTY_PPM_MIN) ppm = DUTY_PPM_MIN;
    if (ppm > DUTY_PPM_MAX) ppm = DUTY_PPM_MAX;
    carrierDutyPpm = (uint32_t)ppm;
    applyGate(); printState(o); return;
  }

  // RAMP1/RAMP2 <step_us> <bursts_per_step> <limit_us> [STOP|WRAP]
  // Auto-steps the carrier's own T1 (RAMP1) or T2 (RAMP2) every N bursts.
  // "RAMP1 OFF" / "RAMP2 OFF" cancels, holding whatever value it reached.
  if (!strcmp(s, "RAMP1") || !strcmp(s, "RAMP2")) {
    bool isT1 = (s[4] == '1');
    RampCfg &r = isT1 ? ramp1 : ramp2;
    if (hasArg && !strcmp(arg, "OFF")) {
      r.active = false;
      o.println(isT1 ? "T1 ramp stopped" : "T2 ramp stopped");
      return;
    }
    double stepUs = 0, limitUs = 0; unsigned long bursts = 0;
    char modeBuf[8] = "STOP";
    int nParsed = sscanf(arg, "%lf %lu %lf %7s", &stepUs, &bursts, &limitUs, modeBuf);
    if (nParsed < 3 || stepUs == 0.0 || bursts < 1 || limitUs <= 0.0) {
      o.print("ERROR: RAMP"); o.print(isT1 ? "1" : "2");
      o.println(" <step_us> <bursts_per_step> <limit_us> [STOP|WRAP]");
      return;
    }
    for (char *p = modeBuf; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;

    armRamp(r, isT1, (int64_t)(stepUs * 1000.0), (uint32_t)bursts,
           (uint64_t)(limitUs * 1000.0), !strcmp(modeBuf, "WRAP"));
    if (isT1) {                       // the knob's SWEEP position follows RAMP1
      sweepStepNs  = r.stepNs;   sweepBursts = r.burstsPerStep;
      sweepLimitNs = r.limitNs;  sweepWrap   = r.wrap;
    }

    o.print(isT1 ? "T1" : "T2"); o.print(" ramp armed: step ");
    o.print(stepUs, 3); o.print(" us every "); o.print(r.burstsPerStep);
    o.print(" burst(s), limit "); o.print(limitUs, 3);
    o.print(" us, "); o.println(r.wrap ? "WRAP" : "STOP at limit");
    return;
  }

  if (!strcmp(s, "ON") || !strcmp(s, "OFF")) {
    if (!hasArg) { o.println("ERROR: need a count"); return; }
    if (val < COUNT_MIN || val > (double)COUNT_MAX) {
      o.print("ERROR: range 1.."); o.println(COUNT_MAX); return;
    }
    if (s[1] == 'N') onCount = (uint32_t)val; else offCount = (uint32_t)val;
    shotMode = false;
    applyGate(); printState(o); return;
  }

  if (!strcmp(s, "T") || !strcmp(s, "G")) {
    if (!hasArg || val <= 0.0) { o.println("ERROR: need a positive value"); return; }
    double ns = (s[0] == 'T') ? val * 1000.0 : 1e9 / val;
    if (ns < (double)PERIOD_NS_MIN) ns = (double)PERIOD_NS_MIN;
    if (ns > (double)PERIOD_NS_MAX) ns = (double)PERIOD_NS_MAX;
    gatePeriodNs = (uint64_t)ns;
    applyGate(); printState(o); return;
  }

  if (!strcmp(s, "D") || !strcmp(s, "W")) {
    if (!hasArg || val <= 0.0) { o.println("ERROR: need a positive value"); return; }
    double ppm = (s[0] == 'D') ? val * 10000.0
                               : val * 1000.0 * 1e6 / (double)gatePeriodNs;
    if (ppm < DUTY_PPM_MIN) ppm = DUTY_PPM_MIN;
    if (ppm > DUTY_PPM_MAX) ppm = DUTY_PPM_MAX;
    gateDutyPpm = (uint32_t)ppm;
    applyGate(); printState(o); return;
  }

  if (!strcmp(s, "M")) {
    if (!hasArg) { o.println("ERROR: need 0 or 1"); return; }
    gateMode = ((int)val == 1) ? MODE_COUNT : MODE_TIME;
    selParam = 0;
    if (gateMode != MODE_COUNT) elong.active = false;   // elongation needs COUNT
    applyGate(); printState(o); return;
  }

  if (!strcmp(s, "E")) {
    if (!hasArg) { o.println("ERROR: need 0 or 1"); return; }
    outputEnabled = ((int)val != 0);
    applyGate();
    o.println(outputEnabled ? "output ENABLED" : "output DISABLED");
    // Say so rather than letting "output ENABLED" sit there next to dead pins.
    if (outputEnabled && interlockTripped)
      o.println("  ...but the INTERLOCK is open, so nothing is coming out. ARM to clear.");
    return;
  }

  // LOCK 0|1 -- enable the enclosure interlock. Enabling always lands tripped,
  // whatever the pin says: arming is a deliberate act, and "I turned it on and
  // it stayed running" teaches the operator the interlock does nothing.
  if (!strcmp(s, "LOCK")) {
    if (!hasArg) {
      o.print("interlock "); o.print(interlockEnabled ? "ON" : "OFF");
      o.print(", "); o.print(interlockTripped ? "TRIPPED" : "armed");
      o.print(", GP26 reads "); o.println(interlockClosed() ? "closed" : "OPEN");
      return;
    }
    interlockEnabled = ((int)val != 0);
    interlockTripped = interlockEnabled;
    interlockOpenAt = interlockShutAt = 0;
    cfgTouch();
    applyGate();
    if (interlockEnabled) o.println("interlock ON -- tripped until you ARM");
    else                  o.println("interlock OFF -- output no longer gated by GP26");
    return;
  }

  if (!strcmp(s, "ARM")) { interlockArm(o); return; }

  // RUN <ms> — silent, then output for exactly <ms>, then silent again.
  // Returns immediately; loop() closes the window. The host arms its scope
  // before calling this and polls /state (running:0) or just waits ms.
  if (!strcmp(s, "RUN")) {
    if (!hasArg || val <= 0.0) { o.println("ERROR: need a duration in ms"); return; }
    // Refuse rather than run a silent window. A host that arms a scope, calls
    // RUN, and is told "started" would record an empty capture and blame the
    // trigger -- the same reason burst length reports the clamped value.
    if (interlockTripped) { o.println("ERROR: INTERLOCK open -- ARM first"); return; }
    if (val > 3600000.0) val = 3600000.0;              // 1 hour ceiling
    shotMode = false;
    outputEnabled = false; applyGate();                 // guarantee a silent start
    runLenMs   = (uint32_t)val;
    runUntilMs = millis() + runLenMs;
    if (!runUntilMs) runUntilMs = 1;                    // never collide with "idle"
    outputEnabled = true;  applyGate();
    o.print("RUN "); o.print(runLenMs); o.println(" ms started");
    return;
  }

  if (!strcmp(s, "STOP")) {
    runUntilMs = 0; shotMode = false;
    outputEnabled = false; applyGate();
    o.println("stopped"); return;
  }

  // SHOT — fire exactly one burst of ON pulses, then stop. Pairs with a
  // single-shot scope armed on the GP4 marker: one burst, one capture.
  if (!strcmp(s, "SHOT")) {
    if (gateMode != MODE_COUNT) {
      o.println("ERROR: SHOT needs COUNT mode (M 1)"); return;
    }
    if (interlockTripped) { o.println("ERROR: INTERLOCK open -- ARM first"); return; }
    runUntilMs = 0;
    shotMode = true;
    outputEnabled = false; applyGate();     // guarantee a silent, synced start
    outputEnabled = true;  applyGate();     // fires one burst, then parks
    o.print("SHOT: one burst of "); o.print(onCount);
    o.println(" pulses fired");
    return;
  }

  if (!strcmp(s, "I")) {
    gateInvert = !gateInvert; applyGate();
    o.println(gateInvert ? "gate INVERTED" : "gate normal"); return;
  }

  if (!strcmp(s, "V")) {
    pulseInvert = !pulseInvert; applyGate();
    o.println(pulseInvert ? "pulses ACTIVE-LOW" : "pulses active-high"); return;
  }

  if (!strcmp(s, "R")) {
    onCount = 10; offCount = 90;
    gatePeriodNs = 10000000ULL; gateDutyPpm = 100000UL;
    carrierPeriodNs = 10000ULL; carrierDutyPpm = 500000UL;
    gateInvert = false; pulseInvert = false; selParam = 0;
    shotMode = false;
    ramp1 = RampCfg(); ramp2 = RampCfg();   // a ramp surviving "defaults" would be surprising
    elong = ElongCfg();                     // likewise -- back to inactive, factor 1.05
    seqLen = 0;                             // and a loaded envelope is no different
    trainLen = 0;                           // ...nor an amplitude ring
    phaseMode = PH_OFF; phaseLen = 3;        // ...nor channel rotation
    shapeMode = SHAPE_FD;
    // INT, not EXT. EXT is a leftover from the Nano original, which had no
    // carrier of its own -- but on this board "restore defaults" then hands
    // back a rig waiting on a signal generator that may not exist, and the
    // symptom is silence with every setting looking correct. Defaults must
    // land somewhere that actually pulses.
    // interlockEnabled/interlockTripped are untouched here, and that is not an
    // oversight: "restore defaults" must never be a way to disarm a safety
    // input. Output comes back on as intent; if the enclosure is open it still
    // will not emit, which is correct.
    sigSource = SRC_INT; outputEnabled = true;
    // ROTATE 3, not OFF. "Known state" exists to be pressed when nothing is
    // coming out -- and single mode idles GP18-20 and the GP22 marker, so
    // resetting into it silently kills four of the five pins anyone is likely
    // to have a probe on. Landing here lights every output at once: the three
    // channels, the cycle marker, and GP5 which carries the train regardless.
    phaseMode = PH_ROTATE; phaseLen = 3;
    applyGate();
    inLogAction("reset to known state");
    o.println("defaults restored -- 100 kHz / 50%, 10 on / 90 off, offset x3, output ON");
    printState(o); return;
  }

  o.print("Unknown command: "); o.println(s);
  o.println("Type ? for help");
}

// ============================================================================
// Encoder + buttons
// ============================================================================
//
// Plain polling. On the Nano this had to be hand-tuned around the software AND
// loop; here PIO owns the waveform, so the CPU is free and nothing it does can
// perturb the output.

static const int8_t ENC_LUT[16] = {
   0,  1, -1,  0,
  -1,  0,  0,  1,
   1,  0,  0, -1,
   0, -1,  1,  0
};

static uint8_t  encState = 0;
static int8_t   encAccum = 0;
static uint32_t encLastMs = 0;

// GP11/12: carrier frequency (FD) or T1 width (T12).
static uint8_t  enc2State = 0;
static int8_t   enc2Accum = 0;
static uint32_t enc2LastMs = 0;

// GP14/16: carrier duty (FD) or T2 space (T12).
static uint8_t  enc3State = 0;
static int8_t   enc3Accum = 0;
static uint32_t enc3LastMs = 0;

static void encLogRaw(const char *name, uint8_t cur, int8_t accum) {
  if (!encLogEnabled) return;
  Serial.print(name); Serial.print(" raw: A="); Serial.print(cur & 1);
  Serial.print(" B="); Serial.print((cur >> 1) & 1);
  Serial.print(" accum="); Serial.println(accum);
}

static void encLogStep(const char *name, uint8_t gp, int8_t dir) {
  // The ring is fed unconditionally -- ENCLOG only gates the serial chatter.
  // A decoded step that never appears next to its raw edges is the signature
  // of a swapped A/B pair, so the two have to be in the same log to compare.
  inLogPush(gp, 0, 1, dir);
  if (!encLogEnabled) return;
  Serial.print(name); Serial.println(dir > 0 ? " STEP dir=+1" : " STEP dir=-1");
}

// Acceleration is per-encoder (each call site passes its own lastMs) so
// spinning one knob fast right after touching another cannot bleed a fast
// step rate onto the one that was actually still.
static uint32_t encAccelerate(uint32_t &lastMs, uint32_t base) {
  uint32_t now = millis();
  uint32_t dt  = now - lastMs;
  lastMs = now;
  if (dt < 15) return base * 100;
  if (dt < 40) return base * 10;
  return base;
}

// Acceleration for the log-scaled knobs, in basis points. Deliberately NOT
// encAccelerate(): that scales one `base` by 10 and 100, and these params want
// a fine end of 0.1 % with a fast end still near 50 % -- a 500x spread a single
// geometric ladder cannot cover. Split, a flick still crosses a decade in ~6
// clicks while a slow turn resolves 25 Hz at 25 kHz.
static uint32_t encAccelerateBp(uint32_t &lastMs) {
  uint32_t now = millis();
  uint32_t dt  = now - lastMs;
  lastMs = now;
  if (dt < 15) return 5000;                         // 50 %  -- decade in ~6
  if (dt < 40) return 200;                          //  2 %
  return 10;                                        //  0.1 %
}

// Multiplicative step shared by every log-scaled parameter (gate period,
// carrier period, T1, T2) -- they all span several decades, so a constant
// step in ns/us would be unusable at one end of the range and too coarse at
// the other. `bp` is the change per click in basis points (hundredths of a
// percent) before acceleration. Percent alone bottomed out at 1 %, which is
// 250 Hz at 25 kHz -- 60x coarser than the 4.2 Hz the PIO resolves there,
// since genCycles() holds cdiv at 1 and gets 6000 clocks into that period.
// Worst case below is v * 15000 = 9e14, four decades inside uint64.
static uint64_t multiplicativeStep(uint64_t v, int8_t dir, uint32_t bp) {
  if (bp > 5000) bp = 5000;                         // cap one click at +50 %
  uint64_t nv = (dir > 0) ? (v * (10000 + bp) + 5000) / 10000
                          : (v * 10000 + (10000 + bp) / 2) / (10000 + bp);
  if (nv <= v && dir > 0) nv = v + 1;               // never stall
  if (nv >= v && dir < 0) nv = (v > 0) ? v - 1 : 0;
  return nv;
}

// What each knob just did, in words, read back from live state AFTER the step
// was applied -- so the log reports what actually happened rather than what was
// intended. Turning a knob and seeing the wrong parameter move is the failure
// this is here to catch.
static void logEnc1() {
  if (gateMode == MODE_COUNT)
    inLogAction(selParam == 0 ? "ON = %lu pulses" : "OFF = %lu pulses",
                (unsigned long)(selParam == 0 ? onCount : offCount));
  else if (selParam == 0)
    inLogAction("gate period = %lu us", (unsigned long)(gatePeriodNs / 1000ULL));
  else
    inLogAction("gate duty = %lu.%02lu%%", (unsigned long)(gateDutyPpm / 10000UL),
                (unsigned long)((gateDutyPpm / 100UL) % 100UL));
}

static void logCarrier(const char *which) {
  if (shapeMode == SHAPE_T12) {
    uint64_t t1 = currentT1Ns(), t2 = currentT2Ns();
    uint64_t v  = (which[0] == 'T') ? t1 : t2;
    inLogAction("%s = %lu.%02lu us", which, (unsigned long)(v / 1000ULL),
                (unsigned long)((v % 1000ULL) / 10ULL));
  } else if (which[0] == 'T') {
    uint64_t hz = carrierPeriodNs ? (1000000000ULL / carrierPeriodNs) : 0;
    inLogAction("carrier = %lu Hz", (unsigned long)hz);
  } else {
    inLogAction("carrier duty = %lu.%02lu%%",
                (unsigned long)(carrierDutyPpm / 10000UL),
                (unsigned long)((carrierDutyPpm / 100UL) % 100UL));
  }
}

static void applyEncoderStep(int8_t dir) {
  if (gateMode == MODE_COUNT) {
    // ON pulses stays linear -- one click, one pulse, however fast the knob
    // spins. Burst length is a count you dial to an exact number ("give me
    // 7"), and an acceleration multiplier makes that target unhittable: a
    // quick flick jumps by 10 or 100 and you have to creep back. OFF pulses
    // keeps the speed-up because it routinely runs to hundreds and nobody
    // cares whether the gap is 90 or 91. encAccelerate() still runs in both
    // cases so encLastMs stays fresh across a selParam switch.
    uint32_t accel = encAccelerate(encLastMs, 1);
    uint32_t step  = (selParam == 0) ? 1 : accel;
    uint32_t *p = (selParam == 0) ? &onCount : &offCount;
    int64_t  v  = (int64_t)*p + (int64_t)dir * (int64_t)step;
    if (v < (int64_t)COUNT_MIN) v = COUNT_MIN;
    if (v > (int64_t)COUNT_MAX) v = COUNT_MAX;
    *p = (uint32_t)v;
  } else if (selParam == 0) {
    // Multiplicative: the period spans eight decades (1 us .. 60 s).
    uint32_t bp = encAccelerateBp(encLastMs);
    uint64_t v = multiplicativeStep(gatePeriodNs, dir, bp);
    if (v < PERIOD_NS_MIN) v = PERIOD_NS_MIN;
    if (v > PERIOD_NS_MAX) v = PERIOD_NS_MAX;
    gatePeriodNs = v;
  } else {
    int32_t step = (int32_t)encAccelerate(encLastMs, 500);     // 0.05 %/click
    int64_t v = (int64_t)gateDutyPpm + (int64_t)dir * step;
    if (v < (int64_t)DUTY_PPM_MIN) v = DUTY_PPM_MIN;
    if (v > (int64_t)DUTY_PPM_MAX) v = DUTY_PPM_MAX;
    gateDutyPpm = (uint32_t)v;
  }
  applyGate();
}

// "A" knob (GP11/12): carrier frequency in FD shape, T1 width in T12 shape.
// Turning it implies SRC INT -- same trap C/CD already close off for the
// serial API, now closed for the front panel too.
static void applyEncoder2Step(int8_t dir) {
  sigSource = SRC_INT;
  uint32_t bp = encAccelerateBp(enc2LastMs);
  if (shapeMode == SHAPE_FD) {
    // Frequency, not period: turning "up" must raise Hz, i.e. shrink the
    // period, so the step direction is inverted relative to the raw period.
    uint64_t v = multiplicativeStep(carrierPeriodNs, (int8_t)-dir, bp);
    if (v < CARRIER_NS_MIN) v = CARRIER_NS_MIN;
    if (v > CARRIER_NS_MAX) v = CARRIER_NS_MAX;
    carrierPeriodNs = v;
    applyGate();
  } else {
    uint64_t t1 = currentT1Ns(), t2 = currentT2Ns();
    applyT1T2(multiplicativeStep(t1, dir, bp), t2);
  }
}

// "B" knob (GP14/16): carrier duty in FD shape, T2 space in T12 shape.
static void applyEncoder3Step(int8_t dir) {
  sigSource = SRC_INT;
  if (shapeMode == SHAPE_FD) {
    int32_t step = (int32_t)encAccelerate(enc3LastMs, 500);    // 0.05 %/click
    int64_t v = (int64_t)carrierDutyPpm + (int64_t)dir * step;
    if (v < (int64_t)DUTY_PPM_MIN) v = DUTY_PPM_MIN;
    if (v > (int64_t)DUTY_PPM_MAX) v = DUTY_PPM_MAX;
    carrierDutyPpm = (uint32_t)v;
    applyGate();
  } else {
    uint32_t bp = encAccelerateBp(enc3LastMs);
    uint64_t t1 = currentT1Ns(), t2 = currentT2Ns();
    applyT1T2(t1, multiplicativeStep(t2, dir, bp));
  }
}

static void pollEncoder() {
  uint8_t cur = (uint8_t)((gpio_get(PIN_ENC_A) ? 1 : 0) |
                          (gpio_get(PIN_ENC_B) ? 2 : 0));
  if (cur == (encState & 0x03)) return;
  encState = ((encState << 2) | cur) & 0x0F;
  encAccum += ENC_LUT[encState];
  encLogRaw("enc1", cur, encAccum);
  if (encAccum >= 4)       { encLogStep("enc1", PIN_ENC_A, +1); applyEncoderStep(+1); logEnc1(); encAccum = 0; }
  else if (encAccum <= -4) { encLogStep("enc1", PIN_ENC_A, -1); applyEncoderStep(-1); logEnc1(); encAccum = 0; }
}

static void pollEncoder2() {
  uint8_t cur = (uint8_t)((gpio_get(PIN_ENC2_A) ? 1 : 0) |
                          (gpio_get(PIN_ENC2_B) ? 2 : 0));
  if (cur == (enc2State & 0x03)) return;
  enc2State = ((enc2State << 2) | cur) & 0x0F;
  enc2Accum += ENC_LUT[enc2State];
  encLogRaw("enc2", cur, enc2Accum);
  if (enc2Accum >= 4)       { encLogStep("enc2", PIN_ENC2_A, +1); applyEncoder2Step(+1); logCarrier("T1"); enc2Accum = 0; }
  else if (enc2Accum <= -4) { encLogStep("enc2", PIN_ENC2_A, -1); applyEncoder2Step(-1); logCarrier("T1"); enc2Accum = 0; }
}

static void pollEncoder3() {
  uint8_t cur = (uint8_t)((gpio_get(PIN_ENC3_A) ? 1 : 0) |
                          (gpio_get(PIN_ENC3_B) ? 2 : 0));
  if (cur == (enc3State & 0x03)) return;
  enc3State = ((enc3State << 2) | cur) & 0x0F;
  enc3Accum += ENC_LUT[enc3State];
  encLogRaw("enc3", cur, enc3Accum);
  if (enc3Accum >= 4)       { encLogStep("enc3", PIN_ENC3_A, +1); applyEncoder3Step(+1); logCarrier("T2"); enc3Accum = 0; }
  else if (enc3Accum <= -4) { encLogStep("enc3", PIN_ENC3_A, -1); applyEncoder3Step(-1); logCarrier("T2"); enc3Accum = 0; }
}

// Toggles the T1/T2 elongation ramp on or off as one unit -- a physical
// pushbutton has no way to type in step/bursts/limit, so ON re-arms whatever
// was last configured (over serial/HTTP, or a previous press of this same
// button); the very first press ever, with nothing configured yet, arms a
// gentle default sweep instead of silently doing nothing.
// off -> rotate -> sync -> off. Bound to a long press on the elongation
// encoder's switch.
// Count out a number on the status LED. A standalone box has no display, so
// without this the only way to know which mode a press landed on is to plug in
// USB -- which defeats the point of the panel. Blocking is fine here: it runs
// from a button handler, and PIO owns the waveform regardless of what the CPU
// is doing.
static void ledBlink(uint8_t times) {
  const bool was = gpio_get(PIN_STATUS_LED);
  for (uint8_t i = 0; i < times; i++) {
    gpio_put(PIN_STATUS_LED, 1); delay(90);
    gpio_put(PIN_STATUS_LED, 0); delay(140);
  }
  gpio_put(PIN_STATUS_LED, was);
}

// Steps the OUTPUT axis only: single -> offset -> sync -> single. Elongation
// and sweep are separate axes on the encoder button, so every combination is
// reachable. Derived from the live state rather than a counter, so a mode set
// over serial leaves this button in the right place.
static void cyclePhaseMode() {
  if (!elongPioReady) { Serial.println("modes unavailable (no elongation PIO)"); return; }
  phaseMode = (phaseMode == PH_OFF)    ? PH_ROTATE
            : (phaseMode == PH_ROTATE) ? PH_SYNC
                                       : PH_OFF;
  if (phaseOn()) phaseArm(); else applyGate();
  Serial.print("output: ");
  Serial.println(phaseMode == PH_OFF    ? "SINGLE -- one output on GP5"
               : phaseMode == PH_ROTATE ? "OFFSET -- one burst per channel"
                                        : "SYNC -- all channels together");
  inLogAction(phaseMode == PH_OFF ? "output SINGLE"
            : phaseMode == PH_ROTATE ? "output OFFSET" : "output SYNC");
  ledBlink(phaseMode == PH_OFF ? 1 : phaseMode == PH_ROTATE ? 2 : 3);
}

// The sweep axis. Arms RAMP1 with whatever RAMP1 was last given, so the button
// re-arms the sweep you set up rather than a canned one.
static void toggleSweep() {
  if (ramp1.active || ramp2.active) {
    ramp1.active = false; ramp2.active = false;
    Serial.println("sweep: OFF");
      inLogAction("sweep OFF");
    ledBlink(1);
    return;
  }
  armRamp(ramp1, true, sweepStepNs, sweepBursts, sweepLimitNs, sweepWrap);
  applyGate();
  Serial.print("sweep: ON -- T1 steps ");
  Serial.print(sweepStepNs / 1000.0, 3); Serial.print(" us every ");
  Serial.print(sweepBursts); Serial.print(" bursts to ");
  Serial.print(sweepLimitNs / 1000.0, 3); Serial.print(" us, ");
  Serial.println(sweepWrap ? "WRAP" : "STOP");
  ledBlink(2);
}

static void toggleElongation() {
  if (elong.active) {
    elongDisarm();
    Serial.println("elongation: OFF -- flat base train");
      inLogAction("elongation OFF");
  } else {
    elongArm();
    Serial.print("elongation: ON, ratio ");
    Serial.print(elong.ratio, 4);
    Serial.print("x over "); Serial.print(elongPulseCount());
    Serial.print(" pulses ("); Serial.print(elongPerPulseFactor(), 4);
    Serial.println("/pulse)");
        inLogAction("elongation ON");
  }
}

// Runs before anything else in loop(). Nothing here can be starved by a slow
// HTTP request or a flash write, because the trip path is a pin read and an
// applyGate() -- and applyGate() stops the state machines rather than asking
// them nicely.
static void pollInterlock() {
  if (!interlockEnabled) { interlockOpenAt = interlockShutAt = 0; return; }

  const uint32_t now = millis() ? millis() : 1;   // 0 is the "not timing" value

  if (!interlockClosed()) {
    interlockShutAt = 0;
    if (interlockTripped) return;                 // already down; nothing to do
    if (!interlockOpenAt) { interlockOpenAt = now; return; }
    if ((uint32_t)(now - interlockOpenAt) < INTERLOCK_TRIP_MS) return;

    interlockTripped = true;
    interlockTrips++;
    applyGate();                                  // output dies here, not later
    inLogAction("INTERLOCK OPEN - output cut");
    Serial.println("INTERLOCK OPEN -- output cut. Close the enclosure, then ARM.");
    return;
  }

  interlockOpenAt = 0;
  if (!interlockShutAt) interlockShutAt = now;    // start (or continue) timing
}

// Split out so the command, the button and the web endpoint cannot drift apart
// on what counts as a legal re-arm.
static bool interlockArm(Print &o) {
  if (!interlockEnabled) { o.println("interlock is OFF (LOCK 1 to enable)"); return false; }
  if (!interlockTripped) { o.println("interlock already armed");             return false; }
  if (!interlockClosed()) {
    o.println("REFUSED: enclosure still open (GP26 is high)");
    return false;
  }
  if (!interlockShutAt || (uint32_t)(millis() - interlockShutAt) < INTERLOCK_SHUT_MS) {
    o.print("REFUSED: needs "); o.print(INTERLOCK_SHUT_MS);
    o.println(" ms of steady closure first -- check for an intermittent contact");
    return false;
  }
  interlockTripped = false;
  applyGate();
  inLogAction("interlock ARMED");
  o.println("interlock ARMED");
  return true;
}

static void pollButtons() {
  static uint8_t  swLast = 1, disLast = 1, modeLast = 0xFF;
  static uint8_t  sw2Last = 1, sw3Last = 1;
  static uint8_t  sw2RawLast = 1, sw3RawLast = 1;   // ENCLOG edge tracking, independent of debounce
  static uint32_t swAt = 0, disAt = 0, sw2At = 0, sw3At = 0;
  static uint32_t sw3DownAt = 0, phAt = 0;
  static uint8_t  phLast = 1;
  static bool     sw3Held = false;
  const uint32_t DEBOUNCE_MS = 30;
  // Long-press threshold on the elongation encoder's switch: below this it is a
  // plain elongation toggle, at or above it cycles the output mode.
  const uint32_t HOLD_MS = 700;
  uint32_t now = millis();

  uint8_t sw = gpio_get(PIN_ENC_SW) ? 1 : 0;
  if (sw != swLast && now - swAt > DEBOUNCE_MS) {
    swAt = now; swLast = sw;
    if (!sw) {
      selParam ^= 1;
            inLogAction(selParam ? "edit: OFF/duty" : "edit: ON/period");
      Serial.print("edit: ");
      if (gateMode == MODE_COUNT) Serial.println(selParam ? "OFF pulses" : "ON pulses");
      else                        Serial.println(selParam ? "DUTY"       : "PERIOD");
    }
  }

  uint8_t dis = gpio_get(PIN_DISABLE) ? 1 : 0;
  if (dis != disLast && now - disAt > DEBOUNCE_MS) {
    disAt = now; disLast = dis;
    if (!dis) {
      outputEnabled = !outputEnabled;
      applyGate();
      Serial.println(outputEnabled ? "output ENABLED" : "output DISABLED");
    }
  }

  // Edge-triggered so a serial/HTTP M command can override until the next flip.
  uint8_t ms = gpio_get(PIN_MODE_SW) ? 1 : 0;
  if (ms != modeLast) {
    bool announce = (modeLast != 0xFF);
    modeLast = ms;
    gateMode = ms ? MODE_COUNT : MODE_TIME;
    selParam = 0;
    if (gateMode != MODE_COUNT) elong.active = false;   // elongation needs COUNT
    if (announce) {
      applyGate();
      Serial.println(gateMode == MODE_COUNT ? "mode: COUNT" : "mode: TIME");
    }
  }

  uint8_t sw2 = gpio_get(PIN_ENC2_SW) ? 1 : 0;
  // Logged on every raw edge (own tracking var, updated every call so this
  // fires once per real transition, not once per loop while debounce is
  // pending) -- a button that's wired but chattering looks different in the
  // log from one that never moves at all (dead wiring).
  if (encLogEnabled && sw2 != sw2RawLast) {
    Serial.print("enc2 sw raw="); Serial.println(sw2);
  }
  sw2RawLast = sw2;
  if (sw2 != sw2Last && now - sw2At > DEBOUNCE_MS) {
    sw2At = now; sw2Last = sw2;
    if (!sw2) {
      shapeMode = (shapeMode == SHAPE_FD) ? SHAPE_T12 : SHAPE_FD;
      Serial.println(shapeMode == SHAPE_T12 ? "carrier edit: T1/T2 independent"
                                            : "carrier edit: freq/duty");
      inLogAction(shapeMode == SHAPE_T12 ? "knobs edit T1/T2" : "knobs edit freq/duty");
    }
  }

  uint8_t sw3 = gpio_get(PIN_ENC3_SW) ? 1 : 0;
  if (encLogEnabled && sw3 != sw3RawLast) {
    Serial.print("enc3 sw raw="); Serial.println(sw3);
  }
  sw3RawLast = sw3;
  // Encoder button as it always was: press toggles elongation. Hold toggles the
  // sweep, which used to live here too. Output mode has moved to its own button
  // so it can be combined with either of these.
  if (sw3 != sw3Last && now - sw3At > DEBOUNCE_MS) {
    sw3At = now; sw3Last = sw3;
    if (!sw3) {
      sw3DownAt = now;
      sw3Held   = false;
    } else if (!sw3Held) {
      toggleElongation();
    }
  }
  if (!sw3 && !sw3Held && sw3DownAt && now - sw3DownAt >= HOLD_MS) {
    sw3Held = true;
    toggleSweep();
  }

  // Dedicated output-mode button.
  uint8_t ph = gpio_get(PIN_PHASE_SW) ? 1 : 0;
  if (ph != phLast && now - phAt > DEBOUNCE_MS) {
    phAt = now; phLast = ph;
    if (!ph) cyclePhaseMode();
  }
}

// 64 was fine when every command was a verb and one number. SEQ is not: 25
// steps of "12.500,3.250,7 " is ~380 characters, and the old buffer truncated
// mid-line and then ran whatever survived -- a 26-step sequence silently armed
// as 15 with no error anywhere. Truncation is now an error, not a haircut.
static const size_t CMD_MAX = 512;
static char     cmdBuf[CMD_MAX];
static uint16_t cmdLen = 0;
static bool     cmdOverflow = false;

static void pollSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdOverflow) {
        Serial.print("ERROR: command longer than ");
        Serial.print(CMD_MAX - 1); Serial.println(" chars -- ignored");
      } else if (cmdLen) {
        cmdBuf[cmdLen] = 0;
        runCommand(cmdBuf, Serial);
      }
      cmdLen = 0; cmdOverflow = false;
    } else if (cmdLen < sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = c;
    } else {
      cmdOverflow = true;      // keep draining to the newline, then complain
    }
  }
}

// ============================================================================
// Network
// ============================================================================

static WebServer server(80);
static bool      serverUp = false;

// The page itself lives in webui.h -- see the contract note there. The
// sketch only knows it as a PROGMEM blob to hand to send_P().
#include "webui.h"

static void handleCmd() {
  String q = server.arg("c");
  if (!q.length()) { server.send(400, "text/plain", "missing ?c="); return; }
  // Reject rather than truncate: toCharArray() silently cuts to fit, and a
  // half a SEQ line still parses as a valid shorter sequence, so the bug shows
  // up as "the envelope has fewer pulses than I asked for" with nothing in any
  // log to explain it.
  if (q.length() >= (int)CMD_MAX) {
    server.send(414, "text/plain",
                String("command too long (") + q.length() + " chars, max "
                + (int)(CMD_MAX - 1) + ")");
    return;
  }
  char buf[CMD_MAX];
  q.toCharArray(buf, sizeof(buf));
  StringPrint sp;
  runCommand(buf, sp);
  server.send(200, "text/plain", sp.s);
}

// Live pin levels plus every edge since `since`. One emitter for both the
// serial command and the HTTP route, so the terminal and the browser can never
// disagree about what the panel is doing.
static void printInputsJson(Print &o, uint32_t since) {
  o.print("{\"seq\":"); o.print(inLogSeq);
  o.print(",\"now\":"); o.print(millis());
  o.print(",\"pins\":[");
  for (uint i = 0; i < count_of(INPUTS); i++) {
    if (i) o.print(",");
    o.print("{\"gp\":");     o.print(INPUTS[i].gp);
    o.print(",\"board\":");  o.print(INPUTS[i].board);
    o.print(",\"name\":\"");o.print(INPUTS[i].name);
    o.print("\",\"level\":");o.print(gpio_get(INPUTS[i].gp) ? 1 : 0);
    o.print("}");
  }
  // Only what is still in the ring: a caller that fell behind gets the oldest
  // surviving event rather than a confident replay of entries long overwritten.
  uint32_t first = (inLogSeq > INLOG_MAX) ? (inLogSeq - INLOG_MAX) : 0;
  if (since < first) since = first;
  o.print("],\"events\":[");
  bool sep = false;
  for (uint32_t s = since; s < inLogSeq; s++) {
    const InEvent &e = inLog[s % INLOG_MAX];
    if (sep) o.print(",");
    sep = true;
    o.print("{\"seq\":");   o.print(s);
    o.print(",\"ms\":");    o.print(e.ms);
    o.print(",\"gp\":");    o.print(e.gp);
    o.print(",\"level\":"); o.print(e.level);
    o.print(",\"kind\":");  o.print(e.kind);
    o.print(",\"step\":");  o.print(e.step);
    o.print(",\"msg\":\"");o.print(e.msg); o.print("\"");
    o.print("}");
  }
  o.println("]}");
}

static void printInputs(Print &o) {
  o.println("=== front panel ===");
  o.println("  GP  pin  name      level");
  for (uint i = 0; i < count_of(INPUTS); i++) {
    o.print("  ");   o.print(INPUTS[i].gp);
    o.print(INPUTS[i].gp < 10 ? "   " : "  ");
    o.print(INPUTS[i].board); o.print(INPUTS[i].board < 10 ? "    " : "   ");
    o.print(INPUTS[i].name);
    for (int k = (int)strlen(INPUTS[i].name); k < 10; k++) o.print(' ');
    o.println(gpio_get(INPUTS[i].gp) ? "HIGH (open)" : "LOW  (closed)");
  }
  o.println("  All inputs idle HIGH -- pulled up. LOW means the contact is made,");
  o.println("  so a pin stuck LOW with nothing pressed is shorted to ground.");
  uint32_t first = (inLogSeq > INLOG_MAX) ? (inLogSeq - INLOG_MAX) : 0;
  o.print("  "); o.print(inLogSeq - first); o.print(" recent events (seq ");
  o.print(first); o.print(".."); o.print(inLogSeq); o.println(")");
}

static void handleInputs() {
  uint32_t since = 0;
  if (server.hasArg("since"))
    since = (uint32_t)strtoul(server.arg("since").c_str(), nullptr, 10);
  StringPrint sp;
  printInputsJson(sp, since);
  server.send(200, "application/json", sp.s);
}

static void handleState() {
  StringPrint sp;
  printJson(sp);
  server.send(200, "application/json", sp.s);
}

static void netSetup() {
  if (!strlen(WIFI_SSID)) return;          // no secrets.h -> stay offline
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(MDNS_NAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// Non-blocking: a garage device must keep generating pulses whether or not the
// access point is reachable, so nothing here ever waits for the network.
static void netPoll() {
  static uint32_t lastTry = 0;
  static bool     wasUp   = false;
  if (!strlen(WIFI_SSID)) return;

  bool up = (WiFi.status() == WL_CONNECTED);

  if (up && !wasUp) {
    Serial.print("wifi: ");   Serial.print(WiFi.SSID());
    Serial.print("  http://"); Serial.print(WiFi.localIP()); Serial.println("/");
    if (!serverUp) {
      server.on("/", []{ server.send_P(200, "text/html", INDEX_HTML); });
      server.on("/cmd",   handleCmd);
      server.on("/state", handleState);
      server.on("/inputs", handleInputs);
      server.onNotFound([]{ server.send(404, "text/plain", "no"); });
      server.begin();
      serverUp = true;
    }
    MDNS.begin(MDNS_NAME);
    MDNS.addService("http", "tcp", 80);
    Serial.print("mdns: http://"); Serial.print(MDNS_NAME); Serial.println(".local/");
  }
  if (!up && wasUp) Serial.println("wifi: link lost");
  wasUp = up;

  if (up) {
    if (serverUp) server.handleClient();
    MDNS.update();
  } else if (millis() - lastTry > 15000) {
    lastTry = millis();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

// ============================================================================
// Setup / loop
// ============================================================================

void setup() {
  Serial.begin(115200);                 // USB CDC — baud is ignored

  for (uint i = 0; i < count_of(INPUTS); i++) {
    gpio_init(INPUTS[i].gp);
    gpio_set_dir(INPUTS[i].gp, GPIO_IN);
    gpio_pull_up(INPUTS[i].gp);            // pull-UP throughout: RP2350 erratum E9
  }                                     // affects pads using the internal pull-down.
  gpio_init(PIN_STATUS_LED);
  gpio_set_dir(PIN_STATUS_LED, GPIO_OUT);

  // Not in INPUTS[] on purpose. That table drives the front-panel event log,
  // which is deliberately undebounced and logs every edge -- an interlock
  // chattering during a run would flood it and bury the actual trip message.
  gpio_init(PIN_INTERLOCK);
  gpio_set_dir(PIN_INTERLOCK, GPIO_IN);
  gpio_pull_up(PIN_INTERLOCK);

  pioReady = pio_claim_free_sm_and_add_program(&gating_program,
                                               &pioGate, &smGate, &offGating)
          && pio_claim_free_sm_and_add_program(&gate_gen_program,
                                               &pioGen, &smGen, &offGen);
  if (pioReady) {
    int sc = pio_claim_unused_sm(pioGen, false);
    if (sc < 0) pioReady = false; else smCarrier = (uint)sc;
  }

  // Elongation's own SM + DMA channel. Independent of pioReady above --
  // a claim failure here just leaves elongation unavailable (ELONGATE errors
  // out) rather than taking down the rest of the generator.
  elongPioReady = pio_claim_free_sm_and_add_program(&elongate_program,
                                                     &pioElong, &smElong, &offElong);
  if (elongPioReady) {
    elongDmaChan = dma_claim_unused_channel(false);
    if (elongDmaChan < 0) {
      elongPioReady = false;
    } else {
      dma_channel_set_irq0_enabled(elongDmaChan, true);
      irq_set_exclusive_handler(DMA_IRQ_0, elongDmaIrqHandler);
      irq_set_enabled(DMA_IRQ_0, true);
    }
  }

  freqCounterInit();

  encState  = (uint8_t)((gpio_get(PIN_ENC_A)  ? 1 : 0) | (gpio_get(PIN_ENC_B)  ? 2 : 0));
  enc2State = (uint8_t)((gpio_get(PIN_ENC2_A) ? 1 : 0) | (gpio_get(PIN_ENC2_B) ? 2 : 0));
  enc3State = (uint8_t)((gpio_get(PIN_ENC3_A) ? 1 : 0) | (gpio_get(PIN_ENC3_B) ? 2 : 0));
  gateMode = gpio_get(PIN_MODE_SW) ? MODE_COUNT : MODE_TIME;
  applyGate();

  netSetup();

  Serial.println();
  Serial.println("GatedPulsePico - PIO gated pulse train");
  Serial.println("  GP2 carrier   GP3 SIG IN (3.3 V ONLY)   GP4 gate   GP5 gated out");
  Serial.println("  GP11/12/13 carrier freq/T1 encoder+SW   GP14/16/17 carrier duty/T2 encoder+SW");
  if (!pioReady) Serial.println("!! PIO claim FAILED - no free state machines/instruction space");
  if (!elongPioReady) Serial.println("!! elongation PIO/DMA claim FAILED - ELONGATE unavailable");

  // Restore whatever was last saved, then bring the output up in one go. This
  // is what makes the box standalone: the PIO claims above have already run, so
  // elongation/SEQ/TRAIN come back armed rather than silently downgraded.
  // A board that has never been saved to falls through with its compiled-in
  // defaults, which is the correct first-boot behaviour.
  if (cfgRead(true)) {
    Serial.println("restored saved settings");
    if (cfgBootMode == BOOT_RUN)      Serial.println("  boot mode RUN  - output forced on");
    else if (cfgBootMode == BOOT_OFF) Serial.println("  boot mode OFF  - output forced off");
  } else {
    Serial.println("no saved settings - using defaults (SAVE to store the current state)");
  }
  cfgReady = true;
  applyGate();
  cfgDirtyAt = 0;             // applyGate() just armed a save of what we loaded

  printState(Serial);
  Serial.println("Type ? for commands.");
}

void loop() {
  // Nothing here is timing-critical: PIO owns the waveform and keeps running
  // whatever the CPU does. That is the whole payoff of the port — on the Nano
  // this loop *was* the waveform.
  pollInterlock();           // safety first, literally: before any input that
                             // could ask the output to come back on
  pollInputWatch();          // raw edges first, so a step is always
  pollEncoder();             // preceded in the log by the edges that
  pollEncoder2();            // produced it
  pollEncoder3();
  pollButtons();
  pollSerial();
  freqPoll();
  netPoll();
  pollRamp();
  cfgPoll(Serial);            // writes at most once per editing session

  // Close a one-shot RUN window. Signed compare so it survives millis() wrap.
  if (runUntilMs && (int32_t)(millis() - runUntilMs) >= 0) {
    runUntilMs = 0;
    outputEnabled = false;
    applyGate();
    Serial.print("RUN complete ("); Serial.print(runLenMs); Serial.println(" ms)");
  }

  gpio_put(PIN_STATUS_LED, outputLive() && gpio_get(PIN_GATE_OUT));
}
