#!/bin/sh
# zxc 回归测试。
#
# 文件类用例（语法/语义/反例/运行/多文件）全部交给 **Zx 写的 runner**
# （tools/runner.zx）——它自己读源码、拼命令、起进程、比对输出。
# 这个脚本只做两件事：
#
#   1. 把 runner 编出来，然后按类别把文件清单交给它；
#   2. 跑"增量编译"这一项——它需要看编译器的 ZX_VERBOSE 输出，
#      属于驱动层的行为，不适合塞进 runner。
#
# 用法：sh test.sh        退出码非零 = 有失败。
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

ZXC=./bin/zxc.exe
# 临时文件放项目内：runner 走 cmd.exe，MSYS 的 /tmp 它不认（会变成 \tmp）
# runner 走 cmd.exe：MSYS 的 /e/... 它不认，得用 pwd -W 的 E:/...
ROOTW="$(cd "$ROOT" && pwd -W 2>/dev/null || echo "$ROOT")"
mkdir -p "$ROOT/.tmp"
TMP="$ROOTW/.tmp/zxc_test.$$"
fail=0

# 1. 文件类用例：交给 Zx runner
printf "== 文件类用例（runner: tools/runner.zx）\n"
if ! $ZXC tools/runner.zx -o "$TMP.runner" 2>"$TMP.err"; then
    printf "  FAIL tools/runner.zx 编译失败\n"
    sed 's/^/       /' "$TMP.err"
    rm -f "$TMP"*
    exit 1
fi
if ! "$TMP.runner" "$ZXC" "$TMP" \
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
