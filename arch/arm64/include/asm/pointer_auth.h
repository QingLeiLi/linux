/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64 指针认证 C 接口学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 文件职责：描述用户与内核 PAC key 的内存表示，并提供密钥生成、系统寄存器
 * 装载、exec/切换/suspend 钩子和 prctl 后端接口。PAC 由“指针 + modifier（常见
 * 为栈指针）+ 128 位秘密密钥”计算并嵌入指针高位；认证失败会破坏指针，使后续
 * 使用不能悄悄沿攻击者选择的地址继续。
 *
 * 主要生命周期：
 *   exec -> ptrauth_thread_init_user() -> 新用户 keys 并全部启用
 *   fork/clone -> task/thread 上下文复制 -> 继承 keys 与 enabled 状态
 *   context switch -> 装载 next 的用户非 APIA keys 与内核 APIA key
 *   EL0 return/entry -> 在用户 APIA 与内核 APIA 之间切换
 *   prctl/ptrace -> 按接口重置、启停或检查用户 key 状态
 *   suspend resume -> 恢复可能被固件破坏的用户 key 系统寄存器
 *
 * ownership 与并发：keys_user/keys_kernel 嵌在 task_struct::thread 中，随任务上下文
 * 存活，不单独引用或释放。写硬件 key 寄存器是当前 CPU 的本地副作用，不会替
 * 其他 CPU 加锁；调用者必须处于其任务切换、异常入口或禁止抢占等既定协议中。
 * ISB 只同步本 CPU 后续取指对系统寄存器更新的观察，不是跨 CPU 内存屏障。
 *
 * 方案权衡：用户态五类 key 支持代码/数据和通用签名隔离；内核返回地址只使用
 * APIA，减少切换状态。API A 的用户与内核值复用同一硬件寄存器，因此异常边界
 * 必须额外切换；关闭配置时提供编译期桩，调用者无需复制大量 #ifdef。
 */
#ifndef __ASM_POINTER_AUTH_H
#define __ASM_POINTER_AUTH_H

/* include guard 防止结构体、内联函数和宏在同一翻译单元中重复定义。 */

#include <linux/bitops.h>
#include <linux/prctl.h>
#include <linux/random.h>

#include <asm/cpufeature.h>
#include <asm/memory.h>
#include <asm/sysreg.h>
/*
 * Linux 头提供位图、prctl ABI 与随机数；arm64 头提供能力检测、虚拟地址宽度和
 * 系统寄存器访问。它们仅建立编译期契约，不为 key 对象分配存储。
 */

/*
 * The EL0/EL1 pointer bits used by a pointer authentication code.
 * This is dependent on TBI0/TBI1 being enabled, or bits 63:56 would also apply.
 */
/*
 * PAC 占用 EL0/EL1 指针中未参与地址翻译的高位。这里给出的掩码依赖 TBI0/TBI1
 * 已启用；若不忽略顶字节，63:56 也可能参与 PAC。用户地址受 vabits_actual 限制，
 * 且位 55 留给地址符号扩展规则，所以使用 54..vabits_actual；内核 TTBR1 地址可
 * 使用 63..vabits_actual。掩码描述 PAC 可能所在的位置，不生成或验证 PAC。
 */
#define ptrauth_user_pac_mask()		GENMASK_ULL(54, vabits_actual)
#define ptrauth_kernel_pac_mask()	GENMASK_ULL(63, vabits_actual)

/*
 * prctl 的“启用 key”接口只控制四个地址认证 key 对应的 SCTLR_EL1 En* 位；通用
 * APGAKey 没有同类启停位，故不属于该掩码。反斜杠续行必须保持连续。
 */
#define PR_PAC_ENABLED_KEYS_MASK                                               \
	(PR_PAC_APIAKEY | PR_PAC_APIBKEY | PR_PAC_APDAKEY | PR_PAC_APDBKEY)

#ifdef CONFIG_ARM64_PTR_AUTH
/* 本配置打开用户态指针认证的数据结构与真实实现；关闭时文件末尾提供桩。 */
/*
 * Each key is a 128-bit quantity which is split across a pair of 64-bit
 * registers (Lo and Hi).
 */
/*
 * 每个体系结构 key 是 128 位秘密量，硬件把它拆到一对 64 位 Lo/Hi 系统寄存器。
 * 该结构体是寄存器对的内存快照：由随机数初始化，嵌入任务的 key 集合，并在任务
 * 切换或异常边界装入当前 CPU。lo/hi 必须作为同一个不可部分发布的逻辑 key 看待。
 */
struct ptrauth_key {
	/* lo/hi 分别保存硬件 key 寄存器的低、半高 64 位，无独立 ownership。 */
	unsigned long lo, hi;
};

/*
 * We give each process its own keys, which are shared by all threads. The keys
 * are inherited upon fork(), and reinitialised upon exec*().
 */
/*
 * 每个进程拥有一组用户 PAC key，同一线程组语义上共享；fork() 继承当前值，exec*
 * 为新程序映像重新随机化。实现把快照嵌在每个 task 的 thread 上下文中，以便调度
 * 和异常路径直接访问；创建线程时复制保持初始一致，prctl 定义其后续可见语义。
 *
 * APIA/APIB 用于指令地址，APDA/APDB 用于数据地址，APGA 用于通用认证码。对象由
 * task_struct 生命周期拥有，没有引用计数；ptrace 在受控停止状态下可读写快照。
 */
struct ptrauth_keys_user {
	/* 五个字段分别对应同名硬件 key 寄存器对，均为完整 128 位秘密。 */
	struct ptrauth_key apia;
	struct ptrauth_key apib;
	struct ptrauth_key apda;
	struct ptrauth_key apdb;
	struct ptrauth_key apga;
};

/*
 * __ptrauth_key_install_nosync(k, v) - 把一个 128 位 key 快照写入指定寄存器对。
 *
 * @k: APIA/APIB/APDA/APDB/APGA 之一，是参与 token 拼接的编译期寄存器族名称。
 * @v: struct ptrauth_key 值；先复制到唯一临时量，保证带副作用表达式只求值一次。
 *
 * 宏把 lo/hi 依次写到 SYS_<k>KEYLO_EL1 和 SYS_<k>KEYHI_EL1。调用前必须已经确认
 * CPU 支持对应认证类型，并保证当前 CPU 的 key 寄存器可以由该任务上下文更新。
 * 它不取锁、不睡眠、无返回值，也不附带 ISB；“nosync”明确要求调用者在任何 PAC
 * 指令可能使用新 key 前安排同步，或依赖异常返回等自同步边界。
 *
 * do/while(0) 让多条语句在 if 等调用位置表现为单条 C 语句；这服务于宏安全，
 * 不是运行时循环。
 */
#define __ptrauth_key_install_nosync(k, v)			\
do {								\
	struct ptrauth_key __pki_v = (v);			\
	write_sysreg_s(__pki_v.lo, SYS_ ## k ## KEYLO_EL1);	\
	write_sysreg_s(__pki_v.hi, SYS_ ## k ## KEYHI_EL1);	\
} while (0)

#ifdef CONFIG_ARM64_PTR_AUTH_KERNEL
/* 该配置让内核函数返回地址使用每任务 APIAKey；用户 key 支持可独立存在。 */

/*
 * 内核 PAC key 集只保留 APIA：编译器以内核 APIA 指令签名/认证函数返回地址，
 * 无需为内核维护 B、数据或通用 key。该对象嵌在 thread_struct，创建任务时随机化，
 * cpu_switch_to() 时装入本 CPU，随 task_struct 一起回收。
 */
struct ptrauth_keys_kernel {
	/* apia 是当前任务的 128 位内核指令地址认证秘密。 */
	struct ptrauth_key apia;
};

/*
 * ptrauth_keys_init_kernel() - 为一个任务生成新的内核 APIAKey 快照。
 *
 * @keys: 非 NULL 的输入输出借用指针，指向尚在构造或启动中的任务内嵌 key 集；
 * 调用不接管所有权，调用后对象仍由该 task_struct 持有。
 *
 * boot_init_stack_canary() 为 init_task 调用，copy_thread() 为新任务调用。调用者
 * 已独占目标任务的构造状态，无需锁；get_random_bytes() 不返回错误。本函数不把
 * key 装入硬件，成功副作用仅是内存快照更新。硬件不支持地址认证时保持原内容，
 * 因为随后所有真实 PAC 操作也由能力分支跳过。
 *
 * 返回：无直接返回值；不睡眠、不分配资源、无回滚路径。
 */
static __always_inline void ptrauth_keys_init_kernel(struct ptrauth_keys_kernel *keys)
{
	/* 运行时能力检测允许同一内核镜像在没有 PAC 的 CPU 上安全启动。 */
	if (system_supports_address_auth())
		get_random_bytes(&keys->apia, sizeof(keys->apia));
}

/*
 * ptrauth_keys_switch_kernel() - 把目标任务的内核 APIAKey 发布到当前 CPU。
 *
 * @keys: 非 NULL 的只读借用指针；通常属于即将运行的任务，调用不改变内存快照
 * 或 ownership。boot task 初始化路径在使能 PAC 前调用；普通切换由底层汇编使用
 * 等价宏完成。
 *
 * 支持地址认证时先写 Lo/Hi 寄存器，再执行 ISB，保证本 CPU 后续 PAC 指令按新
 * key 解释返回地址。ISB 不保护共享内存，也不与其他 CPU 配对；任务切换协议确保
 * 不会有两个执行流同时把同一 CPU 的 key 寄存器当作自己的状态。
 *
 * 返回：无直接返回值，无失败码。硬件不支持时无副作用；路径不取锁、不睡眠。
 */
static __always_inline void ptrauth_keys_switch_kernel(struct ptrauth_keys_kernel *keys)
{
	/* 快速跳过不具备地址认证能力的机器，避免访问不存在的 key 寄存器。 */
	if (!system_supports_address_auth())
		return;

	/* nosync 写寄存器后紧跟 ISB，二者共同构成本函数的本地发布边界。 */
	__ptrauth_key_install_nosync(APIA, keys->apia);
	isb();
}

#endif /* CONFIG_ARM64_PTR_AUTH_KERNEL */
/* 上方英文行尾注释说明真实内核 APIAKey 数据结构与 helper 到此结束。 */

/*
 * ptrauth_keys_install_user() - 预装一个任务除 APIA 外的用户 PAC key。
 *
 * @keys: 非 NULL 的只读借用指针，指向目标任务的用户 key 快照；调用不改变快照、
 * 不持有引用，也不转移 ownership。
 *
 * 主要调用者是任务切换、用户 key 初始化和 suspend 恢复路径。地址认证硬件存在时
 * 写 APIB/APDA/APDB，通用认证硬件存在时写 APGA。用户 APIA 刻意不在这里写：
 * APIAKey_EL1 同时承载内核返回地址 key，内核 C 代码运行期间必须保留内核值；
 * entry.S 只在即将 ERET 到 EL0、且之后不再调用内核 C 函数时装入用户 APIA。
 *
 * 返回：无直接返回值。硬件不支持对应能力时该组寄存器保持不变；不取锁、不睡眠、
 * 不返回错误。此 helper 不执行 ISB，调用路径依靠后续任务切换/异常返回的同步边界，
 * 在用户 PAC 指令执行前使写入生效。
 */
static inline void ptrauth_keys_install_user(struct ptrauth_keys_user *keys)
{
	/* 三个地址 key 共享地址认证能力位，按一组装入当前 CPU。 */
	if (system_supports_address_auth()) {
		__ptrauth_key_install_nosync(APIB, keys->apib);
		__ptrauth_key_install_nosync(APDA, keys->apda);
		__ptrauth_key_install_nosync(APDB, keys->apdb);
	}

	/* APGA 使用独立的 generic authentication 能力，不能由地址能力代替判断。 */
	if (system_supports_generic_auth())
		__ptrauth_key_install_nosync(APGA, keys->apga);
}

/*
 * ptrauth_keys_init_user() - 为 exec 后的新用户程序生成完整 PAC key 集。
 *
 * @keys: 非 NULL 的输入输出借用指针，通常是 current->thread.keys_user。调用者在
 * exec 提交新程序映像的串行上下文中拥有写权限；对象仍归 task_struct 所有。
 *
 * 地址认证存在时随机化 APIA/APIB/APDA/APDB，通用认证存在时随机化 APGA，随后
 * 预装可在内核态安全更新的 APIB/APDA/APDB/APGA。APIA 内存值虽已生成，但必须等到
 * 返回 EL0 的最后阶段再替换硬件中的内核 APIAKey。
 *
 * 返回：无直接返回值，无失败码。每次 exec 生成新秘密，使旧程序泄露的 key 不会
 * 直接跨越映像替换；fork/clone 不走本函数，因而继承父任务快照。硬件缺少某类
 * 能力时对应字段不使用，函数不分配资源、不睡眠、无需回滚。
 */
static inline void ptrauth_keys_init_user(struct ptrauth_keys_user *keys)
{
	/* 为四个地址认证用途分别生成独立秘密，避免一个用途的泄露直接复用到另一个。 */
	if (system_supports_address_auth()) {
		get_random_bytes(&keys->apia, sizeof(keys->apia));
		get_random_bytes(&keys->apib, sizeof(keys->apib));
		get_random_bytes(&keys->apda, sizeof(keys->apda));
		get_random_bytes(&keys->apdb, sizeof(keys->apdb));
	}

	/* generic authentication 是独立可选能力，只在硬件支持时初始化 APGA。 */
	if (system_supports_generic_auth())
		get_random_bytes(&keys->apga, sizeof(keys->apga));

	/* 发布本阶段允许立即写入的用户 key；APIA 的延迟装载规则见上一个函数。 */
	ptrauth_keys_install_user(keys);
}

/*
 * ptrauth_prctl_reset_keys() - 实现 PR_PAC_RESET_KEYS 的用户 key 轮换。
 *
 * @tsk: 非 NULL 的目标任务借用指针；正常 prctl 路径是 current，函数不获取长期
 * 引用、不接管 task。调用者必须按 prctl/ptrace 协议保证目标状态可安全修改。
 * @arg: PR_PAC_AP*KEY 位图；0 表示重置所有硬件支持的用户 key，非零只重置选中项。
 *
 * 成功返回 0，并更新目标内存快照及可立即装入的用户寄存器；APIA 在返回 EL0 时
 * 安装。硬件完全不支持、目标是 compat 任务、位图非法或请求了不支持的 key 时
 * 返回 -EINVAL，且在通过全部校验前不修改 key。具体实现位于 pointer_auth.c。
 */
extern int ptrauth_prctl_reset_keys(struct task_struct *tsk, unsigned long arg);

/*
 * ptrauth_set_enabled_keys() - 更新任务的地址认证使能位快照。
 *
 * @tsk: 非 NULL 的目标任务借用指针；不转移 ownership。
 * @keys: 本次允许改变的 APIA/APIB/APDA/APDB 位集合。
 * @enabled: @keys 子集中希望置为启用的位；集合外位必须为零。
 *
 * 成功返回 0，修改 tsk->thread.sctlr_user；目标为 current 时还在禁止抢占区间同步
 * 更新 SCTLR_EL1。无地址认证、compat 任务或位图关系非法时返回 -EINVAL 且状态
 * 不变。调用可用于 prctl 和受 ptrace 协议稳定的目标任务。
 */
extern int ptrauth_set_enabled_keys(struct task_struct *tsk, unsigned long keys,
				    unsigned long enabled);
/*
 * ptrauth_get_enabled_keys() - 从任务 SCTLR 用户快照导出已启用地址 key 位图。
 *
 * @tsk: 非 NULL 的只读借用指针，调用者负责保证读取目标 thread 状态的并发安全。
 * 返回非负 PR_PAC_AP*KEY 位图；无地址认证或 compat 任务返回 -EINVAL。函数不写
 * 硬件、不改变引用或 ownership。
 */
extern int ptrauth_get_enabled_keys(struct task_struct *tsk);

/*
 * ptrauth_enable() - 在当前 CPU 的 EL1 打开四类地址认证指令。
 *
 * boot_init_stack_canary() 在 boot CPU 的 key 已装入后调用。入参：无；调用者处于
 * 不会迁移 CPU 的早期启动上下文，不持有本函数要求的锁。支持地址认证时以读改写
 * 保留 SCTLR_EL1 其他位并设置 EnIA/EnIB/EnDA/EnDB，随后 ISB 令后续指令看到新
 * 控制状态；generic authentication 没有这些使能位。
 *
 * 返回：无直接返回值，无错误码。硬件不支持时无副作用；不睡眠、不分配资源。
 * ISB 是本 CPU 的上下文同步事件，不向其他 CPU 发布内存。
 */
static __always_inline void ptrauth_enable(void)
{
	/* 能力不存在时必须避免触碰相关控制位，同一内核镜像仍可正常运行。 */
	if (!system_supports_address_auth())
		return;
	/* 先更新控制寄存器，再以 ISB 建立后续 PAC 指令可依赖的本地生效边界。 */
	sysreg_clear_set(sctlr_el1, 0, (SCTLR_ELx_ENIA | SCTLR_ELx_ENIB |
					SCTLR_ELx_ENDA | SCTLR_ELx_ENDB));
	isb();
}

/*
 * ptrauth_suspend_exit() 在 CPU resume 的架构恢复阶段重新装入 current 的用户
 * APIB/APDA/APDB/APGA，因为固件或低功耗状态可能没有保存这些寄存器。它不装 APIA，
 * 内核执行期间 APIAKey_EL1 必须继续保存内核返回地址 key。宏无返回值且不睡眠。
 */
#define ptrauth_suspend_exit()                                                 \
	ptrauth_keys_install_user(&current->thread.keys_user)

/*
 * ptrauth_thread_init_user() 在 exec 建立新用户执行上下文时调用：先随机化 current
 * 的全部受支持 key，再把四个地址 key 的 enabled 状态重置为全开，以兼容既有
 * 用户 ABI。宏内原英文“enable all keys”正是这一状态重置，而不是重新生成 key。
 * ptrauth_set_enabled_keys() 内部负责禁止抢占并在需要时更新当前 CPU 控制寄存器。
 * 宏无返回值；已由硬件能力分支保证正常路径不会产生可传播错误。
 */
#define ptrauth_thread_init_user()                                             \
	do {                                                                   \
		ptrauth_keys_init_user(&current->thread.keys_user);            \
									       \
		/* enable all keys */                                          \
		if (system_supports_address_auth())                            \
			ptrauth_set_enabled_keys(current,                      \
						 PR_PAC_ENABLED_KEYS_MASK,     \
						 PR_PAC_ENABLED_KEYS_MASK);    \
	} while (0)

/*
 * ptrauth_thread_switch_user(tsk) 在 __switch_to() 的寄存器准备阶段预装 next 的用户
 * APIB/APDA/APDB/APGA。@tsk 是调度器已稳定的非 NULL 借用指针；不获取引用、不睡眠。
 * 用户 APIA 与内核 APIA 的切换留给 entry.S/cpu_switch_to() 的专门边界。
 */
#define ptrauth_thread_switch_user(tsk)                                        \
	ptrauth_keys_install_user(&(tsk)->thread.keys_user)

#else /* CONFIG_ARM64_PTR_AUTH */
/*
 * 上方英文分支标记表示用户态 PAC 支持未编入内核。void 钩子变为空宏，查询/控制
 * 接口稳定返回 -EINVAL；宏参数不会求值，因此调用者不能依赖参数表达式副作用。
 * 这些桩保持通用 exec、调度和 prctl 代码可编译，但不提供任何认证保护。
 */
#define ptrauth_enable()
#define ptrauth_prctl_reset_keys(tsk, arg)	(-EINVAL)
#define ptrauth_set_enabled_keys(tsk, keys, enabled)	(-EINVAL)
#define ptrauth_get_enabled_keys(tsk)	(-EINVAL)
#define ptrauth_suspend_exit()
#define ptrauth_thread_init_user()
#define ptrauth_thread_switch_user(tsk)
#endif /* CONFIG_ARM64_PTR_AUTH */
/* 上方英文行尾注释说明用户 PAC 的真实实现或配置桩选择到此结束。 */

#ifdef CONFIG_ARM64_PTR_AUTH_KERNEL
/*
 * 两个 task 级包装宏把 task_struct 映射到其内嵌 keys_kernel：init 用于启动/新任务
 * 构造，switch 用于把既有快照装入 CPU。@tsk 是借用指针，宏不改变 ownership；
 * 反斜杠续行中不能插入独立注释。
 */
#define ptrauth_thread_init_kernel(tsk)					\
	ptrauth_keys_init_kernel(&(tsk)->thread.keys_kernel)
#define ptrauth_thread_switch_kernel(tsk)				\
	ptrauth_keys_switch_kernel(&(tsk)->thread.keys_kernel)
#else
/*
 * 未编入内核返回地址 PAC 时包装宏为空且不求值 @tsk，使任务创建和启动调用点无需
 * 条件编译；用户态 PAC 仍可独立启用。
 */
#define ptrauth_thread_init_kernel(tsk)
#define ptrauth_thread_switch_kernel(tsk)
#endif /* CONFIG_ARM64_PTR_AUTH_KERNEL */
/* 上方英文行尾注释说明内核 PAC 包装宏的真实实现或空桩选择到此结束。 */

#endif /* __ASM_POINTER_AUTH_H */
/* 上方英文行尾注释说明 __ASM_POINTER_AUTH_H include guard 到此结束。 */
