// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 Guarded Control Stack（GCS）用户态管理学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * GCS 是硬件保护的返回地址栈：普通 store 不能随意写入，受控 push/pop、
 * capability token 和 GCSPR_EL0 共同维护控制流。该文件负责为 clone/prctl/
 * map_shadow_stack 建 VMA、初始化线程寄存器快照、切换 GCSCRE0_EL1 模式并
 * 在任务退出时 unmap；异常入口实际保存/恢复寄存器位于其他架构代码。
 *
 * 栈 VMA 属于 task->mm，thread.gcs_* 只保存该任务的基址、大小和当前指针。
 * 修改其他任务受 task 锁/ptrace 上层序列化；需要创建 VMA 或写硬件寄存器
 * 的路径限制为 current。禁用后不允许用旧 base 重新启用，避免复用状态
 * 不明的 capability 链。失败回滚必须先 unmap VMA，再丢弃线程字段。
 */

#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/syscalls.h>
#include <linux/types.h>

#include <asm/cmpxchg.h>
#include <asm/cpufeature.h>
#include <asm/gcs.h>
#include <asm/page.h>

/*
 * 通过 MM 核心创建 VM_SHADOW_STACK 映射。addr=0 允许内核选择地址，否则
 * 是用户期望基址；size 为页对齐字节数。返回用户 VA 或 unsigned long
 * 编码的负 errno，成功 VMA 归 current->mm，需 vm_munmap 回收。
 */
static unsigned long alloc_gcs(unsigned long addr, unsigned long size)
{
	return vm_mmap_shadow_stack(addr, size, 0);
}

/*
 * 规范化 GCS 大小。非零 size 向上页对齐；0 使用当前 RLIMIT_STACK/2，
 * 并限制为 [PAGE_SIZE,2GiB]。返回字节数，不分配资源。减半给普通栈与
 * guard stack 留出不同增长/限制空间，是默认策略而非硬件要求。
 */
static unsigned long gcs_size(unsigned long size)
{
	if (size)
		return PAGE_ALIGN(size);

	/* Allocate RLIMIT_STACK/2 with limits of PAGE_SIZE..2G */
	/* 防止无限 rlimit 导致过大 VA 消耗，也保证最小能容纳一个映射页。 */
	size = PAGE_ALIGN(min_t(unsigned long long,
				rlimit(RLIMIT_STACK) / 2, SZ_2G));
	return max(PAGE_SIZE, size);
}

/*
 * 为 clone 创建的 tsk 准备用户 GCS。args 描述 clone flags/用户栈大小；
 * 返回新映射基址、0 表示无需新栈，或负 errno 编码。无硬件/任务未启用时
 * 空操作；非 CLONE_VM 或 vfork 路径继承当前硬件 GCSPR 快照，不另建 VMA。
 * 真正共享 mm 的线程获得独立半尺寸 GCS，成功后发布 base/size/gcspr。
 */
unsigned long gcs_alloc_thread_stack(struct task_struct *tsk,
				     const struct kernel_clone_args *args)
{
	unsigned long addr, size;

	if (!system_supports_gcs())
		return 0;

	if (!task_gcs_el0_enabled(tsk))
		return 0;

	if ((args->flags & (CLONE_VFORK | CLONE_VM)) != CLONE_VM) {
		/* fork 拥有复制后的 mm/VMA；vfork 共享执行流，均只继承当前指针。 */
		tsk->thread.gcspr_el0 = read_sysreg_s(SYS_GCSPR_EL0);
		return 0;
	}

	size = args->stack_size / 2;

	size = gcs_size(size);
	addr = alloc_gcs(0, size);
	if (IS_ERR_VALUE(addr))
		return addr;

	tsk->thread.gcs_base = addr;
	tsk->thread.gcs_size = size;
	tsk->thread.gcspr_el0 = addr + size - sizeof(u64);
	/* GCS 向低地址增长，初始 SP 指向映射末端预留的首个 8-byte frame。 */

	return addr;
}

/*
 * map_shadow_stack(addr,size,flags) 为用户显式建立 GCS VMA。
 * addr 必须页对齐，size 必须 8-byte 对齐且不能恰为 8；flags 仅允许创建
 * switch capability token 与可选顶部 marker。成功返回用户 VA，失败负
 * errno。token 写失败会 unmap 整个已分配区，不泄漏 VMA。
 */
SYSCALL_DEFINE3(map_shadow_stack, unsigned long, addr, unsigned long, size, unsigned int, flags)
{
	unsigned long alloc_size;
	unsigned long __user *cap_ptr;
	unsigned long cap_val;
	int ret = 0;
	int cap_offset;

	if (!system_supports_gcs())
		return -EOPNOTSUPP;

	if (flags & ~(SHADOW_STACK_SET_TOKEN | SHADOW_STACK_SET_MARKER))
		return -EINVAL;

	if (!PAGE_ALIGNED(addr))
		return -EINVAL;

	if (size == 8 || !IS_ALIGNED(size, 8))
		return -EINVAL;

	/*
	 * An overflow would result in attempting to write the restore token
	 * to the wrong location. Not catastrophic, but just return the right
	 * error code and block it.
	 */
	/* PAGE_ALIGN 回绕会让 token 地址落到请求区外，必须在分配前拒绝。 */
	alloc_size = PAGE_ALIGN(size);
	if (alloc_size < size)
		return -EOVERFLOW;

	addr = alloc_gcs(addr, alloc_size);
	if (IS_ERR_VALUE(addr))
		return addr;

	/*
	 * Put a cap token at the end of the allocated region so it
	 * can be switched to.
	 */
	/* token 位于用户请求的 size 末端，而非包含页尾 padding 的 alloc_size 末端。 */
	if (flags & SHADOW_STACK_SET_TOKEN) {
		/* Leave an extra empty frame as a top of stack marker? */
		/* marker 使 token 前额外留一个空 frame，cap_offset 以 unsigned long 为单位。 */
		if (flags & SHADOW_STACK_SET_MARKER)
			cap_offset = 2;
		else
			cap_offset = 1;

		cap_ptr = (unsigned long __user *)(addr + size -
						   (cap_offset * sizeof(unsigned long)));
		cap_val = GCS_CAP(cap_ptr);

		put_user_gcs(cap_val, cap_ptr, &ret);
		/* 专用 GCS store 遵守硬件写保护；普通 put_user 无法合法写 capability。 */
		if (ret != 0) {
			vm_munmap(addr, size);
			return -EFAULT;
		}

		/*
		 * Ensure the new cap is ordered before standard
		 * memory accesses to the same location.
		 */
		/* GCSB DSYNC 在把地址返回用户前发布 capability，防止普通访问先观察。 */
		gcsb_dsync();
	}

	return addr;
}

/*
 * Apply the GCS mode configured for the specified task to the
 * hardware.
 */
/*
 * 根据 task->thread.gcs_el0_mode 构造并写 GCSCRE0_EL1。task 通常是即将
 * 返回 EL0 的 current；函数不修改 task 状态，只改变本 CPU EL0 行为。
 * nTR 是基础策略，ENABLE 打开返回校验/PCR 选择，WRITE/PUSH 分别允许
 * guarded store/push。调用者负责调度/异常上下文序列化。
 */
void gcs_set_el0_mode(struct task_struct *task)
{
	u64 gcscre0_el1 = GCSCRE0_EL1_nTR;

	if (task->thread.gcs_el0_mode & PR_SHADOW_STACK_ENABLE)
		gcscre0_el1 |= GCSCRE0_EL1_RVCHKEN | GCSCRE0_EL1_PCRSEL;

	if (task->thread.gcs_el0_mode & PR_SHADOW_STACK_WRITE)
		gcscre0_el1 |= GCSCRE0_EL1_STREn;

	if (task->thread.gcs_el0_mode & PR_SHADOW_STACK_PUSH)
		gcscre0_el1 |= GCSCRE0_EL1_PUSHMEn;

	write_sysreg_s(gcscre0_el1, SYS_GCSCRE0_EL1);
}

/*
 * 释放 task 的自动 GCS VMA并清线程快照。只有 task->mm==current->mm 时
 * 才能安全调用 vm_munmap（它操作 current mm）；内核线程/其他 mm 空操作。
 * unmap 后无论 base 是否存在都清 gcspr/base/size，防止之后误恢复旧地址。
 */
void gcs_free(struct task_struct *task)
{
	if (!system_supports_gcs())
		return;

	if (!task->mm || task->mm != current->mm)
		return;

	if (task->thread.gcs_base)
		vm_munmap(task->thread.gcs_base, task->thread.gcs_size);

	task->thread.gcspr_el0 = 0;
	task->thread.gcs_base = 0;
	task->thread.gcs_size = 0;
}

/*
 * 实现 PR_SET_SHADOW_STACK_STATUS。task 是目标线程，arg 是完整新模式位图；
 * 返回 0 或 -EINVAL/-EBUSY/分配 errno。拒绝无 GCS、compat、未知位及被
 * lock 的变化。首次 enable 仅允许 current，分配默认栈并立即写 GCSPR；
 * 最后提交 mode，若目标为 current 同步硬件控制寄存器。
 */
int arch_set_shadow_stack_status(struct task_struct *task, unsigned long arg)
{
	unsigned long gcs, size;
	int ret;

	if (!system_supports_gcs())
		return -EINVAL;

	if (is_compat_thread(task_thread_info(task)))
		return -EINVAL;

	/* Reject unknown flags */
	/* 只接受内核向用户 ABI 明确公布的状态位，避免未来扩展位被旧内核误解释。 */
	if (arg & ~PR_SHADOW_STACK_SUPPORTED_STATUS_MASK)
		return -EINVAL;

	ret = gcs_check_locked(task, arg);
	if (ret != 0)
		return ret;

	/* If we are enabling GCS then make sure we have a stack */
	/* mode 尚未 enable 且 base/sp 必须全零，保证一生只自动创建一次。 */
	if (arg & PR_SHADOW_STACK_ENABLE &&
	    !task_gcs_el0_enabled(task)) {
		/* Do not allow GCS to be reenabled */
		/* 禁用保留的旧地址可能已被 unmap/篡改，拒绝重新激活它。 */
		if (task->thread.gcs_base || task->thread.gcspr_el0)
			return -EINVAL;

		if (task != current)
			return -EBUSY;

		size = gcs_size(0);
		gcs = alloc_gcs(0, size);
		if (IS_ERR_VALUE(gcs))
			return gcs;

		task->thread.gcspr_el0 = gcs + size - sizeof(u64);
		task->thread.gcs_base = gcs;
		task->thread.gcs_size = size;
		/* 三个软件字段写完后才更新硬件，返回用户时观察一致状态。 */
		if (task == current)
			write_sysreg_s(task->thread.gcspr_el0,
				       SYS_GCSPR_EL0);
	}

	task->thread.gcs_el0_mode = arg;
	if (task == current)
		gcs_set_el0_mode(task);

	return 0;
}

/*
 * 实现 PR_GET_SHADOW_STACK_STATUS。task 必须是支持 GCS 的原生 AArch64
 * 线程，arg 是用户输出指针；返回 put_user 的 0/-EFAULT 或 -EINVAL。
 * 只读取上层已序列化的 mode，不访问硬件寄存器。
 */
int arch_get_shadow_stack_status(struct task_struct *task,
				 unsigned long __user *arg)
{
	if (!system_supports_gcs())
		return -EINVAL;

	if (is_compat_thread(task_thread_info(task)))
		return -EINVAL;

	return put_user(task->thread.gcs_el0_mode, arg);
}

/*
 * 将 arg 位永久 OR 入 task 的 GCS 状态锁。支持未知位是面向未来的 ABI：
 * 老内核可记录应用不希望未来改变的位。返回 0/-EINVAL；锁只增不减，
 * gcs_check_locked() 在 SET 路径执行实际变化校验。
 */
int arch_lock_shadow_stack_status(struct task_struct *task,
				  unsigned long arg)
{
	if (!system_supports_gcs())
		return -EINVAL;

	if (is_compat_thread(task_thread_info(task)))
		return -EINVAL;

	/*
	 * We support locking unknown bits so applications can prevent
	 * any changes in a future proof manner.
	 */
	/* OR 而非赋值确保此前锁定位永不被本调用清除。 */
	task->thread.gcs_el0_locked |= arg;

	return 0;
}
