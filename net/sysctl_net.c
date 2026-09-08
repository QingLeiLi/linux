// SPDX-License-Identifier: GPL-2.0-only
/* -*- linux-c -*-
 * sysctl_net.c: sysctl interface to net subsystem.
 *
 * Begun April 1, 1996, Mike Shaver.
 * Added /proc/sys/net directories for each protocol family. [MS]
 *
 * Revision 1.2  1996/05/08  20:24:40  shaver
 * Added bits for NET_BRIDGE and the NET_IPV4_ARP stuff and
 * NET_IPV4_IP_FORWARD.
 *
 *
 */
/*
 * 本文件从 1996 年开始为网络子系统建立 sysctl 接口，后来加入按协议族组织的 /proc/sys/net
 * 目录，以及 bridge、IPv4 ARP 和转发等控制项。当前实现的核心职责已扩展为：让同一路径在
 * 不同网络命名空间解析到不同 ctl_table_set，并阻止容器可写项意外指向内核全局数据。
 */

#include <linux/mm.h>
#include <linux/export.h>
#include <linux/sysctl.h>
#include <linux/nsproxy.h>

/* sock.h 提供网络命名空间辅助定义；后两组头只在相应配置下补充协议常量，避免无关配置依赖。 */
#include <net/sock.h>

#ifdef CONFIG_INET
#include <net/ip.h>
#endif

#ifdef CONFIG_NET
#include <linux/if_ether.h>
#endif

/*
 * net_ctl_header_lookup() - 为一次 /proc/sys/net 查找选择当前任务所属 netns 的 sysctl 集合
 *
 * 业务背景：proc sysctl 核心从共享 net_sysctl_root 进入时调用此回调，把相同路径动态路由到
 * current 的网络命名空间；这是路径名共享而数值隔离的分派点。
 * 入参：@root 是常驻的网络 sysctl 根，只用于满足通用回调签名，本函数不读取也不持有它。
 * 出参/返回：返回 current->nsproxy->net_ns 内嵌 sysctls 的借用指针，不增加 netns 引用。
 * 注意事项：调用路径必须保证 current 的 nsproxy/net_ns 在查找期间稳定；函数不加锁、不睡眠，
 * 返回指针不得越过当前 sysctl 查找所提供的命名空间生命周期保护保存。
 */
static struct ctl_table_set *
net_ctl_header_lookup(struct ctl_table_root *root)
{
	/* nsproxy 是任务的命名空间集合；取其中 net_ns 后即可定位这个容器自己的目录树。 */
	return &current->nsproxy->net_ns->sysctls;
}

/*
 * is_seen() - 判断给定 sysctl 集合是否属于当前任务的网络命名空间
 *
 * 业务背景：sysctl 目录遍历需过滤其他 netns 的同名节点；setup_sysctl_set() 把本回调保存到 set。
 * 入参：@set 是待判定 ctl_table_set 的借用指针，允许指向任一仍存活 netns 的内嵌集合，不可为空。
 * 出参/返回：属于 current netns 返回 1，否则返回 0；不改变引用、目录或任务状态。
 * 注意事项：比较对象地址而不是 namespace ID，依赖 netns 生命周期稳定对象身份；不加锁且不睡眠。
 */
static int is_seen(struct ctl_table_set *set)
{
	return &current->nsproxy->net_ns->sysctls == set;
}

/* Return standard mode bits for table entry. */
/* 返回表项的标准权限位；下方额外把所属 userns 的 CAP_NET_ADMIN 映射成传统 root 等价权限。 */
/*
 * net_ctl_permissions() - 按访问者在目标 user namespace 中的能力计算网络 sysctl 权限
 *
 * 业务背景：容器 root 的 kuid 未必是初始命名空间 uid 0；通用 sysctl 权限检查通过 root 回调
 * 询问网络层，使目标 netns 的管理员可获得与文件 owner 相同的读写权限。
 * 入参：@head 是已选中表的借用 header，其 set 内嵌于 struct net；@table 是当前表项的只读借用
 * 指针，mode 保存 owner/group/other 三组三位 Unix 权限。两者均不可为空，ownership 不转移。
 * 出参/返回：有 CAP_NET_ADMIN 时把 owner 三位复制到三组并返回；否则原样返回 table->mode。
 * 注意事项：container_of 只做嵌入成员到宿主 net 的地址换算；ns_capable_noaudit 不产生审计记录，
 * 本函数不修改表、不获取长期引用且不可依赖返回值延长 net/head 生命周期。
 */
static int net_ctl_permissions(struct ctl_table_header *head,
			       const struct ctl_table *table)
{
	/* head->set 的对象身份由本文件 setup 时建立，因此可安全反推出其唯一宿主 netns。 */
	struct net *net = container_of(head->set, struct net, sysctls);

	/* Allow network administrator to have same access as root. */
	/* 若调用者在该 netns 所属 user_ns 中具备网络管理能力，就把 owner 权限授予三类身份。 */
	if (ns_capable_noaudit(net->user_ns, CAP_NET_ADMIN)) {
		/* 右移六位取得 owner 的 rwx 三位；复制不会凭空增加 owner 原本没有的权限。 */
		int mode = (table->mode >> 6) & 7;
		return (mode << 6) | (mode << 3) | mode;
	}

	/* 普通访问者继续由 VFS 按表中原始 owner/group/other 位判定。 */
	return table->mode;
}

/*
 * net_ctl_set_ownership() - 把 proc sysctl inode 的属主映射为目标 userns 的 root
 *
 * 业务背景：显示在容器中的 /proc/sys/net 文件应由容器所见 uid/gid 0 拥有；sysctl 核心创建或
 * 查询 inode 属性时调用该 root 回调。
 * 入参：@head 借用目标表 header；@uid/@gid 是输入输出槽，进入时含通用默认属主，成功映射时
 * 分别被覆盖。所有指针不可为空，本函数不接管任何对象。
 * 出参/返回：无直接返回值；有效映射会更新一个或两个输出槽，映射无效的槽保持原值。
 * 注意事项：userns 映射可能不包含 namespace 内的 0，故必须分别用 uid_valid/gid_valid 检查；
 * 函数只读稳定的 net->user_ns，不分配资源、不睡眠。
 */
static void net_ctl_set_ownership(struct ctl_table_header *head,
				  kuid_t *uid, kgid_t *gid)
{
	/* net 标识目标目录集合；两个局部量保存 userns 内 0 映射到内核 kuid/kgid 后的候选值。 */
	struct net *net = container_of(head->set, struct net, sysctls);
	kuid_t ns_root_uid;
	kgid_t ns_root_gid;

	/* uid 与 gid 的映射可独立缺失，因此分两阶段提交，不能假定一个有效就代表另一个有效。 */
	ns_root_uid = make_kuid(net->user_ns, 0);
	if (uid_valid(ns_root_uid))
		*uid = ns_root_uid;

	ns_root_gid = make_kgid(net->user_ns, 0);
	if (gid_valid(ns_root_gid))
		*gid = ns_root_gid;
}

/*
 * 网络 sysctl 的常驻操作表：lookup 选择 current netns，permissions 计算能力覆盖后的 mode，
 * set_ownership 负责 inode uid/gid 映射。default_set 未承载各 netns 数据，实际集合由 lookup 返回。
 */
static struct ctl_table_root net_sysctl_root = {
	.lookup = net_ctl_header_lookup,
	.permissions = net_ctl_permissions,
	.set_ownership = net_ctl_set_ownership,
};

/*
 * sysctl_net_init() - 为一个新网络命名空间初始化其内嵌 sysctl 目录集合
 *
 * 业务背景：register_pernet_subsys() 会对 init_net 和以后创建的每个 netns 调用此函数，使协议
 * 子系统随后能把表注册到 &net->sysctls。
 * 入参：@net 是正在构造、由 netns 核心独占并借给回调的对象，不可为空；ownership 不转移。
 * 出参/返回：恒返回 0；副作用是清零并初始化 net->sysctls，绑定共享 root 与 is_seen 回调。
 * 注意事项：__net_init 表示 pernet 初始化阶段；setup 不会注册具体叶表且当前阶段可睡眠，
 * 后续 pernet ops 依赖此集合已就绪。
 */
static int __net_init sysctl_net_init(struct net *net)
{
	setup_sysctl_set(&net->sysctls, &net_sysctl_root, is_seen);
	return 0;
}

/*
 * sysctl_net_exit() - 在网络命名空间退出时确认其 sysctl 集合已经清空
 *
 * 业务背景：pernet 退出按注册逆序执行；协议表应先注销，最后由本层 retire 根集合。
 * 入参：@net 是正在销毁且不再接受新协议注册的借用对象，不可为空。
 * 出参/返回：无直接返回值；retire_sysctl_set() 在目录树非空时 WARN，不负责替遗漏方注销叶表。
 * 注意事项：调用者保证生命周期；警告意味着 pernet 注销次序或 register/unregister 不配对。
 */
static void __net_exit sysctl_net_exit(struct net *net)
{
	retire_sysctl_set(&net->sysctls);
}

/* sysctl pernet 操作表常驻；netns 核心按创建正序调用 init、销毁逆序调用 exit。 */
static struct pernet_operations sysctl_pernet_ops = {
	.init = sysctl_net_init,
	.exit = sysctl_net_exit,
};

/* 保存顶层空目录“net”的注册句柄；只在启动初始化失败时回滚，成功后与 sysctl 子系统同寿命。 */
static struct ctl_table_header *net_header;

/*
 * net_sysctl_init() - 建立共享 /proc/sys/net 挂载点并注册 per-net sysctl 生命周期
 *
 * 业务背景：sysctl 框架不能直接把一个目录同时当普通全局目录和命名空间 root，因此先注册零叶项
 * 的全局 net 目录，再让 lookup 把目录下访问分派到每个 netns 的集合。网络初始化路径调用一次。
 * 入参：无。
 * 出参/返回：成功返回 0；顶层目录分配失败返回 -ENOMEM；pernet 注册失败返回其负 errno。
 * 成功后 net_header 与 sysctl_pernet_ops 均已发布；失败会注销已经建立的顶层目录并清空句柄。
 * 注意事项：__init 表示仅启动期调用且允许睡眠；顺序不能互换，否则 pernet 回调可能面对尚不存在
 * 的共享挂载点。empty 是函数内 static，注册后仍常驻，满足 ctl_table 不可位于栈上的契约。
 */
__init int net_sysctl_init(void)
{
	/* empty 只提供稳定表地址，元素数为 0，所以不会创建叶文件；ret 保存逐阶段的最终状态。 */
	static struct ctl_table empty[1];
	int ret = -ENOMEM;
	/* Avoid limitations in the sysctl implementation by
	 * registering "/proc/sys/net" as an empty directory not in a
	 * network namespace.
	 */
	/*
	 * 为绕过 sysctl 实现限制，先在任何 netns 之外把 /proc/sys/net 注册成空目录；真正叶表仍放入
	 * per-net set。register_sysctl_sz() 返回持有的 header 或 NULL，不使用 ERR_PTR 编码。
	 */
	net_header = register_sysctl_sz("net", empty, 0);
	/* 阶段 1 失败时尚无资源可回收，保留预置 -ENOMEM。 */
	if (!net_header)
		goto out;

	/* 阶段 2 发布 pernet ops：注册会初始化既有 netns，之后也接收新建/退出回调。 */
	ret = register_pernet_subsys(&sysctl_pernet_ops);
	if (ret)
		goto out1;
out:
	/* 统一出口同时承载成功 0、分配 -ENOMEM 和 pernet 注册错误。 */
	return ret;
out1:
	/* pernet ops 未发布成功，唯一已取得资源是 net_header，按构造逆序注销并避免悬空全局指针。 */
	unregister_sysctl_table(net_header);
	net_header = NULL;
	goto out;
}

/* Verify that sysctls for non-init netns are safe by either:
 * 1) being read-only, or
 * 2) having a data pointer which points outside of the global kernel/module
 *    data segment, and rather into the heap where a per-net object was
 *    allocated.
 */
/*
 * 对非初始 netns 的 sysctl 做防泄漏校验：表项要么只读，要么 data 必须指向堆上按 netns 分配的
 * 对象，而不能指向内核或模块全局数据。否则容器的一次写入会跨越隔离边界影响所有命名空间。
 */
/*
 * ensure_safe_net_sysctl() - 审计并降权非 init netns 中可能共享全局数据的可写表项
 *
 * 业务背景：各协议通常复制 ctl_table 但也可能忘记把 data 重定位到 per-net 字段；注册入口在
 * 表发布前调用本函数，把潜在 namespace leak 从“可写全局变量”降级成带 WARN 的只读项。
 * 入参：@net 是目标非 init netns 的借用指针；@path 是 NUL 结尾路径借用字符串；@table 是调用者
 * 拥有、允许本函数修改 mode 的表数组；@table_size 是元素个数而非字节数。各参数均须有效。
 * 出参/返回：无直接返回值；安全项不变，危险项清除 0222 三组写位，并发出一次 WARN。
 * 注意事项：调用者必须在表尚未发布、无并发读写时调用；此启发式只识别 core/module 静态地址，
 * 不能证明任意堆指针确实属于目标 netns，也不会替调用者修复 data ownership。
 */
static void ensure_safe_net_sysctl(struct net *net, const char *path,
				   struct ctl_table *table, size_t table_size)
{
	/* ent 顺序遍历待发布数组；循环内 addr/where 仅用于给可疑 data 地址分类并生成诊断。 */
	struct ctl_table *ent;

	pr_debug("Registering net sysctl (net %p): %s\n", net, path);
	ent = table;
	/* i 控制元素数，ent 与之同步递增；table 不要求 NULL 哨兵，因此不能只看 procname 结束。 */
	for (size_t i = 0; i < table_size; ent++, i++) {
		unsigned long addr;
		const char *where;

		pr_debug("  procname=%s mode=%o proc_handler=%ps data=%p\n",
			 ent->procname, ent->mode, ent->proc_handler, ent->data);

		/* If it's not writable inside the netns, then it can't hurt. */
		/* 非任何身份可写的条目不会通过 sysctl 修改 data，即便指向全局区也不构成该类写泄漏。 */
		if ((ent->mode & 0222) == 0) {
			pr_debug("    Not writable by anyone\n");
			continue;
		}

		/* Where does data point? */
		/* 把 data 转为整数仅做地址区间分类，不解引用；模块区和内核 core data 都是跨 netns 共享。 */
		addr = (unsigned long)ent->data;
		if (is_module_address(addr))
			where = "module";
		else if (is_kernel_core_data(addr))
			where = "kernel";
		else
			/* 堆地址通过本启发式；正确性仍依赖注册者确实为每个 netns 分配独立对象。 */
			continue;

		/* If it is writable and points to kernel/module global
		 * data, then it's probably a netns leak.
		 */
		/* 可写且命中全局区很可能是 netns 泄漏：先报告具体路径、表项与地址归属，便于定位注册者。 */
		WARN(1, "sysctl %s/%s: data points to %s global data: %ps\n",
		     path, ent->procname, where, ent->data);

		/* Make it "safe" by dropping writable perms */
		/* 在随后发布前清掉 owner/group/other 全部写位；读权限保留，使观测可用但阻断跨容器修改。 */
		ent->mode &= ~0222;
	}
}

/*
 * register_net_sysctl_sz() - 向指定网络命名空间注册一组 sysctl 叶表
 *
 * 业务背景：IPv4/IPv6/core 等 pernet 初始化路径通过此统一入口把表挂到 net/<path>；它在通用
 * __register_sysctl_table() 之前为非 init netns 增加全局数据泄漏防护。
 * 入参：@net 是目标 netns 的借用指针；@path 是相对 sysctl 根的只读路径；@table 是调用者持有且
 * 注册期后必须持续有效的表数组，非 init netns 时其 mode 可能被降权；@table_size 是元素数。
 * 出参/返回：成功返回需由 unregister_net_sysctl_table() 注销的 header；分配/校验/插入失败返回
 * NULL。函数不接管 table 内存，注销完成后调用者才可释放动态表。
 * 注意事项：注册会分配内存并获取 sysctl 锁，必须在可睡眠上下文；同一表不可在发布后并发改写。
 */
struct ctl_table_header *register_net_sysctl_sz(struct net *net,
						const char *path,
						struct ctl_table *table,
						size_t table_size)
{
	/* init_net 可以合法承载历史全局参数；隔离风险只针对可由非初始命名空间访问的表。 */
	if (!net_eq(net, &init_net))
		ensure_safe_net_sysctl(net, path, table, table_size);

	/* 发布边界：成功返回后 proc/sysctl 查找者可通过 net->sysctls 获得这些表项。 */
	return __register_sysctl_table(&net->sysctls, path, table, table_size);
}
EXPORT_SYMBOL_GPL(register_net_sysctl_sz);

/*
 * unregister_net_sysctl_table() - 注销 register_net_sysctl_sz() 返回的表
 *
 * 业务背景：协议的 pernet exit 或初始化回滚调用此对称包装，先阻止新 proc 查找，再由 sysctl
 * 核心等待/延迟处理仍在使用的 inode/header 引用。
 * 入参：@header 是注册成功获得的持有句柄；可为 NULL，通用注销函数会把它当作无操作。
 * 出参/返回：无直接返回值；表从目录树摘除，注销可能睡眠，header 最终由 sysctl 核心释放。
 * 注意事项：调用后不得再访问 header；动态 ctl_table 也必须遵循通用 sysctl 注销完成后的生命周期。
 */
void unregister_net_sysctl_table(struct ctl_table_header *header)
{
	/* 网络包装不增加额外状态，所有引用等待、RCU 回收与 NULL 容忍语义都委托给通用层。 */
	unregister_sysctl_table(header);
}
EXPORT_SYMBOL_GPL(unregister_net_sysctl_table);
