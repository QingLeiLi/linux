# Linux 内核锁子系统入门与源码学习路线

> 适用内核版本：v7.2-rc3（基于当前仓库 `679f04aeef73`）
> 核心范围：`kernel/locking/`、`include/linux/{spinlock,mutex,rtmutex,rwsem,semaphore,percpu-rwsem}.h`
> 文档定位：这是一份学习路线和机制地图，不是 API 参考手册。它告诉你先建立什么模型、再读哪些
> 源码、每一阶段要回答什么问题，以及如何用实验验证理解。

---

## 目录

1. [先从一个真实问题开始](#1-先从一个真实问题开始)
2. [学习锁之前必须分清的五件事](#2-学习锁之前必须分清的五件事)
3. [Linux 锁子系统的总体方案](#3-linux-锁子系统的总体方案)
4. [进入源码前的三层宏观地图](#4-进入源码前的三层宏观地图)
5. [贯穿案例：从无竞争获取到 PI 链交接](#5-贯穿案例从无竞争获取到-pi-链交接)
6. [核心对象和状态不变量](#6-核心对象和状态不变量)
7. [宏观学习阶段：按什么顺序学](#7-宏观学习阶段按什么顺序学)
8. [细节学习阶段：一个锁函数应该怎么读](#8-细节学习阶段一个锁函数应该怎么读)
9. [各类锁的细读重点](#9-各类锁的细读重点)
10. [PREEMPT_RT 为什么会改变你的直觉](#10-preempt_rt-为什么会改变你的直觉)
11. [常见问题与容易混淆的结论](#11-常见问题与容易混淆的结论)
12. [常见错误的学习方式](#12-常见错误的学习方式)
13. [实验与观测：怎样证明自己真的理解了](#13-实验与观测怎样证明自己真的理解了)
14. [分阶段验收清单](#14-分阶段验收清单)
15. [推荐源码阅读索引](#15-推荐源码阅读索引)
16. [进一步学习边界](#16-进一步学习边界)

---

## 1. 先从一个真实问题开始

设系统中有三个单 CPU 可运行的任务：

```text
H：高优先级，等待低延迟事件
M：中优先级，持续进行 CPU 密集计算
L：低优先级，持有锁 lock，正在更新共享对象
```

时间线如下：

```text
L 获得 lock
→ H 被事件唤醒并尝试获得 lock
→ H 因 lock 被 L 持有而阻塞
→ M 变为 runnable，并因为优先级高于 L 而抢占 L
→ L 无法运行，也就无法释放 lock
→ H 虽然优先级最高，却被与 lock 无关的 M 间接拖住
```

这不是普通的“高优先级任务等低优先级任务”那么简单。真正危险的是等待时间没有上界：只要 M
持续运行，L 就无法完成临界区，H 也无法继续。这叫无界优先级反转。

一个锁实现至少同时面对五个目标：

1. 互斥或读写共享语义必须正确；
2. 无竞争路径必须足够短，不能每次都调度或操作复杂队列；
3. 竞争时不能丢失唤醒，也不能让两个任务同时认为自己获得了锁；
4. 锁对象、owner 和 waiter 的生命周期必须跨越并发窗口仍然有效；
5. 实时场景还要约束优先级反转，必要时沿嵌套锁链传播优先级。

最简单的全局原子标志只能回答“现在是否有人占用”。它回答不了：谁是 owner、谁应该先获得、
等待者能否睡眠、如何唤醒、能否从中断上下文使用、是否允许非 owner 释放、以及如何处理 PI 链。
`kernel/locking/` 的复杂性，正来自这些问题必须同时成立。

本文一直用上面的 H/M/L 案例。学习普通 spinlock/mutex 时关注互斥与等待方式；学习 rtmutex 时再把
H 的优先级捐赠给 L；学习 rwsem/percpu-rwsem 时把单一 owner 扩展为“多个 reader 或一个 writer”。

---

## 2. 学习锁之前必须分清的五件事

### 2.1 当前执行上下文能不能睡眠

先问上下文，再选锁：

| 上下文 | 能否普通睡眠 | 常见选择 |
|---|---:|---|
| 可抢占任务上下文 | 可以 | mutex、rt_mutex、rwsem、semaphore |
| 原子区/持 raw spinlock | 不可以 | raw_spinlock、原子操作、重新设计边界 |
| hardirq/NMI | 不可以 | raw_spinlock 或专用无锁协议 |
| softirq | 取决于配置与设计 | 通常使用 `_bh`、raw/local 保护并核对 PREEMPT_RT |
| PREEMPT_RT 上的线程化中断路径 | 不能照搬普通内核直觉 | 区分 `spinlock_t` 与 `raw_spinlock_t` |

`might_sleep()` 是诊断，不是让代码获得睡眠能力。持有 raw spinlock、关闭中断或抢占计数不允许调度
时，即使调用的是“睡眠锁 API”，也只是把错误推迟到调试告警或死锁。

### 2.2 等待方式：自旋、乐观自旋还是睡眠

三者解决的是不同成本区间：

```text
自旋：
  临界区很短，等待期间持续占用 CPU；适合不可睡眠上下文。

乐观自旋：
  mutex/rwsem 本质可睡眠，但若 owner 正在 CPU 上运行，短暂自旋可能比睡眠—唤醒便宜。
  owner 下线、需要调度或公平条件不满足时必须退出并进入睡眠路径。

睡眠：
  waiter 入队并让出 CPU；适合临界区可能较长，但要付出调度、队列和唤醒成本。
```

“自旋锁一定快”是错误命题。锁类型必须匹配上下文和临界区，不是按名称判断性能。

### 2.3 owner 语义与计数语义

mutex、rtmutex、spinlock 等通常要求获取者释放。semaphore 不记录严格 owner，`up()` 可以由另一个
上下文调用；这使它能表达资源计数或事件，但也使优先级继承失去明确捐赠目标。

因此二值 semaphore 不是“换个名字的 mutex”：

- mutex 能检查 owner、递归获取和错误释放；
- rtmutex 能把 waiter 优先级捐赠给 owner；
- semaphore 没有明确 owner，PREEMPT_RT 也无法为它提供同样的 PI 保证。

### 2.4 状态互斥与对象生命周期不是同一件事

锁保护字段或结构不变量，但通常不自动延长“锁对象所在内存”的生命。

例如 `mutex_unlock()` 的内部慢路径可能在逻辑上释放 owner 后仍访问 mutex 结构以完成唤醒。因此另一个
任务不能刚获得锁就立即释放包含 mutex 的对象，并假设前一个 unlock 已不再访问它。对象生命周期要由
引用计数、外部锁、RCU 或更高层协议保证。

同理：

- task 引用只保证 `task_struct` 不释放，不冻结 `task->prio`；
- wait_lock 稳定 rtmutex waiter/owner 状态，不自动持有外层 pi_state；
- RCU 保护读侧内存寿命，不替代修改方的状态锁。

### 2.5 锁还承担内存序

正确的 lock/unlock 不只排斥并发执行，还建立临界区写入的发布—观察关系：后来的获取者应该看到前一
owner 在释放前完成的写入。源码中的 `cmpxchg_acquire()`、`cmpxchg_release()`、自旋锁 acquire/release
和显式屏障是在实现这一契约。

不要把 `READ_ONCE()`/`WRITE_ONCE()` 当成锁。它们主要限制编译器合并、撕裂或重复访问，不自动提供
完整互斥、对象生命周期或跨字段事务一致性。

---

## 3. Linux 锁子系统的总体方案

Linux 没有一把万能锁，而是把语义、上下文和性能目标拆成多种机制：

| 类型 | 等待方式 | owner | 主要目标 | 当前核心实现 |
|---|---|---|---|---|
| raw spinlock | 忙等 | 严格上下文 | 最底层、不可睡眠临界区 | 架构 fastpath + qspinlock slowpath |
| spinlock_t | 普通内核忙等；RT 可睡眠 | 严格 | 通用短临界区 | 普通 spinlock / PREEMPT_RT rtmutex 包装 |
| rwlock_t | 普通内核读共享忙等；RT 可睡眠 | 读写语义 | 短读写临界区 | qrwlock / PREEMPT_RT rwbase |
| mutex | 睡眠，可乐观自旋 | 严格 task owner | 普通任务互斥 | tagged owner + wait queue + OSQ |
| ww_mutex | 睡眠 | task + acquire context | 多锁集合的可恢复死锁消解 | mutex/rtmutex + Wound-Wait/Wait-Die |
| rt_mutex | 睡眠，可有限 owner spin | 严格 task owner | 优先级继承 | 双红黑树 + PI chainwalk |
| rw_semaphore | 多 reader/单 writer睡眠 | 默认严格，reader 有特例 | 较长读写临界区 | 原子 count/owner + waiter queue；RT 用 rwbase |
| percpu_rw_semaphore | reader 极轻，writer 很重 | 读写语义 | 读极多、写极少 | per-CPU count + rcu_sync + writer 等待 |
| semaphore | 睡眠/trylock | 无严格 owner | 计数资源或传统同步 | count + FIFO waiter queue |

总体设计不是“每种锁各写一套完全独立代码”，而是组合复用：

```text
公开 API/配置层
  include/linux/*.h
          │
          ├── 普通 spin/rwlock → 架构 fastpath + qspinlock/qrwlock slowpath
          ├── 普通 mutex       → mutex.c
          ├── rt_mutex         → rtmutex_api.c 包含 rtmutex.c
          ├── ww_mutex         → 普通 mutex 或 WW_RT 参数化 rtmutex
          ├── PREEMPT_RT spin  → spinlock_rt.c 包含 rtmutex/rwbase 核心
          ├── rwsem            → rwsem.c；PREEMPT_RT 选择 rwbase_rt
          └── 验证             → lockdep、lock events、locktorture、自测试
```

这种方案的收益是：常见无竞争路径可以保持非常短，同时竞争路径仍提供公平、调试、睡眠和 PI 等完整
语义。代价是配置分支和文本包含较多，同一个 API 在 PREEMPT_RT 下可能落到完全不同的数据结构。

---

## 4. 进入源码前的三层宏观地图

### 4.1 子系统边界

```text
调用者
  驱动、VFS、MM、调度器、futex、RCU……
        │
        ▼
公开语义层
  include/linux/spinlock.h
  include/linux/mutex.h
  include/linux/rtmutex.h
  include/linux/rwsem.h
  include/linux/percpu-rwsem.h
  include/linux/semaphore.h
        │
        ▼
核心算法层：kernel/locking/
  qspinlock / qrwlock
  mutex / ww_mutex / osq
  rtmutex / rwbase_rt
  rwsem / percpu-rwsem / semaphore
        │
        ├── 调度器：阻塞、唤醒、有效优先级与 PI
        ├── 架构原子操作：cmpxchg、xchg、barrier
        ├── lockdep：依赖图与上下文验证
        └── trace/统计/torture：观测与压力验证
```

锁子系统不拥有被保护对象。它只管理锁状态、等待者和同步边界；“锁对象什么时候可以销毁”仍由调用
该锁的上层模块负责。

### 4.2 对象关系

普通 mutex 的简化关系：

```text
struct mutex
├── owner：task 指针 + WAITERS/HANDOFF/PICKUP 等低位状态
├── wait_lock：保护 waiter 队列和交接状态
├── first_waiter：等待队列队首的缓存入口
├── osq：限制乐观 spinner
└── dep_map：lockdep 类与依赖状态

阻塞任务的栈
└── struct mutex_waiter
    ├── list：连接 mutex 等待队列
    ├── task：等待任务
    └── ww_ctx：普通 mutex 为 NULL，ww_mutex 携带获取上下文
```

`wake_q` 不嵌在 `struct mutex_waiter` 里。需要批量唤醒时，慢路径另外创建局部 `wake_q`，在
`wait_lock` 保护下收集任务，放锁后再统一唤醒。

rtmutex 多一层 PI 关系：

```text
每把锁 lock                         每个 owner task
lock->waiters                       task->pi_waiters
保存该锁的全部 waiter               只保存该 task 所持每把锁的 top waiter
按 prio/deadline 排序               按 donor prio/deadline 排序
由 lock->wait_lock 保护             由 task->pi_lock 保护
          │                                  │
          └──── top waiter 作为 donor ───────┘
```

为什么 task PI 树只保存每把锁的 top waiter？因为同一把锁上只有最高优先级 waiter 能决定当前 owner
需要提升到什么程度；把其余 waiter 全部重复挂入 owner 树只会增加重排成本，不改变最高捐赠结果。

### 4.3 一次获取的数据流

```text
调用 lock()
→ 无竞争原子 fastpath
  → 成功：发布 current 为 owner，进入临界区
  → 失败：读取竞争状态
→ 可选乐观自旋/有限 owner spin
  → owner 即将释放：再次原子获取
  → owner 下线、need_resched、公平条件失败：停止自旋
→ slowpath
  → 取得内部 wait_lock
  → 复查锁（关闭 fastpath 与加锁之间的竞态）
  → 构造并入队栈上 waiter
  → 必要时传播 PI
  → 设置 task state，释放内部锁，schedule
→ 被唤醒后重新取得内部锁并再次尝试
→ 成功出队、清 blocked_on、成为 owner
→ unlock 时清 owner或选择 top waiter交接
```

唤醒不等于获得锁。wake 只让 waiter 重新 runnable；最终 ownership 必须在锁实现的串行化边界内重新
确认。理解这一点可以避免把所有等待队列代码误读成“被 wake 的任务已持锁”。

---

## 5. 贯穿案例：从无竞争获取到 PI 链交接

### 5.1 阶段一：L 无竞争获取

普通 mutex/rtmutex 首先尝试把空 owner 原子替换为 `current`：

```text
入口：owner = NULL，无 waiter
原子 acquire cmpxchg(NULL → L)
出口：owner = L，L 可观察前一 owner release 前的临界区写入
```

这是性能关键路径：不建立 waiter、不调度、不操作 PI 树。调试配置或架构缺少所需原子能力时，同一
语义会退化到内部 wait_lock 保护的慢路径，但 API 的成功保证不变。

### 5.2 阶段二：H 到来并发现竞争

H 的 fastpath 失败后不能直接睡眠，必须先关闭三个竞态：

1. fastpath 失败后 L 可能已经释放；所以持 wait_lock 后必须复查；
2. H 必须先把 waiter/睡眠状态发布，再允许 L 唤醒，否则可能丢失唤醒；
3. H 的栈上 waiter 只有在仍处于阻塞调用栈时才有效，所有出队路径必须在函数返回前完成。

普通 mutex 可能先对正在 CPU 上运行的 L 乐观自旋。若 L 很快释放，自旋避免一次 schedule；若 L 已
下线或 H 需要调度，则继续占 CPU 没有收益，必须排队睡眠。

### 5.3 阶段三：rtmutex 建立优先级捐赠

在 rtmutex 中，H 入 `lock->waiters` 后成为 top waiter。锁实现把 H 对应 waiter 的 `pi_tree` 节点挂入
L 的 `task->pi_waiters`，再通过调度器把 L 的有效优先级提高到 H 的等级：

```text
H(prio 10) waits on lock owned by L(prio 80)

lock->waiters.top = H
L->pi_waiters.top = H
L effective prio: 80 → 10
```

现在 M(prio 50) 不能继续抢占提升后的 L。L 得以运行并尽快释放 lock，从而把 H 的阻塞时间限制在
L 的临界区和更深的锁依赖上，而不是受所有中优先级工作任意拖延。

### 5.4 阶段四：PI 链向更深 owner 传播

若 L 自己又阻塞在 K 持有的另一把锁上：

```text
H → lock1 → L → lock2 → K
```

只提升 L 没有用，因为 L 仍不能运行。`rt_mutex_adjust_prio_chain()` 必须继续把捐赠传播到 K。每一步
围绕以下关系反复验证：

```text
task->pi_blocked_on 指向哪个 waiter？
waiter->lock 还是刚才记录的 next_lock 吗？
该 waiter 仍是决定 owner 优先级的 top waiter 吗？
下一把 lock 的 owner 是否变化？
是否回到 orig_lock/top_task 形成环？
```

chainwalk 不能长期同时锁住整条链，否则锁顺序和延迟都不可接受。它每级只持有限的 `pi_lock` 与
`wait_lock`，用 task 引用跨越放锁窗口，并在重新进入时验证链是否已被并发解锁或重排改变。

### 5.5 阶段五：L 解锁、deboost 与 H 唤醒

有 waiter 时，解锁不能简单写 `owner = NULL` 后随便 wake 一个任务。rtmutex 的交接顺序是：

```text
持 wait_lock
→ 找到 top waiter H
→ 从 L->pi_waiters 移除 H donor
→ 按剩余 donor 重算 L 的有效优先级（deboost）
→ owner 进入 NULL|HAS_WAITERS 交接态，阻止低优先级 fastpath 偷锁
→ 把 H 加入锁外 wake_q
→ 释放内部锁
→ 真正唤醒 H
→ H 重新竞争并成为 owner
```

deboost 后到 H 真正入 runnable 队列之间若允许 L 被抢占，可能重新产生优先级反转。因此实现会在这个
短窗口保持抢占关闭，并由 wake_q 消费函数配对恢复。

### 5.6 阶段六：H 超时或收到信号

可中断锁或 PI-futex proxy 等路径不能只返回错误。若 waiter 已经入树，它必须：

```text
从 lock->waiters 摘除
→ 清 task->pi_blocked_on
→ 若它原来是 top，从 owner->pi_waiters 替换/删除 donor
→ 重算 owner 有效优先级
→ 必要时继续向 PI 链传播 deboost
→ 修复 owner 的 HAS_WAITERS 过渡位
→ 恢复 TASK_RUNNING
→ 才能让 waiter 所在栈帧返回
```

PI-futex 还有一个特殊竞态：等待函数可能先报告超时，但并发 unlock 已经开始把 ownership 授予该
waiter。cleanup 必须在 wait_lock 下再次 trylock 并检查最终 owner；若 current 已获得锁，调用者要忽略
原超时/信号错误。错误码只是一次观察，owner 状态才是最终事实。

---

## 6. 核心对象和状态不变量

### 6.1 tagged owner 指针

mutex/rtmutex 利用 `task_struct` 指针对齐后恒为零的低位保存状态标志。概念上：

```text
真实指针部分：当前 owner task
低位标志：     是否有 waiter、是否要求 handoff/pickup 等
```

因此读取 owner 时不能直接把原始整数当 task 指针解引用；必须屏蔽标志。fastpath 也通常只在 owner
指针和所有状态位都为空时成功。先置 waiter 位可以阻止新 fastpath 越过正在建立的慢路径窗口。

rtmutex 的最小状态可理解为：

| owner 指针 | HAS_WAITERS | 含义 |
|---|---:|---|
| NULL | 0 | 完全空闲，可 fast acquire |
| NULL | 1 | 空闲交接态，top waiter 应优先接管 |
| task | 0 | 已持有，无 waiter，可 fast release |
| task | 1 | 已持有，有 waiter或慢路径过渡态 |

过渡态不是错误，但必须由 wait_lock 串行，并在退出 slow/try/error 路径前由 set/fixup helper 收敛。

### 6.2 waiter 为什么常放在任务栈上

阻塞调用尚未返回时，其栈帧一直存在，waiter 可以直接记录 task、lock 和树/队列节点，避免额外分配。
代价是生命周期契约极其严格：函数返回前节点必须从所有共享树中摘除，其他 CPU 不能再通过 wake_q、
PI 树或等待队列访问它。

阅读任一慢路径时都要建立配对表：

| 建立 | 撤销/转移 |
|---|---|
| waiter 入 lock 队列 | 成功接管或错误 cleanup 出队 |
| task->pi_blocked_on = waiter | 成功或撤销时清 NULL |
| waiter 成为 owner donor | top 改变、解锁或取消时从 pi_waiters 移除 |
| get_task_struct(owner) | chainwalk 各出口 put_task_struct |
| task 加入 wake_q | wake_up_q 消费其队列引用 |

### 6.3 wait_lock 与 pi_lock 分别保护什么

- `lock->wait_lock`：owner tagged 状态、该锁的 waiter 树/队列及 top waiter 身份；
- `task->pi_lock`：task 的 `pi_blocked_on`、`pi_waiters` 和与有效优先级调整相关的 PI 状态；
- task 引用：只保证任务内存存在，不能替代上述任一状态锁。

常规次序通常是 wait_lock → pi_lock。PI chainwalk 已持某 task 的 pi_lock 后要追到下一把 lock 时形成
反序需求，因此使用 trylock：失败就释放 pi_lock、放松 CPU 后重试，而不是阻塞等待制造 ABBA。

### 6.4 rwsem 不只是“带计数的 mutex”

普通 rwsem 同时编码 reader 数、writer/等待状态，并维护 owner、OSQ 和 waiter 队列：

```text
无 writer 且策略允许 → reader 可并发增加 count
writer 获取            → 独占状态
发生竞争               → wait_lock 下排队，按 reader/writer 类型决定唤醒批次
owner 正在运行          → 配置允许时 optimistic spin
```

reader 没有唯一 owner，导致 PI 比 mutex 更困难。PREEMPT_RT 使用 `rwbase_rt` 重新实现时，会让 writer
可以接受 reader 的优先级捐赠，但无法把一个高优先级 writer 同时捐赠给多个低优先级 reader。这也是
RT 配置下公平性取舍改变的根源。

### 6.5 percpu-rwsem 把成本偏向 writer

reader fastpath 在禁抢占区增加本 CPU 计数，避免所有 CPU 修改同一 cacheline。writer 切换
`rcu_sync` 状态，等待旧 fast reader 经过同步边界，再汇总/等待 per-CPU reader 离场。

收益：读极多、写极少时扩展性好。代价：writer 获取昂贵，状态切换涉及 RCU 同步，而且 reader 路径
必须用禁抢占保证“本 CPU 计数”和当前 CPU 身份一致。

---

## 7. 宏观学习阶段：按什么顺序学

不要按文件名字母顺序阅读。推荐按“上下文基础→忙等→睡眠→死锁消解→PI→读写锁→验证”推进。

### 阶段 0：准备知识与边界

先掌握：

- task state、`schedule()`、wake_q 和等待队列；
- 抢占计数、IRQ/softirq/task context；
- 原子 cmpxchg、acquire/release、`READ_ONCE()`；
- intrusive list/rbtree、`container_of()`；
- task 基础优先级、有效优先级和调度类；
- 引用、锁、RCU 分别保证什么。

建议入口：

- `doc/11 linux-scheduler-internals.md` 的睡眠/唤醒、实时调度和 PI 章节；
- `Documentation/locking/locktypes.rst`；
- `Documentation/locking/locktorture.rst`。

完成标志：看到一个 lock API，能先判断上下文能否睡眠，而不是先猜实现快不快。

### 阶段 1：raw spinlock、qspinlock 与 qrwlock

阅读顺序：

```text
include/linux/spinlock*.h / arch atomic fastpath
→ kernel/locking/spinlock.c
→ kernel/locking/qspinlock.h
→ kernel/locking/qspinlock.c
→ kernel/locking/qrwlock.c
```

先回答：

1. 为什么持 spinlock 会禁抢占？
2. `_irq`、`_irqsave`、`_bh` 分别关闭哪个并发来源？
3. qspinlock 的 locked/pending/tail 状态如何避免所有 waiter 打同一 cacheline？
4. pending 快速过渡为什么不能替代 MCS 排队？
5. paravirt 路径为何需要 halt/kick，而不是让被抢占 vCPU 永久忙等？

本阶段不要先钻每个位运算。先画出：无竞争锁定→pending 竞争→MCS 节点排队→head 接管→unlock 发布。

### 阶段 2：普通 mutex 与 OSQ

阅读顺序：

```text
include/linux/mutex.h
→ kernel/locking/mutex.h
→ kernel/locking/mutex.c 的 fast try/unlock
→ osq_lock.c
→ mutex_optimistic_spin()
→ __mutex_lock_common()
→ __mutex_unlock_slowpath()
```

先建立三段模型：

```text
fastpath 原子获取
→ midpath owner 乐观自旋
→ slowpath waiter 入队睡眠
```

重点追踪 owner 低位、handoff/pickup、防止 starvation 的交接，以及为什么唤醒后仍需重新确认 owner。

完成标志：能解释“mutex 是睡眠锁，为什么源码里仍有大量 spin”。

### 阶段 3：ww_mutex 的多锁死锁消解

阅读顺序：

```text
include/linux/ww_mutex.h
→ kernel/locking/ww_mutex.h
→ mutex.c 中 ww 包装
→ ww_rt_mutex.c
→ test-ww_mutex.c
```

普通 lockdep 是发现错误；ww_mutex 的目标是让合法业务在获取未知数量/顺序的锁时能够退避并重试。
学习 `acquire_ctx`、stamp、Wound-Wait 与 Wait-Die，关注 `-EDEADLK` 后谁释放已经获得的锁、谁等待
contended lock、以及重试时如何保证前进性。

完成标志：能写出“获取集合→收到 -EDEADLK→逆序释放→slow acquire contended→重试”的状态机。

### 阶段 4：rtmutex 与优先级继承

阅读顺序：

```text
Documentation/locking/rt-mutex.rst
→ Documentation/locking/rt-mutex-design.rst
→ include/linux/rtmutex.h
→ rtmutex_common.h
→ rtmutex.c 的 owner/双树 helper
→ try_to_take_rt_mutex()
→ task_blocks_on_rt_mutex()
→ rt_mutex_adjust_prio_chain()
→ slowlock/unlock
→ rtmutex_api.c
```

第一次只追 H→L；第二次再追 H→L→K；第三次才看 deadlock、WW 假环、proxy futex 和配置分支。

完成标志：可以画出两棵红黑树、两把内部锁、task 引用的获取/释放，以及解锁 deboost→wake 顺序。

### 阶段 5：rwsem、rwbase_rt 与 percpu-rwsem

阅读顺序：

```text
include/linux/rwsem.h
→ rwsem.c 的 count/owner fastpath
→ waiter 类型与 wake 批次
→ optimistic spin
→ read/write slowpath 与 unlock
→ include/linux/rwbase_rt.h + rwbase_rt.c
→ include/linux/percpu-rwsem.h + percpu-rwsem.c
```

用同一组问题比较三种设计：reader fastpath 改哪条 cacheline？writer 如何阻止新 reader？谁负责唤醒
一批 reader？writer starvation 如何避免？PREEMPT_RT 能向谁传播优先级？

完成标志：能解释为什么 rwsem 适合一般读写临界区，而 percpu-rwsem 只适合极端读多写少。

### 阶段 6：semaphore、lockdep 与验证工具

阅读顺序：

```text
include/linux/semaphore.h → semaphore.c
lockdep_states.h → lockdep_internals.h → lockdep.c → lockdep_proc.c
lock_events_list.h → lock_events.h/c
locktorture.c → test-ww_mutex.c
```

学习 semaphore 时重点是“无 owner”带来的语义差异；学习 lockdep 时重点是它验证依赖图和上下文，
不替代真实锁；学习 torture 时重点是如何制造竞争、嵌套和停顿，而不是只看最终 PASS/FAIL。

---

## 8. 细节学习阶段：一个锁函数应该怎么读

对每个核心函数固定做三遍，而不是一遍读完所有语句。

### 8.1 第一遍：控制骨架

只记录：

```text
调用者是谁
入口持什么锁/中断状态
fastpath 条件
什么时候进入队列
什么时候 schedule
成功和错误有哪些出口
返回后谁继续
```

把函数压缩成 5～15 个节点。若压缩不了，说明你还在被局部语法牵着走。

### 8.2 第二遍：状态与 ownership

为关键对象画表：

| 对象 | 入口 | 中间 | 成功出口 | 错误出口 |
|---|---|---|---|---|
| owner | 原 task/NULL | tagged 过渡态 | current | 原 owner/修复态 |
| waiter | 未入队 | lock 树 + 可能 PI 树 | 已出队 | cleanup 已出队 |
| task state | RUNNING | INTERRUPTIBLE/UNINTERRUPTIBLE | RUNNING | RUNNING |
| task 引用 | 借用 | chainwalk 可能 get | 已消费 | 已消费 |
| wake_q | 空 | 收集待唤醒 task | 锁外消费 | 锁外消费 |

每见一个 `get`、入队、字段发布，就马上找对应的 `put`、出队和清除。找不到时先记为问题，不能凭感觉
假定别处会处理。

### 8.3 第三遍：并发与内存序

对关键操作强制回答：

1. 竞争双方是哪两个 CPU/任务？
2. 它们读写哪个字段？
3. 为什么当前锁足以稳定它？
4. 释放锁后裸指针为什么仍可或不可使用？
5. acquire/release 与哪条路径配对？
6. 如果删除该位、屏障或复查，具体会出现“双 owner、丢唤醒、UAF、错误 PI”中的哪一种？

### 8.4 宏和条件编译的阅读方法

先看最终翻译单元，而不是把 `.c` 文件名当成唯一实现：

- `rtmutex_api.c` 定义 `RT_MUTEX_BUILD_MUTEX` 后包含 `rtmutex.c`；
- `ww_rt_mutex.c` 以 WW_RT 参数再次生成共享核心；
- `spinlock_rt.c` 为 PREEMPT_RT sleeping spin/rwlock 生成另一套入口；
- `rwsem.c` 在 PREEMPT_RT 下通过 `rwbase_rt` 改变实现。

建议每次写一张配置表：

| 配置 | 公共 API | 真正核心 | 关键语义变化 |
|---|---|---|---|
| !PREEMPT_RT mutex | `mutex_lock` | `mutex.c` | 普通 mutex owner/OSQ/waiter |
| PREEMPT_RT mutex | 同名 API | `rtmutex_api.c` | 底层 rtmutex，具备 PI |
| !PREEMPT_RT spinlock_t | spin API | 架构/qspinlock | 忙等、禁抢占 |
| PREEMPT_RT spinlock_t | 同名 API | `spinlock_rt.c` | 可睡眠 RT lock |
| raw_spinlock_t | raw API | 架构/qspinlock | 两种配置下仍不可睡眠 |

### 8.5 每读完一个函数都要留下的问题卡片

```text
函数：
一句话职责：
入口锁/上下文：
对象状态变化：
引用/队列 ownership：
fast/slow/error：
关键并发窗口：
返回保证：
尚未验证的问题：
下一处源码证据：
```

问题卡片能防止“看懂每一行但说不出整个协议”。

---

## 9. 各类锁的细读重点

### 9.1 qspinlock：不要一开始背位布局

第一遍只理解三条路径：空锁直接抢、短期 pending、严重竞争进入 MCS 队列。第二遍再看 per-CPU 节点
嵌套编号、tail 编码和 head 接管。第三遍分析 PV vCPU 被抢占时的 halt/kick。

关键问题：

- 为什么排队节点自旋本地字段能减少 cacheline bouncing？
- 为什么队首仍需要竞争全局 locked 字段？
- 节点耗尽时退化路径牺牲了什么？
- unlock 为什么通常比 lock slowpath 简单？

### 9.2 mutex：围绕 owner 状态机学习

不要把 `mutex_lock()` 看成一个函数；它是 fast/mid/slow 三套策略的汇合。重点理解 WAITERS、HANDOFF、
PICKUP 等状态如何防止新到任务长期越过已经排队者，以及 OSQ 为什么只允许有限 spinner 竞争 owner。

关键问题：

- owner 正在运行为什么是“值得自旋”的必要但非充分条件？
- 为什么 need_resched 时必须离开 OSQ？
- handoff 给 waiter 后为什么还需要 pickup 确认？
- unlock 已清 owner后为何仍可能访问 mutex 对象？

### 9.3 ww_mutex：把错误码看成协议事件

`-EDEADLK` 不是普通失败，而是通知当前 acquire context 退避。一定要从调用者追踪：它释放了哪些已经
持有的锁，如何等待 contended lock，再如何带同一 context 重试。

关键问题：

- stamp 如何定义“更老”和“更新”的事务？
- Wound-Wait 与 Wait-Die 谁主动 wound，谁主动退出？
- 为什么 ctx 必须覆盖完整的一批锁获取？
- WW 产生的暂态环为何不能直接当普通 rtmutex 死锁报告？

### 9.4 rtmutex：先学双树，再学 chainwalk

`rt_mutex_adjust_prio_chain()` 很长，但本质反复做四件事：验证链、重排当前 waiter、更新下一 owner 的
donor、决定继续或退出。先画每一步持锁表，再读 goto/retry；否则很容易把引用存活误当成状态稳定。

关键问题：

- 为什么一条 PI 链可以合并却不会从一个 task 向多个等待锁分叉？
- 为什么 task PI 树只保存各锁 top waiter？
- 为什么 chainwalk 最多持有限的两把内部锁？
- `next_lock` 放锁后为何只能比较地址、不能解引用？
- deboost 为什么也需要沿链传播？

### 9.5 rwsem：同时跟踪 reader 数和 waiter 类型

先把 reader fastpath、writer fastpath、reader slowpath、writer slowpath 分开。再看 wake 逻辑如何选择
一个 writer或一批 reader，以及 handoff 位如何限制偷取。

关键问题：

- reader 并发成功时谁是 owner？owner 字段只是精确事实还是投机提示？
- 为什么 reader optimistic spin 与 writer spin 的退出条件不同？
- 新 reader 何时允许越过等待 writer，何时会导致 starvation？
- downgrade_write 为什么和 up_write 后重新 down_read 不等价？

### 9.6 percpu-rwsem：用 cacheline 视角理解

把 64 CPU 同时 reader 的写入位置画出来：普通共享计数会反复搬运一条 cacheline；per-CPU count 让每
个 CPU 写自己的槽。随后再问 writer 如何冻结 fastpath、等待已有 reader 并阻止新 reader漏过同步点。

关键问题：

- reader 增减为什么必须禁抢占？
- `rcu_sync` 提供的是哪一个阶段边界？
- writer 为什么比普通 rwsem writer 更昂贵？
- 哪类负载会因写频繁而完全抵消 reader 收益？

### 9.7 semaphore：先确认你真的需要“无 owner”

新代码若只需要互斥，通常优先考虑 mutex；若只需要一次事件，考虑 completion。semaphore 的价值在于
计数许可或跨上下文 `down/up`，但无 owner 也意味着更弱的调试和 PI 能力。

关键问题：

- `down_trylock()` 的返回约定是否与 mutex_trylock 相同？
- `up()` 从中断上下文调用时，真正唤醒在哪里发生？
- count 与 waiter 队列如何避免“许可既被计数又被授予 waiter”两次消费？
- 为什么 PREEMPT_RT 不把 semaphore 自动变成 PI 锁？

---

## 10. PREEMPT_RT 为什么会改变你的直觉

普通内核中，`spinlock_t` 通常意味着忙等并禁抢占；PREEMPT_RT 为缩短不可抢占区，把大量
`spinlock_t`/`rwlock_t` 映射为可睡眠、基于 rtmutex/rwbase 的锁。`raw_spinlock_t` 保留真正忙等语义，
用于调度器、低层中断和实现 RT 锁本身所需的最小原子区域。

因此读代码时不要只看 API 名称，要同时看配置：

```text
spin_lock(&x)
  普通内核：通常禁抢占并忙等
  PREEMPT_RT：可能阻塞和调度，不能再假定隐式禁抢占

raw_spin_lock(&x)
  两者：保持不可睡眠的底层原子锁语义
```

这带来三类学习陷阱：

1. 依赖 `spin_lock()` 隐式禁止迁移来保护 per-CPU 指针，在 RT 上可能失效；
2. 在 spinlock_t 临界区调用只适用于原子上下文的 helper，配置切换后语义可能相反；
3. 锁嵌套顺序必须同时考虑 sleeping lock 与 raw lock，不能让可睡眠路径持 raw lock 阻塞。

学习 PREEMPT_RT 的正确方式不是背“哪些锁被替换”，而是对每段代码重新回答：当前能否调度、是否
保证 CPU 不迁移、是否关闭 IRQ、谁能获得 PI、内部实现使用哪种 raw 锁保护自身。

---

## 11. 常见问题与容易混淆的结论

### 11.1 spinlock 和 mutex 到底怎么选

先看上下文：不能睡眠才考虑 spin/raw spin。可以睡眠时再看临界区和语义，通常优先 mutex。不要用
“代码只有五行”作为唯一依据；这五行可能 page fault、调用回调或等待硬件。

### 11.2 mutex_trylock 可以在中断里用吗

不可以。`mutex_trylock()` 本次获取不会睡眠，并不等于 mutex 的使用上下文约束消失；当前
`Documentation/locking/mutex-design.rst` 明确禁止在 hardirq、softirq、tasklet、timer 等硬件或软件
中断上下文使用 mutex。若确实需要中断上下文 trylock，应选择符合该上下文的锁；传统 semaphore 的
`down_trylock()` 是一个语义不同、明确允许中断上下文调用的特例。

### 11.3 被 wake 的 waiter 是否已经拿到锁

通常没有。wake 让 task runnable；waiter 醒来后要重新取得内部锁并执行 try-to-take。handoff 模式可能
保留特定接管资格，但仍要由接收者完成 pickup/owner 状态转换。

### 11.4 公平队列为什么仍允许 steal

严格公平会增加交接延迟和空转。某些实现允许在限定条件下由当前运行者或更高优先级任务偷取空锁，
同时用 handoff、top-waiter 或 RT 优先级规则限制无界越过。公平通常是工程策略，不是所有 API 的 ABI。

### 11.5 PI 是否能解决所有优先级反转

不能。PI 只能沿有明确 owner 的锁依赖传播，不能提升未知 semaphore owner，也不能穿透长期关闭抢占/
中断的区域，更不能修复 CPU、I/O、内存回收等非锁资源造成的全部反转。

### 11.6 为什么不能只保存最高优先级 waiter

锁仍需保存全部 waiter，因为 top 离开、超时或获得锁后要找到下一个；owner 的 PI 树才只需要每把锁的
top donor。这是“锁队列完整性”和“owner 有效优先级摘要”的分工。

### 11.7 lockdep 能证明锁实现没有竞态吗

不能。lockdep 主要验证锁依赖顺序、递归、上下文和 usage 类别。它不会证明 tagged owner 位运算正确，
也不会发现所有缺失屏障、UAF 或业务字段未受锁保护。KCSAN、locktorture、代码审查和专门实验仍必要。

### 11.8 `mutex_is_locked()` 能否证明 current 持锁

不能。它只说明锁看起来非空，不提供 owner 证明，也常是 data-race 容忍的投机查询。若需要断言当前
持有关系，应使用 lockdep assert 或由调用协议保证。

### 11.9 为什么错误路径也要修复 waiter bit

慢路径通常先置 HAS_WAITERS 来阻止并发 fastpath，再检查 owner 和入队。trylock、信号或超时可能在
真正留下 waiter 之前退出；若不按真实队列清位，锁会永久退化慢路径，甚至破坏交接判断。

### 11.10 为什么解锁要在锁外 wake

直接在内部 wait_lock 下唤醒可能进入调度器并拉长 raw 临界区，还会制造锁顺序递归。wake_q 先取得
必要引用并记录任务，释放内部锁后再统一唤醒。

### 11.11 rwsem reader 多就一定比 mutex 快吗

不一定。读临界区太短、写频繁、cacheline 竞争或 NUMA 成本都可能让 rwsem 更差。读写锁只在并行读
带来的工作重叠足以覆盖更复杂状态和 writer 协调成本时有收益。

### 11.12 `down_trylock()` 为什么容易写反

mutex/rwsem trylock 通常成功返回 1；传统 semaphore `down_trylock()` 成功返回 0、失败非零。必须按
具体 API 检查，不能凭“trylock”名字统一推断。

---

## 12. 常见错误的学习方式

### 12.1 一开始就背所有字段和位

修正：先画 fast→spin→queue→sleep→wake→handoff 的状态流程，再把每个位放回它关闭的竞态窗口。

### 12.2 只读 lock，不读 unlock 和取消路径

锁的公平性、release 内存序、deboost、wake 和对象寿命通常在 unlock/cleanup 中才能闭环。至少成对读：

```text
lock ↔ unlock
enqueue ↔ dequeue
get reference ↔ put reference
set blocked_on ↔ clear blocked_on
preempt_disable ↔ preempt_enable
```

### 12.3 把注释中的“protected”理解成永久不变

锁只在持有期间稳定对应字段。放锁后保存的地址可能只适合比较，不能解引用；引用只保内存，不保字段。

### 12.4 忽略配置和体系结构

只读 `mutex.c` 就断言 PREEMPT_RT mutex 的实现，或只读 qspinlock slowpath 就断言所有架构 fastpath，
都会得到错误模型。每次结论都标明普通/RT、SMP/UP、debug/非 debug 和 PV/native 边界。

### 12.5 用一次无竞争 benchmark 评价锁

无竞争只测 fastpath。至少分别观察：单线程、同 CPU 竞争、跨 CPU 竞争、owner 被抢占、长临界区、
reader/writer 不同比例，以及实时优先级链。

### 12.6 看见 `data_race()` 就认为竞态已经解决

`data_race()` 是对有意无锁观察的标注，表示该值只作启发式判断或稍后会在锁下复核。它不消除竞态。

---

## 13. 实验与观测：怎样证明自己真的理解了

### 13.1 实验一：观察锁竞争 tracepoint

前提：内核启用 tracefs 和相应 lock contention tracepoint，具备 root 权限。

```bash
mount -t tracefs tracefs /sys/kernel/tracing 2>/dev/null || true
cd /sys/kernel/tracing
echo 0 > tracing_on
echo > trace
echo 1 > events/lock/contention_begin/enable
echo 1 > events/lock/contention_end/enable
echo 1 > tracing_on
# 在另一终端运行会制造 mutex/rwsem 竞争的测试负载
sleep 5
echo 0 > tracing_on
cat trace
```

要验证的不是“出现了事件”这么简单，而是：begin/end 能否配对，等待时长是否随临界区延长，事件中的
锁类型标志是否符合所测 API。trace 没记录不等于没有竞争，还可能是配置、事件类别或过滤条件不匹配。

### 13.2 实验二：读取 `/proc/lock_stat`

前提：`CONFIG_LOCKDEP=y`、`CONFIG_LOCK_STAT=y`、procfs 可用。

```bash
echo 1 > /proc/sys/kernel/lock_stat
cat /proc/lock_stat | head -n 40
echo 0 > /proc/lock_stat
```

关注 contention 次数、等待时间、持有时间和调用点。它能帮助定位“哪一类锁争用”，不能单独证明业务
临界区为何变长，也不是某一瞬间的全局原子快照。

### 13.3 实验三：运行 locktorture

前提：`CONFIG_LOCK_TORTURE_TEST=m` 或内建，并在隔离测试环境运行；它会主动制造高竞争和调度压力。

```bash
modprobe locktorture torture_type=mutex_lock stat_interval=10
dmesg -w
# 观察足够时间后
rmmod locktorture
```

随后分别尝试文档支持的 `spin_lock`、`rw_lock`、`rtmutex_lock`、`rwsem_lock`、`percpu_rwsem_lock` 等
类型。先阅读 `Documentation/locking/locktorture.rst` 和当前 `locktorture.c` 参数检查，不要在生产机
盲目加载。

预期证据：writer/reader 进度持续增长、无 lock failure、模块卸载完成。实验能说明压力条件下未观察到
错误，不能形式化证明所有交错都正确。

### 13.4 实验四：构造 H/M/L 优先级反转

写一个测试模块或内核自测，创建三个绑在同一 CPU 的 kthread：

```text
L：先持 rt_mutex，等待控制事件后忙循环一段时间再释放
H：设置高 RT 优先级，等待 L 持锁后尝试获取
M：中间 RT 优先级，在 H 阻塞后持续 runnable
```

用 `sched_switch`、`sched_wakeup` 和 lock contention 事件观察：H 阻塞后 L 的有效优先级是否提升，M 是否
仍能长期压制 L，L 解锁后是否 deboost，H 是否随后获得锁。

这个实验需要谨慎设置运行时上限和停止条件，避免 FIFO 线程锁死测试机。它验证当前配置中的 PI 行为，
不证明 semaphore、普通非 PI mutex 或关闭抢占区也具有同样保证。

### 13.5 实验五：用代码审计验证“仅增加注释”

学习过程中若给源码添加注释，至少执行：

```bash
git diff --check -- kernel/locking
./scripts/checkpatch.pl --no-tree --file kernel/locking/<目标文件>
```

并把原文件与修改后文件做逐行子序列核对，确认原代码和原注释完全保留。格式检查只能证明补丁卫生，
不能替代对锁语义、函数契约和配置分支的完整复读。

---

## 14. 分阶段验收清单

### 14.1 宏观模型验收

- [ ] 能按“上下文能否睡眠”初选锁类型；
- [ ] 能区分自旋、乐观自旋和睡眠等待；
- [ ] 能说明 owner、waiter、被保护对象三者的生命周期不同；
- [ ] 能解释 lock/unlock 的 acquire/release 作用；
- [ ] 能画出普通内核与 PREEMPT_RT 的实现分派；
- [ ] 能解释 semaphore 为什么没有 PI。

### 14.2 mutex 验收

- [ ] 能复述 fastpath、OSQ midpath、sleep slowpath；
- [ ] 能解释 owner 低位和 handoff/pickup；
- [ ] 能说明 wake 不等于 ownership；
- [ ] 能画出 waiter 栈生命周期和错误出队；
- [ ] 能解释 unlock 返回前为何对象仍必须存活。

### 14.3 rtmutex 验收

- [ ] 能画出 lock waiter 树与 task PI 树；
- [ ] 能用 H/M/L 解释 PI；
- [ ] 能把 H→L→K 的链传播完整复述；
- [ ] 能区分 wait_lock、pi_lock 和 task 引用的保证；
- [ ] 能解释 chainwalk trylock/retry；
- [ ] 能复述 deboost→交接态→wake_q→top waiter 接管；
- [ ] 能说明超时/信号 cleanup 为什么可能最终转为成功。

### 14.4 读写锁验收

- [ ] 能区分 qrwlock、rwsem、percpu-rwsem 的上下文和成本；
- [ ] 能说明 writer 如何阻止新 reader；
- [ ] 能解释批量唤醒 reader 与单独唤醒 writer；
- [ ] 能说明 PREEMPT_RT rwsem 的 PI 限制；
- [ ] 能指出 percpu reader fastpath 的禁抢占原因和 writer 成本。

### 14.5 验证能力验收

- [ ] 能读懂一条 lockdep 循环依赖报告；
- [ ] 能用 tracepoint/lockstat 区分获取次数和竞争等待；
- [ ] 能说明 locktorture 能证明和不能证明什么；
- [ ] 能在新配置下重新定位真正实现，而不是沿用旧结论；
- [ ] 能为一个未注释的相邻函数写出入口、状态、ownership、竞态和出口摘要。

---

## 15. 推荐源码阅读索引

### 第一遍必须阅读

```text
kernel/locking/Makefile                 构建与配置地图
Documentation/locking/locktypes.rst    上下文和 PREEMPT_RT 规则
include/linux/mutex.h                   普通/RT mutex API 分派
kernel/locking/mutex.c                  普通 mutex 三段路径
include/linux/rtmutex.h                 rtmutex owner 与公开 API
kernel/locking/rtmutex_common.h         waiter、排序和声明契约
kernel/locking/rtmutex.c                PI 双树、chainwalk、等待和交接
kernel/locking/rtmutex_api.c            公开 API、futex proxy、RT mutex 包装
include/linux/rwsem.h                   普通/RT rwsem 分派
kernel/locking/rwsem.c                  读写 fast/slow/wake
```

### 第二遍按问题展开

```text
qspinlock.c / qspinlock_paravirt.h      高竞争排队与 vCPU 等待
osq_lock.c                              mutex/rwsem optimistic spinner 串行化
ww_mutex.h / ww_rt_mutex.c              多锁事务退避
rwbase_rt.c / spinlock_rt.c             PREEMPT_RT 睡眠 spin/rwlock
percpu-rwsem.c                          读侧扩展性与 writer 同步
semaphore.c                             无 owner 的计数等待
lockdep.c / lockdep_proc.c              依赖图和观测接口
locktorture.c / test-ww_mutex.c         压力与协议自测
```

### 第一次可以暂时跳过

- qspinlock 的全部 PV 细节和统计字段；
- lockdep 的哈希表容量、chain key 计算与 proc 输出格式；
- ww_mutex deadlock injection 和压力参数；
- 所有架构对 fastpath 的微优化；
- 与当前问题无关的 lock events 计数项。

跳过不等于永远不学。主线模型稳定后，再用一个具体问题定向展开，比第一次就淹没在配置分支中更有效。

---

## 16. 进一步学习边界

完成 `kernel/locking/` 后，锁机制会自然延伸到其他模块：

- 调度器：task state、wake_q、有效优先级和 `rt_mutex_setprio()`；
- futex：用户态 owner、pi_state、proxy lock 和用户值修复；
- RCU boost：人工 rtmutex 只利用 PI 副作用提升阻塞 reader；
- lockdep 注解：`__acquires`、`__cond_acquires`、nest lock 和 subclass；
- 内存模型：锁的 acquire/release、等待—唤醒屏障和无锁投机读取；
- PREEMPT_RT：中断线程化、local_lock、sleeping spinlock 与 raw lock 边界。

关联源码的后续注释缺口记录在：

```text
todo/linux-kernel-source-learning-todo.md
```

当前 `kernel/locking` 文件级注释进度记录在：

```text
todo/kernel-locking-learning-comments.md
```

最终目标不是记住每个 helper，而是形成固定的阅读问题：

```text
当前上下文能否睡眠？
谁是 owner，谁只是 waiter？
fastpath 在什么状态失败？
内部哪把锁保护哪条队列或字段？
唤醒和 ownership 在哪里真正交接？
引用、RCU 和锁分别保证什么？
错误退出撤销了哪些已发布状态？
PREEMPT_RT 或调试配置会把入口换到哪里？
如果删除这个位、复查或屏障，具体竞态是什么？
我能用什么 trace、统计或压力实验验证结论？
```

当你面对一个从未读过的锁函数，也会自然先回答这些问题，而不是从第一行开始逐字翻译时，才算真正
掌握了 Linux 锁子系统的学习方法。
