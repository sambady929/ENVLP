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
  wireSel: new Set(),   // selected wire indices (a marquee takes them all)
  selWire: -1,          // the wire under the cursor / last clicked
  selSeg: -1,           // the segment of that wire (a wire drag moves it)
  selLabel: -1,         // selected net label
  drag: null,           // {mode:'move'|'marquee'|'pan'|'wire', ...}
  wireFrom: null,       // {x,y,pin:{ref,index}|null} -- the point a wire starts from
  wireIndex: -1,        // the wire being extended (the desktop extends one wire)
  wirePts: [],          // its points so far
  wireHFirst: true,     // Space swaps the route: horizontal-first or vertical-first
  labelQueue: [],       // net names waiting to be placed (the N key)
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

// Drop an in-progress wire without trying to join it. Used when the document
// itself is replaced or rewound, where the old wire index no longer exists.
function resetWiringState() {
  ED.wireFrom = null;
  ED.wireIndex = -1;
  ED.wirePts = [];
  ED.wireDrag = null;
}

function docOpen(scx) {
  resetWiringState();
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
  resetWiringState();
  Module.ccall('envlp_doc_undo', null, [], []);
  ED.doc = docGeometry();
  afterChange();
}
function docRedo() {
  resetWiringState();
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
  if (ED.placing) drawGhost(ctx);
  if (ED.labelQueue.length) drawPendingLabel(ctx);

  ctx.restore();
  drawHud(ctx, W, H);
}

// The net name that is waiting to be placed, hovering over the cursor with
// an anchor dot -- so you can see where it will land.
function drawPendingLabel(ctx) {
  const name = ED.labelQueue[0];
  if (!name) return;
  ctx.save();
  ctx.font = "bold 12px 'Segoe UI', sans-serif";
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  const w = ctx.measureText(name).width + 10;
  // the anchor dot
  ctx.fillStyle = '#2f6fed';
  ctx.beginPath();
  ctx.arc(ED.mouse.x, ED.mouse.y, 5, 0, 2 * Math.PI);
  ctx.strokeStyle = '#2f6fed';
  ctx.lineWidth = 1.5;
  ctx.stroke();
  // the name, just above the cursor
  ctx.fillStyle = '#e8f0fe';
  ctx.strokeStyle = '#2f6fed';
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.rect(ED.mouse.x - w / 2, ED.mouse.y - 22, w, 18);
  ctx.fill();
  ctx.stroke();
  ctx.fillStyle = '#1f2430';
  ctx.fillText(name, ED.mouse.x, ED.mouse.y - 13);
  ctx.restore();
}

// The part that is about to be placed, drawn under the cursor -- so the user
// can see what they are placing and how it is oriented.
function drawGhost(ctx) {
  const art = ED.art && ED.art[ED.placing];
  if (!art) return;
  ctx.save();
  ctx.translate(ED.mouse.x, ED.mouse.y);
  ctx.rotate((ED.placeRot || 0) * Math.PI / 180);
  ctx.scale(ED.placeFlipH ? -1 : 1, ED.placeFlipV ? -1 : 1);
  ctx.globalAlpha = 0.55;
  ctx.fillStyle = '#2f6fed';
  for (const f of art.fill) ctx.fill(f);
  ctx.lineCap = 'round';
  ctx.lineJoin = 'round';
  ctx.strokeStyle = '#2f6fed';
  for (const s of art.stroke) {
    ctx.lineWidth = s.w;
    ctx.stroke(s.p);
  }
  ctx.globalAlpha = 1;
  for (const t of art.text) {
    ctx.save();
    ctx.fillStyle = '#2f6fed';
    ctx.font = (t.b ? 'bold 15px ' : '12px ') + "'Segoe UI', sans-serif";
    ctx.textAlign = t.c ? 'center' : 'left';
    ctx.textBaseline = t.c ? 'middle' : 'alphabetic';
    ctx.fillText(t.s, t.x, t.y);
    ctx.restore();
  }
  ctx.restore();
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

function samePoint(a, b) {
  return Math.abs(a[0] - b[0]) < 1 && Math.abs(a[1] - b[1]) < 1;
}

// Does p lie strictly inside the segment a-b?
function onSpan(p, a, b) {
  if (samePoint(p, a) || samePoint(p, b)) return false;
  const vx = b[0] - a[0], vy = b[1] - a[1];
  const wx = p[0] - a[0], wy = p[1] - a[1];
  const L2 = vx * vx + vy * vy;
  if (L2 < 1e-9) return false;
  const t = (wx * vx + wy * vy) / L2;
  if (t <= 0 || t >= 1) return false;
  return Math.hypot(p[0] - (a[0] + t * vx), p[1] - (a[1] + t * vy)) <= 1;
}

// Does segment a-b intersect (or lie inside) the axis-aligned rectangle?
// A marquee must take a long wire that crosses the box even when neither end
// is inside it; sampling a few points could miss a thin box.
function segRectHit(a, b, x0, y0, x1, y1) {
  const inside = (p) =>
    p[0] >= x0 - 1e-9 && p[0] <= x1 + 1e-9 &&
    p[1] >= y0 - 1e-9 && p[1] <= y1 + 1e-9;
  if (inside(a) || inside(b)) return true;
  const cross = (p, q, r) =>
    (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0]);
  const on = (p, q, r) =>
    Math.min(p[0], q[0]) - 1e-9 <= r[0] &&
    r[0] <= Math.max(p[0], q[0]) + 1e-9 &&
    Math.min(p[1], q[1]) - 1e-9 <= r[1] &&
    r[1] <= Math.max(p[1], q[1]) + 1e-9;
  const segSeg = (p1, p2, q1, q2) => {
    const o1 = cross(p1, p2, q1), o2 = cross(p1, p2, q2);
    const o3 = cross(q1, q2, p1), o4 = cross(q1, q2, p2);
    if (((o1 > 0 && o2 < 0) || (o1 < 0 && o2 > 0)) &&
        ((o3 > 0 && o4 < 0) || (o3 < 0 && o4 > 0)))
      return true;
    if (Math.abs(o1) < 1e-9 && on(p1, p2, q1)) return true;
    if (Math.abs(o2) < 1e-9 && on(p1, p2, q2)) return true;
    if (Math.abs(o3) < 1e-9 && on(q1, q2, p1)) return true;
    if (Math.abs(o4) < 1e-9 && on(q1, q2, p2)) return true;
    return false;
  };
  return segSeg(a, b, [x0, y0], [x1, y0]) ||
         segSeg(a, b, [x1, y0], [x1, y1]) ||
         segSeg(a, b, [x1, y1], [x0, y1]) ||
         segSeg(a, b, [x0, y1], [x0, y0]);
}

// A dot marks a real connection: three or more distinct branch directions at
// a point. A straight end-to-end join and a corner have two, so they get no
// dot -- exactly like Cadence Virtuoso and the desktop app. A T-junction and
// a shared three-way vertex get one.
function drawJunctions(ctx, doc) {
  const segs = [], verts = [];
  for (const w of doc.wires || []) {
    for (let i = 1; i < w.pts.length; ++i) segs.push([w.pts[i - 1], w.pts[i]]);
    for (const p of w.pts) verts.push(p);
  }
  const dirKey = (dx, dy) =>
    ((dx > 1e-6) - (dx < -1e-6)) + ',' + ((dy > 1e-6) - (dy < -1e-6));
  const seen = [];
  for (const v of verts) {
    const dirs = new Set();
    for (const [a, b] of segs) {
      if (samePoint(v, a)) {
        dirs.add(dirKey(b[0] - a[0], b[1] - a[1]));
      } else if (samePoint(v, b)) {
        dirs.add(dirKey(a[0] - b[0], a[1] - b[1]));
      } else if (onSpan(v, a, b)) {
        dirs.add(dirKey(a[0] - b[0], a[1] - b[1]));
        dirs.add(dirKey(b[0] - a[0], b[1] - a[1]));
      }
    }
    if (dirs.size >= 3 && !seen.some((q) => samePoint(v, q))) {
      seen.push(v);
      ctx.fillStyle = '#10141a';
      ctx.beginPath();
      ctx.arc(v[0], v[1], 3.2, 0, 2 * Math.PI);
      ctx.fill();
    }
  }
}

function drawWires(ctx, doc) {
  ctx.lineCap = 'round';
  for (const w of doc.wires || []) {
    const sel = w.i === ED.selWire || ED.wireSel.has(w.i);
    ctx.strokeStyle = sel ? '#2f6fed' : '#10141a';
    ctx.lineWidth = sel ? 2.6 : 1.6;
    ctx.beginPath();
    w.pts.forEach((p, i) => (i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1])));
    ctx.stroke();
  }
  drawJunctions(ctx, doc);
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
      if (c.has_value && c.value) {
        ctx.fillStyle = '#6b7284';
        ctx.font = "12px 'Segoe UI', sans-serif";
        ctx.textBaseline = 'top';
        ctx.fillText(c.value + (c.mult !== c.den ? ' ×' + c.mult + '/' + c.den : (c.mult > 1 ? ' ×' + c.mult : '')), (c.bbox[0] + c.bbox[2]) / 2, c.bbox[3] + 2);
      }
    } else {
      ctx.textAlign = 'left';
      ctx.textBaseline = 'bottom';
      ctx.fillText(c.ref, c.bbox[2] + 4, (c.bbox[1] + c.bbox[3]) / 2 - 1);
      if (c.has_value && c.value) {
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
    const sel = l.i === ED.selLabel;
    // Plain text, like the desktop: a net name is not a highlight.
    ctx.fillStyle = sel ? '#2f6fed' : '#1f2430';
    ctx.fillText(text, l.x, l.y);
    if (sel) {
      const w = ctx.measureText(text).width;
      ctx.strokeStyle = '#2f6fed';
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.rect(l.x - w / 2 - 3, l.y - 9, w + 6, 18);
      ctx.stroke();
    }
  }
}

function drawWirePreview(ctx) {
  const a = ED.wireFrom, b = ED.mouse;
  ctx.strokeStyle = '#2f6fed';
  ctx.lineWidth = 1.6;
  ctx.setLineDash([5, 4]);
  ctx.beginPath();
  ctx.moveTo(a.x, a.y);
  // The same orthogonal route the router will produce, so the preview is
  // what you get. Space swaps which leg comes first.
  if (Math.abs(b.x - a.x) > 1e-9 && Math.abs(b.y - a.y) > 1e-9) {
    if (ED.wireHFirst) ctx.lineTo(b.x, a.y);
    else ctx.lineTo(a.x, b.y);
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
    hint = ED.wireFrom
      ? 'click each corner; click a pin to end (Enter/Esc)'
      : 'click a pin or a point to start a wire';
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
  const h = hitWireSeg(w);
  return h ? h.wire : -1;
}
// The wire *and* the segment under the cursor: dragging a wire moves that
// segment, perpendicular to itself, the way the desktop app does.
function hitWireSeg(w) {
  let best = null, bd = 8 / ED.zoom;
  for (const wire of ED.doc.wires || []) {
    for (let i = 1; i < wire.pts.length; ++i) {
      const [x0, y0] = wire.pts[i - 1], [x1, y1] = wire.pts[i];
      const vx = x1 - x0, vy = y1 - y0, L2 = vx * vx + vy * vy;
      let t = L2 ? ((w.x - x0) * vx + (w.y - y0) * vy) / L2 : 0;
      t = Math.max(0, Math.min(1, t));
      const d = Math.hypot(w.x - (x0 + t * vx), w.y - (y0 + t * vy));
      if (d < bd) { bd = d; best = { wire: wire.i, seg: i - 1 }; }
    }
  }
  return best;
}
// A net label under the cursor (so it can be selected and deleted).
function hitLabel(w) {
  const ctx = canvas().getContext('2d');
  ctx.font = "bold 12px 'Segoe UI', sans-serif";
  for (let i = (ED.doc.labels || []).length - 1; i >= 0; --i) {
    const l = ED.doc.labels[i];
    const tw = ctx.measureText(l.name || '').width + 8;
    if (Math.abs(w.x - l.x) <= tw / 2 && Math.abs(w.y - l.y) <= 9) return i;
  }
  return -1;
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
      // In the wire tool a click always wires: it starts a wire, or commits
      // the next segment. Moving a wire is the select tool's job.
      const started = !ED.wireFrom;
      wireClick(w);
      if (started) {
        ED.wireDrag = { sx: ED.wireFrom.x, sy: ED.wireFrom.y, moved: false };
      }
      return;
    }

    // Placing net names: every click on a net takes the next name.
    if (ED.labelQueue.length) {
      placeLabelAt(w);
      return;
    }

    // A net label: click it to select it (Del removes it).
    const li = hitLabel(w);
    if (li >= 0) {
      ED.selection.clear();
      ED.selWire = -1; ED.selSeg = -1;
      ED.selLabel = li;
      afterChange();
      return;
    }
    ED.selLabel = -1;

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
      ED.selWire = -1; ED.selSeg = -1;
      ED.wireSel.clear();
      ED.drag = {
        mode: 'move', sx: w.x, sy: w.y,        origin: new Map(Array.from(ED.selection).map((r) => {
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
    const wh = hitWireSeg(w);
    if (wh) {
      ED.selection.clear();
      ED.wireSel.clear();
      ED.selWire = wh.wire;
      ED.selSeg = wh.seg;
      // Grabbing a wire drags the *segment* under the cursor: it moves
      // perpendicular to itself, so its neighbours stretch instead of the
      // whole route going diagonal.
      ED.drag = { mode: 'wire', wire: wh.wire, seg: wh.seg, sx: w.x, sy: w.y };
      docSet([{ op: 'begin' }]);
      afterChange();
      return;
    }
    ED.selection.clear();
    ED.wireSel.clear();
    ED.selWire = -1;
    ED.drag = { mode: 'marquee', sx: w.x, sy: w.y, x0: w.x, y0: w.y, x1: w.x, y1: w.y };
    afterChange();
  });

  c.addEventListener('mousemove', (e) => {
    const w = toWorld(e.clientX, e.clientY);
    ED.mouse = { x: snap(w.x), y: snap(w.y) };
    if (ED.wireDrag) {
      // A drag in the wire tool: the wire follows the cursor, and releasing
      // commits it.
      if (Math.hypot(ED.mouse.x - ED.wireDrag.sx, ED.mouse.y - ED.wireDrag.sy) > 4)
        ED.wireDrag.moved = true;
      draw();
      return;
    }
    if (!ED.drag) {
      // The ghost follows the cursor, and the wire preview follows it too.
      if (ED.wireFrom || ED.placing || ED.labelQueue.length) draw();
      return;
    }
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
      // The selected wires come along with the components.
      const ddx = dx - (ED.drag.lastDx || 0), ddy = dy - (ED.drag.lastDy || 0);
      if (ddx || ddy) {
        ED.drag.lastDx = dx;
        ED.drag.lastDy = dy;
        for (const wi of ED.wireSel)
          ops.push({ op: 'move_wire', wire: wi, seg: -1, dx: ddx, dy: ddy });
      }
      docSet(ops);
      return;
    }
    if (ED.drag.mode === 'wire') {
      const dx = snap(w.x - ED.drag.sx), dy = snap(w.y - ED.drag.sy);
      if (!dx && !dy) return;
      // The move is relative, so send only what has changed since the last
      // step -- otherwise the drag compounds and the wire flies away.
      const ddx = dx - (ED.drag.lastDx || 0);
      const ddy = dy - (ED.drag.lastDy || 0);
      if (!ddx && !ddy) return;
      ED.drag.lastDx = dx;
      ED.drag.lastDy = dy;
      docSet([{ op: 'move_wire', wire: ED.drag.wire, seg: ED.drag.seg, dx: ddx, dy: ddy }]);
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
    if (ED.wireDrag) {
      // Releasing the mouse after a drag in the wire tool commits the wire.
      const drag = ED.wireDrag;
      ED.wireDrag = null;
      if (drag.moved) {
        const pin = hitPin({ x: ED.mouse.x, y: ED.mouse.y });
        wireCommit(pin ? { x: pin.x, y: pin.y, pin }
                       : { x: ED.mouse.x, y: ED.mouse.y, pin: null });
        endWiring();
      }
      return;
    }
    if (ED.drag && ED.drag.mode === 'marquee') {
      // A marquee takes the components *and* the wires it sweeps over.
      const x0 = Math.min(ED.drag.x0, ED.drag.x1), x1 = Math.max(ED.drag.x0, ED.drag.x1);
      const y0 = Math.min(ED.drag.y0, ED.drag.y1), y1 = Math.max(ED.drag.y0, ED.drag.y1);
      const inBox = (x, y) => x >= x0 && x <= x1 && y >= y0 && y <= y1;
      for (const c of ED.doc.components || [])
        if (inBox(c.bbox[0], c.bbox[1]) || inBox(c.bbox[2], c.bbox[3]) ||
            inBox((c.bbox[0] + c.bbox[2]) / 2, (c.bbox[1] + c.bbox[3]) / 2))
          ED.selection.add(c.ref);
      for (const w of ED.doc.wires || []) {
        let hit = false;
        for (let i = 1; i < w.pts.length && !hit; ++i)
          hit = segRectHit(w.pts[i - 1], w.pts[i], x0, y0, x1, y1);
        if (hit) ED.wireSel.add(w.i);
      }
      afterChange();
    }
    if (ED.drag && ED.drag.mode === 'move') docSet([{ op: 'end' }]);
    if (ED.drag && ED.drag.mode === 'wire') docSet([{ op: 'end' }]);
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
  r: 'R', c: 'C', l: 'L', v: 'V', b: 'I', d: 'D',
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
      // Cancel whatever is in progress: the armed part, the wire being
      // drawn, the queued net names and the selection -- the desktop's Esc.
      ED.placing = null;
      endWiring();
      ED.labelQueue = [];
      ED.tool = 'select';
      ED.selection.clear();
      ED.selWire = -1; ED.selSeg = -1; ED.selLabel = -1;
      setStatus('');
      syncTools(); syncPalette(); afterChange();
      return;
    }
    if (k === 'Delete' || k === 'Backspace') {
      e.preventDefault();
      if (ED.selLabel >= 0) {
        docSet([{ op: 'delete_label', i: ED.selLabel }]);
        ED.selLabel = -1;
        return;
      }
      deleteSel();
      return;
    }
    if (k === 'Enter' && ED.tool === 'wire' && ED.wireFrom) {
      e.preventDefault();
      // Enter ends the wire. The last click already committed the segment; the
      // rubber band is only a preview. Committing it here used to add a
      // segment to wherever the cursor had drifted, pulling the endpoint away
      // from the wire it had just reached.
      endWiring();
      return;
    }
    if (k === 'F1') { e.preventDefault(); showHelp(); return; }
    if (ctrl && k === 'Enter') { e.preventDefault(); window.runAnalysis(); return; }
    if (ctrl && (k === 'z' || k === 'Z')) { e.preventDefault(); docUndo(); return; }
    if (ctrl && (k === 'y' || k === 'Y' || (shift && (k === 'z' || k === 'Z')))) {
      e.preventDefault(); docRedo(); return;
    }
    if (ctrl && (k === 'c' || k === 'C')) {
      e.preventDefault();
      copySelection();
      return;
    }
    if (ctrl && (k === 'v' || k === 'V')) {
      e.preventDefault();
      pasteClipboard();
      return;
    }

    // Space rotates, Shift+Space flips horizontally, Ctrl+Space flips
    // vertically -- the desktop's scheme. The armed part is rotated while
    // placing, otherwise the selection is. While wiring, Space swaps the
    // route (horizontal-first or vertical-first).
    if (k === ' ') {
      e.preventDefault();
      if (ED.tool === 'wire' && ED.wireFrom) {
        ED.wireHFirst = !ED.wireHFirst;
        setStatus('Wire route: press Space again to swap.');
        draw();
        return;
      }
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
    if (lower === 'n') {
      // The dialog focuses its text field synchronously; without this the
      // browser's default action would insert the triggering "n" into it.
      e.preventDefault();
      labelWire();
      return;
    }
    // The component menu: every part, as a list.
    if (lower === 'i') { openComponentList(); return; }

    // Frame every component.
    if (lower === 'f' && !shift) { frameAll(); return; }

    // Parts.
    if (shift) {
      if (lower === 'g') { armPlacement('VDD'); return; }
      // Shift+M / Shift+Q pick the P-type straight away; pressing M or Q
      // again while placing switches to the P-type.
      if (lower === 'm') { armPlacement('PMOS'); return; }
      if (lower === 'q') { armPlacement('PNP'); return; }
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
  // Leaving the wire tool finishes the wire (and joins it to a neighbour it
  // meets end to end), exactly like the desktop's tool switch.
  if (ED.tool === 'wire') endWiring();
  ED.labelQueue = [];
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
  // Deleting a wire shifts the indices above it, so go from the highest
  // index down. Ascending order deleted only the first wire of a marquee.
  const wires = Array.from(new Set([
    ...Array.from(ED.wireSel),
    ...(ED.selWire >= 0 ? [ED.selWire] : []),
  ])).sort((a, b) => b - a);
  for (const i of wires) ops.push({ op: 'delete_wire', i: i });
  ED.selection.forEach((r) => ops.push({ op: 'remove', ref: r }));
  ED.selection.clear();
  ED.wireSel.clear();
  ED.selWire = -1;
  docSet(ops);
}

// Copy the selection (and the wires between it) into the engine's clipboard.
function copySelection() {
  if (!ED.selection.size) { setStatus('nothing selected to copy', true); return; }
  docSet([{ op: 'copy', refs: Array.from(ED.selection).join(',') }]);
  setStatus('Copied ' + ED.selection.size + ' component(s)');
}

// Paste the clipboard at the cursor, the way the desktop does.
function pasteClipboard() {
  const x = snap(ED.mouse.x), y = snap(ED.mouse.y);
  docSet([{ op: 'paste', x: x, y: y }]);
  // The engine selects what it pasted.
  const sel = (ED.doc && ED.doc.selected) || [];
  ED.selection.clear();
  sel.forEach((r) => ED.selection.add(r));
  afterChange();
  if (sel.length) setStatus('Pasted ' + sel.length + ' component(s)');
}
function labelWire() {
  // The desktop's flow, in an in-app dialog: type the names (space
  // separated), then click each net to place them one by one.
  if (ED.tool === 'wire') endWiring();
  const dlg = document.getElementById('netlabel');
  if (!dlg) return;
  const input = $('netlabel-input');
  input.value = '';
  dlg.showModal();
  input.focus();
}

// Place the next queued net name on the net under the cursor.
function placeLabelAt(w) {
  const wi = hitWire(w);
  if (wi < 0) {
    setStatus('click a wire to place the net name', true);
    return;
  }
  const name = ED.labelQueue.shift();
  docSet([{ op: 'label', i: wi, name: name, x: ED.mouse.x, y: ED.mouse.y }]);
  if (!ED.labelQueue.length) {
    setStatus('');
    draw();
  } else {
    setStatus('Net label: click a net to place "' + ED.labelQueue[0] + '".');
  }
}

// Leave wiring mode. Whatever is already committed stays on the canvas; only
// the segment in progress is dropped. The finished wire is then joined to any
// other wire it meets end to end, so the two become one shape (Virtuoso).
function endWiring() {
  const wi = ED.wireIndex;
  ED.wireFrom = null;
  ED.wireIndex = -1;
  ED.wirePts = [];
  // A join can remove a wire and shift the indices, so any wire selection is
  // no longer meaningful.
  ED.wireSel.clear();
  ED.selWire = -1;
  ED.selSeg = -1;
  // Always run the join normaliser on finish. It is global: it merges every
  // collinear overlap, including L+L and overlaps that are not at an endpoint.
  if (wi >= 0) docSet([{ op: 'join', wire: wi }]);
  else draw();
}

// A click in the wire tool: the first one starts the wire, every later one
// commits a segment and continues from there (the desktop's behaviour).
function wireClick(w) {
  const pin = hitPin(w);
  const target = pin
    ? { x: pin.x, y: pin.y, pin }
    : { x: snap(w.x), y: snap(w.y), pin: null };
  if (!ED.wireFrom) {
    ED.wireFrom = target;
    ED.wirePts = [{ x: target.x, y: target.y }];
    ED.wireIndex = -1;
    draw();
    return;
  }
  wireCommit(target);
}

// Commit the segment from the last point to the target.
function wireCommit(target) {
  const last = ED.wirePts[ED.wirePts.length - 1];
  if (target.x === last.x && target.y === last.y) return;

  const endPin = {
    end_pin: target.pin ? 'true' : 'false',
    end_ref: target.pin ? target.pin.ref : '',
    end_index: target.pin ? String(target.pin.index) : '0',
  };
  let op;
  if (ED.wireIndex < 0) {
    op = Object.assign({
      op: 'wire',
      x0: ED.wireFrom.x, y0: ED.wireFrom.y,
      x1: target.x, y1: target.y,
      h_first: ED.wireHFirst ? 1 : 0,
      start_pin: ED.wireFrom.pin ? 'true' : 'false',
      start_ref: ED.wireFrom.pin ? ED.wireFrom.pin.ref : '',
      start_index: ED.wireFrom.pin ? String(ED.wireFrom.pin.index) : '0',
    }, endPin);
  } else {
    op = Object.assign({
      op: 'wire_extend',
      wire: ED.wireIndex,
      x: target.x, y: target.y,
      h_first: ED.wireHFirst ? 1 : 0,
    }, endPin);
  }
  docSet([op]);
  if (ED.wireIndex < 0) ED.wireIndex = (ED.doc.wires || []).length - 1;
  // The engine reports the wire it kept. End-to-end joins happen in
  // endWiring(), so while a route is still being drawn this is just the wire
  // being extended.
  if (typeof ED.doc.last_wire === 'number' && ED.doc.last_wire >= 0)
    ED.wireIndex = ED.doc.last_wire;
  const wire = ED.doc.wires && ED.doc.wires[ED.wireIndex];
  ED.wirePts = wire
    ? wire.pts.map((p) => ({ x: p[0], y: p[1] }))
    : [{ x: target.x, y: target.y }];
  ED.wireFrom = { x: target.x, y: target.y, pin: target.pin };

  // Only a component terminal finishes the wire. Landing on another wire's
  // vertex must NOT finish it: an L-shaped wire often turns exactly on the
  // endpoint of a straight wire, and the next click must be able to add the
  // perpendicular leg. The overlap/join is resolved on Enter/Esc/double-click.
  if (target.pin) {
    endWiring();
  } else {
    draw();
  }
}

/* ------------------------------------------------------------ palette --- */

// The parts that really have a shortcut key (the desktop's set). Only these
// are shown with one -- the op-amp is "O", not "U" (U is undo).
const PALETTE_SHORTCUTS = {
  R: 'R', C: 'C', L: 'L', V: 'V', I: 'B', D: 'D',
  NMOS: 'M', PMOS: 'Shift+M', NPN: 'Q', PNP: 'Shift+Q',
  OPAMP: 'O', GND: 'G', VDD: 'Shift+G', T: 'T', K: 'K',
};

function buildPalette() {
  const p = backend('envlp_doc_palette');
  ED.palette = p.kinds;
  const host = document.getElementById('palette-list');
  for (const k of p.kinds) {
    const b = document.createElement('button');
    b.className = 'part';
    b.dataset.token = k.token;
    b.title = k.label + ' — reference prefix ' + k.prefix;
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
    // The name of the part, and its shortcut when it has one: "Resistor (R)".
    // A bare "U" under the op-amp read like a shortcut, and U is undo.
    const sc = PALETTE_SHORTCUTS[k.token];
    t.textContent = sc ? k.label + ' (' + sc + ')' : k.label;
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

// The component menu (the I key): every part in one list, the way the
// desktop's "instance" menu works.
function openComponentList() {
  const dlg = document.getElementById('components');
  if (!dlg) return;
  const host = dlg.querySelector('.clist');
  host.innerHTML = '';
  for (const k of ED.palette) {
    const b = document.createElement('button');
    b.className = 'citem';
    b.textContent = k.label;
    b.title = 'reference prefix ' + k.prefix;
    b.addEventListener('click', () => {
      armPlacement(k.token);
      dlg.close();
    });
    host.appendChild(b);
  }
  if (!dlg.open) dlg.showModal();
}

/* --------------------------------------------------------- properties --- */

// The SI suffixes, the same list (and the same labels) as the desktop's
// value editor.
const SI_SUFFIXES = [
  { s: 'T', label: 'T  tera  (e+12)' },
  { s: 'G', label: 'G  giga  (e+9)' },
  { s: 'M', label: 'M  mega  (e+6)' },
  { s: 'k', label: 'k  kilo  (e+3)' },
  { s: '', label: '(none)  (e0)' },
  { s: 'm', label: 'm  milli (e-3)' },
  { s: 'µ', label: 'µ  micro (e-6)' },
  { s: 'n', label: 'n  nano  (e-9)' },
  { s: 'p', label: 'p  pico  (e-12)' },
  { s: 'f', label: 'f  femto (e-15)' },
];

// Split a value into its number and its SI suffix ("10k" -> ["10", "k"]).
function splitSi(text) {
  const m = /^([-+]?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?)\s*(.*)$/.exec(text || '');
  if (!m) return [text || '', ''];
  const num = m[1];
  let suf = (m[2] || '').trim();
  const alias = { Meg: 'M', meg: 'M', MEG: 'M', u: 'µ', K: 'k', T: 'T' };
  if (alias[suf]) suf = alias[suf];
  const known = SI_SUFFIXES.some((x) => x.s === suf);
  return known ? [num, suf] : [num, suf];
}

const isSource = (kind) => kind === 'V' || kind === 'I';

function renderProps() {
  const host = document.getElementById('props');
  host.innerHTML = '';
  if (ED.selection.size === 1) {
    const ref = Array.from(ED.selection)[0];
    const c = (ED.doc.components || []).find((x) => x.ref === ref);
    if (!c) return;
    // A copy inherits its unit's value and model parameters, so the editor
    // hides those rows exactly as the desktop app does.
    const isCopy = !!c.mirror;
    const row = (label, html) => {
      const d = document.createElement('div');
      d.className = 'prow';
      d.innerHTML = '<span>' + label + '</span>';
      d.appendChild(html);
      host.appendChild(d);
    };
    // The value: the gain of an amplifier, the DC level of a source, the
    // resistance of a resistor -- the desktop names it the same way, and
    // edits it as a number plus an SI suffix.
    const siField = (text, commit, unit) => {
      const wrap = document.createElement('span');
      wrap.className = 'si';
      const [num, suf] = splitSi(text || '');
      const val = document.createElement('input');
      val.className = 'mantissa';
      val.value = num;
      const sel = document.createElement('select');
      sel.className = 'suffix';
      for (const s of SI_SUFFIXES) {
        const o = document.createElement('option');
        o.value = s.s;
        o.textContent = s.label;
        sel.appendChild(o);
      }
      sel.value = suf;
      const fire = () => commit(val.value.trim() + sel.value);
      val.addEventListener('change', fire);
      sel.addEventListener('change', fire);
      wrap.appendChild(val);
      wrap.appendChild(sel);
      // The physical unit, as on the desktop's value editor.
      if (unit) {
        const u = document.createElement('span');
        u.className = 'unit';
        u.textContent = unit;
        wrap.appendChild(u);
      }
      return wrap;
    };
    if (c.kind === 'K') {
      // A coupling marker names the two inductors it ties together; it has
      // no value of its own.
      const inductors = (ED.doc.components || []).filter((o) => o.kind === 'L');
      const links = c.links || [];
      const picker = (which) => {
        const s = document.createElement('select');
        s.innerHTML = '<option value="">(none)</option>' +
          inductors.map((o) => '<option value="' + o.ref + '">' + o.ref + '</option>').join('');
        s.value = links[which] || '';
        s.addEventListener('change', () => {
          const a = which === 0 ? s.value : (links[0] || '');
          const b = which === 1 ? s.value : (links[1] || '');
          docSet([{ op: 'links', ref: ref, a: a, b: b }]);
        });
        return s;
      };
      row('Couples L:', picker(0));
      row('with L:', picker(1));
    } else if (c.kind === 'VDD') {
      // The supply rail has no per-symbol value; its level is a DC analysis
      // setting. Show the same note as the desktop, even for a copy.
      const note = document.createElement('div');
      note.className = 'status';
      note.textContent = 'Supply rail. Its voltage is set in the DC ' +
        'operating-point card.';
      host.appendChild(note);
    } else if (isCopy) {
      // No value or parameter rows: they belong to the unit being copied.
    } else if (isSource(c.kind)) {
      const unit = c.kind === 'V' ? 'V' : 'A';
      row('DC', siField(c.dc || '', (t) =>
        docSet([{ op: 'value', ref: ref, field: 'dc', text: t }]), unit));
      row('AC', siField(c.ac || '', (t) =>
        docSet([{ op: 'value', ref: ref, field: 'ac', text: t }]), unit));
    } else if (c.has_value) {
      // Only the kinds whose single value is meaningful get this row: a
      // nullor, transistor, diode, transformer or coupling marker does not.
      row(c.value_label || 'Value', siField(c.value || '', (t) =>
        docSet([{ op: 'value', ref: ref, field: 'value', text: t }]),
        c.value_unit));
    }

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

    // The model parameters: gm, ro, Cgs, and the passives' parasitics. They
    // are off by default (a component is ideal unless you ask otherwise), so
    // each one has its own switch, exactly as in the desktop app -- and the
    // value is a number plus an SI suffix, as it is there.
    const params = c.params || [];
    if (params.length && !isCopy) {
      const h = document.createElement('div');
      h.className = 'ttl';
      h.textContent = 'model';
      host.appendChild(h);
      for (const p of params) {
        const d = document.createElement('div');
        d.className = 'prow param';
        const name = document.createElement('span');
        name.textContent = p.name;
        name.title = p.desc || p.name;
        d.appendChild(name);
        if (p.parasitic) {
          const sw = document.createElement('input');
          sw.type = 'checkbox';
          sw.checked = !!p.on;
          sw.title = 'switch ' + p.name + ' on or off';
          sw.addEventListener('change', () =>
            docSet([{ op: 'param_on', ref: ref, name: p.name, on: sw.checked ? 'true' : 'false' }]));
          d.appendChild(sw);
        }
        const [num, suf] = splitSi(p.text || '');
        // The coupling coefficient is a plain number between -1 and +1: no
        // unit, no suffix, and a value outside the range snaps to the nearer
        // end.
        const coupling = (c.kind === 'K' && p.name === 'K');
        const val = document.createElement('input');
        val.className = coupling ? 'coupling' : 'mantissa';
        if (coupling) {
          val.type = 'number';
          val.min = '-1';
          val.max = '1';
          val.step = '0.01';
          val.title = 'The coupling coefficient, between -1 and +1';
        }
        val.value = coupling ? (num || '0.9') : num;
        val.disabled = p.parasitic && !p.on;
        d.appendChild(val);
        if (!coupling) {
          const sel = document.createElement('select');
          sel.className = 'suffix';
          for (const s of SI_SUFFIXES) {
            const o = document.createElement('option');
            o.value = s.s;
            o.textContent = s.label;
            sel.appendChild(o);
          }
          sel.value = suf;
          sel.disabled = p.parasitic && !p.on;
          d.appendChild(sel);
          if (p.unit) {
            const u = document.createElement('span');
            u.className = 'unit';
            u.textContent = p.unit;
            d.appendChild(u);
          }
          const commit = () => docSet([{ op: 'param', ref: ref, name: p.name,
                                         text: val.value.trim() + sel.value }]);
          val.addEventListener('change', commit);
          sel.addEventListener('change', commit);
        } else {
          val.addEventListener('change', () => {
            let v = parseFloat(val.value);
            if (!isFinite(v)) v = 0;
            if (v > 1) v = 1;
            if (v < -1) v = -1;
            val.value = String(v);
            docSet([{ op: 'param', ref: ref, name: p.name, text: String(v) }]);
          });
        }
        host.appendChild(d);
      }
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
