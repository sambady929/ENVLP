/* The schematic editor.
 *
 * The model lives in C++ (envlp::Document, compiled to WebAssembly): components,
 * wire bindings, waypoints, net naming and .scx serialisation are all the same
 * code the desktop app uses, so there is one implementation of the electrical
 * rules. This file is the canvas front-end: it renders the geometry the backend
 * hands back and sends batched edits to it.
 *
 *   envlp_doc_open(scx)        -> geometry JSON
 *   envlp_doc_geometry()       -> geometry JSON
 *   envlp_doc_set(ops_json)    -> geometry JSON   (one undo step per batch)
 *   envlp_doc_serialize()      -> {scx}
 *   envlp_doc_palette()        -> {kinds:[...]}
 *   envlp_symbol_art()         -> {kinds:{R:{stroke:[{d,w}],fill:[d],text:[...]}}}
 *   envlp_doc_undo() / envlp_doc_redo()
 *
 * Geometry: {components:[{ref,kind,x,y,rot,flip_h,flip_v,value,bbox,pins:[{i,x,y,net}]}],
 *            wires:[{i,pts:[[x,y]..],a:{pin,ref,index},b:{..},net}],
 *            labels:[{i,name,x,y}], nets:[{name}]}
 */
'use strict';

const ED = {
  doc: null,            // the geometry from the backend
  // Module is a global set by app.js once the WebAssembly module is ready.
  art: null,            // symbol artwork, kind -> {stroke:[Path2D],fill:[Path2D]}
  tool: 'select',
  placing: null,        // kind token armed for placement
  placeRot: 0,          // its rotation, in degrees
  placeFlipH: false,
  placeFlipV: false,
  selection: new Set(), // selected component refs
  selWire: -1,          // selected wire index
  drag: null,           // {mode:'move'|'marquee'|'pan', ...}
  wireFrom: null,       // {x,y,pin:{ref,index}|null}
  pan: { x: 0, y: 0 },  // view offset in world units
  zoom: 1,
  mouse: { x: 0, y: 0 },
  palette: [],
};

/* ------------------------------------------------------------ backend ---- */
function callStr(name, arg) {
  const p = arg === undefined
    ? Module.ccall(name, 'number', [], [])
    : Module.ccall(name, 'number', ['string'], [arg]);
  const s = Module.UTF8ToString(p);
  Module.ccall('envlp_free', null, ['number'], [p]);
  return s;
}
function backend(name, arg) { return JSON.parse(callStr(name, arg)); }

// envlp_run takes two strings: the schematic and the options JSON.
function callStr2(name, a, b) {
  const p = Module.ccall(name, 'number', ['string', 'string'], [a, b]);
  const s = Module.UTF8ToString(p);
  Module.ccall('envlp_free', null, ['number'], [p]);
  return s;
}

function docOpen(scx) {
  ED.doc = JSON.parse(callStr('envlp_doc_open', scx || ''));
  ED.selection.clear();
  ED.selWire = -1;
  afterChange();
}
function docSet(ops) {
  if (!ops.length) return;
  ED.doc = JSON.parse(callStr('envlp_doc_set', JSON.stringify(ops)));
  afterChange();
}
function docGeometry() { return backend('envlp_doc_geometry'); }
function docSerialize() { return backend('envlp_doc_serialize').scx; }
function docUndo() {
  Module.ccall('envlp_doc_undo', null, [], []);
  ED.doc = docGeometry();
  afterChange();
}
function docRedo() {
  Module.ccall('envlp_doc_redo', null, [], []);
  ED.doc = docGeometry();
  afterChange();
}

function afterChange() {
  // selection may reference deleted components
  const live = new Set((ED.doc.components || []).map((c) => c.ref));
  ED.selection.forEach((r) => { if (!live.has(r)) ED.selection.delete(r); });
  if (ED.selWire >= (ED.doc.wires || []).length) ED.selWire = -1;
  if (ED.doc.error) setStatus(ED.doc.error, true);
  renderProps();
  draw();
}

/* ------------------------------------------------------------- the view --- */
const canvas = () => document.getElementById('canvas');

function toWorld(px, py) {
  const r = canvas().getBoundingClientRect();
  return { x: (px - r.left - ED.pan.x) / ED.zoom,
           y: (py - r.top - ED.pan.y) / ED.zoom };
}
function toScreen(x, y) {
  return { x: x * ED.zoom + ED.pan.x, y: y * ED.zoom + ED.pan.y };
}
const snap = (v) => Math.round(v / 10) * 10;

function resize() {
  const c = canvas();
  const r = c.parentElement.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  c.width = Math.max(1, Math.floor(r.width * dpr));
  c.height = Math.max(1, Math.floor(r.height * dpr));
  c.style.width = r.width + 'px';
  c.style.height = r.height + 'px';
  draw();
}
window.addEventListener('resize', resize);

/* ------------------------------------------------------------ rendering --- */
function buildArt() {
  const a = backend('envlp_symbol_art');
  ED.art = {};
  for (const [kind, v] of Object.entries(a.kinds)) {
    ED.art[kind] = {
      stroke: (v.stroke || []).map((s) => ({ p: new Path2D(s.d), w: s.w })),
      fill: (v.fill || []).map((d) => new Path2D(d)),
      text: v.text || [],
    };
  }
}

function draw() {
  const c = canvas();
  if (!c) return;
  const dpr = window.devicePixelRatio || 1;
  const ctx = c.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  const W = c.width / dpr, H = c.height / dpr;
  ctx.fillStyle = '#ffffff';
  ctx.fillRect(0, 0, W, H);

  drawGrid(ctx, W, H);
  ctx.save();
  ctx.translate(ED.pan.x, ED.pan.y);
  ctx.scale(ED.zoom, ED.zoom);

  const doc = ED.doc || { components: [], wires: [], labels: [] };
  drawWires(ctx, doc);
  drawComponents(ctx, doc);
  drawLabels(ctx, doc);
  if (ED.wireFrom) drawWirePreview(ctx);

  ctx.restore();
  drawHud(ctx, W, H);
}

function drawGrid(ctx, W, H) {
  const step = 10 * ED.zoom;
  if (step < 4) return;
  const x0 = ED.pan.x % (step * 5), y0 = ED.pan.y % (step * 5);
  ctx.fillStyle = '#d3d8e0';
  const dot = Math.max(1, Math.round(ED.zoom));
  for (let y = y0 % step; y < H; y += step) {
    for (let x = x0 % step; x < W; x += step) ctx.fillRect(x, y, dot, dot);
  }
}

function drawWires(ctx, doc) {
  ctx.lineCap = 'round';
  for (const w of doc.wires || []) {
    const sel = w.i === ED.selWire;
    ctx.strokeStyle = sel ? '#2f6fed' : '#10141a';
    ctx.lineWidth = sel ? 2.6 : 1.6;
    ctx.beginPath();
    w.pts.forEach((p, i) => (i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1])));
    ctx.stroke();
    // junction dots where three or more wire ends meet
    for (const p of [w.pts[0], w.pts[w.pts.length - 1]]) {
      let n = 0;
      for (const o of doc.wires || [])
        for (const q of [o.pts[0], o.pts[o.pts.length - 1]])
          if (Math.abs(p[0] - q[0]) < 1 && Math.abs(p[1] - q[1]) < 1) ++n;
      if (n > 1) {
        ctx.fillStyle = '#10141a';
        ctx.beginPath();
        ctx.arc(p[0], p[1], 3.2, 0, 2 * Math.PI);
        ctx.fill();
      }
    }
  }
}

function drawComponents(ctx, doc) {
  for (const c of doc.components || []) {
    const art = ED.art && ED.art[c.kind];
    const selected = ED.selection.has(c.ref);
    ctx.save();
    ctx.translate(c.x, c.y);
    ctx.rotate((c.rot || 0) * Math.PI / 180);
    ctx.scale(c.flip_h ? -1 : 1, c.flip_v ? -1 : 1);
    if (art) {
      ctx.fillStyle = '#10141a';
      for (const f of art.fill) ctx.fill(f);
      ctx.lineCap = 'round';
      ctx.lineJoin = 'round';
      for (const s of art.stroke) {
        ctx.strokeStyle = selected ? '#2f6fed' : '#10141a';
        ctx.lineWidth = s.w;
        ctx.stroke(s.p);
      }
      for (const t of art.text) {
        ctx.save();
        ctx.fillStyle = selected ? '#2f6fed' : '#10141a';
        ctx.font = (t.b ? 'bold 15px ' : '12px ') + "'Segoe UI', sans-serif";
        ctx.textAlign = t.c ? 'center' : 'left';
        ctx.textBaseline = t.c ? 'middle' : 'alphabetic';
        ctx.fillText(t.s, t.x, t.y);
        ctx.restore();
      }
    }
    ctx.restore();

    // reference + value, placed by the front-end. Ground carries no text at
    // all and every supply rail is simply called VDD -- the rail is one net,
    // so numbering it would only be noise.
    const horiz = (c.bbox[2] - c.bbox[0]) >= (c.bbox[3] - c.bbox[1]);
    const isGnd = c.kind === 'GND';
    const isVdd = c.kind === 'VDD';
    ctx.fillStyle = selected ? '#2f6fed' : '#10141a';
    ctx.font = "italic bold 12px 'Segoe UI', sans-serif";
    ctx.textAlign = 'center';
    if (isGnd) {
      // no label
    } else if (isVdd) {
      if (horiz) {
        ctx.textBaseline = 'bottom';
        ctx.fillText('VDD', (c.bbox[0] + c.bbox[2]) / 2, c.bbox[1] - 2);
      } else {
        ctx.textAlign = 'left';
        ctx.textBaseline = 'bottom';
        ctx.fillText('VDD', c.bbox[2] + 4, (c.bbox[1] + c.bbox[3]) / 2 - 1);
      }
    } else if (horiz) {
      ctx.textBaseline = 'bottom';
      ctx.fillText(c.ref, (c.bbox[0] + c.bbox[2]) / 2, c.bbox[1] - 2);
      if (c.value) {
        ctx.fillStyle = '#6b7284';
        ctx.font = "12px 'Segoe UI', sans-serif";
        ctx.textBaseline = 'top';
        ctx.fillText(c.value + (c.mult !== c.den ? ' ×' + c.mult + '/' + c.den : (c.mult > 1 ? ' ×' + c.mult : '')), (c.bbox[0] + c.bbox[2]) / 2, c.bbox[3] + 2);
      }
    } else {
      ctx.textAlign = 'left';
      ctx.textBaseline = 'bottom';
      ctx.fillText(c.ref, c.bbox[2] + 4, (c.bbox[1] + c.bbox[3]) / 2 - 1);
      if (c.value) {
        ctx.fillStyle = '#6b7284';
        ctx.font = "12px 'Segoe UI', sans-serif";
        ctx.textBaseline = 'top';
        ctx.fillText(c.value, c.bbox[2] + 4, (c.bbox[1] + c.bbox[3]) / 2 + 1);
      }
    }

    // pins
    for (const p of c.pins || []) {
      ctx.fillStyle = selected ? '#2f6fed' : '#6b7284';
      ctx.beginPath();
      ctx.arc(p.x, p.y, ED.tool === 'wire' || ED.placing ? 3.4 : 2, 0, 2 * Math.PI);
      ctx.fill();
    }
  }
}

function drawLabels(ctx, doc) {
  ctx.font = "bold 12px 'Segoe UI', sans-serif";
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  for (const l of doc.labels || []) {
    const text = l.name || '(unnamed)';
    const w = ctx.measureText(text).width + 8;
    ctx.fillStyle = '#e8f0fe';
    ctx.strokeStyle = '#2f6fed';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.rect(l.x - w / 2, l.y - 9, w, 18);
    ctx.fill();
    ctx.stroke();
    ctx.fillStyle = '#1f2430';
    ctx.fillText(text, l.x, l.y);
  }
}

function drawWirePreview(ctx) {
  const a = ED.wireFrom, b = ED.mouse;
  ctx.strokeStyle = '#2f6fed';
  ctx.lineWidth = 1.6;
  ctx.setLineDash([5, 4]);
  ctx.beginPath();
  ctx.moveTo(a.x, a.y);
  // orthogonal preview, like the router produces
  if (Math.abs(b.x - a.x) > Math.abs(b.y - a.y)) {
    ctx.lineTo((a.x + b.x) / 2, a.y);
    ctx.lineTo((a.x + b.x) / 2, b.y);
  } else {
    ctx.lineTo(a.x, (a.y + b.y) / 2);
    ctx.lineTo(b.x, (a.y + b.y) / 2);
  }
  ctx.lineTo(b.x, b.y);
  ctx.stroke();
  ctx.setLineDash([]);
}

function drawHud(ctx, W, H) {
  ctx.fillStyle = '#6b7284';
  ctx.font = '12px "Segoe UI", sans-serif';
  ctx.textAlign = 'left';
  ctx.textBaseline = 'alphabetic';
  let hint;
  if (ED.placing) {
    hint = 'click to place ' + ED.placing +
      (ED.placeRot ? '  (rotated ' + ED.placeRot + '°' +
        (ED.placeFlipH ? ', flipped' : '') + (ED.placeFlipV ? ', flipped' : '') + ')' : '') +
      '  (Space rotates, Esc cancels)';
  } else if (ED.tool === 'wire') {
    hint = ED.wireFrom ? 'click the end point  (Esc cancels)' : 'click a pin or a point to start a wire';
  } else if (ED.selWire >= 0) {
    hint = 'wire selected — Del removes it, N names its net';
  } else if (ED.selection.size) {
    hint = ED.selection.size + ' selected — drag to move, Space rotates, Del removes';
  } else {
    hint = 'pick a part on the left (or press R, C, L, V, O…), or W to wire';
  }
  ctx.fillText(hint, 10, H - 8);
  const d = ED.doc || {};
  ctx.textAlign = 'right';
  ctx.fillText((d.components || []).length + ' parts · ' + (d.wires || []).length +
               ' wires · ' + Math.round(ED.zoom * 100) + '%', W - 10, H - 8);
}

/* --------------------------------------------------------- hit-testing --- */
function hitComponent(w) {
  for (let i = (ED.doc.components || []).length - 1; i >= 0; --i) {
    const c = ED.doc.components[i];
    const b = c.bbox;
    if (w.x >= b[0] - 3 && w.x <= b[2] + 3 && w.y >= b[1] - 3 && w.y <= b[3] + 3)
      return c;
  }
  return null;
}
function hitPin(w) {
  let best = null, bd = 12 / ED.zoom;
  for (const c of ED.doc.components || [])
    for (const p of c.pins || []) {
      const d = Math.hypot(p.x - w.x, p.y - w.y);
      if (d < bd) { bd = d; best = { ref: c.ref, index: p.i, x: p.x, y: p.y }; }
    }
  return best;
}
function hitWire(w) {
  let best = -1, bd = 8 / ED.zoom;
  for (const wire of ED.doc.wires || []) {
    for (let i = 1; i < wire.pts.length; ++i) {
      const [x0, y0] = wire.pts[i - 1], [x1, y1] = wire.pts[i];
      const vx = x1 - x0, vy = y1 - y0, L2 = vx * vx + vy * vy;
      let t = L2 ? ((w.x - x0) * vx + (w.y - y0) * vy) / L2 : 0;
      t = Math.max(0, Math.min(1, t));
      const d = Math.hypot(w.x - (x0 + t * vx), w.y - (y0 + t * vy));
      if (d < bd) { bd = d; best = wire.i; }
    }
  }
  return best;
}

/* ----------------------------------------------------------- the events --- */
function bindCanvas() {
  const c = canvas();
  c.addEventListener('mousedown', (e) => {
    c.focus();
    const w = toWorld(e.clientX, e.clientY);
    if (e.button === 1 || e.button === 2 || e.shiftKey && e.button === 0 && e.altKey) {
      ED.drag = { mode: 'pan', sx: e.clientX, sy: e.clientY, px: ED.pan.x, py: ED.pan.y };
      return;
    }
    if (e.button !== 0) return;

    if (ED.placing) {
      const x = snap(w.x), y = snap(w.y);
      const op = { op: 'add', kind: ED.placing, x: x, y: y,
                   rot: ED.placeRot,
                   flip_h: ED.placeFlipH ? 1 : 0,
                   flip_v: ED.placeFlipV ? 1 : 0 };
      const kindInfo = ED.palette.find((k) => k.token === ED.placing);
      if (kindInfo && (kindInfo.token === 'V' || kindInfo.token === 'I'))
        op.value = '';
      docSet([op]);
      return;   // stay armed: several can be dropped in a row
    }

    if (ED.tool === 'wire') {
      const pin = hitPin(w);
      if (!ED.wireFrom) {
        ED.wireFrom = pin ? { x: pin.x, y: pin.y, pin } : { x: snap(w.x), y: snap(w.y), pin: null };
        draw();
        return;
      }
      const endPin = hitPin(w);
      const to = endPin ? { x: endPin.x, y: endPin.y } : { x: snap(w.x), y: snap(w.y) };
      const op = {
        op: 'wire',
        x0: ED.wireFrom.x, y0: ED.wireFrom.y,
        x1: to.x, y1: to.y,
        start_pin: ED.wireFrom.pin ? 'true' : 'false',
        start_ref: ED.wireFrom.pin ? ED.wireFrom.pin.ref : '',
        start_index: ED.wireFrom.pin ? String(ED.wireFrom.pin.index) : '0',
        end_pin: endPin ? 'true' : 'false',
        end_ref: endPin ? endPin.ref : '',
        end_index: endPin ? String(endPin.index) : '0',
      };
      ED.wireFrom = null;
      docSet([op]);
      return;
    }

    // select / start a move
    const comp = hitComponent(w);
    if (comp) {
      if (e.shiftKey) {
        if (ED.selection.has(comp.ref)) ED.selection.delete(comp.ref);
        else ED.selection.add(comp.ref);
      } else if (!ED.selection.has(comp.ref)) {
        ED.selection.clear();
        ED.selection.add(comp.ref);
      }
      ED.selWire = -1;
      ED.drag = {
        mode: 'move', sx: w.x, sy: w.y,
        origin: new Map(Array.from(ED.selection).map((r) => {
          const cc = (ED.doc.components || []).find((x) => x.ref === r);
          return [r, { x: cc.x, y: cc.y }];
        })),
        moved: false,
      };
      // One undo step for the whole drag, not one per mouse move.
      docSet([{ op: 'begin' }]);
      afterChange();
      return;
    }
    const wire = hitWire(w);
    if (wire >= 0) {
      ED.selection.clear();
      ED.selWire = wire;
      afterChange();
      return;
    }
    ED.selection.clear();
    ED.selWire = -1;
    ED.drag = { mode: 'marquee', sx: w.x, sy: w.y, x0: w.x, y0: w.y, x1: w.x, y1: w.y };
    afterChange();
  });

  c.addEventListener('mousemove', (e) => {
    const w = toWorld(e.clientX, e.clientY);
    ED.mouse = { x: snap(w.x), y: snap(w.y) };
    if (!ED.drag) { if (ED.wireFrom) draw(); return; }
    if (ED.drag.mode === 'pan') {
      ED.pan.x = ED.drag.px + (e.clientX - ED.drag.sx);
      ED.pan.y = ED.drag.py + (e.clientY - ED.drag.sy);
      draw();
      return;
    }
    if (ED.drag.mode === 'move') {
      const dx = snap(w.x - ED.drag.sx), dy = snap(w.y - ED.drag.sy);
      if (!dx && !dy) return;
      ED.drag.moved = true;
      const ops = [];
      for (const [ref, o] of ED.drag.origin)
        ops.push({ op: 'move', ref: ref, x: o.x + dx, y: o.y + dy });
      docSet(ops);
      return;
    }
    if (ED.drag.mode === 'marquee') {
      ED.drag.x1 = w.x; ED.drag.y1 = w.y;
      const ctx = canvas().getContext('2d');
      draw();
      ctx.save();
      ctx.translate(ED.pan.x, ED.pan.y);
      ctx.scale(ED.zoom, ED.zoom);
      ctx.strokeStyle = '#2f6fed';
      ctx.fillStyle = 'rgba(47,111,237,0.08)';
      const x = Math.min(ED.drag.x0, ED.drag.x1), y = Math.min(ED.drag.y0, ED.drag.y1);
      const ww = Math.abs(ED.drag.x1 - ED.drag.x0), hh = Math.abs(ED.drag.y1 - ED.drag.y0);
      ctx.fillRect(x, y, ww, hh);
      ctx.strokeRect(x, y, ww, hh);
      ctx.restore();
    }
  });

  window.addEventListener('mouseup', () => {
    if (ED.drag && ED.drag.mode === 'marquee') {
      const x0 = Math.min(ED.drag.x0, ED.drag.x1), x1 = Math.max(ED.drag.x0, ED.drag.x1);
      const y0 = Math.min(ED.drag.y0, ED.drag.y1), y1 = Math.max(ED.drag.y0, ED.drag.y1);
      for (const c of ED.doc.components || []) {
        if (c.bbox[0] >= x0 && c.bbox[2] <= x1 && c.bbox[1] >= y0 && c.bbox[3] <= y1)
          ED.selection.add(c.ref);
      }
    }
    if (ED.drag && ED.drag.mode === 'move') docSet([{ op: 'end' }]);
    if (ED.drag) { ED.drag = null; afterChange(); }
  });

  c.addEventListener('wheel', (e) => {
    e.preventDefault();
    const before = toWorld(e.clientX, e.clientY);
    ED.zoom = Math.max(0.2, Math.min(4, ED.zoom * (e.deltaY < 0 ? 1.12 : 1 / 1.12)));
    const after = toWorld(e.clientX, e.clientY);
    ED.pan.x += (after.x - before.x) * ED.zoom;
    ED.pan.y += (after.y - before.y) * ED.zoom;
    draw();
  }, { passive: false });

  c.addEventListener('contextmenu', (e) => e.preventDefault());
}

/* The letter keys place parts, exactly as in the desktop app -- so the two
 * front-ends feel the same. "g" is ground, Shift+G the supply rail, and
 * "m"/"q" toggle between the two transistor polarities. */
const PLACE_KEYS = {
  r: 'R', c: 'C', l: 'L', v: 'V', b: 'I', d: 'D', y: 'D',
  t: 'T', k: 'K', o: 'OPAMP', g: 'GND',
};

function bindKeys() {
  document.addEventListener('keydown', (e) => {
    // A modal dialog (the help) owns the keyboard while it is open.
    const dlg = document.querySelector('dialog[open]');
    if (dlg) {
      if (e.key === 'Escape') { e.preventDefault(); dlg.close(); }
      return;
    }
    // Text fields keep their own keystrokes.
    const tag = (document.activeElement || {}).tagName;
    if (['INPUT', 'TEXTAREA', 'SELECT'].includes(tag)) return;

    const ctrl = e.ctrlKey || e.metaKey;
    const shift = e.shiftKey;
    const k = e.key;

    if (k === 'Escape') {
      ED.placing = null; ED.wireFrom = null; ED.tool = 'select';
      syncTools(); syncPalette(); draw();
      return;
    }
    if (k === 'Delete' || k === 'Backspace') { e.preventDefault(); deleteSel(); return; }
    if (k === 'F1') { e.preventDefault(); showHelp(); return; }
    if (ctrl && k === 'Enter') { e.preventDefault(); window.runAnalysis(); return; }
    if (ctrl && (k === 'z' || k === 'Z')) { e.preventDefault(); docUndo(); return; }
    if (ctrl && (k === 'y' || k === 'Y' || (shift && (k === 'z' || k === 'Z')))) {
      e.preventDefault(); docRedo(); return;
    }

    // Space rotates, Shift+Space flips horizontally, Ctrl+Space flips
    // vertically -- the desktop's scheme. The armed part is rotated while
    // placing, otherwise the selection is.
    if (k === ' ') {
      e.preventDefault();
      if (ctrl) flipGhost(false);
      else if (shift) flipGhost(true);
      else rotateGhost(90);
      return;
    }
    if (ctrl || e.altKey) return;   // leave the other combos to the browser

    const lower = k.length === 1 ? k.toLowerCase() : '';
    if (!lower) return;

    // "?" opens the help as well.
    if (k === '?') { showHelp(); return; }

    // Undo / redo, the desktop's bare letters.
    if (lower === 'u') { shift ? docRedo() : docUndo(); return; }

    // Wire and net labels.
    if (lower === 'w') {
      ED.tool = 'wire'; ED.placing = null;
      syncTools(); syncPalette(); draw();
      return;
    }
    if (lower === 'n') { labelWire(); return; }

    // Frame every component.
    if (lower === 'f' && !shift) { frameAll(); return; }

    // Parts.
    if (shift) {
      if (lower === 'g') { armPlacement('VDD'); return; }
      return;
    }
    if (lower === 'm') {
      armPlacement(ED.placing === 'NMOS' ? 'PMOS' : 'NMOS'); return;
    }
    if (lower === 'q') {
      armPlacement(ED.placing === 'NPN' ? 'PNP' : 'NPN'); return;
    }
    if (PLACE_KEYS[lower]) { armPlacement(PLACE_KEYS[lower]); return; }
  });
}

// Arm a part for placement (the palette does the same by click).
function armPlacement(token) {
  ED.placing = token;
  ED.placeRot = 0; ED.placeFlipH = false; ED.placeFlipV = false;
  ED.tool = 'select';
  syncTools(); syncPalette(); draw();
}

// Rotate / flip the armed part, or the selection when nothing is armed.
function rotateGhost(delta) {
  if (ED.placing) {
    ED.placeRot = ((ED.placeRot + delta) % 360 + 360) % 360;
    draw();
    return;
  }
  if (ED.selection.size) rotateSel(1);
}
function flipGhost(horizontal) {
  if (ED.placing) {
    if (horizontal) ED.placeFlipH = !ED.placeFlipH;
    else ED.placeFlipV = !ED.placeFlipV;
    draw();
    return;
  }
  if (ED.selection.size) flipSel(horizontal ? 'h' : 'v');
}

function rotateSel(dir) {
  const ops = [];
  ED.selection.forEach((r) => ops.push({ op: 'rotate', ref: r, dir: dir }));
  docSet(ops);
}
function flipSel(axis) {
  const ops = [];
  ED.selection.forEach((r) => ops.push({ op: 'flip', ref: r, axis: axis }));
  docSet(ops);
}
function deleteSel() {
  const ops = [];
  if (ED.selWire >= 0) ops.push({ op: 'delete_wire', i: ED.selWire });
  ED.selection.forEach((r) => ops.push({ op: 'remove', ref: r }));
  ED.selection.clear();
  ED.selWire = -1;
  docSet(ops);
}
function labelWire() {
  const i = ED.selWire >= 0 ? ED.selWire : hitWire(ED.mouse);
  if (i < 0) { setStatus('click a wire first, then press N', true); return; }
  const name = window.prompt('Net name:', '');
  if (name === null) return;
  docSet([{ op: 'label', i: i, name: name, x: ED.mouse.x, y: ED.mouse.y }]);
}

/* ------------------------------------------------------------ palette --- */
function buildPalette() {
  const p = backend('envlp_doc_palette');
  ED.palette = p.kinds;
  const host = document.getElementById('palette-list');
  for (const k of p.kinds) {
    const b = document.createElement('button');
    b.className = 'part';
    b.dataset.token = k.token;
    b.title = k.label + ' (' + k.prefix + ')';
    const cv = document.createElement('canvas');
    cv.width = 74; cv.height = 44;
    const ctx = cv.getContext('2d');
    ctx.scale(window.devicePixelRatio || 1, window.devicePixelRatio || 1);
    const art = ED.art[k.token];
    ctx.save();
    ctx.translate(37, 22);
    const s = 0.42;
    ctx.scale(s, s);
    if (art) {
      ctx.fillStyle = '#10141a';
      for (const f of art.fill) ctx.fill(f);
      ctx.lineCap = 'round';
      for (const st of art.stroke) {
        ctx.strokeStyle = '#10141a';
        ctx.lineWidth = st.w;
        ctx.stroke(st.p);
      }
    }
    ctx.restore();
    b.appendChild(cv);
    const t = document.createElement('span');
    t.textContent = k.prefix;
    b.appendChild(t);
    b.addEventListener('click', () => {
      armPlacement(k.token);
    });
    host.appendChild(b);
  }
}
function syncPalette() {
  document.querySelectorAll('#palette-list .part').forEach((b) => {
    b.classList.toggle('armed', b.dataset.token === ED.placing);
  });
}
function syncTools() {
  document.querySelectorAll('.tool').forEach((b) => {
    b.classList.toggle('armed', b.dataset.tool === ED.tool);
  });
}

/* --------------------------------------------------------- properties --- */
function renderProps() {
  const host = document.getElementById('props');
  host.innerHTML = '';
  if (ED.selection.size === 1) {
    const ref = Array.from(ED.selection)[0];
    const c = (ED.doc.components || []).find((x) => x.ref === ref);
    if (!c) return;
    const row = (label, html) => {
      const d = document.createElement('div');
      d.className = 'prow';
      d.innerHTML = '<span>' + label + '</span>';
      d.appendChild(html);
      host.appendChild(d);
    };
    const val = document.createElement('input');
    val.value = c.value || '';
    val.addEventListener('change', () => docSet([{ op: 'value', ref: ref, text: val.value }]));
    row(c.ref, val);

    // copies: "copy of <unit>", n or n/d
    const units = (ED.doc.components || []).filter((o) => o.ref !== ref && o.kind === c.kind);
    const sel = document.createElement('select');
    sel.innerHTML = '<option value="">(none)</option>' +
      units.map((o) => '<option value="' + o.ref + '">copy of ' + o.ref + '</option>').join('');
    sel.value = c.mirror || '';
    sel.addEventListener('change', () => docSet([{ op: 'mirror', ref: ref, unit: sel.value, mult: c.mult, den: c.den }]));
    row('copies', sel);

    if (c.mirror) {
      const m = document.createElement('input');
      m.value = c.den > 1 ? c.mult + '/' + c.den : String(c.mult);
      m.title = 'n or n/d';
      m.addEventListener('change', () => {
        const m2 = m.value.trim().split('/');
        docSet([{ op: 'mirror', ref: ref, unit: c.mirror,
                  mult: parseInt(m2[0], 10) || 1,
                  den: parseInt(m2[1] || '1', 10) || 1 }]);
      });
      row('×', m);
    }
  } else if (ED.selWire >= 0) {
    const w = (ED.doc.wires || [])[ED.selWire];
    const d = document.createElement('div');
    d.className = 'status';
    d.textContent = 'wire ' + w.i + ' — net "' + (w.net || '(unnamed)') +
                    '" (press N to name it)';
    host.appendChild(d);
  } else {
    const d = document.createElement('div');
    d.className = 'status';
    d.textContent = 'nothing selected';
    host.appendChild(d);
  }
}

/* --------------------------------------------------------------- wiring --- */
function startEditor() {
  buildArt();
  buildPalette();
  syncTools();
  bindCanvas();
  bindKeys();
  resize();
}

window.addEventListener('load', () => {
  // app.js boots the module and calls startEditor() when it is ready.
});
