/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64 Image header ABI 学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 * 本文件定义内核 Image 前 64 字节的稳定装载 ABI。head.S/efi-header.S 在构建期
 * 发射该布局，boot loader 和分析工具在内核运行前读取它；字段是小端磁盘格式，
 * 不存在运行期锁、引用或释放。任何偏移、宽度或 flag 编码变化都属于外部 ABI 变化。
 */

#ifndef __ASM_IMAGE_H
#define __ASM_IMAGE_H

#define ARM64_IMAGE_MAGIC	"ARM\x64"
/* header 固定魔数，加载器用它排除非 arm64 Image；并不替代签名或完整性验证。 */

/* flags 位图分三组：端序 1 位、页大小 2 位、物理装载基址语义 1 位。 */
#define ARM64_IMAGE_FLAG_BE_SHIFT		0
#define ARM64_IMAGE_FLAG_PAGE_SIZE_SHIFT	(ARM64_IMAGE_FLAG_BE_SHIFT + 1)
#define ARM64_IMAGE_FLAG_PHYS_BASE_SHIFT \
					(ARM64_IMAGE_FLAG_PAGE_SIZE_SHIFT + 2)
#define ARM64_IMAGE_FLAG_BE_MASK		0x1
#define ARM64_IMAGE_FLAG_PAGE_SIZE_MASK		0x3
#define ARM64_IMAGE_FLAG_PHYS_BASE_MASK		0x1

#define ARM64_IMAGE_FLAG_LE			0
#define ARM64_IMAGE_FLAG_BE			1
#define ARM64_IMAGE_FLAG_PAGE_SIZE_4K		1
#define ARM64_IMAGE_FLAG_PAGE_SIZE_16K		2
#define ARM64_IMAGE_FLAG_PAGE_SIZE_64K		3
#define ARM64_IMAGE_FLAG_PHYS_BASE		1
/* 页大小编码 0 保留，1/2/3 分别表示 4K/16K/64K；PHYS_BASE=1 表示当前基址语义。 */

#ifndef __ASSEMBLER__

#define arm64_image_flag_field(flags, field) \
				(((flags) >> field##_SHIFT) & field##_MASK)
/* 传入完整 flags 和字段名前缀，宏拼接 SHIFT/MASK 后返回已右移的无符号字段值。 */

/*
 * struct arm64_image_header - arm64 kernel image header
 * See Documentation/arch/arm64/booting.rst for details
 *
 * @code0:		Executable code, or
 *   @mz_header		  alternatively used for part of MZ header
 * @code1:		Executable code
 * @text_offset:	Image load offset
 * @image_size:		Effective Image size
 * @flags:		kernel flags
 * @reserved:		reserved
 * @magic:		Magic number
 * @reserved5:		reserved, or
 *   @pe_header:	  alternatively used for PE COFF offset
 */
/*
 * struct arm64_image_header 精确对应 Image 开头的 64 字节快照，由链接期生成、加载器
 * 借用读取，不在运行期创建或销毁。code0/code1 通常是可执行入口指令；EFI 构建把
 * code0 的部分字节复用作 MZ。text_offset 给传统协议的装载偏移，image_size 是有效
 * 镜像范围，flags 携带端序/页大小/物理基址语义；res2..4 保留为零；magic 识别 ABI；
 * res5 在 EFI 镜像中复用为 PE/COFF header 偏移。所有字段用 __le 类型固定外部字节序。
 */

struct arm64_image_header {
	__le32 code0;
	__le32 code1;
	__le64 text_offset;
	__le64 image_size;
	__le64 flags;
	__le64 res2;
	__le64 res3;
	__le64 res4;
	__le32 magic;
	__le32 res5;
};

#endif /* __ASSEMBLER__ */

#endif /* __ASM_IMAGE_H */
