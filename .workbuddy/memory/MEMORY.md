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

## 文档范围（2026-11-13 校正）
- **`docs/manual.md`（入门）、`docs/reference.md`（速查）、`docs/errors.md`（错误码）都在 HEAD 里且持续维护**（§146/§144/1.3.0 都改过）；离线文档站不做。旧记忆「手册已删」已过期 —— README 的文档表指向三者，`std/` 源码与编辑器补全仍是一手资料。
- `docs/errors.md` 是 `zxc explain` details 链接、zxls codeDescription、test.sh 三段覆盖检查的依赖，改动不能丢码与 `id="eNNNN"` 锚点。
- **版本点四处**：`src/zxc.h ZXC_VERSION`、`docs/reference.md` 头、`docs/manual.md` 落款、README 文档表；`zxc init` 写进 zx.json 的 0.1.0 是工程版本，别一起改。当前 **1.3.0**。
- **§153**：region 合并的分配器若是本块局部声明，`__zxrN = zx_heapalloc(al,…)` 必须排在那条 NK_DECL 之后（`emit_region_alloc`），否则 §150 的 late-init `std.heap.page` 局部副本会让 C 先用后声明报 B1002；`bench/run.sh` 的 build FAILED 必须非零退出。

## §154-§156 三个截图反馈（2026-11-13）
- **`[]T.len/.size` 是运行期值**：只有长度写在类型里（`*[N]T`、元组）才是编译期常量；`const L = s.len` 报 E0501。文档 `reference.md §4` 曾写错，spec §3.3 / decisions 旧裁定一直是对的。后端 `ex()` 曾把 `[]T.size` 发成 `layout_size`=16（描述符大小），与 sema `ct_field` 的 `len*sizeof(elem)` 不一致 → 修成 `.len * es`，`is_const_init` 也别再把切片 `.len/.size` 当 C 常量。**类型检查/常量折叠/代码生成三处必须同一条规则**。
- **`continue :label;` 本来就实现了**（冒号前缀，标签写在循环关键字前；`src/parse.c` + `bootstrap/parse.zx`、sema `label_find_loop`、后端 `lab_find_loop`）。`docs/reference.md`/`manual.md` 漏写冒号造成「没实现」的错觉；不造无冒号别名（两种拼写不合规），只改文档 + 补守门。`continue label;` 是 E0201，未知标签 E0405。
- **LSP 行内提示不是源码**：截图里的 `: void`、`self: `、`fmt: `、`args: ` 都是 zxls inlay（Zx 没具名实参，照抄会 E0201）。`fn (T: type, n: T) type` 的值形参可以当返回 struct 的字段默认值，编译器一直支持，调用结果是类型值。
- `tools/zxls.zig sigOfStmt` 曾 `indexOf("fn ") orelse indexOf("fn(")`，类型构造函数里挑中体内方法 → 返回类型报 `void`、参数名报 `self`。**取两种拼写里最靠前的 fn**。改 zxls 必须绕 Zed 锁重编 `bin/zxls.exe` 并让用户重启 Zed。


## §157（2026-11-13）
- **std 新库**：`std/math.zx`（`extern "C" { pub fn … }` 直接把 C 符号当模块公开成员，不写转发层/不 @link，CRT/libm 默认可链）、`std/net.zx`（同步 IPv4 TCP/UDP，`@link("ws2_32")` 写在模块里；只 Windows，POSIX 待条件编译；C 符号私有，句柄 u64，失败用 ?T / -1）。`std/std.zx` 里注册。**extern 无名形参必须按位置编号 `_p%d`**（`gen_extern` 曾全发 `_p`，多参原型被 C 判重定义）。
- **裸名内建种进所有模块作用域，含入口单元**（§158 推翻 §157「入口不种、仍要 std. 前缀」那条）；`sema_check` 在 hoist 后也调 `seed_std_builtins`，三处共用一份 helper。`sema.c check_call` 统一解析内建符号 `m`：限定名走 `ns_member`，裸名走 `lookup` 且只认 `builtin != BI_NONE` 的 SK_NS；后端 NK_CALL 按 `sema_sym_builtin_name(n->a->sym)` 分派，**按符号不按名字**，用户同名函数不会被劫持。守门 `tests/multi/seeded-builtins/`、`tests/good/seeded-builtins-entry.zx`。`std.` 前缀仍是推荐写法。
- **LSP 构造式**：`var gen = Gen(i32,10){…}` 初值以 `}` 收尾，`inferExprType` 要收敛成实例串；`fieldTypeIn` 会连字段默认值一起读出（`mem: T = n`），`substGenericArg` 必须先切掉 `=` 后半段，否则 inlay 被合法字符检查挡掉、hover 拼成 `let n: i32 = n = gen.mem`。类型构造函数要同时给 `Gen(...)` 与 `Gen(...){...}` 两条 snippet；inlay 白名单含 `(`/`)`。
- **测试环境坑**：`test.sh` 的 fixture 目录是 `.tmp/zxc_test.<PID>.cli.proj`，Windows 会复用 PID；沙箱里 `rm -rf` 可能静默失败留下旧 `zx.json`，让 5 条 CLI 冒烟假红（与代码无关）。先清掉残留 `*.cli.proj` 再重跑。

## §158（2026-11-13，两张截图）
- **`std/math.zx` 的 extern 形参一律具名**：无名形参虽能编译（`_p0/_p1`），LSP 的 `sigOfStmt` 会把它显示成 C 原型 `sqrt(f64) f64`，snippet 占位落成类型名 `f64`；具名后是 `sqrt(x: f64) f64` / `sqrt(${1:x})$0`。`_p0/_p1` 能力改由 `tests/good/extern-unnamed.zx` 单独守门。**常量不要跟函数同前缀**（`sqrt2` → `root2`），否则输入 `sqrt` 会连带弹出常量，看起来像重复定义。
- **LSP 假诊断的根因是作用域不对称，不是 zxls 解析**：`runCheck` 写同目录影子文件再 `zxc <影子> --check`，入口单元若不种 `g_std_seeded`，`std/net.zx` 的 `stackAlloc`、`std/fs.zx` 的 `file` 都会假报 E0401。根治 = 入口也种同一张表（纯放宽，不动 zxls、不做按文件名过滤）。`test.sh §158` 直接钉「单文件 --check 零输出」。
- **守门别拿 libc 名字当样本**：`backend.c libc_declared()` 会把 `sqrt/pow/floor/ceil/fmod/fabs` 等 math.h 已声明的 extern 原型从生成 C 里排除；钉「形参具名」要用会发射的 `cbrt/atan2/hypot`，否则断言恒假（§158 首轮全量唯一假红）。

## §159 std.simd（2026-11-13）
- **没有向量类型时的 SIMD 就靠下游 C 编译器**：`std/simd.zx` 两层 —— `Vec4f/Vec4i` 是 4 分量值类型（16B、按值传、标量方法）；切片内核（f32 11 个 + i32 3 个）写成最朴素的逐元素单循环，`--release`（-O2）下 zig cc 发 `vaddps %zmm`，dev（-O0）不向量化。**不手工展开**（实测普通循环得到同样的 zmm，展开只让代码长一倍）、不加运行时、不动编译器。
- 语义契约：按 `min(各 len)` 处理并**返回个数**；逐元素内核允许 `dst` 与输入同块（原地安全）；归约用 4 条累加链（不改浮点顺序、不需 fast-math）；`normalize` 零向量原样返回；`length` 走私有 `extern sqrtf(f32) f32`（`sqrtf` 不在 libc_declared，生成 C 有 `extern float sqrtf(float x);`）。
- 守门 `tests/good/std-simd.zx`（尾块/别名/不等长/两类型）、`test.sh §159`（release 输出与 dev 一致 + sqrtf 原型 + 单文件检查 + C 前端）、`lsptest` 两条补全断言。实测 4M f32 add×20：dev 118ms/8.4GB/s，release 31ms/约31GB/s。

## §160 C shim 通道 @cShim（2026-11-13）
- **内建 9 个**：`@mod @type @cShim @memcpy @memmove @memset @link @compileError @trap`。加内建要动：`src/sema.c check_builtin` + 可能新错误码三张表（`zxc.c g_rules` / `errors.c` / `docs/errors.md` 含锚点 + reference §15 计数）+ `tools/zxls.zig` 的 `builtin_docs` / `toplevel_builtins` / snippets + 文档四处名单（reference §10、manual §8、errors.md E0211 段、spec/zx-spec.md）。**词法/语法不用改**：`@name(...)` 是通用的，`bootstrap/` 也不用动。
- `@cShim("impl.c")`：那份 `.c` 是**自包含 C TU**，zxc 用同一套 cc/flags 编成独立 `.o` 一起链接，**直接编真实路径**（不复制不改名）⇒ C 报错行号即真实行号，报 **B1004**；文件打不开报 **E0602**。路径相对 `g_srcpath` 目录（库可自带实现）；按完整路径去重；`--emit-c` 把 shim 文本贴末尾（自洽单 TU）。shim 看不见 zx 生成的任何东西（只 include 自己的 C 头）。
- `shim 编不过`时构建总结走「N error(s) in the C shim(s)」，**不许**说 "compiler bug"。
- **`run_cc_jobs()` 串行分支必须写回 `jobs[i].rc`**：诊断翻译 `report_cc_log` 只认 rc，漏写时 njobs==1 的失败只剩一句 "build failed in the C backend"（§160 第一轮就是这么被吞的）。
- zxls `@` 补全：`identBefore` 不吃 `'@'`，所以 `@cSh` 这种半截内建名要靠 `ps-1 == '@'` 判（与 dotted 同口径），否则掉进全局标识符分支、只弹一条同名变量。改 `tools/zxls.zig` 后必须先 `mv bin/zxls.exe …` 绕 Zed 锁再重建，用户重启 Zed 生效。
- 守门：`tests/good/cshim.zx` + `tests/good/cshim_impl.c`、`test.sh §160` 四条、`lsptest` 的 `completion.cShim.partialAtIsBuiltin`。
