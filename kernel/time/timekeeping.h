/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KERNEL_TIME_TIMEKEEPING_H
#define _KERNEL_TIME_TIMEKEEPING_H
/*
 * Internal interfaces for kernel/time/
 */
/*
 * 本头文件只暴露 kernel/time 子系统内部的跨文件契约：hrtimer 快照、绝对到期时间到硬件 cycle 的换算、
 * timekeeping/sched_clock 休眠阶段，以及周期 tick 推进接口。它不拥有所声明对象；定义和同步协议分布在
 * timekeeping.c、jiffies.c、timer.c 与 sched_clock.c，调用者必须遵守各接口注明的上下文和锁条件。
 */

/*
 * ktime_get_update_offsets_now() - 为 hrtimer 一次性取得同版本的 mono now、墙钟设时序号和三种偏移。
 * @cwsseq、@offs_real、@offs_boot、@offs_tai 均是调用者提供的非空输出指针；成功返回当前 monotonic
 * ktime。实现以 timekeeper seqcount 重试；仅当 `*cwsseq` 落后时刷新四个输出，否则保留调用者缓存，
 * 但返回值始终来自有效快照。不睡眠、不转移引用；调用者通常持 hrtimer CPU base 锁并据版本重排 timer。
 */
extern ktime_t ktime_get_update_offsets_now(unsigned int *cwsseq,
					    ktime_t *offs_real,
					    ktime_t *offs_boot,
					    ktime_t *offs_tai);

/*
 * ktime_expiry_to_cycles() - 尝试把绝对 monotonic 到期纳秒换成指定 clocksource 的绝对 cycle。
 * @id 标识调用设备绑定的 clocksource；@expires_ns 是绝对到期时间；@cycles 为非空输出。只有当前
 * timekeeper 使用同一 id 时才写输出并返回 true，过早/过远到期值分别钳到 0/安全最大 delta；false
 * 要求调用者回退到读取当前时间再算相对 delta。读侧以 seqcount 取一致快照，不持有 clocksource 引用。
 */
bool ktime_expiry_to_cycles(enum clocksource_ids id, ktime_t expires_ns, u64 *cycles);

/*
 * timekeeping_valid_for_hres() - 判断当前主 clocksource 是否满足高分辨率/oneshot 时间基准要求。
 * 无入参；返回非零表示源具备 CLOCK_SOURCE_VALID_FOR_HRES，返回 0 表示 tick 层必须保持低分辨率路径。
 * 这是对当前 timekeeper 状态的只读查询，不保证返回后源不再切换，调用者应把结果用于当次模式决策。
 */
extern int timekeeping_valid_for_hres(void);
/*
 * timekeeping_max_deferment() - 返回当前 clocksource 在不丢失换算精度或回绕信息前可延后的纳秒上限。
 * 无入参，返回 u64 纳秒；NO_HZ 用它限制下一事件。结果是瞬时快照，换源后可能变化，不睡眠且无错误码。
 */
extern u64 timekeeping_max_deferment(void);
/*
 * timekeeping_warp_clock() - 首次仅设置时区时，把按本地时间解释的启动墙钟校正为 UTC。
 * 无入参/返回值；内部按 sys_tz 计算偏移并走正式设时事务，可能触发 clock-was-set 通知。只能从允许
 * timekeeping 写锁和通知链工作的进程上下文调用；不应作为普通调时接口重复使用。
 */
extern void timekeeping_warp_clock(void);
/*
 * timekeeping_suspend() - 在 syscore 晚期冻结时间维护并记录休眠前基线。
 * 无入参，当前实现固定返回 0。调用时仅剩单 CPU、进程已冻结且中断关闭；它串行停止
 * tick/clocksource/clockevent 更新并发布 suspend 状态，不能由普通运行期路径调用。
 */
extern int timekeeping_suspend(void);
/*
 * timekeeping_resume() - 在 syscore 早期恢复 clocksource，并把可确认的睡眠时长注入墙钟/boottime。
 * 无入参/返回值；依赖与 suspend 配对的单 CPU、进程冻结和中断关闭语境。持久时钟不可用或倒退时按
 * 实现策略跳过/修正注入而不返回错误，随后重启 timekeeping 及相关 tick/clocksource 基础设施。
 */
extern void timekeeping_resume(void);
#ifdef CONFIG_GENERIC_SCHED_CLOCK
/*
 * sched_clock_suspend() - 配置 generic sched_clock 时冻结其 epoch 和 wrap timer。
 * 无入参；当前实现返回 0。只能在系统核心 suspend 串行阶段调用，保证恢复前 sched_clock 读数不累计
 * 睡眠时间；静态 sched_clock 状态由实现拥有，本声明不转移生命周期。
 */
extern int sched_clock_suspend(void);
/*
 * sched_clock_resume() - 配置 generic sched_clock 时以恢复后的硬件 cycle 重新锚定冻结 epoch。
 * 无入参/返回值；必须与 suspend 配对并在系统核心恢复串行阶段调用，随后重新启动 wrap timer。
 */
extern void sched_clock_resume(void);
#else
/*
 * sched_clock_suspend() - 未启用 generic sched_clock 时的无副作用 suspend 桩。
 * 无入参，固定返回 0，使 tick/syscore 调用点无需条件编译；不取锁、不睡眠，也不表示存在可冻结的时钟。
 */
static inline int sched_clock_suspend(void) { return 0; }
/*
 * sched_clock_resume() - 未启用 generic sched_clock 时的空 resume 桩。
 * 无入参/返回值且无副作用，仅保持调用接口对称；任意符合上层 resume 协议的上下文均可调用。
 */
static inline void sched_clock_resume(void) { }
#endif

/*
 * update_process_times() - 为当前 CPU/任务处理一次周期 tick 的记账和子系统推进。
 * @user 为布尔语义：非零表示 tick 中断了用户态，0 表示内核态。硬中断 tick 路径调用，依次进行任务
 * CPU 时间、local timer、RCU、irq_work、scheduler 和 POSIX CPU timer 处理；无返回值且不能睡眠。
 */
extern void update_process_times(int user);
/*
 * do_timer() - 推进全局 jiffies 并更新全局负载采样。
 * @ticks 是本次补记的 tick 数，可大于 1；调用者必须持有 jiffies_lock，并在需要一致读快照的路径由
 * jiffies_seq 写段包围。无返回值、不自行加锁、不睡眠；错误的并发调用会重复或丢失全局时间推进。
 */
extern void do_timer(unsigned long ticks);
/*
 * update_wall_time() - 从当前 clocksource 推进主墙钟及已启用的辅助 timekeeper。
 * 无入参/返回值；tick 路径在不持 jiffies_lock 后调用。内部取得 timekeeper irqsave raw lock，提交
 * 新状态/VDSO，必要时延迟发送 clock-was-set 通知；原子上下文可调用，但同步通知被推迟且本函数不睡眠。
 */
extern void update_wall_time(void);

/*
 * jiffies_lock 是全局 tick 写者的 raw spinlock；jiffies_seq 是绑定该锁的 seqcount，令无锁读者取得完整
 * jiffies_64 快照。写者锁序必须先取得 jiffies_lock 再开启 seq 写段，并在退出前反序结束；二者由
 * jiffies.c 静态常驻定义，本头文件只借用声明，不负责初始化或生命周期。
 */
extern raw_spinlock_t jiffies_lock;
extern seqcount_raw_spinlock_t jiffies_seq;

/* clocksource/clockevent 的内部名称缓冲上限含结尾 NUL，写入接口最多接受 31 个名称字节。 */
#define CS_NAME_LEN	32

#endif
