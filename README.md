# SymCirc

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
- Transistor parasitics (gm, ro, Cgs, Cgd, rpi, Cmu, rb, …) toggled
  **per instance**, each with an editable order-of-magnitude estimate.
- Low-entropy pruning: every term is ranked at frequency `f0` using your
  magnitude estimates (`size_db` gives 10 dB order offsets per component);
  terms more than `threshold_db` below the dominant one are dropped and
  reported with their relative dB.
- Factored forms, pole/zero tables with component labels
  (`τ = R1·C1`, `1/gm·C`, …) and a full human-readable report.
- Bode plot (magnitude/phase) of the exact (unpruned) H.
- Lua console: `mag(f)`, `phase(f)`, `H()`, `roots()`, `dropped()`, … for
  interactive post-processing and custom plots.

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

Configure and build SymCirc:

```bash
cd /d/Projects/Programming/SymCirc
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure   # run the engine tests
./build/bin/SymCirc.exe                      # launch the GUI
```

## Usage

1. Pick a component in the left palette, click the canvas to place it
   (`R` rotates before/during editing, `Del` deletes, `Esc` deselects,
   right-click returns to the Select tool).
2. Use the **Wire** tool: click pin → click pin. Name important nets by
   selecting a wire/label and editing the name on the right (or place a label
   at a pin).
3. Place at least one **Ground** symbol.
4. In the right panel set the input source (`V1`), output (`V(out)` or
   `I(R2)`), the tuning frequency `f0`, and the pruning threshold (dB).
5. Press **F5** (Run → Analyze). Read the report in **Results**, the plot in
   **Bode**, or post-process in **Lua**.

### Component magnitudes

Values accept engineering notation: `10k`, `4.7u`, `100n`, `2.2 pF`, `1M`.
The **size (dB)** field shifts a component's estimate up/down in 10 dB steps
for pruning purposes (e.g. mark a resistor as "really 100× bigger than its
label" without changing the symbol). Device parameters (`gm`, `ro`, `Cgd`,
…) each have an estimate field and a dB offset; parasitic parameters have an
on/off checkbox.

### Lua API

```lua
mag(f)            -- magnitude of H in dB at f Hz
phase(f)          -- phase in degrees
H()               -- factored low-entropy form (string)
Hpoly()           -- expanded pruned polynomial (string)
report()          -- full report (string)
roots("poles")    -- { {w=, tau=, real=, q=, label=, factor=}, ... }
roots("zeros")
estimate("R1")    -- numeric estimate of any symbol
dropped()         -- { {location=, term=, db=}, ... }
```

## File format

`.scx` files are a small line-based text format (`symcirc 1` header:
`comp`, `param`, `wire`, `netlabel`, `req`). Net topology is derived from
wire geometry at analyze time, so files stay geometry-based and diffable.

## Repository layout

- `src/core/` — engine: netlist → MNA → Bareiss determinants → prune → print
  (static lib `symcore`, no GUI deps)
- `src/script/` — Lua VM wrapper (`symscript`)
- `src/app/` — wxWidgets GUI (`SymCirc`)
- `tests/` — engine tests (`test_core`, run by CTest)
- `examples/` — sample circuits
- `docs/design.md` — design notes
