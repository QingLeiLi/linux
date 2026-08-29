// SPDX-License-Identifier: GPL-2.0

#ifndef _LINUX_MM_SLOT_H
#define _LINUX_MM_SLOT_H

#include <linux/hashtable.h>
#include <linux/slab.h>

/*
 * struct mm_slot - hash lookup from mm to mm_slot
 * @hash: link to the mm_slots hash list
 * @mm_node: link into the mm_slots list
 * @mm: the mm that this information is valid for
 */
/*
 * struct mm_slot 是 KSM 与 khugepaged 共用的“被扫描 mm”基础索引：hash 让退出
 * 路径按 mm_struct 快速定位，mm_node 让后台线程按顺序遍历。hash 是哈希桶中的
 * 嵌入节点，mm_node 是扫描链表节点，mm 是本槽描述的借用指针。上层在插入后
 * 另行 mmgrab()、摘除后 mmdrop()，因此本结构本身不拥有引用；两种链接的增删和
 * mm 有效期分别由 ksm_mmlist_lock 或 khugepaged_mm_lock 及该外部引用共同保护。
 */
struct mm_slot {
	struct hlist_node hash;
	struct list_head mm_node;
	struct mm_struct *mm;
};

/*
 * mm_slot_entry() - 从嵌入的基础槽地址恢复上层私有对象。
 * 业务背景：KSM 把 struct mm_slot 嵌入 ksm_mm_slot，扫描/查找得到基础指针后需
 * 回到含 rmap_list 等私有字段的容器；khugepaged 直接使用基础槽则通常不需要它。
 * 入参：ptr 是指向 type.member 的借用指针；type 是容器类型记号；member 是嵌入
 * 成员名。三者必须真实对应，宏不做运行期检查。
 * 出参/返回：返回指向同一存储的 type * 借用指针，不增引用、不分配内存。
 * 注意事项：这是 container_of() 的薄包装；槽被摘除并释放后，返回指针立即失效，
 * 调用者必须仍持所属扫描锁或其他生命周期保证。
 */
#define mm_slot_entry(ptr, type, member) \
	container_of(ptr, type, member)

/*
 * mm_slot_alloc() - 从调用者提供的专用 slab cache 分配零填充槽对象。
 * 业务背景：KSM/khugepaged 注册 mm 时先分配各自大小的对象，随后初始化私有字段
 * 并通过 mm_slot_insert() 发布公共索引。
 * 入参：cache 是调用者持有、可为 NULL 的 kmem_cache 借用指针；cache 的对象大小
 * 决定实际返回的是基础槽还是包含它的更大容器。
 * 出参/返回：成功返回由调用者独占且全零的对象；cache 为 NULL 或分配失败返回
 * NULL。成功对象尚未入 hash/list，也尚未持有 mm 引用。
 * 注意事项：GFP_KERNEL 分配可睡眠，不能在自旋锁内调用；调用者须用同一 cache
 * 交给 mm_slot_free()，并在发布前补齐字段与 mm 生命周期引用。
 */
static inline void *mm_slot_alloc(struct kmem_cache *cache)
{
	if (!cache)	/* initialization failed */
		/* cache 初始化失败时无法安全分配；把失败原样传给上层注册路径处理。 */
		return NULL;
	return kmem_cache_zalloc(cache, GFP_KERNEL);
}

/*
 * mm_slot_free() - 把已从所有索引摘除的槽对象归还原 slab cache。
 * 业务背景：注册失败回滚、mm 退出或扫描线程完成延迟清理后调用它结束槽生命周期。
 * 入参：cache 是分配该对象的不可为 NULL 借用 cache；objp 是待释放对象的拥有指针，
 * 必须来自 mm_slot_alloc(cache) 且不再位于 hash/mm_node 链表中。
 * 出参/返回：无直接返回值；释放对象 ownership，但不自动 mmdrop(objp->mm)。
 * 注意事项：调用者必须先在对应自旋锁保护下摘除两个链接，再在协议规定位置配对
 * mmdrop()；释放后任何基础槽或上层容器指针均不可继续使用。
 */
static inline void mm_slot_free(struct kmem_cache *cache, void *objp)
{
	kmem_cache_free(cache, objp);
}

/*
 * mm_slot_lookup() - 在指定哈希表中按 mm_struct 地址查找槽。
 * 业务背景：KSM/khugepaged 的退出路径需要 O(桶长度) 找回注册记录，而扫描顺序
 * 仍由独立 mm_node 链表维护。
 * 入参：_hashtable 是调用者拥有的 hash table 表达式；_mm 是不可为 NULL 的借用
 * mm 指针并作为指针值哈希键。宏参数仅应传无副作用表达式。
 * 出参/返回：找到返回匹配 tmp_slot->mm 的借用槽，未找到返回 NULL；不增 mm/槽引用。
 * 注意事项：GNU statement expression 让多语句宏产生一个值；宏不加锁，调用者必须
 * 持 ksm_mmlist_lock 或 khugepaged_mm_lock，且只能在该保护/引用有效期内用返回值。
 */
#define mm_slot_lookup(_hashtable, _mm) 				       \
({									       \
	struct mm_slot *tmp_slot, *mm_slot = NULL;			       \
									       \
	/* 指针整数值只用于选桶；桶内仍比较完整指针以处理哈希碰撞。 */ \
	hash_for_each_possible(_hashtable, tmp_slot, hash, (unsigned long)_mm) \
		if (_mm == tmp_slot->mm) {				       \
			mm_slot = tmp_slot;				       \
			break;						       \
		}							       \
									       \
	mm_slot;							       \
})

/*
 * mm_slot_insert() - 绑定 mm 并把已初始化槽发布到哈希索引。
 * 业务背景：上层分配槽、排除重复注册后，在扫描锁内先建立按 mm 查找入口，再把
 * mm_node 接到扫描链表，最后在锁外 mmgrab() 完成长期生命周期协议。
 * 入参：_hashtable 是目标哈希表；_mm 是写入槽的借用 mm 指针；_mm_slot 是未入
 * 哈希且由调用者拥有的输入输出槽，三者不可为 NULL，宏参数不应有副作用。
 * 出参/返回：无供调用者使用的返回值；副作用是 _mm_slot->mm=_mm 并发布 hash 节点，
 * 不增加 mm 引用，也不插入 mm_node。
 * 注意事项：宏不检测重复项、不初始化链表节点且不加锁；调用者须持所属扫描锁，
 * 并保证同一 mm 只注册一次，否则退出查找与引用配对会失去唯一性。
 */
#define mm_slot_insert(_hashtable, _mm, _mm_slot)			       \
({									       \
	_mm_slot->mm = _mm;						       \
	hash_add(_hashtable, &_mm_slot->hash, (unsigned long)_mm);	       \
})

#endif /* _LINUX_MM_SLOT_H */
/* 结束本头文件防重复包含范围；对应开头的 _LINUX_MM_SLOT_H 条件。 */
