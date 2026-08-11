# SPDX-License-Identifier: GPL-2.0

# IRQ 核心基础对象始终链接：描述符、分派/管理、虚假检测、重发、chip 封装、两种空芯片、
# devres 与 kexec 接口在所有构建中提供；各对象内部再用 Kconfig 裁剪不适用的局部路径。
obj-y := irqdesc.o handle.o manage.o spurious.o resend.o chip.o dummychip.o devres.o kexec.o
# 通用可配置 irq_chip 的寄存器、缓存和 domain generic-chip 支持。
obj-$(CONFIG_GENERIC_IRQ_CHIP) += generic-chip.o
# 旧式 probe_irq_on/off/mask 自动探测实现。
obj-$(CONFIG_GENERIC_IRQ_PROBE) += autoprobe.o
# hwirq <-> virq 映射、层级 domain 与可选 domain debugfs 的主体。
obj-$(CONFIG_IRQ_DOMAIN) += irqdomain.o
# 依赖 irq_work/domain 的软件模拟中断 provider。
obj-$(CONFIG_IRQ_SIM) += irq_sim.o
# /proc/irq 接口；/proc/interrupts 的通用展示在源码内再受 GENERIC_IRQ_SHOW 裁剪。
obj-$(CONFIG_PROC_FS) += proc.o
# 中断上下文不能立即迁移时保存并提交 pending affinity。
obj-$(CONFIG_GENERIC_PENDING_IRQ) += migration.o
# CPU 下线前迁离 IRQ，以及上下线阶段调用 irqchip 回调的通用实现。
obj-$(CONFIG_GENERIC_IRQ_MIGRATION) += cpuhotplug.o
# 系统睡眠期间 IRQ action 标记、暂停与恢复协调；非睡眠 PM 不需要此对象。
obj-$(CONFIG_PM_SLEEP) += pm.o
# 通用 MSI 描述符、层级 domain 和设备 MSI API。
obj-$(CONFIG_GENERIC_MSI_IRQ) += msi.o
# 基于层级 irq_domain 的通用 IPI 分配与发送支持。
obj-$(CONFIG_GENERIC_IRQ_IPI) += ipi.o
# 多个逻辑 IPI 复用父 IRQ 的 per-CPU pending 位图分派器。
obj-$(CONFIG_GENERIC_IRQ_IPI_MUX) += ipi-mux.o
# SMP 才需要通用 IRQ affinity 分配与 spreading 算法。
obj-$(CONFIG_SMP) += affinity.o
# 单 IRQ/domain 状态文件与调试注入入口。
obj-$(CONFIG_GENERIC_IRQ_DEBUGFS) += debugfs.o
# CPU/向量矩阵的 managed、reserved 与普通分配记账实现。
obj-$(CONFIG_GENERIC_IRQ_MATRIX_ALLOCATOR) += matrix.o
# IRQ 管理状态机的内建 KUnit 测试对象。
obj-$(CONFIG_IRQ_KUNIT_TEST) += irq_test.o
