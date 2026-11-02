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

# 自举阶段 3：Zx 前端驱动（zxc --fe=zx 的供货方）。用刚编好的 zxc 编它——
# 但必须踩 --fe=c 过河：zxc 默认连模块加载都走 zxfe，而此刻手上的 zxfe
# 还是上一版（新 char 语义它还没吃进去），拿旧前端编新前端会自噬。
# （阶段 4 之后 zxc 可自举出新 zxfe，这一脚才能拆。）
./bin/zxc.exe bootstrap/zxfe.zx -o bin/zxfe.exe --fe=c
echo "built bin/zxfe.exe"
