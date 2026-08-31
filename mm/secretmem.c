// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright IBM Corporation, 2021
 *
 * Author: Mike Rapoport <rppt@linux.ibm.com>
 */

/*
 * memfd_secret() 返回一个匿名文件：用户先 ftruncate 一次确定容量，再以共享映射触发
 * fault。每张 backing 页从内核 direct map 摘除、禁止 GUP/迁移/换出与 core dump，
 * 只经持有该文件的进程所建立的用户页表映射访问；释放时恢复 direct map 并清零，
 * 防止秘密进入普通页分配器。
 */

#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/swap.h>
#include <linux/mount.h>
#include <linux/memfd.h>
#include <linux/bitops.h>
#include <linux/printk.h>
/* page cache、fault、syscall 与伪文件系统接口共同组成 secretmem 的 VFS/MM 边界。 */
#include <linux/pagemap.h>
#include <linux/syscalls.h>
#include <linux/pseudo_fs.h>
#include <linux/secretmem.h>
#include <linux/set_memory.h>
#include <linux/sched/signal.h>

#include <uapi/linux/magic.h>

#include <asm/tlbflush.h>

#include "internal.h"

#undef pr_fmt
/* 本文件所有 pr_* 消息统一加 secretmem 子系统前缀，便于启动诊断归类。 */
#define pr_fmt(fmt) "secretmem: " fmt

/*
 * Define mode and flag masks to allow validation of the system call
 * parameters.
 */
/*
 * mode/flag 掩码用于严格校验 syscall 参数。当前没有私有 mode 位，因此只接受 0 和
 * 通用 O_CLOEXEC；保留独立宏让未来扩展时能与 fcntl 标志做编译期冲突检查。
 */
#define SECRETMEM_MODE_MASK	(0x0)
#define SECRETMEM_FLAGS_MASK	SECRETMEM_MODE_MASK

/*
 * secretmem_enable 是启动期/只读模块参数，初始化后置于只读区；默认开启，管理员可用
 * secretmem.enable=0 禁止挂载和 syscall。0400 只允许读取参数状态，不能运行期改写。
 */
static bool secretmem_enable __ro_after_init = 1;
module_param_named(enable, secretmem_enable, bool, 0400);
MODULE_PARM_DESC(secretmem_enable,
		 "Enable secretmem and memfd_secret(2) system call");

/*
 * secretmem_users 统计已成功创建且尚未 release 的 secretmem file 数；原子操作允许不同
 * 进程并发 open/close。正值还阻止 hibernation，因为休眠镜像不能安全包含这些秘密页。
 */
static atomic_t secretmem_users;

/*
 * 查询系统是否仍存在任何 secretmem 文件。
 * 业务背景：hibernation_available() 用它拒绝在 secretmem 活跃时创建休眠镜像。
 * 入参：无。出参/返回：计数非零返回 true，否则 false；无引用/ownership 变化。
 * 注意事项：原子读取只提供瞬时快照，不冻结并发 create/release，不可据此长期持有对象；
 * 不睡眠、无锁，可在快速状态检查中调用。
 */
bool secretmem_active(void)
{
	return !!atomic_read(&secretmem_users);
}

/*
 * 为 secretmem VMA 的一个文件偏移查找或按需创建 order-0 隐密 folio。
 * 业务背景：mmap_prepare 只建立 VMA，真实页在缺页时分配；本函数在页进入 page cache
 * 前撤销 direct-map 映射，确保普通内核页表无法直接读取秘密。
 * 入参：@vmf 是 fault 核心借用的输入输出上下文，提供 vma/pgoff/gfp；文件、inode、
 * mapping 都从 vma 借用，调用期间由 mmap/file 生命周期稳定。
 * 出参/返回：成功把锁定页写入 vmf->page 并返回 VM_FAULT_LOCKED，fault 核心负责后续
 * 建 PTE 与解锁；越过 i_size 返回 EINVAL fault，分配失败返回 OOM，其他 errno 转 fault。
 * 注意事项：可睡眠；持 mapping invalidate shared lock 与 setattr/truncate 竞争。新页只有
 * direct map 已失效并标记 uptodate 后才发布到 page cache；EEXIST 回滚候选页并重试赢家。
 */
static vm_fault_t secretmem_fault(struct vm_fault *vmf)
{
	/* mapping/inode 是目标 secretmem 文件的借用对象；offset 是文件页索引。 */
	struct address_space *mapping = vmf->vma->vm_file->f_mapping;
	struct inode *inode = file_inode(vmf->vma->vm_file);
	pgoff_t offset = vmf->pgoff;
	/* gfp 沿用 fault 允许的回收语义；addr 只用于 kernel direct-map TLB flush 范围。 */
	gfp_t gfp = vmf->gfp_mask;
	unsigned long addr;
	/* folio 始终为 order-0 且成功出口保持锁定；ret/err 分别是 fault 与 errno 表示。 */
	struct folio *folio;
	vm_fault_t ret;
	int err;

	/* 阶段 1：只允许 fault 落在用户已用首次 truncate 声明的文件大小内。 */
	if (((loff_t)vmf->pgoff << PAGE_SHIFT) >= i_size_read(inode))
		return vmf_error(-EINVAL);

	/* shared 侧允许并发 fault，但与独占 setattr 大小提交串行，稳定 page cache/size 协议。 */
	filemap_invalidate_lock_shared(mapping);

retry:
	/* 阶段 2：先尝试查找并锁定已发布 folio；ERR_PTR 表示该 offset 当前无页。 */
	folio = filemap_lock_folio(mapping, offset);
	if (IS_ERR(folio)) {
		/* 缺页候选必须清零，避免新 secret 页暴露 allocator 中的旧数据。 */
		folio = folio_alloc(gfp | __GFP_ZERO, 0);
		if (!folio) {
			ret = VM_FAULT_OOM;
			goto out;
		}

		/* 阶段 3：在 page-cache 发布前撤销内核 direct map；架构拒绝时释放候选页。 */
		err = set_direct_map_invalid_noflush(folio_page(folio, 0));
		if (err) {
			folio_put(folio);
			ret = vmf_error(err);
			goto out;
		}

		/* 零页内容已完整，标记 uptodate 后才允许 filemap 查找者消费。 */
		__folio_mark_uptodate(folio);
		/* filemap_add_folio 是并发可见发布点；成功后 mapping 接管 page-cache 引用。 */
		err = filemap_add_folio(mapping, folio, offset, gfp);
		if (unlikely(err)) {
			/*
			 * If a split of large page was required, it
			 * already happened when we marked the page invalid
			 * which guarantees that this call won't fail
			 */
			/*
			 * direct map 失效可能需要先拆大页，而这一步已经成功完成；因此恢复默认
			 * 映射不应再因拆分失败。先恢复再 folio_put，避免普通 allocator 接收隐形页。
			 */
			set_direct_map_default_noflush(folio_page(folio, 0));
			folio_put(folio);
			/* 另一 fault 抢先发布同 offset 时重试并锁住赢家，不把竞态暴露为用户错误。 */
			if (err == -EEXIST)
				goto retry;

			ret = vmf_error(err);
			goto out;
		}

		/* 阶段 4：候选页已归 mapping，刷新内核虚拟范围使 direct-map 撤销对所有 CPU 生效。 */
		addr = (unsigned long)folio_address(folio);
		flush_tlb_kernel_range(addr, addr + PAGE_SIZE);
	}

	/* 已有页和新页在此汇合；order-0 folio_file_page 得到本 pgoff 对应的锁定 page。 */
	vmf->page = folio_file_page(folio, vmf->pgoff);
	ret = VM_FAULT_LOCKED;

out:
	/* 所有出口释放 invalidate shared lock；VM_FAULT_LOCKED 故意不解 folio 锁。 */
	filemap_invalidate_unlock_shared(mapping);
	return ret;
}

/* secretmem VMA 只自定义 fault；静态只读表的地址也作为 vma_is_secretmem() 身份标签。 */
static const struct vm_operations_struct secretmem_vm_ops = {
	.fault = secretmem_fault,
};

/*
 * 在最后一个 struct file 引用关闭时撤销全局活跃计数。
 * 业务背景：secretmem_file_create() 成功发布前计数加一，VFS release 在文件生命周期末尾
 * 配对，使休眠门禁反映仍存活的 secretmem 文件。
 * 入参：@inode/@file 均为 VFS 借用对象，本函数不使用也不释放它们。
 * 出参/返回：固定返回 0；secretmem_users 原子减一，无其他输出或 ownership 变化。
 * 注意事项：可并发执行、不需要外部锁；page cache/inode 的实际回收由 VFS 通用路径完成。
 */
static int secretmem_release(struct inode *inode, struct file *file)
{
	atomic_dec(&secretmem_users);
	return 0;
}

/*
 * 在 secretmem 文件 mmap 提交前强制共享、锁页和不可转储属性。
 * 业务背景：秘密页必须使用文件 page cache 在多个同文件映射间共享，且不能被换出或写入
 * core dump；该回调在 VMA 创建阶段一次性建立这些不变量。
 * 入参：@desc 是 mmap 构造中的输入输出描述符，包含 mm、长度和请求 flags；借用且非空。
 * 出参/返回：成功设置 VMA_LOCKED/VMA_DONTDUMP 与 secretmem_vm_ops 并返回 0；非共享请求
 * 返回 -EINVAL，锁页额度不足返回 -EAGAIN；失败时不会发布 VMA。
 * 注意事项：可睡眠；VMA_SHARED 或 VMA_MAYSHARE 至少一位必须存在，len 以字节计；
 * mlock_future_ok 按目标 mm 的 RLIMIT_MEMLOCK/权限验证，后续 munlock 也不能解除 secret 页。
 */
static int secretmem_mmap_prepare(struct vm_area_desc *desc)
{
	/* len 是待建 VMA 的字节长度，只在本次准备阶段有效。 */
	const unsigned long len = vma_desc_size(desc);

	/* 私有且不可共享的映射无法维持 secretmem 文件 page-cache 语义，直接拒绝。 */
	if (!vma_desc_test_any(desc, VMA_SHARED_BIT, VMA_MAYSHARE_BIT))
		return -EINVAL;

	/* 阶段 1：先声明强制锁页与禁止 dump，再验证增加后的锁页预算。 */
	vma_desc_set_flags(desc, VMA_LOCKED_BIT, VMA_DONTDUMP_BIT);
	if (!mlock_future_ok(desc->mm, /*is_vma_locked=*/ true, len))
		return -EAGAIN;
	/* 阶段 2：安装 fault 回调；VMA 发布后此表地址同时供 GUP/mlock 识别 secretmem。 */
	desc->vm_ops = &secretmem_vm_ops;

	return 0;
}

/*
 * 用 vm_ops 身份判断一个已发布 VMA 是否属于 secretmem。
 * 业务背景：GUP 慢路径据此拒绝 pin，mlock 据此阻止解锁；避免秘密页被旁路访问或换出。
 * 入参：@vma 是借用且生命周期已由调用者稳定的非空 VMA。出参/返回：操作表地址相等
 * 返回 true，否则 false；无副作用和 ownership 变化。注意事项：无锁、不睡眠，判断只在
 * VMA 有效窗口内可靠；关闭 CONFIG_SECRETMEM 时公开头提供恒 false 桩。
 */
bool vma_is_secretmem(struct vm_area_struct *vma)
{
	return vma->vm_ops == &secretmem_vm_ops;
}

/* secretmem 文件操作表把关闭计数与 mmap 准备接入 VFS；不提供 read/write 文件 I/O。 */
static const struct file_operations secretmem_fops = {
	.release	= secretmem_release,
	.mmap_prepare	= secretmem_mmap_prepare,
};

/*
 * 明确拒绝迁移 secretmem folio。
 * 业务背景：页已从 direct map 摘除，通用迁移无法在不建立普通内核映射的情况下安全复制
 * 秘密；aops 回调让 compaction/reclaim 保留原物理页。
 * 入参：@mapping、@dst、@src、@mode 均为迁移核心借用输入，本实现不访问或接管。
 * 出参/返回：固定 -EBUSY，源/目标 folio ownership 与内容不变。
 * 注意事项：可在 folio 锁定的迁移上下文调用；不睡眠。调用者可稍后重试但结果仍拒绝。
 */
static int secretmem_migrate_folio(struct address_space *mapping,
		struct folio *dst, struct folio *src, enum migrate_mode mode)
{
	return -EBUSY;
}

/*
 * 在 secretmem folio 离开 page cache、回到普通 allocator 前恢复 direct map 并擦除内容。
 * 业务背景：fault 路径撤销内核映射以保密；最终释放必须逆转该架构属性，否则后续普通
 * 页用户不可访问，同时必须清零避免秘密复用泄漏。
 * 入参：@folio 是 address_space 交来的、即将释放的 order-0 输入输出 folio，ownership
 * 仍归 page-cache 回收路径。出参/返回：无直接返回；恢复默认 direct map 并全页清零。
 * 注意事项：调用者保证 folio 已不可由用户映射并适合释放；操作不应失败，不能跳过清零。
 */
static void secretmem_free_folio(struct folio *folio)
{
	/* 顺序先恢复内核访问能力，再通过 folio helper 擦除整个页。 */
	set_direct_map_default_noflush(folio_page(folio, 0));
	folio_zero_segment(folio, 0, folio_size(folio));
}

/*
 * secretmem_aops 是 page cache 的全局类型标签与生命周期操作表：dirty 为无操作（无回写
 * backing store），free 负责恢复/擦除，migrate 永久拒绝。GUP-fast 也用表地址识别秘密页。
 */
const struct address_space_operations secretmem_aops = {
	.dirty_folio	= noop_dirty_folio,
	.free_folio	= secretmem_free_folio,
	.migrate_folio	= secretmem_migrate_folio,
};

/*
 * 串行处理 inode 属性变更，并把文件大小限制为“只能从零设定一次”。
 * 业务背景：用户用 ftruncate(fd, size) 声明可 fault 范围；一旦大小非零，再扩缩都可能
 * 与已撤销 direct map 的 page cache 页竞争，因此禁止，保持地址范围终生稳定。
 * 入参：@idmap 是借用的挂载 id 映射；@dentry 指向目标匿名 inode；@iattr 是借用属性
 * 请求，ia_valid 指明哪些字段有效。三者 ownership 均不转移。
 * 出参/返回：首次设置 size 或修改其他通用属性时返回 simple_setattr 结果；对非零 inode
 * 再请求 ATTR_SIZE 返回 -EINVAL。无输出参数，成功副作用由 simple_setattr 提交。
 * 注意事项：可睡眠；独占 invalidate lock 与 fault 的 shared 侧串行，所有出口都解锁。
 */
static int secretmem_setattr(struct mnt_idmap *idmap,
			     struct dentry *dentry, struct iattr *iattr)
{
	/* inode/mapping 从 dentry 借用；ia_valid 是本次请求位图，ret 贯穿统一解锁出口。 */
	struct inode *inode = d_inode(dentry);
	struct address_space *mapping = inode->i_mapping;
	unsigned int ia_valid = iattr->ia_valid;
	int ret;

	/* 阶段 1：独占锁冻结并发 fault/page-cache invalidation，再检查一次性 size 规则。 */
	filemap_invalidate_lock(mapping);

	if ((ia_valid & ATTR_SIZE) && inode->i_size)
		ret = -EINVAL;
	else
		ret = simple_setattr(idmap, dentry, iattr);

	/* 阶段 2：无论校验或通用 setattr 成败，均解除独占锁再返回。 */
	filemap_invalidate_unlock(mapping);

	return ret;
}

/* 匿名 secretmem inode 只覆盖 setattr，以执行一次性容量协议。 */
static const struct inode_operations secretmem_iops = {
	.setattr = secretmem_setattr,
};

/* fs_initcall 成功后 secretmem_mnt 持有内核内部伪文件系统挂载，生命周期持续到关机。 */
static struct vfsmount *secretmem_mnt;

/*
 * 构造一份全新的 secretmem 匿名 inode/file，并安装所有安全操作表。
 * 业务背景：memfd_secret syscall 需要先得到尚未发布到 fdtable 的 file；本函数在内部
 * secretmem mount 上创建 VFS 对象、配置不可回收 mapping，并把活跃计数与 release 配对。
 * 入参：@flags 是已由 syscall 校验的私有标志位，当前无有效 mode 且函数体不使用。
 * 出参/返回：成功返回 ownership 交给调用者的 file*；inode 或 file 分配失败返回错误指针，
 * 并释放已取得 inode。成功时 secretmem_users++，最终由 ->release 递减。
 * 注意事项：可睡眠；返回 file 尚未向用户可见。inode/mapping/aops/fops 必须在 FD_ADD
 * 发布前全部初始化，失败路径不能增加 users 计数。
 */
static struct file *secretmem_file_create(unsigned long flags)
{
	/* file/inode 分阶段取得；anon_name 只为安全钩子和诊断提供稳定类别名。 */
	struct file *file;
	struct inode *inode;
	const char *anon_name = "[secretmem]";

	/* 阶段 1：在内部 superblock 分配带 LSM 安全上下文的匿名 inode。 */
	inode = anon_inode_make_secure_inode(secretmem_mnt->mnt_sb, anon_name, NULL);
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	/* 阶段 2：file 成功后接管 inode 生命周期，并绑定只允许 mmap/release 的 fops。 */
	file = alloc_file_pseudo(inode, secretmem_mnt, "secretmem",
				 O_RDWR | O_LARGEFILE, &secretmem_fops);
	if (IS_ERR(file))
		goto err_free_inode;

	/* 阶段 3：fault 从高端用户内存分配；unevictable 禁止 reclaim 把秘密写到 swap。 */
	mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
	mapping_set_unevictable(inode->i_mapping);

	/* 在发布 file 前安装一次性 setattr 与 secret folio 生命周期操作。 */
	inode->i_op = &secretmem_iops;
	inode->i_mapping->a_ops = &secretmem_aops;

	/* pretend we are a normal file with zero size */
	/* 伪装成初始大小为零的普通文件，让 ftruncate/mmap 使用标准 VFS ABI。 */
	inode->i_mode |= S_IFREG;
	inode->i_size = 0;

	/* 对象已完整可用；计数必须晚于所有可失败步骤，并由 release 精确配对。 */
	atomic_inc(&secretmem_users);

	return file;

err_free_inode:
	/* alloc_file_pseudo 失败时 file 保存错误指针，inode ownership 仍由本层 iput。 */
	iput(inode);
	return file;
}

/*
 * 创建 secretmem 文件并原子安装到调用进程 fdtable。
 * 业务背景：这是用户态唯一创建入口；它先验证启动/架构能力和 ABI flags，再把完整 file
 * 交给 FD_ADD，后续用户通过 ftruncate+mmap 实际分配秘密页。
 * 入参：@flags 当前仅允许 O_CLOEXEC；其余 mode 位掩码为空。出参/返回：成功返回新 fd；
 * 功能关闭/架构不能改 direct map 返回 -ENOSYS，未知位 -EINVAL，用户计数溢出状态
 * -ENFILE，创建或 fd 分配错误原样返回。成功 file ownership 转移给 fdtable。
 * 注意事项：可睡眠；FD_ADD 负责 fd 分配、安装及失败时 file 回收。O_CLOEXEC 只控制
 * exec 时关闭，不传入 file->f_flags；创建后仍须先设置非零大小才能 fault。
 */
SYSCALL_DEFINE1(memfd_secret, unsigned int, flags)
{
	/* make sure local flags do not conflict with global fcntl.h */
	/* 编译期保证将来新增私有 mode 位不会与通用 O_CLOEXEC 冲突，避免一位两义。 */
	BUILD_BUG_ON(SECRETMEM_FLAGS_MASK & O_CLOEXEC);

	/* 阶段 1：配置或架构能力缺失时把 syscall 表现为未实现，而不是半安全降级。 */
	if (!secretmem_enable || !can_set_direct_map())
		return -ENOSYS;

	/* 严格拒绝所有未定义标志，给未来 ABI 扩展保留可判定空间。 */
	if (flags & ~(SECRETMEM_FLAGS_MASK | O_CLOEXEC))
		return -EINVAL;
	/* 原子计数若溢出符号位，不再创建文件，避免活跃门禁被错误解释为零。 */
	if (atomic_read(&secretmem_users) < 0)
		return -ENFILE;

	/* 阶段 2：先构造未发布 file，再由 FD_ADD 原子取得 fd、安装或在失败时 fput。 */
	return FD_ADD(flags & O_CLOEXEC, secretmem_file_create(flags));
}

/*
 * 为内部 secretmem 伪文件系统初始化 fs_context。
 * 业务背景：kern_mount() 通过 file_system_type 回调创建匿名 superblock，本函数选择
 * SECRETMEM_MAGIC 供 statfs/诊断识别，不解析任何用户挂载参数。
 * 入参：@fc 是 VFS 借用的输入输出上下文。出参/返回：init_pseudo 成功返回 0 并由 @fc
 * 持有 pseudo context，分配失败返回 -ENOMEM；本函数不直接释放 context。
 * 注意事项：仅启动期内核挂载调用、可睡眠；没有用户可见 mount 入口或额外选项。
 */
static int secretmem_init_fs_context(struct fs_context *fc)
{
	/* ctx 仅用于检测 init_pseudo 分配是否成功，ownership 已挂入 fc。 */
	struct pseudo_fs_context *ctx;

	ctx = init_pseudo(fc, SECRETMEM_MAGIC);
	if (!ctx)
		return -ENOMEM;

	return 0;
}

/*
 * 内部伪文件系统类型：init 创建带专用 magic 的匿名 superblock，kill_sb 在卸载时释放。
 * 静态对象由启动期 kern_mount 借用，成功挂载后通过 secretmem_mnt 长期持有。
 */
static struct file_system_type secretmem_fs = {
	.name		= "secretmem",
	.init_fs_context = secretmem_init_fs_context,
	.kill_sb	= kill_anon_super,
};

/*
 * 在 fs_initcall 阶段建立所有 secretmem file 共享的内部挂载。
 * 业务背景：syscall 创建匿名 inode 前必须已有 superblock/mount；不支持 direct-map 修改
 * 的架构或启动禁用时完全跳过，syscall 同样返回 ENOSYS。
 * 入参：无。出参/返回：禁用或成功返回 0；kern_mount 失败返回其负 errno。成功后
 * secretmem_mnt 持有挂载至系统结束，无本文件退出路径。
 * 注意事项：启动期串行、可睡眠；挂载发布是 syscall 可安全创建 inode 的前置条件。
 */
static int __init secretmem_init(void)
{
	/* 与 syscall 使用相同门禁，避免建立永远不会消费的半初始化文件系统。 */
	if (!secretmem_enable || !can_set_direct_map())
		return 0;

	/* 创建并持有内部挂载；错误指针不发布给后续创建路径。 */
	secretmem_mnt = kern_mount(&secretmem_fs);
	if (IS_ERR(secretmem_mnt))
		return PTR_ERR(secretmem_mnt);

	return 0;
}
fs_initcall(secretmem_init);
