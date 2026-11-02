#!/bin/sh
# bootstrap/diff_lex.sh —— 自举阶段 1 对拍：C 词法器 vs Zx 词法器。
#
# 同一批源文件分别喂给 `zxc --tokens`（src/lex.c）和 `lexdump`（bootstrap/lex.zx），
# 输出逐行 diff，必须全为空。错误用例不比（错误恢复路径本来就不在对拍协议里，
# 见 spec/bootstrap.md §4 阶段 2 的建议）。
#
# 用法：sh bootstrap/diff_lex.sh            # 全量
#       VERBOSE=1 sh bootstrap/diff_lex.sh  # 打出前几行差异
set -e
cd "$(dirname "$0")/.."

ZXC=${ZXC:-bin/zxc.exe}
OUT=.tmp/lexdiff
mkdir -p "$OUT"

pass=0
fail=0
for f in tests/good/*.zx std/*.zx demo/*.zx tools/*.zx bench/*.zx spec/examples/*.zx bootstrap/*.zx; do
    [ -f "$f" ] || continue
    $ZXC "$f" --tokens > "$OUT/c.txt" 2>/dev/null || true
    ./bin/lexdump.exe "$f" > "$OUT/zx.txt" || true
    if diff -q "$OUT/c.txt" "$OUT/zx.txt" >/dev/null 2>&1; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        echo "DIFF $f"
        if [ -n "$VERBOSE" ]; then
            diff "$OUT/c.txt" "$OUT/zx.txt" | head -12
        fi
    fi
done
echo "lex diff: $pass identical, $fail differ"
[ "$fail" -eq 0 ]
