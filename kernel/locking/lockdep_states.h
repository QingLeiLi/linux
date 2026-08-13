/*
 * Lockdep states,
 *
 * please update XXX_LOCK_USAGE_STATES in include/linux/lockdep.h whenever
 * you add one, or come up with a nice dynamic solution.
 */
/*
 * Lockdep 状态表。
 *
 * 这里故意没有 include guard，也不是一份可以单独编译的普通头文件，而是 X-macro 的“数据源”。
 * 包含者先把 LOCKDEP_STATE(state) 定义成所需的代码生成模板，再包含本文件；同一编译单元可以
 * 反复换模板并包含它，从这份唯一且顺序固定的状态清单生成 usage-bit 枚举、位掩码、诊断字符串
 * 和检查函数表。这样能避免这些平行表分别维护后发生编号错位。
 *
 * 每增加一个 LOCKDEP_STATE，lockdep 内部会为它生成四种锁类使用状态：在该上下文中写持有、
 * 在该上下文中读持有、在该上下文已开启时写持有，以及在该上下文已开启时读持有。
 * include/linux/lockdep_types.h 中的 XXX_LOCK_USAGE_STATES 保存本表的行数，并据此计算
 * LOCK_TRACE_STATES；因此增删条目必须同步更新该常量，否则 trace 数组容量和实际枚举数量会
 * 不一致。原注释写的是 include/linux/lockdep.h，但当前版本的常量实际位于
 * include/linux/lockdep_types.h；保留原文，并以当前源码位置作为维护依据。
 */
/*
 * HARDIRQ 表示硬中断上下文及“硬中断已开启”环境。它排在第一位，因此由包含模板派生的四个
 * usage bit 构成编号 0～3；后续通过低两位编码读/写方向与“使用/开启”方向的逻辑依赖此布局。
 */
LOCKDEP_STATE(HARDIRQ)
/*
 * SOFTIRQ 表示软中断上下文及“软中断已开启”环境。它沿用同一四状态布局；lockdep 用这些信息
 * 判断一把锁能否从进程、软中断和硬中断路径以特定读写方式安全地嵌套，而不是表示一把真实锁。
 */
LOCKDEP_STATE(SOFTIRQ)
