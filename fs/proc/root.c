// SPDX-License-Identifier: GPL-2.0
/*
 * procfs 根目录与挂载生命周期学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件是 procfs 与 VFS 的接合层，负责三件事：
 *   1. 把 mount/fsopen/fsconfig 传入的 gid、hidepid、subset、pidns 参数
 *      解析为一次挂载私有的配置；
 *   2. 由配置创建、重配置和销毁 procfs superblock，并在内核启动时注册
 *      "proc" 文件系统类型；
 *   3. 为 /proc 根目录把静态 proc_dir_entry 树与按 PID 命名、随进程
 *      生命周期变化的动态目录合成一个 VFS 目录视图。
 *
 * 主调用链：
 *   proc_root_init() -> register_filesystem()
 *   fsopen/mount -> proc_init_fs_context() -> proc_parse_param()
 *                -> proc_get_tree() -> proc_fill_super()
 *   remount/fsconfig -> proc_reconfigure() -> proc_apply_options()
 *   unmount -> proc_kill_sb()
 *   lookup/readdir/stat("/proc") -> proc_root_lookup()/proc_root_readdir()/
 *                                   proc_root_getattr()
 *
 * 核心对象与生命周期：
 *   proc_fs_context 只属于一次 VFS fs_context，暂存“本次明确指定了哪些
 *   选项”及 PID namespace 引用；proc_fs_context_free() 最终释放它。
 *   proc_fs_info 属于一个已建立的 superblock，保存运行期可查询的挂载策略、
 *   PID namespace 和挂载者凭据引用；proc_kill_sb() 在卸载后经 RCU 延迟释放。
 *   proc_root 是全局静态 PDE，贯穿系统整个生命周期；每个 procfs superblock
 *   只为它创建自己的 VFS inode/dentry，不复制这棵全局静态目录描述树。
 *
 * 并发模型：
 *   建树阶段的对象尚未发布，依靠 fs_context/VFS 挂载串行化完成初始化；
 *   可在线修改的 hidepid/gid 选项由 VFS 重配置路径先同步文件系统再写入。
 *   pid_ns 不能在线替换，否则所有无 RCU 保护的 proc_sb_info()/proc_pid_ns()
 *   读者都可能与引用释放竞争。动态 PID 查找和枚举的 RCU、task 引用协议
 *   则由 fs/proc/base.c 的 proc_pid_lookup()/proc_pid_readdir() 实现。
 *
 * 方案权衡：
 *   全局 PDE 树复用大量静态元数据，而每个挂载只保留 namespace 和可见性
 *   策略，创建成本较低；代价是根目录必须专门拼接静态条目与动态 PID
 *   条目，且某些影响读侧对象身份的选项不能原地重配置。
 */
/*
 *  linux/fs/proc/root.c
 *
 *  Copyright (C) 1991, 1992 Linus Torvalds
 *
 *  proc root directory handling functions
 */
/*
 * 本文件实现 procfs 根目录的处理函数。这里的“根目录处理”不只是目录
 * 操作：它还涵盖 procfs 文件系统类型的注册、挂载上下文、superblock
 * 生命周期，以及 /proc 下静态节点和动态 PID 目录的统一呈现。
 */
#include <linux/errno.h>
#include <linux/time.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/sched/stat.h>
#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/user_namespace.h>
#include <linux/fs_context.h>
#include <linux/mount.h>
#include <linux/pid_namespace.h>
#include <linux/fs_parser.h>
#include <linux/cred.h>
#include <linux/magic.h>
#include <linux/slab.h>

#include "internal.h"

/*
 * proc_fs_context 表示尚在构造或重配置中的一次 procfs 配置事务。
 *
 * pid_ns 持有一个 PID namespace 引用，决定此挂载中的数字 PID 解释域；
 * mask 的第 Opt_* 位记录对应参数是否由调用者明确给出，使默认零值和
 * “显式设置为零”仍可区分；hidepid/gid/pidonly 保存待提交的策略值。
 * 对象由 proc_init_fs_context() 分配并挂到 fc->fs_private，解析期间只由
 * 当前 fs_context 路径修改，最后由 proc_fs_context_free() 释放。
 */
struct proc_fs_context {
	struct pid_namespace	*pid_ns;
	unsigned int		mask;
	enum proc_hidepid	hidepid;
	int			gid;
	enum proc_pidonly	pidonly;
};

/*
 * 每个枚举值既是 fs_parse() 返回的分派编号，也是 ctx->mask 的位号；
 * 因此增加选项时必须保持枚举值可用于 “1 << opt”，并同步参数表和
 * switch。
 */
enum proc_param {
	Opt_gid,
	Opt_hidepid,
	Opt_subset,
	Opt_pidns,
};

/*
 * VFS fs_parser 的声明式参数表：
 *   gid      接受 u32，随后映射到挂载所处 user namespace 的 kgid_t；
 *   hidepid  保留字符串形式，以同时兼容数字值和可读名称；
 *   subset   当前只接受逗号分隔的 pid；
 *   pidns    可由 fsconfig 文件描述符或传统路径字符串指定。
 * 末尾空项是 fs_parse() 识别表结束的哨兵。该表被 fs_type 和解析回调共享，
 * 生命周期为整个内核运行期，初始化后只读。
 */
static const struct fs_parameter_spec proc_fs_parameters[] = {
	fsparam_u32("gid",		Opt_gid),
	fsparam_string("hidepid",	Opt_hidepid),
	fsparam_string("subset",	Opt_subset),
	fsparam_file_or_string("pidns",	Opt_pidns),
	{}
};

/*
 * valid_hidepid() - 判断数值是否是当前实现支持的离散 hidepid 策略。
 *
 * 调用者是 proc_parse_hidepid_param() 的数字兼容路径。value 是用户输入
 * 解析后的无符号纯输入值，无所有权变化、无锁要求、不会睡眠。
 * 返回 1 表示可安全转换为 enum proc_hidepid，返回 0 表示必须拒绝；
 * 枚举值不是连续区间（NOT_PTRACEABLE 为 4），所以不能用简单上下界检查。
 */
static inline int valid_hidepid(unsigned int value)
{
	return (value == HIDEPID_OFF ||
		value == HIDEPID_NO_ACCESS ||
		value == HIDEPID_INVISIBLE ||
		value == HIDEPID_NOT_PTRACEABLE);
}

/*
 * proc_parse_hidepid_param() - 解析 hidepid= 并暂存到挂载上下文。
 *
 * fc 是 VFS 持有的输入输出 fs_context，函数借用其中的 fs_private；
 * param 是本次参数的借用描述，必须携带 NUL 结尾字符串。解析路径允许
 * 睡眠，入口不要求调用者持有 procfs 锁。
 *
 * 数字形式兼容 0/1/2/4，名称形式接受 off/noaccess/invisible/ptraceable。
 * 成功返回 0，只更新 ctx->hidepid；是否“明确给出”由外层
 * proc_parse_param() 在整个选项成功后设置 mask。类型或内容无效时返回
 * invalf() 生成的负 errno，并把诊断记录进 fc，旧的已提交配置不受影响。
 */
static int proc_parse_hidepid_param(struct fs_context *fc, struct fs_parameter *param)
{
	/*
	 * 变量地图：
	 *   ctx              本次配置事务，借用自 fc；
	 *   hidepid_u32_spec 复用 fs_parser 对 u32 参数的进制约定；
	 *   result           仅在数字转换成功时承载 uint_32；
	 *   base             从参数规格中取出的转换进制。
	 */
	struct proc_fs_context *ctx = fc->fs_private;
	struct fs_parameter_spec hidepid_u32_spec = fsparam_u32("hidepid", Opt_hidepid);
	struct fs_parse_result result;
	int base = (unsigned long)hidepid_u32_spec.data;

	if (param->type != fs_value_is_string)
		return invalf(fc, "proc: unexpected type of hidepid value\n");

	/*
	 * 先尝试历史数字 ABI；kstrtouint() 成功后还要验证离散枚举集合，
	 * 避免把未定义的数值写入运行期权限判断。
	 */
	if (!kstrtouint(param->string, base, &result.uint_32)) {
		if (!valid_hidepid(result.uint_32))
			return invalf(fc, "proc: unknown value of hidepid - %s\n", param->string);
		ctx->hidepid = result.uint_32;
		return 0;
	}

	/*
	 * 数字转换失败不立即报错，因为同一个选项还支持语义等价的
	 * 名称。名称逐项映射能保持用户 ABI 可读，最终仍只存统一枚举。
	 */
	if (!strcmp(param->string, "off"))
		ctx->hidepid = HIDEPID_OFF;
	else if (!strcmp(param->string, "noaccess"))
		ctx->hidepid = HIDEPID_NO_ACCESS;
	else if (!strcmp(param->string, "invisible"))
		ctx->hidepid = HIDEPID_INVISIBLE;
	else if (!strcmp(param->string, "ptraceable"))
		ctx->hidepid = HIDEPID_NOT_PTRACEABLE;
	else
		return invalf(fc, "proc: unknown value of hidepid - %s\n", param->string);

	return 0;
}

/*
 * proc_parse_subset_param() - 原地解析 subset= 的逗号分隔子集列表。
 *
 * fc/ctx 均由当前配置事务持有；value 指向 fs_parser 提供的可写字符串，
 * 函数会把逗号改成 '\0'，因此它是输入输出缓冲区，返回后不能再假定
 * 原串保持完整。当前唯一合法的非空 token 是 "pid"，它要求此挂载只
 * 显示 PID 相关视图。空 token 被忽略，用于容忍连续逗号或尾逗号。
 *
 * 成功返回 0 并可能把 ctx->pidonly 置为 ON；遇到未知 token 返回负 errno
 * 并记录诊断。函数无引用转移、无锁要求，也不发布运行期状态。
 */
static int proc_parse_subset_param(struct fs_context *fc, char *value)
{
	struct proc_fs_context *ctx = fc->fs_private;

	/* 每轮 value 指向尚未消费的 token，ptr 保存下一 token 的起点。 */
	while (value) {
		char *ptr = strchr(value, ',');

		if (ptr != NULL)
			*ptr++ = '\0';

		/*
		 * 只有完整识别当前 token 后才推进；未知值立即失败，外层
		 * 不会设置 Opt_subset 的 mask 位，因而不会提交不完整解析。
		 */
		if (*value != '\0') {
			if (!strcmp(value, "pid")) {
				ctx->pidonly = PROC_PIDONLY_ON;
			} else {
				return invalf(fc, "proc: unsupported subset option - %s\n", value);
			}
		}
		value = ptr;
	}

	return 0;
}

#ifdef CONFIG_PID_NS
/*
 * proc_parse_pidns_param() - 校验 pidns= 指定的 namespace 并替换暂存引用。
 *
 * fc/ctx 是输入输出配置事务；param 借用参数，值可以是 fsconfig 传入的
 * struct file 或可打开的路径；result 当前未使用，只为与解析回调形态一致。
 * 函数在进程上下文执行，打开路径和权限检查都可能睡眠，入口不持
 * procfs 锁。
 *
 * 成功必须同时完成两项引用替换：ctx->pid_ns 指向目标 PID namespace，
 * fc->user_ns 指向该 PID namespace 的拥有者 user namespace。旧引用先 put，
 * 新引用通过 get 持有，最终分别由 context free/VFS 释放。失败返回具体
 * 负 errno，自动 cleanup 释放临时文件引用，ctx 与 fc 的 namespace 不变。
 * 仅在 CONFIG_PID_NS=y 时存在。
 */
static int proc_parse_pidns_param(struct fs_context *fc,
				  struct fs_parameter *param,
				  struct fs_parse_result *result)
{
	/*
	 * 变量地图：
	 *   target  从 nsfs inode 恢复出的目标 PID namespace，取得引用前只借用；
	 *   active  current 当前所在 PID namespace，借用且用于祖先关系约束；
	 *   ns      嵌入 target 的通用 namespace 头，借用；
	 *   ns_filp 临时持有 namespace 文件，离开作用域自动 fput。
	 */
	struct proc_fs_context *ctx = fc->fs_private;
	struct pid_namespace *target, *active = task_active_pid_ns(current);
	struct ns_common *ns;
	struct file *ns_filp __free(fput) = NULL;

	switch (param->type) {
	case fs_value_is_file:
		/* came through fsconfig, steal the file reference */
		/*
		 * 该文件来自 fsconfig；no_free_ptr() 把 param->file 的释放责任
		 * 转给带 __free(fput) 的 ns_filp，作用域退出时统一归还引用。
		 */
		ns_filp = no_free_ptr(param->file);
		break;
	case fs_value_is_string:
		/*
		 * 传统字符串参数按只读方式打开，成功引用同样由自动 cleanup
		 * 持有。
		 */
		ns_filp = filp_open(param->string, O_RDONLY, 0);
		break;
	default:
		WARN_ON_ONCE(true);
		break;
	}
	if (!ns_filp)
		ns_filp = ERR_PTR(-EBADF);
	if (IS_ERR(ns_filp)) {
		errorfc(fc, "could not get file from pidns argument");
		return PTR_ERR(ns_filp);
	}

	/*
	 * 分两层验证文件身份：先确认它由 nsfs 提供，再确认通用 ns_type
	 * 真的是 PID namespace，之后 container_of() 才能安全恢复 target。
	 */
	if (!proc_ns_file(ns_filp))
		return invalfc(fc, "pidns argument is not an nsfs file");
	ns = get_proc_ns(file_inode(ns_filp));
	if (ns->ns_type != CLONE_NEWPID)
		return invalfc(fc, "pidns argument is not a pidns file");
	target = container_of(ns, struct pid_namespace, ns);

	/*
	 * pidns= is shorthand for joining the pidns to get a fsopen fd, so the
	 * permission model should be the same as pidns_install().
	 */
	/*
	 * pidns= 可理解为“先加入该 PID namespace 再取得 fsopen fd”的简写，
	 * 因而权限模型必须与 pidns_install() 相同：调用者需在目标拥有者
	 * user namespace 中具有 CAP_SYS_ADMIN，不能借挂载绕过 setns 约束。
	 */
	if (!ns_capable(target->user_ns, CAP_SYS_ADMIN)) {
		errorfc(fc, "insufficient permissions to set pidns");
		return -EPERM;
	}
	if (!pidns_is_ancestor(target, active))
		return invalfc(fc, "cannot set pidns to non-descendant pidns");

	/*
	 * 所有校验通过后才提交引用替换，保持失败原子性。
	 * fc->user_ns 必须与 pid_ns->user_ns 同步，否则后续 gid 映射和
	 * userns 挂载权限会基于错误身份域。get/put 对保证两类 namespace
	 * 都不会提前销毁。
	 */
	put_pid_ns(ctx->pid_ns);
	ctx->pid_ns = get_pid_ns(target);
	put_user_ns(fc->user_ns);
	fc->user_ns = get_user_ns(ctx->pid_ns->user_ns);
	return 0;
}
#endif /* CONFIG_PID_NS */

/*
 * proc_parse_param() - procfs 所有挂载参数的统一分派入口。
 *
 * VFS 的 fsconfig/legacy mount 路径逐个调用本函数。fc 是当前事务的
 * 输入输出上下文，param 是借用的单个参数；允许睡眠，无 procfs 锁要求。
 * fs_parse() 完成名称、类型和值的基础解析，专用 helper 再执行业务校验。
 *
 * 成功返回 0，并在对应值写好后设置 ctx->mask 位；失败返回 fs_parse()
 * 或 helper 的负 errno，mask 不记录该参数，因此不会把失败值应用到
 * superblock。pidns= 在关闭 CONFIG_PID_NS 时明确返回 -EOPNOTSUPP。
 */
static int proc_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	/*
	 * result 保存 fs_parse() 的类型化结果；opt 是 Opt_* 分派号；
	 * err 传递需要二次解析的选项错误，三者只在本次调用期间有效。
	 */
	struct proc_fs_context *ctx = fc->fs_private;
	struct fs_parse_result result;
	int opt, err;

	opt = fs_parse(fc, proc_fs_parameters, param, &result);
	if (opt < 0)
		return opt;

	/* 只有每个分支完整成功，控制流才到末尾设置“显式指定”位。 */
	switch (opt) {
	case Opt_gid:
		ctx->gid = result.uint_32;
		break;

	case Opt_hidepid:
		err = proc_parse_hidepid_param(fc, param);
		if (err)
			return err;
		break;

	case Opt_subset:
		err = proc_parse_subset_param(fc, param->string);
		if (err)
			return err;
		break;

	case Opt_pidns:
#ifdef CONFIG_PID_NS
		/*
		 * We would have to RCU-protect every proc_pid_ns() or
		 * proc_sb_info() access if we allowed this to be reconfigured
		 * for an existing procfs instance. Luckily, procfs instances
		 * are cheap to create, and mount-beneath would let you
		 * atomically replace an instance even with overmounts.
		 */
		/*
		 * 若允许现有实例原地更换 pid_ns，每个未加 RCU 读锁而取得
		 * proc_sb_info()/proc_pid_ns() 裸指针的路径都可能和旧引用释放
		 * 竞争。procfs 实例创建便宜，mount-beneath 又能在保留上层挂载
		 * 的同时原子替换底层实例，所以这里选择拒绝重配置，而不把
		 * 所有热读路径改造成 RCU 协议。
		 */
		if (fc->purpose == FS_CONTEXT_FOR_RECONFIGURE) {
			errorfc(fc, "cannot reconfigure pidns for existing procfs");
			return -EBUSY;
		}
		err = proc_parse_pidns_param(fc, param, &result);
		if (err)
			return err;
		break;
#else
		/*
		 * 构建时无 PID namespace 支持，参数名虽可识别但能力
		 * 不可实现。
		 */
		errorfc(fc, "pidns mount flag not supported on this system");
		return -EOPNOTSUPP;
#endif

	default:
		return -EINVAL;
	}

	ctx->mask |= 1 << opt;
	return 0;
}

/*
 * proc_apply_options() - 把已成功解析且被 mask 标记的选项提交给实例。
 *
 * fs_info 是待建立或已存在 superblock 的输入输出私有信息；fc/ctx 是借用
 * 的配置事务；user_ns 决定用户 gid 到 kgid_t 的映射域。调用者是
 * proc_fill_super() 和 proc_reconfigure()，入口无需 procfs 私有锁。
 *
 * 新挂载时对象尚不可见；重配置时 VFS 串行化该事务，调用者还先执行
 * sync_filesystem()。函数只覆盖用户明确指定的字段。成功返回 0；
 * subset=pid 试图在线改变时返回负 errno。pid_ns 引用替换仅允许建树阶段，
 * 旧引用 put、新引用 get，故 fs_info 始终持有恰好一个有效引用。
 */
static int proc_apply_options(struct proc_fs_info *fs_info,
			       struct fs_context *fc,
			       struct user_namespace *user_ns)
{
	struct proc_fs_context *ctx = fc->fs_private;

	/*
	 * pidonly 会改变静态条目是否可见并给 superblock 打 restricted 标志；
	 * 现有 dcache/inode 已按旧视图建立，故不能像权限策略一样在线切换。
	 */
	if ((ctx->mask & (1 << Opt_subset)) &&
	    fc->purpose == FS_CONTEXT_FOR_RECONFIGURE &&
	    ctx->pidonly != fs_info->pidonly)
		return invalf(fc, "proc: subset=pid cannot be changed\n");

	/* mask 保证未指定的零值不会意外覆盖现有挂载策略。 */
	if (ctx->mask & (1 << Opt_gid))
		fs_info->pid_gid = make_kgid(user_ns, ctx->gid);
	if (ctx->mask & (1 << Opt_hidepid))
		fs_info->hide_pid = ctx->hidepid;
	if (ctx->mask & (1 << Opt_subset))
		fs_info->pidonly = ctx->pidonly;
	if (ctx->mask & (1 << Opt_pidns) &&
	    !WARN_ON_ONCE(fc->purpose == FS_CONTEXT_FOR_RECONFIGURE)) {
		/*
		 * 先归还旧 namespace，再为 superblock 持有配置事务中的
		 * 目标引用。
		 */
		put_pid_ns(fs_info->pid_ns);
		fs_info->pid_ns = get_pid_ns(ctx->pid_ns);
	}
	return 0;
}

/*
 * proc_fill_super() - 构造一个完整、可由 VFS 发布的 procfs superblock。
 *
 * get_tree_nodev() 调用本函数。s 是尚未完成初始化的匿名 superblock，
 * fc 借用本次配置；函数在可睡眠的进程上下文执行，入口不持 procfs 锁。
 *
 * 阶段依次为：分配并固定挂载私有引用；应用选项；建立 superblock
 * 不变量；为全局 proc_root 取得 PDE 引用并创建本实例的 root
 * inode/dentry；最后创建依赖当前任务的 self/thread-self 魔术链接。
 *
 * 成功返回 0，s->s_fs_info/s_root 均有效，后续由 VFS 发布挂载。
 * 失败返回 -ENOMEM 或 setup helper 的负 errno；一旦 s_fs_info 已安装，
 * VFS 的失败清理会进入 proc_kill_sb()，释放 pid_ns、cred 和 fs_info。
 * proc_get_inode() 在 inode 分配失败时会替调用者归还刚取得的 PDE 引用。
 */
static int proc_fill_super(struct super_block *s, struct fs_context *fc)
{
	/*
	 * ctx 是借用的待提交配置；root_inode 的所有权成功后交给 root dentry；
	 * fs_info 分配后属于 superblock；ret 只传递应用/setup 阶段的 errno。
	 */
	struct proc_fs_context *ctx = fc->fs_private;
	struct inode *root_inode;
	struct proc_fs_info *fs_info;
	int ret;

	fs_info = kzalloc_obj(*fs_info);
	if (!fs_info)
		return -ENOMEM;

	/*
	 * superblock 独立持有 PID namespace 与挂载者凭据，不能借用 fc，
	 * 因为 fs_context 在挂载完成后会先于 superblock 销毁。
	 */
	fs_info->pid_ns = get_pid_ns(ctx->pid_ns);
	fs_info->mounter_cred = get_cred(fc->cred);
	ret = proc_apply_options(fs_info, fc, current_user_ns());
	if (ret)
		return ret;

	/* User space would break if executables or devices appear on proc */
	/*
	 * 用户空间长期依赖 procfs 不承载可执行程序或设备节点。内部 iflags
	 * 约束叠加行为，公开 s_flags 则让 VFS 强制 noexec/nosuid，并关闭
	 * 目录 atime 更新，兼顾 ABI 安全假设与伪文件系统开销。
	 */
	s->s_iflags |= SB_I_NOEXEC | SB_I_NODEV;
	s->s_flags |= SB_NODIRATIME | SB_NOSUID | SB_NOEXEC;
	s->s_blocksize = 1024;
	s->s_blocksize_bits = 10;
	s->s_magic = PROC_SUPER_MAGIC;
	s->s_op = &proc_sops;
	s->s_time_gran = 1;
	s->s_fs_info = fs_info;

	/* pid-only 变体被标为受限视图，使 VFS/叠加场景能识别其语义差异。 */
	if (fs_info->pidonly == PROC_PIDONLY_ON)
		s->s_iflags |= SB_I_RESTRICTED_VARIANT;

	/*
	 * procfs isn't actually a stacking filesystem; however, there is
	 * too much magic going on inside it to permit stacking things on
	 * top of it
	 */
	/*
	 * procfs 本身不是 stacking filesystem；但其 inode/dentry 大量由任务和
	 * 回调动态合成，不满足普通下层文件系统的稳定语义。把深度设为
	 * 上限，等价于拒绝继续在它之上堆叠文件系统，避免错误组合。
	 */
	s->s_stack_depth = FILESYSTEM_MAX_STACK_DEPTH;

	/* procfs dentries and inodes don't require IO to create */
	/*
	 * procfs dentry/inode 创建不发起块 I/O，把 shrinker seek 成本设为 0，
	 * 使内存回收器把这些可重建缓存视为便宜的回收候选。
	 */
	s->s_shrink->seeks = 0;

	/*
	 * 全局 proc_root 的 refcnt 初值只维持静态树本身；每个 superblock 再
	 * 取得一份 PDE 引用，随后由 proc inode 持有，evict_inode 时 pde_put。
	 */
	pde_get(&proc_root);
	root_inode = proc_get_inode(s, &proc_root);
	if (!root_inode) {
		pr_err("proc_fill_super: get root inode failed\n");
		return -ENOMEM;
	}

	s->s_root = d_make_root(root_inode);
	if (!s->s_root) {
		pr_err("proc_fill_super: allocate dentry failed\n");
		return -ENOMEM;
	}

	/*
	 * root dentry 可用后依次创建两个每挂载实例的魔术链接。二者目标
	 * 按访问者和此 superblock 的 pid_ns 动态计算，不能作为全局普通
	 * PDE 复用。任何一步失败都使整次挂载失败，不发布残缺根目录。
	 */
	ret = proc_setup_self(s);
	if (ret) {
		return ret;
	}
	return proc_setup_thread_self(s);
}

/*
 * proc_reconfigure() - 将允许在线改变的挂载选项应用到现有 procfs。
 *
 * VFS 在 remount/fsconfig 重配置流程中调用。fc->root 借用目标挂载根，
 * fc->fs_private 保存本次新解析的显式选项；sb/fs_info 都由现有挂载持有。
 * 函数在可睡眠的进程上下文运行，VFS 已串行化同一 superblock 的重配置。
 *
 * 先 sync_filesystem() 让此前文件系统活动到达一致边界，再提交 gid/hidepid
 * 等允许项。成功返回 0；非法的 subset/pidns 变化返回负 errno，现有挂载
 * 仍存活。procfs 没有普通脏数据写回，但遵循通用重配置契约可避免未来
 * superblock 操作与策略切换跨越未完成的文件系统同步。
 */
static int proc_reconfigure(struct fs_context *fc)
{
	/* sb 与 fs_info 都是借用指针，生命周期由 fc->root 所在挂载保证。 */
	struct super_block *sb = fc->root->d_sb;
	struct proc_fs_info *fs_info = proc_sb_info(sb);

	sync_filesystem(sb);

	return proc_apply_options(fs_info, fc, current_user_ns());
}

/*
 * proc_get_tree() - 为当前 fs_context 请求一个无块设备的 procfs 树。
 *
 * fc 是 VFS 持有的输入输出上下文，无所有权转移；get_tree_nodev() 创建
 * 匿名 superblock 并回调 proc_fill_super() 完成实例化。函数可睡眠。
 * 返回 0 表示 fc->root 已获得树根，负 errno 表示建树失败且由 VFS 清理
 * 中间对象。该薄包装固定了 procfs“不依赖块设备”的实例化策略。
 */
static int proc_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, proc_fill_super);
}

/*
 * proc_fs_context_free() - 释放一次 procfs 配置事务的私有状态。
 *
 * VFS 在 fs_context 成功消费或失败丢弃时调用。fc 仍有效，ctx 由本函数
 * 独占释放；无需锁，kfree 不睡眠。函数无直接返回值，副作用是归还
 * proc_init_fs_context()/pidns= 路径持有的 PID namespace 引用并释放 ctx。
 * fc->user_ns 属于通用 fs_context，由 VFS 自己释放，不能在这里重复 put。
 */
static void proc_fs_context_free(struct fs_context *fc)
{
	struct proc_fs_context *ctx = fc->fs_private;

	put_pid_ns(ctx->pid_ns);
	kfree(ctx);
}

/*
 * procfs 的 fs_context 操作表把通用 VFS 状态机连接到本文件：
 *   free          终结尚未发布的配置对象；
 *   parse_param   逐个吸收 mount/fsconfig 参数；
 *   get_tree      创建新 superblock；
 *   reconfigure   更新现有 superblock 的允许策略。
 * 表为全局只读对象，函数指针在文件系统类型注册后由 VFS 间接调用。
 */
static const struct fs_context_operations proc_fs_context_ops = {
	.free		= proc_fs_context_free,
	.parse_param	= proc_parse_param,
	.get_tree	= proc_get_tree,
	.reconfigure	= proc_reconfigure,
};

/*
 * proc_init_fs_context() - 创建 procfs 挂载事务并选择默认命名空间。
 *
 * VFS 通过 proc_fs_type.init_fs_context 调用。fc 是刚建立的输入输出对象；
 * current 的 active PID namespace 作为默认可见域。函数可睡眠，不要求锁。
 *
 * 成功返回 0：ctx 持有 pid_ns 引用，fc->user_ns 被同步替换为该 PID
 * namespace 的拥有者 user namespace，fc->fs_private/ops 完成安装。
 * 分配失败返回 -ENOMEM，fc 不接管 ctx，也没有新增引用需要回滚。
 * 后续参数解析可以通过 pidns= 原子替换这对 namespace 引用。
 */
static int proc_init_fs_context(struct fs_context *fc)
{
	struct proc_fs_context *ctx;

	/*
	 * kzalloc 使 mask 和所有策略默认为 OFF；只有显式参数才置相应
	 * mask 位。
	 */
	ctx = kzalloc_obj(struct proc_fs_context);
	if (!ctx)
		return -ENOMEM;

	/*
	 * ctx 和 fc 各自持有不同类型的 namespace 引用。这里先取得新引用再
	 * 释放 fc 的旧 user_ns，保证后续 gid 映射和挂载权限以同一层级为准。
	 */
	ctx->pid_ns = get_pid_ns(task_active_pid_ns(current));
	put_user_ns(fc->user_ns);
	fc->user_ns = get_user_ns(ctx->pid_ns->user_ns);
	fc->fs_private = ctx;
	fc->ops = &proc_fs_context_ops;
	return 0;
}

/*
 * proc_kill_sb() - 拆除 procfs superblock 并回收挂载私有引用。
 *
 * VFS 卸载/建树失败清理经 proc_fs_type.kill_sb 调用。sb 由 VFS 持有，
 * fs_info 是从 sb 借用的实例私有对象。函数可进入通用 superblock 销毁
 * 流程；调用者不得在返回后继续访问 sb。
 *
 * kill_anon_super() 先摘除并释放匿名 superblock 的 VFS inode/dentry，
 * 包括各 inode 持有的 PDE 引用；随后归还 fs_info 的 pid_ns 与挂载者
 * cred 引用。kfree_rcu() 将 fs_info 的物理释放推迟到一个 RCU 宽限期
 * 之后；它只延长存储期，普通读者仍须由 VFS 的 superblock 生命周期
 * 保证访问有效。函数无直接返回值。
 */
static void proc_kill_sb(struct super_block *sb)
{
	/*
	 * kill_anon_super() 会拆除其他 VFS 状态，先保存仍需清理的
	 * fs_info 借用指针。
	 */
	struct proc_fs_info *fs_info = proc_sb_info(sb);

	kill_anon_super(sb);
	if (fs_info) {
		put_pid_ns(fs_info->pid_ns);
		put_cred(fs_info->mounter_cred);
		kfree_rcu(fs_info, rcu);
	}
}

/*
 * 向 VFS 描述名为 "proc" 的文件系统类型。init/kill 回调闭合实例生命周期；
 * parameters 供通用参数基础设施查询。FS_USERNS_MOUNT 允许 user namespace
 * 内挂载，RESTRICTED 要求按受限挂载规则处理，FS_DISALLOW_NOTIFY_PERM
 * 禁止把 fsnotify permission 事件建立在 procfs 的动态伪 inode 语义之上。
 * 对象在 proc_root_init() 注册后全局可发现，此后不再修改。
 */
static struct file_system_type proc_fs_type = {
	.name			= "proc",
	.init_fs_context	= proc_init_fs_context,
	.parameters		= proc_fs_parameters,
	.kill_sb		= proc_kill_sb,
	.fs_flags		= FS_USERNS_MOUNT | FS_USERNS_MOUNT_RESTRICTED | FS_DISALLOW_NOTIFY_PERM,
};

/*
 * proc_root_init() - 在启动期建立 procfs 全局基础设施并向 VFS 注册。
 *
 * 调用关系：start_kernel() 后段的 proc_root_init() 串行执行本函数；完成后
 * mount -t proc 才能通过 proc_fs_type 找到本文件的挂载回调。
 * 入参：无。返回：无直接返回值；register_filesystem() 的返回值未消费，
 * 因为启动期只有本处注册静态唯一的 "proc" 类型；若重名，属于内核
 * 构建错误。
 *
 * __init 把函数代码放入启动后可回收的 init section。入口仍是单线程式
 * 内核初始化环境，可以睡眠，尚无用户态并发挂载 procfs。函数按依赖顺序
 * 建立 slab、PID 目录元数据、特殊链接、静态子树，最后才发布 fs_type；
 * 因而注册动作是外部可见的提交边界，此前无须为部分初始化提供并发
 * 回滚。
 */
void __init proc_root_init(void)
{
	/*
	 * 创建 procfs 专用的三个 slab 缓存（fs/proc/inode.c）：
	 *   proc_inode_cachep    — struct proc_inode（VFS inode + proc 私有字段合体），
	 *                          每个 /proc 文件背后都有一个，SLAB_RECLAIM_ACCOUNT
	 *                          使其可被 shrinker 在内存压力下回收；
	 *   pde_opener_cache     — struct pde_opener，跟踪 /proc 文件的打开实例，
	 *                          用于在 proc_dir_entry 被移除时等待所有 reader 退出；
	 *   proc_dir_entry_cache — struct proc_dir_entry（PDE），描述一个 /proc 节点
	 *                          的元数据（名称、权限、ops 指针等），
	 *                          kmem_cache_create_usercopy 标记 inline_name 字段
	 *                          可安全复制到用户空间（HARDENED_USERCOPY 合规）。
	 */
	proc_init_kmemcache();

	/*
	 * 预计算 /proc/<pid>/ 和 /proc/<pid>/task/<tid>/ 目录的 nlink 数量。
	 * nlink 等于子目录数 + 2（"." 和 ".."），而子目录数由
	 * tgid_base_stuff / tid_base_stuff 数组中类型为目录的条目数决定。
	 * 预计算结果存入 nlink_tgid / nlink_tid，避免每次 stat() 重新遍历数组。
	 */
	set_proc_pid_nlink();

	/*
	 * 为 /proc/self 预分配 inode 编号（self_inum）。
	 * /proc/self 是指向当前进程 /proc/<pid> 的符号链接；
	 * inode 编号固定后，readlink("/proc/self") 才能稳定返回目标路径。
	 */
	/*
	 * 修正说明：固定 inode 编号保证各 procfs 实例中的 /proc/self 具有稳定
	 * inode 身份；readlink 的目标字符串实际由 fs/proc/self.c 根据 current
	 * 和该 superblock 的 pid_ns 动态生成，并不由 inode 编号决定。
	 */
	proc_self_init();

	/*
	 * 为 /proc/thread-self 预分配 inode 编号（thread_self_inum）。
	 * /proc/thread-self 指向当前线程的 /proc/<pid>/task/<tid>，
	 * 多线程程序通过它读取本线程的 stat/status 而无需知道自己的 tid。
	 */
	proc_thread_self_init();

	/*
	 * 创建 /proc/mounts → self/mounts 的符号链接。
	 * 用户态工具（mount(8)、df(1) 等）通过 /proc/mounts 读取
	 * 当前挂载表，实际内容由 /proc/<pid>/mounts 的 seq_file 生成。
	 */
	proc_symlink("mounts", NULL, "self/mounts");

	/*
	 * 初始化 /proc/net 目录框架。
	 * /proc/net 是每网络命名空间独立的目录，内容随 net namespace 切换。
	 * 这里只建立框架和 pernet_operations 注册机制；具体条目（如
	 * /proc/net/dev、/proc/net/tcp）由各网络子系统后续注册。
	 */
	proc_net_init();

	/* 建立 /proc/fs 目录：各文件系统注册自己的调试/统计节点的挂载点 */
	proc_mkdir("fs", NULL);
	/* 建立 /proc/driver 目录：驱动程序注册自己的状态节点 */
	proc_mkdir("driver", NULL);
	/* 为 nfsd 文件系统预留挂载点 /proc/fs/nfsd，nfsd 模块加载时在此挂载 */
	proc_create_mount_point("fs/nfsd");
#if defined(CONFIG_SUN_OPENPROMFS) || defined(CONFIG_SUN_OPENPROMFS_MODULE)
	/* SPARC OpenPROM 文件系统的挂载点，ARM64 上此分支不编译 */
	proc_create_mount_point("openprom");
#endif

	/*
	 * 初始化 /proc/tty 目录，注册 TTY 驱动信息节点：
	 *   /proc/tty/drivers  — 已注册的 TTY 驱动列表；
	 *   /proc/tty/ldiscs   — 已注册的线路规程（line discipline）列表。
	 * ARM64 的串口控制台（如 ttyAMA0）依赖这里的注册信息。
	 */
	proc_tty_init();

	/* 建立 /proc/bus 目录：USB、PCI 等总线子系统的设备枚举节点 */
	proc_mkdir("bus", NULL);

	/*
	 * 初始化 /proc/sys 目录并调用 sysctl_init_bases()。
	 * sysctl 是内核参数的运行时读写接口：
	 *   /proc/sys/kernel/  — 进程、调度、崩溃等核心参数；
	 *   /proc/sys/vm/      — 内存管理参数（swappiness、dirty_ratio 等）；
	 *   /proc/sys/net/     — 网络栈参数（tcp_congestion_control 等）。
	 * 此后 sysctl_register_table() 可用，驱动可注册自己的 sysctl 节点。
	 */
	proc_sys_init();

	/*
	 * 最后一步：将 proc_fs_type 注册到 VFS 文件系统表。
	 * 只有注册后，"mount -t proc proc /proc" 才能找到这个文件系统类型。
	 * 故意放在最后：确保所有内部数据结构（缓存、目录、inode 编号）
	 * 完全就绪后，外部才能访问 /proc，避免竞争窗口。
	 */
	register_filesystem(&proc_fs_type);
}

/*
 * proc_root_getattr() - 生成 /proc 根 inode 的 stat 属性并修正动态 nlink。
 *
 * VFS getattr 路径调用。idmap/query_flags 在此实现中不参与计算；path 是
 * 借用的 /proc 根路径；stat 是调用者提供的输出缓冲区；request_mask
 * 指示所需属性。入口由 VFS 保证 path/inode 存活，函数不睡眠且不持有
 * procfs 私有锁。
 *
 * generic_fillattr() 先填普通 inode 属性，再把 nlink 设置为静态根 PDE
 * 链接数加当前进程数。nr_processes() 汇总无全局锁的 per-CPU 计数，
 * 因而该值只是瞬时近似值，不用于枚举或引用进程。成功恒返回 0，
 * 无所有权变化。
 */
static int proc_root_getattr(struct mnt_idmap *idmap,
			     const struct path *path, struct kstat *stat,
			     u32 request_mask, unsigned int query_flags)
{
	/* procfs 不采用挂载 idmap 转换根 inode，显式使用 nop_mnt_idmap。 */
	generic_fillattr(&nop_mnt_idmap, request_mask, d_inode(path->dentry),
			 stat);
	stat->nlink = proc_root.nlink + nr_processes();
	return 0;
}

/*
 * proc_root_lookup() - 在 /proc 根目录中按名称查找动态 PID 或静态 PDE。
 *
 * VFS 路径解析调用。dir 是借用的根 inode，dentry 是待实例化的负/候选
 * dentry，flags 是 lookup 标志；入口由 VFS/dcache 锁协议保证对象存活，
 * helper 内部负责 PID 查找所需的 RCU 和 task 引用，函数可能分配 inode。
 *
 * 先把名称按数字 PID 尝试：proc_pid_lookup() 返回 NULL 表示成功实例化且
 * dentry 已成为正项，立即返回 NULL；返回错误指针（通常 -ENOENT）表示
 * “不是可见 PID”，再交给 proc_lookup() 搜索静态 PDE 红黑树。最终返回
 * NULL、dentry/alias 或错误指针，遵循 VFS lookup 回调契约。
 */
static struct dentry *proc_root_lookup(struct inode * dir, struct dentry * dentry, unsigned int flags)
{
	/* 动态 PID 优先，使数字名称与当前 pid_ns 的任务视图保持一致。 */
	if (!proc_pid_lookup(dentry, flags))
		return NULL;

	return proc_lookup(dir, dentry, flags);
}

/*
 * proc_root_readdir() - 在同一目录流中顺序输出静态条目和动态 PID 条目。
 *
 * VFS iterate_shared 回调调用。file 借用打开的 /proc 根目录，ctx 是输入
 * 输出枚举游标和 emit 回调；目录级共享迭代锁由 VFS 管理，具体 PID 遍历
 * 在 helper 内以 RCU + task 引用抵抗并发 fork/exit，并可能 cond_resched。
 *
 * ctx->pos 的 [0, FIRST_PROCESS_ENTRY) 区间保留给 proc_readdir() 的静态
 * PDE 树；完成后强制跳到 FIRST_PROCESS_ENTRY，再由 proc_pid_readdir()
 * 在独立偏移空间输出 self、thread-self 和可见 TGID。这样 seekdir/telldir
 * 能恢复阶段而不会把静态条目位置误当 PID。返回 0 表示结束或缓冲区
 * 已满，负值表示静态阶段错误；ctx->pos 是主要可观察副作用。
 */
static int proc_root_readdir(struct file *file, struct dir_context *ctx)
{
	if (ctx->pos < FIRST_PROCESS_ENTRY) {
		/*
		 * error 同时承载负 errno、0（停止）和正值（静态阶段
		 * 可继续衔接）。
		 */
		int error = proc_readdir(file, ctx);
		if (unlikely(error <= 0))
			return error;
		/*
		 * 静态空间一次完成后进入动态空间，后续调用不再重复扫描
		 * PDE。
		 */
		ctx->pos = FIRST_PROCESS_ENTRY;
	}

	return proc_pid_readdir(file, ctx);
}

/*
 * The root /proc directory is special, as it has the
 * <pid> directories. Thus we don't use the generic
 * directory handling functions for that..
 */
/*
 * /proc 根目录之所以特殊，是因为它含有按 <pid> 动态生成的目录，不能只用
 * 通用 proc 目录函数。该操作表保留普通目录 read/llseek，但把共享迭代
 * 定向到上面的“两阶段合并”实现；表在所有 procfs superblock 间只读共享。
 */
static const struct file_operations proc_root_operations = {
	.read		 = generic_read_dir,
	.iterate_shared	 = proc_root_readdir,
	.llseek		= generic_file_llseek,
};

/*
 * proc root can do almost nothing..
 */
/*
 * proc 根 inode 几乎不提供普通文件操作：只需专用 lookup 合并动态 PID 与
 * 静态 PDE，并用 getattr 修正动态 nlink；权限等其余行为沿用 VFS 通用逻辑。
 */
static const struct inode_operations proc_root_inode_operations = {
	.lookup		= proc_root_lookup,
	.getattr	= proc_root_getattr,
};

/*
 * This is the root "inode" in the /proc tree..
 */
/*
 * 这是 proc 全局 PDE 树的根“inode 描述”，并非某个 superblock 中真正的
 * struct inode。proc_fill_super() 为每个实例取得它的引用，再由
 * proc_get_inode() 投影成该实例的 VFS inode。
 *
 * 字段不变量：
 *   low_ino/name/namelen/mode 固定根节点身份、名称和只读可搜索目录权限；
 *   nlink 初值 2 表示 "."/".." 基数，getattr 时另加动态进程统计；
 *   refcnt 初始 1 维持全局静态对象，实例 inode 另行 pde_get/pde_put；
 *   proc_iops/proc_dir_ops 连接到本文件的专用根操作；
 *   parent 自指表示树根，subdir 是静态子节点红黑树的根；
 *   对象静态分配并在启动期逐步挂接子树，注册后供并发读者使用。
 */
struct proc_dir_entry proc_root = {
	.low_ino	= PROCFS_ROOT_INO,
	.namelen	= 5,
	.mode		= S_IFDIR | S_IRUGO | S_IXUGO,
	.nlink		= 2,
	.refcnt		= REFCOUNT_INIT(1),
	.proc_iops	= &proc_root_inode_operations,
	.proc_dir_ops	= &proc_root_operations,
	.parent		= &proc_root,
	.subdir		= RB_ROOT,
	.name		= "/proc",
};
