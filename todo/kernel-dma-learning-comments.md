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
- [x] `kernel/dma/swiotlb.c`

## 当前文件

- [x] `kernel/dma/swiotlb.c`
  - 文件职责与生命周期：启动参数和架构策略确定默认容量/物理上限；启动期 memblock 或晚期页
    分配器建立默认 pool，动态配置可由 workqueue 增加常驻 pool，并在同步分配失败时为单次映射
    创建设备瞬态 pool；map 按 area 加锁搜索连续 slot，登记原物理地址并执行 bounce copy；
    sync/unmap 按 DMA 方向复制，普通 pool 合并并归还 slot，瞬态 pool 从 RCU 链表摘除后延迟释放；
    restricted-dma-pool 支线从共享预留区直接分配页，设备解绑只恢复默认关联。
  - 并发与所有权：`area->lock` 保护区内游标、使用量和 slot 空闲/映射元数据；`mem->lock` 与
    `dev->dma_io_tlb_lock` 分别保护公共和设备瞬态 pool 发布/摘除；RCU 保证查池期间对象存活；
    `swiotlb_find_slots()` 的 `smp_mb()` 与 `swiotlb_find_pool()` 的 `smp_rmb()` 配对，保证跨 CPU
    发布 bounce 地址前已更新使用标志并观察到 pool 链表；原缓冲区由 DMA mapping 调用者维持，
    slot/pool 所有权由 map 取得并在 unmap 或 RCU callback 中释放。
  - 第 17 章验收：已按完整修改后文件复读函数、实体、英文注释、成功/失败/配置路径和并发协议；
    复杂函数抽查为 `swiotlb_init_late()`、动态 `swiotlb_find_slots()`、
    `swiotlb_tbl_map_single()`/`swiotlb_release_slots()`，均通过初学者复述与开发者推理测试。
  - 关联读取：`include/linux/swiotlb.h` 的 `io_tlb_pool`、`io_tlb_mem`、`swiotlb_find_pool()` 和
    map/unmap/sync inline 契约（现有学习注释缺失，建议后续按完整头文件任务补注）；
    `kernel/dma/direct.c` 的 `dma_direct_need_sync()`（现有学习注释充分），用于确认落在 SWIOTLB
    pool 中即使设备一致也必须同步 bounce 数据。
  - 修改安全：`kernel/dma/swiotlb.c` 为 850 行注释/空白新增、0 行删除、零新增非注释代码行；
    `git diff --check` 通过；checkpatch 为 0 errors、0 checks，剩余为中文 UTF-8 计宽产生的行宽 warnings；
    当前工作树无 `.config`，未执行目标对象编译。

## 目录状态

- [x] `kernel/dma` 清单中的 15 个文件均已完成第 17 章单文件闭环，目录学习注释任务封账。
