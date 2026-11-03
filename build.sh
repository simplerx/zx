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
rm -f bin/zxc.pdb
echo "built bin/zxc.exe"

# 自举阶段 4：zxc 用**自己的 Zx 前端**编 zxfe——闭环。阶段 3 的 --fe=c
# 脚手架拆了，只留一个种子回退：全新 checkout 没有 bin/zxfe.exe 时，
# zxc 没有前端可用，第一步仍踩 --fe=c 过河（之后每次构建都是 Zx 编 Zx）。
#
# 产物先落在 .zc-cache/：zxc 会把每个模块的 .c/.o 写在输出文件旁边，
# 直接编进 bin/ 会让 bin 里堆 30 多个中间文件。增量照样生效（路径稳定），
# bin/ 只留三件套。
ZFE_FLAG=""
if [ ! -f bin/zxfe.exe ]; then
    ZFE_FLAG="--fe=c"
    echo "seed: bin/zxfe.exe 不存在，首次构建走 C 前端（--fe=c）"
fi
./bin/zxc.exe bootstrap/zxfe.zx -o .zc-cache/zxfe.exe $ZFE_FLAG
cp .zc-cache/zxfe.exe bin/zxfe.exe
echo "built bin/zxfe.exe"

# zxls：Zx 语言服务器（LSP over stdio），给 Zed / VS Code 提供诊断与补全。
# zxls 是宿主工具（不走自举），代码用 0.17 的 std.Io 接口——vendored
# 工具链升到 0.17.0 之后直接用它编，不再绕道 anyzig（旧 0.15 API 不兼容）。
if [ -f .toolchain/zig.exe ]; then
    (unset ZIG_GLOBAL_CACHE_DIR ZIG_LOCAL_CACHE_DIR; exec ./.toolchain/zig.exe build-exe tools/zxls.zig -O ReleaseSafe -femit-bin=bin/zxls.exe) \
        && echo "built bin/zxls.exe" \
        || echo "skip bin/zxls.exe（可能被 Zed 占用；先退出 Zed 或手编到别名再替换）"
else
    echo "skip bin/zxls.exe（未找到 .toolchain/zig.exe）"
fi
