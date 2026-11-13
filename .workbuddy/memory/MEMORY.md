# zx 项目长期约定

## 元规则
- 能力线用 **git commit + 守门用例** 保存；实现被删会连挂，**不许删用例来变绿**（用户明确要求）。定位历史：`git log --oneline --grep=inlayHint`。
- 改动完成即提交推送；精确指定路径，不捎带并行任务的工作区改动。每轮只测本轮结果，全部做完再统一全量。用户偏好实际运行验证交付。
- 控制台输出（进度/诊断/help/usage/explain）**只用 ASCII**；中文只进注释与写进文件的模板。GBK 控制台会把 UTF-8 的 `—`/`…` 渲成乱码。
- 行尾：`src/main.c|parse.c|sema.c`、`tools/zxls.zig`、`tools/lsptest.py`、`std/std.zx` 是 **CRLF**；`src/backend.c`、`src/errors.c`、`std/task.zx`、`docs/`、`spec/`、`README.md`、memory 是 **LF**。Python 文本读写会把 CRLF 变 LF → 一律 bytes 二进制读写，改完用 `git diff --numstat` 确认没炸成全文件。
- `// expect-output` 两种写法 runner 都认（裸标记 + 连续 `// ` 行，或 `// expect-output:一行`）。runner **合并 stdout/stderr** 比对，期望里含**相对路径+行号** → 编译必须传相对路径。
- 改 `tools/zxls.zig` 后必须确认 `bin/zxls.exe` 真被替换（Zed 锁 exe 会让 build skip：`mv bin/zxls.exe bin/zxls-bak.exe` 绕锁再 `-femit-bin=bin/zxls.exe`）。替换后用户要重启 Zed 重新协商 capabilities。验证：`ZXLS_BIN=<exe> python tools/lsptest.py`（独立目录、旁边要有 std/，指 .tmp 会假 FAIL）；编译器改动跑 `sh test.sh`。

## 语言事实（写文档/示例前必看）
- **无 trait / interface / 重载 / 子类型**；函数指针可作 struct 字段（`cb: fn (i32) i32`），但调用点全靠 `(T)bits` 强转，不是多态。
- 内建七个：`@type @memcpy @memmove @memset @link @compileError @trap`。`@type(x)` 是唯一反射；老拼写报 **E0211**。
- **转换只有 `(T)val`**；无 GC/析构/RAII/生命周期/异常（只有 `!T`）/协程/宏/运算符重载/`private`（可见性只有 `pub`）/默认参数/变参。
- 函数定义只有 `const F = fn(…) ret {…};` —— **没有 `pub fn`**，pub 是前缀修饰符。
- **容器方法不能泛型（E0701）**；容器字段/方法/静态成员默认私有（只在声明文件内可见），`pub` 才对外，跨文件访问私有成员报 **E0404**；enum / union(enum) 变体恒公开。锚点 `sema.c member_hidden/deny_member/stamp_mod`；自举要先重建 zxfe。
- **Zx 没有数组值类型**：`*[N]T` 就是指针（`TY_PTR`+`has_len`，`ty_size`→8），长度只写在类型里做界检查与 `.len`，C 后端发 `T *`；`[]T` 是 `{ptr,len}` 16 字节。存储用 `std.stackAlloc(T,N)` / `std.heapAlloc(alloc,T,N)`；`&.{…}` 是"匿名存储+指针"的糖。**表述红线**：`&xs` 是取指针变量的地址，文档不许写"数组的地址"。守门 `tests/good/addr-array.zx`。
- `std.process.args()` 不含 argv[0]；`orelse return …` 不合法；没有 `anytype`；switch 模式无区间；`if (c) { x } else { y }` 块臂不给值（要 `lbl: x;` 收割）。
- **`fn (N: u64) type` 是泛型**：返回 `type` 的函数即类型构造函数，值参数可当数组长度（`*[N]u64`、`*[N + 1]u64`）。`fn_is_generic` 曾只认 `T: type`，是会复发的坑。
- `ct_clear_stmts` 只擦语句子树：结构体字面量里折值字段访问（`N{.a=@type(T).size}`）的 ct 不能擦，否则后端发 `0.size`。

## 句柄检查只在编译期（§135，2026-11-13 用户裁定）
- **运行时 side table 已撤掉**，`std.slot` 删除（用户：抹源靠它兜底是假安全）。`get` 能证明 LIVE 就改写成私有 `getUnchecked`（dev/release 同形态），证明不了报 **E0511**；二次释放 E0509；表已关闭 E0510。
- opt-in 契约（没有 std 库后唯一入口）：容器有 `create` + `get` + **私有** `getUnchecked`；`get` 函数体不进生成代码；**方法名才是契约**（编译器不认识 Handle 类型）。抹源（cast/存字段/跨函数/控制流）→ E0511，无运行时兜底。

## Allocator 与性能（§138，2026-11-13 用户）
- `Allocator = { ctx: ?*u8, alloc(ctx,n) ?*u8, free(ctx,p,n) }`；ctx 让 arena/pool/debug_alloc 把状态塞进去。
- **去虚拟化**（sema `devirt_allocator_call`）：能证明目标就把 `a.alloc(a.ctx,n)` 改成直调。证明链只有四条：`std.heap.page`、`Allocator{…}` 字面量、`if (x) v {…}` 捕获（`Symbol.cap_src`）、工厂调用且函数体恰好一条 `return Allocator{…}`。否定证据：`Symbol.nassign`（被赋值）/`escaped`（被 `&`）/多返回。改完是 NK_IDENT，重复检查幂等。
- **自动内联**：后端 `fn_small_for_inline` —— **本模块私有**、AST ≤24 节点、无循环/嵌套 fn/递归 → 定义发 `ZX_INLINE`（`static inline __attribute__((always_inline))`，MSVC `__forceinline`）。原型保持 `static`。**没有函数级 `inline` 关键字**（只有 `inline for`）。跨模块不内联（单元边界，硬内联要把函数体搬进共享头）。
- 消不掉的分发：容器字段 `self.alloc`、函数形参 —— 类型擦除代价，-O2 也不行；彻底解要 `ArrayList(T, A)` 单态化，不做。
- `std.debug_alloc.DebugAllocator`：块头记 `prev/next/size/seq/magic`，活跃双向链；泄漏 `deinit` 逐块（序号/字节/地址）报告后 panic，`deinitQuiet` 只报告返回块数；double free / 野指针 / 长度不符都打印现场摘要后 panic。`HDR` 由 `@type(Header).size` 上取整，别写死。
- **`std.pool.Pool` 两个地址空间（§140）**：内部 `classPush/classPop/classGrow` 流转**块头地址** `a`；`alloc` 对外返回 `a + HDR`（负载起点），`free` 入参也是负载、`ha = a - HDR` 反查头。块头在返回指针**之前**，解引用不会读到头。HDR 现按 `@type(Header).size` 上取整（与 debug_alloc 同公式，`std/pool.zx` 是 CRLF）。负载 16 对齐靠 backing 基址 + slotSize 都是 16 倍数。守门 `tests/good/pool-header-layout.zx`（含 Pool over DebugAllocator 两层头归还校验）。
- **`Pool.allocator()`（§141）**：`Allocator{ .ctx = (*u8)self, .alloc = poolAlloc, .free = poolFree }`，ctx 直接装 `*Pool`、不另造堆上 State。差异：`free` 的 bool 吞掉、`n` 绑 `_` 忽略（池从块头反查档位）。zx 无生命周期 → Pool 必须比视图活得久、`deinit` 后视图失效。守门 `tests/good/pool-allocator.zx`。
- **`Pool.initThreadSafe`（§142）**：`init()` 不是线程安全的（`Class.head`/`count`/`region`/块头 flags 全共享）。新构造器返回**同一个 `Pool` 类型**，只加 `guard: usize`（0=无锁）；锁字是**单独从 backing 分配的一个 `u64`**，绝不放在 `Pool` 值里（Pool 是值类型，unwrap/复制/传进线程都不该复制出第二把锁）。锁 = `std.atomic` 的 TTAS 自旋锁（M58 起 mutex/cond 运行时已收掉，不新增类型/命名空间/C 运行时）。`alloc`/`free`/`used` 改薄包装，内部逻辑原样搬进私有 `*Impl`；`allocator()` 视图自动继承。三条边界：backing 自己得线程安全（DebugAllocator **不是**）、`deinit` 只能在线程收工后（销毁不加锁）、只适合极短临界区。守门 `tests/good/pool-threadsafe.zx`（6 线程×500，加锁 10/10 绿；把构造器换成 `init` 的对照组 3/3 段错误 exit 139）。
- **§138 工厂链去虚拟化是假绿（§141 定位，未修）**：`DebugAllocator.init` / `ArenaAllocator.init` / `Pool.allocator()` 这类**容器方法工厂**返回的 Allocator 仍是间接调用。两层根因：`allocator_target` 的 NK_CALL 只认 `fs->decl == NK_DECL`（容器方法 decl 是 NK_FN，NK_FIELD callee 常无 sym、只有 `->inst = Method.fn`）；且工厂体 `schedule_body` 延迟检查，`return Allocator{…}` 的字面量 `ty` 为空、`.alloc = f` 标识符 `sym` 也为空，拿不到可直调目标。彻底修要改检查顺序或跨模块符号解析，本轮回退了 sema 实验。`Allocator{…}` 字面量链不受影响。`test.sh` §138 已改成按**调用点**断言（含刻意断言工厂链仍间接），别再只 grep 函数名。
- **容器体内裸名互调（§139 已修）**：`struct { pub const a = fn () i32 {…}; pub const b = fn () i32 { return a() + 1; }; }` 曾经编不过（B1001 `call to undeclared 'a'`：定义发 `a_i0`，裸名发 `a`）。修法：`Symbol.in_agg` 记容器体节点，`check_expr` 的 NK_IDENT 遇到体里的 NK_FN 就填 `n->inst = s->decl`，后端 NK_IDENT 分支看到 `inst` 就发 `inst_name`。守门 `tests/good/member-bare-call.zx`（六种形状：链式/当值传/enum/泛型容器/方法体内/同名隔离）。`std/debug_alloc` 的 `shutdown` 仍在模块级，不必回改。
- `zx_debug_panic` 先 `fflush(stdout)`：重定向时 stdout 全缓冲，abort 不走退出清理，报告会整块丢。

## LSP 能力线（`tools/zxls.zig`）
- inlay/hover/snippet 三处配套缺一不可：`inlayHintProvider:true`、`textDocument/inlayHint` 分派、`inlayHintsAt`。snippet 必须放在 `addIdents` **之前**（否则被 seen 去重挡掉）；缩进写死 4 空格且**不设 `insertTextMode`**。
- 命名空间三分：`RecvKind{module,ty,instance}`，类型与实例列表**不许有交集**；实例 detail **保留首个 self 形参**；列成员只能在自己容器体里找（`findMemberStmt`，全文 `findDeclStmt` 会串签名）。
- 泛型/构造推断四个断点（§136）：`inferContainer` 认返回 `type` 的函数；`lit_ctx` 认 `X(2){ .`（括号配平反扫）与 `L{ .`（genericBodyOf）；单段裸名字进推断但**只接泛型构造函数**；`containerMembersAny` **逐文件**试、文件内泛型在前（全局泛型优先会抢同会话别的文档的同名容器，挂 26 条）。
- 形参接收者（§137）：`completeAt` 兜底段 `namedTypeNear` 分支；`namedTypeNear` 必须挡空 needle，否则 `indexOfPos` 越界 panic（`struct{…}.` 编辑期 path 为空串）。
- **fixture 容器名全局唯一**（`SRC_DIR` 全进同一 `scanFiles`，`P`/`List` 会串味，新用例用 `Pv*`）；lsptest 8z 段用**独立短命会话**；新 needle 不带尾随 `\n`。
- **0 项补全大多是 `.ty` 语义过滤，不是路径解析失败**：能弹出静态常量就说明路径通了。

## Task 并发（§125 / §128 / §129）
- `Task.` **7 项**：`coro/single/pool/fromId` + `SEL_AWAIT/SEL_RECV` + `group` 工厂；`t.` **12 项**：`spawn/await/awaitAll/cancel/yield/send/recv/trySend/tryRecv/select/close` + `id`；`Group` 实例 `g.spawn/wait/run/close`。
- 无 interface/子类型 → scope/chan/timer **不做对外类型**（§128 用户拍板）；机制留在 C 运行时被唤醒链复用，C 侧一字不改。
- `std.io` 纯同步、无隐式 offload；要并发显式 `task + io`。

## 编译器 / 后端
- **if 剪枝**：常量条件只检查会走的一支，结论存 `n->ival`；后端 `gen_if_body` 用 `ct ? ct->bval : n->ival`。死分支标识符要 `mark_uses` 标 used，否则假报 E0410。
- **`return <expr>;` + defer 的求值顺序（§142 已修）**：`gen_return` 普通分支原来把 `cxs(n->a, want)` 的**表达式字符串**直接塞进 `return %s;`，却先发 `run_defers(0)` → defer 跑在表达式求值**之前**（返回值被改、锁提前放开）。修法：统一先把值存进 `_dr%d` 临时量再展开 defer/errdefer 再返回；`want==TY_VOID` 时先发一句 `v;` 再 defer 再 `return;`。`is_ctl` 分支本来就对。守门 `tests/good/defer-return-order.zx`（`ret:1 log:101 vside:0 vlog:10`）。**写 Pool 这类锁包装仍用显式单一出口，别依赖 defer**。
- **zxls 文档表**：换/关文档先 `dirty.fetchRemove(path)` 再 free，否则悬垂键在扩容重哈希时 panic。
- **诊断码由 `src/zxc.c` 的 `g_rules` strstr 顺序决定**：新规则放 `found \`` 这类笼统规则**之前**。
- 守门：`demo/snake.zx --sim` 300 局、`tests/good/casts-one-way.zx`、`tests/bad/removed-*.zx`（E0211）、`tests/multibad/private-member`。

## 文档范围（用户 2026-11-10 裁定）
- 手册与离线文档站已删（std 未定型）；**`docs/errors.md` 保留**（`zxc explain` 的 details 链接、zxls 的 codeDescription、test.sh 三段覆盖检查都依赖它）。README 写明"std/ 源码 + 补全就是文档"。
