/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Common Code for DAMON Sysfs Interface
 *
 * Author: SeongJae Park <sj@kernel.org>
 */
/*
 * DAMON sysfs 接口的共享契约：本头文件连接 sysfs.c 的上下文管理、
 * sysfs-schemes.c 的 DAMOS 目录实现与 DAMON core 对象。作者信息保留原样；
 * “Common Code”具体指两编译单元共同使用的对象布局、kobject 类型和转换接口。
 */

#include <linux/damon.h>
#include <linux/kobject.h>

/*
 * damon_sysfs_lock 串行化可写 sysfs 操作与 DAMON 回调对同一目录快照的访问。
 * 它由 sysfs-common.c 定义；调用者通常用 mutex_trylock()，忙时向用户返回
 * -EBUSY。互斥量只保护 sysfs 侧对象关系，不替代 DAMON core 自己的同步。
 */
extern struct mutex damon_sysfs_lock;

/*
 * damon_sysfs_ul_range 表示一个含 min/max 文件的 sysfs 范围目录。
 * kobj 嵌入对象并以引用计数决定最终释放；min/max 是用户配置的无符号长整型
 * 端点，具体单位由父目录决定。对象由 alloc 创建、kobject_init_and_add 发布，
 * 最后经 ktype.release 回收；字段访问依赖外层 sysfs 串行化。
 */
struct damon_sysfs_ul_range {
	struct kobject kobj;
	unsigned long min;
	unsigned long max;
};

/*
 * damon_sysfs_ul_range_alloc() - 分配并初始化尚未发布的范围目录对象。
 * 业务背景：父目录在构建访问次数、年龄或区域大小范围时先取得容器，再交给
 * kobject_init_and_add() 建立 sysfs 节点。
 * 入参：min、max 分别是初始下/上界，单位和合法顺序由具体父属性定义；均为值输入。
 * 出参/返回：成功返回由调用者独占的新对象；内存不足返回 NULL。kobject 此时仅清零，
 * 尚无 sysfs 引用；成功发布后释放责任转交 kobject_put()/ktype.release。
 * 注意事项：GFP_KERNEL 分配可睡眠；函数不取 damon_sysfs_lock，也不验证 min <= max。
 */
struct damon_sysfs_ul_range *damon_sysfs_ul_range_alloc(
		unsigned long min,
		unsigned long max);
/*
 * damon_sysfs_ul_range_release() - 在范围 kobject 最后一个引用消失时释放容器。
 * 业务背景：这是 damon_sysfs_ul_range_ktype.release 的生命周期终点，由 kobject
 * core 调用，而不是普通调用者直接销毁对象。
 * 入参：kobj 是嵌入有效 range 的输入指针；调用获得其最终引用的释放权，禁止为 NULL。
 * 出参/返回：无直接返回值和输出参数；释放包含该 kobj 的 range 内存。
 * 注意事项：到达这里时目录已不可再被新查找；调用者不得随后解引用 kobj/range。
 */
void damon_sysfs_ul_range_release(struct kobject *kobj);

/*
 * 范围目录的 kobject 操作表：把 min/max 默认属性、通用 sysfs_ops 与最终 release
 * 绑定为一个不可变全局描述符；kobject_init_and_add() 借用它且不会取得释放责任。
 */
extern const struct kobj_type damon_sysfs_ul_range_ktype;

/*
 * schemes directory
 */
/*
 * 以下对象实现 schemes 目录：目录拥有按索引排列的 scheme 子目录。数组元素在
 * 创建成功后由各自 kobject 引用管理，容器负责摘除子目录并释放指针数组。
 */

/*
 * damon_sysfs_schemes 是 sysfs 配置快照中的 schemes/ 容器。kobj 控制容器生命期；
 * schemes_arr 拥有 nr 个 damon_sysfs_scheme 指针槽，nr 同时是有效元素数和用户
 * 可见编号上界。结构修改由 damon_sysfs_lock 串行化，运行中读取还需遵守调用链约束。
 */
struct damon_sysfs_schemes {
	struct kobject kobj;
	struct damon_sysfs_scheme **schemes_arr;
	int nr;
};

/*
 * damon_sysfs_schemes_alloc() - 分配一个空的 schemes 容器。
 * 业务背景：sysfs.c 构造 DAMON context 目录时先创建容器，再发布其 kobject。
 * 入参：无。
 * 出参/返回：成功返回全字段清零、由调用者独占的对象；内存不足返回 NULL。
 * 注意事项：分配可睡眠且不取锁；返回对象尚未发布，失败没有残留资源。
 */
struct damon_sysfs_schemes *damon_sysfs_schemes_alloc(void);
/*
 * damon_sysfs_schemes_rm_dirs() - 摘除并释放容器拥有的全部 scheme 子目录。
 * 业务背景：调整 nr_schemes、构造失败回滚或父目录销毁时统一执行逆序清理。
 * 入参：schemes 是不可为 NULL 的输入输出借用指针；进入时拥有 schemes_arr 及 nr
 * 个子 kobject，返回时 nr=0、schemes_arr=NULL，容器 kobj 本身仍由调用者持有。
 * 出参/返回：无直接返回值；每个子对象经 kobject_put() 延迟到最后引用时释放。
 * 注意事项：调用者须用 damon_sysfs_lock 排除并发目录重配；函数可触发释放路径。
 */
void damon_sysfs_schemes_rm_dirs(struct damon_sysfs_schemes *schemes);

/* schemes 容器的不可变 kobject 类型，绑定 nr_schemes 属性和最终容器释放回调。 */
extern const struct kobj_type damon_sysfs_schemes_ktype;

/*
 * damon_sysfs_add_schemes() - 把 sysfs 配置快照转换为 DAMON core 的 DAMOS 链表。
 * 业务背景：启动/提交 DAMON context 时，由 sysfs.c 在已构造 ctx 上逐项创建 scheme。
 * 入参：ctx 是输入输出借用指针，成功后拥有新 scheme；sysfs_schemes 是只读借用的
 * 配置容器，二者均不可为 NULL，索引顺序必须对应。
 * 出参/返回：全部成功返回 0；任一分配/转换失败返回 -ENOMEM，并销毁 ctx 当前链表
 * 中的 scheme，因而调用者不能假定保留部分成功结果。
 * 注意事项：可睡眠、不在内部取 damon_sysfs_lock；调用者须稳定 sysfs 快照，且应
 * 在尚未由 DAMON 工作线程并发使用的 ctx 上调用。
 */
int damon_sysfs_add_schemes(struct damon_ctx *ctx,
		struct damon_sysfs_schemes *sysfs_schemes);

/*
 * damon_sysfs_schemes_update_stats() - 将 core 统计快照复制回对应 sysfs 节点。
 * 业务背景：用户请求更新统计时，sysfs.c 按 scheme 顺序同步运行结果供 show 读取。
 * 入参：sysfs_schemes 是被更新的输入输出借用指针；ctx 是提供 scheme->stat 的只读
 * 借用指针，均不可为 NULL，匹配以列表/数组索引为准。
 * 出参/返回：无直接返回值；只更新仍存在的 stats 字段，不改变引用或 core 状态。
 * 注意事项：可睡眠性取决于上层；函数不取锁，调用者须稳定两侧列表。用户已删除
 * 某目录时会在数组末端停止，避免越界。
 */
void damon_sysfs_schemes_update_stats(
		struct damon_sysfs_schemes *sysfs_schemes,
		struct damon_ctx *ctx);

/*
 * damos_sysfs_populate_region_dir() - 把一次 DAMOS walk 结果累计/发布到 tried_regions。
 * 业务背景：DAMON 回调逐区域上报时先按 core scheme 找到同索引 sysfs scheme，
 * 再累计总字节；非仅统计模式还创建带 kobject 的区域子目录。
 * 入参：sysfs_schemes 为输入输出借用容器；ctx、t、r、s 均为回调期间有效的借用
 * 指针，分别表示上下文、目标、区域和命中的方案；total_bytes_only 选择只累计字节；
 * sz_filter_passed 是该区域通过方案过滤器的字节数。
 * 出参/返回：无直接返回值；可能增加 total_bytes、regions_list 和 nr_regions；分配
 * 或发布失败时静默保留已累计总量，不转移 DAMON 对象 ownership。
 * 注意事项：调用点必须已持 damon_sysfs_lock；用户删除对应目录时安全返回。
 */
void damos_sysfs_populate_region_dir(struct damon_sysfs_schemes *sysfs_schemes,
		struct damon_ctx *ctx, struct damon_target *t,
		struct damon_region *r, struct damos *s,
		bool total_bytes_only, unsigned long sz_filter_passed);

/*
 * damon_sysfs_schemes_clear_regions() - 清空所有方案已尝试区域的 sysfs 快照。
 * 业务背景：开始新一轮快照或处理用户 clear 命令时，删除旧子目录并把总字节归零。
 * 入参：sysfs_schemes 是不可为 NULL 的输入输出借用指针，调用期间数组须稳定。
 * 出参/返回：当前实现总返回 0；副作用是各 tried_regions 目录被摘除且统计清零。
 * 注意事项：函数不自行取锁；调用者须以 damon_sysfs_lock 排除 walk 回调并发填充。
 */
int damon_sysfs_schemes_clear_regions(
		struct damon_sysfs_schemes *sysfs_schemes);

/*
 * damos_sysfs_set_quota_scores() - 把 sysfs quota goals 提交给运行中的 core schemes。
 * 业务背景：用户发出 commit_schemes_quota_goals 命令时，临时构造 goal 链表并调用
 * damos_commit_quota_goals() 替换每个方案的目标。
 * 入参：sysfs_schemes 提供只读配置，ctx 的 scheme quota 是输入输出；均为借用且
 * 不可为 NULL，用户删除目录时只处理仍能按索引匹配的前缀。
 * 出参/返回：成功返回 0；分配或 core 提交失败返回负 errno。临时 goal 总会销毁，
 * 已成功提交到较早 scheme 的更新不会由本函数回滚。
 * 注意事项：函数可睡眠且不取全局锁；调用者负责阻止目录和 scheme 链并发变化。
 */
int damos_sysfs_set_quota_scores(struct damon_sysfs_schemes *sysfs_schemes,
		struct damon_ctx *ctx);

/*
 * damos_sysfs_update_effective_quotas() - 回写各 core scheme 实际生效的字节配额。
 * 业务背景：配额调优后，sysfs 命令读取 scheme->quota.esz 并更新 effective_bytes。
 * 入参：sysfs_schemes 为被更新的输入输出借用容器；ctx 为只读借用的 core 上下文。
 * 出参/返回：无直接返回值；仅更新匹配前缀的 effective_sz，不改变引用或 core 配额。
 * 注意事项：不自行取锁；调用者须稳定两侧，目录少于 core scheme 时安全停止。
 */
void damos_sysfs_update_effective_quotas(
		struct damon_sysfs_schemes *sysfs_schemes,
		struct damon_ctx *ctx);

/*
 * damon_sysfs_memcg_path_to_id() - 把用户提供的 cgroup 路径解析为 DAMOS memcg ID。
 * 业务背景：构造 memcg 类型过滤器时，core 需要稳定的数值 ID 而 sysfs 接收路径。
 * 入参：memcg_path 是 NUL 结尾的只读借用字符串，不可为 NULL；id 是不可为 NULL
 * 的输出指针，仅成功时写入在线 memcg 的 u64 ID，ownership 均不转移。
 * 出参/返回：找到返回 0；空路径指针或未找到返回 -EINVAL；临时 PATH_MAX 缓冲区
 * 分配失败返回 -ENOMEM。
 * 注意事项：迭代和分配可睡眠；离线 memcg 被跳过。CONFIG_MEMCG=n 时不会匹配，
 * 因而非空输入最终也返回 -EINVAL。
 */
int damon_sysfs_memcg_path_to_id(char *memcg_path, u64 *id);
