# Linux 调度器内部实现深度解析

> 适用内核版本：v7.2-rc1（基于当前仓库 `a14c021eef14`）
> 核心源码：`kernel/sched/core.c`、`kernel/sched/fair.c`、`kernel/sched/rt.c`、`kernel/sched/deadline.c`、`kernel/sched/sched.h`、`include/linux/sched.h`

---

## 目录

1. [调度器解决什么问题](#1-调度器解决什么问题)
2. [总体架构与调度类](#2-总体架构与调度类)
3. [核心数据结构](#3-核心数据结构)
4. [调度发生的条件](#4-调度发生的条件)
5. [schedule 完整切换路径](#5-schedule-完整切换路径)
6. [睡眠、等待与唤醒](#6-睡眠等待与唤醒)
7. [首次唤醒与 fork](#7-首次唤醒与-fork)
8. [EEVDF 公平调度原理](#8-eevdf-公平调度原理)
9. [Linux EEVDF 的数据结构与选取算法](#9-linux-eevdf-的数据结构与选取算法)
10. [实体入队、出队与延迟出队](#10-实体入队出队与延迟出队)
11. [nice、权重、slice 与抢占](#11-nice权重slice-与抢占)
12. [PELT 负载跟踪](#12-pelt-负载跟踪)
13. [SMP CPU 选择与负载均衡](#13-smp-cpu-选择与负载均衡)
14. [调度域、拓扑与能效调度](#14-调度域拓扑与能效调度)
15. [调度组与 CFS bandwidth](#15-调度组与-cfs-bandwidth)
16. [实时调度类](#16-实时调度类)
17. [SCHED_DEADLINE](#17-sched_deadline)
18. [NOHZ、时钟与 tick](#18-nohz时钟与-tick)
19. [亲和性、迁移与 CPU 热插拔](#19-亲和性迁移与-cpu-热插拔)
20. [优先级继承、proxy execution 与 core scheduling](#20-优先级继承proxy-execution-与-core-scheduling)
21. [并发控制与内存序](#21-并发控制与内存序)
22. [调度延迟的定位方法](#22-调度延迟的定位方法)
23. [常见误区与关键函数速查](#23-常见误区与关键函数速查)

---

## 1. 调度器解决什么问题

系统中的 runnable task 往往多于 CPU。调度器必须反复回答四个问题：

1. 哪些 task 当前可以运行？
2. 每个 CPU 下一刻应运行谁？
3. 新唤醒 task 应放在哪个 CPU？
4. 如何在公平、延迟、吞吐、实时保证、能耗和缓存局部性之间取舍？

“选优先级最高的进程”远远不够：

- 普通交互任务需要低唤醒延迟，CPU 密集任务需要长期比例公平；
- 实时任务需要优先级语义，deadline task 需要 runtime/deadline/period 保证；
- 每 CPU 独立 runqueue 能减少全局锁，却引入负载不均；
- 把 task 搬到空闲 CPU 可减少排队，却会丢失 cache、NUMA 和能效局部性；
- cgroup 要获得组间份额和带宽上限，而组内还要继续调度；
- tickless CPU、频率变化、SMT 安全和 CPU 热插拔都会改变决策条件。

Linux 的总体方案是：**per-CPU runqueue + 可插拔调度类 + 分层调度实体 + 分布式负载均衡**。

调度器管理的是 `task_struct` 执行流，不是用户概念中的线程组。一个多线程进程中的每个线程
都有独立调度实体、状态、CPU 亲和性和运行时间。

---

## 2. 总体架构与调度类

### 2.1 调度类优先级

```
stop class       CPU stop machine、迁移等最高内部优先级
    ↓
deadline class   SCHED_DEADLINE：EDF + CBS
    ↓
realtime class   SCHED_FIFO / SCHED_RR
    ↓
fair class       SCHED_NORMAL / SCHED_BATCH / SCHED_IDLE，EEVDF
    ↓
ext class        sched_ext/BPF；启用全接管时 active-class 遍历跳过 fair
    ↓
idle class       每 CPU idle task
```

调度类通过链接脚本排成有序区间。`pick_next_task()` 从高到低询问 class；高类有 runnable task
时，低类不会获得 CPU。`sched_class` 的主要接口包括：

```c
enqueue_task / dequeue_task
wakeup_preempt
balance / pick_task
put_prev_task / set_next_task
select_task_rq / migrate_task_rq
task_tick / task_fork / task_dead
set_cpus_allowed / rq_online / rq_offline
update_curr / reweight_task
```

核心层负责状态、runqueue 锁、context switch 和共同统计；每个 class 负责自己的排队结构、
选取规则、抢占条件与 class-specific balance。

### 2.2 为什么不用一个全局队列

全局 runqueue 让“最空闲 CPU”容易判断，但每次唤醒、阻塞和 tick 都争用同一把锁。Linux 为每
个 CPU 建 `struct rq`，本地 enqueue/pick 通常只锁本 CPU；跨 CPU 唤醒或迁移再协调两个队列。

代价是没有持续精确的全局视图。调度域定期 balance、idle CPU 主动拉任务、繁忙 CPU 必要时
push，形成最终趋于平衡而非每一瞬间绝对均匀的分布式系统。

---

## 3. 核心数据结构

### 3.1 `struct rq`：每 CPU 运行队列

定义于 `kernel/sched/sched.h`：

```
rq
├── __lock                 raw spinlock，本 CPU 调度状态总锁
├── curr / donor           当前执行/调度上下文 task
├── idle / stop            特殊 task
├── nr_running             runnable task 总数
├── cfs / rt / dl / scx    各调度类子队列
├── clock / clock_task     wall-like rq clock 与 task 可运行时钟
├── clock_pelt             PELT 时间基准
├── nr_switches            context switch 统计
├── root_domain            DL/RT/CPU capacity 共享域
├── sd                     sched_domain 拓扑链
├── next_balance           下次 periodic balance 时间
├── nohz_*                 tickless balance 状态
├── cpu_capacity           当前 CPU 可用容量
├── uclamp[]               runnable task 聚合利用率约束
└── balance_callback       解锁前后的延迟 balance 工作
```

`rq->curr` 是执行上下文；启用 proxy execution 后 `rq->donor` 表示提供调度资格的 task，二者
可能不同。普通配置下通过 union 重叠，二者相同。

### 3.2 task 中的调度字段

```
task_struct
├── __state              TASK_RUNNING/INTERRUPTIBLE/...
├── on_rq / on_cpu       是否排队、是否正在 CPU 执行
├── prio/static_prio/normal_prio/rt_priority
├── policy               NORMAL/BATCH/FIFO/RR/DEADLINE/IDLE/EXT
├── sched_class          当前 class
├── se / rt / dl / scx   各类嵌入实体
├── cpus_mask/cpus_ptr   CPU affinity
├── wake_cpu             唤醒放置提示
├── migration_disabled   禁迁移嵌套状态
└── sched_info/stats     延迟和运行统计
```

`on_rq`、`on_cpu` 和 `__state` 是不同维度。`TASK_RUNNING` 既可以正在 CPU 上，也可以在队列
等待。处于迁移/唤醒中间状态时，更不能仅凭一个字段推断完整位置。

### 3.3 `sched_entity` 与 `cfs_rq`

fair task 嵌入 `sched_entity se`：

```
sched_entity
├── load.weight           nice 映射后的权重
├── run_node              EEVDF augmented RB-tree 节点
├── vruntime              已获得服务的虚拟时间
├── deadline              当前虚拟请求截止期
├── slice                 请求长度
├── vlag                  离队时保存的近似 virtual lag
├── vprot                 protected slice 边界
├── min_vruntime          子树最小 vruntime augmentation
├── min_slice/max_slice   子树 slice augmentation
├── on_rq/sched_delayed   物理/逻辑排队状态
├── exec_start/sum_exec_runtime
├── avg                   PELT load/util/runnable
└── parent/my_q/cfs_rq    group scheduling 层次链接
```

`cfs_rq.tasks_timeline` 按 deadline 排序，并在每个节点维护子树最小 vruntime，使选取过程能跳过
整个不 eligible 的子树。它不再是旧 CFS 教程所说的“纯粹按 vruntime 排序的红黑树”。

---

## 4. 调度发生的条件

### 4.1 主动调度

任务在无法继续时设置睡眠状态并调用 `schedule()`：等待锁、等待队列、I/O、定时器、futex 等。
如果状态非 `TASK_RUNNING` 且没有竞态唤醒，`__schedule()` 把它从 runqueue deactivate。

### 4.2 被动抢占

内核通过 `TIF_NEED_RESCHED`/preempt need-resched 请求当前 task 尽快调度：

- 更高调度类或更合适的 task 被唤醒；
- 当前 EEVDF request/slice 到期；
- RR time slice 到期；
- deadline/RT bandwidth 状态变化；
- CPU offline、迁移或调度策略改变。

真正切换发生在合法调度点：中断返回、系统调用返回、显式 preemption check、解开最后一层
`preempt_disable()` 等。设置 need-resched 不等于瞬间夺走 CPU。

### 4.3 抢占模型

- `PREEMPT_NONE`：内核态主要在显式 schedule/用户返回点抢占；
- `PREEMPT_VOLUNTARY`：更多显式 cond_resched 点；
- `PREEMPT`：除不可抢占临界区外允许内核抢占；
- `PREEMPT_RT`：大量 spinlock 转为可睡眠 rtmutex，并改变中断线程化与锁语义。

`preempt_count` 编码抢占禁用、softirq/hardirq/NMI 等上下文。持有普通 raw spinlock 或禁中断
期间不可 schedule；调度器用 debug 检查捕获 atomic context sleeping。

---

## 5. `schedule()` 完整切换路径

### 5.1 主调用链

```
schedule()
  → sched_submit_work()           block 前提交 plug/task work 等
  → __schedule_loop(SM_NONE)
    → __schedule(SM_NONE)
      → local_irq_disable()
      → rcu_note_context_switch()
      → rq_lock(rq)
      → smp_mb__after_spinlock()
      → update_rq_clock()
      → 读取 prev->__state
      → 若主动睡眠：try_to_block_task()/deactivate_task()
      → pick_next_task()
      → clear need_resched
      → rq->curr = next
      → context_switch(rq, prev, next)
        → prepare_task_switch()
        → switch_mm_irqs_off()/enter_lazy_tlb()
        → switch_to(prev, next, prev)
        → finish_task_switch(prev)
  → sched_update_worker()
```

### 5.2 为什么状态检查需要完整屏障

等待方和唤醒方的经典竞态：

```
waiter CPU                         waker CPU
set_current_state(INTERRUPTIBLE)   condition = true
检查 condition                    读取 task state
schedule()                         try_to_wake_up()
```

若两边读写重排，waiter 可能看不到 condition，waker 又看不到睡眠状态，产生 lost wakeup。
`rq_lock()` 后的 `smp_mb__after_spinlock()` 与唤醒侧锁/屏障配对，保证至少一边观察到另一边状态。

### 5.3 主动阻塞与抢占的区别

抢占时 prev 仍是 `TASK_RUNNING`，保留 runnable 身份并由 class 的 `put_prev_task()` 重新放置。
主动睡眠时 prev 状态非 0，`deactivate_task()` 从 class 队列移除，切换计入 voluntary switch
`nvcsw`；抢占通常计入 involuntary switch `nivcsw`。

如果主动睡眠前已有信号/唤醒把状态改回 `TASK_RUNNING`，调度路径不能再 dequeue，否则会丢失
唤醒。状态、`p->pi_lock`、rq lock 和屏障共同串行化该决定。

### 5.4 `pick_next_task()`

核心层从高优先级 class 到低优先级 class：先让 class 做 balance，再调用 `pick_task()`；选定后
完成 prev/next 的 class hook。fair-only 常见场景有优化，但不能绕过更高 class 的存在判断。

若没有普通 runnable task，最终一定选到 per-CPU idle task，所以调度器不返回 NULL。

### 5.5 `context_switch()` 不只是换寄存器

- 切换或借用 `mm_struct`，处理内核线程 `active_mm`；
- 更新 membarrier 所需顺序；
- 调用体系结构 `switch_to()` 保存/恢复 callee-saved 寄存器、栈、TLS 等；
- 完成 perf、RCU、lockdep、MMU notifier、架构状态切换；
- 在新 task 的内核栈上返回；
- `finish_task_switch()` 释放旧 task 的 rq lock 相关状态，并可能处理已死亡 task。

代码在 `switch_to()` 后继续执行时，CPU、`current` 和内核栈都已经属于 next。局部变量 `prev`
仍指向被切走者，这是理解 context switch 汇编与 C 交界的关键。

---

## 6. 睡眠、等待与唤醒

### 6.1 等待队列协议

正确模式不是“条件不满足就 schedule”：

```c
for (;;) {
    prepare_to_wait(&wq, &wait, TASK_INTERRUPTIBLE);
    if (condition)
        break;
    if (signal_pending(current))
        break;
    schedule();
}
finish_wait(&wq, &wait);
```

先入等待队列并发布状态，再检查 condition，关闭“事件发生在检查与睡眠之间”的窗口。
`wait_event*()` 宏封装了这套协议。唤醒不保证条件仍成立，waiter 必须循环复查。

### 6.2 `try_to_wake_up()` 状态机

```
try_to_wake_up(p, state_mask, wake_flags)
  → raw_spin_lock_irqsave(&p->pi_lock)
  → 匹配 p->__state
  → smp_rmb()/acquire 等待 p->on_cpu 清零
  → p->__state = TASK_WAKING
  → select_task_rq(p, ...)
  → 必要时 set_task_cpu()
  → ttwu_queue()
      ├── 本地/直接：rq lock → ttwu_do_activate()
      └── 远端 wakelist：llist_add + IPI
  → enqueue_task()
  → p->__state = TASK_RUNNING
  → wakeup_preempt()，必要时 resched_curr()
```

`p->pi_lock` 不只用于优先级继承，它也是 task 状态、CPU 归属与唤醒/迁移之间的重要稳定锁。
唤醒侧必须等待 `on_cpu` 清除，防止前一 CPU 尚未完成 switch-out 就在另一 CPU 运行同一 task。

### 6.3 远端 wake list

直接取得远端 rq lock 会造成 cacheline bouncing。满足条件时，waker 把 task 放进目标 CPU 的
lockless wake list，设置 `ttwu_pending` 并发 IPI；目标 CPU 批量 `sched_ttwu_pending()` 入队。

这减少跨 CPU 锁争用，但要求严格发布顺序：目标 CPU 看到 wake-list 节点时，必须同时看到
`TASK_WAKING`、目标 CPU 和唤醒前的 task 状态写入。

### 6.4 CPU 选择与 wake affinity

fair class 的 `select_task_rq_fair()` 综合：

- affinity/cpuset/online CPU 约束；
- waker 与 wakee 的 CPU/cache 关系；
- prev CPU 的 cache hotness；
- idle sibling、LLC domain 和 CPU capacity；
- task utilization、uclamp、misfit；
- NUMA preferred node；
- energy-aware placement。

选择“当前最空闲 CPU”未必最好。把短睡眠 task 留在 cache warm 的 prev CPU，常比迁到空闲但
遥远的 CPU 更快。

---

## 7. 首次唤醒与 fork

`copy_process()` 中 `sched_fork()` 初始化 policy、priority、调度实体和 CPU；新 task 尚未对
调度器 runnable。`kernel_clone()` 完成 PID、trace 和用户指针处理后调用：

```
wake_up_new_task(p)
  → p->__state = TASK_RUNNING
  → p->pi_lock + 目标 rq lock
  → update_rq_clock()
  → activate_task()/enqueue_task(... ENQUEUE_NOCLOCK)
  → class->wakeup_preempt(... WF_FORK)
  → class->task_woken()
```

新子任务首次运行从体系结构 `ret_from_fork` 恢复预造寄存器现场。公平类可通过 child-runs-first
特性调整父子相对位置，但必须保持长期 lag/fairness 约束。

fork 前 task 不可被运行，发布后父进程不能再假定裸 `task_struct *p` 永远有效；子进程可能在
另一 CPU 立即运行、exec 并退出。

---

## 8. EEVDF 公平调度原理

### 8.1 从比例公平到 virtual runtime

task 的 nice 值映射到权重 `w_i`。运行真实时间 `delta_exec` 后：

```
delta_vruntime = delta_exec × NICE_0_LOAD / w_i
```

nice 0 权重约为基准，virtual time 与真实时间近似相同；权重大者 vruntime 增长慢，因此长期
得到更多 CPU。权重比决定份额，而非“每次固定多运行几毫秒”。

### 8.2 ideal service 与 lag

EEVDF 用 lag 衡量实际服务与理想服务差：

```
lag_i = S_i - s_i = w_i × (V - v_i)

S_i：理想公平系统应给的服务
s_i：实际得到的服务
V：系统 virtual time
v_i：实体 vruntime
```

`lag >= 0` 表示任务没有超额获得服务，称为 eligible；`lag < 0` 表示已经领先，不应仅因 deadline
小就继续运行。在 Linux 实现中 eligibility 等价于 `V >= v_i`，但为避免除法精度损失，源码
用加权和直接比较。

### 8.3 virtual deadline

每个实体以 slice 形成 request：

```
virtual_slice = slice × NICE_0_LOAD / weight
deadline = vruntime + virtual_slice
```

从 eligible 实体中选择最早 virtual deadline，短 request 更早完成，改善交互延迟；长期服务仍
由 lag 约束，不会让不断提交短 request 的 task 无限窃取份额。

### 8.4 virtual time 的实现

公平系统满足总 lag 守恒：

```
Σ lag_i = 0
Σ w_i(V - v_i) = 0
V = Σ(w_i × v_i) / Σw_i
```

直接计算会溢出。`cfs_rq` 保存参考点 `zero_vruntime`、相对加权和 `sum_w_vruntime` 和
`sum_weight`：

```
V = zero_vruntime + Σ[w_i(v_i - zero_vruntime)] / Σw_i
```

`avg_vruntime()` 还把当前正在运行但不在树中的 entity 计入。参考点随 V 移动，保持差值范围
有限；极端权重/运行时间下源码还有 shift/paranoid 路径防止乘法溢出。

### 8.5 加入和离开为何困难

若 task 带着非零 lag 离开，剩余实体的加权平均 V 会跳动；睡眠后再原样回来又可能凭旧优势
获得不公平服务。`place_entity()` 在 enqueue 时用保存的 `vlag` 与当前 V 重建 vruntime，并
对 lag 做边界限制，在交互唤醒与份额公平之间折中。

---

## 9. Linux EEVDF 的数据结构与选取算法

### 9.1 augmented RB-tree

`tasks_timeline` 的主排序键是 `deadline`。每个节点额外维护：

```
node.min_vruntime = min(node.vruntime,
                        left.min_vruntime,
                        right.min_vruntime)
```

若左子树 `min_vruntime > V`，左子树没有 eligible entity，可整体跳过。这样能在按 deadline
排序的树上同时完成 eligibility 过滤，复杂度 O(log n)，而不是扫描所有 runnable task。

当前源码还维护子树 `min_slice/max_slice`，用于 protected slice、run-to-parity 等选择和保护。

### 9.2 `pick_eevdf()`

算法要点：

1. 若队列只有一个实体，直接返回 curr 或树中实体；
2. 可选 PICK_BUDDY 且 next buddy eligible 时优先它；
3. 当前 curr 若已不 eligible，则排除；
4. curr 在 protected slice 内时可继续运行，保证最小进展；
5. 若 deadline 最小的 leftmost entity eligible，直接选择；
6. 否则从根开始，利用左子树 `min_vruntime` 剪枝，找最早 eligible deadline；
7. 最后比较 curr 与候选 deadline，保留更早者。

这不是单纯 EDF：没有 eligibility 条件，领先任务也能凭短 deadline 继续占用 CPU；也不是旧 CFS
的 min-vruntime pick：deadline 把 request size 纳入延迟选择。

### 9.3 当前任务为什么常不在树中

`cfs_rq->curr` 单独保存正在运行实体。pick/set_next 会把 chosen entity 从可选树语义中取出，
put_prev 再更新运行时间和重新放置。计算 V、load 或 eligibility 时源码显式把 curr 加回来，
否则系统平均值会在每次切换时人为跳变。

### 9.4 protected slice

频繁的新 wakee 若总能以更早 deadline 抢占，当前 task 可能反复只运行极短时间，带来上下文
切换开销和无进展。`vprot` 给当前实体一段受保护的 virtual service；`protect_slice()` 成立时，
`pick_eevdf()` 可继续选择 curr。保护长度受 base slice、队列最小 slice 与 RUN_TO_PARITY 影响。

保护不是永久不可抢占：更高调度类仍可抢占，保护边界到期也会重新比较。

---

## 10. 实体入队、出队与延迟出队

### 10.1 enqueue

fair task enqueue 需要逐级处理 group hierarchy：

```
enqueue_task_fair(rq, p, flags)
  → for_each_sched_entity(se)
      → enqueue_entity(cfs_rq, se, flags)
          → update_curr()
          → update_load_avg()/PELT
          → place_entity()（新/唤醒/迁移时）
          → __enqueue_entity() 插 augmented RB-tree
      → 子 cfs_rq 从空变非空时，把其 group se 入父 cfs_rq
  → 更新 h_nr_running、util_est、overutilized 等
```

同一个 task 在叶子 cfs_rq 中是实体；它所属 task_group 在父 cfs_rq 中又由 group entity 代表。
只有层层选中 group entity，最终 task 才运行。

### 10.2 dequeue 与 lag 保存

真正离队前 `update_entity_lag()` 计算并限制 `vlag = V - vruntime` 的虚拟表示。睡眠 task 不再
参与当前 V，但唤醒时 `place_entity()` 用 vlag 恢复相对欠账/超额状态。

迁移需要先把 vruntime 转换为相对源 cfs_rq 的表示，再按目标 cfs_rq 基准放置，否则两个 CPU
不同 virtual timeline 会让 task 平白获得或失去服务。

### 10.3 delayed dequeue

当前树支持 `sched_delayed`。负 lag task 睡眠时可暂时逻辑离开、物理上保留在 EEVDF tree，
让其负 lag 随系统 virtual time 前进而衰减，避免通过频繁 sleep/requeue 逃避已经超额的服务。

```
task sleep，lag < 0
  → 标记 sched_delayed
  → 仍保持 se->on_rq/tree membership
  → 不作为普通 runnable task 执行
  → V 前进，负 lag 接近 0
  → delayed dequeue 完成，或 spurious wakeup 恢复
```

因此 `se->on_rq == 1` 不再必然等价于 task 对用户语义 runnable；调试当前版本必须同时看
`sched_delayed`。这是旧 CFS 文档最容易遗漏的差异之一。

### 10.4 `update_curr()`

调度 tick、enqueue/dequeue、pick 等入口先更新当前实体：

```
delta_exec = rq_clock_task(rq) - curr->exec_start
curr->sum_exec_runtime += delta_exec
curr->vruntime += calc_delta_fair(delta_exec, curr)
account_cfs_rq_runtime()       cgroup bandwidth
update_deadline()              request 用完则换新 deadline/slice
```

时钟更新必须在 rq lock 下按协议进行。重复用旧 clock 或在未标记的路径更新，会造成负 delta、
双重计费或 PELT 时间异常。

---

## 11. nice、权重、slice 与抢占

### 11.1 nice 是乘法权重

nice 范围通常为 -20..19，映射到离散 `prio_to_weight[]`。相邻 nice 的权重约相差 1.25 倍，
所以 nice 差值表达相对比例，而非绝对时间片：

```
两个 CPU-bound task 权重 w1、w2：
share1 ≈ w1 / (w1 + w2)
share2 ≈ w2 / (w1 + w2)
```

若还有 task_group，task 权重先在组内分配，组的 shares 再决定组间份额。

### 11.2 slice 的含义

EEVDF slice 是 request size，影响 virtual deadline 和延迟，而长期 CPU 份额仍由 weight/lag
决定。默认基于 normalized base slice，task 可通过支持的 latency/slice 接口影响请求长度，
但内核会做边界和层级处理。

slice 越短，deadline 往往更早、响应更灵敏，但 context switch 增多；越长吞吐更好，但交互
任务等待可能增加。这是延迟—吞吐的工程旋钮，不是实时保证。

### 11.3 唤醒抢占

新 task 入队后 `wakeup_preempt()` 比较 class 与 class-specific 条件。fair class 会考虑 eligibility、
deadline、protected slice、buddy、fork 和 delayed 状态，而不是简单比较 vruntime。

若需抢占，`resched_curr(rq)` 设置当前 task need-resched；远端 CPU 可能收到 reschedule IPI。
若目标 CPU 即将自然经过调度点，可避免不必要 IPI。

---

## 12. PELT 负载跟踪

### 12.1 三种信号

PELT（Per-Entity Load Tracking）对历史做指数衰减，主要区分：

- `load_avg`：runnable 时间按 task/group weight 加权；
- `runnable_avg`：runnable 程度，不乘最终 CPU capacity 使用方式；
- `util_avg`：实际 running 占比，近似 CPU 利用需求。

概念上每经过半衰期，旧贡献减半；源码用定点数和周期累积避免浮点运算。blocked task 的贡献
不会立刻归零，而是逐渐衰减，使短暂睡眠任务保留负载历史。

### 12.2 PELT 的消费者

- load balance 判断组间负载；
- CPU capacity/misfit 判断 task 是否需要更强 CPU；
- schedutil cpufreq 估计所需频率；
- energy-aware scheduling 估算 placement 能耗；
- cgroup 层级传播 runnable/load；
- util_est 用 enqueue-time 估计弥补 PELT 对突发任务响应较慢。

PELT 不是精确未来预测。I/O burst、频率限制、IRQ 时间、thermal pressure 和 uclamp 都会让
`util_avg` 与 wall-clock CPU 百分比不同。

### 12.3 capacity normalization

异构 CPU 和动态频率下，同样运行 1ms 完成的工作量不同。调度器用 `SCHED_CAPACITY_SCALE`
归一化 CPU capacity，并把频率、架构原始能力、IRQ/thermal pressure 等因素纳入有效容量。

若 task util 超过当前 CPU capacity，它是 misfit task，应尽量迁往更强 CPU；但 affinity、
cpuset 或其他约束可能使迁移不可能。

---

## 13. SMP CPU 选择与负载均衡

### 13.1 三种主要时机

1. wakeup placement：`select_task_rq_fair()` 决定新唤醒 task 去哪；
2. newidle balance：CPU 即将 idle 时从别处拉 task；
3. periodic balance：tick/softirq 按 sched_domain 周期平衡。

此外 active balance 可让繁忙 CPU 通过 stop task 强制迁出难以被被动拉走的 task。

### 13.2 `sched_balance_rq()` 的层次

```
sched_balance_domains(this_rq, idle)
  → 逐级 sched_domain
    → sched_balance_rq(this_cpu, this_rq, sd, idle_type)
      → update_sd_lb_stats()
      → sched_balance_find_src_group()
      → sched_balance_find_src_rq()
      → detach_tasks(src_rq)
      → attach_tasks(dst_rq)
      → 若长期失败且严重失衡，active_balance
```

balance 不是简单比较 `nr_running`。不同 nice、group、CPU capacity 和 task util 下，一个 task
不等于一个单位负载。算法还检查 cache hot、affinity、migration cost、NUMA 和 asym packing。

### 13.3 双 rq 锁

迁移 runnable task 必须同时从源队列 dequeue、改变 `task_cpu()`、再向目标 enqueue。需要两把
rq lock 时使用规定的地址/CPU 顺序和 double-lock helper，避免 CPU A→B 与 B→A 死锁。

正在运行的 task 不能像普通 queued task 一样直接搬走。active migration 借助 CPU stopper 在
源 CPU 高优先级上下文中切走/迁移，或设置 migration request 等待安全点。

### 13.4 newidle balance 的代价

CPU 空闲时主动扫描可缩短等待，但扫描大量 domain/rq 本身耗时并污染 cache。调度器根据平均
idle duration、balance cost、拓扑层级判断是否值得继续；短暂 idle 可能直接进入 idle class，
等待下一次 wakeup。

---

## 14. 调度域、拓扑与能效调度

### 14.1 sched_domain 层次

体系结构根据 CPU 拓扑构建重叠层级，例如：

```
SMT sibling
  → MC/core cluster
    → LLC/package
      → NUMA node
        → 跨 NUMA system
```

每层由 sched_group 划分 peer group，并带 balance interval、imbalance、wake affine、asym packing
等 flags。低层迁移 cache 成本小、平衡频繁；跨 NUMA 成本高、频率低且更保守。

### 14.2 root_domain

root_domain 表示一组共同参与实时/Deadline 带宽和 CPU 优先级管理的 CPU，包含 cpupri、cpudl、
DL bandwidth、overload masks 等。cpuset 隔离可能形成不同 root_domain。

### 14.3 Energy-Aware Scheduling

EAS 在非对称容量平台上，基于 energy model 比较候选 CPU placement 的估算能耗，同时保证 task
fits capacity。它主要在适用拓扑、fair task、系统未明显 overutilized 等条件下介入。

EAS 不是“永远放小核”。低 util task 放小核可能省电，高 util/misfit task 放大核可能更快完成
并尽早 idle；uclamp.min/max 可表达性能下限和利用率上限。

---

## 15. 调度组与 CFS bandwidth

### 15.1 分层公平

启用 `CONFIG_FAIR_GROUP_SCHED` 时，每个 task_group 在每 CPU 有一个 `cfs_rq`，并在父 cfs_rq
中有对应 group `sched_entity`：

```
root cfs_rq on CPU0
├── task A
├── group G entity ─→ G.cfs_rq on CPU0
│                     ├── task B
│                     └── subgroup H entity ─→ H.cfs_rq
└── group K entity
```

选择从根逐层进入子队列。组 shares 决定组间比例，组内 task nice 决定组内比例；因此单看 task
nice 无法推断跨 cgroup 的最终 CPU 份额。

### 15.2 quota/period

cgroup v2 `cpu.max` 对应周期带宽控制。task_group 有全局 runtime pool，各 CPU cfs_rq 批量领取
runtime，减少每次执行争用全局锁：

```
period 开始 → 补充 group runtime
CPU cfs_rq 执行 → runtime_remaining 下降
耗尽 → throttle_cfs_rq()，整棵组实体从父队列不可运行
下一周期/余量分配 → unthrottle_cfs_rq()
```

批量 slice 会造成短期 burst 和多 CPU 间近似误差，但显著降低锁开销。quota 是上限，不保证
一定获得 CPU；shares/weight 是竞争时比例，不是硬上限。

### 15.3 throttling 的层级传播

子 cfs_rq 被 throttle 后，其 runnable load 不能继续当作可执行负载传播给父层；PELT clock 在
throttle 期间也需特殊处理，避免解封时凭空累计 runnable/running 时间。

观察调度延迟时，应同时检查 `cpu.stat` 中 throttled_usec/nr_throttled；task runnable 很久但
不运行，原因可能是 cgroup quota 而非 CPU runqueue 饱和。

---

## 16. 实时调度类

### 16.1 FIFO 与 RR

RT class 按静态实时优先级维护队列数组和 bitmap：找到最高非空优先级近似 O(1)。

- `SCHED_FIFO`：运行到阻塞、yield、被更高 RT/DL 抢占或改变策略；
- `SCHED_RR`：同优先级 FIFO 基础上增加时间片，到期轮转到同优先级队尾。

普通 fair task 无论 nice 多高，都不能抢占 runnable RT task。

### 16.2 push/pull

多 CPU 上，RT 维护 overloaded 信息与 `cpupri`，唤醒/优先级变化时选择能容纳 task 的 CPU；
队列过载时 push 低优先级 RT task，CPU 空闲/降优先级时 pull 更合适 task。

迁移仍受 affinity、cpuset 和 migration-disabled 约束。错误的 RT affinity 可把多个高优先级
task 固定到单 CPU，其他 CPU 空闲也无能为力。

### 16.3 RT bandwidth

为防止 RT task 永久饿死内核维护和普通任务，系统/组可设置 `sched_rt_runtime_us` /
`sched_rt_period_us`。耗尽后 RT rq 被 throttle，直到补充。配置 `runtime=-1` 可取消保护，使用
不当会让系统几乎无法管理。

RT priority 只保证相对调度顺序，不保证某段代码在硬 deadline 前完成；缺页、IRQ、锁、SMI、
thermal throttling 都会影响最坏延迟。

---

## 17. `SCHED_DEADLINE`

### 17.1 参数与 CBS

用户指定：

```
runtime  Q：每个 job 可消耗 CPU 时间
deadline D：相对截止期
period   P：job 周期/最小间隔
通常要求 0 < Q <= D <= P
```

调度使用 EDF 选择 absolute deadline 最早的实体，并用 Constant Bandwidth Server 限制其带宽。
执行消耗 remaining runtime；耗尽后 throttle，在 replenishment timer 到期时恢复预算并推进 deadline。

### 17.2 admission control

设置 DL 策略前内核做带宽准入，避免允许的 DL utilization 超出 root_domain 能力。`dl_bw` 近似
`Q/P`，density 与 `Q/D` 相关；CPU hotplug、cpuset 和 affinity 改变都需重新检查可行性。

准入保证依赖模型假设，不自动包含应用持锁、I/O 或 page fault 的最坏时间。

### 17.3 push/pull 与 cpudl

DL class 用按 earliest deadline 组织的 rq 和 `cpudl` 结构寻找可迁移 CPU。当某 CPU 有多个 DL
task 或更早 deadline task 唤醒时，push/pull 维持全局 EDF 的近似可行执行。

deadline task 阻塞后可能在一定时间内仍计入 active utilization，inactive timer 防止通过频繁
阻塞逃避 bandwidth accounting。

### 17.4 reclaim 与 deadline server

可选 GRUB reclaim 让 DL task 使用其他 server 未消费带宽，同时保持隔离公式。当前 rq 还包含
`fair_server`，用于在需要的配置下以 deadline server 为 fair class 提供带宽/饥饿保护接口；
这说明 class 优先级并非所有内部场景都只是简单链表比较。

---

## 18. NOHZ、时钟与 tick

### 18.1 scheduler tick

周期 tick 中 `sched_tick()`：

```
锁本 CPU rq
  → update_rq_clock()
  → curr->sched_class->task_tick()
      fair: update_curr/PELT/request 到期检查
      RT: RR slice、runtime
      DL: runtime/deadline
  → perf/task stats/uclamp 等
  → 触发 periodic load balance 条件
```

tick 设置 need-resched，真正切换通常在中断返回的抢占检查发生。

### 18.2 tickless idle

idle CPU 停止周期 tick 后，不能靠每 CPU tick 自己参与 balance。NOHZ 选择 idle balance CPU
维护全局 idle mask；繁忙 CPU 或定时器触发 `nohz_idle_balance`，远端更新 blocked PELT 并寻找
应拉到 idle CPU 的任务。

### 18.3 NOHZ full

`nohz_full` CPU 在只有一个 runnable task 等条件下也可停 tick，降低 HPC/实时抖动。但仍有
RCU、POSIX CPU timer、perf、调度带宽等 tick dependency；内核用 tick dependency mask 决定
能否真正停止。

停 tick 不代表调度统计停止。上下文切换、enqueue/dequeue 和远端更新会用 rq clock 补算时间。

### 18.4 rq clock

`rq->clock` 接近调度墙钟，`clock_task` 扣除 IRQ/steal 等不可归因于 task 的时间，`clock_pelt`
服务 PELT 衰减。更新受 rq lock 和 `RQCF_*` flags 约束，以免嵌套路径重复读 sched_clock。

---

## 19. 亲和性、迁移与 CPU 热插拔

### 19.1 CPU 允许集合

task 最终可运行 CPU 是 affinity、cpuset、online/active 状态及调度类限制的交集。`cpus_ptr`
可能临时指向受限 mask；不能只读取用户设置的 `cpus_mask` 推断当前有效集合。

改变 affinity 需要锁住 task/rq，若当前 CPU 已不允许则发起 migration。task 可能正 running、
正在 waking、排队或 sleeping，每种状态的迁移方式不同。

### 19.2 `migrate_disable()`

它禁止当前 task 被迁到其他 CPU，但通常不等同于禁止抢占：task 可被切走，之后仍必须回到同一
CPU。PREEMPT_RT 用它支持 per-CPU 数据和锁语义。长期 migration-disable 会妨碍 balance/hotplug。

### 19.3 CPU offline

热拔 CPU 时 stop-machine/cpu stopper 协调：阻止新 task 放入、迁走普通/RT/DL task、迁移 timer
和 IRQ 相关状态，最后只留下特殊 idle/stop 上下文。per-CPU kthread 和 pinned task 若无法迁移，
可能阻止 offline 或走专门 park 协议。

---

## 20. 优先级继承、proxy execution 与 core scheduling

### 20.1 PI 为什么进入调度器核心

高优先级 task 等待低优先级 task 持有的 rtmutex 时，低优先级 owner 被临时提升 effective prio，
避免中优先级任务造成无界 priority inversion。`p->pi_lock` 保护 PI chain 与调度状态交界；改变
effective priority 可能需要 dequeue、换 class/priority、再 enqueue 并触发抢占。

PI chain walk 必须处理嵌套锁、owner 变化和死锁检测，不能仅改 `task->prio`。

### 20.2 proxy execution

当前源码可选 `CONFIG_SCHED_PROXY_EXEC`，区分：

- donor：提供调度优先级/资格的 blocked task；
- curr：真正占用 CPU 执行、可能是锁 owner 的 task。

若高优先级 waiter 被 mutex 阻塞，调度器可沿 blocked-on 链选择 owner 代其执行，使释放锁的工作
直接消耗 donor 的调度机会，减少复杂依赖下的优先级反转。`__schedule()` 因此可能先选 donor，
再用 `find_proxy_task()` 找执行者。

这是当前开发树的重要新路径；未启用时 rq->donor/curr union 等价，传统调度语义保持。

### 20.3 core scheduling

SMT sibling 共享微架构资源。core scheduling 给 task 设置 cookie，只允许兼容 cookie 的任务
同时运行在同一物理 core；无法匹配时某 sibling 可能 force-idle，即使其本地 rq 有 runnable
task。

它用吞吐损失换取跨信任域侧信道隔离。调试“CPU 空闲但有 runnable task”时，需检查
core-forceidle，而不能只看单 CPU rq。

---

## 21. 并发控制与内存序

### 21.1 锁的职责

| 锁/机制 | 保护内容 |
|---------|----------|
| `rq->__lock` | 本 CPU class queues、curr、on_rq、时钟和统计 |
| `p->pi_lock` | task wakeup/CPU 归属/PI 与 rq locking 交界 |
| double rq lock | 跨 CPU runnable task 迁移 |
| RCU | sched_domain/root 视图、rq->curr 等读侧生命周期 |
| seqcount | 部分统计、拓扑和无锁快照 |
| stopper | 必须在目标 CPU 安全执行的迁移/热插拔操作 |

`task_rq_lock()` 先稳定 `p->pi_lock` 与 task 所属 rq，再锁对应 rq；若迁移恰在发生会重试。
裸读 `task_cpu(p)` 后直接锁该 rq 可能锁错队列。

### 21.2 `on_cpu` 协议

switch-out 结束前旧 task 的 `on_cpu` 仍为 1。唤醒者在 `p->pi_lock` 下以 acquire 语义等待它
清零，确保旧 CPU 对 task 的写入完成后，新 CPU 才 enqueue/运行。否则同一内核栈可能被两个
CPU 同时使用。

### 21.3 state 与 wakeup

等待者用 `set_current_state()` 的 barrier 语义发布睡眠，唤醒者在锁/屏障后检查匹配 state。
`__set_current_state()` 更弱，只能在调用者已经通过其他锁序保证不会丢唤醒时使用。

调度器注释中的 memory-order pair 是正确性组成部分，不是性能提示。删除“看起来多余”的
`smp_rmb()` 或把 `WRITE_ONCE` 换成普通赋值，可能只在弱序多核上形成极难复现的挂死。

### 21.4 锁住 rq 时不能做什么

rq lock 是 raw spinlock，通常关本地中断；临界区不能睡眠、不能执行不可控长操作，也要避免
触发会反向取得 rq/PI 锁的路径。class hook 的 locking contract 写在 `struct sched_class` 定义
上方，是实现新调度类必须遵守的 ABI。

---

## 22. 调度延迟的定位方法

### 22.1 先区分四种时间

```
sleep time：      task 等事件，非 runnable
runnable delay：  已 runnable，在 rq 等 CPU
execution time：  实际 on CPU
throttled time：  因 cgroup/RT/DL bandwidth 暂不可执行
```

只看到 task 很久没运行，不能直接归因于 scheduler：它可能在等锁/I/O，也可能 affinity 受限、
cgroup throttle、core force-idle 或更高 class 长期占用。

### 22.2 常用观测

```bash
# task policy、优先级、CPU 和等待点
ps -eLo pid,tid,psr,stat,cls,rtprio,pri,ni,wchan:28,comm
cat /proc/<pid>/sched
cat /proc/<pid>/status
taskset -pc <pid>
chrt -p <pid>

# 全局/每 CPU 调度统计
cat /proc/schedstat
cat /proc/sched_debug
mpstat -P ALL 1
pidstat -w -t 1

# cgroup throttling 与压力
cat /sys/fs/cgroup/<group>/cpu.stat
cat /sys/fs/cgroup/<group>/cpu.max
cat /proc/pressure/cpu
```

`/proc/<pid>/sched` 的字段受配置和版本影响，解释前应对照当前源码；等待时间统计也可能因
`CONFIG_SCHEDSTATS`/运行时开关而缺失。

### 22.3 tracepoints

核心事件：

- `sched_waking`：开始唤醒，task 尚未必入队；
- `sched_wakeup`：唤醒完成、目标 CPU 已确定；
- `sched_switch`：prev→next，包含 prev_state；
- `sched_migrate_task`：CPU 迁移；
- `sched_process_fork/exec/exit`：生命周期；
- `sched_stat_wait/sleep/iowait/runtime`：启用统计时的时间归因。

一次 wake-to-run latency 是某 task `sched_wakeup` 到首次作为 next 出现在 `sched_switch` 的差，
需要按 PID/TID、唤醒序列和迁移关联，不能简单对所有相邻事件相减。

### 22.4 perf/ftrace/eBPF

```bash
perf sched record -- <workload>
perf sched timehist
perf sched latency

trace-cmd record -e sched_switch -e sched_wakeup -e sched_migrate_task <cmd>
```

长 runnable latency 排查顺序：检查目标 CPU 上更高 class task/IRQ；检查 affinity/cpuset；检查
cgroup throttle；检查 migration/NUMA；检查 runqueue lock 或关中断长临界区；最后才深入 EEVDF
选择细节。

### 22.5 latency nice、uclamp 与优先级的区别

- nice/weight：长期 fair CPU 份额；
- slice/latency hint：request 长度和响应—吞吐偏好；
- uclamp：用于频率、capacity fitting 的 utilization 边界；
- RT priority：高于 fair 的固定优先级 class；
- deadline 参数：runtime/deadline/period 预留模型。

它们作用在不同决策层，不能互相替代。

---

## 23. 常见误区与关键函数速查

### 23.1 常见误区

**误区一：当前公平调度仍是“挑最小 vruntime”。** 当前 fair class 使用 EEVDF：先 eligible，
再挑最早 virtual deadline，树也按 deadline 排序。

**误区二：时间片决定 CPU 份额。** 长期份额主要由 weight/lag 决定；slice 主要影响 request
deadline、响应延迟与切换频率。

**误区三：`TASK_RUNNING` 表示正在运行。** 它也包括在 runqueue 排队的 task。

**误区四：need-resched 一设置就立即切换。** 它是请求，真正切换要到允许的调度点。

**误区五：唤醒就是在当前 CPU 入队。** class 会选择目标 CPU，也可能经远端 wake list 批量入队。

**误区六：CPU 使用率低就不可能有调度延迟。** affinity、单 CPU 热点、RT、throttle、SMT
force-idle 或长关中断都可与全局低利用率并存。

**误区七：nice -20 是实时。** 它仍属于 fair class，任何 runnable RT/DL task 都在其上。

**误区八：更多迁移总能改善负载。** 迁移会损失 cache/NUMA 局部性，调度器有意容忍短期不均。

**误区九：`sched_delayed` task 的 `se.on_rq` 一定表示可运行。** 当前 EEVDF 延迟出队会让逻辑
睡眠实体暂时保留在树中。

### 23.2 关键函数

| 阶段 | 函数 | 位置 | 作用 |
|------|------|------|------|
| 主调度 | `__schedule()` | `kernel/sched/core.c` | block/pick/context switch 核心 |
| 任务选择 | `pick_next_task()` | `kernel/sched/core.c` | 按 class 优先级选择 |
| 上下文切换 | `context_switch()` | `kernel/sched/core.c` | mm、寄存器和 task 切换 |
| 唤醒 | `try_to_wake_up()` | `kernel/sched/core.c` | state/CPU/入队/抢占状态机 |
| 首次唤醒 | `wake_up_new_task()` | `kernel/sched/core.c` | fork 后第一次 enqueue |
| tick | `sched_tick()` | `kernel/sched/core.c` | class tick、计时和 balance |
| fair 入队 | `enqueue_task_fair()` | `kernel/sched/fair.c` | 分层 enqueue 与 PELT 更新 |
| fair 出队 | `dequeue_task_fair()` | `kernel/sched/fair.c` | lag 保存与分层 dequeue |
| EEVDF 选择 | `pick_eevdf()` | `kernel/sched/fair.c` | earliest eligible virtual deadline |
| 当前计费 | `update_curr()` | `kernel/sched/fair.c` | runtime、vruntime、deadline |
| virtual time | `avg_vruntime()` | `kernel/sched/fair.c` | 加权平均 virtual time |
| 唤醒放置 | `select_task_rq_fair()` | `kernel/sched/fair.c` | affinity、idle、energy/capacity |
| 周期平衡 | `sched_balance_rq()` | `kernel/sched/fair.c` | source group/rq 选择与迁移 |
| RT | `pick_task_rt()` 等 | `kernel/sched/rt.c` | FIFO/RR 队列与 push/pull |
| Deadline | `pick_task_dl()` 等 | `kernel/sched/deadline.c` | EDF/CBS、带宽和迁移 |
| affinity | `set_cpus_allowed_ptr()` | `kernel/sched/core.c` | 修改允许 CPU 并迁移 |

### 23.3 一次完整的睡眠—唤醒—抢占

```
Task A on CPU0
  → prepare_to_wait(TASK_INTERRUPTIBLE)
  → condition false
  → schedule()
      → rq0 lock
      → deactivate A
      → fair pick B
      → rq0.curr=B
      → switch_to(A,B)

Task C on CPU1 产生事件
  → condition=true
  → wake_up(A)
  → try_to_wake_up(A)
      → pi_lock
      → A: SLEEPING → WAKING
      → select_task_rq_fair() 选择 CPU0
      → 远端 wake list + reschedule IPI

CPU0
  → sched_ttwu_pending()
  → enqueue_task_fair(A)
      → place_entity(vlag, current V)
      → 计算 deadline，插 augmented RB-tree
  → wakeup_preempt(B, A)
      → 若 A 更应运行，设置 B need_resched
  → 中断返回进入 preempt schedule
  → pick_eevdf()：eligible 集合中选最早 deadline
  → 可能 B→A
```

阅读调度器源码时始终抓住五个不变量：

1. **每个 task 在任一时刻只能由一个 CPU 执行，`on_cpu` 协议守住这个边界；**
2. **task 状态、CPU 归属和 runqueue membership 必须在 PI/rq 锁协议下转换；**
3. **class 决定“选谁”，核心层决定“如何安全切换”；**
4. **fairness 依赖 virtual lag 守恒，latency 由 eligible deadline 与 request size 调节；**
5. **SMP 调度是局部最优加周期修正，迁移收益必须覆盖 cache、NUMA 和同步成本。**
