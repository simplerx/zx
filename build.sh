#!/bin/sh
# Build the zxc compiler with the vendored zig toolchain (.toolchain/zig.exe).
# This script lives in the project root.
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

export ZIG_LOCAL_CACHE_DIR="$ROOT/.zc-cache/local"
export ZIG_GLOBAL_CACHE_DIR="$ROOT/.zc-cache/global"
mkdir -p bin .zc-cache/local .zc-cache/global

./.toolchain/zig.exe cc -std=c11 -Wall -Wextra -O1 -o bin/zxc.exe \
    src/zxc.c src/ty.c src/val.c src/lex.c src/parse.c src/sema.c src/backend.c src/main.c
echo "built bin/zxc.exe"
