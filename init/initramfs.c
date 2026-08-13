// SPDX-License-Identifier: GPL-2.0
#include <linux/async.h>
#include <linux/delay.h>
#include <linux/dirent.h>
#include <linux/export.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/hex.h>
#include <linux/init.h>
#include <linux/init_syscalls.h>
#include <linux/kstrtox.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/namei.h>
#include <linux/overflow.h>
#include <linux/security.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/types.h>
#include <linux/umh.h>
#include <linux/utime.h>

#include <asm/byteorder.h>

#include "do_mounts.h"
#include "initramfs_internal.h"

/*
 * initramfs 解包总览：输入既可能是链接进内核的 cpio，也可能是 boot loader
 * 提供的外部 initrd。unpack_to_rootfs() 负责识别“裸 newc/crc cpio、零填充、
 * 压缩数据”拼接流，并把解压输出送入下方有限状态机；状态机再通过 init_* 与
 * VFS 接口在 rootfs 中创建对象。所有解析状态均为 __initdata，只允许启动阶段
 * 串行使用；错误记录采用“首错优先”，调用者决定 panic、降级成旧式 initrd，
 * 或只打印错误。外部 initrd 最后按 retain/crashkernel 策略保留或释放。
 */

/* crc 格式才累计文件内容字节和；两者在每个 cpio 文件开始时重置。 */
static __initdata bool csum_present;
static __initdata u32 io_csum;

/*
 * 将一个普通文件的 cpio body 完整写入已打开文件。
 *
 * 输入/输出：file 与 pos 由 do_name()/do_copy() 持有，p/count 是当前输入窗口；
 * 返回实际写入量，若首次写失败则返回负 errno，已有进展后失败则返回短写长度。
 * EINTR/EAGAIN 原地重试，0 表示底层无法继续。crc cpio 的校验和只累计成功写入
 * 的字节，因此短写最终会由 do_copy() 记录为 write error。
 */
static ssize_t __init xwrite(struct file *file, const unsigned char *p,
		size_t count, loff_t *pos)
{
	ssize_t out = 0;

	/* 单次内核写入同样不能超过 MAX_RW_COUNT，即约 2 GiB 减 4 KiB。 */
	/* sys_write only can write MAX_RW_COUNT aka 2G-4K bytes at most */
	while (count) {
		ssize_t rv = kernel_write(file, p, count, pos);

		if (rv < 0) {
			if (rv == -EINTR || rv == -EAGAIN)
				continue;
			return out ? out : rv;
		} else if (rv == 0)
			break;

		if (csum_present) {
			ssize_t i;

			for (i = 0; i < rv; i++)
				io_csum += p[i];
		}

		p += rv;
		out += rv;
		count -= rv;
	}

	return out;
}

/* 首个解析/解压错误的静态字符串；后续错误不得覆盖最接近根因的诊断。 */
static __initdata char *message;

/* 记录首个错误。参数指向静态字符串，函数不复制也不取得动态内存所有权。 */
static void __init error(char *x)
{
	if (!message)
		message = x;
}

/* 不可恢复的启动期分配/内置归档错误先输出内存状态，再终止启动以保留诊断。 */
#define panic_show_mem(fmt, ...) \
	({ show_mem(); panic(fmt, ##__VA_ARGS__); })

/* link hash */
/*
 * newc 用 (major, minor, ino, 文件类型) 标识同一硬链接集合。首次
 * 遇到的路径写入 32 桶哈希表，后续成员用 init_link() 指向它。name 的对齐长度
 * 给每个节点预留 PATH_MAX 字符；节点由 find_link() 分配，由 TRAILER!!! 或
 * unpack_to_rootfs() 收尾时的 free_hash() 释放。
 */

/* 固定头长 110（模 4 为 2）；110 + N_ALIGN(name_len) 恰好落在四字节边界。 */
#define N_ALIGN(len) ((((len) + 1) & ~3) + 2)

static __initdata struct hash {
	int ino, minor, major;
	umode_t mode;
	struct hash *next;
	char name[N_ALIGN(PATH_MAX)];
} *head[32];
static __initdata bool hardlink_seen;

/* 将 cpio 设备号和 inode 折叠到 32 个桶；这里只要求快速分桶，不承担唯一性。 */
static inline int hash(int major, int minor, int ino)
{
	unsigned long tmp = ino + minor + (major << 3);
	tmp += tmp >> 5;
	return tmp & 31;
}

/*
 * 查询或登记硬链接集合的首路径。
 *
 * 找到相同键时返回哈希节点内的路径（所有权仍归节点）；首次出现则分配节点、
 * 复制 name 并返回 NULL。分配失败无法安全还原链接拓扑，启动期直接 panic。
 * 文件类型也参与匹配，防止损坏归档用同一 inode 键混淆不同 VFS 对象类型。
 */
static char __init *find_link(int major, int minor, int ino,
			      umode_t mode, char *name)
{
	struct hash **p, *q;
	for (p = head + hash(major, minor, ino); *p; p = &(*p)->next) {
		if ((*p)->ino != ino)
			continue;
		if ((*p)->minor != minor)
			continue;
		if ((*p)->major != major)
			continue;
		if (((*p)->mode ^ mode) & S_IFMT)
			continue;
		return (*p)->name;
	}
	q = kmalloc_obj(struct hash);
	if (!q)
		panic_show_mem("can't allocate link hash entry");
	q->major = major;
	q->minor = minor;
	q->ino = ino;
	q->mode = mode;
	strscpy(q->name, name);
	q->next = NULL;
	*p = q;
	hardlink_seen = true;
	return NULL;
}

/* 清空所有硬链接桶；hardlink_seen 避免无硬链接归档做无意义的桶扫描。 */
static void __init free_hash(void)
{
	struct hash **p, *q;
	for (p = head; hardlink_seen && p < head + 32; p++) {
		while (*p) {
			q = *p;
			*p = q->next;
			kfree(q);
		}
	}
	hardlink_seen = false;
}

#ifdef CONFIG_INITRAMFS_PRESERVE_MTIME
/* 同时恢复路径的 atime/mtime；归档只携带一份 mtime，故两个时间使用同一秒值。 */
static void __init do_utime(char *filename, time64_t mtime)
{
	struct timespec64 t[2] = { { .tv_sec = mtime }, { .tv_sec = mtime } };
	init_utimes(filename, t);
}

/* 已持有 struct path 的文件使用 VFS 接口恢复时间，避免再次按名字解析路径。 */
static void __init do_utime_path(const struct path *path, time64_t mtime)
{
	struct timespec64 t[2] = { { .tv_sec = mtime }, { .tv_sec = mtime } };
	vfs_utimes(path, t);
}

/*
 * 目录时间必须延迟到整个归档展开后恢复：若创建子项后立即恢复父目录 mtime，
 * 后续对子项的创建又会改写父目录时间。节点按头插保存，最终按创建逆序回放，
 * 使常见的“父目录先于子项”归档最后再恢复父目录；name 随节点在回放后释放。
 */
static __initdata LIST_HEAD(dir_list);
struct dir_entry {
	struct list_head list;
	time64_t mtime;
	char name[];
};

/* 保存目录路径和期望 mtime；分配失败意味着无法忠实还原归档，故启动期 panic。 */
static void __init dir_add(const char *name, size_t nlen, time64_t mtime)
{
	struct dir_entry *de;

	de = kmalloc_flex(*de, name, nlen);
	if (!de)
		panic_show_mem("can't allocate dir_entry buffer");
	INIT_LIST_HEAD(&de->list);
	strscpy(de->name, name, nlen);
	de->mtime = mtime;
	list_add(&de->list, &dir_list);
}

/* 在所有成员创建完毕后恢复目录时间，并消费、释放整个 dir_list。 */
static void __init dir_utime(void)
{
	struct dir_entry *de, *tmp;
	list_for_each_entry_safe(de, tmp, &dir_list, list) {
		list_del(&de->list);
		do_utime(de->name, de->mtime);
		kfree(de);
	}
}
#else
/* 关闭 mtime 保留时保留按名字恢复接口；桩不访问 filename，也不产生副作用。 */
static void __init do_utime(char *filename, time64_t mtime) {}
/* 关闭配置时按 path 恢复同样为空操作，不取得 path 引用。 */
static void __init do_utime_path(const struct path *path, time64_t mtime) {}
/* 不需要最终回放目录时间，因此不分配、不保存目录节点。 */
static void __init dir_add(const char *name, size_t nlen, time64_t mtime) {}
/* 对应的收尾桩无需遍历或释放目录时间链表。 */
static void __init dir_utime(void) {}
#endif

/* 当前 cpio 项的公共元数据；由 parse_header() 覆盖，由后续状态消费。 */
static __initdata time64_t mtime;

/* cpio header parsing */
/*
 * 以下字段对应 newc/crc 固定头。解析器是单实例启动期状态机，字段
 * 因而不放在每次调用的栈上；每个合法头都会整体覆盖它们。major/minor/ino/nlink
 * 用于硬链接归并，mode/uid/gid/rdev 用于创建对象，长度驱动边界和对齐计算。
 */

static __initdata unsigned long ino, major, minor, nlink;
static __initdata umode_t mode;
static __initdata unsigned long body_len, name_len;
static __initdata uid_t uid;
static __initdata gid_t gid;
static __initdata unsigned rdev;
static __initdata u32 hdr_csum;

/*
 * 把 110 字节头中 magic 之后的 13 个 8 位十六进制字段解码为宿主序状态。
 * magic 已由 do_header() 验证；hex2bin() 失败记录 damaged header 并返回错误。
 * rdev 由归档中的主次设备号编码成内核 dev_t 表示；mtime 仍受 32 位秒值限制。
 */
static int __init parse_header(char *s)
{
	__be32 header[13];
	int ret;

	ret = hex2bin((u8 *)header, s + 6, sizeof(header));
	if (ret) {
		error("damaged header");
		return ret;
	}

	ino = be32_to_cpu(header[0]);
	mode = be32_to_cpu(header[1]);
	uid = be32_to_cpu(header[2]);
	gid = be32_to_cpu(header[3]);
	nlink = be32_to_cpu(header[4]);
	mtime = be32_to_cpu(header[5]); /* breaks in y2106 */
	body_len = be32_to_cpu(header[6]);
	major = be32_to_cpu(header[7]);
	minor = be32_to_cpu(header[8]);
	rdev = new_encode_dev(MKDEV(be32_to_cpu(header[9]), be32_to_cpu(header[10])));
	name_len = be32_to_cpu(header[11]);
	hdr_csum = be32_to_cpu(header[12]);
	return 0;
}

/* Finite-state machine */
/*
 * 状态机允许输入跨解压回调任意切片。Start 取固定头，Collect 拼接跨块
 * 数据，GotHeader 校验并计算下一个头偏移，GotName 创建非符号链接对象，CopyFile
 * 流式写普通文件，GotSymlink 一次处理“名字+目标”，SkipIt 跳过 body/对齐区，
 * Reset 消费归档间零填充。actions[] 是状态到处理器的唯一分派表。
 */

static __initdata enum state {
	Start,
	Collect,
	GotHeader,
	SkipIt,
	GotName,
	CopyFile,
	GotSymlink,
	Reset
} state, next_state;

/* 当前输入窗口与全局流偏移；eat() 是三者同步前进的唯一入口。 */
static __initdata char *victim;
static unsigned long byte_count __initdata;
static __initdata loff_t this_header, next_header;

/* 消费当前窗口的 n 字节，同时推进指针/流偏移并减少剩余窗口长度。 */
static inline void __init eat(unsigned n)
{
	victim += n;
	this_header += n;
	byte_count -= n;
}

/* 跨窗口收集状态：collected 指完整对象起点，collect 指下一写入处。 */
static __initdata char *collected;
static long remains __initdata;
static __initdata char *collect;

/*
 * 请求连续 size 字节并在完成后转入 next。
 * 当前窗口足够时零复制借用 victim，并立即消费；不足时改用本次解包调用持有的
 * 工作缓冲 buf，进入 Collect，后续 do_collect() 可跨多个输入块拼齐数据。
 */
static void __init read_into(char *buf, unsigned size, enum state next)
{
	if (byte_count >= size) {
		collected = victim;
		eat(size);
		state = next;
	} else {
		collect = collected = buf;
		remains = size;
		next_state = next;
		state = Collect;
	}
}

/* unpack_to_rootfs() 一次分配的三段工作缓冲；仅在该调用期间有效。 */
static __initdata char *header_buf, *symlink_buf, *name_buf;

/* 开始一个 cpio 项：取得固定长度头，完成后交给 GotHeader。 */
static int __init do_start(void)
{
	read_into(header_buf, CPIO_HDRLEN, GotHeader);
	return 0;
}

/*
 * 将当前窗口尽可能复制到 collect；返回 1 请求上层补充输入，返回 0 表示收集
 * 完成并已切换 next_state。remains/collect 保存跨 flush_buffer() 回调的进度。
 */
static int __init do_collect(void)
{
	unsigned long n = remains;
	if (byte_count < n)
		n = byte_count;
	memcpy(collect, victim, n);
	eat(n);
	collect += n;
	if ((remains -= n) != 0)
		return 1;
	state = next_state;
	return 0;
}

/*
 * 验证 newc(070701) 或 crc(070702) magic、解析元数据并计算四字节对齐的下一头
 * 偏移。非法名字长度或过长的符号链接 body 不越界读取，而是保持 SkipIt。链接需
 * 同时收集名字和 target；普通文件或空 body 对象先收集名字，其余未知带 body
 * 类型也直接跳过。返回 1 只表示当前流应停下（通常已有 error），不是 errno。
 */
static int __init do_header(void)
{
	if (!memcmp(collected, "070701", 6)) {
		csum_present = false;
	} else if (!memcmp(collected, "070702", 6)) {
		csum_present = true;
	} else {
		if (memcmp(collected, "070707", 6) == 0)
			error("incorrect cpio method used: use -H newc option");
		else
			error("no cpio magic");
		return 1;
	}
	if (parse_header(collected))
		return 1;
	next_header = this_header + N_ALIGN(name_len) + body_len;
	next_header = (next_header + 3) & ~3;
	state = SkipIt;
	if (name_len <= 0 || name_len > PATH_MAX)
		return 0;
	if (S_ISLNK(mode)) {
		if (body_len > PATH_MAX)
			return 0;
		collect = collected = symlink_buf;
		remains = N_ALIGN(name_len) + body_len;
		next_state = GotSymlink;
		state = Collect;
		return 0;
	}
	if (S_ISREG(mode) || !body_len)
		read_into(name_buf, N_ALIGN(name_len), GotName);
	return 0;
}

/*
 * 丢弃到 next_header 的 body 或对齐填充。窗口未覆盖目标时消费全部并返回 1；
 * 到达目标时精确消费差值、切到 next_state 并返回 0，避免把下一头误吞掉。
 */
static int __init do_skip(void)
{
	if (this_header + byte_count < next_header) {
		eat(byte_count);
		return 1;
	} else {
		eat(next_header - this_header);
		state = next_state;
		return 0;
	}
}

/*
 * 在归档成员之间消费零填充。若下一个非零字节不在四字节边界则记录 broken
 * padding；始终返回 1 把控制交回 flush_buffer()，由它识别下一段裸/压缩数据。
 */
static int __init do_reset(void)
{
	while (byte_count && *victim == '\0')
		eat(1);
	if (byte_count && (this_header & 3))
		error("broken padding");
	return 1;
}

/*
 * 为即将创建的 path 清除类型冲突的旧对象。lstat 语义不跟随末端符号链接；
 * 同类型对象保留给 open/mkdir/mknod 覆盖或复用，不同类型则按“目录/非目录”
 * 选择 rmdir/unlink。删除失败由后续创建自然暴露，本函数不持有 path 引用。
 */
static void __init clean_path(char *path, umode_t fmode)
{
	struct kstat st;

	if (!init_stat(path, &st, AT_SYMLINK_NOFOLLOW) &&
	    (st.mode ^ fmode) & S_IFMT) {
		if (S_ISDIR(st.mode))
			init_rmdir(path);
		else
			init_unlink(path);
	}
}

/*
 * 处理 nlink>=2 的 cpio 项。首次成员只登记并返回 0；后续成员先清理目标路径，
 * 再从首路径创建硬链接，成功返回 1，失败返回 -1。调用者据此决定是否还需创建
 * 或写入实体；硬链接共享 inode，只有首次普通文件成员应写 body。
 */
static int __init maybe_link(void)
{
	if (nlink >= 2) {
		char *old = find_link(major, minor, ino, mode, collected);
		if (old) {
			clean_path(collected, 0);
			return (init_link(old, collected) < 0) ? -1 : 1;
		}
	}
	return 0;
}

/* 当前普通文件及其写偏移，仅在 CopyFile 状态有效，由 do_copy() 最终 fput。 */
static __initdata struct file *wfile;
static __initdata loff_t wfile_pos;

/*
 * 消费已验证且 NUL 结尾的路径名并创建对应 VFS 对象。
 *
 * TRAILER!!! 仅结束当前 cpio 档并释放 hardlink 表；普通文件根据 maybe_link()
 * 决定新建/截断还是只建链接，成功打开后设置 owner/mode/长度并转 CopyFile；
 * 目录先创建后登记延迟 mtime；设备、FIFO、socket 通过 mknod 创建。多数 init_*
 * 返回值沿用启动期“尽力展开”策略，致命格式/写入问题则由 message 汇总。
 */
static int __init do_name(void)
{
	state = SkipIt;
	next_state = Reset;

	/* do_header() 已保证 name_len 位于 1..PATH_MAX。 */
	/* name_len > 0 && name_len <= PATH_MAX checked in do_header */
	if (collected[name_len - 1] != '\0') {
		pr_err("initramfs name without nulterm: %.*s\n",
		       (int)name_len, collected);
		error("malformed archive");
		return 1;
	}

	if (strcmp(collected, "TRAILER!!!") == 0) {
		free_hash();
		return 0;
	}
	clean_path(collected, mode);
	if (S_ISREG(mode)) {
		int ml = maybe_link();
		if (ml >= 0) {
			int openflags = O_WRONLY|O_CREAT|O_LARGEFILE;
			if (ml != 1)
				openflags |= O_TRUNC;
			wfile = filp_open(collected, openflags, mode);
			if (IS_ERR(wfile))
				return 0;
			wfile_pos = 0;
			io_csum = 0;

			vfs_fchown(wfile, uid, gid);
			vfs_fchmod(wfile, mode);
			if (body_len)
				vfs_truncate(&wfile->f_path, body_len);
			state = CopyFile;
		}
	} else if (S_ISDIR(mode)) {
		init_mkdir(collected, mode);
		init_chown(collected, uid, gid, 0);
		init_chmod(collected, mode);
		dir_add(collected, name_len, mtime);
	} else if (S_ISBLK(mode) || S_ISCHR(mode) ||
		   S_ISFIFO(mode) || S_ISSOCK(mode)) {
		if (maybe_link() == 0) {
			init_mknod(collected, mode, rdev);
			init_chown(collected, uid, gid, 0);
			init_chmod(collected, mode);
			do_utime(collected, mtime);
		}
	}
	return 0;
}

/*
 * 把普通文件剩余 body 从当前输入窗口写入 wfile。窗口覆盖尾部时完成写入、恢复
 * mtime、fput 并核对 crc，然后转 SkipIt；否则写完整个窗口，递减 body_len 并
 * 返回 1 等待下一输入块。无论短写还是 checksum 错误都以首错写入 message。
 */
static int __init do_copy(void)
{
	if (byte_count >= body_len) {
		if (xwrite(wfile, victim, body_len, &wfile_pos) != body_len)
			error("write error");

		do_utime_path(&wfile->f_path, mtime);
		fput(wfile);
		if (csum_present && io_csum != hdr_csum)
			error("bad data checksum");
		eat(body_len);
		state = SkipIt;
		return 0;
	} else {
		if (xwrite(wfile, victim, byte_count, &wfile_pos) != byte_count)
			error("write error");
		body_len -= byte_count;
		eat(byte_count);
		return 1;
	}
}

/*
 * 解析同一缓冲中的“对齐路径名 + 非 NUL target”。先验证路径终止符，再在工作
 * 缓冲尾部补 NUL；清理旧路径后创建链接，chown 使用 NOFOLLOW 避免修改目标。
 * 完成后跳过本项剩余对齐并进入 Reset。target 长度已在 do_header() 做上界检查。
 */
static int __init do_symlink(void)
{
	if (collected[name_len - 1] != '\0') {
		pr_err("initramfs symlink without nulterm: %.*s\n",
		       (int)name_len, collected);
		error("malformed archive");
		return 1;
	}
	collected[N_ALIGN(name_len) + body_len] = '\0';
	clean_path(collected, 0);
	init_symlink(collected + N_ALIGN(name_len), collected);
	init_chown(collected, uid, gid, AT_SYMLINK_NOFOLLOW);
	do_utime(collected, mtime);
	state = SkipIt;
	next_state = Reset;
	return 0;
}

/* 状态分派表与 enum state 一一对应；处理器 0=可继续，1=需退出当前输入循环。 */
static __initdata int (*actions[])(void) = {
	[Start]		= do_start,
	[Collect]	= do_collect,
	[GotHeader]	= do_header,
	[SkipIt]	= do_skip,
	[GotName]	= do_name,
	[CopyFile]	= do_copy,
	[GotSymlink]	= do_symlink,
	[Reset]		= do_reset,
};

/*
 * 向 cpio 状态机提交一段连续数据。循环执行 action 直到处理器要求补充数据或
 * 回到 flush_buffer() 判别边界，返回本窗口已消费字节数；状态保留供下一次调用。
 */
static long __init write_buffer(char *buf, unsigned long len)
{
	byte_count = len;
	victim = buf;

	while (!actions[state]())
		;
	return len - byte_count;
}

/*
 * 解压器输出回调：持续把输出交给 write_buffer()。状态机在一个 cpio 尾部停下
 * 时，字符 '0' 表示紧随其后的另一段 newc，NUL 表示填充/Reset；其他字节说明
 * 压缩流内部夹杂垃圾。已有 message 时返回 -1 令解压器尽快停止。
 */
static long __init flush_buffer(void *bufv, unsigned long len)
{
	char *buf = bufv;
	long written;
	long origLen = len;
	if (message)
		return -1;
	while ((written = write_buffer(buf, len)) < len && !message) {
		char c = buf[written];
		if (c == '0') {
			buf += written;
			len -= written;
			state = Start;
		} else if (c == 0) {
			buf += written;
			len -= written;
			state = Reset;
		} else
			error("junk within compressed archive");
	}
	return origLen;
}

/* 解压器回写它从当前压缩输入消费的字节数，用于继续扫描拼接归档。 */
static unsigned long my_inptr __initdata; /* index of next byte to be processed in inbuf */

#include <linux/decompress/generic.h>

/**
 * unpack_to_rootfs - decompress and extract an initramfs archive
 * @buf: input initramfs archive to extract
 * @len: length of initramfs data to process
 *
 * Returns: NULL for success or an error message string
 *
 * This symbol shouldn't be used externally. It's available for unit tests.
 *
 * 契约：buf/len 是可由裸 cpio、零填充和多个压缩流拼接而成的输入区间，
 * 本函数不取得也不释放输入所有权。它为一次调用分配头、名字和符号链接工作
 * 缓冲，初始化全局 __init 状态，按输入魔数在“直接送状态机”和“调用对应解压器”
 * 之间切换；解压器用 flush_buffer() 回送输出并用 my_inptr 告知消费量。
 *
 * 成功返回 NULL；失败返回首个静态错误字符串。无论是否看到可选 TRAILER!!!，
 * 都会恢复延迟目录时间、释放硬链接表和工作缓冲。该实现不可重入，只允许启动
 * 阶段串行调用；导出可见性仅服务单元测试，不构成运行期公共 API。
 */
char * __init unpack_to_rootfs(char *buf, unsigned long len)
{
	long written;
	decompress_fn decompress;
	const char *compress_name;
	struct {
		char header[CPIO_HDRLEN];
		char symlink[PATH_MAX + N_ALIGN(PATH_MAX) + 1];
		char name[N_ALIGN(PATH_MAX)];
	} *bufs = kmalloc_obj(*bufs);

	if (!bufs)
		panic_show_mem("can't allocate buffers");

	header_buf = bufs->header;
	symlink_buf = bufs->symlink;
	name_buf = bufs->name;

	state = Start;
	this_header = 0;
	message = NULL;
	while (!message && len) {
		loff_t saved_offset = this_header;
		if (*buf == '0' && !(this_header & 3)) {
			state = Start;
			written = write_buffer(buf, len);
			buf += written;
			len -= written;
			continue;
		}
		if (!*buf) {
			buf++;
			len--;
			this_header++;
			continue;
		}
		this_header = 0;
		decompress = decompress_method(buf, len, &compress_name);
		pr_debug("Detected %s compressed data\n", compress_name);
		if (decompress) {
			int res = decompress(buf, len, NULL, flush_buffer, NULL,
				   &my_inptr, error);
			if (res)
				error("decompressor failed");
		} else if (compress_name) {
			pr_err("compression method %s not configured\n",
			       compress_name);
			error("decompressor failed");
		} else
			error("invalid magic at start of compressed archive");
		if (state != Reset)
			error("junk at the end of compressed archive");
		this_header = saved_offset + my_inptr;
		buf += my_inptr;
		len -= my_inptr;
	}
	dir_utime();
	/* 即使归档省略可选 TRAILER!!!，也必须回收残留硬链接状态。 */
	/* free any hardlink state collected without optional TRAILER!!! */
	free_hash();
	kfree(bufs);
	return message;
}

/* 外部 initrd 是否在展开后保留；保留时通过 firmware sysfs 只读导出。 */
static int __initdata do_retain_initrd;

/* `retain_initrd` 是无值布尔启动参数；带值视为未消费，避免接受含糊输入。 */
static int __init retain_initrd_param(char *str)
{
	if (*str)
		return 0;
	do_retain_initrd = 1;
	return 1;
}
__setup("retain_initrd", retain_initrd_param);

#ifdef CONFIG_ARCH_HAS_KEEPINITRD
/* 架构兼容参数 `keepinitrd` 与 retain_initrd 汇合到同一个保留决策。 */
static int __init keepinitrd_setup(char *__unused)
{
	do_retain_initrd = 1;
	return 1;
}
__setup("keepinitrd", keepinitrd_setup);
#endif

/* 默认异步展开以并行启动；initramfs_async=0 要求 initcall 内同步等待。 */
static bool __initdata initramfs_async = true;

/* 严格按 kstrtobool 解析；成功才声明已消费该启动参数。 */
static int __init initramfs_async_setup(char *str)
{
	return kstrtobool(str, &initramfs_async) == 0;
}
__setup("initramfs_async=", initramfs_async_setup);

/* 链接脚本提供内置 initramfs 的起点和字节数；内存随 init 段生命周期存在。 */
extern char __initramfs_start[];
extern unsigned long __initramfs_size;
#include <linux/initrd.h>
#include <linux/kexec.h>

/* retain 模式的只读二进制属性；size/private 仅在成功展开后提交。 */
static BIN_ATTR(initrd, 0440, sysfs_bin_attr_simple_read, NULL, 0);

/*
 * 在 memblock 分配开始前预留 boot loader 提供的物理 initrd。
 *
 * 设备树阶段可能暂存过尚不可用的虚拟地址，故先清零 initrd_start/end，再以
 * phys_initrd_* 为唯一输入。预留范围向页边界扩张，既与后续释放粒度一致，也
 * 防止重叠页被其他早期分配占用；范围必须属于内存且尚未 reserved。成功后才
 * 转成虚拟区间并允许低于传统起点，失败则禁用外部 initrd 而不留下半提交状态。
 */
void __init reserve_initrd_mem(void)
{
	phys_addr_t start;
	unsigned long size;

	/* 忽略设备树解析阶段计算的虚拟地址，以物理范围重新校验并转换。 */
	/* Ignore the virtul address computed during device tree parsing */
	initrd_start = initrd_end = 0;

	if (!phys_initrd_size)
		return;
	/* 按 free_initrd_mem() 的页粒度扩张预留区，覆盖所有相交页面。 */
	/*
	 * Round the memory region to page boundaries as per free_initrd_mem()
	 * This allows us to detect whether the pages overlapping the initrd
	 * are in use, but more importantly, reserves the entire set of pages
	 * as we don't want these pages allocated for other purposes.
	 */
	start = round_down(phys_initrd_start, PAGE_SIZE);
	size = phys_initrd_size + (phys_initrd_start - start);
	size = round_up(size, PAGE_SIZE);

	if (!memblock_is_region_memory(start, size)) {
		pr_err("INITRD: 0x%08llx+0x%08lx is not a memory region",
		       (u64)start, size);
		goto disable;
	}

	if (memblock_is_region_reserved(start, size)) {
		pr_err("INITRD: 0x%08llx+0x%08lx overlaps in-use memory region\n",
		       (u64)start, size);
		goto disable;
	}

	memblock_reserve(start, size);
	/* 物理区间成功预留后，才发布可解包的虚拟起止地址。 */
	/* Now convert initrd to virtual addresses */
	initrd_start = (unsigned long)__va(phys_initrd_start);
	initrd_end = initrd_start + phys_initrd_size;
	initrd_below_start_ok = 1;

	return;
disable:
	pr_cont(" - disabling initrd\n");
	initrd_start = 0;
	initrd_end = 0;
}

/*
 * 释放页对齐后的 initrd reserved area，并以 init-memory poison 标记。
 * 弱定义允许架构覆盖特殊映射/回收方式；调用后该虚拟区间不可再解包或导出。
 */
void __weak __init free_initrd_mem(unsigned long start, unsigned long end)
{
	free_reserved_area((void *)start, (void *)end, POISON_FREE_INITMEM,
			"initrd");
}

#ifdef CONFIG_CRASH_RESERVE
/*
 * 处理 initrd 与 crashkernel 保留区重叠：不重叠返回 false，让调用者整体释放；
 * 重叠则先清零整个 initrd，再只释放 crashkernel 两侧，返回 true 表示已完成处置。
 * 清零是为补足 kexec 启动未初始化该共享内存的边界，不能释放 crashkernel 页面。
 */
static bool __init kexec_free_initrd(void)
{
	unsigned long crashk_start = (unsigned long)__va(crashk_res.start);
	unsigned long crashk_end   = (unsigned long)__va(crashk_res.end);

	/* 与 crashkernel 重叠时仅释放两侧，保留共享的崩溃内核页面。 */
	/*
	 * If the initrd region is overlapped with crashkernel reserved region,
	 * free only memory that is not part of crashkernel region.
	 */
	if (initrd_start >= crashk_end || initrd_end <= crashk_start)
		return false;

	/* kexec 未初始化该 initrd 区，因此在部分保留前先整体清零。 */
	/*
	 * Initialize initrd memory region since the kexec boot does not do.
	 */
	memset((void *)initrd_start, 0, initrd_end - initrd_start);
	if (initrd_start < crashk_start)
		free_initrd_mem(initrd_start, crashk_start);
	if (initrd_end > crashk_end)
		free_initrd_mem(crashk_end, initrd_end);
	return true;
}
#else
/* 无 crash reserve 时不存在部分保留需求，由普通释放路径处理整个 initrd。 */
static inline bool kexec_free_initrd(void)
{
	return false;
}
#endif /* CONFIG_KEXEC_CORE */

#ifdef CONFIG_BLK_DEV_RAM
/*
 * 外部镜像无法按 initramfs 解包时，把原始字节保存为 /initrd.image，供稍后的
 * 旧式 RAM-disk 根路径使用。文件由本函数创建并释放；写失败只打印诊断，因为
 * 内置 initramfs 已可作为 rootfs，调用链仍需继续完成安全通知和内存处置。
 */
static void __init populate_initrd_image(char *err)
{
	ssize_t written;
	struct file *file;
	loff_t pos = 0;

	printk(KERN_INFO "rootfs image is not initramfs (%s); looks like an initrd\n",
			err);
	file = filp_open("/initrd.image", O_WRONLY|O_CREAT|O_LARGEFILE, 0700);
	if (IS_ERR(file))
		return;

	written = xwrite(file, (char *)initrd_start, initrd_end - initrd_start,
			&pos);
	if (written != initrd_end - initrd_start)
		pr_err("/initrd.image: incomplete write (%zd != %ld)\n",
		       written, initrd_end - initrd_start);
	fput(file);
}
#endif /* CONFIG_BLK_DEV_RAM */

/*
 * initramfs 异步工作主体，按不可交换顺序构造初始 rootfs。
 *
 * 1. 内置归档属于内核构建产物，失败即 panic；2. 若外部 initrd 存在且未启用
 * INITRAMFS_FORCE，再尝试覆盖/补充 rootfs；3. 外部格式失败时有 BLK_DEV_RAM
 * 则降级保存旧式镜像，否则只报告；4. 通知 LSM rootfs 已构造；5. 按 retain、
 * crashkernel 重叠规则释放或导出原始 initrd；6. 清空全局地址并冲刷延迟 fput。
 *
 * async 框架传入的参数无需使用。该函数拥有展开期间的 initrd 字节访问权，但
 * 不拥有内置链接段；安全钩子之前不得让依赖完整 rootfs 的消费者继续运行。
 */
static void __init do_populate_rootfs(void *unused, async_cookie_t cookie)
{
	/* 始终先展开可信的内置 initramfs，作为 rootfs 的基础内容。 */
	/* Load the built in initramfs */
	char *err = unpack_to_rootfs(__initramfs_start, __initramfs_size);
	if (err)
		/* 内置归档损坏意味着构建产物不可启动，必须立即终止。 */
		panic_show_mem("%s", err); /* Failed to decompress INTERNAL initramfs */

	if (!initrd_start || IS_ENABLED(CONFIG_INITRAMFS_FORCE))
		goto done;

	if (IS_ENABLED(CONFIG_BLK_DEV_RAM))
		printk(KERN_INFO "Trying to unpack rootfs image as initramfs...\n");
	else
		printk(KERN_INFO "Unpacking initramfs...\n");

	err = unpack_to_rootfs((char *)initrd_start, initrd_end - initrd_start);
	if (err) {
#ifdef CONFIG_BLK_DEV_RAM
		populate_initrd_image(err);
#else
		printk(KERN_EMERG "Initramfs unpacking failed: %s\n", err);
#endif
	}

done:
	security_initramfs_populated();

	/* 释放外部 initrd 时避开 crashkernel 仍需持有的重叠页面。 */
	/*
	 * If the initrd region is overlapped with crashkernel reserved region,
	 * free only memory that is not part of crashkernel region.
	 */
	if (!do_retain_initrd && initrd_start && !kexec_free_initrd()) {
		free_initrd_mem(initrd_start, initrd_end);
	} else if (do_retain_initrd && initrd_start) {
		bin_attr_initrd.size = initrd_end - initrd_start;
		bin_attr_initrd.private = (void *)initrd_start;
		if (sysfs_create_bin_file(firmware_kobj, &bin_attr_initrd))
			pr_err("Failed to create initrd sysfs file");
	}
	initrd_start = 0;
	initrd_end = 0;

	init_flush_fput();
}

/* 专属异步域隔离 rootfs 工作；cookie=0 同时作为“尚未调度”的哨兵。 */
static ASYNC_DOMAIN_EXCLUSIVE(initramfs_domain);
static async_cookie_t initramfs_cookie;

/*
 * 等待本域中截至 initramfs_cookie 的解包工作完成。cookie+1 形成包含当前任务的
 * 上界；若 rootfs_initcall 之前误调用，等待一个未调度任务会死锁，故只警告并
 * 返回，让过早访问按旧行为失败。可由需要完整 initramfs 的后续子系统调用。
 */
void wait_for_initramfs(void)
{
	if (!initramfs_cookie) {
		/* rootfs initcall 前尚无 cookie；此时等待会死锁，只告警返回。 */
		/*
		 * Something before rootfs_initcall wants to access
		 * the filesystem/initramfs. Probably a bug. Make a
		 * note, avoid deadlocking the machine, and let the
		 * caller's access fail as it used to.
		 */
		pr_warn_once("wait_for_initramfs() called before rootfs_initcalls\n");
		return;
	}
	async_synchronize_cookie_domain(initramfs_cookie + 1, &initramfs_domain);
}
EXPORT_SYMBOL_GPL(wait_for_initramfs);

/*
 * rootfs_initcall 入口：在专属域调度展开，随后开放 usermode helper。默认让其与
 * 其他启动工作并行；initramfs_async=0 时立即通过同一公共等待接口同步。返回 0
 * 仅表示任务已调度/等待完成，真正不可恢复的内置归档错误在工作函数中 panic。
 */
static int __init populate_rootfs(void)
{
	initramfs_cookie = async_schedule_domain(do_populate_rootfs, NULL,
						 &initramfs_domain);
	usermodehelper_enable();
	if (!initramfs_async)
		wait_for_initramfs();
	return 0;
}
rootfs_initcall(populate_rootfs);
