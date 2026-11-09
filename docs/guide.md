# Zx 语言入门指南

> 面向新人的完整教程：从第一个程序到类型系统、函数、接口（模块）、错误处理与标准库。  
> 本指南对应语言规格 v0.28；所有示例都取自编译器回归测试里真跑过的代码。  
> 想查规则的精确表述，看 `spec/zx-spec.md`；本文负责讲"为什么"和"怎么用"。

---

## 目录

1. [Zx 是什么](#1-zx-是什么)
2. [五分钟上手](#2-五分钟上手)
3. [词法基础](#3-词法基础)
4. [变量：const / let / var](#4-变量const--let--var)
5. [基础类型](#5-基础类型)
6. [复合类型](#6-复合类型)
7. [控制流](#7-控制流)
8. [空值与错误处理](#8-空值与错误处理)
9. [函数](#9-函数)
10. [泛型](#10-泛型)
11. [模块与 pub（Zx 的"接口"）](#11-模块与-pubzx-的接口)
12. [格式化输出](#12-格式化输出)
13. [内存管理](#13-内存管理)
14. [标准库速查](#14-标准库速查)
15. [内建函数（@ 前缀）](#15-内建函数-前缀)
16. [常见陷阱速查](#16-常见陷阱速查)
17. [与 C / Zig 差异速查](#17-与-c--zig-差异速查)
18. [下一步读什么](#18-下一步读什么)

---

## 1. Zx 是什么

Zx 是一门**编译到 C11** 的系统级语言：`zxc` 把 `.zx` 源码翻译成可读的 C，再用任意  
C 编译器编成本机码。这带来三件事：

- **零依赖运行**：产物就是普通可执行文件，没有 VM、没有 GC、没有运行时安装。
- **与 C 无缝互操作**：`extern fn` 直接声明 libc / 系统 API，反向也能被 C 调。
- **可自举**：编译器前端正用 Zx 重写自己（见 `spec/bootstrap.md`），语言能力由  
  "能写编译器"这条硬标准验证。

设计上它从 Zig 借了形状（复合字面量、泛型即编译期函数、defer），从 C 借了内存模型  
（栈默认、显式分配器、指针裸奔），然后砍掉了三样东西：

| 砍掉的              | 换成的                                      |
| ---------------- | ---------------------------------------- |
| `break` / `goto` | **收割语句** `tag: value;`（带值离开任意标签作用域，§7.5） |
| 异常 / try-catch   | **错误值** `!T`（错误码就是返回值，§8）                |
| 隐式类型转换           | **显式转换** `(T)e`（§5.3）                    |

一句最核心的话：**`{x}` 是一元组，不是块**——所以块不能"自动继承末尾表达式的值"，  
一切值都由明确语法产生（`return`、收割、表达式臂）。这让每段代码的语义都完全由  
语法决定，没有需要猜测的角落。

---

## 2. 五分钟上手

编译器在 `bin/zxc.exe`（自带 vendored 工具链，无需另装 C 编译器）。

```sh
# 编译并运行
./bin/zxc.exe hello.zx -o hello
./hello

# 只做类型检查（不产出二进制）
./bin/zxc.exe hello.zx --check

# 看词法单元 / AST / 生成的 C
./bin/zxc.exe hello.zx --tokens
./bin/zxc.exe hello.zx --ast
./bin/zxc.exe hello.zx --emit-c
```

第一个程序：

```zx
// hello.zx
const std = module("std");

const main = fn () void {
    std.println("hello, {}!", "zx");
};
```

三个要点：

- **函数只有一种定义写法**：`const 名字 = fn (参数) 返回类型 { … };`。函数是值，  
  绑到名字上才叫得出来（§9）。
- **入口是 `main`**，返回 `void` / 整数 / `!void` 都行（§11）。
- `module("std")` 引入标准库命名空间（§11）。

注意：Zx 也可以写 `fn name(…)`？**不可以**——`fn name(…) {}` 只留给没有函数体的  
`extern` 声明（见 §9）。看到旧资料里的 `fn main() {}` 写法，那是 v0.24 之前的。

---

## 3. 词法基础

### 3.1 注释

```zx
// 行注释
/* 块注释，不嵌套 */
```

### 3.2 关键字（全部 25 个）

```
const  let  var  fn  module  type  null  undefined
if  else  while  for  inline  switch  continue  return
defer  struct  union  enum  error  pub  extern  orelse  try
```

注意：**没有 `break`**。`true` / `false` 是布尔字面量，不是关键字。

### 3.3 字面量

```zx
42               // 整数，默认 i32（var 无标注时）
42u64            // 后缀定类型：u8 u16 u32 u64 u128 i8 i16 i32 i64 i128 usize isize
0xFF  0b1010  0o17   // 十六进制 / 二进制 / 八进制
1_000_000        // 下划线分组
3.14             // 浮点，默认 f64
3.14f32  1e9     // 浮点后缀：f16 f32 f64 f128
'a'  '\n'  '中'   // char：1..4 字节 UTF-8，参与整数运算时就是码位
"hi\n"           // 字符串；{} 占位符原样保留（格式化层的事，§12）
"\x41  \u4e2d"   // 十六进制与 Unicode 转义
```

字符串转义全集：`\n \t \r \0 \\ \" \' \a \b \f \v \e \xHH \uXXXX`。

### 3.4 两个特殊标点

- **`.{`** 是一个词法单元：复合字面量的开头（`&.{1, 2, 3}`、`Person{…}` 的无类型名形式）。
- **`..`** 是范围 token（`for (0..n)`），所以 `1..2` 不会被读成 `1.` 浮点。
- 反过来，**浮点必须有整数部分**：`.5` 非法，`0.5` 才行——因为 `t.0.0` 这种元组  
  下标链需要 `.` + 数字保持原义。

---

## 4. 变量：const / let / var

三种绑定，各管一件事：

| 形式              | 求值时机      | 存储                   | 可写 |
| --------------- | --------- | -------------------- | -- |
| `const X = e;`  | **必须编译期** | 无运行时存储（折叠成常量 / 静态数据） | 否  |
| `let X: T = e;` | 运行时       | 栈 / 字段               | 否  |
| `var X: T = e;` | 运行时       | 栈 / 静态 / 字段          | 是  |

```zx
const MAX = 100;              // 编译期常量；类型可省（字面量推断）
let n: usize = count();       // 运行时只读
var i: usize = 0;             // 可变
```

- **`const` 是"编译期一定算得出"**，不是"我不想改"。函数、类型、模块、泛型  
  全走 `const`。编译期可以跑完整的 `if` / `while` / `switch`，也能调用编译期函数。  
  无标注的整数字面量缺省落 `i32`，要别的宽度写在标注上（`const N: usize = 100;`）。
- **`let` 是运行时只读**。函数参数天然只读，不用再写 const。
- **`var` 是唯一的可写绑定**。读代码时一眼分清什么是状态。
- `var` 无类型标注时：整数字面量默认 `i32`，浮点默认 `f64`，其余按初值类型。
- 右值（函数返回值、字面量）不能直接调 `*const Self` / `*Self` 方法——先绑到  
  `let` 再调。

---

## 5. 基础类型

### 5.1 类型总表

| 写法                              | 大小（64 位）         | 说明                    |
| ------------------------------- | ---------------- | --------------------- |
| `u8 u16 u32 u64 u128` / `usize` | 1/2/4/8/16 / 8   | 无符号整数                 |
| `i8 i16 i32 i64 i128` / `isize` | 1/2/4/8/16 / 8   | 有符号整数                 |
| `f16 f32 f64`                   | 2/4/8            | IEEE-754 浮点           |
| `bool`                          | 1                | `true` / `false`      |
| `void`                          | 0                | 空类型（等于空元组 `{}`）       |
| `type`                          | 0                | 类型值，仅编译期（§10）         |
| `{T1, T2}`                      | 字段之和             | 元组（§6.1）              |
| `*[N]T`                         | 8                | 数组：指向 N 个连续 T（§6.2）   |
| `*T` / `*const T`               | 8                | 裸指针 / 只读指针（§6.3）      |
| `[]T`                           | 16               | 切片 `{ptr, len}`（§6.2） |
| `struct{…}`                     | 按声明              | 结构体（§6.4）             |
| `enum{…}` / `enum(T){…}`        | sizeof(T)，默认 u32 | 枚举，就是整数（§6.5）         |
| `union{…}`                      | 最大成员（+标签）        | 联合体（§6.6）             |
| `?T`                            | sizeof(T)+1（对齐后） | 空值联合体（§8.1）           |
| `!T`                            | sizeof(T)+2（对齐后） | 错误联合体（§8.2）           |
| `fn(P…) R`                      | 8                | 函数类型（§9）              |
| `error`                         | 2                | 就是 u16，0 表示无错误        |
| `comptime_int` 等                | —                | 字面量类型，写不出来            |
| `noreturn`                      | 0                | 必不返回（`@trap()` 等）     |

### 5.2 数值语义（R10）

Zx 的整数运算**不静默绕回**：

- **溢出**：安全模式下运行时 trap（abort 并报错），编译期常量直接编译错。  
  确实要绕回用位运算（`&` 掩码）；位重解释走指针转换：`(*i32)(&f).0`。
- **除零**：trap。`a / 0` 在编译期能定就直接编译错。
- **有符号右移**是算术移位，左移掩到类型宽度。

### 5.3 只有显式转换（R4）

```zx
let a: i32 = 5;
let b: i64 = a;          // ✗ 编译错误：不能隐式加宽
let b: i64 = (i64)a;     // ✓ 显式转换
let p: usize = (usize)ptr_int;
```

同宽符号互转 `(i32)u` / 位宽收窄都要求写出来。字面量例外：`42` 落到任何整数  
类型都行（comptime_int 会自适应落点）。

---

## 6. 复合类型

### 6.1 元组：`{…}`

类型 `{T1, T2, …}`，值内联（不是指针），元素类型可以不同。

```zx
let t = {1, 2.5, "hi"};      // {comptime_int, comptime_float, []const u8}
t.0                          // 1 —— 下标是编译期的
t.len                        // 3，编译期
t.size                       // 总字节数，编译期
let {a, b, c} = t;           // 解构
```

**`{x}` 是一元组，不是块。** 花括号的身份只看顶层内容：

| 写法                 | 身份         | 值            |
| ------------------ | ---------- | ------------ |
| `{1, 2}`           | 二元组        | `{i32, i32}` |
| `{x}` / `{x,}`     | 一元组        | `{T}`        |
| `{}`               | 空元组 = void | `{}`         |
| `{ f(); }`         | 块（顶层有 `;`） | `{}`         |
| `{ let a = 1; a }` | 块（有声明）     | `{}`         |

块想交出值只有一条路：**标签 + 收割**（§7.5）。要跳过判定规则直接写字面量，  
用 `.{`：

```zx
let p: Point = .{ .x = 1, .y = 2 };   // 有期望类型，.{…} 就是那个类型
```

### 6.2 数组与切片

**数组 `*[N]T` 是 8 字节指针**——数量 N 写在类型里，但表示就是地址：

```zx
var arr: *[4]i32 = &.{1, 2, 3, 4};   // &.{…} 数组字面量
arr.0 = 10;                          // 编译期下标：.0 解引用首元素
arr[2] = 30;                         // 运行时下标
arr.len                              // 4，编译期
arr.size                             // 16，编译期（字节数）
```

`*[3]i32` 和 `*[4]i32` 是**不同的类型**——长度错误在编译期就报。

**切片 `[]T`** 是 `{ptr: *T, len: usize}`（16 字节），长度随值走：

```zx
const sum = fn (xs: []const i32) i32 {
    var s: i32 = 0;
    for (xs) x { s = s + x; }
    return s;
};
sum(arr[0..])            // 数组 → 切片：零开销，只是把指针配上长度
sum(arr[1..3])           // 子切片，左闭右开
```

|      | 元组 `{…}`         | 数组 `&.{…}`        |
| ---- | ---------------- | ----------------- |
| 类型   | `{i32, f32}` 值内联 | `*[N]T` 8 字节指针    |
| 元素类型 | 可以不同             | 必须相同              |
| 访问   | `.N`（编译期）        | `.0` 或 `[i]`（运行时） |
| 切片   | 无                | `[a..b]` → `[]T`  |
| 传参   | 按值复制整个           | 只传地址              |

字符串字面量 `"hi"` 的类型是 `*[N]const u8`——指向定长数组的指针，`N` 是字节
数（`"hi".len == 2`，编译期常量）。赋给 `[]const u8` 时自动带出长度变成切片，
所以字符串传参照旧是"指针 + 长度"：`let s: []const u8 = "hi";`。要裸指针就写
`"hi".ptr`（或直接传字面量，数组指针会退化成 `*const u8`）。

### 6.3 指针

```zx
var x: i32 = 5;
let p: *i32 = &x;        // 取地址
p.0 = 7;                 // 解引用：.0 是"元素 0"
p[0] = 7;                // 等价写法
let q = &p;              // *​*i32
```

三条规则：

1. **`.0` 解引用**。指针、数组、元组统一用 `.下标`。
2. **`.` 自动穿透一层指针**：`p.name` ≡ `p.0.name`。写链式访问不用一路 `.0`。
3. `*const T` 透过它写入是编译错误；`&x` 对 `var` 得 `*T`，对 `let`/`const` 得 `*const T`。

指针算术用 `p + i`（按元素步进）；整数 ↔ 指针用 `(usize)` / `(*T)` 指针转换。

**生命周期只有一条硬规则**：指向当前栈帧的指针不许 `return` 出去（编译器强制）。  
其余靠约定——Zx 没有借用检查，这是它简单的一部分。

### 6.4 结构体

```zx
const Person = struct {
    name: []const u8,
    age: i32 = 0,               // 字段默认值
};
```

构造用 `.字段 = 值`，缺的字段用默认值补（没有默认值的必须给）：

```zx
var p = Person{ .name = "ann" };   // age 取默认 0
p.age = 31;
```

有期望类型时用匿名形式 `.{…}`（R6）：

```zx
var p: Person = .{ .name = "ann" };
take(.{ .name = "bob" });          // 实参位置同理
```

结构体体是作用域，能放静态字段、常量和方法（§6.7）：

```zx
const V = struct {
    var count: i32 = 0;            // 静态字段：全程序一份
    const LIMIT = 100;             // 类型上的编译期常量
};
V.count = 20;
```

**自引用必须经过指针**（值类型里放自己大小无穷）：

```zx
const Node = struct { v: i32, next: ?*Node };   // ✓
```

布局：声明顺序 + 自然对齐，与 C 完全兼容（`struct(packed)` 紧凑无填充）。

### 6.5 枚举：就是整数

```zx
const Color = enum { red, green = 5, blue };   // 底层 u32，blue == 6
const Small = enum(u8) { a, b };               // 指定底层类型
```

- 变体就是整数：`Color.red == 0`；显式互转 `(u32)Color.red`、`(Color)5`。
- 枚举体是作用域，可以放常量、全局和方法：

```zx
const Color = enum {
    red, green, blue,

    const FIRST = Color.red;
    var calls: u32 = 0;

    const name = fn (self: Self) []const u8 {
        return switch (self) {
            .red: "red";
            .green: "green";
            .blue: "blue";
        };
    };
    const count = fn () u32 { return 3; };     // 关联函数：没有 self
};

Color.red.name();    // "red" —— 方法
Color.count();       // 3 —— 关联函数（没有接收者）
```

### 6.6 联合体（带标签）

**首选 `union(enum)`**——变体名就是隐式枚举，标签自动按声明顺序 0..n-1 填：

```zx
const Shape = union(enum) {
    circle: f64,
    rect:   { f64, f64 },          // 载荷本身是元组也可以
};

let s = Shape{ .circle = 2.0 };    // 构造只写成员名，标签编译器填

let area = switch (s) {
    .circle: r  a: { a: 3.14159 * r * r; };   // r 捕获载荷；捕获臂配标签块交值
    .rect:   wh a: { a: wh.0 * wh.1; };
};
```

也可以 `switch (s.tag)` 按标签匹配、`s.成员` 访问载荷（两种都对）：

```zx
let area2 = switch (s.tag) {
    .circle: 3.14159 * s.circle * s.circle;
    .rect:   s.rect.0 * s.rect.1;
};
```

- **穷尽性检查**：漏掉成员又没 `else` 是编译错误。
- 读载荷 `s.circle`；要修改取 `&s.circle`（`s` 必须可变）。
- `?T` 和 `!T` 本质也是带标签联合体（§8），但日常用 `if` 捕获。
- **不带标签的普通联合体**存在（`union { i: i32, f: f32 }`），读错成员是 UB，  
  少用。

### 6.7 方法

方法就是"第一个参数是接收者的普通函数"，写在与类型同体的作用域里：

```zx
const Vec = struct {
    x: i32,
    y: i32,

    const add = fn (self: Self, o: Self) Self {
        return Self{ .x = self.x + o.x, .y = self.y + o.y };
    };
    const shift = fn (self: *Self, d: i32) void {
        self.x = self.x + d;         // 要改自己就用 *Self
    };
    const len2 = fn (self: *const Self) i32 {
        return self.x * self.x + self.y * self.y;
    };
};

var a = Vec{ .x = 1, .y = 2 };
a.shift(10);          // ≡ Vec.shift(&a, 10) —— 完全等价的糖
let b = a.add(a);     // add 按值收拷贝，返回新 Vec
b.len2();             // 右值要先绑 let，才能调 *const Self 方法
```

接收者三选一：

| 写法                  | 含义      | 调用要求          |
| ------------------- | ------- | ------------- |
| `self: Self`        | 拿一份拷贝   | 什么都能传         |
| `self: *Self`       | 改的就是调用者 | 必须可变绑定（`var`） |
| `self: *const Self` | 只读不拷贝   | 能取地址即可        |

- `Self` 是类型体内的别名，等于这个类型自己；泛型体里也用它（§10.2）。
- **第一参数不叫 `self` 就是关联函数**（如 `Color.count()`、`ArrayList.init(al)`）。
- 方法不能与字段重名。

---

## 7. 控制流

### 7.1 if

```zx
// 语句
if (n > 0) { use(n); } else { report(); }
if (n > 0) use(n); else report();      // 单语句臂可省花括号

// 值形式一：表达式臂
let max = if (a > b) a else b;

// 值形式二：标签加在 if 上，臂里收割
let sign = s: if (x > 0) {
    s: 1;
} else if (x < 0) {
    s: -1;
} else {
    s: 0;
};

// 空值/错误捕获（§8）
if (maybe) v { use(v); } else { /* null 或错误 */ }

// 捕获即返回值（v0.28）：臂要交的就是载荷本身时，名字后面就不用写了
let v = maybe orelse 0;      // 等价写法之一
let w = if (maybe) v;        // 有值就是 v，没有是零值
let x = if (maybe) v else 0;
```


条件必须是 `bool`（没有真值语义）。**捕获名紧跟 `{`，标签名紧跟 `:`**——  
`if (u) v { … }` 的 `v` 是载荷拷贝，`if (c) t: { … }` 的 `t` 是块的名字。

### 7.2 while

```zx
var i: usize = 0;
while (i < 10) { i = i + 1; }          // 没有自增子句，自增写在体内

// 遍历可选值：有值继续，空/出错结束
while (it.next()) v { total = total + v; } else { std.print("done\n"); }

// 带值跳出：标签写在 while 前面（这就是 Zx 的 break）
const w = done: while (true) {
    n = n + 1;
    if (n == 7) { done: n * 10; }
};
```

**标签写在循环关键字前面**——标签是**循环**的名字，收割 `done: v;` 结束整个循环  
并交值。没有 break；收割就是更强的 break。

### 7.3 for

序列只有两种：**切片** 和 **整数范围 `lo..hi`**（左闭右开）。多绑定时序列个数 =  
绑定个数，且必须等长：

```zx
for (xs) x { sum = sum + x; }                 // 遍历切片
for (0..n) i { buf[i] = 0; }                  // 整数范围
for (xs, 0..) x, i { buf[i] = x; }            // 值 + 下标（0.. 上界取别的序列长度）
for (xs, ys) a, b { dot = dot + a * b; }      // 两个等长序列
for (0..rows, 0..cols) r, c { grid[r][c] = 0; }
```

带值的 for（找第一个满足条件的下标，没有 break 就这么写）：

```zx
const idx = hit: for (0..n) i {
    if (xs[i] == target) { hit: (i32)i; }
} else -1;                                     // 正常跑完（没找到）的值
```

数组 `*[N]T` 不是序列，先 `arr[0..]` 变切片（零开销）。

**`inline for`** 是编译期展开，不是循环——按编译期长度把体克隆 N 份，绑定是  
编译期常量，配合 `@typeInfo` 做元编程（§15）。

### 7.4 switch

```zx
// 枚举：模式 .成员，表达式臂给值，必须穷尽（或有 else）
let name = switch (color) {
    .red:   "red";
    .green: "green";
    else:   "other";
};

// char 模式：词法器的写法（逗号并列同臂多模式）
switch (c) {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9': { nd = nd + 1; };
    ' ', '\t', '\n': { };
    else: { };
}

// 整数 / 负数 / bool
switch (n) {
    -2, -1: { };
    0:      { };
    else:   { };
}

// 块臂 + 标签收割交值
let area = switch (s.tag) {
    .circle a: {
        if (s.circle < 0) { a: (error)Err.neg; }
        a: 3.14159 * s.circle * s.circle;
    };
    .rect:  s.rect.0 * s.rect.1;
};
```

规则要点：

- **每条臂以 `;` 结尾（含末臂）**；逗号只作同臂模式并列（`'a', 'b': …`）。
- **冒号后紧跟 `{` 是捕获**（联合体臂捕获载荷，枚举臂捕获主语值），**紧跟 `:` 是  
  标签**（块收割用）；**什么都不跟**（紧跟 `;` 或 `}`）就是"臂值 = 捕获值"
  （v0.28）：`switch (u) { .some: v; .none: 0; }`。
- **穷尽性**：枚举、带标签联合体、`?T`、`!T`、bool 的模式必须列全或有 `else`。
- switch 是表达式，各臂值必须同型（块臂全 `{}` 也行——语句位置统一写块）。

### 7.5 收割、标签与 continue（没有 break 的世界）

**收割语句 `tag: expr;`** = 给标签作用域设值并立即离开它。跳出和带值是同一条  
语句，这就是 Zx 用来替代 break/goto 的东西：

```zx
// 块算值 + 提前退出
let area = shape: {
    if (w <= 0) { shape: 0; }
    shape: w * h;
};

// 一次跳出两层循环
let hit = hit: for (0..rows) i {
    for (0..cols) j {
        if (grid[i][j] == 0) { hit: {i, j}; }   // 值是下标元组
    }
} else null;

// 深处结束外层循环：收割穿过中间的无标签结构直接命中目标
done: for (xs[0..]) x {
    if (x == 0) { done: {}; }
    total = total + x;
}
```

- 标签是独立命名空间；`标签名: 值` 是收割，`标签名: 结构` 是打标签。
- **标签必须写在它要离开的结构前面**：块 `tag: {…}`、循环 `tag: while (…)` /  
  `tag: for (…)`。
- 循环/标签块的 `else` 只在**正常跑完**时执行，收割离开会跳过它。
- `continue;` 跳到本轮末尾；`continue :tag;` 直接进入外层标签循环的下一轮。

### 7.6 defer

离开**所在块**时执行，LIFO（后声明的先跑）：

```zx
const locked = fn (m: *Mutex, f: fn() void) void {
    m.lock();
    defer m.unlock();        // 与 lock 成对，任何路径退出都执行
    f();
};
```

- 表达式在**退出时**求值，不是声明时。
- `return` 的值先算出来，`defer` 后跑——所以 `defer free(buf); return buf;`  
  是**错的**（返回值指向已释放内存）。defer 只适合"清理不影响返回值"的场景。
- defer 体内不能 return / 收割 / continue。

---

## 8. 空值与错误处理

Zx 没有异常。两种"可能没有"都用带标签联合体表示，**载荷在前、标签在后**：

```
?T   ≡ union { some: {T,true}, none: {false} }     // 标签是 bool
!T   ≡ union { ok: {T,0}, … }                       // 标签是 error(u16)，0 = 无错误
```

```zx
let a: ?i32 = 5;
let b: ?i32 = null;
let c: !i32 = 5;                    // tag 0，成功
let d: !i32 = (error)1;             // 出错，码 1
```

要点：

- `null` 只是给 `?T` 填空的字面量，**没有自己的类型**——`let a = null;` 是编译错。
- `?T` / `!T` **不能嵌套**（`??T`、`?!T`、`!?T` 都是编译错误）。两个都要表达时，  
  让"没有"也成为一个错误码，或者把 `?T` 当结构体字段。
- **错误码 0 保留给"无错误"**：命名错误码从 1 起（`enum(u16) { negative = 1 }`）。  
  否则出错会静默变成"成功 + 未初始化载荷"。
- 标签为空/出错时读载荷是 UB；取地址永远安全（`&x` 就是载荷地址，零偏移）。

### 8.1 四种取值出口

| 写法                            | 左值          | 空/出错时                  | 用在       |
| ----------------------------- | ----------- | ---------------------- | -------- |
| `if (u) v { … } else e { … }` | `?T` / `!T` | 走 else，`e` 捕获 null/错误码 | 就地处理     |
| `a orelse b`                  | `?T` / `!T` | 取 `b`（惰性求值）            | 有合理默认值   |
| `a.?`                         | `?T` / `!T` | 打印说明后 abort            | 这里不该出错   |
| `try e`                       | `!T`        | 带着错误码 return           | 中间层，向上传播 |

```zx
// if 捕获：v 是载荷拷贝；else 分支 e 捕获错误码
if (parse(s)) n {
    use(n);
} else e {
    std.println("bad input, code={}", (usize)e);
}

// orelse：右结合，b 惰性
let port = cfg.port orelse 8080;

// .?：断言这里必须有
let p = n.?;              // 空了就是 bug，直接 abort

// try：出错就带着同一个码往上返回
const read_head = fn (p: []const u8) !i32 {
    let fd = try open_file(p);       // 一行顶 if(…){…}else e{return e;}
    return fd + 1;
};
```

`try` 的约束：左值必须是 `!T`（`?T` 的"空"不是错误）；所在函数必须返回 `!U`；  
defer 体里不能用。

### 8.2 最外层：`main` 收下一切

```zx
const main = fn () !void {
    try run();        // 出错 → 进程退出码就是错误码
};
```

`main` 返回值到退出码的映射：`void` → 0；整数 → 那个值；`!void` → 标签即错误码

|                                                |   |     |
| ---------------------------------------------- | - | --- |
| （0 成功）；`?T` → 有值 0 空 1。错误码直接对得上 shell 的 \`&& / |   | \`。 |

### 8.3 错误集：把"只会出这些错"写进类型

`!T` 是 anyerror——任何码都收，编译器帮不上忙。给返回类型加一个错误集，
承诺就变成了检查：

```zx
const IoError = error{ open, read, write };   // 集合；码按名字自动编号

const open_file = fn (p: []const u8) IoError!i32 {
    if (p.len == 0) { return error.open; }    // error.open 就是那个码
    return 100;
};

const read_it = fn (p: []const u8) IoError!i32 {
    let fd = try open_file(p);                // {open} ⊆ IoError ✓
    return fd + 1;
};
```

- **集合协变**：`{open} ⊆ {open, read}`，窄集合的值可以交给宽集合；反过来不行。
- 编译器守三件事：`return` 的码（E0901）、`try` 可能抛的码（E0902）、
  anyerror 抛进声明了集合的函数（E0903，必须显式放宽返回类型）。
- **不改变布局**：`E!T` 和 `!T` 在内存里一模一样，只多一份编译期信息；
  所以存量代码不用改，`(error)Err.x` 旧写法照旧可用。
- `?T` 的 `try` 抛保留码 `error.null`（65535），而 `error{null}` 写不出来
  ——所以带集合的函数不能 `try` 可选值，要么就地 `if` / `orelse` 处理。

---

## 9. 函数

### 9.1 唯一的定义写法

```zx
const add = fn (a: i32, b: i32) i32 {
    return a + b;
};

const fact = fn (n: i32) i32 {        // 声明形式允许自递归
    if (n <= 1) { return 1; }
    return n * fact(n - 1);
};
```

- `fn name(…) {}` **不存在**（v0.24 起）。`fn` 后面永远是参数表；  
  裸 `fn (…) R;` 只用于 `extern`：

```zx
extern fn system(cmd: *const u8) i32;    // 声明 libc / 外部符号，无函数体
```

- 函数类型是值：`fn(i32, i32) i32`，8 字节，可当参数可当字段（回调）。
- 无重载、无默认参数、无变参；多返回值用元组 `let {q, r} = divmod(a, b);`。
- **函数体是块**，末尾表达式不是返回值——`return` 是唯一出口。
- 前向引用可用：函数可以调用后面才定义的函数（同文件）。

### 9.2 每条路径都要返回

返回类型非 void 时，编译器做到达性分析：

```zx
const bad = fn (n: i32) i32 {
    if (n > 0) { return 1; }        // ✗ n <= 0 掉出去了
};
```

能拦住路径的只有：`return` / `continue`、`noreturn` 表达式（`@trap()`）、  
`while (true)`、每条出路都拦住的 if / 穷尽的 switch。不可达代码同样是编译错误。  
**`main` 例外**：掉出末尾 = 返回 0 / 无错误。

### 9.3 参数

```zx
fn sumSlice(xs: []const i32) i32         // 切片：长度随值走（推荐）
fn sumN(n: usize, p: *const [n]i32) i32  // 定长数组：n 是编译期参数
```

参数是只读槽位——函数体内不能赋值，要可变就 `var t: T = a;`。实参全是编译期  
值时整个调用在编译期完成（这也是泛型的原理）。

---

## 10. 泛型

`type` 是一等编译期值，**泛型就是编译期函数**，靠单态化实现：

```zx
const List = fn (T: type) type {
    return struct {
        buf: *T,
        len: usize,
        cap: usize,

        const push = fn (self: *Self, v: T) bool { … };   // Self = List(T)
    };
};

const xs = List(i32).init(std.heap.page);   // List(i32) 是一次编译期调用，结果是类型
```

- 类型参数用 `T: type`，也可以带编译期值参数：`Buffer(T, n)` 里 `n: usize`——  
  `Buffer(u8, 4)` 和 `Buffer(u8, 8)` 是不同类型，长度错误编译期报。
- 泛型体里**写 `Self`**，不要重写自己的名字（`List(T, cap)` 会无限套娃报错）。
- 实例的打印名跟构造式走（`List(i32)`），不跟别名走。
- 没有 trait / 约束：需要约束时用编译期 `if` + `@typeInfo` 自检（§15）。
- 每次实例化生成独立代码——这就是单态化，零运行时开销。

---

## 11. 模块与 pub（Zx 的"接口"）

Zx 没有 class/interface 的继承体系，它的接口边界就是**模块**：一个文件一个模块，  
`pub` 决定哪些名字对外可见。

### 11.1 导入与导出

```zx
// util.zx
pub const VERSION: u32 = 3;
pub const add = fn (a: i32, b: i32) i32 { return a + b; };
const secret = fn (n: i32) i32 { return n * 2; };    // 私有：模块外不可见

// main.zx
const util = module("util.zx");
util.VERSION;          // ok
util.add(1, 2);        // ok
util.secret(1);        // ✗ 'secret' is private to module 'util'
```

规则：

1. `module(path)` 路径**相对调用者所在文件**；`"std"` 是内建模块。
2. 同一路径只加载一次；循环导入直接报错。
3. 模块顶层只能有声明（`const` / `let` / `var` / `fn`），没有自由语句。
4. **没有单独的 import 语法**——`const` 绑定模块成员就是别名，函数、类型、  
   常量、命名空间一视同仁：

```zx
const std = module("std");
const println = std.println;       // 函数别名
const HashMap = std.map.HashMap;       // 泛型类型别名
const heap = std.heap;             // 命名空间本身也能绑
```

别名只是指向：声明还是模块里那一份，`var` 还是同一个变量。

### 11.2 类型即接口

需要"多态"时，Zx 的做法不是继承，而是三种普通手段：

1. **带标签联合体**（§6.6）——封闭集合的多态：

```zx
const Shape = union(enum) { circle: f64, rect: {f64, f64} };
// switch 穷尽处理，漏一个变体编译期就报
```

1. **函数类型字段**——开放集合的多态（回调 / vtable）：

```zx
const Allocator = struct {
    alloc: fn(usize) ?*u8,
    free:  fn(*u8, usize) void,
};
// 任何符合签名的函数都能填进来；std.heap.page 就是现成实现
```

1. **泛型 + `@typeInfo`**——编译期多态：要求"有这些字段"用  
   `@hasField(T, "len")` 编译期自检，缺了在实例化点报错。

---

## 12. 格式化输出

```zx
std.print("x = {}\n", x);            // stdout，不换行
std.println("x = {}", x);            // 带换行
std.format(alloc, "{}:{}", i, v);    // → ![]u8，分配到堆
std.io.print(h, "{s}\n", s);         // 写任意句柄（一律运行时解析）
```

格式串语法：

| 占位符                   | 含义               |
| --------------------- | ---------------- |
| `{}`                  | 按顺序取下一个参数，默认格式   |
| `{0}`                 | 按索引取（可与 `{}` 混用） |
| `{d}` / `{x}` / `{b}` | 十进制 / 十六进制 / 二进制 |
| `{s}`                 | 字符串（切片或字符串字面量）    |
| `{c}`                 | 字符（整数按码位输出）      |
| `{{` `}}`             | 字面量花括号           |

- **`fmt` 是编译期常量时整个格式化在编译期展开**——运行时零解析零分配，生成的  
  C 就是几条 `fwrite` + 类型化打印调用。动词与实参类型配错是编译错误。
- `fmt` 是运行时字符串时回落到运行时解析器，语义完全一致。
- 默认格式对聚合值直接展开：`Point{x: 1, y: 2}`、`some(5)`、`ok(7)`、  
  `[10, 20, 30]`；嵌套字符串带引号；指针只打地址不递归。

---

## 13. 内存管理

无 GC、无隐藏分配。**栈是默认**，堆分配必须显式传分配器：

```zx
const alloc = std.heap.page;         // 现成分配器（malloc/free）

let raw = alloc.alloc(1024);         // ?*u8 —— 失败给 null
if (raw) p {
    let buf = p[0..1024];            // 裸指针 + 上界 = 切片
    // … 用 buf …
    alloc.free(p, 1024);
}
```

- **分配器是一个普通结构体值**（`alloc` / `free` 两个函数字段），可以自己实现  
  （`tests/good/arena.zx` 是个 40 行的 bump 分配器示例）。
- `std.stackAlloc(T, N)` 栈上拿数组；`std.heapAlloc(alloc, T, N)` 堆上拿数组，  
  OOM 直接 abort。
- 清理靠 **`defer`**（§7.6），不靠析构函数，没有 RAII。
- 生命周期两条：栈指针不许 return 出去（编译器强制）；其余靠约定。
- `?T` / `!T` 的 `&x` 拿到的是载荷地址（布局就是为这个设计的）。

---

## 14. 标准库速查

编译器随附 `std/`（七个 .zx 文件：std / str / map / fs / json / time / pool，
全是普通 Zx 代码——它们同时是"怎么写泛型  
容器"的标准样例）。

### 14.1 ArrayList(T)

```zx
var xs = std.ArrayList(i32).init(std.heap.page);
defer xs.deinit();

xs.push(7);                      // bool；容量不够自动翻倍
xs.append(chunk[0..]);           // 批量追加切片（一次 memcpy）
xs.insertAt(i, v);  xs.removeAt(i);
xs.items();                      // []T —— 拿切片去遍历
xs.get(i);  xs.set(i, v);        // 带边界检查，返回 ?T / bool
xs.pop();  xs.first();  xs.last();
xs.reverse();  xs.resize(n, fill);  xs.swap(i, j);  xs.clear();
```

### 14.2 HashMap(V)（字符串键）

```zx
var m = std.map.HashMap(u64).init(std.heap.page);
defer m.deinit();

m.put(key, v);                   // bool；负载过 3/4 自动翻倍
if (m.get(key)) v { use(v); }    // ?V，捕获式取值
m.contains(key);  m.count();  m.clear();
```

要点：**键不复制**——表里存的就是那个切片，字符串必须比表活得久；  
没有 `remove`（要删就重建一张表，探测链里不留墓碑）。

### 14.3 str

```zx
std.str.eq(a, b);  std.str.startsWith(s, p);  std.str.contains(s, sub);
std.str.indexOf(s, sub);                       // ?usize
std.str.trim(s);  std.str.trimEnd(s);          // 返回原串视图
std.str.parseInt(s);                           // ?i64
std.str.concat(al, a, b);                      // ?[]u8
std.str.split(al, s, ",");                     // ?[][]const u8；用完 freeSplit
```

### 14.4 file / io / process

```zx
std.file.read(al, path);           // ?[]const u8，整个文件
std.file.write(path, data);        // bool
std.file.exists(path);  std.file.remove(path);

let h = std.io.openRead(path);     // ?usize（0 = 没打开，close(0) 安全）
std.io.read(h, buf);  std.io.write(h, data);   // 返回实际字节数
std.io.stdout();  std.io.stderr();  std.io.stdin();

std.process.args();                // [][]const u8，argv[0] 不在内
std.process.getEnv("HOME");        // ?[]const u8
std.process.exit(1);               // noreturn
```


### 14.5 BufReader / BufWriter

```zx
// 逐行读：一次读 4K，按行交付（行指向内部缓冲，下次 next 后失效）
if (std.BufReader.open(al, "big.txt", 4096)) r0 {
    var r = r0;
    defer r.deinit();
    while (r.next()) line { std.println("{s}", line); }
}

// 攒够再写
if (std.BufWriter.create(al, "out.txt", 4096)) w0 {
    var w = w0;
    defer w.deinit();              // deinit 自动 flush
    w.writeLine("hello");
}
```

---

## 15. 内建函数（@ 前缀）

不可重定义，用户标识符不能以 `@` 开头。

| 内建                                                   | 用途                |
| ---------------------------------------------------- | ----------------- |
| `@sizeOf(T)` / `@alignOf(T)`                         | 字节数 / 对齐          |
| `@typeOf(e)` / `@typeName(T)`                        | 取类型 / 类型名         |
| `@typeInfo(T)`                                       | 类型元数据（编译期值）       |
| `@hasField(T, "name")`                               | 字段或静态成员存在性        |
| `@fieldType(T, "name")`                              | 字段类型              |
| `@fieldName(T, i)`                                   | 第 i 个字段名          |
| `@field(v, "name")`                                  | 按编译期字符串取/写字段      |
| `@trap()`                                            | 中止（noreturn）      |
| `@compileError(msg)`                                 | 编译期报错             |
| `@memcpy(d, s, n)` / `@memmove` / `@memset(d, v, n)` | 批量内存原语（std 容器的底座） |

**cast 类内建没有**：指针互转、位重解释、窄化转换统一是 `(T) v`（§7.R4）。

### 15.1 元编程：@typeInfo + inline for

`@typeInfo(T)` 是纯编译期值（kind / name / bytes / align / fieldCount / fields），  
配 `inline for` 就能对任意结构体写字段无关的泛型代码：

```zx
const eql = fn (T: type, x: T, y: T) bool {
    const info = @typeInfo(T);
    if (info.kind != std.TypeKind.struct_) {
        return x == y;                        // 标量直接比
    }
    inline for (0..info.fieldCount) i {       // 编译期展开：每个字段一份
        const FT = @fieldType(T, @fieldName(T, i));
        if (!eql(FT, @field(x, @fieldName(T, i)), @field(y, @fieldName(T, i)))) {
            return false;
        }
    }
    return true;
};
```

这就是 Zx 的"反射"：全部发生在编译期，运行时零开销，生成的 C 和手写的一样。

---

## 16. 常见陷阱速查

新人最容易踩的坑，按出现频率排：

1. **`{x}` 是一元组不是块**。想用块算值：`tag: { …; tag: v; }`。
2. **没有 break**。循环跳出用标签 + 收割：`done: while (…) { done: v; }`，  
   标签写在 `while` / `for` **前面**。
3. **块没有"末尾表达式即返回值"**。函数用 `return`，`if` 想有值用表达式臂  
   `if (c) a else b` 或收割。
4. **`null` 没有自己的类型**。`let a = null;` 编译错；写出落点  
   `let a: ?i32 = null;`。
5. **错误码从 1 起**。`(error)0` 编译错——0 是"无错误"。
6. **defer 与返回值**。`defer free(buf); return buf;` 返回悬空指针——defer  
   在 return 值算出后、真正离开前执行。
7. **switch 语句位置所有臂要同型**。语句 switch 统一写块臂 `模式: { … };`。
8. **枚举穷尽**。switch 枚举漏变体又没 `else` 是编译错误（这是保护，不是麻烦）。
9. **HashMap 键生命周期**。键切片必须比表活得久；别把指向局部缓冲的键放进去。
10. **浮点必须有整数部分**。`.5` 非法（`t.0.0` 下标链要用 `.`+数字）。
11. **`fn name() {}` 不存在**。一律 `const name = fn () … { … };`。
12. **整数溢出会 trap**。要绕回自己写位运算，别依赖回绕。

---

## 17. 与 C / Zig 差异速查

| 特性      | Zx                               | C                | Zig                |
| ------- | -------------------------------- | ---------------- | ------------------ |
| 解引用     | `.0`，`.` 自动穿透                    | `*p` / `p->`     | `p.*`              |
| 数组      | `*[N]T`（8 字节指针，N 进类型）            | `T[N]` 值类型       | `[N]T` 值类型         |
| 数组字面量   | `&.{1, 2, 3}`                    | `(int[]){1,2,3}` | `&.{1,2,3}`        |
| 块的值     | 只来自收割 `tag: v;`                  | 语句               | 末尾表达式即值            |
| 提前跳出    | 收割（带值）                           | `break` / `goto` | `break :blk v`     |
| 空值捕获    | `if (x) v { }`                   | —                | `if (x) \|v\| { }` |
| `?T` 布局 | **载荷在前、标签在后**                    | 手写 struct        | 标签在前               |
| 结构体字面量  | `P{ .x = 1 }` / `.{ .x = 1 }`    | `(P){.x=1}`      | `.{ .x = 1 }`      |
| 可变性     | `const`(编译期) / `let`(只读) / `var` | `const`          | `const` / `var`    |
| 错误      | `!T` 错误值在返回值里                    | 错误码/errno        | error set + try    |
| 泛型      | `fn(T: type) type` 单态化           | 宏                | comptime 单态化       |
| 格式化     | 编译期展开（常量 fmt）                    | printf 运行时       | comptime           |

---

## 18. 下一步读什么

按顺序：

1. **`spec/examples/` 与 `tests/good/`**——语言特性的可运行目录，每个文件一个  
   主题，从 `hello` 类用例读到 `union-tag.zx`、`comptime.zx`。
2. **`std/std.zx` + `std/map.zx`**——ArrayList、HashMap 全是普通 Zx 代码，  
   是"泛型 + 方法 + defer + @memcpy"的综合教材。
3. **`tools/runner.zx`**——一个 340 行的真实程序：读文件、拼命令、起进程、  
   比对输出。想看"Zx 工程怎么组织"就看它。
4. **`spec/zx-spec.md`**——规则的精确定义（本文的每一节都对应它的章节）。
5. **`spec/decisions.md`**——每个设计为什么这么定，以及否掉了哪些方案。
6. **`spec/bootstrap.md` + `bootstrap/`**——编译器正在用 Zx 重写自己的现场，  
   `bootstrap/lex.zx` 是第一块：与 C 版词法器逐字节对拍的 Zx 实现。

命令与工程化（C 库接入、`vendor/`、离线构建、编辑器配置）见  
`docs/zxc-usage.md`；完整语言与命令行参考见 `docs/manual.md`；  
诊断里每个 `E0xxx` 的修法见 `docs/errors.md`。断网或想全文检索时，  
直接双击 `docs/site/index.html`：全部文档合成一个可离线搜索的单页。  

编译器本身的编译模型（每个模块一份 C、增量编译、交叉验证开关）见  
`spec/zx-spec.md` §16 与 `README.md` 的里程碑日志。
