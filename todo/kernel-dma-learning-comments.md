# kernel/dma 学习注释任务清单

## 使用规则

本清单是 `kernel/dma` 目录学习注释长任务的持久化进度源。发生会话中断或上下文压缩后，先读取
本文件并核对当前工作树，不凭对话记忆推断进度。

严格采用单文件闭环：

1. 开始前把目标文件标为 `[~]`，完整读取并建立函数、实体、英文注释和语义索引。
2. 仅追加中文学习注释；不改代码，不删除、改写或移动原有注释。
3. 按源码顺序以 1～5 个函数为一批修改，每批复读当前窗口并检查局部 diff。
4. 完整复读修改后的文件，按方法论第 17 章完成独立内容验收和修改安全检查。
5. 只有验收全部通过后才标为 `[x]`；一个 `[~]` 文件未闭环前不开始下一个文件。
6. 为核对契约而读取的跨目录源码只登记到关联读取记录；需要后续补注的文件追加到
   `todo/linux-kernel-source-learning-todo.md`，不在本任务中顺带修改。

状态：`[ ]` 未开始；`[~]` 正在处理或验收；`[x]` 已通过第 17 章单文件闭环。

## 文件进度（源码物理顺序）

- [x] `kernel/dma/Kconfig`
- [x] `kernel/dma/Makefile`
- [x] `kernel/dma/coherent.c`
- [x] `kernel/dma/contiguous.c`
- [x] `kernel/dma/debug.c`
- [x] `kernel/dma/debug.h`
- [x] `kernel/dma/direct.c`
- [x] `kernel/dma/direct.h`
- [x] `kernel/dma/dummy.c`
- [x] `kernel/dma/map_benchmark.c`
- [x] `kernel/dma/mapping.c`
- [x] `kernel/dma/ops_helpers.c`
- [x] `kernel/dma/pool.c`
- [x] `kernel/dma/remap.c`
- [~] `kernel/dma/swiotlb.c`

## 当前文件

- [~] `kernel/dma/swiotlb.c`
  - 文件职责与生命周期：待完整读取后填写。
  - 当前阶段：`pool.c` 已独立验收封账；准备完整读取本文件并建立索引，尚未修改。

## 已完成文件验收记录

完成一个文件后，在这里记录函数/实体/英文注释/路径/并发与生命周期清单、三个抽查点、关联读取、
零代码改动与零原注释改写证明，以及格式、checkpatch、编译的实际执行结果或未执行原因。

### kernel/dma/direct.c

- 版本与范围：基于 `b0df6b2ce1d6`；完整复读修改后 1095 行文件。
- 函数与实体清单：31 个函数均有直接位于声明前的专用中文学习头；覆盖 `zone_dma_limit`、
  DMA/物理地址双向换算、required mask 和 zone 选择、可寻址性过滤、加密属性切换、
  SWIOTLB/CMA/伙伴系统连续页、NO_KERNEL_MAPPING、体系结构/全局/原子池、coherent alloc/free、
  非一致性 page alloc/free、SG 同步与 map/unmap、PCI P2P bus address、sgtable、mmap、能力查询、
  全 RAM range 覆盖检查及固定 offset map 发布。参数、输出、返回类别、睡眠/锁边界、ownership、
  配对接口和失败回滚均按源码路径核对。
- 原英文注释：原文件 25 个注释起点逐项复核；SPDX/版权保留，文件职责、ZONE_DMA、zone 回退、
  atomic pool、脏缓存/opaque cookie、非一致性池、remap、高端页、P2P、32 位 mask、SME、RAM range、
  SWIOTLB 上限及 `dma_direct_set_offset()` kernel-doc 均原样保留并有相邻中文翻译或机制展开。
- 三个复杂抽查点：
  1. `dma_direct_alloc()`/`dma_direct_free()`：核对体系结构、全局池、原子池、连续页、remap、
     uncached 与解密路径的选择顺序；确认映射失败释放，而解密或重新加密失败有意泄漏以保护页状态。
  2. `dma_direct_map_sg()`/sync/unmap：核对普通 RAM、穿 host bridge 的 P2P、直接 PCI bus address
     三态；确认 device 方向先 bounce copy 后 cache sync、CPU 方向顺序相反，失败回滚跳过 CPU sync。
  3. `dma_direct_all_ram_mapped()`/`dma_direct_set_offset()`：核对零结尾 `bus_dma_region` 数组、跨多个
     range 的 PFN 推进、资源遍历提前终止及两条目分配后再发布的初始化边界。
- 关联读取：`kernel/dma/direct.h` 的 map/unmap 内联契约；`kernel/dma/mapping.c:700-735` 的通用
  page 分派；`kernel/dma/pool.c:249-308` 的 pool 返回 page/CPU 地址及归还；
  `include/linux/dma-direct.h:1-122` 的 range/encryption 翻译；
  `include/linux/pci-p2pdma.h:145-225`、`drivers/pci/p2pdma.c:680-755,1080-1150` 的 P2P 三态；
  `kernel/resource.c:520-585` 的 RAM walker；`include/linux/dma-mapping.h:470-530` 的非一致性 page API；
  `include/linux/set_memory.h` 的加密属性声明。跨目录缺口已登记到总 TODO，未越界修改。
- 修改安全：`CODE_TOKENS_UNCHANGED`；`git diff --numstat` 为 `417 0`，新增均为注释/空行，
  原代码、声明、预处理、原注释及缩进无删除或改写；31/31 函数头邻接门禁通过；未引入语言标签前缀。
- 工具验证：`git diff --check` 通过；checkpatch 为 0 errors、119 warnings、0 checks，全部 warning
  均为含中文 UTF-8 的 100 列行宽提示，无其他告警。仓库无 `.config`，目标对象编译记为
  `NO_CONFIG`；本机 Apple Clang 17.0.0 也低于本树声明的 17.0.1 最低版本。
- 验收结论：`全文件完成`，通过方法论第 17 章单文件闭环。

### kernel/dma/map_benchmark.c

- 版本与范围：基于 `b0df6b2ce1d6`；完整复读修改后 824 行文件。
- 函数与实体清单：19 个函数均有直接位于完整声明前的专用中文学习头；4 个核心结构体及
  single/SG 操作表、模式索引、file_operations、platform/PCI driver、module init/exit 和元数据均
  已说明。覆盖 debugfs ioctl 会话、每线程私有缓冲、DMA ownership、atomic64 聚合、task/device
  引用、NUMA 绑定、mask 临时修改和 devm/debugfs 生命周期；参数、返回、睡眠/并发及回滚逐项核对。
- 原英文注释：原文件 13 个注释起点逐项复核；SPDX/版权保留，非一致性缓存预热、SG granule、
  模拟 DMA、平方和、cond_resched 防饿死、旧统计清零、线程 join、均值/标准差、DMA mask 恢复及
  单设备 debugfs 限制均原样保留并有相邻中文翻译和机制展开。
- 三个复杂抽查点：
  1. single/SG prepare-unprepare：核对 `__free(kfree)` ownership 转移、exact-pages 配对、SG 第 i 页
     失败时仅释放已完成项，以及 map 成功后必须按原输入 nents 和方向 unmap。
  2. `map_benchmark_thread()`/`do_map_benchmark()`：核对完整样本才增加 loops、cond_resched 停止
     可达性、创建中途 stop 回滚、唤醒前清零、get_task_struct 与 kthread_stop_put 消费引用及全部
     join 后才计算统计；记录 `u64` 平方和无溢出检测的源码边界。
  3. ioctl/probe/module 生命周期：核对参数校验、设备引用、benchmark 成败后恢复旧 DMA mask、
     同名根 debugfs 限制、devm action 删除及 PCI/platform 注册失败逆序回滚；记录无会话 mutex、
     `dma_bits` 未单独限宽的当前实现边界，未越权修改代码。
- 关联读取：`include/uapi/linux/map_benchmark.h` 的 ioctl ABI 与字段单位；
  `include/linux/cleanup.h` 的 `return_ptr()` ownership；已验收 `include/linux/kthread.h`、
  `kernel/kthread.c` 的 `kthread_stop_put()`；`include/linux/debugfs.h:90-132`、
  `fs/debugfs/inode.c:760-790` 的创建/删除契约；`fs/libfs.c:735-755` 的 `simple_open()`；
  `drivers/base/devres.c:775-810` 的 devm action。未覆盖的跨目录文件均登记总 TODO。
- 修改安全：`CODE_TOKENS_UNCHANGED`；`git diff --numstat` 为 `246 0`，新增均为注释/空行，
  原代码、声明、预处理、原注释及缩进无删除或改写；19/19 函数头邻接门禁通过；无语言标签前缀。
- 工具验证：`git diff --check` 通过；checkpatch 为 0 errors、77 warnings、0 checks，全部 warning
  均为中文 UTF-8 触发的 100 列行宽提示，无其他告警。仓库无 `.config`，目标对象编译记为
  `NO_CONFIG`；本机 Apple Clang 17.0.0 低于本树最低 17.0.1。
- 验收结论：`全文件完成`，通过方法论第 17 章单文件闭环。

### kernel/dma/mapping.c

- 版本与范围：基于 `b0df6b2ce1d6`；完整复读修改后 1669 行文件。
- 函数与实体清单：52 个函数（含 `CONFIG_DMA_NEED_SYNC` 两种实现）均有直接位于声明前的专用
  中文学习头；`dma_default_coherent`、`CREATE_TRACE_POINTS`、`dma_devres` 及全部导出 API 已说明。
  覆盖 managed coherent devres、direct/IOMMU/ops 选择、phys/page/SG/resource streaming map、
  KMSAN/trace/debug、CPU/设备 sync、need-sync/unmap、sgtable 风险、pgprot/mmap、coherent alloc/free、
  非一致性 page/noncontiguous/vmap/mmap、P2P、mask 发布和 mapping-size/merge-boundary 查询。
- 原英文注释：原文件 31 个注释起点逐项复核；SPDX/版权保留，managed API、direct bypass、SG
  kernel-doc 与错误语义、SWIOTLB 逐地址 sync、dma_get_sgtable 风险、pgprot/mmap、32 位 required
  mask、compound/zone、IRQ free、P2P bypass、mask 裁剪、addressing limited 及 merge boundary 均
  原样保留并有相邻中文翻译/机制展开；配置尾注保持代码原状并由对应配置分支注释解释。
- 三个复杂抽查点：
  1. `dma_go_direct()` 与 phys/page/SG map/unmap：核对 coherent/streaming 两种 mask、dma-iommu 禁止
     bypass、MMIO/CC_SHARED、REQUIRE_COHERENT、KMSAN/trace/debug 时序及原 nents 的 unmap 契约。
  2. managed/coherent/page/noncontiguous 生命周期：核对 devres 先建记录后发布、显式 free 防二次释放、
     设备专用池优先、IRQ/vunmap 边界、single-SGT 失败逆序回滚和 dma-iommu 页数组/连续 IOVA handle。
  3. sync/mask/capability：核对 SWIOTLB 首次映射清除 skip、逐地址 need-sync、DMA debug 强制 unmap、
     dma_addr_t 裁剪后发布 mask、direct RAM range 覆盖以及 hard/optimal mapping size 的取小关系。
- 关联读取：已验收 `kernel/dma/direct.h`/`direct.c` 的 direct 契约；
  `include/linux/dma-mapping.h:530-610` 的 sgtable sync/unmap 内联；
  `include/linux/iommu-dma.h` 声明及 `drivers/iommu/dma-iommu.c:930-1110,1200-1255,1400-1465,
  1738-1775` 的 IOVA、SG、noncontiguous 与能力实现；`include/linux/kmsan.h:180-218` 的方向语义；
  `include/trace/events/dma.h:270-360` 的 nents/ents 与截断事件。跨目录缺口已登记总 TODO，未修改。
- 修改安全：`CODE_TOKENS_UNCHANGED`；`git diff --numstat` 为 `650 0`，新增均为注释/空行，原代码、
  声明、预处理、原注释及缩进无删除或改写；52/52 函数头邻接门禁通过；无语言标签前缀。
- 工具验证：`git diff --check` 通过；checkpatch 为 0 errors、121 warnings、0 checks，全部 warning
  均为中文 UTF-8 触发的 100 列行宽提示，无其他告警。仓库无 `.config`，目标对象编译记为
  `NO_CONFIG`；Apple Clang 17.0.0 低于本树最低 17.0.1。
- 验收结论：`全文件完成`，通过方法论第 17 章单文件闭环。

### kernel/dma/pool.c

- 版本与范围：基于 `b0df6b2ce1d6`；完整复读修改后 467 行文件。
- 函数与实体清单：13 个函数均有直接位于声明前的专用中文学习头；DMA/DMA32/kernel 三类
  `gen_pool`、容量统计、`atomic_pool_size`、静态扩容 work、early_param、debugfs、initcall 与配置
  宏均已说明。覆盖默认容量、CMA zone 判断、伙伴逐阶回退、cache prep/remap/永久解密、后台翻倍、
  候选池不放宽选择、物理地址回调、双输出 alloc 和按地址来源探测 free。
- 原英文注释：原文件 10 个注释起点逐项复核；SPDX/版权保留，coherent_pool 容量、动态扩容、CMA
  不跨 zone、最大 page order、原子池永久未加密、有意泄漏、默认 RAM 比例及全部内存可能位于 DMA
  zone 的说明均原样保留并有相邻中文翻译和机制展开。
- 三个复杂抽查点：
  1. `atomic_pool_expand()`：核对 CMA/伙伴逐阶取页、coherent cache 准备、remap/直接别名、解密后
     gen_pool 发布及失败标签；明确记录 gen_pool-add 原错误被重新加密结果覆盖、可能返回 0/发布空池。
  2. init/resize/work：核对命令行与 RAM 比例夹取、managed zone 条件建池、单池失败仍尝试其他池、
     低水位 schedule_work 合并及以当前总量扩容近似翻倍，成功池永不收缩或正常重新加密。
  3. `dma_guess_pool()`/alloc/free：核对首次 gfp 首选与缺失替代、失败后只向更受限 zone 推进、
     gen_pool 原子位图、phys_addr_ok 失败归还、低水位异步扩容、清零后双视图返回和完整区间来源探测。
- 关联读取：已验收 `kernel/dma/contiguous.c`、`direct.c` 的 CMA/direct 调用方；
  `include/linux/genalloc.h:90-185` 与 `lib/genalloc.c:130-240,535-615,700-745` 的 spinlock/RCU chunk、
  原子位图、地址和容量查询；`include/linux/workqueue.h:688-770` 的 schedule_work 去重与内存序；
  `include/linux/cma.h`、`mm/cma.c`、`include/linux/set_memory.h` 的未覆盖实现已在总 TODO 中登记。
- 修改安全：`CODE_TOKENS_UNCHANGED`；`git diff --numstat` 为 `158 0`，新增均为注释/空行，原代码、
  声明、预处理、原注释及缩进无删除或改写；13/13 函数头邻接门禁通过；无语言标签前缀。
- 工具验证：`git diff --check` 通过；checkpatch 为 0 errors、49 warnings、0 checks，全部 warning
  均为中文 UTF-8 触发的 100 列行宽提示，无其他告警。仓库无 `.config`，目标对象编译记为
  `NO_CONFIG`；Apple Clang 17.0.0 低于本树最低 17.0.1。
- 验收结论：`全文件完成`，通过方法论第 17 章单文件闭环。

### kernel/dma/coherent.c

- 版本与范围：基于 `b0df6b2ce1d6`；完整复读修改后 748 行文件。
- 函数清单：21 个函数均有直接位于声明前的专用中文学习头。覆盖 pool 查询与 DMA 基址换算；
  `dma_init_coherent_memory()`、`_dma_release_coherent_memory()`、挂接/声明/整池释放；设备池的内部及
  公开 alloc/release/mmap；全局池的 alloc/release/mmap/init；reserved-memory 的设备 init/release、
  节点 setup 和默认池 init。逐项核对全部参数、输出参数、返回类别、调用上下文、副作用与后续去向。
- 实体清单：`struct dma_coherent_mem` 的 7 个字段均说明地址空间、单位、锁保护与 ownership；三个全局
  静态实体（默认池指针及两个 `__initdata` 暂存量）、`rmem_dma_ops` 回调表，以及所有局部变量均说明
  角色、有效期或锁语义。`dev->dma_mem` 是借用槽，设备树共享 pool 由 `rmem->priv` 持有且系统期常驻。
- 英文注释清单：文件说明 2～5、设备声明说明 188～203、分配成功说明 295～297、三个公开 API 的
  kernel-doc（313～330、385～400、459～478）、reserved-memory 章节说明 589～591 和能力警告 643
  均逐字保留，并在紧邻位置补齐完整翻译与学习展开；SPDX 按规则豁免。
- 路径清单：零长度/映射/元数据/位图失败及逆序回滚，设备为空/已有池，位图容量超限/碎片耗尽，
  设备池“存在即接管”、释放命中/未命中、mmap 归属/边界/实际映射错误，全局池未初始化/耗尽，
  reusable、ARM no-map、默认池重定义和 reserved-memory 惰性共享等分支全部覆盖。
- 并发与生命周期：位图由 `spin_lock_irqsave()` 保护，分配块置位后在锁外清零；挂接、整池销毁和
  `rmem->priv` 首次发布依赖外层初始化/拆除串行化。整池释放前必须停止 DMA、归还所有块并阻止读者；
  mmap 期间缓冲区/pool 必须存活。全局指针 `__ro_after_init`，单设备 reserved-memory release 只解绑，
  不销毁共享 pool。初始化/memremap 路径可睡眠，位图 alloc/release 路径不睡眠。
- 注释抽查：抽查 `dma_init_coherent_memory()` 可从注释恢复构造阶段与逆序回滚；抽查
  `__dma_alloc_from_coherent()` 可推导二次幂位图占用、IRQ 锁及锁外清零安全性；抽查
  `rmem_dma_device_init()` 可推导惰性共享、每设备地址换算、挂接错误被忽略及单设备解绑不销毁。
  三项均通过初学者复述和开发者顺序/ownership 推理。
- 关联读取：`kernel/dma/mapping.c:500-715`（设备专用池在通用 alloc/free 中的接管语义，本任务待处理）、
  `kernel/dma/direct.c:205-245,312-340,525-555`（全局池 alloc/free/mmap 的调用次序，本任务待处理）、
  `include/linux/dma-map-ops.h:125-230`（声明与关闭配置 stub，缺失，原总 TODO 项继续有效）、
  `include/linux/of_reserved_mem.h:1-180`（描述符、回调和声明宏，缺失，已登记总 TODO）、
  `arch/hexagon/kernel/dma.c:25-50`（体系结构全局池初始化调用者，缺失，已登记总 TODO）。
- 修改安全：`git diff --numstat` 为 338 新增、0 删除；逐行审计所有新增行均为注释，故代码 token
  不变且原注释零删除/零改写；基线与当前函数清单均为 21 项；`git diff --check` 通过。checkpatch
  为 0 errors、100 个中文 UTF-8 显示宽度超过 100 列的 warnings，无其他告警。仓库为 `NO_CONFIG`，
  Apple Clang 17.0.0 仍低于当前树要求的 17.0.1，未执行目标对象编译。
- 结论：已按方法论第 17 章完成强制单文件验收，状态 `[x]`。

### kernel/dma/contiguous.c

- 版本与范围：基于 `b0df6b2ce1d6`；完整复读修改后 926 行文件。
- 函数清单：22 个逻辑函数全部覆盖；`cma_early_percent_memory()` 与 `dma_numa_cma_reserve()` 各有
  启用/关闭配置双实现，故共 24 个物理定义。包括区域插入/枚举、cma=/numa_cma=/cma_pernuma=
  解析、设备区域选择、百分比容量、NUMA/默认/自定义早期保留、体系结构弱修正钩子、两组运行期
  alloc/release，以及 6 个 reserved-memory 验证/创建/设备回调。均说明全部参数、返回与上下文。
- 实体清单：默认、枚举、每 NUMA 节点 CMA 指针及计数的系统期借用关系已说明；命令行大小/base/
  limit、逐节点/统一容量和 configured 标记的 `__initdata` 生命周期已说明；CMA_SIZE/size_bytes、
  pr_fmt、early_param、EXPORT 与 OF 声明宏均覆盖。所有局部变量含地址单位、输出槽、解析游标、
  页数/页阶和回退角色；`rmem_cma_ops` 五个回调字段逐项说明。
- 英文注释清单：原文件 2～36 的 CMA 原理说明、7 个 kernel-doc、默认大小说明、NUMA 优先级、
  用户 limit、默认区列表一致性、两处体系结构 fixup、可睡眠限制、三处分配来源/free 回退说明、
  reserved-memory 章节说明全部逐字保留并紧邻翻译；版权/作者和 SPDX 按规则豁免。
- 路径清单：命令行覆盖四种 Kconfig 大小策略、固定/范围保留、默认区已存在/保留失败/列表满，
  NUMA 显式与自动容量、离线/零容量/单节点失败继续，专用 CMA 无回退、一页/非阻塞退回上层、
  NUMA zone 限制与默认回退，release 的专用/实际页节点/默认/buddy 逆向路径，以及 DT 的
  reusable/no-map/cma= 优先级/对齐/初始化/列表告警全部覆盖。
- 并发与生命周期：memblock、参数和区域指针发布均在串行启动期；CMA 区域由 MM 核心持有并系统期
  常驻，设备与枚举数组只借用。运行期分配可能迁移页并睡眠，CMA 核心用 mutex/自旋锁保护位图；
  调用者取得页面 ownership，释放前须停止 DMA/CPU 用户并成对传回大小。设备解绑只清借用指针。
- 注释抽查：`dma_contiguous_reserve()` 可恢复命令行/Kconfig 优先级及失败对 NUMA 的影响；
  `dma_alloc_contiguous()`/`dma_free_contiguous()` 可恢复专用、NUMA、默认与 buddy 的非对称回退；
  `rmem_cma_setup()` 可恢复验证、对齐、CMA 发布和非致命列表失败。三项均通过初学者复述和开发者
  顺序、ownership 与失败回滚推理。
- 关联读取：`include/linux/cma.h:1-64`、`mm/cma.h:1-102`、`mm/cma.c:220-520,900-1055`
  （CMA 对象、锁、memblock 声明、迁移分配与逐页 release 契约，缺失，已登记总 TODO）；
  `drivers/dma-buf/heaps/cma_heap.c:380-430`（区域枚举消费者，缺失，已登记总 TODO）；
  `include/linux/dma-map-ops.h:85-150`（DMA_CMA 声明与关闭 stub，原总 TODO 有效）；复用前一文件已读
  `include/linux/of_reserved_mem.h:1-180`（回调阶段，已登记）。本任务内还核对 `pool.c`、`ops_helpers.c`、
  `direct.c` 调用点；通过 `rg` 只读定位 arm64/riscv/csky/arm/microblaze/x86/mips/loongarch/s390/xtensa
  启动保留点、arm DMA 修正钩子和 dma-iommu 分配点，未扩展修改，ARM DMA 文件原总 TODO 已覆盖。
- 修改安全：`git diff --numstat` 为 338 新增、0 删除；逐行差异审计确认全部新增为注释或空行，
  代码 token 与原注释不变；`git diff --check` 通过。checkpatch 为 0 errors、97 个中文 UTF-8 显示
  宽度 warnings、0 checks、无其他 warning。仓库为 `NO_CONFIG`，未执行目标对象编译。
- 结论：已按方法论第 17 章完成强制单文件验收，状态 `[x]`。

### kernel/dma/debug.c

- 版本与范围：基于 `b0df6b2ce1d6`；原文件 1670 行和修改后 2390 行均按有界窗口完整复读。
- 函数清单：59 个函数全部具有直接位于声明前的专用学习头；覆盖禁用/驱动过滤、地址哈希与
  best-fit、cacheline radix 重叠计数、entry 对象池、内核/debugfs dump、过滤器文件操作、初始化/
  命令行、bus 解绑泄漏通知、unmap/sync/栈/text/SG 校验，以及 12 个公开 DMA debug hook。
  `err_printk` 宏另有独立接口说明；所有参数、输出、返回、调用上下文和副作用逐项核对。
- 实体清单：`dma_debug_entry` 13 个逻辑字段和 `hash_bucket` 两字段均说明单位、借用关系及锁保护；
  两组枚举、match_fn、哈希/entry/cacheline 常量、四类锁、radix tree、free-list、错误显示控制、
  driver filter、字符串表、file_operations、init/setup/export 宏均覆盖。所有局部变量说明用途。
- 英文注释清单：`git grep` 得到原文件 63 个 `/*` 起点；版权/作者块按规则豁免，其余 62 个
  kernel-doc、章节和行内英文说明均逐字保留并紧邻翻译。特别标出两处历史描述与当前代码差异：
  hash 位号以 SHIFT/MASK 表达式为准，当前 SG map 为每个 mapped entry 分配调试对象。
- 路径清单：未初始化/显式禁用快速旁路、预分配部分/完全失败、原子扩容与 cacheline ENOMEM
  全局禁用、重叠映射告警抑制组合、唯一/完美/歧义查找、跨桶包含查找、map-error 检查状态、
  unmap 的 size/type/paddr/SG count/direction/attrs 全部诊断、CPU/device sync、栈/text/rodata、
  MMIO 映射 RAM、SG 段长/边界、coherent/pages 及设备解绑泄漏路径全部覆盖。
- 并发与生命周期：每桶锁保护活动链表，radix_lock 保护 cacheline key/tag，free_entries_lock 保护
  对象池与水位，driver_name_lock 保护过滤名称发布；锁之间不嵌套，unmap 先释放桶锁再撤销 radix，
  避免 ABBA。error_count/显示配额按原设计接受近似竞争；hook 使用 GFP_ATOMIC entry 扩容且不应
  睡眠，debugfs 用户复制在锁外可睡眠。entry 不拥有 device/page/真实 DMA 映射，失败只漏记。
- 注释抽查：`__hash_bucket_find()`/`bucket_find_contain()` 可恢复歧义拒绝和最终桶仍加锁契约；
  `add_dma_entry()`/`dma_entry_free()` 可恢复哈希、radix、free-list 的发布/逆序回收与锁序；
  `check_unmap()` 可恢复“报告所有参数错误但仍消费记录”的完整控制流。三项均通过初学者复述和
  开发者并发、ownership、错误路径推理。
- 边界披露：当前 `dma_debug_entries=0` 未被拒绝，空池会被视为初始化成功，首次扩容后的增长提示
  对初始请求数取模/相除，存在零除风险；注释如实记录，遵守 comment-only 范围未修改实现。
- 关联读取：`kernel/dma/mapping.c:177-226,259-265,349-442,661-756,813-837`（12 类 hook 的
  真实操作前后次序，本任务内待处理）；复用已验收 `kernel/dma/debug.h` 的启用/关闭声明契约；
  `drivers/base/bus.c:1048-1075`、`include/linux/device/bus.h:245-305`（blocking notifier 与
  UNBOUND_DRIVER 锁上下文，缺失，已登记总 TODO）；`include/linux/radix-tree.h:145-165`（item
  生命周期/tag 并发契约，缺失，已登记）。只读定位 PCI 与 pseries VIO 的 bus 注册调用点；它们是
  平台初始化胶水，本次不扩展修改。`lib/radix-tree.c` 只做符号索引，已有中文学习注释未登记缺口。
- 修改安全：`git diff --numstat` 为 720 新增、0 删除；逐行审计所有新增均为注释或空行，代码 token、
  59 个函数定义和全部原注释不变；`git diff --check` 通过。checkpatch 为 0 errors、189 个中文 UTF-8
  显示宽度 warnings、0 checks、无其他 warning。仓库为 `NO_CONFIG`，未执行目标对象编译。
- 结论：已按方法论第 17 章完成强制单文件验收，状态 `[x]`。

### kernel/dma/dummy.c

- 版本与范围：基于 `b0df6b2ce1d6`；完整复读修改后 183 行文件。
- 函数清单：`dma_dummy_mmap()`（头 17～34，声明 35，体止 41）、
  `dma_dummy_map_phys()`（43～58，声明 59，体止 64）、`dma_dummy_unmap_phys()`（66～80，
  声明 81，体止 93）、`dma_dummy_map_sg()`（95～111，声明 112，体止 118）、
  `dma_dummy_unmap_sg()`（120～134，声明 135，体止 147）、`dma_dummy_supported()`（149～160，
  声明 161，体止 165）；六项均核对职责、调用位置、全部参数、上下文、返回、副作用与 ownership。
- 实体清单：文件级失败后端不变量已说明；唯一全局实体 `dma_dummy_ops`（167～183）已说明 const
  生命周期、安装者、借用关系、全部已设置回调和 NULL 回调的通用层后果；无局部变量、结构体定义。
- 英文注释清单：原文 2～4、84～87、139～141 均逐字保留，并在紧邻下方补充完整中文语义；
  SPDX 许可证按规则豁免。
- 路径清单：mmap `-ENXIO`、物理映射 `DMA_MAPPING_ERROR`、SG `-EINVAL`、mask false，以及两个
  不可达 unmap 告警路径全部覆盖；该文件没有成功、资源分配或 cleanup 路径。
- 并发与生命周期：操作表为内核镜像期只读对象；设备仅借用指针；所有 map 都在发布 DMA 地址或
  转移 cache ownership 前失败；无锁、RCU、引用、屏障或可睡眠阶段，unmap 仅以一次性告警查错。
- 注释抽查：以 mmap、SG、mask 三条代表性契约完成初学者和开发者视角复述；本文件全为短失败
  stub，没有三个复杂函数，故“复杂函数”条件按实际结构豁免，但六个函数均逐项验收。
- 关联读取：`drivers/acpi/scan.c:1653-1679`（安装条件与初始化顺序，学习注释缺失）、
  `include/linux/dma-map-ops.h:16-91,430`（操作表与分派契约，缺失）、
  `include/linux/dma-mapping.h:106-180,220-310`（错误哨兵和无 DMA stub，部分覆盖）、
  `kernel/dma/mapping.c:120-360,550-623,625-759,873-944`（通用分派、返回折叠和 mask 语义，
  本任务内待处理）、`kernel/dma/Makefile:1-12`（CONFIG_ARCH_HAS_DMA_OPS 构建条件，本任务内待处理）。
  三个跨目录缺口已登记到总 TODO，没有越权修改关联源码。
- 修改安全：`git diff --numstat` 为 125 新增、0 删除；零代码改动、零原注释删除或改写；
  `git diff --check` 通过。checkpatch 为 0 errors、43 个中文 UTF-8 显示宽度超过 100 列的 warnings，
  无其他告警。仓库没有 `.config`（`NO_CONFIG`），因此未执行目标对象编译。
- 结论：已按方法论第 17 章完成强制单文件验收，状态 `[x]`。

### kernel/dma/remap.c

- 版本与范围：基于 `b0df6b2ce1d6`；完整复读修改后 163 行文件。
- 函数清单：`dma_common_find_pages()`（头 9～24，声明 25，体止 37）、
  `dma_common_pages_remap()`（英文 39～42、中文头 43～64，声明 65，体止 78）、
  `dma_common_contiguous_remap()`（英文 80～83、中文头 84～104，声明 105，体止 126）、
  `dma_common_free_remap()`（英文 128～130、中文头 131～149，声明 150，体止 163）；四项均逐项
  核对调用位置、全部参数、可睡眠约束、阶段、返回值、副作用和 ownership。
- 实体清单：文件不定义结构体或全局/静态变量；四个函数的 `area`、`vaddr`、`count`、`pages`、
  `i` 均说明业务角色、有效期和所有权。`caller`、`size` 在相应实现中未使用的事实及后果已明确。
- 英文注释清单：39～42、80～83、128～130 原文逐字保留并紧邻补齐翻译与学习说明；许可证、
  版权按规则豁免。
- 路径清单：vmap 成功/失败、临时 pages[] 分配失败、连续页目录构造、有效 free、非法地址告警
  并保留映射等路径全部覆盖；无 goto，资源清理顺序为“销毁 KVA 后由上层释放数组/物理页”。
- 并发与生命周期：vmap/vunmap 可睡眠且不能在中断上下文；vm_struct 和 area->pages 都是借用指针；
  地址发布前完成 pages[] 登记；调用方必须串行化 find/free 并在 vunmap 前停止所有 KVA 用户；
  无本地锁、RCU、引用或显式内存屏障，物理页 ownership 始终留在分配后端。
- 注释抽查：以离散页 remap、连续页 remap、free 三条路径完成初学者复述和开发者顺序推理；确认
  临时 pages[] 可在 vmap 后释放、持久 pages[] 只登记不转移、非法地址不能误拆普通 vmalloc 区域。
- 关联读取：`mm/vmalloc.c:3752-3772,3991-4085,5381-5445`（find/vmap/vunmap 与用户映射契约，
  现有学习注释充分）、`include/linux/vmalloc.h:26`（VM_DMA_COHERENT，缺失，已登记总 TODO）、
  `arch/arm/mm/dma-mapping.c:328-435,1016-1164`（离散/连续页调用及回收顺序，缺失，已登记总 TODO）、
  `include/linux/dma-map-ops.h:195-212`（接口声明，缺失，已登记总 TODO）、
  `kernel/dma/direct.c:245-300`、`kernel/dma/pool.c:80-143`（本目录调用者，本任务内待处理）。
- 修改安全：`git diff --numstat` 为 91 新增、0 删除；零代码改动、零原注释删除或改写；
  `git diff --check` 通过。checkpatch 为 0 errors、46 个中文 UTF-8 显示宽度超过 100 列的 warnings，
  无其他告警。仓库仍为 `NO_CONFIG`，未执行目标对象编译。
- 结论：已按方法论第 17 章完成强制单文件验收，状态 `[x]`。

### kernel/dma/Makefile

- 版本与范围：基于 `b0df6b2ce1d6`；完整复读修改后 46 行文件。
- 函数与实体清单：Makefile 不含 C 函数、结构体或运行时变量，函数级门禁不适用；文件级说明覆盖
  `obj-$(CONFIG_FOO)` 展开、built-in.a 链接语义，以及 10 条对象选择规则的配置来源、对象职责和
  缺省不编译结果。每条原始 Kbuild 规则均有紧邻中文说明。
- 英文注释清单：除 SPDX 许可证外无原有英文注释；许可证按规则豁免且原样保留。
- 路径清单：HAS_DMA 基础对象、传统 ops、dummy、CMA、设备 coherent 内存、API debug、SWIOTLB、
  coherent 原子池、MMU remap 和 benchmark 十个配置分支全部覆盖；这些 bool 关闭时对应对象不链接。
- 并发与生命周期：本文件只决定构建期对象集合，不执行运行时状态、ownership 或并发协议；注释已把
  各对象在运行时承担的映射、回收和查错角色与其构建条件对应起来。
- 注释抽查：抽查 HAS_DMA 的 mapping/direct 配对、ARCH_HAS_DMA_OPS 的 dummy 安全后端、MMU 的
  remap 条件，均可从注释推导“何时编译、提供什么、关闭后缺少什么”。不存在复杂函数，按文件类型豁免。
- 关联读取：`kernel/dma/Kconfig:1-180,245-279`（各本地配置定义，本任务内待处理）；
  `arch/Kconfig:20-27`（ARCH_HAS_DMA_OPS 选择约束，学习注释缺失，已登记总 TODO）。
- 修改安全：`git diff --numstat` 为 34 新增、0 删除；零规则改动、零原注释删除或改写；
  `git diff --check` 通过；checkpatch 为 0 errors、0 warnings。仓库为 `NO_CONFIG`，未运行 Kbuild
  对象构建，配置到对象的对应关系通过当前 Kconfig 和 Makefile 静态核对。
- 结论：已按方法论第 17 章完成适用于 Kbuild 文件的强制验收，状态 `[x]`。

### kernel/dma/ops_helpers.c

- 版本与范围：基于 `b0df6b2ce1d6`；完整复读修改后 200 行文件。
- 函数清单：`dma_common_vaddr_to_page()`（头 14～22，声明 23，体止 29）、
  `dma_common_get_sgtable()`（英文 31～33、中文头 34～48，声明 49，体止 62）、
  `dma_common_mmap()`（英文 64～66、中文头 67～82，声明 83，体止 116）、
  `dma_common_alloc_pages()`（头 118～136，声明 137，体止 169）、
  `dma_common_free_pages()`（头 171～184，声明 185，体止 200）；五项均逐一核对全部参数、
  输出参数、上下文、配置差异、成功/失败返回、后续释放和 ownership。
- 实体清单：无全局/静态数据或结构体定义；所有局部变量按函数说明，包括借用首页 `page`、
  SG 分配结果 `ret`、VMA 页数/偏移、设备操作表、物理地址和设备池返回值。
- 英文注释清单：文件说明 2～5、SG 说明 31～33、mmap 说明 64～66 原样保留并紧邻完整翻译；
  `#endif /* CONFIG_MMU */` 的配置含义紧邻补充；SPDX 豁免。
- 路径清单：直接/vmalloc 首页转换、SG 节点成功/失败、设备 coherent 池优先、VMA 越界、MMU/nMMU、
  CMA/buddy 两级页分配、dma-iommu/传统 ops 两种映射、映射失败回滚及配对释放全部覆盖。
- 并发与生命周期：SG/mmap 和可阻塞分配路径允许睡眠；页面、设备和操作表均按借用/持有边界说明；
  用户 PTE 是 mmap 发布点；非一致性页映射以 SKIP_CPU_SYNC 保留显式 cache ownership 协议；释放必须
  先摘除设备地址再归还物理页。无本地锁/RCU/屏障，调用方负责停止 DMA 和串行化生命周期。
- 注释抽查：抽查 SG 导出的一项物理连续假设、mmap 的 `off >= count` 先行防下溢、alloc/free 的
  DMA 地址与物理页逆序回收，均通过初学者复述和开发者顺序推理。
- 关联读取：`include/linux/dma-map-ops.h:196-205`（接口声明，缺失，已在总 TODO）、
  `kernel/dma/mapping.c:700-758`、`kernel/dma/contiguous.c:405-472`（通用入口及 CMA/buddy 释放，本任务待处理），
  `Documentation/core-api/dma-api.rst:550-625`、`dma-attributes.rst:46-68`（公开 ownership/sync 契约）。
  另核对操作表采用点：`arch/alpha/kernel/pci_iommu.c:919-922`、`arch/x86/kernel/amd_gart_64.c:680-681`、
  `arch/mips/jazz/jazzdma.c:624-627`、`arch/powerpc/kernel/dma-iommu.c:225-228`、
  `arch/powerpc/platforms/pseries/vio.c:616-619`、`arch/powerpc/platforms/ps3/system-bus.c:700-717`、
  `drivers/parisc/ccio-dma.c:1026-1028`、`drivers/parisc/sba_iommu.c:1090-1092`、
  `drivers/xen/swiotlb-xen.c:447-450`、`drivers/xen/grant-dma-ops.c:291-292`。这些区域只显示回调绑定，
  学习注释缺失/部分覆盖；不建议因公共 helper 任务扩展整文件，留待相应架构/IOMMU 专题处理。
- 修改安全：`git diff --numstat` 为 97 新增、0 删除；零代码改动、零原注释删除或改写；
  `git diff --check` 通过；checkpatch 为 0 errors、55 个中文 UTF-8 显示宽度 warnings，无其他告警；
  `NO_CONFIG`，未执行对象编译。
- 结论：已按方法论第 17 章完成强制单文件验收，状态 `[x]`。

### kernel/dma/direct.h

- 版本与范围：基于 `b0df6b2ce1d6`；完整复读修改后 371 行文件。
- 函数/声明清单：7 个 `direct.c` 外部声明（声明行 30、41、55、67、80、90、99）、三组配置相关
  SG 声明或空 stub（声明行 113/126、146/173、159/188），以及
  `dma_direct_sync_single_for_device()`（头 194～205，声明 206，体止 220）、
  `dma_direct_sync_single_for_cpu()`（头 222～234，声明 235，体止 252）、
  `dma_direct_map_phys()`（头 254～272，声明 273，体止 332）、
  `dma_direct_unmap_phys()`（头 334～348，声明 349，体止 369）。每项均逐参数说明输入输出、单位、
  ownership、调用者、返回类别、上下文、配置差异和后续去向；空 stub 明确无副作用。
- 实体清单：无结构体、全局或静态数据；`paddr`、`dma_addr`、`phys` 三个局部变量的地址空间、
  生命周期和诊断用途均已说明；include guard 的编译期作用已覆盖。
- 英文注释清单：文件说明 2～6 原样保留并紧邻翻译；357 的“无需处理”原文紧邻补充 cache/SWIOTLB
  原因；370 的 include-guard 尾注紧邻解释；许可证和版权豁免。
- 路径清单：有/无体系结构 sync、SWIOTLB 有/无、CPU/device ownership 双向同步、强制 bounce、
  CC_SHARED、MMIO、普通 RAM、mask/bus-limit 溢出、kmalloc 对齐 bounce、SKIP_CPU_SYNC、
  REQUIRE_COHERENT 快速解除和 SG 批量 flush 全部覆盖。
- 并发与生命周期：直映射地址不占独立对象；SWIOTLB 槽由 map 成功持有并在 unmap 释放；调用方须
  停止 DMA 后解除，不能并发 sync/unmap；非一致性 cache hook 和 bounce copy 的先后顺序已明确；
  无本地锁/RCU/引用，`flush=false` 仅用于把体系结构批处理提交延后到 SG 外层末尾。
- 注释抽查：只读 `map_phys` 注释可复述 direct→SWIOTLB 退化与三个地址类型；只读 `unmap_phys`
  可推导“先同步、后释放槽”和防重复同步；只读 CPU/device 两个 sync 可解释 bounce copy 与 cache
  操作为何顺序相反，均通过初学者与开发者视角测试。
- 关联读取：`kernel/dma/direct.c:330-509,511-638`（SG、mmap、能力查询和本头内联调用者，
  本任务内待处理）；`kernel/dma/mapping.c:153-228,381-456`（通用 map/unmap/sync 分派，本任务待处理）。
  未读取新的跨子系统源码，因此本文件没有新增总 TODO 项。
- 修改安全：`git diff --numstat` 为 216 新增、0 删除；零代码改动、零原注释删除或改写；
  `git diff --check` 通过；checkpatch 为 0 errors、61 个中文 UTF-8 显示宽度 warnings，且无其他
  warning 类别；仓库 `NO_CONFIG`，未执行包含该头的目标对象编译。
- 结论：已按方法论第 17 章完成强制单文件验收，状态 `[x]`。

### kernel/dma/debug.h

- 版本与范围：基于 `b0df6b2ce1d6`；完整复读修改后 240 行文件。
- 函数/声明清单：12 个逻辑 hook 的启用版 extern 声明位于 25、34、43、52、60、69、77、86、
  95、104、113、122 行；关闭版同名 inline stub 位于 133、142、151、160、169、178、187、196、
  205、214、223、232 行。逐项覆盖 phys/SG/coherent/pages 的 map/alloc 与 unmap/free，以及
  single/SG 的 CPU/device sync；所有参数、单位、输入输出、返回、副作用和配置差异均已核对。
- 实体清单：本头不定义结构体、全局/静态数据或局部变量；include guard、DMA_API_DEBUG 条件分支、
  extern 与 inline ABI 关系均有说明。真实调试 entry 的 ownership 明确属于 debug.c，不属于缓冲区。
- 英文注释清单：只有 SPDX、版权/作者及两个 `#endif` 条件尾注；前两类按规则豁免，条件尾注均有
  紧邻中文解释，原文无删除或改写。
- 路径清单：debug 开启时的成功登记、调试项耗尽允许漏记、非法地址忽略、配对删除、sync 校验；
  debug 关闭时 12 个零返回/零副作用 stub 全部覆盖。调试失败不会回滚或改变真实 DMA 操作结果。
- 并发与生命周期：启用版使用预分配池、GFP_ATOMIC 补充和自旋锁，不主动睡眠；锁只保护调试镜像；
  真实设备、SG、页面、CPU 地址均是借用输入，调用方仍负责其原生命周期。关闭版不取得任何引用。
- 注释抽查：抽查 map_phys→unmap_phys、map_sg→sync/unmap_sg、alloc_pages→free_pages 三组，能够
  复述“真实操作—调试镜像建立—参数校验—镜像删除”链路，并区分调试项不足与真实 DMA 失败。
- 关联读取：`kernel/dma/debug.c:625-718,1261-1651`（GFP_ATOMIC entry 分配、12 个启用版实现及
  check_sync/check_unmap 调用关系，本任务内待处理）。未读取新的跨子系统源码，无新增总 TODO。
- 修改安全：`git diff --numstat` 为 112 新增、0 删除；零代码改动、零原注释删除或改写；
  `git diff --check` 通过；checkpatch 为 0 errors、36 个中文 UTF-8 显示宽度 warnings，且无其他
  warning 类别；仓库 `NO_CONFIG`，未执行包含该头的对象编译。
- 结论：已按方法论第 17 章完成强制单文件验收，状态 `[x]`。

### kernel/dma/Kconfig

- 版本与范围：基于 `b0df6b2ce1d6`；完整复读修改后 380 行文件。
- 函数与实体清单：Kconfig 不含 C 函数，函数级门禁不适用；43 个 `config` 符号、1 个 `choice`
  四分支、`if DMA_CMA` 条件组和默认大小菜单全部逐项说明。覆盖隐藏能力位、用户可见 bool/int、
  depends/select/default/range 的构建后果、运行时角色和关闭配置退化行为。
- 英文注释/help 清单：IOMMU bypass、平台 direct、write-combine、默认 coherent、noncoherent mmap、
  arch alloc 六组原注释原样保留并紧邻翻译；SWIOTLB_DYNAMIC、DMA_RESTRICTED_POOL、DMA_CMA、
  DMA_NUMA_CMA、CMA_SIZE_PERNUMA、两个 CMA size、CMA_ALIGNMENT、DMA_API_DEBUG、
  DMA_MAP_BENCHMARK 共 10 个 help 块完整覆盖其条件、命令行、代价、默认建议和引用路径。
- 路径清单：HAS_DMA/NO_DMA、传统 ops/direct/dma-iommu、体系结构 sync 组合、SWIOTLB 动态与受限池、
  noncoherent mmap、三种 coherent 分配方案互斥、CMA NUMA/大小 choice/alignment、debug 与 benchmark
  依赖全部覆盖；choice 与 if/endif 边界均有中文说明。
- 并发与生命周期：本文件只建立配置期能力图，不执行运行时锁或 ownership；注释把每个能力位与
  后续编译对象、cache ownership、bounce 槽、预留内存及调试账本生命周期对应起来。
- 注释抽查：抽查 DMA_NEED_SYNC 派生式可恢复为何保留 sync 状态；抽查 DMA_DIRECT_REMAP 与
  GLOBAL_POOL/ARCH_ALLOC 互斥可恢复 coherent CPU 映射策略；抽查 CMA choice 可推导固定值、百分比、
  min/max 和命令行覆盖顺序，均通过配置视角复述。
- 关联读取：`scripts/Kconfig.include:1-65`（定位完整内核配置验证的编译器版本检查；仅构建基础设施，
  与 DMA 学习范围无直接语义关系，不建议加入 DMA 关联补注 TODO）。
- 修改安全：`git diff --numstat` 为 101 新增、0 删除；零配置语句改动、零原注释/help 删除或改写；
  `git diff --check` 通过；checkpatch 为 0 errors、0 warnings。系统 `make` 3.81 不满足要求，改用
  `/opt/homebrew/bin/gmake` 成功构建当前树的 Kconfig `conf`；完整 `allnoconfig` 又因 Clang 17.0.0
  低于最低 17.0.1 停止。随后用同一 `conf --allnoconfig` 通过绝对路径 wrapper 独立解析本文件，
  43 个符号和全部条件块语法验证成功；所有 `/tmp` 验证产物已删除，源码树仍为 `NO_CONFIG`。
- 结论：已按方法论第 17 章完成适用于 Kconfig 文件的强制验收，状态 `[x]`。
