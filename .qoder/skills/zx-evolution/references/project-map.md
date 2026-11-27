# Zx 项目地图

本文件只放稳定导航。版本、当前路线图和未完成项必须从仓库实时读取。

## 系统全貌

Zx 是面向系统编程的自举语言：C 驱动负责 CLI、工程构建与代码生成；默认前端和标准库由 Zx 编写；后端把已检查的 AST 翻译为可读 C，再交给 Zig 0.17 的 `cc`。编译期内存安全、单一 `Task` 并发抽象和显式分配是核心设计约束。

```text
.zx 源码
  -> bootstrap/zxfe.zx（默认 Zx 前端；--fe=c 使用 C 前端）
  -> src/astload.c（重建统一 Node 树）
  -> src/sema*.c（类型、泛型、comptime、内存安全）
  -> src/backend.c（生成 C）
  -> .toolchain/zig cc（编译/链接）
  -> 可执行文件

编辑体验：tools/zxls.zig -> 调用 bin/zxc.exe 检查 -> LSP 客户端
语法高亮：editor/tree-sitter-zx + editor/zed
```

## 入口索引

| 路径 | 职责 | 何时读 |
| --- | --- | --- |
| `README.md` | 定位、命令、布局、性能与发行说明 | 每轮开始、发版 |
| `build.sh` | 构建 `zxc`、自举 `zxfe`、`zxls` | 任何实现改动后 |
| `test.sh` | 文件回归、CLI、双前端、平台与专项守门 | 添加守门、全量验证 |
| `tools/runner.c` | 正反例发现、输出/退出码断言、超时与计时 | 理解测试协议 |
| `tools/stress.sh` | 多轮回归、fuzz、LSP、数值、并发构建 | 运行时、性能、发版 |
| `.github/workflows/ci.yml` | 云端守门与工具版本 | 确认 CI 覆盖；修改需授权 |
| `src/zxc.h` | 共享定义、版本唯一源、诊断与分配契约 | 类型/驱动/诊断改动 |
| `src/zxc.c` | 核心定义与诊断码规则表 | 新增或调整诊断 |
| `src/main.c` | CLI、工程清单、依赖递归、增量构建、fmt、测试驱动 | 驱动/格式化/平台/依赖改动 |
| `src/cbind.c`、`src/csrc.c`、`src/clib.c` | C 绑定生成、C 源增量编译、`.clib` 清单 | C 互操作与工程输入改动 |
| `src/fe.c` | 前端选择与 `zxfe` 进程桥 | 自举、诊断、进程问题 |
| `src/astload.c` | 序列化 AST 装载 | AST 协议变化 |
| `src/lex.c`、`src/parse.c` | C 参考前端 | 词法/语法改动 |
| `bootstrap/lex.zx`、`bootstrap/parse.zx` | 默认 Zx 前端 | 词法/语法改动 |
| `bootstrap/zxfe.zx` | 自举前端驱动与 AST 序列化 | 自举链与前端诊断 |
| `src/ast.h` | Node 结构与编号 | 新节点、AST 分析 |
| `src/ty.c`、`src/val.c` | 类型系统与编译期值 | 类型、布局、常量折叠 |
| `src/sema.c` | 语义统一入口与模块拓扑 | 理解 sema 全链路 |
| `src/sema_internal.h` | sema 唯一共享声明入口 | 跨 sema TU 修改 |
| `src/sema_core.c` | 核心类型/符号/内建与反射 | 类型、内建、反射 |
| `src/sema_expr.c`、`src/sema_stmt.c` | 表达式与语句检查 | 语义规则 |
| `src/sema_call.c`、`src/sema_ct.c` | 调用、内建、泛型与 comptime | 调用/泛型/编译期 |
| `src/sema_flow.c`、`src/sema_region.c` | 控制流与区域分析 | 分支、逃逸、区域 |
| `src/sema_tag.c`、`src/sema_tagflow.c` | tag 生命周期账本 | 内存安全 |
| `src/backend.c` | C 发射与内嵌运行时 | codegen、task、ABI |
| `src/platform.c`、`src/platform_std/` | 目标预设与平台 std 镜像 | 跨平台 |
| `src/errors.c` | 用户可读错误码说明 | 诊断与 `zxc explain` |
| `std/std.zx` | 标准库公开门面 | 新增/调整 std 模块 |
| `std/task.zx` | 统一任务 API | 并发语义 |
| `tools/zxls.zig` | LSP over stdio | 补全、诊断、跳转、inlay |
| `tools/lsptest.py`、`tools/lspstress.py` | LSP 协议与压力测试 | 编辑器/服务器改动 |
| `editor/tree-sitter-zx/` | 语法与生成物 | 新语法、高亮、折叠 |
| `editor/zed/` | Zed 扩展资产与接线 | 编辑器集成 |
| `tests/good/`、`tests/bad/` | 自动发现的正反例 | 所有语言行为改动 |
| `tests/multi/`、`tests/multibad/` | 多文件正反例 | 模块、可见性、跨模块 |
| `spec/examples/` | 可执行规范样例 | 语法与语义变化 |
| `demo/`、`bench/` | 真实压力与性能基线 | 综合验证、性能 |
| `docs/ROADMAP.md` | 当前改进候选与优先级 | 选择升级任务；本地文件 |
| `docs/reference.md`、`docs/manual.md`、`docs/language.md`、`docs/errors.md` | 用户文档 | 行为/API/诊断变化；本地文件 |
| `spec/decisions.md`、`spec/zx-spec.md`、`spec/bootstrap.md` | 设计理由、语言规范、自举协议 | 改语义前；本地文件 |
| `.workbuddy/memory/MEMORY.md` | 长期约定与踩坑索引 | 仅作线索，必须复核；本地文件 |

## 测试布局协议

- `tests/good/*.zx`：必须通过；有完整入口和 `// expect-output` 时编译并实跑。
- `tests/bad/*.zx`：必须失败；`// expect:` 匹配诊断，`// expect-code:` 匹配错误码，`// expect-count:` 限定条数。
- `tests/multi/*/main.zx` 与 `tests/multibad/*/main.zx`：多文件版本的同类协议。
- `src/test/*.zx`：`zxc test` 的工程内测试，零参非泛型 `pub` 函数为用例。
- `test.sh`：除 runner 外还承载生成 C、CLI、双前端、平台与静态不变量守门。
- LSP 不在 `test.sh` 主链中；必须单独运行 `tools/lsptest.py`，压力场景加 `tools/lspstress.py`。

## 本地文档与入库边界

`.gitignore` 排除了 `docs/`、`spec/` 和 `.workbuddy/`；`.qoder/` 未被忽略。若本地设计文档缺失，以源码、测试和 Git 历史为准，不伪造历史裁定。当前 `.qoder/skills/` 可随项目保存；是否提交由用户决定。

## 维护地图的触发条件

只在入口移动、模块拆分、构建/测试协议变化或发行模型变化时更新本文件。不要记录每次 bug 的临时文件名、当前二进制时间或本轮未提交状态。
