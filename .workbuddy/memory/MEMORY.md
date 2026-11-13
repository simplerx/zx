# zx 项目长期约定

> 根因、守门用例、git 锚点等细节都在 `spec/decisions.md` §N；本文件只留跨会话必须遵守的红线与索引。

## 元规则
- 能力线用 **git commit + 守门用例** 保存；实现被删会连挂，**不许删用例来变绿**。定位历史：`git log --oneline --grep=inlayHint`。
- 改动完成即提交推送；精确指定路径，不捎带并行任务的工作区改动。每轮只测本轮结果，全部做完再统一全量。用户偏好实际运行验证交付。
- 控制台输出（进度/诊断/help/usage/explain）**只用 ASCII**；中文只进注释与写进文件的模板。GBK 控制台会把 UTF-8 的 `—`/`…` 渲成乱码。
- 行尾：`src/main.c|parse.c|sema.c|ast.h`、`tools/zxls.zig`、`tools/lsptest.py`、`std/std.zx` 是 **CRLF**（ast.h 混合）；`src/backend.c`、`src/errors.c`、`std/task.zx`、`docs/`、`spec/`、`README.md`、memory 是 **LF**。Python 文本读写会把 CRLF 变 LF → 一律 bytes 二进制读写，改完用 `git diff --numstat` 确认没炸成全文件。
- `// expect-output` 两种写法 runner 都认；runner **合并 stdout/stderr** 比对，期望里含相对路径+行号 → 编译必须传相对路径。bad 用例用 `// expect: <诊断子串>`。
- 改 `tools/zxls.zig` 后必须确认 `bin/zxls.exe` 真被替换（Zed 锁 exe 会让 build skip：`mv bin/zxls.exe bin/zxls-bak.exe` 绕锁再 `-femit-bin=bin/zxls.exe`），用户要重启 Zed。验证：`ZXLS_BIN=<exe> python tools/lsptest.py`；编译器改动 `sh build.sh` 再 `sh test.sh`。

## 语言事实（写文档/示例前必看）
- **无 trait / interface / 重载 / 子类型**；函数指针可作 struct 字段，调用点全靠 `(T)bits` 强转。
- 内建八个：`@mod @type @memcpy @memmove @memset @link @compileError @trap`（§144 起 `module` 收成 `@mod`）。`@type(x)` 是唯一反射；旧拼写报 **E0211**。
- **转换只有 `(T)val`**；无 GC/析构/RAII/生命周期/异常（只有 `!T`）/协程/宏/运算符重载/`private`/默认参数/变参。
- 函数定义只有 `const F = fn(…) ret {…};` —— **没有 `pub fn`**，pub 是前缀修饰符。
- **容器方法不能泛型（E0701）**；容器字段/方法/静态成员默认私有，`pub` 才对外，跨文件访问私有报 **E0404**；enum / union(enum) 变体恒公开。锚点 `sema.c member_hidden/deny_member/stamp_mod`；自举要先重建 zxfe。
- **Zx 没有数组值类型**：`*[N]T` 就是指针（`TY_PTR`+`has_len`，`ty_size`→8），长度只写在类型里做界检查与 `.len`；`[]T` 是 `{ptr,len}` 16 字节。存储用 `std.stackAlloc` / `std.heapAlloc(alloc,T,N)`（N 编译期常量）/ `std.heapSlice(alloc,T,n)`（n 运行期 → `[]T`）；`&.{…}` 是匿名存储+指针的糖。**表述红线**：`&xs` 是取指针变量的地址，文档不许写"数组的地址"。守门 `tests/good/addr-array.zx`。
- **`*[N]T` 同一个指针值两种下标都能读写**：`ptr.0/.1` 编译期常量下标、`ptr[0]/ptr[i]` 运行期下标（带界检查）；函数参数/字段/`?*[N]T`/泛型 N 全通过。守门 `tests/good/array-pointer-index.zx`。
- `std.process.args()` 不含 argv[0]；`orelse return …` 不合法；没有 `anytype`；switch 模式无区间；`if (c) { x } else { y }` 块臂不给值。
- **`fn (N: u64) type` 是泛型**：返回 `type` 的函数即类型构造函数，值参数可当数组长度。`fn_is_generic` 曾只认 `T: type`，是会复发的坑。
- `// expect-output` 与 `ct_clear_stmts` 只擦语句子树：结构体字面量里折值字段访问（`N{.a=@type(T).size}`）的 ct 不能擦，否则后端发 `0.size`。

## 能力线索引（细节见 decisions.md）
- **句柄检查只在编译期（§135）**：运行时 side table 已撤（`std.slot` 删除）。`get` 能证明 LIVE 就改写成私有 `getUnchecked`，证明不了报 **E0511**；二次释放 E0509；表已关闭 E0510。**方法名才是契约**（编译器不认识 Handle 类型）。
- **Allocator / 去虚拟化（§138、§148-§150）**：`Allocator{ctx,alloc,free}`；四条证明链 `std.heap.page`、`Allocator{…}` 字面量、if 捕获（`cap_src`）、工厂单 `return Allocator{…}`；否定证据 `nassign`/`escaped`/多返回。§148 修工厂链假绿（`callee->inst` 取 NK_FN、`lit_is_allocator` 缺戳按名字认、剥 `?Allocator` 外壳、`devirt_retry_all`）；§149 拆 `devirt_prove`/`devirt_rewrite`，登记所有候选点末轮双向复核（后置否定证据能回退已直调的点）；§150 补模块级聚合 `V_FN` 走 `g_late`/`zx_init_<unit>` 与 `synthetic_fn_sym()` 认领 `zx_page_alloc/free`。**`&` 的 escaped 标记必须在 `check_expr(n->a)` 之后**（sym 那时才绑定）。守门按**调用点**断言 + 反向断言，双前端。
- **自动内联（§138）**：后端 `fn_small_for_inline` —— 本模块私有、AST ≤24 节点、无循环/嵌套 fn/递归 → `ZX_INLINE`。**没有函数级 `inline` 关键字**。容器字段 `self.alloc`、函数形参消不掉（类型擦除代价）。
- **Pool / SyncPool（§140-§143）**：Pool 返回 `a + HDR`（HDR 按 `@type(Header).size` 上取整），`free(p)` 用 `p - HDR` 反查；`Pool.allocator()` 的 ctx 直接装 `*Pool`。线程安全是独立 `SyncPool{pool, guard: *u64, gpa}`，guard 是**堆上 u64 的地址**（值副本共享一把锁）；Pool 保持纯无锁。**简约的判断标准是有无重复逻辑，不是有无新增类型**。
- **数组/内存（§130、§144）**：`heapAlloc` 的 N 必须编译期常量（运行期 N 报 **E0501**），§130 编译期合并正是依赖它，不删；运行期长度走 `heapSlice` → `[]T`。`@mod` 改动要动四处：C `lex.c/parse.c/zxc.h`、`bootstrap/lex.zx/parse.zx`（token 枚举一一对应）、`tools/zxls.zig` 三处硬编码长度、`g_std_seeded[]`。重建 zxfe 要先移走旧 exe 强制 `--fe=c`。
- **方法接收者（§151，2026-11-13 用户裁定）**：第一参数只有 `Self` / `*Self`。`Self` 恒不可变，编译期按 `ty_size(Self)` 选 ABI（`ZX_SELF_VALUE_MAX = 16`：≤16 按值、>16 按 `const Self*`）；`*Self` 是唯一能改调用者的形态。**只改 `ft->params[0]`，其他参数保持源码写法**（用户明确追问确认）。形参节点 ty 恒留 `Self` + `Symbol.self_by_ptr` 标记，后端 NK_IDENT 发 `(*self)`；显式调用 `Type.f(a)` 自动补 `&`，指针接收者 `p.f()` 在按值形态自动解引用（复用 `.0` 节点，后端发 `(*p)`）。`*const Self` 仍兼容但全库已迁到 `Self`。**`size == 0` 不退化成关联函数**：分界是首参是否叫 self，按大小加判据会让泛型实例下调用方式漂移、LSP 也要动态判；后端给空聚合补 `uint8_t _zx_pad;`。守门 `tests/good/self-recv-abi.zx`、`tests/bad/self-immutable.zx`（E0403）、`test.sh` §151。**§152：`&self` 恒是一层只读指针 `*const Self`（不是 `*Self` / `**Self`）；`lvalue()` 的 NK_IDENT 同样要穿透 `self_by_ptr` 发 `&((*self))`，否则大类型生成 `Self **` 被 C 拒编 B1001。写入 E0403、返回地址 E0503；地址身份随 ABI 变但不承诺指向调用者。** 守门 `tests/good/addr-self.zx`、`tests/bad/addr-self-{write,return}.zx`、`test.sh` §152。
- **LSP（§136/137、§149-§150）**：inlay/hover/snippet 三处配套；命名空间三分、类型与实例列表不许有交集、实例 detail 保留首个 self 形参；列成员只能在自己容器体里找；`containerMembersAny` 逐文件且泛型在前；`tyMembersOf` 统一类型串→成员表（匿名串/具名容器/内建）；`tyCtorFallback` 只在末段确为类型名时兜底（`isTypeValuedRecv` 保 `KT.`）；`namedTypeNear` 必须挡空 needle；**fixture 容器名全局唯一**；0 项补全大多是 `.ty` 语义过滤。
- **Task 并发（§125/128/129）**：`Task.` 7 项、`t.` 12 项、`Group` 4 项；scope/chan/timer 不做对外类型（C 运行时保留机制）；`std.io` 纯同步、无隐式 offload。

## 编译器 / 后端
- **if 剪枝**：常量条件只检查会走的一支，结论存 `n->ival`；死分支标识符要 `mark_uses` 标 used。
- **`return <expr>;` + defer**（§142）：`gen_return` 先把值存 `_dr%d` 再展开 defer/errdefer；`void` 先发 `v;`。写锁包装仍用显式单一出口。
- **容器体内裸名互调（§139）**：`Symbol.in_agg` 记容器体，`check_expr` NK_IDENT 遇体里的 NK_FN 填 `n->inst`，后端发 `inst_name`。
- `clone_tree` 会清 `ty/type_val/ct/sym` —— 合成标识符只靠 `n->sym` 不够，要 `synthetic_fn_sym()` 按名字认领。
- zxls 文档表：换/关文档先 `dirty.fetchRemove(path)` 再 free，否则悬垂键扩容时 panic。
- 诊断码由 `src/zxc.c` 的 `g_rules` strstr 顺序决定；新规则放 `found \`` 这类笼统规则**之前**。
- `zx_debug_panic` 先 `fflush(stdout)`：abort 不走退出清理，重定向时报告会整块丢。
- 守门：`demo/snake.zx --sim` 300 局、`tests/good/casts-one-way.zx`、`tests/bad/removed-*.zx`（E0211）、`tests/multibad/private-member`。

## 文档范围（用户 2026-11-10 裁定）
- 手册与离线文档站已删（std 未定型）；**`docs/errors.md` 保留**（`zxc explain` 的 details 链接、zxls codeDescription、test.sh 三段覆盖检查都依赖）。README 写明 "std/ 源码 + 补全就是文档"。
