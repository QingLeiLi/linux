// SPDX-License-Identifier: GPL-2.0
#include <linux/unistd.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/initrd.h>

#include "do_mounts.h"

/*
 * legacy initrd 交接地图：架构/bootloader 提供物理或映射后边界，早期参数可
 * 覆盖物理位置；prepare_namespace() 调用 initrd_load() 后，镜像经
 * rd_load_image() 写入 ram0。该机制已弃用，推荐使用直接解包的 initramfs。
 */

/* 映射后 initrd 内存区间 [start,end)，由架构早期启动代码设置并由解包路径读取。 */
unsigned long initrd_start, initrd_end;
/* 非零表示架构允许 initrd 位于通常内核起始地址以下；只表达布局许可。 */
int initrd_below_start_ok;
/* 1 表示尝试装载 legacy initrd；`noinitrd` 在启动期将它清零。 */
static int __initdata mount_initrd = 1;

/* bootloader/early 参数给出的 initrd 物理起址，仅在启动期转换和保留。 */
phys_addr_t phys_initrd_start __initdata;
/* 从物理起址开始的 initrd 字节数；0 表示没有有效显式区域。 */
unsigned long phys_initrd_size __initdata;

/*
 * no_initrd() - 处理弃用的无值 `noinitrd` 参数并禁止 ram0 装载
 *
 * @str 是命令行解析器提供的借用后缀，本实现不读取其内容；函数在单线程
 * __init 上下文不持锁、不睡眠。它打印弃用告警、清零 mount_initrd，并返回
 * 1 表示参数已消费。镜像临时文件仍由 initrd_load() 删除，不转移 ownership。
 */
static int __init no_initrd(char *str)
{
	pr_warn("noinitrd option is deprecated and will be removed soon\n");
	mount_initrd = 0;
	return 1;
}

__setup("noinitrd", no_initrd);

/*
 * early_initrdmem() - 解析 `initrdmem=<物理起址>,<字节数>`
 *
 * @p 是借用 NUL 文本，不可为 NULL。函数在 early_param 阶段不持锁、不睡眠；
 * memparse() 允许 K/M/G 等大小后缀。只有第一个值后紧跟逗号才解析并发布两个
 * 全局字段；缺逗号时保持旧值。函数始终返回 0（early_param 的成功约定），
 * 不报告第二项无效，也不取得输入 ownership。
 */
static int __init early_initrdmem(char *p)
{
	/* start/size 是解析后的物理地址与字节数；endp 指向首个值后的未消费字符。 */
	phys_addr_t start;
	unsigned long size;
	char *endp;

	/* 先解析起址，只有显式逗号才能把这组值作为完整区域提交。 */
	start = memparse(p, &endp);
	if (*endp == ',') {
		size = memparse(endp + 1, NULL);

		/* 两个字段一起发布，避免只更新起址却沿用旧长度。 */
		phys_initrd_start = start;
		phys_initrd_size = size;
	}
	return 0;
}
early_param("initrdmem", early_initrdmem);

/*
 * early_initrd() - 为 `initrd=` 提供与 `initrdmem=` 相同的早期解析别名
 *
 * @p 是借用参数文本；本 wrapper 不改变 ownership、锁或睡眠属性，直接返回
 * early_initrdmem() 的 0。保留独立 handler 使两个启动参数名共享唯一语义。
 */
static int __init early_initrd(char *p)
{
	return early_initrdmem(p);
}
early_param("initrd", early_initrd);

/*
 * initrd_load() - 可选地把 legacy initrd 镜像装入 ram0 并清理临时入口
 *
 * 由 prepare_namespace() 在根设备解析后调用；无入参和直接返回值，运行于
 * 可睡眠的进程 1 __init 上下文。mount_initrd 非零时，先把 `/dev/ram` 指向
 * Root_RAM0，再由 rd_load_image() 打开 `/initrd.image`、识别/解压并写入。
 * create_dev() 的错误未单独处理，最终装载结果由 rd_load_image() 决定；返回
 * 1 表示成功并触发弃用告警。无论禁用或失败，最后都 unlink 临时镜像路径，
 * 因而该函数不是可重试事务，且不会把文件/缓冲区 ownership 留给调用者。
 */
void __init initrd_load(void)
{
	/* noinitrd 只跳过设备创建和装载，仍执行共同的临时文件清理。 */
	if (mount_initrd) {
		create_dev("/dev/ram", Root_RAM0);
		/*
		 * Load the initrd data into /dev/ram0.
		 */
		/* 把 initrd 数据装入 `/dev/ram0`；helper 内部处理压缩和所有资源释放。 */
		if (rd_load_image()) {
			pr_warn("using deprecated initrd support, will be removed in January 2027; "
				"use initramfs instead or (as a last resort) /sys/firmware/initrd; "
				"see section \"Workaround\" in "
				"https://lore.kernel.org/lkml/20251010094047.3111495-1-safinaskar@gmail.com\n");
		}
	}
	/* 摘除 initramfs/rootfs 中的暂存入口，成功与否都不再允许重复消费。 */
	init_unlink("/initrd.image");
}
