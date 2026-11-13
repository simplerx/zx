# Zx

一门为「写得少、跑得快」而设计的系统级语言。

语法接近 Zig，但把容易出错的自由度削掉了：没有隐式整型提升、没有隐式空指针、
没有运算符重载、没有异常、没有 GC、没有宏。剩下的东西各有唯一写法，读代码时
不需要推断作者的风格，编译器也不需要为风格差异付出代价。

- **一个值一个类型**：`?T` 和 `!T` 是普通值类型，`try` / 捕获 / `orelse` / `switch`
  四条出口，逃不掉也不用猜。
- **一个并发抽象**：`std.task.Task` 实例。线程、协程、任务池、select 与默认通道
  都挂在实例上，不用挑模型；`std.io` 保持纯同步，要并发就显式 `task + io`。
- **一个内存模型**：手动管理，标准库给 arena 和 pool；没有隐式分配。
- **直出 C**：后端把 Zx 生成 C，再交给项目内自带的 zig `cc`。生成物可读，
  也可以 `--emit-c` 拿出来看——性能问题不在黑盒里。

自举：前端（词法/语法）和整个标准库都是 Zx 自己写的，编译器主程序是 C。

## 语言速览

```zx
const std = @mod("std");

const Point = struct {
    x: f64,
    y: f64,

    const len2 = fn (self: *Self) f64 {
        return self.x * self.x + self.y * self.y;
    };
};

const parse = fn (s: []const u8) !Point {
    let n = std.str.parseInt(s);          // ?i64
    if (n) v {
        return Point{ .x = (f64)v, .y = 0.0 };
    }
    return error.bad_number;
};

pub const main = fn () !i32 {
    var p = Point{ .x = 3.0, .y = 4.0 };
    std.println("{}", p.len2());          // 25

    if (parse("12")) pt {                 // 捕获：成功拿值，失败拿错误码
        std.println("x={}", pt.x);
    } else e {
        std.println("err {}", std.debug.errName(e));
    }
    return 0;
};
```

要点：变量只有 `let`（不可变）和 `var`（可变）；函数是值，写成
`const name = fn (args) ret { ... };`；`!T` 表示「要么是 `T`，要么是一个错误」；
`Self` 指当前结构体；没有 `break`，提前退出用带标签的表达式
`const v = done: while (...) { ... done: 42; };`。

## 构建与测试

```sh
sh build.sh     # → bin/zxc.exe（编译器）· bin/zxfe.exe（自举前端）· bin/zxls.exe（语言服务器）
sh test.sh      # 全量回归：语法 / 语义 / 反例 / 运行 / 多文件 / 增量 / CLI / LSP
```

工具链是项目内自带的 zig（`.toolchain/zig.exe`，不进版本库，约 360 MB）。
缺它时把 zig 0.17.0 解到 `.toolchain/zig.exe`，或者 `ZIG=zig sh build.sh` 用系统 zig。
Windows 是主平台（FFI 演示、语言服务器都按它写），其余平台尚未验证。

跑一下带自检的示例：

```sh
bin/zxc.exe demo/snake.zx -o snake.exe
./snake.exe          # 控制台贪吃蛇：WASD/方向键，P 暂停，R 重开，Q 退出
./snake.exe --sim    # 无人值守：机器人自跑 300 局，校验每一步的不变式
```

## 命令

单文件：

```sh
zxc file.zx              # 静态检查（默认）
zxc file.zx -o prog      # 生成可执行文件
zxc file.zx --emit-c     # 只打印生成的 C
zxc file.zx --ast        # 打印语法树
zxc file.zx --tokens     # 打印词法单元
zxc file.zx --release    # -O2（默认 dev 是 -O0）
```

工程（读当前目录的 `zx.json`）：

```sh
zxc init [name]          # 建工程：src/ + vendor/ + zx.json
zxc run                  # 构建后直接跑
zxc build                # 产物进 out/，中间文件进 .zxc-cache/
zxc test [filter]        # 跑 src/test/ 下的用例（零参 pub 函数）
zxc check                # 整个工程静态检查
zxc fmt [--check]        # 格式化
```

接 C 库：

```sh
zxc include ../raylib    # 目录 → vendor/<名>.zx 绑定 + vendor/<名>.clib 清单
zxc include foo.h        # 单个头文件
zxc add https://.../xx.h  or xx.git  or lib  # 从网上拉, 存入vendor，SHA-256 / commit 写进 zx.lock
zxc fetch                # 换机器时按锁复现 vendor/
```

`vendor/` 是模块搜索根：`const r = @mod("raylib");` 之后补全、跳转、行内类型
提示照常生效，生成的绑定是普通 Zx 源码，可以直接改。

## 性能

同 `zig cc -O2`、同基线的对照（`sh bench/run.sh`，每项 7 次取中位数）：

| 负载 | Zx | 手写 C |
| --- | --- | --- |
| fib(34) | 14.4 ms | 14.1 ms |
| mandel | 130.9 ms | 132.0 ms |
| 矩阵乘 | 92.1 ms | 93.6 ms |
| 字符串扫描 | 2.6 ms | 2.0 ms |

**运行时已经和手写 C 打平**：生成的是普通 C，没有运行时解释、没有隐式装箱、
没有 GC 停顿。容器（`ArrayList` / `HashMap`）用 `@memcpy`/`@memmove`/`@memset`
走批量路径，扩容时哈希随键缓存，实测比朴素实现快约 23%。

编译侧同样是线性的：小文件 0.1 s 级，多文件工程按模块出 `.o` 增量重编；
语言服务器给 16000 行文件算 48000 条行内类型提示只要 0.09 s。

## 目录

| 路径 | 内容 |
| --- | --- |
| `src/` | 编译器 C 实现：词法 / 语法 / 语义 / C 发射 / 诊断 |
| `bootstrap/` | 自举前端（Zx 写），编成 `bin/zxfe.exe`，默认走它 |
| `std/` | 标准库：`str` `map` `fs` `json` `time` `arena` `pool` `task` |
| `tests/` | `good/` 正例、`bad/` 反例（带期望错误码）、`multi/` 多文件 |
| `demo/` | `snake.zx` 控制台游戏；`ffi/` Win32 + OpenGL 绑定示例 |
| `bench/` | 与手写 C 对照的基准与基线 |
| `spec/` | `zx-spec.md` 规格、`zx.ebnf` 文法、`decisions.md` 设计裁定 |
| `editor/` | Zed 扩展与语言服务器源码 `tools/zxls.zig` |

## 错误码

```sh
zxc errors               # 所有错误码，按类别
zxc explain E0304        # 单个错误码的含义与修法
```

错误码文案就在 `src/errors.c` 里，`zxc errors` / `zxc explain` 直接念它，
不用翻文件；完整参考在 `docs/errors.md`（诊断里的 `details:` 链接、编辑器里
点诊断上的码都跳到那里）。

标准库还没有定型，所以**没有写手册**：`std/` 的源码顺序和注释就是文档，
编辑器里的补全会给出签名。手册与离线文档站以后再补。语言规格与设计裁定在
`spec/`（`zx-spec.md`、`decisions.md`）。
