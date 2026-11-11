# Zx 语言查阅文档（Reference）

> 版本 1.2.3 · 一份文档看完全部定义和用法。  
> 语言细则与设计理由在 `spec/zx-spec.md`；错误码逐条解释在 `docs/errors.md`；  
> 本文是**查阅用**的完整速查：语法、类型、内建、模块、std 全 API、工具链。

---

## 0. 十分钟速览



```zx
const std = module("std");                    // 模块必须显式绑定（§12）

const Point = struct {                        // 类型声明就是 const 绑定
    x: i32,
    y: i32,
    const ORIGIN = Point{ .x = 0, .y = 0 };   // 静态成员
    const len2 = fn (self: *const Point) i32 {
        return self.x * self.x + self.y * self.y;
    };
};

const scale = fn (T: type, v: T, k: T) T {    // 泛型：编译期函数
    return v * k;
};

pub const main = fn () u8 {
    var p = Point{ .x = 3, .y = 4 };
    std.println("{}", p.len2());              // 方法：首参是 self
    std.println("{}", @type(Point).size);     // 反射：唯一内建 @type
    let xs = std.ArrayList(i32).init(std.heap.page);
    defer xs.deinit();
    _ = xs.push(1);
    if (xs.first()) v { std.println("first {}", v); }
    return 0;
};
```

函数是一等值，写成 `const 名字 = fn (参数) 返回 { … };`。类型同理是值（`type`）。

---

## 1. 词法

- 注释 `//` 到行尾；**没有块注释**。
- 标识符：ASCII 字母/数字/`_`，不能以 `@` 开头（`@` 留给内建）。
- `true` / `false` 是布尔字面量，不是关键字。
- 字符串 `"…"`：`[]const u8`，转义 `\\ \" \n \r \t \0 \xHH`；**字符串里不能有裸换行**。
- 字符字面量 `'A'`；数字字面量支持 `0x` / `0b` / `0o` 前缀与 `_` 分位。
- 未定长整数：字面量先按 `comptime_int` / `comptime_float` 携带，落类型时检查拟合。

### 1.1 关键字（25 个）

```
const  let  var  fn  module  type  null  undefined
if  else  while  for  inline  switch  continue  return
defer  errdefer  struct  union  enum  error  pub  extern  orelse  try
```

**没有 `break`**：离开带标签的作用域靠收割语句 `tag: expr;`（§8）。

### 1.2 运算符与标点

```
&  *  /  %  +  -  <<  >>  <  <=  >  >=  ==  !=  ~  !  |  ^  &&  ||
=  +=  -=  *=  /=  %=  &=  |=  ^=  <<=  >>=
.  .{  ..  ,  :  ;  (  )  [  ]  {  }  @
```

二义由位置消解：`&` 前缀取地址 / 中缀位与；`*` 前缀是解引用 `.0` 的时代已过——  
现在指针解引用统一 `.0`，`*` 只在类型位置和乘法出现；`!` 前缀逻辑非 / 类型位置错误联合体。

---

## 2. 声明与变量

```zx
const A = 10;         // 编译期绑定：必须能算出来（R1），不可变
let  b: i32 = f();    // 运行时不可变绑定
var  c: i32 = 0;      // 可变绑定
pub  const D = …;     // pub：对其它模块可见（只用于文件作用域）
extern var g: i32;    // 外部 C 全局变量
```

- **未使用的 `let`/`var`/形参报 E0410**；显式丢弃只有一种写法：`_`。  
  `_` 不是名字（不进作用域），可重复出现：`let _ = f();`、`mix(_, _, used)`。  
  `_` 前缀（`_foo`）**不豁免**。
- 解构：`const {a, b} = tup;` / `let .{a, b} = t;`，槽位可写 `_` 丢弃。
- 变量必须先赋值后使用；初始化器指向当前栈帧时编译器做逃逸检查（E0503）。
- **可见性（2026-11-11 起）**：`pub` 对文件作用域声明与**容器体成员**是同一个语义——  
  容器体里的字段、方法（`const f = fn…`）、静态成员默认**私有**，只在声明它的文件内可见；  
  带 `pub` 才对其它文件开放（编译器报 E0404，补全也不显示）。枚举/`union(enum)` 的  
  变体是值不是成员，恒公开，不要求逐个写 `pub`。函数定义只有 `const F = fn(…) ret {…};`  
  一种写法，没有 `pub fn` 语法——`pub` 是前缀修饰符，不是另一种声明。

---

## 3. 类型总表（64 位下的大小）

| 写法                                | 名称    | 大小                   | 说明                               |
| --------------------------------- | ----- | -------------------- | -------------------------------- |
| `u8 u16 u32 u64 u128` / `usize`   | 无符号整数 | 1/2/4/8/16 / 8       |                                  |
| `i8 i16 i32 i64 i128` / `isize`   | 有符号整数 | 1/2/4/8/16 / 8       |                                  |
| `f16 f32 f64`                     | 浮点    | 2/4/8                | IEEE-754                         |
| `bool`                            | 布尔    | 1                    | `true` / `false`                 |
| `void`                            | 空     | 0                    | 等于 `{}`                          |
| `type`                            | 类型值   | 0                    | 仅编译期                             |
| `{T1, T2, …}`                     | 元组    | 字段之和（含填充）            | 值类型                              |
| `*[N]T`                           | 数组    | **8**                | 纯指针；N 进类型，`*[3]i32` ≠ `*[4]i32`  |
| `*T`                              | 裸指针   | 8                    | 数量未知，与 `*[N]T` 不同型               |
| `*const T`                        | 只读指针  | 8                    | 透过它写入是编译错误                       |
| `[]T` / `[]const T`               | 切片    | 16                   | `{ptr, len}`                     |
| `struct{…}`                       | 结构体   | 按声明顺序                |                                  |
| `enum{…}` / `enum(T){…}`          | 枚举    | `sizeof(T)`，默认 `u32` | 就是整数                             |
| `union{…}`                        | 联合体   | 最大成员（带标签再 + 标签）      |                                  |
| `?T`                              | 可选值   | `1 + pad + sizeof T` | §7                               |
| `!T` / `E!T`                      | 错误联合体 | `2 + pad + sizeof T` | §7                               |
| `fn(P…) R`                        | 函数    | 8                    | 值语义                              |
| `comptime_int` / `comptime_float` | 字面量类型 | —                    | 不可写出                             |
| `noreturn`                        | 不返回   | 0                    | `@compileError` / `@trap` / 死循环臂 |

**Zx 没有"数组值"**：`*[N]T` 就是 8 字节指针，长度只写在类型里，用于编译期界检查与  
`.len`。存储来自 `std.stackAlloc(T, n)`（栈）、`std.heapAlloc(al, T, n)`（堆）、  
`&.{ … }`（匿名存储糖）。`&xs`（xs 是 `*[N]T` 变量）是**取指针变量的地址**，得 `**T`。

---

## 4. 指针 / 数组 / 切片

```zx
var x: i32 = 1;
let p = &x;                  // *i32（取地址）
std.println("{}", p.0);      // 解引用是 `.0`
p.0 = 2;                     // 透过可变指针写入
```

- `.` 自动穿透一层指针：`p.name` ≡ `p.0.name`。
- 切片语法：`a[lo..hi]`（`hi` 省略到结尾）；下标 `a[i]` 运行期界检查由调用方保证语义、  
  `*[N]T` 的 `.N` 是**编译期**下标。
- `.len` / `.size` 是编译器字段：`[]T` 的 `.len`、`*[N]T` 的 `.len` 与 `.size`  
  （`size` = N × 元素大小）都是编译期常量。聚合体自己有 `len`/`size` 字段时字段优先。
- 指针算术 `p + n` 按元素偏移，要求 `usize` 或字面量。
- 指针 ↔ 整数只有显式转换：`(usize)p` / `(*u8)addr`。

---

## 5. 聚合类型

### 5.1 元组 `{…}`

```zx
const t = {1, 2.5, "x"};
const {a, b} = t;            // 解构
t.0                          // 编译期下标访问（.N）
t[0]                         // 同 .N：N 必须是编译期值（字面量 / const / inline for 变量）
var w = {1, 2};
w[1] = 6;                    // ct 索引也能当赋值目标，与 .N 同一链路
```

- **`tup[N]` 的 N 是编译期值**：元组是异构的，第 i 个和第 i+1 个元素类型
  不同，运行时下标给不出单一返回类型（E0501）。遍历配 `inline for`
  （循环变量就是编译期绑定）。越界在编译期报。
- 元组没有连续存储，不做运行时索引。

### 5.2 结构体

```zx
const P = struct { x: i32 = 0, y: i32 };    // 字段可给默认值
const p = P{ .x = 1, .y = 2 };              // 指名初始化（编译期检查完整性）
const q = P{ .x = 1 };                      // 有默认值的字段可以不写
```

- 匿名结构体 `.{ .x = 1 }`：靠期望类型定型。
- 静态成员：体里的 `const`（常量、函数、嵌套类型）；访问走 `P.name`。
- `packed struct` / `extern struct`：面向 FFI 的布局保证。

### 5.3 枚举：就是整数

```zx
const Color = enum(u8) { red, green = 4, blue };
Color.red                          // 值可指定，未指定顺延
switch (c) { .red: …; .green: …; .blue: …; }    // 臂必须完整覆盖
```

### 5.4 联合体（Zig 风格）

```zx
const Shape = union(enum) {
    circle: f64,
    rect: { w: f64, h: f64 },
};
const s = Shape{ .circle = 2.0 };
switch (s.tag) {
    .circle: use(s.circle);
    .rect:  use(s.rect.w);
}
```

读错 tag 的载荷是未定义行为，分派一律走 `switch (s.tag)`。  
带 `u8`/`void` 载荷的成员按普通字段处理；`s.tag` 是枚举。

### 5.5 方法

首参是 `self` 的成员函数就是方法，`p.method(args)` 等价 `P.method(&p, args)`：

```zx
const Counter = struct {
    n: i32 = 0,
    const bump = fn (self: *Counter) i32 { self.n = self.n + 1; return self.n; };
};
var c = Counter{};
_ = c.bump();
```

---

## 6. 控制流

```zx
if (cond) { … } else if (c2) { … } else { … }     // 语句形式
let v = if (cond) 1 else 2;                        // 表达式形式（糖）
let v = if (cond) blk: { … blk: 2; } else 3;       // 多语句臂用收割交值

while (cond) { … }

for (xs) x { … }                                   // 遍历切片
for (xs, 0..) x, i { … }                           // 带下标
for (0..n) i { … }                                 // 整数范围（不含 n）
inline for (0..info.fieldCount) i { … }            // 编译期展开（i 是编译期常量）

// 循环没有 else 分支（E0405）：收尾逻辑写在循环后面。

switch (v) {
    1, 2: …;
    3:  …;
    else: …;                                       // 整数要 else 兜底
}
```

- **收割**：`label: expr;` 把 `expr` 交给最近一层名为 `label` 的作用域当值，并**立即离开**该作用域；
  `label: { … }` 给块起标签，`continue label` 跳过本轮。块不收割时值为 `{}`（void）。
- **收割是循环的正常出口**：恒真循环里收割自己的标签（`n: {}` 空收割 / `n: v;` 带值）会跳出循环，
  循环后面的语句可达——这是 `while (true)` 加收尾代码的唯一写法（1.2.3 起不再误报 E0502）：

  ```zx
  var hits: i32 = 0;
  var total: i32 = 0;
  n: while (true) {
      hits = hits + 1;
      if (hits == 3) { n:{}; }          // 空收割跳出
  }
  var got = m: while (true) {
      total = total + 1;
      if (total == 5) { m: total; }     // 带值收割：循环的值就是它
  };
  ```
- **defer**：离开当前作用域时倒序执行；**errdefer** 只在以错误返回时执行。
- `switch` 臂是 `模式: 体`，同一 subject 的臂必须构成完整划分（E0504）。

---

## 7. 空值与错误

```zx
var s: ?i32 = null;                 // ?T：空值联合体，null 是字面量
if (s) v { use(v); } else { … }     // 捕获绑定（无竖线）
let u = s orelse 0;                 // 空则取右值（右边必须是表达式）
let w = s.?;                        // 断言非空，空则 trap
// 没有 `orelse return`——提前返回要写 if/else：
if (s) v { use(v); } else { return error.noent; }

const f = fn (path: []const u8) !i32 {    // !T：错误联合体（anyerror）
    if (std.io.openRead(path)) h {        // 可选值 → 错误要手工转
        …
        const n = try read(h);            // try：出错就把错误码原样返回
    }
    return error.noent;
};

const E = error{no, io};             // 命名错误集
const g = fn () E!void { … };
std.debug.errName(e)                 // 错误码 → "no"
```

- 错误码是 `u16`；`error.name` 字面量可直接当值；`error.null`（65535）保留。
- 函数声明的错误集检查返回值与 `try` 传播（E0901–E0903）。
- `(error)enumVar`：把枚举当错误码用（FFI 惯用）。

---

## 8. 函数、泛型与编译期

```zx
const add = fn (a: i32, b: i32) i32 { return a + b; };   // 函数是值
const apply = fn (f: fn (i32) i32, x: i32) i32 { return f(x); };
pub const main = fn () u8 { … }          // pub 控制跨文件导出（见 §2 可见性）
```

- 返回 `!T` 的函数每条路径都要 return 或到达 `noreturn`（E0308）。
- **尾逗号**：参数表、调用实参、error 集、元组类型/字面量、struct 字面量
  都允许以 `,` 结尾（`fn (a: i32, b: i32,)`、`add(1, 2,)`）。`zxc fmt` 的
  约定：**一行里的 `(...)` 或字面量组（`.{ … }`、`T{ … }`）以 `,` 结尾就
  拆成一行一个，并逐层递归**——内层组不带尾逗号则保持单行（见 §12 格式化）。
- **泛型**：形参写 `T: type` 的函数是编译期函数，`ArrayList(i32)` 是一次编译期调用。  
  实例名取自构造式（`var xs = std.ArrayList(i32).init(…)` 推断为该实例）。
- **编译期执行**：const 初始化器、泛型实参、`inline for` 都在编译期算；  
  除零、越界在编译期直接报错（E0505/E0506）。
- `@compileError("msg")`：真的执行到才炸（放进编译期 `if` 做类型分派守卫）。
- 没有反射遍历之外的 trait/约束机制：需要约束时用编译期 `if` + `@type(T)` 自检。

---

## 9. 转换规则（R4：唯一写法 `(T)v`）

```zx
let x: u16 = (u16)n;            // 窄化/扩宽、整数↔浮点、指针互转、指针↔整数
let f = (f64)i;
```

没有隐式数值转换；字面量（comptime_int/float）例外，会向期望类型定型并检查拟合。  
castable 表覆盖全部合法转换；不合法的转换编译期报 E0301/E0306。

---

## 10. 内建（`@` 前缀，1.2.2 起共 7 个）

| 内建                      | 签名                                     | 用途              |
| ----------------------- | -------------------------------------- | --------------- |
| `@type(x)`              | `fn(type\|expr) Type`                  | 编译期描述符，见下       |
| `@memcpy(dst, src, n)`  | `(ptr\|slice, ptr\|slice, usize) void` | 批量复制（不得重叠）      |
| `@memmove(dst, src, n)` | 同上                                     | 批量复制（可重叠）       |
| `@memset(dst, byte, n)` | `(ptr\|slice, int, usize) void`        | 批量填充            |
| `@link("lib")`          | `fn(str) void`                         | 链接外部库（FFI 唯一入口） |
| `@compileError(msg)`    | `fn([]const u8) noreturn`              | 编译期报错           |
| `@trap()`               | `fn() noreturn`                        | 运行期断点/中止        |

### 10.1 `@type(x)` —— 唯一的反射

x 是**类型**（`Point`、`i32`、`[]const u8`、泛型 T）或**表达式**（取它的类型），  
返回编译期描述符（只许 `const` 绑定，开不了 `var`）：

| 字段                                | 类型           | 说明                   |
| --------------------------------- | ------------ | -------------------- |
| `.name`                           | `[]const u8` | 类型名                  |
| `.size`                           | `usize`      | 字节大小                 |
| `.align`                          | `usize`      | 对齐                   |
| `.ty`                             | `type`       | 类型本身（表达式实参时就是"它的类型"） |
| `.fieldCount`                     | `usize`      | 数据字段数                |
| `.fields[i].name / .ty / .offset` | —            | 逐字段：名字 / 类型 / 偏移     |

两个描述符类型名是编译器内建的，可直接拿来标注：`const info: Type = @type(T);`、  
`const f: Field = info.fields[0];`（`var` / `let` 仍然不行——它们没有运行时表示）。  
读字段要在编译期展开里做，`i` 才是常量：

```zx
const describe = fn (T: type) void {
    const info: Type = @type(T);
    inline for (0..info.fieldCount) i {
        const f: Field = info.fields[i];
        std.println("{}: {} @{}", f.name, @type(f.ty).name, f.offset);
    }
};
```

`@sizeOf` / `@alignOf` / `@typeName` / `@typeInfo` / `@typeOf` 都是它的历史拼写，  
现报 E0211 并提示替代写法。字段偏移也可现场算：`((usize)&v.f) - ((usize)&v)`。

```zx
const ti = @type(Point);
if (ti.size != 8) { return 1; }
inline for (0..ti.fieldCount) i {
    const f: Field = ti.fields[i];      // i 是编译期常量，这里才折得动
    std.println("{}: {}", f.name, @type(f.ty).name);
}
```

---

## 11. 格式化与打印

`std.print` / `std.println` / `std.format` / `std.io.print(ln)` 共用一套格式化：

- `{}` 顺取下一个实参，按类型的默认格式。
- `{N}` 绝对下标：`N` 从 0 数实参（`{0}` 就是第一个实参）；可带选项 `{0:>6}`。
- verb（只能顺序用，不能写成 `{0:x}`）：`{d}` 十进制、`{x}` 十六进制、  
  `{b}` 二进制、`{s}` 字符串、`{c}` 字符、`{any}` 聚合展开。
- 选项在冒号后：填充符 + 对齐 + 宽度 + `.精度`——  
  `{:>8}` 右对齐宽 8（数字默认右）、`{:<6}` 左对齐（字符串默认左）、  
  `{:0>2}` 零填充、`{:*^7}` 自定义填充居中、`{:.3}` 浮点精度（`%f` 语义）。
- **顺取与 `{N}` 绝对下标互不感知**：`std.println("{2} {}", a, b, c)` 里  
  `{2}` 是 `c`，但后面的 `{}` 仍从 `a` 顺取。混用容易数错，不建议。
- 占位符与实参个数不匹配报 E0802。

```zx
std.println("[{x}] [{b}] [{0:>6}] [{1:.2}]", 255, 3.5, 42);
// [ff] [11111111] [    42] —— {N} 与选项可组合，verb 不行
```

---

## 12. 模块系统

```zx
const std = module("std");          // 显式绑定；std 解析到编译器旁边的 std/
const gl = module("glfw3");         // 具名模块：glfw3.zx 或 glfw3/glfw3.zx
const v  = module("ext");           // vendor/ 里的模块直接写名字，不带路径

pub const task = module("task.zx"); // 再导出：别的模块经 std.task 访问
```


- **没有隐式跨文件名字**：别的 .zx 里的标识符必须先 `module(…)` 绑定（E0401）。
- 解析顺序：源文件所在目录的相对路径 → 向上每一层的 `vendor/`（近→远）→
  zx.json 的 `deps`。**vendor/ 里的模块直接写名字**（`module("ext")`），
  不在 vendor/ 里的才写相对路径（`module("lmod.zx")`，相对源文件所在目录）。
- 未 `pub` 的声明是私有的（E0404）。
- 循环导入报 E0601（带原因，不刷误导成员错误）。
- 编译器随附 `std/`；`std.print/heap/file/io/…` 这些**编译器内建成员**见 §13。

---

## 13. 编译器提供的 std 内建面

这些名字由编译器 seed 进 `std`（不是 Zx 代码，LSP 里 hover 有签名）：

| 名字 | 形态 | 说明 |
|---|---|---|
| `std.print(fmt, args)` / `std.println` / `std.format(al, fmt, args)` | 函数 | 打印 / 格式化成 `[]u8` |
| `std.heap.page` | 值（Allocator） | 现成分配器：malloc/free |
| `std.Allocator` | 类型 | `{ ctx: ?*u8, alloc: fn, free: fn }` 接口 |
| `std.stackAlloc(T, n)` | 函数 | 栈上分配 → `*[n]T` |
| `std.heapAlloc(al, T, n)` | 函数 | 堆分配 → `*[n]T`，OOM abort |
| `std.file.read(al, path)` | 函数 | 整块读，`?[]const u8`（NUL 结尾） |
| `std.file.write / append(path, data)` | 函数 | 整块写/追加，`bool` |
| `std.file.exists(path)` / `remove(path)` | 函数 | 元信息，`bool`（不带分配器） |
| `std.io.openRead / openWrite / openAppend(path)` | 函数 | `?usize` 句柄 |
| `std.io.read(h, buf) usize` / `write(h, data) usize` | 函数 | 分次读写 |
| `std.io.seek / tell / close / flush` | 函数 | 句柄操作 |
| `std.io.stdin / stdout / stderr` | 函数 | 标准流句柄 |
| `std.process.args() [][]const u8`（不含 argv[0]） | | |
| `std.process.getEnv / setEnv / exit` | | |
| `std.debug.panic(fmt, args)` / `assert(cond, msg)` | | 位置由编译器注入 |
| `std.debug.errName(e)` / `printTrace()` | | |
| `std.atomic.atomicAdd / atomicSub / atomicLoad / atomicStore / atomicSwap / atomicCAS` | 函数 | `*u64` 上的原子操作 |

---

## 14. std 库 API 全表

全部是普通 Zx 代码（`std/` 目录）。约定：**要分配的函数分配器放第一参**；
不分配的函数返回指向原串的切片；结果带 NUL 的可直接当 C 串传 FFI。

### 14.1 std.ArrayList(T) —— 可增长数组（std.zx）

| 成员 | 签名 | 说明 |
|---|---|---|
| `init` | `(al) Self` | |
| `deinit` | `(self) void` | 释放缓冲 |
| `at` | `(self, i) ?*T` | 越界 null |
| `get` / `set` | `(self, i) ?T` / `(self, i, v) bool` | 值读写 |
| `items` | `(self) []T` | 整段切片视图 |
| `clear` | `(self) void` | len=0，容量不动 |
| `reserve` | `(self, need) bool` | 倍增扩容（@memcpy 搬家） |
| `push` / `pop` | `(self, v) bool` / `(self) ?T` | |
| `append` | `(self, vs: []const T) bool` | 一次 memcpy |
| `insertAt` / `removeAt` | `(self, i, v) bool` / `(self, i) bool` | memmove 挪动 |
| `first` / `last` | `(self) ?T` | |
| `reverse` / `swap` / `resize` | | resize 变长用 fill 补 |

字段：`buf: *T, len, cap, alloc`。

### 14.2 std.BufReader / std.BufWriter —— 缓冲流（std.zx）

```zx
var r = std.BufReader.open(al, "a.txt", 4096) orelse return error.io;
defer r.deinit();
while (r.next()) line { … }          // 行指向内部缓冲，next() 后失效
```

- BufReader：`open(al, path, cap) ?Self` / `init(al, h, cap)` / `deinit` / `close` /
  `next() ?[]const u8`（去 `\r\n`）/ `chunk() ?[]const u8`（二进制）/ `buffered() usize`。
  行比缓冲长会自动翻倍，不会读一半断掉。
- BufWriter：`create(al, path, cap) ?Self`（清空建）/ `appendTo`（追加建）/
  `write(data) bool` / `writeByte` / `writeLine` / `flush() bool` / `pending() usize` /
  `deinit`（自动 flush + 关句柄）。大块直写不绕缓冲。

### 14.3 std.str —— 字符串工具

| 函数 | 签名 | 说明 |
|---|---|---|
| `eq / startsWith / endsWith` | `(a, b) bool` | 零分配 |
| `indexOf / indexOfFrom` | `(hay, needle[, from]) ?usize` | |
| `contains` | `(hay, needle) bool` | |
| `trim / trimStart / trimEnd` | `(s) []const u8` | 切片指向原串 |
| `parseInt` | `(s) ?i64` | 十进制带符号，溢出 null |
| `concat` | `(al, a, b) ?[]u8` | 结果 NUL 结尾 |
| `split` | `(al, s, sep) ?[][]const u8` | 段指向原串；sep 空则整串一段 |
| `freeSplit` | `(al, parts) void` | 释放 split 的数组 |

### 14.4 std.map —— 哈希表（键 `[]const u8`，值任意）

```zx
var m = std.map.HashMap(i32).init(al);
defer m.deinit();
_ = m.put("a", 1);
std.println("{}", m.get("a") orelse 0);
var i: usize = 0;
while (i < m.capacity()) {          // 没有 while 续写语法，递增写在体内
    if (m.keyAt(i)) k { … }
    i = i + 1;
}
```

- `HashMap(V)`：开放寻址 + 线性探测，容量 2 的幂，负载 3/4 翻倍；哈希 FNV-1a
  （`std.map.fnv1a(s) u64`）。键不复制——键要比表活得久。
- 成员（全部）：`init(al) Self` / `deinit(self)` / `count() usize` /
  `capacity() usize` / `get(key) ?V` / `put(key, val) bool`（覆盖旧值）/
  `keyAt(i) ?[]const u8` / `valueAt(i) ?V`。字段私有。
- **没有 remove / contains / clear**：要删就重建一张表（探测链无墓碑，
  少一类 bug）。含不存在等价于 `get(key) != null`。

### 14.5 std.pool.Pool —— 尺寸分级内存池

```zx
var p = std.pool.Pool.init(al) orelse return 1;   // ?Pool
defer p.deinit();
let q = p.alloc(64);                 // ?*u8（19 档按尺寸归档）
if (q) b { if (!p.free(b)) …; }      // false = 重复释放/非法块，被拦下
_ = p.used(64);                      // 诊断：该档已用字节
```

19 档（16–128 每 16B 一档；256B–256KiB 每档 ×2），更大绕过池直连分配器。
槽位从不搬动；`used(size)` 供诊断。

### 14.6 std.fs —— 文件系统

- 纯路径（零分配，切片指向原串）：`isAbs / basename / dirname / ext / stem`；
  `join(al, a, b) ?[]u8`（NUL 结尾）。
- 元数据：`stat(al, path) ?Stat{size, mtime, isDir, isHidden}` / `exists(al, path) bool`。
- 变更：`createDir(al, path)`（单层）/ `removeFile` / `removeDir`（空目录），都返回 `bool`。
- 遍历：`DirIter.open(al, path) ?Self`（流式零拷贝）。**没有 break**，
  用 while 捕获收尾：

```zx
if (std.fs.DirIter.open(al, path)) dd {
    var d = dd;                     // next/isDir 要可变接收者
    defer d.close();
    while (d.next()) n {            // ?[]const u8，指向内部缓冲，next 后失效
        std.println("{} {}", d.isDir(), n);   // isDir() bool / size() u64
    }
}
```

  `readDir(al, path) ?[][]const u8`（收集版，名字带 NUL，空目录返回长度 0 切片）。
- 批量整块：`readAll(al, path) ?[]const u8` / `writeAll(path, data) bool` /
  `appendAll(path, data) bool`。
- 平台：Windows 先行（kernel32）；POSIX 等条件编译后再补。

### 14.7 std.json —— JSON

**没有 `catch` 语法**。`parse` 返回 `!Value`，用 `if (x) v { … }` 捕获成功值、
`else` 收错误，或 `.?` 断言 / `try` 传播：

```zx
if (std.json.parse(al, text)) v {
    if (std.json.member(v, "name")) m {          // 取成员（?Value）
        if (std.json.text(m)) s { … }            // text 只对 str 成员有值
    }
} else { /* errLine / errCol 已记录位置 */ }
const f = parsed.?.float;                        // float 成员直接读
```

- `Value = union(enum) { nul: u8, boolean: bool, int: i64, float: f64,
  str: []const u8, arr: *Arr, obj: *Obj }`——`nul` 是 `u8` 占位（恒 0），
  因为 **void 载荷的 union 成员构造不出来**。
- `Arr.at(i)` / `Obj.get(key)` / `Obj.put` / `Obj.count`；
  辅助：`member(v, key) ?Value` / `item(v, i) ?Value` / `text(v) ?[]const u8`。
- `ParseErr = { syntax, depth, oom, trailing }`；`errLine` / `errCol`（1 起，成功归零）。
- 零拷贝解析（字符串指向原文本）；arena 友好。

### 14.8 std.arena.ArenaAllocator —— 一次性批量分配

```zx
var ar = std.arena.ArenaAllocator.init(std.heap.page) orelse return 1;
defer ar.deinit();                       // 一次全释放（free 本身是 no-op）
if (std.json.parse(ar, doc)) v { … }     // 全程 arena
```

`init(backing) ?Allocator` / `reset(a)`（保留块、bump 归零）/ `deinit(a)`。
alloc 是 O(1) bump、16 对齐；free 是 no-op（arena 语义）。**不是线程安全的分配器，
别跨 Task 共享**。

### 14.9 std.time —— 时间

- 单调钟（测耗时用这个）：`counter() u64`（原始刻度）/ `freq()` / `nsBetween(a, b)` /
  `nsSince(a)`（u128 中转换算）。
- 墙钟：`unixMs()` / `unixSecs()` / `unixNs()` / `wall100ns()`（自 1601 的 FILETIME）。
- 日历：`DateTime{year, month, day, hour, min, sec, ms}`、`fromUnixMs(ms) DateTime`
  （Hinnant 算法）、`formatUtc(al, ms) ?[]u8`（ISO 8601，固定 24 字节）。

### 14.10 std.task —— 唯一的并发抽象

```zx
const job = fn (ctx: usize) !usize { … };    // 任务函数形状固定

var t = std.task.Task.single();       // 或 coro() / pool(n)
defer t.close();
if (t.spawn(job, ctx)) h {            // spawn 返回 ?usize（句柄）
    if (t.await(h)) r { … }           // await 返回 !usize，捕获或 try
}                                     //（try 要求所在函数返回 !T）
try t.awaitAll();
try t.send(v);  const x = try t.recv();     // 实例自带 rendezvous 通道
```

- `Task.coro()`：协程实例（不建线程，await 时推进）；`single()`：1 条专属线程；
  `pool(n)`：n 条工作线程（n=0 取 CPU-1）。
- 实例方法：`spawn / await / awaitAll / cancel / yield / send / recv / trySend /
  tryRecv / fromId / close`。
- `std.task.Task.group()` → `Group`：零设施的内核调度组（`spawn` 每任务一线程，
  `wait` 无锁等待，组可复用）。

---

## 15. 错误码索引（37 个，详见 `docs/errors.md` / `zxc explain CODE`）

| 类别 | 码 |
|---|---|
| 词法 | E0101 未闭合字面量/注释 · E0102 非法字符 |
| 语法 | E0200/E0201/E0202 · E0211 内建已删除（给替代写法） |
| 类型 | E0300–E0308（不匹配/字面量越界/实参/成员/形状/运算符/聚合字面量/返回类型） |
| 名字与可见性 | E0401 未知标识符 · E0402 重复声明 · E0403 不可赋值 · E0404 私有成员 · E0405 控制流位置错 · E0410 未使用 · E0411 返回值未接 |
| 编译期规则 | E0501 须编译期可算 · E0502 控制流完整性 · E0503 指针逃逸栈帧 · E0504 switch 不完整 · E0505 编译期除零 · E0506 编译期 null 解包 · E0507 @compileError/@trap 到达 · E0508 保留错误码 |
| 模块 | E0601 模块解析失败 |
| 泛型 | E0701 实例化失败 |
| 格式串 | E0802 占位符与实参不匹配 |
| 错误集 | E0900–E0903（错误码不在集合内 / try 越界传播） |

---

## 16. 工具链

```sh
zxc file.zx -o prog       # 编译（生成 C → zig cc）
zxc file.zx --check       # 只检查（默认行为）
zxc file.zx --emit-c      # 打印生成的 C
zxc file.zx --ast / --tokens
zxc run [-- prog args]    # zx.json 工程：构建并运行
zxc build / test / fmt / check
zxc include <lib>         # 把本地 C 库接进来（vendor/，自动生成绑定）
zxc add <url> / fetch / sync / cbuild
zxc errors / explain E0301
```

- 工程文件 `zx.json`；`zxc init name` 生成骨架（src/、vendor/、zx.json）。
- 环境变量：`ZX_CC` 指定 C 编译器；`ZX_VERBOSE=1` 打印构建细节。
- 增量编译按模块缓存 .o；`--release` 走 -O2。
- LSP（`bin/zxls.exe`）：诊断、补全（`.` 成员 / `@` 内建 / snippet）、跳转定义、
  hover、inlayHint（未写类型的 `const/let/var` 显示 `: Type`）、签名帮助、引用、
  格式化。Zed 配 `lsp.zx-lsp.binary.path` 指向 zxls；换 exe 后要重启编辑器。
- **fmt 规则（树状铺开）**：4 空格缩进、去尾空白、连续空行压 1、末尾单换行
  （CRLF 文档保持 CRLF）。一行里或一段手工换行的区域中，只要**存在以 `,`
  收尾的组**（`(...)`、字面量 `.{ … }`、`T{ … }`），fmt 就把整个最外层组
  铺开成规范树：一行一个实参/字段、逐层缩进、闭括号独立成行，缺的尾逗号
  由 fmt 补齐；内层组不带尾逗号（如 `.{.b = 120}`）保持单行。区域内完全
  没有尾逗号的手工排版原样保留；有注释的区域不动。再跑一遍 fmt 输出
  一字不变（幂等），fmt 产物保证可编译。

---

## 17. 与 C / Zig 的关键差异

| 特性 | Zx | C | Zig |
|---|---|---|---|
| 数组 | **纯指针** `*[N]T`（8 字节） | `T[N]` 值类型 | `[N]T` 值 |
| 解引用 | `.0`，`.` 自动穿透 | `*p` / `p->` | `p.*` |
| 块的值 | 只来自收割 `tag: v;` | 语句 | 末尾表达式 |
| 跳出 | `continue label` + 收割（无 break） | `goto` | `break :label` |
| 空值捕获 | `if (x) v { }`（无竖线） | — | `if (x) \|v\| { }` |
| 转换 | 只有 `(T)v` | 隐式一大堆 | `@intCast` 等 |
| 反射 | 唯一 `@type(x)` | 无 | `@typeInfo` |
| 丢弃 | `_`（不是名字） | `(void)x` | `_ = x;` |
| 错误 | `!T` / `error{…}` / `try` | 返回码 | `!T` / `try` |

---

*本文由 std 源码与 spec 生成；语言演进以 `spec/zx-spec.md` 的裁定为准。*
