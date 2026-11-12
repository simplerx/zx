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
- **容器体内裸调同容器静态函数编不过**（B1001 `call to undeclared 'a'`：定义发 `a_i0`，裸名发 `a`）→ 写 `Self.a()`。修它要动 sema 的容器上下文，留作后续。
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
- **zxls 文档表**：换/关文档先 `dirty.fetchRemove(path)` 再 free，否则悬垂键在扩容重哈希时 panic。
- **诊断码由 `src/zxc.c` 的 `g_rules` strstr 顺序决定**：新规则放 `found \`` 这类笼统规则**之前**。
- 守门：`demo/snake.zx --sim` 300 局、`tests/good/casts-one-way.zx`、`tests/bad/removed-*.zx`（E0211）、`tests/multibad/private-member`。

## 文档范围（用户 2026-11-10 裁定）
- 手册与离线文档站已删（std 未定型）；**`docs/errors.md` 保留**（`zxc explain` 的 details 链接、zxls 的 codeDescription、test.sh 三段覆盖检查都依赖它）。README 写明"std/ 源码 + 补全就是文档"。
