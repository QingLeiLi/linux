# SPDX-License-Identifier: GPL-2.0

CONTEXT_ANALYSIS_core.o := y
CONTEXT_ANALYSIS_fair.o := y
# Clang 上下文分析只对显式选择的目标生效。这里让两个独立的大型调度器编译单元
# 接受睡眠/原子上下文属性检查；该开关只增加编译期诊断，不改变生成对象的运行语义。

# The compilers are complaining about unused variables inside an if(0) scope
# block. This is daft, shut them up.
# 编译器会报告 if (0) 作用域中的变量“赋值但未使用”；这些不可达分支常用于让宏
# 保持统一结构，告警没有调度语义价值，因此在当前目录统一关闭它。
# cc-disable-warning 会先探测当前编译器是否真正支持该选项，避免直接传入未知的
# -Wno-*；ccflags-y 随后把得到的选项用于本 Makefile 发起的所有普通 C 编译。
ccflags-y += $(call cc-disable-warning, unused-but-set-variable)

# These files are disabled because they produce non-interesting flaky coverage
# that is not a function of syscall inputs. E.g. involuntary context switches.
# 本目录关闭 KCOV：调度器会因时钟、中断、抢占和其他任务的非自愿切换而执行，
# 覆盖变化并非只由当前系统调用输入决定。把这种异步噪声反馈给 fuzzing 会降低
# 输入与覆盖之间的可重复性；KCOV_INSTRUMENT := n 会阻止当前目录的所有目标加入
# KCOV 编译插桩。
KCOV_INSTRUMENT := n

# Disable KCSAN to avoid excessive noise and performance degradation. To avoid
# false positives ensure barriers implied by sched functions are instrumented.
# 完整 KCSAN 访存插桩会显著扰动调度热路径并制造大量无关报告，所以本目录整体关闭。
# 但调度器 API 隐含的屏障仍是 KCSAN 并发模型的一部分：若屏障也不可见，检测器会
# 错把已经有 happens-before 关系的访问判为竞态。因此第二个开关保留“仅屏障”插桩，
# 使未插桩调度函数中的同步语义仍能被检测器识别。
KCSAN_SANITIZE := n
KCSAN_INSTRUMENT_BARRIERS := y

ifneq ($(CONFIG_SCHED_OMIT_FRAME_POINTER),y)
# According to Alan Modra <alan@linuxcare.com.au>, the -fno-omit-frame-pointer is
# needed for x86 only.  Why this used to be enabled for all architectures is beyond
# me.  I suspect most platforms don't need this, but until we know that for sure
# I turn this off for IA-64 only.  Andreas Schwab says it's also needed on m68k
# to get a correct value for the wait-channel (WCHAN in ps). --davidm
# 这段历史说明认为 -fno-omit-frame-pointer 主要是 x86 所需；作者不清楚它为何曾对
# 所有体系结构启用，并推测多数平台不需要，但在得到确定结论前只对 IA-64 关闭。
# Andreas Schwab 还指出 m68k 需要保留帧指针，ps 的等待通道 WCHAN 才能取到正确值。
#
# 当前源码的实际配置边界比这段历史文字更广：x86、ARC、LoongArch、MIPS、PowerPC
# 和 s390 都定义了 SCHED_OMIT_FRAME_POINTER。配置值不是 y 时，只给 core.o 追加
# 可选的 PROFILING 参数和 -fno-omit-frame-pointer；值为 y 时不在这里强制保留帧指针。
# := 在解析本 Makefile 时立即展开右侧，CFLAGS_core.o 又只影响 core.c 这一编译单元。
CFLAGS_core.o := $(PROFILING) -fno-omit-frame-pointer
endif

# Branch profiling isn't noinstr-safe
# 分支跟踪会在 likely()/unlikely() 等位置加入统计调用，而 noinstr 路径要求编译器不能
# 注入这类可能递归进入跟踪或破坏低层上下文的代码。因此启用全局分支分析时，下面
# 仍对两个聚合编译单元定义 DISABLE_BRANCH_PROFILING，使 compiler.h 保留分支预测
# 语义但跳过 ftrace 分支计数；条件关闭时不额外传入该宏。
ifdef CONFIG_TRACE_BRANCH_PROFILING
CFLAGS_build_policy.o += -DDISABLE_BRANCH_PROFILING
CFLAGS_build_utility.o += -DDISABLE_BRANCH_PROFILING
endif
#
# Build efficiency:
#
# These compilation units have roughly the same size and complexity - so their
# build parallelizes well and finishes roughly at once:
#
# 这四个编译单元的规模和复杂度大致相当，因此构建系统可以并行编译，通常也会在
# 相近时间结束，避免一个超大目标成为串行尾部瓶颈。build_policy.c 和
# build_utility.c 不是普通的单功能实现文件：它们用 #include 把多份调度器 .c 源码
# 合并为两个翻译单元，以摊薄重复解析公共头文件的成本；core.c 与 fair.c 单独编译。
#
# obj-y 表示这些对象无条件进入 kernel/sched/built-in.a，父级 kernel/Makefile 又以
# obj-y += sched/ 让该薄归档最终参与 vmlinux 链接。这里的排列也是归档/最终链接顺序，
# 不能把它误解为运行时调度类的优先级；真正的类次序由调度类定义和链接脚本协议决定。
obj-y += core.o
obj-y += fair.o
obj-y += build_policy.o
obj-y += build_utility.o
