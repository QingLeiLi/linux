// SPDX-License-Identifier: GPL-2.0
/*
 * System bus type for containers.
 *
 * Copyright (C) 2013, Intel Corporation
 * Author: Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 */
/*
 * 本文件定义“container”系统总线。这里的 container 是可作为一组系统设备
 * 在线/离线边界的固件或平台容器，不等同于用户空间 namespace/cgroup 容器。
 * 总线把通用 device online/offline 协议转发给具体 struct container_dev。
 */

#include <linux/container.h>

#include "base.h"

/* 同时作为 bus_type.name 和成员设备默认名前缀，保证两个 sysfs 视图命名一致。 */
#define CONTAINER_BUS_NAME	"container"

/*
 * trivial_online() - container 总线的默认上线回调。
 *
 * @dev: 正在上线的设备；纯借用，回调不取得引用，也不修改该指针的 ownership。
 * 当前总线不要求额外的总线上线动作，因而直接返回 0，把通用状态提交留给
 * 外层设备 online 协议。
 * 本回调运行在设备 online/offline 管理路径的锁约束内，本身不睡眠。
 */
static int trivial_online(struct device *dev)
{
	return 0;
}

/*
 * container_offline() - 把通用 device 下线请求分派给容器实现。
 *
 * @dev: 内嵌在 struct container_dev 中的设备；调用期间为借用指针，外层设备
 * 核心保证对象存活。to_container_dev() 恢复承载对象，不增加引用。
 *
 * 若具体容器提供 @offline 回调，则返回其全部结果类别（0 成功、负 errno
 * 拒绝或失败）；没有回调表示无需专用动作，按成功返回 0。函数本身不改变
 * 注册关系，是否睡眠取决于具体回调及外层 offline 协议。
 */
static int container_offline(struct device *dev)
{
	/* cdev 只是 dev 所在对象的别名，所有权仍由设备核心持有。 */
	struct container_dev *cdev = to_container_dev(dev);

	/* 条件分派避免要求每一种 container 都实现空的 offline 回调。 */
	return cdev->offline ? cdev->offline(cdev) : 0;
}

/*
 * container_subsys 描述全局唯一的 container 总线：name 决定 /sys/bus/container，
 * dev_name 为没有显式名称的成员设备提供前缀；online/offline 是设备核心执行
 * 状态切换时调用的策略入口。该静态描述符常驻，注册后由 bus 核心建立并
 * 管理其内部 subsys_private；运行期读写由设备核心相应锁保护。
 */
const struct bus_type container_subsys = {
	.name = CONTAINER_BUS_NAME,
	.dev_name = CONTAINER_BUS_NAME,
	.online = trivial_online,
	.offline = container_offline,
};

/*
 * container_dev_init() - 将 container_subsys 注册为 system 子系统。
 *
 * 【宏观位置】driver_init() 的最后一步；devices_init() 和 buses_init() 已经
 * 按顺序先行。正确入口前提是它们已成功发布 /sys/devices/system 与 /sys/bus，
 * 因为 subsys_system_register() 会直接使用 system_kset；满足前提时才能同时
 * 建立总线视图和 system 根设备。无入参，运行于可睡眠的早期进程上下文。
 *
 * 返回：无直接返回值。成功副作用是 container_subsys 对其他注册者可见；
 * 失败时 helper 保持未注册状态，本函数只记录 errno，不 panic、也不重试。
 * ret 仅在本函数内传递注册结果，不持有对象或引用。
 */
void __init container_dev_init(void)
{
	int ret;

	/* NULL groups 表示 container system 根设备没有额外的公共属性组。 */
	ret = subsys_system_register(&container_subsys, NULL);
	if (ret)
		/* 日志是 void 接口向启动诊断暴露失败的唯一途径。 */
		pr_err("%s() failed: %d\n", __func__, ret);
}
