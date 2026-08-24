// Onboard web UI, served from PROGMEM at "/". Its own file so the page can
// grow without burying the sketch, and so "what changed in the UI" never
// tangles with "what changed in the generator".
//
// Contract with the sketch: this file defines INDEX_HTML and nothing else.
// Every control goes through the two existing endpoints -- GET /cmd?c=<cmd>
// (text) and GET /state (JSON) -- so the page can never reach state the serial
// console cannot, and adding a control is just adding a button that types.
//
// Self-contained on purpose: no fonts, no CDN, no external anything. A phone
// in a garage with no internet has to render it whole.
//
// Two structural decisions worth keeping:
//
//   1. PARAMS is the single source of truth for every continuous control --
//      range, formatting, how to read it out of /state, what command to send,
//      how a nudge steps. The faders and their exact-entry boxes are BUILT
//      from it at load. Adding a knob means adding a row there, not writing
//      another slab of near-identical HTML and another poll branch to forget.
//
//   2. Dirty windows, not focus checks. The poll refills every control from
//      the board every 2 s, which fought manual edits: an activeElement test
//      does not survive a <select>, and mobile blurs inputs mid-gesture. Any
//      touch now stamps its control GROUP, and the poll leaves that group
//      alone until it has been quiet. Same bug, same fix, as the dashboard.
#pragma once

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta name=viewport content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name=color-scheme content=dark>
<title>Gated Pulse</title><style>
:root{--bg:#0d1117;--card:#161b22;--card2:#1c2230;--line:#30363d;--fg:#c9d1d9;
      --dim:#8b949e;--go:#2ea043;--warn:#bb8009;--acc:#2f81f7;--red:#da3633;
      --mono:ui-monospace,SFMono-Regular,Menlo,monospace}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--fg);
  font:14px/1.4 system-ui,-apple-system,sans-serif;-webkit-text-size-adjust:100%}
body{padding-bottom:70px}

/* ---- sticky header: state you must never lose while on another tab ---- */
header{position:sticky;top:0;z-index:9;background:#0d1117ee;
  backdrop-filter:blur(8px);border-bottom:1px solid var(--line);
  padding:8px 10px calc(8px + env(safe-area-inset-bottom,0px))}
.hrow{display:flex;gap:10px;align-items:stretch}
#pw{flex:0 0 108px;border:0;border-radius:10px;font:600 15px/1.1 system-ui;
  color:#fff;background:#30363d;cursor:pointer;padding:10px 4px;letter-spacing:.4px}
#pw.on{background:var(--go);box-shadow:0 0 0 1px #3fb95055,0 0 14px #2ea04355}
/* A tripped interlock is not the same as "stopped" and must not look like it:
   stopped is something you chose, tripped is the enclosure refusing. */
#pw.trip{background:var(--red);box-shadow:0 0 0 1px #e5484d55,0 0 14px #e5484d55}
#pw small{display:block;font-weight:400;font-size:10px;opacity:.85;margin-top:2px}
.hstat{flex:1;min-width:0;display:flex;flex-direction:column;justify-content:center;gap:5px}
.chips{display:flex;flex-wrap:wrap;gap:4px}
.chip{font:500 11px/1 var(--mono);padding:4px 7px;border-radius:999px;
  border:1px solid var(--line);color:var(--dim);white-space:nowrap}
.chip.k{background:var(--card2);color:var(--fg)}
.chip.on{background:var(--go);color:#fff;border-color:transparent}
.chip.md{background:var(--acc);color:#fff;border-color:transparent}
.chip.wr{background:var(--warn);color:#fff;border-color:transparent}
.hact{display:flex;gap:5px}
.hact button{flex:1;padding:6px 4px;font-size:12px}

/* ---- tabs ---- */
nav{display:flex;gap:4px;overflow-x:auto;padding:8px 10px 0;
  border-bottom:1px solid var(--line);scrollbar-width:none}
nav::-webkit-scrollbar{display:none}
nav button{border:1px solid transparent;border-bottom:0;background:none;color:var(--dim);
  padding:8px 13px;border-radius:8px 8px 0 0;font-size:13px;cursor:pointer;white-space:nowrap}
nav button.on{background:var(--card);border-color:var(--line);color:var(--fg);font-weight:600}
main{padding:10px}
section{display:none}section.on{display:block}

/* ---- cards ---- */
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;
  padding:10px 12px;margin:0 0 10px}
.card>h2{font:600 12px/1 system-ui;letter-spacing:.5px;text-transform:uppercase;
  color:var(--dim);margin:0 0 9px}
.note{font-size:11.5px;color:var(--dim);margin:8px 0 0}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:0 10px;
  align-items:start}

/* ---- vertical faders. Big on purpose: this gets driven with a thumb, often
       one-handed, sometimes with the other hand on a probe. ---- */
.bank{display:flex;gap:8px;flex-wrap:wrap;justify-content:flex-start}
.fd{display:flex;flex-direction:column;align-items:center;gap:6px;
  background:var(--card2);border:1px solid var(--line);border-radius:10px;
  padding:9px 7px;min-width:96px}
.fd>b{font:600 13px/1.2 var(--mono);color:#fff;text-align:center;min-height:16px}
input[type=range].v{-webkit-appearance:slider-vertical;appearance:slider-vertical;
  writing-mode:vertical-lr;direction:rtl;width:54px;height:250px;
  margin:0;padding:0;background:transparent;accent-color:var(--acc);cursor:ns-resize}
.fl{font-size:11px;color:var(--dim);text-transform:uppercase;letter-spacing:.4px}
.nz{display:flex;gap:4px;width:100%}
.nz button{flex:1;padding:7px 0;font:600 15px/1 var(--mono);min-height:34px}
.fd input[type=number],.fd input[type=text]{width:100%;text-align:center;
  font:12px var(--mono);padding:5px 2px}

/* ---- generic controls ---- */
button{background:#21262d;color:var(--fg);border:1px solid var(--line);border-radius:7px;
  padding:9px 12px;cursor:pointer;font-size:13px;min-height:36px;-webkit-tap-highlight-color:transparent}
button:active{transform:translateY(1px)}
button.go{background:var(--go);color:#fff;border-color:transparent}
button.acc{background:var(--acc);color:#fff;border-color:transparent}
button.warn{background:var(--warn);color:#fff;border-color:transparent}
button.red{background:var(--red);color:#fff;border-color:transparent}
button[disabled]{opacity:.35;cursor:not-allowed;filter:grayscale(1)}
button.sel{background:var(--acc);color:#fff;border-color:transparent;font-weight:600}
input,select{background:#0d1117;color:var(--fg);border:1px solid var(--line);
  border-radius:7px;padding:9px;font-size:14px;min-height:36px}
input[type=number],input[type=text]{width:110px}
input.wide{width:100%}
.row{display:flex;flex-wrap:wrap;gap:6px;align-items:center;margin:6px 0}
.row>label{color:var(--dim);font-size:12px;min-width:96px}
.seg{display:flex;gap:0;border:1px solid var(--line);border-radius:8px;overflow:hidden}
.seg button{border:0;border-radius:0;flex:1;border-right:1px solid var(--line)}
.seg button:last-child{border-right:0}
.kv{font:12px var(--mono);color:var(--dim)}.kv b{color:var(--fg)}
.pins{display:grid;grid-template-columns:repeat(auto-fit,minmax(112px,1fr));gap:6px}
.pin{background:var(--card2);border:1px solid var(--line);border-radius:8px;
     padding:7px 8px;font:11px var(--mono);transition:background .12s,border-color .12s}
.pin .nm{color:var(--fg);font-weight:600;font-size:12px}
.pin .gp{color:var(--dim)}
.pin .lv{float:right;font-weight:600}
.pin.hi .lv{color:var(--dim)}
.pin.lo{background:#1d3a24;border-color:var(--go)}
.pin.lo .lv{color:#7ee787}
.pin.hit{background:var(--acc);border-color:transparent}
.pin.hit .nm,.pin.hit .gp,.pin.hit .lv{color:#fff}

/* ---- output ---- */
pre{background:#010409;border:1px solid var(--line);padding:9px;border-radius:9px;
  overflow-x:auto;font:12px/1.45 var(--mono);color:#7ee787;white-space:pre-wrap;
  max-height:230px;overflow-y:auto;margin:0}
#toast{position:fixed;left:50%;bottom:14px;transform:translateX(-50%) translateY(70px);
  background:#21262dfa;border:1px solid var(--line);color:var(--fg);padding:9px 15px;
  border-radius:999px;font:12px var(--mono);max-width:88vw;white-space:nowrap;
  overflow:hidden;text-overflow:ellipsis;transition:transform .18s;z-index:20;
  pointer-events:none}
#toast.up{transform:translateX(-50%) translateY(0)}
#toast.bad{border-color:var(--red);color:#ffb3ae}

/* Only one gating block is real at a time; showing both invites setting the
   one that is not running and wondering why nothing changed. */
body.count .timeonly,body.time .countonly{display:none}
</style></head><body class=count>

<header>
 <div class=hrow>
  <button id=pw onclick="tglE()">OUTPUT<small id=pws>&mdash;</small></button>
  <div class=hstat>
   <div class=chips id=hkv></div>
   <div class=chips id=hmode></div>
   <div class=hact>
    <button onclick="c('SHOT')">single burst</button>
    <button class=acc onclick="c('SAVE')">save</button>
    <button class=warn onclick="c('R')" title="100 kHz / 50%, 10 on / 90 off, internal carrier, offset x3 so GP18-20 and GP22 are all live, output ON">known state</button>
   </div>
  </div>
 </div>
</header>

<nav id=tabs></nav>
<main>

<section data-tab=carrier>
 <div class=grid>
  <div class=card><h2>carrier</h2>
   <div class=bank id=bank_carrier></div>
   <p class=note>Four views of one pair &mdash; move frequency and T1/T2 follow,
    move either time and frequency follows. Faders send on release; the boxes
    set an exact value.</p>
  </div>
  <div>
   <div class=card><h2>source</h2>
    <div class="row seg">
     <button id=src_int onclick="c('SRC INT')">internal GP2</button>
     <button id=src_ext onclick="c('SRC EXT')">external GP3</button>
    </div>
    <p class=note>GP3 is <b>3.3 V only</b> and not 5 V tolerant.</p>
   </div>
   <div class=card><h2>panel knobs edit</h2>
    <div class="row seg">
     <button id=shp_fd onclick="c('SHAPE FD')">freq + duty</button>
     <button id=shp_t12 onclick="c('SHAPE T12')">T1 + T2</button>
    </div>
   </div>
   <div class=card><h2>jump to</h2>
    <div class=row id=fpre></div>
   </div>
  </div>
 </div>
</section>

<section data-tab=burst>
 <div class=grid>
  <div class=card><h2>gating mode</h2>
   <div class="row seg">
    <button id=m_count onclick="c('M 1')">COUNT &mdash; pulses</button>
    <button id=m_time onclick="c('M 0')">TIME &mdash; window</button>
   </div>
   <p class=note>COUNT gates whole pulses, so a burst is always exactly N of
    them. TIME opens a fixed window regardless of where the carrier is.</p>
  </div>
  <div class="card countonly"><h2>pulses per burst</h2>
   <div class=bank id=bank_count></div>
  </div>
  <div class="card timeonly"><h2>gate window</h2>
   <div class=bank id=bank_time></div>
  </div>
  <div class=card><h2>one-shot</h2>
   <div class=row>
    <button onclick="c('SHOT')">one burst</button>
    <input id=runms type=number value=1000 min=1><button onclick="c('RUN '+v('runms'))">run ms</button>
    <button class=red onclick="c('STOP')">stop</button>
   </div>
  </div>
  <div class=card><h2>polarity</h2>
   <div class=row>
    <button onclick="c('I')">invert gate</button>
    <button onclick="c('V')">invert pulses</button>
   </div>
  </div>
 </div>
</section>

<section data-tab=channels>
 <div class=grid>
  <div class=card><h2>output mode</h2>
   <div class="row seg">
    <button id=ph_off onclick="c('PHASE OFF')">single</button>
    <button id=ph_rot onclick="c('PHASE ROT '+v('phn'))">offset</button>
    <button id=ph_sync onclick="c('PHASE SYNC '+v('phn'))">sync</button>
   </div>
   <div class=row><label>channels</label>
    <select id=phn onchange="mark('ph')">
     <option>1</option><option>2</option><option selected>3</option></select>
    <button onclick="c('PHASE DUMP')">dump tables</button>
   </div>
   <p class=note id=phnote></p>
  </div>
  <div class=card><h2>pins</h2>
   <p class=note>
    <b>GP5</b> pin 7 &mdash; every pulse, every mode. Always a valid monitor.<br>
    <b>GP4</b> pin 6 &mdash; high for every burst.<br>
    <b>GP18/19/20</b> pins 24/25/26 &mdash; the channels.<br>
    <b>GP22</b> pin 29 &mdash; cycle marker, high for burst 1 of the pattern
    only. Trigger here with holdoff <b>off</b> and the pattern sits still.<br>
    <b>GP2</b> pin 4 &mdash; raw carrier tap.</p>
  </div>
 </div>
</section>

<section data-tab=shape>
 <div class=grid>
  <div class=card><h2>elongation &mdash; growth within one burst</h2>
   <div class=bank id=bank_elong></div>
   <div class=row>
    <button class=go onclick="sendFade('elr')">arm ratio</button>
    <button onclick="c('ELONGATE PHI')">golden ratio</button>
    <button onclick="c('ELONGATE OFF')">off</button>
   </div>
   <p class=note>Total span of the last pulse over the first. Above 1 grows,
    below 1 shrinks. Pulse 1 stays at the current T1/T2.</p>
  </div>
  <div>
   <div class=card><h2>sweep &mdash; T1 stepped between bursts</h2>
    <div class=row><label>step (us)</label>
     <input id=r1s type=number value=0.2 step=0.1 oninput="mark('r1')"></div>
    <div class=row><label>every N bursts</label>
     <input id=r1b type=number value=50 min=1 oninput="mark('r1')"></div>
    <div class=row><label>limit (us)</label>
     <input id=r1l type=number value=9 step=0.1 oninput="mark('r1')"></div>
    <div class=row><label>at the limit</label>
     <select id=r1w onchange="mark('r1')"><option>WRAP</option><option>STOP</option></select></div>
    <div class=row>
     <button class=go onclick="c('RAMP1 '+v('r1s')+' '+v('r1b')+' '+v('r1l')+' '+v('r1w'))">arm sweep</button>
     <button onclick="c('RAMP1 OFF')">off</button>
    </div>
    <p class=note>A resonance sweep across bursts &mdash; distinct from
     elongation, which reshapes pulses inside one burst.</p>
   </div>
   <div class=card><h2>seq &mdash; explicit per-pulse table</h2>
    <div class=row><input id=sq class=wide type=text placeholder="1,1 2,1 4,1"
      oninput="mark('sq')"></div>
    <div class=row>
     <button class=go onclick="c('SEQ '+v('sq'))">arm</button>
     <button onclick="c('SEQ')">print</button>
     <button onclick="c('SEQ OFF')">off</button>
    </div>
    <p class=note>Each step is <code>t1_us,t2_us[,amp]</code>, space separated.
     Replaces the elongation ratio when loaded. Max 25 steps.</p>
   </div>
  </div>
 </div>
</section>

<section data-tab=train>
 <div class=card><h2>train &mdash; per-burst amplitude ring</h2>
  <div class=row>
   <label>bursts in ring</label>
   <select id=trn onchange="mark('train');buildTrain(+this.value);armTrain()">
    <option>2</option><option>3</option><option>4</option><option>5</option>
    <option selected>6</option><option>7</option><option>8</option></select>
   <button class=go onclick="armTrain()">arm ring</button>
   <button onclick="c('TRAIN OFF')">off</button>
  </div>
  <div class=bank id=bank_train></div>
  <div class=row>
   <button onclick="setTrain([100,75,50,25])">ramp down</button>
   <button onclick="setTrain([25,50,75,100])">ramp up</button>
   <button onclick="setTrain([100,25,100,50,100,75])">reference x6</button>
   <button onclick="setTrain(Array(+v('trn')).fill(100))">all 100</button>
  </div>
  <p class=note>Percent of the base T1 for each burst in turn, then repeats.
   For an inductive primary the current at turn-off is V&middot;T1/L, so
   narrowing T1 attenuates that whole burst without changing its shape.
   Composes with elongation and seq &mdash; this scales, they shape.</p>
 </div>
</section>

<section data-tab=inputs>
 <div class=card><h2>front panel &mdash; live wiring check</h2>
  <p class=note>Every input idles <b>HIGH</b> (internal pull-up). <b>LOW</b> means
   the contact is made. Turn a knob and watch its A and B cells blink:
   <b>neither blinks</b> = wiring or a dry joint; <b>both blink but no STEP
   appears</b> = A and B are swapped; <b>stuck LOW untouched</b> = shorted to
   ground. Board pin numbers are the ones you count on the header.</p>
  <div id=pins class=pins></div>
 </div>
</section>

<section data-tab=sensors>
 <div class=card><h2>sensor node</h2>
  <div class=row>
   <label>host</label>
   <input id=snhost type=text placeholder="gatedsensor.local" style="width:180px"
          onchange="saveSn()">
   <button onclick="pollSensors(1)">read now</button>
   <button onclick="snCmd('SCAN')">rescan I2C</button>
   <span class=kv id=snstat></span>
  </div>
  <p class=note>A second board, polled by your browser directly &mdash; not
   relayed through this one, so a sensor fault cannot disturb the waveform.
   Wire its <b>GP2 (pin 4)</b> to this board's <b>GP22 (pin 29)</b> plus a
   ground, and it locks to the cycle marker.</p>
 </div>
 <div class=card><h2>cycle-marker lock</h2><div class=chips id=snsync></div></div>
 <div class=card><h2>analog</h2>
  <div class=chips id=snadc></div>
  <p class=note>Raw millivolts from the on-chip ADC. <b>Not calibrated current</b>
   &mdash; the real path is an external 16-bit converter behind an analog
   peak-hold.</p>
 </div>
 <div class=card><h2>I&sup2;C bus</h2><pre id=sni2c>&mdash;</pre></div>
</section>

<section data-tab=sys>
 <div class=grid>
  <div class=card><h2>save &amp; power-on</h2>
   <div class=row>
    <button class=acc onclick="c('SAVE')">save now</button>
    <button onclick="c('LOAD')">reload saved</button>
    <button onclick="c('FORGET')">forget</button>
   </div>
   <div class=row><label>at power-on</label></div>
   <div class="row seg">
    <button id=bm_saved onclick="c('BOOT SAVED')">as saved</button>
    <button id=bm_run onclick="c('BOOT RUN')">always run</button>
    <button id=bm_off onclick="c('BOOT OFF')">always off</button>
   </div>
   <div class="row seg">
    <button id=as_1 onclick="c('AUTOSAVE 1')">autosave on</button>
    <button id=as_0 onclick="c('AUTOSAVE 0')">autosave off</button>
   </div>
   <p class=note>Autosave writes ~5 s after the last change. That is what makes
    a ring you armed once come back every power-on &mdash; clear what you do not
    want persisted.</p>
  </div>
  <div>
   <div class=card><h2>console</h2>
    <div class=row><input id=cl class=wide type=text
      placeholder="any serial command, Enter to send"
      onkeydown="if(event.key=='Enter'){c(this.value);this.select()}"></div>
    <div class=row>
     <button onclick="c('P')">state</button>
     <button onclick="c('J')">json</button>
     <button onclick="c('DIAG')">diag</button>
     <button onclick="c('?')">help</button>
    </div>
   </div>
   <div class=card><h2>HV enclosure interlock</h2>
    <div class=chips id=lockchips></div>
    <div class=row>
     <button id=lockbtn onclick="tglLock()">&mdash;</button>
     <button id=armbtn class=go onclick="c('ARM')">ARM</button>
    </div>
    <p class=note>Wire a normally-closed switch or a Hall sensor between
     <b>GP26 (pin 31)</b> and <b>GND (pin 33)</b>: closed to ground = safe.
     A cut wire or an unplugged sensor reads open and cuts the output, which is
     the point &mdash; so leave this off until it is wired.
     Trips latch; you must ARM to clear.</p>
    <p class=note><b>This is a reminder, not a guard.</b> A magnet defeats it in
     seconds. The bleeder resistor and a meter check across the capacitor stay
     mandatory before anything goes inside.</p>
   </div>
   <div class=card><h2>danger</h2>
    <div class=row>
     <button class=warn onclick="ask('restore all defaults?','R')">defaults</button>
     <button class=red onclick="ask('reboot the board?','REBOOT')">reboot</button>
    </div>
   </div>
  </div>
 </div>
</section>

<div class=card id=logcard><h2>activity &mdash; panel + commands</h2>
 <div class=row>
  <button id=logbtn class=go onclick="toggleLog()">pause</button>
  <button onclick="evts=[];drawLog()">clear</button>
  <label style="min-width:auto"><input type=checkbox id=rawchk checked
    onchange="drawLog()" style="min-height:auto;width:auto"> raw edges</label>
  <span class=kv id=logstat></span>
 </div>
 <pre id=evlog>turn a knob or press a button&hellip;</pre>
</div>
<div class=card><h2>last command response</h2><pre id=out>loading&hellip;</pre></div>
</main>
<div id=toast></div>

<script>
const $=i=>document.getElementById(i), v=i=>$(i).value.trim();
let st=null;

/* ---- dirty windows -------------------------------------------------------
   The poll refills every control from the board. Any touch stamps its group,
   and the poll then leaves that group alone until it is quiet. A focus check
   is not enough: a <select> is not an INPUT, and mobile blurs mid-gesture. */
const dirty={}, HOLD=4000;
const mark=g=>dirty[g]=Date.now();
const clean=g=>!dirty[g]||Date.now()-dirty[g]>HOLD;

/* ---- formatting ---- */
const fHz=x=>x>=1e6?(x/1e6).toFixed(3)+' MHz':x>=1e3?(x/1e3).toFixed(3)+' kHz':x.toFixed(2)+' Hz';
const fUs=x=>x>=1e3?(x/1e3).toFixed(3)+' ms':x>=1?x.toFixed(3)+' us':(x*1000).toFixed(0)+' ns';
const fPc=x=>x.toFixed(2)+' %';
const fN =x=>String(Math.round(x));

/* ---- the parameter table -------------------------------------------------
   Every continuous control is one row here: its range, how to render it, how
   to read it back out of /state, what to send, and how a nudge steps. The
   faders and their entry boxes are built from this, so a new knob is a row
   rather than another slab of HTML plus another poll branch to forget.

   Log tracks by default, because most of these are decade ranges -- 1 Hz to
   5 MHz on a linear slider puts everything below 100 kHz in the bottom 2%.
   `lin:1` opts out, and duty needs it: a percentage is BOUNDED, not a decade
   range, so a log track wastes half the travel on 0.05-1% and parks 1% at the
   midpoint. Bounded quantity -> linear; ranges spanning decades -> log. */
const PARAMS={
  freq:{lab:'freq', lo:1,    hi:5e6,  fmt:fHz, grp:'carr', mul:1.02,
        read:s=>s.carrier_ns?1e9/s.carrier_ns:0, send:x=>['C '+x.toFixed(4)]},
  duty:{lab:'duty', lo:0.05, hi:99.9, fmt:fPc, grp:'carr', add:0.5, lin:1,
        read:s=>s.carrier_duty_ppm/1e4,          send:x=>['CD '+x.toFixed(2)]},
  t1:  {lab:'T1',   lo:0.05, hi:1e5,  fmt:fUs, grp:'carr', mul:1.02,
        read:s=>s.carrier_ns/1000*s.carrier_duty_ppm/1e6, send:x=>pair(x,val('t2'))},
  t2:  {lab:'T2',   lo:0.05, hi:1e5,  fmt:fUs, grp:'carr', mul:1.02,
        read:s=>s.carrier_ns/1000*(1-s.carrier_duty_ppm/1e6), send:x=>pair(val('t1'),x)},
  on:  {lab:'on',   lo:1,    hi:1000, fmt:fN,  grp:'burst', add:1, int:1,
        read:s=>s.on,  send:x=>['ON '+Math.round(x)]},
  off: {lab:'off',  lo:1,    hi:10000,fmt:fN,  grp:'burst', add:1, int:1,
        read:s=>s.off, send:x=>['OFF '+Math.round(x)]},
  gper:{lab:'period',lo:1,   hi:1e6,  fmt:fUs, grp:'gate', mul:1.02,
        read:s=>s.period_ns/1000, send:x=>['T '+x.toFixed(3)]},
  gdty:{lab:'duty', lo:0.05, hi:99.9, fmt:fPc, grp:'gate', add:0.5, lin:1,
        read:s=>s.duty_ppm/1e4,   send:x=>['D '+x.toFixed(2)]},
  elr: {lab:'ratio',lo:0.01, hi:1000, fmt:x=>x.toFixed(3)+' x', grp:'elong', mul:1.02,
        read:s=>s.elong_ratio,    send:x=>['ELONGATE '+x.toFixed(4)]},
};

/* T1 and T2 have no firmware command of their own -- the pair IS the carrier,
   so it maps exactly onto C + CD. CD resolves 0.01%, which loses nothing here. */
const pair=(t1,t2)=>['C '+(1e6/(t1+t2)).toFixed(4), 'CD '+(100*t1/(t1+t2)).toFixed(2)];

const lg=x=>Math.log10(x);
const clamp=(p,x)=>Math.max(p.lo,Math.min(p.hi,x));
const toSlider=(p,x)=>{
  x=clamp(p,x);
  const f=p.lin ? (x-p.lo)/(p.hi-p.lo)
                : (lg(x)-lg(p.lo))/(lg(p.hi)-lg(p.lo));
  return Math.max(0,Math.min(1000,Math.round(1000*f)));
};
const fromSlider=(p,s)=>{
  const x=p.lin ? p.lo+(s/1000)*(p.hi-p.lo)
                : 10**(lg(p.lo)+(s/1000)*(lg(p.hi)-lg(p.lo)));
  return p.int?Math.max(p.lo,Math.round(x)):x;
};
const val=k=>fromSlider(PARAMS[k], +$('f_'+k).value);

function faderHTML(k){
  const p=PARAMS[k];
  return '<div class=fd><b id="v_'+k+'">&ndash;</b>'
    +'<input type=range class=v id="f_'+k+'" min=0 max=1000 value=0 '
    +'oninput="onFade(\''+k+'\')" onchange="sendFade(\''+k+'\')">'
    +'<div class=nz><button onclick="nudge(\''+k+'\',-1)">&minus;</button>'
    +'<button onclick="nudge(\''+k+'\',1)">+</button></div>'
    +'<input id="n_'+k+'" type=number step=any oninput="mark(\''+p.grp+'\')" '
    +'onchange="setExact(\''+k+'\')">'
    +'<span class=fl>'+p.lab+'</span></div>';
}
const bank=(id,keys)=>$(id).innerHTML=keys.map(faderHTML).join('');

// Drag: update the label and the box live, but do not send until release --
// a drag would otherwise spray a hundred commands at the board.
function onFade(k){
  const p=PARAMS[k], x=val(k);
  mark(p.grp);
  $('v_'+k).textContent=p.fmt(x);
  $('n_'+k).value=p.int?Math.round(x):+x.toPrecision(6);
  if(k=='freq'||k=='duty') mirrorTimes();
  if(k=='t1'||k=='t2')     mirrorFreq();
}
function mirrorTimes(){
  const per=1e6/val('freq'), d=val('duty')/100;
  set('t1',per*d); set('t2',per*(1-d));
}
function mirrorFreq(){
  const t1=val('t1'), t2=val('t2');
  set('freq',1e6/(t1+t2)); set('duty',100*t1/(t1+t2));
}
function set(k,x){
  const p=PARAMS[k];
  $('f_'+k).value=toSlider(p,x);
  $('v_'+k).textContent=p.fmt(x);
  $('n_'+k).value=p.int?Math.round(x):+x.toPrecision(6);
}
function sendFade(k){ PARAMS[k].send(val(k)).forEach(cmd=>c(cmd)); }
function nudge(k,dir){
  const p=PARAMS[k];
  let x=val(k);
  x = p.mul ? (dir>0?x*p.mul:x/p.mul) : x+dir*p.add;
  x = Math.max(p.lo,Math.min(p.hi,x));
  mark(p.grp); set(k,x);
  if(k=='freq'||k=='duty') mirrorTimes(); if(k=='t1'||k=='t2') mirrorFreq();
  sendFade(k);
}
function setExact(k){
  const p=PARAMS[k], x=parseFloat($('n_'+k).value);
  if(!isFinite(x)) return;
  mark(p.grp); set(k,Math.max(p.lo,Math.min(p.hi,x)));
  if(k=='freq'||k=='duty') mirrorTimes(); if(k=='t1'||k=='t2') mirrorFreq();
  sendFade(k);
}

/* ---- train ring ----
   These sliders send on release, like every other control here. They used to
   need a separate "arm ring" click, which made them the one place where the
   screen could hold values the firmware had never been told about -- and the
   dirty window is a 4 s timeout, not a commit, so once it lapsed the poller
   found sliders disagreeing with /state and dutifully overwrote them. The edit
   vanished several seconds after it was made, which reads as the board
   fighting you rather than as a value that was never sent.
   Sending on change removes the disagreement instead of papering over it. */
function buildTrain(n,vals){
  const keep=[...document.querySelectorAll('.trs')].map(e=>+e.value);
  $('bank_train').innerHTML=Array.from({length:n},(_,i)=>{
    const x=(vals&&vals[i])||keep[i]||100;
    return '<div class=fd><b>'+x+' %</b>'
      +'<input type=range class="v trs" min=1 max=100 value='+x
      +' oninput="mark(\'train\');this.previousElementSibling.textContent=this.value+\' %\'"'
      +' onchange="armTrain()">'
      +'<span class=fl>burst '+(i+1)+'</span></div>';
  }).join('');
}
const trainVals=()=>[...document.querySelectorAll('.trs')].map(e=>+e.value);
// setTrain is the USER path (presets, ring resize): render, then push.
// The poller calls buildTrain() directly so its own refresh neither re-marks
// the group nor echoes a command back at the board.
const setTrain=a=>{$('trn').value=a.length;buildTrain(a.length,a);armTrain()};
// mark() here and not only on the slider: the hold has to be refreshed by the
// SEND, or a slow drag can lapse the window before the command even goes out.
const armTrain=()=>{mark('train');c('TRAIN '+trainVals().join(' '))};

/* ---- transport ---- */
let toastT=null;
function toast(msg,bad){
  const t=$('toast');
  t.textContent=msg; t.className='up'+(bad?' bad':'');
  clearTimeout(toastT); toastT=setTimeout(()=>t.className='',2600);
}
async function c(cmd){
  if(!cmd) return;
  try{
    const r=await fetch('/cmd?c='+encodeURIComponent(cmd));
    const txt=await r.text();
    $('out').textContent=txt||'(no output)';
    const first=(txt||'ok').split('\n')[0].slice(0,70);
    toast(first, /error/i.test(txt));
  }catch(e){ toast('request failed',1); }
  poll();
}
const ask=(q,cmd)=>{ if(confirm(q)) c(cmd); };
const tglE=()=>c(st&&st.enabled?'E 0':'E 1');
const tglLock=()=>c(st&&st.interlock?'LOCK 0':'LOCK 1');
function paintLock(){
  const on=!!st.interlock, shut=!!st.interlock_closed, trip=!!st.interlock_tripped;
  $('lockchips').innerHTML=
     chip(on?'interlock ON':'interlock OFF',on?'md':'k')
    +chip('GP'+st.interlock_pin+(shut?' closed':' OPEN'),shut?'k':'wr')
    +(on?chip(trip?'TRIPPED':'armed',trip?'wr':'md'):'')
    +(st.interlock_trips?chip(st.interlock_trips+' trip'+(st.interlock_trips>1?'s':'')
                              +' since boot','k'):'');
  $('lockbtn').textContent=on?'disable interlock':'enable interlock';
  $('lockbtn').className=on?'warn':'';
  // Disabled rather than hidden: a greyed ARM you cannot press explains why
  // better than an ARM that vanishes.
  $('armbtn').disabled=!(on&&trip&&shut);
}

/* ---- tabs ---- */
const TABS=[['carrier','carrier'],['burst','burst'],['channels','channels'],
            ['shape','shape'],['train','train'],['inputs','panel'],['sensors','sensors'],['sys','system']];
function showTab(id){
  document.querySelectorAll('section').forEach(s=>s.classList.toggle('on',s.dataset.tab==id));
  document.querySelectorAll('#tabs button').forEach(b=>b.classList.toggle('on',b.dataset.t==id));
  try{localStorage.setItem('gptab',id)}catch(e){}
}
$('tabs').innerHTML=TABS.map(([id,lab])=>
  '<button data-t="'+id+'" onclick="showTab(\''+id+'\')">'+lab+'</button>').join('');

/* ---- front-panel input log ----------------------------------------------
   Polled far faster than /state and only while its tab is visible: encoder
   edges arrive in milliseconds, so a 2 s poll would show a handful of them and
   miss the bounce entirely -- and bounce is one of the things being looked for.
   The board keeps the ring; we only ask for what is new via `since`. */
let evts=[], logSeq=0, logOn=true, logTimer=null;
const GPNAME={};
function toggleLog(){
  logOn=!logOn;
  $('logbtn').textContent=logOn?'pause':'resume';
  $('logbtn').className=logOn?'go':'';
}
function drawLog(){
  const el=$('evlog'), raw=$('rawchk').checked;
  const rows=evts.filter(e=>raw||e.kind!=0);
  if(!rows.length){el.textContent='turn a knob or press a button...';return}
  el.textContent=rows.slice(-260).map(e=>{
    const nm=GPNAME[e.gp]||('GP'+e.gp);
    const t=(e.ms/1000).toFixed(3).padStart(9);
    if(e.kind==2) return `${t}  ${'>>'.padEnd(9)} ${e.msg}`;
    if(e.kind==1) return `${t}  ${nm.padEnd(9)} STEP ${e.step>0?'+1':'-1'}`;
    return `${t}  ${nm.padEnd(9)}   ${e.level?'HIGH':'LOW '}`;
  }).join('\n');
  el.scrollTop=el.scrollHeight;
}
async function pollInputs(){
  // Runs on every tab, not just the panel one: the log is now the footer, and
  // the whole point is seeing what the panel did while you are looking at the
  // control it should have moved.
  if(!logOn) return;
  const vis=document.querySelector('section[data-tab=inputs]').classList.contains('on');
  let d; try{ d=await(await fetch('/inputs?since='+logSeq)).json() }catch(e){ return }
  const hit=new Set(d.events.filter(e=>e.kind==0).map(e=>e.gp));
  d.pins.forEach(p=>GPNAME[p.gp]=p.name);
  if(vis) $('pins').innerHTML=d.pins.map(p=>{
    const cls=hit.has(p.gp)?'hit':(p.level?'hi':'lo');
    return `<div class="pin ${cls}"><span class=lv>${p.level?'HIGH':'LOW'}</span>`
      +`<div class=nm>${p.name}</div><div class=gp>GP${p.gp} &middot; pin ${p.board}</div></div>`;
  }).join('');
  if(d.events.length){ evts=evts.concat(d.events).slice(-400); drawLog() }
  // A gap means the ring wrapped between polls -- say so rather than presenting
  // a continuous-looking log with events silently missing from the middle.
  const gap=d.events.length && logSeq && d.events[0].seq>logSeq;
  logSeq=d.seq;
  $('logstat').innerHTML=`seq <b>${d.seq}</b> &middot; ${evts.length} shown`
    +(gap?' &middot; <b>gap: events were overwritten between polls</b>':'');
}
setInterval(pollInputs,250);

/* ---- sensor node --------------------------------------------------------
   Fetched by the BROWSER from the other board, not proxied through this one.
   Proxying would couple the two, so a sensor problem could stall the generator
   -- exactly what running them on separate hardware was meant to avoid. */
function snHost(){
  let h=$('snhost').value.trim();
  if(!h){ try{h=localStorage.getItem('snhost')||''}catch(e){} }
  return h||'gatedsensor.local';
}
function saveSn(){ try{localStorage.setItem('snhost',$('snhost').value.trim())}catch(e){} }
async function snCmd(c){
  try{ await fetch(`http://${snHost()}/cmd?c=${encodeURIComponent(c)}`); toast('sensor: '+c) }
  catch(e){ toast('sensor unreachable',1) }
  pollSensors(1);
}
async function pollSensors(force){
  const vis=document.querySelector('section[data-tab=sensors]').classList.contains('on');
  if(!vis&&!force) return;
  let d;
  try{
    const r=await fetch(`http://${snHost()}/state`,{cache:'no-store'});
    d=await r.json();
  }catch(e){
    $('snstat').innerHTML='<b>unreachable</b>';
    $('snsync').innerHTML=chip('no answer from '+snHost(),'wr');
    $('snadc').innerHTML=''; $('sni2c').textContent='—';
    return;
  }
  $('snstat').innerHTML=`up <b>${d.uptime_s}s</b> &middot; ${d.wifi}`;
  $('snsync').innerHTML = d.sync_live
    ? chip('LOCKED','on')
      +chip('pattern '+(d.pattern_period_us/1000).toFixed(3)+' ms','k')
      +chip(d.pattern_hz.toFixed(2)+' Hz','k')
      +chip('marker '+(d.marker_high_us/1000).toFixed(3)+' ms','k')
      +chip('duty '+d.marker_duty_pct.toFixed(1)+'%','k')
      +chip('ring '+d.implied_ring_len,'md')
      +chip(d.sync_edges+' edges','k')
    : chip('NO SYNC — wire GP22 (pin 29) to its GP2 (pin 4) + ground','wr');
  $('snadc').innerHTML=(d.adc_mv||[]).map((v,i)=>
      chip('GP'+(26+i)+' '+v.toFixed(1)+' mV','k')).join('')
    +chip('chip '+d.chip_c.toFixed(1)+' °C','k');
  $('sni2c').textContent = d.i2c_count
    ? d.i2c.map(x=>`${x.addr}  ${x.guess}`).join('\n')
    : 'nothing on the bus — wire something and press rescan';
}
setInterval(pollSensors,2000);

/* ---- poll ---- */
const chip=(txt,cls)=>'<span class="chip '+(cls||'')+'">'+txt+'</span>';
const seg=(id,on)=>$(id)&&$(id).classList.toggle('sel',!!on);

async function poll(){
  try{ st=await(await fetch('/state')).json(); }catch(e){ return; }
  const f=st.carrier_ns?1e9/st.carrier_ns:0;
  const t1=st.carrier_ns/1000*st.carrier_duty_ppm/1e6;

  // "live", not "enabled": with the interlock open the operator's intent is
  // still ON while nothing is coming out, and a green button there would be a
  // lie told next to a high-voltage cell.
  $('pw').className=st.live?'on':(st.interlock_tripped?'trip':'');
  $('pws').textContent=st.live?'running':(st.interlock_tripped?'INTERLOCK':'stopped');
  paintLock();

  $('hkv').innerHTML=chip(fHz(f),'k')+chip(fPc(st.carrier_duty_ppm/1e4),'k')
    +chip('T1 '+fUs(t1),'k')
    +(st.mode=='COUNT'?chip(st.on+' on / '+st.off+' off','k')
                      :chip('gate '+fUs(st.period_ns/1000),'k'));

  const om=st.phase_mode=='OFF'?'single'
          :(st.phase_mode=='ROTATE'?'offset':'sync')+' x'+st.phase_len;
  $('hmode').innerHTML=chip(st.mode+' / '+st.src,'md')+chip(om,'md')
    +(st.ring_len>1?chip('ring '+st.ring_len+' - GP'+st.cycle_pin+' marks #1','md'):'')
    +(st.elongation?chip('elong '+st.elong_ratio+'x','wr'):'')
    +(st.sweep?chip('sweep','wr'):'')
    +(st.seq_len?chip('seq '+st.seq_len,'wr'):'')
    +(st.train_len?chip('train '+st.train_len,'wr'):'')
    +chip(st.autosave?'autosave':'manual save', st.saved?'on':'');

  document.body.className=st.mode=='COUNT'?'count':'time';
  seg('m_count',st.mode=='COUNT'); seg('m_time',st.mode!='COUNT');
  seg('src_int',st.src=='INT');    seg('src_ext',st.src!='INT');
  seg('shp_fd',st.shape=='FD');    seg('shp_t12',st.shape!='FD');
  seg('ph_off',st.phase_mode=='OFF'); seg('ph_rot',st.phase_mode=='ROTATE');
  seg('ph_sync',st.phase_mode=='SYNC');
  seg('bm_saved',st.boot_mode=='SAVED'); seg('bm_run',st.boot_mode=='RUN');
  seg('bm_off',st.boot_mode=='OFF');
  seg('as_1',st.autosave); seg('as_0',!st.autosave);

  $('phnote').innerHTML = st.phase_mode=='OFF'
    ? 'One output on GP5. GP18-20 idle, cycle marker idle.'
    : (st.phase_mode=='ROTATE'
       ? 'Each channel takes one burst in turn, separated by the off-time.'
       : 'All channels fire the same burst together, every burst.')
      + (st.ring_len>1
         ? ' Pattern repeats every '+st.ring_len+' bursts; GP22 is high for burst 1 only.'
         : ' Pattern is one burst, so the cycle marker stays idle.');

  if(clean('carr')){ set('freq',f||1); set('duty',st.carrier_duty_ppm/1e4);
                     set('t1',Math.max(t1,PARAMS.t1.lo));
                     set('t2',Math.max(st.carrier_ns/1000-t1,PARAMS.t2.lo)); }
  if(clean('burst')){ set('on',st.on); set('off',st.off); }
  if(clean('gate')){ set('gper',st.period_ns/1000); set('gdty',st.duty_ppm/1e4); }
  if(clean('elong')){ set('elr',st.elong_ratio||1); }
  if(clean('ph')){ $('phn').value=st.phase_len; }
  if(clean('r1')&&st.ramp1_active){
    $('r1s').value=(st.ramp1_step_ns/1000).toFixed(3);
    $('r1b').value=st.ramp1_bursts;
    $('r1l').value=(st.ramp1_limit_ns/1000).toFixed(3);
    $('r1w').value=st.ramp1_wrap?'WRAP':'STOP';
  }
  if(clean('train')&&st.train_len){
    const cur=trainVals();
    if(cur.length!=st.train_len||cur.some((x,i)=>x!=st.train[i]))
      buildTrain(st.train_len,st.train);      // render only -- never re-send
  }
}

/* ---- boot ---- */
bank('bank_carrier',['freq','duty','t1','t2']);
bank('bank_count',  ['on','off']);
bank('bank_time',   ['gper','gdty']);
bank('bank_elong',  ['elr']);
buildTrain(6);
$('fpre').innerHTML=[100,500,1000,5000,10000,20000,50000].map(hz=>
  '<button onclick="mark(\'carr\');set(\'freq\','+hz+');mirrorTimes();c(\'C '+hz+'\')">'
  +(hz>=1000?hz/1000+'k':hz)+'</button>').join('');
try{ showTab(localStorage.getItem('gptab')||'carrier') }catch(e){ showTab('carrier') }
c('P'); setInterval(poll,2000);
</script></body></html>)HTML";
