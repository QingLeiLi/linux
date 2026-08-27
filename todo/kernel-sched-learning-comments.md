# kernel/sched 学习注释任务清单

状态：`[ ]` 未开始；`[~]` 正在处理或验收；`[x]` 全文件闭环；`[-]` 明确排除。

处理与验收以 `doc/linux-kernel-source-learning-methodology.md` 为准；单文件按 `[~] → 第 17 章验收 → [x]`
闭环。新增规则向前生效，不自动重开已完成文件。

## 文件进度（按建议学习顺序）

### 构建入口、公共数据结构与调度特性

- [x] `kernel/sched/Makefile`
- [x] `kernel/sched/sched.h`
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
- [x] `kernel/sched/rt.c`
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

- 无（`kernel/sched/sched.h` 已完成第 17 章验收）
