# Zx 手册

> 面向第一次写 Zx 的人：从装工具到写出带并发和 JSON 的程序。
> 逐条查阅语法与 API 用 `docs/reference.md`；错误码逐条解释用
> `zxc explain E0301` 或 `docs/errors.md`。
> 本文所有代码都可以直接 `zxc` 编译运行——示例就是照着实现写的。

---

## 1. 第一个程序

```zx
const std = module("std");

const main = fn () u8 {
    std.println("hello, {}!", "zx");
    return 0;
};
```

编译运行：

```sh
zxc hello.zx -o hello
./hello
```

要点：

- 函数只有一种写法：`const 名字 = fn (参数) 返回类型 { … };`。
  **没有 `pub fn`**——`pub` 是前缀修饰符，写在声明前面。
- 入口是 `main`，返回 `u8` 作退出码；也可以写 `fn () !u8`，错误 tag 就是退出码。
- `std.println("{}", x)` 直接传参，不是元组。
- 别的文件里的名字必须先 `module("路径")` 绑定才能用，没有隐式跨文件名。

---

## 2. 绑定：const / let / var

```zx
const N = 2 + 3 * 4;        // 编译期绑定（R1）：初始化必须算得出来
let  x: i32 = f();          // 运行时绑定，不可再赋值
var  y: i32 = 0;            // 可变绑定
y = y + 1;
_ = N; _ = x;               // 没用到的绑定报 E0410，用 `_` 丢弃
```

- `const` 是编译期常量，可以当数组长度、类型实参用。
- `let`/`var` 是运行时值；**函数体里的 `const` 同样要求编译期可算**，
  拿运行时值请写 `let`。
- 未使用的 `let`/`var`/形参报 E0410；`_` 不是名字，可重复出现。
- 变量必须先赋值后使用；把指向当前栈帧的值存进活得更久的地方报 E0503（逃逸检查）。

## 2.1 可见性

- 顶层声明与容器成员默认**私有**，只在声明它的文件内可见；`pub` 才对外
  （跨文件访问私有成员报 E0404，补全也不显示）。
- `enum` / `union(enum)` 的变体是值，恒公开，不用逐个写 `pub`。

---

## 3. 类型

| 写法 | 大小 | 说明 |
|---|---|---|
| `u8…u128` `i8…i128` `usize` `isize` | 1–16 | 整数，字面量支持 `0x`/`0b`/`0o` 与 `1_000_000` |
| `f16 f32 f64` | 2/4/8 | IEEE-754，`3.5e2` 科学计数 |
| `bool` | 1 | `true` / `false` |
| `void` | 0 | |
| `type` | — | 类型本身是值，仅编译期 |
| `{T1, T2}` | 字段之和 | 元组 |
| `*[N]T` | **8** | 见下——Zx 没有数组值 |
| `[]T` / `[]const T` | 16 | 切片 `{ptr, len}` |
| `?T` | 1+pad+T | 可选值 |
| `!T` / `E!T` | 2+pad+T | 错误联合体 |
| `struct` / `union` / `enum` | 按成员 | §5 |

### 3.1 数组就是指针

这是 Zx 刻意和 C 划清界限的地方：

- `*[N]T` 是**一个 8 字节指针**，`N` 写在类型里，编译器拿它做界检查和
  `.len`/`.size`。`*[3]i32` 和 `*[4]i32` 是不同类型。
- 存储从哪来：`std.stackAlloc(T, n)`（栈）、`std.heapAlloc(al, T, n)`（堆）、
  `&.{ … }`（匿名存储的糖）。
- `&xs`（xs 是 `*[N]T` 变量）是**取这个指针变量的地址**，得到 `**T`——
  不是"数组的地址"，因为 Zx 里没有数组变量。

```zx
var xs: *[3]i32 = &.{ 1, 2, 3 };
xs[0] = 10;                          // 运行期下标
let first = xs.0;                    // 编译期下标（.N）
std.println("{} {}", xs.len, xs.size);   // 3 12
```

`[]T` 是 16 字节的 `{ptr, len}` 视图；`xs[0..2]` 切出子视图，
`hi` 省略表示到结尾。

---

## 4. 函数与泛型

```zx
const add = fn (a: i32, b: i32) i32 { return a + b; };

const scale = fn (T: type, v: T, k: T) T {   // 形参里有 T: type → 编译期函数
    return v * k;
};

const main = fn () u8 {
    std.println("{}", scale(u32, 6, 7));     // 42
    return 0;
};
```

- 函数是一等值：`fn (i32) i32` 是类型，可以当参数传。
- 泛型实例 `std.ArrayList(i32)` 是一次编译期调用，产生一个具体类型。
- 返回 `!T` 的函数每条路径都要 `return` 或走到 `noreturn`（E0308）。

---

## 5. 聚合类型

```zx
const Point = struct { x: i32 = 0, y: i32 = 0 };   // 字段可给默认值

const Color = enum(u8) { red, green = 4, blue };    // 枚举就是整数

const Shape = union(enum) {                         // 带标签联合
    circle: f64,
    rect: { f64, f64 },                             // 载荷是元组类型
};

const Counter = struct {
    n: i32 = 0,
    const bump = fn (self: *Counter) i32 {          // 首参 self → 方法
        self.n = self.n + 1;
        return self.n;
    };
};

const main = fn () u8 {
    let p = Point{ .y = 2 };                        // 指名初始化，默认值可省
    let c = Color.blue;                             // 变体做实参要全名
    var s = Shape{ .circle = 1.5 };
    let area = switch (s.tag) {                     // 分派走 .tag
        .circle: s.circle * 3.0;
        .rect:  s.rect.0 * s.rect.1;
    };
    var cnt = Counter{};
    _ = cnt.bump();                                 // p.method() ≡ T.method(&p)
    std.println("{} {} {}", p.y, c == Color.blue, area == 4.5);
    return 0;
};
```

- 字面量 `T{ .f = v }` 编译期检查完整性；匿名结构 `.{ .x = 1 }` 靠期望类型定型。
- `switch` 臂是 `模式: 体;`，整数/枚举必须完整划分（不完整报 E0504）。
- plain `union` 读错成员是未定义行为，一律用 `union(enum)` + `.tag` 分派。
- 元组访问：`t.0` 与 `t[0]` 等价；`tup[N]` 的 **N 必须是编译期值**
  （元组是异构的，运行时下标给不出单一返回类型，E0501）——遍历配
  `inline for`，越界编译期就报。

---

## 6. 控制流

```zx
// if 是表达式；块想交值用收割 tag: v;
let grade = if (score >= 60) blk: {
    passed = passed + 1;
    blk: 1;
} else 0;

// 可选值 / 错误联合体的捕获（无竖线）
if (opt) v { use(v); } else { … }
if (result) v { use(v); } else { … }

// 循环：没有 break，条件不满足自然结束；也没有 else 分支（E0405）
for (0..n) i { … }
for (xs) x { … }                 // 遍历切片
for (xs, 0..) x, i { … }         // 带下标
while (i < n) { i = i + 1; }     // 没有 while (c) : (续写) 语法
while (r.next()) line { … }      // while 也能捕获

// inline for：编译期展开，i 是常量——读反射字段只有这条路
inline for (0..info.fieldCount) i {
    const f: Field = info.fields[i];
}
```

- `defer` 离开作用域倒序执行；`errdefer` 只在以错误返回时执行（函数须返回 `!T`）。
- `continue` 有；**`break` 没有**——多层退出用 `continue label` 或收割。
- **收割退出恒真循环**：`n: while(true) { …; n:{}; }` 里的空收割是正常出口，循环后面
  的代码可达；带值收割 `var got = m: while(true) { …; m: x; };` 让循环交出值。不带标签的
  `while (true)` 永远退不出来（后面报 E0502 unreachable）。
- `for` 的循环变量是 `usize`：和别的宽度混算要显式 `(T)i`。
- `inline for` 的体里可以给外面的局部 `var` 赋值（`off = off + d.fields[i].offset;`）。

---

## 7. 空值与错误

```zx
// ?T：null 是字面量；解包三件套
let u = opt orelse 0;            // 空则取右值（右边必须是表达式）
let w = opt.?;                   // 断言非空，空则 trap
if (opt) v { … }                 // 捕获

// !T：错误码是 u16，error.name 直接当值
const read = fn (path: []const u8) !i32 {
    if (std.file.read(std.heap.page, path)) data {
        return (i32)data.len;
    }
    return error.noent;
};

const main = fn () !u8 {
    let n = try read("a.txt");       // 出错就把错误码原样返回（try 出来是
    std.println("{}", n);            // 运行时值，用 let 不用 const）
    return 0;
};
```

**没有 `catch` 语法，也没有 `orelse return`**（`orelse` 右边必须是表达式）。
处理错误就三招：`if (x) v { … } else { … }` 捕获、`.?` 断言、`try` 传播。
`try` 只能写在返回 `!T` 的函数里。

---

## 8. 编译期与反射

- `const` 初始化器、泛型实参、`inline for` 都在编译期执行；
  编译期除零/越界/null 解包直接报错（E0505/E0506）。
- 转换只有一种写法 `(T)v`，覆盖窄化/扩宽、整数↔浮点、指针互转、指针↔整数。
  没有隐式数值转换（字面量除外，会向期望类型定型并检查拟合）。
- **内建只有 7 个**：`@type @memcpy @memmove @memset @link @compileError @trap`。

`@type(x)` 是唯一反射，x 可以是类型或表达式：

```zx
const Point = struct { x: i32, y: i32 };

const describe = fn (T: type) void {
    const info: Type = @type(T);                 // 描述符只能 const 绑定
    std.println("{} {}B {}字段", info.name, info.size, info.fieldCount);
    inline for (0..info.fieldCount) i {
        const f: Field = info.fields[i];         // i 是编译期常量才折得动
        std.println("  {}: {} @{}", f.name, @type(f.ty).name, f.offset);
    }
};
```

| 字段 | 说明 |
|---|---|
| `.name` / `.size` / `.align` | 名字 / 字节大小 / 对齐 |
| `.ty` | 类型本身（表达式实参时 = 它的类型） |
| `.fieldCount` / `.fields[i]` | 逐字段 `.name` / `.ty` / `.offset` |

`@sizeOf`/`@typeName` 等旧拼写报 E0211 并提示替代写法。

---

## 9. 内存

```zx
const al = std.heap.page;                    // 现成分配器（malloc/free）

// 两个便捷函数：拿存储；要写入就用 var 绑定
var xs = std.stackAlloc(i32, 4);             // 栈，*[4]i32
var ys = std.heapAlloc(al, i32, 100);        // 堆，OOM abort；let 绑定只读

// 手动分配：Allocator 接口 { ctx, alloc, free }，alloc 返回 ?*u8
let raw = al.alloc(al.ctx, 64);
if (raw) p { al.free(al.ctx, p, 64); }

// arena：批量分配一次总释放
if (std.arena.ArenaAllocator.init(al)) a {
    let s = std.str.concat(a, "he", "llo");  // ?[]u8，归 arena 管
    _ = s;
}                                            // 出作用域全释放
```

- std 约定：**要分配的函数，分配器放第一个参数**；返回带 NUL 的结果可直接传 FFI。
- `std.pool.Pool`：19 档尺寸分级内存池（16B–256KiB），`alloc/free/used`，
  槽位从不搬动。
- arena 的 `free` 是 no-op，不逐块释放；**不是线程安全**，别跨 Task 共享。

---

## 10. std 速览

全部是普通 Zx 代码（`std/` 目录），完整签名见 `docs/reference.md` §14。

```zx
// ArrayList：可增长数组
var xs = std.ArrayList(i32).init(al);
defer xs.deinit();
_ = xs.push(1);
if (xs.first()) v { _ = v; }

// HashMap：键 []const u8，开放寻址；没有 remove，要删就重建
var m = std.map.HashMap(i32).init(al);
defer m.deinit();
_ = m.put("k", 1);
std.println("{}", m.get("k") orelse 0);

// 字符串
let n = std.str.parseInt("42");              // ?i64
let parts = std.str.split(al, "a,b", ",");   // ?[][]const u8

// JSON：零拷贝解析，错误处理用捕获（没有 catch）
if (std.json.parse(al, text)) v {
    if (std.json.member(v, "name")) m2 {
        if (std.json.text(m2)) s2 { use(s2); }   // text 只对 str 成员有值
    }
}

// 文件：整块读写 + 目录流式遍历（没有 break，用 while 捕获收尾）
_ = std.fs.writeAll("out.txt", "hi");
if (std.fs.DirIter.open(al, ".")) dd {
    var d = dd;                          // next()/isDir() 要可变接收者
    defer d.close();
    while (d.next()) name { use(name); }
}

// Task：唯一的并发抽象。coro()/single()/pool(n) 三种实例
const job = fn (ctx: usize) !usize { return ctx * 2; };
var t = std.task.Task.single();
defer t.close();
if (t.spawn(job, 21)) h {
    if (t.await(h)) r { std.println("{}", r); }   // 42
}
```

编译器 seed 的内建面（不是 Zx 代码）：`std.print/println/format`、
`std.heap.page`、`std.stackAlloc/heapAlloc`、`std.file.*`、`std.io.*`、
`std.process.args/getEnv/setEnv/exit`（**args 不含 argv[0]**）、
`std.debug.panic/assert/errName`、`std.atomic.*`。

---

## 11. 工具链

```sh
zxc main.zx -o prog        # 编译（生成 C → zig cc）
zxc main.zx                # 只检查
zxc main.zx --emit-c       # 看生成的 C
zxc run -- --flag          # 工程构建并运行（读 zx.json）
zxc test                   # 跑 src/test/ 里的零参 pub 函数
zxc fmt src/               # 格式化
zxc explain E0404          # 解释一个错误码
zxc init myproj            # 生成工程骨架
```

- 工程描述在 `zx.json`；`include/add/fetch/sync/cbuild` 管 vendor/ 下的 C 库。
- `--release` 走 -O2；增量编译按模块缓存 .o。
- **fmt（树状铺开）**：4 空格缩进、去尾空白、压空行；同行或手工换行区域里
  只要存在以 `,` 收尾的组，fmt 就把最外层组铺成规范树（一行一个、逐层缩进、
  闭括号独立行，缺的尾逗号补齐）；完全没有尾逗号的手工排版原样保留，
  幂等，产物可编译。
- LSP（`bin/zxls.exe`）：诊断、补全（含字段类型 detail 与 snippet）、
  hover、inlayHint（未写类型的绑定显示 `: Type`）、签名帮助、格式化。
  编辑器里指到 zxls 路径；**换 exe 后要完全重启编辑器**。

---

## 12. 常见坑（每条都实测踩过）

| 症状 | 原因 | 正确写法 |
|---|---|---|
| E0201 `found 'return'` | `orelse return` 不存在 | `if (x) v { … } else { return e; }` |
| E0201 `found 'catch'` | 没有 catch 语法 | if 捕获 / `.?` / `try` |
| E0201 `found ':'` | while 没有续写语法 | 递增写在循环体里 |
| E0301 数值混算 | 没有隐式转换 | `(i32)a.len + b` |
| E0303 方法调在类型上 | 静态/实例空间分离 | `T.make()` 静态、`t.method()` 实例 |
| E0404 私有成员 | 容器成员默认私有 | 声明处标 `pub` |
| E0410 未使用 | 每个绑定都要用 | `_ = x;` 丢弃 |
| E0501 const 需编译期 | 函数体 const 也是 R1 | 运行时值用 `let` |
| switch 臂报语法错 | 值体臂要分号 | `.red: "r";` |
| enum 变体传参报错 | 不吃 `.blue` 糖 | 写全名 `Color.blue` |
| union 读不出成员 | plain union 无标签 | `union(enum)` + `.tag` 分派 |
| 恒真循环后全报 unreachable | 体里没有收割循环标签，退不出来 | 体里加 `n:{};` / `n:v;` 收割 |

---

*本文按 Zx 1.2.3 实现逐片段实测写成；语言演进以编译器行为为准，
API 细节以 `std/` 源码与 `docs/reference.md` 为准。*
