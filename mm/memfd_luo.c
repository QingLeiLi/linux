// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Pratyush Yadav <ptyadav@amazon.de>
 */

/**
 * DOC: Memfd Preservation via LUO
 *
 * Overview
 * ========
 *
 * Memory file descriptors (memfd) can be preserved over a kexec using the Live
 * Update Orchestrator (LUO) file preservation. This allows userspace to
 * transfer its memory contents to the next kernel after a kexec.
 *
 * The preservation is not intended to be transparent. Only select properties of
 * the file are preserved. All others are reset to default. The preserved
 * properties are described below.
 *
 * .. note::
 *    The LUO API is not stabilized yet, so the preserved properties of a memfd
 *    are also not stable and are subject to backwards incompatible changes.
 *
 * .. note::
 *    Currently a memfd backed by Hugetlb is not supported. Memfds created
 *    with ``MFD_HUGETLB`` will be rejected.
 *
 * Preserved Properties
 * ====================
 *
 * The following properties of the memfd are preserved across kexec:
 *
 * File Contents
 *   All data stored in the file is preserved.
 *
 * File Size
 *   The size of the file is preserved. Holes in the file are filled by
 *   allocating pages for them during preservation.
 *
 * File Position
 *   The current file position is preserved, allowing applications to continue
 *   reading/writing from their last position.
 *
 * File Status Flags
 *   memfds are always opened with ``O_RDWR`` and ``O_LARGEFILE``. This property
 *   is maintained.
 *
 * Seals
 *   File seals set on the memfd are preserved and re-applied on restore.
 *   Only seals known to this LUO version (see ``MEMFD_LUO_ALL_SEALS``) may
 *   be present; preservation fails with ``-EOPNOTSUPP`` otherwise.
 *
 * Non-Preserved Properties
 * ========================
 *
 * All properties which are not preserved must be assumed to be reset to
 * default. This section describes some of those properties which may be more of
 * note.
 *
 * ``FD_CLOEXEC`` flag
 *   A memfd can be created with the ``MFD_CLOEXEC`` flag that sets the
 *   ``FD_CLOEXEC`` on the file. This flag is not preserved and must be set
 *   again after restore via ``fcntl()``.
 */
/*
 * LUO 让普通 shmem memfd 的内容跨 kexec 交给下一内核，但这不是透明迁移：只
 * 保留文件数据、大小、当前 f_pos、固定的 O_RDWR|O_LARGEFILE 状态和当前版本
 * 认识的 seals；FD_CLOEXEC 等 fd/文件属性恢复默认，用户态须重新设置。洞会在
 * preserve 时实际分配并归零。Hugetlb memfd 当前拒绝。
 *
 * ABI 尚未稳定；memfd_luo_ser/memfd_luo_folio_ser 的 packed 布局及 compatible
 * 版本字符串共同定义跨内核契约。新增 seal 或更改布局都必须同步提升版本，
 * 否则新旧内核会以相同 compatible 误解序列化数据。
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

/* 错误指针、file/inode 与物理地址转换支撑序列对象和页的 ownership 交接。 */
#include <linux/bits.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/io.h>
/* KHO ABI/实现与 LUO 回调协议共同定义跨内核句柄和阶段状态机。 */
#include <linux/kexec_handover.h>
#include <linux/kho/abi/memfd.h>
#include <linux/liveupdate.h>
/* shmem/memfd 提供冻结、seal、pin 与 page-cache 重建，vmalloc 承载页描述数组。 */
#include <linux/shmem_fs.h>
#include <linux/vmalloc.h>
#include <linux/memfd.h>
#include <uapi/linux/memfd.h>

#include "internal.h"

/*
 * 业务背景：prepare/preserve 阶段把 memfd 的全部逻辑内容页 pin 并登记进 KHO，
 * 同时构造跨内核的 PFN/index/flags 数组；由 memfd_luo_preserve() 持 inode 锁调用。
 * 入参：file 是冻结的普通 shmem memfd 借用；kho_vmalloc 是主序列结构内的输出
 * 描述符；out_folios_ser 输出当前内核 vmap 指针；nr_foliosp 输出记录数。
 * 出参/返回：成功 0，输出数组/数量且所有 folio 仍 pinned+preserved；零长度时
 * 输出 0/NULL。失败负 errno，并逆序撤销已 preserve 页、unpin 全部页、释放数组。
 * 注意事项：可睡眠且要求 inode 已锁、shmem 已 freeze；洞会被实例化，所有页
 * 强制 dirty+uptodate 以防下一内核 reclaim 丢数据，成功 ownership 交给 LUO。
 */
static int memfd_luo_preserve_folios(struct file *file,
				     struct kho_vmalloc *kho_vmalloc,
				     struct memfd_luo_folio_ser **out_folios_ser,
				     u64 *nr_foliosp)
{
	/*
	 * inode/size 描述文件快照；folios 是临时 pin 指针数组；folios_ser 是保留的
	 * ABI 数组；max/nr_pinned/nr_folios/offset/i 管理容量、返回数量与回滚水位；
	 * err 始终保存对外 errno。
	 */
	struct inode *inode = file_inode(file);
	struct memfd_luo_folio_ser *folios_ser;
	unsigned int max_folios;
	long i, size, nr_pinned;
	struct folio **folios;
	int err = -EINVAL;
	pgoff_t offset;
	u64 nr_folios;

	/* freeze+inode lock 稳定 size，零长度无需创建任何 KHO 描述。 */
	size = i_size_read(inode);
	/*
	 * If the file has zero size, then the folios and nr_folios properties
	 * are not set.
	 */
	/* 文件大小为零时 folios 描述和数量明确清空，调用者可跳过 unpreserve。 */
	if (!size) {
		*nr_foliosp = 0;
		*out_folios_ser = NULL;
		return 0;
	}

	/*
	 * Guess the number of folios based on inode size. Real number might end
	 * up being smaller if there are higher order folios.
	 */
	/*
	 * 以基础页数作为指针数组上限；若 shmem 存在高阶 folio，实际记录更少。
	 * PAGE_ALIGN 覆盖最后一个部分页，前置大小上限保证 unsigned int 不截断。
	 */
	max_folios = PAGE_ALIGN(size) / PAGE_SIZE;
	folios = kvmalloc_objs(*folios, max_folios);
	if (!folios)
		return -ENOMEM;

	/*
	 * Pin the folios so they don't move around behind our back. This also
	 * ensures none of the folios are in CMA -- which ensures they don't
	 * fall in KHO scratch memory. It also moves swapped out folios back to
	 * memory.
	 *
	 * A side effect of doing this is that it allocates a folio for all
	 * indices in the file. This might waste memory on sparse memfds. If
	 * that is really a problem in the future, we can have a
	 * memfd_pin_folios() variant that does not allocate a page on empty
	 * slots.
	 */
	/*
	 * pin 使 folio 在 kexec 前不迁移，并把换出页调回内存；CMA 页也被排除，避免
	 * 与 KHO scratch 重叠。代价是稀疏 memfd 的空洞也分配真实页，属于明确语义。
	 */
	nr_pinned = memfd_pin_folios(file, 0, size - 1, folios, max_folios,
				     &offset);
	if (nr_pinned < 0) {
		err = nr_pinned;
		pr_err("failed to pin folios: %d\n", err);
		goto err_free_folios;
	}
	nr_folios = nr_pinned;

	/* ABI 数组用 vmalloc 清零；尚未 preserve，失败只需 unpin 临时页列表。 */
	folios_ser = vcalloc(nr_folios, sizeof(*folios_ser));
	if (!folios_ser) {
		err = -ENOMEM;
		goto err_unpin;
	}

	/* 逐 folio 先登记 KHO，再在页锁内规范化可恢复状态，最后填写 ABI 记录。 */
	for (i = 0; i < nr_folios; i++) {
		struct memfd_luo_folio_ser *pfolio = &folios_ser[i];
		struct folio *folio = folios[i];

		/* KHO 记录 PFN/order 并拒绝 scratch 重叠；失败时当前页尚未登记。 */
		err = kho_preserve_folio(folio);
		if (err)
			goto err_unpreserve;

		folio_lock(folio);

		/*
		 * A dirty folio is one which has been written to. A clean folio
		 * is its opposite. Since a clean folio does not carry user
		 * data, it can be freed by page reclaim under memory pressure.
		 *
		 * Saving the dirty flag at prepare() time doesn't work since it
		 * can change later. Saving it at freeze() also won't work
		 * because the dirty bit is normally synced at unmap and there
		 * might still be a mapping of the file at freeze().
		 *
		 * To see why this is a problem, say a folio is clean at
		 * preserve, but gets dirtied later. The pfolio flags will mark
		 * it as clean. After retrieve, the next kernel might try to
		 * reclaim this folio under memory pressure, losing user data.
		 *
		 * Unconditionally mark it dirty to avoid this problem. This
		 * comes at the cost of making clean folios un-reclaimable after
		 * live update.
		 */
		/*
		 * dirty 可在 prepare 后继续变化，freeze 时映射写脏位也可能尚未同步；若把
		 * 当时的 clean 状态带到新内核，reclaim 可丢掉后来写入的数据。因此一律
		 * 标 dirty，以牺牲 clean 页可回收性换取跨 kexec 数据完整性。
		 */
		folio_mark_dirty(folio);

		/*
		 * If the folio is not uptodate, it was fallocated but never
		 * used. Saving this flag at prepare() doesn't work since it
		 * might change later when someone uses the folio.
		 *
		 * Since we have taken the performance penalty of allocating,
		 * zeroing, and pinning all the folios in the holes, take a bit
		 * more and zero all non-uptodate folios too.
		 *
		 * NOTE: For someone looking to improve preserve performance,
		 * this is a good place to look.
		 */
		/*
		 * 非 uptodate 页通常只 fallocate 未使用；既然洞已付出分配/pin 成本，就在
		 * 页锁内完整归零、刷新 dcache 并发布 uptodate，避免新内核暴露旧物理内容。
		 * 这是 preserve 性能可优化点，但不能省略数据初始化不变量。
		 */
		if (!folio_test_uptodate(folio)) {
			folio_zero_range(folio, 0, folio_size(folio));
			flush_dcache_folio(folio);
			folio_mark_uptodate(folio);
		}

		folio_unlock(folio);

		/* 页已稳定且状态规范化，序列记录发布 PFN、固定 flags 和原 page-cache index。 */
		pfolio->pfn = folio_pfn(folio);
		pfolio->flags = MEMFD_LUO_FOLIO_DIRTY | MEMFD_LUO_FOLIO_UPTODATE;
		pfolio->index = folio->index;
	}

	/* 最后把 ABI 数组自身的 vmalloc backing 登记 KHO；失败撤销全部 folio。 */
	err = kho_preserve_vmalloc(folios_ser, kho_vmalloc);
	if (err)
		goto err_unpreserve;

	/* 临时指针数组不跨 kexec；输出 vmap 指针仅供旧内核 abort/unpreserve 使用。 */
	kvfree(folios);
	*nr_foliosp = nr_folios;
	*out_folios_ser = folios_ser;

	/*
	 * Note: folios_ser is purposely not freed here. It is preserved
	 * memory (via KHO). In the 'unpreserve' path, we use the vmap pointer
	 * that is passed via private_data.
	 */
	/* folios_ser 故意不释放：其物理页已由 KHO 接管，private_data 保留旧 vmap。 */
	return 0;

err_unpreserve:
	/* 当前 i 项失败或数组 preserve 失败；只撤销 [0,i) 已成功登记的 folio。 */
	for (i = i - 1; i >= 0; i--)
		kho_unpreserve_folio(folios[i]);
	vfree(folios_ser);
err_unpin:
	/* 所有 memfd_pin_folios 返回的页均需 unpin，不论其中多少已 preserve。 */
	unpin_folios(folios, nr_folios);
err_free_folios:
	/* 最后释放不含页 ownership 的临时指针数组并返回最初错误。 */
	kvfree(folios);

	return err;
}

/*
 * 业务背景：旧内核取消 live update 时撤销 preserve_folios 成功状态，恢复普通
 * memfd 页和 vmalloc 数组生命周期；由 memfd_luo_unpreserve() 调用。
 * 入参：kho_vmalloc 是已 preserve ABI 数组描述；folios_ser 是其旧内核 vmap
 * 借用指针；nr_folios 是记录数。三者必须来自同一次 preserve 成功输出。
 * 出参/返回：无直接返回值；撤销数组/每页 KHO 登记、解除 pin 并释放 ABI 数组。
 * 注意事项：可睡眠；零记录快速返回且两个指针无需有效。PFN 为零槽跳过。
 */
static void memfd_luo_unpreserve_folios(struct kho_vmalloc *kho_vmalloc,
					struct memfd_luo_folio_ser *folios_ser,
					u64 nr_folios)
{
	/* i 是序列记录游标。 */
	long i;

	if (!nr_folios)
		return;

	/* 先撤销数组物理页的 KHO 保留，但保持旧 vmap 到遍历完成。 */
	kho_unpreserve_vmalloc(kho_vmalloc);

	/* 每个有效 PFN 恢复为 folio，撤销 KHO 后归还 memfd_pin_folios 引用。 */
	for (i = 0; i < nr_folios; i++) {
		const struct memfd_luo_folio_ser *pfolio = &folios_ser[i];
		struct folio *folio;

		if (!pfolio->pfn)
			continue;

		folio = pfn_folio(pfolio->pfn);

		kho_unpreserve_folio(folio);
		unpin_folio(folio);
	}

	/* 数组不再被 KHO 或遍历者使用，最终释放其虚拟映射/backing。 */
	vfree(folios_ser);
}

/*
 * 业务背景：LUO prepare 的 memfd 主入口，在旧内核冻结 shmem 变化、验证 seals，
 * 保存文件元数据和全部 folio，并把跨内核句柄交回 LUO core。
 * 入参：args 是 LUO 拥有的输入输出；入口 file 为待保存 memfd，成功写
 * serialized_data/private_data，handler 等字段借用且不改变。
 * 出参/返回：成功 0，inode 保持 freeze（但释放 inode lock），KHO 接管 ser/
 * folio/数组；失败负 errno，撤销已分配主结构并解冻/解锁，不留输出 ownership。
 * 注意事项：可睡眠；inode_lock 串行 size/seal/page-cache 准备。只接受当前 ABI
 * seals，且基础页数不得超过 memfd_pin_folios 的 UINT_MAX 容量。
 */
static int memfd_luo_preserve(struct liveupdate_file_op_args *args)
{
	/*
	 * inode 为锁定对象；ser 是 preserved 主 ABI；folios_ser 仅旧内核 abort 使用；
	 * nr_folios/inode_size 是稳定快照；seals/err 分别保存策略位和返回错误。
	 */
	struct inode *inode = file_inode(args->file);
	struct memfd_luo_folio_ser *folios_ser;
	struct memfd_luo_ser *ser;
	u64 nr_folios, inode_size;
	int err = 0, seals;

	/* 先锁 inode 再 freeze shmem，阻止后续改变需要序列化的文件主体状态。 */
	inode_lock(inode);
	shmem_freeze(inode, true);

	/* Allocate the main serialization structure in preserved memory */
	/* 主 packed 结构直接来自 KHO preserved 内存，物理地址将作为跨内核句柄。 */
	ser = kho_alloc_preserve(sizeof(*ser));
	if (IS_ERR(ser)) {
		err = PTR_ERR(ser);
		goto err_unlock;
	}

	/* seals 读取失败或含未来版本未知位时必须在保存页之前拒绝。 */
	seals = memfd_get_seals(args->file);
	if (seals < 0) {
		err = seals;
		goto err_free_ser;
	}

	/* Make sure the file only has the seals supported by this version. */
	/* 只允许 compatible 版本定义的 seal 集；未知语义不能静默丢失。 */
	if (seals & ~MEMFD_LUO_ALL_SEALS) {
		err = -EOPNOTSUPP;
		goto err_free_ser;
	}

	/* prepare 保存初始位置供调试/回滚，freeze 会在切换前再取最终 f_pos。 */
	ser->pos = args->file->f_pos;
	inode_size = i_size_read(inode);

	/*
	 * memfd_pin_folios() caps at UINT_MAX folios; refuse larger
	 * files to avoid silently preserving only a prefix.
	 */
	/* pin helper 的 folio 数参数为 unsigned int；超限直接 -EFBIG，禁止截断前缀。 */
	if (DIV_ROUND_UP_ULL(inode_size, PAGE_SIZE) > UINT_MAX) {
		err = -EFBIG;
		goto err_free_ser;
	}

	/* 元数据校验完成后写 ABI 标量，再保存可能失败的 folio/数组图。 */
	ser->size = inode_size;
	ser->seals = seals;

	/* 成功后 ser->folios 描述跨内核数组，folios_ser 则是旧内核私有 vmap。 */
	err = memfd_luo_preserve_folios(args->file, &ser->folios,
					&folios_ser, &nr_folios);
	if (err)
		goto err_free_ser;

	/* 全部状态已 preserve；发布数量并释放 inode 锁，但维持 shmem freeze。 */
	ser->nr_folios = nr_folios;
	inode_unlock(inode);

	/* 两个输出是 LUO 后续 unpreserve/freeze 入口的 ownership 交接点。 */
	args->private_data = folios_ser;
	args->serialized_data = virt_to_phys(ser);

	return 0;

err_free_ser:
	/* 页保存尚未成功或已自行回滚，只需撤销并释放主 KHO 结构。 */
	kho_unpreserve_free(ser);
err_unlock:
	/* 所有失败路径恢复文件可写状态并逆序释放 inode 锁。 */
	shmem_freeze(inode, false);
	inode_unlock(inode);
	return err;
}

/*
 * 业务背景：旧内核真正 kexec 前的最后冻结回调，只刷新 preserve 后仍可变化的
 * file position；数据/size/seals 已由持续 shmem freeze 保持。
 * 入参：args 借用成功 preserve 的 file 和非零 serialized_data。
 * 出参/返回：成功 0 并更新 preserved ser->pos；缺失句柄返回 -EINVAL。
 * 注意事项：不取得 inode_lock，LUO 调用时点保证文件位置快照语义；物理句柄
 * 通过 direct map 转回旧内核虚拟地址。
 */
static int memfd_luo_freeze(struct liveupdate_file_op_args *args)
{
	/* ser 是成功 preserve 的主结构借用指针，只更新其中最终位置。 */
	struct memfd_luo_ser *ser;

	if (WARN_ON_ONCE(!args->serialized_data))
		return -EINVAL;

	ser = phys_to_virt(args->serialized_data);

	/*
	 * The pos might have changed since prepare. Everything else stays the
	 * same.
	 */
	/* preserve 后只有 f_pos 允许继续变化，freeze 时覆盖为最终值；其余字段不动。 */
	ser->pos = args->file->f_pos;

	return 0;
}

/*
 * 业务背景：live update 在旧内核 abort 时完整撤销 memfd preserve，解冻原文件
 * 并归还所有 KHO/pin 资源。
 * 入参：args 必须来自成功 preserve；file/serialized_data/private_data 均借用。
 * 出参/返回：无直接返回值；文件恢复可修改，序列结构和 folio 数组被释放。
 * 注意事项：可睡眠并获取 inode_lock；无 serialized_data 仅告警返回，不能重复。
 */
static void memfd_luo_unpreserve(struct liveupdate_file_op_args *args)
{
	/* inode 是解冻同步对象；ser 从跨内核物理句柄恢复。 */
	struct inode *inode = file_inode(args->file);
	struct memfd_luo_ser *ser;

	if (WARN_ON_ONCE(!args->serialized_data))
		return;

	/* 与 preserve 相同锁序，在锁内解除 freeze 并销毁其序列化资源。 */
	inode_lock(inode);
	shmem_freeze(inode, false);

	ser = phys_to_virt(args->serialized_data);

	/* 先撤销所有子资源，最后才能释放承载 folios 描述符的主 ser。 */
	memfd_luo_unpreserve_folios(&ser->folios, args->private_data,
				    ser->nr_folios);

	kho_unpreserve_free(ser);
	inode_unlock(inode);
}

/*
 * 业务背景：新内核未调用 retrieve 时丢弃一组 preserved folio，避免 KHO 页永远
 * 保留；由 finish 的“未尝试恢复”分支调用。
 * 入参：folios_ser 是已 restore-vmalloc 的只读记录数组借用；nr_folios 为元素数。
 * 出参/返回：无直接返回值；每个可恢复 PFN 转为 folio 后 put，释放其 KHO 持有。
 * 注意事项：可睡眠；单页恢复失败只限速告警并继续，无法通过 void 回滚/上报。
 */
static void memfd_luo_discard_folios(const struct memfd_luo_folio_ser *folios_ser,
				     u64 nr_folios)
{
	/* i 遍历 ABI 记录。 */
	u64 i;

	/* PFN 0 是空记录哨兵；其余物理地址逐项从 KHO 转回普通 folio ownership。 */
	for (i = 0; i < nr_folios; i++) {
		const struct memfd_luo_folio_ser *pfolio = &folios_ser[i];
		struct folio *folio;
		phys_addr_t phys;

		if (!pfolio->pfn)
			continue;

		phys = PFN_PHYS(pfolio->pfn);
		/* restore 成功返回一个待 put 的 folio；失败页无法在此进一步定位释放。 */
		folio = kho_restore_folio(phys);
		if (!folio) {
			pr_warn_ratelimited("Unable to restore folio at physical address: %llx\n",
					    phys);
			continue;
		}

		folio_put(folio);
	}
}

/*
 * 业务背景：新内核会话结束时清理未被 retrieve 消费的 incoming memfd 状态；
 * retrieve 成功或失败都已自行处理，只有 status==0 需要这里接管。
 * 入参：args 是 LUO 借用上下文；retrieve_status 表示未尝试/成功/失败，
 * serialized_data 是 incoming 主结构物理句柄。
 * 出参/返回：无直接返回值；未尝试时尽力 restore+put folios，释放数组与主 ser。
 * 注意事项：可睡眠；数组恢复失败仍跳过其页并释放主结构，属于尽力 cleanup。
 */
static void memfd_luo_finish(struct liveupdate_file_op_args *args)
{
	/* folios_ser 是临时恢复的 vmap；ser 是 incoming 主 ABI 借用。 */
	struct memfd_luo_folio_ser *folios_ser;
	struct memfd_luo_ser *ser;

	/*
	 * If retrieve was successful, nothing to do. If it failed, retrieve()
	 * already cleaned up everything it could. So nothing to do there
	 * either. Only need to clean up when retrieve was not called.
	 */
	/* 非零表示 retrieve 已经负责成功 ownership 或失败清理，重复处理会 double put。 */
	if (args->retrieve_status)
		return;

	/* 零/无效句柄没有可清理主结构。 */
	ser = phys_to_virt(args->serialized_data);
	if (!ser)
		return;

	/* 有记录才恢复 vmalloc 数组；成功后逐页丢弃并释放临时 vmap。 */
	if (ser->nr_folios) {
		folios_ser = kho_restore_vmalloc(&ser->folios);
		if (!folios_ser)
			goto out;

		memfd_luo_discard_folios(folios_ser, ser->nr_folios);
		vfree(folios_ser);
	}

out:
	/* 无论数组是否恢复成功，主 ser 的 incoming KHO ownership 在此终结。 */
	kho_restore_free(ser);
}

/*
 * 业务背景：新内核把 preserved folio 逐个恢复并插入新建 shmem memfd 的 page
 * cache，重建 memcg、inode block、LRU 与 dirty/uptodate 状态。
 * 入参：file 是尚未对外发布的新 memfd，由调用者持有；folios_ser 为恢复数组
 * 借用；nr_folios 为记录数。
 * 出参/返回：成功 0，所有有效页 ownership 转入 file mapping；失败负 errno，
 * 当前页按阶段回滚，已插入页留给最终 fput，尚未处理页直接 restore+put。
 * 注意事项：可睡眠；调用者尚未发布 file，故无需 inode_lock。page-cache 插入
 * 是单页不可回滚边界，最终 shmem_recalc_inode 只校正成功加入的基础页数量。
 */
static int memfd_luo_retrieve_folios(struct file *file,
				     struct memfd_luo_folio_ser *folios_ser,
				     u64 nr_folios)
{
	/*
	 * inode/mapping 是目标；folio 是当前 restored 页；npages/nr_added_pages 负责
	 * inode 计账；i 是回滚分界；err 默认 -EIO 并由具体 helper 覆盖。
	 */
	struct inode *inode = file_inode(file);
	struct address_space *mapping = inode->i_mapping;
	struct folio *folio;
	long npages, nr_added_pages = 0;
	int err = -EIO;
	long i;

	/* 按序列顺序恢复；index 决定 page-cache 位置而非数组顺序。 */
	for (i = 0; i < nr_folios; i++) {
		const struct memfd_luo_folio_ser *pfolio = &folios_ser[i];
		phys_addr_t phys;
		u64 index;
		int flags;

		if (!pfolio->pfn)
			continue;

		/* PFN/order 由 KHO 恢复为持引用 folio；失败后转统一剩余页清理。 */
		phys = PFN_PHYS(pfolio->pfn);
		folio = kho_restore_folio(phys);
		if (!folio) {
			pr_err("Unable to restore folio at physical address: %llx\n",
			       phys);
			err = -EIO;
			goto put_folios;
		}
		index = pfolio->index;
		flags = pfolio->flags;

		/* Set up the folio for insertion. */
		/* 新 mapping 插入要求 folio 锁定且带 swapbacked，匹配 shmem 页语义。 */
		__folio_set_locked(folio);
		__folio_set_swapbacked(folio);

		/* 先向当前根/默认 memcg 计费；后续插入失败由 folio_put 路径解除计费。 */
		err = mem_cgroup_charge(folio, NULL, mapping_gfp_mask(mapping));
		if (err) {
			pr_err("shmem: failed to charge folio index %ld: %d\n",
			       i, err);
			goto unlock_folio;
		}

		/* 成功插入是其他 mapping 操作可观察的单页发布点，失败页仍由本地持有。 */
		err = shmem_add_to_page_cache(folio, mapping, index, NULL,
					      mapping_gfp_mask(mapping));
		if (err) {
			pr_err("shmem: failed to add to page cache folio index %ld: %d\n",
			       i, err);
			goto unlock_folio;
		}

		/* ABI flags 在插入后恢复；当前 preserve 总置两位，但仍按版本字段解析。 */
		if (flags & MEMFD_LUO_FOLIO_UPTODATE)
			folio_mark_uptodate(folio);
		if (flags & MEMFD_LUO_FOLIO_DIRTY)
			folio_mark_dirty(folio);

		/* page cache 已接管页后补 inode block 计账；失败必须先从 cache 摘除。 */
		npages = folio_nr_pages(folio);
		err = shmem_inode_acct_blocks(inode, npages);
		if (err) {
			pr_err("shmem: failed to account folio index %ld(%ld pages): %d\n",
			       i, npages, err);
			goto remove_from_cache;
		}

		/* 计账完成后加入 LRU、解锁并释放本地引用，mapping 成为最终 owner。 */
		nr_added_pages += npages;
		folio_add_lru(folio);
		folio_unlock(folio);
		folio_put(folio);
	}

	/* 批次成功后一次重算 inode alloced/swapped 统计，反映所有加入基础页。 */
	shmem_recalc_inode(inode, nr_added_pages, 0);

	return 0;

remove_from_cache:
	/* 当前页已插入但 block 计账失败：先撤销 page-cache ownership。 */
	filemap_remove_folio(folio);
unlock_folio:
	/* 当前页未插入或已摘除，解锁并 put 会撤销 memcg/KHO 恢复引用。 */
	folio_unlock(folio);
	folio_put(folio);
put_folios:
	/*
	 * Note: don't free the folios already added to the file. They will be
	 * freed when the file is freed. Free the ones not added yet here.
	 */
	/*
	 * 已成功加入的前缀留在 file 中，由上层失败 fput 统一释放；只 restore+put
	 * i 之后尚未处理的记录，避免既泄漏 preserved 页又 double-free 已发布页。
	 */
	for (long j = i + 1; j < nr_folios; j++) {
		/* j 是失败点之后尚未发布记录的游标；pfolio/phys 仅在本轮借用。 */
		const struct memfd_luo_folio_ser *pfolio = &folios_ser[j];
		phys_addr_t phys;

		if (!pfolio->pfn)
			continue;

		phys = PFN_PHYS(pfolio->pfn);
		/* restore+put 直接消费该 KHO 页，不把它插入失败 file。 */
		folio = kho_restore_folio(phys);
		if (folio)
			folio_put(folio);
	}

	shmem_recalc_inode(inode, nr_added_pages, 0);

	return err;
}

/*
 * 业务背景：新内核按 incoming ABI 重建一个普通 memfd、恢复 seals/位置/大小和
 * page cache，并把新 file 输出给 LUO core。
 * 入参：args 是输入输出上下文，serialized_data 指向 preserved ser；成功写 file。
 * 出参/返回：成功 0，args->file 取得新 file ownership，ser/数组被消费释放；
 * 失败负 errno，fput 已创建 file 并释放本函数已经恢复/插入的页，主 ser 被释放，
 * 不输出有效 file。
 * 注意事项：可睡眠；先以 MFD_ALLOW_SEALING 创建再重放 seals，未知 seal 拒绝；
 * file 尚未发布，无并发读者。Hugetlb 不会通过 can_preserve 进入此 ABI；在
 * 数组恢复前失败时本函数不遍历子 folio，这是当前实现明确的尽力清理边界，
 * 不能据此声称所有 preserved 子页都已在本函数内回收。
 */
static int memfd_luo_retrieve(struct liveupdate_file_op_args *args)
{
	/* folios_ser 是临时 restored vmap；ser 是主 ABI；file 为构造中 owner；err 传错。 */
	struct memfd_luo_folio_ser *folios_ser;
	struct memfd_luo_ser *ser;
	struct file *file;
	int err;

	/* 主物理句柄必须可由 direct map 解释；NULL 在取得资源前失败。 */
	ser = phys_to_virt(args->serialized_data);
	if (!ser)
		return -EINVAL;

	/* Make sure the file only has seals supported by this version. */
	/* 再次验证 incoming 数据，防止不同版本/损坏 ABI 把未知 seal 静默应用。 */
	if (ser->seals & ~MEMFD_LUO_ALL_SEALS) {
		err = -EOPNOTSUPP;
		goto free_ser;
	}

	/*
	 * The seals are preserved. Allow sealing here so they can be added
	 * later.
	 */
	/* 创建时临时允许 sealing，随后才能把原 seal（包括 F_SEAL_SEAL）完整重放。 */
	file = memfd_alloc_file("", MFD_ALLOW_SEALING);
	if (IS_ERR(file)) {
		pr_err("failed to setup file: %pe\n", file);
		err = PTR_ERR(file);
		goto free_ser;
	}

	/* seals 先于文件发布及页恢复提交，失败只需 fput 空文件。 */
	err = memfd_add_seals(file, ser->seals);
	if (err) {
		pr_err("failed to add seals: %pe\n", ERR_PTR(err));
		goto put_file;
	}

	/* 恢复 f_pos 与 i_size；setpos 的合法范围由跨内核 ABI/原文件保证。 */
	vfs_setpos(file, ser->pos, MAX_LFS_FILESIZE);
	i_size_write(file_inode(file), ser->size);

	/* 非空文件恢复描述数组并逐页转入新 mapping；vmap 无论成功失败都释放。 */
	if (ser->nr_folios) {
		folios_ser = kho_restore_vmalloc(&ser->folios);
		if (!folios_ser) {
			err = -EINVAL;
			goto put_file;
		}

		/* helper 消费各 preserved folio；数组 vmap 在返回后不再含 ownership。 */
		err = memfd_luo_retrieve_folios(file, folios_ser, ser->nr_folios);
		vfree(folios_ser);
		if (err)
			goto put_file;
	}

	/* 所有构造完成后才向 LUO 发布 file；随后主 ser 已无用途并被消费。 */
	args->file = file;
	kho_restore_free(ser);

	return 0;

put_file:
	/* fput 清理已插入前缀；仅调用过 helper 时，未插入页才已由 helper 处理。 */
	fput(file);
free_ser:
	/* 主 incoming 序列结构在所有返回路径只释放一次。 */
	kho_restore_free(ser);
	return err;
}

/*
 * 业务背景：LUO 扫描 fd 时用轻量检查挑出本 handler 支持的匿名普通 shmem
 * memfd，排除有目录链接的普通 tmpfs 文件和 Hugetlb memfd。
 * 入参：handler 是当前注册处理器借用且本实现不读取；file 是候选文件借用。
 * 出参/返回：shmem_file 且 inode->i_nlink==0 返回 true，否则 false；无副作用。
 * 注意事项：按框架契约必须轻量、不可睡眠；结果只判类型，preserve 时仍需
 * 重新验证 seals/大小等可变状态。
 */
static bool memfd_luo_can_preserve(struct liveupdate_file_handler *handler,
				   struct file *file)
{
	/* inode 只借用来检查无目录链接这一 memfd 身份特征。 */
	struct inode *inode = file_inode(file);

	return shmem_file(file) && !inode->i_nlink;
}

/*
 * 业务背景：为同一 live inode 的多个 fd 提供会话内稳定标识，使 LUO 识别共享
 * file backing，而非按 fd 号重复保存。
 * 入参：file 是存活候选文件借用指针。
 * 出参/返回：返回当前内核 inode 地址转换的 unsigned long，不取得引用。
 * 注意事项：仅本次旧内核运行期唯一，绝不能作为跨 kexec ABI 地址解引用；
 * file/inode 生命周期由 LUO 调用点保证。
 */
static unsigned long memfd_luo_get_id(struct file *file)
{
	return (unsigned long)file_inode(file);
}

/*
 * memfd_luo_file_ops 把旧内核 preserve/freeze/unpreserve 与新内核
 * retrieve/finish 生命周期配对；owner 持有模块引用，防止回调执行期卸载。
 * 未提供 unfreeze/can_finish，框架使用默认时序。
 */
static const struct liveupdate_file_ops memfd_luo_file_ops = {
	/* 切换前最后刷新 f_pos；新内核未 retrieve 时 finish 丢弃 incoming 页。 */
	.freeze = memfd_luo_freeze,
	.finish = memfd_luo_finish,
	/* 新内核重建 file；旧内核建立或 abort 整个 KHO 对象图。 */
	.retrieve = memfd_luo_retrieve,
	.preserve = memfd_luo_preserve,
	.unpreserve = memfd_luo_unpreserve,
	/* 扫描阶段按匿名 shmem 匹配，并以 inode 地址合并同一 backing。 */
	.can_preserve = memfd_luo_can_preserve,
	.get_id = memfd_luo_get_id,
	.owner = THIS_MODULE,
};

/* handler 以 memfd-v2 compatible 绑定上述 packed ABI 与回调表；注册后由 LUO 借用。 */
static struct liveupdate_file_handler memfd_luo_handler = {
	.ops = &memfd_luo_file_ops,
	.compatible = MEMFD_LUO_FH_COMPATIBLE,
};

/*
 * 业务背景：late init 时向 LUO core 注册 memfd-v2 handler，使会话扫描可匹配。
 * 入参：无。
 * 出参/返回：注册成功返回 0；LIVEUPDATE 未启用的 -EOPNOTSUPP 被视为正常禁用；
 * 其他负 errno 记录日志并使 initcall 报错。
 * 注意事项：late_initcall 可睡眠且单次执行；静态 handler/ops 生命周期覆盖内核。
 */
static int __init memfd_luo_init(void)
{
	/* err 是全局 handler 列表注册结果。 */
	int err = liveupdate_register_file_handler(&memfd_luo_handler);

	/* 配置桩返回 -EOPNOTSUPP 不应污染启动日志；真实注册错误必须传播。 */
	if (err && err != -EOPNOTSUPP) {
		pr_err("Could not register luo filesystem handler: %pe\n",
		       ERR_PTR(err));

		return err;
	}

	return 0;
}
late_initcall(memfd_luo_init);
