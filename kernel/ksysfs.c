// SPDX-License-Identifier: GPL-2.0-only
/*
 * kernel/ksysfs.c - sysfs attributes in /sys/kernel, which
 * 		     are not related to any other subsystem
 *
 * Copyright (C) 2004 Kay Sievers <kay.sievers@vrfy.org>
 */
/*
 * 本文件集中实现不适合归入某个专门子系统的 /sys/kernel 通用接口。
 * ksysfs_init() 在驱动模型初始化之前创建全局 kernel_kobj，随后一次性发布普通
 * 文本属性，并按配置发布 uevent、profiling、vmcoreinfo 和 RCU 策略接口；x86
 * 等体系结构的后续 initcall 再以 kernel_kobj 为父对象扩展自己的目录。
 *
 * sysfs show/store 回调都在发起 read/write 的进程上下文执行，通常允许睡眠。
 * kobject/sysfs 核心保证回调期间属性对象仍有效，但不会替本文件同步其背后的
 * 全局变量；各变量必须依靠自身的原子操作、READ_ONCE 或专用互斥协议。
 */

#include <asm/byteorder.h>
#include <linux/kobject.h>
#include <linux/ksysfs.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/vmcore_info.h>
#include <linux/profile.h>
#include <linux/stat.h>
#include <linux/sched.h>
#include <linux/capability.h>
#include <linux/compiler.h>

#include <linux/rcupdate.h>	/* rcu_expedited and rcu_normal */

#if defined(__LITTLE_ENDIAN)
#define CPU_BYTEORDER_STRING	"little"
#elif defined(__BIG_ENDIAN)
#define CPU_BYTEORDER_STRING	"big"
#else
#error Unknown byteorder
#endif

/*
 * KERNEL_ATTR_RO/RW 把名为 X 的 show/store 回调与静态 kobj_attribute 绑定。
 * __ATTR_RO(X) 生成只读 X_attr，__ATTR_RW(X) 生成可读写 X_attr；这些对象随后
 * 进入 kernel_attrs 数组，由一次 sysfs_create_group() 原子式地组织为属性组。
 */
#define KERNEL_ATTR_RO(_name) \
static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

#define KERNEL_ATTR_RW(_name) \
static struct kobj_attribute _name##_attr = __ATTR_RW(_name)

/* current uevent sequence number */
/*
 * 当前 uevent 序列号。lib/kobject_uevent.c 每准备发送一个事件便原子递增它；
 * 这里仅获取瞬时快照，不承诺读出值与随后到达用户态的某个事件一一对应。
 *
 * 入参：@kobj 是借用的 kernel_kobj，@attr 是借用的 uevent_seqnum 属性描述符，
 * @buf 是 sysfs 提供的 PAGE_SIZE 输出缓冲区；三者所有权均不转移。
 * 返回写入字节数或 sysfs_emit() 的负错误码。回调可在进程上下文运行且不持锁；
 * atomic64_read() 与事件侧递增配对，保证不会撕裂 64 位计数值。
 */
static ssize_t uevent_seqnum_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", (u64)atomic64_read(&uevent_seqnum));
}
KERNEL_ATTR_RO(uevent_seqnum);

/* cpu byteorder */
/*
 * 输出构建当前内核所用体系结构字节序，而非某个进程或设备的可变属性。
 * @kobj/@attr 为借用的 sysfs 上下文，@buf 为输出缓冲区；无 ownership 转移。
 * 返回输出长度或负错误码。值由编译期宏固定，读取无锁、无状态副作用。
 */
static ssize_t cpu_byteorder_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", CPU_BYTEORDER_STRING);
}
KERNEL_ATTR_RO(cpu_byteorder);

/* address bits */
/*
 * 输出内核本身的指针宽度（位数），用于区分 32/64 位内核 ABI 环境。
 * @kobj/@attr 为借用输入，@buf 为 sysfs 输出缓冲区；返回输出长度或负错误码，
 * 不修改全局状态。sizeof(void *) 在编译时确定，注释中的 CHAR_BIT 表明这里按
 * 内核支持的 8 位字节换算，而不是报告物理/虚拟地址空间实际可用位数。
 */
static ssize_t address_bits_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	/* CHAR_BIT 表示一个字节包含的位数；内核支持的平台在这里均为 8。 */
	return sysfs_emit(buf, "%zu\n", sizeof(void *) * 8 /* CHAR_BIT */);
}
KERNEL_ATTR_RO(address_bits);

#ifdef CONFIG_UEVENT_HELPER
/* uevent helper program, used during early boot */
/*
 * CONFIG_UEVENT_HELPER 下展示当前用户态热插拔辅助程序路径。该机制主要供早期
 * 启动使用；成熟系统通常依赖 netlink uevent。@buf 接收对全局字符数组的快照，
 * @kobj/@attr 均为借用指针；返回输出长度或负错误码，无引用转移。
 */
static ssize_t uevent_helper_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", uevent_helper);
}
/*
 * 更新后续 uevent 要执行的 helper 路径。
 *
 * @kobj/@attr 是借用的属性上下文；@buf 是 sysfs 提供、长度为 @count 的只读
 * 输入，并不保证以 NUL 结尾；@count 单位为字节。路径必须能连同终止 NUL 放入
 * UEVENT_HELPER_PATH_LEN。成功返回原始 @count，即使末尾换行被剥除；过长返回
 * -ENOENT 且保持旧值。函数直接更新共享数组，不取得 @buf 所有权。
 *
 * 写入先复制完整内容、补 NUL，再去掉 echo 常带的换行。读者应注意此旧接口
 * 没有内部锁，show、sysctl 和事件发送路径可能看到并发写入中的瞬时内容；它是
 * 兼容性接口而非用于频繁并发重配置的事务配置面。
 */
static ssize_t uevent_helper_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	if (count+1 > UEVENT_HELPER_PATH_LEN)
		return -ENOENT;
	memcpy(uevent_helper, buf, count);
	uevent_helper[count] = '\0';
	if (count && uevent_helper[count-1] == '\n')
		uevent_helper[count-1] = '\0';
	return count;
}
KERNEL_ATTR_RW(uevent_helper);
#endif

#ifdef CONFIG_PROFILING
/*
 * 展示 legacy kernel profiling 是否已启用以及选择的 profiling 类型。
 * @kobj/@attr 为借用输入，@buf 为输出缓冲区；返回长度或负错误码，无副作用。
 * prof_on 由 profile_setup()/profile_init() 协议管理，零表示尚未启用。
 */
static ssize_t profiling_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", prof_on);
}
/*
 * 在运行中的内核里一次性启用 legacy profiling 基础设施。
 *
 * @kobj 是借用的 kernel_kobj，@attr 是借用的 profiling 属性描述符；二者只
 * 标识本次 sysfs 写入，不由本函数保存或释放。
 * @buf 是长度为 @count 的借用输入，内容沿用启动参数的 profile= 语法；其 const
 * 限定稍后只因 get_option() 的旧接口而被转换，不应被本层长期保存。成功返回
 * @count；已启用返回 -EEXIST；profile_init() 分配/参数失败时返回对应 errno。
 * 当前 create_proc_profile() 不传播 proc_create() 失败，但这里保留下游错误检查。
 * 失败不会完整回滚 profile_setup()/profile_init() 已改变的全局状态，
 * 因而这是一次性状态转换而不是可反复重试的事务。
 *
 * 静态 mutex 覆盖“检查 prof_on → 解析 → 分配 → 发布 proc 文件”全过程，防止
 * 两个 sysfs writer 重复分配 prof_buffer。guard(mutex) 在所有 return 路径离开
 * 作用域时自动解锁；函数运行在可睡眠进程上下文。
 */
static ssize_t profiling_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	int ret;
	static DEFINE_MUTEX(lock);

	/*
	 * We need serialization, for profile_setup() initializes prof_on
	 * value and profile_init() must not reallocate prof_buffer after
	 * once allocated.
	 */
	/*
	 * 必须串行化，因为 profile_setup() 会初始化 prof_on，而 profile_init() 在
	 * prof_buffer 分配一次后不得再次分配。锁把检查和发布合成单一状态转换；
	 * 省略它会允许两个 writer 都通过 prof_on 检查并争用全局缓冲区。
	 */
	guard(mutex)(&lock);
	if (prof_on)
		return -EEXIST;
	/*
	 * This eventually calls into get_option() which
	 * has a ton of callers and is not const.  It is
	 * easiest to cast it away here.
	 */
	/*
	 * 调用最终进入被大量调用且参数未标 const 的 get_option()；在这里去掉 const
	 * 是最小兼容边界。sysfs 输入仍只在本次回调有效，任何下游都不得保存该指针。
	 */
	profile_setup((char *)buf);
	/* 分配并初始化 profiling 缓冲区；成功后 prof_on 对外代表基础设施已激活。 */
	ret = profile_init();
	if (ret)
		return ret;
	/* 最后发布 /proc/profile 用户接口；到此成功才向 sysfs writer 返回完整写入。 */
	ret = create_proc_profile();
	if (ret)
		return ret;
	return count;
}
KERNEL_ATTR_RW(profiling);
#endif

#ifdef CONFIG_VMCORE_INFO

/*
 * 输出 vmcoreinfo ELF note 的物理地址与固定预留大小，供崩溃转储工具定位解析。
 * @kobj/@attr 为借用输入，@buf 为输出；返回输出长度或负错误码，无所有权转移。
 * paddr_vmcoreinfo_note() 允许体系结构覆盖默认地址计算；%pa 要求传物理地址变量
 * 的地址。这里展示的是启动时建立并持续维护的 note 区域，不负责创建它。
 */
static ssize_t vmcoreinfo_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	phys_addr_t vmcore_base = paddr_vmcoreinfo_note();
	return sysfs_emit(buf, "%pa %x\n", &vmcore_base,
			  (unsigned int)VMCOREINFO_NOTE_SIZE);
}
KERNEL_ATTR_RO(vmcoreinfo);

#endif /* CONFIG_VMCORE_INFO */

/* whether file capabilities are enabled */
/*
 * 输出本次启动是否允许从文件扩展属性获得 capabilities。file_caps_enabled 默认
 * 为 1，若启动参数禁用 file capabilities 则变为 0；本接口只读、不改变安全
 * 策略。@kobj/@attr 为借用输入，@buf 为输出，返回长度或负错误码。
 */
static ssize_t fscaps_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", file_caps_enabled);
}
KERNEL_ATTR_RO(fscaps);

#ifndef CONFIG_TINY_RCU
/*
 * RCU 全局策略量由启动参数、模块参数和本文件的 sysfs 属性共同使用。
 * rcu_expedited 非零请求后续同步宽限期采用 expedited 路径；rcu_normal 非零则
 * 强制普通路径并在两者同时设置时优先。它们是策略开关而非引用计数，RCU 核心
 * 在决策点读取；TINY_RCU 不提供这组可调策略。
 */
int rcu_expedited;
/*
 * 展示 rcu_expedited 的单次一致快照。@kobj/@attr 为借用输入，@buf 为输出；
 * 返回长度或负错误码。READ_ONCE 防止编译器合并/拆分这次共享变量访问，但它
 * 不建立跨 CPU 的事务，也不等待正在进行的宽限期改变实现。
 */
static ssize_t rcu_expedited_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(rcu_expedited));
}
/*
 * 解析并立即发布 rcu_expedited 新值。@buf 是 @count 字节的借用文本输入，基数
 * 0 允许十进制及带前缀形式；成功返回 @count，解析失败返回 -EINVAL。kstrtoint
 * 只在完整转换成功时写目标，因此失败保持旧值。这里没有 ownership 转移；并发
 * writer 以最后一次完整整数写入为准，正在进行的宽限期不被追溯转换。
 * @kobj 是借用的根对象，@attr 是借用的属性描述符，二者均不被修改或保存。
 */
static ssize_t rcu_expedited_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	if (kstrtoint(buf, 0, &rcu_expedited))
		return -EINVAL;

	return count;
}
KERNEL_ATTR_RW(rcu_expedited);

/*
 * rcu_normal 是普通宽限期的强制开关；与上面的 rcu_expedited 共享静态寿命，
 * 由 RCU 核心借用读取，sysfs 写入不转移 ownership。
 */
int rcu_normal;
/*
 * 展示强制普通 RCU 宽限期策略的瞬时值。参数和返回约定同
 * rcu_expedited_show()；READ_ONCE 只保证本次标量读取，不冻结 RCU 其他状态。
 */
static ssize_t rcu_normal_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(rcu_normal));
}
/*
 * 解析并发布 rcu_normal。@buf/@count 是借用的文本及字节数，成功返回 @count，
 * 非法整数返回 -EINVAL 且旧值不变。非零值在 RCU 决策函数中压过 expedited
 * 请求，但启动早期 RCU 强制加速窗口仍由 rcu_scheduler_active 单独约束。
 * @kobj 和 @attr 是 sysfs 核心在回调期间保持有效的借用指针，无 ownership 转移。
 */
static ssize_t rcu_normal_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	if (kstrtoint(buf, 0, &rcu_normal))
		return -EINVAL;

	return count;
}
KERNEL_ATTR_RW(rcu_normal);
#endif /* #ifndef CONFIG_TINY_RCU */

/*
 * Make /sys/kernel/notes give the raw contents of our kernel .notes section.
 */
/*
 * 让 /sys/kernel/notes 原样暴露内核链接产物的 .notes 段。起止符号由链接脚本
 * 提供，不是普通 C 对象；两地址相减得到连续区间字节数，读取者借助 ELF note
 * 头解析 build-id 等记录。bin_attr_notes 在初始化时补入地址和大小，之后标记为
 * __ro_after_init，避免运行期篡改 sysfs 二进制文件的后端范围。
 */
extern const void __start_notes;
extern const void __stop_notes;
#define	notes_size (&__stop_notes - &__start_notes)

static __ro_after_init BIN_ATTR_SIMPLE_RO(notes);

/*
 * /sys/kernel 的全局 kobject 根。ksysfs_init() 创建并持有其初始引用，成功发布后
 * 各子系统只借用这个指针来建立子对象；它被导出给 GPL 模块。对象预期贯穿内核
 * 运行期，初始化失败路径会 put 并留下错误日志，之后不得假定它非 NULL。
 */
struct kobject *kernel_kobj;
EXPORT_SYMBOL_GPL(kernel_kobj);

/*
 * 通用文本属性清单。每个元素借用静态 kobj_attribute 中嵌入的 attribute，NULL
 * 为 sysfs 遍历终止哨兵；条件编译决定当前内核实际可见的文件集合。数组和属性
 * 都是静态寿命，因此属性组发布后不需要额外引用管理。
 */
static struct attribute * kernel_attrs[] = {
	&fscaps_attr.attr,
	&uevent_seqnum_attr.attr,
	&cpu_byteorder_attr.attr,
	&address_bits_attr.attr,
#ifdef CONFIG_UEVENT_HELPER
	&uevent_helper_attr.attr,
#endif
#ifdef CONFIG_PROFILING
	&profiling_attr.attr,
#endif
#ifdef CONFIG_VMCORE_INFO
	&vmcoreinfo_attr.attr,
#endif
#ifndef CONFIG_TINY_RCU
	&rcu_expedited_attr.attr,
	&rcu_normal_attr.attr,
#endif
	NULL
};

/* 把上述文本属性作为 /sys/kernel 根上的一个匿名组统一创建与统一撤销。 */
static const struct attribute_group kernel_attr_group = {
	.attrs = kernel_attrs,
};

/*
 * ksysfs_init() - 创建 /sys/kernel 根并发布通用文本及 notes 二进制属性。
 *
 * 调用链：kernel_init_freeable() → do_basic_setup() → ksysfs_init()，随后
 * driver_init() 和各级 initcall 才会把子目录挂到 kernel_kobj 下。
 *
 * 入参：无。运行在单线程推进基础设施的启动期进程上下文，可睡眠，不持有需要
 * 本函数继承的锁。返回：无直接返回值。成功时 kernel_kobj 和属性均已对用户态
 * sysfs 访问及后续内核子系统可见；任一失败都逆序撤销本函数已建立的对象并记录
 * errno，但不阻止启动。notes_size 为零时只跳过二进制文件，不视为失败。
 *
 * ownership：创建成功取得 kernel_kobj 初始引用并将其作为全局长寿命根保留；
 * 失败路径由本函数 put。属性描述符均为静态对象，sysfs 只建立目录项关联。
 */
void __init ksysfs_init(void)
{
	int error;

	/* 阶段 1：先发布根对象；没有父对象意味着它直接位于 sysfs 顶层。 */
	kernel_kobj = kobject_create_and_add("kernel", NULL);
	if (!kernel_kobj) {
		error = -ENOMEM;
		goto exit;
	}
	/* 阶段 2：一次发布普通属性组；失败时根对象仍由当前函数持有。 */
	error = sysfs_create_group(kernel_kobj, &kernel_attr_group);
	if (error)
		goto kset_exit;

	/*
	 * 阶段 3：链接器确实提供非空 .notes 段时，才补齐只读二进制属性后端并发布。
	 * private 是 BIN_ATTR_SIMPLE_RO 读取 helper 的源地址，size 限制所有 offset/count，
	 * 因而必须在 sysfs_create_bin_file() 使文件可见之前完成初始化。
	 */
	if (notes_size > 0) {
		bin_attr_notes.private = (void *)&__start_notes;
		bin_attr_notes.size = notes_size;
		error = sysfs_create_bin_file(kernel_kobj, &bin_attr_notes);
		if (error)
			goto group_exit;
	}

	return;

group_exit:
	/* notes 发布失败：普通组已可见，先撤销组，再释放根对象引用。 */
	sysfs_remove_group(kernel_kobj, &kernel_attr_group);
kset_exit:
	/* 根已创建但后续阶段失败；put 触发从 sysfs 摘除并最终释放 kobject。 */
	kobject_put(kernel_kobj);
exit:
	/* 根创建失败时 error 已显式设为 -ENOMEM；其他路径保留下游 errno。 */
	pr_err("failed to initialize the kernel kobject: %d\n", error);
}
