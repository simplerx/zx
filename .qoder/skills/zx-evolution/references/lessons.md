# 经验库

每条经验必须可复核。日期是发现日；规则被后续事实证明错误时，重写为当前有效版本并注明取代关系。不要在这里保存临时探针文件名或当前未提交状态。

## 2026-11-21 — 数组与指针表示不能靠猜

- 证据：`spec/decisions.md` §201、§205；`src/ty.c`；`tests/good/dot-brace-array-value.zx`。
- 规则：`[N]T` 是数组值，`*[N]T` 是指向整个数组的普通指针；`p[0]` 取数组，不是首元素。跨局部、全局、extern、静态初始化保持同一 C 外壳。
- 规则：`p.0[i]` / `p[0][i]` 是原数组元素的直接左值访问，发成 `(p[0]).v[i]`，不复制数组；`let copy = p.0` 才按数组值语义复制。dev 档只检查内层数组值下标，外层数组指针没有个数可查。
- 守门：数组值/指针正例、越界与转换负例、`--emit-c` 的直接访问形状、实跑读写输出。

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

## 2026-11-27 — 栈来源与捕获借用只保留一套标签账本

- 证据：`spec/decisions.md` §275；`src/sema_tag.c` 的 `tag_stack_new`；`tests/bad/capture-borrow-args.zx`；`tests/good/stack-tags-disabled.zx`。
- 规则：局部存储、栈分配、栈临时值和捕获借用都建 `TagSlot`；符号的 `storage_tag` 与值 `tag_id` 必须分开。`tag.enable` 同时门控建槽和 E0503/E0512/E0532，不能另留独立栈扫描或借用布尔账本。
- 守门：if/while/switch 捕获作用域 E0512、所有调用出口 E0532、关闭开关正例、合法堆指针值捕获与标量字段返回。

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

- 证据：`.workbuddy/memory/MEMORY.md`；`docs/ROADMAP.md` 的分配扫描假红记录；`test.sh` 的 PID 临时目录与缓存设置；Windows Store 占位 Python 以退出码 49 制造空 LSP 日志。
- 规则：先确认 Python 占位符、Zig 版本/缓存、Windows 文件锁、PID 残留和相对路径。测试失败不自动等于产品回归；基线对照必须使用可运行的解释器与临时目录，不覆盖当前二进制。
- 守门：保留失败日志与实际退出码；修复环境后原探针必须得到确定结果；基线对照使用临时目录，不覆盖当前二进制。

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

## 2026-11-27 — 统一栈账本要按值身份传播，不能按背存猜测

- 证据：`spec/decisions.md` §275；`src/sema_flow.c` 的动态 `for`；`src/sema_tag.c` 的 `tag_for_element_ftags`；`tests/bad/for-element-mixed-tags.zx`；`tests/bad/reflection-field-ptr-escape.zx`。
- 规则：数组/切片动态迭代继承元素值和字段来源，不能把背存标签当成每个指针元素的标签；一致才继承，混合或未知必须报 E0521。反射描述符克隆会清掉 `sym`，实例存储标签必须在克隆上显式保留。
- 守门：同来源堆指针循环放行，局部字段返回 E0503，混合元素只报 E0521，反射字段指针返回 E0503；分支合流、整数 cast、格式串、聚合解包和数组字面量各保留一条借用逃逸负例。

## 2026-11-27 — 接收者转换要限定在点调用与不可变方向

- 证据：`spec/decisions.md` §274；`tests/good/method-receiver-coerce.zx`；`tests/bad/method-const-pointer-mutate.zx`；`src/sema_call.c`。
- 规则：值可自动取只读地址，裸聚合指针可自动 `.0` 后按值传；`*const Self` 不得转成 `*Self`。只改点方法调用，显式 `Type.method` 与普通 coercion 不跟随放宽；struct/union/enum 使用同一矩阵。
- 守门：旧实现指针按值报 E0301、枚举只读指针报 E0300；新实现双前端实跑、生成 C 的 `&`/`(*p)` 形状、const 可变方法负例同时通过。

## 2026-11-28 — 推测检查不建运行期账本，诊断后节点可能没有类型

- 证据：`src/sema_ct.c` 的克隆体 `check_block`；`src/sema_tag.c` 的 `g_ct_checking` 门控；`tests/good/tag-crossfn.zx`；`tests/bad/tag-undefend.zx`；`tests/bad/no-len.zx` 与 `no-size-bare-ptr.zx`。
- 规则：编译期推测检查只验证克隆体，不能创建运行期 `TagSlot`；否则工厂分配会被克隆体重复建槽，终扫误报泄漏。字段诊断返回的节点可能没有 `ty`/`sym`，标签传播必须容忍半成品 AST。E0528 已说明未确认释放按未释放合流，就必须恢复该臂状态，不能继续级联 E0515。
- 守门：跨函数分配无泄漏；三个原重复诊断用例恢复精确计数；裸指针 `.len`/`.size` 只报一次 E0305 并正常退出。

## 2026-11-28 — 动态循环继承槽状态，切片选择必须保留下界

- 证据：`src/sema_tag.c` 的 `tag_for_element_ftags`；`tests/bad/for-element-freed-heap-pointer.zx`；`tests/good/for-element-slice-offset.zx`；`tests/bad/for-element-slice-mixed-tags.zx`。
- 规则：动态元素来源一致后仍须从槽读取当前状态，不能把非作用域释放当成 LIVE。剥离 `NK_SLICE` 时累加常量下界，字段账与数组群都按 `[offset, offset + len)` 选择；否则非零下界切片会拿原数组下标误比切片长度并误报 E0521。
- 守门：普通 free 后动态循环报 E0512；非零下界、切片外含不同标签的数组只放行切片内同源元素；切片内混合来源仍报 E0521。

## 2026-11-28 — 工程依赖别名的模块上下文与真实来源必须一起传递

- 证据：`spec/decisions.md` §276；`tools/zxls.zig` 的 `containerAliasOf`/`resolvedSrcPath`；`tools/lsptest.py` 的 projectTransitive、projectRelativeAlias、projectRelativeUnion/Enum 与 definition.projectRelativeAlias。
- 规则：`pub const Alias = other.Type` 展开时以别名所在文件为 Document 解析 `@mod`；不能拿消费者目录猜相对路径。解析结果同时携带文本、真实成员名和来源路径，成员跳转不得回退到消费者文件或按末段同名声明搜索。别名接收者使用实例命名空间，直接容器保持类型命名空间。
- 守门：旧二进制直接别名补全失败；新二进制覆盖传递依赖、裸别名链、相对导入、struct/union/enum，并以真实 URI 和精确声明偏移验证跳转，最终输出 `SMOKE ALL PASS`。

## 2026-11-28 — 固定入口名与符号转义表必须一致

- 证据：`spec/decisions.md` §277；`src/backend.c` 的 `c_keyword`/`gen_fn`；`tests/good/main-fn-alias.zx`。
- 规则：入口定义固定生成 C `main` 时，函数值引用与别名不得被转义成 `main_`。仅 `check` 通过不足以发现 codegen 符号断裂，必须保存编译并运行的守门。
- 守门：`const main = fn () i32 { return 0; }; pub const start = main;` 在旧实现构建报 B1001，新实现编译运行退出 0。

## 2026-11-28 — 模块绑定为空不是字段回退信号

- 证据：`spec/decisions.md` §278；`tools/zxls.zig` 的补全模块分支；`tools/lsptest.py` 的 `completion.moduleFieldCollision.*`。
- 规则：`const ball = @mod("ball")` 等绑定一旦识别就固定为模块接收者；模块无公开成员时返回空列表，不能继续用 `namedTypeNear` 把同名字段 `ball: BallJoint` 当成接收者。别名扫描还要检查标识符边界，避免前缀串名。
- 守门：有成员模块与空模块分别验证；旧二进制在空模块同名字段守门上补出 `BallJoint` 字段，新二进制不补且输出 `SMOKE ALL PASS`；多文件正例用 `ball: *BallJoint` 与模块别名共存，双前端精确解析模块成员。

## 长期 — 不能删守门变绿

- 证据：`spec/decisions.md` 的能力线记录；`.workbuddy/memory/MEMORY.md` 的元规则。
- 规则：能力线由实现与守门共同构成。行为确需变化时用等强或更强守门替代，并写清设计理由。
- 守门：新守门在旧实现失败；相关旧守门在全量回归继续通过。
