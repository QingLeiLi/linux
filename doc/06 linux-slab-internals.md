# Linux SLUB 分配器内部机制

> 基于 Linux 6.x SLUB 实现（`mm/slub.c`）

---

## 目录

1. [为什么需要 slab 分配器](#1-为什么需要-slab-分配器)
2. [三层体系结构总览](#2-三层体系结构总览)
3. [核心数据结构](#3-核心数据结构)
4. [Slab 页内部布局](#4-slab-页内部布局)
5. [分配路径：从调用到返回指针](#5-分配路径从调用到返回指针)
6. [释放路径：从指针到归还](#6-释放路径从指针到归还)
7. [Sheaf 机制详解](#7-sheaf-机制详解)
8. [kmem_cache 创建流程](#8-kmem_cache-创建流程)
9. [NUMA 支持](#9-numa-支持)
10. [内存回收](#10-内存回收)
11. [调试与安全机制](#11-调试与安全机制)
12. [关键函数速查表](#12-关键函数速查表)

---

## 1. 为什么需要 slab 分配器

内核频繁创建和销毁固定大小的对象（`task_struct`、`inode`、`dentry`、网络 sk_buff 等）。如果每次都向伙伴系统（buddy allocator）申请和归还页，有两个核心问题：

**问题一：碎片化**
伙伴系统最小分配单位是 4KB 页。一个 `task_struct` 约 7KB，用一页不够、用两页浪费近一半。大量小对象会导致严重的内部碎片。

**问题二：性能**
伙伴系统每次分配都需要操作全局链表，加锁开销高。频繁创建/销毁对象时，每次都走伙伴系统代价极大。

Slab 分配器的解法：
- **批量申请**：一次向伙伴系统申请若干页，切成等大槽位，多次分配共用这些页。
- **对象复用**：释放的对象不还给伙伴系统，留在 slab 内备用，下次分配直接取，跳过初始化。
- **per-CPU 缓存**：每个 CPU 持有一批预取对象，分配/释放无需跨 CPU 加锁。

Linux 历史上有三个实现：SLAB（原始版）、SLOB（嵌入式精简版）、SLUB（现代默认版，2.6.22 起）。本文档描述 SLUB。

---

## 2. 三层体系结构总览

SLUB 分三层管理内存，从最快到最慢：

```
┌─────────────────────────────────────────────────────────────────┐
│  Layer 1：per-CPU sheaf（束）                                    │
│  每 CPU 一组对象指针数组，无需跨 CPU 锁，分配 = 数组 pop       │
│  struct slub_percpu_sheaves { main, spare, rcu_free }           │
└────────────────────────────┬────────────────────────────────────┘
                             │ sheaf 空/满时
┌────────────────────────────▼────────────────────────────────────┐
│  Layer 2：per-node barn + partial list                           │
│  每 NUMA 节点一个 barn（sheaf 中转站）                           │
│  每 NUMA 节点一个 partial list（部分空闲的 slab 页链表）        │
│  barn 有库存时：换 sheaf；否则从 partial list 批量取对象        │
└────────────────────────────┬────────────────────────────────────┘
                             │ partial list 也空时
┌────────────────────────────▼────────────────────────────────────┐
│  Layer 3：伙伴系统（buddy allocator）                            │
│  申请新的 2^order 页，切割成对象槽位，填充 partial list         │
└─────────────────────────────────────────────────────────────────┘
```

**核心设计原则**：快速路径（Layer 1）不加全局锁，只用 per-CPU 的 `local_trylock`；失败即退出到慢速路径，不自旋等待。

---

## 3. 核心数据结构

### 3.1 `struct kmem_cache`：缓存描述符

> `mm/slab.h:222`

每种固定大小的对象类型对应一个 `kmem_cache`（如 `task_struct_cachep`、`filp_cachep`）。

```
struct kmem_cache {
    cpu_sheaves    → __percpu *slub_percpu_sheaves  每 CPU 一组 sheaf（快速路径入口）
    flags          → slab 标志（SLAB_TYPESAFE_BY_RCU、SLAB_HWCACHE_ALIGN 等）
    min_partial    → per-node partial list 最少保留的 slab 数
    size           → 对象在 slab 内占用的字节数（含对齐、元数据）
    object_size    → 用户请求的原始大小（sizeof(struct foo)）
    offset         → freepointer 在对象内的字节偏移
    sheaf_capacity → 每个 sheaf 最多持有的对象数
    oo             → 编码了理想分配 order 和每 slab 对象数
    min            → 内存不足时的最小 order 配置
    inuse          → 对象"有效区域"末尾偏移
    align          → 对象对齐字节数
    per_node[]     → 每 NUMA 节点的 (kmem_cache_node*, node_barn*) 对
}
```

关键字段关系：
```
object_size ≤ inuse ≤ size

object_size：用户结构体大小
inuse：      含左 red zone（调试）、freepointer（在对象内时），四舍五入对齐后的大小
size：       final 步长，含所有元数据和尾部对齐，slab 内对象间距
```

`oo` 字段编码（`mm/slub.c:597`）：
```
oo = (order << OO_SHIFT) | objects_per_slab
oo_order(oo) = oo >> OO_SHIFT
oo_objects(oo) = oo & OO_MASK
```

### 3.2 `struct slab`：slab 页描述符

> `mm/slab.h:98`

`struct slab` 与 `struct page` 内存布局兼容（前几字段完全重叠），slab 用的就是页描述符自身的内存，无额外开销。

```
struct slab {
    flags          → NUMA node ID + SL_partial/SL_pfmemalloc 标志
    slab_cache     → 指回所属 kmem_cache
    slab_list      → 挂入 per-node partial list 的链表节点
    freelist        → slab 内空闲对象链表头（指向第一个空闲对象）
    inuse          → 已分配对象数
    objects        → 该 slab 总对象数
    frozen         → 是否被某 CPU"冻结"（旧 SLUB，现已被 sheaf 替代）
    __page_type    → 存储 PGTY_slab 标识（与伙伴系统区分）
}
```

`freelist` 是单链表：每个空闲对象的 `offset` 处存放"下一个空闲对象的地址"（开启 `SLAB_FREELIST_HARDENED` 时经 XOR 混淆）。

### 3.3 `struct kmem_cache_node`：per-NUMA-node 管理

> `mm/slub.c:443`

```
struct kmem_cache_node {
    list_lock      → 保护 partial 链表的自旋锁
    nr_partial     → partial 链表上的 slab 页数量
    partial        → 部分空闲 slab 链表（slab.slab_list 挂入此处）
    // 调试模式还有 full 链表（满载 slab）
}
```

访问方式：`get_node(s, nid)` 返回 `s->per_node[nid].node`。

### 3.4 `struct slab_sheaf`：对象束

> `mm/slub.c:417`

```
struct slab_sheaf {
    cache          → 所属 kmem_cache
    size           → 当前持有的有效对象数（0 ~ sheaf_capacity）
    objects[]      → 柔性数组，存储对象指针
    // union：rcu_head（rcu_free 用）/ barn_list（挂入 barn 用）/ capacity（prefill 用）
}
```

sheaf 本质是**对象指针数组**，像栈一样操作：
- 分配：`objects[size - 1]`，`size--`
- 释放：`objects[size]`，`size++`

### 3.5 `struct slub_percpu_sheaves`：per-CPU sheaf 集合

> `mm/slub.c:433`

```
struct slub_percpu_sheaves {
    lock           → per-CPU 轻量锁（local_trylock）
    main           → 主 sheaf：解锁时永不为 NULL，当前服务分配/释放
    spare          → 备用 sheaf：size 为 0（空）或 capacity（满），可为 NULL
    rcu_free       → 专用于 kfree_rcu() 批量延迟释放
}
```

### 3.6 `struct node_barn`：per-node sheaf 仓库

> `mm/slub.c:409`

```
struct node_barn {
    lock           → 保护仓库链表的自旋锁
    sheaves_full   → 已满 sheaf 队列（可直接用于分配）
    sheaves_empty  → 空 sheaf 队列（可直接用于释放）
    nr_full / nr_empty
}
```

barn 是 Layer 1（per-CPU）和 Layer 2（per-node partial list）之间的缓冲层，减少直接操作 slab freelist 的频率。

---

## 4. Slab 页内部布局

### 4.1 对象排列

一个 slab（连续 `2^order` 个页）内，对象从偏移 0 开始按 `size` 步长排列：

```
slab 起始地址（page-aligned）
│
├─ [object 0: s->size bytes]
├─ [object 1: s->size bytes]
├─ [object 2: s->size bytes]
│   ...
└─ [object N-1: s->size bytes]
   [可能有尾部 padding]

N = oo_objects(s->oo) = (PAGE_SIZE << order) / s->size
```

### 4.2 单个对象的内部结构

对象内部布局由 `calculate_sizes()`（`mm/slub.c:7843`）确定，根据开启的特性不同而变化。

**普通模式**（无调试，无 RCU，无 ctor）：
```
0                   object_size         inuse              size
│←── 用户数据区 ───→│                    │←─ freepointer ─→│
                    │←────────── inuse ─────────────────────│
                    │                   ↑
                    │               offset（≈ object_size/2）
                    │                   freepointer 藏在对象中间
                    │←──────────────── size ────────────────│
```

freepointer 放在**对象中间**（`offset = ALIGN_DOWN(object_size / 2, sizeof(void*))`）是刻意设计：越界写通常从对象头部开始，放在中间可以减少被越界覆盖的概率。

**调试模式**（`SLAB_POISON`、`SLAB_RED_ZONE`）：
```
│← red_left_pad →│←── 用户数据 ──→│← red zone →│← freepointer →│← track →│ align │
                                                  ↑ offset = inuse（freepointer 在对象外）
```

调试模式下 freepointer 强制放在 `inuse` 处（对象末尾之外），保证 poison pattern 覆盖整个用户区不被 freepointer 打断。

**Freepointer 读写**（`mm/slub.c:543`）：
```c
// 读取：object + offset 处的指针，经 XOR 解码（SLAB_FREELIST_HARDENED）
get_freepointer(s, object) → freelist_dereference(s, object + s->offset)

// 写入：
set_freepointer(s, object, fp) → *(object + s->offset) = freelist_ptr_encode(s, fp, ...)
```

### 4.3 空闲链表的组织

slab 页内的空闲对象通过 freepointer 串成单向链表：

```
slab->freelist ──→ [obj_A freeptr]──→ [obj_C freeptr]──→ [obj_F freeptr]──→ NULL

已分配的对象：freepointer 处存放 POISON 值（调试）或残留旧指针（非调试）
```

分配：取 `slab->freelist` 指向的对象，freelist 指针前移一步。
释放：将对象的 freepointer 设为当前 `slab->freelist`，再将 freelist 指向该对象（头插法）。

`freelist` 指针与 `inuse`/`objects` 计数合并在 `struct freelist_counters`（`mm/slab.h:65`）中，可通过 `cmpxchg` 原子同时更新，避免 ABA 问题。

---

## 5. 分配路径：从调用到返回指针

### 5.1 完整调用链

```
kmem_cache_alloc(s, flags)                     [slab.h，宏]
  └─ alloc_hooks(kmem_cache_alloc_noprof(...)) [插入 alloc_tag 追踪]
       └─ kmem_cache_alloc_noprof(s, flags)    [slub.c:4950]
            └─ slab_alloc_node(s, flags, NUMA_NO_NODE, &ac)  [slub.c:4920]
```

`kmem_cache_alloc_node` 与之相同，只是多传一个 `node` 参数。

### 5.2 `slab_alloc_node`：分配入口

> `mm/slub.c:4920`，`__fastpath_inline` 标记

```
slab_alloc_node(s, gfpflags, node, ac):
  1. slab_pre_alloc_hook()    → 检查 should_failslab、might_sleep_if 等
  2. kfence_alloc()           → KFENCE 隔离分配（调试，概率约 1/512，通常跳过）
  3. alloc_from_pcs()         → 【快速路径】per-CPU sheaf 弹出对象
  4. __slab_alloc_node()      → 【慢速路径】快速路径失败时调用
  5. maybe_wipe_obj_freeptr() → 调试时清除 freepointer（防止信息泄漏）
  6. slab_post_alloc_hook()   → KASAN 标记可访问、memcg 计账
```

### 5.3 快速路径：`alloc_from_pcs`

> `mm/slub.c:4756`

```
alloc_from_pcs(s, gfp, alloc_flags, node):

  [NUMA 前置检查]
  if node_requested && node != numa_mem_id():
      return NULL  // 请求跨节点，退出快速路径

  [获取 per-CPU 锁]
  if !local_trylock(&s->cpu_sheaves->lock):
      return NULL  // 锁竞争，退出快速路径（不等待）

  pcs = this_cpu_ptr(s->cpu_sheaves)

  [main sheaf 空了，换一个]
  if pcs->main->size == 0:
      pcs = __pcs_replace_empty_main(s, pcs, gfp, alloc_flags)
      if !pcs: return NULL

  [取对象：数组 pop]
  object = pcs->main->objects[pcs->main->size - 1]
  pcs->main->size--

  [NUMA 验证：防止 CPU 迁移导致对象 node 错位]
  if node_requested && page_to_nid(virt_to_page(object)) != node:
      pcs->main->size++  // 放回
      local_unlock(...)
      return NULL

  local_unlock(...)
  return object  // stat: ALLOC_FASTPATH
```

**关键性质**：整个操作在 `local_trylock` 内，失败立即退出，绝不自旋。

### 5.4 Main sheaf 耗尽时：`__pcs_replace_empty_main`

> `mm/slub.c:4643`

```
按优先级尝试：

1. spare->size > 0？
   → swap(main, spare)  无需访问 barn，最快

2. barn_replace_empty_sheaf(barn, main)？
   → 将空 main 存入 barn.sheaves_empty
   → 取 barn.sheaves_full 中的一个满 sheaf 作为新 main
   → barn 需加 spinlock，但不走 slab freelist

3. barn_get_empty_sheaf() + refill_sheaf()？
   → 从 barn 取空 sheaf，调用 refill_sheaf 从 partial list / 伙伴系统填满
   → 最慢，可能触发页分配

4. alloc_empty_sheaf() + refill_sheaf()？
   → barn 也没有空 sheaf，先 kmalloc 一个新 sheaf 结构，再填满
```

### 5.5 慢速路径：`___slab_alloc`

> `mm/slub.c:4456`（核心逻辑），由 `__slab_alloc_node`（`slub.c:4529`）调用

```
___slab_alloc(s, gfpflags, node, ac):

new_objects:
  1. 构造 trynode_flags（NUMA 节点约束标志）

  2. get_from_partial(s, node, trynode_flags, ac)
     ├─ get_from_partial_node(s, get_node(s, local_node), ...)
     │    持 list_lock，从 n->partial 链表头取一个 slab
     │    CAS 原子减少 slab->freelist + inuse
     │    若 slab 耗尽则 remove_partial
     └─ get_from_any_partial(...)  // 本节点无 partial 时遍历所有节点

  3. 若仍失败：new_slab(s, trynode_flags, alloc_flags, node)
     └─ allocate_slab() → alloc_slab_page() → 向伙伴系统申请 2^order 页

  4. alloc_from_new_slab(s, slab, &object, 1, ...)
     从新 slab 的 freelist 取出对象，剩余对象留在 slab 内
     将 slab 加入 per-node partial list（add_partial）

  5. 若 node 约束失败：清除 try_thisnode 标志，goto new_objects 重试
```

### 5.6 从伙伴系统申请新 slab

> `allocate_slab`：`mm/slub.c:3387`

```
allocate_slab(s, flags, node):
  1. 先用 s->oo（理想 order）尝试 alloc_slab_page()
     附加 __GFP_NOWARN | __GFP_NORETRY | __GFP_NOMEMALLOC（快速失败）
  2. 失败则用 s->min（最小 order）重试（ORDER_FALLBACK 统计）
  3. 初始化 slab 描述符：
       slab->objects = oo_objects(oo)
       slab->inuse = 0
       slab->slab_cache = s
  4. shuffle_freelist()（SLAB_FREELIST_RANDOM 时随机化链表顺序）
  5. account_slab()（memcg 计账）
```

---

## 6. 释放路径：从指针到归还

### 6.1 完整调用链

```
kmem_cache_free(s, x)            [slub.c:6512]
  ├─ trace_kmem_cache_free()
  └─ slab_free(s, slab, x, _RET_IP_)   [slub.c:6399]
       ├─ alloc_tagging_slab_free_hook()  → 更新 alloc_tag 统计
       ├─ slab_free_hook()               → KASAN 标记不可访问、poison 填充
       ├─ 快速路径：can_free_to_pcs() && free_to_pcs()   [slub.c:6408]
       └─ 慢速路径：__slab_free()                        [slub.c:5664]
```

### 6.2 快速释放路径：`free_to_pcs`

> `mm/slub.c:5958`

前置检查 `can_free_to_pcs`（`slub.c:6154`）：
- 对象的物理页必须属于本 CPU 的 NUMA 内存节点（或该节点是 memoryless 节点）。
- 不能是 pfmemalloc 对象（来自紧急内存储备）。

```
free_to_pcs(s, object, allow_spin):
  local_trylock(&s->cpu_sheaves->lock)  // 失败则返回 false，走慢路径

  pcs = this_cpu_ptr(s->cpu_sheaves)

  [main sheaf 满了，换一个]
  if pcs->main->size == sheaf_capacity:
      __pcs_replace_full_main(s, pcs, allow_spin)
        优先级：
        1. spare->size == 0 → swap(main, spare)
        2. barn_replace_full_sheaf(barn, main) → 存入 barn，换回空 sheaf
        3. kmalloc 新空 sheaf，swap

  [放入对象：数组 push]
  pcs->main->objects[pcs->main->size++] = object

  local_unlock(...)
  return true
```

### 6.3 慢速释放路径：`__slab_free`

> `mm/slub.c:5664`

慢速路径处理：对象归还到 slab 的 freelist，并决定 slab 是否需要加入/移出 partial list 或归还给伙伴系统。

**核心：CAS 无锁更新 slab 元数据**

```
do {
    old = {slab->freelist, slab->counters}
    was_full = (old.freelist == NULL && old.inuse == old.objects)
    
    // 头插法：对象的 freepointer 指向旧链表头
    set_freepointer(s, tail, old.freelist)
    
    new.freelist = head       // 新链表头 = 刚释放的对象
    new.inuse -= cnt          // 减少已用计数
    
    if (!new.inuse || was_full):
        n = get_node(s, slab_nid(slab))
        spin_lock_irqsave(&n->list_lock)  // 预取节点锁

} while (!slab_update_freelist(s, slab, old, new))  // cmpxchg 原子更新
```

**CAS 成功后的后处理**：

```
场景 A：slab 仍有已用对象，且不是 was_full → 无需操作，返回

场景 B：was_full（slab 从满 → 部分空）：
    add_partial(n, slab, ADD_TO_TAIL)  // 加入 partial list 尾部（尽量最后被用）

场景 C：new.inuse == 0（slab 全空）：
    if n->nr_partial >= s->min_partial:
        remove_partial(n, slab)
        discard_slab(s, slab)          // 归还伙伴系统
    else:
        保留在 partial list（作为备用空 slab）
```

**slab 归还给伙伴系统的条件**：
1. slab 的所有对象都已释放（`inuse == 0`）
2. 本节点 partial list 已有足够备用（`nr_partial >= min_partial`）

`discard_slab` → `free_slab` → `__free_slab`：清除 `PG_slab` 标志，`free_frozen_pages()` 归还给伙伴系统。

若开启 `SLAB_TYPESAFE_BY_RCU`：不立即释放，改为 `call_rcu(&slab->rcu_head, rcu_free_slab)` 延迟到 RCU 宽限期后再释放，保证持有 RCU 读锁的代码可以安全访问"已释放"的对象。

---

## 7. Sheaf 机制详解

Sheaf（束）是 Linux 6.x 引入的 per-CPU 缓存层，替代了旧版 SLUB 中直接操作 slab freelist 的 per-CPU frozen slab 机制。

### 7.1 三个 Sheaf 的角色

| 成员 | 角色 | 状态约束 |
|---|---|---|
| `main` | 当前服务分配/释放的主 sheaf | 解锁时不为 NULL |
| `spare` | 备用 sheaf，快速换手 | 为 NULL，或 size=0（空），或 size=capacity（满）|
| `rcu_free` | 专用于 `kfree_rcu()` 批量延迟释放 | 可为 NULL；未满时接收对象 |

**Main sheaf 生命周期**：

```
                    分配耗尽            释放填满
                       │                  │
[满 sheaf] ──pop──→ [半满] ──pop──→ [空 sheaf]
                                          │
                           __pcs_replace_empty_main
                           (从 spare / barn / partial list 补充)

[空 sheaf] ──push──→ [半满] ──push──→ [满 sheaf]
                                          │
                           __pcs_replace_full_main
                           (到 spare / barn / partial list 腾空)
```

### 7.2 Sheaf 容量计算

> `mm/slub.c:7792`

| 对象大小 | 初始容量 |
|---|---|
| ≥ PAGE_SIZE（通常 4096B）| 4 |
| ≥ 1024B | 12 |
| ≥ 256B | 26 |
| < 256B | 60 |

最终容量再对齐到 `kmalloc` 大小桶边界，使 `sizeof(slab_sheaf) + capacity * sizeof(void*)` 正好是一个 kmalloc bucket 的大小，减少 sheaf 结构自身的内部碎片。

### 7.3 `refill_sheaf`：批量填充对象

> `mm/slub.c:2823`

```c
refill_sheaf(s, sheaf, gfp):
    to_fill = s->sheaf_capacity - sheaf->size
    filled = refill_objects(s, &sheaf->objects[sheaf->size], gfp, to_fill, to_fill)
    sheaf->size += filled
```

`refill_objects`（`slub.c:7318`）按优先级取对象：
1. 本 NUMA 节点 partial list（持 `list_lock`，批量取）
2. 任意节点 partial list
3. `new_slab()`：向伙伴系统申请新 slab，`alloc_from_new_slab` 批量提取对象，剩余留在 partial list

### 7.4 `kfree_rcu` 的 sheaf 优化

> `mm/slub.c:6053`

`kfree_rcu()` 调用路径：对象先放入 `pcs->rcu_free` sheaf，而不是立即注册 RCU 回调。当 sheaf 满后，一次 `call_rcu()` 批量释放整个 sheaf，将 N 个对象的 RCU 开销合并为一次，显著减少 RCU 回调数量。

---

## 8. `kmem_cache` 创建流程

### 8.1 顶层 API

```
kmem_cache_create(name, size, align, flags, ctor)
  └─ __kmem_cache_create_args(name, size, args, flags)   [slab_common.c:318]
       ├─ kmem_cache_sanity_check()    参数合法性检查
       ├─ __kmem_cache_alias()         尝试复用已有 cache（大小/flags 可合并时）
       ├─ calculate_alignment()        确定最终对齐字节数
       └─ create_cache()               [slab_common.c:232]
            ├─ kmem_cache_zalloc(kmem_cache, GFP_KERNEL)  分配 struct kmem_cache 本身
            └─ do_kmem_cache_create()  [slub.c:8629]
```

### 8.2 `do_kmem_cache_create`：核心初始化

> `mm/slub.c:8629`

```
1. 设置基本字段：name, object_size, flags, align, ctor

2. calculate_sizes(args, s)                    [slub.c:7843]
   └─ 确定 inuse、offset、size、oo、min、sheaf_capacity

3. s->min_partial = clamp(ilog2(s->size) / 2, MIN_PARTIAL, MAX_PARTIAL)
   （对象越大，每 slab 对象数越少，保留更多备用 slab）

4. s->cpu_sheaves = alloc_percpu(struct slub_percpu_sheaves)

5. init_cache_random_seq(s)                    SLAB_FREELIST_RANDOM：随机化 freelist 顺序

6. init_kmem_cache_nodes(s)                    [slub.c:7694]
   为每个 online NUMA node 分配 kmem_cache_node + node_barn

7. init_percpu_sheaves(s)
   为每个 CPU 分配并初始化 main sheaf（空）和 spare sheaf（空）
```

### 8.3 `calculate_sizes`：对象大小与 slab order

> `mm/slub.c:7843`

```
1. size = ALIGN(object_size, sizeof(void*))
2. 调试时增加 red zone、poison、track info 等
3. 确定 freepointer offset（s->offset）
4. size = ALIGN(size, s->align)   → s->size（最终步长）

5. calculate_order(size)          [slub.c:7536]
   min_objects ≈ 4 * (fls(nr_cpus) + 1)  （CPU 数越多，每 slab 对象数要求越多）
   从 min_order 到 slub_max_order（默认 3，即 32KB）：
     找满足"waste ≤ 1/16"的最小 order
   失败则依次放宽到 1/8、1/4、1/2

6. s->oo = oo_make(order, size)        理想配置
   s->min = oo_make(get_order(size), size)  最坏配置（内存不足时的回退）
   s->sheaf_capacity = calculate_sheaf_capacity(s, args)
```

---

## 9. NUMA 支持

### 9.1 数据结构布局

每个 `kmem_cache` 通过 `per_node[MAX_NUMNODES]` 为每个 NUMA 节点维护独立状态：

```
kmem_cache.per_node[0] → { kmem_cache_node*, node_barn* }   // Node 0
kmem_cache.per_node[1] → { kmem_cache_node*, node_barn* }   // Node 1
kmem_cache.per_node[2] → { kmem_cache_node*, node_barn* }   // Node 2
...
```

访问接口：
- `get_node(s, nid)` → `s->per_node[nid].node`（partial list）
- `get_barn_node(s, nid)` → `s->per_node[nid].barn`（sheaf 仓库）

### 9.2 分配时的 NUMA 两阶段检查

> `mm/slub.c:4784`

```
阶段 1（软件快速过滤）：
  if node_requested && node != numa_mem_id():
      return NULL  // 请求的 node 不是本 CPU 的内存节点，直接退出快速路径

阶段 2（物理验证）：
  取出对象后，再次检查：
  if page_to_nid(virt_to_page(object)) != node:
      // CPU 可能在取对象过程中迁移到其他 NUMA 节点
      // 对象实际来自其他节点，放回并返回 NULL
```

两阶段设计：第一阶段廉价（比较整数），第二阶段准确（物理验证），组合使用以最小化开销。

### 9.3 慢速路径的 NUMA 优先级

> `mm/slub.c:4481`（`___slab_alloc`）

```
1. 先用本地节点 + __GFP_THISNODE 严格约束：
   get_from_partial_node(s, get_node(s, local_node), ...)

2. 本节点无 partial 时，允许跨节点（去掉 THISNODE 标志）：
   get_from_any_partial(s, ...)   // 遍历所有节点

3. 若 node 请求失败：
   清除 try_thisnode，goto new_objects 重试

语义：尽量本地分配，不强制，__GFP_THISNODE 才强制。
```

### 9.4 NUMA 节点上/下线时的处理

- **上线**：`slab_mem_going_online_callback`（`slub.c:8372`）为新节点分配 `kmem_cache_node` + `node_barn`。
- **下线**：`slab_mem_going_offline_callback`（`slub.c:8358`）对所有 cache 执行 `flush_all` + `__kmem_cache_do_shrink`，将该节点的 slab 对象转移或释放。

---

## 10. 内存回收

### 10.1 Slab 页归还伙伴系统

在 `__slab_free` 的后处理阶段（见第 6.3 节），当两个条件同时满足时才归还：

```
条件 1：new.inuse == 0            该 slab 页的所有对象都已释放
条件 2：n->nr_partial >= s->min_partial   该节点已有足够的备用 partial slab

触发：discard_slab(s, slab)
  → free_slab(s, slab)
  → __free_slab(s, slab)           清除 PG_slab 标志
  → free_frozen_pages(page, order) 归还给伙伴系统
```

若不满足条件 2（partial list 不够充裕），空 slab 留在 partial list 中作为备用，避免下次分配又去向伙伴系统申请。

`SLAB_TYPESAFE_BY_RCU` 时，归还改为：
```
call_rcu(&slab->rcu_head, rcu_free_slab)  // 延迟到 RCU 宽限期后归还
```

### 10.2 主动缩减：`kmem_cache_shrink`

> `mm/slub.c:8352`

```
kmem_cache_shrink(s)
  └─ __kmem_cache_shrink(s)
       1. flush_all(s)：向所有 CPU 发 work，将 per-CPU sheaf 中的对象刷回 slab
       2. __kmem_cache_do_shrink(s)   [slub.c:8283]：
          ├─ 对每个 NUMA node：barn_shrink(s, barn)
          │    清空 barn 中的满/空 sheaf，将对象归还到 partial list 或伙伴系统
          └─ 对每个 kmem_cache_node：
               遍历 n->partial，分类处理：
               ├─ 全空 slab → 移到 discard 列表，最终 discard_slab
               └─ 接近满的 slab → 提升到链表头（SHRINK_PROMOTE_MAX 层级）
                  目的：让接近满的 slab 优先被分配，尽快脱离 partial list，节约内存
```

### 10.3 触发时机

| 触发来源 | 场景 |
|---|---|
| `kmem_cache_shrink()` | 用户主动调用（如 `/proc/sys/vm/drop_caches`） |
| shrinker 框架 | 系统内存压力时由 VM 调度 |
| NUMA 节点下线 | 节点 hotplug 下线时清理所有 cache |
| 模块卸载 | `kmem_cache_destroy()` 内部调用 |

---

## 11. 调试与安全机制

### 11.1 SLUB_DEBUG：Poison + Red Zone

开启 `CONFIG_SLUB_DEBUG` 后，每个对象增加额外元数据：
- **Left red zone**（`red_left_pad`）：对象左侧的填充区，写入固定 pattern，释放时检测。
- **Right red zone**：`inuse` 到 `size` 之间的空间，检测越界写。
- **Poison**：分配时填写 `POISON_FREE`（0x6b），释放时填写 `POISON_END`（0xa5）+ `POISON_FREE`；再次分配前验证 poison pattern，检测 use-after-free。
- **Track info**（`SLAB_STORE_USER`）：对象末尾存储最近两次 alloc/free 的调用栈。

### 11.2 SLAB_FREELIST_HARDENED：freepointer 混淆

> `mm/slub.c:530`

```c
// 编码（写 freepointer）
encoded = (unsigned long)ptr ^ s->random ^ (unsigned long)object_addr

// 解码（读 freepointer）
ptr = encoded ^ s->random ^ (unsigned long)object_addr
```

`s->random` 是 cache 创建时生成的随机值，`object_addr` 是存放 freepointer 的内存地址。攻击者若想伪造 freepointer 链（实现任意地址写），需要同时知道 `s->random` 和具体地址，增加利用难度。

### 11.3 SLAB_FREELIST_RANDOM：分配顺序随机化

slab 初始化时（`setup_slab_debug` 调用 `shuffle_freelist`，`slub.c:2285`），对 freelist 链表顺序进行 Fisher-Yates 随机打乱，使攻击者无法预测分配到的对象地址。

### 11.4 KFENCE：隔离采样

KFENCE（Kernel Electric Fence）对少量分配（约 1/512 概率）使用专用隔离内存页（每个对象独占一页 + guard page），可精确检测 use-after-free 和越界写，无需 ASAN 的全量 shadow 内存开销。

### 11.5 SLAB_TYPESAFE_BY_RCU

使用此 flag 创建的 cache，slab 页释放前必须经过 RCU 宽限期。语义：
- 持有 RCU 读锁期间，即使对象已调用 `kmem_cache_free()`，其内存仍然有效（但内容可能已被复用）。
- 允许 lock-free 的指针解引用模式：先 `rcu_dereference`，再持结构体内的锁验证对象仍有效（generation counter 等）。

---

## 12. 关键函数速查表

| 函数 / 结构 | 文件 | 行号 | 说明 |
|---|---|---|---|
| `struct freelist_counters` | `mm/slab.h` | 65 | freelist + inuse/objects 合并结构 |
| `struct slab` | `mm/slab.h` | 98 | slab 页描述符（复用 struct page） |
| `struct kmem_cache` | `mm/slab.h` | 222 | 缓存主控结构 |
| `struct node_barn` | `mm/slub.c` | 409 | per-node sheaf 仓库 |
| `struct slab_sheaf` | `mm/slub.c` | 417 | 对象指针数组（束） |
| `struct slub_percpu_sheaves` | `mm/slub.c` | 433 | per-CPU sheaf 集合 |
| `struct kmem_cache_node` | `mm/slub.c` | 443 | per-node partial list 管理 |
| `get_freepointer` | `mm/slub.c` | 543 | 读取对象 freepointer |
| `set_freepointer` | `mm/slub.c` | 554 | 写入对象 freepointer |
| `oo_make/order/objects` | `mm/slub.c` | 597 | order + objects 编码/解码 |
| `allocate_slab` | `mm/slub.c` | 3387 | 向伙伴系统申请新 slab 页 |
| `new_slab` | `mm/slub.c` | 3444 | 封装 allocate_slab |
| `__free_slab` | `mm/slub.c` | 3457 | 将 slab 页归还伙伴系统 |
| `discard_slab` | `mm/slub.c` | 3506 | 触发 slab 页释放 |
| `add_partial / remove_partial` | `mm/slub.c` | 3547/3561 | partial list 增删 |
| `get_from_partial_node` | `mm/slub.c` | 3826 | 从本节点 partial list 取对象 |
| `get_from_partial` | `mm/slub.c` | 3966 | NUMA 感知的 partial list 取对象 |
| `alloc_from_new_slab` | `mm/slub.c` | 4401 | 从新 slab 批量提取对象 |
| `___slab_alloc`（慢路径核心）| `mm/slub.c` | 4456 | partial list + 新 slab 分配 |
| `__slab_alloc_node`（NUMA 策略）| `mm/slub.c` | 4529 | mempolicy 处理后调 ___slab_alloc |
| `__pcs_replace_empty_main` | `mm/slub.c` | 4643 | main sheaf 耗尽时换 sheaf |
| `alloc_from_pcs`（快速路径）| `mm/slub.c` | 4756 | per-CPU sheaf pop |
| `slab_alloc_node`（inlined 入口）| `mm/slub.c` | 4920 | 分配总入口，调度快/慢路径 |
| `kmem_cache_alloc_noprof` | `mm/slub.c` | 4950 | 公开分配接口（无追踪版） |
| `kmem_cache_alloc_node_noprof` | `mm/slub.c` | 5008 | NUMA 节点指定分配 |
| `__slab_free`（慢速释放）| `mm/slub.c` | 5664 | CAS 更新 freelist + partial 管理 |
| `__pcs_replace_full_main` | `mm/slub.c` | 5835 | main sheaf 满时换 sheaf |
| `free_to_pcs`（快速释放）| `mm/slub.c` | 5958 | per-CPU sheaf push |
| `__kfree_rcu_sheaf` | `mm/slub.c` | 6053 | kfree_rcu 批量延迟释放 |
| `can_free_to_pcs` | `mm/slub.c` | 6154 | 快速释放前置 NUMA 检查 |
| `slab_free` | `mm/slub.c` | 6399 | 释放入口，调度快/慢路径 |
| `kmem_cache_free` | `mm/slub.c` | 6512 | 公开释放接口 |
| `refill_objects` | `mm/slub.c` | 7318 | 从 partial list / 伙伴系统批量取对象 |
| `calculate_order` | `mm/slub.c` | 7536 | 计算最优 slab order |
| `calculate_sheaf_capacity` | `mm/slub.c` | 7792 | 计算 sheaf 容量 |
| `calculate_sizes` | `mm/slub.c` | 7843 | 计算对象 size/inuse/offset/oo |
| `refill_sheaf` | `mm/slub.c` | 2823 | 向 sheaf 批量填充对象 |
| `barn_replace_empty_sheaf` | `mm/slub.c` | 3177 | 从 barn 换入满 sheaf |
| `barn_replace_full_sheaf` | `mm/slub.c` | 3210 | 向 barn 存入满 sheaf |
| `__kmem_cache_do_shrink` | `mm/slub.c` | 8283 | 主动回收 partial list 中空 slab |
| `__kmem_cache_shrink` | `mm/slub.c` | 8352 | flush all + shrink |
| `do_kmem_cache_create` | `mm/slub.c` | 8629 | cache 初始化核心 |
| `__kmem_cache_create_args` | `mm/slab_common.c` | 318 | cache 创建入口 |
