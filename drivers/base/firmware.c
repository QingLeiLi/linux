// SPDX-License-Identifier: GPL-2.0
/*
 * firmware.c - firmware subsystem hoohaw.
 *
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 * Copyright (c) 2007 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (c) 2007 Novell Inc.
 */
/*
 * 本文件只负责建立固件子系统的 sysfs 顶层容器。这里的“firmware”不是
 * request_firmware() 所加载的固件镜像，而是供设备树、ACPI 等固件描述机制
 * 挂接其用户态视图的 /sys/firmware 根目录。
 */
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>

#include "base.h"

struct kobject *firmware_kobj;
/*
 * firmware_kobj 是已发布的 /sys/firmware kobject。firmware_init() 创建并
 * 持有其初始引用，随后 of_core_init() 等调用者只借用该指针作为父对象；
 * 启动期对象常驻，不在正常运行期间注销。导出符号允许其他内核组件在同一
 * 命名空间下建立自己的固件属性目录。
 */
EXPORT_SYMBOL_GPL(firmware_kobj);

/*
 * firmware_init() - 发布固件相关 sysfs 对象的共同父目录。
 *
 * 【宏观位置】driver_init() 的第一层核心初始化；早于 of_core_init()。
 * 入参：无。运行于可睡眠的早期进程上下文，调用者不持有需要继承的锁。
 *
 * kobject_create_and_add() 同时分配、初始化并把名为 firmware 的对象加入
 * sysfs；父指针为 NULL 表示直接位于 sysfs 根。成功后全局 firmware_kobj
 * 持有该对象并向后续子系统发布；失败时没有对象和引用需要回滚。
 *
 * 返回：0 表示 /sys/firmware 已可作为父目录；-ENOMEM 表示创建失败。
 * driver_init() 不上传该错误，因此依赖者仍必须把父对象不可用视为启动环境
 * 严重退化，而不能假设本函数形成了可回滚事务。
 */
int __init firmware_init(void)
{
	/* 唯一提交点：返回非 NULL 时对象已经加入 kobject/sysfs 层次。 */
	firmware_kobj = kobject_create_and_add("firmware", NULL);
	if (!firmware_kobj)
		return -ENOMEM;
	/* 全局指针保留创建所得引用，调用者无需也不能在这里 put。 */
	return 0;
}
