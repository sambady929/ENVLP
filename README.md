# ENVLP

Symbolic circuit analysis for schematics, with **low-entropy** transfer
functions: terms that are orders of magnitude below the dominant behaviour at
your operating frequency are pruned away, so you get the compact expression a
designer would actually write by hand — not a page of negligible products.

```
H(s) = K / ((1 + s·R1·C1)·(1 + s·R2·C2))
```

Built with C++17, [GiNaC](https://www.ginac.de/) for symbolic math, wxWidgets
for the GUI, and Lua for interactive scripting/plotting.

## Features

- Schematic capture: place resistors, capacitors, inductors, V/I sources,
  VCVS/VCCS, NMOS/PMOS, NPN/PNP, ground; wire pins; label nets.
- Symbolic analysis: transfer function H = num/den in `s`, any node voltage
  `V(node)` or branch current `I(ref)`, driven by one ideal source.
- Analyses: **transfer function**, **AC**, **DC**, **PSR/PSRR**, **loop gain**
  (nullor substitution → return ratio `T = H/(H∞−H)`), **short-circuit
  current**, **input/output impedance**, and **noise** (input- and
  output-referred, with a per-source breakdown).
- Transistor parasitics (gm, ro, Cgs, Cgd, rpi, Cmu, rb, …) toggled
  **per instance**, each with an editable order-of-magnitude estimate.
- Low-entropy pruning: every term is ranked at frequency `f0` using your
  magnitude estimates (`size_db` gives 10 dB order offsets per component);
  terms more than `threshold_db` below the dominant one are dropped and
  reported with their relative dB.
- **Factored forms** with symbolic **parallel** terms (`R1||R2`, never
  `R1*R2/(R1+R2)`), pole/zero tables with component labels
  (`τ = R1·C1`, `(Rd||ro)·(Cgd+CL)`, `1/gm·C`, …) and a full report.
- **Exact factoring only when exact**: a factor is accepted only if it really
  divides the polynomial. When the denominator does not factor symbolically,
  numeric roots are matched to physical time constants ("approx roots").
- Bode plot (magnitude/phase), **Nyquist** and **Nichols** plots, exported as
  **SVG / PNG / CSV**.
- LaTeX for every expression, copy-to-clipboard.
- Lua console: `mag(f)`, `phase(f)`, `H()`, `latex()`, `roots()`, `dropped()`,
  … for interactive post-processing and custom plots.

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
cd /c/Users/$USER/AppData/Local/Temp/opencode/deps   # or wherever you unpacked
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
ctest --test-dir build --output-on-failure   # run the engine tests
./build/bin/ENVLP.exe                      # launch the GUI
```

## Usage

1. Pick a component in the left palette, or press its key, then click the
   canvas to place it. While the ghost is following the cursor, **Space**
   rotates, **Shift+Space / Ctrl+Space** flip, **Esc** cancels. A click places
   one component and returns to Select, so keys and Space work at any time.
2. **Wire (W)**: click to start (a red dot marks the snap point), then click
   the destination — a pin or any grid point — to finish in one click. **Space**
   while wiring swaps the route between horizontal-first and vertical-first;
   **Esc** cancels. Moving a component drags the wires attached to its pins.
3. **Net labels**: select a wire and type its **Net name** in the properties
   panel (a label is created above the wire), or press **N** to place several
   labels at once — type space-separated names (`out in`) and click each net.
   Click a label to edit its text and font size.
4. Place at least one **Ground** symbol.
5. Configure analyses with the cards on the right: pick the input source,
   output (`V(out)` or `I(R2)`), the **sweep** (start / stop / decade·octave·
   linear / points per interval), and the `ignore negligible` / `gm·ro≫1` /
   `approx roots` switches. Run one card or **Run all**, or press **F5**.
6. Pan with a right-drag (works even past the window edges) and zoom with the
   wheel; **F** frames all components. Read the report in **Results**, the plot
   in **Plot** (switch to Nyquist / Nichols).

To select several components, drag a box on empty canvas with the Select tool
(or Shift-click to add/remove); the group moves and rotates together.

`examples/cs_test.scx` is a common-source amplifier (NMOS with a drain load
and a source-degeneration / input network), the main test fixture, pre-loaded
with TF and Zout cards.

### Ignore negligible terms

The **Ignore negligible terms** check item lives on the toolbar and in the
**View** menu. It turns low-entropy pruning on/off globally (the exact,
unpruned expression is shown when it is off). Each analysis card can override
it.

### Component magnitudes

Values accept engineering notation: `10k`, `4.7u`, `100n`, `2.2 pF`, `1M`.
The **size (dB)** field shifts a component's estimate up/down in 10 dB steps
for pruning purposes (e.g. mark a resistor as "really 100× bigger than its
label" without changing the symbol). Device parameters (`gm`, `ro`, `Cgd`,
…) each have an estimate field and a dB offset; parasitic parameters have an
on/off checkbox.

### Frequency sweep and term ranking

Every transfer-like card uses the standard SPICE AC-sweep controls:

- **start** / **stop** frequency,
- interval type: **decade**, **octave** or **linear**,
- **points per interval**.

The sweep drives the Bode / Nyquist / Nichols plots *and* the low-entropy
ranking: a term is judged by its worst-case magnitude anywhere in the band, so
a term that is negligible at low frequency but dominant at high frequency is
kept. DC and noise cards do not use the sweep.

### Lua API

```lua
mag(f)            -- magnitude of H in dB at f Hz (unpruned, exact)
phase(f)          -- phase in degrees
H()               -- factored low-entropy form (string)
Hpoly()           -- expanded pruned polynomial (string)
latex()           -- LaTeX of the factored form (string)
report()          -- full report (string)
roots("poles")    -- { {w=, tau=, real=, q=, label=, factor=}, ... }
roots("zeros")
estimate("R1")    -- numeric estimate of any symbol
dropped()         -- { {location=, term=, db=}, ... }
```

## File format

`.scx` files are a small line-based text format (`envlp 1` header:
`comp`, `param`, `wire`, `netlabel`, `req`, `card`). Net topology is derived
from wire geometry at analyze time, so files stay geometry-based and diffable.
The `req` line ends with the engine switches and the sweep
(`fstart fstop type points prune gm*ro approx`); `card` lines configure the
analysis stack and round-trip verbatim.

## Repository layout

- `src/core/` — engine: netlist → MNA → Bareiss determinants → prune → print
  (static lib `envlpcore`, no GUI deps)
- `src/script/` — Lua VM wrapper (`envlpscript`)
- `src/app/` — wxWidgets GUI (`ENVLP`)
- `tests/` — engine tests (`test_core`, run by CTest)
- `examples/` — sample circuits
- `docs/design.md` — design notes
