#!/bin/sh
# bootstrap/diff_errors.sh —— 错误路径双前端对拍。
#
# diff_ast.sh 用 astdump 对拍好用例（它是简化 dump 工具，不查 parser 错误状态，
# 喂 bad 用例会产出残缺 AST，造成假差异）。本脚本改让 zxc 的两个**完整**前端
# 各跑一遍 bad/multibad 用例：C 前端（--fe=c）vs Zx 前端（默认）。
# 比较**首个**诊断码：两个前端的错误恢复策略不必相同（Zx 前端可能在首个
# 错误后多报一个恢复性 E0201），但同一个根因必须报同一个码。比全集合会把
# 恢复差异淹没成假漂移。一边有诊断另一边没有也算漂移。
#
# 用法：sh bootstrap/diff_errors.sh
#       LIMIT=30 sh bootstrap/diff_errors.sh     # 只跑前 N 个（抽样）
# 退出码：0 = 无漂移，非 0 = 漂移个数。
set -u
cd "$(dirname "$0")/.."

fail=0
total=0
for f in tests/bad/*.zx tests/multibad/*/main.zx; do
    [ -f "$f" ] || continue
    if [ -n "${LIMIT:-}" ] && [ "$total" -ge "$LIMIT" ]; then break; fi
    total=$((total+1))

    cout=$(./bin/zxc.exe "$f" --fe=c 2>&1)
    zout=$(./bin/zxc.exe "$f" 2>&1)
    ccs=$(printf '%s\n' "$cout" | grep -oE 'error\[E[0-9]{4}\]' | head -1)
    zcs=$(printf '%s\n' "$zout" | grep -oE 'error\[E[0-9]{4}\]' | head -1)

    if [ "$ccs" != "$zcs" ]; then
        printf 'DIAG-DIFF %s\n  c:  [%s]\n  zx: [%s]\n' "$f" "$ccs" "$zcs"
        fail=$((fail+1))
    fi
done
echo "----"
echo "error-path diff: $((total-fail)) identical, $fail mismatched (of $total)"
exit $fail
