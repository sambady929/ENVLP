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

## Low-entropy engine — implemented decisions

The low-entropy output is the most important part of the project. The engine
(`src/core/LowEntropy.cpp`) currently does:

- **Exact factoring only when exact.** A candidate `(1 + s*tau)` factor is
  accepted only if it actually divides the polynomial
  (`poly_remainder_is_zero`, GiNaC's polynomial remainder after clearing any
  symbolic denominator via `normal()`). The earlier vacuous test
  `(poly - (poly/f)*f).is_zero()` — an algebraic identity that accepted
  *wrong* factors — is gone.
- **Approximate (numeric) factoring when exact fails.** Coefficients are
  estimated numerically, rooted with Durand–Kerner, and each real root becomes
  `1 + s*tau`, matched to a physically meaningful time constant (`R*C`,
  `(R1||R2)*C`, `R*(C1+C2)`, `L/R`, `C/gm`) when one is within ~2 %,
  otherwise carrying the numeric `tau`. Complex pairs become second-order
  factors. The reconstruction is verified before the factors are accepted.
  Toggle: **approx roots** per card and in the properties panel.
- **Parallel form kept symbolic.** `par(a,b)` is a registered GiNaC function
  printed `a||b` (`\parallel` in LaTeX): two parallel resistors are `R1||R2`,
  never `R1*R2/(R1+R2)`.
- **Factored, not multiplied out.** `(1 + s*tau1)(1 + s*tau2)`, with compound
  factors parenthesized so the printed form is unambiguous.
- **Magnitude pruning, always reported.** Every addend is ranked at `f0` from
  the user estimates; anything more than `threshold_db` below the dominant term
  is dropped and listed with its relative dB. A product of two small terms in a
  coefficient (`s*(C1*C2 + C3)`) drops the product, leaving `s*C3`.
- **`gm*ro >> 1` idealization** (default on) removes the `+1` beside a
  dominating `gm*ro` product.

The **prune** checkbox switches the whole reduction on/off; **approx roots** is
independent, so exact-but-unfactored output is also available.

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
| M | NMOS (press M again to toggle PMOS while placing) |
| K | inductor coupling (mutual inductance) |
| G | ground (universal); Shift+G → VDD (universal supply) |
| D | diode |
| T | transformer |
| W | wire tool |
| N | net label(s) — type space-separated names, click each net |
| I | instance menu (all components: R C L V B M K G D T, E VCVS, G VCCS, nullor, op-amp, FD op-amp, amplifier, 1/s block, s block, VDD, …) |
| Space | rotate CCW (while placing); swaps wire route while wiring; rotates a selected component |
| Shift+Space | flip horizontal |
| Ctrl+Space | flip vertical |

Space / rotate / flip all work **while a component is following the cursor**,
not only after placing. The wire tool commits an orthogonal route and Space
swaps horizontal-first ↔ vertical-first. Moving a placed component carries the
wire endpoints attached to its pins.

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
  options edited inside the card, cards run in listed order. Each transfer-like
  card carries the SPICE-style sweep (start, stop, decade/octave/linear, points
  per interval), which drives both the plots and the term ranking.
- Toolbar + **View** menu: **Ignore negligible terms** toggle (replaces the old
  per-document "prune" checkbox); Select / Wire / Net-label / Analyze tools.
  The old "Analysis defaults" block was removed — everything is per card.
- Ground / supply symbols are anonymous: they show no reference and several
  share the `GND` / `VDD` ref.

---

## Phase B — Analyses (analysis cards)

1. **AC small-signal** — MOSFETs/diodes converted to small-signal equivalents
   using only enabled non-idealities; node voltages / branch currents as
   expressions + Bode plots; low-entropy factored form. **DONE**
2. **s-domain transfer function** — mark a source `Vin`/`Iin` and a node or
   branch `out`; produce H(s). **DONE**
3. **DC** — symbolic node voltages / branch currents (s -> 0), with LaTeX.
   **DONE** (symbolic operating-point form; the square-law / gm-Id nonlinear
   bias model remains future work).
4. **PSR / PSRR** — excitation = universal VDD (per-unit); PSR = H(VDD→out);
   PSRR = H(Vin→out) / H(VDD→out). **DONE**
5. **Return ratio (loop gain)** — user selects reference element:
   (a) replace with a real **nullor** (nullator + norator) → ideal H∞,
   (b) return ratio T(s) = H/(H∞ − H); plots: Bode, **Nyquist**, **Nichols**.
   **DONE**
6. **Short-circuit current** — short chosen node to gnd through 0 V source;
   give I through that branch. **DONE**
7. **Input/output impedance** — Zin (input source spec), Zout (output node
   label); symbolic Z expressions, LaTeX. **DONE**
8. **Noise** — input- and output-referred noise densities with a per-source
   contribution breakdown; thermal `4kT/R`, MOSFET `4kT*(2/3)*gm`, BJT and
   diode shot noise. **DONE**

All analyses: right-click any node/branch → expression and/or plot.

## Phase C — Low-entropy engine upgrades. **DONE** (see "Low-entropy engine"
above). Remaining: an explicit EET/Blackman card option is folded into the
numeric-approx factoring; the `1 + gm*ro` idealization is implemented.

## Phase D — Output & export

- **LaTeX**: `to_latex` / `low_entropy_latex` → copy buttons ("Copy LaTeX") in
  the results panel and via `latex()` in Lua. **DONE**
- **Plots**: save as **SVG**, **PNG**, **CSV** (Bode / Nyquist / Nichols).
  **DONE**

## Phase E — Later: switched-capacitor z-domain

Clock phases, switched-component schematic entry, z^-1 blocks. Papers and
schematic-entry conventions TBD with the user.

---

## Open research / risks

- Symbolic nonlinear DC (square-law / gm-Id): approach spike needed before
  promising arbitrary-topology DC.
- Return-ratio element selection: T(s) = H/(H∞ − H) assumes no direct
  feedthrough at the break; a general double-injection form is future work.
- Approximate factoring matches numeric roots to named time constants within
  ~2 %; a genuinely complex-conjugate pole pair is emitted with numeric terms.
