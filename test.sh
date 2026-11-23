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
        src/zxc.c src/ty.c src/val.c src/lex.c src/parse.c src/sema*.c \
        src/backend.c src/main.c src/fe.c src/astload.c src/errors.c \
        src/cbind.c src/csrc.c src/clib.c src/fetch.c src/platform.c
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
   && grep -q "list built-in platform targets" "$TPFX.cli.h" \
   && $ZXC build -h 2>&1 | grep -q "zxc build" \
   && $ZXC targets -h 2>&1 | grep -q '"targets"' \
   && $ZXC targets -h 2>&1 | grep -q 'macos-aarch64'; then
    printf "  ok   zxc -h / zxc build -h / zxc targets -h\n"
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
    # init 的 README 必须讲多平台用法，且与 `zxc targets -h` 对得上
    if grep -q '"targets"' "$pdir/demo/README.md" \
       && grep -q 'zxc targets' "$pdir/demo/README.md" \
       && grep -q 'macos-aarch64' "$pdir/demo/README.md" \
       && grep -q 'linux-x86_64' "$pdir/demo/README.md"; then
        printf "  ok   init 的 README 与 zxc targets -h 对齐\n"
    else
        printf "  FAIL init 的 README 缺少/不匹配多平台用法\n"; fail=1
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
    # vendor/ 是模块根：@mod("ext") → vendor/ext.zx（可省 .zx）
    printf 'const std = @mod("std");\n\npub const hi = fn () void {\n    std.println("vendor ok");\n};\n' \
        > "$pdir/demo/vendor/ext.zx"
    printf 'const std = @mod("std");\nconst ext = @mod("ext");\n\nconst main = fn () i32 {\n    ext.hi();\n    return 0;\n};\n' \
        > "$pdir/demo/src/main.zx"
    rm -f "$pdir/demo/src/bad.zx" 2>/dev/null || true
    if ( cd "$pdir/demo" && "$ZXCW" run > "$TPFX.cli.r" 2>&1 ) && grep -q "vendor ok" "$TPFX.cli.r"; then
        printf "  ok   vendor/ 里的模块能被 @mod(\"ext\") 找到\n"
    else
        printf "  FAIL vendor/ 模块解析失败\n"; cat "$TPFX.cli.r"; fail=1
    fi
else
    printf "  FAIL zxc init\n"; cat "$TPFX.cli.init"; fail=1
fi

# 2.51b 尾逗号列表 + fmt 拆行（用户 2026-11-11）：参数/实参/error 集/
# 元组/字面量以 ',' 结尾合法；fmt 见同行 (...) / 字面量组（.{…}、T{…}）
# 尾逗号就拆成一行一个，并递归（18:xx 扩展）；且必须幂等（拆完再 fmt 一字不动）。
printf "\n== 尾逗号列表 + fmt 拆行\n"
mkdir -p "$pdir/tc/src"
printf 'const add = fn (a: i32, b: i32,) i32 {\n    return a + b;\n};\n\nconst Cfg = struct { name: []const u8, val: i32 };\n\nconst main = fn () i32 {\n    _ = add(1, 2,);\n    const e = error{a, b,};\n    const t: {i32, i32,} = {5, 6,};\n    const c = Cfg{ .name = "a", .val = 1, };\n    _ = e;\n    _ = t;\n    _ = c;\n    return 0;\n};\n' > "$pdir/tc/src/main.zx"
if ( cd "$pdir/tc" && "$ZXCW" src/main.zx --check > "$TPFX.tc.chk" 2>&1 ); then
    printf "  ok   尾逗号：参数/实参/error集/元组类型/字面量全解析\n"
else
    printf "  FAIL 尾逗号列表解析失败\n"; cat "$TPFX.tc.chk"; fail=1
fi
# 手工多行区域归一（zig-fmt 口径）：闭括号前有尾逗号的组一行一项树状展开；
# 根组没尾逗号保持单行（内层带尾逗号的 .{…} 照常展开）——**不补逗号**；
# fmt 产物必须可编译（内容保全——曾复现过拆行丢实参的 bug，编译检查就是守门）。
printf 'const pa = fn (v: i32, info: struct{name: []const u8, data: struct{b: i32, i: i32, data: struct{b: i32}}}) i32 { _ = info; return v; };\n\nconst main = fn () i32 {\n    _ = pa(10, .{\n        .name = "weo",\n        .data = .{\n            .b = 1, .i = 0, .data = .{.b = 120},\n        }, } );\n    return 0;\n};\n' > "$pdir/tc/src/ml.zx"
if ( cd "$pdir/tc" && "$ZXCW" fmt src/ml.zx > "$TPFX.tc.mlf" 2>&1 ) \
   && grep -qxF '    _ = pa(10, .{' "$pdir/tc/src/ml.zx" \
   && grep -qxF '        .name = "weo",' "$pdir/tc/src/ml.zx" \
   && grep -qxF '        .data = .{' "$pdir/tc/src/ml.zx" \
   && grep -qxF '            .b = 1,' "$pdir/tc/src/ml.zx" \
   && grep -qxF '            .data = .{ .b = 120 },' "$pdir/tc/src/ml.zx" \
   && grep -qxF '        },' "$pdir/tc/src/ml.zx" \
   && grep -qxF '    });' "$pdir/tc/src/ml.zx" \
   && cp "$pdir/tc/src/ml.zx" "$TPFX.tc.ml1" \
   && ( cd "$pdir/tc" && "$ZXCW" fmt src/ml.zx > /dev/null 2>&1 ) \
   && cmp -s "$pdir/tc/src/ml.zx" "$TPFX.tc.ml1" \
   && ( cd "$pdir/tc" && "$ZXCW" src/ml.zx --check > /dev/null 2>&1 ); then
    printf "  ok   手工多行归一（尾逗号树）+ 不补逗号 + 幂等 + 产物可编译\n"
else
    printf "  FAIL 手工多行归一\n"; cat "$TPFX.tc.mlf" 2>/dev/null; fail=1
fi
# 无尾逗号 → 整块收一行（用户截图场景：删掉尾逗号要回到一行），不补逗号，
# 幂等，产物可编译。
printf 'const pb = fn (a: i32, b: i32, c: i32) i32 { return a + b + c; };\n\nconst main = fn () i32 {\n    _ = pb(1,\n        2,\n        3\n    );\n    return 0;\n};\n' > "$pdir/tc/src/cl.zx"
if ( cd "$pdir/tc" && "$ZXCW" fmt src/cl.zx > "$TPFX.tc.clf" 2>&1 ) \
   && grep -qxF '    _ = pb(1, 2, 3);' "$pdir/tc/src/cl.zx" \
   && cp "$pdir/tc/src/cl.zx" "$TPFX.tc.cl1" \
   && ( cd "$pdir/tc" && "$ZXCW" fmt src/cl.zx > /dev/null 2>&1 ) \
   && cmp -s "$pdir/tc/src/cl.zx" "$TPFX.tc.cl1" \
   && ( cd "$pdir/tc" && "$ZXCW" src/cl.zx --check > /dev/null 2>&1 ); then
    printf "  ok   无尾逗号整块收一行 + 幂等 + 产物可编译\n"
else
    printf "  FAIL 无尾逗号收拢\n"; cat "$TPFX.tc.clf" 2>/dev/null; fail=1
fi
# 缩进口径（zig fmt）：字面量组嵌在调用括号里只比开组行深一级——字段 8 格、
# 闭行 }); 回到 4 格；同一行连开的组共享一级（曾多缩一级、闭行少退一级）。
printf 'const Cfg = struct{ width: i32, height: i32 };\n\nconst mk = fn (c: Cfg) i32 { return c.width + c.height; };\n\nconst main = fn () i32 {\n    _ = mk(.{ .width = 800, .height = 600, });\n    return 0;\n};\n' > "$pdir/tc/src/ind.zx"
if ( cd "$pdir/tc" && "$ZXCW" fmt src/ind.zx > "$TPFX.tc.indf" 2>&1 ) \
   && grep -qxF '    _ = mk(.{' "$pdir/tc/src/ind.zx" \
   && grep -qxF '        .width = 800,' "$pdir/tc/src/ind.zx" \
   && grep -qxF '    });' "$pdir/tc/src/ind.zx" \
   && cp "$pdir/tc/src/ind.zx" "$TPFX.tc.ind1" \
   && ( cd "$pdir/tc" && "$ZXCW" fmt src/ind.zx > /dev/null 2>&1 ) \
   && cmp -s "$pdir/tc/src/ind.zx" "$TPFX.tc.ind1" \
   && ( cd "$pdir/tc" && "$ZXCW" src/ind.zx --check > /dev/null 2>&1 ); then
    printf "  ok   嵌套字面量组缩进（字段 +1 级 / 闭行回语句层）+ 幂等\n"
else
    printf "  FAIL 嵌套字面量组缩进\n"; cat "$TPFX.tc.indf" 2>/dev/null; fail=1
fi
# 收拢后会超 100 列就原样保留（手工折行的长表达式不该被并成一条长行）。
printf 'const f = fn (a: u64, b: u64, c: u64, d: u64, e: u64, g: u64) u64 { return a + b + c + d + e + g; };\n\nconst main = fn () i32 {\n    _ = f(111111111111111,\n          222222222222222,\n          333333333333333,\n          444444444444444,\n          555555555555555,\n          666666666666666);\n    return 0;\n};\n' > "$pdir/tc/src/wd.zx"
if ( cd "$pdir/tc" && "$ZXCW" fmt src/wd.zx > "$TPFX.tc.wdf" 2>&1 ) \
   && ! grep -q '111111111111111, 222222222222222' "$pdir/tc/src/wd.zx" \
   && grep -qxF '    _ = f(111111111111111,' "$pdir/tc/src/wd.zx" \
   && ( cd "$pdir/tc" && "$ZXCW" src/wd.zx --check > /dev/null 2>&1 ); then
    printf "  ok   收拢超宽原样保留（不产长行）\n"
else
    printf "  FAIL 超宽收拢保护\n"; cat "$TPFX.tc.wdf" 2>/dev/null; fail=1
fi
# 宽度保护按"最终缩进"算，不按原始前导空白算：原始 16 格的行 fmt 会归一
# 到 4 格，按原始算会第一遍"超宽保留"、第二遍"不超宽收拢"来回摆（压测逮住）。
printf 'const pa = fn (a: i32, b: i32) i32 { return a + b; };\n\nconst main = fn () i32 {\n                _ = pa(1,\n                    2);\n    return 0;\n};\n' > "$pdir/tc/src/wi.zx"
if ( cd "$pdir/tc" && "$ZXCW" fmt src/wi.zx > "$TPFX.tc.wif" 2>&1 ) \
   && grep -qxF '    _ = pa(1, 2);' "$pdir/tc/src/wi.zx" \
   && cp "$pdir/tc/src/wi.zx" "$TPFX.tc.wi1" \
   && ( cd "$pdir/tc" && "$ZXCW" fmt src/wi.zx > /dev/null 2>&1 ) \
   && cmp -s "$pdir/tc/src/wi.zx" "$TPFX.tc.wi1" \
   && ( cd "$pdir/tc" && "$ZXCW" src/wi.zx --check > /dev/null 2>&1 ); then
    printf "  ok   宽度保护按最终缩进（原始超缩进行收拢后仍幂等）\n"
else
    printf "  FAIL 宽度保护不幂等\n"; cat "$TPFX.tc.wif" 2>/dev/null; fail=1
fi
if ( cd "$pdir/tc" && "$ZXCW" fmt src/main.zx > "$TPFX.tc.fmt" 2>&1 ) \
   && grep -q "^const add = fn ($" "$pdir/tc/src/main.zx" \
   && grep -q "^    a: i32,$" "$pdir/tc/src/main.zx" \
   && grep -q "^) i32 {$" "$pdir/tc/src/main.zx" \
   && grep -q "^        1,$" "$pdir/tc/src/main.zx" \
   && grep -q "^    const c = Cfg{$" "$pdir/tc/src/main.zx" \
   && grep -q "^        \.name = \"a\",$" "$pdir/tc/src/main.zx" \
   && grep -q "^    };$" "$pdir/tc/src/main.zx"; then
    cp "$pdir/tc/src/main.zx" "$TPFX.tc.once"
    ( cd "$pdir/tc" && "$ZXCW" fmt src/main.zx > /dev/null 2>&1 )
    if cmp -s "$pdir/tc/src/main.zx" "$TPFX.tc.once"; then
        printf "  ok   fmt 拆行（调用 + 字面量组递归）+ 幂等\n"
    else
        printf "  FAIL fmt 拆行不幂等\n"; diff "$TPFX.tc.once" "$pdir/tc/src/main.zx"; fail=1
    fi
else
    printf "  FAIL fmt 尾逗号拆行\n"; cat "$TPFX.tc.fmt"; fail=1
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
const std = @mod("std");
const c = @mod("cliadd");
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
const std = @mod("std");
const t = @mod("treelib");
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
    printf 'const std = @mod("std");\nconst g = @mod("geolib");\nconst main = fn () i32 {\n    std.println("add={} twice={} ver={}", g.geo_add(2, 3), g.geo_twice(), g.GEO_VER);\n    return 0;\n};\n' \
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
    printf 'const std = @mod("std");\nconst s = @mod("small");\nconst main = fn () i32 {\n    std.println("mul={} k={}", s.small_mul(4, 5), s.SMALL_K);\n    return 0;\n};\n' \
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
       && grep -q '"name": *"demo"' "$FDEMO/zx.json" \
       && [ "$(grep -Ec '"(http|https|file)://' "$FDEMO/zx.json")" -ge 3 ] \
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
    printf 'const std = @mod("std");\nconst a = @mod("flib");\nconst b = @mod("fgit");\nconst c = @mod("fmini");\nconst main = fn () i32 {\n    std.println("add={} mul={} k={} mini={}", a.flib_add(3, 4), b.fgit_mul(2, 6), b.FGIT_K, c.FMINI_K);\n    return 0;\n};\n' > "$FDEMO/src/main.zx"
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
    # 沙箱 rm 可能静默失败：用 mv 把库挪走等效模拟缺库
    mv "$FDEMO/vendor/flib" "$FDEMO/vendor/flib.gone" 2>/dev/null || true
    if ( cd "$FDEMO" && ZX_ALLOW_LOCAL_FETCH=1 "$ZXCW" fetch flib > "$TPFX.cli.fres" 2>&1 ) \
       && [ -f "$FDEMO/vendor/flib/src/add.c" ] \
       && grep -q "restored flib" "$TPFX.cli.fres"; then
        printf "  ok   缺库按锁恢复\n"
    else
        printf "  FAIL 缺库恢复\n"; cat "$TPFX.cli.fres" 2>/dev/null; fail=1
    fi
    rm -rf "$FDEMO/vendor/flib.gone" 2>/dev/null || true
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
    sed -i 's|"vendor": *\[|"vendor": [\n    "https://example.com/newlib.h",|' "$FDEMO/zx.json"
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
   && printf 'const std = @mod("std");\nconst v = @mod("vlib");\nconst s = @mod("slib");\nconst main = fn () i32 { std.println("{}", v.V + s.S); return 0; };\n' > "$MDIR/src/main.zx"; then
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
    # 注意顶层 const 是惰性求值的：q 不被使用时 @mod(...) 压根不加载，
    # 所以必须真的引用 q.X 才会走进模块解析链。
    printf 'const q = @mod("nope_missing");\nconst main = fn () i32 { return q.X; };\n' > "$MDIR/src/bad.zx"
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
# 三处码集要完全一致——src/zxc.c 的 g_rules（真源）、src/errors.c 的文案
# （`zxc errors` / `zxc explain` 都念它）、docs/errors.md（完整参考，诊断里的
# `details:` 链接与编辑器的 codeDescription 都指向它）。漏一处就是"用户拿着
# 码搜不到东西"，而码一旦发布又不能改，只能靠这里挡。
# 手册与离线文档站已删（std 还没定型），只留错误码参考。
printf "\n== 错误码目录（zxc errors / explain / docs/errors.md 三方一致）\n"
# 真源 = 编译器里实际会发出的码；文案表必须与它**相等**（多一个少一个都算漂移）
# 扫整个 src/*.c 而不是点名几个文件：新加的 .c 里出现错误码字面量时，
# 不能因为没被点名就悄悄跳出三方一致性检查。
codes=$(grep -ohE '"E[0-9]{4}"' src/*.c | tr -d '"' | sort -u)
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
# `zxc explain` 要真的把链接打出来（details: 行）
if $ZXC explain E0304 2>&1 | grep -q "#e0304"; then
    printf "  ok   zxc explain 给出文档链接\n"
else
    printf "  FAIL zxc explain 没给文档链接\n"; fail=1
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
printf "\n== §130 预分配：同区间 heapAlloc 合并为一次 alloc\n"
ZX_REGION=1 $ZXC tests/good/region-alloc.zx -o "$TPFX.rgnbin.exe" > "$TPFX.rgn.log" 2>&1 || true
rgc="$TPFX.rgnbin.exe_main.c"
na=$(grep -c 'zx_heapalloc(' "$rgc" 2>/dev/null || printf 0)
nf=$(grep -c 'zx_heapfree(' "$rgc" 2>/dev/null || printf 0)
if grep -q 'region merged: gid=.* slots=2 ' "$TPFX.rgn.log" \
   && [ "$na" = "1" ] && [ "$nf" = "1" ]; then
    printf "  ok   slots=2 合并：1 次 alloc + 静态偏移 + 1 次 free\n"
else
    printf "  FAIL 预分配守门失败（账本 / C 里 alloc=%s free=%s）\n" "$na" "$nf"
    sed -n '1,20p' "$TPFX.rgn.log"
    fail=1
fi

printf "\n== §132 账本可观测：explain region 打印 slot 状态与合并组\n"
$ZXC explain region tests/good/region-alloc.zx > "$TPFX.ledger.log" 2>&1 || true
if grep -q '^region ledger:' "$TPFX.ledger.log" \
   && grep -q 'slot 1 LIVE' "$TPFX.ledger.log" \
   && grep -q 'gid=1 off=0' "$TPFX.ledger.log"; then
    printf "  ok   explain region 输出 slot 状态 / 尺寸 / 合并组\n"
else
    printf "  FAIL explain region 账本守门失败\n"
    sed -n '1,20p' "$TPFX.ledger.log"
    fail=1
fi

printf "\n== §135 静态句柄：证明得了就发 getUnchecked，dev/release 同形态\n"
$ZXC tests/good/handle-erase.zx --emit-c --release > "$TPFX.erz.rel.c" 2>/dev/null
$ZXC tests/good/handle-erase.zx --emit-c > "$TPFX.erz.dev.c" 2>/dev/null
ZX_REGION=1 $ZXC tests/good/handle-erase.zx --check --release > "$TPFX.erz.log" 2>&1 || true
if grep -q 'erase: 2/2 proven, 1 erased' "$TPFX.erz.log" \
   && grep -q '_c0 = getUnchecked_' "$TPFX.erz.rel.c" \
   && grep -q '_c0 = getUnchecked_' "$TPFX.erz.dev.c" \
   && ! grep -q '_c0 = get_' "$TPFX.erz.dev.c"; then
    printf "  ok   两档都把 get 改成 getUnchecked，账本记 2/2 proven\n"
else
    printf "  FAIL 静态句柄擦除守门失败（账本或发射层）\n"
    sed -n '1,12p' "$TPFX.erz.log"
    grep -n '_c0 = ' "$TPFX.erz.rel.c" "$TPFX.erz.dev.c" | head -n 6
    fail=1
fi
$ZXC tests/good/handle-erase.zx -o "$TPFX.erz.bin" --release > /dev/null 2>&1 || true
"$TPFX.erz.bin" > "$TPFX.erz.run" 2>&1 || true
if grep -q '^get 11$' "$TPFX.erz.run" \
   && grep -q '^free true$' "$TPFX.erz.run"; then
    printf "  ok   静态句柄用例实跑输出正确\n"
else
    printf "  FAIL 静态句柄用例运行输出不对\n"
    sed -n '1,10p' "$TPFX.erz.run"
    fail=1
fi

printf "\n== §135 句柄只在编译期查：确定失效 E0510、证明不了 E0511\n"
$ZXC tests/bad/handle-invalid-unwrap.zx --check > "$TPFX.e0510.log" 2>&1 || true
if grep -q 'error\[E0510\]' "$TPFX.e0510.log" \
   && grep -q 'read of an invalid handle' "$TPFX.e0510.log"; then
    printf "  ok   E0510 按 path:line:col: error[CODE] 输出（diagnosticsWorker 可解析）\n"
else
    printf "  FAIL E0510 诊断格式不对（LSP 会解析不到）\n"
    sed -n '1,12p' "$TPFX.e0510.log"
    fail=1
fi
$ZXC tests/bad/handle-provenance-lost.zx --check > "$TPFX.e0511.log" 2>&1 || true
if grep -q 'error\[E0511\]' "$TPFX.e0511.log" \
   && grep -q 'cannot prove this handle is alive' "$TPFX.e0511.log"; then
    printf "  ok   抹源（Handle 存进字段）直接 E0511，没有运行时兜底\n"
else
    printf "  FAIL E0511 抹源守门失败\n"
    sed -n '1,12p' "$TPFX.e0511.log"
    fail=1
fi
$ZXC explain E0511 > "$TPFX.e0511.explain" 2>&1 || true
if grep -q 'docs/errors.md#e0511' "$TPFX.e0511.explain"; then
    printf "  ok   zxc explain E0511 有修法与文档锚点\n"
else
    printf "  FAIL zxc explain E0511 缺锚点\n"
    sed -n '1,12p' "$TPFX.e0511.explain"
    fail=1
fi

printf "\n== §148 分配器去虚拟化：能证明目标就直调\n"
$ZXC tests/good/devirt-alloc.zx --emit-c > "$TPFX.dv.c" 2>/dev/null
# 断言要看**调用点**，不能只 grep 函数名：函数定义/原型里本来就有那个名字，
# 只查名字会让「工厂链根本没去虚拟化」也变绿（§141 查出来的假绿）。
# §148 修好后工厂（DebugAllocator.init）也直调：容器方法是 NK_FIELD callee，
# 走 callee->inst 拿函数体；惰性子模块的体全查完后还有一轮重试，那时
# return Allocator{…} 里的 .alloc 才有 sym/ty。运行期分配器（被 opaque()
# 重新绑定过）证明不了，仍保持间接。
if grep -Eq 'zx_page_alloc\([a-z_0-9]+\.ctx, 32\)' "$TPFX.dv.c" \
   && grep -Eq 'countAlloc\([a-z_0-9]+\.ctx, 16\)' "$TPFX.dv.c" \
   && grep -Eq 'countFree\([a-z_0-9]+\.ctx,' "$TPFX.dv.c" \
   && grep -qF '= debug_alloc_trackedAlloc(d.ctx, 40)' "$TPFX.dv.c" \
   && grep -qF 'debug_alloc_trackedFree(d.ctx, r1, 40)' "$TPFX.dv.c" \
   && ! grep -qF 'd.alloc(d.ctx, 40)' "$TPFX.dv.c" \
   && grep -Eq 'c_[a-z_0-9]+\.alloc\(c_[a-z_0-9]+\.ctx, 8\)' "$TPFX.dv.c"; then
    printf "  ok   page / 字面量 / 工厂三条链直调；运行期分配器仍间接（§148）\n"
else
    printf "  FAIL 去虚拟化没按预期生效\n"
    grep -n 'alloc(' "$TPFX.dv.c" | head -n 12
    fail=1
fi
# 同一段在 C 前端也要成立（两个前端共用 sema，但 AST 形状可能不同）
$ZXC tests/good/devirt-alloc.zx --emit-c --fe=c > "$TPFX.dv.fec.c" 2>/dev/null
if grep -qF '= debug_alloc_trackedAlloc(d.ctx, 40)' "$TPFX.dv.fec.c" \
   && ! grep -qF 'd.alloc(d.ctx, 40)' "$TPFX.dv.fec.c"; then
    printf "  ok   --fe=c 下工厂链同样直调\n"
else
    printf "  FAIL --fe=c 下工厂链没直调\n"
    fail=1
fi
# §148 否定证据：改写 .alloc 字段 / 交出字段地址，都必须退回间接分发
$ZXC tests/good/devirt-alloc-negative.zx --emit-c > "$TPFX.dvn.c" 2>/dev/null
if grep -Eq 'w_[0-9_]+\.alloc\(w_[0-9_]+\.ctx, 8\)' "$TPFX.dvn.c" \
   && grep -Eq 'esc_[0-9_]+\.alloc\(esc_[0-9_]+\.ctx, 8\)' "$TPFX.dvn.c" \
   && ! grep -qE '= cAlloc\(|cFree\([a-z_0-9]+\.ctx,' "$TPFX.dvn.c"; then
    printf "  ok   字段改写 / 取字段地址两条否定链保持间接\n"
else
    printf "  FAIL 否定证据没拦住去虚拟化\n"
    grep -n 'alloc(' "$TPFX.dvn.c" | head -n 12
    fail=1
fi

printf "\n== §149 已知边界：跨函数后置抹源要回退已证明的调用点\n"
$ZXC tests/good/devirt-alloc-crossfn.zx --emit-c > "$TPFX.dvx.c" 2>/dev/null
# useIt() 的体先于 spoil() / main 检查：第一次按 G 的声明初值证明成功，
# 之后才出现 `G.alloc = cAllocB`。§148 只重试首次
# 失败的点，不回退已成功的改写 → 生成 `cAlloc(G.ctx, 8)`，spoil 之后
# useIt 仍走旧函数（假安全）。§149 用原始 NK_FIELD 统一复核并回退。
if grep -qF 'G.alloc(G.ctx, 8)' "$TPFX.dvx.c" \
   && grep -qF 'G.free_(G.ctx, p, 8)' "$TPFX.dvx.c" \
   && ! grep -qF '= cAlloc(G.ctx, 8)' "$TPFX.dvx.c"; then
    printf "  ok   后置否定证据让已证明的调用点回退成间接分发\n"
else
    printf "  FAIL 跨函数后置抹源没回退\n"
    grep -n 'alloc(' "$TPFX.dvx.c" | head -n 10
    fail=1
fi
$ZXC tests/good/devirt-alloc-crossfn.zx -o "$TPFX.dvx.exe" > /dev/null 2>&1 || true
"$TPFX.dvx.exe" > "$TPFX.dvx.run" 2>&1 || true
if grep -q '^cross: 1 1$' "$TPFX.dvx.run"; then
    printf "  ok   实跑：spoil 后 useIt 走新目标（§148 版会输出 cross: 2 0）\n"
else
    printf "  FAIL 回退后行为不对\n"
    sed -n '1,6p' "$TPFX.dvx.run"
    fail=1
fi
$ZXC tests/good/devirt-alloc-crossfn.zx --emit-c --fe=c > "$TPFX.dvx.fec.c" 2>/dev/null
if grep -qF 'G.alloc(G.ctx, 8)' "$TPFX.dvx.fec.c" \
   && ! grep -qF '= cAlloc(G.ctx, 8)' "$TPFX.dvx.fec.c"; then
    printf "  ok   --fe=c 下同样回退\n"
else
    printf "  FAIL --fe=c 下没回退\n"
    fail=1
fi

printf "\n== §150 模块级聚合初值 + std.heap.page 直写（两个既有后端缺口）\n"
$ZXC tests/good/global-agg-init.zx --emit-c > "$TPFX.gai.c" 2>/dev/null
# V_FN（函数引用）不能物化成静态数据：带函数引用的聚合交给 zx_init_main
# 按源码表达式赋值；纯标量聚合仍在静态初始化里，别一律改成运行时。
if grep -qF 'G = ((ZxS1){ .f = inc, .n = 5 });' "$TPFX.gai.c" \
   && grep -qF 'O = ((ZxS3){ .in = ((ZxS2){ .f = inc }), .n = 7 });' "$TPFX.gai.c" \
   && grep -qF 'static ZxS4 P = { 3, 7 };' "$TPFX.gai.c"; then
    printf "  ok   V_FN 聚合走 zx_init_main，标量聚合仍静态初始化\n"
else
    printf "  FAIL 模块级聚合初值没发出来\n"
    grep -n 'zx_init_main\|static ZxS' "$TPFX.gai.c" | head -n 12
    fail=1
fi
$ZXC tests/good/global-agg-init.zx -o "$TPFX.gai.exe" > /dev/null 2>&1 || true
"$TPFX.gai.exe" > "$TPFX.gai.run" 2>&1 || true
if grep -q '^g: 5 5$' "$TPFX.gai.run" \
   && grep -q '^o: 2 7$' "$TPFX.gai.run" \
   && grep -q '^p: 3 7$' "$TPFX.gai.run"; then
    printf "  ok   实跑：全局函数指针字段可调用（§149 版会段错误）\n"
else
    printf "  FAIL 全局聚合初值实跑不对\n"
    sed -n '1,6p' "$TPFX.gai.run"
    fail=1
fi
# 小函数里直写 page.alloc：合成标识符 zx_page_alloc 在 ct_call 克隆体
# 重查时不该报 E0401（§149 版会）。双前端都要过。
for fe in "" "--fe=c"; do
    $ZXC tests/good/heap-page-direct.zx --check $fe > "$TPFX.hpd.log" 2>&1 || true
    if ! grep -q 'E0401' "$TPFX.hpd.log"; then
        printf "  ok   %s 下 page.alloc 合成标识符可重查\n" "${fe:-默认前端}"
    else
        printf "  FAIL %s 下 page.alloc 仍报 E0401\n" "${fe:-默认前端}"
        head -n 6 "$TPFX.hpd.log"
        fail=1
    fi
done
$ZXC tests/good/heap-page-direct.zx -o "$TPFX.hpd.exe" > /dev/null 2>&1 || true
"$TPFX.hpd.exe" > "$TPFX.hpd.run" 2>&1 || true
if grep -q '^page: ok$' "$TPFX.hpd.run"; then
    printf "  ok   实跑：page.alloc/free 行为正常\n"
else
    printf "  FAIL page.alloc 实跑不对\n"
    sed -n '1,6p' "$TPFX.hpd.run"
    fail=1
fi

# 自动内联（§138）：入口模块的小函数在 -O0 也展开
if [ "$(grep -c '^ZX_INLINE ' "$TPFX.dv.c")" -ge 1 ]; then
    printf "  ok   小函数自动带 ZX_INLINE（dev 档 -O0 也内联）\n"
else
    printf "  FAIL 自动内联没有生效\n"
    fail=1
fi
$ZXC tests/good/devirt-alloc.zx -o "$TPFX.dv.exe" > /dev/null 2>&1 || true
"$TPFX.dv.exe" > "$TPFX.dv.run" 2>&1 || true
if grep -q '^page: live 0 bytes 0$' "$TPFX.dv.run" \
   && grep -q '^literal: live 0 allocs 2$' "$TPFX.dv.run" \
   && grep -q '^factory: live 0$' "$TPFX.dv.run" \
   && grep -q '^runtime: live 0$' "$TPFX.dv.run"; then
    printf "  ok   四条链行为一致（实跑）\n"
else
    printf "  FAIL 去虚拟化后行为不对\n"
    sed -n '1,8p' "$TPFX.dv.run"
    fail=1
fi

printf "\n== §138 DebugAllocator：泄漏 / 重复释放 / 长度不符都报现场\n"
$ZXC tests/good/debug-alloc.zx -o "$TPFX.da.exe" > /dev/null 2>&1 || true
"$TPFX.da.exe" > "$TPFX.da.run" 2>&1 || true
if grep -q '^pushes: len 4 live 1 bytes 32 peak 32$' "$TPFX.da.run" \
   && grep -q '^after deinit: live 0 bytes 0$' "$TPFX.da.run"; then
    printf "  ok   正常路径记账准确、收尾干净\n"
else
    printf "  FAIL DebugAllocator 正常路径\n"
    sed -n '1,8p' "$TPFX.da.run"
    fail=1
fi
$ZXC tests/good/debug-alloc-abort.zx -o "$TPFX.daab.exe" > /dev/null 2>&1 || true
"$TPFX.daab.exe" leak > "$TPFX.da.leak" 2>&1 || true
if grep -q 'LEAK: 1 block(s) / 24 byte(s) still live' "$TPFX.da.leak" \
   && grep -q 'block #1  24 byte(s)' "$TPFX.da.leak" \
   && grep -q 'panic: \[debug_alloc\] memory leak' "$TPFX.da.leak"; then
    printf "  ok   泄漏：逐块列出（序号/大小/地址）后 panic\n"
else
    printf "  FAIL 泄漏报告\n"
    sed -n '1,10p' "$TPFX.da.leak"
    fail=1
fi
"$TPFX.daab.exe" double > "$TPFX.da.dbl" 2>&1 || true
if grep -q 'double free: block #1 at' "$TPFX.da.dbl" \
   && grep -q 'panic: \[debug_alloc\] double free' "$TPFX.da.dbl"; then
    printf "  ok   重复释放：认出已释放的块并报错\n"
else
    printf "  FAIL 重复释放\n"
    sed -n '1,10p' "$TPFX.da.dbl"
    fail=1
fi
"$TPFX.daab.exe" size > "$TPFX.da.size" 2>&1 || true
if grep -q 'size mismatch at .*: free(64) but block #1 is 16 byte(s)' "$TPFX.da.size" \
   && grep -q 'panic: \[debug_alloc\] free size mismatch' "$TPFX.da.size"; then
    printf "  ok   释放长度不符：报出两边的大小\n"
else
    printf "  FAIL 释放长度校验\n"
    sed -n '1,10p' "$TPFX.da.size"
    fail=1
fi

printf "\n== 模块名必须显式绑定（别名可缩写 / 可再导出）\n"
BP="$TPFX.modbind"
if mkdir -p "$BP/src" \
   && printf '{"name":"modbind","version":"0.1.0","main":"src/main.zx","out":"modbind"}\n' > "$BP/zx.json" \
   && printf 'pub const add = fn (a: i32, b: i32) i32 { return a + b; };\n' > "$BP/src/util.zx" \
   && printf 'pub const math = @mod("util");\n' > "$BP/src/api.zx" \
   && printf 'const std = @mod("std");\nconst g = @mod("util");\nconst api = @mod("api");\n\npub const main = fn () !i32 {\n    std.println("{} {}", g.add(1, 2), api.math.add(3, 4));\n    return 0;\n};\n' > "$BP/src/main.zx" \
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

# 2.56b §203：模块数超过旧定长 64 时不得静默截断。
#     过去 backend_emit_split 有 `if (nmods > maxunits) nmods = maxunits`，
#     第 65 个模块被丢掉：sema 检查过、头文件也声明了原型，却不发 .c，
#     链接报 undeclared。这里造 1 入口 + 64 模块，逐个调用，验证和 = 64。
printf "\n== 超过 64 个模块不得挤掉最后一个（§203）\n"
MP="$TPFX.many"
if mkdir -p "$MP/src"; then
    printf '{"name":"many","version":"0.1.0","main":"src/main.zx","out":"many"}\n' > "$MP/zx.json"
    {
        printf 'const std = @mod("std");\n'
        i=1; while [ $i -le 64 ]; do
            printf 'const m%02d = @mod("m%02d");\n' $i $i
            i=$((i+1))
        done
        printf 'pub const main = fn () u8 {\n    var s: u64 = 0;\n'
        i=1; while [ $i -le 64 ]; do
            printf '    s = s + m%02d.f();\n' $i
            i=$((i+1))
        done
        printf '    if (s != 64) { return 1; }\n    std.println("sum={}", s);\n    return 0;\n};\n'
    } > "$MP/src/main.zx"
    i=1; while [ $i -le 64 ]; do
        printf 'pub const f = fn () u64 { return 1; };\n' > "$MP/src/m$(printf '%02d' $i).zx"
        i=$((i+1))
    done
    ( cd "$MP" && "$ZXCW" run > "$TPFX.manyrun" 2>&1 )
    if grep -q "^sum=64$" "$TPFX.manyrun"; then
        printf "  ok   65 个模块全部发射，和 = 64\n"
    else
        printf "  FAIL 第 65 个模块被挤掉\n"; tail -15 "$TPFX.manyrun"; fail=1
    fi
else
    printf "  FAIL 无法创建 65 模块测试目录\n"; fail=1
fi

# 2.57 实参个数不匹配只报 E0303，不得假报形参 unknown identifier。
#     用户 2026-11-08 截图踩中：ct_call 克隆执行函数体时只绑
#     min(形参,实参) 个形参，push() 零参调用把 push 体里的 a、b 全部
#     假报成 unknown identifier（还带 "did you mean 调用者的 h" 的
#     误导建议）。KEEP 标记在 src/sema_ct.c ct_call。
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
   && printf 'const u = @mod("util");\n\npub const main = fn () i32 {\n    return u.nope();\n};\n' > "$BP/src/main.zx" \
   && ( cd "$BP" && "$ZXCW" src/main.zx --check > "$TPFX.mfail1" 2>&1 || true ) \
   && grep -q "src/main.zx:1:11: error\[E0601\]: module 'src/util.zx' failed to parse: 1:22: hex literal needs digits" "$TPFX.mfail1" \
   && ! grep -q "has no member" "$TPFX.mfail1"; then
    printf "  ok   导入点报出行列与原因，且无误导成员错误\n"
else
    printf "  FAIL 子模块解析失败的父诊断不完整\n"
    cat "$TPFX.mfail1" 2>/dev/null
    fail=1
fi

# LSP 冒烟（tools/lsptest.py）依赖 python，已从回归主链移除（§187：
# 测试基建只依赖 zig 与本编译器自身）；需要时手动跑：
#   ZXLS_BIN=<zxls.exe> python tools/lsptest.py

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

# 2.1b 基准校验和闸门：bench/containers.zx 五负载校验和必须与基线逐字符
# 一致（bench/BASELINE.md——差分测试锚点，跨后端/跨优化等级都要一致）。
# 2026-11-19 曾失效过一阵：std 容器行为变了、文档没跟上，没有测试守它。
printf "\n== 基准校验和（bench/containers.zx，对齐 bench/BASELINE.md）\n"
CNT="$TPFX.cont.exe"
if $ZXC bench/containers.zx -o "$CNT" > "$TPFX.contbuild" 2>&1; then
    CONT_OUT="$("$CNT" 2>/dev/null | tail -n 1 | tr -d '\r')"
    if [ "$CONT_OUT" = "7385034332107988923 6784541450624509831 4843076618602921639 2486116468307319228 17061912414904705757" ]; then
        printf "  ok   cont 五负载校验和与基线一致\n"
    else
        printf "  FAIL cont 校验和漂移（got: %s）——std 容器行为变了：先确认是不是有意的，\n" "$CONT_OUT"
        printf "        是就同步 bench/BASELINE.md 与本行锚点，不是就先修容器\n"
        fail=1
    fi
else
    printf "  FAIL bench/containers.zx 编译失败\n"; cat "$TPFX.contbuild"; fail=1
fi

printf "\n== §144 heapSlice / @mod / 数组指针下标\n"
$ZXC tests/good/heap-slice.zx --emit-c > "$TPFX.hs.c" 2>/dev/null
# count 只许求值一次：后端按 []T 生成一份特化助手，n 是唯一的函数实参。
# 行为上 heap-slice.zx 的 `calls = 1` 已经守住；这里再钉住生成代码结构，
# 免得哪天改回 { .ptr=..., .len=n } 那种把 n 发两遍的形状。
if grep -q 'ZxSlice_i32 zx_heapslice_ZxSlice_i32(' "$TPFX.hs.c" \
   && grep -q 'r.len = (size_t)n;' "$TPFX.hs.c" \
   && grep -q 'zx_heapalloc(al, n \* sizeof' "$TPFX.hs.c"; then
    printf "  ok   heapSlice 按 []T 生成助手，count 只求值一次\n"
else
    printf "  FAIL heapSlice 生成代码不合预期\n"
    grep -n 'zx_heapslice_\|r.len\|zx_heapalloc(' "$TPFX.hs.c" | head -n 12
    fail=1
fi
# module 关键字已换成内建 @mod：旧写法必须给 E0211 与替代写法
$ZXC tests/bad/removed-module.zx --check > "$TPFX.oldmod.log" 2>&1 || true
if grep -q 'E0211' "$TPFX.oldmod.log" && grep -q '@mod' "$TPFX.oldmod.log"; then
    printf "  ok   旧 module(...) 写法给出 E0211 + @mod 提示\n"
else
    printf "  FAIL 旧 module(...) 写法没有迁移提示\n"; cat "$TPFX.oldmod.log"; fail=1
fi
# *[N]T 的 ptr.0/.1、ptr[0]/ptr[i] 由 array-pointer-index.zx 逐字比对；
# 这里再确认运行期界检查确实进了生成代码。
if $ZXC tests/good/array-pointer-index.zx --emit-c > "$TPFX.api.c" 2>/dev/null \
   && grep -q 'out of bounds' "$TPFX.api.c"; then
    printf "  ok   *[N]T 的 ptr[i] 带运行期界检查\n"
else
    printf "  FAIL ptr[i] 没生成界检查\n"; fail=1
fi
# §205：.{…} 位置形式已是 [N]T 数组值；removed-dot-tuple 现在守的是数组 .N
# 点下标的 E0211。解构 .{a,b} 仍是 E0211。两套前端都给迁移提示。
# runner 只跑默认前端（zxfe），这里补一条 --fe=c 的对拍。
for dt in removed-dot-tuple removed-dot-pattern; do
    $ZXC "tests/bad/$dt.zx" --check --fe=c > "$TPFX.$dt.c.log" 2>&1 || true
    if grep -q 'E0211' "$TPFX.$dt.c.log" \
       && grep -q 'was removed' "$TPFX.$dt.c.log"; then
        printf "  ok   %s 在 C 前端也给迁移提示\n" "$dt"
    else
        printf "  FAIL %s 在 C 前端没有迁移提示\n" "$dt"
        cat "$TPFX.$dt.c.log"; fail=1
    fi
done

# §224：方法接收者形态由写法决定，编译器不再按大小改 ABI。
# `Self` 一律按值（8B 和 20B 都是）、`*Self` 发可变指针、`*const Self` 发
# const 指针；方法体里不再补 `(*self)`，调用点不再自动补 `&` / 自动解引用。
$ZXC tests/good/self-recv-abi.zx --emit-c > "$TPFX.s151.c" 2>/dev/null
if grep -Eq 'sum_[A-Za-z0-9_]+\(ZxS[0-9]+ self\)' "$TPFX.s151.c" \
   && grep -Eq 'total_[A-Za-z0-9_]+\(ZxS[0-9]+ self\)' "$TPFX.s151.c" \
   && grep -Eq 'copy_[A-Za-z0-9_]+\(ZxS[0-9]+ self\)' "$TPFX.s151.c" \
   && grep -Eq 'bumpMe_[A-Za-z0-9_]+\(ZxS[0-9]+ \* self,' "$TPFX.s151.c" \
   && grep -Eq 'peek_[A-Za-z0-9_]+\(ZxS[0-9]+ \* self\)' "$TPFX.s151.c" \
   && ! grep -qF '(*self)' "$TPFX.s151.c"; then
    printf "  ok   §224 Self 接收者按写法定形态（值/可变指针/只读指针，无 (*self)）\n"
else
    printf "  FAIL §224 Self 接收者形态判定\n"
    grep -nE 'sum_|total_|copy_|zero_|peek_|bumpMe_' "$TPFX.s151.c" | head -20
    fail=1
fi

# §224：`&self` 就是普通形参取址，发 `&(self)`，不再按 ABI 穿透成
# `&((*self))`（按值 self 是副本，取址取的就是这个副本；要拿调用者地址
# 得把方法写成 `self: *Self` / `self: *const Self`）。
# 只读 / 不可返回由 tests/bad/addr-self-*.zx 的 E0403、E0503 守门。
$ZXC tests/good/addr-self.zx --emit-c > "$TPFX.s152.c" 2>/dev/null
if grep -qF '&(self)' "$TPFX.s152.c" \
   && grep -qF '&(self.a)' "$TPFX.s152.c" \
   && ! grep -qF '&((*self))' "$TPFX.s152.c"; then
    printf "  ok   §224 &self 按普通形参取址（&(self)，无 ABI 穿透）\n"
else
    printf "  FAIL §224 &self 取址形态\n"
    grep -nE '&\(\(?\*?self' "$TPFX.s152.c" | head -20
    fail=1
fi

# §153：region 合并用的分配器是本块里的局部声明时，`al` 的 C 声明必须排在
# `__zxrN = zx_heapalloc(al, …)` 之前，否则 C 先用后声明报 B1002。
# 用例本身由 runner 实跑（并触发 merged=2），这里只钉住发射顺序。
$ZXC tests/good/region-local-alloc.zx --emit-c > "$TPFX.s153.c" 2>/dev/null
S153_AL=$(grep -nE 'ZxS[0-9]+ al_[0-9_]+ = zx_alloc_page;' "$TPFX.s153.c" | head -1 | cut -d: -f1)
S153_RG=$(grep -nE '__zxr[0-9]+ = .*zx_heapalloc\(al_' "$TPFX.s153.c" | head -1 | cut -d: -f1)
if [ -n "$S153_AL" ] && [ -n "$S153_RG" ] && [ "$S153_AL" -lt "$S153_RG" ]; then
    printf "  ok   §153 region 局部分配器先声明后使用\n"
else
    printf "  FAIL §153 region 分配器声明顺序\n"
    grep -nE 'al_[0-9_]+|__zxr' "$TPFX.s153.c" | head -12
    fail=1
fi

# §154：[]T 的 .size 是运行期 len × 元素大小。后端曾发 layout_size(切片)=16，
# 和 sema 的 ct_field（len × sizeof(elem)）给出两个答案。.len 的运行期性由
# tests/bad/slice-len-const.zx、slice-size-const.zx 的 E0501 守门。
$ZXC tests/good/slice-len-runtime.zx --emit-c > "$TPFX.s154.c" 2>/dev/null
if grep -qF '.len) * (size_t)4' "$TPFX.s154.c" \
   && ! grep -qF '((size_t)16)' "$TPFX.s154.c"; then
    printf "  ok   §154 []T.size 按运行期 len 计算（非描述符 16 字节）\n"
else
    printf "  FAIL §154 []T.size 发射形状\n"
    grep -nE '\.len\)|\(\(size_t\)16\)' "$TPFX.s154.c" | head -12
    fail=1
fi

# §155：带标签 continue 必须落到外层循环的 continue 标号（goto Lcont1），
# 不是内层的 Lcont2；未知标签报 E0405。冒号不能省，语法由 bad 用例守门。
$ZXC tests/good/continue-label.zx --emit-c > "$TPFX.s155.c" 2>/dev/null
if grep -qF 'goto Lcont1;' "$TPFX.s155.c" \
   && grep -qF 'Lcont2: ;' "$TPFX.s155.c" \
   && grep -qF 'Lcont1: ;' "$TPFX.s155.c"; then
    printf "  ok   §155 continue :label 跨层跳转\n"
else
    printf "  FAIL §155 continue :label 没落到外层循环\n"
    grep -nE 'Lcont|goto' "$TPFX.s155.c" | head -12
    fail=1
fi

# §156：类型构造函数 fn (T: type, n: T) type 返回的是类型值，n 可作字段默认值。
# 两个前端都得认（默认前端 zxfe 由 runner 实跑，这里补 C 前端对拍）。
for f156 in slice-len-runtime continue-label generic-type-default; do
    if $ZXC "tests/good/$f156.zx" --check --fe=c > "$TPFX.$f156.fe.log" 2>&1; then
        printf "  ok   §154-§156 %s 在 C 前端通过\n" "$f156"
    else
        printf "  FAIL §154-§156 %s 在 C 前端失败\n" "$f156"
        head -12 "$TPFX.$f156.fe.log"; fail=1
    fi
done

# §157：新 std.math / std.net，以及它们牵出的两个真编译器缺口。
# 1) 多参 extern 原型的无名形参必须按位置编号；全叫 `_p` 会被 C 判参数重定义。
#    §158 把 std/math.zx 的形参改成具名后，这条能力改由 extern-unnamed.zx 钉住。
$ZXC tests/good/extern-unnamed.zx --emit-c > "$TPFX.s157u.c" 2>/dev/null
if grep -qF 'zxUnnamedA(int32_t _p0, int32_t _p1);' "$TPFX.s157u.c" \
   && grep -qF 'zxUnnamedB(double _p0, double _p1);' "$TPFX.s157u.c" \
   && ! grep -qF '_p, double _p' "$TPFX.s157u.c"; then
    printf "  ok   §157 无名 extern 形参按位置编号\n"
else
    printf "  FAIL §157 多参 extern 原型仍撞名\n"
    grep -nE 'zxUnnamed' "$TPFX.s157u.c" | head -8; fail=1
fi
# §158：库里公开成员的 extern 形参一律具名，补全/hover 不能退化成 C 原型。
# sqrt/pow/floor/... 在 backend.c 的 libc_declared() 名单里：生成的 C 头
# 已 include math.h，这些名字不重复声明（否则与标准头冲突），所以这里只钉
# 真正会发射出来的 cbrt/atan2/hypot；一元、二元都要带形参名，且不能出现
# gen_extern 给无名形参补的 _p0/_p1 编号。
$ZXC tests/good/std-math.zx --emit-c > "$TPFX.s157m.c" 2>/dev/null
if grep -qF 'extern double cbrt(double x);' "$TPFX.s157m.c" \
   && grep -qF 'extern double atan2(double y, double x);' "$TPFX.s157m.c" \
   && grep -qF 'extern double hypot(double x, double y);' "$TPFX.s157m.c" \
   && ! grep -qE '\(double _p[0-9]' "$TPFX.s157m.c"; then
    printf "  ok   §158 std.math 的 extern 形参具名\n"
else
    printf "  FAIL §158 std.math 的 extern 形参退化成无名原型\n"
    grep -nE 'extern double (cbrt|atan2|hypot)|double _p[0-9]' "$TPFX.s157m.c" | head -8; fail=1
fi

# 2) 模块内裸名内建（g_std_seeded）必须和 std. 前缀走同一条分派：发射真的
#    alloca / heapslice / zx_format / printf，而不是未声明的裸标识符。
$ZXC tests/multi/seeded-builtins/main.zx --emit-c > "$TPFX.s157s.c" 2>/dev/null
if grep -qF '__builtin_alloca' "$TPFX.s157s.c" \
   && grep -qF '= zx_format(' "$TPFX.s157s.c" \
   && grep -qF 'printf("println-ok' "$TPFX.s157s.c"; then
    printf "  ok   §157 模块内裸名内建发射真实 C\n"
else
    printf "  FAIL §157 裸名内建发射形状\n"
    grep -nE 'alloca|zx_format|println\(' "$TPFX.s157s.c" | head -12; fail=1
fi

# 3) 新库 / 新守门用例在 C 前端也要过（默认 zxfe 前端由 runner 实跑）。
for f157 in std-math std-net; do
    if $ZXC "tests/good/$f157.zx" --check --fe=c > "$TPFX.$f157.fe.log" 2>&1; then
        printf "  ok   §157 %s 在 C 前端通过\n" "$f157"
    else
        printf "  FAIL §157 %s 在 C 前端失败\n" "$f157"
        head -12 "$TPFX.$f157.fe.log"; fail=1
    fi
done
if $ZXC tests/multi/seeded-builtins/main.zx --check --fe=c > "$TPFX.s157s.fe.log" 2>&1; then
    printf "  ok   §157 seeded-builtins 在 C 前端通过\n"
else
    printf "  FAIL §157 seeded-builtins 在 C 前端失败\n"
    head -12 "$TPFX.s157s.fe.log"; fail=1
fi

# §158：入口单元也种 g_std_seeded 之后，库文件当单文件检查不能再假报 E0401。
# LSP 的 runCheck 走的就是这条 `zxc <同目录影子文件> --check`，所以这几行
# 直接钉住用户截图里 std/net.zx 的红色波浪线；std/fs.zx 的裸名 `file`
# 同一根因，一起守门。
for f158 in net fs; do
    if $ZXC "std/$f158.zx" --check > "$TPFX.s158.$f158.log" 2>&1 \
       && [ ! -s "$TPFX.s158.$f158.log" ]; then
        printf "  ok   §158 std/%s.zx 单文件检查无假诊断\n" "$f158"
    else
        printf "  FAIL §158 std/%s.zx 单文件检查仍有假诊断\n" "$f158"
        head -12 "$TPFX.s158.$f158.log"; fail=1
    fi
done

$ZXC tests/good/seeded-builtins-entry.zx --emit-c > "$TPFX.s158e.c" 2>/dev/null
if grep -qF '__builtin_alloca' "$TPFX.s158e.c" \
   && grep -qF 'printf("entry=' "$TPFX.s158e.c" \
   && grep -qF '= zx_format(' "$TPFX.s158e.c"; then
    printf "  ok   §158 入口裸名内建发射真实 C\n"
else
    printf "  FAIL §158 入口裸名内建发射形状\n"
    grep -nE 'alloca|entry=|zx_format' "$TPFX.s158e.c" | head -12; fail=1
fi

for f158c in extern-unnamed seeded-builtins-entry; do
    if $ZXC "tests/good/$f158c.zx" --check --fe=c > "$TPFX.$f158c.fe.log" 2>&1; then
        printf "  ok   §158 %s 在 C 前端通过\n" "$f158c"
    else
        printf "  FAIL §158 %s 在 C 前端失败\n" "$f158c"
        head -12 "$TPFX.$f158c.fe.log"; fail=1
    fi
done

# §159 / §165：std.simd 是 `@Vec` 的数学层。dev（-O0）与 release（-O2）是
# 两条代码路径（后者会把 select 的四次循环展开），数值语义必须一致；库文件
# 单文件检查不能假报；f32 的平方根走 sqrtf，不做 f32→f64→f32 往返。
$ZXC tests/good/std-simd.zx -o "$TPFX.s159.exe" > "$TPFX.s159.log" 2>&1 || true
"$TPFX.s159.exe" > "$TPFX.s159.dev" 2>&1 || true
$ZXC tests/good/std-simd.zx --release -o "$TPFX.s159r.exe" > "$TPFX.s159r.log" 2>&1 || true
"$TPFX.s159r.exe" > "$TPFX.s159.run" 2>&1 || true
if cmp -s "$TPFX.s159.dev" "$TPFX.s159.run" \
   && grep -qF 'metric: dot=25 lenSq=25 len=5 dist=5' "$TPFX.s159.run" \
   && grep -qF 'quat slerp: true true' "$TPFX.s159.run"; then
    printf "  ok   §159 std.simd 数学层 dev / release 数值一致\n"
else
    printf "  FAIL §159 std.simd 数学层输出漂移\n"
    diff "$TPFX.s159.dev" "$TPFX.s159.run" | head -8
    head -6 "$TPFX.s159.log"; head -6 "$TPFX.s159r.log"; fail=1
fi

$ZXC tests/good/std-simd.zx --emit-c > "$TPFX.s159.c" 2>/dev/null
if grep -qF 'extern float sqrtf(float x);' "$TPFX.s159.c"; then
    printf "  ok   §159 length() 走 f32 的 sqrtf\n"
else
    printf "  FAIL §159 生成了 f64 往返的 sqrt\n"
    grep -nE 'sqrtf?\(double|sqrt\(' "$TPFX.s159.c" | head -6; fail=1
fi

if $ZXC std/simd.zx --check > "$TPFX.s159m.log" 2>&1; then
    printf "  ok   §159 std/simd.zx 单文件检查无假诊断\n"
else
    printf "  FAIL §159 std/simd.zx 单文件检查有诊断\n"
    head -12 "$TPFX.s159m.log"; fail=1
fi

if $ZXC tests/good/std-simd.zx --check --fe=c > "$TPFX.s159c.log" 2>&1; then
    printf "  ok   §159 std-simd 在 C 前端通过\n"
else
    printf "  FAIL §159 std-simd 在 C 前端失败\n"
    head -12 "$TPFX.s159c.log"; fail=1
fi

# §168：std.simd 数值压测。dev 档已由 tests/good/*.zx 自动发现比对过
# expect-output；这里只额外钉 release 档——优化不能改变浮点语义，随机样本
# 也不许出现任何一个 bad。`stress eps0` 守 approxEq 的 eps=0 退化语义。
$ZXC tests/good/simd-stress.zx --release -o "$TPFX.s168r.exe" > "$TPFX.s168r.log" 2>&1 || true
"$TPFX.s168r.exe" > "$TPFX.s168.run" 2>&1 || true
if grep -qF 'stress mat4 inv: n=20000 bad=0' "$TPFX.s168.run" \
   && grep -qF 'stress quat m3: n=20000 bad=0' "$TPFX.s168.run" \
   && grep -qF 'stress persp: n=5000 bad=0' "$TPFX.s168.run" \
   && grep -qF 'stress eps0: eq=true ne=false' "$TPFX.s168.run" \
   && ! grep -q 'bad=[1-9]' "$TPFX.s168.run"; then
    printf "  ok   §168 std.simd 压测 release 档全样本通过\n"
else
    printf "  FAIL §168 std.simd 压测 release 档有失败样本\n"
    head -12 "$TPFX.s168.run"; head -6 "$TPFX.s168r.log"; fail=1
fi

# §162：语言级向量类型 @Vec(T, N)。语言层只管"这是个 N 分量的值"，
# 落到 C 的是 clang 的 vector_size 类型——不是用 struct 模拟的（用 struct
# 模拟在 -O0 下完全不会用向量指令，而且字段布局要自己维护）。
$ZXC tests/good/vec.zx --emit-c > "$TPFX.s162.c" 2>/dev/null
if grep -qF 'vector_size(16)' "$TPFX.s162.c" \
   && grep -qF 'vector_size(32)' "$TPFX.s162.c" \
   && grep -qF 'zx_splat_vf32_4(' "$TPFX.s162.c"; then
    printf "  ok   §162 @Vec 落到 C 的 vector_size 类型（含 splat 助手）\n"
else
    printf "  FAIL §162 --emit-c 里没有 vector_size 类型\n"
    head -6 "$TPFX.s162.c"; fail=1
fi

# 两个前端必须生成同一份东西：向量类型是新加的 NodeKind，自举前端漏了
# 就会在 --fe=zx 下报"unknown type"，而 --fe=c 一切正常。
$ZXC tests/good/vec.zx -o "$TPFX.s162zx.exe" > "$TPFX.s162zx.log" 2>&1 || true
$ZXC tests/good/vec.zx --fe=c -o "$TPFX.s162c.exe" > "$TPFX.s162c.log" 2>&1 || true
"$TPFX.s162zx.exe" > "$TPFX.s162zx.out" 2>&1 || true
"$TPFX.s162c.exe"  > "$TPFX.s162c.out"  2>&1 || true
if cmp -s "$TPFX.s162zx.out" "$TPFX.s162c.out" \
   && grep -qF 'field: pos3=100 pos7=1.25 tag=7 bodysize=64' "$TPFX.s162zx.out"; then
    printf "  ok   §162 zx / c 两个前端输出一致（向量字段布局 = 对齐 = 大小）\n"
else
    printf "  FAIL §162 两个前端输出不一致\n"
    diff "$TPFX.s162zx.out" "$TPFX.s162c.out" | head -6
    head -6 "$TPFX.s162zx.log"; head -6 "$TPFX.s162c.log"; fail=1
fi

# -O2 会真的生成向量指令；数值必须是同一份（不许因为优化改变结果）
$ZXC tests/good/vec.zx --release -o "$TPFX.s162rel.exe" > "$TPFX.s162rel.log" 2>&1 || true
"$TPFX.s162rel.exe" > "$TPFX.s162rel.out" 2>&1 || true
if cmp -s "$TPFX.s162zx.out" "$TPFX.s162rel.out"; then
    printf "  ok   §162 dev / release 输出一致\n"
else
    printf "  FAIL §162 release 输出不同\n"
    diff "$TPFX.s162zx.out" "$TPFX.s162rel.out" | head -6; fail=1
fi

# §162 续：字面量 / 比较必须是"真的落到 C 的向量操作"，而不是被包成逐分量
# 循环或整数运算：
#   字面量 → 复合字面量 `(ZxVec_vf32_4){…}`（不是 4 次下标赋值）
#   比较   → 一次向量比较后再 cast 成掩码类型（C 里本来就是 0 / -1）
# 掩码**选择**不再由编译器发：`std.simd.select` 是普通函数，C 里看不到
# `_selN` 临时量，也看不到随类型发的 `zx_any_*` 助手（§165 撤掉了内建）。
if grep -qF '(ZxVec_vf32_4){(float)(1.0)' "$TPFX.s162.c" \
   && grep -qF 'ZxVec_vi32_4 lt_' "$TPFX.s162.c" \
   && ! grep -qF 'zx_any_vi32_4' "$TPFX.s162.c" \
   && ! grep -qF '_sel' "$TPFX.s162.c"; then
    printf "  ok   §162 字面量 = 复合字面量、比较 = 掩码 cast（选择已不在编译器里）\n"
else
    printf "  FAIL §162 字面量/比较的 C 发射不是向量操作\n"
    grep -n 'ZxVec_' "$TPFX.s162.c" | head -8; fail=1
fi

# §165：向量的三个内建撤掉——`@Vec` 是唯一的向量内建，掩码选择与判真是
# std.simd 里的普通函数。两件事各自守住：
#   旧拼写 → E0211 + "moved to std.simd.xxx"，两个前端说同一句；
#   库函数 → 纯 Zx（没有 @shim、没有 intrinsic），语义见 good/std-simd.zx。
for vgone in removed-select removed-any removed-all; do
    $ZXC "tests/bad/$vgone.zx" --check > "$TPFX.$vgone.log" 2>&1 || true
    $ZXC "tests/bad/$vgone.zx" --check --fe=c > "$TPFX.$vgone.c.log" 2>&1 || true
    if grep -q 'E0211' "$TPFX.$vgone.log" && grep -q 'std\.simd\.' "$TPFX.$vgone.log" \
       && grep -q 'E0211' "$TPFX.$vgone.c.log"; then
        printf "  ok   §165 %s 报 E0211 + std.simd 迁移提示（双前端）\n" "$vgone"
    else
        printf "  FAIL §165 %s 没有迁移提示\n" "$vgone"
        head -6 "$TPFX.$vgone.log"; fail=1
    fi
done

$ZXC tests/good/std-simd.zx --emit-c > "$TPFX.s165.c" 2>/dev/null
if grep -qF 'vector_size' "$TPFX.s165.c" && ! grep -qE '_mm[0-9]*_' "$TPFX.s165.c"; then
    printf "  ok   §165 std.simd 是纯 Zx（没有 C shim / intrinsic）\n"
else
    printf "  FAIL §165 std.simd 仍挂着 C 实现\n"; fail=1
fi

if grep -qF 'mask: sel=1,2,3,7 any=true all=false' "$TPFX.s159.dev" \
   && grep -qF 'mask edges: allT=true anyF=false allF=false' "$TPFX.s159.dev"; then
    printf "  ok   §165 select / any / all 的语义正确（半真 / 全 0 / 全 -1）\n"
else
    printf "  FAIL §165 掩码函数语义不对\n"
    grep -n 'mask' "$TPFX.s159.dev" | head -4; fail=1
fi

# §166：前端 spawn 不借命令解释器的 `2>` 重定向（CreateProcess + stderr 句柄
# 直连日志）。以前被编辑器拉起时那条链会静默失败，连正常的 `@mod("std")`
# 都被标一句 "the Zx frontend failed without a diagnostic ... compiler bug"。
# ZXC_NO_AST_CACHE=1 强制真跑一次 zxfe，不让缓存命中把这段掩盖过去。
cat > "$TPFX.s166mod.zx" <<'ZXEOF'
const std = @mod("std");
const nope = @mod("nope_s166");
pub const main = fn () i32 { std.println("{}", nope.x); return 0; };
ZXEOF
if ZXC_NO_AST_CACHE=1 $ZXC "$TPFX.s166mod.zx" --check > "$TPFX.s166mod.log" 2>&1; then
    printf "  FAIL §166 导入错误居然通过了\n"; fail=1
elif grep -qF 'error[E0601]' "$TPFX.s166mod.log" \
     && ! grep -qF 'failed without a diagnostic' "$TPFX.s166mod.log" \
     && ! grep -qF 'compiler bug' "$TPFX.s166mod.log"; then
    printf "  ok   §166 导入诊断正常（没有假的 compiler bug）\n"
else
    printf "  FAIL §166 前端 spawn 诊断不对\n"
    head -6 "$TPFX.s166mod.log"; fail=1
fi

printf 'const x = fn ( {\n' > "$TPFX.s166parse.zx"
if ZXC_NO_AST_CACHE=1 $ZXC "$TPFX.s166parse.zx" --check > "$TPFX.s166parse.log" 2>&1; then
    printf "  FAIL §166 语法错误居然通过了\n"; fail=1
elif grep -qF 'error[E0201]' "$TPFX.s166parse.log" \
     && ! grep -qF 'failed without a diagnostic' "$TPFX.s166parse.log"; then
    printf "  ok   §166 缓存失效时前端诊断原样回放\n"
else
    printf "  FAIL §166 缓存失效时前端诊断丢失\n"
    head -6 "$TPFX.s166parse.log"; fail=1
fi

# §167：Mat3 / Mat4 与四元数互转。守的是"通用求逆"和 Shoemake 四个分支，
# 不是只测单位矩阵：gen 是非仿射的一般 4x4、persp 是投影矩阵，两者都能
# 求逆往返才说明伴随矩阵没有偷偷退化成仿射专用公式；sing 钉住奇异契约
# （det=0 -> 零矩阵）；q2m / m2q 各四个 true 覆盖 trace>0 与 x/y/z 分支。
if grep -qF 'mat4 gen: det=15 round=true' "$TPFX.s159.dev" \
   && grep -qF 'mat4 persp: f=true c22=true c23=true round=true' "$TPFX.s159.dev" \
   && grep -qF 'mat4 sing: det=0 zero=true' "$TPFX.s159.dev" \
   && grep -qF 'mat3 tr: 4 6 1 det=1' "$TPFX.s159.dev" \
   && grep -qF 'mat3 inv: det=24 round=true' "$TPFX.s159.dev" \
   && grep -qF 'q2m: true true true true' "$TPFX.s159.dev" \
   && grep -qF 'm2q: true true true true' "$TPFX.s159.dev" \
   && grep -qF 'mat3<->mat4: true true' "$TPFX.s159.dev"; then
    printf "  ok   §167 矩阵 / 四元数互转（通用求逆 · 四分支 · 奇异契约）\n"
else
    printf "  FAIL §167 矩阵语义回归\n"
    grep -E '^mat|^q2m|^m2q' "$TPFX.s159.dev" | head -20; fail=1
fi

# §197：字符串字面量 .ptr 是编译期常量——静态初始化成裸指针，不走 g_late。
# 曾经 is_view_expr 把它当视图 → `static uint8_t *P = {0}` + 运行期赋值；
# val_init 曾把它物化成 `{ptr, len}` 切片形状（与裸指针元素类型不符）。
$ZXC tests/good/string-ptr-const.zx --emit-c > "$TPFX.s197.c" 2>/dev/null
# §201 后全局 [N]T 也走 `struct { T v[N]; }`，不再是裸 C 数组；A 是指向
# 静态 backing 的指针，backing 里装两个字符串首字节指针。
if grep -qF 'static uint8_t * P = ((uint8_t*)"hello");' "$TPFX.s197.c" \
   && grep -qF '((uint8_t*)"aa"), ((uint8_t*)"bb")' "$TPFX.s197.c" \
   && grep -qF 'static ZxArr_apu8_2 * A = &' "$TPFX.s197.c" \
   && ! grep -qE '^[[:space:]]+P = ' "$TPFX.s197.c"; then
    printf "  ok   §197 字符串 .ptr 静态初始化（非 g_late 运行期赋值）\n"
else
    printf "  FAIL §197 字符串 .ptr 编译期常量发射形状\n"
    grep -nE 'static uint8_t \* P|static uint8_t \* A|\* P =' "$TPFX.s197.c" | head -12
    fail=1
fi

# §198 codegen：泛型体内"不依赖泛型参数"的嵌套 struct 跨实例共享身份——
# 后端只发一份 struct{int32_t x;}。修前 Body(i32)/Body(u8) 各发一份同构
# typedef（ZxS2/ZxS4），这里按发射形状钉死"只生成一份"。
$ZXC tests/good/generic-nested-independent-type.zx --emit-c > "$TPFX.s198.c" 2>/dev/null
nfixed=$(grep -c 'int32_t x;' "$TPFX.s198.c" || true)
if [ "$nfixed" = "1" ]; then
    printf "  ok   §198 泛型独立嵌套类型只发一份 C typedef\n"
else
    printf "  FAIL §198 泛型嵌套类型生成了 %s 份同构 typedef（应为 1）\n" "$nfixed"
    grep -nE 'int32_t x;|typedef struct ZxS' "$TPFX.s198.c" | head -16
    fail=1
fi

# §199 codegen：全局 []const T = &.{...} 必须发成"静态 backing 数组 +
# {ptr,len} slice"，不能把元素直接塞进 slice 结构体（修前 B1002）。
$ZXC tests/good/global-slice-literal.zx --emit-c > "$TPFX.s199.c" 2>/dev/null
if grep -qE 'static int32_t _ignored_[0-9]+\[3\] = \{ 3, 5, 7 \};' "$TPFX.s199.c" \
   && grep -qE 'static ZxSlice_i32 G_I = \{ \(int32_t\*\)_ignored_[0-9]+, 3 \};' "$TPFX.s199.c" \
   && grep -qE 'static ZxSlice_ZxS[0-9]+ G_P = \{ \(ZxS[0-9]+\*\)_ignored_[0-9]+, 2 \};' "$TPFX.s199.c"; then
    printf "  ok   §199 全局 slice 字面量发成 backing 数组 + {ptr,len}\n"
else
    printf "  FAIL §199 全局 slice 字面量 C 形状不对（应为 backing 数组 + {ptr,len}）\n"
    grep -nE 'static .* G_[IPS]' "$TPFX.s199.c" | head -12
    fail=1
fi

# §192：@shim 通道已删除（写它报 E0211，见 tests/bad/shim-removed.zx）；
# 本块的「--emit-c 末尾贴 shim / C 前端通过」两项检查随之移除。

# §192：自举对拍（C 前端 vs Zx 前端）纳入回归。两套解析器对同一输入的
# AST dump（含位置）必须逐字节一致——曾经 17 处差异不在任何测试里
# （2026-11-20 评测揪出：类型点路径列号错位、@Vec dump 形状、enum 体
# pub 方法丢 pub 标记）。astdump.exe 不入 git 白名单：先编一份再对拍。
# 必须每次重建：astdump 自己内嵌一份 zx 前端，zxc 升级后旧 exe 解析不了
# `[N]T` 这类新语法，对拍会刷出几十处假差异（2026-11-21 §201 迁移踩到）。
if $ZXC bootstrap/astdump.zx -o bin/astdump.exe > "$TPFX.astdump.log" 2>&1; then
    printf "  ok   构建 astdump（对拍工具）\n"
else
    printf "  FAIL astdump 构建失败\n"; cat "$TPFX.astdump.log"; fail=1
fi
printf "\n== 自举对拍（C 前端 vs Zx 前端，AST 含位置逐字节一致）\n"
ndiff=$(sh bootstrap/diff_ast.sh 2>/dev/null | grep -c "^DIFF\|^REJECT-ONLY-ONE" || true)
if [ "$ndiff" = "0" ]; then
    printf "  ok   双前端对拍 0 差异\n"
else
    printf "  FAIL 双前端对拍 %s 处差异（sh bootstrap/diff_ast.sh 看明细）\n" "$ndiff"
    fail=1
fi

# §234：词法层对拍（C lexer vs Zx lexer）。此前 diff_lex.sh 是孤儿脚本，
# 而且 lexdump.exe 从没有人构建——接进回归，与 AST 对拍互补：token 级
# 漂移会在 AST 之前暴露。必须每次重建 lexdump（自带一份 Zx 词法器）。
if $ZXC bootstrap/lexdump.zx -o bin/lexdump.exe > "$TPFX.lexdump.log" 2>&1; then
    printf "  ok   构建 lexdump（词法对拍工具）\n"
else
    printf "  FAIL lexdump 构建失败\n"; cat "$TPFX.lexdump.log"; fail=1
fi
lexdiff=$(sh bootstrap/diff_lex.sh 2>/dev/null | tail -1)
if printf '%s' "$lexdiff" | grep -q '^lex diff: [0-9]* identical, 0 differ'; then
    printf "  ok   双前端词法对拍（%s）\n" "$lexdiff"
else
    printf "  FAIL 双前端词法对拍：%s（sh bootstrap/diff_lex.sh 看明细）\n" "$lexdiff"
    fail=1
fi

# §237：错误路径双前端对拍。每个 bad / multibad 用例分别喂 C 前端和 Zx 前端，
# 比较首个诊断码——同一个根因两个前端必须报同一个码（后续恢复诊断允许不同）。
# astdump 不能干这事：它不查 parser 错误状态，会产出残缺 AST 造成假差异。
errdiff=$(sh bootstrap/diff_errors.sh 2>/dev/null | tail -1)
if printf '%s' "$errdiff" | grep -q '^error-path diff: [0-9]* identical, 0 mismatched'; then
    printf "  ok   %s\n" "$errdiff"
else
    printf "  FAIL 错误路径双前端对拍：%s（sh bootstrap/diff_errors.sh 看明细）\n" "$errdiff"
    fail=1
fi

# §208：多平台目标（配置驱动 + 后台生成）。
# 守三件事：targets 预设齐全、zx.json targets 能驱动三目标交叉构建、
# 产物 magic 分别是 PE/ELF/Mach-O。运行需目标机，这里只验到编译+链接。
printf "\n== §208 多平台交叉构建（windows/linux/macos）\n"
tgt_out=$($ZXC targets 2>/dev/null)
if printf '%s' "$tgt_out" | grep -q 'linux-aarch64' \
   && printf '%s' "$tgt_out" | grep -q 'macos-aarch64' \
   && printf '%s' "$tgt_out" | grep -q 'windows-x86_64'; then
    printf "  ok   zxc targets 列出 6 个预设\n"
else
    printf "  FAIL zxc targets 预设不全\n"; fail=1
fi
P208="$ROOT/.tmp/p208.$$"
rm -rf "$P208"; mkdir -p "$P208/src"
cat > "$P208/zx.json" <<'ZJEOF'
{"name":"p208","main":"src/main.zx","out":"p208",
 "targets":["windows-x86_64","linux-x86_64","macos-aarch64"]}
ZJEOF
cat > "$P208/src/main.zx" <<'ZXEOF'
const std = @mod("std");
const main = fn () i32 {
    std.println("{}", std.time.unixMs());
    return 0;
};
ZXEOF
(
    cd "$P208"
    ZX_STD="$ROOT/std" "$ROOT/$ZXC" build --release > "$TPFX.p208.log" 2>&1
)
if [ $? -eq 0 ] \
   && [ -f "$P208/out/p208.exe" ] \
   && [ -f "$P208/out/linux-x86_64/p208" ] \
   && [ -f "$P208/out/macos-aarch64/p208" ]; then
    mz=$(od -An -tx1 -N2 "$P208/out/p208.exe" 2>/dev/null | tr -d ' ')
    elf=$(od -An -tx1 -N4 "$P208/out/linux-x86_64/p208" 2>/dev/null | tr -d ' ')
    mach=$(od -An -tx1 -N4 "$P208/out/macos-aarch64/p208" 2>/dev/null | tr -d ' ')
    if [ "$mz" = "4d5a" ] && [ "$elf" = "7f454c46" ] && [ "$mach" = "cffaedfe" ]; then
        printf "  ok   三目标产物 magic 正确 (PE/ELF/Mach-O)\n"
    else
        printf "  FAIL magic: mz=%s elf=%s mach=%s\n" "$mz" "$elf" "$mach"; fail=1
    fi
    if [ -f "$P208/.zxc-cache/platform/linux-x86_64/std/platform.zx" ]; then
        printf "  ok   std 平台镜像 + platform.zx 已生成\n"
    else
        printf "  FAIL std 平台镜像缺失\n"; fail=1
    fi
else
    printf "  FAIL §208 多目标构建失败\n"; tail -12 "$TPFX.p208.log"; fail=1
fi
rm -rf "$P208"

# §211：运行时诊断走 CRT 无缓冲写 + 崩溃自动打点（报 zx 位置，不是 C 行号）。
# 守三件事：显式 panic 只有一条诊断（崩溃 handler 不重复报）；裸 @trap 的
# 崩溃 handler 打点含 zx 文件:行；崩溃退出码仍是 abort 的 3。
printf "\n== §211 CRT 无缓冲诊断 + 崩溃自动打点\n"
$ZXC tests/good/debug-alloc-abort.zx -o "$TPFX.d211.exe" > /dev/null 2>&1 || true
"$TPFX.d211.exe" leak > "$TPFX.d211.panic" 2>&1 || true
if grep -q 'panic: \[debug_alloc\] memory leak' "$TPFX.d211.panic" \
   && grep -Eq 'at .*debug_alloc\.zx:[0-9]+:[0-9]+ in deinit' "$TPFX.d211.panic" \
   && ! grep -q 'zx: fatal' "$TPFX.d211.panic"; then
    printf "  ok   显式 panic 单条诊断，无重复崩溃报告\n"
else
    printf "  FAIL §211 显式 panic 打点\n"; sed -n '1,10p' "$TPFX.d211.panic"; fail=1
fi
cat > "$TPFX.c211.zx" <<'ZXEOF'
const main = fn () i32 {
    let a: i32 = 1;
    let b: i32 = 2;
    let c = a + b;
    if (c == 3) {
        @trap();
    }
    return 0;
};
ZXEOF
$ZXC "$TPFX.c211.zx" -o "$TPFX.c211.exe" > /dev/null 2>&1 || true
"$TPFX.c211.exe" > "$TPFX.c211.run" 2>&1 || true
if grep -q 'zx: fatal SIGABRT' "$TPFX.c211.run" \
   && grep -q 'c211.zx:6:9' "$TPFX.c211.run"; then
    printf "  ok   崩溃 handler 打点含 zx 文件:行（非 C 行号）\n"
else
    printf "  FAIL §211 崩溃打点位置\n"; cat "$TPFX.c211.run"; fail=1
fi

# §213：break = 最近外循环的收割糖（空值/带值/无标签循环）；
# @panic 走 CRT 无缓冲单条诊断，报 zx 位置，crash handler 不补 fatal。
printf "\n== §213 break 收割糖 + @panic 内建\n"
if $ZXC tests/good/break-loop.zx -o "$TPFX.c213break.exe" > /dev/null 2>&1 \
   && "$TPFX.c213break.exe" > "$TPFX.c213break.run" 2>&1; then
    printf "  ok   break 空/带值收割最近外循环\n"
else
    printf "  FAIL §213 break 收割\n"
    cat "$TPFX.c213break.run" 2>/dev/null || true
    fail=1
fi
cat > "$TPFX.c213.zx" <<'ZXEOF'
const main = fn () i32 {
    let a: i32 = 1;
    let b: i32 = 2;
    let c = a + b;
    if (c == 3) {
        @panic("boom");
    }
    return 0;
};
ZXEOF
$ZXC "$TPFX.c213.zx" -o "$TPFX.c213.exe" > /dev/null 2>&1 || true
"$TPFX.c213.exe" > "$TPFX.c213.run" 2>&1 || true
if grep -q 'panic: boom' "$TPFX.c213.run" \
   && grep -q 'c213.zx:6:9' "$TPFX.c213.run" \
   && ! grep -q 'zx: fatal' "$TPFX.c213.run"; then
    printf "  ok   @panic 单条无缓冲诊断，带 zx 位置\n"
else
    printf "  FAIL §213 @panic 打点\n"; cat "$TPFX.c213.run"; fail=1
fi
cat > "$TPFX.c213b.zx" <<'ZXEOF'
const main = fn () i32 {
    let a: i32 = 1;
    let b: i32 = 2;
    let c = a + b;
    if (c == 3) {
        @panic();
    }
    return 0;
};
ZXEOF
$ZXC "$TPFX.c213b.zx" -o "$TPFX.c213b.exe" > /dev/null 2>&1 || true
"$TPFX.c213b.exe" > "$TPFX.c213b.run" 2>&1 || true
if grep -q '^panic$' "$TPFX.c213b.run" \
   && grep -q 'c213b.zx:6:9' "$TPFX.c213b.run" \
   && ! grep -q 'zx: fatal' "$TPFX.c213b.run"; then
    printf "  ok   @panic() 无消息也带位置\n"
else
    printf "  FAIL §213 @panic() 无消息\n"; cat "$TPFX.c213b.run"; fail=1
fi

# §214：zxc test --file 按测试文件路径过滤（位置参数仍按函数名，可组合）。
printf "\n== §214 zxc test --file 文件过滤\n"
HAD_ZXJSON=0; [ -e zx.json ] && HAD_ZXJSON=1
HAD_TESTDIR=0; [ -d src/test ] && HAD_TESTDIR=1
[ "$HAD_ZXJSON" = 1 ] && cp zx.json "$TPFX.zxjson.bak" 2>/dev/null || true
mkdir -p src/test
cat > src/test/zz_filesel_a.zx <<'ZXEOF'
pub const alpha_case = fn () bool { return true; };
ZXEOF
cat > src/test/zz_filesel_b.zx <<'ZXEOF'
pub const beta_case = fn () bool { return true; };
ZXEOF
[ "$HAD_ZXJSON" = 1 ] || printf '{"name":"zxtest"}\n' > zx.json
FS_OUT=$($ZXC test --file zz_filesel_a 2>&1) || true
if printf '%s' "$FS_OUT" | grep -q 'PASS.*alpha_case' \
   && ! printf '%s' "$FS_OUT" | grep -q 'beta_case'; then
    printf "  ok   --file 只跑匹配的测试文件\n"
else
    printf "  FAIL §214 --file 过滤\n"; printf '%s\n' "$FS_OUT"; fail=1
fi
FS_OUT=$($ZXC test --file zz_filesel_a beta 2>&1) || true
if printf '%s' "$FS_OUT" | grep -q 'no cases matched' \
   && ! printf '%s' "$FS_OUT" | grep -q 'PASS'; then
    printf "  ok   --file + 名字过滤可组合（无交集则明确提示）\n"
else
    printf "  FAIL §214 --file + 名字组合\n"; printf '%s\n' "$FS_OUT"; fail=1
fi
rm -f src/test/zz_filesel_a.zx src/test/zz_filesel_b.zx zx.json
rm -f out/test_zz_filesel_a.exe out/test_zz_filesel_b.exe
rm -f .zxc-cache/zz_filesel_* .zxc-cache/test_zz_filesel_*
[ "$HAD_ZXJSON" = 1 ] && cp "$TPFX.zxjson.bak" zx.json 2>/dev/null || rm -f zx.json
[ "$HAD_TESTDIR" = 1 ] || rmdir src/test 2>/dev/null || true

# §216：非简单基址（链式切片）下标必须带运行时越界检查；写入/取地址不回归。
printf "\n== §216 链式切片下标越界检查\n"
cat > "$TPFX.c216.zx" <<'ZXEOF'
const std = @mod("std");
const main = fn () i32 {
    var b: [2]i32 = .{ 1, 2 };
    let p = &b[0];
    var i: usize = 5;
    if (std.process.args().len > 99) { i = 0; }
    std.println("{}", p[0..2][i]);
    return 0;
};
ZXEOF
$ZXC "$TPFX.c216.zx" -o "$TPFX.c216.exe" > /dev/null 2>&1 || true
"$TPFX.c216.exe" > "$TPFX.c216.run" 2>&1 || true
if grep -q 'index 5 is out of bounds (len 2)' "$TPFX.c216.run" \
   && grep -q 'c216.zx:7' "$TPFX.c216.run"; then
    printf "  ok   链式切片读越界报运行时诊断（zx 位置）\n"
else
    printf "  FAIL §216 链式切片越界\n"; cat "$TPFX.c216.run"; fail=1
fi
cat > "$TPFX.c216b.zx" <<'ZXEOF'
const main = fn () i32 {
    var b: [2]i32 = .{ 1, 2 };
    let p = &b[0];
    p[0..2][1] = 42;
    let s = p[0..2];
    if (b[1] != 42) { return 1; }
    if (s[1] != 42) { return 2; }
    let q = &p[0..2][0];
    q.0 = 9;
    if (b[0] != 9) { return 3; }
    return 0;
};
ZXEOF
if $ZXC "$TPFX.c216b.zx" -o "$TPFX.c216b.exe" > /dev/null 2>&1 \
   && "$TPFX.c216b.exe" > "$TPFX.c216b.run" 2>&1; then
    printf "  ok   链式切片写入/取地址与简单基址不回归\n"
else
    printf "  FAIL §216 链式切片写入\n"; cat "$TPFX.c216b.run" 2>/dev/null || true; fail=1
fi

# §217：读真正非法地址时崩溃 handler 报 zx 坐标 + 转发 C/OS 的故障地址。
printf "\n== §217 崩溃转发 C 故障地址\n"
cat > "$TPFX.c217.zx" <<'ZXEOF'
const std = @mod("std");
const main = fn () i32 {
    let p: *i32 = (*i32)(0);
    std.println("{}", p[0]);
    return 0;
};
ZXEOF
if $ZXC "$TPFX.c217.zx" -o "$TPFX.c217.exe" > /dev/null 2>&1; then
    "$TPFX.c217.exe" > "$TPFX.c217.run" 2>&1 || true
    if grep -q 'zx: fatal SIGSEGV' "$TPFX.c217.run" \
       && grep -q 'c217.zx:4:16' "$TPFX.c217.run" \
       && grep -qE '\[fault addr [0-9a-fA-F]+\]' "$TPFX.c217.run"; then
        printf "  ok   非法地址崩溃报 zx 坐标 + C 故障地址\n"
    else
        printf "  FAIL §217 故障地址转发\n"; cat "$TPFX.c217.run"; fail=1
    fi
else
    printf "  FAIL §217 编译失败\n"; fail=1
fi

# §218：[]u8 与 []const u8 是两个 Type*，但都映射成 C 名 ZxSlice_u8。
# 非简单基址下标各请求一次 idx helper，收集口若按 Type* 去重就会把同名
# 原型+定义各发两遍，cc 报 redefinition（B1003）。按 C 名去重后应只剩一份。
printf "\n== §218 同名 C 类型的 slice helper 去重\n"
cat > "$TPFX.c218.zx" <<'ZXEOF'
const mut = fn (s: []u8) []u8 { return s; };
const con = fn (s: []const u8) []const u8 { return s; };
const main = fn () i32 {
    var buf: [4]u8 = .{ 'a', 'b', 'c', 'd' };
    let m: []u8 = buf[0..4];
    let c: []const u8 = buf[0..4];
    let a = mut(m)[1];
    let b = con(c)[2];
    if (a != 'b') { return 1; }
    if (b != 'c') { return 2; }
    return 0;
};
ZXEOF
$ZXC "$TPFX.c218.zx" --emit-c > "$TPFX.c218.c" 2>/dev/null || true
NIDX=$(grep -Fc 'uint8_t *zx_idx_ZxSlice_u8(ZxSlice_u8 s, long long i, const char *loc) {' "$TPFX.c218.c")
NIDXU=$(grep -Fc 'uint8_t *zx_idxu_ZxSlice_u8(ZxSlice_u8 s, uint64_t i, const char *loc) {' "$TPFX.c218.c")
if [ "$NIDX" = 1 ] && [ "$NIDXU" = 1 ] \
   && $ZXC "$TPFX.c218.zx" -o "$TPFX.c218.exe" > "$TPFX.c218.log" 2>&1 \
   && "$TPFX.c218.exe"; then
    printf "  ok   []u8/[]const u8 同名 slice 只发一份 helper（无 B1003）\n"
else
    printf "  FAIL §218 slice helper 去重（def idx=%s idxu=%s）\n" "$NIDX" "$NIDXU"
    cat "$TPFX.c218.log" 2>/dev/null || true; fail=1
fi

# §222 用户裁定：移除 §219 的 at() 单元素指针长切片限制。
# at() 返回 ?*T，拿它造 `p[0..n]` 此前报 E0531；现在完全放行，长度由写码
# 者自己负责（越界后果自负），编译器不拦。守门反转为「必须能编译」；
# items() 的 .len 运行时兜底与 alloc() 裸指针通道照旧验证。
printf "\n== §222 移除 at() 长切片限制\n"
cat > "$TPFX.c219.zx" <<'ZXEOF'
const std = @mod("std");
const main = fn () i32 {
    var list = std.array.List(i32).init(std.heap.page);
    defer list.deinit();
    _ = list.push(200);
    let plist: *i32 = list.at(0).?;
    return (i32)plist[0..100][99];
};
ZXEOF
$ZXC "$TPFX.c219.zx" -o "$TPFX.c219.exe" > "$TPFX.c219.log" 2>&1 && ok219=1 || ok219=0
if [ "$ok219" = 1 ]; then
    printf "  ok   at().? 的 let 中转长切片已放行\n"
else
    printf "  FAIL §222 let 中转仍被拦\n"; cat "$TPFX.c219.log"; fail=1
fi
cat > "$TPFX.c219b.zx" <<'ZXEOF'
const std = @mod("std");
const main = fn () i32 {
    var list = std.array.List(i32).init(std.heap.page);
    defer list.deinit();
    _ = list.push(200);
    return (i32)list.at(0).?[0..100][99];
};
ZXEOF
$ZXC "$TPFX.c219b.zx" -o "$TPFX.c219b.exe" > "$TPFX.c219b.log" 2>&1 && ok219b=1 || ok219b=0
if [ "$ok219b" = 1 ]; then
    printf "  ok   直接 at().?[0..N] 已放行\n"
else
    printf "  FAIL §222 直接形式仍被拦\n"; cat "$TPFX.c219b.log"; fail=1
fi
# items() 是正当兜底：编译过、运行时按 .len 界检查报非 0
cat > "$TPFX.c219c.zx" <<'ZXEOF'
const std = @mod("std");
const main = fn () i32 {
    var list = std.array.List(i32).init(std.heap.page);
    defer list.deinit();
    _ = list.push(200);
    return (i32)list.items()[0..100][99];
};
ZXEOF
if $ZXC "$TPFX.c219c.zx" -o "$TPFX.c219c.exe" > /dev/null 2>&1; then
    "$TPFX.c219c.exe" > "$TPFX.c219c.run" 2>&1 || true
    if grep -q 'out of bounds' "$TPFX.c219c.run"; then
        printf "  ok   items() 运行时按 .len 兜底越界\n"
    else
        printf "  FAIL §219 items() 未兜底\n"; cat "$TPFX.c219c.run"; fail=1
    fi
else
    printf "  FAIL §219 items() 编译失败\n"; fail=1
fi
# alloc() 返回的裸指针切固定长度是合法的分配器通道，不误伤
cat > "$TPFX.c219d.zx" <<'ZXEOF'
const std = @mod("std");
const main = fn () i32 {
    let raw = std.heap.page.alloc(std.heap.page.ctx, 24);
    if (raw) b {
        defer std.heap.page.free(std.heap.page.ctx, b, 24);
        let s = b[0..24];
        return (i32)s.len - 24;
    }
    return 1;
};
ZXEOF
if $ZXC "$TPFX.c219d.zx" -o "$TPFX.c219d.exe" > "$TPFX.c219d.log" 2>&1 \
   && "$TPFX.c219d.exe"; then
    printf "  ok   alloc() 裸指针 b[0..24] 不误伤\n"
else
    printf "  FAIL §219 alloc 误伤\n"; cat "$TPFX.c219d.log"; fail=1
fi

# §220：orelse 单语句兜底臂（不写 {}）——return / break / continue / 标签收割。
# 分号属于外层 let；块臂形状与 §200 的 `{ return ... }` 一致，sema/后端复用发散臂。
printf "\n== §220 orelse 单语句兜底臂 + 标签收割 + inlay\n"
cat > "$TPFX.c220.zx" <<'ZXEOF'
const mk = fn (x: i32) ?i32 { if (x < 0) return null; return x; };

// return 臂：optional + error union 两种
const ret = fn (o: ?i32) i32 { let v = o orelse return 99; return v + 1; };
const ef = fn (x: i32) !i32 { if (x < 0) return error.Bad; return x; };
const eret = fn (x: i32) i32 { let v = ef(x) orelse return -1; return v + 1; };

const main = fn () i32 {
    if (ret(5) != 6) { return 1; }
    if (ret(null) != 99) { return 2; }
    if (eret(5) != 6) { return 3; }
    if (eret(-1) != -1) { return 4; }
    // break 臂
    var i: i32 = 0; var acc: i32 = 0;
    while (i < 5) {
        acc = acc + 1;
        let v = mk(i - 2) orelse break;
        acc = acc + v;
        i = i + 1;
    }
    if (acc != 1) { return 5; }
    // continue 臂
    i = 0; acc = 0;
    while (i < 5) {
        i = i + 1;
        let v = mk(2 - i) orelse continue;
        acc = acc + v;
    }
    if (acc != 1) { return 6; }
    // 标签收割臂：orelse out: 7 跳出标签块并带回 7
    let g = out: {
        let v = mk(-1) orelse out: 7;
        out: v + 1;
    };
    if (g != 7) { return 7; }
    // 值臂不误伤
    let d = mk(3) orelse 42;
    if (d != 3) { return 8; }
    // §223：标签块是值（不是收割到外层 pick），且只在左臂为空时求值
    let lb = mk(-1) orelse pick: { pick: 77; };
    if (lb != 77) { return 9; }
    let lb2 = mk(4) orelse pick: { pick: 77; };
    if (lb2 != 4) { return 10; }
    var touched: i32 = 0;
    let lazy = mk(3) orelse lazy_pick: { touched = touched + 1; lazy_pick: 8; };
    if (touched != 0 or lazy != 3) { return 11; }
    return 0;
};
ZXEOF
if $ZXC "$TPFX.c220.zx" -o "$TPFX.c220.exe" > "$TPFX.c220.log" 2>&1 \
   && "$TPFX.c220.exe"; then
    printf "  ok   return/break/continue/标签收割/值臂 全覆盖\n"
else
    printf "  FAIL §220 单语句臂\n"; cat "$TPFX.c220.log"; fail=1
fi
# 双前端交叉验证：默认 Zx 前端已跑，这里再验 C 前端同构
if $ZXC --fe=c "$TPFX.c220.zx" -o "$TPFX.c220c.exe" > "$TPFX.c220c.log" 2>&1 \
   && "$TPFX.c220c.exe"; then
    printf "  ok   C 前端交叉验证通过\n"
else
    printf "  FAIL §220 C 前端交叉\n"; cat "$TPFX.c220c.log"; fail=1
fi

# §221：捕获指针 *t —— 写回原对象 + 栈借用禁逃逸（账本开关关不掉）。
printf "\n== §221 捕获指针 *t 写回与逃逸检查\n"
cat > "$TPFX.c221.zx" <<'ZXEOF'
const std = @mod("std");
const Shape = union(enum) { circle: f64, square: i32 };
const main = fn () i32 {
    // if 捕获：写回 optional 载荷
    var a: ?i32 = 1;
    if (a) *t { t[0] = 9; };
    if (a.? != 9) { return 1; }
    // 别名读载荷（t[0] 是值，不是借用，不许误判成逃逸）
    if (a) *t2 { let v = t2[0] + 1; if (v != 10) { return 2; } };
    // while 捕获：逐轮写回
    var b: ?i32 = 1;
    var n: i32 = 0;
    while (b) *w { w[0] = w[0] + 10; n = n + 1; if (n == 3) { b = null; } };
    if (n != 3) { return 3; }
    // switch 捕获：写回变体载荷
    var s: Shape = Shape{ .square = 3 };
    switch (s) { .square: *v { v[0] = 77; }; .circle: {}; };
    if (s.square != 77) { return 4; }
    // flat ?*T 捕获：t 是 **T，写回让 optional 改指
    var x: i32 = 5;
    var y: i32 = 88;
    var p: ?*i32 = &x;
    if (p) *f { f[0] = &y; };
    if (p.?[0] != 88) { return 5; }
    return 0;
};
ZXEOF
if $ZXC "$TPFX.c221.zx" -o "$TPFX.c221.exe" > "$TPFX.c221.log" 2>&1 \
   && "$TPFX.c221.exe"; then
    printf "  ok   if/while/switch/flat 写回 + 别名读载荷\n"
else
    printf "  FAIL §221 写回\n"; cat "$TPFX.c221.log"; fail=1
fi
# 双前端交叉
if $ZXC --fe=c "$TPFX.c221.zx" -o "$TPFX.c221c.exe" > "$TPFX.c221c.log" 2>&1 \
   && "$TPFX.c221c.exe"; then
    printf "  ok   C 前端交叉验证通过\n"
else
    printf "  FAIL §221 C 前端交叉\n"; cat "$TPFX.c221c.log"; fail=1
fi
# 逃逸出口全拦：每个用例期望 E0532；账本开着（tag.enable）的模块也要拦
cap_escape() {
    name=$1; body=$2
    printf '%s\n' "$body" > "$TPFX.c221e.zx"
    if $ZXC "$TPFX.c221e.zx" > "$TPFX.c221e.log" 2>&1; then
        printf "  FAIL §221 逃逸未拦: %s\n" "$name"; cat "$TPFX.c221e.log"; fail=1
    elif ! grep -q "E0532" "$TPFX.c221e.log"; then
        printf "  FAIL §221 逃逸诊断码不符: %s\n" "$name"; cat "$TPFX.c221e.log"; fail=1
    else
        printf "  ok   逃逸拦截: %s\n" "$name"
    fi
}
cap_escape return 'const main = fn () ?*i32 { var o: ?i32 = 42; if (o) *t { return t; }; return null; };'
cap_escape arg 'const use = fn (p: *i32) i32 { return p[0]; }; const main = fn () void { var o: ?i32 = 42; if (o) *t { let x = use(t); }; };'
cap_escape field 'const Box = struct { p: ?*i32 = null }; const main = fn () void { var o: ?i32 = 42; var b = Box{}; if (o) *t { b.p = t; }; };'
cap_escape global 'var g: ?*i32 = null; const main = fn () void { var o: ?i32 = 42; if (o) *t { g = t; }; };'
cap_escape alias 'const main = fn () ?*i32 { var o: ?i32 = 42; if (o) *t { let u = t; return u; }; return null; };'
cap_escape non-lvalue 'const mk = fn () ?i32 { return 42; }; const main = fn () void { if (mk()) *t { }; };'
cap_escape ledger-on 'tag.enable; const main = fn () ?*i32 { var o: ?i32 = 42; if (o) *t { return t; }; return null; };'

# §222：and/or 是 &&/|| 的词法别名——同优先级、同短路、可混用；双前端各跑一遍
printf "\n== §222 and/or 现代布尔拼写\n"
cat > "$TPFX.c222.zx" <<'ZXEOF'
const main = fn () i32 {
    let t = true;
    let f = false;
    if (!(t and !f)) { return 1; }
    if (!(f or t)) { return 2; }
    if (t or f and f) {} else { return 3; }   // and 优先于 or
    if (f and t or t) {} else { return 4; }
    if (t and f or t) {} else { return 5; }
    return 0;
};
ZXEOF
if $ZXC "$TPFX.c222.zx" -o "$TPFX.c222.exe" > "$TPFX.c222.log" 2>&1 \
   && "$TPFX.c222.exe"; then
    printf "  ok   and/or 语义与优先级\n"
else
    printf "  FAIL §222 and/or 语义\n"; cat "$TPFX.c222.log"; fail=1
fi
if $ZXC --fe=c "$TPFX.c222.zx" -o "$TPFX.c222c.exe" > "$TPFX.c222c.log" 2>&1 \
   && "$TPFX.c222c.exe"; then
    printf "  ok   and/or C 前端交叉\n"
else
    printf "  FAIL §222 and/or C 前端\n"; cat "$TPFX.c222c.log"; fail=1
fi
# 旧拼写必须被拒（E0211）：两个前端各验一次
for oldop in '&&' '||'; do
    printf 'const main = fn () i32 { let a = true; let b = false; if (a %s b) { return 1; } return 0; };\n' "$oldop" > "$TPFX.c222old.zx"
    $ZXC "$TPFX.c222old.zx" -o "$TPFX.c222old.exe" > "$TPFX.c222old.log" 2>&1 && oldok=1 || oldok=0
    if [ "$oldok" = 0 ] && grep -q 'E0211' "$TPFX.c222old.log"; then
        printf "  ok   旧拼写 %s 报 E0211\n" "$oldop"
    else
        printf "  FAIL §222 旧拼写 %s 未拒绝\n" "$oldop"; cat "$TPFX.c222old.log"; fail=1
    fi
done

# §227：f16 编译期收敛 + f128 类型。R10 按目标精度判等（不是 double 判等）。
printf "\n== §227 f16 编译期舍入 + f128\n"
cat > "$TPFX.c224.zx" <<'ZXEOF'
const c16: f16 = 1.0 / 3.0;
const c128: f128 = 1.0 / 3.0;
const lit: f128 = 0.1f128;
const div16 = fn (a: f16, b: f16) f16 { return a / b; };
const div128 = fn (a: f128, b: f128) f128 { return a / b; };
const main = fn () i32 {
    var a16: f16 = 1.0;
    var b16: f16 = 3.0;
    var a128: f128 = 1.0;
    var b128: f128 = 3.0;
    if (c16 != div16(a16, b16)) { return 1; }
    if (c128 != div128(a128, b128)) { return 2; }
    if (lit != 0.1f128) { return 3; }
    if (@type(f128).size != 16) { return 4; }
    // 半精度真值 0x1.555p-2，不是 double 的 1/3
    if ((f32)c16 != 0.333251953125) { return 5; }
    return 0;
};
ZXEOF
if $ZXC "$TPFX.c224.zx" -o "$TPFX.c224.exe" > "$TPFX.c224.log" 2>&1 \
   && "$TPFX.c224.exe"; then
    printf "  ok   f16 ct=rt / f128 ct=rt / 后缀精度 / size=16\n"
else
    printf "  FAIL §227 f16/f128 语义\n"; cat "$TPFX.c224.log"; fail=1
fi
if $ZXC --fe=c "$TPFX.c224.zx" -o "$TPFX.c224c.exe" > "$TPFX.c224c.log" 2>&1 \
   && "$TPFX.c224c.exe"; then
    printf "  ok   C 前端交叉验证通过\n"
else
    printf "  FAIL §227 C 前端交叉\n"; cat "$TPFX.c224c.log"; fail=1
fi

# §225：标签账本默认关闭——tag.enable 才开账；tag.close 已删除。
printf "\n== §225 标签账本默认关闭 + tag.enable\n"
# ① 默认关闭：未贴标的堆分配泄漏不报 E0513（账本休眠）
printf 'const std = @mod("std");\npub const main = fn () u8 {\n    let p = std.heapAlloc(std.heap.page, i32, 4);\n    std.println("{}", p[0]);\n    return 0;\n};\n' > "$TPFX.c225off.zx"
if $ZXC "$TPFX.c225off.zx" --check > "$TPFX.c225off.log" 2>&1 \
   && ! grep -q 'E0513' "$TPFX.c225off.log"; then
    printf "  ok   默认关闭：未贴标泄漏不报 E0513\n"
else
    printf "  FAIL §225 默认未关闭\n"; cat "$TPFX.c225off.log"; fail=1
fi
# ② tag.enable：同一份源码开账后必须报 E0513
printf 'tag.enable;\nconst std = @mod("std");\npub const main = fn () u8 {\n    let p = std.heapAlloc(std.heap.page, i32, 4);\n    std.println("{}", p[0]);\n    return 0;\n};\n' > "$TPFX.c225on.zx"
if ! $ZXC "$TPFX.c225on.zx" --check > "$TPFX.c225on.log" 2>&1 \
   && grep -q 'E0513' "$TPFX.c225on.log"; then
    printf "  ok   tag.enable 开账后泄漏报 E0513\n"
else
    printf "  FAIL §225 tag.enable 未开账\n"; cat "$TPFX.c225on.log"; fail=1
fi
# ③ tag.close 已删：用了报 E0211（两个前端各验一次）
printf 'tag.close;\nconst main = fn () i32 { return 0; };\n' > "$TPFX.c225close.zx"
for fe in zx c; do
    $ZXC --fe=$fe "$TPFX.c225close.zx" --check > "$TPFX.c225close.log" 2>&1 && ck=0 || ck=1
    if [ "$ck" = 1 ] && grep -q 'E0211' "$TPFX.c225close.log" && grep -q 'gone' "$TPFX.c225close.log"; then
        printf "  ok   tag.close 删除（--fe=%s 报 E0211）\n" "$fe"
    else
        printf "  FAIL §225 tag.close 未删（--fe=%s）\n" "$fe"; cat "$TPFX.c225close.log"; fail=1
    fi
done
# ④ 开关按模块独立：main 不开账、helper 开账——只有 helper 的分配受检
mkdir -p "$TPFX.c225mod"
printf 'const h = @mod("h.zx");\npub const main = fn () i32 {\n    h.run();\n    return 0;\n};\n' > "$TPFX.c225mod/main.zx"
printf 'tag.enable;\nconst std = @mod("std");\npub const run = fn () void {\n    let p = std.heapAlloc(std.heap.page, i32, 4);\n    std.println("{}", p[0]);\n};\n' > "$TPFX.c225mod/h.zx"
if ! ( cd "$TPFX.c225mod" && "$ZXCW" main.zx --check > "$TPFX.c225mod.log" 2>&1 ) \
   && grep -q 'E0513' "$TPFX.c225mod.log"; then
    printf "  ok   开关按模块独立（helper 开账才报）\n"
else
    printf "  FAIL §225 开关非按模块独立\n"; cat "$TPFX.c225mod.log"; fail=1
fi

# §230：标准库 API 缺口——排序/搜索、str 新增、HashMap 放出 pub 方法。
printf "\n== §230 std.sort / str / map 新 API\n"
# 用仓库里的正式守门文件（runner 也跑它），双前端各编一次并核对输出。
for fe in zx c; do
    if $ZXC --fe=$fe tests/good/stdlib_collections.zx -o "$TPFX.coll$fe.exe" \
            > "$TPFX.coll$fe.log" 2>&1 \
       && "$TPFX.coll$fe.exe" > "$TPFX.coll$fe.out" 2>&1 \
       && grep -q '^str true$' "$TPFX.coll$fe.out" \
       && grep -q '^sort true$' "$TPFX.coll$fe.out" \
       && grep -q '^map true$' "$TPFX.coll$fe.out"; then
        printf "  ok   std 新 API（--fe=%s）\n" "$fe"
    else
        printf "  FAIL §230 std 新 API（--fe=%s）\n" "$fe"
        cat "$TPFX.coll$fe.log" "$TPFX.coll$fe.out" 2>/dev/null
        fail=1
    fi
done

# §231：跨模块泛型函数在辅助函数里实例化——旧实现里 ct_call 折叠零参数
# 辅助函数时会拿未绑定的泛型模板重新解析签名，误报 E0401 unknown type 'T'。
# 新守门文件由 runner 自动发现并跑输出断言；这里双前端各编一次并核对。
printf "\n== §231 跨模块泛型函数辅助函数内实例化\n"
for fe in zx c; do
    if $ZXC --fe=$fe tests/good/crossmod_generic_helper.zx -o "$TPFX.cg$fe.exe" \
            > "$TPFX.cg$fe.log" 2>&1 \
       && "$TPFX.cg$fe.exe" > "$TPFX.cg$fe.out" 2>&1 \
       && grep -q '^crossmod generic helper ok$' "$TPFX.cg$fe.out"; then
        printf "  ok   辅助函数内跨模块泛型函数（--fe=%s）\n" "$fe"
    else
        printf "  FAIL §231 辅助函数内跨模块泛型函数（--fe=%s）\n" "$fe"
        cat "$TPFX.cg$fe.log" "$TPFX.cg$fe.out" 2>/dev/null
        fail=1
    fi
done

# §232：内存分配 OOM 一律干净报错——核心分配点必须走 xmalloc/xcalloc/
# xrealloc/xstrdup（zxc.h 声明，zxc.c 统一打印 zxc: out of memory 后 exit）。
# 静态守门：扫 src/*.c 里裸 malloc/calloc/realloc/strdup，只允许三种情况：
#   ① 后端生成的 C 源码字符串（在字符串字面量里）
#   ② 紧随判空的库 API / 解码路径（NULL 是契约，不是疏漏）
#   ③ x* 包装自身的实现（zxc.c）
printf "\n== §232 分配 OOM 统一报错\n"
if python - <<'PY'
import re, glob, os, sys
pat = re.compile(r'\b(malloc|calloc|realloc|strdup)\s*\(')
bad = []
for f in sorted(glob.glob('src/*.c')):
    lines = open(f, encoding='utf-8', errors='replace').read().split('\n')
    for i, line in enumerate(lines):
        m = pat.search(line)
        if not m: continue
        # ③ x* 实现自身
        if os.path.basename(f)=='zxc.c' and ('void *xmalloc' in line or
           'void *xcalloc' in line or 'void *xrealloc' in line or
           'char *xstrdup' in line or 'char *p = strdup(s)' in line):
            continue
        # ① 字符串字面量里的（后端发射的 C 代码）
        if '"' in line[:m.start()] and line[:m.start()].count('"')%2==1: continue
        # ② 三行内有判空
        ctx='\n'.join(lines[i+1:i+4])
        if re.search(r'if\s*\(\s*!|==\s*NULL|!=\s*NULL',ctx): continue
        bad.append(f'{f}:{i+1}: {line.strip()[:80]}')
if bad:
    print('Unguarded raw allocations:')
    for x in bad[:20]: print('  '+x)
    sys.exit(1)
print('  ok   所有裸分配均有判空或契约保护')
PY
then
    printf "  ok   §232 静态扫描\n"
else
    printf "  FAIL §232 裸分配逃逸\n"; fail=1
fi
# 运行时守门：x* 实现必须打印 OOM 诊断并以非零退出（不是 segfault/静默）。
grep -q 'zxc: out of memory' src/zxc.c \
    && grep -q 'exit(1)' src/zxc.c \
    && printf "  ok   OOM 统一诊断 + exit(1)\n" \
    || { printf "  FAIL §232 OOM 诊断缺失\n"; fail=1; }

# §233：zxc fmt 的 token 间空格规范化（此前只归一缩进/尾空白/尾逗号，
# x=1 原样保留，不算正式格式化工具）。写一份全粘死的源码 → fmt → 断言
# 规范空格 + 双前端产物一致 + 幂等 + 产物可编译运行 + 字符串/注释内容不动。
printf "\n== §233 fmt token 空格规范化 + Zed 缩进查询\n"
mkdir -p "$pdir/fsp"
printf 'const Cfg=struct{a:i32,b:i32};\nconst add=fn(a:i32,b:i32)i32{return a+b;};\nconst main=fn()i32{\nvar x:i32=1;\nlet y:i32=x*2+add(3,4);\nif(x==y){x=-x;}else{x=x+1;}\nconst s:[]const u8="a=1 // not comment";\nconst c=Cfg{.a=1,.b=2};\n_=s;\n_=c;\nreturn 0;\n};\n' > "$pdir/fsp/a.zx"
cp "$pdir/fsp/a.zx" "$pdir/fsp/b.zx"
if ( cd "$pdir/fsp" && "$ZXCW" fmt a.zx > "$TPFX.fsp.log" 2>&1 ) \
   && grep -qxF 'const Cfg = struct { a: i32, b: i32 };' "$pdir/fsp/a.zx" \
   && grep -qxF 'const add = fn (a: i32, b: i32) i32 { return a + b; };' "$pdir/fsp/a.zx" \
   && grep -qxF '    let y: i32 = x * 2 + add(3, 4);' "$pdir/fsp/a.zx" \
   && grep -qxF '    if (x == y) { x = -x; } else { x = x + 1; }' "$pdir/fsp/a.zx" \
   && grep -qxF '    const s: []const u8 = "a=1 // not comment";' "$pdir/fsp/a.zx" \
   && grep -qxF '    const c = Cfg{ .a = 1, .b = 2 };' "$pdir/fsp/a.zx" \
   && grep -qxF '    _ = s;' "$pdir/fsp/a.zx" \
   && cp "$pdir/fsp/a.zx" "$TPFX.fsp.once" \
   && ( cd "$pdir/fsp" && "$ZXCW" fmt a.zx > /dev/null 2>&1 ) \
   && cmp -s "$pdir/fsp/a.zx" "$TPFX.fsp.once" \
   && ( cd "$pdir/fsp" && "$ZXCW" a.zx -o fsp.exe > "$TPFX.fsp.cc" 2>&1 ) \
   && ( cd "$pdir/fsp" && ./fsp.exe ); then
    printf "  ok   §233 token 空格规范化 + 幂等 + 产物可编译运行\n"
else
    printf "  FAIL §233 token 空格规范化\n"; cat "$TPFX.fsp.log" "$TPFX.fsp.cc" 2>/dev/null; fail=1
fi
# 字符串/注释内容一字不改（fmt 只动 token 间空白，绝不动字面量内部）。
if grep -qF '"a=1 // not comment"' "$pdir/fsp/a.zx"; then
    printf "  ok   §233 字符串内部保持原样\n"
else
    printf "  FAIL §233 字符串内容被改\n"; fail=1
fi
# Zed indents.scm：@end 必须嵌在 @indent 内（否则 `{}` 回车/vim o 回行首）。
if grep -qE '\(block[^\n]*"}" @end\) @indent' editor/zed/languages/zx/indents.scm \
   && ! grep -qE '^\] @end$' editor/zed/languages/zx/indents.scm \
   && grep -q 'tab_size = 4' editor/zed/languages/zx/config.toml; then
    printf "  ok   §233 Zed 缩进查询结构 + tab_size 4\n"
else
    printf "  FAIL §233 Zed indents 结构\n"; fail=1
fi

# §235：标准库第二轮补齐。tests/good 会自动发现并跑通该用例，这里额外
# 钉双前端——新模块（random/test）和 HashMap 墓碑逻辑都要两边一致。
printf "\n== §235 标准库补齐（random/test/sort/str/math/list/map.remove）\n"
for fe in zx c; do
    if $ZXC --fe=$fe tests/good/stdlib_more.zx -o "$TPFX.smore$fe.exe" \
        > "$TPFX.smore$fe.log" 2>&1 \
       && "$TPFX.smore$fe.exe" > /dev/null 2>&1; then
        printf "  ok   §235 --fe=%s 标准库新 API 编译运行通过\n" "$fe"
    else
        printf "  FAIL §235 --fe=%s 标准库新 API\n" "$fe"
        tail -8 "$TPFX.smore$fe.log"; fail=1
    fi
done

# §236：平台 std 镜像的对外 API 一致性。§208 已经守了「按 target 生成镜像」，
# 但没守两版 API 不漂移——Windows 上能编的跨平台代码，在 Linux 目标上可能
# 因为 POSIX 版少了某个 pub 而编不过。三个平台相关模块逐一比对 pub 名集合。
printf "\n== §236 平台 std 镜像 API 一致性（windows vs posix）\n"
for m in fs net time; do
    pfile="src/platform_std/posix/$m.zx"
    if [ ! -f "$pfile" ]; then
        printf "  FAIL §236 POSIX 实现缺失: %s\n" "$pfile"; fail=1
        continue
    fi
    wapi=$(grep -oE '^pub const [A-Za-z_][A-Za-z0-9_]*' "std/$m.zx" \
           | sed 's/pub const //' | sort -u)
    papi=$(grep -oE '^pub const [A-Za-z_][A-Za-z0-9_]*' "$pfile" \
           | sed 's/pub const //' | sort -u)
    if [ "$wapi" = "$papi" ]; then
        printf "  ok   §236 %s 两版 pub API 一致（%s 项）\n" "$m" "$(printf '%s\n' "$wapi" | grep -c .)"
    else
        printf "  FAIL §236 %s API 漂移\n" "$m"
        printf "       只在 windows:%s\n" "$(printf '%s\n' "$wapi" | grep -vxF "$papi" | tr '\n' ' ')"
        printf "       只在 posix:  %s\n" "$(printf '%s\n' "$papi" | grep -vxF "$wapi" | tr '\n' ' ')"
        fail=1
    fi
done

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
