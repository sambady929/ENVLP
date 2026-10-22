#!/usr/bin/env bash
# Install the Emscripten SDK into $HOME/emsdk, pinned to one version.
#
#   bash tools/install_emsdk.sh            # uses EMSDK_VERSION
#   EMSDK_VERSION=6.0.12 bash tools/install_emsdk.sh
#
# Why this exists instead of the mymindstorm/setup-emsdk action: that action
# downloads emsdk's *master* branch, which is a frozen legacy snapshot (it
# still carries the old emscripten-releases-tags.txt and the pre-3.0 layout)
# and knows none of the current releases -- "No tool or SDK found by name
# '6.0.12'". emsdk's main branch is the current one, so it is cloned here.
#
# The SDK is large (llvm, node and python), so callers should cache
# $HOME/emsdk between runs.
set -euo pipefail

EMSDK_VERSION=${EMSDK_VERSION:-6.0.12}
EMSDK_DIR=${EMSDK_DIR:-$HOME/emsdk}

# emsdk's own wrapper prefers the python it bundles, then python3 from PATH.
command -v python3 >/dev/null 2>&1 || command -v python >/dev/null 2>&1 || {
    echo "python3 is required to run emsdk" >&2
    exit 1
}
command -v git >/dev/null 2>&1 || { echo "git is required" >&2; exit 1; }

if [ ! -x "$EMSDK_DIR/emsdk" ]; then
    echo "=== cloning emsdk into $EMSDK_DIR ==="
    rm -rf "$EMSDK_DIR"
    git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
fi

cd "$EMSDK_DIR"
echo "=== installing emscripten $EMSDK_VERSION ==="
./emsdk install "$EMSDK_VERSION"
./emsdk activate "$EMSDK_VERSION"

echo "=== emscripten is at $EMSDK_DIR ==="
