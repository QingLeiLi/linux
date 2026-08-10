/* SPDX-License-Identifier: GPL-2.0 */
/*
 * IRQ subsystem internal functions and variables:
 *
 * Do not ever include this file from anything else than
 * kernel/irq/. Do not even think about using any information outside
 * of this file for your non core code.
 */
/*
 * 本文件声明 IRQ 核心内部共享的函数、状态和变量。上面的警告要求它只能被
 * kernel/irq/ 内部实现包含：驱动和其他子系统若依赖这里的字段或 helper，会绕过
 * <linux/irq.h> 提供的稳定契约，并在 IRQ 核心重构锁、状态位或生命周期时失效。
 *
 * 阅读地图：顶部定义 irqaction 线程标志和 irq_desc 瞬时状态；中部连接启动、
 * 关闭、重发、proc、亲和性与描述符加锁协议；后部提供 irq_data 状态、统计、
 * 电源管理和待处理迁移的内联访问器。这里多数 helper 不自行稳定 irq_desc：调用者
 * 必须依靠 desc->lock、RCU/rcuref 或尚未发布时的独占关系满足各实现的入口条件。
 */
#include <linux/irqdesc.h>
#include <linux/kernel_stat.h>
#include <linux/pm_runtime.h>
#include <linux/rcuref.h>
#include <linux/sched/clock.h>

#include "debugfs.h"
#include "proc.h"

/*
 * 稀疏 IRQ 模式用 Maple Tree 按需保存描述符，因此理论 IRQ 号上界可扩到 int
 * 能表达的最大正值；非稀疏模式使用静态描述符数组，只能在编译期 NR_IRQS 范围
 * 内索引。该常量约束分配空间，运行时实际可见数量仍由 total_nr_irqs 控制。
 */
#ifdef CONFIG_SPARSE_IRQ
# define MAX_SPARSE_IRQS	INT_MAX
#else
# define MAX_SPARSE_IRQS	NR_IRQS
#endif

/*
 * irq_desc 对外故意把该字段命名为 core_internal_state__do_not_mess_with_it；只有
 * 包含本内部头的 IRQ 核心才能通过 desc->istate 这个短别名访问。宏只形成访问
 * 栅栏，不提供同步；字段的并发读改写仍主要由 desc->lock 串行化。
 */
#define istate core_internal_state__do_not_mess_with_it

/*
 * noirqdebug 是启动参数/模块参数控制的只读为主策略。spurious.c 定义并更新它，
 * manage.c 在安装 action 时据此设置 NO_DEBUG；true 表示关闭 IRQ 锁死与杂散检测
 * 启发式，而不是关闭全部 IRQ 调试输出。
 */
extern bool noirqdebug;
/*
 * irq_poll_cpu 记录当前执行杂散 IRQ 轮询的 CPU 编号。spurious.c 写入，chip.c 用它
 * 检测同一 CPU 上的递归轮询；它是轮询协议状态，不是 IRQ 的目标亲和性。
 */
extern int irq_poll_cpu;
/*
 * total_nr_irqs 是当前逻辑 IRQ 编号空间的排他上界，由 irqdesc.c 初始化/扩展，
 * 分配、查找和 /proc 输出路径读取。它表示数量/哨兵，不是最后一个有效 IRQ 号。
 */
extern unsigned int total_nr_irqs;

/*
 * chained_action 是所有级联控制器描述符共用的哨兵 action。chip.c 把 desc->action
 * 指向它来标识“由父 IRQ 流处理器继续分发”，并不当作普通驱动 action 执行；
 * 指针身份比较因此是协议的一部分，对象静态存活且不转移所有权。
 */
extern struct irqaction chained_action;

/*
 * Bits used by threaded handlers:
 * IRQTF_RUNTHREAD - signals that the interrupt handler thread should run
 * IRQTF_WARNED    - warning "IRQ_WAKE_THREAD w/o thread_fn" has been printed
 * IRQTF_AFFINITY  - irq thread is requested to adjust affinity
 * IRQTF_FORCED_THREAD  - irq action is force threaded
 * IRQTF_READY     - signals that irq thread is ready
 */
/*
 * 这些位保存在每个 irqaction->thread_flags 中。原注释依次表示：RUNTHREAD 通知
 * 中断线程应运行；WARNED 记住“返回 IRQ_WAKE_THREAD 却没有 thread_fn”的警告已经
 * 打印；AFFINITY 请求线程在自身上下文调整亲和性；FORCED_THREAD 说明 action 被
 * 强制线程化；READY 表示新线程已完成启动握手。
 *
 * handle.c 在硬中断分发侧设置 RUNTHREAD，manage.c 的 irq thread 通过原子 bitops
 * 领取并清除它，避免唤醒与线程检查之间丢事件。其他位也跨硬中断、管理线程和
 * 创建者共享，必须使用 set_bit/test_and_clear_bit 等原子位操作，不能普通赋值。
 */
enum {
	/* handler 已提交一次线程工作；线程清除此位后取得该次工作的处理权。 */
	IRQTF_RUNTHREAD,
	/* 缺少 thread_fn 的一次性诊断已经发出，防止每次中断重复刷屏。 */
	IRQTF_WARNED,
	/* 亲和性更新被委托给 irq thread，以避开不能睡眠的调用上下文。 */
	IRQTF_AFFINITY,
	/* 主 handler 由核心改造成线程处理，退出/同步路径需采用对应规则。 */
	IRQTF_FORCED_THREAD,
	/* irq thread 已进入等待循环，创建者可以安全完成注册发布。 */
	IRQTF_READY,
};

/*
 * Bit masks for desc->core_internal_state__do_not_mess_with_it
 *
 * IRQS_AUTODETECT		- autodetection in progress
 * IRQS_SPURIOUS_DISABLED	- was disabled due to spurious interrupt
 *				  detection
 * IRQS_POLL_INPROGRESS		- polling in progress
 * IRQS_ONESHOT			- irq is not unmasked in primary handler
 * IRQS_REPLAY			- irq has been resent and will not be resent
 * 				  again until the handler has run and cleared
 * 				  this flag.
 * IRQS_WAITING			- irq is waiting
 * IRQS_PENDING			- irq needs to be resent and should be resent
 * 				  at the next available opportunity.
 * IRQS_SUSPENDED		- irq is suspended
 * IRQS_NMI			- irq line is used to deliver NMIs
 * IRQS_SYSFS			- descriptor has been added to sysfs
 */
/*
 * 这些掩码保存在 desc->istate，原注释描述的是 IRQ 核心正在经历的瞬时阶段，而
 * settings.h 中的位描述长期策略：AUTODETECT 表示自动探测进行中；
 * SPURIOUS_DISABLED 表示因杂散检测而禁用；POLL_INPROGRESS 表示轮询正在执行；
 * ONESHOT 表示 primary handler 返回后不能立即解屏蔽；REPLAY 表示已提交一次重发，
 * 在 handler 清除此位前不得重复提交；WAITING/PENDING 分别表示等待探测事件和仍需
 * 找机会重发；SUSPENDED 表示电源管理已挂起；NMI 表示线路用于 NMI；SYSFS 表示
 * 描述符已经成功发布到 sysfs，删除时必须配对撤销。
 *
 * IRQS_TIMINGS 没有出现在上面的历史清单中；它为 IRQ 时间统计保留状态位。当前树
 * 只定义该位而没有直接使用者，不能据此推断计时功能已经启用。除明确使用原子
 * bitops 的协议外，这组字段通常在 desc->lock 下修改；引用只保内存存活，不冻结状态。
 */
enum {
	/* autoprobe 已把该描述符纳入一次探测事务。 */
	IRQS_AUTODETECT		= 0x00000001,
	/* spurious.c 的阈值策略已关闭线路，后续共享申请可触发恢复。 */
	IRQS_SPURIOUS_DISABLED	= 0x00000002,
	/* 当前描述符正被轮询，阻止递归进入同一轮询协议。 */
	IRQS_POLL_INPROGRESS	= 0x00000008,
	/* 至少一个 action 使用 oneshot，线程结束前保持线路屏蔽。 */
	IRQS_ONESHOT		= 0x00000020,
	/* 软件/硬件重发已经提交，等待实际 handler 运行后重新开放重发。 */
	IRQS_REPLAY		= 0x00000040,
	/* 自动探测正等待线路产生事件；事件到来会清除此位。 */
	IRQS_WAITING		= 0x00000080,
	/* 事件已到但当前不能处理，必须在下一个安全机会补做重发。 */
	IRQS_PENDING		= 0x00000200,
	/* suspend 路径已经把普通中断停住，resume 路径负责清除。 */
	IRQS_SUSPENDED		= 0x00000800,
	/* IRQ 时间统计的内部状态位；当前版本没有直接状态转换点。 */
	IRQS_TIMINGS		= 0x00001000,
	/* 描述符当前按 NMI 线路管理，普通 IRQ API 必须拒绝混用。 */
	IRQS_NMI		= 0x00002000,
	/* irqdesc.c 已为描述符创建 sysfs 表示，释放前需要 kobject_del。 */
	IRQS_SYSFS		= 0x00004000,
};

#include "debug.h"
#include "settings.h"

/*
 * __irq_set_trigger() - 通过 irqchip 设置触发类型并同步核心状态
 *
 * @desc: 输入输出的非 NULL 描述符借用指针；调用者持有 desc->lock，并在慢总线
 *        控制器需要时由更外层 bus lock 串行化。本函数不取得描述符引用。
 * @flags: IRQF_TRIGGER_* 或 IRQ_TYPE_* 候选位；函数只保留 IRQ_TYPE_SENSE_MASK。
 *
 * manage.c 的 action 安装/重配置路径调用它。函数可能调用 irq_mask、irq_set_type
 * 和 irq_unmask 回调，不能与同一线路的并发配置交错；不在此主动睡眠。返回 0 表示
 * 类型已记录，或控制器没有 set_type 而保持原配置；其他值原样传回 irqchip 错误。
 * 成功后 irq_data 与 settings 中的触发/电平派生位一致，失败时调用者仍拥有 desc。
 */
extern int __irq_set_trigger(struct irq_desc *desc, unsigned long flags);
/*
 * __disable_irq() - 增加嵌套禁用深度并在首次禁用时停止线路
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者必须持有 desc->lock，且已稳定生命期。
 *
 * disable_irq_nosync 等管理入口调用。无直接返回值、不等待正在运行的 handler，
 * 也不自行睡眠；depth 从 0 变 1 时才调用 irq_disable() 改软件/硬件状态，更深层
 * 禁用只累加计数。每次成功调用必须由后续 enable 配对，否则线路保持禁用。
 */
extern void __disable_irq(struct irq_desc *desc);
/*
 * __enable_irq() - 撤销一层嵌套禁用并在归零时启动 IRQ
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者持有 desc->lock，描述符不能为 NMI。
 *
 * enable_irq 管理路径调用。无直接返回值且不主动睡眠；depth 从 1 变 0 时通过
 * irq_startup() 首次启动或重新启用并处理待重发事件。depth 已为 0 或 IRQ 仍处于
 * suspend 时会 WARN 且不建立虚假的 enable 配对；对象所有权始终留在调用者。
 */
extern void __enable_irq(struct irq_desc *desc);

/*
 * 下面的布尔别名让 irq_startup() 调用点显式表达两个独立策略：是否检查并重发
 * pending IRQ，以及调用者是否以 enable/autoprobe 等“必须启动”的语义进入。
 * FORCE 不会绕过 managed IRQ 的在线 CPU 条件；条件不满足时仍中止，并额外 WARN
 * 暴露错误调用。它们不是状态位，不能组合保存，只作为函数实参表达控制流。
 */
#define IRQ_RESEND	true
#define IRQ_NORESEND	false

/* FORCE 标记无条件启动请求并对非法 managed 调用报警；COND 是常规条件启动。 */
#define IRQ_START_FORCE	true
#define IRQ_START_COND	false

/*
 * irq_activate() - 激活普通 IRQ domain 层级为硬件资源建立映射
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者已串行化描述符配置和生命周期。
 *
 * action 安装在真正 startup 前调用。非 managed IRQ 进入 irq_domain_activate_irq()，
 * 返回 0 或 domain/chip 的负错误码；managed IRQ 的激活延迟到具备目标 CPU 的启动
 * 阶段并在这里返回 0。函数不取得引用；回调必须遵守 IRQ 核心的原子上下文约束。
 */
extern int irq_activate(struct irq_desc *desc);
/*
 * irq_activate_and_startup() - 完成 domain 激活后以 FORCE 语义尝试启动线路
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者持有该 IRQ 的配置串行化条件。
 * @resend: true 要在启动后检查 IRQS_PENDING 并补发，false 跳过重发检查。
 *
 * 自动探测与级联 handler 建立路径使用。激活异常会 WARN 并返回 0，不继续 startup；
 * 否则返回 irq_startup() 的 irqchip 结果。对没有在线目标 CPU 的 managed IRQ，
 * FORCE 会触发 WARN，但仍可能返回 0 且保持 depth=1/MANAGED_SHUTDOWN；因此调用者要
 * 结合 IRQD_IRQ_STARTED 或描述符状态判断是否真正启动。函数不转移 desc 所有权。
 */
extern int irq_activate_and_startup(struct irq_desc *desc, bool resend);
/*
 * irq_startup() - 让已配置描述符进入 started/enabled 状态
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者持有 desc->lock 或等价独占条件。
 * @resend: true 在启动完成后调用 check_irq_resend() 补交待处理事件。
 * @force: true 表示 enable/autoprobe 等无条件启动请求；若 managed IRQ 没有在线
 *         目标仍会 WARN 并中止。false 是常规条件启动，等待 CPU hotplug 恢复。
 *
 * chip.c 根据首次/重复启动、managed 状态和控制器亲和性顺序调用 irqchip 回调。
 * 返回 0 或 irq_startup 回调的结果；managed 条件不满足也返回 0，但 depth 恢复为
 * 1 并设置 MANAGED_SHUTDOWN，不能把它误读成硬件已经启动。函数不睡眠、不持有引用。
 */
extern int irq_startup(struct irq_desc *desc, bool resend, bool force);
/*
 * irq_startup_managed() - 在目标 CPU 再次上线时恢复 managed IRQ
 *
 * @desc: 输入输出的非 NULL managed IRQ 描述符借用指针；CPU hotplug 路径已保证
 *        亲和性条件成立并串行化状态。
 *
 * 无直接返回值、不转移所有权。它先清 MANAGED_SHUTDOWN，再只撤销 hot-unplug
 * 增加的一层 depth；计数归零才条件启动并请求重发，避免用户先 disable 后经历
 * 热插拔却被意外重新启用。
 */
extern void irq_startup_managed(struct irq_desc *desc);

/*
 * irq_shutdown() - 停止已启动 IRQ 并保留可恢复的禁用深度
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者持有 desc->lock 或等价独占条件。
 *
 * CPU hotplug、释放和电源路径调用。无直接返回值、不等待 handler；若 IRQ 已 started，
 * 先取消重发、增加 depth，再调用 irq_shutdown 或强制 disable/mask，最终清 started。
 * 未启动时保持不变。增加的 depth 与 irq_startup_managed() 的恢复步骤严格配对。
 */
extern void irq_shutdown(struct irq_desc *desc);
/*
 * irq_shutdown_and_deactivate() - 停止线路并撤销 IRQ domain 激活
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者串行化该 IRQ 的配置和生命周期。
 *
 * action 最终释放时调用。无直接返回值、不转移引用；先执行 irq_shutdown()，再无
 * 条件调用 domain deactivate，因为资源可能在 request/startup 前已经激活。返回后
 * 描述符不再拥有该层激活资源，重新使用前必须再次 activate。
 */
extern void irq_shutdown_and_deactivate(struct irq_desc *desc);
/*
 * irq_disable() - 将 IRQ 标为 disabled，并按策略立即或延迟屏蔽硬件
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者持有 desc->lock。
 *
 * __disable_irq() 在 depth 首次从 0 增加时调用。无直接返回值、不等待 handler；
 * 有 irq_disable 回调时立即执行，否则默认只记软件 disabled，让后续到达的中断
 * 流处理器屏蔽并记 pending。IRQ_DISABLE_UNLAZY 可要求立刻 mask。
 */
extern void irq_disable(struct irq_desc *desc);
/*
 * irq_percpu_enable() - 在指定 CPU 上启用 per-CPU IRQ
 *
 * @desc: 输入输出的非 NULL per-CPU 描述符借用指针；调用者保证当前配置稳定。
 * @cpu: 要在 desc->percpu_enabled 中置位的有效 CPU 编号，必须对应本次硬件操作。
 *
 * 无直接返回值、不睡眠；优先调用 irq_enable，否则 irq_unmask，随后发布该 CPU
 * 已启用状态。函数不修改其他 CPU 位，也不取得 desc 引用。
 */
extern void irq_percpu_enable(struct irq_desc *desc, unsigned int cpu);
/*
 * irq_percpu_disable() - 在指定 CPU 上停止 per-CPU IRQ
 *
 * @desc: 输入输出的非 NULL per-CPU 描述符借用指针；调用者串行化对应 CPU 状态。
 * @cpu: 要从 desc->percpu_enabled 清除的有效 CPU 编号。
 *
 * 无直接返回值、不等待 handler；优先调用 irq_disable，否则 irq_mask，硬件停止后
 * 才清除软件位，使观察者不会在硬件仍开启时看到“已禁用”。
 */
extern void irq_percpu_disable(struct irq_desc *desc, unsigned int cpu);
/*
 * mask_irq() - 幂等地要求 irqchip 屏蔽线路并同步 IRQD_IRQ_MASKED
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者持有 desc->lock。
 *
 * 无直接返回值、不睡眠。已 masked 时快速返回；存在 irq_mask 回调才执行硬件屏蔽
 * 并在回调后置软件位。没有回调时保持原状态，调用者不能据此假设硬件一定可屏蔽。
 */
extern void mask_irq(struct irq_desc *desc);
/*
 * unmask_irq() - 幂等地解除 irqchip 屏蔽并同步 IRQD_IRQ_MASKED
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者持有 desc->lock。
 *
 * 无直接返回值、不等待事件。未 masked 时快速返回；存在 irq_unmask 回调时先操作
 * 硬件再清软件位，确保软件不会提前宣称线路开放。所有权和 disabled 状态不变。
 */
extern void unmask_irq(struct irq_desc *desc);
/*
 * unmask_threaded_irq() - 在线程化 handler 完成后按 irqchip 协议 EOI 并解屏蔽
 *
 * @desc: 输入输出的非 NULL oneshot/threaded IRQ 描述符借用指针；调用者在最后一个
 *        活跃线程退出的串行化路径中持有 desc->lock。
 *
 * 无直接返回值、不睡眠。IRQCHIP_EOI_THREADED 要求先在线程完成点调用 irq_eoi，
 * 然后统一进入 unmask_irq()；顺序反转可能在 EOI 前重新接收同一电平事件。
 */
extern void unmask_threaded_irq(struct irq_desc *desc);

#ifdef CONFIG_SPARSE_IRQ
/*
 * irq_mark_irq() - 稀疏模式下的已分配标记空操作
 *
 * @irq: 调用者正在登记的逻辑 IRQ 号；该值在本配置中不被消费。
 *
 * 稀疏描述符在插入 Maple Tree 时已经完成“已分配”发布，所以兼容调用点无需重复
 * 标记。函数无返回值、不睡眠、无副作用；空实现保持两种配置的调用接口一致。
 */
static __always_inline void irq_mark_irq(unsigned int irq) { }
/*
 * irq_desc_free_rcu() - 在最后一个 rcuref 消失后安排描述符延迟释放
 *
 * @desc: 所有权已由最后一次 irq_desc_put_ref() 交出的动态描述符；调用者此后不得
 *        再解引用，函数最终经 RCU 回调丢弃 kobject 初始引用。
 *
 * 无直接返回值、不等待宽限期、不能睡眠。RCU 让既有查找者安全退出；sysfs 等
 * kobject 引用仍可把最终内存析构推迟得更久。
 */
void irq_desc_free_rcu(struct irq_desc *desc);

/*
 * irq_desc_get_ref() - 尝试为动态 irq_desc 取得跨 RCU 临界区的稳定引用
 *
 * @desc: RCU 或其他有效生命周期保护下的非 NULL 借用指针。
 *
 * proc.c 在准备离开 RCU 读侧前调用。返回 true 表示 rcuref 成功增加，调用者获得
 * 一份必须 irq_desc_put_ref() 的持有引用；false 表示对象已进入死亡状态，不能在
 * 保护范围外继续使用。函数不睡眠，也不保证 desc 字段内容不并发变化。
 */
static __always_inline bool irq_desc_get_ref(struct irq_desc *desc)
{
	return rcuref_get(&desc->refcnt);
}

/*
 * irq_desc_put_ref() - 释放一份动态描述符持有引用并触发最终 RCU 回收
 *
 * @desc: 调用者持有的非 NULL irq_desc；本次调用消费恰好一份 rcuref。
 *
 * 无直接返回值、不睡眠。rcuref_put() 判定这是最后一份引用时，所有权转交给
 * irq_desc_free_rcu()；否则对象继续存活。调用后无论是否为最后引用，调用者都
 * 不能再凭被释放的那份引用访问 desc。
 */
static __always_inline void irq_desc_put_ref(struct irq_desc *desc)
{
	if (rcuref_put(&desc->refcnt))
		irq_desc_free_rcu(desc);
}
#else
/*
 * irq_mark_irq() - 把静态 irq_desc 数组中的逻辑号发布为已分配
 *
 * @irq: 小于 NR_IRQS 的有效逻辑号；静态配置下对应槽永久存在。
 *
 * irqdesc.c 的实现可睡眠获取 sparse_irq_lock（两种配置共用该锁名），再把槽加入
 * 已分配索引。无直接返回值，不取得调用者可释放的引用。
 */
extern void irq_mark_irq(unsigned int irq);
/*
 * irq_desc_get_ref() - 静态描述符配置下确认永久对象可继续使用
 *
 * @desc: 非 NULL 静态 irq_desc 借用指针；参数仅用于保持统一接口。
 *
 * 静态数组不会按单个 IRQ 释放，因此总返回 true、不增加计数、不睡眠；调用者仍
 * 需要锁来保证字段一致性，永久存储期不等于状态不可变。
 */
static __always_inline bool irq_desc_get_ref(struct irq_desc *desc) { return true; }
/*
 * irq_desc_put_ref() - 静态描述符配置下的引用释放空操作
 *
 * @desc: 与 get 接口配对的静态描述符借用指针；不会被消费或释放。
 *
 * 无返回值、不睡眠、无副作用，因为静态数组由内核映像永久拥有。保留调用可让
 * proc 等用户不必为两种配置维护不同控制流。
 */
static __always_inline void irq_desc_put_ref(struct irq_desc *desc) { }
#endif

/*
 * __handle_irq_event_percpu() - 顺序调用描述符上的全部 primary action handler
 *
 * @desc: 非 NULL 借用描述符；当前在 hardirq 且本地中断关闭。普通流处理器已置
 *        IRQD_IRQ_INPROGRESS 并暂释 desc->lock；
 *        不需全局串行的 per-CPU 流可直接进入，依靠注册期保证 action 链稳定。
 *
 * handle.c 遍历稳定的 action 链，执行 trace、可选耗时检查，并把 IRQ_WAKE_THREAD
 * 转成 __irq_wake_thread()。返回所有 handler irqreturn_t 的按位合并结果；不直接
 * 做随机性或杂散判定。handler 可改变设备状态，但不得破坏本地 IRQ 关闭约束。
 */
irqreturn_t __handle_irq_event_percpu(struct irq_desc *desc);
/*
 * handle_irq_event_percpu() - 完成 action 分发后的每 CPU 收尾工作
 *
 * @desc: 与 __handle_irq_event_percpu() 相同的非 NULL 借用描述符和中断上下文；
 *        普通/每 CPU 调用者分别提供 in-progress 或 per-CPU 生命周期保证。
 *
 * 调用底层分发后加入中断随机性，并在未设置 NO_DEBUG 时执行 note_interrupt()
 * 杂散检测。返回原样的合并 irqreturn_t；不获取 desc->lock、不睡眠，也不延长生命期。
 */
irqreturn_t handle_irq_event_percpu(struct irq_desc *desc);
/*
 * handle_irq_event() - 在 desc 锁协议内发布 in-progress 并运行设备 action
 *
 * @desc: 输入输出的非 NULL 借用描述符；入口必须持有 desc->lock 且本地 IRQ 关闭。
 *
 * 流处理器调用。它清 pending、置 IRQD_IRQ_INPROGRESS，释放锁运行 handler，再
 * 重新取得同一锁并清 in-progress；成功与所有出口都保持“返回时锁仍持有”。返回
 * action 合并结果。锁外窗口允许 handler/线程唤醒，但生命周期由注册协议稳定。
 */
irqreturn_t handle_irq_event(struct irq_desc *desc);

/* Resending of interrupts :*/
/*
 * 原注释表示以下接口负责中断重发：当边沿事件在禁用/屏蔽窗口到达时，核心先记
 * IRQS_PENDING，之后优先请求 irqchip 硬件 retrigger；硬件不支持时可把 desc 放入
 * tasklet 软件重发队列。电平 IRQ 由仍然有效的电平让硬件自然再次触发，不走此机制。
 */
/*
 * check_irq_resend() - 在安全时机领取 pending 事件并提交一次硬件或软件重发
 *
 * @desc: 输入输出的非 NULL 借用描述符；入口本地 IRQ 关闭并持有 desc->lock。
 * @inject: true 即使没有 IRQS_PENDING 也尝试注入，false 只处理真实待重发事件。
 *
 * 返回 0 表示无需动作或重发已成功提交；-EINVAL 表示电平/不可软件重发等限制，
 * -EBUSY 表示已有 IRQS_REPLAY，其他错误来自重发路径。提交成功置 REPLAY；失败时
 * pending 已被领取清除。函数不等待 handler，软件路径只把工作转交 tasklet。
 */
int check_irq_resend(struct irq_desc *desc, bool inject);
/*
 * clear_irq_resend() - 从软件重发队列摘除描述符
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者保证对象尚未释放。
 *
 * shutdown 在清 started 前调用。无直接返回值；启用软件重发时内部取得
 * irq_resend_lock 并幂等删除节点，关闭配置时为空操作。返回后 tasklet 不能再从
 * 队列取得该节点，但已开始的 handler 仍由上层同步协议处理。
 */
void clear_irq_resend(struct irq_desc *desc);
/*
 * irq_resend_init() - 初始化新描述符的软件重发链表节点
 *
 * @desc: 输入输出的非 NULL、尚未发布描述符借用指针。
 *
 * irqdesc.c 在构造阶段调用。无返回值、不睡眠；软件重发配置下把 resend_node
 * 初始化为空闲未入链状态，关闭配置时为空操作。函数不激活重发或取得引用。
 */
void irq_resend_init(struct irq_desc *desc);
/*
 * __irq_wake_thread() - 把一次 IRQ_WAKE_THREAD 提交给 action 的 irq thread
 *
 * @desc: 输入输出的非 NULL 描述符借用指针；hardirq 分发期间对象和 action 链稳定。
 * @action: 输入输出的非 NULL action 借用指针，必须已有 thread/thread_fn。
 *
 * 不睡眠、无返回值。线程正在退出或 RUNTHREAD 已置位时幂等返回；否则原子置位、
 * 合并 oneshot mask、增加 threads_active 并唤醒线程。线程完成后负责减少 active
 * 并可能唤醒 synchronize_irq()；这些状态共同防止 oneshot 过早解屏蔽。
 */
void __irq_wake_thread(struct irq_desc *desc, struct irqaction *action);

/*
 * wake_threads_waitq() - 结束一次活跃 irq thread 记账并唤醒归零等待者
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者恰好配对一次此前的 threads_active
 *        增量，通常位于线程 handler 返回或异常退出路径。
 *
 * 无直接返回值、不可重复消费同一次增量。atomic_dec_and_test() 使最后退出者唯一
 * 唤醒 wait_for_threads，供 synchronize_irq/free_irq 继续；非最后线程只减计数。
 */
void wake_threads_waitq(struct irq_desc *desc);

#ifdef CONFIG_PROC_FS
/*
 * register_irq_proc() - 按需创建 /proc/irq/<irq>/ 描述符目录及属性
 *
 * @irq: 要格式化为目录名的逻辑 IRQ 号，必须与 @desc->irq_data.irq 一致。
 * @desc: 输入输出的非 NULL 借用描述符；函数成功时写 desc->dir，不取得引用。
 *
 * action 安装完成后调用，可睡眠获取内部 mutex 并分配 proc 项。proc 根未建立、
 * 使用 no_irq_chip、目录已存在或分配失败时静默返回；无直接返回值。内部 mutex
 * 防多个并发 request 者重复创建，同一目录的最终撤销由 unregister_irq_proc() 配对。
 */
extern void register_irq_proc(unsigned int irq, struct irq_desc *desc);
/*
 * unregister_irq_proc() - 删除描述符级 /proc/irq/<irq>/ 目录树
 *
 * @irq: 与 @desc 对应的逻辑号，用于定位父目录中的名称。
 * @desc: 输入输出的非 NULL 借用描述符；其 proc 目录可能尚未创建。
 *
 * irqdesc 最终摘除时调用，可能进入 procfs 删除路径；无直接返回值。根或 desc->dir
 * 不存在时幂等返回，否则先删属性再删 IRQ 目录。调用者负责先阻止新 action 注册
 * 并稳定 desc 生命周期；函数不释放 desc 本身。
 */
extern void unregister_irq_proc(unsigned int irq, struct irq_desc *desc);
/*
 * register_handler_proc() - 为一个具名 irqaction 创建 handler 子目录
 *
 * @irq: action 所属逻辑 IRQ 号，用来查找对应 desc。
 * @action: 输入输出的非 NULL action 借用指针；成功时写 action->dir。
 *
 * request 路径在 IRQ 目录就绪后调用，可能分配 proc 对象。缺目录、名称为空、已有
 * 目录或同一 IRQ 下名称重复时静默返回；无直接返回值。目录不持有可替代 action
 * 注册协议的所有权，free 路径必须先 unregister_handler_proc()。
 */
extern void register_handler_proc(unsigned int irq, struct irqaction *action);
/*
 * unregister_handler_proc() - 删除一个 action 的 proc handler 目录
 *
 * @irq: action 所属逻辑号；当前实现不消费该参数，但接口与注册路径保持对称。
 * @action: 输入输出的非 NULL 借用 action；action->dir 可为 NULL，proc_remove 幂等处理。
 *
 * free_irq 在释放 action 内存前调用。无直接返回值，可能进入 procfs 清理；返回后
 * 用户不能再通过该目录发起新访问，但 action 的最终同步/释放仍由 manage.c 完成。
 */
extern void unregister_handler_proc(unsigned int irq, struct irqaction *action);
/*
 * irq_proc_update_valid() - 重新计算描述符能否出现在 /proc/interrupts
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者在 desc->lock 下更新 action、hidden
 *        或 chained 状态后调用。
 *
 * 无返回值、不睡眠。普通且可见并至少有一个 action 时置 PROC_VALID，否则清除；
 * 该资格只是 RCU 枚举的快速筛选，读者仍须 irq_desc_get_ref() 稳定对象生命周期。
 */
void irq_proc_update_valid(struct irq_desc *desc);
#else
/*
 * register_irq_proc() - 未启用 procfs 时保留调用点的空实现
 *
 * @irq: 未消费的逻辑号。
 * @desc: 未消费的描述符借用指针。
 * 返回：无直接返回值；不睡眠、不分配目录、不改变 desc 或所有权。
 */
static inline void register_irq_proc(unsigned int irq, struct irq_desc *desc) { }
/*
 * unregister_irq_proc() - 未启用 procfs 时的描述符目录删除空实现
 *
 * @irq: 未消费的逻辑号。
 * @desc: 未消费的描述符借用指针。
 * 返回：无直接返回值；没有 proc 对象需要撤销，其他释放步骤仍由调用者执行。
 */
static inline void unregister_irq_proc(unsigned int irq, struct irq_desc *desc) { }
/*
 * register_handler_proc() - 未启用 procfs 时的 action 目录注册空实现
 *
 * @irq: 未消费的逻辑号。
 * @action: 未消费的 action 借用指针。
 * 返回：无直接返回值；不睡眠、不写 action->dir，也不改变 action 生命周期。
 */
static inline void register_handler_proc(unsigned int irq,
					 struct irqaction *action) { }
/*
 * unregister_handler_proc() - 未启用 procfs 时的 action 目录删除空实现
 *
 * @irq: 未消费的逻辑号。
 * @action: 未消费的 action 借用指针。
 * 返回：无直接返回值且无副作用，action 仍由 free IRQ 路径正常回收。
 */
static inline void unregister_handler_proc(unsigned int irq,
					   struct irqaction *action) { }
/*
 * irq_proc_update_valid() - 未启用 procfs 时省略输出资格维护
 *
 * @desc: 未消费的描述符借用指针。
 * 返回：无直接返回值；不设置 PROC_VALID，不影响 IRQ 的处理、申请或释放语义。
 */
static inline void irq_proc_update_valid(struct irq_desc *desc) { }
#endif

/*
 * irq_find_desc_at_or_after() - 在 RCU 下查找编号不小于起点的首个描述符
 *
 * @offset: 包含式逻辑 IRQ 搜索起点，允许等于当前空洞中的号码。
 *
 * proc/迭代路径必须已进入 RCU 读侧。返回受该临界区保护但未增加 rcuref 的借用
 * desc，或没有匹配时返回 NULL；退出 RCU 后继续使用前必须另取稳定引用。函数
 * 不睡眠、不修改 Maple Tree，搜索上界为 total_nr_irqs。
 */
struct irq_desc *irq_find_desc_at_or_after(unsigned int offset);

/*
 * irq_can_set_affinity_usr() - 判断用户空间是否有权修改某 IRQ 的亲和性
 *
 * @irq: 待检查的逻辑 IRQ 号。
 *
 * SMP proc/sysfs 写路径调用。返回 true 要求 desc 存在、可均衡、chip 提供
 * irq_set_affinity，且不是核心管理的 managed affinity；否则 false。函数不改变
 * 掩码、不取得引用、不睡眠，结果是资格快照，实际设置仍需锁内再次校验。
 */
extern bool irq_can_set_affinity_usr(unsigned int irq);

/*
 * irq_do_set_affinity() - 过滤目标 CPU 后调用 irqchip 编程亲和性
 *
 * @data: 输入输出的非 NULL irq_data 借用指针；其 desc 和 chip 生命周期已稳定。
 * @dest: 只读、非 NULL cpumask 借用指针，表示调用者期望保存的目标集合。
 * @force: false 只向 chip 提交在线 CPU；true 按原始 @dest 强制提交。
 *
 * 调用者持有 desc->lock，当前 CPU 的临时掩码只在函数内有效。返回 0 表示 chip
 * 接受并已同步保存 affinity/通知线程；-EINVAL 表示缺回调或无可用在线目标，
 * 其他值来自 chip。managed housekeeping 可能缩窄实际编程集合，但保存期望掩码。
 */
extern int irq_do_set_affinity(struct irq_data *data,
			       const struct cpumask *dest, bool force);
/*
 * irq_affinity_schedule_notify_work() - 为一次已完成的亲和性变化排队通知
 *
 * @desc: 输入输出的非 NULL 借用描述符；入口必须持有 desc->lock，且
 *        desc->affinity_notify 已安装并由其 kref 管理。
 *
 * 无直接返回值、不在锁内执行通知回调。函数先为待执行 work 取得 kref；若 work
 * 已排队则立即归还多取的引用。成功排队后 worker 消费该引用并在进程上下文通知。
 */
extern void irq_affinity_schedule_notify_work(struct irq_desc *desc);

#ifdef CONFIG_SMP
/*
 * irq_setup_affinity() - 为启动中的 IRQ 选择并编程初始 SMP 亲和性
 *
 * @desc: 输入输出的非 NULL 描述符借用指针；调用者在 startup 的锁内配置阶段。
 *
 * 通用实现综合默认、用户/managed 掩码、在线 CPU 和 NUMA 节点，再调用
 * irq_do_set_affinity()；架构可用 CONFIG_AUTO_IRQ_AFFINITY 替换选择器。返回 0 或
 * 底层设置错误，不转移掩码/desc 所有权，irqchip 回调不得睡眠。
 */
extern int irq_setup_affinity(struct irq_desc *desc);
#else
/*
 * irq_setup_affinity() - UP 配置下省略无意义的 CPU 亲和性设置
 *
 * @desc: 未消费的描述符借用指针。返回：恒为 0，表示无需配置；不睡眠、无副作用，
 * IRQ 的启动状态仍由调用者随后建立。
 */
static inline int irq_setup_affinity(struct irq_desc *desc) { return 0; }
#endif

/*
 * for_each_action_of_desc() 按 desc->action 单链表顺序把每个 irqaction 借用指针
 * 赋给 @act。调用者必须用 desc->lock、IRQD_IRQ_INPROGRESS/注册协议或其他条件保证
 * 链表在遍历期间不会释放；宏不取得 action 引用，循环结束后 @act 为 NULL。
 */
#define for_each_action_of_desc(desc, act)			\
	for (act = desc->action; act; act = act->next)

/* Inline functions for support of irq chips on slow busses */
/*
 * 原注释表示以下内联函数支持挂在慢速总线上的 irqchip。普通 raw spinlock 只能
 * 保护内存状态，不能包围可能睡眠的 I2C/SPI 寄存器访问；因此上层先取得可睡眠的
 * chip bus lock，再取得 desc->lock，退出时反向释放并同步硬件写入。
 */
/*
 * chip_bus_lock() - 在描述符 raw lock 之前取得可选的 irqchip 总线锁
 *
 * @desc: 非 NULL 借用描述符；irq_data.chip 已稳定，调用者尚未持有 desc->lock。
 *
 * 若 irq_bus_lock 存在则间接调用，否则快速空操作。无直接返回值；回调允许睡眠，
 * 因而使用 bus=true 的上层接口只能从可睡眠上下文进入。锁 ownership 交给调用者，
 * 必须由 chip_bus_sync_unlock() 精确配对。
 */
static inline void chip_bus_lock(struct irq_desc *desc)
{
	if (unlikely(desc->irq_data.chip->irq_bus_lock))
		desc->irq_data.chip->irq_bus_lock(&desc->irq_data);
}

/*
 * chip_bus_sync_unlock() - 同步慢总线状态并释放可选 irqchip 总线锁
 *
 * @desc: 与 chip_bus_lock() 相同的非 NULL 借用描述符；调用者已释放 desc->lock，
 *        但仍持有 chip bus lock（若控制器实现了该协议）。
 *
 * 有 irq_bus_sync_unlock 时调用它刷新缓存寄存器并解锁，否则为空操作。无直接
 * 返回值，回调可能睡眠；返回后调用者不再拥有总线锁，desc 所有权不变。
 */
static inline void chip_bus_sync_unlock(struct irq_desc *desc)
{
	if (unlikely(desc->irq_data.chip->irq_bus_sync_unlock))
		desc->irq_data.chip->irq_bus_sync_unlock(&desc->irq_data);
}

/*
 * 描述符获取校验位：CHECK 启用类型检查；PERCPU 指明调用者要求的是 per-CPU devid
 * 描述符。GLOBAL 组合拒绝 per-CPU devid，PERCPU 组合反过来拒绝普通描述符，防止
 * 两套 request/enable API 操作错误的 action 与 dev_id 模型。
 */
#define _IRQ_DESC_CHECK		(1 << 0)
#define _IRQ_DESC_PERCPU	(1 << 1)

#define IRQ_GET_DESC_CHECK_GLOBAL	(_IRQ_DESC_CHECK)
#define IRQ_GET_DESC_CHECK_PERCPU	(_IRQ_DESC_CHECK | _IRQ_DESC_PERCPU)

/*
 * __irq_get_desc_lock() - 查找、按调用模型校验并锁定 IRQ 描述符
 *
 * @irq: 待查找的逻辑 IRQ 号。
 * @flags: 非 NULL 输出参数；成功时保存 raw_spin_lock_irqsave 前的本地 IRQ 状态。
 * @bus: true 先取得可能睡眠的 chip bus lock，false 只取 desc raw lock。
 * @check: 0 跳过类型检查，或使用 IRQ_GET_DESC_CHECK_GLOBAL/PERCPU。
 *
 * 找不到/类型不匹配返回 NULL，未持任何锁且 @flags 不可用；成功返回锁内借用 desc，
 * 本地 IRQ 已关闭，必须以相同 @bus 和输出 flags 调 __irq_put_desc_unlock()。bus=true
 * 可能睡眠；成功后的 raw lock 临界区不可睡眠，函数不增加 desc 引用。
 */
struct irq_desc *__irq_get_desc_lock(unsigned int irq, unsigned long *flags, bool bus,
				     unsigned int check);
/*
 * __irq_put_desc_unlock() - 释放描述符锁并恢复获取前的 IRQ/总线状态
 *
 * @desc: __irq_get_desc_lock() 成功返回的非 NULL 锁内借用对象。
 * @flags: 对应获取调用写出的本地 IRQ 状态，不得来自其他临界区。
 * @bus: 必须与获取时一致；true 在 raw unlock 后同步并释放 chip bus lock。
 *
 * 无直接返回值。返回后不再持有任何相关锁，不能继续假设 desc 字段稳定；先释放
 * raw lock 再执行可能睡眠的 bus unlock，与获取顺序严格反向配对。
 */
void __irq_put_desc_unlock(struct irq_desc *desc, unsigned long flags, bool bus);

/*
 * cleanup class 把“可能获取失败的 desc 锁”包装成作用域资源：class 对象保存 lock、
 * irqsave flags 和 bus 策略；离开作用域时仅在 lock 非 NULL 时自动调用 put/unlock。
 * 因此函数内的 return/goto 都不会漏解锁，失败构造则让 scoped_guard 直接跳过主体。
 */
__DEFINE_CLASS_IS_CONDITIONAL(irqdesc_lock, true);
__DEFINE_UNLOCK_GUARD(irqdesc_lock, struct irq_desc,
		      if (_T->lock) __irq_put_desc_unlock(_T->lock, _T->flags, _T->bus),
		      unsigned long flags; bool bus);

/*
 * class_irqdesc_lock_constructor() - 构造自动清理的描述符锁 guard
 *
 * @irq: 传给底层查找的逻辑 IRQ 号。
 * @bus: 是否把可睡眠 chip bus lock 纳入 guard 生命周期。
 * @check: 描述符类型校验组合，语义同 __irq_get_desc_lock()。
 *
 * 返回按值 class 对象：lock 非 NULL 表示已持有 desc/raw IRQ 状态（及可选 bus lock），
 * flags/bus 由析构使用；lock 为 NULL 表示获取失败，未持资源。构造本身不另取引用，
 * bus=true 时可能睡眠；成功对象的所有权转交当前 C 作用域自动清理。
 */
static inline class_irqdesc_lock_t class_irqdesc_lock_constructor(unsigned int irq, bool bus,
								  unsigned int check)
{
	/* 先保存 bus 策略；flags 只会在底层成功获取时被初始化和消费。 */
	class_irqdesc_lock_t _t = { .bus = bus, };

	_t.lock = __irq_get_desc_lock(irq, &_t.flags, bus, check);

	return _t;
}

/*
 * 两个 scoped 宏分别选择普通 desc lock 和包含慢总线锁的版本；@_check 决定普通/
 * per-CPU API 校验。宏创建名为 scope 的 guard，获取失败时不执行随后的作用域主体。
 */
#define scoped_irqdesc_get_and_lock(_irq, _check)		\
	scoped_guard(irqdesc_lock, _irq, false, _check)

#define scoped_irqdesc_get_and_buslock(_irq, _check)		\
	scoped_guard(irqdesc_lock, _irq, true, _check)

/* 在 scoped_guard 主体内取出已锁定 desc 的借用指针；不可逃逸出该词法作用域。 */
#define scoped_irqdesc		((struct irq_desc *)(__guard_ptr(irqdesc_lock)(&scope)))

/*
 * irq_common_data 故意把 state_use_accessors 设为私有成员；这个临时宏借
 * ACCESS_PRIVATE() 只向 IRQ 核心内部暴露可赋值的左值。后面的 #undef 缩短绕过
 * 公共 irqd_* 查询 API 的范围，普通驱动不能直接依赖内部布局。
 */
#define __irqd_to_state(d) ACCESS_PRIVATE((d)->common, state_use_accessors)

/*
 * irqd_get() - 取得 irq_data 公共状态位的当前快照
 *
 * @d: 只读借用的非 NULL irq_data；调用者负责 data/desc 生命周期和并发稳定性。
 *
 * 返回完整 IRQD_* 位图，不取得引用、不睡眠、无副作用。普通读取不是锁或原子
 * 快照协议；需要与并发修改一致时，调用者必须持有对应 desc->lock。
 */
static inline unsigned int irqd_get(struct irq_data *d)
{
	return __irqd_to_state(d);
}

/*
 * Manipulation functions for irq_data.state
 */
/*
 * 原注释表示以下函数集中修改 irq_data.state。它们都只执行内存位操作，不调用
 * irqchip、不提供原子同步；通常由持有 desc->lock 的 chip/manage/migration 路径
 * 使用，使软件状态与随后或此前完成的硬件操作保持规定顺序。
 */
/*
 * irqd_set_move_pending() - 标记一次亲和性迁移等待安全完成
 *
 * @d: 输入输出的非 NULL irq_data 借用指针；调用者串行化亲和性状态。
 * 返回：无直接返回值、不睡眠；置 IRQD_SETAFFINITY_PENDING，目标掩码由 desc 的
 * pending_mask 另行保存，本函数不编程 irqchip。
 */
static inline void irqd_set_move_pending(struct irq_data *d)
{
	__irqd_to_state(d) |= IRQD_SETAFFINITY_PENDING;
}

/*
 * irqd_clr_move_pending() - 宣告待处理亲和性迁移已完成或取消
 *
 * @d: 输入输出的非 NULL irq_data 借用指针；调用者已处理/丢弃 pending_mask。
 * 返回：无直接返回值、不睡眠；只清 IRQD_SETAFFINITY_PENDING，不修改实际 affinity。
 */
static inline void irqd_clr_move_pending(struct irq_data *d)
{
	__irqd_to_state(d) &= ~IRQD_SETAFFINITY_PENDING;
}

/*
 * irqd_set_managed_shutdown() - 记录 managed IRQ 因无在线目标 CPU 而暂缓启动
 *
 * @d: 输入输出的非 NULL managed irq_data 借用指针；调用者持有 desc->lock。
 * 返回：无直接返回值、不睡眠；只发布 IRQD_MANAGED_SHUTDOWN，CPU hotplug 路径
 * 之后据此恢复，不等价于普通用户 disable。
 */
static inline void irqd_set_managed_shutdown(struct irq_data *d)
{
	__irqd_to_state(d) |= IRQD_MANAGED_SHUTDOWN;
}

/*
 * irqd_clr_managed_shutdown() - 领取并清除 managed IRQ 的热插拔恢复状态
 *
 * @d: 输入输出的非 NULL managed irq_data 借用指针；调用者已确认目标 CPU 可用。
 * 返回：无直接返回值、不睡眠；清位后由 irq_startup_managed() 配对调整 depth/启动。
 */
static inline void irqd_clr_managed_shutdown(struct irq_data *d)
{
	__irqd_to_state(d) &= ~IRQD_MANAGED_SHUTDOWN;
}

/*
 * irqd_clear() - 批量清除 irq_data 的内部状态位
 *
 * @d: 输入输出的非 NULL irq_data 借用指针。
 * @mask: 要清除的 IRQD_* 位集合。
 * 返回：无直接返回值、不睡眠；mask 外的位保持不变。调用者负责锁和清位前所需的
 * 硬件/生命周期动作，错误 mask 可能破坏软件与控制器状态对应关系。
 */
static inline void irqd_clear(struct irq_data *d, unsigned int mask)
{
	__irqd_to_state(d) &= ~mask;
}

/*
 * irqd_set() - 批量置位 irq_data 的内部状态
 *
 * @d: 输入输出的非 NULL irq_data 借用指针。
 * @mask: 要置上的 IRQD_* 位集合。
 * 返回：无直接返回值、不睡眠；不自动执行位所代表的硬件操作，调用顺序和锁由
 * 上层协议保证。
 */
static inline void irqd_set(struct irq_data *d, unsigned int mask)
{
	__irqd_to_state(d) |= mask;
}

/*
 * irqd_has_set() - 判断候选状态集合中是否至少有一位已设置
 *
 * @d: 只读借用的非 NULL irq_data。
 * @mask: 待测试的 IRQD_* 位集合。
 * 返回 true 表示交集非空、false 表示全部未置；不是“所有 mask 位都已设置”的测试。
 * 函数不睡眠、无副作用，并发一致性仍由外层 desc->lock 保证。
 */
static inline bool irqd_has_set(struct irq_data *d, unsigned int mask)
{
	return __irqd_to_state(d) & mask;
}

/*
 * irq_state_set_disabled() - 在主 irq_data 上发布软件 disabled 状态
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者已持锁并按 irqchip 协议停止线路。
 * 返回：无直接返回值、不睡眠；只置 IRQD_IRQ_DISABLED，不修改 depth 或 masked 位。
 */
static inline void irq_state_set_disabled(struct irq_desc *desc)
{
	irqd_set(&desc->irq_data, IRQD_IRQ_DISABLED);
}

/*
 * irq_state_set_masked() - 在主 irq_data 上记录硬件线路已屏蔽
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者已完成 irq_mask/disable 类硬件动作。
 * 返回：无直接返回值、不睡眠；只置 IRQD_IRQ_MASKED，不能替代实际 irqchip 回调。
 */
static inline void irq_state_set_masked(struct irq_desc *desc)
{
	irqd_set(&desc->irq_data, IRQD_IRQ_MASKED);
}

#undef __irqd_to_state

/*
 * __kstat_incr_irqs_this_cpu() - 增加当前 CPU 的 IRQ 计数和全局每 CPU 汇总
 *
 * @desc: 输入输出的非 NULL 借用描述符；kstat_irqs 已分配，调用者处于 hardirq/NMI
 *        等当前 CPU 不会迁移的上下文。
 *
 * 无直接返回值、不睡眠。只修改当前 CPU 的 desc->kstat_irqs->cnt 与 kstat.irqs_sum，
 * 不触碰共享 tot_count，因而可用于不以 desc->lock 串行的 per-CPU/NMI 流处理器。
 */
static inline void __kstat_incr_irqs_this_cpu(struct irq_desc *desc)
{
	__this_cpu_inc(desc->kstat_irqs->cnt);
	__this_cpu_inc(kstat.irqs_sum);
}

/*
 * kstat_incr_irqs_this_cpu() - 为普通串行 IRQ 同时维护 per-CPU 和快速总计
 *
 * @desc: 输入输出的非 NULL 普通 IRQ 描述符借用指针；调用者的流处理器通过
 *        desc->lock 串行化 tot_count，并处于不可迁移的中断上下文。
 *
 * 无直接返回值、不睡眠。先更新当前 CPU/全局每 CPU 统计，再增加用于普通非 NMI、
 * 非 per-CPU IRQ 快速查询的 tot_count；per-CPU handler 必须调用双下划线版本。
 */
static inline void kstat_incr_irqs_this_cpu(struct irq_desc *desc)
{
	__kstat_incr_irqs_this_cpu(desc);
	desc->tot_count++;
}

/*
 * irq_desc_get_node() - 查询描述符关联资源的 NUMA 节点
 *
 * @desc: 只读借用的非 NULL 描述符；调用者保证对象存活。
 *
 * 返回 irq_common_data 的节点编号，NUMA_NO_NODE 表示没有偏好；不睡眠、无副作用、
 * 不取得节点或描述符引用。分配/亲和性选择把结果作为提示而非硬性在线保证。
 */
static inline int irq_desc_get_node(struct irq_desc *desc)
{
	return irq_common_data_get_node(&desc->irq_common_data);
}

/*
 * irq_desc_is_chained() - 通过哨兵 action 判断描述符是否为级联父 IRQ
 *
 * @desc: 只读借用的非 NULL 描述符；调用者须用 desc->lock 或注册协议稳定 action。
 *
 * 返回非零仅当 action 存在且指针恰为全局 chained_action；否则返回 0。函数不睡眠、
 * 无副作用。指针身份而非 action 内容决定类型，普通驱动不得伪造该哨兵。
 */
static inline int irq_desc_is_chained(struct irq_desc *desc)
{
	return (desc->action && desc->action == &chained_action);
}

/*
 * irq_is_nmi() - 查询描述符当前是否按 NMI 线路管理
 *
 * @desc: 只读借用的非 NULL 描述符；管理路径通常在 desc->lock 下读取 IRQS_NMI。
 *
 * 返回 true 表示普通 IRQ request/free/affinity 等接口必须采用 NMI 特例或拒绝，
 * false 表示普通模型；不睡眠、无副作用，存储期引用本身不保证该状态不变化。
 */
static inline bool irq_is_nmi(struct irq_desc *desc)
{
	return desc->istate & IRQS_NMI;
}

#ifdef CONFIG_PM_SLEEP
/*
 * irq_pm_handle_wakeup() - 把挂起期间到达的唤醒 IRQ 转成系统唤醒事件
 *
 * @desc: 输入输出的非 NULL、已 armed 唤醒描述符借用指针；流处理器持 desc->lock。
 *
 * 无直接返回值、不睡眠。它清 WAKEUP_ARMED，设置 SUSPENDED|PENDING、增加 depth 并
 * 禁用线路，最后通知 PM 核心；pending 让 resume 后补处理原事件。所有权保持不变，
 * depth 增量必须由恢复路径配对，防止唤醒风暴反复进入。
 */
void irq_pm_handle_wakeup(struct irq_desc *desc);
/*
 * irq_pm_install_action() - 把新 action 的 suspend 策略计入描述符聚合状态
 *
 * @desc: 输入输出的非 NULL 描述符；__setup_irq() 已把 @action 接入链并持 desc->lock。
 * @action: 只读借用的新 action；函数读取 FORCE_RESUME/NO_SUSPEND/COND_SUSPEND flags。
 *
 * 无直接返回值、不睡眠；增加 nr_actions 及对应深度，并 WARN 不兼容的共享组合。
 * 它不修改 action 所有权，失败诊断也不回滚安装，策略一致性由上层请求规则保证。
 */
void irq_pm_install_action(struct irq_desc *desc, struct irqaction *action);
/*
 * irq_pm_remove_action() - 从描述符聚合 suspend 策略中扣除已摘除 action
 *
 * @desc: 输入输出的非 NULL 描述符；__free_irq() 已摘链并持 desc->lock。
 * @action: 只读借用、尚未释放的 action，flags 必须保持安装时的值。
 *
 * 无直接返回值、不睡眠；精确递减 install 阶段增加的计数。函数不释放 action，
 * 调用后上层才能继续同步线程并回收对象；错误配对会造成深度下溢或挂起策略失真。
 */
void irq_pm_remove_action(struct irq_desc *desc, struct irqaction *action);
#else
/*
 * irq_pm_handle_wakeup() - 未启用系统睡眠时的唤醒处理空实现
 *
 * @desc: 未消费的描述符借用指针。返回：无直接返回值，不改 depth/pending 或 PM 状态。
 */
static inline void irq_pm_handle_wakeup(struct irq_desc *desc) { }
/*
 * irq_pm_install_action() - 未启用系统睡眠时省略 action 的 PM 计数
 *
 * @desc: 未消费的描述符借用指针。
 * @action: 未消费的 action 借用指针。
 * 返回：无直接返回值、不睡眠、无副作用，普通 IRQ 注册仍由调用者完成。
 */
static inline void
irq_pm_install_action(struct irq_desc *desc, struct irqaction *action) { }
/*
 * irq_pm_remove_action() - 未启用系统睡眠时省略 PM 计数撤销
 *
 * @desc: 未消费的描述符借用指针。
 * @action: 未消费的 action 借用指针。
 * 返回：无直接返回值、无副作用，action 内存仍由 free 路径释放。
 */
static inline void
irq_pm_remove_action(struct irq_desc *desc, struct irqaction *action) { }
#endif

#ifdef CONFIG_GENERIC_IRQ_CHIP
/*
 * irq_init_generic_chip() - 初始化调用者已分配的通用 irqchip 容器
 *
 * @gc: 输入输出的非 NULL 借用对象，尾部至少容纳 @num_ct 个 chip_types；所有权不变。
 * @name: 可为 NULL 的只读名称借用指针；非 NULL 时生命周期须覆盖 irq_chip 使用期。
 * @num_ct: chip type 数量，必须为正且不超过 @gc 的实际分配容量。
 * @irq_base: 该 generic chip 管理的起始硬件/域内 IRQ 编号。
 * @reg_base: 可为 NULL 的 MMIO 基址借用指针，由具体访问回调解释。
 * @handler: 可为 NULL 的默认高层流处理函数指针，写入首个 chip type。
 *
 * generic-chip 分配/domain 构造路径调用。无返回值、不睡眠；初始化 gc->lock、数量、
 * 基址并为全部 type 绑定名称，但不注册 IRQ、不映射寄存器，也不取得参数引用。
 */
void irq_init_generic_chip(struct irq_chip_generic *gc, const char *name,
			   int num_ct, unsigned int irq_base,
			   void __iomem *reg_base, irq_flow_handler_t handler);
#else
/*
 * irq_init_generic_chip() - 未启用 generic irqchip 时的初始化空实现
 *
 * @gc: 未消费的 generic chip 借用指针。
 * @name: 未消费的可空名称借用指针。
 * @num_ct: 未消费的 chip type 数量。
 * @irq_base: 未消费的起始 IRQ 编号。
 * @reg_base: 未消费的可空 MMIO 基址借用指针。
 * @handler: 未消费的可空流处理函数指针。
 * 返回：无直接返回值、不睡眠、无副作用。此配置下调用者不能依赖 gc 字段已初始化，
 * 相关通用芯片功能本身也不会构建。
 */
static inline void
irq_init_generic_chip(struct irq_chip_generic *gc, const char *name,
		      int num_ct, unsigned int irq_base,
		      void __iomem *reg_base, irq_flow_handler_t handler) { }
#endif /* CONFIG_GENERIC_IRQ_CHIP */
/* 上述条件块在启用通用 irqchip 时使用真实初始化器，否则编译为空操作。 */

#ifdef CONFIG_GENERIC_PENDING_IRQ
/*
 * irq_can_move_pcntxt() - 判断 irqchip 是否允许在进程上下文立即迁移
 *
 * @data: 只读借用的非 NULL irq_data；data->chip 已稳定。
 *
 * 返回 false 表示 chip 声明 IRQCHIP_MOVE_DEFERRED，亲和性改变必须记录 pending 并
 * 等中断屏蔽窗口完成；true 表示可直接编程。函数不睡眠、无副作用。
 */
static inline bool irq_can_move_pcntxt(struct irq_data *data)
{
	return !(data->chip->flags & IRQCHIP_MOVE_DEFERRED);
}
/*
 * irq_move_pending() - 查询 irq_data 是否仍有延迟亲和性迁移
 *
 * @data: 只读借用的非 NULL irq_data；调用者用 desc->lock 稳定状态。
 * 返回 true 表示 pending_mask 尚待安全应用，false 表示没有；不睡眠、无副作用。
 */
static inline bool irq_move_pending(struct irq_data *data)
{
	return irqd_is_setaffinity_pending(data);
}
/*
 * irq_copy_pending() - 保存下一次延迟迁移要应用的目标 CPU 集合
 *
 * @desc: 输入输出的非 NULL 描述符，pending_mask 已分配；调用者持 desc->lock。
 * @mask: 只读非 NULL cpumask 借用指针，内容复制后可由调用者立即复用/释放。
 * 返回：无直接返回值、不睡眠；只更新 owned pending_mask，是否 pending 由单独状态位表达。
 */
static inline void
irq_copy_pending(struct irq_desc *desc, const struct cpumask *mask)
{
	cpumask_copy(desc->pending_mask, mask);
}
/*
 * irq_get_pending() - 把描述符保存的延迟迁移目标复制给调用者
 *
 * @mask: 非 NULL 输出 cpumask；调用前内容被覆盖，所有权仍属于调用者。
 * @desc: 只读非 NULL 描述符，pending_mask 已分配且由 desc->lock 稳定。
 * 返回：无直接返回值、不睡眠；输出是值副本，不借出内部存储。
 */
static inline void
irq_get_pending(struct cpumask *mask, struct irq_desc *desc)
{
	cpumask_copy(mask, desc->pending_mask);
}
/*
 * irq_desc_get_pending_mask() - 借出描述符内部 pending affinity 掩码
 *
 * @desc: 非 NULL 描述符借用指针；调用者保证配置启用且对象存活。
 * 返回可写内部 cpumask 借用指针，不增加引用、不会为 NULL；只能在 desc 生命周期和
 * 适当锁范围内使用，调用者不得释放该存储。函数不睡眠、无副作用。
 */
static inline struct cpumask *irq_desc_get_pending_mask(struct irq_desc *desc)
{
	return desc->pending_mask;
}
/*
 * irq_fixup_move_pending() - CPU 下线时修正未完成的 IRQ 迁移
 *
 * @desc: 输入输出的非 NULL 描述符；CPU hotplug 调用者持有 desc->lock。
 * @force_clear: true 无条件清 pending 位；false 仅在目标中已没有在线 CPU 时清除。
 *
 * 返回 true 表示原先确有 pending 且目标仍含除下线 CPU 外的在线 CPU；false 表示
 * 无 pending 或已无可用目标。函数不应用亲和性、不清 pending_mask、不转移所有权。
 */
bool irq_fixup_move_pending(struct irq_desc *desc, bool force_clear);
/*
 * irq_force_complete_move() - 请求 domain 层级立即完成底层向量迁移清理
 *
 * @desc: 输入输出的非 NULL 描述符借用指针；hotplug/迁移路径已串行化层级状态。
 *
 * 无直接返回值、不睡眠。函数从顶层 irq_data 向 parent 扫描，调用首个提供
 * irq_force_complete_move 的 chip 后停止；没有实现则为空结果。回调负责实际硬件
 * 清理，本函数不改变 pending 位或掩码。
 */
void irq_force_complete_move(struct irq_desc *desc);
#else /* CONFIG_GENERIC_PENDING_IRQ */
/* 未启用通用 pending IRQ 时，没有可保存或稍后完成的延迟亲和性迁移状态。 */
/*
 * irq_can_move_pcntxt() - 无 pending 机制时允许调用者走直接迁移判断
 *
 * @data: 未消费的 irq_data 借用指针。
 * 返回：恒为 true、不睡眠、无副作用；若 chip 自身拒绝，直接设置路径仍返回错误。
 */
static inline bool irq_can_move_pcntxt(struct irq_data *data)
{
	return true;
}
/*
 * irq_move_pending() - 无 pending 机制时报告不存在延迟迁移
 *
 * @data: 未消费的 irq_data 借用指针。
 * 返回：恒为 false、不睡眠、无副作用。
 */
static inline bool irq_move_pending(struct irq_data *data)
{
	return false;
}
/*
 * irq_copy_pending() - 无 pending 存储配置下的目标保存空实现
 *
 * @desc: 未消费的描述符借用指针。
 * @mask: 未消费的只读掩码借用指针。
 * 返回：无直接返回值、不睡眠；调用者不能期待稍后恢复该目标。
 */
static inline void
irq_copy_pending(struct irq_desc *desc, const struct cpumask *mask)
{
}
/*
 * irq_get_pending() - 无 pending 存储配置下的目标读取空实现
 *
 * @mask: 未修改的输出候选缓冲区。
 * @desc: 未消费的描述符借用指针。
 * 返回：无直接返回值；不会初始化 @mask，调用者必须由配置语义避免使用其旧内容。
 */
static inline void
irq_get_pending(struct cpumask *mask, struct irq_desc *desc)
{
}
/*
 * irq_desc_get_pending_mask() - 无 pending 存储时返回空指针
 *
 * @desc: 未消费的描述符借用指针。
 * 返回 NULL，明确不存在内部掩码；不睡眠、无副作用、无引用变化。
 */
static inline struct cpumask *irq_desc_get_pending_mask(struct irq_desc *desc)
{
	return NULL;
}
/*
 * irq_fixup_move_pending() - 无 pending 机制时的 CPU 下线修正空实现
 *
 * @desc: 未消费的描述符借用指针。
 * @fclear: 未消费的强制清除策略。
 * 返回恒为 false，表示没有遗留迁移可修正；不睡眠、无副作用。
 */
static inline bool irq_fixup_move_pending(struct irq_desc *desc, bool fclear)
{
	return false;
}
/*
 * irq_force_complete_move() - 无 pending 机制时的强制完成空实现
 *
 * @desc: 未消费的描述符借用指针。
 * 返回：无直接返回值、不调用 irqchip、不改变亲和性或生命周期。
 */
static inline void irq_force_complete_move(struct irq_desc *desc) { }
#endif /* !CONFIG_GENERIC_PENDING_IRQ */
/* 上述条件块为启用配置维护延迟迁移状态；关闭配置用常量/空操作消除相关分支。 */

#if !defined(CONFIG_IRQ_DOMAIN) || !defined(CONFIG_IRQ_DOMAIN_HIERARCHY)
/*
 * irq_domain_activate_irq() - 无层级 domain 时仅发布 irq_data 已激活状态
 *
 * @data: 输入输出的非 NULL irq_data 借用指针；调用者串行化激活生命周期。
 * @reserve: 未消费的资源预留策略；此配置没有层级 activate 回调可区分它。
 *
 * 返回恒为 0、不睡眠；置 IRQD_ACTIVATED 让通用 startup 状态机保持相同不变量，
 * 但不调用硬件/domain 回调、不取得资源引用。后续必须由 deactivate 配对清位。
 */
static inline int irq_domain_activate_irq(struct irq_data *data, bool reserve)
{
	irqd_set_activated(data);
	return 0;
}
/*
 * irq_domain_deactivate_irq() - 无层级 domain 时撤销软件激活标记
 *
 * @data: 输入输出的非 NULL irq_data 借用指针；与先前 activate 对应。
 * 返回：无直接返回值、不睡眠；只清 IRQD_ACTIVATED，没有 domain 资源需要释放。
 */
static inline void irq_domain_deactivate_irq(struct irq_data *data)
{
	irqd_clr_activated(data);
}
#endif

/*
 * irqd_get_parent_data() - 取得 IRQ domain 层级中的上一层 irq_data
 *
 * @irqd: 只读借用的非 NULL irq_data；调用者保证整个层级在遍历期间存活。
 *
 * 启用 CONFIG_IRQ_DOMAIN_HIERARCHY 时返回 parent_data 借用指针，根节点可返回 NULL；
 * 非层级配置恒返回 NULL，编译器可消除父层循环。函数不增加 domain/data 引用、
 * 不睡眠、无副作用；返回指针不得逃逸出调用者的层级生命周期保护。
 */
static inline struct irq_data *irqd_get_parent_data(struct irq_data *irqd)
{
#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
	return irqd->parent_data;
#else
	return NULL;
#endif
}
