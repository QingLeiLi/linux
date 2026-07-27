/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM64 机密计算内存 private/shared 抽象接口学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 通用 DMA 和内存代码使用 set_memory_encrypted()/decrypted()，而具体环境
 * 通过 arm64_mem_crypt_ops 注册唯一后端。Arm CCA Realm 后端在 pageattr.c
 * 实现 PTE invalid -> RSI RIPAS 转换 -> PTE valid 三阶段协议。
 *
 * “encrypted”在 ARM64 CCA 上表示 canonical/protected IPA，不一定是某条
 * CPU 指令直接加密；“decrypted”表示设置 Realm IPA 的 top bit，访问 Host
 * 可见的 Non-Secure/shared 别名。接口不转移 struct page ownership，调用者
 * 必须保证范围无人以冲突属性并发访问。
 */
#ifndef __ASM_MEM_ENCRYPT_H
#define __ASM_MEM_ENCRYPT_H

#include <asm/rsi.h>

/* 这里只需声明设备指针类型；force_dma_unencrypted() 不解引用或持有设备。 */
struct device;

/*
 * struct arm64_mem_crypt_ops - ARM64 内存可见性转换后端操作表。
 *
 * 注册层长期借用一个静态 const 实例，不取得模块引用且没有注销接口。
 * encrypt/decrypt 都接收页对齐 linear-map 虚拟地址和页数，返回 0 或后端
 * errno；回调负责页表、TLB、RIPAS/固件调用及失败后的安全状态。
 */
struct arm64_mem_crypt_ops {
	/* 把 Host-shared 范围恢复为 canonical private/protected。 */
	int (*encrypt)(unsigned long addr, int numpages);
	/* 把 protected 范围转换为 Host/device 可见 shared。 */
	int (*decrypt)(unsigned long addr, int numpages);
};

/*
 * 启动期注册唯一后端；ops 是长期借用指针，成功返回 0，重复注册返回
 * -EBUSY。声明与实现位于 mem_encrypt.c。
 */
int arm64_mem_crypt_ops_register(const struct arm64_mem_crypt_ops *ops);

/*
 * 通用转换入口：addr 为页对齐内核虚拟地址，numpages 单位为页；无后端时
 * 成功空操作，有后端时透传回调结果。调用方仍拥有页面。
 */
int set_memory_encrypted(unsigned long addr, int numpages);
int set_memory_decrypted(unsigned long addr, int numpages);

/* 将 pageattr.c 的静态 Realm 操作表注册到上述唯一分发槽。 */
int realm_register_memory_enc_ops(void);

/*
 * force_dma_unencrypted() - 判断 DMA 是否必须使用 Host 可见共享内存。
 *
 * @dev：借用的设备指针，可为 NULL；当前 Realm 策略与具体设备无关，因此
 * 不解引用。任何上下文可调用，无锁、不睡眠。在 Realm 中返回 true，强制
 * DMA 路径借助 SWIOTLB/private-shared 转换；普通世界返回 false。
 */
static inline bool force_dma_unencrypted(struct device *dev)
{
	return is_realm_world();
}

/*
 * For Arm CCA guests, canonical addresses are "encrypted", so no changes
 * required for dma_addr_encrypted().
 * The unencrypted DMA buffers must be accessed via the unprotected IPA,
 * "top IPA bit" set.
 */
/*
 * 对 Arm CCA guest，canonical 地址天然代表 protected（这里称 encrypted），
 * 所以 dma_addr_encrypted() 不必改地址。共享 DMA buffer 必须通过设置
 * 未保护 IPA 的最高位来访问，PROT_NS_SHARED 正是启动期从 Realm ipa_bits
 * 推导出的该位编码。
 */
#define dma_addr_unencrypted(x)		((x) | PROT_NS_SHARED)

/* Clear the "top" IPA bit while converting back */
/*
 * 从 DMA/shared 地址恢复 canonical IPA 时清除 top bit；这只是地址编码转换，
 * 不会自行改变 RMM RIPAS，也不能替代 set_memory_encrypted() 的状态协议。
 */
#define dma_addr_canonical(x)		((x) & ~PROT_NS_SHARED)

#endif	/* __ASM_MEM_ENCRYPT_H */
