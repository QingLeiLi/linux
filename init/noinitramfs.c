// SPDX-License-Identifier: GPL-2.0-only
/*
 * init/noinitramfs.c
 *
 * Copyright (C) 2006, NXP Semiconductors, All Rights Reserved
 * Author: Jean-Paul Saman <jean-paul.saman@nxp.com>
 */
#include <linux/init.h>
#include <linux/stat.h>
#include <linux/kdev_t.h>
#include <linux/syscalls.h>
#include <linux/init_syscalls.h>
#include <linux/umh.h>

/*
 * CONFIG_BLK_DEV_INITRD=n 时没有 cpio 解包器，本文件提供互斥的最小 rootfs
 * 填充路径。它不加载外部内容，只创建启动后续代码普遍依赖的 /dev、控制台节点
 * 和 /root；对象一旦成功创建就归 rootfs/VFS 持有，本 init 函数不负责回收。
 */

/*
 * Create a simple rootfs that is similar to the default initramfs
 */
/*
 * 契约：rootfs_initcall 阶段先开放 usermode helper，再依次创建 0755 的 /dev、
 * 字符设备 /dev/console（主设备 5、次设备 1，仅 root 可读写）和 0700 的 /root。
 * 任一步失败即返回首个负 errno 并打印统一警告；此前已创建的对象不会回滚，
 * 因而该过程不是事务。成功返回 0，后续挂载/启动用户空间可依赖这三个基础路径。
 */
static int __init default_rootfs(void)
{
	int err;

	usermodehelper_enable();
	err = init_mkdir("/dev", 0755);
	if (err < 0)
		goto out;

	err = init_mknod("/dev/console", S_IFCHR | S_IRUSR | S_IWUSR,
			new_encode_dev(MKDEV(5, 1)));
	if (err < 0)
		goto out;

	err = init_mkdir("/root", 0700);
	if (err < 0)
		goto out;

	return 0;

out:
	printk(KERN_WARNING "Failed to create a rootfs\n");
	return err;
}
/* 在 rootfs initcall 层执行，且由 Makefile 保证不与完整 initramfs 填充器同时链接。 */
rootfs_initcall(default_rootfs);
