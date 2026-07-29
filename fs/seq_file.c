// SPDX-License-Identifier: GPL-2.0
/*
 * seq_file 学习导读
 *
 * 中文学习注释模型：OpenAI GPT-5 Codex（2026-07-28）。
 *
 * seq_file 把“可能动态变化、无法一次生成全部内容的记录序列”适配成普通
 * 可读、可 seek 的文件。调用者提供 start/next/show/stop 迭代器；本文件
 * 负责缓冲、用户复制、逻辑字节位置恢复、溢出扩容和通用 list/hlist
 * 遍历 helper，不负责保护调用者的数据结构。
 *
 * 主链路：
 *   open -> seq_open() 建立每 file 状态
 *   read/read_iter -> start -> show -> next ... -> stop -> copy_to_iter
 *   pread/lseek 或位置不连续 -> traverse() 从头重放到目标字节
 *   release -> 释放缓冲和 seq_file
 *
 * m->lock 串行化同一 file 的 index、read_pos、buf/count/from；调用者若
 * 遍历共享对象，仍须在 iterator 回调中使用自己的 mutex/RCU/引用协议。
 * show 溢出时扩容并从当前 index 重启，因而回调必须遵守 seq_operations
 * 契约。收益是统一正确的分段输出；代价是随机 seek 可能 O(n) 重放，
 * 单条超大记录还会多次生成和倍增缓冲。
 */
/*
 * linux/fs/seq_file.c
 *
 * helper functions for making synthetic files from sequences of records.
 * initial implementation -- AV, Oct 2001.
 */
/*
 * 本文件提供把一串记录制作成合成文件的通用 helper；
 * 最初实现由 AV 于 2001 年 10 月完成。原作者说明保留在上方。
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cache.h>
#include <linux/fs.h>
#include <linux/export.h>
#include <linux/hex.h>
#include <linux/seq_file.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/mm.h>
#include <linux/printk.h>
#include <linux/string_helpers.h>
#include <linux/uio.h>

#include <linux/uaccess.h>
#include <asm/page.h>

/* seq_file_cache：启动后只读的 struct seq_file 专用 slab，open/release 配对使用。 */
static struct kmem_cache *seq_file_cache __ro_after_init;

/*
 * seq_set_overflow() - 把当前缓冲标记为已溢出。
 * @m：调用者独占或持有 m->lock 的 seq_file；令 count==size，供
 * seq_has_overflowed() 统一识别。无返回、不分配。
 */
static void seq_set_overflow(struct seq_file *m)
{
	m->count = m->size;
}

/*
 * seq_buf_alloc() - 分配可由 kmalloc/vmalloc 支撑的记账缓冲。
 * @size：字节数；超过单次 VFS I/O 上限直接返回 NULL。成功返回需由
 * kvfree() 释放的缓冲，失败返回 NULL；GFP_KERNEL_ACCOUNT 可睡眠。
 */
static void *seq_buf_alloc(unsigned long size)
{
	if (unlikely(size > MAX_RW_COUNT))
		return NULL;

	return kvmalloc(size, GFP_KERNEL_ACCOUNT);
}

/**
 *	seq_open -	initialize sequential file
 *	@file: file we initialize
 *	@op: method table describing the sequence
 *
 *	seq_open() sets @file, associating it with a sequence described
 *	by @op.  @op->start() sets the iterator up and returns the first
 *	element of sequence. @op->stop() shuts it down.  @op->next()
 *	returns the next element of sequence.  @op->show() prints element
 *	into the buffer.  In case of error ->start() and ->next() return
 *	ERR_PTR(error).  In the end of sequence they return %NULL. ->show()
 *	returns 0 in case of success and negative number in case of error.
 *	Returning SEQ_SKIP means "discard this element and move on".
 *	Note: seq_open() will allocate a struct seq_file and store its
 *	pointer in @file->private_data. This pointer should not be modified.
 */
/*
 * seq_open() 初始化顺序文件，把 @op 描述的序列与 @file 绑定。
 * start 建立迭代器并返回首元素，next 推进，show 写入缓冲，stop 无论正常、
 * EOF 或错误都负责收尾。start/next 用 ERR_PTR 返回错误、NULL 表示 EOF；
 * show 返回 0 成功、负数硬错误、SEQ_SKIP 丢弃当前记录继续。
 *
 * @file：open 路径持有的新 file，private_data 必须为空；成功后其生命周期
 * 拥有 seq_file。@op：调用者保证至少覆盖 file 生命周期的只读操作表。
 * 返回 0/-ENOMEM；成功初始化 mutex/op/file 并清除历史 FMODE_PWRITE。
 */
int seq_open(struct file *file, const struct seq_operations *op)
{
	struct seq_file *p;

	WARN_ON(file->private_data);

	p = kmem_cache_zalloc(seq_file_cache, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	/* 分配完成后才发布到 private_data，失败时 file 保持未绑定状态。 */
	file->private_data = p;

	mutex_init(&p->lock);
	p->op = op;

	// No refcounting: the lifetime of 'p' is constrained
	// to the lifetime of the file.
	/*
	 * p 不单独计数，它的存活范围严格受 file 引用约束；
	 * release 回调是唯一销毁点，因此 p->file 是借用反向指针。
	 */
	p->file = file;

	/*
	 * seq_files support lseek() and pread().  They do not implement
	 * write() at all, but we clear FMODE_PWRITE here for historical
	 * reasons.
	 *
	 * If a client of seq_files a) implements file.write() and b) wishes to
	 * support pwrite() then that client will need to implement its own
	 * file.open() which calls seq_open() and then sets FMODE_PWRITE.
	 */
	/*
	 * seq_file 支持 lseek/pread，默认不实现 write；出于历史
	 * 兼容仍清除 FMODE_PWRITE。确实提供 write+pwrite 的客户需自定义
	 * open，在 seq_open() 成功后重新设置该模式位。
	 */
	file->f_mode &= ~FMODE_PWRITE;
	return 0;
}
EXPORT_SYMBOL(seq_open);

/*
 * traverse() - 从序列开头重放输出，恢复到目标逻辑字节偏移。
 *
 * @m：已持有 m->lock 的 seq_file；函数重置 index/count/from。
 * @offset：非负目标字节位置。成功返回 0，并令缓冲保存跨过 offset 的
 * 剩余数据；回调错误原样返回。缓冲溢出则 stop、倍增重分配并返回
 * -EAGAIN，调用者必须从头重试；-ENOMEM 表示扩容失败。
 */
static int traverse(struct seq_file *m, loff_t offset)
{
	loff_t pos = 0;
	int error = 0;
	void *p;

	/* 阶段 1：清空上次读取窗口；offset==0 无需触碰 iterator。 */
	m->index = 0;
	m->count = m->from = 0;
	if (!offset)
		return 0;

	if (!m->buf) {
		m->buf = seq_buf_alloc(m->size = PAGE_SIZE);
		if (!m->buf)
			return -ENOMEM;
	}
	/* 阶段 2：逐记录生成，累计的是输出字节而不是记录个数。 */
	p = m->op->start(m, &m->index);
	while (p) {
		error = PTR_ERR(p);
		if (IS_ERR(p))
			break;
		error = m->op->show(m, p);
		if (error < 0)
			break;
		if (unlikely(error)) {
			/* SEQ_SKIP/正返回丢弃本记录已写内容，但不终止遍历。 */
			error = 0;
			m->count = 0;
		}
		if (seq_has_overflowed(m))
			goto Eoverflow;
		p = m->op->next(m, p, &m->index);
		if (pos + m->count > offset) {
			/*
			 * 目标落在当前记录内部：from 指向首个待读字节，count 缩为
			 * 其后长度；index 保持当前迭代协议位置供后续 read 继续。
			 */
			m->from = offset - pos;
			m->count -= m->from;
			break;
		}
		pos += m->count;
		m->count = 0;
		if (pos == offset)
			break;
	}
	/* 正常、错误与 EOF 都必须 stop，配对 start 可能取得的锁和引用。 */
	m->op->stop(m, p);
	return error;

Eoverflow:
	/*
	 * 当前记录装不下时必须先 stop，释放 iterator 可能持有的锁/引用；
	 * 丢弃旧缓冲并倍增。返回 -EAGAIN 让调用者从头完整重放。
	 */
	m->op->stop(m, p);
	kvfree(m->buf);
	m->count = 0;
	m->buf = seq_buf_alloc(m->size <<= 1);
	return !m->buf ? -ENOMEM : -EAGAIN;
}

/**
 *	seq_read -	->read() method for sequential files.
 *	@file: the file to read from
 *	@buf: the buffer to read to
 *	@size: the maximum number of bytes to read
 *	@ppos: the current position in the file
 *
 *	Ready-made ->f_op->read()
 */
/*
 * 这是可直接安装为 file_operations.read 的同步包装。
 * @file 为已 seq_open 的 file；@buf/@size 是用户目标及字节容量；
 * @ppos 输入输出逻辑字节位置。函数把单缓冲 read 适配为 ITER_DEST，
 * 委托 seq_read_iter()，并把 ki_pos 写回。返回复制字节数、EOF 0 或 errno；
 * 可能访问用户内存、调用迭代器并睡眠。
 */
ssize_t seq_read(struct file *file, char __user *buf, size_t size, loff_t *ppos)
{
	struct iovec iov = { .iov_base = buf, .iov_len = size};
	struct kiocb kiocb;
	struct iov_iter iter;
	ssize_t ret;

	/* 阶段 1：把传统 user buffer 包装成单段 ITER_DEST 与同步 kiocb。 */
	init_sync_kiocb(&kiocb, file);
	iov_iter_init(&iter, ITER_DEST, &iov, 1, size);

	/* 阶段 2：核心 iterator 更新 ki_pos；旧 read ABI 要显式回写 *ppos。 */
	kiocb.ki_pos = *ppos;
	ret = seq_read_iter(&kiocb, &iter);
	*ppos = kiocb.ki_pos;
	return ret;
}
EXPORT_SYMBOL(seq_read);

/*
 * Ready-made ->f_op->read_iter()
 */
/*
 * 可直接安装为 file_operations.read_iter 的核心读取状态机。
 *
 * @iocb：同步/异步 I/O 控制块，本实现使用 ki_filp 与输入输出 ki_pos。
 * @iter：目标 iov_iter，函数最多填充其剩余容量。
 *
 * m->lock 串行化同一 file 的 iterator、缓冲和字节位置。先恢复非连续
 * ki_pos，再交付上次残留；随后 start/show/next 生成至少一条非空记录。
 * 单条记录溢出时 stop、倍增缓冲并从同一 index 重新 start；得到首条后
 * 尽量批量追加更多完整记录，最后 stop 并 copy_to_iter。
 *
 * 返回已复制字节数（即使随后遇到错误也优先返回部分成功）、EOF 0，
 * 或无任何进展时的回调/-ENOMEM/-EFAULT。所有出口释放 mutex；stop 与
 * 每次成功 start 配对。调用者的 show 必须允许因扩容/seek 被重复执行。
 */
ssize_t seq_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct seq_file *m = iocb->ki_filp->private_data;
	size_t copied = 0;
	size_t n;
	void *p;
	int err = 0;

	/*
	 * 变量地图：
	 *   m       file 私有的持久状态；
	 *   copied  本次已经成功交给用户的字节，决定部分成功语义；
	 *   n       单次 copy_to_iter 实际进展；
	 *   p       iterator 回调返回的借用位置或 ERR_PTR；
	 *   err     尚未被部分成功覆盖的终止原因。
	 */
	if (!iov_iter_count(iter))
		return 0;

	mutex_lock(&m->lock);

	/*
	 * if request is to read from zero offset, reset iterator to first
	 * record as it might have been already advanced by previous requests
	 */
	/*
	 * 从字节 0 读取必须清空之前推进的记录索引和缓存长度，
	 * 否则 rewind 后会从旧 iterator 状态继续，破坏文件位置语义。
	 */
	if (iocb->ki_pos == 0) {
		m->index = 0;
		m->count = 0;
	}

	/* Don't assume ki_pos is where we left it */
	/*
	 * pread、lseek 或共享 file position 的其他调用可能让 ki_pos
	 * 与 read_pos 分离。traverse 从头重放；-EAGAIN 只表示缓冲已扩容，
	 * 必须继续重试。真实错误则把持久状态强制复位到文件开头。
	 */
	if (unlikely(iocb->ki_pos != m->read_pos)) {
		while ((err = traverse(m, iocb->ki_pos)) == -EAGAIN)
			;
		if (err) {
			/* With prejudice... */
			/* 位置恢复失败后彻底丢弃旧游标，避免半恢复状态被复用。 */
			m->read_pos = 0;
			m->index = 0;
			m->count = 0;
			goto Done;
		} else {
			m->read_pos = iocb->ki_pos;
		}
	}

	/* grab buffer if we didn't have one */
	/* 首次真正读取时懒分配一页，未打开即不承担缓冲内存。 */
	if (!m->buf) {
		m->buf = seq_buf_alloc(m->size = PAGE_SIZE);
		if (!m->buf)
			goto Enomem;
	}
	// something left in the buffer - copy it out first
	/*
	 * 上次用户缓冲太小时，m->from/count 描述尚未交付的尾部；
	 * 必须先交付它，才能调用 next 生成后续记录，保持字节流顺序。
	 */
	if (m->count) {
		n = copy_to_iter(m->buf + m->from, m->count, iter);
		m->count -= n;
		m->from += n;
		copied += n;
		if (m->count)	// hadn't managed to copy everything
			/* 用户目标已满或 fault 导致未复制完，保留尾部下次重试。 */
			goto Done;
	}
	// get a non-empty record in the buffer
	/* 残留交付完毕，从当前记录索引生成下一条非空、可容纳记录。 */
	m->from = 0;
	p = m->op->start(m, &m->index);
	while (1) {
		err = PTR_ERR(p);
		if (!p || IS_ERR(p))	// EOF or an error
			/* NULL 是正常 EOF；错误指针由 PTR_ERR 转为 errno。 */
			break;
		err = m->op->show(m, p);
		if (err < 0)		// hard error
			/* 负 show 返回是硬错误，必须 stop 后结束。 */
			break;
		if (unlikely(err))	// ->show() says "skip it"
			/* 正返回要求丢弃本条已写内容并推进，而非向用户报错。 */
			m->count = 0;
		if (unlikely(!m->count)) { // empty record
			/* 空记录没有可交付字节，继续 next 避免返回伪 EOF。 */
			p = m->op->next(m, p, &m->index);
			continue;
		}
		if (!seq_has_overflowed(m)) // got it
			/* 完整首条记录已在缓冲，转入批量填充阶段。 */
			goto Fill;
		// need a bigger buffer
		/*
		 * show 已把 count 标为溢出。先 stop 释放回调资源，再
		 * 丢弃不完整内容、倍增缓冲，并以当前 index 重新 start/show。
		 */
		m->op->stop(m, p);
		kvfree(m->buf);
		m->count = 0;
		m->buf = seq_buf_alloc(m->size <<= 1);
		if (!m->buf)
			goto Enomem;
		p = m->op->start(m, &m->index);
	}
	// EOF or an error
	/* 未得到可交付记录；仍须 stop，与最后一次 start 配对。 */
	m->op->stop(m, p);
	m->count = 0;
	goto Done;
Fill:
	// one non-empty record is in the buffer; if they want more,
	// try to fit more in, but in any case we need to advance
	// the iterator once for every record shown.
	/*
	 * 缓冲已有一条完整记录。若用户还要更多，就尝试追加整条
	 * 记录；无论是否继续 show，已展示的每条都必须恰好调用一次 next，
	 * 才能维持调用者 iterator 的锁/引用和 index 协议。
	 */
	while (1) {
		size_t offs = m->count;
		loff_t pos = m->index;

		p = m->op->next(m, p, &m->index);
		if (pos == m->index) {
			/*
			 * next 必须推进 *pos；错误实现会让 seek/read 永久循环。
			 * 限速告警后内核兜底递增，优先保证系统还能向前运行。
			 */
			pr_info_ratelimited("buggy .next function %ps did not update position index\n",
					    m->op->next);
			m->index++;
		}
		if (!p || IS_ERR(p))	// no next record for us
			/* 下一位置为 EOF/错误，保留已完成记录并结束批量。 */
			break;
		if (m->count >= iov_iter_count(iter))
			break;
		err = m->op->show(m, p);
		if (err > 0) {		// ->show() says "skip it"
			/* 回滚本条产生的字节，之前完整记录仍可交付。 */
			m->count = offs;
		} else if (err || seq_has_overflowed(m)) {
			/* 硬错误或下一条放不下时同样回滚到 offs，绝不交付半条记录。 */
			m->count = offs;
			break;
		}
	}
	/* 批量迭代结束，先释放回调资源，再访问用户内存复制稳定缓冲。 */
	m->op->stop(m, p);
	n = copy_to_iter(m->buf, m->count, iter);
	copied += n;
	m->count -= n;
	m->from = n;
Done:
	/*
	 * 无复制进展时，残留 count 表示 copy_to_iter 未能接收已有数据，
	 * 返回 -EFAULT；否则返回迭代错误/EOF。已有进展时 POSIX 部分成功
	 * 优先，只推进两个字节位置，错误留待下次调用观察。
	 */
	if (unlikely(!copied)) {
		copied = m->count ? -EFAULT : err;
	} else {
		iocb->ki_pos += copied;
		m->read_pos += copied;
	}
	mutex_unlock(&m->lock);
	return copied;
Enomem:
	/* 所有分配失败汇入 Done，确保 mutex 解锁并保留统一返回语义。 */
	err = -ENOMEM;
	goto Done;
}
EXPORT_SYMBOL(seq_read_iter);

/**
 *	seq_lseek -	->llseek() method for sequential files.
 *	@file: the file in question
 *	@offset: new position
 *	@whence: 0 for absolute, 1 for relative position
 *
 *	Ready-made ->f_op->llseek()
 */
/*
 * 这是 seq_file 的通用 llseek。@offset 按 @whence 解释，
 * 仅支持 SEEK_SET/SEEK_CUR；目标是字节位置而不是记录号。
 *
 * 函数持 m->lock；若目标不同于 read_pos，就用 traverse 从头重放，
 * -EAGAIN 表示扩容后重试。成功同步 file->f_pos/read_pos 并保留跨目标
 * 记录的缓冲尾部；失败把所有位置重置为 0。返回新位置或 -EINVAL/
 * iterator/-ENOMEM 错误。
 */
loff_t seq_lseek(struct file *file, loff_t offset, int whence)
{
	struct seq_file *m = file->private_data;
	loff_t retval = -EINVAL;

	mutex_lock(&m->lock);
	/* 阶段 1：只把 SET/CUR 归一化为绝对非负字节位置。 */
	switch (whence) {
	case SEEK_CUR:
		offset += file->f_pos;
		fallthrough;
	case SEEK_SET:
		if (offset < 0)
			break;
		retval = offset;
		if (offset != m->read_pos) {
			/* 阶段 2：缓冲扩容会要求 -EAGAIN，从序列开头重新重放。 */
			while ((retval = traverse(m, offset)) == -EAGAIN)
				;
			if (retval) {
				/* with extreme prejudice... */
				/* 重放失败后彻底复位，禁止继续使用半有效 iterator。 */
				file->f_pos = 0;
				m->read_pos = 0;
				m->index = 0;
				m->count = 0;
			} else {
				m->read_pos = offset;
				retval = file->f_pos = offset;
			}
		/* 无需重放时只同步 VFS f_pos，现有缓冲窗口仍对应同一 read_pos。 */
		} else {
			file->f_pos = offset;
		}
	}
	/* 阶段 3：位置状态已一致，释放互斥锁后返回新偏移或错误。 */
	mutex_unlock(&m->lock);
	return retval;
}
EXPORT_SYMBOL(seq_lseek);

/**
 *	seq_release -	free the structures associated with sequential file.
 *	@inode: its inode
 *	@file: file in question
 *
 *	Frees the structures associated with sequential file; can be used
 *	as ->f_op->release() if you don't have private data to destroy.
 */
/*
 * 释放顺序文件的通用 release；若客户没有额外 private 数据，
 * 可直接安装为 f_op->release。@inode 未使用；@file->private_data 必须
 * 是 seq_open 分配且当前无并发操作的 m。kvfree 兼容 kmalloc/vmalloc
 * 缓冲，随后归还 slab 对象。返回 0，无失败；不会调用 iterator stop，
 * 因为每次 read/traverse 已在离开前配对。
 */
int seq_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;
	kvfree(m->buf);
	kmem_cache_free(seq_file_cache, m);
	return 0;
}
EXPORT_SYMBOL(seq_release);

/**
 * seq_escape_mem - print data into buffer, escaping some characters
 * @m: target buffer
 * @src: source buffer
 * @len: size of source buffer
 * @flags: flags to pass to string_escape_mem()
 * @esc: set of characters that need escaping
 *
 * Puts data into buffer, replacing each occurrence of character from
 * given class (defined by @flags and @esc) with printable escaped sequence.
 *
 * Use seq_has_overflowed() to check for errors.
 */
/*
 * 把 @src 的 @len 字节按 string_escape_mem 的 @flags/@esc
 * 规则转成可打印转义序列，追加到 @m。@src/@esc 均为借用输入，@m 由
 * show 回调独占。缓冲不足时 seq_commit(-1) 标记溢出，不返回 errno；
 * 调用者必须用 seq_has_overflowed() 检查，外层会扩容重试。
 */
void seq_escape_mem(struct seq_file *m, const char *src, size_t len,
		    unsigned int flags, const char *esc)
{
	char *buf;
	size_t size = seq_get_buf(m, &buf);
	int ret;

	ret = string_escape_mem(src, len, buf, size, flags, esc);
	seq_commit(m, ret < size ? ret : -1);
}
EXPORT_SYMBOL(seq_escape_mem);

/*
 * seq_vprintf() - 以 va_list 格式化追加文本并用统一方式标记溢出。
 * @m 由 show 独占；@f/@args 是借用格式与参数。成功只增加 count；
 * vsnprintf 所需长度达到剩余空间时不保留截断记录，而令 count=size，
 * 由外层扩容重放。无直接返回。
 */
void seq_vprintf(struct seq_file *m, const char *f, va_list args)
{
	int len;

	/* 先尝试在当前尾部完整格式化；成功路径只提交实际字符数。 */
	if (m->count < m->size) {
		len = vsnprintf(m->buf + m->count, m->size - m->count, f, args);
		if (m->count + len < m->size) {
			m->count += len;
			return;
		}
	}
	/* 截断长度不可信为完整记录，统一转换成 seq_file overflow 状态。 */
	seq_set_overflow(m);
}
EXPORT_SYMBOL(seq_vprintf);

/*
 * seq_printf() - seq_vprintf 的可变参数入口。
 * @m/@f 语义同上；无直接错误返回，调用者通过 overflow 状态判断。
 * va_start/va_end 只管理本次参数游标，不改变调用者数据所有权。
 */
void seq_printf(struct seq_file *m, const char *f, ...)
{
	va_list args;

	va_start(args, f);
	seq_vprintf(m, f, args);
	va_end(args);
}
EXPORT_SYMBOL(seq_printf);

#ifdef CONFIG_BINARY_PRINTF
/*
 * seq_bprintf() - 按二进制参数数组格式化追加文本。
 * @binary 是与 @f 匹配的 u32 编码参数流；仅 CONFIG_BINARY_PRINTF 构建。
 * 成功推进 count，空间不足标记整条记录溢出，不交付截断输出。
 */
void seq_bprintf(struct seq_file *m, const char *f, const u32 *binary)
{
	int len;

	/* binary 参数流只改变格式化前端，提交/溢出协议与 seq_vprintf 相同。 */
	if (m->count < m->size) {
		len = bstr_printf(m->buf + m->count, m->size - m->count, f,
				  binary);
		if (m->count + len < m->size) {
			m->count += len;
			return;
		}
	}
	/* 不交付 bstr_printf 的截断前缀，交由外层扩容重放。 */
	seq_set_overflow(m);
}
EXPORT_SYMBOL(seq_bprintf);
#endif /* CONFIG_BINARY_PRINTF */
/* 关闭 CONFIG_BINARY_PRINTF 时不提供二进制 printf 接口。 */

/**
 *	mangle_path -	mangle and copy path to buffer beginning
 *	@s: buffer start
 *	@p: beginning of path in above buffer
 *	@esc: set of characters that need escaping
 *
 *      Copy the path from @p to @s, replacing each occurrence of character from
 *      @esc with usual octal escape.
 *      Returns pointer past last written character in @s, or NULL in case of
 *      failure.
 */
/*
 * 把缓冲内从 @p 开始的路径向前搬到 @s，并把 @esc 中每个字符
 * 编成反斜杠加三位八进制。@s 与 @p 位于同一缓冲且 s<=p；循环条件和
 * “写转义前仍不追上读指针”检查允许安全原地改写。
 *
 * 成功返回最后写入字节后一位；空间重叠不足返回 NULL。函数不追加终止
 * NUL、不分配，字符按 unsigned 位掩码拆为三组八进制数字。
 */
char *mangle_path(char *s, const char *p, const char *esc)
{
	while (s <= p) {
		char c = *p++;
		/* 每轮先消费一个输入字符，再按原样一字节或八进制四字节提交。 */
		if (!c) {
			return s;
		} else if (!strchr(esc, c)) {
			*s++ = c;
		} else if (s + 4 > p) {
			break;
		} else {
			/* s + 4 <= p 已证明展开写不会覆盖尚未读取的输入。 */
			*s++ = '\\';
			*s++ = '0' + ((c & 0300) >> 6);
			*s++ = '0' + ((c & 070) >> 3);
			*s++ = '0' + (c & 07);
		}
	}
	return NULL;
}
EXPORT_SYMBOL(mangle_path);

/**
 * seq_path - seq_file interface to print a pathname
 * @m: the seq_file handle
 * @path: the struct path to print
 * @esc: set of characters to escape in the output
 *
 * return the absolute path of 'path', as represented by the
 * dentry / mnt pair in the path parameter.
 */
/*
 * 把 @path 的 dentry/mount 组合解析为绝对路径，并转义 @esc
 * 字符后追加到 @m。d_path 从缓冲尾部向前生成，mangle_path 再原地搬到
 * 开头。成功返回写入字节数；失败/空间不足返回负值并通过 seq_commit(-1)
 * 标记溢出，外层可扩容重试。path 为借用且须由调用者稳定。
 */
int seq_path(struct seq_file *m, const struct path *path, const char *esc)
{
	char *buf;
	size_t size = seq_get_buf(m, &buf);
	int res = -1;

	/* 在 seq_file 剩余区中原地完成“尾部生成、前移转义”两阶段。 */
	if (size) {
		char *p = d_path(path, buf, size);
		if (!IS_ERR(p)) {
			char *end = mangle_path(buf, p, esc);
			if (end)
				res = end - buf;
		}
	}
	/* res<0 作为 overflow 提交；不会把路径的截断前缀暴露给用户。 */
	seq_commit(m, res);

	return res;
}
EXPORT_SYMBOL(seq_path);

/**
 * seq_file_path - seq_file interface to print a pathname of a file
 * @m: the seq_file handle
 * @file: the struct file to print
 * @esc: set of characters to escape in the output
 *
 * return the absolute path to the file.
 */
/*
 * 从持有引用的 @file 借用 f_path，复用 seq_path 输出绝对路径。
 * 返回值、转义与溢出语义完全相同，不取得额外 file/path 引用。
 *
 * 契约补充：@m 是持锁调用链中的输出缓冲，@file 为调用者持有引用的输入，
 * @esc 是 NUL 结尾转义字符集合且在调用期间有效。路径解析可能睡眠；
 * 返回 0、-ENAMETOOLONG 或路径 helper errno，输入 ownership 均不转移。
 */
int seq_file_path(struct seq_file *m, struct file *file, const char *esc)
{
	return seq_path(m, &file->f_path, esc);
}
EXPORT_SYMBOL(seq_file_path);

/*
 * Same as seq_path, but relative to supplied root.
 */
/*
 * 与 seq_path 相同，但输出限制在调用者提供的 @root 之下。
 *
 * @m/@path/@root/@esc 均为借用；__d_path 返回 NULL 表示 path 不在 root
 * 可表示范围，映射为 SEQ_SKIP，让 show 丢弃当前记录。真实 helper 错误
 * 原样返回；-ENAMETOOLONG 通过 overflow 状态请求扩容，但对 show 返回 0，
 * 避免被当成不可恢复硬错误。
 */
int seq_path_root(struct seq_file *m, const struct path *path,
		  const struct path *root, const char *esc)
{
	char *buf;
	size_t size = seq_get_buf(m, &buf);
	int res = -ENAMETOOLONG;

	/* 阶段 1：相对 root 解析；NULL 是“不在 root 下”的 SEQ_SKIP 语义。 */
	if (size) {
		char *p;

		p = __d_path(path, root, buf, size);
		if (!p)
			return SEQ_SKIP;
		res = PTR_ERR(p);
		if (!IS_ERR(p)) {
			/* 阶段 2：解析成功后原地转义，重叠不足统一视为需扩容。 */
			char *end = mangle_path(buf, p, esc);
			if (end)
				res = end - buf;
			else
				res = -ENAMETOOLONG;
		}
	}
	/* 仅真实路径错误向 show 传播；空间错误保留在 overflow 状态中。 */
	seq_commit(m, res);

	return res < 0 && res != -ENAMETOOLONG ? res : 0;
}

/*
 * returns the path of the 'dentry' from the root of its filesystem.
 */
/*
 * 输出 @dentry 相对其文件系统根的路径，不跨 mount 解析。
 * dentry 必须由调用者稳定；成功返回字节数，失败返回负值并把 m 标为
 * overflow。该接口适合只拥有 dentry、没有完整 struct path 的调用者。
 */
int seq_dentry(struct seq_file *m, struct dentry *dentry, const char *esc)
{
	char *buf;
	size_t size = seq_get_buf(m, &buf);
	int res = -1;

	/* dentry_path 在尾部生成，mangle_path 再安全前移并转义。 */
	if (size) {
		char *p = dentry_path(dentry, buf, size);
		if (!IS_ERR(p)) {
			char *end = mangle_path(buf, p, esc);
			if (end)
				res = end - buf;
		}
	}
	/* 负 res 触发 overflow，禁止输出部分路径。 */
	seq_commit(m, res);

	return res;
}
EXPORT_SYMBOL(seq_dentry);

/*
 * single_start() - 单记录 seq_file 的 start 回调。
 * @p 未使用；@pos==0 返回非数据哨兵 SEQ_START_TOKEN 触发一次 show，
 * 其他位置返回 NULL/EOF。哨兵不需引用，也不能当真实对象解引用。
 */
void *single_start(struct seq_file *p, loff_t *pos)
{
	return *pos ? NULL : SEQ_START_TOKEN;
}

/*
 * single_next() - 消费唯一记录并把位置推进到 EOF。
 * @p/@v 不需使用；@pos 增一满足 seq iterator 必须前进的契约，返回 NULL。
 */
static void *single_next(struct seq_file *p, void *v, loff_t *pos)
{
	++*pos;
	return NULL;
}

/*
 * single_stop() - 单记录 iterator 的空收尾回调。
 * 没有锁或引用需要释放；保留该函数是为了提供完整 seq_operations 契约。
 */
static void single_stop(struct seq_file *p, void *v)
{
}

/*
 * single_open() - 为只需一次 show 的合成文件动态构造 iterator。
 *
 * @file：新 file；@show：调用者回调，接收 SEQ_START_TOKEN；@data 借用
 * 私有指针，生命周期由调用者/外层 file 管理。函数分配 ops，再调用
 * seq_open；成功把 data 存入 m->private，ops ownership 转给
 * single_release。失败释放 ops，返回 -ENOMEM 或 seq_open errno。
 */
int single_open(struct file *file, int (*show)(struct seq_file *, void *),
		void *data)
{
	struct seq_operations *op = kmalloc_obj(*op, GFP_KERNEL_ACCOUNT);
	int res = -ENOMEM;

	/* 阶段 1：动态 ops 把通用 start/next/stop 与客户 show 组合起来。 */
	if (op) {
		op->start = single_start;
		op->next = single_next;
		op->stop = single_stop;
		op->show = show;
		/* 阶段 2：seq_open 成功才把 ops 和 data 的生命周期绑定到 file。 */
		res = seq_open(file, op);
		if (!res)
			((struct seq_file *)file->private_data)->private = data;
		else
			kfree(op);
	}
	return res;
}
EXPORT_SYMBOL(single_open);

/*
 * single_open_size() - 以调用者指定初始缓冲创建单记录 seq_file。
 * @size 为字节数；先分配缓冲，再 single_open，任一步失败完整回滚。
 * 成功后缓冲 ownership 转给 seq_file，release 用 kvfree；返回 0/-ENOMEM。
 * 较大初始值可避免已知大记录的扩容重放，代价是每个 open 立即占用更多内存。
 */
int single_open_size(struct file *file, int (*show)(struct seq_file *, void *),
		void *data, size_t size)
{
	char *buf = seq_buf_alloc(size);
	int ret;
	/* 缓冲尚未交给 seq_file；open 失败时仍由本函数负责回滚。 */
	if (!buf)
		return -ENOMEM;
	ret = single_open(file, show, data);
	if (ret) {
		kvfree(buf);
		return ret;
	}
	/* single_open 成功后替换惰性缓冲配置，ownership 转给 seq_release。 */
	((struct seq_file *)file->private_data)->buf = buf;
	((struct seq_file *)file->private_data)->size = size;
	return 0;
}
EXPORT_SYMBOL(single_open_size);

/*
 * single_release() - 释放 single_open 动态 ops 与通用 seq_file 状态。
 * 必须先保存 op，再由 seq_release 释放 m，最后 kfree(op)；@inode 借用。
 * 返回 seq_release 的 0。只适用于 single_open 家族，不可用于静态 ops。
 */
int single_release(struct inode *inode, struct file *file)
{
	const struct seq_operations *op = ((struct seq_file *)file->private_data)->op;
	int res = seq_release(inode, file);
	kfree(op);
	return res;
}
EXPORT_SYMBOL(single_release);

/*
 * seq_release_private() - 释放 __seq_open_private 分配的私有区再通用释放。
 * @file->private_data 必须是对应 open 建立的 m；先 kfree private 并清空，
 * 防止后续误用，再 seq_release 缓冲/m。返回 0。
 */
int seq_release_private(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;

	kfree(seq->private);
	seq->private = NULL;
	return seq_release(inode, file);
}
EXPORT_SYMBOL(seq_release_private);

/*
 * __seq_open_private() - 分配零初始化私有区并绑定普通 seq_file。
 *
 * @f/@ops 同 seq_open；@psize 为私有字节数。成功返回 m->private（由
 * seq_release_private 释放）；任一步失败返回 NULL 并逆序释放候选区。
 * 注意 seq_open 的非 -ENOMEM 错误也压缩为 NULL，公共包装统一报 -ENOMEM。
 */
void *__seq_open_private(struct file *f, const struct seq_operations *ops,
		int psize)
{
	int rc;
	void *private;
	struct seq_file *seq;

	/* 阶段 1：私有区独立分配，尚未与 file 绑定。 */
	private = kzalloc(psize, GFP_KERNEL_ACCOUNT);
	if (private == NULL)
		goto out;

	/* 阶段 2：建立 seq_file；成功后 private 由 release_private 回收。 */
	rc = seq_open(f, ops);
	if (rc < 0)
		goto out_free;

	seq = f->private_data;
	seq->private = private;
	return private;

out_free:
	/* seq_open 未接管 private，仍由本函数释放。 */
	kfree(private);
out:
	return NULL;
}
EXPORT_SYMBOL(__seq_open_private);

/*
 * seq_open_private() - 把私有区指针返回约定包装成 open 风格 errno。
 * 参数语义同 __seq_open_private；成功 0，任何失败 -ENOMEM。
 */
int seq_open_private(struct file *filp, const struct seq_operations *ops,
		int psize)
{
	return __seq_open_private(filp, ops, psize) ? 0 : -ENOMEM;
}
EXPORT_SYMBOL(seq_open_private);

/*
 * seq_putc() - 向 m 追加一个字符。
 * 缓冲已满时保持 count>=size 的 overflow 状态，不越界写；无返回。
 * 调用者通过 seq_has_overflowed() 观察失败。
 */
void seq_putc(struct seq_file *m, char c)
{
	if (m->count >= m->size)
		return;

	m->buf[m->count++] = c;
}
EXPORT_SYMBOL(seq_putc);

/*
 * __seq_puts() - 追加 NUL 结尾字符串但不写终止符。
 * @s 为借用且在调用期间稳定；复用 seq_write 的完整写或 overflow 语义。
 */
void __seq_puts(struct seq_file *m, const char *s)
{
	seq_write(m, s, strlen(s));
}
EXPORT_SYMBOL(__seq_puts);

/**
 * seq_put_decimal_ull_width - A helper routine for putting decimal numbers
 * 			       without rich format of printf().
 * only 'unsigned long long' is supported.
 * @m: seq_file identifying the buffer to which data should be written
 * @delimiter: a string which is printed before the number
 * @num: the number
 * @width: a minimum field width
 *
 * This routine will put strlen(delimiter) + number into seq_filed.
 * This routine is very quick when you show lots of numbers.
 * In usual cases, it will be better to use seq_printf(). It's easier to read.
 */
/*
 * 快速输出十进制 unsigned long long，可选先写 @delimiter，
 * @width 是最小字段宽度。它避免 printf 的通用解析开销，适合大量数字；
 * 普通场景 seq_printf 更易读。
 *
 * 预留至少两个字节并在每步检查容量；任何不足都调用 seq_set_overflow，
 * 不留下可被当成完整记录的截断结果。无直接返回。
 */
void seq_put_decimal_ull_width(struct seq_file *m, const char *delimiter,
			 unsigned long long num, unsigned int width)
{
	int len;

	if (m->count + 2 >= m->size) /* we'll write 2 bytes at least */
		/* 分隔符加至少一位数字，保守要求可容纳两个字节。 */
		goto overflow;

	if (delimiter && delimiter[0]) {
		if (delimiter[1] == 0)
			seq_putc(m, delimiter[0]);
		else
			seq_puts(m, delimiter);
	}

	/* 阶段 2：规范化宽度并在转换前一次性验证最小剩余容量。 */
	if (!width)
		width = 1;

	if (m->count + width >= m->size)
		goto overflow;

	len = num_to_str(m->buf + m->count, m->size - m->count, num, width);
	if (!len)
		goto overflow;

	m->count += len;
	return;

overflow:
	/* 任一容量/转换失败都统一标记外层需扩容重放。 */
	seq_set_overflow(m);
}

/*
 * seq_put_decimal_ull() - 无最小宽度的十进制快速包装。
 * width=0 在底层规范化为 1；其余参数、溢出和所有权语义不变。
 */
void seq_put_decimal_ull(struct seq_file *m, const char *delimiter,
			 unsigned long long num)
{
	return seq_put_decimal_ull_width(m, delimiter, num, 0);
}
EXPORT_SYMBOL(seq_put_decimal_ull);

/**
 * seq_put_hex_ll - put a number in hexadecimal notation
 * @m: seq_file identifying the buffer to which data should be written
 * @delimiter: a string which is printed before the number
 * @v: the number
 * @width: a minimum field width
 *
 * seq_put_hex_ll(m, "", v, 8) is equal to seq_printf(m, "%08llx", v)
 *
 * This routine is very quick when you show lots of numbers.
 * In usual cases, it will be better to use seq_printf(). It's easier to read.
 */
/*
 * 以十六进制输出 @v，@width 指定最小零填充宽度；示例调用
 * 等价于 "%08llx"。同十进制 helper，它为大量数字优化，容量不足只
 * 标记 overflow，让外层丢弃并重放完整记录。
 */
void seq_put_hex_ll(struct seq_file *m, const char *delimiter,
				unsigned long long v, unsigned int width)
{
	unsigned int len;
	int i;

	/* 阶段 1：先追加可选分隔符；单字符走 seq_putc 快路径。 */
	if (delimiter && delimiter[0]) {
		if (delimiter[1] == 0)
			seq_putc(m, delimiter[0]);
		else
			seq_puts(m, delimiter);
	}
	/* 分隔符写入也遵循 seq_file 的完整记录/overflow 协议。 */

	/* 阶段 2：计算有效十六进制位数，再与调用者最小宽度取较大者。 */
	/* If x is 0, the result of __builtin_clzll is undefined */
	/*
	 * 编译器内建前导零计数对 0 没有定义，必须单独把 0 设为
	 * 一位；非零值按最高有效 bit 向上换算为十六进制位数。
	 */
	if (v == 0)
		len = 1;
	else
		len = (sizeof(v) * 8 - __builtin_clzll(v) + 3) / 4;

	if (len < width)
		len = width;

	if (m->count + len > m->size) {
		seq_set_overflow(m);
		return;
	}

	/* 阶段 3：从低位取 nibble、反向填充，最后原子推进逻辑尾部。 */
	for (i = len - 1; i >= 0; i--) {
		m->buf[m->count + i] = hex_asc[0xf & v];
		v = v >> 4;
	}
	m->count += len;
}

/*
 * seq_put_decimal_ll() - 快速追加有符号十进制数及可选分隔符。
 * @num 为 long long；负数先写 '-' 再转换绝对值，单数字走快速路径，
 * 其余交给 num_to_str。空间/转换不足统一标记 overflow，无直接错误返回。
 */
void seq_put_decimal_ll(struct seq_file *m, const char *delimiter, long long num)
{
	int len;

	if (m->count + 3 >= m->size) /* we'll write 2 bytes at least */
		/* 为分隔符、负号和至少一位数字保守预留空间。 */
		goto overflow;

	if (delimiter && delimiter[0]) {
		if (delimiter[1] == 0)
			seq_putc(m, delimiter[0]);
		else
			seq_puts(m, delimiter);
	}

	/* 阶段 2：写符号前再次保证“符号加至少一位数字”的空间。 */
	if (m->count + 2 >= m->size)
		goto overflow;

	if (num < 0) {
		m->buf[m->count++] = '-';
		num = -num;
	}

	if (num < 10) {
		m->buf[m->count++] = num + '0';
		return;
	}

	/* 多位绝对值交给无 printf 解析开销的 num_to_str。 */
	len = num_to_str(m->buf + m->count, m->size - m->count, num, 0);
	if (!len)
		goto overflow;

	m->count += len;
	return;

overflow:
	seq_set_overflow(m);
}
EXPORT_SYMBOL(seq_put_decimal_ll);

/**
 * seq_write - write arbitrary data to buffer
 * @seq: seq_file identifying the buffer to which data should be written
 * @data: data address
 * @len: number of bytes
 *
 * Return 0 on success, non-zero otherwise.
 */
/*
 * 把 @data 的 @len 个任意字节完整追加到 @seq。只有严格小于
 * size 才复制，保留 seq 缓冲的边界哨兵语义；成功 0，空间不足标记
 * overflow 并返回 -1。data 为借用，不保留；memcpy 前已完成边界检查。
 */
int seq_write(struct seq_file *seq, const void *data, size_t len)
{
	if (seq->count + len < seq->size) {
		memcpy(seq->buf + seq->count, data, len);
		seq->count += len;
		return 0;
	}
	seq_set_overflow(seq);
	return -1;
}
EXPORT_SYMBOL(seq_write);

/**
 * seq_pad - write padding spaces to buffer
 * @m: seq_file identifying the buffer to which data should be written
 * @c: the byte to append after padding if non-zero
 */
/*
 * 先用空格把输出推进到 m->pad_until，再可选追加 @c。
 * pad_until/count/size 单位均为字节；空间不足时不写部分 padding，而标记
 * overflow。@c==0 表示不追加尾字符，无直接返回。
 */
void seq_pad(struct seq_file *m, char c)
{
	int size = m->pad_until - m->count;
	/* 只有整个 padding 可容纳时才写，保持单条 show 输出不可截断。 */
	if (size > 0) {
		if (size + m->count > m->size) {
			seq_set_overflow(m);
			return;
		}
		memset(m->buf + m->count, ' ', size);
		m->count += size;
	}
	/* 可选尾字符复用 seq_putc 的统一 overflow 语义。 */
	if (c)
		seq_putc(m, c);
}
EXPORT_SYMBOL(seq_pad);

/* A complete analogue of print_hex_dump() */
/*
 * 这是把 print_hex_dump() 的完整布局写入 seq_file 的等价接口。
 *
 * @prefix_str/@prefix_type 控制每行前缀；@rowsize 仅接受 16/32，否则归一
 * 为 16；@groupsize 控制字节分组；@buf/@len 是借用输入；@ascii 决定
 * 是否追加字符栏。逐行格式化，任一 helper 标记 overflow 即停止，外层
 * 扩容后重放整条 show。无直接返回。
 */
void seq_hex_dump(struct seq_file *m, const char *prefix_str, int prefix_type,
		  int rowsize, int groupsize, const void *buf, size_t len,
		  bool ascii)
{
	const u8 *ptr = buf;
	int i, linelen, remaining = len;
	char *buffer;
	size_t size;
	int ret;

	/* 阶段 1：规范化每行宽度；其余参数由 hex_dump_to_buffer 校验/解释。 */
	if (rowsize != 16 && rowsize != 32)
		rowsize = 16;

	for (i = 0; i < len && !seq_has_overflowed(m); i += rowsize) {
		linelen = min(remaining, rowsize);
		remaining -= rowsize;

		/* 阶段 2：先生成本行地址/偏移/普通前缀。 */
		switch (prefix_type) {
		case DUMP_PREFIX_ADDRESS:
			seq_printf(m, "%s%p: ", prefix_str, ptr + i);
			break;
		case DUMP_PREFIX_OFFSET:
			seq_printf(m, "%s%.8x: ", prefix_str, i);
			break;
		/* NONE 与未知类型均只写固定前缀，不附加地址或偏移。 */
		default:
			seq_printf(m, "%s", prefix_str);
			break;
		}

		/* 阶段 3：直接格式化到剩余区，完整提交后再追加换行。 */
		size = seq_get_buf(m, &buffer);
		ret = hex_dump_to_buffer(ptr + i, linelen, rowsize, groupsize,
					 buffer, size, ascii);
		seq_commit(m, ret < size ? ret : -1);

		seq_putc(m, '\n');
	}
}
EXPORT_SYMBOL(seq_hex_dump);

/*
 * seq_list_start() - 按零基记录位置定位普通双向链表节点。
 * @head：借用哨兵头；@pos：首元素为 0。返回借用 list_head 或 EOF NULL。
 * 本函数不加锁，调用者须在 start/stop 之间自行持有保护链表的锁。
 */
struct list_head *seq_list_start(struct list_head *head, loff_t pos)
{
	struct list_head *lh;

	list_for_each(lh, head)
		if (pos-- == 0)
			return lh;

	return NULL;
}
EXPORT_SYMBOL(seq_list_start);

/*
 * seq_list_start_head() - 把链表头作为位置 0 的标题令牌。
 * pos==0 返回 @head（show 必须识别为标题），数据节点从位置 1 开始；
 * 其余复用 seq_list_start。锁与所有权语义同上。
 */
struct list_head *seq_list_start_head(struct list_head *head, loff_t pos)
{
	if (!pos)
		return head;

	return seq_list_start(head, pos - 1);
}
EXPORT_SYMBOL(seq_list_start_head);

/*
 * seq_list_next() - 推进普通链表 iterator 并同步逻辑位置。
 * @v 为当前节点或标题头；@head 为哨兵；@ppos 输入输出位置。返回下一
 * 借用节点，回到 head 时返回 NULL。调用者仍须持外部锁。
 */
struct list_head *seq_list_next(void *v, struct list_head *head, loff_t *ppos)
{
	struct list_head *lh;

	lh = ((struct list_head *)v)->next;
	++*ppos;
	return lh == head ? NULL : lh;
}
EXPORT_SYMBOL(seq_list_next);

/*
 * seq_list_start_rcu() - 在 RCU 读侧按位置定位链表节点。
 * 调用者必须从 start 到 stop 持 rcu_read_lock；返回裸节点只在该临界区
 * 有生命周期保证，字段一致性仍取决于各字段自己的同步协议。
 */
struct list_head *seq_list_start_rcu(struct list_head *head, loff_t pos)
{
	struct list_head *lh;

	list_for_each_rcu(lh, head)
		if (pos-- == 0)
			return lh;

	return NULL;
}
EXPORT_SYMBOL(seq_list_start_rcu);

/*
 * seq_list_start_head_rcu() - RCU 链表的标题令牌版本。
 * 位置 0 返回 head，数据从 1 开始；调用者仍需覆盖整个 iterator 的
 * RCU 读锁，不能把返回裸指针带出 stop。
 */
struct list_head *seq_list_start_head_rcu(struct list_head *head, loff_t pos)
{
	if (!pos)
		return head;

	return seq_list_start_rcu(head, pos - 1);
}
EXPORT_SYMBOL(seq_list_start_head_rcu);

/*
 * seq_list_next_rcu() - 使用 list_next_rcu 推进并更新 *ppos。
 * 返回 NULL 表示回到哨兵。RCU 只防止节点内存回收，不冻结链表内容；
 * 并发增删可能使一次 seq 输出成为允许的弱一致快照。
 */
struct list_head *seq_list_next_rcu(void *v, struct list_head *head,
				    loff_t *ppos)
{
	struct list_head *lh;

	lh = list_next_rcu((struct list_head *)v);
	++*ppos;
	return lh == head ? NULL : lh;
}
EXPORT_SYMBOL(seq_list_next_rcu);

/**
 * seq_hlist_start - start an iteration of a hlist
 * @head: the head of the hlist
 * @pos:  the start position of the sequence
 *
 * Called at seq_file->op->start().
 */
/*
 * 供 seq_file start 回调按零基 @pos 定位 hlist 节点。
 * @head 由调用者稳定；返回借用节点或 NULL/EOF，不加锁、不增引用。
 * 普通版本要求调用者在 iterator 外持有 hlist 的互斥保护。
 */
struct hlist_node *seq_hlist_start(struct hlist_head *head, loff_t pos)
{
	struct hlist_node *node;

	hlist_for_each(node, head)
		if (pos-- == 0)
			return node;
	return NULL;
}
EXPORT_SYMBOL(seq_hlist_start);

/**
 * seq_hlist_start_head - start an iteration of a hlist
 * @head: the head of the hlist
 * @pos:  the start position of the sequence
 *
 * Called at seq_file->op->start(). Call this function if you want to
 * print a header at the top of the output.
 */
/*
 * 标题版本在位置 0 返回 SEQ_START_TOKEN，数据位置整体加一；
 * show 必须识别哨兵而非解引用。其余位置委托 seq_hlist_start。
 */
struct hlist_node *seq_hlist_start_head(struct hlist_head *head, loff_t pos)
{
	if (!pos)
		return SEQ_START_TOKEN;

	return seq_hlist_start(head, pos - 1);
}
EXPORT_SYMBOL(seq_hlist_start_head);

/**
 * seq_hlist_next - move to the next position of the hlist
 * @v:    the current iterator
 * @head: the head of the hlist
 * @ppos: the current position
 *
 * Called at seq_file->op->next().
 */
/*
 * 供 next 回调推进 hlist。每次先递增 @ppos；若当前是标题哨兵，
 * 返回首节点，否则返回 node->next。NULL 自然表示 EOF。返回均为借用指针，
 * 外部锁协议必须与 start/stop 成对覆盖。
 */
struct hlist_node *seq_hlist_next(void *v, struct hlist_head *head,
				  loff_t *ppos)
{
	struct hlist_node *node = v;

	/* 位置先推进；起始哨兵与普通节点分别选择 head 或 next 发布边。 */
	++*ppos;
	if (v == SEQ_START_TOKEN)
		return head->first;
	else
		return node->next;
}
EXPORT_SYMBOL(seq_hlist_next);

/**
 * seq_hlist_start_rcu - start an iteration of a hlist protected by RCU
 * @head: the head of the hlist
 * @pos:  the start position of the sequence
 *
 * Called at seq_file->op->start().
 *
 * This list-traversal primitive may safely run concurrently with
 * the _rcu list-mutation primitives such as hlist_add_head_rcu()
 * as long as the traversal is guarded by rcu_read_lock().
 */
/*
 * RCU hlist 的 start 定位器。只要遍历整体由 rcu_read_lock()
 * 保护，就可与 hlist_add_head_rcu 等更新并发；返回节点存储期安全，但
 * 不是冻结快照，也不会自动取得对象引用。
 */
struct hlist_node *seq_hlist_start_rcu(struct hlist_head *head,
				       loff_t pos)
{
	struct hlist_node *node;

	__hlist_for_each_rcu(node, head)
		if (pos-- == 0)
			return node;
	return NULL;
}
EXPORT_SYMBOL(seq_hlist_start_rcu);

/**
 * seq_hlist_start_head_rcu - start an iteration of a hlist protected by RCU
 * @head: the head of the hlist
 * @pos:  the start position of the sequence
 *
 * Called at seq_file->op->start(). Call this function if you want to
 * print a header at the top of the output.
 *
 * This list-traversal primitive may safely run concurrently with
 * the _rcu list-mutation primitives such as hlist_add_head_rcu()
 * as long as the traversal is guarded by rcu_read_lock().
 */
/*
 * RCU hlist 的标题版本；位置 0 返回 SEQ_START_TOKEN，之后按
 * pos-1 定位节点。调用者必须让 RCU 读锁跨越 start/show/next/stop。
 */
struct hlist_node *seq_hlist_start_head_rcu(struct hlist_head *head,
					    loff_t pos)
{
	if (!pos)
		return SEQ_START_TOKEN;

	return seq_hlist_start_rcu(head, pos - 1);
}
EXPORT_SYMBOL(seq_hlist_start_head_rcu);

/**
 * seq_hlist_next_rcu - move to the next position of the hlist protected by RCU
 * @v:    the current iterator
 * @head: the head of the hlist
 * @ppos: the current position
 *
 * Called at seq_file->op->next().
 *
 * This list-traversal primitive may safely run concurrently with
 * the _rcu list-mutation primitives such as hlist_add_head_rcu()
 * as long as the traversal is guarded by rcu_read_lock().
 */
/*
 * RCU hlist 的 next 回调。标题后通过 rcu_dereference 读取
 * head->first，普通节点通过同样语义读取 next，与 _rcu 写侧发布配对；
 * 每次递增位置，NULL 表示 EOF。裸节点不可带出 RCU 临界区。
 */
struct hlist_node *seq_hlist_next_rcu(void *v,
				      struct hlist_head *head,
				      loff_t *ppos)
{
	struct hlist_node *node = v;

	++*ppos;
	if (v == SEQ_START_TOKEN)
		return rcu_dereference(head->first);
	/* 普通节点的 next 同样通过 RCU 读取侧原语取得。 */
	else
		return rcu_dereference(node->next);
}
EXPORT_SYMBOL(seq_hlist_next_rcu);

/**
 * seq_hlist_start_percpu - start an iteration of a percpu hlist array
 * @head: pointer to percpu array of struct hlist_heads
 * @cpu:  pointer to cpu "cursor"
 * @pos:  start position of sequence
 *
 * Called at seq_file->op->start().
 */
/*
 * 把所有 possible CPU 的 per-CPU hlist 视为一条逻辑序列。
 * @head 是 per-CPU 数组；@cpu 为输入输出 CPU 游标；@pos 是跨 CPU 的
 * 零基记录位置。返回借用节点或 NULL。possible 集合含离线 CPU，因而
 * 可遍历其静态槽；数据并发保护仍由调用者提供。
 */
struct hlist_node *
seq_hlist_start_percpu(struct hlist_head __percpu *head, int *cpu, loff_t pos)
{
	struct hlist_node *node;

	/* 外层按 possible CPU，内层按 bucket 节点，共同消耗全局 pos。 */
	for_each_possible_cpu(*cpu) {
		hlist_for_each(node, per_cpu_ptr(head, *cpu)) {
			if (pos-- == 0)
				return node;
		}
	}
	return NULL;
}
EXPORT_SYMBOL(seq_hlist_start_percpu);

/**
 * seq_hlist_next_percpu - move to the next position of the percpu hlist array
 * @v:    pointer to current hlist_node
 * @head: pointer to percpu array of struct hlist_heads
 * @cpu:  pointer to cpu "cursor"
 * @pos:  start position of sequence
 *
 * Called at seq_file->op->next().
 */
/*
 * 先尝试当前 CPU bucket 的 node->next；到尾部后用
 * cpumask_next 扫描后续 possible CPU，返回首个非空 bucket 的首节点。
 * @cpu 与 @pos 同步推进，保证 seek 重放可恢复；返回节点为借用指针，
 * 调用者必须确保 per-CPU 链表遍历期间不会无保护释放。
 */
struct hlist_node *
seq_hlist_next_percpu(void *v, struct hlist_head __percpu *head,
			int *cpu, loff_t *pos)
{
	struct hlist_node *node = v;

	/* 每次 next 对应一个逻辑记录，因此无论是否跨 CPU 都先推进 pos。 */
	++*pos;

	/* 当前 bucket 未结束时保持 CPU 游标不变。 */
	if (node->next)
		return node->next;

	/* bucket 结束后扫描后续 possible CPU，跳过空链表。 */
	for (*cpu = cpumask_next(*cpu, cpu_possible_mask); *cpu < nr_cpu_ids;
	     *cpu = cpumask_next(*cpu, cpu_possible_mask)) {
		struct hlist_head *bucket = per_cpu_ptr(head, *cpu);

		if (!hlist_empty(bucket))
			return bucket->first;
	}
	return NULL;
}
EXPORT_SYMBOL(seq_hlist_next_percpu);

/*
 * seq_file_init() - 启动期创建 seq_file 专用 slab。
 * 入参/直接返回：无。SLAB_ACCOUNT 纳入内存记账，SLAB_PANIC 表示失败
 * 无法降级；成功后 seq_file_cache 由 __ro_after_init 固定供 open 使用。
 */
void __init seq_file_init(void)
{
	seq_file_cache = KMEM_CACHE(seq_file, SLAB_ACCOUNT|SLAB_PANIC);
}
