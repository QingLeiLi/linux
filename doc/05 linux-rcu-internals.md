# Linux RCU 机制深度解析

> 适用内核版本：v5.0+（本文基于 kernel/rcu/ 源码分析）

---

## 目录

1. [RCU 解决什么问题](#1-rcu-解决什么问题)
2. [核心原理](#2-核心原理)
3. [宽限期与静止状态](#3-宽限期与静止状态)（含 QS 推导、上报机制、来源、housekeeping 影响）
4. [RCU 状态机（启动阶段）](#4-rcu-状态机启动阶段)
5. [API 速查与选型](#5-api-速查与选型)
6. [RCU 的三种变体](#6-rcu-的三种变体)
7. [expedited 与 normal 宽限期](#7-expedited-与-normal-宽限期)
8. [典型使用场景与代码模式](#8-典型使用场景与代码模式)
9. [常见错误与注意事项](#9-常见错误与注意事项)
10. [与其他同步机制的对比](#10-与其他同步机制的对比)
11. [调试工具](#11-调试工具)

---

## 1. RCU 解决什么问题

### 1.1 经典困境：读多写少的数据结构

内核中大量数据结构（路由表、进程列表、文件系统 dentry 缓存、网络连接表）
有一个共同特征：**读操作远多于写操作，且对读性能极度敏感**。

用普通读写锁（`rwlock`）保护它们有一个根本缺陷：即使是读锁，在多核系统上
也需要原子操作来维护引用计数，这在高并发场景下会导致 cacheline 在各 CPU
之间反复失效，产生严重的"锁争用"开销——哪怕根本没有写者。

```
rwlock 的问题（32 核机器，纯读负载）：
  CPU 0 rlock → 原子 inc(readers)  ← cacheline 失效
  CPU 1 rlock → 原子 inc(readers)  ← cacheline 失效
  CPU 2 rlock → 原子 inc(readers)  ← cacheline 失效
  ... 32 个 CPU 在同一 cacheline 上轮番竞争
```

### 1.2 RCU 的答案

RCU（Read-Copy Update）的核心思想：**让读者完全不需要任何锁、原子操作、
甚至内存屏障**（在多数体系结构上），代价是写者必须走"复制-修改-替换"的
迂回路径，并等待老读者离场后才能释放旧数据。

```
RCU 读者：零开销进入临界区，直接读，零开销退出
RCU 写者：复制 → 修改副本 → 原子替换指针 → 等宽限期 → 释放旧数据
```

这不是魔法，而是一个工程取舍：把同步负担从**每次读**转移到**每次写**，
适合读多写少的场景。

---

## 2. 核心原理

### 2.1 三步走：Copy-Update-Reclaim

以替换一个全局指针 `gp` 指向的结构体为例：

```c
/* 受保护的数据 */
struct foo { int val; };
struct foo __rcu *gp;  /* __rcu 标注告诉 sparse 这个指针需要 RCU 访问 */

/* 写者 */
void writer(void)
{
    struct foo *new = kmalloc(sizeof(*new), GFP_KERNEL);
    new->val = 42;

    struct foo *old = rcu_dereference_protected(gp, lockdep_is_held(&my_mutex));

    rcu_assign_pointer(gp, new);   /* 步骤1：原子替换指针（含写屏障） */
    synchronize_rcu();             /* 步骤2：等待所有持有 old 的读者退出 */
    kfree(old);                    /* 步骤3：安全释放旧数据 */
}

/* 读者 */
void reader(void)
{
    rcu_read_lock();               /* 进入临界区（通常是空操作） */
    struct foo *p = rcu_dereference(gp);  /* 读取指针（含读屏障） */
    if (p)
        do_something(p->val);     /* 安全使用，保证 p 在临界区内有效 */
    rcu_read_unlock();             /* 退出临界区（通常是空操作） */
}
```

### 2.2 为什么读者不需要锁

关键洞察：写者**从不修改读者正在读的内存**，而是创建新副本再替换指针。

```
时间轴：
  读者 R1 进入临界区，持有指向 old 的指针
  写者 W  替换指针：gp → new（old 仍然完整存在于内存中）
  读者 R2 进入临界区，拿到的是 new 的指针
  读者 R1 退出临界区（此时 W 仍在 synchronize_rcu() 中等待）
  写者 W  从 synchronize_rcu() 返回，kfree(old)——此时 R1 已经离场
```

old 在 R1 持有引用期间从未被修改或释放，所以 R1 不需要任何锁。

### 2.3 rcu_assign_pointer 与 rcu_dereference 的内存序作用

- `rcu_assign_pointer(p, v)`：在赋值前插入**写屏障**（`smp_store_release`
  语义），确保新数据的初始化对所有 CPU 可见后，指针才更新。
  防止编译器/CPU 将指针赋值重排到数据初始化之前。

- `rcu_dereference(p)`：插入**数据依赖屏障**（DEC Alpha 等弱序架构需要），
  确保用指针加载数据时不会因 CPU 推测执行拿到过期值。
  在 x86 等强序架构上通常编译为普通 `READ_ONCE()`。

---

## 3. 宽限期与静止状态

### 3.1 宽限期（Grace Period）

宽限期是 RCU 的核心概念：**一段时间，在此期间系统保证所有"在宽限期开始之前
已经开始"的 RCU 读临界区都已经结束**。

```
宽限期示意：
  ←── 宽限期开始 ─────────────────── 宽限期结束 ──→
  读者 R1：[临界区开始 ─── 临界区结束]              ← 宽限期内离场，安全
  读者 R2：              [临界区开始 ─── 结束]      ← 宽限期内离场，安全
  读者 R3：                                [开始 ─→ ← 宽限期开始后才进入，
                                                       看到的是新数据，不持有旧指针
  写者：  替换指针 → synchronize_rcu() 阻塞 → 返回 → kfree(old)
                                          ↑
                                    确认 R1、R2 已离场
```

### 3.2 静止状态（Quiescent State，QS）

RCU 不直接跟踪每个读者的生命周期（那样代价太高），而是利用一个关键观察：

> **一个 CPU 如果经历了一次上下文切换（或进入 idle/用户态），就一定不在
> 任何 RCU 读临界区中**（因为 rcu_read_lock 不能跨越调度点）。

这样的时刻称为"静止状态"。当**所有 CPU** 都经历过至少一次静止状态后，
宽限期就可以结束。

```
CPU 0:  [任务A rlock─────runlock] [切换] [任务B ...]
                                    ↑
                               QS for CPU 0

CPU 1:  [任务C ─────────────────────────] [切换] ...
                                              ↑
                                         QS for CPU 1

宽限期结束时刻：所有 CPU 都报告了 QS
```

### 3.3 "上下文切换 = QS"的逻辑推导

上下文切换能作为 QS 的判据，不是巧合，而是 `rcu_read_lock` 主动制造的等价关系。

**非抢占内核中 `rcu_read_lock()` 的实现：**

```c
// include/linux/rcupdate.h (!CONFIG_PREEMPT_RCU)
static inline void __rcu_read_lock(void)
{
    preempt_disable();   // 禁止抢占
}
```

`preempt_disable()` 保证了一条硬性约束：**RCU 读临界区不能跨越调度点**。
任务在 `rcu_read_lock()` 到 `rcu_read_unlock()` 之间绝对不会被切换出去。

由此可以反推：
```
能发生上下文切换
  → 此刻没有执行 preempt_disable()
  → 此刻不在 rcu_read_lock() 临界区内
  → 此 CPU 上宽限期开始前的所有读临界区已结束
  → 构成一次 QS
```

这不是 RCU 利用了调度器的特性，而是 RCU 自己为读临界区立了一条规矩，
使得调度器的行为可以作为"临界区已结束"的**充分证明**。

可抢占内核（`CONFIG_PREEMPT_RCU`）放弃了这个约束，`rcu_read_lock()` 改为
递增 `task_struct.rcu_read_lock_nesting`，不禁止抢占。代价是 GP kthread
必须主动跟踪被抢占的读者（挂到 `rnp->blkd_tasks` 链表），宽限期要等链表
清空才能结束——复杂度和开销都显著上升。

### 3.4 QS 如何上报：调度器主动通知

RCU 自己不轮询，也不观测 CPU 状态，而是由**调度器在切换时主动调用**。

```c
// kernel/sched/core.c  __schedule()
local_irq_disable();
rcu_note_context_switch(preempt);  // ← 所有上下文切换的唯一入口
// ... 之后才执行真正的 context_switch()
```

选在 `local_irq_disable()` 之后、`context_switch()` 之前调用，原因是：
- 中断已关，操作原子，不会被打断
- `current` 仍指向旧任务，`rcu_read_lock_nesting` 还能被正确读取
- 切换之后 `current` 变成新任务，旧任务状态就再也读不到了

`rcu_note_context_switch()` 内部根据配置分两条路：

```
非抢占内核：
  直接调用 rcu_qs()，将本 CPU 的 cpu_no_qs.b.norm 清零，
  告知 GP kthread "我已经过了一次 QS"。

可抢占内核：
  先检查 rcu_read_lock_nesting：
    == 0：不在临界区，直接 rcu_qs() 上报 QS
    > 0 ：在临界区内被抢占，将任务挂入 rnp->blkd_tasks，
          标记 blocked=true，GP kthread 必须等此链表清空
```

### 3.5 上下文切换之外的 QS 来源

上下文切换是最主要的 QS 来源，但不是唯一的，还有三种补充路径：

| 来源 | 触发时机 | 原因 |
|------|---------|------|
| `rcu_idle_enter()` | CPU 进入 idle | idle 状态下显然没有任何读临界区 |
| `rcu_user_enter()` | CPU 返回用户态 | 用户态代码不在内核 rcu_read_lock 里 |
| `rcu_sched_clock_irq()` | 每次时钟中断 | 兜底：针对长时间不调度的 CPU（如 nohz_full） |

前两种是精确的即时上报；时钟中断是兜底保障——对于关掉周期 tick 的 CPU，
如果它既没有切换任务、也没进 idle，就只能靠每次时钟中断检查一次是否可以上报 QS。

### 3.6 rcu_read_lock 的真实作用

`rcu_read_lock()` 看起来像一把"读锁"，实际上**它不阻止任何并发**，
它的唯一作用是维持上面那条"上下文切换 = QS"的等价关系。

#### 两种内核配置下的实现完全不同

**非抢占内核（`!CONFIG_PREEMPT_RCU`，服务器/嵌入式常用）：**

```c
// include/linux/rcupdate.h
static inline void __rcu_read_lock(void)
{
    preempt_disable();   // 禁止抢占，仅此而已
}
```

`preempt_disable()` 在纯非抢占内核（`CONFIG_PREEMPTION=n`）下直接编译为空，
在有计数但无抢占点的配置下是一条对 per-CPU 变量的 inc，几乎零开销。

**可抢占内核（`CONFIG_PREEMPT_RCU`，桌面/实时内核）：**

```c
// kernel/rcu/tree_plugin.h
void __rcu_read_lock(void)
{
    current->rcu_read_lock_nesting++;   // 递增 task_struct 中的嵌套计数
    barrier();
}
```

不禁止抢占，而是记录嵌套深度。调度器切换时检查这个计数：
- `== 0`：不在临界区，直接上报 QS
- `> 0`：在临界区内被抢占，将任务挂到 `rnp->blkd_tasks`，
  GP kthread 等链表清空后才能结束宽限期

#### 为什么不统一用计数方案

看起来"递增计数"更通用，实则在非抢占内核里是纯粹的额外开销：

```
计数方案的代价（每次 rcu_read_lock/unlock）：
  读写 task_struct.rcu_read_lock_nesting
  → task_struct 不常驻 L1 cache
  → 高并发下每次进出临界区都可能引发 cache miss
  → GP kthread 还要扫描 blkd_tasks 链表

非抢占内核的 preempt_disable() 代价：
  纯非抢占配置：零指令（编译时消除）
  有计数配置：一条 per-CPU 变量 inc（已在 L1 cache）
```

非抢占内核根本不存在"临界区内被抢占"的场景，用计数方案等于付出
cache miss 的代价去处理一个永远不会发生的情况。

#### 不能将 rcu_read_lock 持有过长时间

既然 `rcu_read_lock` 在非抢占内核里就是 `preempt_disable`，持有不释放的后果：

```
非抢占内核：
  这个 CPU 上的抢占被永久关闭
  → 该 CPU 永远不会发生上下文切换
  → 永远不会上报 QS
  → 所有 synchronize_rcu() 调用者永久阻塞
  → 约 21 秒后触发 RCU CPU stall warning，内核打印调用栈

可抢占内核：
  任务可以被切走，但挂在 blkd_tasks 链表上
  → GP kthread 等链表清空，你不 unlock 就永不清空
  → 所有依赖宽限期的路径（内存回收、模块卸载、热插拔）逐渐死锁
```

**正确的用法**：临界区只用来"安全取到指针"，取到后立即退出，
靠引用计数保护对象的长时使用：

```c
// 错误：用 RCU 临界区保护长时任务
rcu_read_lock();
p = rcu_dereference(gp);
do_long_work(p);        // 可能持续数秒，导致 CPU stall
rcu_read_unlock();

// 正确：临界区只做"取引用"这一瞬间
rcu_read_lock();
p = rcu_dereference(gp);
if (p)
    get_device(p);      // 在临界区内取得引用计数
rcu_read_unlock();      // 立即退出

do_long_work(p);        // 对象由引用计数保护，可以长时使用
put_device(p);
```

RCU 负责"安全取到指针"这一瞬间，引用计数负责"持有对象"这段时间，两者各司其职。

### 3.7 QS 的扩展定义（v5.0+）

从 v5.0 起，以下上下文也构成静止状态（因为它们隐含地禁止了 RCU 读临界区）：
- 关中断（`local_irq_disable`）
- 关抢占（`preempt_disable`）
- 关软中断（`local_bh_disable`）
- 硬中断处理函数、软中断处理函数、NMI 处理函数

这意味着上述上下文本身就是隐式的 RCU 读临界区，无需显式调用 `rcu_read_lock()`。

### 3.8 housekeeping 对 RCU 的影响

housekeeping（`isolcpus=` / `nohz_full=`）将 CPU 分为两类：
- **housekeeping CPU**：运行内核噪声（定时器、中断、kthread 等）
- **isolated CPU**：尽量排除内核干扰，专供实时任务或高性能计算

这个划分从三个维度影响 RCU。

#### 影响一：GP kthread 的 CPU 亲和性

RCU GP kthread（`rcu_preempt` / `rcu_sched`）在启动时会把自己绑到
housekeeping CPU 上：

```c
// kernel/rcu/tree_plugin.h
static void rcu_bind_gp_kthread(void)
{
    if (!tick_nohz_full_enabled())
        return;
    housekeeping_affine(current, HK_TYPE_RCU);  // 绑到 HK_TYPE_KERNEL_NOISE CPU
}
```

`HK_TYPE_RCU` 是 `HK_TYPE_KERNEL_NOISE` 的别名，即"非 nohz_full 的 CPU"。

**为什么要绑**：GP kthread 需要频繁唤醒、处理 QS 上报、推进宽限期序号，
这些操作本身就是"内核噪声"。让它运行在 isolated CPU 上会破坏 isolated CPU
的低延迟目标，也会让 GP kthread 自己被 tick 限制干扰。

#### 影响二：nohz_full CPU 的 QS 上报困难

nohz_full CPU 关掉了周期性 tick，在用户态运行时不会收到时钟中断。
一旦它进入内核态执行较长时间（如系统调用、page fault），就可能长时间不经历
上下文切换，也不进 idle，既不能上报 QS，又没有 tick 触发 `rcu_sched_clock_irq()`。

RCU 为此设计了一套"强制 tick"机制：

```
GP kthread 检测到某 nohz_full CPU 长时间未上报 QS
    → 设置该 CPU 的 rdp->rcu_urgent_qs = true
    → 该 CPU 下次收到任意中断/异常时触发 __rcu_irq_enter_check_tick()
    → 检查 rcu_urgent_qs，若为 true 则强制开启该 CPU 的 tick
         tick_dep_set_cpu(cpu, TICK_DEP_BIT_RCU)
    → tick 恢复后，rcu_sched_clock_irq() 可以检测 QS
    → QS 上报成功后，清除强制 tick
         tick_dep_clear_cpu(cpu, TICK_DEP_BIT_RCU)
         rcu_forced_tick = false
```

极端情况下（精心调优、完全没有中断），GP kthread 会主动向目标 CPU 发送一个
IPI 来触发上述流程。

另外，为避免 nohz_full CPU 刚进入内核就被立即打扰，RCU 提供了耐心等待参数：

```
nohz_full_patience_delay（模块参数，单位 ms，默认 0）
  → GP kthread 在标记 nohz_full CPU 为"急需 QS"前等待的时间
  → 短内核执行路径不会触发强制 tick，减少不必要的干扰
```

#### 影响三：nocb——将回调处理卸载到 housekeeping CPU

`CONFIG_RCU_NOCB_CPU` + 启动参数 `rcu_nocbs=` 可以将指定 CPU 的
`call_rcu()` 回调处理卸载给专用的 `rcuop/N` kthread。

```
普通模式：
  CPU N 调用 call_rcu() → 回调挂在 CPU N 的 rdp->cblist
  → CPU N 在每次时钟中断时处理自己的回调队列
  → isolated CPU 上仍然有 tick 驱动的回调处理噪声

nocb 模式（rcu_nocbs=N）：
  CPU N 调用 call_rcu() → 回调通过 nocb bypass 发给 rcuop/N kthread
  → rcuop/N 运行在 housekeeping CPU 上
  → CPU N 完全不处理 RCU 回调，tick 可以更彻底地停掉
```

nocb kthread 同样会被 `housekeeping_affine()` 绑到非 isolated CPU，
确保回调处理的噪声不会泄漏到 isolated CPU。

#### 影响四：宽限期变长导致内存堆积

nohz_full 拖慢 QS 上报会引发一个容易被忽视的连锁反应：

```
nohz_full CPU 拖慢 QS 上报
    → 宽限期变长
    → call_rcu() / kfree_rcu() 的回调迟迟得不到调用
    → 待释放对象持续堆积在 rdp->cblist（或 nocb 模式的 rcuop 队列）
    → 堆积量 = 写入速率 × 宽限期时长
```

这不是小数字。假设系统每秒产生 100 万次 `call_rcu()`（路由表更新、
dentry 回收等高频场景），宽限期从正常的 5ms 拉长到 50ms：

```
正常：1,000,000/s × 0.005s =  5,000 个对象滞留
拉长：1,000,000/s × 0.050s = 50,000 个对象滞留
```

每个对象 1KB 时，差距是 5MB vs 50MB——在内存紧张的系统上足以触发 OOM。

因此 housekeeping / nohz_full 带来的内存开销有**两层**，性质完全不同：

| 来源 | 量级 | 性质 |
|------|------|------|
| nocb kthread 本身（task_struct + 栈） | MB 级，固定 | 可预测，一次性 |
| 宽限期变长导致的回调堆积 | 与写入速率正相关，无上界 | 动态的，高负载下可能很大 |

第二层是真正的风险：它随写入速率和宽限期延长时间**成正比放大**，在高负载下可能远超第一层。

**内核的防御机制**：当某 CPU 的待处理回调数超过 `qhimark`（默认 10000）时，
RCU 会提升紧迫度，强制触发 expedited GP 来缩短宽限期。但 expedited GP
要向所有 CPU 发 IPI——这正是 isolated CPU 最不想要的干扰，绕了一圈回到根本矛盾。

**实践建议**：在高写入速率场景下使用 nohz_full，应当：
1. 监控 `/sys/kernel/debug/rcu/` 下的宽限期长度和回调积压量
2. 在 isolated CPU 上**完全避免写者路径**，写操作统一在 housekeeping CPU 上发起
3. 若无法避免，考虑降低 `nohz_full_patience_delay` 或接受更频繁的 tick 干扰

#### 小结

| housekeeping 机制 | 对 RCU 的影响 | RCU 的应对 |
|-------------------|-------------|-----------|
| nohz_full（关 tick）| isolated CPU 可能长时间不上报 QS | 强制 tick + 耐心等待参数 |
| isolcpus（隔离 CPU）| GP kthread 不应运行在 isolated CPU | `housekeeping_affine(HK_TYPE_RCU)` |
| rcu_nocbs（卸载回调）| isolated CPU 上有 call_rcu 回调噪声 | nocb kthread 绑到 housekeeping CPU |
| 宽限期变长（nohz_full 副作用）| 待释放对象堆积，内存消耗与写入速率正相关 | qhimark 触发 expedited GP（代价：IPI 干扰）|

---

## 4. RCU 状态机（启动阶段）

理解启动阶段的 RCU 行为对调试早期内核问题很重要。

```
rcu_scheduler_active 的三个状态：

  INACTIVE（初始）
      │
      │  rcu_scheduler_starting()  ← 在 rest_init() 创建第一个任务前调用
      │  · 对齐所有 rcu_node 的 gp_seq
      │  · 切换状态
      ▼
    INIT
      │
      │  core_initcall(rcu_set_runtime_mode)  ← RCU kthread 全部启动后
      │  · 激活 kfree_rcu 的 monitor
      │  · 切换状态
      ▼
  RUNNING（全功能）
```

| 状态 | synchronize_rcu() 行为 | call_rcu() 行为 | 场景 |
|------|----------------------|-----------------|------|
| INACTIVE | 退化为 barrier() + 序号记账 | 早期初始化处理 | 单任务启动阶段 |
| INIT | 走完整宽限期（无 GP kthread） | 正常排队 | 调度器已启动但 kthread 未就绪 |
| RUNNING | 走完整宽限期（GP kthread 驱动） | 正常排队 | 正常运行 |

---

## 5. API 速查与选型

### 5.1 读者侧 API

| API | 作用 | 何时用 |
|-----|------|--------|
| `rcu_read_lock()` | 进入 RCU 读临界区 | 绝大多数情况 |
| `rcu_read_unlock()` | 退出 RCU 读临界区 | 与 lock 配对 |
| `rcu_read_lock_bh()` | 进入临界区并关软中断 | 需要同时防软中断的场景 |
| `rcu_read_lock_sched()` | 进入临界区并关抢占 | 需要同时防调度的场景 |
| `rcu_dereference(p)` | 安全读取 RCU 保护的指针 | 临界区内读指针 |
| `rcu_dereference_protected(p, c)` | 在持锁时读取指针（跳过 lockdep 警告） | 写者侧读指针 |

### 5.2 写者侧 API

| API | 作用 | 何时用 |
|-----|------|--------|
| `rcu_assign_pointer(p, v)` | 安全替换 RCU 指针 | 写者更新指针 |
| `synchronize_rcu()` | 阻塞等待宽限期结束 | 可以睡眠的写者 |
| `synchronize_rcu_expedited()` | 加速宽限期（IPI 驱动） | 对延迟敏感的场景 |
| `call_rcu(head, func)` | 注册宽限期后回调（异步） | 不能睡眠的写者 |
| `kfree_rcu(ptr, field)` | 宽限期后自动 kfree | 最简单的释放场景 |
| `kfree_rcu(ptr)` | 宽限期后自动 kfree（无 rcu_head 字段） | v5.12+ 支持 |

### 5.3 如何选择 synchronize_rcu vs call_rcu

```
是否可以睡眠？
    ├── 是 → synchronize_rcu()（代码最简洁）
    └── 否 → call_rcu()（异步，需要在结构体里嵌入 struct rcu_head）

只是释放内存？
    └── kfree_rcu()（最简洁，底层是 call_rcu 的封装）

对宽限期延迟敏感（如关机、热插拔）？
    └── synchronize_rcu_expedited()（代价：对系统干扰大）
```

---

## 6. RCU 的三种变体

内核提供三套独立的 RCU 实现，覆盖不同场景。

### 6.1 经典 RCU（Tree RCU / Tiny RCU）

本文主要讨论的实现。读临界区**不能睡眠**。

```c
rcu_read_lock();
/* 临界区内禁止 schedule()、msleep() 等 */
p = rcu_dereference(gp);
rcu_read_unlock();
```

### 6.2 SRCU（Sleepable RCU）

允许读临界区内睡眠，代价是需要显式的 `srcu_struct` 实例。
适用于文件系统模块、设备驱动卸载等需要在临界区内调用可能睡眠的函数的场景。

```c
static DEFINE_SRCU(my_srcu);

/* 读者 */
int idx = srcu_read_lock(&my_srcu);
p = srcu_dereference(gp, &my_srcu);
msleep(100);  /* 允许睡眠！ */
srcu_read_unlock(&my_srcu, idx);

/* 写者 */
rcu_assign_pointer(gp, new);
synchronize_srcu(&my_srcu);
kfree(old);
```

**注意**：SRCU 的 `srcu_read_lock()` 返回一个 `int` 索引，
`srcu_read_unlock()` 需要把这个索引传回去——不能遗漏。

### 6.3 Tasks RCU

等待所有任务都经历过一次**显式调度点**（`schedule()` 或用户态进入）。
主要用于 ftrace、BPF 等需要修改正在执行中函数的场景。

```c
synchronize_rcu_tasks();  /* 等待所有任务调度过 */
```

---

## 7. expedited 与 normal 宽限期

### 7.1 normal 宽限期

依赖各 CPU 自然发生的上下文切换来上报 QS，宽限期可能需要数毫秒到数十毫秒。
优点：对系统几乎无干扰，吞吐量高（多个调用者共享同一个宽限期）。

实现（`synchronize_rcu_normal()`）：
- 快路径：把请求加入全局 llist，GP kthread 在宽限期结束后批量唤醒
- 慢路径（并发超过 64 或功能关闭时）：通过 `call_rcu_hurry` 独立提交

### 7.2 expedited 宽限期

向所有在线 CPU 发送 IPI，强制它们立即通过静止状态，宽限期可在数百微秒内完成。
代价：大量 IPI 中断正在运行的任务，影响实时性和延迟。

**启动阶段默认 expedited**：从调度器启动到 `rcu_end_inkernel_boot()`（在
`do_initcalls` 结束后调用），所有宽限期都被强制 expedited，以加快启动速度。
之后恢复 normal。

```
可通过内核参数控制：
  rcu_expedited=1  → 始终使用 expedited（不推荐生产环境）
  rcu_normal=1     → 禁用 expedited（rcu_normal 优先级高于 rcu_expedited）
```

---

## 8. 典型使用场景与代码模式

### 8.1 保护链表（最常见）

```c
/* 链表节点 */
struct node {
    int data;
    struct list_head list;
    struct rcu_head rcu;  /* 用于 call_rcu / kfree_rcu */
};

LIST_HEAD(my_list);
DEFINE_SPINLOCK(list_lock);  /* 写者之间用锁互斥，读者不需要 */

/* 读者：遍历链表 */
void reader(void)
{
    struct node *p;
    rcu_read_lock();
    list_for_each_entry_rcu(p, &my_list, list) {
        /* 安全访问 p->data，p 在临界区内保证有效 */
        process(p->data);
    }
    rcu_read_unlock();
}

/* 写者：删除节点 */
void delete_node(struct node *target)
{
    spin_lock(&list_lock);
    list_del_rcu(&target->list);  /* 从链表中摘除（带内存屏障） */
    spin_unlock(&list_lock);

    kfree_rcu(target, rcu);  /* 宽限期后自动释放，最简洁 */
}
```

### 8.2 保护全局指针（替换整个对象）

```c
struct config __rcu *global_cfg;

/* 写者：热更新配置 */
void update_config(struct config *new_cfg)
{
    struct config *old;

    mutex_lock(&cfg_mutex);
    old = rcu_dereference_protected(global_cfg,
                                    lockdep_is_held(&cfg_mutex));
    rcu_assign_pointer(global_cfg, new_cfg);
    mutex_unlock(&cfg_mutex);

    synchronize_rcu();
    kfree(old);
}

/* 读者：读取配置 */
void use_config(void)
{
    struct config *cfg;
    rcu_read_lock();
    cfg = rcu_dereference(global_cfg);
    if (cfg)
        apply(cfg->value);
    rcu_read_unlock();
    /* 注意：临界区结束后不能再用 cfg，对象可能已被 kfree */
}
```

### 8.3 不能睡眠时用 call_rcu

```c
/* 中断上下文或持有自旋锁时无法调用 synchronize_rcu() */
void delete_node_irq(struct node *target)
{
    spin_lock_irq(&list_lock);
    list_del_rcu(&target->list);
    spin_unlock_irq(&list_lock);

    call_rcu(&target->rcu, free_node_rcu);  /* 异步，立即返回 */
}

static void free_node_rcu(struct rcu_head *head)
{
    struct node *p = container_of(head, struct node, rcu);
    kfree(p);
}
```

---

## 9. 常见错误与注意事项

### 9.1 在 RCU 临界区内睡眠（经典 RCU）

```c
/* 错误：rcu_read_lock 后不能睡眠 */
rcu_read_lock();
msleep(100);        /* BUG：会触发 might_sleep() 警告 */
rcu_read_unlock();

/* 正确：改用 SRCU，或在临界区外睡眠 */
```

### 9.2 临界区外使用 rcu_dereference 获取的指针

```c
rcu_read_lock();
p = rcu_dereference(gp);
rcu_read_unlock();
/* 此后 p 随时可能被 kfree，不能再用！ */
p->val;  /* BUG：use-after-free */

/* 正确：在临界区内复制需要的值 */
rcu_read_lock();
p = rcu_dereference(gp);
val = p ? p->val : 0;  /* 在临界区内取出值 */
rcu_read_unlock();
/* 之后只用 val，不用 p */
```

### 9.3 写者之间没有互斥

```c
/* 错误：两个写者并发替换同一指针，old 可能被 double-free */
void writer_a(void) { rcu_assign_pointer(gp, new_a); synchronize_rcu(); kfree(old); }
void writer_b(void) { rcu_assign_pointer(gp, new_b); synchronize_rcu(); kfree(old); }

/* 正确：写者之间必须用锁互斥（spinlock 或 mutex） */
```

RCU 只保护读者，**写者之间的并发必须用其他锁来保护**。

### 9.4 在 synchronize_rcu() 持有锁

```c
/* 危险：持有自旋锁调用 synchronize_rcu() 会死锁或优先级反转 */
spin_lock(&my_lock);
synchronize_rcu();   /* BUG：synchronize_rcu 可能调度，但持有自旋锁 */
spin_unlock(&my_lock);

/* 正确：先解锁，再等宽限期 */
spin_lock(&my_lock);
old = rcu_dereference_protected(gp, ...);
rcu_assign_pointer(gp, new);
spin_unlock(&my_lock);
synchronize_rcu();   /* 解锁后再等 */
kfree(old);
```

### 9.5 忘记 __rcu 标注与 sparse 检查

给 RCU 保护的指针加上 `__rcu` 标注，然后用 `make C=2` 运行 sparse，
它会检查是否所有访问都通过了 `rcu_dereference`/`rcu_assign_pointer`。

```c
struct foo __rcu *gp;  /* 正确：加了 __rcu */
struct foo *gp;        /* 漏了标注，sparse 无法检查 */
```

### 9.6 在 nohz_full / isolated CPU 上调用 synchronize_rcu

在 `nohz_full=` 隔离的 CPU 上调用 `synchronize_rcu()` 本身没有错，但要
意识到：该 CPU 自身可能就是拖慢宽限期的那个——它关了 tick，若长时间在
内核态运行而不切换任务，GP kthread 要等它上报 QS，整个宽限期会被拉长。

如果对宽限期延迟有严格要求，在 isolated CPU 上优先使用
`synchronize_rcu_expedited()`（通过 IPI 强制各 CPU 立即过 QS），
或者避免在 isolated CPU 上调用同步版本，改用 `call_rcu()` 异步处理。

### 9.7 kfree_rcu 的 rcu_head 位置限制

`kfree_rcu(ptr, field)` 中的 `field` 必须是 `struct rcu_head` 类型，
且**不能位于结构体起始偏移超过 4096 字节处**（内核实现限制）。

```c
struct big {
    char padding[5000];
    struct rcu_head rcu;  /* 偏移 5000，超限！kfree_rcu 会 fallback 到 call_rcu */
};
/* 将 rcu_head 放到结构体靠前的位置 */
```

---

## 10. 与其他同步机制的对比

| 机制 | 读开销 | 写开销 | 读者能睡眠 | 适用场景 |
|------|--------|--------|-----------|---------|
| spinlock | 有（原子操作） | 有 | 否 | 读写均频繁，临界区极短 |
| rwlock | 有（原子操作） | 有 | 否 | 读多写少，但仍有锁争用 |
| mutex | 有（可睡眠） | 有 | 是 | 临界区较长，不在中断上下文 |
| RCU（经典） | **极低（通常零）** | 高（需等宽限期） | **否** | 读多写少，读性能关键 |
| SRCU | 低（每 CPU 计数） | 高 | **是** | 读临界区需要睡眠 |
| seqlock | 极低（读不锁） | 低 | 否 | 写频繁但读者可重试（如时间戳） |

**选型原则**：
- 读远多于写，且读者不睡眠 → **经典 RCU**
- 读远多于写，读者需要睡眠 → **SRCU**
- 写也很频繁，或数据量小 → **spinlock/mutex**
- 数据是单调递增的计数器/时间戳 → **seqlock**

---

## 11. 调试工具

### 11.1 CONFIG_PROVE_RCU（推荐开发时开启）

开启后：
- `rcu_test_sync_prims()` 在每次 RCU 状态机切换时验证同步原语行为
- `rcu_dereference_check()` 在未持 rcu_read_lock 时访问 RCU 指针会报警告
- `rcu_sr_normal_complete()` 验证回调触发时确实经历了完整宽限期

### 11.2 lockdep-RCU

锁依赖检测的 RCU 扩展，能检测：
- 在 RCU 临界区内调用 `synchronize_rcu()`（死锁）
- 在非 RCU 保护的上下文中使用 `rcu_dereference()`

### 11.3 RCU CPU stall 检测

如果一个 CPU 在宽限期内长时间不上报 QS（默认 21 秒），内核会打印：

```
rcu: INFO: rcu_preempt detected stalls on CPUs/tasks:
rcu:     0-...0: (1 GPs behind) idle=...
```

常见原因：CPU 陷入长时间关中断的循环、spinlock 死锁、RCU 临界区内意外睡眠。

### 11.4 rcutorture

`CONFIG_RCU_TORTURE_TEST` 提供压力测试模块，可通过 `tools/testing/selftests/rcutorture/`
驱动，用于验证 RCU 实现的正确性。

---

## 延伸阅读

- `Documentation/RCU/` — 内核官方 RCU 文档目录
- `Documentation/RCU/Design/Memory-Ordering/Tree-RCU-Memory-Ordering.rst` — 内存序实现细节
- `Documentation/RCU/whatisRCU.rst` — Paul McKenney 的经典入门文档
- `kernel/rcu/tree.c` — Tree RCU 核心实现
- `kernel/rcu/update.c` — RCU 通用工具函数与启动逻辑
- `include/linux/rcupdate.h` — 所有对外 API 定义
