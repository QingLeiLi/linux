// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/init/main.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 * 这是 Linux 内核启动的核心 C 文件。
 * head.S 完成硬件初始化（MMU/页表）后，最终跳转到本文件的 start_kernel()。
 * 整个内核的 C 世界从这里开始。
 *
 * 文件结构：
 *   1. 全局变量和命令行参数处理
 *   2. start_kernel()：内核主初始化函数
 *   3. rest_init()：创建 PID 1 和 PID 2，自身变成 idle
 *   4. kernel_init()：PID 1 的执行体，最终 exec 用户态 init
 *   5. do_initcalls()：驱动和子系统的批量初始化机制
 */

/* 启用 initcall_debug，允许通过内核参数开启初始化调试输出 */
#define DEBUG		/* Enable initcall_debug */

#include <linux/types.h>
#include <linux/export.h>
#include <linux/extable.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/binfmts.h>
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/stackprotector.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/initrd.h>
#include <linux/memblock.h>
#include <linux/acpi.h>
#include <linux/bootconfig.h>
#include <linux/console.h>
#include <linux/nmi.h>
#include <linux/percpu.h>
#include <linux/kmod.h>
#include <linux/kprobes.h>
#include <linux/kmsan.h>
#include <linux/ksysfs.h>
#include <linux/vmalloc.h>
#include <linux/kernel_stat.h>
#include <linux/start_kernel.h>
#include <linux/security.h>
#include <linux/smp.h>
#include <linux/profile.h>
#include <linux/kfence.h>
#include <linux/rcupdate.h>
#include <linux/srcu.h>
#include <linux/moduleparam.h>
#include <linux/kallsyms.h>
#include <linux/buildid.h>
#include <linux/writeback.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/memcontrol.h>
#include <linux/cgroup.h>
#include <linux/tick.h>
#include <linux/sched/isolation.h>
#include <linux/interrupt.h>
#include <linux/taskstats_kern.h>
#include <linux/delayacct.h>
#include <linux/unistd.h>
#include <linux/utsname.h>
#include <linux/rmap.h>
#include <linux/mempolicy.h>
#include <linux/key.h>
#include <linux/debug_locks.h>
#include <linux/debugobjects.h>
#include <linux/lockdep.h>
#include <linux/kmemleak.h>
#include <linux/padata.h>
#include <linux/pid_namespace.h>
#include <linux/device/driver.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/init.h>
#include <linux/signal.h>
#include <linux/idr.h>
#include <linux/kgdb.h>
#include <linux/ftrace.h>
#include <linux/async.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/perf_event.h>
#include <linux/ptrace.h>
#include <linux/pti.h>
#include <linux/blkdev.h>
#include <linux/sched/clock.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/context_tracking.h>
#include <linux/random.h>
#include <linux/moduleloader.h>
#include <linux/list.h>
#include <linux/integrity.h>
#include <linux/proc_ns.h>
#include <linux/io.h>
#include <linux/cache.h>
#include <linux/rodata_test.h>
#include <linux/jump_label.h>
#include <linux/kcsan.h>
#include <linux/init_syscalls.h>
#include <linux/stackdepot.h>
#include <linux/randomize_kstack.h>
#include <linux/pidfs.h>
#include <linux/ptdump.h>
#include <linux/time_namespace.h>
#include <linux/unaligned.h>
#include <linux/vdso_datastore.h>
#include <net/net_namespace.h>

#include <asm/io.h>
#include <asm/setup.h>
#include <asm/sections.h>
#include <asm/cacheflush.h>

#define CREATE_TRACE_POINTS
#include <trace/events/initcall.h>

#include <kunit/test.h>

/* kernel_init 是 PID 1 的执行体，在 rest_init() 中创建，这里前向声明 */
static int kernel_init(void *);

/*
 * 早期启动 IRQ 禁用标志。
 * 此标志为 true 时表示系统处于早期启动阶段：
 *   - 只有 boot CPU 在运行
 *   - IRQ 处于禁用状态
 * 作用：让某些通常不允许在 IRQ 关闭时执行的操作可以在早期启动时执行，
 * 同时防止意外开启 IRQ（开启前必须先清除此标志）。
 * __read_mostly：提示编译器把这个变量放在 Cache 友好的位置，
 * 因为它会被频繁读取但很少写入。
 */
bool early_boot_irqs_disabled __read_mostly;

/*
 * 系统当前状态，供各子系统判断启动阶段。
 * 状态枚举：SYSTEM_BOOTING → SYSTEM_SCHEDULING → SYSTEM_RUNNING 等。
 * EXPORT_SYMBOL 使驱动模块可以读取这个变量。
 */
enum system_states system_state __read_mostly;
EXPORT_SYMBOL(system_state);

/*
 * Boot 命令行参数。
 * Boot loader（ABL）把内核命令行字符串放在特定内存地址，
 * setup_arch() 解析设备树后把它复制到 boot_command_line。
 * 示例命令行：console=ttyMSM0 androidboot.hardware=walleye
 */
#define MAX_INIT_ARGS CONFIG_INIT_ENV_ARG_LIMIT
#define MAX_INIT_ENVS CONFIG_INIT_ENV_ARG_LIMIT

/*
 * 晚期时间初始化函数指针，默认为 NULL。
 * 某些架构需要在大部分系统初始化完成后才能初始化时钟，
 * 在 start_kernel() 末尾调用。
 * __initdata：此变量只在初始化阶段使用，初始化完成后内存会被释放。
 */
void (*__initdata late_time_init)(void);

/* boot_command_line：arch 代码保存的原始命令行，不会被修改，供 /proc/cmdline 使用 */
char __initdata boot_command_line[COMMAND_LINE_SIZE];
/* saved_command_line：最终保存的完整命令行（可能包含 bootconfig 追加的内容） */
/* __ro_after_init：初始化完成后变为只读，防止运行时被意外修改 */
char *saved_command_line __ro_after_init;
unsigned int saved_command_line_len __ro_after_init;
/* static_command_line：用于参数解析的副本，解析过程会就地修改字符串 */
static char *static_command_line;
/* extra_command_line：来自 bootconfig 的额外内核参数 */
static char *extra_command_line;
/* extra_init_args：来自 bootconfig 的额外 init 参数（"init." 前缀的键） */
static char *extra_init_args;

#ifdef CONFIG_BOOT_CONFIG
/* Is bootconfig on command line? */
static bool bootconfig_found;
static size_t initargs_offs;
#else
# define bootconfig_found false
# define initargs_offs 0
#endif

/*
 * execute_command：通过 init= 命令行参数指定的 init 程序路径。
 * 例如：init=/sbin/init 或 init=/bin/bash（单用户恢复模式）。
 */
static char *execute_command;
/*
 * ramdisk_execute_command：从 ramdisk 运行的 init 程序，默认 "/init"。
 * 可通过 rdinit= 命令行参数覆盖。
 * Android 的 /init 就是通过这个路径启动的。
 */
static char *ramdisk_execute_command = "/init";
static bool __initdata ramdisk_execute_command_set;

/*
 * Used to generate warnings if static_key manipulation functions are used
 * before jump_label_init is called.
 */
bool static_key_initialized __read_mostly;
EXPORT_SYMBOL_GPL(static_key_initialized);

/*
 * If set, this is an indication to the drivers that reset the underlying
 * device before going ahead with the initialization otherwise driver might
 * rely on the BIOS and skip the reset operation.
 *
 * This is useful if kernel is booting in an unreliable environment.
 * For ex. kdump situation where previous kernel has crashed, BIOS has been
 * skipped and devices will be in unknown state.
 */
unsigned int reset_devices;
EXPORT_SYMBOL(reset_devices);

static int __init set_reset_devices(char *str)
{
	reset_devices = 1;
	return 1;
}

__setup("reset_devices", set_reset_devices);

static const char *argv_init[MAX_INIT_ARGS+2] = { "init", NULL, };
const char *envp_init[MAX_INIT_ENVS+2] = { "HOME=/", "TERM=linux", NULL, };
static const char *panic_later, *panic_param;

static bool __init obsolete_checksetup(char *line)
{
	const struct obs_kernel_param *p;
	bool had_early_param = false;

	p = __setup_start;
	do {
		int n = strlen(p->str);
		if (parameqn(line, p->str, n)) {
			if (p->early) {
				/* Already done in parse_early_param?
				 * (Needs exact match on param part).
				 * Keep iterating, as we can have early
				 * params and __setups of same names 8( */
				if (line[n] == '\0' || line[n] == '=')
					had_early_param = true;
			} else if (!p->setup_func) {
				pr_warn("Parameter %s is obsolete, ignored\n",
					p->str);
				return true;
			} else if (p->setup_func(line + n))
				return true;
		}
		p++;
	} while (p < __setup_end);

	return had_early_param;
}

/*
 * This should be approx 2 Bo*oMips to start (note initial shift), and will
 * still work even if initially too large, it will just take slightly longer
 */
unsigned long loops_per_jiffy = (1<<12);
EXPORT_SYMBOL(loops_per_jiffy);

static int __init debug_kernel(char *str)
{
	console_loglevel = CONSOLE_LOGLEVEL_DEBUG;
	return 0;
}

static int __init quiet_kernel(char *str)
{
	console_loglevel = CONSOLE_LOGLEVEL_QUIET;
	return 0;
}

early_param("debug", debug_kernel);
early_param("quiet", quiet_kernel);

static int __init loglevel(char *str)
{
	int newlevel;

	/*
	 * Only update loglevel value when a correct setting was passed,
	 * to prevent blind crashes (when loglevel being set to 0) that
	 * are quite hard to debug
	 */
	if (get_option(&str, &newlevel)) {
		console_loglevel = newlevel;
		return 0;
	}

	return -EINVAL;
}

early_param("loglevel", loglevel);

#ifdef CONFIG_BLK_DEV_INITRD
static void * __init get_boot_config_from_initrd(size_t *_size)
{
	u32 size, csum;
	char *data;
	u8 *hdr;
	int i;

	if (!initrd_end)
		return NULL;

	data = (char *)initrd_end - BOOTCONFIG_MAGIC_LEN;
	/*
	 * Since Grub may align the size of initrd to 4, we must
	 * check the preceding 3 bytes as well.
	 */
	for (i = 0; i < 4; i++) {
		if (!memcmp(data, BOOTCONFIG_MAGIC, BOOTCONFIG_MAGIC_LEN))
			goto found;
		data--;
	}
	return NULL;

found:
	hdr = (u8 *)(data - 8);
	size = get_unaligned_le32(hdr);
	csum = get_unaligned_le32(hdr + 4);

	data = ((void *)hdr) - size;
	if ((unsigned long)data < initrd_start) {
		pr_err("bootconfig size %d is greater than initrd size %ld\n",
			size, initrd_end - initrd_start);
		return NULL;
	}

	if (xbc_calc_checksum(data, size) != csum) {
		pr_err("bootconfig checksum failed\n");
		return NULL;
	}

	/* Remove bootconfig from initramfs/initrd */
	initrd_end = (unsigned long)data;
	if (_size)
		*_size = size;

	return data;
}
#else
static void * __init get_boot_config_from_initrd(size_t *_size)
{
	return NULL;
}
#endif

#ifdef CONFIG_BOOT_CONFIG

/* Make an extra command line under given key word */
static char * __init xbc_make_cmdline(const char *key)
{
	struct xbc_node *root;
	char *new_cmdline;
	int ret, len = 0;

	root = xbc_find_node(key);
	if (!root)
		return NULL;

	/* Count required buffer size */
	len = xbc_snprint_cmdline(NULL, 0, root);
	if (len <= 0)
		return NULL;

	new_cmdline = memblock_alloc(len + 1, SMP_CACHE_BYTES);
	if (!new_cmdline) {
		pr_err("Failed to allocate memory for extra kernel cmdline.\n");
		return NULL;
	}

	ret = xbc_snprint_cmdline(new_cmdline, len + 1, root);
	if (ret < 0 || ret > len) {
		pr_err("Failed to print extra kernel cmdline.\n");
		memblock_free(new_cmdline, len + 1);
		return NULL;
	}

	return new_cmdline;
}

static int __init bootconfig_params(char *param, char *val,
				    const char *unused, void *arg)
{
	if (strcmp(param, "bootconfig") == 0) {
		bootconfig_found = true;
	}
	return 0;
}

static int __init warn_bootconfig(char *str)
{
	/* The 'bootconfig' has been handled by bootconfig_params(). */
	return 0;
}

static void __init setup_boot_config(void)
{
	static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;
	const char *msg, *data;
	int pos, ret;
	size_t size;
	char *err;

	/* Cut out the bootconfig data even if we have no bootconfig option */
	data = get_boot_config_from_initrd(&size);
	/* If there is no bootconfig in initrd, try embedded one. */
	if (!data)
		data = xbc_get_embedded_bootconfig(&size);

	strscpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
	err = parse_args("bootconfig", tmp_cmdline, NULL, 0, 0, 0, NULL,
			 bootconfig_params);

	if (IS_ERR(err) || !(bootconfig_found || IS_ENABLED(CONFIG_BOOT_CONFIG_FORCE)))
		return;

	/* parse_args() stops at the next param of '--' and returns an address */
	if (err)
		initargs_offs = err - tmp_cmdline;

	if (!data) {
		/* If user intended to use bootconfig, show an error level message */
		if (bootconfig_found)
			pr_err("'bootconfig' found on command line, but no bootconfig found\n");
		else
			pr_info("No bootconfig data provided, so skipping bootconfig");
		return;
	}

	if (size >= XBC_DATA_MAX) {
		pr_err("bootconfig size %ld greater than max size %d\n",
			(long)size, XBC_DATA_MAX);
		return;
	}

	ret = xbc_init(data, size, &msg, &pos);
	if (ret < 0) {
		if (pos < 0)
			pr_err("Failed to init bootconfig: %s.\n", msg);
		else
			pr_err("Failed to parse bootconfig: %s at %d.\n",
				msg, pos);
	} else {
		xbc_get_info(&ret, NULL);
		pr_info("Load bootconfig: %ld bytes %d nodes\n", (long)size, ret);
		/* keys starting with "kernel." are passed via cmdline */
		extra_command_line = xbc_make_cmdline("kernel");
		/* Also, "init." keys are init arguments */
		extra_init_args = xbc_make_cmdline("init");
	}
	return;
}

static void __init exit_boot_config(void)
{
	xbc_exit();
}

#else	/* !CONFIG_BOOT_CONFIG */

static void __init setup_boot_config(void)
{
	/* Remove bootconfig data from initrd */
	get_boot_config_from_initrd(NULL);
}

static int __init warn_bootconfig(char *str)
{
	pr_warn("WARNING: 'bootconfig' found on the kernel command line but CONFIG_BOOT_CONFIG is not set.\n");
	return 0;
}

#define exit_boot_config()	do {} while (0)

#endif	/* CONFIG_BOOT_CONFIG */

early_param("bootconfig", warn_bootconfig);

bool __init cmdline_has_extra_options(void)
{
	return extra_command_line || extra_init_args;
}

/* Change NUL term back to "=", to make "param" the whole string. */
static void __init repair_env_string(char *param, char *val)
{
	if (val) {
		/* param=val or param="val"? */
		if (val == param+strlen(param)+1)
			val[-1] = '=';
		else if (val == param+strlen(param)+2) {
			val[-2] = '=';
			memmove(val-1, val, strlen(val)+1);
		} else
			BUG();
	}
}

/* Anything after -- gets handed straight to init. */
static int __init set_init_arg(char *param, char *val,
			       const char *unused, void *arg)
{
	unsigned int i;

	if (panic_later)
		return 0;

	repair_env_string(param, val);

	for (i = 0; argv_init[i]; i++) {
		if (i == MAX_INIT_ARGS) {
			panic_later = "init";
			panic_param = param;
			return 0;
		}
	}
	argv_init[i] = param;
	return 0;
}

/*
 * Unknown boot options get handed to init, unless they look like
 * unused parameters (modprobe will find them in /proc/cmdline).
 */
static int __init unknown_bootoption(char *param, char *val,
				     const char *unused, void *arg)
{
	size_t len = strlen(param);
	/*
	 * Well-known bootloader identifiers:
	 * 1. LILO/Grub pass "BOOT_IMAGE=...";
	 * 2. kexec/kdump (kexec-tools) pass "kexec".
	 */
	const char *bootloader[] = { "BOOT_IMAGE=", "kexec", NULL };

	/* Handle params aliased to sysctls */
	if (sysctl_is_alias(param))
		return 0;

	repair_env_string(param, val);

	/* Handle bootloader identifier */
	for (int i = 0; bootloader[i]; i++) {
		if (strstarts(param, bootloader[i]))
			return 0;
	}

	/* Handle obsolete-style parameters */
	if (obsolete_checksetup(param))
		return 0;

	/* Unused module parameter. */
	if (strnchr(param, len, '.'))
		return 0;

	if (panic_later)
		return 0;

	if (val) {
		/* Environment option */
		unsigned int i;
		for (i = 0; envp_init[i]; i++) {
			if (i == MAX_INIT_ENVS) {
				panic_later = "env";
				panic_param = param;
			}
			if (!strncmp(param, envp_init[i], len+1))
				break;
		}
		envp_init[i] = param;
	} else {
		/* Command line option */
		unsigned int i;
		for (i = 0; argv_init[i]; i++) {
			if (i == MAX_INIT_ARGS) {
				panic_later = "init";
				panic_param = param;
			}
		}
		argv_init[i] = param;
	}
	return 0;
}

static int __init init_setup(char *str)
{
	unsigned int i;

	execute_command = str;
	/*
	 * In case LILO is going to boot us with default command line,
	 * it prepends "auto" before the whole cmdline which makes
	 * the shell think it should execute a script with such name.
	 * So we ignore all arguments entered _before_ init=... [MJ]
	 */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("init=", init_setup);

static int __init rdinit_setup(char *str)
{
	unsigned int i;

	ramdisk_execute_command = str;
	ramdisk_execute_command_set = true;
	/* See "auto" comment in init_setup */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("rdinit=", rdinit_setup);

#ifndef CONFIG_SMP
static inline void setup_nr_cpu_ids(void) { }
static inline void smp_prepare_cpus(unsigned int maxcpus) { }
#endif

/*
 * We need to store the untouched command line for future reference.
 * We also need to store the touched command line since the parameter
 * parsing is performed in place, and we should allow a component to
 * store reference of name/value for future reference.
 */
static void __init setup_command_line(char *command_line)
{
	size_t len, xlen = 0, ilen = 0;

	if (extra_command_line)
		xlen = strlen(extra_command_line);
	if (extra_init_args) {
		extra_init_args = strim(extra_init_args); /* remove trailing space */
		ilen = strlen(extra_init_args) + 4; /* for " -- " */
	}

	len = xlen + strlen(boot_command_line) + ilen + 1;

	saved_command_line = memblock_alloc_or_panic(len, SMP_CACHE_BYTES);

	len = xlen + strlen(command_line) + 1;

	static_command_line = memblock_alloc_or_panic(len, SMP_CACHE_BYTES);

	if (xlen) {
		/*
		 * We have to put extra_command_line before boot command
		 * lines because there could be dashes (separator of init
		 * command line) in the command lines.
		 */
		strcpy(saved_command_line, extra_command_line);
		strcpy(static_command_line, extra_command_line);
	}
	strcpy(saved_command_line + xlen, boot_command_line);
	strcpy(static_command_line + xlen, command_line);

	if (ilen) {
		/*
		 * Append supplemental init boot args to saved_command_line
		 * so that user can check what command line options passed
		 * to init.
		 * The order should always be
		 * " -- "[bootconfig init-param][cmdline init-param]
		 */
		if (initargs_offs) {
			len = xlen + initargs_offs;
			strcpy(saved_command_line + len, extra_init_args);
			len += ilen - 4;	/* strlen(extra_init_args) */
			strcpy(saved_command_line + len,
				boot_command_line + initargs_offs - 1);
		} else {
			len = strlen(saved_command_line);
			strcpy(saved_command_line + len, " -- ");
			len += 4;
			strcpy(saved_command_line + len, extra_init_args);
		}
	}

	saved_command_line_len = strlen(saved_command_line);
}

/*
 * We need to finalize in a non-__init function or else race conditions
 * between the root thread and the init thread may cause start_kernel to
 * be reaped by free_initmem before the root thread has proceeded to
 * cpu_idle.
 *
 * gcc-3.4 accidentally inlines this function, so use noinline.
 */

static __initdata DECLARE_COMPLETION(kthreadd_done);

/*
 * rest_init()：start_kernel() 的最后一步，创建内核进程并让自身变成 idle。
 *
 * noinline：禁止内联，防止 start_kernel 被 free_initmem 释放前 rest_init 还在栈上。
 * __ref：标记此函数可以在初始化阶段调用 __init 代码。
 * __noreturn：此函数永远不返回（最终进入 idle 循环）。
 *
 * 执行完后系统状态：
 *   PID 0：当前执行流变成 idle 进程（cpu_startup_entry）
 *   PID 1：kernel_init（等待 kthreadd 就绪后执行用户态 init）
 *   PID 2：kthreadd（所有内核线程的父进程）
 */
static noinline void __ref __noreturn rest_init(void)
{
	struct task_struct *tsk; /* 用于临时持有新创建线程的 task_struct 指针 */
	int pid;                 /* 接收 user_mode_thread/kernel_thread 返回的 PID */

	/*
	 * 通知 RCU 调度器即将启动，从此 RCU 进入正常工作模式。
	 * 在此之前 RCU 处于"早期启动"阶段，不支持抢占式 RCU 读侧临界区。
	 */
	rcu_scheduler_starting();

	/*
	 * 必须先创建 init（PID 1），再创建 kthreadd（PID 2），
	 * 这样 PID 编号才正确。
	 * 但 init 会尝试创建内核线程，如果 kthreadd 还不存在就调度 init，
	 * 会触发 OOPS。
	 * 解决方案：先创建 init 但让它阻塞在 kthreadd_done 完成量上，
	 * 等 kthreadd 创建完再释放它。
	 *
	 * user_mode_thread()：创建一个将来会进入用户态的内核线程。
	 * 与 kernel_thread() 的区别：会设置用户态相关的初始状态（信号、寄存器等），
	 * 使线程可以通过 execve() 切换到用户态程序。
	 * CLONE_FS：与父进程共享文件系统信息（根目录、当前目录、umask）。
	 */
	pid = user_mode_thread(kernel_init, NULL, CLONE_FS); /* 创建 PID 1（init），入口为 kernel_init() */

	/*
	 * 把 init 固定在 boot CPU 上运行。
	 * 原因：sched_init_smp() 还没运行，任务迁移功能未就绪，
	 * 如果 init 跑到其他 CPU 上可能出问题。
	 * sched_init_smp() 之后会解除限制，允许 init 在所有非隔离 CPU 上运行。
	 */
	rcu_read_lock();                                          /* 进入 RCU 读侧临界区，防止 task_struct 被释放 */
	tsk = find_task_by_pid_ns(pid, &init_pid_ns);            /* 通过 PID 在初始 PID 命名空间中找到 init 的 task_struct */
	tsk->flags |= PF_NO_SETAFFINITY;                         /* 禁止用户态/外部代码通过 sched_setaffinity() 修改 init 的 CPU 亲和性 */
	set_cpus_allowed_ptr(tsk, cpumask_of(smp_processor_id())); /* 将 init 限制在当前 boot CPU 上运行 */
	rcu_read_unlock();                                        /* 退出 RCU 读侧临界区 */

	/*
	 * 恢复默认 NUMA 内存策略（MPOL_DEFAULT：允许从任意 NUMA 节点分配内存）。
	 * 之前可能因为 NUMA 初始化需要而临时改变了内存策略。
	 */
	numa_default_policy();

	/*
	 * 创建 kthreadd（PID 2）。
	 * kthreadd 是所有内核线程的守护进程，负责代替其他内核代码异步创建内核线程。
	 * 内核代码调用 kthread_create() 实际上是向 kthreadd 的任务队列提交请求，
	 * 再由 kthreadd 调用 kernel_thread() 完成实际创建，从而避免在任意上下文
	 * 直接调用 kernel_thread() 的限制。
	 * CLONE_FS | CLONE_FILES：与父进程共享文件系统信息和文件描述符表。
	 */
	pid = kernel_thread(kthreadd, NULL, NULL, CLONE_FS | CLONE_FILES); /* 创建 PID 2（kthreadd），入口为 kthreadd() */
	rcu_read_lock();                                                    /* 进入 RCU 读侧临界区 */
	/*
	 * 将 kthreadd 的 task_struct 指针保存到全局变量 kthreadd_task，
	 * 供后续 kthread_create() / wake_up_process(kthreadd_task) 使用。
	 *
	 * 为什么需要 rcu_read_lock()：
	 *   find_task_by_pid_ns() 内部通过 pid 哈希表查找进程，最终调用
	 *   pid_task()，后者用 rcu_dereference_check() 遍历 pid->tasks[] hlist。
	 *   该链表由 RCU 保护——写侧（attach_pid/detach_pid）在修改时会用
	 *   hlist_add_head_rcu / hlist_del_rcu，因此读侧必须持有 RCU 读锁才能
	 *   安全地取到链表中的指针，否则可能读到被并发修改中的半更新状态。
	 *
	 * 拿到指针后为何可以在 rcu_read_unlock() 之后直接裸用，指针不会在数据更新时变化：
	 *   RCU 在这里保护的是"pid 哈希表 → pid->tasks[] hlist → task_struct *"
	 *   这条查找路径上的中间结构，而不是 task_struct 对象本身。
	 *   task_struct 遵循"地址不变"原则：内核不会用"分配新对象、RCU 替换旧
	 *   指针"的方式更新进程描述符；调度器对 state/prio/se 等字段的修改全部
	 *   是原地写入。因此一旦拿到指针，其指向的对象地址在进程存活期间永远
	 *   有效，不存在"写侧换指针导致旧指针过期"的问题。
	 *
	 * 为何不需要 get_task_struct() 引用计数：
	 *   kthreadd 是 PID 2，系统运行期间永不退出，task_struct 永不释放，
	 *   持有裸指针是安全的。若保存的是普通进程的指针，则必须先
	 *   get_task_struct() 增加引用计数，使用完毕后 put_task_struct() 释放，
	 *   否则进程退出后指针将悬空。
	 */
	kthreadd_task = find_task_by_pid_ns(pid, &init_pid_ns);   /* 保存 kthreadd 的 task_struct 到全局变量，供
kthread_create() 使用 */
	rcu_read_unlock();                                        /* 退出 RCU 读侧临界区 */

	/*
	 * 将系统状态切换为 SYSTEM_SCHEDULING，表示调度器已可正常工作。
	 * 从此可以开启 might_sleep() 和 smp_processor_id() 的运行时检查。
	 * 之前不能开启是因为 CONFIG_PREEMPTION=y 时 kernel_thread()
	 * 内部会触发 might_sleep() 警告（彼时调度器尚未就绪）。
	 */
	system_state = SYSTEM_SCHEDULING;

	/*
	 * 发出 kthreadd_done 完成量信号，解除 kernel_init（PID 1）的阻塞。
	 * kernel_init() 开头调用了 wait_for_completion(&kthreadd_done)，
	 * 此处 complete() 之后 PID 1 才真正开始执行初始化工作。
	 * 这保证了 PID 1 创建内核线程时 kthreadd（PID 2）已经就绪。
	 */
	complete(&kthreadd_done);

	/*
	 * boot idle 线程（当前执行流，即 swapper/0，PID 0）必须至少调用一次
	 * schedule()，让调度器开始工作，把 CPU 交给就绪队列中的其他线程。
	 * 使用 schedule_preempt_disabled() 而非 schedule()，
	 * 是因为此时抢占仍处于禁用状态（preempt_count > 0）。
	 */
	schedule_preempt_disabled();

	/*
	 * 进入 cpu_idle 循环，当前执行流正式成为 boot CPU 的 idle 进程（PID 0）。
	 * idle 进程在没有其他可运行任务时执行 cpu_relax()/WFI 指令，
	 * 让 CPU 进入低功耗状态直到下一次中断唤醒。
	 * CPUHP_ONLINE 表示 CPU 处于完全在线状态。
	 * 此函数内部是死循环，永不返回，与函数签名 __noreturn 一致。
	 */
	cpu_startup_entry(CPUHP_ONLINE);
}

/* Check for early params. */
static int __init do_early_param(char *param, char *val,
				 const char *unused, void *arg)
{
	const struct obs_kernel_param *p;

	for (p = __setup_start; p < __setup_end; p++) {
		if (p->early && parameq(param, p->str)) {
			if (p->setup_func(val) != 0)
				pr_warn("Malformed early option '%s'\n", param);
		}
	}
	/* We accept everything at this stage. */
	return 0;
}

void __init parse_early_options(char *cmdline)
{
	parse_args("early options", cmdline, NULL, 0, 0, 0, NULL,
		   do_early_param);
}

/* Arch code calls this early on, or if not, just before other parsing. */
void __init parse_early_param(void)
{
	static int done __initdata;
	static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;

	if (done)
		return;

	/* All fall through to do_early_param. */
	strscpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
	parse_early_options(tmp_cmdline);
	done = 1;
}

void __init __weak arch_post_acpi_subsys_init(void) { }

// 空函数体——适用于不需要显式设置 CPU ID 的架构
// 各架构按需提供强符号实现，链接时自动替换弱符号
/*
┌─────────┬─────────────────────────────────┬─────────────────────────────────────────────────────────┐
│  架构    │            实现位置              │                        主要工作                          │
├─────────┼─────────────────────────────────┼─────────────────────────────────────────────────────────┤
│ arm64   │ arch/arm64/kernel/setup.c:153   │ 读取 MPIDR_EL1 寄存器，建立逻辑 CPU 0 到物理 CPU 的映射      │
├─────────┼─────────────────────────────────┼─────────────────────────────────────────────────────────┤
│ arm     │ arch/arm/kernel/setup.c:596     │ 读取 MPIDR 寄存器，初始化 cpu_logical_map                  │
├─────────┼─────────────────────────────────┼─────────────────────────────────────────────────────────┤
│ s390    │ arch/s390/kernel/smp.c:974      │ 读取 CPU 地址，初始化 cpu_number                           │
├─────────┼─────────────────────────────────┼─────────────────────────────────────────────────────────┤
│ riscv   │ arch/riscv/kernel/smp.c:59      │ 读取 hart ID，初始化逻辑/物理 CPU 映射                      │
├─────────┼─────────────────────────────────┼─────────────────────────────────────────────────────────┤
│ sparc64 │ arch/sparc/kernel/smp_64.c:1206 │ 类似工作                                                  │
└─────────┴─────────────────────────────────┴─────────────────────────────────────────────────────────┘
*/
void __init __weak smp_setup_processor_id(void)
{
}

void __init __weak smp_prepare_boot_cpu(void)
{
}

# if THREAD_SIZE >= PAGE_SIZE
void __init __weak thread_stack_cache_init(void)
{
}
#endif

void __init __weak poking_init(void) { }

void __init __weak pgtable_cache_init(void) { }

void __init __weak trap_init(void) { }

bool initcall_debug;
core_param(initcall_debug, initcall_debug, bool, 0644);

#ifdef TRACEPOINTS_ENABLED
static void __init initcall_debug_enable(void);
#else
static inline void initcall_debug_enable(void)
{
}
#endif

#ifdef CONFIG_RANDOMIZE_KSTACK_OFFSET
DEFINE_STATIC_KEY_MAYBE_RO(CONFIG_RANDOMIZE_KSTACK_OFFSET_DEFAULT,
			   randomize_kstack_offset);
DEFINE_PER_CPU(struct rnd_state, kstack_rnd_state);

static int __init random_kstack_init(void)
{
	prandom_seed_full_state(&kstack_rnd_state);
	return 0;
}
late_initcall(random_kstack_init);

static int __init early_randomize_kstack_offset(char *buf)
{
	int ret;
	bool bool_result;

	ret = kstrtobool(buf, &bool_result);
	if (ret)
		return ret;

	if (bool_result)
		static_branch_enable(&randomize_kstack_offset);
	else
		static_branch_disable(&randomize_kstack_offset);
	return 0;
}
early_param("randomize_kstack_offset", early_randomize_kstack_offset);
#endif

static void __init print_unknown_bootoptions(void)
{
	char *unknown_options;
	char *end;
	const char *const *p;
	size_t len;

	if (panic_later || (!argv_init[1] && !envp_init[2]))
		return;

	/*
	 * Determine how many options we have to print out, plus a space
	 * before each
	 */
	len = 1; /* null terminator */
	for (p = &argv_init[1]; *p; p++) {
		len++;
		len += strlen(*p);
	}
	for (p = &envp_init[2]; *p; p++) {
		len++;
		len += strlen(*p);
	}

	unknown_options = memblock_alloc(len, SMP_CACHE_BYTES);
	if (!unknown_options) {
		pr_err("%s: Failed to allocate %zu bytes\n",
			__func__, len);
		return;
	}
	end = unknown_options;

	for (p = &argv_init[1]; *p; p++)
		end += sprintf(end, " %s", *p);
	for (p = &envp_init[2]; *p; p++)
		end += sprintf(end, " %s", *p);

	/* Start at unknown_options[1] to skip the initial space */
	pr_notice("Unknown kernel command line parameters \"%s\", will be passed to user space.\n",
		&unknown_options[1]);
	memblock_free(unknown_options, len);
}

static void __init early_numa_node_init(void)
{
// 不开启 CONFIG_USE_PERCPU_NUMA_NODE_ID 时：整个函数体为空，cpu_to_node 由架构自己的方式实现（如查静态表）
#ifdef CONFIG_USE_PERCPU_NUMA_NODE_ID
// 架构已定义 cpu_to_node 宏时：也跳过，说明架构有更高效的实现
#ifndef cpu_to_node
	int cpu;

	/* The early_cpu_to_node() should be ready here. */
	for_each_possible_cpu(cpu)
		// 将每个 CPU 所属的 NUMA 节点号，写入该 CPU 的 per-cpu 变量 numa_node
		/*
			写完之后，cpu_to_node(cpu) 和 numa_node_id() 才能正常工作：

			// 这两个函数内部直接读 per-cpu(numa_node)
			cpu_to_node(cpu)   // 查某个 CPU 属于哪个节点
			numa_node_id()     // 查当前 CPU 属于哪个节点（最快路径）

			调度器、内存分配器等大量依赖这两个函数来做 NUMA 亲和性决策：

			分配内存时：优先从 numa_node_id() 返回的节点分配
			进程迁移时：尽量让进程跑在离其内存近的 CPU 上
		*/
		set_cpu_numa_node(cpu, early_cpu_to_node(cpu));
#endif
#endif
}

#define KERNEL_CMDLINE_PREFIX		"Kernel command line: "
#define KERNEL_CMDLINE_PREFIX_LEN	(sizeof(KERNEL_CMDLINE_PREFIX) - 1)
#define KERNEL_CMDLINE_CONTINUATION	" \\"
#define KERNEL_CMDLINE_CONTINUATION_LEN	(sizeof(KERNEL_CMDLINE_CONTINUATION) - 1)

#define MIN_CMDLINE_LOG_WRAP_IDEAL_LEN	(KERNEL_CMDLINE_PREFIX_LEN + \
					 KERNEL_CMDLINE_CONTINUATION_LEN)
#define CMDLINE_LOG_WRAP_IDEAL_LEN	(CONFIG_CMDLINE_LOG_WRAP_IDEAL_LEN > \
					 MIN_CMDLINE_LOG_WRAP_IDEAL_LEN ? \
					 CONFIG_CMDLINE_LOG_WRAP_IDEAL_LEN : \
					 MIN_CMDLINE_LOG_WRAP_IDEAL_LEN)

#define IDEAL_CMDLINE_LEN		(CMDLINE_LOG_WRAP_IDEAL_LEN - KERNEL_CMDLINE_PREFIX_LEN)
#define IDEAL_CMDLINE_SPLIT_LEN		(IDEAL_CMDLINE_LEN - KERNEL_CMDLINE_CONTINUATION_LEN)

/**
 * print_kernel_cmdline() - Print the kernel cmdline with wrapping.
 * @cmdline: The cmdline to print.
 *
 * Print the kernel command line, trying to wrap based on the Kconfig knob
 * CONFIG_CMDLINE_LOG_WRAP_IDEAL_LEN.
 *
 * Wrapping is based on spaces, ignoring quotes. All lines are prefixed
 * with "Kernel command line: " and lines that are not the last line have
 * a " \" suffix added to them. The prefix and suffix count towards the
 * line length for wrapping purposes. The ideal length will be exceeded
 * if no appropriate place to wrap is found.
 *
 * Example output if CONFIG_CMDLINE_LOG_WRAP_IDEAL_LEN is 40:
 *   Kernel command line: loglevel=7 \
 *   Kernel command line: init=/sbin/init \
 *   Kernel command line: root=PARTUUID=8c3efc1a-768b-6642-8d0c-89eb782f19f0/PARTNROFF=1 \
 *   Kernel command line: rootwait ro \
 *   Kernel command line: my_quoted_arg="The \
 *   Kernel command line: quick brown fox \
 *   Kernel command line: jumps over the \
 *   Kernel command line: lazy dog."
 */
static void __init print_kernel_cmdline(const char *cmdline)
{
	size_t len;

	/* Config option of 0 or anything longer than the max disables wrapping */
	if (CONFIG_CMDLINE_LOG_WRAP_IDEAL_LEN == 0 ||
	    IDEAL_CMDLINE_LEN >= COMMAND_LINE_SIZE - 1) {
		pr_notice("%s%s\n", KERNEL_CMDLINE_PREFIX, cmdline);
		return;
	}

	len = strlen(cmdline);
	while (len > IDEAL_CMDLINE_LEN) {
		const char *first_space;
		const char *prev_cutoff;
		const char *cutoff;
		int to_print;
		size_t used;

		/* Find the last ' ' that wouldn't make the line too long */
		prev_cutoff = NULL;
		cutoff = cmdline;
		while (true) {
			cutoff = strchr(cutoff + 1, ' ');
			if (!cutoff || cutoff - cmdline > IDEAL_CMDLINE_SPLIT_LEN)
				break;
			prev_cutoff = cutoff;
		}
		if (prev_cutoff)
			cutoff = prev_cutoff;
		else if (!cutoff)
			break;

		/* Find the beginning and end of the string of spaces */
		first_space = cutoff;
		while (first_space > cmdline && first_space[-1] == ' ')
			first_space--;
		to_print = first_space - cmdline;
		while (*cutoff == ' ')
			cutoff++;
		used = cutoff - cmdline;

		/* If the whole string is used, break and do the final printout */
		if (len == used)
			break;

		if (to_print)
			pr_notice("%s%.*s%s\n", KERNEL_CMDLINE_PREFIX,
				  to_print, cmdline, KERNEL_CMDLINE_CONTINUATION);

		len -= used;
		cmdline += used;
	}
	if (len)
		pr_notice("%s%s\n", KERNEL_CMDLINE_PREFIX, cmdline);
}

/*
 * start_kernel()：内核 C 代码的真正起点。
 *
 * 由 head.S 中的 __primary_switched() 调用，此时：
 *   - MMU 已开启，虚拟地址空间就绪
 *   - 只有 boot CPU（CPU0）在运行
 *   - 中断处于禁用状态
 *   - 只有最基本的内存可用（memblock）
 *
 * 函数修饰符说明：
 *   asmlinkage：使用 C 调用约定，参数通过栈传递（供汇编调用）
 *   __visible：禁止编译器把此符号优化为内部符号，确保调试器可见
 *   __init：代码在初始化完成后可以被释放
 *   __no_sanitize_address：禁用 KASAN 检测（初始化阶段内存状态特殊）
 *   __noreturn：此函数永不返回（最终调用 rest_init() 进入 idle）
 *   __no_stack_protector：禁用栈保护（初始化阶段栈 canary 尚未就绪）
 */
asmlinkage __visible __init __no_sanitize_address __noreturn __no_stack_protector
void start_kernel(void)
{
	char *command_line;
	char *after_dashes;

	/*
	 * ── 第一批：最早期初始化（无任何子系统可用）──────────────────
	 */

	/* 在 init_task（PID 0 的 task_struct）栈末尾写入魔数
	 * 用于运行时检测栈溢出（如果魔数被覆盖则触发 panic） */
	// task_struct 是静态编译到内核镜像中的（不是动态分配的），此时处于 start_kernel() 最早期，内存分配器尚未就绪，所以只能操作这个静态的 init_task，而不是动态分配的进程
	set_task_stack_end_magic(&init_task);

	/* 设置 CPU ID（某些架构需要在最早期识别自己的 CPU 编号） */
	smp_setup_processor_id();

	/* 初始化调试对象跟踪系统（检测对象生命周期错误） */
	debug_objects_early_init();

	/* 记录内核编译 ID（用于崩溃报告中唯一标识内核版本） */
	init_vmlinux_build_id();

	/* cgroup 早期初始化（在内存分配器就绪前做最小化设置） */
	cgroup_init_early();

	/* 显式关闭中断，确保后续初始化不被打断 */
	local_irq_disable();
	early_boot_irqs_disabled = true;

	/*
	 * ── 第二批：中断仍禁用，完成必要设置 ──────────────────────────
	 */

	/* 标记 boot CPU 为在线状态，初始化 CPU 位图 */
	boot_cpu_init();

	/* 初始化高端内存（highmem）的页地址哈希表（32位系统需要） */
	/*
		背景：highmem 问题

		32 位系统的内核虚拟地址空间只有 1GB（通常），无法把所有物理内存永久映射进来。超出这个范围的物理内存叫 highmem，内核需要用时临时映射（kmap），用完再解除。

		这就带来一个问题：给定一个 highmem 的 struct page *，怎么快速查到它当前被临时映射到哪个虚拟地址？

		---
		page_address_init 做的事

		初始化一张哈希表 page_address_htable，结构：

		page_address_htable[hash(page)] → 链表 → {page*, 虚拟地址} → ...

		page_address_init 只是把这张表的每个槽初始化：

		for (i = 0; i < ARRAY_SIZE(page_address_htable); i++) {
			INIT_LIST_HEAD(&page_address_htable[i].lh);   /
			spin_lock_init(&page_address_htable[i].lock); // 初始化自旋锁
		}

		之后 kmap() 建立临时映射时向表中插入记录，page_addrunmap() 解除映射时从表中删除。
	*/
	/*
		是什么限制了1G

		32 位地址线本身，总共只有 2³² = 4GB 可寻址空间，内核和用户进程必须共享这 4GB 虚拟地址空间。

		---
		经典的 3G/1G 分割

		Linux 在 32 位上默认这样划分：

		0x00000000 ~ 0xBFFFFFFF   3GB   用户空间
		0xC0000000 ~ 0xFFFFFFFF   1GB   内核空间

		这个分割点由 PAGE_OFFSET（通常 0xC0000000）决定，可以编译时调整（有 2G/2G、1G/3G 等变体），但总和永远是 4GB。

		---
		为什么内核要和用户共享同一个 4GB

		因为 32 位 CPU 的 MMU 只有 32 根地址线，任何时刻能寻址的虚拟地址范围就是 0 ~ 4GB，没有更多了。

		内核和用户进程运行在同一个 CPU 上，切换时为了避免刷 TLB（代价极高），Linux 选择让内核页表常驻在每个进程的地址空间高端，两者共用一张页表，系统调用进入内核时不需要切换 CR3。

		---
		1GB 内核空间放不下所有物理内存时

		假设物理内存有 2GB：

		内核直接映射区：0xC0000000 ~ 0xFFFFFFFF = 1GB
						只能永久映射 1GB 物理内存

		超出的物理内存 → highmem
						需要 kmap() 临时映射，用完再释放
						page_address_htable 就是为了追踪这些临时映射

		---
		64 位为何没有这个问题

		64 位地址线有 48 位可用（实际寻址 256TB），内核和用户各自拥有 128TB，物理内存再大也能全部直接映射进内核空间，highmem 机制完全消失。这也是为什么现代 64 位 Linux 上 page_address_htable 实际上永远空着。
	 */
	page_address_init();

	/* 打印内核版本横幅（就是 dmesg 开头那行 "Linux version ..."） */
	pr_notice("%s", linux_banner);

	/*
	 * 架构相关的核心初始化，ARM64 在这里做：
	 *   - 解析设备树（DTB），识别内存范围、CPU 数量
	 *   - 初始化 memblock（早期内存分配器）
	 *   - 建立完整内核页表（替换 head.S 的临时页表）
	 *   - 解析命令行到 command_line
	 * 这是 start_kernel 中最重要的单个调用。
	 */
	setup_arch(&command_line);

	/* 内存管理核心早期初始化（页分配器就绪前的准备工作） */
	mm_core_init_early();

	/* 初始化 static key（一种高效的运行时开关，用汇编 NOP/JMP 实现） */
	/*
		静态分支

		内核中大量存在这样的代码：

		if (static_branch_unlikely(&some_feature))
			do_something();

		这个 if 在运行时极少改变（如某功能默认关闭），普通条件跳转会浪费 CPU 分支预测资源。

		静态分支的解决方案：直接在机器码层面打补丁：
		- 功能关闭时：该位置是一条 NOP（什么都不做，直接往下执行）
		- 功能开启时：该位置被 patch 成 JMP（跳转到功能代码）

		CPU 执行到这里时，看到的就是一条固定指令，完全没有分支预测的开销。
	*/
	jump_label_init();

	/* 初始化 static call（比函数指针更快的间接调用机制） */
	/* static call
		static call 是静态分支机制的"函数指针版本"。

		---
		问题背景

		普通函数指针调用：

		void (*func_ptr)(void) = default_func;

		func_ptr();   // 每次都要：读指针变量 → 间接跳转
					// 间接跳转 CPU 无法预测目标，流水线代价大
					// 还破坏了 CFI（控制流完整性）安全机制

		这与静态分支面对的问题类似——调用目标在运行时极少改变，但每次调用都承担间接跳转的开销。

		---
		static call 的解决方案

		和静态分支一样，直接在代码段原地 patch 机器码：

		DEFINE_STATIC_CALL(my_func, default_func);

		static_call(my_func)();   // 编译后是一条直接 CALL 指令
								// call default_func  ← 直接调用，CPU 可预测

		切换目标时：

		static_call_update(my_func, new_func);
		// 找到所有 call 指令的位置
		// 把 call default_func 改写成 call new_func

		---
		与静态分支的对比

		┌──────────────┬──────────────────────────┬─────────────────────────────────┐
		│              │  static key（静态分支）    │           static call           │
		├──────────────┼──────────────────────────┼─────────────────────────────────┤
		│ 解决的问题     │ if (flag) 的条件判断开销   │ 函数指针间接调用开销               │
		├──────────────┼──────────────────────────┼─────────────────────────────────┤
		│ patch 的内容  │ NOP ↔ JMP                │ CALL target 的目标地址            │
		├──────────────┼──────────────────────────┼─────────────────────────────────┤
		│ 典型场景      │ 特性开关                   │ 可替换的函数实现（如 paravirt）    │
		└──────────────┴──────────────────────────┴─────────────────────────────────┘

		---
		典型使用场景

		内核里大量的虚拟化钩子（paravirt ops）、tracepoint 调用、调度器钩子都用 static call：

		// 虚拟化：裸机用 native_xxx，虚拟机用 xen_xxx / kvm_xxx
		DEFINE_STATIC_CALL(pv_tlb_flush, native_flush_tlb);

		// 启动时检测到运行在 Xen 上：
		static_call_update(pv_tlb_flush, xen_flush_tlb);
		// 之后每次 tlb flush 都是直接 call xen_flush_tlb，零间接跳转开销

		start_kernel() 里调用 static_call_init() 就是建立类似 __jump_table 的索引表，为后续 static_call_update() 能快速找到所有需要 patch 的 call 指令做准备。
	*/
	static_call_init();

	/* LSM（Linux 安全模块，如 SELinux）早期初始化 */
	// 初始化 lockdown，进行内核完整性保护
	early_security_init();

	/* 解析 bootconfig（附加在 initramfs 末尾的扩展配置格式） */
	setup_boot_config();

	/* 整理命令行：合并 bootconfig 参数，保存多份副本 */
	setup_command_line(command_line);

	/* 确定系统实际 CPU 数量（从设备树读取） */
	// nr 是 number of 的缩写，是 Linux 内核代码里极常见的前缀，表示"数量"
	// 将 CPU 数量收紧到实际值，之后所有循环用此作上界，避免遍历大量空槽位浪费时间
	setup_nr_cpu_ids();

	/*
	 * 为每个 CPU 分配 per-cpu 变量区域。
	 * per-cpu 变量：每个 CPU 有独立副本，无需加锁，性能极高。
	 * 例如：运行队列、中断计数器都是 per-cpu 变量。
	 */
	/* per-cpu 变量
		内核里用 DEFINE_PER_CPU 声明的变量，每个 CPU 有自己独立的一份：

		DEFINE_PER_CPU(int, cpu_number);          // 每个 CPU 有自己的 cpu_number
		DEFINE_PER_CPU(struct runqueue, runqueues); // 每个 CPU 有自己的运行队列

		访问时用专用宏：

		per_cpu(cpu_number, 0)   // 访问 CPU 0 的副本
		this_cpu_read(cpu_number) // 访问当前 CPU 的副本（最快）

		---
		为什么需要 per-cpu 变量

		普通全局变量：
			所有 CPU 共享 → 需要加锁 → 锁竞争 → 性能差

		per-cpu 变量：
			每个 CPU 独占自己的副本 → 不需要加锁 → 零竞争 → 极快
			还避免了 cache bouncing（多核争抢同一 cache line）
	*/
	setup_per_cpu_areas();

	/* arch 特定的 boot CPU 钩子（ARM64 这里初始化一些 CPU 特性标志） */
	smp_prepare_boot_cpu();	/* arch-specific boot-cpu hooks */

	/* 初始化每个 CPU 的 NUMA 节点 ID（NUMA = 非统一内存访问架构） */
	// 将每个 CPU 所属的 NUMA 节点号，写入该 CPU 的 per-cpu 变量 numa_node
	early_numa_node_init();

	// 将 CPU 状态设置为 CPUHP_ONLINE 状态，表示已完全上线
	// 这里只是补记状态，因为热插拔的状态机还没建立，boot cpu 跳过了整个过程，直接运行的
	boot_cpu_hotplug_init();

	/* 打印完整的内核命令行到 dmesg */
	print_kernel_cmdline(saved_command_line);

	/*
	 * 解析 early_param 参数（console=、loglevel= 等需要最早处理的参数）。
	 * early_param 注册的处理函数在这里调用，早于普通的 __setup 参数。
	 */
	parse_early_param();

	/*
	 * 解析剩余的内核参数。
	 * __start___param / __stop___param：链接器生成的内核参数表边界。
	 * 通过 module_param() 或 __setup() 注册的参数在这里处理。
	 * '--' 之后的参数会传给 init 进程，after_dashes 指向这部分。
	 */
	after_dashes = parse_args("Booting kernel",
				  static_command_line, __start___param,
				  __stop___param - __start___param,
				  -1, -1, NULL, &unknown_bootoption);
	print_unknown_bootoptions();
	if (!IS_ERR_OR_NULL(after_dashes))
		/* '--' 后面的参数作为 init 进程的命令行参数 */
		parse_args("Setting init args", after_dashes, NULL, 0, -1, -1,
			   NULL, set_init_arg);
	if (extra_init_args)
		/* bootconfig 中 "init." 前缀的参数也传给 init */
		parse_args("Setting extra init args", extra_init_args,
			   NULL, 0, -1, -1, NULL, set_init_arg);

	/* 早期随机数初始化（在内存分配器就绪前，用命令行作为熵源） */
	random_init_early(command_line);

	/*
	 * ── 第三批：内存管理初始化 ────────────────────────────────────
	 * 以下调用需要大块 memblock 分配，必须在 page allocator 初始化前完成。
	 */

	/* 分配内核日志缓冲区（dmesg 的存储区域） */
	setup_log_buf(0);

	/* VFS 缓存早期初始化（dcache/inode cache 的哈希表，需要大量内存） */
	vfs_caches_init_early();

	/* 对内核异常表排序（加速异常处理时的查找） */
	// 让后续查找时能用二分搜索（O(log n)）而不是线性扫描，提高页错误处理路径的速度
	/* 内核异常表（extable）
		内核软件层面的一张查找表，专门服务于一种特定 trap 的处理流程——页错误（page fault）中的用户地址访问失败场景
		内核访问用户态内存时（copy_from_user、get_user 等），可能因用户指针无效而触发页错误。但内核不能像用户进程那样直接被 SIGSEGV 杀死，需要一种"出错了能安全继续执行"的机制。

		解决方案：对每个可能出错的访问指令，预先记录一条"修复记录"：

		struct exception_table_entry {
			int insn;    // 可能出错的指令地址（相对偏移）
			int fixup;   // 出错后跳转到哪里继续执行（相对偏移）
			int data;    // 额外数据（如错误处理类型）
		};

		---
		编译时写入的方式

		与 jump_entry 类似，通过内联汇编写入特殊 section：

		// copy_from_user 的某条 load 指令旁边
		asm volatile(
			"1: ldrb %w0, [%1]\n"          // 可能出错的指令，标记为 "1:"
			".pushsection __ex_table\n"    // 切换到异常表 section
			".long 1b - .\n"               // 记录：出错指令在哪
			".long 2f - .\n"               // 记录：出错后跳到哪（fixup 代码）
			".popsection\n"
			...
			"2: mov %w0, #-EFAULT\n"       // fixup：返回错误码
		);

		链接后所有条目合并为 __start___ex_table ~ __stop___ex_table 数组。

		运行时如何使用

		用户地址访问触发页错误
			↓
		CPU 执行指令 → MMU 检测到问题 → CPU 触发 trap（硬件）
                                        ↓
                              跳入异常向量表入口
                                        ↓
                              do_page_fault()（软件）
                                        ↓
                         search_exception_tables(fault_addr)
                                        ↓
                    ┌───────────────────┴───────────────────┐
                  找到                                    找不到
                    ↓                                        ↓
             跳到 fixup 代码                         真正的内核 bug
             返回 -EFAULT                            oops / panic
	*/
	/*
		extable 只是 trap 处理程序内部的一个工具

		do_page_fault() 不是只用 extable，它的完整决策树是：

		do_page_fault()
			↓
		是缺页（valid address，页还没加载）？→ 分配物理页，映射，返回
			↓
		是写时复制（COW）？→ 复制页，映射，返回
			↓
		是内核访问用户地址失败？→ search_exception_tables() → fixup 或 oops
			↓
		是用户态非法访问？→ 发 SIGSEGV 给进程

		extable 只处理其中"内核访问用户地址失败"这一个分支，其他分支有各自的处理逻辑，互不干扰。
	*/
	sort_main_extable();

	/* 初始化异常向量，将异常向量的内存地址写入指定寄存器，之后 CPU 发生任何异常都会自动找到正确的处理入口 */
	/*
		trap（陷阱/异常）

		CPU 硬件层面的概念，指令执行时触发的同步异常，ARM64 叫 exception，x86 叫 trap/fault：

		除零、非法指令、页错误、系统调用、断点...

		触发后 CPU 自动跳转到异常向量表（vectors），由内核的异常处理程序接管。这是硬件机制，由 CPU 架构定义。
	*/
	/*
		arm64 不需要在这里初始化异常向量
		arm64 的异常向量表（vectors）在更早的阶段就已设置好了——在汇编启动代码 head.S 里
		// arch/arm64/kernel/head.S
		adr_l   x0, vectors     // vectors 是 entry.S 中定义的异常向量表
		msr     vbar_el1, x0    // 写入 VBAR_EL1 寄存器，CPU 从这里取异常入口

		VBAR_EL1（Vector Base Address Register）在进入 start_kernel() 之前就已经指向正确的向量表了，不需要在 trap_init() 里再做任何事。
	*/
	trap_init();

	/*
	 * 内存管理核心初始化：
	 *   - 把 memblock 管理的内存移交给 buddy system（正式的页分配器）
	 *   - 初始化 slab/slub 分配器（kmalloc 的基础）
	 *   - 建立内存 zone（DMA/Normal/HighMem）
	 * 完成后 kmalloc() 可以使用。
	 */
	/*
		buddy system（伙伴系统）—— 物理页分配器

		管理物理内存，分配单位是页（4KB），只能分配 2 的幂次个连续页：

		free_area[0]  → 1页  (4KB)  的空闲链表
		free_area[1]  → 2页  (8KB)  的空闲链表
		free_area[2]  → 4页  (16KB) 的空闲链表
		...
		free_area[10] → 1024页(4MB) 的空闲链表

		名字来自"伙伴"机制：分配出去的块释放时，若相邻的"伙伴块"也空闲，两者合并成更大的块，防止碎片化。

		局限：最小分配单位是 4KB，申请 100 字节也给你 4KB，浪费严重。
	*/
	/*
		slab/slub —— 小对象分配器

		建立在 buddy system 之上，解决小对象分配问题：

		buddy system 分配几页大块内存
			↓
		slab/slub 把大块切成固定大小的小对象
			↓
		内核代码 kmalloc(64) → 从对应大小的 slab 缓存取一个对象

		slab 是原始实现（复杂），slub 是简化重写版（现代系统默认用 slub）。两者接口相同，实现不同。

		每种常用对象都有专用缓存（如 struct task_struct、struct inode），分配时从缓存取，释放时还回缓存，不真正归还给 buddy，下次直接复用，极快。
	*/
	mm_core_init();

	/* 初始化 maple tree（内核的 B-tree 实现，用于 VMA（虚拟内存区域） 管理） */
	maple_tree_init();

	/* 初始化代码修补机制（用于 ftrace、kprobes 等运行时代码插桩） */
	poking_init();

	/* 初始化 ftrace（内核函数跟踪框架） */
	ftrace_init();

	/* 初始化早期跟踪（trace_printk 从此可用） */
	early_trace_init();

	/*
	 * ── 第四批：调度器初始化 ──────────────────────────────────────
	 * 必须在任何中断（包括时钟中断）启动前初始化调度器。
	 * SMP 完整拓扑在 smp_init() 时才建立，但此时调度器已可工作。
	 */
	sched_init();

	/* 健康检查：如果中断在这里是开启的，说明之前某处错误地开启了 */
	if (WARN(!irqs_disabled(),
		 "Interrupts were enabled *very* early, fixing it\n"))
		local_irq_disable();

	/* 初始化 radix tree（内核的基数树，page cache 等数据结构的基础） */
	/*
		Radix tree（基数树）是一种用整数键做快速索引的树形结构。

		---
		基本原理

		将键（通常是页号或偏移量）按固定位数分层，每层是一个数组索引：

		键值 = 0xABCDEF（24位，每层8位）

		根节点[0xAB] → 中间节点[0xCD] → 叶节点[0xEF] → 值

		查找复杂度 O(k)，k 是键的层数，与存储的条目数量无关。

		---
		内核里的主要用途

		页缓存（Page Cache）

		这是 radix tree 最核心的用途——每个文件的 address_space 用一棵 radix tree 管理所有已缓存的页：

		文件偏移（页号）→ struct page *

		read(file, offset):
			页号 = offset / PAGE_SIZE
			page = radix_tree_lookup(&mapping->page_tree, 页号)
			若命中 → 直接读缓存
			若未命中 → 从磁盘读入，插入树中

		文件可能有数十亿个页，radix tree 能在 O(log n) 时间内按页号精确查找，比链表快得多。

		IDR（ID 分配器）

		进程 PID、文件描述符编号等整数 ID 的分配和查找，底层也用 radix tree（IDR 是 radix tree 的封装）。

		---
		与 maple tree 的关系

		内核正在逐步用 XArray（radix tree 的现代封装）和 maple tree 替代旧的 radix tree：

		旧：page cache 用 radix_tree
		新：page cache 用 XArray（内部还是类 radix tree 结构）

		旧：VMA 管理用红黑树
		新：VMA 管理用 maple tree

		radix_tree_init 仍然存在是因为 XArray 底层共享了 radix tree 的节点和 slab 缓存，两者并非完全独立。

	*/
	radix_tree_init();

	/* 初始化 housekeeping CPU（隔离实时任务和普通任务的机制） */
	/*
		背景：CPU 隔离（isolcpus / nohz_full）

		通过内核命令行参数可以隔离部分 CPU，让它们专跑实时任务或高性能计算，避免内核噪声：

		isolcpus=2,3       # CPU 2,3 不参与调度域，不跑内核后台任务
		nohz_full=2,3      # CPU 2,3 关闭周期性时钟中断（tick）

		"housekeeping CPU"就是剩余的非隔离 CPU，负责承担所有内核杂务：
		定时器处理、RCU 回调、工作队列、内核线程、中断亲和等。
	*/
	housekeeping_init();

	/*
	 * 初始化工作队列（workqueue）早期框架。
	 * 此时只能创建队列和排队工作项，实际执行要等 workqueue_init()。
	 * workqueue 是内核的异步执行机制，大量驱动依赖它。
	 */
	/*
		workqueue 是内核内部的延迟执行机制，用于把工作推迟到稍后执行：

		// 驱动、子系统等内核代码提交工作
		INIT_WORK(&my_work, my_handler);
		queue_work(system_wq, &my_work);
		// → 稍后由 worker kthread 在合适时机执行 my_handler()

		典型使用场景：
		- 中断处理程序把耗时工作推迟到进程上下文（可以睡眠）
		- 驱动的异步初始化、热插拔事件处理
		- 网络子系统的报文处理
		- 文件系统的后台刷盘

		与 进程调度 完全独立

		用户进程和内核线程的调度由**调度器（scheduler）**管理，与 workqueue 无关：

		进程调度体系：
			struct task_struct（每个进程/线程）
			→ CFS/RT/DL 调度器
			→ 运行队列（runqueue）
			→ CPU 执行

		workqueue 体系：
			struct work_struct（一个工作项）
			→ worker_pool（工作池）
			→ worker kthread（工作线程，本质上也是 task_struct）
			→ CPU 执行

		worker kthread 本身也是一个普通的内核线程，受调度器管理，但它只是 workqueue 机制的载体，调度器并不知道"这是 workqueue 的线程"。

	*/
	workqueue_init_early();

	/*
	 * 初始化 RCU（Read-Copy-Update）。
	 * RCU 是内核最重要的无锁同步机制：
	 *   读者：无锁，极快
	 *   写者：复制-修改-替换，等待读者完成后释放旧版本
	 * 几乎所有内核子系统都依赖 RCU。
	 */
	/*
		RCU（Read-Copy-Update）是内核的一种无锁并发读取机制。

		---
		解决的问题

		多个 CPU 同时读取一个数据结构，偶尔有写者修改它。用普通锁：

		// 读者也要加锁，即使只是读
		read_lock(&lock);
		p = list_head->next;
		read_unlock(&lock);

		高并发场景下读锁竞争严重，性能差。

		---
		RCU 的核心思想

		读者完全不加锁，写者遵循"复制-修改-替换"的规则：

		// 写者：不修改原数据，而是复制一份新的
		new_node = kmalloc(...);
		*new_node = *old_node;      // 复制
		new_node->value = new_val;  // 修改副本
		rcu_assign_pointer(p, new_node);  // 原子替换指针

		// 等所有正在读旧数据的 CPU 读完（宽限期）
		synchronize_rcu();

		// 释放旧数据
		kfree(old_node);

		读者：

		rcu_read_lock();            // 不加真正的锁，只是标记"我在读"
		p = rcu_dereference(ptr);  // 读指针
		use(p->value);
		rcu_read_unlock();          // 标记读完

		---
		宽限期（Grace Period）

		写者替换指针后不能立即释放旧数据——可能有 CPU 还在读旧数据。

		RCU 等待所有 CPU 都经历了一次上下文切换（或执行了 RCU 静止点），此时可以确认没有任何 CPU 还持有旧数据的引用，才安全释放：

		CPU 0：读旧数据 ────────────────────┐ 上下文切换
		CPU 1：          读旧数据 ──────────┘ 上下文切换
		CPU 2：                    写者替换指针，等待宽限期结束
													↓
												kfree(旧数据)

		---
		两种实现

		┌────────┬───────────────────────────────┬───────────────────────────────┐
		│        │ tiny RCU（kernel/rcu/tiny.c）  │ tree RCU（kernel/rcu/tree.c） │
		├────────┼───────────────────────────────┼───────────────────────────────┤
		│ 适用    │ 单 CPU（UP）系统                │ SMP 多 CPU 系统               │
		├────────┼───────────────────────────────┼───────────────────────────────┤
		│ 复杂度  │ 极简，几百行                    │ 复杂，处理多 CPU 宽限期协调       │
		├────────┼───────────────────────────────┼───────────────────────────────┤
		│ 宽限期  │ 简单等待                        │ 分层树形结构跟踪各 CPU 状态      │
		└────────┴───────────────────────────────┴───────────────────────────────┘

		---
		典型使用场景

		网络路由表     → 查路由极频繁，RCU 读，路由更新时写
		进程列表       → 频繁遍历，任务退出时写
		文件系统 dentry缓存 → 频繁查找，目录变化时写
		设备驱动注册表 → 频繁查设备，注册/注销时写

		总结：RCU 用读者零开销换取写者的复杂性，适合读多写少的共享数据结构。
	*/
	rcu_init();

	/* 初始化 kvfree_rcu（延迟释放内存的 RCU 版本） */
	/*
		kfree_rcu() / kvfree_rcu() 的批处理加速机制，是建立在 RCU 之上的一个优化层。

		背景：kfree_rcu() 的问题

		kfree_rcu(ptr, rcu_head);
		// 等待当前 GP 结束后调用 kfree(ptr)

		每个 kfree_rcu() 调用都向 RCU 注册一个回调，大量调用时（如网络包处理）会产生海量回调，消耗大量 CPU 时间来处理回调链表。

		解决方案：把多个待释放指针攒成批次，GP 结束后一次性批量释放，减少回调注册次数。
	*/
	kvfree_rcu_init();

	/* 初始化内核跟踪框架（trace events 从此可用） */
	trace_init();

	if (initcall_debug)
		/* 开启 initcall 调试：每个初始化函数的耗时都会打印到 dmesg */
		initcall_debug_enable();

	/* 初始化上下文跟踪（用于 NOHZ 模式，精确跟踪用户/内核态切换） */
	context_tracking_init();

	/*
	 * ── 第五批：中断系统初始化 ────────────────────────────────────
	 */

	/* 早期 IRQ 初始化（建立 IRQ 描述符数组） */
	// 实现在 kernel/irq/irqdesc.c
	early_irq_init();

	/*
	 * 初始化中断控制器（ARM64 上是 GIC - Generic Interrupt Controller）。
	 * 之后硬件中断可以被接收（但 CPU 中断还是关闭的）。
	 */
	init_IRQ();

	/* 初始化时钟事件框架（tick = 内核的基本时间单位） */
	tick_init();

	/* 初始化 RCU 的 NOHZ（无滴答）模式 */
	rcu_init_nohz();

	/* 初始化定时器（timer wheel，低精度定时器） */
	timers_init();

	/* 初始化 SRCU（Sleepable RCU，允许读者睡眠的 RCU 变体） */
	srcu_init();

	/* 初始化高精度定时器（hrtimer，纳秒级精度） */
	hrtimers_init();

	/* 初始化软中断（softirq）系统（网络、块设备等用软中断处理延迟工作） */
	softirq_init();

	/* 初始化 VDSO 数据页（Virtual Dynamic Shared Object，gettimeofday 加速） */
	vdso_setup_data_pages();

	/* 初始化时间保持系统（wall clock，单调时钟等） */
	timekeeping_init();

	/*
	 * 架构相关的时钟初始化（ARM64 上初始化 arch timer，即 Generic Timer）。
	 * 之后时钟中断可以正常工作，jiffies 开始计时。
	 */
	time_init();

	/* 完整的随机数初始化（依赖时钟，时钟就绪后才能做） */
	random_init();

	/* 初始化 KFENCE（内核内存安全性检测，采样式检测堆越界） */
	kfence_init();

	/* 初始化栈 canary（每个进程栈的保护值，检测栈溢出） */
	boot_init_stack_canary();

	/* 初始化性能事件框架（perf，硬件性能计数器接口） */
	perf_event_init();

	/* 初始化性能分析框架（profile，用于 oprofile 等工具） */
	profile_init();

	/* 初始化跨 CPU 函数调用机制（smp_call_function 的基础） */
	call_function_init();

	WARN(!irqs_disabled(), "Interrupts were enabled early\n");

	/*
	 * ── 关键时刻：开启中断 ────────────────────────────────────────
	 * 从这里开始系统可以响应外部中断（时钟、设备等）。
	 * 在此之前所有工作都在关中断状态下完成。
	 */
	early_boot_irqs_disabled = false;
	local_irq_enable();

	/* 完成 slab 分配器的后期初始化（中断开启后才能完成的部分） */
	kmem_cache_init_late();

	/*
	 * 初始化控制台（串口、framebuffer 等输出设备）。
	 * 注意：此时 PCI 等总线还没初始化完成，console_init() 必须能处理这种情况。
	 * 尽管如此，我们需要尽早有输出，方便调试启动问题。
	 */
	console_init();

	/* 如果命令行参数太多，这里会 panic（之前只是记录，现在才真正报错） */
	if (panic_later)
		panic("Too many boot %s vars at `%s'", panic_later,
		      panic_param);

	/* 初始化 lockdep（运行时死锁检测，仅 debug 内核） */
	lockdep_init();

	/* 锁机制自测（需要中断开启，因为要测试中断上下文的锁行为） */
	locking_selftest();

#ifdef CONFIG_BLK_DEV_INITRD
	/* 检查 initrd 是否被内存布局覆盖，如果是则禁用 initrd */
	if (initrd_start && !initrd_below_start_ok &&
	    page_to_pfn(virt_to_page((void *)initrd_start)) < min_low_pfn) {
		pr_crit("initrd overwritten (0x%08lx < 0x%08lx) - disabling it.\n",
		    page_to_pfn(virt_to_page((void *)initrd_start)),
		    min_low_pfn);
		initrd_start = 0;
	}
#endif

	/*
	 * ── 第六批：各子系统完整初始化 ───────────────────────────────
	 */

	/* 为每个 CPU 分配页集合（用于加速页分配，减少全局锁争用） */
	setup_per_cpu_pageset();

	/* 初始化 NUMA 内存策略（控制内存从哪个 NUMA 节点分配） */
	/* 背景
		在进程申请内存（malloc → mmap → 缺页中断）时，决定从哪个 NUMA 节点的物理 DRAM 上分配这个物理页。
		硬件层面的现实

		┌─────────────────────────────────────────────────────┐
		│                    主板                              │
		│                                                     │
		│  ┌──────────────────┐    ┌──────────────────┐       │
		│  │   CPU Socket 0   │    │   CPU Socket 1   │       │
		│  │  ┌────────────┐  │    │  ┌────────────┐  │       │
		│  │  │ Core 0-15  │  │    │  │ Core 16-31 │  │       │
		│  │  │  L1/L2/L3  │  │    │  │  L1/L2/L3  │  │       │
		│  │  └────────────┘  │    │  └────────────┘  │       │
		│  │  内存控制器 IMC   │    │  内存控制器 IMC  │       │
		│  └────────┬─────────┘    └────────┬─────────┘       │
		│           │                       │                 │
		│      ┌────┴────┐             ┌────┴────┐            │
		│      │ 64GB    │             │ 64GB    │            │
		│      │ DRAM    │             │ DRAM    │            │
		│      │ Node 0  │             │ Node 1  │            │
		│      └─────────┘             └─────────┘            │
		│           └──────── QPI/UPI ──────────┘             │
		└─────────────────────────────────────────────────────┘

		- L1/L2/L3 Cache：CPU 内部的硬件，内核管不了，完全由硬件自动管理
		- DRAM：每个 Socket 有自己直连的内存，这才是 NUMA 策略控制的对象

		---
		分配时机：缺页中断

		内存不是一开始就分配的，Linux 用懒分配：

		malloc(1GB)
			│
			▼
		mmap() ── 只建立虚拟地址空间，不分配物理页
			│
			▼
		第一次访问该地址
			│
			▼
		缺页中断 (page fault)
			│
			▼
		__alloc_pages()  ← NUMA 策略在这里生效
			│
			├─ MPOL_LOCAL     → 从 CPU 当前所在节点的 DRAM 取一页
			├─ MPOL_BIND      → 只从指定节点的 DRAM 取一页
			└─ MPOL_INTERLEAVE → 轮流从 node0/node1 取页

		---
		关键点

		┌───────────────────────┬─────────────────────────────────────────┐
		│         问题          │                  答案                   │
		├───────────────────────┼─────────────────────────────────────────┤
		│ 是预分配一块区域吗？  │ 不是，每次缺页才按策略选节点            │
		├───────────────────────┼─────────────────────────────────────────┤
		│ 是分配 CPU Cache 吗？ │ 不是，Cache 由硬件全自动管理            │
		├───────────────────────┼─────────────────────────────────────────┤
		│ 控制的是什么？        │ 物理页从哪个节点的 DRAM 分配            │
		├───────────────────────┼─────────────────────────────────────────┤
		│ 什么时候生效？        │ 缺页中断时（第一次访问虚拟地址）        │
		├───────────────────────┼─────────────────────────────────────────┤
		│ 已分配的页能改变吗？  │ 能，mbind() + MPOL_MF_MOVE 可迁移已有页 │
		└───────────────────────┴─────────────────────────────────────────┘

		---
		为什么快/慢

		Core 0 访问 Node 0 的内存：
		Core 0 → 本地 IMC → Node 0 DRAM    ~60ns ✓

		Core 0 访问 Node 1 的内存：
		Core 0 → 本地 IMC → QPI总线 → Node 1 IMC → Node 1 DRAM    ~120ns ✗

		慢的原因是多了一跳 QPI/UPI 总线，而不是 Cache 问题。NUMA 策略的本质就是尽量让 Core 和它访问的 DRAM 在同一个 Socket，少走这一跳。

	*/
	/*
		NUMA 内存策略

		NUMA（Non-Uniform Memory Access） 是多处理器系统中的内存架构——不同 CPU 访问本地内存快、访问远端节点内存慢。内存策略就是控制内核从哪个 NUMA 节点分配内存的规则。

		---
		numa_policy_init 做了什么

		1. 创建 slab 缓存 — policy_cache（存 mempolicy 结构体）和 sn_cache（存共享策略节点）
		2. 为每个 NUMA 节点预建 PREFERRED 策略 — preferred_node_policy[nid]，供后续快速使用
		3. 为系统 init 进程设置 INTERLEAVE 策略 — 内存交错分布到各节点（>= 16MB 的节点才参与；若全部太小则选最大节点）
		4. 调用 check_numabalancing_enable() — 检查是否开启自动 NUMA 均衡

		---
		六种策略模式

		┌──────────────────────────┬──────────────────────────────────────────────────┐
		│           模式           │                       含义                       │
		├──────────────────────────┼──────────────────────────────────────────────────┤
		│ MPOL_DEFAULT             │ 默认，从当前进程所在节点分配                     │
		├──────────────────────────┼──────────────────────────────────────────────────┤
		│ MPOL_PREFERRED           │ 优先从指定节点分配，不够再用其他节点             │
		├──────────────────────────┼──────────────────────────────────────────────────┤
		│ MPOL_PREFERRED_MANY      │ 优先从多个指定节点分配                           │
		├──────────────────────────┼──────────────────────────────────────────────────┤
		│ MPOL_BIND                │ 严格绑定到指定节点集，不允许溢出                 │
		├──────────────────────────┼──────────────────────────────────────────────────┤
		│ MPOL_INTERLEAVE          │ 轮询交错分配到多个节点（均摊带宽，适合大数据集） │
		├──────────────────────────┼──────────────────────────────────────────────────┤
		│ MPOL_LOCAL               │ 强制只用本地节点（类似 PREFERRED 但不指定节点）  │
		├──────────────────────────┼──────────────────────────────────────────────────┤
		│ MPOL_WEIGHTED_INTERLEAVE │ 带权重的交错（按节点容量比例分配）               │
		└──────────────────────────┴──────────────────────────────────────────────────┘

		各策略的优势场景

		MPOL_DEFAULT / MPOL_LOCAL — 延迟最低
		进程在哪个 CPU 运行就从哪个节点分配，数据和计算在同一节点，不走慢速跨节点互联。适合单线程、延迟敏感的应用。

		MPOL_BIND — 隔离保证
		强制限定只能用指定节点的内存。优势是可预测性——避免某个进程悄悄占用了其他节点的内存，适合容器/虚拟机的资源隔离。

		MPOL_INTERLEAVE — 带宽最大
		内存页轮流放在 node0/node1/node0/node1...，读写时多个节点的内存控制器并行工作，聚合带宽翻倍。适合大内存、随机访问的场景（数据库 buffer pool、科学计算）。

		MPOL_PREFERRED — 折中
		优先本地，本地不够时自动溢出到其他节点，不会因为本地内存紧张而 OOM。适合不确定内存用量的通用场景。

		---
		为什么 init 用 INTERLEAVE

		内核启动时不知道未来的工作负载，INTERLEAVE 策略可以将内核数据结构均匀散布到各 NUMA 节点，避免启动时所有内存都集中在 node 0，后续用户进程根据需要再通过 set_mempolicy(2) / mbind(2) 设置自己的策略。
	*/
	numa_policy_init();

	/* ACPI 早期初始化（解析 ACPI 表，发现设备拓扑） */
	/*
		ACPI（Advanced Configuration and Power Interface，高级配置与电源接口）是一个开放标准，定义了操作系统与硬件固件之间的接口，用于：

		主要功能：
		- 电源管理：控制 CPU 频率调节、睡眠/唤醒（S0-S5 状态）、设备电源开关
		- 硬件配置：让 OS 发现和配置主板上的设备（替代早期的 PnP BIOS）
		- 热管理：监控温度、控制风扇转速

		工作原理：
		- 固件（UEFI/BIOS）提供 ACPI 表（如 DSDT、SSDT），用 AML（ACPI Machine Language）字节码描述硬件
		- 内核内置 AML 解释器（drivers/acpi/）解析这些表
		- OS 通过 ACPI 接口与硬件交互，而无需为每块主板写专用驱动

		在 Linux 内核中：
		- init/main.c 中的 acpi_early_init() 在启动早期初始化 ACPI 子系统
		- 相关代码在 drivers/acpi/ 和 include/acpi/

		简单说：ACPI 是硬件告诉操作系统"我有什么、怎么控制我"的语言，是现代 x86 系统电源管理的核心机制。
	*/
	/*
		ARM 平台上是有 ACPI 的，但情况比 x86 复杂：

		ARM64（AArch64）上的 ACPI：
		- 从 Linux 3.x 开始，ARM64 逐步加入 ACPI 支持
		- 主要面向服务器场景（SBSA/SBBR 规范），如 Ampere、Kunpeng、ThunderX 等服务器 SoC
		- 内核中有专门的 ARM64 ACPI 适配代码（arch/arm64/kernel/acpi.c）

		但 ARM 嵌入式/移动设备通常用 DT（Device Tree）：
		- 手机、嵌入式板卡（树莓派、开发板等）几乎全用 Device Tree（.dts/.dtb）描述硬件
		- 原因：ACPI 依赖固件质量，而嵌入式厂商固件参差不齐；DT 更轻量、更灵活
		- DT 是 ARM 嵌入式世界的事实标准

		32位 ARM（arm）：
		- 基本不用 ACPI，几乎全是 DT

		总结：

		┌────────────────────────┬────────────────────┐
		│          场景          │        机制        │
		├────────────────────────┼────────────────────┤
		│ x86 PC/服务器          │ ACPI               │
		├────────────────────────┼────────────────────┤
		│ ARM64 服务器           │ ACPI（越来越普及） │
		├────────────────────────┼────────────────────┤
		│ ARM64/ARM 嵌入式、手机 │ Device Tree        │
		└────────────────────────┴────────────────────┘

		你在 init/main.c 里看到的 acpi_early_init() 在 ARM 嵌入式设备上通常是空操作或直接跳过，由 CONFIG_ACPI 编译选项控制。
	*/
	/*
		ACPI 的职责分四大块：

		1. 电源管理（最广为人知）
		- CPU 睡眠状态（C-state）、性能档位（P-state）
		- 系统睡眠/唤醒（S0 正常运行 → S5 关机）
		- 设备电源开关（如关闭 USB 控制器省电）

		2. 硬件发现与配置
		- 告诉 OS 板上有哪些设备（嵌入式控制器、传感器、按钮等）
		- 这些设备没有标准总线（不像 PCI/USB 可自动枚举），靠 ACPI 命名空间描述

		3. 热管理
		- 读取温度传感器
		- 控制风扇转速
		- 触发过热保护（降频或强制关机）

		4. 平台事件通知
		- 电源键按下、合盖、电池插拔、对接坞连接等事件通过 SCI 中断通知 OS
		- OS 收到 SCI 后执行对应的 AML 方法处理事件

		直观比例：

		ACPI 职责
		├── 电源管理        ████████░░  重要但不是全部
		├── 硬件描述/发现   ██████░░░░  现代系统非常依赖
		├── 热管理          ████░░░░░░
		└── 事件通知        ███░░░░░░░

		历史上 ACPI 是为了统一替换三样东西：APM（纯电源管理）、PnP BIOS（设备发现）、以及各厂商私有的热管理方案。所以它天生是个"大杂烩"规范，电源只是其中最显眼的部分。
	*/
	/*
		ACPI 描述写在固件（BIOS/UEFI）里，不是硬件。

		固件存储在主板上的一块 Flash 芯片中，DSDT/SSDT 等 AML 字节码就烧录在这里。启动时固件把这些表加载到内存，内核去读取。

		---
		有 bug 的修复方式，有三种，不需要重装系统：

		1. 刷新固件（最根本）
		厂商发布 BIOS 更新修复 AML bug，用户更新固件即可。但很多老机器厂商已停止更新。

		2. 内核 override DSDT（dmi_check_system 做的事）
		内核在编译时内置一份修正过的 DSDT，启动时检测到对应机型就用内置版本替换固件版本，固件本身不变。这是内核绕过固件 bug 的手段，不需要用户做任何操作，对用户透明。

		3. 用户手动覆盖 DSDT
		把修正后的 DSDT 编译成 .aml 文件，放到 initramfs 中，内核启动时加载用户提供的版本替换固件版本。适合固件无更新且内核也没有内置 quirk 的情况：

		# 大致流程
		iasl -d dsdt.dat          # 反编译固件 DSDT 为可读的 .dsl
		# 手动修改 .dsl 中的 bug
		iasl -tc dsdt.dsl         # 重新编译为 .aml
		# 打包进 initramfs，内核通过 INITRD_DSDT 加载

		---
		总结：

		┌────────────────┬────────────┬──────────────────┐
		│    修复方式    │   谁来做   │    需要重装？    │
		├────────────────┼────────────┼──────────────────┤
		│ 刷固件         │ 用户/厂商  │ 否               │
		├────────────────┼────────────┼──────────────────┤
		│ 内核内置 quirk │ 内核开发者 │ 否，升级内核即可 │
		├────────────────┼────────────┼──────────────────┤
		│ 用户覆盖 DSDT  │ 用户       │ 否               │
		└────────────────┴────────────┴──────────────────┘

		三种方式都不需要重装系统，固件 bug 属于"硬件厂商欠的债"，内核长期维护着大量这类 workaround。
	*/
	/*
		是谁将其从flash芯片读到内存的

		是固件自己（UEFI/BIOS）完成的，内核看到的时候表已经在内存里了。

		完整流程：

		上电
		│
		▼
		CPU 从 Flash 芯片固定地址开始执行固件代码
		│
		▼
		固件初始化内存控制器（此前连 RAM 都不可用）
		│
		▼
		固件将 ACPI 表（DSDT/SSDT/FADT 等）从 Flash 复制到 RAM
		并在特定内存区域写入 RSDP（入口指针）
		│
		▼
		固件把控制权交给 bootloader（GRUB 等）
		│
		▼
		bootloader 加载内核
		│
		▼
		内核通过 RSDP 找到根表，再顺着指针找到所有 ACPI 表
		内核只是"读客"，表已经在内存里等着了

		内核侧的入口就是你现在打开的 tbxface.c 里的 acpi_find_root_pointer()——它负责在内存中搜索 RSDP 签名（"RSD PTR "），找到后整个 ACPI 表树就可以顺着指针遍历了。

		acpi_reallocate_root_table()（acpi_early_init 里调用的那个）做的事情是把固件放在 EfiBootServices 内存里的表再拷贝一份到内核自己管理的内存，因为 EfiBootServices 那块内存后面会被释放掉。
	*/
	/*
		acpi描述的内存地址是如何传递的

		这是一个地址链，每一级指向下一级：

		UEFI 启动路径（现代）：

		固件构建 EFI System Table（放在内存某处）
		│  包含 ConfigurationTable[] 数组，其中一项是 RSDP 地址
		│
		▼
		bootloader/EFI stub 把 EFI System Table 地址
		写入 boot_params（x86 启动协议的结构体）
		│
		▼
		内核从 boot_params.efi_info 拿到 EFI System Table 地址
		→ 遍历 ConfigurationTable[] 找 ACPI RSDP 条目
		│
		▼
		RSDP 内含 XSDT 物理地址
		XSDT 内含所有其他表的物理地址数组（FADT、MADT、SSDT…）
		FADT 内含 DSDT 物理地址

		Legacy BIOS 路径（老机器）：

		固件把 RSDP 写入约定的内存区域
		├── EBDA（Extended BIOS Data Area）：低 1MB 内 0xE0000-0xFFFFF
		└── 或 BIOS ROM 区域
		│
		▼
		内核直接扫描这段物理内存，找 "RSD PTR " 签名（8字节）

		找到 RSDP 之后的地址链：

		RSDP
		└─→ XSDT（64位）或 RSDT（32位）
			└─→ [ FADT地址, MADT地址, SSDT地址, ... ]
					│
					▼
					FADT
					└─→ DSDT地址（主 AML 表）
						└─→ 可引用多个 SSDT（补充 AML 表）

		关键点：
		- 整个链条都是物理地址，内核读取前需要用 ioremap 映射到虚拟地址
		- acpi_reallocate_root_table() 就是在把这些物理地址对应的内容拷贝到内核自己的内存，然后更新内部指针指向新位置
		- 地址本身不是"传递"的，而是固件按规范写在约定位置，内核按规范去找——是一种约定寻址，不是函数调用
	*/
	acpi_early_init();

	/*
	* 晚期时间初始化（late_time_init）：
	*
	* 设计动机：某些架构的时钟硬件依赖 ioremap（将物理地址映射到内核虚拟地址）
	* 才能访问，而 ioremap 本身需要内存管理子系统就绪。因此这些架构在早期的
	* time_init() 中只是把真正的初始化函数赋给 late_time_init 函数指针，
	* 留到这里——内存/ACPI 都已就绪之后——再执行。
	*
	* 以 x86 为例（arch/x86/kernel/time.c: x86_late_time_init）：
	*   1. intr_mode_select()  — 选择中断投递模式（PIC / APIC / x2APIC）；
	*      决定 PIT 是否需要初始化，必须在 timer_init() 之前完成。
	*   2. timer_init()        — 初始化传统定时器：优先启用 HPET，若不可用
	*      则退回 8254 PIT；同时注册 IRQ0 定时器中断处理函数。
	*   3. intr_mode_init()    — 完成中断模式的最终切换（Legacy PIC → APIC）。
	*   4. tsc_init()          — 初始化 TSC（Time Stamp Counter）时钟源，
	*      校准 TSC 频率，并决定是否将其注册为高精度 clocksource。
	*      若 CPU 支持 WAITPKG，还会启用 tpause 指令优化 udelay。
	*
	* 其他架构的实现：
	*   - ARM  : twd_timer_setup()     — 初始化 per-CPU 的 TWD 本地定时器
	*   - MIPS : ocelot_late_init()    — 初始化 Ocelot SoC 的时钟
	*   - SH   : sh_late_time_init()   — SuperH 平台定时器
	*   - UML  : um_timer_init()       — User Mode Linux 虚拟定时器
	*
	* 若架构无此需求，指针保持 NULL，if 判断直接跳过，零开销。
	*/
	if (late_time_init)
		late_time_init();

	/*
	 * 初始化调度时钟（sched_clock）。
	 *
	 * 目的：
	 *   为调度器建立一个每 CPU 的纳秒级单调时钟。调度器用它测量任务的运行
	 *   时间（vruntime）、计算调度延迟、以及 perf/ftrace 的时间戳。
	 *
	 * 调用后的影响：
	 *   - sched_clock_cpu(cpu)、local_clock()、cpu_clock(cpu) 开始返回
	 *     有意义的纳秒时间戳；
	 *   - 若架构配置了 CONFIG_HAVE_UNSTABLE_SCHED_CLOCK，还会计算好
	 *     __gtod_offset，使后续每个 tick 中断里的 sched_clock_tick()
	 *     能从当前时刻无缝衔接，不产生跳变；
	 *   - 将 sched_clock_running 引用计数从 0 增到 1，激活
	 *     sched_clock_cpu() 的 per-CPU 快路径。
	 *
	 * 为何在此处调用：
	 *   必须在 late_time_init()（TSC/HPET 初始化）之后，才能读到
	 *   有效的硬件时间源；同时必须在 calibrate_delay()（依赖时间测量）
	 *   之前，保证后续一切时间相关操作都有可用的时钟基础。
	 */
	sched_clock_init();

	/*
	 * 计算 BogoMIPS（Bogus MIPS，一个粗略的 CPU 速度指标）。
	 * 通过测量空循环延迟来估算 CPU 频率，用于 udelay() 的校准。
	 * dmesg 中会看到 "Calibrating delay loop... 1234.56 BogoMIPS"。
	 */
	calibrate_delay();

	/* CPU 最终初始化（处理器特性检测、漏洞缓解措施最终确认） */
	arch_cpu_finalize_init();

	/* 初始化 PID 的 IDR（整数 ID 分配器，用于分配进程 ID） */
	pid_idr_init();

	/*
	 * 初始化匿名页反向映射（rmap）的 slab 缓存。
	 *
	 * 目的：
	 *   在 ARM64 上，每次 mmap 私有匿名映射或 fork 时，内核都需要分配
	 *   anon_vma 和 anon_vma_chain 对象。此函数预先创建对应的 slab 缓存，
	 *   使后续分配走快速的每 CPU slab 路径，而非每次 kmalloc。
	 *
	 * 产生的影响：
	 *   - anon_vma_cachep 就绪：此后 anon_vma_alloc() 可用，
	 *     do_mmap() / anon_vma_prepare() 才能为匿名 VMA 建立 rmap 链；
	 *   - anon_vma_chain_cachep 就绪：fork 时 dup_mmap() 调用的
	 *     anon_vma_clone() 才能分配 anon_vma_chain 节点；
	 *   - 没有这两个缓存，第一个用户进程（init）执行任何匿名 mmap 或
	 *     fork 都会立刻 panic（SLAB_PANIC 标志）。
	 *
	 * 为何在此处调用：
	 *   必须在 slab 子系统（kmem_cache_init）就绪之后；
	 *   必须在第一个用户进程 kernel_init 创建任何匿名映射之前。
	 *   ARM64 无特殊硬件依赖，纯内存分配，时序窗口宽松，放在这里合适。
	 */
	anon_vma_init();

	/* 初始化线程栈的 slab 缓存（加速线程创建） */
	thread_stack_cache_init();

	/* 初始化进程凭证（credentials）系统（uid/gid/capabilities） */
	cred_init();

	/*
	 * 初始化 fork 机制。
	 * fork 是创建新进程的基础，Zygote fork App 就用这个。
	 * 建立 task_struct 的 slab 缓存等。
	 */
	fork_init();

	/* 初始化 /proc 文件系统的各种 slab 缓存 */
	proc_caches_init();

	/* 初始化 UTS 命名空间（主机名、域名的隔离，容器技术基础） */
	uts_ns_init();

	/* 初始化时间命名空间（容器内独立时钟偏移） */
	time_ns_init();

	/* 初始化内核密钥系统（存储加密密钥、凭证等） */
	key_init();

	/* 安全框架完整初始化（SELinux 策略在这里加载） */
	security_init();

	/* 调试相关的晚期初始化 */
	dbg_late_init();

	/* 初始化网络命名空间（network namespace，容器网络隔离的基础） */
	net_ns_init();

	/*
	 * VFS 层的完整初始化：建立路径查找、文件描述符、挂载点等全部基础设施。
	 *
	 * 目的：
	 *   完成内核 VFS 层所需的全部 slab 缓存和哈希表，使后续的 open/read/
	 *   write/mount 等系统调用拥有完整的数据结构支撑。
	 *
	 * 调用后的影响（7 个子步骤，详见 fs/dcache.c: vfs_caches_init）：
	 *   1. filename_init    — 路径名 slab 缓存就绪，sys_open 可分配路径对象；
	 *   2. dcache_init      — dentry slab + 哈希表就绪，路径查找缓存生效；
	 *   3. inode_init       — inode slab + 哈希表就绪，文件元数据缓存生效；
	 *   4. files_init       — struct file / backing_file slab 就绪，
	 *                         文件描述符可分配；
	 *   5. files_maxfiles_init — 按当前内存大小计算系统级 max_files 上限；
	 *   6. mnt_init         — 挂载点 slab + 哈希表就绪，kernfs/sysfs 初始化，
	 *                         rootfs 和初始挂载树建立，VFS 命名空间可用；
	 *   7. bdev_cache_init  — 块设备 inode slab 和伪文件系统就绪，
	 *      chrdev_init      — 字符设备映射表就绪。
	 *
	 * 为何在此处调用：
	 *   依赖 slab（kmem_cache_init）、内存管理、网络命名空间（net_ns_init）
	 *   均已就绪；mnt_init 内部的 shmem_init 依赖页缓存前置条件也已满足。
	 *   此函数执行完后，pagecache_init 和 signals_init 才能安全运行，
	 *   整个 VFS 栈从这一刻起对后续子系统完全可用。
	 */
	vfs_caches_init();

	/* 初始化页缓存（文件内容的内存缓存） */
	pagecache_init();

	/* 初始化信号机制（kill/signal 的基础数据结构） */
	signals_init();

	/* 初始化 seq_file（/proc 文件的顺序读取接口） */
	seq_file_init();

	/*
	 * 挂载并完整初始化 procfs（/proc 文件系统）。
	 *
	 * 目的：
	 *   procfs 是内核向用户空间暴露运行时状态的主要接口，提供：
	 *     /proc/<pid>/         — 每个进程的内存映射、文件描述符、状态等；
	 *     /proc/self/          — 当前进程的符号链接快捷方式；
	 *     /proc/sys/           — sysctl 参数的文件系统视图（可读写调参）；
	 *     /proc/net/           — 网络栈统计（每网络命名空间独立）；
	 *     /proc/tty/           — TTY 驱动注册信息。
	 *
	 * 调用后的影响：
	 *   - proc_inode_cachep / pde_opener_cache / proc_dir_entry_cache 就绪；
	 *   - /proc/self、/proc/thread-self 的 inode 编号预分配完成；
	 *   - /proc/mounts（→ self/mounts）、/proc/fs、/proc/driver、
	 *     /proc/bus、/proc/net、/proc/sys、/proc/tty 目录全部建立；
	 *   - proc_fs_type 注册到 VFS，后续 mount procfs 时可通过名字找到；
	 *   - sysctl_init_bases() 完成，/proc/sys/kernel、/proc/sys/vm 等
	 *     基础 sysctl 节点可读写。
	 *
	 * 为何在此处调用：
	 *   必须在 vfs_caches_init()（dentry/inode/file 缓存）和
	 *   seq_file_init()（/proc 文件的顺序读接口）之后；
	 *   必须在 fork_init()（创建第一个进程）之前，否则第一个进程
	 *   的 /proc/<pid>/ 目录无法建立。
	 */
	proc_root_init();

	/* 初始化命名空间文件系统（/proc/*/ns/ 目录） */
	nsfs_init();

	/* 初始化 PID 文件系统（通过文件描述符引用进程，防止 PID 复用竞争） */
	pidfs_init();

	/* 初始化 cpuset（CPU 和内存节点的分组管理，cgroup 的一部分） */
	cpuset_init();

	/* 初始化 memory cgroup（内存使用量限制和统计） */
	mem_cgroup_init();

	/* 完整初始化 cgroup（控制组，容器技术的核心机制） */
	cgroup_init();

	/* 初始化任务统计接口（进程资源使用统计，供 /proc 使用） */
	taskstats_init_early();

	/* 初始化延迟记账（记录进程等待调度、IO 等的时间） */
	delayacct_init();

	/* ACPI 子系统完整初始化 */
	acpi_subsystem_init();

	/* 架构在 ACPI 初始化后的钩子（ARM64 这里处理 ACPI 平台设备） */
	arch_post_acpi_subsys_init();

	/* 初始化 KCSAN（内核并发安全分析，检测数据竞争） */
	kcsan_init();

	/*
	 * ── 最后一步 ──────────────────────────────────────────────────
	 * 进入 rest_init()：创建 PID 1 和 PID 2，自身变成 idle 进程。
	 * 注释"we're now alive"：内核现在完全活着了，后续工作交给内核线程。
	 */
	/*
		---
		start_kernel 为何变成 idle 进程，而不是被回收？

		核心问题：PID 0（swapper/idle）是一个永久存在的特殊进程，每个 CPU 各有一个，它是调度器的兜底，不能也不需要回收。

		执行流变成 idle 的全过程

		start_kernel()
		└─ rest_init()                     ← 永不返回
			├─ user_mode_thread(kernel_init) → fork 出 PID 1
			├─ kernel_thread(kthreadd)       → fork 出 PID 2
			├─ complete(&kthreadd_done)      → 解锁 PID 1
			├─ schedule_preempt_disabled()   → 第一次调度，把 CPU 让出去
			└─ cpu_startup_entry(CPUHP_ONLINE) → 进入 idle 死循环，永不返回

		start_kernel 的执行流本身（CPU 上的那段寄存器 + 栈）没有消失，它"变身"为 PID 0 / swapper / idle，永远在 cpu_startup_entry 的循环里跑 WFI（Wait For Interrupt）。

		---
		为什么不回收，而是让它一直跑？

		1. 调度器的兜底角色

		调度器必须保证"runqueue 永远不为空"。当所有任务都在睡眠、没有可运行任务时，调度器会选择 idle 进程。如果 idle 进程不存在，调度器就无处可去，CPU 会乱跑。

		2. PID 0 不在进程树里，无法被 wait() 回收

		普通进程被回收的路径是：父进程调用 wait()，内核清理 task_struct。但 PID 0 没有父进程，内核也从未将它注册进可 wait 的进程树，所以标准的进程生命周期管理根本不适用于它。

		3. 每个 CPU 都需要一个自己的 idle 进程

		SMP 系统中，每个 CPU 都有独立的 idle 线程（swapper/0, swapper/1, …），它们的栈和 task_struct 都是静态分配的，不走 kmalloc/kfree 路径，天生就是"永久的"。

		4. idle 不是"什么都不干"，而是让 CPU 省电

		cpu_startup_entry 循环本质上是：

		while (1) {
			// 挑选 cpuidle 驱动提供的最深睡眠状态
			cpuidle_idle_call();   // 内部执行 WFI / MWAIT / HLT 等指令
		}

		这是 CPU 功耗管理的核心路径。没有 idle 进程，CPU 就无法进入 C-state 节电。

		---
		为什么 rest_init 标记了 __noreturn？

		因为 cpu_startup_entry 永不返回，整条调用链都不会回到 start_kernel。__noreturn 告诉编译器不用为返回路径生成代码，也方便静态分析工具验证这一不变式。

		---
		一句话总结： start_kernel 的执行流变成 idle 不是"浪费"，而是被复用为调度器兜底 + CPU 节电的必要基础设施，每个 CPU 各一个，静态存在，无需也无法回收。
	*/
	rest_init();

	/*
	 * Avoid stack canaries in callers of boot_init_stack_canary for gcc-10
	 * and older.
	 */
#if !__has_attribute(__no_stack_protector__)
	prevent_tail_call_optimization();
#endif
}

/* Call all constructor functions linked into the kernel. */
static void __init do_ctors(void)
{
/*
 * For UML, the constructors have already been called by the
 * normal setup code as it's just a normal ELF binary, so we
 * cannot do it again - but we do need CONFIG_CONSTRUCTORS
 * even on UML for modules.
 */
#if defined(CONFIG_CONSTRUCTORS) && !defined(CONFIG_UML)
	ctor_fn_t *fn = (ctor_fn_t *) __ctors_start;

	for (; fn < (ctor_fn_t *) __ctors_end; fn++)
		(*fn)();
#endif
}

#ifdef CONFIG_KALLSYMS
struct blacklist_entry {
	struct list_head next;
	char *buf;
};

static __initdata_or_module LIST_HEAD(blacklisted_initcalls);

static int __init initcall_blacklist(char *str)
{
	char *str_entry;
	struct blacklist_entry *entry;

	/* str argument is a comma-separated list of functions */
	do {
		str_entry = strsep(&str, ",");
		if (str_entry) {
			pr_debug("blacklisting initcall %s\n", str_entry);
			entry = memblock_alloc_or_panic(sizeof(*entry),
					       SMP_CACHE_BYTES);
			entry->buf = memblock_alloc_or_panic(strlen(str_entry) + 1,
						    SMP_CACHE_BYTES);
			strcpy(entry->buf, str_entry);
			list_add(&entry->next, &blacklisted_initcalls);
		}
	} while (str_entry);

	return 1;
}

static bool __init_or_module initcall_blacklisted(initcall_t fn)
{
	struct blacklist_entry *entry;
	char fn_name[KSYM_SYMBOL_LEN];
	unsigned long addr;

	if (list_empty(&blacklisted_initcalls))
		return false;

	addr = (unsigned long) dereference_function_descriptor(fn);
	sprint_symbol_no_offset(fn_name, addr);

	/*
	 * fn will be "function_name [module_name]" where [module_name] is not
	 * displayed for built-in init functions.  Strip off the [module_name].
	 */
	strreplace(fn_name, ' ', '\0');

	list_for_each_entry(entry, &blacklisted_initcalls, next) {
		if (!strcmp(fn_name, entry->buf)) {
			pr_debug("initcall %s blacklisted\n", fn_name);
			return true;
		}
	}

	return false;
}
#else
static int __init initcall_blacklist(char *str)
{
	pr_warn("initcall_blacklist requires CONFIG_KALLSYMS\n");
	return 0;
}

static bool __init_or_module initcall_blacklisted(initcall_t fn)
{
	return false;
}
#endif
__setup("initcall_blacklist=", initcall_blacklist);

static __init_or_module void
trace_initcall_start_cb(void *data, initcall_t fn)
{
	ktime_t *calltime = data;

	printk(KERN_DEBUG "calling  %pS @ %i\n", fn, task_pid_nr(current));
	*calltime = ktime_get();
}

static __init_or_module void
trace_initcall_finish_cb(void *data, initcall_t fn, int ret)
{
	ktime_t rettime, *calltime = data;

	rettime = ktime_get();
	printk(KERN_DEBUG "initcall %pS returned %d after %lld usecs\n",
		 fn, ret, (unsigned long long)ktime_us_delta(rettime, *calltime));
}

static __init_or_module void
trace_initcall_level_cb(void *data, const char *level)
{
	printk(KERN_DEBUG "entering initcall level: %s\n", level);
}

static ktime_t initcall_calltime;

#ifdef TRACEPOINTS_ENABLED
static void __init initcall_debug_enable(void)
{
	int ret;

	ret = register_trace_initcall_start(trace_initcall_start_cb,
					    &initcall_calltime);
	ret |= register_trace_initcall_finish(trace_initcall_finish_cb,
					      &initcall_calltime);
	ret |= register_trace_initcall_level(trace_initcall_level_cb, NULL);
	WARN(ret, "Failed to register initcall tracepoints\n");
}
# define do_trace_initcall_start	trace_initcall_start
# define do_trace_initcall_finish	trace_initcall_finish
# define do_trace_initcall_level	trace_initcall_level
#else
static inline void do_trace_initcall_start(initcall_t fn)
{
	if (!initcall_debug)
		return;
	trace_initcall_start_cb(&initcall_calltime, fn);
}
static inline void do_trace_initcall_finish(initcall_t fn, int ret)
{
	if (!initcall_debug)
		return;
	trace_initcall_finish_cb(&initcall_calltime, fn, ret);
}
static inline void do_trace_initcall_level(const char *level)
{
	if (!initcall_debug)
		return;
	trace_initcall_level_cb(NULL, level);
}
#endif /* !TRACEPOINTS_ENABLED */

/*
 * do_one_initcall()：执行单个 initcall 函数，并做健康检查。
 *
 * 每个 initcall 函数执行后，检查：
 *   1. 抢占计数是否平衡（如果不平衡说明驱动有锁泄露）
 *   2. 中断是否意外被关闭（如果是说明驱动忘记 local_irq_enable）
 * 发现问题时打印警告但继续启动（不 panic），尽量让系统跑起来。
 */
int __init_or_module do_one_initcall(initcall_t fn)
{
	int count = preempt_count(); /* 记录执行前的抢占计数，用于检测泄露 */
	char msgbuf[64];
	int ret;

	/* 检查黑名单（可通过 initcall_blacklist= 参数跳过特定初始化函数） */
	if (initcall_blacklisted(fn))
		return -EPERM;

	do_trace_initcall_start(fn);
	ret = fn(); /* 执行 initcall 函数 */
	do_trace_initcall_finish(fn, ret);

	msgbuf[0] = 0;

	/* 检查抢占计数是否平衡（spin_lock/unlock 必须成对） */
	if (preempt_count() != count) {
		sprintf(msgbuf, "preemption imbalance ");
		preempt_count_set(count); /* 强制修复，避免后续代码崩溃 */
	}
	/* 检查中断状态（驱动不应该在 initcall 结束时关着中断） */
	if (irqs_disabled()) {
		strlcat(msgbuf, "disabled interrupts ", sizeof(msgbuf));
		local_irq_enable(); /* 强制修复 */
	}
	WARN(msgbuf[0], "initcall %pS returned with %s\n", fn, msgbuf);

	/* 向随机数熵池添加一点隐含的随机性（利用执行时间的不确定性） */
	add_latent_entropy();
	return ret;
}


/*
 * initcall 机制说明：
 *
 * 内核中的驱动和子系统通过宏注册自己的初始化函数：
 *   pure_initcall(fn)      → level 0，最先运行
 *   core_initcall(fn)      → level 1
 *   postcore_initcall(fn)  → level 2
 *   arch_initcall(fn)      → level 3
 *   subsys_initcall(fn)    → level 4
 *   fs_initcall(fn)        → level 5
 *   device_initcall(fn)    → level 6  （module_init() 默认用这个）
 *   late_initcall(fn)      → level 7，最后运行
 *
 * 这些宏展开后把函数指针放入特殊的 ELF 段（如 .initcall1.init）。
 * 链接脚本把同一 level 的所有函数指针连续排列，
 * __initcallN_start 指向每段的起始地址。
 * do_initcalls() 按 level 顺序遍历这些指针并逐个调用。
 *
 * 好处：添加驱动只需加一行宏，不需要修改任何初始化流程代码。
 */
static initcall_entry_t *initcall_levels[] __initdata = {
	__initcall0_start,  /* pure */
	__initcall1_start,  /* core */
	__initcall2_start,  /* postcore */
	__initcall3_start,  /* arch */
	__initcall4_start,  /* subsys */
	__initcall5_start,  /* fs */
	__initcall6_start,  /* device（驱动的默认 level） */
	__initcall7_start,  /* late */
	__initcall_end,
};

/* 与 include/linux/init.h 中的 initcall 宏保持同步 */
static const char *initcall_level_names[] __initdata = {
	"pure",
	"core",
	"postcore",
	"arch",
	"subsys",
	"fs",
	"device",
	"late",
};

static int __init ignore_unknown_bootoption(char *param, char *val,
			       const char *unused, void *arg)
{
	return 0;
}

static void __init do_initcall_level(int level, char *command_line)
{
	initcall_entry_t *fn;

	parse_args(initcall_level_names[level],
		   command_line, __start___param,
		   __stop___param - __start___param,
		   level, level,
		   NULL, ignore_unknown_bootoption);

	do_trace_initcall_level(initcall_level_names[level]);
	for (fn = initcall_levels[level]; fn < initcall_levels[level+1]; fn++)
		do_one_initcall(initcall_from_entry(fn));
}

static void __init do_initcalls(void)
{
	int level;
	size_t len = saved_command_line_len + 1;
	char *command_line;

	command_line = kzalloc(len, GFP_KERNEL);
	if (!command_line)
		panic("%s: Failed to allocate %zu bytes\n", __func__, len);

	for (level = 0; level < ARRAY_SIZE(initcall_levels) - 1; level++) {
		/* Parser modifies command_line, restore it each time */
		strcpy(command_line, saved_command_line);
		do_initcall_level(level, command_line);
	}

	kfree(command_line);
}

/*
 * Ok, the machine is now initialized. None of the devices
 * have been touched yet, but the CPU subsystem is up and
 * running, and memory and process management works.
 *
 * Now we can finally start doing some real work..
 */
static void __init do_basic_setup(void)
{
	cpuset_init_smp();
	ksysfs_init();
	driver_init();
	init_irq_proc();
	do_ctors();
	do_initcalls();
}

static void __init do_pre_smp_initcalls(void)
{
	initcall_entry_t *fn;

	do_trace_initcall_level("early");
	for (fn = __initcall_start; fn < __initcall0_start; fn++)
		do_one_initcall(initcall_from_entry(fn));
}

static int run_init_process(const char *init_filename)
{
	const char *const *p;

	argv_init[0] = init_filename;
	pr_info("Run %s as init process\n", init_filename);
	pr_debug("  with arguments:\n");
	for (p = argv_init; *p; p++)
		pr_debug("    %s\n", *p);
	pr_debug("  with environment:\n");
	for (p = envp_init; *p; p++)
		pr_debug("    %s\n", *p);
	return kernel_execve(init_filename, argv_init, envp_init);
}

static int try_to_run_init_process(const char *init_filename)
{
	int ret;

	ret = run_init_process(init_filename);

	if (ret && ret != -ENOENT) {
		pr_err("Starting init: %s exists but couldn't execute it (error %d)\n",
		       init_filename, ret);
	}

	return ret;
}

static noinline void __init kernel_init_freeable(void);

#if defined(CONFIG_STRICT_KERNEL_RWX) || defined(CONFIG_STRICT_MODULE_RWX)
bool rodata_enabled __ro_after_init = true;

#ifndef arch_parse_debug_rodata
static inline bool arch_parse_debug_rodata(char *str) { return false; }
#endif

static int __init set_debug_rodata(char *str)
{
	if (arch_parse_debug_rodata(str))
		return 0;

	if (str && !strcmp(str, "on"))
		rodata_enabled = true;
	else if (str && !strcmp(str, "off"))
		rodata_enabled = false;
	else
		pr_warn("Invalid option string for rodata: '%s'\n", str);
	return 0;
}
early_param("rodata", set_debug_rodata);
#endif

static void mark_readonly(void)
{
	if (IS_ENABLED(CONFIG_STRICT_KERNEL_RWX) && rodata_enabled) {
		/*
		 * load_module() results in W+X mappings, which are cleaned
		 * up with init_free_wq. Let's make sure that queued work is
		 * flushed so that we don't hit false positives looking for
		 * insecure pages which are W+X.
		 */
		flush_module_init_free_work();
		jump_label_init_ro();
		mark_rodata_ro();
		debug_checkwx();
		rodata_test();
	} else if (IS_ENABLED(CONFIG_STRICT_KERNEL_RWX)) {
		pr_info("Kernel memory protection disabled.\n");
	} else if (IS_ENABLED(CONFIG_ARCH_HAS_STRICT_KERNEL_RWX)) {
		pr_warn("Kernel memory protection not selected by kernel config.\n");
	} else {
		pr_warn("This architecture does not have kernel memory protection.\n");
	}
}

void __weak free_initmem(void)
{
	free_initmem_default(POISON_FREE_INITMEM);
}

/*
 * kernel_init()：PID 1 的执行体。
 *
 * 由 rest_init() 通过 user_mode_thread() 创建，
 * 最终通过 kernel_execve() 变成用户态的 init 进程。
 * 执行成功后此函数不再存在（被 exec 替换），
 * 失败则 panic（没有 init 进程内核无法继续运行）。
 *
 * 这是内核线程变成用户进程的关键转变点。
 */
static int __ref kernel_init(void *unused)
{
	int ret;

	/*
	 * 等待 kthreadd（PID 2）完全建立。
	 * 原因：kernel_init 可能会创建内核线程，
	 * 必须等 kthreadd 就绪才能安全地创建内核线程。
	 * rest_init() 在创建完 kthreadd 后会调用 complete(&kthreadd_done)。
	 */
	wait_for_completion(&kthreadd_done);

	/*
	 * kernel_init_freeable()：完成剩余的初始化工作。
	 *   - 启动其他 CPU（SMP 初始化）
	 *   - 运行所有 do_initcalls（驱动初始化）
	 *   - 挂载根文件系统
	 * 这些工作放在单独的函数里是为了避免栈 canary 问题（见函数注释）。
	 */
	kernel_init_freeable();

	/* 等待所有异步 __init 代码完成，再释放 init 内存 */
	async_synchronize_full();

	/*
	 * 释放 __init 段内存。
	 * 所有标记了 __init 的函数和数据（包括 start_kernel 本身）
	 * 在这里被释放，通常能释放数百 KB 内存。
	 * dmesg 中会看到 "Freeing unused kernel image memory: XXXK freed"。
	 */
	system_state = SYSTEM_FREEING_INITMEM;
	kprobe_free_init_mem();  /* 清理 kprobe 在 init 段的数据 */
	ftrace_free_init_mem();  /* 清理 ftrace 在 init 段的数据 */
	kgdb_free_init_mem();    /* 清理 kgdb 在 init 段的数据 */
	exit_boot_config();      /* 释放 bootconfig 内存 */
	free_initmem();          /* 释放 __init 段 */

	/*
	 * 把内核代码/数据段标记为只读（W^X 保护）。
	 * 之前因为 __init 段在同一映射中需要先释放再标记，
	 * 现在 __init 内存已释放，可以安全地设置只读保护了。
	 * 这防止了对内核代码段的运行时修改（安全加固）。
	 */
	mark_readonly();

	/*
	 * 内核映射已最终确定，更新用户空间页表以完成 PTI（Page Table Isolation）。
	 * PTI 是 Meltdown 漏洞的缓解措施：用户态和内核态使用不同页表，
	 * 防止用户态推测执行读取内核内存。
	 */
	pti_finalize();

	/* 系统正式进入运行状态 */
	system_state = SYSTEM_RUNNING;
	numa_default_policy();

	/* 通知 RCU 内核启动阶段结束 */
	rcu_end_inkernel_boot();

	/* 处理通过 sysctl 命令行参数传入的 sysctl 设置 */
	do_sysctl_args();

	/*
	 * 尝试运行 init 进程，按优先级顺序：
	 *
	 * 1. ramdisk 中的 init（Android 的 /init 就是这个）
	 *    通过 rdinit= 参数指定，默认是 "/init"
	 */
	if (ramdisk_execute_command) {
		ret = run_init_process(ramdisk_execute_command);
		if (!ret)
			return 0; /* exec 成功，此函数不再存在 */
		pr_err("Failed to execute %s (error %d)\n",
		       ramdisk_execute_command, ret);
	}

	/*
	 * 2. 通过 init= 参数指定的 init
	 *    如果指定了但执行失败则直接 panic（用户明确指定了就不再尝试其他）
	 */
	if (execute_command) {
		ret = run_init_process(execute_command);
		if (!ret)
			return 0;
		panic("Requested init %s failed (error %d).",
		      execute_command, ret);
	}

	/* 3. Kconfig 中指定的默认 init（CONFIG_DEFAULT_INIT） */
	if (CONFIG_DEFAULT_INIT[0] != '\0') {
		ret = run_init_process(CONFIG_DEFAULT_INIT);
		if (ret)
			pr_err("Default init %s failed (error %d)\n",
			       CONFIG_DEFAULT_INIT, ret);
		else
			return 0;
	}

	/*
	 * 4. 按传统路径逐个尝试。
	 *    /bin/sh 作为最后手段，让管理员可以修复严重故障的系统。
	 */
	if (!try_to_run_init_process("/sbin/init") ||
	    !try_to_run_init_process("/etc/init") ||
	    !try_to_run_init_process("/bin/init") ||
	    !try_to_run_init_process("/bin/sh"))
		return 0;

	/* 所有尝试都失败，内核无法继续运行，panic */
	panic("No working init found.  Try passing init= option to kernel. "
	      "See Linux Documentation/admin-guide/init.rst for guidance.");
}

/* Open /dev/console, for stdin/stdout/stderr, this should never fail */
void __init console_on_rootfs(void)
{
	struct file *file = filp_open("/dev/console", O_RDWR, 0);

	if (IS_ERR(file)) {
		pr_err("Warning: unable to open an initial console.\n");
		return;
	}
	init_dup(file);
	init_dup(file);
	init_dup(file);
	fput(file);
}

/*
 * kernel_init_freeable()：kernel_init() 中可释放部分的初始化工作。
 *
 * 单独拆分为此函数的原因：
 *   防止 start_kernel() 和 boot_init_stack_canary() 的调用者
 *   在 free_initmem() 之前被 GCC-10 及更早版本尾调用优化掉。
 *   noinline 确保栈帧独立存在。
 *
 * 执行顺序：
 *   1. SMP 初始化（启动其他 CPU）
 *   2. do_initcalls（所有驱动和子系统的批量初始化）
 *   3. 挂载根文件系统
 */
static noinline void __init kernel_init_freeable(void)
{
	/*
	 * 现在调度器完全就绪，可以进行阻塞分配了。
	 * __GFP_BITS_MASK 开启所有 GFP 标志，包括允许睡眠等待内存。
	 * 之前只允许非阻塞分配（避免在调度器就绪前睡眠）。
	 */
	gfp_allowed_mask = __GFP_BITS_MASK;

	/* init 进程可以从任意 NUMA 节点分配内存 */
	set_mems_allowed(node_states[N_MEMORY]);

	/* 记录 init 进程的 PID（用于 Ctrl+Alt+Del 重启信号发送目标） */
	cad_pid = get_pid(task_pid(current));

	/* 为所有 CPU 做启动前的准备工作（分配每 CPU 数据结构等） */
	smp_prepare_cpus(setup_max_cpus);

	/*
	 * 完成工作队列初始化（启动 kworker 内核线程）。
	 * 之前 workqueue_init_early() 只建立了框架，
	 * 现在才真正启动工作线程来执行队列中的工作项。
	 */
	workqueue_init();

	/* 内存管理内部数据结构的后期初始化 */
	init_mm_internals();

	/*
	 * 运行 SMP 启动前的 initcalls（level 0，"pure" 级别）。
	 * 这些是必须在其他 CPU 启动前完成的初始化。
	 */
	do_pre_smp_initcalls();

	/* 初始化 CPU 死锁检测（watchdog/NMI），监控 CPU 是否卡死 */
	lockup_detector_init();

	/*
	 * 启动所有其他 CPU（SMP 初始化）。
	 * 从这里开始系统变成真正的多核系统。
	 * 每个 CPU 执行 secondary_startup → secondary_start_kernel。
	 */
	smp_init();

	/* 初始化 SMP 调度（建立完整的 CPU 拓扑，迁移任务到各 CPU） */
	sched_init_smp();

	/* 根据 CPU 拓扑（NUMA/缓存层次）优化工作队列的 CPU 绑定策略 */
	workqueue_init_topology();

	/* 初始化异步函数调用框架（async_schedule 等） */
	async_init();

	/* 初始化并行数据处理框架（加密、校验等并行计算） */
	padata_init();

	/* 页分配器的晚期初始化（处理内存热插拔等） */
	page_alloc_init_late();

	/*
	 * do_basic_setup()：运行所有 initcalls（level 1-7）。
	 * 这是驱动初始化的核心，包括：
	 *   - 总线驱动（PCI/USB/I2C 等）
	 *   - 存储驱动（eMMC/UFS/SATA 等）
	 *   - 网络驱动
	 *   - 文件系统注册
	 *   - Android 相关驱动（Binder/ion 等）
	 * 执行完后所有内置驱动都已初始化。
	 */
	do_basic_setup();

	/* 运行所有 KUnit 测试（内核单元测试框架，调试内核时使用） */
	kunit_run_all_tests();

	/* 等待 initramfs 完全加载到内存 */
	wait_for_initramfs();

	/*
	 * 打开 /dev/console，绑定到 stdin/stdout/stderr（fd 0/1/2）。
	 * init 进程需要有控制台才能输出信息。
	 */
	console_on_rootfs();

	/*
	 * 检查 ramdisk 中是否存在 init 程序。
	 * 如果 ramdisk_execute_command（默认 "/init"）可访问，
	 * 就用它作为 init（Android 就走这条路）。
	 * 如果不可访问，则调用 prepare_namespace() 挂载真正的根文件系统，
	 * 再从磁盘上找 init。
	 */
	int ramdisk_command_access;
	ramdisk_command_access = init_eaccess(ramdisk_execute_command);
	if (ramdisk_command_access != 0) {
		if (ramdisk_execute_command_set)
			pr_warn("check access for rdinit=%s failed: %i, ignoring\n",
				ramdisk_execute_command, ramdisk_command_access);
		ramdisk_execute_command = NULL;
		/* 没有 ramdisk init，挂载真实根文件系统 */
		prepare_namespace();
	}

	/*
	 * 初始启动完成，系统基本运行起来了。
	 * 释放 initmem 段，启动用户态...
	 *
	 * 根文件系统现在可用，加载公钥和默认模块。
	 */

	/* 加载完整性验证公钥（IMA/EVM 等安全特性使用） */
	integrity_load_keys();
}
