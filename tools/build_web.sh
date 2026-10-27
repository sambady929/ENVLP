#!/usr/bin/env bash
# Build ENVLP's analysis core to WebAssembly and lay out the web/ directory so
# it can be served (or uploaded to a website) as-is.
#
#   source /c/emsdk/emsdk_env.sh   # or run emsdk_env.bat / emsdk_env.ps1
#   tools/build_web.sh
#
# The desktop GUI (wxWidgets) and the Lua console are deliberately not part of
# this build: the browser is the UI here, and JavaScript is the scripting layer.
# What *is* compiled is the whole symbolic engine plus the .scx loader, which is
# all wx-free.
#
# The symbolic dependencies (GMP, CLN, GiNaC) are built by tools/build_deps.sh,
# which works for any host and for a wasm cross build alike.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WEB="$ROOT/web"
BUILD="${ENVLP_WEB_BUILD:-$ROOT/build-web}"
DEPS="$BUILD/deps"
JOBS=${JOBS:-4}

say() { printf '\n=== %s ===\n' "$*" >&2; }

# Pick up the Emscripten SDK if it is not already on PATH. (build_deps.sh does
# the same for itself; a child process cannot pass its PATH back.)
if ! command -v em++ >/dev/null 2>&1; then
    for sdk in "${EMSDK:-}" /c/emsdk /d/emsdk "$HOME/emsdk" /opt/emsdk \
               /usr/local/emsdk; do
        [ -n "$sdk" ] || continue
        [ -f "$sdk/upstream/emscripten/em++.py" ] || continue
        # node and python ship with the SDK; emcc spawns both.
        for n in "$sdk"/node/*_64bit "$sdk"/python/*_64bit; do
            [ -d "$n" ] && PATH="$n:$PATH"
        done
        PATH="$sdk/upstream/emscripten:$PATH"
        export EM_CONFIG="${EM_CONFIG:-$sdk/.emscripten}"
        break
    done
fi
command -v em++ >/dev/null 2>&1 || {
    echo "em++ is not on PATH. Install and activate the Emscripten SDK:" >&2
    echo "  git clone https://github.com/emscripten-core/emsdk.git ~/emsdk" >&2
    echo "  cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest" >&2
    exit 1
}

# --- 1. the dependencies (GMP, CLN, GiNaC) for wasm32 ----------------------
say "building the symbolic dependencies for wasm32"
bash "$ROOT/tools/build_deps.sh" --toolchain=emscripten --prefix="$DEPS" --jobs="$JOBS"

# --- 2. the engine --------------------------------------------------------
say "compiling envlp.js + envlp.wasm"
em++ -O2 -std=c++17 -fexceptions \
    -I"$ROOT/src" -I"$DEPS/include" \
    "$ROOT"/src/core/*.cpp \
    "$ROOT/src/app/Document.cpp" \
    "$ROOT/src/app/SymbolGeom.cpp" \
    "$ROOT/src/app/SymbolDraw.cpp" \
    "$ROOT/src/web/WebApi.cpp" \
    "$ROOT/src/web/EditorApi.cpp" \
    "$ROOT/src/web/SymbolArt.cpp" \
    -L"$DEPS/lib" -lginac -lcln -lgmp \
    -sMODULARIZE=1 -sEXPORT_NAME=createEnvLp \
    -sEXPORTED_FUNCTIONS='["_envlp_run","_envlp_free","_envlp_doc_open","_envlp_doc_geometry","_envlp_doc_serialize","_envlp_doc_palette","_envlp_doc_set","_envlp_doc_undo","_envlp_doc_redo","_envlp_symbol_art"]' \
    -sEXPORTED_RUNTIME_METHODS='["ccall","UTF8ToString"]' \
    # A large symbolic circuit (transconductance block + coupled inductors)
    # recurses deeply enough to exhaust the default wasm stack; the trap
    # surfaced as "table index is out of bounds" in the browser. 16 MiB is
    # comfortably above the native stack these analyses need.
    -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=16777216 -sENVIRONMENT=web \
    -o "$WEB/envlp.js"

# --- 3. assets ------------------------------------------------------------
say "copying the typesetting and plotting bundles + examples"
mkdir -p "$WEB/mathjax" "$WEB/plotly" "$WEB/examples"
cp "$ROOT/resources/mathjax/tex-svg.js"            "$WEB/mathjax/"
cp "$ROOT/resources/plotly/plotly-basic.min.js"   "$WEB/plotly/"
cp "$ROOT"/examples/*.scx                         "$WEB/examples/"
( cd "$WEB/examples" && ls *.scx | sed 's/^/"/;s/$/"/' | paste -sd, - \
    | sed 's/^/[/;s/$/]/' > index.json )
# The stub module + its canned result are only for exercising the page before
# the real build exists (envlp.js above is the real one now).
rm -f "$WEB/fake_result.json" "$WEB/_art.json" "$WEB/_geom.json"

say "done"
ls -la "$WEB" | sed -n '1,20p'
du -sh "$WEB" 2>/dev/null || true
