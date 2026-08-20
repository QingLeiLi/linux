// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Andrei Vagin <avagin@openvz.org>
 * Author: Dmitry Safonov <dima@arista.com>
 */

#include <linux/cleanup.h>
#include <linux/mm.h>
#include <linux/time_namespace.h>
#include <linux/time.h>
#include <linux/vdso_datastore.h>

#include <vdso/clocksource.h>
#include <vdso/datapage.h>

#include "namespace_internal.h"

/*
 * 把内核 timespec64 形式的已校验 offset 压缩为 VDSO 数据页 ABI 使用的 sec/nsec 对；不归一化也不修改输入，
 * 因为 proc 写入路径已保证纳秒范围和整体可表示性。
 */
static struct timens_offset offset_from_ts(struct timespec64 off)
{
	struct timens_offset ret;

	ret.sec = off.tv_sec;
	ret.nsec = off.tv_nsec;

	return ret;
}

/*
 * A time namespace VVAR page has the same layout as the VVAR page which
 * contains the system wide VDSO data.
 *
 * For a normal task the VVAR pages are installed in the normal ordering:
 *     VVAR
 *     PVCLOCK
 *     HVCLOCK
 *     TIMENS   <- Not really required
 *
 * Now for a timens task the pages are installed in the following order:
 *     TIMENS
 *     PVCLOCK
 *     HVCLOCK
 *     VVAR
 *
 * The check for vdso_clock->clock_mode is in the unlikely path of
 * the seq begin magic. So for the non-timens case most of the time
 * 'seq' is even, so the branch is not taken.
 *
 * If 'seq' is odd, i.e. a concurrent update is in progress, the extra check
 * for vdso_clock->clock_mode is a non-issue. The task is spin waiting for the
 * update to finish and for 'seq' to become even anyway.
 *
 * Timens page has vdso_clock->clock_mode set to VDSO_CLOCKMODE_TIMENS which
 * enforces the time namespace handling path.
 */
/*
 * time namespace 的 VVAR 页沿用系统 VVAR 的布局，但映射顺序互换：普通任务先看到真实 VVAR，命名空间任务则
 * 先看到 TIMENS 页、最后才看到真实 VVAR。TIMENS 页把 seq 固定为奇数 1，并以特殊 clock_mode 告诉 VDSO：
 * 这不是等待中的普通写更新，而应改读后一个槽位的宿主时钟数据，再叠加本页按 clock id 保存的 offset。
 * 普通任务仅在真实 seq 偶发为奇数时多检查一次 mode，因此不会给稳定快路径增加常态分支成本。
 */
/*
 * 初始化一个 clocksource base 对应的命名空间 VDSO 描述。MONOTONIC 的 offset 同时供 raw/coarse 派生 clock
 * 使用，BOOTTIME 同时供 alarm clock 使用；其余 offset 保持零页分配时的 0，REALTIME 因而不被虚拟化。
 * 调用者持有 timens_offset_lock，且只在 namespace 首次冻结时写入一次。
 */
static void timens_setup_vdso_clock_data(struct vdso_clock *vc,
					 struct time_namespace *ns)
{
	struct timens_offset *offset = vc->offset;
	struct timens_offset monotonic = offset_from_ts(ns->offsets.monotonic);
	struct timens_offset boottime = offset_from_ts(ns->offsets.boottime);

	vc->seq				= 1;
	vc->clock_mode			= VDSO_CLOCKMODE_TIMENS;
	offset[CLOCK_MONOTONIC]		= monotonic;
	offset[CLOCK_MONOTONIC_RAW]	= monotonic;
	offset[CLOCK_MONOTONIC_COARSE]	= monotonic;
	offset[CLOCK_BOOTTIME]		= boottime;
	offset[CLOCK_BOOTTIME_ALARM]	= boottime;
}

/*
 * 为当前任务触发的 VVAR fault 选择其 time namespace 专属页。正常 fault 必须来自 current->mm；远程内存访问
 * 本应被 special mapping 的 VM_IO/VM_PFNMAP 类限制挡住，若仍到达则 WARN 并返回 NULL，使调用者不映射专属页。
 * 返回指针由 namespace 持有，本函数不增加 page 引用，fault 层在真正安装页面前负责 get_page()。
 */
struct page *find_timens_vvar_page(struct vm_area_struct *vma)
{
	if (likely(vma->vm_mm == current->mm))
		return current->nsproxy->time_ns->vvar_page;

	/*
	 * VM_PFNMAP | VM_IO protect .fault() handler from being called
	 * through interfaces like /proc/$pid/mem or
	 * process_vm_{readv,writev}() as long as there's no .access()
	 * in special_mapping_vmops().
	 * For more details check_vma_flags() and __access_remote_vm()
	 */
	/*
	 * VM_PFNMAP | VM_IO 与 special mapping 不提供 .access() 的组合，应阻止 /proc/$pid/mem、
	 * process_vm_readv()/writev() 等远程接口调用 fault；细节见 check_vma_flags() 和 __access_remote_vm()。
	 */

	WARN(1, "vvar_page accessed remotely");

	return NULL;
}

/*
 * 在非初始 namespace 第一次投入使用时，把已验证的 offsets 固化到其零初始化 vvar_page。双重检查让后续任务
 * 无锁返回，首个调用者则在 timens_offset_lock 下只初始化一次全部常规及可选 aux clock bases。
 * init_time_ns 始终使用系统 VVAR，无需专属页；本函数无错误返回，task 参数仅保持 commit 接口的任务语境。
 */
static void timens_set_vvar_page(struct task_struct *task,
				struct time_namespace *ns)
{
	struct vdso_time_data *vdata;
	struct vdso_clock *vc;
	unsigned int i;

	if (ns == &init_time_ns)
		return;

	/* Fast-path, taken by every task in namespace except the first. */
	/* 快路径供该 namespace 中除首个提交者外的所有任务使用，避免再次获取全局 offset 锁。 */
	if (likely(ns->frozen_offsets))
		return;

	guard(mutex)(&timens_offset_lock);
	/* Nothing to-do: vvar_page has been already initialized. */
	/* 锁内复查处理并发首访：另一提交者若已完成初始化，本次不再写页面。 */
	if (ns->frozen_offsets)
		return;

	ns->frozen_offsets = true;
	vdata = page_address(ns->vvar_page);
	vc = vdata->clock_data;

	for (i = 0; i < CS_BASES; i++)
		timens_setup_vdso_clock_data(&vc[i], ns);

	if (IS_ENABLED(CONFIG_POSIX_AUX_CLOCKS)) {
		for (i = 0; i < ARRAY_SIZE(vdata->aux_clock_data); i++)
			timens_setup_vdso_clock_data(&vdata->aux_clock_data[i], ns);
	}
}

/*
 * The vvar page layout depends on whether a task belongs to the root or
 * non-root time namespace. Whenever a task changes its namespace, the VVAR
 * page tables are cleared and then they will be re-faulted with a
 * corresponding layout.
 * See also the comment near timens_setup_vdso_clock_data() for details.
 */
/*
 * VVAR 页布局取决于任务处于初始还是非初始 time namespace。任务切换 namespace 时先清除其 mm 中已有 VVAR
 * 页表项；后续 fault 再依据新的 current->nsproxy 选择普通或 TIMENS 互换布局。具体顺序见
 * timens_setup_vdso_clock_data() 附近说明。
 */
/*
 * 在 task->mm 的全部 VMA 中找出通用 VVAR special mapping 并 zap 已建立的页表项。mmap 读锁稳定 VMA 遍历；
 * 不删除 VMA，也不主动 fault，新布局由下一次用户态访问按需建立。当前实现始终返回 0，ns 仅表达目标语境。
 */
static int vdso_join_timens(struct task_struct *task, struct time_namespace *ns)
{
	struct mm_struct *mm = task->mm;
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, 0);

	guard(mmap_read_lock)(mm);
	for_each_vma(vmi, vma) {
		if (vma_is_special_mapping(vma, &vdso_vvar_mapping))
			zap_vma(vma);
	}
	return 0;
}

/*
 * 提交任务的 time namespace VDSO 视图：先确保目标 namespace 的 offset 页只初始化一次，再清除任务 mm 中旧
 * VVAR 映射，使下一次 fault 按新布局重建。调用点位于 fork 子任务尚未运行或 setns 的不可回滚阶段；无失败
 * 返回，且 vdso_join_timens() 当前的恒 0 结果无需传播。
 */
void timens_commit(struct task_struct *tsk, struct time_namespace *ns)
{
	timens_set_vvar_page(tsk, ns);
	vdso_join_timens(tsk, ns);
}

/*
 * 为新 time namespace 分配一页计入内核内存账户且清零的 VVAR 存储，并把所有权写入 ns->vvar_page。
 * 成功返回 0；内存不足返回 -ENOMEM 且字段为 NULL。清零保证尚未显式设置的 clock offsets 保持 0。
 */
int timens_vdso_alloc_vvar_page(struct time_namespace *ns)
{
	ns->vvar_page = alloc_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	if (!ns->vvar_page)
		return -ENOMEM;

	return 0;
}

/*
 * 释放先前成功分配且由 ns 独占持有的 VVAR 页；调用方保证生命周期已无新 fault，并且不会对失败分配或同一页
 * 重复调用。本函数不清空指针，因为紧随其后的是 namespace 对象销毁或 clone 失败回滚。
 */
void timens_vdso_free_vvar_page(struct time_namespace *ns)
{
	__free_page(ns->vvar_page);
}
