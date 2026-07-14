/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _IO_H
#define _IO_H

#include "types.h"

/*
 * Low-level I/O routines.
 *
 * Copied from <file:arch/powerpc/include/asm/io.h> (which has no copyright)
 *
 * PowerPC 是大端（Big-Endian）架构，寄存器和内存均以大端字节序存储。
 * MMIO（内存映射 I/O）设备可能是大端或小端，因此提供两组读写函数：
 *   in_beN / out_beN：设备是大端，直接读写，无需字节交换
 *   in_leN / out_leN：设备是小端，读写时需要字节序反转
 *
 * 所有读函数结尾均有 "twi 0,%0,0; isync" 屏障，所有写函数结尾均有 "sync"，
 * 原因见各函数注释。
 *
 * 内联汇编操作数约定：
 *   "=r"  输出到通用寄存器
 *   "=m"  输出到内存（让编译器知道该地址被写入）
 *   "r"   输入来自通用寄存器
 *   "m"   输入来自内存（让编译器知道该地址被读取）
 *   %UN   若编译器为操作数 N 选择了 update 寻址模式，展开为 'u'，否则为空
 *   %XN   若编译器为操作数 N 选择了 indexed 寻址模式，展开为 'x'，否则为空
 *   两者组合使 lbz%U1%X1 可按需展开为 lbz / lbzu / lbzx / lbzux 四种形式
 */

/*
 * in_8 - 从 MMIO 地址读取 1 字节（无字节序问题，单字节天然无端序）
 * @addr: 映射到 I/O 空间的内存地址，volatile 防止编译器缓存该地址的值
 *
 * 汇编序列：lbz; twi; isync
 *   lbz%U1%X1 %0,%1
 *     Load Byte and Zero-extend：从 *addr 读 1 字节，零扩展到 32 位存入 ret。
 *     %U1%X1 让编译器自由选择寻址模式（位移/索引/更新）。
 *
 *   twi 0,%0,0
 *     Trap Word Immediate，TO=0 表示永远不触发 trap。
 *     其真实作用是在 ret 上建立数据依赖：CPU 必须等 lbz 完成、ret 值确定后，
 *     才能执行 twi 的条件判断。这迫使乱序处理器不能将后续指令投机执行到
 *     lbz 尚未完成之前，等效于一个"读完成"屏障。
 *
 *   isync
 *     Instruction Synchronize：清空指令流水线，确保在此之后取到的指令
 *     都是在 isync 之后重新 fetch 的，防止 CPU 投机执行后续代码时
 *     使用了尚未从 MMIO 设备返回的旧数据。
 *
 *   twi + isync 的组合是 PowerPC MMIO 读的标准完成屏障，
 *   等价于"等待设备响应后再继续"。
 */
static inline int in_8(const volatile unsigned char *addr)
{
	int ret;

	__asm__ __volatile__("lbz%U1%X1 %0,%1; twi 0,%0,0; isync"
			     : "=r" (ret) : "m" (*addr));
	return ret;
}

/*
 * out_8 - 向 MMIO 地址写入 1 字节
 * @addr: 目标 MMIO 地址
 * @val:  要写入的值（只使用低 8 位）
 *
 * 汇编序列：stb; sync
 *   stb%U0%X0 %1,%0
 *     Store Byte：将 val 的低 8 位写入 *addr。
 *
 *   sync
 *     Full Memory Barrier（重量级同步屏障）：确保 stb 产生的总线写事务
 *     已经提交到系统总线（设备侧可见），之后的任何指令都不会被提前。
 *     MMIO 写后必须有 sync，否则后续轮询设备状态寄存器时
 *     可能读到写入之前的旧值（写尚未抵达设备）。
 */
static inline void out_8(volatile unsigned char *addr, int val)
{
	__asm__ __volatile__("stb%U0%X0 %1,%0; sync"
			     : "=m" (*addr) : "r" (val));
}

/*
 * in_le16 - 从 MMIO 地址读取 16 位小端值，转换为 CPU（大端）字节序
 * @addr: 指向小端设备寄存器的地址
 *
 * 汇编序列：lhbrx; twi; isync
 *   lhbrx %0,0,%1
 *     Load Halfword Byte-Reversed Indexed：从 addr+0 读 2 字节，
 *     在加载时硬件自动反转字节序（lo-byte↔hi-byte），结果零扩展到 32 位。
 *     PowerPC 是大端，lhbrx 将小端内存值直接转换为大端寄存器值，无需软件交换。
 *     注意：brx 系列指令只有 indexed 寻址（需要基址寄存器），
 *     因此必须将 addr 作为寄存器操作数 "r"(addr) 传入，
 *     而非像 lhz 那样用内存约束 "m"(*addr)。
 *
 *   twi 0,%0,0; isync：同 in_8，强制等待读完成。
 */
static inline unsigned in_le16(const volatile u16 *addr)
{
	unsigned ret;

	__asm__ __volatile__("lhbrx %0,0,%1; twi 0,%0,0; isync"
			     : "=r" (ret) : "r" (addr), "m" (*addr));

	return ret;
}

/*
 * in_be16 - 从 MMIO 地址读取 16 位大端值（与 CPU 字节序相同，无需转换）
 * @addr: 指向大端设备寄存器的地址
 *
 * 汇编序列：lhz; twi; isync
 *   lhz%U1%X1 %0,%1
 *     Load Halfword and Zero-extend：从 *addr 读 2 字节，零扩展到 32 位。
 *     不做字节序转换，直接反映内存原始值（大端与 CPU 一致）。
 *
 *   twi 0,%0,0; isync：同 in_8，强制等待读完成。
 */
static inline unsigned in_be16(const volatile u16 *addr)
{
	unsigned ret;

	__asm__ __volatile__("lhz%U1%X1 %0,%1; twi 0,%0,0; isync"
			     : "=r" (ret) : "m" (*addr));
	return ret;
}

/*
 * out_le16 - 向 MMIO 地址写入 16 位值，自动转换为小端字节序
 * @addr: 目标小端设备寄存器地址
 * @val:  要写入的值（CPU 大端序），函数负责反转后写入
 *
 * 汇编序列：sthbrx; sync
 *   sthbrx %1,0,%2
 *     Store Halfword Byte-Reversed Indexed：将 val 的低 16 位字节反转后
 *     写入 addr+0。硬件完成大端→小端转换，设备看到的是小端字节序。
 *     同样只有 indexed 寻址，故 addr 以 "r"(addr) 传入。
 *
 *   sync：同 out_8，确保写事务提交到总线后再继续。
 */
static inline void out_le16(volatile u16 *addr, int val)
{
	__asm__ __volatile__("sthbrx %1,0,%2; sync" : "=m" (*addr)
			     : "r" (val), "r" (addr));
}

/*
 * out_be16 - 向 MMIO 地址写入 16 位大端值（无字节序转换）
 * @addr: 目标大端设备寄存器地址
 * @val:  要写入的值
 *
 * 汇编序列：sth; sync
 *   sth%U0%X0 %1,%0
 *     Store Halfword：将 val 的低 16 位原样写入 *addr，不做字节序转换。
 *
 *   sync：同 out_8。
 */
static inline void out_be16(volatile u16 *addr, int val)
{
	__asm__ __volatile__("sth%U0%X0 %1,%0; sync"
			     : "=m" (*addr) : "r" (val));
}

/*
 * in_le32 - 从 MMIO 地址读取 32 位小端值，转换为 CPU（大端）字节序
 * @addr: 指向小端设备寄存器的地址
 *
 * 汇编序列：lwbrx; twi; isync
 *   lwbrx %0,0,%1
 *     Load Word Byte-Reversed Indexed：从 addr+0 读 4 字节，
 *     硬件反转全部 4 个字节的顺序，零扩展到寄存器宽度。
 *     同样只有 indexed 寻址，addr 以 "r"(addr) 传入。
 *
 *   twi 0,%0,0; isync：同 in_8，强制等待读完成。
 */
static inline unsigned in_le32(const volatile unsigned *addr)
{
	unsigned ret;

	__asm__ __volatile__("lwbrx %0,0,%1; twi 0,%0,0; isync"
			     : "=r" (ret) : "r" (addr), "m" (*addr));
	return ret;
}

/*
 * in_be32 - 从 MMIO 地址读取 32 位大端值（与 CPU 字节序相同，无需转换）
 * @addr: 指向大端设备寄存器的地址
 *
 * 汇编序列：lwz; twi; isync
 *   lwz%U1%X1 %0,%1
 *     Load Word and Zero-extend：从 *addr 读 4 字节，不做字节序转换。
 *
 *   twi 0,%0,0; isync：同 in_8，强制等待读完成。
 */
static inline unsigned in_be32(const volatile unsigned *addr)
{
	unsigned ret;

	__asm__ __volatile__("lwz%U1%X1 %0,%1; twi 0,%0,0; isync"
			     : "=r" (ret) : "m" (*addr));
	return ret;
}

/*
 * out_le32 - 向 MMIO 地址写入 32 位值，自动转换为小端字节序
 * @addr: 目标小端设备寄存器地址
 * @val:  要写入的值（CPU 大端序）
 *
 * 汇编序列：stwbrx; sync
 *   stwbrx %1,0,%2
 *     Store Word Byte-Reversed Indexed：将 val 的 4 个字节反转后写入 addr+0。
 *     硬件完成大端→小端转换。
 *
 *   sync：同 out_8，确保写事务提交。
 */
static inline void out_le32(volatile unsigned *addr, int val)
{
	__asm__ __volatile__("stwbrx %1,0,%2; sync" : "=m" (*addr)
			     : "r" (val), "r" (addr));
}

/*
 * out_be32 - 向 MMIO 地址写入 32 位大端值（无字节序转换）
 * @addr: 目标大端设备寄存器地址
 * @val:  要写入的值
 *
 * 汇编序列：stw; sync
 *   stw%U0%X0 %1,%0
 *     Store Word：将 val 原样写入 *addr，不做字节序转换。
 *
 *   sync：同 out_8。
 */
static inline void out_be32(volatile unsigned *addr, int val)
{
	__asm__ __volatile__("stw%U0%X0 %1,%0; sync"
			     : "=m" (*addr) : "r" (val));
}

/*
 * sync - PowerPC 全内存屏障（硬件级）
 *
 * 发出 PowerPC "sync"（也称 hwsync）指令：
 *   - 等待所有之前发出的内存访问（load/store）在系统总线上完成
 *   - 之后的任何指令都不会被提前到 sync 之前执行
 *   - 等价于 x86 的 mfence，是最重量级的 PowerPC 内存屏障
 *
 * "memory" clobber 同时充当编译器屏障，禁止编译器跨此处重排内存访问。
 * 用于 MMIO 写之后，确保设备侧已看到写操作，再执行后续的状态轮询等操作。
 */
static inline void sync(void)
{
	asm volatile("sync" : : : "memory");
}

/*
 * eieio - Enforce In-order Execution of I/O（I/O 顺序屏障）
 *
 * 发出 PowerPC "eieio" 指令，是比 sync 更轻量的 I/O 专用屏障：
 *   - 确保之前所有对 I/O 空间（弱序内存）的 store 按程序顺序到达设备
 *   - 不等待 store 完成，只保证顺序，延迟低于 sync
 *   - 不影响普通内存（cacheable memory）的访问顺序
 *
 * 典型用法：向同一设备连续写入多个寄存器时，用 eieio 保证写序，
 * 但无需等待每次写都完成（最后一次写后再用 sync 等待全部完成）。
 * "memory" clobber 同时充当编译器屏障。
 */
static inline void eieio(void)
{
	asm volatile("eieio" : : : "memory");
}

/*
 * barrier - 纯编译器屏障（不生成任何机器指令）
 *
 * 展开为：asm volatile("" : : : "memory")
 *   ""        汇编模板为空字符串，不生成任何机器码，运行时零开销
 *   volatile  禁止编译器删除或移动这条 asm（即使它看起来没有副作用）
 *   "memory"  clobber：告诉编译器此处可能读写任意内存，
 *             因此必须将所有活跃的内存写入刷新到内存模型，
 *             并且不能将此处之后的内存读取提前到此处之前
 *
 * 效果：禁止编译器跨越此处重排内存访问，但 CPU 仍可乱序执行。
 * 若需同时约束 CPU 乱序，应使用 sync()（硬件屏障）。
 * 常用于：关中断/开中断前后防止编译器将临界区代码移出保护范围。
 *
 * "memory" clobber 本质上是向编译器撒了一个谎：

▎ "这条 asm 语句可能读写了任意内存地址。"

编译器信以为真，于是被迫做两件事：

1. barrier 之前 — 将所有寄存器中缓存的内存值写回内存

编译器为了优化，常把内存变量的值暂存在寄存器里避免重复读取。但 "memory" 一出现，它不知道哪个地址被修改了，只能保守地将所有"脏"寄存器值刷回对应的内存位置。

2. barrier 之后 — 不再复用寄存器中的旧值，必须重新从内存读取

后续代码再访问任何内存变量时，编译器不敢说"我寄存器里已经有这个值了"，必须重新生成 load 指令。

int x = *addr;   // 编译器可能把 x 缓存在寄存器 r3
barrier();       // 编译器认为 r3 可能已被 asm 破坏，不可信
int y = *addr;   // 必须重新生成 load 指令，不能复用 r3

关键限制：这一切只发生在编译器的视角，CPU 在运行时完全不知道 barrier 的存在，乱序执行照常进行。所以：

- 防编译器重排 → barrier()（零指令）
- 防 CPU 乱序 → sync()（真实硬件指令）
- 两者都防 → 两个都写，或用 sync()（它自带 "memory" clobber）
 */
static inline void barrier(void)
{
	asm volatile("" : : : "memory");
}

#endif /* _IO_H */
