/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __GUP_TEST_H
#define __GUP_TEST_H

#include <linux/types.h>

/*
 * 该头文件定义 debugfs 文件 /sys/kernel/debug/gup_test 的用户态 ABI。
 * tools/testing/selftests/mm/gup_test.c 等程序把下面的命令号和结构体传给
 * mm/gup_test.c:gup_test_ioctl()；字段布局一旦改变会同时影响内核与现有
 * 测试程序，因此这里使用固定宽度的 __u32/__u64，而不是随架构变化的
 * unsigned long。
 */

/*
 * 前六个命令共用 struct gup_test，并由 _IOWR 标明参数会双向传递：用户态
 * 输入地址、长度、批大小和标志，内核回写实际处理长度与获取/释放耗时。
 * FAST 命令调用无须预持 mmap_read_lock 的 fast GUP/PUP 接口；BASIC 命令
 * 走普通接口；PIN_LONGTERM 额外施加 FOLL_LONGTERM；DUMP 则选取页输出状态。
 * ioctl 类型字符 'g' 与序号 1..6 共同构成稳定命令号，不能随意重排复用。
 */
#define GUP_FAST_BENCHMARK	_IOWR('g', 1, struct gup_test)
#define PIN_FAST_BENCHMARK	_IOWR('g', 2, struct gup_test)
#define PIN_LONGTERM_BENCHMARK	_IOWR('g', 3, struct gup_test)
#define GUP_BASIC_TEST		_IOWR('g', 4, struct gup_test)
#define PIN_BASIC_TEST		_IOWR('g', 5, struct gup_test)
#define DUMP_USER_PAGES_TEST	_IOWR('g', 6, struct gup_test)

/*
 * 这三个命令构成长时间 pin 的有状态协议：START 从用户态读取 pin 范围并把
 * page 数组保存在驱动全局状态中，READ 把所 pin 页内容复制到用户缓冲区，
 * STOP 解除 pin 并释放数组。mm/gup_test.c 用 mutex 串行化协议，文件关闭时
 * 也会执行 STOP，因而 START 成功后的资源不会依赖测试程序显式清理。
 * STOP 无参数所以用 _IO；START/READ 只从用户态取参数所以使用 _IOW。
 */
#define PIN_LONGTERM_TEST_START	_IOW('g', 7, struct pin_longterm_test)
#define PIN_LONGTERM_TEST_STOP	_IO('g', 8)
#define PIN_LONGTERM_TEST_READ	_IOW('g', 9, __u64)

/*
 * 一次最多请求打印八个页，固定数组使 ioctl 结构大小稳定，并限制 dump_page()
 * 带来的日志量。数组槽中的 0 是“不处理”哨兵，不代表第 0 页。
 */
#define GUP_TEST_MAX_PAGES_TO_DUMP		8

/* DUMP 命令置此位时用 pin_user_pages()，否则用 get_user_pages()。 */
#define GUP_TEST_FLAG_DUMP_PAGES_USE_PIN	0x1

/*
 * struct gup_test 描述一次短生命周期 GUP/PUP 基准或页转储请求。
 *
 * 生命周期与所有权：用户态拥有结构体和目标虚拟地址区间；ioctl 入口先把
 * 结构体复制到内核栈，__gup_test_ioctl() 临时取得 page 引用或 DMA pin，
 * 测量后在返回前全部 put/unpin，最后把输出字段复制回用户态。因此该结构体
 * 不转移目标内存所有权，也不让 page 引用跨 ioctl 存活。
 *
 * 字段约定：
 * - get_delta_usec/put_delta_usec：内核输出，分别是取得和归还所有页的微秒数；
 * - addr/size：addr 是输入起始用户虚拟地址；size 输入请求字节数，成功时
 *   回写实际遍历的字节数，调用者不能再把它当成原始请求长度；
 * - nr_pages_per_call：每批交给 GUP/PUP helper 的页数，决定循环粒度；
 * - gup_flags：直接传给 GUP/PUP 的 FOLL_* 请求位；长期命令由内核再补
 *   FOLL_LONGTERM；
 * - test_flags：仅选择测试层行为，目前只控制 DUMP 使用 get 还是 pin；
 * - which_pages：相对 addr 的 1-based 页号；0 跳过，越界项由内核清零后回写。
 */
struct gup_test {
	__u64 get_delta_usec;
	__u64 put_delta_usec;
	__u64 addr;
	__u64 size;
	__u32 nr_pages_per_call;
	__u32 gup_flags;
	__u32 test_flags;
	/*
	 * Each non-zero entry is the number of the page (1-based: first page is
	 * page 1, so that zero entries mean "do nothing") from the .addr base.
	 */
	/*
	 * 每个非零元素都是相对 .addr 的页编号，采用从 1 开始的编码：第一页为
	 * 1，因而 0 可以明确表示“不执行”。dump_pages_test() 使用前会检查它
	 * 不超过本次实际取得的页数，再减一转换成 page 指针数组下标。
	 */
	__u32 which_pages[GUP_TEST_MAX_PAGES_TO_DUMP];
};

/*
 * 长期 pin 的策略位：USE_WRITE 使内核附加 FOLL_WRITE，验证写访问语义；
 * USE_FAST 选择 pin_user_pages_fast()，未设置时在 mmap_read_lock 下调用普通
 * pin_user_pages()。其他位会被 START 拒绝，避免 ABI 静默接受未知行为。
 */
#define PIN_LONGTERM_TEST_FLAG_USE_WRITE	1
#define PIN_LONGTERM_TEST_FLAG_USE_FAST		2

/*
 * struct pin_longterm_test 是 START 命令的纯输入参数。
 * addr/size 以字节为单位，二者都必须页对齐，size 必须非零且不超过
 * LONG_MAX；flags 只能由上面两个策略位组成。START 成功后内核持有对应页的
 * long-term DMA pin，直到 STOP、同一 debugfs 文件 release 或失败回滚。
 */
struct pin_longterm_test {
	__u64 addr;
	__u64 size;
	__u32 flags;
};

/* 结束 include guard；它防止 ABI 声明被同一编译单元重复定义。 */
#endif	/* __GUP_TEST_H */
