// SPDX-License-Identifier: GPL-2.0

/*
 * arm64 用户态指针认证控制学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 文件职责：实现用户空间通过 prctl 重置 PAC key，并供 prctl/ptrace 选择地址
 * 认证 key 是否启用以及查询当前启用状态。key 的内存表示和系统寄存器装载
 * helper 位于 <asm/pointer_auth.h>；本文件负责参数校验和 task 状态提交，不处理
 * PAC 指令编码，也不管理内核返回地址使用的 keys_kernel。
 *
 * 主路径：
 *   PR_PAC_RESET_KEYS -> 校验能力/ABI/位图 -> 生成新随机 key -> 装载当前 CPU
 *   PR_PAC_SET_ENABLED_KEYS -> 位图转 SCTLR En* -> 更新 thread.sctlr_user
 *   PR_PAC_GET_ENABLED_KEYS -> SCTLR En* -> PR_PAC_AP*KEY 位图
 *
 * 核心对象与生命周期：keys_user 和 sctlr_user 都嵌在 task_struct::thread 中，随
 * 任务创建、exec、切换和销毁，不单独分配或引用。这里的 task 指针均是调用者
 * 已按 prctl/ptrace 协议稳定的借用指针；函数不改变 ownership。
 *
 * 并发模型：修改非 current 的停止任务依赖 ptrace 等上层串行协议；修改 current
 * 时禁止抢占，保证“写任务快照 + 写本 CPU SCTLR_EL1”发生在同一 CPU。禁止抢占
 * 不是保护任意远端 task 的通用锁。key 系统寄存器写入只影响当前 CPU，返回 EL0
 * 前的异常路径会完成用户 APIA 与控制状态同步。
 *
 * 方案权衡：严格拒绝 compat 任务和未知位可保持 AArch64 PAC ABI 清晰；用一个
 * sctlr_user 快照统一保存多个 key 的启用状态，使上下文切换只需恢复寄存器，但
 * 调用者必须遵守目标任务稳定和 CPU 亲和的并发约束。
 */

#include <linux/compat.h>
#include <linux/errno.h>
#include <linux/prctl.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <asm/cpufeature.h>
#include <asm/pointer_auth.h>

/*
 * Linux 头提供 compat 判定、errno、prctl ABI、随机数和 task_struct；体系结构头
 * 提供硬件能力检测、PAC key helper 与 SCTLR 位定义。这里只建立编译期依赖。
 */

/*
 * ptrauth_prctl_reset_keys() - 按用户位图轮换目标任务的用户 PAC key。
 *
 * 调用关系：PR_PAC_RESET_KEYS 的 arm64 prctl 后端调用；成功后返回系统调用层，
 * 当前任务最终经 entry.S 返回 EL0 并装入新的用户 APIAKey。
 *
 * @tsk: 非 NULL 且必须为 current 的 task_struct 借用指针；kernel/sys.c 的 prctl
 * 路径以 me 传入。函数不增减引用、不转移 ownership。
 * @arg: PR_PAC_APIAKEY/APIBKEY/APDAKEY/APDBKEY/APGAKEY 位图；0 特别表示重置
 * 当前硬件支持的全部用户 key，其他位必须精确落在合法集合内。
 *
 * 返回：成功为 0；硬件完全无 PAC、AArch32 compat 任务、未知位或请求硬件不支持
 * 的 key 时返回 -EINVAL。所有校验先于第一次写入，失败保证 key 快照和寄存器
 * 不变；成功路径不分配资源、无 cleanup，也不转移任何 ownership。
 *
 * 并发：函数自身不加锁。current 的 prctl 路径在内核态串行更新自身状态；不得
 * 用本接口修改非 current，因为最后的寄存器安装明确作用于当前 CPU/任务。
 */
int ptrauth_prctl_reset_keys(struct task_struct *tsk, unsigned long arg)
{
	/*
	 * 变量地图：
	 *   keys          目标任务内嵌用户 key 集的借用指针。
	 *   addr_key_mask 四个地址认证 key 的合法输入集合。
	 *   key_mask      再加入通用 APGAKey 后的完整合法集合。
	 */
	struct ptrauth_keys_user *keys = &tsk->thread.keys_user;
	unsigned long addr_key_mask = PR_PAC_APIAKEY | PR_PAC_APIBKEY |
				      PR_PAC_APDAKEY | PR_PAC_APDBKEY;
	unsigned long key_mask = addr_key_mask | PR_PAC_APGAKEY;

	/* 两类硬件能力都不存在时没有任何可重置对象，保持状态并拒绝请求。 */
	if (!system_supports_address_auth() && !system_supports_generic_auth())
		return -EINVAL;

	/* PAC 用户 ABI 只定义给原生 AArch64 task，不能把同一位图套到 AArch32 状态。 */
	if (is_compat_thread(task_thread_info(tsk)))
		return -EINVAL;

	/*
	 * arg==0 是 ABI 规定的“全部重置”快速路径。helper 按实际硬件能力随机化所有
	 * 可用字段并预装非 APIA 用户 key；APIA 留到返回 EL0 的最后阶段安装。
	 */
	if (!arg) {
		ptrauth_keys_init_user(keys);
		return 0;
	}

	/* 未知位无法对应任何 key，必须在部分随机化之前整体拒绝。 */
	if (arg & ~key_mask)
		return -EINVAL;

	/* 地址 key 与通用 key 有独立能力位；合法名称仍可能在本机不可实现。 */
	if (((arg & addr_key_mask) && !system_supports_address_auth()) ||
	    ((arg & PR_PAC_APGAKEY) && !system_supports_generic_auth()))
		return -EINVAL;

	/*
	 * 阶段 2：逐项轮换被选中的 128 位秘密。每个字段都由 task_struct 持有；
	 * get_random_bytes() 直接覆盖旧快照且无错误返回，因此通过校验后无需回滚。
	 */
	if (arg & PR_PAC_APIAKEY)
		get_random_bytes(&keys->apia, sizeof(keys->apia));
	if (arg & PR_PAC_APIBKEY)
		get_random_bytes(&keys->apib, sizeof(keys->apib));
	if (arg & PR_PAC_APDAKEY)
		get_random_bytes(&keys->apda, sizeof(keys->apda));
	if (arg & PR_PAC_APDBKEY)
		get_random_bytes(&keys->apdb, sizeof(keys->apdb));
	if (arg & PR_PAC_APGAKEY)
		get_random_bytes(&keys->apga, sizeof(keys->apga));
	/*
	 * 把 APIB/APDA/APDB/APGA 发布到当前 CPU；APIAKey_EL1 此刻仍需保留内核
	 * 返回地址 key，entry.S 会在不再调用内核 C 函数后装入新的用户 APIA。
	 */
	ptrauth_keys_install_user(keys);

	return 0;
}

/*
 * arg_to_enxx_mask() - 把用户 ABI 的 key 位图转换为 SCTLR_EL1 使能位。
 *
 * @arg: 仅允许 PR_PAC_ENABLED_KEYS_MASK 中的 APIA/APIB/APDA/APDB 位；调用者
 * 应在进入前完成用户输入校验。该值是纯输入，无单位、无 ownership。
 *
 * 返回：由 SCTLR_ELx_ENIA/ENIB/ENDA/ENDB 组成的 u64 掩码；空输入返回 0。
 * 函数不读写任务或硬件、不睡眠、无失败码。WARN_ON 只捕获内核调用契约错误，
 * 不能替代外层对不可信用户位图的 -EINVAL 校验。
 */
static u64 arg_to_enxx_mask(unsigned long arg)
{
	/* sctlr_enxx_mask 从空集合累加每个被请求 key 对应的硬件控制位。 */
	u64 sctlr_enxx_mask = 0;

	/* 若触发说明内核调用者漏掉校验；继续只转换已知位，避免污染控制寄存器。 */
	WARN_ON(arg & ~PR_PAC_ENABLED_KEYS_MASK);
	/* 四个独立 if 允许一次请求组合任意地址 key，而不是互斥选择。 */
	if (arg & PR_PAC_APIAKEY)
		sctlr_enxx_mask |= SCTLR_ELx_ENIA;
	if (arg & PR_PAC_APIBKEY)
		sctlr_enxx_mask |= SCTLR_ELx_ENIB;
	if (arg & PR_PAC_APDAKEY)
		sctlr_enxx_mask |= SCTLR_ELx_ENDA;
	if (arg & PR_PAC_APDBKEY)
		sctlr_enxx_mask |= SCTLR_ELx_ENDB;
	return sctlr_enxx_mask;
}

/*
 * ptrauth_set_enabled_keys() - 成组更新任务快照中的用户 PAC 使能选择。
 *
 * 调用关系：PR_PAC_SET_ENABLED_KEYS prctl、ptrace regset 以及 exec 初始化路径调用；
 * 下次返回 EL0 或任务切换时，thread.sctlr_user 成为硬件 SCTLR_EL1 的来源。
 *
 * @tsk: 非 NULL 的目标任务借用指针，不改变引用或 ownership；非 current 目标必须
 * 已由上层停止/稳定。
 * @keys: 本次允许修改的四个地址 key 位集合。
 * @enabled: @keys 的子集；位为 1 表示启用，位为 0 表示在本次范围内禁用。
 *
 * 返回：成功为 0；硬件无地址认证、compat 任务、未知 key 位或 enabled 超出 keys
 * 时返回 -EINVAL，且快照/硬件均不变。成功不分配资源，无失败回滚。
 *
 * 并发：preempt_disable() 保证 current 比较、任务快照写入以及可选硬件更新留在
 * 同一 CPU；它不锁住远端 tsk，远端目标的互斥责任仍在调用者。
 */
int ptrauth_set_enabled_keys(struct task_struct *tsk, unsigned long keys,
			     unsigned long enabled)
{
	/* sctlr 是目标任务用户控制寄存器快照的工作副本，提交前只在本函数有效。 */
	u64 sctlr;

	/* 没有地址认证能力时不存在可设置的 EnIA/EnIB/EnDA/EnDB 语义。 */
	if (!system_supports_address_auth())
		return -EINVAL;

	if (is_compat_thread(task_thread_info(tsk)))
		return -EINVAL;

	if ((keys & ~PR_PAC_ENABLED_KEYS_MASK) || (enabled & ~keys))
		return -EINVAL;

	/*
	 * 阶段 2：在禁止迁移的窗口内做“读取旧快照 -> 清本次范围 -> 置启用子集 ->
	 * 提交”。先清后置使未列入 @keys 的控制位保持原值。
	 */
	preempt_disable();
	sctlr = tsk->thread.sctlr_user;
	sctlr &= ~arg_to_enxx_mask(keys);
	sctlr |= arg_to_enxx_mask(enabled);
	tsk->thread.sctlr_user = sctlr;
	/*
	 * current 正在此 CPU 上运行，必须同步真实 SCTLR_EL1；远端/停止任务只更新
	 * 快照，等其将来切换上 CPU 时由 __switch_to() 恢复。
	 */
	if (tsk == current)
		update_sctlr_el1(sctlr);
	preempt_enable();

	return 0;
}

/*
 * ptrauth_get_enabled_keys() - 把任务 SCTLR 用户快照转换回 prctl key 位图。
 *
 * @tsk: 非 NULL 的只读借用指针；调用者保证目标任务状态在读取期间稳定，本函数
 * 不获取引用、不取锁，也不改变 ownership。
 *
 * 返回：成功为零或多个 PR_PAC_APIA/APIB/APDA/APDBKEY 位；硬件无地址认证或
 * compat 任务返回 -EINVAL。函数不读取当前 CPU SCTLR，而以 thread.sctlr_user
 * 为权威任务状态，因此对停止的非 current 任务同样适用。
 */
int ptrauth_get_enabled_keys(struct task_struct *tsk)
{
	/* retval 从空 ABI 位图累加每个已设置的 SCTLR En* 位。 */
	int retval = 0;

	/* 与 set 路径保持相同能力和 ABI 边界，避免向不支持任务伪造状态。 */
	if (!system_supports_address_auth())
		return -EINVAL;

	if (is_compat_thread(task_thread_info(tsk)))
		return -EINVAL;

	/* 四个状态彼此独立，组合后精确返回当前启用集合。 */
	if (tsk->thread.sctlr_user & SCTLR_ELx_ENIA)
		retval |= PR_PAC_APIAKEY;
	if (tsk->thread.sctlr_user & SCTLR_ELx_ENIB)
		retval |= PR_PAC_APIBKEY;
	if (tsk->thread.sctlr_user & SCTLR_ELx_ENDA)
		retval |= PR_PAC_APDAKEY;
	if (tsk->thread.sctlr_user & SCTLR_ELx_ENDB)
		retval |= PR_PAC_APDBKEY;

	return retval;
}
