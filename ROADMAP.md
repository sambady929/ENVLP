# SymCirc Roadmap

Symbolic circuit analysis with schematic capture and "low-entropy" (factored,
design-useful) expression output. C++ / GiNaC / wxWidgets. Toolchain: MSYS2
UCRT64 (`C:\msys64`), project root `D:\Projects\Programming\SymCirc`.

Architecture (current):
- `src/core` — `symcore` static lib: Netlist/ParamTable, MNA + Bareiss solve,
  low-entropy pruning (`Prune.cpp`), pretty/LaTeX printing, report formatting.
- `src/symscript` — Lua extension layer (post-processing, plots).
- `src/app` — wxWidgets GUI (schematic canvas, docked panels, Bode panel).
- `tests/test_core.cpp` — CTest suite (13 tests).

Status: core builds and runs; test fixes in progress, then Phase A below.

---

## Confirmed decisions (user)

- **I** opens the *instance menu* (Cadence-Virtuoso style popup listing every
  component). **B** places the current source.
- Amplifier family = three devices:
  1. **op-amp** — differential input, single-ended output.
  2. **fully-differential op-amp** — differential input, differential output.
  3. **amplifier** — generic gain block, single input / single output: a VCVS
     whose inverting input terminal and inverting output terminal are both
     implicitly tied to ground (pins: `inp`, `out`, gain `A`).
- Build priority: **Schematic UX first**, then analyses, exports woven in.
- Engine defaults: ranking = per-coefficient (global-at-f0 optional),
  threshold 40 dB, f0 = 1 kHz, `size_db` per component, `gm*ro >> 1` on.
- Switched-capacitor z-domain: deferred ("figure it out later").

## Style references (symbols)

- Razavi textbook schematics; Linear Technology / LTspice datasheet style
  (https://ltspiceguru.com/LTspiceStyler/LTspiceStyler.html).
- Cleanliness references: https://circuitpaint.org/ ,
  https://analog-canvas.tokenzhang.com/g/asytdhx3k9 — **standard components
  only**, no logic gates.
- Conventions: IEEE/US shapes — zigzag resistor, parallel-plate capacitor,
  arced inductor, circular independent sources, triangle amplifiers; thin crisp
  strokes (~1.5–2 px at 100 % zoom), filled arrows at device terminals, minimal
  fills, anti-aliased. Diode = filled triangle + cathode bar. Ground =
  universal 3-bar; VDD = mirrored universal bar (up).

---

## Phase A — Schematic UX (first)

### Placement keyboard map
| Key | Action |
|-----|--------|
| R | resistor |
| C | capacitor |
| L | inductor |
| V | voltage source |
| B | current source |
| M | NMOS; press M again while placing → PMOS |
| K | inductor coupling (mutual inductance) |
| G | ground (universal); Shift+G → VDD (universal supply) |
| D | diode |
| T | transformer |
| W | wire tool |
| I | instance menu (all components: R C L V B M K G D T, E VCVS, G VCCS, nullor, op-amp, FD op-amp, amplifier, 1/s block, s block, VDD, …) |
| Space | rotate CCW (while ghosting; also rotates a selected component) |
| Shift+Space | flip horizontal |
| Ctrl+Space | flip vertical |

### New components (netlist `Kind` + symbols + MNA stamps)
- diode D (small-signal: gm diode + optional non-idealities),
- transformer T (2 coupled windings),
- inductor coupling K (M between two L's: `s·M` branch stamps),
- **VDD** universal supply node (name-normalized like `GND`→`0`; excitation
  for PSR/PSRR),
- nullor (nullator + norator; used for ideal H-infinity),
- op-amp (diff in / single out), fully-differential op-amp,
- "amplifier" (VCVS gain block, inv-in & inv-out implicitly grounded),
- ideal **1/s** block and ideal **s** block (s-domain analyses).

### Right-side panels (all interaction through them)
- Contextual properties panel: click a MOSFET → non-ideality checkboxes; each
  checked parameter gets two dropdowns — mantissa {1, 3.3, 10, 33, 100, 330…}
  (10 dB steps) and exponent in steps of 3 (engineering notation, editable for
  odd values like −13) → value = mantissa × 10^exp (e.g. Cgs = 100e-13,
  ro = 10e3). Same mechanism feeds `size_db`/estimates into pruning.
- Node/branch right-click → context menu: show expression (and plot for AC).
- Analysis panel: **cards** — add-analysis dropdown, one card per analysis,
  options edited inside the card, cards run in listed order.

---

## Phase B — Analyses (analysis cards)

1. **AC small-signal** — MOSFETs/diodes converted to small-signal equivalents
   using only enabled non-idealities; node voltages / branch currents as
   expressions + Bode plots; low-entropy factored form.
2. **s-domain transfer function** — mark a source `Vin`/`Iin` and a node or
   branch `out`; produce H(s).
3. **DC** — symbolic node voltages / branch currents; MOSFET model option:
   square-law (Vgs) vs gm/Id (Vgs = Vth + vdsat); right-click → expression.
   *Research risk:* nonlinear symbolic DC — start with square-law closed forms
   for tractable chains, then generalize.
4. **PSR / PSRR** — excitation = universal VDD (per-unit); PSR = H(VDD→out);
   PSRR = H(Vin→out) / H(VDD→out).
5. **Return ratio (loop gain)** — user selects reference element:
   (a) replace with nullor → ideal H∞, (b) compute return ratio T(s);
   plots: Bode, **Nyquist**, **Nichols chart**. (Refs: Rosenstark, Middlebrook,
   Tahan short tutorial.)
6. **Short-circuit current** — short chosen node to gnd through 0 V source;
   give I through that branch.
7. **Input/output impedance** — Zin (needs input source spec), Zout (needs
   output node label); symbolic Z expressions.

All analyses: right-click any node/branch → expression and/or plot.

## Phase C — Low-entropy engine upgrades

- Method selector per card: **time/transfer constants** (current peel +
  candidate-τ, extended) and **EET/Blackman** (extra-element theorem) for
  pulling poles/zeros into factored form.
- **gm·ro ≫ 1 assumption (default ON):** idealization rewrite pass that
  eliminates `1 + gm·ro`-type combinations (estimates decide dominance);
  toggle on the card.
- Keep: Bareiss solve, per-coefficient/global ranking, f0/threshold, size_db,
  degree drop, pole/zero tables, hidden internal nodes (rb etc.).
- Ideal 1/s and s blocks participate in AC/TF/return-ratio analyses.

## Phase D — Output & export

- **LaTeX**: GiNaC `print_latex` → copy-to-clipboard buttons on every
  expression (results panel and right-click popups).
- **Plots**: save as SVG, PNG, CSV (Bode / Nyquist / Nichols panels).

## Phase E — Later: switched-capacitor z-domain

Clock phases, switched-component schematic entry, z^-1 blocks. Papers and
schematic-entry conventions TBD with the user.

---

## Open research / risks

- Symbolic nonlinear DC (square-law / gm-Id): approach spike needed before
  promising arbitrary-topology DC.
- Return-ratio element selection + nullor substitution conventions.
- EET factoring path for MNA-derived polynomials (interaction with pruning).
