// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 机密计算内存加密状态分发层学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 通用调用者只认识 set_memory_{en,de}crypted()，具体 pKVM/CCA 环境通过
 * arm64_mem_crypt_ops 注册唯一后端。这里不维护逐页状态或锁；注册必须
 * 在初始化期、并发调用开始前完成，之后指针只读。无后端时成功空操作，
 * 使普通平台无需条件编译；收益是统一 API，代价是错误注册只能告警拒绝。
 */
/*
 * Implementation of the memory encryption/decryption API.
 *
 * Since the low-level details of the operation depend on the
 * Confidential Computing environment (e.g. pKVM, CCA, ...), this just
 * acts as a top-level dispatcher to whatever hooks may have been
 * registered.
 *
 * Author: Will Deacon <will@kernel.org>
 * Copyright (C) 2024 Google LLC
 *
 * "Hello, boils and ghouls!"
 */
/*
 * 上游说明强调本文件只是顶层 dispatcher：加密意味着改变 host/guest
 * 对页面的可见性或归属，具体缓存、页表和 hypercall 顺序由注册后端负责。
 */

#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/err.h>
#include <linux/mm.h>

#include <asm/mem_encrypt.h>

/* 注册成功后长期借用后端静态操作表；本文件不拥有也不释放它。 */
static const struct arm64_mem_crypt_ops *crypt_ops;

/*
 * 注册唯一内存加密后端。ops 必须非 NULL、生命周期覆盖整个运行期，且
 * encrypt/decrypt 回调可按接口约定调用；成功返回 0 并发布指针，重复
 * 注册告警并返回 -EBUSY，原后端保持不变。调用方必须在并发服务前串行注册。
 */
int arm64_mem_crypt_ops_register(const struct arm64_mem_crypt_ops *ops)
{
	if (WARN_ON(crypt_ops))
		return -EBUSY;

	crypt_ops = ops;
	return 0;
}

/*
 * 将 addr 起始的 numpages 个内核映射页交给后端切为“加密/私有”状态。
 * addr 必须 PAGE_SIZE 对齐，numpages 单位为页；无后端或地址未对齐时
 * 返回 0（后者同时告警），否则透传后端返回码。接口不分配页、不转移
 * struct page 所有权，调用者负责确保范围稳定且无人以冲突属性访问。
 */
int set_memory_encrypted(unsigned long addr, int numpages)
{
	if (likely(!crypt_ops) || WARN_ON(!PAGE_ALIGNED(addr)))
		return 0;

	return crypt_ops->encrypt(addr, numpages);
}
EXPORT_SYMBOL_GPL(set_memory_encrypted);

/*
 * set_memory_encrypted() 的逆向状态转换：把范围切为解密/共享状态。
 * 参数、空后端和对齐语义相同；实际 cache/TLB 同步及失败后的部分转换
 * 语义完全由 decrypt 后端契约决定，本薄层不尝试二次回滚。
 */
int set_memory_decrypted(unsigned long addr, int numpages)
{
	if (likely(!crypt_ops) || WARN_ON(!PAGE_ALIGNED(addr)))
		return 0;

	return crypt_ops->decrypt(addr, numpages);
}
EXPORT_SYMBOL_GPL(set_memory_decrypted);
