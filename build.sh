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
    src/zxc.c src/ty.c src/val.c src/lex.c src/parse.c src/sema.c src/backend.c src/main.c \
    src/fe.c src/astload.c
echo "built bin/zxc.exe"

# 自举阶段 4：zxc 用**自己的 Zx 前端**编 zxfe——闭环。阶段 3 的 --fe=c
# 脚手架拆了，只留一个种子回退：全新 checkout 没有 bin/zxfe.exe 时，
# zxc 没有前端可用，第一步仍踩 --fe=c 过河（之后每次构建都是 Zx 编 Zx）。
ZFE_FLAG=""
if [ ! -f bin/zxfe.exe ]; then
    ZFE_FLAG="--fe=c"
    echo "seed: bin/zxfe.exe 不存在，首次构建走 C 前端（--fe=c）"
fi
./bin/zxc.exe bootstrap/zxfe.zx -o bin/zxfe.exe $ZFE_FLAG
echo "built bin/zxfe.exe"
