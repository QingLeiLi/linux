// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/init_syscalls.h>
#include <linux/initrd.h>
#include <linux/stringify.h>
#include <linux/timekeeping.h>
#include "initramfs_internal.h"

/*
 * initramfs 解包器的启动期 KUnit 契约测试。每个用例在内存中构造 newc/crc
 * 归档，调用真实 unpack_to_rootfs() 修改当前 rootfs，再验证 inode 元数据、内容
 * 或首错结果并清理创建物。解包器使用全局 __initdata 状态，suite_init 必须先与
 * 启动时异步展开同步；测试和其归档缓冲也只在 init 段仍有效时运行。
 */

/* 一个 newc 成员的逻辑字段；fill_cpio() 将其编码成固定宽度 ASCII 头和对齐 body。 */
struct initramfs_test_cpio {
	char *magic;
	unsigned int ino;
	unsigned int mode;
	unsigned int uid;
	unsigned int gid;
	unsigned int nlink;
	unsigned int mtime;
	unsigned int filesize;
	unsigned int devmajor;
	unsigned int devminor;
	unsigned int rdevmajor;
	unsigned int rdevminor;
	unsigned int namesize;
	unsigned int csum;
	char *fname;
	char *data;
};

/* 标准 newc 头由 magic、13 个八位十六进制字段和紧随其后的名字组成。 */
/* regular newc header format */
#define CPIO_HDR_FMT "%s%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%s"
/*
 * Bogus newc header with "0x" prefixes on the uid, gid, and namesize values.
 * parse_header()/simple_str[n]toul() accepted this, contrary to the initramfs
 * specification. hex2bin() now fails.
 */
/*
 * 故意向 uid/gid/namesize 注入 0x/0X 前缀。旧解析器会违反 newc 规范
 * 接受它，当前 hex2bin() 必须拒绝；该格式只服务 initramfs_test_hdr_hex()。
 */
#define CPIO_HDR_OX_INJECT \
	"%s%08x%08x0x%06x0X%06x%08x%08x%08x%08x%08x%08x%08x0x%06x%08x%s"

/*
 * 将 csz 个逻辑成员顺序编码进调用者提供的 out，不分配也不接管缓冲。
 * inject_ox 选择标准头或畸形前缀头；每个名字（含 NUL）和 body 后都补零到四字节
 * 边界，返回总字节数。调用者必须按最坏长度预留空间；本辅助函数不做容量检查。
 */
static size_t fill_cpio(struct initramfs_test_cpio *cs, size_t csz,
			bool inject_ox, char *out)
{
	int i;
	size_t off = 0;

	for (i = 0; i < csz; i++) {
		char *pos = &out[off];
		struct initramfs_test_cpio *c = &cs[i];
		size_t thislen;

		/* sprintf() 的返回值不含 NUL，所以加 1 得到头加名字的实际占用。 */
		/* +1 to account for nulterm */
		thislen = sprintf(pos,
			inject_ox ? CPIO_HDR_OX_INJECT : CPIO_HDR_FMT,
			c->magic, c->ino, c->mode, c->uid, c->gid, c->nlink,
			c->mtime, c->filesize, c->devmajor, c->devminor,
			c->rdevmajor, c->rdevminor, c->namesize, c->csum,
			c->fname) + 1;

		pr_debug("packing (%zu): %.*s\n", thislen, (int)thislen, pos);
		if (thislen != CPIO_HDRLEN + c->namesize)
			pr_debug("padded to: %u\n", CPIO_HDRLEN + c->namesize);
		off += CPIO_HDRLEN + c->namesize;
		while (off & 3)
			out[off++] = '\0';

		memcpy(&out[off], c->data, c->filesize);
		off += c->filesize;
		while (off & 3)
			out[off++] = '\0';
	}

	return off;
}

/*
 * 基线展开用例：构造空普通文件、目录和 TRAILER，验证类型、uid/gid、nlink、
 * blocks 及 mtime 配置分支。关闭 PRESERVE_MTIME 时只要求创建时间落在调用前后；
 * 用例最后 unlink/rmdir，cpio_srcbuf 的所有权始终由测试持有并在 out 路径释放。
 */
static void __init initramfs_test_extract(struct kunit *test)
{
	char *err, *cpio_srcbuf;
	size_t len;
	struct timespec64 ts_before, ts_after;
	struct kstat st = {};
	struct initramfs_test_cpio c[] = { {
		.magic = "070701",
		.ino = 1,
		.mode = S_IFREG | 0777,
		.uid = 12,
		.gid = 34,
		.nlink = 1,
		.mtime = 56,
		.filesize = 0,
		.devmajor = 0,
		.devminor = 1,
		.rdevmajor = 0,
		.rdevminor = 0,
		.namesize = sizeof("initramfs_test_extract"),
		.csum = 0,
		.fname = "initramfs_test_extract",
	}, {
		.magic = "070701",
		.ino = 2,
		.mode = S_IFDIR | 0777,
		.nlink = 1,
		.mtime = 57,
		.devminor = 1,
		.namesize = sizeof("initramfs_test_extract_dir"),
		.fname = "initramfs_test_extract_dir",
	}, {
		.magic = "070701",
		.namesize = sizeof("TRAILER!!!"),
		.fname = "TRAILER!!!",
	} };

	/* 额外 3 字节覆盖任意四字节尾对齐的最坏填充。 */
	/* +3 to cater for any 4-byte end-alignment */
	cpio_srcbuf = kzalloc(ARRAY_SIZE(c) * (CPIO_HDRLEN + PATH_MAX + 3),
			      GFP_KERNEL);
	len = fill_cpio(c, ARRAY_SIZE(c), false, cpio_srcbuf);

	ktime_get_real_ts64(&ts_before);
	err = unpack_to_rootfs(cpio_srcbuf, len);
	ktime_get_real_ts64(&ts_after);
	if (err) {
		KUNIT_FAIL(test, "unpack failed %s", err);
		goto out;
	}

	KUNIT_EXPECT_EQ(test, init_stat(c[0].fname, &st, 0), 0);
	KUNIT_EXPECT_TRUE(test, S_ISREG(st.mode));
	KUNIT_EXPECT_TRUE(test, uid_eq(st.uid, KUIDT_INIT(c[0].uid)));
	KUNIT_EXPECT_TRUE(test, gid_eq(st.gid, KGIDT_INIT(c[0].gid)));
	KUNIT_EXPECT_EQ(test, st.nlink, 1);
	if (IS_ENABLED(CONFIG_INITRAMFS_PRESERVE_MTIME)) {
		KUNIT_EXPECT_EQ(test, st.mtime.tv_sec, c[0].mtime);
	} else {
		KUNIT_EXPECT_GE(test, st.mtime.tv_sec, ts_before.tv_sec);
		KUNIT_EXPECT_LE(test, st.mtime.tv_sec, ts_after.tv_sec);
	}
	KUNIT_EXPECT_EQ(test, st.blocks, c[0].filesize);

	KUNIT_EXPECT_EQ(test, init_stat(c[1].fname, &st, 0), 0);
	KUNIT_EXPECT_TRUE(test, S_ISDIR(st.mode));
	if (IS_ENABLED(CONFIG_INITRAMFS_PRESERVE_MTIME)) {
		KUNIT_EXPECT_EQ(test, st.mtime.tv_sec, c[1].mtime);
	} else {
		KUNIT_EXPECT_GE(test, st.mtime.tv_sec, ts_before.tv_sec);
		KUNIT_EXPECT_LE(test, st.mtime.tv_sec, ts_after.tv_sec);
	}

	KUNIT_EXPECT_EQ(test, init_unlink(c[0].fname), 0);
	KUNIT_EXPECT_EQ(test, init_rmdir(c[1].fname), 0);
out:
	kfree(cpio_srcbuf);
}

/*
 * Don't terminate filename. Previously, the cpio filename field was passed
 * directly to filp_open(collected, O_CREAT|..) without nulterm checks. See
 * https://lore.kernel.org/linux-fsdevel/20241030035509.20194-2-ddiss@suse.de
 */
/*
 * 破坏名字 NUL 和其后填充，并在更远处放置受控 NUL。若解包器越过
 * namesize 搜索终止符，就可能误把毒化后缀作为路径；正确行为是返回格式错误且
 * 不创建对象。该回归用例对应曾把未终止 collected 直接交给 filp_open() 的缺陷。
 */
static void __init initramfs_test_fname_overrun(struct kunit *test)
{
	char *err, *cpio_srcbuf;
	size_t len, suffix_off;
	struct initramfs_test_cpio c[] = { {
		.magic = "070701",
		.ino = 1,
		.mode = S_IFREG | 0777,
		.uid = 0,
		.gid = 0,
		.nlink = 1,
		.mtime = 1,
		.filesize = 0,
		.devmajor = 0,
		.devminor = 1,
		.rdevmajor = 0,
		.rdevminor = 0,
		.namesize = sizeof("initramfs_test_fname_overrun"),
		.csum = 0,
		.fname = "initramfs_test_fname_overrun",
	} };

	/* 用非零字节毒化输入，使任何越过声明名字范围的读取都可观察。 */
	/*
	 * poison cpio source buffer, so we can detect overrun. source
	 * buffer is used by read_into() when hdr or fname
	 * are already available (e.g. no compression).
	 */
	cpio_srcbuf = kmalloc(CPIO_HDRLEN + PATH_MAX + 3, GFP_KERNEL);
	memset(cpio_srcbuf, 'B', CPIO_HDRLEN + PATH_MAX + 3);
	/* 远端 NUL 限制错误路径长度，避免测试本身崩溃或只得到 ENAMETOOLONG。 */
	/* limit overrun to avoid crashes / filp_open() ENAMETOOLONG */
	cpio_srcbuf[CPIO_HDRLEN + strlen(c[0].fname) + 20] = '\0';

	len = fill_cpio(c, ARRAY_SIZE(c), false, cpio_srcbuf);
	/* overwrite trailing fname terminator and padding */
	suffix_off = len - 1;
	while (cpio_srcbuf[suffix_off] == '\0') {
		cpio_srcbuf[suffix_off] = 'P';
		suffix_off--;
	}

	err = unpack_to_rootfs(cpio_srcbuf, len);
	KUNIT_EXPECT_NOT_NULL(test, err);

	kfree(cpio_srcbuf);
}

/*
 * 普通文件 body 用例：展开四字节内容，重新只读打开并读回同一测试缓冲，验证长度
 * 与逐字节内容，再 fput/unlink。它覆盖 CopyFile、xwrite 和文件 offset 的基本路径。
 */
static void __init initramfs_test_data(struct kunit *test)
{
	char *err, *cpio_srcbuf;
	size_t len;
	struct file *file;
	struct initramfs_test_cpio c[] = { {
		.magic = "070701",
		.ino = 1,
		.mode = S_IFREG | 0777,
		.uid = 0,
		.gid = 0,
		.nlink = 1,
		.mtime = 1,
		.filesize = sizeof("ASDF") - 1,
		.devmajor = 0,
		.devminor = 1,
		.rdevmajor = 0,
		.rdevminor = 0,
		.namesize = sizeof("initramfs_test_data"),
		.csum = 0,
		.fname = "initramfs_test_data",
		.data = "ASDF",
	} };

	/* 名字和数据各最多补 3 字节，因此额外预留 6 字节。 */
	/* +6 for max name and data 4-byte padding */
	cpio_srcbuf = kmalloc(CPIO_HDRLEN + c[0].namesize + c[0].filesize + 6,
			      GFP_KERNEL);

	len = fill_cpio(c, ARRAY_SIZE(c), false, cpio_srcbuf);

	err = unpack_to_rootfs(cpio_srcbuf, len);
	KUNIT_EXPECT_NULL(test, err);

	file = filp_open(c[0].fname, O_RDONLY, 0);
	if (IS_ERR(file)) {
		KUNIT_FAIL(test, "open failed");
		goto out;
	}

	/* 把文件内容读回 cpio_srcbuf，验证解包结果与原 body 完全一致。 */
	/* read back file contents into @cpio_srcbuf and confirm match */
	len = kernel_read(file, cpio_srcbuf, c[0].filesize, NULL);
	KUNIT_EXPECT_EQ(test, len, c[0].filesize);
	KUNIT_EXPECT_MEMEQ(test, cpio_srcbuf, c[0].data, len);

	fput(file);
	KUNIT_EXPECT_EQ(test, init_unlink(c[0].fname), 0);
out:
	kfree(cpio_srcbuf);
}

/*
 * crc/newc 混合用例：先证明 070702 的正确字节和与随后的 070701 可独立切换，
 * 再篡改 crc 并要求返回错误。坏校验在 body 写完后才发现，所以首文件仍存在，
 * 状态机停止后第二文件不存在；用例显式清理这项当前可观察但可改进的副作用。
 */
static void __init initramfs_test_csum(struct kunit *test)
{
	char *err, *cpio_srcbuf;
	size_t len;
	struct initramfs_test_cpio c[] = { {
		/* 070702 表示头中的 csum 必须等于文件 body 的无符号字节和。 */
		/* 070702 magic indicates a valid csum is present */
		.magic = "070702",
		.ino = 1,
		.mode = S_IFREG | 0777,
		.nlink = 1,
		.filesize = sizeof("ASDF") - 1,
		.devminor = 1,
		.namesize = sizeof("initramfs_test_csum"),
		.csum = 'A' + 'S' + 'D' + 'F',
		.fname = "initramfs_test_csum",
		.data = "ASDF",
	}, {
		/* 紧随一个无校验 070701 成员，验证校验开关按成员重置。 */
		/* mix csum entry above with no-csum entry below */
		.magic = "070701",
		.ino = 2,
		.mode = S_IFREG | 0777,
		.nlink = 1,
		.filesize = sizeof("ASDF") - 1,
		.devminor = 1,
		.namesize = sizeof("initramfs_test_csum_not_here"),
		/* 070701 必须忽略该故意错误的 csum 字段。 */
		/* csum ignored */
		.csum = 5555,
		.fname = "initramfs_test_csum_not_here",
		.data = "ASDF",
	} };

	cpio_srcbuf = kmalloc(8192, GFP_KERNEL);

	len = fill_cpio(c, ARRAY_SIZE(c), false, cpio_srcbuf);

	err = unpack_to_rootfs(cpio_srcbuf, len);
	KUNIT_EXPECT_NULL(test, err);

	KUNIT_EXPECT_EQ(test, init_unlink(c[0].fname), 0);
	KUNIT_EXPECT_EQ(test, init_unlink(c[1].fname), 0);

	/* 将期望和减一，确认数据落盘后的校验失败能通过返回值传播。 */
	/* mess up the csum and confirm that unpack fails */
	c[0].csum--;
	len = fill_cpio(c, ARRAY_SIZE(c), false, cpio_srcbuf);

	err = unpack_to_rootfs(cpio_srcbuf, len);
	KUNIT_EXPECT_NOT_NULL(test, err);

	/*
	 * file (with content) is still retained in case of bad-csum abort.
	 * Perhaps we should change this.
	 */
	/* 当前坏校验不会回滚已写文件；测试固定此行为，同时保留未来改进空间。 */
	KUNIT_EXPECT_EQ(test, init_unlink(c[0].fname), 0);
	KUNIT_EXPECT_EQ(test, init_unlink(c[1].fname), -ENOENT);
	kfree(cpio_srcbuf);
}

/*
 * hardlink hashtable may leak when the archive omits a trailer:
 * https://lore.kernel.org/r/20241107002044.16477-10-ddiss@suse.de/
 */
/*
 * 构造没有 TRAILER 的两个同键 nlink=2 成员，且把数据放在后一个成员。
 * 验证二者最终 inode 相同、链接计数为 2，并间接覆盖 unpack_to_rootfs() 在缺少
 * 尾标记时仍调用 free_hash() 的收尾路径，避免哈希节点跨调用泄漏。
 */
static void __init initramfs_test_hardlink(struct kunit *test)
{
	char *err, *cpio_srcbuf;
	size_t len;
	struct kstat st0 = {}, st1 = {};
	struct initramfs_test_cpio c[] = { {
		.magic = "070701",
		.ino = 1,
		.mode = S_IFREG | 0777,
		.nlink = 2,
		.devminor = 1,
		.namesize = sizeof("initramfs_test_hardlink"),
		.fname = "initramfs_test_hardlink",
	}, {
		/* 数据故意放在硬链接集合最后一个归档成员。 */
		/* hardlink data is present in last archive entry */
		.magic = "070701",
		.ino = 1,
		.mode = S_IFREG | 0777,
		.nlink = 2,
		.filesize = sizeof("ASDF") - 1,
		.devminor = 1,
		.namesize = sizeof("initramfs_test_hardlink_link"),
		.fname = "initramfs_test_hardlink_link",
		.data = "ASDF",
	} };

	cpio_srcbuf = kmalloc(8192, GFP_KERNEL);

	len = fill_cpio(c, ARRAY_SIZE(c), false, cpio_srcbuf);

	err = unpack_to_rootfs(cpio_srcbuf, len);
	KUNIT_EXPECT_NULL(test, err);

	KUNIT_EXPECT_EQ(test, init_stat(c[0].fname, &st0, 0), 0);
	KUNIT_EXPECT_EQ(test, init_stat(c[1].fname, &st1, 0), 0);
	KUNIT_EXPECT_EQ(test, st0.ino, st1.ino);
	KUNIT_EXPECT_EQ(test, st0.nlink, 2);
	KUNIT_EXPECT_EQ(test, st1.nlink, 2);

	KUNIT_EXPECT_EQ(test, init_unlink(c[0].fname), 0);
	KUNIT_EXPECT_EQ(test, init_unlink(c[1].fname), 0);

	kfree(cpio_srcbuf);
}

/* 压力用例成员数和“固定前缀+十进制编号+NUL”的最大路径缓冲。 */
#define INITRAMFS_TEST_MANY_LIMIT 1000
#define INITRAMFS_TEST_MANY_PATH_MAX (sizeof("initramfs_test_many-") \
			+ sizeof(__stringify(INITRAMFS_TEST_MANY_LIMIT)))

/*
 * 顺序生成并展开 1000 个空普通文件，验证状态机在大量相邻成员、不同 inode 和
 * 对齐边界间不会残留上一项状态。成功后逐一 unlink，也验证所有预期路径都创建。
 */
static void __init initramfs_test_many(struct kunit *test)
{
	char *err, *cpio_srcbuf, *p;
	size_t len = INITRAMFS_TEST_MANY_LIMIT *
		     (CPIO_HDRLEN + INITRAMFS_TEST_MANY_PATH_MAX + 3);
	char thispath[INITRAMFS_TEST_MANY_PATH_MAX];
	int i;

	p = cpio_srcbuf = kmalloc(len, GFP_KERNEL);

	for (i = 0; i < INITRAMFS_TEST_MANY_LIMIT; i++) {
		struct initramfs_test_cpio c = {
			.magic = "070701",
			.ino = i,
			.mode = S_IFREG | 0777,
			.nlink = 1,
			.devminor = 1,
			.fname = thispath,
		};

		c.namesize = 1 + sprintf(thispath, "initramfs_test_many-%d", i);
		p += fill_cpio(&c, 1, false, p);
	}

	len = p - cpio_srcbuf;
	err = unpack_to_rootfs(cpio_srcbuf, len);
	KUNIT_EXPECT_NULL(test, err);

	for (i = 0; i < INITRAMFS_TEST_MANY_LIMIT; i++) {
		sprintf(thispath, "initramfs_test_many-%d", i);
		KUNIT_EXPECT_EQ(test, init_unlink(thispath), 0);
	}

	kfree(cpio_srcbuf);
}

/*
 * An initramfs filename is namesize in length, including the zero-terminator.
 * A filename can be zero-terminated prior to namesize, with the remainder used
 * as padding. This can be useful for e.g. alignment of file data segments with
 * a 4KB filesystem block, allowing for extent sharing (reflinks) between cpio
 * source and destination. This hack works with both GNU cpio and initramfs, as
 * long as PATH_MAX isn't exceeded.
 */
/*
 * namesize 包含 NUL，但允许 NUL 之后到 namesize 末尾都是额外填充；
 * 这里把名字字段扩到使 body 位于 4 KiB 偏移，验证解包器只把首个 NUL 前内容
 * 当路径、仍按完整 namesize 定位数据。该布局可让 cpio 源和目标做块对齐 reflink。
 */
static void __init initramfs_test_fname_pad(struct kunit *test)
{
	char *err;
	size_t len;
	struct file *file;
	char fdata[] = "this file data is aligned at 4K in the archive";
	struct test_fname_pad {
		char padded_fname[4096 - CPIO_HDRLEN];
		char cpio_srcbuf[CPIO_HDRLEN + PATH_MAX + 3 + sizeof(fdata)];
	} *tbufs = kzalloc_obj(struct test_fname_pad);
	struct initramfs_test_cpio c[] = { {
		.magic = "070701",
		.ino = 1,
		.mode = S_IFREG | 0777,
		.uid = 0,
		.gid = 0,
		.nlink = 1,
		.mtime = 1,
		.filesize = sizeof(fdata),
		.devmajor = 0,
		.devminor = 1,
		.rdevmajor = 0,
		.rdevminor = 0,
		/* 通过扩大名字字段，让该成员数据从归档 4 KiB 偏移开始。 */
		/* align file data at 4K archive offset via padded fname */
		.namesize = 4096 - CPIO_HDRLEN,
		.csum = 0,
		.fname = tbufs->padded_fname,
		.data = fdata,
	} };

	memcpy(tbufs->padded_fname, "padded_fname", sizeof("padded_fname"));
	len = fill_cpio(c, ARRAY_SIZE(c), false, tbufs->cpio_srcbuf);

	err = unpack_to_rootfs(tbufs->cpio_srcbuf, len);
	KUNIT_EXPECT_NULL(test, err);

	file = filp_open(c[0].fname, O_RDONLY, 0);
	if (IS_ERR(file)) {
		KUNIT_FAIL(test, "open failed");
		goto out;
	}

	/* 复用归档缓冲读回文件，确认加长名字填充没有偏移 body。 */
	/* read back file contents into @cpio_srcbuf and confirm match */
	len = kernel_read(file, tbufs->cpio_srcbuf, c[0].filesize, NULL);
	KUNIT_EXPECT_EQ(test, len, c[0].filesize);
	KUNIT_EXPECT_MEMEQ(test, tbufs->cpio_srcbuf, c[0].data, len);

	fput(file);
	KUNIT_EXPECT_EQ(test, init_unlink(c[0].fname), 0);
out:
	kfree(tbufs);
}

/*
 * namesize 上界用例：首个目录名字为 PATH_MAX+1 且带 body，必须整项跳过而不报错；
 * 随后的 PATH_MAX 合法目录必须仍能解析和创建。两项连续放置验证 SkipIt 精确越过
 * 超限名字、body 与填充后，状态机仍在正确的下一头边界恢复。
 */
static void __init initramfs_test_fname_path_max(struct kunit *test)
{
	char *err;
	size_t len;
	struct kstat st0 = {}, st1 = {};
	char fdata[] = "this file data will not be unpacked";
	struct test_fname_path_max {
		char fname_oversize[PATH_MAX + 1];
		char fname_ok[PATH_MAX];
		char cpio_src[(CPIO_HDRLEN + PATH_MAX + 3 + sizeof(fdata)) * 2];
	} *tbufs = kzalloc_obj(struct test_fname_path_max);
	struct initramfs_test_cpio c[] = { {
		.magic = "070701",
		.ino = 1,
		.mode = S_IFDIR | 0777,
		.nlink = 1,
		.namesize = sizeof(tbufs->fname_oversize),
		.fname = tbufs->fname_oversize,
		.filesize = sizeof(fdata),
		.data = fdata,
	}, {
		.magic = "070701",
		.ino = 2,
		.mode = S_IFDIR | 0777,
		.nlink = 1,
		.namesize = sizeof(tbufs->fname_ok),
		.fname = tbufs->fname_ok,
	} };

	memset(tbufs->fname_oversize, '/', sizeof(tbufs->fname_oversize) - 1);
	memset(tbufs->fname_ok, '/', sizeof(tbufs->fname_ok) - 1);
	memcpy(tbufs->fname_oversize, "fname_oversize",
	       sizeof("fname_oversize") - 1);
	memcpy(tbufs->fname_ok, "fname_ok", sizeof("fname_ok") - 1);
	len = fill_cpio(c, ARRAY_SIZE(c), false, tbufs->cpio_src);

	/* 超长项是可跳过输入，不应污染 message 或阻止后续合法项。 */
	/* unpack skips over fname_oversize instead of returning an error */
	err = unpack_to_rootfs(tbufs->cpio_src, len);
	KUNIT_EXPECT_NULL(test, err);

	KUNIT_EXPECT_EQ(test, init_stat("fname_oversize", &st0, 0), -ENOENT);
	KUNIT_EXPECT_EQ(test, init_stat("fname_ok", &st1, 0), 0);
	KUNIT_EXPECT_EQ(test, init_rmdir("fname_ok"), 0);

	kfree(tbufs);
}

/*
 * 固定宽十六进制语法回归：用 inject_ox 在三个字段塞入 0x/0X，要求 hex2bin()
 * 拒绝整个头。即使数值看似可解析，也不能接受违反 newc 每字段恰为八个 hex digit
 * 的表示；用例只检查返回错误，因为不应创建任何归档对象。
 */
static void __init initramfs_test_hdr_hex(struct kunit *test)
{
	char *err;
	size_t len;
	char fdata[] = "this file data will not be unpacked";
	struct initramfs_test_bufs {
		char cpio_src[(CPIO_HDRLEN + PATH_MAX + 3 + sizeof(fdata)) * 2];
	} *tbufs = kzalloc(sizeof(struct initramfs_test_bufs), GFP_KERNEL);
	struct initramfs_test_cpio c[] = { {
		.magic = "070701",
		.ino = 1,
		.mode = S_IFREG | 0777,
		.uid = 0x123456,
		.gid = 0x123457,
		.nlink = 1,
		.namesize = sizeof("initramfs_test_hdr_hex_0"),
		.fname = "initramfs_test_hdr_hex_0",
		.filesize = sizeof(fdata),
		.data = fdata,
	}, {
		.magic = "070701",
		.ino = 2,
		.mode = S_IFDIR | 0777,
		.uid = 0x000056,
		.gid = 0x000057,
		.nlink = 1,
		.namesize = sizeof("initramfs_test_hdr_hex_1"),
		.fname = "initramfs_test_hdr_hex_1",
	} };

	/* inject_ox=true 选择带 0x 字段前缀的畸形头模板。 */
	/* inject_ox=true to add "0x" cpio field prefixes */
	len = fill_cpio(c, ARRAY_SIZE(c), true, tbufs->cpio_src);

	err = unpack_to_rootfs(tbufs->cpio_src, len);
	KUNIT_EXPECT_NOT_NULL(test, err);

	kfree(tbufs);
}

/*
 * The kunit_case/_suite struct cannot be marked as __initdata as this will be
 * used in debugfs to retrieve results after test has run.
 */
/*
 * 测试函数可位于 init 段，但 case/suite 描述符在测试结束后仍由 debugfs
 * 查询结果，因此使用 __refdata 而非会被回收的 __initdata；空项终止用例数组。
 */
static struct kunit_case __refdata initramfs_test_cases[] = {
	KUNIT_CASE(initramfs_test_extract),
	KUNIT_CASE(initramfs_test_fname_overrun),
	KUNIT_CASE(initramfs_test_data),
	KUNIT_CASE(initramfs_test_csum),
	KUNIT_CASE(initramfs_test_hardlink),
	KUNIT_CASE(initramfs_test_many),
	KUNIT_CASE(initramfs_test_fname_pad),
	KUNIT_CASE(initramfs_test_fname_path_max),
	KUNIT_CASE(initramfs_test_hdr_hex),
	{},
};

/*
 * suite 级前置同步。unpack_to_rootfs() 的 victim/byte_count/state 等均为模块静态
 * 可写状态，测试线程若与启动期 do_populate_rootfs() 并发会数据竞争甚至崩溃；
 * 等待 initramfs 专属 async cookie 后，后续各 KUnit case 才能串行复用解包器。
 */
static int __init initramfs_test_init(struct kunit_suite *suite)
{
	/*
	 * unpack_to_rootfs() uses module-static state (victim, byte_count,
	 * state, ...). The boot-time async do_populate_rootfs() may still be
	 * running, so wait for it to finish before we call unpack_to_rootfs()
	 * from the test thread, otherwise the two writers race and crash.
	 */
	/* 先等启动解包结束，消除测试线程与异步线程对单实例状态的并发写。 */
	wait_for_initramfs();
	return 0;
}

/* suite 描述符持有初始化钩子和用例表，生命周期延续到 debugfs 结果读取结束。 */
static struct kunit_suite __refdata initramfs_test_suite = {
	.name = "initramfs",
	.suite_init = initramfs_test_init,
	.test_cases = initramfs_test_cases,
};
/* 把 suite 放入 init-section KUnit 注册表，使其在 initramfs 实现仍未回收时运行。 */
kunit_test_init_section_suites(&initramfs_test_suite);

MODULE_DESCRIPTION("Initramfs KUnit test suite");
MODULE_LICENSE("GPL v2");
