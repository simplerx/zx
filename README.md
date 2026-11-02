# zxc — Zx 语言编译器

Zx 的参考实现。规格见 `spec/zx-spec.md`，文法见 `spec/zx.ebnf`，裁定记录见 `spec/decisions.md`。

## 构建

```sh
sh build.sh          # → bin/zxc.exe
sh test.sh           # 语法 + 语义 + 反例 + 运行 + 多文件 五组回归
```

编译器本身用 C11 写，构建工具链是项目内 vendored 的 zig（`.toolchain/zig.exe`，
它的 `cc` 就是 clang + 自带 libc）。不依赖系统 gcc/clang。

> 为什么把 zig 拷进项目：这台机器上 anyzig 的 `zig cc` 存在缓存/权限问题
> （`CacheCheckFailed`、`unable to load tlcsprng.zig: AccessDenied`），
> 复制工具链到本地盘并显式指定 `ZIG_LOCAL_CACHE_DIR` / `ZIG_GLOBAL_CACHE_DIR`
> 后正常。`.toolchain/` 和 `.zc-cache/` 可以从版本控制里排除。

## 用法

```sh
bin/zxc.exe file.zx               # lex + parse + 类型检查（默认）
bin/zxc.exe file.zx --check       # 同上，显式写法
bin/zxc.exe file.zx --tokens      # 打印 token 流
bin/zxc.exe file.zx --ast         # 打印语法树（s-expression）
bin/zxc.exe file.zx --emit-c      # 只吐生成的 C
bin/zxc.exe file.zx -o prog       # 生成 C 并调 zig cc 编成可执行文件
```

后端用的 C 编译器是 `ZX_CC` 环境变量指定的命令，缺省是项目内的
`.toolchain/zig.exe cc`。

## 代码结构

| 文件 | 内容 |
|---|---|
| `src/zxc.h` | token 种类表（X-macro）、Token/Lexer/Buf 定义 |
| `src/zxc.c` | 诊断输出、可变长缓冲、token 名字表 |
| `src/lex.c` | 词法分析：关键字、数字（含进制/后缀）、字符/字符串转义、`{` 前扫描所需的一切 |
| `src/ast.h` | AST 节点：一个 Node 结构 + NodeKind 枚举；sema 阶段往节点上挂 `ty` / `sym` |
| `src/parse.c` | 递归下降 parser，含两处消歧（见下） |
| `src/ty.h` `src/ty.c` | 类型表示：结构类型 intern、名义类型按声明节点定身份、`size`/`align` 布局、打印 |
| `src/val.h` `src/val.c` | 编译期值模型：整数/浮点/布尔/字符串/类型/聚合/函数/指针 |
| `src/sema.c` | 名字解析（R5）+ 类型检查 + R7 免拷贝提升 + 标签收割类型统一 + 编译期求值 + 泛型单态化 |
| `src/backend.c` | C 后端：类型映射、控制流降级到 slot + goto、defer 展开、泛型实例发射、按模块切分多个 translation unit |
| `src/main.c` | 驱动（`--emit-c` / `-o` / `ZX_VERBOSE`）：写文件、mtime 比对、按需 `cc -c`、链接 |
| `spec/examples/*.zx` | 规格示例（`syntax.zx` 是语法回归用例） |
| `tests/good/*.zx` | 必须零错误；有 `fn main` 的还要输出和 `// expect-output` 一致 |
| `tests/good/program.zx` | 综合用例：定长向量 + 排序 + 统计 + 状态机 + 递归 |
| `tests/good/stdlib.zx` | 标准库用例：ArrayList（含自建 arena）+ 字符串 + 文件读写，跑真二进制比对输出 |
| `tests/good/captures.zx` | `if` 捕获名的三种合法尾巴：块、单值、以二元运算符开头的表达式 |
| `tests/good/tool.zx` | 整合用例：读文件 → split → parseInt → ArrayList → 插入排序 |
| `tests/good/map.zx` | `std.HashMap` 用例：增删改查 + 20 键扩容 + 遍历 |
| `tests/good/io.zx` | `std.io` + 缓冲写/逐行读/按块读/seek-tell |
| `tests/good/typeinfo.zx` | `@typeInfo` / `@hasField` / `@fieldType` 的各种类型 |
| `tests/good/inline_for.zx` | `inline for` 展开 / `@field` 左值 / 泛型 `eql` 全字段比较 |
| `tests/good/alloc.zx` | `stackAlloc` / `heapAlloc` / `undefined` 不分配 / 逃逸放行形态 |
| `tests/good/fields.zx` | `fields` 元表：结构体/枚举遍历、`f.ty` 当类型、`@field` 按名取值 |
| `tests/good/process.zx` | `std.process`：args / getEnv / setEnv / exit（noreturn） |
| `tests/good/io_fmt.zx` | `std.io.print` / `println`：写文件读回比对、stdout/stderr 句柄、聚合展开、变量格式串 |
| `tests/good/if_arm.zx` | `if` 单语句臂：`return` / `continue` / 捕获 + `return`；表达式语句臂 `if (c) f(); else g();`（含赋值臂、else-if 链） |
| `tests/good/arraylist.zx` | `ArrayList` 补全：insertAt / removeAt / first / last / reverse / resize / swap |
| `tests/good/method_fold.zx` | 折叠函数体里的方法调用（`ct_call` 复检幂等回归） |
| `tests/good/union_enum.zx` | `union(enum)` 糖：三种载荷形态、隐式枚举标签、`union(enum(u8))` 布局、方法 |
| `tests/good/switch_cap.zx` | switch 捕获：联合体载荷 / `?T` / `!T` / 枚举主语、单值臂、无捕获臂 |
| `std/std.zx` | 标准库主体（普通 Zx 代码）：`ArrayList`、`BufReader`、`BufWriter`、`map` / `str` 子模块 |
| `std/str.zx` | 字符串工具：比较/查找/trim/parseInt/split/concat |
| `std/map.zx` | `HashMap(V)`：FNV-1a + 线性探测的开放寻址表 |
| `tests/multi/alias/main.zx` | 多文件工程 + `const` 别名（`const square = mathx.square;`） |
| `tests/multi/*/main.zx` | 多文件工程：同目录下的 `.zx` 作为模块参与编译 |
| `tests/multibad/*/main.zx` | 多文件反例：取模块的私有名字、取不存在的名字 |
| `tests/bad/*.zx` | 必须报错，首行 `// expect: <错误信息片段>` |

## parser 里的三处关键消歧

1. **`{ … }` 是块还是元组**：从 `{` 扫到匹配的 `}`，只看顶层——出现 `;` 或
   `const`/`let`/`var`/`fn`/`defer`/`return`/控制流关键字就是块，否则是元组。
   强制块位置（函数体、`if` 臂、类型体、switch 体）跳过扫描。
   所以 `{42}` 是一元组，`{42;}` 是块，`{}` 是空元组；`&.{…}` 用 `.{` 词法单元避开这条判断。

2. **`IDENT ":"` 是标签还是收割**：后面跟 `for`/`while`/`switch`/`if` → 给该结构打标签；
   跟 `{` → 再套一次上面的块判断（块 → 标签作用域，元组 → 收割值）；
   其他 → 收割语句 `tag: expr;`。`true` / `false` 在这个位置是**模式**不是标签
   （`switch (b) { true: { … }, … }`），两个词特判排除。

3. **switch 臂里的模式 vs 捕获 vs 标签**：`模式 : 臂体`，模式可以是 `.成员` / 整数字面量 /
   `true`/`false`。冒号后的名字按"后面跟 `{` 还是 `:`"分流：`{` 是捕获，`:` 是块标签。
   两者都只允许"非 bool 的标识符"，否则 `true:` 会被读成"给块加标签"。

## 类型检查（M3）

三件事：**类型表示**（`ty.c`）、**名字解析**（作用域 + hoist + 惰性求值，`sema.c`）、
**表达式/语句检查**（每类节点一个 `check_*`）。

几条落到代码里的规则：

- **判等就是指针比较**。指针/切片/元组/`?T`/`!T`/`fn` 这些结构类型在构造时 intern，
  同构即同址；`struct`/`union`/`enum` 是名义类型，身份来自那条类型声明的 AST 节点。
  所以 `*[3]i32 ≠ *[4]i32 ≠ *i32` 不需要额外代码。
- **R5 与顺序无关**：进入作用域先 hoist 全部声明（含解构的每个名字、`fn` 声明），
  再按语句顺序检查；符号类型惰性解析，`state == 1` 时再被引用就是成环报错。
- **先签名后查体**：`fn` 先算出 `fn(…) R`，函数体排队到后面统一检查，
  所以自递归、互递归天然合法（不需要"占位类型"）。
- **R7 是唯一的隐式规则**，集中在 `can_coerce()` 一个函数里：
  `*[N]T → *T`、`*[N]T → []T`、`*T → *const T`、`T → ?T / !T`、字面量定型。
  反向（`*T → *[N]T`）一律要写 `(*[3]i32)p`，`castable()` 才放行。
- **标签是独立命名空间**，用栈实现；收割语句把类型交给 `Label.ty`，
  多处收割时用 `unify()` 统一（`usize` + `error` → `!usize`）。
- **可写性单独判**（`lvalue_mutable()`）：`var` 可以，`let`/`const`/参数/捕获不行；
  透过 `*const` 不行；但切成 `[]T` 之后元素是否可写只看切片的 `const`——
  这正是"切片值只读、被指的东西另说"。

## 编译期求值（M4）

`const` 是真求值，不是"检查一下就算"。求值器建立在已检查的树上：
`ident` 都有 `sym`、节点都有 `ty`，执行只是往符号的 `ct` 槽位里填值。

- **值模型**（`val.c`）：`V_INT / V_FLOAT / V_BOOL / V_STR / V_TYPE / V_AGG / V_FN / V_PTR`。
  全部只分配不回收——编译器进程很短命。`V_TYPE` 让"类型是一等值"变成字面实现。
- **折叠**发生在 `check_expr` 的包装层：类型检查完顺手算一遍，算不出来就留 `NULL`，
  后端当运行时表达式处理。所以"编译期能算就算"是默认行为，不需要 `comptime` 关键字。
- **控制流**：`if` / `while` / `for` / `switch` 都能在编译期跑，标签收割同样成立
  （`build: { …; build: t; }` 整块物化成静态数据）。循环有 100 万次上限，
  超了就当作"运行时循环"，不会把编译器挂死。
- **`@compileError` 只在真的执行到时才报**：条件是编译期常量时，`if` 只检查
  真的会走的那一支——这也让 `if (@sizeOf(T) > 8) @compileError(…)` 这种写法成立。
- **两个坑**（都已处理）：`var` 每次赋值都会改符号的 `ct`，所以标识符节点不缓存；
  循环每跑一轮要清掉上一轮在节点上留下的折叠结果（`ct_clear`）。
- **R1 是强制的**（M6）：`const` 的初值算不出来就报错。为了让这条规则不误伤，视图也得会折：
  切片值就是 `V_PTR{base=底层聚合, off=元素下标, nitems=长度}`，字符串切片直接变成
  一小段 `V_STR`；`&x` 产生指向编译期对象的指针；`.len` / `.size` / `[i]` / `.ptr`
  都按这个表示算。**读 `var`/参数/捕获**只在编译期真的在跑的时候（`ct_run` 里）才允许，
  否则 `const k = runtimeVar + 1` 会悄悄折成一个过时的数——`g_ct_exec` 就是这个闸门。

## 泛型（单态化）

泛型不是语法糖，就是**编译期函数**：`List(i32)` 是一次编译期调用，函数体返回一个类型。

实现只有一个动作：**克隆**。每个实例拿到自己那棵树，于是里面的 `struct{…}`
有独立身份（`ty_agg` 拿节点当键），`List(i32)` 和 `List(u8)` 天生是两个不同类型——
不需要任何"类型替换表"。

- 顺序必须是**先绑定参数、再解析签名**：签名里就写着 `*List(T)`，不知道 `T` 是什么就解析不出来。
- 哪些实参是泛型实参：**声明为 `type` 的参数**；如果函数返回 `type`（类型构造函数），
  它的全部参数都是泛型实参（所以 `Buffer(u8, 4)` 里的 `4` 也进类型）。
- 实例表按 `(函数节点, 实参值)` 缓存，`val_eq` 比较——同一组实参必须拿到同一个类型，
  否则类型身份（指针比较）就断了。
- 函数泛型（`push(i32, &l, 11)`）只做**签名特化**：用绑定了 `T` 的克隆算出
  `*List(i32)` / `?i32`，再按具体签名检查剩下的运行时实参；函数体留给后端生成。

## 缺返回路径（M7）

`fn f(n: i32) i32 { if (n > 0) { return 1; } }` 类型上挑不出毛病，但条件不成立时
函数掉出末尾——C 里这就是 UB。所以要做**到达性分析**（`sema.c` 的 `falls_through`）：
从函数体第一条语句走到末尾，看有没有一条路能落到 `}`。

能拦住路径的只有三样：`return` / `continue`、类型是 `noreturn` 的表达式（`@trap()`）、
退不出来的循环（只有 `while (true)`；`for` 的序列一定有限，不算）。
`if` / `switch` 要**每条出路**都拦住才算拦住，而 `switch` 还要覆盖穷尽——
枚举变体、`?T` 的 `.some`/`.none`、`!T` 的 `.ok`/`.err`、bool 的 `true`/`false`
缺一个，运行时就是一条掉出函数的路径。整数 `switch` 从不认为穷尽。

两个例外：`main` 掉出末尾等价于 `return 0`（后端本来就发）；`noreturn` 函数反而
**必须**回不来——声明了不返回却能掉出去，一样是撒谎。泛型每份单态化的身体都查，
错误只在源码位置报一次。

这条分析顺手挖出两个真 bug：`for (0..)` 曾生成 `_i0 < 0`（当零长度，循环一次都不跑，
`spin()` 直接返回 0）；`@trap()` 曾因为检查阶段折叠而误报 "reached at compile time"。

## 不可达代码与运行时格式化（M8）

**不可达代码**：和缺返回分析共用一份判定——`stmt_blocks_next()` 就是 `stmt_ends()`
再多认一个"收割"（`tag: expr;` 会跳出所在块）。反向用一遍就能查：同一条语句序列里，
前一条把控制转移走，后面就是死代码。

```
unreachable code: the `return` above never falls through
unreachable code: the `while (true)` above never falls through
unreachable code: the `switch` above never falls through
```

一个必须保留的保守：**标签块不能判成"后面到不了"**。`out: { … }` 收完值就接着往下走，
而 `falls_through` 不认收割，所以 `stmt_ends(块)` 只在块里真的 `return`/`continue` 时为真。
报一次，按 (line, col) 去重——编译期折叠会把函数体 clone 一份再检查，同一句会走两遍。

**运行时格式串**：`fmt` 是变量时走 `zx_fmt`（一趟解析、边解析边写）。
实参先统一成 `ZxFmtArg`，由 `zx_fa_i/u/f/b/s/c/p` 装进去——这样每个实参只求值一次
（直接展开成复合字面量的话，切片参数会被求值两次）。分派顺序照抄编译期的 `fmt_spec`，
两条路径行为一致：`{d}` 套在字符串上仍然打印字符串，不分叉。

```
std.println("v={}", 7);        // fmt 是常量 → 拆成 printf 串
var f: []const u8 = "v={}";
std.println(f, 7);             // fmt 是变量 → zx_fmt_out(stdout, f, …)
```

编译期能数的（占位符个数、`{N}` 越界）在 sema 里报，数不了的（fmt 是变量）在运行时
报 `zx: format error: …` 然后中止。

**`std.format(alloc, fmt, args…) → ![]u8`**：先量长度、再向分配器要内存、再写一遍，
末尾补 `\0`（`s.ptr` 可以直接当 C 串）。分配器是普通的结构体值，不是编译器魔法：

```zx
const Allocator = struct {
    alloc: fn(usize) ?*u8,
    free:  fn(*u8, usize) void,
};

const alloc = std.heap.page;
var r = std.format(alloc, "{}:{}", i, v);
if (r) s {
    std.println("{}", s);
    alloc.free(s.ptr, s.len);     // 字段里存的是函数指针，调用就是普通调用
}
```

## 指针算术与自建分配器（M9）

`p + n` 挪 **n 个元素**、`p - q` 得 **元素个数**，`(usize)p` / `(*u8)i` 是显式转换：

```zx
let p = buf[0..].ptr;        // *u8
let q = p + 3;               // 第 3 个字节
q.0 = 122;
let addr = (usize)p;         // 地址当数字
let back = (*u8)(addr + 3);  // 数字当地址
std.println("{} {}", q - p, back.0);   // 3 122
```

结果类型永远是 `*T`：`*[N]T + 1` 会把数量丢掉（数量没法跟着减）。偏移量只收
`usize`（字面量除外），`p + i32` 报错并直接给出写法 `(usize)…`。

**C 那边的坑**：`*[4]i32` 在 C 里是 `int32_t (*)[4]`，直接 `+1` 会跳 4 个元素。
后端先把操作数退成元素指针再加减，`p - q` 也一样，然后才转回目标类型。

顺带修的两个真 bug：

- **`?T` 的 tag 语义反了**。`zx_page_alloc` / `zx_format` 把 tag 当"失败标志"，
  而语言里 `?T` 的 tag 是"有值"（`!T` 的 tag 才是错误码，0 表示 ok）。内置分配器
  两头都反所以自洽，用户写的分配器一接上就永远"分配失败"。
- **自引用类型解析不出来**：`const Node = struct { next: ?*Node }` 报
  `cyclic dependency`。现在解析聚合之前先把外壳挂到符号上（`state == 3`），
  字段里遇到自己就有东西可取；值类型自引用（`next: Node`）则明确报错。
  C 那边还要求前向声明写在该结构体的字段处理之前，否则 `?*Node` 的 typedef
  会先引用还没声明的 `ZxS1`。

于是真正的 arena 跑通了（`tests/good/arena.zx`）：

```zx
const Node = struct { value: i32, next: ?*Node };
var pool: *[128]u8 = undefined;
var used: usize = 0;

const bump = fn (n: usize) ?*u8 {
    if (used + n > pool.len) { return null; }
    let p = pool[0..].ptr + used;
    used = used + n;
    return p;
};

const arena = Allocator{ .alloc = bump, .free = noop };
var s = std.format(arena, "x={} y={}", 1, 2);   // 分配就在 pool 里
let node = (*Node)bump(16);                      // 同一块缓冲也能放结构体
node.value = 7;
```

## 结构展开与三条配套检查（M10）

打印一个聚合值，以前只得到一个地址。现在 `{}` 默认就展开，`{any}` 多给两样东西：
带类型名的枚举、带引号的字符串。

```zx
const Point = struct { x: i32, y: i32 };
const Line  = struct { a: Point, b: Point, name: []const u8 };
std.println("{}", Point{ .x = 1, .y = 2 });     // Point{x: 1, y: 2}
std.println("{}", Line{ .a = ..., .name = "ab" });
// Line{a: Point{x: 1, y: 2}, b: Point{x: 3, y: 4}, name: "ab"}
std.println("{any}", Color.blue);                // Color.blue（{} 是 2）
std.println("{any}", "a\tb");                    // "a\tb"（{} 是裸的 a<TAB>b）
```

规则一张表说完（详见规格 §11.2）：结构体/元组 `名字{字段: 值}`、带标签联合体
`Shape{circle: 2.5}`、`?T` 是 `null`/`some(5)`、`!T` 是 `ok(7)`/`err(error(3))`、
切片 `[10, 20, 30]`、指针只打地址不跟进去。

**实现只有一份**：每个用到的类型生成一个描述符（`kind` + 字段名 + `offsetof`
+ 成员下标），`zx_any` 按描述符递归打印。编译期路径和运行时路径（`fmt` 是变量、
`std.format`）都调它，输出不可能分叉。字段偏移直接用 C 的 `offsetof`，
编译器不自己复算布局；描述符之间用下标引用，递归类型（`Node` 里放 `?*Node`）
不需要前向声明。

顺手补的两条检查：

- **switch 穷尽性是它自己的规则**：语句形式的 switch 漏变体也报
  `switch on Color is not exhaustive: missing .blue; add an else arm`。
  判定抽成 `switch_coverage()`，和缺返回分析共用一份，两边不会给出不同答案。
- **多序列 for 的静态长度**：`arr[0..]` 的长度来自 `*[N]T`，数组字面量来自元素个数，
  算得出来的必须相等（`for (a[0..], b[0..])` 报 `3 vs 2`）；切片参数的
  `s[0..]` 算不出来，不报。
- **动词配错实参是编译错误**：`{s}` 要切片、`{c}`/`{b}` 要整数、`{d}`/`{x}` 要
  整数或浮点。以前 `std.print("{s}", point)` 会生成 `(p).len` 这种编不过的 C。

## 泛型类型别名与结构体方法（M11）

两件事一起做，因为它们卡在同一处：**类型表达式的结果**和**给类型起名字**混在了一起。

**`const P = Pair(i32);` 现在就是类型。** 类型值只在编译期存在，C 里没有对应的东西，
所以后端遇到 `TY_TYPE` 直接不生成。名字改由构造式决定，不再由"谁先给它起别名"决定：

```zx
const IPair = Pair(i32);
const P = Pair(i32);                       // 同一个实例，同一个类型
std.println("{}", P{ .a = 3, .b = 4 });    // Pair(i32){a: 3, b: 4}
var l: List(i32, 2) = List(i32, 2){ … };   // 类型标注里直接写实例
```

**方法 = 第一个参数是接收者的普通函数**，`p.f(a)` ≡ `Type.f(p, a)`：

```zx
const Vec = struct {
    x: i32, y: i32,
    const ZERO = Vec{ .x = 0, .y = 0 };

    const add = fn (self: Self, o: Self) Self { return Self{ .x = self.x + o.x, .y = self.y + o.y }; };
    const shift = fn (self: *Self, d: i32) void { self.x = self.x + d; };
    const len2 = fn (self: *const Self) i32 { return self.x * self.x + self.y * self.y; };
};

a.add(b).neg().dot(Vec.ZERO.add(a));   // 链式就是普通调用链
Vec.dot(a, b);                         // 显式写法，同一个函数
```

- `Self` 是体里的类型别名（不是关键字）；泛型体里照样写 `Self`，不用重写 `List(T, cap)`。
- `*Self` 接收者自动补 `&`：接收者必须能取地址、且是 `var`；右值先绑到 `let`。
- 方法不能与字段重名，不能泛型（泛型写在类型上）。
- 联合体体里的 `fn` 一样写：`self.tag` 读标签、`self.成员` 读载荷。

顺手补的一条：**匿名字面量现在由期望类型驱动**（R6）——`var v: Vec = .{ .x = 1 };`、
`return .{ … };`、`f(.{ … })` 都不用再把类型名写一遍。以前 `.{…}` 只会自成一个匿名
结构体，于是 `var l: Alias = .{…}` 报 ``expected `List(i32, 2)`, found `struct{buf: …, len: comptime_int}` ``。

三个绕不开的实现细节：静态成员的字段布局要**两遍**（`const ZERO = Vec{…}` 的初值要
构造自己，字段表得先挂好）；泛型编译期实参在 C 里没有变量，值**内联成字面量**；
泛型体里的方法晚点才检查，待检查项必须**带上作用域快照**，否则 `T` / `cap` 跟着
`specialize()` 的栈帧一起没了。

## 枚举成员与标准库（M12）

三件事，共同点是"让语言能写真正像样的程序"。

**① `enum` 体是完整作用域。** 变体之外还能写 `const` / `var` / `fn`，方法表和学生
struct 共用一套：

```zx
const Color = enum {
    red, green = 5, blue,

    const FIRST = Color.red;        // 体里的常量，可以引用自己的变体
    var calls: u32 = 0;             // 静态变量：全程序一份

    const name = fn (self: Self) []const u8 { … };   // 方法
    const count = fn () u32 { return 3; };           // 关联函数：第一参数不叫 self
};

Color.red.name();      // "red"
Color.count();         // 3，编译期就能折
Color.name(Color.blue); // 显式写法，同一个函数
```

成员的 `const` **不进变体表**（`Type.statics` / `nstatics`）——`fields` 是"变体表"，
穷尽性、`{any}` 的变体名、C 枚举常量都按它遍历，掺进去就全错了（decisions #34）。

**② 标准库补到"能写程序"**。`std/std.zx` 和 `std/str.zx` 都是普通 Zx 代码：

```zx
const std = module("std");
let al = std.heap.page;

var xs = std.ArrayList(i32).init(al);      // 泛型容器，arena 友好
xs.push(7);
std.println("{}", xs.items());
xs.deinit();

if (std.str.indexOf("hello", "ell")) i { std.println("at {}", i); }
let parts = std.str.split(al, "10,20,30", ",");
std.file.write("out.txt", "hi");           // 读回来是 ?[]const u8
```

`ArrayList` 的 `reserve` 就是"向分配器要、搬、释放旧的"，`std.str` 全部是切片操作
（`trim` 返回视图不复制，`split` 各段指向原串）。这逼出了四个真 bug：`*[]T` 曾被
解析成"`*[N]T` 但 N 为空"、裸指针切片必须写上界、`undefined` 定型要走 `ctype()`、
`if (o) v if (…)` 里的第二个 `if` 曾被读成兄弟语句（捕获名的判据缺了一条）。
写标准库的过程本身就在给语言做体检——这是这次最值得记下来的一点。

**③ 选择性导入：别名就是 `const`**：

```zx
const mathx = module("lib/mathx.zx");
const square = mathx.square;             // 只拿一个名字
const Vec2   = mathx.Vec2;               // 类型也能别名
const heap   = std.heap;                 // 拿一个子命名空间
```

没有任何专门语法——`const X = <模块成员>;` 走的就是普通的 `sym_link`：不生成变量，
只把 `decl / cname / ty / ct` 拷过来（函数体还是模块那一份、模块级 `var` 还是同一个
C 全局），别名自己没有 C 存储。写在使用之前即可，和普通 `const` 一样受 R5 约束。

`sh test.sh` **122 项全过**（语法 7 + 语义 30 + 反例 49 + 运行 30 + 多文件 2 + 多文件反例 2 + 增量 2）。

## C 后端（M5）

目标很窄：**每个 `Type` 对应一个 C 类型，每个函数体直译成 C 语句，绝不引入运行时库**。
生成的 C 只有 `stdint/stdbool/stddef/stdio/stdlib/string` 六个头，剩下的全靠自己。

### 类型映射

| Zx | C |
|---|---|
| `u8/i32/f64/usize` … | `uint8_t` / `int32_t` / `double` / `size_t` |
| `[]T` | `typedef struct { T *ptr; size_t len; } ZxSlice_T;` |
| `?T` | `typedef struct { T payload; bool tag; } ZxOpt_T;` |
| `!T` | `typedef struct { T payload; uint16_t tag; } ZxErr_T;` |
| `{A, B}` | `typedef struct { A f0; B f1; } ZxTup_…;` |
| `fn(A) B` | `typedef B (*ZxFn_…)(A);` |
| `struct` / `union` | `ZxS<n>` / `ZxU<n>`（`n` 是类型声明的 AST 节点序号） |
| `*[N]T` | `T *`（N 只在类型里，运行时就是一个地址；全局数组用真正的 `T name[N]`） |

结构类型按内容拼后缀（`ZxSlice_u8`、`ZxTup_i32_u8`…），同一份 C 名只发一次。
`const` 不往 C 里带：Zx 的 `const` 是"绑定不可变"，由 sema 保证，C 那边写成 `const`
只会让 `[]const u8 → []u8` 这类转换到处掉 qualifier。

`?T` / `!T` 是**载荷在前**，所以 `&payload` 和 `&opt` 同址——这正是规格要的"取地址不会出错"。
`!T` 的 tag 用 `uint16_t` 是为了让 `@sizeOf(!u8) == 4`（`ty_align` 里同步改成 ≥2）。

### 有值的控制流：写槽位 + goto

`if` / `while` / `for` / `switch` / 带标签的块在 Zx 里都是表达式。C 没有这种表达式，
所以一律降级成：

```c
T slot = {0};
... 各分支里 slot = …; goto Lend3;
Lend3: ;
```

- 带标签的块：压一个标签帧，`tag: expr;` 处赋值后 `goto LendN`，块尾就是 `LendN:`。
- `continue` 跳到**循环体末尾**的 `LcontN:`（不是循环头），这样本轮 `defer` 一定先跑。
- `for` 的序列在进循环前求值一次（`_sq<fid>_<i>` + 上界 `_i<fid>`），多绑定逐个解构。
- `?T` / `!T` 捕获式 `if`/`while` 先取临时量，再看 tag，载荷直接 `T x = tmp.payload;`。

### defer：编译期展开

`defer` 不是运行时机制。每个 `CScope` 挂一个 defer 列表：

- 正常离开作用域 → `cscope_leave()` 逆序展开这一层；
- `return` / 收割 / `continue` → `run_defers(down_to)` 把"要离开的那几层"逆序展开，再跳。

### 全局与常量

- 编译期算得出来的（`V_AGG` 的表、标量、`?T`/`!T`）直接物化成静态初始化器；
- 算不出来的（切片视图、字段读取、控制流初值）先 `static T x = {0};`，
  再在生成的 `zx_init_globals()` 里按声明顺序赋值，`main` 第一句就调它。

### 泛型实例

`sema` 单态化时把克隆节点的 `ival` 记成"前 n 个参数是编译期的"。后端：

- 实例的 C 签名跳过前 `n` 个参数（类型参数在 C 里没有表示）；
- 调用点用 `n->ival` 决定从第几个实参开始传；
- 实例名 = `声明名_i<序号>`，第一次遇到时登记，所有函数体发完之后统一补发实例本体——
  这样实例里再调实例（嵌套单态化）也能收敛。

### 打印

格式串是编译期字符串时，逐段直接展开成 `printf`；`{b}` 走生成的 `zx_print_bin()`
（`printf` 没有二进制），聚合值走 `zx_any_stdout()`（见 M10），多段之间用逗号表达式
串起来。字符串参数用 `%.*s` + `(int)len, ptr`，长度不被 `\0` 截断。
`fmt` 是变量时整条交给 `zx_fmt_out()`——同一套语法，只是把解析推迟到运行时。

## 模块与 `pub`

**一个文件就是一个模块**，`module("path.zx")` 在编译期把这个文件读进来检查一遍，
返回它的命名空间。可见性只看一件事：**顶层声明有没有 `pub`**。

```zx
// util.zx
pub const VERSION: u32 = 3;
pub const add = fn (a: i32, b: i32) i32 { return a + b; };
const secret = fn (n: i32) i32 { return n * 2; };        // 私有
```

```zx
const util = module("util.zx");
const v = util.VERSION;                        // ok
// util.secret(1)                              // 'secret' is private to module 'util'
```

不想写 `util.` 时用 `const` 拿名字（就是普通别名，没有专门的导入语法）：

```zx
const add     = util.add;
const VERSION = util.VERSION;
const api     = util;                          // 整个模块也可以再起个名
```

别名复制的是**指向**（`sym_link` → `sym_copy`：decl / cname / ty / ct / 编译期值），
函数体只有模块那一份、模块级 `var` 还是同一个全局，别名本身不产生任何 C 存储
（后端遇到"没有存储的符号"直接跳过声明）。所以 `const square = mathx.square;`
之后 `&square` 就是 `mathx.square` 的地址，`.cap` 之类的字段访问也照样成立。

实现上有几处必须照顾到：

- **模块不共享名字**。每个模块有自己的 `Scope`，`parent` 为空；`ensure(符号)` 会临时把
  `g_scope` / `g_mod` / `g_srcpath` 切到那个符号所属的模块，这样它的初值在本模块里解析，
  报错也指回自己那个文件。
- **函数体按模块分批检查**。待检查队列里的每一项都记着所属模块，检查前把作用域换回去，
  否则模块 A 的函数体会在模块 B 的作用域里找名字。
- **C 名字要防撞车**。非入口模块的顶层名字统一加 `文件名_` 前缀（`util_add`），
  入口文件保持原名（`main` 才叫 `main`）。类型名本来就是 `ZxS<n>` 序号，天然唯一。
- **循环导入直接报错**：模块在 `loading` 期间被再次导入就是环。
- **入口文件不能被别人导入**；模块里写 `fn main` 也是错误。
- **别名要保住自己的 `pub`**。`sym_copy` 会把源符号的可见性一起抄过来，于是
  `pub const HashMap = map.HashMap;` 会变成"私有"（源在 `map` 里是 pub，但中间那层
  `std.map` 是别名，抄过来的值不可靠）。`sym_link` 先存下 `dst->is_pub`、抄完再恢复。

生成的 C 从 M13 起是 **每模块一个 `.o`**，共用一个 `prog.h`：

```
prog.h                类型定义 / static 运行时辅助 / 导出原型 / extern 全局
prog_main.c           入口模块
prog_mathx.c          其他每个模块一份
zx_init_<stem>(void)  每模块一个初始化函数，main 按依赖倒序调用
```

非入口模块里 `pub` 的 `fn` / `const` 去掉 `static`（原型进头文件），入口模块一律
`static`（`main` 除外）。`static` 仍在大量使用，但它现在只负责"模块内部私有"，
不再承担"防止撞车全靠一个文件"的职责。

## 状态

- [x] M1 lexer —— 全部示例零错误
- [x] M2 parser —— 全部示例零错误，`--ast` 可查结构
- [x] M3 类型系统 + 语义分析
- [x] M4 编译期求值 + 泛型单态化
- [x] M5 C 后端 —— 7 个示例 + 9 个正向用例全部**编成真二进制并跑出期望输出**
      （含 `program.zx` 这种带排序/状态机/递归的完整小程序）
- [x] M6 R1 强制 + 编译期视图折叠 + `pub` / 多文件模块
- [x] M7 缺返回路径分析（到达性）—— 非 void 函数每条路径都要 `return`，
      `main` 除外（掉出末尾 = `return 0`）；`noreturn` 函数不许掉出末尾。
- [x] M8 不可达代码 + 运行时格式串 + `std.format(alloc, …)`
- [x] M9 指针算术（`p ± n` / `p - q`）+ 指针 ↔ 整数转换 + 自引用类型
      —— 自建 arena 分配器跑通：同一块缓冲上既能格式化字符串也能造链表。
- [x] M10 `{any}` 结构展开（描述符驱动，编译期/运行时共用一份）
      + 独立的 switch 穷尽性报错 + 多序列 for 的静态长度检查
- [x] M11 泛型类型别名（`const P = Pair(i32)`）+ 结构体/联合体方法
      （`Self` / `*Self` / `*const Self`，`p.f(a)` ≡ `Type.f(p, a)`，链式、泛型体、自引用递归）
- [x] M12 枚举体成员（`const` / `var` / `fn`，方法与关联函数）+ `std.ArrayList` /
      `std.str` / `std.file` 标准库 —— `sh test.sh` **112 项全过**
- [x] M13 撤销 `import`（改为 `const` 别名）+ 捕获名后面只允许块或单值 +
      `std.HashMap` + **每模块一个 `.o`**（增量编译）
      —— `sh test.sh` **118 项全过**（语法 7 + 语义 29 + 反例 49 + 运行 27 +
      多文件 2 + 多文件反例 2 + 增量 2）
- [x] M14 `std.io`（句柄流）+ `std.BufReader` / `std.BufWriter`（逐行读不再整块搬）
      + `@typeInfo` 真正落地（`TypeInfo` / `TypeKind` / `@fieldType`）
      —— `sh test.sh` **122 项全过**
- [x] M15 `inline for`（编译期展开，循环变量是编译期常量，绑定可省）
      + `@field`（可读可写）/ `@fieldName` —— 泛型序列化 / 比较 / 打印自动生成
      —— `sh test.sh` **124 项全过**
- [x] M16 统一分配 API：`std.stackAlloc(T, N)` / `std.heapAlloc(alloc, T, N)`
      都返回 `*[N]T`；`undefined` 不再隐式分配；**逃逸检查**（栈帧指针不许 return）
      —— `sh test.sh` **128 项全过**
- [x] M17 `fields` 元表（`info.fields`：编译期切片，`f.ty` 直接当类型用）
      + `std.process`（args / getEnv / setEnv / exit）+ 类型点路径（`std.HashMap` 直接写）
      —— `sh test.sh` **134 项全过**
- [x] M18 `std.io.print` / `std.io.println`（句柄流格式化，与 `std.print` 共用一套语法）
- [x] M19 `if` 单语句臂（`return` / `continue` / `defer`）+ `std.ArrayList` 补全
- [x] M20 `if` 臂可以是一条表达式语句（`if (c) f(); else g();`）
- [x] M21 **数值语义钉死**：`i128`/`u128` 全链路、字面量必须装进目标类型、
      整数按位宽回绕、除零 trap、移位掩码、浮点 IEEE
      —— `sh test.sh` **153 项全过**
- [x] M22 **诊断规范化**：稳定错误码（`error[E0403]`）+ 解析器声明边界恢复
      + 打印上限 30 条 —— 一次编译尽量把错报全
- [x] M23 `extern "C"` 最小 FFI：`extern fn` 声明、原始符号名、`*u8` ↔ `char*`
      —— `sh test.sh` **155 项全过**
- [x] M24 **测试脚本换成 Zx 自己写的 runner**（`tools/runner.zx`）：
      读源码 / 切字符串 / 拼命令 / 起进程 / 比对 stdout 全在语言里完成；
      `test.sh` 退化成 60 行包装 —— `sh test.sh` **158 项全过**
- [x] M25 **自举评估**：`tests/good/selfhost.zx` 实测前端所需能力，
      结论与路线写在 `spec/bootstrap.md`；顺带挖出并修掉两个静默错误
      （联合体标签不填、标签 `if` 的收割被丢）——`sh test.sh` **158 项全过**
- [x] M26 benchmark 基线：`fib`/`mandel`/`mmat`/`str` 四个程序，宿主侧计时 + 校验和锚点
- [x] M27 `union(enum)` 语法糖：变体名即隐式枚举，布局统一为载荷在前、标签在后
- [x] M28 **`orelse` 与 `.?`**（自举缺口 G1）：`a orelse b` 惰性取默认值，
      `a.?` 空/错即 trap（与除零同一处置）；编译期能定就折
- [x] M29 **标签块**：标签写在条件 / 模式之后、块之前，判据统一为
      "名字后跟 `{` 是捕获、跟 `:` 是标签"；没有 `else` 的 `if` 也能当值
- [x] M30 **错误处理补齐**：`try e` 把 `!T` 的错误原样上抛（明式，不是异常）；
      `!void` 三条成功路径修通；错误码 0 保留给"无错误"（E0508）；
      `.size` / `.len` 与同名字段统一成"字段优先"；`main` 可以返回 `!void`，
      退出码 = 错误码 —— `sh test.sh` **185 项全过**
- [x] M31 **标签写在结构前面**：循环的标签统一到关键字前（`done: while (…)`），
      写在体前报错指路；**函数定义只剩 `const name = fn (…) … { … };`**
      （`fn name(…)` 只留给 `extern`）——`sh test.sh` **全部通过**

### M13：四件"语言设计上的回头路"

**① `import` 是多余的。** 模块本来就是一个普通值（`module("x.zx")` 返回命名空间），
而 `const` 已经能绑任何东西——那就不该再有一个专门的关键字、一套专门的语法、一张专门
的 AST 节点。全删掉，`const square = mathx.square;` 就行。删掉 `import` 顺带删掉了
`import_bind()`、`NK_IMPORT` 和 hoist 里的导入循环，语义层少了一整条路径。

**② 捕获名后面只允许块或单值。** 原来 `if (o) v if (v > 0) { … }` 能把第二个 `if`
读成兄弟语句——那不是"宽容"，是把错误静悄悄地解释成别的东西。现在判据是四分支：

| 下一个 token | 含义 |
|---|---|
| `{` | 捕获 + 块 |
| 二元运算符 | 捕获 + 以它开头的表达式（`if (m) v * 2`） |
| 值开始（数字/字符串/标识符/`null`/`undefined`） | 捕获 + 单个返回值 |
| 语句关键字（`if`/`while`/`for`/`switch`/`return`/…） | **报错**："put the statement inside a block" |

`starts_binop` 里**不能**有 `.` / `(` / `[`——它们是后缀运算符，`parse_binary_rest`
不认识，`.cap` 会漏到外层挂到 if 的结果上（`let n = if (c) 8 else self.cap * 2;`
就是这么错的）。写 `std/map.zx` 时踩到的，不是为它打的补丁。

**③ 每模块一个 `.o`。** 之前是单 translation unit，改一行全量重编。现在把一份 AST
切成 `prog.h` + `prog_<stem>.c`：

- 头文件放四类东西：类型定义、`static` 运行时辅助、导出的原型（无 `static`）、
  `extern` 全局声明，外加每个模块的 `zx_init_<stem>()` 原型。
- 初始化按模块切开，`main` 里按模块表**倒序**（被依赖的先）依次调用——顺序不能再靠
  "源文件里声明得早"这种偶然性质。
- 增量靠 mtime 比对，但比的是**生成物**（`prog_<stem>.c` / `prog.h` vs `.o`），
  不是 `.zx` 源文件——因为中间还隔了一层 `write_file_if_changed()`：内容没变就不重写，
  mtime 不动，于是"改注释"和"什么都没改"一样，零重编。（如果只比 `.zx` 的 mtime，
  改个注释就得重编它，白白丢掉一次机会。）
- `ZX_VERBOSE=1` 打印 `  cc prog_mathx.c`，能一眼看出到底重编了谁。

实测：改 `mathx.zx` 的注释 → 零重编；改它的实现 → 只编 `prog_mathx.c`。

**④ `std.HashMap`。** FNV-1a + 开放寻址（线性探测），`HashMap(i32)` 是泛型函数返回
类型的老用法。API：`get` / `getPtr`（返回 `*V`，原地改不用重插）/ `put` / `putNew` /
`contains` / `count` / `capacity` / `keyAt` / `valueAt` / `clear` / `grow`。
写它逼出两个真 bug：上面说的 `starts_binop` 收后缀，和 `val_init` 剥数组外壳后
元素类型丢失（`&.{ "alpha", … }` 每个字符串长度都成 0）。

### M14：先流起来，再照镜子

**① `std.io`：句柄。** `std.file.read` 是整块读（`fseek` → `ftell` → 一次 `fread`），
读一行也要把整个文件搬一遍。补的第一层是句柄：

```zx
const io = std.io;
if (io.openRead("log.txt")) h {          // ?usize
    var buf: *[4096]u8 = undefined;
    let n = io.read(h, buf[0..4096]);
    io.seek(h, 0);
    io.write(io.stdout(), buf[0..n]);
    io.close(h);
}
```

句柄就是装进 `usize` 的 `FILE*`，**0 = 没打开**，`close(0)` / `flush(0)` 是空操作，
调用方不用到处判空。

**② 缓冲层写在库里，不在编译器里。** 这是这一轮唯一真正的"设计选择"：

```zx
if (std.BufReader.open(al, "big.txt", 4096)) r0 {
    var r = r0;
    while (r.next()) line { std.println("{s}", line); }
    r.deinit();
}
```

`BufReader` 一次要一块、按行切、行比缓冲长就翻倍；`BufWriter` 攒满才落盘，大块直接写。
两个都是 `std.zx` 里的普通 Zx 代码——编译器只提供 syscall 那一层。理由和 §37 / `std.file`
一样：策略属于库。代价是编译器内建的命名空间只能在 `std.zx` 里按名字用，
所以它没能单独成文件。

写它逼出三个真 bug：`fill()` 把"已全部交付"的缓冲当成残段往前提（块读会越读越大）、
`if` 捕获名是只读的（`if (init()) got { got.owned = true; }` 得先 `var r = got;`）、
以及下面第 ④ 条那个。

**③ `@typeInfo` 落地。** 以前只有个返回 `bool` 的空壳。现在是真东西：

```zx
const kindName = fn (T: type) []const u8 {
    return switch (@typeInfo(T).kind) {
        .int_: "int",
        .struct_: "struct",
        .slice: "slice",
        else: "?",
    };
};
```

`TypeInfo { kind, name, bytes, align, fieldCount }` 整块是编译期值，`kind` 是
`std.TypeKind` 枚举（`.void_` / `.int_` / `.struct_` …，跟关键字撞车的加下划线）。
配套 `@hasField(T, "x")`（真查了）和新的 `@fieldType(T, "x")`。

字段叫 `bytes` 不叫 `size`：`.size` 是语言自己的编译期字段，叫 `size` 会被它截胡
（第一版就是这么错的，`@typeInfo(i32).size` 返回 48 —— `TypeInfo` 自己的大小）。

**④ 两个藏在底下的真 bug。**

- **模块作用域的快照会冻结符号表。** `schedule_body()` 给待检查的函数体存了一份作用域，
  `snap_scope()` 把 `first` 冻在那一刻；而编译器内建是 `std.zx` 解析完、所有函数体都排好队
  **之后**才并进 std 作用域的。于是 `std.zx` 自己的函数体一个内建都看不见——以前没人用到，
  一直没暴露。现在模块作用域标 `is_module`，快照直接引用同一个 Scope。
- **`@typeName(@typeOf(x))` 打印出 `type`。** 后端取的是外层 `@typeOf` 节点的类型
  （永远是 `type`），不是语义层算好的名字。

顺带给后端加了一条通用规则：**折叠过的标量按字面量发**。否则 `@typeInfo(T).align`
会被发成"读一块全零描述符的字段"，静静地得到 0——不报错，只是错。

已知待办（都是有意留到后面里程碑的）：

- **模块**：`module(...)` / `pub` / `const` 别名 / 每模块一个 `.o` 都已支持。
- **标准库**：`ArrayList` / `HashMap` / `BufReader` / `BufWriter` / 字符串 / 文件 /
  `std.process`（args / getEnv / setEnv / exit）都有了；`std.io.print` / `println`
  把格式化输出带给了所有句柄（M18）。
- **反射**：`@typeInfo` 的 kind / name / bytes / align / fieldCount / **fields 元表**
  都有了（M15 补 `inline for` / `@fieldName` / `@field`，M17 补 `fields`）。
- **分配**：M16 有了 `stackAlloc` / `heapAlloc` 与逃逸检查；逃逸已知保守面是
  "栈指针存进聚合体字段再整体取出"（decisions §44.1）。
- parser 层临时处理：`(a)(b)` 会被误判成转换（规格里没这种写法）；
  超过 u64 的字面量只带 `big_int` 标记，值留给 M4 用十进制文本重新解析。

### M15：inline for——反射的最后一块骨头

`inline for (0..N) i { … }` 的实现一句话：**语义层把循环体克隆 N 份，每份把绑定换成
编译期常量**，后端逐份发射。没有循环、没有运行时表示，`i` 是货真价实的编译期值，
能当数组长度用（`var buf: *[i + 1]u8`）。绑定可省：`inline for (0..3) { … }` 就是纯重复。

配上 `@fieldName(T, i)` 和可读可写的 `@field(v, "name")`，泛型的序列化 / 比较 / 打印
从"手写每个字段"变成两行：

```zx
const eql = fn (T: type, x: T, y: T) bool {
    const info = @typeInfo(T);
    if (info.kind != std.TypeKind.struct_) { return x == y; }
    inline for (0..info.fieldCount) i {
        const FT = @fieldType(T, @fieldName(T, i));
        if (!eql(FT, @field(x, @fieldName(T, i)), @field(y, @fieldName(T, i)))) { return false; }
    }
    return true;
};
```

`@hasField` / `@fieldType` 也放宽为接受编译期字符串——不再只认字面量，所以能吃
`@fieldName(T, i)` 的返回值。

实现细节和这轮逼出来的三个"编译期值烙在真实节点上"的真 bug（spin 被剪成裸 return、
jumps 的 `hit`/`got` 全错），见 decisions **§43**。

另：`sh test.sh` 在这台机器上偶发"stdout 不符"的假失败（发射的 C 逐字节一致、
单独重跑即过）——zig cc 的缓存按 mtime 判断新旧，同一秒内重写 C 文件会复用旧 `.o`。
测试结果可疑时先清 `/tmp/zxc_*` 再跑一遍。

### M16：分配显式化——申请的都是数组，返回的都是数组指针

旧规则里 `var buf: *[64]u8 = undefined;` 会"额外申请 64 字节"——分配点是源码里
搜不到的，`undefined` 也没表达"我要内存"。v0.10 起：

- `std.stackAlloc(T, N)`（栈帧，N 编译期已知）/ `std.heapAlloc(alloc, T, N)`
  （显式分配器，OOM abort）—— 都返回 `*[N]T`，`.len` 是编译期常量；
- `undefined` 只初始化指针槽（后端发 `T *x = 0`），要内存就显式申请；
- **逃逸检查**：指向当前栈帧的指针不许 `return` 出去——局部变量槽位、`&.{…}`、
  `stackAlloc` 的结果及其字段/下标/指针运算都拦，经局部变量中转也逃不掉；
  返回 `&self.field` 这类"取形参所指物里的东西"照常放行。

栈/堆故意不用类型区分：那是生命周期问题，需要硬保证的那一条由逃逸检查直接强制。
实现与取舍见 decisions **§44 / §44.1**。

### M17：fields 元表 + std.process

反射闭环：`info.fields` 是编译期切片 `[]std.FieldInfo`（`{ name, ty }`——字段叫
`ty`，因为 `type` 是关键字）。`inline for` 遍历后 `f.ty` 直接当类型用，递归描述、
自动生成序列化 / 比较 / 打印都不用再写 `@fieldName` + `@fieldType` 的三行舞蹈：

```zx
inline for (0..info.fields.len) i {
    const f = info.fields[i];
    std.println("  {s} : {s}", f.name, @typeName(f.ty));
}
```

TypeInfo / FieldInfo 是**纯编译期类型**：声明成变量或参数编译器直接拒收
（`var x: std.TypeInfo` 没有运行时表示，只会得到一块全零）。

`std.process` 补上进程面：`args()`（不含 argv[0]）、`getEnv(name) ?[]const u8`、
`setEnv(name, value) bool`、`exit(code) noreturn`（它之后的代码编译器拒收）。

顺手把类型语法补成点路径：`var m: std.HashMap(i32)` 现在能直接写，不用先
`const H = std.HashMap;` 起别名。

实现与取舍见 decisions **§45**。

### M18：std.io.print——句柄流的格式化输出

`std.print` 只认 stdout，`std.io` 的句柄此前只能裸 `write` 字节。现在
`std.io.print(h, fmt, args…)` / `std.io.println(h, fmt, args…)` 把整套格式串语法
（`{}` / `{d}` / `{x}` / `{b}` / `{s}` / `{c}` / `{any}` / `{N}` 索引 / `{{}}` 转义）
带给了任意句柄——文件、`stderr()`、`stdout()` 一视同仁：

```zx
if (io.openWrite("log.txt")) h {
    io.println(h, "n={} hex={x}", 42, 255);
    io.close(h);
}
io.println(io.stdout(), "to stdout {}", 7);
```

关键裁定：**句柄流一律走运行时解析器，不做编译期拆段**。`zx_fmt_out` 本来就带
`FILE*` 参数，一条调用发出去——句柄只求值一次、与 stdout 路径共用同一套 `zx_fmt`
（输出不可能分叉）、文件 I/O 面前解析开销可忽略。占位符个数、下标越界、动词配型
仍在 sema 编译期报（`check_fmt_args`）。

识别按符号不按拼写：`std.io.print` 和 `const io = std.io` 别名走的是同一份
`BI_IO_PRINT` / cname 依据（decisions §46）——第一版按拼写识别，立刻被别名写法
打脸。实现与取舍见 decisions **§46**。

### M19：if 单语句臂 + ArrayList 补全 + 一个折叠期幂等 bug

三条小改动，都来自"写一个真实 CLI"时暴露的不顺手：

**① `if (c) return x;` 不再要求花括号。** 以前只有表达式臂能省花括号，
`return` / `continue` / `defer` 必须写块——一个只有 return 的分支要写两对括号。
现在这三个关键字可以直接当臂（解析时包一层单语句块，语义零变化）。
为什么只放这三个：`if` / `while` / `for` / `switch` 跟在捕获名后面会有
"分支体 vs 兄弟语句"的真歧义，`return` / `continue` / `defer` 不会。

**② `ArrayList` 补全**：`first` / `last` / `insertAt` / `removeAt` / `reverse` /
`resize` / `swap`。仍然没有 `remove(v)`——语言没有 trait，泛型里约束不了
"元素可比较"；要按值删就自己 `indexOf` 循环 + `removeAt`（decisions §47）。

**③ 修了一个既存 bug**：`ct_call` 折叠函数调用时会 clone 已检查过的函数体
再 check 一遍，而方法调用第一次检查已经把接收者塞进了参数表（nitems 1→2），
克隆体带着改写后的形状复检就报 "method takes 1 argument(s), 2 given"。
触发条件窄（函数体里有局部结构体上的方法调用，且该函数被编译期折叠），
测试套件一直没覆盖到。修法是给 Node 加 `meth_done`，让方法改写幂等；
回归用例 `tests/good/method_fold.zx`。

配套成品是 `demo/todo.zx`（add / list / done / rm，文件持久化），
所有函数都写成 `const 名字 = fn (参数) 返回 { … };`——函数是值。
编译运行：`bin/zxc.exe demo/todo.zx -o bin/todo.exe`。

### M20：if 臂可以是一条表达式语句

M19 只放行了 `return` / `continue` / `defer` 当臂，写真实代码时最常撞的却是
另一条——每边只有一句调用，还是得写两对花括号：

```zx
if (c) pump(); else drain();        // 现在（v0.15）
if (c) x = 1; else x = 2;           // 赋值在 Zx 里是表达式，一并能用
if (n) v use(v); else report();     // 捕获 + 语句臂
```

难点不在语法，在**分号归谁**：`let x = if (c) a else b;` 的这个分号是声明语句的，
`if (c) f(); else g();` 的 then 臂分号却是臂自己的。判据只有一条——分号后面
**紧跟 `else`** 才算臂的；否则归外层，和 v0.13 行为完全一致。判据挂在 then 臂上，
值形态因此自动免疫。

一个附带修补：标签语句路径（`lbl: if (c) f(); else g();`）不经过
`parse_stmt` 末尾的分号接收，那里补了一次。

测试 `tests/good/if_arm.zx` 扩到覆盖表达式语句臂、赋值臂、else-if 链，
以及值形态不受影响的对照。实现与取舍见 decisions **§50**。

---

## M21：数值语义——趁只有一个实现，把"什么算对"写死

这些行为一旦发布就改不动了，所以先钉住再往下走（规格 §7.R10）。

**三处真 bug，都是"编译通过、运行给错答案"那一类：**

1. **超 u64 的字面量静默变 0。** 值模型只有 `uint64_t ival + neg`，超出就
   置 `big` 标志、值不可信。`let x: i128 = 170141183460469231731687303715884105727;`
   编译通过，打印出 `0`——最恶劣的那种错。补法：`Value` 加 `hi`（绝对值高 64 位），
   `val_big128` 从十进制文本切分，后端发 `((unsigned __int128)hi << 64 | lo)`。
2. **`let a: i8 = 300;` / `let b: u8 = -1;` 静默截断。** 字面量定型路径
   （`check_expr` 的 `NK_INT` 分支）根本没有范围检查，`coerce_node` 那条路
   （结构体字段、函数实参）也没有。两处都补上，并给 `INT_MIN` 留例外：
   `-170141183460469231731687303715884105728` 里的正数字面量是 max+1，
   只在**紧跟一元 `-`** 时放行（静态上下文标志，检查是单线程递归的）。
3. **无类型标注的大字面量打印按 64 位截断。**
   `println("{}", 170141183460469231731687303715884105727)` 的类型是
   `comptime_int`，打印快速路径只认 `TY_INT && bits==128`，于是走 `%lld`，
   低 64 位全 1 → 输出 `-1`。判断条件改成"或 comptime_int 且值确实超出 64 位"，
   小字面量仍走 `%lld`，`{:x}` / `{:b}` 的原有行为不变。

**语义裁定（写进规格的）：**

- 整数 `+ - * <<` 与复合赋值**按目标类型位宽回绕**，后端统一发成"先转无符号
  算再转回"——C 的有符号溢出是 UB，直接发 `a + b` 在 `-O3` 下会被当成"不会溢出"
  从而改写控制流。这是"在 C 之上做语言"最容易踩的坑。
- 除零：编译期可见就报错；运行时发 `zx_div0()`（stderr 说明 + abort），
  不用 C 的 `/`（那也是 UB）。`MIN / -1` 回绕成 `MIN`，不 trap。
- 移位量按位宽掩码（`n & (bits-1)`）；浮点走 IEEE（除零得 `inf`，`0.0/0.0` 得 `nan`）。
- **编译期与运行时同宽同回绕**：`let x: i8 = 100 * 2;` 是 `-56`。要抓溢出请用
  更宽的类型——两个阶段给出两个答案比不能报错更糟。

**顺带修掉测试长期"随机假失败"的根因**：不是测试脚本，是驱动的 `.o` 复用判据
`o < fmtime(c)` 只有**秒级**精度，同一秒内重写 `.c` 分不出新旧，上一轮的 `.o`
被当新鲜的复用。改成"内容变了就删 `.o`"。连跑 6 次全稳，之前大约每三次坏一次。

## M22：诊断——一次能报几个错，决定编译器好不好用

- **稳定错误码**：`file.zx:12:9: error[E0403]: cannot assign to 'n': ...`。
  码段见规格 §17：E01xx 词法、E02xx 语法、E03xx 类型、E04xx 名字、E05xx 规则、
  E06xx 模块、E07xx 泛型、E08xx 内建/格式。码表在 `zxc.c` 里一处分派：
  消息片段 → 码，命中不了的按阶段给默认码；解析器自己定 E0201/E0202
  （语义层也有 `expected X, found Y` 形状的消息，那是 E0301，不能让码表猜）。
- **解析器在声明边界恢复**：一个 `fn` 里出了语法错，跳到下一个 `fn`/`const`/
  `struct`… 继续解析。之前 `fn c() void` 漏个花括号，后面所有函数一起被吞，
  用户只能一轮修一个错。
- **上限 30 条**（继续计数），结尾汇总；超限说明 `(first 30 shown)`。
  级联刷屏会把根因埋掉，第一个错通常就是根因。
- 测试脚本支持 `// expect-code: E0302 E0403` 与 `// expect-count: 3`：
  断言码，不断言措辞——措辞会为了更准确而改，码不会。

## M23：extern "C"——标准库从"自己写"变成"能借"

```zx
extern fn puts(s: *const u8) i32;
extern fn malloc(n: usize) *u8;

const main = fn () void {
    puts("hello from libc".ptr);
    let p = malloc(8);
    free(p);
};
```

语义层只认签名，后端只发一条 `extern` 原型，不生成函数体。三个坑都是"看着能做、
一编就炸"的那种：

1. **名字不能转义。** 后端给 C 关键字/头文件公共名加下划线（`puts` → `puts_`）
   是为了避免和 include 进来的 stdio 撞车，但 extern 要的正是那个外部符号。
   现在 extern 走独立的名字发射路径，原样发。
2. **标准头已经声明过的名字不能再声明。** C 里 `void* malloc(size_t)` 和我们
   按 Zx 类型发的 `char* malloc(uint64_t)` 是冲突类型。这些名字直接跳过原型，
   用头里的声明。
3. **`*u8` 在边界要写成 `char*`。** Zx 没有 `char`，但 libc 的字符串函数收
   `const char*`。只在 extern 原型里映射，调用点显式转型，`-Wpointer-sign` 清零。

能调 libc 之后，写长命程序不必再自己搓 arena：`malloc`/`free` 就是现成的。
不做的部分（变参、C 宏、回调约定）写在规格 §18——"不能做什么"和"能做什么"
一样是规格的一部分。测试 `tests/good/ffi.zx` 走 `puts` + `strlen` + `malloc`
往返，`sh test.sh` **155 项全过**。

## M24：测试脚本也用 Zx 写——顺便给语言做一次真实压力测试

`test.sh` 原本是 200 行 shell：遍历文件、判正反例、比对 `// expect`、跑二进制、
比 stdout。这类活 shell 能做，但每加一条断言就要动一次引号转义，而且 Windows 上
`sh`、路径、编码三件事轮流出问题。换成 `tools/runner.zx`（约 270 行）之后，
测试脚本本身也进了类型检查——写错一个字编不过，而不是跑到一半才炸。

**这不是"为了自举而自举"**：runner 恰好覆盖了语言最常被使用的那条路，
每一处能力缺口都会在这里当场暴露，比再写十个 hello world 有用：

| 需要的能力 | runner 里的用法 |
|---|---|
| 读文件 | `std.file.read` 读源码、读捕获的 stdout |
| 切字符串 | `std.str.split` 按行切开找指令注释 |
| 拼字符串 | `std.str.concat` + `ArrayList` 拼命令、拼期望输出 |
| 起进程 | `extern fn system` 调 `cmd`，把 stdout/stderr 收进临时文件 |
| 格式化 | `std.println("{}", ...)` 打报告 |
| 显式转换 | `@intCast(usize, n)` |

**跑出来的三个真 bug**（都是"写 hello world 永远碰不到"的那种）：

1. **`stackAlloc` 和 `heapAlloc` 不是一个东西。** `Cmd.init()` 里用
   `stackAlloc` 申请 4KB 缓冲再返回指针——缓冲分配在 `init()` 的栈帧里，
   返回即悬空，命令串变成垃圾。Zx 没有借用检查，所以它照样编过；
   现在有 149 个用例在替它兜底。
2. **Windows 上 `system()` 的引号规则。** `system()` 实际是 `cmd /c <整串>`，
   而 cmd 有个古怪规则：命令以 `"` 开头、且引号总数不是「恰好两个」时，
   它剥掉最外层的一对。于是 `"E:\..\a.exe" > "E:\..\out" 2>&1` 被糟蹋成
   `E:\..\a.exe" > "E:\..\out 2>&1`，报「文件名、目录名或卷标语法不正确」。
   对策是自己先包一层引号，让它剥掉的正好是那一层。
3. **诊断会重复。** 加上 `// expect-count:` 之后立刻抓到：`1 / 0` 报了两遍——
   语义检查对同一表达式求值两次（定型一次、折叠一次），而求值带副作用。
   修在诊断的唯一出口：按「位置 + 码 + 文字」去重，用户看到的条数
   永远等于摘要里的数字。

**顺带修正的两处测试布局**：`tests/bad/priv-util.zx` 是 `private.zx` 的配套模块、
不是独立反例，挪进 `tests/bad/priv/`，让 `tests/bad/*.zx` 通配只命中根文件；
反例的三条断言（`// expect:` / `// expect-code:` / `// expect-count:`）
一次全查、全报，不用修一个才看见下一个。

`test.sh` 现在只剩 60 行：编 runner、把文件清单交出去、跑"增量编译"那一项
（它要看编译器的 `ZX_VERBOSE` 输出，属驱动层行为，不适合塞进 runner）。
`sh test.sh` **158 项全过**，连跑多次稳定。

## M25：自举评估——先让语言挨一遍真打

"自举"在这里有个具体的意思：**把前端（词法 + 语法 + AST + 诊断，约 2400 行 C）
换成 Zx 写的**。后端发射 C 这件事永远该由 C 写，语义层是第二阶段。
完整评估在 `spec/bootstrap.md`，这里只放结论。

**表达力够用，有实测。** `tests/good/selfhost.zx` 是一个真跑起来的小型词法器 +
递归下降求值器：递归类型（`?*Node`）、带标签联合体、方法互递归、按字节扫字符串、
`HashMap` 关键字表、`ArrayList(*Node)`、20 层嵌套括号求值——全部通过。
没有一条是"Zx 根本写不出来"。

**两个缺口值得改语言**：

1. **没有 `orelse` / `.?`**。每访问一个可选子节点都要写一个 helper 函数
   （因为它不能出现在表达式位置），AST 遍历代码会翻倍。这是唯一真正疼的地方。
2. **char 字面量不能做 switch 模式**。词法主循环只能写
   `switch ((i32)c) { 48, 49, … }`，能跑，但难看。

**一个缺口不改**：没有 `break`。前端 37 处 `break` 换成
`label: while (…) { … label: v; … }` 就行——而且收割能带值出去，
比 `break` 再补一条赋值更短。

**这轮最实在的产出不是结论，是两个 bug。** 都是按"编译器的形状"写代码才会踩到、
编译不报错、答案错的类型：

- 带标签联合体 `U{ .b = 7 }` 只写载荷不写 `.tag`，C 的指定初始化把标签零填成
  第一个成员——`switch (u.tag)` 永远走第一个分支。token/AST 分派全靠它。
- 规格 §8.1 标为**推荐**的 `s: if (x > 0) { s: 1; } else { s: 0; }`，
  生成的 C 是**两个空臂**加 `return 0`：后端漏给 `if` 自己那层建标签作用域，
  而找不到目标的收割被静默丢弃。Zx 删掉了 `break`，收割是唯一的值出口——
  出口坏了等于地基裂了。

现在找不到收割目标会报内部错误，不再静默丢。回归：`union-tag.zx`、`label-if.zx`。

**顺序建议：先补语言缺口，再搬前端。** `orelse` 和 char 模式的收益是全局的
（所有 Zx 代码都变短、变能读），自举只是其中一项；反过来先搬 3000 行、
再发现"每天都想写 `orelse`"，成本就高得多。

## M26：benchmark 基线

性能工作没有标尺就是盲走。`bench/` 四个负载覆盖四种典型压力：
`fib`（调用开销 + i64）、`mandel`（f64 内层循环 + 分支预测）、
`mmat`（256×256 矩阵乘，内存带宽）、`str`（格式化 + 分配器）。
宿主侧计时、每程序 3 次取最好；**每个负载都打印校验和**——那既是正确性
锚点（跨后端必须逐字节一致），也防止优化器把计算折叠掉。

基线（zig cc -O1）：fib 83ms / mandel 196ms / mmat 149ms / str 79ms，
记录在 `bench/BASELINE.md`。以后动后端先跑 `sh bench/run.sh`，校验和不齐
或耗时回退超过 5% 就地停下。

## M27：union(enum)——变体名即枚举

联合体终于有了 Zig 式写法，不用再手写标签枚举：

```zx
const Shape = union(enum) {
    circle: f64,
    rect:   { f64, f64 },
};

let s = Shape{ .circle = 2.0 };
switch (s.tag) {
    .circle: std.println("r={}", s.circle),
    .rect:   std.println("area={}", s.rect.0 * s.rect.1),
}
```

- 变体名即隐式枚举，标签值按声明顺序 `0..n-1`；`union(enum(u8))` 指定
  标签底层宽度。
- **布局统一为载荷在前、标签在后**——和 `?T` 的 `{值, ok}` 同向，
  §5 的承诺现在覆盖所有带标签联合体。访问器都是编译器生成的，
  现有用例输出逐字节不变。
- 实现是纯糖：sema 合成隐式枚举、变体打 tag，之后全走显式标签联合体
  已有的机制，后端零感知（decisions §56）。

测试：`tests/good/union_enum.zx`（三种载荷形态 + 方法 + u8 标签布局 +
指针遍历）。`switch (s)` 直接分派（带载荷捕获）留作独立一轮。

## M28：`orelse` 与 `.?`——给可选值两个出口

自举评估把"没有 `.?` / `orelse`"排在缺口第一位。理由不是"Zig 有所以我们也要有"，
而是**写 AST 遍历时每个子节点都是 `?*Node`**：

```zx
// 以前：每个子节点都要 if 捕获 + 标签块，三行变十行
const lhs = fn (nd: *Node) *Node {
    return r: if (nd.lhs) l { r: l; } else { r: nd; };
};

// 现在：必须有值就 `.?`，给默认值就 `orelse`
const lhs = fn (nd: *Node) *Node { return nd.lhs.?; };
const lhs_or = fn (nd: *Node, dflt: *Node) *Node { return nd.lhs orelse dflt; };
```

两条规则分开是有意的：`orelse` 管"没有就用这个"，`.?` 管"这里不可能没有"。混成
一条，读代码的人就分不清"作者想过没有值的情况"还是"作者赌它不会空"。

**`orelse` 是关键字而不是运算符符号。** 它只有一个优先级——高于赋值、低于 `||`，
右结合。写成运算符就得在优先级表里给它找个位置，而它的语义是"取个值回来"，
和布尔/算术都不该比黏度。解析上它占二元运算的位置，但造自己的节点（要短路，
不能当普通二元运算发）。

**右边必须惰性。** `x orelse expensive()` 在 `x` 有值时一次也不该算，所以后端
不能摊成 C 三元式（那会把两边都算掉）——左边先落进临时量，再写
`(tmp.tag ? tmp.payload : (右边))`。

**`.?` 是"我知道它有值"，不是可恢复错误。** Zx 没有异常，错误值就在返回值里：
想处理就用 `if` 捕获（§8.1）或 `orelse`（§5.1），三条路各管一段，不需要第四种。
空值解包按整数除零那一套处置——`fflush(stdout)` → stderr 一行
`zx: unwrap of a null optional at 文件:行` → `abort()`。

**v0.21 补记：`null` 不许当值用。** 这一轮最初为了让 `null orelse x`、`null.?`
"能跑"，前后打了两个补丁——根子是同一个错误：**把 `null` 当成了"空的可选值"**。
它其实只是给 `?T` 填空的字面量，没有自己的类型，离开"有 `?T` 落点"的位置就该报错：

```zx
let a: ?i32 = null;                     // 对：落点是 ?i32
let b = null;                           // 错：推不出类型
null.?;                                 // 错：没有 payload 可解
null orelse 42;                         // 错：没有"左边是不是空的"可问
const Z: ?i32 = null; const V = Z.?;    // 错：编译期就空了（E0506）
var z: ?i32 = null; z.?;                // 对：运行期才 trap
```

想表达"没有就用默认值"，先给"没有"一个名字：`const NONE: ?i32 = null;` 然后
`NONE orelse 42`。宁可多写一行声明，也不让 `null` 变成"哪儿都能凑合"的值——
理由在 decisions §59。编译期折叠照旧：`NONE orelse 42` 折成 `42`，
`SomeConst orelse 0` 也折得出来；而"编译期就空"的 `.?' 现在报 **E0506**，
和编译期除零（E0505）是对称的一对。

测试：`tests/good/optional.zx`（基本语义 / 右结合与优先级 / 惰性 / 链式解包 /
const 折叠）、`tests/bad/unwrap-nonoptional.zx`、`tests/bad/orelse-nonoptional.zx`、
`tests/bad/unwrap-null.zx`、`tests/bad/orelse-null.zx`、
`tests/bad/unwrap-comptime-null.zx`；`tests/good/if_arm.zx` 补了值位置的
`else if` 链。`sh test.sh` **167 项全过**。

## M28：switch 捕获——模式说是什么，捕获直接拿

M27 的 `union(enum)` 落地后，最扎眼的就是这个组合：`switch (s.tag)` 匹配了
变体，载荷还得手写 `s.circle`。现在：

```zx
switch (s) {
    .circle: r  { return r * 3.14159 * r; },   // r = 载荷
    .rect:   wh { return wh.0 * wh.1; },       // 每个臂捕获各自的类型
}
```

**一条规则覆盖所有主语：被捕获的就是"匹配到的东西"。** 联合体臂捕获变体
载荷；`?T` 的 `.some` 捕获载荷（`.none` 没有载荷，捕获直接报错）；`!T` 的
`.ok` 捕获载荷、`.err` 捕获错误值；枚举 / 整数 / bool 臂捕获主语值本身。
单值臂（`.red: "red"`）和无捕获块臂照旧。

判别凭据是**名字后面紧跟 `{`**：`.circle: r { … }` 是捕获，`.circle: a: { … }`
是臂标签（收割交值用；v0.22 起标签挪到冒号后，与捕获同一位置）。捕获是只读
绑定，作用域限臂，只允许单模式臂，
`else` 臂没有东西可捕。捕获只配块臂——`cap 值` 和裸表达式臂语法上无法区分。

实现复用了 `if` 捕获的整套机制（`SK_CAPTURE` 作用域符号 + 臂内一行声明）。
**顺带修掉一个潜伏 bug**：`?T` / `!T` / 带标签联合体作 switch 主语时后端
会发 `switch (struct)`（非法 C）——sema 早就放行了这类主语，只是没有测试
覆盖到后端。现在主语一律先落临时量、头部用判别值。

测试：`tests/good/switch_cap.zx`（五种主语 + 单值臂 + 无捕获臂）+
`tests/bad/switch-cap-{none,else,multi}.zx`。实现与取舍见 decisions **§58**。

## M29：标签是块的名字——写在条件 / 模式之后

> **v0.24 修订（M31）**：循环的标签已挪到关键字**前面**——`done: while (…)`、
> `hit: for (…) x`。块仍是"标签紧贴块"。下面的 while / for 示例已按新写法更新。

标签原先只有前缀写法（`tag: if (…) { … }`、`tag: for (…) x { … }`）。switch
臂里另有一个"臂标签"，位置在模式与冒号之间（`.circle a: { … }`）。两套位置，
读的时候要先分辨"这个 `:` 是分隔符还是块名"，写的时候也容易放错。

现在收成一条：**块臂头部的名字，后跟 `{` 是捕获、后跟 `:` 是标签**，两者可
同时出现（捕获在前）。四处语法完全一样：

```zx
switch (s) {
    .circle: r  a: { a: r * 2.0; },        // 捕获 r，块名 a
    .rect:   wh a: { a: wh.0 * wh.1; },
}

if (o) v t: { t: v; } else { t: 0; }

const w = done: while (true) {            // 没有 break：收割就是出口
    n = n + 1;
    if (n == 7) { done: n * 10; }
};

const i = hit: for (0..8) k {
    if (k == 3) { hit: (i32)k; }
} else -1;
```

**循环的标签写在关键字前，块的标签紧贴块**（v0.24 定稿）。收割要结束的是
循环——标签若贴在体前，读起来像"体的名字"，跳到体末就会进下一轮；块没有这个
问题，`if` 的标签贴在臂的块上，语义就是"这一臂的值"。

**顺带定下：没有 `else` 的 `if` 也能当值**（`const i = if (true) tag: { tag: 5; };`）。
类型由 then 臂给出；条件为假时没有臂执行，值就是零值——和"块没被收割就是 `{}`"
同一条规则。条件是编译期已知为假时类型被剪成 `void`，这时才算没有值；后端也
不再发 `void j = {0};`（非法 C），只发初始化器的语句部分。

**破坏性**：`.circle a: { … }` 现在报"the arm label goes after the `:`"，指出
新位置。旧位置与新规则只差一格，两种都收会让"标签属于谁"重新变模糊。

实现是解析器四处各加几行（同一个判据）+ `check_if` 无 else 时的类型 + 后端
void 声明的分支；标签作用域本身仍由 `if` / 循环 / 块自己的 `label` 承担。

测试：`tests/good/label_block.zx`（四处语法 + 捕获组合 + 值形式）+
`tests/bad/arm-label-pos.zx`、`tests/bad/label-no-block.zx`。取舍见 decisions **§60**。

## M30：错误处理——四种出口，各管一段

`!T` 从 M21 就"能用"，但真正拿它写超过两层调用链就会发现：**能表达，不好用**。
三层传播原来要七行，中间还夹一层 if：

```zx
const read_head = fn (p: []const u8) !i32 {
    let r = open_file(p);
    if (r) fd {
        if (fd < 0) { return (error)Err.read; }
        return fd + 1;
    } else e {
        return e;                    // 把捕获到的错误码重新包回去
    }
};
```

现在：

```zx
const read_head = fn (p: []const u8) !i32 {
    let fd = try open_file(p);
    if (fd < 0) { return (error)Err.read; }
    return fd + 1;
};
```

### `try` 不是异常

规格 §5.1 写着"不做 `try`/`catch` 那套"。那句话反对的是**异常**——隐式跳转、
栈展开、看不见的退出点。`try` 一样都不沾：

- 调用点写着 `try`，读者一眼看到"这里可能退出"，和 `return` 一样显式
- 没有栈展开，没有 catch 块，没有新的控制流构造
- 返回类型不变，还是那个 `!T`，错误值始终在返回值里

它是 `if (e) v { v } else err { return err; }` 的**压缩**，不是新机制。选前缀不选
Rust 的 `e?`，是因为 `?` 已经被 `.?` 占了——`e?` 和 `.?` 挨在一起，读的人分不清
哪个是 trap、哪个是传播。

### 四种出口

| 写法 | 用在 | 出错时 |
|---|---|---|
| `try e` | 我在中间层，说了不算 | 带着同一个码 `return` |
| `if (u) v {}` | 就地处理这一处 | `else e` 拿到错误码 |
| `a orelse b` | 有合理的默认值 | 取 `b` |
| `a.?` | 这里不该出错 | 打一行说明后 `abort()` |

四条不是功能重叠，是**四种意图**。只留 `try` 和 `.?`，代码里就分不出"作者想过
容错"和"作者赌它不出错"。

三条边界：`?T` 不能 `try`（空不是错误，没有码可带）；所在函数必须能返回错误；
`defer` 体里不能 `try`（它自己再提前返回会覆盖正在返回的值）。

### 顺手挖出来的四个静默错误

加语法没花多少时间，时间都花在这三处：

1. **`!void` 根本无法正常返回**。`fn f() !void { }` 报"能走到末尾却没返回值"、
   `return;` 报"这个 return 没有值"、`return {};` 报"expected `!void`"。三条路全
   堵死——而它是最常见的错误签名。根子是缺返回检查只给 `TY_VOID` 开口子，加上
   后端把 `!void` 当普通 `!T` 发，生成 `typedef struct { void payload; … }`（C 不允许）。
2. **错误码 0 静默吞错**。`error ≡ u16`，0 表示"无错误"，而枚举首成员默认就是 0：

   ```zx
   const Err = enum { open, read };
   return (error)Err.open;      // tag = 0 → 被读成"成功"
   ```

   调用方拿到的是"成功 + 一段未初始化的 payload"，拿着垃圾继续跑。现在编译期
   能算出的 0 一律报 **E0508**（字面量和枚举变体都查）。**规格自带的
   `spec/examples/errors.zx` 就踩了这个坑**——它把 `ok = 0` 放进了错误码枚举。
3. **`!void` 的载荷被当值捕获**。`if (x) v { … }` 生成 `void v = _c0.payload;`，
   C 直接编不过。现在语义层说清楚："这个联合体没有载荷可捕获，成功分支写
   `if (x) { … }`"。switch 的 `.ok` 臂同理。
4. **`main() !void` 生成非法 C**。收尾时才发现：`main` 的 C 签名是 `int main(...)`，
   后端对返回语句无条件加 `(int)` 强转，于是 `return (error)Err.boom;` 变成
   `return (int)((ZxErr_void){ .tag = 7 });`——结构体转 int，C 直接拒绝。也就是
   说**最想用的那种入口签名根本编不出来**，而 `try` 在 `main` 里的提前返回走的
   是另一条路，同样非法。

   现在 `main` 的返回值折一次：`!T` 用**标签本身**当退出码（0 = 成功，正好是
   shell 的约定），`?T` 有值 0、空 1。折完之后 `try` 能一路传到进程边界，"四种
   出口"才补上了最后收下的那个人：

   ```zx
   const main = fn () !void {
       try run();               // 出错 → 退出码就是那个错误码
   };
   ```

**这一条还牵出了测试基建的问题**：runner 用 `contains(src, "fn main(")` 判断
"这个文件有入口、值得编出来跑一遍"。而入口写法已经改成 `const main = fn (…)`，
于是 48 个文件一个都不匹配，47 个 `// expect-output` 用例**静默退化成纯类型
检查**——套件照样报绿，运行期行为没人验。修完判断、并加上 `// expect-exit:`
断言退出码之后，用例数从 135 涨到 **185**——48 条是被跳过的运行用例重新跑起来，2 条是新加的退出码用例。
教训：**测试静默跳过比测试失败更危险**，它把"没验证"伪装成"验证过了"。

### `.size` 与同名字段：三处判断不一致

写测试用例时给结构体起了个 `size` 字段，输出 `100 8`——8 是 `sizeof(Head)`。
查下去是**三处各判各的**：类型检查认为它是 `i32` 字段，常量折叠和代码生成都
无条件当"字节数"。于是 `h.size` 静默变成 `sizeof(h)`。

规格原来写"`len`/`size`/`ptr` 不能作字段名"，但 `std.BufWriter` 自己就有合理的
`len` 字段。所以改成统一规则：**聚合体自己有同名字段时，字段优先**，要类型大小写
`@sizeOf(T)`。三处一起改，规格 §3.3 同步。

把"实现不一致"改成"语言限制"是条捷径，但那条限制没有收益——字段优先是唯一
没有歧义的读法。

测试：`tests/good/errcatch.zx`（三层传播 / defer 交互 / `!void` 三种成功 /
错误码比较 / `try` 在表达式中间 / payload 是结构体）、
`tests/good/main-exit.zx`（`main() !void` + `try` 传播 + 退出码 = 错误码）、
`tests/bad/err-code-zero.zx`、`err-code-zero-enum.zx`、`try-nonerror.zx`、
`try-outside.zx`、`errvoid-capture.zx`、`try-in-defer.zx`。
取舍见 decisions **§61**（四种出口）与 **§62**（字段优先）。

## M31：标签写在结构前面；函数只剩 const 一种定义

M29 定了"标签是块的名字"。这条规则套到循环上就出问题：`while (true) done: { … }`
把 `done:` 放在**体**前面，读起来是"体叫 done"，而收割 `done: v;` 实际要做的是
**结束整个循环**——如果它只跳到体末，下一轮就又进来了。同一个位置，两种读法，
一种错。M29 的实现把标签挂在循环节点上（语义是对的），但写法仍在体前，读的人
没法从代码看出这一点。

所以循环的标签挪到关键字前，块保持"标签紧贴块"：

```zx
const w = done: while (true) {           // 标签是循环的名字
    n = n + 1;
    if (n == 7) { done: n * 10; }        // 跳出循环，值 = 70
};

const i = hit: for (0..8) k {
    if (k == 3) { hit: (i32)k; }
} else -1;

if (o) v t: { t: v; } else { t: 0; }     // 块的标签仍紧贴块
```

判据收成一句：**标签写在被它离开的结构前面**。块 → 紧贴块；循环 → 在关键字前。
写在循环体前报 `the label of a loop goes before the keyword` 并给出正确写法，
不静默兼容——两种位置都对，规则就等于没有。

### 函数定义只剩一种

`const name = fn (…) R { … };` 是唯一写法。`fn name(…)` 只留给没有函数体的
`extern` 声明。理由：函数是值，名字属于**绑定**；两套写法让"函数名"和"一个值
绑到名字上"变成两个概念，而它们的行为完全一样（自递归、取地址、传参都没差别）。
`fn` 后面永远直接是参数表，解析器少一个分支，读的人也少一次判断。

方法同理，体里写 `const add = fn (self: Self, …) … { … };`——解析成同一个
带名字的 NK_FN，方法表、调用改写、后端都不用动。

### 顺带修掉的一个真 bug

跨模块的**常量折叠**（`ct_call`）在调用方的作用域里重新检查函数体：`a.api(3)`
按主模块的名字表解释 `api` 的体，于是它引用的私有辅助函数全变成 unknown
identifier。泛型实例化早就切了模块上下文，`ct_call` 漏了同一手。修法照抄：
按 `callee->fn->sym->mod` 切 `g_mod` / `g_scope` / `g_srcpath`，每条 return
路径都恢复。这个 bug 与新写法无关，只是被大规模改写顺手撞上了。

### 迁移

339 处定义（`std/`、`tests/`、`tools/`、`demo/`、`spec/examples/`）机械改写，
文档里 52 个代码块同步。测试：`tests/good/label_block.zx` 换成前缀写法，
新增 `tests/bad/label-after-cond.zx`、`fn-name.zx`、`fn-name-method.zx`。
取舍见 decisions **§63**。
