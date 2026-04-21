#!/usr/bin/env bash
# Build slothdb.wasm + slothdb.js for the playground.
#
# Requires: emsdk activated (source <emsdk>/emsdk_env.sh).
# Output: build-wasm/src/slothdb.js + slothdb.wasm

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-wasm"

# Prefer native emsdk if present; helper for common install path.
if ! command -v emcmake >/dev/null 2>&1; then
    if [ -f "$HOME/emsdk/emsdk_env.sh" ]; then
        # shellcheck source=/dev/null
        source "$HOME/emsdk/emsdk_env.sh" >/dev/null
    elif [ -f "/c/Users/$USER/emsdk/emsdk_env.sh" ]; then
        # shellcheck source=/dev/null
        source "/c/Users/$USER/emsdk/emsdk_env.sh" >/dev/null
    fi
fi

# On Git Bash / Windows, emcmake ships as emcmake.bat. Resolve either.
EMCMAKE=$(command -v emcmake || command -v emcmake.bat || true)
EMMAKE=$(command -v emmake || command -v emmake.bat || true)
if [ -z "$EMCMAKE" ] || [ -z "$EMMAKE" ]; then
    echo "error: emcmake/emmake not on PATH. Source emsdk_env.sh first." >&2
    exit 1
fi

mkdir -p "$BUILD"
"$EMCMAKE" cmake -S "$ROOT" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSLOTHDB_BUILD_TESTS=OFF \
    -DSLOTHDB_BUILD_SHELL=OFF \
    -G "MinGW Makefiles"

"$EMMAKE" cmake --build "$BUILD" -j --target slothdb_wasm

echo
echo "Built: $BUILD/src/slothdb.js + slothdb.wasm"
ls -lh "$BUILD/src/slothdb.js" "$BUILD/src/slothdb.wasm" 2>/dev/null || true
