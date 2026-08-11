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
