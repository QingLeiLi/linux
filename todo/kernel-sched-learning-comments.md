# kernel/sched 学习注释任务清单

## 使用规则

本清单是 `kernel/sched` 目录详尽学习注释长任务的持久化进度源。发生会话中断或上下文压缩后，
先读取本文件、`doc/linux-kernel-source-learning-methodology.md` 并核对当前工作树，不凭对话记忆推断
进度。

严格采用单文件闭环：

1. 开始前把目标文件标为 `[~]`，完整读取并建立函数、实体、英文注释和语义索引。
2. 仅追加中文学习注释；不改代码，不删除、改写或移动任何原有代码与原有注释。
3. 非豁免英文注释必须保留原文，并在紧邻位置补充完整翻译、上下文和机制说明；新增注释不使用
   `中文说明：`、`背景知识：` 等只标识语言或用途的模板前缀。
4. 按源码顺序以 1～5 个函数为一批修改；每批修改前读取最新连续窗口，修改后复读窗口并检查局部
   diff，避免函数头错位或补丁覆盖原内容。
5. 完整复读修改后的目标文件，按方法论第 17 章核对函数、实体、英文注释、控制路径、并发、生命周期
   和关联读取清单，再执行追加式安全审计、格式检查及可用的构建检查。
6. 只有单文件内容验收全部通过后才标为 `[x]` 并登记验收摘要；一个 `[~]` 文件未闭环前不开始
   下一个文件。
7. 为核对契约而读取的关联源码只登记到当前文件记录；读取关联文件不等于授权修改。
8. 本清单覆盖创建时 `kernel/sched` 下的全部 51 个文件：46 个进入计划，5 个按用户要求排除。
   后续若目录新增文件，先登记到“范围变化”再决定学习顺序，不能静默遗漏。

状态：`[ ]` 未开始；`[~]` 正在处理或验收；`[x]` 已按第 17 章完成全文件闭环；`[-]` 明确排除。

## 文件进度（按建议学习顺序）

### 构建入口、公共数据结构与调度特性

- [x] `kernel/sched/Makefile`
  - 文件职责：定义调度目录的编译期上下文分析、KCOV/KCSAN、帧指针和分支跟踪边界，并把调度器
    组织为 `core.o`、`fair.o`、`build_policy.o`、`build_utility.o` 四个规模接近的内建编译单元；后两者
    通过包含多份 `.c` 摊薄头文件解析成本，父级再把 `kernel/sched/built-in.a` 链入 `vmlinux`。
  - 第 17 章验收：完整复读最终 76 行。文件没有函数、结构体、运行时控制流、ownership 或资源释放，
    函数清单和三个复杂函数抽查不适用；实体清单覆盖 2 个上下文分析目标、目录级告警/KCOV/KCSAN
    变量、2 个配置分支、3 组目标级 CFLAGS 和 4 个 `obj-y` 条目。所有非豁免英文注释均逐字保留，
    并有紧邻完整翻译及当前 Kbuild 语义补充。
  - 路径与配置验收：可仅凭注释复述 KCOV 为何排除异步覆盖噪声、KCSAN 为何关闭访存插桩却保留
    屏障建模、`SCHED_OMIT_FRAME_POINTER` 两侧如何决定 `core.o` 的帧指针参数、启用分支分析时两个
    聚合单元如何通过 `DISABLE_BRANCH_PROFILING` 避免 noinstr 不安全插桩，以及四目标的归档去向。
  - 关联读取：`scripts/Makefile.lib` 的 KCOV/KCSAN/context-analysis flag 选择，
    `scripts/Makefile.context-analysis` 与 `scripts/Makefile.compiler` 的具体编译选项和告警探测，
    `Documentation/kbuild/makefiles.rst` 与 `scripts/Makefile.build` 的 `ccflags-y`/目标 CFLAGS/`obj-y` 规则，
    `kernel/Makefile` 的 `sched/` 入口，`build_policy.c`/`build_utility.c` 的聚合内容，六个体系结构 Kconfig
    中的帧指针配置，以及 `include/linux/compiler.h` 的分支跟踪门控；均只读未修改。`core.c` 已有充分
    学习导读，`compiler.h` 所读区域部分覆盖，其余关联区域缺失或仅有上游英文；计划内聚合文件后续
    继续补注，目录外 Kbuild/Kconfig 文档不在本任务修改范围。
  - 修改安全：新增 34 行、删除 0 行；过滤注释和空行后全部 Kbuild 语句与 `HEAD` 逐行一致；禁用
    模板前缀扫描无命中，`git diff --check` 通过，完整文件 checkpatch 为 0 errors/0 warnings。工作树
    无 `.config`，未执行目标内核配置构建。已按方法论第 17 章完成强制验收，状态为“全文件完成”。
- [ ] `kernel/sched/sched.h`
- [x] `kernel/sched/smp.h`
  - 文件职责：定义通用 SMP call-single 队列与调度器之间的内部接口，使普通跨 CPU 回调和远端 task
    唤醒可以共用每 CPU `call_single_queue`，并约定目标 CPU 通过 IPI、polling idle 或 migration
    stopper 消费队列。
  - 第 17 章验收：完整复读最终 70 行。函数/声明清单覆盖 `sched_ttwu_pending(void *)`、
    `call_function_single_prep_ipi(int)`、SMP 版 `flush_smp_call_function_queue()` 外部实现和 UP 版空
    stub；4/4 均有紧邻专属契约，逐项说明参数、可空性、返回类别、中断/任务上下文、可睡眠性、
    队列节点与 task/CSD ownership、可观察副作用和配置差异。文件无结构体、全局变量或复杂函数体，
    三个复杂函数抽查不适用。
  - 英文与路径验收：许可证豁免；唯一英文总览逐字保留，并有紧邻翻译和调用链补充。仅凭注释可
    复述 TTWU 链表在目标 CPU 批量激活及延后清 pending、polling idle 通过 need_resched 省 IPI、
    非 polling 目标继续发硬件 IPI、SMP 主动 flush 的关中断/恢复与 SYNC→ASYNC/IRQ_WORK→TTWU
    次序，以及 UP 配置为何安全退化为空操作。
  - 关联读取：`kernel/sched/core.c` 的 `sched_ttwu_pending()`、
    `call_function_single_prep_ipi()` 和 migration stopper 调用点，`kernel/smp.c` 的发送、IPI 接收、
    分阶段队列消费及公开 flush 实现，`kernel/sched/idle.c` 的 polling idle 承诺和主动 flush 调用；
    均只读未修改，所读实现区域已有充分中文学习注释。`idle.c` 仍按用户要求排除，不因关联读取
    重新纳入修改范围。
  - 修改安全：新增 48 行、删除 0 行，diff 中全部新增行均位于注释，原 22 行逐行保留；禁用模板
    前缀扫描无命中，`git diff --check` 通过，完整文件 checkpatch 为 0 errors/0 warnings。工作树无
    `.config`，未执行配置构建。已按方法论第 17 章完成强制验收，状态为“全文件完成”。
- [ ] `kernel/sched/features.h`
- [x] `kernel/sched/rq-offsets.c`
  - 文件职责：作为构建期类型布局转接器，使用目标编译器把完整内部 `struct rq` 的 `nr_pinned`
    字段偏移编码进 `rq-offsets.s`，再由 Kbuild 提取为 `include/generated/rq-offsets.h`；公共
    `include/linux/sched.h` 因只持有 `struct rq` 前置声明，借此偏移访问当前 CPU 的该字段。
  - 第 17 章验收：完整复读最终 53 行。唯一函数 `main(void)` 有紧邻专属契约，明确无入参、构建期
    上下文、不会链接或运行、无锁/睡眠/ownership/运行时失败路径、返回 0 不被消费，以及真正输出是
    `DEFINE()` 汇编标记；函数体两阶段说明覆盖 `offsetof()` 常量求值、标记发布和形式返回。
  - 实体与路径验收：覆盖 `COMPILE_OFFSETS` 打破生成头循环依赖的作用、`linux/kbuild.h` 中
    `DEFINE()` 的 `.ascii` 编码、目标 ABI 类型来源、内部 `sched.h` 的完整结构布局，以及
    `rq-offsets.c → rq-offsets.s → rq-offsets.h → this_rq_pinned()` 全链路。原文件除 SPDX 外没有英文
    注释，许可证按规则豁免；文件没有结构体定义、运行时快速/慢速/回滚路径或三个复杂函数可抽查。
  - 关联读取：顶层 `Kbuild` 的生成目标和 prepare 依赖，`scripts/Makefile.lib` 的 offsets sed/filechk
    规则，`include/linux/kbuild.h` 的 `DEFINE()` 实现，`include/linux/sched.h` 的生成头门控、
    `this_rq_raw()`/`this_rq_pinned()` 与 offsets 编译 stubs，以及 `kernel/sched/sched.h` 中 `struct rq` 的
    `nr_pinned` 字段；均只读未修改。公共 sched 头所读区域已有部分学习注释，内部 `sched.h` 缺失，
    后者仍保留在本清单待处理。
  - 修改安全：新增 41 行、删除 0 行，diff 中全部新增行均位于注释；原 12 行源码逐行保留，禁用模板
    前缀扫描无命中，`git diff --check` 通过，完整文件 checkpatch 为 0 errors/0 warnings。工作树无
    `.config`，未执行目标架构的 offsets 重新生成。已按方法论第 17 章完成强制验收，状态为
    “全文件完成”。
- [ ] `kernel/sched/build_policy.c`
- [ ] `kernel/sched/build_utility.c`

### 系统调用、等待与跨 CPU 调度接口

- [ ] `kernel/sched/syscalls.c`
- [ ] `kernel/sched/wait.c`
- [ ] `kernel/sched/wait_bit.c`
- [ ] `kernel/sched/membarrier.c`
- [ ] `kernel/sched/clock.c`

### 调度类与运行队列策略

- [ ] `kernel/sched/fair.c`
- [ ] `kernel/sched/rt.c`
- [ ] `kernel/sched/deadline.c`
- [ ] `kernel/sched/stop_task.c`
- [ ] `kernel/sched/core_sched.c`
- [ ] `kernel/sched/autogroup.h`
- [ ] `kernel/sched/autogroup.c`

### CPU 优先级、截止期与拓扑

- [ ] `kernel/sched/cpupri.h`
- [ ] `kernel/sched/cpupri.c`
- [ ] `kernel/sched/cpudeadline.h`
- [ ] `kernel/sched/cpudeadline.c`
- [ ] `kernel/sched/topology.c`

### PELT、负载、统计与 CPU 时间

- [ ] `kernel/sched/sched-pelt.h`
- [ ] `kernel/sched/pelt.h`
- [ ] `kernel/sched/pelt.c`
- [ ] `kernel/sched/loadavg.c`
- [ ] `kernel/sched/cputime.c`
- [ ] `kernel/sched/cpuacct.c`
- [ ] `kernel/sched/stats.h`
- [ ] `kernel/sched/stats.c`
- [ ] `kernel/sched/debug.c`
- [ ] `kernel/sched/psi.c`

### CPU 频率调节

- [ ] `kernel/sched/cpufreq.c`
- [ ] `kernel/sched/cpufreq_schedutil.c`

### sched_ext 可扩展调度器

- [ ] `kernel/sched/ext/types.h`
- [ ] `kernel/sched/ext/internal.h`
- [ ] `kernel/sched/ext/ext.h`
- [ ] `kernel/sched/ext/arena.h`
- [ ] `kernel/sched/ext/arena.c`
- [ ] `kernel/sched/ext/cid.h`
- [ ] `kernel/sched/ext/cid.c`
- [ ] `kernel/sched/ext/idle.h`
- [ ] `kernel/sched/ext/idle.c`
- [ ] `kernel/sched/ext/ext.c`

## 明确排除

- [-] `kernel/sched/core.c`（按用户要求直接跳过）
- [-] `kernel/sched/completion.c`（按用户要求直接跳过）
- [-] `kernel/sched/idle.c`（按用户要求直接跳过；原请求重复列出一次，本清单合并为一个排除项）
- [-] `kernel/sched/isolation.c`（按用户要求直接跳过）
- [-] `kernel/sched/swait.c`（按用户要求直接跳过）

## 当前处理文件

- 无；`kernel/sched/smp.h` 已完成。
- `kernel/sched/sched.h` 只做过局部只读建图，未修改源码，保持 `[ ]`。
- `kernel/sched/sched-pelt.h` 标明为自动生成且禁止直接修改，本轮保持 `[ ]`，等待范围决策。

## 单文件完成记录模板

每个 `[x]` 文件下至少追加以下可复核记录：

- 文件职责与主调用链。
- 第 17 章验收：函数清单、实体清单、英文注释清单、成功/快速/慢速/失败/释放/配置路径清单。
- 并发与生命周期：锁、RCU、引用、屏障、发布、摘除和最终释放。
- 至少三个复杂函数的初学者复述与开发者推理抽查；不足三个时说明实际数量及豁免原因。
- 关联读取：文件、符号或范围、读取原因、结论、现有学习注释覆盖状态；关联文件只读不改。
- 修改安全：零代码改动、零原注释删除或改写、禁用模板前缀扫描、`git diff --check`、checkpatch
  和可用的构建结果；未执行项必须写明原因。
- 最终状态必须准确使用“全文件完成”“主路径检查点完成”或“尚未达到标准”。只有前者可标 `[x]`。

## 范围变化

- 2026-08-20：创建清单。目录快照共 51 个文件，46 个进入计划，5 个明确排除。
