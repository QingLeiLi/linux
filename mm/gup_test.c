// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>
#include <linux/debugfs.h>
#include <linux/highmem.h>
#include "gup_test.h"

/*
 * 学习提示：测试接口必须严格区分 get_user_pages() 引用与 pin_user_pages() DMA pin。
 * 每条 ioctl 路径都要按取得方式释放，并把部分成功数量而非请求数量交给清理端。
 */
/*
 * 业务背景：一次性 GUP 基准结束后必须依据取得 API 精确归还普通引用或 DMA pin。
 * 入参：cmd 指定取得方式；pages 借用实际成功页数组；nr_pages 是有效项数；gup_test_flags 仅为 dump 模式选择 pin/get。
 * 出参/返回：无直接返回值；释放 pages[0..nr_pages) 持有的全部引用，不释放数组本身。
 * 注意事项：调用者保证 cmd/flags 与取得路径一致；put_page 与 unpin 不可混用，函数可触发批量引用释放副作用。
 */
static void put_back_pages(unsigned int cmd, struct page **pages,
			   unsigned long nr_pages, unsigned int gup_test_flags)
{
	/* cmd 决定 pages[] 中保存的是普通 page 引用还是 FOLL_PIN 引用。 */
	unsigned long i;

	switch (cmd) {
	/* GUP_FAST_BENCHMARK：fast get 取得普通引用；与下一 case 共用逐页 put 路径。 */
	case GUP_FAST_BENCHMARK:
	/* GUP_BASIC_TEST：slow get 取得普通引用；逐项 put_page 后返回调用者清理数组。 */
	case GUP_BASIC_TEST:
		/* get_user_pages 家族逐页 put_page()，不能用 unpin 接口。 */
		for (i = 0; i < nr_pages; i++)
			put_page(pages[i]);
		break;

	/* PIN_FAST_BENCHMARK：fast FOLL_PIN；落入统一批量 unpin 路径。 */
	case PIN_FAST_BENCHMARK:
	/* PIN_BASIC_TEST：slow FOLL_PIN；落入统一批量 unpin 路径。 */
	case PIN_BASIC_TEST:
	/* PIN_LONGTERM_BENCHMARK：带 FOLL_LONGTERM 的 pin；同样以 unpin_user_pages 归还。 */
	case PIN_LONGTERM_BENCHMARK:
		/* pin 家族统一批量 unpin，兑现 DMA pin 计数。 */
		unpin_user_pages(pages, nr_pages);
		break;
	/* DUMP_USER_PAGES_TEST：依据 USE_PIN 标志选择 unpin 或逐页 put，随后结束释放分派。 */
	case DUMP_USER_PAGES_TEST:
		/* dump 测试由标志选择 get/pin，释放端必须复现相同选择。 */
		if (gup_test_flags & GUP_TEST_FLAG_DUMP_PAGES_USE_PIN) {
			unpin_user_pages(pages, nr_pages);
		} else {
			/* 普通 get 模式逐项归还引用，数组本身由外层释放。 */
			for (i = 0; i < nr_pages; i++)
				put_page(pages[i]);

		}
		break;
	}
}

/*
 * 业务背景：PIN 基准在计时区间外验证结果确实带 DMA pin，并验证长期 pin 的 folio 资格。
 * 入参：cmd 是测试命令；pages 是借用结果数组；nr_pages 是有效项数，均不由本函数修改。
 * 出参/返回：无直接返回值；不改变引用，仅在首个不变量失败时 WARN 并 dump folio。
 * 注意事项：普通 GUP/dump 命令无副作用；maybe_dma_pinned 允许假阳性，检查用于发现明确的实现错误。
 */
static void verify_dma_pinned(unsigned int cmd, struct page **pages,
			      unsigned long nr_pages)
{
	/* 只有 PIN 命令需要验证 folio 的 DMA pin 估算状态。 */
	unsigned long i;
	struct folio *folio;

	switch (cmd) {
	/* PIN_FAST_BENCHMARK：检查每个 fast pin folio 的 DMA pin 状态。 */
	case PIN_FAST_BENCHMARK:
	/* PIN_BASIC_TEST：检查每个 slow pin folio 的 DMA pin 状态。 */
	case PIN_BASIC_TEST:
	/* PIN_LONGTERM_BENCHMARK：除 DMA pin 外还检查长期可固定性。 */
	case PIN_LONGTERM_BENCHMARK:
		/* page 可能是大 folio 的任一尾页，检查统一落到 head folio。 */
		for (i = 0; i < nr_pages; i++) {
			folio = page_folio(pages[i]);

			/* maybe 语义允许假阳性，但测试不能接受明确的未 pin。 */
			if (WARN(!folio_maybe_dma_pinned(folio),
				 "pages[%lu] is NOT dma-pinned\n", i)) {

				/* 首个不变量失败即打印并停止，避免重复噪声。 */
				dump_page(&folio->page, "gup_test failure");
				break;
				/* 长期命令还要求 folio 类型满足长期固定策略。 */
			} else if (cmd == PIN_LONGTERM_BENCHMARK &&
				WARN(!folio_is_longterm_pinnable(folio),
				     "pages[%lu] is NOT pinnable but pinned\n",
				     i)) {
				/* 长期可固定性失败同样只报告第一个 folio。 */
				dump_page(&folio->page, "gup_test failure");
				break;
			}
		}
		break;
	}
}

/*
 * 业务背景：DUMP_USER_PAGES_TEST 按用户给定的 1-based 索引输出已取得页面的诊断信息。
 * 入参：gup 是输入输出参数快照；pages 是借用结果数组；nr_pages 是实际有效项数。
 * 出参/返回：无直接返回值；越界的 which_pages 元素被清零，合法页面被打印，引用 ownership 不变。
 * 注意事项：只允许访问实际成功前缀；零索引表示跳过，打印可能产生大量日志但不在基准计时内。
 */
static void dump_pages_test(struct gup_test *gup, struct page **pages,
			    unsigned long nr_pages)
{
	/* which_pages 使用 1-based ABI，零值保留为“不打印”。 */
	unsigned int index_to_dump;
	unsigned int i;

	/*
	 * Zero out any user-supplied page index that is out of range. Remember:
	 * .which_pages[] contains a 1-based set of page indices.
	 */
	/*
	 * 译注：先把用户提供的越界页索引清零；需要特别注意，which_pages[] 使用从 1 开始
	 * 的页号，而 pages[] 使用从 0 开始的数组下标。
	 */
	/* 第一遍原地清除越过实际 pin 数量的用户索引。 */
	for (i = 0; i < GUP_TEST_MAX_PAGES_TO_DUMP; i++) {
		if (gup->which_pages[i] > nr_pages) {
			pr_warn("ZEROING due to out of range: .which_pages[%u]: %u\n",
				i, gup->which_pages[i]);
			gup->which_pages[i] = 0;
		}
	}

	/* 第二遍只消费非零合法项，并在数组访问前转换为 0-based。 */
	for (i = 0; i < GUP_TEST_MAX_PAGES_TO_DUMP; i++) {
		index_to_dump = gup->which_pages[i];

		if (index_to_dump) {
			/* 合法非零索引必定位于实际 pages[] 前缀内。 */
			index_to_dump--; // Decode from 1-based, to 0-based
			/* 译注：把 ABI 的 1-based 页号解码为 C 数组使用的 0-based 下标。 */
			pr_info("---- page #%u, starting from user virt addr: 0x%llx\n",
				index_to_dump, gup->addr);
			dump_page(pages[index_to_dump],
				  "gup_test: dump_pages() test");
		}
	}
	/* dump 只观察页面，不改变 pages[] 引用所有权。 */
}

/*
 * 业务背景：一次性测试核心按命令分批执行 get/pin，记录取得与归还耗时并回写实际处理范围。
 * 入参：cmd 是六种一次性命令之一；gup 是输入输出结构，提供地址/大小/flags/批量并接收计时与实际 size。
 * 出参/返回：成功返回 0；地址/溢出错误 -EINVAL，数组失败 -ENOMEM，锁中断 -EINTR；所有已取页均归还。
 * 注意事项：slow 命令持 current->mm 的可杀 mmap 读锁；允许 GUP 部分成功，返回 0 不代表请求范围全部完成。
 */
static int __gup_test_ioctl(unsigned int cmd,
		struct gup_test *gup)
{
	/* fast GUP 自行处理页表并发，其他命令由本测试持 mmap 读锁。 */
	ktime_t start_time, end_time;
	unsigned long i, nr_pages, addr, next;
	long nr;
	struct page **pages;
	unsigned long end;
	int ret = 0;
	/* needs_mmap_lock 同时控制取锁和统一出口的解锁。 */
	bool needs_mmap_lock =
		cmd != GUP_FAST_BENCHMARK && cmd != PIN_FAST_BENCHMARK;

	/* fast/slow 锁策略在进入任何资源分配前固定。 */
	/* UAPI 是 64 位字段；先检查本机 unsigned long 可表示性与区间溢出。 */
	if (gup->addr > ULONG_MAX || gup->size > ULONG_MAX)
		return -EINVAL;
	if (check_add_overflow((unsigned long)gup->addr,
			       (unsigned long)gup->size, &end))
		return -EINVAL;

	/* pages[] 按完整基础页数分配，非整页尾部不会进入请求。 */
	nr_pages = gup->size / PAGE_SIZE;
	pages = kvcalloc(nr_pages, sizeof(void *), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	/* 可杀取锁失败在尚未取得任何页时跳到仅释放数组的出口。 */
	if (needs_mmap_lock && mmap_read_lock_killable(current->mm)) {
		ret = -EINTR;
		goto free_pages;
	}

	/* i 累计实际取得页数，nr 保存每批返回数量。 */
	i = 0;
	nr = gup->nr_pages_per_call;
	start_time = ktime_get();
	/* 每批必须完整成功才继续；短 pin 表示在当前地址停止。 */
	for (addr = gup->addr; addr < end; addr = next) {
		if (nr != gup->nr_pages_per_call)
			break;

		/* 最后一批收缩到 end，避免请求越过用户给定范围。 */
		next = addr + nr * PAGE_SIZE;
		if (next > end) {
			next = end;
			nr = (next - addr) / PAGE_SIZE;
		}

		switch (cmd) {
		/* GUP_FAST_BENCHMARK：无外层 mmap 锁调用 fast get，返回普通引用数量。 */
		case GUP_FAST_BENCHMARK:
			/* fast get 返回普通引用，不要求外层 mmap_lock。 */
			nr = get_user_pages_fast(addr, nr, gup->gup_flags,
						 pages + i);
			break;
		/* GUP_BASIC_TEST：在外层 mmap 读锁下调用 slow get，返回普通引用数量。 */
		case GUP_BASIC_TEST:
			/* basic get 在当前 mm 上走带锁的慢路径。 */
			nr = get_user_pages(addr, nr, gup->gup_flags, pages + i);
			break;
		/* PIN_FAST_BENCHMARK：无外层 mmap 锁建立 DMA pin，稍后统一 unpin。 */
		case PIN_FAST_BENCHMARK:
			/* fast pin 建立 DMA pin 计数，释放必须 unpin。 */
			nr = pin_user_pages_fast(addr, nr, gup->gup_flags,
						 pages + i);
			break;
		/* PIN_BASIC_TEST：在 mmap 读锁下建立普通 DMA pin。 */
		case PIN_BASIC_TEST:
			/* basic pin 复用外层 mmap 读锁保护页表遍历。 */
			nr = pin_user_pages(addr, nr, gup->gup_flags, pages + i);
			break;
		/* PIN_LONGTERM_BENCHMARK：slow pin 叠加 FOLL_LONGTERM 资格检查。 */
		case PIN_LONGTERM_BENCHMARK:
			/* FOLL_LONGTERM 额外拒绝不适合长期固定的 folio。 */
			nr = pin_user_pages(addr, nr,
					    gup->gup_flags | FOLL_LONGTERM,
					    pages + i);
			break;
		/* DUMP_USER_PAGES_TEST：按 USE_PIN 选择引用类型，随后进入页面打印阶段。 */
		case DUMP_USER_PAGES_TEST:
			/* dump 模式由 test_flags 选择 pin 或普通 get。 */
			if (gup->test_flags & GUP_TEST_FLAG_DUMP_PAGES_USE_PIN)
				nr = pin_user_pages(addr, nr, gup->gup_flags,
						    pages + i);
			else
				nr = get_user_pages(addr, nr, gup->gup_flags,
						    pages + i);
			break;
		/* default：内部防御未知命令，设置 EINVAL 并跳到锁释放出口。 */
		default:
			/* 命令已由顶层过滤，此分支仍作为内部防御。 */
			ret = -EINVAL;
			goto unlock;
		}

		/* 负错误和零进展均终止；已取得前缀仍会正常归还。 */
		if (nr <= 0)
			break;
		i += nr;
	}
	end_time = ktime_get();

	/* Shifting the meaning of nr_pages: now it is actual number pinned: */
	/* 从此 nr_pages 表示实际取得数，所有验证和释放只覆盖该前缀。 */
	nr_pages = i;

	/* 计时只包围 GUP/PIN 循环，验证和 dump 不污染 get 指标。 */
	gup->get_delta_usec = ktime_us_delta(end_time, start_time);
	gup->size = addr - gup->addr;

	/*
	 * Take an un-benchmark-timed moment to verify DMA pinned
	 * state: print a warning if any non-dma-pinned pages are found:
	 */
	/* DMA 状态检查位于基准区间之外，避免 WARN/dump 扭曲结果。 */
	verify_dma_pinned(cmd, pages, nr_pages);

	if (cmd == DUMP_USER_PAGES_TEST)
		dump_pages_test(gup, pages, nr_pages);

	start_time = ktime_get();

	/* put 计时独立记录，并按原命令/标志选择配对释放方法。 */
	put_back_pages(cmd, pages, nr_pages, gup->test_flags);

	end_time = ktime_get();
	gup->put_delta_usec = ktime_us_delta(end_time, start_time);

unlock:
	/* 仅 slow 命令实际取得 mmap 锁，统一出口按 needs_mmap_lock 配对。 */
	if (needs_mmap_lock)
		mmap_read_unlock(current->mm);
free_pages:
	kvfree(pages);
	return ret;
}

/*
 * pin_longterm_test_mutex 串行所有跨 ioctl 会话状态；全局数组非空即表示会话已发布，
 * nr_pages 只计算其中已经成功取得且最终必须 unpin 的前缀。
 */
static DEFINE_MUTEX(pin_longterm_test_mutex);
/* 三个全局量共同表示一次跨 ioctl 存活的长期 pin 会话。 */
static struct page **pin_longterm_test_pages;
static unsigned long pin_longterm_test_nr_pages;

/*
 * 业务背景：长期 pin 会话结束或文件关闭时统一释放跨 ioctl 保存的 pin 和数组。
 * 入参：无。
 * 出参/返回：无直接返回值；unpin 已成功前缀、释放全局数组并把会话状态恢复为空。
 * 注意事项：调用者必须持 pin_longterm_test_mutex；函数幂等，数组 ownership 在非空期间属于全局会话。
 */
static inline void pin_longterm_test_stop(void)
{
	/* 调用者持全局 mutex；stop 可重复调用且最终恢复空会话。 */
	if (pin_longterm_test_pages) {
		/* 仅 unpin 已成功取得的前缀，数组容量不等于 pin 数量。 */
		if (pin_longterm_test_nr_pages)
			unpin_user_pages(pin_longterm_test_pages,
					 pin_longterm_test_nr_pages);
		kvfree(pin_longterm_test_pages);
		/* 先释放资源再清发布指针和计数，锁内无并发观察者。 */
		pin_longterm_test_pages = NULL;
		pin_longterm_test_nr_pages = 0;
	}
}

/*
 * 业务背景：START ioctl 创建跨调用存活的 FOLL_LONGTERM 会话，供后续 READ 检查固定页内容。
 * 入参：arg 是指向 struct pin_longterm_test 的用户地址数值；结构含页对齐地址/大小及 WRITE/FAST 标志。
 * 出参/返回：成功返回 0并由全局状态持有全部 pin；返回 -EINVAL/-EFAULT/-ENOMEM/-EINTR 或 GUP 负错误并回滚。
 * 注意事项：调用者持全局 mutex；slow 路径可睡眠并持 mmap 读锁，已存在会话时禁止覆盖。
 */
static inline int pin_longterm_test_start(unsigned long arg)
{
	/* start 建立独占会话，失败必须撤销所有部分 pin 和数组。 */
	/* nr_pages 是目标总页数，cur_pages 是本轮结果，remaining_pages 是尚未 pin 的数量。 */
	long nr_pages, cur_pages, addr, remaining_pages;
	/* gup_flags 最少含 LONGTERM，并按用户 WRITE 位追加写访问要求。 */
	int gup_flags = FOLL_LONGTERM;
	/* args 是固定 UAPI 的内核快照；pages 是分配起点并在循环中作为输出游标推进。 */
	struct pin_longterm_test args;
	struct page **pages;
	/* ret 保存需要向 ioctl 传播的首个负 GUP 错误，零表示全部完成。 */
	int ret = 0;
	/* fast 决定使用无外层 mmap_lock 的 fast-GUP，还是锁内 slow-GUP。 */
	bool fast;

	/* 非 NULL 是会话占用标志，禁止覆盖仍需 stop 的所有权。 */
	if (pin_longterm_test_pages)
		return -EINVAL;

	/* 先完整复制固定 ABI，再验证 flags、对齐、大小和非空范围。 */
	if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
		return -EFAULT;

	/* 未知标志拒绝，避免未来位在旧内核上被静默误解释。 */
	if (args.flags &
	    ~(PIN_LONGTERM_TEST_FLAG_USE_WRITE|PIN_LONGTERM_TEST_FLAG_USE_FAST))
		return -EINVAL;
	/* 地址与长度都须页对齐，按位或一次验证两者低位。 */
	if (!IS_ALIGNED(args.addr | args.size, PAGE_SIZE))
		return -EINVAL;
	if (args.size > LONG_MAX)
		return -EINVAL;
	nr_pages = args.size / PAGE_SIZE;
	if (!nr_pages)
		return -EINVAL;

	/* kvcalloc 支持大数组并清零，尚未 pin 时失败可直接返回。 */
	pages = kvcalloc(nr_pages, sizeof(void *), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	/* WRITE 控制写 pin，FAST 控制是否需要外层 mmap 读锁。 */
	if (args.flags & PIN_LONGTERM_TEST_FLAG_USE_WRITE)
		gup_flags |= FOLL_WRITE;
	fast = !!(args.flags & PIN_LONGTERM_TEST_FLAG_USE_FAST);

	/* slow 会话在完整 pin 循环期间持锁；信号失败尚未发布全局状态。 */
	if (!fast && mmap_read_lock_killable(current->mm)) {
		kvfree(pages);
		return -EINTR;
	}

	/* 锁取得后发布数组，计数从零开始记录可清理前缀。 */
	pin_longterm_test_pages = pages;
	pin_longterm_test_nr_pages = 0;

	/* 循环允许短 pin，持续请求剩余范围直至完成或负错误。 */
	while (nr_pages - pin_longterm_test_nr_pages) {
		remaining_pages = nr_pages - pin_longterm_test_nr_pages;
		addr = args.addr + pin_longterm_test_nr_pages * PAGE_SIZE;

		/* 输出写入当前 pages 游标，物理页数组连续累积。 */
		if (fast)
			cur_pages = pin_user_pages_fast(addr, remaining_pages,
							gup_flags, pages);
		else
			cur_pages = pin_user_pages(addr, remaining_pages,
						   gup_flags, pages);
		/* 负错误触发 stop 回滚；零返回理论上会无进展，依赖 GUP 契约。 */
		if (cur_pages < 0) {
			pin_longterm_test_stop();
			ret = cur_pages;
			break;
		}
		/* 先增加已拥有计数，再推进局部输出游标。 */
		pin_longterm_test_nr_pages += cur_pages;
		pages += cur_pages;
	}

	/* slow 路径无论成功或 GUP 错误都在返回前释放 mmap 锁。 */
	if (!fast)
		mmap_read_unlock(current->mm);
	return ret;
}

/*
 * 业务背景：READ ioctl 将活动长期 pin 会话中的页面逐页复制到用户连续缓冲区以验证可访问性。
 * 入参：arg 指向一个 __u64 用户目标地址；全局 pages/nr_pages 是锁下借用的活动会话状态。
 * 出参/返回：全部复制成功返回 0；无会话返回 -EINVAL，参数或数据复制失败返回 -EFAULT；pin 保持不变。
 * 注意事项：调用者持全局 mutex；每页使用 kmap_local/kunmap_local 配对，函数不终止会话或转移 pin ownership。
 */
static inline int pin_longterm_test_read(unsigned long arg)
{
	/* read 把长期 pin 页内容逐页复制到用户提供的连续目标。 */
	__u64 user_addr;
	unsigned long i;

	/* 必须存在活动会话；全局 mutex 保证 stop 不会并发释放数组。 */
	if (!pin_longterm_test_pages)
		return -EINVAL;

	if (copy_from_user(&user_addr, (void __user *)arg, sizeof(user_addr)))
		return -EFAULT;

	/* kmap_local 兼容 highmem，每页复制后立即解除本地映射。 */
	for (i = 0; i < pin_longterm_test_nr_pages; i++) {
		void *addr = kmap_local_page(pin_longterm_test_pages[i]);
		unsigned long ret;

		ret = copy_to_user((void __user *)(unsigned long)user_addr, addr,
				   PAGE_SIZE);
		kunmap_local(addr);
		/* copy_to_user 非零表示残余字节，本 ioctl 折叠为 EFAULT。 */
		if (ret)
			return -EFAULT;
		user_addr += PAGE_SIZE;
	}
	return 0;
}

/*
 * 业务背景：长期测试命令需要共享全局会话，专用分派器用 mutex 串行 START/STOP/READ。
 * 入参：filep 是未使用的借用文件指针；cmd 是三种长期命令；arg 是对应用户参数地址或 STOP 的无用值。
 * 出参/返回：返回具体操作结果；未知命令 -EINVAL，锁等待被信号打断返回 -EINTR。
 * 注意事项：mutex 覆盖整个操作并保护全局数组/计数；所有出口必须解锁，STOP 幂等且返回 0。
 */
static long pin_longterm_test_ioctl(struct file *filep, unsigned int cmd,
				    unsigned long arg)
{
	/* mutex 串行 start/read/stop，并保护跨文件描述符共享的全局会话。 */
	int ret = -EINVAL;

	if (mutex_lock_killable(&pin_longterm_test_mutex))
		return -EINTR;

	/* 未识别命令保持初始 EINVAL，所有分支共用解锁出口。 */
	switch (cmd) {
	/* PIN_LONGTERM_TEST_START：复制用户参数并建立全局长期 pin 会话。 */
	case PIN_LONGTERM_TEST_START:
		ret = pin_longterm_test_start(arg);
		break;
	/* PIN_LONGTERM_TEST_STOP：幂等释放会话，不消费 arg，结果固定为 0。 */
	case PIN_LONGTERM_TEST_STOP:
		/* stop 是幂等操作，即使会话为空也返回成功。 */
		pin_longterm_test_stop();
		ret = 0;
		break;
	/* PIN_LONGTERM_TEST_READ：保持 pin 的同时把页面内容复制给用户。 */
	case PIN_LONGTERM_TEST_READ:
		ret = pin_longterm_test_read(arg);
		break;
	}

	/* 所有操作都在释放 mutex 后才把结果交回 VFS。 */
	mutex_unlock(&pin_longterm_test_mutex);
	return ret;
}

/*
 * 业务背景：debugfs unlocked_ioctl 顶层按一次性基准 ABI 与长期会话 ABI 分派并完成用户结构复制。
 * 入参：filep 是借用文件指针；cmd 是受支持 ioctl；arg 是用户结构地址或长期命令参数地址。
 * 出参/返回：成功 0；未知命令 -EINVAL，用户复制 -EFAULT，或原样传播核心/长期操作错误。
 * 注意事项：一次性 gup 结构只在内核快照上执行，成功后结果复制回用户；不持有 filep 引用。
 */
static long gup_test_ioctl(struct file *filep, unsigned int cmd,
		unsigned long arg)
{
	/* 顶层先按 ABI 族分派，长期会话命令不使用 struct gup_test。 */
	struct gup_test gup;
	int ret;

	/* 白名单阻止未知 ioctl 进入用户结构复制和基准逻辑。 */
	switch (cmd) {
	/* GUP_FAST_BENCHMARK：允许进入一次性 fast get ABI，后续复制 struct gup_test。 */
	case GUP_FAST_BENCHMARK:
	/* PIN_FAST_BENCHMARK：允许进入一次性 fast pin ABI。 */
	case PIN_FAST_BENCHMARK:
	/* PIN_LONGTERM_BENCHMARK：允许进入一次性 longterm pin 基准 ABI。 */
	case PIN_LONGTERM_BENCHMARK:
	/* GUP_BASIC_TEST：允许进入一次性 slow get ABI。 */
	case GUP_BASIC_TEST:
	/* PIN_BASIC_TEST：允许进入一次性 slow pin ABI。 */
	case PIN_BASIC_TEST:
	/* DUMP_USER_PAGES_TEST：允许进入一次性 get/pin 后打印页面 ABI。 */
	case DUMP_USER_PAGES_TEST:
		/* 六种一次性命令共享 struct gup_test ABI。 */
		break;
	/* PIN_LONGTERM_TEST_START：转交长期会话分派器并直接返回其结果。 */
	case PIN_LONGTERM_TEST_START:
	/* PIN_LONGTERM_TEST_STOP：转交长期会话分派器并直接返回其结果。 */
	case PIN_LONGTERM_TEST_STOP:
	/* PIN_LONGTERM_TEST_READ：转交长期会话分派器并直接返回其结果。 */
	case PIN_LONGTERM_TEST_READ:
		/* 三种会话命令由独立 mutex 保护的分派器处理。 */
		return pin_longterm_test_ioctl(filep, cmd, arg);
	/* default：拒绝所有未定义 ioctl，不触碰用户内存或全局会话。 */
	default:
		return -EINVAL;
	}

	/* 基准参数使用内核快照，执行后再复制回计时与实际范围。 */
	if (copy_from_user(&gup, (void __user *)arg, sizeof(gup)))
		return -EFAULT;

	ret = __gup_test_ioctl(cmd, &gup);
	if (ret)
		return ret;

	/* 执行成功但结果复制失败仍向用户报告 EFAULT。 */
	if (copy_to_user((void __user *)arg, &gup, sizeof(gup)))
		return -EFAULT;

	return 0;
}

/*
 * 业务背景：最后的文件关闭路径兜底停止可能遗留的长期 pin，避免测试资源永久占用。
 * 入参：inode/file 均为 VFS 借用指针且当前实现不解引用、不取得引用。
 * 出参/返回：始终返回 0；释放活动长期 pin 会话及数组。
 * 注意事项：当前实现未取得测试 mutex；多个打开实例若并发 ioctl/release，会与全局会话状态竞争，测试方须串行关闭与操作。
 */
static int gup_test_release(struct inode *inode, struct file *file)
{
	/* 关闭 debugfs 文件兜底终止长期会话，避免永久 pin 泄漏。 */
	pin_longterm_test_stop();

	return 0;
}

/*
 * gup_test_fops 在 late init 后由 debugfs dentry 长期借用；表本身只读且无需锁。
 * open 禁止 seek，原生/compat ioctl 汇合到上述 ABI 分派，release 则兜底回收长期 pin。
 */
static const struct file_operations gup_test_fops = {
	/* unsafe debugfs 由 ioctl 自行完成用户指针检查；长期 ioctl 由专用 mutex 串行。 */
	/* open 只建立不可 seek 的普通文件状态，不创建私有会话。 */
	.open = nonseekable_open,
	/* 原生 ioctl 解析 struct gup_test 或长期会话参数。 */
	.unlocked_ioctl = gup_test_ioctl,
	/* compat ioctl 只规范化用户指针，命令布局与原生 ABI 共用。 */
	.compat_ioctl = compat_ptr_ioctl,
	/* 最后关闭时释放仍由全局状态持有的长期 pin。 */
	.release = gup_test_release,
};

/*
 * 业务背景：内核晚期初始化时发布 gup_test debugfs 控制文件，供自测程序执行 ioctl。
 * 入参：无。
 * 出参/返回：始终返回 0；尽力创建 0600 debugfs 文件，未保存 dentry，也无显式失败传播。
 * 注意事项：仅初始化期调用且可睡眠；debugfs 不可用时创建可静默失败，测试接口相应不存在。
 */
static int __init gup_test_init(void)
{
	/* late init 发布仅 root 可读写的非 seek 测试控制文件。 */
	debugfs_create_file_unsafe("gup_test", 0600, NULL, NULL,
				   &gup_test_fops);

	return 0;
}

late_initcall(gup_test_init);
