// SPDX-License-Identifier: GPL-2.0
/*
 * tick-common.c 中文学习导读
 *
 * 中文学习注释模型：OpenAI GPT-5 Codex。
 *
 * 文件职责：
 *   本文件把通用 clockevents 层提供的硬件“到期后发中断”能力，接到内核
 *   周期 tick、jiffies/timekeeping 更新、进程时间记账和 NO_HZ/broadcast
 *   管理上；它还负责为每个 CPU 选择、安装、更换、挂起和恢复本地
 *   clock_event_device。具体硬件驱动、hrtimer 队列和 NO_HZ 停 tick 算法
 *   分别位于 clockevents 驱动、hrtimer.c 和 tick-sched.c，不由本文件实现。
 *
 * 主调用链：
 *   clockevents_register_device()
 *     -> tick_check_new_device()
 *     -> tick_setup_device()
 *     -> tick_setup_periodic()/tick_setup_oneshot()
 *     -> 硬件到期调用 event_handler
 *     -> tick_handle_periodic()
 *     -> tick_periodic()
 *     -> do_timer() + update_process_times()
 *
 * 核心对象与生命周期：
 *   每个 CPU 的 tick_cpu_device 保存当前借用的 clock_event_device 指针及
 *   PERIODIC/ONESHOT 工作模式。设备由 clockevents 核心注册和持有；tick
 *   层通过模块引用阻止其实现被卸载，并在替换、CPU 下线时交还设备。
 *   tick_do_timer_cpu 指定唯一的常规全局时间更新 CPU；tick_next_period
 *   是全局下一 jiffy 边界，受 jiffies_lock 串行写入，并借助 jiffies_seq
 *   向无锁/seqcount 读者提供一致快照。
 *
 * 并发模型：
 *   设备注册和替换路径由 clockevents_lock 串行化且关闭本地中断；
 *   per-CPU tick_device 只由目标 CPU 或 CPU 热插拔串行区间修改。
 *   周期中断可在不同 CPU 同时发生，但只有 tick_do_timer_cpu 更新全局
 *   时间，jiffies_lock 仍作为最终串行化保证。s2idle freeze/unfreeze
 *   使用 tick_freeze_lock 和深度计数建立“最后冻结者停 timekeeping、
 *   第一恢复者启 timekeeping”的全局屏障。
 *
 * 方案权衡：
 *   per-CPU 设备使本地 tick 无需全局设备锁；单一 do_timer CPU 避免所有
 *   CPU 争抢 timekeeping 锁。代价是 NO_HZ、CPU 热插拔和 suspend 时必须
 *   显式交接职责；会在深 idle 停止的设备还需要 broadcast 设备代为唤醒。
 */
/*
 * This file contains the base functions to manage periodic tick
 * related events.
 *
 * Copyright(C) 2005-2006, Linutronix GmbH, Thomas Gleixner <tglx@kernel.org>
 * Copyright(C) 2005-2007, Red Hat, Inc., Ingo Molnar
 * Copyright(C) 2006-2007, Timesys Corp., Thomas Gleixner
 */
/*
 * 本文件包含管理周期 tick 相关事件的基础函数。这里的“周期”既可以由
 * 硬件周期模式直接产生，也可以用 oneshot 设备逐次编程来模拟。
 * 版权名单属于原始历史材料，不逐项翻译。
 */
#include <linux/compiler.h>
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/nmi.h>
#include <linux/percpu.h>
#include <linux/profile.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <trace/events/power.h>

#include <asm/irq_regs.h>

#include "tick-internal.h"

/*
 * Tick devices
 */
/*
 * 每 CPU tick 设备。evtdev 是借用的当前本地 clockevent 设备，mode 表示
 * tick 层选择的周期或单次模式；per-CPU 布局让中断路径读取本 CPU 状态时
 * 不必取得全局锁。设备对象的注册、模块引用和最终回收仍由 clockevents
 * 核心负责。
 */
DEFINE_PER_CPU(struct tick_device, tick_cpu_device);
/*
 * Tick next event: keeps track of the tick time. It's updated by the
 * CPU which handles the tick and protected by jiffies_lock. There is
 * no requirement to write hold the jiffies seqcount for it.
 */
/*
 * 下一次全局 tick 的时间点：记录 jiffies 时间线下一周期边界。处理全局
 * tick 的 CPU 在 jiffies_lock 下更新它；写这个字段本身不一律要求打开
 * jiffies_seq 写段，因为 tick-sched.c 的 64 位路径还会以 release store
 * 发布，而需要与 jiffies 一致快照的路径会同时遵守 seqcount 协议。
 * ktime_t 使用纳秒时间表示，初次安装 tick 设备时从 ktime_get() 初始化。
 */
ktime_t tick_next_period;

/*
 * tick_do_timer_cpu is a timer core internal variable which holds the CPU NR
 * which is responsible for calling do_timer(), i.e. the timekeeping stuff. This
 * variable has two functions:
 *
 * 1) Prevent a thundering herd issue of a gazillion of CPUs trying to grab the
 *    timekeeping lock all at once. Only the CPU which is assigned to do the
 *    update is handling it.
 *
 * 2) Hand off the duty in the NOHZ idle case by setting the value to
 *    TICK_DO_TIMER_NONE, i.e. a non existing CPU. So the next cpu which looks
 *    at it will take over and keep the time keeping alive.  The handover
 *    procedure also covers cpu hotplug.
 */
/*
 * tick_do_timer_cpu 保存负责调用 do_timer()、推进全局 jiffies/timekeeping
 * 的 CPU 编号。
 *
 * 它有两个目的：
 * 1. 避免大量 CPU 在同一 tick 同时争抢 timekeeping 锁，只有被指派者做
 *    全局更新时间；各 CPU 仍各自执行进程时间和 profiling 记账。
 * 2. NO_HZ idle 下负责人准备长睡时写入 TICK_DO_TIMER_NONE，让下一颗执行
 *    tick 的 CPU 接管；CPU 热插拔也遵循这一交接协议。
 *
 * TICK_DO_TIMER_BOOT 表示早期启动尚未选定负责人，TICK_DO_TIMER_NONE
 * 表示职责暂时空缺。READ_ONCE/WRITE_ONCE 防止编译器拆分、合并或缓存
 * 并发访问；它们不提供互斥，真正的 jiffies 更新仍由 jiffies_lock
 * 串行化。__read_mostly 把这个读多写少的变量放到适合的缓存布局区域。
 */
int tick_do_timer_cpu __read_mostly = TICK_DO_TIMER_BOOT;
#ifdef CONFIG_NO_HZ_FULL
/*
 * tick_do_timer_boot_cpu indicates the boot CPU temporarily owns
 * tick_do_timer_cpu and it should be taken over by an eligible secondary
 * when one comes online.
 */
/*
 * tick_do_timer_boot_cpu 记录“启动 CPU 只是临时承担全局时间更新”的情况。
 * 若启动 CPU 属于 nohz_full，第一颗上线且可做 housekeeping 的次级 CPU
 * 会接管 tick_do_timer_cpu。-1 表示不存在待交接的启动 CPU；该变量只在
 * CONFIG_NO_HZ_FULL 下存在。
 */
static int tick_do_timer_boot_cpu __read_mostly = -1;
#endif

/*
 * Debugging: see timer_list.c
 */
/*
 * 调试接口，timer_list.c 等诊断代码可借它查看指定 CPU 的 tick_device。
 *
 * @cpu：纯输入逻辑 CPU 编号，调用者必须保证编号有效；函数不增加设备
 *       或模块引用，返回值不能超出相应 CPU/设备状态受保护的观察窗口。
 * 返回：指向静态 per-CPU tick_device 的借用指针，不会返回 NULL。
 * 上下文：只计算 per-CPU 地址，不加锁、不睡眠，也不修改任何状态。
 */
struct tick_device *tick_get_device(int cpu)
{
	return &per_cpu(tick_cpu_device, cpu);
}

/**
 * tick_is_oneshot_available - check for a oneshot capable event device
 */
/*
 * tick_is_oneshot_available() - 判断本 CPU 能否可靠使用 oneshot tick。
 *
 * 调用关系：NO_HZ/high-resolution tick 切换代码用它检查基础能力。
 * 入参：无。函数在当前 CPU 上读取 per-CPU evtdev；返回的 dev 是借用指针，
 *       调用者所处的 tick/clockevents 串行上下文保证其不会被并发替换。
 * 返回：1 表示本地设备可直接 oneshot，或设备会在深 idle 停止但存在可用
 *       的 oneshot broadcast；0 表示没有设备、没有 oneshot 能力，或缺少
 *       必需的 broadcast 后备。无状态副作用，不睡眠。
 */
int tick_is_oneshot_available(void)
{
	/*
	 * dev 只在本函数内有效。__this_cpu_read 明确读取当前 CPU 的 evtdev，
	 * 不取得对象引用；设备更换协议必须保证检查期间对象仍然存活。
	 */
	struct clock_event_device *dev = __this_cpu_read(tick_cpu_device.evtdev);

	/* 本地设备本身必须支持按到期点逐次编程。 */
	if (!dev || !(dev->features & CLOCK_EVT_FEAT_ONESHOT))
		return 0;
	/*
	 * 不带 C3STOP 的设备在深 idle 仍运行，可独立保证唤醒；带 C3STOP
	 * 则必须把到期事件委托给不会停摆的 broadcast 设备。
	 */
	if (!(dev->features & CLOCK_EVT_FEAT_C3STOP))
		return 1;
	return tick_broadcast_oneshot_available();
}

/*
 * Periodic tick
 */
/*
 * tick_periodic() - 执行一次周期 tick 的软件记账。
 *
 * 调用关系：由 tick_handle_periodic() 的硬件中断处理路径调用；oneshot
 * 补发循环也可能在一次中断中多次调用，以追赶已经错过的周期。
 * @cpu：纯输入当前逻辑 CPU 编号；必须与正在处理中断的 CPU 一致。
 * 入口：硬中断上下文，本地中断已关闭，不能睡眠；不持有 jiffies_lock。
 * 过程：若本 CPU 是全局时间负责人，则推进下一 tick 边界和 jiffies，
 *       再更新 wall time；无论是否负责全局时间，都更新当前 CPU/进程
 *       的用户态或内核态时间以及 profiling。
 * 返回：无直接返回值。副作用包括全局时间推进及本 CPU 统计更新；
 *       无失败
 *       返回，所有操作均须适合中断上下文。
 */
static void tick_periodic(int cpu)
{
	/*
	 * READ_ONCE 取得一次职责快照。职责可能因 NO_HZ/hotplug 交接；即使
	 * 短暂有两个 CPU 都认为自己负责，jiffies_lock 也会串行化真正更新。
	 */
	if (READ_ONCE(tick_do_timer_cpu) == cpu) {
		/*
		 * jiffies_lock 防止其他 CPU 同时推进全局 tick；jiffies_seq
		 * 让无锁读者检测写入区间。锁提供写者互斥，seqcount 提供读者
		 * 一致性，两者不可互相替代。
		 */
		raw_spin_lock(&jiffies_lock);
		write_seqcount_begin(&jiffies_seq);

		/* Keep track of the next tick event */
		/*
		 * 记录下一个 tick 事件：先按固定 TICK_NSEC 推进时间线，再由
		 * do_timer(1) 把 jiffies 增加一个周期，使二者保持同一节拍。
		 */
		tick_next_period = ktime_add_ns(tick_next_period, TICK_NSEC);

		do_timer(1);
		write_seqcount_end(&jiffies_seq);
		raw_spin_unlock(&jiffies_lock);
		/*
		 * update_wall_time() 放在 jiffies_lock 外，避免扩大这把全局
		 * 自旋锁的临界区；前面的 jiffies 推进已建立本次更新的基准。
		 */
		update_wall_time();
	}

	/*
	 * 每 CPU 进程记账不能只由全局 timekeeper 执行。irq_regs 保存被中断
	 * 现场，user_mode() 决定本 tick 计入用户态还是内核态；随后采集
	 * CPU profiling 样本。
	 */
	update_process_times(user_mode(get_irq_regs()));
	profile_tick(CPU_PROFILING);
}

/*
 * Event handler for periodic ticks
 */
/*
 * tick_handle_periodic() - 周期 tick 的 clockevent 回调。
 *
 * @dev：纯输入且非 NULL，指向当前 CPU 正在触发的 clock_event_device；
 *       tick 层借用该对象，不转移所有权。next_event 使用 ktime 纳秒表示。
 * 入口：硬中断上下文，不能睡眠；设备可能是真周期硬件，也可能是用
 *       CLOCK_EVT_STATE_ONESHOT 模拟周期的设备。
 * 过程：先处理当前 tick；若仍处于本回调且设备为 oneshot，则从旧的
 *       next_event 沿固定周期向前编程。过期事件会在循环中补做软件 tick，
 *       直到成功安排一个未来事件。
 * 返回：无直接返回值。若处理中切换到 HIGHRES/NOHZ，新 handler 接管，
 *       本函数不再触碰其编程状态。
 */
void tick_handle_periodic(struct clock_event_device *dev)
{
	/*
	 * cpu 标识本次记账归属；next 保存“本应到期”的时间线，而不是
	 * 每轮
	 * 重新读取当前时间，从而避免中断处理延迟令周期相位持续漂移。
	 */
	int cpu = smp_processor_id();
	ktime_t next = dev->next_event;

	/* 新一次真实中断到达，清除此前被迫使用最小 delta 的诊断状态。 */
	dev->next_event_forced = 0;
	tick_periodic(cpu);

	/*
	 * The cpu might have transitioned to HIGHRES or NOHZ mode via
	 * update_process_times() -> run_local_timers() ->
	 * hrtimer_run_queues().
	 */
	/*
	 * 当前 CPU 可能在上述调用链中切换到 HIGHRES 或 NOHZ，并把设备回调
	 * 换成新 handler。继续按周期模式编程会覆盖新模式的到期点，
	 * 所以发现
	 * handler 已改变就把控制权交给新模式。
	 */
	if (IS_ENABLED(CONFIG_TICK_ONESHOT) && dev->event_handler != tick_handle_periodic)
		return;

	/* 真正的硬件周期模式会自动产生下一中断，无需软件再次编程。 */
	if (!clockevent_state_oneshot(dev))
		return;
	for (;;) {
		/*
		 * Setup the next period for devices, which do not have
		 * periodic mode:
		 */
		/*
		 * 对没有硬件 periodic 模式的设备，以旧到期点增加 TICK_NSEC
		 * 来模拟严格周期；next 是循环内候选到期点。
		 */
		next = ktime_add_ns(next, TICK_NSEC);

		/*
		 * 返回 0 表示未来到期点已提交给硬件；非 0 表示该点通常已经
		 * 过期，需要继续推进时间线。force=false 不掩盖漏掉的周期。
		 */
		if (!clockevents_program_event(dev, next, false))
			return;
		/*
		 * Have to be careful here. If we're in oneshot mode,
		 * before we call tick_periodic() in a loop, we need
		 * to be sure we're using a real hardware clocksource.
		 * Otherwise we could get trapped in an infinite
		 * loop, as the tick_periodic() increments jiffies,
		 * which then will increment time, possibly causing
		 * the loop to trigger again and again.
		 */
		/*
		 * oneshot 补发只有在 timekeeping 使用真实、适合高分辨率的
		 * clocksource 时才可每轮调用 tick_periodic()。若时间由 jiffies
		 * 推导，tick_periodic() 增加 jiffies 又会令“当前时间”前进，
		 * 候选点可能永远显得过期，从而形成死循环。
		 */
		if (timekeeping_valid_for_hres())
			tick_periodic(cpu);
	}
}

/*
 * Setup the device for a periodic tick
 */
/*
 * tick_setup_periodic() - 把 clockevent 配置成提供周期 tick。
 *
 * @dev：输入的非 NULL clockevent 借用指针，必须已经归 tick/broadcast
 *       层选中；函数不接管其分配所有权。
 * @broadcast：非零表示为全局 broadcast 设备安装处理器，零表示本地设备；
 *             它影响 tick_set_periodic_handler() 选择的 event_handler。
 * 入口：设备管理串行区间，通常持 clockevents_lock 且中断关闭；不能睡眠。
 * 过程：先安装回调；dummy 设备只作 broadcast 占位，直接返回。真周期设备
 *       在 broadcast oneshot 未激活时进入硬件 PERIODIC；否则读取一致的
 *       tick_next_period，切到 ONESHOT 并越过已过期边界直至编程成功。
 * 返回：无直接返回值。可用设备返回时已能产生周期 tick。
 */
void tick_setup_periodic(struct clock_event_device *dev, int broadcast)
{
	/* 建立硬件中断到本地或 broadcast 周期处理器的分派关系。 */
	tick_set_periodic_handler(dev, broadcast);

	/* Broadcast setup ? */
	/*
	 * 是否只是 broadcast 占位设备？CLOCK_EVT_FEAT_DUMMY 表示没有可编程
	 * 硬件，handler 设置到这里即完成占位关系，不能继续切换设备状态。
	 */
	if (!tick_device_is_functional(dev))
		return;

	/*
	 * 首选硬件 PERIODIC：设备自行按固定间隔重装，开销最低。但 broadcast
	 * 已工作在 oneshot 时必须保持广播协议的单次到期语义。
	 */
	if ((dev->features & CLOCK_EVT_FEAT_PERIODIC) &&
	    !tick_broadcast_oneshot_active()) {
		clockevents_switch_state(dev, CLOCK_EVT_STATE_PERIODIC);
	} else {
		/*
		 * seq 是 jiffies_seq 版本戳；next 是对应版本的下一全局 tick
		 * 边界。32 位机器读取 ktime_t 可能撕裂，seqcount 重试也保证
		 * next 不落在 tick_periodic() 的写入中间。
		 */
		unsigned int seq;
		ktime_t next;

		do {
			seq = read_seqcount_begin(&jiffies_seq);
			next = tick_next_period;
		} while (read_seqcount_retry(&jiffies_seq, seq));

		/* 没有合适硬件周期模式时，用 oneshot 逐次模拟周期事件。 */
		clockevents_switch_state(dev, CLOCK_EVT_STATE_ONESHOT);

		for (;;) {
			/*
			 * 成功即完成发布；若 next 已在过去，向前跳一个
			 * 周期再试。
			 * 从全局 tick_next_period 起步使新设备与 jiffies 时间线
			 * 对齐，而不是从安装时刻重新建立一个漂移的周期。
			 */
			if (!clockevents_program_event(dev, next, false))
				return;
			next = ktime_add_ns(next, TICK_NSEC);
		}
	}
}

/*
 * Setup the tick device
 */
/*
 * tick_setup_device() - 把选中的 clockevent 发布为指定 CPU 的 tick 设备。
 *
 * 调用关系：tick_check_new_device() 或替换路径完成候选选择/模块引用后调用；
 * 返回后由 periodic 或 oneshot handler 接管硬件事件。
 * @td：目标 CPU 的 per-CPU tick_device，输入输出借用指针，非 NULL。
 * @newdev：已选中的新 clockevent，输入借用指针；成功后由 td->evtdev 指向。
 * @cpu：目标逻辑 CPU 编号，也是 timekeeping 职责和 broadcast 判断的身份。
 * @cpumask：希望设备中断绑定到的 CPU 集合，借用且在调用期间稳定。
 * 入口：clockevents 设备管理串行区间，本地中断关闭，不能睡眠。
 * 过程：首次安装时初始化全局 tick 负责人和模式；替换时保存旧 handler/
 *       next_event 并使旧设备静默；随后发布新指针、设置 IRQ affinity，
 *       最后与 broadcast 协议协调并恢复原来的 periodic/oneshot 模式。
 * 返回：无直接返回值。若新设备充当 broadcast placeholder，本地配置由
 *       broadcast 层保留；其余情况下新设备已被编程。无错误返回。
 */
static void tick_setup_device(struct tick_device *td,
			      struct clock_event_device *newdev, int cpu,
			      const struct cpumask *cpumask)
{
	/*
	 * handler/next_event 只在替换已有设备时保存，用于在新设备上延续
	 * oneshot 模式的回调和已承诺到期点；首次安装使用空值。
	 */
	void (*handler)(struct clock_event_device *) = NULL;
	ktime_t next_event = 0;

	/*
	 * First device setup ?
	 */
	/* td->evtdev 为空表示该 CPU 第一次获得可用本地 tick 设备。 */
	if (!td->evtdev) {
		/*
		 * If no cpu took the do_timer update, assign it to
		 * this cpu:
		 */
		/*
		 * 若启动哨兵仍在，当前 CPU 成为第一任 do_timer 负责人，并以
		 * 当前单调时间建立 tick_next_period 基准。WRITE_ONCE 与中断
		 * 路径 READ_ONCE 配对为单次可见访问，而非完整锁协议。
		 */
		if (READ_ONCE(tick_do_timer_cpu) == TICK_DO_TIMER_BOOT) {
			WRITE_ONCE(tick_do_timer_cpu, cpu);
			tick_next_period = ktime_get();
#ifdef CONFIG_NO_HZ_FULL
			/*
			 * The boot CPU may be nohz_full, in which case the
			 * first housekeeping secondary will take do_timer()
			 * from it.
			 */
			/*
			 * 启动 CPU 若属于 nohz_full，不适合长期承担 housekeeping；
			 * 记录其编号，等待第一颗非 nohz_full 次级 CPU 上线接管。
			 */
			if (tick_nohz_full_cpu(cpu))
				tick_do_timer_boot_cpu = cpu;

		} else if (tick_do_timer_boot_cpu != -1 && !tick_nohz_full_cpu(cpu)) {
			tick_do_timer_boot_cpu = -1;
			/*
			 * The boot CPU will stay in periodic (NOHZ disabled)
			 * mode until clocksource_done_booting() called after
			 * smp_init() selects a high resolution clocksource and
			 * timekeeping_notify() kicks the NOHZ stuff alive.
			 *
			 * So this WRITE_ONCE can only race with the READ_ONCE
			 * check in tick_periodic() but this race is harmless.
			 */
			/*
			 * 启动 CPU 会一直保留周期 tick，直到 smp_init() 后选择好
			 * 高分辨率 clocksource 并激活 NOHZ。因此这里只有与
			 * tick_periodic() 的一次 READ_ONCE 判断竞争；交接附近即使
			 * 某个周期由旧/新 CPU 执行，jiffies_lock 仍保证不会并发
			 * 破坏全局时间。
			 */
			WRITE_ONCE(tick_do_timer_cpu, cpu);
#endif
		}

		/*
		 * Startup in periodic mode first.
		 */
		/*
		 * 启动阶段先采用最普遍的 PERIODIC 语义；高分辨率/NOHZ 准备好
		 * 后再显式切换 ONESHOT，避免早期启动依赖尚不可用的设施。
		 */
		td->mode = TICKDEV_MODE_PERIODIC;
	} else {
		/*
		 * 替换设备时保留逻辑 handler 和下一到期点，使切换不丢事件；
		 * 先把旧设备 handler 置为 noop，防止迟到中断继续操作已退出
		 * 服务的设备。设备本体仍由 clockevents 交换协议管理。
		 */
		handler = td->evtdev->event_handler;
		next_event = td->evtdev->next_event;
		td->evtdev->event_handler = clockevents_handle_noop;
	}

	/*
	 * 发布 per-CPU 当前设备指针。此后本 CPU 的 tick 查询都会看到 newdev，
	 * 上层已确认其资格并取得模块引用；后续代码会在重新开中断前
	 * 完成
	 * affinity、handler 和到期点配置。
	 */
	td->evtdev = newdev;

	/*
	 * When the device is not per cpu, pin the interrupt to the
	 * current cpu:
	 */
	/*
	 * 非严格 per-CPU 的共享设备需要把 IRQ 亲和性收窄到当前目标集合，
	 * 保证硬件中断在拥有这份 per-CPU tick 状态的 CPU 上处理。
	 */
	if (!cpumask_equal(newdev->cpumask, cpumask))
		irq_set_affinity(newdev->irq, cpumask);

	/*
	 * When global broadcasting is active, check if the current
	 * device is registered as a placeholder for broadcast mode.
	 * This allows us to handle this x86 misfeature in a generic
	 * way. This function also returns !=0 when we keep the
	 * current active broadcast state for this CPU.
	 */
	/*
	 * broadcast 层可能把本地设备登记成占位符，或要求保留 CPU
	 * 当前已激活
	 * 的广播状态（兼容某些 x86 设备缺陷）。非零返回表示 broadcast 层
	 * 已接管配置，本函数不能再按普通本地设备重编程。
	 */
	if (tick_device_uses_broadcast(newdev, cpu))
		return;

	/*
	 * 按替换前保存的逻辑模式恢复服务，而不是按新硬件能力擅自
	 * 改语义。
	 */
	if (td->mode == TICKDEV_MODE_PERIODIC)
		tick_setup_periodic(newdev, 0);
	else
		tick_setup_oneshot(newdev, handler, next_event);
}

/*
 * tick_install_replacement() - 安装 clockevents 核心已选定的替代设备。
 *
 * @newdev：输入的非 NULL 新设备；调用者已取得其模块引用，成功后引用责任
 *          随设备安装关系转移给 clockevents/tick 生命周期。
 * 入口：当前 CPU 上，clockevents 替换串行区间内，不能睡眠。
 * 返回：无直接返回值。旧设备被交换回 detached/released 管理，新设备继承
 *       本 CPU 模式；若支持 oneshot，还通知 tick 层重新评估高分辨率能力。
 */
void tick_install_replacement(struct clock_event_device *newdev)
{
	/*
	 * td/cpu 均描述当前执行 CPU，调用者必须已把替换工作派到正确 CPU。
	 */
	struct tick_device *td = this_cpu_ptr(&tick_cpu_device);
	int cpu = smp_processor_id();

	clockevents_exchange_device(td->evtdev, newdev);
	tick_setup_device(td, newdev, cpu, cpumask_of(cpu));
	/* 新增 oneshot 能力可能使 NOHZ/highres 从周期模式升级。 */
	if (newdev->features & CLOCK_EVT_FEAT_ONESHOT)
		tick_oneshot_notify();
}

/*
 * tick_check_percpu() - 判断 newdev 能否安全地服务当前 CPU。
 *
 * @curdev：当前设备的可空借用指针；只用于比较亲和性和本地性。
 * @newdev：候选设备的非空借用指针。
 * @cpu：目标 CPU 编号。
 * 返回：true 表示候选覆盖该 CPU，且共享设备的 IRQ 可迁移，并且不会用
 *       共享设备替换现有严格 CPU-local 设备；false 表示不满足这些约束。
 * 无副作用、不睡眠；调用者仍需继续比较功能和 rating。
 */
static bool tick_check_percpu(struct clock_event_device *curdev,
			      struct clock_event_device *newdev, int cpu)
{
	/* 设备 cpumask 不覆盖目标 CPU，硬件中断不可能正确服务它。 */
	if (!cpumask_test_cpu(cpu, newdev->cpumask))
		return false;
	/* 严格 CPU-local 设备无需 IRQ 迁移，是最直接的适配。 */
	if (cpumask_equal(newdev->cpumask, cpumask_of(cpu)))
		return true;
	/* Check if irq affinity can be set */
	/*
	 * 对共享候选，必须能把有效 IRQ 的 affinity 设到目标 CPU；否则中断
	 * 可能落到没有对应 td 状态的 CPU。无 IRQ（负值）的抽象设备
	 * 不受此限。
	 */
	if (newdev->irq >= 0 && !irq_can_set_affinity(newdev->irq))
		return false;
	/* Prefer an existing cpu local device */
	/*
	 * 已有严格本地设备时不退化成共享设备，即使后者其他属性更高；
	 * 本地
	 * 设备没有跨 CPU affinity 和共享竞争成本。
	 */
	if (curdev && cpumask_equal(curdev->cpumask, cpumask_of(cpu)))
		return false;
	return true;
}

/*
 * tick_check_preferred() - 在适配 CPU 的设备之间比较功能与优先级。
 *
 * @curdev：当前最佳设备，可为 NULL；借用且不改变。
 * @newdev：待比较的非 NULL 候选；借用且不改变。
 * 返回：true 表示 newdev 在 oneshot 能力、rating 或 CPU-local 性方面值得
 *       替换 curdev；false 表示保留现状。函数无副作用、不睡眠。
 */
static bool tick_check_preferred(struct clock_event_device *curdev,
				 struct clock_event_device *newdev)
{
	/* Prefer oneshot capable device */
	/*
	 * 优先具备 oneshot 的设备。候选若不支持 oneshot，不能替换已有
	 * oneshot 设备；系统已经进入 oneshot 模式后更不能退化，因为 NOHZ/
	 * highres 已依赖逐次编程语义。
	 */
	if (!(newdev->features & CLOCK_EVT_FEAT_ONESHOT)) {
		if (curdev && (curdev->features & CLOCK_EVT_FEAT_ONESHOT))
			return false;
		if (tick_oneshot_mode_active())
			return false;
	}

	/*
	 * Use the higher rated one, but prefer a CPU local device with a lower
	 * rating than a non-CPU local device
	 */
	/*
	 * 一般选择更高 rating；但 CPU-local 是独立维度：只要 mask 不同，
	 * 候选仍可胜出，从而允许较低 rating 的本地设备替换共享设备。
	 * 若 mask 相同且 rating 不高，则没有替换收益。
	 */
	return !curdev ||
		newdev->rating > curdev->rating ||
	       !cpumask_equal(curdev->cpumask, newdev->cpumask);
}

/*
 * Check whether the new device is a better fit than curdev. curdev
 * can be NULL !
 */
/*
 * tick_check_replacement() - 判断 newdev 是否是当前 CPU 更合适的替代品。
 *
 * @curdev：当前设备，可为 NULL；借用，不转移所有权。
 * @newdev：候选设备，非 NULL；借用。
 * 返回：只有候选先通过 CPU 覆盖/IRQ affinity/本地性约束，再通过功能与
 *       rating 比较时才为 true。无副作用、无错误码、不睡眠。
 */
bool tick_check_replacement(struct clock_event_device *curdev,
			    struct clock_event_device *newdev)
{
	/* 先排除根本无法安全投递到本 CPU 的候选，再做质量排序。 */
	if (!tick_check_percpu(curdev, newdev, smp_processor_id()))
		return false;

	return tick_check_preferred(curdev, newdev);
}

/*
 * Check, if the new registered device should be used. Called with
 * clockevents_lock held and interrupts disabled.
 */
/*
 * tick_check_new_device() - 尝试把新注册设备安装为当前 CPU 本地 tick。
 *
 * @newdev：新注册的非 NULL clockevent 借用指针；若被选中，函数取得模块
 *          引用并把设备交给本地 tick 生命周期；若未选中，仍可尝试安装
 *          为 broadcast 设备。
 * 入口：持有 clockevents_lock 且本地中断关闭，当前 CPU 身份稳定；不能
 *       睡眠。该锁串行设备列表、状态交换和 td->evtdev 更新。
 * 返回：无直接返回值。成功本地替换后 newdev 已发布并配置；
 *       模块引用失败
 *       时完全不改变设备关系；不适合作本地设备时交给 broadcast 选择器。
 */
void tick_check_new_device(struct clock_event_device *newdev)
{
	/*
	 * curdev/td 都是当前 CPU 的借用对象；cpu 在关中断区间不会迁移。
	 */
	struct clock_event_device *curdev;
	struct tick_device *td;
	int cpu;

	cpu = smp_processor_id();
	td = &per_cpu(tick_cpu_device, cpu);
	curdev = td->evtdev;

	if (!tick_check_replacement(curdev, newdev))
		goto out_bc;

	/*
	 * 在发布设备前固定其 owner 模块。失败表示模块正在卸载，不能保存
	 * newdev 指针；此时直接返回，既不替换本地设备也不尝试 broadcast。
	 */
	if (!try_module_get(newdev->owner))
		return;

	/*
	 * Replace the eventually existing device by the new
	 * device. If the current device is the broadcast device, do
	 * not give it back to the clockevents layer !
	 */
	/*
	 * 用新设备替换可能存在的旧设备。若旧设备同时是 broadcast 设备，
	 * 它仍归 broadcast 层所有，不能作为普通 released 设备交还列表；
	 * 这里只关闭硬件并把本地 curdev 视为空。
	 */
	if (tick_is_broadcast_device(curdev)) {
		clockevents_shutdown(curdev);
		curdev = NULL;
	}
	clockevents_exchange_device(curdev, newdev);
	tick_setup_device(td, newdev, cpu, cpumask_of(cpu));
	/* 新 oneshot 能力可能解除此前 highres/NOHZ 的能力阻塞。 */
	if (newdev->features & CLOCK_EVT_FEAT_ONESHOT)
		tick_oneshot_notify();
	return;

out_bc:
	/*
	 * Can the new device be used as a broadcast device ?
	 */
	/*
	 * 候选不适合本地 tick 时仍可能覆盖多个 CPU、且在深 idle
	 * 中持续运行，
	 * 因而交给 broadcast 层评估。该调用不保证一定安装。
	 */
	tick_install_broadcast_device(newdev, cpu);
}

/**
 * tick_broadcast_oneshot_control - Enter/exit broadcast oneshot mode
 * @state:	The target state (enter/exit)
 *
 * The system enters/leaves a state, where affected devices might stop
 * Returns 0 on success, -EBUSY if the cpu is used to broadcast wakeups.
 *
 * Called with interrupts disabled, so clockevents_lock is not
 * required here because the local clock event device cannot go away
 * under us.
 */
/*
 * tick_broadcast_oneshot_control() - 进入或退出本 CPU 的 oneshot 广播模式。
 *
 * @state：纯输入目标状态，TICK_BROADCAST_ENTER 或 TICK_BROADCAST_EXIT。
 * 调用关系：CPU idle 路径在本地设备可能随深 idle 停止时调用；broadcast
 *           层据此代为安排唤醒或恢复本地事件。
 * 入口：本地中断关闭，当前 CPU 稳定；因此本地 per-CPU evtdev 不会在
 *       检查中消失，无需 clockevents_lock。不能睡眠。
 * 返回：0 表示无需广播或状态切换成功；-EBUSY 表示该 CPU 正承担 broadcast
 *       唤醒职责，当前不能进入目标状态。没有 ownership 转移。
 */
int tick_broadcast_oneshot_control(enum tick_broadcast_state state)
{
	/* td/evtdev 为当前 CPU 的借用对象，关中断是其生命周期保护条件。 */
	struct tick_device *td = this_cpu_ptr(&tick_cpu_device);

	/* 不会在深 idle 停止的本地设备不需要 broadcast 代偿。 */
	if (!(td->evtdev->features & CLOCK_EVT_FEAT_C3STOP))
		return 0;

	return __tick_broadcast_oneshot_control(state);
}
EXPORT_SYMBOL_GPL(tick_broadcast_oneshot_control);

#ifdef CONFIG_HOTPLUG_CPU
/*
 * tick_assert_timekeeping_handover() - 断言下线 CPU 已交出全局计时职责。
 *
 * 入参：无。入口为 CPU hotplug teardown 的本 CPU 上下文。
 * 返回：无直接返回值；若 tick_do_timer_cpu 仍指向当前 CPU，仅告警一次，
 *       用于暴露 hotplug 调用顺序错误，不在此处尝试修复或交接。
 * 不获取锁、不睡眠；hotplug 状态机提供 CPU 集合的串行性。
 */
void tick_assert_timekeeping_handover(void)
{
	WARN_ON_ONCE(tick_do_timer_cpu == smp_processor_id());
}
/*
 * Stop the tick and transfer the timekeeping job away from a dying cpu.
 */
/*
 * tick_cpu_dying() - 停止 dying CPU 的 tick 并移交全局时间更新职责。
 *
 * @dying_cpu：正在执行 teardown 的逻辑 CPU 编号，纯输入；调用发生在该
 *             CPU 上，且 stop-machine 阶段保证其余在线 CPU 不处于 idle。
 * 调用关系：CPU hotplug 下线状态机调用；之后 tick_shutdown() 解除设备。
 * 入口：hotplug 串行区间，不能睡眠；无需普通运行期锁。
 * 返回：始终为 0。副作用是可能改写 tick_do_timer_cpu、阻止该 CPU 再接管
 *       tick-sched 职责，并把 CPU 从 broadcast 掩码摘除。
 */
int tick_cpu_dying(unsigned int dying_cpu)
{
	/*
	 * If the current CPU is the timekeeper, it's the only one that can
	 * safely hand over its duty. Also all online CPUs are in stop
	 * machine, guaranteed not to be idle, therefore there is no
	 * concurrency and it's safe to pick any online successor.
	 */
	/*
	 * 只有当前负责人自己能在停止服务前安全交接。此时所有在线 CPU
	 * 都在
	 * stop-machine 中且保证不 idle，不存在 NO_HZ 同时放弃/争抢职责的
	 * 运行期并发，所以任选 cpu_online_mask 中第一颗在线后继即可。
	 */
	if (tick_do_timer_cpu == dying_cpu)
		tick_do_timer_cpu = cpumask_first(cpu_online_mask);

	/* Make sure the CPU won't try to retake the timekeeping duty */
	/*
	 * 通知 tick-sched 销毁该 CPU 的调度 tick timer，防止 teardown 后它
	 * 通过 NO_HZ 协议重新领取 timekeeping 职责。
	 */
	tick_sched_timer_dying(dying_cpu);

	/* Remove CPU from timer broadcasting */
	/* 从 broadcast 管理中摘除 dying CPU，之后不再为它安排代理唤醒。 */
	tick_offline_cpu(dying_cpu);

	return 0;
}

/*
 * Shutdown an event device on the outgoing CPU:
 *
 * Called by the dying CPU during teardown, with clockevents_lock held
 * and interrupts disabled.
 */
/*
 * tick_shutdown() - 在 outgoing CPU 上解除本地 tick 设备。
 *
 * 入参：无。调用关系：CPU teardown 在 tick_cpu_dying() 完成交接后调用。
 * 入口：由 dying CPU 执行，持有 clockevents_lock 且中断关闭；不能睡眠。
 * 过程：把逻辑模式复位为 PERIODIC；若有设备，则通过 exchange 协议交还
 *       clockevents 层，把迟到中断的 handler 改为 noop，再清空 per-CPU
 *       指针。这一顺序保证 td 不再发布一个已交还设备。
 * 返回：无直接返回值。退出时 td->evtdev == NULL，可供以后 CPU online
 *       按首次安装路径重新初始化；设备对象内存仍由 clockevents/驱动持有。
 */
void tick_shutdown(void)
{
	/* td 是当前 outgoing CPU 的静态对象；dev 是调用期间的借用快照。 */
	struct tick_device *td = this_cpu_ptr(&tick_cpu_device);
	struct clock_event_device *dev = td->evtdev;

	/* 下次上线从保守的 PERIODIC 模式重新建立状态。 */
	td->mode = TICKDEV_MODE_PERIODIC;
	if (dev) {
		/*
		 * exchange 处理设备状态/模块引用关系；noop 吸收可能已在途
		 * 但不应
		 * 再做 tick 记账的中断，最后清空对外可见的 per-CPU 关联。
		 */
		clockevents_exchange_device(dev, NULL);
		dev->event_handler = clockevents_handle_noop;
		td->evtdev = NULL;
	}
}
#endif

/**
 * tick_suspend_local - Suspend the local tick device
 *
 * Called from the local cpu for freeze with interrupts disabled.
 *
 * No locks required. Nothing can change the per cpu device.
 */
/*
 * tick_suspend_local() - 冻结当前 CPU 的本地 tick 硬件。
 *
 * 入参：无。由本 CPU 在 freeze 路径且中断关闭时调用；该执行条件保证
 * per-CPU evtdev 不会被替换，故无需设备锁。函数不能睡眠。
 * 返回：无直接返回值；clockevents_shutdown() 令设备停止产生事件，但保留
 * td->evtdev 关联、模式和所有权，以便 resume 原样恢复。
 */
void tick_suspend_local(void)
{
	/* td 为当前 CPU 静态状态，evtdev 只借用且在关中断区间稳定。 */
	struct tick_device *td = this_cpu_ptr(&tick_cpu_device);

	clockevents_shutdown(td->evtdev);
}

/**
 * tick_resume_local - Resume the local tick device
 *
 * Called from the local CPU for unfreeze or XEN resume magic.
 *
 * No locks required. Nothing can change the per cpu device.
 */
/*
 * tick_resume_local() - 恢复当前 CPU 的本地 tick 并重建到期编程。
 *
 * 入参：无。由本 CPU 在 unfreeze 或 Xen 特殊 resume 路径调用；调用环境
 * 保证 per-CPU 设备不变，故无需锁且不能睡眠。
 * 过程：先询问 broadcast 层本 CPU 是否仍由代理服务，再恢复 clockevent；
 *       若无需代理，按 suspend 前的 PERIODIC/ONESHOT 模式重新设置；
 *       最后恢复本地 hrtimer 时间线并校正硬件下一事件。
 * 返回：无直接返回值。退出时本地 tick 或 broadcast 至少一方负责唤醒，
 *       hrtimer 队列与恢复后的时间基准一致。
 */
void tick_resume_local(void)
{
	/*
	 * broadcast 表示代理设备将继续负责当前 CPU；为 true 时不能同时
	 * 重编本地设备，否则可能产生重复事件或覆盖代理协议。
	 */
	struct tick_device *td = this_cpu_ptr(&tick_cpu_device);
	bool broadcast = tick_resume_check_broadcast();

	clockevents_tick_resume(td->evtdev);
	if (!broadcast) {
		/* mode 是 suspend 前保留下来的逻辑模式。 */
		if (td->mode == TICKDEV_MODE_PERIODIC)
			tick_setup_periodic(td->evtdev, 0);
		else
			tick_resume_oneshot();
	}

	/*
	 * Ensure that hrtimers are up to date and the clockevents device
	 * is reprogrammed correctly when high resolution timers are
	 * enabled.
	 */
	/*
	 * 恢复 hrtimer 的基准和队列；启用高分辨率 timer 时，该 helper 还会
	 * 按恢复后的时间重新编程 clockevent，避免沿用 suspend 前的过期值。
	 */
	hrtimers_resume_local();
}

/**
 * tick_suspend - Suspend the tick and the broadcast device
 *
 * Called from syscore_suspend() via timekeeping_suspend with only one
 * CPU online and interrupts disabled or from tick_unfreeze() under
 * tick_freeze_lock.
 *
 * No locks required. Nothing can change the per cpu device.
 */
/*
 * tick_suspend() - 按“本地设备、broadcast 设备”顺序挂起 tick 子系统。
 *
 * 入参：无。由 syscore_suspend()->timekeeping_suspend() 在仅一颗 CPU
 * 在线且中断关闭时调用，或由持 tick_freeze_lock 的 tick_unfreeze()
 * 协议调用；这些条件排除设备并发更换，函数不能睡眠。
 * 返回：无直接返回值；本地与 broadcast clockevent 都停止，关联仍保留。
 */
void tick_suspend(void)
{
	/* 先阻止本地事件，再关闭为其他 CPU 服务的 broadcast 事件。 */
	tick_suspend_local();
	tick_suspend_broadcast();
}

/**
 * tick_resume - Resume the tick and the broadcast device
 *
 * Called from syscore_resume() via timekeeping_resume with only one
 * CPU online and interrupts disabled.
 *
 * No locks required. Nothing can change the per cpu device.
 */
/*
 * tick_resume() - 恢复 broadcast 与本地 tick 设备。
 *
 * 入参：无。由 syscore_resume()->timekeeping_resume() 在唯一在线 CPU、
 * 中断关闭的恢复早期调用；无需额外锁且不能睡眠。
 * 返回：无直接返回值。先恢复 broadcast 全局代理状态，再让本地恢复逻辑
 * 据此决定是否编程本地设备，避免错误地重复承担同一到期事件。
 */
void tick_resume(void)
{
	/* 顺序与 suspend 不简单镜像：local resume 需要先看到 broadcast 决策。 */
	tick_resume_broadcast();
	tick_resume_local();
}

#ifdef CONFIG_SUSPEND
/*
 * suspend freeze 协议的全局状态：
 * @tick_freeze_lock：串行各在线 CPU 的 freeze/unfreeze 到达顺序，并保护
 *                    tick_freeze_depth 及“最后/第一 CPU”状态转换。
 * @tick_freeze_map：只供 lockdep 描述特殊阶段；它不是实际互斥锁。此时
 *                   系统已无并发，允许 PREEMPT_RT 上取得通常可睡眠的锁。
 * @tick_freeze_depth：已经进入 freeze 且尚未 unfreeze 的在线 CPU 数。
 *                     0 到 num_online_cpus() 的计数构成全局屏障。
 * 三者只在 CONFIG_SUSPEND 下存在，生命周期贯穿内核运行期。
 */
static DEFINE_RAW_SPINLOCK(tick_freeze_lock);
static DEFINE_WAIT_OVERRIDE_MAP(tick_freeze_map, LD_WAIT_SLEEP);
static unsigned int tick_freeze_depth;

/**
 * tick_freeze - Suspend the local tick and (possibly) timekeeping.
 *
 * Check if this is the last online CPU executing the function and if so,
 * suspend timekeeping.  Otherwise suspend the local tick.
 *
 * Call with interrupts disabled.  Must be balanced with %tick_unfreeze().
 * Interrupts must not be enabled before the subsequent %tick_unfreeze().
 */
/*
 * tick_freeze() - 冻结本 CPU tick；最后到达者同时冻结全局 timekeeping。
 *
 * 入参：无。每颗在线 CPU 在 suspend-to-idle 屏障中各调用一次。
 * 入口：本地中断关闭；必须与后续 tick_unfreeze() 配对，且两者之间不可
 *       重新开中断。函数取得 tick_freeze_lock，不在持锁期间正常调度。
 * 过程：递增到达深度；非最后 CPU 只停本地 tick 并留在 idle，最后 CPU
 *       在系统已无并发时把 system_state 切到 SUSPEND，依次停 sched_clock
 *       与 timekeeping。
 * 返回：无直接返回值。退出时本 CPU tick 已停；若它是最后到达者，全局
 *       timekeeping 也已停。错误的调用配对会破坏深度不变量，没有 errno。
 */
void tick_freeze(void)
{
	/*
	 * raw spinlock 在 PREEMPT_RT 上仍保持真正自旋语义，保护跨 CPU 到达
	 * 计数和最后到达者判定；关中断避免本 CPU tick 路径重入。
	 */
	raw_spin_lock(&tick_freeze_lock);

	/* 加一后等于在线 CPU 数，当前 CPU 就是冻结屏障的最后到达者。 */
	tick_freeze_depth++;
	if (tick_freeze_depth == num_online_cpus()) {
		/* tracepoint 标记全局 timekeeping freeze 的可观察边界。 */
		trace_suspend_resume(TPS("timekeeping_freeze"),
				     smp_processor_id(), true);
		/*
		 * All other CPUs have their interrupts disabled and are
		 * suspended to idle. Other tasks have been frozen so there
		 * is no scheduling happening. This means that there is no
		 * concurrency in the system at this point. Therefore it is
		 * okay to acquire a sleeping lock on PREEMPT_RT, such as a
		 * spinlock, because the lock cannot be held by other CPUs
		 * or threads and acquiring it cannot block.
		 *
		 * Inform lockdep about the situation.
		 */
		/*
		 * 其他 CPU 已关中断并停在 idle，其他任务也已冻结，
		 * 系统此刻没有
		 * 调度并发。因此 PREEMPT_RT 上 timekeeping 内部即使取得可睡眠
		 * 的 spinlock 也不会阻塞。tick_freeze_map 仅把这一事实告诉
		 * lockdep，避免其按普通运行期锁规则误报；不提供实际排他。
		 */
		lock_map_acquire_try(&tick_freeze_map);
		/*
		 * 先发布 SYSTEM_SUSPEND，再停 sched_clock 和 timekeeping，
		 * 让被调子系统按 suspend 语义处理；此处是全局计时停止点。
		 */
		system_state = SYSTEM_SUSPEND;
		sched_clock_suspend();
		timekeeping_suspend();
		lock_map_release(&tick_freeze_map);
	} else {
		/*
		 * 尚有 CPU 未到屏障，timekeeping 仍须运行；当前 CPU 只关闭自己
		 * 的 clockevent，之后进入 idle 等待最后到达者完成全局冻结。
		 */
		tick_suspend_local();
	}

	raw_spin_unlock(&tick_freeze_lock);
}

/**
 * tick_unfreeze - Resume the local tick and (possibly) timekeeping.
 *
 * Check if this is the first CPU executing the function and if so, resume
 * timekeeping.  Otherwise resume the local tick.
 *
 * Call with interrupts disabled.  Must be balanced with %tick_freeze().
 * Interrupts must not be enabled after the preceding %tick_freeze().
 */
/*
 * tick_unfreeze() - 恢复本 CPU tick；第一位离开者先恢复全局 timekeeping。
 *
 * 入参：无。与本 CPU 之前的 tick_freeze() 一一配对。
 * 入口：中断从 freeze 前至今一直关闭；取得 tick_freeze_lock，不能正常
 *       睡眠。tick_freeze_depth 至少为 1。
 * 过程：若深度仍等于在线 CPU 数，当前 CPU 是第一个恢复者，先恢复
 *       timekeeping/sched_clock，再发布 SYSTEM_RUNNING；其余 CPU 只恢复
 *       本地 tick。最后统一递减深度。
 * 返回：无直接返回值。退出时本 CPU 已具备计时事件来源；
 *       第一恢复者还保证
 *       后续 CPU 观察到运行中的全局 timekeeping。
 */
void tick_unfreeze(void)
{
	/*
	 * 与 freeze 使用同一 raw lock，使“第一恢复者”判定和深度递减
	 * 原子化。
	 */
	raw_spin_lock(&tick_freeze_lock);

	/* 深度尚为满值表示没有其他 CPU 先离开冻结屏障。 */
	if (tick_freeze_depth == num_online_cpus()) {
		/*
		 * Similar to tick_freeze(). On resumption the first CPU may
		 * acquire uncontended sleeping locks while other CPUs block on
		 * tick_freeze_lock.
		 */
		/*
		 * 与 tick_freeze() 同理：其他 CPU 仍阻塞在 freeze lock 上，当前
		 * CPU 可无竞争地取得 PREEMPT_RT 上可能睡眠的内部锁。该 map 仅
		 * 修正 lockdep 对特殊无并发阶段的模型。
		 */
		lock_map_acquire_try(&tick_freeze_map);
		/*
		 * 先恢复 timekeeping，再恢复依赖其基准的 sched_clock；完成后
		 * 才发布 SYSTEM_RUNNING 和 resume tracepoint，避免观察者看到
		 * RUNNING 却读到尚未恢复的时间基准。
		 */
		timekeeping_resume();
		sched_clock_resume();
		lock_map_release(&tick_freeze_map);

		system_state = SYSTEM_RUNNING;
		trace_suspend_resume(TPS("timekeeping_freeze"),
				     smp_processor_id(), false);
	} else {
		/*
		 * 全局时间已由第一恢复者启动。本 CPU 更新 softlockup watchdog
		 * 的触碰时间，避免长时间 freeze 被误判为卡死，然后恢复
		 * 本地 tick。
		 */
		touch_softlockup_watchdog();
		tick_resume_local();
	}

	/* 当前 CPU 离开屏障；与 freeze 的递增严格一一配对。 */
	tick_freeze_depth--;

	raw_spin_unlock(&tick_freeze_lock);
}
#endif /* CONFIG_SUSPEND */
/*
 * 上述 freeze 深度屏障和全局 timekeeping 冻结逻辑仅在
 * CONFIG_SUSPEND 启用时编译；关闭配置时公共头文件提供空操作入口。
 */

/**
 * tick_init - initialize the tick control
 */
/*
 * tick_init() - 初始化通用 tick 控制的两个可选子系统。
 *
 * 入参：无。由时间子系统启动路径调用一次；__init 表示启动完成后函数
 *       代码可被释放。此时尚无并发运行期设备切换，初始化过程可以建立
 *       全局状态。
 * 过程：先初始化 broadcast 基础设施，再初始化 NO_HZ；NO_HZ 依赖前者在
 *       本地设备深 idle 停摆时提供后备唤醒。
 * 返回：无直接返回值；两个 helper 在禁用对应配置时可能是空 stub，无错误
 *       返回和 ownership 输出。
 */
void __init tick_init(void)
{
	/* 初始化顺序建立 broadcast 可用后再开放 NO_HZ 停 tick 决策。 */
	tick_broadcast_init();
	tick_nohz_init();
}
