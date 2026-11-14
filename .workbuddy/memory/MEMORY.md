# zx 项目长期约定

> 根因、守门用例、实测数据都在 `spec/decisions.md` §N；本文件只留跨会话必须遵守的红线与索引。当前版本 **1.4.2**。

## 元规则
- 能力线用 **git commit + 守门用例** 保存；实现被删会连挂，**不许删用例来变绿**。定位历史：`git log --oneline --grep=关键词`。
- 改动完成即提交推送；精确指定路径，不捎带并行任务的工作区改动。每轮只测本轮结果，全部做完再统一全量。用户偏好实际运行验证交付。Git Bash 多行提交用 `printf '%s\n' … | git commit -F -`。
- 控制台输出（进度/诊断/help/usage/explain）**只用 ASCII**；中文只进注释与写进文件的模板。GBK 控制台会把 `—`/`…` 渲成乱码。
- **压测入口 `sh tools/stress.sh`**（regress [plain|nocache|checked] / fuzz N / lsp / numeric / concurrent，§169）。concurrent 阶段必须导出 `ZIG_LOCAL_CACHE_DIR`/`ZIG_GLOBAL_CACHE_DIR` 到仓库内——默认全局缓存下 16 并发会有一个进程卡死在 `compiling N units...`（4min+），仓库内共享缓存 10s 全过。simd 求逆判据用**后向误差** `‖m·inv−I‖/(‖m‖·‖inv‖) ≤ 1e-5`，绝对误差会随样本量在条件数尾部抖动。
- **行尾**：`src/{main,parse,sema}.c`、`src/{astload.c,ty.c,ty.h}`、`tools/{zxls.zig,lsptest.py}`、`std/std.zx` 是 **CRLF**；`src/ast.h`、`spec/decisions.md` 是**混合**（同一文件两种都有）；`src/backend.c`、`src/errors.c`、`bootstrap/`、`docs/`、`test.sh`、`build.sh`、memory 是 **LF**。Python 一律 bytes 读写，改完 `git diff --numstat` 确认没炸整文件（炸了就按 HEAD 每条行尾回填）。
- `// expect-output` 两种写法 runner 都认；runner **合并 stdout/stderr** 比对，期望里含相对路径+行号 → 编译必须传相对路径。bad 用例用 `// expect: <诊断子串>`。`tests/good|bad/*.zx` 自动发现，新增文件不用改 test.sh（但特性级断言要单写一节）。
- 改 `tools/zxls.zig` 后必须确认 `bin/zxls.exe` 真被替换（Zed 锁 exe 会让 build skip：`mv bin/zxls.exe bin/zxls-bak.exe`，再 `./.toolchain/zig.exe build-exe tools/zxls.zig -O ReleaseFast -femit-bin=bin/zxls.exe`），用户要重启 Zed。验证：`ZXLS_BIN=bin/zxls.exe python tools/lsptest.py`；编译器改动 `sh build.sh` 再 `sh test.sh`（当前基线 passed 429 / failed 0 + LSP 冒烟 454 项全过 / SMOKE ALL PASS；§166 已根治旧的 diagnostics.* 假崩溃）。

## 语言事实（写文档/示例前必看）
- **无 trait / interface / 重载 / 子类型**；函数指针可作 struct 字段，调用点靠 `(T)bits` 强转。
- 值内建**九个**：`@mod @type @shim @memcpy @memmove @memset @link @compileError @trap`；**`@Vec(T, N)` 是唯一的向量内建，且是类型内建**（只在类型位置/类型值，**不进 `check_builtin`**）。**向量的一切运算都在 `std.simd`**（§165：`select` / `any` / `all` 与原内建同义，旧拼写 `@select` / `@any` / `@all` 报 **E0211** 并指到同名函数）。`@type(x)` 是唯一反射。
- **转换只有 `(T)val`**；无 GC/析构/RAII/生命周期/异常（只有 `!T`）/协程/宏/运算符重载/`private`/默认参数/变参。函数定义只有 `const F = fn(…) ret {…};`（**没有 `pub fn`**，pub 是前缀修饰符）。
- **容器方法不能泛型（E0701）**；容器字段/方法/静态成员默认私有，跨文件访问报 **E0404**；enum / union(enum) 变体恒公开。锚点 `sema.c member_hidden/deny_member/stamp_mod`。
- **没有数组值类型**：`*[N]T` 就是指针（`TY_PTR`+`has_len`，`ty_size`→8），长度只写在类型里；`[]T` 是 `{ptr,len}` 16B。存储：`std.stackAlloc` / `heapAlloc(alloc,T,N)`（N 编译期常量，否则 E0501）/ `heapSlice(alloc,T,n)`（运行期 n → `[]T`）/ `&.{…}`（匿名存储糖）。**表述红线**：`&xs` 是取指针变量的地址，文档不许写"数组的地址"。
- `*[N]T` 同一个指针值两种下标都能读写：`ptr.0` 编译期、`ptr[i]` 运行期（带界检查）；参数/字段/`?*[N]T`/泛型 N 全通过。
- **`.len`/`.size` 只有长度写在类型里才是编译期常量**（`*[N]T`、元组、`@Vec`）；`[]T` 的两者都是**运行期值**（`const L = s.len` 报 E0501）。类型检查/常量折叠/代码生成三处必须同一条规则。
- **`@Vec(T, N)`（§162/§163）**：值类型、对齐 = 大小、N ∈ {1,2,4,8,16,32,64}（否则 E0305）；`+ - *`、浮点 `/`、一元 `-`、**六个比较**（结果 = 同 lane 的**有符号整数掩码** 0/-1）；`v.N` 左值、`v[i]` 带界；**splat 就是转换 `(@Vec(T,N))x`**，不做隐式 splat；同大小可互转。字面量 `@Vec(T,N){…}` / 别名 `V4{…}` 在 parse 脱糖成"cast 一个元组"（不加 NodeKind）。**没有 `@select` / `@any` / `@all` 内建了**（§165 之前后端是 `_selN` 位运算与 `zx_any_*` 助手，已删）。后端落 clang `vector_size`（不是 struct 模拟）。**`@Vec` 长度不能泛型**：模块级签名在实例化前就过 `resolve_type`，`fn (N: usize, a: @Vec(f32, N))` 报 E0501，库层写死 `V4` / `M4`。**`std.simd.V4{…}` 这种带路径的字面量不合法**，要先 `const V4 = std.simd.V4;`。**clang 拒绝 `_Bool` 向量与向量三元 `m ? a : b`**。
- `std.process.args()` 不含 argv[0]；`orelse return …` 不合法；没有 `anytype`；switch 模式无区间；`if (c) { x } else { y }` 块臂不给值；`fn (N: u64) type` 也是泛型（`fn_is_generic` 只认 `T: type` 是会复发的坑）。
- LSP 的 inlay（`self: `、`fmt: `、`: void`）**不是源码**，照抄会 E0201。

## 能力线索引（细节见 decisions.md）
- **句柄检查只在编译期（§135）**：运行时 side table 已撤。`get` 能证明 LIVE 就改写成 `getUnchecked`，证明不了报 **E0511**；二次释放 E0509、表已关闭 E0510。**方法名才是契约**。
- **Allocator 去虚拟化（§138、§148-§150）**：证明链 `std.heap.page` / 字面量 / if 捕获 / 工厂单返回；否定证据 `nassign`/`escaped`/多返回。末轮双向复核；模块级走 `g_late`/`zx_init_<unit>` + `synthetic_fn_sym()`。**`&` 的 escaped 标记必须在 `check_expr(n->a)` 之后**。守门按调用点断言 + 反向断言，双前端。
- **自动内联（§138）**：`fn_small_for_inline` —— 本模块私有、AST ≤24 节点、无循环/嵌套 fn/递归 → `ZX_INLINE`。**没有函数级 `inline` 关键字**。
- **Pool / SyncPool（§140-§143）**：Pool 返回 `a + HDR`、`free(p)` 用 `p - HDR` 反查，ctx 装 `*Pool`；线程安全是独立 `SyncPool{pool, guard: *u64, gpa}`（guard 是堆上 u64 的地址）。**简约的判断标准是有无重复逻辑，不是有无新增类型**。
- **`@mod` 改动要动四处**：C `lex.c/parse.c/zxc.h`、`bootstrap/lex.zx/parse.zx`（token 枚举一一对应）、`tools/zxls.zig` 三处硬编码长度、`g_std_seeded[]`。重建 zxfe 要先移走旧 exe 强制 `--fe=c`。
- **方法接收者（§151/§152）**：首参只有 `Self` / `*Self`；`Self` 不可变，按 `ty_size` 选 ABI（`ZX_SELF_VALUE_MAX=16`，>16 按 `const Self*`）；`self_by_ptr` 标记，后端发 `(*self)`；`&self` 恒是 `*const Self`，写入 E0403、返回地址 E0503。**size==0 不退化成关联函数**，后端补 `uint8_t _zx_pad;`。
- **LSP（§136/137、§149-§150、§159-§162）**：inlay/hover/snippet 三处配套；命名空间三分、类型与实例列表不许有交集；`tyMembersOf` 统一类型串→成员表；`tyCtorFallback` 只在末段确为类型名时兜底；`namedTypeNear` 挡空 needle；**fixture 容器名全局唯一**；0 项补全大多是 `.ty` 语义过滤；`sigOfStmt` 取两种 fn 拼写里**最靠前**的那个。
- **Task 并发（§125/128/129）**：`Task.` 7 项、`t.` 12 项、`Group` 4 项；scope/chan/timer 不对外；`std.io` 纯同步。
- **std 库（§157-§161、§165、§167）**：`std/math.zx`（`extern "C"` 直接公开 C 符号，形参**必须具名**，常量别跟函数同前缀）、`std/net.zx`（同步 IPv4 TCP/UDP，`@link("ws2_32")`，只 Windows）、`std/simd.zx`（§165 起是**纯 Zx 数学层**：形参就是 `@Vec(f32,4)`，掩码、度量、逐分量、cross/reflect、四元数 `Quat`；§167 起再加列主序 `Mat3`(3 列) / `Mat4`(4 列) 与四元数互转；**`simd_impl.c` 已删**，标准库现在一份 shim 都没有）。`min/max/clamp/abs` 走「比较 + select」，NaN 原样留下、不清 -0.0 符号位；`dot` 加法顺序固定 `((0+1)+(2+3))`。**`approxEq` 的 eps 是逐分量最大绝对差，eps=0 退化为精确相等（`near4` 用 `<=`，§168）；近似线性相关矩阵 det 非精确 0，`invert` 只保证 det==0.0 返零矩阵**。
- **`@shim("impl.c")`（§160）**：自包含 C TU，真实路径编成独立 `.o`（报错行号即真实行号，编不过报 **B1004** 且不许说 compiler bug；缺文件 **E0602**）；路径相对写它的 `.zx`；按完整路径去重；`--emit-c` 贴末尾。旧名 `@cShim` 报 E0211。
- **`@Vec` 实现锚点**：`ty.h TY_VEC`、`ast.h NK_VEC_TYPE`（末尾追加）、`bootstrap/parse.zx` 的 `vec` + **值位置入口**（只在类型位置接会报 unknown builtin；别名与泛型实参都写在值位置）。后端 `tsuffix/tn/ensure_type/ctype` + `.len/.N/下标/lvalue/cast/描述符`。**LSP**：`vecHead/vecTyParse/vecBinOp` + `inferExprType` 三分支 + `addVecMembers`；**inlay 类型串白名单必须含 `@`**，且 inlay 遇第一个语法错就停。

## 编译器 / 后端
- **if 剪枝**：常量条件只检查会走的一支，结论存 `n->ival`；死分支标识符要 `mark_uses` 标 used。
- **`return <expr>;` + defer**（§142）：`gen_return` 先把值存 `_dr%d` 再展开 defer/errdefer。
- **容器体内裸名互调（§139）**：`Symbol.in_agg` + `n->inst`，后端发 `inst_name`。
- `clone_tree` 会清 `ty/type_val/ct/sym` —— 合成标识符要 `synthetic_fn_sym()` 按名字认领。
- zxls 文档表：换/关文档先 `dirty.fetchRemove(path)` 再 free，否则悬垂键扩容 panic。
- 诊断码由 `src/zxc.c` 的 `g_rules` strstr 顺序决定；新规则放 `found \`` 这类笼统规则**之前**。加码要同时改 `zxc.c` / `errors.c` / `docs/errors.md`（含 `id="eNNNN"` 锚点）/ reference §15 计数。
- `zx_debug_panic` 先 `fflush(stdout)`：abort 不走退出清理，重定向时报告会整块丢。
- **前端 spawn 不经命令解释器（§166）**：`src/fe.c fe_spawn_zxfe` 在 Windows 用
  `CreateProcessA` + 可继承日志句柄填 `STARTUPINFOA.hStdError`，zxls 的 `runCheck`
  也直接 `argv={zxc,shadow,--check}`。以前 `system("zxfe ... 2> log")` 两层壳，
  受限宿主里重定向建不出来 → rc=1 + 空日志 → 编辑器把正常 `@mod` 标成
  "compiler bug"。**关缓存（`ZXC_NO_AST_CACHE=1`）才测得到这段**。
- **LSP 导入补全不能扫 `g_std_dir`（§166）**：std/ 里的 .zx 是 std 模块内部文件，
  顶层入口只有 `@mod("std")`。`addImportModules` 走专用 `modImportDirsInto`；
  否则推荐 `@mod("arena")` 这种编译不过的 E0601。补全 detail 不许露 snippet `$1`。
- **大聚合的 ABI 红线（§167 实测）**：① 别写 `Mat4.fromMat3(m: Mat3)` 这类
  「跨容器关联函数收 >16B 聚合」——同一函数若又被 `self`（`*const Mat3`）形态
  调一次，形参会被钉成指针，所有传值调用报 **E0301**。互转统一走 `toXxx(self)`
  方法链。② >16B 容器的方法按 `const Self*` 收接收者，**临时值不能调方法**
  （`Mat4.identity().mul(m)` 报 **E0403**），中间值先 `let tmp = ...;`。Quat 正好
  16B 按值，不受影响。
- **矩阵数值契约（§167）**：`determinant` 与 `invert` 共用同一批 3x3 余子式
  （`minors` + `drop0..drop3` + `det3=dot3(a,cross(b,c))`），是通用伴随矩阵，
  投影矩阵也能求逆；`det==0` 时 `invert` 返回零矩阵。`toQuat` 只认纯旋转矩阵，
  结果可能是 `q` 或 `-q`（比较旋转效果，别比分量）。
- **LSP 补全两套列表（§167）**：`Type.` 只列关联函数（构造入口），实例 `.` 才列
  方法；写守门时别把方法断言挂在 `Mat4.` 上。
- **影子文件要先抹属性**：`hideFile` 设 HIDDEN|TEMPORARY，LSP 被杀会留下点文件；
  Windows 对 HIDDEN 文件 `CREATE_ALWAYS` 重写 ACCESS_DENIED，会让那个文档永久没诊断。
  `unhideFile` 写前/删前各调一次。
- 守门常客：`demo/snake.zx --sim` 300 局、`casts-one-way.zx`、`tests/bad/removed-*.zx`（E0211）、`tests/multibad/private-member`。
- 测试环境坑：`test.sh` 的 fixture 是 `.tmp/zxc_test.<PID>.cli.proj`，Windows 会复用 PID；沙箱里 `rm -rf` 可能静默失败留下旧 `zx.json`，让 5 条 CLI 冒烟假红。先清残留再重跑。

## 文档
- `docs/manual.md`（入门）、`docs/reference.md`（速查）、`docs/errors.md`（错误码）都在 HEAD 且持续维护；离线文档站不做。`errors.md` 是 `zxc explain` 详情、zxls codeDescription、test.sh 覆盖检查的依赖。
- **版本点四处**：`src/zxc.h ZXC_VERSION`、`docs/reference.md` 头、`docs/manual.md` 落款、README 文档表；`zxc init` 写进 zx.json 的 0.1.0 是工程版本，别一起改。
- **守门别拿 libc 名字当样本**：`backend.c libc_declared()` 会把 `sqrt/pow/floor/fabs` 等已声明原型从生成 C 里排除，钉"形参具名"要用会发射的 `cbrt/atan2/hypot`，否则断言恒假。
