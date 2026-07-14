// SPDX-License-Identifier: GPL-2.0
/* Lock down the kernel
 *
 * Copyright (C) 2016 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */

#include <linux/security.h>
#include <linux/export.h>
#include <linux/lsm_hooks.h>
#include <uapi/linux/lsm.h>

static enum lockdown_reason kernel_locked_down;

static const enum lockdown_reason lockdown_levels[] = {LOCKDOWN_NONE,
						 LOCKDOWN_INTEGRITY_MAX,
						 LOCKDOWN_CONFIDENTIALITY_MAX};

/*
 * Put the kernel into lock-down mode.
 */
static int lock_kernel_down(const char *where, enum lockdown_reason level)
{
	if (kernel_locked_down >= level)
		return -EPERM;

	kernel_locked_down = level;
	pr_notice("Kernel is locked down from %s; see man kernel_lockdown.7\n",
		  where);
	return 0;
}

static int __init lockdown_param(char *level)
{
	if (!level)
		return -EINVAL;

	if (strcmp(level, "integrity") == 0)
		lock_kernel_down("command line", LOCKDOWN_INTEGRITY_MAX);
	else if (strcmp(level, "confidentiality") == 0)
		lock_kernel_down("command line", LOCKDOWN_CONFIDENTIALITY_MAX);
	else
		return -EINVAL;

	return 0;
}

early_param("lockdown", lockdown_param);

/**
 * lockdown_is_locked_down - Find out if the kernel is locked down
 * @what: Tag to use in notice generated if lockdown is in effect
 */
static int lockdown_is_locked_down(enum lockdown_reason what)
{
	if (WARN(what >= LOCKDOWN_CONFIDENTIALITY_MAX,
		 "Invalid lockdown reason"))
		return -EPERM;

	if (kernel_locked_down >= what) {
		if (lockdown_reasons[what])
			pr_notice_ratelimited("Lockdown: %s: %s is restricted; see man kernel_lockdown.7\n",
				  current->comm, lockdown_reasons[what]);
		return -EPERM;
	}

	return 0;
}

static struct security_hook_list lockdown_hooks[] __ro_after_init = {
	LSM_HOOK_INIT(locked_down, lockdown_is_locked_down),
};

static const struct lsm_id lockdown_lsmid = {
	.name = "lockdown",
	.id = LSM_ID_LOCKDOWN,
};

static int __init lockdown_lsm_init(void)
{
#if defined(CONFIG_LOCK_DOWN_KERNEL_FORCE_INTEGRITY)
	lock_kernel_down("Kernel configuration", LOCKDOWN_INTEGRITY_MAX);
#elif defined(CONFIG_LOCK_DOWN_KERNEL_FORCE_CONFIDENTIALITY)
	lock_kernel_down("Kernel configuration", LOCKDOWN_CONFIDENTIALITY_MAX);
#endif
	security_add_hooks(lockdown_hooks, ARRAY_SIZE(lockdown_hooks),
			   &lockdown_lsmid);
	return 0;
}

static ssize_t lockdown_read(struct file *filp, char __user *buf, size_t count,
			     loff_t *ppos)
{
	char temp[80] = "";
	int i, offset = 0;

	for (i = 0; i < ARRAY_SIZE(lockdown_levels); i++) {
		enum lockdown_reason level = lockdown_levels[i];

		if (lockdown_reasons[level]) {
			const char *label = lockdown_reasons[level];

			if (kernel_locked_down == level)
				offset += sprintf(temp+offset, "[%s] ", label);
			else
				offset += sprintf(temp+offset, "%s ", label);
		}
	}

	/* Convert the last space to a newline if needed. */
	if (offset > 0)
		temp[offset-1] = '\n';

	return simple_read_from_buffer(buf, count, ppos, temp, strlen(temp));
}

static ssize_t lockdown_write(struct file *file, const char __user *buf,
			      size_t n, loff_t *ppos)
{
	char *state;
	int i, len, err = -EINVAL;

	state = memdup_user_nul(buf, n);
	if (IS_ERR(state))
		return PTR_ERR(state);

	len = strlen(state);
	if (len && state[len-1] == '\n') {
		state[len-1] = '\0';
		len--;
	}

	for (i = 0; i < ARRAY_SIZE(lockdown_levels); i++) {
		enum lockdown_reason level = lockdown_levels[i];
		const char *label = lockdown_reasons[level];

		if (label && !strcmp(state, label))
			err = lock_kernel_down("securityfs", level);
	}

	kfree(state);
	return err ? err : n;
}

static const struct file_operations lockdown_ops = {
	.read  = lockdown_read,
	.write = lockdown_write,
};

static int __init lockdown_secfs_init(void)
{
	struct dentry *dentry;

	dentry = securityfs_create_file("lockdown", 0644, NULL, NULL,
					&lockdown_ops);
	return PTR_ERR_OR_ZERO(dentry);
}

// 注册 LSM
/*
	lockdown 是内核完整性保护机制，核心目标是：防止 root 用户破坏内核自身。

	---
	背景问题

	传统 Linux 中 root 权限几乎等同于内核权限：
	- root 可以写 /dev/mem 直接改内核内存
	- root 可以加载内核模块（任意代码进内核）
	- root 可以用 kexec 替换运行中的内核
	- root 可以读 /proc/kcore 窥探内核数据

	这意味着：一旦攻击者获得 root，就能绕过 SELinux/AppArmor 等所有安全机制，直接篡改内核。

	---
	lockdown 的两个级别

	LOCKDOWN_INTEGRITY_MAX   ← 完整性模式
		防止内核被修改（写操作被拦截）
		- 禁止写 /dev/mem、/dev/kmem
		- 禁止加载未签名内核模块
		- 禁止 kexec 加载未签名内核
		- 禁止 hibernation（会写内核镜像到磁盘）
		- 禁止调试器写内核内存（ptrace 部分功能）

	LOCKDOWN_CONFIDENTIALITY_MAX   ← 保密模式（包含完整性模式的所有限制）
		额外防止内核数据被读取
		- 禁止读 /dev/mem、/proc/kcore
		- 禁止 perf 采样内核地址
		- 禁止某些 ACPI 表访问

	---
	与 Secure Boot 的关系

	lockdown 最典型的用法是配合 UEFI Secure Boot：

	Secure Boot 验证内核镜像签名
		↓ 内核启动时检测到 Secure Boot 开启
		↓ 自动启用 lockdown（完整性模式）

	效果：从固件到内核的信任链完整
		攻击者即使获得 root，也无法注入未签名代码破坏这条链

	没有 lockdown，攻击者 root 后可以通过 /dev/mem 修改内核，完全绕过 Secure Boot 建立的信任链。

	---
	为什么要 DEFINE_EARLY_LSM

	lockdown 必须在任何可能被它拦截的操作之前就绪。如果等到普通 security_init() 才初始化，早期启动阶段就有窗口期可以绕过它。放入 early_security_init() 确保它是最早生效的安全机制之一。
*/
#ifdef CONFIG_SECURITY_LOCKDOWN_LSM_EARLY
DEFINE_EARLY_LSM(lockdown) = {
#else
DEFINE_LSM(lockdown) = {
#endif
	.id = &lockdown_lsmid,
	.init = lockdown_lsm_init,
	.initcall_core = lockdown_secfs_init,
};
