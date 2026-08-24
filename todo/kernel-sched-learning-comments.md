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
- [ ] `kernel/sched/sched.h`
- [x] `kernel/sched/smp.h`
- [x] `kernel/sched/features.h`
- [x] `kernel/sched/rq-offsets.c`
- [x] `kernel/sched/build_policy.c`
- [x] `kernel/sched/build_utility.c`

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
- [x] `kernel/sched/autogroup.h`
- [ ] `kernel/sched/autogroup.c`

### CPU 优先级、截止期与拓扑

- [x] `kernel/sched/cpupri.h`
- [ ] `kernel/sched/cpupri.c`
- [x] `kernel/sched/cpudeadline.h`
- [ ] `kernel/sched/cpudeadline.c`
- [ ] `kernel/sched/topology.c`

### PELT、负载、统计与 CPU 时间

- [ ] `kernel/sched/sched-pelt.h`
- [x] `kernel/sched/pelt.h`
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
- [x] `kernel/sched/ext/arena.h`
- [ ] `kernel/sched/ext/arena.c`
- [ ] `kernel/sched/ext/cid.h`
- [ ] `kernel/sched/ext/cid.c`
- [x] `kernel/sched/ext/idle.h`
- [ ] `kernel/sched/ext/idle.c`
- [ ] `kernel/sched/ext/ext.c`

## 明确排除

- [-] `kernel/sched/core.c`（按用户要求直接跳过）
- [-] `kernel/sched/completion.c`（按用户要求直接跳过）
- [-] `kernel/sched/idle.c`（按用户要求直接跳过；原请求重复列出一次，本清单合并为一个排除项）
- [-] `kernel/sched/isolation.c`（按用户要求直接跳过）
- [-] `kernel/sched/swait.c`（按用户要求直接跳过）

## 当前处理文件

- 无；本轮 8 个文件 `cpupri.h`、`ext/arena.h`、`ext/idle.h`、`autogroup.h`、
  `build_policy.c`、`build_utility.c`、`features.h`、`pelt.h` 均已完成整体审计。
- 本轮源文件合计新增 675 行、删除 0 行；逐文件新增行均仅为注释或空行，原代码、声明、宏、条件
  编译和原注释逐行保留。8 个目标的 `git diff --check` 与忽略中文 UTF-8 字节行长后的 checkpatch
  均为 0 errors/0 warnings；工作树无 `.config`，未执行构建或运行时验证。
- 当前计划进度为 12/46 个文件 `[x]`、34/46 个文件 `[ ]`，5 个用户明确排除文件 `[-]`。
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
- 2026-08-24：按小文件优先的单文件闭环完成上述 8 个目标；计划完成度由 4/46 提升为 12/46。
