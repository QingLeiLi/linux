// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/minix_fs.h>
#include <linux/ext2_fs.h>
#include <linux/romfs_fs.h>
#include <uapi/linux/cramfs_fs.h>
#include <linux/initrd.h>
#include <linux/string.h>
#include <linux/string_choices.h>
#include <linux/slab.h>

#include "do_mounts.h"
#include "../fs/squashfs/squashfs_fs.h"

#include <linux/decompress/generic.h>

/*
 * legacy RAM disk 装载地图：`/initrd.image` 是输入，`/dev/ram` 是输出；先读取
 * magic 判断压缩流或可直接复制的文件系统镜像。压缩路径把全局 file/offset
 * 暴露给 decompressor 回调，原始路径按 1 KiB BLOCK_SIZE 复制，所有出口统一
 * fput、释放缓冲区并 unlink 临时 `/dev/ram` 节点。
 */

/* 装载期间持有的输入/输出 file 引用，仅在单线程 __init 路径和解压回调间共享。 */
static struct file *in_file, *out_file;
/* 对应 file 的内核读写偏移，kernel_read/write 通过指针原地推进，单位字节。 */
static loff_t in_pos, out_pos;

int __initdata rd_image_start;		/* starting block # of image */
/* RAM disk 镜像起始块号，单位 1 KiB BLOCK_SIZE；只在启动装载阶段有效。 */

/*
 * ramdisk_start_setup() - 解析弃用的 `ramdisk_start=<块号>` 参数
 *
 * @str 是借用数字文本；kstrtoint(base=0) 成功时更新 rd_image_start。函数在
 * __init 参数解析上下文不持锁、不睡眠，先打印弃用告警；返回 1 表示成功消费，
 * 0 表示解析失败且旧值保持不变。输入 ownership 不转移。
 */
static int __init ramdisk_start_setup(char *str)
{
	pr_warn("ramdisk_start= option is deprecated and will be removed soon\n");
	return kstrtoint(str, 0, &rd_image_start) == 0;
}
__setup("ramdisk_start=", ramdisk_start_setup);

/* crd_load() 的前置声明；完整输入/输出与错误契约见文件末尾定义。 */
static int __init crd_load(decompress_fn deco);

/*
 * This routine tries to find a RAM disk image to load, and returns the
 * number of blocks to read for a non-compressed image, 0 if the image
 * is a compressed image, and -1 if an image with the right magic
 * numbers could not be found.
 *
 * We currently check for the following magic numbers:
 *	minix
 *	ext2
 *	romfs
 *	cramfs
 *	squashfs
 *	gzip
 *	bzip2
 *	lzma
 *	xz
 *	lzo
 *	lz4
 */
/*
 * 在输入中寻找可装载 RAM disk 镜像：非压缩镜像返回需读取的 KiB 数，压缩
 * 镜像返回 0；没有匹配 magic 返回 -1，内存分配失败返回 -ENOMEM。
 * 当前依次识别 minix、ext2、romfs、cramfs、squashfs 及列出的压缩格式。
 */
/*
 * identify_ramdisk_image() - 读取镜像头并选择原始复制长度或解压器
 *
 * @file 是调用者持有引用的输入 file，不转移 ownership；@pos 名义为起始偏移，
 * 当前实现会立即按全局 rd_image_start 重算，因此传入值不影响结果；
 * @decompressor 是不可空输出参数，压缩 magic 命中时写入函数指针，若对应算法
 * 未配置则写 NULL。函数在可睡眠 __init 上下文分配 512 字节缓冲并做 kernel_read。
 * 返回正数时单位实际是 KiB/BLOCK_SIZE 而非通用“块”；0 表示压缩流；负值表示
 * 无有效格式或 -ENOMEM。缓冲区在统一出口释放，file 和输出函数指针归调用者管理。
 */
static int __init
identify_ramdisk_image(struct file *file, loff_t pos,
		decompress_fn *decompressor)
{
	/* 所有格式探测复用 512 字节窗口；各 superblock 指针只是该页的类型化视图。 */
	const int size = 512;
	struct minix_super_block *minixsb;
	struct romfs_super_block *romfsb;
	struct cramfs_super *cramfsb;
	struct squashfs_super_block *squashfsb;
	/* nblocks 默认 -1；正值实际为 KiB，0 专门表示需要流式解压。 */
	int nblocks = -1;
	unsigned char *buf;
	const char *compress_name;
	unsigned long n;
	int start_block = rd_image_start;

	/* 单一缓冲区依次承载 block 0、偏移 0x200 和 block 1 的格式字段。 */
	buf = kmalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	minixsb = (struct minix_super_block *) buf;
	romfsb = (struct romfs_super_block *) buf;
	cramfsb = (struct cramfs_super *) buf;
	squashfsb = (struct squashfs_super_block *) buf;
	memset(buf, 0xe5, size);

	/*
	 * Read block 0 to test for compressed kernel
	 */
	/* 读取镜像起始块，优先识别压缩流以及位于 block 0 的文件系统。 */
	pos = start_block * BLOCK_SIZE;
	/* 当前历史实现不检查短读/错误；0xe5 预填充降低残留字节误碰 magic 的概率。 */
	kernel_read(file, buf, size, &pos);

	/* compress_name 非 NULL 表示 magic 已识别；decompressor NULL 表示算法未编入。 */
	*decompressor = decompress_method(buf, size, &compress_name);
	if (compress_name) {
		printk(KERN_NOTICE "RAMDISK: %s image found at block %d\n",
		       compress_name, start_block);
		if (!*decompressor)
			printk(KERN_EMERG
			       "RAMDISK: %s decompressor not configured!\n",
			       compress_name);
		nblocks = 0;
		goto done;
	}

	/* romfs is at block zero too */
	/* romfs 同样从 block 0 开始；网络序 size 向上取整为 1 KiB 单位。 */
	if (romfsb->word0 == ROMSB_WORD0 &&
	    romfsb->word1 == ROMSB_WORD1) {
		printk(KERN_NOTICE
		       "RAMDISK: romfs filesystem found at block %d\n",
		       start_block);
		nblocks = (ntohl(romfsb->size)+BLOCK_SIZE-1)>>BLOCK_SIZE_BITS;
		goto done;
	}

	/* 未填充的 cramfs 也从 block 0 开始，size 同样向上取整。 */
	if (cramfsb->magic == CRAMFS_MAGIC) {
		printk(KERN_NOTICE
		       "RAMDISK: cramfs filesystem found at block %d\n",
		       start_block);
		nblocks = (cramfsb->size + BLOCK_SIZE - 1) >> BLOCK_SIZE_BITS;
		goto done;
	}

	/* squashfs is at block zero too */
	/* squashfs 也位于 block 0；其字段为小端且 bytes_used 为 64 位。 */
	if (le32_to_cpu(squashfsb->s_magic) == SQUASHFS_MAGIC) {
		printk(KERN_NOTICE
		       "RAMDISK: squashfs filesystem found at block %d\n",
		       start_block);
		nblocks = (le64_to_cpu(squashfsb->bytes_used) + BLOCK_SIZE - 1)
			 >> BLOCK_SIZE_BITS;
		goto done;
	}

	/*
	 * Read 512 bytes further to check if cramfs is padded
	 */
	/* 再前进 512 字节识别带传统 padding 的 cramfs 镜像。 */
	pos = start_block * BLOCK_SIZE + 0x200;
	kernel_read(file, buf, size, &pos);

	if (cramfsb->magic == CRAMFS_MAGIC) {
		printk(KERN_NOTICE
		       "RAMDISK: cramfs filesystem found at block %d\n",
		       start_block);
		nblocks = (cramfsb->size + BLOCK_SIZE - 1) >> BLOCK_SIZE_BITS;
		goto done;
	}

	/*
	 * Read block 1 to test for minix and ext2 superblock
	 */
	/* minix/ext2 superblock 位于第二个 1 KiB 块，因此最后读取 block 1。 */
	pos = (start_block + 1) * BLOCK_SIZE;
	kernel_read(file, buf, size, &pos);

	/* Try minix */
	/* 尝试 Minix 两种 magic；zone 数按 log_zone_size 换算成 KiB。 */
	if (minixsb->s_magic == MINIX_SUPER_MAGIC ||
	    minixsb->s_magic == MINIX_SUPER_MAGIC2) {
		printk(KERN_NOTICE
		       "RAMDISK: Minix filesystem found at block %d\n",
		       start_block);
		nblocks = minixsb->s_nzones << minixsb->s_log_zone_size;
		goto done;
	}

	/* Try ext2 */
	/* 尝试 ext2；helper 返回按 RAM disk 复制协议需要的 KiB 数，0 表示不匹配。 */
	n = ext2_image_size(buf);
	if (n) {
		printk(KERN_NOTICE
		       "RAMDISK: ext2 filesystem found at block %d\n",
		       start_block);
		nblocks = n;
		goto done;
	}

	printk(KERN_NOTICE
	       "RAMDISK: Couldn't find valid RAM disk image starting at %d.\n",
	       start_block);

done:
	/* 所有识别路径都只借用 file，统一释放本地探测缓冲区。 */
	kfree(buf);
	return nblocks;
}

/*
 * nr_blocks() - 返回块设备 file 容量的 KiB 数
 *
 * @file 是借用且必须有效的 file；函数借用其 mapping host inode，不增引用、
 * 不睡眠。非块设备返回 0；块设备以 i_size_read() 取得字节容量并右移 10 转为
 * KiB。返回值只用于检查 ramdisk 输出容量，不改变 file 或 inode 状态。
 */
static unsigned long nr_blocks(struct file *file)
{
	/* inode 生命周期由调用者持有的 file 引用稳定。 */
	struct inode *inode = file->f_mapping->host;

	if (!S_ISBLK(inode->i_mode))
		return 0;
	return i_size_read(inode) >> 10;
}

/*
 * rd_load_image() - 把 `/initrd.image` 复制或解压到 `/dev/ram`
 *
 * 由 initrd_load() 调用；无入参，运行于可睡眠的进程 1 __init 上下文，入口
 * 不持锁。函数取得 out_file/in_file 引用并设置全局 in_pos/out_pos；格式探测
 * 返回 0 时调用 crd_load() 流式解压，返回正 KiB 数时检查 ramdisk 容量并按
 * BLOCK_SIZE 复制。返回 1 表示装载路径认为成功，0 表示打开、识别、容量、
 * 分配或解压失败。所有标签按取得顺序逆向 fput，释放本地 buf，并无条件删除
 * `/dev/ram` 临时节点；全局 file 指针只在本调用及同步解压回调期间有效。
 */
int __init rd_load_image(void)
{
	/* res 是对调用者的布尔式结果；容量变量单位 KiB，nr_disks 为当前分片数。 */
	int res = 0;
	unsigned long rd_blocks, devblocks, nr_disks;
	int nblocks, i;
	/* buf 是原始复制的一块临时缓冲；rotate/rotator 只驱动启动进度显示。 */
	char *buf = NULL;
	unsigned short rotate = 0;
	decompress_fn decompressor = NULL;
	char rotator[4] = { '|' , '/' , '-' , '\\' };

	/* 阶段 1：先打开输出 ramdisk；失败时尚未取得任何 file 引用。 */
	out_file = filp_open("/dev/ram", O_RDWR, 0);
	if (IS_ERR(out_file))
		goto out;

	/* 再打开输入镜像；失败从 noclose_input 只释放已经取得的 out_file。 */
	in_file = filp_open("/initrd.image", O_RDONLY, 0);
	if (IS_ERR(in_file))
		goto noclose_input;

	/* 阶段 2：识别 magic。in_pos/out_pos 也供后续 decompressor 回调原地推进。 */
	in_pos = rd_image_start * BLOCK_SIZE;
	nblocks = identify_ramdisk_image(in_file, in_pos, &decompressor);
	if (nblocks < 0)
		goto done;

	/* 0 是压缩流哨兵，不代表空镜像；解压成功直接进入共同成功出口。 */
	if (nblocks == 0) {
		if (crd_load(decompressor) == 0)
			goto successful_load;
		goto done;
	}

	/*
	 * NOTE NOTE: nblocks is not actually blocks but
	 * the number of kibibytes of data to load into a ramdisk.
	 */
	/* nblocks 名称有误：它实际是要装入 ramdisk 的 KiB 数，而非任意块计数。 */
	/* 阶段 3：原始镜像必须完整容纳于输出块设备，超限不可部分装载。 */
	rd_blocks = nr_blocks(out_file);
	if (nblocks > rd_blocks) {
		printk("RAMDISK: image too big! (%dKiB/%ldKiB)\n",
		       nblocks, rd_blocks);
		goto done;
	}

	/*
	 * OK, time to copy in the data
	 */
	/* 容量检查通过，开始复制数据。 */
	/*
	 * 当前 devblocks 直接等于 nblocks，因此 nr_disks 计算结果恒为 1，循环中的
	 * 多盘切换条件在 `i < nblocks` 范围内不会命中；这是保留的历史分片骨架。
	 */
	devblocks = nblocks;

	if (devblocks == 0) {
		printk(KERN_ERR "RAMDISK: could not determine device size\n");
		goto done;
	}

	/* 每次搬运 1 KiB；分配失败时 file 引用仍由 done 标签释放。 */
	buf = kmalloc(BLOCK_SIZE, GFP_KERNEL);
	if (!buf) {
		printk(KERN_ERR "RAMDISK: could not allocate buffer\n");
		goto done;
	}

	nr_disks = (nblocks - 1) / devblocks + 1;
	pr_notice("RAMDISK: Loading %dKiB [%ld disk%s] into ram disk... ",
		  nblocks, nr_disks, str_plural(nr_disks));
	/* raw copy 路径按声明长度循环；当前代码不逐次校验短读/短写返回值。 */
	for (i = 0; i < nblocks; i++) {
		if (i && (i % devblocks == 0)) {
			pr_cont("done disk #1.\n");
			rotate = 0;
			fput(in_file);
			break;
		}
		kernel_read(in_file, buf, BLOCK_SIZE, &in_pos);
		kernel_write(out_file, buf, BLOCK_SIZE, &out_pos);
		if (!IS_ENABLED(CONFIG_S390) && !(i % 16)) {
			pr_cont("%c\b", rotator[rotate & 0x3]);
			rotate++;
		}
	}
	pr_cont("done.\n");

successful_load:
	/* 只有解压返回 0 或原始复制循环结束会发布成功结果。 */
	res = 1;
done:
	/* 到达时 in_file 与 out_file 均已成功打开，先释放输入。 */
	fput(in_file);
noclose_input:
	/* 输入打开失败时也会到此；out_file 一定是有效引用。 */
	fput(out_file);
out:
	/* buf 可为 NULL；设备节点无论成功失败都只是启动期临时入口。 */
	kfree(buf);
	init_unlink("/dev/ram");
	return res;
}

/* decompressor error callback 设置的历史退出状态；当前本文件不再读取该值。 */
static int exit_code;
/* 输入/输出或算法错误哨兵；一旦置 1，crd_load() 强制报告失败。 */
static int decompress_error;

/*
 * compr_fill() - 为通用解压器从 initrd 输入 file 填充压缩字节
 *
 * @buf 是解压器持有的可写输出缓冲，至少 @len 字节；@len 是期望读取字节数。
 * 函数借用全局 in_file 并通过 in_pos 推进偏移，可睡眠且只允许同步 crd_load()
 * 调用。返回 kernel_read() 的正字节数、0 EOF 或负 errno；错误/过早 EOF 会
 * 打印诊断，但本函数不设置 decompress_error，由解压器根据返回值触发 error。
 */
static long __init compr_fill(void *buf, unsigned long len)
{
	/* r 同时承载实际字节数、EOF 和负 errno。 */
	long r = kernel_read(in_file, buf, len, &in_pos);
	if (r < 0)
		printk(KERN_ERR "RAMDISK: error while reading compressed data");
	else if (r == 0)
		printk(KERN_ERR "RAMDISK: EOF while reading compressed data");
	return r;
}

/*
 * compr_flush() - 把解压窗口写入 ramdisk 输出 file
 *
 * @window 是解压器持有的只读数据窗口；@outcnt 是必须提交的字节数。函数借用
 * 全局 out_file 并推进 out_pos，可睡眠。完整写入返回 outcnt；短写或负错误
 * 只在首次打印，设置 decompress_error 并返回 -1，让解压器终止。无 ownership
 * 转移，窗口生命周期仍由解压器管理。
 */
static long __init compr_flush(void *window, unsigned long outcnt)
{
	/* written 是 kernel_write 的实际字节数或负 errno，必须精确等于请求长度。 */
	long written = kernel_write(out_file, window, outcnt, &out_pos);
	if (written != outcnt) {
		if (decompress_error == 0)
			printk(KERN_ERR
			       "RAMDISK: incomplete write (%ld != %ld)\n",
			       written, outcnt);
		decompress_error = 1;
		return -1;
	}
	return outcnt;
}

/*
 * error() - 接收通用解压器的致命错误文本并锁存失败状态
 *
 * @x 是解压器借用的 NUL 字符串，不可为 NULL；函数同步打印它，把 exit_code
 * 和 decompress_error 都置 1。无直接返回值、不取得文本 ownership；状态由
 * crd_load() 在解压调用返回后读取。
 */
static void __init error(char *x)
{
	printk(KERN_ERR "%s\n", x);
	exit_code = 1;
	decompress_error = 1;
}

/*
 * crd_load() - 以回调适配器运行选定解压器并写入 ramdisk
 *
 * @deco 是 identify_ramdisk_image() 选出的解压函数指针，可为 NULL；NULL
 * 表示识别了压缩格式但内核未配置对应算法，此时打印紧急信息并 panic，因为
 * 无法恢复目标 initrd。函数在可睡眠 __init 上下文同步调用 decompressor，
 * 输入由 compr_fill() 从 in_file 提供，输出由 compr_flush() 写 out_file，错误
 * 交给 error()。返回 0 表示解压成功；非零是算法结果，任何锁存的
 * decompress_error 都强制返回 1。file 引用仍归 rd_load_image()，本函数不释放。
 * 装载路径启动期只调用一次，因此静态错误哨兵不在入口重置；若未来允许重试，
 * 必须在每次调用前同时清零 exit_code/decompress_error 和 file offset。
 */
static int __init crd_load(decompress_fn deco)
{
	/* result 承载具体解压器返回；外部只按 0/非0判断。 */
	int result;

	/* magic 已识别却没有算法不是“换下一个格式”，而是构建配置无法启动。 */
	if (!deco) {
		pr_emerg("Invalid ramdisk decompression routine.  "
			 "Select appropriate config option.\n");
		panic("Could not decompress initial ramdisk image.");
	}

	/* 解压同步完成后才返回，因此全局 file/offset 在所有 callback 期间保持有效。 */
	result = deco(NULL, 0, compr_fill, compr_flush, NULL, NULL, error);
	if (decompress_error)
		result = 1;
	return result;
}
