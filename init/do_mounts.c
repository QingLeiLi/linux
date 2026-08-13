// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/ctype.h>
#include <linux/fd.h>
#include <linux/tty.h>
#include <linux/suspend.h>
#include <linux/root_dev.h>
#include <linux/security.h>
#include <linux/delay.h>
#include <linux/mount.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/initrd.h>
#include <linux/async.h>
#include <linux/fs_struct.h>
#include <linux/slab.h>
#include <linux/ramfs.h>
#include <linux/shmem_fs.h>
#include <linux/ktime.h>

#include <linux/nfs_fs.h>
#include <linux/nfs_fs_sb.h>
#include <linux/nfs_mount.h>
#include <linux/raid/detect.h>
#include <uapi/linux/mount.h>

#include "do_mounts.h"

/*
 * 根文件系统挂载状态地图：
 *
 * prepare_namespace() 在设备探测和 initrd 处理后解析 root=，按 ROOT_DEV 分派
 * NFS、CIFS、无块设备文件系统或普通块设备挂载，最终把进程 1 的根和 cwd
 * pivot 到新 rootfs。这里的参数处理器都在单线程启动早期运行；带 __initdata
 * 的字符串/数值只服务启动，init section 回收后不得再引用。
 */

/* 根挂载的 VFS 标志；默认只读且抑制部分消息，ro/rw 参数只切换只读位。 */
int root_mountflags = MS_RDONLY | MS_SILENT;
/* `root=` 原始借用值的固定副本，最多保存 63 字节加 NUL，供稍后设备解析。 */
static char __initdata saved_root_name[64];
/* 0 表示不额外等待，-1 表示无限等待，正数表示 rootwait 秒数换算后的毫秒。 */
static int root_wait;

/* 解析后根设备号或 Root_NFS/Root_CIFS/Root_Generic 等特殊哨兵，由挂载分派读取。 */
dev_t ROOT_DEV;

/*
 * readonly() - 处理无值 `ro` 启动参数，把根挂载策略设为只读
 *
 * 调用者：启动命令行 __setup 解析器。@str 是参数名后的借用字符串；合法
 * `ro` 应为空，非空表示当前 handler 不接受。函数在单线程 __init 上下文
 * 运行、可不持锁且不睡眠。成功置 MS_RDONLY 并返回 1 表示已消费；非空返回
 * 0 且不改变状态，解析器可按未知/未处理参数继续处置。无所有权转移。
 */
static int __init readonly(char *str)
{
	/* 只接受精确的无值形式，避免把 `ro=something` 静默当成有效策略。 */
	if (*str)
		return 0;
	/* 仅增加只读位，保留 MS_SILENT 等其他根挂载标志。 */
	root_mountflags |= MS_RDONLY;
	return 1;
}

/*
 * readwrite() - 处理无值 `rw` 启动参数，把根挂载策略设为可写
 *
 * 调用者、上下文和 @str ownership 与 readonly() 相同。空参数清除
 * MS_RDONLY、保留其他标志并返回 1；非空返回 0 且无副作用。若命令行同时
 * 出现 ro/rw，启动参数的顺序解析使后出现者决定最终只读位。
 */
static int __init readwrite(char *str)
{
	/* 非空后缀不是本 handler 的合法 ABI。 */
	if (*str)
		return 0;
	/* 清除只读位后，实际文件系统仍可能因自身能力或错误退化为只读。 */
	root_mountflags &= ~MS_RDONLY;
	return 1;
}

/* 将两个无值参数绑定到各自启动期 handler；注册本身不在运行期保留。 */
__setup("ro", readonly);
__setup("rw", readwrite);

/*
 * root_dev_setup() - 保存 `root=` 指定的根设备/网络根/文件系统名称
 *
 * @line 是命令行缓冲区中的 NUL 结尾借用字符串，可为空；函数在启动参数解析
 * 阶段将它复制到永久到 init 回收前有效的 saved_root_name，不持锁、不睡眠，
 * 也不保留输入指针。strscpy() 保证目标 NUL 结尾，超长名称会截断；本 handler
 * 始终返回 1 表示已消费，稍后 parse_root_device() 才验证并分类该值。
 */
static int __init root_dev_setup(char *line)
{
	/* 延迟语义解析，使块设备枚举和网络根准备可以在后续启动阶段完成。 */
	strscpy(saved_root_name, line, sizeof(saved_root_name));
	return 1;
}

__setup("root=", root_dev_setup);

/*
 * rootwait_setup() - 处理无值 `rootwait`，要求无限等待根设备出现
 *
 * @str 为借用后缀，必须为空。成功把 root_wait 设为 -1 哨兵并返回 1；非空
 * 返回 0 且保持原值。该函数不睡眠，真正等待由 prepare_namespace() 后续调用
 * wait_for_root() 完成，因此命令行解析本身不会阻塞。
 */
static int __init rootwait_setup(char *str)
{
	if (*str)
		return 0;
	/* -1 与所有正超时值区分，等待循环不会触发期限分支。 */
	root_wait = -1;
	return 1;
}

__setup("rootwait", rootwait_setup);

/*
 * rootwait_timeout_setup() - 解析 `rootwait=<秒>` 并保存毫秒超时
 *
 * @str 是 NUL 结尾借用文本，不可为空；允许 kstrtoint(base=0) 接受的进制，
 * 但秒数必须非负。函数在启动参数解析上下文运行，不持锁、不睡眠。成功把秒
 * 安全换算成毫秒写入 root_wait 并返回 1；语法错误、负数或乘法溢出均告警，
 * 退化为 -1 无限等待并仍返回 1，避免无效已知参数再落入 unknown 参数路径。
 */
static int __init rootwait_timeout_setup(char *str)
{
	/* sec 保存用户输入秒数；通过检查后才允许参与单位换算。 */
	int sec;

	/* 同时拒绝非整数和负期限；0 是合法值，表示等待循环可立即到期。 */
	if (kstrtoint(str, 0, &sec) || sec < 0) {
		pr_warn("ignoring invalid rootwait value\n");
		goto ignore;
	}

	/* root_wait 使用毫秒，显式溢出检查防止大秒数绕回短等待或负值。 */
	if (check_mul_overflow(sec, MSEC_PER_SEC, &root_wait)) {
		pr_warn("ignoring excessive rootwait value\n");
		goto ignore;
	}

	return 1;

ignore:
	/* Fallback to indefinite wait */
	/* 无效有限期限回退为无限等待，比过早挂载失败更符合 rootwait 的用户意图。 */
	root_wait = -1;

	return 1;
}

__setup("rootwait=", rootwait_timeout_setup);

/* `rootflags=` 的借用命令行字符串，作为文件系统 mount data 传给根挂载。 */
static char * __initdata root_mount_data;

/*
 * root_data_setup() - 保存 `rootflags=` 文件系统专用挂载参数
 *
 * @str 是启动命令行缓冲区中的 NUL 结尾借用字符串，可为空；该缓冲区在根
 * 挂载阶段仍有效，函数不复制也不取得释放责任。它在单线程 __init 上下文
 * 不持锁、不睡眠，保存指针并返回 1。do_mount_root() 稍后复制为完整页交给
 * VFS；参数是否合法由目标文件系统解析，当前 handler 不预先验证。
 */
static int __init root_data_setup(char *str)
{
	root_mount_data = str;
	return 1;
}

/* `rootfstype=` 的借用逗号分隔列表；NULL 表示自动枚举块文件系统。 */
static char * __initdata root_fs_names;

/*
 * fs_names_setup() - 保存 `rootfstype=` 指定的文件系统候选列表
 *
 * @str 是命令行缓冲区内借用字符串，允许空项和逗号分隔多项；函数不复制、
 * 不验证驱动是否存在，也不转移 ownership。它不持锁、不睡眠，始终返回 1。
 * split_fs_names() 在挂载阶段才把只读来源复制到可修改页并拆成 NUL 字符串组。
 */
static int __init fs_names_setup(char *str)
{
	root_fs_names = str;
	return 1;
}

/* `rootdelay=` 的无符号秒数；0 表示 prepare_namespace() 不主动延时。 */
static unsigned int __initdata root_delay;

/*
 * root_delay_setup() - 解析 `rootdelay=<秒>` 的固定启动延时
 *
 * @str 是借用数字文本；kstrtouint(base=0) 接受常见进制并拒绝负数/溢出。
 * 成功更新 root_delay、返回 1；失败返回 0，root_delay 保持此前值。函数只做
 * 解析且不睡眠，真正的秒级 ssleep() 位于 prepare_namespace() 挂根之前。
 */
static int __init root_delay_setup(char *str)
{
	if (kstrtouint(str, 0, &root_delay))
		return 0;
	return 1;
}

/* 注册三个有值启动参数；handler 返回 1 后参数不会再传给后续解析层。 */
__setup("rootflags=", root_data_setup);
__setup("rootfstype=", fs_names_setup);
__setup("rootdelay=", root_delay_setup);

/* This can return zero length strings. Caller should check */
/*
 * 该函数可能生成零长度项，调用者必须检查。连续逗号或首尾逗号都会在输出
 * NUL 字符串组中形成空字符串，返回 count 仍包含这些位置。
 */
/*
 * split_fs_names() - 把 rootfstype 逗号列表复制并原地拆成 NUL 字符串组
 *
 * @page 是调用者持有的可写输出缓冲区，不能为 NULL；@size 是其字节容量。
 * root_fs_names 必须已由 fs_names_setup() 设置。函数在 __init 上下文不分配、
 * 不睡眠：strscpy() 最多复制 size 字节并保证 NUL，随后把每个逗号替换为
 * NUL。返回值是项位置数量，至少为 1且包含空项；page ownership 不变，调用者
 * 通过 `strlen(p)+1` 遍历，并负责最终释放。
 */
static int __init split_fs_names(char *page, size_t size)
{
	/* count 计数逻辑项；p 始终指向已复制页内当前扫描字符。 */
	int count = 1;
	char *p = page;

	/* 不修改命令行原串，因为它还可能用于诊断或其他启动期读取。 */
	strscpy(p, root_fs_names, size);
	/* 每发现一个分隔符就终止当前项并增加下一项的位置计数。 */
	while (*p++) {
		if (p[-1] == ',') {
			p[-1] = '\0';
			count++;
		}
	}

	return count;
}

/*
 * do_mount_root() - 用一个指定文件系统把根来源挂到临时 `/root`
 *
 * 调用链：mount_root_generic()/网络根/nodev 根 → 本函数 → init_mount()。
 * @name 是设备名或网络/伪文件系统来源字符串，借用、不可为 NULL；@fs 是文件
 * 系统类型名，借用、不可为 NULL；@flags 是 MS_/SB_ 兼容挂载标志值；@data
 * 是可空的文件系统专用 NUL 文本，纯输入且不转移 ownership。进程 1 的启动
 * 上下文可睡眠，入口不持 VFS 锁。
 *
 * 有 data 时分配整页并用 NUL 填充，满足 init_mount() 第五参数必须可访问完整
 * 页的内部契约。挂载成功后 chdir 到 `/root`，从 current->fs->pwd 取得已挂载
 * superblock，发布其 s_dev 到 ROOT_DEV 并打印最终类型/只读状态。返回 0 表示
 * `/root` 已挂载且 cwd/ROOT_DEV 已更新；返回 -ENOMEM 或 init_mount errno 时
 * 外部可观察根状态未由本函数提交。临时页在所有出口释放。
 */
static int __init do_mount_root(const char *name, const char *fs,
				 const int flags, const void *data)
{
	/* s 借用挂载后 pwd 的 superblock；data_page 是本函数持有的临时整页。 */
	struct super_block *s;
	char *data_page = NULL;
	/* ret 原样承载 init_mount() 的 0 或负 errno。 */
	int ret;

	if (data) {
		/* init_mount() requires a full page as fifth argument */
		/* init_mount() 的第五参数要求完整一页；不能直接传长度未知的命令行片段。 */
		data_page = kmalloc(PAGE_SIZE, GFP_KERNEL);
		if (!data_page)
			return -ENOMEM;
		strscpy_pad(data_page, data, PAGE_SIZE);
	}

	/* VFS 在 `/root` 创建挂载；失败时尚不能读取该路径的新 superblock。 */
	ret = init_mount(name, "/root", fs, flags, data_page);
	if (ret)
		goto out;

	/* 挂载成功是提交边界；切换 cwd 后可从路径稳定读取实际 superblock 属性。 */
	init_chdir("/root");
	s = current->fs->pwd.dentry->d_sb;
	ROOT_DEV = s->s_dev;
	printk(KERN_INFO
	       "VFS: Mounted root (%s filesystem)%s on device %u:%u.\n",
	       s->s_type->name,
	       sb_rdonly(s) ? " readonly" : "",
	       MAJOR(ROOT_DEV), MINOR(ROOT_DEV));

out:
	/* VFS 已在 init_mount() 返回前消费参数内容，不取得临时页 ownership。 */
	kfree(data_page);
	return ret;
}

/*
 * mount_root_generic() - 依次尝试候选块文件系统并在必要时退化为只读挂载
 *
 * 调用者：Root_Generic 分支或 mount_block_root()。@name 是实际传给 VFS 的
 * 来源路径，借用、不可为 NULL；@pretty_name 是诊断用 root= 文本，借用、
 * 可与 name 相同；@flags 是初始挂载标志。函数在进程 1 的 __init 上下文可
 * 睡眠且入口不持锁。候选来自用户 rootfstype=，否则由已注册块文件系统列表
 * 生成。成功时 do_mount_root() 已挂载 `/root`、更新 cwd/ROOT_DEV，本函数释放
 * 名单页后返回；所有可接受候选失败时先把可写请求退化成只读再完整重试。
 * 内存不足、设备打开类硬错误或两轮均无可挂载文件系统会 panic，因启动无法
 * 在没有根文件系统时安全继续。
 */
void __init mount_root_generic(char *name, char *pretty_name, int flags)
{
	/* fs_names 由本函数持有；p 游走 NUL 字符串组，b 保存稳定的设备号诊断名。 */
	char *fs_names = kmalloc(PAGE_SIZE, GFP_KERNEL);
	char *p;
	char b[BDEVNAME_SIZE];
	/* num_fs 是候选位置数，i 是当前索引。 */
	int num_fs, i;

	if (!fs_names)
		panic("VFS: Unable to mount root fs: not enough memory");

	/* 即使 root= 名称不可用，也能用已解析 major:minor 精确报告目标。 */
	scnprintf(b, BDEVNAME_SIZE, "unknown-block(%u,%u)",
		  MAJOR(ROOT_DEV), MINOR(ROOT_DEV));
	if (root_fs_names)
		num_fs = split_fs_names(fs_names, PAGE_SIZE);
	else
		num_fs = list_bdev_fs_names(fs_names, PAGE_SIZE);
retry:
	/* 一轮中按用户顺序或注册顺序尝试；空项来自连续/首尾逗号，必须跳过。 */
	for (i = 0, p = fs_names; i < num_fs; i++, p += strlen(p)+1) {
		/* err 仅描述当前文件系统尝试，不跨候选保留。 */
		int err;

		if (!*p)
			continue;
		err = do_mount_root(name, p, flags, root_mount_data);
		/* 成功提交；EACCES/EINVAL 常表示该 fs 不匹配，可安全换下一个候选。 */
		switch (err) {
			case 0:
				goto out;
			case -EACCES:
			case -EINVAL:
#ifdef CONFIG_BLOCK
				/* 等待失败尝试触发的延迟 close 完成，避免后续 open 看见伪冲突。 */
				init_flush_fput();
#endif
				continue;
		}
	        /*
		 * Allow the user to distinguish between failed sys_open
		 * and bad superblock on root device.
		 * and give them a list of the available devices
		 */
		/*
		 * 让用户区分根设备 sys_open 失败与 superblock 不匹配，并列出当前
		 * 可用分区。非 EACCES/EINVAL 被视为设备/挂载硬错误，不再猜测其他 fs。
		 */
		printk("VFS: Cannot open root device \"%s\" or %s: error %d\n",
				pretty_name, b, err);
		printk("Please append a correct \"root=\" boot option; here are the available partitions:\n");
		printk_all_partitions();

		/* 用户限制过类型时，失败诊断改列出内核全部块文件系统，帮助修正参数。 */
		if (root_fs_names)
			num_fs = list_bdev_fs_names(fs_names, PAGE_SIZE);
		if (!num_fs)
			pr_err("Can't find any bdev filesystem to be used for mount!\n");
		else {
			pr_err("List of all bdev filesystems:\n");
			for (i = 0, p = fs_names; i < num_fs; i++, p += strlen(p)+1)
				pr_err(" %s", p);
			pr_err("\n");
		}

		panic("VFS: Unable to mount root fs on %s", b);
	}
	/* 一轮仅得到“不匹配”时，可写请求最后再以只读标志完整重试一次。 */
	if (!(flags & SB_RDONLY)) {
		flags |= SB_RDONLY;
		goto retry;
	}

	printk("List of all partitions:\n");
	printk_all_partitions();
	printk("No filesystem could mount root, tried: ");
	for (i = 0, p = fs_names; i < num_fs; i++, p += strlen(p)+1)
		printk(" %s", p);
	printk("\n");
	panic("VFS: Unable to mount root fs on \"%s\" or %s", pretty_name, b);
out:
	/* 只有成功路径可达；名单页始终由本函数释放。 */
	kfree(fs_names);
}
 
#ifdef CONFIG_ROOT_NFS

/* NFS 重试睡眠单位均为秒：从 5 秒开始、30 秒封顶、允许 5 次等待后重试。 */
#define NFSROOT_TIMEOUT_MIN	5
#define NFSROOT_TIMEOUT_MAX	30
#define NFSROOT_RETRY_MAX	5

/*
 * mount_nfs_root() - 解析 NFS 根参数并以指数退避重试网络根挂载
 *
 * 仅在 CONFIG_ROOT_NFS 下存在，由 mount_root() 的 Root_NFS 分支调用。入参、
 * 直接返回值均无；nfs_root_data() 通过 @root_dev/@root_data 输出借用字符串，
 * 本函数不释放。进程 1 上下文可睡眠，不持锁。首次立即尝试，随后最多睡眠
 * 5 次并进行第 6 次挂载；等待从 5 秒倍增并封顶 30 秒。成功时 `/root` 已提交；
 * 参数解析或全部尝试失败仅打印错误并返回，由上层启动流程决定后续命运。
 */
static void __init mount_nfs_root(void)
{
	/* root_dev/root_data 是 NFS helper 输出；timeout 单位秒，try 从 1 计尝试轮次。 */
	char *root_dev, *root_data;
	unsigned int timeout;
	int try;

	if (nfs_root_data(&root_dev, &root_data))
		goto fail;

	/*
	 * The server or network may not be ready, so try several
	 * times.  Stop after a few tries in case the client wants
	 * to fall back to other boot methods.
	 */
	/*
	 * 服务端或网络可能尚未就绪，因此重试数次；有限上限避免阻止客户端回退
	 * 到其他启动方式。每轮 do_mount_root() 自行释放其 mount-data 临时页。
	 */
	timeout = NFSROOT_TIMEOUT_MIN;
	for (try = 1; ; try++) {
		if (!do_mount_root(root_dev, "nfs", root_mountflags, root_data))
			return;
		if (try > NFSROOT_RETRY_MAX)
			break;

		/* Wait, in case the server refused us immediately */
		/* 即使服务器立即拒绝也等待，给链路配置和远端服务留下恢复时间。 */
		ssleep(timeout);
		timeout <<= 1;
		if (timeout > NFSROOT_TIMEOUT_MAX)
			timeout = NFSROOT_TIMEOUT_MAX;
	}
fail:
	/* 到达时没有成功 root mount，也没有本函数持有的动态资源需要回滚。 */
	pr_err("VFS: Unable to mount root fs via NFS.\n");
}
#else
/*
 * mount_nfs_root() - CONFIG_ROOT_NFS 关闭时的无操作桩
 *
 * 无入参、无返回值、无副作用且不睡眠；保持 mount_root() 调用结构统一。
 */
static inline void mount_nfs_root(void)
{
}
#endif /* CONFIG_ROOT_NFS */

#ifdef CONFIG_CIFS_ROOT

/* CIFS/SMB 与 NFS 使用相同的 5→10→20→30 秒有界退避策略。 */
#define CIFSROOT_TIMEOUT_MIN	5
#define CIFSROOT_TIMEOUT_MAX	30
#define CIFSROOT_RETRY_MAX	5

/*
 * mount_cifs_root() - 解析 CIFS/SMB 根参数并以指数退避重试
 *
 * 仅在 CONFIG_CIFS_ROOT 下由 Root_CIFS 分支调用。无入参和直接返回值；
 * cifs_root_data() 输出的 root_dev/root_data 为借用字符串。上下文可睡眠，
 * 尝试/等待次数与 NFS 相同：首次立即尝试、5 次睡眠、最多第 6 次挂载，间隔
 * 5 秒倍增并封顶 30 秒。成功提交 `/root` 后返回；解析或重试失败打印 SMB
 * 根错误，不取得输出字符串 ownership。
 */
static void __init mount_cifs_root(void)
{
	/* timeout 以秒计，try 记录已发起的挂载轮次。 */
	char *root_dev, *root_data;
	unsigned int timeout;
	int try;

	if (cifs_root_data(&root_dev, &root_data))
		goto fail;

	/* 网络和服务端启动竞态用有界指数退避吸收。 */
	timeout = CIFSROOT_TIMEOUT_MIN;
	for (try = 1; ; try++) {
		if (!do_mount_root(root_dev, "cifs", root_mountflags,
				   root_data))
			return;
		if (try > CIFSROOT_RETRY_MAX)
			break;

		ssleep(timeout);
		timeout <<= 1;
		if (timeout > CIFSROOT_TIMEOUT_MAX)
			timeout = CIFSROOT_TIMEOUT_MAX;
	}
fail:
	/* 参数输出均为借用指针；失败出口只报告，不释放 helper 管理的数据。 */
	pr_err("VFS: Unable to mount root fs via SMB.\n");
}
#else
/*
 * mount_cifs_root() - CONFIG_CIFS_ROOT 关闭时的无操作桩
 *
 * 无入参、无返回值、无副作用且不睡眠；使通用分派无需散布条件编译。
 */
static inline void mount_cifs_root(void)
{
}
#endif /* CONFIG_CIFS_ROOT */

/*
 * fs_is_nodev() - 判断文件系统类型是否无需块设备即可挂载
 *
 * @fstype 是借用 NUL 类型名，不可为 NULL。get_fs_type() 可能请求模块并返回
 * 带引用的 file_system_type；函数可睡眠、入口不持锁。找到类型后检查其
 * FS_REQUIRES_DEV 标志并用 put_filesystem() 对称释放引用。返回 true 仅表示
 * 已注册且不要求设备；未知类型和要求设备都返回 false，无引用泄漏。
 */
static bool __init fs_is_nodev(char *fstype)
{
	/* fs 在成功查找后持有临时引用；ret 默认把未知类型按不可用处理。 */
	struct file_system_type *fs = get_fs_type(fstype);
	bool ret = false;

	if (fs) {
		ret = !(fs->fs_flags & FS_REQUIRES_DEV);
		put_filesystem(fs);
	}

	return ret;
}

/*
 * mount_nodev_root() - 在 rootfstype 明确列表中尝试无需块设备的根文件系统
 *
 * @root_device_name 是借用的 root= 来源字符串，可供伪文件系统解释但不转移
 * ownership。调用前 root_fs_names 必须非 NULL。函数在 __init 进程上下文可
 * 睡眠：分配一页拆分类型，只尝试已注册且未标 FS_REQUIRES_DEV 的候选，并把
 * rootflags 数据传给 do_mount_root()。返回 0 表示 `/root` 已挂载；否则返回
 * -EINVAL（含分配失败/无合格项）或最后一次 nodev 挂载 errno。名单页总会释放。
 */
static int __init mount_nodev_root(char *root_device_name)
{
	/* fs_names 由本函数持有；fstype 遍历页内项；err 保存最后一次有效尝试结果。 */
	char *fs_names, *fstype;
	int err = -EINVAL;
	int num_fs, i;

	fs_names = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!fs_names)
		return -EINVAL;
	num_fs = split_fs_names(fs_names, PAGE_SIZE);

	/* 用户顺序有语义：第一个成功的 nodev 文件系统赢得根挂载。 */
	for (i = 0, fstype = fs_names; i < num_fs;
	     i++, fstype += strlen(fstype) + 1) {
		if (!*fstype)
			continue;
		if (!fs_is_nodev(fstype))
			continue;
		err = do_mount_root(root_device_name, fstype, root_mountflags,
				    root_mount_data);
		if (!err)
			break;
	}

	kfree(fs_names);
	return err;
}

#ifdef CONFIG_BLOCK
/*
 * mount_block_root() - 创建设备节点并挂载已解析的块设备根
 *
 * @root_device_name 是借用诊断名，不可为 NULL；ROOT_DEV 已是有效 dev_t。
 * 函数在进程 1 的 __init 上下文可睡眠。create_dev() 先替换 `/dev/root` 为
 * 指向 ROOT_DEV 的 0600 块设备节点；创建失败会紧急告警但仍进入通用挂载，
 * 后者成功返回或 panic，不提供 errno。无 ownership 转移。
 */
static void __init mount_block_root(char *root_device_name)
{
	/* err 只用于报告设备节点构造，挂载失败由 mount_root_generic() 诊断。 */
	int err = create_dev("/dev/root", ROOT_DEV);

	if (err < 0)
		pr_emerg("Failed to create /dev/root: %d\n", err);
	mount_root_generic("/dev/root", root_device_name, root_mountflags);
}
#else
/*
 * mount_block_root() - CONFIG_BLOCK 关闭时的无操作桩
 *
 * @root_device_name 仅保持签名一致，函数不读取、不睡眠且无副作用。有效配置
 * 应通过 nodev/网络根分支启动；块设备根在此配置下无法由本函数挂载。
 */
static inline void mount_block_root(char *root_device_name)
{
}
#endif /* CONFIG_BLOCK */

/*
 * mount_root() - 根据 ROOT_DEV 分类选择网络、通用、nodev 或块设备根挂载器
 *
 * @root_device_name 是借用 root= 文本，部分分支允许 NULL；函数无直接返回值，
 * 在进程 1 的 __init 上下文可睡眠且入口不持锁。Root_NFS/Root_CIFS 进入网络
 * helper，Root_Generic 直接枚举文件系统，已解析 dev_t 进入块设备路径。
 * ROOT_DEV==0 时，只有同时给出名称与 rootfstype 才先尝试 nodev；失败再显式
 * 落入块设备路径。成功保证 `/root` 已挂载；通用/块路径不可恢复失败
 * 会 panic，网络 helper 失败则仅记录错误后返回。
 */
void __init mount_root(char *root_device_name)
{
	/* ROOT_DEV 同时承载真正 dev_t 和不会与其冲突的 Root_* 特殊哨兵。 */
	switch (ROOT_DEV) {
	case Root_NFS:
		mount_nfs_root();
		break;
	case Root_CIFS:
		mount_cifs_root();
		break;
	case Root_Generic:
		mount_root_generic(root_device_name, root_device_name,
				   root_mountflags);
		break;
	case 0:
		/* 未解析成设备时，用户明确的 nodev 类型仍可能直接构造根 superblock。 */
		if (root_device_name && root_fs_names &&
		    mount_nodev_root(root_device_name) == 0)
			break;
		/* nodev 不适用或失败，按普通块设备路径处理并由其给出最终诊断。 */
		fallthrough;
	default:
		mount_block_root(root_device_name);
		break;
	}
}

/* wait for any asynchronous scanning to complete */
/* 等待所有异步扫描结束，确保随后挂载不与尚未完成的设备发现并发。 */
/*
 * wait_for_root() - 等待驱动探测完成并把 root= 解析为可用块设备
 *
 * @root_device_name 是借用设备名，调用时不可为 NULL。仅当 ROOT_DEV==0 时
 * 工作；root_wait 为 -1 时无限等待、正数时表示毫秒期限。函数在进程上下文
 * 周期性睡眠 5ms，可睡眠且不持锁；循环同时要求 driver_probe_done() 且
 * early_lookup_bdev() 成功，成功时后者写 ROOT_DEV。有限期限到达可带着
 * ROOT_DEV==0 退出。最后无条件 async_synchronize_full()，保证全局异步启动
 * 工作完成后才继续挂根；函数无直接返回值、无指针 ownership 变化。
 */
static void __init wait_for_root(char *root_device_name)
{
	/* end 是 raw monotonic 时钟上的绝对毫秒期限，仅 root_wait>0 时比较。 */
	ktime_t end;

	if (ROOT_DEV != 0)
		return;

	pr_info("Waiting for root device %s...\n", root_device_name);

	end = ktime_add_ms(ktime_get_raw(), root_wait);

	/* 驱动仍在 probe 或目标尚不可解析时继续；逻辑 OR 防止过早承诺设备稳定。 */
	while (!driver_probe_done() ||
	       early_lookup_bdev(root_device_name, &ROOT_DEV) < 0) {
		msleep(5);
		if (root_wait > 0 && ktime_after(ktime_get_raw(), end))
			break;
	}

	/* 即使有限 rootwait 超时，也收拢异步扫描再让 mount_root() 做最终判断。 */
	async_synchronize_full();

}

/*
 * parse_root_device() - 把 root= 文本分类为特殊根类型或具体 dev_t
 *
 * @root_device_name 是借用 NUL 字符串，不可为 NULL；函数在 __init 上下文
 * 可能经 early_lookup_bdev() 查询设备，入口不持锁。mtd/ubi 返回 Root_Generic，
 * `/dev/nfs`、`/dev/cifs`、`/dev/ram` 返回各自哨兵；普通名称成功返回 dev_t。
 * 查找失败返回 0 表示尚未解析。若错误是 -EINVAL，名称本身无效，继续 rootwait
 * 永远无意义，因此清零 root_wait；暂时不存在等其他错误保留等待策略。
 */
static dev_t __init parse_root_device(char *root_device_name)
{
	/* error 保存 lookup errno；dev 仅在成功时初始化并返回。 */
	int error;
	dev_t dev;

	/* 这些来源没有普通块 dev_t 或需要专用挂载协议，先映射为分派哨兵。 */
	if (!strncmp(root_device_name, "mtd", 3) ||
	    !strncmp(root_device_name, "ubi", 3))
		return Root_Generic;
	if (strcmp(root_device_name, "/dev/nfs") == 0)
		return Root_NFS;
	if (strcmp(root_device_name, "/dev/cifs") == 0)
		return Root_CIFS;
	if (strcmp(root_device_name, "/dev/ram") == 0)
		return Root_RAM0;

	/* 普通设备名可能因 probe 尚未完成而暂时失败，交给 rootwait 路径重查。 */
	error = early_lookup_bdev(root_device_name, &dev);
	if (error) {
		if (error == -EINVAL && root_wait) {
			pr_err("Disabling rootwait; root= is invalid.\n");
			root_wait = 0;
		}
		return 0;
	}
	return dev;
}

/*
 * Prepare the namespace - decide what/where to mount, load ramdisks, etc.
 */
/*
 * 准备 mount namespace：决定根来源和挂载位置，并处理 ramdisk/initrd 等启动介质。
 */
/*
 * prepare_namespace() - 等待存储就绪、挂载根并切换进程 1 的根 namespace
 *
 * 调用者位于内核启动主线，在命令行解析、驱动初始化和初始 rootfs 建立后进入。
 * 入参、直接返回值均无；函数运行于可睡眠的进程 1 __init 上下文，入口不持锁。
 * 主要阶段：应用 rootdelay；收拢设备 probe；启动 MD；解析 root=；尝试 legacy
 * initrd；按 rootwait 等待并挂载 `/root`；在新根挂 devtmpfs；以 `pivot_root(.,.)`
 * 把当前 `/root` 提升为 namespace 根，再 lazy-unmount 旧 rootfs。
 *
 * 成功返回时 cwd/root 已位于新根且旧 rootfs 已摘除。pivot 或 unmount 失败只
 * 打印错误并提前返回：此前挂载和部分 namespace 状态不回滚，本函数不是事务。
 */
void __init prepare_namespace(void)
{
	/* rootdelay 是无条件固定等待，发生在任何设备完成检查之前。 */
	if (root_delay) {
		printk(KERN_INFO "Waiting %d sec before mounting root device...\n",
		       root_delay);
		ssleep(root_delay);
	}

	/*
	 * wait for the known devices to complete their probing
	 *
	 * Note: this is a potential source of long boot delays.
	 * For example, it is not atypical to wait 5 seconds here
	 * for the touchpad of a laptop to initialize.
	 */
	/*
	 * 等待已知设备完成 probe。这里可能造成较长启动延迟，例如笔记本触摸板
	 * 初始化常让全局 probe 等待约 5 秒；helper 还刷新 deferred probe 和 async。
	 */
	wait_for_device_probe();

	/* 在根设备解析前完成 RAID 自动探测/命令行阵列组装，使阵列 dev_t 可被找到。 */
	md_run_setup();

	/* 只有显式非空 root= 才先分类；否则 ROOT_DEV 保留先前默认/架构设置。 */
	if (saved_root_name[0])
		ROOT_DEV = parse_root_device(saved_root_name);

	/* 传统 initrd 可接管启动；配置关闭时这是 do_mounts.h 中的无操作桩。 */
	initrd_load();

	/* rootwait 只对尚未解析的普通设备有效；随后分派最终根挂载实现。 */
	if (root_wait)
		wait_for_root(saved_root_name);
	mount_root(saved_root_name);
	/* 若配置/命令行要求，在新根的 dev/ 上发布 devtmpfs；失败仅由 helper 记录。 */
	devtmpfs_mount();

	/* cwd 已由 do_mount_root() 设为新 `/root`；同点 pivot 把旧根叠到当前点。 */
	if (init_pivot_root(".", ".")) {
		pr_err("VFS: Failed to pivot into new rootfs\n");
		return;
	}
	/* lazy detach 被 pivot 留下的旧 rootfs；活动引用可延迟释放，但新查找不再进入。 */
	if (init_umount(".", MNT_DETACH)) {
		pr_err("VFS: Failed to unmount old rootfs\n");
		return;
	}
	pr_info("VFS: Pivoted into new rootfs\n");
}

/* 启动期确定 rootfs 后端：false 为 ramfs，true 为 shmem/tmpfs；注册后只读。 */
static bool is_tmpfs;

/*
 * rootfs_init_fs_context() - 为 rootfs mount 选择 ramfs 或 shmem/tmpfs fs_context
 *
 * @fc 是 VFS 创建并持有的可写 fs_context，不能为 NULL；本函数借用它且不
 * 转移 ownership。调用发生在 rootfs 文件系统类型创建 superblock 的上下文，
 * 可由后端初始化分配/失败。CONFIG_TMPFS 编入且 init_rootfs() 选择 tmpfs 时
 * 委托 shmem_init_fs_context()，否则委托 ramfs_init_fs_context()；返回后端的
 * 0 或负 errno，副作用和 cleanup 责任遵循相应后端 fs_context 契约。
 */
static int rootfs_init_fs_context(struct fs_context *fc)
{
	/* IS_ENABLED 在未编入 TMPFS 时编译期为假，不引用不可用的 shmem 后端。 */
	if (IS_ENABLED(CONFIG_TMPFS) && is_tmpfs)
		return shmem_init_fs_context(fc);

	return ramfs_init_fs_context(fc);
}

/*
 * 永久注册的 rootfs 文件系统类型：名称用于 VFS 查找，init 回调按启动选择
 * 构造 ramfs/tmpfs context，匿名 superblock 由 kill_anon_super() 统一销毁。
 * 对象由文件系统注册表借用并贯穿内核运行期，不属于 __init section。
 */
struct file_system_type rootfs_fs_type = {
	.name		= "rootfs",
	.init_fs_context = rootfs_init_fs_context,
	.kill_sb	= kill_anon_super,
};

/*
 * init_rootfs() - 根据启动参数选择初始 rootfs 使用 tmpfs 还是 ramfs
 *
 * 无入参、无直接返回值；在命令行已解析且 rootfs 首次实例化前的单线程
 * __init 上下文运行，不持锁、不睡眠。CONFIG_TMPFS 关闭时保持 ramfs。启用时，
 * 未给 root= 且未限制 rootfstype 的默认场景选择 tmpfs；显式 rootfstype 列表
 * 只要包含子串 `tmpfs` 也选择 tmpfs。函数只写一次 is_tmpfs，不创建/挂载对象；
 * 后续 rootfs_init_fs_context() 消费该决定。
 */
void __init init_rootfs(void)
{
	/* 编译期无 tmpfs 支持时整个分支消失，rootfs 必然使用 ramfs。 */
	if (IS_ENABLED(CONFIG_TMPFS)) {
		/* 没有外部根提示时，优先使用可 swap、有限额的完整 shmem 后端。 */
		if (!saved_root_name[0] && !root_fs_names)
			is_tmpfs = true;
		/* 用户候选列表显式包含 tmpfs 时同样选择；否则维持 ramfs。 */
		else if (root_fs_names && !!strstr(root_fs_names, "tmpfs"))
			is_tmpfs = true;
	}
}
