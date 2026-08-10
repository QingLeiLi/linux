// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/memory.h>
#include <linux/of.h>
#include <linux/backing-dev.h>

#include "base.h"

/**
 * driver_init - initialize driver model.
 *
 * Call the driver model init functions to initialize their
 * subsystems. Called early from init/main.c.
 */
/*
 * driver_init() - 在常规 initcall 执行前搭好 Linux 设备模型的公共骨架。
 *
 * 【宏观位置】
 * start_kernel() 后期 → do_basic_setup() → driver_init() → do_initcalls()。
 * 此时内存管理和调度器已可用，ksysfs 初始化也已经尝试，但普通内建驱动尚未
 * 通过各级 initcall 注册；本函数继续准备它们随后要依附的 kset、总线和根设备。
 *
 * 【入口条件】
 * - 入参：无；调用者固定是早期启动路径 do_basic_setup()。
 * - 进程上下文，可以睡眠；没有由调用者传入的锁或对象引用。
 * - do_basic_setup() 已先调用 ksysfs_init()；正常成功时 kobject/kset 可向
 *   sysfs 发布目录，但该调用同样没有向这里传递可检查的成功凭据。
 * - __init 表示启动完成后本函数代码会随 init 段一起释放，不能成为运行期入口。
 * - 下文的 /sys 路径以 CONFIG_SYSFS=y 为主线；关闭时仍建立内核对象与注册
 *   关系，但不会形成同样的用户态目录视图。
 *
 * 【顺序不变量】
 * 1. 先初始化无实际后端的 BDI 和 devtmpfs，使缺少专用 BDI 的映射有回写
 *    占位状态，并为随后出现的字符/块设备节点准备承载环境。
 * 2. 再建立 devices/bus/class 以及 firmware/hypervisor 这些顶层命名空间。
 * 3. 最后注册依赖上述父对象的具体总线和 CPU、内存、NUMA、container 设备。
 *    例如 buses_init() 会把 system_kset 挂到 devices_kset 下，of_core_init()
 *    会把 devicetree kset 挂到 firmware_kobj 下，因此这些调用不能任意交换。
 *
 * 【返回与失败】
 * 返回：无直接返回值，也不向调用者移交引用。可观察副作用是发布一组全局
 * kobject/kset、总线、根设备、工作队列和 devtmpfs 后台线程。helper 只对其
 * 自身范围做回滚、日志或 panic；本编排层既没有统一 rollback，也直接丢弃
 * 所有 int 返回值。因而某些可选层失败只会缺少功能，而基础父对象或 BDI 失败
 * 可能在后续依赖处继续放大；这里不存在可供 do_basic_setup() 处理的错误通道。
 * 完成后 do_basic_setup() 才进入构造器和分级 initcall。
 *
 * 原英文注释说明：本函数初始化驱动模型，依次调用各子系统的初始化函数；
 * 它由 init/main.c 在启动早期调用。这里补出的关键背景是：它建立的是后续
 * 驱动注册所依赖的“父目录和注册框架”，而不是探测具体硬件。
 */
void __init driver_init(void)
{
	/* These are the core pieces */
	/*
	 * 以下是最底层的核心构件。它们先建立通用对象容器；后续注册函数会直接
	 * 使用这些全局根节点，因而这里既是初始化也是对其他子系统的发布边界。
	 */
	/*
	 * 初始化 noop_backing_dev_info 的引用、比例、链表、等待队列和默认
	 * writeback 实例。它是没有真实块设备回写能力时的占位 BDI；这里明确忽略
	 * wb_init() 可能返回的内存分配错误，顶层没有回滚或上报通道。
	 */
	bdi_init(&noop_backing_dev_info);
	/*
	 * 创建内核内部 devtmpfs 挂载、注册文件系统类型并启动 kdevtmpfs；之后
	 * device_add() 路径才能把字符/块设备节点请求交给该后台线程。
	 */
	devtmpfs_init();
	/*
	 * 发布 /sys/devices、/sys/dev/{block,char} 和 device_link_wq；这是几乎
	 * 所有 struct device 以及 system_kset 的共同上层。
	 */
	devices_init();
	/* 发布 /sys/bus，并在 /sys/devices 下建立 system 容器。 */
	buses_init();
	/* 发布 /sys/class，供按功能而非物理拓扑组织设备。 */
	classes_init();
	/* 发布 /sys/firmware；OF 核心稍后会把 devicetree 挂到它下面。 */
	firmware_init();
	/* CONFIG_SYS_HYPERVISOR 下发布 /sys/hypervisor；关闭配置时是无副作用 stub。 */
	hypervisor_init();

	/* These are also core pieces, but must come after the
	 * core core pieces.
	 */
	/*
	 * 以下同样属于驱动核心，但必须晚于上述“核心中的核心”：它们注册的是
	 * 具体总线或系统设备，会消费已经发布的 bus_kset、devices_kset、
	 * class_kset、firmware_kobj 等父对象。原英文注释特别强调了这一依赖层级。
	 */
	/* 建立 faux 根设备、faux 总线和配套驱动；失败时 helper 内部逆序注销。 */
	faux_bus_init();
	/*
	 * CONFIG_OF 下把启动时已有的 device_node 发布到 sysfs 并建立 phandle
	 * 快取；关闭配置时为空 stub。正常层次要求 firmware_kobj 已由
	 * firmware_init() 建立，但本编排层不检查该前置条件；若它为 NULL，kset
	 * 会失去预期父对象，不能把“调用在后”误解成“父对象一定存在”。
	 */
	of_core_init();
	/*
	 * 正常成功时建立 /sys/kernel/software_nodes，供非固件描述的 fwnode 统一
	 * 表示设备属性；若先前 ksysfs_init() 未建立 kernel_kobj，父层同样会退化。
	 */
	software_node_init();
	/* 注册 platform 根设备和 platform_bus_type，承接大量板级/固件枚举设备。 */
	platform_bus_init();
	/* CONFIG_AUXILIARY_BUS 下注册辅助总线；关闭配置时为空 stub。 */
	auxiliary_bus_init();
	/*
	 * CONFIG_MEMORY_HOTPLUG 下发布 memory 子系统并为启动时已存在的内存段
	 * 创建 memory block 设备；关闭配置时为空 stub。
	 */
	memory_dev_init();
	/* CONFIG_NUMA 下创建 node 设备，并把先前的 CPU/内存设备链接到 NUMA 节点。 */
	node_dev_init();
	/* 注册 CPU 系统总线、通用 CPU 设备及体系结构提供的漏洞属性。 */
	cpu_dev_init();
	/* 最后发布 container 系统总线，为可在线/离线的容器型设备提供共同接口。 */
	container_dev_init();
}
