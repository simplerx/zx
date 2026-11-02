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

# zxls：Zx 语言服务器（LSP over stdio），给 Zed / VS Code 提供诊断与补全。
# 注意：zxls 是宿主工具（不走自举），用 anyzig 管理的 zig 0.17.0 编译；
# vendored 0.15 的 API 与 0.17 不兼容（std.Io 接口等）。
# anyzig 会被 ZIG_GLOBAL_CACHE_DIR 干扰（找不到已装版本），故 subshell 里清掉。
if command -v zig >/dev/null 2>&1; then
    if (unset ZIG_GLOBAL_CACHE_DIR ZIG_LOCAL_CACHE_DIR; exec zig 0.17.0 version) >/dev/null 2>&1; then
        (unset ZIG_GLOBAL_CACHE_DIR ZIG_LOCAL_CACHE_DIR; exec zig 0.17.0 build-exe tools/zxls.zig -O ReleaseSafe -femit-bin=bin/zxls.exe)
        echo "built bin/zxls.exe"
    else
        echo "skip bin/zxls.exe（anyzig 侧 zig 0.17.0 不可用，见 README）"
    fi
else
    echo "skip bin/zxls.exe（未找到 zig/anyzig，见 README）"
fi
