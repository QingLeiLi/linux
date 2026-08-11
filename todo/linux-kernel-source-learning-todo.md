# Linux 内核源码学习注释待办

本文件记录在关联源码核对中发现、但尚未纳入完整学习注释范围的文件。

## 待处理文件

- [ ] `kernel/sched/syscalls.c`
  - 当前状态：部分覆盖。
  - 建议范围：`yield_to()`、`sched_setaffinity()`、`sched_getaffinity()` 及相关调度策略系统调用。
  - 补注重点：返回值类别、权限检查、task 生命周期、亲和性掩码与 runqueue 锁协议。

- [ ] `fs/exec.c`
  - 当前状态：`__set_task_comm()` 相关学习注释缺失。
  - 建议范围：`__set_task_comm()` 以及 exec 过程中 task 名称更新的调用链。
  - 补注重点：无锁读写、NUL 终止与零填充、trace/perf 通知时序。

- [ ] `kernel/fork.c`
  - 当前状态：与 `task_struct` 创建和 `vfork_done` 有关的区域部分覆盖。
  - 建议范围：task 创建、vfork completion 建立、失败回滚与新 task 发布路径。
  - 补注重点：引用获取、父子同步、发布边界以及失败清理顺序。

- [ ] `kernel/exit.c`
  - 当前状态：与 `vfork_done`、task 退出和最终释放有关的区域部分覆盖。
  - 建议范围：vfork completion 唤醒、退出状态发布、父进程通知和 task 最终释放路径。
  - 补注重点：锁、RCU、引用计数、等待者竞争及不可回滚边界。

- [ ] `drivers/acpi/scan.c`
  - 当前状态：`acpi_dma_configure_id()` 及其 DMA/IOMMU 配置分支缺少完整学习注释。
  - 建议范围：固件 DMA 属性解析、`DEV_DMA_NOT_SUPPORTED` 安装 `dma_dummy_ops`、
    IOMMU 延迟探测以及体系结构 DMA ops 建立过程。
  - 补注重点：设备初始化状态转换、`dev->dma_ops` 发布、`-EPROBE_DEFER` 与其他错误的区别。

- [ ] `include/linux/dma-map-ops.h`
  - 当前状态：`struct dma_map_ops`、`get_dma_ops()`、`set_dma_ops()` 和 dummy 声明缺少完整学习注释。
  - 建议范围：DMA 后端操作表契约以及 `CONFIG_ARCH_HAS_DMA_OPS` 两种配置。
  - 补注重点：各回调返回类别、缓冲区 ownership、同步回调、操作表生命周期与分派条件。

- [ ] `include/linux/dma-mapping.h`
  - 当前状态：通用 DMA API 的地址、错误哨兵与映射生命周期说明仅部分覆盖。
  - 建议范围：`DMA_MAPPING_ERROR`、map/unmap、SG、mmap 和 mask 设置接口。
  - 补注重点：CPU/设备地址空间区别、方向与 cache ownership、失败检查及 `CONFIG_HAS_DMA=n` stub。

- [ ] `arch/arm/mm/dma-mapping.c`
  - 当前状态：32 位 ARM coherent/IOMMU 分配、重映射和回收路径缺少完整学习注释。
  - 建议范围：`__alloc_remap_buffer()`、`__alloc_from_contiguous()`、`__iommu_get_pages()`、
    `arm_iommu_alloc_attrs()` 与 `arm_iommu_free_attrs()`。
  - 补注重点：连续页与离散 pages[] 的 ownership、KVA/IOVA 双重映射、可睡眠约束和失败回滚。

- [ ] `include/linux/vmalloc.h`
  - 当前状态：`VM_DMA_COHERENT` 仅有简短英文用途说明，缺少与 DMA remap 生命周期的对应关系。
  - 建议范围：`vm_struct` 标志及 vmap/vunmap 相关声明。
  - 补注重点：哪些标志允许用户映射、pages[] 是否被 vmalloc 层持有，以及描述符查询的生命周期。

- [ ] `arch/Kconfig`
  - 当前状态：`ARCH_HAS_DMA_OPS` 只有简短英文选择约束，缺少与通用 DMA 构建产物的对应说明。
  - 建议范围：`ARCH_HAS_DMA_OPS` 及其选择的 `DMA_OPS_HELPERS`。
  - 补注重点：只能由体系结构选择、传统 IOMMU 与 dma-iommu 的边界，以及对 `dma_map_ops`、
    `dummy.o` 和辅助回调编译范围的影响。

- [ ] `include/linux/of_reserved_mem.h`
  - 当前状态：`struct reserved_mem`、`struct reserved_mem_ops` 与声明宏缺少完整学习注释。
  - 建议范围：保留内存描述符、节点/设备生命周期回调、`RESERVEDMEM_OF_DECLARE()` 及关闭配置 stub。
  - 补注重点：`priv` 的实现方 ownership、早期 OF 扫描与设备绑定阶段、共享区域的 init/release 配对。

- [ ] `arch/hexagon/kernel/dma.c`
  - 当前状态：Hexagon cache 同步与默认 coherent pool 初始化路径缺少完整学习注释。
  - 建议范围：cache flush/invalidate 分支、`hexagon_dma_init()` 和 `core_initcall` 调用链。
  - 补注重点：非一致性 cache ownership、预留 16 MiB 区域来源、全局池建立时序与失败传播。

- [ ] `include/linux/cma.h`
  - 当前状态：CMA 区域声明、分配、释放与枚举接口只有简短声明/kernel-doc，学习注释不足。
  - 建议范围：最低对齐宏、declare/init/alloc/release/frozen/for_each 接口及各输出参数。
  - 补注重点：字节/页/页阶单位、可睡眠约束、页面引用 ownership、系统期区域对象与配置差异。

- [ ] `mm/cma.h`
  - 当前状态：`struct cma_memrange` 与 `struct cma` 仅部分英文说明，锁和生命周期缺少完整串联。
  - 建议范围：多 range、位图、available_count、lock/alloc_mutex、统计与 NUMA 字段。
  - 补注重点：每 bit 页阶、迁移/分配串行化、初始化 union 状态、系统期对象与 debug/sysfs 并发。

- [ ] `mm/cma.c`
  - 当前状态：CMA memblock 声明、页迁移分配、引用建立和释放核心路径缺少完整学习注释。
  - 建议范围：`cma_init_reserved_mem()`、`cma_declare_contiguous_nid()`、`cma_alloc()`、
    `cma_release()` 及位图、隔离、失败回滚辅助函数。
  - 补注重点：启动期对齐/zone 约束、mutex 与自旋锁分工、alloc_contig_range、页面引用和 bitmap 时序。

- [ ] `drivers/dma-buf/heaps/cma_heap.c`
  - 当前状态：CMA 区域枚举、dma-buf heap 建立及缓冲区 attach/map/release 路径缺少完整学习注释。
  - 建议范围：`add_cma_heaps()`、heap allocate、attachment SG 复制、vmap/mmap 与最终 release。
  - 补注重点：默认区去重、枚举终止哨兵、dma-buf/SG/page ownership、设备映射与并发生命周期。

- [ ] `drivers/base/bus.c`
  - 当前状态：bus notifier 注册/注销与设备绑定事件分发区域缺少完整学习注释。
  - 建议范围：`bus_register_notifier()`、`bus_unregister_notifier()` 及 bind/unbind 通知调用链。
  - 补注重点：subsys_private 引用、blocking notifier 生命周期、device lock 上下文和回调返回语义。

- [ ] `include/linux/device/bus.h`
  - 当前状态：bus notifier 事件已有英文 kernel-doc，但缺少与驱动绑定状态和锁上下文对应的学习说明。
  - 建议范围：`enum bus_notifier_event`、notifier 注册声明及 `struct bus_type` 相关字段。
  - 补注重点：UNBIND/UNBOUND 时序、回调 data 的 device 借用期、设备锁和 notifier 可睡眠边界。

- [ ] `include/linux/radix-tree.h`
  - 当前状态：radix tree 的 tag 与 RCU/外部锁契约以英文说明为主，缺少完整中文学习注释。
  - 建议范围：root 初始化、insert/lookup/delete、tag set/clear/get 与迭代接口。
  - 补注重点：item ownership、tag 聚合位、修改方互斥、RCU 查找结果生命周期及调用方同步责任。

- [ ] `include/linux/dma-direct.h`
  - 当前状态：dma-direct 地址换算、`struct bus_dma_region` 和加密地址辅助接口仅有英文说明。
  - 建议范围：CPU physical/DMA 双向范围翻译、range min/max、加密与未加密地址转换及配置 stub。
  - 补注重点：零结尾数组、地址空洞错误哨兵、SME 位与真实总线位的区别、映射发布后的只读生命周期。

- [ ] `include/linux/pci-p2pdma.h`
  - 当前状态：P2P 映射状态和 bus-address 辅助接口有 kernel-doc，但缺少完整学习注释。
  - 建议范围：`struct pci_p2pdma_map_state`、`pci_p2pdma_state()`、
    `pci_p2pdma_bus_addr_map()` 及关闭配置时的退化路径。
  - 补注重点：普通页/host-bridge/bus-address 三态、provider 缓存、页与 provider 生命周期及调用方 unmap 责任。

- [ ] `drivers/pci/p2pdma.c`
  - 当前状态：PCI P2P 拓扑判定、ACS 分流及 map state 更新实现缺少完整中文学习注释。
  - 建议范围：`calc_map_type_and_dist()`、`pci_p2pdma_map_type()`、
    `__pci_p2pdma_update_state()` 及 provider 发布/销毁路径。
  - 补注重点：switch/host bridge 拓扑、ACS redirect、XArray 缓存、RCU 保护和 provider/page ownership。

- [ ] `kernel/resource.c`
  - 当前状态：`walk_system_ram_range()` 等资源树遍历接口主要只有英文行为说明。
  - 建议范围：System RAM 查找与正/反向遍历、`walk_system_ram_range()` 及相关资源过滤辅助函数。
  - 补注重点：PFN 向上/向下取整、BUSY/SYSTEM_RAM 标志、回调提前终止语义和资源树并发边界。

- [ ] `include/linux/set_memory.h`
  - 当前状态：页面缓存/加密属性转换的通用声明与无体系结构实现 stub 缺少完整学习注释。
  - 建议范围：set/clear memory 属性接口，尤其是 `set_memory_decrypted()` 与
    `set_memory_encrypted()` 的配置分派。
  - 补注重点：页对齐与页数单位、潜在阻塞/TLB 同步、失败后的页面状态及 DMA 释放时的安全泄漏策略。

- [ ] `include/uapi/linux/map_benchmark.h`
  - 当前状态：DMA map benchmark 的 ioctl ABI、模式常量和结果/输入结构字段只有简短英文说明。
  - 建议范围：`DMA_MAP_BENCHMARK`、线程/时长/延时上限、single/SG 模式及 `struct map_benchmark`。
  - 补注重点：输入输出字段方向、100 ns 统计单位、single 与 SG 的 granule 差异、ABI 保留区和兼容性。

- [ ] `include/linux/debugfs.h`
  - 当前状态：debugfs 文件创建/删除及完整/short fops 接口主要依赖英文 kernel-doc。
  - 建议范围：`debugfs_create_file*()`、`debugfs_remove()`、错误指针容忍及配置关闭 stub。
  - 补注重点：inode `i_private` 借用、open 文件与 remove 并发、模块卸载责任和 debugfs 非稳定 ABI。

- [ ] `fs/debugfs/inode.c`
  - 当前状态：debugfs dentry 创建与递归删除实现缺少完整中文学习注释。
  - 建议范围：`__debugfs_create_file()`、full/short 包装、`debugfs_remove()` 与 lookup/remove 路径。
  - 补注重点：根目录选择、ERR_PTR/NULL 容忍、simple_recursive_removal 和打开文件的安全代理生命周期。

- [ ] `fs/libfs.c`
  - 当前状态：`simple_open()` 等通用伪文件系统辅助函数只有局部英文说明。
  - 建议范围：`simple_open()` 及与 simple_read/write/attribute 相关的常用辅助接口。
  - 补注重点：`inode->i_private` 到 `file->private_data` 的借用传递、引用归属和 release 责任。

- [ ] `drivers/base/devres.c`
  - 当前状态：自定义 devm action 的注册、释放与移除路径缺少完整学习注释。
  - 建议范围：`__devm_add_action()`、`devm_add_action_or_reset()`、release/remove/match 辅助函数。
  - 补注重点：LIFO 回滚时序、action/data 借用期、注册失败是否立即执行以及设备解绑并发边界。

- [ ] `include/linux/iommu-dma.h`
  - 当前状态：dma-iommu 的 map/sync/alloc/noncontiguous 与能力查询声明缺少完整学习注释。
  - 建议范围：物理/SG map、coherent/page/noncontiguous 分配、vmap/mmap 和 mapping-size 接口。
  - 补注重点：IOVA ownership、方向与 cache sync、负错误语义、页数组/SG 表和配置关闭 stub。

- [ ] `drivers/iommu/dma-iommu.c`
  - 当前状态：dma-iommu 作为通用 DMA API 后端的 IOVA、SWIOTLB、SG 和 noncontiguous 实现注释不足。
  - 建议范围：`iommu_dma_map_phys()`、`iommu_dma_map_sg()`、noncontiguous alloc/free/vmap/mmap、
    merge boundary 与最大/最优 mapping size。
  - 补注重点：IOVA 分配与回滚、IOMMU page 对齐、原 SG 字段可逆改写、P2P/SWIOTLB 分流和页数组寿命。

- [ ] `include/trace/events/dma.h`
  - 当前状态：DMA tracepoint 字段和 SG 截断逻辑以实现宏为主，缺少面向学习的事件生命周期说明。
  - 建议范围：map/unmap/sync/alloc/free 事件、`dma_map_sg` 与 `dma_map_sg_err`。
  - 补注重点：原 nents/映射 ents 的区别、128 项截断、地址/长度采样时机及错误事件参数。

- [ ] `include/linux/kmsan.h`
  - 当前状态：KMSAN DMA 钩子有英文 kernel-doc，但缺少与 DMA direction/ownership 对应的中文说明。
  - 建议范围：`kmsan_handle_dma()`、`kmsan_handle_dma_sg()` 及关闭 KMSAN 时的空实现。
  - 补注重点：TO_DEVICE 检查初始化状态、FROM_DEVICE 标记初始化、BIDIRECTIONAL 双向处理及 MMIO 排除。

- [ ] `include/linux/genalloc.h`
  - 当前状态：通用特殊内存池的结构、alloc/free/add/查询接口主要依赖英文 kernel-doc。
  - 建议范围：`struct gen_pool`/chunk、创建与发布、alloc/free、virt-to-phys、size/avail 和算法回调。
  - 补注重点：每 bit 最小阶、RCU chunk 遍历、原子位图并发、NMI 限制、地址 ownership 与返回哨兵。

- [ ] `lib/genalloc.c`
  - 当前状态：gen_pool 的 chunk 发布、RCU 查询、位图分配及 order-align 算法缺少完整中文学习注释。
  - 建议范围：create/add/destroy、alloc/free、has_addr、virt_to_phys、avail/size 和 first-fit 算法。
  - 补注重点：pool spinlock 与 RCU 分工、atomic bitmap 更新、chunk owner、范围溢出和销毁前零占用要求。

- [ ] `include/linux/workqueue.h`
  - 当前状态：`queue_work()`/`schedule_work()` 的内存序和重复排队语义只有英文 kernel-doc。
  - 建议范围：work 初始化、queue/schedule/cancel/flush 及 delayed-work 常用内联接口。
  - 补注重点：PENDING/RUNNING 状态、同一 work 不并发执行、运行中再次排队、发布屏障和取消/销毁边界。

## 验收要求

处理每个文件时，遵循 `doc/linux-kernel-source-learning-methodology.md`，仅添加学习注释，
保留原有代码和英文注释，并按第 17 章完成全文件内容验收与修改安全检查。
