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

# 所有子进程（runner → zxc → zig cc）统一用仓库内缓存，理由有两条：
# ① 不往用户 home 里的 zig 缓存塞东西；② 避免多份 zig 抢同一个全局缓存
# 锁——抢锁时表现为"某一次 backend compile 卡到 60s 超时"，每次卡的文件
# 还不一样，看起来像随机 flaky。
# 注意用 ROOTW（Windows 形式）：POSIX 形式的 "/e/..." 交给 zig/lld 会被当成
# 无盘符路径，链接时报 "unable to make temporary file"。
export ZIG_LOCAL_CACHE_DIR="$ROOTW/.zc-cache/local"
export ZIG_GLOBAL_CACHE_DIR="$ROOTW/.zc-cache/global"
mkdir -p "$ZIG_LOCAL_CACHE_DIR" "$ZIG_GLOBAL_CACHE_DIR"

# 临时前缀**不能**叫 TMP：TMP/TEMP 是环境里已导出的系统临时目录，覆盖它
# 会把 "%TMP%\zig-xxxx" 这种路径传给子进程（zig/lld），链接时报
# "unable to make temporary file: No such file or directory"。
TPFX="$ROOTW/.tmp/zxc_test.$$"
fail=0

# 0. 用哪份编译器
if [ "${ZX_CHECKED:-0}" = "1" ]; then
    printf "== 构建编译器（Debug -O0，zig 的运行时检查开着）\n"
    $ZYG cc -std=c11 -O0 -g -o bin/zxc_checked.exe \
        src/zxc.c src/ty.c src/val.c src/lex.c src/parse.c src/sema.c \
        src/backend.c src/main.c src/fe.c src/astload.c src/errors.c
    ZXC=./bin/zxc_checked.exe
fi
# zxc 从自己的路径推 zig cc 的位置，给反斜杠路径它就不必靠 cwd
ZXCW="$(printf '%s' "$ROOTW/${ZXC#./}" | tr '/' '\\')"

# 1. runner 自己（编到本次运行的临时名下：两个人同时跑 test.sh 不会抢同一个 exe）
printf "\n== 构建 runner（tools/runner.c）\n"
RUNNER="$TPFX.runner.exe"
if ! $ZYG cc -std=c11 -O2 -o "$RUNNER" tools/runner.c; then
    printf "  FAIL runner 编译失败\n"
    exit 1
fi

# 2. 文件类用例
printf "\n== 构建 Zx 前端（bootstrap/zxfe.zx，由 zxc 自己编译——自举）\n"
# 自举阶段 4：zxfe 由 zxc 的 Zx 前端自己编（闭环）；无 zxfe.exe 时回退 --fe=c
ZFE_FLAG=""
if [ ! -f bin/zxfe.exe ]; then ZFE_FLAG="--fe=c"; fi
if ! $ZXC bootstrap/zxfe.zx -o bin/zxfe.exe $ZFE_FLAG > "$TPFX.zxfe.log" 2>&1; then
    printf "  FAIL zxfe 编译失败\n"
    cat "$TPFX.zxfe.log"
    exit 1
fi
# zxc 把每个模块的中间文件写在输出旁边——bin/ 只该留 zxfe.exe 本体
rm -f bin/zxfe.exe_* bin/zxfe.exe.h bin/zxfe.exe.flags bin/*.pdb 2>/dev/null || true

printf "\n== 文件类用例\n"
if ! "$RUNNER" "$@" "$ZXCW" "$TPFX" \
        spec/examples/*.zx tests/good/*.zx tests/bad/*.zx \
        tests/multi/*/main.zx tests/multibad/*/main.zx; then
    fail=1
fi

# 2.5 CLI 冒烟：-h / -v / check 真的会检查 / init 带 vendor/
# （这一组挡的是驱动层的事：子命令曾经因为没把 argv[0] 交给前端，从 cwd
#   找不到 zxfe.exe，`check` 只打印 "1 of 1 file(s) failed"、一个诊断都没有）
printf "\n== CLI 冒烟（-h / -v / check / init vendor/）\n"
if $ZXC -v > "$TPFX.cli.v" 2>&1 && grep -q "^zxc " "$TPFX.cli.v"; then
    printf "  ok   zxc -v\n"
else
    printf "  FAIL zxc -v\n"; fail=1
fi
if $ZXC --help > "$TPFX.cli.h" 2>&1 && grep -q "usage" "$TPFX.cli.h" \
   && $ZXC build -h 2>&1 | grep -q "zxc build"; then
    printf "  ok   zxc -h / zxc build -h\n"
else
    printf "  FAIL zxc -h\n"; fail=1
fi
pdir="$TPFX.cli.proj"
rm -rf "$pdir" 2>/dev/null || true
mkdir -p "$pdir"
if ( cd "$pdir" && "$ZXCW" init demo > "$TPFX.cli.init" 2>&1 ); then
    if [ -d "$pdir/demo/vendor" ] && [ -f "$pdir/demo/vendor/README.md" ]; then
        printf "  ok   init 生成 vendor/\n"
    else
        printf "  FAIL init 没有生成 vendor/\n"; fail=1
    fi
    if ( cd "$pdir/demo" && "$ZXCW" check > "$TPFX.cli.c1" 2>&1 ); then
        printf "  ok   init 出来的工程 check 通过\n"
    else
        printf "  FAIL init 出来的工程 check 失败\n"; cat "$TPFX.cli.c1"; fail=1
    fi
    # 坏文件必须报出真正的诊断（不只是"failed"）
    printf 'const main = fn () i32 {\n    let x: i32 = "abc";\n    return 0;\n};\n' \
        > "$pdir/demo/src/bad.zx"
    if ( cd "$pdir/demo" && "$ZXCW" check > "$TPFX.cli.c2" 2>&1 ); then
        printf "  FAIL check 放过了类型错误\n"; fail=1
    elif grep -q "E0301" "$TPFX.cli.c2"; then
        printf "  ok   check 报出类型错误（带诊断码）\n"
    else
        printf "  FAIL check 没有诊断输出\n"; cat "$TPFX.cli.c2"; fail=1
    fi
    # vendor/ 是模块根：module("ext") → vendor/ext.zx（可省 .zx）
    printf 'const std = module("std");\n\npub const hi = fn () void {\n    std.println("vendor ok");\n};\n' \
        > "$pdir/demo/vendor/ext.zx"
    printf 'const std = module("std");\nconst ext = module("ext");\n\nconst main = fn () i32 {\n    ext.hi();\n    return 0;\n};\n' \
        > "$pdir/demo/src/main.zx"
    rm -f "$pdir/demo/src/bad.zx"
    if ( cd "$pdir/demo" && "$ZXCW" run > "$TPFX.cli.r" 2>&1 ) && grep -q "vendor ok" "$TPFX.cli.r"; then
        printf "  ok   vendor/ 里的模块能被 module(\"ext\") 找到\n"
    else
        printf "  FAIL vendor/ 模块解析失败\n"; cat "$TPFX.cli.r"; fail=1
    fi
else
    printf "  FAIL zxc init\n"; cat "$TPFX.cli.init"; fail=1
fi

# 2.52 `zxc add`：C 头 -> Zx 绑定。守的是整条链路（clang AST 解析、
# 类型映射、extern var 文件作用域、生成物能被 Zx 自己检查），
# 不是只测命令有没有打印成功。
printf "\n== zxc add（C 头自动生成绑定）\n"
cat > "$pdir/demo/vendor/cliadd.h" <<'CHDR'
#define CLI_MAGIC 7
typedef struct Pt { int x; int y; } Pt;
enum CliMode { CLI_A = 2, CLI_B };
extern int cli_g;
int cli_sum(int a, int b);
void cli_move(Pt *p, int dx);
CHDR
if ( cd "$pdir/demo" && "$ZXCW" add cliadd > "$TPFX.cli.add" 2>&1 ) \
   && grep -q "pub fn cli_sum(a: i32, b: i32) i32;" "$pdir/demo/vendor/cliadd.zx" \
   && grep -q "pub const CLI_MAGIC: i32 = 7;" "$pdir/demo/vendor/cliadd.zx" \
   && grep -q "pub extern var cli_g: i32;" "$pdir/demo/vendor/cliadd.zx" \
   && ( cd "$pdir/demo" && "$ZXCW" check vendor/cliadd.zx > "$TPFX.cli.addcheck" 2>&1 ); then
    printf "  ok   zxc add 生成可检查的 C 绑定\n"
else
    printf "  FAIL zxc add\n"
    cat "$TPFX.cli.add" "$TPFX.cli.addcheck" 2>/dev/null
    cat "$pdir/demo/vendor/cliadd.zx" 2>/dev/null
    fail=1
fi
# 2.53 zxc 自己编 C 库：add 自动编译 vendor/*.c，改 .c 后 build 自动
# 增量重编并重链。挡的是两件事：① 手工编库时 sanitizer 档位对不上；
# ② "无改动重建"跳过链接，导致改了 C 源码却还在跑旧二进制。
printf "\n== zxc 编译 C 库（add / build / cbuild）\n"
cat > "$pdir/demo/vendor/cliadd.c" <<'CSRC'
#include "cliadd.h"
int cli_g = 4;
int cli_sum(int a, int b) { return a + b; }
void cli_move(Pt *p, int dx) { p->x += dx; }
CSRC
cat > "$pdir/demo/src/main.zx" <<'ZSRC'
const std = module("std");
const c = module("cliadd");
const main = fn () i32 {
    std.println("sum={} g={}", c.cli_sum(2, 3), c.cli_g);
    return 0;
};
ZSRC
if ( cd "$pdir/demo" && "$ZXCW" add cliadd --force > "$TPFX.cli.cadd" 2>&1 ) \
   && [ -f "$pdir/demo/vendor/libcliadd.a" ] \
   && grep -q '@link("vendor/libcliadd.a")' "$pdir/demo/vendor/cliadd.zx" \
   && ( cd "$pdir/demo" && "$ZXCW" run > "$TPFX.cli.crun" 2>&1 ) \
   && grep -q "sum=5 g=4" "$TPFX.cli.crun"; then
    printf "  ok   add 自动编译 C 源码并链接运行\n"
else
    printf "  FAIL add 自动编译 C 库\n"
    cat "$TPFX.cli.cadd" "$TPFX.cli.crun" 2>/dev/null
    fail=1
fi
# 改 .c 后只跑 run（内部就是 build）：必须自动重编、强制重链
sed -i 's/return a + b;/return a + b + 10;/' "$pdir/demo/vendor/cliadd.c"
if ( cd "$pdir/demo" && "$ZXCW" run > "$TPFX.cli.crun2" 2>&1 ) \
   && grep -q "sum=15 g=4" "$TPFX.cli.crun2"; then
    printf "  ok   改 .c 后 build 自动重编重链\n"
else
    printf "  FAIL 改 .c 后没有重编重链\n"; cat "$TPFX.cli.crun2" 2>/dev/null; fail=1
fi
# 内容没变：cbuild 增量应跳过（内容指纹，不靠 mtime）
if ( cd "$pdir/demo" && "$ZXCW" cbuild > "$TPFX.cli.cb" 2>&1 ) \
   && grep -q "0 C unit(s) recompiled" "$TPFX.cli.cb"; then
    printf "  ok   cbuild 内容指纹增量跳过\n"
else
    printf "  FAIL cbuild 增量失效\n"; cat "$TPFX.cli.cb" 2>/dev/null; fail=1
fi

rm -rf "$pdir" 2>/dev/null || true

# 2.55 错误码目录：诊断里出现的每个码都必须查得到。
# 三处码集要完全一致——src/zxc.c 的 g_rules（真源）、src/errors.c 的文案
# （`zxc explain` 念它）、docs/errors.md（完整参考）。漏一处就是"用户拿着码
# 搜不到东西"，而码一旦发布又不能改，只能靠这里挡。
printf "\n== 错误码目录（zxc errors / explain / docs/errors.md 三方一致）\n"
# 真源 = 编译器里实际会发出的码；文案表必须与它**相等**（多一个少一个都算漂移）
codes=$(grep -ohE '"E[0-9]{4}"' src/zxc.c src/sema.c src/parse.c src/lex.c src/backend.c | tr -d '"' | sort -u)
codes_doc=$(grep -ohE '"E[0-9]{4}"' src/errors.c | tr -d '"' | sort -u)
ncodes=$(printf "%s\n" "$codes" | grep -c '^E')
$ZXC errors > "$TPFX.cli.codes" 2>&1 || true
miss_list=""
for c in $codes; do
    grep -q "$c" "$TPFX.cli.codes" || miss_list="$miss_list $c"
done
if [ "$codes" = "$codes_doc" ]; then
    printf "  ok   errors.c 的码集与编译器一致（%s 个）\n" "$ncodes"
else
    printf "  FAIL errors.c 与编译器码集不一致\n"
    printf "       只在编译器里:%s\n" "$(printf "%s\n" "$codes" | grep -vxF "$codes_doc" | tr "\n" " ")"
    printf "       只在 errors.c:%s\n" "$(printf "%s\n" "$codes_doc" | grep -vxF "$codes" | tr "\n" " ")"
    fail=1
fi
if [ -z "$miss_list" ]; then
    printf "  ok   zxc errors 列出全部 %s 个码\n" "$ncodes"
else
    printf "  FAIL zxc errors 漏了:%s\n" "$miss_list"; fail=1
fi
bad_explain=""
for c in $codes; do
    $ZXC explain "$c" > /dev/null 2>&1 || bad_explain="$bad_explain $c"
done
if [ -z "$bad_explain" ]; then
    printf "  ok   zxc explain 认得全部 %s 个码\n" "$ncodes"
else
    printf "  FAIL zxc explain 不认:%s\n" "$bad_explain"; fail=1
fi
if $ZXC explain E9999 > /dev/null 2>&1; then
    printf "  FAIL explain 对不存在的码返回了成功\n"; fail=1
else
    printf "  ok   explain 对未知码返回非零\n"
fi
doc_miss=""
anchor_miss=""
for c in $codes; do
    grep -q "$c" docs/errors.md || doc_miss="$doc_miss $c"
    # 锚点：lsp 的 codeDescription 与 `zxc explain` 都靠 `#e0304` 直接跳转
    lc=$(printf '%s' "$c" | tr 'A-Z' 'a-z')
    grep -q "id=\"$lc\"" docs/errors.md || anchor_miss="$anchor_miss $c"
done
if [ -z "$doc_miss" ]; then
    printf "  ok   docs/errors.md 覆盖全部 %s 个码\n" "$ncodes"
else
    printf "  FAIL docs/errors.md 缺:%s\n" "$doc_miss"; fail=1
fi
if [ -z "$anchor_miss" ]; then
    printf "  ok   docs/errors.md 每个码都有锚点\n"
else
    printf "  FAIL docs/errors.md 缺锚点:%s\n" "$anchor_miss"; fail=1
fi
# 文档地址在 C（zxc explain 输出）和 Zig（lsp codeDescription）各存一份，
# 只能靠这里挡住漂移：两处字面量必须一致，否则编辑器里的链接指向旧位置。
url_c=$(grep -oE 'https://[^"]*docs/errors\.md' src/errors.c | head -1)
url_zig=$(grep -oE 'https://[^"]*docs/errors\.md' tools/zxls.zig | head -1)
if [ -n "$url_c" ] && [ "$url_c" = "$url_zig" ]; then
    printf "  ok   errors.c 与 zxls.zig 的文档地址一致\n"
else
    printf "  FAIL 文档地址漂移\n        errors.c:%s\n        zxls.zig:%s\n" "$url_c" "$url_zig"; fail=1
fi
# 兜底修法：诊断里没有具体建议时，help 行要换成该码的通用修法
printf 'const main = fn () void {\n    let y: i32 = "abc";\n};\n' > "$TPFX.fix.zx"
$ZXC "$TPFX.fix.zx" --check > "$TPFX.fix.log" 2>&1 || true
if grep -q "E0301" "$TPFX.fix.log" && grep -q "help: .*@intCast" "$TPFX.fix.log"; then
    printf "  ok   无具体建议时诊断带通用修法\n"
else
    printf "  FAIL 诊断没带通用修法\n"; cat "$TPFX.fix.log"; fail=1
fi
if $ZXC explain E0304 2>&1 | grep -q "#e0304"; then
    printf "  ok   zxc explain 给出文档链接\n"
else
    printf "  FAIL zxc explain 没给文档链接\n"; fail=1
fi


# 2.6 LSP 冒烟：语言服务器的模块解析必须和编译器**同一条链**（vendor/ +
# zx.json 的 deps 都要认）。这条挡的是"能编过、IDE 里补不出来"的偏差：
# 两侧各写一份解析链，缺一个目录就是一个点了没反应的句点。
# zxls 编进临时目录并把 zxc.exe 拷到它旁边——zxls 按兄弟目录找 zxc，
# 这样格式化冒烟才能打到真 fmt，而不是空响应假通过。
printf "\n== LSP 冒烟（tools/lsptest.py：补全/跳转 + vendor/ 与 deps）\n"
if [ -x "$ZYG" ] && command -v python >/dev/null 2>&1; then
    mkdir -p "$TPFX.lspbin"
    cp "$ZXC" "$TPFX.lspbin/zxc.exe" 2>/dev/null || true
    # 诊断检查要起 zxc --check，zxc 又要同目录的 zxfe 解析——少一个就
    # 静默没诊断（本文件 2.6 节的教训：模块解析必须和编译器同一条链）。
    cp bin/zxfe.exe "$TPFX.lspbin/zxfe.exe" 2>/dev/null || true
    if $ZYG build-exe tools/zxls.zig -O ReleaseSafe -femit-bin="$TPFX.lspbin/zxls.exe" > "$TPFX.zxls.log" 2>&1; then
        if ZXLS_BIN="$TPFX.lspbin/zxls.exe" python tools/lsptest.py > "$TPFX.lsp.log" 2>&1; then
            printf "  ok   %s 项全过（含 vendor/ 与 deps 模块）\n" \
                "$(grep -c '^ok' "$TPFX.lsp.log" | tr -d ' ')"
        else
            printf "  FAIL LSP 冒烟\n"; tail -20 "$TPFX.lsp.log"; fail=1
        fi
    else
        printf "  FAIL zxls 编译失败\n"; tail -10 "$TPFX.zxls.log"; fail=1
    fi
else
    printf "  skip（未找到 .toolchain/zig.exe 或 python）\n"
fi

# 2. 增量编译：每个模块一个 .o，没改动的模块不重编
printf "\n== 增量编译 (per-module .o)\n"
ib="$TPFX.inc"
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

# 用例会在仓库根留下自己的临时文件：io_fmt.zx 写 io_fmt.tmp，stdfs.zx 建
# fs_t_dir / fs_t_dir2。跑完统一带走——测试不该往工作区里堆东西。
rm -rf io_fmt.tmp fs_t_dir fs_t_dir2 2>/dev/null || true
rm -rf "$TPFX.lspbin" 2>/dev/null || true
rm -f "$TPFX"* 2>/dev/null || true

if [ $fail -ne 0 ]; then
    echo "FAILED"
    exit 1
fi
echo "all checks passed"
