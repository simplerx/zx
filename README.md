# zxc — Zx 语言编译器

Zx 是一门**自举的系统级编程语言**：编译器后端把 Zx 翻译成 C（用项目内 vendored 的
zig 的 `cc` 当 clang + 自带 libc 编出原生可执行文件），前端与标准库用 Zx 自身编写。
设计上追求「显式、可读、零隐藏开销」：没有 GC、没有运行时魔法，错误用返回值类型
`!T` / `?T` 显式表达，并发只有一个 `Task` 实例抽象。

- 规格：`spec/zx-spec.md`　·　文法：`spec/zx.ebnf`　·　语言手册：`docs/manual.md`
- 设计裁定记录（每个取舍为什么这么定）：`spec/decisions.md`
- 仓库：<https://gitcode.com/simplex/zx>

---

## 是什么

| 维度 | 现状 |
| --- | --- |
| 编译器 | `zxc`，C11 实现；前端（parser/sema）走 Zx 自举，后端是 C 发射器 |
| 后端 | 生成 C11，经 vendored zig `cc` 编成原生二进制（Windows 为主力开发平台，C 后端本身可移植） |
| 内存模型 | 手动管理 + 自建 arena；无 GC、无引用计数 |
| 错误处理 | `!T`（错误联合）与 `?T`（可选）是值类型，四种出口 `try` / `if (r) v else e` / `orelse` / `switch` |
| 并发 | 唯一抽象是 `std.Task` 实例：协程 / 单线程 / 多线程三种物理形态，实例自带通信 |
| 泛型 | 编译期单态化 |
| 模块 | 多文件 `pub` 模块 + `vendor/` 源码依赖 + `deps` 外部目录 |

---

## 现在到哪个程度

**版本 `1.0.0`。** 编译器自举可跑、标准库 8 个模块可用、并发 Task 实例与 rendezvous
通信已落地并压测稳定、Zed 编辑器有完整 IDE 能力、扩展库 `x/` 开张（稀疏表 / 多重数组 /
数学小件）。回归 `sh test.sh`：文件类 **270 项全过**
+ CLI 冒烟 6 项 + 错误码目录 9 项 + LSP 会话 23 项。

**已经稳的东西**：捕获即返回值的语法糖、编译期求值与泛型单态化、`@file/@line/@col/@fnName`
零开销位置内建、`std.debug`（panic/assert/errName/printTrace）、文件类与反例测试、
多文件模块与增量编译、结构展开 `{any}`、指针算术、枚举错误名回放、
**FFI**（`extern "C"` 块一次声明一组 C 函数/变量/结构体，`@link` 挂系统库，
`demo/ffi/` 有打真实显卡的 OpenGL 例子）、`@embedFile` 编译期嵌文件、
格式化 `{[idx][verb]:[fill][align][width][.prec]}` 全规格、`_ = expr` 显式丢弃、
`?*T` 无标签 optional（null 即 0 指针，sizeof = 指针宽度）、
**诊断体验**（报错带源码行 + `^` 指位，多字节字符与 tab 指位也准；
`did you mean` 拼写建议；重复声明/跨模块私有访问带相关位置 note；
连锁错误抑制——用户看到的条数等于根因数）。

**已知限制（会变，但别按"它有"来设计）**：

| 方向 | 限制 |
| --- | --- |
| 并发 | 无读写锁、无 `select`、无带缓冲通道；`close` 后句柄失效（槽位不复用）；根上下文通信是协作式 `would_block`；实例由外部调用者串行使用（构造 / `awaitAll` / `close`），但实例管理的那组工作线程内部是线程安全的 |
| 调试信息 | 后端不生成 DWARF/PDB，`-g` 未暴露；单步不可用，靠 `std.debug` + 打印/断言 |
| 包管理 | 没有网络依赖解析；依赖靠 `vendor/` 或 `deps` 手工放置 |
| 优化 | 只把 `-O0`/`-O2` 交给 C 编译器，没有 Zx 层优化 |
| 约束/特征 | 无 trait/接口/约束；用 `union(enum)`、函数字段、`@typeInfo` 自检 |
| 反射 | 仅编译期 `@typeInfo`，无运行时类型信息 |
| 元组下标 | 编译期 `.N`；运行时下标要转数组/切片 |
| 平台 | 开发主力是 Windows；非 Windows 需自备 `ZX_CC` 与工具链路径 |
| VS Code | 无官方 client；高亮与补全只在 Zed / 通用 LSP 客户端 |

---

## 如何使用

### 构建

```sh
sh build.sh          # → bin/zxc.exe + bin/zxfe.exe + bin/zxls.exe
sh test.sh           # 回归（语法 + 语义 + 反例 + 运行 + 多文件 + 增量 + CLI + LSP）
```

编译器用 C11 写，工具链是项目内 vendored 的 zig（`.toolchain/zig.exe`，其 `cc` 即
clang + 自带 libc），不依赖系统 gcc/clang。首次 clone 时 `.toolchain/` 为空（约 360 MB，
不入库）：把 **zig 0.17.0** 解到 `.toolchain/zig.exe`，或用 `ZIG=zig sh build.sh` 走
PATH 上的 zig。

### 命令行

```sh
bin/zxc.exe file.zx               # lex + parse + 类型检查（默认）
bin/zxc.exe file.zx -o prog       # 生成 C 并编成可执行文件
bin/zxc.exe file.zx --emit-c      # 只吐生成的 C
bin/zxc.exe file.zx --ast         # 打印语法树
bin/zxc.exe -v                    # 版本

# 工程子命令（读 zx.json，zxc init 生成）
bin/zxc.exe init [name]           # 创建工程：src/、vendor/、zx.json
bin/zxc.exe build [--release]     # src/ → out/<名>.exe
bin/zxc.exe build --lib           # 静态库；--lib --shared 动态库
bin/zxc.exe run [-- args...]      # 构建后直接运行
bin/zxc.exe test [过滤]            # 运行 src/test/ 下零参 pub 函数（每用例一个进程）
bin/zxc.exe check                 # 静默检查 *.zx
bin/zxc.exe fmt [--check]         # 格式化
```

后端 C 编译器由 `ZX_CC` 环境变量指定，缺省是 `.toolchain/zig.exe cc`。

### 快速上手

```zx
const std = module("std");

const main = fn () !i32 {
    std.println("hello, {}", "zx");
    return 0;
};
```

`zxc hello.zx -o hello && ./hello` 即可。变量用 `let`（不可变）/ `var`（可变）；
函数用 `const name = fn (args) ret { ... }` 定义；`!i32` 表示「返回 `i32` 或错误」。

### 语言速览

```zx
// 错误联合与可选：!T 可能带错误，?T 可能为空
const mayFail = fn (x: usize) !usize {
    if (x > 100) { return error.too_big; }
    return x * 2;
};

const main = fn () !i32 {
    // 四种出口
    let r = try mayFail(10);                 // 出错就沿 !T 传播链冒出去
    if (mayFail(200)) v {                    // 成功臂捕获值 v
        std.println("ok:{}", v);
    } else e {
        std.println("err:{}", std.debug.errName(e));  // 错误臂拿到错误码
    }
    let o = mayFail(5).?;                     // ? 解包，空则沿 !T 传播
    _ = o;
    return 0;
};

// 结构体与枚举（variant 即枚举成员）
const Point = struct { x: i32, y: i32 };
const Shape = union(enum) { circle: f64, rect: Point };

// 模块：标准库是普通 Zx 代码，可读可改（仓库 std/ 下）
const std = module("std");
```

要点：错误用 `!T`/`?T` 显式承载；`union(enum)` 当带数据的枚举；模块经
`module("std")` 引入，`std.<ns>` 取成员；编译期求值与泛型单态化在后台完成。
完整语法与每个特性的语义见 `docs/manual.md`。

### 并发：Task 实例

唯一并发抽象。`std.Task` 是实例——每个实例物理隔离（自己的任务表、就绪队列、
通信通道、一组线程），三种形态只是「谁来推进」不同：

```zx
const std = module("std");
const Task = std.Task;

const work = fn (ctx: usize) !usize {
    let t = Task.fromId(ctx);     // 把实例 id 当 ctx 传进来，任务里还原
    var i: usize = 0;
    while (i < 10) { t.yield(); i = i + 1; }
    return 42;
};

const main = fn () !i32 {
    let co = Task.coro();         // 协程：不建线程，调用者 await 时推进
    let st = Task.single();       // 单线程：1 条专属 OS 线程
    let pl = Task.pool(4);        // 多线程：4 条工作线程（0 = 自动 CPU-1）

    let h = pl.spawn(work, pl.id).?;   // 入本实例队列，返回 ?usize 句柄
    try pl.awaitAll();                 // 等本实例名下全部完成（纯屏障）
    let r = try pl.await(h);           // 取单个结果 / 错误
    std.println("r={}", r);
    pl.close();                        // 停线程组，句柄失效
    co.close();
    st.close();
    return 0;
};
```

| 构造 | 线程 | 推进 |
| --- | --- | --- |
| `Task.coro()` | 0 | 调用者在 `await`/`awaitAll` 时驱动，交错确定 |
| `Task.single()` | 1 | 一条专属 OS 线程跑调度循环 |
| `Task.pool(n)` | n | n 条工作线程抢本实例就绪队列（`n=0` 自动 CPU-1） |

实例方法：`spawn(fn, ctx) ?usize` · `await(h) !usize` · `awaitAll() !void`（屏障）·
`cancel(h)` · `yield()` · `close()` · 通信 `send/recv/trySend/tryRecv`（见下）。
任务签名固定 `fn (usize) !usize`；多值打包成堆上结构体传指针。跨实例 await 允许：
A 的任务等 B 的任务，B 完成时等待者被推回 A 的队列。

**通信：实例自带无缓冲 rendezvous**——`send`/`recv` 在任务内真停靠，先到者挂等待链、
后到者直接握手；`trySend`/`tryRecv` 永不停靠；`close()` 唤醒全部停靠者（收 `error.closed`），
`cancel(h)` 提前唤醒正停靠在通信上的任务（收 `error.cancelled`）。根上下文没有推进力，
通信是协作式 `would_block`。

### 标准库（8 个模块）

`std/str`、`std/map`、`std/fs`、`std/json`、`std/time`、`std/pool`、`std/task`，
加聚合入口 `std`。另有 `std.debug`（panic/assert/errName/printTrace）、`std.atomic`
（Add/Sub/Load/Store/Swap/CAS，纯函数）、`std.process`、`std.heap` 等。容器（ArrayList /
HashMap / 字符串）与格式化（`{}` 占位）齐备。

### 工程配置（zx.json）与模块搜索路径

`zxc init` 生成的 `zx.json`：

```json
{
  "name": "demo",
  "version": "0.1.0",
  "main": "src/main.zx",
  "out": "demo",
  "deps": ["vendor2", "../shared"]
}
```

模块解析顺序：**相对导入文件所在目录** → 工程根 `vendor/`（存在即自动加入）→
`deps` 列表。模块名可省 `.zx`（`module("ext")` 即 `vendor/ext.zx`）。`vendor/` 是
源码依赖根，应提交进版本控制；`deps` 留给工程外的目录。

### FFI：直接调 C

不用写绑定生成器，`extern "C"` 块一次声明一组；`@link` 挂系统库：

```zx
@link("user32");
extern "C" {
    pub const MessageBoxW = fn (hWnd: ?*anyopaque, text: ?*const u16,
                                caption: ?*const u16, type: u32) i32;
    pub const GetTickCount = fn () u32;
}
```

函数、变量（`extern var`）、结构体（`extern struct`）都能声明，布局按 C ABI
对齐；完整例子见 `demo/ffi/`（user32/gdi32/opengl32 绑定 + 创建窗口 + OpenGL
上下文，跑起来打印显卡型号）。SDL/GLFW 这类深嵌套布局的库照此办理。

### 编辑器（Zed）

`sh build.sh` 编出三件套后，`sh editor/zed/install.sh` 装扩展即获得语法高亮、大纲、
LSP 补全/跳转/诊断。zxls（语言服务器）走 vendored zig 0.17.0；诊断与编译器 100% 同源
（同一份 sema），错误消息的源码片段、`did you mean` 建议与相关位置 note 原样进编辑器，
且不依赖宿主环境变量（临时文件落工具链自身目录）。诊断消息自带 `help:` 修法，
诊断上的错误码可点开 `docs/errors.md` 里对应的一条（`codeDescription`，
客户端不支持时自动不发）。排障见 `editor/README.md`。
其他 LSP 客户端也可接 `bin/zxls.exe`。

---

## 扩展库 `x/`（1.0.0 起）

std 之外的第二层：常用但不值得让每个人都编译的库。不进 std、不进聚合，用哪个拉哪个——`module("../x/…")` 相对导入，或写进 `zx.json` 的 `deps` 搜索路径。

```zx
const stair   = module("../x/stair.zx");     // 稀疏表：O(n log n) 预处理，O(1) 区间 min/max/gcd
const lattice = module("../x/lattice.zx");   // 多重数组：任意维数，行主序扁平存储
const numkit  = module("../x/numkit.zx");    // 数学小件：gcd / powMod / isqrt / floorLog2

const less = fn (a: i64, b: i64) bool { return a < b; };
var t = stair.SparseTable(i64).init(std.heap.page, &data, less);
if (t) st {
    let m = st.query(2, 9);    // 区间 [2,9] 的最小值，O(1)
    st.deinit();
};
```

稀疏表只吃**可重复贡献**的运算（min/max/与/或/gcd），不支持改——要"能改 + O(log n)"是线段树的事。多重数组跟它同一个底子：一块扁平大数组扛所有数据，步长折算下标。`x/demo.zx` 是全套验收（暴力对拍 + 偏移公式 + 断言清单）。
---

## 代码结构（给想读源码 / 贡献的人）

| 目录 | 内容 |
| --- | --- |
| `src/` | 编译器 C 实现：`parse.c`（前端 parser）、`sema.c`（类型/语义）、`backend.c`（C 发射）、`zxc.c`（诊断/缓冲）等 |
| `bootstrap/` | 自举前端（Zx 代码）：`parse.zx` / `lex.zx`，编译成 `bin/zxfe.exe`，zxc 默认走它 |
| `std/` | 标准库（Zx 代码，可读可改）：`str/map/fs/json/time/pool/task.zx` + 聚合 `std.zx` |
| `x/` | 扩展库（不进 std，按需 `module("../x/…")`，不付编译速度）：`stair.zx` 稀疏表、`lattice.zx` 多重数组、`numkit.zx` 数学小件，`demo.zx` 验收 |
| `docs/` | `manual.md` 语言手册、`guide.md` 教程、`errors.md` 错误码参考 |
| `spec/` | `zx-spec.md` 规格、`zx.ebnf` 文法、`decisions.md` 设计裁定记录（102 条） |
| `tests/` | `good/`（正例，带 `// expect-output:`）+ `bad/`（反例）+ `multi/`（多文件） |
| `demo/` | `ffi/`：Windows API + OpenGL 全链路例子（extern "C" 绑定的真实用户） |
| `bench/` | 基准（JSON 解析等） |
| `tools/` | `lsptest.py`（LSP 冒烟）、`runner.c`（测试驱动）、`zxls.zig`（语言服务器） |
| `editor/zed/` | Zed 扩展（高亮 + 大纲 + LSP 接线） |
| `editor/tree-sitter-zx/` | tree-sitter 语法（含 wasm 构建，其他编辑器接入用） |

设计取舍的来龙去脉都记在 `spec/decisions.md`，不在本文里复述。
