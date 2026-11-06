// Stream overlay, served at /overlay -- meant for an OBS Browser Source.
//
// Same contract as webui.h: one PROGMEM blob, reads only GET /state, no
// external anything. Read-only on purpose: it has no controls, so a scene
// left running on a stream machine can never change the waveform.
//
//   /overlay           full-width lower third (size the source 1920 x 260)
//   /overlay?v=card    compact card for a corner     (size it 460 x 330)
//   &bg=1              solid background, for checking it in a normal browser
//
// The background is transparent, so OBS composites it straight over whatever
// is underneath. Everything is sized in vw, so it scales with the source.
#pragma once

static const char OVERLAY_HTML[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>GatedPulse overlay</title><style>
:root{--ink:#e1e5ea;--dim:#98a3b1;--sheet:#14181ee6;--rule:#2a323c;--amber:#d7a343;
      --cobalt:#7fa8f2;--go:#5cc088;--red:#f0776e;
      --sans:"IBM Plex Sans",system-ui,-apple-system,"Segoe UI",sans-serif;
      --serif:"IBM Plex Serif",Georgia,serif;--mono:"IBM Plex Mono",ui-monospace,Menlo,monospace}
*{box-sizing:border-box;margin:0}
html,body{background:transparent;color:var(--ink);font-family:var(--sans);overflow:hidden}
body.bg{background:#0b0e12}
.p{position:absolute;left:1.2vw;right:1.2vw;bottom:1.2vw;background:var(--sheet);
   border:1px solid var(--rule);border-left:.45vw solid var(--amber);border-radius:.6vw;
   padding:1vw 1.6vw;display:flex;align-items:center;gap:2.2vw}
.id{flex:0 0 auto;padding-right:2vw;border-right:1px solid var(--rule)}
.id .k{font:500 .78vw/1 var(--mono);letter-spacing:.25vw;color:var(--dim);text-transform:uppercase}
.id .n{font:700 2vw/1.15 var(--sans);margin:.35vw 0 .2vw;white-space:nowrap}
.id .s{font:italic 1.1vw/1 var(--serif);color:var(--dim)}
.st{display:inline-flex;align-items:center;gap:.5vw;margin-top:.7vw;
    font:600 .85vw/1 var(--mono);letter-spacing:.12vw;color:var(--dim)}
.st i{width:.75vw;height:.75vw;border-radius:50%;background:#56606c}
.st.on{color:var(--go)}.st.on i{background:var(--go);box-shadow:0 0 .8vw var(--go);
       animation:pulse 1.2s ease-in-out infinite}
.st.trip{color:var(--red)}.st.trip i{background:var(--red)}
@keyframes pulse{50%{opacity:.35}}
.g{flex:1;display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:.7vw 2vw}
.c span{display:block;font:500 .72vw/1 var(--sans);letter-spacing:.14vw;
        text-transform:uppercase;color:var(--dim)}
.c b{display:block;font:600 1.75vw/1.25 var(--sans);font-variant-numeric:tabular-nums;
     white-space:nowrap;transition:color .6s}
.c b.md{color:var(--cobalt)}
.c b.ch{color:var(--amber);transition:none}
.ln{grid-column:1/-1;font:1vw/1.2 var(--mono);color:var(--dim);
    border-top:1px solid var(--rule);padding-top:.55vw}
.ln b{color:var(--ink);font-weight:600}
.fl{color:var(--amber)}
/* corner card: the same pieces, stacked */
body.card .p{left:2vw;right:2vw;bottom:2vw;top:2vw;flex-direction:column;align-items:stretch;
             gap:3vw;padding:4vw 4.5vw;border-left-width:1.6vw;border-radius:2.5vw}
body.card .id{border-right:0;padding:0}
body.card .id .k{font-size:2.5vw;letter-spacing:.7vw}
body.card .id .n{font-size:6.5vw}
body.card .id .s{font-size:3.6vw}
body.card .st{font-size:2.8vw;gap:1.6vw;margin-top:2vw}
body.card .st i{width:2.4vw;height:2.4vw}
body.card .g{grid-template-columns:repeat(2,minmax(0,1fr));gap:2.6vw 5vw}
body.card .c span{font-size:2.4vw;letter-spacing:.4vw}
body.card .c b{font-size:5.6vw}
body.card .ln{font-size:2.45vw;padding-top:2vw}
body.card .hide{display:none}
.off .g,.off .ln{opacity:.35}
</style></head><body>
<div class=p id=p>
 <div class=id>
  <div class=k>The Stanley Meyer Archive</div>
  <div class=n>Stan&rsquo;s Legacy</div>
  <div class=s>GatedPulse</div>
  <div class=st id=st><i></i><span id=stt>CONNECTING</span></div>
 </div>
 <div class=g>
  <div class=c><span>Frequency</span><b id=f>&ndash;</b></div>
  <div class=c><span>T1 on</span><b id=t1>&ndash;</b></div>
  <div class=c><span>T2 off</span><b id=t2>&ndash;</b></div>
  <div class=c><span>Duty</span><b id=d>&ndash;</b></div>
  <div class="c hide"><span id=bl>Burst</span><b id=b>&ndash;</b></div>
  <div class="c hide"><span>Gating</span><b id=m class=md>&ndash;</b></div>
  <div class="c hide"><span>Output</span><b id=o class=md>&ndash;</b></div>
  <div class="c hide"><span>Ring</span><b id=r>&ndash;</b></div>
  <div class=ln id=ln>&nbsp;</div>
 </div>
</div>
<script>
const q=new URLSearchParams(location.search), $=i=>document.getElementById(i);
if(q.get('v')=='card') document.body.classList.add('card');
if(q.get('bg')=='1')   document.body.classList.add('bg');
const sig=(x,n)=>String(+x.toPrecision(n));
const fHz=x=>x>=1e6?sig(x/1e6,5)+' MHz':x>=1e3?sig(x/1e3,5)+' kHz':sig(x,4)+' Hz';
const fUs=x=>x>=1e3?sig(x/1e3,4)+' ms':x>=1?sig(x,4)+' µs':sig(x*1e3,3)+' ns';
// A changed value flashes amber for a moment -- on a stream, the eye should
// land on what just moved.
function put(id,t){
  const e=$(id); if(e.textContent===t) return;
  e.textContent=t; e.classList.add('ch');
  clearTimeout(e._t); e._t=setTimeout(()=>e.classList.remove('ch'),50);
}
let miss=0;
async function tick(){
  let s;
  try{ s=await(await fetch('/state',{cache:'no-store'})).json(); miss=0 }
  catch(e){ if(++miss>3){ $('st').className='st'; $('stt').textContent='NO SIGNAL';
                          $('p').classList.add('off') } return }
  $('p').classList.toggle('off',!s.live);
  $('st').className='st'+(s.live?' on':s.interlock_tripped?' trip':'');
  $('stt').textContent=s.live?'OUTPUT LIVE':s.interlock_tripped?'INTERLOCK':'STOPPED';
  const per=s.carrier_ns/1000, t1=per*s.carrier_duty_ppm/1e6, t2=per-t1;
  if(s.fsweep){ put('f',fHz(s.fsweep_hz)+(s.fsweep_dir=='up'?' ↑':' ↓')) }
  else put('f',s.carrier_ns?fHz(1e9/s.carrier_ns):'–');
  put('t1',fUs(t1)); put('t2',fUs(t2));
  put('d',sig(s.carrier_duty_ppm/1e4,4)+' %');
  const cnt=s.mode=='COUNT';
  $('bl').textContent=cnt?'On / off':'Gate';
  put('b',cnt?s.on+' / '+s.off:fUs(s.period_ns/1000));
  put('m',s.mode+' · '+s.src.toLowerCase());
  put('o',s.phase_mode=='OFF'?'single':(s.phase_mode=='ROTATE'?'offset':'sync')+' ×'+s.phase_len);
  put('r',s.ring_len>1?s.ring_len+' bursts':'—');
  const flags=[s.fsweep&&'SWEEP',s.elongation&&('ELONG '+s.elong_ratio+'×'),
               s.train_len&&('TRAIN '+s.train_len),s.seq_len&&('SEQ '+s.seq_len)].filter(Boolean);
  let ln='';
  if(cnt && !s.fsweep) ln='burst <b>'+fUs(s.on*per)+'</b> · gap <b>'+fUs(s.off*per)
     +'</b> · repeats <b>'+fHz(1e6/((s.on+s.off)*per))+'</b>';
  if(flags.length) ln+=(ln?' · ':'')+'<span class=fl>'+flags.join(' · ')+'</span>';
  $('ln').innerHTML=ln||'&nbsp;';
}
tick(); setInterval(tick,1000);   // 1 Hz: the board serves one client at a time
</script></body></html>)HTML";
