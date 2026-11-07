# Zx 语言手册

> 对应版本 **zxc 0.49.0**（语言规格 `spec/zx-spec.md` v0.27）
> 本文是**查表用的**：命令怎么敲、某个语法点怎么写、标准库有什么、什么地方会坑。
> 想从头学，先读 `docs/guide.md`；想改编译器，读 `README.md` 与 `spec/decisions.md`。

---

## 0. 文档分工

| 文档 | 回答什么 | 什么时候读 |
| --- | --- | --- |
| `docs/guide.md` | 从 hello 到泛型的完整教程，讲"为什么" | 第一次接触 Zx |
| `docs/manual.md`（本文） | 命令、语法点、标准库、坑与限制 | 写代码时随手查 |
| `spec/zx-spec.md` | 规则的精确定义（编译期语义、布局、诊断） | 要判断"这到底算不算合法" |
| `spec/decisions.md` | 每个设计的取舍、否掉的替代方案 | 想知道"为什么不能那样" |
| `README.md` | 编译器实现与里程碑日志 | 改编译器、看历史 |
| `editor/README.md` | 编辑器安装与排障 | 高亮/补全不工作 |

Zx 的目标很窄，先记住这一句能省很多疑惑：**编译到可读的 C11，产物是普通本机可执行文件**。
没有虚拟机、没有 GC、没有运行时安装；Zx 只负责把语言翻成 C，剩下的交给 C 编译器。

---

## 1. 十分钟上手

### 1.1 环境

只需要仓库自带的工具链，不用装 gcc/clang：

```sh
sh build.sh        # → bin/zxc.exe（编译器）bin/zxfe.exe（前端）bin/zxls.exe（语言服务器）
sh test.sh         # 回归；退出码非 0 = 有失败
```

- 编译器本身是 C11，用 `.toolchain/zig.exe cc` 编（zig 的 cc 就是 clang + 自带 libc）。
- 首次 clone 时 `.toolchain/` 为空（360 MB，不入库）：放一份 zig 0.17.0 到
  `.toolchain/zig.exe`，或 `ZIG=zig sh build.sh` / `ZIG=<路径> sh build.sh`
  指定工具链。别用 0.15——`zxls` 依赖 0.17 的 `std.Io`。
- `.toolchain/`、`.zc-cache/`、`bin/` 不入版本控制，换机器重新 `build.sh` 即可。
- 后端 C 编译器可用 `ZX_CC` 换成系统的 `cc`/`clang`/`gcc`（见 §2.4）。

### 1.2 第一个程序

```zx
const std = module("std");

const main = fn () i32 {
    std.println("hello, zx");
    return 0;
};
```

```sh
bin/zxc.exe hello.zx -o hello && ./hello
```

注意三件事，它们几乎能解释一半的"新语法看不懂"：

1. **函数就是常量**：`const main = fn () i32 { … };`，`fn main() {}` 这种写法不存在。
2. **`return` 是唯一出口**：函数体是块，块末尾的表达式不是返回值。
3. **`main` 的返回类型决定退出码**：`i32` → 那个值；`void` → 0；`!void` → 错误码。

### 1.3 工程

```sh
bin/zxc.exe init shop     # 新建工程（当前目录下创建 shop/）
cd shop
bin/zxc.exe run           # 构建 + 运行
```

`zxc init` 生成：

```
shop/
├── zx.json              工程清单
├── src/main.zx          入口
├── vendor/              外部模块根（第三方 Zx 源码放这里）
│   └── README.md        用法说明（空目录进不了 git，用这份占位）
├── README.md
└── .gitignore           out/ .zxc-cache/ 等
```

`zx.json` 只有四个常用字段：

```json
{
  "name": "shop",
  "version": "0.1.0",
  "main": "src/main.zx",
  "out": "shop",
  "deps": ["vendor2", "../shared"]
}
```

`deps` 是"工程外"的模块搜索目录；工程内的第三方代码请放 `vendor/`（存在即自动生效，
不用写进 deps）。两者的搜索顺序见 §3.7。

---

## 2. 命令行参考

### 2.1 总览

```sh
zxc -h | --help | help [cmd]      帮助（总览 / 子命令细节），stdout + 退出码 0
zxc -v | -V | --version           版本（含前端与后端工具链实际路径）
zxc <file.zx> [选项]              单文件：解析 + 类型检查（默认）
zxc <file.zx> -o <prog>           单文件：生成 C 并编成可执行文件
```

| 子命令 | 作用 | 关键细节 |
| --- | --- | --- |
| `add <name\|header.h>` | 接入 C 库 | 自动生成 `vendor/<name>.zx` 绑定；见 §3.17 |
| `init [name]` | 创建工程 | 生成 `src/`、`vendor/`、`zx.json`、`.gitignore`；建完自检 `main.zx` |
| `build` | 按 `zx.json` 构建到 `out/` | 中间文件在 `.zxc-cache/`，见 §5 |
| `run` | `build` 后直接运行 | `--` 之后的参数传给程序 |
| `test [过滤]` | 跑 `src/test/` 用例 | 零参 `pub` 函数即用例，每例一个进程 |
| `check [file...]` | 静态检查 | 缺省当前目录，回落 `src/`、`src/test/` |
| `fmt [file...] [--check]` | 格式化 | `--check` 只报告不写回 |

### 2.2 单文件选项

| 选项 | 作用 |
| --- | --- |
| `--check` | 只做检查（等价默认行为，显式写法） |
| `--tokens` | 打印 token 流（调词法器） |
| `--ast` | 打印语法树（s-expression） |
| `--emit-c` | 只把生成的 C 打到 stdout |
| `-o <path>` | 输出可执行文件（`-opath` 连写也认） |
| `--release` | `-O2` 发布档；默认 dev 档 `-O0`（迭代快） |
| `--fe=zx|c` | 前端选择：默认 Zx 前端；`c` 只用于交叉验证/种子构建 |

### 2.3 `build` 细节

```sh
zxc build                  # out/<out>.exe（Windows）或 out/<out>
zxc build --release        # -O2
zxc build --lib            # out/<out>.a
zxc build --lib --shared   # out/<out>.dll（非 Windows: .so）
```

- `--shared` 必须配 `--lib`（可执行文件不是库）。
- 构建失败时，后端 C 的报错会被翻回 **zx 源码行列**；`ZX_VERBOSE=1` 看原始命令行与 C 报错。
- 同一模块内容没变不重写、不重编（按 mtime 判定）。

### 2.4 环境变量与退出码

| 变量 | 作用 |
| --- | --- |
| `ZX_CC` | 后端 C 编译器命令；缺省 `<zxc 目录>/../.toolchain/zig.exe cc` |
| `ZX_VERBOSE=1` | 打印构建细节（cc 命令行、后端原始报错、缓存命中） |

- 退出码：成功 `0`；**有诊断**或构建失败 `1`；**用法错误** `2`。
- 帮助与版本打 **stdout + 0**——用户主动要的信息不是错误，可以直接 `zxc -h | less`。
- 脚本里判断"能不能编过"请看退出码，不要 grep 输出。

---

## 3. 语言参考

> 本节是速查；每条规则的完整推导见 `docs/guide.md` 对应章节。

### 3.1 词法

```zx
// 行注释     /* 块注释，不嵌套 */
42  42u64  0xFF  0b1010  0o17  1_000_000
3.14  3.14f32  1e9
'a'  '\n'  '中'          // char：1..4 字节 UTF-8，参与整数运算时是码位
"hi\n"                   // 字符串 → []const u8
```

- 关键字 25 个：`const let var fn module type null undefined if else while for inline switch continue return defer struct union enum error pub extern orelse try`，另有 `errdefer`。
- **没有 `break`**；`true`/`false` 是字面量不是关键字。
- 字符串转义：`\n \t \r \0 \\ \" \' \a \b \f \v \e \xHH \uXXXX`。
- `.{` 是一个 token（复合字面量开头）；`..` 是范围；浮点必须有整数部分（`.5` 非法）。

### 3.2 三种绑定

| 形式 | 求值 | 可写 | 用途 |
| --- | --- | --- | --- |
| `const X = e;` | 必须编译期 | 否 | 常量、函数、类型、模块、泛型 |
| `let x: T = e;` | 运行时 | 否 | 只读值 |
| `var x: T = e;` | 运行时 | 是 | 唯一可变绑定 |

无标注时：整数字面量落 `i32`，浮点落 `f64`，其余按初值类型。参数天然只读。

**用不着的绑定必须写出来（E0410）**。函数体里的 `let` / `var` / 参数 / 捕获
绑定都要被读或写一次；否则写成 `_x`（照常声明，只是免检）或 `_`（丢弃，
连槽位都没有）：

```zx
const scale = fn (ctx: i32) i32 { return 1; };   // ✗ E0410: unused parameter 'ctx'
const scale = fn (_ctx: i32) i32 { return 1; };  // ✓ 有意不用

var tmp = 5;        // ✗ E0410
var _tmp = 5;       // ✓
_ = compute();      // ✓ 只要副作用，不要返回值
```

`self` 例外（方法接收者位置决定签名）。函数体里已经有别的错误时不报这条：
错误恢复下"没被用过"多半是假象，修完硬错误重编它自然会出现。

### 3.3 类型总表

| 写法 | 大小(64 位) | 说明 |
| --- | --- | --- |
| `u8…u128` / `usize` | 1/2/4/8/16 / 8 | 无符号 |
| `i8…i128` / `isize` | 1/2/4/8/16 / 8 | 有符号 |
| `f16 f32 f64` | 2/4/8 | IEEE-754 |
| `bool` / `void` / `type` / `noreturn` | 1 / 0 / 0 / 0 | — |
| `{T1, T2}` | 字段和 | 元组（值内联） |
| `*[N]T` | 8 | 数组：指针 + 长度进类型 |
| `*T` / `*const T` | 8 | 裸指针 / 只读 |
| `[]T` / `[]const T` | 16 | 切片 `{ptr, len}` |
| `struct{…}` / `union{…}` | 按声明 | 与 C 布局兼容 |
| `enum{…}` / `enum(T){…}` | sizeof(T)，默认 u32 | 枚举就是整数 |
| `?T` / `!T` | sizeof(T)+1 / +2 | 载荷在前、标签在后 |
| `?*T` | 8（指针宽度） | **无标签**：null 就是 0 指针，`?*Node` 链表节点不多占一个标签字节 |
| `fn(P…) R` | 8 | 函数值（回调） |
| `error` | 2 | u16，0 = 无错误 |

**只有显式转换**：`(i64)a`、`(usize)ptr`、`(Color)5`。同宽符号互转也要写出来，
字面量例外（`42` 能落到任何整数类型）。

### 3.4 数值语义

- 整数溢出：安全模式运行时 trap；编译期常量直接编译错。要绕回自己写位运算。
- 除零 trap；`a / 0` 编译期能定就是编译错误。
- 有符号右移是算术移位；左移掩到类型宽度。

### 3.5 表达式与运算符

| 类别 | 运算符 |
| --- | --- |
| 算术 | `+ - * / %` |
| 位 | `& \| ^ ~ << >>` |
| 比较 | `== != < <= > >=` |
| 逻辑 | `&& \|\| !`（另有 `and` / `or`） |
| 其它 | `a orelse b`、`e.?`、`try e`、`(T)e`、`&e`、`e[idx]`、`e.field`、`e.0` |

- `.` 自动穿透一层指针：`p.name` ≡ `p.0.name`；`.0` 是"元素 0"（指针/数组/元组统一）。
- 条件必须是 `bool`，没有真值语义（`if (n)` 非法，写 `if (n != 0)`）。
- `orelse` 惰性求值、右结合；`.?` 空了直接 abort 并打印说明。

### 3.6 复合类型

```zx
const Person = struct { name: []const u8, age: i32 = 0 };   // 字段默认值
var p = Person{ .name = "ann" };                            // 缺省字段用默认值
var q: Person = .{ .name = "bob" };                         // 有期望类型时用 .{}

var arr: *[4]i32 = &.{1, 2, 3, 4};   // 数组字面量
arr[2] = 30;  arr.len;  arr.size;    // 运行时下标；len/size 是编译期
sum(arr[0..]);                       // 数组 → 切片，零开销
arr[1..3]                            // 左闭右开

const Color = enum(u8) { red, green = 5, blue };   // blue == 6
const Shape = union(enum) { circle: f64, rect: {f64, f64} };
let s = Shape{ .circle = 2.0 };
switch (s) { .circle: r a: { a: 3.14159 * r * r; }; .rect: wh a: { a: wh.0 * wh.1; }; }
```

- 结构体体是作用域：可以放 `var` 静态字段、`const` 编译期常量、方法。
- 方法 = 第一参数是接收者的普通函数；接收者 `Self` / `*Self` / `*const Self` 三选一。
- 第一参数不叫 `self` 就是关联函数（`Color.count()`）。
- 联合体 `union(enum)` 的标签按声明顺序自动编号；普通 `union{}` 无标签，读错成员是 UB。
- 自引用必须过指针：`next: ?*Node`。

### 3.7 控制流（没有 break）

```zx
if (n > 0) { use(n); } else { report(); }
let max = if (a > b) a else b;                 // 表达式臂
if (maybe) v { use(v); } else e { log((usize)e); }   // ?T / !T 捕获
let v = if (maybe) v;                          // 捕获即返回值：臂就是 v
let v = if (maybe) v else 0;                   // 同上，假分支给 0

while (i < 10) { i = i + 1; }                  // 没有自增子句
while (it.next()) v { total = total + v; } else { done(); }

for (xs) x { }                                 // 遍历切片
for (0..n) i { }                               // 整数范围，左闭右开
for (xs, 0..) x, i { }                         // 值 + 下标
for (xs, ys) a, b { }                          // 两个等长序列

const w = done: while (true) {                 // 标签写在结构前面
    n = n + 1;
    if (n == 7) { done: n * 10; }              // 收割 = 带值的 break
};

let idx = hit: for (0..n) i {                  // 循环的 else：正常跑完才执行
    if (xs[i] == target) { hit: (i32)i; }
} else -1;
```

**收割 `tag: expr;`** 是 Zx 里唯一的"提前离开 + 带值"手段：块 `tag: { … }`、
循环 `tag: while/for (…)`。`continue;` 走本轮末尾，`continue :tag;` 跳到外层标签循环的下一轮。

`tag: { … }` 里的花括号是**块**还是**值**，看里面有没有语句：

```zx
tag: { … }        // 有语句 → 带标签的块（里面的 `tag: v;` 收它）
tag: {}           // 空的 → 是值，这就是"空手收割"
tag: {a, b}       // 只有值 → 元组，收割一个元组
```

**同一条规则在臂位置也成立**，所以要跳出循环不必多包一层块：

```zx
tag: while (i < 10) {
    i += 1;
    if (i == 5) tag: {};      // 臂上的收割：直接跳出
    if (i == 7) tag: 0;       // 带值收割
    if (i == 9) { tag: {}; }  // 包一层块也行，效果一样
}
```

`switch` 的模式：`.成员` / 整数 / char / `true` / `false` / 负数 / `else`；**每条臂以 `;` 结尾**
（含末臂），**逗号只作同臂模式并列**（`'a', 'b', 'c': x;`）。**冒号后紧跟 `{` 是捕获，
紧跟 `:` 是标签，什么都不跟（`;` / `}` 前）就是"臂值 = 捕获值"**。
枚举、`union(enum)`、`?T`、`!T`、bool 必须穷尽或有 `else`。

```zx
let n = switch (maybe) { .some: v; .none: 0; };   // 冒号后只有一个名字 → 臂值就是 v
let a = switch (shape) { .circle: r { r * r }; .rect: wh t: { t: wh.0 * wh.1; }; };
```

`defer` 离开所在块时执行（LIFO），**表达式在退出时求值**——所以 `defer free(buf); return buf;`
返回的是悬空指针。`errdefer` 只在"因错误返回"时执行。

### 3.8 函数与泛型

```zx
const add = fn (a: i32, b: i32) i32 { return a + b; };
const fact = fn (n: i32) i32 { if (n <= 1) { return 1; } return n * fact(n - 1); };

extern fn system(cmd: *const u8) i32;          // 只有声明，没有函数体
```

- 无重载、无默认参数、无变参；多返回值用元组 `let {q, r} = divmod(a, b);`。
- 非 void 函数**每条路径都要 return**（到达性分析），不可达代码也是错误。
- 函数值是 `fn(i32, i32) i32`，8 字节，可当参数/字段。
- 定长数组参数用编译期长度：`fn f(n: usize, p: *const [n]i32)`。

泛型就是"返回类型的编译期函数"：

```zx
const List = fn (T: type) type {
    return struct {
        buf: *T, len: usize, cap: usize,
        const push = fn (self: *Self, v: T) bool { … };   // 泛型体里写 Self
    };
};
var xs = List(i32).init(std.heap.page);
```

- 也可以有编译期值参数：`Buffer(u8, 4)` 与 `Buffer(u8, 8)` 是不同类型。
- 每次实例化生成独立代码（单态化），零运行时开销。
- **没有 trait/约束**：要约束用编译期 `if` + `@typeInfo` 自检。

### 3.9 空值与错误

```zx
let a: ?i32 = 5;      let b: ?i32 = null;
let c: !i32 = 5;      let d: !i32 = (error)1;

if (parse(s)) n { use(n); } else e { log((usize)e); }   // 就地处理
let port = cfg.port orelse 8080;                        // 默认值
let p = n.?;                                            // 断言有值，否则 abort
let fd = try open_file(p);                              // 错误原样上抛
```

- `?T` / `!T` 都是带标签联合体，**载荷在前、标签在后**；`&x` 拿的是载荷地址。
  唯一的例外是 `?*T`——它无标签，null 就是 0 指针，语法（if 捕获 / orelse / try /
  switch `.some`/`.none`）与带标签版完全一致，只是布局按指针走。
- `null` 没有自己的类型：`let a = null;` 编译错。
- `?T` / `!T` **不能嵌套**。
- 错误码 `0` 是"无错误"，候选码从 1 起；`65535` 保留给 `error.null`。
- `main` 的 `!void`：出错时退出码就是错误码，能直接对 shell 的 `&&` / `||`。

**错误集**把"只会出这些错"写进类型：

```zx
const IoError = error{ open, read, write };

const open_file = fn (p: []const u8) IoError!i32 {
    if (p.len == 0) { return error.open; }
    return 100;
};
const read_it = fn (p: []const u8) IoError!i32 {
    let fd = try open_file(p);     // {open} ⊆ {open, read, write} ✓
    return fd + 1;
};
```

- 集合协变：窄集合可以交给宽集合，反过来不行。
- **命名集合的成员可以直接取**：`AppErr.open`（与 `error.open` 是同一个码）。
  取的名字必须在集合里，拼错会给出 `did you mean`。
- **集合可以合并**：字面量里引用别的错误集就是并集——

  ```zx
  const All = error{ IoErr, NetErr, bad };   // {open,read} ∪ {timeout,reset} ∪ {bad}
  ```

  引用的名字解析到错误集类型才合并；同名函数/普通常量照旧当成员名。
- 编译器守三件事：`return` 的码（E0901）、`try` 可能抛的码（E0902）、
  anyerror 抛进声明了集合的函数（E0903，得显式放宽返回类型）。
- 布局不变：`E!T` 与 `!T` 内存里一模一样，只多一份编译期信息。
- 带集合的函数不能 `try` 可选值（`?T` 的空用保留码 `error.null`，写不进 `error{…}`）。

### 3.10 模块与可见性

```zx
// util.zx
pub const VERSION: u32 = 3;
pub const add = fn (a: i32, b: i32) i32 { return a + b; };
const secret = fn (n: i32) i32 { return n * 2; };

// main.zx
const util = module("util");        // 省 .zx；相对调用者所在文件
util.add(1, 2);
util.secret(1);                     // ✗ private to module 'util'
```

**搜索顺序**（相对导入打不开时依次回退）：

1. 调用者所在文件目录
2. 工程根 `vendor/`（存在即自动加入）
3. `zx.json` 的 `deps`（按数组顺序）

- `module("std")` 是内建模块，从仓库 `std/` 找。
- 同一文件只加载一次（路径先归一化，折叠 `..`/`.`），循环导入直接报错。
- 模块顶层只能有声明，没有自由语句。
- **没有 import 语法**：`const` 绑定就是别名，函数/类型/常量/命名空间一视同仁
  （`const println = std.println;`、`const HashMap = std.HashMap;`）。
- `pub` 是唯一的可见性修饰；别名不改可见性。

### 3.11 内建（`@` 前缀）

| 内建 | 用途 |
| --- | --- |
| `@sizeOf(T)` / `@alignOf(T)` | 字节数 / 对齐 |
| `@offsetOf(T, "field")` | 字段偏移（布局在 `@sizeOf` 时一并算好） |
| `@typeOf(e)` / `@typeName(T)` | 取类型 / 类型名 |
| `@typeInfo(T)` | 类型元数据（`kind/name/bytes/align/fieldCount/fields`） |
| `@hasField(T, "x")` / `@fieldType` / `@fieldName(T, i)` | 编译期字段查询 |
| `@field(v, "x")` | 按编译期字符串取/写字段 |
| `@trap()` / `@compileError(msg)` | 中止 / 编译期报错 |
| `@file()` / `@fnName()` | 当前源文件路径 / 当前函数名（`[]const u8`，编译期常量） |
| `@line()` / `@col()` | 当前行列（`i32`，编译期常量） |
| `@memcpy(d, s, n)` / `@memmove` / `@memset(d, v, n)` | 批量内存原语 |

`@` 开头的名字不能被用户重定义。**没有 cast 类内建**（`@ptrCast` 等已从规格删除），
转换统一写 `(T)v`。

元编程示例见 `docs/guide.md` §15.1（`@typeInfo` + `inline for` 写通用 `eql`）。

### 3.12 外部函数

```zx
extern fn puts(s: *const u8) i32;                 // libc
extern fn GetTickCount64() u64;                   // Win32（大小写敏感）
```

- `extern fn` 只声明、无函数体；符号名就是声明名（不做名字修饰）。
- 参数/返回值只用 C ABI 兼容类型；`*u8` ↔ `char*`，`[]const u8` 不保证 NUL 结尾。
- **没有 `extern "C" { … }` 块**，逐条声明。
- **C 回调**：Zx 函数可以直接当函数指针传给 extern（如 `qsort` 的比较器）。
  调用点经 `(void*)` 显式中转，规避 Zx 签名与 C 原型（`const void*`）的
  文本差异；C 侧按 Zx 生成签名调用，指针宽度一致即 ABI 兼容。
- math.h 已进生成头：`pow`/`sqrt`/`sin` 等 libc_declared 名单里的数学函数
  真正可用（M24 前名单有名字、头里没声明，编译必失败）。
- 编译器魔法（`print`/`file`/`io`/`Allocator`/`stackAlloc`…）种进**每个**
  模块的作用域——std.zx 之外也能裸用 `file.read`；模块自己的同名声明优先。

### 3.13 extern struct / extern union —— 镜像 C 结构体布局

调用真实 C 库（OpenGL、Win32、SDL…）绕不开对方的结构体。`extern struct`
让 Zx 侧的类型按 C 的自然布局落内存：

```zx
const SystemTime = extern struct {
    wYear: u16, wMonth: u16, wDayOfWeek: u16, wDay: u16,
    wHour: u16, wMinute: u16, wSecond: u16, wMilliseconds: u16,
};
extern fn GetLocalTime(st: *SystemTime) void;

var st: SystemTime = undefined;
GetLocalTime(&st);
std.println("{}-{}", st.wYear, st.wMonth);
```

- **写法**跟普通类型一致——类型是值：`const Vec3 = extern struct { … };`。
  语句位置的 `extern struct Name { … };` 不是合法语法。
- **字段约束**（sema 逐个校验，不合规直接报错）：
  - 整数（i1 除外）/ 浮点 / bool / 枚举 / 指针——指针就是一个机器字；
  - **定长数组 `*[N]T` 展开成 C 内联数组**（`char name[64]`），元素同样
    受约束；`@sizeOf`/`@offsetOf` 按内联数组算（20 字节的 `char[16]+u32`，
    不是 8 字节指针）；
  - 嵌套聚合体必须自己也是 `extern struct`/`extern union`——普通 Zx 结构体
    的布局带运行时信息，不保证与 C 一致；
  - **函数指针字段** `cb: fn (i32) i32`——C 结构体带回调的形态，Zx 函数直接赋；
  - `?T`（带标签 optional）、`[]T`（切片）、`!T`（错误联合）、元组、
    方法、静态成员、字段默认值：**全部拒绝**——它们要么是 Zx 专有布局，
    要么根本不进 C 布局。C 指针写 `*T`（null 就是 0）。
- `extern union`：C 联合体（所有成员偏移 0）；`union(enum)` 的带标签形式
  是 Zx 布局，不允许混进来。
- `extern struct(packed)` 不支持：C 结构体跟平台自然布局走。
- 链接系统库用 **`@link("ws2_32")`**（见 §3.14）。

### 3.14 extern var —— 外部 C 全局变量

```zx
extern var g_counter: i32;              // C 侧: int g_counter;
extern var g_table: *const u8;          // C 侧: const char *g_table;
extern var g_origin: *Point;            // 指向 extern struct 的全局
```

- 只声明不定义：存储在 C 那边，`extern` 声明进生成头，名字原样（不转义）。
- **必须有类型标注、不能有初始化值**；只允许 `var`（C 全局没有只读保证）；
  只能写在文件根。
- Zx 与 C 读写的是同一份存储：Zx 侧赋值，C 侧立刻可见（反之亦然）。

### 3.15 @link —— 源码里声明外部库

```zx
@link("ws2_32");                    // → 链接命令加 -lws2_32
@link("./vendor/foo.lib");          // 带路径/扩展名的按文件原样传
@link("./counter.o");               // 手写的 C 对象文件也能挂进来
extern fn htonl(u32) u32;
```

- 编译期动作，运行时 no-op；写在文件根或任何语句位置都行，多次声明同名去重。
- 库列表记到构建缓存（`<stem>.libs`），增删库会**强制重链**——.o 没变也要
  重链，否则多链/少链一个库看不出来。
- 静态库构建（`--lib`）不消费 @link：.a 里没有链接这一步。

### 3.16 extern "C" { … } —— 块糖

一个库一组声明，`extern` 关键字只写一次；块内声明逐条上提到文件根，
语义与逐条写 `extern` 完全等价：

```zx
@link("ws2_32");
extern "C" {
    pub fn htonl(u32) u32;          // → extern fn
    pub const Ipv4Addr = struct {   // → extern struct（C 布局校验照常做）
        b0: u8, b1: u8, b2: u8, b3: u8,
    };
    var g_host: *u8;                // → extern var
}
```

- 块内收 `fn` / `var` / `const Name = struct|union`；`pub` 直接透传——
  **绑定模块的 extern 声明就是它的公开 API**（`pub extern fn` 也合法）。
- `enum` 不收：C 枚举就是 int，把常量当 `const` 写。
- 绑定模块的组织方式见 `demo/ffi/`：一库一模块，`@link` 收在模块里，
  应用代码只见 Zx。OpenGL 全链路演示（窗口 → 像素格式 → GL 上下文 →
  打印驱动字符串）也在这里。

### 3.17 zxc add —— 一条命令接入 C 库

手写 FFI 里最费心的不是 `extern fn`，而是抄函数签名、逐字段镜像 C 结构体布局。
`zxc add` 把 C 头交给项目内 vendored 的 clang 解析，产出一份**普通、可读可改的
Zx 源码**放进 `vendor/`：

```sh
zxc add ./vendor/raylib.h     # 本地头文件：完整绑定
zxc add sqlite3               # 内置配方：自动定位头文件 + 配好 @link
zxc add --list                # 列出内置配方
zxc add ./vendor/foo.h --lib foo --force
```

生成物形如：

```zx
@link("vendor/libfoo.a");

pub extern var foo_counter: i32;

extern "C" {
    pub const Point = struct {
        x: i32,
        y: i32
    };
    pub fn foo_sum(a: i32, b: i32) i32;
}

// C constants / enum values
pub const FOO_MAGIC: i32 = 7;
```

之后 `const foo = module("foo");` 就能直接调。`vendor/` 本来就是模块搜索根，
生成物是普通 Zx 源码，所以补全、hover、行内类型提示全部照常生效。

- **自动翻译**：函数（含参数名）、`extern var`、`struct`/`union` 字段、`typedef`、
  枚举常量、纯数字 `#define`。`void*` 用 `*u8` 占位（ABI 上都是一个机器字）。
- **翻不了的会跳过**，并在文件末尾以注释列出原因（变参函数、多级指针、位域、
  函数指针之外的复杂声明、Zx 同名的关键字符号）。宁可留白也不猜一个错布局。
- **链接**：配方自带 `@link`；本地头文件旁边若有 `vendor/<名>.lib` /
  `vendor/lib<名>.a` / `vendor/<名>.a` 会自动挂上；否则用 `--lib NAME` 指定。
  静态库要按 C 编译器的同一套 sanitizer 标志编（工程默认 `-fno-sanitize=undefined`），
  否则链接时会缺 `__ubsan_handle_*` 符号。
- **系统头配方**（`m` / `c` / `ws2_32` / `user32` …）走工具链自带头文件，
  只导出基础类型的函数签名：SDK 内部结构互相引用、带位域和匿名 union，
  硬生成只会得到编译不过的绑定。要结构体和常量，把库的头文件放进工程，
  用路径方式 `zxc add`（Win32/OpenGL 的手写范例见 `demo/ffi/`）。
- 重新生成用 `--force`（会覆盖手改）。命令不联网、不下载源码，不是包管理器。

---

## 4. 标准库

`std` = 编译器种下的内建 + 仓库 `std/*.zx`（普通 Zx 代码，可读可改）。

### 4.1 内建命名空间

| 名字 | 成员 |
| --- | --- |
| `std.print(fmt, …)` / `std.println` | 格式化到 stdout |
| `std.format(alloc, fmt, …)` | `![]u8`，堆上格式化 |
| `std.heap.page` | 默认分配器（`Allocator`：`{ ctx, alloc, free }`；`a.alloc(a.ctx, n)` / `a.free(a.ctx, p, n)`。`ctx` 让 arena 等有状态分配器不用再靠全局变量实现） |
| `std.file` | `read(al, path)`、`write(path, data)`、`append`、`exists`、`remove` |
| `std.io` | `openRead/openWrite/openAppend(path)`、`read/write/seek/tell/close/flush`、`stdin/stdout/stderr`、`print/println` |
| `std.process` | `args()`、`getEnv(name)`、`setEnv`、`exit(code)` |
| `std.debug` | `panic(msg)`、`assert(cond, msg)`、`errName(err)`、`printTrace()` |
| `std.Task` | 唯一的并发类型：`coro/single/pool` 建实例，`group()` 取内核调度线程组；`spawn/await/awaitAll/cancel/yield/send/recv/close` 是实例方法（§4.6/§4.8） |
| `std.atomic` | `Add/Sub/Load/Store/Swap/CAS`，usize，SEQ_CST（纯函数，不是并发类型） |
| `std.stackAlloc(T, N)` / `std.heapAlloc(al, T, N)` | 数组分配（OOM abort） |

### 4.2 容器

| 类型 | 要点 |
| --- | --- |
| `std.ArrayList(T)` | `init(al)`、`push`、`append`、`insertAt`、`removeAt`、`items`、`get`/`set`、`pop`/`first`/`last`、`resize`、`reverse`、`swap`、`clear`、`deinit` |
| `std.HashMap(V)` | 字符串键；`put`、`get`(→`?V`)、`contains`、`count`、`clear`、`deinit`；**键不复制**，必须比表活得久；没有 `remove` |
| `std.BufReader` | `open(al, path, cap)` / `init(al, h, cap)`、`next()` 逐行、`chunk()`、`deinit` |
| `std.BufWriter` | `create(al, path, cap)` / `init` / `appendTo`、`write`/`writeLine`、`flush`、`deinit`（自动 flush） |
| `std.Pool` | 19 档分级内存池（≤256 KiB），`init(gpa)`、`alloc`、`free`、`used` |

### 4.3 字符串 / 路径 / 数据

| 模块 | 主要函数 |
| --- | --- |
| `std.str` | `eq`、`startsWith`、`endsWith`、`indexOf`、`indexOfFrom`、`contains`、`trim`、`trimStart`、`trimEnd`、`parseInt`、`concat`、`split`、`freeSplit` |
| `std.fs` | `isAbs`、`basename`、`dirname`、`ext`、`stem`、`join`、`stat`、`exists`、`createDir`、`removeFile`、`removeDir`、`DirIter`、`readDir`；批量整块读写 `readAll(al, path)` / `writeAll(path, data)` / `appendAll`（每次调用独立 FILE*，多 Task 并发安全——分配器得用线程安全的那份，如 `heap.page`；Arena 别跨任务共享） |
| `std.json` | `parse(al, src)` → `!Value`、`stringify(al, v)`；`Value` 是 `union(enum)`；`member/item/text` 取值；`errLine/errCol` 定位 |
| `std.ArenaAllocator` | 有状态 arena：`init(backing)` 返回 `?Allocator`，可直接喂给 `json.parse` / `ArrayList` 等；`alloc` O(1) 16 对齐，`free` no-op，`reset` 帧重置（保留块、指针归零），`deinit` 总清算 |
| `std.time` | `counter()`/`freq()`/`nsSince()` 计时；`unixMs/unixSecs/unixNs`；`fromUnixMs`、`formatUtc` |

`std.fs` 与 `std.file` 有分工：`std.file` 是编译器种下的最小读写集（不依赖分配器），
`std.fs` 是纯 Zx 的路径与目录扩展（大多数函数要传 `alloc` 做临时缓冲）。

### 4.4 格式化

```zx
std.print("x = {}\n", x);
std.println("{}:{}", i, v);
```

| 占位符 | 含义 |
| --- | --- |
| `{}` | 按顺序取下一个参数，默认格式 |
| `{0}` | 按索引取 |
| `{d}` / `{x}` / `{b}` | 十进制 / 十六进制 / 二进制 |
| `{s}` | 字符串（要求切片） |
| `{c}` | 字符（整数按码位） |
| `{{` `}}` | 字面量花括号 |

- `fmt` 是**编译期常量**时整个格式化在编译期展开：零运行时解析、零分配，动词与
  实参类型配错是编译错误。
- `fmt` 是运行时字符串时回落到运行时解析器，语义一致（`std.io.print` 一律走运行时）。

### 4.5 调试

```zx
std.debug.assert(n > 0, "n must be positive");   // 失败 → panic（带 file:line:col 与函数名）
std.debug.panic("unreachable branch");            // noreturn，之后的代码报不可达（E0502）
std.println("{}", std.debug.errName(e));          // 捕到的错误值直接传，返回错误名
std.debug.printTrace();                           // 回放捕获环（最老→最新，见 §4.7）
```

- `panic` / `assert` 的位置实参（file/line/col/fn）是**编译器在调用点注入**的，
  源码里不用写、也写不出来。
- `--release` 下 `assert` 的运行时函数体整体擦除（条件副作用保留）；`panic` 不擦。
- `errName` 查编译器生成的静态表：`error.foo` 字面量首次被引用即登记，查表 O(1)
  （枚举错误 `(error)Err.x` 按显式码占位登记，decisions §90，也能回放成员名）。
- `@file()/@line()/@col()/@fnName()` 单独可用，全部编译期折成字面量、零开销。

### 4.6 任务：Task 实例（唯一的并发抽象）

M58 起并发只有一个类型：`std.Task` 的**实例**。每个实例物理隔离——自己的
任务表、就绪队列、通信通道和一组线程，实例之间不共享任何调度状态。三种
形态只是「谁来推进」不同（decisions §94）：

| 构造 | 线程 | 推进方式 |
| --- | --- | --- |
| `Task.coro()` | 0 | 不建线程；调用者在 `await/awaitAll` 时驱动，交错确定 |
| `Task.single()` | 1 | 一条专属 OS 线程跑调度循环 |
| `Task.pool(n)` | n | n 条工作线程抢本实例的就绪队列（`n=0` 自动 CPU-1） |

```zx
const std = module("std");
const Task = std.Task;

const produce = fn (n: usize) !usize {
    var acc: usize = 0;
    var i: usize = 0;
    while (i <= n) { acc = acc + i; i = i + 1; }
    return acc;
};

const main = fn () !i32 {
    let co = Task.coro();
    let h = co.spawn(produce, 100).?;    // 入队即返回，?usize 句柄
    let r = try co.await(h);             // 等完成并取结果
    std.println("{}", r);                // 5050
    co.close();                          // 停线程组，句柄失效
    return 0;
};
```

任务签名固定 `fn (usize) !usize`。多值打包成堆上结构体传指针；`ctx` 只有
一个 usize，要在任务里用实例就把 `t.id` 传进去、`Task.fromId(ctx)` 还原。
`awaitAll()` 是**纯屏障**：只等本实例名下 pending 归零，结果仍用 `await(h)`
逐个取（Zx 的 `!T` 是单值，不为它引入结果数组）。

| 实例方法 | 语义 |
| --- | --- |
| `spawn(fn, ctx) ?usize` | 入本实例就绪队列；失败返回 null |
| `await(h) !usize` | 等单个任务并取结果/错误；任务内真挂起，根上下文顺便推进调度 |
| `awaitAll() !void` | 等本实例全部任务完成（屏障） |
| `cancel(h)` | 置取消旗标 + 提前唤醒正停靠在通信上的任务 |
| `yield()` | 当前任务回队列尾 |
| `close()` | 拒绝新操作、唤醒全部停靠者、停本实例线程组 |
| `send/recv/trySend/tryRecv` | 见 §4.8 |

实例由创建它的外部线程串行使用（构造 / `awaitAll` / `close`）；**实例内部
那组工作线程之间的调度、任务表、等待链与通信由运行时自己保证线程安全**
（decisions §94）。跨实例 await 是允许的：A 的任务等 B 的任务，B 完成时
等待者会被推回 A 的队列，不会留在 B 上跑。

**`Task.group()`：内核调度线程组（零锁）**。与实例形态互补的另一极：
不建用户态线程池、不建队列、不建纤程，`spawn` 即起一条 OS 线程，调度
完全交给内核。分组只是一个原子计数（先加计数、后起线程，起线程失败自动
回退），`wait` 用 让步 → 短睡 的退避循环读到计数归零——全程无互斥锁、
无条件变量。内存序保证「计数归零即数据就绪」：任务体内的 `std.atomic`
写 happens-before 完成方减计数，等待方读到 0 时全部可见。任务即发即忘：
返回值与错误不回收（要结果、取消、通道就用实例形态）；组可复用，wait 后
可继续 spawn。

```zx
let g = Task.group();
var i: usize = 0;
while (i < 4) { _ = g.spawn(work, i); i = i + 1; }   // false = 起线程失败
g.wait();                                            // 无锁等全部完成
```

### 4.7 await 门与错误捕获 trace

**await 门**：每个返回 `!T` 的函数入口都是一个 await 点（decisions §88）。
在任务里：调用在入口**自动挂起**回调度循环；续跑后看取消旗标，被取消就
以 `error.cancelled` 走 `!T` 传播链。根上下文里门是一次纯 false 检查；
`--release` 下门保留（取消是运行时语义）。

**捕获环 trace（捕获感知任务）**：`else err { … }` 捕获点在 dev 档自动
登记一条 `{错误名, file, line, col, fn}` 到**当前任务**的环形缓冲（64 条，
满了挤最老）；`std.debug.printTrace()` 回放最老→最新。

- **trace 随任务走**：环挂在任务上下文上，不挂 TLS——任务被哪个线程调度
  trace 都不断链；任务的捕获与根上下文的捕获天然隔离。
- `try` 传播**不记**（码没有落地）；`orelse`/不带名的 else 臂也记。
- `--release` 下捕获点整体抹掉，trace 为空、零开销。
- 枚举错误 `(error)Err.x` 的名字也进登记表（decisions §90）：trace /
  errName 直接回放 `Err.open` 这样的成员名；显式码占坑，运行时错误码
  （cancelled 等）跳过已占槽位分配。

**同步原语**：并发类型只留 `Task`；`std.mutex` / `std.cond` / `std.channel`
/ `std.thread` 在 M58 一并收掉。`std.atomic`（`Add/Sub/Load/Store/Swap/CAS`，
usize，SEQ_CST）是纯函数，保留，按需链接。

### 4.8 通信：实例自带的 rendezvous

每个 Task 实例自带一条**无缓冲通道**，`send/recv` 在任务里是真停靠：先到
的一方挂在等待链上切回调度循环，后到的一方直接握手（值直投对方的 `cval`）
并把它推回所属实例的队列。没有 ring buffer、没有 cap，因此也不再有
「句柄定长 64」的限制。

```zx
let co = Task.coro();
let hp = co.spawn(producer, co.id).?;   // 任务里 try t.send(v)
let hc = co.spawn(consumer, co.id).?;   // 任务里 let v = try t.recv()
try co.awaitAll();
co.close();
```

- 根上下文没有推进力：`send/recv` 是协作式的，没有对手就返回 `would_block`；
  `trySend/tryRecv` 永不停靠。
- `close()` 唤醒全部停靠者，它们收到 `error.closed`；`cancel(h)` 会提前把
  停靠在通信链上的任务摘出来，返回 `error.cancelled`。
- 通道属于实例，但等待者按自己 `s` 的归属回到原实例队列，跨实例通信也可行。

## 5. 构建、产物与测试

### 5.1 产物布局

```
.zxc-cache/           中间文件（每个模块一个 .c/.o，内容没变不重编）
out/<out>[.exe|.a|.dll]
```

- dev 档 `-O0`（迭代快），`--release` 才 `-O2`。
- 增量判据是"内容 hash 没变就不重写，mtime 不动就不重编"，删掉 `.zxc-cache/` 等于全量重建。
- 交叉验证：`--fe=c` 用 C 版前端，与默认 Zx 前端对比（自举阶段的守门机制）。

### 5.2 测试

```sh
zxc test                  # src/test/*.zx 全部用例
zxc test parse            # 名字过滤
zxc test --release        # 发布档跑（性能用例用）
```

- `src/test/` 下每个 `*.zx` 是测试模块，**零参 `pub` 函数**就是用例。
- 每个用例一个独立进程：崩溃/abort 不会带走整轮。
- 仓库自己的回归是 `sh test.sh`（文件类用例 + 增量编译 + CLI 冒烟 + LSP 会话冒烟）。

---

## 6. 编辑器

完整安装与排障见 **`editor/README.md`**，这里只给结论：

```sh
sh build.sh                 # 三件套
sh editor/zed/install.sh    # 装 Zed 扩展（高亮 + 大纲 + LSP 接线）
```

- 高亮/折叠/大纲 = tree-sitter 语法（`editor/tree-sitter-zx/` + `highlights.scm`），**不需要编译器**。
- 补全/诊断/跳转/hover/签名/格式化/引用 = `zxls`，它转手问 `zxc`，**需要三件套**。
- 扩展按平台取 `<worktree>/bin/zxls[.exe]`；**不要在 `.zed/settings.json` 里覆盖绝对路径**。
- 改了 `zx.json` 的 `deps` 要重启语言服务器（搜索路径只在 initialize 时算一次）。
- 保存即格式化已默认开启（语言配置里 `format_on_save = "on"` +
  `formatter = "language_server"`），要求 `bin/` 里三件套齐全。
- 点补全支持：`X.` 出容器成员与方法、`X{ .` 只出字段/变体（字面量上下文）、
  `std.` / 模块绑定 / `@` 内建各归各位。
- 诊断上的码可点：点了打开 `docs/errors.md` 里那一条（`codeDescription`）；
  诊断消息下方自动带 `help:` 修法，不必再跑一次 `zxc explain`。

---

## 7. 性能

设计上的零开销点（都有 benchmark 守着，`bench/`）：

- 泛型单态化：实例化生成专用代码，没有虚表/装箱。
- 编译期格式化：常量 `fmt` 直接展开成 `fwrite` + 类型化打印。
- 数组是 8 字节指针 + 长度在类型里：传参不复制。
- 切片是 `{ptr, len}`：子切片零分配。
- 容器 `ArrayList`/`HashMap` 的底座是 `@memcpy/@memset` 与手写探测，无隐藏分配。
- 结构展开 `{any}` 由描述符驱动，编译期与运行时共用一份。

`bench/BASELINE.md` 是锚点（fib / mandel / mmat / str），`sh test.sh --baseline <tsv>` 做对比。

---

## 8. 注意项

### 8.1 语言层面的坑（按踩中频率）

1. **`{x}` 是一元组不是块**。块要交值只能靠收割：`tag: { …; tag: v; }`。
2. **没有 `break`**。跳出用收割，标签写在结构**前面**：`done: while (…) { done: v; }`。
3. **块末尾表达式不是返回值**。函数用 `return`；`if` 要值用表达式臂或收割。
4. **`null` 没有类型**。`let a = null;` 编译错，写落点 `let a: ?i32 = null;`。
5. **错误码从 1 起**。`(error)0` 编译错（0 = 无错误）。
6. **`defer` 与返回值**：`defer free(buf); return buf;` 返回悬空指针。
7. **`?T`/`!T` 不能嵌套**；两个都要表达就让"没有"也变成一个错误码。
8. **switch 语句位置所有臂同型**：语句 switch 统一写块臂 `模式: { … };`。
9. **枚举必须穷尽**（或 `else`）——这是保护，不是麻烦。
10. **HashMap 键不复制**：键切片必须比表活得久。
11. **浮点必须有整数部分**：`.5` 非法（`.0` 要留给元组下标）。
12. **`fn name() {}` 不存在**：一律 `const name = fn () … { … };`。
13. **整数溢出 trap**，不静默绕回。
14. **不能隐式数值转换**：`let b: i64 = a;`（`a: i32`）编译错，写 `(i64)a`。
15. **右值不能直接调 `*const Self`/`*Self` 方法**：先绑 `let`。
16. **`module("x")` 省 `.zx` 是允许的**，但显式写全路径更不容易歧义。
17. **`try` 只吃 `!T`**：`?T` 的空不是错误，用 `if` / `orelse` / `.?`。
18. **defer 体里不能再 return / 收割 / continue**；`try` 也不能用在 defer 里。
19. **switch 冒号后的裸名字一定读作捕获名**：`1: K;` 的值是匹配到的 `1`，不是常量 `K`。
    要常量就把名字括起来 `1: (K);`。（`if (c) x else y` 不受影响：bool 条件没载荷可捕。）

### 8.2 工程与工具链

- **模块解析链有三处**：编译器（`setup_search_paths`）、LSP（`modDirsInto`）、
  以及构建时的相对路径。改搜索路径要**同时**改编译器与 LSP，否则会出现
  "能编过但补不出来"（`spec/decisions.md` §80）。
- **Windows 上 zxls 拉起 zxc 必须经 `cmd /c`**：直连会断掉孙进程（zxfe）的
  stderr 句柄继承，诊断全丢。改 spawn 逻辑时不要"优化"掉这层。
- `.zxc-cache/` 与 `out/` 都该进 `.gitignore`；`vendor/` 相反，是源码的一部分。
- `bin/zxls.exe` 被编辑器占用时编不进去：`mv bin/zxls.exe bin/zxls.old.exe` 再编
  （运行中的 exe 可改名），编完删掉 `.old`。
- **不要混用 zig 工具链版本**：`zxls` 用 0.17 的 `std.Io` 接口，vendored 工具链是
  0.17.0；0.15 的 std 编不过。

### 8.3 已知限制（会变，但别按"它有"来设计）

| 限制 | 现状 | 影响 |
| --- | --- | --- |
| 并发 | `std.Task` 实例（coro/single/pool 物理隔离，§4.6）+ 实例自带 rendezvous 通信 + `std.atomic` | 无读写锁、无 select、无带缓冲通道；`close` 后句柄失效（槽位不复用）；根上下文通信是协作式 `would_block`；外部调用者需串行使用实例 |
| 调试信息 | 后端不生成 DWARF/PDB，`-g` 未暴露 | 单步调试不可用；打印/断言用 `std.debug`，位置内建零开销 |
| 包管理 | 没有网络依赖解析 | 依赖靠 `vendor/` 或 `deps` 手工放置 |
| 优化 | 只做 `-O0`/`-O2` 交给 C 编译器 | 没有 Zx 层优化（不做内联决策） |
| 约束/特征 | 无 trait/接口/约束 | 用 `union(enum)`、函数字段、`@typeInfo` 自检 |
| 元组下标 | 编译期 `.N` | 运行时下标要转数组/切片 |
| 反射 | 仅编译期 `@typeInfo` | 无运行时类型信息 |
| 平台 | 开发主力是 Windows；C 后端本身可移植 | 非 Windows 需自备 `ZX_CC` 与工具链路径 |
| VS Code | 无官方 client | 高亮与补全只在 Zed / 通用 LSP 客户端可用 |

---

## 9. 完成度

判断标准不是"文档写了什么"，而是"有没有能跑的东西守着"。

| 维度 | 状态 | 证据 |
| --- | --- | --- |
| 词法 / 语法 / 类型检查 | 完成 | 25 关键字、泛型单态化、错误集、编译期求值；`tests/good` 52 + `tests/bad` 95 + 多文件 |
| C11 后端 | 完成 | 7 个示例 + 正例编译成真二进制跑出期望输出；结构布局与 C 一致 |
| 自举 | 完成（阶段 1–4） | `bootstrap/zxfe.zx` 用 Zx 写前端，`zxc` 默认走它；`--fe=c` 只作种子与交叉验证 |
| 标准库 | 可用 | 8 个模块（std/str/map/fs/json/time/pool/task）+ 容器 + 格式化 + debug/atomic/Task 实例 |
| 工具链 | 可用 | `init/build/run/test/check/fmt` + `-h/-v` + 增量编译 + 动态/静态库 |
| 编辑器 | 可用 | tree-sitter 高亮/大纲 + zxls（补全/诊断/跳转/hover/签名/格式化/引用） |
| 回归 | 在守 | `sh test.sh`：文件类 233 项全过 + CLI 冒烟 6 项 + LSP 会话 16 项 |
| 性能 | 有基线 | fib/mandel/mmat/str 四个锚点，前端已修掉两处 O(n²) |
| 生态 | 起步 | 无包管理、无调试信息；调试内建（M52）、Task 实例并发（M58 重构后）已有 |

一句话结论：**作为"能写真实程序（编译器自己就是用户）"的系统语言是完整可用的；
作为"给别人用的语言产品"还差包管理与调试两件套。** 语言核心不再有已知的、
会逼你改设计的缺口：自举时暴露的 G1（`orelse`/`.?`）与 G2（char 模式）都已
补掉；并发在 M58 收敛成一个物理隔离的 Task 实例类型，双实例通信与 pool 压测各 20/20。

---

## 10. 诊断速查

格式（stderr，可被编辑器和脚本解析；首行之后是源码片段和指位）：

```
path:line:col: error[E0301]: initializer: expected `i32`, found `[]const u8`
 4 |     let x: i32 = "abc";
   |                  ^
  help: ...
  note: 'foo' declared here
 2 | const foo = 1;
   |       ^^^
```

- **源码片段**：紧跟首行，`^` 落在出错列。列号按字节算，渲染时换算显示
  宽度（UTF-8 多字节算 2 格、tab 展开 4），CJK 源码指位也对。
- **`help:`**：fix-it 建议——未知标识符/类型名/成员名给编辑距离最近的候选
  （OSA 距离，相邻换位算 1 次编辑；短名容 1 次、长名容 2 次）。
- **`note:`**：相关位置——重复声明带 `first declared here`，跨模块访问私有
  符号带另一份源码里的声明点。
- 诊断被去重或超过打印上限时，help/note 随主诊断一起丢弃，不会孤立出现。

| 码段 | 含义 | 典型诱因 |
| --- | --- | --- |
| `E01xx` | 词法 | 未闭合字符串/注释、非法字符、数字后缀 |
| `E02xx` | 语法 | `expected …`、声明边界、缺少分号 |
| `E03xx` | 类型 | 类型不匹配、参数个数、缺少字段、联合体初始化 |
| `E04xx` | 名字与作用域 | 未知标识符、私有成员、取地址、循环外 continue |
| `E05xx` | 编译期与流程 | 非常量、缺返回路径、栈逃逸、穷尽性、除零、保留错误码 |
| `E06xx` | 模块与标准库 | 找不到模块、循环导入、std 成员不存在 |
| `E07xx` | 泛型 | 实例化失败、类型参数误用 |
| `E08xx` | 格式化 | 占位符与实参不匹配、序列个数 |
| `E09xx` | 错误集 | 返回码不在集合、try 可能抛的码、anyerror 混入 |

每个码都能单独查：

```
zxc errors            # 按类别列出全部码
zxc explain E0304     # 单条：什么意思、通常怎么修（大小写不敏感），末尾给本文链接
```

完整参考见 [`docs/errors.md`](errors.md)：每条带触发条件、典型消息和示例。
**码一旦发布就不再改**，可以拿它搜索、贴进 issue、写进 CI 的失败判定。

诊断本身也带修法：

- 编译器知道具体改法时给精确建议（`help: did you mean \`count\`?`）。
- 不知道时退到该码的通用修法（`help: 加显式转换 \`@intCast\` …`），**每个码只印一次**——
  同类错误连成一片时重复同一句只是噪音；想看全了用 `zxc explain`。
- 编辑器里诊断上的码可以直接点开（`docs/errors.md` 的对应一条），靠语言服务器
  的 `codeDescription`；客户端不支持这个能力时不会发（见 `spec/decisions.md` §98）。

- 诊断会尽量多报（一次编译能报几个就报几个）；操作数已经报过错的地方
  （比如未知标识符参与算术）不再跟连锁的次生诊断，根因一条就够。
- 只看"能不能过"用 `zxc check <file>`；要看生成物用 `--emit-c` / `--ast`。

---

## 附录 A：关键字与保留字

```
const  let  var  fn  module  type  null  undefined
if  else  while  for  inline  switch  continue  return
defer  errdefer  struct  union  enum  error  pub  extern  orelse  try
and  or
```

`true` / `false` 是字面量。`@` 前缀保留给内建。`_` 可用于丢弃。

## 附录 B：与 C / Zig 的一页对照

| 你熟悉的写法 | Zx 的写法 |
| --- | --- |
| `*p` / `p->x` | `p.0` / `p.x`（`.` 自动穿透一层） |
| `T a[N]` | `*[N]T`（8 字节指针） |
| `(int[]){1,2,3}` | `&.{1, 2, 3}` |
| `break` / `goto` | 标签 + 收割 `done: tag: v;` |
| `printf("%d", x)` | `std.println("{}", x)`（编译期展开） |
| `struct { … }` + typedef | `const P = struct { … };` |
| `#define` / 模板 | 编译期 `const` + `fn(T: type) type` |
| `errno` / 返回码 | `!T` / `E!T` + `try` / `orelse` |

更细的差异（布局、`?T` 标签顺序、块求值）见 `docs/guide.md` §17 与
`spec/zx-spec.md` §15。
