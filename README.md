# ENVLP

Symbolic circuit analysis for schematics, with **low-entropy** transfer
functions: terms that are orders of magnitude below the dominant behaviour at
your operating frequency are pruned away, so you get the compact expression a
designer would actually write by hand — not a page of negligible products.

```
H(s) = K / ((1 + s·R1·C1)·(1 + s·R2·C2))
```

Built with C++17, [GiNaC](https://www.ginac.de/) for symbolic math, and
wxWidgets for the GUI.

The analysis engine also builds to **WebAssembly**, so it can run entirely in a
browser (no server, nothing uploaded) — see [`web/README.md`](web/README.md).
The plots and the typeset math are Plotly and MathJax in either case.

## Features

- **Schematic capture**: place resistors, capacitors, inductors, V/I sources,
  VCVS/VCCS, NMOS/PMOS, NPN/PNP and ground; wire pins; label nets. Several
  schematics can be open at once, each in its own tab with its own analyses.
- **Symbolic analysis**: transfer function H = num/den in `s`, any node voltage
  `V(node)` or branch current `I(ref)`, or a port such as `V(a)-V(b)`.
- **Analyses**: transfer function, AC, DC, PSR/PSRR, loop gain (return ratio
  `T = H/(H∞−H)`), short-circuit current, input and output impedance, noise
  (input- and output-referred, with a per-source breakdown), and
  differential / common-mode gain (Adm, Acm, CMRR).
- **Input impedance** is the input source's own response (the EET view): a
  current source gives the voltage across it, a voltage source the current
  through it, so only a source — not an output — is needed.
- **Device models** toggled and valued **per instance**: MOSFET `gm`, `ro`,
  `W/L`, `Cgs/Cgd/Cds`; bipolar transistors in the **hybrid-pi** form
  (`gm`, `rpi`, `ro`, `Cpi`, `Cmu`); diodes as `rd` + `Cd`.
- **Low-entropy pruning**: every term is ranked by its worst-case magnitude
  across the sweep, and negligible terms are dropped and reported with their
  relative dB. The **Negligible terms…** dialog sets the thresholds as ratios:
  a *component* threshold (10× collapses a 10k in parallel with a 1k), a
  *pole/zero* threshold, and whether poles/zeros are judged against the
  dominant pole/zero or against the **unity-gain bandwidth**.
- **Factored forms** with symbolic **parallel** terms (`R1||R2`, never
  `R1*R2/(R1+R2)`), pole/zero tables with component labels
  (`τ = R1·C1`, `(Rd||ro)·(Cgd+CL)`, `1/gm·C`, …) and a full report.
  A factor is accepted only when it really divides the polynomial; when the
  denominator does not factor symbolically, numeric roots are matched to
  physical time constants ("approx roots").
- **Plots**: Bode (magnitude/phase), Nyquist and Nichols, exported as
  SVG / PNG / CSV.
- **LaTeX** for every expression, copy-to-clipboard.

## Building (Windows, MSYS2 UCRT64)

Prerequisites: [MSYS2](https://www.msys2.org/) installed at `C:\msys64`.

```bash
# in an MSYS2 UCRT64 shell (C:\msys64\ucrt64.exe, or bash with MSYSTEM=UCRT64)
pacman -S --needed base-devel git mingw-w64-ucrt-x86_64-toolchain \
    mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
    mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-lua \
    mingw-w64-ucrt-x86_64-wxwidgets3.3-common \
    mingw-w64-ucrt-x86_64-wxwidgets3.3-common-libs \
    mingw-w64-ucrt-x86_64-wxwidgets3.3-msw \
    mingw-w64-ucrt-x86_64-wxwidgets3.3-msw-libs \
    mingw-w64-ucrt-x86_64-gmp mingw-w64-ucrt-x86_64-readline
```

GiNaC and CLN are not packaged by MSYS2 — build them from source once:

```bash
cd /tmp && mkdir deps && cd deps
curl -LO https://www.ginac.de/CLN/cln-1.3.7.tar.bz2
curl -LO https://www.ginac.de/ginac-1.8.10.tar.bz2
tar xf cln-1.3.7.tar.bz2 && cd cln-1.3.7
./configure --prefix=/ucrt64 && make -j$(nproc) && make install
cd .. && tar xf ginac-1.8.10.tar.bz2 && cd ginac-1.8.10
./configure --prefix=/ucrt64 && make -j$(nproc) && make install
```

Configure and build ENVLP:

```bash
cd /d/Projects/Programming/ENVLP
cmake -B build -G Ninja
cmake --build build
./build/bin/ENVLP.exe                      # launch the GUI
```

### Portable release (no install)

```powershell
powershell -ExecutionPolicy Bypass -File tools/make_portable.ps1
```

This builds and assembles `dist/ENVLP-<version>-win64/` — `ENVLP.exe`, every
runtime DLL it needs (found transitively with `objdump`, then stripped), and
the example circuits. Copy that folder anywhere and run `ENVLP.exe`; nothing
needs to be installed.

### Code signing (why Windows warns, and how to stop it)

An unsigned executable downloaded from the internet makes Windows SmartScreen
show *"Windows protected your PC"* with a **Run anyway** link. That warning is
driven by **Authenticode**:

- **The real fix** is to sign `ENVLP.exe` with a code-signing certificate that
  chains to a publicly trusted CA:
  - an **EV** certificate is trusted immediately;
  - an **OV** certificate earns SmartScreen reputation as downloads grow.
- A **self-signed** certificate does **not** help other people — they would
  have to install it into their Trusted Root store first. It is only useful for
  your own machines.

`tools/sign.ps1` signs the built exe and every packaged copy with
`osslsigncode` (install once: `pacman -S mingw-w64-ucrt-x86_64-osslsigncode`):

```powershell
# PFX (certificate + private key)
powershell -ExecutionPolicy Bypass -File tools/sign.ps1 `
    -Pfx mycert.pfx -Password 'secret'

# or PEM certificate + key
powershell -ExecutionPolicy Bypass -File tools/sign.ps1 -Cert mycert.pem -Key mykey.pem
```

The executable also carries **VERSIONINFO** metadata (publisher, product,
version), generated from the `project()` version in `CMakeLists.txt`, so the
Properties dialog shows who produced the binary and which release it is.

Tagging a commit `v*` runs the packaging in CI and attaches `ENVLP-win64.zip`
to the GitHub Release. To sign in CI, add repository secrets
`SIGNING_PFX_BASE64` (the base64 of your .pfx) and `SIGNING_PFX_PASSWORD`; the
workflow decodes them and runs `tools/sign.ps1` before uploading.

## Usage

1. Pick a component in the left palette, or press its key, then click the
   canvas to place it. While the ghost follows the cursor, **Space** rotates,
   **Shift+Space / Ctrl+Space** flip, **Esc** cancels.
2. **Wire (W)**: click to start (a red dot marks the snap point), then click
   the destination — a pin or any grid point. **Space** while wiring swaps the
   route between horizontal-first and vertical-first. Moving a component drags
   the wires attached to its pins.
3. **Net labels**: select a wire and type its **Net name** in the properties
   panel, or press **N** to place several labels at once — type space-separated
   names (`out in`) and click each net.
4. Place at least one **Ground** symbol.
5. Add **analysis cards** with the *+ Add* picker on the right. Each card has
   its own **Run** button. Fill in the input source (`in`) and the output
   (`out`), e.g. `V(out)`.
6. **Clicking a component replaces the analysis column with that component's
   editor** — value, model parameters, "copy of". Click empty canvas to bring
   the analysis cards back.
7. Pan with a right-drag, zoom with the wheel; **F** frames all components.
   Read the report in **Results**, the plot in **Plot**.

To select several components, drag a box on empty canvas with the Select tool
(or Shift-click to add/remove); the group moves and rotates together.

### Negligible terms

**Negligible terms…** (toolbar, or the **View** menu) sets the pruning
thresholds, as ratios:

- **Component value threshold** — a series/parallel pair collapses when one
  element is at least this many times the other. `10` means 10× and above
  (20 dB): a 10k in parallel with a 1k collapses to the 1k.
- **Pole / zero threshold** — a pole or zero is dropped when it is more than
  this many times the reference frequency.
- **Reference** — judge poles/zeros against the **dominant pole/zero**, or
  against the **unity-gain bandwidth** (drops everything beyond a multiple of
  the crossover frequency).

### Component values

Values use an SI-suffix dropdown (k, M, G, m, µ, n, p, f) so the prefix is
picked from a list rather than typed. The **size (dB)** field shifts a
component's estimate up/down in 10 dB steps for pruning purposes (mark a
resistor as "really 100× bigger than its label" without changing the symbol).

### Parasitics and the supply rail

Passives carry optional parasitics, **off by default**: a resistor's parallel
`Cpar` and series `Lser`; a capacitor's `Rser` / `Lser` (ESR / ESL) and leakage
`Rpar`; an inductor's winding `Rser`, self-`Cpar` and core-loss `Rpar`. A
transformer (`T`) has the standard `Lp`, `Ls` (turns ratio `n = sqrt(Ls/Lp)`)
and coupling `k`, plus winding `Rp` / `Rs` and inter-winding `Cps`.

A **VDD** symbol is a bare supply rail with no value of its own: its voltage is
set per analysis in the **DC operating point** card's *DC analysis options*
dialog, so any number of VDD symbols can be placed and they are all one rail.

### Frequency sweep and term ranking

Every transfer-like card uses the standard SPICE AC-sweep controls: **start** /
**stop** frequency, interval type (**decade**, **octave** or **linear**) and
**points per interval**. The sweep drives the plots *and* the low-entropy
ranking: a term is judged by its worst-case magnitude anywhere in the band, so
a term negligible at low frequency but dominant at high frequency is kept.

## File format

`.scx` files are a small line-based text format (`envlp 1` header: `comp`,
`param`, `wire`, `netlabel`, `req`, `card`). Net topology is derived from wire
geometry at analyze time, so files stay geometry-based and diffable. The `req`
line carries the engine switches and the sweep; `card` lines configure the
analysis stack and round-trip verbatim.

## Repository layout

- `src/core/` — engine: netlist → MNA → Bareiss determinants → prune → print
  (static lib `envlpcore`, no GUI deps)
- `src/app/` — wxWidgets GUI (`ENVLP`)
- `examples/` — sample circuits
- `tools/` — icon + portable-packaging scripts
