// SPDX-License-Identifier: GPL-2.0

/*
 * ELF build-id 读取学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5），2026-07-27。
 * 分析基线：分支 doc/lql，提交 83476cc97bc6。
 * 方法依据：doc/linux-kernel-source-learning-methodology.md。
 *
 * 文件职责：
 *   把 ELF 文件或一段内存中的 GNU build-id note 解析成最长
 *   BUILD_ID_SIZE_MAX 字节的稳定标识。这里负责安全读取、ELF program
 *   header/note 遍历和结果复制；不验证 build-id 的密码学性质，不缓存
 *   普通用户 ELF 的结果，也不管理传入 file/VMA 的生命周期。
 *
 * 主调用链：
 *   perf/BPF/proc VMA 查询
 *     -> build_id_parse[_nofault]()/build_id_parse_file()
 *     -> __build_id_parse()
 *     -> get_build_id_32()/get_build_id_64()
 *     -> parse_build_id()
 *     -> freader_fetch()
 *
 *   模块或内核启动 note
 *     -> build_id_parse_buf()
 *     -> parse_build_id()
 *
 * 核心对象与所有权：
 *   struct freader 是栈上读取游标，借用 file、内存缓冲区和调用者提供的
 *   临时缓冲区。文件无缺页模式最多持有一个 page-cache folio 引用及其
 *   local mapping，下一次跨 folio 读取或 cleanup 时成对释放。
 *
 * 并发与上下文：
 *   每个 freader 只供一次调用链私有使用，无内部锁。may_fault=true 允许
 *   __kernel_read() 睡眠并把数据读入页缓存；false 路径只查询已存在且
 *   Uptodate 的 folio，适合 perf 等不能触发文件 I/O 的路径。READ_ONCE()
 *   防止解析期间从可变化映射重复读取 ELF 字段得到自相矛盾的值，但不把
 *   整个文件冻结成一致快照。
 *
 * 安全边界与权衡：
 *   所有来自 ELF 的长度、偏移和计数都按不可信输入处理，先检查溢出和
 *   上界再读取。统一 freader 接口简化解析器，代价是跨 folio 请求需要
 *   复制到固定临时缓冲；无缺页模式则以可能返回 -EFAULT 换取不阻塞。
 */

#include <linux/buildid.h>
#include <linux/cache.h>
#include <linux/elf.h>
#include <linux/kernel.h>
#include <linux/pagemap.h>
#include <linux/fs.h>
#include <linux/secretmem.h>

/* ELF note 的 n_type=3 表示 GNU build-id；名称还必须精确匹配 "GNU\0"。 */
#define BUILD_ID 3

/*
 * 限制从不可信 ELF 读取的 program header 数量。超过部分不再扫描，
 * 防止伪造 e_phnum 造成长时间循环；因此异常 ELF 的 build-id 可能找不到。
 */
#define MAX_PHDR_CNT 256

/*
 * freader_init_from_file() - 初始化文件后端读取器。
 *
 * r 是调用者拥有的输出对象；buf/buf_sz 是解析期借用的线性暂存区；
 * file 是已由调用者稳定生命周期的借用引用，本函数不 get_file()；
 * may_fault 决定后续 fetch 能否通过 __kernel_read() 睡眠并发起 I/O。
 * 函数清空旧状态后发布这些输入，无失败返回；若 r 先前仍持有 folio，
 * 调用者必须先 cleanup，不能用重新初始化代替释放。
 */
void freader_init_from_file(struct freader *r, void *buf, u32 buf_sz,
			    struct file *file, bool may_fault)
{
	memset(r, 0, sizeof(*r));
	r->buf = buf;
	r->buf_sz = buf_sz;
	r->file = file;
	r->may_fault = may_fault;
}

/*
 * freader_init_from_mem() - 初始化连续内存后端读取器。
 *
 * r 为输出对象；data/data_sz 描述调用期间保持有效的只读借用区间。
 * buf 保持 NULL，后续 freader_fetch() 据此选择无复制的内存路径。
 * 本函数不验证数据内容、不取得引用，也不分配资源；返回：无。
 */
void freader_init_from_mem(struct freader *r, const char *data, u64 data_sz)
{
	memset(r, 0, sizeof(*r));
	r->data = data;
	r->data_sz = data_sz;
}

/*
 * freader_put_folio() - 释放文件无缺页模式缓存的当前 folio。
 *
 * r 由当前调用链独占；folio 非 NULL 时，addr 必定是该 folio 的
 * kmap_local 映射。必须先 kunmap_local() 再 folio_put()，避免映射
 * 继续指向已归还页缓存的对象。NULL 表示没有资源，函数可幂等调用且
 * 不睡眠。
 */
static void freader_put_folio(struct freader *r)
{
	if (!r->folio)
		return;
	kunmap_local(r->addr);
	folio_put(r->folio);
	r->folio = NULL;
}

/*
 * freader_get_folio() - 让 r 缓存覆盖 file_off 的 Uptodate 页缓存 folio。
 *
 * file_off 是文件字节偏移，必须非负且已由上层完成范围检查。函数仅用于
 * 文件、may_fault=false 模式；r->file 是借用引用。命中当前 folio 时
 * 保留现有引用和映射；否则先释放旧 folio，再用 filemap_get_folio()
 * 查找页缓存，绝不发起读取。
 *
 * 成功返回 0，并持有一个 folio 引用和 local mapping，直到下一次换页或
 * cleanup。不存在/错误/尚未 Uptodate 都折叠为 -EFAULT，失败时 r 不持有
 * folio。local mapping 要求同一执行上下文按栈式规则释放，不能跨调用链
 * 长期保存 addr。
 */
static int freader_get_folio(struct freader *r, loff_t file_off)
{
	/* check if we can just reuse current folio */
	/*
	 * 当前 folio 已覆盖目标偏移：复用引用和映射，避免重复查页缓存。
	 */
	if (r->folio && file_off >= r->folio_off &&
	    file_off < r->folio_off + folio_size(r->folio))
		return 0;

	/*
	 * 换页前先撤销 local mapping 并归还旧引用；r 同时最多持有一个
	 * folio。
	 */
	freader_put_folio(r);

	/* only use page cache lookup - fail if not already cached */
	/*
	 * 页索引以 PAGE_SHIFT 换算；large folio 命中后再通过 folio_pos/size
	 * 恢复其完整文件区间。此接口不会像 read path 那样填充缺失页面。
	 */
	r->folio = filemap_get_folio(r->file->f_mapping, file_off >> PAGE_SHIFT);

	/*
	 * 错误指针表示缓存中没有可用 folio；非 Uptodate folio 虽有对象，
	 * 其内容也不能用于解析。后一种情况需显式 put 已取得的引用。
	 */
	if (IS_ERR(r->folio) || !folio_test_uptodate(r->folio)) {
		if (!IS_ERR(r->folio))
			folio_put(r->folio);
		r->folio = NULL;
		return -EFAULT;
	}

	r->folio_off = folio_pos(r->folio);
	/* 建立 CPU-local 临时映射；对应释放集中在 freader_put_folio()。 */
	r->addr = kmap_local_folio(r->folio, 0);

	return 0;
}

/*
 * freader_fetch() - 从统一后端取得 [file_off, file_off + sz) 的连续视图。
 *
 * r 是可修改的私有游标；file_off/sz 分别是字节偏移与长度。返回指针只是
 * 借用视图：内存模式指向 data；可缺页文件模式指向 r->buf；无缺页单
 * folio 模式可能直接指向 local mapping。任何后续 fetch 都可能覆盖 buf
 * 或更换 folio，因此会使先前返回值失效，调用者必须先复制所需字段。
 *
 * 成功返回可读 sz 字节的指针。失败返回 NULL 并把精确错误写入 r->err：
 * -E2BIG 表示暂存区不足，-EOVERFLOW 表示区间加法溢出，-ERANGE 表示内存
 * 后端越界，-EFAULT 表示禁止读取 secretmem 或无缺页缓存不可用，
 * __kernel_read() 的负 errno 原样保留，短读转换为 -EIO。
 *
 * may_fault=true 可能睡眠；false 路径不发起 I/O，但 page-cache 查找和
 * local mapping 仍要求调用者遵守其上下文约束。函数不接管任何输入对象。
 */
const void *freader_fetch(struct freader *r, loff_t file_off, size_t sz)
{
	/*
	 * 变量地图：
	 * folio_sz 是当前 folio 覆盖的字节数；跨 folio 分支中的 part_sz
	 * 是本轮复制量，off 是已复制到 r->buf 的累计字节数。
	 */
	size_t folio_sz;

	/* provided internal temporary buffer should be sized correctly */
	/*
	 * 文件模式依赖调用者提供的暂存区承接 __kernel_read 或跨 folio
	 * 拼接。WARN_ON 暴露内部契约违例，同时用 -E2BIG 安全终止。
	 */
	if (WARN_ON(r->buf && sz > r->buf_sz)) {
		r->err = -E2BIG;
		return NULL;
	}

	if (unlikely(file_off + sz < file_off)) {
		r->err = -EOVERFLOW;
		return NULL;
	}

	/* working with memory buffer is much more straightforward */
	/*
	 * buf==NULL 是内存后端判据。边界检查后直接返回 data 内部指针，
	 * 不复制、不取得引用，生命周期仍由初始化时的调用者保证。
	 */
	if (!r->buf) {
		if (file_off + sz > r->data_sz) {
			r->err = -ERANGE;
			return NULL;
		}
		return r->data + file_off;
	}

	/* reject secretmem folios created with memfd_secret() */
	/*
	 * secretmem 明确禁止通过普通页缓存映射暴露内容；即使数据似乎
	 * 在缓存，build-id 辅助代码也不能绕过其保密语义。
	 */
	if (secretmem_mapping(r->file->f_mapping)) {
		r->err = -EFAULT;
		return NULL;
	}

	/* use __kernel_read() for sleepable context */
	/*
	 * 可睡眠路径直接执行精确长度读取。file_off 是局部副本，
	 * __kernel_read() 推进它不会改变 file->f_pos，也不会泄露给调用者。
	 */
	if (r->may_fault) {
		ssize_t ret;

		ret = __kernel_read(r->file, r->buf, sz, &file_off);
		if (ret != sz) {
			r->err = (ret < 0) ? ret : -EIO;
			return NULL;
		}
		return r->buf;
	}

	/* fetch or reuse folio for given file offset */
	/* 无缺页路径先稳定包含起点的 folio，失败时 r->err 已可直接传播。 */
	r->err = freader_get_folio(r, file_off);
	if (r->err)
		return NULL;

	/* if requested data is crossing folio boundaries, we have to copy
	 * everything into our local buffer to keep a simple linear memory
	 * access interface
	 */
	/*
	 * 跨 folio 时不能返回两个不连续映射，所以把各段依次拼入 r->buf。
	 * 每次 get 会使旧 addr 失效，必须先复制本段再切换到下一 folio。
	 * 中途失败时已复制内容不对外发布，调用者只看到 NULL。
	 */
	folio_sz = folio_size(r->folio);
	if (file_off + sz > r->folio_off + folio_sz) {
		u64 part_sz = r->folio_off + folio_sz - file_off, off;

		memcpy(r->buf, r->addr + file_off - r->folio_off, part_sz);
		off = part_sz;

		while (off < sz) {
			/* fetch next folio */
			/*
			 * 以上一 folio 末端定位下一页；large folio 大小可逐轮
			 * 变化。
			 */
			r->err = freader_get_folio(r, r->folio_off + folio_sz);
			if (r->err)
				return NULL;
			folio_sz = folio_size(r->folio);
			part_sz = min_t(u64, sz - off, folio_sz);
			memcpy(r->buf + off, r->addr, part_sz);
			off += part_sz;
		}

		return r->buf;
	}

	/* if data fits in a single folio, just return direct pointer */
	/*
	 * 单 folio 快速路径避免复制；返回值仅在下一次 fetch/cleanup 前有效，
	 * 偏移差把文件字节位置转换为当前映射内地址。
	 */
	return r->addr + (file_off - r->folio_off);
}

/*
 * freader_cleanup() - 结束读取并释放 reader 内部临时资源。
 *
 * r 仍归调用者所有。内存模式从未取得资源，直接返回；文件模式释放
 * 可能缓存的 folio 和 local mapping。函数不释放 r、buf、data 或
 * file，无直接返回值，调用后所有 fetch 返回的借用指针都不得再使用。
 */
void freader_cleanup(struct freader *r)
{
	if (!r->buf)
		return; /* non-file-backed mode */

	freader_put_folio(r);
}

/*
 * Parse build id from the note segment. This logic can be shared between
 * 32-bit and 64-bit system, because Elf32_Nhdr and Elf64_Nhdr are
 * identical.
 */
/*
 * parse_build_id() - 在一个 ELF PT_NOTE 区间中查找 GNU build-id。
 *
 * 上述英文说明强调 32/64 位 note header 布局相同，因此两条 ELF 路径
 * 可以共用本函数。r 是私有读取器；build_id 是至少
 * BUILD_ID_SIZE_MAX 字节的调用者输出区；size 可为 NULL，否则成功时写入
 * 实际描述符长度；note_off/note_size 是文件或内存中的字节区间。
 *
 * 函数逐个验证 note header、4 字节对齐后的 name/desc 边界，再匹配
 * type=GNU build-id、名称 "GNU\0" 和允许的长度。成功复制描述符并把输出
 * 尾部清零，返回 0；读取失败传播 r->err，畸形、越界或未找到均返回
 * -EINVAL。输出只在成功时有效，r 和输入后端的所有权不变。
 */
static int parse_build_id(struct freader *r, unsigned char *build_id, __u32 *size,
			  loff_t note_off, Elf32_Word note_size)
{
	/*
	 * 变量地图：
	 * note_end 是整个 PT_NOTE 的开区间末端；new_off 是验证后的下一 note；
	 * name_sz/desc_sz 来自不可信 header；build_id_off 指向描述符数据；
	 * nhdr/data 都是 fetch 返回的短期借用指针，下一次 fetch 即可能失效。
	 */
	const char note_name[] = "GNU";
	const size_t note_name_sz = sizeof(note_name);
	u32 build_id_off, new_off, note_end, name_sz, desc_sz;
	const Elf32_Nhdr *nhdr;
	const char *data;

	/* 先证明区间末端可表示，后续所有减法和边界比较才有意义。 */
	if (check_add_overflow(note_off, note_size, &note_end))
		return -EINVAL;

	/*
	 * 至少要容纳 header 和完整 "GNU\0" 才值得读取。循环每轮只在完整
	 * 验证 new_off 后推进，恶意零长度字段也会因 header 大小保持前进。
	 */
	while (note_end - note_off > sizeof(Elf32_Nhdr) + note_name_sz) {
		nhdr = freader_fetch(r, note_off, sizeof(Elf32_Nhdr) + note_name_sz);
		if (!nhdr)
			return r->err;

		/*
		 * fetch 视图可能来自仍可被外部修改的页缓存/内存。把长度
		 * 各读一次并保存，避免同一轮校验和地址计算观察到不同值。
		 */
		name_sz = READ_ONCE(nhdr->n_namesz);
		desc_sz = READ_ONCE(nhdr->n_descsz);

		/*
		 * ELF note 的 name 与 descriptor 各自向 4 字节对齐。两次受检
		 * 加法既防整数回绕，也保证整个 note 没有越过 PT_NOTE 末端。
		 */
		new_off = note_off + sizeof(Elf32_Nhdr);
		if (check_add_overflow(new_off, ALIGN(name_sz, 4), &new_off) ||
		    check_add_overflow(new_off, ALIGN(desc_sz, 4), &new_off) ||
		    new_off > note_end)
			break;

		/*
		 * 只有类型、带 NUL 的名称和长度全部满足契约才是目标 note。
		 * 长度上限同时保护固定输出缓冲，并与公开 build-id ABI 对齐。
		 */
		if (nhdr->n_type == BUILD_ID &&
		    name_sz == note_name_sz &&
		    memcmp(nhdr + 1, note_name, note_name_sz) == 0 &&
		    desc_sz > 0 && desc_sz <= BUILD_ID_SIZE_MAX) {
			build_id_off = note_off + sizeof(Elf32_Nhdr) + ALIGN(note_name_sz, 4);

			/* freader_fetch() will invalidate nhdr pointer */
			/*
			 * 下一次 fetch 可能复用临时 buf 或切换 folio，故从此不再
			 * 解引用 nhdr；只使用此前复制到局部变量的 desc_sz。
			 */
			data = freader_fetch(r, build_id_off, desc_sz);
			if (!data)
				return r->err;

			/*
			 * 先复制真实字节，再清零固定输出的剩余部分。
			 * 比较或复制完整数组时不会泄露调用者旧内容。
			 */
			memcpy(build_id, data, desc_sz);
			memset(build_id + desc_sz, 0, BUILD_ID_SIZE_MAX - desc_sz);
			if (size)
				*size = desc_sz;
			return 0;
		}

		note_off = new_off;
	}

	return -EINVAL;
}

/* Parse build ID from 32-bit ELF */
/*
 * get_build_id_32() - 扫描 ELF32 program headers 中的 PT_NOTE。
 *
 * r 已指向通过魔数和 e_type 初筛的 ELF；build_id/size 的输出契约同
 * parse_build_id()。函数读取并快照 e_phnum/e_phoff，把扫描量截断到
 * MAX_PHDR_CNT，逐项尝试 PT_NOTE。找到 build-id 返回 0；读取错误传播
 * r->err，偏移溢出或所有 note 均无匹配返回 -EINVAL。无资源所有权转移。
 */
static int get_build_id_32(struct freader *r, unsigned char *build_id, __u32 *size)
{
	/* phnum 是有效扫描项数，phoff 是字节起点，i 是当前 header 索引。 */
	const Elf32_Ehdr *ehdr;
	const Elf32_Phdr *phdr;
	__u32 phnum, phoff, i;

	ehdr = freader_fetch(r, 0, sizeof(Elf32_Ehdr));
	if (!ehdr)
		return r->err;

	/* subsequent freader_fetch() calls invalidate pointers, so remember locally */
	/*
	 * 后续 fetch 会使 ehdr 失效，故先把两个不可信字段各读一次到局部
	 * 快照；这也避免解析期间重复观察可变化的映射。
	 */
	phnum = READ_ONCE(ehdr->e_phnum);
	phoff = READ_ONCE(ehdr->e_phoff);

	/* set upper bound on amount of segments (phdrs) we iterate */
	/*
	 * 超出防滥用上限时只扫描前 256 项，而不信任文件声明的工作量。
	 */
	if (phnum > MAX_PHDR_CNT)
		phnum = MAX_PHDR_CNT;

	/* check that phoff is not large enough to cause an overflow */
	/* 先证明 program-header 表的末端未回绕，再进入逐项偏移计算。 */
	if (phoff + phnum * sizeof(Elf32_Phdr) < phoff)
		return -EINVAL;

	/*
	 * 每次 fetch 后 phdr 仅在本轮有效。PT_NOTE 解析失败并不终止扫描：
	 * 一个 ELF 可以有多个 note segment，后面的 segment 仍可能含 build-id。
	 */
	for (i = 0; i < phnum; ++i) {
		phdr = freader_fetch(r, phoff + i * sizeof(Elf32_Phdr), sizeof(Elf32_Phdr));
		if (!phdr)
			return r->err;

		if (phdr->p_type == PT_NOTE &&
		    !parse_build_id(r, build_id, size, READ_ONCE(phdr->p_offset),
				    READ_ONCE(phdr->p_filesz)))
			return 0;
	}
	return -EINVAL;
}

/* Parse build ID from 64-bit ELF */
/*
 * get_build_id_64() - 扫描 ELF64 program headers 中的 PT_NOTE。
 *
 * 语义、上下文和 ownership 与 get_build_id_32() 相同；区别是 e_phoff
 * 和 program-header 布局为 64 位。成功返回 0；读取错误传播，未找到或
 * 表末端溢出返回 -EINVAL。size 可空，非空时仅在成功时更新。
 */
static int get_build_id_64(struct freader *r, unsigned char *build_id, __u32 *size)
{
	/* phoff 是 64 位文件字节偏移；phnum/i 分别是扫描上限和当前索引。 */
	const Elf64_Ehdr *ehdr;
	const Elf64_Phdr *phdr;
	__u32 phnum, i;
	__u64 phoff;

	ehdr = freader_fetch(r, 0, sizeof(Elf64_Ehdr));
	if (!ehdr)
		return r->err;

	/* subsequent freader_fetch() calls invalidate pointers, so remember locally */
	/*
	 * 在下一次 fetch 前快照字段，避免继续持有将失效的 ehdr 借用指针。
	 */
	phnum = READ_ONCE(ehdr->e_phnum);
	phoff = READ_ONCE(ehdr->e_phoff);

	/* set upper bound on amount of segments (phdrs) we iterate */
	/* 与 ELF32 路径保持同一抗滥用扫描上限。 */
	if (phnum > MAX_PHDR_CNT)
		phnum = MAX_PHDR_CNT;

	/* check that phoff is not large enough to cause an overflow */
	/*
	 * 64 位加法同样必须显式排除回绕，不能依赖读取器事后拒绝。
	 */
	if (phoff + phnum * sizeof(Elf64_Phdr) < phoff)
		return -EINVAL;

	/*
	 * 逐项读取短期视图，并允许在坏或无关 PT_NOTE 后继续找下一段。
	 */
	for (i = 0; i < phnum; ++i) {
		phdr = freader_fetch(r, phoff + i * sizeof(Elf64_Phdr), sizeof(Elf64_Phdr));
		if (!phdr)
			return r->err;

		if (phdr->p_type == PT_NOTE &&
		    !parse_build_id(r, build_id, size, READ_ONCE(phdr->p_offset),
				    READ_ONCE(phdr->p_filesz)))
			return 0;
	}

	return -EINVAL;
}

/* enough for Elf64_Ehdr, Elf64_Phdr, and all the smaller requests */
/*
 * 64 字节足以容纳本解析器单次最大的固定结构请求（Elf64_Ehdr/Phdr）；
 * note descriptor 最大 20 字节。该缓冲还承接跨 folio 拼接和可睡眠读取。
 */
#define MAX_FREADER_BUF_SZ 64

/*
 * __build_id_parse() - 文件后端 ELF build-id 解析的共同核心。
 *
 * file 是调用者已稳定引用的借用对象；build_id 是至少
 * BUILD_ID_SIZE_MAX 字节的输出；size 可空；may_fault 选择可睡眠读取或
 * 仅页缓存读取。函数先验证最小 ELF 头、魔数和文件类型，再按 EI_CLASS
 * 分派 32/64 位 program-header 扫描。
 *
 * 成功返回 0并填充输出；-EINVAL 表示格式/类型/class 不支持或未找到，
 * 其他负 errno 来自 freader。所有出口都 cleanup reader，不释放 file；
 * 失败时调用者不能使用 build_id/size 的内容。may_fault=true 可能睡眠。
 */
static int __build_id_parse(struct file *file, unsigned char *build_id,
			    __u32 *size, bool may_fault)
{
	/*
	 * buf 是 reader 生命周期内的固定暂存区；ehdr 是下一次 fetch 前有效
	 * 的借用视图；ret 汇总精确错误并穿过统一 cleanup 出口。
	 */
	const Elf32_Ehdr *ehdr;
	struct freader r;
	char buf[MAX_FREADER_BUF_SZ];
	int ret;

	freader_init_from_file(&r, buf, sizeof(buf), file, may_fault);

	/* fetch first 18 bytes of ELF header for checks */
	/*
	 * 这里只取到 e_type 末端，足够判断 magic、class 和对象类型；
	 * 完整 header 留给对应位数解析器读取，避免尚未确定 class 就越读。
	 */
	ehdr = freader_fetch(&r, 0, offsetofend(Elf32_Ehdr, e_type));
	if (!ehdr) {
		ret = r.err;
		goto out;
	}

	ret = -EINVAL;

	/* compare magic x7f "ELF" */
	/* 魔数不匹配说明输入不是 ELF，统一按格式错误返回。 */
	if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0)
		goto out;

	/* only support executable file and shared object file */
	/*
	 * build-id 的本接口面向可执行映像及共享对象；可重定位对象等类型
	 * 不进入 program-header 扫描，即使内部偶然存在 note。
	 */
	if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
		goto out;

	/* EI_CLASS 决定后续字段宽度；未知值保持预设的 -EINVAL。 */
	if (ehdr->e_ident[EI_CLASS] == ELFCLASS32)
		ret = get_build_id_32(&r, build_id, size);
	else if (ehdr->e_ident[EI_CLASS] == ELFCLASS64)
		ret = get_build_id_64(&r, build_id, size);
out:
	/* 无论读取/校验/扫描在哪失败，都撤销可能仍存在的 local mapping。 */
	freader_cleanup(&r);
	return ret;
}

/**
 * build_id_parse_nofault() - Parse build ID of ELF file mapped to vma
 * @vma:      vma object
 * @build_id: buffer to store build id, at least BUILD_ID_SIZE long
 * @size:     returns actual build id size in case of success
 *
 * Assumes no page fault can be taken, so if relevant portions of ELF file are
 * not already paged in, fetching of build ID fails.
 *
 * Return: 0 on success; negative error, otherwise
 */
/*
 * 中文契约：
 * vma 是调用者在 mmap/VMA 锁或等价生命周期保护下借用的映射，函数不
 * 持有它；build_id 为固定输出区；size 可为 NULL，否则成功时输出实际
 * 长度。上述英文强调本入口不能承受 page fault：它只查找已经 Uptodate
 * 的页缓存，适用于 perf mmap 事件等不可阻塞路径。
 *
 * 无 vm_file 返回 -EINVAL；其余结果来自共同解析器。成功不改变 VMA、
 * file 或页缓存内容，失败也不残留 folio 引用。调用期间外层必须保证
 * vma->vm_file 指针有效。
 */
int build_id_parse_nofault(struct vm_area_struct *vma, unsigned char *build_id, __u32 *size)
{
	if (!vma->vm_file)
		return -EINVAL;

	return __build_id_parse(vma->vm_file, build_id, size, false /* !may_fault */);
}

/**
 * build_id_parse() - Parse build ID of ELF file mapped to VMA
 * @vma:      vma object
 * @build_id: buffer to store build id, at least BUILD_ID_SIZE long
 * @size:     returns actual build id size in case of success
 *
 * Assumes faultable context and can cause page faults to bring in file data
 * into page cache.
 *
 * Return: 0 on success; negative error, otherwise
 */
/*
 * 中文契约：
 * 参数与 ownership 同 nofault 入口，但当前上下文必须允许睡眠；读取器
 * 通过 __kernel_read() 可触发文件系统读取，把缺失数据带入页缓存。
 * 无文件映射返回 -EINVAL；成功填充 build_id 和可选 size，其他负 errno
 * 精确表示格式或 I/O 失败。函数不取得长期 file 引用，调用者必须在整个
 * 调用期间稳定 VMA 及 vm_file。
 */
int build_id_parse(struct vm_area_struct *vma, unsigned char *build_id, __u32 *size)
{
	if (!vma->vm_file)
		return -EINVAL;

	return __build_id_parse(vma->vm_file, build_id, size, true /* may_fault */);
}

/**
 * build_id_parse_file() - Parse build ID of ELF file
 * @file:      file object
 * @build_id: buffer to store build id, at least BUILD_ID_SIZE long
 * @size:     returns actual build id size in case of success
 *
 * Assumes faultable context and can cause page faults to bring in file data
 * into page cache.
 *
 * Return: 0 on success; negative error, otherwise
 */
/*
 * 中文契约：
 * file 是调用者持有并在调用期间保持有效的借用引用；build_id 是固定
 * 输出区；size 可空。此入口跳过 VMA 包装，供已经在锁外通过 get_file()
 * 稳定文件的 BPF/proc 等路径使用。它可能睡眠和执行文件 I/O，不消费
 * file 引用；成功/失败类别与 __build_id_parse() 完全相同。
 */
int build_id_parse_file(struct file *file, unsigned char *build_id, __u32 *size)
{
	return __build_id_parse(file, build_id, size, true /* may_fault */);
}

/**
 * build_id_parse_buf - Get build ID from a buffer
 * @buf:      ELF note section(s) to parse
 * @buf_size: Size of @buf in bytes
 * @build_id: Build ID parsed from @buf, at least BUILD_ID_SIZE_MAX long
 *
 * Return: 0 on success, -EINVAL otherwise
 */
/*
 * 中文契约：
 * buf/buf_size 是调用期间有效的连续只读 note 区间，不要求包含 ELF
 * header；build_id 至少 BUILD_ID_SIZE_MAX 字节。本入口用于模块 note
 * 以及链接器导出的内核 note，纯内存读取不睡眠、不取得引用。
 *
 * 成功返回 0 并以零填充输出尾部；畸形或无 GNU build-id 返回 -EINVAL。
 * freader 的内存模式没有 cleanup 资源，保留统一 cleanup 调用是为了让
 * 两种后端遵循同一生命周期结构。
 */
int build_id_parse_buf(const void *buf, unsigned char *build_id, u32 buf_size)
{
	/* r 仅借用 buf；err 保存 parse_build_id() 的最终成功或格式错误。 */
	struct freader r;
	int err;

	freader_init_from_mem(&r, buf, buf_size);

	err = parse_build_id(&r, build_id, NULL, 0, buf_size);

	freader_cleanup(&r);
	return err;
}

#if IS_ENABLED(CONFIG_STACKTRACE_BUILD_ID) || IS_ENABLED(CONFIG_VMCORE_INFO)
/*
 * 运行内核的 build-id 固定存储。init 阶段由 init_vmlinux_build_id()
 * 唯一写入，随后 __ro_after_init 阻止修改；栈回溯和 vmcore 元数据读取。
 * 全零初值也承担“启动 note 未解析成功”的降级表示。
 */
unsigned char vmlinux_build_id[BUILD_ID_SIZE_MAX] __ro_after_init;

/**
 * init_vmlinux_build_id - Compute and stash the running kernel's build ID
 */
/*
 * 中文契约：
 * 启动 init 路径在内核 linker note 边界已经可访问、只读保护尚未最终
 * 收紧时调用。入参：无；返回：无直接返回值。__start_notes/__stop_notes
 * 是链接脚本提供的半开区间，二者差值是字节数。
 *
 * 函数把解析结果写入全局 vmlinux_build_id，不分配资源也不睡眠。
 * 当前接口有意忽略解析错误：失败时静态数组保持全零，消费者据此只能
 * 得到降级标识，而启动不会因缺失 build-id 失败。
 */
void __init init_vmlinux_build_id(void)
{
	/*
	 * 链接器符号本身代表地址边界而非普通对象；取地址相减得到内核
	 * notes 的连续字节长度，整个区间在 init 调用期间保持有效。
	 */
	extern const void __start_notes;
	extern const void __stop_notes;
	unsigned int size = &__stop_notes - &__start_notes;

	build_id_parse_buf(&__start_notes, vmlinux_build_id, size);
}
#endif
