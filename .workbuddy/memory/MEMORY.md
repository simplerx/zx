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
