# SymCirc design notes

## Goals

Give a designer the closed-form expression they would derive on paper —
*low entropy*: only the terms that matter at the frequency of interest, in a
factored form with recognizable time constants.

## Pipeline

```
schematic (Document)
  └─ wire geometry ─▶ net resolution (union-find over pins/wires/labels)
       └─ Circuit (pins carry node names)
            └─ AnalysisRequest (input ref, output V(node)|I(ref), f0, threshold)
                 └─ MNA (modified nodal analysis, symbolic, in s)
                      └─ Cramer: H = det(A_num)/det(A_den)
                           (fraction-free Bareiss determinant, custom)
                           └─ Prune at f0  ─▶ normalized/factored low-entropy form
                                └─ report / Bode / Lua
```

### Why custom Bareiss determinants?

GiNaC's `det()` on generic symbolic matrices blows up in expression size for
even modest circuits. The fraction-free Bareiss algorithm keeps intermediate
entries as polynomials with exact division, which is dramatically smaller for
MNA matrices whose entries are sparse monomials (`s·C`, `gm`, `1/R`, …).

### Low-entropy pruning

1. Every addend of num/den is a product of symbols. Using the user's SI
   estimates (`Component::estimate()`, `param_estimate()`, both modulated by
   the 10 dB `size_db` / `param_db` offsets) each addend gets a numeric
   magnitude at `f0` via `|term(j·2πf0)|`.
2. Terms more than `threshold_db` (default 40) below the dominant addend are
   dropped; each drop is recorded with its relative dB for the report.
   Ranking is global over the polynomial (`global_ref = true`) or per
   s-coefficient.
3. The denominator is normalized so its constant term is 1; the remaining
   constant ratio becomes the gain `K`.
4. Factoring: degree-1 factors peel exactly; higher-order denominators are
   factored by generating physically meaningful time-constant candidates
   (`R·C`, `L/R`, `C/gm`, products of these from pole tables) and verifying
   each division symbolically before accepting it.
5. Roots of the surviving polynomial (Durand–Kerner) give numeric pole/zero
   frequencies; recognizable factors get component labels (`R1·C1`).

Everything dropped is shown in the report — pruning is never silent.

### Parasitics

Device parameters live in `param_defs(Kind)`: name, default estimate, unit,
`parasitic` flag (on/off checkbox per instance), `default_on`. Enabling `rb`
on a BJT introduces the hidden internal node `Q1_bi`; symbol names are
`<param>_<ref>` (`gm_M1`, `Cgd_M1`).

### Units & ranking

`UnitClass` per symbol (Ohm/Farad/Henry/Siemens/Volt/Plain) lets the pruner
build dimensionally correct time-constant candidates for factoring and pole
labels.

## GUI architecture

- `Document` — pure data: circuit + placements + wires + labels + request +
  dirty flag; serialization to `.scx`; net resolution.
- `SchematicCanvas` — drawing and interaction (place/wire/select/drag/
  rotate/delete); grid snapped; selection strings (`ref`, `#wireN`,
  `#labelN`) shared with the properties panel.
- `PalettePanel` (tools + components), `PropertiesPanel` (analysis request +
  selection editor), `ResultsPanel` (report), `BodePanel` (mag/phase paint),
  `LuaConsole` (REPL with history), all orchestrated by `MainFrame`.
- UTF-8 everywhere internally; conversion to `wxString` at the UI boundary
  (`wxString::FromUTF8`), ASCII literals in GUI-authored strings.

## Scripting

`LuaVm` (Lua 5.5 from MSYS2) exposes the most recent `AnalysisResult`:
`mag/phase` (numeric, unpruned), `H/Hpoly/report` (text), `roots`, `estimate`,
`dropped`. The console captures `print` output.

## Testing

`tests/test_core.cpp` — assert-based, run by CTest: value parsing, polynomial
roots, resistive divider, RC low-pass (analytic + Bode −3 dB/−45° checks),
common-source with parasitics toggled (analytic identities), pruning
(drops/keeps), two-stage factoring, branch-current outputs, BJT hidden node,
error messages, `size_db` offsets.
