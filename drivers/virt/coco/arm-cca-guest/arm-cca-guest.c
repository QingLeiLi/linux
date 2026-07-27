// SPDX-License-Identifier: GPL-2.0-only
/*
 * Arm CCA Realm guest attestation 驱动学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本模块把 RMM 的 RSI attestation token 状态机接入通用 TSM configfs：
 *
 *   用户提交 challenge 并读取 report/outblob
 *       -> TSM core 调 arm_cca_report_new()
 *       -> 在选定 CPU 执行 RSI_ATTESTATION_TOKEN_INIT
 *       -> 按返回上界分配最终 token buffer
 *       -> 分配物理连续 4 KiB granule 作为 RMM bounce buffer
 *       -> 始终在同一 CPU 多轮执行 TOKEN_CONTINUE
 *       -> 分页复制并把 outblob ownership 转给 TSM core
 *
 * INIT/CONTINUE 的生成进度是 RMM 隐式的 per-CPU 状态，驱动用同步
 * smp_call_function_single() 明确把每次 SMC 派发到同一逻辑 CPU。最终 token
 * 可能大于一页且非物理连续，RMM 只能直接写单个 granule，因此需要 bounce
 * page 和复制开销。
 *
 * 并发由 TSM core 的 tsm_rwsem 串行化 provider/report 生成；本文件没有
 * 自有锁。临时 token 使用 cleanup class 自动 kvfree，成功时 no_free_ptr()
 * 显式转移给 report；bounce granule 在统一出口释放。模块只在
 * is_realm_world() 为真时注册为唯一 TSM report provider。
 */
/*
 * Copyright (C) 2023 ARM Ltd.
 */

#include <linux/arm-smccc.h>
#include <linux/cc_platform.h>
#include <linux/kernel.h>
#include <linux/device-id/platform.h>
#include <linux/module.h>
#include <linux/smp.h>
#include <linux/tsm.h>
#include <linux/types.h>

#include <asm/rsi.h>

/**
 * struct arm_cca_token_info - a descriptor for the token buffer.
 * @challenge:		Pointer to the challenge data
 * @challenge_size:	Size of the challenge data
 * @granule:		PA of the granule to which the token will be written
 * @offset:		Offset within granule to start of buffer in bytes
 * @result:		result of rsi_attestation_token_continue operation
 */
/*
 * struct arm_cca_token_info - 跨 CPU 回调共享的一次 token 生成游标。
 *
 * 对象由 arm_cca_report_new() 在栈上创建；每次同步
 * smp_call_function_single(..., wait=true) 期间目标 CPU 只借用其指针，
 * 回调返回后不再持有，所以栈生命周期足够且不需要引用计数。
 *
 * challenge/challenge_size 是 INIT 阶段输入；granule/offset 是 CONTINUE
 * 传输游标；result 在 INIT 后暂存 token 最大大小，在 CONTINUE 阶段改为
 * RSI 状态。调用线程与目标 CPU 通过同步 IPI 顺序访问字段，没有并发读写。
 */
struct arm_cca_token_info {
	/* 借用 TSM descriptor 中的 challenge；驱动从不释放。 */
	void           *challenge;
	/* challenge 的有效字节数，合法范围 32..64。 */
	unsigned long   challenge_size;
	/* 物理连续 bounce granule 的 Realm IPA，按 4 KiB 对齐。 */
	phys_addr_t     granule;
	/* 当前 bounce granule 内下一次写入位置，单位字节。 */
	unsigned long   offset;
	/* INIT 大小上界或最近一次 CONTINUE 的 RSI 状态，按阶段复用。 */
	unsigned long   result;
};

/*
 * arm_cca_attestation_init() - 在指定 CPU 启动 RMM token 生成状态机。
 *
 * @param：不可为 NULL，指向调用线程栈上的 arm_cca_token_info；目标 CPU
 * 仅在同步回调期间借用并写 result，不取得 ownership。
 *
 * 由 smp_call_function_single() 在 IPI/不可睡眠上下文执行，不持本驱动锁。
 * 返回类型为 void，成功的 token 大小上界或失败的负 errno 经 info->result
 * 带回等待中的调用线程。
 */
static void arm_cca_attestation_init(void *param)
{
	/* info 是同步跨 CPU 参数；显式类型转换不改变其栈对象 ownership。 */
	struct arm_cca_token_info *info;

	info = (struct arm_cca_token_info *)param;

	/* 此调用在目标 CPU 建立后续 CONTINUE 必须匹配的隐式 RMM 状态。 */
	info->result = rsi_attestation_token_init(info->challenge,
						  info->challenge_size);
}

/**
 * arm_cca_attestation_continue - Retrieve the attestation token data.
 *
 * @param: pointer to the arm_cca_token_info
 *
 * Attestation token generation is a long running operation and therefore
 * the token data may not be retrieved in a single call. Moreover, the
 * token retrieval operation must be requested on the same CPU on which the
 * attestation token generation was initialised.
 * This helper function is therefore scheduled on the same CPU multiple
 * times until the entire token data is retrieved.
 */
/*
 * 生成 token 是长操作，一次调用可能取不完；提取还必须发生在初始化它的
 * 同一 CPU，因此主路径反复把本 helper 调度到固定 CPU。
 *
 * @param：借用的 arm_cca_token_info，不可为 NULL。入口时 granule 指向
 * RMM 可写的 4 KiB IPA，offset 位于页内；回调计算剩余 size，执行一次
 * CONTINUE，把 RSI 状态写入 result，并按 RMM 返回 len 推进 offset。
 *
 * 同步 IPI 上下文不可睡眠、不分配。返回类型为 void；所有结果通过 info
 * 字段传回，函数不取得 bounce page ownership。
 */
static void arm_cca_attestation_continue(void *param)
{
	/*
	 * len 是本次 RMM 写入字节数；size 是 granule 从 offset 起的剩余容量；
	 * info 指向调用线程仍在等待的栈对象。
	 */
	unsigned long len;
	unsigned long size;
	struct arm_cca_token_info *info;

	info = (struct arm_cca_token_info *)param;

	/* 每次只开放当前 4 KiB granule 的剩余部分，禁止跨越 bounce page。 */
	size = RSI_GRANULE_SIZE - info->offset;
	info->result = rsi_attestation_token_continue(info->granule,
						      info->offset, size, &len);
	/* 外围据 RSI 状态决定继续当前页还是复制后换下一页。 */
	info->offset += len;
}

/**
 * arm_cca_report_new - Generate a new attestation token.
 *
 * @report: pointer to the TSM report context information.
 * @data:  pointer to the context specific data for this module.
 *
 * Initialise the attestation token generation using the challenge data
 * passed in the TSM descriptor. Allocate memory for the attestation token
 * and schedule calls to retrieve the attestation token on the same CPU
 * on which the attestation token generation was initialised.
 *
 * The challenge data must be at least 32 bytes and no more than 64 bytes. If
 * less than 64 bytes are provided it will be zero padded to 64 bytes.
 *
 * Return:
 * * %0        - Attestation token generated successfully.
 * * %-EINVAL  - A parameter was not valid.
 * * %-ENOMEM  - Out of memory.
 * * %-EFAULT  - Failed to get IPA for memory page(s).
 * * A negative status code as returned by smp_call_function_single().
 */
/*
 * 根据 TSM challenge 同步生成一份完整 CCA attestation token。
 *
 * @report：借用的 TSM report 上下文，不可为 NULL。desc.inblob 是输入；
 * 成功时本函数分配 outblob 并把 ownership 转给 report/TSM core，后者最终
 * kvfree。bounce page 分配后的失败把 outblob_len 置为 0；更早的失败不修改
 * report 输出字段，调用它的 TSM core 已先清空 outblob 指针。
 * @data：TSM provider 私有数据；本模块注册时传 NULL，本函数不读取也不持有。
 *
 * 调用者是 TSM report 生成慢路径，当前为可睡眠进程上下文。函数选定一个
 * CPU，并通过 wait=true 同步 IPI 保证 INIT 和所有 CONTINUE 都在那里执行。
 *
 * 成功返回 0。参数错误返回 -EINVAL，分配失败返回 -ENOMEM，RMM CONTINUE
 * 状态错误转换为 -ENXIO，IPI/CPU hotplug 错误原样返回。英文契约还列出
 * -EFAULT；当前实现的 virt_to_phys() 路径没有该错误出口。临时 token 由
 * cleanup class 自动回收；bounce granule 分配后始终走统一释放出口。
 */
static int arm_cca_report_new(struct tsm_report *report, void *data)
{
	/*
	 * 变量地图：
	 *   ret        最近一次 IPI/最终 Linux 返回码；
	 *   cpu        整个 RMM token 状态机绑定的逻辑 CPU；
	 *   max_size   INIT 返回的最终 token 字节上界；
	 *   token_size 已复制到最终 token 的累计字节数；
	 *   info       同步 IPI 参数和当前 granule 游标；
	 *   buf        物理连续 bounce granule，需显式 free_pages_exact；
	 *   token      最终输出，离开作用域自动 kvfree，成功时取消自动释放；
	 *   desc       借用 report 内嵌输入描述，不持有额外引用。
	 */
	int ret;
	int cpu;
	long max_size;
	unsigned long token_size = 0;
	struct arm_cca_token_info info;
	void *buf;
	u8 *token __free(kvfree) = NULL;
	struct tsm_report_desc *desc = &report->desc;

	/*
	 * 阶段 1：RSI 接受 32..64 字节 challenge；短输入由底层零初始化的
	 * SMCCC 寄存器补到 64 字节，输入内容仍直接借用 desc->inblob。
	 */
	if (desc->inblob_len < 32 || desc->inblob_len > 64)
		return -EINVAL;

	/*
	 * The attestation token 'init' and 'continue' calls must be
	 * performed on the same CPU. smp_call_function_single() is used
	 * instead of simply calling get_cpu() because of the need to
	 * allocate outblob based on the returned value from the 'init'
	 * call and that cannot be done in an atomic context.
	 */
	/*
	 * INIT 和 CONTINUE 必须位于同一 CPU。不能用 get_cpu() 包住全过程，
	 * 因为它会禁用抢占，而 INIT 返回大小后必须执行可能睡眠的内存分配。
	 * 这里记录一个 CPU 编号，每次短 SMC 都通过同步 IPI 投递过去，中间阶段
	 * 允许当前任务睡眠。
	 */
	cpu = smp_processor_id();

	/* INIT 回调只消费 challenge 两字段，其余 info 字段稍后再初始化。 */
	info.challenge = desc->inblob;
	info.challenge_size = desc->inblob_len;

	/*
	 * wait=true 保证返回时目标 CPU 已完成 SMC 且 info.result 可读；派发失败
	 * 时 RMM 状态未按本路径可靠建立，直接把 errno 交给 TSM core。
	 */
	ret = smp_call_function_single(cpu, arm_cca_attestation_init,
				       &info, true);
	if (ret)
		return ret;
	/* INIT 阶段把 result 重新解释为带符号大小上界；负 errno/零大小均拒绝。 */
	max_size = info.result;

	if (max_size <= 0)
		return -EINVAL;

	/* Allocate outblob */
	/*
	 * 按 RMM 上界分配零初始化输出。kvzalloc 可退化到 vmalloc，所以虚拟连续
	 * 但不保证物理连续；__free(kvfree) 覆盖两种来源和所有提前返回。
	 */
	token = kvzalloc(max_size, GFP_KERNEL);
	if (!token)
		return -ENOMEM;

	/*
	 * Since the outblob may not be physically contiguous, use a page
	 * to bounce the buffer from RMM.
	 */
	/*
	 * 最终 outblob 可能不是物理连续，不能直接交给 RMM。额外分配一个恰好
	 * RSI_GRANULE_SIZE 的物理连续页作 bounce buffer；token 自动 cleanup
	 * 已经生效，后续失败不会泄漏它。
	 */
	buf = alloc_pages_exact(RSI_GRANULE_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* Get the PA of the memory page(s) that were allocated */
	/*
	 * RMM 接收 Realm IPA 而非内核 VA；buf 是 direct-map 地址，可用
	 * virt_to_phys() 取得 granule 首地址。页面仍由本函数拥有。
	 */
	info.granule = (unsigned long)virt_to_phys(buf);

	/* Loop until the token is ready or there is an error */
	/* 阶段 2：外层循环按 bounce granule 逐页组装最终 token。 */
	do {
		/* Retrieve one RSI_GRANULE_SIZE data per loop iteration */
		/* 每个 granule 轮次从页首写入，offset 记录本页累计有效字节数。 */
		info.offset = 0;
		do {
			/*
			 * Schedule a call to retrieve a sub-granule chunk
			 * of data per loop iteration.
			 */
			/*
			 * 一次 CONTINUE 可能只填页内一段。同步 IPI 始终投递到 INIT
			 * 所选 cpu；wait=true 使 result/offset 在返回后稳定可读。
			 */
			ret = smp_call_function_single(cpu,
						       arm_cca_attestation_continue,
						       (void *)&info, true);
			if (ret != 0) {
				/* CPU 派发失败后清零可见长度，并从统一出口回收 bounce 页。 */
				token_size = 0;
				goto exit_free_granule_page;
			}
		} while (info.result == RSI_INCOMPLETE &&
			 info.offset < RSI_GRANULE_SIZE);

		/* Break out in case of failure */
		/*
		 * 只有 SUCCESS 和 INCOMPLETE 是可组装状态；其他 RMM 状态统一转为
		 * -ENXIO，避免向通用 TSM API 泄漏 RSI 私有状态码。
		 */
		if (info.result != RSI_SUCCESS && info.result != RSI_INCOMPLETE) {
			ret = -ENXIO;
			token_size = 0;
			goto exit_free_granule_page;
		}

		/*
		 * Copy the retrieved token data from the granule
		 * to the token buffer, ensuring that the RMM doesn't
		 * overflow the buffer.
		 */
		/*
		 * INIT 给出的 max_size 是 RMM 承诺上界。复制前用累计长度加本页
		 * 产量验证该承诺，防止错误返回长度写越 token。WARN 分支不复制
		 * 越界块并跳出；当前代码随后仍以最近一次 ret 发布已累计前缀，
		 * 因而该告警代表“不应发生”的 RMM 契约违例，而非普通可恢复分支。
		 * 正常路径复制有效前缀并推进累计量。
		 */
		if (WARN_ON(token_size + info.offset > max_size))
			break;
		memcpy(&token[token_size], buf, info.offset);
		token_size += info.offset;
	} while (info.result == RSI_INCOMPLETE);

	/*
	 * 阶段 3：把最终 buffer ownership 提交给 TSM report。no_free_ptr()
	 * 取消 token 的作用域自动 kvfree；TSM core 在报告失效/销毁时负责释放。
	 */
	report->outblob = no_free_ptr(token);
exit_free_granule_page:
	/*
	 * 统一清理点只在 bounce page 分配成功后可达。长度与返回状态一起发布；
	 * 失败路径已把 token_size 清零且未转移 token，自动 cleanup 随后执行。
	 */
	report->outblob_len = token_size;
	free_pages_exact(buf, RSI_GRANULE_SIZE);
	return ret;
}

/*
 * TSM provider 的静态操作表。TSM core 注册成功后长期借用；name 暴露为
 * configfs provider，report_new 是唯一生成回调。本驱动不提供额外
 * privilege、auxblob 或 manifest 定制。
 */
static const struct tsm_report_ops arm_cca_tsm_ops = {
	.name = KBUILD_MODNAME,
	.report_new = arm_cca_report_new,
};

/**
 * arm_cca_guest_init - Register with the Trusted Security Module (TSM)
 * interface.
 *
 * Return:
 * * %0        - Registered successfully with the TSM interface.
 * * %-ENODEV  - The execution context is not an Arm Realm.
 * * %-EBUSY   - Already registered.
 */
/*
 * 在 Realm 中注册唯一 TSM report provider。
 *
 * 入参：无。模块加载时调用，处于可睡眠进程上下文，不持本驱动锁。
 * is_realm_world() 为 false 返回 -ENODEV；Realm 中把静态 ops 和 NULL 私有
 * 数据交给 TSM core。成功返回 0；已有 provider 或 configfs report 实例时
 * 返回 -EBUSY，其他负 errno 原样返回并记录日志。
 *
 * 注册成功是发布边界：之后用户读取 report 可进入 arm_cca_report_new()；
 * 模块卸载必须用同一 ops 摘除 provider。
 */
static int __init arm_cca_guest_init(void)
{
	/* ret 保存 TSM 注册结果；失败时 ops 尚未发布，无本地资源需要回滚。 */
	int ret;

	/* 普通 ARM64 guest/host 没有 RSI 状态机，不能暴露伪 attestation provider。 */
	if (!is_realm_world())
		return -ENODEV;

	/* NULL 表示 report_new() 的 data 参数没有 provider 私有对象。 */
	ret = tsm_report_register(&arm_cca_tsm_ops, NULL);
	if (ret < 0)
		pr_err("Error %d registering with TSM\n", ret);

	return ret;
}
/* 模块加载入口；内建配置下对应启动期 initcall。 */
module_init(arm_cca_guest_init);

/**
 * arm_cca_guest_exit - unregister with the Trusted Security Module (TSM)
 * interface.
 */
/*
 * 从 TSM core 摘除本模块的静态 ops，使新的报告生成不再进入模块代码。
 * 入参和直接返回值均无；模块卸载上下文可睡眠。TSM core 用写侧 rwsem 与
 * provider 选择路径串行化；已有 report outblob 仍由 report 对象管理。
 * tsm_report_unregister() 的返回码受 void 模块退出接口限制而被忽略。
 */
static void __exit arm_cca_guest_exit(void)
{
	tsm_report_unregister(&arm_cca_tsm_ops);
}
/* 模块卸载入口，与成功的 module_init 注册配对。 */
module_exit(arm_cca_guest_exit);

/* modalias, so userspace can autoload this module when RSI is available */
/*
 * platform ID 只生成 modalias，让 rsi.c 发布 RSI_PDEV_NAME 虚拟设备时用户
 * 空间可自动加载本模块；本模块不注册 platform_driver，实际初始化仍由
 * module_init 完成。__maybe_unused 避免内建场景的未使用告警。
 */
static const struct platform_device_id arm_cca_match[] __maybe_unused = {
	/* 第一项匹配 Realm RSI 虚拟设备，空项终止设备 ID 表。 */
	{ RSI_PDEV_NAME, 0},
	{ }
};

/* 导出 platform alias；以下元数据声明模块作者、功能和 GPL 许可证。 */
MODULE_DEVICE_TABLE(platform, arm_cca_match);
MODULE_AUTHOR("Sami Mujawar <sami.mujawar@arm.com>");
MODULE_DESCRIPTION("Arm CCA Guest TSM Driver");
MODULE_LICENSE("GPL");
