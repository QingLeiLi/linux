# mm 学习注释进度

规则：单文件按 `doc/linux-kernel-source-learning-methodology.md` 第 17 章闭环；只加注释；`[~]` 未闭环时不开始下一文件。恢复任务先读本文件和 `todo/mm-learning-comments-baseline.md`，目标未变更时复用探查结论。

## 筛选

- 2026-08-27：扫描 190 个 `.c/.h`；密度门禁通过 0，未通过 190。
- 暂不重复：已有中文学习注释的 18 个文件（后续仅复审，不作为首轮目标）。
  `backing-dev.c`、`early_ioremap.c`、`filemap.c`、`hugetlb.c`、`hugetlb_cma.c`、`init-mm.c`、
  `memcontrol.c`、`mempolicy.c`、`mm_init.c`、`rmap.c`、`slab_common.c`、`slub.c`、`vmalloc.c`、
  `vmstat.c`、`kasan/hw_tags.c`、`kasan/sw_tags.c`、`kfence/core.c`、`kfence/report.c`。

## 进度

- [~] `mm/gup_test.h`

## 固定顺序

1. 闭环 `mm/gup_test.h`。
2. 构建入口：`mm/Makefile` → `mm/Kconfig` → `mm/Kconfig.debug` → `damon/{Makefile,Kconfig}` → `kasan/Makefile` → `kfence/Makefile` → `kmsan/Makefile`。
3. 按基线表序号 2 起处理零中文 `.c/.h`；最后复审标记 `r` 的 18 个文件。
4. 排除 `.DS_Store`、两份 `.kunitconfig`；Rust 测试不套用 C/H 方法论，另行定标。

## 已完成

- [x] `mm/damon/ops-common.h`：13/111/8.538/2（code/中文/density/max_gap）；仅新增 152 行注释。
- [x] `mm/damon/modules-common.c`：24/38/1.583/6；仅新增 59 行注释。

## 待决

- [-] `mm/damon/modules-common.h`：语义补注完成；统计宏为不可拆的 11 行续行，门禁 max_gap=11，未改宏凑数。

## 已探查索引

- 基线版本：HEAD `3d4371ed2b62`；主方法论 1474 行已完整读取；密度命令固定为 `--min-density 0.20 --max-code-gap 10`。
- `ops-common.h`：已读完整实现 `ops-common.c`，调用点 `paddr.c`、`vaddr.c`、`core.c`，以及 `include/linux/{damon,migrate}.h`；已核对 folio 引用、young/idle、过滤、评分和迁移链表契约。
- `modules-common.c/.h`：已读 `core.c` 的 ops/context/target 构造销毁，以及 `reclaim.c`、`lru_sort.c` 的全部宏调用和构造调用；已核对 attrs/quota/watermarks/stat 的单位与权限。
- `gup_test.h`：已读 `gup_test.c:1-375`；已定位 9 个 ioctl、两组 flags、`gup_test`/`pin_longterm_test` 字段，以及 get/pin、fast/basic、长期 pin、dump、read/stop 的输入输出和释放配对；头文件尚未修改。
