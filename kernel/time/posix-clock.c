// SPDX-License-Identifier: GPL-2.0+
/*
 * Support for dynamic clock devices
 *
 * Copyright (C) 2010 OMICRON electronics GmbH
 */
/*
 * 学习总览：本层把驱动提供的动态 POSIX clock 同时暴露为字符设备和由 fd 编码的负 clockid。
 * 每次普通文件操作从 file->private_data 找到 per-open context；clock_* 系统调用先 fget 对应 fd，再验证
 * 它确由本层 open。rwsem 读锁覆盖驱动 callback，注销者以写锁置 zombie，从而不让硬件消失与新操作交叉；
 * device 引用则让已打开文件的 clock 私有容器一直存活到最后 release。原版权与动态时钟用途见上方保留块。
 */
#include <linux/device.h>
#include <linux/export.h>
#include <linux/file.h>
#include <linux/posix-clock.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>

#include "posix-timers.h"

/*
 * Returns NULL if the posix_clock instance attached to 'fp' is old and stale.
 */
/*
 * get_posix_clock() - 从已成功 open 的 @fp 取得 clock，并获取其 rwsem 读锁。
 * private_data 必须是有效 posix_clock_context；若 zombie=false，返回借用 clk 且锁保持到 put_posix_clock；
 * 若已注销则自行解锁并返回 NULL。读锁让 unregister 的写侧等待当前 driver callback 完成，不新增 device 引用。
 */
static struct posix_clock *get_posix_clock(struct file *fp)
{
	struct posix_clock_context *pccontext = fp->private_data;
	struct posix_clock *clk = pccontext->clk;

	down_read(&clk->rwsem);

	if (!clk->zombie)
		return clk;

	up_read(&clk->rwsem);

	return NULL;
}

/* put_posix_clock() 释放与成功 get_posix_clock 配对的 rwsem 读锁；@clk 之后不得再由该保护域解引用。 */
static void put_posix_clock(struct posix_clock *clk)
{
	up_read(&clk->rwsem);
}

/*
 * posix_clock_read() - 字符设备 read 包装。
 * @fp 提供 context/f_flags，@buf/@count 原样交给可选驱动 read，@ppos 未使用。stale 返回 -ENODEV，未实现
 * 返回 -EINVAL，否则返回驱动字节数/错误；callback 全程持 clock rwsem 读锁，用户复制责任在驱动。
 */
static ssize_t posix_clock_read(struct file *fp, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct posix_clock_context *pccontext = fp->private_data;
	struct posix_clock *clk = get_posix_clock(fp);
	int err = -EINVAL;

	if (!clk)
		return -ENODEV;

	if (clk->ops.read)
		err = clk->ops.read(pccontext, fp->f_flags, buf, count);

	put_posix_clock(clk);

	return err;
}

/*
 * posix_clock_poll() - 字符设备 poll 包装。
 * stale clock 返回 EPOLLERR，未实现 poll 返回 0；否则把 context、file 和 wait table 交驱动，并在 rwsem
 * 读锁内返回其事件 mask。驱动负责 poll_wait 注册及自身事件锁，函数不睡眠等待事件本身。
 */
static __poll_t posix_clock_poll(struct file *fp, poll_table *wait)
{
	struct posix_clock_context *pccontext = fp->private_data;
	struct posix_clock *clk = get_posix_clock(fp);
	__poll_t result = 0;

	if (!clk)
		return EPOLLERR;

	if (clk->ops.poll)
		result = clk->ops.poll(pccontext, fp, wait);

	put_posix_clock(clk);

	return result;
}

/*
 * posix_clock_ioctl() - native/compat 共用的字符设备 ioctl 包装。
 * @cmd/@arg 原样交给驱动，context 可通过保存的 fp 检查访问模式；stale 返回 -ENODEV，缺方法 -ENOTTY。
 * 驱动 long 返回值当前经局部 int 保存后返回，ABI 转换与用户指针解释均由驱动负责；callback 持 rwsem 读锁。
 */
static long posix_clock_ioctl(struct file *fp,
			      unsigned int cmd, unsigned long arg)
{
	struct posix_clock_context *pccontext = fp->private_data;
	struct posix_clock *clk = get_posix_clock(fp);
	int err = -ENOTTY;

	if (!clk)
		return -ENODEV;

	if (clk->ops.ioctl)
		err = clk->ops.ioctl(pccontext, cmd, arg);

	put_posix_clock(clk);

	return err;
}

/*
 * posix_clock_open() - 为一次字符设备 open 建立 posix_clock_context。
 * 从 inode->i_cdev 反推 clk，持 rwsem 读锁拒绝 zombie；分配清零 context，写入 clk/fp，并调用可选驱动
 * open(context, f_mode)，驱动可设置 private_clkdata。失败释放 context 并返回 -ENODEV/-ENOMEM/驱动错误；
 * 成功发布 fp->private_data、get_device 保持驱动私有容器，返回 0。读锁使 open 与 unregister 严格排序。
 */
static int posix_clock_open(struct inode *inode, struct file *fp)
{
	int err;
	struct posix_clock *clk =
		container_of(inode->i_cdev, struct posix_clock, cdev);
	struct posix_clock_context *pccontext;

	down_read(&clk->rwsem);

	if (clk->zombie) {
		err = -ENODEV;
		goto out;
	}
	pccontext = kzalloc_obj(*pccontext);
	if (!pccontext) {
		err = -ENOMEM;
		goto out;
	}
	pccontext->clk = clk;
	pccontext->fp = fp;
	if (clk->ops.open) {
		err = clk->ops.open(pccontext, fp->f_mode);
		if (err) {
			kfree(pccontext);
			goto out;
		}
	}

	fp->private_data = pccontext;
	get_device(clk->dev);
	err = 0;
out:
	up_read(&clk->rwsem);
	return err;
}

/*
 * posix_clock_release() - 关闭一次成功 open 的 context。
 * private_data 缺失返回 -ENODEV；否则调用可选驱动 release（其错误作为 close 结果），再 put_device、释放
 * context 并清 private_data。release 不取 rwsem，允许已 zombie 的打开文件完成清理；device 引用保证 clk
 * 及驱动私有容器在 callback 返回前仍存活，驱动负责同步自身硬件/事件资源。
 */
static int posix_clock_release(struct inode *inode, struct file *fp)
{
	struct posix_clock_context *pccontext = fp->private_data;
	struct posix_clock *clk;
	int err = 0;

	if (!pccontext)
		return -ENODEV;
	clk = pccontext->clk;

	if (clk->ops.release)
		err = clk->ops.release(pccontext);

	put_device(clk->dev);

	kfree(pccontext);
	fp->private_data = NULL;

	return err;
}

/* 字符设备操作表静态常驻；compat 与 native ioctl 共用入口，具体 ABI 差异必须由驱动 cmd 处理。 */
static const struct file_operations posix_clock_file_operations = {
	.owner		= THIS_MODULE,
	.read		= posix_clock_read,
	.poll		= posix_clock_poll,
	.unlocked_ioctl	= posix_clock_ioctl,
	.compat_ioctl	= posix_clock_ioctl,
	.open		= posix_clock_open,
	.release	= posix_clock_release,
};

/*
 * posix_clock_register() - 发布驱动提供的动态 clock 字符设备。
 * @clk 必须嵌在由 @dev 生命周期管理的驱动对象中，ops 已填；@dev 已初始化、devt/release 已设置且尚未暴露。
 * 调用者还须保证首次注册时 clk->zombie=false，本函数只初始化 rwsem，不会清该状态，注销后的对象不可直接重用。
 * 初始化 rwsem，以公共 fops 初始化 cdev，再由 cdev_device_add 同时发布 cdev/device。失败打印设备号并原样
 * 返回错误，且按 cdev helper 契约用户态可能已短暂打开，驱动仍须处理回滚并最终释放 device；成功设置
 * driver module owner 与 clk->dev，返回 0。调用者随后以 unregister 配对，不转移 ops ownership。
 */
int posix_clock_register(struct posix_clock *clk, struct device *dev)
{
	int err;

	init_rwsem(&clk->rwsem);

	cdev_init(&clk->cdev, &posix_clock_file_operations);
	err = cdev_device_add(&clk->cdev, dev);
	if (err) {
		pr_err("%s unable to add device %d:%d\n",
			dev_name(dev), MAJOR(dev->devt), MINOR(dev->devt));
		return err;
	}
	clk->cdev.owner = clk->ops.owner;
	clk->dev = dev;

	return 0;
}
EXPORT_SYMBOL_GPL(posix_clock_register);

/*
 * posix_clock_unregister() - 撤销已成功注册的动态 clock，并阻止后续操作进入驱动。
 * 先 cdev_device_del 移除新 open/sysfs 入口；已打开 fops 仍可能运行。随后取得 rwsem 写锁，等待所有持读锁
 * callback 退出并置 zombie，释放锁后 put_device 交还注册时的初始引用。已有 open 各持一份 device 引用，
 * 仍可 release，最后一份引用触发 dev->release 释放驱动容器；本函数返回后驱动不可再接受 read/ioctl/clock_*。
 */
void posix_clock_unregister(struct posix_clock *clk)
{
	cdev_device_del(&clk->cdev, clk->dev);

	down_write(&clk->rwsem);
	clk->zombie = true;
	up_write(&clk->rwsem);

	put_device(clk->dev);
}
EXPORT_SYMBOL_GPL(posix_clock_unregister);

/* posix_clock_desc 在一次动态 clock_* 调用中配对保存 fget 的 file 与持 rwsem 读锁的 clock 借用指针。 */
struct posix_clock_desc {
	struct file *fp;
	struct posix_clock *clk;
};

/*
 * get_clock_desc() - 把 fd 编码的负 @id 解析为受保护的动态 clock 描述符。
 * clockid_to_fd 后 fget；无 fd 返回 -EINVAL。只有 file fops 的 open 精确等于本层入口且 private_data 非空
 * 才接受，随后 get_posix_clock 获取读锁；zombie 返回 -ENODEV。成功向 @cd 写 fp/clk 并返回 0，调用者必须
 * put_clock_desc；任一失败都 fput，不留下读锁或 file 引用。
 */
static int get_clock_desc(const clockid_t id, struct posix_clock_desc *cd)
{
	struct file *fp = fget(clockid_to_fd(id));
	int err = -EINVAL;

	if (!fp)
		return err;

	if (fp->f_op->open != posix_clock_open || !fp->private_data)
		goto out;

	cd->fp = fp;
	cd->clk = get_posix_clock(fp);

	err = cd->clk ? 0 : -ENODEV;
out:
	if (err)
		fput(fp);
	return err;
}

/* put_clock_desc() 先释放 clock rwsem 读锁，再 fput fd 引用；只可用于成功初始化的 @cd。 */
static void put_clock_desc(struct posix_clock_desc *cd)
{
	put_posix_clock(cd->clk);
	fput(cd->fp);
}

/*
 * pc_clock_adjtime() - 动态 clock 的 clock_adjtime 适配。
 * 先取得 desc；@tx->modes!=0 表示修改，要求原 fd 含 FMODE_WRITE，否则 -EACCES，modes=0 查询允许只读 fd。
 * 缺 clock_adjtime 返回 -EOPNOTSUPP，否则在 rwsem 读锁下返回驱动结果；所有成功 get 路径统一释放 desc。
 */
static int pc_clock_adjtime(clockid_t id, struct __kernel_timex *tx)
{
	struct posix_clock_desc cd;
	int err;

	err = get_clock_desc(id, &cd);
	if (err)
		return err;

	if (tx->modes && (cd.fp->f_mode & FMODE_WRITE) == 0) {
		err = -EACCES;
		goto out;
	}

	if (cd.clk->ops.clock_adjtime)
		err = cd.clk->ops.clock_adjtime(cd.clk, tx);
	else
		err = -EOPNOTSUPP;
out:
	put_clock_desc(&cd);

	return err;
}

/*
 * pc_clock_gettime() - 动态 clock_gettime 适配。
 * @ts 为驱动输出；descriptor/stale 错误原样返回，缺方法 -EOPNOTSUPP，否则在读锁内调用并返回驱动结果，
 * 最终总是释放 file 引用与 rwsem。用户空间复制由上层 POSIX syscall 完成。
 */
static int pc_clock_gettime(clockid_t id, struct timespec64 *ts)
{
	struct posix_clock_desc cd;
	int err;

	err = get_clock_desc(id, &cd);
	if (err)
		return err;

	if (cd.clk->ops.clock_gettime)
		err = cd.clk->ops.clock_gettime(cd.clk, ts);
	else
		err = -EOPNOTSUPP;

	put_clock_desc(&cd);

	return err;
}

/*
 * pc_clock_getres() - 动态 clock_getres 适配。
 * 取得 @id 对应 desc 后调用可选 clock_getres 写 @ts；缺方法 -EOPNOTSUPP，其余错误/成功原样返回并配对释放。
 */
static int pc_clock_getres(clockid_t id, struct timespec64 *ts)
{
	struct posix_clock_desc cd;
	int err;

	err = get_clock_desc(id, &cd);
	if (err)
		return err;

	if (cd.clk->ops.clock_getres)
		err = cd.clk->ops.clock_getres(cd.clk, ts);
	else
		err = -EOPNOTSUPP;

	put_clock_desc(&cd);

	return err;
}

/*
 * pc_clock_settime() - 动态 clock_settime 适配。
 * 先以 timespec64_valid_strict 拒绝负秒、越界纳秒等非法 @ts，再取得 desc；fd 必须 FMODE_WRITE，否则
 * -EACCES，缺方法 -EOPNOTSUPP，否则在读锁内调用驱动。验证失败不取引用，其余路径统一 put desc。
 */
static int pc_clock_settime(clockid_t id, const struct timespec64 *ts)
{
	struct posix_clock_desc cd;
	int err;

	if (!timespec64_valid_strict(ts))
		return -EINVAL;

	err = get_clock_desc(id, &cd);
	if (err)
		return err;

	if ((cd.fp->f_mode & FMODE_WRITE) == 0) {
		err = -EACCES;
		goto out;
	}

	if (cd.clk->ops.clock_settime)
		err = cd.clk->ops.clock_settime(cd.clk, ts);
	else
		err = -EOPNOTSUPP;
out:
	put_clock_desc(&cd);

	return err;
}

/*
 * clock_posix_dynamic 是 POSIX core 对所有 CLOCKFD 编码负 clockid 的静态操作表；只开放 getres/set/gettime/
 * adjtime，timer 与 nanosleep 操作不支持。系统调用层负责用户复制，四个入口负责 fd 权限、stale 与 driver 派发。
 */
const struct k_clock clock_posix_dynamic = {
	.clock_getres		= pc_clock_getres,
	.clock_set		= pc_clock_settime,
	.clock_get_timespec	= pc_clock_gettime,
	.clock_adj		= pc_clock_adjtime,
};
