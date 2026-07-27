# Linux 内存管理内部实现

> 适用内核版本：v7.2-rc1（基于当前仓库 `5f1d3c45f80d`）
> 核心源码：`mm/`、`include/linux/mm*.h`、`include/linux/mmzone.h`、`arch/*/mm/`
>
> 启动初始化主线：`mm/mm_init.c`、`mm/memblock.c`、`mm/sparse.c`、
> `mm/sparse-vmemmap.c`、`mm/page_alloc.c`、`mm/memory_hotplug.c`

---

## 目录

1. [背景与设计目标](#1-背景与设计目标)
2. [总体技术方案与架构](#2-总体技术方案与架构)
3. [物理内存模型](#3-物理内存模型)
4. [进程虚拟地址空间](#4-进程虚拟地址空间)
5. [页表与地址转换](#5-页表与地址转换)
6. [缺页异常处理](#6-缺页异常处理)
7. [匿名内存、文件映射与页缓存](#7-匿名内存文件映射与页缓存)
8. [写时复制](#8-写时复制)
9. [物理页分配：伙伴系统](#9-物理页分配伙伴系统)
10. [内核对象与虚拟连续内存](#10-内核对象与虚拟连续内存)
11. [内存回收与交换](#11-内存回收与交换)
12. [脏页回写](#12-脏页回写)
13. [NUMA 内存管理](#13-numa-内存管理)
14. [透明大页与 HugeTLB](#14-透明大页与-hugetlb)
15. [内存控制组与 OOM](#15-内存控制组与-oom)
16. [并发、引用与生命周期](#16-并发引用与生命周期)
17. [启动阶段内存管理](#17-启动阶段内存管理)
18. [观测、调试与常见误区](#18-观测调试与常见误区)
19. [关键函数速查](#19-关键函数速查)

---

## 1. 背景与设计目标

内存管理面对的并不是简单的“分配和释放 RAM”。它需要同时解决：

- 每个进程都认为自己拥有独立、连续的地址空间；
- 物理内存有限，但进程申请总量可以超过 RAM；
- 多个进程要安全共享共享库、文件页和共享内存；
- CPU、设备 DMA、文件系统和匿名内存对连续性、延迟和回收能力的要求不同；
- 多核和 NUMA 系统要尽量让数据靠近使用它的 CPU；
- 内存紧张时必须在回收、回写、交换和杀进程之间做选择；
- 热路径要极快，同时还要保证权限隔离、记账和生命周期安全。

Linux 的核心解法是把“地址”“内容”“所有权”和“存储位置”分离：

```
用户虚拟地址
    │ 由 VMA 描述语义与权限
    ▼
多级页表
    │ 缓存于 TLB，按页建立映射
    ▼
物理页 / 文件页 / swap entry / 尚未分配
```

虚拟地址可以存在但尚无物理页；物理页可以被多个页表映射；文件内容可在内存和磁盘之间
迁移；匿名页可换出到 swap；一个对象的内核虚拟地址连续，也不代表其物理页连续。

---

## 2. 总体技术方案与架构

### 2.1 分层架构

```
┌───────────────────────────────────────────────────────────────┐
│ 用户接口：brk / mmap / munmap / mprotect / madvise / mlock   │
├───────────────────────────────────────────────────────────────┤
│ 地址空间层：mm_struct + Maple Tree 中的 vm_area_struct       │
│ 决定某段虚拟地址是否合法、权限和后备存储                    │
├───────────────────────────────────────────────────────────────┤
│ 映射层：多级页表、TLB、缺页异常、COW、反向映射               │
│ 建立“虚拟页 → 物理 folio/swap”的实际关系                     │
├──────────────────────────┬────────────────────────────────────┤
│ 匿名内存 / swap          │ 文件映射 / page cache / writeback │
├──────────────────────────┴────────────────────────────────────┤
│ 物理页层：NUMA node → zone → buddy → page/folio              │
├──────────────────────────┬────────────────────────────────────┤
│ SLUB：小对象             │ vmalloc：虚拟连续、物理可离散     │
├──────────────────────────┴────────────────────────────────────┤
│ 回收：LRU/MGLRU、kswapd、direct reclaim、compaction、OOM     │
└───────────────────────────────────────────────────────────────┘
```

### 2.2 六个关键技术选择

| 问题 | 技术方案 |
|------|----------|
| 地址隔离 | 每进程独立页表与硬件权限检查 |
| 延迟成本 | demand paging，首次访问才分配/装入 |
| 快速 fork | 页表只读共享 + Copy-on-Write |
| 文件 I/O 缓存 | 统一 page cache，读写与 mmap 共享 folio |
| 物理页分配 | 分区的 buddy allocator + per-CPU page lists |
| 内存不足 | 水位线、LRU/MGLRU、回写、swap、compaction、OOM |

---

## 3. 物理内存模型

### 3.1 PFN、`struct page` 与 folio

内核把可管理物理内存切成页帧，页帧号 PFN 通常满足：

```
physical_address = PFN << PAGE_SHIFT
```

每个页帧有一个 `struct page` 描述符，定义于 `include/linux/mm_types.h`。它使用 union
复用字段：同一物理页在不同阶段可能属于 buddy、page cache、匿名映射、slab 或设备内存。
重要状态通过 page flags 表示，如 dirty、writeback、LRU、locked、uptodate。

`struct folio` 是“一个或多个物理页组成、作为整体管理的内存单位”。它避免大量代码误把
compound page 的任意 tail page 当作独立对象，也让 page cache、匿名内存和回收路径自然
支持大于一页的块。

```
folio（order=2）
├── page 0：head，保存主要元数据
├── page 1：tail
├── page 2：tail
└── page 3：tail
```

folio 不等于“大页”：order-0 folio 就是一页；较大的 folio 也不一定使用 PMD 大页映射。

### 3.2 node、zone 与 pageblock

物理内存按硬件拓扑和可寻址能力分层：

```
系统
└── pglist_data（一个 NUMA node）
    ├── ZONE_DMA / ZONE_DMA32（受设备寻址限制）
    ├── ZONE_NORMAL（内核直接映射的普通内存）
    ├── ZONE_MOVABLE（主要容纳可迁移页）
    └── 每个 zone
        ├── free_area[order]：伙伴空闲链表
        ├── per_cpu_pageset：per-CPU 页缓存
        ├── watermark：min/low/high/promo/demote 等水位
        └── LRU 与回收统计
```

pageblock 是迁移和反碎片的粒度。页按 `MIGRATE_UNMOVABLE`、`MOVABLE`、`RECLAIMABLE`、
`CMA` 等迁移类型分组，尽量避免不可移动内核对象把可形成大连续块的区域切碎。

### 3.3 直接映射区

大部分普通 RAM 被永久映射到内核虚拟地址空间，内核可用 `page_address()` 或
`phys_to_virt()` 一类接口访问。映射关系通常是固定偏移，但代码不应在不满足体系结构约束时
自行做地址算术。`HIGHMEM` 体系结构上并非全部物理内存都能永久映射，需要 `kmap_local_page()`。

---

## 4. 进程虚拟地址空间

### 4.1 `mm_struct`

每个用户地址空间由 `struct mm_struct` 描述，典型内容包括：

- 页表根 `pgd`；
- VMA Maple Tree `mm_mt`；
- `mmap_lock` 与页表锁；
- 代码、数据、堆、参数和环境变量边界；
- RSS、锁页数和页表页等统计；
- NUMA 策略、TLB 上下文、用户数与引用数。

`mm_users` 与 `mm_count` 含义不同：前者大致表示使用该用户地址空间的 task 数，后者保护
`mm_struct` 本身及内核侧引用。`mmput()` 和 `mmdrop()` 对应不同层次的释放。

### 4.2 VMA：连续且属性一致的虚拟区间

`struct vm_area_struct` 描述 `[vm_start, vm_end)`：

```
VMA
├── vm_mm                   所属 mm
├── vm_start / vm_end       虚拟地址范围
├── vm_flags                R/W/X、shared、locked、may* 等
├── vm_page_prot            页表保护属性
├── vm_file                 文件映射后端；匿名映射通常为 NULL
├── vm_pgoff                文件页偏移或特殊匿名编码
├── vm_ops                  fault、close、page_mkwrite 等回调
└── anon_vma                匿名页反向映射根
```

VMA 只描述一个范围的规则，并不表示范围内每页都已有 PTE 或物理页。一个 1 GiB 匿名 mmap
在从未访问时，主要成本是 VMA 元数据而非 1 GiB RAM。

当前内核用 Maple Tree 管理 VMA，而不是旧资料常见的 VMA 红黑树。Maple Tree 针对非重叠
区间查找设计，支持 RCU 友好读路径；修改地址空间仍需遵守 `mmap_lock` 和 VMA 锁协议。

### 4.3 `brk()` 与 `mmap()`

- `brk()` 调整传统 heap 末端，最终仍体现为 VMA 的扩展或收缩；
- `mmap()` 创建文件或匿名映射，可指定共享/私有、权限和地址策略；
- `munmap()` 拆分/删除 VMA、清理页表并减少物理页映射引用；
- `mprotect()` 改 VMA 与 PTE 权限，通常需要 TLB shootdown；
- `madvise()` 提供访问模式、回收、fork 继承、THP 等提示或语义控制。

用户分配器的小对象通常从已有 arena 切分；只有扩展/收缩 arena 时才调用 `brk/mmap`，
所以一次 `malloc()` 不等于一次内核物理页分配。

---

## 5. 页表与地址转换

### 5.1 多级页表

Linux 用体系结构无关命名抽象页表层级：

```
虚拟地址
  ├── PGD index
  ├── P4D index
  ├── PUD index
  ├── PMD index
  ├── PTE index
  └── page offset

mm->pgd → PGD → P4D → PUD → PMD → PTE → PFN + permissions
```

具体体系结构可能折叠某些层，也可能允许 PUD/PMD 直接映射 huge page。PTE 不只表示 present
物理页；非 present entry 还可编码 swap、migration、device-private 等状态。

### 5.2 TLB 与页表缓存

CPU 不会为每次访存完整遍历页表，而是把转换缓存到 TLB。修改或撤销 PTE 后，内核必须让
相关 CPU 的旧 TLB entry 失效。若 mm 曾在多个 CPU 上运行，就可能发送跨 CPU IPI，称为
TLB shootdown。

因此“修改一个页表项”不是纯本地操作。`mmu_gather`/`tlb_gather_mmu()` 把一段 unmap 的
失效和页表页释放批量化，避免每页一次 IPI，并确保硬件不再使用页表后再释放内存。

### 5.3 ASID/PCID

ARM64 ASID、x86 PCID 给 TLB entry 加地址空间标签，切换 mm 时不必清空全部 TLB。标识数量
有限，内核需要分配、换代和处理回绕。`switch_mm()` 一类体系结构接口完成页表根与地址空间
上下文切换。

### 5.4 权限检查

页表同时编码用户/内核、读写、执行、脏和访问状态。VMA 是软件策略，PTE 是硬件实际执行的
权限。内核改变 VMA 权限时必须同步更新已有 PTE；尚未建立的 PTE 会在后续 fault 时依据
VMA 生成。

---

## 6. 缺页异常处理

### 6.1 缺页不等于错误

CPU 访问不存在或权限不满足的映射时进入体系结构异常处理。内核先判断地址与访问类型，
再进入通用 MM：

```
体系结构 page fault handler
  → 查找覆盖 address 的 VMA
  → 检查读/写/执行权限与栈扩展条件
  → handle_mm_fault()
      → __handle_mm_fault()
          → 分配中间页表
          → handle_pte_fault()
              ├── do_anonymous_page()
              ├── do_fault() / filemap_fault()
              ├── do_swap_page()
              └── do_wp_page()          COW/写保护 fault
```

如果地址无 VMA、权限不允许或后端 I/O 失败，用户态通常收到 `SIGSEGV` 或 `SIGBUS`；如果成功，
内核修复映射并重试导致异常的指令，对程序透明。

### 6.2 minor 与 major fault

- minor fault：无需从块设备读取，例如首次匿名页、COW 或页已在 page cache；
- major fault：需要等待存储 I/O，例如文件页不在缓存或换出页需读回。

major/minor 是是否需要 I/O 的统计分类，并不直接等同于“慢/快”。内存分配、锁竞争和回收
都可能让 minor fault 延迟很高。

### 6.3 匿名页首次写入

私有匿名映射可在只读访问时映射共享 zero page，避免为全零内容分配实际页；首次写入再分配
并清零新 folio。具体是否使用 zero page 受映射类型和配置影响。

### 6.4 fault 上下文中的竞态

fault 可能在等待 I/O 或分配内存时释放锁，期间 VMA、PTE 或文件页状态可能变化。返回值中的
`VM_FAULT_RETRY`、PTE lock、folio lock 和重新校验共同处理这些竞态。文件系统 fault 回调
不能假定从开始到结束映射完全不变。

---

## 7. 匿名内存、文件映射与页缓存

### 7.1 匿名内存

堆、用户栈和 `MAP_ANONYMOUS` 私有映射没有普通文件作为后端。它们的内容由匿名 folio 承载，
通过 `anon_vma` 建立反向映射，内存不足时可以进入 swap。

匿名页不能简单丢弃：若页不是全零且还要保留内容，必须保留在 RAM 或先写入 swap/zswap。

### 7.2 文件映射与 page cache

文件内容的内存缓存由 `address_space` 管理，其 `i_pages` 使用 XArray 把文件页索引映射到
folio 或特殊 entry：

```
inode
  └── address_space (i_mapping)
      └── i_pages: XArray
          index 0 → folio A
          index 1 → folio A 的后续范围（large folio）
          index N → shadow/value/special entry
```

`read()`、`write()` 和 `mmap()` 访问同一文件时使用同一 page cache，因此不需要两份独立
缓存。缓存命中直接复制或映射；未命中则分配 folio 并通过文件系统 `address_space_operations`
读取。直接 I/O 会绕过主要的数据缓存路径，但仍需和已有缓存、写回及文件大小变化协调一致性。

### 7.3 MAP_SHARED 与 MAP_PRIVATE

- `MAP_SHARED` 写入修改 page cache，其他共享映射可观察，之后按回写策略落盘；
- `MAP_PRIVATE` 初始也可映射 page cache，写入时 COW 成匿名页，不修改原文件。

“private”不是立即复制整个文件，只是在写 fault 时复制被修改的 folio/页。

### 7.4 反向映射 rmap

回收、迁移和 COW 不只要知道“虚拟地址映射到哪个页”，还要从页找到所有 PTE。匿名页通过
`anon_vma`，文件页通过 `address_space` 和 VMA 区间建立 rmap。`rmap_walk()` 一类路径据此
找到并撤销或更新映射。

---

## 8. 写时复制

### 8.1 fork COW 流程

`fork()` 复制 `mm_struct`、VMA 和页表结构，但私有可写页面不会立刻复制内容：

```
fork 前： parent PTE = writable → folio X

fork 后： parent PTE = read-only ─┐
                                  ├→ folio X (mapcount 增加)
          child  PTE = read-only ─┘

child 写：write fault → do_wp_page()
          → 若无法独占复用，分配 folio Y 并复制
          → child PTE = writable → folio Y
```

只读文件映射本来就不可写，不需要 COW；共享可写映射写回原后端，也不是 fork 私有 COW。

### 8.2 是否一定复制

不一定。如果内核确认 folio 已被当前映射独占且其他条件允许，可以直接把 PTE 升级为可写，
避免复制。引用计数不等于映射计数，page cache、GUP pin、swap cache、KSM 和大 folio 都会
影响能否安全复用。

### 8.3 长期 pin 的影响

RDMA、VFIO 等通过 `pin_user_pages*()` 长期固定用户页。pin 会妨碍迁移、回收和 COW，内核
使用 FOLL_PIN 语义单独记账。驱动不应把普通 `get_user_pages()` 当成无限期 DMA pin 接口。

---

## 9. 物理页分配：伙伴系统

### 9.1 order 与伙伴合并

伙伴系统管理 `2^order` 个连续页：

```
order 0 = 1 page
order 1 = 2 pages
order 2 = 4 pages
...
```

申请 order 2 而只有 order 4 空闲块时，order 4 被逐级拆分；释放时，如果同阶 buddy 也空闲
且迁移类型允许，就逐级合并。buddy PFN 可由块大小对应位翻转得到。

### 9.2 分配路径

```
alloc_pages(gfp, order)
  → __alloc_pages_noprof()
    → get_page_from_freelist()
      ├── order-0：优先 per-CPU page list
      └── rmqueue()：从 zone free_area 拆块
    → 首次失败
      ├── 唤醒 kswapd
      ├── direct reclaim
      ├── compaction（高阶分配）
      ├── 按 GFP 策略重试/访问 reserve
      └── OOM（满足条件时）
```

per-CPU page lists 缓存少量 order-0/低阶页，避免每次分配都争用 zone lock；批量补充和归还
又把全局锁成本摊薄。

### 9.3 GFP 标志不是“内存类型名称”

GFP 同时描述可睡眠性、允许的回收行为、zone 范围和用途：

- `GFP_KERNEL`：普通内核上下文，可睡眠并执行直接回收；
- `GFP_ATOMIC`：原子上下文，不能睡眠，可使用部分紧急 reserve；
- `GFP_NOWAIT`：不做可能睡眠的直接回收；
- `GFP_DMA/GFP_DMA32`：限制物理 zone；
- `__GFP_ZERO`：返回前清零；
- `__GFP_MOVABLE/RECLAIMABLE`：帮助反碎片与迁移决策。

选择错误的 GFP 可能导致“sleeping function called from invalid context”、无谓 OOM，或耗尽本应
留给关键路径的 reserve。

### 9.4 水位与 reserves

zone 的 min/low/high 水位控制分配与后台回收：低于 low 通常唤醒 kswapd，回收到 high 附近；
低于 min 后普通分配受限，但 `PF_MEMALLOC` 等关键回收路径可访问 reserve，避免“回收本身也因
分不到内存而无法继续”的死锁。

### 9.5 碎片与 compaction

空闲页总数足够不代表存在高阶连续块。compaction 将可迁移页搬到一侧、空闲页聚到另一侧，
为 THP、设备或高阶分配形成连续空间。不可移动页散布会造成外部碎片，使 compaction 失败。

---

## 10. 内核对象与虚拟连续内存

### 10.1 分配器层次

| 接口 | 连续性 | 典型用途 |
|------|--------|----------|
| `alloc_pages()` | 物理连续页 | 页表、DMA 基础、页级对象 |
| `kmalloc()` | 小对象通常物理连续 | 通用内核对象/缓冲区 |
| `kmem_cache_alloc()` | slab 对象 | 固定类型高频对象 |
| `vmalloc()` | 虚拟连续，物理可离散 | 大型内核缓冲、模块等 |
| `kv*alloc()` | 优先 kmalloc，失败回退 vmalloc | 不强制物理连续的大缓冲 |

SLUB 从伙伴系统批量取得页，再切成对象并通过 per-CPU sheaf/cache 等快速路径服务分配；
详细实现见 `doc/06 linux-slab-internals.md`。

### 10.2 vmalloc

`vmalloc()` 先在内核 vmalloc 地址区预留连续虚拟范围，再分配离散物理页并建立内核页表。
它降低高阶连续分配压力，但有页表、TLB 和映射建立成本，也不能在所有原子上下文使用。

设备 DMA 需要满足设备和 IOMMU 的地址约束，不能因为 CPU 看到虚拟连续就假定设备也能直接
访问；驱动应使用 DMA API。

---

## 11. 内存回收与交换

### 11.1 可回收对象

内存压力下主要目标包括：

- 干净文件 folio：可直接丢弃，未来从文件重读；
- 脏文件 folio：先安排 writeback，再回收；
- 匿名 folio：若启用 swap，写入 swap/zswap 后回收；
- 可收缩 slab cache：通过 shrinker 回收 inode、dentry 等缓存；
- 可迁移页：为连续分配进行 compaction，而非真正释放内容。

被 mlock、长期 pin、正在 writeback 或其他条件约束的页可能暂时不可回收。

### 11.2 后台回收与直接回收

`kswapd` 是每 node 的后台线程，水位降低时提前回收；若分配路径仍无法取得页，调用者进入
direct reclaim，自己扫描并可能等待回写。直接回收会直接增加请求延迟，因此生产系统关注
它的发生频率和耗时，而不只是剩余内存数。

### 11.3 LRU 与 MGLRU

传统模型按匿名/文件、active/inactive 组织 LRU；现代内核可使用 Multi-Gen LRU，把 folio
按最近访问代际分组，利用 PTE young 位、反向映射和访问反馈估计热度，优先回收老代。

“LRU”并非每次访问都移动一条全局链表，那会产生巨大锁开销。访问位采样、批处理、per-CPU
缓存和代际更新共同近似工作集。

### 11.4 reclaim 的核心循环

```
内存压力
  → shrink_node()
    → 估算 anonymous/file 扫描比例
    → shrink_lruvec()
      → 隔离候选 folio
      → 检查引用、脏、writeback、映射和可迁移性
      → unmap / swap / writeback / free
    → shrink_slab() 调用注册的 shrinker
```

回收效率用扫描量、回收量、refault 和 PSI stall 等共同评价。频繁回收后很快 refault，说明
工作集大于可用内存或回收选择不佳，系统可能发生 thrashing。

### 11.5 swap、swap cache 与 zswap

匿名页换出时分配 swap entry，PTE 从 present 映射变为 swap entry。再次访问触发
`do_swap_page()`，从 swap cache、zswap 或后端设备恢复。

zswap 是 swap 前端的压缩内存缓存：压缩后的页仍占 RAM，但通常比原页小，可减少慢速设备
I/O；它不能凭空增加无限内存，达到限制后仍要淘汰到真正 swap 后端。

### 11.6 refault 与 thrashing

文件页回收时可留下 shadow entry；若很快再次访问，内核用 refault 距离判断它是否属于工作集，
并调整活跃度。PSI memory 指标反映任务因回收/compaction 等内存压力而停顿的时间，比单看
`MemFree` 更接近用户感受到的性能问题。

---

## 12. 脏页回写

写入 page cache 后，folio 被标为 dirty。内容不会在每次 `write()` 返回前立刻落盘，而是由
writeback 机制批量处理：

```
write/mmap store
  → folio dirty
  → 超过后台阈值或到期
  → wb_work / flusher
  → filesystem writepages/writepages equivalent
  → block layer / device
  → 清 dirty，结束 writeback
```

若脏页产生速度超过设备写出能力，`balance_dirty_pages()` 对写入者节流，防止全部可用内存被
脏页占满。阈值可按全局和 memcg/domain 计算。

`fsync()` 请求文件数据及必要元数据达到持久化语义；`write()` 成功通常只表示数据已进入内核，
不能等同于掉电安全。mmap 写入需要 fault 时的 `page_mkwrite` 等协议与文件系统协调块分配、
文件大小和冻结状态。

---

## 13. NUMA 内存管理

### 13.1 首次触碰

匿名内存通常按 first-touch 分配：建立映射不立即拿物理页，哪个 CPU 首次写入，fault 就倾向
在哪个 NUMA node 分配。因此并行程序只由主线程串行初始化大数组，可能把页集中在一个 node。

### 13.2 分配策略与 zonelist

分配器按 mempolicy、cpuset 和 zonelist 选择首选 node/zone，失败后按允许范围 fallback。
常见策略包括 default/local、bind、preferred 和 interleave。`mbind()`、`set_mempolicy()`、
`numactl` 可控制用户空间策略。

### 13.3 自动 NUMA balancing

内核可周期性把 PTE 设成特殊不可访问状态，通过 hinting fault 观察哪个 task/node 在访问，
随后迁移页或任务。迁移收益需要覆盖扫描、fault、复制和 TLB 失效成本，因此不是越积极越好。

### 13.4 memory tiering

系统可能同时有快慢内存。内核可把冷页 demote 到较慢 tier，在访问或策略驱动下 promote。
zone 的回收水位与 node 间 demotion path 协同工作，但硬件拓扑、cpuset、memcg 与 pin 都会限制迁移。

---

## 14. 透明大页与 HugeTLB

### 14.1 为什么需要大页

更大的映射覆盖更多地址，减少页表层级和 TLB miss；代价是分配/compaction 延迟、内部碎片、
COW 成本以及回收粒度变粗。

### 14.2 THP

Transparent Huge Pages 尽量对应用透明。fault 时可直接分配大 folio，或由 `khugepaged`
把满足条件的小页合并；内存压力、mprotect、COW 等情况下可拆分。现代 THP 不应只理解为固定
PMD 大页，内核也支持不同尺寸的 multi-size THP（取决于体系结构和配置）。

### 14.3 HugeTLB

HugeTLB 使用预留/显式管理的 huge page pool，应用通过 hugetlbfs 或 `MAP_HUGETLB` 使用。
它的资源保证和记账更明确，但配置与使用更不透明，不能像 THP 一样随意回退为普通页。

---

## 15. 内存控制组与 OOM

### 15.1 memcg 记账

memory cgroup 对匿名、page cache、部分内核内存和 swap 等记账，并提供 `memory.current`、
`memory.max`、`memory.high`、`memory.low/min`、`memory.events` 等控制接口。

- `memory.max` 是硬限制，超限回收失败可触发 memcg OOM；
- `memory.high` 主要通过回收/节流施压，不等同于立即杀进程；
- `memory.low/min` 提供不同强度的回收保护；
- `memory.swap.max` 控制 swap 使用。

一个 folio 可能被多个进程映射，但只归属一个 memcg charge 关系；共享并不意味着对每个映射者
重复计算同一物理页。

### 15.2 OOM 是最后手段

当允许的分配范围内，回收、swap、compaction 和重试都无法满足关键分配时，才可能进入 OOM：

```
分配失败
  → 判断是否允许 OOM、是否有正在退出者可释放内存
  → 选择 victim（oom_badness 等启发式）
  → 标记/发送 SIGKILL
  → victim 获得内存回收优先权，尽快退出释放 mm
```

系统 OOM 与 memcg OOM 范围不同：前者在允许的系统节点/zone 中选择，后者通常限制在超限
cgroup 子树。`oom_score_adj` 影响选择，但不是资源保障机制。

### 15.3 overcommit

虚拟内存承诺不等于物理页已存在。`vm.overcommit_memory` 和 commit accounting 决定某些
匿名映射/扩展是否在申请时被拒绝。即使申请成功，运行时仍可能因实际触碰、memcg、NUMA
约束、不可回收页或物理碎片而失败；反之，严格记账也不是物理页预留系统。

---

## 16. 并发、引用与生命周期

内存管理同时面对 CPU 页表遍历、fault、unmap、回收、迁移、GUP 和设备访问。核心同步包括：

| 机制 | 保护范围 |
|------|----------|
| `mmap_lock` | mm 的 VMA 拓扑和大范围操作 |
| VMA lock / Maple Tree 协议 | VMA fault 读路径与局部修改协调 |
| page-table lock | PTE/PMD 等页表项修改 |
| folio lock | folio 内容填充、截断、迁移等状态转换 |
| XArray lock | page cache 索引结构修改 |
| LRU lock | lruvec 链表/代际的结构性操作 |
| zone lock | buddy free_area |
| refcount/mapcount/pincount | folio 所有权、映射数和长期 pin |
| RCU | Maple Tree 等允许无锁读的对象生命周期 |

### 16.1 引用计数不等于映射计数

folio/page 的 `_refcount` 包含 page cache、调用者临时引用、映射相关引用和其他所有者；
`_mapcount` 描述 PTE 映射维度。判断页能否复用、迁移或释放不能只看其中一个数字，应使用
对应 MM helper。

### 16.2 先撤销可达性，再延迟释放

unmap 的典型原则是：在锁下让新查找无法到达对象，清页表并收集 TLB invalidation，确认 CPU
不再使用旧转换后再释放物理页/页表页。顺序错误会形成 use-after-free，硬件仍可能通过陈旧
TLB 访问已经重新分配给其他用途的页。

### 16.3 `get_user_pages` 不是普通 memcpy

GUP 必须处理 fault、COW、权限、huge page、迁移和退出竞态。拿到 page 后必须按 API 配对
释放；DMA 写入还涉及 dirty 标记和 cache coherency。能从用户指针读写，不代表可绕过
`copy_from_user()`/`copy_to_user()` 的异常与安全协议。

---

## 17. 启动阶段内存管理

### 17.1 为什么不能一开始就使用伙伴系统

机器刚进入内核时，固件、设备树或 EFI 只能告诉内核“哪些物理地址可能是 RAM”。此时还没有
可工作的伙伴系统，因为伙伴系统本身依赖以下元数据：

- 每个 NUMA node 的 `pg_data_t`；
- 每个 node 内各个 `struct zone` 的 PFN 边界；
- 每个 zone 的 `free_area[order]` 链表和锁；
- 每个物理页对应的 `struct page`；
- pageblock 的迁移类型；
- zonelist 和 per-CPU pageset。

这些对象又需要内存才能建立。若要求伙伴系统分配自己的元数据，就会出现循环依赖：

```text
要分配 struct page / zone 元数据
  → 需要页分配器
  → 页分配器要先读取 struct page / zone
  → 元数据尚不存在
```

Linux 用 memblock 打破循环。memblock 只维护少量物理区间，算法不适合长期高频分配，但在
页分配器尚不存在时已经足够：

```text
memblock.memory
  固件确认的 RAM 区间

memblock.reserved
  内核镜像、DTB、initrd、早期页表、CMA、crashkernel 等不可交给 buddy 的区间

可移交页
  memory - reserved
```

贯穿本章的问题是：

> 一段固件报告的 RAM，经过哪些状态和发布边界，才会成为
> `alloc_pages()` 可以返回的物理页？

答案不是“注册到 memblock 后立即可用”，而是先建立描述它的全部元数据，最后才一次或分批
移交给 buddy。

### 17.2 完整阶段地图

当前源码的主调用阶段是：

```text
start_kernel()
  → setup_arch()
      → arm64_memblock_init()
          建立 memblock.memory / memblock.reserved
          建立内核镜像、DTB、initrd 等保留关系
      → bootmem_init()
          确定 min/max PFN、NUMA、DMA zone、CMA、crashkernel
  → mm_core_init_early()
      → hugetlb_cma_reserve()
      → hugetlb_bootmem_alloc()
      → free_area_init()
          arch zone 边界
          sparse/vmemmap backing
          ZONE_MOVABLE
          pgdat/zone/free_area
          struct page 基态
  → mm_core_init()
      → build_all_zonelists()
      → page allocator CPU hotplug/per-CPU 基础设施
      → 调试和安全元数据
      → memblock_free_all()          关键所有权移交
      → arch mem_init()
      → kmem_cache_init()            SLUB 上线
      → vmalloc_init()
  → padata_init()
  → page_alloc_init_late()
      → 完成 deferred struct page
      → 丢弃 memblock 私有元数据
      → page_ext/sysctl 等晚期收尾
```

这里有三个容易混淆的时间点：

| 时间点 | 已经具备 | 仍然不能假设 |
|--------|----------|--------------|
| `setup_arch()` 后 | memblock 物理区间、早期页表、架构 PFN/NUMA 信息 | buddy、slab、vmalloc 可用 |
| `mm_core_init_early()` 后 | pgdat/zone/memmap 和空 buddy 容器 | 所有普通 RAM 已进入 free list |
| `memblock_free_all()` 后 | buddy 已接收普通空闲页，slab 可以随后建立 | deferred 高端 `struct page` 已全部完成 |
| `page_alloc_init_late()` 后 | deferred 页、最终统计、page_ext 和 memblock 清理完成 | memblock 仍可作为普通分配器使用 |

所以“buddy 数据结构初始化完成”和“物理页已经交给 buddy”是两个不同事件。

### 17.3 四类核心对象与三个页数

#### 17.3.1 `pg_data_t`：一个 NUMA node 的内存管理根

`struct pglist_data` 定义于 `include/linux/mmzone.h`，常通过 `pg_data_t` 使用。它拥有：

```text
pg_data_t
├── node_id
├── node_start_pfn
├── node_spanned_pages
├── node_present_pages
├── node_zones[MAX_NR_ZONES]
├── node_zonelists[]
├── kswapd / kcompactd 状态与等待队列
├── node 级 lruvec
└── deferred init 边界 first_deferred_pfn
```

`pg_data_t` 是 node 的长期对象。启动时由架构、静态数组或早期分配提供，本文件主要负责填充；
memoryless node 也可能拥有 `pg_data_t`，但其 zone 容量为零，之后热添加内存时再重建内部状态。

#### 17.3.2 `struct zone`：地址能力与分配策略边界

zone 不是“任意大小的内存池”。它首先表达物理地址能力：

- `ZONE_DMA`、`ZONE_DMA32`：满足受限设备的 DMA 地址范围；
- `ZONE_NORMAL`：内核通常可以直接映射的普通内存；
- `ZONE_HIGHMEM`：32 位等平台无法永久直接映射的高端内存；
- `ZONE_MOVABLE`：尽量只容纳可迁移页，提高热拔除和连续分配成功率；
- `ZONE_DEVICE`：由设备内存映射管理的特殊页。

每个 zone 内部才有伙伴系统的 `free_area[]`、zone lock、watermark、per-CPU pageset 和
pageblock 元数据。

#### 17.3.3 `struct page`：PFN 的软件描述符

`struct page` 不是物理页内容，而是内核用来管理该页的元数据。对 PFN `pfn`，内存模型提供：

```text
pfn_to_page(pfn)   PFN → 描述符
page_to_pfn(page)  描述符 → PFN
```

`__init_single_page()` 建立 flags 中的 node/zone/section 链接、引用基态、mapcount、链表和调试
状态。此时 page 只是“描述符有效且尚未空闲”，并不意味着它已经进入伙伴 free list。

#### 17.3.4 pageblock：迁移和反碎片粒度

伙伴系统以 order 管理连续块，迁移和隔离则以更粗的 pageblock 管理。一个 pageblock 保存
`MIGRATE_MOVABLE`、`MIGRATE_UNMOVABLE`、`MIGRATE_RECLAIMABLE`、`MIGRATE_CMA` 等类型。

pageblock 类型不是页的所有权；它是对分配器的放置提示和迁移约束。错误地让大量不可移动对象
散入 MOVABLE/CMA pageblock，会使内存热拔除、THP 和高阶连续分配更容易失败。

#### 17.3.5 `spanned`、`present` 与 `managed`

这三个计数不能混用：

```text
spanned_pages
  zone_start_pfn 到 zone_end_pfn 的地址跨度，包含物理洞

present_pages
  span 中实际存在并归属该 zone 的物理页
  = spanned_pages - absent_pages

managed_pages
  当前真正由 buddy 管理的 present 页
  通常不含仍被固件、内核镜像、早期元数据等保留的页
```

例子：

```text
zone PFN span             0x10000 pages
其中物理洞               0x02000 pages
present_pages            0x0e000 pages
其中启动保留             0x01000 pages
最终 managed_pages       0x0d000 pages
```

`free_area_init_core()` 会先用启动容量建立 zone 基线；`memblock_free_all()` 在真正移交前调用
`reset_all_zones_managed_pages()` 清零，再由 `__free_pages_core()` 按实际释放页累加。
因此最终 managed 数来自真实移交结果，而不是简单复制 present。

### 17.4 从固件内存图到 memblock

不同架构获得 RAM 的方式不同：

```text
x86       EFI/e820
arm64     DT / ACPI
其他架构  固件或平台表
```

以 arm64 为例，`setup_arch()` 中的 `arm64_memblock_init()` 整理物理内存并建立早期保留关系，
随后 `bootmem_init()` 完成：

```text
min_low_pfn / max_pfn
  DRAM 的 PFN 边界

arch_numa_init()
  为 memblock ranges 建立 nid

dma_limits_init()
  决定 ZONE_DMA / ZONE_DMA32 上界

dma_contiguous_reserve()
  在正确 DMA 地址范围内预留 CMA

arch_reserve_crashkernel()
  在资源树建立前锁定 crash kernel 内存
```

顺序会影响正确性。例如 CMA 必须在 DMA 物理上界确定后选址，否则可能预留到设备不能访问的
高地址；crashkernel 必须在标准资源登记前保留，否则同一地址可能被当成普通 RAM 使用。

memblock 的 range 描述物理地址，不等于 zone。zone 要到架构上界、NUMA 和 movable 策略都
确定后才能计算。

### 17.5 `mm_core_init_early()`：先预留，再建立分配器骨架

`mm_core_init_early()` 位于 `setup_arch()` 之后：

```text
mm_core_init_early()
  → hugetlb_cma_reserve()
      为 gigantic HugeTLB 预留 CMA 区域
  → hugetlb_bootmem_alloc()
      用 memblock 分配 buddy 无法保证得到的巨大连续页
  → free_area_init()
      建立通用物理页管理骨架
```

gigantic hugepage 的 order 可能超过伙伴系统最大 order。若等 buddy 上线后再申请，大块连续
物理内存可能已经被切碎，所以必须先于普通页移交完成预留。

#### 17.5.1 `free_area_init()` 的六个阶段

```text
阶段 1：架构 zone 上界与内存模型 backing
  arch_zone_limits_init(max_zone_pfn)
  sparse_init()

阶段 2：ZONE_MOVABLE
  find_zone_movable_pfns_for_nodes()

阶段 3：输出 zone/node 范围并建立 subsection map
  sparse_init_subsection_map()

阶段 4：全局不变量
  mminit_verify_pageflags_layout()
  setup_nr_node_ids()
  set_pageblock_order()

阶段 5：逐 node 初始化
  free_area_init_node()
    → calculate_node_totalpages()
    → alloc_node_mem_map()          FLATMEM
    → free_area_init_core()
    → lru_gen_init_pgdat()

阶段 6：页描述符和最终边界
  sparse_vmemmap_init_nid_late()
  calc_nr_kernel_pages()
  memmap_init()
  set_high_memory()
```

这一阶段建立的是“空的分配器容器”：

```text
zone->free_area[order] 链表已经初始化
zone lock / seqlock / waitqueue 已经初始化
struct page 基础状态已经建立

但是：
普通空闲页尚未批量挂入 free_area[]
```

这种“先构造、后发布”避免其他 CPU 或子系统拿到元数据尚不完整的页。

### 17.6 zone 边界、空洞与 `ZONE_MOVABLE`

#### 17.6.1 架构 zone 是全局候选范围

`arch_zone_limits_init()` 只给出每种 zone 的最大 PFN。例如 arm64 根据 DMA mask 与 DRAM
末端填写 DMA、DMA32 和 NORMAL 上界。`free_area_init()` 再把相邻上界转换成：

```text
arch_zone_lowest_possible_pfn[zone]
arch_zone_highest_possible_pfn[zone]
```

这是“理论上可能属于该 zone 的全局范围”，不表示每个 node 在其中都有 RAM。

#### 17.6.2 每 node 的真实容量

`get_pfn_range_for_nid()` 从 memblock ranges 求 node 的最小包围区间。对每个 zone：

```text
zone_spanned_pages_in_node()
  架构 zone ∩ node span ∩ movable 策略

zone_absent_pages_in_node()
  物理洞 + mirror 策略排除页

present_pages
  spanned - absent
```

物理洞仍属于 span，但没有可分配页。这样 zone 可以用一个连续 PFN 范围表示，同时通过
`pfn_valid()`、memblock 和 present 计数跳过不存在的 RAM。

#### 17.6.3 `kernelcore=` 与 `movablecore=`

`ZONE_MOVABLE` 没有固定的架构地址窗口。启动参数可以指定：

```text
kernelcore=<size|percent>
  至少保留多少内存给不可迁移内核分配

movablecore=<size|percent>
  希望划出多少内存用于可迁移分配

kernelcore=mirror
  把固件标记的镜像内存优先留给不可迁移内核分配

movable_node
  倾向把 hotpluggable memblock ranges 划入 movable
```

`find_zone_movable_pfns_for_nodes()` 尽量把 kernelcore 均匀分布到可用 node。容量小的 node
可能先耗尽，剩余 kernelcore 再由其他 node 承担，所以不同 node 的
`zone_movable_pfn[nid]` 可以不同。

`ZONE_MOVABLE` 提高热拔除和连续分配成功率，但不是绝对保证。长期 pin、启动期 memblock
分配、内存洞、hwpoison、offline 页和 memmap-on-memory 都可能形成不能正常迁移的例外。

### 17.7 SPARSEMEM、vmemmap 与 `struct page`

#### 17.7.1 为什么不能简单分配一个巨大数组

若物理地址空间稀疏，按最大 PFN 分配平坦 `struct page` 数组会为不存在的洞浪费大量内存。
SPARSEMEM 按 section 管理：

```text
物理地址空间
  → section
      → present / online 状态
      → node id
      → subsection map
      → 对应的 struct page backing
```

`sparse_init()` 先调用 `memblocks_present()` 标记 present sections，再按 node 聚合 section，
由 `sparse_init_nid()` 建立 backing 和 PFN→section 映射。

#### 17.7.2 vmemmap 的作用

SPARSEMEM_VMEMMAP 在内核虚拟地址中为 `struct page` 提供统一连续视图：

```text
vmemmap + pfn * sizeof(struct page)
  → pfn 对应描述符
```

虚拟连续不要求描述符 backing 物理连续。它让 `pfn_to_page()` 接近固定地址算术，同时仍可只
为实际存在的 section 建立页表和 backing。

HugeTLB 和 ZONE_DEVICE 还会优化 vmemmap：多个 compound tail PFN 可复用少量描述符页。
`compound_nr_pages()` 和 `vmemmap_populate_compound_pages()` 必须对同一种几何达成一致，
否则初始化代码可能访问根本没有独立 backing 的 tail 描述符。

#### 17.7.3 `memmap_init()` 的发布顺序

`memmap_init()` 按 PFN 顺序遍历 memblock ranges，并让每个 zone 与 range 求交：

```text
memmap_init()
  → memmap_init_zone_range()
      → memmap_init_range()
          → __init_single_page()
          → 初始化 pageblock migratetype
      → init_unavailable_range()
          为有描述符 backing、但没有真实 RAM 的洞建立 Reserved 状态
```

顺序要求是：

```text
描述符 backing 存在
  → struct page 基础字段有效
  → pageblock 类型有效
  → 页才有资格进入 buddy
```

如果先释放页再初始化描述符，buddy 在计算相邻块、读取 flags 或合并时可能访问未初始化
`struct page`。

### 17.8 `memblock_free_all()`：真正的所有权移交

`mm_core_init()` 前半段先完成 zonelist、CPU hotplug、page_ext、KFENCE、KMSAN 等必须位于
移交前的准备，然后执行：

```text
memblock_free_all()
  → free_unused_memmap()
  → reset_all_zones_managed_pages()
  → memblock_clear_kho_scratch_only()
  → free_low_memory_core_early()
      遍历 memory - reserved
      → memblock_free_pages(pfn, order)
          → __free_pages_core(page, order, MEMINIT_EARLY)
  → totalram_pages_add(pages)
```

`__free_pages_core()` 完成关键状态转换：

```text
移交前：
  struct page 已初始化
  refcount 表示 allocated/not-free 基态
  页不在 buddy free_area

移交：
  清除启动保留状态
  refcount → 0
  zone->managed_pages 增加
  以尽可能大的 buddy block 加入 free_area

移交后：
  alloc_pages() 可以返回该页
  其他 CPU/子系统可能立即修改其 flags、引用和内容
```

跨过这个边界后，memblock 不再拥有该空闲页。任何早期代码都不能继续把同一物理范围当作
memblock 私有临时存储，否则会和正常分配者形成 use-after-free 或双重分配。

`memblock_free_all()` 后，arm64 的 `mem_init()` 发布 `page_alloc_available=true`，随后
`kmem_cache_init()` 建立 SLUB，`vmalloc_init()` 建立 vmalloc 管理结构。SLUB 如何从 buddy
取得页并切成小对象，见 `doc/06 linux-slab-internals.md`。

### 17.9 deferred struct page：为什么延迟，又如何保持安全

大内存机器逐页初始化全部 `struct page` 会显著延长单核启动时间。启用
`CONFIG_DEFERRED_STRUCT_PAGE_INIT` 时，最高 zone 的高 PFN 尾部可以延迟：

```text
早期 memmap_init()
  → 低 zone 全部初始化
  → 最高 zone 至少初始化一个 section
  → 在 section 边界记录 pgdat->first_deferred_pfn
  → 剩余尾部暂不触碰
```

低 zone 不能延迟，因为 DMA、DMA32 等地址受限启动分配没有替代来源。`early_page_ext`
要求完整连续描述符时也会关闭 deferred。

#### 17.9.1 按需增长

后台 worker 启动前，分配器可能已需要更多页：

```text
get_page_from_freelist()
  → 发现 deferred_pages static key 开启
  → deferred_grow_zone(zone, order)
      持 pgdat_resize_lock
      按 section 初始化
      立即释放其中 memblock free pages
      推进 first_deferred_pfn
```

函数返回“确实推进过”不等于请求 order 一定能成功。新 section 内可能夹有 memblock 保留页，
因此仍可能无法形成连续高阶块，调用者必须重新检查 free list。

#### 17.9.2 后台并行完成

`page_alloc_init_late()` 运行前，`padata_init()` 已经完成：

```text
page_alloc_init_late()
  → 每个 N_MEMORY node 创建 pgdatinit<nid> kthread
  → deferred_init_memmap()
      绑定 node CPU mask
      取得并关闭 first_deferred_pfn 的按需增长所有权
      padata 按 section 切成不重叠任务
      deferred_init_pages()
      deferred_free_pages()
  → atomic completion 等待所有 node
  → static_branch_disable(&deferred_pages)
```

每个任务遵循“先初始化本 chunk 全部描述符，再释放页”。洞和 reserved 页的描述符已在早期
建立，因而 `__free_one_page()` 查看潜在 buddy 时不会读到无效元数据。

并发控制包括：

- `pgdat_resize_lock`：按需 grow 与后台 worker 争夺 deferred 边界；
- padata 不重叠 PFN chunk：避免两个 worker 初始化同一页；
- `pgdat_init_n_undone` 与 completion：最后一个 node 唤醒启动线程；
- `deferred_pages` static key：全部完成后永久移除分配热路径检查。

### 17.10 `page_alloc_init_late()` 的收尾边界

deferred worker 汇合后，内存总量和空闲量才最终稳定：

```text
page_alloc_init_late()
  → mem_init_print_info()
      打印最终 Memory: available/total
  → buffer_init()
  → memblock_discard()
      丢弃 memblock 私有数组/backing
  → shuffle_free_memory()
      随机化每 node 的空闲链表
  → set_zone_contiguous()
      验证 zone 是否完全无物理洞
  → page_ext_init()               deferred 情况
  → page_alloc_sysctl_init()
```

`memblock_discard()` 是第二个重要生命周期边界：

```text
memblock_free_all()
  释放 memblock 管理的普通空闲物理页

memblock_discard()
  释放 memblock 自身为 region 数组等私有元数据占用的内存
```

前者改变物理页所有权，后者销毁早期分配器自己的管理数据，不能混为同一步。

### 17.11 memory hotplug 是启动流程的运行期镜像

热添加内存不能重新执行整个启动序列，但要建立相同不变量：

```text
新 node 尚无内存
  → hotadd_init_pgdat()
      → free_area_init_core_hotplug(pgdat)
          初始化 pgdat/zone 锁、等待队列和空计数
      → build_all_zonelists(pgdat)

添加 PFN range
  → 建立 sparse section / vmemmap backing
  → memmap_init_range(..., MEMINIT_HOTPLUG)
      普通 hotplug page 初始标 PageOffline
  → online_pages()
      更新 zone span/present
      → generic_online_page()
          → __free_pages_core(..., MEMINIT_HOTPLUG)
              清 PageOffline
              managed_pages 增加
              加入 buddy
```

启动路径与 hotplug 路径的共同不变量是：

```text
先有稳定拓扑和 struct page
  → 再发布 present/managed 计数
  → 最后让分配器能够取得该页
```

差异在于启动期大多单线程，而 hotplug 发生在运行期，必须使用 memory hotplug 锁、pgdat resize
锁、zone span seqlock、CPU hotplug 与 notifier 协议，并具备失败回滚：

```text
online 失败
  → MEM_CANCEL_ONLINE 通知
  → 撤销 node first-memory 通知
  → remove_pfn_range_from_zone()
  → 页面保持 offline，不进入 buddy
```

### 17.12 并发、发布和失败模型

#### 17.12.1 早期阶段为什么很多写入没有锁

`free_area_init()` 大部分路径运行在 boot CPU、SMP 普通并发尚未开始的阶段。正确性来自调用
顺序，而不是 zone lock：

```text
架构边界
  → 对象分配
  → 字段初始化
  → free list 容器
  → 页移交
  → 并发分配者出现
```

“没有加锁”不代表这些字段运行期可以无锁修改。它表示对象尚未发布，没有竞态对手。

#### 17.12.2 最重要的发布点

| 发布点 | 发布前 | 发布后 |
|--------|--------|--------|
| `zone->initialized = 1` | zone 边界/free lists 正在构造 | hotplug/分配器可视其为已初始化 |
| page 加入 `free_area` | 启动代码独占 page 元数据 | 任意分配者可能立即取得并复用 |
| `node_set_state(N_MEMORY)` | node 可能只有占位 pgdat | node-state 遍历会访问该 node |
| 关闭 `deferred_pages` | 分配路径可能触发 grow | 所有 struct page 必须已经有效 |
| `memblock_discard()` | 仍可读取 memblock 私有结构 | 私有 backing 已可被其他对象复用 |

#### 17.12.3 失败处理

启动内存初始化的失败分两类：

- 可降级：某些调试设施、优化或可选元数据失败后关闭功能并继续；
- 不可恢复：`pgdat`、FLATMEM `mem_map`、pageblock usemap 等关键元数据分配失败会 panic。

关键对象一旦部分发布，继续运行通常比停止更危险。例如没有 pageblock flags 时，buddy 无法
维护迁移/隔离不变量；没有 memmap backing 时，任何 PFN 操作都可能越界。因此这些失败没有
普通 errno 回滚。

hotplug 不同：它是运行期操作，入口必须返回错误并把已更新的 zone/node 状态收敛回 offline，
不能因为一条内存条上线失败就直接破坏现有系统。

### 17.13 重要配置分支

| 配置 | 初始化差异 |
|------|------------|
| `CONFIG_NUMA` | 按 node 建 pgdat、PFN→nid 缓存、NUMA zonelist 和 hashdist |
| `CONFIG_FLATMEM` | 为 node 分配连续 `node_mem_map` |
| `CONFIG_SPARSEMEM` | 以 section/subsection 建立稀疏 memmap |
| `CONFIG_SPARSEMEM_VMEMMAP` | 用虚拟连续 vmemmap 映射描述符 backing |
| `CONFIG_DEFERRED_STRUCT_PAGE_INIT` | 延迟最高 zone 的高端描述符并行初始化 |
| `CONFIG_MEMORY_HOTPLUG` | 保留 `__meminit` 代码并支持 pgdat/zone 运行期重建 |
| `kernelcore=` / `movablecore=` / `movable_node` | 通过启动策略动态切分 `ZONE_MOVABLE` |
| `CONFIG_ZONE_DEVICE` | 建立 dev_pagemap、Reserved 设备页和特殊 compound 几何 |
| `CONFIG_CMA` | pageblock 可标 MIGRATE_CMA，并维护 CMA 专项页数 |
| `CONFIG_HIGHMEM` | `high_memory` 截到 HIGHMEM 边界，高端页需临时映射 |
| `CONFIG_DEBUG_MEMORY_INIT` | 启用 page flags 布局和 zonelist 诊断 |

阅读源码时必须先确认当前 `.config`。同一函数在不同配置下可能是完整实现、空 stub，甚至根本
不参与构建。

### 17.14 可运行的观察方法

#### 17.14.1 启动日志

```bash
dmesg | grep -E \
  'Zone ranges|Movable zone start|Early memory node ranges|Initmem setup|deferred pages|Memory:'
```

这些日志依次对应：

```text
Zone ranges
  架构 zone 候选边界

Movable zone start
  每 node 动态 movable 起点

Early memory node ranges
  memblock 中实际 RAM 与 nid

Initmem setup
  pgdat/node span 初始化

deferred pages initialised
  后台 struct page 完成耗时

Memory:
  最终 buddy 可用量与内核/reserved/CMA 占用
```

启用 `CONFIG_DEBUG_MEMORY_INIT` 时可通过启动参数增加验证输出：

```text
mminit_loglevel=<level>
```

#### 17.14.2 运行期视图

```bash
cat /proc/zoneinfo
cat /proc/buddyinfo
cat /proc/pagetypeinfo
cat /proc/meminfo
numactl --hardware
```

- `/proc/zoneinfo`：检查 node/zone 的 present、managed、水位与 per-CPU 统计；
- `/proc/buddyinfo`：检查各 order 当前空闲块数；
- `/proc/pagetypeinfo`：检查 pageblock 迁移类型和碎片；
- `/proc/meminfo`：检查最终总量、CMA、HugeTLB、Slab 等汇总；
- `numactl --hardware`：从用户空间核对 node 容量和距离。

不要用 `/proc/buddyinfo` 反推固件报告的全部 RAM。buddy 只显示已经 managed 且当前空闲的页，
不包含物理洞、保留页、正在使用的页或尚未 online 的内存。

### 17.15 推荐源码阅读顺序

第一次阅读不建议直接从近八千行的 `mm/page_alloc.c` 开始。更容易形成闭环的顺序是：

```text
1. include/linux/mmzone.h
   先理解 pg_data_t、zone、free_area、三个页数

2. include/linux/memblock.h
   理解 memory/reserved ranges 与迭代接口

3. arch/arm64/mm/init.c
   看架构如何给出 PFN、DMA zone、NUMA、CMA

4. mm/mm_init.c
   看通用对象构造、memmap、deferred 和阶段边界

5. mm/sparse.c + mm/sparse-vmemmap.c
   看 struct page backing 如何存在

6. mm/memblock.c
   重点看 memblock_free_all() 的所有权移交

7. mm/page_alloc.c
   从 __free_pages_core()、free_one_page()、rmqueue()、
   get_page_from_freelist() 进入运行期 buddy

8. mm/memory_hotplug.c
   用 hot-add/online/offline 验证同一组不变量

9. mm/page_ext.c + mm/vmstat.c
   补充扩展元数据与统计发布
```

读完后应能回答：

1. 为什么 `present_pages` 不能直接当成 `managed_pages`？
2. 为什么先初始化 `struct page`，再把页放入 buddy？
3. `memblock_free_all()` 和 `memblock_discard()` 分别释放什么？
4. 为什么 deferred init 只能从最高 zone 的高端开始？
5. `ZONE_MOVABLE` 为什么是动态边界，而不是架构固定地址段？
6. 热添加页面为什么先是 `PageOffline`，之后才增加 managed 计数？
7. 哪个时刻之后早期代码再访问 memblock 私有数据会成为生命周期错误？

### 17.16 回到开头问题：一页 RAM 的状态变化

一页普通 RAM 从固件描述到可分配页，可以压缩成：

```text
固件报告
  物理地址存在，但通用 MM 尚不知道如何管理

memblock.memory
  早期分配器知道该范围是 RAM

减去 memblock.reserved
  判断它是否有资格在启动时移交

sparse/vmemmap backing
  pfn_to_page(pfn) 已有合法存储位置

__init_single_page()
  struct page 的 node/zone/refcount/mapcount 等基础状态有效

pageblock 初始化
  迁移类型和反碎片属性有效

memblock_free_pages()
  选择实际可释放的连续块

__free_pages_core()
  refcount → 0
  managed_pages 增加
  加入 buddy free_area

alloc_pages()
  该页第一次可能被普通运行期分配者取得
```

这一实现的主要权衡是：

| 方案 | 收益 | 代价或退化条件 |
|------|------|----------------|
| memblock 早期区间分配 | 不依赖 `struct page` 和 buddy，可打破初始化循环 | 查询/插入不适合运行期高频使用，生命周期必须及时结束 |
| node/zone 分层 | 满足 DMA、NUMA locality 和回收边界 | zonelist fallback、计数和热插拔状态更复杂 |
| SPARSEMEM/vmemmap | 稀疏地址空间不必为所有洞分配完整描述符 | 需要 section、页表和 vmemmap backing 元数据 |
| pageblock 迁移分类 | 降低不可移动页造成的长期外部碎片 | 类型只是启发式，pin 和启动保留仍可能破坏可迁移性 |
| deferred struct page | 大内存机器更快进入可用启动阶段，并可并行初始化 | 分配热路径暂时多一个 grow 分支，需要严格区分可访问 PFN |
| 关键元数据失败即 panic | 避免带着损坏的 PFN/zone 不变量继续运行 | 启动失败不可用普通 errno 回滚 |

最重要的结论是：

> 物理地址存在、`struct page` 存在、page 属于某个 zone，以及 page 已进入
> buddy，是四个不同状态。源码中的初始化顺序就是在安全地跨越这些状态。

启动日志中的 “Memory: available/total” 已扣除多类保留和内核占用，并不等于固件报告的全部
DRAM；应结合 EFI/e820/DT、memblock、reserved-memory、CMA、HugeTLB 和 deferred 状态分析。

---

## 18. 观测、调试与常见误区

### 18.1 用户空间观测

```bash
# 全局容量、回收、缺页、脏页与 slab
cat /proc/meminfo
cat /proc/vmstat
cat /proc/zoneinfo
cat /proc/buddyinfo
cat /proc/pagetypeinfo
slabtop

# 进程地址空间与实际驻留
cat /proc/<pid>/maps
cat /proc/<pid>/smaps_rollup
cat /proc/<pid>/status
pmap -x <pid>

# NUMA、cgroup 与压力
numastat -p <pid>
cat /sys/fs/cgroup/<group>/memory.stat
cat /sys/fs/cgroup/<group>/memory.events
cat /proc/pressure/memory
```

`MemFree` 很低不一定有问题：Linux 主动用空闲 RAM 做 page cache。更有意义的是
`MemAvailable`、reclaim/scan、major fault、swap I/O、workingset refault、PSI 和应用延迟。

### 18.2 内核调试工具

- tracepoints/perf：page fault、kmem、compaction、vmscan、writeback、OOM；
- eBPF/bpftrace：按进程、cgroup、调用栈统计分配和回收延迟；
- page owner：追踪物理页分配栈，成本较高；
- KASAN/KFENCE：越界和 use-after-free；
- KMSAN：未初始化内存使用；
- kmemleak：扫描可能的内核内存泄漏；
- debug_pagealloc、page poisoning：增强释放后访问检测；
- crash/drgn：转储后分析 mm、VMA、页表、folio、zone 和 slab。

### 18.3 常见误区

**误区一：虚拟内存就是 swap。** 虚拟内存是地址抽象；swap 只是匿名页的一种后备存储。

**误区二：申请 10 GiB 就立即占用 10 GiB RAM。** demand paging 下通常在触碰时才分配，
但 VMA、页表、commit 和 memcg 记账仍可能产生即时成本。

**误区三：RSS 可以准确相加得到物理占用。** 共享库、共享内存和共享 page cache 会在多个
进程 RSS 中出现；PSS 按映射者分摊，更适合归因但也是采样时刻视图。

**误区四：缓存占内存就是泄漏。** 可回收 page cache 和 slab cache 是性能设计；要结合是否
可回收、是否持续增长、shrinker 效率和压力指标判断。

**误区五：`vmalloc` 得到的内存不能连续访问。** 它在内核虚拟地址上连续，只是物理页可离散。

**误区六：有足够 free pages，高阶分配就一定成功。** 外部碎片可能导致没有连续 `2^order` 块。

**误区七：minor fault 一定便宜。** 它只表示无存储 I/O，仍可能涉及分配、清零、COW、页表锁
和直接回收。

**误区八：OOM 只看哪个进程 RSS 最大。** 选择还受 oom_score_adj、可释放内存、共享 mm、
memcg 范围和任务状态等影响。

---

## 19. 关键函数速查

| 模块 | 函数 | 位置 | 作用 |
|------|------|------|------|
| mmap | `do_mmap()` | `mm/mmap.c` | 创建映射的通用核心 |
| munmap | `do_munmap()` / `do_vmi_munmap()` | `mm/mmap.c`、`mm/vma.c` | 拆分 VMA 并撤销映射 |
| VMA 查找 | `find_vma()` | `include/linux/mm.h` 等 | 在 Maple Tree 中查找地址区间 |
| fault 总入口 | `handle_mm_fault()` | `mm/memory.c` | 通用缺页处理 |
| 匿名 fault | `do_anonymous_page()` | `mm/memory.c` | 首次建立匿名映射 |
| COW | `do_wp_page()` | `mm/memory.c` | 处理私有写保护 fault |
| swap-in | `do_swap_page()` | `mm/memory.c` | 从 swap entry 恢复映射 |
| 文件 fault | `filemap_fault()` | `mm/filemap.c` | 从 page cache 满足文件映射 fault |
| MM 早期入口 | `mm_core_init_early()` | `mm/mm_init.c` | 巨页/CMA 预留并建立 node/zone/buddy 骨架 |
| zone 初始化 | `free_area_init()` | `mm/mm_init.c` | 计算 zone、初始化 pgdat 和 struct page |
| memmap 初始化 | `memmap_init_range()` | `mm/mm_init.c` | 建立 PFN 范围的 page 与 pageblock 基态 |
| sparse 初始化 | `sparse_init()` | `mm/sparse.c` | 为 present sections 建立 memmap backing |
| memblock 移交 | `memblock_free_all()` | `mm/memblock.c` | 把未保留物理页移交给 buddy |
| deferred 收尾 | `page_alloc_init_late()` | `mm/mm_init.c` | 汇合 deferred page worker 并销毁早期状态 |
| 热添加 node | `hotadd_init_pgdat()` | `mm/memory_hotplug.c` | 为 memoryless/offline node 重建 pgdat 与 zone |
| 页分配 | `__alloc_pages_noprof()` | `mm/page_alloc.c` | 伙伴系统分配总入口 |
| buddy 慢路 | `__alloc_pages_slowpath()` | `mm/page_alloc.c` | 回收、压缩、重试与 OOM |
| 页释放 | `__free_pages()` | `mm/page_alloc.c` | 降引用并归还 buddy |
| 回收入口 | `try_to_free_pages()` | `mm/vmscan.c` | 直接回收入口 |
| node 回收 | `shrink_node()` | `mm/vmscan.c` | 扫描 lruvec 和 slab |
| compaction | `compact_zone()` | `mm/compaction.c` | 迁移页并聚合连续空闲空间 |
| 回写节流 | `balance_dirty_pages()` | `mm/page-writeback.c` | 控制脏页产生速度 |
| OOM | `out_of_memory()` | `mm/oom_kill.c` | 选择并终止 victim |
| VMA 复制 | `dup_mmap()` | `mm/mmap.c` | fork 时复制地址空间元数据和页表 |
| mm 退出 | `exit_mmap()` | `mm/mmap.c` | 解除全部用户映射并释放资源 |
| vmalloc | `__vmalloc_node_range()` | `mm/vmalloc.c` | 建立虚拟连续映射 |

### 一次匿名内存从申请到回收

```
mmap(MAP_ANONYMOUS)
  → 创建 VMA，不一定分配物理页
  → CPU 首次写入
  → page fault
  → handle_mm_fault
  → do_anonymous_page
  → alloc_pages/folio
  → 建 PTE、加入 anon rmap/LRU、memcg charge
  → 正常读写
  → 内存压力下扫描
      ├── 仍热：保留/提升代际
      └── 冷：unmap → 写 swap/zswap → 释放物理页
  → 再次访问：do_swap_page 恢复
  → munmap/进程退出：清 PTE、TLB invalidation、减引用
  → 最后引用归零：folio 回到 buddy
```

理解 Linux 内存管理可以抓住五个不变量：

1. **VMA 描述“允许怎样访问”，页表描述“当前实际映射”；**
2. **虚拟地址、物理页和后备存储彼此独立，按 fault 动态绑定；**
3. **page/folio 是内容载体，rmap、引用和 mapcount 决定其可回收性；**
4. **快速分配依赖缓存，低水位后由回收、回写、交换和压缩共同兜底；**
5. **撤销映射必须与并发 fault、硬件 TLB、DMA pin 和对象生命周期同步。**
