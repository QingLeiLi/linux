# mm 学习注释进度

规则：单文件按 `doc/linux-kernel-source-learning-methodology.md` 第 17 章闭环；只加注释；`[~]` 未闭环时不开始下一文件。恢复任务先读本文件和 `todo/mm-learning-comments-baseline.md`，目标未变更时复用探查结论。

## 筛选

- 2026-08-27：扫描 190 个 `.c/.h`；密度门禁通过 0，未通过 190。
- 暂不重复：已有中文学习注释的 18 个文件（后续仅复审，不作为首轮目标）。
  `backing-dev.c`、`early_ioremap.c`、`filemap.c`、`hugetlb.c`、`hugetlb_cma.c`、`init-mm.c`、
  `memcontrol.c`、`mempolicy.c`、`mm_init.c`、`rmap.c`、`slab_common.c`、`slub.c`、`vmalloc.c`、
  `vmstat.c`、`kasan/hw_tags.c`、`kasan/sw_tags.c`、`kfence/core.c`、`kfence/report.c`。

## 进度

- [~] 当前文件：`mm/debug_vm_pgtable.c`

## 固定顺序

1. 闭环 `mm/gup_test.h`。
2. 构建入口：`mm/Makefile` → `mm/Kconfig` → `mm/Kconfig.debug` → `damon/{Makefile,Kconfig}` → `kasan/Makefile` → `kfence/Makefile` → `kmsan/Makefile`。
3. 按基线表序号 2 起处理零中文 `.c/.h`；最后复审标记 `r` 的 18 个文件。
4. 排除 `.DS_Store`、两份 `.kunitconfig`；Rust 测试不套用 C/H 方法论，另行定标。
