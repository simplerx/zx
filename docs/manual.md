# Zx 手册

Zx 的完整查询文档。按「想查什么」组织，不按语法书顺序：前面是写代码天天要用的
东西，后面是查一次就够的细节（FFI、内建、命令行、错误码、坑）。

怎么用：

| 想查 | 去哪 |
| --- | --- |
| 某个写法对不对 | 目录里找章节，或 `Ctrl/Cmd+F` 搜关键字 |
| 编译报错什么意思 | `zxc explain E0304`，或第 14 章 |
| 标准库某个函数签名 | 第 12 章，或直接看 `std/*.zx`（就是普通 Zx 代码） |
| 这段话为什么这么设计 | `spec/decisions.md` |

记号约定：`T` 指任意类型，`N` 指编译期整数，`expr` 指任意表达式，
`…` 表示省略。示例都是按当前实现验证过的；能独立编译运行的会写成完整程序。

---

## 1. 起步

### 1.1 工具链

仓库自带 zig（`.toolchain/zig.exe`），Zx 编译器把它当 C 编译器用，不需要系统装
gcc/clang/msvc。

```sh
sh build.sh     # 产出三个可执行文件
sh test.sh      # 全量回归
```

| 产物 | 作用 |
| --- | --- |
| `bin/zxc.exe` | 编译器本体（检查 / 生成 C / 链接 / 工程命令） |
| `bin/zxfe.exe` | 自举前端，把 `.zx` 解析成 AST；`zxc` 默认走它 |
| `bin/zxls.exe` | 语言服务器（补全 / 跳转 / 提示 / 诊断），给编辑器用 |

一次编译是三段：**前端**（词法 + 语法，Zx 写的 `bootstrap/`）→ **语义 + 生成 C**
（C 写的 `src/`）→ **C 编译器**（zig cc）。想核对生成物：

```sh
zxc file.zx --emit-c      # 生成的 C 打到 stdout
zxc file.zx --ast         # 语法树（s-expression）
zxc file.zx --tokens      # 词法单元流
```

### 1.2 第一个程序

```zx
const std = module("std");

const main = fn () u8 {
    std.println("hello, {}", "zx");
    return 0;
};
```

```sh
zxc hello.zx -o hello.exe && ./hello.exe
```

三个固定套路：

- 入口是 `main`，返回退出码（`u8` / `i32` / `!i32` 都可以）。
- 用标准库要先 `const std = module("std");`——没有隐式全局命名空间。
- 函数是普通的常量：`const 名字 = fn (参数) 返回类型 { ... };`。

### 1.3 工程

单文件够用，多文件就切成工程：

```sh
zxc init myapp     # 生成 zx.json + src/main.zx + vendor/
zxc run            # 编译并运行
zxc build          # 只编译，产物在 out/
zxc test           # 跑 src/test/ 里的用例
```

`zx.json`：

```json
{
  "name": "myapp",
  "version": "0.1.0",
  "main": "src/main.zx",
  "out": "myapp",
  "deps": []
}
```

`deps` 是工程内依赖的模块名数组（对应 `src/<名>.zx` 或 `vendor/<名>.zx`）。
`zxc build` 按模块产出 `.o` 放进 `.zxc-cache/`，只重编改动过的模块。

---

## 2. 词法

### 2.1 源文件

- 编码 UTF-8。开头有 BOM 会被跳过，换行 `\n` / `\r\n` 都行。
- 注释两种：`// 到行尾`，`/* 可跨行，不能嵌套 */`。
- 语句以 `;` 结尾，块用 `{ }`，不需要分号分隔的只有 `}` 收尾的块语句本身。

### 2.2 标识符与关键字

标识符：字母 / `_` 开头，后接字母、数字、`_`。大小写敏感。允许非 ASCII 字母
（中文变量名可以用，但不推荐）。

关键字只有 26 个：

```
const  let  var  fn  module  type  struct  union  enum  error
if  else  while  for  inline  switch  continue  return
defer  errdefer  try  orelse  pub  extern  null  undefined
```

注意**没有**：`break`、`and`/`or`/`not`、`true`/`false`（`true`/`false` 是字面量
不是关键字）、`async`、`await`、`pub fn`（函数一律写成 `const f = fn …`）。

### 2.3 字面量

| 种类 | 写法 | 说明 |
| --- | --- | --- |
| 十进制 | `123` `1_000_000` | 下划线任意位置分隔，只为好读 |
| 十六进制 | `0xFF` `0xffffffff` | 大小写都行 |
| 二进制 | `0b1010` | |
| 八进制 | `0o755` | |
| 浮点 | `1.5` `1e9` `2.5e-3` | 无后缀，默认 `f64`，落点是 `f32` 时按 `f32` |
| 布尔 | `true` `false` | |
| 空 | `null` | 只对 `?T` 有定义 |
| 未初始化 | `undefined` | 只要空间不要值；读它是未定义行为 |
| 字符 | `'a'` `'\n'` `'中'` | 落点是整数类型时按该类型定型，否则 `u8` |
| 字符串 | `"hi"` | 类型是 `*[N]const u8`（长度在类型里，见 §3.2） |

转义序列：

| 写法 | 含义 |
| --- | --- |
| `\n` `\r` `\t` `\0` | 换行、回车、制表、NUL |
| `\\` `\"` `\'` | 反斜杠、双引号、单引号 |
| `\xHH` | 一个字节（必须两位十六进制） |
| `\u{1F600}` | Unicode 码点，编码成 UTF-8（1–4 字节） |

```zx
std.println("{}", "\u{4e2d}\u{6587}");   // 中文
```

### 2.4 运算符

从高到低（同层左结合，`=` 系列右结合）：

| 优先级 | 运算符 |
| --- | --- |
| 1 | `x.y` `x[i]` `x[a..b]` `f(...)` `x.?`（后缀） |
| 2 | `&x` `*p` `-x` `+x` `!b` `~i`（前缀） |
| 3 | `*` `/` `%` |
| 4 | `+` `-` |
| 5 | `<<` `>>` |
| 6 | `&` |
| 7 | `^` |
| 8 | `\|` |
| 9 | `==` `!=` `<` `<=` `>` `>=` |
| 10 | `&&` `\|\|`（无短路以外的特殊语义，见下） |
| 11 | `..`（区间，只用于 `for` 和切片） |
| 12 | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` |

规则：

- **两边类型必须完全一致**，没有隐式整型提升。`i32 + u64` 直接报错，
  必须写 `(i64)a + (i64)b` 之类。
- `&&` / `||` 是短路求值，操作数必须是 `bool`。
- 比较运算产出 `bool`；比较两边类型必须一致（指针比指针、整数比整数）。
- `!T`、非扁平 `?T`（即载荷本身带标签的）不能直接 `== null`——用 `if` 捕获或
  `switch` 拆（§5.2 / §5.4）。
- 没有运算符重载，没有隐式字符串拼接，`+` 只能加数字。

---

## 3. 类型

### 3.1 标量

| 类别 | 类型 |
| --- | --- |
| 有符号整数 | `i8` `i16` `i32` `i64` `i128` |
| 无符号整数 | `u8` `u16` `u32` `u64` `u128` |
| 指针宽度整数 | `usize`（无符号）/ `isize`（有符号） |
| 浮点 | `f32` `f64` |
| 布尔 | `bool` |
| 空 | `void` |
| 永返 | `noreturn` |
| 错误 | `error`（运行时就是 `u16`；`error.null` = 65535 保留） |

整型运算溢出按二进制补回绕（不 panic）；除零在运行期是未定义行为，常量除零
在编译期直接报 E0305。整型字面量本身是「宽度未定」的，落点定类型；落不下
就报 E0302。

### 3.2 指针、数组视图、切片

Zx 里**没有"数组"这种值类型**：`*[N]T` 只是一个指针（8 字节），
"N 个 T 连续存放"这件事只写在类型里，内存得自己弄。

| 类型 | 含义 | 大小 |
| --- | --- | --- |
| `*[N]T` | 指向 `N` 个连续 `T` 的指针（长度在类型里） | 8 |
| `*T` | 指向一个 `T` 的指针 | 8 |
| `[]T` | 切片：`{ptr, len}` | 16 |
| `[]const T` | 只读切片 | 16 |

```zx
var xs: *[4]i32 = &.{ 1, 2, 3, 4 };   // 字面量：自动分配一段匿名存储，给出指针
std.println("{}", @sizeOf(*[4]i32));   // 8：它就是指针
std.println("{}", xs.len);             // 4：长度是类型的一部分（编译期常量）
std.println("{}", xs[2]);              // 3

var ys = std.stackAlloc(i32, 8);       // 栈上分配 8 个 i32，拿到 *[8]i32
ys[0] = 7;
ys[1] = 8;                             // 直接写
var sl: []i32 = ys[1..3];              // 切片：看得到、改得动
let ro: []const i32 = xs[0..];         // 只读视图
std.println("{} {}", sl[0], ro[0]);    // 8 和 1（slice 是 {ptr, len} 16 字节）

var px = &xs;                          // 取的是指针 xs 的地址 → **i32
px[0][2] = 9;                          // 通过二级指针改 xs 指向的存储
std.println("{}", xs[2]);              // 9
```

要点：

- **`&xs` 取的是指针变量的地址，不是"数组的地址"。** Zx 里没有数组变量，
  `xs` 这个名字指的就是那个 8 字节的指针槽，所以 `&xs` 得到 `**T`
  （指向指针的指针），和 `&p`（p 为 `*T`）完全一回事。
  这是有意为之：C 里 `&arr` 与 `arr` 的衰减规则是著名的混淆源，
  Zx 干脆删掉"数组变量"这个概念，只有指针，没有第二种解释。
- **拿内存**：栈用 `std.stackAlloc(T, N)`，堆用 `std.heapAlloc(alloc, T, N)`；
  `&.{ … }` 是"顺手给一段匿名存储"的糖。三者结果类型都是 `*[N]T`。
- `.len` 是类型里的常量，不是字段：`*[N]T` 没有运行时长度，长度要变就用 `[]T`。
- 字符串字面量的类型是 `*[N]const u8`，因此 `s.len`、`s[i]` 直接可用，
  需要切片时 `s[0..]`。
- `?*T` 是扁平可选（`null` 就是 0 指针，省一个标签字节），`?i32` 是带标签的。
- 切片越界是未定义行为，运行期不检查（ReleaseSafe 的编译器也不插检查，
  只检查编译器自身的逻辑）。

### 3.3 可选 `?T`

```zx
var o: ?i32 = 5;
if (o) v { std.println("有值 {}", v); } else { std.println("空"); }
std.println("{}", o orelse -1);        // 空就取右边
let n = o.?;                            // 空则沿 !T 传播（函数必须返回 !T）
```

`?T` 的内存布局：`{ payload: T, tag: bool }`；指针载荷另有扁平化处理（见上）。

### 3.4 错误联合 `!T` 与错误集

```zx
const IOErr = error{ open, read };      // 错误集：编译期类型，运行时是 u16

const readInt = fn (s: []const u8) IOErr!i32 {
    if (s.len == 0) { return IOErr.open; }   // 返回错误
    return 1;
};
```

- `!T` 的值要么是 `T`，要么是一个错误码；布局 `{ payload: T, tag: error }`。
- 错误集可以合并：`error{IOErr, extra}` 是 `open, read, extra` 三个码。
- 错误码名不需要预先声明，`return error.boom;` 就行；声明错误集只是把可能性
  写进类型。
- 错误集之间按「子集」赋值：返回 `error{a}!i32` 的函数可以当 `error{a,b}!i32` 用。
- 函数能不能失败写在返回类型里：`fn () !void`。**裸 `error` 或裸 `error{a,b}`
  不能当返回类型**（E0308）——它没说清失败时有没有值。

四种出口，都在 §5：

```zx
let v = try f();        // 失败就往上抛（调用方必须返回 !T）
if (f()) v { } else e { }   // 分别处理
let d = f() orelse -1;      // 失败取默认值
switch (f()) { .ok: v { }; .err: e { }; }   // 分支处理
```

### 3.5 元组

`{T1, T2, …}` 是**值类型**，内联存储，用 `.0` `.1` 取值。

```zx
let t = .{ 1, "two", 3.0 };   // 匿名元组，类型推断成 {i32, *[3]const u8, f64}
std.println("{}", t.1);
```

带类型的元组写作 `{i32, f64}`；命名访问请用结构体。联合体的成员也是元组
（§3.6）。

### 3.6 结构体、联合体、枚举

**结构体**：字段 + 方法 + 静态成员。`Self` 指当前类型。

```zx
const Point = struct {
    x: f64,
    y: f64,

    const origin = fn () Self { return Point{ .x = 0.0, .y = 0.0 }; };  // 静态
    const len2 = fn (self: *Self) f64 { return self.x * self.x + self.y * self.y; };
    const scale = fn (self: *Self, k: f64) void { self.x = self.x * k; };
};
```

- 字段用 `,` 分隔，方法/常量用 `;` 结尾。
- 方法就是「第一个参数是接收者的普通函数」；`p.len2()` 自动补 `&p`。
- 静态成员（`const` / `var`）不放实例里，走 `Point.origin()`。
- 字段可以带默认值：`count: i32 = 0`，字面量里省略就用默认值。
- 结构体字面量必须写全所有无默认值的字段，重复给同一字段报 E0307。

**联合体**（带标签）：成员写成 `名: {标签值, 载荷类型}`，读的时候先看 `.tag`。

```zx
const Tag = enum { one, two };
const U = union {
    one: {Tag.one, i32},
    two: {Tag.two, f64},
};

const show = fn (u: U) []const u8 {
    return switch (u.tag) {
        .one: "one";
        .two: "two";
    };
};

let u = U{ .two = 2.5 };   // 字面量只写一个成员，标签自动填
```

`.tag` 的类型就是那个枚举，所以 `switch (u.tag)` 能用 `.one` 简写，并且编译器
检查穷尽性（E0504）。

标签枚举要自己写太啰嗦时，用 **`union(enum)`**——标签类型自动生成，成员直接写载荷：

```zx
const V = union(enum) {
    i: i64,
    f: f64,
};

const show = fn (v: V) []const u8 {
    return switch (v.tag) {
        .i: "int";
        .f: "float";
    };
};

let a = V{ .i = 7 };
let b = V{ .f = 1.5 };
std.println("{} {}", a.i, b.f);     // 读之前先看 tag
```

裸 `union { f: f64 }`（不写 `(enum)`、成员也不是 `{标签, 载荷}`）只是共享存储，
没有标签，读错成员是未定义行为。

**枚举**：本质是整数，底层类型默认 `i32`（可写 `enum(u8) { … }`）。

```zx
const Color = enum { red, green, blue };
let c = Color.green;
std.println("{} {}", c, (i32)c);       // green 1
```

### 3.7 函数类型

`fn (T1, T2) R` 是值类型，8 字节（代码指针），可以存进结构体字段、当参数传。

```zx
const op = fn (a: i32, b: i32) i32 { return a + b; };
const apply = fn (f: fn (i32, i32) i32, a: i32, b: i32) i32 { return f(a, b); };
std.println("{}", apply(op, 2, 3));    // 5
```

没有闭包：函数值不带环境。要带状态就把状态显式传进去（或用结构体方法）。

### 3.8 类型值、泛型

类型本身是编译期的值，类型是 `type`。函数可以返回类型，这就是泛型的全部：

```zx
const Box = fn (T: type) type {
    return struct {
        v: T,
        const get = fn (self: *Self) T { return self.v; };
    };
};

var b = Box(i32){ .v = 9 };
std.println("{}", b.get());
```

- 泛型是**编译期单态化**：每个实参组合生成一份代码，没有运行期类型信息。
- 泛型实参必须是编译期常量（类型、整数、字符串字面量）。
- 没有 `anytype`（不支持隐式泛型参数）；需要「什么都能接」时写多个重载名的
  普通函数，或者用 `[]const u8` 之类统一到具体类型。

### 3.9 布局与大小

```zx
@sizeOf(T)        // 字节数
@alignOf(T)       // 对齐
@typeName(T)      // 类型名（字符串）
```

结构体按字段声明顺序布局，遵循 C 的对齐规则（有填充，不重排）。要和 C 对接
的结构体直接按 C 的字段顺序写，`extern struct` 见 §11。

---

## 4. 声明与作用域

### 4.1 `const` / `let` / `var` / `pub`

| 关键字 | 语义 |
| --- | --- |
| `const` | 编译期常量，或文件级不可变绑定 |
| `let` | 局部不可变绑定（运行期求值一次） |
| `var` | 局部可变绑定 |
| `pub` | 导出给别的模块（默认私有） |

```zx
const N = 16;              // 编译期常量，可当数组长度
let name = "zx";           // 不可变
var count: i32 = 0;        // 可变
count += 1;
```

- 改 `let` 绑定报 E0403；改 `const` 报 E0403。
- 所有值必须被使用：未用的局部变量报 E0410（不想用就命名 `_x`），
  丢弃返回值报 E0411（要丢就写 `_ = f();`）。
- 作用域就是词法块，内层可以遮蔽外层；没有提升（hoisting）。

### 4.2 模块

```zx
const std = module("std");      // 标准库
const util = module("util");    // 工程内 src/util.zx 或 vendor/util.zx
const win = module("user32.zx");// vendor/ 下的 C 绑定（带扩展名也对）
```

- 模块名 = 文件名（去掉 `.zx`），按顺序在 `src/`、`vendor/`、`deps` 里找；
  标准库在 `std/`。
- 导入是**显式绑定**：不写 `const x = module(...)` 就没有任何名字可用，
  也没有「同名即命名空间」的隐式规则。
- 跨模块访问需要对方 `pub`，否则报 E0404。
- 可以再导出：`pub const str = module("str.zx");`，使用者就能 `std.str.eq(...)`。
- 模块里可以写 `test` 约定（§12.13）：`zxc test` 会跑 `pub const xxx = fn () void`。

---

## 5. 语句与表达式

Zx 里**块和分支都是表达式**：`if` / `switch` / 带标签的循环都能产出值。

### 5.1 赋值

```zx
var a: i32 = 1;
a = 2;
a += 3;      // += -= *= /= %= &= |= ^= <<= >>= 都有
```

赋值本身不产出值（不能 `let b = (a = 1);`）。

### 5.2 `if` 与捕获

```zx
if (cond) { ... } else if (other) { ... } else { ... }

// 作为表达式
let m = if (a > b) a else b;

// 捕获可选值 / 错误联合 / 指针
if (maybe) v { ... } else { ... }          // ?T：有值拿 v
if (maybe) v { ... } else e { ... }        // !T：成功拿 v，失败拿错误码 e
```

- 捕获变量只在对应分支里可见，类型就是载荷类型。
- `?T` 的 `else` 分支拿不到值；`!T` 的 `else e` 里 `e` 是错误码。
- 这是**唯一**的解包写法，没有 `|v|` 那种竖线语法。

### 5.3 循环

```zx
while (i < n) { i += 1; }

for (0..n) i { ... }            // i: usize，从 0 到 n-1
for (xs[0..]) v { ... }         // 遍历元素
for (xs[0..], 0..) v, i { ... } // 元素 + 下标
inline for (0..4) i { ... }     // 编译期展开（i 是编译期常量）
```

- **没有 `break`**。提前退出有两种写法：
  1. 条件里带标志位；
  2. 标签收割——给循环起个名字，用 `名字: 值;` 从里面「交出」一个值：

```zx
var i: i32 = 0;
let found = done: while (i < 100) {
    i += 1;
    if (i == 17) { done: i * 2; }    // 立刻结束循环，整个表达式取值 34
};
std.println("{}", found);
```

- `continue` 有，直接进下一轮。
- 标签也能标在普通块上：`let v = blk: { if (x) { blk: 1; } 2 };`

### 5.4 `switch`

```zx
return switch (c) {
    'a', 'b', 'c': "low";
    '0'...'9': "digit";        // 区间模式
    else: "other";
};
```

- 臂的写法是 `模式, 模式: 表达式;`，每条臂以 `;` 结尾。
- 模式可以是整数字面量、字符、`.枚举成员`、区间、`true`/`false`、`else`。
- 分支体是块时也要写 `;`：`.none: { return 2; };`
- **穷尽性**：枚举 / 布尔 / `?T` / `!T` 的 switch 必须覆盖全部或写 `else`，
  否则报 E0504。整数 switch 建议写 `else`。
- 捕获：臂名后面跟变量，块里可用：

```zx
switch (a.next) {                    // a.next: ?*Node
    .some: p { std.println("{}", p.val); };
    .none: { std.println("end"); };
}

switch (f()) {                       // f() 返回 !i32
    .ok: v { std.println("ok {}", v); };
    .err: e { std.println("err {}", std.debug.errName(e)); };
}
```

- 联合体按 `.tag` 分派（§3.6），标签类型是枚举时同样检查穷尽。
- 同一个模式写两次报 E0504。

### 5.5 `defer` / `errdefer`

```zx
const work = fn () !void {
    let h = open() orelse return error.open_failed;
    defer close(h);              // 离开作用域时执行（正常返回、return、错误返回都执行）
    errdefer std.println("失败回滚");   // 只在「带错误返回」时执行
    try doIt(h);
};
```

- 顺序是后进先出（LIFO）。
- `defer` 里不能 `return`（E0405）。
- `errdefer` 只能出现在返回 `!T` 的函数里（E0405）。

### 5.6 `try` / `orelse` / `.?`

| 写法 | 语义 | 前提 |
| --- | --- | --- |
| `try e` | `e` 是错误就把它原样往上抛 | 所在函数返回 `!T` |
| `e orelse d` | `e` 空/错就取 `d` | `e` 是 `?T` 或 `!T` |
| `e.?` | 空/错就往上抛 | 所在函数返回 `!T` |

### 5.7 `return`

```zx
return;          // void
return 0;        // 值
return error.x;  // 错误
```

- 有返回值的函数每条路径都必须 return（E0502）。
- `noreturn` 函数（如 `std.process.exit`）之后不许再有代码（E0502）。

---

## 6. 函数与方法

### 6.1 定义

```zx
const add = fn (a: i32, b: i32) i32 { return a + b; };
const greet = fn (name: []const u8) void { std.println("hi {}", name); };
const fail = fn () !void { return error.nope; };
```

- 参数必须写类型；返回类型必须写（`void` 也要写）。
- 没有重载、没有默认参数、没有可变参数（`std.print` 这类是编译器特判的内建）。
- 函数是值，可以赋给常量、存进结构体、当参数传（§3.7）。
- 递归没问题，深度受栈限制。

### 6.2 方法与 `Self`

```zx
const Counter = struct {
    n: i32,
    const init = fn () Self { return Counter{ .n = 0 }; };
    const bump = fn (self: *Self) i32 { self.n += 1; return self.n; };
    const value = fn (self: *Self) i32 { return self.n; };   // 只读也可用 *Self
};

var c = Counter.init();
c.bump();
std.println("{}", c.value());
```

- `c.bump()` 等价于 `Counter.bump(&c)`；接收者要能取地址（临时值不行）。
- 也可以在类型上调用：`Counter.bump(&c)`。
- 方法体里的 `Self` 就是所在结构体/联合体/枚举的类型。

### 6.3 泛型函数

```zx
const max = fn (T: type, a: T, b: T) T { return if (a > b) a else b; };
std.println("{}", max(i32, 3, 7));
```

类型参数写在参数表最前面，调用时显式传（`max(i32, …)`），编译器为每组实参
单态化一份。

### 6.4 `noreturn`

永不返回的函数（`panic`、`exit`）返回类型写 `noreturn`，调用点之后的代码
被当作不可达——写了就报 E0502。

---

## 7. 内建 `@`

内建是编译器直接处理的表达式，不能重定义、不能当值传。**全部只有九个**：

| 内建 | 类型 | 说明 |
| --- | --- | --- |
| `@sizeOf(T)` | `usize` | 字节数（含填充） |
| `@alignOf(T)` | `usize` | 对齐 |
| `@typeName(T)` | `[]const u8` | 类型名（编译期定死） |
| `@memcpy(d, s, n)` | `void` | 批量复制（不重叠） |
| `@memmove(d, s, n)` | `void` | 批量复制（可重叠） |
| `@memset(d, byte, n)` | `void` | 批量填充 |
| `@link("ws2_32")` | `void` | 链接一个系统库 / 静态库（FFI 用） |
| `@compileError("…")` | `noreturn` | 编译期报错并停下（泛型拒绝非法类型） |
| `@trap()` | `noreturn` | 编译期可达就报错；否则运行期 trap |

**类型转换只有一种写法：`(T)val`。** 没有 `@intCast` / `@ptrCast` / `@bitCast` /
`@as` 这些变体，`castable` 规则统一在 §3.6 说明：

```zx
var a: i32 = 300;
let b: u8  = (u8)a;         // 截断 → 44
let c: i64 = (i64)a;        // 扩宽 → 300
let d: f64 = (f64)a;        // 整数 → 浮点 → 300.0
let p: *i32 = &a;
let q = (u64)p;             // 指针 → 整数
let r = (*i32)q;            // 整数 → 指针
std.println("{} {} {} {}", b, c, d, r[0]);   // 44 300 300 300
```

**反射只有三个**：`@typeName` / `@sizeOf` / `@alignOf`。够拿名字、大小、对齐，
足够写序列化、FFI 布局校验、分配器；不再提供字段遍历（没有 `@typeInfo` /
`@fieldName` / `@fieldType` / `@field` / `@hasField` / `@offsetOf`）。

字段偏移要自己算：拿真实地址相减，和 `offsetof` 等价，而且零依赖。

```zx
const Buf = extern struct { data: *[16]u8, n: u32 };

const check = fn () bool {
    var b: Buf = undefined;
    return ((usize)&b.n) - ((usize)&b) == 16;   // 字段偏移
};
```

**源码位置**（`@file` / `@line` / `@col` / `@fnName`）也删了：位置信息只由
`std.debug.panic` / `std.debug.assert` 在调用点由编译器注入，语言层不再暴露。

被删掉的老名字会报 E0211，并直接告诉你换成什么写法：

```
error[E0211]: @intCast was removed: type conversion is written `(T)value`
```

---

## 8. 打印与格式化

```zx
std.print("无换行 {}", 1);
std.println("带换行 {}", 2);
let s = std.format(std.heap.page, "{} {}", 1, 2);   // ?[]u8，第一个实参是分配器
```

占位符：

| 写法 | 用于 |
| --- | --- |
| `{}` | 自动：整数按十进制、浮点、字符串、`bool`、枚举按变体名 |
| `{s}` | 切片 / 字符串 |
| `{c}` | 整数当字符打印 |
| `{d}` | 整数十进制或浮点 |
| `{x}` | 十六进制 |
| `{b}` | 二进制 |
| `{0}` `{1}` | 按下标取实参（可以和顺序占位混用） |
| `{}` 加选项 | `{x:>8}` 右对齐宽 8；`{d:0>5}` 补零；`{:^10}` 居中；`{:.2}` 两位小数 |

```zx
std.println("[{:>6}] [{:0>5}] [{:.2}]", 42, 42, 3.14159);
// [    42] [00042] [3.14]
```

- 格式串是编译期常量时，占位符个数、下标、动词类型当场检查（E0802）；
  格式串是变量则留到运行时，规则一样。
- `{{` / `}}` 打花括号。
- 想写到别的句柄：`std.io.print(h, "…", …)` / `std.io.println(h, …)`。

---

## 9. 内存

没有 GC、没有隐式分配。标准库统一用一个 `Allocator` 值：

```zx
const Allocator = struct {
    ctx: ?*u8,
    alloc: fn (?*u8, usize) ?*u8,
    free_: fn (?*u8, *u8, usize) void,
};
```

用法：

```zx
let a = std.heap.page;                    // 直接要系统内存
let p = a.alloc(a.ctx, 100);              // ?*u8
if (p) buf {
    var b = (*[100]u8)buf;
    b[0] = 1;
    a.free(a.ctx, buf, 100);
}
```

| 分配器 | 从哪里拿 | 怎么还 |
| --- | --- | --- |
| `std.heap.page` | 系统（malloc） | 必须逐块 `free` |
| `std.arena.ArenaAllocator.init(backing)` | 从 backing 一次要一大块 | `deinit` 一次全还，或 `reset` 复用 |
| `std.pool.Pool.init(gpa)` | 定长块池 | `free` 归还到池；重复释放会被拦下 |

```zx
var ar = std.arena.ArenaAllocator.init(std.heap.page);
if (ar) a {
    let s = a.alloc(a.ctx, 64);          // 一路 bump，不单独释放
    if (s) p { (p + 0).0 = 1; }
    std.arena.ArenaAllocator.deinit(a);  // 一次全还
}
```

规则：

- 分配返回 `?*u8`（失败是 `null`），必须判空。
- 指针算术和转发用 `p + i`；取值 `(p + i).0`；转成类型化指针 `(*[N]T)p`。
- **不能返回指向栈上的指针/切片**（E0503）——要活过函数就分配。
- `undefined` 只表示"要空间不要值"，读它是未定义行为。

---

## 10. 并发

唯一的并发抽象是 `std.task.Task` 实例：

```zx
const work = fn (i: usize) !usize { return i * 2; };

const runAll = fn () !void {
    var t = std.task.Task.pool(4);          // 4 个线程的任务池
    for (0..8) i { _ = t.spawn(work, i); }  // 句柄可以不要，awaitAll 会等全部
    try t.awaitAll();
    t.close();
};
```

| 构造 | 线程模型 |
| --- | --- |
| `Task.coro()` | 当前线程上轮流跑（无抢占） |
| `Task.single()` | 一个后台线程 |
| `Task.pool(n)` | n 个线程的池 |

| 方法 | 作用 |
| --- | --- |
| `t.spawn(f, ctx) ?usize` | 提交一个 `fn (usize) !usize`，返回句柄 |
| `t.await(h) !usize` | 等一个任务 |
| `t.awaitAll() !void` | 等全部，任一失败就返回错误 |
| `t.cancel(h)` | 取消 |
| `t.send(v) !void` / `t.recv() !usize` | 实例自带的通道 |
| `t.trySend` / `t.tryRecv` | 非阻塞版 |
| `t.yield()` | 让出 |
| `t.close()` | 关池、回收 |

任务之间只通过通道和 `std.atomic` 共享状态，没有全局锁：

```zx
std.atomic.atomicAdd(&counter, 1);     // 返回旧值
std.atomic.atomicLoad(&counter);
std.atomic.atomicStore(&counter, 0);
std.atomic.atomicSwap(&counter, 9);
std.atomic.atomicCAS(&counter, want, new);
```

---

## 11. 调 C

### 11.1 声明外部函数和结构体

```zx
@link("kernel32");

extern "C" {
    pub fn GetAsyncKeyState(vk: i32) i16;
    pub fn Sleep(ms: u32) void;

    pub const Point = struct { x: i32, y: i32 };   // 按 C 布局写
}
```

- 句柄一律用 `*u8`（C 的 `HANDLE`/`HWND` 都是指针宽度）。
- `#[repr]` 不需要：`struct` 就是 C 布局（字段顺序 + 自然对齐）。
- 函数指针字段直接写 `fn (…) R`，Zx 函数可以填进去。
- `@link("user32")` 挂系统库；`@link("vendor/libfoo.a")` 挂静态库。
- `extern fn f(…) R;` 是单条声明的简写，不必包块。

### 11.2 用 `zxc include` 生成绑定

```sh
zxc include ../third_party/raylib    # 递归扫目录 → vendor/raylib.zx + vendor/raylib.clib
zxc include ./vendor/stb_image.h     # 单个头文件
zxc run                              # 自动编 vendor/*.c 成静态库并链接
```

生成的是普通 Zx 源码，可以手改；已有同名文件不会被覆盖（`--force` 才重写）。

### 11.3 从网上下载库

```sh
zxc add https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
zxc fetch          # 换机器时按 zx.lock 复现（只认已锁定的源）
```

`zxc.lock` 记录下载内容的 SHA-256、git commit、文件树哈希；`zxc run`/`build`
永远离线，缺库只提示不下载。

### 11.4 约定

- Zx 字符串是 `[]const u8`（指针 + 长度），**不是** NUL 结尾。传给 C 之前
  自己补 `\0`，或者用接受长度的 C API。
- 从 C 拿回来的 `char *` 按定长数组视图读，扫到 NUL 再切：

```zx
const cstr = fn (p: *const u8) []const u8 {
    let buf = (*const [256]u8)p;
    var n: usize = 0;
    while (n < 255) { if (buf[n] == 0) { return buf[0..n]; } n += 1; }
    return buf[0..255];
};
```

---

## 12. 标准库

标准库是普通 Zx 代码（`std/*.zx`），可以直接读、直接改。

### 12.1 `std.str` 字符串

| 函数 | 说明 |
| --- | --- |
| `eq(a, b) bool` | 逐字节比较 |
| `startsWith(s, p)` / `endsWith(s, p)` | 前后缀 |
| `contains(hay, needle)` / `indexOf(hay, needle) ?usize` / `indexOfFrom(hay, needle, from)` | 查找 |
| `trim(s)` / `trimStart(s)` / `trimEnd(s)` | 去空白（返回子切片，不复制） |
| `parseInt(s) ?i64` | 十进制解析，失败 `null` |
| `concat(alloc, a, b) ?[]u8` | 拼接（新分配） |
| `split(alloc, s, sep) ?[][]const u8` | 切分；`freeSplit(alloc, parts)` 归还 |

### 12.2 `std.ArrayList(T)` 可增长数组

```zx
var list = std.ArrayList(i32).init(std.heap.page);
defer list.deinit();
list.push(1);
list.append(&.{ 2, 3 });        // 一次追加一片
std.println("{}", list.items()); // []i32 视图
list.deinit();
```

`at(i) ?*T`、`get(i) ?T`、`set(i, v)`、`first()`、`last()`、`pop()`、`insertAt(i, v)`、
`removeAt(i)`、`clear()`、`reserve(n)`、`resize(n, fill)`、`reverse()`、`swap(i, j)`。

### 12.3 `std.map.HashMap(V)`

```zx
var m = std.map.HashMap(i32).init(std.heap.page);
defer m.deinit();
m.put("a", 1);
if (m.get("a")) v { std.println("{}", v); }
```

`get(key) ?V`、`getPtr(key) ?*V`、`contains(key)`、`putNew(key, v) ?V`、
`count()`、`capacity()`、`empty()`、`clear()`、`keyAt(i)`、`valueAt(i)`。
键是字符串切片，**不复制**——键的生命周期要长于表。

### 12.4 `std.fs` 文件系统

| 函数 | 说明 |
| --- | --- |
| `readAll(alloc, path) ?[]const u8` | 整块读 |
| `writeAll(path, data) bool` / `appendAll(path, data) bool` | 整块写 / 追加 |
| `exists(alloc, path) bool` | 是否存在 |
| `stat(alloc, path) ?Stat` | 元数据（`size`、`isDir`…） |
| `createDir` / `removeFile` / `removeDir` | 目录与文件操作 |
| `readDir(alloc, path) ?[][]const u8` | 一次列出目录项 |
| `DirIter` | 流式遍历（`open`/`next`/`close`） |
| `join(alloc, a, b) ?[]u8` | 拼路径 |
| `basename` / `dirname` / `ext` / `stem` / `isAbs` | 纯切片操作，零分配 |

### 12.5 `std.io` 带句柄读写

| 函数 | 说明 |
| --- | --- |
| `openRead(path) ?usize` / `openWrite` / `openAppend` | 开文件 |
| `read(h, buf) usize` / `write(h, data) usize` | 分次读写 |
| `seek(h, off) bool` / `tell(h) usize` / `close(h)` / `flush(h)` | 定位与收尾 |
| `stdin()` / `stdout()` / `stderr()` | 标准句柄 |

`std.BufReader`（`{ h, buf, cap }`，`next() ?[]const u8` 按行取、`fill()`、`chunk()`）
和 `std.BufWriter`（`write`、`writeByte`、`writeLine`、`flush`）是缓冲层。

另外 `std.file` 是更简单的整块版：`read(alloc, path)` / `write(path, data)` /
`append` / `exists` / `remove`。

### 12.6 `std.json`

```zx
var ar = std.arena.ArenaAllocator.init(std.heap.page);
if (ar) a {
    if (std.json.parse(a, "{\"a\":[1,2]}")) v {      // !Value → 捕获取值
        if (std.json.member(v, "a")) arr {
            std.println("{}", std.json.stringify(a, arr));
        }
    } else e {
        std.println("bad json: {}", std.debug.errName(e));
    }
    std.arena.ArenaAllocator.deinit(a);
}
```

`Value` 是 `union(enum)`：`nul` / `boolean` / `int` / `float` / `str` / `arr` / `obj`。
辅助：`member(v, key) ?Value`、`item(v, i) ?Value`、`text(v) ?[]const u8`、
`parse(alloc, src) !Value`、`stringify(alloc, v) ?[]u8`。解析零拷贝（字符串指向源）。

### 12.7 `std.time`

| 函数 | 说明 |
| --- | --- |
| `counter() u64` / `freq() u64` | 单调高精度计数器与频率 |
| `nsBetween(a, b) u64` / `nsSince(a) u64` | 纳秒差 |
| `unixMs()` / `unixSecs()` / `unixNs()` | 墙钟 |
| `fromUnixMs(ms) DateTime` | 拆年月日时分秒 |
| `formatUtc(alloc, ms) ?[]u8` | ISO 8601 |

### 12.8 `std.arena` / `std.pool`

见 §9。`ArenaAllocator` 是 `init/alloc/free/reset/deinit`（`alloc` 等挂在返回的
`Allocator` 上）；`Pool` 是 `init/alloc/free/used/deinit`，定长块、重复释放会被拦。

### 12.9 `std.task` / `std.atomic`

见 §10。

### 12.10 其它

| 名字 | 说明 |
| --- | --- |
| `std.print` / `std.println` / `std.format` | 输出与格式化（§8） |
| `std.io.print` / `std.io.println` | 往指定句柄输出 |
| `std.process.args() [][]const u8` | 命令行参数（**不含** argv[0]） |
| `std.process.getEnv(name) ?[]const u8` / `setEnv` | 环境变量 |
| `std.process.exit(code) noreturn` | 退出 |
| `std.debug.panic(msg, args)` | 带位置信息的崩溃 |
| `std.debug.assert(cond)` | 断言（失败即 panic） |
| `std.debug.errName(err) []const u8` | 错误码 → 名字 |
| `std.heap.page` | 现成的分配器 |
| `std.Allocator` | 分配器类型本身（§9） |

### 12.11 测试约定

`src/test/` 下的零参 `pub` 函数就是用例（`zxc test` 会找）：

```zx
const std = module("std");

pub const add_works = fn () void {
    std.debug.assert(1 + 1 == 2);
};
```

---

## 13. 命令行

```
zxc <file.zx> [选项]        单文件：检查（默认）/ 编译 / 打印
zxc <子命令> [参数]         工程级操作（读当前目录的 zx.json）
```

单文件选项：

| 选项 | 作用 |
| --- | --- |
| `--check` | 只检查（默认行为，显式写法） |
| `-o <path>` | 生成可执行文件 |
| `--emit-c` | 生成的 C 打到 stdout |
| `--ast` | 语法树 |
| `--tokens` | 词法单元 |
| `--release` | `-O2`（默认 dev 是 `-O0`） |
| `--fe=zx\|c` | 选前端（`c` 只用于交叉验证） |

子命令：

| 命令 | 作用 |
| --- | --- |
| `init [name]` | 建工程 |
| `build [--release]` | 按 zx.json 构建到 `out/` |
| `run [-- args]` | 构建后运行 |
| `test [filter]` | 跑用例 |
| `check [file…]` | 静态检查（默认 cwd，回退 src/、src/test/） |
| `fmt [file…] [--check]` | 格式化（`--check` 只报告） |
| `errors` | 列出全部错误码 |
| `explain <CODE>` | 解释一个错误码 |
| `include <名\|头文件\|目录>` | 接入本地 C 库 |
| `add <url>` / `fetch [名]` | 下载 / 复现 C 库 |
| `sync` | 扫 vendor/ 补生成绑定 |
| `cbuild [group]` | 只重编 vendor/*.c |

环境变量：

| 变量 | 作用 |
| --- | --- |
| `ZX_CC` | 指定 C 编译器（默认 `.toolchain/zig.exe cc`） |
| `ZX_VERBOSE=1` | 打印 cc 命令行与后端原始输出 |
| `ZX_CHECKED=1` | `test.sh` 用它换 Debug + 运行时检查的编译器跑同一套回归 |

---

## 14. 诊断

格式固定四段：

```
src/main.zx:12:9: error[E0304]: `Point` has no field 'coutn'
 12 |     let a = p.coutn;
    |               ^
  help: did you mean `count`?
```

- `路径:行:列` 的列按字节算，渲染时多字节字符按两格对齐。
- 码共 36 个，分六类（`zxc errors` 分组列出）：`E01xx` 词法、`E02xx` 语法、
  `E03xx` 类型、`E04xx` 名字与可见性、`E05xx` 编译期与语言规则、`E06xx`/`E07xx`/`E08xx`
  模块 / 泛型 / 格式串。
- `zxc explain E0304` 给出含义、常见修法和文档链接；`docs/errors.md` 是完整版。
- 一行里超长的代码会截断成窗口（160 列），保证 `^` 不会被冲出屏幕。
- `B1001` 是**编译器自己的 bug**：Zx 层检查通过、生成的 C 被 C 编译器拒收。
  看到它请带上 `ZX_VERBOSE=1` 的原始输出报 issue。

---

## 15. 编辑器

`bin/zxls.exe` 是一个标准 stdio LSP 服务端，能力：

| 能力 | 说明 |
| --- | --- |
| 补全 | 成员、模块、枚举变体、结构体字段、关键字、代码片段（`if`/`fn`/`for` 等） |
| 悬停 | 类型、签名、声明位置的原始写法 |
| 行内类型提示 | 未写类型的 `let` / `var` 右边显示灰色 `: T`；调用处显示实参名 |
| 跳转定义 / 查找引用 | 跨文件、含 `vendor/` 与 `zx.json` 的 deps |
| 诊断 | 与 `zxc --check` 同一条链，报同样的码 |
| 格式化 | 与 `zxc fmt` 同一实现 |
| 文档符号 / 签名帮助 | |

Zed 里的配置（`.zed/settings.json`）：

```json
{
  "lsp": {
    "zx-lsp": {
      "binary": { "path": "E:/workbuddy/zx/bin/zxls.exe" },
      "settings": {}
    }
  },
  "languages": { "Zx": { "language_servers": ["zx-lsp"] } }
}
```

改完 `tools/zxls.zig` 要重编 `bin/zxls.exe` 并重启编辑器（capabilities 只在
初始化时协商一次）。

---

## 16. 容易被绊到的语义

这些不是 bug，是刻意的取舍——先看一眼能省很多时间。

**没有 `break`。** 提前退出循环用标签收割：`const v = lbl: while (…) { … lbl: 值; };`
（`continue` 有）。

**没有"数组"这种值类型。** `*[N]T` 是指向 N 个连续 T 的**指针**（8 字节），
`*T` 是普通指针，`[]T` 是 `{ptr, len}`。要存储就 `std.stackAlloc`（栈）或
`std.heapAlloc`（堆），或者用 `&.{ … }` 糖。相应地，`&xs`（xs 为 `*[N]T`）
取的是**指针变量的地址**（得 `**T`），不存在"数组的地址"——这就是删掉
数组变量的目的：C 里 `&arr` 与指针衰减那套歧义在 Zx 里无从发生。

**字符串是 `*[N]const u8`。** 长度在类型里，`s.len`、`s[i]` 都能用；需要可变
或运行时长度时切 `s[0..]`。Zx 字符串不带 NUL 结尾。

**没有隐式数值转换。** `i32 + u64` 是错误，`let x: i64 = i32值` 也是错误，
都要写 `(i64)v`（转换只有这一种写法；`@intCast` 那类内建在 1.2.0 已删）。

**整型溢出回绕，除零未定义。** 常量除零在编译期就报 E0305；运行时除以 0
是未定义行为，自己先判。

**`?T` / `!T` 不能互相比。** 非扁平 `?T` 连 `== null` 都不行；用 `if` 捕获、
`switch` 或 `orelse`。`?*T` 是扁平的（等于 0 指针），行为和指针一致。

**`if` 是表达式，但只有"裸表达式臂"和"带标签的块臂"能出值。**
`let m = if (a > b) a else b;` 可以；`if (a > b) { a } else { b }` 拿不到值——
块的值要靠标签交出：`if (c) { v: 上一行结果; } else { v: 0; }`。

**条件是编译期常量时只检查会走的那一支。** 这是为了让
`if (@sizeOf(T) > 8) @compileError("太大了");` 能写。反过来说，死分支里的
类型错误不会报——别指望编译器替你检查 `if (DEBUG)` 的另一支。

**块表达式和元组字面量长得像。** `.{ 1, 2 }` 是元组，`&.{ 1, 2 }` 是
"匿名存储 + 指针"，`{ … }` 在值位置是元组、在语句位置是块。

**`let` 绑定不可变，`var` 才是变量。** 想改就 `var`；改 `let` 报 E0403。

**所有值都要被用到。** 未用的局部变量报 E0410（命名 `_x` 关掉），丢弃返回值
报 E0411（写 `_ = f();`）。

**没有 `anytype` / 闭包 / 运算符重载 / 宏 / 异常。** 泛型只有 `type` 参数这一条
路；需要"什么都能接"就写多个函数或统一到具体类型。

---

## 17. 已知边界

写在这里的都是当前实现的事实，不是承诺：

- **平台**：Windows 是主平台（FFI 演示、语言服务器、标准库的时间/文件都按它写）。
  其它平台未验证。
- **switch 没有区间模式**：`'0'...'9'` 目前不解析（`spec/decisions.md` 里记着
  这条意向，但前端还没实现）；要区间判断就写 `if`。
- **块臂必须用标签交出值**：`if (c) { s } else { "y" }` 这种"块里最后一句就是值"
  的写法没有实现，见 §16。
- **嵌套与链长有上限**：括号嵌套 256 层、同层运算符链 2048 个——超过会明确
  报 E0202，而不是让前端爆栈。
- **运行期没有边界检查**：切片越界、`undefined` 读取都是未定义行为。
- **调试信息**：生成 C 的符号名带 `_行_列` 后缀，用 C 调试器看栈帧时按它对照源码。
