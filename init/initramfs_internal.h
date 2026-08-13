// SPDX-License-Identifier: GPL-2.0
#ifndef __INITRAMFS_INTERNAL_H__
#define __INITRAMFS_INTERNAL_H__

/*
 * initramfs 解包器的目录内共享边界。生产调用者和 KUnit 测试都可传入一段由
 * 裸 newc/crc cpio、零填充或压缩流拼接成的内存；实现只读取输入，不接管或
 * 释放 buf。接口仅在启动期串行可用，因为实现依赖 __initdata 状态且函数本体
 * 标为 __init；成功返回 NULL，失败返回无需调用者释放的首个静态错误字符串。
 * 声明本身不带 __init，便于测试配置引用，但不能据此在 init 段回收后调用。
 */
char *unpack_to_rootfs(char *buf, unsigned long len);

/* newc/crc 的 ASCII 固定头为 110 字节：6 字节 magic 加 13 个 8 字节十六进制字段。 */
#define CPIO_HDRLEN 110

#endif
