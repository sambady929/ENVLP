#!/usr/bin/env bash
# Build ENVLP's C++ dependencies (GMP, CLN, GiNaC) into a prefix.
#
#   bash tools/build_deps.sh                        # native build -> build-web/host/deps
#   bash tools/build_deps.sh --prefix ~/local/deps  # native build elsewhere
#   bash tools/build_deps.sh --toolchain=emscripten --prefix build-web/deps
#   bash tools/build_deps.sh --toolchain=emscripten # wasm build (for tools/build_web.sh)
#
# Linux distributions and macOS's Homebrew both ship GiNaC, so on those systems
# you can usually skip this and let CMake find the system packages. macOS is the
# exception: Homebrew has GMP but neither CLN nor GiNaC, so there this script
# builds the two into a local prefix that CMake picks up through
# -DENVLP_DEPS_PREFIX=<prefix>.
#
# The WebAssembly build needs the same three libraries compiled for wasm32,
# which is what --toolchain=emscripten does.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="${ENVLP_DEPS_BUILD:-$ROOT/build-web}"
TOOLCHAIN=host
JOBS=${JOBS:-4}
PREFIX_GIVEN=0

GMP_VER=6.3.0
CLN_VER=1.3.7
GINAC_VER=1.8.10

usage() {
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
    --prefix)      DEPS="$2"; PREFIX_GIVEN=1; shift 2 ;;
    --prefix=*)    DEPS="${1#*=}"; PREFIX_GIVEN=1; shift ;;
    --toolchain)   TOOLCHAIN="$2"; shift 2 ;;
    --toolchain=*) TOOLCHAIN="${1#*=}"; shift ;;
    --build-dir)   BUILD="$2"; shift 2 ;;
    --build-dir=*) BUILD="${1#*=}"; shift ;;
    --jobs)        JOBS="$2"; shift 2 ;;
    --jobs=*)      JOBS="${1#*=}"; shift ;;
    -h|--help)     usage 0 ;;
    *) echo "unknown option: $1" >&2; usage 1 ;;
    esac
done
# Each toolchain gets its own source tree and its own default prefix, so a
# native build and a wasm build never reconfigure each other's GMP/CLN/GiNaC
# and never overwrite each other's libraries.
BUILD="$BUILD/$TOOLCHAIN"
if [ "$PREFIX_GIVEN" = 0 ]; then
    DEPS="$BUILD/deps"
fi
case "$DEPS" in /*) ;; *) DEPS="$BUILD/$DEPS" ;; esac
# The prefix and the source trees both have to exist before anything is
# downloaded or unpacked into them.
mkdir -p "$DEPS" "$BUILD"

say() { printf '\n=== %s ===\n' "$*" >&2; }

# ---------------------------------------------------------------------------
# The toolchain
# ---------------------------------------------------------------------------
if [ "$TOOLCHAIN" = emscripten ]; then
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
    export CC=emcc CXX=em++ AR=emar RANLIB=emranlib NM=llvm-nm LD=emcc
    # Exceptions stay on and must match the app build: GiNaC throws
    # (pole_error and friends) and ENVLP catches them.
    export CFLAGS="-O2" CXXFLAGS="-O2 -fexceptions"
    # CLN miscompiles for wasm under -O1/-O2 (integer parsing and multiplication
    # read out of bounds; the traps vanish at -O0), so it is built unoptimised.
    # The probe in tools/wasm-probes/ is the regression test for this.
    CLN_OPT=${CLN_OPT:--O0}
else
    CLN_OPT=${CLN_OPT:--O2}
fi

# The CLN patch is a small awk program (awk is everywhere; python is not).
AWK_PATCH="$ROOT/tools/cln-int-ctor-patch.awk"
[ -f "$AWK_PATCH" ] || { echo "missing $AWK_PATCH" >&2; exit 1; }


fetch() { # fetch <url> <file>
    [ -f "$DEPS/$2" ] || { say "downloading $2"; curl -fsSL "$1" -o "$DEPS/$2"; }
}

unpack() { # unpack <archive> -> $BUILD
    local a="$DEPS/$1" d="${1%.tar.*}"
    [ -d "$BUILD/$d" ] || { say "unpacking $1"; tar -xf "$a" -C "$BUILD"; }
    echo "$BUILD/$d"
}

# ---------------------------------------------------------------------------
# 1. GMP
# ---------------------------------------------------------------------------
# For wasm, GMP is built generic (no assembly): there is no wasm assembly to
# use, and the generic C is what CLN's configure has to link against.
if [ ! -f "$DEPS/lib/libgmp.a" ] && [ ! -f "$DEPS/lib/libgmp.dylib" ]; then
    fetch "https://ftp.gnu.org/gnu/gmp/gmp-$GMP_VER.tar.xz" "gmp-$GMP_VER.tar.xz"
    d=$(unpack "gmp-$GMP_VER.tar.xz")
    say "building GMP"
    (
        cd "$d"
        # GMP's "long long reliability test" compiles a program that calls a
        # zero-argument function with arguments, and requires the compile to
        # SUCCEED -- it is a check for a 2003 compiler crash. Since GCC 14 (and
        # current clang) that call is a hard error, so configure gives up with
        # "could not find a working compiler". Give the function a matching
        # signature: the program then compiles, which is all the test wants.
        if grep -q '^void g(){}$' configure; then
            sed -i 's|^void g(){}$|void g(int a, void *b, int c, void *d, void *e, int f){}|' configure
        fi
        if [ "$TOOLCHAIN" = emscripten ]; then
            ./configure --disable-assembly --host=none-unknown-none \
                --prefix="$DEPS" --disable-shared --enable-static
        else
            ./configure --prefix="$DEPS" --disable-shared --enable-static
        fi
        make -j"$JOBS"
        make install
    )
fi

# ---------------------------------------------------------------------------
# 2. CLN
# ---------------------------------------------------------------------------
if [ ! -f "$DEPS/lib/libcln.a" ] && [ ! -f "$DEPS/lib/libcln.dylib" ]; then
    fetch "https://www.ginac.de/CLN/cln-$CLN_VER.tar.bz2" "cln-$CLN_VER.tar.bz2"
    d=$(unpack "cln-$CLN_VER.tar.bz2")
    say "building CLN"
    (
        cd "$d"
        # CLN's timing code uses the POSIX HZ macro, which macOS does not
        # define either.
        export CFLAGS="$CLN_OPT -DHZ=100" CXXFLAGS="$CLN_OPT -fexceptions -DHZ=100"
        # CLN's int constructors pack their argument straight into a fixnum with
        # no range check, which is only valid when every int fits one
        # (cl_value_len >= 32). On a 32-bit target cl_value_len is 30, so values
        # with bit 29 set are silently corrupted (1000000007 became -73741817).
        # The long constructors already range-check, so make the int ones
        # delegate to them. The patch is inert on a 64-bit target.
        if ! grep -q "CLNFIX" "$d/include/cln/number.h"; then
            awk -f "$AWK_PATCH" "$d/include/cln/number.h" > "$d/include/cln/number.h.new"
            mv "$d/include/cln/number.h.new" "$d/include/cln/number.h"
        fi
        if [ "$TOOLCHAIN" = emscripten ]; then
            # GiNaC finds CLN through pkg-config, which may not be on PATH.
            for d in /c/msys64/ucrt64/bin /usr/bin /usr/local/bin; do
                [ -d "$d" ] && PATH="$d:$PATH"
            done
            export PKG_CONFIG_PATH="$DEPS/lib/pkgconfig"
            export CLN_CFLAGS="-I$DEPS/include"
            export CLN_LIBS="-L$DEPS/lib -lcln -lgmp"
            ./configure --host=wasm32 --disable-shared --enable-static \
                --prefix="$DEPS" \
                CPPFLAGS="-I$DEPS/include" LDFLAGS="-L$DEPS/lib"
        else
            ./configure --prefix="$DEPS" --disable-shared --enable-static \
                CPPFLAGS="-I$DEPS/include" LDFLAGS="-L$DEPS/lib" \
                PKG_CONFIG_PATH="$DEPS/lib/pkgconfig"
        fi
        make -j"$JOBS"
        make install
    )
fi

# ---------------------------------------------------------------------------
# 3. GiNaC
# ---------------------------------------------------------------------------
if [ ! -f "$DEPS/lib/libginac.a" ] && [ ! -f "$DEPS/lib/libginac.dylib" ]; then
    fetch "https://www.ginac.de/ginac-$GINAC_VER.tar.bz2" "ginac-$GINAC_VER.tar.bz2"
    d=$(unpack "ginac-$GINAC_VER.tar.bz2")
    say "building GiNaC"
    (
        cd "$d"
        export PKG_CONFIG_PATH="$DEPS/lib/pkgconfig"
        export CLN_CFLAGS="-I$DEPS/include"
        export CLN_LIBS="-L$DEPS/lib -lcln -lgmp"
        if [ "$TOOLCHAIN" = emscripten ]; then
            ./configure --host=wasm32 --disable-shared --enable-static \
                --prefix="$DEPS" \
                CPPFLAGS="-I$DEPS/include" LDFLAGS="-L$DEPS/lib"
        else
            ./configure --prefix="$DEPS" --disable-shared --enable-static \
                CPPFLAGS="-I$DEPS/include" LDFLAGS="-L$DEPS/lib"
        fi
        # CLN's headers are baked into GiNaC's objects (the int constructors are
        # inline), so a CLN change means rebuilding GiNaC from scratch.
        make clean > /dev/null 2>&1 || true
        make -j"$JOBS"
        make install
    )
fi

say "dependencies ready in $DEPS"
ls "$DEPS/lib" 2>/dev/null | head -20 >&2
