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
   和关联读取清单，再执行注释密度门禁、追加式安全审计、格式检查及可用的构建检查。密度命令固定为
   `scripts/check-learning-comment-density.py --min-density 0.20 --max-code-gap 10 <目标文件>`；两项阈值
   必须同时通过，不能以总新增行数代替。
6. 只有单文件内容验收全部通过后才标为 `[x]` 并登记验收摘要；一个 `[~]` 文件未闭环前不开始
   下一个文件。
7. 为核对契约而读取的关联源码只登记到当前文件记录；读取关联文件不等于授权修改。
8. 本清单覆盖创建时 `kernel/sched` 下的全部 51 个文件：46 个进入计划，5 个按用户要求排除。
   后续若目录新增文件，先登记到“范围变化”再决定学习顺序，不能静默遗漏。
9. 从 2026-08-25 当前 `[~]` 文件起，后续每个函数的专属中文函数头必须紧邻函数定义并显式包含四项：
   **业务背景**（必须包含理解该函数所需的业务背景知识、上层问题、存在理由和调用链位置）、**入参**（逐参数的含义、单位、范围、可空性、
   输入/输出属性和 ownership）、**出参/返回**（逐输出参数的前后状态与 ownership，以及全部直接返回
   类别）、**注意事项**（作为函数的强制注释事项，覆盖锁/中断/RCU/睡眠、生命周期、失败副作用、配置差异和误用后果中的适用项）。
   无参数、无输出参数或 `void` 返回也必须明确写“无”，不能靠上下文猜测；函数组说明和函数体走读
   不能替代单个函数的四项契约。验收记录必须逐函数核对这四项，密度脚本通过不能替代该人工门禁。
10. 本规则采用向前生效：规则写入前已经标为 `[x]` 的文件不因新增四项格式而重新打开；当前 `[~]`
    和所有 `[ ]` 文件必须严格执行。若已完成文件以后因其他原因重新打开，则该次重新验收也适用新规则。

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

- [x] `kernel/sched/syscalls.c`
- [x] `kernel/sched/wait.c`
- [x] `kernel/sched/wait_bit.c`
- [x] `kernel/sched/membarrier.c`
- [x] `kernel/sched/clock.c`

### 调度类与运行队列策略

- [ ] `kernel/sched/fair.c`
- [~] `kernel/sched/rt.c`
- [ ] `kernel/sched/deadline.c`
- [x] `kernel/sched/stop_task.c`
- [x] `kernel/sched/core_sched.c`
- [x] `kernel/sched/autogroup.h`
- [x] `kernel/sched/autogroup.c`

### CPU 优先级、截止期与拓扑

- [x] `kernel/sched/cpupri.h`
- [x] `kernel/sched/cpupri.c`
- [x] `kernel/sched/cpudeadline.h`
- [x] `kernel/sched/cpudeadline.c`
- [x] `kernel/sched/topology.c`

### PELT、负载、统计与 CPU 时间

- [ ] `kernel/sched/sched-pelt.h`
- [x] `kernel/sched/pelt.h`
- [x] `kernel/sched/pelt.c`
- [x] `kernel/sched/loadavg.c`
- [x] `kernel/sched/cputime.c`
- [x] `kernel/sched/cpuacct.c`
- [x] `kernel/sched/stats.h`
- [x] `kernel/sched/stats.c`
- [x] `kernel/sched/debug.c`
- [x] `kernel/sched/psi.c`

### CPU 频率调节

- [x] `kernel/sched/cpufreq.c`
- [x] `kernel/sched/cpufreq_schedutil.c`

### sched_ext 可扩展调度器

- [x] `kernel/sched/ext/types.h`
- [x] `kernel/sched/ext/internal.h`
- [x] `kernel/sched/ext/ext.h`
- [x] `kernel/sched/ext/arena.h`
- [x] `kernel/sched/ext/arena.c`
- [x] `kernel/sched/ext/cid.h`
- [x] `kernel/sched/ext/cid.c`
- [x] `kernel/sched/ext/idle.h`
- [x] `kernel/sched/ext/idle.c`
- [ ] `kernel/sched/ext/ext.c`

## 明确排除

- [-] `kernel/sched/core.c`（按用户要求直接跳过）
- [-] `kernel/sched/completion.c`（按用户要求直接跳过）
- [-] `kernel/sched/idle.c`（按用户要求直接跳过；原请求重复列出一次，本清单合并为一个排除项）
- [-] `kernel/sched/isolation.c`（按用户要求直接跳过）
- [-] `kernel/sched/swait.c`（按用户要求直接跳过）

## 当前处理文件

- 当前 `[~]` 文件为 `rt.c`。新增密度门禁回溯原 43 个 `[x]` 时有 27 个 C/头文件失败；随后已重新
  处理并关闭其中 17 个。`Makefile` 不属于 C-family 统计范围，按原验收保留 `[x]`。
