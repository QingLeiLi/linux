# SPDX-License-Identifier: GPL-2.0
# KASAN 运行时本身实现影子内存检查和错误报告，绝不能再被 KASAN/UBSAN/KCOV
# 递归插桩；目录级 n 在所有本目录对象上关闭这些工具，再按对象移除 ftrace。
KASAN_SANITIZE := n
UBSAN_SANITIZE := n
KCOV_INSTRUMENT := n

# Disable ftrace to avoid recursion.
# 关闭 ftrace 以避免 KASAN helper 被跟踪后，跟踪代码再次访问内存并重新进入
# KASAN 的递归。逐对象移除只覆盖运行时对象，测试对象仍使用专门测试 flags。
CFLAGS_REMOVE_common.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_generic.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_init.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_quarantine.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_report.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_report_generic.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_report_hw_tags.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_report_sw_tags.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_shadow.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_hw_tags.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_sw_tags.o = $(CC_FLAGS_FTRACE)

# Function splitter causes unnecessary splits in __asan_load1/__asan_store1
# see: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=63533
# GCC 的 function splitter 会把 __asan_load1/__asan_store1 等极短热 helper
# 无意义拆分；若编译器支持，-fno-conserve-stack 抑制该优化并保持运行时布局。
CC_FLAGS_KASAN_RUNTIME := $(call cc-option, -fno-conserve-stack)
# 运行时不能依赖 stack protector，因为保护代码本身可能调用尚未安全可用的内存
# 检查路径；所有运行时对象共享该 flag 集。
CC_FLAGS_KASAN_RUNTIME += -fno-stack-protector
# Disable branch tracing to avoid recursion.
# 分支 profiling 也会插入回调并再次触发 KASAN，宏使相关头文件跳过插桩。
CC_FLAGS_KASAN_RUNTIME += -DDISABLE_BRANCH_PROFILING

# core、各模式实现、shadow/quarantine 与报告路径统一使用递归安全的运行时 flags；
# 这里显式枚举可防止以后新增测试对象意外继承并失去应有的 KASAN 插桩。
CFLAGS_common.o := $(CC_FLAGS_KASAN_RUNTIME)
CFLAGS_generic.o := $(CC_FLAGS_KASAN_RUNTIME)
CFLAGS_init.o := $(CC_FLAGS_KASAN_RUNTIME)
CFLAGS_quarantine.o := $(CC_FLAGS_KASAN_RUNTIME)
CFLAGS_report.o := $(CC_FLAGS_KASAN_RUNTIME)
CFLAGS_report_generic.o := $(CC_FLAGS_KASAN_RUNTIME)
CFLAGS_report_hw_tags.o := $(CC_FLAGS_KASAN_RUNTIME)
CFLAGS_report_sw_tags.o := $(CC_FLAGS_KASAN_RUNTIME)
CFLAGS_shadow.o := $(CC_FLAGS_KASAN_RUNTIME)
CFLAGS_hw_tags.o := $(CC_FLAGS_KASAN_RUNTIME)
CFLAGS_sw_tags.o := $(CC_FLAGS_KASAN_RUNTIME)

CFLAGS_KASAN_TEST := $(CFLAGS_KASAN)
# 测试代码必须被 KASAN 插桩才能主动制造并观察错误；编译器不支持带前缀的
# memintrinsic 插桩时禁用 builtin 内联，确保 memcpy 等仍经过可检测调用边界。
ifndef CONFIG_CC_HAS_KASAN_MEMINTRINSIC_PREFIX
# If compiler instruments memintrinsics by prefixing them with __asan/__hwasan,
# we need to treat them normally (as builtins), otherwise the compiler won't
# recognize them as instrumentable. If it doesn't instrument them, we need to
# pass -fno-builtin, so the compiler doesn't inline them.
# 若编译器会把内存 intrinsic 改写为 __asan/__hwasan 前缀，应保持 builtin 让它
# 识别可插桩；若不会改写，则传 -fno-builtin，防止内联绕过测试希望命中的检查。
CFLAGS_KASAN_TEST += -fno-builtin
endif

# KASAN 测试刻意使用很小的 VLA 等边界场景，移除该告警避免测试意图被构建告警
# 阻断；C 与 Rust 测试分别继承各自语言的 KASAN 插桩 flags。
CFLAGS_REMOVE_kasan_test_c.o += $(call cc-option, -Wvla-larger-than=1)
CFLAGS_kasan_test_c.o := $(CFLAGS_KASAN_TEST)
RUSTFLAGS_kasan_test_rust.o := $(RUSTFLAGS_KASAN)

obj-y := common.o report.o
# common/report 始终存在；三种 KASAN 模式再选择各自 shadow/tag 初始化、访问检查
# 和报告器。相同对象被多个模式列出时由互斥 Kconfig 保证不会形成冲突组合。
obj-$(CONFIG_KASAN_GENERIC) += init.o generic.o report_generic.o shadow.o quarantine.o
obj-$(CONFIG_KASAN_HW_TAGS) += hw_tags.o report_hw_tags.o tags.o report_tags.o
obj-$(CONFIG_KASAN_SW_TAGS) += init.o report_sw_tags.o shadow.o sw_tags.o tags.o report_tags.o

kasan_test-objs := kasan_test_c.o
# kasan_test 是复合 KUnit 目标；启用 Rust 时把 Rust 用例加入同一测试模块，否则
# 只链接 C 用例，外层 CONFIG_KASAN_KUNIT_TEST 决定整个复合目标是否生成。
ifdef CONFIG_RUST
	kasan_test-objs += kasan_test_rust.o
endif

obj-$(CONFIG_KASAN_KUNIT_TEST) += kasan_test.o
