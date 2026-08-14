#!/bin/sh
# Build audiotard's DSP core to WebAssembly with plain clang + wasi-libc.
set -e
cd "$(dirname "$0")"
clang --target=wasm32-wasi --sysroot=/usr \
    -isystem /usr/include/wasm32-wasi \
    -O2 -std=c17 -DNDEBUG \
    -nostartfiles -Wl,--no-entry -Wl,--export=__heap_base \
    -Wl,--initial-memory=67108864 -Wl,--max-memory=1073741824 \
    -mexec-model=reactor \
    exports.c ../src/chain.c ../src/engine.c ../src/effects.c \
    -o audiotard.wasm
ls -la audiotard.wasm
