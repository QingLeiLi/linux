# SPDX-License-Identifier: GPL-2.0
# Any varying coverage in these files is non-deterministic
# and is generally not a function of system call inputs.
# 锁路径的执行次数主要由 CPU 竞争、抢占和中断时序决定，而不是由系统调用参数稳定决定；若让
# KCOV 记录本目录，模糊测试器会把调度噪声误当成可复现的新覆盖，还会给最热的同步路径增加扰动。
KCOV_INSTRUMENT		:= n

# Clang 上下文分析只对已适配其线程安全属性的翻译单元开启。变量后缀是最终目标文件名；值 y
# 使 scripts/Makefile.lib 为这些对象加入 CFLAGS_CONTEXT_ANALYSIS，用静态检查补充运行时 lockdep。
# rtmutex.c 和 rwbase_rt.c 并不独立编译，它们由下列 API/RT 包装文件以不同构建宏文本包含，故开关
# 应写在实际翻译单元上。这里启用分析不会改变运行时代码的锁语义。
CONTEXT_ANALYSIS_mutex.o := y
CONTEXT_ANALYSIS_rtmutex_api.o := y
CONTEXT_ANALYSIS_ww_rt_mutex.o := y
CONTEXT_ANALYSIS_rwsem.o := y

# 这四个对象是所有配置下的睡眠同步基础设施。具体实现内部再用 CONFIG_* 选择 UP/SMP、普通内核
# 或 PREEMPT_RT 路径；obj-y 表示直接链接进 vmlinux，而不是可选模块。
obj-y += mutex.o semaphore.o rwsem.o percpu-rwsem.o

# Avoid recursion lockdep -> sanitizer -> ... -> lockdep & improve performance.
# lockdep 自己维护图锁并拦截其他锁操作；若 KASAN/KCSAN 对 lockdep.o 插桩，插桩运行时可能再次
# 获取或报告锁，形成“lockdep → sanitizer → lockdep”的递归。仅对该对象关闭两种 sanitizer，
# 既切断递归并降低验证器热路径开销，也不代表关闭全内核的内存或数据竞争检测。
KASAN_SANITIZE_lockdep.o := n
KCSAN_SANITIZE_lockdep.o := n

ifdef CONFIG_FUNCTION_TRACER
# CONFIG_FUNCTION_TRACER 打开时，普通对象会获得 -pg/-mfentry 等函数入口插桩。三个 lockdep/锁调试
# 对象位于跟踪基础设施可能依赖的路径上，故从各自编译标志中移除 CC_FLAGS_FTRACE，避免跟踪回调
# 再进入锁验证和调试代码。ifdef 只判断变量是否已定义；这些赋值只影响列出的目标文件。
CFLAGS_REMOVE_lockdep.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_lockdep_proc.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_mutex-debug.o = $(CC_FLAGS_FTRACE)
endif

# DEBUG_IRQFLAGS 提供中断开关配对检查；DEBUG_MUTEXES 叠加 mutex 慢路径一致性检查；LOCKDEP 构建
# 运行时依赖图验证器。obj-$(CONFIG_*) 在 y 时链接进内核、在 m 时生成模块、未启用时不选中对象。
obj-$(CONFIG_DEBUG_IRQFLAGS) += irqflag-debug.o
obj-$(CONFIG_DEBUG_MUTEXES) += mutex-debug.o
obj-$(CONFIG_LOCKDEP) += lockdep.o
ifeq ($(CONFIG_PROC_FS),y)
# /proc/lockdep、/proc/lockdep_stats 等接口依赖 procfs。本层先要求 PROC_FS=y，再沿用 LOCKDEP 选择；
# 明确比较 y 可防止把只适用于内建 procfs 的接口当成独立模块组合。
obj-$(CONFIG_LOCKDEP) += lockdep_proc.o
endif
# SMP 需要通用自旋锁 API 的慢路径/导出实现；PROVE_LOCKING 或 DEBUG_SPINLOCK 即使在其他组合下也
# 需要同一对象。Kbuild 对 obj-* 列表中的重复目标去重，所以多项同时为 y 不会把 spinlock.o 链接多次。
obj-$(CONFIG_SMP) += spinlock.o
# mutex/rwsem 的 owner 乐观自旋共用 osq_lock.o；只有架构和 SMP 条件使 LOCK_SPIN_ON_OWNER 成立时
# 才需要排队节点实现。QUEUED_SPINLOCKS/QUEUED_RWLOCKS 则选择对应架构无关排队锁算法。
obj-$(CONFIG_LOCK_SPIN_ON_OWNER) += osq_lock.o
obj-$(CONFIG_PROVE_LOCKING) += spinlock.o
obj-$(CONFIG_QUEUED_SPINLOCKS) += qspinlock.o
# rtmutex_api.o 通过 #include "rtmutex.c" 生成普通 rt_mutex API；PREEMPT_RT 的 spinlock_rt.o 和
# ww_rt_mutex.o 也用不同宏包含同一核心源码，把可睡眠且支持优先级继承的实现包装成 RT 锁语义。
obj-$(CONFIG_RT_MUTEXES) += rtmutex_api.o
obj-$(CONFIG_PREEMPT_RT) += spinlock_rt.o ww_rt_mutex.o
obj-$(CONFIG_DEBUG_SPINLOCK) += spinlock.o
obj-$(CONFIG_DEBUG_SPINLOCK) += spinlock_debug.o
obj-$(CONFIG_QUEUED_RWLOCKS) += qrwlock.o
# 最后三项是可选验证/观测组件：locktorture 持续制造锁竞争，ww_mutex selftest 覆盖 wound/wait
# 事务式死锁消解，lock_events 为选定锁事件维护轻量计数并经 debugfs 暴露。
obj-$(CONFIG_LOCK_TORTURE_TEST) += locktorture.o
obj-$(CONFIG_WW_MUTEX_SELFTEST) += test-ww_mutex.o
obj-$(CONFIG_LOCK_EVENT_COUNTS) += lock_events.o
