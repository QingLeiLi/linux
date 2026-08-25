// SPDX-License-Identifier: GPL-2.0
/*
 * Scheduler code and data structures related to cpufreq.
 *
 * Copyright (C) 2016, Intel Corporation
 * Author: Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 */
/*
 * 调度器与 CPUFreq 之间的利用率通知接口。
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 调度类在持有目标运行队列锁、更新 PELT 等利用率状态后，通过 sched.h 中的
 * cpufreq_update_util() 查找目标 CPU 的回调；schedutil、传统按需 governor 或
 * intel_pstate 在启用期间把各自嵌入对象注册到这里。该层只建立和摘除回调入口，
 * 不计算利用率、不选择频率，也不拥有 governor/驱动分配的对象。
 *
 * 每 CPU 指针用 RCU 发布：注册方必须在发布前完成 func 和外层容器初始化；调度器
 * 热路径在 RCU-sched 读侧借用对象并直接调用，因而回调不得睡眠。注销只阻止后续
 * 读者取得旧指针，已经开始的读者仍可继续执行；调用方必须等待 RCU 宽限期或采用
 * RCU 回调后，才能释放承载 update_util_data 的容器。这把热路径开销压到一次 RCU
 * 解引用、一次空值判断和一次间接调用，代价是生命周期回收责任留给注册方。
 */
#include "sched.h"

/*
 * 每个 CPU 唯一的利用率更新入口。
 *
 * 槽位为 NULL 表示该 CPU 当前没有接收调度器利用率通知的 governor/驱动；非 NULL
 * 时借用指向注册方对象内嵌的 update_util_data，所有权始终属于注册方。`__rcu`
 * 标注要求读者使用 rcu_dereference_sched()，写者使用 RCU 发布原语；它既不充当锁，
 * 也不单独延长对象生命周期。CPUFreq 的启停/热插拔控制路径负责串行化同一 CPU 的
 * 注册与注销，调度器热路径则只读该槽。
 */
DEFINE_PER_CPU(struct update_util_data __rcu *, cpufreq_update_util_data);

/**
 * cpufreq_add_update_util_hook - Populate the CPU's update_util_data pointer.
 * @cpu: The CPU to set the pointer for.
 * @data: New pointer value.
 * @func: Callback function to set for the CPU.
 *
 * Set and publish the update_util_data pointer for the given CPU.
 *
 * The update_util_data pointer of @cpu is set to @data and the callback
 * function pointer in the target struct update_util_data is set to @func.
 * That function will be called by cpufreq_update_util() from RCU-sched
 * read-side critical sections, so it must not sleep.  @data will always be
 * passed to it as the first argument which allows the function to get to the
 * target update_util_data structure and its container.
 *
 * The update_util_data pointer of @cpu must be NULL when this function is
 * called or it will WARN() and return with no effect.
 */
/*
 * cpufreq_add_update_util_hook() 填充指定 CPU 的 update_util_data
 * 指针。@cpu 是要设置槽位的 CPU，@data 是新指针值，@func 是为该 CPU 安装的
 * 回调。函数设置并发布该 CPU 的指针，同时把目标 update_util_data 的 func 字段
 * 设为 @func。cpufreq_update_util() 会在 RCU-sched 读侧临界区调用它，因此回调
 * 不得睡眠；@data 总作为第一个实参传入，使回调可以找回 update_util_data 及其
 * 外层容器。调用时该 CPU 的槽必须为 NULL，否则触发 WARN() 并且不产生效果。
 *
 * cpufreq_add_update_util_hook() - 发布一个 CPU 的调度器利用率更新回调
 *
 * 调用位置：CPUFreq governor 启动或 intel_pstate 为 CPU 启用利用率反馈时；返回后，
 * fair/RT/DL 等调度路径可经 cpufreq_update_util() 进入注册回调。
 *
 * @cpu: 纯输入的逻辑 CPU 编号；必须是调用者正在配置且其槽仍为空的 CPU。本函数不
 *       检查编号范围，也不负责 CPU 上线或 policy 归属。
 * @data: 输入并被发布的非 NULL 借用指针，通常嵌入 governor/驱动的 per-CPU 对象。
 *        所有权不转移；从发布到注销后的 RCU 宽限期结束，调用者必须保持整个容器有效。
 * @func: 非 NULL 回调，接收同一个 @data、rq 时钟的 ns 时间戳和 SCHED_CPUFREQ_*
 *        原因标志；函数代码及其依赖在注册期间必须有效，且回调不得睡眠。
 *
 * 返回：无直接返回值。@data 或 @func 为空、或者槽已占用时，WARN_ON() 后原样返回，
 * 调用者无法通过返回值区分失败，只能保证自身启停协议不违反前置条件。成功时先写
 * data->func，再以 rcu_assign_pointer() 的发布语义让读者看到完整初始化；不分配
 * 内存、不等待读者，也不接管任何引用。
 *
 * 并发约束：注册方必须在 CPUFreq 控制路径中串行化同一槽的写者。RCU 只保证已发布
 * 初始化对读者可见并延迟回收，不会阻止两个注册者竞态；这里的占用检查也不是原子
 * 认领操作。函数只做不可睡眠操作，但调用者上下文仍由外层 CPUFreq 启停协议决定。
 */
void cpufreq_add_update_util_hook(int cpu, struct update_util_data *data,
			void (*func)(struct update_util_data *data, u64 time,
				     unsigned int flags))
{
	/* 无有效对象或回调就无法建立可调用入口；告警后保持槽和 @data 均不变。 */
	if (WARN_ON(!data || !func))
		return;

	/* 每 CPU 只允许一个提供者；外层串行化使该检查成为协议断言而非竞争锁。 */
	if (WARN_ON(per_cpu(cpufreq_update_util_data, cpu)))
		return;

	/*
	 * 先完成被发布对象的函数指针初始化，再发布对象地址。与热路径的
	 * rcu_dereference_sched() 配对后，看到非 NULL data 的读者也能看到 func；
	 * 若顺序相反，调度器可能解引用尚未初始化的回调。
	 */
	data->func = func;
	rcu_assign_pointer(per_cpu(cpufreq_update_util_data, cpu), data);
}
EXPORT_SYMBOL_GPL(cpufreq_add_update_util_hook);

/**
 * cpufreq_remove_update_util_hook - Clear the CPU's update_util_data pointer.
 * @cpu: The CPU to clear the pointer for.
 *
 * Clear the update_util_data pointer for the given CPU.
 *
 * Callers must use RCU callbacks to free any memory that might be
 * accessed via the old update_util_data pointer or invoke synchronize_rcu()
 * right after this function to avoid use-after-free.
 */
/*
 * cpufreq_remove_update_util_hook() 清除指定 CPU 的
 * update_util_data 指针。调用者必须通过 RCU 回调释放任何可能经旧指针访问的内存，
 * 或在本函数之后立即调用 synchronize_rcu()，从而避免 use-after-free。
 *
 * cpufreq_remove_update_util_hook() - 摘除一个 CPU 的利用率更新入口
 *
 * 调用位置：governor 停止、驱动撤销回调或 CPUFreq 热插拔清理路径；通常对 policy
 * 内所有 CPU 摘除后统一 synchronize_rcu()，再释放或复用 governor/驱动状态。
 *
 * @cpu: 纯输入的逻辑 CPU 编号；调用者负责保证编号有效并串行化该槽的启停。槽已经
 *       为 NULL 时重复写 NULL 不会报错。
 *
 * 返回：无直接返回值。函数只用 RCU 发布语义把槽改为 NULL，不读取或清空旧对象的
 * func，不释放内存，也不等待回调结束。返回后新读者不会再从该槽取得旧对象，但已在
 * RCU-sched 临界区内取得旧指针的读者仍可能执行 func；因此旧容器的 ownership 仍由
 * 调用者承担，直至 synchronize_rcu() 返回或对应 RCU callback 被执行。
 *
 * 摘除还使 cpufreq_this_cpu_can_update() 在 remote-DVFS 分支把本 CPU 视为不再适合
 * 发起更新，从而避免正在下线的 CPU 留下新的频率工作。函数本身不睡眠；等待宽限期
 * 是调用者在适合睡眠的控制路径中完成的独立阶段。
 */
void cpufreq_remove_update_util_hook(int cpu)
{
	/* 发布空槽先切断新读者入口；旧读者的完成由随后宽限期等待负责。 */
	rcu_assign_pointer(per_cpu(cpufreq_update_util_data, cpu), NULL);
}
EXPORT_SYMBOL_GPL(cpufreq_remove_update_util_hook);

/**
 * cpufreq_this_cpu_can_update - Check if cpufreq policy can be updated.
 * @policy: cpufreq policy to check.
 *
 * Return 'true' if:
 * - the local and remote CPUs share @policy,
 * - dvfs_possible_from_any_cpu is set in @policy and the local CPU is not going
 *   offline (in which case it is not expected to run cpufreq updates any more).
 */
/*
 * cpufreq_this_cpu_can_update() 检查 CPUFreq policy 能否从当前 CPU
 * 更新。以下任一条件成立就返回 true：本地 CPU 与被更新的远端 CPU 共享 @policy；
 * 或者 @policy 允许任意 CPU 发起 DVFS，且本地 CPU 没有正在下线——下线中的 CPU
 * 不应再执行 CPUFreq 更新。
 *
 * cpufreq_this_cpu_can_update() - 判断当前执行 CPU 能否代表目标 policy 发起更新
 *
 * 调用位置：schedutil 和通用 governor 的 update_util 回调入口。此时调度器正持有
 * 被更新 rq 的锁并处于 RCU-sched 读侧，当前 CPU 不会在检查中迁移；本函数只做门禁，
 * 真正计算和提交频率由回调后续路径负责。
 *
 * @policy: 纯输入、不可为 NULL 的借用 policy。其 cpus 掩码只包含当前在线且共享该
 *          频率控制域的 CPU；dvfs_possible_from_any_cpu 表示硬件/驱动允许其他 policy
 *          的 CPU 代为发起 DVFS。对象生命周期由已安装回调及 CPUFreq 启停协议保证，
 *          本函数不取得引用、锁或 ownership。
 *
 * 返回 true 表示当前 CPU 具备发起该 policy 更新的资格，false 表示调用方应立即跳过；
 * 无错误码、无状态副作用且不会睡眠。第一项是共享 policy 的常见快速路径。仅在本地
 * CPU 不属于 policy 时才检查 remote-DVFS 能力，并以本 CPU 的 update_util 槽仍为
 * 非 NULL 作为“尚未撤销、没有进入下线清理”的生命期信号。
 *
 * 这里的 RCU 解引用只判断指针是否存在，不在临界区外保存或调用它；RCU 保证检查期间
 * 指针可安全读取，但不冻结 policy 字段。调用者所在的 governor/CPUFreq 协议负责使
 * policy->cpus 与 remote-DVFS 标志在本次回调期间保持可用。
 */
bool cpufreq_this_cpu_can_update(struct cpufreq_policy *policy)
{
	/*
	 * 当前 CPU 在线且属于同一 policy 时可直接更新。否则只有硬件允许跨 policy
	 * DVFS，并且当前 CPU 自己仍保有回调槽（尚未进入下线撤销）时才放行。
	 */
	return cpumask_test_cpu(smp_processor_id(), policy->cpus) ||
		(policy->dvfs_possible_from_any_cpu &&
		 rcu_dereference_sched(*this_cpu_ptr(&cpufreq_update_util_data)));
}
