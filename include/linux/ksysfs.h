/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _KSYSFS_H_
#define _KSYSFS_H_

/*
 * ksysfs_init() - 建立全局 /sys/kernel 目录并发布通用内核属性。
 *
 * 宏观位置：do_basic_setup() 在 driver_init() 和各级 initcall 之前调用本函数，
 * 因而后续子系统可以把自己的 kobject、属性或挂载点挂到 kernel_kobj 之下。
 *
 * 入参：无。调用发生在可睡眠的启动期进程上下文；不要求调用者持锁。
 * 返回：无直接返回值。成功时 kernel_kobj 指向已发布的 /sys/kernel 根，并且
 * 通用属性组及可用的内核 notes 二进制文件已经建立；失败时函数自行逆序撤销
 * 已发布对象并打印错误，启动流程仍继续，因此调用者不能从返回值判断结果。
 * 该函数带 __init 的定义位于 kernel/ksysfs.c，启动结束后其代码可被回收。
 */
void ksysfs_init(void);

#endif /* _KSYSFS_H_ */
