# SPDX-License-Identifier: GPL-2.0

# KFENCE 依赖编译器 context analysis 检查调用上下文约束；核心与报告器始终随
# CONFIG_KFENCE 进入本目录后链接，采样分配和守护页检测在 core.o 中协作。
CONTEXT_ANALYSIS := y

obj-y := core.o report.o

# 测试保留 frame pointer 并禁止 sibling-call 优化，使故障报告中的分配/释放栈
# 稳定可断言；只有 KFENCE KUnit 配置开启时才把测试对象链接进内核。
CFLAGS_kfence_test.o := -fno-omit-frame-pointer -fno-optimize-sibling-calls
obj-$(CONFIG_KFENCE_KUNIT_TEST) += kfence_test.o
