"""The single page telemetry_ui.py serves, as one string.

Split out of telemetry_ui.py for the reason CLAUDE.md gives for every other
split in this tree: `python scripts/check_lines.py` counts scripts/*.py and the
server plus its markup does not fit under 1000 lines. The seam is the obvious
one -- HTTP and SQL on one side, the document on the other -- and it is the same
shape as dev_server.py holding the HTTP layer while sim_*.py hold what it
serves.

Nothing here is loaded from a network: no CDN, no charting library, no font.
The device's own pages are forbidden one because they must work offline, and a
LOCAL tool that needs the internet to draw a chart of an offline archive is
worse than that -- it fails at exactly the moment the archive is the only thing
left to look at. Charts are hand-drawn on <canvas>, which is also what
data/history.js does with inline SVG, for the same reason.
"""

PAGE = r"""<!doctype html>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>esp-garden telemetry archive</title>
<style>
:root {
  --bg: #12151a; --surface: #191d24; --surface-2: #21262f;
  --line: #2c333d; --text: #dfe4ea; --muted: #8b95a3;
  --ok: #4caf82; --warn: #d9a441; --bad: #d9605a; --accent: #5aa9e6;
  --s1: #5aa9e6; --s2: #4caf82; --s3: #d9a441; --s4: #b98bd9;
  --s5: #d9605a; --s6: #56c8c8; --s7: #c8b45a; --s8: #9aa7b8;
}
* { box-sizing: border-box; }
body {
  margin: 0; background: var(--bg); color: var(--text);
  font: 13px/1.45 "Segoe UI", system-ui, sans-serif;
}
code, .mono, table { font-family: Consolas, "SF Mono", monospace; }
a { color: var(--accent); }
header {
  display: flex; flex-wrap: wrap; gap: 14px; align-items: center;
  padding: 10px 16px; background: var(--surface); border-bottom: 1px solid var(--line);
  position: sticky; top: 0; z-index: 5;
}
header h1 { font-size: 14px; margin: 0; font-weight: 600; letter-spacing: .3px; }
.spacer { flex: 1; }
.pill {
  padding: 3px 9px; border-radius: 10px; font-size: 11px;
  background: var(--surface-2); border: 1px solid var(--line); white-space: nowrap;
}
.pill.ok { color: var(--ok); border-color: #2c4a3d; }
.pill.warn { color: var(--warn); border-color: #4a3f2c; }
.pill.bad { color: var(--bad); border-color: #4a2f2f; }
button {
  background: var(--surface-2); color: var(--text); border: 1px solid var(--line);
  border-radius: 4px; padding: 4px 10px; cursor: pointer; font-size: 12px;
}
button:hover:not(:disabled) { border-color: var(--accent); }
button:disabled { opacity: .5; cursor: default; }
button.on { background: var(--accent); border-color: var(--accent); color: #0d1117; }
nav { display: flex; gap: 2px; padding: 0 16px; background: var(--surface);
      border-bottom: 1px solid var(--line); }
nav button { border: 0; border-bottom: 2px solid transparent; border-radius: 0;
             background: none; padding: 8px 14px; color: var(--muted); }
nav button.on { color: var(--text); border-bottom-color: var(--accent); background: none; }
main { padding: 14px 16px 40px; }
.view { display: none; } .view.on { display: block; }
.row { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; margin-bottom: 10px; }
.note { color: var(--muted); font-size: 11.5px; }
.warnbox { border: 1px solid #4a3f2c; background: #1e1a14; color: var(--warn);
           padding: 7px 10px; border-radius: 4px; font-size: 12px; margin-bottom: 10px; }
table { width: 100%; border-collapse: collapse; font-size: 12px; }
th, td { text-align: left; padding: 4px 8px; border-bottom: 1px solid var(--line);
         white-space: nowrap; }
th { color: var(--muted); font-weight: 600; cursor: pointer; user-select: none; }
tbody tr:hover { background: var(--surface); }
td.num { text-align: right; }
.tag { font-size: 10px; padding: 1px 6px; border-radius: 8px; border: 1px solid var(--line);
       color: var(--muted); }
.tag.dead { color: var(--bad); border-color: #4a2f2f; background: #1e1414; }
.tag.num { color: var(--s1); } .tag.bool { color: var(--s2); } .tag.text { color: var(--s4); }
/* Key picker: a column list, because 43 keys in a <select multiple> hides the
   dead ones behind a scrollbar and the dead ones are the point. */
#picker { display: grid; grid-template-columns: repeat(auto-fill, minmax(210px, 1fr));
          gap: 2px 12px; max-height: 190px; overflow: auto; padding: 8px;
          background: var(--surface); border: 1px solid var(--line); border-radius: 4px; }
#picker label { display: flex; gap: 6px; align-items: center; cursor: pointer;
                font-size: 12px; }
#picker label.dead span.k { color: var(--bad); }
.panel { background: var(--surface); border: 1px solid var(--line); border-radius: 4px;
         margin-bottom: 8px; }
.panel .head { display: flex; gap: 10px; align-items: baseline; padding: 6px 10px;
               border-bottom: 1px solid var(--line); flex-wrap: wrap; }
.panel .head .name { font-weight: 600; font-size: 12.5px; }
.panel canvas { display: block; width: 100%; height: 132px; }
.panel.text canvas { height: 62px; }
.legend { display: flex; gap: 12px; flex-wrap: wrap; font-size: 11px; color: var(--muted); }
.sw { display: inline-block; width: 9px; height: 9px; border-radius: 2px;
      margin-right: 4px; vertical-align: -1px; }
#readout { position: fixed; pointer-events: none; background: #0d1117ee; color: var(--text);
           border: 1px solid var(--line); border-radius: 4px; padding: 5px 8px;
           font: 11px Consolas, monospace; display: none; z-index: 20; white-space: pre; }
</style>

<header>
  <h1>esp-garden telemetry archive</h1>
  <span id="fresh" class="pill">reading…</span>
  <span id="span" class="pill"></span>
  <span class="spacer"></span>
  <span id="syncmsg" class="note"></span>
  <button id="sync">Refresh from ThingsBoard</button>
</header>

<nav>
  <button data-view="chart" class="on">Chart</button>
  <button data-view="inv">Key inventory</button>
  <button data-view="latest">Latest values</button>
  <button data-view="boots">Boots &amp; gaps</button>
</nav>

<main>
  <section id="v-chart" class="view on">
    <div class="row">
      <span class="note">Window</span>
      <span id="windows"></span>
      <span class="spacer"></span>
      <button id="clearkeys">clear</button>
      <span id="chartnote" class="note"></span>
    </div>
    <div id="picker"></div>
    <div id="charts" style="margin-top:10px"></div>
  </section>

  <section id="v-inv" class="view">
    <div id="deadsummary"></div>
    <table id="invtable"><thead><tr>
      <th data-sort="key">key</th><th data-sort="kind">kind</th>
      <th data-sort="count" class="num">points</th><th data-sort="per_day" class="num">/day</th>
      <th data-sort="first">first</th><th data-sort="last">last</th>
      <th data-sort="age_ms" class="num">age</th><th data-sort="dead">status</th>
    </tr></thead><tbody></tbody></table>
  </section>

  <section id="v-latest" class="view">
    <p class="note">The newest stored point per key, and how old it is.</p>
    <table id="lattable"><thead><tr>
      <th data-sort="key">key</th><th data-sort="last_value">value</th>
      <th data-sort="last">at</th><th data-sort="age_ms" class="num">age</th>
      <th data-sort="dead">status</th>
    </tr></thead><tbody></tbody></table>
  </section>

  <section id="v-boots" class="view">
    <div class="row">
      <span class="note">Window</span><span id="windows2"></span>
      <span class="note" style="margin-left:10px">Gap threshold</span><span id="gapsel"></span>
      <span class="spacer"></span><span id="bootsum" class="note"></span>
    </div>
    <div class="panel"><div class="head"><span class="name">bootReason</span>
      <span id="bootlegend" class="legend"></span></div>
      <canvas id="bootstrip" style="height:56px"></canvas></div>
    <div class="row" style="align-items:flex-start; gap:18px">
      <div style="flex:1 1 340px; min-width:320px">
        <p class="note">Boots, newest first</p>
        <table id="boottable"><thead><tr><th>when</th><th>reason</th>
          <th class="num">free heap</th></tr></thead><tbody></tbody></table>
      </div>
      <div style="flex:1 1 340px; min-width:320px">
        <p class="note" id="gapnote"></p>
        <table id="gaptable"><thead><tr><th>from</th><th>to</th>
          <th class="num">length</th></tr></thead><tbody></tbody></table>
      </div>
    </div>
  </section>
</main>
<div id="readout"></div>

<script>
"use strict";
var SERIES = ['--s1','--s2','--s3','--s4','--s5','--s6','--s7','--s8'];
var WINDOWS = [['1h',3600],['6h',21600],['12h',43200],['1d',86400],
               ['7d',604800],['30d',2592000],['all',0]];
var GAPS = [5,10,15,30,60];
var state = { win: 86400, keys: [], inv: [], summary: null, series: {},
              gapMin: 5, sort: { inv: 'dead', latest: 'age_ms' }, desc: true };

function $(s) { return document.querySelector(s); }

// The view is in the URL, so a particular window over a particular set of keys
// is a link -- worth having when the answer to "look at this" is otherwise
// eleven clicks, and it is what makes the page testable from a headless
// browser at all.
// Saved across reloads, but the URL always wins. A link carrying ?win=30d is
// someone saying "look at THIS"; restoring the reader's own last window over it
// would quietly answer a different question than the one they were sent.
var STORE = 'telemetry_ui.v1';

function loadSaved() {
  try {
    return JSON.parse(localStorage.getItem(STORE) || 'null');
  } catch (e) {
    // Private windows and blocked site-data throw on ACCESS, not just on read,
    // so this has to be caught rather than checked for.
    return null;
  }
}
function saveState(view) {
  try {
    localStorage.setItem(STORE, JSON.stringify({
      view: view, win: state.win, gapMin: state.gapMin, keys: state.keys,
      sort: state.sort, desc: state.desc
    }));
  } catch (e) { /* nothing here is worth failing a render over */ }
}

function readUrl() {
  var q = new URLSearchParams(location.search);
  var fresh = !q.has('view') && !q.has('win') && !q.has('keys') && !q.has('gap');
  if (fresh) {
    var saved = loadSaved();
    if (saved) {
      if (typeof saved.win === 'number') state.win = saved.win;
      if (typeof saved.gapMin === 'number') state.gapMin = saved.gapMin;
      if (Array.isArray(saved.keys)) state.keys = saved.keys;
      if (saved.sort) state.sort = saved.sort;
      if (typeof saved.desc === 'boolean') state.desc = saved.desc;
      return saved.view || 'chart';
    }
  }
  if (q.has('win')) state.win = parseInt(q.get('win'), 10) || 0;
  if (q.has('gap')) state.gapMin = parseInt(q.get('gap'), 10) || 5;
  if (q.has('keys')) state.keys = q.get('keys').split(',').filter(Boolean);
  return q.get('view') || 'chart';
}
function writeUrl() {
  var q = new URLSearchParams();
  q.set('view', currentView());
  q.set('win', state.win);
  if (state.keys.length) q.set('keys', state.keys.join(','));
  if (currentView() === 'boots') q.set('gap', state.gapMin);
  history.replaceState(null, '', '?' + q.toString());
  saveState(currentView());
}
function currentView() {
  var on = document.querySelector('nav button.on');
  return on ? on.dataset.view : 'chart';
}
function el(tag, cls, text) {
  var e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}
function css(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}
function fmtTime(ms) {
  var d = new Date(ms), p = function (n) { return (n < 10 ? '0' : '') + n; };
  return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' ' +
         p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
}
function fmtClock(ms) {
  var d = new Date(ms), p = function (n) { return (n < 10 ? '0' : '') + n; };
  return p(d.getHours()) + ':' + p(d.getMinutes());
}
function fmtDay(ms) {
  var d = new Date(ms), p = function (n) { return (n < 10 ? '0' : '') + n; };
  return p(d.getMonth() + 1) + '-' + p(d.getDate());
}
function fmtInt(n) {
  return String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ' ');
}
function fmtAge(ms) {
  if (ms === null || ms === undefined) return '';
  var s = ms / 1000;
  if (s < 90) return Math.round(s) + ' s';
  if (s < 5400) return (s / 60).toFixed(s < 600 ? 1 : 0) + ' min';
  if (s < 172800) return (s / 3600).toFixed(1) + ' h';
  return (s / 86400).toFixed(1) + ' d';
}
function get(path) {
  return fetch(path).then(function (r) {
    return r.json().then(function (j) {
      if (!r.ok) throw new Error(j.error || ('HTTP ' + r.status));
      return j;
    });
  });
}

// ---------- header ----------
function renderSummary(s) {
  state.summary = s;
  var f = $('#fresh');
  var age = s.age_ms;
  f.textContent = 'newest datapoint ' + fmtAge(age) + ' old';
  f.className = 'pill ' + (age < 3600000 ? 'ok' : age < 21600000 ? 'warn' : 'bad');
  $('#span').textContent = fmtInt(s.rows) + ' rows · ' + s.keys +
      ' keys · ' + fmtTime(s.first) + ' → ' + fmtTime(s.last);
  if (!s.sync_enabled) {
    $('#sync').disabled = true;
    $('#syncmsg').textContent = 'sync disabled (--no-sync)';
  }
}

// ---------- the window ----------
function windowRange() {
  var s = state.summary;
  if (!s) return [0, 0];
  if (!state.win) return [s.first, s.last];
  return [s.last - state.win * 1000, s.last];
}

// ---------- chart ----------
// One canvas per key -- small multiples, never two units on one y axis. That is
// the rule data/history.js states for the device's own charts and it is the
// same rule here: moisture in percent and ping in milliseconds cannot share a
// scale honestly, and a chart is exactly where that kind of lie is not noticed.
function drawSeries(canvas, s, colour, x0, x1) {
  var dpr = window.devicePixelRatio || 1;
  var w = canvas.clientWidth, h = canvas.clientHeight;
  canvas.width = Math.round(w * dpr); canvas.height = Math.round(h * dpr);
  var g = canvas.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, w, h);
  var L = 46, R = 10, T = 8, B = 16;
  var pts = s.points || [];

  var lo = Infinity, hi = -Infinity;
  for (var i = 0; i < pts.length; i++) {
    if (pts[i][2] < lo) lo = pts[i][2];
    if (pts[i][3] > hi) hi = pts[i][3];
  }
  if (lo === Infinity) { lo = 0; hi = 1; }
  if (s.kind === 'bool') { lo = 0; hi = 1; }
  if (lo === hi) { lo -= 0.5; hi += 0.5; }
  var pad = (hi - lo) * 0.08; lo -= pad; hi += pad;

  var X = function (t) { return L + (t - x0) / (x1 - x0 || 1) * (w - L - R); };
  var Y = function (v) { return T + (1 - (v - lo) / (hi - lo)) * (h - T - B); };

  // grid + y labels
  g.strokeStyle = css('--line'); g.fillStyle = css('--muted');
  g.font = '10px Consolas, monospace'; g.lineWidth = 1;
  var ticks = s.kind === 'bool' ? [0, 1] : niceTicks(lo, hi, 3);
  for (var t = 0; t < ticks.length; t++) {
    var y = Math.round(Y(ticks[t])) + 0.5;
    g.beginPath(); g.moveTo(L, y); g.lineTo(w - R, y); g.stroke();
    g.textAlign = 'right'; g.fillText(fmtTick(ticks[t]), L - 5, y + 3);
  }
  // x labels
  g.textAlign = 'left'; g.fillText(labelFor(x0, x1 - x0), L, h - 4);
  g.textAlign = 'right'; g.fillText(labelFor(x1, x1 - x0), w - R, h - 4);

  // min/max band first, so the mean line sits on top of it. Without the band a
  // downsampled bucket shows only its mean and a one-sample spike -- the 45.04
  // degree reading -- vanishes at exactly the zoom someone is hunting it at.
  if (s.downsampled) {
    // One filled polygon per UNBROKEN run -- upper edge forward, lower edge
    // back. A run ends at a gap, so the band never spans one either.
    g.fillStyle = colour + '33';
    var run = [];
    for (i = 0; i <= pts.length; i++) {
      if (i === pts.length || (i > 0 && pts[i][5])) {
        if (run.length > 1) {
          g.beginPath();
          for (var k = 0; k < run.length; k++) {
            var p = pts[run[k]];
            g[k ? 'lineTo' : 'moveTo'](X(p[0]), Y(p[3]));
          }
          for (k = run.length - 1; k >= 0; k--) {
            p = pts[run[k]]; g.lineTo(X(p[0]), Y(p[2]));
          }
          g.closePath(); g.fill();
        }
        run = [];
      }
      if (i < pts.length) run.push(i);
    }
  }

  // the mean line, broken at every gap and at the seam
  g.strokeStyle = colour; g.lineWidth = 1.4; g.lineJoin = 'round';
  g.beginPath();
  var pen = false;
  for (i = 0; i < pts.length; i++) {
    var px = X(pts[i][0]), py = Y(pts[i][1]);
    if (!pen || pts[i][5]) { g.moveTo(px, py); pen = true; }
    else if (s.kind === 'bool') { g.lineTo(px, Y(pts[i - 1][1])); g.lineTo(px, py); }
    else g.lineTo(px, py);
  }
  g.stroke();
  if (pts.length === 1) {
    g.fillStyle = colour;
    g.beginPath(); g.arc(X(pts[0][0]), Y(pts[0][1]), 2.5, 0, 7); g.fill();
  }
  drawSeam(g, s, X, T, h - B, w);
  canvas._geom = { X: X, Y: Y, L: L, R: R, w: w, series: s };
}

// The probe index seam. Drawn on every panel whose key crosses it, with the
// label, because a line that continues across that instant draws two different
// physical sensors as one series.
function drawSeam(g, s, X, top, bottom, w) {
  if (!s.seam) return;
  var x = X(s.seam.ts);
  if (x < 0 || x > w) return;
  g.save();
  g.strokeStyle = css('--bad'); g.setLineDash([4, 3]); g.lineWidth = 1;
  g.beginPath(); g.moveTo(x, top); g.lineTo(x, bottom); g.stroke();
  g.setLineDash([]);
  g.fillStyle = css('--bad'); g.font = '10px Consolas, monospace';
  var right = x > w * 0.6;
  g.textAlign = right ? 'right' : 'left';
  g.fillText('index shift', x + (right ? -4 : 4), top + 9);
  g.restore();
}

function niceTicks(lo, hi, count) {
  var raw = (hi - lo) / count;
  if (!(raw > 0)) return [lo];
  var mag = Math.pow(10, Math.floor(Math.log(raw) / Math.LN10));
  var norm = raw / mag;
  var step = (norm >= 5 ? 10 : norm >= 2 ? 5 : norm >= 1 ? 2 : 1) * mag;
  var out = [];
  for (var t = Math.ceil(lo / step) * step; t <= hi; t += step) out.push(t);
  return out;
}
function fmtTick(v) {
  var a = Math.abs(v);
  return a >= 1000 ? v.toFixed(0) : a >= 10 ? v.toFixed(1) :
         a >= 1 ? v.toFixed(2) : v.toFixed(3).replace(/0+$/, '');
}
// Past six hours the two edge labels can land on the same clock time a day
// apart -- a 1 d window reads "19:28 … 19:28" -- so the date joins them.
function labelFor(ms, span) {
  return span > 21600000 ? fmtDay(ms) + ' ' + fmtClock(ms) : fmtClock(ms);
}

// Text keys are not a line chart. bootReason, firmware, relayNEvent and
// moisture1State are strings, and a string has no y axis -- they get a tick per
// event on the same time axis, so they can be read beside the numbers.
function drawEvents(canvas, s, x0, x1) {
  var dpr = window.devicePixelRatio || 1;
  var w = canvas.clientWidth, h = canvas.clientHeight;
  canvas.width = Math.round(w * dpr); canvas.height = Math.round(h * dpr);
  var g = canvas.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0); g.clearRect(0, 0, w, h);
  var L = 46, R = 10;
  var X = function (t) { return L + (t - x0) / (x1 - x0 || 1) * (w - L - R); };
  var values = [];
  (s.events || []).forEach(function (e) {
    if (values.indexOf(e[1]) < 0) values.push(e[1]);
  });
  g.strokeStyle = css('--line'); g.beginPath();
  g.moveTo(L, h - 16.5); g.lineTo(w - R, h - 16.5); g.stroke();
  (s.events || []).forEach(function (e) {
    var c = css(SERIES[values.indexOf(e[1]) % SERIES.length]);
    var x = X(e[0]);
    g.strokeStyle = c; g.lineWidth = 1.6;
    g.beginPath(); g.moveTo(x, 8); g.lineTo(x, h - 16); g.stroke();
  });
  g.fillStyle = css('--muted'); g.font = '10px Consolas, monospace';
  g.textAlign = 'left'; g.fillText(labelFor(x0, x1 - x0), L, h - 4);
  g.textAlign = 'right'; g.fillText(labelFor(x1, x1 - x0), w - R, h - 4);
  canvas._geom = { X: X, L: L, R: R, w: w, series: s, values: values };
  return values;
}

function renderCharts() {
  var host = $('#charts'); host.textContent = '';
  var range = windowRange();
  if (!state.keys.length) {
    host.appendChild(el('p', 'note', 'Pick one or more keys above.'));
    return;
  }
  state.keys.forEach(function (key, i) {
    var s = state.series[key];
    if (!s) return;
    var panel = el('div', 'panel' + (s.kind === 'text' ? ' text' : ''));
    var head = el('div', 'head');
    head.appendChild(el('span', 'name', key));
    head.appendChild(el('span', 'tag ' + s.kind, s.kind));
    if (s.dead) head.appendChild(el('span', 'tag dead', 'dead'));
    var note = el('span', 'note');
    if (s.kind === 'text') {
      note.textContent = s.shown + ' event' + (s.shown === 1 ? '' : 's') +
        (s.truncated ? ' (newest ' + s.shown + ' of ' + s.raw + ')' : '');
    } else if (!s.shown) {
      note.textContent = 'no points in this window';
    } else if (s.downsampled) {
      note.textContent = 'downsampled: ' + s.shown + ' buckets of ' +
        fmtAge(s.bucket_ms) + ' from ' + s.raw + ' points · band = bucket min/max';
    } else {
      note.textContent = s.shown + ' points, not downsampled';
    }
    head.appendChild(note);
    // The vertical rule says WHERE; the header says WHAT, because a dashed line
    // labelled "index shift" is a puzzle to anyone who has not read tb_export.
    if (s.seam) {
      var warn = el('span', 'tag dead',
                    fmtTime(s.seam.ts) + ' — ' + s.seam.note);
      head.appendChild(warn);
    }
    panel.appendChild(head);
    var canvas = el('canvas');
    panel.appendChild(canvas);
    host.appendChild(panel);
    if (s.kind === 'text') {
      var values = drawEvents(canvas, s, range[0], range[1]);
      var lg = el('span', 'legend');
      values.forEach(function (v, vi) {
        var sp = el('span');
        var sw = el('span', 'sw'); sw.style.background = css(SERIES[vi % SERIES.length]);
        sp.appendChild(sw); sp.appendChild(document.createTextNode(v));
        lg.appendChild(sp);
      });
      head.appendChild(lg);
    } else {
      drawSeries(canvas, s, css(SERIES[i % SERIES.length]), range[0], range[1]);
    }
    canvas.addEventListener('mousemove', function (ev) { hover(ev, canvas); });
    canvas.addEventListener('mouseleave', function () {
      $('#readout').style.display = 'none';
    });
  });
}

// The readout names the nearest stored point and its distance in time. It never
// invents a value between two points: if the pointer is inside a gap the
// distance is what says so.
function hover(ev, canvas) {
  var geom = canvas._geom;
  if (!geom) return;
  var rect = canvas.getBoundingClientRect();
  var px = ev.clientX - rect.left;
  var s = geom.series, best = null, bestD = Infinity;
  var list = s.kind === 'text' ? (s.events || []) : (s.points || []);
  for (var i = 0; i < list.length; i++) {
    var d = Math.abs(geom.X(list[i][0]) - px);
    if (d < bestD) { bestD = d; best = list[i]; }
  }
  var out = $('#readout');
  if (!best || bestD > 60) { out.style.display = 'none'; return; }
  var text;
  if (s.kind === 'text') text = fmtTime(best[0]) + '\n' + best[1];
  else {
    text = fmtTime(best[0]) + '\n' + s.key + ' = ' + best[1];
    if (best[4] > 1) text += '\nbucket min ' + best[2] + ' max ' + best[3] +
                             ' (' + best[4] + ' samples)';
  }
  out.textContent = text;
  out.style.display = 'block';
  out.style.left = Math.min(ev.clientX + 12, window.innerWidth - 240) + 'px';
  out.style.top = (ev.clientY + 14) + 'px';
}

function loadSeries() {
  writeUrl();
  if (!state.keys.length) { renderCharts(); return; }
  var range = windowRange();
  $('#chartnote').textContent = 'loading…';
  var t0 = performance.now();
  get('/api/series?keys=' + encodeURIComponent(state.keys.join(',')) +
      '&since=' + range[0] + '&until=' + range[1])
    .then(function (j) {
      state.series = {};
      j.series.forEach(function (s) { state.series[s.key] = s; });
      $('#chartnote').textContent = fmtInt(j.rows_scanned) +
        ' rows scanned in ' + j.query_ms + ' ms server-side, ' +
        Math.round(performance.now() - t0) + ' ms end to end';
      renderCharts();
    })
    .catch(function (e) { $('#chartnote').textContent = 'error: ' + e.message; });
}

// ---------- pickers ----------
function renderPicker() {
  var host = $('#picker'); host.textContent = '';
  state.inv.forEach(function (row) {
    var lab = el('label', row.dead ? 'dead' : '');
    var box = el('input'); box.type = 'checkbox'; box.value = row.key;
    box.checked = state.keys.indexOf(row.key) >= 0;
    box.addEventListener('change', function () {
      if (box.checked) { if (state.keys.indexOf(row.key) < 0) state.keys.push(row.key); }
      else state.keys = state.keys.filter(function (k) { return k !== row.key; });
      loadSeries();
    });
    lab.appendChild(box);
    lab.appendChild(el('span', 'k', row.key));
    lab.appendChild(el('span', 'tag ' + row.kind, row.kind));
    lab.title = row.count + ' points, last ' + fmtTime(row.last) +
                (row.dead ? ' — DEAD: ' + fmtAge(row.age_ms) + ' since the last one' : '');
    host.appendChild(lab);
  });
}

// Rendered into both hosts: the chart and the boot timeline share one window,
// and a selector that only exists on one tab means the other silently shows a
// range nothing on screen says it is showing.
function renderWindows() {
  ['#windows', '#windows2'].forEach(function (sel) {
    var host = $(sel); host.textContent = '';
    WINDOWS.forEach(function (w) {
      var b = el('button', state.win === w[1] ? 'on' : '', w[0]);
      b.addEventListener('click', function () {
        state.win = w[1]; renderWindows(); loadSeries();
        if ($('#v-boots').classList.contains('on')) loadBoots();
      });
      host.appendChild(b);
    });
  });
}

// ---------- tables ----------
function sortRows(rows, key, desc) {
  return rows.slice().sort(function (a, b) {
    var x = a[key], y = b[key];
    if (typeof x === 'string' || typeof y === 'string') {
      x = String(x); y = String(y);
      return desc ? (x < y ? 1 : x > y ? -1 : 0) : (x > y ? 1 : x < y ? -1 : 0);
    }
    return desc ? y - x : x - y;
  });
}

function renderInventory() {
  var dead = state.inv.filter(function (r) { return r.dead; });
  var box = $('#deadsummary'); box.textContent = '';
  if (dead.length) {
    var w = el('div', 'warnbox');
    w.textContent = dead.length + ' of ' + state.inv.length +
      ' keys are DEAD — nothing has arrived for far longer than that key\'s own ' +
      'publish interval: ' + dead.map(function (r) { return r.key; }).join(', ');
    box.appendChild(w);
  }
  var body = $('#invtable tbody'); body.textContent = '';
  sortRows(state.inv, state.sort.inv, state.desc).forEach(function (r) {
    var tr = el('tr');
    tr.appendChild(el('td', '', r.key));
    var k = el('td'); k.appendChild(el('span', 'tag ' + r.kind, r.kind)); tr.appendChild(k);
    tr.appendChild(el('td', 'num', fmtInt(r.count)));
    tr.appendChild(el('td', 'num', r.per_day.toFixed(0)));
    tr.appendChild(el('td', '', fmtTime(r.first)));
    tr.appendChild(el('td', '', fmtTime(r.last)));
    tr.appendChild(el('td', 'num', fmtAge(r.age_ms)));
    var st = el('td');
    if (r.dead) st.appendChild(el('span', 'tag dead', 'DEAD'));
    else st.appendChild(el('span', 'tag', 'live'));
    tr.appendChild(st);
    body.appendChild(tr);
  });
}

function renderLatest() {
  var body = $('#lattable tbody'); body.textContent = '';
  sortRows(state.inv, state.sort.latest, state.desc).forEach(function (r) {
    var tr = el('tr');
    tr.appendChild(el('td', '', r.key));
    tr.appendChild(el('td', '', r.last_value));
    tr.appendChild(el('td', '', fmtTime(r.last)));
    tr.appendChild(el('td', 'num', fmtAge(r.age_ms)));
    var st = el('td');
    if (r.dead) st.appendChild(el('span', 'tag dead', 'DEAD'));
    tr.appendChild(st);
    body.appendChild(tr);
  });
}

function wireSort(table, which, rerender) {
  table.querySelectorAll('th[data-sort]').forEach(function (th) {
    th.addEventListener('click', function () {
      var col = th.dataset.sort;
      if (state.sort[which] === col) state.desc = !state.desc;
      else { state.sort[which] = col; state.desc = true; }
      rerender();
    });
  });
}

// ---------- boots and gaps ----------
var bootData = null;
function loadBoots() {
  writeUrl();
  var range = windowRange();
  get('/api/boots?since=' + range[0] + '&until=' + range[1] +
      '&gap_min=' + state.gapMin)
    .then(function (j) { bootData = j; renderBoots(); })
    .catch(function (e) { $('#bootsum').textContent = 'error: ' + e.message; });
}

function renderBoots() {
  if (!bootData) return;
  var j = bootData, range = windowRange();
  var reasons = Object.keys(j.by_reason).sort();
  $('#bootsum').textContent = j.boots.length + ' boots · ' +
    reasons.map(function (r) { return j.by_reason[r] + ' ' + r; }).join(' · ');

  var lg = $('#bootlegend'); lg.textContent = '';
  reasons.forEach(function (r, i) {
    var sp = el('span');
    var sw = el('span', 'sw'); sw.style.background = css(SERIES[i % SERIES.length]);
    sp.appendChild(sw);
    sp.appendChild(document.createTextNode(r + ' (' + j.by_reason[r] + ')'));
    lg.appendChild(sp);
  });

  var canvas = $('#bootstrip');
  var dpr = window.devicePixelRatio || 1;
  var w = canvas.clientWidth, h = canvas.clientHeight;
  canvas.width = Math.round(w * dpr); canvas.height = Math.round(h * dpr);
  var g = canvas.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0); g.clearRect(0, 0, w, h);
  var L = 46, R = 10;
  var X = function (t) { return L + (t - range[0]) / (range[1] - range[0] || 1) * (w - L - R); };
  // Gaps behind the boots, so a reboot storm and the outage it caused are read
  // as one picture rather than two tables.
  g.fillStyle = css('--bad') + '22';
  j.gaps.forEach(function (gp) { g.fillRect(X(gp[0]), 6, Math.max(1, X(gp[1]) - X(gp[0])), h - 22); });
  g.strokeStyle = css('--line'); g.beginPath();
  g.moveTo(L, h - 15.5); g.lineTo(w - R, h - 15.5); g.stroke();
  j.boots.forEach(function (b) {
    g.strokeStyle = css(SERIES[reasons.indexOf(b[1]) % SERIES.length]);
    g.lineWidth = 1.5;
    g.beginPath(); g.moveTo(X(b[0]), 6); g.lineTo(X(b[0]), h - 15); g.stroke();
  });
  g.fillStyle = css('--muted'); g.font = '10px Consolas, monospace';
  g.textAlign = 'left'; g.fillText(fmtTime(range[0]), L, h - 3);
  g.textAlign = 'right'; g.fillText(fmtTime(range[1]), w - R, h - 3);

  var body = $('#boottable tbody'); body.textContent = '';
  j.boots.slice().reverse().forEach(function (b) {
    var tr = el('tr');
    tr.appendChild(el('td', '', fmtTime(b[0])));
    tr.appendChild(el('td', '', b[1]));
    tr.appendChild(el('td', 'num', b[2] === null ? '' : b[2] + ' KB'));
    body.appendChild(tr);
  });

  $('#gapnote').textContent = j.gaps.length + ' gap' + (j.gaps.length === 1 ? '' : 's') +
    ' over ' + state.gapMin + ' min in this window. ' + j.gap_note;
  body = $('#gaptable tbody'); body.textContent = '';
  j.gaps.slice().sort(function (a, b) { return (b[1] - b[0]) - (a[1] - a[0]); })
    .forEach(function (gp) {
      var tr = el('tr');
      tr.appendChild(el('td', '', fmtTime(gp[0])));
      tr.appendChild(el('td', '', fmtTime(gp[1])));
      tr.appendChild(el('td', 'num', fmtAge(gp[1] - gp[0])));
      body.appendChild(tr);
    });
}

function renderGapSel() {
  var host = $('#gapsel'); host.textContent = '';
  GAPS.forEach(function (m) {
    var b = el('button', state.gapMin === m ? 'on' : '', m + ' min');
    b.addEventListener('click', function () {
      state.gapMin = m; renderGapSel(); loadBoots();
    });
    host.appendChild(b);
  });
}

// ---------- sync ----------
var syncTimer = null;
function pollSync() {
  get('/api/sync').then(function (j) {
    $('#syncmsg').textContent = j.message || '';
    $('#sync').disabled = j.running || !j.enabled;
    if (!j.running) {
      clearInterval(syncTimer); syncTimer = null;
      if (j.finished) boot();
    }
  });
}
function startSync() {
  $('#sync').disabled = true;
  $('#syncmsg').textContent = 'syncing…';
  fetch('/api/sync', { method: 'POST' })
    .then(function (r) { return r.json(); })
    .then(function () {
      if (!syncTimer) syncTimer = setInterval(pollSync, 1500);
    });
}

// ---------- boot ----------
function boot() {
  return get('/api/summary').then(function (s) {
    renderSummary(s);
    return get('/api/inventory');
  }).then(function (j) {
    state.inv = j.keys;
    var have = {};
    j.keys.forEach(function (r) { have[r.key] = 1; });
    state.keys = state.keys.filter(function (k) { return have[k]; });
    if (!state.keys.length) state.keys = j.default_keys.slice();
    renderPicker(); renderInventory(); renderLatest();
    loadSeries();
    if ($('#v-boots').classList.contains('on')) loadBoots();
  }).catch(function (e) {
    $('#fresh').textContent = 'error: ' + e.message;
    $('#fresh').className = 'pill bad';
  });
}

function showView(name) {
  document.querySelectorAll('nav button').forEach(function (o) {
    o.classList.toggle('on', o.dataset.view === name);
  });
  document.querySelectorAll('.view').forEach(function (v) {
    v.classList.toggle('on', v.id === 'v-' + name);
  });
  writeUrl();
  if (name === 'boots') loadBoots();
  if (name === 'chart') renderCharts();
}
document.querySelectorAll('nav button').forEach(function (b) {
  b.addEventListener('click', function () { showView(b.dataset.view); });
});
$('#sync').addEventListener('click', startSync);
$('#clearkeys').addEventListener('click', function () {
  state.keys = []; renderPicker(); renderCharts();
});
wireSort($('#invtable'), 'inv', renderInventory);
wireSort($('#lattable'), 'latest', renderLatest);
window.addEventListener('resize', function () {
  renderCharts();
  if ($('#v-boots').classList.contains('on')) renderBoots();
});
var startView = readUrl();
renderWindows(); renderGapSel();
boot().then(function () { if (startView !== 'chart') showView(startView); });
</script>
"""
