# zx 项目长期约定

## 元规则
- 能力线以 **git commit + 守门用例** 保存；实现被删会连挂，**不许删用例来变绿**（用户明确要求）。历次定位：`git log --oneline --grep=inlayHint`。
- **`// expect-output` 两种写法 runner 都认**（2026-11-12 修）：裸标记 + 连续 `// ` 行，或 `// expect-output:一行` 多行。此前只认裸标记 → 16 个用例（全部 `task_*`/`chan_*`/`errdefer` 等）stdout 断言长期假绿，改期望仍 passed；修后文件用例 321→337。runner **合并 stdout/stderr** 比对，`printTrace()` 的 trace 两行也要写进期望，文本含**相对路径+行号** → 编译必须传相对路径，且别改用例前部行数。`// expect-` 指令行不进期望。
- 改动完成即提交、工作区干净；精确指定路径，不捎带并行任务的工作区改动。每轮只测本轮结果，不重复跑全量测试。用户偏好实际运行验证交付。
- 行尾：`src/main.c|parse.c|sema.c`、`tools/zxls.zig`、`tools/lsptest.py`、`std/std.zx` 是 CRLF；`src/errors.c`、`std/task.zx`、`docs/`、`spec/`、`README.md`、memory 是 LF。Python 文本读写会把 CRLF 变 LF，git diff 炸成上万行 → 用 bytes 二进制读写或保留 newline。
- 控制台输出（进度、诊断、help、usage、explain）只用 ASCII；中文只在注释与写进文件的模板里。GBK 控制台会把 UTF-8 的 `—`/`…` 渲成乱码。
- 改 `tools/zxls.zig` 后必须确认 `bin/zxls.exe` **真被替换**：Zed 锁 exe 会让 build skip，用 `mv bin/zxls.exe bin/zxls-bak.exe` 绕锁再 `-femit-bin=bin/zxls.exe`，skip 当失败。替换后用户需重启 Zed 重新协商 capabilities。`sh test.sh` 只构建临时 `.tmp/*.lspbin/zxls.exe`，不替换 bin。
- 验证：LSP 改动跑 `ZXLS_BIN=<exe> python tools/lsptest.py`（独立目录、旁边有 std/，直接指 .tmp 会假 FAIL）；编译器改动跑 `sh test.sh` 全量回归。

## 语言事实（写文档/示例前必看）
- **无 trait / interface / 类型类、无重载、无子类型**；`spec/decisions.md`「明确不做的事」与 `zx-spec.md` 都写死。函数指针可作 struct 字段（`cb: fn (i32) i32`），所以手写 vtable（`{ctx, fn, fn}`）技术上可行，但调用点全靠 `(T)bits` 强转、零静态检查，不是多态。
- 内建只有七个：`@type @memcpy @memmove @memset @link @compileError @trap`。`@type(x)` 是唯一反射（描述符 `Type{.name,.size,.align,.ty,.fieldCount,.fields[]}`）；老拼写报 **E0211**（sema.c `check_builtin` 的 gone[] + zxc.c g_rules 的 `"was removed:"` 规则 + errors.c）。
- **转换只有 `(T)val`**；castable 表覆盖窄化/扩宽/整数浮点/指针互转/指针整数互转。反射只有名字/大小/对齐；字段偏移用 `((usize)&v.f)-((usize)&v)`。
- 无 GC/析构/RAII/生命周期、无异常（只有 `!T`）、无协程/async、无宏、无运算符重载、无 `private`（可见性只有 `pub`）、无默认参数/变参。
- 函数定义只有 `const F = fn(…) ret {…};` —— **没有 `pub fn`**，pub 是前缀修饰符（文件作用域与容器成员同义）。
- **容器方法不能泛型（E0701）**；容器字段/方法/静态成员默认私有、只在声明文件内可见，`pub` 才对外，跨文件访问私有成员报 **E0404**；enum / union(enum) 变体恒公开。带私有字段的容器跨文件构造走 pub 构造器。实现锚点 `sema.c member_hidden/deny_member/stamp_mod`、parse.c 与 bootstrap/parse.zx 的 mem_pub；自举要先重建 zxfe，别加环境开关旁路。
- `std.process.args()` 不含 argv[0]；`orelse return …` 不合法；没有 `anytype`；switch 模式无区间（`'0'...'9'`）；`if (c) { x } else { y }` 块臂不给值（要 `lbl: x;` 收割）。
- **Zx 没有数组值类型**：`*[N]T` 就是指针（`TY_PTR`+`has_len`，`ty_size`→8），长度只写在类型里做编译期界检查与 `.len`，C 后端发 `T *`；`[]T` 是 `{ptr,len}` 16 字节。存储用 `std.stackAlloc(T,N)` / `std.heapAlloc(alloc,T,N)`；`&.{…}` 是"匿名存储+指针"的糖。`&xs`（xs 为 `*[N]T`）合法、得 `**T`。**表述红线**：这是取指针变量的地址，不是"数组的地址"——文档不许出现后者。守门 `tests/good/addr-array.zx`。
- ct_clear 的 `ct_clear_stmts` 只擦语句子树：结构体字面量里折值字段访问（`N{.a=@type(T).size}`）的 ct 绝不能擦，否则后端发 `0.size`。`std.debug.panic/assert` 的位置实参由 `debug_inject_loc` 现造 NK_STR/NK_INT 节点。

## LSP 能力线
### inlayHint / hover / snippet（2026-11-08 用户要求"保存这条线"）
- 未写类型的 `const/let/var` 绑定名字后显示灰色 `: Type`；Zed 只渲染，能力全由 zxls 决定（hover 同理）。三处配套缺一不可：`inlayHintProvider:true`、`textDocument/inlayHint` 分派、`inlayHintsAt`（复用 `inferExprType`，跳过显式类型/容器/fn/module/注释/字符串）。源码有 `KEEP` 标记。
- 同线：hover 用真实关键字（`var/let/const/pub const`）、容器声明贴完整定义、形参/字段/`self.*`/成员表达式右值/内联 struct 形参字段推断；调用处参数名 inlay（kind=2，`push(info: .{…})` / `add(a: 1, b: 2)`）。`let anon = .{ .x = 1 }` 推成 `struct{x:i32}`（§33）。
- **字面量字段类型不做行内 inlay**（`.age: i32 = 10` 太吵），改为补全候选 detail 显示字段声明类型（`age i32`）：`Decl.ty` 带类型、`readFieldTy` 读到顶层分隔符（括号内逗号不截断）、`addItem` 用 `dcl.ty`。
- **snippet**：`if`/`fn`/`main`/`println`/`test`/`ifelse` + Tab 展开骨架，块尾带 `;`，块内 4 空格缩进。LSP `insertTextFormat:2`、kind=15、`$1/${1:默认值}/$0`；缩进写死 4 空格且**不设 `insertTextMode`**（设了客户端二次加缩进变 8）。额外模板必须放在 `addIdents` **之前**，否则被 seen 去重挡掉。
- 明确不做：字段值/字段名的行内 inlay；`std.println` 等内建无可读签名，不插参数名。
- 守门：lsptest 3n/3o/3p 19 条 inlay + 3/3b/3c/3d 4 条字段类型 + 3q 8 条 snippet。

### 命名空间三分（2026-11-10 用户："Task 命名空间污染了"）
- `RecvKind{module,ty,instance}`，类型与实例列表**不许有交集**：`Task.` / `std.BufReader.` / `HashMap(i32).` 只出关联函数（首参非 self）、嵌套类型、关联常量、enum 变体；`t.` 只出字段/方法/关联常量，不出静态构造器与嵌套类型。判据 `sigOfStmt` 首参是否 self；字面量上下文 `Person{.` 不过滤。
- 实例 detail **必须保留首个 self 形参**（`spawn(self: *const Task, …)`），`Self` 统一换容器真名（`appendSelfSub`）；骨架不插 self。链式接收者：末位被调名本身是容器名才是类型，`f(…).` 是值。
- **列成员声明只能在自己容器体里找**（`findMemberStmt`）：全文 `findDeclStmt` 会让同名成员串签名（`completion.instance.sameNameMember`）。
- **静态值（关联常量/静态 const）只在类型命名空间**，enum / union(enum) 实例上也不出；变体恒公开。
- 可选值 `?T`：`arena.` 只提示 `?`，`arena.?.` 才出内层成员（`recvOptionalTy`、`recvExprStart` 要吞 `.?`、`inferContainer` 开头剥）。`if (x) v {` 捕获由 `captureBindTy` 推。
- 内建类型 Allocator 无源文件声明：`builtinTyMembers` 硬编码 ctx/alloc/free，与 `sema_allocator_type` 对齐；`isBuiltinTy` 认 `init(...) ?Allocator`。`std.heap.page` 是**值**不是命名空间。
- 关联常量/嵌套类型：`static_member_ty()` 把 `c.K` 折成值；后端物化静态成员跳过 TY_TYPE；类型成员挂 `dflt->type_val`；静态切片常量显式发 `{ptr,len}`。守门 `tests/good/assoc-const.zx`。

### 同名容器串味的两条铁律（§136，2026-11-13）
- **泛型构造函数不能提到全局最前**：`containerMembersAny` 必须按 `scanFiles`
  的文件优先级**逐文件**试，同一文件内泛型在前。全局泛型优先会让同会话别的
  文档 `const Box = fn (T: type) type` 抢走 leaklib 的普通 `Box`（挂 26 条守卫）。
- **裸名字推断只接泛型构造函数**：`completeAt` 放开单段裸名字后，若不加
  `genericBodyOf(ty0) != null` 的闸门，`vp.Cfg{…}` / `std.json.Obj{…}` /
  `m.A.B.Cfg{…}` 三类限定路径实例会被抢（挂 3 条）。限定路径实例有专门的
  链，别在推断里二次猜。
- 语义：`List = fn (N: u64) type` 只有值参数也算泛型（§135 `fn_is_generic`）；
  `var x = List(2)` 的 x 是**类型**（只出构造骨架），字段/方法要 `L{…}` 实例
  或 `var v: L` 标注。

- **fixture 容器名必须全局唯一**（§137）：`SRC_DIR` 下所有 fixture 进同一个
  `scanFiles`，`pval.zx` 里写 `P` / `List` 会直接把 instChain 的 `P.`、gval 的
  `List` 串味。新 fixture 用 `Pv*` 之类前缀。
- **形参接收者**（§137）：命名类型形参（`p: *P` / `xs: []const i32` /
  `l: *List(4)`）走 `completeAt` 兜底段的 `namedTypeNear` 分支；`namedTypeNear`
  必须挡空 needle，否则 `indexOfPos` 越界 panic（`struct{…}.` 触发整机崩溃）。

### 解析链优先级与守门会话
- `scanFiles()` 按**裸名字**找容器（`files[0]` 是当前文档，其后文档目录、std、模块目录）。限定路径必须走 `modContainerOf / resolveContainerPath` 锁模块文件，不能靠裸名兜底；`appendGenericSynthetics` 合成声明要 `insertSlice(gpa,1,…)` 排前部，否则被同名容器抢走。
- 多段路径不许"拿末段名字配容器"：`resolveContainerPath` 每段只在上一层体里 `findMemberStmt`（深度感知）。三处入口：限定类型路径、`instancePathContainer`、`isTypeValuedRecv/typeValuedContainer`。emit 用 `mem_body_fix`；hover 实例分支同源。守门 `completion.deep.*` 6 条 + `hover.deepNestField`。
- 匿名类型串派生字段是虚拟成员：`anon_only` 时 `stmt = null`，不许 `findDeclStmt` 全文配声明。
- `tools/lsptest.py` 的 8z 段用**独立短命 LSP 会话**（`p8/s8/r8`，在 `# 9 shutdown/exit` 之前）：主会话几百文档会污染扫描缓存，主进程 shutdown 后全局 `send()` 会 `OSError 22`。新守门 needle **不要带尾随 `\n`**。
- **0 项补全大多是 `.ty` 语义过滤字段，不是路径解析失败**：加个静态常量能弹出就说明路径通了，别再去改 `resolveContainerPath`。

### 两条解析链全量覆盖（§126，2026-11-12）
- **链路 A 容器嵌套类型**：`resolveContainerPathD` n==1 单段类型值别名（`const T: type = t.G.F;`）；`lit_ctx` 的 `{` 校验也认 `resolveContainerPath`；`pathNoGenerics` 让 `t.G(i32).F.` 逐段下钻；`.ty` 空列表由 `tyCtorFallback` 补 `F{...}` 构造骨架（**textEdit 吃掉刚输入的 `.`**），字段项绝不混进类型空间。
- **链路 B 形参/局部匿名嵌套类**：显式 `var x: struct{…}` 标注优先于初值推断；`paramStructTypeNear` 按花括号深度选最浅 `name:`；`selfContainerAt` 按"包含光标且跨度最小"取词法容器；`anonBindingNestedFields` 的 `= .{` 不再要求 `=` 前紧邻标识符。`genericContainerDef` 跳过形参上的 `: struct{…}`；初值推不出类型时 hover 用 `declTyOf` 兜底。
- **口径红线**：`completion.literalSecond` 钉住"具名容器字面量续补给全量字段"，**不许给 emit 循环加去重**（加过一次立刻挂）；只有匿名实参/嵌套字面量两条链去重。
- 守门：lsptest ⑩ 段 24 条（四种容器嵌套类型、三层 Leaf、泛型返回体/实例尾段、类型值别名、构造骨架、显式匿名标注、`cfg./cfg.nest./self./data./data.data.`、snippet）。

### 编辑期位置与泛型别名链（§127）
- `anonParamTy` 必须同时认 `fn (` 与紧凑 `fn(`（真实代码常写 `fn(b:i32,data:struct{…})`）；带空格 fixture 会掩盖断点造成假绿。
- `.{` 后**还没输入点号**时 Zed 就弹补全：光标前是 `{`、`dotted=false` 会落到全局标识符 → 用"虚拟点上下文" `brace_base` 把 `base_end` 指到 `{` 后一位复用实参签名链；`struct{…}.` 编辑期位置同理（点号前紧邻 `}` 才触发，别误伤 `x.`）。编译器报 E0201 的编辑期非法写法也要给字段与 snippet。
- 泛型类型值别名要跟**别名链**：`const G = t.GG; const g: type = G(T: i32);` 的 `finalCalleeOf` 只拿到 `G` → `genericCalleeOf` 沿 `const 别名 = 限定路径` 剥末段，三处调用点都要接。`appendGenericSynthetics` 只放行 `: type` 标注合成 `const g = struct{…}`，否则 `g{ .` / `var x: g; x.` 按名字找容器体是 0 项。
- 守门：lsptest ⑪ 段 13 条（8z 短命会话，⑩ 后 shutdown 前）。

## Task 并发接口（§125 / §128 / §129，2026-11-12）
- `std.task.Task.` **7 项**：4 个构造器 `coro/single/pool/fromId` + `SEL_AWAIT/SEL_RECV` + `group` 工厂（§129）。这是类型命名空间语义，**不是没实现**；别为了"好找"把实例方法塞回类型空间。用户 §125 明确：Task 是接口、生成实例后走 `t.` 分发，不是关联函数集合；不引入 vtable、不动 C 运行时。
- **§128 用户拍板**：scope/chan/timer 本想做成"都是 Task 的多态类型"，但 zx 无 interface/子类型、契约不匹配（Task 是调度器、Scope 是任务树、Chan 是端点、Timer 无独立对象）、E0701 挡住 `Chan(T)`，做不成 → 按条件**删去**，`Task` 回到紧凑接口。
- **§129（用户最终拍板）**：恢复 b620057 的历史 `Group` 实例形态 —— `Task.group()` 是工厂方法，返回独立的 `pub const Group`（`g.spawn/wait/run/close` 走 `zx_tg_*`）；Task 实例上的 5 个 `group*` 全删。这是第二个对外类型，但不是"实现 Task 接口"（zx 无 interface/子类型）。C 运行时一字未改。
- 现行 `t.` **12 项**（11 方法 + `id`）：`spawn/await/awaitAll/cancel/yield` + `send/recv/trySend/tryRecv` + `select` + `close`。`Group` 实例 `g.` 出 `spawn/wait/run/close`，hover `let g: Group = ...`、inlay `: Group` 都由 zxls 自动识别（未改 zxls 源码）。删掉的 14 个：`scope*` 6、`chan*` 6、`sleep/awaitTimeout` 2。
- 守门：`completion.stdTask` 7 项等式、`unifiedApi`/`sameNameMember`/`stdInstance`/`nest.*`/`mchain.*` 按新语义改断言，新增 `groupInstance.methods` 与 `noGroup` 反向断言，冒烟 375 → 378。
- **C 运行时一个字不改**：`zx_task_close` 遍历 `ZxScope`/`ZxChan` 停靠者链唤醒，`zx_sel_wait` 的 SEL_RECV 走 `zx_chan_recv(s,NULL,…)`，默认通道也走同一套 `zx_chan_*`；机制全被保留能力复用、按需整体发射。它们是内部实现，不是对外类型。
- `std.io` 纯同步、无隐式 offload；要并发显式 `task + io`。无静态数据竞争防护、无真异步 I/O、通道载荷统一 usize 由调用点 cast；C 侧保留机制层。
- 用例：删 `task_chan/task_scope*/task_timeout` 6 个；改写 `task_blocking`（sleep→yield，证据从"推进次数"改成"tick 有没有跑完"，因 `!T` 取消门本来会推进一次队列）、`task_select`（默认通道 `SEL_RECV,0` + 空分支 n=0 测超时）、`chan-wide`（t.send 守 E0300）。`chan_basic.zx` 是默认通道测试，保留。

## 编译器 / 后端
- **if 剪枝**：编译期常量条件 sema 只检查会走的一支，结论存 `n->ival`；后端 `gen_if_body` 用 `ct ? ct->bval : n->ival`，只看 ct 会两条臂都生成（B1001）。死分支标识符由 `mark_uses` 按名字现查标 used，否则假报 E0410。
- **zxls 文档表**：`docs` 与 `dirty` 键共用 `doc.path`。换/关文档**先** `dirty.fetchRemove(path)` 再 free，否则悬垂键在下次 put 扩容重哈希时 panic；诊断线程要深拷贝 Document。守门 `completion.dupOpen.alive`。
- **诊断码由 `src/zxc.c` 的 `g_rules` strstr 顺序决定**：新规则要放在 `found \``（E0301）这类笼统规则**之前**，否则帮助文案不对路。
- 守门：`demo/snake.zx --sim` 300 局、`tests/good/casts-one-way.zx`、`tests/bad/removed-*.zx`（4 个 E0211）、`tests/multibad/private-member`、`tests/good/addr-array.zx`。

## 句柄检查只在编译期（§135，2026-11-13 用户裁定）
- **运行时 side table 已撤掉**，`std.slot` 删除（用户：抹源靠它兜底是假安全）。`get` 能证明 LIVE 就改写成私有 `getUnchecked`（dev/release 同形态），证明不了报 **E0511**；free 二次释放 E0509、表已关闭 E0510。E0510 定位从「`.?` 强制解包」扩大到「读失效句柄」；`handle_dead_get` / `ZX_HANDLE_CHECKS` 已删。
- opt-in 契约（没有 std 库后的唯一入口）：容器有 `create` + `get` + **私有** `getUnchecked`。`get` 的函数体不进生成代码（要么被改写、要么报错）；`Handle` 类型本身不被识别，**方法名才是契约**。抹源（cast / 存字段 / 跨函数传参 / 控制流里发行）→ E0511，没有运行时候补。
- 守门：`tests/good/handle-erase.zx`（自写容器样板）、`tests/bad/handle-provenance-lost.zx`、`tests/good/generic-value-param.zx`、lsptest 的 `e0510/e0511CodeDescription`、test.sh §135 两段。
- **`fn (N: u64) type` 是泛型**：返回 `type` 的函数即类型构造函数，值参数可当数组长度（`*[N]u64`、`*[N + 1]u64`）。`fn_is_generic` 曾只认 `T: type`，是会复发的坑。

## 文档范围（用户 2026-11-10 最终裁定）
- 删的是**手册与离线文档站**（manual.md / guide.md / site/ / mksite.py 与 test.sh 离线站检查），理由 std 未定型、写出来就是错。**`docs/errors.md` 保留**，`zxc explain` 的 `details:` 链接、zxls 的 codeDescription、test.sh 的「errors.md 覆盖全部码 / 每码有锚点 / C-Zig 地址漂移」三段都保留。lsptest 的 `diagnostics.codeDescription` 守 `href` 以 `docs/errors.md#e0304` 结尾。README 写明"std/ 源码 + 补全就是文档"。以后重写手册是独立任务，别把 errors.md 一起删。
