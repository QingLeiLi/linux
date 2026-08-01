// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/init/version.c
 *
 *  Copyright (C) 1992  Theodore Ts'o
 *
 *  May be freely distributed as part of Linux.
 */
/*
 * 内核构建身份初始化导读
 *
 * 本文件连接启动参数、生成头文件和 UTS 初始对象。early_hostname() 在启动
 * 早期覆盖 init_uts_ns.name.nodename；linux_proc_banner/linux_banner 提供
 * procfs 与启动日志使用的构建描述；version-timestamp.c 则在构建最后阶段
 * 注入真实 release、编译器和时间戳。此阶段尚无并发 UTS 写者，因此早期
 * hostname 更新不使用运行期 uts_sem。
 */

#include <generated/compile.h>
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/uts.h>
#include <linux/utsname.h>
#include <linux/proc_ns.h>

/*
 * early_hostname() - 处理内核命令行 hostname=，初始化初始主机名。
 *
 * @arg 是命令行解析器提供的 NUL 结尾借用字符串，不可为空。函数运行在
 * 单线程启动早期，init_uts_ns 已静态存在但尚未进入正常并发使用，无需
 * uts_sem。strscpy() 最多写入字段容量并始终保持结尾 NUL；超长输入被截断
 * 并打印警告，但函数仍返回 0，让启动继续使用截断后的主机名。
 */
static int __init early_hostname(char *arg)
{
	/* bufsize 包含 NUL 空间；maxlen 是用户可见主机名的最大字符数。 */
	size_t bufsize = sizeof(init_uts_ns.name.nodename);
	size_t maxlen  = bufsize - 1;
	/* 非负值是实际复制长度，负值表示源字符串超过目标容量。 */
	ssize_t arglen;

	arglen = strscpy(init_uts_ns.name.nodename, arg, bufsize);
	if (arglen < 0) {
		/*
		 * 截断已安全完成；这里只报告实际生效的上限，不回滚字段。
		 */
		pr_warn("hostname parameter exceeds %zd characters and will be truncated",
			maxlen);
	}
	return 0;
}
/* 把 early_hostname() 注册为仅在启动命令行解析阶段可用的 hostname= handler。 */
early_param("hostname", early_hostname);

/*
 * /proc/version 使用的格式模板。调用者依次提供 sysname、release 和 version；
 * 构建用户/主机与编译器字符串已在编译期嵌入。数组永久只读，
 * 不属于任何单独 UTS namespace，运行期 hostname 修改不会影响它。
 */
const char linux_proc_banner[] =
	"%s version %s"
	" (" LINUX_COMPILE_BY "@" LINUX_COMPILE_HOST ")"
	" (" LINUX_COMPILER ") %s\n";

/*
 * 把构建 salt 与 LTO 元信息放入内核镜像的约定 section，供模块版本、安全
 * 加固和链接工具消费；宏不创建运行期可修改的 UTS 状态。
 */
BUILD_SALT;
BUILD_LTO_INFO;

/*
 * init_uts_ns and linux_banner contain the build version and timestamp,
 * which are really fixed at the very last step of build process.
 * They are compiled with __weak first, and without __weak later.
 */
/*
 * init_uts_ns 与 linux_banner 含有只能在构建最后一步确定的版本和时间戳。
 * 构建流程先以 __weak 形式编译占位定义，随后再用非 weak 的最终定义
 * 覆盖。这样绝大多数对象无需因时间戳变化而重编译，同时最终镜像仍
 * 携带准确身份。
 */

/*
 * weak 占位对象仅服务构建流程；最终字段与引用布局由下方
 * 生成文件给出。
 */
struct uts_namespace init_uts_ns __weak;
const char linux_banner[] __weak;

/* 引入构建末期生成的强定义：初始 UTS 字段、ns_common 和最终 banner。 */
#include "version-timestamp.c"

/* 允许其他内核组件读取永久初始 UTS 对象；导出不额外增加对象引用。 */
EXPORT_SYMBOL_GPL(init_uts_ns);
