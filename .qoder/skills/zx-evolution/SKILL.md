---
name: zx-evolution
description: Plan, implement, validate, and evolve the self-hosted Zx language repository. Use for Zx language or compiler changes, standard-library APIs, runtime and task fixes, LSP/editor work, performance optimization, refactoring, diagnostics, roadmap items, and release preparation; requires reproduced evidence, minimal changes, dual-frontend parity, durable guard tests, and lessons learned.
---

# Zx 有序演进

## 使命

把每次 Zx 升级变成可复现、可验证、可回溯的能力线。优先修真实断点，再考虑扩展；用守门用例保存行为，用设计裁定保存理由，用本 Skill 保存跨任务经验。

## 事实优先级

出现冲突时按以下顺序判断：

1. 当前工作树、源码与实际二进制行为。
2. 当前测试和可复现探针。
3. 当前 `README.md`、构建脚本与 CI。
4. 本地 `spec/decisions.md`、`docs/ROADMAP.md` 与相关规范。
5. `references/lessons.md` 和 `.workbuddy/memory/MEMORY.md`。

`docs/`、`spec/`、`.workbuddy/` 是本地维护内容，可能未入库或已过期；不得把记忆当成现状。先读 [项目地图](references/project-map.md)，再按需读 [子系统工作流](references/workflows.md) 与 [经验库](references/lessons.md)。

## 开始每轮升级

1. 保存现场基线：
   ```sh
   git status --short
   git diff --stat
   git diff
   git log -12 --oneline
   ```
   识别用户未提交的源码、测试和二进制改动。不得覆盖、回退、清理或把它们混入本轮改动。
2. 阅读当前路线图、相关设计裁定和最近提交。用 `git log --grep` 查历史能力线，避免重复实现已修问题。
3. 明确本轮契约：用户目标、验收行为、涉及子系统、非目标、兼容性是否允许破坏。需求含糊时先给候选方案和一个关键问题，不要直接扩大范围。
4. 建立最小复现与基线。记录修复前输出、退出码、生成 C、耗时或协议行为；性能问题必须先量化。
5. 先写失败守门，再改实现。无法稳定复现时，先找证据，不凭猜测改代码。

## 选择变更路径

| 变更 | 必读 | 必须覆盖 |
| --- | --- | --- |
| 词法、语法、AST | `references/workflows.md` 的双前端规则 | C 前端、Zx 前端、AST/词法/错误路径对拍、fmt |
| 类型、泛型、 comptime、反射 | 类型身份与编译期规则 | 正反例、ct-only 边界、跨模块/泛型实例 |
| 内存安全、tag、捕获、逃逸 | tag 与借用工作流 | 开关默认态、分支合并、传参/返回/字段出口 |
| 后端与 codegen | `src/backend.c`、`src/ty.c` | `--emit-c` 形状、dev/release、实跑、跨目标 |
| 标准库 | `std/std.zx` 与目标模块 | 公开/私有边界、分配/OOM、双前端、文档 |
| task/运行时 | `references/workflows.md` | 调度不变量、并发/取消、Windows/POSIX、压力 |
| 驱动、增量、工程、平台 | `src/main.c`、`src/platform.c` | CLI、缓存隔离、相对路径、多目标子进程 |
| 诊断 | 诊断工作流 | 专用码、修复建议、双前端首码一致、码集守门 |
| LSP/编辑器 | LSP 工作流 | 真实协议、异步竞态、中间态、影子文件、二进制加载 |
| 性能 | 对应 benchmark 与基线 | 修复前后同机数据、正确性不变、负伸缩/惊群检查 |
| 发版 | 版本工作流 | 三处版本、全量/压力、产物与兼容性说明 |

## 实施纪律

- 一次只解决一条能力线；把顺带发现登记为后续候选，除非它是本轮修复的必要组成部分。
- 先找不变量与根因，再做最小修改。禁止用放宽检查、吞掉异常、删除守门或改成假绿来绕过失败。
- 遵守语言既有设计：无 GC、无 trait/重载/子类型/异常系统；不引入第二套反射入口、隐式分配或与现有类型哲学冲突的语法。
- 新增共享语义声明只进 `src/sema_internal.h`；长函数拆分保持机械等价。
- 每次行为变更都审计受影响符号、节点、常量和错误码周边的注释、静态断言与测试断言；更新已过时的内容，直接删除失去意义的旧注释，不留“已删除/曾经如此”的墓碑说明。
- 断言不再描述现行不变量时，先判断该不变量是否真的被设计撤销；更新为等价或更强断言，只有确认断言目标消失时才删除，禁止为变绿而删。
- 新 AST 节点追加到 `src/ast.h` 与 `bootstrap/parse.zx` 的 NK 枚举末尾，保持编号同序。
- 新编译器内部分配优先用 `xmalloc`/`xcalloc`/`xrealloc`/`xstrdup`；保留明确以 NULL 为契约的边界。
- 保持文件原有行尾。特别保护 `src/main.c` 的 CRLF；用 `git diff --numstat` 检查意外整文件变化。
- 不自动提交、推送、发版或修改 CI。用户明确授权后才执行这些共享状态操作。

## 守门设计

每个能力至少保存一条能在旧实现上失败、在新实现上通过的证据：

- 正向行为放 `tests/good/` 或 `tests/multi/`；需要执行时写 `// expect-output`，必要时写 `// expect-exit`。
- 拒绝行为放 `tests/bad/` 或 `tests/multibad/`；写 `// expect:`，并尽量用 `// expect-code:` 钉住错误码。
- 仅类型检查不够时，必须编译并实际运行，验证输出、退出码或生成的 C。
- 并发/时序问题至少做多轮或旧版对照；不能靠一次碰巧通过。
- 语法、生成 C、ABI、协议行为需要专门断言时，在 `test.sh` 增加对应守门段。
- 禁止删除或弱化既有守门来让本轮变绿。语义确需变化时，保留等强替代用例并写明裁定。

## 验证阶梯

从便宜到昂贵逐层推进；前一层通过后再进入下一层。

```sh
# 1. 构建三件套
sh build.sh

# 2. 单文件静态检查、生成 C、编译运行
./bin/zxc.exe tests/good/<case>.zx --check
./bin/zxc.exe tests/good/<case>.zx --emit-c
./bin/zxc.exe tests/good/<case>.zx -o .tmp/<case>.exe
./.tmp/<case>.exe

# 3. 负例与另一前端
./bin/zxc.exe tests/bad/<case>.zx --check --fe=c

# 4. 双前端对拍（test.sh 会构建对拍工具；单独运行前先构建）
./bin/zxc.exe bootstrap/astdump.zx -o bin/astdump.exe
./bin/zxc.exe bootstrap/lexdump.zx -o bin/lexdump.exe
sh bootstrap/diff_ast.sh
sh bootstrap/diff_lex.sh
sh bootstrap/diff_errors.sh

# 5. LSP 协议测试
ZXLS_BIN=bin/zxls.exe python tools/lsptest.py

# 6. 全量回归与压力
sh test.sh
sh tools/stress.sh
```

当前回归脚本按 Windows 的 `.exe` 与仓库内 Zig 0.17 工具链运行。不要为了绕开环境问题改坏主链；先检查工具链、文件锁、PID 临时目录和缓存。涉及 AST 缓存时补 `ZXC_NO_AST_CACHE=1 sh test.sh`；涉及 C 内存/未初始化问题时补 `ZX_CHECKED=1 sh test.sh`。

测试输出必须保留真实汇总。命令返回 0 但缺少 `all checks passed`、LSP 缺少 `SMOKE ALL PASS` 或压力测试缺少成功标记时，不得宣称通过。

## 完成标准

只有满足以下全部条件才结束本轮：

- 最小复现、根因、修改范围与验收结果对应。
- 新守门在旧实现上失败、新实现上通过；既有相关守门未弱化。
- 双前端、文档、LSP 和平台镜像中所有受影响的同步点已更新。
- 受影响注释、静态断言和测试断言已逐项复核：无过期说明，无失效断言，无为变绿而删的守门。
- 完成必要定向验证；本轮全部实现完成后再跑全量 `sh test.sh`。
- 性能改动有同机基线数据；并发改动有必要多轮验证。
- `git diff --check` 通过，差异只包含本轮相关文件，未覆盖用户现场。
- 无法执行的验证明确标注；尤其不能把协议测试通过等同于真实编辑器 UI 已验证。
- 执行下面的自我提升检查，并向用户报告改动、证据、未验项和下一步。

## 自我提升闭环

每轮结束前判断是否产生了可复用知识。命中任一项就更新本 Skill：

- 新子系统或架构边界。
- 非显然根因、失败模式、假绿原因或用户纠偏。
- 新红线、不变量、测试纪律或命令变化。
- 现有规则被事实证明不完整或相互矛盾。
- 代码演进暴露出过时注释、静态断言或测试断言，形成新的同步检查方法。

更新方式：

1. 把稳定规则写回 `references/workflows.md`；把带日期与证据的根因写进 `references/lessons.md`。
2. 结构或入口变化时更新 `references/project-map.md`。
3. 新规则必须引用源码、测试、设计裁定或实测输出；没有证据的猜测标记为“待验证”，不得升格为红线。
4. 新规则推翻旧规则时，写明 `取代` 与原因，并删除旧规则中的现行指令，避免两个真相。
5. 不复制整份设计文档；只保留触发条件、判断顺序、禁止事项和验证入口。
6. 每五条新经验或每次发版前，合并重复规则，清理过期路径与未验证断言。
7. 运行：
   ```sh
   sh .qoder/skills/zx-evolution/scripts/check-skill.sh
   ```

自我提升不是修改产品代码，也不是自动提交。它只让下一轮升级从已验证的经验开始，而不是重新踩同一个坑。
