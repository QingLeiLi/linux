/* SPDX-License-Identifier: GPL-2.0 */

/*
 * 用户态测试工具的 per-CPU 变量模拟层
 *
 * 在真实内核中，per-CPU 变量为每个 CPU 核心维护一份独立副本，以消除缓存行
 * 竞争、避免跨 CPU 的锁开销。用户态测试程序无法使用内核的 per-CPU 基础设施，
 * 因此本文件将所有 per-CPU 操作退化为对同一个普通全局变量的操作，让内核代码
 * 无需修改即可在用户态编译和运行。
 *
 * 注意：这些宏不提供真正的 CPU 隔离语义，仅用于功能验证测试。
 */

/*
 * DECLARE_PER_CPU - 声明一个 per-CPU 变量（用于头文件中的外部声明）
 * DEFINE_PER_CPU  - 定义一个 per-CPU 变量（分配存储）
 *
 * 用户态退化为普通的 extern/非 extern 全局变量声明。
 */
#define DECLARE_PER_CPU(type, val) extern type val
#define DEFINE_PER_CPU(type, val) type val

/*
 * __get_cpu_var - 获取当前 CPU 的 per-CPU 变量值（已废弃接口，内核中已移除）
 * this_cpu_ptr  - 返回当前 CPU 上该 per-CPU 变量的指针
 * this_cpu_read - 读取当前 CPU 上该 per-CPU 变量的值
 *
 * 用户态退化：直接返回变量本身，不做任何 CPU 选择。
 */
#define __get_cpu_var(var)	var
#define this_cpu_ptr(var)	var
#define this_cpu_read(var)	var

/*
 * this_cpu_xchg    - 原子地将当前 CPU 的 per-CPU 变量替换为新值，返回旧值
 * this_cpu_cmpxchg - 原子地比较并交换当前 CPU 的 per-CPU 变量：
 *                    若当前值 == old，则写入 new 并返回 old；否则返回当前值
 *
 * 用户态通过 liburcu 提供的 uatomic_xchg/uatomic_cmpxchg 实现原子语义，
 * 与内核的 this_cpu_* 原子操作保持相同的接口契约。
 */
#define this_cpu_xchg(var, val)		uatomic_xchg(&var, val)
#define this_cpu_cmpxchg(var, old, new)	uatomic_cmpxchg(&var, old, new)

/*
 * per_cpu_ptr - 获取指定 CPU 上 per-CPU 指针 ptr 所指向的地址
 * @ptr: per-CPU 变量的基指针
 * @cpu: 目标 CPU 编号
 *
 * 用户态退化：忽略 cpu 参数（通过 (void)(cpu) 消除未使用变量警告），
 * 直接返回 ptr 本身，所有 CPU 共享同一份数据。
 */
#define per_cpu_ptr(ptr, cpu)   ({ (void)(cpu); (ptr); })

/*
 * per_cpu - 获取指定 CPU 上 per-CPU 变量 var 的左值引用
 * @var: per-CPU 变量名（非指针）
 * @cpu: 目标 CPU 编号
 *
 * 展开为 *per_cpu_ptr(&(var), cpu)，先取变量地址再解引用，
 * 使调用方可以像访问普通变量一样读写该 per-CPU 槽位。
 */

 #define per_cpu(var, cpu)	(*per_cpu_ptr(&(var), cpu))
