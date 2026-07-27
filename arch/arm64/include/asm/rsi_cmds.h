/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Arm CCA RSI SMC 类型安全封装学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * rsi_smc.h 定义裸 function ID 和寄存器 ABI；本文件把它们封装成 C
 * inline helper，负责装填 SMCCC 寄存器、提取输出和执行最小参数检查。
 * rsi.h 再在这些原语之上实现范围推进、DESTROYED 策略和 Linux errno。
 *
 * 所有调用都是同步 SMC：参数指针仅在调用期间借用，RMM 真正接收的是寄存器
 * 值或 IPA。普通 helper 没有锁、引用计数和动态分配；attestation 是例外，
 * INIT 在当前 CPU 建立隐式生成状态，所有 CONTINUE 必须回到同一 CPU。
 *
 * 封装不会统一抹平 RSI 状态：查询/attestation 保留 RSI_SUCCESS、
 * RSI_INCOMPLETE 等原值；部分策略 helper 把 Host 拒绝转换为 Linux errno。
 * 调用者仍需负责范围对齐、对象生命周期、CPU 亲和性和失败后的资源回收。
 */
/*
 * Copyright (C) 2023 ARM Ltd.
 */

#ifndef __ASM_RSI_CMDS_H
#define __ASM_RSI_CMDS_H

#include <linux/arm-smccc.h>
#include <linux/string.h>
#include <asm/memory.h>

#include <asm/rsi_smc.h>

/* RSI 的最小状态/传输粒度为 2^12=4096 字节；范围和 token 页均以此对齐。 */
#define RSI_GRANULE_SHIFT		12
#define RSI_GRANULE_SIZE		(_AC(1, UL) << RSI_GRANULE_SHIFT)

/*
 * enum ripas - Realm IPA 的 RMM 受保护状态。
 *
 * EMPTY 表示未作为 Realm RAM/可信设备使用，通常对应 Host 模拟或共享 backing；
 * RAM 表示受 RMM 保证的 Realm RAM；DESTROYED 表示 Host 未经允许破坏了原
 * 受保护内容，数据不可再信任；DEV 表示可信设备内存或 Realm 侧可信 MMIO。
 * 状态由 RMM 持有，Linux 查询或请求转换，不在本地枚举值上维护引用。
 */
enum ripas {
	RSI_RIPAS_EMPTY = 0,
	RSI_RIPAS_RAM = 1,
	RSI_RIPAS_DESTROYED = 2,
	RSI_RIPAS_DEV = 3,
};

/*
 * rsi_request_version() - 请求 RMM 协商 RSI ABI 版本。
 *
 * @req：按 RSI_ABI_VERSION 格式编码的请求版本，纯输入。
 * @out_lower：可为 NULL；非 NULL 时写入 RMM 返回的最低实现版本。
 * @out_higher：可为 NULL；非 NULL 时写入 RMM 返回的最高实现版本。
 *
 * 函数通过 SMC_RSI_ABI_VERSION 把 req 放入 x1，返回 x0 原始 SMCCC/RSI
 * 状态。两个输出指针均为借用的输出槽，不取得 ownership；当前实现无条件
 * 复制 x1/x2，因此调用者只应在返回状态允许时解释其含义。同步 SMC 不分配
 * 内存、不睡眠，也不建立全局就绪状态。
 */
static inline unsigned long rsi_request_version(unsigned long req,
						unsigned long *out_lower,
						unsigned long *out_higher)
{
	/* res 是本次调用的寄存器快照，离开 inline 后失效。 */
	struct arm_smccc_res res;

	/* 未使用的 x2..x7 清零，避免把调用方寄存器内容误当作 ABI 参数。 */
	arm_smccc_smc(SMC_RSI_ABI_VERSION, req, 0, 0, 0, 0, 0, 0, &res);

	/* NULL 允许只探测状态；非 NULL 输出由调用方存储并决定何时可信。 */
	if (out_lower)
		*out_lower = res.a1;
	if (out_higher)
		*out_higher = res.a2;

	return res.a0;
}

/*
 * rsi_get_realm_config() - 让 RMM 写回当前 Realm 的固定配置页。
 *
 * @cfg：借用的可写 struct realm_config 指针，不可为 NULL，必须满足类型的
 * 4 KiB 对齐并位于 RMM 可访问的 Realm RAM。调用前由调用者拥有存储；
 * 调用后 ownership 不变，成功时内容由 RMM 初始化。
 *
 * virt_to_phys() 把 Linux 虚拟地址转为 SMC x1 所需 IPA。返回 x0 原始 RSI
 * 状态；失败时 cfg 内容不能作为有效配置使用。函数同步执行，不保留 cfg
 * 指针，也不提供额外锁或缓存一致性协议。
 */
static inline unsigned long rsi_get_realm_config(struct realm_config *cfg)
{
	/* res 只保存 RMM 返回状态；配置主体由 RMM 直接写入 cfg 指向的 granule。 */
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_REALM_CONFIG, virt_to_phys(cfg),
		      0, 0, 0, 0, 0, 0, &res);
	return res.a0;
}

/*
 * rsi_ipa_state_get() - 查询从 start 开始的一段同质 RIPAS。
 *
 * @start：查询起始 IPA，单位字节，应按 RSI granule 对齐。
 * @end：查询排他末端，单位字节，必须大于 start 且按 granule 对齐。
 * @state：可为 NULL；成功时写入 [start,*top) 的 enum ripas。
 * @top：可为 NULL；成功时写入同状态段的排他末端。
 *
 * 所有指针均是借用输出槽。只有 res.a0==RSI_SUCCESS 才写 state/top，失败
 * 保留调用前内容；返回值是原始 RSI 状态。调用者负责验证 top 严格前进且
 * 不越过 end，并循环覆盖整个范围。
 */
static inline unsigned long rsi_ipa_state_get(phys_addr_t start,
					      phys_addr_t end,
					      enum ripas *state,
					      phys_addr_t *top)
{
	/* res.a1/a2 仅在成功状态下被提升为 C 输出，避免传播无效寄存器。 */
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_IPA_STATE_GET,
		      start, end, 0, 0, 0, 0, 0,
		      &res);

	if (res.a0 == RSI_SUCCESS) {
		if (top)
			*top = res.a1;
		if (state)
			*state = res.a2;
	}

	return res.a0;
}

/*
 * rsi_set_addr_range_state() - 请求改变一段 IPA 的 RIPAS 并检查 Host 接受。
 *
 * @start/@end：目标物理/IPA 半开区间，单位字节，按 RSI granule 对齐。
 * @state：目标 RIPAS。
 * @flags：RSI_CHANGE_DESTROYED 或 RSI_NO_CHANGE_DESTROYED，决定是否允许
 * 把 DESTROYED 页转换为目标状态。
 * @top：可为 NULL；写入 RMM 报告的本次处理末端，即使调用最终返回错误，
 * 调用者也不能仅凭 top 假定范围已经成功提交。
 *
 * RMM 的 x0 状态和 Host 的 x2 接受位是两层结果。Host 非 ACCEPT 时优先
 * 返回 -EPERM；接受时返回 x0 原始状态。函数不循环、不回滚部分推进，范围
 * 完整性由 rsi_set_memory_range() 检查。
 */
static inline long rsi_set_addr_range_state(phys_addr_t start,
					    phys_addr_t end,
					    enum ripas state,
					    unsigned long flags,
					    phys_addr_t *top)
{
	/* res 同时承载 RSI status、推进 top 和 Host decision。 */
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_IPA_STATE_SET, start, end, state,
		      flags, 0, 0, 0, &res);

	if (top)
		*top = res.a1;

	/* Host 拒绝与 RMM 自身错误不同，转换为 Linux 权限错误供策略层处理。 */
	if (res.a2 != RSI_ACCEPT)
		return -EPERM;

	return res.a0;
}

/**
 * rsi_attestation_token_init - Initialise the operation to retrieve an
 * attestation token.
 *
 * @challenge:	The challenge data to be used in the attestation token
 *		generation.
 * @size:	Size of the challenge data in bytes.
 *
 * Initialises the attestation token generation and returns an upper bound
 * on the attestation token size that can be used to allocate an adequate
 * buffer. The caller is expected to subsequently call
 * rsi_attestation_token_continue() to retrieve the attestation token data on
 * the same CPU.
 *
 * Returns:
 *  On success, returns the upper limit of the attestation report size.
 *  Otherwise, -EINVAL
 */
/*
 * 初始化 attestation token 生成，并返回最终 token 大小的上界。
 *
 * @challenge：借用的只读挑战字节串，非 NULL，长度由 size 指定；不保存指针。
 * @size：挑战长度，单位字节，范围 32..64。短于 64 的剩余 SMC 参数寄存器
 * 因 regs 零初始化而自动补零。
 *
 * 调用把最多 64 字节直接复制到 a1..a8，并用 SMCCC 1.2 SMC 原地复用 regs
 * 作为输入/输出寄存器块。成功返回正的 token 最大字节数；任何本地参数错误
 * 或 RSI 失败统一返回 -EINVAL。成功还在 RMM 中为当前 CPU 建立隐式生成
 * 状态，调用者必须在同一 CPU 调用 rsi_attestation_token_continue()。
 */
static inline long
rsi_attestation_token_init(const u8 *challenge, unsigned long size)
{
	/*
	 * regs 清零既固定 function ID 之外的输入，也实现英文契约所需的挑战值
	 * zero padding；调用返回后 a0/a1 分别承载状态和最大 token 大小。
	 */
	struct arm_smccc_1_2_regs regs = { 0 };

	/* The challenge must be at least 32bytes and at most 64bytes */
	/* 挑战必须至少 32 字节、至多 64 字节；NULL 即使 size 合法也拒绝。 */
	if (!challenge || size < 32 || size > 64)
		return -EINVAL;

	/*
	 * &regs.a1 指向连续寄存器槽，memcpy 按字节把 challenge 填入 a1..a8；
	 * size 上限保证不会越过 arm_smccc_1_2_regs 的输入区域。
	 */
	regs.a0 = SMC_RSI_ATTESTATION_TOKEN_INIT;
	memcpy(&regs.a1, challenge, size);
	arm_smccc_1_2_smc(&regs, &regs);

	/* 只在 RSI_SUCCESS 时把 a1 解释为分配上界，其余状态收敛为 Linux errno。 */
	if (regs.a0 == RSI_SUCCESS)
		return regs.a1;

	return -EINVAL;
}

/**
 * rsi_attestation_token_continue - Continue the operation to retrieve an
 * attestation token.
 *
 * @granule: {I}PA of the Granule to which the token will be written.
 * @offset:  Offset within Granule to start of buffer in bytes.
 * @size:    The size of the buffer.
 * @len:     The number of bytes written to the buffer.
 *
 * Retrieves up to a RSI_GRANULE_SIZE worth of token data per call. The caller
 * is expected to call rsi_attestation_token_init() before calling this
 * function to retrieve the attestation token.
 *
 * Return:
 * * %RSI_SUCCESS     - Attestation token retrieved successfully.
 * * %RSI_INCOMPLETE  - Token generation is not complete.
 * * %RSI_ERROR_INPUT - A parameter was not valid.
 * * %RSI_ERROR_STATE - Attestation not in progress.
 */
/*
 * 继续从当前 CPU 的 RMM attestation 状态机提取 token。
 *
 * @granule：RMM 可写的单个 RSI granule IPA，必须对齐且在调用期间保持有效。
 * @offset：granule 内起始字节偏移，范围小于 RSI_GRANULE_SIZE。
 * @size：从 offset 起可写的字节数，不得越过该 granule。
 * @len：可为 NULL；写入本次实际复制字节数。当前实现无论状态为何都复制
 * res.a1，调用者应结合返回状态和自己的边界验证解释。
 *
 * 调用前必须在同一 CPU 成功执行 token_init。返回 RSI_SUCCESS 表示完整
 * token 已生成，RSI_INCOMPLETE 要求继续，ERROR_INPUT/ERROR_STATE 分别表示
 * 参数无效或没有进行中的生成。函数不拥有 granule，也不推进调用方 offset。
 */
static inline unsigned long rsi_attestation_token_continue(phys_addr_t granule,
							   unsigned long offset,
							   unsigned long size,
							   unsigned long *len)
{
	/* res.a0/a1 是状态和本次写入长度；token 数据由 RMM 直接写入 granule。 */
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RSI_ATTESTATION_TOKEN_CONTINUE,
			     granule, offset, size, 0, &res);

	/* 可选输出让只关心状态的调用者省略长度槽，但驱动通常需要它推进 offset。 */
	if (len)
		*len = res.a1;
	return res.a0;
}

#endif /* __ASM_RSI_CMDS_H */
