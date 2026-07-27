/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Arm CCA Realm Services Interface SMC ABI 常量学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件是 Linux 与 Realm Management Monitor（RMM）之间的二进制契约：
 * 定义 ABI 版本、通用状态码、SMCCC function ID、寄存器入参/返回布局以及
 * RMM 写回的 Realm 配置结构。它只描述协议，不发起 SMC；实际寄存器封装在
 * rsi_cmds.h，策略和生命周期在 rsi.c/rsi.h。
 *
 * 所有 function ID 都是 Standard Service owner、64-bit、fast SMC。fast
 * 表示调用由同步固件边界完成，并不意味着执行成本等同普通函数。寄存器中
 * 地址均是 Realm IPA/物理地址，不是 Linux 虚拟指针；调用方必须处理对齐、
 * 缓冲区可访问性、同一 CPU 状态和错误回滚。
 *
 * 该头也可能被汇编包含，所以 C 结构体放在 !__ASSEMBLER__ 条件内；宏的
 * 数值和寄存器布局则由 C/汇编共同消费。
 */
/*
 * Copyright (C) 2023 ARM Ltd.
 */

#ifndef __ASM_RSI_SMC_H_
#define __ASM_RSI_SMC_H_

#include <linux/arm-smccc.h>

/*
 * This file describes the Realm Services Interface (RSI) Application Binary
 * Interface (ABI) for SMC calls made from within the Realm to the RMM and
 * serviced by the RMM.
 */
/*
 * 本文件描述 Realm 内部发往 RMM、并由 RMM 服务的 RSI SMC 应用二进制
 * 接口。这里的 argN/retN 分别对应进入/返回时的 SMCCC xN 寄存器。
 */

/*
 * The major version number of the RSI implementation.  This is increased when
 * the binary format or semantics of the SMC calls change.
 */
/*
 * 主版本在 SMC 二进制格式或语义发生不兼容变化时递增；内核不能假定不同
 * major 的调用布局可互操作。
 */
#define RSI_ABI_VERSION_MAJOR		UL(1)

/*
 * The minor version number of the RSI implementation.  This is increased when
 * a bug is fixed, or a feature is added without breaking binary compatibility.
 */
/*
 * 次版本用于兼容性修复或向后兼容的新功能；版本协商仍需由
 * RSI_ABI_VERSION 调用确认，不能只比较编译期宏。
 */
#define RSI_ABI_VERSION_MINOR		UL(0)

/*
 * ABI 版本以高 16 位 major、低 16 位 minor 打包。GET 宏只做字段提取，
 * 参数应是已从 RMM 返回或由同一格式构造的 unsigned long。
 */
#define RSI_ABI_VERSION			((RSI_ABI_VERSION_MAJOR << 16) | \
					 RSI_ABI_VERSION_MINOR)

#define RSI_ABI_VERSION_GET_MAJOR(_version) ((_version) >> 16)
#define RSI_ABI_VERSION_GET_MINOR(_version) ((_version) & 0xFFFF)

/*
 * RSI 通用返回状态，位于 ret0/x0：
 *   SUCCESS       调用完整成功；
 *   ERROR_INPUT   参数、地址、长度或对齐不合法；
 *   ERROR_STATE   当前 Realm/RMM 状态不允许该操作；
 *   INCOMPLETE    长操作尚未完成，调用方需按协议继续；
 *   ERROR_UNKNOWN 无法归入上述类别的实现错误。
 *
 * 这些是 RSI 状态而非 Linux errno，封装层按各 API 契约选择保留或转换。
 */
#define RSI_SUCCESS		UL(0)
#define RSI_ERROR_INPUT		UL(1)
#define RSI_ERROR_STATE		UL(2)
#define RSI_INCOMPLETE		UL(3)
#define RSI_ERROR_UNKNOWN	UL(4)

/*
 * 把 RSI operation number 编码为 SMCCC function ID。预处理器续行是一个
 * 不可拆分表达式：fast call、SMC64、Standard Service owner 三项共同决定
 * RMM 的分派命名空间。
 */
#define SMC_RSI_FID(n)		ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,      \
						   ARM_SMCCC_SMC_64,         \
						   ARM_SMCCC_OWNER_STANDARD, \
						   n)

/*
 * Returns RSI version.
 *
 * arg1 == Requested interface revision
 * ret0 == Status / error
 * ret1 == Lower implemented interface revision
 * ret2 == Higher implemented interface revision
 */
/*
 * 协商 RSI 版本：x1 传入内核请求版本；x0 返回状态，x1/x2 返回 RMM 实现的
 * 最低/最高版本。调用方只在协议允许时解释区间，并据结果决定是否继续 RSI
 * 初始化。
 */
#define SMC_RSI_ABI_VERSION	SMC_RSI_FID(0x190)

/*
 * Read feature register.
 *
 * arg1 == Feature register index
 * ret0 == Status / error
 * ret1 == Feature register value
 */
/*
 * 读取按索引编号的 RSI feature register：x1 是寄存器索引，成功时 x1
 * 返回特性位图。未知索引或未实现功能通过 x0 状态报告。
 */
#define SMC_RSI_FEATURES			SMC_RSI_FID(0x191)

/*
 * Read measurement for the current Realm.
 *
 * arg1 == Index, which measurements slot to read
 * ret0 == Status / error
 * ret1 == Measurement value, bytes:  0 -  7
 * ret2 == Measurement value, bytes:  8 - 15
 * ret3 == Measurement value, bytes: 16 - 23
 * ret4 == Measurement value, bytes: 24 - 31
 * ret5 == Measurement value, bytes: 32 - 39
 * ret6 == Measurement value, bytes: 40 - 47
 * ret7 == Measurement value, bytes: 48 - 55
 * ret8 == Measurement value, bytes: 56 - 63
 */
/*
 * 读取当前 Realm 的 measurement slot。x1 选择槽位，成功时 x1..x8 按
 * 每寄存器 8 字节顺序返回最多 64 字节测量值；寄存器拼接顺序属于 ABI，
 * 上层不能按主机结构体布局猜测。
 */
#define SMC_RSI_MEASUREMENT_READ		SMC_RSI_FID(0x192)

/*
 * Extend Realm Extensible Measurement (REM) value.
 *
 * arg1  == Index, which measurements slot to extend
 * arg2  == Size of realm measurement in bytes, max 64 bytes
 * arg3  == Measurement value, bytes:  0 -  7
 * arg4  == Measurement value, bytes:  8 - 15
 * arg5  == Measurement value, bytes: 16 - 23
 * arg6  == Measurement value, bytes: 24 - 31
 * arg7  == Measurement value, bytes: 32 - 39
 * arg8  == Measurement value, bytes: 40 - 47
 * arg9  == Measurement value, bytes: 48 - 55
 * arg10 == Measurement value, bytes: 56 - 63
 * ret0  == Status / error
 */
/*
 * 扩展 Realm Extensible Measurement（REM）：x1 是槽位，x2 是有效数据
 * 字节数（最大 64），x3..x10 携带测量内容。操作会改变 Realm 可证明状态，
 * 没有独立输出缓冲区，成败只由 x0 表达。
 */
#define SMC_RSI_MEASUREMENT_EXTEND		SMC_RSI_FID(0x193)

/*
 * Initialize the operation to retrieve an attestation token.
 *
 * arg1 == Challenge value, bytes:  0 -  7
 * arg2 == Challenge value, bytes:  8 - 15
 * arg3 == Challenge value, bytes: 16 - 23
 * arg4 == Challenge value, bytes: 24 - 31
 * arg5 == Challenge value, bytes: 32 - 39
 * arg6 == Challenge value, bytes: 40 - 47
 * arg7 == Challenge value, bytes: 48 - 55
 * arg8 == Challenge value, bytes: 56 - 63
 * ret0 == Status / error
 * ret1 == Upper bound of token size in bytes
 */
/*
 * 启动 attestation token 生成：挑战值直接装入 x1..x8，每个寄存器 8 字节；
 * x0 返回状态，成功时 x1 给出 token 最大字节数，供调用者在进程上下文分配
 * 最终输出缓冲区。后续 continue 必须在同一 CPU 继续该隐式状态机。
 */
#define SMC_RSI_ATTESTATION_TOKEN_INIT		SMC_RSI_FID(0x194)

/*
 * Continue the operation to retrieve an attestation token.
 *
 * arg1 == The IPA of token buffer
 * arg2 == Offset within the granule of the token buffer
 * arg3 == Size of the granule buffer
 * ret0 == Status / error
 * ret1 == Length of token bytes copied to the granule buffer
 */
/*
 * 继续提取 attestation token：x1 是 RMM 可写 granule 的 IPA，x2 是页内
 * 字节偏移，x3 是本次可写长度；x1 返回实际写入字节数。x0 为 INCOMPLETE
 * 时调用方继续推进 offset/下一个 granule，SUCCESS 才表示整个 token 完成。
 */
#define SMC_RSI_ATTESTATION_TOKEN_CONTINUE	SMC_RSI_FID(0x195)

/*
 * 汇编只需要数值宏；以下 C 布局会被 RMM 按固定 ABI 写入，不能由编译器配置
 * 任意改变大小或对齐。
 */
#ifndef __ASSEMBLER__

/*
 * struct realm_config - RMM 返回的当前 Realm 配置页。
 *
 * 由 ARM64 启动代码静态分配并把 IPA 交给 RSI_REALM_CONFIG；RMM 写入，内核
 * 在初始化后只读。第一段 0x200 字节目前公开 IPA 位宽和 measurement hash
 * 算法，其余通过 pad 保留 ABI 扩展空间；第二段保存 64 字节 Realm
 * Personalization Value，并扩展到整个 4 KiB granule。
 *
 * union 保证已命名字段与固定 ABI 大小共享同一起点；调用者不应把 padding
 * 当作已定义数据，也不取得 RMM 内部对象引用。
 */
struct realm_config {
	union {
		/* ipa_bits 是 IPA 有效位宽；hash_algo 标识 measurement 哈希算法。 */
		struct {
			unsigned long ipa_bits; /* Width of IPA in bits */
			unsigned long hash_algo; /* Hash algorithm */
		};
		u8 pad[0x200];
	};
	union {
		/* rpv 是 Realm 个性化值；pad2 把结构补足为单个 RSI granule。 */
		u8 rpv[64]; /* Realm Personalization Value */
		u8 pad2[0xe00];
	};
	/*
	 * The RMM requires the configuration structure to be aligned to a 4k
	 * boundary, ensure this happens by aligning this structure.
	 */
	/*
	 * RMM 要求配置结构位于 4 KiB 边界，因此类型本身强制 0x1000 对齐；
	 * 配合两个 union 的固定大小，任何该类型对象都满足单 granule ABI。
	 */
} __aligned(0x1000);

#endif /* __ASSEMBLER__ */

/*
 * Read configuration for the current Realm.
 *
 * arg1 == struct realm_config addr
 * ret0 == Status / error
 */
/*
 * 读取当前 Realm 配置：x1 指向可由 RMM 写入的 struct realm_config IPA；
 * x0 返回状态。缓冲区的分配、对齐、地址转换和写回后的只读生命周期均由
 * Linux 调用方负责。
 */
#define SMC_RSI_REALM_CONFIG			SMC_RSI_FID(0x196)

/*
 * Request RIPAS of a target IPA range to be changed to a specified value.
 *
 * arg1 == Base IPA address of target region
 * arg2 == Top of the region
 * arg3 == RIPAS value
 * arg4 == flags
 * ret0 == Status / error
 * ret1 == Top of modified IPA range
 * ret2 == Whether the Host accepted or rejected the request
 */
/*
 * 请求修改 [base,top) IPA 的 RIPAS：x1/x2 是半开范围，x3 是目标状态，
 * x4 控制 DESTROYED 是否允许转换；x1 返回本次实际推进到的 top，x2 表示
 * 非可信 Host 是否接受。RMM 成功与 Host 接受是两个独立条件，封装层必须
 * 同时检查。
 */
#define SMC_RSI_IPA_STATE_SET			SMC_RSI_FID(0x197)

/*
 * DESTROYED 策略：NO_CHANGE 用于启动时安全接纳 RAM，拒绝把遭 Host 破坏的
 * 页重新包装成可信内容；CHANGE 用于调用方已经决定丢弃旧内容的显式转换。
 */
#define RSI_NO_CHANGE_DESTROYED			UL(0)
#define RSI_CHANGE_DESTROYED			UL(1)

/* Host 对 IPA 状态转换请求的独立接受/拒绝结果，位于 ret2/x2。 */
#define RSI_ACCEPT				UL(0)
#define RSI_REJECT				UL(1)

/*
 * Get RIPAS of a target IPA range.
 *
 * arg1 == Base IPA of target region
 * arg2 == End of target IPA region
 * ret0 == Status / error
 * ret1 == Top of IPA region which has the reported RIPAS value
 * ret2 == RIPAS value
 */
/*
 * 查询 [base,end) 的 RIPAS。成功时 x1 返回从 base 开始保持同一状态的
 * 排他末端，x2 返回该段 RIPAS；调用方据此分段推进并防御 top 不前进。
 */
#define SMC_RSI_IPA_STATE_GET			SMC_RSI_FID(0x198)

/*
 * Make a Host call.
 *
 * arg1 == IPA of host call structure
 * ret0 == Status / error
 */
/*
 * 发起 Realm 到 Host 的显式调用：x1 指向共享的 host-call 结构 IPA，
 * x0 返回协议状态。结构必须先按共享内存规则发布，RMM 不接收普通内核
 * 虚拟地址。
 */
#define SMC_RSI_HOST_CALL			SMC_RSI_FID(0x199)

#endif /* __ASM_RSI_SMC_H_ */
