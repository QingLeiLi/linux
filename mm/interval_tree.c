// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/interval_tree.c - interval tree for mapping->i_mmap
 *
 * Copyright (C) 2012, Michel Lespinasse <walken@google.com>
 */
/*
 * 本文件为 file-backed VMA 的 mapping->i_mmap 和匿名反向映射的 anon_vma
 * 建立增强红黑区间树。节点按起始页偏移排序，并缓存子树最大结束偏移，使缺页、
 * 反向映射、截断和 memory failure 能跳过不相交子树，而不必线性扫描全部 VMA。
 */

#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/rmap.h>
#include <linux/interval_tree_generic.h>

/*
 * vma_start_pgoff() - 提供 file-backed VMA 区间的起始页索引。
 * 业务背景：INTERVAL_TREE_DEFINE 需要统一 accessor 生成插入和相交查询代码。
 * 入参：@v 是树中借用 VMA，非 NULL；只读 vm_pgoff，不取得引用。
 * 出参/返回：返回文件内起始页偏移，单位为 PAGE_SIZE 页。
 * 注意事项：调用者须持 mapping->i_mmap_rwsem 等外层锁稳定 VMA 字段；不睡眠。
 */
static inline unsigned long vma_start_pgoff(struct vm_area_struct *v)
{
	return v->vm_pgoff;
}

/*
 * 为 struct vm_area_struct 生成 vma_interval_tree_{insert,remove,
 * subtree_search,iter_first,iter_next} 及增强回调。shared.rb 是嵌入节点，
 * shared.rb_subtree_last 缓存子树最大 vma_last_pgoff；“empty”参数表示生成的
 * API 非 static，供 i_mmap 用户跨文件调用。查询区间端点均为包含式页索引。
 * 插入/删除者持 mapping->i_mmap_rwsem；读者也在同一锁协议下借用返回 VMA。
 */
INTERVAL_TREE_DEFINE(struct vm_area_struct, shared.rb,
		     unsigned long, shared.rb_subtree_last,
		     vma_start_pgoff, vma_last_pgoff, /* empty */, vma_interval_tree)

/* Insert node immediately after prev in the interval tree */
/*
 * 把 @node 直接插到区间树中 @prev 的中序后继位置。该专用路径用于 fork 复制：
 * 两者起始 pgoff 相同，明确维持父 VMA 后紧跟子 VMA，避免通用插入重复搜索。
 */
/*
 * vma_interval_tree_insert_after() - 在同起点 @prev 后插入新的 file-backed VMA。
 *
 * 业务背景：dup_mmap() 已按父 VMA 顺序复制，利用已知前驱可更快链接 i_mmap，
 * 同时仍维护每级 rb_subtree_last 和红黑树平衡。
 * 入参：@node 是尚未入树、由其 mm 拥有的 VMA；@prev 是同 @root 中起始 pgoff
 * 相同的借用前驱；@root 是 mapping->i_mmap 的借用可写根。ownership 不转移。
 * 出参/返回：无直接返回值；@node 被发布到树中，祖先增强值与 cached root 更新。
 * 注意事项：调用者持 i_mmap 写锁及 dcache mmap 锁；不得重复插入。若起始偏移
 * 不同则 VM_BUG_ON_VMA。函数不分配、不睡眠，读者须由同一锁协议串行。
 */
void vma_interval_tree_insert_after(struct vm_area_struct *node,
				    struct vm_area_struct *prev,
				    struct rb_root_cached *root)
{
	/* link 指向最终父节点的 child 槽；parent 是其 VMA；last 是 node 的闭区间末页。 */
	struct rb_node **link;
	struct vm_area_struct *parent;
	unsigned long last = vma_last_pgoff(node);

	VM_BUG_ON_VMA(vma_start_pgoff(node) != vma_start_pgoff(prev), node);

	/* 快路径：prev 无右子树，中序后继位置就是其右 child。 */
	if (!prev->shared.rb.rb_right) {
		parent = prev;
		link = &prev->shared.rb.rb_right;
	} else {
		/*
		 * 否则后继是右子树最左节点；沿下行路径先把 subtree maximum 提升到
		 * 至少 @last，保证随后并发不可见前增强元数据已一致。
		 */
		parent = rb_entry(prev->shared.rb.rb_right,
				  struct vm_area_struct, shared.rb);
		/* 右子树根也将成为 node 的祖先，先把它的最大末页向上扩展。 */
		if (parent->shared.rb_subtree_last < last)
			parent->shared.rb_subtree_last = last;
		while (parent->shared.rb.rb_left) {
			/* 沿左链寻找最小后继，每个经过节点都必须包含新 node 的增强贡献。 */
			parent = rb_entry(parent->shared.rb.rb_left,
				struct vm_area_struct, shared.rb);
			if (parent->shared.rb_subtree_last < last)
				parent->shared.rb_subtree_last = last;
		}
		/* 最左节点没有左 child，新节点链接在这里才紧邻 prev。 */
		link = &parent->shared.rb.rb_left;
	}

	/* 初始化自身增强值，链接原始 rb 节点，再由 augmented 插入旋转并向上修复缓存。 */
	node->shared.rb_subtree_last = last;
	rb_link_node(&node->shared.rb, &parent->shared.rb, link);
	rb_insert_augmented(&node->shared.rb, &root->rb_root,
			    &vma_interval_tree_augment);
}

/*
 * avc_start_pgoff() - 返回 anon_vma_chain 所连 VMA 的起始页偏移。
 * 业务背景：匿名区间树节点是 AVC，但查询区间来自其 vma。
 * 入参：@avc 是 anon_vma 树内借用链节点，vma 生命周期由外层锁稳定。
 * 出参/返回：返回 vma->vm_pgoff，单位为页，无 ownership 变化。
 * 注意事项：调用者持 anon_vma 写/读锁；不睡眠。
 */
static inline unsigned long avc_start_pgoff(struct anon_vma_chain *avc)
{
	return vma_start_pgoff(avc->vma);
}

/*
 * avc_last_pgoff() - 返回 anon_vma_chain 所连 VMA 的包含式末页偏移。
 * 业务背景：与 avc_start_pgoff() 配对，为模板提供匿名映射区间端点。
 * 入参：@avc 是借用节点，非 NULL，不保存指针。
 * 出参/返回：返回 vma_last_pgoff(avc->vma)，单位为页。
 * 注意事项：VMA 边界更新前必须先从所有 anon_vma 树摘除，更新后再插回。
 */
static inline unsigned long avc_last_pgoff(struct anon_vma_chain *avc)
{
	return vma_last_pgoff(avc->vma);
}

/*
 * 为 anon_vma_chain 生成仅本文件可见的 __anon_vma_interval_tree_* 实现：rb 为
 * 嵌入节点，rb_subtree_last 为子树最大末页。公开 wrapper 在调试配置下额外
 * 维护端点快照。插入/删除受 anon_vma->rwsem 写侧保护，查询受其读侧保护；
 * 返回 AVC 均为锁窗口内借用指针。
 */
INTERVAL_TREE_DEFINE(struct anon_vma_chain, rb, unsigned long, rb_subtree_last,
		     avc_start_pgoff, avc_last_pgoff,
		     static inline, __anon_vma_interval_tree)

/*
 * anon_vma_interval_tree_insert() - 把 AVC 发布到对应 anon_vma 区间树。
 * 业务背景：fault/fork/split/remap 建立 VMA↔anon_vma 关系后，用树支持按页偏移
 * 找到所有可能映射同一匿名页的 VMA。
 * 入参：@node 是尚未入树的借用 AVC；@root 是其 anon_vma 的可写树根。
 * 出参/返回：无直接返回值；节点入树，调试配置同步缓存当前 VMA 两端点。
 * 注意事项：调用者持 anon_vma 写锁；不得重复插入，VMA 边界期间必须稳定。
 */
void anon_vma_interval_tree_insert(struct anon_vma_chain *node,
				   struct rb_root_cached *root)
{
#ifdef CONFIG_DEBUG_VM_RB
	/* 快照只用于以后发现“改 VMA 边界却未先摘树”的协议错误。 */
	node->cached_vma_start = avc_start_pgoff(node);
	node->cached_vma_last = avc_last_pgoff(node);
#endif
	__anon_vma_interval_tree_insert(node, root);
}

/*
 * anon_vma_interval_tree_remove() - 从 anon_vma 树摘除 AVC 并修复增强值。
 * 业务背景：VMA 销毁或边界改变前调用，阻止 rmap 继续按旧区间找到该关系。
 * 入参：@node 是已入 @root 的借用 AVC；@root 是可写树根。
 * 出参/返回：无直接返回值；节点不再可查询，storage ownership 不变。
 * 注意事项：调用者持 anon_vma 写锁；摘除后可更新 VMA 端点或最终释放 AVC。
 */
void anon_vma_interval_tree_remove(struct anon_vma_chain *node,
				   struct rb_root_cached *root)
{
	__anon_vma_interval_tree_remove(node, root);
}

/*
 * anon_vma_interval_tree_iter_first() - 查找首个与查询页区间相交的 AVC。
 * 业务背景：rmap、KSM、memory failure 以它开始剪枝遍历匿名映射关系。
 * 入参：@root 为借用树；@first/@last 是包含式查询页偏移且 first <= last。
 * 出参/返回：返回首个相交 AVC 的借用指针；无匹配返回 NULL。
 * 注意事项：调用者持 anon_vma 读/写锁并在锁内使用结果；不睡眠。
 */
struct anon_vma_chain *
anon_vma_interval_tree_iter_first(struct rb_root_cached *root,
				  unsigned long first, unsigned long last)
{
	return __anon_vma_interval_tree_iter_first(root, first, last);
}

/*
 * anon_vma_interval_tree_iter_next() - 继续查找下一个相交 AVC。
 * 业务背景：与 iter_first() 组成 anon_vma_interval_tree_foreach() 扫描协议。
 * 入参：@node 是上次返回且仍在树中的借用节点；@first/@last 必须与首查一致。
 * 出参/返回：返回下一相交 AVC，遍历结束返回 NULL，不取得引用。
 * 注意事项：整个迭代保持 anon_vma 锁，期间不得删除/重排当前树。
 */
struct anon_vma_chain *
anon_vma_interval_tree_iter_next(struct anon_vma_chain *node,
				 unsigned long first, unsigned long last)
{
	return __anon_vma_interval_tree_iter_next(node, first, last);
}

#ifdef CONFIG_DEBUG_VM_RB
/*
 * anon_vma_interval_tree_verify() - 检查 AVC 缓存端点仍匹配当前 VMA。
 * 业务背景：VMA merge/split/调整后用于发现遗漏的 pre-remove/post-insert 协议。
 * 入参：@node 是树内借用 AVC，非 NULL。
 * 出参/返回：无直接返回值；任一端点变化触发一次性 WARN，不修复树。
 * 注意事项：仅 CONFIG_DEBUG_VM_RB；调用者以相应锁稳定 node/vma，函数不睡眠。
 */
void anon_vma_interval_tree_verify(struct anon_vma_chain *node)
{
	WARN_ON_ONCE(node->cached_vma_start != avc_start_pgoff(node));
	WARN_ON_ONCE(node->cached_vma_last != avc_last_pgoff(node));
}
#endif
