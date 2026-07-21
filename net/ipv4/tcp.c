// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * TCP Echo 字节流接口学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件连接用户 send/recv 语义与 TCP 内部队列。发送侧把 iov_iter 中的字节
 * 复制或引用到 write queue，为其分配序列号并在满足 push 条件时交给输出引擎；
 * 接收侧从 receive queue 按 copied_seq 复制给用户并释放接收窗口。
 *
 * 关键不变量：send 返回只表示 TCP 接受了字节；snd_una <= snd_nxt <= write_seq；
 * recv 只能交付从 copied_seq 开始的连续字节。进程上下文以 lock_sock() 串行，
 * 收包软中断遇到用户 owner 时进入 sk_backlog，release_sock() 后再处理。
 */
/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Implementation of the Transmission Control Protocol(TCP).
 *
 * Authors:	Ross Biro
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Mark Evans, <evansmp@uhura.aston.ac.uk>
 *		Corey Minyard <wf-rch!minyard@relay.EU.net>
 *		Florian La Roche, <flla@stud.uni-sb.de>
 *		Charles Hedrick, <hedrick@klinzhai.rutgers.edu>
 *		Linus Torvalds, <torvalds@cs.helsinki.fi>
 *		Alan Cox, <gw4pts@gw4pts.ampr.org>
 *		Matthew Dillon, <dillon@apollo.west.oic.com>
 *		Arnt Gulbrandsen, <agulbra@nvg.unit.no>
 *		Jorge Cwik, <jorge@laser.satlink.net>
 *
 * Fixes:
 *		Alan Cox	:	Numerous verify_area() calls
 *		Alan Cox	:	Set the ACK bit on a reset
 *		Alan Cox	:	Stopped it crashing if it closed while
 *					sk->inuse=1 and was trying to connect
 *					(tcp_err()).
 *		Alan Cox	:	All icmp error handling was broken
 *					pointers passed where wrong and the
 *					socket was looked up backwards. Nobody
 *					tested any icmp error code obviously.
 *		Alan Cox	:	tcp_err() now handled properly. It
 *					wakes people on errors. poll
 *					behaves and the icmp error race
 *					has gone by moving it into sock.c
 *		Alan Cox	:	tcp_send_reset() fixed to work for
 *					everything not just packets for
 *					unknown sockets.
 *		Alan Cox	:	tcp option processing.
 *		Alan Cox	:	Reset tweaked (still not 100%) [Had
 *					syn rule wrong]
 *		Herp Rosmanith  :	More reset fixes
 *		Alan Cox	:	No longer acks invalid rst frames.
 *					Acking any kind of RST is right out.
 *		Alan Cox	:	Sets an ignore me flag on an rst
 *					receive otherwise odd bits of prattle
 *					escape still
 *		Alan Cox	:	Fixed another acking RST frame bug.
 *					Should stop LAN workplace lockups.
 *		Alan Cox	: 	Some tidyups using the new skb list
 *					facilities
 *		Alan Cox	:	sk->keepopen now seems to work
 *		Alan Cox	:	Pulls options out correctly on accepts
 *		Alan Cox	:	Fixed assorted sk->rqueue->next errors
 *		Alan Cox	:	PSH doesn't end a TCP read. Switched a
 *					bit to skb ops.
 *		Alan Cox	:	Tidied tcp_data to avoid a potential
 *					nasty.
 *		Alan Cox	:	Added some better commenting, as the
 *					tcp is hard to follow
 *		Alan Cox	:	Removed incorrect check for 20 * psh
 *	Michael O'Reilly	:	ack < copied bug fix.
 *	Johannes Stille		:	Misc tcp fixes (not all in yet).
 *		Alan Cox	:	FIN with no memory -> CRASH
 *		Alan Cox	:	Added socket option proto entries.
 *					Also added awareness of them to accept.
 *		Alan Cox	:	Added TCP options (SOL_TCP)
 *		Alan Cox	:	Switched wakeup calls to callbacks,
 *					so the kernel can layer network
 *					sockets.
 *		Alan Cox	:	Use ip_tos/ip_ttl settings.
 *		Alan Cox	:	Handle FIN (more) properly (we hope).
 *		Alan Cox	:	RST frames sent on unsynchronised
 *					state ack error.
 *		Alan Cox	:	Put in missing check for SYN bit.
 *		Alan Cox	:	Added tcp_select_window() aka NET2E
 *					window non shrink trick.
 *		Alan Cox	:	Added a couple of small NET2E timer
 *					fixes
 *		Charles Hedrick :	TCP fixes
 *		Toomas Tamm	:	TCP window fixes
 *		Alan Cox	:	Small URG fix to rlogin ^C ack fight
 *		Charles Hedrick	:	Rewrote most of it to actually work
 *		Linus		:	Rewrote tcp_read() and URG handling
 *					completely
 *		Gerhard Koerting:	Fixed some missing timer handling
 *		Matthew Dillon  :	Reworked TCP machine states as per RFC
 *		Gerhard Koerting:	PC/TCP workarounds
 *		Adam Caldwell	:	Assorted timer/timing errors
 *		Matthew Dillon	:	Fixed another RST bug
 *		Alan Cox	:	Move to kernel side addressing changes.
 *		Alan Cox	:	Beginning work on TCP fastpathing
 *					(not yet usable)
 *		Arnt Gulbrandsen:	Turbocharged tcp_check() routine.
 *		Alan Cox	:	TCP fast path debugging
 *		Alan Cox	:	Window clamping
 *		Michael Riepe	:	Bug in tcp_check()
 *		Matt Dillon	:	More TCP improvements and RST bug fixes
 *		Matt Dillon	:	Yet more small nasties remove from the
 *					TCP code (Be very nice to this man if
 *					tcp finally works 100%) 8)
 *		Alan Cox	:	BSD accept semantics.
 *		Alan Cox	:	Reset on closedown bug.
 *	Peter De Schrijver	:	ENOTCONN check missing in tcp_sendto().
 *		Michael Pall	:	Handle poll() after URG properly in
 *					all cases.
 *		Michael Pall	:	Undo the last fix in tcp_read_urg()
 *					(multi URG PUSH broke rlogin).
 *		Michael Pall	:	Fix the multi URG PUSH problem in
 *					tcp_readable(), poll() after URG
 *					works now.
 *		Michael Pall	:	recv(...,MSG_OOB) never blocks in the
 *					BSD api.
 *		Alan Cox	:	Changed the semantics of sk->socket to
 *					fix a race and a signal problem with
 *					accept() and async I/O.
 *		Alan Cox	:	Relaxed the rules on tcp_sendto().
 *		Yury Shevchuk	:	Really fixed accept() blocking problem.
 *		Craig I. Hagan  :	Allow for BSD compatible TIME_WAIT for
 *					clients/servers which listen in on
 *					fixed ports.
 *		Alan Cox	:	Cleaned the above up and shrank it to
 *					a sensible code size.
 *		Alan Cox	:	Self connect lockup fix.
 *		Alan Cox	:	No connect to multicast.
 *		Ross Biro	:	Close unaccepted children on master
 *					socket close.
 *		Alan Cox	:	Reset tracing code.
 *		Alan Cox	:	Spurious resets on shutdown.
 *		Alan Cox	:	Giant 15 minute/60 second timer error
 *		Alan Cox	:	Small whoops in polling before an
 *					accept.
 *		Alan Cox	:	Kept the state trace facility since
 *					it's handy for debugging.
 *		Alan Cox	:	More reset handler fixes.
 *		Alan Cox	:	Started rewriting the code based on
 *					the RFC's for other useful protocol
 *					references see: Comer, KA9Q NOS, and
 *					for a reference on the difference
 *					between specifications and how BSD
 *					works see the 4.4lite source.
 *		A.N.Kuznetsov	:	Don't time wait on completion of tidy
 *					close.
 *		Linus Torvalds	:	Fin/Shutdown & copied_seq changes.
 *		Linus Torvalds	:	Fixed BSD port reuse to work first syn
 *		Alan Cox	:	Reimplemented timers as per the RFC
 *					and using multiple timers for sanity.
 *		Alan Cox	:	Small bug fixes, and a lot of new
 *					comments.
 *		Alan Cox	:	Fixed dual reader crash by locking
 *					the buffers (much like datagram.c)
 *		Alan Cox	:	Fixed stuck sockets in probe. A probe
 *					now gets fed up of retrying without
 *					(even a no space) answer.
 *		Alan Cox	:	Extracted closing code better
 *		Alan Cox	:	Fixed the closing state machine to
 *					resemble the RFC.
 *		Alan Cox	:	More 'per spec' fixes.
 *		Jorge Cwik	:	Even faster checksumming.
 *		Alan Cox	:	tcp_data() doesn't ack illegal PSH
 *					only frames. At least one pc tcp stack
 *					generates them.
 *		Alan Cox	:	Cache last socket.
 *		Alan Cox	:	Per route irtt.
 *		Matt Day	:	poll()->select() match BSD precisely on error
 *		Alan Cox	:	New buffers
 *		Marc Tamsky	:	Various sk->prot->retransmits and
 *					sk->retransmits misupdating fixed.
 *					Fixed tcp_write_timeout: stuck close,
 *					and TCP syn retries gets used now.
 *		Mark Yarvis	:	In tcp_read_wakeup(), don't send an
 *					ack if state is TCP_CLOSED.
 *		Alan Cox	:	Look up device on a retransmit - routes may
 *					change. Doesn't yet cope with MSS shrink right
 *					but it's a start!
 *		Marc Tamsky	:	Closing in closing fixes.
 *		Mike Shaver	:	RFC1122 verifications.
 *		Alan Cox	:	rcv_saddr errors.
 *		Alan Cox	:	Block double connect().
 *		Alan Cox	:	Small hooks for enSKIP.
 *		Alexey Kuznetsov:	Path MTU discovery.
 *		Alan Cox	:	Support soft errors.
 *		Alan Cox	:	Fix MTU discovery pathological case
 *					when the remote claims no mtu!
 *		Marc Tamsky	:	TCP_CLOSE fix.
 *		Colin (G3TNE)	:	Send a reset on syn ack replies in
 *					window but wrong (fixes NT lpd problems)
 *		Pedro Roque	:	Better TCP window handling, delayed ack.
 *		Joerg Reuter	:	No modification of locked buffers in
 *					tcp_do_retransmit()
 *		Eric Schenk	:	Changed receiver side silly window
 *					avoidance algorithm to BSD style
 *					algorithm. This doubles throughput
 *					against machines running Solaris,
 *					and seems to result in general
 *					improvement.
 *	Stefan Magdalinski	:	adjusted tcp_readable() to fix FIONREAD
 *	Willy Konynenberg	:	Transparent proxying support.
 *	Mike McLagan		:	Routing by source
 *		Keith Owens	:	Do proper merging with partial SKB's in
 *					tcp_do_sendmsg to avoid burstiness.
 *		Eric Schenk	:	Fix fast close down bug with
 *					shutdown() followed by close().
 *		Andi Kleen 	:	Make poll agree with SIGIO
 *	Salvatore Sanfilippo	:	Support SO_LINGER with linger == 1 and
 *					lingertime == 0 (RFC 793 ABORT Call)
 *	Hirokazu Takahashi	:	Use copy_from_user() instead of
 *					csum_and_copy_from_user() if possible.
 *
 * Description of States:
 *
 *	TCP_SYN_SENT		sent a connection request, waiting for ack
 *
 *	TCP_SYN_RECV		received a connection request, sent ack,
 *				waiting for final ack in three-way handshake.
 *
 *	TCP_ESTABLISHED		connection established
 *
 *	TCP_FIN_WAIT1		our side has shutdown, waiting to complete
 *				transmission of remaining buffered data
 *
 *	TCP_FIN_WAIT2		all buffered data sent, waiting for remote
 *				to shutdown
 *
 *	TCP_CLOSING		both sides have shutdown but we still have
 *				data we have to finish sending
 *
 *	TCP_TIME_WAIT		timeout to catch resent junk before entering
 *				closed, can only be entered from FIN_WAIT2
 *				or CLOSING.  Required because the other end
 *				may not have gotten our last ACK causing it
 *				to retransmit the data packet (which we ignore)
 *
 *	TCP_CLOSE_WAIT		remote side has shutdown and is waiting for
 *				us to finish writing our data and to shutdown
 *				(we have to close() to move on to LAST_ACK)
 *
 *	TCP_LAST_ACK		out side has shutdown after remote has
 *				shutdown.  There may still be data in our
 *				buffer that we have to finish sending
 *
 *	TCP_CLOSE		socket is finished
 */

#define pr_fmt(fmt) "TCP: " fmt

#include <crypto/md5.h>
#include <crypto/utils.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/poll.h>
#include <linux/inet_diag.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/skbuff.h>
#include <linux/splice.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/random.h>
#include <linux/memblock.h>
#include <linux/highmem.h>
#include <linux/cache.h>
#include <linux/err.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/errqueue.h>
#include <linux/static_key.h>
#include <linux/btf.h>

#include <net/icmp.h>
#include <net/inet_common.h>
#include <net/inet_ecn.h>
#include <net/tcp.h>
#include <net/tcp_ecn.h>
#include <net/mptcp.h>
#include <net/proto_memory.h>
#include <net/xfrm.h>
#include <net/ip.h>
#include <net/psp.h>
#include <net/sock.h>
#include <net/rstreason.h>

#include <linux/uaccess.h>
#include <asm/ioctls.h>
#include <net/busy_poll.h>
#include <net/hotdata.h>
#include <trace/events/tcp.h>
#include <net/rps.h>

#include "../core/devmem.h"

/* Track pending CMSGs. */
/* 位值记录解锁后仍需生成的 INQ/时间戳控制消息，避免持 socket lock 访问用户缓冲。 */
enum {
	TCP_CMSG_INQ = 1,
	TCP_CMSG_TS = 2
};

DEFINE_PER_CPU(unsigned int, tcp_orphan_count);

long sysctl_tcp_mem[3] __read_mostly;

DEFINE_PER_CPU(int, tcp_memory_per_cpu_fw_alloc);
EXPORT_PER_CPU_SYMBOL_GPL(tcp_memory_per_cpu_fw_alloc);

#if IS_ENABLED(CONFIG_SMC)
DEFINE_STATIC_KEY_FALSE(tcp_have_smc);
EXPORT_SYMBOL(tcp_have_smc);
#endif

/*
 * Current number of TCP sockets.
 */
/* percpu_counter 让创建/销毁热路径分散更新，读取总数时才跨 CPU 汇总。 */
struct percpu_counter tcp_sockets_allocated ____cacheline_aligned_in_smp;

/*
 * Pressure flag: try to collapse.
 * Technical note: it is used by multiple contexts non atomically.
 * All the __sk_mem_schedule() is of this nature: accounting
 * is strict, actions are advisory and have some latency.
 */
unsigned long tcp_memory_pressure __read_mostly;
EXPORT_SYMBOL_GPL(tcp_memory_pressure);

/*
 * tcp_enter_memory_pressure - 首次发布 TCP 协议内存压力及其开始时间。
 *
 * @sk 仅用于选择统计所在 netns，不转移 ownership。全局值为 0 表示无压力，
 * 非零保存开始 jiffies；cmpxchg 让多个并发失败者中只有一个完成 0->timestamp
 * 转换并增加次数统计。这里不要求精确同步：内存记账必须严格，但 pressure 只是
 * 促使各 socket 更积极 collapse/prune 的滞后策略信号。
 */
void tcp_enter_memory_pressure(struct sock *sk)
{
	unsigned long val;

	if (READ_ONCE(tcp_memory_pressure))
		return;
	val = jiffies;

	if (!val)
		val--;
	if (!cmpxchg(&tcp_memory_pressure, 0, val))
		NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPMEMORYPRESSURES);
}

/*
 * tcp_leave_memory_pressure - 原子结束一次压力区间并累计持续时间。
 *
 * xchg 同时取得旧开始时间和发布 0，只有真正观察到非零的调用者记时；与 enter
 * 竞态时允许策略信号略有延迟，但不会破坏协议内存的实际页数记账。
 */
void tcp_leave_memory_pressure(struct sock *sk)
{
	unsigned long val;

	if (!READ_ONCE(tcp_memory_pressure))
		return;
	val = xchg(&tcp_memory_pressure, 0);
	if (val)
		NET_ADD_STATS(sock_net(sk), LINUX_MIB_TCPMEMORYPRESSURESCHRONO,
			      jiffies_to_msecs(jiffies - val));
}

/* Convert seconds to retransmits based on initial and max timeout */
/*
 * 把用户给出的秒数换算为“初始 timeout、逐轮翻倍且封顶 rto_max”模型下的重传
 * 次数。返回 u8 并在 255 饱和；它是配置表示转换，不读取连接实时 RTT。
 */
static u8 secs_to_retrans(int seconds, int timeout, int rto_max)
{
	u8 res = 0;

	if (seconds > 0) {
		int period = timeout;

		res = 1;
		while (seconds > period && res < 255) {
			res++;
			timeout <<= 1;
			if (timeout > rto_max)
				timeout = rto_max;
			period += timeout;
		}
	}
	return res;
}

/* Convert retransmits to seconds based on initial and max timeout */
/* 与 secs_to_retrans 互为近似逆变换；累计每轮退避时长，封顶后按 rto_max 线性增加。 */
static int retrans_to_secs(u8 retrans, int timeout, int rto_max)
{
	int period = 0;

	if (retrans > 0) {
		period = timeout;
		while (--retrans) {
			timeout <<= 1;
			if (timeout > rto_max)
				timeout = rto_max;
			period += timeout;
		}
	}
	return period;
}

/*
 * tcp_compute_delivery_rate - 把最近 rate sample 转为每秒交付字节数。
 *
 * rate_delivered 的单位是 MSS 个数，rate_interval_us 是微秒；先扩成 u64 再乘 MSS
 * 和 USEC_PER_SEC，避免 32 位溢出。任一采样为零表示尚无可用速率，返回 0。
 * READ_ONCE 只保证字段单次读取，不保证两个字段来自同一个瞬时快照。
 */
static u64 tcp_compute_delivery_rate(const struct tcp_sock *tp)
{
	u32 rate = READ_ONCE(tp->rate_delivered);
	u32 intv = READ_ONCE(tp->rate_interval_us);
	u64 rate64 = 0;

	if (rate && intv) {
		rate64 = (u64)rate * tp->mss_cache * USEC_PER_SEC;
		do_div(rate64, intv);
	}
	return rate64;
}

#ifdef CONFIG_TCP_MD5SIG
/*
 * tcp_md5_destruct_sock - 从 socket 撤销全部 TCP-MD5 key 并关闭全局静态分支引用。
 *
 * 析构上下文保证 sk 不再被普通用户修改。先清 key list，再用 rcu_replace_pointer
 * 使新读者不可发现 md5sig_info，最后释放容器；static key 延迟递减避免在原子
 * 热路径同步 patch 指令。配置关闭时整个函数不存在，调用点由条件编译约束。
 */
void tcp_md5_destruct_sock(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (tp->md5sig_info) {

		tcp_clear_md5_list(sk);
		kfree(rcu_replace_pointer(tp->md5sig_info, NULL, 1));
		static_branch_slow_dec_deferred(&tcp_md5_needed);
	}
}
#endif

/* Address-family independent initialization for a tcp_sock.
 *
 * NOTE: A lot of things set to zero explicitly by call to
 *       sk_alloc() so need not be done here.
 */
/*
 * tcp_init_sock - 把刚由 sk_alloc 清零的通用 sock 初始化成可用 tcp_sock。
 *
 * @sk 已分配但尚未对连接 hash/用户发布，函数无失败返回。它建立 OOO/重传树、
 * timer/list、RTO/RTT 初值、cwnd/ssthresh/MSS、拥塞算法、读写 buffer、zero-copy
 * 能力和 socket 计数。显式字段只是非零默认值；其余依赖 sk_alloc 的清零契约。
 * 返回后对象仍未 bind/connect，地址和序列号由后续状态机填写。
 *
 * 初始化顺序避免 timer 或回调看到半初始化队列；sk_sockets_allocated_inc 和 xarray
 * 初始化建立后续析构必须对称撤销的资源。默认 buffer 来自当前 netns sysctl，
 * 每 socket setsockopt 或 autotuning 可再覆盖。
 */
void tcp_init_sock(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	int rto_min_us, rto_max_ms;

	tp->out_of_order_queue = RB_ROOT;
	sk->tcp_rtx_queue = RB_ROOT;
	tcp_init_xmit_timers(sk);
	INIT_LIST_HEAD(&tp->tsq_node);
	INIT_LIST_HEAD(&tp->tsorted_sent_queue);

	icsk->icsk_rto = TCP_TIMEOUT_INIT;

	rto_max_ms = READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_rto_max_ms);
	icsk->icsk_rto_max = msecs_to_jiffies(rto_max_ms);

	rto_min_us = READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_rto_min_us);
	icsk->icsk_rto_min = usecs_to_jiffies(rto_min_us);
	icsk->icsk_delack_max = TCP_DELACK_MAX;
	tp->mdev_us = jiffies_to_usecs(TCP_TIMEOUT_INIT);
	minmax_reset(&tp->rtt_min, tcp_jiffies32, ~0U);

	/* So many TCP implementations out there (incorrectly) count the
	 * initial SYN frame in their delayed-ACK and congestion control
	 * algorithms that we must have the following bandaid to talk
	 * efficiently to them.  -DaveM
	 */
	/* 兼容部分把 SYN 错算进 delayed-ACK/拥塞控制的对端，初始 cwnd 采用传统默认值。 */
	tcp_snd_cwnd_set(tp, TCP_INIT_CWND);

	/* There's a bubble in the pipe until at least the first ACK. */
	/* 首个 ACK 前不存在可靠 delivery-rate 样本，用哨兵标记应用受限区间。 */
	tp->app_limited = ~0U;
	tp->rate_app_limited = 1;

	/* See draft-stevens-tcpca-spec-01 for discussion of the
	 * initialization of these values.
	 */
	/* ssthresh 先视为无限，cwnd clamp 不限，MSS 在 route/握手确定前使用安全默认。 */
	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	tp->snd_cwnd_clamp = ~0;
	tp->mss_cache = TCP_MSS_DEFAULT;

	tp->reordering = READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_reordering);
	tcp_assign_congestion_control(sk);

	tp->tsoffset = 0;
	tp->rack.reo_wnd_steps = 1;

	sk->sk_write_space = sk_stream_write_space;
	sock_set_flag(sk, SOCK_USE_WRITE_QUEUE);

	icsk->icsk_sync_mss = tcp_sync_mss;

	WRITE_ONCE(sk->sk_sndbuf, READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_wmem[1]));
	WRITE_ONCE(sk->sk_rcvbuf, READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_rmem[1]));
	tcp_scaling_ratio_init(sk);

	set_bit(SOCK_SUPPORT_ZC, &sk->sk_socket->flags);
	sk_sockets_allocated_inc(sk);
	xa_init_flags(&sk->sk_user_frags, XA_FLAGS_ALLOC1);
}

/*
 * tcp_tx_timestamp - 把本次 sendmsg 的发送时间戳请求绑定到最后一个序列字节。
 *
 * @sockc 是已解析控制消息的借用对象；函数不分配 skb。优先 write queue 尾，若
 * 数据已发送则取重传树尾。SOF_TIMESTAMPING_TX_ACK 记录“等 ACK 后通知”，tskey
 * 用 skb 起始 seq + payload len - 1 标识完成边界；没有 skb 时请求无法绑定。
 * BPF sockops 可观察同一发送事件，但不取得 skb ownership。
 */
static void tcp_tx_timestamp(struct sock *sk, struct sockcm_cookie *sockc)
{
	struct sk_buff *skb = tcp_write_queue_tail(sk);
	u32 tsflags = sockc->tsflags;

	if (unlikely(!skb))
		skb = skb_rb_last(&sk->tcp_rtx_queue);

	if (tsflags && skb) {
		struct skb_shared_info *shinfo = skb_shinfo(skb);
		struct tcp_skb_cb *tcb = TCP_SKB_CB(skb);

		sock_tx_timestamp(sk, sockc, &shinfo->tx_flags);
		if (tsflags & SOF_TIMESTAMPING_TX_ACK)
			tcb->txstamp_ack |= TSTAMP_ACK_SK;
		if (tsflags & SOF_TIMESTAMPING_TX_RECORD_MASK)
			shinfo->tskey = TCP_SKB_CB(skb)->seq + skb->len - 1;
	}

	if (cgroup_bpf_enabled(CGROUP_SOCK_OPS) &&
	    SK_BPF_CB_FLAG_TEST(sk, SK_BPF_CB_TX_TIMESTAMPING) && skb)
		bpf_skops_tx_timestamping(sk, skb, BPF_SOCK_OPS_TSTAMP_SENDMSG_CB);
}

/* @wake is one when sk_stream_write_space() calls us.
 * This sends EPOLLOUT only if notsent_bytes is half the limit.
 * This mimics the strategy used in sock_def_write_space().
 */
/*
 * 判断 poll/send waiter 是否应把“未发送字节已降到 TCP_NOTSENT_LOWAT 以下”视为
 * 可写。@wake=1 时阈值等效减半，给边沿触发通知留出迟滞，避免在临界点频繁唤醒。
 * write_seq-snd_nxt 使用序列号模运算得到仍未提交网络的字节，不等于未 ACK 字节。
 */
bool tcp_stream_memory_free(const struct sock *sk, int wake)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	u32 notsent_bytes = READ_ONCE(tp->write_seq) - READ_ONCE(tp->snd_nxt);

	return (notsent_bytes << wake) < tcp_notsent_lowat(tp);
}
EXPORT_SYMBOL(tcp_stream_memory_free);

/* 达到 TCP low-water 或通用 socket 的 error/EOF 条件任一成立，poll 即可读。 */
static bool tcp_stream_is_readable(struct sock *sk, int target)
{
	if (tcp_epollin_ready(sk, target))
		return true;
	return sk_is_readable(sk);
}

/*
 *	Wait for a TCP event.
 *
 *	Note that we don't need to lock the socket, as the upper poll layers
 *	take care of normal races (between the test and the event) and we don't
 *	go look at any of the socket buffers directly.
 */
/*
 * tcp_poll - 在不取得 socket lock 的情况下生成当前 epoll/poll 事件快照。
 *
 * @file/@sock 在 poll 调用期间固定，@wait 用于把当前任务注册进 sk_sleep；返回
 * EPOLL* 位集合，允许状态返回后立刻变化。正确性来自“先注册 waiter，再读取条件”
 * 的 poll 协议和各生产者的唤醒/内存屏障，不要求跨字段一致快照。
 *
 * RCV_SHUTDOWN 同时给 EPOLLIN/RDHUP，使队列排空后的 EOF 仍立即可读；只有双向
 * shutdown 或 TCP_CLOSE 才给不可屏蔽 HUP，保留 CLOSE_WAIT 的写能力。写空间不足
 * 时先设置 NOSPACE 位，再以屏障后二次检查打破释放空间与发布 waiter 的竞态。
 */
__poll_t tcp_poll(struct file *file, struct socket *sock, poll_table *wait)
{
	__poll_t mask;
	struct sock *sk = sock->sk;
	const struct tcp_sock *tp = tcp_sk(sk);
	u8 shutdown;
	int state;

	sock_poll_wait(file, sock, wait);

	state = inet_sk_state_load(sk);
	if (state == TCP_LISTEN)
		return inet_csk_listen_poll(sk);

	/* Socket is not locked. We are protected from async events
	 * by poll logic and correct handling of state changes
	 * made by other threads is impossible in any case.
	 */
	/* poll 只生成瞬时提示；注册 waitqueue 后的状态变化由对应 wakeup 保证不会永久遗漏。 */

	mask = 0;

	/*
	 * EPOLLHUP is certainly not done right. But poll() doesn't
	 * have a notion of HUP in just one direction, and for a
	 * socket the read side is more interesting.
	 *
	 * Some poll() documentation says that EPOLLHUP is incompatible
	 * with the EPOLLOUT/POLLWR flags, so somebody should check this
	 * all. But careful, it tends to be safer to return too many
	 * bits than too few, and you can easily break real applications
	 * if you don't tell them that something has hung up!
	 *
	 * Check-me.
	 *
	 * Check number 1. EPOLLHUP is _UNMASKABLE_ event (see UNIX98 and
	 * our fs/select.c). It means that after we received EOF,
	 * poll always returns immediately, making impossible poll() on write()
	 * in state CLOSE_WAIT. One solution is evident --- to set EPOLLHUP
	 * if and only if shutdown has been made in both directions.
	 * Actually, it is interesting to look how Solaris and DUX
	 * solve this dilemma. I would prefer, if EPOLLHUP were maskable,
	 * then we could set it on SND_SHUTDOWN. BTW examples given
	 * in Stevens' books assume exactly this behaviour, it explains
	 * why EPOLLHUP is incompatible with EPOLLOUT.	--ANK
	 *
	 * NOTE. Check for TCP_CLOSE is added. The goal is to prevent
	 * blocking on fresh not-connected or disconnected socket. --ANK
	 */
	/*
	 * poll 没有“仅一个方向 HUP”的表达。Linux 仅在双向 shutdown/CLOSE 报 HUP，
	 * 单独收到 FIN 则用 RDHUP+IN，保留 CLOSE_WAIT 下继续写的能力；宁可多报可重查
	 * 的事件，也不能少报导致应用永久阻塞。
	 */
	shutdown = READ_ONCE(sk->sk_shutdown);
	if (shutdown == SHUTDOWN_MASK || state == TCP_CLOSE)
		mask |= EPOLLHUP;
	if (shutdown & RCV_SHUTDOWN)
		mask |= EPOLLIN | EPOLLRDNORM | EPOLLRDHUP;

	/* Connected or passive Fast Open socket? */
	/* 握手完成或 TFO child 已存在时才按普通连接检查数据与写空间。 */
	if (state != TCP_SYN_SENT &&
	    (state != TCP_SYN_RECV || rcu_access_pointer(tp->fastopen_rsk))) {
		int target = sock_rcvlowat(sk, 0, INT_MAX);
		u16 urg_data = READ_ONCE(tp->urg_data);

		if (unlikely(urg_data) &&
		    READ_ONCE(tp->urg_seq) == READ_ONCE(tp->copied_seq) &&
		    !sock_flag(sk, SOCK_URGINLINE))
			target++;

		if (tcp_stream_is_readable(sk, target))
			mask |= EPOLLIN | EPOLLRDNORM;

		if (!(shutdown & SEND_SHUTDOWN)) {
			if (__sk_stream_is_writeable(sk, 1)) {
				mask |= EPOLLOUT | EPOLLWRNORM;
			} else {  /* send SIGIO later */
				/* 先登记异步缺空间，释放 wmem 的路径才知道需要发送 SIGIO/EPOLLOUT。 */
				sk_set_bit(SOCKWQ_ASYNC_NOSPACE, sk);
				set_bit(SOCK_NOSPACE, &sk->sk_socket->flags);

				/* Race breaker. If space is freed after
				 * wspace test but before the flags are set,
				 * IO signal will be lost. Memory barrier
				 * pairs with the input side.
				 */
				/* 原子位置位后屏障，再检查一次空间，闭合“先释放、后登记”的丢失通知窗口。 */
				smp_mb__after_atomic();
				if (__sk_stream_is_writeable(sk, 1))
					mask |= EPOLLOUT | EPOLLWRNORM;
			}
		} else
			mask |= EPOLLOUT | EPOLLWRNORM;

		if (urg_data & TCP_URG_VALID)
			mask |= EPOLLPRI;
	} else if (state == TCP_SYN_SENT &&
		   inet_test_bit(DEFER_CONNECT, sk)) {
		/* Active TCP fastopen socket with defer_connect
		 * Return EPOLLOUT so application can call write()
		 * in order for kernel to generate SYN+data
		 */
		/* 延迟 connect 的 TFO 需要应用 write 才生成 SYN+data，因此提前报告可写。 */
		mask |= EPOLLOUT | EPOLLWRNORM;
	}
	/* This barrier is coupled with smp_wmb() in tcp_done_with_error() */
	/* 先读 shutdown/队列，再以读屏障观察错误；与错误发布写屏障共同保证不漏 EPOLLERR。 */
	smp_rmb();
	if (READ_ONCE(sk->sk_err) ||
	    !skb_queue_empty_lockless(&sk->sk_error_queue))
		mask |= EPOLLERR;

	return mask;
}
EXPORT_SYMBOL(tcp_poll);

/*
 * tcp_ioctl - 实现 TCP 字节队列相关的 SIOCINQ/OUTQ/OUTQNSD/ATMARK 查询。
 *
 * @karg 是内核输出指针，不是用户地址；上层负责 copy_to_user。SIOCINQ 需要短暂
 * socket lock 取得接收可读量；ATMARK 比较 urgent 与 copied 游标；OUTQ 返回
 * write_seq-snd_una（已写但未 ACK），OUTQNSD 返回 write_seq-snd_nxt（尚未发送）。
 * LISTEN/未知命令返回错误。无锁序号读取是诊断快照，不承诺多个字段原子一致。
 */
int tcp_ioctl(struct sock *sk, int cmd, int *karg)
{
	struct tcp_sock *tp = tcp_sk(sk);
	int answ;
	bool slow;

	switch (cmd) {
	case SIOCINQ:
		if (sk->sk_state == TCP_LISTEN)
			return -EINVAL;

		slow = lock_sock_fast(sk);
		answ = tcp_inq(sk);
		unlock_sock_fast(sk, slow);
		break;
	case SIOCATMARK:
		answ = READ_ONCE(tp->urg_data) &&
		       READ_ONCE(tp->urg_seq) == READ_ONCE(tp->copied_seq);
		break;
	case SIOCOUTQ:
		if (sk->sk_state == TCP_LISTEN)
			return -EINVAL;

		if ((1 << sk->sk_state) & (TCPF_SYN_SENT | TCPF_SYN_RECV))
			answ = 0;
		else
			answ = READ_ONCE(tp->write_seq) - tp->snd_una;
		break;
	case SIOCOUTQNSD:
		if (sk->sk_state == TCP_LISTEN)
			return -EINVAL;

		if ((1 << sk->sk_state) & (TCPF_SYN_SENT | TCPF_SYN_RECV))
			answ = 0;
		else
			answ = READ_ONCE(tp->write_seq) -
			       READ_ONCE(tp->snd_nxt);
		break;
	default:
		return -ENOIOCTLCMD;
	}

	*karg = answ;
	return 0;
}

/* 把当前尾 skb 标为 PSH，并记录本次 push 的 write_seq 边界供 forced_push 比较。 */
void tcp_mark_push(struct tcp_sock *tp, struct sk_buff *skb)
{
	TCP_SKB_CB(skb)->tcp_flags |= TCPHDR_PSH;
	tp->pushed_seq = tp->write_seq;
}

static inline bool forced_push(const struct tcp_sock *tp)
{
	return after(tp->write_seq, tp->pushed_seq + (tp->max_window >> 1));
}

/*
 * tcp_skb_entail - 把一个新建 headerless skb 发布到 TCP write queue 尾部。
 *
 * 调用者：tcp_sendmsg_locked() 在分配新 skb 后、复制任何 payload 前调用。
 * @sk 由 lock_sock 串行化；@skb 必须尚未属于其他队列，调用成功后 ownership
 * 转给 TCP write queue。函数无失败返回：所需内存已在分配 skb 时取得。
 *
 * 入口：write_seq=W，skb 未覆盖序列空间、未做 socket queued-memory 记账。
 * 出口：skb 表示空区间 [W,W)，默认携带 ACK，挂在 write queue 尾；truesize
 * 同时计入 sk_wmem_queued 和协议内存。后续 copy 成功才扩大 end_seq/write_seq；
 * 若一直为空，错误路径必须 tcp_remove_empty_skb()，不能裸 kfree。
 */
void tcp_skb_entail(struct sock *sk, struct sk_buff *skb)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct tcp_skb_cb *tcb = TCP_SKB_CB(skb);

	/* 链式赋值从右向左：seq/end_seq 同取当前 write_seq，形成空的右开区间。 */
	tcb->seq     = tcb->end_seq = tp->write_seq;
	tcb->tcp_flags = TCPHDR_ACK;
	/* 声明 payload/header ownership 布局允许后续 clone 在头部 COW，而不复制 frags。 */
	__skb_header_release(skb);
	psp_enqueue_set_decrypted(sk, skb);
	/* 侵入式挂链是本函数的发布点；从此 skb 必须通过 TCP queue helper 移除。 */
	tcp_add_write_queue_tail(sk, skb);
	/* queued 是 socket 逻辑发送队列记账；sk_mem_charge 是协议全局/每 socket 配额。 */
	sk_wmem_queued_add(sk, skb->truesize);
	sk_mem_charge(sk, skb->truesize);
	if (tp->nonagle & TCP_NAGLE_PUSH)
		/* 一次性 PUSH 策略由这个新 skb 消费，避免永久禁用 Nagle。 */
		tp->nonagle &= ~TCP_NAGLE_PUSH;

	/* 长时间 idle 后按策略降低 cwnd，不能沿用旧路径容量突然发送大 burst。 */
	tcp_slow_start_after_idle_check(sk);
}

static inline void tcp_mark_urg(struct tcp_sock *tp, int flags)
{
	if (flags & MSG_OOB)
		tp->snd_up = tp->write_seq;
}

/* If a not yet filled skb is pushed, do not send it if
 * we have data packets in Qdisc or NIC queues :
 * Because TX completion will happen shortly, it gives a chance
 * to coalesce future sendmsg() payload into this skb, without
 * need for a timer, and with no latency trade off.
 * As packets containing data payload have a bigger truesize
 * than pure acks (dataless) packets, the last checks prevent
 * autocorking if we only have an ACK in Qdisc/NIC queues,
 * or if TX completion was delayed after we processed ACK packet.
 */
/*
 * 自动 cork 只延迟尚未达到 GSO 目标的尾 skb，且必须确认 qdisc/NIC 中仍有本连接
 * payload。等待旧 TX completion 的短时间内，新 send 可与尾 skb 合并，减少小包；
 * 不使用定时器，因而通常不增加可见延迟。额外的 wmem/truesize 与 collapse 检查
 * 排除“只有纯 ACK 在途”或 skb 已不可合并的情况。
 */
static bool tcp_should_autocork(struct sock *sk, struct sk_buff *skb,
				int size_goal)
{
	return skb->len < size_goal &&
	       READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_autocorking) &&
	       !tcp_rtx_queue_empty(sk) &&
	       refcount_read(&sk->sk_wmem_alloc) > skb->truesize &&
	       tcp_skb_can_collapse_to(skb);
}

/*
 * tcp_push - 把一次 send 的 MSG_MORE/OOB/Nagle 策略落实到尾 skb 并尝试输出。
 *
 * @sk 已持 socket lock；@flags 是消息标志；@mss_now/@size_goal 分别是线速 segment
 * 与 GSO 聚合目标；@nonagle 是调用者初始 Nagle 策略。无尾 skb 时无副作用。
 * 非 MSG_MORE 或累计超过半个窗口时设置 PSH；MSG_OOB 更新 urgent pointer。
 * autocork 成功会设置 TSQ_THROTTLED 并暂返，TX completion 负责重新推动；屏障后
 * 二次检查关闭 completion 先发生造成的永久停顿。否则进入 tcp_write_xmit。
 */
void tcp_push(struct sock *sk, int flags, int mss_now,
	      int nonagle, int size_goal)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *skb;

	skb = tcp_write_queue_tail(sk);
	if (!skb)
		return;
	if (!(flags & MSG_MORE) || forced_push(tp))
		tcp_mark_push(tp, skb);

	tcp_mark_urg(tp, flags);

	if (tcp_should_autocork(sk, skb, size_goal)) {

		/* avoid atomic op if TSQ_THROTTLED bit is already set */
		/* 位已设置时跳过重复原子 RMW，降低 send 热路径 cacheline 争用。 */
		if (!test_bit(TSQ_THROTTLED, &sk->sk_tsq_flags)) {
			NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPAUTOCORKING);
			set_bit(TSQ_THROTTLED, &sk->sk_tsq_flags);
			smp_mb__after_atomic();
		}
		/* It is possible TX completion already happened
		 * before we set TSQ_THROTTLED.
		 */
		/* completion 可能早于节流位发布；复查 wmem，已排空则不能返回等待不存在的唤醒。 */
		if (refcount_read(&sk->sk_wmem_alloc) > skb->truesize)
			return;
	}

	if (flags & MSG_MORE)
		nonagle = TCP_NAGLE_CORK;

	__tcp_push_pending_frames(sk, mss_now, nonagle);
}

/*
 * tcp_splice_data_recv - tcp_read_sock 的 actor，把 skb 页引用接入 pipe 而非复制。
 *
 * @offset/@len 描述当前 skb 可交付区间，实际最多取 rd_desc->count；正返回值为已
 * splice 字节并同步扣减剩余额度，0/负值让外层停止或报错。pipe buffer 持有页面
 * 引用，因此原 receive skb 后续可释放但页面直到 pipe 消费完成才回收。
 */
int tcp_splice_data_recv(read_descriptor_t *rd_desc, struct sk_buff *skb,
			 unsigned int offset, size_t len)
{
	struct tcp_splice_state *tss = rd_desc->arg.data;
	int ret;

	ret = skb_splice_bits(skb, skb->sk, offset, tss->pipe,
			      min(rd_desc->count, len), tss->flags);
	if (ret > 0)
		rd_desc->count -= ret;
	return ret;
}

/* 把 splice 状态包装成 read_descriptor，并复用通用 tcp_read_sock 队列遍历。 */
static int __tcp_splice_read(struct sock *sk, struct tcp_splice_state *tss)
{
	/* Store TCP splice context information in read_descriptor_t. */
	/* read_descriptor 把剩余长度和 pipe 私有状态统一传给逐 skb actor。 */
	read_descriptor_t rd_desc = {
		.arg.data = tss,
		.count	  = tss->len,
	};

	return tcp_read_sock(sk, &rd_desc, tcp_splice_data_recv);
}

/**
 *  tcp_splice_read - splice data from TCP socket to a pipe
 * @sock:	socket to splice from
 * @ppos:	position (not valid)
 * @pipe:	pipe to splice to
 * @len:	number of bytes to splice
 * @flags:	splice modifier flags
 *
 * Description:
 *    Will read pages from given socket and fill them into a pipe.
 *
 **/
/*
 * tcp_splice_read - 实现 socket 到 pipe 的零拷贝式读取循环。
 *
 * socket 不可 seek，非零 @ppos 返回 ESPIPE。函数持 lock_sock 遍历 receive queue；
 * 无数据时按 DONE/error/shutdown/CLOSE/nonblock 顺序决定 EOF 或 errno，再用
 * sk_wait_data 安全睡眠。已有进度优先返回短读，后续错误留给下次调用。每批后
 * 释放再重取 socket，让 backlog/其他线程取得进展；splice 只转页面引用，不保证
 * 下游 pipe 消费完成。
 */
ssize_t tcp_splice_read(struct socket *sock, loff_t *ppos,
			struct pipe_inode_info *pipe, size_t len,
			unsigned int flags)
{
	struct sock *sk = sock->sk;
	struct tcp_splice_state tss = {
		.pipe = pipe,
		.len = len,
		.flags = flags,
	};
	long timeo;
	ssize_t spliced;
	int ret;

	sock_rps_record_flow(sk);
	/*
	 * We can't seek on a socket input
	 */
	/* TCP 是字节流当前游标接口，没有文件偏移语义。 */
	if (unlikely(*ppos))
		return -ESPIPE;

	ret = spliced = 0;

	lock_sock(sk);

	timeo = sock_rcvtimeo(sk, sock->file->f_flags & O_NONBLOCK);
	while (tss.len) {
		ret = __tcp_splice_read(sk, &tss);
		if (ret < 0)
			break;
		else if (!ret) {
			if (spliced)
				break;
			if (sock_flag(sk, SOCK_DONE))
				break;
			if (sk->sk_err) {
				ret = sock_error(sk);
				break;
			}
			if (sk->sk_shutdown & RCV_SHUTDOWN)
				break;
			if (sk->sk_state == TCP_CLOSE) {
				/*
				 * This occurs when user tries to read
				 * from never connected socket.
				 */
				/* 从未连接/已 disconnect 的 CLOSED socket 无 EOF 语义，返回 ENOTCONN。 */
				ret = -ENOTCONN;
				break;
			}
			if (!timeo) {
				ret = -EAGAIN;
				break;
			}
			/* if __tcp_splice_read() got nothing while we have
			 * an skb in receive queue, we do not want to loop.
			 * This might happen with URG data.
			 */
			/* 队列非空但 actor 无进度通常是 urgent 边界；继续循环会忙等。 */
			if (!skb_queue_empty(&sk->sk_receive_queue))
				break;
			ret = sk_wait_data(sk, &timeo, NULL);
			if (ret < 0)
				break;
			if (signal_pending(current)) {
				ret = sock_intr_errno(timeo);
				break;
			}
			continue;
		}
		tss.len -= ret;
		spliced += ret;

		if (!tss.len || !timeo)
			break;
		release_sock(sk);
		lock_sock(sk);

		if (tcp_recv_should_stop(sk))
			break;
	}

	release_sock(sk);

	if (spliced)
		return spliced;

	return ret;
}

/* We allow to exceed memory limits for FIN packets to expedite
 * connection tear down and (memory) recovery.
 * Otherwise tcp_send_fin() could be tempted to either delay FIN
 * or even be forced to close flow without any FIN.
 * In general, we want to allow one skb per socket to avoid hangs
 * with edge trigger epoll()
 */
/*
 * 强制从 forward_alloc/协议内存取得 @size 预算，不因普通 tcp_mem 上限失败。
 * 用于 FIN 或“至少一个 skb”进展保证：若连关闭控制包都因压力无法分配，连接会
 * 占用更多资源更久，边沿触发 writer 也可能再无唤醒。代价是短暂超限；memcg
 * 仍以 __GFP_NOFAIL 记账，协议全局页数同步增加。函数只做预算，不分配 skb。
 */
void sk_forced_mem_schedule(struct sock *sk, int size)
{
	int delta, amt;

	delta = size - sk->sk_forward_alloc;
	if (delta <= 0)
		return;

	amt = sk_mem_pages(delta);
	sk_forward_alloc_add(sk, amt << PAGE_SHIFT);

	if (mem_cgroup_sk_enabled(sk))
		mem_cgroup_sk_charge(sk, amt, gfp_memcg_charge() | __GFP_NOFAIL);

	if (sk->sk_bypass_prot_mem)
		return;

	sk_memory_allocated_add(sk, amt);
}

/*
 * tcp_stream_alloc_skb - 分配适合 TCP write queue 的 headerless fclone skb。
 *
 * @gfp 决定是否可睡眠；@force_schedule 选择普通内存准入或强制进展预算。成功
 * 返回未入队、由调用者独占的 skb：预留 MAX_TCP_HEADER headroom、设置 partial
 * checksum 并初始化时间排序节点；失败返回 NULL 且无 ownership 转移。元数据
 * 分配成功但记账失败时先释放 skb；分配本身失败会进入 memory pressure 并收缩
 * sndbuf，促使后续 send 等待而非无限争抢内存。
 */
struct sk_buff *tcp_stream_alloc_skb(struct sock *sk, gfp_t gfp,
				     bool force_schedule)
{
	struct sk_buff *skb;

	skb = alloc_skb_fclone(MAX_TCP_HEADER, gfp);
	if (likely(skb)) {
		bool mem_scheduled;

		skb->truesize = SKB_TRUESIZE(skb_end_offset(skb));
		if (force_schedule) {
			mem_scheduled = true;
			sk_forced_mem_schedule(sk, skb->truesize);
		} else {
			mem_scheduled = sk_wmem_schedule(sk, skb->truesize);
		}
		if (likely(mem_scheduled)) {
			skb_reserve(skb, MAX_TCP_HEADER);
			skb->ip_summed = CHECKSUM_PARTIAL;
			INIT_LIST_HEAD(&skb->tcp_tsorted_anchor);
			return skb;
		}
		__kfree_skb(skb);
	} else {
		if (!sk->sk_bypass_prot_mem)
			tcp_enter_memory_pressure(sk);
		sk_stream_moderate_sndbuf(sk);
	}
	return NULL;
}

/*
 * tcp_xmit_size_goal - 计算一个 write-queue GSO skb 希望聚合的 payload 上限。
 *
 * 不允许大包（如 OOB）时退化为一个 MSS。普通路径受发送窗口一半、设备最大 GSO
 * size/segs 限制，并缓存 gso_segs 以避开每次 send 的除法；边界跨越一个 MSS 时
 * 才重算。返回至少 mss_now，聚合只改变软件对象数量，不改变线上 TCP segment。
 */
static unsigned int tcp_xmit_size_goal(struct sock *sk, u32 mss_now,
				       int large_allowed)
{
	struct tcp_sock *tp = tcp_sk(sk);
	u32 new_size_goal, size_goal;

	if (!large_allowed)
		return mss_now;

	/* Note : tcp_tso_autosize() will eventually split this later */
	/* 当前只定聚合目标，输出阶段仍会按 pacing/cwnd/设备限制重新切分。 */
	new_size_goal = tcp_bound_to_half_wnd(tp, sk->sk_gso_max_size);

	/* We try hard to avoid divides here */
	/* 缓存 gso_segs，仅跨 MSS 档位时做除法，减少每次 send 的整数除法成本。 */
	size_goal = tp->gso_segs * mss_now;
	if (unlikely(new_size_goal < size_goal ||
		     new_size_goal >= size_goal + mss_now)) {
		tp->gso_segs = min_t(u16, new_size_goal / mss_now,
				     sk->sk_gso_max_segs);
		size_goal = tp->gso_segs * mss_now;
	}

	return max(size_goal, mss_now);
}

/* 返回当前路径 MSS，并通过 @size_goal 输出本次 send 可聚合的 GSO payload 目标。 */
int tcp_send_mss(struct sock *sk, int *size_goal, int flags)
{
	int mss_now;

	mss_now = tcp_current_mss(sk);
	*size_goal = tcp_xmit_size_goal(sk, mss_now, !(flags & MSG_OOB));

	return mss_now;
}

/* In some cases, sendmsg() could have added an skb to the write queue,
 * but failed adding payload on it. We need to remove it to consume less
 * memory, but more importantly be able to generate EPOLLOUT for Edge Trigger
 * epoll() users. Another reason is that tcp_write_xmit() does not like
 * finding an empty skb in the write queue.
 */
/*
 * sendmsg 可能先 entail 空 skb，随后 copy/内存准入失败。此函数只在尾节点区间
 * [seq,end_seq) 为空时摘队并撤销 wmem；若队列因此为空还停止 busy chrono。
 * 这既维护 tcp_write_xmit“不遇空 skb”的不变量，也让 EPOLLOUT 边沿能够重新产生。
 */
void tcp_remove_empty_skb(struct sock *sk)
{
	struct sk_buff *skb = tcp_write_queue_tail(sk);

	if (skb && TCP_SKB_CB(skb)->seq == TCP_SKB_CB(skb)->end_seq) {
		tcp_unlink_write_queue(skb, sk);
		if (tcp_write_queue_empty(sk))
			tcp_chrono_stop(sk, TCP_CHRONO_BUSY);
		tcp_wmem_free_skb(sk, skb);
	}
}

/* skb changing from pure zc to mixed, must charge zc */
/*
 * pure zerocopy skb 的 truesize 曾按较低元数据成本记账；一旦追加复制数据变为 mixed，
 * 必须先为差额取得 wmem 再清 PURE 标志。失败返回 -ENOMEM，skb 保持 pure 且调用者
 * 仍拥有它，不能先改标志后补记账。
 */
static int tcp_downgrade_zcopy_pure(struct sock *sk, struct sk_buff *skb)
{
	if (unlikely(skb_zcopy_pure(skb))) {
		u32 extra = skb->truesize -
			    SKB_TRUESIZE(skb_end_offset(skb));

		if (!sk_wmem_schedule(sk, extra))
			return -ENOMEM;

		sk_mem_charge(sk, extra);
		skb_shinfo(skb)->flags &= ~SKBFL_PURE_ZEROCOPY;
	}
	return 0;
}


/*
 * tcp_wmem_schedule - 为本轮 payload 取得最多 @copy 字节的发送预算。
 *
 * 普通 sk_wmem_schedule 成功返回完整 copy；失败且队列仍低于 tcp_wmem[0] 时使用
 * forced budget 保证小流至少前进，最终返回 min(copy, forward_alloc)，可能是部分
 * 成功或 0。调用者必须只复制返回字节数，不能把部分预算当布尔值。
 */
int tcp_wmem_schedule(struct sock *sk, int copy)
{
	int left;

	if (likely(sk_wmem_schedule(sk, copy)))
		return copy;

	/* We could be in trouble if we have nothing queued.
	 * Use whatever is left in sk->sk_forward_alloc and tcp_wmem[0]
	 * to guarantee some progress.
	 */
	/* 空队列若完全拒绝首包将无法靠 ACK 释放空间，借最低 sndbuf 预算保证启动进展。 */
	left = READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_wmem[0]) - sk->sk_wmem_queued;
	if (left > 0)
		sk_forced_mem_schedule(sk, min(left, copy));
	return min(copy, sk->sk_forward_alloc);
}

/* 释放尚未完成的客户端 Fast Open 请求容器；其中 data/uarg 由各自协议另行管理。 */
void tcp_free_fastopen_req(struct tcp_sock *tp)
{
	if (tp->fastopen_req) {
		kfree(tp->fastopen_req);
		tp->fastopen_req = NULL;
	}
}

/*
 * tcp_sendmsg_fastopen - 让 sendmsg 在握手阶段携带数据并返回部分进度。
 *
 * @sk 已持 lock_sock；@msg/@size 描述用户数据；@copied 输出 SYN 中已接受字节；
 * @uarg 是可空 zerocopy completion 借用引用。功能未启用返回 EOPNOTSUPP，已有请求
 * 返回 EALREADY，分配失败返回 ENOBUFS。函数暂存 msg 指针仅覆盖同步 connect 调用；
 * connect/超时/RST 可能提前释放 fastopen_req，所以返回后必须重新检查指针。
 * DEFER_CONNECT 路径失败要恢复 CLOSE、dport 和 route caps，避免半初始化连接。
 */
int tcp_sendmsg_fastopen(struct sock *sk, struct msghdr *msg, int *copied,
			 size_t size, struct ubuf_info *uarg)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct inet_sock *inet = inet_sk(sk);
	struct sockaddr *uaddr = msg->msg_name;
	int err, flags;

	if (!(READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_fastopen) &
	      TFO_CLIENT_ENABLE) ||
	    (uaddr && msg->msg_namelen >= sizeof(uaddr->sa_family) &&
	     uaddr->sa_family == AF_UNSPEC))
		return -EOPNOTSUPP;
	if (tp->fastopen_req)
		return -EALREADY; /* Another Fast Open is in progress */
	/* 同一 socket 只能有一个持有 msg 生命周期的 TFO 请求，避免覆盖悬挂指针。 */

	tp->fastopen_req = kzalloc_obj(struct tcp_fastopen_request,
				       sk->sk_allocation);
	if (unlikely(!tp->fastopen_req))
		return -ENOBUFS;
	tp->fastopen_req->data = msg;
	tp->fastopen_req->size = size;
	tp->fastopen_req->uarg = uarg;

	if (inet_test_bit(DEFER_CONNECT, sk)) {
		err = tcp_connect(sk);
		/* Same failure procedure as in tcp_v4/6_connect */
		/* connect 已发布过部分状态，失败必须恢复为可再次连接的 CLOSED 身份。 */
		if (err) {
			tcp_set_state(sk, TCP_CLOSE);
			inet->inet_dport = 0;
			sk->sk_route_caps = 0;
		}
	}
	flags = (msg->msg_flags & MSG_DONTWAIT) ? O_NONBLOCK : 0;
	err = __inet_stream_connect(sk->sk_socket, (struct sockaddr_unsized *)uaddr,
				    msg->msg_namelen, flags, 1);
	/* fastopen_req could already be freed in __inet_stream_connect
	 * if the connection times out or gets rst
	 */
	/* connect 等待期间 RST/timeout 可异步结束请求，故只能在非 NULL 时读取 copied。 */
	if (tp->fastopen_req) {
		*copied = tp->fastopen_req->copied;
		tcp_free_fastopen_req(tp);
		inet_clear_bit(DEFER_CONNECT, sk);
	}
	return err;
}

/* If a gap is detected between sends, mark the socket application-limited. */
/*
 * 当 write queue 不足一 MSS、主机 qdisc/NIC 无本连接数据、cwnd 仍有空间且丢失段
 * 已重传时，低速来自应用没有供数而非网络瓶颈。记录 app_limited 交付边界，让
 * BBR 等速率采样算法不要用这段低速错误降低带宽估计；GNU `?: 1` 避免零作为
 * “未标记”的哨兵冲突。
 */
void tcp_rate_check_app_limited(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (/* We have less than one packet to send. */
	    /* 不足一 MSS 表示发送端当前没有持续供数。 */
	    tp->write_seq - tp->snd_nxt < tp->mss_cache &&
	    /* Nothing in sending host's qdisc queues or NIC tx queue. */
	    /* wmem 近空排除“数据只是滞留本机下层”的情况。 */
	    sk_wmem_alloc_get(sk) < SKB_TRUESIZE(1) &&
	    /* We are not limited by CWND. */
	    /* cwnd 尚有余额，低发送量不能归因于拥塞窗口。 */
	    tcp_packets_in_flight(tp) < tcp_snd_cwnd(tp) &&
	    /* All lost packets have been retransmitted. */
	    /* 若仍有待重传 loss，发送空档属于恢复受限而非应用受限。 */
	    tp->lost_out <= tp->retrans_out)
		tp->app_limited =
			(tp->delivered + tcp_packets_in_flight(tp)) ? : 1;
}
EXPORT_SYMBOL_GPL(tcp_rate_check_app_limited);

/*
 * tcp_sendmsg_locked - 把消息字节加入一个 TCP 连接的发送序列空间。
 *
 * 调用关系：tcp_sendmsg() 持有 socket 用户锁后调用；成功时 tcp_push() 可能
 * 继续进入 tcp_write_xmit()，但也可能因窗口/Nagle/pacing 只留下排队数据。
 * 入参：@sk 必须是 TCP full socket；@msg->msg_iter 是输入且会按实际复制量推进；
 * @size 是入口请求量，普通路径的真实剩余量以 msg_data_left() 为准。
 * 前置条件：调用者持 lock_sock(sk)，进程上下文，可睡眠；函数可在等待连接或
 * send-buffer 空间时释放 CPU，但 socket ownership 协议保证状态串行。
 * 返回：非负值为已被 TCP 接受并分配序列号的字节数，允许部分成功；负 errno
 * 表示未完成部分的原因。返回成功不表示数据已离开主机或得到 ACK。
 * ownership：普通 copy 模式把 payload 放入内核 page frag；MSG_ZEROCOPY 会保持
 * 用户页/ubuf 引用直到异步 completion。write queue skb 由 socket/TCP 管理。
 *
 * 局部调用地图（普通 copy 主线）：
 *
 *   tcp_sendmsg
 *     -> tcp_sendmsg_locked
 *        -> tcp_send_mss                 计算 MSS/GSO 聚合目标
 *        -> tcp_stream_alloc_skb         只分配 skb 元数据和线性头空间
 *        -> tcp_skb_entail               挂入 write queue，初始化 seq/end_seq
 *        -> sk_page_frag_refill          取得 socket 可复用的 backing page
 *        -> skb_copy_to_page_nocache     用户字节复制到 page fragment
 *        -> tcp_wmem_schedule            发送内存准入和记账
 *        -> tcp_push
 *           -> tcp_write_xmit            检查 rwnd/cwnd/Nagle/pacing/TSQ
 *              -> __tcp_transmit_skb     clone 并构造 TCP header
 *
 * 进入/离开状态（普通成功）：
 *
 *   入口：iterator 剩余 N；write_seq=W；用户仍拥有 buffer；可能没有尾 skb。
 *   循环：payload 进入 skb frag；skb 覆盖 [seq,end_seq)；socket 负担 wmem。
 *   提交：write_seq=W+n；iterator 减少 n；copied 增加 n。
 *   返回：TCP 拥有前 n 字节；应用可复用普通 buffer；字节可能仍未发出。
 *
 * 失败标签拥有的资源：
 *
 *   out：       可能已复制字节，执行最终 push 和正常引用释放；
 *   out_nopush：Fast Open/repair 已自行处理发送，不再 push 普通 write queue；
 *   do_error：  可能有空尾 skb和部分成功，先移除空 skb，再优先返回短写；
 *   out_err：   无可返回进度，abort 本函数创建的 zero-copy completion 引用。
 */
int tcp_sendmsg_locked(struct sock *sk, struct msghdr *msg, size_t size)
{
	/*
	 * 变量地图：
	 *   tp             tcp_sk() 将通用 struct sock 恢复为外层 struct tcp_sock；
	 *                  这是基于结构体嵌入布局的内核式“向下转型”，不是新分配。
	 *   skb            当前正在追加的 write-queue 元素；payload 可为非线性 frags。
	 *   copied         普通循环已经提交的字节；copied_syn 是 Fast Open SYN 内数据。
	 *   mss_now        一个线上 TCP segment 的当前 payload 上限。
	 *   size_goal      一个 GSO skb 希望聚合的 payload，可能是多个 mss_now。
	 *   uarg/binding   zero-copy completion 与 device-memory binding 的临时引用。
	 *   process_backlog连续分配计数；定期处理 backlog，避免长 send 饿死收包。
	 */
	struct net_devmem_dmabuf_binding *binding = NULL;
	struct tcp_sock *tp = tcp_sk(sk);
	struct ubuf_info *uarg = NULL;
	struct sk_buff *skb;
	struct sockcm_cookie sockc;
	int flags, err, copied = 0;
	int mss_now = 0, size_goal, copied_syn = 0;
	int process_backlog = 0;
	int sockc_err = 0;
	int zc = 0;
	long timeo;

	/* 阶段 1：解析控制信息和 zero-copy 策略；此时尚未消费 payload。 */
	flags = msg->msg_flags;

	/*
	 * 复合字面量 `(struct sockcm_cookie){...}` 创建一个已清零的结构体值，只
	 * 显式设置 tsflags。READ_ONCE 是并发读取约束，不意味着取得了配置锁。
	 */
	sockc = (struct sockcm_cookie){ .tsflags = READ_ONCE(sk->sk_tsflags) };
	if (msg->msg_controllen) {
		/*
		 * cmsg 可要求 timestamp、device-memory 等每消息属性。helper 只解析到
		 * sockc；错误暂存而不立即退出，是因为 Fast Open 可能已经把 SYN data
		 * 提交，必须按部分成功规则优先报告进度。
		 */
		sockc_err = sock_cmsg_send(sk, msg, &sockc);
		/* Don't return error until MSG_FASTOPEN has been processed;
		 * that may succeed even if the cmsg is invalid.
		 */
	}

	if ((flags & MSG_ZEROCOPY) && size) {
		/*
		 * zero-copy 不是无生命周期成本：ubuf 记录用户页，socket/完成队列要在
		 * DMA/发送引用结束后通知应用。设备不支持 scatter-gather 时保留完成
		 * 协议但回退复制，保证 API 正确而非强行失败。
		 */
		if (msg->msg_ubuf) {
			/* 内核调用者已固定 ubuf，当前函数只借用，不增加/减少该外部引用。 */
			uarg = msg->msg_ubuf;
			if (sk->sk_route_caps & NETIF_F_SG)
				zc = MSG_ZEROCOPY;
		} else if (sock_flag(sk, SOCK_ZEROCOPY)) {
			/*
			 * `sock_flag` 测 socket 位标志；只有 setsockopt 开启 SO_ZEROCOPY
			 * 才允许用户 MSG_ZEROCOPY。realloc 可把相邻 send 合并到同一完成
			 * 区间，减少 error-queue completion 数量。
			 */
			skb = tcp_write_queue_tail(sk);
			uarg = msg_zerocopy_realloc(sk, size, skb_zcopy(skb),
						    !sockc_err && sockc.dmabuf_id);
			if (!uarg) {
				/* 尚未消费 payload；直接进入无进度错误清理。 */
				err = -ENOBUFS;
				goto out_err;
			}
			if (sk->sk_route_caps & NETIF_F_SG)
				zc = MSG_ZEROCOPY;
			else
				uarg_to_msgzc(uarg)->zerocopy = 0;

			if (!sockc_err && sockc.dmabuf_id) {
				/* get_binding 取得引用；IS_ERR/PTR_ERR 是“指针编码 errno”惯用法。 */
				binding = net_devmem_get_binding(sk, sockc.dmabuf_id);
				if (IS_ERR(binding)) {
					err = PTR_ERR(binding);
					binding = NULL;
					goto out_err;
				}
			}
		}
	} else if (unlikely(msg->msg_flags & MSG_SPLICE_PAGES) && size) {
		/* splice 模式引用 iterator 提供的页，不要求用户 zero-copy completion。 */
		if (sk->sk_route_caps & NETIF_F_SG)
			zc = MSG_SPLICE_PAGES;
	}

	if (!sockc_err && sockc.dmabuf_id &&
	    (!(flags & MSG_ZEROCOPY) || !sock_flag(sk, SOCK_ZEROCOPY))) {
		err = -EINVAL;
		/* dmabuf id 只有在显式 zero-copy 协议下才有生命周期接收者。 */
		goto out_err;
	}

	if (unlikely(flags & MSG_FASTOPEN ||
		     inet_test_bit(DEFER_CONNECT, sk)) &&
	    !tp->repair) {
		/*
		 * Fast Open 可把 payload 放进 SYN，因此“连接尚未建立”不一定意味着零
		 * 进度。copied_syn 是输出参数；-EINPROGRESS 且 copied_syn>0 时必须
		 * 返回该进度，而不是用错误覆盖已经取得的用户可见效果。
		 */
		err = tcp_sendmsg_fastopen(sk, msg, &copied_syn, size, uarg);
		if (err == -EINPROGRESS && copied_syn > 0)
			goto out;
		else if (err)
			goto out_err;
	}

	/* 阶段 2：把 blocking/O_NONBLOCK 语义转换为剩余等待时间。 */
	timeo = sock_sndtimeo(sk, flags & MSG_DONTWAIT);

	/* 给带宽采样标记“应用供数不足”，避免拥塞控制把低速误判为网络容量。 */
	tcp_rate_check_app_limited(sk);  /* is sending application-limited? */
	/* 在本轮加入新数据前记录空档边界，避免新 write_seq 掩盖应用供数不足。 */

	/* Wait for a connection to finish. One exception is TCP Fast Open
	 * (passive side) where data is allowed to be sent before a connection
	 * is fully established.
	 */
	/*
	 * `1 << state` 把枚举状态转换为集合中的一个 bit；TCPF_* 是状态 bitmask。
	 * 取反后相与表示“当前状态不属于 ESTABLISHED/CLOSE_WAIT 集合”。
	 * CLOSE_WAIT 仍允许发送，因为它只表示对端关闭了发送方向。
	 */
	if (((1 << sk->sk_state) & ~(TCPF_ESTABLISHED | TCPF_CLOSE_WAIT)) &&
	    !tcp_passive_fastopen(sk)) {
		/* 睡眠等待握手状态变化；不能持 CPU 自旋等待一个 RTT。 */
		err = sk_stream_wait_connect(sk, &timeo);
		if (err != 0)
			goto do_error;
	}

	if (unlikely(tp->repair)) {
		/* TCP_REPAIR 是 checkpoint/restore 特殊接口，可直接编辑逻辑队列，不发包。 */
		if (tp->repair_queue == TCP_RECV_QUEUE) {
			copied = tcp_send_rcvq(sk, msg, size);
			goto out_nopush;
		}

		err = -EINVAL;
		if (tp->repair_queue == TCP_NO_QUEUE)
			goto out_err;

		/* 'common' sending to sendq */
		/* repair 模式指定的队列合法；未选特殊队列时继续走普通发送队列。 */
	}

	if (sockc_err) {
		err = sockc_err;
		goto out_err;
	}

	/* This should be in poll */
	/* 新一次发送先清旧的异步“无空间”提示；真正阻塞时会重新发布 NOSPACE。 */
	sk_clear_bit(SOCKWQ_ASYNC_NOSPACE, sk);

	/* Ok commence sending. */
	copied = 0;

restart:
	/*
	 * 阶段 3：MSS 是线速 TCP payload 上限；size_goal 可因 GSO 大于 MSS，允许
	 * 一个 skb 表示多个 segment。route/PMTU 改变或睡眠返回后必须重新计算。
	 */
	mss_now = tcp_send_mss(sk, &size_goal, flags);

	err = -EPIPE;
	if (sk->sk_err || (sk->sk_shutdown & SEND_SHUTDOWN))
		/* socket 已有异步错误或本地 shutdown(SHUT_WR)，不能分配新的序列空间。 */
		goto do_error;

	while (msg_data_left(msg)) {
		int copy = 0;

		/* 优先追加尾 skb，减少元数据分配；只有可折叠且仍低于目标大小才复用。 */
		skb = tcp_write_queue_tail(sk);
		if (skb)
			copy = size_goal - skb->len;

		trace_tcp_sendmsg_locked(sk, msg, skb, size_goal);

		if (copy <= 0 || !tcp_skb_can_collapse_to(skb)) {
			/*
			 * `skb` 可为 NULL；helper 按约定可处理 NULL 并返回 false。不能只看
			 * 剩余长度：克隆/zero-copy/已发送等 skb 即使有空间也不可原地扩展。
			 */
			bool first_skb;

new_segment:
			/*
			 * 阶段 4：创建新的 headerless TCP skb。内存配额检查先于分配，
			 * tcp_skb_entail() 会把 skb 挂入 write queue 并建立 socket 记账。
			 */
			if (!sk_stream_memory_free(sk))
				goto wait_for_space;

			if (unlikely(process_backlog >= 16)) {
				/*
				 * send 持有用户 owner 时，RX softirq 只能向 backlog 追加。每 16
				 * 个新 skb 主动 flush，既回收 ACK 带来的发送空间，也限制收包延迟。
				 * flush 可能改变 MSS/route/状态，所以处理后从 restart 重建决策。
				 */
				process_backlog = 0;
				if (sk_flush_backlog(sk))
					goto restart;
			}
			/* 首个 skb 的 headroom/记账策略可能与已有发送链不同。 */
			first_skb = tcp_rtx_and_write_queues_empty(sk);
			skb = tcp_stream_alloc_skb(sk, sk->sk_allocation,
						   first_skb);
			if (!skb)
				goto wait_for_space;

			process_backlog++;

#ifdef CONFIG_SKB_DECRYPTED
			skb->decrypted = !!(flags & MSG_SENDPAGE_DECRYPTED);
#endif
			/*
			 * entail 是第一次 ownership 发布：把新 skb 接到 write queue 尾，
			 * 以当前 write_seq 初始化 TCP_SKB_CB.seq/end_seq，并增加 wmem。
			 * 此后错误路径不能直接 kfree，必须用 TCP queue helper 撤销。
			 */
			tcp_skb_entail(sk, skb);
			copy = size_goal;

			/* All packets are restored as if they have
			 * already been sent. skb_mstamp_ns isn't set to
			 * avoid wrong rtt estimation.
			 */
			/* repair 注入的是历史队列状态，不生成新发送时间样本，否则 RTT/RACK 会误判。 */
			if (tp->repair)
				TCP_SKB_CB(skb)->sacked |= TCPCB_REPAIRED;
		}

		/* Try to append data to the end of skb. */
		/* `copy` 是容量，不能超过 iterator 真正剩余字节。 */
		if (copy > msg_data_left(msg))
			copy = msg_data_left(msg);

		if (zc == 0) {
			/*
			 * 普通模式从用户 iterator 复制到 socket 的 page_frag。skb frags 只
			 * 保存 page/offset/len，因此新增 frag 必须增加 page 引用；能与尾
			 * frag 连续时只扩大长度，避免额外描述符。
			 */
			bool merge = true;
			int i = skb_shinfo(skb)->nr_frags;
			struct page_frag *pfrag = sk_page_frag(sk);

			/* refill 可能分配 page；失败不丢已有进度，而转 send-memory 等待。 */
			if (!sk_page_frag_refill(sk, pfrag))
				goto wait_for_space;

			if (!skb_can_coalesce(skb, i, pfrag->page,
					      pfrag->offset)) {
				if (i >= READ_ONCE(net_hotdata.sysctl_max_skb_frags)) {
					/* frag 描述符达到上限：结束当前 skb，另建 segment，而非越界。 */
					tcp_mark_push(tp, skb);
					goto new_segment;
				}
				merge = false;
			}

			/* min_t 显式以 int 比较，避免 size_t/有符号隐式转换改变结果。 */
			copy = min_t(int, copy, pfrag->size - pfrag->offset);

			if (unlikely(skb_zcopy_pure(skb) || skb_zcopy_managed(skb))) {
				if (tcp_downgrade_zcopy_pure(sk, skb))
					goto wait_for_space;
				skb_zcopy_downgrade_managed(skb);
			}

			/* 返回本次获准记账的字节，可能小于请求；0 表示需要等待空间。 */
			copy = tcp_wmem_schedule(sk, copy);
			if (!copy)
				goto wait_for_space;

			/* 这里才真正触碰用户页；fault/短复制错误仍可走部分成功语义。 */
			err = skb_copy_to_page_nocache(sk, &msg->msg_iter, skb,
						       pfrag->page,
						       pfrag->offset,
						       copy);
			if (err)
				goto do_error;

			/* Update the skb. */
			if (merge) {
				/* 已有最后 frag 与 page+offset 物理连续，只扩大长度，不新增引用。 */
				skb_frag_size_add(&skb_shinfo(skb)->frags[i - 1], copy);
			} else {
				/* 新 descriptor 借用 pfrag->page，page_ref_inc 把存活期交给 skb。 */
				skb_fill_page_desc(skb, i, pfrag->page,
						   pfrag->offset, copy);
				page_ref_inc(pfrag->page);
			}
			pfrag->offset += copy;
		} else if (zc == MSG_ZEROCOPY)  {
			/*
			 * zero-copy helper 从 iterator pin/附加页并返回实际附加字节。
			 * -EMSGSIZE/-EEXIST 表示当前 skb 形态不能继续追加，不是连接错误；
			 * push 当前 skb 并从 new_segment 重试剩余 iterator。
			 */
			/* First append to a fragless skb builds initial
			 * pure zerocopy skb
			 */
			/* 首次直接附加用户页时先标 pure，后续混入复制数据才补收较高 truesize。 */
			if (!skb->len)
				skb_shinfo(skb)->flags |= SKBFL_PURE_ZEROCOPY;

			if (!skb_zcopy_pure(skb)) {
				copy = tcp_wmem_schedule(sk, copy);
				if (!copy)
					goto wait_for_space;
			}

			err = skb_zerocopy_iter_stream(sk, skb, msg, copy, uarg,
						       binding);
			if (err == -EMSGSIZE || err == -EEXIST) {
				tcp_mark_push(tp, skb);
				goto new_segment;
			}
			if (err < 0)
				goto do_error;
			copy = err;
		} else if (zc == MSG_SPLICE_PAGES) {
			/* Splice in data if we can; copy if we can't. */
			/* helper 优先转移页引用；布局不支持时内部复制，但 TCP 序列语义相同。 */
			if (tcp_downgrade_zcopy_pure(sk, skb))
				goto wait_for_space;
			copy = tcp_wmem_schedule(sk, copy);
			if (!copy)
				goto wait_for_space;

			err = skb_splice_from_iter(skb, &msg->msg_iter, copy);
			if (err < 0) {
				if (err == -EMSGSIZE) {
					tcp_mark_push(tp, skb);
					goto new_segment;
				}
				goto do_error;
			}
			copy = err;

			if (!(flags & MSG_NO_SHARED_FRAGS))
				skb_shinfo(skb)->flags |= SKBFL_SHARED_FRAG;

			/* splice helper 不走普通 copy 记账，成功后在此显式增加 queued/proto memory。 */
			sk_wmem_queued_add(sk, copy);
			sk_mem_charge(sk, copy);
		}

		/* 本次 send 的首批数据清旧 PSH，最终是否 push 由 flags/策略统一决定。 */
		if (!copied)
			TCP_SKB_CB(skb)->tcp_flags &= ~TCPHDR_PSH;

		/*
		 * 阶段 5：提交本轮字节的序列空间。write_seq 是连接全局的下一个
		 * 待分配序号，end_seq 是当前 skb 的右开边界；两者必须同步推进，
		 * 否则 ACK/重传会把字节归入错误区间。WRITE_ONCE 还避免并发诊断
		 * 读到编译器制造的撕裂/重复访问，但不代替 socket lock。
		 */
		WRITE_ONCE(tp->write_seq, tp->write_seq + copy);
		TCP_SKB_CB(skb)->end_seq += copy;
		/* payload 改变使旧 GSO segment count 失效，置 0 让输出阶段按 MSS 重算。 */
		tcp_skb_pcount_set(skb, 0);

		copied += copy;
		if (!msg_data_left(msg)) {
			/* iterator 而非入口 size 是完成依据，因为 Fast Open/短附加会推进它。 */
			if (unlikely(flags & MSG_EOR))
				TCP_SKB_CB(skb)->eor = 1;
			goto out;
		}

		/* 未到聚合目标就继续填同一 skb；OOB/repair 有各自发送/队列规则。 */
		if (skb->len < size_goal || (flags & MSG_OOB) || unlikely(tp->repair))
			continue;

		if (forced_push(tp)) {
			/* 到达强制 push 边界，立即让输出引擎评估窗口；不保证一定发出。 */
			tcp_mark_push(tp, skb);
			__tcp_push_pending_frames(sk, mss_now, TCP_NAGLE_PUSH);
		} else if (skb == tcp_send_head(sk))
			tcp_push_one(sk, mss_now);
		continue;

wait_for_space:
		/*
		 * 阶段 6：send buffer/协议内存不足。先发布 SOCK_NOSPACE，让 ACK 或
		 * completion 释放显著空间时唤醒 poll/epoll writer；已有 copied 数据
		 * 先尝试 push，再睡眠或按 MSG_DONTWAIT 返回。
		 */
		set_bit(SOCK_NOSPACE, &sk->sk_socket->flags);
		tcp_remove_empty_skb(sk);
		if (copied)
			tcp_push(sk, flags & ~MSG_MORE, mss_now,
				 TCP_NAGLE_PUSH, size_goal);

		/*
		 * helper 按 socket waitqueue 睡眠并在 ACK、内存释放、错误、signal 或
		 * timeout 时返回。它会维护锁/等待状态，调用者不能自行 schedule 代替。
		 */
		err = sk_stream_wait_memory(sk, &timeo);
		if (err != 0)
			goto do_error;

		mss_now = tcp_send_mss(sk, &size_goal, flags);
	}

out:
	if (copied) {
		/*
		 * 最终 push 决定 MSG_MORE/CORK/Nagle 语义。即便输出受阻，copied 字节
		 * 已属于 TCP，故仍可成功返回；可靠交付由后续 ACK/重传完成。
		 */
		tcp_tx_timestamp(sk, &sockc);
		tcp_push(sk, flags, mss_now, tp->nonagle, size_goal);
	}
out_nopush:
	/* msg->msg_ubuf is pinned by the caller so we don't take extra refs */
	/*
	 * 若 ubuf 来自 msg，引用由外层 caller 保持，本函数不能 put；
	 * 若本函数通过 msg_zerocopy_realloc 创建/取得，则在这里交还本地引用。
	 */
	if (uarg && !msg->msg_ubuf)
		net_zcopy_put(uarg);
	if (binding)
		net_devmem_dmabuf_binding_put(binding);
	return copied + copied_syn;

do_error:
	/* 空 skb 尚未承载序列空间，错误返回前把它从 write queue/记账中撤销。 */
	tcp_remove_empty_skb(sk);

	/* 已取得进度时返回短写；错误留给未完成部分，应用应循环发送。 */
	if (copied + copied_syn)
		goto out;
out_err:
	/* msg->msg_ubuf is pinned by the caller so we don't take extra refs */
	/* 无可报告进度时，abort 尚未正常发布的 zero-copy completion。 */
	if (uarg && !msg->msg_ubuf)
		net_zcopy_put_abort(uarg, true);
	/* 将内部原因结合 SO_ERROR、SIGPIPE/MSG_NOSIGNAL 和 socket 状态映射为用户 errno。 */
	err = sk_stream_error(sk, flags, err);
	/* make sure we wake any epoll edge trigger waiter */
	if (unlikely(tcp_rtx_and_write_queues_empty(sk) && err == -EAGAIN)) {
		/* edge-trigger epoll 可能错过“再次可写”边沿，主动调用 write_space 修复通知。 */
		READ_ONCE(sk->sk_write_space)(sk);
		tcp_chrono_stop(sk, TCP_CHRONO_SNDBUF_LIMITED);
	}
	if (binding)
		net_devmem_dmabuf_binding_put(binding);

	return err;
}
EXPORT_SYMBOL_GPL(tcp_sendmsg_locked);

/*
 * tcp_sendmsg - 为用户发送取得可睡眠 socket 锁的薄包装。
 * @sk/@msg/@size 语义与 tcp_sendmsg_locked 相同；返回字节数或 errno。
 * lock_sock 与 softirq 的 bh_lock_sock/backlog 协议配对：用户持锁期间到达的包
 * 不会并发修改 TCP 状态，而在 release_sock 中得到延后处理。
 */
int tcp_sendmsg(struct sock *sk, struct msghdr *msg, size_t size)
{
	int ret;

	lock_sock(sk);
	ret = tcp_sendmsg_locked(sk, msg, size);
	release_sock(sk);

	return ret;
}
EXPORT_SYMBOL(tcp_sendmsg);

/*
 * tcp_splice_eof - splice 写入端报告 EOF 时强制推动尚在 TCP write queue 的尾数据。
 *
 * @sock 在调用期间有效。没有尾 skb 可直接返回；否则取得 socket lock，重新计算
 * MSS/GSO 目标并按当前 Nagle 策略 tcp_push。它不发送 FIN，只结束 splice producer
 * 隐含的“后面可能还有数据”聚合机会，避免最后一个未满 skb 永久留在队列。
 */
void tcp_splice_eof(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct tcp_sock *tp = tcp_sk(sk);
	int mss_now, size_goal;

	if (!tcp_write_queue_tail(sk))
		return;

	lock_sock(sk);
	mss_now = tcp_send_mss(sk, &size_goal, 0);
	tcp_push(sk, 0, mss_now, tp->nonagle, size_goal);
	release_sock(sk);
}

/*
 *	Handle reading urgent data. BSD has very simple semantics for
 *	this, no blocking and very strange errors 8)
 */

/*
 * tcp_recv_urg - 实现 recv(MSG_OOB) 的单字节紧急数据接口。
 *
 * @sk 已由 tcp_recvmsg 持锁；@msg 是输出 iterator；@len 是容量；@flags 支持 PEEK/
 * TRUNC。SO_OOBINLINE、无 urgent 或已读返回 EINVAL；有效字节最多交付 1，并在非
 * PEEK 时把 urg_data 标成 READ。紧急指针已知但字节尚未到达时始终 EAGAIN，即使
 * fd 是阻塞模式，这是历史 BSD 语义。返回 0 只用于连接关闭/接收 shutdown。
 */
static int tcp_recv_urg(struct sock *sk, struct msghdr *msg, int len, int flags)
{
	struct tcp_sock *tp = tcp_sk(sk);

	/* No URG data to read. */
	/* OOBINLINE 由普通 recv 消费；无数据或已经读取时 MSG_OOB 请求本身无效。 */
	if (sock_flag(sk, SOCK_URGINLINE) || !tp->urg_data ||
	    tp->urg_data == TCP_URG_READ)
		return -EINVAL;	/* 没有可供 MSG_OOB 单独读取的字节，EINVAL 正是历史 ABI。 */

	if (sk->sk_state == TCP_CLOSE && !sock_flag(sk, SOCK_DONE))
		return -ENOTCONN;

	if (tp->urg_data & TCP_URG_VALID) {
		int err = 0;
		char c = tp->urg_data;

		if (!(flags & MSG_PEEK))
			WRITE_ONCE(tp->urg_data, TCP_URG_READ);

		/* Read urgent data. */
		/* urgent 缓存只有一个字节；非 PEEK 先标已读，copy fault 也不重复交付。 */
		msg->msg_flags |= MSG_OOB;

		if (len > 0) {
			if (!(flags & MSG_TRUNC))
				err = memcpy_to_msg(msg, &c, 1);
			len = 1;
		} else
			msg->msg_flags |= MSG_TRUNC;

		return err ? -EFAULT : len;
	}

	if (sk->sk_state == TCP_CLOSE || (sk->sk_shutdown & RCV_SHUTDOWN))
		return 0;

	/* Fixed the recv(..., MSG_OOB) behaviour.  BSD docs and
	 * the available implementations agree in this case:
	 * this call should never block, independent of the
	 * blocking state of the socket.
	 * Mike <pall@rz.uni-karlsruhe.de>
	 */
	return -EAGAIN;
}

/*
 * tcp_peek_sndq - TCP_REPAIR 下把未确认树和未发送队列内容复制给管理程序。
 *
 * 按序先遍历 rtx RB-tree，再遍历 write queue；@len 由上层验证，实际 copy 受 msg
 * iterator 限制。成功返回复制字节，copy fault 返回错误（已有复制不按普通 send/
 * recv 短读处理，因为 repair 需要一致快照）。函数只借用 skb，不改变队列游标。
 */
static int tcp_peek_sndq(struct sock *sk, struct msghdr *msg, int len)
{
	struct sk_buff *skb;
	int copied = 0, err = 0;

	skb_rbtree_walk(skb, &sk->tcp_rtx_queue) {
		err = skb_copy_datagram_msg(skb, 0, msg, skb->len);
		if (err)
			return err;
		copied += skb->len;
	}

	skb_queue_walk(&sk->sk_write_queue, skb) {
		err = skb_copy_datagram_msg(skb, 0, msg, skb->len);
		if (err)
			break;

		copied += skb->len;
	}

	return err ?: copied;
}

/* Clean up the receive buffer for full frames taken by the user,
 * then send an ACK if necessary.  COPIED is the number of bytes
 * tcp_recvmsg has given to the user so far, it speeds up the
 * calculation of whether or not we must ACK for the sake of
 * a window update.
 */
/*
 * __tcp_cleanup_rbuf - 应用消费 receive queue 后决定是否立即发送 ACK/window update。
 *
 * @copied 是本轮新交付 payload 字节，可为 0。函数不释放 skb；队列释放已由 reader
 * 完成。它首先兑现 delayed-ACK 条件，再检查应用释放空间是否让公告窗口至少翻倍；
 * 只有仍可接收时才发 window-open ACK。避免每次小 read 都发 ACK 的收益是减少包，
 * 代价是窗口更新略延迟，所以“大幅增加”或 pushed/队列清空触发立即 ACK。
 */
void __tcp_cleanup_rbuf(struct sock *sk, int copied)
{
	struct tcp_sock *tp = tcp_sk(sk);
	bool time_to_ack = false;

	if (inet_csk_ack_scheduled(sk)) {
		const struct inet_connection_sock *icsk = inet_csk(sk);

		if (/* Once-per-two-segments ACK was not sent by tcp_input.c */
		    /* 接收推进超过一个 rcv_mss 时立即补 ACK，避免 delayed ACK 再拖延。 */
		    tp->rcv_nxt - tp->rcv_wup > icsk->icsk_ack.rcv_mss ||
		    /*
		     * If this read emptied read buffer, we send ACK, if
		     * connection is not bidirectional, user drained
		     * receive buffer and there was a small segment
		     * in queue.
		     */
		    /* 队列被读空且曾收到 PUSH/小段时，立即 ACK 可及时反馈窗口与交付。 */
		    (copied > 0 &&
		     ((icsk->icsk_ack.pending & ICSK_ACK_PUSHED2) ||
		      ((icsk->icsk_ack.pending & ICSK_ACK_PUSHED) &&
		       !inet_csk_in_pingpong_mode(sk))) &&
		      !atomic_read(&sk->sk_rmem_alloc)))
			time_to_ack = true;
	}

	/* We send an ACK if we can now advertise a non-zero window
	 * which has been raised "significantly".
	 *
	 * Even if window raised up to infinity, do not send window open ACK
	 * in states, where we will not receive more. It is useless.
	 */
	/* 已 shutdown 接收方向时不再公告新窗口；仍接收时仅对显著扩窗主动发 ACK。 */
	if (copied > 0 && !time_to_ack && !(sk->sk_shutdown & RCV_SHUTDOWN)) {
		__u32 rcv_window_now = tcp_receive_window(tp);

		/* Optimize, __tcp_select_window() is not cheap. */
		/* 当前窗口已超过 clamp 一半时不可能再翻倍，省去昂贵窗口计算。 */
		if (2*rcv_window_now <= tp->window_clamp) {
			__u32 new_window = __tcp_select_window(sk);

			/* Send ACK now, if this read freed lots of space
			 * in our buffer. Certainly, new_window is new window.
			 * We can advertise it now, if it is not less than current one.
			 * "Lots" means "at least twice" here.
			 */
			/* 窗口至少翻倍才值得单独发 ACK，避免小幅 read 造成 ACK 风暴。 */
			if (new_window && new_window >= 2 * rcv_window_now)
				time_to_ack = true;
		}
	}
	if (time_to_ack) {
		tcp_mstamp_refresh(tp);
		tcp_send_ack(sk);
	}
}

/* 校验 copied_seq 仍落在队首范围内，再调用 ACK/window 更新决策；WARN 暴露队列不变量破坏。 */
void tcp_cleanup_rbuf(struct sock *sk, int copied)
{
	struct sk_buff *skb = skb_peek(&sk->sk_receive_queue);
	struct tcp_sock *tp = tcp_sk(sk);

	WARN(skb && !before(tp->copied_seq, TCP_SKB_CB(skb)->end_seq),
	     "cleanup rbuf bug: copied %X seq %X rcvnxt %X\n",
	     tp->copied_seq, TCP_SKB_CB(skb)->end_seq, tp->rcv_nxt);
	__tcp_cleanup_rbuf(sk, copied);
}

/*
 * tcp_eat_recv_skb - 摘除一个已完全消费的 receive skb 并归还接收内存。
 *
 * @skb 必须仍在 sk_receive_queue 且由 @sk 记账。sock_rfree 先撤销 rmem owner，清
 * destructor/sk 防止二次记账，再尝试延迟释放以改善缓存局部性；非标准 destructor
 * 走通用 kfree。返回后 skb 指针无效。
 */
static void tcp_eat_recv_skb(struct sock *sk, struct sk_buff *skb)
{
	__skb_unlink(skb, &sk->sk_receive_queue);
	if (likely(skb->destructor == sock_rfree)) {
		sock_rfree(skb);
		skb->destructor = NULL;
		skb->sk = NULL;
		return skb_attempt_defer_free(skb);
	}
	__kfree_skb(skb);
}

/*
 * tcp_recv_skb - 定位覆盖 TCP 序号 @seq 的 receive-queue skb。
 *
 * @sk 已持锁；@off 输出 seq 相对 skb payload 起点的偏移。返回借用 skb，调用者
 * 不得越过锁保存；NULL 表示当前无覆盖对象。函数可顺便清理由 splice 释放锁期间
 * GRO collapse/split 留下的、已完全落在 seq 之前的空壳，因此不是纯查询。
 * FIN 无 payload 但占序列空间，offset==len 时仍需返回供 reader 消费 EOF 标记。
 */
struct sk_buff *tcp_recv_skb(struct sock *sk, u32 seq, u32 *off)
{
	struct sk_buff *skb;
	u32 offset;

	while ((skb = skb_peek(&sk->sk_receive_queue)) != NULL) {
		offset = seq - TCP_SKB_CB(skb)->seq;
		if (unlikely(TCP_SKB_CB(skb)->tcp_flags & TCPHDR_SYN)) {
			pr_err_once("%s: found a SYN, please report !\n", __func__);
			offset--;
		}
		if (offset < skb->len || (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN)) {
			*off = offset;
			return skb;
		}
		/* This looks weird, but this can happen if TCP collapsing
		 * splitted a fat GRO packet, while we released socket lock
		 * in skb_splice_bits()
		 */
		/* 释放锁期间队列形态可变，但 seq 单调；过期队首可安全消费后继续定位。 */
		tcp_eat_recv_skb(sk, skb);
	}
	return NULL;
}
EXPORT_SYMBOL(tcp_recv_skb);

/*
 * This routine provides an alternative to tcp_recvmsg() for routines
 * that would like to handle copying from skbuffs directly in 'sendfile'
 * fashion.
 * Note:
 *	- It is assumed that the socket was locked by the caller.
 *	- The routine does not block.
 *	- At present, there is no support for reading OOB data
 *	  or for 'peeking' the socket using this routine
 *	  (although both would be easy to implement).
 */
/*
 * __tcp_read_sock - 用回调 actor 非阻塞消费连续 receive skb。
 *
 * @desc 同时携带 actor 私有状态和剩余额度；@recv_actor 可复制、splice 或交付引用；
 * @noack 控制是否跳过窗口/ACK 更新；@copied_seq 是输入输出游标，可为 socket 正式
 * 游标或调用者私有游标。调用者持 socket lock，函数不等待数据。正返回已消费字节，
 * 0 表示当前无进度，首个 actor 错误原样返回；已有进度后错误转短读。
 *
 * actor 可能临时释放锁，期间 skb 会被 collapse 替换，所以回调后绝不能继续信任
 * 原指针，必须按新 seq 重新 tcp_recv_skb。FIN 只推进 seq 1，不计入返回字节。
 */
static int __tcp_read_sock(struct sock *sk, read_descriptor_t *desc,
			   sk_read_actor_t recv_actor, bool noack,
			   u32 *copied_seq)
{
	struct sk_buff *skb;
	struct tcp_sock *tp = tcp_sk(sk);
	u32 seq = *copied_seq;
	u32 offset;
	int copied = 0;

	if (sk->sk_state == TCP_LISTEN)
		return -ENOTCONN;
	while ((skb = tcp_recv_skb(sk, seq, &offset)) != NULL) {
		if (offset < skb->len) {
			int used;
			size_t len;

			len = skb->len - offset;
			/* Stop reading if we hit a patch of urgent data */
			/* actor API 不支持 OOB，必须在 urgent 序号前截断并留给专用接口。 */
			if (unlikely(tp->urg_data)) {
				u32 urg_offset = tp->urg_seq - seq;
				if (urg_offset < len)
					len = urg_offset;
				if (!len)
					break;
			}
			used = recv_actor(desc, skb, offset, len);
			if (used <= 0) {
				if (!copied)
					copied = used;
				break;
			}
			if (WARN_ON_ONCE(used > len))
				used = len;
			seq += used;
			copied += used;
			offset += used;

			/* If recv_actor drops the lock (e.g. TCP splice
			 * receive) the skb pointer might be invalid when
			 * getting here: tcp_collapse might have deleted it
			 * while aggregating skbs from the socket queue.
			 */
			/* 回调后只信任 seq，不信任旧 skb 地址；重新查找防止 collapse 后 UAF。 */
			skb = tcp_recv_skb(sk, seq - 1, &offset);
			if (!skb)
				break;
			/* TCP coalescing might have appended data to the skb.
			 * Try to splice more frags
			 */
			/* 若原尾部被扩展，继续同一逻辑段可减少 actor/pipe 调用。 */
			if (offset + 1 != skb->len)
				continue;
		}
		if (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN) {
			tcp_eat_recv_skb(sk, skb);
			++seq;
			break;
		}
		tcp_eat_recv_skb(sk, skb);
		if (!desc->count)
			break;
		WRITE_ONCE(*copied_seq, seq);
	}
	WRITE_ONCE(*copied_seq, seq);

	if (noack)
		goto out;

	tcp_rcv_space_adjust(sk);

	/* Clean up data we have read: This will do ACK frames. */
	/* 已消费的数据必须在此回收；释放接收窗口还可能触发 ACK/窗口更新。 */
	if (copied > 0) {
		tcp_recv_skb(sk, seq, &offset);
		tcp_cleanup_rbuf(sk, copied);
	}
out:
	return copied;
}

/* 使用真实 copied_seq 且消费后生成 ACK/window update 的标准 actor 读取包装。 */
int tcp_read_sock(struct sock *sk, read_descriptor_t *desc,
		  sk_read_actor_t recv_actor)
{
	return __tcp_read_sock(sk, desc, recv_actor, false,
			       &tcp_sk(sk)->copied_seq);
}
EXPORT_SYMBOL(tcp_read_sock);

/* 允许内核调用者提供私有序号游标，并显式选择是否抑制 ACK 的高级包装。 */
int tcp_read_sock_noack(struct sock *sk, read_descriptor_t *desc,
			sk_read_actor_t recv_actor, bool noack,
			u32 *copied_seq)
{
	return __tcp_read_sock(sk, desc, recv_actor, noack, copied_seq);
}

/*
 * tcp_read_skb - 把完整 receive skb 的 ownership 逐个交给 actor。
 *
 * 与 tcp_read_sock 的“actor 借用 skb 并返回字节数”不同，这里先从队列摘除并重新
 * 确认 socket owner，再调用 @recv_actor；actor 接管 skb 的后续释放。负返回在无
 * 进度时传播，已有进度则短读；遇含 FIN 的 skb 后停止。调用者必须已串行化 socket。
 */
int tcp_read_skb(struct sock *sk, skb_read_actor_t recv_actor)
{
	struct sk_buff *skb;
	int copied = 0;

	if (sk->sk_state == TCP_LISTEN)
		return -ENOTCONN;

	while ((skb = skb_peek(&sk->sk_receive_queue)) != NULL) {
		u8 tcp_flags;
		int used;

		__skb_unlink(skb, &sk->sk_receive_queue);
		WARN_ON_ONCE(!skb_set_owner_sk_safe(skb, sk));
		tcp_flags = TCP_SKB_CB(skb)->tcp_flags;
		used = recv_actor(sk, skb);
		if (used < 0) {
			if (!copied)
				copied = used;
			break;
		}
		copied += used;

		if (tcp_flags & TCPHDR_FIN)
			break;
	}
	return copied;
}

/*
 * tcp_read_done - 为此前外部读取/映射完成的 @len 字节提交 copied_seq 和队列释放。
 *
 * 函数重新按当前队列定位，逐个释放完全覆盖的 skb，部分尾 skb 保留；遇 FIN 推进
 * 一个序列号。请求 len 超过当前可用量时只提交实际覆盖部分。最后调整 autotuning
 * 并按真实消费量触发 ACK/window update。它是“先看/处理，后提交消费”API 的配对点。
 */
void tcp_read_done(struct sock *sk, size_t len)
{
	struct tcp_sock *tp = tcp_sk(sk);
	u32 seq = tp->copied_seq;
	struct sk_buff *skb;
	size_t left;
	u32 offset;

	if (sk->sk_state == TCP_LISTEN)
		return;

	left = len;
	while (left && (skb = tcp_recv_skb(sk, seq, &offset)) != NULL) {
		int used;

		used = min_t(size_t, skb->len - offset, left);
		seq += used;
		left -= used;

		if (skb->len > offset + used)
			break;

		if (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN) {
			tcp_eat_recv_skb(sk, skb);
			++seq;
			break;
		}
		tcp_eat_recv_skb(sk, skb);
	}
	WRITE_ONCE(tp->copied_seq, seq);

	tcp_rcv_space_adjust(sk);

	/* Clean up data we have read: This will do ACK frames. */
	/* splice 成功移走的字节等价于 read 消费，也要回收队列并推进 ACK。 */
	if (left != len)
		tcp_cleanup_rbuf(sk, len - left);
}
EXPORT_SYMBOL(tcp_read_done);

/* 返回当前 TCP 可读 payload 提示，不移动 copied_seq；是 getsockopt/MSG_TRUNC 类查询快照。 */
int tcp_peek_len(struct socket *sock)
{
	return tcp_inq(sock->sk);
}

/* Make sure sk_rcvbuf is big enough to satisfy SO_RCVLOWAT hint */
/*
 * tcp_set_rcvlowat - 设置最小可读阈值，并保证自动接收 buffer 有能力容纳该阈值。
 *
 * @val 被用户锁定 rcvbuf 或 netns tcp_rmem 最大值的一半限制，0 规范化为 1。写入
 * 后立即 tcp_data_ready，避免队列早已满足新较小阈值却没有新包触发唤醒。未锁定
 * SO_RCVBUF 时按窗口到内存的换算扩容 sk_rcvbuf/window_clamp；返回 0，无部分失败。
 */
int tcp_set_rcvlowat(struct sock *sk, int val)
{
	struct tcp_sock *tp = tcp_sk(sk);
	int space, cap;

	if (sk->sk_userlocks & SOCK_RCVBUF_LOCK)
		cap = sk->sk_rcvbuf >> 1;
	else
		cap = READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_rmem[2]) >> 1;
	val = min(val, cap);
	WRITE_ONCE(sk->sk_rcvlowat, val ? : 1);

	/* Check if we need to signal EPOLLIN right now */
	/* 阈值降低后旧数据可能已满足条件，不能等待下一包才产生边沿通知。 */
	tcp_data_ready(sk);

	if (sk->sk_userlocks & SOCK_RCVBUF_LOCK)
		return 0;

	space = tcp_space_from_win(sk, val);
	if (space > sk->sk_rcvbuf) {
		WRITE_ONCE(sk->sk_rcvbuf, space);

		if (tp->window_clamp && tp->window_clamp < val)
			WRITE_ONCE(tp->window_clamp, val);
	}
	return 0;
}

/* 把用户指定的接收内存换算成 TCP 序列窗口，并复用 window-clamp 设置与校验。 */
void tcp_set_rcvbuf(struct sock *sk, int val)
{
	tcp_set_window_clamp(sk, tcp_win_from_space(sk, val));
}

#ifdef CONFIG_MMU
static const struct vm_operations_struct tcp_vm_ops = {
};

/*
 * tcp_mmap - 建立只读、不可执行的空 VMA，供 TCP_ZEROCOPY_RECEIVE 插入接收页。
 *
 * @vma 地址/长度由 mmap 层验证；函数不立即映射任何 skb page。拒绝 WRITE/EXEC
 * 并清 MAY 位，防止以后 mprotect 提权；VM_MIXEDMAP 允许 vm_insert_pages 在已持
 * VMA 锁的路径工作。tcp_vm_ops 作为类型标签，使 zerocopy ioctl 只能向本接口创建
 * 的 VMA 插页。成功返回 0，权限错误返回 EPERM。
 */
int tcp_mmap(struct file *file, struct socket *sock,
	     struct vm_area_struct *vma)
{
	if (vma->vm_flags & (VM_WRITE | VM_EXEC))
		return -EPERM;
	vm_flags_clear(vma, VM_MAYWRITE | VM_MAYEXEC);

	/* Instruct vm_insert_page() to not mmap_read_lock(mm) */
	/* zerocopy 路径自行持 per-VMA 或 mmap read lock，避免 helper 重复获取同一锁。 */
	vm_flags_set(vma, VM_MIXEDMAP);

	vma->vm_ops = &tcp_vm_ops;
	return 0;
}

/*
 * skb_advance_to_frag - 把 skb payload 字节偏移转换为 frags[] 指针和页内 frag 偏移。
 *
 * 只支持非线性 page frags；偏移落在线性 head、越过 skb 或存在 frag_list 时返回
 * NULL。成功返回借用的 frag descriptor，@offset_frag 输出相对该 frag 的字节数。
 * 指针算术每次 `++frag` 按 skb_frag_t 元素推进，不是按字节推进。
 */
static skb_frag_t *skb_advance_to_frag(struct sk_buff *skb, u32 offset_skb,
				       u32 *offset_frag)
{
	skb_frag_t *frag;

	if (unlikely(offset_skb >= skb->len))
		return NULL;

	offset_skb -= skb_headlen(skb);
	if ((int)offset_skb < 0 || skb_has_frag_list(skb))
		return NULL;

	frag = skb_shinfo(skb)->frags;
	while (offset_skb) {
		if (skb_frag_size(frag) > offset_skb) {
			*offset_frag = offset_skb;
			return frag;
		}
		offset_skb -= skb_frag_size(frag);
		++frag;
	}
	*offset_frag = 0;
	return frag;
}

/*
 * can_map_frag - 判断一个 skb frag 能否整页直接插入用户 TCP VMA。
 *
 * 必须恰好覆盖对齐的 PAGE_SIZE，且 backing page 不是 compound、没有 page->mapping。
 * 这些限制避免把大页子页或页缓存/文件映射以错误生命周期暴露；不满足时只能复制。
 */
static bool can_map_frag(const skb_frag_t *frag)
{
	struct page *page;

	if (skb_frag_size(frag) != PAGE_SIZE || skb_frag_off(frag))
		return false;

	page = skb_frag_page(frag);

	if (PageCompound(page) || page->mapping)
		return false;

	return true;
}

/* 返回从当前 frag 到下一个可映射整页之间必须复制/跳过的字节数；0 表示当前可映射。 */
static int find_next_mappable_frag(const skb_frag_t *frag,
				   int remaining_in_skb)
{
	int offset = 0;

	if (likely(can_map_frag(frag)))
		return 0;

	while (offset < remaining_in_skb && !can_map_frag(frag)) {
		offset += skb_frag_size(frag);
		++frag;
	}
	return offset;
}

/*
 * tcp_zerocopy_set_hint_for_skb - 告知用户在下一可映射页前应先普通 recv 的字节数。
 *
 * @offset 是 copied_seq 在 @skb 中的位置；函数只分析布局，写 zc->recv_skip_hint，
 * 不消费队列。若落在线性区、frag_list、最后一个 partial frag 或没有可映射页，hint
 * 保守覆盖剩余 skb；否则精确到下一个完整独占 page，用户据此 copy 后重试映射。
 */
static void tcp_zerocopy_set_hint_for_skb(struct sock *sk,
					  struct tcp_zerocopy_receive *zc,
					  struct sk_buff *skb, u32 offset)
{
	u32 frag_offset, partial_frag_remainder = 0;
	int mappable_offset;
	skb_frag_t *frag;

	/* worst case: skip to next skb. try to improve on this case below */
	/* 先给保守上界，后续只有确认更早可映射整页时才缩小 hint。 */
	zc->recv_skip_hint = skb->len - offset;

	/* Find the frag containing this offset (and how far into that frag) */
	/* copied_seq 可能落在 frag 中部，必须同时得到 descriptor 和相对偏移。 */
	frag = skb_advance_to_frag(skb, offset, &frag_offset);
	if (!frag)
		return;

	if (frag_offset) {
		struct skb_shared_info *info = skb_shinfo(skb);

		/* We read part of the last frag, must recvmsg() rest of skb. */
		/* 最后 frag 无后续整页可映射，只能把其余数据全部普通复制。 */
		if (frag == &info->frags[info->nr_frags - 1])
			return;

		/* Else, we must at least read the remainder in this frag. */
		/* 先复制当前 partial frag 尾部，下一 frag 才可能从页边界映射。 */
		partial_frag_remainder = skb_frag_size(frag) - frag_offset;
		zc->recv_skip_hint -= partial_frag_remainder;
		++frag;
	}

	/* partial_frag_remainder: If part way through a frag, must read rest.
	 * mappable_offset: Bytes till next mappable frag, *not* counting bytes
	 * in partial_frag_remainder.
	 */
	/* hint 等于 partial 尾数加其后不可映射 frags，总是指向下一整页边界。 */
	mappable_offset = find_next_mappable_frag(frag, zc->recv_skip_hint);
	zc->recv_skip_hint = mappable_offset + partial_frag_remainder;
}

static int tcp_recvmsg_locked(struct sock *sk, struct msghdr *msg, size_t len,
			      int flags, struct scm_timestamping_internal *tss,
			      int *cmsg_flags);
/*
 * receive_fallback_to_copy - 可读量可装入用户 copybuf 时退回普通非阻塞 recv。
 *
 * 先验证 64 位 UAPI 地址能无损转换为本机 unsigned long，import_ubuf 建立目标
 * iterator，再调用已持锁版 recvmsg。成功把实际复制量写 copybuf_len，并计算下一
 * 次 zerocopy 的 skip hint；zc->length 清零表示本轮没有页映射。负 errno 不提交
 * 额外状态，普通 recvmsg 已遵守自己的部分消费语义。
 */
static int receive_fallback_to_copy(struct sock *sk,
				    struct tcp_zerocopy_receive *zc, int inq,
				    struct scm_timestamping_internal *tss)
{
	unsigned long copy_address = (unsigned long)zc->copybuf_address;
	struct msghdr msg = {};
	int err;

	zc->length = 0;
	zc->recv_skip_hint = 0;

	if (copy_address != zc->copybuf_address)
		return -EINVAL;

	err = import_ubuf(ITER_DEST, (void __user *)copy_address, inq,
			  &msg.msg_iter);
	if (err)
		return err;

	err = tcp_recvmsg_locked(sk, &msg, inq, MSG_DONTWAIT,
				 tss, &zc->msg_flags);
	if (err < 0)
		return err;

	zc->copybuf_len = err;
	if (likely(zc->copybuf_len)) {
		struct sk_buff *skb;
		u32 offset;

		skb = tcp_recv_skb(sk, tcp_sk(sk)->copied_seq, &offset);
		if (skb)
			tcp_zerocopy_set_hint_for_skb(sk, zc, skb, offset);
	}
	return 0;
}

/*
 * tcp_copy_straggler_data - 复制无法整页映射的前缀/尾数并同步推进局部 seq/offset。
 *
 * 只有 copy 完整成功后才减少 skip_hint、推进游标；地址截断、import 或 copy fault
 * 返回错误，调用者不能提交这些字节。正返回值强转 s32 前由 UAPI 长度边界保证。
 */
static int tcp_copy_straggler_data(struct tcp_zerocopy_receive *zc,
				   struct sk_buff *skb, u32 copylen,
				   u32 *offset, u32 *seq)
{
	unsigned long copy_address = (unsigned long)zc->copybuf_address;
	struct msghdr msg = {};
	int err;

	if (copy_address != zc->copybuf_address)
		return -EINVAL;

	err = import_ubuf(ITER_DEST, (void __user *)copy_address, copylen,
			  &msg.msg_iter);
	if (err)
		return err;
	err = skb_copy_datagram_msg(skb, *offset, &msg, copylen);
	if (err)
		return err;
	zc->recv_skip_hint -= copylen;
	*offset += copylen;
	*seq += copylen;
	return (__s32)copylen;
}

/*
 * tcp_zc_handle_leftover - 页映射停止后，利用可选 copybuf 消费下一段不可映射字节。
 *
 * @skb 可空（可读量小于一页的路径），此时重新按 @seq 定位并采集 timestamp。
 * 返回实际成功复制量；copy 错误保存在 zc->copybuf_len 的负值中但返回 0，使主路径
 * 不推进 copied_seq。该双通道约定让 ioctl 同时报告映射进度和尾数复制错误。
 */
static int tcp_zc_handle_leftover(struct tcp_zerocopy_receive *zc,
				  struct sock *sk,
				  struct sk_buff *skb,
				  u32 *seq,
				  s32 copybuf_len,
				  struct scm_timestamping_internal *tss)
{
	u32 offset, copylen = min_t(u32, copybuf_len, zc->recv_skip_hint);

	if (!copylen)
		return 0;
	/* skb is null if inq < PAGE_SIZE. */
	/* 小于一页的快速路径尚未定位 skb，只有需要 copy 尾数时再查。 */
	if (skb) {
		offset = *seq - TCP_SKB_CB(skb)->seq;
	} else {
		skb = tcp_recv_skb(sk, *seq, &offset);
		if (TCP_SKB_CB(skb)->has_rxtstamp) {
			tcp_update_recv_tstamps(skb, tss);
			zc->msg_flags |= TCP_CMSG_TS;
		}
	}

	zc->copybuf_len = tcp_copy_straggler_data(zc, skb, copylen, &offset,
						  seq);
	return zc->copybuf_len < 0 ? 0 : copylen;
}

/*
 * tcp_zerocopy_vm_insert_batch_error - 处理 vm_insert_pages 部分成功后的重试与回滚。
 *
 * vm_insert_pages 即使返回错误也可能已插入前缀，所以 @pages_remaining、@address、
 * @seq/@length 都是输入输出账本。EBUSY 且用户声明 TLB_CLEAN_HINT 时，旧 PTE 可能
 * 尚在 VMA：先 zap 目标范围再重试。最终仍失败时只回滚未映射页对应的 length，
 * 并把这些字节加回 skip_hint；已经成功的映射不能撤销或重复消费。
 */
static int tcp_zerocopy_vm_insert_batch_error(struct vm_area_struct *vma,
					      struct page **pending_pages,
					      unsigned long pages_remaining,
					      unsigned long *address,
					      u32 *length,
					      u32 *seq,
					      struct tcp_zerocopy_receive *zc,
					      u32 total_bytes_to_map,
					      int err)
{
	/* At least one page did not map. Try zapping if we skipped earlier. */
	/* TLB-clean hint 可能错误，EBUSY 时用 zap 纠正后给本批一次重试机会。 */
	if (err == -EBUSY &&
	    zc->flags & TCP_RECEIVE_ZEROCOPY_FLAG_TLB_CLEAN_HINT) {
		u32 maybe_zap_len;

		/*
		 * 失败区间 = 原计划映射量 - 已映射或待提交量 + 最后一批未映射页。
		 * 只撤销失败尾部，保留已经成功建立的用户映射。
		 */
		maybe_zap_len = total_bytes_to_map -  /* 原计划映射的全部字节 */
				*length + /* 已映射或正在提交的字节 */
				(pages_remaining * PAGE_SIZE); /* 本批映射失败的尾部 */
		zap_vma_range(vma, *address, maybe_zap_len);
		err = 0;
	}

	if (!err) {
		unsigned long leftover_pages = pages_remaining;
		int bytes_mapped;

		/* We called zap_vma_range, try to reinsert. */
		/* 重试仍允许部分成功，pages_remaining 是唯一可靠的未映射数量。 */
		err = vm_insert_pages(vma, *address,
				      pending_pages,
				      &pages_remaining);
		bytes_mapped = PAGE_SIZE * (leftover_pages - pages_remaining);
		*seq += bytes_mapped;
		*address += bytes_mapped;
	}
	if (err) {
		/* Either we were unable to zap, OR we zapped, retried an
		 * insert, and still had an issue. Either ways, pages_remaining
		 * is the number of pages we were unable to map, and we unroll
		 * some state we speculatively touched before.
		 */
		/* 只撤销未映射后缀；已建立 PTE 的前缀必须继续计入 length/seq。 */
		const int bytes_not_mapped = PAGE_SIZE * pages_remaining;

		*length -= bytes_not_mapped;
		zc->recv_skip_hint += bytes_not_mapped;
	}
	return err;
}

/*
 * tcp_zerocopy_vm_insert_batch - 批量把接收 frag pages 插入用户 VMA并提交映射前缀。
 *
 * @pages_to_map 最多 32；@address/@seq 按实际成功页数各推进 PAGE_SIZE。返回 0 表示
 * 整批或错误恢复成功，非零表示仍有未映射页且状态已由 error helper 精确回滚。
 * 批处理摊薄 VMA/PTE 锁成本，但要求失败时显式处理部分成功，不能事务式假设全成全败。
 */
static int tcp_zerocopy_vm_insert_batch(struct vm_area_struct *vma,
					struct page **pages,
					unsigned int pages_to_map,
					unsigned long *address,
					u32 *length,
					u32 *seq,
					struct tcp_zerocopy_receive *zc,
					u32 total_bytes_to_map)
{
	unsigned long pages_remaining = pages_to_map;
	unsigned int pages_mapped;
	unsigned int bytes_mapped;
	int err;

	err = vm_insert_pages(vma, *address, pages, &pages_remaining);
	pages_mapped = pages_to_map - (unsigned int)pages_remaining;
	bytes_mapped = PAGE_SIZE * pages_mapped;
	/* Even if vm_insert_pages fails, it may have partially succeeded in
	 * mapping (some but not all of the pages).
	 */
	/* 因此无论 err 是否为零，都先按 pages_to_map-pages_remaining 提交地址和 seq。 */
	*seq += bytes_mapped;
	*address += bytes_mapped;

	if (likely(!err))
		return 0;

	/* Error: maybe zap and retry + rollback state for failed inserts. */
	/* error helper 接收成功前缀后的 pages 指针，避免重插已经映射的页面。 */
	return tcp_zerocopy_vm_insert_batch_error(vma, pages + pages_mapped,
		pages_remaining, address, length, seq, zc, total_bytes_to_map,
		err);
}

#define TCP_VALID_ZC_MSG_FLAGS   (TCP_CMSG_TS)
/*
 * tcp_zc_finalize_rx_tstamp - 把页映射期间积累的时间戳写回用户 control buffer。
 *
 * UAPI 用 64 位整数传地址/长度，先构造只用于 put_cmsg 的 msghdr；必须验证往返转换
 * 未截断。兼容进程设置 MSG_CMSG_COMPAT。输出更新 msg_control/controllen/msg_flags，
 * 无法安全表示地址时静默不给 cmsg，而不回滚已经完成的数据映射。
 */
static void tcp_zc_finalize_rx_tstamp(struct sock *sk,
				      struct tcp_zerocopy_receive *zc,
				      struct scm_timestamping_internal *tss)
{
	unsigned long msg_control_addr;
	struct msghdr cmsg_dummy;

	msg_control_addr = (unsigned long)zc->msg_control;
	cmsg_dummy.msg_control_user = (void __user *)msg_control_addr;
	cmsg_dummy.msg_controllen =
		(__kernel_size_t)zc->msg_controllen;
	cmsg_dummy.msg_flags = in_compat_syscall()
		? MSG_CMSG_COMPAT : 0;
	cmsg_dummy.msg_control_is_user = true;
	zc->msg_flags = 0;
	if (zc->msg_control == msg_control_addr &&
	    zc->msg_controllen == cmsg_dummy.msg_controllen) {
		tcp_recv_timestamp(&cmsg_dummy, sk, tss);
		zc->msg_control = (__u64)
			((uintptr_t)cmsg_dummy.msg_control_user);
		zc->msg_controllen =
			(__u64)cmsg_dummy.msg_controllen;
		zc->msg_flags = (__u32)cmsg_dummy.msg_flags;
	}
}

/*
 * find_tcp_vma - 查找覆盖 @address 且确由 tcp_mmap 创建的 VMA，并返回持锁对象。
 *
 * 先尝试 per-VMA RCU read lock；成功时 @mmap_locked=false，调用者用 vma_end_read。
 * fallback 取得整个 mmap_read_lock 并用 vma_lookup，输出 true，调用者用
 * mmap_read_unlock。类型不匹配或地址无 VMA 返回 NULL 且已释放所取锁。
 */
static struct vm_area_struct *find_tcp_vma(struct mm_struct *mm,
					   unsigned long address,
					   bool *mmap_locked)
{
	struct vm_area_struct *vma = lock_vma_under_rcu(mm, address);

	if (vma) {
		if (vma->vm_ops != &tcp_vm_ops) {
			vma_end_read(vma);
			return NULL;
		}
		*mmap_locked = false;
		return vma;
	}

	mmap_read_lock(mm);
	vma = vma_lookup(mm, address);
	if (!vma || vma->vm_ops != &tcp_vm_ops) {
		mmap_read_unlock(mm);
		return NULL;
	}
	*mmap_locked = true;
	return vma;
}

#define TCP_ZEROCOPY_PAGE_BATCH_SIZE 32
/*
 * tcp_zerocopy_receive - 将 receive queue 的完整独占页映射到用户预建只读 VMA。
 *
 * @sk 已持 socket lock；@zc 同时携带页对齐目标地址、最大映射长度、可选 copybuf、
 * TLB-clean hint 和全部输出；@tss 收集接收时间戳。成功返回 0，zc->length 是映射
 * 字节，copybuf_len 是尾数复制量，recv_skip_hint 指示下一不可映射前缀；EINVAL/
 * ENOTCONN/EIO 等表示参数、状态或 EOF。只有完成映射/复制的字节才推进 copied_seq。
 *
 * 方案并非把任意 TCP 数据“零拷贝”：线性 head、partial page、compound/page-cache
 * page、frag_list 都不能映射，需 copybuf 或普通 recv。映射前通常 zap 用户范围；
 * 以 32 页批处理 vm_insert_pages。VMA 页表获得 page 引用后 receive skb 才能消费，
 * 失败则按部分成功精确提交，避免用户映射与 TCP 序号游标不一致。
 */
static int tcp_zerocopy_receive(struct sock *sk,
				struct tcp_zerocopy_receive *zc,
				struct scm_timestamping_internal *tss)
{
	u32 length = 0, offset, vma_len, avail_len, copylen = 0;
	unsigned long address = (unsigned long)zc->address;
	struct page *pages[TCP_ZEROCOPY_PAGE_BATCH_SIZE];
	s32 copybuf_len = zc->copybuf_len;
	struct tcp_sock *tp = tcp_sk(sk);
	const skb_frag_t *frags = NULL;
	unsigned int pages_to_map = 0;
	struct vm_area_struct *vma;
	struct sk_buff *skb = NULL;
	u32 seq = tp->copied_seq;
	u32 total_bytes_to_map;
	int inq = tcp_inq(sk);
	bool mmap_locked;
	int ret;

	zc->copybuf_len = 0;
	zc->msg_flags = 0;

	/* 位掩码检查页对齐；往返比较阻止 32 位内核截断 64 位 UAPI 地址。 */
	if (address & (PAGE_SIZE - 1) || address != zc->address)
		return -EINVAL;

	if (sk->sk_state == TCP_LISTEN)
		return -ENOTCONN;

	sock_rps_record_flow(sk);

	if (inq && inq <= copybuf_len)
		return receive_fallback_to_copy(sk, zc, inq, tss);

	if (inq < PAGE_SIZE) {
		zc->length = 0;
		zc->recv_skip_hint = inq;
		if (!inq && sock_flag(sk, SOCK_DONE))
			return -EIO;
		return 0;
	}

	/* 只允许插入由 tcp_mmap 标记的当前进程 VMA，并记录对应解锁方式。 */
	vma = find_tcp_vma(current->mm, address, &mmap_locked);
	if (!vma)
		return -EINVAL;

	vma_len = min_t(unsigned long, zc->length, vma->vm_end - address);
	avail_len = min_t(u32, vma_len, inq);
	total_bytes_to_map = avail_len & ~(PAGE_SIZE - 1);
	if (total_bytes_to_map) {
		if (!(zc->flags & TCP_RECEIVE_ZEROCOPY_FLAG_TLB_CLEAN_HINT))
			zap_vma_range(vma, address, total_bytes_to_map);
		zc->length = total_bytes_to_map;
		zc->recv_skip_hint = 0;
	} else {
		zc->length = avail_len;
		zc->recv_skip_hint = avail_len;
	}
	ret = 0;
	/* 每轮只收集完整、可独立映射的 page；遇不可映射布局即返回 skip hint。 */
	while (length + PAGE_SIZE <= zc->length) {
		int mappable_offset;
		struct page *page;

		if (zc->recv_skip_hint < PAGE_SIZE) {
			u32 offset_frag;

			if (skb) {
				if (zc->recv_skip_hint > 0)
					break;
				skb = skb->next;
				offset = seq - TCP_SKB_CB(skb)->seq;
			} else {
				skb = tcp_recv_skb(sk, seq, &offset);
			}

			if (!skb_frags_readable(skb))
				break;

			if (TCP_SKB_CB(skb)->has_rxtstamp) {
				tcp_update_recv_tstamps(skb, tss);
				zc->msg_flags |= TCP_CMSG_TS;
			}
			zc->recv_skip_hint = skb->len - offset;
			frags = skb_advance_to_frag(skb, offset, &offset_frag);
			if (!frags || offset_frag)
				break;
		}

		mappable_offset = find_next_mappable_frag(frags,
							  zc->recv_skip_hint);
		if (mappable_offset) {
			zc->recv_skip_hint = mappable_offset;
			break;
		}
		page = skb_frag_page(frags);
		if (WARN_ON_ONCE(!page))
			break;

		prefetchw(page);
		pages[pages_to_map++] = page;
		length += PAGE_SIZE;
		zc->recv_skip_hint -= PAGE_SIZE;
		frags++;
		if (pages_to_map == TCP_ZEROCOPY_PAGE_BATCH_SIZE ||
		    zc->recv_skip_hint < PAGE_SIZE) {
			/* Either full batch, or we're about to go to next skb
			 * (and we cannot unroll failed ops across skbs).
			 */
			/* 失败回滚账本不能跨 skb，故切换 skb 前也必须强制提交当前小批。 */
			ret = tcp_zerocopy_vm_insert_batch(vma, pages,
							   pages_to_map,
							   &address, &length,
							   &seq, zc,
							   total_bytes_to_map);
			if (ret)
				goto out;
			pages_to_map = 0;
		}
	}
	if (pages_to_map) {
		ret = tcp_zerocopy_vm_insert_batch(vma, pages, pages_to_map,
						   &address, &length, &seq,
						   zc, total_bytes_to_map);
	}
out:
	if (mmap_locked)
		mmap_read_unlock(current->mm);
	else
		vma_end_read(vma);
	/* Try to copy straggler data. */
	if (!ret)
		copylen = tcp_zc_handle_leftover(zc, sk, skb, &seq, copybuf_len, tss);

	/* 页映射/PTE 已成功后才发布 copied_seq，并释放相应 receive queue/window 空间。 */
	if (length + copylen) {
		WRITE_ONCE(tp->copied_seq, seq);
		tcp_rcv_space_adjust(sk);

		/* Clean up data we have read: This will do ACK frames. */
		/* 映射建立后数据已对用户可见，再释放 receive buffer 并更新公告窗口。 */
		tcp_recv_skb(sk, seq, &offset);
		tcp_cleanup_rbuf(sk, length + copylen);
		ret = 0;
		if (length == zc->length)
			zc->recv_skip_hint = 0;
	} else {
		if (!zc->recv_skip_hint && sock_flag(sk, SOCK_DONE))
			ret = -EIO;
	}
	zc->length = length;
	return ret;
}
#endif

/* Similar to __sock_recv_timestamp, but does not require an skb */
/*
 * tcp_recv_timestamp - 从已缓存 tss 构造 SO_TIMESTAMP/TIMESTAMPING 控制消息。
 *
 * 数据 skb 可能已在 zerocopy/read 路径释放，所以函数不依赖 skb。它按 socket 的
 * NEW/OLD ABI 和 ns/us 精度选择 cmsg 布局，再依据软件/硬件 RX filter 清除未请求
 * 的样本；剩余 software/raw hardware 时间写 SCM_TIMESTAMPING。put_cmsg 的截断
 * 通过 msg flags 报告，不影响已交付 payload。
 */
void tcp_recv_timestamp(struct msghdr *msg, const struct sock *sk,
			struct scm_timestamping_internal *tss)
{
	int new_tstamp = sock_flag(sk, SOCK_TSTAMP_NEW);
	u32 tsflags = READ_ONCE(sk->sk_tsflags);

	if (tss->ts[0]) {
		if (sock_flag(sk, SOCK_RCVTSTAMP)) {
			struct timespec64 tv = ktime_to_timespec64(tss->ts[0]);

			if (sock_flag(sk, SOCK_RCVTSTAMPNS)) {
				if (new_tstamp) {
					struct __kernel_timespec kts = {
						.tv_sec = tv.tv_sec,
						.tv_nsec = tv.tv_nsec,
					};
					put_cmsg(msg, SOL_SOCKET, SO_TIMESTAMPNS_NEW,
						 sizeof(kts), &kts);
				} else {
					struct __kernel_old_timespec ts_old = {
						.tv_sec = tv.tv_sec,
						.tv_nsec = tv.tv_nsec,
					};
					put_cmsg(msg, SOL_SOCKET, SO_TIMESTAMPNS_OLD,
						 sizeof(ts_old), &ts_old);
				}
			} else {
				if (new_tstamp) {
					struct __kernel_sock_timeval stv = {
						.tv_sec = tv.tv_sec,
						.tv_usec = tv.tv_nsec / 1000,
					};
					put_cmsg(msg, SOL_SOCKET, SO_TIMESTAMP_NEW,
						 sizeof(stv), &stv);
				} else {
					struct __kernel_old_timeval otv = {
						.tv_sec = tv.tv_sec,
						.tv_usec = tv.tv_nsec / 1000,
					};
					put_cmsg(msg, SOL_SOCKET, SO_TIMESTAMP_OLD,
						 sizeof(otv), &otv);
				}
			}
		}

		if (!(tsflags & SOF_TIMESTAMPING_SOFTWARE &&
		    (tsflags & SOF_TIMESTAMPING_RX_SOFTWARE ||
		     !(tsflags & SOF_TIMESTAMPING_OPT_RX_FILTER))))
			tss->ts[0] = 0;
	}

	if (tss->ts[2]) {
		if (!(tsflags & SOF_TIMESTAMPING_RAW_HARDWARE &&
		    (tsflags & SOF_TIMESTAMPING_RX_HARDWARE ||
		     !(tsflags & SOF_TIMESTAMPING_OPT_RX_FILTER))))
			tss->ts[2] = 0;
	}

	if (tss->ts[0] | tss->ts[2]) {
		tss->ts[1] = 0;
		if (sock_flag(sk, SOCK_TSTAMP_NEW))
			put_cmsg_scm_timestamping64(msg, tss);
		else
			put_cmsg_scm_timestamping(msg, tss);
	}
}

/*
 * tcp_inq_hint - 无锁估算 rcv_nxt-copied_seq，竞态异常时回退持锁精确读取。
 *
 * 两次 copied_seq 读取检测 reader 并发推进；序号差解释成有符号负值也表明快照
 * 不一致。FIN 后即使 payload 为 0 也返回 1，提示 io_uring/用户继续调用 recv 以
 * 观察 EOF。结果只是 hint，不预留这些字节。
 */
static int tcp_inq_hint(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	u32 copied_seq = READ_ONCE(tp->copied_seq);
	u32 rcv_nxt = READ_ONCE(tp->rcv_nxt);
	int inq;

	inq = rcv_nxt - copied_seq;
	if (unlikely(inq < 0 || copied_seq != READ_ONCE(tp->copied_seq))) {
		lock_sock(sk);
		inq = tp->rcv_nxt - tp->copied_seq;
		release_sock(sk);
	}
	/* After receiving a FIN, tell the user-space to continue reading
	 * by returning a non-zero inq.
	 */
	/* 用 1 代表“还需观察 EOF”而非真实 payload，下一次 recv 最终返回 0。 */
	if (inq == 0 && sock_flag(sk, SOCK_DONE))
		inq = 1;
	return inq;
}

/* batch __xa_alloc() calls and reduce xa_lock()/xa_unlock() overhead. */
/*
 * device-memory 接收需要把用户 token 与 netmem 引用放进 sk_user_frags xarray。
 * pool 预分配最多 MAX_SKB_FRAGS 个 token，复制成功的前 idx 个提交真实 netmem，
 * 未使用后缀回滚；把多次 xa_lock 合成一次，代价是显式维护 max/idx 两个阶段游标。
 */
struct tcp_xa_pool {
	u8		max; /* max <= MAX_SKB_FRAGS；本批次预留槽位数。 */
	u8		idx; /* idx <= max */
	__u32		tokens[MAX_SKB_FRAGS];
	netmem_ref	netmems[MAX_SKB_FRAGS];
};

/* 调用者持 xa_lock_bh：提交已交给用户的 token，删除预留但未使用的 token。 */
static void tcp_xa_pool_commit_locked(struct sock *sk, struct tcp_xa_pool *p)
{
	int i;

	/* Commit part that has been copied to user space. */
	/* idx 前缀已有成功 cmsg，可把 XA_ZERO_ENTRY 原子替换为真实 netmem。 */
	for (i = 0; i < p->idx; i++)
		__xa_cmpxchg(&sk->sk_user_frags, p->tokens[i], XA_ZERO_ENTRY,
			     (__force void *)p->netmems[i], GFP_KERNEL);
	/* Rollback what has been pre-allocated and is no longer needed. */
	/* idx 之后未向用户发送 token，直接 erase 占位且不增加 netmem 引用。 */
	for (; i < p->max; i++)
		__xa_erase(&sk->sk_user_frags, p->tokens[i]);

	p->max = 0;
	p->idx = 0;
}

/* 若 pool 非空，取得 xarray BH 锁并原子完成提交与回滚；返回后 pool 游标清零。 */
static void tcp_xa_pool_commit(struct sock *sk, struct tcp_xa_pool *p)
{
	if (!p->max)
		return;

	xa_lock_bh(&sk->sk_user_frags);

	tcp_xa_pool_commit_locked(sk, p);

	xa_unlock_bh(&sk->sk_user_frags);
}

/*
 * tcp_xa_pool_refill - 提交上一批 token，并为后续至多 @max_frags 个 frag 预留新 token。
 *
 * pool 尚有未消费 token 时直接成功。否则在一次 xa_lock_bh 临界区先 commit/rollback
 * 旧批，再以 XA_ZERO_ENTRY 占位分配 31-bit token。至少成功一个即返回 0，调用者
 * 可取得部分批次；一个也没有才返回 xa errno。占位必须最终由 commit 转真实
 * netmem 或 erase，防止 token 泄漏。
 */
static int tcp_xa_pool_refill(struct sock *sk, struct tcp_xa_pool *p,
			      unsigned int max_frags)
{
	int err, k;

	if (p->idx < p->max)
		return 0;

	xa_lock_bh(&sk->sk_user_frags);

	tcp_xa_pool_commit_locked(sk, p);

	for (k = 0; k < max_frags; k++) {
		err = __xa_alloc(&sk->sk_user_frags, &p->tokens[k],
				 XA_ZERO_ENTRY, xa_limit_31b, GFP_KERNEL);
		if (err)
			break;
	}

	xa_unlock_bh(&sk->sk_user_frags);

	p->max = k;
	p->idx = 0;
	return k ? 0 : err;
}

/* On error, returns the -errno. On success, returns number of bytes sent to the
 * user. May not consume all of @remaining_len.
 */
/*
 * tcp_recvmsg_dmabuf - 把 device-memory skb 描述成控制消息，而不复制 DMA-BUF payload。
 *
 * @skb 是当前位置的借用对象；@offset/@remaining_len 是本轮连续区间；@msg iterator
 * 接收可能存在的 linear header，control buffer 接收每个 device frag 的 dmabuf_id、
 * virtual offset、size 和新 token。返回已交付字节，可短读；零进度错误返回负 errno。
 *
 * 每个成功 cmsg 后增加 net_iov page-pool 引用，并把 token->netmem 延迟到 xarray
 * batch commit，用户稍后凭 token 归还 buffer。若 put_cmsg/fault 中途失败，已发送
 * token 必须提交，未发送预留必须擦除，因此所有出口统一 tcp_xa_pool_commit。
 * skb_frags_readable 为 false 是设备内存不应被 CPU 普通复制的类型保证，逐 frag
 * 仍防御性复查，避免错误元数据导致把普通 page 当 net_iov 解引用。
 */
static int tcp_recvmsg_dmabuf(struct sock *sk, const struct sk_buff *skb,
			      unsigned int offset, struct msghdr *msg,
			      int remaining_len)
{
	struct dmabuf_cmsg dmabuf_cmsg = { 0 };
	struct tcp_xa_pool tcp_xa_pool;
	unsigned int start;
	int i, copy, n;
	int sent = 0;
	int err = 0;

	tcp_xa_pool.max = 0;
	tcp_xa_pool.idx = 0;
	do {
		start = skb_headlen(skb);

		if (skb_frags_readable(skb)) {
			err = -ENODEV;
			goto out;
		}

		/* Copy header. */
		/* linear 区仍在普通内存，必须复制并用 SO_DEVMEM_LINEAR 描述这段混合布局。 */
		copy = start - offset;
		if (copy > 0) {
			copy = min(copy, remaining_len);

			n = copy_to_iter(skb->data + offset, copy,
					 &msg->msg_iter);
			if (n != copy) {
				err = -EFAULT;
				goto out;
			}

			offset += copy;
			remaining_len -= copy;

			/* First a dmabuf_cmsg for # bytes copied to user
			 * buffer.
			 */
			/* 先发 linear 描述，保持 control message 顺序与 TCP 字节顺序一致。 */
			memset(&dmabuf_cmsg, 0, sizeof(dmabuf_cmsg));
			dmabuf_cmsg.frag_size = copy;
			err = put_cmsg_notrunc(msg, SOL_SOCKET,
					       SO_DEVMEM_LINEAR,
					       sizeof(dmabuf_cmsg),
					       &dmabuf_cmsg);
			if (err)
				goto out;

			sent += copy;

			if (remaining_len == 0)
				goto out;
		}

		/* after that, send information of dmabuf pages through a
		 * sequence of cmsg
		 */
		/* 每个 frag 只传位置与 token；payload 留在 device memory，不由 CPU copy。 */
		for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
			skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
			struct net_iov *niov;
			u64 frag_offset;
			int end;

			/* !skb_frags_readable() should indicate that ALL the
			 * frags in this skb are dmabuf net_iovs. We're checking
			 * for that flag above, but also check individual frags
			 * here. If the tcp stack is not setting
			 * skb_frags_readable() correctly, we still don't want
			 * to crash here.
			 */
			/* 外层类型位若损坏也不能盲目强转，逐 frag 验证是防崩溃的第二道边界。 */
			if (!skb_frag_net_iov(frag)) {
				net_err_ratelimited("Found non-dmabuf skb with net_iov");
				err = -ENODEV;
				goto out;
			}

			niov = skb_frag_net_iov(frag);
			if (!net_is_devmem_iov(niov)) {
				err = -ENODEV;
				goto out;
			}

			end = start + skb_frag_size(frag);
			copy = end - offset;

			if (copy > 0) {
				copy = min(copy, remaining_len);

				frag_offset = net_iov_virtual_addr(niov) +
					      skb_frag_off(frag) + offset -
					      start;
				dmabuf_cmsg.frag_offset = frag_offset;
				dmabuf_cmsg.frag_size = copy;
				err = tcp_xa_pool_refill(sk, &tcp_xa_pool,
							 skb_shinfo(skb)->nr_frags - i);
				if (err)
					goto out;

				/* Will perform the exchange later */
				/* token 先占位，只有 cmsg 成功后才增加引用并在批末发布映射。 */
				dmabuf_cmsg.frag_token = tcp_xa_pool.tokens[tcp_xa_pool.idx];
				dmabuf_cmsg.dmabuf_id = net_devmem_iov_binding_id(niov);

				offset += copy;
				remaining_len -= copy;

				err = put_cmsg_notrunc(msg, SOL_SOCKET,
						       SO_DEVMEM_DMABUF,
						       sizeof(dmabuf_cmsg),
						       &dmabuf_cmsg);
				if (err)
					goto out;

				atomic_long_inc(&niov->desc.pp_ref_count);
				tcp_xa_pool.netmems[tcp_xa_pool.idx++] = skb_frag_netmem(frag);

				sent += copy;

				if (remaining_len == 0)
					goto out;
			}
			start = end;
		}

		tcp_xa_pool_commit(sk, &tcp_xa_pool);
		if (!remaining_len)
			goto out;

		/* if remaining_len is not satisfied yet, we need to go to the
		 * next frag in the frag_list to satisfy remaining_len.
		 */
		/* frag_list 优先保持聚合 skb 的逻辑顺序，否则沿 receive queue next 继续。 */
		skb = skb_shinfo(skb)->frag_list ?: skb->next;

		offset = offset - start;
	} while (skb);

	if (remaining_len) {
		err = -EFAULT;
		goto out;
	}

out:
	tcp_xa_pool_commit(sk, &tcp_xa_pool);
	if (!sent)
		sent = err;

	return sent;
}

/*
 *	This routine copies from a sock struct into the user buffer.
 *
 *	Technical note: in 2.3 we work on _locked_ socket, so that
 *	tricks with *seq access order and skb->users are not required.
 *	Probably, code can be easily improved even more.
 */

/*
 * tcp_recvmsg_locked - 从 TCP 有序 receive queue 向用户消息交付连续字节。
 *
 * @sk 是已由 lock_sock 串行化的 TCP socket；@msg->msg_iter 是输出目标并会
 * 推进；@len 是最多请求字节；@flags 控制 PEEK/WAITALL/OOB/TRUNC 等语义；
 * @tss/@cmsg_flags 是输出参数，记录需要在解锁后生成的时间戳控制消息。
 * 返回非负复制字节数，0 可表示 FIN 后 EOF；负 errno 表示未取得数据的失败。
 * MSG_PEEK 使用局部 peek_seq，不消费 tp->copied_seq 和 skb ownership；普通
 * 读取推进 copied_seq，完全消费 skb 时从 receive queue 摘除并释放接收记账。
 *
 * 局部调用地图：
 *
 *   tcp_recvmsg
 *     -> [可选] sk_busy_loop             主动 poll NAPI 降低等待延迟
 *     -> lock_sock
 *     -> tcp_recvmsg_locked
 *        -> skb_queue_walk               寻找覆盖 copied_seq 的有序 skb
 *        -> skb_copy_datagram_msg        linear/frags -> 用户 iterator
 *        -> tcp_eat_recv_skb             完全消费后摘除 skb/归还 rmem
 *        -> tcp_cleanup_rbuf              评估 ACK 和 receive-window update
 *        -> sk_wait_data                  无数据时无丢失唤醒地睡眠
 *     -> release_sock                    顺带处理 RX softirq 留下的 backlog
 *
 * 三个不能混淆的游标：
 *
 *   rcv_nxt：    TCP 已连续接收的右边界，由收包路径推进；
 *   copied_seq：应用已经消费的右边界，由非 PEEK recv 推进；
 *   peek_seq：   单次 MSG_PEEK 的临时观察位置，不改变 socket 状态。
 *
 * 正常情况下 copied_seq <= rcv_nxt。两者差值附近的数据构成应用可读积压；
 * out_of_order_queue 中洞后的数据不在本函数扫描范围内。
 */
static int tcp_recvmsg_locked(struct sock *sk, struct msghdr *msg, size_t len,
			      int flags, struct scm_timestamping_internal *tss,
			      int *cmsg_flags)
{
	/*
	 * 变量地图：
	 *   seq       指向 copied_seq；PEEK 时改指向栈上的 peek_seq。
	 *   offset    当前 seq 相对一个 skb 起始 TCP seq 的字节偏移。
	 *   used      本轮实际交付量，受 skb 剩余和调用者 len 双重限制。
	 *   target    本次至少希望取得的字节；受 SO_RCVLOWAT/MSG_WAITALL 影响。
	 *   last      睡眠前看到的队尾；sk_wait_data 用它判断是否已有新数据到达。
	 *   urg_hole  OOB 字节未内联时在序列空间中跳过的 1 字节。
	 */
	struct tcp_sock *tp = tcp_sk(sk);
	int last_copied_dmabuf = -1; /* 尚未向控制消息报告任何 dmabuf 片段。 */
	int copied = 0;
	u32 peek_seq;
	u32 *seq;
	unsigned long used;
	int err;
	int target;		/* 至少读取这么多字节，除非 EOF、错误或超时先发生。 */
	long timeo;
	struct sk_buff *skb, *last;
	u32 peek_offset = 0;
	u32 urg_hole = 0;

	/* 阶段 1：确认 socket/特殊模式，并计算阻塞语义与最低返回目标。 */
	err = -ENOTCONN;
	if (sk->sk_state == TCP_LISTEN)
		/* LISTEN socket 没有连接字节流；应用应使用 accept 而非 recv。 */
		goto out;

	if (tp->recvmsg_inq)
		/* 请求 TCP_INQ cmsg 时只记录需求，实际剩余字节在解锁后计算。 */
		*cmsg_flags = TCP_CMSG_INQ;
	timeo = sock_rcvtimeo(sk, flags & MSG_DONTWAIT);

	/* Urgent data needs to be handled specially. */
	/* MSG_OOB 读取 urgent byte，不走普通连续 payload 消费循环。 */
	if (flags & MSG_OOB)
		goto recv_urg;

	if (unlikely(tp->repair)) {
		/* repair 仅允许 PEEK 队列内容，避免普通 recv 改变待恢复的协议状态。 */
		err = -EPERM;
		if (!(flags & MSG_PEEK))
			goto out;

		if (tp->repair_queue == TCP_SEND_QUEUE)
			goto recv_sndq;

		err = -EINVAL;
		if (tp->repair_queue == TCP_NO_QUEUE)
			goto out;

		/* 'common' recv queue MSG_PEEK-ing */
	}

	/* copied_seq 是应用下一个要消费的序列号；PEEK 必须使用副本保持非消费语义。 */
	seq = &tp->copied_seq;
	if (flags & MSG_PEEK) {
		/*
		 * sk_peek_offset 支持多次 PEEK 的可选偏移。max(...,0) 把“未设置”的
		 * 负哨兵转换为 0；所有运算在 32-bit TCP 序列空间按 wrap-safe 规则用。
		 */
		peek_offset = max(sk_peek_offset(sk, flags), 0);
		peek_seq = tp->copied_seq + peek_offset;
		seq = &peek_seq;
	}

	/* WAITALL 会提高目标，但 EOF、错误、信号仍可使调用提前短读。 */
	target = sock_rcvlowat(sk, flags & MSG_WAITALL, len);

	do {
		u32 offset;

		/* Are we at urgent data? Stop if we have read anything or have SIGURG pending. */
		if (unlikely(tp->urg_data) && tp->urg_seq == *seq) {
			/* 非内联 urgent byte 位于当前游标时，普通数据不能悄悄跨过信号边界。 */
			if (copied)
				break;
			if (signal_pending(current)) {
				copied = timeo ? sock_intr_errno(timeo) : -EAGAIN;
				break;
			}
		}

		/* Next get a buffer. */

		/*
		 * 阶段 2：按序扫描 receive queue，寻找覆盖当前 seq 的 skb。offset
		 * 是字节序列差，不是数组下标猜测；队列应不存在 seq 之前的洞。
		 */
		/* peek_tail 返回借用指针；socket lock 保证遍历期间队列不会被并发用户修改。 */
		last = skb_peek_tail(&sk->sk_receive_queue);
		skb_queue_walk(&sk->sk_receive_queue, skb) {
			/*
			 * skb_queue_walk 是侵入式链表宏：skb 本身含 next/prev，不创建数组。
			 * TCP_SKB_CB(skb) 把通用 skb->cb 强制解释成 tcp_skb_cb；只有当前
			 * 协议拥有 cb 时这种类型复用才有效。
			 */
			last = skb;
			/* Now that we have two receive queues this
			 * shouldn't happen.
			 */
			/* 两条接收队列已按序分工；若序号倒退，说明队列迁移或排序不变量被破坏。 */
			if (WARN(before(*seq, TCP_SKB_CB(skb)->seq),
				 "TCP recvmsg seq # bug: copied %X, seq %X, rcvnxt %X, fl %X\n",
				 *seq, TCP_SKB_CB(skb)->seq, tp->rcv_nxt,
				 flags))
				break;

			/* u32 自然回绕与 TCP 序列空间一致；前面的 before() 负责顺序判定。 */
			offset = *seq - TCP_SKB_CB(skb)->seq;
			if (unlikely(TCP_SKB_CB(skb)->tcp_flags & TCPHDR_SYN)) {
				pr_err_once("%s: found a SYN, please report !\n", __func__);
				offset--;
			}
			if (offset < skb->len)
				/* 当前 skb 覆盖 seq，跳到复制块而不是继续扫描。 */
				goto found_ok_skb;
			if (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN)
				goto found_fin_ok;
			WARN(!(flags & MSG_PEEK),
			     "TCP recvmsg seq # bug 2: copied %X, seq %X, rcvnxt %X, fl %X\n",
			     *seq, TCP_SKB_CB(skb)->seq, tp->rcv_nxt, flags);
		}

		/* Well, if we have backlog, try to process it now yet. */

		/* 已达目标仍需确认 backlog 为空，否则那里可能有能立即合并的新数据。 */
		if (copied >= target && !READ_ONCE(sk->sk_backlog.tail))
			break;

		if (copied) {
			/* 已有进度时 timeout、push/urgent 等停止条件产生短读，而不是错误。 */
			if (!timeo ||
			    tcp_recv_should_stop(sk))
				break;
		} else {
			if (sock_flag(sk, SOCK_DONE))
				/* 协议已完成且队列空：正常 EOF，copied 保持 0。 */
				break;

			if (sk->sk_err) {
				/* sock_error 原子取走 pending error；仅在零进度时作为返回值。 */
				copied = sock_error(sk);
				break;
			}

			if (sk->sk_shutdown & RCV_SHUTDOWN)
				/* 已收到 FIN/本地关闭接收且无排队数据，返回 0 EOF。 */
				break;

			if (sk->sk_state == TCP_CLOSE) {
				/* This occurs when user tries to read
				 * from never connected socket.
				 */
				/* 从未连接的 CLOSED socket 没有正常 EOF 语义，应报告 ENOTCONN。 */
				copied = -ENOTCONN;
				break;
			}

			if (!timeo) {
				/* MSG_DONTWAIT 或超时耗尽：队列空时返回 EAGAIN。 */
				copied = -EAGAIN;
				break;
			}

			if (signal_pending(current)) {
				copied = sock_intr_errno(timeo);
				break;
			}
		}

		if (copied >= target) {
			/* Do not sleep, just process backlog. */
			/* 用户持锁期间软中断延后的 skb 在这里转入真正 TCP 状态机。 */
			__sk_flush_backlog(sk);
		} else {
			/*
			 * 阶段 3：数据不足。先根据已消费空间决定 ACK/window update，再把
			 * 当前任务放入 socket waitqueue。sk_wait_data 会正确处理唤醒、
			 * timeout 和 signal，避免“检查为空后、入睡前”丢失唤醒。
			 */
			tcp_cleanup_rbuf(sk, copied);
			err = sk_wait_data(sk, &timeo, last);
			if (err < 0) {
				err = copied ? : err;
				goto out;
			}
		}

		if ((flags & MSG_PEEK) &&
		    (peek_seq - peek_offset - copied - urg_hole != tp->copied_seq)) {
			net_dbg_ratelimited("TCP(%s:%d): Application bug, race in MSG_PEEK\n",
					    current->comm,
					    task_pid_nr(current));
			/*
			 * 另一个共享 fd 的 reader 可在本线程睡眠时推进 copied_seq。PEEK
			 * 不再基于旧基线继续，否则可能重复/跳过不可预测区域；重同步并告警。
			 */
			peek_seq = tp->copied_seq + peek_offset;
		}
		continue;

found_ok_skb:
		/* Ok so how much can we use? */
		/* 阶段 4：本轮只复制 skb 剩余、用户剩余两者的较小值，允许部分消费。 */
		used = skb->len - offset;
		if (len < used)
			used = len;

		/* Do we have urgent data here? */
		if (unlikely(tp->urg_data)) {
			/*
			 * urgent byte 位于本轮范围时：SO_OOBINLINE 关闭则跳过它并计入
			 * urg_hole；位于范围中间则先只交付 urgent byte 之前的普通数据。
			 */
			u32 urg_offset = tp->urg_seq - *seq;
			if (urg_offset < used) {
				if (!urg_offset) {
					if (!sock_flag(sk, SOCK_URGINLINE)) {
						WRITE_ONCE(*seq, *seq + 1);
						urg_hole++;
						offset++;
						used--;
						if (!used)
							goto skip_copy;
					}
				} else
					used = urg_offset;
			}
		}

		if (!(flags & MSG_TRUNC)) {
			/* MSG_TRUNC 只推进/丢弃相应字节，不把 payload 写入用户 iterator。 */
			if (last_copied_dmabuf != -1 &&
			    last_copied_dmabuf != !skb_frags_readable(skb))
				/* 单次返回不混合两种用户可见承载协议，已有进度则短读。 */
				break;

			if (skb_frags_readable(skb)) {
				/* 普通 skb 将 linear/frags 统一复制到 msg iterator。 */
				err = skb_copy_datagram_msg(skb, offset, msg,
							    used);
				if (err) {
					/* Exception. Bailout! */
					/* copy fault 前已有进度则返回短读；否则返回 -EFAULT。 */
					if (!copied)
						copied = -EFAULT;
					break;
				}
			} else {
				if (!(flags & MSG_SOCK_DEVMEM)) {
					/* dmabuf skbs can only be received
					 * with the MSG_SOCK_DEVMEM flag.
					 */
					/* 设备内存不能按普通用户缓冲复制；调用者必须显式请求其元数据。 */
					if (!copied)
						copied = -EFAULT;

					break;
				}

				err = tcp_recvmsg_dmabuf(sk, skb, offset, msg,
							 used);
				if (err < 0) {
					if (!copied)
						copied = err;

					break;
				}
				used = err;
			}
		}

		/* `!` 把布尔条件规范化为 0/1，-1 仍表示尚未复制任何承载类型。 */
		last_copied_dmabuf = !skb_frags_readable(skb);

		/*
		 * 复制成功后才提交消费序列号；若 copy_to_user fault，不能越过未交付
		 * 字节。对 PEEK 该写入只改局部 peek_seq，不改变 socket 可读状态。
		 */
		WRITE_ONCE(*seq, *seq + used);
		copied += used;
		len -= used;
		if (flags & MSG_PEEK)
			/* 只推进该 file/socket 的 peek bookkeeping，不释放 receive skb。 */
			sk_peek_offset_fwd(sk, used);
		else
			/* 普通消费使历史 peek offset 相对新 copied_seq 后退。 */
			sk_peek_offset_bwd(sk, used);
		/* 根据应用消费速度更新自动 receive-buffer/window 调节样本。 */
		tcp_rcv_space_adjust(sk);

skip_copy:
		if (unlikely(tp->urg_data) && after(tp->copied_seq, tp->urg_seq)) {
			WRITE_ONCE(tp->urg_data, 0);
			tcp_fast_path_check(sk);
		}

		if (TCP_SKB_CB(skb)->has_rxtstamp) {
			/* 保存最后相关接收时间戳，真正 cmsg 在 socket 解锁后生成。 */
			tcp_update_recv_tstamps(skb, tss);
			*cmsg_flags |= TCP_CMSG_TS;
		}

		/* skb 仍有 payload：保留在队首，下一循环从新 seq/offset 继续。 */
		if (used + offset < skb->len)
			continue;

		if (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN)
			goto found_fin_ok;
		if (!(flags & MSG_PEEK))
			/* 完全消费后摘除 skb，并归还 sk_rmem 记账；PEEK 保留它。 */
			tcp_eat_recv_skb(sk, skb);
		continue;

found_fin_ok:
		/* Process the FIN. */
		/* FIN 在 TCP 序列空间占 1，但不向用户复制字节；其后无数据时 recv 返回 0。 */
		WRITE_ONCE(*seq, *seq + 1);
		if (!(flags & MSG_PEEK))
			tcp_eat_recv_skb(sk, skb);
		break;
	} while (len > 0);

	/* According to UNIX98, msg_name/msg_namelen are ignored
	 * on connected socket. I was just happy when found this 8) --ANK
	 */
	/* TCP 是已连接字节流，recvmsg 不返回“每段发送者地址”。 */

	/* Clean up data we have read: This will do ACK frames. */
	/* 阶段 5：根据释放的接收空间决定是否立即 ACK/公告更大 rwnd。 */
	tcp_cleanup_rbuf(sk, copied);
	return copied;

out:
	/* 只有尚未取得任何普通数据的早期失败才直接走此返回。 */
	return err;

recv_urg:
	/* 特殊 OOB helper 自己实现 urgent-byte 复制和状态消费。 */
	err = tcp_recv_urg(sk, msg, len, flags);
	goto out;

recv_sndq:
	/* repair 模式只观察 send queue，不改变正常 receive copied_seq。 */
	err = tcp_peek_sndq(sk, msg, len);
	goto out;
}

/*
 * tcp_recvmsg - TCP 用户接收入口。
 *
 * 参数/返回语义见 tcp_recvmsg_locked。普通路径可先 busy-poll 关联 NAPI，以持续
 * 占用当前 CPU 换取更低设备到 socket 延迟；随后必须取得 socket 用户锁。
 * 解锁后才生成部分 cmsg，缩短 TCP 状态临界区。MSG_ERRQUEUE 读取异步错误/zero
 * copy completion，不消费普通 receive queue。
 */
int tcp_recvmsg(struct sock *sk, struct msghdr *msg, size_t len, int flags)
{
	int cmsg_flags = 0, ret;
	struct scm_timestamping_internal tss;

	if (unlikely(flags & MSG_ERRQUEUE))
		return inet_recv_error(sk, msg, len);

	if (sk_can_busy_loop(sk) &&
	    skb_queue_empty_lockless(&sk->sk_receive_queue) &&
	    sk->sk_state == TCP_ESTABLISHED)
		/* 仅在队列空且 ESTABLISHED 时主动 poll；这是可选延迟优化，不改语义。 */
		sk_busy_loop(sk, flags & MSG_DONTWAIT);

	lock_sock(sk);
	ret = tcp_recvmsg_locked(sk, msg, len, flags, &tss, &cmsg_flags);
	release_sock(sk);

	if ((cmsg_flags | msg->msg_get_inq) && ret >= 0) {
		if (cmsg_flags & TCP_CMSG_TS)
			tcp_recv_timestamp(msg, sk, &tss);
		if ((cmsg_flags & TCP_CMSG_INQ) | msg->msg_get_inq) {
			msg->msg_inq = tcp_inq_hint(sk);
			if (cmsg_flags & TCP_CMSG_INQ)
				put_cmsg(msg, SOL_TCP, TCP_CM_INQ,
					 sizeof(msg->msg_inq), &msg->msg_inq);
		}
	}
	return ret;
}

/*
 * tcp_set_state - 提交 TCP 状态变化及其 hash、端口、统计和 BPF 副作用。
 *
 * @sk 已由调用路径串行化；@state 是 TCP_* 内部状态。函数不是普通字段赋值：先向
 * BPF 报告 old/new，维护 CurrEstab/EstabResets；进入 CLOSE 时先从连接 hash 摘除
 * 并按 bind 锁策略归还端口，最后用 inet_sk_state_store 发布新状态。必须先 unhash
 * 后发布 CLOSE，避免无锁查找仍命中一个已宣告关闭的 socket。无返回值。
 * BUILD_BUG_ON 保证内部枚举仍可直接传给稳定 BPF UAPI；BTF 显式 emit 只影响类型
 * 元数据，不在运行时转换状态。
 */
void tcp_set_state(struct sock *sk, int state)
{
	int oldstate = sk->sk_state;

	/* We defined a new enum for TCP states that are exported in BPF
	 * so as not force the internal TCP states to be frozen. The
	 * following checks will detect if an internal state value ever
	 * differs from the BPF value. If this ever happens, then we will
	 * need to remap the internal value to the BPF value before calling
	 * tcp_call_bpf_2arg.
	 */
	/* 编译期逐值比对允许直接把内部 state 交给 BPF；若未来分叉必须增加显式映射。 */
	BUILD_BUG_ON((int)BPF_TCP_ESTABLISHED != (int)TCP_ESTABLISHED);
	BUILD_BUG_ON((int)BPF_TCP_SYN_SENT != (int)TCP_SYN_SENT);
	BUILD_BUG_ON((int)BPF_TCP_SYN_RECV != (int)TCP_SYN_RECV);
	BUILD_BUG_ON((int)BPF_TCP_FIN_WAIT1 != (int)TCP_FIN_WAIT1);
	BUILD_BUG_ON((int)BPF_TCP_FIN_WAIT2 != (int)TCP_FIN_WAIT2);
	BUILD_BUG_ON((int)BPF_TCP_TIME_WAIT != (int)TCP_TIME_WAIT);
	BUILD_BUG_ON((int)BPF_TCP_CLOSE != (int)TCP_CLOSE);
	BUILD_BUG_ON((int)BPF_TCP_CLOSE_WAIT != (int)TCP_CLOSE_WAIT);
	BUILD_BUG_ON((int)BPF_TCP_LAST_ACK != (int)TCP_LAST_ACK);
	BUILD_BUG_ON((int)BPF_TCP_LISTEN != (int)TCP_LISTEN);
	BUILD_BUG_ON((int)BPF_TCP_CLOSING != (int)TCP_CLOSING);
	BUILD_BUG_ON((int)BPF_TCP_NEW_SYN_RECV != (int)TCP_NEW_SYN_RECV);
	BUILD_BUG_ON((int)BPF_TCP_BOUND_INACTIVE != (int)TCP_BOUND_INACTIVE);
	BUILD_BUG_ON((int)BPF_TCP_MAX_STATES != (int)TCP_MAX_STATES);

	/* bpf uapi header bpf.h defines an anonymous enum with values
	 * BPF_TCP_* used by bpf programs. Currently gcc built vmlinux
	 * is able to emit this enum in DWARF due to the above BUILD_BUG_ON.
	 * But clang built vmlinux does not have this enum in DWARF
	 * since clang removes the above code before generating IR/debuginfo.
	 * Let us explicitly emit the type debuginfo to ensure the
	 * above-mentioned anonymous enum in the vmlinux DWARF and hence BTF
	 * regardless of which compiler is used.
	 */
	/* Clang 会优化掉纯断言，显式 BTF emit 保证 CO-RE/BPF 仍能看到匿名枚举类型。 */
	BTF_TYPE_EMIT_ENUM(BPF_TCP_ESTABLISHED);

	if (BPF_SOCK_OPS_TEST_FLAG(tcp_sk(sk), BPF_SOCK_OPS_STATE_CB_FLAG))
		tcp_call_bpf_2arg(sk, BPF_SOCK_OPS_STATE_CB, oldstate, state);

	switch (state) {
	case TCP_ESTABLISHED:
		if (oldstate != TCP_ESTABLISHED)
			TCP_INC_STATS(sock_net(sk), TCP_MIB_CURRESTAB);
		break;
	case TCP_CLOSE_WAIT:
		if (oldstate == TCP_SYN_RECV)
			TCP_INC_STATS(sock_net(sk), TCP_MIB_CURRESTAB);
		break;

	case TCP_CLOSE:
		if (oldstate == TCP_CLOSE_WAIT || oldstate == TCP_ESTABLISHED)
			TCP_INC_STATS(sock_net(sk), TCP_MIB_ESTABRESETS);

		sk->sk_prot->unhash(sk);
		if (inet_csk(sk)->icsk_bind_hash &&
		    !(sk->sk_userlocks & SOCK_BINDPORT_LOCK))
			inet_put_port(sk);
		fallthrough;
	default:
		if (oldstate == TCP_ESTABLISHED || oldstate == TCP_CLOSE_WAIT)
			TCP_DEC_STATS(sock_net(sk), TCP_MIB_CURRESTAB);
	}

	/* Change state AFTER socket is unhashed to avoid closed
	 * socket sitting in hash tables.
	 */
	/* 先让网络查找不可见，再发布 CLOSE，保证 hash 可见性与状态不发生矛盾。 */
	inet_sk_state_store(sk, state);
}

/*
 *	State processing on a close. This implements the state shift for
 *	sending our FIN frame. Note that we only send a FIN for some
 *	states. A shutdown() may have already sent the FIN, or we may be
 *	closed.
 */
/* new_state 把“目标状态”和“是否需新排 FIN”编码在同一字节，避免分散 switch 漏状态。 */

static const unsigned char new_state[16] = {
  /* 当前状态：             新状态：        附加动作： */
  [0 /* 无效状态槽 */]	= TCP_CLOSE,
  [TCP_ESTABLISHED]	= TCP_FIN_WAIT1 | TCP_ACTION_FIN,
  [TCP_SYN_SENT]	= TCP_CLOSE,
  [TCP_SYN_RECV]	= TCP_FIN_WAIT1 | TCP_ACTION_FIN,
  [TCP_FIN_WAIT1]	= TCP_FIN_WAIT1,
  [TCP_FIN_WAIT2]	= TCP_FIN_WAIT2,
  [TCP_TIME_WAIT]	= TCP_CLOSE,
  [TCP_CLOSE]		= TCP_CLOSE,
  [TCP_CLOSE_WAIT]	= TCP_LAST_ACK  | TCP_ACTION_FIN,
  [TCP_LAST_ACK]	= TCP_LAST_ACK,
  [TCP_LISTEN]		= TCP_CLOSE,
  [TCP_CLOSING]		= TCP_CLOSING,
  [TCP_NEW_SYN_RECV]	= TCP_CLOSE,	/* should not happen ! */
};

/* 查 new_state 表提交 close 状态，并返回是否还需由调用者实际排队 FIN。 */
static int tcp_close_state(struct sock *sk)
{
	int next = (int)new_state[sk->sk_state];
	int ns = next & TCP_STATE_MASK;

	tcp_set_state(sk, ns);

	return next & TCP_ACTION_FIN;
}

/*
 *	Shutdown the sending side of a connection. Much like close except
 *	that we don't receive shut down or sock_set_flag(sk, SOCK_DEAD).
 */

/*
 * tcp_shutdown - 实现 shutdown(SHUT_WR) 的 TCP 半关闭。
 *
 * @sk 仍有 file/socket owner，调用者持 socket lock；@how 是 SHUT_* 位集合。
 * 非发送方向请求无副作用。ESTABLISHED/SYN_SENT/CLOSE_WAIT 中首次发送关闭会用
 * 状态表转 FIN_WAIT1 或 LAST_ACK，并由 tcp_send_fin() 把 FIN 加入序列空间。
 * 与 close() 不同，本函数不 orphan socket、不清接收队列，也不置 SOCK_DEAD，
 * 因此应用仍能 recv 对端在 FIN 前发送的数据并最终观察 EOF。
 */
void tcp_shutdown(struct sock *sk, int how)
{
	/*	We need to grab some memory, and put together a FIN,
	 *	and then put it into the queue to be sent.
	 *		Tim MacKenzie(tym@dibbler.cs.monash.edu.au) 4 Dec '92.
	 */
	/* FIN 分配允许走强制内存预算，因为尽快关闭反而能释放更多连接资源。 */
	if (!(how & SEND_SHUTDOWN))
		return;

	/* If we've already sent a FIN, or it's a closed state, skip this. */
	/* 状态表之外意味着 FIN 已排队/确认或连接不具备发送方向，重复 shutdown 幂等。 */
	if ((1 << sk->sk_state) &
	    (TCPF_ESTABLISHED | TCPF_SYN_SENT |
	     TCPF_CLOSE_WAIT)) {
		/* Clear out any half completed packets.  FIN if needed. */
		/* 状态转换返回 action 位，只有首次关闭发送方向才创建 FIN。 */
		if (tcp_close_state(sk))
			tcp_send_fin(sk);
	}
}

/* 汇总所有 possible CPU 的 orphan 计数；迁移/并发近似可能短暂为负，最终钳到 0。 */
int tcp_orphan_count_sum(void)
{
	int i, total = 0;

	for_each_possible_cpu(i)
		total += per_cpu(tcp_orphan_count, i);

	return max(total, 0);
}

static int tcp_orphan_cache;
static struct timer_list tcp_orphan_timer;
#define TCP_ORPHAN_TIMER_PERIOD msecs_to_jiffies(100)

/* 每 100ms 把昂贵的 per-CPU 汇总缓存成热路径可 READ_ONCE 的全局近似值。 */
static void tcp_orphan_update(struct timer_list *unused)
{
	WRITE_ONCE(tcp_orphan_cache, tcp_orphan_count_sum());
	mod_timer(&tcp_orphan_timer, jiffies + TCP_ORPHAN_TIMER_PERIOD);
}

/* shift 用于在更高压力场景收紧阈值；缓存允许至多一个 timer 周期的策略滞后。 */
static bool tcp_too_many_orphans(int shift)
{
	return READ_ONCE(tcp_orphan_cache) << shift >
		READ_ONCE(sysctl_tcp_max_orphans);
}

/* 只有单 socket 已超过最小 sndbuf 且协议总内存越过 high 水位，才判真实内存压力。 */
static bool tcp_out_of_memory(const struct sock *sk)
{
	if (sk->sk_wmem_queued > SOCK_MIN_SNDBUF &&
	    sk_memory_allocated(sk) > sk_prot_mem_limits(sk, 2))
		return true;
	return false;
}

/*
 * tcp_check_oom - 综合 orphan 数量和协议内存决定无用户连接是否应被牺牲。
 * 返回 true 供 close/timer 发送 reset 或销毁；日志限速，避免资源灾难再被 printk
 * 放大。该结果是资源策略快照，不是分配器的确定失败证明。
 */
bool tcp_check_oom(const struct sock *sk, int shift)
{
	bool too_many_orphans, out_of_socket_memory;

	too_many_orphans = tcp_too_many_orphans(shift);
	out_of_socket_memory = tcp_out_of_memory(sk);

	if (too_many_orphans)
		net_info_ratelimited("too many orphaned sockets\n");
	if (out_of_socket_memory)
		net_info_ratelimited("out of memory -- consider tuning tcp_mem\n");
	return too_many_orphans || out_of_socket_memory;
}

/*
 * __tcp_close - 在最后一个文件引用关闭时决定优雅 FIN、RST 或立即销毁。
 *
 * @sk 已由 tcp_close() 通过 lock_sock 独占；@timeout 是 linger 等待时长。函数先
 * 关闭用户两方向并清空 receive queue：若其中仍有 copied_seq 之后的未读数据，
 * 应用已经主动丢弃可靠字节流，必须发 RST 而不能用 FIN 假装所有数据被接收。
 * SO_LINGER=0 同样要求 abort；其余连接按状态表排 FIN，并可等待状态推进。
 *
 * 后半段把 sk 从 file owner 转成 orphan：sock_hold 保住协议引用，sock_orphan
 * 断开用户等待队列，BH 锁下排空 backlog，之后由 ACK/FIN/重传/keepalive 定时器
 * 完成销毁。FIN_WAIT2 和内存压力有独立上限，防止无应用 socket 永久占资源。
 * 返回时不保证 struct sock 已释放；只保证用户引用退出，剩余生命周期归协议。
 */
void __tcp_close(struct sock *sk, long timeout)
{
	bool data_was_unread = false;
	struct sk_buff *skb;
	int state;

	WRITE_ONCE(sk->sk_shutdown, SHUTDOWN_MASK);

	if (sk->sk_state == TCP_LISTEN) {
		tcp_set_state(sk, TCP_CLOSE);

		/* Special case. */
		/* listener 没有字节流 FIN，关闭含义是停止接收新 request/child。 */
		inet_csk_listen_stop(sk);

		goto adjudge_to_death;
	}

	/*  We need to flush the recv. buffs.  We do this only on the
	 *  descriptor close, not protocol-sourced closes, because the
	 *  reader process may not have drained the data yet!
	 */
	/* 只有 fd close 表示应用放弃未读数据；协议侧关闭仍需保留队列供 reader 取完。 */
	while ((skb = skb_peek(&sk->sk_receive_queue)) != NULL) {
		u32 end_seq = TCP_SKB_CB(skb)->end_seq;

		if (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN)
			end_seq--;
		/* FIN 不算未读 payload；任何 copied_seq 之后的数据都会把 close 变成 abort。 */
		if (after(end_seq, tcp_sk(sk)->copied_seq))
			data_was_unread = true;
		tcp_eat_recv_skb(sk, skb);
	}

	/* If socket has been already reset (e.g. in tcp_reset()) - kill it. */
	/* 状态机已提交 CLOSE 时无需再构造 FIN/RST，直接进入 orphan/析构裁决。 */
	if (sk->sk_state == TCP_CLOSE)
		goto adjudge_to_death;

	/* As outlined in RFC 2525, section 2.17, we send a RST here because
	 * data was lost. To witness the awful effects of the old behavior of
	 * always doing a FIN, run an older 2.1.x kernel or 2.0.x, start a bulk
	 * GET in an FTP client, suspend the process, wait for the client to
	 * advertise a zero window, then kill -9 the FTP client, wheee...
	 * Note: timeout is always zero in such a case.
	 */
	/* 丢弃未读数据意味着无法再兑现可靠有序交付，RST 才能明确通知对端数据作废。 */
	if (unlikely(tcp_sk(sk)->repair)) {
		sk->sk_prot->disconnect(sk, 0);
	} else if (data_was_unread) {
		/* Unread data was tossed, zap the connection. */
		/* 先置 CLOSE 再发 active reset，阻止后续路径继续按正常连接处理。 */
		NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPABORTONCLOSE);
		tcp_set_state(sk, TCP_CLOSE);
		tcp_send_active_reset(sk, sk->sk_allocation,
				      SK_RST_REASON_TCP_ABORT_ON_CLOSE);
	} else if (sock_flag(sk, SOCK_LINGER) && !sk->sk_lingertime) {
		/* Check zero linger _after_ checking for unread data. */
		/* 未读数据的 abort 原因优先；否则 SO_LINGER=0 明确要求立即中止。 */
		sk->sk_prot->disconnect(sk, 0);
		NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPABORTONDATA);
	} else if (tcp_close_state(sk)) {
		/* We FIN if the application ate all the data before
		 * zapping the connection.
		 */
		/* 只有应用消费完接收数据，FIN 才能表达不丢数据的有序半关闭。 */

		/* RED-PEN. Formally speaking, we have broken TCP state
		 * machine. State transitions:
		 *
		 * TCP_ESTABLISHED -> TCP_FIN_WAIT1
		 * TCP_SYN_RECV	-> TCP_FIN_WAIT1 (it is difficult)
		 * TCP_CLOSE_WAIT -> TCP_LAST_ACK
		 *
		 * are legal only when FIN has been sent (i.e. in window),
		 * rather than queued out of window. Purists blame.
		 *
		 * F.e. "RFC state" is ESTABLISHED,
		 * if Linux state is FIN-WAIT-1, but FIN is still not sent.
		 *
		 * The visible declinations are that sometimes
		 * we enter time-wait state, when it is not required really
		 * (harmless), do not send active resets, when they are
		 * required by specs (TCP_ESTABLISHED, TCP_CLOSE_WAIT, when
		 * they look as CLOSING or LAST_ACK for Linux)
		 * Probably, I missed some more holelets.
		 * 						--ANK
		 * XXX (TFO) - To start off we don't support SYN+ACK+FIN
		 * in a single packet! (May consider it later but will
		 * probably need API support or TCP_CORK SYN-ACK until
		 * data is written and socket is closed.)
		 */
		/* Linux 在 FIN 仍因窗口排队时就提前显示 FIN_WAIT1/LAST_ACK；简化实现但会使
		 * 少数 reset/TIME_WAIT 决策与严格 RFC 瞬时状态不同，后续分支显式补偿。
		 */
		tcp_send_fin(sk);
	}

	sk_stream_wait_close(sk, timeout);

adjudge_to_death:
	state = sk->sk_state;
	/* file 引用即将消失，先取得协议侧引用，再断开 socket/waitqueue 的用户关系。 */
	sock_hold(sk);
	sock_orphan(sk);

	local_bh_disable();
	bh_lock_sock(sk);
	/* remove backlog if any, without releasing ownership. */
	/* orphan 前把延迟收包串行处理完，但仍保持用户 owner，避免 close 与 backlog 竞态。 */
	__release_sock(sk);

	tcp_orphan_count_inc();

	/* Have we already been destroyed by a softirq or backlog? */
	/* backlog 可能处理 RST/最终 ACK 并关闭 socket，必须以最新状态重新裁决。 */
	if (state != TCP_CLOSE && sk->sk_state == TCP_CLOSE)
		goto out;

	/*	This is a (useful) BSD violating of the RFC. There is a
	 *	problem with TCP as specified in that the other end could
	 *	keep a socket open forever with no application left this end.
	 *	We use a 1 minute timeout (about the same as BSD) then kill
	 *	our end. If they send after that then tough - BUT: long enough
	 *	that we won't make the old 4*rto = almost no time - whoops
	 *	reset mistake.
	 *
	 *	Nope, it was not mistake. It is really desired behaviour
	 *	f.e. on http servers, when such sockets are useless, but
	 *	consume significant resources. Let's do it with special
	 *	linger2	option.					--ANK
	 */
	/* orphan 的 FIN_WAIT2 无应用负责超时，linger2 给它有限存活期，避免对端永久占资源。 */

	if (sk->sk_state == TCP_FIN_WAIT2) {
		struct tcp_sock *tp = tcp_sk(sk);
		if (READ_ONCE(tp->linger2) < 0) {
			tcp_set_state(sk, TCP_CLOSE);
			tcp_send_active_reset(sk, GFP_ATOMIC,
					      SK_RST_REASON_TCP_ABORT_ON_LINGER);
			__NET_INC_STATS(sock_net(sk),
					LINUX_MIB_TCPABORTONLINGER);
		} else {
			const int tmo = tcp_fin_time(sk);

			if (tmo > TCP_TIMEWAIT_LEN) {
				tcp_reset_keepalive_timer(sk,
						tmo - TCP_TIMEWAIT_LEN);
			} else {
				tcp_time_wait(sk, TCP_FIN_WAIT2, tmo);
				goto out;
			}
		}
	}
	if (sk->sk_state != TCP_CLOSE) {
		if (tcp_check_oom(sk, 0)) {
			tcp_set_state(sk, TCP_CLOSE);
			tcp_send_active_reset(sk, GFP_ATOMIC,
					      SK_RST_REASON_TCP_ABORT_ON_MEMORY);
			__NET_INC_STATS(sock_net(sk),
					LINUX_MIB_TCPABORTONMEMORY);
		} else if (!check_net(sock_net(sk))) {
			/* Not possible to send reset; just close */
			/* netns 已退出，发送路径不可用，只能本地摘除状态。 */
			tcp_set_state(sk, TCP_CLOSE);
		}
	}

	if (sk->sk_state == TCP_CLOSE) {
		struct request_sock *req;

		req = rcu_dereference_protected(tcp_sk(sk)->fastopen_rsk,
						lockdep_sock_is_held(sk));
		/* We could get here with a non-NULL req if the socket is
		 * aborted (e.g., closed with unread data) before 3WHS
		 * finishes.
		 */
		/* 三次握手未完就 abort 时仍要解除 child/request 的 Fast Open 互引。 */
		if (req)
			reqsk_fastopen_remove(sk, req, false);
		inet_csk_destroy_sock(sk);
	}
	/* Otherwise, socket is reprieved until protocol close. */
	/* 非 CLOSE orphan 继续由 FIN/ACK/timer 持有，不能在 close syscall 返回时销毁。 */

out:
	bh_unlock_sock(sk);
	local_bh_enable();
}

/*
 * tcp_close - struct proto 的 close 入口，包住 TCP 关闭状态机和最终引用释放。
 *
 * @sk 是 file 最后关闭时仍有效的 socket；@timeout 来自 linger 策略。函数在进程
 * 上下文取得可睡眠 socket ownership，__tcp_close 可能等待并把对象 orphan；
 * release_sock 处理并发 backlog。无网络 namespace 引用时同步取消计时器，最后
 * sock_put 交还调用者的 sk 引用。无返回值，关闭错误不能再经已消失 fd 报告。
 */
void tcp_close(struct sock *sk, long timeout)
{
	lock_sock(sk);
	__tcp_close(sk, timeout);
	release_sock(sk);
	if (!sk->sk_net_refcnt)
		inet_csk_clear_xmit_timers_sync(sk);
	sock_put(sk);
}
EXPORT_SYMBOL(tcp_close);

/* These states need RST on ABORT according to RFC793 */
/* 这些状态仍代表可见连接/半连接，中止时需用 RST 明确通知对端。 */

static inline bool tcp_need_reset(int state)
{
	return (1 << state) &
	       (TCPF_ESTABLISHED | TCPF_CLOSE_WAIT | TCPF_FIN_WAIT1 |
		TCPF_FIN_WAIT2 | TCPF_SYN_RECV);
}

/*
 * tcp_rtx_queue_purge - 释放整棵未确认/重传 RB-tree 及 socket 发送内存。
 *
 * 调用者正在销毁/重置连接，故无需逐节点维护 tcp_tsorted_anchor 链：整个索引随后
 * 一并失效。遍历前保存 rb_next，再 unlink/free 当前 skb；返回后树空且 highest_sack
 * 清除。逻辑 packets_out 等计数由外层 tcp_write_queue_purge 统一归零。
 */
static void tcp_rtx_queue_purge(struct sock *sk)
{
	struct rb_node *p = rb_first(&sk->tcp_rtx_queue);

	tcp_sk(sk)->highest_sack = NULL;
	while (p) {
		struct sk_buff *skb = rb_to_skb(p);

		p = rb_next(p);
		/* Since we are deleting whole queue, no need to
		 * list_del(&skb->tcp_tsorted_anchor)
		 */
		/* 整条时间链马上重建，逐节点 list_del 只增加成本且不改善可观察一致性。 */
		tcp_rtx_queue_unlink(skb, sk);
		tcp_wmem_free_skb(sk, skb);
	}
}

/*
 * tcp_write_queue_purge - 同时清空未发送 write queue 与已发送重传树。
 *
 * 每个 skb 经 tcp_wmem_free_skb 撤销协议/发送记账；停止 busy chrono，重建时间排序
 * 空链，清所有缓存 hint、packets_out 和 RTO backoff。用于 disconnect/析构，不保留
 * 任何可重发 payload；调用者必须已决定是否先发 RST。
 */
void tcp_write_queue_purge(struct sock *sk)
{
	struct sk_buff *skb;

	tcp_chrono_stop(sk, TCP_CHRONO_BUSY);
	while ((skb = __skb_dequeue(&sk->sk_write_queue)) != NULL) {
		tcp_skb_tsorted_anchor_cleanup(skb);
		tcp_wmem_free_skb(sk, skb);
	}
	tcp_rtx_queue_purge(sk);
	INIT_LIST_HEAD(&tcp_sk(sk)->tsorted_sent_queue);
	tcp_clear_all_retrans_hints(tcp_sk(sk));
	tcp_sk(sk)->packets_out = 0;
	inet_csk(sk)->icsk_backoff = 0;
}

/*
 * tcp_disconnect - 中止当前连接并把同一 socket 恢复为可再次 connect 的初始状态。
 *
 * @sk 由用户上下文持 lock_sock；@flags 当前仅供协议接口一致性。根据旧状态决定
 * 停 listener、记录 repair abort 或发送 RST；随后清全部 timer、接收/乱序/发送
 * 队列、route、地址绑定、ACK/RTT/拥塞/SACK/ECN/统计和 Fast Open 状态。返回 0
 * 表示本地重置完成，不代表 RST 被对端收到。
 *
 * 新 write_seq 取旧 write_seq + max_window + 2（零则改 1），避免同一四元组快速
 * 重连时新序列空间与残留旧报文重叠。释放拥塞算法私有状态后清 ca_priv，再回 Open；
 * xchg sk_rx_dst 同时从无锁读者视图摘除旧 route，并对取得的引用 dst_release。
 */
int tcp_disconnect(struct sock *sk, int flags)
{
	struct inet_sock *inet = inet_sk(sk);
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	int old_state = sk->sk_state;
	struct request_sock *req;
	u32 seq;

	if (old_state != TCP_CLOSE)
		tcp_set_state(sk, TCP_CLOSE);

	/* ABORT function of RFC793 */
	/* 根据旧状态选择 listener stop、RST 或仅本地错误，之后统一清空全部状态。 */
	if (old_state == TCP_LISTEN) {
		inet_csk_listen_stop(sk);
	} else if (unlikely(tp->repair)) {
		WRITE_ONCE(sk->sk_err, ECONNABORTED);
	} else if (tcp_need_reset(old_state)) {
		tcp_send_active_reset(sk, gfp_any(), SK_RST_REASON_TCP_STATE);
		WRITE_ONCE(sk->sk_err, ECONNRESET);
	} else if (tp->snd_nxt != tp->write_seq &&
		   (1 << old_state) & (TCPF_CLOSING | TCPF_LAST_ACK)) {
		/* The last check adjusts for discrepancy of Linux wrt. RFC
		 * states
		 */
		/* Linux 可能已进入关闭状态但 FIN 尚未实际发送，仍有尾数据时必须 RST。 */
		tcp_send_active_reset(sk, gfp_any(),
				      SK_RST_REASON_TCP_DISCONNECT_WITH_DATA);
		WRITE_ONCE(sk->sk_err, ECONNRESET);
	} else if (old_state == TCP_SYN_SENT)
		WRITE_ONCE(sk->sk_err, ECONNRESET);

	tcp_clear_xmit_timers(sk);
	__skb_queue_purge(&sk->sk_receive_queue);
	WRITE_ONCE(tp->copied_seq, tp->rcv_nxt);
	WRITE_ONCE(tp->urg_data, 0);
	sk_set_peek_off(sk, -1);
	tcp_write_queue_purge(sk);
	tcp_fastopen_active_disable_ofo_check(sk);
	skb_rbtree_purge(&tp->out_of_order_queue);

	inet->inet_dport = 0;

	inet_bhash2_reset_saddr(sk);

	WRITE_ONCE(sk->sk_shutdown, 0);
	sock_reset_flag(sk, SOCK_DONE);
	tp->srtt_us = 0;
	tp->mdev_us = jiffies_to_usecs(TCP_TIMEOUT_INIT);
	tp->rcv_rtt_last_tsecr = 0;

	seq = tp->write_seq + tp->max_window + 2;
	if (!seq)
		seq = 1;
	WRITE_ONCE(tp->write_seq, seq);

	icsk->icsk_backoff = 0;
	WRITE_ONCE(icsk->icsk_probes_out, 0);
	icsk->icsk_probes_tstamp = 0;
	icsk->icsk_rto = TCP_TIMEOUT_INIT;
	WRITE_ONCE(icsk->icsk_rto_min, TCP_RTO_MIN);
	WRITE_ONCE(icsk->icsk_delack_max, TCP_DELACK_MAX);
	WRITE_ONCE(tp->snd_ssthresh, TCP_INFINITE_SSTHRESH);
	tcp_snd_cwnd_set(tp, TCP_INIT_CWND);
	tp->snd_cwnd_cnt = 0;
	tp->is_cwnd_limited = 0;
	tp->max_packets_out = 0;
	tp->window_clamp = 0;
	tp->delivered = 0;
	tp->delivered_ce = 0;
	tp->accecn_fail_mode = 0;
	tp->saw_accecn_opt = TCP_ACCECN_OPT_NOT_SEEN;
	tcp_accecn_init_counters(tp);
	tp->prev_ecnfield = 0;
	tp->accecn_opt_tstamp = 0;
	tp->pkts_acked_ewma = 0;
	if (icsk->icsk_ca_initialized && icsk->icsk_ca_ops->release)
		icsk->icsk_ca_ops->release(sk);
	memset(icsk->icsk_ca_priv, 0, sizeof(icsk->icsk_ca_priv));
	icsk->icsk_ca_initialized = 0;
	tcp_set_ca_state(sk, TCP_CA_Open);
	tp->is_sack_reneg = 0;
	tcp_clear_retrans(tp);
	tp->total_retrans = 0;
	inet_csk_delack_init(sk);
	/* Initialize rcv_mss to TCP_MIN_MSS to avoid division by 0
	 * issue in __tcp_select_window()
	 */
	/* 清空 rx_opt 后仍给窗口算法非零分母，直到新连接重新测得 rcv_mss。 */
	icsk->icsk_ack.rcv_mss = TCP_MIN_MSS;
	memset(&tp->rx_opt, 0, sizeof(tp->rx_opt));
	__sk_dst_reset(sk);
	dst_release(unrcu_pointer(xchg(&sk->sk_rx_dst, NULL)));
	tcp_saved_syn_free(tp);
	tp->compressed_ack = 0;
	tp->segs_in = 0;
	tp->segs_out = 0;
	tp->bytes_sent = 0;
	tp->bytes_acked = 0;
	tp->bytes_received = 0;
	tp->bytes_retrans = 0;
	tp->data_segs_in = 0;
	tp->data_segs_out = 0;
	tp->duplicate_sack[0].start_seq = 0;
	tp->duplicate_sack[0].end_seq = 0;
	tp->dsack_dups = 0;
	tp->reord_seen = 0;
	tp->retrans_out = 0;
	tp->sacked_out = 0;
	tp->tlp_high_seq = 0;
	tp->last_oow_ack_time = 0;
	tp->plb_rehash = 0;
	/* There's a bubble in the pipe until at least the first ACK. */
	/* 重连初始 delivery sample 再次标 application-limited 哨兵。 */
	tp->app_limited = ~0U;
	tp->rate_app_limited = 1;
	tp->rack.mstamp = 0;
	tp->rack.advanced = 0;
	tp->rack.reo_wnd_steps = 1;
	tp->rack.last_delivered = 0;
	tp->rack.reo_wnd_persist = 0;
	tp->rack.dsack_seen = 0;
	tp->syn_data_acked = 0;
	tp->syn_fastopen_child = 0;
	tp->rx_opt.saw_tstamp = 0;
	tp->rx_opt.dsack = 0;
	tp->rx_opt.num_sacks = 0;
	tp->rcv_ooopack = 0;


	/* Clean up fastopen related fields */
	/* request 与客户端请求分别持有不同资源，均需释放并清 DEFER_CONNECT。 */
	req = rcu_dereference_protected(tp->fastopen_rsk,
					lockdep_sock_is_held(sk));
	if (req)
		reqsk_fastopen_remove(sk, req, false);
	tcp_free_fastopen_req(tp);
	inet_clear_bit(DEFER_CONNECT, sk);
	tp->fastopen_client_fail = 0;

	WARN_ON(inet->inet_num && !icsk->icsk_bind_hash);

	if (sk->sk_frag.page) {
		put_page(sk->sk_frag.page);
		sk->sk_frag.page = NULL;
		sk->sk_frag.offset = 0;
	}
	sk_error_report(sk);
	return 0;
}

/* TCP_REPAIR 可直接伪造队列/序号，只允许当前 netns 的 CAP_NET_ADMIN，且禁止 listener。 */
static inline bool tcp_can_repair_sock(const struct sock *sk)
{
	return sockopt_ns_capable(sock_net(sk)->user_ns, CAP_NET_ADMIN) &&
		(sk->sk_state != TCP_LISTEN);
}

/*
 * tcp_repair_set_window - 从 checkpoint 数据恢复发送/接收窗口游标。
 *
 * 仅 repair 模式且长度精确匹配才读取用户结构。先验证 max_window>=snd_wnd、snd_wl1
 * 未越过当前接收窗口右界、rcv_wup 未越过 rcv_nxt，再一次性写入所有字段并重建
 * rcv_mwnd_seq。验证完成前不修改 tp，因此 EFAULT/EINVAL/EPERM 均保持原状态。
 */
static int tcp_repair_set_window(struct tcp_sock *tp, sockptr_t optbuf, int len)
{
	struct tcp_repair_window opt;

	if (!tp->repair)
		return -EPERM;

	if (len != sizeof(opt))
		return -EINVAL;

	if (copy_from_sockptr(&opt, optbuf, sizeof(opt)))
		return -EFAULT;

	if (opt.max_window < opt.snd_wnd)
		return -EINVAL;

	if (after(opt.snd_wl1, tp->rcv_nxt + opt.rcv_wnd))
		return -EINVAL;

	if (after(opt.rcv_wup, tp->rcv_nxt))
		return -EINVAL;

	tp->snd_wl1	= opt.snd_wl1;
	tp->snd_wnd	= opt.snd_wnd;
	tp->max_window	= opt.max_window;

	tp->rcv_wnd	= opt.rcv_wnd;
	tp->rcv_wup	= opt.rcv_wup;
	tp->rcv_mwnd_seq = opt.rcv_wup + opt.rcv_wnd;

	return 0;
}

/*
 * tcp_repair_options_est - 为尚未发送数据的恢复连接重建握手协商结果。
 *
 * optbuf 是 tcp_repair_opt 数组，逐项恢复 MSS、window scale、SACK permitted 和
 * timestamp；未知 opt_code 被忽略以保持扩展兼容。window scale 超协议上限、无需
 * value 的选项却带非零值时返回错误。前面已成功的选项不会回滚，因此调用者应把
 * 输入视为有序配置事务，并只在 repair+ESTABLISHED+bytes_sent==0 时调用。
 */
static int tcp_repair_options_est(struct sock *sk, sockptr_t optbuf,
		unsigned int len)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct tcp_repair_opt opt;
	size_t offset = 0;

	while (len >= sizeof(opt)) {
		if (copy_from_sockptr_offset(&opt, optbuf, offset, sizeof(opt)))
			return -EFAULT;

		offset += sizeof(opt);
		len -= sizeof(opt);

		switch (opt.opt_code) {
		case TCPOPT_MSS:
			tp->rx_opt.mss_clamp = opt.opt_val;
			tcp_mtup_init(sk);
			break;
		case TCPOPT_WINDOW:
			{
				u16 snd_wscale = opt.opt_val & 0xFFFF;
				u16 rcv_wscale = opt.opt_val >> 16;

				if (snd_wscale > TCP_MAX_WSCALE || rcv_wscale > TCP_MAX_WSCALE)
					return -EFBIG;

				tp->rx_opt.snd_wscale = snd_wscale;
				tp->rx_opt.rcv_wscale = rcv_wscale;
				tp->rx_opt.wscale_ok = 1;
			}
			break;
		case TCPOPT_SACK_PERM:
			if (opt.opt_val != 0)
				return -EINVAL;

			tp->rx_opt.sack_ok |= TCP_SACK_SEEN;
			break;
		case TCPOPT_TIMESTAMP:
			if (opt.opt_val != 0)
				return -EINVAL;

			tp->rx_opt.tstamp_ok = 1;
			break;
		}
	}

	return 0;
}

DEFINE_STATIC_KEY_FALSE(tcp_tx_delay_enabled);

/*
 * tcp_enable_tx_delay - 启用每 socket 发送延迟模型，并校正存量 RTT 派生状态。
 *
 * 全局 static key 只在首次非零配置时开启，使默认关闭的发送热路径几乎无成本。
 * 活连接修改 delay 时，srtt_us 内部按 1/8 微秒保存，故 delta 左移 3；随后重算
 * RTO、清最小 RTT 样本并更新 pacing。校正是 best effort，已有 RTO backoff 不被
 * 完整逆推。调用者最后才写 tp->tcp_tx_delay。
 */
static void tcp_enable_tx_delay(struct sock *sk, int val)
{
	struct tcp_sock *tp = tcp_sk(sk);
	s32 delta = (val - tp->tcp_tx_delay) << 3;

	if (val && !static_branch_unlikely(&tcp_tx_delay_enabled)) {
		static int __tcp_tx_delay_enabled = 0;

		if (cmpxchg(&__tcp_tx_delay_enabled, 0, 1) == 0) {
			static_branch_enable(&tcp_tx_delay_enabled);
			pr_info("TCP_TX_DELAY enabled\n");
		}
	}
	/* If we change tcp_tx_delay on a live flow, adjust tp->srtt_us,
	 * tp->rtt_min, icsk_rto and sk->sk_pacing_rate.
	 * This is best effort.
	 */
	/* delay 属于本地调度而非网络 RTT，修改时补偿估计，避免 RTO/pacing 突跳。 */
	if (delta && sk->sk_state == TCP_ESTABLISHED) {
		s64 srtt = (s64)tp->srtt_us + delta;

		WRITE_ONCE(tp->srtt_us,
			   clamp_t(s64, srtt, 1, ~0U));

		/* Note: does not deal with non zero icsk_backoff */
		/* 已处于超时退避的 RTO 仍是 best-effort，不能精确反演历史 backoff。 */
		tcp_set_rto(sk);

		minmax_reset(&tp->rtt_min, tcp_jiffies32, ~0U);

		tcp_update_pacing_rate(sk);
	}
}

/* When set indicates to always queue non-full frames.  Later the user clears
 * this option and we transmit any pending partial frames in the queue.  This is
 * meant to be used alongside sendfile() to get properly filled frames when the
 * user (for example) must write out headers with a write() call first and then
 * use sendfile to send out the data parts.
 *
 * TCP_CORK can be set together with TCP_NODELAY and it is stronger than
 * TCP_NODELAY.
 */
/*
 * __tcp_sock_set_cork - 在已持 socket lock 时切换“尽量填满再发”的持久策略。
 *
 * 开启只置 CORK；关闭时若 NODELAY 同时存在，置一次性 PUSH，并立即推动队列。
 * 因而 CORK 在持续期间强于 NODELAY，但解除 CORK 不会忘记用户先前的 NODELAY。
 */
void __tcp_sock_set_cork(struct sock *sk, bool on)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (on) {
		tp->nonagle |= TCP_NAGLE_CORK;
	} else {
		tp->nonagle &= ~TCP_NAGLE_CORK;
		if (tp->nonagle & TCP_NAGLE_OFF)
			tp->nonagle |= TCP_NAGLE_PUSH;
		tcp_push_pending_frames(sk);
	}
}

/* 为内核调用者提供取得/释放可睡眠 socket lock 的 CORK 包装。 */
void tcp_sock_set_cork(struct sock *sk, bool on)
{
	lock_sock(sk);
	__tcp_sock_set_cork(sk, on);
	release_sock(sk);
}
EXPORT_SYMBOL(tcp_sock_set_cork);

/* TCP_NODELAY is weaker than TCP_CORK, so that this option on corked socket is
 * remembered, but it is not activated until cork is cleared.
 *
 * However, when TCP_NODELAY is set we make an explicit push, which overrides
 * even TCP_CORK for currently queued segments.
 */
/*
 * __tcp_sock_set_nodelay - 切换 Nagle 禁用位；开启时额外 PUSH 当前已排队数据。
 * CORK 仍控制后续小 skb 聚合，但显式设置 NODELAY 的这一刻会覆盖 CORK 推一次，
 * 避免应用以为设置成功却让已有命令/请求头继续滞留。
 */
void __tcp_sock_set_nodelay(struct sock *sk, bool on)
{
	if (on) {
		tcp_sk(sk)->nonagle |= TCP_NAGLE_OFF|TCP_NAGLE_PUSH;
		tcp_push_pending_frames(sk);
	} else {
		tcp_sk(sk)->nonagle &= ~TCP_NAGLE_OFF;
	}
}

/* 内核便捷接口：持锁开启 NODELAY；关闭能力由通用 setsockopt 传 false 完成。 */
void tcp_sock_set_nodelay(struct sock *sk)
{
	lock_sock(sk);
	__tcp_sock_set_nodelay(sk, true);
	release_sock(sk);
}
EXPORT_SYMBOL(tcp_sock_set_nodelay);

/*
 * __tcp_sock_set_quickack - 调整 delayed-ACK 的 ping-pong/quickack 模式。
 *
 * val=0 进入交互流 ping-pong 模式，允许延迟 ACK；非零退出并在已建立且 ACK pending
 * 时标 PUSHED，通过 cleanup_rbuf 立即重新评估发送。偶数非零值在完成一次 quick
 * ACK 后重新进入 ping-pong，是内部一次性语义。它不是永久“每包立即 ACK”保证。
 */
static void __tcp_sock_set_quickack(struct sock *sk, int val)
{
	if (!val) {
		inet_csk_enter_pingpong_mode(sk);
		return;
	}

	inet_csk_exit_pingpong_mode(sk);
	if ((1 << sk->sk_state) & (TCPF_ESTABLISHED | TCPF_CLOSE_WAIT) &&
	    inet_csk_ack_scheduled(sk)) {
		inet_csk(sk)->icsk_ack.pending |= ICSK_ACK_PUSHED;
		tcp_cleanup_rbuf(sk, 1);
		if (!(val & 1))
			inet_csk_enter_pingpong_mode(sk);
	}
}

/* 取得 socket lock 后设置 quickack，避免与接收 ACK 状态机并发修改 pending 位。 */
void tcp_sock_set_quickack(struct sock *sk, int val)
{
	lock_sock(sk);
	__tcp_sock_set_quickack(sk, val);
	release_sock(sk);
}
EXPORT_SYMBOL(tcp_sock_set_quickack);

/* 设置主动 SYN 最大重试次数；范围外 EINVAL，WRITE_ONCE 与 timer 路径无锁读取配对。 */
int tcp_sock_set_syncnt(struct sock *sk, int val)
{
	if (val < 1 || val > MAX_TCP_SYNCNT)
		return -EINVAL;

	WRITE_ONCE(inet_csk(sk)->icsk_syn_retries, val);
	return 0;
}
EXPORT_SYMBOL(tcp_sock_set_syncnt);

/*
 * 设置 TCP_USER_TIMEOUT（毫秒）：限制数据无确认或零窗口 probe 无进展的总时长。
 * 0 恢复协议默认；它不等同 keepalive，也不限制正常空闲连接。
 */
int tcp_sock_set_user_timeout(struct sock *sk, int val)
{
	/* Cap the max time in ms TCP will retry or probe the window
	 * before giving up and aborting (ETIMEDOUT) a connection.
	 */
	/* USER_TIMEOUT 只约束“已发送但无交付进展”，最终由重传/probe timer 判 ETIMEDOUT。 */
	if (val < 0)
		return -EINVAL;

	WRITE_ONCE(inet_csk(sk)->icsk_user_timeout, val);
	return 0;
}
EXPORT_SYMBOL(tcp_sock_set_user_timeout);

/*
 * tcp_sock_set_keepidle_locked - 设置首次 keepalive probe 前的空闲秒数并重装活跃 timer。
 *
 * 调用者持锁。若 SO_KEEPALIVE 已启用且状态有效，按已经空闲的 elapsed 计算剩余
 * 时间；新阈值已过去则以 0 尽快触发。参数错误不修改字段/定时器。
 */
int tcp_sock_set_keepidle_locked(struct sock *sk, int val)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (val < 1 || val > MAX_TCP_KEEPIDLE)
		return -EINVAL;

	/* Paired with WRITE_ONCE() in keepalive_time_when() */
	/* 与无锁读取配对，避免编译器把并发更新拆分或重复读取。 */
	WRITE_ONCE(tp->keepalive_time, val * HZ);
	if (sock_flag(sk, SOCK_KEEPOPEN) &&
	    !((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_LISTEN))) {
		u32 elapsed = keepalive_time_elapsed(tp);

		if (tp->keepalive_time > elapsed)
			elapsed = tp->keepalive_time - elapsed;
		else
			elapsed = 0;
		tcp_reset_keepalive_timer(sk, elapsed);
	}

	return 0;
}

/* 取得 socket lock 的 keepidle 包装，返回 locked helper 的 0 或 EINVAL。 */
int tcp_sock_set_keepidle(struct sock *sk, int val)
{
	int err;

	lock_sock(sk);
	err = tcp_sock_set_keepidle_locked(sk, val);
	release_sock(sk);
	return err;
}
EXPORT_SYMBOL(tcp_sock_set_keepidle);

/* 设置 keepalive probe 之间的秒数；只更新策略，现有 timer 在下一次处理时采用。 */
int tcp_sock_set_keepintvl(struct sock *sk, int val)
{
	if (val < 1 || val > MAX_TCP_KEEPINTVL)
		return -EINVAL;

	WRITE_ONCE(tcp_sk(sk)->keepalive_intvl, val * HZ);
	return 0;
}
EXPORT_SYMBOL(tcp_sock_set_keepintvl);

/* 设置判定 keepalive 失败前允许的 probe 次数，与 timer 路径 READ_ONCE 配对。 */
int tcp_sock_set_keepcnt(struct sock *sk, int val)
{
	if (val < 1 || val > MAX_TCP_KEEPCNT)
		return -EINVAL;

	/* Paired with READ_ONCE() in keepalive_probes() */
	/* 探测定时器可在另一 CPU 无锁读取，因此用单次原子可见的字段发布。 */
	WRITE_ONCE(tcp_sk(sk)->keepalive_probes, val);
	return 0;
}
EXPORT_SYMBOL(tcp_sock_set_keepcnt);

/*
 * tcp_set_window_clamp - 限制本端最大公告接收窗口并同步 autotuning 阈值。
 *
 * val=0 只允许未连接 socket，用于恢复自动默认。非零至少为 SOCK_MIN_RCVBUF/2。
 * 缩小时 __tcp_adjust_rcv_ssthresh 同步 reserved-memory provisioning；扩大时只提高
 * rcv_ssthresh 到当前 rcv_wnd/新 clamp 的合理值，不能凭配置声称尚无 buffer 的空间。
 */
int tcp_set_window_clamp(struct sock *sk, int val)
{
	u32 old_window_clamp, new_window_clamp, new_rcv_ssthresh;
	struct tcp_sock *tp = tcp_sk(sk);

	if (!val) {
		if (sk->sk_state != TCP_CLOSE)
			return -EINVAL;
		WRITE_ONCE(tp->window_clamp, 0);
		return 0;
	}

	old_window_clamp = tp->window_clamp;
	new_window_clamp = max_t(int, SOCK_MIN_RCVBUF / 2, val);

	if (new_window_clamp == old_window_clamp)
		return 0;

	WRITE_ONCE(tp->window_clamp, new_window_clamp);

	/* Need to apply the reserved mem provisioning only
	 * when shrinking the window clamp.
	 */
	/* 缩窗可能使既有 reserved 预算过大，需同步降低；扩窗不要求立即预留全部内存。 */
	if (new_window_clamp < old_window_clamp) {
		__tcp_adjust_rcv_ssthresh(sk, new_window_clamp);
	} else {
		new_rcv_ssthresh = min(tp->rcv_wnd, new_window_clamp);
		tp->rcv_ssthresh = max(new_rcv_ssthresh, tp->rcv_ssthresh);
	}
	return 0;
}

/* 设置用户 MSS clamp；0 表示自动，最终仍受 route MTU 限制，设置时可能尚未知出口。 */
int tcp_sock_set_maxseg(struct sock *sk, int val)
{
	/* Values greater than interface MTU won't take effect. However
	 * at the point when this call is done we typically don't yet
	 * know which interface is going to be used
	 */
	/* 这里只保存用户上限；route 确定后再与 PMTU/option 开销取更小值。 */
	if (val && (val < TCP_MIN_MSS || val > MAX_TCP_WINDOW))
		return -EINVAL;

	WRITE_ONCE(tcp_sk(sk)->rx_opt.user_mss, val);
	return 0;
}

/*
 *	Socket option code for TCP.
 */
/*
 * do_tcp_setsockopt - 解析并应用 SOL_TCP 选项的核心分派。
 *
 * @optval 是用户或内核 sockptr，必须使用 copy_from_sockptr 系列访问；@optlen 是
 * 字节数。字符串/结构选项先独立处理，普通 int 统一复制一次。只含单字段且 timer
 * 使用 READ_ONCE 的选项可无 socket lock 更新；会改变队列、状态机、拥塞算法、
 * ACK 或发送行为的选项必须包在 sockopt_lock_sock 内。返回 0 或精确 errno。
 *
 * 这不是全有或全无事务：单值选项均先验证再写，但 repair option 数组、ULP/认证
 * 等子系统可能已有自己的部分更新语义。TCP_REPAIR 尤其能直接修改序列空间，只对
 * CAP_NET_ADMIN 开放；关闭 repair 默认发送 window probe，让恢复后的对端重新同步。
 */
int do_tcp_setsockopt(struct sock *sk, int level, int optname,
		      sockptr_t optval, unsigned int optlen)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct net *net = sock_net(sk);
	int val;
	int err = 0;

	/* 阶段 1：变长字符串/密钥不能按 int 读取，分别验证长度并复制到有界内核缓冲。 */
	/* These are data/string values, all the others are ints */
	/* 先分流非整数 ABI；否则统一 copy 一个 int 会截断字符串或密钥。 */
	switch (optname) {
	case TCP_CONGESTION: {
		char name[TCP_CA_NAME_MAX];

		if (optlen < 1)
			return -EINVAL;

		val = strncpy_from_sockptr(name, optval,
					min_t(long, TCP_CA_NAME_MAX-1, optlen));
		if (val < 0)
			return -EFAULT;
		name[val] = 0;

		sockopt_lock_sock(sk);
		err = tcp_set_congestion_control(sk, name, !has_current_bpf_ctx(),
						 sockopt_ns_capable(sock_net(sk)->user_ns,
								    CAP_NET_ADMIN));
		sockopt_release_sock(sk);
		return err;
	}
	case TCP_ULP: {
		char name[TCP_ULP_NAME_MAX];

		if (optlen < 1)
			return -EINVAL;

		val = strncpy_from_sockptr(name, optval,
					min_t(long, TCP_ULP_NAME_MAX - 1,
					      optlen));
		if (val < 0)
			return -EFAULT;
		name[val] = 0;

		sockopt_lock_sock(sk);
		err = tcp_set_ulp(sk, name);
		sockopt_release_sock(sk);
		return err;
	}
	case TCP_FASTOPEN_KEY: {
		__u8 key[TCP_FASTOPEN_KEY_BUF_LENGTH];
		__u8 *backup_key = NULL;

		/* Allow a backup key as well to facilitate key rotation
		 * First key is the active one.
		 */
		/* 同时接受 active+backup，使轮换期间旧 cookie 仍可验证而新 cookie 用首 key。 */
		if (optlen != TCP_FASTOPEN_KEY_LENGTH &&
		    optlen != TCP_FASTOPEN_KEY_BUF_LENGTH)
			return -EINVAL;

		if (copy_from_sockptr(key, optval, optlen))
			return -EFAULT;

		if (optlen == TCP_FASTOPEN_KEY_BUF_LENGTH)
			backup_key = key + TCP_FASTOPEN_KEY_LENGTH;

		return tcp_fastopen_reset_cipher(net, sk, key, backup_key);
	}
	default:
		/* fallthru */
		/* 非变长选项落入下面的整数解析与分派。 */
		break;
	}

	if (optlen < sizeof(int))
		return -EINVAL;

	if (copy_from_sockptr(&val, optval, sizeof(val)))
		return -EFAULT;

	/* 阶段 2：这些选项以单字段 WRITE_ONCE 发布，接收者容忍配置在任意时刻变化。 */
	/* Handle options that can be set without locking the socket. */
	/* 这些 setter 自己用 READ/WRITE_ONCE 维护单字段并发语义，可省去 socket 锁。 */
	switch (optname) {
	case TCP_SYNCNT:
		return tcp_sock_set_syncnt(sk, val);
	case TCP_USER_TIMEOUT:
		return tcp_sock_set_user_timeout(sk, val);
	case TCP_KEEPINTVL:
		return tcp_sock_set_keepintvl(sk, val);
	case TCP_KEEPCNT:
		return tcp_sock_set_keepcnt(sk, val);
	case TCP_LINGER2:
		if (val < 0)
			WRITE_ONCE(tp->linger2, -1);
		else if (val > TCP_FIN_TIMEOUT_MAX / HZ)
			WRITE_ONCE(tp->linger2, TCP_FIN_TIMEOUT_MAX);
		else
			WRITE_ONCE(tp->linger2, val * HZ);
		return 0;
	case TCP_DEFER_ACCEPT:
		/* Translate value in seconds to number of retransmits */
		/* request timer 按重传轮次运行，故把用户秒数映射为退避模型的次数。 */
		WRITE_ONCE(icsk->icsk_accept_queue.rskq_defer_accept,
			   secs_to_retrans(val, TCP_TIMEOUT_INIT / HZ,
					   TCP_RTO_MAX / HZ));
		return 0;
	case TCP_RTO_MAX_MS:
		if (val < MSEC_PER_SEC || val > TCP_RTO_MAX_SEC * MSEC_PER_SEC)
			return -EINVAL;
		WRITE_ONCE(inet_csk(sk)->icsk_rto_max, msecs_to_jiffies(val));
		return 0;
	case TCP_RTO_MIN_US: {
		int rto_min = usecs_to_jiffies(val);

		if (rto_min > TCP_RTO_MIN || rto_min < TCP_TIMEOUT_MIN)
			return -EINVAL;
		WRITE_ONCE(inet_csk(sk)->icsk_rto_min, rto_min);
		return 0;
	}
	case TCP_DELACK_MAX_US: {
		int delack_max = usecs_to_jiffies(val);

		if (delack_max > TCP_DELACK_MAX || delack_max < TCP_TIMEOUT_MIN)
			return -EINVAL;
		WRITE_ONCE(inet_csk(sk)->icsk_delack_max, delack_max);
		return 0;
	}
	case TCP_MAXSEG:
		return tcp_sock_set_maxseg(sk, val);
	}

	/* 阶段 3：以下分支会联合修改队列/状态/定时器，必须与 TCP 数据面串行。 */
	sockopt_lock_sock(sk);

	switch (optname) {
	case TCP_NODELAY:
		__tcp_sock_set_nodelay(sk, val);
		break;

	case TCP_THIN_LINEAR_TIMEOUTS:
		if (val < 0 || val > 1)
			err = -EINVAL;
		else
			tp->thin_lto = val;
		break;

	case TCP_THIN_DUPACK:
		if (val < 0 || val > 1)
			err = -EINVAL;
		break;

	case TCP_REPAIR:
		/* ON 冻结正常协议语义并允许复用地址；OFF 恢复后主动探测对端窗口。 */
		if (!tcp_can_repair_sock(sk))
			err = -EPERM;
		else if (val == TCP_REPAIR_ON) {
			tp->repair = 1;
			sk->sk_reuse = SK_FORCE_REUSE;
			tp->repair_queue = TCP_NO_QUEUE;
		} else if (val == TCP_REPAIR_OFF) {
			tp->repair = 0;
			sk->sk_reuse = SK_NO_REUSE;
			tcp_send_window_probe(sk);
		} else if (val == TCP_REPAIR_OFF_NO_WP) {
			tp->repair = 0;
			sk->sk_reuse = SK_NO_REUSE;
		} else
			err = -EINVAL;

		break;

	case TCP_REPAIR_QUEUE:
		if (!tp->repair)
			err = -EPERM;
		else if ((unsigned int)val < TCP_QUEUES_NR)
			tp->repair_queue = val;
		else
			err = -EINVAL;
		break;

	case TCP_QUEUE_SEQ:
		/* 只允许 CLOSED 且目标队列为空，防止新游标与既有 skb 序列范围冲突。 */
		if (sk->sk_state != TCP_CLOSE) {
			err = -EPERM;
		} else if (tp->repair_queue == TCP_SEND_QUEUE) {
			if (!tcp_rtx_queue_empty(sk))
				err = -EPERM;
			else
				WRITE_ONCE(tp->write_seq, val);
		} else if (tp->repair_queue == TCP_RECV_QUEUE) {
			if (tp->rcv_nxt != tp->copied_seq) {
				err = -EPERM;
			} else {
				WRITE_ONCE(tp->rcv_nxt, val);
				WRITE_ONCE(tp->copied_seq, val);
			}
		} else {
			err = -EINVAL;
		}
		break;

	case TCP_REPAIR_OPTIONS:
		if (!tp->repair)
			err = -EINVAL;
		else if (sk->sk_state == TCP_ESTABLISHED && !tp->bytes_sent)
			err = tcp_repair_options_est(sk, optval, optlen);
		else
			err = -EPERM;
		break;

	case TCP_CORK:
		__tcp_sock_set_cork(sk, val);
		break;

	case TCP_KEEPIDLE:
		err = tcp_sock_set_keepidle_locked(sk, val);
		break;
	case TCP_SAVE_SYN:
		/* 0: disable, 1: enable, 2: start from ether_header */
		/* 模式 1 保存 L3+L4，模式 2 连 L2 一并保存；只影响后续新 SYN。 */
		if (val < 0 || val > 2)
			err = -EINVAL;
		else
			tp->save_syn = val;
		break;

	case TCP_WINDOW_CLAMP:
		err = tcp_set_window_clamp(sk, val);
		break;

	case TCP_QUICKACK:
		__tcp_sock_set_quickack(sk, val);
		break;

	case TCP_AO_REPAIR:
		if (!tcp_can_repair_sock(sk)) {
			err = -EPERM;
			break;
		}
		err = tcp_ao_set_repair(sk, optval, optlen);
		break;
#ifdef CONFIG_TCP_AO
	case TCP_AO_ADD_KEY:
	case TCP_AO_DEL_KEY:
	case TCP_AO_INFO: {
		/* If this is the first TCP-AO setsockopt() on the socket,
		 * sk_state has to be LISTEN or CLOSE. Allow TCP_REPAIR
		 * in any state.
		 */
		/* 首次建立 AO 状态会改变认证不变量，已连接 socket 仅在已有 AO 或 repair 时允许。 */
		if ((1 << sk->sk_state) & (TCPF_LISTEN | TCPF_CLOSE))
			goto ao_parse;
		if (rcu_dereference_protected(tcp_sk(sk)->ao_info,
					      lockdep_sock_is_held(sk)))
			goto ao_parse;
		if (tp->repair)
			goto ao_parse;
		err = -EISCONN;
		break;
ao_parse:
		err = tp->af_specific->ao_parse(sk, optname, optval, optlen);
		break;
	}
#endif
#ifdef CONFIG_TCP_MD5SIG
	case TCP_MD5SIG:
	case TCP_MD5SIG_EXT:
		err = tp->af_specific->md5_parse(sk, optname, optval, optlen);
		break;
#endif
	case TCP_FASTOPEN:
		if (val >= 0 && ((1 << sk->sk_state) & (TCPF_CLOSE |
		    TCPF_LISTEN))) {
			tcp_fastopen_init_key_once(net);

			fastopen_queue_tune(sk, val);
		} else {
			err = -EINVAL;
		}
		break;
	case TCP_FASTOPEN_CONNECT:
		if (val > 1 || val < 0) {
			err = -EINVAL;
		} else if (READ_ONCE(net->ipv4.sysctl_tcp_fastopen) &
			   TFO_CLIENT_ENABLE) {
			if (sk->sk_state == TCP_CLOSE)
				tp->fastopen_connect = val;
			else
				err = -EINVAL;
		} else {
			err = -EOPNOTSUPP;
		}
		break;
	case TCP_FASTOPEN_NO_COOKIE:
		if (val > 1 || val < 0)
			err = -EINVAL;
		else if (!((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_LISTEN)))
			err = -EINVAL;
		else
			tp->fastopen_no_cookie = val;
		break;
	case TCP_TIMESTAMP:
		if (!tp->repair) {
			err = -EPERM;
			break;
		}
		/* val is an opaque field,
		 * and low order bit contains usec_ts enable bit.
		 * Its a best effort, and we do not care if user makes an error.
		 */
		/* checkpoint 保存的是带本地 clock offset 的不透明时间域，仅低位显式编码精度。 */
		tp->tcp_usec_ts = val & 1;
		WRITE_ONCE(tp->tsoffset, val - tcp_clock_ts(tp->tcp_usec_ts));
		break;
	case TCP_REPAIR_WINDOW:
		err = tcp_repair_set_window(tp, optval, optlen);
		break;
	case TCP_NOTSENT_LOWAT:
		WRITE_ONCE(tp->notsent_lowat, val);
		/* 阈值变化本身可能已使 socket 可写，立即重评估并通知 epoll waiter。 */
		READ_ONCE(sk->sk_write_space)(sk);
		break;
	case TCP_INQ:
		if (val > 1 || val < 0)
			err = -EINVAL;
		else
			tp->recvmsg_inq = val;
		break;
	case TCP_TX_DELAY:
		/* tp->srtt_us is u32, and is shifted by 3 */
		/* 限制 val 确保换算成内部 1/8 微秒后不溢出有符号校正范围。 */
		if (val < 0 || val >= (1U << (31 - 3))) {
			err = -EINVAL;
			break;
		}
		tcp_enable_tx_delay(sk, val);
		WRITE_ONCE(tp->tcp_tx_delay, val);
		break;
	default:
		err = -ENOPROTOOPT;
		break;
	}

	sockopt_release_sock(sk);
	return err;
}

/*
 * tcp_setsockopt - TCP proto 入口；非 SOL_TCP 选项转交当前地址族实现。
 *
 * af_ops 可在 IPv6/映射连接配置期间替换，READ_ONCE 保证函数表指针只取一次；
 * SOL_TCP 进入通用实现。参数 ownership 全部借用，返回值直接传给 syscall 层。
 */
int tcp_setsockopt(struct sock *sk, int level, int optname, sockptr_t optval,
		   unsigned int optlen)
{
	const struct inet_connection_sock *icsk = inet_csk(sk);

	if (level != SOL_TCP)
		/* Paired with WRITE_ONCE() in do_ipv6_setsockopt() and tcp_v6_connect() */
		/* af_ops 可因地址族切换而更新；一次快照保证函数表指针读取一致。 */
		return READ_ONCE(icsk->icsk_af_ops)->setsockopt(sk, level, optname,
								optval, optlen);
	return do_tcp_setsockopt(sk, level, optname, optval, optlen);
}

/*
 * tcp_get_info_chrono_stats - 汇总 busy/rwnd-limited/sndbuf-limited 持续微秒数。
 *
 * 当前 chrono 尚未结算时把 now-start 临时加入对应桶。函数可能无 socket lock，
 * READ_ONCE 只能给 best-effort 诊断快照；为了热路径不加昂贵同步，三个桶与总和
 * 可能跨越一次状态切换，但不会影响协议行为。
 */
static void tcp_get_info_chrono_stats(const struct tcp_sock *tp,
				      struct tcp_info *info)
{
	u64 stats[__TCP_CHRONO_MAX], total = 0;
	enum tcp_chrono i, cur;

	/* Following READ_ONCE()s pair with WRITE_ONCE()s in tcp_chrono_set().
	 * This is because socket lock might not be owned by us at this point.
	 * This is best effort, tcp_get_timestamping_opt_stats() can
	 * see wrong values. A real fix would be too costly for TCP fast path.
	 */
	/* 允许桶切换时出现轻微跨时刻误差，换取发送热路径无需诊断专用锁。 */
	cur = READ_ONCE(tp->chrono_type);
	for (i = TCP_CHRONO_BUSY; i < __TCP_CHRONO_MAX; ++i) {
		stats[i] = READ_ONCE(tp->chrono_stat[i - 1]);
		if (i == cur)
			stats[i] += tcp_jiffies32 - READ_ONCE(tp->chrono_start);
		stats[i] *= USEC_PER_SEC / HZ;
		total += stats[i];
	}

	info->tcpi_busy_time = total;
	info->tcpi_rwnd_limited = stats[TCP_CHRONO_RWND_LIMITED];
	info->tcpi_sndbuf_limited = stats[TCP_CHRONO_SNDBUF_LIMITED];
}

/* Return information about state of tcp endpoint in API format. */
/*
 * tcp_get_info - 把内部 TCP 状态转换为稳定 struct tcp_info UAPI 快照。
 *
 * @info 先清零，非 SOCK_STREAM 直接返回空结构。state、pacing 和 listener backlog
 * 可无锁读取；full connection 用 lock_sock_fast 在无竞争时取得轻量锁，再转换
 * RTO/RTT 单位、队列计数、窗口、速率、ECN 和累计统计。listener 复用 unacked/
 * sacked 字段表示 ready/max accept backlog，这是 UAPI 约定而非发送 ACK 含义。
 * 函数无错误返回，允许相邻无锁字段来自略有不同的时刻，适合诊断而非控制决策。
 */
void tcp_get_info(struct sock *sk, struct tcp_info *info)
{
	const struct tcp_sock *tp = tcp_sk(sk); /* iff sk_type == SOCK_STREAM */
	/* tcp_sk 只是基于嵌入布局的转换，故读取任何 tcp_sock 字段前先验证 SOCK_STREAM。 */
	const struct inet_connection_sock *icsk = inet_csk(sk);
	const u8 ect1_idx = INET_ECN_ECT_1 - 1;
	const u8 ect0_idx = INET_ECN_ECT_0 - 1;
	const u8 ce_idx = INET_ECN_CE - 1;
	unsigned long rate;
	u32 now;
	u64 rate64;
	bool slow;

	memset(info, 0, sizeof(*info));
	if (sk->sk_type != SOCK_STREAM)
		return;

	info->tcpi_state = inet_sk_state_load(sk);

	/* Report meaningful fields for all TCP states, including listeners */
	/* pacing/reordering/cwnd 在 listener 也有定义，可先无锁填充通用前缀。 */
	rate = READ_ONCE(sk->sk_pacing_rate);
	rate64 = (rate != ~0UL) ? rate : ~0ULL;
	info->tcpi_pacing_rate = rate64;

	rate = READ_ONCE(sk->sk_max_pacing_rate);
	rate64 = (rate != ~0UL) ? rate : ~0ULL;
	info->tcpi_max_pacing_rate = rate64;

	info->tcpi_reordering = tp->reordering;
	info->tcpi_snd_cwnd = tcp_snd_cwnd(tp);

	if (info->tcpi_state == TCP_LISTEN) {
		/* listeners aliased fields :
		 * tcpi_unacked -> Number of children ready for accept()
		 * tcpi_sacked  -> max backlog
		 */
		/* UAPI 历史复用字段：此状态下不能按发送队列的 unacked/sacked 解读。 */
		info->tcpi_unacked = READ_ONCE(sk->sk_ack_backlog);
		info->tcpi_sacked = READ_ONCE(sk->sk_max_ack_backlog);
		return;
	}

	slow = lock_sock_fast(sk);

	info->tcpi_ca_state = icsk->icsk_ca_state;
	info->tcpi_retransmits = icsk->icsk_retransmits;
	info->tcpi_probes = icsk->icsk_probes_out;
	info->tcpi_backoff = icsk->icsk_backoff;

	if (tp->rx_opt.tstamp_ok)
		info->tcpi_options |= TCPI_OPT_TIMESTAMPS;
	if (tcp_is_sack(tp))
		info->tcpi_options |= TCPI_OPT_SACK;
	if (tp->rx_opt.wscale_ok) {
		info->tcpi_options |= TCPI_OPT_WSCALE;
		info->tcpi_snd_wscale = tp->rx_opt.snd_wscale;
		info->tcpi_rcv_wscale = tp->rx_opt.rcv_wscale;
	}

	if (tcp_ecn_mode_any(tp))
		info->tcpi_options |= TCPI_OPT_ECN;
	if (tp->ecn_flags & TCP_ECN_SEEN)
		info->tcpi_options |= TCPI_OPT_ECN_SEEN;
	if (tp->syn_data_acked)
		info->tcpi_options |= TCPI_OPT_SYN_DATA;
	if (tp->tcp_usec_ts)
		info->tcpi_options |= TCPI_OPT_USEC_TS;
	if (tp->syn_fastopen_child)
		info->tcpi_options |= TCPI_OPT_TFO_CHILD;

	info->tcpi_rto = jiffies_to_usecs(icsk->icsk_rto);
	info->tcpi_ato = jiffies_to_usecs(min_t(u32, icsk->icsk_ack.ato,
						tcp_delack_max(sk)));
	info->tcpi_snd_mss = tp->mss_cache;
	info->tcpi_rcv_mss = icsk->icsk_ack.rcv_mss;

	info->tcpi_unacked = tp->packets_out;
	info->tcpi_sacked = tp->sacked_out;

	info->tcpi_lost = tp->lost_out;
	info->tcpi_retrans = tp->retrans_out;

	now = tcp_jiffies32;
	info->tcpi_last_data_sent = jiffies_to_msecs(now - tp->lsndtime);
	info->tcpi_last_data_recv = jiffies_to_msecs(now - icsk->icsk_ack.lrcvtime);
	info->tcpi_last_ack_recv = jiffies_to_msecs(now - tp->rcv_tstamp);

	info->tcpi_pmtu = icsk->icsk_pmtu_cookie;
	info->tcpi_rcv_ssthresh = tp->rcv_ssthresh;
	info->tcpi_rtt = tp->srtt_us >> 3;
	info->tcpi_rttvar = tp->mdev_us >> 2;
	info->tcpi_snd_ssthresh = tp->snd_ssthresh;
	info->tcpi_advmss = tp->advmss;

	info->tcpi_rcv_rtt = tp->rcv_rtt_est.rtt_us >> 3;
	info->tcpi_rcv_space = tp->rcvq_space.space;

	info->tcpi_total_retrans = tp->total_retrans;

	info->tcpi_bytes_acked = tp->bytes_acked;
	info->tcpi_bytes_received = tp->bytes_received;
	info->tcpi_notsent_bytes = max_t(int, 0, tp->write_seq - tp->snd_nxt);
	tcp_get_info_chrono_stats(tp, info);

	info->tcpi_segs_out = tp->segs_out;

	/* segs_in and data_segs_in can be updated from tcp_segs_in() from BH */
	/* 即便持用户锁，BH 统计仍可能更新，单次读取防编译器撕裂但快照允许变化。 */
	info->tcpi_segs_in = READ_ONCE(tp->segs_in);
	info->tcpi_data_segs_in = READ_ONCE(tp->data_segs_in);

	info->tcpi_min_rtt = tcp_min_rtt(tp);
	info->tcpi_data_segs_out = tp->data_segs_out;

	info->tcpi_delivery_rate_app_limited = tp->rate_app_limited ? 1 : 0;
	rate64 = tcp_compute_delivery_rate(tp);
	if (rate64)
		info->tcpi_delivery_rate = rate64;
	info->tcpi_delivered = tp->delivered;
	info->tcpi_delivered_ce = tp->delivered_ce;
	info->tcpi_bytes_sent = tp->bytes_sent;
	info->tcpi_bytes_retrans = tp->bytes_retrans;
	info->tcpi_dsack_dups = tp->dsack_dups;
	info->tcpi_reord_seen = tp->reord_seen;
	info->tcpi_rcv_ooopack = tp->rcv_ooopack;
	info->tcpi_snd_wnd = tp->snd_wnd;
	info->tcpi_rcv_wnd = tp->rcv_wnd;
	info->tcpi_rehash = tp->plb_rehash + tp->timeout_rehash;
	info->tcpi_fastopen_client_fail = tp->fastopen_client_fail;

	info->tcpi_total_rto = tp->total_rto;
	info->tcpi_total_rto_recoveries = tp->total_rto_recoveries;
	info->tcpi_total_rto_time = tp->total_rto_time;
	if (tp->rto_stamp)
		info->tcpi_total_rto_time += tcp_clock_ms() - tp->rto_stamp;

	if (tcp_ecn_disabled(tp))
		info->tcpi_ecn_mode = TCPI_ECN_MODE_DISABLED;
	else if (tcp_ecn_mode_rfc3168(tp))
		info->tcpi_ecn_mode = TCPI_ECN_MODE_RFC3168;
	else if (tcp_ecn_mode_accecn(tp))
		info->tcpi_ecn_mode = TCPI_ECN_MODE_ACCECN;
	else if (tcp_ecn_mode_pending(tp))
		info->tcpi_ecn_mode = TCPI_ECN_MODE_PENDING;
	info->tcpi_accecn_fail_mode = tp->accecn_fail_mode;
	info->tcpi_accecn_opt_seen = tp->saw_accecn_opt;
	info->tcpi_received_ce = tp->received_ce;
	info->tcpi_delivered_e1_bytes = tp->delivered_ecn_bytes[ect1_idx];
	info->tcpi_delivered_e0_bytes = tp->delivered_ecn_bytes[ect0_idx];
	info->tcpi_delivered_ce_bytes = tp->delivered_ecn_bytes[ce_idx];
	info->tcpi_received_e1_bytes = tp->received_ecn_bytes[ect1_idx];
	info->tcpi_received_e0_bytes = tp->received_ecn_bytes[ect0_idx];
	info->tcpi_received_ce_bytes = tp->received_ecn_bytes[ce_idx];

	unlock_sock_fast(sk, slow);
}
EXPORT_SYMBOL_GPL(tcp_get_info);

/* 精确累计 timestamping 可选统计的 netlink attribute 对齐空间，供一次性 alloc_skb。 */
static size_t tcp_opt_stats_get_size(void)
{
	return
		nla_total_size_64bit(sizeof(u64)) + /* TCP_NLA_BUSY */
		nla_total_size_64bit(sizeof(u64)) + /* TCP_NLA_RWND_LIMITED */
		nla_total_size_64bit(sizeof(u64)) + /* TCP_NLA_SNDBUF_LIMITED */
		nla_total_size_64bit(sizeof(u64)) + /* TCP_NLA_DATA_SEGS_OUT */
		nla_total_size_64bit(sizeof(u64)) + /* TCP_NLA_TOTAL_RETRANS */
		nla_total_size_64bit(sizeof(u64)) + /* TCP_NLA_PACING_RATE */
		nla_total_size_64bit(sizeof(u64)) + /* TCP_NLA_DELIVERY_RATE */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_SND_CWND */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_REORDERING */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_MIN_RTT */
		nla_total_size(sizeof(u8)) + /* TCP_NLA_RECUR_RETRANS */
		nla_total_size(sizeof(u8)) + /* TCP_NLA_DELIVERY_RATE_APP_LMT */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_SNDQ_SIZE */
		nla_total_size(sizeof(u8)) + /* TCP_NLA_CA_STATE */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_SND_SSTHRESH */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_DELIVERED */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_DELIVERED_CE */
		nla_total_size_64bit(sizeof(u64)) + /* TCP_NLA_BYTES_SENT */
		nla_total_size_64bit(sizeof(u64)) + /* TCP_NLA_BYTES_RETRANS */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_DSACK_DUPS */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_REORD_SEEN */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_SRTT */
		nla_total_size(sizeof(u16)) + /* TCP_NLA_TIMEOUT_REHASH */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_BYTES_NOTSENT */
		nla_total_size_64bit(sizeof(u64)) + /* TCP_NLA_EDT */
		nla_total_size(sizeof(u8)) + /* TCP_NLA_TTL */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_REHASH */
		0;
}

/* Returns TTL or hop limit of an incoming packet from skb. */
/* 按 skb->protocol 选择 IPv4 TTL 或 IPv6 hop_limit；未知 L3 返回 0 作为“无样本”。 */
static u8 tcp_skb_ttl_or_hop_limit(const struct sk_buff *skb)
{
	if (skb->protocol == htons(ETH_P_IP))
		return ip_hdr(skb)->ttl;
	else if (skb->protocol == htons(ETH_P_IPV6))
		return ipv6_hdr(skb)->hop_limit;
	else
		return 0;
}

/*
 * tcp_get_timestamping_opt_stats - 为发送时间戳 completion 构造 TCP 统计 netlink skb。
 *
 * @orig_skb 提供 EDT，@ack_skb 可选地提供反向 TTL；三者均只借用。GFP_ATOMIC
 * 分配失败返回 NULL，不影响时间戳主体。成功返回由调用者拥有的 stats skb，内部
 * 以预计算容量追加 NLA；字段多为 READ_ONCE/data_race 的近似快照，避免 ACK 热路径
 * 取得 socket lock。该统计用于观测，不应被当成同一时刻的控制事务。
 */
struct sk_buff *tcp_get_timestamping_opt_stats(const struct sock *sk,
					       const struct sk_buff *orig_skb,
					       const struct sk_buff *ack_skb)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *stats;
	struct tcp_info info;
	unsigned long rate;
	u64 rate64;

	stats = alloc_skb(tcp_opt_stats_get_size(), GFP_ATOMIC);
	if (!stats)
		return NULL;

	tcp_get_info_chrono_stats(tp, &info);
	nla_put_u64_64bit(stats, TCP_NLA_BUSY,
			  info.tcpi_busy_time, TCP_NLA_PAD);
	nla_put_u64_64bit(stats, TCP_NLA_RWND_LIMITED,
			  info.tcpi_rwnd_limited, TCP_NLA_PAD);
	nla_put_u64_64bit(stats, TCP_NLA_SNDBUF_LIMITED,
			  info.tcpi_sndbuf_limited, TCP_NLA_PAD);
	nla_put_u64_64bit(stats, TCP_NLA_DATA_SEGS_OUT,
			  READ_ONCE(tp->data_segs_out), TCP_NLA_PAD);
	nla_put_u64_64bit(stats, TCP_NLA_TOTAL_RETRANS,
			  READ_ONCE(tp->total_retrans), TCP_NLA_PAD);

	rate = READ_ONCE(sk->sk_pacing_rate);
	rate64 = (rate != ~0UL) ? rate : ~0ULL;
	nla_put_u64_64bit(stats, TCP_NLA_PACING_RATE, rate64, TCP_NLA_PAD);

	rate64 = tcp_compute_delivery_rate(tp);
	nla_put_u64_64bit(stats, TCP_NLA_DELIVERY_RATE, rate64, TCP_NLA_PAD);

	nla_put_u32(stats, TCP_NLA_SND_CWND, READ_ONCE(tp->snd_cwnd));
	nla_put_u32(stats, TCP_NLA_REORDERING, READ_ONCE(tp->reordering));
	nla_put_u32(stats, TCP_NLA_MIN_RTT, data_race(tcp_min_rtt(tp)));

	nla_put_u8(stats, TCP_NLA_RECUR_RETRANS,
		   READ_ONCE(inet_csk(sk)->icsk_retransmits));
	nla_put_u8(stats, TCP_NLA_DELIVERY_RATE_APP_LMT, data_race(!!tp->rate_app_limited));
	nla_put_u32(stats, TCP_NLA_SND_SSTHRESH, READ_ONCE(tp->snd_ssthresh));
	nla_put_u32(stats, TCP_NLA_DELIVERED, READ_ONCE(tp->delivered));
	nla_put_u32(stats, TCP_NLA_DELIVERED_CE, READ_ONCE(tp->delivered_ce));

	nla_put_u32(stats, TCP_NLA_SNDQ_SIZE,
		    max_t(int, 0,
			  READ_ONCE(tp->write_seq) - READ_ONCE(tp->snd_una)));
	nla_put_u8(stats, TCP_NLA_CA_STATE, inet_csk(sk)->icsk_ca_state);

	nla_put_u64_64bit(stats, TCP_NLA_BYTES_SENT, READ_ONCE(tp->bytes_sent),
			  TCP_NLA_PAD);
	nla_put_u64_64bit(stats, TCP_NLA_BYTES_RETRANS,
			  READ_ONCE(tp->bytes_retrans), TCP_NLA_PAD);
	nla_put_u32(stats, TCP_NLA_DSACK_DUPS, READ_ONCE(tp->dsack_dups));
	nla_put_u32(stats, TCP_NLA_REORD_SEEN, READ_ONCE(tp->reord_seen));
	nla_put_u32(stats, TCP_NLA_SRTT, READ_ONCE(tp->srtt_us) >> 3);
	nla_put_u16(stats, TCP_NLA_TIMEOUT_REHASH,
		    READ_ONCE(tp->timeout_rehash));
	nla_put_u32(stats, TCP_NLA_BYTES_NOTSENT,
		    max_t(int, 0,
			  READ_ONCE(tp->write_seq) - READ_ONCE(tp->snd_nxt)));
	nla_put_u64_64bit(stats, TCP_NLA_EDT, orig_skb->skb_mstamp_ns,
			  TCP_NLA_PAD);
	if (ack_skb)
		nla_put_u8(stats, TCP_NLA_TTL,
			   tcp_skb_ttl_or_hop_limit(ack_skb));

	nla_put_u32(stats, TCP_NLA_REHASH,
		    READ_ONCE(tp->plb_rehash) + READ_ONCE(tp->timeout_rehash));
	return stats;
}

/*
 * do_tcp_getsockopt - 把内部 TCP 配置/状态复制为 SOL_TCP UAPI。
 *
 * @optlen 是输入输出 sockptr：先读取用户容量，成功时写实际/所需长度；@optval 为
 * 输出。普通整数最多复制 sizeof(int)；TCP_INFO、CC_INFO、密钥、saved SYN、repair
 * 和 zerocopy 使用各自结构长度。返回 0 或 EFAULT/EINVAL/EPERM/ENOPROTOOPT。
 *
 * 多数值是诊断快照，可无锁 READ_ONCE；会消费对象或修改接收状态的 SAVED_SYN、
 * TCP_ZEROCOPY_RECEIVE、AO 查询显式取得 sockopt lock。zerocopy 是特殊“get”操作：
 * 它映射/复制数据并推进 copied_seq，随后按调用者结构版本选择可写回的字段，保持
 * UAPI 向后兼容。SAVED_SYN 成功复制后释放保存对象，因而也不是幂等查询。
 */
int do_tcp_getsockopt(struct sock *sk, int level,
		      int optname, sockptr_t optval, sockptr_t optlen)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	struct net *net = sock_net(sk);
	int user_mss;
	int val, len;

	if (copy_from_sockptr(&len, optlen, sizeof(int)))
		return -EFAULT;

	if (len < 0)
		return -EINVAL;

	len = min_t(unsigned int, len, sizeof(int));

	switch (optname) {
	case TCP_MAXSEG:
		val = tp->mss_cache;
		user_mss = READ_ONCE(tp->rx_opt.user_mss);
		if (user_mss &&
		    ((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_LISTEN)))
			val = user_mss;
		if (tp->repair)
			val = tp->rx_opt.mss_clamp;
		break;
	case TCP_NODELAY:
		val = !!(tp->nonagle&TCP_NAGLE_OFF);
		break;
	case TCP_CORK:
		val = !!(tp->nonagle&TCP_NAGLE_CORK);
		break;
	case TCP_KEEPIDLE:
		val = keepalive_time_when(tp) / HZ;
		break;
	case TCP_KEEPINTVL:
		val = keepalive_intvl_when(tp) / HZ;
		break;
	case TCP_KEEPCNT:
		val = keepalive_probes(tp);
		break;
	case TCP_SYNCNT:
		val = READ_ONCE(icsk->icsk_syn_retries) ? :
			READ_ONCE(net->ipv4.sysctl_tcp_syn_retries);
		break;
	case TCP_LINGER2:
		val = READ_ONCE(tp->linger2);
		if (val >= 0)
			val = (val ? : READ_ONCE(net->ipv4.sysctl_tcp_fin_timeout)) / HZ;
		break;
	case TCP_DEFER_ACCEPT:
		val = READ_ONCE(icsk->icsk_accept_queue.rskq_defer_accept);
		val = retrans_to_secs(val, TCP_TIMEOUT_INIT / HZ,
				      TCP_RTO_MAX / HZ);
		break;
	case TCP_WINDOW_CLAMP:
		val = READ_ONCE(tp->window_clamp);
		break;
	case TCP_INFO: {
		struct tcp_info info;

		if (copy_from_sockptr(&len, optlen, sizeof(int)))
			return -EFAULT;

		tcp_get_info(sk, &info);

		len = min_t(unsigned int, len, sizeof(info));
		if (copy_to_sockptr(optlen, &len, sizeof(int)))
			return -EFAULT;
		if (copy_to_sockptr(optval, &info, len))
			return -EFAULT;
		return 0;
	}
	case TCP_CC_INFO: {
		const struct tcp_congestion_ops *ca_ops;
		union tcp_cc_info info;
		size_t sz = 0;
		int attr;

		if (copy_from_sockptr(&len, optlen, sizeof(int)))
			return -EFAULT;

		ca_ops = icsk->icsk_ca_ops;
		if (ca_ops && ca_ops->get_info)
			sz = ca_ops->get_info(sk, ~0U, &attr, &info);

		len = min_t(unsigned int, len, sz);
		if (copy_to_sockptr(optlen, &len, sizeof(int)))
			return -EFAULT;
		if (copy_to_sockptr(optval, &info, len))
			return -EFAULT;
		return 0;
	}
	case TCP_QUICKACK:
		val = !inet_csk_in_pingpong_mode(sk);
		break;

	case TCP_CONGESTION:
		if (copy_from_sockptr(&len, optlen, sizeof(int)))
			return -EFAULT;
		len = min_t(unsigned int, len, TCP_CA_NAME_MAX);
		if (copy_to_sockptr(optlen, &len, sizeof(int)))
			return -EFAULT;
		if (copy_to_sockptr(optval, icsk->icsk_ca_ops->name, len))
			return -EFAULT;
		return 0;

	case TCP_ULP:
		if (copy_from_sockptr(&len, optlen, sizeof(int)))
			return -EFAULT;
		len = min_t(unsigned int, len, TCP_ULP_NAME_MAX);
		if (!icsk->icsk_ulp_ops) {
			len = 0;
			if (copy_to_sockptr(optlen, &len, sizeof(int)))
				return -EFAULT;
			return 0;
		}
		if (copy_to_sockptr(optlen, &len, sizeof(int)))
			return -EFAULT;
		if (copy_to_sockptr(optval, icsk->icsk_ulp_ops->name, len))
			return -EFAULT;
		return 0;

	case TCP_FASTOPEN_KEY: {
		u64 key[TCP_FASTOPEN_KEY_BUF_LENGTH / sizeof(u64)];
		unsigned int key_len;

		if (copy_from_sockptr(&len, optlen, sizeof(int)))
			return -EFAULT;

		key_len = tcp_fastopen_get_cipher(net, icsk, key) *
				TCP_FASTOPEN_KEY_LENGTH;
		len = min_t(unsigned int, len, key_len);
		if (copy_to_sockptr(optlen, &len, sizeof(int)))
			return -EFAULT;
		if (copy_to_sockptr(optval, key, len))
			return -EFAULT;
		return 0;
	}
	case TCP_THIN_LINEAR_TIMEOUTS:
		val = tp->thin_lto;
		break;

	case TCP_THIN_DUPACK:
		val = 0;
		break;

	case TCP_REPAIR:
		val = tp->repair;
		break;

	case TCP_REPAIR_QUEUE:
		if (tp->repair)
			val = tp->repair_queue;
		else
			return -EINVAL;
		break;

	case TCP_REPAIR_WINDOW: {
		struct tcp_repair_window opt;

		if (copy_from_sockptr(&len, optlen, sizeof(int)))
			return -EFAULT;

		if (len != sizeof(opt))
			return -EINVAL;

		if (!tp->repair)
			return -EPERM;

		opt.snd_wl1	= tp->snd_wl1;
		opt.snd_wnd	= tp->snd_wnd;
		opt.max_window	= tp->max_window;
		opt.rcv_wnd	= tp->rcv_wnd;
		opt.rcv_wup	= tp->rcv_wup;

		if (copy_to_sockptr(optval, &opt, len))
			return -EFAULT;
		return 0;
	}
	case TCP_QUEUE_SEQ:
		if (tp->repair_queue == TCP_SEND_QUEUE)
			val = tp->write_seq;
		else if (tp->repair_queue == TCP_RECV_QUEUE)
			val = tp->rcv_nxt;
		else
			return -EINVAL;
		break;

	case TCP_USER_TIMEOUT:
		val = READ_ONCE(icsk->icsk_user_timeout);
		break;

	case TCP_FASTOPEN:
		val = READ_ONCE(icsk->icsk_accept_queue.fastopenq.max_qlen);
		break;

	case TCP_FASTOPEN_CONNECT:
		val = tp->fastopen_connect;
		break;

	case TCP_FASTOPEN_NO_COOKIE:
		val = tp->fastopen_no_cookie;
		break;

	case TCP_TX_DELAY:
		val = READ_ONCE(tp->tcp_tx_delay);
		break;

	case TCP_TIMESTAMP:
		val = tcp_clock_ts(tp->tcp_usec_ts) + READ_ONCE(tp->tsoffset);
		if (tp->tcp_usec_ts)
			val |= 1;
		else
			val &= ~1;
		break;
	case TCP_NOTSENT_LOWAT:
		val = READ_ONCE(tp->notsent_lowat);
		break;
	case TCP_INQ:
		val = tp->recvmsg_inq;
		break;
	case TCP_SAVE_SYN:
		val = tp->save_syn;
		break;
	case TCP_SAVED_SYN: {
		if (copy_from_sockptr(&len, optlen, sizeof(int)))
			return -EFAULT;

		sockopt_lock_sock(sk);
		if (tp->saved_syn) {
			if (len < tcp_saved_syn_len(tp->saved_syn)) {
				len = tcp_saved_syn_len(tp->saved_syn);
				if (copy_to_sockptr(optlen, &len, sizeof(int))) {
					sockopt_release_sock(sk);
					return -EFAULT;
				}
				sockopt_release_sock(sk);
				return -EINVAL;
			}
			len = tcp_saved_syn_len(tp->saved_syn);
			if (copy_to_sockptr(optlen, &len, sizeof(int))) {
				sockopt_release_sock(sk);
				return -EFAULT;
			}
			if (copy_to_sockptr(optval, tp->saved_syn->data, len)) {
				sockopt_release_sock(sk);
				return -EFAULT;
			}
			tcp_saved_syn_free(tp);
			sockopt_release_sock(sk);
		} else {
			sockopt_release_sock(sk);
			len = 0;
			if (copy_to_sockptr(optlen, &len, sizeof(int)))
				return -EFAULT;
		}
		return 0;
	}
#ifdef CONFIG_MMU
	case TCP_ZEROCOPY_RECEIVE: {
		struct scm_timestamping_internal tss;
		struct tcp_zerocopy_receive zc = {};
		int err;

		if (copy_from_sockptr(&len, optlen, sizeof(int)))
			return -EFAULT;
		if (len < 0 ||
		    len < offsetofend(struct tcp_zerocopy_receive, length))
			return -EINVAL;
		if (unlikely(len > sizeof(zc))) {
			err = check_zeroed_sockptr(optval, sizeof(zc),
						   len - sizeof(zc));
			if (err < 1)
				return err == 0 ? -EINVAL : err;
			len = sizeof(zc);
			if (copy_to_sockptr(optlen, &len, sizeof(int)))
				return -EFAULT;
		}
		if (copy_from_sockptr(&zc, optval, len))
			return -EFAULT;
		if (zc.reserved)
			return -EINVAL;
		if (zc.msg_flags &  ~(TCP_VALID_ZC_MSG_FLAGS))
			return -EINVAL;
		/* 名为 getsockopt 但会消费 receive queue，必须与普通 recv 串行。 */
		sockopt_lock_sock(sk);
		err = tcp_zerocopy_receive(sk, &zc, &tss);
		err = BPF_CGROUP_RUN_PROG_GETSOCKOPT_KERN(sk, level, optname,
							  &zc, &len, err);
		sockopt_release_sock(sk);
		if (len >= offsetofend(struct tcp_zerocopy_receive, msg_flags))
			goto zerocopy_rcv_cmsg;
		switch (len) {
		case offsetofend(struct tcp_zerocopy_receive, msg_flags):
			goto zerocopy_rcv_cmsg;
		case offsetofend(struct tcp_zerocopy_receive, msg_controllen):
		case offsetofend(struct tcp_zerocopy_receive, msg_control):
		case offsetofend(struct tcp_zerocopy_receive, flags):
		case offsetofend(struct tcp_zerocopy_receive, copybuf_len):
		case offsetofend(struct tcp_zerocopy_receive, copybuf_address):
		case offsetofend(struct tcp_zerocopy_receive, err):
			goto zerocopy_rcv_sk_err;
		case offsetofend(struct tcp_zerocopy_receive, inq):
			goto zerocopy_rcv_inq;
		case offsetofend(struct tcp_zerocopy_receive, length):
		default:
			goto zerocopy_rcv_out;
		}
zerocopy_rcv_cmsg:
		if (zc.msg_flags & TCP_CMSG_TS)
			tcp_zc_finalize_rx_tstamp(sk, &zc, &tss);
		else
			zc.msg_flags = 0;
zerocopy_rcv_sk_err:
		if (!err)
			zc.err = sock_error(sk);
zerocopy_rcv_inq:
		zc.inq = tcp_inq_hint(sk);
zerocopy_rcv_out:
		if (!err && copy_to_sockptr(optval, &zc, len))
			err = -EFAULT;
		return err;
	}
#endif
	case TCP_AO_REPAIR:
		if (!tcp_can_repair_sock(sk))
			return -EPERM;
		return tcp_ao_get_repair(sk, optval, optlen);
	case TCP_AO_GET_KEYS:
	case TCP_AO_INFO: {
		int err;

		sockopt_lock_sock(sk);
		if (optname == TCP_AO_GET_KEYS)
			err = tcp_ao_get_mkts(sk, optval, optlen);
		else
			err = tcp_ao_get_sock_info(sk, optval, optlen);
		sockopt_release_sock(sk);

		return err;
	}
	case TCP_IS_MPTCP:
		val = 0;
		break;
	case TCP_RTO_MAX_MS:
		val = jiffies_to_msecs(tcp_rto_max(sk));
		break;
	case TCP_RTO_MIN_US:
		val = jiffies_to_usecs(READ_ONCE(inet_csk(sk)->icsk_rto_min));
		break;
	case TCP_DELACK_MAX_US:
		val = jiffies_to_usecs(READ_ONCE(inet_csk(sk)->icsk_delack_max));
		break;
	default:
		return -ENOPROTOOPT;
	}

	if (copy_to_sockptr(optlen, &len, sizeof(int)))
		return -EFAULT;
	if (copy_to_sockptr(optval, &val, len))
		return -EFAULT;
	return 0;
}

/*
 * 标记 TCP_ZEROCOPY_RECEIVE 绕过通用 BPF getsockopt 包装；核心实现已在持锁临界区
 * 内显式运行 BPF_CGROUP_RUN_PROG_GETSOCKOPT_KERN，重复包装会多拿锁或执行两次。
 */
bool tcp_bpf_bypass_getsockopt(int level, int optname)
{
	/* TCP do_tcp_getsockopt has optimized getsockopt implementation
	 * to avoid extra socket lock for TCP_ZEROCOPY_RECEIVE.
	 */
	/* 零拷贝接收在 TCP 层已有专门的无额外加锁路径，BPF 包装层应让它直达。 */
	if (level == SOL_TCP && optname == TCP_ZEROCOPY_RECEIVE)
		return true;

	return false;
}

/* 非 SOL_TCP 转地址族函数表；SOL_TCP 把裸用户指针封成 USER_SOCKPTR 后进入通用实现。 */
int tcp_getsockopt(struct sock *sk, int level, int optname, char __user *optval,
		   int __user *optlen)
{
	struct inet_connection_sock *icsk = inet_csk(sk);

	if (level != SOL_TCP)
		/* Paired with WRITE_ONCE() in do_ipv6_setsockopt() and tcp_v6_connect() */
		/* 对地址族函数表只取一次快照，避免检查与间接调用看到不同指针。 */
		return READ_ONCE(icsk->icsk_af_ops)->getsockopt(sk, level, optname,
								optval, optlen);
	return do_tcp_getsockopt(sk, level, optname, USER_SOCKPTR(optval),
				 USER_SOCKPTR(optlen));
}

#ifdef CONFIG_TCP_MD5SIG
/*
 * tcp_md5_hash_skb_data - 按线上顺序把 TCP payload 的 linear、page frags、frag_list
 * 递归送入 MD5。@header_len 跳过当前 skb 的 TCP header；高端页用 kmap_local_page
 * 临时映射并立即 kunmap，不能跨循环保存 vaddr。函数只读 skb，不改变引用。
 */
void tcp_md5_hash_skb_data(struct md5_ctx *ctx, const struct sk_buff *skb,
			   unsigned int header_len)
{
	const unsigned int head_data_len = skb_headlen(skb) > header_len ?
					   skb_headlen(skb) - header_len : 0;
	const struct skb_shared_info *shi = skb_shinfo(skb);
	struct sk_buff *frag_iter;
	unsigned int i;

	md5_update(ctx, (const u8 *)tcp_hdr(skb) + header_len, head_data_len);

	for (i = 0; i < shi->nr_frags; ++i) {
		const skb_frag_t *f = &shi->frags[i];
		u32 p_off, p_len, copied;
		const void *vaddr;
		struct page *p;

		skb_frag_foreach_page(f, skb_frag_off(f), skb_frag_size(f),
				      p, p_off, p_len, copied) {
			vaddr = kmap_local_page(p);
			md5_update(ctx, vaddr + p_off, p_len);
			kunmap_local(vaddr);
		}
	}

	skb_walk_frags(skb, frag_iter)
		tcp_md5_hash_skb_data(ctx, frag_iter, 0);
}

/*
 * tcp_md5_hash_key - 把一个可由配置侧并发替换的 key 加入当前 MD5 上下文。
 * keylen 用 READ_ONCE 取得；key bytes 用 data_race 明示允许非原子快照。配置更新
 * 竞态最多使当前报文认证失败，避免为每包 hash 获取管理锁拖慢热路径。
 */
void tcp_md5_hash_key(struct md5_ctx *ctx,
		      const struct tcp_md5sig_key *key)
{
	u8 keylen = READ_ONCE(key->keylen); /* paired with WRITE_ONCE() in tcp_md5_do_add */
	/* 先稳定取得长度；更新端以 WRITE_ONCE 发布它。 */

	/* We use data_race() because tcp_md5_do_add() might change
	 * key->key under us
	 */
	/* 这里允许与密钥替换并发：data_race 标明这是审计过的有意竞争，而非漏锁。 */
	data_race(({ md5_update(ctx, key->key, keylen), 0; }));
}

/* Called with rcu_read_lock() */
/*
 * tcp_inbound_md5_hash - 查找匹配 peer/VRF 的 key 并常量时间比较报文签名。
 *
 * 调用者持 RCU，@hash_location 指向 skb TCP option。无预期 key 和摘要不匹配使用
 * 不同 drop reason/统计；IPv4 与地址族 hook 构造各自 pseudo-header。crypto_memneq
 * 避免普通 memcmp 的早停时序泄露。函数只验证，不消费 skb。
 */
static enum skb_drop_reason
tcp_inbound_md5_hash(const struct sock *sk, const struct sk_buff *skb,
		     const void *saddr, const void *daddr,
		     int family, int l3index, const __u8 *hash_location)
{
	/* This gets called for each TCP segment that has TCP-MD5 option.
	 * We have 2 drop cases:
	 * o An MD5 signature is present, but we're not expecting one.
	 * o The MD5 signature is wrong.
	 */
	/* “带签名但未配置 key”和“配置 key但摘要错误”分别计数，便于区分策略与攻击。 */
	const struct tcp_sock *tp = tcp_sk(sk);
	struct tcp_md5sig_key *key;
	u8 newhash[16];

	key = tcp_md5_do_lookup(sk, l3index, saddr, family);
	if (!key) {
		NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPMD5UNEXPECTED);
		trace_tcp_hash_md5_unexpected(sk, skb);
		return SKB_DROP_REASON_TCP_MD5UNEXPECTED;
	}

	/* Check the signature.
	 * To support dual stack listeners, we need to handle
	 * IPv4-mapped case.
	 */
	/* 双栈 listener 的 IPv4-mapped 报文必须按实际 IPv4 pseudo-header 计算摘要。 */
	if (family == AF_INET)
		tcp_v4_md5_hash_skb(newhash, key, NULL, skb);
	else
		tp->af_specific->calc_md5_hash(newhash, key, NULL, skb);
	if (crypto_memneq(hash_location, newhash, 16)) {
		NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPMD5FAILURE);
		trace_tcp_hash_md5_mismatch(sk, skb);
		return SKB_DROP_REASON_TCP_MD5FAILURE;
	}
	return SKB_NOT_DROPPED_YET;
}
#else
static inline enum skb_drop_reason
tcp_inbound_md5_hash(const struct sock *sk, const struct sk_buff *skb,
		     const void *saddr, const void *daddr,
		     int family, int l3index, const __u8 *hash_location)
{
	return SKB_NOT_DROPPED_YET;
}

#endif

#if defined(CONFIG_TCP_MD5SIG) || defined(CONFIG_TCP_AO)
/*
 * Parse Signature options
 */
/*
 * tcp_do_parse_auth_options - 单遍扫描 TCP option 并定位唯一 MD5 或 AO 认证字段。
 *
 * @th->doff 给出 option 总长度；@md5_hash/@ao_hash 先置 NULL，成功后输出借用指针。
 * NOP 消耗 1 字节，普通 option 必须至少 2 字节且不越界；认证长度错误或同一报文
 * 出现两个认证 option 返回 EINVAL/EEXIST。函数不验证摘要，只建立后续验证所需位置。
 */
int tcp_do_parse_auth_options(const struct tcphdr *th,
			      const u8 **md5_hash, const u8 **ao_hash)
{
	int length = (th->doff << 2) - sizeof(*th);
	const u8 *ptr = (const u8 *)(th + 1);
	unsigned int minlen = TCPOLEN_MD5SIG;

	if (IS_ENABLED(CONFIG_TCP_AO))
		minlen = sizeof(struct tcp_ao_hdr) + 1;

	*md5_hash = NULL;
	*ao_hash = NULL;

	/* If not enough data remaining, we can short cut */
	/* 剩余长度小于最短认证 option 时不可能再命中，避免读 opcode/opsize 越界。 */
	while (length >= minlen) {
		int opcode = *ptr++;
		int opsize;

		switch (opcode) {
		case TCPOPT_EOL:
			return 0;
		case TCPOPT_NOP:
			length--;
			continue;
		default:
			opsize = *ptr++;
			if (opsize < 2 || opsize > length)
				return -EINVAL;
			if (opcode == TCPOPT_MD5SIG) {
				if (opsize != TCPOLEN_MD5SIG)
					return -EINVAL;
				if (unlikely(*md5_hash || *ao_hash))
					return -EEXIST;
				*md5_hash = ptr;
			} else if (opcode == TCPOPT_AO) {
				if (opsize <= sizeof(struct tcp_ao_hdr))
					return -EINVAL;
				if (unlikely(*md5_hash || *ao_hash))
					return -EEXIST;
				*ao_hash = ptr;
			}
		}
		ptr += opsize - 2;
		length -= opsize;
	}
	return 0;
}
#endif

/* Called with rcu_read_lock() */
/*
 * tcp_inbound_hash - 强制执行连接配置的 TCP-AO/MD5 入站认证策略。
 *
 * @req 可空，握手 request 存在时还验证报文认证类型与 SYN 阶段协商一致；dif/sdif
 * 用于计算 L3 master 域，防止跨 VRF 误用 key。无认证 option 的 fast path 仍必须
 * 查询“该 peer 是否要求签名”，否则攻击者可通过删除 option 绕过认证。返回
 * SKB_NOT_DROPPED_YET 表示验证通过，其他值是精确 drop reason；函数不消费 skb。
 */
enum skb_drop_reason
tcp_inbound_hash(struct sock *sk, const struct request_sock *req,
		 const struct sk_buff *skb,
		 const void *saddr, const void *daddr,
		 int family, int dif, int sdif)
{
	const struct tcphdr *th = tcp_hdr(skb);
	const struct tcp_ao_hdr *aoh;
	const __u8 *md5_location;
	int l3index;

	/* Invalid option or two times meet any of auth options */
	/* option 结构错误或 MD5/AO 重复/并存都在做任何 key lookup 前拒绝。 */
	if (tcp_parse_auth_options(th, &md5_location, &aoh)) {
		trace_tcp_hash_bad_header(sk, skb);
		return SKB_DROP_REASON_TCP_AUTH_HDR;
	}

	if (req) {
		if (tcp_rsk_used_ao(req) != !!aoh) {
			u8 keyid, rnext, maclen;

			if (aoh) {
				keyid = aoh->keyid;
				rnext = aoh->rnext_keyid;
				maclen = tcp_ao_hdr_maclen(aoh);
			} else {
				keyid = rnext = maclen = 0;
			}

			NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPAOBAD);
			trace_tcp_ao_handshake_failure(sk, skb, keyid, rnext, maclen);
			return SKB_DROP_REASON_TCP_AOFAILURE;
		}
	}

	/* sdif set, means packet ingressed via a device
	 * in an L3 domain and dif is set to the l3mdev
	 */
	/* VRF 场景用 master 域索引选择 key，普通设备则 l3index=0。 */
	l3index = sdif ? dif : 0;

	/* Fast path: unsigned segments */
	/* 无认证 option 是常见路径，但仍需确认连接策略没有强制认证。 */
	if (likely(!md5_location && !aoh)) {
		/* Drop if there's TCP-MD5 or TCP-AO key with any rcvid/sndid
		 * for the remote peer. On TCP-AO established connection
		 * the last key is impossible to remove, so there's
		 * always at least one current_key.
		 */
		/* 配置要求认证却缺 option 必须丢弃；否则“去掉签名”会成为直接绕过。 */
		if (tcp_ao_required(sk, saddr, family, l3index, true)) {
			trace_tcp_hash_ao_required(sk, skb);
			return SKB_DROP_REASON_TCP_AONOTFOUND;
		}
		if (unlikely(tcp_md5_do_lookup(sk, l3index, saddr, family))) {
			NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPMD5NOTFOUND);
			trace_tcp_hash_md5_required(sk, skb);
			return SKB_DROP_REASON_TCP_MD5NOTFOUND;
		}
		return SKB_NOT_DROPPED_YET;
	}

	if (aoh)
		return tcp_inbound_ao_hash(sk, skb, family, req, l3index, aoh);

	return tcp_inbound_md5_hash(sk, skb, saddr, daddr, family,
				    l3index, md5_location);
}

/*
 * tcp_done - 把连接推进到 TCP_CLOSE，并选择通知用户或销毁 orphan。
 *
 * 调用者已在某个 TCP 状态机/定时器串行上下文中持有 socket 保护。函数清发送
 * 定时器，移除可能残留的 Fast Open request，并发布双向 shutdown。若仍有用户
 * file，sk_state_change 唤醒 connect/recv/poll 观察最终状态；SOCK_DEAD 表明没有
 * 用户接收通知，直接 inet_csk_destroy_sock 进入协议析构。它不等价于立即 kfree：
 * hash、timer、RCU 和引用计数仍决定存储何时真正释放。
 */
void tcp_done(struct sock *sk)
{
	struct request_sock *req;

	/* We might be called with a new socket, after
	 * inet_csk_prepare_forced_close() has been called
	 * so we can not use lockdep_sock_is_held(sk)
	 */
	/* 强制关闭准备阶段可能没有常规 lockdep owner，但调用协议仍保证对象受保护。 */
	req = rcu_dereference_protected(tcp_sk(sk)->fastopen_rsk, 1);

	if (sk->sk_state == TCP_SYN_SENT || sk->sk_state == TCP_SYN_RECV)
		TCP_INC_STATS(sock_net(sk), TCP_MIB_ATTEMPTFAILS);

	tcp_set_state(sk, TCP_CLOSE);
	tcp_clear_xmit_timers(sk);
	if (req)
		reqsk_fastopen_remove(sk, req, false);

	WRITE_ONCE(sk->sk_shutdown, SHUTDOWN_MASK);

	if (!sock_flag(sk, SOCK_DEAD))
		sk->sk_state_change(sk);
	else
		inet_csk_destroy_sock(sk);
}

/*
 * tcp_abort - 由内核/BPF 管理路径强制终止任意形态的 TCP endpoint。
 *
 * NEW_SYN_RECV 只是 request，需从 listener SYN queue 摘除；TIME_WAIT 是轻量对象，
 * 临时加引用后 deschedule；full socket 则取得用户锁（BPF 上下文已保证锁），防止与
 * close 并发，再在 BH lock 下按状态发 RST并以 @err 完成。已 CLOSE 返回 ENOENT。
 * 成功 0 只表示本地对象已中止，RST 仍可能丢失。
 */
int tcp_abort(struct sock *sk, int err)
{
	int state = inet_sk_state_load(sk);

	if (state == TCP_NEW_SYN_RECV) {
		struct request_sock *req = inet_reqsk(sk);

		local_bh_disable();
		inet_csk_reqsk_queue_drop(req->rsk_listener, req);
		local_bh_enable();
		return 0;
	}
	if (state == TCP_TIME_WAIT) {
		struct inet_timewait_sock *tw = inet_twsk(sk);

		refcount_inc(&tw->tw_refcnt);
		local_bh_disable();
		inet_twsk_deschedule_put(tw);
		local_bh_enable();
		return 0;
	}

	/* BPF context ensures sock locking. */
	/* BPF hook 已由调用框架串行 socket，重复 lock_sock 会自锁。 */
	if (!has_current_bpf_ctx())
		/* Don't race with userspace socket closes such as tcp_close. */
		/* 非 BPF 管理调用必须取得用户锁，确保只一个路径执行 full-socket abort。 */
		lock_sock(sk);

	/* Avoid closing the same socket twice. */
	/* 第二个 abort 不再发送 RST/重复析构，以 ENOENT 表示目标已结束。 */
	if (sk->sk_state == TCP_CLOSE) {
		if (!has_current_bpf_ctx())
			release_sock(sk);
		return -ENOENT;
	}

	if (sk->sk_state == TCP_LISTEN) {
		tcp_set_state(sk, TCP_CLOSE);
		inet_csk_listen_stop(sk);
	}

	/* Don't race with BH socket closes such as inet_csk_listen_stop. */
	/* 用户锁之外再取 BH 短锁，串行软中断状态变化和 reset 发送。 */
	local_bh_disable();
	bh_lock_sock(sk);

	if (tcp_need_reset(sk->sk_state))
		tcp_send_active_reset(sk, GFP_ATOMIC,
				      SK_RST_REASON_TCP_STATE);
	tcp_done_with_error(sk, err);

	bh_unlock_sock(sk);
	local_bh_enable();
	if (!has_current_bpf_ctx())
		release_sock(sk);
	return 0;
}
EXPORT_SYMBOL_GPL(tcp_abort);

extern struct tcp_congestion_ops tcp_reno;

static __initdata unsigned long thash_entries;
/* 解析启动参数 thash_entries=；返回 1 表示参数已消费，非法值保持自动大小。 */
static int __init set_thash_entries(char *str)
{
	ssize_t ret;

	if (!str)
		return 0;

	ret = kstrtoul(str, 0, &thash_entries);
	if (ret)
		return 0;

	return 1;
}
__setup("thash_entries=", set_thash_entries);

/*
 * tcp_init_mem - 按可用于 buffer 的物理页数计算 TCP 全局 pressure 三水位。
 * 最少以 128 页基准；low/pressure/high 约为 limit 的 3/4、1、3/2。仅启动期写入，
 * 后续 sysctl 可覆盖，百分比是对总 buffer pages 的经验性资源预算。
 */
static void __init tcp_init_mem(void)
{
	unsigned long limit = nr_free_buffer_pages() / 16;

	limit = max(limit, 128UL);
	sysctl_tcp_mem[0] = limit / 4 * 3;		/* 4.68 % */
	sysctl_tcp_mem[1] = limit;			/* 6.25 % */
	sysctl_tcp_mem[2] = sysctl_tcp_mem[0] * 2;	/* 9.37 % */
}

/*
 * tcp_struct_check - 编译/启动期验证 tcp_sock 热字段仍位于声明的 cacheline 分组。
 * CACHELINE_ASSERT_GROUP_MEMBER 失败会在构建期暴露结构体布局漂移，防止普通字段
 * 添加悄然把 TX/RX 热路径跨更多 cache line。它不调整布局，只验证头文件契约。
 */
static void __init tcp_struct_check(void)
{
	/* TX read-mostly hotpath cache lines */
	/* 发送频繁读取、较少写入的字段必须保持在指定 cacheline group。 */
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_tx, max_window);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_tx, rcv_ssthresh);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_tx, reordering);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_tx, notsent_lowat);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_tx, gso_segs);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_tx, retransmit_skb_hint);
#if IS_ENABLED(CONFIG_TLS_DEVICE)
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_tx, tcp_clean_acked);
#endif

	/* TXRX read-mostly hotpath cache lines */
	/* 收发两侧共享只读热点集中放置，减少两个方向的 cache miss。 */
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_txrx, tsoffset);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_txrx, snd_wnd);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_txrx, mss_cache);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_txrx, snd_cwnd);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_txrx, prr_out);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_txrx, lost_out);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_txrx, sacked_out);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_txrx, scaling_ratio);

	/* RX read-mostly hotpath cache lines */
	/* 接收路径频繁读取的游标、RTT、乱序树入口保持局部。 */
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, copied_seq);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, snd_wl1);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, tlp_high_seq);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, rttvar_us);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, retrans_out);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, advmss);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, urg_data);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, lost);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, rtt_min);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, out_of_order_queue);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, snd_ssthresh);

	/* TX read-write hotpath cache lines */
	/* 高频写字段与只读组隔离，降低其他 CPU 读取时的伪共享失效。 */
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, data_segs_out);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, delivered);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, delivered_ce);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, bytes_acked);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, bytes_sent);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, first_tx_mstamp);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, delivered_mstamp);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, snd_sml);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, chrono_start);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, chrono_stat);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, write_seq);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, pushed_seq);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, lsndtime);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, mdev_us);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, tcp_wstamp_ns);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, accecn_opt_tstamp);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, rtt_seq);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, max_packets_out);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, cwnd_usage_seq);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, rate_delivered);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, rate_interval_us);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, tsorted_sent_queue);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, highest_sack);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, ecn_flags);

	/* TXRX read-write hotpath cache lines */
	/* ACK 同时读写的核心序号、窗口和时间字段共享一组。 */
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, pred_flags);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, tcp_clock_cache);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, tcp_mstamp);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, rcv_nxt);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, snd_nxt);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, snd_una);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, window_clamp);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, srtt_us);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, packets_out);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, snd_up);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, received_ce);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, received_ecn_bytes);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, app_limited);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, rcv_wnd);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, rcv_mwnd_seq);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, rcv_tstamp);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, rx_opt);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, segs_in);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, segs_out);

	/* RX read-write hotpath cache lines */
	/* 只由接收提交频繁修改的统计和窗口估计集中，避免污染发送专用行。 */
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, bytes_received);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, data_segs_in);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, rcv_wup);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, rcv_rtt_last_tsecr);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, delivered_ecn_bytes);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, pkts_acked_ewma);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, rcv_rtt_est);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, rcvq_space);
}

/*
 * tcp_init - 启动期建立全局 TCP hash、slab、内存水位、timer 和默认算法。
 *
 * `__init` 表示系统启动完成后函数代码可释放，不能在运行期再调用。BUILD_BUG_ON
 * 验证最小 MSS 足以容纳 option、tcp_skb_cb 装得进 skb->cb；随后验证 cacheline
 * 布局。函数创建 listen/established/bind/bind2 索引及 bucket locks，大小按物理
 * 内存和 thash_entries 启动参数决定；SLAB_PANIC/显式 panic 表示这些基础设施失败
 * 无法降级启动网络栈。
 *
 * 发布顺序是先完成所有表和锁，再设置内存/sysctl 默认，最后初始化 IPv4、metrics、
 * 注册 Reno、TSQ 与 MPTCP。返回后收包查找和 socket 创建才可安全依赖 tcp_hashinfo。
 * 函数无返回值；不可恢复错误直接 BUG/panic。
 */
void __init tcp_init(void)
{
	int max_rshare, max_wshare, cnt;
	unsigned long limit;
	unsigned int i;

	BUILD_BUG_ON(TCP_MIN_SND_MSS <= MAX_TCP_OPTION_SPACE);
	BUILD_BUG_ON(sizeof(struct tcp_skb_cb) >
		     sizeof_field(struct sk_buff, cb));

	tcp_struct_check();

	percpu_counter_init(&tcp_sockets_allocated, 0, GFP_KERNEL);

	timer_setup(&tcp_orphan_timer, tcp_orphan_update, TIMER_DEFERRABLE);
	mod_timer(&tcp_orphan_timer, jiffies + TCP_ORPHAN_TIMER_PERIOD);

	inet_hashinfo2_init(&tcp_hashinfo, "tcp_listen_portaddr_hash",
			    thash_entries, 21,  /* 每 2 MB 内存预留一个槽位 */
			    0, 64 * 1024);
	tcp_hashinfo.bind_bucket_cachep =
		kmem_cache_create("tcp_bind_bucket",
				  sizeof(struct inet_bind_bucket), 0,
				  SLAB_HWCACHE_ALIGN | SLAB_PANIC |
				  SLAB_ACCOUNT,
				  NULL);
	tcp_hashinfo.bind2_bucket_cachep =
		kmem_cache_create("tcp_bind2_bucket",
				  sizeof(struct inet_bind2_bucket), 0,
				  SLAB_HWCACHE_ALIGN | SLAB_PANIC |
				  SLAB_ACCOUNT,
				  NULL);

	/* Size and allocate the main established and bind bucket
	 * hash tables.
	 *
	 * The methodology is similar to that of the buffer cache.
	 */
	/* 按内存比例自动缩放，显式 thash_entries 则尊重管理员容量选择。 */
	/* established ehash 按四元组查 full/request/timewait；nulls hlist 可检测遍历期间重排。 */
	tcp_hashinfo.ehash =
		alloc_large_system_hash("TCP established",
					sizeof(struct inet_ehash_bucket),
					thash_entries,
					17, /* 每 128 KB 内存预留一个槽位 */
					0,
					NULL,
					&tcp_hashinfo.ehash_mask,
					0,
					thash_entries ? 0 : 512 * 1024);
	for (i = 0; i <= tcp_hashinfo.ehash_mask; i++)
		INIT_HLIST_NULLS_HEAD(&tcp_hashinfo.ehash[i].chain, i);

	if (inet_ehash_locks_alloc(&tcp_hashinfo))
		panic("TCP: failed to alloc ehash_locks");
	/* bhash/bhash2 分别支持端口和端口+地址约束；连续分配后拆成两个同尺寸数组。 */
	tcp_hashinfo.bhash =
		alloc_large_system_hash("TCP bind",
					2 * sizeof(struct inet_bind_hashbucket),
					tcp_hashinfo.ehash_mask + 1,
					17, /* 每 128 KB 内存预留一个槽位 */
					0,
					&tcp_hashinfo.bhash_size,
					NULL,
					0,
					64 * 1024);
	tcp_hashinfo.bhash_size = 1U << tcp_hashinfo.bhash_size;
	tcp_hashinfo.bhash2 = tcp_hashinfo.bhash + tcp_hashinfo.bhash_size;
	for (i = 0; i < tcp_hashinfo.bhash_size; i++) {
		spin_lock_init(&tcp_hashinfo.bhash[i].lock);
		INIT_HLIST_HEAD(&tcp_hashinfo.bhash[i].chain);
		spin_lock_init(&tcp_hashinfo.bhash2[i].lock);
		INIT_HLIST_HEAD(&tcp_hashinfo.bhash2[i].chain);
	}

	tcp_hashinfo.pernet = false;

	cnt = tcp_hashinfo.ehash_mask + 1;
	sysctl_tcp_max_orphans = cnt / 2;

	tcp_init_mem();
	/* Set per-socket limits to no more than 1/128 the pressure threshold */
	/* 默认单连接上限限制为总 pressure 预算的 1/128，防止少数连接独占全局内存。 */
	limit = nr_free_buffer_pages() << (PAGE_SHIFT - 7);
	max_wshare = min(4UL*1024*1024, limit);
	max_rshare = min(32UL*1024*1024, limit);

	init_net.ipv4.sysctl_tcp_wmem[0] = PAGE_SIZE;
	init_net.ipv4.sysctl_tcp_wmem[1] = 16*1024;
	init_net.ipv4.sysctl_tcp_wmem[2] = max(64*1024, max_wshare);

	init_net.ipv4.sysctl_tcp_rmem[0] = PAGE_SIZE;
	init_net.ipv4.sysctl_tcp_rmem[1] = 131072;
	init_net.ipv4.sysctl_tcp_rmem[2] = max(131072, max_rshare);

	pr_info("Hash tables configured (established %u bind %u)\n",
		tcp_hashinfo.ehash_mask + 1, tcp_hashinfo.bhash_size);

	/* 所有共享资源就绪后才注册协议入口，避免早到报文观察半初始化全局状态。 */
	tcp_v4_init();
	tcp_metrics_init();
	BUG_ON(tcp_register_congestion_control(&tcp_reno) != 0);
	tcp_tsq_work_init();
	mptcp_init();
}
