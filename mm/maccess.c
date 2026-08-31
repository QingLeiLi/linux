// SPDX-License-Identifier: GPL-2.0-only
/*
 * Access kernel or user memory without faulting.
 */
/*
 * 本文件提供“尽力探测、绝不把缺页异常扩散给调用者”的内核/用户地址访问原语，
 * 供调试器、跟踪器、BPF 和诊断路径在地址可能失效时安全取样。它们只保证异常被
 * 折叠为错误码，不会固定映射或对象生命周期；并发释放仍可能使复制失败或得到旧快照。
 */
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <asm/tlb.h>

/*
 * copy_from_kernel_nofault_allowed() - 让体系结构过滤不能安全探测的内核地址区间。
 *
 * 业务背景：通用无故障读取在真正触碰地址前调用本钩子；体系结构可提供强定义覆盖
 * 这个弱实现，阻止会绕过地址空间规则或触发不可恢复异常的访问。
 * 入参：unsafe_src 是只借用、不保证有效的源地址；size 是拟读取的字节数，可为 0。
 * 出参/返回：true 表示允许继续尝试，并不承诺地址可读；false 由上层转成 -ERANGE。
 * 注意事项：不取得引用、不访问数据、不睡眠；默认实现无副作用，安全策略由架构覆盖。
 */
bool __weak copy_from_kernel_nofault_allowed(const void *unsafe_src,
		size_t size)
{
	return true;
}

/*
 * The below only uses kmsan_check_memory() to ensure uninitialized kernel
 * memory isn't leaked.
 */
/*
 * 下列读取循环调用 kmsan_check_memory() 的唯一目的，是在把内核数据交给目标缓冲区前
 * 阻止未初始化内容泄漏；它不让 KMSAN 参与地址可访问性判断。宏按 type 宽度重复复制，
 * 任一底层取值异常便跳到调用者的 err_label；dst/src/len 都是会被原地推进的宏实参。
 */
#define copy_from_kernel_nofault_loop(dst, src, len, type, err_label)	\
	while (len >= sizeof(type)) {					\
		__get_kernel_nofault(dst, src, type, err_label);	\
		kmsan_check_memory(src, sizeof(type));			\
		dst += sizeof(type);					\
		src += sizeof(type);					\
		len -= sizeof(type);					\
	}

/*
 * copy_from_kernel_nofault() - 从不可信内核地址复制固定长度数据并吸收访问异常。
 *
 * 业务背景：KGDB、BPF 与诊断代码在对象可能并发变化时用它取得尽力快照；本函数位于
 * 调用者和体系结构 __get_kernel_nofault() 异常表实现之间。
 * 入参：dst 是调用者拥有且至少 size 字节的可写内核缓冲区；src 是仅借用、可能无效的
 * 内核地址；size 为字节数，允许 0，二者所有权均不转移。
 * 出参/返回：完整复制返回 0；架构策略拒绝返回 -ERANGE；任一访问异常返回 -EFAULT，
 * 此时 dst 可能已经包含前缀数据。
 * 注意事项：临时禁用 page fault，不能靠缺页补齐映射，也不稳定 src 生命周期；函数不
 * 睡眠。宽度选择只优化访问次数，最终 u8 循环处理余数。
 */
long copy_from_kernel_nofault(void *dst, const void *src, size_t size)
{
	unsigned long align = 0;

	/* 无高效非对齐访问时，把两端地址的低位合并为可用宽度约束。 */
	if (!IS_ENABLED(CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS))
		align = (unsigned long)dst | (unsigned long)src;

	/* 架构拒绝的区间不应尝试触碰，与实际 fault 使用不同错误码。 */
	if (!copy_from_kernel_nofault_allowed(src, size))
		return -ERANGE;

	/* 从最大安全宽度依次消费；每个宏会同步推进指针并缩短剩余 size。 */
	pagefault_disable();
	if (!(align & 7))
		copy_from_kernel_nofault_loop(dst, src, size, u64, Efault);
	if (!(align & 3))
		copy_from_kernel_nofault_loop(dst, src, size, u32, Efault);
	/* 若仍满足 2 字节对齐则继续批量取 u16，最后总由 u8 收尾。 */
	if (!(align & 1))
		copy_from_kernel_nofault_loop(dst, src, size, u16, Efault);
	copy_from_kernel_nofault_loop(dst, src, size, u8, Efault);
	pagefault_enable();
	return 0;
Efault:
	/* 异常表跳转到这里时必须恢复当前任务的 pagefault 状态。 */
	pagefault_enable();
	return -EFAULT;
}

/* 只导出读取接口给 GPL 模块；写接口主要留给内核内建调试/补丁路径。 */
EXPORT_SYMBOL_GPL(copy_from_kernel_nofault);

/*
 * copy_to_kernel_nofault_loop 的结构与读取宏对称；__put_kernel_nofault() 成功写入
 * 每个 type 后再调用 instrument_write()，让 KASAN 等工具观察目标内存写事件。
 * 宏会推进 dst/src/len，异常时直接跳到外层函数的 err_label。
 */
#define copy_to_kernel_nofault_loop(dst, src, len, type, err_label)	\
	while (len >= sizeof(type)) {					\
		__put_kernel_nofault(dst, src, type, err_label);	\
		instrument_write(dst, sizeof(type));			\
		dst += sizeof(type);					\
		src += sizeof(type);					\
		len -= sizeof(type);					\
	}

/*
 * copy_to_kernel_nofault() - 向可能不可写的内核地址尽力复制固定长度数据。
 *
 * 业务背景：内核调试器、代码补丁等路径需要把已准备的数据写到待验证地址，本函数将
 * 体系结构异常表、页故障禁用和内存检测插桩封装为统一边界。
 * 入参：dst 是只借用、可能无效的目标内核地址；src 是至少 size 字节的可读内核缓冲区；
 * size 为字节数，可为 0，双方 ownership 均不改变。
 * 出参/返回：全部写入返回 0；异常返回 -EFAULT，目标可能已经被部分修改。
 * 注意事项：不保证原子性，也不回滚已写前缀；不触发可睡眠缺页处理。调用者必须另行
 * 保证写权限、文本修改同步和目标生命周期。
 */
long copy_to_kernel_nofault(void *dst, const void *src, size_t size)
{
	unsigned long align = 0;

	/* 仅在体系结构需要时根据源、目标共同对齐选择安全访问宽度。 */
	if (!IS_ENABLED(CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS))
		align = (unsigned long)dst | (unsigned long)src;

	/* 先尝试大宽度，再逐级处理不能整除的尾部。 */
	pagefault_disable();
	if (!(align & 7))
		copy_to_kernel_nofault_loop(dst, src, size, u64, Efault);
	if (!(align & 3))
		copy_to_kernel_nofault_loop(dst, src, size, u32, Efault);
	/* 逐级缩小访问粒度，同时保持每次写入都符合两端对齐限制。 */
	if (!(align & 1))
		copy_to_kernel_nofault_loop(dst, src, size, u16, Efault);
	copy_to_kernel_nofault_loop(dst, src, size, u8, Efault);
	pagefault_enable();
	return 0;
Efault:
	/* 部分写入不可撤销；这里只恢复 fault 状态并报告失败。 */
	pagefault_enable();
	return -EFAULT;
}

/*
 * strncpy_from_kernel_nofault() - 从不可信内核地址取得有界 NUL 字符串。
 *
 * 业务背景：跟踪/BPF 等诊断路径需要读取可能已经失效的内核字符串；它逐字节探测，
 * 在异常或截断时仍尽量给调用者一个可终止的目标缓冲区。
 * 入参：dst 是调用者拥有且至少 count 字节的内核缓冲区；unsafe_addr 是只借用的源；
 * count 是含终止 NUL 在内的最大字节数，非正值表示不访问。
 * 出参/返回：成功返回已消费字节数（包含读到或补写的 NUL）；策略拒绝返回 -ERANGE；
 * fault 返回 -EFAULT，并在当前 dst 位置写 NUL，之前复制的前缀保留。
 * 注意事项：不睡眠、不固定源对象；并发修改可产生不一致字符串，调用者须按返回值判定。
 */
long strncpy_from_kernel_nofault(char *dst, const void *unsafe_addr, long count)
{
	const void *src = unsafe_addr;

	/* 空容量不能写终止符，因此直接以 0 表示未消费数据。 */
	if (unlikely(count <= 0))
		return 0;
	if (!copy_from_kernel_nofault_allowed(unsafe_addr, count))
		return -ERANGE;

	/* 逐字节读取，遇 NUL 或达到 count 即结束，避免越过调用者缓冲区。 */
	pagefault_disable();
	do {
		__get_kernel_nofault(dst, src, u8, Efault);
		dst++;
		src++;
	} while (dst[-1] && src - unsafe_addr < count);
	pagefault_enable();

	/* 达到上限时覆盖最后一字节，保证 count>0 的成功结果总是 NUL 结尾。 */
	dst[-1] = '\0';
	return src - unsafe_addr;
Efault:
	/* dst 已推进到未成功读取的位置，在那里补 NUL 保留安全前缀。 */
	pagefault_enable();
	dst[0] = '\0';
	return -EFAULT;
}

/**
 * copy_from_user_nofault(): safely attempt to read from a user-space location
 * @dst: pointer to the buffer that shall take the data
 * @src: address to read from. This must be a user address.
 * @size: size of the data chunk
 *
 * Safely read from user address @src to the buffer at @dst. If a kernel fault
 * happens, handle that and return -EFAULT.
 */
/*
 * 安全地尝试从用户地址 @src 读取 @size 字节到内核缓冲区 @dst；若发生内核访问异常，
 * 则捕获异常并返回 -EFAULT。这里的“安全”只指不传播 fault，不代表用户数据快照稳定。
 *
 * 业务背景：NMI、跟踪和 BPF 路径不能进入常规可睡眠 uaccess，本函数在地址范围/NMI
 * 上下文校验后调用 inatomic 复制。
 * 入参：dst 是至少 size 字节的可写内核缓冲区；src 是 __user 借用地址；size 为字节数。
 * 出参/返回：完整复制返回 0；范围、NMI 限制或任何未复制字节统一返回 -EFAULT，dst 可能
 * 已含前缀。所有权不转移。
 * 注意事项：pagefault_disable() 期间不睡眠、不补页；用户可并发修改数据，不能据此完成
 * 安全决策，调用者需要更高层同步或二次校验。
 */
long copy_from_user_nofault(void *dst, const void __user *src, size_t size)
{
	long ret = -EFAULT;

	/* 先拒绝越出用户地址范围的区间，避免进入体系结构复制器。 */
	if (!__access_ok(src, size))
		return ret;

	/* 某些体系结构的当前 NMI 状态不允许访问用户地址。 */
	if (!nmi_uaccess_okay())
		return ret;

	/* inatomic helper 返回未复制字节数；任何非零余量都折叠为 -EFAULT。 */
	pagefault_disable();
	ret = __copy_from_user_inatomic(dst, src, size);
	pagefault_enable();

	if (ret)
		return -EFAULT;
	return 0;
}
EXPORT_SYMBOL_GPL(copy_from_user_nofault);

/**
 * copy_to_user_nofault(): safely attempt to write to a user-space location
 * @dst: address to write to
 * @src: pointer to the data that shall be written
 * @size: size of the data chunk
 *
 * Safely write to address @dst from the buffer at @src.  If a kernel fault
 * happens, handle that and return -EFAULT.
 */
/*
 * 安全地尝试把内核缓冲区 @src 的 @size 字节写入用户地址 @dst；访问异常被转换为
 * -EFAULT。目标区可能在失败前已被部分修改，因此该接口不提供事务语义。
 *
 * 业务背景：不能睡眠的跟踪/诊断路径用它执行尽力用户写，底层由 inatomic uaccess
 * 完成真实复制。
 * 入参：dst 是 __user 借用目标，src 是至少 size 字节的只读内核缓冲区，size 为字节数。
 * 出参/返回：全部写完返回 0；范围无效或存在未复制字节返回 -EFAULT；ownership 不变。
 * 注意事项：关闭缺页期间不可补入用户页，且用户映射可并发变化；部分写入不回滚。
 */
long copy_to_user_nofault(void __user *dst, const void *src, size_t size)
{
	long ret = -EFAULT;

	/* 只有整个目标区间通过用户地址校验才进入不可睡眠复制阶段。 */
	if (access_ok(dst, size)) {
		pagefault_disable();
		ret = __copy_to_user_inatomic(dst, src, size);
		pagefault_enable();
	}

	if (ret)
		return -EFAULT;
	return 0;
}
EXPORT_SYMBOL_GPL(copy_to_user_nofault);

/**
 * strncpy_from_user_nofault: - Copy a NUL terminated string from unsafe user
 *				address.
 * @dst:   Destination address, in kernel space.  This buffer must be at
 *         least @count bytes long.
 * @unsafe_addr: Unsafe user address.
 * @count: Maximum number of bytes to copy, including the trailing NUL.
 *
 * Copies a NUL-terminated string from unsafe user address to kernel buffer.
 *
 * On success, returns the length of the string INCLUDING the trailing NUL.
 *
 * If access fails, returns -EFAULT (some data may have been copied
 * and the trailing NUL added).
 *
 * If @count is smaller than the length of the string, copies @count-1 bytes,
 * sets the last byte of @dst buffer to NUL and returns @count.
 */
/*
 * 从不可信用户地址复制 NUL 结尾字符串到内核 @dst；成功返回值包含尾随 NUL。
 * 访问失败返回 -EFAULT，此时可能已有数据复制并补上 NUL；字符串超过容量时复制
 * @count-1 字节、把最后一字节置 NUL，并返回 @count。
 *
 * 业务背景：跟踪/BPF 等原子上下文需要常规 strncpy_from_user() 的字符串语义，但不能
 * 允许缺页睡眠，因此在调用期间关闭 page fault。
 * 入参：dst 是至少 count 字节的可写内核缓冲区；unsafe_addr 是 __user 借用源；count
 * 是包括终止符的最大字节数，非正值不访问。
 * 出参/返回：0 表示 count 无效；正值含终止 NUL，截断时为 count；负值为底层错误。
 * 注意事项：所有权不变，失败可留下部分内容；用户并发写可能使所得字符串不一致。
 */
long strncpy_from_user_nofault(char *dst, const void __user *unsafe_addr,
			      long count)
{
	long ret;

	if (unlikely(count <= 0))
		return 0;

	/* 常规 helper 的 fault 被异常表吸收，关闭 page fault 阻止进入可睡眠补页。 */
	pagefault_disable();
	ret = strncpy_from_user(dst, unsafe_addr, count);
	pagefault_enable();

	/* 把常规 helper 的“不含 NUL 长度”统一成此 API 的“包含 NUL 长度”。 */
	if (ret >= count) {
		ret = count;
		dst[ret - 1] = '\0';
	} else if (ret >= 0) {
		ret++;
	}

	return ret;
}

/**
 * strnlen_user_nofault: - Get the size of a user string INCLUDING final NUL.
 * @unsafe_addr: The string to measure.
 * @count: Maximum count (including NUL)
 *
 * Get the size of a NUL-terminated string in user space without pagefault.
 *
 * Returns the size of the string INCLUDING the terminating NUL.
 *
 * If the string is too long, returns a number larger than @count. User
 * has to check the return value against "> count".
 * On exception (or invalid count), returns 0.
 *
 * Unlike strnlen_user, this can be used from IRQ handler etc. because
 * it disables pagefaults.
 */
/*
 * 在不触发缺页的前提下测量用户字符串，返回值包含末尾 NUL；过长时返回大于 @count
 * 的值，异常或非法 count 返回 0。调用者必须显式用 “> count” 判断过长。
 * 与 strnlen_user() 相比，它关闭 page fault，因而可在 IRQ 等不可睡眠上下文使用。
 *
 * 业务背景：跟踪探针先用它计算安全复制尺寸，再分配/填充事件数据。
 * 入参：unsafe_addr 是 __user 借用字符串地址；count 是含 NUL 的最大探测字节数。
 * 出参/返回：1..count 表示找到 NUL；大于 count 表示过长；0 表示异常或无效输入。
 * 注意事项：无输出参数、无 ownership 变化；用户并发修改会使长度只是瞬时结果。
 */
long strnlen_user_nofault(const void __user *unsafe_addr, long count)
{
	int ret;

	/* 复用体系结构 strnlen_user()，仅改变其 fault 能否进入缺页处理的上下文。 */
	pagefault_disable();
	ret = strnlen_user(unsafe_addr, count);
	pagefault_enable();

	return ret;
}

/*
 * __copy_overflow() - 报告编译器对象大小检查发现的潜在复制越界。
 *
 * 业务背景：uaccess/fortify 包装在已知目标对象 size 小于请求 count 时调用此符号，
 * 以统一留下运行时告警。
 * 入参：size 是编译器推导的目标容量（字节，可能来自有符号接口）；count 是请求字节数。
 * 出参/返回：无直接返回值；产生一次 WARN 日志和告警状态，不修改被复制对象。
 * 注意事项：可在多种上下文触发，不取得锁或引用；WARN 只是诊断，调用方的控制流随后继续。
 */
void __copy_overflow(int size, unsigned long count)
{
	WARN(1, "Buffer overflow detected (%d < %lu)!\n", size, count);
}
EXPORT_SYMBOL(__copy_overflow);
