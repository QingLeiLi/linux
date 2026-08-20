// SPDX-License-Identifier: GPL-2.0
/*
 * List pending timers
 *
 * Copyright(C) 2006, Red Hat, Inc., Ingo Molnar
 */
/*
 * 本文件生成 `/proc/timer_list` 与 SysRq-Q 共用的诊断快照：按在线 CPU 输出每个 hrtimer clock base、
 * 活动 timer 和 NO_HZ 状态，再按配置输出 broadcast/per-CPU clockevent。它不是一致性 ABI：只在复制单个
 * hrtimer 时持 base 锁，其余计数、online mask 和 tick device 均允许并发变化，以避免诊断读取扰动热路径。
 */

#include <linux/proc_fs.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/kallsyms.h>
#include <linux/nmi.h>

#include <linux/uaccess.h>

#include "tick-internal.h"

struct timer_list_iter {
	int cpu;
	bool second_pass;
	u64 now;
};
/*
 * 每个打开的 proc seq_file 私有一份迭代器：cpu=-1 是每轮 header/broadcast 哨兵，second_pass=false 输出
 * hrtimer/NO_HZ，true 输出 clockevent；now 在 offset=0 时采样一次，使同次遍历的相对到期值共享基准。
 */

/*
 * This allows printing both to /proc/timer_list and
 * to the console (on SysRq-Q):
 */
/* 同一格式化入口既可写 seq_file，也可在 SysRq-Q 中直接写控制台。 */
/*
 * SEQ_printf() - 按 @m 是否为空把 printf 风格内容路由到 proc seq 缓冲或 printk。
 * @m 为可空借用指针；@fmt 及可变参数遵循 printf，`__printf(2,3)` 启用编译期格式检查。args 只在本调用
 * va_start/end 之间有效。无返回值，seq/console 输出失败不传播；proc 路径可睡眠，SysRq 路径须可原子打印。
 */
__printf(2, 3)
static void SEQ_printf(struct seq_file *m, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);

	if (m)
		seq_vprintf(m, fmt, args);
	else
		vprintk(fmt, args);

	va_end(args);
}

/*
 * print_timer() - 输出一个已复制 hrtimer 的身份、回调、排队位和相对/绝对到期范围。
 * @m 可空决定 proc/console；@taddr 只作为原对象地址值打印，不解引用；@timer 指向调用者栈上稳定副本；
 * @idx 是该 base 遍历序号；@now 是对应 clock base 坐标的纳秒基准。无返回值，不持锁、不拥有对象；
 * soft/hard expiry 减 now 可为负，表示已过期但尚未执行/正在竞态处理。
 */
static void
print_timer(struct seq_file *m, struct hrtimer *taddr, struct hrtimer *timer,
	    int idx, u64 now)
{
	SEQ_printf(m, " #%d: <%p>, %ps", idx, taddr, ACCESS_PRIVATE(timer, function));
	SEQ_printf(m, ", S:%02x", timer->is_queued);
	SEQ_printf(m, "\n");
	SEQ_printf(m, " # expires at %Lu-%Lu nsecs [in %Ld to %Ld nsecs]\n",
		(unsigned long long)ktime_to_ns(hrtimer_get_softexpires(timer)),
		(unsigned long long)ktime_to_ns(hrtimer_get_expires(timer)),
		(long long)(ktime_to_ns(hrtimer_get_softexpires(timer)) - now),
		(long long)(ktime_to_ns(hrtimer_get_expires(timer)) - now));
}

/*
 * print_active_timers() - 逐个复制并输出一个 hrtimer clock base 当前活动队列。
 * @m 可空；@base 是静态 per-CPU base 借用指针；@now 已换到该 base 坐标。curr/timer 仅在 cpu_base raw lock
 * 内有效，tmp 是解锁前的完整栈副本，flags 保存 IRQ 状态；next/i 通过每轮从头走到第 N 项推进。
 *
 * 打印可能睡眠/耗时，故不能持锁；实现以 O(N^2) 反复加锁、定位、复制、解锁、打印，并触碰 NMI watchdog。
 * 并发插入/删除可令诊断输出重复或跳项，但绝不在解锁后解引用原 timer。无返回值或稳定快照保证。
 */
static void print_active_timers(struct seq_file *m, struct hrtimer_clock_base *base, u64 now)
{
	struct timerqueue_linked_node *curr;
	struct hrtimer *timer, tmp;
	unsigned long next = 0, i;
	unsigned long flags;

next_one:
	i = 0;

	touch_nmi_watchdog();

	raw_spin_lock_irqsave(&base->cpu_base->lock, flags);

	curr = timerqueue_linked_first(&base->active);
	/*
	 * Crude but we have to do this O(N*N) thing, because
	 * we have to unlock the base when printing:
	 */
	/* 打印期间必须释放 base 锁，只能每次重新从队首走到 next，换取对象字段复制时的一致性。 */
	while (curr && i < next) {
		curr = timerqueue_linked_next(curr);
		i++;
	}

	if (curr) {

		timer = container_of(curr, struct hrtimer, node);
		tmp = *timer;
		raw_spin_unlock_irqrestore(&base->cpu_base->lock, flags);

		print_timer(m, timer, &tmp, i, now);
		next++;
		goto next_one;
	}
	raw_spin_unlock_irqrestore(&base->cpu_base->lock, flags);
}

/*
 * print_base() - 输出一个 hrtimer clock base 的元数据和活动 timer。
 * @m 可空；@base 为借用指针；@now 是 monotonic 纳秒快照。高分辨率配置下显示 base offset，并把
 * `now+offset` 传给活动队列，使 realtime/boottime 等 timer 的相对差在各自坐标中计算。除单 timer
 * 复制由下层加锁外，resolution/index/offset 是诊断性无锁读取；无返回值。
 */
static void
print_base(struct seq_file *m, struct hrtimer_clock_base *base, u64 now)
{
	SEQ_printf(m, "  .base:       %p\n", base);
	SEQ_printf(m, "  .index:      %d\n", base->index);

	SEQ_printf(m, "  .resolution: %u nsecs\n", hrtimer_resolution);
#ifdef CONFIG_HIGH_RES_TIMERS
	SEQ_printf(m, "  .offset:     %Ld nsecs\n",
		   (long long) base->offset);
#endif
	SEQ_printf(m,   "active timers:\n");
	print_active_timers(m, base, now + ktime_to_ns(base->offset));
}

/*
 * print_cpu() - 输出一个 CPU 的全部 hrtimer bases、highres 计数和可选 NO_HZ tick_sched 状态。
 * @m 可空；@cpu 应是调用时在线的有效 CPU id；@now 是本轮 monotonic 纳秒快照。cpu_base/ts 都是 per-CPU
 * 常驻对象借用指针，i 遍历所有 clock base。活动 timer 由下层逐项锁内复制，其余字段和 jiffies 无锁读取，
 * 可跨多个更新版本；CPU hotplug/NO_HZ 变化也只影响诊断一致性。无返回值，不取得 hotplug 或对象引用。
 */
static void print_cpu(struct seq_file *m, int cpu, u64 now)
{
	struct hrtimer_cpu_base *cpu_base = &per_cpu(hrtimer_bases, cpu);
	int i;

	SEQ_printf(m, "cpu: %d\n", cpu);
	for (i = 0; i < HRTIMER_MAX_CLOCK_BASES; i++) {
		SEQ_printf(m, " clock %d:\n", i);
		print_base(m, cpu_base->clock_base + i, now);
	}
#define P(x) \
	SEQ_printf(m, "  .%-15s: %Lu\n", #x, \
		   (unsigned long long)(cpu_base->x))
#define P_ns(x) \
	SEQ_printf(m, "  .%-15s: %Lu nsecs\n", #x, \
		   (unsigned long long)(ktime_to_ns(cpu_base->x)))
	/* P/P_ns 仅在本函数词法范围生成 cpu_base 字段的普通值或 ktime→ns 格式化，使用后立即 undef。 */

#ifdef CONFIG_HIGH_RES_TIMERS
	P_ns(expires_next);
	P(hres_active);
	P(nr_events);
	P(nr_retries);
	P(nr_hangs);
	P(max_hang_time);
#endif
#undef P
#undef P_ns

#ifdef CONFIG_TICK_ONESHOT
# define P(x) \
	SEQ_printf(m, "  .%-15s: %Lu\n", #x, \
		   (unsigned long long)(ts->x))
# define P_ns(x) \
	SEQ_printf(m, "  .%-15s: %Lu nsecs\n", #x, \
		   (unsigned long long)(ktime_to_ns(ts->x)))
# define P_flag(x, f)			    \
	SEQ_printf(m, "  .%-15s: %d\n", #x, !!(ts->flags & (f)))
	/* oneshot 分支把相同打印宏改绑到 tick_sched；P_flag 把 bitmask 规范成 0/1。 */

	{
		struct tick_sched *ts = tick_get_tick_sched(cpu);
		P_flag(nohz, TS_FLAG_NOHZ);
		P_flag(highres, TS_FLAG_HIGHRES);
		P_ns(last_tick);
		P_flag(tick_stopped, TS_FLAG_STOPPED);
		P(idle_calls);
		P(idle_sleeps);
		P_ns(idle_entrytime);
		P_ns(idle_waketime);
		P(last_jiffies);
		P(next_timer);
		P_ns(idle_expires);
		SEQ_printf(m, "jiffies: %Lu\n",
			   (unsigned long long)jiffies);
	}
#endif

#undef P
#undef P_ns
	SEQ_printf(m, "\n");
}

#ifdef CONFIG_GENERIC_CLOCKEVENTS
/*
 * print_tickdevice() - 输出 broadcast 或指定 CPU tick_device 及其 clock_event_device 回调/范围/状态。
 * @m 可空；@td 是 tick core 常驻借用对象；@cpu<0 表示 broadcast，否则是 per-CPU id。dev 为空时只打印
 * `<NULL>` 早退；非空时读取名称、换算参数、next_event、状态回调、handler、重试和可选 wakeup device。
 * td 本身为静态 per-CPU/broadcast 对象，但函数不为 evtdev 取得模块/设备引用；并发替换可能混合字段，
 * 驱动仍须遵守 clockevents 已注册对象生命周期。无返回值，长输出间触碰 NMI watchdog。
 */
static void
print_tickdevice(struct seq_file *m, struct tick_device *td, int cpu)
{
	struct clock_event_device *dev = td->evtdev;

	touch_nmi_watchdog();

	SEQ_printf(m, "Tick Device: mode:     %d\n", td->mode);
	if (cpu < 0)
		SEQ_printf(m, "Broadcast device\n");
	else
		SEQ_printf(m, "Per CPU device: %d\n", cpu);

	SEQ_printf(m, "Clock Event Device: ");
	if (!dev) {
		SEQ_printf(m, "<NULL>\n");
		return;
	}
	SEQ_printf(m, "%s\n", dev->name);
	SEQ_printf(m, " max_delta_ns:   %llu\n",
		   (unsigned long long) dev->max_delta_ns);
	SEQ_printf(m, " min_delta_ns:   %llu\n",
		   (unsigned long long) dev->min_delta_ns);
	SEQ_printf(m, " mult:           %u\n", dev->mult);
	SEQ_printf(m, " shift:          %u\n", dev->shift);
	SEQ_printf(m, " mode:           %d\n", clockevent_get_state(dev));
	SEQ_printf(m, " next_event:     %Ld nsecs\n",
		   (unsigned long long) ktime_to_ns(dev->next_event));

	SEQ_printf(m, " set_next_event: %ps\n", dev->set_next_event);

	if (dev->set_state_shutdown)
		SEQ_printf(m, " shutdown:       %ps\n",
			dev->set_state_shutdown);

	if (dev->set_state_periodic)
		SEQ_printf(m, " periodic:       %ps\n",
			dev->set_state_periodic);

	if (dev->set_state_oneshot)
		SEQ_printf(m, " oneshot:        %ps\n",
			dev->set_state_oneshot);

	if (dev->set_state_oneshot_stopped)
		SEQ_printf(m, " oneshot stopped: %ps\n",
			dev->set_state_oneshot_stopped);

	if (dev->tick_resume)
		SEQ_printf(m, " resume:         %ps\n",
			dev->tick_resume);

	SEQ_printf(m, " event_handler:  %ps\n", dev->event_handler);
	SEQ_printf(m, "\n");
	SEQ_printf(m, " retries:        %lu\n", dev->retries);

#ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST
	if (cpu >= 0) {
		const struct clock_event_device *wd = tick_get_wakeup_device(cpu);

		SEQ_printf(m, "Wakeup Device: %s\n", wd ? wd->name : "<NULL>");
	}
#endif
	SEQ_printf(m, "\n");
}

/*
 * timer_list_show_tickdevices_header() - 输出 clockevent 第二遍的 broadcast 设备与相关 CPU mask。
 * @m 可空；仅 GENERIC_CLOCKEVENTS_BROADCAST 下有内容，并在 ONESHOT 下追加 oneshot mask。mask/tick device
 * 都是 tick core 借用的无锁诊断快照；无返回值，不改变 broadcast 状态。
 */
static void timer_list_show_tickdevices_header(struct seq_file *m)
{
#ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST
	print_tickdevice(m, tick_get_broadcast_device(), -1);
	SEQ_printf(m, "tick_broadcast_mask: %*pb\n",
		   cpumask_pr_args(tick_get_broadcast_mask()));
#ifdef CONFIG_TICK_ONESHOT
	SEQ_printf(m, "tick_broadcast_oneshot_mask: %*pb\n",
		   cpumask_pr_args(tick_get_broadcast_oneshot_mask()));
#endif
	SEQ_printf(m, "\n");
#endif
}
#endif

/*
 * timer_list_header() - 输出格式版本、clock base 数量和本轮统一 monotonic now。
 * @m 可空；@now 为纳秒值。无返回值或状态访问，格式版本 v0.11 是用户诊断文本标记而非稳定 ABI 承诺。
 */
static inline void timer_list_header(struct seq_file *m, u64 now)
{
	SEQ_printf(m, "Timer List Version: v0.11\n");
	SEQ_printf(m, "HRTIMER_MAX_CLOCK_BASES: %d\n", HRTIMER_MAX_CLOCK_BASES);
	SEQ_printf(m, "now at %Ld nsecs\n", (unsigned long long)now);
	SEQ_printf(m, "\n");
}

/*
 * sysrq_timer_list_show() - 把 timer_list 全量快照直接打印到控制台。
 * 无入参/返回值；先采一次 monotonic now，遍历当时 online CPU 输出 hrtimer/NO_HZ，再按配置输出 broadcast
 * 和每 CPU clockevent。SysRq 路径不持 CPU hotplug 读锁，online 集合和设备字段可变化；输出很长但各
 * timer 仅短持 raw lock，并周期触碰 watchdog。`m=NULL` 保证所有文本走 printk 而非 seq_file。
 */
void sysrq_timer_list_show(void)
{
	u64 now = ktime_to_ns(ktime_get());
	int cpu;

	timer_list_header(NULL, now);

	for_each_online_cpu(cpu)
		print_cpu(NULL, cpu, now);

#ifdef CONFIG_GENERIC_CLOCKEVENTS
	timer_list_show_tickdevices_header(NULL);
	for_each_online_cpu(cpu)
		print_tickdevice(NULL, tick_get_device(cpu), cpu);
#endif
	return;
}

#ifdef CONFIG_PROC_FS
/*
 * timer_list_show() - 把 seq iterator 当前逻辑位置分派到 header/CPU 或 clockevent 第二遍输出。
 * @m 是 seq_file；@v 必须是其 private `timer_list_iter`。第一遍 cpu=-1 打 header，其余 CPU 打 hrtimer；
 * 配置 clockevents 时第二遍 cpu=-1 打 broadcast header，其余打 per-CPU device。成功固定返回 0，底层
 * 输出错误不单独传播；函数不推进 iterator，now 由 start 为本次遍历冻结。
 */
static int timer_list_show(struct seq_file *m, void *v)
{
	struct timer_list_iter *iter = v;

	if (iter->cpu == -1 && !iter->second_pass)
		timer_list_header(m, iter->now);
	else if (!iter->second_pass)
		print_cpu(m, iter->cpu, iter->now);
#ifdef CONFIG_GENERIC_CLOCKEVENTS
	else if (iter->cpu == -1 && iter->second_pass)
		timer_list_show_tickdevices_header(m);
	else
		print_tickdevice(m, tick_get_device(iter->cpu), iter->cpu);
#endif
	return 0;
}

/*
 * move_iter() - 从 iterator 当前状态向前移动 @offset 个 seq 逻辑记录。
 * @iter 是可写私有状态；@offset 为非负步数。每步用当前 `cpu_online_mask` 找下一在线 CPU；第一遍耗尽后，
 * 有 clockevents 时切到 cpu=-1/second_pass=true 作为 broadcast 记录，再遍历第二遍 CPU，最终返回 NULL；
 * 无 clockevents 时第一遍耗尽即 NULL。返回 @iter 或结束哨兵，不持 hotplug 锁，CPU 集合变化可跳项/重复。
 */
static void *move_iter(struct timer_list_iter *iter, loff_t offset)
{
	for (; offset; offset--) {
		iter->cpu = cpumask_next(iter->cpu, cpu_online_mask);
		if (iter->cpu >= nr_cpu_ids) {
#ifdef CONFIG_GENERIC_CLOCKEVENTS
			if (!iter->second_pass) {
				iter->cpu = -1;
				iter->second_pass = true;
			} else
				return NULL;
#else
			return NULL;
#endif
		}
	}
	return iter;
}

/*
 * timer_list_start() - 为 seq_file 的目标 offset 重建 timer_list 遍历状态。
 * @file 提供 `timer_list_iter` 私有区；@offset 是非空当前位置。offset=0 时重新采样 monotonic now；随后
 * 总把 cpu=-1、second_pass=false，再由 move_iter 重放到 offset。非零 restart 保留原 now，使缓冲区
 * 分段读取的相对时间基准一致；返回 iterator 或 EOF NULL，不分配/释放私有区。
 */
static void *timer_list_start(struct seq_file *file, loff_t *offset)
{
	struct timer_list_iter *iter = file->private;

	if (!*offset)
		iter->now = ktime_to_ns(ktime_get());
	iter->cpu = -1;
	iter->second_pass = false;
	return move_iter(iter, *offset);
}

/*
 * timer_list_next() - 把 seq 位置加 1 并把私有 iterator 推进一步。
 * @file 提供私有状态；@v 是当前记录但未使用；@offset 非空并原地递增。返回下一 iterator 或 EOF NULL；
 * CPU online mask 的并发变化仍按 move_iter 的诊断性弱一致语义处理。
 */
static void *timer_list_next(struct seq_file *file, void *v, loff_t *offset)
{
	struct timer_list_iter *iter = file->private;
	++*offset;
	return move_iter(iter, 1);
}

/*
 * timer_list_stop() - seq 遍历结束/暂停回调。
 * @seq/@v 均未使用；private iterator 由 proc seq 框架拥有且需跨分段读取保留，因此本函数无释放、解锁或
 * 其他副作用，也无返回值。
 */
static void timer_list_stop(struct seq_file *seq, void *v)
{
}

/* seq 框架按 start→show→next 循环并以 stop 收尾；四个回调共享每次 open 的私有 iterator。 */
static const struct seq_operations timer_list_sops = {
	.start = timer_list_start,
	.next = timer_list_next,
	.stop = timer_list_stop,
	.show = timer_list_show,
};

/*
 * init_timer_list_procfs() - 创建 root `/proc/timer_list` 的只读 seq 文件和每-open 私有 iterator。
 * 无入参；proc_create_seq_private 以 0400 权限注册静态 sops，并让框架为每次打开分配/清零
 * `sizeof(timer_list_iter)` 私有区。创建失败返回 -ENOMEM，成功返回 0；procfs 内建条目无本文件退出清理。
 */
static int __init init_timer_list_procfs(void)
{
	struct proc_dir_entry *pe;

	pe = proc_create_seq_private("timer_list", 0400, NULL, &timer_list_sops,
			sizeof(struct timer_list_iter), NULL);
	if (!pe)
		return -ENOMEM;
	return 0;
}
/* core initcall 阶段注册 proc 条目；CONFIG_PROC_FS 关闭时整段不存在，SysRq 输出仍可用。 */
__initcall(init_timer_list_procfs);
#endif
