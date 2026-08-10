// SPDX-License-Identifier: GPL-2.0
/*
 * hypervisor.c - /sys/hypervisor subsystem.
 *
 * Copyright (C) IBM Corp. 2006
 * Copyright (C) 2007 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (C) 2007 Novell Inc.
 */
/*
 * 本文件在 CONFIG_SYS_HYPERVISOR 配置下建立 /sys/hypervisor。具体虚拟化
 * 平台随后可在该公共父目录下发布类型、版本或平台特有属性；本文件本身不
 * 识别 hypervisor，也不与虚拟机监控器通信。
 */

#include <linux/kobject.h>
#include <linux/device.h>
#include <linux/export.h>
#include "base.h"

struct kobject *hypervisor_kobj;
/*
 * hypervisor_kobj 指向已经发布的 /sys/hypervisor，并持有创建时得到的引用。
 * 它在启动期创建后常驻，其他子系统借用该全局父对象；配置关闭时 base.h
 * 提供空 stub，因此该变量和目录都不存在。
 */
EXPORT_SYMBOL_GPL(hypervisor_kobj);

/*
 * hypervisor_init() - 发布虚拟化平台信息的 sysfs 顶层目录。
 *
 * 【调用位置】driver_init() 在基础 kset 建立后调用；无入参，返回后没有后续
 * 探测动作。运行于可睡眠的早期进程上下文，不要求调用者持锁。
 *
 * 返回：0 表示对象已加入 sysfs 且全局指针持有引用；-ENOMEM 表示分配或
 * 注册失败，此时 hypervisor_kobj 为 NULL、没有资源需要释放。driver_init()
 * 忽略返回值，所以失败只使该可选 sysfs 命名空间缺失，不存在局部重试。
 */
int __init hypervisor_init(void)
{
	/* 父对象为 NULL，使目录直接发布在 sysfs 根。 */
	hypervisor_kobj = kobject_create_and_add("hypervisor", NULL);
	if (!hypervisor_kobj)
		return -ENOMEM;
	return 0;
}
