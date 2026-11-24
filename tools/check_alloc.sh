#!/bin/bash
# §232 静态守门（bash/awk 版，替代 python——WindowsApps 的 python 是
# 商店占位符，跑起来 exit=49 无输出，python 版断言在不少机器上是假红）。
# 规则与 python 版一致：src/*.c 里的裸 malloc/calloc/realloc/strdup，
# 只允许三种情况——
#   ① 在 C 字符串字面量里（后端发射的 C 代码文本）
#   ② 后三行内有判空（NULL 是契约，不是疏漏）
#   ③ x* 包装自身的实现（zxc.c）
bad=0
for f in src/*.c; do
    out=$(awk -v FN="$f" '
    { line[NR] = $0 }
    END {
        for (i = 1; i <= NR; i++) {
            l = line[i]
            # 裸分配：前面不是字母/数字/下划线（排除 xmalloc 等包装）
            if (!match(l, /(^|[^_A-Za-z0-9])(malloc|calloc|realloc|strdup)[ \t]*\(/)) continue
            mstart = RSTART + RLENGTH - 1          # 括号位置
            pre = substr(l, 1, mstart)
            # ① 匹配点处于奇数个引号之后 = 字符串字面量内部
            q = 0
            for (j = 1; j <= length(pre); j++)
                if (substr(pre, j, 1) == "\"") q++
            if (q % 2 == 1) continue
            # ③ x* 实现自身
            if (FN == "src/zxc.c" &&
                (l ~ /void \*xmalloc/ || l ~ /void \*xcalloc/ ||
                 l ~ /void \*xrealloc/ || l ~ /char \*xstrdup/ ||
                 l ~ /char \*p = strdup\(s\)/)) continue
            # ② 后三行内判空
            guarded = 0
            for (j = i + 1; j <= i + 3 && j <= NR; j++)
                if (line[j] ~ /if[ \t]*\([ \t]*!/ || line[j] ~ /==[ \t]*NULL/ ||
                    line[j] ~ /!=[ \t]*NULL/) { guarded = 1; break }
            if (guarded) continue
            printf "%s:%d: %s\n", FN, i, substr(l, 1, 80)
            bad++
        }
        exit(bad > 0 ? 1 : 0)
    }' "$f")
    rc=$?
    [ -n "$out" ] && { printf '%s\n' "$out"; bad=1; } || true
    [ $rc -ne 0 ] && [ -z "$out" ] && bad=1     # awk 自身出错也算红
done
if [ $bad -ne 0 ]; then
    echo "Unguarded raw allocations (see above)"
    exit 1
fi
echo "  ok   所有裸分配均有判空或契约保护"
