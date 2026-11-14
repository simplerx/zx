# zx 项目长期约定

> 根因、守门用例、实测数据都在 `spec/decisions.md` §N；本文件只留跨会话必须遵守的红线与索引。当前版本 **1.4.0**。

## 元规则
- 能力线用 **git commit + 守门用例** 保存；实现被删会连挂，**不许删用例来变绿**。定位历史：`git log --oneline --grep=关键词`。
- 改动完成即提交推送；精确指定路径，不捎带并行任务的工作区改动。每轮只测本轮结果，全部做完再统一全量。用户偏好实际运行验证交付。Git Bash 多行提交用 `printf '%s\n' … | git commit -F -`。
- 控制台输出（进度/诊断/help/usage/explain）**只用 ASCII**；中文只进注释与写进文件的模板。GBK 控制台会把 `—`/`…` 渲成乱码。
- **行尾**：`src/main.c|parse.c|sema.c|ast.h|astload.c|ty.c|ty.h`、`tools/zxls.zig`、`tools/lsptest.py`、`std/std.zx`、`test.sh` 是 **CRLF**（ast.h 混合）；`src/backend.c`、`src/errors.c`、`std/task.zx`、`bootstrap/`、`docs/`、`spec/`、`README.md`、memory 是 **LF**。Python 文本读写会把 CRLF 变 LF → 一律 bytes 二进制读写，改完用 `git diff --numstat` 确认没炸成全文件（炸了就按 HEAD 每条行尾回填）。
- `// expect-output` 两种写法 runner 都认；runner **合并 stdout/stderr** 比对，期望里含相对路径+行号 → 编译必须传相对路径。bad 用例用 `// expect: <诊断子串>`。`tests/good|bad/*.zx` 自动发现，新增文件不用改 test.sh（但特性级断言要单写一节）。
- 改 `tools/zxls.zig` 后必须确认 `bin/zxls.exe` 真被替换（Zed 锁 exe 会让 build skip：`mv bin/zxls.exe bin/zxls-bak.exe`，再 `./.toolchain/zig.exe build-exe tools/zxls.zig -O ReleaseFast -femit-bin=bin/zxls.exe`），用户要重启 Zed。验证：`ZXLS_BIN=bin/zxls.exe python tools/lsptest.py`；编译器改动 `sh build.sh` 再 `sh test.sh`（当前基线 passed 421 / failed 0 + LSP SMOKE ALL PASS）。

## 语言事实（写文档/示例前必看）
- **无 trait / interface / 重载 / 子类型**；函数指针可作 struct 字段，调用点靠 `(T)bits` 强转。
- 值内建九个：`@mod @type @shim @memcpy @memmove @memset @link @compileError @trap`；另有**类型内建** `@Vec(T, N)`（§162，只存在于类型位置/类型值，**不占九个的名额**、不进 `check_builtin`）。`@type(x)` 是唯一反射；旧拼写报 **E0211**。
- **转换只有 `(T)val`**；无 GC/析构/RAII/生命周期/异常（只有 `!T`）/协程/宏/运算符重载/`private`/默认参数/变参。函数定义只有 `const F = fn(…) ret {…};`（**没有 `pub fn`**，pub 是前缀修饰符）。
- **容器方法不能泛型（E0701）**；容器字段/方法/静态成员默认私有，跨文件访问报 **E0404**；enum / union(enum) 变体恒公开。锚点 `sema.c member_hidden/deny_member/stamp_mod`。
- **没有数组值类型**：`*[N]T` 就是指针（`TY_PTR`+`has_len`，`ty_size`→8），长度只写在类型里；`[]T` 是 `{ptr,len}` 16B。存储：`std.stackAlloc` / `heapAlloc(alloc,T,N)`（N 编译期常量，否则 E0501）/ `heapSlice(alloc,T,n)`（运行期 n → `[]T`）/ `&.{…}`（匿名存储糖）。**表述红线**：`&xs` 是取指针变量的地址，文档不许写"数组的地址"。
- `*[N]T` 同一个指针值两种下标都能读写：`ptr.0` 编译期、`ptr[i]` 运行期（带界检查）；参数/字段/`?*[N]T`/泛型 N 全通过。
- **`.len`/`.size` 只有长度写在类型里才是编译期常量**（`*[N]T`、元组、`@Vec`）；`[]T` 的两者都是**运行期值**（`const L = s.len` 报 E0501）。类型检查/常量折叠/代码生成三处必须同一条规则。
- **`@Vec(T, N)`**：值类型、对齐 = 大小、N ∈ {1,2,4,8,16,32,64}（否则 E0305）；`+ - *` 与浮点 `/`、一元 `-`；`v.N` 左值、`v[i]` 带界；**splat 就是转换 `(@Vec(T,N))x`**，不做隐式 splat；同大小向量可互转。后端落 clang `vector_size`（不是 struct 模拟），`{}` 描述符按分量展开。
- `std.process.args()` 不含 argv[0]；`orelse return …` 不合法；没有 `anytype`；switch 模式无区间；`if (c) { x } else { y }` 块臂不给值；`fn (N: u64) type` 是泛型（`fn_is_generic` 只认 `T: type` 是会复发的坑）。
- LSP 的 inlay（`self: `、`fmt: `、`: void`）**不是源码**，照抄会 E0201。

## 能力线索引（细节见 decisions.md）
- **句柄检查只在编译期（§135）**：运行时 side table 已撤。`get` 能证明 LIVE 就改写成 `getUnchecked`，证明不了报 **E0511**；二次释放 E0509、表已关闭 E0510。**方法名才是契约**。
- **Allocator 去虚拟化（§138、§148-§150）**：证明链 `std.heap.page` / 字面量 / if 捕获 / 工厂单返回；否定证据 `nassign`/`escaped`/多返回。§149 末轮双向复核（后置否定证据能回退已直调的点）；§150 模块级聚合走 `g_late`/`zx_init_<unit>` + `synthetic_fn_sym()`。**`&` 的 escaped 标记必须在 `check_expr(n->a)` 之后**。守门按调用点断言 + 反向断言，双前端。
- **自动内联（§138）**：`fn_small_for_inline` —— 本模块私有、AST ≤24 节点、无循环/嵌套 fn/递归 → `ZX_INLINE`。**没有函数级 `inline` 关键字**。
- **Pool / SyncPool（§140-§143）**：Pool 返回 `a + HDR`、`free(p)` 用 `p - HDR` 反查，ctx 装 `*Pool`；线程安全是独立 `SyncPool{pool, guard: *u64, gpa}`（guard 是堆上 u64 的地址）。**简约的判断标准是有无重复逻辑，不是有无新增类型**。
- **`@mod` 改动要动四处**：C `lex.c/parse.c/zxc.h`、`bootstrap/lex.zx/parse.zx`（token 枚举一一对应）、`tools/zxls.zig` 三处硬编码长度、`g_std_seeded[]`。重建 zxfe 要先移走旧 exe 强制 `--fe=c`。
- **方法接收者（§151/§152）**：首参只有 `Self` / `*Self`；`Self` 不可变，按 `ty_size` 选 ABI（`ZX_SELF_VALUE_MAX=16`，>16 按 `const Self*`）；`self_by_ptr` 标记，后端发 `(*self)`；`&self` 恒是 `*const Self`，写入 E0403、返回地址 E0503。**size==0 不退化成关联函数**，后端补 `uint8_t _zx_pad;`。
- **LSP（§136/137、§149-§150、§159-§162）**：inlay/hover/snippet 三处配套；命名空间三分、类型与实例列表不许有交集；`tyMembersOf` 统一类型串→成员表；`tyCtorFallback` 只在末段确为类型名时兜底；`namedTypeNear` 挡空 needle；**fixture 容器名全局唯一**；0 项补全大多是 `.ty` 语义过滤；`sigOfStmt` 取两种 fn 拼写里**最靠前**的那个。
- **Task 并发（§125/128/129）**：`Task.` 7 项、`t.` 12 项、`Group` 4 项；scope/chan/timer 不对外；`std.io` 纯同步。
- **std 库（§157-§161）**：`std/math.zx`（`extern "C"` 直接公开 C 符号，形参**必须具名**，常量别跟函数同前缀）、`std/net.zx`（同步 IPv4 TCP/UDP，`@link("ws2_32")`，只 Windows）、`std/simd.zx` + `std/simd_impl.c`（**手写 SSE2/AVX2/标量三路**，经 `@shim` 进构建，三路**逐位一致**、8 条累加链、无 FMA、不加 `restrict`；`ZX_SIMD_ISA` 强制降档、`isaName()`）。`Vec4f/Vec4i` 仍是纯 Zx 4 分量类型（语言级向量用 `@Vec`）。
- **`@shim("impl.c")`（§160）**：自包含 C TU，真实路径编成独立 `.o`（报错行号即真实行号，编不过报 **B1004** 且不许说 compiler bug；缺文件 **E0602**）；路径相对写它的 `.zx`；按完整路径去重；`--emit-c` 贴末尾。旧名 `@cShim` 报 E0211。
- **`@Vec`（§162）**：`ty.h TY_VEC`、`ast.h NK_VEC_TYPE`（末尾追加，`astload.c` 上界跟着改）、`bootstrap/parse.zx` 的 `vec` + **值位置入口**（`const V4 = @Vec(f32,4);` 写在表达式里，只在类型位置接会报"unknown builtin @Vec"）。后端 `tsuffix/tn/ensure_type/ctype` + `.len/.N/下标/lvalue/cast/描述符`。

## 编译器 / 后端
- **if 剪枝**：常量条件只检查会走的一支，结论存 `n->ival`；死分支标识符要 `mark_uses` 标 used。
- **`return <expr>;` + defer**（§142）：`gen_return` 先把值存 `_dr%d` 再展开 defer/errdefer。
- **容器体内裸名互调（§139）**：`Symbol.in_agg` + `n->inst`，后端发 `inst_name`。
- `clone_tree` 会清 `ty/type_val/ct/sym` —— 合成标识符要 `synthetic_fn_sym()` 按名字认领。
- zxls 文档表：换/关文档先 `dirty.fetchRemove(path)` 再 free，否则悬垂键扩容 panic。
- 诊断码由 `src/zxc.c` 的 `g_rules` strstr 顺序决定；新规则放 `found \`` 这类笼统规则**之前**。加码要同时改 `zxc.c` / `errors.c` / `docs/errors.md`（含 `id="eNNNN"` 锚点）/ reference §15 计数。
- `zx_debug_panic` 先 `fflush(stdout)`：abort 不走退出清理，重定向时报告会整块丢。
- 守门常客：`demo/snake.zx --sim` 300 局、`casts-one-way.zx`、`tests/bad/removed-*.zx`（E0211）、`tests/multibad/private-member`。
- 测试环境坑：`test.sh` 的 fixture 是 `.tmp/zxc_test.<PID>.cli.proj`，Windows 会复用 PID；沙箱里 `rm -rf` 可能静默失败留下旧 `zx.json`，让 5 条 CLI 冒烟假红。先清残留再重跑。

## 文档
- `docs/manual.md`（入门）、`docs/reference.md`（速查）、`docs/errors.md`（错误码）都在 HEAD 且持续维护；离线文档站不做。`errors.md` 是 `zxc explain` 详情、zxls codeDescription、test.sh 覆盖检查的依赖。
- **版本点四处**：`src/zxc.h ZXC_VERSION`、`docs/reference.md` 头、`docs/manual.md` 落款、README 文档表；`zxc init` 写进 zx.json 的 0.1.0 是工程版本，别一起改。
- **守门别拿 libc 名字当样本**：`backend.c libc_declared()` 会把 `sqrt/pow/floor/fabs` 等已声明原型从生成 C 里排除，钉"形参具名"要用会发射的 `cbrt/atan2/hypot`，否则断言恒假。
