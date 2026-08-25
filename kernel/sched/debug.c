// SPDX-License-Identifier: GPL-2.0-only
/*
 * kernel/sched/debug.c
 *
 * Print the CFS rbtree and other debugging details
 *
 * Copyright(C) 2007, Red Hat, Inc., Ingo Molnar
 */
/*
 * 本文件是调度器的只读诊断面与少量 debugfs 控制面。输出路径把同一套
 * SEQ_printf 同时用于 seq_file 和 SysRq 控制台；展示的是逐字段即时快照，
 * 除显式持 rq 锁复制的红黑树边界外，不保证所有字段来自同一时刻。
 * 写路径先解析并验证用户输入，再在 CPU hotplug、domain mutex 或 rq 锁的
 * 对应保护下发布配置；失败必须保持旧配置或返回明确 errno。
 */
#include <linux/debugfs.h>
#include <linux/nmi.h>
#include <linux/log2.h>
#include "sched.h"

/* SEQ_printf 以 m 是否为空选择 seq_file 或控制台，调用者无需维护两套格式字符串。 */
/*
 * This allows printing both to /sys/kernel/debug/sched/debug and
 * to the console
 */
#define SEQ_printf(m, x...)			\
 do {						\
	if (m)					\
		seq_printf(m, x);		\
	else					\
		pr_cont(x);			\
 } while (0)

/*
 * Ease the printing of nsec fields:
 */
/* 将有符号纳秒的整数微秒部分取出，供 SPLIT_NS 保持负值打印符号。 */
/*
 * 业务背景：调度诊断把有符号纳秒拆成“微秒整数.六位余数”，负值的符号必须只打印一次。
 * 入参：nsec 按位承载 signed long long 纳秒，可覆盖正负 64 位范围，纯输入且无 ownership。
 * 出参/返回：返回带符号的整数微秒部分；无输出参数或状态副作用。
 * 注意事项：参数类型虽为 unsigned 仍按 signed 解释；do_div 修改局部副本并截断不足一微秒部分。
 */
static long long nsec_high(unsigned long long nsec)
{
	if ((long long)nsec < 0) {
		nsec = -nsec;
		do_div(nsec, 1000000);
		return -nsec;
	}
	do_div(nsec, 1000000);

	return nsec;
}

/* 返回纳秒绝对值除以一百万后的六位余数，与 nsec_high() 配对显示。 */
/*
 * 业务背景：SPLIT_NS 需要绝对值的小数微秒部分，与 nsec_high() 的符号化整数部分拼接。
 * 入参：nsec 按位承载 signed long long 纳秒，纯输入且无 ownership。
 * 出参/返回：返回 0..999999 的纳秒余数；无输出参数或状态副作用。
 * 注意事项：负值先取绝对值；最小负数按无符号运算处理，结果只用于诊断格式化。
 */
static unsigned long nsec_low(unsigned long long nsec)
{
	/* 小数部分始终取绝对值，使负号只由 nsec_high() 的整数部分承担。 */
	if ((long long)nsec < 0)
		nsec = -nsec;

	return do_div(nsec, 1000000);
}

#define SPLIT_NS(x) nsec_high(x), nsec_low(x)

/* features.h 首次展开为稳定名称表，后续同一索引用于位图和 static key。 */
#define SCHED_FEAT(name, enabled)	\
	#name ,

static const char * const sched_feat_names[] = {
#include "features.h"
};

#undef SCHED_FEAT

/* 将所有调度 feature 及 NO_ 前缀写到 @m；@v 未使用，成功恒返回 0。 */
/*
 * 业务背景：debugfs sched/features 读取需把位图转换为可再次写入的 feature 名称列表。
 * 入参：m 是不可空、seq_file 拥有的输出上下文；v 是 seq 接口未使用的借用参数，可为 NULL。
 * 出参/返回：成功恒返回 0；向 m 追加全部 feature 文本，无独立输出参数或 ownership 变化。
 * 注意事项：无锁读取 sysctl 位图，列表可能跨并发写入形成诊断快照；seq_file 负责缓冲错误。
 */
static int sched_feat_show(struct seq_file *m, void *v)
{
	int i;

	/* 位图中清零的 feature 以 NO_ 前缀输出，文本可直接回写控制面。 */
	for (i = 0; i < __SCHED_FEAT_NR; i++) {
		if (!(sysctl_sched_features & (1UL << i)))
			seq_puts(m, "NO_");
		seq_printf(m, "%s ", sched_feat_names[i]);
	}
	/* 所有名称位于一行，最终换行形成一次完整的 seq 记录。 */
	seq_puts(m, "\n");

	return 0;
}

/* JUMP_LABEL 分支再次展开 features.h，为每个名称构造初始真假 static key。 */
#ifdef CONFIG_JUMP_LABEL

#define jump_label_key__true  STATIC_KEY_INIT_TRUE
#define jump_label_key__false STATIC_KEY_INIT_FALSE

#define SCHED_FEAT(name, enabled)	\
	jump_label_key__##enabled ,

struct static_key sched_feat_keys[__SCHED_FEAT_NR] = {
#include "features.h"
};

#undef SCHED_FEAT

/* CPU 热插拔读锁下关闭第 @i 个 static key，使 feature 位与跳转标签一致。 */
/*
 * 业务背景：启用 jump label 时，关闭 feature 不仅要改位图，还要补丁化所有 CPU 的静态分支。
 * 入参：i 是 0..__SCHED_FEAT_NR-1 的 feature 索引，纯输入。
 * 出参/返回：无直接返回值、无输出参数；关闭对应 sched_feat_keys[i]。
 * 注意事项：调用者必须持 cpus_read_lock；索引越界会访问错误 static key，操作可能同步且不可任意嵌套。
 */
static void sched_feat_disable(int i)
{
	static_key_disable_cpuslocked(&sched_feat_keys[i]);
}

/* CPU 热插拔读锁下开启第 @i 个 static key；无直接返回值。 */
/*
 * 业务背景：开启 feature 时要让编译出的静态分支与 sysctl 位图同步生效。
 * 入参：i 是 0..__SCHED_FEAT_NR-1 的 feature 索引，纯输入。
 * 出参/返回：无直接返回值、无输出参数；开启对应 sched_feat_keys[i]。
 * 注意事项：调用者必须持 cpus_read_lock；static-key 补丁可能同步，索引越界属于严重误用。
 */
static void sched_feat_enable(int i)
{
	static_key_enable_cpuslocked(&sched_feat_keys[i]);
}
#else /* !CONFIG_JUMP_LABEL: */
/* 无 JUMP_LABEL 时位图本身就是唯一状态，静态键同步为空操作。 */
/*
 * 业务背景：未配置 JUMP_LABEL 时保留统一关闭调用接口，实际 feature 状态只由位图控制。
 * 入参：i 是未使用的 feature 索引输入。
 * 出参/返回：无直接返回值、无输出参数或状态副作用。
 * 注意事项：仅配置替代实现；不校验索引，也不提供静态分支同步保证。
 */
static void sched_feat_disable(int i) { };
/* 与上面的关闭 stub 对称，@i 不产生副作用。 */
/*
 * 业务背景：未配置 JUMP_LABEL 时为 feature 开启路径提供同签名空实现。
 * 入参：i 是未使用的 feature 索引输入。
 * 出参/返回：无直接返回值、无输出参数或状态副作用。
 * 注意事项：仅配置替代实现；真正状态已由调用者更新 sysctl_sched_features。
 */
static void sched_feat_enable(int i) { };
#endif /* !CONFIG_JUMP_LABEL */

/* 解析可选 NO_ 的 feature 名，更新位图及静态键；未知名称返回负 errno。 */
/*
 * 业务背景：debugfs 写入以 NAME/NO_NAME 文本原子切换调度 feature 及其可选 jump label。
 * 入参：cmp 是不可空、NUL 结尾且允许函数推进的可写借用字符串，内容为 feature 名。
 * 出参/返回：匹配并更新成功返回 0；未知名称返回 match_string 的负 errno；无输出参数。
 * 注意事项：调用者持 inode 锁和 cpus_read_lock；位图先改再更新 static key，锁外并发会破坏一致性。
 */
static int sched_feat_set(char *cmp)
{
	int i;
	int neg = 0;

	/* NO_ 只改变目标布尔值，实际匹配始终使用 features.h 生成的裸名称。 */
	if (strncmp(cmp, "NO_", 3) == 0) {
		neg = 1;
		cmp += 3;
	}

	i = match_string(sched_feat_names, __SCHED_FEAT_NR, cmp);
	if (i < 0)
		return i;

	/* 位图与 static key 在调用者锁区间中按同一方向更新，避免控制面长期分裂。 */
	if (neg) {
		sysctl_sched_features &= ~(1UL << i);
		sched_feat_disable(i);
	} else {
		sysctl_sched_features |= (1UL << i);
		sched_feat_enable(i);
	}

	return 0;
}

/*
 * 从用户缓冲区截取至多 63 字节并原子更新 feature 位与 static key；
 * inode 锁串行写者，cpus_read_lock 稳定 static-key CPU 集，成功返回消费字节数。
 */
/*
 * 业务背景：sched/features debugfs 写接口要安全复制用户文本并串行更新位图与 jump label。
 * 入参：filp 是不可空借用文件；ubuf 是用户态只读缓冲区；cnt 是输入字节数；ppos 是不可空输入/输出偏移。
 * 出参/返回：成功返回实际消费的 0..63 字节并推进 *ppos；复制失败 -EFAULT，名称错误返回负 errno。
 * 注意事项：可睡眠并获取 CPU hotplug 读锁/inode 锁；超过 63 字节被截断，用户缓冲 ownership 不变。
 */
static ssize_t
sched_feat_write(struct file *filp, const char __user *ubuf,
		size_t cnt, loff_t *ppos)
{
	char buf[64];
	char *cmp;
	int ret;
	struct inode *inode;

	/* 固定栈缓冲只接受 63 字节并保留末尾 NUL，长输入按实际截断量消费。 */
	if (cnt > 63)
		cnt = 63;

	if (copy_from_user(buf, ubuf, cnt))
		return -EFAULT;

	buf[cnt] = 0;
	cmp = strstrip(buf);

	/* 用户复制和清理结束后才进入全局锁区，失败不会持锁返回。 */
	/* Ensure the static_key remains in a consistent state */
	inode = file_inode(filp);
	cpus_read_lock();
	inode_lock(inode);
	ret = sched_feat_set(cmp);
	inode_unlock(inode);
	cpus_read_unlock();
	if (ret < 0)
		return ret;

	/* 只有 feature 与跳转标签同步成功后才推进 VFS 文件偏移。 */
	*ppos += cnt;

	return cnt;
}

/* 为 sched_features 建立 single_open seq_file；返回分配结果或负 errno。 */
/*
 * 业务背景：debugfs features 文件打开时需要把 show 回调装入 single_open 管理的 seq_file。
 * 入参：inode 是未使用的借用 inode；filp 是不可空、VFS 拥有的输入/输出文件对象。
 * 出参/返回：成功返回 0 并初始化 filp->private_data；失败返回 single_open 的负 errno。
 * 注意事项：可分配并睡眠；资源 ownership 交给 filp，release 必须由 single_release 配对。
 */
static int sched_feat_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, sched_feat_show, NULL);
}

/* fops 用 single_release 与 open 配对，读写共享同一 inode 级 feature 控制面。 */
static const struct file_operations sched_feat_fops = {
	.open		= sched_feat_open,
	.write		= sched_feat_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* 写入 tunable scaling 枚举；越界或全局重算失败返回 -EINVAL，成功消费 @cnt。 */
/*
 * 业务背景：用户可经 debugfs 选择 scheduler tunable 随 CPU 数量的缩放策略并触发全局重算。
 * 入参：filp 是未使用借用文件；ubuf 是用户只读文本；cnt 是字节数；ppos 是不可空输入/输出偏移。
 * 出参/返回：成功返回 cnt 并推进 *ppos；解析错误原样返回，越界或重算失败返回 -EINVAL。
 * 注意事项：全局值在 sched_update_scaling 失败前已写入，失败并非完全无副作用；用户指针 ownership 不变。
 */
static ssize_t sched_scaling_write(struct file *filp, const char __user *ubuf,
				   size_t cnt, loff_t *ppos)
{
	unsigned int scaling;
	int ret;

	/* 解析阶段只产生局部枚举，失败和越界都不会进入全局重算。 */
	ret = kstrtouint_from_user(ubuf, cnt, 10, &scaling);
	if (ret)
		return ret;

	if (scaling >= SCHED_TUNABLESCALING_END)
		return -EINVAL;

	sysctl_sched_tunable_scaling = scaling;
	/* 重算失败会报告 -EINVAL，但全局枚举已经写入，调用者需按该实际副作用理解。 */
	if (sched_update_scaling())
		return -EINVAL;

	*ppos += cnt;
	return cnt;
}

/* 输出当前 scaling 枚举；读取是瞬时值，不冻结并发写者。 */
/*
 * 业务背景：tunable_scaling debugfs 读取要向用户展示当前全局枚举数值。
 * 入参：m 是不可空输出 seq_file；v 是未使用的借用 seq 参数，可为 NULL。
 * 出参/返回：成功恒返回 0，并向 m 输出十进制值和换行；无输出参数或 ownership 变化。
 * 注意事项：无锁快照可能紧随返回被并发写者替换；seq_file 负责缓冲和截断处理。
 */
static int sched_scaling_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", sysctl_sched_tunable_scaling);
	return 0;
}

/* 把 scaling show 回调绑定到 @filp，失败返回 single_open 的 errno。 */
/*
 * 业务背景：打开 tunable_scaling 时需创建 single_open 上下文供标准 seq_read 使用。
 * 入参：inode 是未使用借用 inode；filp 是不可空、VFS 拥有的输入/输出文件。
 * 出参/返回：成功返回 0 并初始化文件私有 seq 状态；分配失败返回负 errno。
 * 注意事项：可睡眠；成功资源由 filp/single_release 管理，调用者不得自行释放 private_data。
 */
static int sched_scaling_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, sched_scaling_show, NULL);
}

/* scaling 文件的 standard seq 生命周期与 features 文件相同。 */
static const struct file_operations sched_scaling_fops = {
	.open		= sched_scaling_open,
	.write		= sched_scaling_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

#ifdef CONFIG_SCHED_CACHE
/* 解析布尔输入并重算 cache active 状态；解析失败不修改全局请求值。 */
/*
 * 业务背景：SCHED_CACHE 用户控制项要保存显式请求，并重新评估硬件/运行时条件下的实际 active 状态。
 * 入参：filp 是未使用借用文件；ubuf 是用户只读文本；cnt 是字节数；ppos 是不可空输入/输出偏移。
 * 出参/返回：成功返回 cnt、推进偏移并更新请求/active；解析失败返回负 errno且不改请求值。
 * 注意事项：仅 CONFIG_SCHED_CACHE；全局更新可与读者形成瞬时快照，用户缓冲 ownership 不变。
 */
static ssize_t
sched_cache_enable_write(struct file *filp, const char __user *ubuf,
			 size_t cnt, loff_t *ppos)
{
	bool val;
	int ret;

	/* 用户文本先解析为局部 bool，保证失败时请求值保持原样。 */
	ret = kstrtobool_from_user(ubuf, cnt, &val);
	if (ret)
		return ret;

	sysctl_sched_cache_user = val;

	/* active_set 再结合拓扑等条件计算实际启用状态，不等同于简单复制请求值。 */
	sched_cache_active_set();

	*ppos += cnt;

	return cnt;
}

/* 输出用户请求的 sched cache 开关，不等同于所有运行时条件均已满足。 */
/*
 * 业务背景：llc_balancing/enabled 读取展示用户请求值，以区别内部综合条件后的 active 状态。
 * 入参：m 是不可空输出 seq_file；v 是未使用借用参数，可为 NULL。
 * 出参/返回：成功恒返回 0并输出 0/1 和换行；无输出参数或 ownership 变化。
 * 注意事项：仅 CONFIG_SCHED_CACHE；无锁读取允许并发变化，值不代表功能此刻一定 active。
 */
static int sched_cache_enable_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", sysctl_sched_cache_user);
	return 0;
}

/* 建立 cache enable 的 single_open 文件上下文。 */
/*
 * 业务背景：打开 cache enable 节点时需把 show 回调绑定到标准 single_open 生命周期。
 * 入参：inode 是未使用借用 inode；filp 是不可空、VFS 拥有的输入/输出文件。
 * 出参/返回：成功返回 0并初始化 private_data；失败返回 single_open 的负 errno。
 * 注意事项：仅 CONFIG_SCHED_CACHE且可睡眠；成功资源由 single_release 回收。
 */
static int sched_cache_enable_open(struct inode *inode,
				   struct file *filp)
{
	return single_open(filp, sched_cache_enable_show, NULL);
}

/* cache 节点用 single_open/read/release，并允许布尔写回。 */
static const struct file_operations sched_cache_enable_fops = {
	.open           = sched_cache_enable_open,
	.write          = sched_cache_enable_write,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release        = single_release,
};
#endif

#ifdef CONFIG_PREEMPT_DYNAMIC

/* 解析 preemption 模式名并调用全局动态补丁入口；非法字符串返回负 errno。 */
/*
 * 业务背景：动态抢占 debugfs 写接口把用户模式名转换为内核补丁模式并立即全局生效。
 * 入参：filp 是未使用借用文件；ubuf 是用户只读文本；cnt 是字节数；ppos 是不可空输入/输出偏移。
 * 出参/返回：成功返回截断到 15 的消费字节并推进偏移；复制 -EFAULT，非法模式返回负 errno。
 * 注意事项：仅 PREEMPT_DYNAMIC；更新会修改全局静态调用/键且可同步，长输入被静默截断。
 */
static ssize_t sched_dynamic_write(struct file *filp, const char __user *ubuf,
				   size_t cnt, loff_t *ppos)
{
	char buf[16];
	int mode;

	/* 固定缓冲保留 NUL；超长模式名被截断后通常在解析阶段拒绝。 */
	if (cnt > 15)
		cnt = 15;

	if (copy_from_user(buf, ubuf, cnt))
		return -EFAULT;

	buf[cnt] = 0;
	mode = sched_dynamic_mode(strstrip(buf));
	if (mode < 0)
		return mode;

	/* 解析成功后才触发全局动态抢占补丁，随后提交文件偏移。 */
	sched_dynamic_update(mode);

	*ppos += cnt;

	return cnt;
}

/* 枚举本配置支持的抢占模式，并用括号标出 READ_ONCE 取得的当前模式。 */
/*
 * 业务背景：动态抢占读取需要列出本构建支持的模式，并标识当前选择供用户往返配置。
 * 入参：m 是不可空输出 seq_file；v 是未使用借用参数，可为 NULL。
 * 出参/返回：成功恒返回 0并输出模式列表；无输出参数或 ownership 变化。
 * 注意事项：仅 PREEMPT_DYNAMIC；当前模式用 READ_ONCE 快照，配置数组范围受 RT/LAZY 选项裁剪。
 */
static int sched_dynamic_show(struct seq_file *m, void *v)
{
	int i = (IS_ENABLED(CONFIG_PREEMPT_RT) || IS_ENABLED(CONFIG_ARCH_HAS_PREEMPT_LAZY)) * 2;
	int mode = READ_ONCE(preempt_dynamic_mode);
	int j;

	/* 构建选项决定起始可见模式，避免展示本内核无法切换的条目。 */
	/* Count entries in NULL terminated preempt_modes */
	for (j = 0; preempt_modes[j]; j++)
		;
	j -= !IS_ENABLED(CONFIG_ARCH_HAS_PREEMPT_LAZY);

	/* 当前模式仅加括号标记，列表中的其他名称仍保持可回写格式。 */
	for (; i < j; i++) {
		if (mode == i)
			seq_puts(m, "(");
		seq_puts(m, preempt_modes[i]);
		if (mode == i)
			seq_puts(m, ")");

		seq_puts(m, " ");
	}

	/* 列表末尾换行与 write 接口的单模式输入语法分离。 */
	seq_puts(m, "\n");
	return 0;
}

/* 为动态抢占状态建立 single_open 读取上下文。 */
/*
 * 业务背景：打开 preempt 节点时需建立由 sched_dynamic_show 驱动的 single seq_file。
 * 入参：inode 是未使用借用 inode；filp 是不可空、VFS 拥有的输入/输出文件。
 * 出参/返回：成功返回 0并初始化文件私有状态；失败返回 single_open 的负 errno。
 * 注意事项：仅 PREEMPT_DYNAMIC且可睡眠；private_data ownership 交给 single_release。
 */
static int sched_dynamic_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, sched_dynamic_show, NULL);
}

/* preempt 节点把模式列表读取与全局补丁写入组合为同一文件接口。 */
static const struct file_operations sched_dynamic_fops = {
	.open		= sched_dynamic_open,
	.write		= sched_dynamic_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

#endif /* CONFIG_PREEMPT_DYNAMIC */

__read_mostly bool sched_debug_verbose;

static struct dentry           *sd_dentry;


/*
 * 在 CPU hotplug 读锁与 sched_domains_mutex 下切换 verbose；0→1 重建
 * domain debugfs，1→0 摘除整棵目录，返回 debugfs bool 写入结果。
 */
/*
 * 业务背景：verbose 开关改变时，sched-domain debugfs 子树必须与布尔状态同步创建或移除。
 * 入参：filp 是绑定全局布尔值的借用文件；ubuf 是用户只读文本；cnt 是字节数；ppos 是不可空输入/输出偏移。
 * 出参/返回：返回 debugfs_write_file_bool 的字节数或负 errno；成功可能重建/移除 sd_dentry 并推进偏移。
 * 注意事项：可睡眠并按 CPU hotplug 读锁→domains mutex 顺序加锁；写失败后的 bool/目录语义继承 helper。
 */
static ssize_t sched_verbose_write(struct file *filp, const char __user *ubuf,
				  size_t cnt, loff_t *ppos)
{
	ssize_t result;
	bool orig;

	/* 锁顺序固定为 hotplug 读锁后 domains mutex，稳定 domain 对象和目录重建。 */
	cpus_read_lock();
	sched_domains_mutex_lock();

	orig = sched_debug_verbose;
	result = debugfs_write_file_bool(filp, ubuf, cnt, ppos);

	/* 只在布尔值真正跨越边界时创建或移除整棵 domains 子树。 */
	if (sched_debug_verbose && !orig)
		update_sched_domain_debugfs();
	else if (!sched_debug_verbose && orig) {
		debugfs_remove(sd_dentry);
		sd_dentry = NULL;
	}

	/* 无论 bool helper 成败均按逆序释放两把控制面锁。 */
	sched_domains_mutex_unlock();
	cpus_read_unlock();

	return result;
}

static const struct file_operations sched_verbose_fops = {
	.read =         debugfs_read_file_bool,
	.write =        sched_verbose_write,
	.open =         simple_open,
	.llseek =       default_llseek,
};

/* 多元素 sched/debug 的 seq_operations 在下文定义，此处只做前向绑定。 */
static const struct seq_operations sched_debug_sops;

/* 建立遍历 header 加 online CPU 的 seq_file；返回 seq_open 结果。 */
/*
 * 业务背景：sched/debug 文件需要 seq_operations 迭代一个 header 和所有在线 CPU。
 * 入参：inode 是未使用借用 inode；filp 是不可空、VFS 拥有的输入/输出文件。
 * 出参/返回：成功返回 0并安装 seq 状态；失败返回 seq_open 的负 errno。
 * 注意事项：可分配并睡眠；资源由 seq_release 回收，迭代期间 CPU 热插拔只通过 mask 快照近似处理。
 */
static int sched_debug_open(struct inode *inode, struct file *filp)
{
	return seq_open(filp, &sched_debug_sops);
}

/* sched/debug 使用多元素 seq_release，不是 single_open 的 single_release。 */
static const struct file_operations sched_debug_fops = {
	.open		= sched_debug_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

/* server 公共接口只允许选择 runtime 或 period 两个可写参数。 */
enum dl_param {
	DL_RUNTIME = 0,
	DL_PERIOD,
};

static unsigned long dl_server_period_max = (1UL << 22) * NSEC_PER_USEC; /* ~4 seconds */
static unsigned long dl_server_period_min = (100) * NSEC_PER_USEC;     /* 100 us */

/*
 * 修改指定 CPU 的 fair/ext deadline server runtime 或 period；rq irqsave 锁
 * 串行 stop→apply→start，范围、runtime<=period、CPU online 任一失败均返回 errno。
 */
/*
 * 业务背景：fair/ext deadline server 参数写入必须在目标 rq 上停服、校验并原子重启，避免半配置运行。
 * 入参：filp 提供私有 CPU；ubuf 是用户只读文本；cnt 是字节数；ppos 是偏移输出；param 选 runtime/period；server 是借用实体。
 * 出参/返回：成功返回 cnt并推进偏移；解析错误、-EINVAL、-EBUSY或 apply 的负 errno 直接返回。
 * 注意事项：在 rq irqsave 锁内不可睡眠；apply 失败仍会重启 server，参数 helper 的回滚语义需由其自身保证。
 */
static ssize_t sched_server_write_common(struct file *filp, const char __user *ubuf,
					 size_t cnt, loff_t *ppos, enum dl_param param,
					 void *server)
{
	/* 文件私有 CPU、目标 server 与其 rq 必须来自同一个节点创建上下文。 */
	long cpu = (long) ((struct seq_file *) filp->private_data)->private;
	struct sched_dl_entity *dl_se = (struct sched_dl_entity *)server;
	u64 old_runtime, runtime, period;
	struct rq *rq = cpu_rq(cpu);
	int retval = 0;
	size_t err;
	u64 value;

	/* CPU 来自创建节点时编码的 private 值，server 必须属于同一个 rq。 */
	/* 用户值保持纳秒单位；解析完成前不获取 rq 锁或修改 server。 */
	err = kstrtoull_from_user(ubuf, cnt, 10, &value);
	if (err)
		return err;

	scoped_guard (rq_lock_irqsave, rq) {
		/* 锁内快照旧 runtime，并在局部变量上选择性替换一个参数。 */
		old_runtime = runtime = dl_se->dl_runtime;
		period = dl_se->dl_period;

		switch (param) {
		case DL_RUNTIME:
			if (runtime == value)
				break;
			runtime = value;
			break;
		/* period 分支保持 runtime 不变，统一校验会拒绝新的 period 小于 runtime。 */
		case DL_PERIOD:
			if (value == period)
				break;
			period = value;
			break;
		}

		/* runtime/period 关系和全局周期上下界在停服前一次性验证。 */
		if (runtime > period ||
		    period > dl_server_period_max ||
		    period < dl_server_period_min) {
			return  -EINVAL;
		}

		if (!cpu_online(cpu_of(rq)))
			return -EBUSY;

		/* 在线 rq 上先刷新时钟，再以 stop→apply→start 形成完整 server 更新事务。 */
		update_rq_clock(rq);
		dl_server_stop(dl_se);
		retval = dl_server_apply_params(dl_se, runtime, period, 0);
		dl_server_start(dl_se);

		if (retval < 0)
			return retval;
	}

	/* runtime 在零/非零间切换才输出启停提示，普通数值调整保持安静。 */
	if (!!old_runtime ^ !!runtime) {
		pr_info("%s server %sabled on CPU %d%s.\n",
			server == &rq->fair_server ? "Fair" : "Ext",
			runtime ? "en" : "dis",
			cpu_of(rq),
			runtime ? "" : ", system may malfunction due to starvation");
	}

	/* 所有校验和 apply 成功后才提交用户消费字节数。 */
	*ppos += cnt;
	return cnt;
}

/* 输出 server 的 runtime 或 period 纳秒；返回 0，值是无锁诊断快照。 */
/*
 * 业务背景：四个 server debugfs 读取节点共享选择 runtime/period 并格式化的逻辑。
 * 入参：m 是不可空输出 seq_file；v 是未使用借用值；param 选择字段；server 是不可空借用 sched_dl_entity。
 * 出参/返回：成功恒返回 0并输出所选纳秒值；无独立输出参数或 ownership 变化。
 * 注意事项：无 rq 锁诊断快照，可与参数更新并发；param 必须是 DL_RUNTIME 或 DL_PERIOD，否则 value 未初始化。
 */
static size_t sched_server_show_common(struct seq_file *m, void *v, enum dl_param param,
				       void *server)
{
	struct sched_dl_entity *dl_se = (struct sched_dl_entity *)server;
	u64 value;

	/* param 只选择一个字段，不取得 rq 锁或修改 server。 */
	switch (param) {
	case DL_RUNTIME:
		value = dl_se->dl_runtime;
		break;
	case DL_PERIOD:
		value = dl_se->dl_period;
		break;
	}

	/* 统一以无符号十进制纳秒和换行输出，便于脚本读取。 */
	seq_printf(m, "%llu\n", value);
	return 0;
}

/* 将 fair server runtime 写请求转交共同校验/加锁实现。 */
/*
 * 业务背景：fair runtime 节点需把文件私有 CPU 映射到 rq->fair_server 并复用共同写协议。
 * 入参：filp/ubuf/cnt/ppos 分别是借用文件、用户只读文本、字节数和不可空偏移输入/输出。
 * 出参/返回：返回 common writer 的成功字节数或负 errno；可能更新目标 CPU fair runtime。
 * 注意事项：filp->private_data 必须是 single_open seq_file且 CPU 有效；ownership 均不转移。
 */
static ssize_t
sched_fair_server_runtime_write(struct file *filp, const char __user *ubuf,
				size_t cnt, loff_t *ppos)
{
	long cpu = (long) ((struct seq_file *) filp->private_data)->private;
	struct rq *rq = cpu_rq(cpu);

	return sched_server_write_common(filp, ubuf, cnt, ppos, DL_RUNTIME,
					&rq->fair_server);
}

/* 输出 @m 私有 CPU 的 fair server runtime。 */
/*
 * 业务背景：fair runtime 读取节点要从 seq_file 私有 CPU 找到对应 server 并输出纳秒预算。
 * 入参：m 是不可空且 private 为有效 CPU 的输出 seq_file；v 是未使用借用参数。
 * 出参/返回：成功返回 0并输出 runtime；无输出参数或 ownership 变化。
 * 注意事项：无锁诊断快照；无效 CPU/private 会访问错误 rq。
 */
static int sched_fair_server_runtime_show(struct seq_file *m, void *v)
{
	unsigned long cpu = (unsigned long) m->private;
	struct rq *rq = cpu_rq(cpu);

	/* wrapper 只完成 CPU→server 关联，字段选择和格式化由 common helper 负责。 */
	return sched_server_show_common(m, v, DL_RUNTIME, &rq->fair_server);
}

/* 以 inode 私有 CPU 号建立 fair runtime single_open 上下文。 */
/*
 * 业务背景：每 CPU fair runtime 文件打开时要把 inode->i_private CPU 传给 show 回调。
 * 入参：inode 是不可空借用且 i_private 编码 CPU；filp 是不可空 VFS 输入/输出文件。
 * 出参/返回：成功返回 0并建立 single_open；失败返回负 errno，private ownership 不转移。
 * 注意事项：可睡眠；成功资源由 single_release 回收，inode 私有 CPU 必须在 possible 范围。
 */
static int sched_fair_server_runtime_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, sched_fair_server_runtime_show, inode->i_private);
}

/* fair runtime 文件用 inode CPU 私有值贯穿 open/show/write。 */
static const struct file_operations fair_server_runtime_fops = {
	.open		= sched_fair_server_runtime_open,
	.write		= sched_fair_server_runtime_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static struct dentry *debugfs_sched;

#ifdef CONFIG_SCHED_CLASS_EXT
/* 将 ext server runtime 写请求转交共同校验/加锁实现。 */
/*
 * 业务背景：sched_ext runtime 节点要把文件私有 CPU 映射到 rq->ext_server 后复用共同写协议。
 * 入参：filp/ubuf/cnt/ppos 是借用文件、用户只读文本、字节数及不可空偏移输入/输出。
 * 出参/返回：返回 common writer 的成功字节数或负 errno；可能更新目标 CPU ext runtime。
 * 注意事项：仅 SCHED_CLASS_EXT；private_data 必须是含有效 CPU 的 seq_file，ownership 不转移。
 */
static ssize_t
sched_ext_server_runtime_write(struct file *filp, const char __user *ubuf,
			       size_t cnt, loff_t *ppos)
{
	long cpu = (long) ((struct seq_file *) filp->private_data)->private;
	struct rq *rq = cpu_rq(cpu);

	return sched_server_write_common(filp, ubuf, cnt, ppos, DL_RUNTIME,
					&rq->ext_server);
}

/* 输出 @m 私有 CPU 的 ext server runtime 纳秒。 */
/*
 * 业务背景：sched_ext runtime 读取节点需展示目标 CPU server 当前纳秒预算。
 * 入参：m 是不可空且 private 编码有效 CPU 的 seq_file；v 是未使用借用参数。
 * 出参/返回：成功返回 0并输出 runtime；无输出参数或 ownership 变化。
 * 注意事项：仅 SCHED_CLASS_EXT且为无锁诊断快照；无效 private 会访问错误 rq。
 */
static int sched_ext_server_runtime_show(struct seq_file *m, void *v)
{
	unsigned long cpu = (unsigned long) m->private;
	struct rq *rq = cpu_rq(cpu);

	return sched_server_show_common(m, v, DL_RUNTIME, &rq->ext_server);
}

/* 建立 ext runtime 的 single_open 上下文。 */
/*
 * 业务背景：每 CPU ext runtime 文件打开时需把 inode 私有 CPU 传递给 show 回调。
 * 入参：inode 是不可空借用且 i_private 编码 CPU；filp 是不可空 VFS 输入/输出文件。
 * 出参/返回：成功返回 0并建立 single_open；失败返回负 errno。
 * 注意事项：仅 SCHED_CLASS_EXT且可睡眠；成功资源由 single_release 回收，ownership 不转移。
 */
static int sched_ext_server_runtime_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, sched_ext_server_runtime_show, inode->i_private);
}

/* ext runtime 文件与 fair runtime 保持相同 single_open 生命周期。 */
static const struct file_operations ext_server_runtime_fops = {
	.open		= sched_ext_server_runtime_open,
	.write		= sched_ext_server_runtime_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* 将 ext server period 写请求转交共同校验/加锁实现。 */
/*
 * 业务背景：sched_ext period 节点要定位目标 CPU ext_server 并复用 runtime/period 共同事务。
 * 入参：filp/ubuf/cnt/ppos 是借用文件、用户只读文本、字节数及不可空偏移输入/输出。
 * 出参/返回：返回 common writer 的成功字节数或负 errno；可能更新目标 CPU ext period。
 * 注意事项：仅 SCHED_CLASS_EXT；CPU 来自 seq private且必须有效，用户缓冲 ownership 不变。
 */
static ssize_t
sched_ext_server_period_write(struct file *filp, const char __user *ubuf,
			      size_t cnt, loff_t *ppos)
{
	long cpu = (long) ((struct seq_file *) filp->private_data)->private;
	struct rq *rq = cpu_rq(cpu);

	return sched_server_write_common(filp, ubuf, cnt, ppos, DL_PERIOD,
					&rq->ext_server);
}

/* 输出 @m 私有 CPU 的 ext server period 纳秒。 */
/*
 * 业务背景：sched_ext period 读取节点向调试者展示目标 CPU 当前 server 周期。
 * 入参：m 是不可空且 private 编码有效 CPU 的输出 seq_file；v 是未使用借用参数。
 * 出参/返回：成功返回 0并输出 period 纳秒；无输出参数或 ownership 变化。
 * 注意事项：仅 SCHED_CLASS_EXT；无锁读取可能与写入并发，值是瞬时快照。
 */
static int sched_ext_server_period_show(struct seq_file *m, void *v)
{
	unsigned long cpu = (unsigned long) m->private;
	struct rq *rq = cpu_rq(cpu);

	return sched_server_show_common(m, v, DL_PERIOD, &rq->ext_server);
}

/* 建立 ext period 的 single_open 上下文。 */
/*
 * 业务背景：每 CPU ext period 文件打开时需建立携带 inode 私有 CPU 的 single seq 上下文。
 * 入参：inode 是不可空借用且 i_private 编码 CPU；filp 是不可空 VFS 输入/输出文件。
 * 出参/返回：成功返回 0并初始化 private_data；失败返回 single_open 负 errno。
 * 注意事项：仅 SCHED_CLASS_EXT且可睡眠；成功资源必须由 single_release 回收。
 */
static int sched_ext_server_period_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, sched_ext_server_period_show, inode->i_private);
}

/* ext period 文件只在 common helper 的 param 上区别于 runtime。 */
static const struct file_operations ext_server_period_fops = {
	.open		= sched_ext_server_period_open,
	.write		= sched_ext_server_period_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* 为每个 possible CPU 创建 ext server runtime/period 节点；目录失败则安静退化。 */
/*
 * 业务背景：sched debugfs 初始化需为每个 possible CPU 暴露 ext server 两个参数文件。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参数；创建 ext_server/cpuN/runtime,period 节点，失败可留下部分树。
 * 注意事项：仅 SCHED_CLASS_EXT且在初始化可睡眠上下文；debugfs 失败不报告 errno，dentry ownership 归 debugfs。
 */
static void debugfs_ext_server_init(void)
{
	struct dentry *d_ext;
	unsigned long cpu;

	d_ext = debugfs_create_dir("ext_server", debugfs_sched);
	if (!d_ext)
		return;

	/* possible 而非 online CPU，保证后续热上线无需动态补建控制文件。 */
	for_each_possible_cpu(cpu) {
		struct dentry *d_cpu;
		char buf[32];

		snprintf(buf, sizeof(buf), "cpu%lu", cpu);
		d_cpu = debugfs_create_dir(buf, d_ext);

		/* 两个文件共享 CPU private 值，但分别选择 runtime 和 period。 */
		debugfs_create_file("runtime", 0644, d_cpu, (void *) cpu, &ext_server_runtime_fops);
		debugfs_create_file("period", 0644, d_cpu, (void *) cpu, &ext_server_period_fops);
	}
}
#endif /* CONFIG_SCHED_CLASS_EXT */

/* 将 fair server period 写请求转交共同校验/加锁实现。 */
/*
 * 业务背景：fair period 节点要把文件私有 CPU 映射到 rq->fair_server 并复用共同写事务。
 * 入参：filp/ubuf/cnt/ppos 是借用文件、用户只读文本、字节数及不可空偏移输入/输出。
 * 出参/返回：返回 common writer 的成功字节数或负 errno；可能更新目标 CPU fair period。
 * 注意事项：private_data 必须是含有效 CPU 的 seq_file；用户缓冲和文件 ownership 均不转移。
 */
static ssize_t
sched_fair_server_period_write(struct file *filp, const char __user *ubuf,
			       size_t cnt, loff_t *ppos)
{
	long cpu = (long) ((struct seq_file *) filp->private_data)->private;
	struct rq *rq = cpu_rq(cpu);

	return sched_server_write_common(filp, ubuf, cnt, ppos, DL_PERIOD,
					&rq->fair_server);
}

/* 输出 @m 私有 CPU 的 fair server period 纳秒。 */
/*
 * 业务背景：fair period 读取节点需要展示目标 CPU deadline server 的当前周期。
 * 入参：m 是不可空且 private 编码有效 CPU 的输出 seq_file；v 是未使用借用参数。
 * 出参/返回：成功返回 0并输出 period 纳秒；无输出参数或 ownership 变化。
 * 注意事项：无锁诊断快照可与 rq 锁内更新并发；无效 CPU 会访问错误 rq。
 */
static int sched_fair_server_period_show(struct seq_file *m, void *v)
{
	unsigned long cpu = (unsigned long) m->private;
	struct rq *rq = cpu_rq(cpu);

	return sched_server_show_common(m, v, DL_PERIOD, &rq->fair_server);
}

/* 以 inode 私有 CPU 号建立 fair period single_open 上下文。 */
/*
 * 业务背景：每 CPU fair period 文件打开时需把 inode 私有 CPU 绑定给 show 回调。
 * 入参：inode 是不可空借用且 i_private 编码 CPU；filp 是不可空 VFS 输入/输出文件。
 * 出参/返回：成功返回 0并建立 single_open；失败返回负 errno。
 * 注意事项：可睡眠；成功资源由 single_release 回收，inode/private ownership 不转移。
 */
static int sched_fair_server_period_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, sched_fair_server_period_show, inode->i_private);
}

/* fair period 文件以 single_release 回收 open 分配的 seq 上下文。 */
static const struct file_operations fair_server_period_fops = {
	.open		= sched_fair_server_period_open,
	.write		= sched_fair_server_period_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* 为每个 possible CPU 创建 fair server runtime/period 节点；不持有 dentry 引用。 */
/*
 * 业务背景：sched debugfs 初始化要为所有 possible CPU 暴露 fair deadline server 参数。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参数；创建 fair_server/cpuN/runtime,period，失败可部分完成。
 * 注意事项：初始化可睡眠上下文；debugfs 接管 dentry 生命周期，目录创建失败时安静返回。
 */
static void debugfs_fair_server_init(void)
{
	struct dentry *d_fair;
	unsigned long cpu;

	d_fair = debugfs_create_dir("fair_server", debugfs_sched);
	if (!d_fair)
		return;

	/* 为所有 possible CPU 预建节点，使 offline CPU 上线后仍有稳定路径。 */
	for_each_possible_cpu(cpu) {
		struct dentry *d_cpu;
		char buf[32];

		snprintf(buf, sizeof(buf), "cpu%lu", cpu);
		d_cpu = debugfs_create_dir(buf, d_fair);

		/* dentry 由 debugfs 树持有，本函数无需保存每 CPU 指针。 */
		debugfs_create_file("runtime", 0644, d_cpu, (void *) cpu, &fair_server_runtime_fops);
		debugfs_create_file("period", 0644, d_cpu, (void *) cpu, &fair_server_period_fops);
	}
}

/* late init 阶段创建 sched debugfs 树；单个节点失败不阻止其余诊断接口。 */
/*
 * 业务背景：调度器 late-init 要集中建立全局、NUMA、LLC 和每 CPU server 的 debugfs 控制/诊断树。
 * 入参：无。
 * 出参/返回：恒返回 0；创建可用节点并发布 debugfs_sched，单节点失败允许留下部分树。
 * 注意事项：__init 可睡眠上下文且会持 sched_domains_mutex；debugfs 接管节点生命周期，无失败回滚。
 */
static __init int sched_init_debug(void)
{
	struct dentry __maybe_unused *numa, *llc;

	/* 先发布根目录，后续所有节点失败均独立退化而不回滚已创建部分。 */
	debugfs_sched = debugfs_create_dir("sched", NULL);

	debugfs_create_file("features", 0644, debugfs_sched, NULL, &sched_feat_fops);
	debugfs_create_file_unsafe("verbose", 0644, debugfs_sched, &sched_debug_verbose, &sched_verbose_fops);
#ifdef CONFIG_PREEMPT_DYNAMIC
	debugfs_create_file("preempt", 0644, debugfs_sched, NULL, &sched_dynamic_fops);
#endif

	/* 核心调度粒度和延迟告警参数直接绑定全局 u32 存储。 */
	debugfs_create_u32("base_slice_ns", 0644, debugfs_sched, &sysctl_sched_base_slice);

	debugfs_create_u32("latency_warn_ms", 0644, debugfs_sched, &sysctl_resched_latency_warn_ms);
	debugfs_create_u32("latency_warn_once", 0644, debugfs_sched, &sysctl_resched_latency_warn_once);

	debugfs_create_file("tunable_scaling", 0644, debugfs_sched, NULL, &sched_scaling_fops);
	debugfs_create_u32("migration_cost_ns", 0644, debugfs_sched, &sysctl_sched_migration_cost);
	debugfs_create_u32("nr_migrate", 0644, debugfs_sched, &sysctl_sched_nr_migrate);

	/* domain 子树引用活动拓扑，创建时用 domains mutex 稳定其生命周期。 */
	sched_domains_mutex_lock();
	update_sched_domain_debugfs();
	sched_domains_mutex_unlock();

#ifdef CONFIG_NUMA_BALANCING
	/* NUMA 子目录集中暴露扫描周期、大小和热页阈值。 */
	numa = debugfs_create_dir("numa_balancing", debugfs_sched);

	debugfs_create_u32("scan_delay_ms", 0644, numa, &sysctl_numa_balancing_scan_delay);
	debugfs_create_u32("scan_period_min_ms", 0644, numa, &sysctl_numa_balancing_scan_period_min);
	debugfs_create_u32("scan_period_max_ms", 0644, numa, &sysctl_numa_balancing_scan_period_max);
	debugfs_create_u32("scan_size_mb", 0644, numa, &sysctl_numa_balancing_scan_size);
	debugfs_create_u32("hot_threshold_ms", 0644, numa, &sysctl_numa_balancing_hot_threshold);
#endif /* CONFIG_NUMA_BALANCING */

#ifdef CONFIG_SCHED_CACHE
	/* LLC balancing 子目录区分用户 enable 请求与多项算法调优参数。 */
	llc = debugfs_create_dir("llc_balancing", debugfs_sched);
	debugfs_create_file("enabled", 0644, llc, NULL,
			    &sched_cache_enable_fops);
	debugfs_create_u32("aggr_tolerance", 0644, llc,
			   &llc_aggr_tolerance);
	debugfs_create_u32("epoch_period", 0644, llc,
			   &llc_epoch_period);
	/* epoch 后半组参数控制亲和超时和聚合/不均衡百分比阈值。 */
	debugfs_create_u32("epoch_affinity_timeout", 0644, llc,
			   &llc_epoch_affinity_timeout);
	debugfs_create_u32("overaggr_pct", 0644, llc,
			   &llc_overaggr_pct);
	debugfs_create_u32("imb_pct", 0644, llc,
			   &llc_imb_pct);
#endif

	/* 只读完整转储和每 CPU server 节点最后创建，均依赖前面的公共根目录。 */
	debugfs_create_file("debug", 0444, debugfs_sched, NULL, &sched_debug_fops);

	debugfs_fair_server_init();
#ifdef CONFIG_SCHED_CLASS_EXT
	debugfs_ext_server_init();
#endif

	return 0;
}
late_initcall(sched_init_debug);

static cpumask_var_t		sd_sysctl_cpus;

/* 把 inode 私有的 sched-domain flags 位图展开成名称列表。 */
/*
 * 业务背景：domain flags 数值难以阅读，debugfs 需按 sd_flag_debug 表展开为名称列表。
 * 入参：m 是不可空且 private 指向 unsigned int flags 的输出 seq_file；v 是未使用借用参数。
 * 出参/返回：成功恒返回 0并输出所有置位名称；无输出参数或 ownership 变化。
 * 注意事项：无锁读取 domain 字段且 private 必须在文件生命周期内有效；未知高位不输出。
 */
static int sd_flags_show(struct seq_file *m, void *v)
{
	unsigned long flags = *(unsigned int *)m->private;
	int idx;

	/* 只遍历已知位数，按定义表顺序生成稳定、空格分隔的名称。 */
	for_each_set_bit(idx, &flags, __SD_FLAG_CNT) {
		seq_puts(m, sd_flag_debug[idx].name);
		seq_puts(m, " ");
	}
	seq_puts(m, "\n");

	return 0;
}

/* 将 sched-domain flags 地址作为 single_open 私有数据传给 show。 */
/*
 * 业务背景：flags 文件打开时需把 register_sd 绑定的字段地址传给名称展开回调。
 * 入参：inode 是不可空且 i_private 指向存活 flags 的借用对象；file 是不可空 VFS 输入/输出文件。
 * 出参/返回：成功返回 0并建立 single_open；失败返回负 errno，字段 ownership 不转移。
 * 注意事项：可睡眠；domain/debugfs 生命周期必须覆盖打开文件，资源由 single_release 回收。
 */
static int sd_flags_open(struct inode *inode, struct file *file)
{
	return single_open(file, sd_flags_show, inode->i_private);
}

static const struct file_operations sd_flags_fops = {
	.open		= sd_flags_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* 在 @parent 下暴露 @sd 的可调字段和只读拓扑字段；仅借用 sd/dentry。 */
/*
 * 业务背景：verbose domain 树需为一个 sched_domain 创建间隔、负载均衡、名称、flags 和层级节点。
 * 入参：sd 是不可空存活 domain 的借用输入/输出；parent 是不可空父 dentry 借用输入。
 * 出参/返回：无直接返回值、无输出参数；创建指向 sd 字段的 debugfs 文件，失败可部分完成。
 * 注意事项：调用者持 domain 生命周期/重建串行化；节点直接引用字段，sd 销毁前必须移除目录。
 */
static void register_sd(struct sched_domain *sd, struct dentry *parent)
{
	/* 可写调优字段直接绑定 sd 成员，name 只读，debugfs 失败不影响其余节点。 */
#define SDM(type, mode, member)	\
	debugfs_create_##type(#member, mode, parent, &sd->member)

	SDM(ulong, 0644, min_interval);
	SDM(ulong, 0644, max_interval);
	SDM(u64,   0644, max_newidle_lb_cost);
	SDM(u32,   0644, busy_factor);
	SDM(u32,   0644, imbalance_pct);
	SDM(u32,   0644, cache_nice_tries);
	SDM(str,   0444, name);

#undef SDM

	/* flags 用名称化 show，level 和不对称优选 CPU 作为只读拓扑事实。 */
	debugfs_create_file("flags", 0444, parent, &sd->flags, &sd_flags_fops);
	debugfs_create_file("groups_flags", 0444, parent, &sd->groups->flags, &sd_flags_fops);
	debugfs_create_u32("level", 0444, parent, (u32 *)&sd->level);

	if (sd->flags & SD_ASYM_PACKING)
		debugfs_create_u32("group_asym_prefer_cpu", 0444, parent,
				   (u32 *)&sd->groups->asym_prefer_cpu);
}

/*
 * 重建被 dirty CPU 的 domain debugfs 子树；调用者持 hotplug/domain 串行化，
 * 未初始化、verbose 关闭或 cpumask 分配失败时保留待办并直接返回。
 */
/*
 * 业务背景：拓扑变化后只重建 dirty CPU 的 sched-domain debugfs 子树，并在首次启用时覆盖全部 CPU。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参数；可能分配 dirty mask、创建 domains 树并清已处理 CPU 位。
 * 注意事项：调用者持 sched_domains_mutex及所需 hotplug 串行化且可睡眠；分配/创建失败允许延后或部分树。
 */
void update_sched_domain_debugfs(void)
{
	int cpu, i;

	/*
	 * This can unfortunately be invoked before sched_debug_init() creates
	 * the debug directory. Don't touch sd_sysctl_cpus until then.
	 */
	if (!debugfs_sched)
		return;

	/* verbose 是 domain 树的总开关，关闭时保留 dirty mask 等待以后重建。 */
	if (!sched_debug_verbose)
		return;

	if (!cpumask_available(sd_sysctl_cpus)) {
		/* 首次分配把全部 possible CPU 置脏，保证初始树不会静默缺项。 */
		if (!alloc_cpumask_var(&sd_sysctl_cpus, GFP_KERNEL))
			return;
		cpumask_copy(sd_sysctl_cpus, cpu_possible_mask);
	}

	if (!sd_dentry) {
		/* 目录被 verbose 关闭路径移除后，空 mask 需用当前 online 集重新播种。 */
		sd_dentry = debugfs_create_dir("domains", debugfs_sched);

		/* rebuild sd_sysctl_cpus if empty since it gets cleared below */
		if (cpumask_empty(sd_sysctl_cpus))
			cpumask_copy(sd_sysctl_cpus, cpu_online_mask);
	}

	for_each_cpu(cpu, sd_sysctl_cpus) {
		struct sched_domain *sd;
		struct dentry *d_cpu;
		char buf[32];

		/* 每个 dirty CPU 先整体移除旧目录，再从活动 domain 链完整重建。 */
		snprintf(buf, sizeof(buf), "cpu%d", cpu);
		debugfs_lookup_and_remove(buf, sd_dentry);
		d_cpu = debugfs_create_dir(buf, sd_dentry);

		i = 0;
		/* domain 编号按当前链顺序生成，register_sd 绑定字段的活动地址。 */
		for_each_domain(cpu, sd) {
			struct dentry *d_sd;

			snprintf(buf, sizeof(buf), "domain%d", i);
			d_sd = debugfs_create_dir(buf, d_cpu);

			register_sd(sd, d_sd);
			i++;
		}

		/* 只有该 CPU 全部 domain 节点创建尝试结束后才清 dirty 位。 */
		__cpumask_clear_cpu(cpu, sd_sysctl_cpus);
	}
}

/* 将 @cpu 标成 domain debugfs 待重建；cpumask 尚未分配时无需记录。 */
/*
 * 业务背景：单 CPU domain 拓扑变脏时先记录增量待办，下一次统一重建其 debugfs 子树。
 * 入参：cpu 是 0..nr_cpu_ids-1 的有效 CPU 编号，纯输入。
 * 出参/返回：无直接返回值、无输出参数；mask 已分配时设置对应位，否则无副作用。
 * 注意事项：调用者负责与 mask 重建并发串行；越界 CPU 会破坏 cpumask，函数不分配内存。
 */
void dirty_sched_domain_sysctl(int cpu)
{
	if (cpumask_available(sd_sysctl_cpus))
		__cpumask_set_cpu(cpu, sd_sysctl_cpus);
}

#ifdef CONFIG_FAIR_GROUP_SCHED
/* 输出 @tg 在 @cpu 的 sched_entity 与可选 schedstats；无实体时为空输出。 */
/*
 * 业务背景：CFS group 调试需要展示该组在指定 CPU 上代表性 entity 的运行与等待统计。
 * 入参：m 是可空输出目标（NULL 写控制台）；cpu 是有效 CPU；tg 是不可空存活 task_group 借用输入。
 * 出参/返回：无直接返回值、无输出参数；有 entity 时追加字段，否则不输出。
 * 注意事项：仅 FAIR_GROUP_SCHED；无 rq 锁诊断快照，tg/entity 生命周期由调用者保证，字段可跨时刻。
 */
static void print_cfs_group_stats(struct seq_file *m, int cpu, struct task_group *tg)
{
	struct sched_entity *se = tg_se(tg, cpu);

	/* 局部宏统一整数和纳秒格式，schedstat 版本经运行时开关读取。 */
#define P(F)		SEQ_printf(m, "  .%-30s: %lld\n",	#F, (long long)F)
#define P_SCHEDSTAT(F)	SEQ_printf(m, "  .%-30s: %lld\n",	\
		#F, (long long)schedstat_val(stats->F))
#define PN(F)		SEQ_printf(m, "  .%-30s: %lld.%06ld\n", #F, SPLIT_NS((long long)F))
#define PN_SCHEDSTAT(F)	SEQ_printf(m, "  .%-30s: %lld.%06ld\n", \
		#F, SPLIT_NS((long long)schedstat_val(stats->F)))

	if (!se)
		return;

	/* 基础 EEVDF 运行字段无论 schedstats 开关都输出。 */
	PN(se->exec_start);
	PN(se->vruntime);
	PN(se->sum_exec_runtime);

	if (schedstat_enabled()) {
		struct sched_statistics *stats;
		stats = __schedstats_from_se(se);

		/* schedstats 启用时再输出等待/睡眠/阻塞起点、峰值和累计量。 */
		PN_SCHEDSTAT(wait_start);
		PN_SCHEDSTAT(sleep_start);
		PN_SCHEDSTAT(block_start);
		PN_SCHEDSTAT(sleep_max);
		PN_SCHEDSTAT(block_max);
		/* 峰值、等待累计和次数构成 schedstats 的第二组输出。 */
		PN_SCHEDSTAT(exec_max);
		PN_SCHEDSTAT(slice_max);
		PN_SCHEDSTAT(wait_max);
		PN_SCHEDSTAT(wait_sum);
		P_SCHEDSTAT(wait_count);
	}

	/* load/PELT 平均值来自 entity 当前无锁快照，与上面的统计不保证同刻。 */
	P(se->load.weight);
	P(se->avg.load_avg);
	P(se->avg.util_avg);
	P(se->avg.runnable_avg);

	/* 函数局部宏必须全部撤销，避免下方 task 打印宏发生重定义污染。 */
#undef PN_SCHEDSTAT
#undef PN
#undef P_SCHEDSTAT
#undef P
}
#endif /* CONFIG_FAIR_GROUP_SCHED */

#ifdef CONFIG_CGROUP_SCHED
	/* 全局长路径缓冲由 trylock 保护，竞争者不阻塞而使用各自栈缓冲。 */
static DEFINE_SPINLOCK(sched_debug_lock);
static char group_path[PATH_MAX];

/* 优先生成 autogroup 路径，否则写入 cgroup 路径；@path 由调用者提供。 */
/*
 * 业务背景：任务调试行需用统一文本标识其 autogroup 或普通调度 cgroup 路径。
 * 入参：tg 是不可空存活 task_group 借用输入；path 是不可空可写输出缓冲；plen 是缓冲字节容量。
 * 出参/返回：无直接返回值；path 被写成 NUL 结尾路径，ownership 不变。
 * 注意事项：仅 CGROUP_SCHED；调用者保证 tg/cgroup 生命周期和 plen>0，路径可按容量截断。
 */
static void task_group_path(struct task_group *tg, char *path, int plen)
{
	if (autogroup_path(tg, path, plen))
		return;

	cgroup_path(tg->css.cgroup, path, plen);
}

/*
 * Only 1 SEQ_printf_task_group_path() caller can use the full length
 * group_path[] for cgroup path. Other simultaneous callers will have
 * to use a shorter stack buffer. A "..." suffix is appended at the end
 * of the stack buffer so that it will show up in case the output length
 * matches the given buffer size to indicate possible path name truncation.
 */
/* 全局 PATH_MAX 缓冲仅供 trylock 获胜者使用；竞争者改用带“...”哨兵的栈缓冲。 */
#define SEQ_printf_task_group_path(m, tg, fmt...)			\
{									\
	if (spin_trylock(&sched_debug_lock)) {				\
		task_group_path(tg, group_path, sizeof(group_path));	\
		SEQ_printf(m, fmt, group_path);				\
		spin_unlock(&sched_debug_lock);				\
	} else {							\
		/* 竞争退化不等待锁，短缓冲末尾主动保留截断标记。 */ \
		char buf[128];						\
		char *bufend = buf + sizeof(buf) - 3;			\
		task_group_path(tg, buf, bufend - buf);			\
		strcpy(bufend - 1, "...");				\
		SEQ_printf(m, fmt, buf);				\
	}								\
}
#endif

/* 输出一个 task 的状态、EEVDF、运行时间、切换、NUMA 与 cgroup 列；不取得引用。 */
/*
 * 业务背景：sched/debug runnable 表需要把单个 task 的调度身份、EEVDF 和统计压缩成一行。
 * 入参：m 是可空输出目标；rq 是不可空目标 runqueue 借用输入；p 是不可空且 RCU 稳定的 task 借用输入。
 * 出参/返回：无直接返回值、无输出参数；向目标追加一行任务快照，ownership 不变。
 * 注意事项：不持 task rq 锁，字段可来自不同时刻；cgroup 路径用 trylock，竞争时可能截断。
 */
static void
print_task(struct seq_file *m, struct rq *rq, struct task_struct *p)
{
	/* 行首区分此 rq 的 current 与其他 task 的瞬时状态字符。 */
	if (task_current(rq, p))
		SEQ_printf(m, ">R");
	else
		SEQ_printf(m, " %c", task_state_to_char(p));

	/* 主列集中输出身份、EEVDF资格/截止期/切片、runtime、切换数和优先级。 */
	SEQ_printf(m, " %15s %5d %9Ld.%06ld   %c   %9Ld.%06ld %c %9Ld.%06ld %9Ld.%06ld %9Ld   %5d ",
		p->comm, task_pid_nr(p),
		SPLIT_NS(p->se.vruntime),
		entity_eligible(cfs_rq_of(&p->se), &p->se) ? 'E' : 'N',
		SPLIT_NS(p->se.deadline),
		p->se.custom_slice ? 'S' : ' ',
		SPLIT_NS(p->se.slice),
		SPLIT_NS(p->se.sum_exec_runtime),
		(long long)(p->nvcsw + p->nivcsw),
		p->prio);

	/* schedstats 未启用时 helper 输出零，保持表格列布局不随运行时开关改变。 */
	SEQ_printf(m, "%9lld.%06ld %9lld.%06ld %9lld.%06ld",
		SPLIT_NS(schedstat_val_or_zero(p->stats.wait_sum)),
		SPLIT_NS(schedstat_val_or_zero(p->stats.sum_sleep_runtime)),
		SPLIT_NS(schedstat_val_or_zero(p->stats.sum_block_runtime)));

#ifdef CONFIG_NUMA_BALANCING
	/* 配置专属列只在表头也启用相同配置时追加。 */
	SEQ_printf(m, "   %d      %d", task_node(p), task_numa_group_id(p));
#endif
#ifdef CONFIG_CGROUP_SCHED
	SEQ_printf_task_group_path(m, task_group(p), "        %s")
#endif

	SEQ_printf(m, "\n");
}

/*
 * 遍历全局 tasklist 并输出当前标记在 @rq_cpu 的任务；RCU 只保证 task
 * 生命周期，字段会并发变化，因此各行是诊断快照而非原子队列转储。
 */
/*
 * 业务背景：runqueue 调试段要列出当前标记在目标 CPU 上的所有 task，而非只遍历某一调度类树。
 * 入参：m 是可空输出目标；rq 是不可空目标 rq 借用输入；rq_cpu 是有效 CPU 编号过滤条件。
 * 出参/返回：无直接返回值、无输出参数；输出表头和匹配 task 行。
 * 注意事项：RCU 仅稳定 task 生命周期，不锁迁移或字段；task_cpu 可在检查后变化，结果是近似诊断。
 */
static void print_rq(struct seq_file *m, struct rq *rq, int rq_cpu)
{
	struct task_struct *g, *p;

	/* 表头按配置拼接 NUMA/cgroup 列，必须与 print_task 的条件列一致。 */
	SEQ_printf(m, "\n");
	SEQ_printf(m, "runnable tasks:\n");
	SEQ_printf(m, " S            task   PID       vruntime   eligible    "
		   "deadline             slice          sum-exec      switches  "
		   "prio         wait-time        sum-sleep       sum-block"
		   /* NUMA 与 cgroup 条件列必须同时影响表头、分隔线和 task 行。 */
#ifdef CONFIG_NUMA_BALANCING
		   "  node   group-id"
#endif
#ifdef CONFIG_CGROUP_SCHED
		   "  group-path"
#endif
		   "\n");
	/* 分隔线按同一配置扩展，之后才进入 RCU tasklist 遍历。 */
	SEQ_printf(m, "-------------------------------------------------------"
		   "------------------------------------------------------"
		   "------------------------------------------------------"
#ifdef CONFIG_NUMA_BALANCING
		   "--------------"
#endif
#ifdef CONFIG_CGROUP_SCHED
		   "--------------"
#endif
		   "\n");

	/* RCU 稳定进程/线程链；CPU 过滤只是瞬时判断，不阻止检查后的迁移。 */
	rcu_read_lock();
	for_each_process_thread(g, p) {
		if (task_cpu(p) != rq_cpu)
			continue;

		print_task(m, rq, p);
	}
	rcu_read_unlock();
}

/*
 * 输出 CFS rq 的树边界、加权平均与 PELT/带宽字段；树节点先在 rq 锁下
 * 复制，解锁后打印，其他统计允许稍有时差以避免长时间持锁。
 */
/*
 * 业务背景：CFS rq 调试需同时展示 EEVDF 树边界、加权平均、PELT、层级和带宽状态。
 * 入参：m 是可空输出目标；cpu 是有效 CPU；cfs_rq 是不可空、属于该 CPU 的借用队列。
 * 出参/返回：无直接返回值、无输出参数；向目标追加完整 CFS rq 快照。
 * 注意事项：仅树边界/加权字段在 rq irqsave 锁下复制，后续字段无锁可有时差；不可传错 CPU/rq 配对。
 */
void print_cfs_rq(struct seq_file *m, int cpu, struct cfs_rq *cfs_rq)
{
	s64 left_vruntime = -1, right_vruntime = -1, left_deadline = -1, spread;
	s64 zero_vruntime = -1, sum_w_vruntime = -1;
	u64 avruntime;
	struct sched_entity *last, *first, *root;
	struct rq *rq = cpu_rq(cpu);
	unsigned int sum_shift;
	unsigned long flags;
	u64 sum_weight;

	/* 组调度构建用 cgroup 路径区分多个 cfs_rq，根队列构建只输出 CPU 编号。 */
#ifdef CONFIG_FAIR_GROUP_SCHED
	SEQ_printf(m, "\n");
	SEQ_printf_task_group_path(m, cfs_rq->tg, "cfs_rq[%d]:%s\n", cpu);
#else
	SEQ_printf(m, "\n");
	SEQ_printf(m, "cfs_rq[%d]:\n", cpu);
#endif

	/* rq 锁内只复制红黑树边界和加权聚合，避免解锁后解引用可能变化的 entity。 */
	raw_spin_rq_lock_irqsave(rq, flags);
	root = __pick_root_entity(cfs_rq);
	if (root)
		left_vruntime = root->min_vruntime;
	first = __pick_first_entity(cfs_rq);
	if (first)
		left_deadline = first->deadline;
	/* last/root/first 不转移引用，相关数值必须仍在锁内复制。 */
	last = __pick_last_entity(cfs_rq);
	if (last)
		right_vruntime = last->vruntime;
	zero_vruntime = cfs_rq->zero_vruntime;
	sum_w_vruntime = cfs_rq->sum_w_vruntime;
	sum_weight = cfs_rq->sum_weight;
	sum_shift = cfs_rq->sum_shift;
	avruntime = avg_vruntime(cfs_rq);
	raw_spin_rq_unlock_irqrestore(rq, flags);

	/* 第一组输出 EEVDF 边界、零点及加权虚拟运行时间。 */
	SEQ_printf(m, "  .%-30s: %Ld.%06ld\n", "left_deadline",
			SPLIT_NS(left_deadline));
	SEQ_printf(m, "  .%-30s: %Ld.%06ld\n", "left_vruntime",
			SPLIT_NS(left_vruntime));
	SEQ_printf(m, "  .%-30s: %Ld.%06ld\n", "zero_vruntime",
			SPLIT_NS(zero_vruntime));
	SEQ_printf(m, "  .%-30s: %Ld (%d bits)\n", "sum_w_vruntime",
		   sum_w_vruntime, ilog2(abs(sum_w_vruntime)));
	/* sum_weight/sum_shift 描述归一化权重表示，avg_vruntime 是其当前中心。 */
	SEQ_printf(m, "  .%-30s: %Lu\n", "sum_weight",
		   sum_weight);
	SEQ_printf(m, "  .%-30s: %u\n", "sum_shift", sum_shift);
	SEQ_printf(m, "  .%-30s: %Ld.%06ld\n", "avg_vruntime",
			SPLIT_NS(avruntime));
	/* spread 由已复制左右边界计算；空树保留 -1 哨兵的诊断含义。 */
	SEQ_printf(m, "  .%-30s: %Ld.%06ld\n", "right_vruntime",
			SPLIT_NS(right_vruntime));
	spread = right_vruntime - left_vruntime;
	SEQ_printf(m, "  .%-30s: %Ld.%06ld\n", "spread", SPLIT_NS(spread));
	SEQ_printf(m, "  .%-30s: %d\n", "nr_queued", cfs_rq->nr_queued);
	SEQ_printf(m, "  .%-30s: %d\n", "h_nr_runnable", cfs_rq->h_nr_runnable);
	/* 层级 runnable/queued/idle 计数用于区分本层实体与子组传播量。 */
	SEQ_printf(m, "  .%-30s: %d\n", "h_nr_queued", cfs_rq->h_nr_queued);
	SEQ_printf(m, "  .%-30s: %d\n", "h_nr_idle", cfs_rq->h_nr_idle);
	/* runnable 层级计数之后输出 load 权重和 PELT 三类平均值。 */
	SEQ_printf(m, "  .%-30s: %ld\n", "load", cfs_rq->load.weight);
	SEQ_printf(m, "  .%-30s: %lu\n", "load_avg",
			cfs_rq->avg.load_avg);
	SEQ_printf(m, "  .%-30s: %lu\n", "runnable_avg",
			cfs_rq->avg.runnable_avg);
	SEQ_printf(m, "  .%-30s: %lu\n", "util_avg",
			cfs_rq->avg.util_avg);
	SEQ_printf(m, "  .%-30s: %u\n", "util_est",
			cfs_rq->avg.util_est);
	/* removed 累计表示尚待从聚合 PELT 中衰减/移除的贡献。 */
	SEQ_printf(m, "  .%-30s: %ld\n", "removed.load_avg",
			cfs_rq->removed.load_avg);
	SEQ_printf(m, "  .%-30s: %ld\n", "removed.util_avg",
			cfs_rq->removed.util_avg);
	SEQ_printf(m, "  .%-30s: %ld\n", "removed.runnable_avg",
			cfs_rq->removed.runnable_avg);
#ifdef CONFIG_FAIR_GROUP_SCHED
	/* 组配置再关联 task_group 全局负载贡献，字段与前面快照允许存在时差。 */
	SEQ_printf(m, "  .%-30s: %lu\n", "tg_load_avg_contrib",
			cfs_rq->tg_load_avg_contrib);
	SEQ_printf(m, "  .%-30s: %ld\n", "tg_load_avg",
			atomic_long_read(&cfs_rq->tg->load_avg));
#endif /* CONFIG_FAIR_GROUP_SCHED */
#ifdef CONFIG_CFS_BANDWIDTH
	/* 带宽配置最后补充 throttle 状态和嵌套计数。 */
	SEQ_printf(m, "  .%-30s: %d\n", "throttled",
			cfs_rq->throttled);
	SEQ_printf(m, "  .%-30s: %d\n", "throttle_count",
			cfs_rq->throttle_count);
#endif

#ifdef CONFIG_FAIR_GROUP_SCHED
	print_cfs_group_stats(m, cpu, cfs_rq->tg);
#endif
}

/* 输出 RT rq 的 runnable 数及组调度限流字段；不修改队列。 */
/*
 * 业务背景：调度诊断要展示 RT 队列可运行数，并在组调度配置下展示 runtime/throttle 状态。
 * 入参：m 是可空输出目标；cpu 是有效 CPU；rt_rq 是不可空、属于该 CPU 的只读借用队列。
 * 出参/返回：无直接返回值、无输出参数；追加 RT rq 字段。
 * 注意事项：无 rq 锁瞬时快照；RT_GROUP_SCHED 改变输出字段，调用者保证对象生命周期。
 */
void print_rt_rq(struct seq_file *m, int cpu, struct rt_rq *rt_rq)
{
	/* 组调度下用路径区分队列，否则仅以 CPU 标识根 RT rq。 */
#ifdef CONFIG_RT_GROUP_SCHED
	SEQ_printf(m, "\n");
	SEQ_printf_task_group_path(m, rt_rq->tg, "rt_rq[%d]:%s\n", cpu);
#else
	SEQ_printf(m, "\n");
	SEQ_printf(m, "rt_rq[%d]:\n", cpu);
#endif

	/* 标题完成后定义三种局部格式宏，分别覆盖有符号、无符号和纳秒字段。 */
#define P(x) \
	SEQ_printf(m, "  .%-30s: %Ld\n", #x, (long long)(rt_rq->x))
#define PU(x) \
	SEQ_printf(m, "  .%-30s: %lu\n", #x, (unsigned long)(rt_rq->x))
	/* PN 专用于纳秒 runtime 字段，避免与普通整数单位混淆。 */
#define PN(x) \
	SEQ_printf(m, "  .%-30s: %Ld.%06ld\n", #x, SPLIT_NS(rt_rq->x))

	/* runnable 数始终存在，throttle/runtime 只在 RT group 配置下有意义。 */
	PU(rt_nr_running);

#ifdef CONFIG_RT_GROUP_SCHED
	P(rt_throttled);
	PN(rt_time);
	PN(rt_runtime);
#endif

	/* 局部打印宏在函数尾撤销，避免污染后续 DL/CPU 打印定义。 */
#undef PN
#undef PU
#undef P
}

/* 输出 DL runnable 数和 root-domain 带宽快照；@dl_rq 仅借用。 */
/*
 * 业务背景：deadline 调试需关联本 CPU 可运行数与 root-domain 总带宽配额。
 * 入参：m 是可空输出目标；cpu 是有效 CPU；dl_rq 是不可空、属于该 CPU 的只读借用队列。
 * 出参/返回：无直接返回值、无输出参数；追加 DL rq 和 dl_bw 字段。
 * 注意事项：无 rq/root-domain 锁诊断快照，rd 可能并发变化；调用者保证 cpu/rq 对应和生命周期。
 */
void print_dl_rq(struct seq_file *m, int cpu, struct dl_rq *dl_rq)
{
	struct dl_bw *dl_bw;

	/* DL rq 标题与 runnable 数先输出，再关联当前 rq 所属 root-domain 带宽。 */
	SEQ_printf(m, "\n");
	SEQ_printf(m, "dl_rq[%d]:\n", cpu);

#define PU(x) \
	SEQ_printf(m, "  .%-30s: %lu\n", #x, (unsigned long)(dl_rq->x))

	PU(dl_nr_running);
	/* rd 带宽字段无锁读取，仅用于同时观察配置配额和当前累计。 */
	dl_bw = &cpu_rq(cpu)->rd->dl_bw;
	SEQ_printf(m, "  .%-30s: %lld\n", "dl_bw->bw", dl_bw->bw);
	SEQ_printf(m, "  .%-30s: %lld\n", "dl_bw->total_bw", dl_bw->total_bw);

#undef PU
}

/* 汇总 @cpu 的 rq 时钟、调度类统计和 runnable task；@m 为 NULL 时写控制台。 */
/*
 * 业务背景：sched/debug 和 SysRq 共用每 CPU 汇总器，依次展示频率、rq、各调度类和 task 表。
 * 入参：m 是可空输出 seq_file，NULL 表示控制台；cpu 是在线且有效的 CPU 编号。
 * 出参/返回：无直接返回值、无输出参数；向选定目标追加一个 CPU 的诊断段。
 * 注意事项：大部分字段无锁读取且不构成原子快照；rq/current 生命周期由在线 CPU/调用者稳定。
 */
static void print_cpu(struct seq_file *m, int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	/* x86 额外展示启动期 kHz 换算的 MHz；未知频率用 1 避免除零。 */
#ifdef CONFIG_X86
	{
		unsigned int freq = cpu_khz ? : 1;

		SEQ_printf(m, "cpu#%d, %u.%03u MHz\n",
			   cpu, freq / 1000, (freq % 1000));
	}
#else /* !CONFIG_X86: */
	SEQ_printf(m, "cpu#%d\n", cpu);
#endif /* !CONFIG_X86 */

	/* P/PN 宏按字段宽度选择整数格式，并把纳秒拆成固定六位小数。 */
#define P(x)								\
do {									\
	if (sizeof(rq->x) == 4)						\
		SEQ_printf(m, "  .%-30s: %d\n", #x, (int)(rq->x));	\
	else								\
		SEQ_printf(m, "  .%-30s: %Ld\n", #x, (long long)(rq->x));\
} while (0)

	/* PN 与 P 分离，明确 rq 时钟字段采用固定小数纳秒格式。 */
#define PN(x) \
	SEQ_printf(m, "  .%-30s: %Ld.%06ld\n", #x, SPLIT_NS(rq->x))

	P(nr_running);
	P(nr_switches);
	P(nr_uninterruptible);
	PN(next_balance);
	SEQ_printf(m, "  .%-30s: %ld\n", "curr->pid", (long)(task_pid_nr(rq->curr)));
	PN(clock);
	PN(clock_task);
	/* 基础 rq 状态输出后立即撤销宏，防止与后续统计格式冲突。 */
#undef P
#undef PN

#define P64(n) SEQ_printf(m, "  .%-30s: %Ld\n", #n, rq->n);
	/* idle balance 两个 64 位成本字段单独使用确定格式。 */
	P64(avg_idle);
	P64(max_idle_balance_cost);
#undef P64

#define P(n) SEQ_printf(m, "  .%-30s: %d\n", #n, schedstat_val(rq->n));
	/* schedstats 运行时关闭时不读取或展示事件计数。 */
	if (schedstat_enabled()) {
		P(yld_count);
		P(sched_count);
		P(sched_goidle);
		P(ttwu_count);
		P(ttwu_local);
	}
#undef P

	/* 各调度类私有统计之后，最后输出全局 tasklist 过滤出的 runnable 表。 */
	print_cfs_stats(m, cpu);
	print_rt_stats(m, cpu);
	print_dl_stats(m, cpu);

	print_rq(m, rq, cpu);
	SEQ_printf(m, "\n");
}

/* scaling 名称表与写路径枚举一一对应，header 用它把数值翻译成人类可读文本。 */
static const char *sched_tunable_scaling_names[] = {
	"none",
	"logarithmic",
	"linear"
};

/* 关本地中断取得三种时钟样本并输出全局调度开关；不保证与后续 CPU 行同刻。 */
/*
 * 业务背景：调度转储开头需提供内核版本、三种时钟和关键 sysctl，帮助解释后续每 CPU 数值。
 * 入参：m 是可空输出 seq_file，NULL 表示连续写控制台。
 * 出参/返回：无直接返回值、无输出参数；追加 header 与全局参数快照。
 * 注意事项：仅采集三种时钟时关本地中断；其余全局值无锁，scaling 枚举必须保持有效范围。
 */
static void sched_debug_header(struct seq_file *m)
{
	u64 ktime, sched_clk, cpu_clk;
	unsigned long flags;

	/* 三种时钟在同一关中断窗口采样，减少比较它们时的人为跨中断偏差。 */
	local_irq_save(flags);
	ktime = ktime_to_ns(ktime_get());
	sched_clk = sched_clock();
	cpu_clk = local_clock();
	local_irq_restore(flags);

	/* 版本行只取 uts version 首个空格前片段，避免多余构建文本破坏布局。 */
	SEQ_printf(m, "Sched Debug Version: v0.11, %s %.*s\n",
		init_utsname()->release,
		(int)strcspn(init_utsname()->version, " "),
		init_utsname()->version);

#define P(x) \
	SEQ_printf(m, "%-40s: %Ld\n", #x, (long long)(x))
#define PN(x) \
	SEQ_printf(m, "%-40s: %Ld.%06ld\n", #x, SPLIT_NS(x))
	/* 时钟以微秒小数展示，jiffies 和稳定性状态保持整数格式。 */
	PN(ktime);
	PN(sched_clk);
	PN(cpu_clk);
	P(jiffies);
	/* 只有架构声明 sched_clock 可能不稳定时才输出其运行时稳定性判断。 */
#ifdef CONFIG_HAVE_UNSTABLE_SCHED_CLOCK
	P(sched_clock_stable());
#endif
#undef PN
#undef P

	SEQ_printf(m, "\n");
	SEQ_printf(m, "sysctl_sched\n");

	/* 第二组局部宏为 sysctl 增加缩进，与上面的全局时钟字段区分。 */
#define P(x) \
	SEQ_printf(m, "  .%-40s: %Ld\n", #x, (long long)(x))
#define PN(x) \
	SEQ_printf(m, "  .%-40s: %Ld.%06ld\n", #x, SPLIT_NS(x))
	PN(sysctl_sched_base_slice);
	P(sysctl_sched_features);
#undef PN
#undef P

	/* scaling 同时输出枚举值和名称，数组索引依赖写路径的范围校验。 */
	SEQ_printf(m, "  .%-40s: %d (%s)\n",
		"sysctl_sched_tunable_scaling",
		sysctl_sched_tunable_scaling,
		sched_tunable_scaling_names[sysctl_sched_tunable_scaling]);
	SEQ_printf(m, "\n");
}

/* seq 位置 1 输出 header，其余编码为 CPU+2；成功恒返回 0。 */
/*
 * 业务背景：sched_debug seq iterator 把 header 哨兵和 CPU 编码对象分派给对应打印器。
 * 入参：m 是不可空输出 seq_file；v 是不可空编码值，1 表示 header，CPU+2 表示 CPU。
 * 出参/返回：成功恒返回 0；向 m 输出 header 或一个 CPU 段，无输出参数。
 * 注意事项：v 不是可解引用对象，只能按整数编码解释；错误编码可能产生无效 CPU 访问。
 */
static int sched_debug_show(struct seq_file *m, void *v)
{
	int cpu = (unsigned long)(v - 2);

	if (cpu != -1)
		print_cpu(m, cpu);
	else
		sched_debug_header(m);

	return 0;
}

/* SysRq 控制台入口：输出 header 和所有在线 CPU，并触碰 watchdog 避免长转储误报。 */
/*
 * 业务背景：紧急 SysRq 调度转储不能依赖 debugfs/seq_file，需要直接向控制台输出全部在线 CPU。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参数；打印 header/CPU 段并持续重置 NMI/softlockup watchdog。
 * 注意事项：可能在异常上下文执行且输出很长，不取得全局一致锁；不得调用会睡眠的路径。
 */
void sysrq_sched_debug_show(void)
{
	int cpu;

	sched_debug_header(NULL);
	for_each_online_cpu(cpu) {
		/*
		 * Need to reset softlockup watchdogs on all CPUs, because
		 * another CPU might be blocked waiting for us to process
		 * an IPI or stop_machine.
		 */
		touch_nmi_watchdog();
		touch_all_softlockup_watchdogs();
		print_cpu(NULL, cpu);
	}
}

/*
 * This iterator needs some explanation.
 * It returns 1 for the header position.
 * This means 2 is CPU 0.
 * In a hotplugged system some CPUs, including CPU 0, may be missing so we have
 * to use cpumask_* to iterate over the CPUs.
 */
/*
 * 位置 0 返回 header 哨兵 1，位置 CPU+1 返回编码 CPU+2；用 online mask
 * 跳过热拔插造成的洞，越过 nr_cpu_ids 时返回 NULL 结束迭代。
 */
/*
 * 业务背景：seq_file 迭代需在 CPU 热插拔有洞时用位置编码稳定地产生 header 和在线 CPU 项。
 * 入参：file 是未使用借用 seq_file；offset 是不可空、调用者拥有的输入/输出逻辑位置。
 * 出参/返回：返回编码哨兵 1 或 CPU+2，结束返回 NULL；同时把 *offset 更新到实际 CPU 位置。
 * 注意事项：返回值不可解引用；online mask 可并发变化，迭代允许跳过/变化但不得越过 nr_cpu_ids。
 */
static void *sched_debug_start(struct seq_file *file, loff_t *offset)
{
	unsigned long n = *offset;

	/* 逻辑位置零专门保留给 header，返回的整数 1 只是 seq 哨兵。 */
	if (n == 0)
		return (void *) 1;

	n--;

	/* 后续位置从上一 CPU 继续，首个 CPU 位置则从 online mask 起点开始。 */
	if (n > 0)
		n = cpumask_next(n - 1, cpu_online_mask);
	else
		n = cpumask_first(cpu_online_mask);

	*offset = n + 1;

	/* 有效 CPU 编码为 n+2，与 header 的 1 和结束 NULL 均不冲突。 */
	if (n < nr_cpu_ids)
		return (void *)(unsigned long)(n + 2);

	return NULL;
}

/* 推进 offset 后复用 start 的 hotplug-aware CPU 查找；末尾返回 NULL。 */
/*
 * 业务背景：seq_file next 要从当前逻辑位置继续查找下一个在线 CPU 编码项。
 * 入参：file 是借用 seq_file；data 是未使用的当前编码；offset 是不可空输入/输出位置。
 * 出参/返回：返回下一个编码项或 NULL；先递增 *offset，ownership 不变。
 * 注意事项：不持 hotplug 锁，语义继承 sched_debug_start；data 不是可解引用指针。
 */
static void *sched_debug_next(struct seq_file *file, void *data, loff_t *offset)
{
	(*offset)++;
	return sched_debug_start(file, offset);
}

/* 迭代器未取得长期锁或引用，stop 无需释放资源。 */
/*
 * 业务背景：seq_file 要求 stop 回调，但本迭代器没有在 start 中获取跨 show 持有的资源。
 * 入参：file 是未使用借用 seq_file；data 是未使用的最后编码值，可为 NULL。
 * 出参/返回：无直接返回值、无输出参数或状态副作用。
 * 注意事项：若将来 start 获取锁/引用，必须同步更新本函数释放；当前为空操作。
 */
static void sched_debug_stop(struct seq_file *file, void *data)
{
}

/* sops 将无资源迭代器和统一 show 回调组合，编码规则由 start/next 维护。 */
static const struct seq_operations sched_debug_sops = {
	.start		= sched_debug_start,
	.next		= sched_debug_next,
	.stop		= sched_debug_stop,
	.show		= sched_debug_show,
};

/* /proc/PID/sched 的局部打印宏统一标签宽度，并区分整数与纳秒小数。 */
#define __PS(S, F) SEQ_printf(m, "%-45s:%21Ld\n", S, (long long)(F))
#define __P(F) __PS(#F, F)
#define   P(F) __PS(#F, p->F)
#define   PM(F, M) __PS(#F, p->F & (M))
#define __PSN(S, F) SEQ_printf(m, "%-45s:%14Ld.%06ld\n", S, SPLIT_NS((long long)(F)))
#define __PN(F) __PSN(#F, F)
#define   PN(F) __PSN(#F, p->F)


#ifdef CONFIG_NUMA_BALANCING
/* 输出单 NUMA node 的 task/group 私有与共享 fault 计数。 */
/*
 * 业务背景：NUMA balancing 诊断需要按 node 展示 task/group 与 private/shared 两个维度的 fault 计数。
 * 入参：m 是可空输出目标；node 是 NUMA 节点号；tsf/tpf/gsf/gpf 是四类无符号计数输入。
 * 出参/返回：无直接返回值、无输出参数；追加一行 NUMA fault 统计，ownership 不变。
 * 注意事项：仅 NUMA_BALANCING；不校验 node 范围，计数是调用者提供的瞬时快照。
 */
void print_numa_stats(struct seq_file *m, int node, unsigned long tsf,
		unsigned long tpf, unsigned long gsf, unsigned long gpf)
{
	SEQ_printf(m, "numa_faults node=%d ", node);
	SEQ_printf(m, "task_private=%lu task_shared=%lu ", tpf, tsf);
	SEQ_printf(m, "group_private=%lu group_shared=%lu\n", gpf, gsf);
}
#endif


/* 配置启用时输出 @p 的 NUMA 扫描、迁移和分组快照；关闭时为空操作。 */
/*
 * 业务背景：/proc/PID/sched 的 NUMA 尾段要在同一入口兼容启用和关闭 NUMA balancing 的构建。
 * 入参：p 是不可空且生命周期稳定的只读借用 task；m 是不可空输出 seq_file。
 * 出参/返回：无直接返回值、无输出参数；启用配置时追加 NUMA 字段，关闭时无副作用。
 * 注意事项：不锁 mm/numa_group，字段可并发变化；p->mm 可空且必须检查后再访问。
 */
static void sched_show_numa(struct task_struct *p, struct seq_file *m)
{
#ifdef CONFIG_NUMA_BALANCING
	/* mm 可空，只在用户进程存在地址空间时输出扫描序列。 */
	if (p->mm)
		P(mm->numa_scan_seq);

	P(numa_pages_migrated);
	P(numa_preferred_nid);
	P(total_numa_faults);
	/* 当前 node/group 身份和逐 node fault 明细作为 NUMA 尾段输出。 */
	SEQ_printf(m, "current_node=%d, numa_group_id=%d\n",
			task_node(p), task_numa_group_id(p));
	show_numa_stats(p, m);
#endif /* CONFIG_NUMA_BALANCING */
}

/*
 * 为 /proc/PID/sched 输出 @p 在 @ns 可见的身份、EEVDF/PELT、切换、
 * schedstats、uclamp 与 NUMA 字段；调用者稳定 task，输出字段允许并发更新。
 */
/*
 * 业务背景：/proc/PID/sched 为单 task 提供调度器内部状态、统计和策略的可读诊断快照。
 * 入参：p 是不可空且生命周期稳定的借用 task；ns 是不可空 PID namespace 借用输入；m 是不可空输出 seq_file。
 * 出参/返回：无直接返回值、无输出参数；向 m 追加身份、EEVDF/PELT、统计、策略及 NUMA 字段。
 * 注意事项：不冻结 task/rq，跨字段可能不一致；配置项裁剪输出，除短暂本 CPU 读外不取得 ownership。
 */
void proc_sched_show_task(struct task_struct *p, struct pid_namespace *ns,
						  struct seq_file *m)
{
	unsigned long nr_switches;

	/* 身份行使用目标 PID namespace 可见编号，并同时展示线程组规模。 */
	SEQ_printf(m, "%s (%d, #threads: %d)\n", p->comm, task_pid_nr_ns(p, ns),
						get_nr_threads(p));
	SEQ_printf(m,
		"---------------------------------------------------------"
		"----------\n");

#define P_SCHEDSTAT(F)  __PS(#F, schedstat_val(p->stats.F))
#define PN_SCHEDSTAT(F) __PSN(#F, schedstat_val(p->stats.F))

	/* 开头固定输出 EEVDF 三个核心时间字段和累计切换/迁移基数。 */
	PN(se.exec_start);
	PN(se.vruntime);
	PN(se.sum_exec_runtime);

	nr_switches = p->nvcsw + p->nivcsw;

	P(se.nr_migrations);

	/* schedstats 关闭时整段跳过，避免把未维护字段误当作有效零值。 */
	if (schedstat_enabled()) {
		u64 avg_atom, avg_per_cpu;

		PN_SCHEDSTAT(sum_sleep_runtime);
		PN_SCHEDSTAT(sum_block_runtime);
		PN_SCHEDSTAT(wait_start);
		PN_SCHEDSTAT(sleep_start);
		PN_SCHEDSTAT(block_start);
		/* 等待/睡眠/阻塞的峰值与累计量用于定位长尾延迟。 */
		PN_SCHEDSTAT(sleep_max);
		PN_SCHEDSTAT(block_max);
		PN_SCHEDSTAT(exec_max);
		PN_SCHEDSTAT(slice_max);
		PN_SCHEDSTAT(wait_max);
		PN_SCHEDSTAT(wait_sum);
		P_SCHEDSTAT(wait_count);
		PN_SCHEDSTAT(iowait_sum);
		P_SCHEDSTAT(iowait_count);
		/* 迁移失败原因逐项展开，区分 affinity、running、hot 和强制迁移。 */
		P_SCHEDSTAT(nr_migrations_cold);
		P_SCHEDSTAT(nr_failed_migrations_affine);
		P_SCHEDSTAT(nr_failed_migrations_running);
		P_SCHEDSTAT(nr_failed_migrations_hot);
		P_SCHEDSTAT(nr_forced_migrations);
		P_SCHEDSTAT(nr_wakeups);
		/* wakeup 子计数按同步、迁移、本地/远端、亲和及 idle 等路径分类。 */
		P_SCHEDSTAT(nr_wakeups_sync);
		P_SCHEDSTAT(nr_wakeups_migrate);
		P_SCHEDSTAT(nr_wakeups_local);
		P_SCHEDSTAT(nr_wakeups_remote);
		P_SCHEDSTAT(nr_wakeups_affine);
		P_SCHEDSTAT(nr_wakeups_affine_attempts);
		P_SCHEDSTAT(nr_wakeups_passive);
		P_SCHEDSTAT(nr_wakeups_idle);

		/* avg_atom 用总 runtime/切换数，零切换以 -1 哨兵表示不可计算。 */
		avg_atom = p->se.sum_exec_runtime;
		if (nr_switches)
			avg_atom = div64_ul(avg_atom, nr_switches);
		else
			avg_atom = -1LL;

		/* avg_per_cpu 用迁移数归一，零迁移同样输出 -1 而非伪造除数。 */
		avg_per_cpu = p->se.sum_exec_runtime;
		if (p->se.nr_migrations) {
			avg_per_cpu = div64_u64(avg_per_cpu,
						p->se.nr_migrations);
		} else {
			avg_per_cpu = -1LL;
		}

		__PN(avg_atom);
		__PN(avg_per_cpu);

		/* core scheduling 配置再追加该 task 因 cookie 隔离损失的强制 idle 累计。 */
#ifdef CONFIG_SCHED_CORE
		PN_SCHEDSTAT(core_forceidle_sum);
#endif
	}

	__P(nr_switches);
	__PS("nr_voluntary_switches", p->nvcsw);
	__PS("nr_involuntary_switches", p->nivcsw);

	/* PELT 部分同时展示原始 sum、衰减 avg、更新时间和清除标志后的 util_est。 */
	P(se.load.weight);
	P(se.avg.load_sum);
	P(se.avg.runnable_sum);
	P(se.avg.util_sum);
	P(se.avg.load_avg);
	P(se.avg.runnable_avg);
	P(se.avg.util_avg);
	P(se.avg.last_update_time);
	PM(se.avg.util_est, ~UTIL_AVG_UNCHANGED);
	/* uclamp 配置区分 task 请求值与层级/系统约束后的 effective 值。 */
#ifdef CONFIG_UCLAMP_TASK
	__PS("uclamp.min", p->uclamp_req[UCLAMP_MIN].value);
	__PS("uclamp.max", p->uclamp_req[UCLAMP_MAX].value);
	__PS("effective uclamp.min", uclamp_eff_value(p, UCLAMP_MIN));
	__PS("effective uclamp.max", uclamp_eff_value(p, UCLAMP_MAX));
#endif /* CONFIG_UCLAMP_TASK */
	/* 最后按调度策略选择 DL 参数、fair slice 或 sched_ext 启用标志。 */
	P(policy);
	P(prio);
	if (task_has_dl_policy(p)) {
		P(dl.runtime);
		P(dl.deadline);
	} else if (fair_policy(p->policy)) {
		P(se.slice);
	}
	/* sched_ext 配置额外展示 task 是否已真正挂入 SCX，而非仅看 policy 数值。 */
#ifdef CONFIG_SCHED_CLASS_EXT
	__PS("ext.enabled", task_on_scx(p));
#endif
#undef PN_SCHEDSTAT
#undef P_SCHEDSTAT

	/* 连续两次本 CPU clock 读取的差值用于估计诊断时钟调用自身开销。 */
	{
		unsigned int this_cpu = raw_smp_processor_id();
		u64 t0, t1;

		t0 = cpu_clock(this_cpu);
		t1 = cpu_clock(this_cpu);
		__PS("clock-delta", t1-t0);
	}

	/* NUMA 子段由配置感知 helper 追加，关闭配置时保持空操作。 */
	sched_show_numa(p, m);
}

/* exec 等重置点清零 @p 的可选 schedstats；无配置时不产生副作用。 */
/*
 * 业务背景：exec 等任务语义重置点要清除旧映像的调度统计，避免 /proc/PID/sched 混合两代数据。
 * 入参：p 是不可空、由调用者独占重置阶段的输入/输出 task 借用对象。
 * 出参/返回：无直接返回值、无输出参数；SCHEDSTATS 配置下清零 p->stats，否则无副作用。
 * 注意事项：调用者必须保证没有并发统计写者；memset 不保留旧累计且操作不可恢复。
 */
void proc_sched_set_task(struct task_struct *p)
{
#ifdef CONFIG_SCHEDSTATS
	memset(&p->stats, 0, sizeof(p->stats));
#endif
}

/* need_resched 长期未调度时按小时限速打印 @cpu/@latency 并转储栈。 */
/*
 * 业务背景：CPU 长时间持有 need_resched 却不调度可能表示关抢占/中断缺陷，需要限速告警和栈定位。
 * 入参：cpu 是触发告警的有效 CPU 编号；latency 是已观测的纳秒延迟。
 * 出参/返回：无直接返回值、无输出参数；限速允许时打印 rq tick 信息并 dump_stack，否则无副作用。
 * 注意事项：全局 ratelimit 每小时一次，可能抑制其他 CPU 告警；诊断上下文不可依赖告警一定输出。
 */
void resched_latency_warn(int cpu, u64 latency)
{
	static DEFINE_RATELIMIT_STATE(latency_check_ratelimit, 60 * 60 * HZ, 1);

	if (likely(!__ratelimit(&latency_check_ratelimit)))
		return;

	pr_err("sched: CPU %d need_resched set for > %llu ns (%d ticks) without schedule\n",
	       cpu, latency, cpu_rq(cpu)->ticks_without_resched);
	dump_stack();
}
