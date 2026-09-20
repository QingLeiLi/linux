# mm 学习注释第 17 章验收记录

本文件保存 `doc/linux-kernel-source-learning-methodology.md` 第 17 章要求的逐文件内容验收证据。
行号对应该文件完成本轮编辑后的工作区版本；后续改动使行号漂移时，必须重建对应文件记录。

自 2026-09-20 的标准升级起，新闭环记录必须单列并逐项保存“函数使用场景清单、结构体字段清单、
枚举清单”；缺少任一适用项不得标记完成。此前记录中的实体总述仍是旧标准证据，不自动证明已经
满足三类新门禁；需要按新标准复审时必须补建清单，不能从原完成标记反推通过。

## mm/debug_vm_pgtable.c

### 函数、实体与结构验收

已按物理顺序核对 47 个条件编译定义：PTE/PMD/PUD 的 basic、advanced、leaf、clear、populate、
PROT_NONE、soft-dirty、swap/migration、THP/HugeTLB/huge-vmap 测试，以及 `get_random_vaddr()`、
大页分配/释放、`destroy_args()`、`phys_align_check()`、`init_fixed_pfns()`、`init_args()` 和总入口。
每个真实定义的专属中文函数头均位于声明正上方，覆盖业务位置、参数/借用关系、返回/副作用和
锁/配置/失败约束；条件编译空桩逐项说明不适用原因与无副作用契约。`pgtable_debug_args` 的 owner、
当前槽位、释放基址、真实页 PFN、纯编码 PFN、保护值及 softleaf 字段组均已说明。

### 复杂函数只读复述抽查

- `pte_advanced_tests()`：调用者先映射并锁住 PTE；函数依次验证写保护并取走、access-flags 把只读
  clean 升为可写 dirty、young test-and-clear，三轮之间都把槽位恢复为 none，并用 cache flush 清理
  arm64 测试页的架构私有标志。
- `init_args()`：先把所有 owner 字段置成可回滚哨兵，再创建 mm/VMA 和逐级页表；固定 PFN 只用于
  编码，真实测试页按 PUD→PMD→order-0 降级分配。核心对象失败统一交给 `destroy_args()` 逆序释放，
  大页缺失只让相应测试跳过，不阻止纯值测试。
- `debug_vm_pgtable()`：先遍历权限组合执行无锁纯值断言，再覆盖叶、PROT_NONE、soft-dirty、swap、
  migration 和大页分类；修改真实槽位时依次持 PTE、PMD、PUD、顶层页表锁且不嵌套，最后统一销毁。

### 英文、路径、并发与关联证据

- 原英文说明均保留，并在其后邻接补充完整中文含义与测试原因；许可证、版权和作者元数据按豁免处理。
- 成功、能力缺失跳过、核心分配失败回滚、大页分配降级、折叠页表和各配置空桩均已覆盖。
- `Documentation/mm/arch_pgtable_helpers.rst`：核对各层 pure-value 与 modifying helper 的通用语义；
  它是英文接口表，不属于源码学习注释目标，建议随 helper 变化同步复审。

### 机械与追加式安全门禁

- 密度：`code=937 comments=666 chinese=338 density=0.361 max_gap=10`，通过。
- `git diff --check -- mm/debug_vm_pgtable.c`：通过。
- 相对基线：新增 430 行、删除 0 行；可执行代码和原有英文注释未改。
- checkpatch（忽略 UTF-8 `LONG_LINE_COMMENT`）：0 error、0 warning。
- 工作区无 `.config`，未执行目标对象编译，不声称编译通过。

## mm/migrate_device.c

### 函数与实体清单

下表声明行对应最终复读版本；所有真实定义均有紧邻声明的中文契约，条件编译空桩分别说明配置边界：

|函数|声明行|17.2 结论|
|---|---:|---|
|`migrate_vma_collect_skip`|35|通过|
|`migrate_vma_collect_hole`|59|通过|
|`migrate_vma_split_folio`|120|通过|
|`migrate_vma_collect_huge_pmd`|187|通过|
|`migrate_vma_collect_pmd`|318|通过|
|`migrate_vma_collect`|638|通过|
|`migrate_vma_check_page`|679|通过|
|`migrate_device_unmap`|724|通过|
|`migrate_vma_unmap`|843|通过|
|`migrate_vma_setup`|925|通过|
|`migrate_vma_insert_huge_pmd_page`|998|通过|
|`migrate_vma_split_unmapped_folio`|1138|通过|
|`migrate_vma_nr_pages`|1194|通过|
|`migrate_vma_insert_page`|1227|通过|
|`__migrate_device_pages`|1387|通过|
|`migrate_device_pages`|1564|通过|
|`migrate_vma_pages`|1585|通过|
|`__migrate_device_finalize`|1599|通过|
|`migrate_device_finalize`|1673|通过|
|`migrate_vma_finalize`|1697|通过|
|`migrate_device_pfn_lock`|1712|通过|
|`migrate_device_range`|1760|通过|
|`migrate_device_pfns`|1808|通过|
|`migrate_device_coherent_folio`|1853|通过|

静态 `migrate_vma_walk_ops` 已在声明前说明生命周期、字段分派和 mmap 读锁契约；MIGRATE 数组槽、
PFN/flags、页数、fault owner、notifier range、页表锁和 cleanup 局部变量均在有效阶段内覆盖。

### 复杂函数只读复述抽查

- `migrate_vma_collect_pmd()`：先把 THP/device-private PMD 交给整 folio 路径，普通页再在 PTE 锁下
  过滤页类型与 pgmap owner。它为候选取得 folio 引用，只 trylock 规避迁移互锁；单映射快路直接把
  PTE 替换成保留 write/young/dirty/soft-dirty/UFFD-WP 的 migration entry，并把 rmap 引用转换成事务引用。
- `migrate_device_unmap()`：普通 folio 先隔离 LRU并放掉 collect 引用，剩余映射走 rmap unmap；仍被
  映射或 refcount 超出已知来源的页判为 pinned。失败页在第二遍恢复 migration PTE、LRU、锁和引用，
  成功页保持锁定，直到 pages/finalize 阶段完成。
- `__migrate_device_pages()`：无目标即清 MIGRATE；空洞槽在 notifier 窗口内发布驱动目标；THP 粒度不
  匹配时只允许有 VMA 的源大页拆成 base 页，反向合并不支持。实体页先筛目标 ZONE_DEVICE 类型，再由
  `folio_migrate_mapping()` 转移 mapping/flags；逐槽失败只清对应 MIGRATE。
- `__migrate_device_finalize()`：失败或无目标时选择 src，成功时选择 dst；普通最终 folio 先回 LRU，
  `remove_migration_ptes()` 是 CPU 映射恢复/替换提交点，之后消费源和被采用/丢弃目标的临时锁与引用，
  但保留 fault owner 的源锁。

### 英文、并发、生命周期与关联证据

- 所有非豁免英文注释已由新邻接门禁逐单元核对，`untranslated=[]`；原文零删除、零改写。
- 已覆盖 mmap 读锁、PTE/PMD 锁、folio trylock 防互锁、MMU notifier、uptodate 发布屏障、rmap/LRU、
  memcg、compound 拆分、userfaultfd 退让、pin 拒绝和 setup→pages→finalize 强制闭环。
- `include/linux/migrate.h`：核对 MIGRATE_PFN 编码、选择 flags、`struct migrate_vma` 数组和 owner 契约；
  现有学习注释缺失，建议后续补 flags、数组阶段约束和公开 API。
- `mm/migrate.c`：核对 `folio_migrate_mapping()` 的 expected refcount 与 `remove_migration_ptes()` 的 rmap
  恢复语义；现有学习注释缺失，建议随基线顺序处理相关函数。
- `mm/huge_memory.c::set_pmd_migration_entry()`：核对 PMD invalidation、anon-exclusive、软件位保存及
  rmap/ref 转换；现有学习注释缺失，建议后续覆盖 THP migration 成对路径。

### 机械与追加式安全门禁

- 新门禁：`code=978 comments=723 chinese=323 density=0.330 max_gap=10 untranslated=[]`，通过。
- 相对基线新增 405 行、删除 0 行；目标文件可执行代码和原注释未改。
- `git diff --check` 通过；checkpatch 忽略 UTF-8 `LONG_LINE_COMMENT` 后 0 error、0 warning。
- 工作区无 `.config`，未执行目标对象编译，不声称编译通过。

## mm/zswap.c

### 函数、结构与状态验收

按物理顺序复读 56 个条件编译定义（54 个唯一函数名）：开关/容量查询、swap tree 分片、压缩池创建与
RCU/percpu_ref 退役、运行时换算法、memcg/NUMA list_lru、条目分配释放、每 CPU crypto 上下文、
压缩/解压、swap-cache 回写、动态 shrinker、store/load/invalidate、swapon/swapoff、debugfs 双实现和
setup/init。所有真实定义均在声明正上方具有中文契约，覆盖参数借用、返回值、资源 owner、可睡眠性、
锁/RCU 条件和失败后的 folio/entry 状态；`crypto_acomp_ctx`、`zswap_pool`、`zswap_entry`、全局池链表、
收缩游标、容量滞回与初始化状态均已说明。

### 复杂函数只读复述抽查

- `zswap_compressor_param_set()`：初始化前只保存算法名；运行时先在池锁下尝试取得同算法退役池，锁外
  新建或 resurrect，再在池锁下把采用池发布到 RCU 链表头。成功 kill 旧 current 的身份引用，失败则
  把候选放到尾部后 kill；既有 entry 的 pool 引用使旧池延寿到最后条目释放。
- `zswap_compress()` / `zswap_decompress()`：同一 CPU 的 mutex 串行复用 request/buffer，异步 crypto
  提交后同步等待。压缩无收益时只有允许 writeback 才按 PAGE_SIZE 原样保存；zsmalloc 使用 NOWAIT。
  解压以 begin/end 稳定最多两段 SG，原样页走 memcpy，算法路径还校验输出恰为一页。
- `zswap_writeback_entry()` / `shrink_memcg_cb()`：LRU 回调不能先摘除无独立引用的 entry，故先旋到
  尾部、复制 swpentry 后放锁。回写建立并锁住同 slot 的 swap-cache folio，再以 xarray 指针相等验证
  entry 尚存，成功才解压、删树、释放压缩副本并续接 swap writepage；已缓存页作为进入热区的停止信号。
- `zswap_shrinker_count()` / `shrink_worker()`：count 先消费近期磁盘 swapin 作为过度回收反馈，再以
  backing/stored 压缩比缩放候选。池满 worker 持锁推进带 iterator 引用的 memcg 游标，对在线组另取扫描
  引用，并与 offline cleanup 协调，轮转回写到 accept threshold 或达到连续失败上限。
- `zswap_store()` / `zswap_load()`：store 在 folio 锁保护下逐 base page 发布；entry 先入 xarray 后补
  pool/objcg 引用和计费，最后入 LRU 才对 writeback 可见。任一子页失败会清除整个 folio 范围的新旧
  条目，防止旧数据回写覆盖新 swapfile 内容。load 命中后解压并把 swapcache 设为权威脏副本，消费
  zswap entry；`-ENOENT` 唯一保留 folio 锁并交回真实 swap 读路径。

### 英文、调用链与关联证据

- 所有非豁免英文注释均由邻接门禁逐单元核对，`untranslated=[]`；原英文与可执行代码零删除、零改写。
- `mm/page_io.c`：核对 write path 在 `zswap_store()` 成功后终止设备写出，read path 只有 `-ENOENT`
  才访问慢设备并记录 `zswap_folio_swapin()`；该文件已有中文学习注释。
- `mm/swapfile.c`：核对 slot free 先逐项 `zswap_invalidate()`，swapon 在设备发布前建立 tree，失败和
  swapoff 路径销毁 tree；已有中文但本轮未重新验收。
- `mm/memcontrol.c`：核对 memcg offline 在 list_lru/objcg 重挂前推进 zswap 游标，以及祖先级
  `memory.zswap.writeback` 策略；已有中文但本轮未重新验收。
- `include/linux/zswap.h`：核对公开返回/空桩契约和 disk swapin 反馈字段；当前无中文学习注释，建议
  后续按基线顺序独立处理，不能由本文件密度替代。

### 机械与追加式安全门禁

- 新门禁：`code=1096 comments=989 chinese=383 density=0.349 max_gap=10 untranslated=[]`，通过。
- 相对基线新增 531 行、删除 0 行；目标文件可执行代码和原注释未改。
- `git diff --check` 通过；仅检查补丁新增行的 checkpatch 在忽略 UTF-8 `LONG_LINE_COMMENT` 后为
  0 error、0 warning。
- 验收脚本 `py_compile` 与当时 4 个漏译回归用例通过；随后新增命名参数标签用例后总计 5 个；
  工作区无 `.config`，未执行目标对象编译，
  不声称编译通过。

## mm/mremap.c

### 函数、实体与状态验收

已按物理顺序复读全部条件编译实现和空桩：页表层级取得/分配、rmap 锁、soft-dirty、PTE/PMD/PUD
搬迁、范围对齐与进度游标、VMA 计账/统计、源端删除与失败回滚、原地缩放、显式目标搬迁、hugetlb、
userfaultfd、多 VMA 纯搬迁、参数准备和 syscall 主状态机。每个函数定义均有紧邻声明的中文契约，覆盖
输入范围、返回值、锁与 owner、可睡眠点、失败后的 VMA/页表/计账状态；`pagetable_move_control` 和
`vma_remap_struct` 的阶段性字段在首次参与控制流处均有说明。

### 复杂函数只读复述抽查

- `move_ptes()` / `move_page_tables()`：顶层先尝试完整 PUD、PMD 或 huge leaf 转接，不能整层搬时下钻
  到 PTE；PTE 批处理在两端页表锁和必要的 rmap 锁下保留软脏、UFFD-WP 等软件状态。realign 只同步
  向下扩展两个起点，`pmc_progress()` 再把内部游标钳回用户原始范围；分配失败返回短进度。
- `copy_vma_and_data()`：目标 VMA 建立后搬页表，短搬迁或文件 `mremap` 回调失败即把已搬前缀反向
  搬回；回滚强制取得 rmap 锁，并改写事务让上层删除目标而保留源端。
- `unmap_source_vma()`：正常 move 已由调用者完成 commit 计账，删除源前临时清 `VM_ACCOUNT`，避免
  `munmap` 二次撤账；若源 VMA 被切成左右残段，再借助复位后的 iterator 恢复两段标志。DONTUNMAP
  失败回滚删的是目标，故不走该临时清位技巧。
- `remap_move()`：FIXED 等长路径逐个处理相交 VMA，以首段实际目标为基准保留源 VMA 间隙；起点空洞、
  UFFD 或不能证明服从 FIXED 的自定义地址选择器会在不安全的多段组合前拒绝。
- `do_mremap()`：先规范化长度和参数，再以 killable mmap 写锁运行早期 map-count 门禁；按纯搬迁或
  单 VMA resize 分派。shrink 可由内部释放锁，最终只按 `mmap_locked` 解锁，锁外再完成 mlock populate
  与成功/失败对应的 UFFD 通知。

### 英文、调用链与关联证据

- 所有非豁免英文注释均由邻接门禁逐单元核对，`untranslated=[]`；命名参数标签如
  `/* drop_lock= */` 只按精确结构豁免，普通行内英文仍由回归用例证明会失败。
- `mm/internal.h`：核对 `struct pagetable_move_control` 与 `PAGETABLE_MOVE` 初始化关系；它是下一验收
  文件，本记录不以当前调用点替代其独立内容审查。
- `mm/vma_exec.c::relocate_vma_down()`：核对 exec 路径复用 `move_page_tables()`，短搬迁由即将销毁的
  exec mm 生命周期收口；`mm/vma.c::copy_vma()` 核对 VMA merge 与 `need_rmap_locks` 输出。
- `mm/userfaultfd.c`：核对 mremap prep/complete/fail 的引用生命周期与必须在 mmap 锁外完成的通知。

### 机械与追加式安全门禁

- 新门禁：`code=1120 comments=1199 chinese=381 density=0.340 max_gap=10 untranslated=[]`，通过。
- 相对基线新增 517 行、删除 0 行；目标文件可执行代码和原注释未改。
- `git diff --check` 通过；仅检查补丁新增行的 checkpatch 在忽略 UTF-8 `LONG_LINE_COMMENT` 后为
  0 error、0 warning。
- 验收脚本 5 个漏译/豁免回归用例通过；工作区无 `.config`，未执行目标对象编译，不声称编译通过。

## mm/internal.h

### 内联函数、结构与配置空桩验收

已按物理顺序复读 100 个条件编译内联定义/空桩，并逐个确认声明正上方存在专属中文契约；覆盖页表
搬迁控制块、GFP/分配标志、anon_vma 锁与引用、PTE/swap 批处理、回收与 fault 包装、buddy/compound
页初始化、sparsemem、compaction/CMA、mlock/VMA 地址换算、启动调试、NUMA/node reclaim、memory
failure、vmalloc、GUP/soft-dirty、shrinker debugfs、mmap action 和 MMU notifier。配置关闭空桩均明确
返回哨兵、输出参数和无副作用边界，不以一段共享说明替代单函数契约。

`pagetable_move_control`、`alloc_context`、`compact_control`、`capture_control`、
`migration_target_control` 及 fallback/GUP/ALLOC 枚举与掩码均已说明字段阶段、owner、扫描方向和限制；
连续声明区也按调用域分组说明，未把头文件原型误写成实现细节。

### 复杂路径只读复述抽查

- `folio_pte_batch_flags()`：先把扫描上限裁到 folio 尾部，以架构 batch hint 推进期望 PFN；比较时按
  flags 清除可忽略位，真实 PTE 只读，write/young/dirty 只 OR 合并到调用者传入的首项副本。
- buddy/compound helpers：`buddy_order()` 依赖 zone 锁或等价排他保证，unsafe 变体只提供单次读取；
  `page_is_buddy()` 依次验证 guard/PageBuddy、order、zone 与零引用。compound head/tail 分别初始化
  order、mapcount/pincount/deferred-list 和 tail 的 head/mapping/node/zone 编码。
- `compact_control`：低端 `migrate_pfn` 与高端 `free_pfn` 相向扫描，隔离源/目标队列以 base page 计数；
  direct/proactive/alloc_contig、skip hint、竞争和 pageblock 收尾字段决定可退让程度与进展保证。
- `maybe_unlock_mmap_for_io()`：仅首次允许 retry 且非 NOWAIT 时先 pin 文件再释放 fault/mmap 锁，引用
  由调用者在 IO/retry 后归还；已有 `fpin` 时不重复取引用或解锁。
- `gup_must_unshare()`：只读 PIN 才需额外判断；匿名页要求 exclusive 并与 rmap 共享屏障配对，文件页
  仅长期 pin 的私有可写映射需提前拆 COW，fast 路缺 VMA 时保守要求回退慢路。
- mmap action/notifier wrappers：IO remap 先做架构 PFN 与解密 pgprot 规范化，再只准备 action；事务完成
  或错误 unmap 前释放暂持 i_mmap 写锁。young wrappers 把 CPU 页表结果与二级 MMU notifier 结果合并。

### 关联实现与脚本盲区证据

- `mm/memory.c`、`mm/mprotect.c`、`mm/madvise.c`、`mm/vmscan.c`、`mm/rmap.c`：核对
  `folio_pte_batch_flags()` 在 copy、保护、回收和 rmap 路径对 flags/首 PTE 副本的使用。
- `mm/gup.c` 与 `include/linux/rmap.h`：核对 PTE/PMD/PUD、fast/slow GUP 调用点及 anon-exclusive 屏障。
- `mm/page_alloc.c`、`mm/compaction.c`：核对 fallback 三态、双向扫描游标和 direct/CMA 初始化字段。
- `mm/util.c`、`mm/memory.c`：核对 mmap action prepare/finish 中 PFN remap 与 i_mmap 锁的 owner 转移。
- 原脚本只统计全文件中文密度与连续代码空窗，附近任意中文都能重置空窗，无法把某个英文注释块与
  它自己的中文解释建立对应关系。现加入词法 comment-unit 提取与无代码间隔的邻接门禁，并以 5 个
  回归用例覆盖漏译、正确邻接、代码隔断、元数据/预处理/literal 豁免和命名参数标签；它仍只证明
  邻接存在，翻译完整性和函数归属继续由本节语义复读验收。

### 机械与追加式安全门禁

- 新门禁：`code=1146 comments=948 chinese=320 density=0.279 max_gap=10 untranslated=[]`，通过。
- 相对基线新增 362 行、删除 0 行；目标文件可执行代码和原注释未改。
- `git diff --check` 通过；仅检查补丁新增行的 checkpatch 在忽略 UTF-8 `LONG_LINE_COMMENT` 后为
  0 error、0 warning。
- 验收脚本 `py_compile` 与 5 个回归用例通过；工作区无 `.config`，未执行目标对象编译，
  不声称编译通过。

## mm/cma.c

### 函数清单

`函数头行 | 声明及函数体行 | 结论`：

|函数|函数头|声明及函数体|17.2 结论|
|---|---:|---:|---|
|`cma_get_base`|49-55|56-60|通过|
|`cma_get_size`|62-68|69-72|通过|
|`cma_get_name`|74-80|81-84|通过|
|`cma_bitmap_aligned_mask`|87-93|94-100|通过|
|`cma_bitmap_aligned_offset`|106-112|113-119|通过|
|`cma_bitmap_pages_to_bits`|121-127|128-132|通过|
|`cma_clear_bitmap`|134-140|141-155|通过|
|`cma_validate_zones`|162-168|169-210|通过|
|`cma_activate_area`|212-219|220-296|通过|
|`cma_init_reserved_areas`|298-304|305-314|通过|
|`cma_reserve_pages_on_error`|317-323|324-327|通过|
|`cma_new_area`|329-335|336-368|通过|
|`cma_drop_area`|370-376|377-381|通过|
|`cma_init_reserved_mem`|395-402|403-447|通过|
|`revsizecmp`|469-475|476-480|通过|
|`basecmp`|482-488|489-493|通过|
|`list_insert_sorted`|498-504|505-524|通过|
|`cma_fixed_reserve`|526-532|533-557|通过|
|`cma_alloc_mem`|559-565|566-618|通过|
|`__cma_declare_contiguous_nid`|620-627|628-732|通过|
|`cma_declare_contiguous_multi`|743-749|750-967|通过|
|`cma_declare_contiguous_nid`|989-996|997-1021|通过|
|`cma_debug_show_areas`|1023-1029|1030-1061|通过|
|`cma_range_alloc`|1063-1071|1072-1169|通过|
|`__cma_alloc_frozen`|1171-1178|1179-1247|通过|
|`cma_alloc_frozen`|1249-1255|1256-1263|通过|
|`cma_alloc_frozen_compound`|1265-1271|1272-1278|通过|
|`cma_alloc`|1290-1296|1297-1309|通过|
|`find_cma_memrange`|1312-1318|1319-1357|通过|
|`__cma_release_frozen`|1359-1366|1367-1381|通过|
|`cma_release`|1393-1399|1400-1424|通过|
|`cma_release_frozen`|1427-1433|1434-1448|通过|
|`cma_for_each_area`|1450-1456|1457-1471|通过|
|`cma_intersects`|1473-1481|1482-1504|通过|
|`cma_reserve_early`|1528-1536|1537-1582|通过|

结构复核：34 个函数均有位于声明正上方的专属中文函数头，且可直接定位业务背景、入参、
返回/副作用和注意事项。英文 kernel-doc 保持在前，中文契约在后；未发现函数尾部、导出宏附近或
条件编译分支错放的函数头。

### 复杂函数只读复述抽查

- `cma_activate_area()`：先为所有 ranges 保存 `early_pfn` 并分配 bitmap，再验证 zone；把早期取走的
  前缀置位、把剩余 pageblocks 初始化成 CMA，最后初始化运行期锁并发布 `CMA_ACTIVATED`。任一步失败
  都只释放已经分配的 bitmap；未设置保留策略时，从各 range 的真实 early 边界开始把后缀交回 buddy，
  最后撤销 `totalcma_pages` 并把 area 容量清零。
- `cma_declare_contiguous_multi()`：先尝试单段，只有 `-ENOMEM` 才进入多段慢路；慢路筛选 4GB 以上、
  同时满足 pageblock/bitmap 对齐的候选，先按大小保留最多八段，再按地址升序逐段 reserve，最后发布
  `nranges/nid/res_cma`。物理 reserve 中途失败会释放此前完整保留的 ranges 并回滚尾部 area 槽位。
- `cma_range_alloc()`：锁内检查总可用量并认领满足对齐的空 bitmap 区，确认 memmap 连续后扣减
  `available_count`；锁外在 `alloc_mutex` 下迁移并冻结物理页。成功保留 bit 并输出 frozen 首页；失败先
  清 bit/恢复计数，只有 `-EBUSY` 会移动到下一个候选，其它错误直接返回。
- `cma_reserve_early()`：只在未激活、单线程 init 阶段工作；校验两种粒度和总余量后，从能单独容纳
  请求的首个 range 底部推进 `early_pfn`，不跨 range 拼接且不可撤销。调用者接管页结构和失败后的
  生命周期责任。

### 关联文件与语义证据

- `include/linux/cma.h`：核对公开声明及单段包装；学习注释现状为不足，未纳入本文件验收。
- `mm/cma.h`：核对 `struct cma`、`struct cma_memrange`、flags、bitmap 粒度和 sysfs 计账；部分覆盖，
  尚未独立按第 17 章验收。
- `mm/page_alloc.c`：核对 `alloc_contig_frozen_range()` / `free_contig_frozen_range()` 的 frozen ownership、
  单 zone 与 GFP 约束；部分覆盖，尚未独立验收。
- `mm/hugetlb_cma.c`：核对 early reserve 与 frozen compound 的调用配对；已有中文但仅部分覆盖。
- `arch/s390/mm/init.c`：核对 memory-offline 传给 `cma_intersects()` 的排他 `end`；学习注释不足。

### 英文、状态与边界审计

- 已逐段核对原有英文说明，紧邻中文覆盖其条件、因果、失败、锁和 ownership；原英文均保留。
- 已覆盖 `cma_areas/cma_area_count`、`memranges`、候选结构字段、range/area 状态和 bitmap 粒度。
- 已把四个未决行为边界记录到 `doc/questions/memory-management-questions.md`；它们不是本轮行为修改。

### 机械与追加式安全门禁

- 密度：`code=729 comments=676 chinese=358 density=0.491 max_gap=10`，通过。
- `git diff --check -- mm/cma.c doc/questions/memory-management-questions.md`：通过。
- `mm/cma.c` 相对基线：新增 433 行、删除 0 行；未改代码或原有英文注释。
- checkpatch：仅 `LONG_LINE_COMMENT`（245 条），无其它 warning/check/error。
- 构建：工作区无 `.config`，未声称目标对象编译通过。

## mm/hugetlb_cgroup.c

### 函数清单

下表格式为 `函数 | 函数头 | 声明及函数体`，全部通过第 17.2 节：

|函数|函数头|声明及函数体|
|---|---:|---:|
|`__hugetlb_cgroup_counter_from_cgroup`|55-65|66-72|
|`hugetlb_cgroup_counter_from_cgroup`|80-86|87-90|
|`hugetlb_cgroup_counter_from_cgroup_rsvd`|97-104|105-108|
|`hugetlb_cgroup_from_css`|115-122|123-126|
|`hugetlb_cgroup_from_task`|133-141|142-145|
|`hugetlb_cgroup_is_root`|148-153|154-157|
|`parent_hugetlb_cgroup`|160-166|167-170|
|`hugetlb_cgroup_have_usage`|176-184|185-195|
|`hugetlb_cgroup_init`|203-212|213-256|
|`hugetlb_cgroup_free`|263-270|271-279|
|`hugetlb_cgroup_css_alloc`|287-296|297-346|
|`hugetlb_cgroup_css_free`|349-355|356-359|
|`hugetlb_cgroup_move_parent`|372-381|382-426|
|`hugetlb_cgroup_css_offline`|437-445|446-464|
|`hugetlb_event`|472-481|482-494|
|`__hugetlb_cgroup_charge_cgroup`|503-514|515-562|
|`hugetlb_cgroup_charge_cgroup`|565-571|572-576|
|`hugetlb_cgroup_charge_cgroup_rsvd`|579-586|587-591|
|`__hugetlb_cgroup_commit_charge`|598-608|609-634|
|`hugetlb_cgroup_commit_charge`|637-643|644-649|
|`hugetlb_cgroup_commit_charge_rsvd`|652-658|659-664|
|`__hugetlb_cgroup_uncharge_folio`|673-682|683-721|
|`hugetlb_cgroup_uncharge_folio`|724-729|730-734|
|`hugetlb_cgroup_uncharge_folio_rsvd`|737-742|743-747|
|`__hugetlb_cgroup_uncharge_cgroup`|753-762|763-777|
|`hugetlb_cgroup_uncharge_cgroup`|780-785|786-790|
|`hugetlb_cgroup_uncharge_cgroup_rsvd`|793-798|799-803|
|`hugetlb_cgroup_uncharge_counter`|809-818|819-829|
|`hugetlb_cgroup_uncharge_file_region`|835-845|846-871|
|`hugetlb_cgroup_read_numa_stat`|897-906|907-971|
|`hugetlb_cgroup_read_u64`|977-985|986-1027|
|`hugetlb_cgroup_read_u64_max`|1033-1041|1042-1092|
|`hugetlb_cgroup_write`|1102-1112|1113-1162|
|`hugetlb_cgroup_write_legacy`|1165-1170|1171-1175|
|`hugetlb_cgroup_write_dfl`|1178-1183|1184-1188|
|`hugetlb_cgroup_reset`|1194-1203|1204-1242|
|`mem_fmt`|1248-1256|1257-1266|
|`__hugetlb_events_show`|1272-1279|1280-1300|
|`hugetlb_events_show`|1303-1308|1309-1312|
|`hugetlb_events_local_show`|1315-1320|1321-1324|
|`hugetlb_cgroup_cfttypes_init`|1455-1465|1466-1499|
|`__hugetlb_cgroup_file_dfl_init`|1502-1507|1508-1515|
|`__hugetlb_cgroup_file_legacy_init`|1518-1523|1524-1531|
|`__hugetlb_cgroup_file_init`|1534-1539|1540-1544|
|`__hugetlb_cgroup_file_pre_init`|1550-1557|1558-1571|
|`__hugetlb_cgroup_file_post_init`|1574-1580|1581-1587|
|`hugetlb_cgroup_file_init`|1593-1601|1602-1611|
|`hugetlb_cgroup_migrate`|1621-1630|1631-1654|

抽查复述：charge 从 leaf 沿父链逐级试扣并在失败点逆序取消；commit 把 reservation 取消后计入
actual，但 `page_counter_cancel()` 只减本层，不能误解为递归减祖先；css offline 把 actual folio 迁到父组，
reservation 依靠持有的 css 引用延迟释放。文件接口按 hstate/actual-reserved 属性解码，写入与 reset 分别
维护 max/rsvd 限额和 failure 计数。

关联证据：`include/linux/hugetlb_cgroup.h`（结构/inline，部分覆盖）、`mm/hugetlb.c`（reserve、commit、
rollback、migration 调用链，已有中文但未独立验收）、`mm/page_counter.c` 与
`include/linux/page_counter.h`（charge/cancel 语义，前者较充分、后者部分覆盖）。

机械门禁：`code=721 comments=835 chinese=565 density=0.784 max_gap=10`；新增 754 行、删除 0；
`diff --check` 通过；checkpatch 仅 `LONG_LINE_COMMENT`；无 `.config`，未执行对象编译。

## mm/page_owner.c

### 函数清单

下表格式为 `函数 | 函数头 | 声明及函数体`，全部通过第 17.2 节：

|函数|函数头|声明及函数体|
|---|---:|---:|
|`set_current_in_page_owner`|100-107|108-118|
|`unset_current_in_page_owner`|121-126|127-130|
|`early_page_owner_param`|136-143|144-153|
|`need_page_owner`|157-162|163-166|
|`create_dummy_stack`|172-178|179-186|
|`register_dummy_stack`|189-193|194-197|
|`register_failure_stack`|200-204|205-208|
|`register_early_stack`|211-215|216-219|
|`init_page_owner`|226-234|235-256|
|`get_page_owner`|271-277|278-281|
|`save_stack`|288-296|297-316|
|`add_stack_record_to_list`|323-332|333-371|
|`inc_stack_record_count`|378-387|388-415|
|`dec_stack_record_count`|421-429|430-441|
|`__update_page_owner_handle`|448-458|459-493|
|`__update_page_owner_free_handle`|500-510|511-537|
|`__reset_page_owner`|544-552|553-597|
|`__set_page_owner`|604-613|614-627|
|`__folio_set_owner_migrate_reason`|633-642|643-656|
|`__split_page_owner`|662-671|672-686|
|`__folio_copy_owner`|693-704|705-766|
|`pagetypeinfo_showmixedcount_print`|773-782|783-886|
|`print_page_owner_memcg`|895-904|905-946|
|`print_page_owner`|953-963|964-1027|
|`__dump_page_owner`|1034-1042|1043-1104|
|`read_page_owner`|1111-1121|1122-1234|
|`lseek_page_owner`|1240-1247|1248-1265|
|`init_pages_in_zone`|1274-1282|1283-1370|
|`init_early_allocated_pages`|1373-1378|1379-1386|
|`stack_start`|1401-1409|1410-1434|
|`stack_next`|1440-1446|1447-1458|
|`stack_print`|1468-1476|1477-1514|
|`stack_stop`|1517-1522|1523-1525|
|`page_owner_stack_open`|1544-1553|1554-1569|
|`page_owner_threshold_get`|1584-1589|1590-1594|
|`page_owner_threshold_set`|1597-1603|1604-1608|
|`pageowner_init`|1619-1627|1628-1660|

抽查复述：stack 保存用 current 重入哨兵避免 stack depot 再分配递归；每个 page_ext owner 记录与 stack
depot 引用按 set/reset/copy/split 路径配对；debugfs page 顺序文件在 zone/PFN 上迭代并只输出已分配记录；
stack 聚合序列按 threshold 过滤。当前实现的 free-handle 参数 `pid/tgid` 未使用而写 current，copy 路径
在 `page_ext_put()` 后继续读 owner 字段，均已按真实代码和外层序列化前提说明。

关联证据：`include/linux/page_owner.h`（wrapper 边界，部分覆盖）、`include/linux/page_ext.h` 与
`mm/page_ext.c`（RCU get/put 生命周期，部分覆盖）、`include/linux/stackdepot.h` 与 `lib/stackdepot.c`
（stack handle/refcount，部分覆盖）、`mm/migrate.c`、`mm/hugetlb.c`、`mm/huge_memory.c`、
`mm/page_alloc.c`（主要调用点，覆盖程度不一，均未随本文件独立验收）。

机械门禁：`code=727 comments=766 chinese=509 density=0.700 max_gap=10`；新增 660 行、删除 0；
`diff --check` 通过；checkpatch 仅 `LONG_LINE_COMMENT`；无 `.config`，未执行对象编译。

## mm/util.c

### 函数清单

|函数|函数头|声明及函数体|17.2 结论|
|---|---:|---:|---|
|`kfree_const`|52-58|59-64|通过|
|`__kmemdup_nul`|76-82|83-98|通过|
|`kstrdup`|107-114|115-119|通过|
|`kstrdup_const`|133-139|140-147|通过|
|`kstrndup`|160-166|167-171|通过|
|`kmemdup_noprof`|184-190|191-201|通过|
|`kmemdup_array`|215-221|222-226|通过|
|`kvmemdup`|239-245|246-255|通过|
|`kmemdup_nul`|267-273|274-278|通过|
|`init_user_buckets`|284-290|291-297|通过|
|`memdup_user`|309-315|316-333|通过|
|`vmemdup_user`|345-351|352-368|通过|
|`strndup_user`|378-384|385-410|通过|
|`memdup_user_nul`|421-427|428-445|通过|
|`vma_is_stack_for_current`|449-455|456-462|通过|
|`vma_set_file`|467-473|474-481|通过|
|`randomize_stack_top`|489-495|496-514|通过|
|`randomize_page`|530-536|537-556|通过|
|`arch_randomize_brk`|561-567|568-576|通过|
|`arch_mmap_rnd`|578-584|585-598|通过|
|`mmap_is_legacy`|602-608|609-623|通过|
|`mmap_base`|633-639|640-673|通过|
|`arch_pick_mmap_layout` (default topdown)|679-685|686-703|通过|
|`arch_pick_mmap_layout` (fixed fallback)|705-711|712-717|通过|
|`__account_locked_vm`|738-744|745-776|通过|
|`account_locked_vm`|793-799|800-815|通过|
|`vm_mmap_pgoff`|820-826|827-856|通过|
|`vm_mmap`|875-881|882-893|通过|
|`vm_mmap_shadow_stack`|904-910|911-934|通过|
|`__vmalloc_array_noprof`|943-949|950-959|通过|
|`vmalloc_array_noprof`|967-973|974-978|通过|
|`__vcalloc_noprof`|987-993|994-998|通过|
|`vcalloc_noprof`|1006-1012|1013-1017|通过|
|`folio_anon_vma`|1022-1028|1029-1037|通过|
|`folio_mapping`|1051-1057|1058-1077|通过|
|`folio_copy`|1090-1096|1097-1110|通过|
|`folio_mc_copy`|1113-1119|1120-1137|通过|
|`overcommit_ratio_handler`|1159-1165|1166-1176|通过|
|`sync_overcommit_as`|1178-1184|1185-1189|通过|
|`overcommit_policy_handler`|1191-1197|1198-1237|通过|
|`overcommit_kbytes_handler`|1239-1245|1246-1257|通过|
|`init_vm_util_sysctls`|1306-1312|1313-1318|通过|
|`vm_commit_limit`|1327-1333|1334-1349|通过|
|`vm_memory_committed`|1371-1377|1378-1382|通过|
|`__vm_enough_memory`|1403-1409|1410-1462|通过|
|`get_cmdline`|1476-1482|1483-1541|通过|
|`memcmp_pages`|1545-1551|1552-1565|通过|
|`mem_dump_obj`|1579-1585|1586-1613|通过|
|`page_offline_freeze`|1640-1646|1647-1651|通过|
|`page_offline_thaw`|1653-1659|1660-1664|通过|
|`page_offline_begin`|1666-1672|1673-1677|通过|
|`page_offline_end`|1680-1686|1687-1691|通过|
|`flush_dcache_folio`|1695-1701|1702-1711|通过|
|`compat_set_desc_from_vma`|1733-1739|1740-1762|通过|
|`__compat_vma_mmap`|1787-1793|1794-1809|通过|
|`compat_vma_mmap`|1838-1844|1845-1863|通过|
|`set_ps_flags`|1868-1874|1875-1892|通过|
|`snapshot_page`|1909-1915|1916-1979|通过|
|`call_vma_mapped`|1981-1987|1988-2007|通过|
|`mmap_action_finish`|2009-2015|2016-2047|通过|
|`check_mmap_action`|2051-2057|2058-2067|通过|
|`mmap_action_prepare` (`CONFIG_MMU`)|2076-2082|2083-2112|通过|
|`mmap_action_complete` (`CONFIG_MMU`)|2126-2132|2133-2160|通过|
|`mmap_action_prepare` (`!CONFIG_MMU`)|2163-2169|2170-2187|通过|
|`mmap_action_complete` (`!CONFIG_MMU`)|2190-2196|2197-2220|通过|
|`folio_pte_batch`|2250-2256|2257-2263|通过|
|`page_range_contiguous`|2281-2287|2288-2309|通过|

结构复核：67 个条件编译函数定义均有紧邻声明的专属中文函数头和四类显式信息；英文 kernel-doc
保持在前。全局 sysctl、per-CPU 计数、用户复制 bucket、离线 rwsem、布局宏和 action 表也已覆盖。

### 复杂函数只读复述抽查

- `__vm_enough_memory()`：先把请求加入 `vm_committed_as`；ALWAYS 直接批准，GUESS 只拒绝单请求已超过
  RAM+swap 的明显不可能情况。NEVER 计算 commit limit，依权限扣 admin reserve、依 mm 规模扣有限 user
  reserve，再与正的 committed 近似值比较；所有拒绝分支统一打印并回滚最初记账。
- `get_cmdline()`：`get_task_mm` 固定对象，`arg_lock` 只把 argv/env 四边界取成同一快照；跨进程读取可能
  部分成功。若 argv 尾 NUL 被覆盖且缓冲仍有空间，先查已读内容，再按剩余容量拼接 env 并截到首个
  NUL；所有 mm 路径最终 `mmput`，返回长度不承诺尾 NUL。
- `snapshot_page()`：先复制原 page 并解析 compound_info；order-0 直接形成单页快照，tail 则还原 head
  与 index。索引在快照表示范围内时复制 folio head/第二页并取页数；split/merge 竞争导致 index 不匹配时
  最多重试五次，耗尽后清 compound head、降级为单页并清 FAITHFUL。
- mmap action：prepare 在 VMA 发布前按 type 验证/准备资源；complete 在已存在 VMA 上提交映射，再由
  finish 调用 mapped、释放可能持有的 rmap 锁。普通新映射失败只 unmap 本次未合并 VMA，compat 失败留给
  外层 post hook；NOMMU 保持接口但对非 NOTHING action 告警并在 complete 阶段失败。

### 关联文件与语义证据

- `include/linux/mm.h`：核对公开复制、布局、folio、snapshot 和 action helper 契约；已有中文但仅部分覆盖。
- `include/linux/mm_types.h`：核对 `mmap_action`、`vm_area_desc` 可变字段与 action ownership；学习注释不足。
- `mm/vma.c::__mmap_region()` / `call_mmap_prepare()`：确认 prepare 在 merge/新 VMA 前执行，complete 只对
  新建映射提交；部分覆盖，尚未独立验收。
- `mm/memory.c`：核对 PFN、I/O 和 kernel-pages 的 prepare/complete 配对；学习注释不足。
- `mm/migrate.c`：核对 `folio_mc_copy()` 的 -EHWPOISON 调用处理；部分覆盖。
- `fs/proc/kcore.c`：核对 page_offline freeze/thaw 包围随机 PFN 内容访问；学习注释不足。
- `mm/debug.c` / `fs/proc/page.c`：核对 snapshot faithful 标志的消费者；部分覆盖。

### 英文、状态与边界审计

- 原英文 kernel-doc、两阶段 action、PageOffline 协议、overcommit 顺序和 PTE batch 前提均有邻接中文覆盖。
- 六组工具的返回约定已区分 NULL、ERR_PTR、地址/负 errno、借用指针和部分写入。
- 三个尚未证明的边界已记录到 `doc/questions/memory-management-questions.md`，未混入行为修改。

### 机械与追加式安全门禁

- 密度：`code=909 comments=1194 chinese=547 density=0.602 max_gap=10`，通过。
- `git diff --check -- mm/util.c`：通过。
- `mm/util.c` 相对基线：新增 703 行、删除 0 行；未改代码或原有英文注释。
- checkpatch：仅 `LONG_LINE_COMMENT`（219 条），无其它 warning/check/error。
- 构建：工作区无 `.config`，未声称目标对象编译通过。

## mm/oom_kill.c

### 函数清单

|函数|函数头|声明及函数体|17.2 结论|
|---|---:|---:|---|
|`is_memcg_oom`|93-99|100-103|通过|
|`oom_cpuset_eligible` (`CONFIG_NUMA`)|118-124|125-161|通过|
|`oom_cpuset_eligible` (`!CONFIG_NUMA`)|163-169|170-173|通过|
|`find_lock_task_mm`|182-189|190-205|通过|
|`is_sysrq_oom`|211-217|218-221|通过|
|`oom_unkillable_task`|224-230|231-238|通过|
|`should_dump_unreclaim_slab`|246-252|253-269|通过|
|`oom_badness`|280-286|287-333|通过|
|`constrained_alloc`|346-352|353-417|通过|
|`oom_evaluate_task`|419-425|426-486|通过|
|`select_bad_process`|492-498|499-518|通过|
|`dump_task`|520-526|527-563|通过|
|`dump_tasks`|575-581|582-606|通过|
|`dump_oom_victim`|608-614|615-626|通过|
|`dump_header`|628-634|635-658|通过|
|`process_shares_mm`|677-683|684-697|通过|
|`__oom_reap_task_mm`|713-719|720-766|通过|
|`oom_reap_task_mm`|774-780|781-826|通过|
|`oom_reap_task`|829-835|836-868|通过|
|`oom_reaper`|870-876|877-901|通过|
|`wake_oom_reaper`|903-909|910-934|通过|
|`queue_oom_reaper` (`CONFIG_MMU`)|949-954|955-969|通过|
|`oom_init`|1003-1008|1009-1017|通过|
|`queue_oom_reaper` (`!CONFIG_MMU`)|1020-1026|1027-1030|通过|
|`mark_oom_victim`|1043-1049|1050-1080|通过|
|`exit_oom_victim`|1085-1090|1091-1099|通过|
|`oom_killer_enable`|1104-1109|1110-1115|通过|
|`oom_killer_disable`|1132-1137|1138-1165|通过|
|`__task_will_free_mem`|1167-1172|1173-1197|通过|
|`task_will_free_mem`|1206-1211|1212-1266|通过|
|`__oom_kill_process`|1268-1273|1274-1370|通过|
|`oom_kill_memcg_member`|1376-1381|1382-1392|通过|
|`oom_kill_process`|1394-1399|1400-1456|通过|
|`check_panic_on_oom`|1461-1466|1467-1490|通过|
|`register_oom_notifier`|1495-1500|1501-1506|通过|
|`unregister_oom_notifier`|1509-1514|1515-1519|通过|
|`out_of_memory`|1531-1536|1537-1618|通过|
|`pagefault_out_of_memory`|1626-1631|1632-1649|通过|
|`process_mrelease`|1651-1656|1657-1730|通过|

结构复核：两个条件编译双实现均单独验收，共 39 个定义；所有中文函数头紧邻声明并显式覆盖四类
信息。原 kernel-doc 位于中文契约之前，`EXPORT_SYMBOL*()` 与下一函数之间未出现错属函数头。

### 复杂函数只读复述抽查

- `out_of_memory()`：禁用时唯一返回 false；全局域先给 notifier 回收机会，已有退出的 current 则直接
  标记 victim/排 reaper。随后处理 NOFS、分类约束、panic 策略和“杀 current”sysctl；常规扫描可能得到
  NULL、`-1` 哨兵或持引用 task，只有最后一种进入 kill。全局真实分配若无任何可杀者会 panic。
- `__oom_kill_process()`：先用带 `task_lock` 的活 mm 线程替换可能已退出的 leader，并转移 chosen 引用；
  `mmgrab` 后先记录事件、发 SIGKILL，再标记 MEMDIE，随后解 task 锁。RCU 下向其它共享同一 mm 的用户
  线程组发信号；若 global init 共享该 mm，则置 OOM_SKIP 并禁止 reaper。末尾严格 `mmdrop` 和 put victim。
- reaper 链：`queue_oom_reaper()` 用 mm flag 去重、取 task 引用并启动延迟 timer；timer 若发现 OOM_SKIP
  就消费引用，否则锁下入队并唤醒 worker；worker 锁下摘链、锁外有限次 trylock/zap，最终置 OOM_SKIP、
  清节点并消费引用。`MMF_UNSTABLE` 在实际遍历前发布，使后续私有 fault 拒绝不可靠内容。
- `process_mrelease()`：pidfd 先取得 task 引用，再找带锁的活 mm 线程并 `mmgrab`；只有所有 mm 共享者都会
  退出才允许 reap。解 task 锁后以 killable mmap 读锁与 exit_mmap 串行，锁内再次检查 OOM_SKIP；所有
  标签路径都按 mm 引用、task 引用逆序释放，非 MMU 配置返回 `-ENOSYS`。

### 关联文件与语义证据

- `include/linux/oom.h`：核对 `struct oom_control`、chosen 字段和公开 API；学习注释不足。
- `mm/page_alloc.c::__alloc_pages_may_oom()`：确认全局入口以 `oom_lock` 串行，并把 true 当作前进条件；
  已有中文但仅部分覆盖。
- `mm/memcontrol.c::mem_cgroup_out_of_memory()`：确认 memcg 入口也在 `oom_lock` 下调用并稳定 memcg 域；
  部分覆盖，尚未独立验收。
- `kernel/exit.c::exit_mm()` 与 `kernel/fork.c::free_signal_struct()`：确认 MEMDIE 计数退出配对，以及
  `signal->oom_mm` 的异步 mmdrop；部分覆盖。
- `mm/mmap.c::exit_mmap()`：确认 OOM_SKIP 在已解除映射后发布，并通过 mmap 写锁周期与 reaper 同步；
  学习注释不足。
- `kernel/power/process.c::freeze_processes()`：确认冻结用户态后调用 disable，并以超时失败回退；
  学习注释不足。

### 英文、状态与边界审计

- 原有英文说明均保留；已覆盖其域资格、选择哨兵、task/mm 引用、锁、reaper 延迟原因与失败边界。
- 已覆盖 sysctl、两把全局 mutex、victim 计数/等待队列、reaper 队列、notifier 链和约束名表。
- 条件编译分别说明 `CONFIG_NUMA`、`CONFIG_MMU` 和 `CONFIG_SYSCTL` 的行为不变量。

### 机械与追加式安全门禁

- 密度：`code=745 comments=833 chinese=389 density=0.522 max_gap=10`，通过。
- `git diff --check -- mm/oom_kill.c`：通过。
- `mm/oom_kill.c` 相对基线：新增 473 行、删除 0 行；未改代码或原有英文注释。
- checkpatch：仅 `LONG_LINE_COMMENT`（278 条），无其它 warning/check/error。
- 构建：工作区无 `.config`，未声称目标对象编译通过。
