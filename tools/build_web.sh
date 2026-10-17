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
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WEB="$ROOT/web"
BUILD="${ENVLP_WEB_BUILD:-$ROOT/build-web}"
DEPS="$BUILD/deps"

GMP_VER=6.3.0
CLN_VER=1.3.7
GINAC_VER=1.8.10
JOBS=${JOBS:-4}

say() { printf '\n=== %s ===\n' "$*" >&2; }

# Pick up the Emscripten SDK if it is not already on PATH (the msys2 shell here
# starts without it; emsdk lives at /c/emsdk on this machine by default).
if ! command -v em++ >/dev/null 2>&1; then
    for sdk in /c/emsdk /d/emsdk "$HOME/emsdk"; do
        if [ -f "$sdk/upstream/emscripten/em++.py" ]; then
            # node + python ship with the SDK; emcc/emconfigure spawn both.
            for n in "$sdk"/node/*_64bit "$sdk"/python/*_64bit; do
                [ -d "$n" ] && PATH="$n:$PATH"
            done
            PATH="$sdk/upstream/emscripten:$PATH"
            EM_CONFIG="${EM_CONFIG:-$sdk/.emscripten}"
            export EM_CONFIG
            break
        fi
    done
fi

command -v em++ >/dev/null 2>&1 || {
    echo "em++ is not on PATH." >&2
    echo "Install/activate Emscripten first, e.g." >&2
    echo "  git clone https://github.com/emscripten-core/emsdk.git /c/emsdk" >&2
    echo "  cd /c/emsdk && ./emsdk install latest && ./emsdk activate latest" >&2
    echo "  source /c/emsdk/emsdk_env.sh" >&2
    exit 1
}

# The CLN patch step below is a small Python heredoc; the SDK ships one.
PYTHON=$(command -v python3 || command -v python || true)
[ -n "$PYTHON" ] || { echo "Python is needed to patch CLN." >&2; exit 1; }
export PYTHON

mkdir -p "$DEPS" "$BUILD" "$WEB/mathjax" "$WEB/plotly" "$WEB/examples"

fetch() { # fetch <url> <file>
    [ -f "$DEPS/$2" ] || { say "downloading $2"; curl -fsSL "$1" -o "$DEPS/$2"; }
}

unpack() { # unpack <archive> -> $BUILD
    local a="$DEPS/$1" d="${1%.tar.*}"
    [ -d "$BUILD/$d" ] || { say "unpacking $1"; tar -xf "$a" -C "$BUILD"; }
    echo "$BUILD/$d"
}

# Autotools under Emscripten, the way that actually works on Windows: emconfigure
# runs the configure script itself and Windows cannot exec a /bin/sh script, so
# set the compiler environment by hand and let the shell run configure/make.
emu_env() {
    export CC=emcc CXX=em++ AR=emar RANLIB=emranlib NM=llvm-nm LD=emcc
    # Exceptions stay on and must match the app build: GiNaC throws
    # (pole_error and friends) and ENVLP catches them.
    export CFLAGS="-O2" CXXFLAGS="-O2 -fexceptions"
}

# --- 1. GMP ---------------------------------------------------------------
# Built from source: CLN's configure has to *link* against it, which the
# Emscripten gmp port does not make easy from a plain configure run.
if [ ! -f "$DEPS/lib/libgmp.a" ]; then
    fetch "https://ftp.gnu.org/gnu/gmp/gmp-$GMP_VER.tar.xz" "gmp-$GMP_VER.tar.xz"
    d=$(unpack "gmp-$GMP_VER.tar.xz")
    say "building GMP (generic C, no assembly)"
    ( cd "$d"
      emu_env
      ./configure --disable-assembly --host=none-unknown-none \
          --prefix="$DEPS" --disable-shared --enable-static
      make -j"$JOBS"
      make install )
fi

# --- 2. CLN ---------------------------------------------------------------
if [ ! -f "$DEPS/lib/libcln.a" ]; then
    fetch "https://www.ginac.de/CLN/cln-$CLN_VER.tar.bz2" "cln-$CLN_VER.tar.bz2"
    d=$(unpack "cln-$CLN_VER.tar.bz2")
    say "building CLN"
    ( cd "$d"
      emu_env
      # CLN's timing code uses the POSIX HZ macro (ticks per second), which the
      # Emscripten libc does not define.
      #
      # -O0: under Emscripten's optimiser CLN miscompiles -- integer parsing and
      # multiplication read out of bounds at -O1 and -O2, and the traps vanish at
      # -O0. It is not alloca (the malloc fallback behaves the same), not strict
      # aliasing (-fno-strict-aliasing changes nothing) and not the stack size.
      # tools/wasm-probes/ passes completely at -O0, so that is what we build with.
      # The int-constructor patch below is a separate, genuine CLN bug.
      export CXXFLAGS="-O0 -fexceptions -DHZ=100" CFLAGS="-O0 -DHZ=100"
      # CLN's int constructors pack their argument straight into a fixnum with no
      # range check, which is only valid when every int fits one (cl_value_len
      # >= 32). On a 32-bit target cl_value_len is 30, so values with bit 29 set
      # are silently corrupted -- 1000000007 became -73741817, 1<<29 became
      # -2^29, and 1<<30 became 0 -- which breaks the whole engine (and GiNaC's
      # own startup). The long / unsigned long constructors already range-check
      # through cl_I_constructor_from_L / _from_UL, so make the int ones delegate
      # to them. See tools/wasm-probes/.
      if ! grep -q "CLNFIX" "$d/include/cln/number.h"; then
          "${PYTHON:-python}" - "$d/include/cln/number.h" <<'PYEOF'
import io, sys
p = sys.argv[1]
t = io.open(p, "r", encoding="utf-8", errors="surrogateescape").read()
old_ctor = ("#define CL_DEFINE_INT_CONSTRUCTORS(_class_)  \\\n"
            "CL_DEFINE_INT_CONSTRUCTOR(_class_, int)\t\t\t\t\t\\\n"
            "CL_DEFINE_INT_CONSTRUCTOR(_class_, unsigned int)\n")
new_ctor = ("#if (cl_value_len >= 32)\n"
            "// Every int fits into a fixnum value; the packed form is correct.\n"
            "#define CL_DEFINE_INT_CONSTRUCTORS(_class_)  \\\n"
            "CL_DEFINE_INT_CONSTRUCTOR(_class_, int)\t\t\t\t\t\\\n"
            "CL_DEFINE_INT_CONSTRUCTOR(_class_, unsigned int)\n"
            "#else\n"
            "// CLNFIX: cl_value_len is 30 here, so an int does not always fit a\n"
            "// fixnum (bit 29 set, and unsigned >= 2^29, do not). Delegate to the\n"
            "// long / unsigned long constructors, which range-check and build a\n"
            "// bignum when needed.\n"
            "#define CL_DEFINE_INT_CONSTRUCTORS(_class_)  \\\n"
            "inline _class_::_class_ (const int wert) : _class_ ((long) wert) {}\t\\\n"
            "inline _class_::_class_ (const unsigned int wert) : _class_ ((unsigned long) wert) {}\n"
            "#endif\n")
old_assign = ("#define CL_DEFINE_INT_ASSIGNMENT_OPERATORS(_class_)  \\\n"
              "CL_DEFINE_INT_ASSIGNMENT_OPERATOR(_class_, int)\t\t\t\t\\\n"
              "CL_DEFINE_INT_ASSIGNMENT_OPERATOR(_class_, unsigned int)\n")
new_assign = ("#if (cl_value_len >= 32)\n"
              "#define CL_DEFINE_INT_ASSIGNMENT_OPERATORS(_class_)  \\\n"
              "CL_DEFINE_INT_ASSIGNMENT_OPERATOR(_class_, int)\t\t\t\t\\\n"
              "CL_DEFINE_INT_ASSIGNMENT_OPERATOR(_class_, unsigned int)\n"
              "#else\n"
              "#define CL_DEFINE_INT_ASSIGNMENT_OPERATORS(_class_)  \\\n"
              "inline _class_& _class_::operator= (const int wert)\t\t\t\\\n"
              "{ return *this = (long) wert; }\t\t\t\t\t\\\n"
              "inline _class_& _class_::operator= (const unsigned int wert)\t\t\\\n"
              "{ return *this = (unsigned long) wert; }\n"
              "#endif\n")
for old, new in ((old_ctor, new_ctor), (old_assign, new_assign)):
    if old not in t:
        sys.exit("CLN patch: pattern not found in " + p)
    t = t.replace(old, new, 1)
io.open(p, "w", encoding="utf-8", errors="surrogateescape").write(t)
PYEOF
      fi
      ./configure --host=wasm32 --disable-shared --enable-static \
          --prefix="$DEPS" \
          CPPFLAGS="-I$DEPS/include" LDFLAGS="-L$DEPS/lib"
      make clean > /dev/null 2>&1 || true
      make -j"$JOBS"
      make install )
fi

# --- 3. GiNaC -------------------------------------------------------------
if [ ! -f "$DEPS/lib/libginac.a" ]; then
    fetch "https://www.ginac.de/ginac-$GINAC_VER.tar.bz2" "ginac-$GINAC_VER.tar.bz2"
    d=$(unpack "ginac-$GINAC_VER.tar.bz2")
    say "building GiNaC"
    ( cd "$d"
      emu_env
      # GiNaC finds CLN through pkg-config, which is not on this shell's PATH
      # (it lives in the MSYS2 toolchain tree). Point it at the .pc we just
      # installed and hand it the flags directly as a fallback.
      export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
      export PKG_CONFIG_PATH="$DEPS/lib/pkgconfig"
      export CLN_CFLAGS="-I$DEPS/include"
      export CLN_LIBS="-L$DEPS/lib -lcln -lgmp"
      ./configure --host=wasm32 --disable-shared --enable-static \
          --prefix="$DEPS" \
          CPPFLAGS="-I$DEPS/include" LDFLAGS="-L$DEPS/lib"
      # CLN's headers are baked into GiNaC's objects (int constructors are
      # inline), so a CLN change means rebuilding GiNaC from scratch.
      make clean > /dev/null 2>&1 || true
      make -j"$JOBS"
      make install )
fi

# --- 4. the engine --------------------------------------------------------
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
    -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web \
    -o "$WEB/envlp.js"

# --- 5. assets ------------------------------------------------------------
say "copying the typesetting and plotting bundles + examples"
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
