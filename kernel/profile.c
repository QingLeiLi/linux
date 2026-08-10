// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/kernel/profile.c
 *  Simple profiling. Manages a direct-mapped profile hit count buffer,
 *  with configurable resolution, support for restricting the cpus on
 *  which profiling is done, and switching between cpu time and
 *  schedule() calls via kernel command line parameters passed at boot.
 *
 *  Scheduler profiling support, Arjan van de Ven and Ingo Molnar,
 *	Red Hat, July 2004
 *  Consolidation of architecture support code for profiling,
 *	Nadia Yvette Chambers, Oracle, July 2004
 *  Amortized hit count accounting via per-cpu open-addressed hashtables
 *	to resolve timer interrupt livelocks, Nadia Yvette Chambers,
 *	Oracle, 2004
 */
/*
 * 本文件实现旧式内核文本地址采样器：把 _stext.._etext 按 prof_shift 划分为
 * direct-mapped 原子计数槽，时钟中断或调度/KVM 埋点累加命中，再通过
 * /proc/profile 输出“采样步长 + 原始计数数组”。kernel/ksysfs.c 的 profiling
 * 属性复用 profile_setup() → profile_init() → create_proc_profile()，允许启动后
 * 首次启用，但整个设计仍是一次性全局设施，不支持安全停用或重新分配。
 */

#include <linux/export.h>
#include <linux/profile.h>
#include <linux/memblock.h>
#include <linux/notifier.h>
#include <linux/mm.h>
#include <linux/cpumask.h>
#include <linux/cpu.h>
#include <linux/highmem.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/sched/stat.h>

#include <asm/sections.h>
#include <asm/irq_regs.h>
#include <asm/ptrace.h>

/*
 * profile_hit 是体系结构批量摊还命中的紧凑条目：pc 保存槽索引，hits 保存该槽
 * 待合并次数。每页划为若干 8 项分组，用开放寻址降低高频中断直接争用全局
 * atomic_t 计数器的成本；条目本身由使用它的体系结构路径管理。
 */
struct profile_hit {
	u32 pc, hits;
};
/* 每组 2^3=8 项；一页可容纳的条目数和组数都由结构大小派生。 */
#define PROFILE_GRPSHIFT	3
#define PROFILE_GRPSZ		(1 << PROFILE_GRPSHIFT)
#define NR_PROFILE_HIT		(PAGE_SIZE/sizeof(struct profile_hit))
#define NR_PROFILE_GRP		(NR_PROFILE_HIT/PROFILE_GRPSZ)

/*
 * prof_buffer 指向每个采样槽的原子计数数组，profile_init() 分配后运行期不替换；
 * prof_len 是槽数；prof_shift 是地址右移位数，因而采样步长为 1UL<<prof_shift。
 * 三者由初始化路径写、采样和 proc 读取路径并发读，缓冲区贯穿运行期且不释放。
 */
static atomic_t *prof_buffer;
static unsigned long prof_len;
static unsigned short int prof_shift;

/*
 * 当前 profiling 类型：0 表示关闭，其他值对应 CPU/SCHED/KVM profiling。
 * __read_mostly 优化频繁读取布局；profile_setup() 写入，采样入口据此过滤类型。
 */
int prof_on __read_mostly;
EXPORT_SYMBOL_GPL(prof_on);

/*
 * profile_setup() - 按 profile= 启动参数语法选择采样类型和分辨率。
 * @str 是可写、NUL 结尾的借用字符串；get_option() 会推进局部指针但不保存它。
 * "schedule[,shift]"、"kvm[,shift]" 选择对应事件源，整数选择 CPU profiling；
 * shift 被夹在 0..BITS_PER_LONG-1。返回恒为 1，表示 __setup 已消费该参数；无
 * 动态 ownership。函数只配置全局量，真正缓冲区由后续 profile_init() 分配。
 */
int profile_setup(char *str)
{
	/* schedstr/kvmstr 为静态关键字；select 标记命中的命名模式；par 接收数值 shift。 */
	static const char schedstr[] = "schedule";
	static const char kvmstr[] = "kvm";
	const char *select = NULL;
	int par;

	/* 阶段 1：选择事件源；只有成功识别时才把 prof_on 从 0 切到启用类型。 */
	if (!strncmp(str, schedstr, strlen(schedstr))) {
		prof_on = SCHED_PROFILING;
		select = schedstr;
	} else if (!strncmp(str, kvmstr, strlen(kvmstr))) {
		prof_on = KVM_PROFILING;
		select = kvmstr;
	} else if (get_option(&str, &par)) {
		prof_shift = clamp(par, 0, BITS_PER_LONG - 1);
		prof_on = CPU_PROFILING;
		pr_info("kernel profiling enabled (shift: %u)\n",
			prof_shift);
	}

	/* 阶段 2：命名模式允许逗号后的可选 shift；缺省则保留初值 0。 */
	if (select) {
		if (str[strlen(select)] == ',')
			str += strlen(select) + 1;
		if (get_option(&str, &par))
			prof_shift = clamp(par, 0, BITS_PER_LONG - 1);
		pr_info("kernel %s profiling enabled (shift: %u)\n",
			select, prof_shift);
	}

	return 1;
}
__setup("profile=", profile_setup);


/*
 * profile_init() - 根据文本范围和 shift 分配全局原子采样缓冲区。
 * 入参：无。由启动路径或 ksysfs profiling_store() 在可睡眠进程上下文调用；
 * 调用者必须保证串行且 prof_buffer 尚未分配。未启用直接返回 0；shift 使槽数为
 * 零时清除 prof_on 并返回 -EINVAL；三种分配方式均失败返回 -ENOMEM（prof_on
 * 仍保持启用，因此上层不能把失败当作完整回滚）；成功返回 0 并持有运行期缓冲区。
 * __ref 允许 init/非 init 调用关系，不意味着返回引用。
 */
int __ref profile_init(void)
{
	/* buffer_bytes 是 prof_len 个 atomic_t 所需字节数，只在本次分配阶段有效。 */
	int buffer_bytes;
	if (!prof_on)
		return 0;

	/* only text is profiled */
	/* 只覆盖内核 .text；右移把字节偏移映射为固定分辨率槽索引。 */
	prof_len = (_etext - _stext) >> prof_shift;

	if (!prof_len) {
		pr_warn("profiling shift: %u too large\n", prof_shift);
		prof_on = 0;
		return -EINVAL;
	}

	buffer_bytes = prof_len*sizeof(atomic_t);

	/*
	 * 按低开销到高兼容性依次尝试 kmalloc、连续页和 vmalloc；全部要求零填充，
	 * __GFP_NOWARN 避免前两种预期退化产生噪声。任一成功即提交全局缓冲区。
	 */
	prof_buffer = kzalloc(buffer_bytes, GFP_KERNEL|__GFP_NOWARN);
	if (prof_buffer)
		return 0;

	prof_buffer = alloc_pages_exact(buffer_bytes,
					GFP_KERNEL|__GFP_ZERO|__GFP_NOWARN);
	if (prof_buffer)
		return 0;

	prof_buffer = vzalloc(buffer_bytes);
	if (prof_buffer)
		return 0;

	return -ENOMEM;
}

/*
 * do_profile_hits() - 把某文本地址的批量命中原子累加到直接映射槽。
 * @type 保留与公共入口一致的类型上下文，过滤已由调用者完成；@__pc 是借用的
 * 指令地址；@nr_hits 是累加次数。返回无直接值。地址落在 prof_len 外时忽略；
 * atomic_add 允许多个 CPU 并发更新同一槽，但不提供采样事件间的全局顺序。
 */
static void do_profile_hits(int type, void *__pc, unsigned int nr_hits)
{
	unsigned long pc;
	pc = ((unsigned long)__pc - (unsigned long)_stext) >> prof_shift;
	if (pc < prof_len)
		atomic_add(nr_hits, &prof_buffer[pc]);
}

/*
 * profile_hits() - 公共批量采样入口，先校验启用类型与缓冲区发布状态。
 * @type 是事件源类型；@__pc 是借用指令地址；@nr_hits 是正向累加量。返回无直接
 * 值。未启用、类型不匹配或缓冲区尚未分配时快速忽略，否则原子累计，无 ownership
 * 转移且不可睡眠，可供低层/中断相关路径调用。
 */
void profile_hits(int type, void *__pc, unsigned int nr_hits)
{
	if (prof_on != type || !prof_buffer)
		return;
	do_profile_hits(type, __pc, nr_hits);
}
EXPORT_SYMBOL_GPL(profile_hits);

/*
 * profile_tick() - 在 profiling 时钟 tick 上采样被中断的内核 PC。
 * @type 标识 tick 所属 profiling 类型。返回无直接值；借用当前中断 pt_regs，
 * 用户态被中断时不计数，内核态则经 profile_hit() 进入对应槽。函数不可睡眠。
 */
void profile_tick(int type)
{
	struct pt_regs *regs = get_irq_regs();

	/* This is the old kernel-only legacy profiling */
	/* 这是仅采样内核地址的旧式 profiling；用户态 PC 明确排除。 */
	if (!user_mode(regs))
		profile_hit(type, (void *)profile_pc(regs));
}

#ifdef CONFIG_PROC_FS
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

/*
 * This function accesses profiling information. The returned data is
 * binary: the sampling step and the actual contents of the profile
 * buffer. Use of the program readprofile is recommended in order to
 * get meaningful info out of these data.
 */
/*
 * read_profile() - 读取 /proc/profile 的二进制 ABI。
 * @file 为借用文件；@buf 是用户输出缓冲区；@count 是请求字节数；@ppos 是输入
 * 输出文件偏移。开头一个 unsigned int 是采样步长，随后是 prof_buffer 原始槽；
 * 推荐 readprofile 解码。成功返回复制字节数并推进 *ppos，EOF 返回 0，用户拷贝
 * 失败返回 -EFAULT。函数不锁住采样器，所得计数是并发更新中的近似快照。
 */
static ssize_t
read_profile(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	/* p/read 跟踪本次窗口；pnt 指向计数区起点；sample_step 是 ABI 头值。 */
	unsigned long p = *ppos;
	ssize_t read;
	char *pnt;
	unsigned long sample_step = 1UL << prof_shift;

	if (p >= (prof_len+1)*sizeof(unsigned int))
		return 0;
	if (count > (prof_len+1)*sizeof(unsigned int) - p)
		count = (prof_len+1)*sizeof(unsigned int) - p;
	read = 0;

	/* 阶段 1：请求覆盖头部时逐字节安全写入用户空间。 */
	while (p < sizeof(unsigned int) && count > 0) {
		if (put_user(*((char *)(&sample_step)+p), buf))
			return -EFAULT;
		buf++; p++; count--; read++;
	}
	/* 阶段 2：p 已越过同尺寸头部，换算到原子计数数组并批量复制剩余窗口。 */
	pnt = (char *)prof_buffer + p - sizeof(atomic_t);
	if (copy_to_user(buf, (void *)pnt, count))
		return -EFAULT;
	read += count;
	*ppos += read;
	return read;
}

/* default is to not implement this call */
/*
 * 默认体系结构不支持动态调整 profiling 中断频率。弱符号允许体系结构提供强定义；
 * @mult 是用户请求的倍率，默认实现始终返回 -EINVAL 且无副作用。
 */
int __weak setup_profiling_timer(unsigned mult)
{
	return -EINVAL;
}

/*
 * Writing to /proc/profile resets the counters
 *
 * Writing a 'profiling multiplier' value into it also re-sets the profiling
 * interrupt frequency, on architectures that support this.
 */
/*
 * write_profile() - 清零所有计数，并可选调整体系结构 profiling 频率。
 * @file/@ppos 为借用上下文；@buf 是用户输入；@count 是字节数。SMP 下恰好写入
 * sizeof(int) 时把值解释为 multiplier，拷贝失败返回 -EFAULT、体系结构拒绝返回
 * -EINVAL，且此时不清零；其他写入直接清零。成功返回原 @count。memset 与采样
 * atomic_add 可并发，接口只承诺重置操作，不提供采样停止后的严格事务快照。
 */
static ssize_t write_profile(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
#ifdef CONFIG_SMP
	if (count == sizeof(int)) {
		unsigned int multiplier;

		if (copy_from_user(&multiplier, buf, sizeof(int)))
			return -EFAULT;

		if (setup_profiling_timer(multiplier))
			return -EINVAL;
	}
#endif
	memset(prof_buffer, 0, prof_len * sizeof(atomic_t));
	return count;
}

/* /proc/profile 把二进制读取、重置写入和默认 seek 绑定到同一个静态操作表。 */
static const struct proc_ops profile_proc_ops = {
	.proc_read	= read_profile,
	.proc_write	= write_profile,
	.proc_lseek	= default_llseek,
};

/*
 * create_proc_profile() - 在 profiling 启用时发布 /proc/profile。
 * 入参：无；未启用直接返回 0。启用时以 root 可读、root 可写模式创建 proc 项，
 * 并把文件大小设为一个步长头加 prof_len 个 atomic_t。当前实现即使 proc_create()
 * 返回 NULL 也返回 0，因此调用者不能由返回值确认文件存在；entry 由 procfs 管理，
 * 无需本函数释放。__ref 支持启动 initcall 与运行期 ksysfs 两种调用位置。
 */
int __ref create_proc_profile(void)
{
	struct proc_dir_entry *entry;
	int err = 0;

	if (!prof_on)
		return 0;
	entry = proc_create("profile", S_IWUSR | S_IRUGO,
			    NULL, &profile_proc_ops);
	if (entry)
		proc_set_size(entry, (1 + prof_len) * sizeof(atomic_t));
	return err;
}
subsys_initcall(create_proc_profile);
#endif /* CONFIG_PROC_FS */
