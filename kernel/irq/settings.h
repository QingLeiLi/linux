/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Internal header to deal with irq_desc->status which will be renamed
 * to irq_desc->settings.
 */
/*
 * 本内部头文件集中封装 irq_desc 的策略/属性位。上面的历史注释说该字段将从
 * status 改名为 settings；当前版本实际字段名是 status_use_accessors，名称也在
 * 强调调用者应通过本文件的访问器读写，而不是把这些长期属性与 desc->istate
 * 中的瞬时运行状态混在一起。
 *
 * irq_desc 由 irqdesc.c 分配和初始化；chip.c、manage.c、proc.c 等管理路径借助
 * 这些访问器决定中断能否申请、线程化、探测、自动启用或参与负载均衡。访问器
 * 不自行加锁：初始化阶段尚未发布的 desc 可由创建者独占修改；运行期写入通常
 * 由持有 desc->lock 的上层路径完成。单纯拥有 desc 指针只保证能定位对象，并不
 * 自动保证这里的复合读改写可与并发写者安全竞争。
 */
enum {
	/* 新描述符采用体系结构给出的初始策略位，irqdesc.c 初始化时一次性写入。 */
	_IRQ_DEFAULT_INIT_FLAGS	= IRQ_DEFAULT_INIT_FLAGS,
	/* 每 CPU 中断不在 CPU 之间迁移，其处理语义依附当前 CPU。 */
	_IRQ_PER_CPU		= IRQ_PER_CPU,
	/* 控制器按电平触发方式处理该 IRQ；重发和流控路径会据此选择策略。 */
	_IRQ_LEVEL		= IRQ_LEVEL,
	/* 传统自动探测不得尝试该 IRQ，避免探测过程扰动不可探测的硬件。 */
	_IRQ_NOPROBE		= IRQ_NOPROBE,
	/* 普通 request_irq() 路径不得占用该描述符，常用于尚未准备好的 IRQ。 */
	_IRQ_NOREQUEST		= IRQ_NOREQUEST,
	/* 该 IRQ 不允许强制线程化，硬中断处理上下文必须保留。 */
	_IRQ_NOTHREAD		= IRQ_NOTHREAD,
	/* 安装 action 后不自动启动 IRQ，驱动需要在准备完成后显式启用。 */
	_IRQ_NOAUTOEN		= IRQ_NOAUTOEN,
	/* 亲和性管理不得在 CPU 之间均衡该 IRQ。 */
	_IRQ_NO_BALANCING	= IRQ_NO_BALANCING,
	/* 该 IRQ 的线程处理嵌套在父 IRQ 线程中，不创建普通独立 irq thread。 */
	_IRQ_NESTED_THREAD	= IRQ_NESTED_THREAD,
	/* 每个 CPU 使用不同 dev_id 注册 action，管理接口必须按 CPU 取身份。 */
	_IRQ_PER_CPU_DEVID	= IRQ_PER_CPU_DEVID,
	/* 中断可参与失控/杂散检测路径的轮询补偿。 */
	_IRQ_IS_POLLED		= IRQ_IS_POLLED,
	/* 禁止延迟屏蔽：disable_irq 路径必须立即把屏蔽动作落实到 irqchip。 */
	_IRQ_DISABLE_UNLAZY	= IRQ_DISABLE_UNLAZY,
	/* 不向 /proc/interrupts 等用户可见接口展示该 IRQ。 */
	_IRQ_HIDDEN		= IRQ_HIDDEN,
	/* note_interrupt() 不对该 IRQ 执行未处理/杂散中断调试统计。 */
	_IRQ_NO_DEBUG		= IRQ_NO_DEBUG,
	/* 复用公共保留位记录 proc 目录项当前有效，供 proc.c 的并发拆建协议使用。 */
	_IRQ_PROC_VALID		= IRQ_RESERVED,
	/* 公共 API 允许修改的位集合；批量更新必须把 clr/set 都限制在此掩码内。 */
	_IRQF_MODIFY_MASK	= IRQF_MODIFY_MASK,
};

/*
 * 从这里开始故意毒化公共 IRQ_* 名称：本目录内部若绕过访问器直接使用公共位名，
 * 编译会在 GOT_YOU_MORON 处失败。这样既防止新代码直接操作字段，也防止无意混用
 * 公共 API 标志和本文件保存下来的 _IRQ_* 内部别名。IRQF_MODIFY_MASK 先取消旧宏
 * 再毒化，是因为它本来就是宏而不是枚举常量。
 */
#define IRQ_PER_CPU		GOT_YOU_MORON
#define IRQ_NO_BALANCING	GOT_YOU_MORON
#define IRQ_LEVEL		GOT_YOU_MORON
#define IRQ_NOPROBE		GOT_YOU_MORON
#define IRQ_NOREQUEST		GOT_YOU_MORON
#define IRQ_NOTHREAD		GOT_YOU_MORON
#define IRQ_NOAUTOEN		GOT_YOU_MORON
#define IRQ_NESTED_THREAD	GOT_YOU_MORON
#define IRQ_PER_CPU_DEVID	GOT_YOU_MORON
#define IRQ_IS_POLLED		GOT_YOU_MORON
#define IRQ_DISABLE_UNLAZY	GOT_YOU_MORON
#define IRQ_HIDDEN		GOT_YOU_MORON
#define IRQ_NO_DEBUG		GOT_YOU_MORON
#define IRQ_RESERVED		GOT_YOU_MORON
#undef IRQF_MODIFY_MASK
#define IRQF_MODIFY_MASK	GOT_YOU_MORON

/*
 * irq_settings_clr_and_set() - 受控地批量替换描述符的可修改策略位
 *
 * 调用位置：irq_set_status_flags()/irq_clear_status_flags() 等外部设置接口最终在
 * chip.c 中取得 desc->lock，再调用本函数完成真正的字段更新；irqdesc.c 也在新
 * 描述符尚未发布时用它装入体系结构默认值。
 *
 * @desc: 输入输出的 IRQ 描述符借用指针，必须非 NULL；本函数不取得引用，也不
 *        延长对象生命周期。运行期调用者应持有 desc->lock，初始化期则须独占对象。
 * @clr:  希望清除的候选位集合；不在 _IRQF_MODIFY_MASK 内的位会被忽略。
 * @set:  希望置位的候选位集合；同样只允许修改公共 API 明确开放的策略位。
 *
 * 本函数不睡眠、无直接返回值。它先清除后置位，因此同一位同时出现在 clr 和
 * set 时最终为 1；成功后 desc->status_use_accessors 立即反映新策略，所有权不变。
 * 掩码限制阻止调用者借公共设置接口破坏只应由 IRQ 核心维护的内部状态。
 */
static inline void
irq_settings_clr_and_set(struct irq_desc *desc, u32 clr, u32 set)
{
	/* 两次读改写依赖调用者串行化；它们不是跨 CPU 的原子更新协议。 */
	desc->status_use_accessors &= ~(clr & _IRQF_MODIFY_MASK);
	desc->status_use_accessors |= (set & _IRQF_MODIFY_MASK);
}

/*
 * irq_settings_is_per_cpu() - 查询 IRQ 是否具有固定的每 CPU 处理语义
 *
 * @desc: 只读借用的非 NULL 描述符；调用者负责保证对象存活和所需的一致性。
 *
 * manage.c、irqdesc.c 和 proc.c 用结果排除普通亲和性/共享管理路径。返回 true
 * 表示设置了 _IRQ_PER_CPU，false 表示可继续按普通 IRQ 判断；不修改状态、不取得
 * 引用、不会睡眠。这个短 getter 的函数体仅完成掩码读取，阶段说明予以豁免。
 */
static inline bool irq_settings_is_per_cpu(struct irq_desc *desc)
{
	return desc->status_use_accessors & _IRQ_PER_CPU;
}

/*
 * irq_settings_is_per_cpu_devid() - 查询 action 身份是否按 CPU 分开保存
 *
 * @desc: 只读借用的非 NULL 描述符；其生命周期和并发稳定性由调用者保证。
 *
 * request_percpu_irq()/free_percpu_irq() 等接口用它拒绝错误的普通 IRQ 操作。
 * 返回 true 表示每 CPU 都以各自 dev_id 管理 action，false 表示不是这种注册模型；
 * 函数不睡眠、无副作用，也不会把借用指针转换为持有引用。
 */
static inline bool irq_settings_is_per_cpu_devid(struct irq_desc *desc)
{
	return desc->status_use_accessors & _IRQ_PER_CPU_DEVID;
}

/*
 * irq_settings_set_per_cpu() - 把描述符标记为不可按普通 IRQ 迁移的每 CPU IRQ
 *
 * @desc: 输入输出的非 NULL 借用指针；调用者必须持有 desc->lock，或在发布前独占。
 *
 * manage.c 在安装 IRQF_PERCPU action 时调用。无直接返回值且不睡眠；返回后后续
 * 亲和性、proc 和释放路径会采用每 CPU 规则。位只会被置上，本函数不负责撤销，
 * 也不改变 desc/action 的引用所有权。
 */
static inline void irq_settings_set_per_cpu(struct irq_desc *desc)
{
	desc->status_use_accessors |= _IRQ_PER_CPU;
}

/*
 * irq_settings_set_no_balancing() - 禁止 IRQ 核心自动迁移该描述符
 *
 * @desc: 输入输出的非 NULL 借用描述符；运行期写入要求调用者串行化 desc->lock。
 *
 * 安装 IRQF_NOBALANCING action 时由 manage.c 调用。无直接返回值、不睡眠；置位后
 * 亲和性管理保留现有目标 CPU，而不是参与常规均衡。本函数不移动 IRQ、不编程
 * 控制器，也不取得任何引用，那些后续动作仍由上层管理路径负责。
 */
static inline void irq_settings_set_no_balancing(struct irq_desc *desc)
{
	desc->status_use_accessors |= _IRQ_NO_BALANCING;
}

/*
 * irq_settings_has_no_balance_set() - 查询描述符是否禁止 IRQ 自动均衡
 *
 * @desc: 只读借用的非 NULL 描述符；调用者保证对象存活及读取期间状态稳定。
 *
 * chip.c 在更换 irqchip/handler 后据此重建 irq_data 状态。返回 true 表示必须把
 * IRQD_NO_BALANCING 同步到 irq_data，false 表示没有该限制；不睡眠、无副作用。
 */
static inline bool irq_settings_has_no_balance_set(struct irq_desc *desc)
{
	return desc->status_use_accessors & _IRQ_NO_BALANCING;
}

/*
 * irq_settings_get_trigger_mask() - 取得描述符保存的硬件触发类型
 *
 * @desc: 只读借用的非 NULL 描述符；本函数不取得引用。
 *
 * 返回 IRQ_TYPE_* 中受 IRQ_TYPE_SENSE_MASK 约束的位组合，0 表示尚未指定；chip.c
 * 和 manage.c 用它比较已有配置与新 action 的触发要求。函数不睡眠、不改状态；
 * 调用者若要求与并发设置严格一致，必须在外层持有 desc->lock。
 */
static inline u32 irq_settings_get_trigger_mask(struct irq_desc *desc)
{
	return desc->status_use_accessors & IRQ_TYPE_SENSE_MASK;
}

/*
 * irq_settings_set_trigger_mask() - 原位替换描述符的 IRQ_TYPE_* 触发类型
 *
 * @desc: 输入输出的非 NULL 借用描述符；运行期调用者必须用 desc->lock 串行化。
 * @mask: 新触发类型的候选位；函数只保留 IRQ_TYPE_SENSE_MASK 覆盖的部分，其他
 *        请求标志不会泄漏进描述符策略字段。
 *
 * manage.c 在硬件触发配置成功后记录结果。无直接返回值、不睡眠；返回后旧触发位
 * 已全部清除并由新值替代，其他 settings 位保持不变。两步读改写不是原子事务，
 * 因而不能脱离调用者的锁与并发设置路径竞争。
 */
static inline void
irq_settings_set_trigger_mask(struct irq_desc *desc, u32 mask)
{
	/* 先清空整组触发位，避免从边沿改为电平时残留互斥的旧类型。 */
	desc->status_use_accessors &= ~IRQ_TYPE_SENSE_MASK;
	desc->status_use_accessors |= mask & IRQ_TYPE_SENSE_MASK;
}

/*
 * irq_settings_is_level() - 查询核心流控是否把 IRQ 视为电平触发
 *
 * @desc: 只读借用的非 NULL 描述符；调用者保证其生命周期和并发一致性。
 *
 * chip.c 和 resend.c 用结果选择电平 IRQ 的屏蔽、确认及重发规则。返回 true 表示
 * _IRQ_LEVEL 已置位，false 表示未置位；不睡眠、无副作用。该派生流控位与上面的
 * IRQ_TYPE_* 原始触发掩码用途不同，二者由设置 handler 的路径保持一致。
 */
static inline bool irq_settings_is_level(struct irq_desc *desc)
{
	return desc->status_use_accessors & _IRQ_LEVEL;
}

/*
 * irq_settings_clr_level() - 清除 IRQ 核心的电平流控标记
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者持 desc->lock 或在发布前独占。
 *
 * manage.c 在重新配置触发类型时先清除此位，再按新类型决定是否置回。无直接
 * 返回值、不睡眠；只改变 _IRQ_LEVEL，不会自行改 irqchip 或硬件触发寄存器。
 */
static inline void irq_settings_clr_level(struct irq_desc *desc)
{
	desc->status_use_accessors &= ~_IRQ_LEVEL;
}

/*
 * irq_settings_set_level() - 标记后续 IRQ 核心路径采用电平触发流控
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者持 desc->lock 或独占未发布对象。
 *
 * 触发类型设置路径确认 IRQ_TYPE_LEVEL_* 后调用。无直接返回值、不睡眠；置位后
 * resend.c 等路径会避免把电平仍有效误当成一次性边沿事件，所有权保持不变。
 */
static inline void irq_settings_set_level(struct irq_desc *desc)
{
	desc->status_use_accessors |= _IRQ_LEVEL;
}

/*
 * irq_settings_can_request() - 判断普通 IRQ 申请接口是否可占用描述符
 *
 * @desc: 只读借用的非 NULL 描述符；调用者负责稳定对象和所需锁。
 *
 * manage.c 在 request_irq 类路径中把 false 当作不可申请条件。返回 true 表示
 * _IRQ_NOREQUEST 未设置、仍可继续其他合法性检查；false 只说明核心策略禁止申请，
 * 不代表 IRQ 已被占用。函数不睡眠、不修改 action 或引用。
 */
static inline bool irq_settings_can_request(struct irq_desc *desc)
{
	return !(desc->status_use_accessors & _IRQ_NOREQUEST);
}

/*
 * irq_settings_clr_norequest() - 开放描述符给普通 IRQ 申请接口
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者须持 desc->lock 或独占未发布对象。
 *
 * irq_test 等受控初始化路径在 IRQ 已准备好后清除初始的禁止申请位。无直接
 * 返回值、不睡眠；返回后仅通过 settings 放开资格，尚未创建 action、启用 IRQ
 * 或取得模块引用，真正申请仍可能因共享规则、资源冲突等原因失败。
 */
static inline void irq_settings_clr_norequest(struct irq_desc *desc)
{
	desc->status_use_accessors &= ~_IRQ_NOREQUEST;
}

/*
 * irq_settings_set_norequest() - 禁止普通申请路径占用描述符
 *
 * @desc: 输入输出的非 NULL 借用描述符；运行期写入由 desc->lock 串行化。
 *
 * chip.c 在建立 chained handler 等由核心专用的中断流时调用。无直接返回值、
 * 不睡眠；返回后 request_irq 类接口会拒绝新 action，但本函数不会删除已经存在
 * 的 action，也不会等待正在执行的 handler，调用顺序必须由上层保证安全。
 */
static inline void irq_settings_set_norequest(struct irq_desc *desc)
{
	desc->status_use_accessors |= _IRQ_NOREQUEST;
}

/*
 * irq_settings_can_thread() - 判断该 IRQ 是否允许进入线程化处理模型
 *
 * @desc: 只读借用的非 NULL 描述符；调用者稳定对象及 settings 读取。
 *
 * handle.c 的强制线程化和 manage.c 的 action 安装路径使用该结果。返回 true 只
 * 表示 _IRQ_NOTHREAD 未禁止线程化，仍需结合 action flags 和 handler 形态决定；
 * 返回 false 要保留硬中断上下文。函数不睡眠、无状态或所有权副作用。
 */
static inline bool irq_settings_can_thread(struct irq_desc *desc)
{
	return !(desc->status_use_accessors & _IRQ_NOTHREAD);
}

/*
 * irq_settings_clr_nothread() - 撤销描述符的禁止线程化约束
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者须持锁或独占未发布对象。
 *
 * 当前树内没有直接调用者；保留它是为了与置位访问器形成受控接口，供需要恢复
 * 可线程化资格的核心路径使用。无直接返回值且不睡眠。返回后线程化只是“被允许”，
 * 现存 action 不会被自动重建为线程，真正的 irq thread 创建仍在 request/setup 路径。
 */
static inline void irq_settings_clr_nothread(struct irq_desc *desc)
{
	desc->status_use_accessors &= ~_IRQ_NOTHREAD;
}

/*
 * irq_settings_set_nothread() - 强制描述符保留非线程化的硬中断语义
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者须持 desc->lock 或在发布前独占。
 *
 * chained IRQ 等必须在父控制器硬中断上下文继续分发的路径会置此位。无直接
 * 返回值、不睡眠；它只建立后续申请约束，不负责停止已经运行的 irq thread。
 */
static inline void irq_settings_set_nothread(struct irq_desc *desc)
{
	desc->status_use_accessors |= _IRQ_NOTHREAD;
}

/*
 * irq_settings_can_probe() - 判断传统 IRQ 自动探测是否可试探该描述符
 *
 * @desc: 只读借用的非 NULL 描述符；autoprobe.c 在持有其全局探测互斥协议并
 *        锁定具体 desc 后读取，本函数自身不取得锁或引用。
 *
 * 返回 true 表示 _IRQ_NOPROBE 未置位，探测代码还会继续检查 desc->action；false
 * 表示必须跳过。函数不睡眠、不触碰硬件，实际启动/关闭探测由调用者完成。
 */
static inline bool irq_settings_can_probe(struct irq_desc *desc)
{
	return !(desc->status_use_accessors & _IRQ_NOPROBE);
}

/*
 * irq_settings_clr_noprobe() - 允许传统自动探测路径尝试该 IRQ
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者负责 desc->lock 或发布前独占。
 *
 * 当前树内没有直接调用者；它作为与置位访问器对称的核心内部接口保留。无直接
 * 返回值、不睡眠；返回后只是取消策略屏障，autoprobe 仍会验证描述符没有 action，
 * 并通过 irqchip 回调实际操纵线路。本函数不保证硬件适合探测。
 */
static inline void irq_settings_clr_noprobe(struct irq_desc *desc)
{
	desc->status_use_accessors &= ~_IRQ_NOPROBE;
}

/*
 * irq_settings_set_noprobe() - 阻止自动探测代码扰动该 IRQ
 *
 * @desc: 输入输出的非 NULL 借用描述符；运行期写入由外层 desc->lock 保护。
 *
 * chained handler 等核心专用线路在配置时置位。无直接返回值、不睡眠；后续
 * autoprobe 会跳过该描述符，但正常的显式 IRQ 管理是否允许仍由其他设置位决定。
 */
static inline void irq_settings_set_noprobe(struct irq_desc *desc)
{
	desc->status_use_accessors |= _IRQ_NOPROBE;
}

/*
 * irq_settings_can_autoenable() - 判断首个 action 安装后是否应自动启动 IRQ
 *
 * @desc: 只读借用的非 NULL 描述符；调用者负责对象生命周期和一致读取。
 *
 * manage.c 在 setup_irq 完成硬件与 action 准备后查询。返回 true 表示没有
 * _IRQ_NOAUTOEN，可由核心清除禁用深度并启动线路；false 表示驱动需要稍后显式
 * enable_irq。函数不睡眠、无副作用，且不会直接改变 disable_depth 或硬件状态。
 */
static inline bool irq_settings_can_autoenable(struct irq_desc *desc)
{
	return !(desc->status_use_accessors & _IRQ_NOAUTOEN);
}

/*
 * irq_settings_is_nested_thread() - 查询处理函数是否运行在父 IRQ 的线程上下文
 *
 * @desc: 只读借用的非 NULL 描述符；调用者保证描述符有效。
 *
 * manage.c、pm.c 和 resend.c 用结果避开普通独立 irq thread 的创建、挂起和重发
 * 规则。返回 true 表示 nested-thread 模型，false 表示普通模型；不睡眠、不修改
 * action，也不取得父 IRQ 的引用。
 */
static inline bool irq_settings_is_nested_thread(struct irq_desc *desc)
{
	return desc->status_use_accessors & _IRQ_NESTED_THREAD;
}

/*
 * irq_settings_is_polled() - 查询 IRQ 是否参加核心的轮询补偿路径
 *
 * @desc: 只读借用的非 NULL 描述符；调用者稳定对象和 settings。
 *
 * spurious.c 用它避免递归轮询，并让指定线路参与共享 IRQ 的误路由恢复。返回
 * true 表示 _IRQ_IS_POLLED 已置位，false 表示普通线路；函数不发起轮询、不睡眠，
 * 也不改变 IRQS_POLL_INPROGRESS 这类瞬时状态。
 */
static inline bool irq_settings_is_polled(struct irq_desc *desc)
{
	return desc->status_use_accessors & _IRQ_IS_POLLED;
}

/*
 * irq_settings_disable_unlazy() - 查询 disable 是否必须立即落实到 irqchip
 *
 * @desc: 只读借用的非 NULL 描述符；chip.c 在 desc->lock 保护的禁用路径读取。
 *
 * 返回 true 时 __irq_disable() 不能采用“仅记软件禁用、等下一次中断再屏蔽”的
 * lazy disable，而要立刻调用 irq_mask/irq_disable；false 允许控制器能力和状态
 * 决定是否延迟。函数不睡眠、不编程硬件、无所有权变化。
 */
static inline bool irq_settings_disable_unlazy(struct irq_desc *desc)
{
	return desc->status_use_accessors & _IRQ_DISABLE_UNLAZY;
}

/*
 * irq_settings_clr_disable_unlazy() - 恢复该 IRQ 使用 lazy disable 的资格
 *
 * @desc: 输入输出的非 NULL 借用描述符；manage.c 在 desc->lock 下、彻底释放
 *        action 时调用，避免旧驱动的 IRQF_NO_AUTOEN/同步要求污染下一位使用者。
 *
 * 无直接返回值、不睡眠；返回后只清除策略位，并不会自动解屏蔽当前 IRQ。后续
 * disable 是否真正延迟仍取决于 irqchip 能力和运行状态。
 */
static inline void irq_settings_clr_disable_unlazy(struct irq_desc *desc)
{
	desc->status_use_accessors &= ~_IRQ_DISABLE_UNLAZY;
}

/*
 * irq_settings_is_hidden() - 查询 IRQ 是否应从用户可见的中断列表隐藏
 *
 * @desc: 只读借用的非 NULL 描述符；调用者保证对象有效。
 *
 * proc.c 在计算 /proc/interrupts 输出资格时使用。返回 true 表示不应展示，false
 * 表示仍需结合 chained handler 和 action 是否存在继续判断；不睡眠、无副作用，
 * 也不控制 debugfs 等其他接口的可见性。
 */
static inline bool irq_settings_is_hidden(struct irq_desc *desc)
{
	return desc->status_use_accessors & _IRQ_HIDDEN;
}

/*
 * irq_settings_set_no_debug() - 排除该 IRQ 的杂散中断调试判定
 *
 * @desc: 输入输出的非 NULL 借用描述符；manage.c 在持有 desc->lock 的 action
 *        安装阶段调用。
 *
 * IRQF_NO_DEBUG、每 CPU IRQ 或全局 noirqdebug 策略会触发置位。无直接返回值、
 * 不睡眠；它不会关闭 IRQ 或日志系统，只让 handle.c 跳过 note_interrupt() 的
 * “持续无人处理则禁用线路”统计，防止特殊 IRQ 被通用启发式误判。
 */
static inline void irq_settings_set_no_debug(struct irq_desc *desc)
{
	desc->status_use_accessors |= _IRQ_NO_DEBUG;
}

/*
 * irq_settings_no_debug() - 查询通用杂散 IRQ 调试是否被禁用
 *
 * @desc: 只读借用的非 NULL 描述符；handle.c 在 desc->lock 保护的分发尾部读取。
 *
 * 返回 true 时跳过 note_interrupt()，false 时把本次 irqreturn 结果交给调试统计；
 * 函数不睡眠、不清除统计、不取得引用。它只表达该描述符的长期策略。
 */
static inline bool irq_settings_no_debug(struct irq_desc *desc)
{
	return desc->status_use_accessors & _IRQ_NO_DEBUG;
}

/*
 * irq_settings_proc_valid() - 查询描述符当前是否有资格出现在 /proc/interrupts
 *
 * @desc: RCU 读侧借用的非 NULL 描述符；proc.c 尚未持有长期引用时读取此快照。
 *
 * 返回 true 后调用者仍必须通过 irq_desc_get_ref() 稳定生命周期，再退出 RCU
 * 临界区；返回 false 则跳过该描述符。此函数不睡眠、无副作用，资格位本身不能
 * 代替引用计数，也不保证 action 字段永久不变。
 */
static inline bool irq_settings_proc_valid(struct irq_desc *desc)
{
	return desc->status_use_accessors & _IRQ_PROC_VALID;
}

/*
 * irq_settings_update_proc_valid() - 在逻辑上替换 proc 输出资格位
 *
 * @desc: 输入输出的非 NULL 借用描述符；调用者处在 action/chained 状态更新的
 *        串行化上下文中，并负责保证与描述符拆除流程的顺序。
 * @set:  资格候选值；只有 _IRQ_PROC_VALID 位会写入，0 清除、非零对应位则置位。
 *
 * proc.c 根据 hidden、chained 和 action 状态计算 set。无直接返回值、不睡眠；
 * 返回后其他 settings 位保持不变。先清后置让任意候选值规范化，但两条普通
 * 读改写并非独立的跨 CPU 同步原语，生命周期仍由 RCU 与 irq_desc 引用协议保证。
 */
static inline void irq_settings_update_proc_valid(struct irq_desc *desc, u32 set)
{
	desc->status_use_accessors &= ~_IRQ_PROC_VALID;
	desc->status_use_accessors |= (set & _IRQ_PROC_VALID);
}
