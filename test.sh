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
# 自举阶段 4：zxfe 由 zxc 的 Zx 前端自己编（闭环）；无 zxfe.exe 时回退 --fe=c
ZFE_FLAG=""
if [ ! -f bin/zxfe.exe ]; then ZFE_FLAG="--fe=c"; fi
if ! $ZXC bootstrap/zxfe.zx -o bin/zxfe.exe $ZFE_FLAG > "$TMP.zxfe.log" 2>&1; then
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

# 2.5 CLI 冒烟：-h / -v / check 真的会检查 / init 带 vendor/
# （这一组挡的是驱动层的事：子命令曾经因为没把 argv[0] 交给前端，从 cwd
#   找不到 zxfe.exe，`check` 只打印 "1 of 1 file(s) failed"、一个诊断都没有）
printf "\n== CLI 冒烟（-h / -v / check / init vendor/）\n"
if $ZXC -v > "$TMP.cli.v" 2>&1 && grep -q "^zxc " "$TMP.cli.v"; then
    printf "  ok   zxc -v\n"
else
    printf "  FAIL zxc -v\n"; fail=1
fi
if $ZXC --help > "$TMP.cli.h" 2>&1 && grep -q "用法" "$TMP.cli.h" \
   && $ZXC build -h 2>&1 | grep -q "zxc build"; then
    printf "  ok   zxc -h / zxc build -h\n"
else
    printf "  FAIL zxc -h\n"; fail=1
fi
pdir="$TMP.cli.proj"
rm -rf "$pdir" 2>/dev/null || true
mkdir -p "$pdir"
if ( cd "$pdir" && "$ZXCW" init demo > "$TMP.cli.init" 2>&1 ); then
    if [ -d "$pdir/demo/vendor" ] && [ -f "$pdir/demo/vendor/README.md" ]; then
        printf "  ok   init 生成 vendor/\n"
    else
        printf "  FAIL init 没有生成 vendor/\n"; fail=1
    fi
    if ( cd "$pdir/demo" && "$ZXCW" check > "$TMP.cli.c1" 2>&1 ); then
        printf "  ok   init 出来的工程 check 通过\n"
    else
        printf "  FAIL init 出来的工程 check 失败\n"; cat "$TMP.cli.c1"; fail=1
    fi
    # 坏文件必须报出真正的诊断（不只是"failed"）
    printf 'const main = fn () i32 {\n    let x: i32 = "abc";\n    return 0;\n};\n' \
        > "$pdir/demo/src/bad.zx"
    if ( cd "$pdir/demo" && "$ZXCW" check > "$TMP.cli.c2" 2>&1 ); then
        printf "  FAIL check 放过了类型错误\n"; fail=1
    elif grep -q "E0301" "$TMP.cli.c2"; then
        printf "  ok   check 报出类型错误（带诊断码）\n"
    else
        printf "  FAIL check 没有诊断输出\n"; cat "$TMP.cli.c2"; fail=1
    fi
    # vendor/ 是模块根：module("ext") → vendor/ext.zx（可省 .zx）
    printf 'const std = module("std");\n\npub const hi = fn () void {\n    std.println("vendor ok");\n};\n' \
        > "$pdir/demo/vendor/ext.zx"
    printf 'const std = module("std");\nconst ext = module("ext");\n\nconst main = fn () i32 {\n    ext.hi();\n    return 0;\n};\n' \
        > "$pdir/demo/src/main.zx"
    rm -f "$pdir/demo/src/bad.zx"
    if ( cd "$pdir/demo" && "$ZXCW" run > "$TMP.cli.r" 2>&1 ) && grep -q "vendor ok" "$TMP.cli.r"; then
        printf "  ok   vendor/ 里的模块能被 module(\"ext\") 找到\n"
    else
        printf "  FAIL vendor/ 模块解析失败\n"; cat "$TMP.cli.r"; fail=1
    fi
else
    printf "  FAIL zxc init\n"; cat "$TMP.cli.init"; fail=1
fi
rm -rf "$pdir" 2>/dev/null || true

# 2.6 LSP 冒烟：语言服务器的模块解析必须和编译器**同一条链**（vendor/ +
# zx.json 的 deps 都要认）。这条挡的是"能编过、IDE 里补不出来"的偏差：
# 两侧各写一份解析链，缺一个目录就是一个点了没反应的句点。
printf "\n== LSP 冒烟（tools/lsptest.py：补全/跳转 + vendor/ 与 deps）\n"
if [ -x "$ZYG" ] && command -v python >/dev/null 2>&1; then
    if $ZYG build-exe tools/zxls.zig -O ReleaseSafe -femit-bin="$TMP.zxls.exe" > "$TMP.zxls.log" 2>&1; then
        if ZXLS_BIN="$TMP.zxls.exe" python tools/lsptest.py > "$TMP.lsp.log" 2>&1; then
            printf "  ok   %s 项全过（含 vendor/ 与 deps 模块）\n" \
                "$(grep -c '^ok' "$TMP.lsp.log" | tr -d ' ')"
        else
            printf "  FAIL LSP 冒烟\n"; tail -20 "$TMP.lsp.log"; fail=1
        fi
    else
        printf "  FAIL zxls 编译失败\n"; tail -10 "$TMP.zxls.log"; fail=1
    fi
else
    printf "  skip（未找到 .toolchain/zig.exe 或 python）\n"
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
