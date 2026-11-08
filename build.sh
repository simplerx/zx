#!/bin/sh
# 构建 zxc（编译器）、zxfe（自举前端）、zxls（语言服务器）。
#
#   sh build.sh              # 用仓库内 .toolchain/zig.exe（推荐）
#   ZIG=zig sh build.sh      # 用 PATH 上的 zig（版本需 0.17+）
#   ZIG=/path/to/zig sh build.sh
#
# 首次 clone 时 .toolchain/ 是空的（工具链 360 MB，不入库）：
# 把 zig 0.17.0 解到 .toolchain/zig.exe，或直接用 ZIG= 指过去。
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
ROOTW="$(cd "$ROOT" && pwd -W 2>/dev/null || echo "$ROOT")"
cd "$ROOT"

# 工具链解析：显式 ZIG > 仓库内 vendored > PATH 上的 zig
ZIG="${ZIG:-./.toolchain/zig.exe}"
if ! command -v "$ZIG" >/dev/null 2>&1; then
    if command -v zig >/dev/null 2>&1; then
        ZIG=zig
    else
        echo "build.sh: 找不到 zig。" >&2
        echo "  把 zig 0.17.0 放到 .toolchain/zig.exe，或用 ZIG=<路径> 指定。" >&2
        echo "  下载：https://ziglang.org/download/（zxls 需要 0.17 的 std.Io）" >&2
        exit 1
    fi
fi
echo "toolchain: $ZIG"

# 用 Windows 形式（ROOTW）传缓存目录：POSIX 的 "/e/..." 交给 zig/lld 会被
# 当成没有盘符的路径，链接时报 "unable to make temporary file"。
export ZIG_LOCAL_CACHE_DIR="$ROOTW/.zc-cache/local"
export ZIG_GLOBAL_CACHE_DIR="$ROOTW/.zc-cache/global"
mkdir -p bin .zc-cache/local .zc-cache/global

"$ZIG" cc -std=c11 -Wall -Wextra -O1 -o bin/zxc.exe \
    src/zxc.c src/ty.c src/val.c src/lex.c src/parse.c src/sema.c src/backend.c src/main.c \
    src/fe.c src/astload.c src/errors.c src/cbind.c src/csrc.c src/clib.c src/fetch.c
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
# zxls 是宿主工具（不走自举），代码用 0.17 的 std.Io 接口——别拿 0.15 编。
# 缓存目录沿用上面的仓库内 .zc-cache：**不要**在这里 unset ZIG_*，否则它会
# 退回用户全局缓存，在这台机器上读 toolchain 的 std.zig 会 AccessDenied
# （旧写法就是在这一步偶发失败）。
# 被编辑器占用时（Windows 上 lld-link 报 Permission denied）跳过，不算失败：
# 编译器与前端已经更新，zxls 下次再编。其它失败要把日志打出来，别静默。
ZXLS_LOG=".zc-cache/zxls-build.log"
if "$ZIG" build-exe tools/zxls.zig -O ReleaseSafe -femit-bin=bin/zxls.exe > "$ZXLS_LOG" 2>&1; then
    rm -f bin/zxls.pdb
    echo "built bin/zxls.exe"
else
    rm -f bin/zxls.pdb
    if grep -qi "permission denied" "$ZXLS_LOG"; then
        echo "skip bin/zxls.exe（被编辑器占用：退出 Zed 后重跑，或先改名再编）"
    else
        echo "zxls 构建失败，日志：$ZXLS_LOG" >&2
        tail -6 "$ZXLS_LOG" >&2
    fi
fi

# bin/ 只留三件套：pdb 与 zxfe 的中间文件（万一被写在输出旁边）一并清掉。
rm -f bin/*.pdb bin/zxfe.exe_* bin/zxfe.exe.h bin/zxfe.exe.flags 2>/dev/null || true
