# mm 学习注释进度

规则：单文件按 `doc/linux-kernel-source-learning-methodology.md` 第 17 章闭环；只加注释；`[~]` 未闭环时不开始下一文件。恢复任务先读本文件和 `todo/mm-learning-comments-baseline.md`，目标未变更时复用探查结论。

## 筛选

- 2026-08-27：扫描 190 个 `.c/.h`；密度门禁通过 0，未通过 190。
- 暂不重复：已有中文学习注释的 18 个文件（后续仅复审，不作为首轮目标）。
  `backing-dev.c`、`early_ioremap.c`、`filemap.c`、`hugetlb.c`、`hugetlb_cma.c`、`init-mm.c`、
  `memcontrol.c`、`mempolicy.c`、`mm_init.c`、`rmap.c`、`slab_common.c`、`slub.c`、`vmalloc.c`、
  `vmstat.c`、`kasan/hw_tags.c`、`kasan/sw_tags.c`、`kfence/core.c`、`kfence/report.c`。

## 进度

- [~] 当前文件：`mm/memory-tiers.c`

## 固定顺序

1. 闭环 `mm/gup_test.h`。
2. 构建入口：`mm/Makefile` → `mm/Kconfig` → `mm/Kconfig.debug` → `damon/{Makefile,Kconfig}` → `kasan/Makefile` → `kfence/Makefile` → `kmsan/Makefile`。
3. 按基线表序号 2 起处理零中文 `.c/.h`；最后复审标记 `r` 的 18 个文件。
4. 排除 `.DS_Store`、两份 `.kunitconfig`；Rust 测试不套用 C/H 方法论，另行定标。

## 已完成

- [x] `mm/damon/ops-common.h`：13/111/8.538/2（code/中文/density/max_gap）；仅新增 152 行注释。
- [x] `mm/damon/modules-common.c`：24/38/1.583/6；仅新增 59 行注释。
- [x] `mm/gup_test.h`：32/41/1.281/8；仅新增 63 行注释；第 17 章验收、diff/checkpatch 通过。
- [x] `mm/Makefile`：仅新增 44 行注释；Kbuild 条件、复合目标和子目录入口已覆盖，GNU Make 解析通过。
- [x] `mm/Kconfig`：仅新增 250 行注释；全部 config/menuconfig 实体附近均有中文语义，Kconfig allnoconfig 解析通过。
- [x] `mm/Kconfig.debug`：仅新增 45 行注释；全部 config 实体附近均有中文语义，Kconfig allnoconfig 解析通过。
- [x] `mm/damon/Makefile`：仅新增 7 行注释；共享对象与配置组合关系已覆盖，GNU Make 解析通过。
- [x] `mm/damon/Kconfig`：仅新增 26 行注释；全部配置实体均有中文语义，Kconfig allnoconfig 解析通过。
- [x] `mm/kasan/Makefile`：仅新增 21 行注释；运行时递归防护、模式对象和 KUnit 复合目标已覆盖，GNU Make 解析通过。
- [x] `mm/kfence/Makefile`：仅新增 4 行注释；context analysis、核心对象和测试栈约束已覆盖，GNU Make 解析通过。
- [x] `mm/kmsan/Makefile`：仅新增 10 行注释；运行时递归防护、对象 flags 和测试反向插桩已覆盖，GNU Make 解析通过。
- [x] `mm/page_reporting.h`：33/56/1.697/8；仅新增 82 行注释；第 17 章验收、diff/checkpatch 通过。
- [x] `mm/rodata_test.c`：33/29/0.879/8；仅新增 46 行注释；启动期 rodata 内容、写保护与页边界四阶段自检已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/damon/sysfs-common.h`：38/92/2.421/5；仅新增 126 行注释；共享锁、两类 kobject 生命周期及 sysfs/core 转换接口已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kasan/report_hw_tags.c`：38/54/1.421/8；仅新增 72 行注释；HW_TAGS 坏地址、slab 大小、metadata 行与标签输出契约已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/mm_slot.h`：40/49/1.225/8；仅新增 61 行注释；KSM/khugepaged 的哈希/扫描双索引、mm 引用与锁边界已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/debug_page_alloc.c`：40/53/1.325/5；仅新增 71 行注释；启动参数、static key 与 buddy guard page 设置/合并协议已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/pgalloc-track.h`：42/49/1.167/5；仅新增 57 行注释；四级页表分配、修改位累积与架构同步边界已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/hugetlb_cma.h`：44/84/1.909/6；仅新增 114 行注释；运行期 frozen folio、启动期 CMA bootmem、exclusive 策略及无 CMA 桩已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/shuffle.h`：44/70/1.591/6；仅新增 96 行注释；static key 门控、node/zone 洗牌、随机插入及关闭配置桩已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/vma_internal.h`：44/16/0.364/10；仅新增 31 行注释；vma.c 可替换依赖边界、include 分组、maple tree/mmap_lock 与 TLB 协议已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/bootmem_info.c`：46/49/1.065/6；仅新增 65 行注释；启动页 private 编码、额外引用、section 去重登记与最终归还 buddy 已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/debug_page_ref.c`：46/38/0.826/6；仅新增 54 行注释；七类 page_ref trace 包装的事后观测、参数语义与 ownership 边界已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/numa.c`：46/41/0.891/9；仅新增 55 行注释；在线/离线 NODE_DATA 启动分配、跨节点退化和两个 node-0 回退桩已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/ioremap.c`：52/41/0.788/7；仅新增 56 行注释；物理区间校验、页对齐、VM_IOREMAP 分配、页表失败回滚与 iounmap 配对已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kasan/report_tags.c`：52/47/0.904/5；仅新增 64 行注释；通用 bug 分类、stack ring 写锁快照、对象生命周期边界和尽力诊断限制已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/tests/lazy_mmu_mode_kunit.c`：52/34/0.654/9；仅新增 48 行注释；enable/disable 与 pause/resume 嵌套状态、KUnit 期望及 suite 生命周期已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/fail_page_alloc.c`：53/34/0.642/10；仅新增 44 行注释；GFP/阶数过滤、FAULT_NOWARN、通用注入器副作用与 debugfs 控制面已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/failslab.c`：53/34/0.642/7；仅新增 46 行注释；bootstrap cache、NOFAIL/reclaim/cache 过滤、-ENOMEM 契约与 debugfs 错误边界已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kasan/report_sw_tags.c`：61/51/0.836/9；仅新增 69 行注释；首坏 granule、shadow 分配大小、metadata 行、标签与任务栈报告已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/hugetlb_internal.h`：67/84/1.254/10；仅新增 124 行注释；HugeTLB NUMA 轮转、池出入、批量发布、demote 错误与 sysfs/sysctl 配置契约已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/cma.h`：68/65/0.956/8；仅新增 88 行注释；多区间 union 生命周期、bitmap/锁、状态位、sysfs 计账及关闭配置桩已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/folio-compat.c`：68/65/0.956/6；仅新增 89 行注释；11 个 page-to-folio 兼容包装的引用、锁、dirty/writeback、page-cache 发布与错误折叠已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/hugetlb_vmemmap.h`：68/76/1.118/7；仅新增 119 行注释；HVO 优化/恢复、批量链表、pre-HVO 初始化、容量判断及关闭配置行为已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kfence/kfence.h`：73/75/1.027/7；仅新增 101 行注释；canary、metadata 状态/锁/RCU、地址映射、错误分类与报告处置边界已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/mmzone.c`：75/97/1.293/8；仅新增 128 行注释；online node/zone 双层遍历、zonelist 策略过滤、lruvec 初始化与 CPUPID 原子交换已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/percpu-km.c`：76/81/1.066/5；仅新增 101 行注释；NOMMU 连续 chunk、整体 backing ownership、创建回滚、发布计数及禁止部分回收已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/msync.c`：77/45/0.584/8；仅新增 56 行注释；参数校验、VMA 洞延迟错误、共享文件同步、解锁 I/O 与游标重建已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/page_poison.c`：79/71/0.899/8；仅新增 90 行注释；static key、free/alloc poison 配对、KASAN 局部映射、限速损坏诊断及架构空桩已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/vma_exec.c`：79/60/0.759/6；仅新增 80 行注释；exec 临时栈创建/KSM 回滚、VMA 扩缩、页表搬移与 TLB 旧范围释放已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/mseal.c`：82/69/0.841/8；仅新增 89 行注释；无洞预检、不可逆密封、VMA split/merge、幂等与部分成功边界已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/interval_tree.c`：84/72/0.857/7；仅新增 101 行注释；file/anon 两类增强红黑区间树、同起点插入、端点快照与锁内借用迭代已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/bpf_memcontrol.c`：86/80/0.930/10；仅新增 102 行注释；BPF kfunc verifier 合约、memcg 引用、跨 controller RCU 查找、统计哨兵及 sleepable flush 已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/damon/tests/sysfs-kunit.h`：86/48/0.558/9；仅新增 60 行注释与空行；sysfs/core target 追加、PID 临时引用、KUnit skip 回滚及计数断言边界已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/memcontrol-v1.h`：90/195/2.167/8；仅新增 289 行注释与空行；v1/v2 遍历、stock 冲刷、local/total 统计、soft-limit/OOM、memsw/kmem/tcpmem 及关闭配置桩已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/zpdesc.h`：90/94/1.044/8；仅新增 135 行注释与空行；struct page 重叠布局、页类型/对象偏移共存、转换宏、folio 锁/引用/highmem 映射及迁移标记已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kmsan/kmsan.h`：98/132/1.347/9；仅新增 192 行注释与空行；shadow/origin 元数据、递归门禁、嵌套 IRQ/NMI 退避、origin 链、poison/check/report 与启动/vmalloc metadata 绑定已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/shuffle.c`：101/64/0.634/9；仅新增 93 行注释与空行；启动参数/static key、PFN 合法性、近似 Fisher-Yates、migratetype 链表不变量、zone 锁让出及无锁随机位缓存已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kasan/tags.c`：104/35/0.337/9；仅新增 55 行注释；启动参数、static key、memblock 栈环、并发槽仲裁与 Stack Depot 引用交接已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/damon/sysfs-common.c`：108/41/0.380/8；仅新增 54 行注释；共享互斥边界、range kobject 生命周期、端点属性与 memcg 迭代引用已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/page_frag_cache.c`：111/47/0.423/10；仅新增 69 行注释；编码页、refill 回退、引用偏置、pfmemalloc 淘汰与 backing 释放已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/memtest.c`：112/45/0.402/10；仅新增 67 行注释；花纹收尾、free-range 裁剪、坏段合并保留与 proc 区分语义已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/hugetlb_sysctl.c`：115/37/0.322/10；仅新增 50 行注释；默认 hstate、ctl_table 临时副本、mempolicy、surplus 上限与配置分支已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/vma_init.c`：115/98/0.852/9；仅新增 138 行注释与空行；VMA slab、浅拷贝、PFN 跟踪引用、配置桩及 detached 释放边界已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/cma_sysfs.c`：117/83/0.709/9；仅新增 120 行注释与空行；CMA 原子计账、只读属性、kobject 包装生命周期及启动失败回滚已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/dmapool_test.c`：122/76/0.623/9；仅新增 97 行注释；DMA 地址配对、批量失败回滚、参数化计时、伪设备与模块资源生命周期已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/hwpoison-inject.c`：139/94/0.676/10；仅新增 124 行注释；设备/flags/memcg 过滤、竞态复核、debugfs 权限、软件 poison/unpoison 与 RCU 注销已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/fadvise.c`：140/90/0.643/8；仅新增 123 行注释与空行；advice 分派、范围饱和、预读模式、完整页 DONTNEED、LRU 双阶段失效及原生/compat ABI 已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/maccess.c`：145/99/0.683/10；仅新增 133 行注释；内核/用户无故障复制、对齐宽度降级、部分成功、字符串 NUL 契约与 KMSAN/KASAN 插桩边界已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kmsan/init.c`：152/98/0.645/9；仅新增 134 行注释；早期范围合并、三块 data/shadow/origin 分组、held-back ownership、降阶回收与运行期开关发布已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/percpu-stats.c`：157/70/0.446/9；仅新增 92 行注释；锁外容量分配/锁内复核、bitmap 段重建、碎片排序、chunk 槽状态与 debugfs 快照生命周期已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/damon/tests/vaddr-kunit.h`：160/86/0.537/9；仅新增 109 行注释；maple tree fixture、两大空洞三分区、region 复用/裁剪/替换、skip 回滚与 KUnit suite 生命周期已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/page_idle.c`：160/83/0.519/9；仅新增 110 行注释与空行；在线 PFN/folio 引用、PTE/PMD/MMU notifier young 清理、reclaim 补偿、位图 ABI 与 sysfs 发布已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/percpu-internal.h`：162/115/0.710/10；仅新增 159 行注释；chunk/bitmap 分层元数据、槽链锁、MEMCG/profiling 扩展、计费换算与统计配置桩已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/cma_debug.c`：163/93/0.571/9；仅新增 127 行注释与空行；debugfs 快照、range bitmap、测试分配记录 ownership、部分释放与输入窄化边界已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/ptdump.c`：163/101/0.620/10；仅新增 127 行注释；页表层级回调、KASAN shared-shadow 折叠、hole/leaf、热插拔与 mmap 锁、W^X debugfs 已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/mapping_dirty_helpers.c`：166/115/0.693/9；仅新增 153 行注释；共享映射 PTE write-protect/dirty 收割、bitmap 保证、MMU notifier/cache/TLB 配对与 THP 边界已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/usercopy.c`：166/116/0.699/10；仅新增 154 行注释；栈帧/活动栈、kmap/vmalloc/slab/compound page、kernel text alias、致命失败与 static-key 启动策略已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kmsan/report.c`：171/110/0.643/10；仅新增 147 行注释；使用点栈、origin 创建/传播链、递归门禁、报告串行、uaccess/runtime 恢复与 panic 策略已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/balloon.c`：180/153/0.850/7；仅新增 191 行注释；inflate/deflate 链表与计账、PageOffline/private 发布、compaction 隔离/回放、设备迁移及 -ENOENT 退化已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/process_vm_access.c`：188/117/0.622/10；仅新增 142 行注释；iovec 导入、真实凭据权限、远端页分批 pin/copy/dirty-unpin、资源回滚与部分成功 ABI 已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/secretmem.c`：194/141/0.727/10；仅新增 180 行注释；direct-map/TLB 发布、一次性容量、强制锁页/禁 dump、GUP/迁移拒绝、释放擦除、伪文件系统与休眠门禁已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/page_counter.c`：200/165/0.825/9；仅新增 211 行注释；层级计费/回滚、max 屏障协议、min/low 差量汇总、限额解析、有效保护五规则与 top-down 计算已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/percpu-vm.c`：201/149/0.741/10；仅新增 189 行注释；NUMA backing 分配回滚、共享临时数组锁、逐页映射/反向索引、cache/TLB 配对及 chunk 回收判定已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kmsan/instrumentation.c`：213/102/0.479/10；仅新增 151 行注释；编译器 metadata ABI、固定宽度生成宏、asm 尽力反毒、内存 intrinsic 的 TLS 返回传播及 alloca/origin 生命周期已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/shrinker_debug.c`：212/72/0.340/10；仅新增 92 行注释与空行；memcg/NUMA 计数快照、主动 scan 输入与引用、IDA 目录发布、rename 回滚及 detach/remove 锁外清理已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/page_table_check.c`：213/86/0.404/10；仅新增 128 行注释；启动 static-key/page_ext 发布、匿名与文件映射互斥、huge leaf 跨度、uffd-wp flags 及 clear-before-set 账本协议已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/page_reporting.c`：215/105/0.488/10；仅新增 135 行注释；order 参数回退、三态请求合并、zone 锁内隔离/锁外 report/锁内 drain、尾批次及 RCU 注册注销已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/mincore.c`：223/170/0.762/7；仅新增 234 行注释；HugeTLB/THP/普通 PTE、swap/tmpfs/page-cache 补查、权限侧信道模糊、跨 VMA 分批及错误出口已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kmsan/shadow.c`：234/171/0.731/8；仅新增 218 行注释；三类地址 metadata 解析、dummy load/store、runtime 递归门禁、页分配/释放毒化、双区 vmap 与启动期三块绑定已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/page_vma_mapped.c`：235/126/0.536/10；仅新增 164 行注释；HugeTLB/THP、PMD/PTE migration、device-private softleaf、lockless 扫描锁后复核、跨页表边界及 memory-failure 同步查询已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/vmpressure.c`：236/166/0.703/9；仅新增 211 行注释；窗口/阈值、优先级兜底、子树 work 合并、三模式祖先传播、socket pressure、eventfd 注册注销与销毁屏障已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/swap_table.h`：244/156/0.639/9；仅新增 224 行注释；编码布局、folio/shadow/count 状态、计数溢出、table 锁与 RCU、零页位图及 MEMCG 开关配置均已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/shmem_quota.c`：249/102/0.410/10；仅新增 136 行注释；内存 quota 红黑树、dquot 激活/释放、默认限制、枚举、锁序与发布屏障已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/damon/stat.c`：251/107/0.426/10；仅新增 139 行注释；物理 RAM context、带宽/空闲分位、自动调优、kdamond 回调及模块参数启停已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kasan/quarantine.c`：263/128/0.487/8；仅新增 172 行注释；per-CPU/全局 FIFO、容量逐出、cache 定向排空、SRCU 可见性与 CPU 热插拔已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/sparse.c`：271/146/0.539/9；仅新增 196 行注释；SPARSEMEM 索引、early nid 编码、present 登记、NUMA 分组、memmap/usage 发布及失败降级已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kasan/report_generic.c`：281/114/0.406/10；仅新增 148 行注释；shadow 坏地址/类型解码、slab 生命周期栈、编译器栈帧描述及 ASan 报告 ABI 已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kmsan/core.c`：291/116/0.399/9；仅新增 142 行注释；task 上下文、shadow/origin 写入、memmove 传播链、使用点分段报告与 metadata 连续性已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/damon/reclaim.c`：292/125/0.428/9；仅新增 153 行注释；模块参数快照、冷区 scheme、quota goal/filter、kdamond commit/启停与统计回调已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/debug.c`：297/94/0.316/10；仅新增 114 行注释；page/folio 快照、VMA/mm/vmg 诊断、flag 名称表及启动期 page struct poisoning 已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/pgtable-generic.c`：299/139/0.465/9；仅新增 202 行注释与空行；通用页表清坏项、PTE/THP TLB 配对、deposit FIFO、RCU/异步释放及 lockless 映射复核已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/page_isolation.c`：307/127/0.414/10；仅新增 161 行注释；可移动性预检、pageblock 首尾隔离、高阶页边界、zone 锁发布、回滚与最终空闲验证已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/swap.h`：307/301/0.980/8；仅新增 402 行注释与空行；全部 inline/外部接口及 CONFIG_SWAP 桩的契约、cluster 结构/枚举、设备引用、锁/RCU、swap-count/cache/I/O 生命周期已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/mmu_gather.c`：309/217/0.702/10；仅新增 289 行注释；33 个函数/配置实现的数据页编码、delayed rmap、页表 batch、TLB→RCU/IPI→释放、内存压力回退及 gather 生命周期已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/mmap_lock.c`：311/164/0.527/8；仅新增 208 行注释；17 个函数/配置实现的 VMA 排斥状态、RCU→引用交接、序号屏障、Maple Tree 重查及 fault 锁升级回滚已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/damon/ops-common.c`：313/136/0.435/9；仅新增 168 行注释；14 个函数的访问证据合并、评分、全部过滤枚举 case、迁移链表 ownership 与失败 putback 已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/numa_memblks.c`：323/152/0.471/10；仅新增 192 行注释；19 个函数的启动期生命周期、距离矩阵、memblock 清洗/合并、保留区节点、热拔限制、拓扑注册、填洞及热添加回退已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kmsan/hooks.c`：327/199/0.609/9；仅新增 265 行注释；19 个函数的四段式契约、runtime 递归门禁、slab/large alloc 生命周期、ioremap 元数据映射回滚、uaccess、USB/DMA 四方向及显式 poison API 已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/gup_test.c`：328/164/0.500/8；仅新增 194 行注释；11 个函数的四段式契约、全部 ioctl case、get/pin 配对、分批计时、长期 pin 会话及 debugfs 生命周期已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/dmapool.c`：340/181/0.532/10；仅新增 237 行注释；18 个函数的四段式契约、结构字段、空闲栈、coherent backing、双锁 sysfs 发布、debug poison、快慢分配与 devres 生命周期已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/memfd.c`：340/179/0.526/10；仅新增 233 行注释；16 个函数/系统调用的四段式契约、folio pin 候选退避、hugetlb 实例化、不可逆 seals、WRITE 发布回滚、mmap 与 noexec namespace 策略已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/damon/lru_sort.c`：346/199/0.575/9；仅新增 253 行注释；17 个函数/回调的四段式契约、全部参数语义、热冷 scheme、quota goal/filter、临时 ctx 原子提交、启停与统计回调生命周期已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/memremap.c`：351/238/0.678/10；仅新增 318 行注释；14 个函数的四段式契约、ZONE_DEVICE 上线/拆除、XArray-RCU/percpu_ref 生命周期、五类设备页释放与失败逆序回滚已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/page_ext.c`：352/252/0.716/9；仅新增 335 行注释；26 个函数/配置实现的四段式契约、客户端布局、FLATMEM/SPARSEMEM 分配、偏移指针编码、热插拔 invalid→RCU→free 与公开 get/put 生命周期已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/hugetlb_sysfs.c`：370/204/0.551/10；仅新增 261 行注释；23 个函数/配置实现的四段式契约、全局/节点 kobject 反查、持久池与 overcommit、demote 双锁/失败回源及 sysfs 树回滚已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/memfd_luo.c`：372/183/0.492/10；仅新增 228 行注释；12 个函数/回调的四段式契约、memfd-v2 packed ABI、folio pin/preserve/freeze、dirty+uptodate 数据不变量、page-cache/memcg/inode/LRU 恢复及分层失败清理已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/show_mem.c`：375/156/0.416/10；仅新增 184 行注释；9 个函数/配置实现的四段式契约、MemAvailable 保守估算、NUMA/cpuset/zone 过滤、全局/节点/zone 统计口径、buddy 锁内快照与非阻塞分配画像已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/damon/paddr.c`：382/219/0.573/10；仅新增 271 行注释；19 个函数的四段式契约、物理地址单位换算、young/idle 采样、probe/memcg 过滤、folio 去重、回收/LRU/NUMA 动作与操作表注册已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/workingset.c`：382/258/0.675/10；相对筛选基线仅新增 337 行注释；14 个具名函数及 CONFIG_LRU_GEN 双实现的四段式契约、shadow 位域/时间戳精度、传统 LRU 与 MGLRU refault、memcg/RCU 生命周期、纯 shadow XArray 节点 shrinker 锁序及初始化回滚已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/numa_emulation.c`：384/172/0.448/10；仅新增 222 行注释；13 个函数及两套 CPU-map 配置实现的四段式契约、候选 meminfo 构造与提交、洞/DMA32 尾段、固定/均分/交错切分、PXM/距离表重建和 CPU 热插拔映射已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kasan/init.c`：385/173/0.449/10；仅新增 230 行注释；24 个条件编译后函数实现的四段式契约、early/zero shadow 共享页表、slab 前后分配、逐级建表与拆表、静态表身份保护、热插拔回滚及低层 errno 不传播边界已覆盖，第 17 章验收、diff/checkpatch 通过；无 `.config`，未编译目标对象。
- [x] `mm/kasan/common.c`：398/221/0.555/10；仅新增 304 行注释；36 个函数的四段式契约、page/slab/kmalloc/mempool/vmalloc poison 生命周期、tag 选择、track 快照、quarantine ownership、KFENCE/RCU/抽样分支与 KASAN 深度下溢计数已覆盖，第 17 章验收、diff/checkpatch 通过；无 `.config`，未编译目标对象。
- [x] `mm/kasan/generic.c`：404/188/0.465/10；仅新增 264 行注释；Generic shadow 编码与固定/变长访问检查、编译器 `__asan_*` ABI、alloca/global redzone、cache metadata 布局回退、quarantine cache 清理和 alloc/free/aux track 生命周期已覆盖，第 17 章验收、diff/checkpatch 通过；无 `.config`，未编译目标对象。
- [x] `mm/kasan/shadow.c`：410/156/0.380/10；仅新增 220 行注释；26 个函数和 vmalloc 批次上下文的四段式契约、mem* 编译器 ABI、shadow 毒码/tag、hotplug notifier、vmalloc 并发建表与延迟拆表、跨 CPU 发布屏障及模块 shadow 回收已覆盖，第 17 章验收、diff/checkpatch 通过；无 `.config`，未编译目标对象。
- [x] `mm/vma.h`：416/170/0.409/10；仅新增 218 行注释；VMA 准备/munmap/merge/unmap 描述符、Maple Tree 游标包装、split/merge 只准备不提交属性、文件/anon_vma ownership、页表释放边界、MMU/64 位配置桩与 MDWE W^X 判定已覆盖，第 17 章验收、diff/checkpatch 通过；无 `.config`，未编译目标对象。
- [x] `mm/mempool.c`：436/130/0.298/10；仅新增 190 行注释；预留元素 ownership、curr_nr/min_nr 锁内不变量、SLUB/KASAN poison、零最小池、resize 并发发布、alloc/free 屏障、waitqueue 保底分配及 slab/kmalloc/page 后端配对已覆盖，第 17 章验收、diff/checkpatch 通过；无 `.config`，未编译目标对象。
- [x] `mm/readahead.c`：440/115/0.261/10；仅新增 149 行注释；同步/异步窗口推断、BDI 上限、large-folio order 对齐与回退、page cache folio 锁/引用交接、invalidate_lock+NOFS、aops 领取/清理边界、PSI、系统调用校验和窗口双向扩展已覆盖，第 17 章验收、diff/checkpatch 通过；无 `.config`，未编译目标对象。
- [x] `mm/execmem.c`：442/152/0.344/10；仅新增 191 行注释；架构窗口发布、KASAN shadow、ROX cache 的 busy/free Maple Tree ownership、direct-map 与 W^X 配对、陷阱指令、延迟释放重试、默认范围继承及启动期校验/发布已覆盖；第 17 章强制验收、diff/check 通过。无 `.config`，未编译目标对象；checkpatch 仅报告原有 MA_STATE 声明空行风格警告，未改代码规避。
- [x] `mm/kasan/kasan.h`：446/123/0.276/10；仅新增 145 行注释；Generic/SW/HW tag 配置分流、shadow granule/毒码 ABI、对象 metadata 与 stack ring 生命周期、报告对象分段填充、地址 tag 架构钩子、硬件 poison/unpoison、KUnit 开关及 ASAN/HWASAN 编译器插桩 ABI 已覆盖；第 17 章强制验收、diff/checkpatch 通过。无 `.config`，未编译目标对象。
- [x] `mm/kasan/report.c`：455/134/0.295/10；仅新增 152 行注释；软件/硬件抑制、首错与 multi-shot、KUnit 归属、raw-spinlock 报告边界、对象/栈/vmalloc/page 归属、metadata 行定位、模式层补全、UACCESS 恢复、异步 fault 与 non-canonical 反解已覆盖；第 17 章强制验收、diff/check 通过。无 `.config`，未编译目标对象；checkpatch 仅报告上游既有 `%px` 诊断输出及条件风格警告，未改代码规避。
- [x] `mm/truncate.c`：484/147/0.304/10；仅新增 157 行注释；XArray shadow/DAX exceptional 清理、folio 截断与拆分回退、两遍 page-cache 删除、final truncate 的 AS_EXITING 屏障、轻量/强制失效、写回与页表撤销、文件大小扩展零化、普通截断与 hole-punch 的 COW 边界已覆盖；第 17 章强制验收、diff/check 通过。无 `.config`，未编译目标对象；checkpatch 仅报告上游文件名、函数指针原型和单语句大括号风格警告，未改代码规避。
- [x] `mm/slab.h`：514/124/0.241/10；仅新增 173 行注释；slab/page ABI overlay、freelist ABA 双字 CAS、cache 与 per-node/sheaf 所有权、kmalloc bucket 选择、启动状态机、对象索引与 KASAN tag、obj-ext sanitizer 访问期、memcg/RCU 钩子、large-kmalloc 编码、随机 freelist 及初始化/调试边界已覆盖；第 17 章强制验收、diff/check 通过。无 `.config`，未编译目标对象；checkpatch 仅报告既有宏对齐、typedef、data_race 说明及无参数名原型风格，未改代码规避。
- [x] `mm/highmem.c`：521/156/0.299/10；仅新增 159 行注释；永久 PKMAP 的 count/PTE/page-address 三元状态、TLB/cache 回收发布、色彩槽与等待队列、highmem 引用配对、compound page 双区间零化、task-local fixmap 的迁移/抢占/调度保存恢复、架构 hook、fork 清理及 hash page-to-virtual 关联锁序已覆盖；第 17 章强制验收、diff/check 通过。无 `.config`，未编译目标对象；checkpatch 仅报告上游宏未用参数、waitqueue 说明与裸 unsigned 风格，未改代码规避。
- [x] `mm/shrinker.c`：530/140/0.264/10；仅新增 142 行注释；memcg per-node unit COW 扩容与 RCU 发布、IDR/bitmap 位屏障、deferred 扫描债务领取/回填、cgroup reparent、NUMA 路由、shrink budget/batch/trace、memcg 与全局 RCU 遍历、注册引用发布、等待回调退出及 RCU 延迟销毁已覆盖；第 17 章强制验收、diff/check 通过。无 `.config`，未编译目标对象；checkpatch 仅报告既有 DEFINE_IDR 与 -ENOSYS 风格，未改代码规避。
- [x] `mm/page_io.c`：534/267/0.500/9；仅新增 334 行注释；文件 swap extent 激活、zeromap 与 large-folio 边界、zswap/memcg 策略、文件/同步块/异步块三种 I/O 后端、bio/iocb/mempool ownership、folio writeback/uptodate/unlock 发布、PSI 与统计、RCU/cluster 锁协议已覆盖；第 17 章强制验收、diff/checkpatch 通过。无 `.config`，未编译目标对象。
- [x] `mm/hugetlb_vmemmap.c`：538/166/0.309/10；仅新增 197 行注释；HVO 的 PMD 拆分、PTE head/tail 重映射、写屏障/TLB 发布、部分失败恢复、memblock/buddy 页 ownership、批量优化低内存重试、bootmem sparsemem 预初始化与跨 zone fallback 已覆盖；第 17 章强制验收、diff/checkpatch 通过。无 `.config`，未编译目标对象。
- [x] `mm/list_lru.c`：548/207/0.378/10；仅新增 250 行注释；node/memcg 分桶、irq/irqsave 锁配对、RCU+XArray 查找、dying memcg 向父链退避、add/del 与 shrinker bit、walker 的全部 lru_status、reparent 的 LONG_MIN/RCU 延迟释放、按需分配与 init/destroy 生命周期已覆盖；第 17 章强制验收、diff/checkpatch 通过。无 `.config`，未编译目标对象。
- [x] `mm/kmsan/kmsan_test.c`：556/297/0.534/10；仅新增 409 行注释；console tracepoint 报告快照、KUnit 预期匹配、分配/栈/参数传播、页与 vmap、UAF、per-CPU、memcpy/origin、stackdepot、反毒、nofault 复制与 suite 生命周期已覆盖；第 17 章强制验收、diff/checkpatch 通过。无 `.config`，未编译 KUnit 目标。
- [x] `mm/mlock.c`：564/229/0.406/10；仅新增 291 行注释；folio LRU 批处理和低位操作 tag、mlock_count/unevictable 迁移、PTE/PMD walk、大 folio 范围约束、VMA split/merge 与 VM_IO 竞争门禁、RLIMIT 重叠核算、mlock/munlock/lockall ABI 及 SHM ucounts 生命周期已覆盖；第 17 章强制验收、diff/checkpatch 通过。无 `.config`，未编译目标对象；checkpatch 仅报告上游既有 3 warnings、12 checks，未改源码消音。

本轮十文件进度（5/10）：`vma.h` 已按修改后完整文件顺序复读；全部结构/字段组、枚举、宏、声明与内联实现，以及非豁免英文注释均有紧邻中文学习说明。相对基线新增 218 行、删除 0 行；密度 `416/170/0.409/10`，`git diff --check` 与 `checkpatch --strict --ignore LONG_LINE_COMMENT` 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译。

本轮十文件进度（6/10）：`mempool.c` 已按修改后完整文件顺序完成第 17 章强制验收；33 个函数/配置桩、故障注入状态、全部非豁免英文注释、普通分配→预留池→等待重试、归还回填/底层释放、resize 竞态和屏障协议均已复读。相对基线新增 190 行、删除 0 行；密度 `436/130/0.298/10`，diff/checkpatch 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译。

本轮十文件进度（7/10）：`readahead.c` 已按修改后完整文件顺序完成第 17 章强制验收；17 个函数/系统调用、窗口状态、全部非豁免英文注释、同步/异步触发、large-folio 对齐降级、folio 发布/领取/移除、EOF/拥塞/冲突和部分扩展均已复读。相对基线新增 149 行、删除 0 行；密度 `440/115/0.261/10`，diff/checkpatch 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译。

本轮十文件进度（1/10）：`workingset.c` 已按修改后完整文件顺序完成第 17 章强制验收；函数/实体、全部非豁免英文注释、传统与 MGLRU 快慢路径、shadow 发布与回收、RCU/引用/锁序及失败出口均已复读。相对基线新增 337 行、删除 0 行，新增非空行仅为注释；密度 `382/258/0.675/10`，`git diff --check` 与 `checkpatch --strict --ignore LONG_LINE_COMMENT` 均为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译。

本轮十文件进度（2/10）：`numa_emulation.c` 已按修改后完整文件顺序完成第 17 章强制验收；全部函数、全局映射、命令行三种模式、成功提交/失败回退、物理洞与尾段、距离矩阵及两套 CPU mask 配置实现均已复读，所有非豁免英文注释均有紧邻完整译注。相对基线新增 222 行、删除 0 行，新增非空行仅为注释；密度 `384/172/0.448/10`，diff/checkpatch 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译。

本轮五文件聚合验收：`kmsan/core.c`、`damon/reclaim.c`、`debug.c`、`pgtable-generic.c`、`page_isolation.c` 合计新增 772 行注释与空行、删除 0 行；新增非空行全部位于注释，原代码、预处理行及上游注释文本/顺序/缩进未改。密度门禁依次为 `291/116/0.399/9`、`292/125/0.428/9`、`297/94/0.316/10`、`299/139/0.465/9`、`307/127/0.414/10`，均通过；聚合 `git diff --check` 及 `checkpatch --strict --ignore LONG_LINE_COMMENT` 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译。

本轮第 17 章内容验收记录：按修改后完整文件顺序复核的函数/回调/配置实现为 `10/13/10/26/10`，另逐项核对 KMSAN 全局/per-CPU 状态、DAMON 参数模板与 control、调试名称表、架构覆盖分支、pageblock 计数及全部局部游标；所有非豁免英文注释均保留原文并有紧邻翻译或学习补充。路径清单覆盖 KMSAN tracked/untracked 与 origin 链退化、DAMON 候选 ctx 原子提交和启动后回调失败、调试快照的竞态容忍、PTE/PMD clear→TLB flush→RCU/异步释放，以及 pageblock 首尾优先隔离与中途失败逆序恢复。复述与推理抽查选取 `kmsan_internal_memmove_metadata()`/`kmsan_internal_check_memory()`、`damon_reclaim_apply_parameters()`/`damon_reclaim_turn()`、`__dump_folio()`/`dump_vmg()`、`__pte_offset_map()`/`pte_offset_map_lock()`、`isolate_single_pageblock()`/`start_isolate_page_range()`，均可由注释恢复入口约束、ownership、锁/RCU/中断、发布点、失败或退化路径、配置差异和返回后的下一步，结论均为通过。

本轮五文件聚合验收：`swap.h`、`mmu_gather.c`、`mmap_lock.c`、`damon/ops-common.c`、`numa_memblks.c` 合计新增 1259 行注释与空行、删除 0 行；新增非空行全部位于注释，原代码、预处理行及上游注释文本/顺序/缩进未改。密度门禁依次为 `307/301/0.980/8`、`309/217/0.702/10`、`311/164/0.527/8`、`313/136/0.435/9`、`323/152/0.471/10`，均通过；聚合 `git diff --check` 及 `checkpatch --strict --ignore LONG_LINE_COMMENT` 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译。

本轮第 17 章内容验收记录：按物理结构复核的接口/函数/配置实现为 `54/33/17/14/19`，并逐项核对 swap 编码与 CONFIG 桩、mmu_gather 批次编码和 sched-RCU、VMA 引用高位状态、DAMON 评分常量与迁移链表、NUMA 距离/内存块全局状态及全部局部游标。路径清单覆盖 swap entry 到设备位置与 cache 引用、页表脱链→TLB 失效→rmap/页释放、RCU 查找→VMA 引用→全局锁慢路径、young 清位→idle 基线→过滤/迁移，以及固件块解析→裁剪合并→memblock 注册→热添加查询。复述与推理抽查选取 `swap_cache_get_folio()`/cluster helpers、`tlb_remove_table()`/`tlb_finish_mmu()`、`vma_start_read()`/`lock_mm_and_find_vma()`、`damon_folio_young()`/`damon_migrate_pages()`、`numa_cleanup_meminfo()`/`numa_fill_memblks()`，均可由注释恢复入口约束、ownership、锁/RCU/中断、发布点、失败或退化路径、配置差异和返回后的下一步，结论均为通过。

本轮五文件聚合验收：`kmsan/hooks.c`、`gup_test.c`、`dmapool.c`、`memfd.c`、`damon/lru_sort.c` 合计新增 1182 行注释、删除 0 行；新增非空行全部位于注释，原代码、预处理行及上游注释文本/顺序/缩进未改。密度门禁依次为 `327/199/0.609/9`、`328/164/0.500/8`、`340/181/0.532/10`、`340/179/0.526/10`、`346/199/0.575/9`，均通过；聚合 `git diff --check` 及 `checkpatch --strict --ignore LONG_LINE_COMMENT` 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译。

本轮第 17 章内容验收记录：按修改后完整文件的物理结构复核函数/回调/配置实现为 `19/11/18/16/17`，另逐项核对 KMSAN 元数据地址与递归状态、GUP 长期会话全局量与 fops、DMA pool 三结构/双锁/计数、memfd seal/tag/flags 常量、LRU sort 全部模块参数模板和 control/ops 对象；所有非豁免英文注释均逐字保留并有紧邻完整译注。路径清单覆盖 allocation/free→shadow/origin 状态、get/pin→验证→配对归还、coherent 页→boundary 分块→借出/归还→managed 销毁、memfd writable deny→pin 排空→seal 发布，以及模块参数→临时 ctx/scheme→commit→kdamond 启停/统计回调。复述与开发者推理抽查选取 `kmsan_ioremap_page_range()`/`kmsan_copy_to_user()`/`kmsan_handle_dma_page()`、`__gup_test_ioctl()`/`pin_longterm_test_start()`/`pin_longterm_test_read()`、`dma_pool_create_node()`/`dma_pool_alloc()`/`dma_pool_destroy()`、`memfd_alloc_folio()`/`memfd_wait_for_pins()`/`memfd_add_seals()`、`damon_lru_sort_apply_parameters()`/`damon_lru_sort_commit_inputs_store()`/`damon_lru_sort_turn()`，均可仅由注释恢复入口约束、ownership、锁/IRQ/runtime、真正发布点、部分失败回滚、配置差异和返回后的下一步，结论均为通过。

本次指定十文件返工总验收：上述 `swap.h` 至 `numa_memblks.c` 五文件与 `kmsan/hooks.c` 至 `damon/lru_sort.c` 五文件合计新增 2441 行注释与空行、删除 0 行；十文件密度与最大代码间隙全部达标，新增非空行仅为注释，聚合 diff/checkpatch 为 0 errors、0 warnings、0 checks。两批均已完成逐函数契约、实体/枚举、英文原注释紧邻译注、并发/ownership/失败路径及第 17 章复述抽查。

本轮五文件聚合验收：`shmem_quota.c`、`damon/stat.c`、`kasan/quarantine.c`、`sparse.c`、`kasan/report_generic.c` 合计新增 791 行注释、删除 0 行；新增非空行全部位于注释，原代码、预处理行及上游注释文本/顺序/缩进未改。密度门禁依次为 `249/102/0.410/10`、`251/107/0.426/10`、`263/128/0.487/8`、`271/146/0.539/9`、`281/114/0.406/10`，均通过；聚合 `git diff --check` 及 `checkpatch --strict --ignore LONG_LINE_COMMENT` 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译。

本轮第 17 章内容验收记录：按物理结构复核的函数/回调/条件配置实现为 `10/15/17/23/14+10 个生成入口`，并逐项核对 quota 红黑树与操作表、DAMON 模块参数和 context、quarantine 队列/全局环/SRCU、mem_section/usage 与临时游标、KASAN shadow 魔数和生成宏；全部非豁免英文注释均有紧邻完整翻译与学习补充。路径清单覆盖 dquot acquire/release/枚举与发布屏障、DAMON context 构造/回滚/启停、对象 per-CPU→global→逐出及 cache 销毁屏障、SPARSEMEM present→memmap 提交与部分失败降级，以及 Generic KASAN 类型/栈帧解码和固定/可变宽度报告 ABI。复述与推理抽查选取 `shmem_acquire_dquot()`/`shmem_release_dquot()`、`damon_stat_build_ctx()`/`damon_stat_start()`、`kasan_quarantine_put()`/`kasan_quarantine_remove_cache()`、`sparse_init_nid()`/`sparse_init()`、`get_shadow_bug_type()`/`get_address_stack_frame_info()`，均可由注释恢复入口约束、ownership、锁/中断/SRCU、发布点、失败或退化路径、配置差异和返回后的下一步，结论均为通过。

本轮五文件聚合验收：`mincore.c`、`kmsan/shadow.c`、`page_vma_mapped.c`、`vmpressure.c`、`swap_table.h` 合计新增 1051 行注释与空行、删除 0 行；新增非空行全部位于注释，原代码、预处理行及上游注释文本/顺序/缩进未改。密度门禁依次为 `223/170/0.762/7`、`234/171/0.731/8`、`235/126/0.536/10`、`236/166/0.703/9`、`244/156/0.639/9`，均通过；聚合 `git diff --check` 及 `checkpatch --strict --ignore LONG_LINE_COMMENT` 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译。

本轮第 17 章内容验收记录：按物理结构复核的函数/系统调用/配置分支实现为 `9/14/7/12/30`，并逐项核对结构体、位布局宏、全局状态、work/eventfd 生命周期、页表游标以及全部非豁免英文注释。路径清单覆盖 mincore 的 HugeTLB/THP/PTE 与权限模糊、KMSAN metadata 地址分派和分配/vmap/启动绑定、反向映射的无锁扫描与锁后复核、vmpressure 的窗口聚合与层级传播，以及 swap table 的状态编解码、计数/零页/MEMCG 辅助表。复述与推理抽查选取 `mincore_pte_range()`/`SYSCALL_DEFINE3(mincore)`、`kmsan_vmap_pages_range_noflush()`、`page_vma_mapped_walk()`、`vmpressure_work_fn()`/`vmpressure_register_event()`、`swap_table_get()`/零页与 MEMCG helpers，均可由注释恢复入口约束、ownership、锁/RCU/递归门禁、失败或退化路径、配置差异和返回后的下一步，结论均为通过。

本轮五文件聚合验收：`percpu-vm.c`、`kmsan/instrumentation.c`、`shrinker_debug.c`、`page_table_check.c`、`page_reporting.c` 合计新增 695 行注释与空行、删除 0 行；新增非空行全部位于注释，原代码、预处理行及上游注释文本/顺序/缩进未改。密度门禁依次为 `201/149/0.741/10`、`213/102/0.479/10`、`212/72/0.340/10`、`213/86/0.404/10`、`215/105/0.488/10`，均通过；聚合 `git diff --check` 及 `checkpatch --strict --ignore LONG_LINE_COMMENT` 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译。

本轮第 17 章内容验收记录：按物理结构复核的函数/生成宏/回调为 `18/15+1/9/17/9`；结构体/字段、全局状态、static key、模块参数、ops/fops、原子状态机、局部游标及全部非豁免英文注释均逐项复读。路径清单覆盖 percpu NUMA backing 分配—映射—TLB 回收、KMSAN 数据与 metadata/TLS 同步、shrinker memcg 引用与 debugfs detach、页表 clear-before-set 账本和 page reporting 隔离—设备处理—buddy 归还。复述与推理抽查选取 `pcpu_populate_chunk()`/`pcpu_depopulate_chunk()`、`__msan_memmove()`/`__msan_poison_alloca()`、`shrinker_debugfs_scan_write()`、`__page_table_check_pmds_set()`、`page_reporting_cycle()`/`page_reporting_unregister()`，均可由注释恢复入口约束、ownership、锁/RCU/递归门禁、发布点、失败回滚或退化路径和返回后的下一步，结论均为通过。

本轮五文件聚合验收：`kmsan/report.c`、`balloon.c`、`process_vm_access.c`、`secretmem.c`、`page_counter.c` 合计新增 871 行注释、删除 0 行；新增非空行全部位于注释，原代码、预处理行及上游注释文本/顺序/缩进未改。密度门禁依次为 `171/110/0.643/10`、`180/153/0.850/7`、`188/117/0.622/10`、`194/141/0.727/10`、`200/165/0.825/9`，均通过；聚合 `git diff --check` 及 `checkpatch --strict --ignore LONG_LINE_COMMENT` 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译。

本轮第 17 章内容验收记录：按物理结构复核的函数/回调为 `3/10/6/11/10`，另核对报告锁与参数、balloon movable_ops、secretmem 的 VFS/MM 操作表、page_counter 配置分支及全部全局/静态状态；所有非豁免英文注释均有紧邻完整翻译与学习补充。路径清单覆盖 KMSAN origin 链与递归恢复、balloon isolate/putback/-ENOENT 退化、process_vm 权限/pin/部分成功、secretmem direct-map/EEXIST/释放擦除，以及 page_counter 层级失败回滚与保护预算分摊。复述与推理抽查选取 `kmsan_report()`、`balloon_page_migrate()`、`process_vm_rw_core()`、`secretmem_fault()`、`page_counter_try_charge()`/`effective_protection()`，均可由注释恢复入口约束、ownership、并发与屏障、发布点、失败/退化路径、配置差异和返回后的下一步，结论均为通过。

本轮五文件聚合验收：`percpu-internal.h`、`cma_debug.c`、`ptdump.c`、`mapping_dirty_helpers.c`、`usercopy.c` 合计新增 720 行注释与空行、删除 0 行；新增非空行全部位于注释，原代码、预处理行及上游注释文本/顺序/缩进未改。密度门禁依次为 `162/115/0.710/10`、`163/93/0.571/9`、`163/101/0.620/10`、`166/115/0.693/9`、`166/116/0.699/10`，均通过；聚合 `git diff --check` 及 `checkpatch --strict --ignore LONG_LINE_COMMENT` 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译。

本轮第 17 章内容验收记录：按物理结构复核的函数/回调/配置桩为 `15/9/10/9/9`，每个实体均有专属契约或配置关闭分支的紧邻契约；结构体/字段组、ops 表、static key、启动参数、局部状态与全部非豁免英文注释均逐项复读。路径清单覆盖 percpu bitmap/chunk 槽链与 MEMCG 扩展、CMA debugfs 测试页 ownership、ptdump leaf/hole/KASAN 折叠、共享映射 dirty 收割的 notifier/cache/TLB 配对，以及 hardened usercopy 的栈/heap/text 分类和 BUG 边界。复述与推理抽查选取 `pcpu_obj_full_size()`、`cma_free_mem()`、`ptdump_walk_pgd()`、`clean_record_shared_mapping_range()`、`__check_object_size()`，均可由注释恢复入口约束、锁/ownership、发布或清位点、配置/架构差异、失败/退化路径和返回后的下一步，结论均为通过。

前批 checkpatch 以 `--strict --ignore LONG_LINE` 执行：中文 UTF-8 字节列宽告警被明确豁免，避免破坏中文语义断句；其余结果为 0 errors、0 warnings、0 checks。

本批五文件同样以补丁模式执行 `--strict --ignore LONG_LINE`，结果为 0 errors、0 warnings、0 checks；追加式审计为 521 行新增、0 行删除，新增行仅含注释或空行。仓库无 `.config`，未执行目标编译。

本批第 17 章内容验收记录：按结构位置核对的函数/配置声明为 `14/7/11/18/4`，均有专属函数头或明确组契约，未发现错位；结构体、字段组、全局量、枚举项、局部变量和全部上游英文注释均已逐项复读，原文删除/改写为 0。路径清单覆盖 NUMA 快慢轮转与 demote 回滚、CMA bitmap 认领/释放及可选计账、folio dirty/writeback/page-cache 发布、HVO 单项/批量恢复与关闭配置、KFENCE 地址失败和 report/handler 分离。复述/推理抽查选取 `demote_pool_huge_page()`、`hugetlb_vmemmap_restore_folios()`、`addr_to_metadata()` + `kfence_report_error()`，可由注释恢复入口锁/ownership、发布点、部分失败、配置差异和返回后的下一步，结论均为通过。

本轮五文件聚合验收：`sysfs-kunit.h`、`memcontrol-v1.h`、`zpdesc.h`、`kmsan.h`、`shuffle.c` 合计新增 769 行注释与空行、删除 0 行；新增非空行全部位于注释，原代码、预处理行及上游注释文本/顺序/缩进未改。密度门禁依次为 `86/48/0.558/9`、`90/195/2.167/8`、`90/94/1.044/8`、`98/132/1.347/9`、`101/64/0.634/9`，均通过；聚合 `git diff --check` 及 `checkpatch --strict --ignore LONG_LINE_COMMENT` 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译。

本轮第 17 章内容验收记录：按结构位置核对的函数/接口契约为 `3/45/13/24/5`（含 CONFIG 分支桩），结构体/字段、static key、参数操作表、per-CPU 状态、枚举/常量、宏与局部变量均有紧邻用途和生命周期说明；全部非豁免英文注释已逐项翻译并补充，`shuffle.c` 旧“新增子区间”表述另以修正说明限定为当前全 zone 实现。路径清单覆盖 KUnit 分配回滚与部分 target 追加、MEMCG_V1 开关/OOM 锁与中性桩、zpdesc 锁/引用/迁移、KMSAN tracked/untracked 与递归退避、zone 随机候选重试/migratetype 拒绝/锁让出。复述与推理抽查选取 `damon_sysfs_test_add_targets()`、`memcg1_oom_prepare()` 两配置契约、`zpdesc_wait_locked()` 引用边界、`kmsan_in_runtime()`/origin 链、`__shuffle_zone()`，均可由注释恢复调用条件、ownership、并发参与者、失败/退化路径和返回后下一步。

本轮五文件聚合验收：`kasan/tags.c`、`damon/sysfs-common.c`、`page_frag_cache.c`、`memtest.c`、`hugetlb_sysctl.c` 合计新增 295 行注释、删除 0 行；新增非空行全部位于注释，原代码、预处理行及上游注释文本/顺序/缩进未改。密度门禁依次为 `104/35/0.337/9`、`108/41/0.380/8`、`111/47/0.423/10`、`112/45/0.402/10`、`115/37/0.322/10`，均通过；聚合 `git diff --check` 及 `checkpatch --strict --ignore LONG_LINE_COMMENT` 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译。

本轮第 17 章内容验收记录：按物理结构复核的函数/接口为 `6/8/9/6/6`，均有专属契约或紧邻的明确组契约；全局状态、编码字段、引用偏置、启动参数、sysctl 表项、CONFIG 分支和原有英文注释均逐项复读。路径清单覆盖 KASAN 栈环写者/报告者互斥与 Stack Depot 引用交接、DAMON range kobject/memcg 迭代、page fragment refill/复用/pfmemalloc 淘汰、memtest 花纹/坏段 reservation/proc 区分，以及 HugeTLB persistent/surplus/mempolicy 提交。复述与推理抽查选取 `save_stack_info()`、`damon_sysfs_memcg_path_to_id()`、`__page_frag_alloc_align()`、`memtest()`、`hugetlb_sysctl_handler_common()`，均可由注释恢复入口约束、ownership/锁、状态发布、失败或退化路径和返回后的下一步，结论均为通过。

本轮五文件聚合验收：`vma_init.c`、`cma_sysfs.c`、`dmapool_test.c`、`hwpoison-inject.c`、`fadvise.c` 合计新增 602 行注释与空行、删除 0 行；新增非空行全部位于注释，原代码、预处理行及上游注释文本/顺序/缩进未改。密度门禁依次为 `115/98/0.852/9`、`117/83/0.709/9`、`122/76/0.623/9`、`139/94/0.676/10`、`140/90/0.643/8`，均通过；聚合 `git diff --check` 及 `checkpatch --strict --ignore LONG_LINE_COMMENT` 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译。

本轮第 17 章内容验收记录：按结构位置核对的函数/配置实现为 `9/11/6/9/6`（含 PFN 跟踪、MEMCG 与架构 ABI 配置分支），结构体/字段、全局状态、属性表、宏、参数和局部变量均有紧邻用途与有效期说明；全部非豁免英文注释已逐项翻译补充。路径清单覆盖 VMA slab/PFN ctx acquire-release、CMA kobject 发布与逆序回滚、DMA pair 部分分配回滚、hwpoison 竞态预筛与页锁内 RCU 复核，以及 DONTNEED 部分页保留/本地 LRU drain/远端重试。复述与推理抽查选取 `vm_area_dup()`、`cma_sysfs_init()`、`dmapool_test_alloc()`、`hwpoison_inject()`、`generic_fadvise()`，均可由注释恢复入口约束、ownership、同步参与者、失败或成功跳过语义、配置差异和返回后的下一步。

本轮五文件聚合验收：`maccess.c`、`kmsan/init.c`、`percpu-stats.c`、`damon/tests/vaddr-kunit.h`、`page_idle.c` 合计新增 578 行注释与空行、删除 0 行；新增非空行全部位于注释，原代码、预处理行及上游注释文本/顺序/缩进未改。密度门禁依次为 `145/99/0.683/10`、`152/98/0.645/9`、`157/70/0.446/9`、`160/86/0.537/9`、`160/83/0.519/9`，均通过；聚合 `git diff --check` 及 `checkpatch --strict --ignore LONG_LINE_COMMENT` 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译和 KUnit 运行。

本轮第 17 章内容验收记录：按结构位置复核的函数/回调为 `9/9/5/8/6`，另核对 nofault 循环宏、KMSAN 启动账本结构、percpu 输出宏、KUnit case/suite 和 page-idle sysfs 属性实体；全部非豁免英文注释均有紧邻翻译与学习补充，vaddr 测试原英文把最终映射终点写作 305 的不一致已保留原文并以修正说明按代码/断言限定为 330。路径清单覆盖 nofault 的 -ERANGE/-EFAULT 与部分复制、KMSAN 三块分组/降阶回收、percpu 锁外扩容重试、DAMON fixture skip/部分更新销毁、page-idle PFN/folio 竞态复核及 PTE/PMD/MMU notifier/reclaim 补偿。复述与推理抽查选取 `copy_from_kernel_nofault()`、`kmsan_memblock_discard()`、`percpu_stats_show()`、`damon_do_test_apply_three_regions()`、`page_idle_bitmap_read()/write()`，均可由注释恢复入口约束、ownership、锁/睡眠边界、失败/退化路径、发布点与返回后的下一步。

本轮五文件聚合验收：`memremap.c`、`page_ext.c`、`hugetlb_sysfs.c`、`memfd_luo.c`、`show_mem.c` 合计新增 1326 行注释、删除 0 行；新增非空行全部位于注释，原代码、预处理行及上游注释文本/顺序/缩进未改。密度门禁依次为 `351/238/0.678/10`、`352/252/0.716/9`、`370/204/0.551/10`、`372/183/0.492/10`、`375/156/0.416/10`，均通过；聚合 `git diff --check` 及 `checkpatch --strict --ignore LONG_LINE_COMMENT` 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译。

本轮第 17 章内容验收记录：按修改后完整文件的物理结构复核函数/回调/配置实现为 `14/26/23/12/9`，另逐项核对 dev_pagemap/XArray、page_ext ops/偏移编码、hstate kobject/属性表、memfd-v2 ABI/回调表、全局统计/迁移类型表和全部局部变量；所有非豁免英文注释均保留原文并有紧邻完整译注。路径清单覆盖 ZONE_DEVICE 上线与失败逆序拆除、page_ext 的 invalid→RCU→free 热拔发布、HugeTLB resize/demote 双锁及回源、memfd folio preserve→freeze→restore 的多层清理，以及 MemAvailable 估算→NUMA 过滤→buddy 锁内复制→锁外打印。复述与推理抽查选取 `devm_memremap_pages()`/`memunmap_pages()`、`page_ext_init()`/`page_ext_callback()`、`demote_store()`/`hugetlb_sysfs_add_hstate()`、`memfd_luo_preserve()`/`memfd_luo_retrieve()`、`si_mem_available()`/`show_free_areas()`/`__show_mem()`，均可由注释恢复入口约束、ownership、锁/RCU/IRQ、状态发布、失败或非阻塞退化、配置差异和返回后的下一步，结论均为通过。

## 待决

- [-] `mm/damon/modules-common.h`：语义补注完成；统计宏为不可拆的 11 行续行，门禁 max_gap=11，未改宏凑数。

## 已探查索引

- 基线版本：HEAD `3d4371ed2b62`；主方法论 1474 行已完整读取；密度命令固定为 `--min-density 0.20 --max-code-gap 10`。
- `ops-common.h`：已读完整实现 `ops-common.c`，调用点 `paddr.c`、`vaddr.c`、`core.c`，以及 `include/linux/{damon,migrate}.h`；已核对 folio 引用、young/idle、过滤、评分和迁移链表契约。
- `modules-common.c/.h`：已读 `core.c` 的 ops/context/target 构造销毁，以及 `reclaim.c`、`lru_sort.c` 的全部宏调用和构造调用；已核对 attrs/quota/watermarks/stat 的单位与权限。
- `gup_test.h`：已读 `gup_test.c:1-375`，并在 `tools/testing/selftests/mm/{gup_test.c,gup_longterm.c,cow.c,hmm-tests.c,uffd-unit-tests.c}` 核对 ABI 使用；9 个 ioctl、两组 flags、两结构体字段及 get/pin、fast/basic、长期 pin、dump、read/stop 的输入输出和释放配对已完成注释。
- 构建元数据：`mm/Kconfig` 的配置值流向 `mm/Makefile`，再由 `source "mm/damon/Kconfig"` 和 `obj-$(CONFIG_DAMON) += damon/` 进入 DAMON；下一次从 `mm/damon/Kconfig` 继续，无需重扫本轮五文件。
- 构建元数据第二批：`mm/damon/Kconfig`、`kasan/Makefile`、`kfence/Makefile`、`kmsan/Makefile` 已闭环；下一轮直接进入基线 `.c/.h`，无需重扫构建入口。
- `page_reporting.h`：已读并在本轮闭环 `mm/page_reporting.c` 的 order 参数、request/notify、process、register/unregister，另核对 `mm/page_alloc.c` 的 free-list 摘除与释放通知调用点，以及 `include/linux/page_reporting.h` 的驱动契约；确认 static key、RCU 设备生命周期、zone 锁下 PageReported 清理和异步批处理边界。私有头与实现现均已达到学习注释标准，公开驱动头仍留待其基线顺序。
- 本批五文件：`rodata_test.c` 已核对 `init/main.c::mark_readonly`、`mm/maccess.c::copy_to_kernel_nofault` 与 `include/linux/rodata_test.h`；`damon/sysfs-common.h` 已核对 `sysfs-common.c`、`sysfs-schemes.c` 对应实现和 `sysfs.c` 调用点；`kasan/report_hw_tags.c` 已核对 `report.c` 报告组装及 `kasan.h` 标签/粒度定义；`mm_slot.h` 已核对 `ksm.c`、`khugepaged.c` 注册退出与锁/引用配对；`debug_page_alloc.c` 已核对 `mm_init.c` static key 提交、`page_alloc.c` guard 拆分/合并及 `include/linux/mm.h` 热路径包装。关联实现的学习注释覆盖不一，本批只修改五个基线目标。
- 本批三文件：`pgalloc-track.h` 已核对 `include/linux/{pgtable,pgalloc}.h` 的修改位/同步契约，以及 `vmalloc.c`、`memory.c` 的页表下降调用链；`hugetlb_cma.h` 已核对 `hugetlb_cma.c` 全部公开实现、`cma.c` frozen/early reservation helper 和 `hugetlb.c` 分配释放/启动路径；`shuffle.h` 已核对 `shuffle.c` 实现、`page_alloc.c` 高阶释放、`mm_init.c` 启动调用与 `memory_hotplug.c` 上线调用。关联实现不在本批修改范围。
- 本批十文件：`bootmem_info.c` 已核对 `include/linux/bootmem_info.h` 和 `arch/x86/mm/init_64.c` 的登记/释放配对；`numa.c` 已核对 `drivers/base/arch_numa.c`、`mm/numa_memblks.c` 及热插拔/CXL/DAX 调用点；`ioremap.c` 已核对 `include/asm-generic/io.h`、`arch/arm64/mm/ioremap.c` 和 `mm/vmalloc.c` 的 VM_IOREMAP/页表回滚；两个 KASAN 文件已核对 `kasan/{tags,report}.c` 与 `kasan.h` 的报告组装及 stack ring 并发；lazy-MMU KUnit 已核对 `include/linux/{pgtable,sched}.h` 的计数与架构 enter/leave/flush 契约；两个 fault-injection 文件已核对 `lib/fault-inject.c`、`mm/{page_alloc,slub}.c` 的调用与错误语义。关联实现不在本批修改范围。
- 本批五文件：`hugetlb_internal.h` 已核对 `hugetlb.c` 的池出入/批量发布/demote/store 实现以及 `hugetlb_sysfs.c`、`hugetlb_sysctl.c` 的调用与初始化；`cma.h` 已核对 `cma.c` 的激活、分配、回滚和释放，以及 `cma_sysfs.c`/`cma_debug.c` 调用点；`folio-compat.c` 已核对 `filemap.c`、`page-writeback.c`、`swap.c` 及相关公开声明的 folio 契约；`hugetlb_vmemmap.h` 已核对 `hugetlb_vmemmap.c` 的单项/批量优化恢复和 pre-HVO；`kfence.h` 已核对 `kfence/{core,report}.c` 的状态转换、fault/report 和最终处置。`hugetlb.c`、`filemap.c`、`kfence/{core,report}.c` 在读取范围已有中文学习注释但未在本轮全文件复审；其余关联实现在读取范围缺少系统学习注释，建议后续仍按基线顺序分别补注，本批未越界修改。
- `mmzone.c`：已核对 `include/linux/mmzone.h` 的遍历宏/zonelist/lruvec 声明、`mm/mm_init.c::pgdat_init_internals()`、`mm/memcontrol.c::alloc_mem_cgroup_per_node_info()` 及 CPUPID 的调度/fault/迁移调用点；确认裸 pgdat/zone 游标依赖外层热插拔稳定性，lruvec 在发布前初始化，flags 内嵌 CPUPID 用 cmpxchg 保留并发 flag 更新。关联区域除已注释的 `mm_init.c`/`memcontrol.c` 外覆盖不一，本轮未越界修改。
- `percpu-km.c`：已核对 `mm/percpu.c` 的后端接口 include、创建/销毁/地址反查及 balance/reclaim 调用点，并与 `mm/percpu-vm.c` 的逐页映射后端对照；确认 KM chunk 在 create 时全量 populated、depopulate/TLB flush 为空、最终按原 order 整体释放。`percpu-km.c` 与 `percpu-vm.c` 现均已闭环，核心 `percpu.c` 仍留待其基线顺序。
- `msync.c`：已核对 `fs/sync.c::vfs_fsync_range()` 与 `include/linux/fs.h` 声明，确认 `datasync=1` 通过文件系统 fsync 回调提交数据及必要元数据；并核对 UAPI MS_* 标志与 mm selftests 调用。`fs/sync.c` 与 UAPI 关联区域缺少本任务级系统学习注释，建议按各自主题另行补注，本轮未修改。
- `page_poison.c`：已核对 `include/linux/mm.h` 的 static-key wrappers、`mm/page_alloc.c` 的 free/alloc 配对、`mm/mm_init.c::mem_debugging_and_hardening_init()` 的开关发布以及 Kconfig 语义；确认 poison 是尽力诊断且不隔离损坏页。`mm_init.c` 关联段已有学习注释，其余关联区域覆盖不一，本轮未修改。
- `vma_exec.c`：已核对 `fs/exec.c::bprm_mm_init()/setup_arg_pages()`、`mm/vma.c::vma_expand()`、`mm/mremap.c::move_page_tables()`、`include/linux/ksm.h::ksm_execve()/ksm_exit()` 与 `mm/vma.h` 声明；确认只适用于未提交 exec mm，页表部分移动失败由整 mm teardown 清理。`fs/exec.c` 有部分学习注释，其余关联实现覆盖不一，本轮未修改。
- `mseal.c`：已核对 `mm/vma.h`/`mm/vma.c::vma_modify_flags()` 的“只准备 split/merge、不提交 flags”契约、`mm/{madvise,mremap}.c` 的 sealed 检查、`include/linux/mm.h` 64 位门控以及 `Documentation/userspace-api/mseal.rst` 的部分成功说明；关联源码缺少全文件系统注释，建议按基线另行处理，本轮未修改。
- `interval_tree.c`：已完整核对 `include/linux/interval_tree_generic.h::INTERVAL_TREE_DEFINE` 生成的 insert/remove/search/iterator 算法，并读取 `mm/vma.c` 的 i_mmap 与 anon_vma pre/post update 锁协议、`mm/rmap.c` AVC 生命周期、`mm/mmap.c::dup_mmap()` 同起点插入；这些关联区域学习注释覆盖不一，建议随各自基线目标补齐，本轮未修改。
- `bpf_memcontrol.c`：已核对 `include/linux/memcontrol.h` 与 `mm/memcontrol.c` 的统计验证/输出/flush 实现、`include/linux/btf.h` flags 及 `kernel/bpf/{btf,verifier}.c` 对 common hook 和 KF_* 的处理；`memcontrol.c` 已有部分学习注释，其余 BPF 关联区域覆盖不一，本轮未修改。
- 本轮五文件：`sysfs-kunit.h` 已核对 `mm/damon/sysfs.c` 的 alloc/add-targets 与 `include/kunit/test.h::kunit_skip`；前者读取区域缺少学习注释，后者部分覆盖。`memcontrol-v1.h` 已核对 `mm/memcontrol-v1.c` 的 soft-limit/OOM/memsw/kmem/tcpmem 实现和 `mm/memcontrol.c` 的 stock、统计、私有 ID、reparent helper；前者读取区域缺失、后者部分覆盖，`include/linux/memcontrol.h` 关联声明缺失。`zpdesc.h` 已核对 `mm/zsmalloc.c` 的分配/链/偏移/迁移、`mm/migrate.c` 的 movable_ops 分派及 `include/linux/{mm_types,page-flags,migrate}.h` 布局/标志，读取区域均缺失。`kmsan.h` 已核对 `mm/kmsan/{core,shadow,report,hooks,init}.c` 与 `include/linux/kmsan.h`，读取区域均缺失。`shuffle.c` 已核对 `mm/shuffle.h`（充分）、`mm/{Kconfig,Makefile}`（充分）及 `mm/{page_alloc,memory_hotplug,mm_init}.c` 调用点（部分覆盖）。缺失/部分覆盖区域建议仍按各自基线或主题顺序补注，本轮未越界修改。
- 本轮五文件：`kasan/tags.c` 已核对 `kasan.h` 的栈环/static-key 声明（缺失）、`common.c` 的 alloc/free 调用与 track 设置（部分覆盖）、`report_tags.c` 的写锁反向扫描（充分）以及 `hw_tags.c`/`sw_tags.c` 初始化入口（部分覆盖）。`damon/sysfs-common.c` 已核对 `sysfs-common.h`（充分）、`sysfs.c`/`sysfs-schemes.c` 的 range 构造、锁和 memcg 过滤调用（缺失）。`page_frag_cache.c` 已核对 `include/linux/{page_frag_cache,mm_types_task}.h` 的编码/包装/结构契约（缺失）、`net/core/skbuff.c` 的串行调用（缺失）和 `page_alloc.c::free_frozen_pages()`（部分覆盖）。`memtest.c` 已核对各架构 early 调用、`include/linux/memblock.h` 声明与关闭配置桩、`fs/proc/meminfo.c` 报告入口（均缺失，`arch/arm64/mm/init.c` 入口附近部分覆盖）。`hugetlb_sysctl.c` 已核对 `hugetlb_internal.h`（充分）、`hugetlb.c` 的池调整/初始化（部分覆盖）、`hugetlb_sysfs.c` 对称接口及 `include/linux/hugetlb.h` movable 门禁（缺失）。本轮只修改五个连续基线目标，关联缺失区域留待其各自顺序。
- 本轮五文件：`vma_init.c` 已核对 `mmap.c`/`nommu.c::mmap_init()`、`mmap.c::dup_mmap()`、`vma.c::__split_vma()/remove_vma()`、`memory.c::pfnmap_track_ctx_release()` 及 `include/linux/{mm,mm_types,mmap_lock,mm_inline}.h`（`include/linux/mm.h` 相关 helper 部分覆盖，其余读取区域缺失）；`cma_sysfs.c` 已核对 `cma.c::__cma_alloc_frozen()/__cma_release_frozen()` 与 `cma.h`（前者缺失，后者充分）；`dmapool_test.c` 已核对 `dmapool.c` create/alloc/free/destroy、`drivers/base/core.c::device_register()/device_del()/put_device()` 及 DMA mask helper（读取区域缺失或仅零散覆盖）；`hwpoison-inject.c` 已核对 `memory-failure.c` 的 RCU filter、`memory_failure()` 与 `unpoison_memory()`（缺失）及 hwpoison 文档；`fadvise.c` 已核对 `truncate.c::mapping_try_invalidate()`、`filemap.c::filemap_flush_range()`、`mm_inline.h::vma_has_recency()`、fd cleanup class、`madvise.c`/`readahead.c`/`f2fs/file.c` 调用点与 UAPI（仅 `filemap.c` 相关函数充分，其余读取区域缺失）。关联缺失源码建议继续按基线或对应子系统主题单独补注，本轮未越界修改。
- `maccess.c`：已核对 `include/linux/uaccess.h` 的公开声明和 `__get_kernel_nofault` 包装，以及 `arch/{mips/mm/maccess.c,arm/mm/fault.c}` 的架构地址许可覆盖；确认通用弱钩子只给出默认许可，架构可拒绝用户区、越界或不安全地址。关联声明和两个架构实现读取区域缺少本任务级系统学习注释，建议按对应主题另行补注，本轮未越界修改。
- `kmsan/init.c`：已核对 `mm/kmsan/shadow.c::kmsan_init_alloc_meta_for_range()/kmsan_setup_meta()`、`mm/mm_init.c::memblock_free_pages()/mm_core_init()` 与 `include/linux/kmsan.h` 声明；确认 false 返回把页块转交 KMSAN 暂扣，true 才允许数据块进入 buddy，且运行期开关在任务状态与遗留页收尾后发布。`mm/mm_init.c` 关联段已有学习注释，`shadow.c` 与公开头读取区域分别为缺失和缺失，本轮未越界修改。
- `percpu-stats.c`：已核对 `mm/percpu-internal.h` 的 `pcpu_chunk`/`percpu_stats` 字段、统计更新钩子及 map-bit helper，`include/linux/percpu.h::pcpu_alloc_info`，并定位 `mm/percpu.c` 的 chunk 槽移动/分配调用；确认所有列表、bitmap 和计数读取由 `pcpu_lock` 稳定，vmalloc/vfree 位于锁外。`percpu-internal.h` 与 `percpu.c` 读取区域缺少系统全文件学习注释，公开头读取区域缺失，本轮未越界修改。
- `damon/tests/vaddr-kunit.h`：已核对 `mm/damon/vaddr.c::__damon_va_three_regions()/__damon_va_init_regions()`、`mm/damon/core.c::damon_set_regions()/damon_new_target()/damon_destroy_target()` 与 `include/kunit/test.h::kunit_skip()`；确认测试头由 vaddr.c 在实现后直接 include，skip 会终止 case，set 只借用 ranges 且 target 销毁覆盖部分更新回滚。`vaddr.c`/`core.c` 读取区域缺少系统全文件学习注释，KUnit 头部分覆盖，本轮未越界修改。
- `page_idle.c`：已核对 `Documentation/admin-guide/mm/idle_page_tracking.rst` 用户 ABI/实现协议、`include/linux/page_idle.h` 的 32 位 page_ext flag 实现、`include/linux/page-flags.h` 的 64 位 folio flag 与 `mm/page_ext.c::page_idle_ops`；确认写 1 是 OR 语义、huge folio 只报告 head、清页表 young 后用 folio young 补偿 reclaim。关联头与 page_ext 读取区域缺少系统全文件学习注释，管理文档不属于源码注释目标，本轮未越界修改。
- 本轮五文件：`percpu-internal.h` 已核对 `mm/percpu.c` 的 bitmap/槽链/计费调用、`percpu-{vm,km,stats}.c` 后端与统计消费者及 `include/linux/percpu.h` 布局声明；`percpu-km.c` 充分，其余读取区域覆盖不一。`cma_debug.c` 已核对 `cma.c` 分配释放、`cma.h` range/bitmap/锁与 `cma_sysfs.c` 计账；两个头和 sysfs 文件充分，`cma.c` 读取区域缺失。`ptdump.c` 已核对 `pagewalk.c::walk_page_range_debug()`、`include/linux/ptdump.h` 及 x86/arm64 回调；公开头和 arm64 部分已有注释，pagewalk/x86 读取区域缺失。`mapping_dirty_helpers.c` 已核对 `pagewalk.c::walk_page_mapping()`、MMU notifier/TLB pending 契约与 vmwgfx 唯一调用链，读取区域覆盖不一。`usercopy.c` 已核对 `ucopysize.h` static-key 入口、SLUB 白名单实现、stack frame 架构契约和 hardening Kconfig；SLUB/Kconfig 相关处部分或充分，其余读取区域缺失。本轮未越界修改关联文件。
- 本轮五文件：`kmsan/report.c` 已核对 `mm/kmsan/{kmsan.h,core.c}` 的 reason/origin 链和范围分组、`include/linux/stackdepot.h` 的借用数组契约；`kmsan.h` 充分，`core.c` 与公开头读取区域缺失。`balloon.c` 已核对 `include/linux/balloon.h` 的 page/private/isolated 不变量、`mm/migrate.c` 的 movable_ops 页锁/引用协议及 virtio/VMware 驱动回调，读取区域缺失。`process_vm_access.c` 已核对 `lib/iov_iter.c::import_iovec()`、`kernel/fork.c::mm_access()`、`mm/gup.c::pin_user_pages_remote()`；fork 相关区域部分覆盖，其余缺失。`secretmem.c` 已核对 `mm/gup.c` 的慢/快 GUP 拒绝、`mm/mlock.c` 的不可解锁、`kernel/power/hibernate.c` 门禁及架构 direct-map 接口，读取区域覆盖不一。`page_counter.c` 已核对 `include/linux/page_counter.h` 布局/初始化、`mm/memcontrol.c::mem_cgroup_calculate_protection()`、hugetlb 调用点和 cgroup v2 保护文档；memcontrol 相关区域部分覆盖，其余源码缺失。关联缺失区域建议仍按各自基线或主题顺序补注，本轮未越界修改。
- 本轮五文件：`percpu-vm.c` 已核对 `percpu.c` 的 populate/depopulate、balance/reclaim 调用与 `percpu-internal.h`/`percpu-km.c` 后端契约（两个后端和私有头充分，核心读取区域缺失）。`kmsan/instrumentation.c` 已核对 `kmsan.h`、`core.c`、`shadow.c` 和公开 string ABI（私有头充分，其余读取区域缺失或部分）。`shrinker_debug.c` 已核对 `shrinker.c` 的 alloc/register/free 顺序、`internal.h` 配置桩、公开 shrinker 布局和 memcg iterator/get-by-id 引用协议（memcontrol 相关实现部分覆盖，其余缺失）。`page_table_check.c` 已核对公开 inline wrappers、page_ext ops、Kconfig 和多架构 set/clear 调用点（Kconfig 充分，其余读取区域缺失）。`page_reporting.c` 的关联关系见前述 `page_reporting.h` 索引；驱动侧另核对 Hyper-V 与 virtio order 选择。关联缺失区域留待各自基线，本轮未越界修改。
- `mincore.c`：已核对 `mm/pagewalk.c::walk_page_range()`、`mm/swap_state.c::swap_cache_get_folio()`、`mm/swapfile.c::get_swap_device()`、`include/linux/fs.h::file_owner_or_capable()/file_permission()` 与 `include/linux/pgtable.h::pte_batch_hint()`；确认 mmap/PTL/HugeTLB 锁边界、swap-device/folio 引用、文件缓存侧信道策略和体系结构 PTE batch 契约。`filemap.c` 相关区域部分覆盖，其余读取区域缺失，建议按各自基线或子系统主题后续补注，本轮未越界修改。
- `kmsan/shadow.c`：已核对 `mm/kmsan/{kmsan.h,core.c,init.c,instrumentation.c,hooks.c}`、`include/linux/kmsan.h`、`mm/vmalloc.c::vmap_pages_range_noflush()` 与 `mm/page_alloc.c` 的 alloc/free 钩子；确认 metadata 连续性、runtime guard、stack-depot origin、三块页配对及 vmap 失败回滚边界。私有 `kmsan.h` 与 `instrumentation.c` 充分，`init.c` 充分，`core.c`/`hooks.c` 与公开头读取区域缺失或部分覆盖，建议按基线后续补注，本轮未越界修改。
- `page_vma_mapped.c`：已核对 `include/linux/rmap.h::page_vma_mapped_walk{,_done,_restart}` 与状态结构、`mm/internal.h::vma_address_end()`、`mm/rmap.c` 的 try-to-unmap 消费路径、`mm/memory-failure.c::collect_procs_anon()` 及 `mm/huge_memory.c` migration PMD 路径；确认命中返回时 PTL ownership、PMD/PTE/HugeTLB 状态编码和同步查询用途。`rmap.c` 读取区域部分覆盖，其余关联区域缺失或部分覆盖，建议按基线后续补注，本轮未越界修改。
- `vmpressure.c`：已核对 `include/linux/vmpressure.h::struct vmpressure`、`mm/memcontrol.c::{memcg_to_vmpressure,vmpressure_to_memcg}`、`mm/vmscan.c::{shrink_one,shrink_node}` 调用及 `mm/memcontrol-v1.c` event_control 注册入口；确认本组/子树两套计数、work 生命周期、eventfd 借用关系与 cgroup v1/v2 消费边界。`memcontrol.c` 相关区域部分覆盖，其余读取区域缺失，建议按基线后续补注，本轮未越界修改。
- `swap_table.h`：已核对 `mm/swap.h` 的位数与 cluster 布局、`mm/swapfile.c` 的 cluster 辅助表分配释放及计数路径、`mm/swap_state.c` 的 swap-cache 状态转换、`mm/page_io.c` 的零页标记路径，以及 `mm/memcontrol-v1.c`/`mm/memcontrol.c` 的 MEMCG 辅助表调用；确认低位类型编码、额外计数溢出、RCU 读取与 cluster 锁写入、零页位图和关闭 MEMCG 时的桩语义。关联文件覆盖不一，建议按各自基线继续补注，本轮未越界修改。
- 本轮五文件：`shmem_quota.c` 已核对 `mm/shmem.c::shmem_enable_quotas()`、`fs/quota/dquot.c` 的 dqget/release/get-next 与 `include/linux/quota.h` 操作表契约；`damon/stat.c` 已核对 `mm/damon/core.c` 的 attrs/start/stop/call/system-RAM region、`paddr.c` 与 `include/linux/damon.h` 区域年龄语义；`kasan/quarantine.c` 已核对 `mm/kasan/common.c` 的 free/alloc 入口、`generic.c` 的 cache shrink/shutdown 以及 `kasan.h` metadata 布局；`sparse.c` 已核对 `mm/internal.h::sparse_init_one_section()`、`sparse-vmemmap.c` 后端、`include/linux/mmzone.h` section 布局和 `mm/mm_init.c` 启动调用；`kasan/report_generic.c` 已核对 `mm/kasan/report.c` 的报告组装与私有 `kasan.h` ABI。除 `mm/mm_init.c` 为部分覆盖外，上述关联读取区域均缺少本任务级系统学习注释，建议仍按各自基线或子系统主题后续补注；本轮未越界修改。
- 本轮五文件：`kmsan/core.c` 已核对 `mm/kmsan/{kmsan.h,shadow.c,report.c,hooks.c}` 的 metadata/origin ABI、报告消费和 hook 调用（前三者充分，`hooks.c` 缺失）；`damon/reclaim.c` 已核对 `mm/damon/{modules-common.c,modules-common.h,core.c,stat.c}` 与 `include/linux/damon.h` 的 context 构造、commit/start/stop/call（共享 helper 与 `stat.c` 充分，core/公开头读取区域缺失）；`debug.c` 已核对 `include/linux/mmdebug.h`、`mm/{page_alloc,page_owner}.c` 和 `include/linux/mm.h` 的 dump 调用与 VMA flags（公开 `mm.h` 部分覆盖，其余读取区域缺失）；`pgtable-generic.c` 已核对 `include/linux/{pgtable,mm}.h`、`mm/{memory,huge_memory,khugepaged}.c` 的 PTE 映射配对、THP deposit/withdraw 和 RCU 延迟释放（`mm.h` 部分覆盖，其余读取区域缺失）；`page_isolation.c` 已核对 `include/linux/page-isolation.h`、`mm/{page_alloc,memory_hotplug,cma,compaction}.c` 的连续分配/下线调用链（读取区域缺失或部分覆盖）。缺失/部分覆盖区域建议仍按各自基线或主题顺序补注，本轮未越界修改。
- 本轮返工五文件关联读取复用既有探查结论：`kmsan/hooks.c` 对照 `mm/kmsan/{kmsan.h,core.c,shadow.c,report.c}`（均已闭环）及 `include/linux/kmsan.h`（缺失），核对 runtime、metadata/origin 与报告 ABI；`gup_test.c` 对照 `mm/gup_test.h`（充分）、`mm/gup.c` 和 `tools/testing/selftests/mm/{gup_test.c,gup_longterm.c}`（源码学习注释缺失），核对 ioctl、get/pin 与长期会话；`dmapool.c` 对照 `mm/dmapool_test.c`（充分）、`include/linux/dmapool.h` 与 `drivers/base/core.c`（缺失或零散），核对 DMA 地址配对、设备列表/sysfs 和 devres；`memfd.c` 对照 `mm/gup.c`、`mm/shmem.c`、`mm/hugetlb.c`、PID namespace noexec 及 UAPI 定义（覆盖不一），核对 pin 排空、后端 file/seal 与策略；`damon/lru_sort.c` 对照 `mm/damon/{modules-common.c,modules-common.h,stat.c,reclaim.c}`（充分）、`core.c` 和 `include/linux/damon.h`（缺失），以及 `mm/damon/{Kconfig,Makefile}`（充分），核对 ctx/scheme commit、启停和 bool 生命周期。缺失/部分覆盖的关联源码建议仍按各自基线顺序补注，本轮未越界修改。
- 本轮五文件：`memremap.c` 已核对 `include/linux/memremap.h`、`mm/sparse-vmemmap.c`、`mm/memory_hotplug.c`、`mm/swap.c` 及驱动调用点；`page_ext.c` 已核对 `include/linux/page_ext.h`、`page_owner.c`、`page_table_check.c` 与相关客户端声明；`hugetlb_sysfs.c` 已核对 `hugetlb_internal.h` 和 `hugetlb.c` 的池调整/demote 实现；`memfd_luo.c` 已核对 `include/linux/kho/abi/memfd.h`、`include/linux/liveupdate.h`、`kernel/liveupdate/kexec_handover.c` 资源接口及已闭环的 `memfd.c`；`show_mem.c` 已核对 `mm/internal.h` 的过滤位、`page_alloc.c`/`oom_kill.c` 调用、`swap_state.c::show_swap_cache_info()`、`lib/alloc_tag.c::alloc_tag_top_users()`、`fs/proc/meminfo.c` 和 DAMON 节点消费者。`hugetlb_internal.h`、`page_table_check.c`、`memfd.c` 在所需契约上充分，其余关联读取区域缺失或部分覆盖，建议仍按各自基线/主题顺序补注；本轮未越界修改。
