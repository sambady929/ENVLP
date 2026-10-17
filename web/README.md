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

## Licensing

GiNaC is GPL-2.0-or-later (CLN GPL-2.0-or-later, GMP LGPL-3.0-or-later). A site
shipping this WebAssembly build is distributing a GPL-covered work, so the
complete corresponding source must be available to visitors — which it is, as
long as this repository stays public. MathJax and Plotly are Apache-2.0 and
MIT respectively; their notices are in `resources/*/README.txt`.
