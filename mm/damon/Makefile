# SPDX-License-Identifier: GPL-2.0

# DAMON 的 Kbuild 组合图：core.o 始终是 CONFIG_DAMON 选中本目录后的框架核心；
# 地址空间操作集、用户接口和内核策略模块再由各自 Kconfig 符号按需叠加。
obj-y				:= core.o
# 虚拟地址与物理地址操作集共享 ops-common.o。Kbuild 会去重同一目录目标，
# 两个配置同时开启时共享辅助实现仍只链接一次，再分别加入 vaddr.o/paddr.o。
obj-$(CONFIG_DAMON_VADDR)	+= ops-common.o vaddr.o
obj-$(CONFIG_DAMON_PADDR)	+= ops-common.o paddr.o
# SYSFS 前端拆成公共对象、scheme 编解码和顶层控制器，配置关闭时不暴露 ABI。
obj-$(CONFIG_DAMON_SYSFS)	+= sysfs-common.o sysfs-schemes.o sysfs.o
# 三个内核模块复用参数解析/context-target 构造辅助层；modules-common.o 与
# 上面的 ops-common.o 一样可被多个选择项列出而不会产生重复链接实体。
obj-$(CONFIG_DAMON_RECLAIM)	+= modules-common.o reclaim.o
obj-$(CONFIG_DAMON_LRU_SORT)	+= modules-common.o lru_sort.o
obj-$(CONFIG_DAMON_STAT)	+= modules-common.o stat.o
