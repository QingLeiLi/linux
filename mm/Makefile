# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the linux memory manager.
#
# Linux 内存管理目录的 Kbuild 入口：把配置符号映射为目标文件或子目录，
# 最终链接进 built-in.a；这里的条件只决定“是否编译”，运行期策略仍由源码决定。

# slab 分配器和 kmemleak 自身参与内存检测基础设施；若再由 KASAN/KCSAN
# 插桩，容易形成递归检测或把检测器的内部元数据访问误报为被测对象访问。
KASAN_SANITIZE_slab_common.o := n
KASAN_SANITIZE_slub.o := n
KASAN_SANITIZE_kmemleak.o := n
KCSAN_SANITIZE_kmemleak.o := n

# These produce frequent data race reports: most of them are due to races on
# the same word but accesses to different bits of that word. Re-enable KCSAN
# for these when we have more consensus on what to do about them.
# 这些文件频繁触发数据竞争报告，多数是并发访问同一机器字中的不同位，
# 并非同一逻辑字段发生冲突；在如何精确建模这种位级并发形成共识前关闭插桩。
# 这只是降低 KCSAN 噪声，不表示这些核心分配路径天然没有并发风险。
KCSAN_SANITIZE_slab_common.o := n
KCSAN_SANITIZE_slub.o := n
KCSAN_SANITIZE_page_alloc.o := n
# But enable explicit instrumentation for memory barriers.
# 即使关闭普通访问插桩，仍显式检查内存屏障，让发布/观察顺序错误保持可见。
KCSAN_INSTRUMENT_BARRIERS := y

# These files are disabled because they produce non-interesting and/or
# flaky coverage that is not a function of syscall inputs. E.g. slab is out of
# free pages, or a task is migrated between nodes.
# 下列路径的覆盖变化常由系统全局状态驱动，例如 slab 缺页或任务跨 NUMA 节点
# 迁移，而非 fuzz 输入本身；关闭 KCOV 可避免把不可复现的覆盖反馈给模糊测试器。
KCOV_INSTRUMENT_slab_common.o := n
KCOV_INSTRUMENT_slub.o := n
KCOV_INSTRUMENT_page_alloc.o := n
KCOV_INSTRUMENT_debug-pagealloc.o := n
KCOV_INSTRUMENT_kmemleak.o := n
KCOV_INSTRUMENT_memcontrol.o := n
KCOV_INSTRUMENT_memcontrol-v1.o := n
KCOV_INSTRUMENT_mmzone.o := n
KCOV_INSTRUMENT_vmstat.o := n
KCOV_INSTRUMENT_failslab.o := n

CFLAGS_init-mm.o += -Wno-override-init

# mmu-y 先以 NOMMU 实现为默认值；CONFIG_MMU=y 时目标专属赋值覆盖它，选择
# 页表、mmap、GUP/rmap 与 vmalloc 等完整虚拟内存实现。$(mmu-y) 稍后只展开
# 一次，因此同一个顶层 obj-y 清单可服务 MMU 与 NOMMU 架构。
mmu-y			:= nommu.o
mmu-$(CONFIG_MMU)	:= highmem.o memory.o mincore.o \
			   mlock.o mmap.o mmu_gather.o mprotect.o mremap.o \
			   msync.o page_vma_mapped.o pagewalk.o \
			   pgtable-generic.o rmap.o vmalloc.o vma.o vma_exec.o


ifdef CONFIG_CROSS_MEMORY_ATTACH
# process_vm_{readv,writev} 既需要 MMU，也需要系统调用功能开关；双重门控避免
# NOMMU 架构或未暴露 ABI 的配置携带不可达实现。
mmu-$(CONFIG_MMU)	+= process_vm_access.o
endif

ifdef CONFIG_64BIT
# mseal 的 VMA 密封 ABI 当前仅在 64 位 MMU 构建中进入核心对象集合。
mmu-$(CONFIG_MMU)	+= mseal.o
endif

# 无条件内存管理核心：页缓存、回写、回收、slab、公用初始化和统计等对象始终
# 链入内核；末尾的 $(mmu-y) 在构建期接入当前体系结构的虚拟内存实现族。
obj-y			:= filemap.o mempool.o oom_kill.o fadvise.o \
			   maccess.o page-writeback.o folio-compat.o \
			   readahead.o swap.o truncate.o vmscan.o shrinker.o \
			   shmem.o util.o mmzone.o vmstat.o backing-dev.o \
			   mm_init.o percpu.o slab_common.o \
			   compaction.o show_mem.o \
			   interval_tree.o list_lru.o workingset.o \
			   debug.o gup.o mmap_lock.o vma_init.o $(mmu-y)

# Give 'page_alloc' its own module-parameter namespace
# 让 page_alloc 使用独立复合对象名，模块参数因此归入 page_alloc 命名空间；
# SHUFFLE_PAGE_ALLOCATOR 打开时 shuffle.o 与 page_alloc.o 一起链接为 page-alloc.o。
page-alloc-y := page_alloc.o
page-alloc-$(CONFIG_SHUFFLE_PAGE_ALLOCATOR) += shuffle.o

# Give 'memory_hotplug' its own module-parameter namespace
# memory_hotplug 同样使用独立复合目标；配置关闭时变量为空，顶层加入空列表。
memory-hotplug-$(CONFIG_MEMORY_HOTPLUG) += memory_hotplug.o

# 这些对象不依赖可选用户功能：复合 page allocator、页片缓存、早期 mm/memblock、
# 可选 hotplug 复合目标以及 SLUB 基础实现均进入 mm/built-in.a。
obj-y += page-alloc.o
obj-y += page_frag_cache.o
obj-y += init-mm.o
obj-y += memblock.o
obj-y += $(memory-hotplug-y)
obj-y += slub.o

ifdef CONFIG_MMU
	# madvise 系统调用必须同时具备 MMU 和 ADVISE_SYSCALLS，外层条件先排除
	# NOMMU；缩进仅表达 Kbuild 可读性，不改变 obj-* 变量语义。
	obj-$(CONFIG_ADVISE_SYSCALLS)	+= madvise.o
endif

# 以下 obj-$(CONFIG_*) 是 Kbuild 的配置选择器：值为 y 时对象内建，为 m 时
# （允许模块的符号）进入模块，为空/n 时完全跳过。每组按其上层能力归类。
# 交换与压缩交换后端。
obj-$(CONFIG_SWAP)	+= page_io.o swap_state.o swapfile.o
obj-$(CONFIG_ZSWAP)	+= zswap.o
obj-$(CONFIG_HAS_DMA)	+= dmapool.o
# HugeTLB 主实现、用户接口与 CMA 支撑；CMA 文件还要由外层 CONFIG_CMA 约束。
obj-$(CONFIG_HUGETLBFS)	+= hugetlb.o hugetlb_sysfs.o hugetlb_sysctl.o
ifdef CONFIG_CMA
obj-$(CONFIG_HUGETLBFS)	+= hugetlb_cma.o
endif
# 物理/虚拟内存模型、页迁移、KSM 及内存检测器子目录。
obj-$(CONFIG_HUGETLB_PAGE_OPTIMIZE_VMEMMAP)	+= hugetlb_vmemmap.o
obj-$(CONFIG_NUMA) 	+= mempolicy.o
obj-$(CONFIG_SPARSEMEM)	+= sparse.o
obj-$(CONFIG_SPARSEMEM_VMEMMAP) += sparse-vmemmap.o
obj-$(CONFIG_MMU_NOTIFIER) += mmu_notifier.o
obj-$(CONFIG_KSM) += ksm.o
obj-$(CONFIG_PAGE_POISONING) += page_poison.o
obj-$(CONFIG_KASAN)	+= kasan/
obj-$(CONFIG_KFENCE) += kfence/
obj-$(CONFIG_KMSAN)	+= kmsan/
# 故障注入与启动期内存测试只在相应调试配置下加入，不污染普通构建路径。
obj-$(CONFIG_FAILSLAB) += failslab.o
obj-$(CONFIG_FAIL_PAGE_ALLOC) += fail_page_alloc.o
obj-$(CONFIG_MEMTEST)		+= memtest.o
obj-$(CONFIG_MIGRATION) += migrate.o
# NUMA 层级、设备页迁移和透明大页依赖迁移/拓扑能力，由 Kconfig 保证前置关系。
obj-$(CONFIG_NUMA) += memory-tiers.o
obj-$(CONFIG_DEVICE_MIGRATION) += migrate_device.o
obj-$(CONFIG_TRANSPARENT_HUGEPAGE) += huge_memory.o khugepaged.o
obj-$(CONFIG_PAGE_COUNTER) += page_counter.o
# memcg v1 与通用 memcg 分开编译，使关闭旧接口时不携带其实现；BPF 控制路径
# 还需 BPF_SYSCALL，故用嵌套条件表达二者的交集。
obj-$(CONFIG_LIVEUPDATE_MEMFD) += memfd_luo.o
obj-$(CONFIG_MEMCG_V1) += memcontrol-v1.o
obj-$(CONFIG_MEMCG) += memcontrol.o vmpressure.o
ifdef CONFIG_BPF_SYSCALL
obj-$(CONFIG_MEMCG) += bpf_memcontrol.o
endif
# 测试、硬件错误注入和内存调试工具：这些对象提供 debugfs/sysfs 或测试入口，
# 不属于正常分配主路径，只有显式配置后才链接。
obj-$(CONFIG_CGROUP_HUGETLB) += hugetlb_cgroup.o
obj-$(CONFIG_GUP_TEST) += gup_test.o
obj-$(CONFIG_DMAPOOL_TEST) += dmapool_test.o
obj-$(CONFIG_MEMORY_FAILURE) += memory-failure.o
obj-$(CONFIG_HWPOISON_INJECT) += hwpoison-inject.o
obj-$(CONFIG_DEBUG_KMEMLEAK) += kmemleak.o
obj-$(CONFIG_DEBUG_RODATA_TEST) += rodata_test.o
obj-$(CONFIG_DEBUG_VM_PGTABLE) += debug_vm_pgtable.o
obj-$(CONFIG_PAGE_OWNER) += page_owner.o
# 隔离、连续内存、NUMA 模拟与 balloon 等对象改变页的可迁移/可分配状态；其
# 运行期协调仍依赖 pageblock、迁移锁与 notifier，本文件只建立链接集合。
obj-$(CONFIG_MEMORY_ISOLATION) += page_isolation.o
obj-$(CONFIG_ZSMALLOC)	+= zsmalloc.o
obj-$(CONFIG_GENERIC_EARLY_IOREMAP) += early_ioremap.o
obj-$(CONFIG_CMA)	+= cma.o
obj-$(CONFIG_NUMA) += numa.o
obj-$(CONFIG_NUMA_MEMBLKS) += numa_memblks.o
obj-$(CONFIG_NUMA_EMU) += numa_emulation.o
obj-$(CONFIG_BALLOON) += balloon.o
obj-$(CONFIG_PAGE_EXTENSION) += page_ext.o
obj-$(CONFIG_PAGE_TABLE_CHECK) += page_table_check.o
# CMA 的调试与 sysfs 前端分别门控，基础 cma.o 不会因此自动暴露诊断接口。
obj-$(CONFIG_CMA_DEBUGFS) += cma_debug.o
obj-$(CONFIG_SECRETMEM) += secretmem.o
obj-$(CONFIG_CMA_SYSFS) += cma_sysfs.o
obj-$(CONFIG_USERFAULTFD) += userfaultfd.o
obj-$(CONFIG_IDLE_PAGE_TRACKING) += page_idle.o
# 调试页分配/引用跟踪与 DAMON 监控作为独立实现或子目录接入；目录尾斜杠让
# Kbuild 继续读取子目录 Makefile，而不是寻找同名目标文件。
obj-$(CONFIG_DEBUG_PAGEALLOC) += debug_page_alloc.o
obj-$(CONFIG_DEBUG_PAGE_REF) += debug_page_ref.o
obj-$(CONFIG_DAMON) += damon/
obj-$(CONFIG_HARDENED_USERCOPY) += usercopy.o
obj-$(CONFIG_PERCPU_STATS) += percpu-stats.o
obj-$(CONFIG_ZONE_DEVICE) += memremap.o
# 设备内存镜像、memfd、页表转储及页面上报等接口各自服从 Kconfig 能力开关。
obj-$(CONFIG_HMM_MIRROR) += hmm.o
obj-$(CONFIG_MEMFD_CREATE) += memfd.o
obj-$(CONFIG_MAPPING_DIRTY_HELPERS) += mapping_dirty_helpers.o
obj-$(CONFIG_PTDUMP) += ptdump.o
obj-$(CONFIG_PAGE_REPORTING) += page_reporting.o
obj-$(CONFIG_HAVE_BOOTMEM_INFO_NODE) += bootmem_info.o
obj-$(CONFIG_GENERIC_IOREMAP) += ioremap.o
obj-$(CONFIG_SHRINKER_DEBUG) += shrinker_debug.o
obj-$(CONFIG_EXECMEM) += execmem.o
obj-$(CONFIG_TMPFS_QUOTA) += shmem_quota.o
# KUnit 目标只在测试配置开启时加入；其路径仍链接进内核测试映像，不是用户态
# selftest。源文件位于 tests/ 子目录，但无需单独递归 Makefile。
obj-$(CONFIG_LAZY_MMU_MODE_KUNIT_TEST) += tests/lazy_mmu_mode_kunit.o
