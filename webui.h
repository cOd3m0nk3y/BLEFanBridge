// webui.h - static web UI for BLEFanBridge (pages, stylesheet, PWA manifest, icon).
// The pages are plain HTML + JS; all data comes from the JSON endpoints in BLEFanBridge.ino:
//   GET /status /config /learnstatus /learn?fan=N /fanctl?fan=N&do=..&v=..
//   POST /fan/add /fan/save /fan/remove /forget /wifireset
#pragma once
#include <Arduino.h>

#define WEB_HEAD                                                                                    \
  "<!DOCTYPE html><html lang=en><head><meta charset=utf-8>"                                         \
  "<meta name=viewport content='width=device-width,initial-scale=1,viewport-fit=cover'>"            \
  "<meta name=color-scheme content='light dark'>"                                                   \
  "<meta name=theme-color content='#f6f7f9' media='(prefers-color-scheme: light)'>"                 \
  "<meta name=theme-color content='#15171a' media='(prefers-color-scheme: dark)'>"                  \
  "<meta name=mobile-web-app-capable content=yes>"                                                  \
  "<meta name=apple-mobile-web-app-capable content=yes>"                                            \
  "<meta name=apple-mobile-web-app-title content='Fan Bridge'>"                                     \
  "<meta name=apple-mobile-web-app-status-bar-style content=default>"                               \
  "<link rel=manifest href=/manifest.json><link rel=icon type='image/svg+xml' href=/icon.svg>"      \
  "<link rel=apple-touch-icon href=/apple-touch-icon.png><link rel=stylesheet href=/style.css>"

// Shared by both pages
#define WEB_JS_COMMON                                                                               \
  "const $=s=>document.querySelector(s);"                                                           \
  "const esc=s=>String(s).replace(/[&<>\"']/g,c=>'&#'+c.charCodeAt(0)+';');"

static const char WEB_CSS[] PROGMEM = R"CSS(
:root{--bg:#eef1f6;--bg2:#dfe6f2;--card:rgba(255,255,255,.86);--fg:#141820;--mute:#5d6573;--line:rgba(20,30,50,.10);
  --btn:rgba(20,30,50,.06);--btnh:rgba(20,30,50,.13);--acc:#2f6fe4;--acc2:#6a5cff;--accfg:#fff;--danger:#c4320a;
  --input:#fff;--shadow:0 1px 2px rgba(20,30,50,.06),0 10px 28px rgba(20,30,50,.09);--glow:rgba(255,184,77,.55)}
@media (prefers-color-scheme:dark){:root{--bg:#0d1015;--bg2:#18202d;--card:rgba(28,33,43,.78);--fg:#eef1f5;--mute:#98a1b0;
  --line:rgba(255,255,255,.08);--btn:rgba(255,255,255,.06);--btnh:rgba(255,255,255,.16);--acc:#5b8cff;--acc2:#8b7bff;
  --danger:#ff8a70;--input:rgba(0,0,0,.25);--shadow:0 1px 2px rgba(0,0,0,.3),0 12px 32px rgba(0,0,0,.4);--glow:rgba(255,184,77,.4)}}
*{box-sizing:border-box}
html{background:var(--bg)}
body{margin:0;min-height:100vh;background:radial-gradient(900px 480px at 15% -120px,var(--bg2),transparent),var(--bg);color:var(--fg);
  font:16px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;-webkit-tap-highlight-color:transparent}
main{max-width:560px;margin:0 auto;padding:calc(10px + env(safe-area-inset-top)) 16px calc(32px + env(safe-area-inset-bottom))}
header{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:10px 0 6px}
.brand{display:flex;align-items:center;gap:10px}.brand img{width:32px;height:32px;border-radius:9px;box-shadow:var(--shadow)}
h1{font-size:24px;margin:0;letter-spacing:-.02em}h2{font-size:19px;margin:0 0 8px;letter-spacing:-.01em}h3{font-size:15px;margin:16px 0 6px}
a{color:var(--acc);font-weight:600;text-decoration:none}
a.pill{display:inline-flex;align-items:center;gap:6px;padding:8px 14px;border-radius:99px;background:var(--card);box-shadow:var(--shadow);color:var(--fg);font-size:15px}
a.pill svg{width:18px;height:18px}
.card{background:var(--card);border:1px solid var(--line);border-radius:22px;padding:16px;margin:14px 0;box-shadow:var(--shadow);
  -webkit-backdrop-filter:blur(14px);backdrop-filter:blur(14px)}
.row{display:grid;gap:8px;margin:8px 0}
button{font:inherit;font-size:16px;min-height:48px;padding:0 12px;border-radius:14px;border:0;background:var(--btn);color:var(--fg);cursor:pointer;
  touch-action:manipulation;transition:transform .12s,background-color .2s,box-shadow .2s,color .2s}
button:active{transform:scale(.96)}
button.on{background:linear-gradient(135deg,var(--acc),var(--acc2));color:var(--accfg);font-weight:600;box-shadow:0 6px 16px rgba(60,100,230,.32)}
button:disabled{opacity:.45;cursor:default;transform:none}
button.danger{color:var(--danger)}
button.wide{width:100%}
label.check{display:flex;align-items:center;gap:10px;margin:12px 0 4px;font-size:16px;color:var(--fg)}
label.check input{width:22px;height:22px;min-height:0;margin:0;accent-color:var(--acc)}
button.hold{user-select:none;-webkit-user-select:none;-webkit-touch-callout:none;touch-action:none}
.small{color:var(--mute);font-size:14px;margin:6px 0}
label{display:block;margin:12px 0 4px;font-size:14px;color:var(--mute)}
input,select{font:inherit;width:100%;min-height:46px;padding:8px 12px;border-radius:12px;border:1px solid var(--line);background:var(--input);color:var(--fg)}
input:focus,select:focus{outline:2px solid var(--acc);outline-offset:1px}
.toolbar{display:flex;flex-wrap:wrap;gap:8px;margin-top:12px}
.toolbar button{flex:1 1 auto}
.msg{min-height:1.45em;margin:8px 0;font-size:14px}
.msg:empty{margin:0;min-height:0}
/* main page */
.fhead{display:flex;align-items:center;gap:14px}
.ficon{flex:none;width:54px;height:54px;border-radius:18px;display:grid;place-items:center;background:var(--btn);color:var(--mute);transition:background .3s,color .3s,box-shadow .3s}
.ficon svg{width:34px;height:34px}
.ficon.on{background:linear-gradient(135deg,var(--acc),var(--acc2));color:#fff;box-shadow:0 8px 20px rgba(60,100,230,.35)}
.ficon.spin svg{animation:spin var(--dur,1s) linear infinite}.ficon.rev svg{animation-direction:reverse}
@keyframes spin{to{transform:rotate(360deg)}}
.ficon.bulb.on{background:radial-gradient(circle at 50% 40%,#fff3c4,#ffc35a 70%);color:#7a4a00;box-shadow:0 0 28px var(--glow)}
.title{flex:1;min-width:0}.title h2,.title h3{margin:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.title h3{font-size:17px}
.status{color:var(--mute);font-size:14px}
.sw{flex:none;position:relative;width:58px;height:34px;min-height:0;padding:0;border-radius:99px;background:var(--btnh)}
.sw::after{content:"";position:absolute;top:3px;left:3px;width:28px;height:28px;border-radius:50%;background:#fff;box-shadow:0 2px 6px rgba(0,0,0,.25);transition:left .22s cubic-bezier(.3,.7,.4,1.2)}
.sw.on::after{left:27px}.sw:active{transform:none}
.seg{display:grid;gap:4px;padding:4px;margin:14px 0 10px;background:var(--btn);border-radius:16px}
.seg button{background:transparent;min-height:44px;border-radius:12px;font-weight:600;font-variant-numeric:tabular-nums}
.seg button.on{background:linear-gradient(135deg,var(--acc),var(--acc2))}
.chips{display:flex;flex-wrap:wrap;gap:8px;margin:8px 0}
.chips button{flex:1 1 0;min-width:0;min-height:42px;font-size:15px;padding:0 10px;white-space:nowrap}
.section{margin-top:16px;padding-top:14px;border-top:1px solid var(--line)}
.step{display:grid;grid-template-columns:56px 1fr 56px;align-items:center;gap:4px;margin:8px 0;padding:4px;background:var(--btn);border-radius:16px}
.step button{background:transparent;min-height:46px;font-size:24px;font-weight:300;border-radius:12px}
.step button.on{background:linear-gradient(135deg,var(--acc),var(--acc2));color:#fff}
.step span{text-align:center;font-size:15px;line-height:1.2}.step small{display:block;color:var(--mute);font-size:12px}
.dot{display:inline-block;width:12px;height:12px;border-radius:50%;margin-right:7px;vertical-align:-1px;border:1px solid rgba(0,0,0,.15)}
.banner{background:linear-gradient(135deg,var(--acc),var(--acc2));color:#fff;border-radius:14px;padding:10px 14px;font-size:14px;margin:8px 0}
.banner:empty{display:none}
.toast{position:fixed;left:50%;bottom:calc(22px + env(safe-area-inset-bottom));transform:translate(-50%,16px);opacity:0;pointer-events:none;
  max-width:calc(100% - 32px);padding:11px 16px;border-radius:14px;background:rgba(18,22,30,.94);color:#fff;font-size:14px;
  box-shadow:0 10px 30px rgba(0,0,0,.3);transition:opacity .25s,transform .25s}
.toast.show{opacity:1;transform:translate(-50%,0)}
.foot{color:var(--mute);font-size:13px;text-align:center;margin:18px 8px 10px}
@media (prefers-reduced-motion:reduce){.ficon.spin svg{animation:none}*{transition:none!important}}
)CSS";

static const char WEB_MANIFEST[] PROGMEM = R"JSON({"name":"Fan Bridge","short_name":"Fans","start_url":"/","scope":"/","display":"standalone","background_color":"#15171a","theme_color":"#15171a","id":"/","icons":[{"src":"/icon-192.png","sizes":"192x192","type":"image/png","purpose":"any"},{"src":"/icon-512-maskable.png","sizes":"512x512","type":"image/png","purpose":"maskable"},{"src":"/icon.svg","sizes":"any","type":"image/svg+xml","purpose":"any"}]})JSON";

static const char WEB_ICON[] PROGMEM = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><rect width="64" height="64" rx="14" fill="#1e6fd9"/><g fill="#fff"><path id="b" d="M32 32C26 23 25 12 31 9c6-3 11 3 9 10-1 5-4 9-8 13z"/><use href="#b" transform="rotate(120 32 32)"/><use href="#b" transform="rotate(240 32 32)"/></g><circle cx="32" cy="32" r="5.5" fill="#1e6fd9" stroke="#fff" stroke-width="3"/></svg>)SVG";

// ---------------------------------------------------------------------------------------------
//  "/" - control page: one card per fan, built from GET /status
// ---------------------------------------------------------------------------------------------
static const char WEB_MAIN[] PROGMEM = WEB_HEAD R"HTML(<title>Fan Bridge</title></head><body><main>
<header><div class=brand><img src=/icon.svg alt=""><h1>Fan Bridge</h1></div>
<a class=pill href=/settings><svg viewBox="0 0 24 24" fill=none stroke=currentColor stroke-width=2 stroke-linecap=round stroke-linejoin=round><circle cx=12 cy=12 r=3 /><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z"/></svg>Settings</a></header>
<div id=banner class=banner></div>
<div id=fans><p class=small>Loading&hellip;</p></div>
<div id=install hidden><button class=wide onclick=install()>&#128241;&ensp;Add to phone</button><p id=howto class=small hidden></p></div>
<p class=foot>Shows the last command sent from here or Alexa. The physical remotes aren't tracked.</p>
<div id=toast class=toast></div>
</main><script>)HTML" WEB_JS_COMMON R"HTML(
// "Add to phone": browsers only offer their install prompt on https pages, and the board is plain
// http, so usually this shows the two taps instead. Hidden when already running from the home screen.
let installEvt=null;
const standalone=matchMedia('(display-mode: standalone)').matches||navigator.standalone;
if(!standalone)$('#install').hidden=false;
addEventListener('beforeinstallprompt',e=>{e.preventDefault();installEvt=e});
function install(){
  if(installEvt){installEvt.prompt();installEvt=null;return}
  const ua=navigator.userAgent,h=$('#howto');
  h.innerHTML=/iPhone|iPad|iPod/.test(ua)
    ?'In Safari, tap the <b>Share</b> button (the square with an arrow), then <b>Add to Home Screen</b>.'
    :/Android/.test(ua)
    ?'In Chrome, tap the <b>&#8942;</b> menu (top right), then <b>Add to Home screen</b> and choose <b>Create shortcut</b>. '+
     'Chrome only <b>installs</b> apps from https pages; to install this one as a full app, open <b>chrome://flags</b>, find '+
     '<b>Insecure origins treated as secure</b>, enter <b>'+location.origin+'</b>, set it to Enabled and relaunch Chrome.'
    :'Open this page on your phone, then use the browser menu: <b>Add to Home screen</b>.';
  h.hidden=false;
}
let fans=null,lastTap=0,lastSig='';
function toast(t){const m=$('#toast');m.textContent=t;m.classList.add('show');clearTimeout(toast.t);toast.t=setTimeout(()=>m.classList.remove('show'),2400)}
const b=(on,label,i,a,v,extra)=>`<button class="${on?'on':''}" ${extra||''} onclick="act(${i},'${a}',${v})">${label}</button>`;
const sw=(on,i,a,v,label)=>`<button class="sw ${on?'on':''}" role=switch aria-checked=${!!on} aria-label="${label}" onclick="act(${i},'${a}',${v})"></button>`;
const BLADE='M32 32C26 23 25 12 31 9c6-3 11 3 9 10-1 5-4 9-8 13z';
const FAN=`<svg viewBox="0 0 64 64" fill=currentColor><path d="${BLADE}"/><path d="${BLADE}" transform="rotate(120 32 32)"/><path d="${BLADE}" transform="rotate(240 32 32)"/><circle cx=32 cy=32 r=5 /></svg>`;
const BULB=`<svg viewBox="0 0 24 24" fill=none stroke=currentColor stroke-width=1.8 stroke-linecap=round stroke-linejoin=round><path d="M9 18h6M10 21h4M12 3a6 6 0 0 0-3.6 10.8c.7.5 1.1 1.3 1.1 2.1V16h5v-.1c0-.8.4-1.6 1.1-2.1A6 6 0 0 0 12 3z"/></svg>`;
function card(f,i){
  const run=f.speedSent&&f.on, lit=f.lightSent&&f.lightOn;
  let st=!f.speedSent?'Tap a speed':run?`Speed ${f.speed} of ${f.speeds}`:'Off';
  if(f.dirSent&&f.reverse)st+=' &middot; reverse';
  let sp='';
  for(let s=1;s<=f.speeds;s++)sp+=b(run&&f.speed==s,s,i,'speed',s,`aria-label="Speed ${s}"`);
  let h=`<div class=card><div class=fhead>`+
    `<div class="ficon ${run?'on spin':''} ${f.reverse?'rev':''}" style="--dur:${(2.4/Math.max(1,f.speed)).toFixed(2)}s">${FAN}</div>`+
    `<div class=title><h2>${esc(f.name)}</h2><div class=status>${st}</div></div>`+
    sw(run,i,'speed',run?0:f.speed,'Fan power')+`</div>`+
    `<div class=seg style="grid-template-columns:repeat(${f.speeds},1fr)">${sp}</div>`+
    `<div class=chips>`+b(f.dirSent&&f.reverse,'&#8635;&ensp;Reverse',i,'dir',f.reverse?0:1)+
    (f.extras?b(0,'&#12316;&ensp;Breeze',i,'breeze',0):'')+`</div>`+
    `<div class=section><div class=fhead>`+
    `<div class="ficon bulb ${lit?'on':''}">${BULB}</div>`+
    `<div class=title><h3>Light</h3><div class=status>${!f.lightSent?'Tap to switch':lit?'On':'Off'}</div></div>`+
    sw(lit,i,'light',lit?0:1,'Light')+`</div>`;
  if(f.extras)h+=
    step('Brightness',i,'bright')+step('Color temperature',i,'temp')+
    `<div class=chips>`+b(0,'<i class=dot style="background:#ffb45c"></i>Warm',i,'cct',0)+
    b(0,'<i class=dot style="background:#fff1d6"></i>Neutral',i,'cct',1)+
    b(0,'<i class=dot style="background:#cfe3ff"></i>Cool',i,'cct',2)+`</div>`+
    `<div class=chips>`+b(0,'&#9790;&ensp;Night light',i,'night',0)+`<button class=danger onclick="act(${i},'alloff',0)">All off</button>`+`</div>`;
  return h+`</div></div>`;
}
function render(force){
  if(!fans)return;
  const sig=JSON.stringify(fans);
  if(!force&&sig==lastSig)return;  // re-render only on change, so the spinning fan doesn't jump
  lastSig=sig;
  const el=$('#fans');
  if(!fans.length){el.innerHTML='<div class=card><h2>No fans yet</h2><p class=small>Open <a href=/settings>Settings</a> and tap <b>+ Add fan</b>.</p></div>';return}
  el.innerHTML=fans.map(card).join('');
}
// Hold buttons: like the remote, the light changes while the button is held. "start" is
// re-sent every 500 ms as a keep-alive; the board lets go by itself if those stop.
const hb=(label,i,a,v,al)=>`<button class=hold aria-label="${al}" data-i=${i} data-a=${a} data-v=${v}>${label}</button>`;
const step=(name,i,a)=>`<div class=step>${hb('&minus;',i,a,0,name+' down')}<span>${name}<small>hold to change</small></span>${hb('+',i,a,1,name+' up')}</div>`;
let held=null;
function holdSend(h,s){
  fetch(`/fanctl?fan=${h.i}&do=${h.a}&v=${h.v}&hold=${s}`).then(r=>r.text()).then(t=>{if(!s)toast(t)})
    .catch(()=>toast("Couldn't reach the board."));
}
function holdStop(){
  if(!held)return;
  clearInterval(held.t);held.el.classList.remove('on');holdSend(held,0);held=null;
}
document.addEventListener('pointerdown',e=>{
  const el=e.target.closest('button.hold');if(!el)return;
  e.preventDefault();holdStop();lastTap=Date.now();
  held={el,i:el.dataset.i,a:el.dataset.a,v:el.dataset.v};
  el.classList.add('on');holdSend(held,1);
  held.t=setInterval(()=>{lastTap=Date.now();holdSend(held,2)},500);
});
['pointerup','pointercancel','blur'].forEach(n=>window.addEventListener(n,holdStop));
document.addEventListener('visibilitychange',()=>{if(document.hidden)holdStop()});
document.addEventListener('contextmenu',e=>{if(e.target.closest('button.hold'))e.preventDefault()});
function act(i,a,v){
  lastTap=Date.now();
  const f=fans[i];
  if(a=='speed'){f.speedSent=true;f.on=v>0;if(v>0)f.speed=v}
  if(a=='light'){f.lightSent=true;f.lightOn=!!v}
  if(a=='dir'){f.dirSent=true;f.reverse=!!v}
  if(a=='alloff'){f.speedSent=f.lightSent=true;f.on=false;f.lightOn=false}
  if(a=='night'){f.lightSent=true;f.lightOn=true}
  render(true);
  fetch(`/fanctl?fan=${i}&do=${a}&v=${v}`).then(r=>r.text()).then(toast)
    .catch(()=>toast("Couldn't reach the board."));
}
function poll(force){
  if(!force&&(document.hidden||Date.now()-lastTap<2500))return;
  fetch('/status').then(r=>r.json()).then(d=>{
    fans=d.fans;
    $('#banner').textContent=d.learning?'Listening for a remote. Commands are held until it finishes.':'';
    render();
  }).catch(()=>{});
}
poll(true);
setInterval(poll,3000);
document.addEventListener('visibilitychange',()=>{if(!document.hidden)poll(true)});
</script></body></html>)HTML";

// ---------------------------------------------------------------------------------------------
//  "/settings" - add/remove/rename fans, learn remotes, pair, WiFi. Built from GET /config
// ---------------------------------------------------------------------------------------------
static const char WEB_SETTINGS[] PROGMEM = WEB_HEAD R"HTML(<title>Fan Bridge settings</title></head><body><main>
<header><div class=brand><img src=/icon.svg alt=""><h1>Settings</h1></div><a class=pill href=/>Done</a></header>
<div id=fans><p class=small>Loading&hellip;</p></div>
<button id=add class=wide onclick="addFan()">+ Add fan</button>
<div class=card><h2>Learn or Pair?</h2>
<p class=small><b>Learn from remote</b> is best when several fans share one wall switch. Tap <b>Learn from remote</b>, then within 20&nbsp;seconds press any button on that remote, close to the board. The board copies the remote's identity and sends commands as that remote, so it controls exactly the fans the remote controls. No pairing is needed, and the remote keeps working.</p>
<p class=small><b>Pair</b> is for a fan without a supported remote. Switch the fan off at the wall, switch it back on, then tap <b>Pair</b> within 5&nbsp;seconds. The fan beeps or its light flashes. Every fan on that switch pairs at the same time.</p>
<p class=small>Adding, removing or renaming a fan restarts the board. Afterwards, say <b>"Alexa, discover devices"</b>.</p></div>
<div class=card><h2>WiFi</h2><button class=wide onclick="wifi()">Change WiFi network</button></div>
</main><script>)HTML" WEB_JS_COMMON R"HTML(
let cfg=null,polling=null;
const post=(u,d)=>fetch(u,{method:'POST',body:new URLSearchParams(d||{})}).then(r=>r.json());
const opt=(v,l,sel)=>`<option value=${v}${sel?' selected':''}>${l}</option>`;
const setMsg=(i,t)=>{const m=$('#m'+i);if(m)m.textContent=t};
const offline=i=>()=>setMsg(i,"Couldn't reach the board.");
function restarting(msg,reload=true){
  $('main').innerHTML='<div class=card><h2>Restarting&hellip;</h2><p class=small>'+esc(msg)+'</p></div>';
  if(reload)setTimeout(()=>location.reload(),8000);
}
function load(){return fetch('/config').then(r=>r.json()).then(c=>{cfg=c;render()})}
function render(){
  $('#fans').innerHTML=cfg.fans.map((f,i)=>`<div class=card><h2>Fan ${i+1}</h2>
<label for=n${i}>Alexa name</label><input id=n${i} maxlength=32 value="${esc(f.name)}">
<label class=check><input type=checkbox id=a${i} ${f.alexa?'checked':''}> Show in Alexa</label>
<label for=l${i}>Light name (leave blank if you don't want a light in Alexa)</label><input id=l${i} maxlength=32 value="${esc(f.light)}">
<label for=s${i}>Speeds</label><select id=s${i}>${opt(6,'6',f.speeds==6)}${opt(3,'3',f.speeds==3)}</select>
${f.learned?'':`<label for=p${i}>Protocol (used by Pair)</label><select id=p${i}>${opt(3,'v3 (try first)',f.proto==3)}${opt(2,"v2 (if v3 won't pair)",f.proto==2)}</select>`}
<div class=toolbar><button onclick="save(${i})">Save</button></div>
<h3>Remote</h3>
<p class=small>${f.learned?`Sends as the learned ${esc(f.kind)} <b>${esc(f.remote)}</b>.`
  :`Not learned. Sends as this board's own ID <b>${esc(f.ownId)}</b>, so the fan has to be paired.`}</p>
<div class=toolbar><button class=learn onclick="learn(${i})">Learn from remote</button>
${f.learned?`<button onclick="forget(${i})">Forget learned remote</button>`
  :`<button onclick="pair(${i},'pair')">Pair</button><button onclick="pair(${i},'unpair')">Unpair</button>`}</div>
<p class=msg id=m${i}></p>
<div class=toolbar><button class=danger onclick="removeFan(${i})">Remove fan</button></div></div>`).join('')
    ||'<div class=card><p class=small>No fans yet. Tap <b>+ Add fan</b> to add one.</p></div>';
  $('#add').disabled=cfg.fans.length>=cfg.max;
  lockLearn(!!polling);
}
function save(i){
  const d={fan:i,name:$('#n'+i).value,light:$('#l'+i).value,speeds:$('#s'+i).value,alexa:$('#a'+i).checked?1:0};
  const p=$('#p'+i);if(p)d.proto=p.value;
  setMsg(i,'Saving…');
  post('/fan/save',d).then(j=>{
    if(j.restart)return restarting(j.msg);
    load().then(()=>setMsg(i,j.msg));
  }).catch(offline(i));
}
function addFan(){post('/fan/add').then(j=>j.restart?restarting(j.msg):alert(j.msg)).catch(()=>alert("Couldn't reach the board."))}
function removeFan(i){
  if(!confirm(`Remove "${cfg.fans[i].name}"? The board restarts.`))return;
  post('/fan/remove',{fan:i}).then(j=>j.restart?restarting(j.msg):setMsg(i,j.msg)).catch(offline(i));
}
function forget(i){
  if(!confirm("Forget the learned remote? This fan goes back to the board's own ID and has to be paired."))return;
  post('/forget',{fan:i}).then(j=>load().then(()=>setMsg(i,j.msg))).catch(offline(i));
}
function pair(i,a){
  setMsg(i,'Sending…');
  fetch(`/fanctl?fan=${i}&do=${a}`).then(r=>r.text())
    .then(t=>setMsg(i,t+(a=='pair'?'. The fan beeps or its light flashes if it paired.':''))).catch(offline(i));
}
function lockLearn(on){document.querySelectorAll('button.learn').forEach(b=>b.disabled=on)}
function learn(i){
  setMsg(i,'Starting…');
  fetch('/learn?fan='+i).then(r=>r.json()).then(j=>{if(!j.ok)return setMsg(i,j.msg);watch()}).catch(offline(i));
}
function watch(){if(polling)return;lockLearn(true);polling=setInterval(tick,1000);tick()}
function tick(){
  fetch('/learnstatus').then(r=>r.json()).then(s=>{
    if(s.state=='listening')return setMsg(s.fan,`Listening… press a button on the remote, close to the board (${s.secs} s left)`);
    clearInterval(polling);polling=null;lockLearn(false);
    if(s.state=='done')load().then(()=>setMsg(s.fan,s.msg));else setMsg(s.fan,s.msg);
  }).catch(()=>{});
}
function wifi(){
  if(!confirm('Forget this WiFi network and restart into setup mode? You then join the FanBridge-Setup network to pick a new one.'))return;
  post('/wifireset').then(()=>restarting('Join the FanBridge-Setup WiFi network to pick a new network.',false)).catch(()=>alert("Couldn't reach the board."));
}
load().then(()=>fetch('/learnstatus')).then(r=>r.json()).then(s=>{if(s.state=='listening')watch()}).catch(()=>{});
</script></body></html>)HTML";
