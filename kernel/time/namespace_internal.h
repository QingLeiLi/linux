/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _TIME_NAMESPACE_INTERNAL_H
#define _TIME_NAMESPACE_INTERNAL_H

#include <linux/mutex.h>

struct time_namespace;

/*
 * Protects possibly multiple offsets writers racing each other
 * and tasks entering the namespace.
 */
/*
 * 该锁把多个 offset 写者以及任务首次进入命名空间的冻结动作串行起来：调用者必须在同一临界区内检查
 * `ns->frozen`、更新 offsets，并在需要时提交 VDSO 快照，避免用户态看到字段组合不一致的时间视图。
 */
extern struct mutex timens_offset_lock;

#ifdef CONFIG_TIME_NS_VDSO
/*
 * 为新命名空间分配专属 vvar 页；成功后页面由 time_namespace 持有，失败返回负 errno，调用方继续执行
 * clone 的逆序回滚。释放接口与之配对，并由 namespace 最终销毁路径调用。
 */
int timens_vdso_alloc_vvar_page(struct time_namespace *ns);
void timens_vdso_free_vvar_page(struct time_namespace *ns);
#else /* !CONFIG_TIME_NS_VDSO */
/*
 * 未启用 VDSO 时间命名空间时没有额外页面需要分配；返回 0 让通用 clone 路径保持成功语义，也不改变 ns。
 */
static inline int timens_vdso_alloc_vvar_page(struct time_namespace *ns)
{
	return 0;
}
/*
 * 与无操作的分配 stub 配对；namespace 销毁路径可无条件调用，ns 的生命周期和内容均不受影响。
 */
static inline void timens_vdso_free_vvar_page(struct time_namespace *ns)
{
}
#endif /* CONFIG_TIME_NS_VDSO */

#endif /* _TIME_NAMESPACE_INTERNAL_H */
