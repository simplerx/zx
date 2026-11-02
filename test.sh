#!/bin/sh
# zxc 回归测试。
#
# 文件类用例（语法/语义/反例/运行/多文件）全部交给 **tools/runner.c**：
# 它自己读源码、起进程、比对输出，并顺手把每项的耗时记下来。
# （旧的 tools/runner.zx 已不再参与；用 system() 拼 cmd 行时它自己的
#   报告会整块丢，详见 runner.c 文件头。）
#
# 这个脚本只做两件事：
#   1. 把 runner 编出来（zig cc，一次 ~1s），然后把文件清单交给它；
#   2. 跑"增量编译"这一项——它需要看编译器的 ZX_VERBOSE 输出，
#      属于驱动层的行为，不适合塞进 runner。
#
# 用法：sh test.sh                 退出码非零 = 有失败
#       sh test.sh --baseline .tmp/zx-perf.tsv   顺带比一下性能基线
#       sh test.sh --save .tmp/zx-perf.tsv       记录一份新基线
#       ZX_CHECKED=1 sh test.sh                  用带运行时检查的 Debug 编译器跑
#                                                （抓未初始化内存这类 UB，慢一倍）
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

ZYG=./.toolchain/zig.exe
ZXC=./bin/zxc.exe
ROOTW="$(cd "$ROOT" && pwd -W 2>/dev/null || echo "$ROOT")"
mkdir -p "$ROOT/.tmp" "$ROOT/bin"
TMP="$ROOTW/.tmp/zxc_test.$$"
fail=0

# 0. 用哪份编译器
if [ "${ZX_CHECKED:-0}" = "1" ]; then
    printf "== 构建编译器（Debug -O0，zig 的运行时检查开着）\n"
    $ZYG cc -std=c11 -O0 -g -o bin/zxc_checked.exe \
        src/zxc.c src/ty.c src/val.c src/lex.c src/parse.c src/sema.c \
        src/backend.c src/main.c src/fe.c src/astload.c
    ZXC=./bin/zxc_checked.exe
fi
# zxc 从自己的路径推 zig cc 的位置，给反斜杠路径它就不必靠 cwd
ZXCW="$(printf '%s' "$ROOTW/${ZXC#./}" | tr '/' '\\')"

# 1. runner 自己（编到本次运行的临时名下：两个人同时跑 test.sh 不会抢同一个 exe）
printf "\n== 构建 runner（tools/runner.c）\n"
RUNNER="$TMP.runner.exe"
if ! $ZYG cc -std=c11 -O2 -o "$RUNNER" tools/runner.c; then
    printf "  FAIL runner 编译失败\n"
    exit 1
fi

# 2. 文件类用例
printf "\n== 构建 Zx 前端（bootstrap/zxfe.zx，由 zxc 自己编译——自举）\n"
# 自举鸡蛋：编 zxfe 必须走 --fe=c（旧 zxfe 吃不进新 char 语义，见 build.sh）
if ! $ZXC bootstrap/zxfe.zx -o bin/zxfe.exe --fe=c > "$TMP.zxfe.log" 2>&1; then
    printf "  FAIL zxfe 编译失败\n"
    cat "$TMP.zxfe.log"
    exit 1
fi

printf "\n== 文件类用例\n"
if ! "$RUNNER" "$@" "$ZXCW" "$TMP" \
        spec/examples/*.zx tests/good/*.zx tests/bad/*.zx \
        tests/multi/*/main.zx tests/multibad/*/main.zx; then
    fail=1
fi

# 2. 增量编译：每个模块一个 .o，没改动的模块不重编
printf "\n== 增量编译 (per-module .o)\n"
ib="$TMP.inc"
rm -f "$ib"* 2>/dev/null || true
$ZXC tests/multi/alias/main.zx -o "$ib" >/dev/null 2>&1 || true
if [ -f "$ib.exe" ] || [ -f "$ib" ]; then
    nobj=$(ls "$ib"*.o 2>/dev/null | wc -l | tr -d ' ')
    if [ "$nobj" -ge 3 ]; then printf "  ok   每个模块一个 .o（%s 个）\n" "$nobj"
    else printf "  FAIL 只生成了 %s 个 .o\n" "$nobj"; fail=1; fi

    again=$(ZX_VERBOSE=1 $ZXC tests/multi/alias/main.zx -o "$ib" 2>&1 >/dev/null | grep -c '^  cc ' || true)
    if [ "$again" = "0" ]; then printf "  ok   内容没变就不重编\n"
    else printf "  FAIL 重复编译又编了 %s 个单元\n" "$again"; fail=1; fi
else
    printf "  FAIL 增量编译用例没编出二进制\n"; fail=1
fi

rm -f "$TMP"* 2>/dev/null || true

if [ $fail -ne 0 ]; then
    echo "FAILED"
    exit 1
fi
echo "all checks passed"
