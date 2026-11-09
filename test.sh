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
        src/backend.c src/main.c src/fe.c src/astload.c src/errors.c \
        src/cbind.c src/csrc.c src/clib.c src/fetch.c
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

# 2.52 `zxc include`：C 头 -> Zx 绑定。守的是整条链路（clang AST 解析、
# 类型映射、extern var 文件作用域、生成物能被 Zx 自己检查），
# 不是只测命令有没有打印成功。
printf "\n== zxc include（C 头自动生成绑定）\n"
cat > "$pdir/demo/vendor/cliadd.h" <<'CHDR'
#define CLI_MAGIC 7
/* 十六进制里的 a-f 是数字，不是 f 后缀：生成器以前把它们一并剥掉，
 * `0xFFFF` 变成 `0x`（坏字面量，整个模块解析失败），`0x0002000F`
 * 变成 `0x0002000`（能解析、但值错 15）。这两个形态各留一条。 */
#define CLI_MASK 0xFFFF
#define CLI_HALF 0xFF00
#define CLI_ALL 0xffffffffu
#define CLI_PI 3.14f
#define CLI_NEG -1
typedef struct Pt { int x; int y; } Pt;
enum CliMode { CLI_A = 2, CLI_B };
extern int cli_g;
int cli_sum(int a, int b);
void cli_move(Pt *p, int dx);
CHDR
if ( cd "$pdir/demo" && "$ZXCW" include cliadd > "$TPFX.cli.add" 2>&1 ) \
   && grep -q "pub fn cli_sum(a: i32, b: i32) i32;" "$pdir/demo/vendor/cliadd.zx" \
   && grep -q "pub const CLI_MAGIC: i32 = 7;" "$pdir/demo/vendor/cliadd.zx" \
   && grep -qx "pub const CLI_MASK: i32 = 0xFFFF;" "$pdir/demo/vendor/cliadd.zx" \
   && grep -qx "pub const CLI_HALF: i32 = 0xFF00;" "$pdir/demo/vendor/cliadd.zx" \
   && grep -qx "pub const CLI_ALL: u32 = 0xffffffff;" "$pdir/demo/vendor/cliadd.zx" \
   && grep -qx "pub const CLI_PI: f64 = 3.14;" "$pdir/demo/vendor/cliadd.zx" \
   && grep -qx "pub const CLI_NEG: i32 = -1;" "$pdir/demo/vendor/cliadd.zx" \
   && grep -q "pub extern var cli_g: i32;" "$pdir/demo/vendor/cliadd.zx" \
   && ( cd "$pdir/demo" && "$ZXCW" check vendor/cliadd.zx > "$TPFX.cli.addcheck" 2>&1 ); then
    printf "  ok   zxc include 生成可检查的 C 绑定\n"
else
    printf "  FAIL zxc include\n"
    cat "$TPFX.cli.add" "$TPFX.cli.addcheck" 2>/dev/null
    cat "$pdir/demo/vendor/cliadd.zx" 2>/dev/null
    fail=1
fi
# 2.53 zxc 自己编 C 库：include 自动编译 vendor/*.c，改 .c 后 build 自动
# 增量重编并重链。挡的是两件事：① 手工编库时 sanitizer 档位对不上；
# ② "无改动重建"跳过链接，导致改了 C 源码却还在跑旧二进制。
printf "\n== zxc 编译 C 库（include / build / cbuild）\n"
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
if ( cd "$pdir/demo" && "$ZXCW" include cliadd --force > "$TPFX.cli.cadd" 2>&1 ) \
   && [ -f "$pdir/demo/vendor/libcliadd.a" ] \
   && grep -q '@link("vendor/libcliadd.a")' "$pdir/demo/vendor/cliadd.zx" \
   && ( cd "$pdir/demo" && "$ZXCW" run > "$TPFX.cli.crun" 2>&1 ) \
   && grep -q "sum=5 g=4" "$TPFX.cli.crun"; then
    printf "  ok   include 自动编译 C 源码并链接运行\n"
else
    printf "  FAIL include 自动编译 C 库\n"
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

# 2.54 目录树模式：库原本就是 include/ + src/ 多层结构、还在工程外。
# 守的是「不用复制任何文件」以及自动发现、自动排除测试目录、深层 .c 改动
# 能触发重编重链。库文件一旦被要求摊平到 vendor/ 一层，这个用例就废了。
printf "\n== zxc include <库目录>（多层路径，不复制文件）\n"
TLIB="$pdir/treelib"
mkdir -p "$TLIB/include/treelib" "$TLIB/src/core" "$TLIB/src/util" "$TLIB/tests"
cat > "$TLIB/include/treelib.h" <<'THDR'
#include "treelib/vec.h"
#define TREE_VER 4
extern int tree_g;
int tree_add(int a, int b);
int tree_hash(void);
THDR
cat > "$TLIB/include/treelib/vec.h" <<'VHDR'
typedef struct Vec { int x; int y; } Vec;
VHDR
cat > "$TLIB/src/util/hash.h" <<'HHDR'
int tree_hash_impl(int v);
HHDR
cat > "$TLIB/src/core/calc.c" <<'CSRC'
#include "treelib.h"
#include "hash.h"
int tree_g = 2;
int tree_add(int a, int b) { return a + b; }
int tree_hash(void) { return tree_hash_impl(tree_g); }
CSRC
cat > "$TLIB/src/util/hash.c" <<'HSRC'
#include "hash.h"
int tree_hash_impl(int v) { return v * 3; }
HSRC
printf 'int main(void){ return 1; }\n' > "$TLIB/tests/main.c"

cat > "$pdir/demo/src/main.zx" <<'ZSRC'
const std = module("std");
const t = module("treelib");
const main = fn () i32 {
    std.println("add={} hash={} g={} ver={}",
                t.tree_add(2, 3), t.tree_hash(), t.tree_g, t.TREE_VER);
    return 0;
};
ZSRC
if ( cd "$pdir/demo" && "$ZXCW" include ../treelib > "$TPFX.cli.tree" 2>&1 ) \
   && [ -f "$pdir/demo/vendor/treelib.clib" ] \
   && [ -f "$pdir/demo/vendor/libtreelib.a" ] \
   && [ "$(grep -c '^source = ' "$pdir/demo/vendor/treelib.clib")" = "2" ] \
   && ! grep -q 'tests/main.c' "$pdir/demo/vendor/treelib.clib" \
   && ! [ -f "$pdir/demo/vendor/calc.c" ] \
   && ! grep -q 'tree_hash_impl' "$pdir/demo/vendor/treelib.zx" \
   && ( cd "$pdir/demo" && "$ZXCW" run > "$TPFX.cli.trun" 2>&1 ) \
   && grep -q "add=5 hash=6 g=2 ver=4" "$TPFX.cli.trun"; then
    printf "  ok   目录树自动发现 / 不复制 / 排除 tests / 端到端可调用\n"
else
    printf "  FAIL zxc include <库目录>\n"
    cat "$TPFX.cli.tree" "$TPFX.cli.trun" 2>/dev/null
    cat "$pdir/demo/vendor/treelib.clib" 2>/dev/null
    fail=1
fi
# 深层子目录里的 .c 改了，只跑 run（内部 build）必须自动重编重链
sed -i 's/return v \* 3;/return v * 5;/' "$TLIB/src/util/hash.c"
if ( cd "$pdir/demo" && "$ZXCW" run > "$TPFX.cli.trun2" 2>&1 ) \
   && grep -q "hash=10" "$TPFX.cli.trun2"; then
    printf "  ok   深层 .c 改动触发自动重编重链\n"
else
    printf "  FAIL 深层 .c 改动没有重编\n"; cat "$TPFX.cli.trun2" 2>/dev/null; fail=1
fi

# 2.54b vendor/ 自动接管：库直接丢进 vendor，不跑 include，一条 zxc run 就能跑。
# 守的是：① 绑定必须在解析入口源码之前生成（否则第一次 run 就报找不到模块）；
# ② 目录树和顶层散头两种放法；③ 幂等且绝不覆盖手工改过的 .zx；
# ④ 纯 zx 模块目录不误判、同名占位可 opt-out、zxc sync 幂等。
printf "\n== vendor/ 自动接管（免 zxc include，直接 run）\n"
AP="$pdir/auto/autodemo"
mkdir -p "$pdir/auto"
if ( cd "$pdir/auto" && "$ZXCW" init autodemo > "$TPFX.cli.ainit" 2>&1 ); then
    mkdir -p "$AP/vendor/geolib/include" "$AP/vendor/geolib/src/core" "$AP/vendor/geolib/src/util"
    printf '#define GEO_VER 3\nextern int geo_g;\nint geo_add(int a, int b);\nint geo_twice(void);\n' \
        > "$AP/vendor/geolib/include/geolib.h"
    printf 'int geo_twice_impl(int v);\n' > "$AP/vendor/geolib/src/util/twice.h"
    printf '#include "geolib.h"\n#include "twice.h"\nint geo_g = 7;\nint geo_add(int a, int b) { return a + b; }\nint geo_twice(void) { return geo_twice_impl(geo_g); }\n' \
        > "$AP/vendor/geolib/src/core/calc.c"
    printf '#include "twice.h"\nint geo_twice_impl(int v) { return v * 2; }\n' \
        > "$AP/vendor/geolib/src/util/twice.c"
    printf 'const std = module("std");\nconst g = module("geolib");\nconst main = fn () i32 {\n    std.println("add={} twice={} ver={}", g.geo_add(2, 3), g.geo_twice(), g.GEO_VER);\n    return 0;\n};\n' \
        > "$AP/src/main.zx"
    if ( cd "$AP" && "$ZXCW" run > "$TPFX.cli.arun" 2>&1 ) \
       && grep -q "auto-added C library vendor/geolib" "$TPFX.cli.arun" \
       && grep -q "add=5 twice=14 ver=3" "$TPFX.cli.arun" \
       && [ -f "$AP/vendor/geolib.clib" ] \
       && [ -f "$AP/vendor/geolib.zx" ] \
       && [ -f "$AP/vendor/libgeolib.a" ]; then
        printf "  ok   丢库进 vendor 后直接 run，自动接管 + 编译 + 链接\n"
    else
        printf "  FAIL vendor 自动接管\n"; cat "$TPFX.cli.arun" 2>/dev/null; fail=1
    fi
    # 第二次 run 不能重复生成，手改过的绑定不能被冲掉
    printf '\n// HAND EDIT -- must survive\n' >> "$AP/vendor/geolib.zx"
    if ( cd "$AP" && "$ZXCW" run > "$TPFX.cli.arun2" 2>&1 ) \
       && ! grep -q "auto-added" "$TPFX.cli.arun2" \
       && grep -q "HAND EDIT" "$AP/vendor/geolib.zx"; then
        printf "  ok   幂等，且不覆盖手工编辑过的绑定\n"
    else
        printf "  FAIL 重复生成或覆盖了手改\n"; cat "$TPFX.cli.arun2" 2>/dev/null; fail=1
    fi
    # 顶层散头 + 散 .c：更简单的放法同样免 include
    printf '#define SMALL_K 9\nint small_mul(int a, int b);\n' > "$AP/vendor/small.h"
    printf '#include "small.h"\nint small_mul(int a, int b) { return a * b; }\n' > "$AP/vendor/small.c"
    printf 'const std = module("std");\nconst s = module("small");\nconst main = fn () i32 {\n    std.println("mul={} k={}", s.small_mul(4, 5), s.SMALL_K);\n    return 0;\n};\n' \
        > "$AP/src/main.zx"
    if ( cd "$AP" && "$ZXCW" run > "$TPFX.cli.arun3" 2>&1 ) \
       && grep -q "auto-added C header vendor/small.h" "$TPFX.cli.arun3" \
       && grep -q "mul=20 k=9" "$TPFX.cli.arun3"; then
        printf "  ok   顶层散头 + 散 .c 也能免 include 直接跑\n"
    else
        printf "  FAIL 散头自动接管\n"; cat "$TPFX.cli.arun3" 2>/dev/null; fail=1
    fi
    # 纯 zx 模块目录、同名占位 opt-out、sync 幂等
    mkdir -p "$AP/vendor/myzx" "$AP/vendor/optout/include"
    printf 'pub const hi = fn () void {};\n' > "$AP/vendor/myzx/util.zx"
    printf 'int nope(void);\n' > "$AP/vendor/optout/include/optout.h"
    printf '// not a binding\n' > "$AP/vendor/optout.zx"
    if ( cd "$AP" && "$ZXCW" sync > "$TPFX.cli.asy" 2>&1 ) \
       && grep -q "up to date" "$TPFX.cli.asy" \
       && [ ! -f "$AP/vendor/myzx.zx" ] \
       && [ ! -f "$AP/vendor/optout.clib" ]; then
        printf "  ok   纯 zx 目录不误判 / 占位可 opt-out / sync 幂等\n"
    else
        printf "  FAIL 误判或 opt-out 失效\n"; cat "$TPFX.cli.asy" 2>/dev/null; fail=1
    fi
else
    printf "  FAIL zxc init（自动接管用例）\n"; cat "$TPFX.cli.ainit" 2>/dev/null; fail=1
fi

# 2.54c zxc add / fetch：清单极简（URL 数组）+ 锁承担全部安全锚点。
# 守的是：① zx.json 只允许 name/version/main/out/deps/vendor，vendor 必须是
# 字符串数组，不许再冒出 ref/sha256/subdir 之类的参数；② 旧 CLI 参数一律删除，
# 旧写法直接报错；③ 单头 / 归档 / git 三种源零参数落地并被自动接管；
# ④ fetch 幂等、篡改检测、缺库恢复、URL 变更拒绝、锁内 sha256/commit 复现；
# ⑤ 源站下线 run 仍成功；⑥ include 的能力完整保留。
printf "\n== zxc add/fetch（zx.json 只记 URL + zx.lock 全锚点，run 不联网）\n"
FDIR="$pdir/fetch"
FSRV="$FDIR/srv"
FDEMO="$FDIR/demo"
if mkdir -p "$FSRV/flib-1.0/include" "$FSRV/flib-1.0/src" "$FSRV/fgit.git/include" "$FSRV/fgit.git/src" \
   && printf '#define FMINI_K 42\n' > "$FSRV/fmini.h" \
   && printf '#define FLIB_V 8\nint flib_add(int a, int b);\n' > "$FSRV/flib-1.0/include/flib.h" \
   && printf '#include "flib.h"\nint flib_add(int a, int b) { return a + b; }\n' > "$FSRV/flib-1.0/src/add.c" \
   && ( cd "$FSRV" && tar -czf flib.tar.gz flib-1.0 ) \
   && printf '#define FGIT_K 5\nint fgit_mul(int a, int b);\n' > "$FSRV/fgit.git/include/fgit.h" \
   && printf '#include "fgit.h"\nint fgit_mul(int a, int b) { return a * b; }\n' > "$FSRV/fgit.git/src/mul.c" \
   && ( cd "$FSRV/fgit.git" && git init -q && git config core.autocrlf false && git config user.email t@t.t && git config user.name t \
        && git add . && git commit -q -m init ) \
   && ( cd "$FDIR" && "$ZXCW" init demo > "$TPFX.cli.finit" 2>&1 ); then
    # 三种源，全部零参数：单头 / tar.gz（自动下钻顶层目录）/ .git 结尾的仓库
    if ( cd "$FDEMO" && ZX_ALLOW_LOCAL_FETCH=1 "$ZXCW" add "file:///$FSRV/fmini.h" > "$TPFX.cli.fa1" 2>&1 ) \
       && ( cd "$FDEMO" && ZX_ALLOW_LOCAL_FETCH=1 "$ZXCW" add "file:///$FSRV/flib.tar.gz" > "$TPFX.cli.fa2" 2>&1 ) \
       && ( cd "$FDEMO" && ZX_ALLOW_LOCAL_FETCH=1 "$ZXCW" add "file:///$FSRV/fgit.git" > "$TPFX.cli.fa3" 2>&1 ) \
       && [ -f "$FDEMO/vendor/fmini.h" ] && [ -f "$FDEMO/vendor/fmini.zx" ] \
       && [ -f "$FDEMO/vendor/flib.clib" ] && [ -f "$FDEMO/vendor/fgit.zx" ] \
       && python -c "import json,sys; d=json.load(open(sys.argv[1])); \
assert set(d) <= set(['name','version','main','out','deps','vendor']), d; \
assert isinstance(d['vendor'], list) and len(d['vendor'])==3, d['vendor']; \
assert all(isinstance(x,str) for x in d['vendor']), d['vendor']; \
assert d['name']=='demo'" "$FDEMO/zx.json" \
       && grep -q "resolved = [0-9a-f]\{40\}" "$FDEMO/zx.lock" \
       && grep -q "tree = [0-9a-f]\{64\}" "$FDEMO/zx.lock" \
       && grep -q "sha256 = [0-9a-f]\{64\}" "$FDEMO/zx.lock"; then
        printf "  ok   零参数 add 三种源：清单只有 URL 数组，锚点自动进 lock\n"
    else
        printf "  FAIL zxc add 三层产物 / 清单格式\n"
        for f in "$TPFX.cli.fa1" "$TPFX.cli.fa2" "$TPFX.cli.fa3"; do cat "$f" 2>/dev/null; done
        cat "$FDEMO/zx.json" 2>/dev/null; fail=1
    fi
    # 旧参数不再存在：给了就明确报错，防止有人把参数悄悄加回来
    if ( cd "$FDEMO" && "$ZXCW" add "file:///$FSRV/fmini.h" --name foo > "$TPFX.cli.fold" 2>&1 ) \
       || ! grep -q "unknown option" "$TPFX.cli.fold"; then
        printf "  FAIL 已删除的 --name 竟被接受\n"; cat "$TPFX.cli.fold" 2>/dev/null; fail=1
    else
        printf "  ok   旧参数（--name/--ref/--sha256/--subdir/--git）已彻底移除\n"
    fi
    # 旧版「每库一个对象」的 vendor 写法按新规矩拒绝，不做兼容
    cp "$FDEMO/zx.json" "$TPFX.cli.fjson.bak"
    printf '{\n  "vendor": { "x": { "url": "https://example.com/x.h" } }\n}\n' > "$FDEMO/zx.json"
    if ( cd "$FDEMO" && "$ZXCW" fetch > "$TPFX.cli.flegacy" 2>&1 ) \
       || ! grep -q "must be a URL list" "$TPFX.cli.flegacy"; then
        printf "  FAIL 旧版 vendor 对象写法未被拒绝\n"; cat "$TPFX.cli.flegacy" 2>/dev/null; fail=1
    else
        printf "  ok   旧版 vendor 对象写法按新规矩报错（无兼容层）\n"
    fi
    cp "$TPFX.cli.fjson.bak" "$FDEMO/zx.json"
    printf 'const std = module("std");\nconst a = module("flib");\nconst b = module("fgit");\nconst c = module("fmini");\nconst main = fn () i32 {\n    std.println("add={} mul={} k={} mini={}", a.flib_add(3, 4), b.fgit_mul(2, 6), b.FGIT_K, c.FMINI_K);\n    return 0;\n};\n' > "$FDEMO/src/main.zx"
    if ( cd "$FDEMO" && "$ZXCW" run > "$TPFX.cli.frun" 2>&1 ) \
       && grep -q "add=7 mul=12 k=5 mini=42" "$TPFX.cli.frun"; then
        printf "  ok   拉下来的三种库都能编译链接跑\n"
    else
        printf "  FAIL add 后的 zxc run\n"; cat "$TPFX.cli.frun" 2>/dev/null; fail=1
    fi
    # 源站整个下线：run 必须照常成功，且输出不带任何下载字样
    mv "$FSRV" "$FSRV.off"
    if ( cd "$FDEMO" && "$ZXCW" run > "$TPFX.cli.frun2" 2>&1 ) \
       && grep -q "add=7 mul=12" "$TPFX.cli.frun2" \
       && ! grep -qi "download\|curl\|clone" "$TPFX.cli.frun2"; then
        printf "  ok   源站下线后 run 仍成功（构建不联网）\n"
    else
        printf "  FAIL run 竟然依赖源站\n"; cat "$TPFX.cli.frun2" 2>/dev/null; fail=1
    fi
    mv "$FSRV.off" "$FSRV"
    # fetch 纯本地：不开本地源开关也能全绿，说明没碰 URL
    if ( cd "$FDEMO" && "$ZXCW" fetch > "$TPFX.cli.ff1" 2>&1 ) \
       && grep -q "3 ok, 0 problem" "$TPFX.cli.ff1"; then
        printf "  ok   fetch 幂等，纯本地校验不碰 URL\n"
    else
        printf "  FAIL fetch 幂等\n"; cat "$TPFX.cli.ff1" 2>/dev/null; fail=1
    fi
    # fetch 的单条过滤：name 和 URL 都认（用户不必记「name 到底是什么」）
    if ( cd "$FDEMO" && "$ZXCW" fetch "file:///$FSRV/fmini.h" > "$TPFX.cli.ffurl" 2>&1 ) \
       && grep -q "1 ok, 0 problem" "$TPFX.cli.ffurl"; then
        printf "  ok   fetch 既认库名也认 URL\n"
    else
        printf "  FAIL fetch 按 URL 定位\n"; cat "$TPFX.cli.ffurl" 2>/dev/null; fail=1
    fi
    printf '// tampered\n' >> "$FDEMO/vendor/fmini.h"
    if ( cd "$FDEMO" && "$ZXCW" fetch fmini > "$TPFX.cli.ftamp" 2>&1 ) \
       || ! grep -q "content differs" "$TPFX.cli.ftamp"; then
        printf "  FAIL 篡改未被检出\n"; cat "$TPFX.cli.ftamp" 2>/dev/null; fail=1
    else
        printf "  ok   本地篡改被 tree hash 检出且不静默覆盖\n"
    fi
    rm -rf "$FDEMO/vendor/flib"
    if ( cd "$FDEMO" && ZX_ALLOW_LOCAL_FETCH=1 "$ZXCW" fetch flib > "$TPFX.cli.fres" 2>&1 ) \
       && [ -f "$FDEMO/vendor/flib/src/add.c" ] \
       && grep -q "restored flib" "$TPFX.cli.fres"; then
        printf "  ok   缺库按锁恢复\n"
    else
        printf "  FAIL 缺库恢复\n"; cat "$TPFX.cli.fres" 2>/dev/null; fail=1
    fi
    # 锁里的 sha256 被改：重拉必须复现锁，不一致就拒绝（安全锚点在锁里）
    sed -i "s/^sha256 = [0-9a-f]\{64\}/sha256 = 0000000000000000000000000000000000000000000000000000000000000000/" "$FDEMO/zx.lock"
    if ( cd "$FDEMO" && ZX_ALLOW_LOCAL_FETCH=1 "$ZXCW" fetch fmini --force > "$TPFX.cli.fsha" 2>&1 ) \
       || ! grep -qE "sha256 mismatch|sha256 differs" "$TPFX.cli.fsha"; then
        printf "  FAIL 锁内 sha256 未被复现校验\n"; cat "$TPFX.cli.fsha" 2>/dev/null; fail=1
    else
        printf "  ok   锁内 sha256 复现校验（参数沉到 lock 仍安全）\n"
    fi
    # 上游 HEAD 前进：锁里的 commit 必须拒绝，且本地库一个字节都不动
    FBEFORE=$(sha256sum "$FDEMO/vendor/fgit/src/mul.c" | cut -d' ' -f1)
    ( cd "$FSRV/fgit.git" && git commit -q --allow-empty -m evil )
    if ( cd "$FDEMO" && ZX_ALLOW_LOCAL_FETCH=1 "$ZXCW" fetch fgit --force > "$TPFX.cli.fevil" 2>&1 ) \
       || ! grep -q "lock mismatch: git commit" "$TPFX.cli.fevil" \
       || [ "$FBEFORE" != "$(sha256sum "$FDEMO/vendor/fgit/src/mul.c" | cut -d' ' -f1)" ]; then
        printf "  FAIL 上游 HEAD 前进未被锁挡住\n"; cat "$TPFX.cli.fevil" 2>/dev/null; fail=1
    else
        printf "  ok   上游 HEAD 前进被 zx.lock 的 commit 拒绝\n"
    fi
    # 声明里的 URL 被改：换源必须显式重新锁定
    sed -i "s#file:///.*/fmini.h#https://example.com/fmini.h#" "$FDEMO/zx.json"
    if ( cd "$FDEMO" && "$ZXCW" fetch fmini > "$TPFX.cli.fswap" 2>&1 ) \
       || ! grep -q "URL in zx.json changed" "$TPFX.cli.fswap"; then
        printf "  FAIL 换源未被拒绝\n"; cat "$TPFX.cli.fswap" 2>/dev/null; fail=1
    else
        printf "  ok   声明 URL 变更被拒绝（要求 --force 重新锁定）\n"
    fi
    # 声明里凭空多一个新源：fetch 只复现锁，绝不替人下载并写锁
    python -c "import json,sys; p=sys.argv[1]; d=json.load(open(p)); d['vendor'].append('https://example.com/newlib.h'); open(p,'w').write(json.dumps(d,indent=2))" "$FDEMO/zx.json"
    if ( cd "$FDEMO" && "$ZXCW" fetch newlib > "$TPFX.cli.fnew" 2>&1 ) \
       || ! grep -q "not in zx.lock" "$TPFX.cli.fnew"; then
        printf "  FAIL fetch 竟引入未锁定的新源\n"; cat "$TPFX.cli.fnew" 2>/dev/null; fail=1
    else
        printf "  ok   fetch 从不引入新源（新源必须显式 zxc add）\n"
    fi
    # 安全边界 + 命令改名回归
    if ( cd "$FDEMO" && "$ZXCW" add http://example.com/x.h > "$TPFX.cli.fsch" 2>&1 ) \
       || ! grep -q "only https" "$TPFX.cli.fsch"; then
        printf "  FAIL 非 https 源未拒绝\n"; cat "$TPFX.cli.fsch" 2>/dev/null; fail=1
    else
        printf "  ok   非 https 源被拒绝\n"
    fi
    if ( cd "$FDEMO" && "$ZXCW" include --list > "$TPFX.cli.finc" 2>&1 ) \
       && grep -q "built-in C library recipes" "$TPFX.cli.finc" \
       && ! ( cd "$FDEMO" && "$ZXCW" add ./vendor/fmini.h > "$TPFX.cli.flocal" 2>&1 ) \
       && grep -q "zxc include" "$TPFX.cli.flocal"; then
        printf "  ok   include 保留原能力 / add 对本地路径指路 include\n"
    else
        printf "  FAIL include 回归或本地路径提示\n"
        cat "$TPFX.cli.finc" "$TPFX.cli.flocal" 2>/dev/null; fail=1
    fi
else
    printf "  FAIL add/fetch 用例的 fixture 准备\n"; cat "$TPFX.cli.finit" 2>/dev/null; fail=1
fi

# 2.54d LSP 诊断的模块搜索路径：单文件分派 `zxc <file> --check` 必须认识
# vendor/ 与 deps。以前只有 build/run（do_build）和 `zxc check` 子命令设了
# 搜索路径，LSP 偏偏 spawn 的是单文件分派，于是「能编过、编辑器却假报
# cannot open module '.../src/<name>'」。守的是 src/main.c 里
# setup_search_paths_for_file() 那次调用（KEEP 标记）——只跑 build / check
# 子命令都发现不了，必须显式打这条链。
printf "\n== 单文件 --check 的模块搜索路径（LSP 诊断假报错守门）\n"
MDIR="$pdir/lspmod"
if mkdir -p "$MDIR/src" "$MDIR/vendor" "$MDIR/shared" \
   && printf '{"name":"lspmod","main":"src/main.zx","deps":["shared"]}\n' > "$MDIR/zx.json" \
   && printf 'pub const V: i32 = 7;\n' > "$MDIR/vendor/vlib.zx" \
   && printf 'pub const S: i32 = 5;\n' > "$MDIR/shared/slib.zx" \
   && printf 'const std = module("std");\nconst v = module("vlib");\nconst s = module("slib");\nconst main = fn () i32 { std.println("{}", v.V + s.S); return 0; };\n' > "$MDIR/src/main.zx"; then
    # ① cwd = 工程根、相对路径：LSP 的典型形态
    if ( cd "$MDIR" && "$ZXCW" src/main.zx --check > "$TPFX.md1" 2>&1 ) \
       && ! grep -q "cannot open module" "$TPFX.md1"; then
        printf "  ok   工程根相对路径：vendor/ 与 deps 都解析得到\n"
    else
        printf "  FAIL 单文件 --check 找不到 vendor/deps 模块\n"; cat "$TPFX.md1"; fail=1
    fi
    # ② 影子文件：LSP 实际写的是 src/.zxls-<hash>.zx
    cp "$MDIR/src/main.zx" "$MDIR/src/.zxls-deadbeef.zx"
    if ( cd "$MDIR" && "$ZXCW" src/.zxls-deadbeef.zx --check > "$TPFX.md2" 2>&1 ) \
       && ! grep -q "cannot open module" "$TPFX.md2"; then
        printf "  ok   LSP 影子文件形态同样解析得到\n"
    else
        printf "  FAIL LSP 影子文件仍假报模块找不到\n"; cat "$TPFX.md2"; fail=1
    fi
    # ③ cwd 不在工程根 + 绝对路径：按源文件向上找 zx.json 后拼绝对路径
    if ( cd "$pdir" && "$ZXCW" "$MDIR/src/main.zx" --check > "$TPFX.md3" 2>&1 ) \
       && ! grep -q "cannot open module" "$TPFX.md3"; then
        printf "  ok   cwd 不在工程根时按源文件向上找工程根\n"
    else
        printf "  FAIL 异目录 cwd 下仍假报模块找不到\n"; cat "$TPFX.md3"; fail=1
    fi
    # ④ 负例：真的写错模块名仍要报错，不许把诊断吞掉。
    # 注意顶层 const 是惰性求值的：q 不被使用时 module(...) 压根不加载，
    # 所以必须真的引用 q.X 才会走进模块解析链。
    printf 'const q = module("nope_missing");\nconst main = fn () i32 { return q.X; };\n' > "$MDIR/src/bad.zx"
    # 注意：报错时 zxc 返回非零，这里不能拿退出码做 && 的前提，
    # 否则"正确报了错"反而会被判成失败——只认诊断文本。
    ( cd "$MDIR" && "$ZXCW" src/bad.zx --check > "$TPFX.md4" 2>&1 ) || true
    if grep -q "cannot open module" "$TPFX.md4"; then
        printf "  ok   真缺模块仍照常报错（没把诊断吞掉）\n"
    else
        printf "  FAIL 真缺模块竟不报错\n"; cat "$TPFX.md4"; fail=1
    fi
else
    printf "  FAIL 单文件搜索路径用例的 fixture 准备\n"; fail=1
fi

rm -rf "$pdir" 2>/dev/null || true

# 2.55 错误码目录：诊断里出现的每个码都必须查得到。
# 两处码集要完全一致——src/zxc.c 的 g_rules（真源）与 src/errors.c 的文案
# （`zxc errors` / `zxc explain` 都念它）。漏一处就是"用户拿着码搜不到东西"，
# 而码一旦发布又不能改，只能靠这里挡。
printf "\n== 错误码目录（zxc errors / explain 与编译器码集一致）\n"
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
# 兜底修法：诊断里没有具体建议时，help 行要换成该码的通用修法
printf 'const main = fn () void {\n    let y: i32 = "abc";\n};\n' > "$TPFX.fix.zx"
$ZXC "$TPFX.fix.zx" --check > "$TPFX.fix.log" 2>&1 || true
if grep -q "E0301" "$TPFX.fix.log" && grep -q "help: .*@intCast" "$TPFX.fix.log"; then
    printf "  ok   无具体建议时诊断带通用修法\n"
else
    printf "  FAIL 诊断没带通用修法\n"; cat "$TPFX.fix.log"; fail=1
fi


# 2.6 LSP 冒烟：语言服务器的模块解析必须和编译器**同一条链**（vendor/ +
# zx.json 的 deps 都要认）。这条挡的是"能编过、IDE 里补不出来"的偏差：
# 两侧各写一份解析链，缺一个目录就是一个点了没反应的句点。
# zxls 编进临时目录并把 zxc.exe 拷到它旁边——zxls 按兄弟目录找 zxc，
# 这样格式化冒烟才能打到真 fmt，而不是空响应假通过。
# 2.56 名字只由显式绑定产生（用户 2026-11-08 否掉「裸模块名即命名空间」）。
#     隐式名一旦和别名/再导出撞名，`pub const glfw3 = glfw3;` 右边会被
#     自己的绑定吃掉，文件一多必乱。守门钉死三态：① 未绑定裸名照常报
#     unknown identifier；② 显式绑定 / 缩写别名 / pub 再导出跑得通；
#     ③ 磁盘上有同名模块也不改变普通绑定的含义。
printf "\n== 模块名必须显式绑定（别名可缩写 / 可再导出）\n"
BP="$TPFX.modbind"
if mkdir -p "$BP/src" \
   && printf '{"name":"modbind","version":"0.1.0","main":"src/main.zx","out":"modbind"}\n' > "$BP/zx.json" \
   && printf 'pub const add = fn (a: i32, b: i32) i32 { return a + b; };\n' > "$BP/src/util.zx" \
   && printf 'pub const math = module("util");\n' > "$BP/src/api.zx" \
   && printf 'const std = module("std");\nconst g = module("util");\nconst api = module("api");\n\npub const main = fn () !i32 {\n    std.println("{} {}", g.add(1, 2), api.math.add(3, 4));\n    return 0;\n};\n' > "$BP/src/main.zx" \
   && ( cd "$BP" && "$ZXCW" run > "$TPFX.modbindrun" 2>&1 ) \
   && grep -q "^3 7$" "$TPFX.modbindrun"; then
    printf "  ok   显式绑定 / 缩写别名 / pub 再导出跑通\n"
else
    printf "  FAIL 显式绑定链路\n"; cat "$TPFX.modbindrun" 2>/dev/null; fail=1
fi
# 负例：没绑定就点模块名，必须照常报 unknown identifier
printf 'pub const main = fn () !i32 {\n    return util.add(1, 2);\n};\n' > "$BP/src/main.zx"
( cd "$BP" && "$ZXCW" src/main.zx --check > "$TPFX.modbindbad" 2>&1 ) || true
if grep -q "unknown identifier 'util'" "$TPFX.modbindbad"; then
    printf "  ok   未绑定裸名仍报 unknown identifier（无隐式命名空间）\n"
else
    printf "  FAIL 裸名被偷偷当成命名空间\n"; cat "$TPFX.modbindbad"; fail=1
fi
# 同名模块文件不改变普通绑定的含义
printf 'pub const main = fn () !i32 {\n    const util = 5;\n    return util + 1;\n};\n' > "$BP/src/main.zx"
if ( cd "$BP" && "$ZXCW" src/main.zx --check > "$TPFX.modbindshadow" 2>&1 ); then
    printf "  ok   同名模块文件不抢普通绑定\n"
else
    printf "  FAIL 模块文件干扰了局部绑定\n"; cat "$TPFX.modbindshadow"; fail=1
fi

# 2.57 实参个数不匹配只报 E0303，不得假报形参 unknown identifier。
#     用户 2026-11-08 截图踩中：ct_call 克隆执行函数体时只绑
#     min(形参,实参) 个形参，push() 零参调用把 push 体里的 a、b 全部
#     假报成 unknown identifier（还带 "did you mean 调用者的 h" 的
#     误导建议）。KEEP 标记在 src/sema.c ct_call。
printf "\n== 实参个数不匹配不假报形参 unknown identifier\n"
BP="$TPFX.arity"
if mkdir -p "$BP/src" \
   && printf '{"name":"arity","version":"0.1.0","main":"src/main.zx","out":"arity"}\n' > "$BP/zx.json" \
   && printf 'const push = fn (a: i32, b: i32) i32 {\n    return a + b;\n};\n\nconst quad = fn (x: f32, y: f32, w: f32, h: f32) void {\n    _ = push();\n};\n\npub const main = fn () i32 {\n    quad(1.0, 2.0, 3.0, 4.0);\n    return 0;\n};\n' > "$BP/src/main.zx" \
   && ( cd "$BP" && "$ZXCW" src/main.zx --check > "$TPFX.aritybad" 2>&1 || true ) \
   && grep -q "E0303" "$TPFX.aritybad" \
   && ! grep -q "unknown identifier 'a'" "$TPFX.aritybad" \
   && ! grep -q "unknown identifier 'b'" "$TPFX.aritybad" \
   && [ "$(grep -c 'error\[' "$TPFX.aritybad")" -eq 1 ]; then
    printf "  ok   实参个数不匹配只报 E0303 一条\n"
else
    printf "  FAIL 形参被假报 unknown identifier（ct_call 洞回退）\n"; cat "$TPFX.aritybad" 2>/dev/null; fail=1
fi
# 对照：实参个数正确时编译期折叠照常工作（常量传播不能被挡）
if printf 'const add = fn (a: i32, b: i32) i32 {\n    return a + b;\n};\n\npub const main = fn () i32 {\n    return add(1, 2);\n};\n' > "$BP/src/main.zx" \
   && ( cd "$BP" && "$ZXCW" src/main.zx --check > "$TPFX.arityok" 2>&1 ); then
    printf "  ok   实参个数正确时折叠不受影响\n"
else
    printf "  FAIL 正常调用被误伤\n"; cat "$TPFX.arityok"; fail=1
fi

# 2.58 子模块解析失败：导入点那条诊断必须说出原因（子模块首条错的行列
#     与消息），并且不再刷 "`mod` (module 'main') has no member"。用户
#     2026-11-09 踩中：vendor/glfw.zx 里 `0xFFFF` 被生成成 `0x`，编辑器
#     只显示 "module 'vendor/glfw.zx' failed to parse"，而它带的源码片段
#     还是子模块第 2 行的注释——真凶（751 行的坏字面量）完全看不见。
printf "\n== 子模块解析失败：导入点带上原因，不刷误导成员错误\n"
BP="$TPFX.mfail"
if mkdir -p "$BP/src" \
   && printf '{"name":"mfail","version":"0.1.0","main":"src/main.zx","out":"mfail"}\n' > "$BP/zx.json" \
   && printf 'pub const X: i32 = 0x;\n' > "$BP/src/util.zx" \
   && printf 'const u = module("util");\n\npub const main = fn () i32 {\n    return u.nope();\n};\n' > "$BP/src/main.zx" \
   && ( cd "$BP" && "$ZXCW" src/main.zx --check > "$TPFX.mfail1" 2>&1 || true ) \
   && grep -q "src/main.zx:1:11: error\[E0601\]: module 'src/util.zx' failed to parse: 1:22: hex literal needs digits" "$TPFX.mfail1" \
   && ! grep -q "has no member" "$TPFX.mfail1"; then
    printf "  ok   导入点报出行列与原因，且无误导成员错误\n"
else
    printf "  FAIL 子模块解析失败的父诊断不完整\n"
    cat "$TPFX.mfail1" 2>/dev/null
    fail=1
fi

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

# 2.1 示例程序：demo/snake.zx 编得过，`--sim` 自检全过。
#     snake 用到了 std.io 的句柄写、std.time 的单调钟、Win32 FFI 三样，
#     外加"蛇身不重叠 / 食物不在身上 / 计分等于长度"四条不变式——它绿了，
#     说明这几条链路在这个编译器上仍然通着。控制台画面没法在 CI 里截图，
#     所以自检走 --sim 的 headless 分支。
printf "\n== 示例程序（demo/snake.zx 编译 + --sim 自检）\n"
SNK="$TPFX.snake.exe"
if $ZXC demo/snake.zx -o "$SNK" > "$TPFX.snakebuild" 2>&1; then
    if "$SNK" --sim > "$TPFX.snakesim" 2>&1 && grep -q '^sim ok:' "$TPFX.snakesim"; then
        printf "  ok   %s\n" "$(cat "$TPFX.snakesim")"
    else
        printf "  FAIL snake --sim 自检没过\n"; cat "$TPFX.snakesim"; fail=1
    fi
else
    printf "  FAIL demo/snake.zx 编译失败\n"; cat "$TPFX.snakebuild"; fail=1
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
