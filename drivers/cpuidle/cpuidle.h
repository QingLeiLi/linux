/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cpuidle.h - The internal header file
 */

#ifndef __DRIVER_CPUIDLE_H
#define __DRIVER_CPUIDLE_H

/* For internal use only */
/*
 * 仅供 drivers/cpuidle/ 内部实现使用。本头把核心执行、驱动注册、governor、sysfs 和
 * coupled-idle 子模块连接起来；对外稳定数据结构与驱动 API 位于 <linux/cpuidle.h>。
 * 这里声明的全局对象均由其定义文件拥有，使用者只借用指针/链表，控制面修改通常受
 * cpuidle_lock 或 cpuidle_driver_lock 保护，idle 快路径则依赖先暂停 handler 的协议。
 */
/* 启动参数选择的 governor 名称缓冲区，模块参数层写入、governor 控制面读取。 */
extern char param_governor[];
/* 当前与上一个 governor；切换期间用于启停设备及失败回退，不拥有 governor 对象。 */
extern struct cpuidle_governor *cpuidle_curr_governor;
extern struct cpuidle_governor *cpuidle_prev_governor;
/* 已注册 governor 和设备的全局链表头；节点分别嵌在 governor/device 对象中。 */
extern struct list_head cpuidle_governors;
extern struct list_head cpuidle_detected_devices;
/* 控制面全局互斥锁，以及专门串行化驱动发布/查找的自旋锁。 */
extern struct mutex cpuidle_lock;
extern spinlock_t cpuidle_driver_lock;
/* 查询全局禁用开关；enter_state 是 IRQ-off 普通状态进入与统计的核心实现。 */
extern int cpuidle_disabled(void);
extern int cpuidle_enter_state(struct cpuidle_device *dev,
		struct cpuidle_driver *drv, int next_state);

/* idle loop */
/*
 * 安装/撤下 idle handler 的发布边界。install 使已完成的设备配置可被 idle loop 读取；
 * uninstall 唤醒 idle CPU 并等待 RCU 读者退出，调用方通常持 cpuidle_lock。
 */
extern void cpuidle_install_idle_handler(void);
extern void cpuidle_uninstall_idle_handler(void);

/* governors */
/* 按名称借用已注册 governor，并在控制面锁协议下切换所有已启用设备的策略回调。 */
extern struct cpuidle_governor *cpuidle_find_governor(const char *str);
extern int cpuidle_switch_governor(struct cpuidle_governor *gov);

/* sysfs */

/* sysfs 边界只需要 struct device 的指针类型，避免内部头引入完整设备模型定义。 */
struct device;

/*
 * sysfs 生命周期分层：interface 管理全局入口，add/remove_sysfs 管理 CPU 注册层目录，
 * add/remove_device_sysfs 管理设备启用后才存在的属性。每对接口必须按注册阶段逆序调用；
 * add 返回 0 或负 errno，失败者不会把未完成资源的所有权交给调用方。
 */
extern int cpuidle_add_interface(void);
extern void cpuidle_remove_interface(struct device *dev);
extern int cpuidle_add_device_sysfs(struct cpuidle_device *device);
extern void cpuidle_remove_device_sysfs(struct cpuidle_device *device);
extern int cpuidle_add_sysfs(struct cpuidle_device *dev);
extern void cpuidle_remove_sysfs(struct cpuidle_device *dev);

#ifdef CONFIG_ARCH_NEEDS_CPU_IDLE_COUPLED
/*
 * coupled-idle 真正实现：识别/验证共享状态，协调成员 CPU 进入，并为每设备建立/拆除
 * 共享对象引用。drv/dev 仍由核心所有，coupled 子系统只管理协调状态及其生命周期。
 */
bool cpuidle_state_is_coupled(struct cpuidle_driver *drv, int state);
int cpuidle_coupled_state_verify(struct cpuidle_driver *drv);
int cpuidle_enter_state_coupled(struct cpuidle_device *dev,
		struct cpuidle_driver *drv, int next_state);
int cpuidle_coupled_register_device(struct cpuidle_device *dev);
void cpuidle_coupled_unregister_device(struct cpuidle_device *dev);
#else
/*
 * 架构未启用 coupled-idle 时的同形桩函数：状态永不 coupled、验证和登记成功、解除
 * 登记为空操作；若误走 coupled 进入则返回 -1，使上层把它当作进入失败而非成功状态。
 */
static inline
bool cpuidle_state_is_coupled(struct cpuidle_driver *drv, int state)
{
	return false;
}

static inline int cpuidle_coupled_state_verify(struct cpuidle_driver *drv)
{
	return 0;
}

static inline int cpuidle_enter_state_coupled(struct cpuidle_device *dev,
		struct cpuidle_driver *drv, int next_state)
{
	return -1;
}

static inline int cpuidle_coupled_register_device(struct cpuidle_device *dev)
{
	return 0;
}

static inline void cpuidle_coupled_unregister_device(struct cpuidle_device *dev)
{
}
#endif

/* 结束 cpuidle 驱动内部接口，防止多次包含。 */
#endif /* __DRIVER_CPUIDLE_H */
