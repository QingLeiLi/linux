/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM64 RSI 策略接口与范围转换 helper 学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件位于裸 RSI SMC 封装与 ARM64 内存管理消费者之间：声明 Realm 全局
 * 就绪 static key，提供低成本环境判断，并把一次可能只推进部分区间的
 * RSI_IPA_STATE_SET 包装成完整 [start,end) 转换。
 *
 * protected/shared 转换改变 RMM 维护的 RIPAS，不直接修改 Linux PTE。
 * pageattr.c 的完整协议会先使映射 invalid、调用这里改变 RIPAS、再重建 PTE，
 * 以避免旧 TLB 或错误 NS alias。调用者拥有页面和范围，本文件不分配、不
 * 引用也不释放 struct page。
 */
/*
 * Copyright (C) 2024 ARM Ltd.
 */

#ifndef __ASM_RSI_H_
#define __ASM_RSI_H_

#include <linux/errno.h>
#include <linux/jump_label.h>
#include <asm/rsi_cmds.h>

/*
 * RSI 初始化成功后创建的虚拟 platform_device 名称；同一字符串也进入
 * Arm CCA guest 驱动的 platform_device_id，用于匹配和模块自动加载。
 */
#define RSI_PDEV_NAME "arm-cca-dev"

/*
 * rsi_present 的定义和唯一 enable 点在 arch/arm64/kernel/rsi.c。
 * 声明为默认 false 的 static key，使非 Realm 热路径能被跳转标签消除。
 */
DECLARE_STATIC_KEY_FALSE(rsi_present);

/* ARM64 setup_arch() 调用的早期 RSI 探测/提交入口，成功状态通过 static key 发布。 */
void __init arm64_rsi_init(void);

/*
 * 查询物理字节区间是否全部为非 EMPTY RIPAS；返回值只描述 RMM backing
 * 属性，不授予页面引用，也不保证内容在 DESTROYED 状态下仍可信。
 */
bool arm64_rsi_is_protected(phys_addr_t base, size_t size);

/*
 * is_realm_world() - 低开销查询 RSI 是否完成全局初始化。
 *
 * 入参：无。任何上下文可调用，不持锁、不睡眠、无副作用。初始化结束前和
 * 非 Realm 平台返回 false；arm64_rsi_init() 最终提交后永久返回 true。
 * static_branch_unlikely() 让默认 false 路径只承担已打补丁的分支成本。
 */
static inline bool is_realm_world(void)
{
	return static_branch_unlikely(&rsi_present);
}

/*
 * rsi_set_memory_range() - 循环完成整个 IPA 半开区间的 RIPAS 转换。
 *
 * @start：起始 IPA，单位字节，应按 RSI_GRANULE_SIZE 对齐。
 * @end：排他末端，单位字节，应大于等于 start 并按 granule 对齐。
 * @state：目标 enum ripas。
 * @flags：DESTROYED 转换策略，传给每次 RSI_IPA_STATE_SET。
 *
 * rsi_set_addr_range_state() 允许 RMM 每次只处理前缀并用 top 返回进度。本
 * helper 反复从 top 继续，直到 start==end。成功返回 0；任何 RMM/Host 错误
 * 或 top 落在 [start,end] 之外均返回 -EINVAL。此前已成功的前缀不会回滚，
 * 因此调用者必须把失败视为范围状态可能部分改变。
 *
 * 函数通过同步 SMC 执行，不分配、不持 Linux 锁；调用者负责先断开冲突映射
 * 并保证转换期间范围 ownership 稳定。
 */
static inline int rsi_set_memory_range(phys_addr_t start, phys_addr_t end,
				       enum ripas state, unsigned long flags)
{
	/*
	 * ret 保存本次 RSI/Host 结果；top 是 RMM 实际推进的排他末端。二者共同
	 * 构成进度不变量：成功轮次必须令 start <= top <= end。
	 */
	unsigned long ret;
	phys_addr_t top;

	/* 空范围直接成功；非空范围按 RMM 每次接受的前缀持续推进。 */
	while (start != end) {
		ret = rsi_set_addr_range_state(start, end, state, flags, &top);
		/*
		 * ret 非零或 top 越界都使完整转换契约失败。top==start 虽未在此处
		 * 显式拒绝，但下一轮仍请求同一区间；协议要求成功响应必须推进。
		 */
		if (ret || top < start || top > end)
			return -EINVAL;
		start = top;
	}

	return 0;
}

/*
 * Convert the specified range to RAM. Do not use this if you rely on the
 * contents of a page that may already be in RAM state.
 */
/*
 * 把指定 [start,end) 转为 RIPAS_RAM。该普通版本允许把 DESTROYED 重新变成
 * RAM，所以不能在调用者依赖页中旧内容时使用；典型 private/shared 转换会
 * 丢弃或重新初始化旧内容。参数单位、对齐、部分失败语义与
 * rsi_set_memory_range() 相同，成功返回 0，失败返回 -EINVAL。
 */
static inline int rsi_set_memory_range_protected(phys_addr_t start,
						 phys_addr_t end)
{
	return rsi_set_memory_range(start, end, RSI_RIPAS_RAM,
				    RSI_CHANGE_DESTROYED);
}

/*
 * Convert the specified range to RAM. Do not convert any pages that may have
 * been DESTROYED, without our permission.
 */
/*
 * 同样把范围转为 RIPAS_RAM，但禁止转换未经 Realm 许可而 DESTROYED 的页。
 * 启动期接纳 memblock RAM 使用该安全版本，避免把 Host 破坏的内容重新标成
 * 可信 RAM；遇到此类页时范围转换失败，由上层选择 panic。
 */
static inline int rsi_set_memory_range_protected_safe(phys_addr_t start,
						      phys_addr_t end)
{
	return rsi_set_memory_range(start, end, RSI_RIPAS_RAM,
				    RSI_NO_CHANGE_DESTROYED);
}

/*
 * rsi_set_memory_range_shared() - 把 [start,end) 转为 RSI_RIPAS_EMPTY。
 *
 * EMPTY 使范围可通过设置 top IPA/NS_SHARED 位交给 Host 或普通设备访问。
 * 此转换允许覆盖 DESTROYED，因为共享路径不再信任原 protected 内容。成功
 * 返回 0；失败可能已有前缀完成转换，调用者必须保持映射 invalid 并避免重用。
 */
static inline int rsi_set_memory_range_shared(phys_addr_t start,
					      phys_addr_t end)
{
	return rsi_set_memory_range(start, end, RSI_RIPAS_EMPTY,
				    RSI_CHANGE_DESTROYED);
}
#endif /* __ASM_RSI_H_ */
