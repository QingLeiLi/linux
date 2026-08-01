/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 通用栈保护 canary 接口学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 文件职责：为各体系结构提供统一的 canary 生成规则，并把启动任务初始化分派
 * 给体系结构实现。编译器的 -fstack-protector 插桩在函数入口保存 guard、在返回
 * 前比较；本文件只负责产生满足 ABI 安全约束的随机值，不负责选择受保护函数，
 * 也不实现比较失败后的 __stack_chk_fail() 处理。
 *
 * 主要调用链：
 *   start_kernel() -> boot_init_stack_canary() -> <asm/stackprotector.h>
 *   dup_task_struct() -> get_random_canary() -> get_random_long()
 *
 * 核心状态：canary 是 unsigned long 值，不拥有动态资源。启动任务由架构入口写入；
 * 后续任务在复制 task_struct 时取得新值。随机值写入任务或全局 guard 后，其读写
 * 同步由对应架构的任务切换/启动协议负责，本通用层不加锁。
 *
 * 方案权衡：64 位平台牺牲 8 位随机性换取一个内存首字节 NUL，以阻断一类未终止
 * C 字符串连续覆盖；32 位平台保留全部 32 位，因为再牺牲一个字节会明显降低本就
 * 有限的熵。体系结构可进一步决定使用全局、per-CPU 或 per-task guard。
 */
#ifndef _LINUX_STACKPROTECTOR_H
#define _LINUX_STACKPROTECTOR_H 1

/*
 * include guard 令本头文件在一个翻译单元内只定义一次。值 1 没有运行时含义，
 * 但允许其他预处理判断把该宏当作已包含标志使用。
 */

#include <linux/compiler.h>
#include <linux/sched.h>
#include <linux/random.h>
/*
 * compiler.h 提供 inline/属性等编译器抽象；sched.h 使架构实现可以访问 current
 * 与 task_struct::stack_canary；random.h 提供不睡眠的内核随机字接口。三者只
 * 建立编译期依赖，不创建对象或转移 ownership。
 */

/*
 * On 64-bit architectures, protect against non-terminated C string overflows
 * by zeroing out the first byte of the canary; this leaves 56 bits of entropy.
 */
/*
 * 在 64 位体系结构上，把 canary 在内存布局中的第一个字节清零，可以防御没有
 * NUL 终止的 C 字符串越界：逐字节复制到 guard 时会先遇到零字节而停止。代价是
 * 64 位随机值只剩 56 位熵。这里所说的“第一个字节”是最低地址字节，因此掩码
 * 必须按大小端选择；它不是固定等同于整数的最低有效 8 位。
 */
#ifdef CONFIG_64BIT
/* 64 位小端把最低有效字节放在最低地址，故清除低 8 位。 */
# ifdef __LITTLE_ENDIAN
#  define CANARY_MASK 0xffffffffffffff00UL
# else /* big endian, 64 bits: */
/*
 * 上方英文说明该分支用于 64 位大端：最低地址保存最高有效字节，因此清除高
 * 8 位，仍保留其余 56 位随机性。
 */
#  define CANARY_MASK 0x00ffffffffffffffUL
# endif
#else /* 32 bits: */
/*
 * 上方英文说明该分支用于 32 位体系结构。掩码保留整个 unsigned long，不主动
 * 制造 NUL 字节，以避免把 canary 的有效搜索空间从 32 位进一步降到 24 位。
 */
# define CANARY_MASK 0xffffffffUL
#endif

/*
 * get_random_canary() - 生成符合通用栈保护布局约束的机器字 canary。
 *
 * 调用关系：启动阶段的体系结构 boot_init_stack_canary() 与新任务构造路径
 * dup_task_struct() 调用本函数，返回值随后写入全局或 task_struct guard。
 *
 * 入参：无。调用者不需要持锁；get_random_long() 可在不能睡眠的早期启动和任务
 * 构造路径使用。本函数不分配资源，不获得引用，也没有失败返回。
 *
 * 返回：一个 unsigned long 随机值。64 位构建保证内存首字节为零并保留 56 位
 * 随机性；32 位构建保留全部位。返回值尚未发布，写入何处以及何时对编译器插桩
 * 可见由调用者负责。
 */
static inline unsigned long get_random_canary(void)
{
	/* 按位与只施加布局约束，不改变随机值其余位，也没有共享状态副作用。 */
	return get_random_long() & CANARY_MASK;
}

/*
 * 启用栈保护时，架构必须初始化编译器读取的 guard。arm64 还复用同一永不返回的
 * 启动边界初始化内核 PAC key，所以即使 STACKPROTECTOR 关闭、只启用
 * ARM64_PTR_AUTH，也必须进入架构头文件。其他配置则使用下方无副作用桩函数，
 * 让 start_kernel() 无需散布条件编译。
 */
#if defined(CONFIG_STACKPROTECTOR) || defined(CONFIG_ARM64_PTR_AUTH)
# include <asm/stackprotector.h>
#else
/*
 * boot_init_stack_canary() - 在无需栈保护和 arm64 PAC 时保持统一启动接口。
 *
 * 入参：无。返回：无直接返回值。任何上下文均不会睡眠、失败或改变状态；编译器
 * 会内联并消除空函数体。它仅保证通用启动代码在所有配置下都能无条件调用同名
 * 接口，不表示系统实际获得了 canary 保护。
 */
static inline void boot_init_stack_canary(void)
{
}
#endif

#endif
/* 上一行结束 _LINUX_STACKPROTECTOR_H include guard。 */
