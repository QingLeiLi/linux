/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 目录内协议总览：k_clock 是 POSIX syscall core 与固定 wall clock、CPU clock、alarm、fd 动态 clock、
 * auxiliary clock 之间的分派契约。clock_* 回调处理时间读写，timer_* 回调在 core 持 it_lock 时维护
 * k_itimer；callback 正运行时不得锁内等待，而以 TIMER_RETRY 请求 core 解锁、RCU 保护、等待并重新查找。
 */
#define TIMER_RETRY 1
/* TIMER_RETRY 只用于 timer_set/timer_del 的内部正值控制流，不是返回用户空间的成功计数。 */

/*
 * posix_timer_state 描述信号型 timer 的 core 状态：DISARMED 无真实排队（SIGEV_NONE 也保持此态），ARMED
 * 已进入底层 timer，REQUEUE_PENDING 表示已到期并排信号、等待实际递送后推进周期。状态由 it_lock 保护。
 */
enum posix_timer_state {
	POSIX_TIMER_DISARMED,
	POSIX_TIMER_ARMED,
	POSIX_TIMER_REQUEUE_PENDING,
};

/*
 * k_clock 回调契约：所有表对象静态常驻，可缺省不支持的操作。which_clock 保留原用户 clockid，动态/CPU
 * 实现据此解析 fd/pid；timespec/timex 指针均是已复制的内核内存。clock_get_timespec 返回当前 time namespace
 * 视图，clock_get_ktime 返回 root/host ktime，二者不能混用。
 *
 * timer_create 在通用字段/ID 预留后初始化 clock union，成功 0；失败须自行回滚局部资源。nsleep 处理已验证
 * 请求并可安装 restart_block。timer_set/timer_del 在 it_lock 下返回 0/错误或 TIMER_RETRY；timer_get 在锁下
 * 填预清零输出。timer_rearm 在信号实际递送时推进并启动，bool 表示是否成功排队；timer_forward 只推进期限并
 * 返回跨期数，timer_remaining 用调用者给的同一 now 求差。try_to_cancel 返回 1/0/负 callback-running；
 * timer_arm 的 expires 是相对值或 host 绝对值，bool false 表示期限已过；wait_running 在解锁+RCU 下推进
 * callback，可能暂退 RCU，返回后不得再解引用原 timr。
 */
struct k_clock {
	int	(*clock_getres)(const clockid_t which_clock,
				struct timespec64 *tp);
	int	(*clock_set)(const clockid_t which_clock,
			     const struct timespec64 *tp);
	/* Returns the clock value in the current time namespace. */
	/* 返回调用任务时间命名空间中的用户可见 clock 值。 */
	int	(*clock_get_timespec)(const clockid_t which_clock,
				      struct timespec64 *tp);
	/* Returns the clock value in the root time namespace. */
	/* 返回 root/host 时间坐标，供 timer expires、forward 与 remaining 内部运算。 */
	ktime_t	(*clock_get_ktime)(const clockid_t which_clock);
	int	(*clock_adj)(const clockid_t which_clock, struct __kernel_timex *tx);
	int	(*timer_create)(struct k_itimer *timer);
	int	(*nsleep)(const clockid_t which_clock, int flags,
			  const struct timespec64 *);
	int	(*timer_set)(struct k_itimer *timr, int flags,
			     struct itimerspec64 *new_setting,
			     struct itimerspec64 *old_setting);
	int	(*timer_del)(struct k_itimer *timr);
	void	(*timer_get)(struct k_itimer *timr,
			     struct itimerspec64 *cur_setting);
	bool	(*timer_rearm)(struct k_itimer *timr);
	s64	(*timer_forward)(struct k_itimer *timr, ktime_t now);
	ktime_t	(*timer_remaining)(struct k_itimer *timr, ktime_t now);
	int	(*timer_try_to_cancel)(struct k_itimer *timr);
	bool	(*timer_arm)(struct k_itimer *timr, ktime_t expires,
			     bool absolute, bool sigev_none);
	void	(*timer_wait_running)(struct k_itimer *timr);
};

/* 六张外部静态操作表分别由 CPU timer、fd 动态时钟、进程/线程 CPU clock、alarmtimer 与辅助时钟实现。 */
extern const struct k_clock clock_posix_cpu;
extern const struct k_clock clock_posix_dynamic;
extern const struct k_clock clock_process;
extern const struct k_clock clock_thread;
extern const struct k_clock alarm_clock;
extern const struct k_clock clock_aux;

/* posix_timer_queue_signal() 要求持 timr->it_lock；设置 DISARMED/REQUEUE_PENDING 并交 signal core 管理引用。 */
void posix_timer_queue_signal(struct k_itimer *timr);

/* 以下 common_* 供 hrtimer 与 alarmtimer 等表复用；调用者均持 timr->it_lock，参数/返回遵循上方回调契约。 */
void common_timer_get(struct k_itimer *timr, struct itimerspec64 *cur_setting);
int common_timer_set(struct k_itimer *timr, int flags,
		     struct itimerspec64 *new_setting,
		     struct itimerspec64 *old_setting);
void posix_timer_set_common(struct k_itimer *timer, struct itimerspec64 *new_setting);
int common_timer_del(struct k_itimer *timer);
