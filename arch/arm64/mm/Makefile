# SPDX-License-Identifier: GPL-2.0
#
# 中文学习注释模型：OpenAI Codex（GPT-5）。
#
# 本目录把 arm64 虚拟内存实现分成四类对象：
#   1. 常驻主链：fault/init/mmu/context/proc 负责异常、启动建表、地址空间切换；
#   2. 数据与缓存：cache/copypage/flush/dma-mapping 维护 CPU 与设备可见性；
#   3. 映射工具：ioremap/mmap/pgd/pageattr/fixmap 提供具体映射和权限变更；
#   4. 配置特性：CONTPTE、hugetlb、KASAN、MTE、GCS、页表转换按 Kconfig 加入。
#
# obj-y 无条件链接进 arm64 内核；obj-$(CONFIG_*) 只在对应能力启用时参与构建。
# 汇编与同名 C 文件可共同组成一个子系统，例如 trans_pgd.o 与
# trans_pgd-asm.o 分别承载页表转换的控制逻辑和切换期间的低级原语。
obj-y				:= dma-mapping.o extable.o fault.o init.o \
				   cache.o copypage.o flush.o \
				   ioremap.o mmap.o pgd.o mem_encrypt.o mmu.o \
				   context.o proc.o pageattr.o fixmap.o
obj-$(CONFIG_ARM64_CONTPTE)	+= contpte.o
obj-$(CONFIG_HUGETLB_PAGE)	+= hugetlbpage.o
obj-$(CONFIG_PTDUMP)		+= ptdump.o
obj-$(CONFIG_PTDUMP_DEBUGFS)	+= ptdump_debugfs.o
obj-$(CONFIG_TRANS_TABLE)	+= trans_pgd.o
obj-$(CONFIG_TRANS_TABLE)	+= trans_pgd-asm.o
obj-$(CONFIG_DEBUG_VIRTUAL)	+= physaddr.o
obj-$(CONFIG_ARM64_MTE)		+= mteswap.o
obj-$(CONFIG_ARM64_GCS)		+= gcs.o
KASAN_SANITIZE_physaddr.o	+= n

obj-$(CONFIG_KASAN)		+= kasan_init.o
KASAN_SANITIZE_kasan_init.o	:= n
