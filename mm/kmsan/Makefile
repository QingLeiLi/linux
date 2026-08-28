# SPDX-License-Identifier: GPL-2.0
#
# Makefile for KernelMemorySanitizer (KMSAN).
#
# KernelMemorySanitizer 构建入口：core/instrumentation/hooks 管理来源与传播，
# shadow 保存初始化状态，report 生成诊断；它们作为一个不可自我插桩的运行时。
#
obj-y := core.o instrumentation.o init.o hooks.o report.o shadow.o

# KMSAN 运行时若被 KMSAN、KCOV 或 UBSAN 再插桩，会在读取自身 shadow/元数据时
# 递归进入检测器；目录级关闭三类插桩，KUnit 测试随后单独重新开启 KMSAN。
KMSAN_SANITIZE := n
KCOV_INSTRUMENT := n
UBSAN_SANITIZE := n

# Disable instrumentation of KMSAN runtime with other tools.
# 运行时关闭 stack protector、可选的 conserve-stack 以及分支 profiling，避免
# 隐式 helper 在 KMSAN 初始化前或报告路径中再次访问未初始化元数据。
CC_FLAGS_KMSAN_RUNTIME := -fno-stack-protector
CC_FLAGS_KMSAN_RUNTIME += $(call cc-option,-fno-conserve-stack)
CC_FLAGS_KMSAN_RUNTIME += -DDISABLE_BRANCH_PROFILING

# Disable ftrace to avoid recursion.
# ftrace 回调也可能访问被 KMSAN 追踪的内存；逐对象移除 ftrace，保护所有核心、
# hook、instrumentation、shadow 和报告入口免于递归。
CFLAGS_REMOVE_core.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_hooks.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_init.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_instrumentation.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_report.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_shadow.o = $(CC_FLAGS_FTRACE)

CFLAGS_core.o := $(CC_FLAGS_KMSAN_RUNTIME)
CFLAGS_hooks.o := $(CC_FLAGS_KMSAN_RUNTIME)
CFLAGS_init.o := $(CC_FLAGS_KMSAN_RUNTIME)
CFLAGS_instrumentation.o := $(CC_FLAGS_KMSAN_RUNTIME)
CFLAGS_report.o := $(CC_FLAGS_KMSAN_RUNTIME)
CFLAGS_shadow.o := $(CC_FLAGS_KMSAN_RUNTIME)

# 测试对象必须反向开启 KMSAN 插桩，才能构造未初始化值并验证报告；关闭编译器
# uninitialized 静态告警，避免它在运行期测试触发前就拒绝这些刻意用例。
obj-$(CONFIG_KMSAN_KUNIT_TEST) += kmsan_test.o
KMSAN_SANITIZE_kmsan_test.o := y
CFLAGS_kmsan_test.o += $(call cc-disable-warning, uninitialized)
