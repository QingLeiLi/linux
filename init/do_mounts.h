/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 根挂载内部接口地图：本头文件连接 do_mounts.c 主分派、RAM disk/initrd 可选
 * 实现和早期内核态 syscall wrapper。调用者都位于启动期；配置关闭时用返回
 * “未加载”或无操作的 inline 桩保持主流程一致，不把 #ifdef 扩散到调用点。
 */
#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/syscalls.h>
#include <linux/unistd.h>
#include <linux/slab.h>
#include <linux/mount.h>
#include <linux/major.h>
#include <linux/root_dev.h>
#include <linux/init_syscalls.h>
#include <linux/task_work.h>
#include <linux/file.h>

/*
 * mount_root_generic() - 按文件系统候选把来源挂到 `/root`
 *
 * @name 是传给 VFS 的借用来源名；@pretty_name 是借用诊断名；@flags 是挂载
 * 标志。函数可睡眠，无直接返回值；成功提交根挂载，无法恢复的失败会 panic。
 */
void  mount_root_generic(char *name, char *pretty_name, int flags);
/*
 * mount_root() - 按 ROOT_DEV 哨兵/dev_t 分派网络、nodev 或块设备根
 *
 * @root_device_name 是可空借用 root= 文本。函数可睡眠，无直接返回值；成功
 * 后 `/root` 已建立，具体不可恢复失败语义由被选挂载器决定。
 */
void  mount_root(char *root_device_name);
/* 默认根挂载标志的共享启动状态，由 do_mounts.c 定义，其他挂载后端只借用。 */
extern int root_mountflags;

/*
 * create_dev() - 用指定 dev_t 创建启动期块设备节点
 *
 * @name 是借用路径、不可为 NULL；@dev 是目标设备号。函数在可睡眠的 __init
 * 上下文先 unlink 同名旧节点，再以 0600 权限 mknod 新块设备。返回 init_mknod()
 * 的 0 或负 errno；unlink 错误被忽略，因此失败时旧路径可能已被删除且不回滚。
 */
static inline __init int create_dev(char *name, dev_t dev)
{
	/* 先清除占位/旧节点，保证后续节点编码与当前 dev_t 一致。 */
	init_unlink(name);
	return init_mknod(name, S_IFBLK | 0600, new_encode_dev(dev));
}

#ifdef CONFIG_BLK_DEV_RAM
/*
 * rd_load_image() - 把 `/initrd.image` 装入 `/dev/ram`，必要时解压
 *
 * 无入参，在可睡眠 __init 上下文运行；返回 1 表示镜像成功装入，0 表示未
 * 装入/失败。实现持有并释放文件、缓冲区，调用者不取得资源 ownership。
 */
int __init rd_load_image(void);
#else
/*
 * rd_load_image() - RAM disk 关闭时报告“没有装入镜像”
 *
 * 无入参、无副作用、不睡眠，固定返回 0，使 initrd 主流程可统一调用。
 */
static inline int rd_load_image(void) { return 0; }
#endif

#ifdef CONFIG_BLK_DEV_INITRD
/*
 * initrd_load() - 可选地把 legacy initrd 装入 ram0 并移除临时镜像文件
 *
 * 无入参和直接返回值，在可睡眠 __init 上下文运行；是否装入由 mount_initrd
 * 等启动状态决定。函数自行管理 `/dev/ram` 和 `/initrd.image`，不向调用者转移资源。
 */
void __init initrd_load(void);
#else
/*
 * initrd_load() - initrd 支持关闭时的无操作桩
 *
 * 无入参、无返回值、无副作用且不睡眠。
 */
static inline void initrd_load(void) { }
#endif

/* Ensure that async file closing finished to prevent spurious errors. */
/* 确保异步文件关闭已经完成，防止后续根挂载因延迟释放而出现伪错误。 */
/*
 * init_flush_fput() - 收拢 delayed fput 和当前任务的文件关闭 task_work
 *
 * 无入参和直接返回值；必须在可睡眠进程上下文调用。先刷新全局 delayed_fput
 * 工作，再执行 current 已排队 task_work，使前一次失败挂载打开的 file 真正
 * 完成最终 fput。函数不保证任意新并发 close，也不转移对象 ownership。
 */
static inline void init_flush_fput(void)
{
	/* 收拢被推迟到 workqueue 的最终 file 引用释放。 */
	flush_delayed_fput();
	/* 执行当前启动任务上仍排队的 close task_work，完成局部释放。 */
	task_work_run();
}
