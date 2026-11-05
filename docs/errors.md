# 错误码参考

每条诊断都带一个码：`error[E0304]: \`Obj\` has no field 'coutn'`。
**码一旦发布就不再改**（见 `src/zxc.c` 里 `g_rules` 上方的注释）——它是对外契约，
可以拿去搜索、贴进 issue、写进 CI 的失败判定。

查法：

| 入口 | 用途 |
| --- | --- |
| `zxc errors` | 按类别列出全部 34 个码 |
| `zxc explain E0304` | 单个码：什么意思、通常怎么修（大小写不敏感），末尾给本文链接 |
| 编辑器里点诊断上的码 | 由语言服务器给出 `codeDescription`，直接打开本文对应的一条 |
| 本文 | 完整版：触发条件、典型消息、示例 |

每条标题前有显式锚点 `<a id="e0304"></a>`，所以 `本文地址#e0304` 能精确定位。
锚点是小写码，和 `zxc explain` 输出的链接、语言服务器给的链接三处一致。

码的来源有两处，改的时候要一起改：

- `src/zxc.c` 的 `g_rules`：**消息片段 → 码**的匹配表，是码的唯一真源
- `src/errors.c` 的 `g_errs`：面向用户的标题与修法，`zxc explain` 念的就是它

`sh test.sh` 会校验本文、`g_errs`、`g_rules` 三方的码集完全一致，新增码漏改任何一处都会红。

---

## 词法 E01xx

<a id="e0100"></a>
### E0100 词法错误（未归类）

词法器报了错但没归到更具体的码。正常情况下不该出现——遇见了说明词法器的
错误文案需要补一条 `g_rules` 规则。

<a id="e0101"></a>
### E0101 字面量或注释没有闭合

**典型消息**：`unterminated string`、`unterminated block comment`

字符串、字符字面量、块注释到文件结尾都没等到配对符号。最常见的是想打一个
反斜杠却打成了引号：

```zx
const p = "C:\temp";       // `\t` 被当成制表符，后面引号提前收尾
const q = "C:\\temp";      // 对：转义反斜杠
```

<a id="e0102"></a>
### E0102 非法字符或非法字面量

**典型消息**：`invalid character`、`invalid escape`

- 字符串里出现裸换行（多行字符串要写成相邻字面量拼接）
- `\x` 转义不是两位十六进制
- 标识符里出现非 ASCII（`let 名字 = 1;` —— 词法器只认 ASCII 标识符）

---

## 语法 E02xx

<a id="e0200"></a>
### E0200 语法错误（未归类）

语法层的兜底码。从报错位置往前找最近一个没写完的结构。

<a id="e0201"></a>
### E0201 期待某个记号——结构没写完

**典型消息**：`expected ';'`、`expected '}'`、`expected ';' after switch arm`

报错文字里已经写明缺的是什么，补上即可。三个高频：

```zx
const main = fn () u8 { return 0; }        // 少了结尾的 `;`（声明要以 `;` 收尾）
const main = fn () u8 { return 0; };       // 对

switch (n) {
    0: { };                                 // switch 每臂都要 `;`（含末臂）
}
```

<a id="e0202"></a>
### E0202 记号出现在不允许的位置

**典型消息**：`unexpected token in top-level declaration`、`only be used inside …`

多见于：顶层多写了语句（顶层只放声明）、表达式里混进声明、`break` 这类不存在的关键字。
Zx 没有 `break`/`continue` 带值那套，提前求值用**标签收割**：

```zx
const r = blk: {
    if (cond) { blk: 1; }     // 相当于 break blk 1
    blk: 2;
};
```

---

## 类型 E03xx

<a id="e0300"></a>
### E0300 类型错误（未归类）

类型层的兜底码。看指位处表达式的类型和期望类型。

<a id="e0301"></a>
### E0301 类型不匹配，且不能隐式转换

**典型消息**：`mismatched types`、`initializer: expected \`i32\`, found \`[]const u8\``

Zx 不做隐式数值转换，也不做字符串/切片互转：

```zx
let a: i32 = 1;
let b: i64 = a;            // E0301
let b: i64 = @intCast(a);  // 对
```

<a id="e0302"></a>
### E0302 字面量超出目标类型范围

**典型消息**：`literal does not fit in \`u8\``、`negative literal`

```zx
let c: u8 = 300;      // E0302
let d: u32 = -1;      // E0302
```

<a id="e0303"></a>
### E0303 参数个数或类型对不上

**典型消息**：`takes 2 argument(s), 1 given`

Zx 没有默认参数和重载，个数必须对上。

<a id="e0304"></a>
### E0304 类型没有这个成员、字段或变体

**典型消息**：`` `Obj` has no field 'coutn' ``（常跟 `help: did you mean \`count\`?`）

成员存在但**不可见**时报的是 E0404，不是这个。

```zx
const Obj = struct { count: i32 };
let o = Obj{ .count = 1 };
let n = o.coutn;          // E0304
```

<a id="e0305"></a>
### E0305 类型形状不合法

**典型消息**：`cannot nest optional`、`pointer has no count`

- `?T` 不能再套 `?`（`??i32` 非法）；要两层就显式建模
- 多级指针写全：`* *T`，不写 `**T`

<a id="e0306"></a>
### E0306 运算符或操作数类型不匹配

**典型消息**：`arithmetic needs numbers`、`cannot add two pointers`、`is not a sequence`

指针不能相加（可以相减求距离）；切片拼接要求元素类型相同。

<a id="e0307"></a>
### E0307 联合体字面量必须指定变体

联合体没有"空"状态，必须点明初始化哪个变体：

```zx
const Shape = union { circle: f64, rect: {f64, f64} };
let s = Shape{};                      // E0307
let s = Shape{ .circle = 1.0 };       // 对
```

---

## 名字与可见性 E04xx

<a id="e0401"></a>
### E0401 未知的标识符或类型

**典型消息**：`unknown identifier 'x'`、`unknown type 'Color'`（常带 `did you mean`）

```zx
const Colour = struct { r: i32 };
let c: Color = undefined;      // E0401 —— help 会说 `did you mean \`Colour\`?`
```

<a id="e0402"></a>
### E0402 重复声明

**典型消息**：`duplicate declaration 'helper'`，后面跟 `note: 'helper' first declared here`

同名声明只能有一处。结构体成员、模块顶层声明都适用。

<a id="e0403"></a>
### E0403 不可赋值 / 不可取地址 / 只读

**典型消息**：`cannot assign to 'x'`、`cannot take the address of …`

`let`/`const` 绑定不可改（要改就用 `var`）；`&` 只能作用于左值。

<a id="e0404"></a>
### E0404 私有成员，或声明位置不合法

**典型消息**：`'secret' is private to module 'lib'; mark it \`pub\` to export it`

跨模块访问要对方 `pub`。后面跟的 `note` 会指出它在哪个文件哪一行声明。

<a id="e0405"></a>
### E0405 控制流语句放错位置

**典型消息**：`'continue' outside a loop`、`'return' outside a function`、`inside \`defer\``

`defer` 块里不能 `return`（它不改变函数出口）。

<a id="e0410"></a>
### E0410 未使用的局部变量、参数或绑定

**典型消息**：`unused variable 'tmp'`、`unused parameter 'ctx'`、`unused binding 'v'`

```zx
const main = fn () u8 {
    var tmp = 5;            // E0410 —— 声明了从没碰过
    let _scratch = 5;       // ok：`_` 开头就是"我知道它没被用"
    return 0;
};
```

函数体里每一个 `let` / `var` / 参数 / 捕获绑定都得被读或写一次。用不着的
写成 `_x`（还是普通变量，只是免检）或 `_`（丢弃，连槽位都没有）。

**为什么是错误而不是警告**：Zx 编到 C，未使用变量在 C 里只是一条 warning，
而用户看不到 C。更要紧的是"声明了从没用过"几乎总意味着写漏了一半——
打算用、结果没接上，或者名字拼错（拼错的那处报 E0401，但只写不读的拼错
完全静默）。要求显式写 `_` 是把意图写出来。

**错误恢复下不报**：函数体里已经有过别的错误时，这条静默——检查中途收尾
会让"用到了"记录不全，报出来只是噪音。修完错重编，它会自己出现。

---

## 编译期与语言规则 E05xx

<a id="e0501"></a>
### E0501 这个值必须能在编译期算出来

**典型消息**：`must be computable at compile time`

`const` 初值、数组长度、泛型实参都要求编译期常量。运行时才算的值用 `let`：

```zx
const now = std.time.now();     // E0501
let now = std.time.now();       // 对
```

<a id="e0502"></a>
### E0502 控制流完整性不成立

**典型消息**：`can reach the end of its body`、`declared \`noreturn\``

有返回值的函数每条路径都要 `return`。`if` 的两个分支都 return 才算覆盖。

<a id="e0503"></a>
### E0503 指针逃逸出当前栈帧

**典型消息**：`cannot return a pointer into the current function's stack frame`

返回指向局部变量的指针/切片，调用方拿到的是悬垂地址。要活得久就用堆：

```zx
const bad = fn () *i32 {
    var x: i32 = 1;
    return &x;                 // E0503
};

const ok = fn () *i32 {
    let p = std.heapAlloc(1) orelse unreachable;
    p.* = 1;
    return p;
};
```

<a id="e0504"></a>
### E0504 switch 没有穷尽

**典型消息**：`switch on enum is not exhaustive`

枚举、`union(enum)`、`?T`、`!T`、`bool` 要列全所有模式，或加 `else` 兜底。

<a id="e0505"></a>
### E0505 编译期除以零

常量除法在编译期就会报：

```zx
const x = 1 / 0;        // E0505
```

<a id="e0506"></a>
### E0506 对编译期就是 null 的值做 `.?` 解包

```zx
let x: ?i32 = null;
let y = x.?;            // E0506 —— 编译期就知道必然失败
```

<a id="e0507"></a>
### E0507 `@compileError` 触发 / `@trap` 在编译期执行到

这是主动报错，不一定是 bug：泛型里常用它表达"这组实参不被支持"。

```zx
const check = fn (comptime T: type) void {
    if (@sizeOf(T) == 0) @compileError("zero-sized types not supported");
};
```

<a id="e0508"></a>
### E0508 错误码用了保留值

`0` 表示"没有错误"，`65535` 留给 `error.null`，都不能手写：

```zx
const E = enum(u16) { none = 0 };      // E0508
```

---

## 模块 E06xx

<a id="e0601"></a>
### E0601 模块解析失败

**典型消息**：`module 'x' not found`、`circular module dependency`、
`standard library modules are not available`（找不到 `std/`）

- 路径按**当前文件所在目录**解析，再看工程根与 `vendor/`
- 循环依赖要去掉一条边（把共享类型挪到第三者）

```zx
const util = module("util");     // 找 util.zx：同目录 → 工程根 → vendor/
```

---

## 泛型 E07xx

<a id="e0701"></a>
### E0701 泛型实例化失败

**典型消息**：`generic instantiation failed`、`type parameters`

函数体对某组实参不成立。后面通常还跟着一条更具体的诊断，那才是真正的原因。

---

## 内建与格式串 E08xx

<a id="e0802"></a>
### E0802 格式串与参数不匹配

**典型消息**：`too many arguments for the format string: 1 given, 0 needed`、
`no argument for format specifier`

```zx
std.println("{}", 1, 2);         // E0802：多给了一个
std.println("{[1]}", 1);         // E0802：索引越界
```

格式串语法：`{[索引][:填充][对齐][宽度][.精度]}`，如 `{[1]:>8.2}`。

---

## 错误集与传播 E09xx

Zx 的错误集是**类型的一部分**：`!T` 表示任意错误，`!{a, b}T` 表示只能是这两种。

<a id="e0900"></a>
### E0900 内部错误（未归类）

诊断层的兜底码。正常情况下不该出现——遇见了多半是编译器的锅，
把最小复现贴到 issue。

<a id="e0901"></a>
### E0901 返回的错误码不在函数声明的错误集里

```zx
const bad = fn () !{Io} i32 {
    return (error)Mem;           // E0901：Mem 不在 {Io} 里
};
```

`help` 行会直接给出改完的返回类型写法。

<a id="e0902"></a>
### E0902 `try` 可能传播的错误码不在声明的错误集里

```zx
const f = fn () !{Io} i32 {
    return g() try;              // g 返回 !{Io, Mem} → E0902
};
```

`?T` 上的 `try` 会抛保留码 `error.null`，声明集里没有它同样报 E0902。

<a id="e0903"></a>
### E0903 `try` 传播任意错误集，但函数声明了窄集合

```zx
const g = fn () !i32 { return 1; };      // !i32 = 任意错误
const f = fn () !{Io} i32 {
    return g() try;                       // E0903
};
```

两个改法：把 `f` 的返回类型放宽成 `!i32`，或者在 `try` 之前用 `catch` 处理掉。
