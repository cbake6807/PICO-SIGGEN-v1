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
//      its step sizes. The rows and their exact-entry boxes are BUILT
//      from it at load. Adding a knob means adding a row there, not writing
//      another slab of near-identical HTML and another poll branch to forget.
//
//   2. Dirty windows, not focus checks. The poll refills every control from
//      the board every 2 s, which fought manual edits: an activeElement test
//      does not survive a <select>, and mobile blurs inputs mid-gesture. Any
//      touch now stamps its control GROUP, and the poll leaves that group
//      alone until it has been quiet. Same bug, same fix, as the dashboard.
#pragma once

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name=color-scheme content=dark>
<title>GatedPulse &middot; Stan&rsquo;s Legacy</title><style>
:root{--bg:#14181e;--card:#1c222a;--card2:#242c35;--line:#2a323c;--fg:#e1e5ea;
      --dim:#98a3b1;--go:#2f8f5b;--warn:#a97615;--acc:#2e62d9;--red:#b4312a;
      --acc2:#7fa8f2;--amber:#d7a343;--grid:#1a1f27;
      /* Stan's Legacy type: IBM Plex where the device has it, system otherwise.
         Nothing is fetched -- the page must render whole with no internet. */
      --sans:"IBM Plex Sans",system-ui,-apple-system,sans-serif;
      --serif:"IBM Plex Serif",Georgia,serif;
      --mono:"IBM Plex Mono",ui-monospace,SFMono-Regular,Menlo,monospace}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--fg);
  font:14px/1.4 var(--sans);-webkit-text-size-adjust:100%}
body{padding-bottom:70px;
  /* the stanslegacy.com blueprint sheet, faint enough to read through */
  background-image:linear-gradient(var(--grid) 1px,transparent 1px),
                   linear-gradient(90deg,var(--grid) 1px,transparent 1px);
  background-size:24px 24px}
/* ---- brand strip ---- */
.brand{display:flex;align-items:center;gap:8px;padding:8px 10px 0;min-width:0}
.brand img{height:26px;width:auto;filter:invert(1);opacity:.92}
.brand b{font:600 14px/1 var(--sans);color:var(--fg);letter-spacing:.2px;white-space:nowrap}
.brand i{font:italic 13px/1 var(--serif);color:var(--dim);white-space:nowrap;
  overflow:hidden;text-overflow:ellipsis}
.brand a{margin-left:auto;font:11px var(--mono);color:var(--acc2);text-decoration:none;
  white-space:nowrap}
.foot{text-align:center;margin:18px 0 6px;color:var(--dim);font-size:12px}
.foot img{height:44px;filter:invert(1);opacity:.85;display:block;margin:0 auto 6px}
.foot .k{font:10.5px var(--mono);letter-spacing:2px;text-transform:uppercase}
.foot hr{width:44px;border:0;border-top:2px solid var(--amber);margin:8px auto}
.foot a{color:var(--acc2);text-decoration:none;font-family:var(--mono)}
.foot .by{margin:12px 0 2px}
.foot .by b{display:block;font:600 14px/1.3 var(--sans);color:var(--fg)}
.foot .by span{font:10.5px var(--mono);letter-spacing:2px;text-transform:uppercase}

/* ---- sticky header: state you must never lose while on another tab.
       One action row, one fixed 4x2 status table (cells never reflow, so a
       value is always in the same place to glance at), a flag row that only
       exists while something unusual is armed, and the tabs. ---- */
header{position:sticky;top:0;z-index:9;background:#14181ef2;
  backdrop-filter:blur(8px);border-bottom:1px solid var(--line);
  padding:calc(8px + env(safe-area-inset-top,0px)) 10px 0}
.hbar{display:flex;gap:6px;align-items:stretch}
.hbar>button{flex:1 1 0;min-width:0;min-height:46px;padding:4px;font-size:12.5px}
#pw{flex:1.35 1 0;border:0;border-radius:9px;font:600 15px/1.1 var(--sans);
  color:#fff;background:#2f3843;cursor:pointer;letter-spacing:.4px}
#pw.on{background:var(--go);box-shadow:0 0 0 1px #5cc08855,0 0 14px #2f8f5b55}
/* A tripped interlock is not the same as "stopped" and must not look like it:
   stopped is something you chose, tripped is the enclosure refusing. */
#pw.trip{background:var(--red);box-shadow:0 0 0 1px #f0776e55,0 0 14px #f0776e55}
#pw small{display:block;font-weight:400;font-size:10px;opacity:.85;margin-top:2px}
.hst{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:1px;margin:7px 0 0;
  background:var(--line);border:1px solid var(--line);border-radius:8px;overflow:hidden}
.hst>div{background:var(--card);padding:4px 7px 5px;min-width:0}
.hst span{display:block;font:500 9.5px/1.2 var(--sans);letter-spacing:.6px;
  text-transform:uppercase;color:var(--dim)}
/* sans with tabular digits, not mono: Android's monospace is wide enough
   that 100.000 kHz no longer fits a quarter of a phone. */
.hst b{display:block;font:600 13.5px/1.35 var(--sans);font-variant-numeric:tabular-nums;color:var(--fg);
  white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.hst b.md{color:var(--acc2)}
#hflags{margin-top:6px}#hflags:empty{display:none}
.chips{display:flex;flex-wrap:wrap;gap:4px}
.chip{font:500 11px/1 var(--mono);padding:4px 7px;border-radius:999px;
  border:1px solid var(--line);color:var(--dim);white-space:nowrap}
.chip.k{background:var(--card2);color:var(--fg)}
.chip.on{background:var(--go);color:#fff;border-color:transparent}
.chip.md{background:var(--acc);color:#fff;border-color:transparent}
.chip.wr{background:var(--warn);color:#fff;border-color:transparent}

/* ---- tabs: inside the sticky header, every tab visible at phone width --
       a tab you have to scroll sideways to find is a tab nobody finds. ---- */
nav{display:grid;grid-template-columns:repeat(6,minmax(0,1fr));margin:7px -10px 0;
  border-top:1px solid var(--line)}
nav button{background:none;border:0;border-radius:0;border-bottom:2px solid transparent;
  color:var(--dim);padding:9px 2px 7px;font-size:13px;min-height:40px;white-space:nowrap}
nav button.on{color:var(--fg);border-bottom-color:var(--amber);font-weight:600}
nav button:active{transform:none}
main{padding:10px}
section{display:none}section.on{display:block}

/* ---- cards ---- */
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;
  padding:10px 12px;margin:0 0 10px}
.card>h2{font:600 12px/1 var(--sans);letter-spacing:.5px;text-transform:uppercase;
  color:var(--dim);margin:0 0 9px}
.note{font-size:11.5px;color:var(--dim);margin:8px 0 0}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:0 10px;
  align-items:start}

/* ---- parameter rows. Driven with a thumb, often one-handed, sometimes with
       the other hand on a probe -- so nothing here needs precision to hit.
       Three ways in, coarse to fine:
         slider  absolute, whole range, to get in the ballpark
         - / +   one step per tap; HOLD to repeat, accelerating
         jog     drag sideways like a knob, one step per detent
       and the step size is the segmented row underneath. A swipe that
       starts on any of them still scrolls the page and changes nothing. ---- */
.bank{display:block}
.prm{background:var(--card2);border:1px solid var(--line);border-radius:12px;
  padding:10px 10px 8px;margin:0 0 8px}
.prh{display:flex;align-items:center;gap:8px;margin-bottom:8px}
.prh .fl{min-width:46px}
.prh b{flex:1;min-width:0;font:600 21px/1.1 var(--mono);color:#fff;white-space:nowrap}
.prh input{width:118px;text-align:right;font:15px var(--mono);padding:8px}
.prj{display:flex;gap:6px;align-items:stretch}
.st{flex:0 0 60px;min-height:60px;padding:0;font:600 26px/1 var(--mono);
  touch-action:pan-y;user-select:none;-webkit-user-select:none;
  -webkit-touch-callout:none}
.st.on{background:var(--acc);color:#fff;border-color:transparent}
.st:active{transform:none}
/* The encoder: a knurled wheel seen edge-on. The ridges scroll under fixed
   cylinder shading as you drag, one detent per step, and the label in the
   middle is the step each detent makes -- so you know what a flick will do
   before you do it. */
.jog{position:relative;flex:1;min-height:60px;border:1px solid #3a4552;border-radius:10px;
  overflow:hidden;touch-action:pan-y;cursor:ew-resize;user-select:none;
  -webkit-user-select:none;display:flex;align-items:center;justify-content:center;
  background:#0d1015 repeating-linear-gradient(90deg,#55627300 0 3px,#556273 3px 5px,
             #2a323c 5px 7px,#55627300 7px 16px)}
.jog::after{content:'';position:absolute;inset:0;pointer-events:none;
  background:linear-gradient(90deg,#0d1015 0,#0d1015d0 9%,#0d101500 34%,
                                   #0d101500 66%,#0d1015d0 91%,#0d1015 100%),
             linear-gradient(#ffffff12,#ffffff00 45%,#00000066)}
.jog span{position:relative;z-index:1;pointer-events:none;background:#0d1015e8;
  padding:4px 9px;border-radius:6px;font:600 12px var(--mono);color:var(--acc2);
  white-space:nowrap}
.jog.act{border-color:var(--acc);box-shadow:inset 0 0 0 1px var(--acc)}
.jog.act span{color:#fff}
/* drive tab: the mode switches, one labelled row each */
.mrow{display:flex;align-items:center;gap:8px;margin:0 0 8px}
.mrow>.fl{flex:0 0 62px}
.mrow>.seg{flex:1;min-width:0}
.mrow .seg button{min-height:44px;padding:6px 4px;white-space:nowrap}
.bsum{font:12.5px var(--sans);font-variant-numeric:tabular-nums;color:var(--dim);margin:2px 2px 0}
.bsum b{color:var(--fg)}
.sz{margin-top:2px}
.sz button{min-height:36px;padding:6px 2px;font:12px var(--mono);color:var(--dim)}
.sz button.sel{color:#fff}
/* Sliders are drawn, not <input type=range>. A native range jumps its thumb
   to wherever a finger lands, so a scroll swipe that happened to START on one
   changed the value -- and on the sweep's cycle-time slider, started a sweep.
   These never jump: a drag moves the thumb RELATIVE to where it was, and only
   after a clear sideways movement; vertical swipes scroll the page. */
.sl{position:relative;height:44px;margin:4px 0 2px;touch-action:pan-y;
  cursor:ew-resize;user-select:none;-webkit-user-select:none;--f:0}
.sl i{position:absolute;left:0;right:0;top:18px;height:8px;border-radius:4px;
  background:linear-gradient(90deg,var(--acc) calc(15px + (100% - 30px)*var(--f)),
                                   #2f3843 calc(15px + (100% - 30px)*var(--f)))}
.sl u{position:absolute;top:7px;left:calc((100% - 30px)*var(--f));width:30px;height:30px;
  box-sizing:border-box;border-radius:50%;background:#fff;border:4px solid var(--acc);
  transition:transform .1s}
.sl.act u{transform:scale(1.2)}
.fl{font-size:11px;color:var(--dim);text-transform:uppercase;letter-spacing:.4px}
/* train ring: one compact row per burst */
.trow{display:flex;align-items:center;gap:6px;margin:0 0 6px}
.trow .fl{flex:0 0 58px}
.trow .sl{flex:1;min-width:0}
.trow .st{flex:0 0 46px;min-height:44px;font-size:22px}
.trow b{flex:0 0 52px;text-align:right;font:600 15px var(--mono);color:#fff}

/* ---- generic controls ---- */
button{background:#242c35;color:var(--fg);border:1px solid var(--line);border-radius:7px;
  padding:9px 12px;cursor:pointer;font-size:13px;min-height:36px;-webkit-tap-highlight-color:transparent}
button:active{transform:translateY(1px)}
button.go{background:var(--go);color:#fff;border-color:transparent}
button.acc{background:var(--acc);color:#fff;border-color:transparent}
button.warn{background:var(--warn);color:#fff;border-color:transparent}
button.red{background:var(--red);color:#fff;border-color:transparent}
button[disabled]{opacity:.35;cursor:not-allowed;filter:grayscale(1)}
button.sel{background:var(--acc);color:#fff;border-color:transparent;font-weight:600}
input,select{background:#10141a;color:var(--fg);border:1px solid var(--line);
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
.pin.lo{background:#1d3a2b;border-color:var(--go)}
.pin.lo .lv{color:#5cc088}
.pin.hit{background:var(--acc);border-color:transparent}
.pin.hit .nm,.pin.hit .gp,.pin.hit .lv{color:#fff}

/* ---- output ---- */
pre{background:#0e1116;border:1px solid var(--line);padding:9px;border-radius:9px;
  overflow-x:auto;font:12px/1.45 var(--mono);color:#5cc088;white-space:pre-wrap;
  max-height:230px;overflow-y:auto;margin:0}
#toast{position:fixed;left:50%;bottom:14px;transform:translateX(-50%) translateY(70px);
  background:#242c35fa;border:1px solid var(--line);color:var(--fg);padding:9px 15px;
  border-radius:999px;font:12px var(--mono);max-width:88vw;white-space:nowrap;
  overflow:hidden;text-overflow:ellipsis;transition:transform .18s;z-index:20;
  pointer-events:none}
#toast.up{transform:translateX(-50%) translateY(0)}
#toast.bad{border-color:var(--red);color:#ff9c93}

/* Only one gating block is real at a time; showing both invites setting the
   one that is not running and wondering why nothing changed. */
body.count .timeonly,body.time .countonly{display:none}

/* board not answering: say so, and make the controls look as dead as they are */
#offbar{display:none;background:var(--amber);color:#14181e;font:600 13px/1.3 var(--sans);
  padding:8px 12px;text-align:center}
body.offline #offbar{display:block}
body.app #offbar{display:none}
body.offline main,body.offline .hst,body.offline .hbar{opacity:.4;pointer-events:none;
  filter:grayscale(.6);transition:opacity .3s}
</style></head><body class=count>

<div class=brand id=brand><img id=mark alt="Stan's Legacy"
  src="data:image/png;base64,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"><b>Stan&rsquo;s Legacy</b><i>GatedPulse</i>
  <a href="https://stanslegacy.com" target=_blank rel=noopener>stanslegacy.com</a></div>
<header>
 <div id=offbar>Connection to the generator lost &mdash; reconnecting&hellip;</div>
 <div class=hbar>
  <button id=pw onclick="tglE()">OUTPUT<small id=pws>&mdash;</small></button>
  <button onclick="c('SHOT')">single<br>burst</button>
  <button id=savebtn class=acc onclick="c('SAVE')">save</button>
  <button class=warn onclick="c('R')" title="100 kHz / 50%, 10 on / 90 off, internal carrier, offset x3 so GP18-20 and GP22 are all live, output ON">known<br>state</button>
 </div>
 <div class=hst>
  <div><span>freq</span><b id=h_f>&ndash;</b></div>
  <div><span>duty</span><b id=h_d>&ndash;</b></div>
  <div><span>T1</span><b id=h_t1>&ndash;</b></div>
  <div><span>T2</span><b id=h_t2>&ndash;</b></div>
  <div><span id=h_bl>on / off</span><b id=h_b>&ndash;</b></div>
  <div><span>gating</span><b id=h_m class=md>&ndash;</b></div>
  <div><span>output</span><b id=h_o class=md>&ndash;</b></div>
  <div><span>ring</span><b id=h_r>&ndash;</b></div>
 </div>
 <div class=chips id=hflags></div>
 <nav id=tabs></nav>
</header>

<main>

<!-- drive: what you reach for with a probe in the other hand. T1/T2, the
     modes, and the burst, as encoders. Everything else is one tab away. -->
<section data-tab=drive>
 <div class=grid>
  <div class=card><h2>pulse &mdash; T1 on, T2 off</h2>
   <div class=bank id=bank_pulse></div>
   <p class=note>Frequency and duty follow &mdash; they are the same pair, seen
    the other way round (carrier tab). Type in the box for an exact value
    (<code>4.7</code>, <code>1.2k</code>).</p>
  </div>
  <div>
   <div class=card><h2>modes</h2>
    <div class=mrow><span class=fl>gating</span><div class=seg>
     <button id=m_count onclick="c('M 1')">COUNT pulses</button>
     <button id=m_time onclick="c('M 0')">TIME window</button></div></div>
    <div class=mrow><span class=fl>output</span><div class=seg>
     <button id=ph_off onclick="c('PHASE OFF')">single</button>
     <button id=ph_rot onclick="c('PHASE ROT')">offset</button>
     <button id=ph_sync onclick="c('PHASE SYNC')">sync</button></div></div>
    <div class=mrow><span class=fl>channels</span><div class=seg>
     <button id=ch1 onclick="tglCh(1)">ch1</button>
     <button id=ch2 onclick="tglCh(2)">ch2</button>
     <button id=ch3 onclick="tglCh(3)">ch3</button></div></div>
    <p class=note id=phnote></p>
   </div>
   <div class="card countonly"><h2>burst &mdash; pulses on, gap off</h2>
    <div class=bank id=bank_count></div>
    <p class=bsum id=bsum></p>
   </div>
   <div class="card timeonly"><h2>gate window</h2>
    <div class=bank id=bank_time></div>
   </div>
  </div>
  <div class=card><h2>one-shot &amp; polarity</h2>
   <div class=row>
    <button onclick="c('SHOT')">one burst</button>
    <input id=runms type=number value=1000 min=1 style=width:84px><button onclick="c('RUN '+v('runms'))">run ms</button>
    <button class=red onclick="c('STOP')">stop</button>
   </div>
   <div class=row>
    <button onclick="c('I')">invert gate</button>
    <button onclick="c('V')">invert pulses</button>
    <button onclick="c('PHASE DUMP')">dump tables</button>
   </div>
   <p class=note>A deselected channel is <b>held low at the pad</b> in every
    mode, so a dead driver cannot be switched on by a stale code. At least one
    stays selected.</p>
  </div>
 </div>
</section>

<section data-tab=carrier>
 <div class=grid>
  <div class=card><h2>frequency &amp; duty</h2>
   <div class=bank id=bank_carrier></div>
   <p class=note>The same pair as T1/T2 on the drive tab &mdash; move one and
    the other follows. Type <code>49.1k</code> or <code>2M</code> for an exact
    value.</p>
  </div>
  <div>
   <div class=card><h2>jump to</h2>
    <div class=row id=fpre></div>
   </div>
   <div class=card><h2>source</h2>
    <div class=seg>
     <button id=src_int onclick="c('SRC INT')">internal GP2</button>
     <button id=src_ext onclick="c('SRC EXT')">external GP3</button>
    </div>
    <p class=note>GP3 is <b>3.3 V only</b> and not 5 V tolerant.</p>
   </div>
   <div class=card><h2>panel knobs edit</h2>
    <div class=seg>
     <button id=shp_fd onclick="c('SHAPE FD')">freq + duty</button>
     <button id=shp_t12 onclick="c('SHAPE T12')">T1 + T2</button>
    </div>
   </div>
  </div>
 </div>
</section>

<section data-tab=sweep>
 <div class=grid>
 <div class=card id=fswcard><h2>frequency sweep &mdash; continuous, ungated</h2>
  <div class=row>
   <button id=fswbtn class=go onclick="tglFsw()">start sweep</button>
   <span class=chips id=fswchips></span>
  </div>
  <div class=row>
   <label>cycle time</label>
   <div class=sl id=fswms onpointerdown="slideFsw(event)"><i></i><u></u></div>
  </div>
  <div class=row>
   <span class=kv>full up-and-back: <b id=fswms_v>&mdash;</b></span>
   <label style=min-width:auto>top</label>
   <input type=number id=fswhi value=10000 step=100 style=width:100px
     onchange="mark('fsw');c('FSWEEP HI '+this.value)"><span class=kv>Hz</span>
  </div>
  <p class=note>Sweeps <b>0 &rarr; top &rarr; 0</b> and repeats, at a fixed
   <b>50 % duty</b>, with <b>no gating at all</b> &mdash; one continuous tone on
   GP5, mirrored on GP2, and on <b>all three channels GP18&ndash;GP20 together</b>
   (one state machine drives all three, so there is no skew between them).
   <b>Encoder 2</b> (the frequency knob) sets the cycle time while this is
   running.</p>
  <p class=note>Three legs conducting at once is <b>three times the draw</b> on
   the shared rail. If these are taps of one winding rather than separate cells,
   simultaneous conduction shorts the turns between them &mdash; the one-hot
   decoder that normally prevents that is bypassed in this mode.</p>
  <p class=note>GP4 marks the sweep instead of a burst: <b>high on the way up,
   low on the way down</b>, so it is still your scope trigger and it tells you
   which direction you are watching. ON/OFF, PHASE, ELONGATE and SEQ do nothing
   while the sweep owns the output.</p>
  <p class=note>The bottom of the band is not DC &mdash; the PIO clock divider
   runs out at <b id=fswlo>&mdash;</b>, which is what "0" really means here.</p>
 </div>
  <div class=card><h2>T1 ramp &mdash; stepped between bursts</h2>
   <div class=row><label>step (us)</label>
    <input id=r1s type=number value=0.2 step=0.1 oninput="mark('r1')"></div>
   <div class=row><label>every N bursts</label>
    <input id=r1b type=number value=50 min=1 oninput="mark('r1')"></div>
   <div class=row><label>limit (us)</label>
    <input id=r1l type=number value=9 step=0.1 oninput="mark('r1')"></div>
   <div class=row><label>at the limit</label>
    <select id=r1w onchange="mark('r1')"><option>WRAP</option><option>STOP</option></select></div>
   <div class=row>
    <button class=go onclick="c('RAMP1 '+v('r1s')+' '+v('r1b')+' '+v('r1l')+' '+v('r1w'))">arm ramp</button>
    <button onclick="c('RAMP1 OFF')">off</button>
   </div>
   <p class=note>A resonance search across bursts, with gating left on &mdash;
    unlike the frequency sweep above, and unlike elongation, which reshapes
    pulses inside one burst.</p>
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
</section>

<section data-tab=io>
 <div class=grid>
  <div class=card><h2>front panel &mdash; live wiring check</h2>
   <p class=note style="margin:0 0 8px">Every input idles <b>HIGH</b> (internal
    pull-up). <b>LOW</b> means the contact is made. Turn a knob and watch its A
    and B cells blink: <b>neither blinks</b> = wiring or a dry joint; <b>both
    blink but no STEP appears</b> = A and B are swapped; <b>stuck LOW
    untouched</b> = shorted to ground. Board pin numbers are the ones you count
    on the header.</p>
   <div id=pins class=pins></div>
  </div>
  <div>
   <div class=card><h2>output pins</h2>
    <p class=note style=margin:0>
     <b>GP5</b> pin 7 &mdash; every pulse, every mode. Always a valid monitor.<br>
     <b>GP4</b> pin 6 &mdash; high for every burst.<br>
     <b>GP18/19/20</b> pins 24/25/26 &mdash; the channels.<br>
     <b>GP22</b> pin 29 &mdash; cycle marker, high for burst 1 of the pattern
     only. Trigger here with holdoff <b>off</b> and the pattern sits still.<br>
     <b>GP2</b> pin 4 &mdash; raw carrier tap.</p>
   </div>
   <div class=card><h2>sensor node</h2>
    <div class=row>
     <input id=snhost type=text placeholder="gatedsensor.local" style="width:170px"
            onchange="saveSn()">
     <button onclick="pollSensors(1)">read now</button>
     <button onclick="snCmd('SCAN')">rescan I2C</button>
     <span class=kv id=snstat></span>
    </div>
    <div class=chips id=snsync style="margin:8px 0 6px"></div>
    <div class=chips id=snadc></div>
    <pre id=sni2c style="margin-top:8px">&mdash;</pre>
    <p class=note>A second board, polled by your browser directly &mdash; not
     relayed through this one, so a sensor fault cannot disturb the waveform.
     Wire its <b>GP2 (pin 4)</b> to this board's <b>GP22 (pin 29)</b> plus a
     ground, and it locks to the cycle marker. Analog values are raw ADC
     millivolts, <b>not calibrated current</b>.</p>
   </div>
  </div>
 </div>
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
<div class=foot>
 <img id=footmark alt="">
 <div class=k>The Stanley Meyer Archive</div><hr>
 GatedPulse is a <a href="https://stanslegacy.com" target=_blank rel=noopener>stanslegacy.com</a> bench instrument
 <div class=by><b>Chris Bake</b><span>Founder</span></div>
 <div class=note style="font-family:var(--mono)">firmware <span id=fwver>&ndash;</span></div>
 <!-- The APK lives in the repo, not in flash: the phone fetches it from
      GitHub, so the download needs internet. Hidden inside the app. -->
 <p class=note id=getapp style="margin-top:10px">
  <a style="color:var(--acc2)" href="https://github.com/cbake6807/PICO-SIGGEN-v1/raw/main/android/GatedPulse.apk">Android app (.apk)</a>
  &middot; <a style="color:var(--acc2)" href="https://github.com/cbake6807/PICO-SIGGEN-v1/blob/main/android/README.md">how to install</a></p>
</div>
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
   to read it back out of /state, what to send, and its step sizes. The
   rows and their entry boxes are built from this, so a new knob is a row
   rather than another slab of HTML plus another poll branch to forget.

   Log tracks by default, because most of these are decade ranges -- 1 Hz to
   5 MHz on a linear slider puts everything below 100 kHz in the bottom 2%.
   `lin:1` opts out, and duty needs it: a percentage is BOUNDED, not a decade
   range, so a log track wastes half the travel on 0.05-1% and parks 1% at the
   midpoint. Bounded quantity -> linear; ranges spanning decades -> log.

   st: the three step sizes (fine / medium / coarse). 'dig' means a digit of
   the current value, the way a bench generator's cursor works: at 49.087 kHz
   that is 10 Hz / 100 Hz / 1 kHz. Fixed lists are for bounded quantities. */
const PARAMS={
  freq:{lab:'freq', lo:1,    hi:5e6,  fmt:fHz, grp:'carr', st:'dig', smin:0.01,
        read:s=>s.carrier_ns?1e9/s.carrier_ns:0, send:x=>['C '+x.toFixed(4)]},
  duty:{lab:'duty', lo:0.05, hi:99.9, fmt:fPc, grp:'carr', st:[0.1,1,5], lin:1,
        read:s=>s.carrier_duty_ppm/1e4,          send:x=>['CD '+x.toFixed(2)]},
  t1:  {lab:'T1',   lo:0.05, hi:1e5,  fmt:fUs, grp:'carr', st:'dig', smin:0.001,
        read:s=>s.carrier_ns/1000*s.carrier_duty_ppm/1e6, send:x=>pair(x,val('t2'))},
  t2:  {lab:'T2',   lo:0.05, hi:1e5,  fmt:fUs, grp:'carr', st:'dig', smin:0.001,
        read:s=>s.carrier_ns/1000*(1-s.carrier_duty_ppm/1e6), send:x=>pair(val('t1'),x)},
  on:  {lab:'pulses',lo:1,   hi:1000, fmt:x=>fN(x)+' on',  grp:'burst', st:[1,10,100], int:1,
        read:s=>s.on,  send:x=>['ON '+Math.round(x)]},
  // OFF is counted in carrier periods, so the gap scales with T1+T2.
  off: {lab:'gap',  lo:1,    hi:10000,fmt:x=>fN(x)+' off', grp:'burst', st:[1,10,100], int:1,
        read:s=>s.off, send:x=>['OFF '+Math.round(x)]},
  gper:{lab:'period',lo:1,   hi:1e6,  fmt:fUs, grp:'gate', st:'dig', smin:0.001,
        read:s=>s.period_ns/1000, send:x=>['T '+x.toFixed(3)]},
  gdty:{lab:'open', lo:0.05, hi:99.9, fmt:fPc, grp:'gate', st:[0.1,1,5], lin:1,
        read:s=>s.duty_ppm/1e4,   send:x=>['D '+x.toFixed(2)]},
  elr: {lab:'ratio',lo:0.01, hi:1000, fmt:x=>x.toFixed(3)+' x', grp:'elong', st:'dig', smin:0.001,
        read:s=>s.elong_ratio,    send:x=>['ELONGATE '+x.toFixed(4)]},
};

/* T1/T2 go out as ONE command. As C + CD they were two engine restarts with a
   round trip between, during which the carrier ran at the new period and the
   old duty -- a T1 nudge visibly kicked T2 on the scope. */
const pair=(t1,t2)=>['PULSE '+t1.toFixed(4)+' '+t2.toFixed(4)];

const lg=x=>Math.log10(x);
const clamp=(p,x)=>Math.max(p.lo,Math.min(p.hi,x));
/* The slider is only a VIEW. The value lives in cur[] at full precision: it
   used to live in the slider position, which is 1000 steps across up to seven
   decades, so a typed 50000 Hz went out as 49656.7 and every nudge landed on
   the nearest 1.5 % grid point instead of where it was aimed. */
const SL=10000, cur={};
const toSlider=(p,x)=>{
  x=clamp(p,x);
  const f=p.lin ? (x-p.lo)/(p.hi-p.lo)
                : (lg(x)-lg(p.lo))/(lg(p.hi)-lg(p.lo));
  return Math.max(0,Math.min(SL,Math.round(SL*f)));
};
const fromSlider=(p,s)=>{
  const x=p.lin ? p.lo+(s/SL)*(p.hi-p.lo)
                : 10**(lg(p.lo)+(s/SL)*(lg(p.hi)-lg(p.lo)));
  // The slider is the coarse control, so land on round numbers, not 49087.33.
  return p.int ? Math.max(p.lo,Math.round(x)) : p.lin ? Math.round(x*10)/10 : +x.toPrecision(3);
};
const val=k=>cur[k];

/* ---- step sizes ---- */
let SS={}; try{ SS=JSON.parse(localStorage.getItem('gpstep')||'{}') }catch(e){}
const selOf=k=>SS[k]??1;
function stepSize(k,i){
  const p=PARAMS[k];
  if(p.st!=='dig') return p.st[i];
  // fine/med/coarse are always a decade apart, even at the floor where smin
  // would otherwise make fine and medium the same step.
  let st=Math.max(p.smin||0, 10**(Math.floor(lg(Math.max(cur[k],p.lo))+1e-9)-3));
  for(let j=0;j<i;j++) st*=10;
  return st;
}
const trim=s=>s.replace(/(\.\d*?)0+(?=\D|$)/,'$1').replace(/\.(?=\D|$)/,'');
// Step labels get their own formatting: the value formatters round to what is
// worth DISPLAYING, which turned a 0.001 Hz step into "0 Hz".
const sig=x=>String(+x.toPrecision(3));
const sig4=x=>String(+x.toPrecision(4));
const stepLabel=(p,x)=>p.fmt===fHz ? (x>=1e6?sig(x/1e6)+' MHz':x>=1e3?sig(x/1e3)+' kHz':sig(x)+' Hz')
                     : p.fmt===fUs ? (x>=1e3?sig(x/1e3)+' ms':x>=1?sig(x)+' us':sig(x*1e3)+' ns')
                     : p.int ? String(x) : trim(p.fmt(x));
function paintSteps(k){
  const b=$('s_'+k).children, sel=selOf(k);
  for(let i=0;i<3;i++){
    const t=stepLabel(PARAMS[k],stepSize(k,i));
    if(b[i].textContent!==t) b[i].textContent=t;
    b[i].className=i==sel?'sel':'';
  }
  const j='\u25C0 '+b[sel].textContent+' \u25B6';
  if($('j_'+k).textContent!==j) $('j_'+k).textContent=j;
}
function pickStep(k,i){
  SS[k]=i; try{ localStorage.setItem('gpstep',JSON.stringify(SS)) }catch(e){}
  paintSteps(k);
}

function faderHTML(k){
  const p=PARAMS[k], q="'"+k+"'";
  return '<div class=prm>'
   +'<div class=prh><span class=fl>'+p.lab+'</span><b id="v_'+k+'">&ndash;</b>'
   +'<input id="n_'+k+'" type=text inputmode=decimal enterkeyhint=done autocomplete=off'
   +' oninput="mark(\''+p.grp+'\')" onchange="setExact('+q+')"'
   +' onkeydown="if(event.key==\'Enter\')this.blur()"></div>'
   +'<div class=prj>'
   +'<button class=st aria-label="'+p.lab+' down" oncontextmenu="return false"'
   +' onpointerdown="press(event,n=>stepP('+q+',-1,n))">&minus;</button>'
   +'<div class=jog onpointerdown="jog(event,'+q+')"><span id="j_'+k+'"></span></div>'
   +'<button class=st aria-label="'+p.lab+' up" oncontextmenu="return false"'
   +' onpointerdown="press(event,n=>stepP('+q+',1,n))">+</button></div>'
   +'<div class=sl id="f_'+k+'" onpointerdown="slideP(event,'+q+')"><i></i><u></u></div>'
   +'<div class="seg sz" id="s_'+k+'">'
   +[0,1,2].map(i=>'<button onclick="pickStep('+q+','+i+')"></button>').join('')
   +'</div></div>';
}
const bank=(id,keys)=>{ $(id).innerHTML=keys.map(faderHTML).join('');
                         keys.forEach(k=>set(k,PARAMS[k].lo)); };

// pos: where the finger has the thumb, when a drag is the source -- the
// thumb follows the finger, not the rounded value it produced.
function set(k,x,pos){
  const p=PARAMS[k];
  x=clamp(p,x); if(p.int) x=Math.round(x);
  cur[k]=x;
  $('f_'+k).style.setProperty('--f',(pos??toSlider(p,x))/SL);
  $('v_'+k).textContent=p.fmt(x);
  const n=$('n_'+k);
  if(document.activeElement!==n) n.value=p.int?Math.round(x):+x.toPrecision(7);
  paintSteps(k);
  if(k=='t1'||k=='t2'||k=='on'||k=='off') paintBurst();
}
// What on/off MEAN in time at the current carrier -- the counts alone make
// you do the multiplication in your head while watching the scope.
function paintBurst(){
  if([cur.t1,cur.t2,cur.on,cur.off].some(x=>x==null)) return;
  const per=cur.t1+cur.t2;
  $('bsum').innerHTML='burst <b>'+trim(fUs(cur.on*per))+'</b> &middot; gap <b>'
    +trim(fUs(cur.off*per))+'</b> &middot; repeats <b>'+trim(fHz(1e6/((cur.on+cur.off)*per)))+'</b>';
}
// Anything that moves one of the four carrier views drags the others along.
function mirror(k){
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
// User changed k: show it, then get it to the board.
function moved(k,x,pos){
  mark(PARAMS[k].grp); set(k,x,pos); mirror(k);
  sendFade(k);
}
function sendFade(k){ liveSend(k,()=>PARAMS[k].send(cur[k]),PARAMS[k].grp); }
function slideP(ev,k){
  const p=PARAMS[k];
  drag(ev,toSlider(p,cur[k])/SL,f=>moved(k,fromSlider(p,f*SL),f*SL));
}
// Snap onto the step grid first, then step: 49 087 Hz +100 goes to 49 100,
// not 49 187, so values stay readable after a couple of presses.
function stepBy(k,dir){
  const p=PARAMS[k];
  let step=held[k];
  // Log quantities glide DOWN a decade at a time: when the next step would
  // pass below one step, the step shrinks tenfold. Holding - from 30 kHz goes
  // 20k, 10k, 9k, 8k ... instead of slamming into the 1 Hz floor.
  while(p.st==='dig' && dir<0 && cur[k]-step < step*0.999 && step/10 >= (p.smin||0)) step/=10;
  held[k]=step;
  const x=cur[k], g=x/step, on=Math.abs(g-Math.round(g))<1e-6;
  const n=on ? Math.round(g)+dir : (dir>0?Math.ceil(g):Math.floor(g));
  moved(k,+(n*step).toPrecision(12));
  buzz();
}
// The step is fixed for the whole press or drag. Recomputing it per step would
// shrink it tenfold mid-hold as 10.000 kHz goes down through 9.9 kHz.
const held={};
function stepP(k,dir,n){
  if(!n) held[k]=stepSize(k,selOf(k));
  stepBy(k,dir);
}
// Typed values: plain numbers, or with a k / M suffix (49.1k, 2M).
function setExact(k){
  const m=$('n_'+k).value.trim().match(/^(-?\d*\.?\d+(?:e-?\d+)?)\s*([kKM]?)/);
  if(!m) { set(k,cur[k]); return; }
  moved(k,+m[1]*({k:1e3,K:1e3,M:1e6}[m[2]]||1));
}

/* ---- press-and-hold, jog, drag ----
   Pointer events, not click: click cannot repeat. A TAP steps once on
   release; a HOLD starts repeating after 400 ms, accelerating. A press that
   turns into a scroll is cancelled by the browser (touch-action:pan-y) and
   does nothing -- stepping on pointerdown made every scroll that began on a
   button a change. */
function press(ev,fn){
  if(ev.button>0) return;
  const el=ev.currentTarget, id=ev.pointerId;
  let n=0, t=null;
  el.classList.add('on');
  const rep=()=>{ fn(n++); t=setTimeout(rep, n<6?140:n<20?70:35); };
  t=setTimeout(()=>{ try{ el.setPointerCapture(id) }catch(e){} rep(); },400);
  const end=()=>{ clearTimeout(t); el.classList.remove('on');
                  el.onpointerup=el.onpointercancel=null; };
  el.onpointerup=e=>{
    const tap=n===0 && el.contains(document.elementFromPoint(e.clientX,e.clientY));
    end(); if(tap) fn(0);
  };
  el.onpointercancel=end;
}
// Shared by sliders and the jog: nothing happens until the finger has moved
// SLOP px sideways, so a vertical swipe is left to scroll the page.
const SLOP=8;
function drag(ev,f0,onF,onEnd){
  if(ev.button>0) return;
  const el=ev.currentTarget, x0=ev.clientX, w=Math.max(1,el.clientWidth-30);
  let live=false;
  el.onpointermove=e=>{
    let dx=e.clientX-x0;
    if(!live){
      if(Math.abs(dx)<SLOP) return;
      live=true; el.classList.add('act');
      try{ el.setPointerCapture(e.pointerId) }catch(_){}
    }
    dx-=Math.sign(dx)*SLOP;
    onF(f0===null ? dx : Math.max(0,Math.min(1,f0+dx/w)));
  };
  el.onpointerup=el.onpointercancel=e=>{
    el.classList.remove('act');
    el.onpointermove=el.onpointerup=el.onpointercancel=null;
    if(live&&onEnd) onEnd(e);
  };
}
const DETENT=16;   // px of drag per step
function jog(ev,k){
  const el=ev.currentTarget;
  let done=0; held[k]=stepSize(k,selOf(k));
  drag(ev,null,dx=>{
    const want=Math.trunc(dx/DETENT);
    el.style.backgroundPosition=dx+'px 0';
    while(done<want){ stepBy(k, 1); done++; }
    while(done>want){ stepBy(k,-1); done--; }
  });
}
const buzz=()=>{ try{ navigator.vibrate&&navigator.vibrate(4) }catch(e){} };

/* ---- live send ----
   One request in flight per control, and the newest value replaces whatever
   was waiting -- so a drag or a held button tracks on the scope in real time
   without queueing a backlog of stale commands at the board. A hundred steps
   cost only as many requests as the link can actually carry, and the LAST
   value always lands. Flash is safe: autosave waits for 5 s of quiet. */
const lane={};
function liveSend(key,mk,grp){
  const L=lane[key]||(lane[key]={});
  L.mk=mk; L.grp=grp;
  if(grp) mark(grp);
  if(!L.busy) pump(L);
}
async function pump(L){
  L.busy=1;
  while(L.mk){
    const cmds=L.mk(); L.mk=null;
    for(const cmd of cmds){
      try{
        const t=await (await fetch('/cmd?c='+encodeURIComponent(cmd))).text();
        $('out').textContent=t||'(no output)';
        if(/error/i.test(t)) toast(t.split('\n')[0].slice(0,70),1);
      }catch(e){ toast('request failed',1); }
    }
    if(L.grp) mark(L.grp);   // the hold runs from the last SEND, not the last touch
  }
  L.busy=0;
  clearTimeout(L.pt); L.pt=setTimeout(poll,250);
}

/* ---- channel select ----
   The mask is the single source of truth; the buttons just toggle a bit and
   send the whole resulting list, so the board is never told a delta it has to
   reconcile against what the page happened to be showing. */
function tglCh(n){
  const cur=(st&&st.phase_mask!==undefined)?st.phase_mask:7;
  const m=cur^(1<<(n-1));
  if(!m){ toast('at least one channel must stay on',1); return; }
  const list=[1,2,3].filter(i=>m&(1<<(i-1))).join(' ');
  c('PHASE CH '+list);
}

/* ---- continuous frequency sweep ---- */
const fmtMs=v=>v>=1000?(v/1000).toFixed(v>=10000?0:2)+' s':v+' ms';
const tglFsw=()=>c(st&&st.fsweep?'FSWEEP OFF':'FSWEEP '+fswMs);
/* A bare FSWEEP <ms> ARMS the sweep, so the slider only sends while one is
   already running. Stopped, it just sets what "start sweep" will use --
   adjusting a setting must not be what turns the output into a sweep. */
let fswMs=2000;
const FSW_LO=50, FSW_HI=20000;
function paintFswMs(){
  $('fswms').style.setProperty('--f',(fswMs-FSW_LO)/(FSW_HI-FSW_LO));
  $('fswms_v').textContent=fmtMs(fswMs);
}
function slideFsw(ev){
  drag(ev,(fswMs-FSW_LO)/(FSW_HI-FSW_LO),f=>{
    fswMs=Math.round((FSW_LO+f*(FSW_HI-FSW_LO))/50)*50;
    mark('fsw'); paintFswMs();
  },()=>{ mark('fsw'); if(st&&st.fsweep) c('FSWEEP '+fswMs); });
}
function paintFsw(){
  const on=!!st.fsweep;
  $('fswbtn').textContent=on?'stop sweep':'start sweep';
  $('fswbtn').className=on?'red':'go';
  // The live frequency is the only way to tell a running sweep from a stalled
  // one, so it is a chip rather than buried in a readout.
  $('fswchips').innerHTML=on
    ? chip(fHz(st.fsweep_hz),'on')+chip(st.fsweep_dir=='up'?'\u2191 rising':'\u2193 falling','md')
    : chip('idle','k');
  $('fswlo').textContent=fHz(st.fsweep_hi_hz/65536);
  if(clean('fsw')){
    fswMs=Math.min(FSW_HI,Math.max(FSW_LO,st.fsweep_ms));
    $('fswhi').value=st.fsweep_hi_hz;
  }
  paintFswMs();
}

/* ---- train ring ----
   These send as they move, like every other control here. They used to need
   a separate "arm ring" click, which made them the one place where the
   screen could hold values the firmware had never been told about -- and the
   dirty window is a 4 s timeout, not a commit, so once it lapsed the poller
   found sliders disagreeing with /state and dutifully overwrote them. The edit
   vanished several seconds after it was made, which reads as the board
   fighting you rather than as a value that was never sent.
   Sending on change removes the disagreement instead of papering over it. */
function buildTrain(n,vals){
  const keep=trainVals();
  $('bank_train').innerHTML=Array.from({length:n},(_,i)=>{
    const x=(vals&&vals[i])||keep[i]||100;
    return '<div class=trow><span class=fl>burst '+(i+1)+'</span>'
      +'<button class=st oncontextmenu="return false" onpointerdown="press(event,()=>trStep('+i+',-1))">&minus;</button>'
      +'<div class="sl trs" data-v='+x+' style="--f:'+((x-1)/99)+'"'
      +' onpointerdown="slideT(event,'+i+')"><i></i><u></u></div>'
      +'<button class=st oncontextmenu="return false" onpointerdown="press(event,()=>trStep('+i+',1))">+</button>'
      +'<b>'+x+'%</b></div>';
  }).join('');
}
const trs=()=>document.querySelectorAll('.trs');
const trainVals=()=>[...trs()].map(e=>+e.dataset.v);
function slideT(ev,i){
  const e=trs()[i];
  drag(ev,(e.dataset.v-1)/99,f=>{
    const x=Math.round(1+f*99);
    if(x!=e.dataset.v){ e.dataset.v=x; trMoved(e); }
  });
}
function trMoved(e){
  e.style.setProperty('--f',(e.dataset.v-1)/99);
  e.parentNode.querySelector('b').textContent=e.dataset.v+'%';
  mark('train');
  liveSend('train',()=>['TRAIN '+trainVals().join(' ')],'train');
}
function trStep(i,d){
  const e=trs()[i];
  e.dataset.v=Math.max(1,Math.min(100,+e.dataset.v+d)); trMoved(e); buzz();
}
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
const TABS=[['drive','drive'],['carrier','carrier'],['sweep','sweep'],
            ['shape','shape'],['io','i/o'],['sys','system']];
const tabOn=id=>document.querySelector('section[data-tab='+id+']').classList.contains('on');
function showTab(id){
  if(!TABS.some(t=>t[0]==id)) id='drive';    // a tab remembered from an older page
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
  const vis=tabOn('io');
  // Fast only while someone can see it. Off screen, 2 s is enough: the board
  // keeps the ring, so scrolling back to the log still shows what happened.
  // Four requests a second from every open page was most of the board's
  // HTTP load, on a server that answers one client at a time.
  if(!vis && !logSeen && Date.now()-lastInPoll<2000) return;
  lastInPoll=Date.now();
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
let logSeen=true, lastInPoll=0;
try{ new IntersectionObserver(e=>{ logSeen=e[0].isIntersecting }).observe($('logcard')) }catch(e){}
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
  const vis=tabOn('io');
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

/* ---- connection health ----
   A failed poll used to just return, so a board that rebooted or dropped off
   the WiFi left the page showing its last values as if nothing had happened,
   and every control "worked" into a void. Two misses and the page says so and
   dims; it recovers by itself. In the app the app's own banner says it (this
   page is served by the board that stopped answering), so only the dimming
   applies there. A poll that hangs is a miss too: no timeout meant a dead
   board could hold a request open for a minute, and the polls piled up. */
let misses=0, myBuild=null;
const timed=ms=>{ try{ return {signal:AbortSignal.timeout(ms)} }catch(e){ return {} } };
function setOffline(off){
  document.body.classList.toggle('offline',off);
}
async function poll(){
  try{ st=await(await fetch('/state',timed(3000))).json(); }
  catch(e){ if(++misses>=2) setOffline(true); return; }
  misses=0; setOffline(false);
  // New firmware since this page loaded (an OTA): load the new page, or this
  // one goes on driving a board whose commands may have changed.
  if(st.build){ if(myBuild&&st.build!==myBuild){ location.reload(); return; } myBuild=st.build; }
  const f=st.carrier_ns?1e9/st.carrier_ns:0;
  const t1=st.carrier_ns/1000*st.carrier_duty_ppm/1e6;

  // "live", not "enabled": with the interlock open the operator's intent is
  // still ON while nothing is coming out, and a green button there would be a
  // lie told next to a high-voltage cell.
  $('pw').className=st.live?'on':(st.interlock_tripped?'trip':'');
  $('pws').textContent=st.live?'running':(st.interlock_tripped?'INTERLOCK':'stopped');
  paintLock();
  paintFsw();

  // The status table: fixed cells, so a value never moves when another
  // changes length. Flags only for what is unusual -- the normal state of
  // things is the table, not a row of chips saying "off".
  // 4 significant digits: the full value is a tab away, and 100.000 kHz
  // does not fit a quarter of a phone.
  $('h_f').textContent=f>=1e6?sig4(f/1e6)+' MHz':f>=1e3?sig4(f/1e3)+' kHz':sig4(f)+' Hz';
  $('h_d').textContent=fPc(st.carrier_duty_ppm/1e4);
  $('h_t1').textContent=fUs(t1);
  $('h_t2').textContent=fUs(st.carrier_ns/1000-t1);
  const cnt=st.mode=='COUNT';
  $('h_bl').textContent=cnt?'on / off':'gate';
  $('h_b').textContent=cnt?st.on+' / '+st.off:fUs(st.period_ns/1000);
  $('h_m').textContent=st.mode+' \u00b7 '+st.src.toLowerCase();
  const mask=st.phase_mask!==undefined?st.phase_mask:7;
  $('h_o').textContent=st.phase_mode=='OFF'?'single'
    :(st.phase_mode=='ROTATE'?'offset':'sync')+' \u00d7'+st.phase_len;
  $('h_r').textContent=st.ring_len>1?st.ring_len+' \u00b7 GP'+st.cycle_pin:'\u2014';
  $('hflags').innerHTML=
     (st.fsweep?chip('SWEEP '+fHz(st.fsweep_hz)+' '+(st.fsweep_dir=='up'?'\u2191':'\u2193'),'on'):'')
    +(mask!=7?chip('ch '+[1,2,3].filter(i=>mask&(1<<(i-1))).join('+')+' only','wr'):'')
    +(st.elongation?chip('elong '+st.elong_ratio+'x','wr'):'')
    +(st.sweep?chip('T1 ramp','wr'):'')
    +(st.seq_len?chip('seq '+st.seq_len,'wr'):'')
    +(st.train_len?chip('train '+st.train_len,'wr'):'')
    +(st.autosave?'':chip('autosave off','k'));
  $('savebtn').textContent=st.saved?'saved':'save';
  $('savebtn').className=st.saved?'':'acc';
  $('fwver').textContent=st.fw?st.fw+' · '+st.build:'1.x';

  // toggle, not className=: body also carries app/offline
  document.body.classList.toggle('count',st.mode=='COUNT');
  document.body.classList.toggle('time', st.mode!='COUNT');
  seg('m_count',st.mode=='COUNT'); seg('m_time',st.mode!='COUNT');
  seg('src_int',st.src=='INT');    seg('src_ext',st.src!='INT');
  seg('shp_fd',st.shape=='FD');    seg('shp_t12',st.shape!='FD');
  seg('ph_off',st.phase_mode=='OFF'); seg('ph_rot',st.phase_mode=='ROTATE');
  seg('ph_sync',st.phase_mode=='SYNC');
  for(let i=1;i<=3;i++){
    const on=!!(st.phase_mask&(1<<(i-1)));
    const b=$('ch'+i);
    b.className=on?'sel':'warn';       // amber, not plain: a channel switched
    b.textContent='ch'+i+(on?'':' off');
    b.title='GP'+(17+i);
  }
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
bank('bank_pulse',  ['t1','t2']);
bank('bank_carrier',['freq','duty']);
bank('bank_count',  ['on','off']);
bank('bank_time',   ['gper','gdty']);
bank('bank_elong',  ['elr']);
buildTrain(6);
$('fpre').innerHTML=[100,500,1000,5000,10000,20000,50000].map(hz=>
  '<button onclick="mark(\'carr\');set(\'freq\','+hz+');mirrorTimes();c(\'C '+hz+'\')">'
  +(hz>=1000?hz/1000+'k':hz)+'</button>').join('');
try{ showTab(localStorage.getItem('gptab')||'drive') }catch(e){ showTab('drive') }
// Inside the app its own top bar already carries the mark; the strip would
// only push the controls down.
$('footmark').src=$('mark').src;   // one copy of the mark in flash, not two
if(/GatedPulseApp\//.test(navigator.userAgent)){ $('getapp').remove(); $('brand').remove();
  document.body.classList.add('app'); }
c('P'); setInterval(poll,2000);
</script></body></html>)HTML";
