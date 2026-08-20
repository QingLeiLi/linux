# Linux time 子系统内部实现

> 源码基线：当前仓库提交 `0e547323a4ac`，内核版本 `7.2.0-rc3`。
>
> 主要范围：`kernel/time/`、`include/linux/time*.h`、`include/linux/clocksource.h`、
> `include/linux/clockchips.h`、`include/linux/hrtimer*.h`、`include/vdso/`。
>
> 本文讨论的是 Linux 如何“读取时间、维护时间和安排未来事件”。PLL、CCF、CPU/总线频率和 DVFS
> 属于给硬件模块供时钟的 clock framework，可配合
> [`03 linux-clock-internals.md`](./03%20linux-clock-internals.md) 阅读，但不是本文主线。

## 1. 从两个看似简单的问题开始

用户程序经常做两件事：

```c
struct timespec now, deadline;

clock_gettime(CLOCK_MONOTONIC, &now);       /* 现在到了哪个单调时刻？ */
deadline = now;
deadline.tv_sec += 1;
clock_nanosleep(CLOCK_MONOTONIC,
                TIMER_ABSTIME, &deadline, NULL); /* 到 deadline 再继续 */
```

第一句是“读现在”，第二句是“等未来”。直觉上，内核似乎只需要一个不断增长的硬件计数器：读寄存器
就得到当前时间，把 deadline 写进比较寄存器，硬件到点发中断即可。

真实系统不能这么简单，因为它必须同时处理：

- 硬件计数器的单位是 cycle，不是纳秒；计数器位宽有限，还可能不稳定；
- 用户既要从 1970 年开始的墙钟，也要不受设时影响的单调时间；
- NTP 要修正晶振误差，但普通超时不能因此忽长忽短；
- CPU 可能进入深度 idle，本地 timer 可能停止工作；
- 每个 CPU 都可能入队、取消和执行 timer，CPU 还会热插拔；
- 低精度 I/O 超时要求极低开销，高精度音视频或睡眠要求纳秒期限；
- `clock_gettime()` 是高频操作，若每次陷入内核，系统调用成本可能高于读时钟本身；
- 容器希望看到不同的启动时间，但不能为每个容器复制一套硬件和 timekeeper；
- “定时器到期”只表示具备执行资格，不保证任务就在该纳秒获得 CPU。

Linux 因而没有一个包办一切的“时钟对象”，而是把问题拆成多层。理解每层只回答什么问题，是读懂
`kernel/time/` 的第一步。

贯穿全文的主案例是：

> 任务使用 `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)` 睡到绝对期限。我们沿着它追踪：
> 当前时间从哪里来，期限怎样进入 hrtimer，谁把最近期限写给硬件，到期中断怎样执行回调并唤醒任务，
> 以及 NTP、信号、CPU idle、NO_HZ 和 time namespace 如何影响这条路径。

## 2. 先消除五组概念混淆

### 2.1 clocksource：回答“现在的底层计数是多少”

clocksource 是一个通常自由运行、单调递增的硬件或软件计数器抽象。它的核心能力是 `read()`：

```text
输入：一个 clocksource 对象
输出：该计数器当前 cycle 值
```

clocksource 不安排未来中断。即使没有任何 timer，硬件 counter 仍持续计数。内核把两次读数相减，
再用 `mult/shift` 把 cycle 差换成纳秒。

### 2.2 clockevent：回答“怎样在未来叫醒这个 CPU”

clockevent device 是可编程中断源。它提供 `set_next_event()` 或 `set_next_ktime()`，把下一次事件写入
硬件比较器。到点后，设备进入自己的 `event_handler()`。

clockevent 不定义 REALTIME、MONOTONIC 等时间语义。它只负责“过多少 cycle 或到哪个硬件时刻发中断”。

### 2.3 timekeeping：回答“cycle 对应哪个 Linux 时间域”

timekeeping 把 clocksource 的 cycle 累积成纳秒，并维护多个相互关联的时间域：

- `CLOCK_MONOTONIC`：启动后单调前进，不包含 suspend；
- `CLOCK_BOOTTIME`：MONOTONIC 加累计 suspend 时间；
- `CLOCK_REALTIME`：Unix epoch 墙钟，可由管理员或校时程序改变；
- `CLOCK_TAI`：REALTIME 加 UTC→TAI offset，用于避开 UTC 闰秒表达；
- `CLOCK_MONOTONIC_RAW`：尽量反映硬件原始走速，不应用 NTP 频率修正。

因此 clocksource 是“尺子的刻度”，timekeeping 是“如何用这把尺子定义各种时间坐标”。

### 2.4 timer：回答“哪个工作在什么时候具备执行资格”

timer 保存 callback 和期限。Linux 有两套主要内核 timer：

- `timer_list`：以 jiffy 为基础的低精度时间轮，适合大量通常会在到期前取消的廉价超时；
- `hrtimer`：以 `ktime_t` 纳秒期限排序，适合 nanosleep、POSIX timer 和精确事件。

timer 本身不是硬件。队列里最早的期限最终要交给 tick/clockevent 层，硬件中断回来后才有机会运行
到期 callback。

### 2.5 tick：周期记账机制，不等于所有时间功能

传统周期 tick 每隔 `1/HZ` 秒触发一次，推进 jiffies、调度记账并驱动低精度 timer。高分辨率和 NO_HZ
出现后，Linux 可以把周期模式切到 oneshot，只为最近的实际事件安排下一次中断。

停止 tick 不等于停止时间。可靠 clocksource 仍持续计数；CPU 再次被唤醒时，timekeeping 可以按 cycle
差一次补上经过的时间。

### 2.6 从周期 tick 到动态 tick 的演进

理解历史演进有助于理解为什么今天仍同时存在多套机制：

```text
早期：周期 tick 推进 jiffies，timer wheel 处理所有超时
  │
  ├─ 问题：精确睡眠被 1/HZ 粒度限制，远期 timer 的级联成本不可预测
  ▼
hrtimer：以纳秒和有序 timerqueue 独立承载精确期限
  │
  ├─ 需要：硬件能够按任意下一期限触发，而不是只能固定周期
  ▼
generic clockevents + oneshot/highres：最早 hrtimer 直接驱动下一中断
  │
  ├─ 问题：无事件时周期 tick 仍浪费功耗、制造抖动
  ▼
NO_HZ + broadcast：idle/单任务 CPU 停 tick，深 idle 由共享设备唤醒
  │
  ├─ 问题：可迁移 timer 仍可能不必要地叫醒 idle CPU
  ▼
timer migration：active CPU 代理 idle CPU 的普通 global timer
```

新机制没有简单删除旧机制，因为工作负载不同：timer wheel 仍是廉价 timeout 的合适结构；周期 tick 在
某些高频短 idle 负载中反而省去反复停启成本；高分辨率和 NO_HZ 由配置与硬件能力逐层启用。

## 3. Linux 的总体方案：分层而不是一个万能定时器

最容易实现的方案是：一个全局锁、一条按绝对时间排序的全局队列、一个硬件 timer，所有 CPU 的
读时间和定时请求都访问它。这个方案容易证明正确，却有明显问题：

- 每次读时间都争用全局锁；
- 所有 CPU 的 timer 入队/取消造成同一 cacheline 抖动；
- 大量粗粒度网络超时承担精确排序成本；
- CPU idle 和热插拔时，一个全局设备难以表达本地中断能力；
- NMI、tracing 等上下文无法安全走普通加锁路径。

Linux 采用组合方案：

| 问题 | Linux 方案 | 收益 | 代价 |
|---|---|---|---|
| 高频读时间 | seqcount 快照；vDSO 用户态读取 | 常见读路径无互斥锁、无 syscall | reader 可能重试，发布协议复杂 |
| cycle→时间域 | 中央 timekeeper + 派生 offset | 多个时钟共享一份底层累计 | 写侧要原子发布一组相关字段 |
| 粗粒度超时 | per-CPU timer wheel | 入队/取消便宜，允许晚到 | 不提供纳秒排序和严格准点 |
| 精确期限 | per-CPU hrtimer timerqueue | 纳秒表达，最早期限明确 | O(log N) 排序、重编硬件成本 |
| 未来中断 | per-CPU clockevent + oneshot | 本地化、适应高分辨率 | idle 断电时需要 broadcast |
| 空闲省电 | NO_HZ 合并下一事件并停周期 tick | 减少唤醒和系统抖动 | 进入/退出多做检查与重编程 |
| 空闲 CPU 的 timer | timer migration hierarchy | 其他 CPU 可代理可迁移 timer | 增加层次状态和跨 CPU 协议 |
| 硬件源故障 | clocksource watchdog + 重选 | 自动退化到可用来源 | 后台比较和切换路径更复杂 |
| 容器时间视图 | 共享 timekeeper + namespace offset | 不复制底层时钟 | vDSO 映射和绝对期限需换坐标 |

这套设计的核心不是“绝对精确”，而是分配成本：常见读路径尽量无锁，常见超时尽量廉价，只有真正
需要精确期限时才承担排序和硬件重编成本；罕见故障与配置差异由慢路径保证完整语义。

## 4. 进入源码前的三层地图

### 4.1 子系统边界

```text
硬件 counter / timer driver
        │ register
        ├──────────────┐
        ▼              ▼
 clocksource       clockevent device
   “读 cycle”       “安排下一中断”
        │              ▲
        ▼              │ 最早期限
   timekeeping      tick / NO_HZ
        │              ▲
        ├──────┐       │
        ▼      ▼       │
 kernel API   vDSO   timer_list / hrtimer / POSIX timer
        │      │       ▲
        └──┬───┘       │
           ▼           │
       用户读时间   用户睡眠/内核超时
```

### 4.2 核心对象关系

```text
struct clocksource
  read + mask + mult/shift + rating + flags
           │ 被当前 timekeeper 借用
           ▼
struct timekeeper
  tkr_mono / tkr_raw
  xtime_sec + offs_real/boot/tai
  NTP 累积状态
           │ 发布多个只读视图
           ├─ 内核 seqcount readers
           ├─ fast latch readers
           └─ vDSO data page readers

每 CPU：
struct clock_event_device ← tick_device
           ▲
           │ 编程最近事件
           ├─ hrtimer_cpu_base 中各 clock base 的最左 timer
           ├─ timer_base 时间轮的下一到期桶
           └─ tick_sched 汇总的 RCU/irq_work/timekeeping 等期限
```

### 4.3 贯穿案例的过程阶段

```text
clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)
  1. 系统调用层校验 clockid、flags 和 timespec64
  2. time namespace 把容器绝对期限换成宿主 MONOTONIC 坐标
  3. 初始化 hrtimer_sleeper，把 timer 插入本 CPU 对应 clock base
  4. 若它成为最早事件，重编 oneshot clockevent
  5. 当前任务设置睡眠状态并调用调度器
  6. 硬件到期产生 IRQ，event_handler 进入 hrtimer_interrupt()
  7. hrtimer 摘除到期节点，sleeper callback 清除 task 指针并唤醒任务
  8. 原任务恢复；正常到期返回 0，信号路径按相对/绝对语义处理 restart
```

后文每个局部机制都应能放回这三层地图，而不是成为孤立的函数列表。

## 5. 时间的表示：cycle、jiffy、ktime_t 与 timespec64

### 5.1 cycle 是硬件刻度，不是纳秒

假设一个 counter 频率为 24 MHz，每秒增加 24,000,000。一个 cycle 约等于 41.67 ns，但内核不能在
读时间热路径执行昂贵除法，于是预计算 `mult` 和 `shift`：

```text
nanoseconds = (cycle_delta * mult) >> shift
```

`clocksource_cyc2ns()` 当前实现正是这条定点公式：

```c
static inline s64 clocksource_cyc2ns(u64 cycles, u32 mult, u32 shift)
{
        return ((u64)cycles * mult) >> shift;
}
```

必须传相对 delta，而不是不断增长的绝对 cycle。绝对值更容易让乘积溢出，也无法表达该读数相对哪个
timekeeper base。

### 5.2 mask 让有限位宽计数器在一圈内可做模减法

若 counter 只有 32 位，读数会从 `0xffffffff` 回到 `0`。timekeeping 使用：

```text
delta = (now - cycle_last) & mask
```

只要两次有效读取间没有跨过超过一个完整周期，模减法就能恢复正确 delta。若系统睡得比 counter 一圈
还久，单凭起点和终点无法知道中间绕了几圈，因此 clocksource 注册时还要计算最大安全 idle 时间。

### 5.3 jiffy 是周期 tick 的逻辑单位

`jiffies` 大致每个 tick 增加一次，粒度由 `CONFIG_HZ` 决定。它适合表达“至少等几十毫秒”的超时，
不适合表示精确墙钟。

因为 `unsigned long` 会回绕，比较 deadline 必须使用 `time_after()`、`time_before()` 等宏。它们把差值
按有符号数解释，只在比较跨度小于半个取值空间的约束下成立。直接写 `expires > jiffies` 会在回绕边界
把未来误判为过去。

### 5.4 ktime_t 与 timespec64 服务不同边界

- `ktime_t` 当前是 64 位纳秒标量，便于内核排序、加减和比较；
- `timespec64` 将秒和纳秒拆开，便于 ABI、日历和避免 32 位秒的 Y2038 溢出；
- `time64_t` 是 64 位秒值；
- 用户传入的 `__kernel_timespec` 必须先校验 `tv_nsec` 范围，再转换成内部格式。

不能只看 C 类型判断语义。同一个 `ktime_t` 可能是 MONOTONIC 绝对时刻、相对时长或 BOOTTIME 期限；
调用契约必须同时说明时间域和单位。

## 6. 五种主要时间域为什么同时存在

### 6.1 MONOTONIC：内核超时的默认坐标

MONOTONIC 从启动开始单调前进。管理员设置墙钟时，它不能跟着跳，否则一个“十秒后超时”的请求可能
立刻到期或额外等待数小时。它不计入 suspend，所以“CPU 可以运行的系统在线时间”是较好的直觉。

### 6.2 BOOTTIME：在 MONOTONIC 上补入 suspend

移动设备可能休眠一小时。若任务表达“从用户视角一小时后过期”，MONOTONIC 会忽略这一小时，而
BOOTTIME 用 `offs_boot` 补入累计 suspend 时间。alarmtimer 又可以借助可唤醒硬件，让系统在 suspend
中因 BOOTTIME/REALTIME alarm 醒来。

### 6.3 REALTIME：可校正的历元墙钟

REALTIME 表示 Unix epoch 以来的 UTC 时间。启动时它需要持久时钟/RTC 或用户空间校时建立合理基准；
管理员可以 step，NTP 也可逐渐校正其走速。因此日志和文件时间戳适合它，测量间隔通常不适合。

### 6.4 TAI：把 UTC 闰秒差作为 offset

TAI 与 REALTIME 共享底层推进，但加上 `tai_offset`。它适合需要连续原子时间坐标的场景。它不是另一块
硬件，也不是完全独立推进的 timekeeper。

### 6.5 MONOTONIC_RAW：观察未经 NTP 调频的硬件走速

timekeeper 为 MONOTONIC 与 RAW 保存不同 read base。两者读取同一当前 clocksource，但 NTP 可以调整
MONOTONIC 路径使用的 `mult`，RAW 仍按硬件原始换算前进。RAW 适合观察本地振荡器或特殊测量，不应
被理解为“比 MONOTONIC 总是更准确”。

### 6.6 它们不是五套独立时钟

当前 `struct timekeeper` 的核心关系可以概括为：

```text
tkr_mono + xtime_sec       受 NTP 调整的主要累计
tkr_raw  + raw_sec         原始累计

MONOTONIC = mono base + 当前 cycle delta
REALTIME  = MONOTONIC + offs_real
BOOTTIME  = MONOTONIC + offs_boot
TAI       = MONOTONIC + offs_tai
```

实际代码还保存秒/纳秒拆分、coarse 快照、闰秒和误差累积等字段。关键不变量是：写侧必须把这些互相关联
的字段作为一代快照发布，读者不能混用更新前后的值。

## 7. clocksource：选择一把可靠的时间尺

### 7.1 一个 clocksource 需要描述什么

除了 `read()`，核心字段还包括：

- `mask`：有效计数位宽；
- `mult/shift`：cycle→ns 换算；
- `rating`：自动选择偏好，不是绝对正确性证明；
- `max_cycles/max_idle_ns`：在回绕和乘法溢出前允许的安全 delta；
- flags：是否连续、是否必须 watchdog 验证、是否在 suspend 中继续等；
- owner/list/watchdog 字段：注册生命周期与交叉验证状态。

驱动通常调用 `clocksource_register_hz()` 或 `clocksource_register_khz()`，最终进入
`__clocksource_register_scale()`。该函数规范化字段并把对象发布到选择、watchdog 和 suspend 视图。

### 7.2 rating 最高并不意味着永不出错

注册完成后，`clocksource_select()` 综合 rating、用户 override、稳定性和当前 tick 约束选择候选，
再通过 `timekeeping_notify()` 请求 timekeeper 换源。

有些来源带 `CLOCK_SOURCE_MUST_VERIFY`。watchdog 周期读取当前源和参考源，比较同一时间窗口的增量。
偏差超过阈值时，它把可疑来源标为 unstable，并触发重新选择。

```text
主 clocksource delta      参考 clocksource delta
          │                         │
          └──── 换算到同一时间量纲 ─┘
                         │
                   偏差是否越界
                  /             \
               否                 是
          更新 watchdog 基线     标 unstable → 重新选源
```

### 7.3 为什么标记故障与真正换源要分阶段

watchdog callback 运行在 timer/中断相关上下文，而换源会经过 mutex、timekeeper 写事务和通知路径。
若在 watchdog 的内部锁下直接完成所有工作，会扩大不可睡眠临界区并造成锁顺序风险。因此故障检测先
记录状态，适合睡眠的后续上下文再完成重选。

### 7.4 换源不能让 MONOTONIC 断裂

换源时，timekeeping 先用旧源推进到当前时刻，再把同一个逻辑时间基点绑定到新源的当前 cycle、mask、
mult 和 shift。这样改变的是后续如何测量 delta，不是已经累计的 MONOTONIC 值。

失败边界也很重要：新 clocksource 若不能 enable，timekeeper 不能先把读者指向它；必须保留旧源或选择
其他可用来源。换源是“准备新参数 → 在写侧提交 → 再停旧源”的有序事务，不是只替换一个指针。

## 8. clockevent：把最近期限交给硬件

### 8.1 clockevent 的方向与 clocksource 相反

clocksource 将硬件 cycle 转成纳秒；clockevent 常把纳秒 delta 转成设备 tick：

```text
device_ticks = (nanoseconds * mult) >> shift
```

`struct clock_event_device` 还保存：

- periodic、oneshot、shutdown 等状态切换回调；
- `min_delta_ns/max_delta_ns`，限制硬件可编程范围；
- `next_event`，记录已承诺的下一期限；
- CPU mask、rating、IRQ 和模块所有者；
- `event_handler`，硬件 IRQ 到来后的软件落点。

### 8.2 注册后还要由 tick 层选择

驱动调用 `clockevents_register_device()` 后，设备进入全局管理列表。`tick_check_new_device()` 判断它能否
服务当前 CPU、是否优于现有设备，随后 `tick_setup_device()` 才把它安装进 per-CPU `tick_device`。

因此“设备已注册”和“设备正在驱动这个 CPU 的 tick”是两个不同状态。替换设备时还涉及模块引用、
旧设备释放和当前 periodic/oneshot 模式继承。

### 8.3 min delta 是防止立即重触发的工程边界

若软件计算完期限时该期限已经过去，把 0 或过小 delta 写给硬件可能立刻再次进中断，形成活锁。
clockevent 层会钳制最小 delta，并在必要时重试。代价是事件会晚一点，但系统仍能向前执行。

## 9. timekeeping 读路径：一次 ktime_get() 做了什么

### 9.1 read base 是“已提交基点 + 尚未提交增量”

`struct tk_read_base` 保存：

```text
clock       当前 clocksource
mask        cycle 回绕掩码
cycle_last  上一代 timekeeper 提交时的 cycle
mult/shift  当前时间域使用的换算比例
xtime_nsec  移位后的子秒余量
base        已提交的 MONOTONIC 纳秒基点
```

读取当前时间时不需要先修改全局 timekeeper：

```text
读取一代稳定的 tk_read_base
→ clock->read() 得到当前 cycle
→ delta = (cycle - cycle_last) & mask
→ ns = base + ((delta * mult + fractional) >> shift)
→ 再按目标 clock 加对应 offset
```

`timekeeping_cycles_to_ns()` 当前代码中的核心正是 mask 求 delta，并在异常大 delta 时进入安全慢算。
正常路径只做一次硬件读和整数乘移。

当前 `ktime_get()` 清楚展示了“稳定快照 → 计算增量 → 验证代次”的完整读路径：

```c
do {
        seq = read_seqcount_begin(&tk_core.seq);
        base = tk->tkr_mono.base;
        nsecs = timekeeping_get_ns(&tk->tkr_mono);
} while (read_seqcount_retry(&tk_core.seq, seq));

return ktime_add_ns(base, nsecs);
```

`base` 是上一代已提交的 MONOTONIC，`nsecs` 是从 `cycle_last` 到硬件当前读数的增量。只有二者来自
同一 seq 代，结果才可接受。

### 9.2 seqcount 为什么适合读多写少

timekeeper 写侧不只是改一个值，而会同时更新 base、cycle_last、mult、offset 和 coarse/vDSO 副本。
普通 reader 使用如下协议：

```text
读 seq，若为奇数则等待/重试
→ 复制所需字段并读 clocksource
→ 再检查 seq
→ seq 未变化：这一组值属于同一代
→ seq 变化：丢弃结果并重读
```

reader 不修改共享 cacheline，也不与其他 reader 争 mutex，适合 `ktime_get()` 这种极高频操作。代价是
写侧期间 reader 可能重试，所以写侧临界区必须短，且不能在 seqcount 写段中睡眠。

seqcount 只保证字段组合的一致性。若快照里包含可释放指针，对象生命周期仍需引用、RCU 或外层协议。
timekeeper 当前 clocksource 的切换与注销因此不能只依赖 seqcount。

### 9.3 精确、coarse 与 fast accessor 的选择

- `ktime_get()`：读取当前 clocksource，得到较新 MONOTONIC；
- `ktime_get_coarse*()`：使用上次更新留下的 coarse 快照，少一次硬件读但可能落后一个 tick；
- `ktime_get_*_fast_ns()`：使用 latch/双副本，允许 NMI、tracing 和 suspend 边界读取；
- `ktime_get_raw*()`：使用 RAW read base，不应用 NTP 频率修正。

接口名更长通常意味着更特殊的语义，不意味着“总是更好”。普通驱动测量持续时间优先使用最简单、
语义足够的 MONOTONIC accessor；只有明确需要 NMI 安全或 coarse 性能时才选特殊版本。

### 9.4 clock_gettime() 还要做 clockid 分派

`kernel/time/posix-timers.c` 的 `posix_clocks[]` 把固定 clockid 映射到 `struct k_clock` 操作表：

```text
CLOCK_REALTIME       → realtime getter，可 set/adj，可建 timer
CLOCK_MONOTONIC      → monotonic getter，可 sleep/建 timer
CLOCK_MONOTONIC_RAW  → 只读
CLOCK_*_COARSE       → 只读 coarse getter
CLOCK_BOOTTIME       → boottime getter，可 sleep/建 timer
CLOCK_PROCESS/THREAD → POSIX CPU clock 实现
```

系统调用先验证 clockid 和用户指针，再调用对应 getter。动态 clockid 还可能分派到 fd-backed POSIX clock，
负 clockid 也可编码进程/线程 CPU clock；不能假设所有 `clock_gettime()` 都读取 core timekeeper。

## 10. timekeeping 写路径：如何推进并原子发布时间

### 10.1 tick 到来不是简单的 wall_time++

`update_wall_time()` 调用 `timekeeping_advance(TK_ADV_TICK)`。内部大致经历：

```text
取得 core raw spin lock，IRQ save
→ 从当前 clocksource 读取 cycle delta
→ 按可处理的 NTP interval 分段累积
→ 推进 RAW 和受 NTP 调整的 MONOTONIC/REALTIME 状态
→ 处理秒边界、闰秒和 NTP 误差
→ 刷新 coarse、fast 与 vDSO 数据
→ 通过 seqcount 结束一代发布
→ 解锁并恢复 IRQ
→ 若发生可观察 clock-set，在锁外排队通知
```

当前外层使用 scope guard 保证所有出口恢复 IRQ 并释放锁。真正的累积发生在 shadow 上，再由
`timekeeping_update_from_shadow()` 提交给 reader 视图，避免 reader 看到只更新了一半的状态。

### 10.2 为什么要保留小数余量

cycle→ns 的定点换算通常不能整除。若每个 tick 都丢弃小数，误差会长期累积。timekeeper 用移位后的
`xtime_nsec`、remainder 和 NTP error 保存未满一个纳秒或一个 interval 的余量，在后续推进中继续累计。

这也是为什么 timekeeping 不能退化成单个 `u64 now_ns`：它还要保存换算误差、频率校正和下一秒边界。

### 10.3 NTP 主要怎样改变走速

晶振可能每秒快或慢若干 ppm。若每次都直接跳墙钟，日志可能倒退，绝对 deadline 也会突然跨过。NTP
纪律层通常调整 timekeeper 使用的换算比例，使同样的 cycle delta 累积出略多或略少的纳秒，逐渐消除误差。

```text
硬件 RAW：      1,000,000 cycles → 固定 raw ns
受校正 MONO：   1,000,000 cycles → raw ns ± 很小频率修正
REALTIME：      MONOTONIC + 当前墙钟 offset
```

`ntp_tick_length()` 提供当前 interval 长度，timekeeper 把它缓存到 `ntp_tick`，确保一个正在处理的 tick
不会中途混用两代 NTP 参数。`second_overflow()` 在秒边界处理状态、闰秒和误差更新。

### 10.4 settimeofday() 如何改墙钟而不破坏 MONOTONIC

`do_settimeofday64()` 不是把所有时间域整体替换。它在写事务中推进到当前时刻，验证新 REALTIME，
然后调整 REALTIME 与 MONOTONIC 的关系，使：

```text
提交前 MONOTONIC == 提交后 MONOTONIC
提交后 REALTIME  == 用户指定的新墙钟
```

也就是说，管理员把墙钟向前调一小时，正在等待 MONOTONIC deadline 的任务不会立刻超时。基于绝对
REALTIME 的 timer 则必须遵守墙钟语义：新墙钟若跨过期限，它可能很快被判定到期。

写侧成功后还要通知 timerfd、hrtimer 或其他关心 clock-set 的消费者。通知放在核心 raw lock 外，避免
在不可睡眠临界区执行复杂回调。

### 10.5 suspend/resume 为什么需要三种时间答案

suspend 时，内核先推进并冻结当前 timekeeper，记录 clocksource/persistent clock 基线，然后停止相关
clockevents。resume 时按平台能力选择证据：

1. 当前 clocksource 带 `CLOCK_SOURCE_SUSPEND_NONSTOP`，可以直接从 cycle 差得到睡眠时长；
2. 使用 persistent clock/RTC 的前后读数；
3. RTC 等较晚恢复时，通过 `timekeeping_inject_sleeptime64()` 补注可确认的时长。

恢复提交必须满足：

```text
MONOTONIC：不计 suspend，因此保持冻结前逻辑连续
BOOTTIME：增加确认的 suspend 时长
REALTIME：增加真实经过的墙上时间
RAW：按当前实现和 clocksource 能力维护对应累计
```

如果平台没有可靠证据，内核不能凭空制造精确睡眠时长；实验结果必须结合 clocksource flags、RTC 与
平台 suspend 实现解释。

## 11. 贯穿案例：绝对 clock_nanosleep 如何等待未来

现在把前面的“读现在”和后面的“安排中断”连起来。

### 11.1 第一步：系统调用选择具体 clock 实现

`SYSCALL_DEFINE4(clock_nanosleep, ...)` 先通过 `clockid_to_kclock()` 找到 `struct k_clock`，复制并校验
用户 `timespec64`，再调用该 clock 的 `nsleep` 操作。

当前固定操作表中：

- REALTIME/TAI 使用 `common_nsleep()`；
- MONOTONIC/BOOTTIME 使用 `common_nsleep_timens()`；
- RAW/COARSE 没有 `nsleep` 回调，因此不能拿来做 POSIX clock_nanosleep；
- ALARM clock 进入 alarmtimer 实现；
- 动态 clock 和 CPU clock 由其他操作表处理。

为什么 MONOTONIC/BOOTTIME 单独使用 `common_nsleep_timens()`？时间 namespace 允许容器为这两个时间域
设置 offset。用户给出的绝对期限属于容器坐标，入宿主 hrtimer 队列前必须用
`timens_ktime_to_host()` 换成宿主坐标。相对“睡 10 ms”与坐标原点无关，不需要换算。

```text
容器看到：MONOTONIC = 120 s
namespace offset：+100 s
用户绝对期限：125 s

宿主当前 MONOTONIC = 20 s
宿主入队期限 = 125 s - 100 s = 25 s
仍然等待 5 s
```

### 11.2 第二步：hrtimer_sleeper 把 timer 与任务连接起来

`hrtimer_nanosleep()` 在当前任务栈上创建 `struct hrtimer_sleeper`。它包含一个 hrtimer 和一个 task 指针：

```text
hrtimer：进入 per-CPU timerqueue，描述期限和 callback
task：   指向正在睡眠的当前任务
```

callback 到期时清除 sleeper 的 task 指针并唤醒任务。原任务恢复后检查该指针，就能区分“timer 正常到期”
与“因为信号提前醒来”。对象位于任务内核栈上，所以函数返回前必须确保 hrtimer 已取消或已经不再被
任何 CPU 执行；否则 callback 会访问已经失效的栈内存。

### 11.3 第三步：选择 clock base 并把期限排入队列

每个 CPU 有 `hrtimer_cpu_base`，其中包含多个 `hrtimer_clock_base`，分别服务 MONOTONIC、REALTIME、
BOOTTIME、TAI 以及 hard/soft callback 变体。timer 启动时会确定：

- 它属于哪个时间域；
- 绝对期限还是相对时长；
- hard callback 还是 softirq callback；
- 是否 pinned 在当前 CPU；
- soft expiry 与 hard expiry 的范围。

`enqueue_hrtimer()` 在 `cpu_base->lock` 下把节点按 hard expiry 插入 timerqueue。当前实现的关键发布顺序是：

```c
WRITE_ONCE(timer->is_queued, HRTIMER_STATE_ENQUEUED);
timerqueue_linked_add(&base->active, &timer->node);
```

`WRITE_ONCE` 让无锁状态查询不被编译器撕裂或合并，但红黑树和 linked-list 的完整结构仍由 raw spin lock
保护。把 `WRITE_ONCE` 误解成“队列已经线程安全”会遗漏真正的结构锁。

timerqueue 同时维护按期限排序的红黑树和可快速取得最左节点的链接。插入是 O(log N)，最早期限可直接
取得。若新 timer 成为最左节点，当前 CPU 已编程的 clockevent 可能太晚，必须重新编程。

### 11.4 soft expiry、hard expiry 与 slack

定时请求往往只要求“不早于某时刻”，允许在一个小范围内稍晚执行。hrtimer 保存：

```text
soft expiry：从这里开始可以执行
hard expiry：最迟应以此作为排序/唤醒边界
```

slack 让内核把相近唤醒合并，减少硬件重编和 idle 唤醒。它换取功耗与吞吐收益，代价是实际唤醒可在
允许窗口内变晚。绝对实时要求强的调用者应明确 slack，而不能看到 `ktime_t` 纳秒单位就假设零抖动。

### 11.5 第四步：任务发布睡眠状态并调度出去

`do_nanosleep()` 形成一个“启动 timer → 设置任务状态 → schedule → 检查原因”的循环。这里有经典的
等待—唤醒竞态：

```text
睡眠 CPU                              到期 CPU/中断
设置可睡眠 task state
启动或确认 timer 已发布              timer 到期
调用 schedule()                      清 sleeper->task 并 wake_up_process()
```

任务状态发布和 wakeup 协议必须保证至少一方观察到另一方。如果先检查条件、后发布睡眠状态，waker 可能
在窗口内认为任务仍在运行而不唤醒，随后任务睡下，形成 lost wakeup。hrtimer sleeper 与调度器 API 的
既定顺序关闭了这个窗口。

### 11.6 第五步：最早期限进入 clockevent

高分辨率模式下，tick 层把本 CPU clockevent 切到 oneshot，`event_handler` 指向 `hrtimer_interrupt()`。
当 hrtimer 队列的最早期限变化时，hrtimer 重新计算 `expires_next` 并调用 clockevent 编程接口。

```text
hrtimer 最早绝对期限
→ 减去当前 MONOTONIC，得到 delta
→ 钳制到 clockevent min/max delta
→ ns 转 device ticks
→ driver 写硬件比较寄存器
```

期限若比设备最大范围更远，可以先编一个较近的中间事件，之后重算；期限已经太近则使用最小 delta。
因此软件的 64 位纳秒期限不受单次硬件可编程范围限制，但很远的事件可能经过多次唤醒才能最终到达。

### 11.7 第六步：hrtimer_interrupt 执行到期队列

硬件中断到来时，`hrtimer_interrupt()` 在 IRQ disabled 上下文执行：

1. 取得当前 CPU 的 hrtimer base lock；
2. 刷新各 clock base 的“现在”；
3. 对 soft timer 只 raise `HRTIMER_SOFTIRQ`；
4. 对 hard timer 扫描已到 soft expiry 的节点并执行 callback；
5. callback 期间按协议临时放锁，返回后处理 RESTART 或 inactive；
6. 重新寻找所有 base 的最早期限；
7. 重编 clockevent。

处理本身需要时间。tracing、长 callback 或虚拟机调度可能导致刚算出的下一个期限已经再次过去。当前
代码最多重试三次；仍然追不上时记录 hang，并使用保护性延迟打破硬中断活锁。这体现了一个重要取舍：
在无法兑现所有过期事件的异常场景中，宁可让事件变晚，也不能让 CPU 永远困在 timer IRQ。

### 11.8 第七步：唤醒、取消与系统调用返回

正常到期时，sleeper callback 唤醒原任务。任务恢复后清理栈上 timer，并返回 0。

信号提前唤醒时：

- 相对睡眠需要计算剩余时长，供 restart 或用户处理；
- 绝对睡眠按当前 POSIX 路径不自动 restart，也不返回剩余时长；用户若重试，应复用同一个绝对
  deadline，不能把信号处理耗时重新加到目标上；
- 返回用户前必须同步取消可能仍在队列或另一个 CPU 正在执行的 timer。

这就是绝对周期循环比反复相对 sleep 更不易漂移的原因：

```text
相对循环：下次期限 = 本次实际醒来时间 + period
          每次调度/信号延迟都会累积

绝对循环：第 N 次期限 = 初始基点 + N * period
          单次晚醒不会移动后续目标坐标
```

## 12. timer_list：为什么精确 hrtimer 没有替代低精度时间轮

### 12.1 大多数 timeout 最终不会到期

网络重传、设备命令和缓存回收常设置保护性 timeout。正常事件到来时，timer 会在到期前被删除。若每个
此类 timer 都进入精确红黑树并频繁重编硬件，系统会为“通常不会发生”的异常支付排序成本。

`timer_list` 的目标是低开销，并承诺不早于 `expires` 执行；它允许按级别粒度晚到。当前时间轮不是用来
生成精确媒体时间线的。

### 12.2 当前时间轮怎样组织期限

每个 CPU 有 timer base，base 中按不同粒度划分多级 buckets，并用 pending bitmap 快速定位非空桶。
较近期限进入细粒度层，越远期限进入越粗层。当前实现避免传统时间轮逐级 cascade：远期 timer 直接
放入与其距离匹配的层，最终按该层粒度到期。

收益：

- 插入和删除主要是链表/位图操作；
- 大量超时不会反复级联搬迁；
- per-CPU base 降低全局锁争用。

代价：

- 越远的 timer 粒度越粗，可能更晚执行；
- 修改 CPU 归属时需要迁移协议；
- callback 与删除并发需要额外状态保证。

### 12.3 mod_timer() 是启动和重排的核心事务

`mod_timer(timer, expires)` 既可启动 inactive timer，也可原子重排 active timer。`__mod_timer()` 大致执行：

```text
检查是否可走同桶快速路径
→ 锁定旧 base，确认 timer 当前归属
→ 若正在迁移则按状态重试
→ 从旧桶摘除
→ 根据 pinned/当前 CPU/idle 状态选择新 base
→ 计算 level 与 bucket
→ 插入并更新 bitmap/next-expiry 缓存
→ 必要时唤醒或通知目标 CPU
```

同桶快速路径避免“摘除再插入”，但只能在新旧期限映射到同一 bucket 且不破坏已有状态时使用。

### 12.4 删除、同步删除和 shutdown 不是同一保证

- 普通 delete：阻止仍在队列中的这一次到期；若 callback 已开始，可能与调用者并发；
- sync delete：等待正在执行的 callback 完成，使调用者可以安全跨过 callback 生命周期边界；
- shutdown：除同步删除外，还阻止 callback 或并发路径再次把 timer 重新入队，适合对象销毁。

典型 use-after-free 来自：对象拥有一个嵌入 timer，销毁路径只做普通 delete；与此同时 callback 已取到
container 指针或再次 rearm。对象内存随后释放，callback 继续访问。正确 API 取决于调用者是否需要：

```text
只取消未来队列节点
等待当前 callback
永久关闭后续 rearm
```

### 12.5 callback 为什么临时释放 base lock

`__run_timers()` 在锁内识别并摘除到期节点，执行 callback 时临时释放 base lock。否则 callback 若启动、
删除其他 timer，或执行较长工作，会自锁或长时间阻塞同 CPU 的 timer 操作。

放锁也意味着 callback 期间队列可能被其他上下文修改。运行中状态、`running_timer`、同步删除协议以及
PREEMPT_RT 的 expiry lock 共同表达“节点已不在桶中，但 callback 生命周期尚未结束”。

### 12.6 到期链路

```text
周期/oneshot tick 到来
→ run_local_timers()
→ raise TIMER_SOFTIRQ
→ run_timer_softirq()
→ __run_timers(base)
→ 收集到期桶
→ 逐个摘除，临时放锁执行 callback
```

timer_list callback 默认在 softirq 上下文，不能执行会睡眠的操作。需要进程上下文的工作通常由 callback
排入 workqueue，而不是直接阻塞。

## 13. POSIX timer、CPU timer、itimer 与 alarmtimer

### 13.1 POSIX timer 是 hrtimer 之上的用户对象与通知协议

`timer_create()` 不只是创建一个 hrtimer。`do_timer_create()` 还需要：

- 验证 clockid 和该 clock 是否支持 timer；
- 分配 `struct k_itimer`；
- 建立进程内 timer ID 索引；
- 复制和校验 `sigevent`；
- 初始化 clock-specific timer；
- 失败时按相反顺序撤销 ID、引用和对象。

`timer_settime()` 通过 `it_lock` 串行对象状态，取消旧期限、保存 interval 并调用具体 clock 的 arm 操作。
周期 timer 到期后不是简单执行一次：它要 forward 到未来第一个周期，并累计 overrun。

标准信号不是无界队列。若多个周期在用户处理上一次信号前经过，内核可能合并通知并用 overrun 表达
错过次数。因此“设置 1 kHz POSIX timer”不等于进程一定每毫秒收到一个独立信号并运行一次。

### 13.2 CPU timer 的时间轴来自运行记账

`CLOCK_PROCESS_CPUTIME_ID` 和 `CLOCK_THREAD_CPUTIME_ID` 不读取 wall clock。它们根据任务或线程组实际
消耗的 user/system runtime 到期：任务睡眠或等待 I/O 时，CPU clock 不前进。

scheduler tick 或任务记账更新后，`run_posix_cpu_timers()` 检查最近的 CPU timer。不同配置可把实际 expiry
安排为 task_work，在任务返回用户前处理，以控制 IRQ-off 路径成本。

这也解释了 NO_HZ_FULL 的限制：需要精细 CPU timer 的任务会要求持续记账机会，可能阻止该 CPU 长时间
停掉调度 tick。

### 13.3 三种传统 itimer

| 接口 | 时间依据 | 到期通知 |
|---|---|---|
| `ITIMER_REAL` | 墙上经过的 REALTIME | `SIGALRM` |
| `ITIMER_VIRTUAL` | 进程用户态 CPU 时间 | `SIGVTALRM` |
| `ITIMER_PROF` | 进程用户态+内核态 CPU 时间 | `SIGPROF` |

它们共享“interval timer”名字，却分属 wall hrtimer 与 CPU accounting 两类机制。阅读 `itimer.c` 时应先判断
时间依据，再追踪具体后端。

### 13.4 alarmtimer 为什么能够跨 suspend

普通 MONOTONIC hrtimer 不承诺在系统 suspend 中唤醒机器。alarmtimer 把 BOOTTIME/REALTIME alarm 语义
连接到可唤醒 RTC 或平台 alarm 设备：suspend 前找出最近 alarm 并编程硬件，设备到期触发 wakeup，恢复后
再由软件队列完成普通到期处理。

这里有两个“到期”：硬件 alarm 负责让系统退出 suspend，Linux hrtimer/POSIX 层负责把正确用户对象判为
到期。RTC 精度、固件唤醒延迟和恢复流程都会让最终 callback 晚于理论期限。

## 14. tick 与高分辨率模式怎样连接 timer 和硬件

### 14.1 周期模式

周期 clockevent 每 `1/HZ` 产生一次中断，handler 完成：

```text
更新 jiffies/timekeeping
→ update_process_times()
→ scheduler_tick()
→ run_local_timers() raise softirq
```

优点是简单：不必每次重编硬件。代价是没有实际工作时仍唤醒 CPU，高精度事件也只能等下一个 tick。

### 14.2 oneshot 与 high-resolution 模式

当 clocksource 满足高分辨率要求、clockevent 支持 oneshot，`tick_switch_to_oneshot()` 切换 per-CPU tick
设备。高分辨率模式下 event handler 进入 `hrtimer_interrupt()`，下一次硬件中断由最早 hrtimer 决定。

低精度 timer wheel 仍然存在。它的下一期限会作为一个事件参与合并；高精度模式不是把所有 timer_list
转成 hrtimer，而是让硬件中断可以按真实最早事件动态安排。

### 14.3 高精度有三层不同含义

```text
表示精度：ktime_t 能表达纳秒
编程精度：clockevent 能否安排足够细的下一事件
执行延迟：IRQ、callback、调度和 CPU 唤醒何时真正运行任务
```

前两层达到纳秒单位，也不能消除第三层的微秒甚至更大抖动。实时系统要同时控制 IRQ-off 临界区、
callback 长度、调度优先级、CPU isolation 和 C-state。

## 15. NO_HZ：为什么以及怎样停止周期 tick

### 15.1 三种模式

- `CONFIG_HZ_PERIODIC`：忙或闲都保留周期 tick；
- `CONFIG_NO_HZ_IDLE`：CPU idle 时停止周期 tick；
- `CONFIG_NO_HZ_FULL`：指定 CPU 在只有一个 runnable task 时也尽量停止 tick。

NO_HZ_IDLE 主要省电，NO_HZ_FULL 主要减少实时/HPC 的周期抖动。它们都不是免费优化：进入和退出要
检查更多状态、计算下一事件并重编硬件。

### 15.2 停 tick 前必须合并所有需要 CPU 的期限

`tick_nohz_next_event()` 不是只看 timer wheel。它先建立一致的 jiffies/MONOTONIC 基点，再检查：

- RCU 是否需要该 CPU；
- architecture hook 是否需要 tick；
- irq_work 是否待处理；
- timer softirq 是否已经 pending；
- timer wheel（以及低分辨率模式下的 hrtimer）下一期限；
- timekeeping 最长可延迟时间；
- timer migration 返回的远端/全局期限。

若 timer softirq 已 pending，再查询下一 timer 会不断得到“立即到期”，硬件被反复编最小 delta，形成
中断循环。因此这种情况直接保留下一个 tick，让已有 softirq 先获得执行机会。

```text
next_event = min(
    RCU/arch/irq_work 的近期要求,
    本地 timer 最早期限,
    hrtimer 最早期限（取决于模式）,
    timekeeping 最大 deferment,
    timer migration 代理期限
)
```

遗漏任何一个消费者都可能让 CPU 睡过头；过度保守则只是少省一点电。NO_HZ 的正确性设计偏向“不漏
事件”，性能优化再尽量延长睡眠。

### 15.3 tick_nohz_idle_stop_tick() 是提交点

idle governor 可能提前查询 sleep length 并缓存同一基点的期限。真正进入 idle 前，
`tick_nohz_idle_stop_tick()` 复用或重新计算期限，检查当前 CPU 是否允许停 tick，然后调用 stop 路径重编
clockevent，并记录 STOPPED/idle 状态。

若计算结果表明一个 tick 内就有工作，函数保留周期 tick。停 tick 是一次有条件状态转换，不是把某个
布尔变量设为 true。

### 15.4 为什么必须保留 timekeeping/housekeeping CPU

NO_HZ_FULL 不能让所有 CPU 永久停止调度 tick。系统仍需要 CPU 推进全局工作、处理未卸载的 RCU callback、
管理 timer 和执行 housekeeping。启动参数通常保留至少一个非 adaptive-tick CPU，并配合 RCU nocb、
IRQ affinity 和任务隔离；只写 `nohz_full=` 不能自动得到无抖动系统。

## 16. 深度 idle：tick broadcast 怎样替代停止的本地设备

某些 CPU 深度 idle 状态会让本地 clockevent 停止。若仍把 deadline 只写在本地设备里，CPU 可能永远无法
被该事件唤醒。

broadcast 层使用一个在深 idle 中仍工作的共享设备：

```text
CPU 准备进入深 idle
→ 将自己加入 broadcast oneshot mask
→ 把本地 next_event 纳入共享设备最早期限
→ 关闭/停用本地 clockevent

共享设备到期
→ tick_handle_oneshot_broadcast()
→ 扫描各目标 CPU 的 next_event
→ 到期 CPU 进入 pending/临时 mask
→ 给远端 CPU 发 IPI；当前 CPU 可直接执行本地 handler
→ 用剩余 CPU 的最早期限重编共享设备
```

`tick_broadcast_lock` 保护 mask、共享设备期限和每次扫描的分类。当前 CPU 不需要给自己发 IPI，因此会在
锁外直接调用本地 handler；这样还可避免在 broadcast lock 下递归进入 hrtimer 或产生锁反序。

broadcast 的收益是允许更深 idle，代价是共享设备争用、mask 扫描、IPI 和额外唤醒延迟。平台本地 timer
若有 `C3STOP` 一类特征，是否需要 broadcast 由驱动能力和 idle 状态共同决定，不能只看 NO_HZ 配置。

## 17. timer migration：谁替 idle CPU 管理可迁移超时

### 17.1 broadcast 与 timer migration 解决不同问题

broadcast 解决“本地 clockevent 停了，谁送中断”。timer migration 解决“CPU 已 idle，为何还让它因可迁移
的普通 timer 醒来”。后者尝试由其他 active CPU 代理 idle CPU 的 global timer，减少无谓唤醒。

pinned timer 具有 CPU 语义，不能任意代理；只有 migratable/global base 中的期限参与层次管理。

### 17.2 层次结构与唯一 migrator

timer migration 按 CPU/拓扑建立 group hierarchy。每个 group 汇总子节点的最早事件，并选出唯一 migrator
负责向上或向下处理到期事件。

```text
CPU local global timer
        │
        ▼
leaf group event ── 汇总最早期限
        │
        ▼
parent/root group event
        │
        ▼
某个 active CPU 作为 migrator 执行远端到期 timer
```

CPU 准备 idle 时，`tmigr_cpu_deactivate()` 发布 inactive 状态和下一 global deadline；active 时
`tmigr_cpu_activate()` 重新取得本地责任；idle 状态下出现更早 timer 时，`tmigr_cpu_new_timer()` 更新层次并
在必要时唤醒负责 CPU；`tmigr_handle_remote()` 在 timer softirq 中执行已经到期的远端代理工作。

### 17.3 为什么状态里需要 sequence

考虑两个并发事件：CPU 正在从 active 变 idle，同时另一个上下文给它加入更早 timer。若只用一个 active
布尔位，入队者可能看到旧 active 而不向层次发布，idle 路径又已经读过旧 deadline，最终睡过头。

迁移状态把 active、migrator 和 sequence 等信息放入原子状态字，通过比较交换让双方检测状态代次变化。
sequence 不是统计计数，而是防止 ABA：状态表面又回到同一位图时，参与者仍能知道中间发生过转换并重试。

### 17.4 代价与适用边界

层次迁移减少 idle CPU 唤醒，却增加：

- per-CPU/base/group 多层锁和锁序；
- CPU hotplug、isolation、NUMA 拓扑变化的维护路径；
- 远端 callback 代理和期限传播；
- 惰性状态造成的保守多唤醒。

协议允许某些竞态最多导致额外 IPI 或较早唤醒，但不能允许漏掉最早期限。理解这类代码时，应先区分
“安全但多做工作”和“违反到期保证”两类竞态后果。

## 18. vDSO：为什么用户读时间通常没有系统调用

### 18.1 普通 syscall 的固定成本

clock_gettime 是高频操作。若每次都从用户态进入内核，即使 timekeeping 计算只需几十条指令，也要支付
特权级切换、入口安全和返回路径成本。

vDSO 把一段受内核控制的用户态代码和 timekeeping 数据页映射到进程。用户 reader 可以：

```text
读 vDSO sequence
→ 复制 clock mode、cycle_last、mask、mult/shift、base/offset
→ 直接读允许用户态访问的硬件 counter
→ 算出目标时间域
→ 再检查 sequence，变化则重试
```

这与内核 seqcount reader 的思想相同，但数据布局是稳定的用户 ABI，体系结构还必须提供安全的 counter
读取方法。

### 18.2 update_vsyscall() 是发布边界

timekeeper 写事务完成一代状态后，`update_vsyscall()` 把所需字段发布到 vDSO data page。写侧 sequence
包围字段更新；reader 只有在前后 sequence 相同且为稳定代时才接受结果。

sequence 在读取期间变化时，vDSO reader 留在用户态重试；若当前 clocksource 不能在用户态直接读、
clock mode 不受支持或该 clockid 没有 vDSO 实现，才回退到真正的 clock_gettime syscall。vDSO 是快速
路径，不是另一套时间真相。

### 18.3 fast 不代表没有版本和架构边界

不同架构可使用不同 counter、barrier 和 vDSO 数据布局。应用只能调用 libc/vDSO 暴露的 ABI，不能复制
某架构实现并假设所有 Linux 都能从用户态读取同一寄存器。

## 19. time namespace：共享时钟，改变观察坐标

time namespace 主要虚拟化 MONOTONIC 和 BOOTTIME offset。REALTIME 通常仍是系统级墙钟。这样容器可以
看到自己的“启动已多久”，但底层仍共享同一个稳定 clocksource 和 timekeeper。

### 19.1 为什么 offset 比复制 timekeeper 更合理

复制一套 timekeeper 会带来：

- 每个 namespace 都要响应 tick、NTP、换源和 suspend；
- 多份 cycle_last 与误差累计可能漂移；
- vDSO 和 timer 队列难以共享；
- namespace 数量直接放大热路径工作。

offset 方案只在边界换坐标：

```text
读取：容器时间 = 宿主时间 + namespace offset
绝对 timer 入队：宿主期限 = 容器期限 - namespace offset
相对 timer：时长不变
```

### 19.2 offset 为什么需要冻结

一旦 namespace 中已有任务和绝对 timer，任意修改 offset 会让已发布的时间坐标与期限语义突变。内核在
适当生命周期点冻结 offset，并用 `timens_offset_lock` 串行检查、更新和 vDSO 初始化提交。

fork/setns 提交新 namespace 后，旧 VVAR 页表映射需要失效，让后续 fault 按新 namespace 选择普通或专属
VVAR 页。页面由 namespace 持有，映射/fault 还会取得自己的页引用；sequence 只保护内容一致性，页面
生命周期仍靠引用和 VMA 协议。

## 20. 并发正确性：四个最值得掌握的竞态

### 20.1 reader 混用两代 timekeeper 字段

```text
reader                         writer
读旧 cycle_last               seq 进入奇数
                               更新 clock/mult/base
读新 mult
算出错误时间                  seq 进入下一偶数
```

seqcount 前后验证让 reader 发现代次变化并重试。raw spin lock 串行 writer，seqcount 为 reader 提供检测，
二者职责不同。

### 20.2 timer 删除与 callback 已开始

```text
销毁线程                       timer CPU
普通 delete 未发现队列节点     已摘除节点，准备 callback
释放 container                 callback 访问 container
```

需要跨越 callback 生命周期时必须使用 sync delete 或 shutdown。队列锁只能保护“节点是否挂在队列”，不自动
保护 callback 外部对象。

### 20.3 hrtimer 远端入队与本 CPU 正在处理到期

hrtimer IRQ 暂时把 `cpu_base->expires_next` 设为 `KTIME_MAX`，避免远端 CPU 在本 CPU 正扫描各 base 时把一个
更早 timer 送进来并基于过渡缓存错误重编。本地入队仍能由当前锁和最终重算吸收。这里的哨兵是并发协议，
不只是“当前没有 timer”。

### 20.4 idle 发布与更早 timer 并发

CPU 停 tick/进入 migration inactive 与远端新 timer 入队并发时，必须确保至少一方负责：要么 idle CPU 看见
新 deadline 并重编，要么入队者看见 inactive 并向层次/目标 CPU 发通知。原子状态、sequence、base lock
和唤醒兜底共同关闭漏事件窗口。

## 21. 失败、退化与不能保证的事情

### 21.1 clocksource 退化

主来源可能 watchdog 失败、频率变化异常或 resume 后不连续。内核可标 unstable 并切换后备源，最坏还可
使用低 rating 的 jiffies clocksource。系统保持时间 API 可用，代价可能是精度和读开销下降。

### 21.2 clockevent 编程失败

设备拒绝过小 delta 或编程时期限已过，通用层会提高最小 delta、重试或立即处理。持续失败会产生告警，
但代码要避免无限重编活锁。

### 21.3 timer 晚到是允许存在的现象

timer 通常保证不早到，不保证在 expiry 那一纳秒执行。晚到来源包括：

- IRQ disabled 临界区；
- 更高优先级中断或 callback；
- softirq backlog；
- CPU 深 idle 唤醒；
- 虚拟机 vCPU 未被宿主调度；
- 被唤醒任务尚未被调度器选中。

定位延迟必须把“硬件 IRQ 到达”“callback 执行”“任务真正运行”分开测量。

### 21.4 配置关闭时 ABI 与实现能力不同

`CONFIG_POSIX_TIMERS=n` 时 `posix-stubs.c` 仍保留部分 clock ABI，支持基础 gettime/nanosleep，并对不支持
操作返回错误。`CONFIG_HIGH_RES_TIMERS=n` 时 hrtimer 仍存在，但由低分辨率 tick 驱动。`CONFIG_NO_HZ=n`
则不进入动态停 tick 主路径。

文档或实验必须写明配置。看到函数存在，不代表运行内核启用了对应能力。

## 22. 用实验把理解变成证据

### 22.1 实验一：同时观察四个时间域

最小程序：

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>

static void show(clockid_t id, const char *name)
{
        struct timespec ts;

        if (clock_gettime(id, &ts) == 0)
                printf("%-14s %lld.%09ld\n", name,
                       (long long)ts.tv_sec, ts.tv_nsec);
}

int main(void)
{
        show(CLOCK_MONOTONIC, "MONOTONIC");
        show(CLOCK_BOOTTIME, "BOOTTIME");
        show(CLOCK_REALTIME, "REALTIME");
        show(CLOCK_MONOTONIC_RAW, "RAW");
        return 0;
}
```

普通运行前后，各 elapsed 应接近。可控测试机 suspend/resume 后，BOOTTIME 和 REALTIME 通常包含睡眠，
MONOTONIC 不包含。该实验证明用户可见语义，不能单独证明 resume 使用 non-stop clocksource 还是 RTC。

可用 `strace` 辅助判断 libc 是否回退 syscall；没有出现 clock_gettime syscall 通常说明 vDSO 成功，但
strace 本身及 libc 版本会影响观察。

### 22.2 实验二：绝对周期与相对周期的漂移

实现两组 1 ms 循环：一组每次 `nanosleep(1 ms)`，另一组固定：

```text
deadline = start + N * 1 ms
clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline)
```

同时周期发送信号或制造 CPU 压力。相对循环会把每次晚醒加入下一周期，绝对循环只表现为单次 miss，
后续仍追赶原时间线。它验证 absolute deadline 语义，不证明任务具备硬实时保证。

### 22.3 实验三：拆分 timer latency

在具备相应配置和权限的测试机上使用 timer/hrtimer tracepoint、ftrace 或 timerlat tracer，记录：

```text
软件设置 expiry
→ clockevent IRQ/handler
→ hrtimer/timer callback
→ sched_wakeup
→ sched_switch 选中目标任务
```

分别在空闲、CPU 压力和较深 idle 条件下测试。若 IRQ 准时而 sched_switch 很晚，问题在调度；若 IRQ 本身
很晚，应检查 IRQ-off、clockevent、broadcast 和 idle wakeup，而不是笼统归因于 hrtimer 红黑树。

### 22.4 实验四：clocksource 选择与 watchdog 证据

```bash
cat /sys/devices/system/clocksource/clocksource0/current_clocksource
cat /sys/devices/system/clocksource/clocksource0/available_clocksource
dmesg | grep -i clocksource
```

在可恢复测试环境中才切换可用 clocksource，并比较读时间成本与 dmesg。`clocksource-wdtest.c` 需要对应
测试配置；故障注入不应在生产机执行。

### 22.5 实验五：NO_HZ 是否真的减少 tick

先检查：

```bash
grep -E 'CONFIG_(NO_HZ|NO_HZ_IDLE|NO_HZ_FULL|HIGH_RES_TIMERS)=' /boot/config-$(uname -r)
cat /proc/cmdline
```

再观察 idle、tick stop/restart、timer enqueue/expire 和中断计数。若内核未开启配置或 tracepoint 不存在，
结果只能写“当前环境不具备验证条件”，不能据此否定源码路径。

## 23. 推荐源码阅读顺序

### 第一遍：建立主路径

1. `kernel/time/Makefile`、`Kconfig`：确认构建边界；
2. `include/linux/timekeeping.h` 与 `Documentation/core-api/timekeeping.rst`：先学公开语义；
3. `kernel/time/timekeeping.c`：只读 `ktime_get()`、`timekeeping_init()`、`update_wall_time()`；
4. `kernel/time/posix-timers.c`：读 clock 操作表和 `clock_nanosleep`；
5. `kernel/time/hrtimer.c`：读 `hrtimer_nanosleep()`、入队和中断；
6. `kernel/time/clockevents.c`、`tick-oneshot.c`：看期限怎样落到设备。

完成后应能复述贯穿案例，不要求理解所有配置分支。

### 第二遍：补全状态与生命周期

1. `include/linux/timekeeper_internal.h`：读 base、offset 和 NTP 字段；
2. `kernel/time/clocksource.c`：注册、watchdog、select 和换源；
3. `kernel/time/timer.c`：timer wheel、删除和 callback 生命周期；
4. `kernel/time/ntp.c`：频率、误差和秒边界；
5. `kernel/time/alarmtimer.c`、`posix-cpu-timers.c`：比较不同时间依据。

### 第三遍：补全跨 CPU 与低功耗

1. `kernel/time/tick-sched.c`：下一事件合并与 stop/restart；
2. `kernel/time/tick-broadcast.c`：深 idle 共享设备；
3. `kernel/time/timer_migration.c/.h`：远端代理和原子状态；
4. `kernel/time/namespace*.c` 与 `include/vdso/`：用户快速路径和虚拟时间坐标；
5. 选择当前体系结构一个真实 timer driver，核对 read 与 set-next-event 最终落点。

## 24. 读完后的自我检查

不看正文，尝试回答：

1. clocksource 与 clockevent 各自只有哪一个核心方向？
2. 管理员把 REALTIME 向前调一小时，为什么 MONOTONIC sleep 不应立刻到期？
3. `ktime_get()` 怎样组合已提交 base 与当前 cycle delta？
4. seqcount 保证什么，为什么它不等于对象生命周期保护？
5. hrtimer 成为队列最左节点后，哪一层负责重编硬件？
6. timer 已从队列摘除，为什么对象仍可能不能释放？
7. NO_HZ 停 tick 前必须询问哪些消费者？
8. 本地 clockevent 在深 idle 停止时，broadcast 怎样避免漏事件？
9. timer migration 中 sequence 防止哪类 active/idle 竞态？
10. vDSO 为什么快，什么情况下会回退 syscall？
11. time namespace 为什么只需转换绝对期限，不转换相对时长？
12. timer 的纳秒 expiry 为什么不等于任务纳秒级准时运行？

如果只能背出函数名，却不能说明状态、同步和失败后果，说明还没有真正理解这条路径。

## 25. 总结：把整个系统压缩成一条因果链

Linux time 子系统可以压缩为下面这条主线：

```text
clocksource 提供连续 cycle
→ timekeeping 用 mult/shift 累积并发布多个时间域
→ 内核或 vDSO reader 用 seqcount/latch 得到一致“现在”
→ timer_list/hrtimer/POSIX timer 把未来工作放入合适队列
→ tick/NO_HZ 汇总每 CPU 最早期限
→ clockevent 把期限写给硬件
→ IRQ 到来后执行到期 callback 或唤醒任务
→ broadcast/migration 在 idle 和跨 CPU 场景维持同一到期保证
→ NTP、换源、设时、suspend 和 namespace 只改变各自负责的坐标或推进方式
```

其中最重要的设计边界是：

- 读时间与制造中断分离；
- 时间坐标与 timer 队列分离；
- 低成本 timeout 与精确 hrtimer 分离；
- reader 一致性与对象生命周期分离；
- timer 到期与任务真正运行分离；
- 快速路径和故障/退化路径分离。

掌握这些边界后，再读 `kernel/time/` 中复杂的锁、per-CPU 状态和配置分支，就能知道每段代码究竟在
维护哪一个不变量，而不是把它们看成一堆互相调用的时间函数。
