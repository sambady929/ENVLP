# Running ENVLP in a browser

ENVLP's analysis engine is portable C++ (GiNaC, plus the `.scx` loader, which is
wx-free), so it compiles to WebAssembly and the whole analyser runs **inside the
visitor's browser** — nothing is uploaded, and the site only serves static files.

This directory is the browser front-end; `tools/build_web.sh` produces the
`envlp.js` + `envlp.wasm` pair next to it.

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

It runs on Linux and macOS as well as Windows: the Emscripten SDK is found
through `$EMSDK`, on `PATH`, or in the usual install locations, and the
dependency build itself is the portable `tools/build_deps.sh` (which also builds
the same three libraries *natively* — that is how you get GiNaC on macOS, where
Homebrew has neither CLN nor GiNaC).

To build the dependencies for the host instead, with no Emscripten involved:

```bash
bash tools/build_deps.sh --prefix ~/local/deps   # native GMP + CLN + GiNaC
```

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

## Put it on GitHub Pages

The browser build is a **static site** — `index.html`, `app.js`, `editor.js`,
`style.css`, `envlp.js` + `envlp.wasm`, the MathJax and Plotly bundles and the
example circuits. Nothing on it needs a server, so GitHub Pages serves it
as-is, and the WordPress embed can then point at that URL: **nothing has to be
uploaded to the WordPress host at all** (no FTP, no plugin upload).

`.github/workflows/pages.yml` does the whole job on every push to `main`:

1. Emscripten is installed (pinned to the version the local build was verified
   with) and `tools/build_web.sh` builds GMP/CLN/GiNaC for wasm32 and the
   engine, and lays out `web/`.
2. `web/` is uploaded as the Pages artifact and published.

**One-time setup:** *Settings → Pages → Build and deployment → Source →
**GitHub Actions***. After that, a push to `main` (or the *pages* workflow run
by hand) publishes the site at

```
https://sambady929.github.io/ENVLP/
```

The workflow caches the dependency cross-build, so a re-run takes a couple of
minutes instead of building GMP, CLN and GiNaC from scratch.

Nothing in the page is an absolute path, so the app also works unchanged under
any other sub-path — a custom domain, a user site, a project site with a
different name — without a rebuild.

The `.wasm` MIME type does not matter here: `envlp.js` compiles it with
`WebAssembly.instantiateStreaming` but falls back to plain `ArrayBuffer`
instantiation, so the app still runs on a host that serves `.wasm` as
`application/octet-stream`.

### Updating a live deployment

Push to `main`; the workflow rebuilds and redeploys. `index.html` loads the
scripts as `app.js?v=2` etc. — bump that number whenever you replace the
JavaScript, otherwise visitors keep the copy their browser cached. The `.wasm`
is fetched by `envlp.js` and can be replaced freely.

## Put it on WordPress

With the app on GitHub Pages the WordPress side needs no uploads at all.

### 1. An iframe (nothing to install)

Paste this into a **Custom HTML** block (or a *Code* widget in the sidebar):

```html
<iframe src="https://sambady929.github.io/ENVLP/" loading="lazy"
        style="width:100%;height:820px;border:0;border-radius:8px"
        allow="fullscreen"></iframe>
```

### 2. Or the shortcode plugin

`wordpress/envlp.php` is a tiny plugin that turns `[envlp]` into that iframe.
It accepts the source and the height:

```
[envlp height="900"]
[envlp src="https://sambady929.github.io/ENVLP/"]
```

Installing it does not need FTP either: WordPress' own **Plugins → Editor →
Add New** accepts a pasted plugin file, and the built-in **Plugin Editor** can
save it straight into `wp-content/plugins/`. (Or upload the file if your host
allows it.)

An iframe is recommended either way: it isolates the app from the theme's CSS
and JS, so the schematic editor and the plots lay out exactly as intended on
any theme.

### 3. Or host the files on the WordPress server instead

Uploading the **contents** of `web/` to `wp-content/uploads/envlp/` still
works, if you have file access. The directory must contain `index.html`,
`envlp.js`, `envlp.wasm`, `app.js`, `editor.js`, `style.css`, `mathjax/`,
`plotly/` and `examples/`. Then use the iframe with
`src="/wp-content/uploads/envlp/index.html"`.

Make sure the host serves `.wasm` as `application/wasm`; most do, and if not,
add to `.htaccess`:

```
AddType application/wasm .wasm
```

The app itself still works with the wrong type — it falls back to
`ArrayBuffer` instantiation — but the console shows a warning.

## What is and isn't in this build

| | |
|---|---|
| ✅ the symbolic engine (DC, AC, transfer function, PSRR, loop gain, Zin/Zout, noise, differential) | `src/core` |
| ✅ the schematic editor's **model**: components, wire bindings, waypoints, the orthogonal router, net naming, labels, copies, undo, `.scx` round-trip | `src/app/Document.cpp`, exposed by `src/web/EditorApi.cpp` |
| ✅ the schematic editor's **canvas**: palette, place (with a ghost), wire (orthogonal, Space swaps the route), select/marquee, drag, rotate, flip, delete, net labels, pan, zoom | `web/editor.js` |
| ✅ the **model parameters** — gm, ro, W/L, Cgs/Cgd/Cds, the passives' parasitics (Cpar, Lser, Rser, …) — with their switches | `src/core/Netlist.cpp` → `web/editor.js` |
| ✅ the **low-entropy thresholds**: the component ratio, the pole/zero ratio and the reference (dominant pole/zero or the unity-gain bandwidth) | `web/index.html` → `src/web/WebApi.cpp` |
| ✅ copy / paste of components and of the wires between them | `src/web/EditorApi.cpp` |
| ✅ the symbol artwork, shared with the desktop app | `src/app/SymbolDraw.cpp` → `src/web/SymbolArt.cpp` |
| ✅ MathJax typesetting and Plotly plots | `resources/mathjax`, `resources/plotly` |
| ❌ the wxWidgets desktop shell | replaced by the web UI |
| ❌ several open schematics (the desktop's tabs) | the browser build edits one schematic at a time |
| ❌ the Lua console | JavaScript is the scripting layer in the browser |

The editor is deliberately *not* a JavaScript re-implementation of the
electrical rules: the page renders the geometry the C++ model returns and sends
it batched edits, so wiring, net naming and `.scx` output are identical to the
desktop app.

## CLN on wasm32 (fixed)

CLN needs a small patch to be correct on a 32-bit target. Without it the engine
returns wrong values and GiNaC's own static initialiser corrupts memory, so
`envlp.wasm` traps before any call. `tools/build_web.sh` applies the patch, and
`tools/wasm-probes/run.sh` is the standalone check (exit 0 means CLN is usable).

**Symptom.** `integer_length(cl_I(1<<k))`:

| k | 0 ... 28 | 29 | 30 |
|---|---|---|---|
| bits needed | k+1 | 30 | 31 |
| before the patch | correct | **29** | **0** |
| after | correct | 30 | 31 |

`1000000007 * 1000000007` also came out inexact, and any shift or product needing
a bignum trapped with "memory access out of bounds".

**Cause.** `cl_I`'s constructors from `int` and `unsigned int` are generated by
`CL_DEFINE_INT_CONSTRUCTOR` in `include/cln/number.h`, which packs the argument
straight into a fixnum immediate with no range check:

```c
word = cl_combine(cl_FN_tag, wert);
```

That is only valid when every `int` fits a fixnum value, i.e. when
`cl_value_len >= 32`. On a 32-bit target `cl_value_len` is 30, so the payload is
read back as a *signed* 30-bit value and anything with bit 29 set is silently
wrong:

| input | payload | read back as |
|---|---|---|
| `1000000007` | `0x3B9ACA07` | **-73741817** (so `integer_length` said 27) |
| `1<<29` | `0x20000000` | **-2^29** (length 29) |
| `1<<30` | `0x40000000<<2` overflows the word | **0** |

CLN's own header documents the trap -- `cl_I (const int);  // |argument| must be
< 2^29` -- and the `long` / `unsigned long` constructors already do it properly:
they call `cl_I_constructor_from_L` / `_from_UL`, which range-check and build a
bignum when one is needed.

**Fix.** When `cl_value_len < 32`, the `int` and `unsigned int` constructors and
assignment operators delegate to the `long` / `unsigned long` ones. The patch
lives in `build_web.sh` (search for `CLNFIX`).

**Ruled out on the way**, with evidence: GMP (verified exact here -- multiply,
wide shifts and a 100-digit decimal, with `GMP_LIMB_BITS` 32 and no nails); the
build configuration (`long` 32, pointers 32, `cl_value_len` 30, `intDsize` 32,
`uintD` matching `mp_limb_t`); stale objects; the optimiser (`-O0` removed the
traps but the values were still wrong); stack and memory sizing; and the
exception ABI.

**Other 32-bit targets.** This is not specific to wasm: any platform with
`cl_value_len < 32` has the same trap, so the patch is worth upstreaming.

**Build flag.** CLN is compiled with `-O0`. Under Emscripten's optimiser it
miscompiles — integer parsing and multiplication read out of bounds at `-O1` and
`-O2`, and every trap disappears at `-O0`. `alloca` (the malloc-based fallback
behaves identically), strict aliasing (`-fno-strict-aliasing` changes nothing) and
the stack size were each ruled out. It is still fast enough in practice: with
`-O0` a transfer function on a 5-transistor OTA takes about a second in the
browser, and the RC low-pass about 300 ms.

**Verified in a browser** against this build: `examples/rc_lowpass.scx` gives
`H(s) = 1 / (1 + s*C1*R1)` and the OTA gives `H(s) = 1 / (1 + s*C1/gm_M1)`, with
the LaTeX typeset by MathJax and the Bode plot drawn by Plotly; and the editor
works against the real model (place, wire, marquee-select, rotate, delete, undo).

## Licensing

GiNaC is GPL-2.0-or-later (CLN GPL-2.0-or-later, GMP LGPL-3.0-or-later). A site
shipping this WebAssembly build is distributing a GPL-covered work, so the
complete corresponding source must be available to visitors — which it is, as
long as this repository stays public. MathJax and Plotly are Apache-2.0 and
MIT respectively; their notices are in `resources/*/README.txt`.
