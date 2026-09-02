// SPDX-License-Identifier: GPL-2.0
/*
 * memfd_create system call and file sealing support
 *
 * Code was originally included in shmem.c, and broken out to facilitate
 * use by hugetlbfs as well as tmpfs.
 */
/*
 * 译注：本文件实现 memfd_create 系统调用与文件封印；代码最初位于 shmem.c，拆出后可让
 * tmpfs 与 hugetlbfs 共享同一套匿名文件和 seal 语义。
 */

#include <linux/fs.h>
#include <linux/vfs.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/khugepaged.h>
/* syscall、hugetlb/shmem 与 PID namespace 分别提供 ABI、后端和 noexec 策略。 */
#include <linux/syscalls.h>
#include <linux/hugetlb.h>
#include <linux/shmem_fs.h>
#include <linux/memfd.h>
#include <linux/pid_namespace.h>
#include <uapi/linux/memfd.h>
#include "swap.h"

/*
 * 学习提示：memfd 把匿名 shmem/hugetlb 文件与不可逆 seals 策略组合起来。
 * 写封印发布前必须阻止新 writable 映射并排空既存 GUP pin，之后 seal 只能增加。
 */
/*
 * We need a tag: a new tag would expand every xa_node by 8 bytes,
 * so reuse a tag which we firmly believe is never set or cleared on tmpfs
 * or hugetlbfs because they are memory only filesystems.
 */
/*
 * 译注：需要一个 XArray tag；新增 tag 会让每个 xa_node 膨胀 8 字节，因此复用一个确信不会被
 * 纯内存 tmpfs/hugetlbfs 设置或清除的现有 tag。
 */
#define MEMFD_TAG_PINNED        PAGECACHE_TAG_TOWRITE
/* 扫描退避最多进行 LAST_SCAN+1 轮，最后一轮同时清理临时 tag。 */
#define LAST_SCAN               4       /* about 150ms max */
/* 译注：按当前退避公式，最长等待约 150 毫秒。 */

/*
 * 业务背景：写封印等待逻辑需要以引用计数偏差保守识别可能仍被 GUP/驱动持有的 folio。
 * 入参：folio 是 XArray 锁下借用的真实 folio，不转移引用。
 * 出参/返回：实际引用数不等于页缓存/LRU 等预期引用数时返回 true，否则 false；无副作用。
 * 注意事项：这是启发式候选判断，短暂内部引用会产生假阳性，后续多轮扫描负责消除。
 */
static bool memfd_folio_has_extra_refs(struct folio *folio)
{
	/* 实际引用偏离页缓存/LRU 等预期引用时，保守视为外部 pin。 */
	return folio_ref_count(folio) != folio_expected_ref_count(folio);
}

/*
 * 业务背景：禁止新 writable 引用后，先扫描 mapping 并给疑似旧 pin 建立可增量复查的候选集合。
 * 入参：xas 指向 mapping->i_pages 且初始索引为零，由调用者借用；函数自行取得/释放 xa IRQ 锁。
 * 出参/返回：无返回；给额外引用 folio 设置 MEMFD_TAG_PINNED，不持有 folio 引用或修改页内容。
 * 注意事项：可调度；大 mapping 周期性 pause/unlock/resched，解锁窗口允许 XArray 并发变化。
 */
static void memfd_tag_pins(struct xa_state *xas)
{
	/* 首轮给疑似 pin folio 打临时 XArray mark，供等待循环增量复查。 */
	struct folio *folio;
	int latency = 0;

	/* 排空本 CPU pagevec，减少 LRU 延迟引用造成的假阳性。 */
	lru_add_drain();

	xas_lock_irq(xas);
	/* value entry 不是 folio；只有真实 folio 才比较引用计数。 */
	xas_for_each(xas, folio, ULONG_MAX) {
		if (!xa_is_value(folio) && memfd_folio_has_extra_refs(folio))
			xas_set_mark(xas, MEMFD_TAG_PINNED);

		/* 定期 pause 并解 IRQ 锁，让大 mapping 扫描可调度。 */
		if (++latency < XA_CHECK_SCHED)
			continue;
		latency = 0;

		xas_pause(xas);
		/* pause 保存可恢复位置，解锁期间 mapping 可发生并发变化。 */
		xas_unlock_irq(xas);
		cond_resched();
		xas_lock_irq(xas);
	}
	xas_unlock_irq(xas);
}

/*
 * This is a helper function used by memfd_pin_user_pages() in GUP (gup.c).
 * It is mainly called to allocate a folio in a memfd when the caller
 * (memfd_pin_folios()) cannot find a folio in the page cache at a given
 * index in the mapping.
 */
/*
 * 译注：这是 gup.c 中 memfd_pin_user_pages() 使用的 helper；当 memfd_pin_folios() 在 mapping
 * 的指定索引找不到页缓存 folio 时，主要通过本函数分配并实例化一个。
 */
/*
 * 业务背景：memfd GUP pin 慢路径需要在缺页索引创建 shmem 或 hugetlb folio，再交回 pin 流程。
 * 入参：memfd 是稳定借用文件；idx 是以基础页为单位的页缓存索引，hugetlb 分支内部换算单位。
 * 出参/返回：成功返回页缓存拥有且已解锁的 folio 借用结果；失败返回 ERR_PTR，已得 reservation/folio 回滚。
 * 注意事项：可睡眠；hugetlb 页为长期 DMA pin 排除 HIGHMEM/MOVABLE，并以 fault mutex 串行同索引实例化。
 */
struct folio *memfd_alloc_folio(struct file *memfd, pgoff_t idx)
{
#ifdef CONFIG_HUGETLB_PAGE
	/* hugetlb 与 shmem 分配协议不同，配置开启时先识别文件类型。 */
	struct folio *folio;
	gfp_t gfp_mask;

	if (is_file_hugepages(memfd)) {
		/* 该 folio 将长期 DMA pin，排除 highmem/movable zone。 */
		/*
		 * The folio would most likely be accessed by a DMA driver,
		 * therefore, we have zone memory constraints where we can
		 * alloc from. Also, the folio will be pinned for an indefinite
		 * amount of time, so it is not expected to be migrated away.
		 */
		/*
		 * 译注：该 folio 很可能由 DMA 驱动访问，因此分配 zone 受限；它还会被无限期 pin，
		 * 不应期待迁移路径稍后把它移走。
		 */
		struct inode *inode = file_inode(memfd);
		struct hstate *h = hstate_file(memfd);
		int err = -ENOMEM;
		long nr_resv;

		gfp_mask = htlb_alloc_mask(h);
		gfp_mask &= ~(__GFP_HIGHMEM | __GFP_MOVABLE);
		/* 页缓存索引从基础页单位换算到 huge page 单位。 */
		idx >>= huge_page_order(h);

		/* 先取得一个 hugepage reservation，任何后续失败都需按返回值归还。 */
		nr_resv = hugetlb_reserve_pages(inode, idx, idx + 1, NULL, EMPTY_VMA_FLAGS);
		if (nr_resv < 0)
			return ERR_PTR(nr_resv);

		/* reserve 分配绑定当前 NUMA 节点，不依赖 VMA/mempolicy。 */
		folio = alloc_hugetlb_folio_reserve(h,
						    numa_node_id(),
						    NULL,
						    gfp_mask);
		if (folio) {
			u32 hash;

			/*
			 * Zero the folio to prevent information leaks to userspace.
			 * Use folio_zero_user() which is optimized for huge/gigantic
			 * pages. Pass 0 as addr_hint since this is not a faulting path
			 *  and we don't have a user virtual address yet.
			 */
			/*
			 * 译注：为防止向用户泄露旧内容，必须清零 folio；folio_zero_user() 针对 huge/gigantic
			 * 页优化。这里不是 fault 路径且尚无用户虚拟地址，所以 addr_hint 传 0。
			 */
			/* 发布到页缓存前清零，防止旧物理内容泄露给用户。 */
			folio_zero_user(folio, 0);

			/*
			 * Mark the folio uptodate before adding to page cache,
			 * as required by filemap.c and other hugetlb paths.
			 */
			/* 译注：按 filemap.c 和其他 hugetlb 路径的契约，加入页缓存前必须先标记 uptodate。 */
			/* uptodate 必须先于页缓存可见性发布。 */
			__folio_mark_uptodate(folio);

			/*
			 * Serialize hugepage allocation and instantiation to prevent
			 * races with concurrent allocations, as required by all other
			 * callers of hugetlb_add_to_page_cache().
			 */
			/*
			 * 译注：必须串行 hugepage 分配与实例化，避免并发分配竞态；所有其他
			 * hugetlb_add_to_page_cache() 调用者也遵循该要求。
			 */
			/* 哈希 fault mutex 串行同一 mapping/index 的实例化竞争。 */
			hash = hugetlb_fault_mutex_hash(memfd->f_mapping, idx);
			mutex_lock(&hugetlb_fault_mutex_table[hash]);

			/* add 成功转移页缓存引用，失败仍由本路径 folio_put。 */
			err = hugetlb_add_to_page_cache(folio,
							memfd->f_mapping,
							idx);

			mutex_unlock(&hugetlb_fault_mutex_table[hash]);

			/* 并发已实例化等错误需释放新 folio 并撤销预留。 */
			if (err) {
				folio_put(folio);
				goto err_unresv;
			}

			/* 成功后绑定 subpool 计账并解锁，返回页缓存持有的 folio。 */
			hugetlb_set_folio_subpool(folio, subpool_inode(inode));
			folio_unlock(folio);
			return folio;
		}
err_unresv:
		/* nr_resv>0 表示本调用新增了预留，才允许 unreserve。 */
		if (nr_resv > 0)
			hugetlb_unreserve_pages(inode, idx, idx + 1, 0);
		return ERR_PTR(err);
	}
#endif
	/* 普通 memfd 委托 shmem 查找或实例化 idx folio。 */
	return shmem_read_folio(memfd->f_mapping, idx);
}

/*
 * Setting SEAL_WRITE requires us to verify there's no pending writer. However,
 * via get_user_pages(), drivers might have some pending I/O without any active
 * user-space mappings (eg., direct-IO, AIO). Therefore, we look at all folios
 * and see whether it has an elevated ref-count. If so, we tag them and wait for
 * them to be dropped.
 * The caller must guarantee that no new user will acquire writable references
 * to those folios to avoid races.
 */
/*
 * 译注：设置 SEAL_WRITE 前必须确认没有待完成写者；驱动可能通过 GUP 保留 direct-I/O/AIO，
 * 即使已无用户映射也仍在 I/O。这里扫描所有 folio，以升高的引用计数标记候选并等待引用下降；
 * 调用者必须先保证不会再取得新的 writable 引用，否则扫描会与新 pin 竞争。
 */
/*
 * 业务背景：WRITE seal 的不可逆发布必须位于旧 GUP pin 全部排空之后，避免设备在封印后继续写文件。
 * 入参：mapping 是 inode 生命周期内借用的页缓存；调用者已经用 mapping_deny_writable 阻止新写映射。
 * 出参/返回：全部候选消退返回 0；最终仍有 pin 返回 -EBUSY；信号只迫使提前进入末轮，所有临时 tag 均清除。
 * 注意事项：可睡眠且多轮指数退避；不取得 folio 引用，XArray IRQ 锁只覆盖单段扫描。
 */
static int memfd_wait_for_pins(struct address_space *mapping)
{
	/* 调用者已阻止新 writable 引用，本函数只等待旧额外引用消退。 */
	XA_STATE(xas, &mapping->i_pages, 0);
	struct folio *folio;
	int error, scan;

	/* tag 是候选集合快照，后续轮次只遍历仍标记项。 */
	memfd_tag_pins(&xas);

	error = 0;
	/* 退避轮次逐步延长等待，最后一轮必须清除所有临时 mark。 */
	for (scan = 0; scan <= LAST_SCAN; scan++) {
		int latency = 0;

		/* mark 全消失即可提前成功，不再触发全局 LRU drain。 */
		if (!xas_marked(&xas, MEMFD_TAG_PINNED))
			break;

		/* 首轮排空所有 CPU pagevec，后续按指数时间可杀等待。 */
		if (!scan)
			lru_add_drain_all();
		else if (schedule_timeout_killable((HZ << scan) / 200))
			scan = LAST_SCAN;

		/* 每轮从 mapping 起点复查 marked folio。 */
		xas_set(&xas, 0);
		xas_lock_irq(&xas);
		xas_for_each_marked(&xas, folio, ULONG_MAX, MEMFD_TAG_PINNED) {
			bool clear = true;

			/* 仍有额外引用时保留 mark，末轮则记录 EBUSY 并清理。 */
			if (!xa_is_value(folio) &&
			    memfd_folio_has_extra_refs(folio)) {
				/*
				 * On the last scan, we clean up all those tags
				 * we inserted; but make a note that we still
				 * found folios pinned.
				 */
				/* 译注：最后一轮要清除本函数插入的全部 tag，同时记住仍发现被 pin 的 folio。 */
				if (scan == LAST_SCAN)
					error = -EBUSY;
				else
					clear = false;
			}
			/* 引用已正常或已到最后一轮时移除临时标记。 */
			if (clear)
				xas_clear_mark(&xas, MEMFD_TAG_PINNED);

			/* 与初次标记相同，定期让出 CPU 并安全恢复游标。 */
			if (++latency < XA_CHECK_SCHED)
				continue;
			latency = 0;

			xas_pause(&xas);
			/* 先暂停游标再解锁，恢复后不会重复或跳过已处理 slot。 */
			xas_unlock_irq(&xas);
			cond_resched();
			xas_lock_irq(&xas);
		}
		xas_unlock_irq(&xas);
	}

	return error;
}

/*
 * 业务背景：共享 seal 逻辑需把 shmem/hugetlbfs 的不同 inode 私有布局抽象成统一位图地址。
 * 入参：file 是稳定借用文件，不要求 memfd 来源，函数不取得 inode/file 引用。
 * 出参/返回：支持时返回 inode 内 seals 字段借用指针，否则 NULL；无状态变化。
 * 注意事项：返回指针生命周期依附 inode；修改者必须另持 inode_lock，纯查询接受当前快照语义。
 */
static unsigned int *memfd_file_seals_ptr(struct file *file)
{
	/* shmem 与 hugetlbfs 将 seals 存在各自 inode 私有结构。 */
	if (shmem_file(file))
		return &SHMEM_I(file_inode(file))->seals;

#ifdef CONFIG_HUGETLBFS
	if (is_file_hugepages(file))
		return &HUGETLBFS_I(file_inode(file))->seals;
#endif

	/* 其他文件系统不支持 memfd seal 操作。 */
	return NULL;
}

#define F_ALL_SEALS (F_SEAL_SEAL | \
		     F_SEAL_EXEC | \
		     F_SEAL_SHRINK | \
		     F_SEAL_GROW | \
		     F_SEAL_WRITE | \
		     F_SEAL_FUTURE_WRITE)
/* F_ALL_SEALS 是 fcntl 输入白名单，未知位必须拒绝。 */
/*
 * 各位用途：SEAL_SEAL 终止后续加 seal；EXEC 冻结执行属性；SHRINK/GROW 冻结大小方向；
 * WRITE 排斥既有 writable/pin 后禁止全部写；FUTURE_WRITE 只阻止未来共享写映射和写入入口。
 */

/*
 * 业务背景：fcntl(F_ADD_SEALS) 将不可信共享方的限制不可逆地并入 inode，WRITE seal 还需排空写引用。
 * 入参：file 是借用且须以 FMODE_WRITE 打开；seals 是 F_ALL_SEALS 白名单内待增加位图。
 * 出参/返回：成功 0；权限/未知位/封印终止/后端错误或活动 pin 返回负 errno；成功只增加位。
 * 注意事项：可睡眠并持 inode_lock；WRITE 路径 deny writable 后失败会 allow 回滚，最终 OR 是发布点。
 */
int memfd_add_seals(struct file *file, unsigned int seals)
{
	/* inode 锁串行 seal 集合、writable deny 和 pin 排空。 */
	struct inode *inode = file_inode(file);
	unsigned int *file_seals;
	int error;

	/*
	 * SEALING
	 * Sealing allows multiple parties to share a tmpfs or hugetlbfs file
	 * but restrict access to a specific subset of file operations. Seals
	 * can only be added, but never removed. This way, mutually untrusted
	 * parties can share common memory regions with a well-defined policy.
	 * A malicious peer can thus never perform unwanted operations on a
	 * shared object.
	 *
	 * Seals are only supported on special tmpfs or hugetlbfs files and
	 * always affect the whole underlying inode. Once a seal is set, it
	 * may prevent some kinds of access to the file. Currently, the
	 * following seals are defined:
	 *   SEAL_SEAL: Prevent further seals from being set on this file
	 *   SEAL_SHRINK: Prevent the file from shrinking
	 *   SEAL_GROW: Prevent the file from growing
	 *   SEAL_WRITE: Prevent write access to the file
	 *   SEAL_EXEC: Prevent modification of the exec bits in the file mode
	 *
	 * As we don't require any trust relationship between two parties, we
	 * must prevent seals from being removed. Therefore, sealing a file
	 * only adds a given set of seals to the file, it never touches
	 * existing seals. Furthermore, the "setting seals"-operation can be
	 * sealed itself, which basically prevents any further seal from being
	 * added.
	 *
	 * Semantics of sealing are only defined on volatile files. Only
	 * anonymous tmpfs and hugetlbfs files support sealing. More
	 * importantly, seals are never written to disk. Therefore, there's
	 * no plan to support it on other file types.
	 */
	/*
	 * 译注：seal 让互不信任的参与方共享 tmpfs/hugetlbfs 文件，同时把允许操作限制为明确子集。
	 * seal 只能增加、不能删除，并作用于整个 inode：SEAL_SEAL 禁止再加 seal，SEAL_SHRINK/GROW
	 * 禁止缩小/增大，SEAL_WRITE 禁止写，SEAL_EXEC 禁止修改 mode 执行位。seal 本身还能封印
	 * “继续加 seal”这一动作。该语义仅定义于匿名易失的 tmpfs/hugetlbfs，seal 不写入磁盘，
	 * 因而没有扩展到普通持久文件的计划。
	 */

	/* 添加策略是修改 inode 状态，文件描述符必须具备写模式。 */
	if (!(file->f_mode & FMODE_WRITE))
		return -EPERM;
	if (seals & ~(unsigned int)F_ALL_SEALS)
		return -EINVAL;

	/* 参数验证后再加 inode 锁，所有错误统一在 unlock 收口。 */
	inode_lock(inode);

	file_seals = memfd_file_seals_ptr(file);
	/* 非 shmem/hugetlbfs 文件没有 seals 存储，返回 EINVAL。 */
	if (!file_seals) {
		error = -EINVAL;
		goto unlock;
	}

	/* F_SEAL_SEAL 是不可逆终止位，阻止任何后续新增。 */
	if (*file_seals & F_SEAL_SEAL) {
		error = -EPERM;
		goto unlock;
	}

	/*
	 * SEAL_EXEC implies SEAL_WRITE, making W^X from the start.
	 */
	/* 译注：SEAL_EXEC 蕴含 SEAL_WRITE，使对象从创建起就满足写与执行互斥。 */
	/* 可执行 inode 的 EXEC seal 同时冻结大小与全部未来写能力。 */
	if (seals & F_SEAL_EXEC && inode->i_mode & 0111)
		seals |= F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE|F_SEAL_FUTURE_WRITE;

	/* 首次 WRITE seal 才执行昂贵的 writable deny 与 pin 等待。 */
	if ((seals & F_SEAL_WRITE) && !(*file_seals & F_SEAL_WRITE)) {
		error = mapping_deny_writable(file->f_mapping);
		if (error)
			goto unlock;

		/* deny 成功阻止新 writable mapping，随后旧 pin 集合只会收缩。 */
		error = memfd_wait_for_pins(file->f_mapping);
		if (error) {
			/* pin 未排空时撤销 deny，seal 集合保持未修改。 */
			mapping_allow_writable(file->f_mapping);
			goto unlock;
		}
	}

	/* 最终发布只做 OR，保证 seals 永不被移除。 */
	*file_seals |= seals;
	error = 0;

unlock:
	inode_unlock(inode);
	return error;
}

/*
 * 业务背景：fcntl(F_GET_SEALS) 读取 shmem/hugetlbfs inode 当前不可逆限制的位图快照。
 * 入参：file 是借用文件，不要求写权限且不取得引用。
 * 出参/返回：支持时返回非负 seal 位图，不支持的文件返回 -EINVAL；无副作用和 ownership 变化。
 * 注意事项：不持 inode_lock，读取用于查询快照；seal 只会 OR，故并发更新不会使限制倒退。
 */
int memfd_get_seals(struct file *file)
{
	/* 读取返回 inode 当前 seal 位图，不支持的文件返回 EINVAL。 */
	unsigned int *seals = memfd_file_seals_ptr(file);

	return seals ? *seals : -EINVAL;
}

/*
 * 业务背景：VFS fcntl 路径把 memfd 的两个 seal 命令汇合到增加和查询实现。
 * 入参：file 是借用文件；cmd 为 F_ADD_SEALS 或 F_GET_SEALS；arg 仅 ADD 时是待加位图。
 * 出参/返回：原样返回 add/get 结果，未知命令返回 -EINVAL；不消费 file 引用。
 * 注意事项：ADD 可睡眠并改变 inode，GET 只读；每个 case 结束后统一返回 error。
 */
long memfd_fcntl(struct file *file, unsigned int cmd, unsigned int arg)
{
	/* memfd fcntl 只接受增加和查询 seal 两种命令。 */
	long error;

	switch (cmd) {
	case F_ADD_SEALS:
		/* 进入条件：调用者请求新增限制；arg 作为位图进入不可逆更新，结果随后统一返回。 */
		error = memfd_add_seals(file, arg);
		break;
	case F_GET_SEALS:
		/* 进入条件：调用者查询限制；忽略 arg、读取 inode 位图，结果随后统一返回。 */
		error = memfd_get_seals(file);
		break;
	default:
		/* 进入条件：非 seal fcntl 误入；不改状态，以 EINVAL 返回 VFS。 */
		error = -EINVAL;
		break;
	}

	return error;
}

#define MFD_NAME_PREFIX "memfd:"
/* 前缀计入 VFS 名称上限，用户部分使用剩余长度。 */
#define MFD_NAME_PREFIX_LEN (sizeof(MFD_NAME_PREFIX) - 1)
#define MFD_NAME_MAX_LEN (NAME_MAX - MFD_NAME_PREFIX_LEN)

#define MFD_ALL_FLAGS (MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_HUGETLB | MFD_NOEXEC_SEAL | MFD_EXEC)
/*
 * MFD_ALL_FLAGS 是非 huge-size 编码白名单：CLOEXEC 控制 fd，ALLOW_SEALING 移除默认终止位，
 * HUGETLB 选择后端，NOEXEC_SEAL/EXEC 是互斥的执行策略。
 */

/*
 * 业务背景：memfd_create 在解析用户 flags 时还要应用当前 PID namespace 的 noexec 默认/强制策略。
 * 入参：flags 是输入输出位图指针；入口可尚未选择 EXEC/NOEXEC_SEAL，ownership 属于 syscall 栈。
 * 出参/返回：允许时返回 0 并补入默认策略；强制 noexec 被违反时返回 -EACCES；无 SYSCTL 时恒成功。
 * 注意事项：只追加执行策略位；强制拒绝会限速打印，namespace 生命周期由 current 任务稳定。
 */
static int check_sysctl_memfd_noexec(unsigned int *flags)
{
#ifdef CONFIG_SYSCTL
	/* 策略取自当前活动 PID namespace，使容器可独立收紧。 */
	struct pid_namespace *ns = task_active_pid_ns(current);
	int sysctl = pidns_memfd_noexec_scope(ns);

	/* 用户未表态时由 sysctl 补入 EXEC 或 NOEXEC_SEAL 默认值。 */
	if (!(*flags & (MFD_EXEC | MFD_NOEXEC_SEAL))) {
		if (sysctl >= MEMFD_NOEXEC_SCOPE_NOEXEC_SEAL)
			*flags |= MFD_NOEXEC_SEAL;
		else
			*flags |= MFD_EXEC;
	}
	/* 显式或默认补位后，flags 必有且仅有一种执行策略。 */

	/* 强制级别拒绝任何未请求 NOEXEC_SEAL 的创建。 */
	if (!(*flags & MFD_NOEXEC_SEAL) && sysctl >= MEMFD_NOEXEC_SCOPE_NOEXEC_ENFORCED) {
		pr_err_ratelimited(
			"%s[%d]: memfd_create() requires MFD_NOEXEC_SEAL with vm.memfd_noexec=%d\n",
			current->comm, task_pid_nr(current), sysctl);
		/* EACCES 区分命名空间策略拒绝与 flags 格式错误。 */
		return -EACCES;
	}
#endif
	return 0;
}

/*
 * 业务背景：mmap 检查只关心会限制共享写能力的两个 seal 位，集中判断可简化调用路径。
 * 入参：seals 是 inode seal 位图快照，纯值输入。
 * 出参/返回：WRITE 或 FUTURE_WRITE 任一存在返回 true，否则 false；无副作用。
 * 注意事项：不判断 EXEC/大小 seal，也不提供同步；调用者负责取得合适快照。
 */
static inline bool is_write_sealed(unsigned int seals)
{
	/* WRITE 与 FUTURE_WRITE 都限制新的共享可写映射。 */
	return seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE);
}

/*
 * 业务背景：写类 seal 必须拒绝新 MAP_SHARED|PROT_WRITE，并封堵只读共享映射未来用 mprotect 升级。
 * 入参：vm_flags_ptr 是 mmap 构造中的输入输出 flags；调用者已确认写 seal 存在。
 * 出参/返回：共享写返回 -EPERM；共享只读成功并清 VM_MAYWRITE；私有映射成功且不修改。
 * 注意事项：私有 COW 不写回文件；清 MAYWRITE 是对未来状态的不可升级约束。
 */
static int check_write_seal(vm_flags_t *vm_flags_ptr)
{
	/* 私有 COW 写不修改文件；只有 shared 映射受 seal 约束。 */
	vm_flags_t vm_flags = *vm_flags_ptr;
	vm_flags_t mask = vm_flags & (VM_SHARED | VM_WRITE);

	/* If a private mapping then writability is irrelevant. */
	/* 译注：若为私有映射，可写性与文件 write seal 无关。 */
	if (!(mask & VM_SHARED))
		return 0;

	/*
	 * New PROT_WRITE and MAP_SHARED mmaps are not allowed when
	 * write seals are active.
	 */
	/* 译注：写类 seal 生效时，不允许新建 PROT_WRITE 且 MAP_SHARED 的映射。 */
	/* 当前就请求共享写直接拒绝。 */
	if (mask & VM_WRITE)
		return -EPERM;

	/*
	 * This is a read-only mapping, disallow mprotect() from making a
	 * write-sealed mapping writable in future.
	 */
	/* 译注：当前虽为只读映射，也要禁止以后用 mprotect() 把受 write seal 保护的映射改成可写。 */
	/* 只读共享映射清 MAYWRITE，封堵未来 mprotect 升级。 */
	*vm_flags_ptr &= ~VM_MAYWRITE;

	return 0;
}

/*
 * 业务背景：shmem/hugetlbfs mmap 建立 VMA 前调用本入口，把 inode write seals 落到本次 vm_flags。
 * 入参：file 是借用映射文件；vm_flags_ptr 是待提交 VMA flags 的输入输出指针。
 * 出参/返回：允许返回 0，可能清 VM_MAYWRITE；禁止共享写返回 -EPERM；不支持 seals 的文件视为无 seal。
 * 注意事项：必须在 VMA 发布前调用；只改变尚未发布的 flags，不取得 file/inode 引用。
 */
int memfd_check_seals_mmap(struct file *file, vm_flags_t *vm_flags_ptr)
{
	/* 不支持 seals 的文件按无 seal 处理，保持通用 mmap 路径兼容。 */
	int err = 0;
	unsigned int *seals_ptr = memfd_file_seals_ptr(file);
	unsigned int seals = seals_ptr ? *seals_ptr : 0;

	/* 仅存在写类 seal 时才需要改写或拒绝 VMA flags。 */
	if (is_write_sealed(seals))
		err = check_write_seal(vm_flags_ptr);

	return err;
}

/*
 * 业务背景：syscall 在分配名称/文件前验证 MFD 位组合、huge-size 编码及 namespace 执行策略。
 * 入参：flags_ptr 是用户 flags 的内核栈输入输出副本，HUGETLB 时可带 huge size 编码。
 * 出参/返回：合法返回 0且可能补 EXEC/NOEXEC_SEAL；未知位/互斥位返回 -EINVAL，策略拒绝 -EACCES。
 * 注意事项：验证失败尚无资源；非 HUGETLB 绝不接受 huge-size 位。
 */
static int sanitize_flags(unsigned int *flags_ptr)
{
	/* hugetlb 模式额外允许 huge size 编码，其余模式只接受公共位。 */
	unsigned int flags = *flags_ptr;

	if (!(flags & MFD_HUGETLB)) {
		if (flags & ~MFD_ALL_FLAGS)
			return -EINVAL;
	} else {
		/* Allow huge page size encoding in flags. */
		/* 译注：HUGETLB 模式允许 flags 携带 huge page 大小编码。 */
		if (flags & ~(MFD_ALL_FLAGS |
				(MFD_HUGE_MASK << MFD_HUGE_SHIFT)))
			return -EINVAL;
	}

	/* Invalid if both EXEC and NOEXEC_SEAL are set.*/
	/* 译注：同时设置 EXEC 与 NOEXEC_SEAL 属于无效的互斥策略组合。 */
	/* EXEC 与 NOEXEC_SEAL 互斥，不能同时声明。 */
	if ((flags & MFD_EXEC) && (flags & MFD_NOEXEC_SEAL))
		return -EINVAL;

	return check_sysctl_memfd_noexec(flags_ptr);
}

/*
 * 业务背景：memfd 的 dentry 诊断名需要安全复制用户字符串，并加固定前缀且满足 NAME_MAX。
 * 入参：uname 是 NUL 结尾的用户地址借用指针，可在复制时 fault。
 * 出参/返回：成功返回调用者拥有的 kmalloc 名称；失败返回 ERR_PTR(-ENOMEM/-EFAULT/-EINVAL)。
 * 注意事项：可睡眠；成功缓冲区由 syscall cleanup 自动释放，超长输入不会被静默截断。
 */
static char *alloc_name(const char __user *uname)
{
	/* 内核名称由固定前缀加受限用户字符串组成，最大不超过 NAME_MAX。 */
	int error;
	char *name;
	long len;

	/* 多一个字节容纳终止 NUL。 */
	name = kmalloc(NAME_MAX + 1, GFP_KERNEL);
	if (!name)
		return ERR_PTR(-ENOMEM);

	memcpy(name, MFD_NAME_PREFIX, MFD_NAME_PREFIX_LEN);
	/* returned length does not include terminating zero */
	/* 译注：strncpy_from_user() 成功返回的长度不包含末尾 NUL。 */
	/* 上限加一用于区分恰好最大长度与缺少终止符的超长输入。 */
	len = strncpy_from_user(&name[MFD_NAME_PREFIX_LEN], uname, MFD_NAME_MAX_LEN + 1);
	if (len < 0) {
		error = -EFAULT;
		goto err_name;
	} else if (len > MFD_NAME_MAX_LEN) {
		error = -EINVAL;
		goto err_name;
	}

	return name;

err_name:
	/* 错误指针携带 EFAULT/EINVAL，调用者无需另行释放。 */
	kfree(name);
	return ERR_PTR(error);
}

/*
 * 业务背景：系统调用和内核调用者用已验证名称/flags 创建具体 shmem 或 hugetlbfs 匿名 file。
 * 入参：name 是调用期间借用的内核字符串；flags 已经 sanitize，决定后端、huge size 与初始 seal。
 * 出参/返回：成功返回调用者拥有一个 file 引用；失败返回 ERR_PTR，已创建 file 在 LSM 失败时 fput。
 * 注意事项：可睡眠；devless 匿名 inode 的安全标签先初始化，NOEXEC seal 在返回前发布为 inode 状态。
 */
struct file *memfd_alloc_file(const char *name, unsigned int flags)
{
	/* 根据 HUGETLB 位选择匿名后端，均以零初始大小和 NORESERVE 创建。 */
	unsigned int *file_seals;
	struct file *file;
	struct inode *inode;
	int err = 0;

	if (flags & MFD_HUGETLB) {
		/* huge size 编码从 flags 提取并交由 hugetlbfs 选择 hstate。 */
		file = hugetlb_file_setup(name, 0, mk_vma_flags(VMA_NORESERVE_BIT),
					HUGETLB_ANONHUGE_INODE,
					(flags >> MFD_HUGE_SHIFT) &
					MFD_HUGE_MASK);
	} else {
		/* 普通模式创建 shmem 匿名文件。 */
		file = shmem_file_setup(name, 0, mk_vma_flags(VMA_NORESERVE_BIT));
	}
	/* 后端错误指针原样传播，不产生 inode 所有权。 */
	if (IS_ERR(file))
		return file;

	inode = file_inode(file);
	/* LSM 匿名 inode 初始化失败必须 fput 新文件。 */
	err = security_inode_init_security_anon(inode,
			&QSTR(MEMFD_ANON_NAME), NULL);
	if (err) {
		fput(file);
		file = ERR_PTR(err);
		return file;
	}

	/* memfd 支持 seek 与位置无关 I/O，并始终使用 largefile。 */
	file->f_mode |= FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE;
	file->f_flags |= O_LARGEFILE;

	/* NOEXEC_SEAL 同时清执行位并预装不可移除的 EXEC seal。 */
	if (flags & MFD_NOEXEC_SEAL) {
		inode->i_mode &= ~0111;
		file_seals = memfd_file_seals_ptr(file);
		if (file_seals) {
			*file_seals &= ~F_SEAL_SEAL;
			*file_seals |= F_SEAL_EXEC;
		}
	} else if (flags & MFD_ALLOW_SEALING) {
		/* 允许 sealing 的可执行 memfd 只移除默认 F_SEAL_SEAL。 */
		/* MFD_EXEC and MFD_ALLOW_SEALING are set */
		/* 译注：此分支对应同时设置 MFD_EXEC 与 MFD_ALLOW_SEALING。 */
		file_seals = memfd_file_seals_ptr(file);
		if (file_seals)
			*file_seals &= ~F_SEAL_SEAL;
	}

	return file;
}

/*
 * 业务背景：用户通过 memfd_create 创建无路径匿名内存文件，并以新 fd 接收其唯一初始引用。
 * 入参：uname 是用户诊断名；flags 控制 CLOEXEC、sealing、hugetlb 和执行策略。
 * 出参/返回：成功返回新 fd；失败返回 sanitize/name/backend/fd 分配的负 errno，所有临时资源自动回收。
 * 注意事项：进程上下文可睡眠；FD_ADD 成功是 file 引用向 fdtable 的发布/ownership 转移点。
 */
SYSCALL_DEFINE2(memfd_create,
		const char __user *, uname,
		unsigned int, flags)
{
	/* cleanup 属性让所有 fd 分配前的返回路径自动释放 name。 */
	char *name __free(kfree) = NULL;
	unsigned int fd_flags;
	int error;

	/* sanitize 还可能依据 PID namespace sysctl 补写默认执行策略。 */
	error = sanitize_flags(&flags);
	if (error < 0)
		return error;

	name = alloc_name(uname);
	if (IS_ERR(name))
		return PTR_ERR(name);

	/* FD_ADD 只在 file 创建成功时安装描述符，并接管 file 引用。 */
	fd_flags = (flags & MFD_CLOEXEC) ? O_CLOEXEC : 0;
	return FD_ADD(fd_flags, memfd_alloc_file(name, flags));
}
