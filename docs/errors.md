# 错误码参考

每条诊断都带一个码：`error[E0304]: \`Obj\` has no field 'coutn'`。
**码一旦发布就不再改**（见 `src/zxc.c` 里 `g_rules` 上方的注释）——它是对外契约，
可以拿去搜索、贴进 issue、写进 CI 的失败判定。

查法：

| 入口 | 用途 |
| --- | --- |
| `zxc errors` | 按类别列出全部 37 个码 |
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

**典型消息**：`expected ';'`、`expected '}'`、`expected ';' after switch arm`、
`expected a member name after '.'`

报错文字里已经写明缺的是什么，补上即可。三个高频：

```zx
const main = fn () u8 { return 0; }        // 少了结尾的 `;`（声明要以 `;` 收尾）
const main = fn () u8 { return 0; };       // 对

switch (n) {
    0: { };                                 // switch 每臂都要 `;`（含末臂）
}
```

点号后面必须跟成员名（标识符 / 整数下标 / `null`）。编辑器里打到一半的
`p.`、`arrs.0.` 就落在这里：报错指在点号上、语句照常收尾，不会再冒成
`unexpected token in top-level declaration` 把整条语句吞掉。

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

<a id="e0211"></a>
### E0211 内建已被删除

**典型消息**：`@intCast was removed: type conversion is written \`(T)value\``

1.2.0 把内建砍到九个；1.2.1 反射收敛成一个 `@type(x)`（x 可以是类型
也可以是表达式），1.2.2 把 `@sizeOf` / `@alignOf` / `@typeName` 也并了进去——
描述符 `Type{.name, .size, .align, .ty, .fieldCount,
.fields[i]{.name, .ty, .offset}}` 全是编译期值。现在共八个：

`@mod` / `@type` / `@cShim` / `@memcpy` / `@memmove` / `@memset` / `@link` / `@compileError` / `@trap`（共九个）。

| 老写法 | 换成 |
| --- | --- |
| `@intCast(T, v)` `@ptrCast` `@bitCast` `@as` | `(T)v` |
| `@typeOf(x)` | `@type(x).ty` |
| `@sizeOf(T)` `@alignOf(T)` `@typeName(T)` `@typeInfo(x)` | `@type(x).size / .align / .name` |
| `@hasField(T, "f")` | 遍历 `@type(T).fields` 比名字 |
| `@fieldName/@fieldType/@offsetOf(T, …)` | `@type(T).fields[i].name / .ty / .offset` |
| `@field(v, "f")` | 无动态成员访问；字段写死为 `v.f` |
| `@embedFile("p")` | 自己用 `std.fs` 读，或写成字面量 |
| `@file()` `@line()` `@col()` `@fnName()` | 已移除；位置只由 `std.debug` 自动注入 |
| `module("x")`（旧关键字） | `@mod("x")`；§144 起模块加载走内建，`module` 不再保留为关键字 |
| `.{1, 2}`（位置式元组，§146） | `{1, 2}`；数组写 `&.{1, 2}` |
| `.{a, b}`（解构模式，§146） | `{a, b}` |

---

<a id="e0300"></a>
### E0300 类型错误（未归类）

类型层的兜底码。看指位处表达式的类型和期望类型。

<a id="e0301"></a>
### E0301 类型不匹配，且不能隐式转换

**典型消息**：`mismatched types`、`initializer: expected \`i32\`, found \`[]const u8\``

Zx 不做隐式数值转换，也不做字符串/切片互转：

```zx
let a: i32 = 1;
let b: i64 = a;         // E0301
let c: i64 = (i64)a;    // 对：转换只有这一种写法 `(T)expr`
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
### E0307 聚合字面量不成立

联合体没有"空"状态，必须点明初始化哪个变体；同一个字段也不能初始化两次：

```zx
const Shape = union { circle: f64, rect: {f64, f64} };
let s = Shape{};                      // E0307
let s = Shape{ .circle = 1.0 };       // 对

const P = struct { x: i32, y: i32 };
let p = P{ .x = 1, .x = 2, .y = 3 };  // E0307 —— `.x` 写了两次
```

字段写两遍不是"后者生效"的语法糖：到底是第一条还是第二条起作用取决于
生成顺序，属于不写清楚的程序。

<a id="e0308"></a>
### E0308 返回类型不成立

**典型消息**：`` `error` is not a return type: it does not say whether the function can fail ``

返回类型只写 `error`（或只写错误集 `error{a, b}`）只说清了"码长什么样"，没说
这条返回路径**能不能失败、载荷是什么**——完整写法只有三种：

```zx
const f = fn () error { … };            // E0308 —— 光秃秃的 error
const g = fn () error{open} { … };      // E0308 —— 只说集合，没有 !T
const h = fn () error{open}!void { … }; // 对：集合 ! 载荷
const k = fn () !i32 { return error.open; };  // 对：anyerror ! 载荷
const m = fn () !void { … };            // 对：只会失败的副作用
```

`error` 作为**值的类型**仍然合法（`var e: error = error.open;`、`error.null`），
被禁的只是"它当一个函数的完整返回类型"。

这条也顺手治了一个老歧义：`fn () error {` 的 `{` 原来被当成错误集字面量
`error{…}` 的开头，报成 `expected identifier, found 'return'`（E0201）——
现在花括号按"后面跟的是错误名还是语句"两 token 前瞻，函数体照常归位。

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
§151 起 `self: Self` 是只读接收者，`self.x = …` 与透过 `&self` 写入
（`p.x = …`）同样报本码；要改调用者请写 `self: *Self`。

<a id="e0404"></a>
### E0404 私有成员，或声明位置不合法

**典型消息**：
- `'secret' is private to module 'lib'; mark it \`pub\` to export it`
- `'n' is a private member of \`HashMap\` (declared in std/map.zx); mark it \`pub\` in the container body to expose it`

跨模块访问要对方 `pub`。后面跟的 `note` 会指出它在哪个文件哪一行声明。
2026-11-11 起容器体成员（字段 / 方法 / 静态成员）同样默认私有：声明在别的文件里、
又没带 `pub` 的成员，访问即报本码——枚举与 `union(enum)` 的变体是值，恒公开。

<a id="e0405"></a>
### E0405 控制流语句放错位置

**典型消息**：`'continue' outside a loop`、`'return' outside a function`、`inside \`defer\``、
`` `for` does not take an `else` branch ``

`defer` 块里不能 `return`（它不改变函数出口）。

**循环没有 else 分支**：

```zx
while (n < 3) { n = n + 1; } else { n = 0; }   // E0405
for (0..3) i { … } else { … }                 // E0405
```

收尾逻辑写在循环后面；想区分"循环跑没跑过"用计数或标志位。

<a id="e0410"></a>
### E0410 未使用的局部变量、参数或绑定

**典型消息**：`unused variable 'tmp'`、`unused parameter 'ctx'`、`unused binding 'v'`

```zx
const main = fn () u8 {
    var tmp = 5;            // E0410 —— 声明了从没碰过
    let _scratch = 5;       // E0410 —— 加 `_` 前缀不再免检，它就是个普通名字
    let _ = 5;              // ok：裸 `_` 是唯一的丢弃写法
    return 0;
};
```

函数体里每一个 `let` / `var` / 参数 / 捕获绑定都得被读或写一次。用不着的
值只有一种写法：**裸 `_`**（`let _ = f();` 或 `_ = f();`，连槽位都不建）。
`_x` 这类"加前缀求豁免"的旧写法已删除——它让"确实不用"和"名字拼错"
长得一模一样，正是这条检查要防的东西；现在 `_foo` 就是个普通变量，
不用照样报错。

**为什么是错误而不是警告**：Zx 编到 C，未使用变量在 C 里只是一条 warning，
而用户看不到 C。更要紧的是"声明了从没用过"几乎总意味着写漏了一半——
打算用、结果没接上，或者名字拼错（拼错的那处报 E0401，但只写不读的拼错
完全静默）。要求显式写 `_` 是把意图写出来。

**错误恢复下不报**：函数体里已经有过别的错误时，这条静默——检查中途收尾
会让"用到了"记录不全，报出来只是噪音。修完错重编，它会自己出现。

<a id="e0411"></a>
### E0411 返回值被丢弃而没有用 `_` 接收

**典型消息**：`return value of type `i32` is discarded; receive it with `_ = f(...)` or use the value`

```zx
const add = fn (a: i32, b: i32) i32 { return a + b; };

const main = fn () u8 {
    add(1, 2);              // E0411 —— i32 结果被默默扔掉
    _ = add(1, 2);          // ok：显式丢弃（M26）
    return 0;
};
```

语言规定**所有值都要被使用**。裸调用语句 `f();` 的非 void 返回值不许无声
消失——那几乎总是忘了接（比如 `parse()` 的结果、`!T` 里的错误码）。要丢
就得写出来：`_ = f(...);`。void 函数、赋值语句、`noreturn` 调用不受影响。

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

`[]T` 的 `.len` / `.size` 也存在这里：长度写在值里（切片的 `{ptr, len}`），
不是编译期常量。`*[N]T` 的 `.len` / `.size` 才是（长度写在类型里）：

```zx
let xs = std.heapSlice(std.heap.page, i32, 4);
let s = xs[1..];
const L = s.len;                // E0501
let L = s.len;                  // 对
```

<a id="e0502"></a>
### E0502 控制流完整性不成立

**典型消息**：`can reach the end of its body`、`declared \`noreturn\``

有返回值的函数每条路径都要 `return`。`if` 的两个分支都 return 才算覆盖。

恒真循环（`while (true)`）后面报 unreachable 时，先看循环体里有没有**收割自己的**
标签（`tag: {};` / `tag: v;`）——收割会跳出循环，是正常出口，后面的代码可达，
不报错。收割要求循环带标签；不带标签的 `while (true)` 体里再怎么收割也退不出来。

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
    let al = std.heap.page;
    var p = std.heapAlloc(al, i32, 1);    // *[1]i32，要写入就用 var
    p.0 = 1;                              // 解引用是 .0
    return (*i32)p;
};
```

<a id="e0504"></a>
### E0504 switch 的臂不构成合法划分

**典型消息**：`switch on enum is not exhaustive`、`` duplicate switch pattern `.x` ``

枚举、`union(enum)`、`?T`、`!T`、`bool` 要列全所有模式，或加 `else` 兜底；
同一模式（以及 `else`）只能出现一次——重复的臂在 C 里就是 duplicate case，
先匹配到哪条取决于生成顺序，不该让程序行为变成实现细节。

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
    if (@type(T).size == 0) @compileError("zero-sized types not supported");
};
```

<a id="e0508"></a>
### E0508 错误码用了保留值

`0` 表示"没有错误"，`65535` 留给 `error.null`，都不能手写：

```zx
const E = enum(u16) { none = 0 };      // E0508
```

`null` 这个**名字**也不许占：它的码是固定的 65535，不参与顺序编号，所以进不了
集合字面量——

```zx
const Bad = error{ null, boom };       // E0508：null 是保留码 65535
```

`error.null` 本身是能写的**值**（`return error.null;`、`e == error.null`），
它就是 `?T` 被 `try` 时抛的那个码；`std.debug.errName(error.null)` 得到 `"null"`。

<a id="e0509"></a>
### E0509 同一笔分配被释放了两次

编译器在 `std.heapAlloc` 的分配点发一个编译期「发行版本」，
`Allocator.free` 是消费这个版本的作废事件。直线代码里第二次消费同一个
版本就是硬错：

```zx
let p = std.heapAlloc(al, u8, 64);
al.free(al.ctx, (*u8)p, 64);
al.free(al.ctx, (*u8)p, 64);   // E0509
```

删掉第二次 `free`；确实要在别处释放时，把所有权显式转移出去。

只判「作废事件可见 + provenance 未断」的局部路径。分支里的释放、
`(usize)` 之类指针↔整数互转、指针存进聚合字段再取出、跨函数 / C 边界
/ 跨线程、循环里同一分配点反复发行，都标 opaque。**opaque 不会再退回
运行时兜底**（§135 已撤掉运行时 side table）：句柄类容器的访问点在这种
情况下报 E0511，普通堆指针的账本只做观测、不报错也不误报。这是**局部
可证明安全**，不是全局内存安全。

<a id="e0510"></a>
### E0510 读取一个编译期已确定失效的句柄

容器同时声明 `get` 与私有 `getUnchecked`，就是 opt-in 了静态句柄检查。
状态机能证明 `free(h)` / `deinit()` 已经结束这个句柄的生命，之后任何
`get(h)` 都直接报错——运行时没有表可以再查一次：

```zx
let h = t.create().?;
t.free(h);
let p = t.get(h).?;   // E0510：已失效还读
```

修法是不要再读，或把 `deinit` 挪到所有 `free` 之后。

`get` 在编译期是**检查点标记**：每个 `t.get(h)` 要么被改写成私有的
`getUnchecked`（纯地址、零检查），要么在 E0510 / E0511 被拒绝——
`get` 的函数体不会出现在生成的代码里。

<a id="e0511"></a>
### E0511 证明不了句柄还活着

句柄检查只在编译期做，没有运行时兜底。provenance 一旦断掉——`(usize)`
强转、存进结构体字段再取出、跨函数传递、在 `if` / 循环里发行——编译器
就不能声称它有效，于是直接拒绝：

```zx
const boxed = Holder{ .h = h };
let p = t.get(boxed.h).?;   // E0511：Handle 从字段取回，来源已断
```

把句柄留在局部变量里，`create` / `get` / `free` 全写在直线代码中；确实
需要放进聚合、跨函数传递时，把容器做成显式的 `?*T` 接口自己判断，不要
指望编译器替你证明。

这条与 E0509 / E0510 是同一份账本上的三种消费：E0509 判「同一笔分配释放
两次」，E0510 判「确定失效还读」，E0511 判「根本证明不了」。

---

## 模块 E06xx

<a id="e0601"></a>
### E0601 模块解析失败

**典型消息**：`module 'x' not found`、`circular module dependency`、
`standard library modules are not available`（找不到 `std/`）

- 路径按**当前文件所在目录**解析，再看工程根与 `vendor/`
- 循环依赖要去掉一条边（把共享类型挪到第三者）
- 子模块**自己**解析失败时，这条诊断挂在 `@mod(...)` 调用点上，并带上
  子模块第一条错的行列与消息：

  ```text
  src/main.zx:2:14: error[E0601]: module 'vendor/glfw.zx' failed to parse:
      751:35: hex literal needs digits after `0x` [E0102]
  ```

  先修那一条，别在当前文件里找：同一个模块的成员访问错误会被抑制，
  免得一串 "has no member" 盖住真正的原因。

```zx
const util = @mod("util");     // 找 util.zx：同目录 → 工程根 → vendor/
```

<a id="e0602"></a>
### E0602 `@cShim` 的 C 文件打不开

**典型消息**：`@cShim: cannot open C shim "impl.c" (resolved to …)`

- 路径按**写 `@cShim(...)` 的那份 `.zx` 所在目录**解析，不是当前工作目录
- 库把 shim 和 `.zx` 放同一目录就能自带实现（`std/simd.zx` → `std/xxx.c`）
- 只是**打不开**才报这个码；文件打得开但 C 本身编不过，是 `B1004`，
  行号直接指到那份 `.c`（zxc 用同一套 cc/flags 编它，不做行列映射）

```zx
@cShim("impl.c");      // 找 impl.c：与这份 .zx 同目录
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
