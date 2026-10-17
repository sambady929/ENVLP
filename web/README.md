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
| ✅ the `.scx` reader and net resolver | `src/app/Document.cpp`, `SymbolicGeom.cpp` |
| ✅ MathJax typesetting and Plotly plots | `resources/mathjax`, `resources/plotly` |
| ❌ the wxWidgets desktop GUI | replace with the web UI here |
| ❌ the Lua console | JavaScript is the scripting layer in the browser |

## Licensing

GiNaC is GPL-2.0-or-later (CLN GPL-2.0-or-later, GMP LGPL-3.0-or-later). A site
shipping this WebAssembly build is distributing a GPL-covered work, so the
complete corresponding source must be available to visitors — which it is, as
long as this repository stays public. MathJax and Plotly are Apache-2.0 and
MIT respectively; their notices are in `resources/*/README.txt`.
