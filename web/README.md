# Running ENVLP in a browser

ENVLP's analysis engine is portable C++ (GiNaC, plus the `.scx` loader, which is
wx-free), so it compiles to WebAssembly and the whole analyser runs **inside the
visitor's browser** — nothing is uploaded, and the site only serves static files.

This directory is the browser front-end; `tools/build_web.sh` produces the
`envlp.js` + `envlp.wasm` pair next to it.

> **Status: the front-end is complete and working; the WebAssembly build of the
> solver is blocked on CLN.** See "Known blocker" below. The schematic editor and
> the analysis UI do not depend on how the solver is compiled — swapping in a
> working engine build is the only step left.

## Build

```bash
# one-time: Emscripten
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest

cd <ENVLP>
bash tools/build_web.sh          # builds GMP + CLN + GiNaC for wasm, then the app
```

The script downloads and builds GMP, CLN and GiNaC for wasm into
`build-web/deps/` (a few minutes the first time), compiles the engine, and
copies the typesetting/plotting bundles and the example circuits into `web/`.
Re-running it only rebuilds what changed.

### Notes for other machines

Building these three autotools libraries for WebAssembly on Windows has three
sharp edges, all handled by the script but worth knowing:

* **`emconfigure` cannot run `configure`.** It spawns the configure script with
  Python's `subprocess`, and Windows cannot execute a `/bin/sh` script (WinError
  193). The script therefore sets `CC=emcc CXX=em++ AR=emar RANLIB=emranlib`
  itself and lets the shell drive `configure`/`make`.
* **CLN needs `-DHZ=100`.** Its timing code uses the POSIX `HZ` macro, which the
  Emscripten libc does not define.
* **GiNaC needs pkg-config to find CLN.** If it is not on `PATH` (in MSYS2 it
  lives in `ucrt64/bin`), the configure fails with "pkg-config script could not
  be found". The script adds the MSYS2 tree to `PATH`, sets `PKG_CONFIG_PATH`,
  and also passes `CLN_CFLAGS`/`CLN_LIBS` directly.

To try it locally, serve the directory over HTTP — `file://` will not work
because the browser fetches the `.wasm`:

```bash
python -m http.server 8000 --directory web
# then open http://localhost:8000/
```

## Put it on WordPress

1. Upload the **contents** of `web/` to `wp-content/uploads/envlp/` (FTP, SFTP,
   or a plugin such as "File Manager"). The directory must contain
   `index.html`, `envlp.js`, `envlp.wasm`, `app.js`, `style.css`, `mathjax/`,
   `plotly/` and `examples/`.
2. Either embed it directly in a page as a Custom HTML block:

   ```html
   <iframe src="/wp-content/uploads/envlp/index.html" loading="lazy"
           style="width:100%;height:820px;border:0;border-radius:8px"></iframe>
   ```

   or install the tiny plugin in `wordpress/envlp.php` (copy it to
   `wp-content/plugins/envlp/envlp.php`, activate it) and just write:

   ```
   [envlp]
   ```

   `[envlp height="900"]` resizes it.
3. Make sure your host serves `.wasm` as `application/wasm`. Most do; if not,
   add to `.htaccess`:

   ```
   AddType application/wasm .wasm
   ```

An iframe is recommended: it isolates the app from the theme's CSS and JS, so
the schematic editor and the plots lay out exactly as intended on any theme.

## What is and isn't in this build

| | |
|---|---|
| ✅ the symbolic engine (DC, AC, transfer function, PSRR, loop gain, Zin/Zout, noise, differential) | `src/core` |
| ✅ the schematic editor's **model**: components, wire bindings, waypoints, the orthogonal router, net naming, labels, copies, undo, `.scx` round-trip | `src/app/Document.cpp`, exposed by `src/web/EditorApi.cpp` |
| ✅ the schematic editor's **canvas**: palette, place, wire, select/marquee, drag, rotate, flip, delete, net labels, pan, zoom | `web/editor.js` |
| ✅ the symbol artwork, shared with the desktop app | `src/app/SymbolDraw.cpp` → `src/web/SymbolArt.cpp` |
| ✅ MathJax typesetting and Plotly plots | `resources/mathjax`, `resources/plotly` |
| ❌ the wxWidgets desktop shell | replaced by the web UI |
| ❌ the Lua console | JavaScript is the scripting layer in the browser |

The editor is deliberately *not* a JavaScript re-implementation of the
electrical rules: the page renders the geometry the C++ model returns and sends
it batched edits, so wiring, net naming and `.scx` output are identical to the
desktop app.

## Known blocker: CLN does not work when compiled for wasm32

GMP, CLN and GiNaC all compile and link for `wasm32-unknown-emscripten`, and the
module loads — but **CLN's integer arithmetic is wrong under wasm**, which makes
GiNaC (and therefore the solver) unusable. It is not ENVLP's code: a two-line
probe that links only CLN reproduces it.

What the probes show (all with CLN built by `build_web.sh`):

| operation | expected | actual |
|---|---|---|
| `1000000007 * 1000000007` vs `cl_I("1000000014000000049")` | equal | **not equal** |
| `integer_length(1000000007)` | 30 | **27** |
| `ash(x, 40)` / `x * x` for 60-bit results | value | **traps: memory access out of bounds** |
| `cl_I(1)/cl_I(3)` squared, `cl_F("1.2020569031595942853997381615")` | correct | correct |

Everything that goes through *parsing* and *small immediates* is right; anything
that needs CLN's multi-digit machinery (shifts, wide products, large literals)
returns wrong values or reads out of bounds. GiNaC's own static initialiser
parses such a literal, so **`envlp.wasm` traps during setup** even before a call.

Ruled out, with evidence:

* **Build configuration** — `cln/intparam.h` reports the correct wasm32 sizes
  (`long` 32, pointers 32, `long long` 64), `gmp.h` reports `GMP_LIMB_BITS 32`,
  and CLN's `config.h` selects `GMP_DEMANDS_UINTD_LONG`, so `uintD` matches
  `mp_limb_t`. The build-tree and installed headers are byte-identical.
* **Stale objects / a bad reconfigure** — a clean `make clean` rebuild is
  identical.
* **The optimiser** — at `-O0` the traps disappear but the *values are still
  wrong*, so it is not (only) miscompilation.
* **The exception ABI** — matching `-fexceptions` across the libraries and the
  app changes nothing.
* **Stack and memory sizing** — `-sSTACK_SIZE=8MB -sINITIAL_MEMORY=64MB` and
  `-sSTACK_OVERFLOW_CHECK=2` change nothing.

This is a known-hard area: there is a fork of CLN, `jan-hudec/cln-em`,
"tweaked to build under emscripten", which exists for this reason.

### What was found, and what remains

**Patch applied (not the cause).** CLN decides whether it has a 64-bit "double
digit" type — needed for digit-sequence multiplication — from a list of CPUs with
fast 64-bit integers, in `include/cln/types.h`:

```c
#if (defined(__alpha__) || ... || defined(__loongarch_lp64))
    #define HAVE_FAST_LONGLONG        // wasm32 is not in this list
#endif
...
#if (intDDsize <= (defined(HAVE_FAST_LONGLONG) ? 64 : 32))
    #define HAVE_DD 1                 // so this is 0, and CLN multiplies digits
                                      // with 32-bit arithmetic
```

wasm has first-class `i64`, so wasm32 belongs in that list. `build_web.sh`
applies that patch — but **measurements are identical before and after it**, so
it is not the cause of the wrong values. It is a consistency fix (without it CLN
takes its 32-bit-emulation paths) and is kept, but the real defect is the bignum
path below.

**Remaining, narrowed to one function:** the bug is at the **immediate/fixnum
boundary**, in the int→`cl_I` construction.

`integer_length(cl_I(1<<k))`, measured from the wasm build:

| k | 0 … 26 | 27 | 28 | 29 | 30 |
|---|---|---|---|---|---|
| bits needed | k+1 | 28 | 29 | 30 | 31 |
| returned | correct | 28 | 29 | **29** | **0** |

Everything up to `1<<28` is right; it breaks exactly when the value needs
`cl_value_len` bits (30 — the immediate payload width, from
`include/cln/object.h`: `cl_value_len = cl_pointer_size - cl_value_shift`). The
engine's own configuration is correct (`cl_pointer_size` 32, `cl_word_alignment`
4, `cl_value_shift` 2, `cl_value_len` 30, `intLsize` 32, `intDsize` 32), and a
small value (`12345` → 14 bits) is exact, as are parsed values and rational
arithmetic. GMP itself is **verified exact** in this build (multiply, wide
shifts, long-decimal parsing), so its `mp_limb_t`/limb configuration is not the
problem.

So the fault is in `src/integer/conv/cl_I_from_L.cc`,
`cl_I_constructor_from_L()`. Checked and *correct*, so not the cause:

* the fit test `wert & minus_bit(cl_value_len-1) == 0` — `minus_bit(n)` really is
  the contiguous high-bit mask (`-1UL << n`), so a 30-bit value is correctly
  routed to the bignum path;
* `mulu32` for wasm is the plain `(uint64)(x)*(uint64)(y)` form;
* GMP: `1000000007²`, wide shifts and a 100-digit decimal all exact, with
  `GMP_LIMB_BITS` 32, no nails, `mp_bits_per_limb` 32.

What is left is the **1-digit bignum construction itself**:

```c
var cl_heap_bignum* ptr = allocate_bignum(1);
arrayLSref(ptr->data,1,0) = wert;
```

`allocate_bignum(1)` allocates `offsetofa(cl_heap_bignum,data)+sizeof(uintD)*1`
and `arrayLSref(array,length,i)` is `lspref(arrayLSDptr(array,length),i)`.
`cl_DS.h` defines `lspref`/`mspref` in **two mirrored conventions** (`lspref(p,i)
= p[i]` versus `p[-i-1]`), selected by a macro, and `arrayLSDptr`/`arrayMSDptr`
compensate for whichever is chosen. That pairing — plus `allocate_bignum`
setting `length` — is the remaining suspect, and the observation that a 31-bit
value yields an *empty* bignum points at the length/pointer, not the digits.

The next step is to instrument `cl_I_constructor_from_L` on wasm: dump the word
produced by `cl_combine` and the `len`/top digit of the allocated bignum for
`1<<28`, `1<<29`, `1<<30`.

Ruled out, with evidence:

* **Build configuration** — `cln/intparam.h` reports the correct wasm32 sizes
  (`long` 32, pointers 32, `long long` 64), `gmp.h` reports `GMP_LIMB_BITS 32`,
  and CLN's `config.h` selects `GMP_DEMANDS_UINTD_LONG`, so `uintD` matches
  `mp_limb_t`. Build-tree and installed headers are byte-identical.
* **Stale objects / a bad reconfigure** — a clean `make clean` rebuild is
  identical.
* **The optimiser** — at `-O0` the traps disappear but the *values are still
  wrong*, so it is not (only) miscompilation.
* **Stack exhaustion** — an 8 MB stack plus `-sSTACK_OVERFLOW_CHECK=2` changes
  nothing, so the bad write is to a heap buffer, not past the stack.
* **The exception ABI** — matching `-fexceptions` across the libraries and the
  app changes nothing.

### Where to pick it up

* The pass/fail test is four lines: link a probe against only `libcln.a` and
  check `cl_I("1000000007") * cl_I("1000000007") == cl_I("1000000014000000049")`,
  `integer_length(cl_I(1000000007)) == 30`, and that `ash` on a 60-bit value does
  not trap. The exact probes used here are described above.
* Then rebuild CLN, GiNaC (their objects embed the headers, so `make clean`
  first) and the app — `tools/build_web.sh` does all three.
* The front-end needs no changes: it talks to the engine through `envlp_run` /
  `envlp_doc_*`. A working `envlp.wasm` drops straight in.

Fallbacks if the port stalls: run the analysis in a small endpoint the page
POSTs the `.scx` to, or keep the web page as the editor/viewer and analyse in the
desktop app.

## Licensing

GiNaC is GPL-2.0-or-later (CLN GPL-2.0-or-later, GMP LGPL-3.0-or-later). A site
shipping this WebAssembly build is distributing a GPL-covered work, so the
complete corresponding source must be available to visitors — which it is, as
long as this repository stays public. MathJax and Plotly are Apache-2.0 and
MIT respectively; their notices are in `resources/*/README.txt`.
