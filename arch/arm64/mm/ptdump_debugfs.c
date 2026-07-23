// SPDX-License-Identifier: GPL-2.0
/*
 * arm64 页表转储的 debugfs 适配层。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件只把 ptdump_info 绑定到 seq_file；真正页表遍历、锁和格式化位于
 * ptdump.c。debugfs 仅供诊断且不构成稳定 ABI，文件以 0400 限制为只读。
 */
#include <linux/debugfs.h>
#include <linux/seq_file.h>

#include <asm/ptdump.h>

/* seq_file show 回调：m 是输出流，v 对单次文件无意义；private 借用注册时 info。 */
static int ptdump_show(struct seq_file *m, void *v)
{
	struct ptdump_info *info = m->private;

	ptdump_walk(m, info);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ptdump);

/*
 * 启动期创建名为 name 的根 debugfs 文件，并把长期有效的 info 作为
 * inode private data。debugfs_create_file 失败可返回 NULL/错误但诊断功能
 * 不值得阻断启动，因此接口无返回、无回滚；info 所有权仍属于调用者。
 */
void __init ptdump_debugfs_register(struct ptdump_info *info, const char *name)
{
	debugfs_create_file(name, 0400, NULL, info, &ptdump_fops);
}
