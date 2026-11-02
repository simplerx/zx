#!/bin/sh
# bootstrap/diff_ast.sh —— 自举阶段 2 的全量对拍。
# 每个源文件分别用 C 版（zxc --ast）和 Zx 版（astdump）dump AST，逐字节 diff。
# 输出为空 = 两套解析器对同一输入产出完全相同的 AST。
cd "$(dirname "$0")/.."

fail=0
total=0
for f in tests/good/*.zx std/*.zx bench/*.zx bootstrap/lex.zx bootstrap/parse.zx spec/*.zx; do
    total=$((total+1))
    ./bin/zxc.exe "$f" --ast > .tmp/c_ast.txt 2>/dev/null
    c_rc=$?
    ./bin/astdump.exe "$f" > .tmp/zx_ast.txt 2>/dev/null
    z_rc=$?
    if [ $c_rc -ne 0 ] && [ $z_rc -ne 0 ]; then
        continue   # 两边都拒收（spec 片段文件），跳过
    fi
    if [ $c_rc -ne 0 ] || [ $z_rc -ne 0 ]; then
        echo "REJECT-ONLY-ONE: $f (c=$c_rc z=$z_rc)"
        fail=$((fail+1))
        continue
    fi
    if ! diff -q .tmp/c_ast.txt .tmp/zx_ast.txt > /dev/null 2>&1; then
        echo "DIFF: $f"
        fail=$((fail+1))
    fi
done
echo "----"
echo "compared: $((total-fail)) identical, $fail mismatched (of $total)"
exit $fail
