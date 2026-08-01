// SPDX-License-Identifier: GPL-2.0-only
/*
 * 构建末期生成的 UTS 初始身份与启动 banner
 *
 * 本文件把 generated/compile.h、generated/utsrelease.h 和 linux/uts.h 中的
 * 构建字符串固化进 init_uts_ns 与 linux_banner。init/version.c 先提供
 * weak 占位定义，并在最终链接前通过包含本文件得到带真实时间戳的强
 * 定义；因此这里的数据代表本次内核镜像，而不是运行期动态探测结果。
 */

#include <generated/compile.h>
#include <generated/utsrelease.h>
#include <linux/proc_ns.h>
#include <linux/refcount.h>
#include <linux/uts.h>
#include <linux/utsname.h>

/*
 * 永久存活的初始 UTS namespace。NS_COMMON_INIT() 静态建立固定类型、ID、
 * inode、操作表以及普通/active 引用 1；name 的六个字段来自构建配置与生成
 * 头文件；user_ns 指向永久初始 user namespace。ucounts 保持零值，因为初始
 * 对象不受动态 UTS namespace 配额约束，也永不进入 free_uts_ns()。
 *
 * 运行期 sethostname()/setdomainname() 可以修改 nodename/domainname，其他
 * 字段主要表达内核构建身份。对象在 uts_ns_init() 中加入 namespace 树。
 */
struct uts_namespace init_uts_ns = {
	.ns = NS_COMMON_INIT(init_uts_ns),
	.name = {
		.sysname	= UTS_SYSNAME,
		.nodename	= UTS_NODENAME,
		.release	= UTS_RELEASE,
		.version	= UTS_VERSION,
		.machine	= UTS_MACHINE,
		.domainname	= UTS_DOMAINNAME,
	},
	.user_ns = &init_user_ns,
};

/* FIXED STRINGS! Don't touch! */
/*
 * 这些是固定字符串，禁止改动。linux_banner 通过编译期字面量拼接生成，
 * 供启动日志等路径打印完整的 release、构建用户/主机、编译器与版本
 * 时间戳。
 * 数组内容本身只读且贯穿内核生命周期，不随 UTS namespace 或 hostname
 * 改变；修改格式还会破坏依赖既有 banner 形态的工具和诊断流程。
 */
const char linux_banner[] =
	"Linux version " UTS_RELEASE " (" LINUX_COMPILE_BY "@"
	LINUX_COMPILE_HOST ") (" LINUX_COMPILER ") " UTS_VERSION "\n";
