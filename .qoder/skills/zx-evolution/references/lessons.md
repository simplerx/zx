# 经验库

每条经验必须可复核。日期是发现日；规则被后续事实证明错误时，重写为当前有效版本并注明取代关系。不要在这里保存临时探针文件名或当前未提交状态。

## 2026-11-21 — 数组与指针表示不能靠猜

- 证据：`spec/decisions.md` §201、§205；`src/ty.c`；`tests/good/dot-brace-array-value.zx`。
- 规则：`[N]T` 是数组值，`*[N]T` 是指向整个数组的普通指针；`p[0]` 取数组，不是首元素。跨局部、全局、extern、静态初始化保持同一 C 外壳。
- 守门：数组值/指针正例、越界与转换负例、`--emit-c` 形状、实跑输出。

## 2026-11-21 — 自举修复必须重建对拍工具

- 证据：`test.sh` 的双前端对拍段；`spec/decisions.md` §201、§252。
- 规则：旧 `astdump`/`lexdump` 内嵌旧前端，会把新语法误判为差异。每次涉及前端的改动都重建工具，不能直接复用旧二进制。
- 守门：AST 含位置一致、token 一致、bad/multibad 首诊断码一致。

## 2026-11-24 — 堆分配失败交调用方，不 abort

- 证据：`spec/decisions.md` §245；`tests/good/alloc-optional-oom.zx`；`test.sh` §245。
- 规则：`heapAlloc`/`heapSlice` 返回 optional，调用方解包；`stackAlloc` 不变。不得在生成运行时恢复 abort。
- 守门：恒失败分配器仍返回 null，成功路径可解包，双前端运行一致。

## 2026-11-25 — 性能方案由数据淘汰

- 证据：`spec/decisions.md` §248、§249；`tools/poolbench.zx`；`tests/good/task_fiber_pool.zx`。
- 规则：先定量再优化。更多 worker、更大栈或惰性建纤程都可能是负优化；复用前整体重置任务，挂起任务不得归还。
- 守门：不同 worker 数基线、递归/await、取消、关闭与并发压力。

## 2026-11-26 — 反射按 kind 分派，不堆通用占位字段

- 证据：`spec/decisions.md` §257；`tests/good/type-reflection.zx`；`tools/lsptest.py` 的 descriptor 用例。
- 规则：当前 `@type` 的通用字段与 `kind` 载荷分离；新增信息进入对应变体，不恢复扁平专用字段。反射值保持编译期专属。
- 守门：各 kind 载荷、运行时拒绝、LSP 变体/成员补全。

## 2026-11-26 — 捕获指针逃逸不能靠 tag 开关关闭

- 证据：`spec/decisions.md` §221、§261/§262；`tests/bad/capture-value-addr-arg.zx`；`tests/bad/capture-value-addr-return.zx`。
- 规则：区分 `in_fn`（地址落在栈帧）与 `stack_val`（值本身来自栈）；捕获值取地址必须传播到所有逃逸出口。
- 守门：传参 E0532、返回 E0503、合法堆载荷捕获不误报。

## 2026-11-26 — didChange 清空诊断必须无条件且带 version

- 证据：`spec/decisions.md` §263、§265；`tools/lsptest.py` 的 clearHasVersion、noStaleAfterRapidEdits、clearOnEveryEdit。
- 规则：异步 `has_diags` 不能作为 didChange 清空门控；清空包带 version，worker 用文本+version 双判据拒绝过期快照。批量 didOpen 仍单独防管道死锁。
- 守门：复用 version 连续 BAD/GOOD，每次编辑均收到清空，最终无陈旧诊断。

## 2026-11-27 — 未保存 buffer 不能被磁盘内容覆盖

- 证据：`spec/decisions.md` §266、§269；`tools/lsptest.py` 的 unsavedOpenDiags、unsavedBufferRespected。
- 规则：磁盘刷新以 mtime 基线而非内容差异为判据；mtime 未变时 buffer 唯一权威，quiet 保护仍保留。
- 守门：didOpen BAD 后静默，磁盘仍为旧内容时不得发布磁盘诊断；真正外部写盘后可自愈。

## 2026-11-27 — “卡死”先区分死锁与崩溃

- 证据：`spec/decisions.md` §267、§270、§270b；`tools/lsptest.py` 与协议探针。
- 规则：检查进程是否仍存活、StartTime 是否早于 exe、日志最后停在哪类请求。锁内 IO 可死锁；伪表达式进入标识符递归可栈溢出；旧进程可表现为“改了没用”。
- 守门：无编辑也醒、逐字输入、六类请求响应、进程存活、旧版/新版对照。

## 2026-11-27 — 模块成员与类型静态成员不是同一类别

- 证据：`spec/decisions.md` §270c；`tests/multi/addr-global-array/`；`src/backend.c` 的 lvalue/ex 分派。
- 规则：模块命名空间也带类型外形，后端必须先检查符号的模块成员身份，再判断静态成员；右值与左值路径保持一致。
- 守门：`&ext.arr` 编译运行、生成 C 使用模块全局符号、相关数组指针语义不回退。

## 长期 — 环境假红先于代码改错

- 证据：`.workbuddy/memory/MEMORY.md`；`docs/ROADMAP.md` 的分配扫描假红记录；`test.sh` 的 PID 临时目录与缓存设置。
- 规则：先确认 Python 占位符、Zig 版本/缓存、Windows 文件锁、PID 残留和相对路径。测试失败不自动等于产品回归。
- 守门：保留失败日志与实际退出码；修复环境后原探针必须得到确定结果。

## 2026-11-27 — 注释与断言必须随行为一起演进

- 证据：用户明确要求 Skill 在后续开发中清理过时注释与断言；`test.sh` 大量静态断言和精确集合会随实现变化失效。
- 规则：行为变更后审计受影响区域的注释、`assert`、静态断言与测试期望。现行不变量仍有效时更新为等价或更强断言；目标消失才删除。过时注释直接删除，不留墓碑说明。
- 守门：搜索旧符号/旧 API/旧文案无残留；最终差异逐项复核断言删除；新断言在旧实现上失败、新实现上通过。

## 2026-11-27 — 递归推断的 depth 必须贯穿捕获回调

- 证据：`spec/decisions.md` §271；`tools/zxls.zig` 的 `captureBindTy`/`switchCaptureTy`；`tools/lsptest.py` 的 `completion.switch.isolatedDot.noCrash`。函数参数主语使 `inferExprType("op") → switchCaptureTy → inferExprType("op", 0)` 成环，旧版 ReleaseSafe 栈溢出。
- 规则：helper 接受表达式或名字并回调推断时，必须透传 depth 并用 `depth + 1` 消耗预算；纯标识符校验只能挡伪表达式，挡不住合法标识符环。
- 守门：孤立点补全在旧进程死亡/超时、新进程返回列表；既有 switch 捕获、第二臂模式和 tag 变体补全不回退。

## 2026-11-27 — 工程编译选项不能散落在生成源码里

- 证据：`spec/decisions.md` §272；`test.sh` §2.53–2.54e；`src/main.c` 的清单 upsert/递归收集。
- 规则：`zx.json` 是编译/链接输入的唯一清单源。管理命令写清单，构建命令只读；生成绑定不写 `@link`。`csrc` 与 `libs` 相对声明工程解析，依赖递归贡献，锁仍只负责远端复现与完整性。
- 守门：本地/Git/传递 Zx 工程可导入；静态库实跑；缺库失败；重复 add 不复制/不重复；fetch 恢复 csrc；vendor 生成物无 `@link`；C/LSP 解析一致。

## 2026-11-27 — 同名模块 stem 会撞 C 单元，不能只改展示名

- 证据：`spec/decisions.md` §272；`src/sema_core.c` 的全局模块单元命名；`test.sh` §2.54e。
- 规则：不同依赖目录的同名文件必须获得全局唯一 C 单元名；仅改用户可见 stem 或依赖顺序不能避免后端互相覆盖。工程 main 与递归 deps 的解析规则需同步 LSP。
- 守门：多工程含同名模块时链接/运行正确，旧实现出现重复定义或缺失符号。

## 2026-11-27 — 隐式捕获启发式必须受臂位置约束

- 证据：`spec/decisions.md` §273；`tests/good/if-else-expression.zx`；`src/parse.c` 与 `bootstrap/parse.zx` 的 `parse_if_arm`。
- 规则：else 臂的普通表达式优先于捕获语法。`starts_value` / `starts_binop` 等启发式必须检查 `implicit_ok`；同类判定要同时审计 `i + 1` 和 `f(x)`，不能只修运算符分支。
- 守门：旧实现编译失败、新实现双前端 check 通过且 dev/release 实跑数组内容正确；check 通过但 build 报未重命名标识符时，必须检查 AST 是否多出捕获节点。

## 长期 — 不能删守门变绿

- 证据：`spec/decisions.md` 的能力线记录；`.workbuddy/memory/MEMORY.md` 的元规则。
- 规则：能力线由实现与守门共同构成。行为确需变化时用等强或更强守门替代，并写清设计理由。
- 守门：新守门在旧实现失败；相关旧守门在全量回归继续通过。
