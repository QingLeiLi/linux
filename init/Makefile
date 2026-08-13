# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the linux kernel.
# 本文件描述内核启动目录的内建对象、配置分支和构建版本生成规则。
#

#
# init/ 构建地图：
#   1. main.o、version.o 和 mounts.o 始终进入 init/built-in.a；
#   2. CONFIG_BLK_DEV_INITRD 在完整 initramfs 解包器和空实现之间二选一；
#   3. 延时校准、initramfs KUnit 测试以及部分挂载后端由各自配置决定；
#   4. version.o 使用临时 UTS_VERSION 参与常规增量构建，最终链接前再单独
#      构建 version-timestamp.o，把最终构建序号和时间写入 vmlinux。
#
# 这里的 obj-y/obj-$(CONFIG_*) 由 scripts/Makefile.build 消费。列入 obj-y
# 表示对象最终链接进内核镜像，不代表其中的 __init 代码会永久占用内存；
# 启动完成后，链接脚本仍可回收标有 __init/__initdata 的 section。
#

# 禁止编译器把本目录函数和数据拆成逐符号 section。启动核心对象需要保持
# 链接布局约束；该目录级 ccflags-y 会附加到下方所有由 Kbuild 编译的对象。
ccflags-y := -fno-function-sections -fno-data-sections

# 无条件构建启动主线、构建身份占位对象和根文件系统挂载组合对象。
# mounts.o 不是同名源文件，而是由后面的 mounts-y 列表复合链接而成。
obj-y                          := main.o version.o mounts.o
#
# CONFIG_BLK_DEV_INITRD=n 时，noinitramfs.o 通过 rootfs initcall 创建 /dev、
# /dev/console 和 /root；启用时改由 initramfs.o 展开内置/外部 cpio 并管理
# initrd 内存。两条根文件系统填充路径必须互斥，避免同时提交不兼容的启动布局。
ifneq ($(CONFIG_BLK_DEV_INITRD),y)
obj-y                          += noinitramfs.o
else
obj-$(CONFIG_BLK_DEV_INITRD)   += initramfs.o
endif
# 架构选择通用延时校准算法时才加入 calibrate.o；否则架构代码提供等价能力。
obj-$(CONFIG_GENERIC_CALIBRATE_DELAY) += calibrate.o
# 仅测试配置启用时把 initramfs_test.o 编入内核，生产配置不会承担测试代码成本。
obj-$(CONFIG_INITRAMFS_TEST)   += initramfs_test.o

# 初始 task、信号、文件表等静态对象必须始终存在，因此不受可选配置控制。
obj-y                          += init_task.o

#
# mounts.o 的复合对象地图：do_mounts.o 提供根设备解析和挂载主流程；RAM disk
# 支持加入 do_mounts_rd.o，传统 initrd 支持再加入 do_mounts_initrd.o。Kbuild
# 先把这里选中的成员做可重定位链接，再把得到的 mounts.o 放入 built-in.a。
# 配置关闭只移除相应后端，主流程仍通过条件编译/桩函数保持一致接口。
mounts-y			:= do_mounts.o
mounts-$(CONFIG_BLK_DEV_RAM)	+= do_mounts_rd.o
mounts-$(CONFIG_BLK_DEV_INITRD)	+= do_mounts_initrd.o

#
# UTS_VERSION
# 下列规则生成 `uname -v` 和 linux_banner 使用的 UTS_VERSION。它是构建身份，
# 不是运行期递增的内核状态；同一源码树的不同构建可因序号、抢占模型或
# 时间戳而不同。
#

# 变量名末尾的 `-y` 是 Kbuild 选择技巧：配置值为 y 时，右侧标签写入最终
# 使用的 smp-flag-y/preempt-flag-y；配置未选中时只会写入 `-n` 等未引用变量。
# 抢占模型由 Kconfig 保证有效组合；动态或 RT 标签用于区分相同 release 下
# 具有不同调度语义的镜像。
smp-flag-$(CONFIG_SMP)			:= SMP
preempt-flag-$(CONFIG_PREEMPT_BUILD)	:= PREEMPT
preempt-flag-$(CONFIG_PREEMPT_DYNAMIC)	:= PREEMPT_DYNAMIC
preempt-flag-$(CONFIG_PREEMPT_RT)	:= PREEMPT_RT

# 优先采用用户通过环境/命令行固定的可复现构建值；没有显式值时，再使用
# 当前目标通过 target-specific variable 提供的自动序号和时间。这里使用递归
# 展开 `=`，使变量到生成目标执行时才读取下方绑定的 auto 值。
build-version = $(or $(KBUILD_BUILD_VERSION), $(build-version-auto))
build-timestamp = $(or $(KBUILD_BUILD_TIMESTAMP), $(build-timestamp-auto))

# Maximum length of UTS_VERSION is 64 chars
# UTS_VERSION 最长为 64 字节。cut 按字节截断由“#序号、SMP/抢占标签、时间”
# 拼成的内容，随后输出一个可由编译器强制包含的宏定义。`$(pound)` 生成字面
# `#`，避免它在 Makefile 解析阶段被当成注释起始符。
# filechk_uts_version 是给通用 `$(call filechk,uts_version)` 调用的命令体；
# scripts/Kbuild.include 中的 filechk 先写临时文件，仅在内容变化时替换目标，
# 从而避免 FORCE 检查无条件改变时间戳并触发无意义重编译。
filechk_uts_version = \
	utsver=$$(echo '$(pound)'"$(build-version)" $(smp-flag-y) $(preempt-flag-y) "$(build-timestamp)" | cut -b -64); \
	echo '$(pound)'define UTS_VERSION \""$${utsver}"\"

#
# Build version.c with temporary UTS_VERSION
# 使用临时 UTS_VERSION 构建 version.c。该翻译单元先声明 weak 的
# init_uts_ns/linux_banner，再包含 version-timestamp.c 得到占位内容；这样常规
# built-in.a 可以稳定复用，不必因为最终构建序号或时间变化而整体重编译。
#

# FORCE 让每轮构建都重新计算候选内容，但 filechk 只在结果真正变化时更新
# init/utsversion-tmp.h。此目标没有设置 auto 值；若用户也未指定 KBUILD_BUILD_*
# 值，占位字符串主要保留 SMP/抢占模型，最终不会压过后面的强定义。
$(obj)/utsversion-tmp.h: FORCE
	$(call filechk,uts_version)

# 告诉 Kbuild clean 阶段删除这个位于对象目录中的生成头文件。
clean-files += utsversion-tmp.h

# 显式依赖保证头文件先生成；`-include` 使编译器在读取 version.c 前注入
# UTS_VERSION，无需在源码中写死生成文件路径。
$(obj)/version.o: $(obj)/utsversion-tmp.h
CFLAGS_version.o := -include $(obj)/utsversion-tmp.h

#
# Build version-timestamp.c with final UTS_VERSION
# 使用最终 UTS_VERSION 单独构建 version-timestamp.c。link-vmlinux.sh 在最终
# vmlinux 链接前显式请求这个对象，并把它追加到链接输入；其中非 weak 定义
# 覆盖 version.o 的占位定义，只让最后这个小对象承受时间戳导致的重编译。
#

# 这两个 target-specific variable 只在生成 include/generated/utsversion.h 及其
# 依赖上下文中生效。未提供可复现构建值时，scripts/build-version 读取并递增
# `.version` 作为构建序号，`LC_ALL=C date` 产生不受本地语言影响的时间字符串。
include/generated/utsversion.h: build-version-auto = $(shell $(srctree)/scripts/build-version)
include/generated/utsversion.h: build-timestamp-auto = $(shell LC_ALL=C date)
# 即使目标存在也重新计算候选最终身份；filechk 仍负责内容相同时保留旧文件。
include/generated/utsversion.h: FORCE
	$(call filechk,uts_version)

# 最终对象必须晚于最终生成头；同样用强制包含把 UTS_VERSION 提供给源文件。
# 该对象不列入上方 obj-y，而由 link-vmlinux.sh 在最终链接阶段显式构建和加入。
$(obj)/version-timestamp.o: include/generated/utsversion.h
CFLAGS_version-timestamp.o := -include include/generated/utsversion.h
