// SPDX-License-Identifier: GPL-2.0
/*
 * 延时环校准学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件为不能直接从稳定硬件频率推导忙等延时的体系结构提供通用校准器，
 * 最终得到 loops_per_jiffy（LPJ）：CPU 在一个 jiffy 内大致能执行的
 * __delay() 循环次数。udelay()/ndelay() 等原子上下文可用的短忙等路径会
 * 用这个尺度把时间换算成循环次数；本文件不负责实现各体系结构的
 * __delay() 指令循环，也不把 BogoMIPS 当作真实 CPU 性能指标。
 *
 * 启动主路径：
 *   start_kernel() → calibrate_delay()
 *     → 复用每 CPU/启动参数/定时器频率/架构已知值
 *     → calibrate_delay_direct() 直接读取硬件计数器
 *     → calibrate_delay_converge() 以 jiffies 和忙等循环测量兜底
 *     → 发布当前 CPU 缓存及全局 loops_per_jiffy
 *     → calibration_delay_done() 通知体系结构校准阶段结束
 *
 * SMP 次级 CPU 也可在各自 bring-up 路径调用 calibrate_delay()。每 CPU 缓存
 * 防止同一 CPU 重复测量，弱钩子允许架构复用同核簇/同频率 CPU 的结果。
 * 这些调用发生在启动或 CPU 热插拔的受控阶段，当前 CPU 身份必须稳定；
 * 本文件没有为任意运行时并发调用提供锁协议。
 *
 * 方案权衡：硬件计数器直测通常更快，并用上下界过滤 SMI 等异步扰动；
 * jiffies 收敛法不依赖专用计时器，适用面更广，但启动耗时更长、精度受
 * tick 粒度和长中断影响。lpj= 启动参数提供最后的人工逃生口，但其正确性
 * 完全由使用者负责，过小会造成欠延时，过大会造成不必要的忙等。
 */
/* calibrate.c: default delay calibration
 *
 * Excised from init/main.c
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */
/*
 * 本文件提供默认的延时校准实现，代码最初从 init/main.c 中拆出。
 * 保留拆分来源有助于理解其仍属于早期启动时间初始化主线，而不是普通
 * 运行时性能测量工具；版权行按原样保留，无需翻译或改写。
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kstrtox.h>
#include <linux/percpu.h>
#include <linux/printk.h>
#include <linux/smp.h>
#include <linux/stddef.h>
#include <linux/timex.h>

/*
 * 两个全局候选值都以“每 jiffy 的延时单位数”为业务尺度，0 表示尚未提供：
 *
 * lpj_fine 由体系结构在已知稳定计时器频率时写入，通常比软件测量更准确；
 * preset_lpj 来自 lpj= 启动参数或平台早期代码，是管理员/平台明确指定值。
 * calibrate_delay() 只读取它们，不取得任何所有权；早期启动完成后它们作为
 * 标量长期存在。preset_lpj 还会被早期 printk 延时逻辑读取。
 */
unsigned long lpj_fine;
unsigned long preset_lpj;

/*
 * lpj_setup() - 解析 lpj= 启动参数并保存人工指定的 LPJ。
 *
 * 调用关系：内核启动参数框架通过下方 __setup 项调用；成功后
 * calibrate_delay() 会优先使用 preset_lpj，跳过所有自动测量。
 * @str: 只读借用的 NUL 结尾参数文本；可使用 0x/0 等前缀让 base=0 自动
 *       判断进制。函数不保存该指针，也不改变其所有权。
 * 入口与上下文：早期启动、单线程解析阶段，可调用会做字符串转换的 helper；
 * 无锁、无引用和输出指针。
 * 返回：kstrtoul() 成功时返回 1，表示参数已被本处理器消费；解析失败时返回
 * 0，启动参数框架可按“未处理/格式错误”继续报告。成功副作用仅是写入
 * preset_lpj；失败时其旧值不变。
 */
static int __init lpj_setup(char *str)
{
	/*
	 * kstrtoul() 同时完成范围检查和转换；只有完整合法的 unsigned long
	 * 才能发布到全局。这里把 errno 折叠为 __setup 所需的 handled 布尔值，
	 * 因而调用者不会从本函数得到具体解析错误码。
	 */
	return kstrtoul(str, 0, &preset_lpj) == 0;
}

/*
 * 把 "lpj=" 名字与 __init 函数关联；参数表仅在启动期间使用，相关初始化
 * 存储随后可回收。此处只注册解析入口，不代表 preset_lpj 已经有效。
 */
__setup("lpj=", lpj_setup);

#ifdef ARCH_HAS_READ_CURRENT_TIMER

/* This routine uses the read_current_timer() routine and gets the
 * loops per jiffy directly, instead of guessing it using delay().
 * Also, this code tries to handle non-maskable asynchronous events
 * (like SMIs)
 */
/*
 * 本路径用 read_current_timer() 直接取得每 jiffy 的计数器增量，不再通过
 * __delay() 猜测。它还尝试识别 SMI 一类不可屏蔽异步事件：事件会拉大
 * jiffy 边界附近两个计数器读数的间隔，使上下界明显分离，这类样本会被丢弃。
 */
/*
 * 测量窗口约为 10ms：低 HZ 配置至少跨 1 个 tick，避免整数结果为 0。
 * 最多采集 5 次样本，在可靠性与早期启动耗时之间折中。
 */
#define DELAY_CALIBRATION_TICKS			((HZ < 100) ? 1 : (HZ/100))
#define MAX_DIRECT_CALIBRATION_RETRIES		5

/*
 * calibrate_delay_direct() - 用体系结构当前计时器直接估算 LPJ。
 *
 * 调用关系：calibrate_delay() 在预设值、精确频率和架构复用值都不可用时
 * 首先调用；失败才退到 calibrate_delay_converge()。
 * 入参：无。
 * 入口与上下文：当前 CPU 的 tick 和 read_current_timer() 已可工作；函数在
 * 启动/CPU bring-up 上下文主动轮询 jiffies，不能睡眠，不能用于 tick 停止
 * 或允许长时间抢占的普通运行时路径。它不分配资源，也不持锁。
 * 过程：最多采集五组跨 tick 边界的计数器上下界，先过滤单次受扰样本，再
 * 迭代剔除离均值更远的极端值。
 * 返回：得到至少两个相互接近的好样本时，返回每 jiffy 的计数器单位数；
 * 计时器不可读、回绕、异步扰动过大或好样本不足时返回 0。无全局副作用，
 * 诊断信息除外；0 明确要求调用者采用软件收敛回退。
 */
static unsigned long calibrate_delay_direct(void)
{
	/*
	 * 变量地图：
	 *   pre_start/start/post_start 包围起始 tick 跳变，给出跳变前后读数；
	 *   pre_end/end/post_end       以同样方法包围测量窗口末端；
	 *   start_jiffies              本轮起点的 jiffies 序号；
	 *   timer_rate_{min,max}       每 jiffy 计数器增量的保守下界/上界；
	 *   good_timer_{sum,count}     当前仍被接受样本的总和与个数；
	 *   measured_times[]           每轮上界样本，0 是无效/已剔除哨兵；
	 *   max/min                    当前最大/最小有效样本下标，-1 表示尚无；
	 *   i                          固定五轮采样及后续重建集合的下标。
	 */
	unsigned long pre_start, start, post_start;
	unsigned long pre_end, end, post_end;
	unsigned long start_jiffies;
	unsigned long timer_rate_min, timer_rate_max;
	unsigned long good_timer_sum = 0;
	unsigned long good_timer_count = 0;
	unsigned long measured_times[MAX_DIRECT_CALIBRATION_RETRIES];
	int max = -1; /* index of measured_times with max/min values or not set */
	int min = -1;
	int i;

	/*
	 * 先验证体系结构计时器确实可读。输出写入 pre_start 只是能力探测；
	 * 后续会重新采样，不依赖这个值。负值表示该实现此时不可用。
	 */
	if (read_current_timer(&pre_start) < 0 )
		return 0;

	/*
	 * A simple loop like
	 *	while ( jiffies < start_jiffies+1)
	 *		start = read_current_timer();
	 * will not do. As we don't really know whether jiffy switch
	 * happened first or timer_value was read first. And some asynchronous
	 * event can happen between these two events introducing errors in lpj.
	 *
	 * So, we do
	 * 1. pre_start <- When we are sure that jiffy switch hasn't happened
	 * 2. check jiffy switch
	 * 3. start <- timer value before or after jiffy switch
	 * 4. post_start <- When we are sure that jiffy switch has happened
	 *
	 * Note, we don't know anything about order of 2 and 3.
	 * Now, by looking at post_start and pre_start difference, we can
	 * check whether any asynchronous event happened or not
	 */
	/*
	 * 不能只在观察到 jiffies 改变时读取一次计数器，因为两次独立读取没有
	 * 原子快照：无法知道 tick 更新与计数器读取谁先发生，夹在其中的 SMI
	 * 还会直接污染 LPJ。于是保存“确定在边界前”的 pre_start 和“确定在
	 * 边界后”的 post_start；start 恰落在哪侧未知，但前后跨度可以暴露干扰。
	 */

	/*
	 * 阶段 1：重复构造同长度测量窗口。所有读数只在本轮有效；pre_* 的 0
	 * 哨兵还可识别等待循环一次都未执行、因而无法建立可靠边界的异常样本。
	 */
	for (i = 0; i < MAX_DIRECT_CALIBRATION_RETRIES; i++) {
		/* 捕获第一个 tick 边界两侧最近的计数器读数。 */
		pre_start = 0;
		read_current_timer(&start);
		start_jiffies = jiffies;
		while (time_before_eq(jiffies, start_jiffies + 1)) {
			pre_start = start;
			read_current_timer(&start);
		}
		read_current_timer(&post_start);

		/*
		 * 从同一起始 jiffy 再等待 DELAY_CALIBRATION_TICKS 个 tick，
		 * 捕获窗口末端边界。end 从 post_start 起步，避免未初始化值参与差值。
		 */
		pre_end = 0;
		end = post_start;
		while (time_before_eq(jiffies, start_jiffies + 1 +
					       DELAY_CALIBRATION_TICKS)) {
			pre_end = end;
			read_current_timer(&end);
		}
		read_current_timer(&post_end);

		/*
		 * 最宽区间 post_end-pre_start 给出速率上界，最窄区间
		 * pre_end-post_start 给出下界；除以跨越 tick 数后，单位统一为
		 * “计数器单位/jiffy”。边界读取的不确定性被显式保留为一个区间。
		 */
		timer_rate_max = (post_end - pre_start) /
					DELAY_CALIBRATION_TICKS;
		timer_rate_min = (pre_end - post_start) /
					DELAY_CALIBRATION_TICKS;

		/*
		 * If the upper limit and lower limit of the timer_rate is
		 * >= 12.5% apart, redo calibration.
		 */
		/*
		 * 若速率上下界相差至少 1/8，说明边界附近很可能有长异步事件，
		 * 本轮重新校准而不把可疑值混入统计。
		 */
		/*
		 * start >= post_end 在通常递增计数器上表示测量期间回绕或读数不再
		 * 可比较；只记录诊断，下面的 start < post_end 条件会拒绝该样本。
		 */
		if (start >= post_end)
			printk(KERN_NOTICE "calibrate_delay_direct() ignoring "
					"timer_rate as we had a TSC wrap around"
					" start=%lu >=post_end=%lu\n",
				start, post_end);
		/*
		 * 有效样本还要求两个等待循环都真正取得边界前读数。接受时保存较大
		 * 的上界而非较小下界，使最终忙等尺度偏保守，避免因低估造成欠延时。
		 * max/min 始终指向当前有效集合的两个端点。
		 */
		if (start < post_end && pre_start != 0 && pre_end != 0 &&
		    (timer_rate_max - timer_rate_min) < (timer_rate_max >> 3)) {
			good_timer_count++;
			good_timer_sum += timer_rate_max;
			measured_times[i] = timer_rate_max;
			if (max < 0 || timer_rate_max > measured_times[max])
				max = i;
			if (min < 0 || timer_rate_max < measured_times[min])
				min = i;
		} else
			/*
			 * 0 是无效哨兵；真实计时器速率应为正，因此后续重建集合时
			 * 可以无歧义地跳过本轮。
			 */
			measured_times[i] = 0;

	}

	/*
	 * Find the maximum & minimum - if they differ too much throw out the
	 * one with the largest difference from the mean and try again...
	 */
	/*
	 * 阶段 2：在好样本集合内做鲁棒收敛。若最大、最小值分散过大，就删除
	 * 离当前均值更远的一端并重算集合；这比盲目平均更能抵抗一次长中断。
	 */
	while (good_timer_count > 1) {
		/*
		 * estimate 是当前样本均值；maxdiff 是允许的 1/8 波动窗口。
		 * 两个局部量只在本轮剔除决策中有效。
		 */
		unsigned long estimate;
		unsigned long maxdiff;

		/* compute the estimate */
		/* 计算当前有效样本的均值，并把 12.5% 作为一致性阈值。 */
		estimate = (good_timer_sum/good_timer_count);
		maxdiff = estimate >> 3;

		/* if range is within 12% let's take it */
		/*
		 * 原注释中的“12%”是对 1/8（12.5%）的近似表述。端点跨度小于
		 * 阈值时，至少两个样本彼此一致，均值可作为成功结果返回。
		 */
		if ((measured_times[max] - measured_times[min]) < maxdiff)
			return estimate;

		/* ok - drop the worse value and try again... */
		/*
		 * 集合尚未收敛：清空聚合器，先从 max/min 中剔除距均值更远者，
		 * 再扫描数组重建 count、sum 和两个端点。相等时删除最大值，
		 * 同样偏向避免保留异常偏大的启动延时估计。
		 */
		good_timer_sum = 0;
		good_timer_count = 0;
		if ((measured_times[max] - estimate) <
				(estimate - measured_times[min])) {
			printk(KERN_NOTICE "calibrate_delay_direct() dropping "
					"min bogoMips estimate %d = %lu\n",
				min, measured_times[min]);
			measured_times[min] = 0;
			min = max;
		} else {
			printk(KERN_NOTICE "calibrate_delay_direct() dropping "
					"max bogoMips estimate %d = %lu\n",
				max, measured_times[max]);
			measured_times[max] = 0;
			max = min;
		}

		/* 从所有非零样本重建集合不变量，为下一轮范围检查做准备。 */
		for (i = 0; i < MAX_DIRECT_CALIBRATION_RETRIES; i++) {
			if (measured_times[i] == 0)
				continue;
			good_timer_count++;
			good_timer_sum += measured_times[i];
			if (measured_times[i] < measured_times[min])
				min = i;
			if (measured_times[i] > measured_times[max])
				max = i;
		}

	}

	/*
	 * 阶段 3：可用样本降到不足两个仍未收敛。这里不发布一个无法交叉验证的
	 * 单样本结果，而是返回 0，让通用忙等测量接管；lpj= 可用于已知平台。
	 */
	printk(KERN_NOTICE "calibrate_delay_direct() failed to get a good "
	       "estimate for loops_per_jiffy.\nProbably due to long platform "
		"interrupts. Consider using \"lpj=\" boot option.\n");
	return 0;
}
#else
/*
 * calibrate_delay_direct() - 未提供当前计时器接口时的编译期占位实现。
 *
 * 入参：无。返回：恒为 0，无副作用、无锁且不睡眠；这会让
 * calibrate_delay() 明确进入 calibrate_delay_converge()，而不需要在主
 * 决策链中重复条件编译。ARCH_HAS_READ_CURRENT_TIMER 配置决定采用哪一版。
 */
static unsigned long calibrate_delay_direct(void)
{
	return 0;
}
#endif

/*
 * This is the number of bits of precision for the loops_per_jiffy.  Each
 * time we refine our estimate after the first takes 1.5/HZ seconds, so try
 * to start with a good estimate.
 * For the boot cpu we can skip the delay calibration and assign it a value
 * calculated based on the timer frequency.
 * For the rest of the CPUs we cannot assume that the timer frequency is same as
 * the cpu frequency, hence do the calibration for those.
 */
/*
 * LPS_PREC 是 loops_per_jiffy 二分逼近保留的精度位数。第一次确定上下界
 * 后，每增加一位精度约需等待 1.5/HZ 秒，因此先用加速搜索找到接近范围。
 * 启动 CPU 若已有按定时器频率计算的值可完全跳过；其他 CPU 的计时器频率
 * 不一定等于 CPU 执行频率，不能据此自动假定忙等循环尺度相同。
 */
#define LPS_PREC 8

/*
 * calibrate_delay_converge() - 仅依赖 jiffies 与 __delay() 收敛估算 LPJ。
 *
 * 调用关系：calibrate_delay() 的最终通用回退；返回值随后写入当前 CPU
 * 缓存和全局 loops_per_jiffy。
 * 入参：无。
 * 入口与上下文：周期 tick 正常递增，当前 CPU 身份稳定；函数反复忙等跨越
 * tick，不能睡眠，期间中断仍可发生。无锁、无资源分配和 ownership 转移。
 * 过程：先以分带递增的延时找到首次越过一个 tick 的上下界，再用逐次减半
 * 的步长逼近；若每一步都成功加入，认为初始范围被长异步事件严重低估，
 * 扩大范围重新校准。
 * 返回：正的估算 LPJ；本算法没有错误码出口。长中断会影响测量，因此结果
 * 是满足短延时用途的启动估计，不是精密 CPU 性能测量。
 */
static unsigned long calibrate_delay_converge(void)
{
	/* First stage - slowly accelerate to find initial bounds */
	/*
	 * 第一阶段缓慢加速以寻找初始上下界。
	 *
	 * 变量地图：
	 *   lpj/lpj_base       当前候选值/明确未越过 tick 的基线；
	 *   ticks              本次观测的 jiffies 快照；
	 *   loopadd/_base      二分逼近步长/初始最大步长；
	 *   chop_limit         LPS_PREC 所允许的最小步长；
	 *   trials             已完成的 lpj 基础试验份数；
	 *   band               当前每次试验叠加的份数；
	 *   trial_in_band      当前 band 已重复次数。
	 */
	unsigned long lpj, lpj_base, ticks, loopadd, loopadd_base, chop_limit;
	int trials = 0, band = 0, trial_in_band = 0;

	lpj = (1<<12);

	/* wait for "start of" clock tick */
	/*
	 * 等到一个新 tick 的起点再计时，使随后可用的窗口尽量接近完整 jiffy；
	 * 这里是主动轮询，若 tick 不推进将无法结束。
	 */
	ticks = jiffies;
	while (ticks == jiffies)
		; /* nothing */
		/* 循环体故意为空：只等待中断路径推进 jiffies，不执行额外工作。 */
	/* Go .. */
	/*
	 * 开始加速搜索。band 每完成 2^band 次试验才增加，单次 __delay()
	 * 逐步变长；trials 记录累计完成的基础份数，直到某次延时跨过 tick。
	 */
	ticks = jiffies;
	do {
		if (++trial_in_band == (1<<band)) {
			++band;
			trial_in_band = 0;
		}
		__delay(lpj * band);
		trials += band;
	} while (ticks == jiffies);
	/*
	 * We overshot, so retreat to a clear underestimate. Then estimate
	 * the largest likely undershoot. This defines our chop bounds.
	 */
	/*
	 * 最后一次 band 已越过 tick，不能计入安全下界。退回它以后：
	 * lpj_base 是明确不足一个 tick 的累计循环数，loopadd_base 是刚才
	 * 造成越界的最大增量，二者共同界定后续二分逼近范围。
	 */
	trials -= band;
	loopadd_base = lpj * band;
	lpj_base = lpj * trials;

recalibrate:
	/*
	 * 每次到达此标签，lpj_base/loopadd_base 都描述一组新的搜索范围；
	 * 函数没有已取得资源需要回滚，只是重置局部候选值重新测量。
	 */
	lpj = lpj_base;
	loopadd = loopadd_base;

	/*
	 * Do a binary approximation to get lpj set to
	 * equal one clock (up to LPS_PREC bits)
	 */
	/*
	 * 对 LPJ 做二分逼近，直到增量小于基线的 1/2^LPS_PREC。每次都从
	 * 新 tick 起点执行候选延时；若跨 tick，撤销本次增量，否则保留。
	 */
	chop_limit = lpj >> LPS_PREC;
	while (loopadd > chop_limit) {
		lpj += loopadd;
		ticks = jiffies;
		while (ticks == jiffies)
			; /* nothing */
			/* 同样只等待下一个 tick 边界，以获得近乎完整的测量窗口。 */
		ticks = jiffies;
		__delay(lpj);
		if (jiffies != ticks)	/* longer than 1 tick */
			/* jiffies 已变化表示候选值超过一个 tick，撤销本轮试加。 */
			lpj -= loopadd;
		loopadd >>= 1;
	}
	/*
	 * If we incremented every single time possible, presume we've
	 * massively underestimated initially, and retry with a higher
	 * start, and larger range. (Only seen on x86_64, due to SMIs)
	 */
	/*
	 * 若从最大步长到最小步长每次试加都未跨 tick，最终值会恰好到达右侧
	 * 边界。这通常说明初始越界是 SMI 等长事件造成的假象，而非 __delay()
	 * 真正耗尽一个 tick；扩大四倍增量重新测量，避免系统性低估 LPJ。
	 */
	if (lpj + loopadd * 2 == lpj_base + loopadd_base * 2) {
		lpj_base = lpj;
		loopadd_base <<= 2;
		goto recalibrate;
	}

	/* 返回最大已验证“不超过一个 tick”的近似值，调用者负责发布。 */
	return lpj;
}

/*
 * 每 CPU 缓存保存该逻辑 CPU 已确定的 LPJ，0 表示从未校准。它避免 CPU
 * 热插拔/重复 bring-up 时再次执行耗时测量，也避免把某 CPU 的执行速率
 * 无条件当成所有 CPU 的速率。当前 CPU 的启动路径写自己的槽位；读写依赖
 * CPU bring-up 串行化及稳定的 CPU 身份，而不是本变量内部的锁。
 */
static DEFINE_PER_CPU(unsigned long, cpu_loops_per_jiffy) = { 0 };

/*
 * Check if cpu calibration delay is already known. For example,
 * some processors with multi-core sockets may have all cores
 * with the same calibration delay.
 *
 * Architectures should override this function if a faster calibration
 * method is available.
 */
/*
 * 检查当前 CPU 的延时校准值是否已知。例如多核插槽中各核心可能共享同一
 * 校准尺度；若体系结构有更快、更可靠的判定，应覆盖这个弱符号。
 */
/*
 * calibrate_delay_is_known() - 向体系结构查询可复用的当前 CPU LPJ。
 *
 * 入参：无；调用时当前 CPU 身份稳定。
 * 默认实现不持锁、不睡眠、无副作用，返回 0 表示“不知道”，使通用代码
 * 继续直测或收敛测量。体系结构强定义可返回非零 LPJ，例如 x86 在恒定且
 * 已同步的 TSC 条件下复用 CPU0/同插槽 CPU 的结果；返回值只是标量，不涉及
 * 引用或 ownership。
 */
unsigned long __attribute__((weak)) calibrate_delay_is_known(void)
{
	return 0;
}

/*
 * Indicate the cpu delay calibration is done. This can be used by
 * architectures to stop accepting delay timer registrations after this point.
 */
/*
 * 表示 CPU 延时校准已经完成。体系结构可据此停止接受更晚到达的延时计时器
 * 注册，避免已经发布的延时实现与其尺度再次被无序替换。
 */

/*
 * calibration_delay_done() - 通知体系结构本次校准的提交阶段已结束。
 *
 * 入参：无。返回：无直接返回值。
 * 默认弱实现无副作用、不持锁且不睡眠；体系结构覆盖实现可设置本地完成
 * 标志。调用发生在 loops_per_jiffy 已发布之后，因此钩子观察到的是完整
 * 结果；通用代码不接收失败反馈，也不会回滚已发布值。
 */
void __attribute__((weak)) calibration_delay_done(void)
{
}

/*
 * calibrate_delay() - 为当前 CPU 选择、校准并发布 loops_per_jiffy。
 *
 * 宏观位置：启动 CPU 由 start_kernel() 调用；部分体系结构在次级 CPU
 * bring-up 中再次调用，随后把全局结果复制到体系结构自己的每 CPU 数据。
 * 入参：无。
 * 入口与上下文：当前 CPU 身份必须稳定，tick/必要计时器已初始化；函数可
 * 长时间忙等但不睡眠，不分配资源，不应在任意 CPU 上无序并发调用。
 * 选择顺序：当前 CPU 缓存 → lpj= 预设 → 启动 CPU 的 lpj_fine →
 * 架构可复用值 → 硬件计时器直测 → jiffies 收敛回退。
 * 返回：无直接返回值、无输出参数。副作用是写当前 CPU 缓存、全局
 * loops_per_jiffy、一次性日志状态 printed，并调用完成钩子。所有候选路径
 * 最终都必须得到非零 lpj；本函数没有失败返回，自动测量失败会内部回退。
 */
void calibrate_delay(void)
{
	/*
	 * 变量地图：
	 *   lpj      本次选中的每 jiffy 延时单位数，离开决策链后统一发布；
	 *   printed  全系统只打印一次完整校准说明/BogoMIPS，避免每个 CPU 刷屏；
	 *   this_cpu 当前稳定逻辑 CPU 编号，用于访问该 CPU 的校准缓存。
	 */
	unsigned long lpj;
	static bool printed;
	int this_cpu = smp_processor_id();

	/*
	 * 阶段 1：按可信度与成本从低到高选择来源。各分支只确定局部 lpj，
	 * 不立即修改 loops_per_jiffy，确保后面的缓存、日志和完成通知统一提交。
	 */
	if (per_cpu(cpu_loops_per_jiffy, this_cpu)) {
		/* 同一 CPU 已校准：复用自己的缓存，避免重复忙等。 */
		lpj = per_cpu(cpu_loops_per_jiffy, this_cpu);
		if (!printed)
			pr_info("Calibrating delay loop (skipped) "
				"already calibrated this CPU");
	} else if (preset_lpj) {
		/*
		 * lpj= 或平台给出的强制值优先于自动推导；这里只信任非零哨兵，
		 * 不校验其与实际 CPU 频率是否匹配。
		 */
		lpj = preset_lpj;
		if (!printed)
			pr_info("Calibrating delay loop (skipped) "
				"preset value.. ");
	} else if ((!printed) && lpj_fine) {
		/*
		 * lpj_fine 仅供首次（通常是启动 CPU）使用。对后续 CPU，即使该
		 * 全局值非零也不能假定计时器频率等于其 CPU 忙等循环频率。
		 */
		lpj = lpj_fine;
		pr_info("Calibrating delay loop (skipped), "
			"value calculated using timer frequency.. ");
	} else if ((lpj = calibrate_delay_is_known())) {
		/*
		 * 体系结构确认可安全复用非零值；空语句表示候选已通过赋值条件
		 * 写入 lpj，无需通用代码再做工作或打印特定来源消息。
		 */
		;
	} else if ((lpj = calibrate_delay_direct()) != 0) {
		/*
		 * 专用计时器直测成功。它已完成异常样本过滤，但尚未发布结果；
		 * 返回 0 才会落入下一分支。
		 */
		if (!printed)
			pr_info("Calibrating delay using timer "
				"specific routine.. ");
	} else {
		/*
		 * 所有快捷来源不可用或直测不可靠时，使用只依赖 tick 的通用算法；
		 * 该路径最慢但没有失败出口，是整个决策链的最终兜底。
		 */
		if (!printed)
			pr_info("Calibrating delay loop... ");
		lpj = calibrate_delay_converge();
	}
	/*
	 * 阶段 2：提交当前 CPU 的稳定结果。先写 per-CPU 缓存，使同一 CPU
	 * 未来调用可直接复用；此值不会自动同步到其他 CPU 的槽位。
	 */
	per_cpu(cpu_loops_per_jiffy, this_cpu) = lpj;
	/*
	 * BogoMIPS 只是把 LPJ 按 HZ 缩放后的历史展示格式，不是基准测试。
	 * printed 为 false 的首次调用负责补完前面 pr_info() 的同一行输出。
	 */
	if (!printed)
		pr_cont("%lu.%02lu BogoMIPS (lpj=%lu)\n",
			lpj/(500000/HZ),
			(lpj/(5000/HZ)) % 100, lpj);

	/*
	 * 阶段 3：把选中值发布给通用 delay 实现，再标记日志已输出。SMP 架构
	 * 若需要长期保留各 CPU 值，会在本函数返回后复制到自己的 CPU 数据。
	 * 这里的全局赋值服务当前受控启动路径，不构成任意并发读者的同步原语。
	 */
	loops_per_jiffy = lpj;
	printed = true;

	/*
	 * 完成钩子必须位于发布之后：体系结构可以从此拒绝晚到的 delay timer
	 * 注册，且其观察到的 loops_per_jiffy 已与本次选择结果一致。
	 */
	calibration_delay_done();
}
