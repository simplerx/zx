# zx 项目长期约定

## 不可丢弃的能力线（2026-11-08 用户确认，要求"保存这条线"）
- **LSP 行内类型提示 inlayHint**：未写类型的 `const/let/var` 绑定要在名字后显示灰色 `: Type`。Zed 侧 `inlay_hints.enabled` 一直是 `true`、`lsp.zx-lsp.binary.path` 指向 `E:/workbuddy/zx/bin/zxls`，所以能力完全由 zxls 决定，Zed 只渲染。hover 卡片同理（内容来自 `textDocument/hover` 响应）。
- 用户记忆里"以前做过、后来那条线删了"：审计日志显示 Zed 从 01:03 起就持续发 `textDocument/inlayHint`，旧 exe 无能力；本地 `bin/zxls.exe` 被反复覆盖、源码若不进版本库就找不回来了。因此这条线以 **git commit + 守门用例** 的形式保存，定位：`git log --oneline --grep=inlayHint`。
- 三处配套、缺一不可：`inlayHintProvider:true` capability 声明、`textDocument/inlayHint` 请求分派、`inlayHintsAt`（复用 `inferExprType` 推断，跳过显式类型/容器/fn/module/注释/字符串）。源码里有 `KEEP` 标记注释。
- 守门：`tools/lsptest.py` 3n/3o/3p 共 19 条 inlayHint 用例 + 3/3b/3c/3d 共 4 条补全字段类型用例 + 3q 共 8 条 snippet 用例，全套 64 项；`sh test.sh` 会从源码重编 zxls 再跑。实现被删会连挂，**不许删用例来变绿**。
- 同属这条类型提示线的还有：hover 用真实关键字（`var/let/const/pub const`）、容器声明贴完整定义、形参/字段/`self.*` 类型推断、成员表达式右值推断、内联 struct 形参字段推断。
- 调用处参数名 inlay（kind=2）：`push(info: .{…})` / `add(a: 1, b: 2)`，覆盖本文件函数、模块函数、实例方法、struct 形参与匿名 struct 实参；排除 fn 定义/控制流/注释/字符串/cast 前缀。匿名 struct 绑定 `let anon = .{ .x = 1 }` 推成 `struct{x:i32}`（spec/decisions.md §33 无期望类型的情形）。
- **用户 02:15 拍板**：struct / 匿名 struct **字面量字段的类型不做行内 inlay**（`.age: i32 = 10` 太长、太吵），改为「写的时候给提示」——补全弹窗里字段候选右侧 detail 显示**字段声明类型**（`age i32` / `name []const u8`），不是容器名 `Person`。实现：`Decl.ty` 带类型、`readFieldTy` 读到顶层分隔符（括号/花括号/方括号内的逗号不截断，`{f64, f64}` 完整保留）、`addItem` 用 `dcl.ty` 当 detail。覆盖命名容器字面量、续字段、匿名 struct 实参、实例成员补全。
- **用户 02:47 要求的代码片段补全**：写 `if` / `fn` + Tab 展开固定骨架，块尾必须带 `;`，块内换行缩进 4 空格。实现为 LSP snippet（`insertTextFormat:2`、kind=15、`$1/${1:默认值}/$0` tabstop），Zed 源码 `snippet::Snippet::parse` 明确支持。缩进写死 4 空格且**不设 `insertTextMode`**——设了 adjustIndentation 客户端会二次加缩进变 8 空格。关键字同名项替换普通关键字补全；`main`/`println`/`test`/`ifelse` 是额外模板，必须放在 `addIdents` **之前**否则会被同名标识符的 seen 去重挡掉。
- 明确不做：字段值/字段名的行内 inlay；`std.println` 等编译器内建没有可读签名，不插参数名提示。

## 工程习惯 / 踩过的坑
- 改 `tools/zxls.zig` 后必须确认 `bin/zxls.exe` **真的被替换**：Zed 锁 exe 会让 `build.sh` skip；用 `mv bin/zxls.exe bin/zxls-bak.exe` 绕锁再 `zig build-exe ... -femit-bin=bin/zxls.exe`，skip 一律当失败处理。替换后用户需重启 Zed 重新协商 capabilities。
- C 源码行尾要保持原样：`src/main.c`、`src/parse.c`、`src/sema.c` 是 CRLF；`src/errors.c`、`tools/lsptest.py` 是 LF；`tools/zxls.zig` 是 CRLF。Python 文本读写会把 CRLF 变 LF，git diff 会炸成上万行——用二进制读写或保留 newline。
- 控制台输出（进度行、诊断、help、usage、explain）只用 ASCII；中文只留在注释和写进文件的模板（如 init 生成的 README）里。GBK 控制台把 UTF-8 的 `—`/`…` 渲染成乱码。
- 验证习惯：LSP 改动跑 `ZXLS_BIN=<exe> python tools/lsptest.py`（要放独立目录、旁边有 std/，直接指 .tmp 会有环境假 FAIL）；编译器改动跑 `sh test.sh` 全量回归。用户偏好用实际运行验证交付。
- 团队另有并行任务做「功能组件化」，改动尽量限定在自己的文件，提交时精确指定路径，不捎带别人的工作区改动。

## 2026-11-09 压测期新钉的约定
- **if 剪枝**：条件是编译期常量时 sema 只检查会走的那一支，结论存 `n->ival`
  （`n->a->ct` 到后端常已被清空）。后端 `gen_if_body` 必须用
  `ct ? ct->bval : n->ival`，只看 ct 会"两条臂都生成"→ B1001。死分支里的标识符
  由 `mark_uses` 标 used（按名字现查，节点上还没有 sym），否则假报 E0410。
- **zxls 文档表**：`docs` 与 `dirty` 的键共用同一份 `doc.path`。换文档/关文档时
  **先** `dirty.fetchRemove(path)` 再 free，否则脏表留悬垂键，下一次 put 扩容
  重哈希就 panic（`hash_map.zig growIfNeeded`）。诊断线程要深拷贝 Document。
- **诊断码是 `src/zxc.c` 里 `g_rules` 的 strstr 顺序决定的**：加规则要放在
  `found \``（E0301）这类笼统规则**之前**，否则帮助文案会完全不对路。
- 语言事实（写文档/示例前先记住）：`std.process.args()` 不含 argv[0]；
  `orelse return …` 不合法；没有 `anytype`；switch 模式没有区间（`'0'...'9'`）；
  `if (c) { x } else { y }` 这种块臂不给值（要 `lbl: x;` 收割）；`&数组` 报 E0403。
- 守门：`demo/snake.zx --sim` 已进 test.sh（300 局不变式）；lsptest 的
  `completion.dupOpen.alive` 守脏表悬垂键。

## 数组模型（2026-11-10 用户确认，写文档前必看）
- **Zx 没有"数组"这种值类型**：`*[N]T` 就是**指针**（`TY_PTR` + `has_len`，
  `ty_size` 走 `TY_PTR → 8`），长度只写在类型里，用来做编译期界检查与 `.len`。
  C 后端把它发成 `T *`。`[]T` 是 `{ptr, len}` 16 字节。
- 拿存储：`std.stackAlloc(T, N)`（栈，返回 `*[N]T`，`var` 绑定可写）、
  `std.heapAlloc(alloc, T, N)`（堆）；`&.{ … }` 只是"匿名存储 + 指针"的糖。
- `&xs`（xs 为 `*[N]T`）**合法**，得到 `**(…)`：变量本身是指针槽。
  以前 sema 里那条"数组内联存储、&数组报 E0403"是错的，已删；
  后端对字面量初始化也改成 `T x_lit[N] = {…}; T *x = x_lit;`（原先直接声明成
  C 数组，导致 `&xs` 变 `int(*)[4]`）。守门用例 tests/good/addr-array.zx。
- 文档：docs/guide.md 一直是对的；docs/manual.md §3.2 曾把 `*[N]T` 写成
  `N*sizeof(T)` 的"数组本身"，已按本模型重写。写手册/README 别再写"内联数组"。
- **表述红线（2026-11-10 用户再次强调）**：`&xs` 是**取指针变量的地址**（得
  `**T`），不是"数组的地址"——Zx 里根本没有数组变量，只有指针。设计目的就是
  划清 C 里数组/指针衰减的混淆。写文档不许出现"数组的地址"这种说法。

## 内建只有九个（2026-11-10 03:xx 用户拍板，1.2.0）
- 只留：`@sizeOf @alignOf @typeName @memcpy @memmove @memset @link @compileError @trap`。
- **转换只有 `(T)val`**：@intCast/@ptrCast/@bitCast/@as 全删；castable 表本来就覆盖
  窄化/扩宽/整数浮点/指针互转/指针整数互转。别再往手册里加 cast 内建。
- 反射只有名字/大小/对齐；@typeInfo/@field*@hasField/@offsetOf/@typeOf/@embedFile
  与位置内建（@file/@line/@col/@fnName）全删。字段偏移用 `((usize)&v.f)-((usize)&v)`。
- 老名字报 **E0211**（src/sema.c check_builtin 末尾的 gone[] 表 + zxc.c g_rules 的
  `"was removed:"` 规则 + errors.c 条目），提示里直接给替代写法。
- std.debug.panic/assert 的位置实参改为 debug_inject_loc 现造字面量节点
  （NK_STR/NK_INT），不再合成位置内建。
- std.TypeKind/TypeInfo/FieldInfo 连 sema_ty_ct_only/typeinfo_value/typekind_tag
  一起删除；zxls 的内建补全表也只剩九个（含 @link）。
- 守门：tests/good/casts-one-way.zx（(T)v 全场景）、tests/bad/removed-*.zx（4 个 E0211）。

## 补全命名空间三分 + 可选值解包（2026-11-10 用户："Task 命名空间污染了"）
- zxls 点补全按 **RecvKind{module, ty, instance}** 分开（2026-11-10 用户二次确认后
  改成**彻底分离**：类型与实例的列表不许有交集）：
  - `Task.` / `std.BufReader.` / `HashMap(i32).` → 只出关联函数（首参不是 self）、
    嵌套类型、关联常量、enum 变体；**不出方法**（`Task.spawn`/`await` 只在实例上）、
    **不出实例字段**（`Task.id` 是 E0300）。代价：`Group.` 空列表、`HashMap.` 只剩
    init/norm——用户明确接受（"Task 空间上也是这些"就是嫌重复）。
  - `t.` / `g.` / `v.` → 只出字段、方法、关联常量；**不出静态构造器**（`t.coro()` 是
    E0303）与嵌套类型（`t.Group` 是 E0304）。
  - 实例 detail **必须保留首个 self 形参**（`spawn(self: *const Task, f: …, ctx: usize)`）：
    摘掉以后和关联函数长得一模一样，用户两次截图当成污染。`Self` 统一换容器真名
    （appendSelfSub，形参与返回类型共用）；骨架里仍不插 self。
  - 判据：`sigOfStmt` 首参是否 `self`；字面量上下文（`Person{.`）不过滤，那里只能写字段。
  - 链式接收者要再分：末位被调名本身是容器名（`HashMap(i32)`）才是类型，`f(…).` 是值。
- **可选值 `?T`**：`arena.` 只提示 `?`（补成 `arena.?`），`arena.?.` 才出内层成员。
  `recvOptionalTy` 负责识别；`recvExprStart` 要吞 `.?`（`?` 不是标识符字符），
  `inferContainer` 开头剥 `.?`。`if (x) v {` 的捕获绑定由 `captureBindTy` 推。
- **内建类型 Allocator** 在源文件里没有声明：`builtinTyMembers` 硬编码 ctx/alloc/free，
  与 src/sema.c 的 sema_allocator_type 对齐；`isBuiltinTy` 让 inferContainer 认
  `init(...) ?Allocator`，否则会退回接收者 ArenaAllocator（弹出编译不过的 init/reset/deinit）。
- `std.heap.page` 是**值**（Allocator），不是命名空间：`std.heap.page.` 不能走 std_ns_table。
- 守门（lsptest 217 项）：completion.optional.gate / instance.optionalRet /
  builtin.allocatorMembers / typePath.noField / chain.genericInstField /
  stdInstance.staticSnippet（反转成"实例无静态构造器"）。
- 关联常量/嵌套类型已修（05:xx）：`static_member_ty()` 把 `c.K` 折成值；
  后端物化静态成员时跳过 TY_TYPE（`const Inner = struct{…}`）；类型成员把
  `dflt->type_val` 挂到节点上（`O.Inner{…}` 才能构造）；静态切片常量显式发
  `{ptr,len}`。守门 tests/good/assoc-const.zx。
- **列成员声明只能在自己容器体里找**（`findMemberStmt`）：整份文件按名字
  findDeclStmt 会让同名成员串签名（Task.spawn / Group.spawn 等 8 组），
  `t.spawn` 曾显示成 Group 的 `… bool`，看着像"实例上冒出关联函数"。
  守门 lsptest `completion.instance.sameNameMember`。
- **文档范围（用户 2026-11-10 两次澄清后的最终裁定）**：删的是**手册与离线
  文档站**——manual.md / guide.md / site/ / tools/mksite.py 与 test.sh 的离线站
  检查，理由是 std 还没定型、写出来就是错的。**错误码参考 docs/errors.md 保留**，
  `zxc explain` 的 `details:` 链接、zxls 的 codeDescription、test.sh 的
  「errors.md 覆盖全部码 / 每码有锚点 / C-Zig 地址漂移」三段也都保留（用户：
  "你那些打印提示的可以留着"）。lsptest 的 `diagnostics.codeDescription` 守
  `href` 以 `docs/errors.md#e0304` 结尾。README 里写明"std/ 源码 + 补全就是文档"。
  以后重写手册是独立任务，别把 errors.md 一起删。
