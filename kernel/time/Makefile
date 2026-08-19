# SPDX-License-Identifier: GPL-2.0

# Branch profiling isn't noinstr-safe
# 分支插桩会在函数入口或分支处加入额外调用，而 sched_clock 的底层读时钟路径可能运行在
# noinstr/NMI 等禁止普通插桩的上下文；配置启用时仅对该对象关闭分支 profiling，避免递归读时钟
# 或进入不可重入的追踪代码。这里改变的是 sched_clock.o 的编译参数，不影响目录中其他对象。
ifdef CONFIG_TRACE_BRANCH_PROFILING
CFLAGS_sched_clock.o += -DDISABLE_BRANCH_PROFILING
endif

# 无条件核心对象：time 提供系统调用和时间格式转换入口，timer/hrtimer/sleep_timeout 分别承载
# jiffy 时间轮、高精度红黑树以及 schedule_timeout 等睡眠适配层。它们始终编入 built-in.a，
# 最终随 vmlinux 链接，不以可卸载模块存在。
obj-y += time.o timer.o hrtimer.o sleep_timeout.o

# 时间维护主链：timekeeping 把 clocksource 周期换成各时间域，ntp 调整换算频率，clocksource
# 管理硬件计数器，jiffies 提供最低保真后备；timer_list 是调试导出。timeconv/timecounter 完成
# 纯换算与外部 cyclecounter 累积，alarmtimer 把可唤醒 RTC 能力接到定时器接口。
obj-y += timekeeping.o ntp.o clocksource.o jiffies.o timer_list.o
obj-y += timeconv.o timecounter.o alarmtimer.o

# POSIX timers 开启时链接完整的墙钟/CPU 时钟、interval timer 和动态时钟实现；关闭时只链接
# posix-stubs.o，保留系统调用 ABI 并返回不支持。两侧互斥可防止同一入口出现重复符号。
ifeq ($(CONFIG_POSIX_TIMERS),y)
 obj-y += posix-timers.o posix-cpu-timers.o posix-clock.o itimer.o
else
 obj-y += posix-stubs.o
endif

# 通用 clockevents 框架启用后，clockevents.o 管理设备状态机，tick-common.o 选择设备并提供
# 周期 tick。使用 obj-$(CONFIG_...) 让 Kconfig 的 y 选择落入 obj-y；相关布尔配置不会生成模块。
obj-$(CONFIG_GENERIC_CLOCKEVENTS)		+= clockevents.o tick-common.o

# 广播 tick 只在架构声明通用广播能力时进入构建；其中 hrtimer 模拟广播设备还依赖 oneshot，
# 因为它通过“编程下一次到期”而非固定周期中断驱动离线本地 tick 的 CPU。
ifeq ($(CONFIG_GENERIC_CLOCKEVENTS_BROADCAST),y)
 obj-y						+= tick-broadcast.o
 obj-$(CONFIG_TICK_ONESHOT)			+= tick-broadcast-hrtimer.o
endif

# sched_clock 是调度器使用的快速局部时间轴；oneshot 同时带入设备切换层和 NO_HZ 调度逻辑；
# 不具备 oneshot 的平台则可选择传统周期设备入口。Kconfig 约束保证这些实现与平台能力匹配。
obj-$(CONFIG_GENERIC_SCHED_CLOCK)		+= sched_clock.o
obj-$(CONFIG_TICK_ONESHOT)			+= tick-oneshot.o tick-sched.o
obj-$(CONFIG_LEGACY_TIMER_TICK)			+= tick-legacy.o

# timer migration 需要跨 CPU 建层次并代替空闲 CPU 处理定时器，因此同时要求 SMP 与 NO_HZ_COMMON；
# 外层 ifeq 避免单处理器构建即使出现异常配置组合也编译无意义的迁移实现。
ifeq ($(CONFIG_SMP),y)
 obj-$(CONFIG_NO_HZ_COMMON)			+= timer_migration.o
endif

# 其余对象按单项能力加入：通用 gettimeofday/vDSO 后端、debugfs 观测、udelay 自测、时间命名空间
# 核心与 vDSO 页、clocksource watchdog 故障注入测试，以及 KUnit 的时间转换测试。测试对象仅在
# 对应测试配置打开时进入内核，不属于生产默认路径。
obj-$(CONFIG_GENERIC_GETTIMEOFDAY)		+= vsyscall.o
obj-$(CONFIG_DEBUG_FS)				+= timekeeping_debug.o
obj-$(CONFIG_TEST_UDELAY)			+= test_udelay.o
obj-$(CONFIG_TIME_NS)				+= namespace.o
obj-$(CONFIG_TIME_NS_VDSO)			+= namespace_vdso.o
obj-$(CONFIG_TEST_CLOCKSOURCE_WATCHDOG)		+= clocksource-wdtest.o
obj-$(CONFIG_TIME_KUNIT_TEST)			+= time_test.o
