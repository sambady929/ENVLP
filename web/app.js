/* The analysis side of the page: boot the WebAssembly module, drive the
 * analysis cards, and typeset / plot the result. The schematic editor lives in
 * editor.js and shares the same module instance.
 */
'use strict';

/* The build id: bump it whenever the front-end or the engine changes, so a
 * browser never mixes an old envlp.js with a new envlp.wasm (or keeps a
 * stale copy of either). */
const BUILD = '39';

const $ = (id) => document.getElementById(id);

function setStatus(msg, err) {
  const s = $('status');
  if (!s) return;
  s.textContent = msg || '';
  s.className = 'status' + (err ? ' err' : '');
}

/* ------------------------------------------------------------- the call --- */
function options() {
  const kind = $('kind').value;
  let output = $('output').value.trim();
  // The differential analysis measures an output port: two nodes, given as
  // V(a)-V(b) -- the way the app's out+ / out- fields compose.
  if (kind === 'Differential') {
    const p = $('out_p').value.trim(), n = $('out_n').value.trim();
    if (p || n) output = 'V(' + p + ')' + (n ? '-V(' + n + ')' : '');
  }
  return {
    kind: kind,
    input: $('input').value.trim(),
    output: output,
    port_p: $('port_p').value.trim(), port_n: $('port_n').value.trim(),
    // The reference amplifier: the part whose gain is set to zero to break
    // the loop (the loop-gain analysis calls it the probe).
    probe: $('probe').value.trim(),
    f0: parseFloat($('f0').value) || 1,
    f1: parseFloat($('f1').value) || 1e9,
    points: parseInt($('points').value, 10) || 10,
    sweep: $('sweep_type').value,
    dc_mode: parseInt($('dc_mode').value, 10) || 1,
    vdd: parseFloat($('vdd').value) || 5,
    vthn: parseFloat($('vthn').value) || 0.6,
    vthp: parseFloat($('vthp').value) || 0.6,
    prune: $('prune').checked,
    approx_factor: $('approx_factor').checked,
    assume_saturation: $('assume_sat').checked,
    // The low-entropy thresholds
    comp_ratio: parseFloat($('comp_ratio').value) || 10,
    pz_ratio: parseFloat($('pz_ratio').value) || 1000,
    pole_ref: $('pole_ref').value,
    // the operating point
    uncox: parseFloat($('uncox').value) || 200e-6,
    upcox: parseFloat($('upcox').value) || 100e-6,
    include_clm: $('include_clm').checked,
  };
}

/* ------------------------------------------------- which options apply --- */
//
// Every option field carries the list of analyses that need it, so the panel
// shows exactly what the chosen analysis uses -- and nothing else. (The
// operating point is not one of them: it is a property of the circuit, so it
// has its own section below the analysis options.)
function applyKind() {
  const k = $('kind').value;
  document.querySelectorAll('[data-kinds]').forEach((el) => {
    const kinds = (el.getAttribute('data-kinds') || '').split(/\s+/);
    el.hidden = kinds.indexOf(k) < 0;
  });
  // The operating point is the DC operating point analysis' own options, so
  // it is only shown for it -- as in the app, where it sits on the DC card.
  const box = $('dcbox');
  if (box) {
    box.hidden = (k !== 'DC');
    if (k === 'DC') box.open = true;
  }
  // The DC model decides which of its fields apply: the square law has the
  // uCox pair and the saturation switch, the gm/Id mode the CLM switch.
  const mode = parseInt($('dc_mode').value, 10) === 2 ? 'square' : 'gmoverid';
  document.querySelectorAll('#dcbox [data-mode]').forEach((el) => {
    el.hidden = el.getAttribute('data-mode') !== mode;
  });
}
$('dc_mode').addEventListener('change', applyKind);


let last = null;   // the last analysis result

function run() {
  const scx = docSerialize();
  if (!scx.trim()) { setStatus('nothing to analyse', true); return; }
  const btn = $('run');
  btn.disabled = true;
  setStatus('solving…');
  // let the browser paint the "solving" state before the (synchronous) solve
  setTimeout(() => {
    try {
      const res = JSON.parse(callStr2('envlp_run', scx, JSON.stringify(options())));
      if (!res.ok) {
        // A failed run must not leave the previous result (or its plot) up,
        // looking like the answer to this analysis.
        last = null;
        $('report').textContent = res.error || 'analysis failed';
        $('math').innerHTML = '';
        drawPlot();
        setStatus(res.error, true);
        return;
      }
      last = res;
      render(res);
      setStatus('');
    } catch (e) {
      // A thrown solver call is just as invalid as an ok:false result: do not
      // leave the previous plot up as if it answered this run.
      last = null;
      const msg = String((e && e.message) || e);
      $('report').textContent = msg;
      $('math').innerHTML = '';
      drawPlot();
      setStatus(msg, true);
    } finally {
      btn.disabled = false;
    }
  }, 20);
}
window.runAnalysis = run;

/* ---------------------------------------------------------- the results --- */
function render(res) {
  $('report').textContent = res.report || res.summary || '';

  const math = $('math');
  math.innerHTML = '';
  if (res.latex) {
    const d = document.createElement('div');
    d.className = 'line head';
    d.textContent = '\\[ ' + res.latex + ' \\]';
    math.appendChild(d);
  }
  (res.latex_report || '').split('\n').forEach((line) => {
    const t = line.replace(/\s+$/, '');
    if (!t) return;
    const d = document.createElement('div');
    d.className = 'line';
    if (t.endsWith(':') && t.indexOf('\\') < 0) d.textContent = t;
    else d.textContent = '\\[ \\displaystyle ' + t + ' \\]';
    math.appendChild(d);
  });
  if (window.MathJax && MathJax.typesetPromise) {
    MathJax.typesetPromise([math]).catch(() => {});
  }
  drawPlot();
}

/* ------------------------------------------------------------- the plot --- */
function currentSeries() {
  if (!last) return null;
  const p = last.plot || {};
  const f = p.f || [], mag = p.mag || [], ph = p.phase || [];
  const n = Math.min(f.length, mag.length, ph.length);
  return { f: f.slice(0, n), mag: mag.slice(0, n), ph: ph.slice(0, n) };
}

function nicholsGrid() {
  const traces = [];
  const push = (xs, ys) => traces.push({
    x: xs, y: ys, mode: 'lines', type: 'scatter',
    line: { color: '#d8dcea', width: 0.8 }, hoverinfo: 'skip',
    showlegend: false,
  });
  for (const mDb of [-12, -6, -3, 0, 3, 6, 12]) {
    for (let branch = 0; branch < (mDb > 0 ? 2 : 1); branch++) {
      const m = Math.pow(10, mDb / 20), xs = [], ys = [];
      for (let k = -180; k <= 0; k++) {
        const phi = k * Math.PI / 180, cp = Math.cos(phi), sp = Math.sin(phi);
        let g;
        if (Math.abs(m - 1) < 1e-9) {
          if (cp >= -1e-9) continue;
          g = -1 / (2 * cp);
        } else {
          const disc = 1 - m * m * sp * sp;
          if (disc < 0) continue;
          const root = m * Math.sqrt(disc);
          g = (branch === 0 ? (m * m * cp + root) : (m * m * cp - root)) / (1 - m * m);
        }
        if (g <= 0) continue;
        xs.push(k); ys.push(20 * Math.log10(g));
      }
      if (xs.length > 1) push(xs, ys);
    }
  }
  for (const alpha of [-150, -120, -90, -60, -30, 0, 30, 60, 90, 120, 150]) {
    const ta = Math.tan(alpha * Math.PI / 180), xs = [], ys = [];
    for (let k = -180; k <= 0; k++) {
      const phi = k * Math.PI / 180;
      const den = Math.sin(phi) - ta * Math.cos(phi);
      if (Math.abs(den) < 1e-9) continue;
      const g = ta / den;
      if (g <= 0) continue;
      xs.push(k); ys.push(20 * Math.log10(g));
    }
    if (xs.length > 1) push(xs, ys);
  }
  return traces;
}

function figureFor(mode, s) {
  const title = last ? last.title : '';
  if (mode === 'nyquist') {
    const re = [], im = [], cd = [];
    for (let i = 0; i < s.f.length; i++) {
      const m = Math.pow(10, s.mag[i] / 20), r = s.ph[i] * Math.PI / 180;
      re.push(m * Math.cos(r)); im.push(m * Math.sin(r));
      cd.push([s.f[i], s.mag[i], s.ph[i]]);
    }
    return {
      traces: [
        { x: re, y: im, customdata: cd, mode: 'lines', type: 'scatter',
          name: 'H(jw)', line: { color: '#c82828', width: 2 },
          hovertemplate: 'Re = %{x:.3g}   Im = %{y:.3g}<br>|H| = %{customdata[1]:.2f} dB   phase = %{customdata[2]:.1f} deg<br>f = %{customdata[0]:.4g} Hz<extra></extra>' },
        { x: [-1], y: [0], mode: 'markers', type: 'scatter', name: '-1',
          marker: { color: '#c82828', size: 9, symbol: 'x' },
          hovertemplate: 'critical point (-1, 0)<extra></extra>' },
      ],
      layout: {
        title: { text: title }, xaxis: { title: { text: 'Re' } },
        yaxis: { title: { text: 'Im' }, scaleanchor: 'x', scaleratio: 1 },
        showlegend: false, margin: { l: 60, r: 20, t: 40, b: 45 },
      },
    };
  }
  if (mode === 'nichols') {
    const cd = [], phLo = Math.min(-180, ...s.ph), phHi = Math.max(0, ...s.ph);
    let mLo = -40, mHi = 20;
    for (let i = 0; i < s.f.length; i++) {
      const Lm = Math.pow(10, s.mag[i] / 20);
      const Lr = Lm * Math.cos(s.ph[i] * Math.PI / 180);
      const Li = Lm * Math.sin(s.ph[i] * Math.PI / 180);
      const dr = 1 + Lr, di = Li, d2 = dr * dr + di * di;
      let gdb = 0, gph = 0;
      if (d2 > 0) {
        const gr = (Lr * dr + Li * di) / d2, gi = (Li * dr - Lr * di) / d2;
        gdb = 20 * Math.log10(Math.hypot(gr, gi));
        gph = Math.atan2(gi, gr) * 180 / Math.PI;
      }
      cd.push([s.f[i], gdb, gph]);
      mLo = Math.min(mLo, s.mag[i]); mHi = Math.max(mHi, s.mag[i]);
    }
    return {
      traces: nicholsGrid().concat([{
        x: s.ph, y: s.mag, customdata: cd, mode: 'lines', type: 'scatter',
        name: 'H(jw)', line: { color: '#2b8a3e', width: 2.2 },
        hovertemplate: 'open loop |H| = %{y:.2f} dB   phase = %{x:.1f} deg<br>closed loop |G| = %{customdata[1]:.2f} dB   phase = %{customdata[2]:.1f} deg<br>f = %{customdata[0]:.4g} Hz<extra></extra>',
      }]),
      layout: {
        title: { text: title },
        xaxis: { title: { text: 'phase [deg]' }, range: [phLo, phHi] },
        yaxis: { title: { text: '|H| [dB]' }, range: [mLo, mHi] },
        showlegend: false, margin: { l: 60, r: 20, t: 40, b: 45 },
      },
    };
  }
  if (mode === 'noise') {
    return {
      traces: [{ x: s.f, y: s.mag, mode: 'lines', type: 'scatter', name: 'S',
                 line: { color: '#c82828', width: 2 },
                 hovertemplate: 'f = %{x:.4g} Hz<br>S = %{y:.2f} dB<extra></extra>' }],
      layout: { title: { text: title }, xaxis: { title: { text: 'f [Hz]' }, type: 'log' },
                yaxis: { title: { text: 'S [dB]' } }, showlegend: false,
                margin: { l: 60, r: 20, t: 40, b: 45 } },
    };
  }
  return {
    traces: [
      { x: s.f, y: s.mag, mode: 'lines', type: 'scatter', name: '|H| dB',
        line: { color: '#c82828', width: 2 }, xaxis: 'x', yaxis: 'y',
        hovertemplate: 'f = %{x:.4g} Hz<br>|H| = %{y:.2f} dB<extra></extra>' },
      { x: s.f, y: s.ph, mode: 'lines', type: 'scatter', name: 'phase',
        line: { color: '#285ac8', width: 2 }, xaxis: 'x2', yaxis: 'y2',
        hovertemplate: 'f = %{x:.4g} Hz<br>phase = %{y:.1f} deg<extra></extra>' },
    ],
    layout: {
      title: { text: title },
      grid: { rows: 2, columns: 1, pattern: 'independent', roworder: 'top to bottom' },
      showlegend: false,
      xaxis: { title: { text: 'f [Hz]' }, type: 'log' },
      yaxis: { title: { text: '|H| [dB]' } },
      xaxis2: { title: { text: 'f [Hz]' }, type: 'log' },
      yaxis2: { title: { text: 'phase [deg]' } },
      margin: { l: 60, r: 20, t: 40, b: 45 },
    },
  };
}

function drawPlot() {
  const s = currentSeries();
  const el = $('plot');
  if (!s || !s.f.length) {
    // Purge the graph as well as the DOM: Plotly keeps its state on the node,
    // and a later analysis would otherwise try to react to a graph that is no
    // longer on the page -- leaving the "no plot" message up for good.
    if (window.Plotly) { try { Plotly.purge(el); } catch (e) { /* not a graph */ } }
    el.innerHTML = '<p class="status">this analysis has no frequency response to plot</p>';
    return;
  }
  el.innerHTML = '';
  const fig = figureFor($('plot-mode').value, s);
  // After a "no plot" result the element has been purged, so there is no
  // layout to react to. Start a fresh graph in that case; react() on a purged
  // node is what used to leave the "no plot" message up for good.
  if (el._fullLayout) {
    Plotly.react(el, fig.traces, fig.layout, { responsive: true, displaylogo: false });
  } else {
    Plotly.newPlot(el, fig.traces, fig.layout, { responsive: true, displaylogo: false });
  }
}

// The plot follows its pane when the layout changes (the gutters).
window.addEventListener('resize', () => {
  const el = $('plot');
  if (window.Plotly && el && el.children.length) {
    try { Plotly.Plots.resize(el); } catch (e) { /* no plot yet */ }
  }
});

/* ----------------------------------------------------------- the exports --- */
function download(name, text, mime) {
  const blob = new Blob([text], { type: mime || 'text/plain' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

function pythonSource() {
  const s = currentSeries();
  const arr = (v) => 'np.array([' + v.map((x) => (+x).toPrecision(10)).join(', ') + '])';
  const mode = $('plot-mode').value;
  const title = last ? last.title : '';
  const head = '"""ENVLP export: ' + title +
    '\n\nRequires: pip install plotly numpy\n"""\n' +
    'import numpy as np\nimport plotly.graph_objects as go\n\n';
  if (mode === 'nyquist') {
    const re = [], im = [];
    for (let i = 0; i < s.f.length; i++) {
      const m = Math.pow(10, s.mag[i] / 20), r = s.ph[i] * Math.PI / 180;
      re.push(m * Math.cos(r)); im.push(m * Math.sin(r));
    }
    return head + 'f = ' + arr(s.f) + '\nre = ' + arr(re) + '\nim = ' + arr(im) +
      '\n\nfig = go.Figure()\n' +
      "fig.add_trace(go.Scatter(x=re, y=im, mode='lines', name='H(jw)'," +
      " line=dict(color='#c82828', width=2)))\n" +
      "fig.add_trace(go.Scatter(x=[-1], y=[0], mode='markers', name='-1'," +
      " marker=dict(color='#c82828', size=9, symbol='x')))\n" +
      "fig.update_layout(title='" + title + "', xaxis_title='Re'," +
      " yaxis_title='Im', showlegend=False)\n" +
      "fig.update_yaxes(scaleanchor='x', scaleratio=1)\nfig.show()\n";
  }
  if (mode === 'nichols') {
    return head + 'f = ' + arr(s.f) + '\nphase_deg = ' + arr(s.ph) +
      '\nmag_db = ' + arr(s.mag) + '\n' +
      'L = 10 ** (mag_db / 20) * np.exp(1j * np.deg2rad(phase_deg))\n' +
      'G = L / (1 + L)\ncl_db = 20 * np.log10(np.abs(G))\n' +
      'cl_ph = np.rad2deg(np.angle(G))\n\nfig = go.Figure()\n' +
      "fig.add_trace(go.Scatter(x=phase_deg, y=mag_db, mode='lines'," +
      " name='H(jw)', line=dict(color='#2b8a3e', width=2.2)," +
      ' customdata=np.stack([f, cl_db, cl_ph], axis=-1)))\n' +
      "fig.update_layout(title='" + title + "', xaxis_title='phase [deg]'," +
      " yaxis_title='|H| [dB]', showlegend=False)\nfig.show()\n";
  }
  return head + 'f = ' + arr(s.f) + '\nmag_db = ' + arr(s.mag) +
    '\nphase_deg = ' + arr(s.ph) + '\n\nfig = go.Figure()\n' +
    "fig.add_trace(go.Scatter(x=f, y=mag_db, mode='lines', name='|H| dB'))\n" +
    "fig.add_trace(go.Scatter(x=f, y=phase_deg, mode='lines', name='phase'))\n" +
    "fig.update_layout(title='" + title + "', xaxis_type='log'," +
    ' showlegend=False)\nfig.show()\n';
}

function htmlSource() {
  const s = currentSeries();
  const fig = figureFor($('plot-mode').value, s);
  return '<!DOCTYPE html><html><head><meta charset="utf-8"><title>' +
    (last ? last.title : 'ENVLP') + '</title>' +
    '<script src="https://cdn.plot.ly/plotly-basic-2.35.2.min.js"><\/script>' +
    '<style>html,body{margin:0;height:100%}#plot{width:100%;height:100vh}</style>' +
    '</head><body><div id="plot"></div><script>' +
    'Plotly.newPlot("plot",' + JSON.stringify(fig.traces) + ',' +
    JSON.stringify(fig.layout) + ',{responsive:true});<\/script></body></html>';
}

/* ------------------------------------------------------------ the wiring --- */
function bindPanel() {
  $('run').addEventListener('click', run);
  $('kind').addEventListener('change', applyKind);
  applyKind();
  $('plot-mode').addEventListener('change', drawPlot);
  $('save-html').addEventListener('click', () => download('plot.html', htmlSource(), 'text/html'));
  $('save-py').addEventListener('click', () => download('plot.py', pythonSource(), 'text/x-python'));

  document.querySelectorAll('.tabs button').forEach((b) => {
    b.addEventListener('click', () => {
      document.querySelectorAll('.tabs button').forEach((x) => x.classList.remove('active'));
      document.querySelectorAll('.tab').forEach((x) => x.classList.remove('active'));
      b.classList.add('active');
      $('tab-' + b.dataset.tab).classList.add('active');
      if (b.dataset.tab === 'plot' && last) window.dispatchEvent(new Event('resize'));
    });
  });

  document.querySelectorAll('.tool').forEach((b) => {
    b.addEventListener('click', () => {
      // Switching away from the wire tool finishes the wire being drawn, so
      // it can still be joined to a wire it meets end to end.
      if (ED.tool === 'wire' && b.dataset.tool !== 'wire') endWiring();
      // The tool buttons are not placement arms: a palette ghost and a
      // queued net-label placement must not survive the switch (the desktop
      // clears both in set_tool).
      ED.placing = null;
      ED.labelQueue = [];
      ED.tool = b.dataset.tool;
      syncTools();
      syncPalette();
      draw();
    });
  });
  $('btn-undo').addEventListener('click', docUndo);
  $('btn-redo').addEventListener('click', docRedo);
  $('btn-open').addEventListener('click', () => $('file').click());
  $('file').addEventListener('change', (e) => {
    const f = e.target.files[0];
    if (!f) return;
    const r = new FileReader();
    r.onload = () => docOpen(r.result);
    r.readAsText(f);
  });
  $('btn-save').addEventListener('click', () => {
    download('schematic.scx', docSerialize(), 'text/plain');
  });
  $('btn-help').addEventListener('click', showHelp);
  $('help-close').addEventListener('click', () => $('help').close());
  $('components-close').addEventListener('click', () => $('components').close());
  $('copy-text').addEventListener('click', () => {
    copyToClipboard($('report').textContent, 'report copied');
  });
  $('copy-latex').addEventListener('click', () => {
    const l = last ? ((last.latex_report || '') + '\n' + (last.latex || '')).trim() : '';
    copyToClipboard(l || $('report').textContent, 'LaTeX copied');
  });
  $('clear-results').addEventListener('click', () => {
    last = null;
    $('report').textContent = '';
    $('math').innerHTML = '';
    $('plot').innerHTML = '';
    setStatus('');
  });
  // The net-label dialog: type the names, then click the nets.
  $('netlabel-cancel').addEventListener('click', () => $('netlabel').close());
  $('netlabel-ok').addEventListener('click', () => {
    const names = $('netlabel-input').value.split(/\s+/).filter(Boolean);
    $('netlabel').close();
    if (!names.length) return;
    ED.labelQueue = names;
    ED.tool = 'select';
    syncTools();
    setStatus('Net label: click a net to place "' + names[0] + '" — Esc stops.');
    draw();
  });
  $('netlabel-input').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') { e.preventDefault(); $('netlabel-ok').click(); }
  });
  initGutters();
}

// Copy to the clipboard, with a fallback for browsers that refuse the async
// clipboard API (an insecure context, say).
function copyToClipboard(text, what) {
  if (!text) { setStatus('nothing to copy yet', true); return; }
  const done = () => setStatus(what || 'copied');
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(text).then(done, () => fallbackCopy(text, done));
  } else {
    fallbackCopy(text, done);
  }
}
function fallbackCopy(text, done) {
  const ta = document.createElement('textarea');
  ta.value = text;
  ta.style.position = 'fixed';
  ta.style.opacity = '0';
  document.body.appendChild(ta);
  ta.select();
  try { document.execCommand('copy'); done(); } catch (e) { setStatus('could not copy', true); }
  document.body.removeChild(ta);
}

/* ------------------------------------------------------------- the help --- */
function showHelp() {
  const d = $('help');
  if (!d) return;
  if (!d.open) d.showModal();
}

/* ------------------------------------------------- resizable side panels --- */
//
// The palette, the canvas and the analysis panel are separated by thin
// dividers that can be dragged; the sizes are remembered between visits.
function initGutters() {
  const root = document.documentElement;
  const vars = ['--palette-w', '--dock-w', '--results-h', '--plot-w'];

  // The results pane and the plot pane start as percentages; make them pixel
  // values so they can be dragged, and restore any remembered sizes.
  const res = document.querySelector('.results');
  if (res) root.style.setProperty('--results-h', res.getBoundingClientRect().height + 'px');
  const plotPane = document.querySelector('.results-right');
  if (plotPane) root.style.setProperty('--plot-w', plotPane.getBoundingClientRect().width + 'px');
  try {
    vars.forEach((v) => {
      const s = localStorage.getItem('envlp' + v);
      if (s) root.style.setProperty(v, s);
    });
  } catch (e) { /* private mode: no remembered sizes */ }

  document.querySelectorAll('.gutter').forEach((g) => {
    const name = g.dataset.target;
    const dir = parseInt(g.dataset.dir || '1', 10);
    const row = g.classList.contains('gutter-h');

    g.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      try { g.setPointerCapture(e.pointerId); } catch (err) { /* no capture available */ }
      g.classList.add('dragging');
      const start = row ? e.clientY : e.clientX;
      const cur = parseFloat(getComputedStyle(root).getPropertyValue(name));
      const base = isNaN(cur) ? 0 : cur;

      const move = (ev) => {
        const now = row ? ev.clientY : ev.clientX;
        let v = base + dir * (now - start);
        const max = row ? window.innerHeight - 200 : window.innerWidth - 420;
        v = Math.max(row ? 90 : 100, Math.min(max, v));
        root.style.setProperty(name, v + 'px');
        // the canvas has to follow its box
        window.dispatchEvent(new Event('resize'));
      };      const up = (ev) => {
        g.classList.remove('dragging');
        try { g.releasePointerCapture(ev.pointerId); } catch (err) { /* already gone */ }
        g.removeEventListener('pointermove', move);
        g.removeEventListener('pointerup', up);
        g.removeEventListener('pointercancel', up);
        try { localStorage.setItem('envlp' + name, root.style.getPropertyValue(name)); }
        catch (err) { /* ignore */ }
      };
      g.addEventListener('pointermove', move);
      g.addEventListener('pointerup', up);
      g.addEventListener('pointercancel', up);
    });
  });
}

/* --------------------------------------------------------------- start-up --- */
window.Module = null;

// Open a shipped example, so the page never starts on an empty canvas (and never
// on a circuit the solver would reject).
function openExample(name) {
  fetch('examples/' + name)
    .then((r) => (r.ok ? r.text() : Promise.reject(new Error('missing ' + name))))
    .then((txt) => { docOpen(txt); setTimeout(frameAll, 30); })
    .catch(() => setStatus('could not load examples/' + name, true));
}

createEnvLp({ locateFile: (p) => (p.endsWith('.wasm') ? p + '?v=' + BUILD : p) })
  .then((m) => {
    window.Module = m;
    bindPanel();
    startEditor();
    openExample('rc_lowpass.scx');
  }).catch((e) => {
    setStatus('could not start the solver: ' + (e && e.message ? e.message : e), true);
  });

function frameAll() {
  const cs = (ED.doc.components || []);
  if (!cs.length) return;
  let x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
  for (const c of cs) {
    x0 = Math.min(x0, c.bbox[0]); y0 = Math.min(y0, c.bbox[1]);
    x1 = Math.max(x1, c.bbox[2]); y1 = Math.max(y1, c.bbox[3]);
  }
  const r = canvas().parentElement.getBoundingClientRect();
  const pad = 60;
  ED.zoom = Math.max(0.3, Math.min(2, Math.min((r.width - 2 * pad) / (x1 - x0 + 1),
                                               (r.height - 2 * pad) / (y1 - y0 + 1))));
  ED.pan.x = (r.width - (x0 + x1) * ED.zoom) / 2;
  ED.pan.y = (r.height - (y0 + y1) * ED.zoom) / 2;
  draw();
}
