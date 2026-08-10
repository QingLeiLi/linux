// SPDX-License-Identifier: GPL-2.0-only
/*
 * Architecture specific sysfs attributes in /sys/kernel
 *
 * Copyright (C) 2007, Intel Corp.
 *      Huang Ying <ying.huang@intel.com>
 * Copyright (C) 2013, 2013 Red Hat, Inc.
 *      Dave Young <dyoung@redhat.com>
 */
/*
 * x86 在通用 ksysfs_init() 已建立的 kernel_kobj 下发布 /sys/kernel/boot_params。
 * version/data 暴露启动协议头和完整 boot_params 快照，setup_data/N/{type,data}
 * 则把引导程序传入的物理地址链表转换为可枚举的 sysfs 层级。
 *
 * setup_data 节点仍驻留在早期物理内存中，读取时必须临时 memremap()，不能把
 * 物理地址直接当普通内核虚拟指针使用。初始化函数只建立元数据和记录文件大小；
 * 真正内容在用户读取时重新映射，因此注释重点是映射生命周期、边界校验和失败
 * 时对已经发布的 kobject/sysfs 节点进行逆序回滚。
 */

#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/init.h>
#include <linux/stat.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/io.h>

#include <asm/setup.h>

/*
 * version_show() - 输出引导协议头 hdr.version 的十六进制版本号。
 * @kobj 是借用的 boot_params kobject，@attr 是借用的 version 属性，@buf 为
 * sysfs 输出缓冲区；返回写入字节数。boot_params 是启动后保留的全局快照，
 * 本回调只读、无 ownership 转移，也不需要映射物理 setup_data。
 */
static ssize_t version_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%04x\n", boot_params.hdr.version);
}

/* 将 version_show 绑定为 /sys/kernel/boot_params/version 只读文本属性。 */
static struct kobj_attribute boot_params_version_attr = __ATTR_RO(version);

/*
 * boot_params_data_read() - 按 sysfs 已校验的区间复制原始 struct boot_params。
 * @fp/@kobj/@bin_attr 均为借用的读取上下文；@buf 是输出缓冲区；@off 和 @count
 * 是相对 boot_params 起点的字节偏移与长度。binary attribute 的 .size 已设为
 * sizeof(boot_params)，sysfs 核心会裁剪请求，本函数成功返回实际 @count。
 * 数据来自常驻全局对象，不分配、不睡眠、不转移所有权。
 */
static ssize_t boot_params_data_read(struct file *fp, struct kobject *kobj,
				     const struct bin_attribute *bin_attr,
				     char *buf, loff_t off, size_t count)
{
	memcpy(buf, (void *)&boot_params + off, count);
	return count;
}

/*
 * /sys/kernel/boot_params/data 的静态描述符：只读、固定为完整 boot_params 大小，
 * read 回调只需处理经核心裁剪后的窗口。
 */
static const struct bin_attribute boot_params_data_attr = {
	.attr = {
		.name = "data",
		.mode = S_IRUGO,
	},
	.read = boot_params_data_read,
	.size = sizeof(boot_params),
};

/* 文本属性数组以 NULL 结尾；元素借用静态 attribute 的全生命周期。 */
static struct attribute *boot_params_version_attrs[] = {
	&boot_params_version_attr.attr,
	NULL,
};

/* 二进制属性数组同样以 NULL 结尾，并通过 const 防止初始化后替换成员。 */
static const struct bin_attribute *const boot_params_data_attrs[] = {
	&boot_params_data_attr,
	NULL,
};

/* 把 boot_params 的 version 文本文件和 data 二进制文件作为一个组发布/撤销。 */
static const struct attribute_group boot_params_attr_group = {
	.attrs = boot_params_version_attrs,
	.bin_attrs = boot_params_data_attrs,
};

/*
 * kobj_to_setup_data_nr() - 把 setup_data 子目录名转换为链表序号。
 * @kobj 是名为十进制 N 的借用对象；@nr 是调用者提供的输出地址，成功后得到
 * 从零开始的索引。返回 0 或 kstrtoint() 的负 errno；不取得 kobject 引用，
 * 不修改目录。sysfs 层级保证回调期间 @kobj 存活。
 */
static int kobj_to_setup_data_nr(struct kobject *kobj, int *nr)
{
	const char *name;

	name = kobject_name(kobj);
	return kstrtoint(name, 10, nr);
}

/*
 * get_setup_data_paddr() - 沿启动参数的物理链表查找第 @nr 个 setup_data 头。
 * @nr 是从零开始且应为非负的输入；@paddr 是仅在成功时写入的物理地址输出。
 * 返回 0、映射失败的 -ENOMEM，或越过链尾的 -EINVAL。函数可睡眠；每轮只映射
 * 一个固定大小的头，读出 next 后立即 memunmap()，因此不把映射指针泄露给
 * 调用者，也不持有跨迭代资源。链表由启动固件/bootloader 构造并在此只读。
 */
static int get_setup_data_paddr(int nr, u64 *paddr)
{
	int i = 0;
	struct setup_data *data;
	u64 pa_data = boot_params.hdr.setup_data;

	while (pa_data) {
		/* 命中时返回的是物理地址值，此时尚未建立任何需调用者释放的映射。 */
		if (nr == i) {
			*paddr = pa_data;
			return 0;
		}
		/* 临时映射当前头，只借用到本轮读出 next；失败时没有遗留映射。 */
		data = memremap(pa_data, sizeof(*data), MEMREMAP_WB);
		if (!data)
			return -ENOMEM;

		pa_data = data->next;
		memunmap(data);
		i++;
	}
	return -EINVAL;
}

/*
 * get_setup_data_size() - 计算第 @nr 个节点应向 sysfs 暴露的数据字节数。
 * @size 是成功时的输出。普通节点使用 data->len；SETUP_INDIRECT 节点需要先把
 * 含间接描述符的整个外层记录映射出来，再选择真实 payload 的 indirect->len；
 * 若间接描述符自身仍声称 SETUP_INDIRECT（未定义嵌套），则退回暴露外层 data。
 * 返回 0、-ENOMEM 或 -EINVAL，所有临时映射在返回前释放。仅供 init 阶段调用。
 */
static int __init get_setup_data_size(int nr, size_t *size)
{
	/*
	 * 变量地图：pa_data/pa_next 是当前与下一节点物理地址；data 是当前临时映射；
	 * indirect 指向 data->data 内、有效期不超过 data 映射；i 是零基位置；len 是
	 * 为读取柔性区而计算的外层映射字节数。
	 */
	u64 pa_data = boot_params.hdr.setup_data, pa_next;
	struct setup_indirect *indirect;
	struct setup_data *data;
	int i = 0;
	u32 len;

	while (pa_data) {
		/* 先映射固定头并保存 next，保证稍后解除映射后仍能继续遍历。 */
		data = memremap(pa_data, sizeof(*data), MEMREMAP_WB);
		if (!data)
			return -ENOMEM;
		pa_next = data->next;

		if (nr == i) {
			if (data->type == SETUP_INDIRECT) {
				/* data[] 柔性区中保存 setup_indirect，需按外层 len 扩大映射。 */
				len = sizeof(*data) + data->len;
				memunmap(data);
				data = memremap(pa_data, len, MEMREMAP_WB);
				if (!data)
					return -ENOMEM;

				indirect = (struct setup_indirect *)data->data;

				if (indirect->type != SETUP_INDIRECT)
					*size = indirect->len;
				else
					*size = data->len;
			} else {
				*size = data->len;
			}

			memunmap(data);
			return 0;
		}

		pa_data = pa_next;
		memunmap(data);
		i++;
	}
	return -EINVAL;
}

/*
 * type_show() - 输出对应 setup_data 记录的有效类型。
 * @kobj 的目录名决定链表索引；@attr 为借用属性，@buf 为输出。普通记录输出
 * data->type；间接记录完整映射后输出 indirect->type。返回写入字节数，或名称
 * 解析、索引查找、memremap 的负 errno。所有物理映射均在返回前解除。
 */
static ssize_t type_show(struct kobject *kobj,
			 struct kobj_attribute *attr, char *buf)
{
	/*
	 * nr 是从目录名恢复的索引，paddr 是对应记录物理地址；data/indirect 只在
	 * memremap 窗口内有效；len 是间接记录外层映射长度；ret 同时承载 errno
	 * 与最终 sprintf 字节数。
	 */
	struct setup_indirect *indirect;
	struct setup_data *data;
	int nr, ret;
	u64 paddr;
	u32 len;

	ret = kobj_to_setup_data_nr(kobj, &nr);
	if (ret)
		return ret;

	ret = get_setup_data_paddr(nr, &paddr);
	if (ret)
		return ret;
	data = memremap(paddr, sizeof(*data), MEMREMAP_WB);
	if (!data)
		return -ENOMEM;

	/* 间接类型描述符位于变长 data[] 中，固定头映射不足以安全解引用。 */
	if (data->type == SETUP_INDIRECT) {
		len = sizeof(*data) + data->len;
		memunmap(data);
		data = memremap(paddr, len, MEMREMAP_WB);
		if (!data)
			return -ENOMEM;

		indirect = (struct setup_indirect *)data->data;

		ret = sprintf(buf, "0x%x\n", indirect->type);
	} else {
		ret = sprintf(buf, "0x%x\n", data->type);
	}

	memunmap(data);
	return ret;
}

/*
 * setup_data_data_read() - 读取第 N 个 setup_data 的 payload 窗口。
 *
 * @fp 是本次打开文件的借用上下文；@kobj 名称提供零基索引；@bin_attr 是借用
 * 的共享 data 属性描述符；@buf 是输出；@off/@count 是请求的字节偏移与长度。
 * 所有参数 ownership 均不转移。普通记录跳过 setup_data 头后读取 data[]；有效的
 * SETUP_INDIRECT 改从 indirect->addr/len 读取真实 payload；未定义的嵌套 indirect
 * 为便于诊断而按普通外层 data[] 暴露。成功返回实际复制字节数（EOF 可为 0），
 * 越界返回 -EINVAL，解析或映射失败返回负 errno。函数可睡眠，且每个 memremap
 * 都在 out 路径配对 memunmap，不向 sysfs 核心转移映射 ownership。
 */
static ssize_t setup_data_data_read(struct file *fp,
				    struct kobject *kobj,
				    const struct bin_attribute *bin_attr,
				    char *buf,
				    loff_t off, size_t count)
{
	/*
	 * nr/paddr 定位记录；data 始终表示最终需在 out 解除的头或外层记录映射；
	 * indirect 借用 data 柔性区；len 是有效 payload 总字节数；p 是仅覆盖复制
	 * 阶段的 payload 映射；ret 初值 0 同时表示合法 EOF。
	 */
	struct setup_indirect *indirect;
	struct setup_data *data;
	int nr, ret = 0;
	u64 paddr, len;
	void *p;

	ret = kobj_to_setup_data_nr(kobj, &nr);
	if (ret)
		return ret;

	ret = get_setup_data_paddr(nr, &paddr);
	if (ret)
		return ret;
	data = memremap(paddr, sizeof(*data), MEMREMAP_WB);
	if (!data)
		return -ENOMEM;

	/* 阶段 1：解析记录布局，得出真正 payload 的物理起点和总长度。 */
	if (data->type == SETUP_INDIRECT) {
		len = sizeof(*data) + data->len;
		memunmap(data);
		data = memremap(paddr, len, MEMREMAP_WB);
		if (!data)
			return -ENOMEM;

		indirect = (struct setup_indirect *)data->data;

		if (indirect->type != SETUP_INDIRECT) {
			paddr = indirect->addr;
			len = indirect->len;
		} else {
			/*
			 * Even though this is technically undefined, return
			 * the data as though it is a normal setup_data struct.
			 * This will at least allow it to be inspected.
			 */
			/*
			 * 虽然嵌套 SETUP_INDIRECT 在协议上未定义，仍把它当普通 setup_data
			 * 的 data[] 返回，至少允许诊断工具检查原始字节，而不是拒绝读取。
			 */
			paddr += sizeof(*data);
			len = data->len;
		}
	} else {
		paddr += sizeof(*data);
		len = data->len;
	}

	/* 阶段 2：验证并裁剪窗口。off==len 是合法 EOF，off>len 才是无效请求。 */
	if (off > len) {
		ret = -EINVAL;
		goto out;
	}

	if (count > len - off)
		count = len - off;

	if (!count)
		goto out;

	/* 阶段 3：只映射 payload 区并复制本次窗口；ret 预置为成功字节数。 */
	ret = count;
	p = memremap(paddr, len, MEMREMAP_WB);
	if (!p) {
		ret = -ENOMEM;
		goto out;
	}
	memcpy(buf, p + off, count);
	memunmap(p);
out:
	/* 无论 payload 映射是否成功，头/外层记录映射都由此统一释放。 */
	memunmap(data);
	return ret;
}

/* 每个数字节点复用同一个 type 属性描述符，具体对象由回调的 kobj 区分。 */
static struct kobj_attribute type_attr = __ATTR_RO(type);

/*
 * 每个 setup_data/N/data 共用此描述符；create_setup_data_node() 在逐节点发布前
 * 更新 .size。初始化是串行的，sysfs 在创建文件时取得该时刻的大小元数据。
 */
static struct bin_attribute data_attr __ro_after_init = {
	.attr = {
		.name = "data",
		.mode = S_IRUGO,
	},
	.read = setup_data_data_read,
};

/* 数字节点的文本属性集合，仅包含 type，NULL 为终止哨兵。 */
static struct attribute *setup_data_type_attrs[] = {
	&type_attr.attr,
	NULL,
};

/* 数字节点的二进制属性集合，仅包含 payload data 文件。 */
static const struct bin_attribute *const setup_data_data_attrs[] = {
	&data_attr,
	NULL,
};

/* type 与 data 必须作为同一数字节点的一个组一同创建或撤销。 */
static const struct attribute_group setup_data_attr_group = {
	.attrs = setup_data_type_attrs,
	.bin_attrs = setup_data_data_attrs,
};

/*
 * create_setup_data_node() - 创建 /setup_data/@nr 并发布其 type/data 文件。
 * @parent 是借用的 setup_data 根；@kobjp 是成功时返回所持 kobject 指针的输出，
 * 其引用 ownership 转给调用者；@nr 为零基索引。返回 0、-ENOMEM 或下游 errno。
 * 失败时本函数 put 已创建对象且不写 @kobjp；成功后调用者负责在批量回滚时调用
 * cleanup_setup_data_node()。函数仅在 init 进程上下文运行，可以睡眠。
 */
static int __init create_setup_data_node(struct kobject *parent,
					 struct kobject **kobjp, int nr)
{
	/*
	 * size 是本节点 payload 边界；kobj 是创建后由本函数暂持的数字目录引用；
	 * name 只在 kobject 创建时作为临时十进制名称；ret 原样携带下游 errno。
	 */
	int ret = 0;
	size_t size;
	struct kobject *kobj;
	char name[16]; /* should be enough for setup_data nodes numbers */
	/* 16 字节足以容纳 setup_data 节点序号的十进制文本及 NUL；名称只在创建时借用。 */
	snprintf(name, 16, "%d", nr);

	kobj = kobject_create_and_add(name, parent);
	if (!kobj)
		return -ENOMEM;

	/* 先确定边界，再发布文件，避免用户看到 size 尚未初始化的 data 属性。 */
	ret = get_setup_data_size(nr, &size);
	if (ret)
		goto out_kobj;

	data_attr.size = size;
	ret = sysfs_create_group(kobj, &setup_data_attr_group);
	if (ret)
		goto out_kobj;
	*kobjp = kobj;

	return 0;
out_kobj:
	/* 组尚未成功发布或已由创建失败内部撤销；put 数字目录的初始引用。 */
	kobject_put(kobj);
	return ret;
}

/*
 * cleanup_setup_data_node() - 撤销一个已成功发布的数字节点。
 * @kobj 是 create_setup_data_node() 转移给批量创建者的持有引用，不可为 NULL。
 * 先摘除属性组阻止新访问，再 put 目录；返回无直接值，调用后指针失效。
 */
static void __init cleanup_setup_data_node(struct kobject *kobj)
{
	sysfs_remove_group(kobj, &setup_data_attr_group);
	kobject_put(kobj);
}

/*
 * get_setup_data_total_num() - 统计从 @pa_data 开始的物理 setup_data 链节点数。
 * @pa_data 为首节点物理地址，零代表空链；@nr 为输出计数，入口即清零，失败时
 * 可能已包含当前无法映射的节点，调用者不得使用。返回 0 或 -ENOMEM。每轮映射固定
 * 头、读取 next 后立即解除，不保留物理映射。
 */
static int __init get_setup_data_total_num(u64 pa_data, int *nr)
{
	int ret = 0;
	struct setup_data *data;

	*nr = 0;
	while (pa_data) {
		*nr += 1;
		data = memremap(pa_data, sizeof(*data), MEMREMAP_WB);
		if (!data) {
			ret = -ENOMEM;
			goto out;
		}
		pa_data = data->next;
		memunmap(data);
	}

out:
	return ret;
}

/*
 * create_setup_data_nodes() - 按启动链表创建 setup_data 根及全部数字子节点。
 * @parent 是借用的 boot_params kobject。空链直接成功且不创建 setup_data 目录。
 * 成功返回 0，并把创建对象的长寿命引用留给 sysfs 层级；失败返回负 errno，按
 * 已完成顺序的逆序撤销所有数字节点、指针数组及根目录，不留下部分层级。
 */
static int __init create_setup_data_nodes(struct kobject *parent)
{
	/*
	 * setup_data_kobj 是批量节点父目录的持有引用；kobjp 暂存每个成功子节点的
	 * 持有指针以支持回滚；nr 是总数，i 是当前创建位置，j 用于逆序清理，ret
	 * 保存首个失败 errno。pa_data 只用于判断链表是否为空并传给计数阶段。
	 */
	struct kobject *setup_data_kobj, **kobjp;
	u64 pa_data;
	int i, j, nr, ret = 0;

	/* 阶段 1：空链是正常配置；非空时先建立批量创建的父目录。 */
	pa_data = boot_params.hdr.setup_data;
	if (!pa_data)
		return 0;

	setup_data_kobj = kobject_create_and_add("setup_data", parent);
	if (!setup_data_kobj) {
		ret = -ENOMEM;
		goto out;
	}

	/* 阶段 2：先计数再一次分配引用数组，便于任意中途失败时精确回滚。 */
	ret = get_setup_data_total_num(pa_data, &nr);
	if (ret)
		goto out_setup_data_kobj;

	kobjp = kmalloc_objs(*kobjp, nr);
	if (!kobjp) {
		ret = -ENOMEM;
		goto out_setup_data_kobj;
	}

	/* 阶段 3：按稳定链表顺序发布 0..nr-1；kobjp[0..i-1] 均为持有引用。 */
	for (i = 0; i < nr; i++) {
		ret = create_setup_data_node(setup_data_kobj, kobjp + i, i);
		if (ret)
			goto out_clean_nodes;
	}

	kfree(kobjp);
	return 0;

out_clean_nodes:
	/* 第 i 个失败，逆序清理此前成功的 0..i-1，避免遗留可见的半成品目录。 */
	for (j = i - 1; j >= 0; j--)
		cleanup_setup_data_node(*(kobjp + j));
	kfree(kobjp);
out_setup_data_kobj:
	/* 数字节点已全部不存在，最后释放 setup_data 根的初始引用。 */
	kobject_put(setup_data_kobj);
out:
	return ret;
}

/*
 * boot_params_ksysfs_init() - 在 /sys/kernel 下发布 x86 启动参数视图。
 *
 * 由 arch_initcall 调用，时间晚于 do_basic_setup() 创建 kernel_kobj；运行在可
 * 睡眠的启动期进程上下文，不持锁。入参：无。成功返回 0，boot_params 组与
 * 可选 setup_data 树对外可见；失败返回负 errno 给 initcall 框架记录，并逆序
 * 撤销本函数已发布的组和对象。kernel_kobj 只是借用，绝不由本函数 put。
 */
static int __init boot_params_ksysfs_init(void)
{
	/* ret 贯穿各发布阶段保存 errno；boot_params_kobj 是成功创建后暂由本函数持有的引用。 */
	int ret;
	struct kobject *boot_params_kobj;

	/* 阶段 1：在通用根下创建体系结构目录并取得其初始引用。 */
	boot_params_kobj = kobject_create_and_add("boot_params",
						  kernel_kobj);
	if (!boot_params_kobj) {
		ret = -ENOMEM;
		goto out;
	}

	/* 阶段 2：先发布固定 boot_params 文件，再构造依赖物理链表的可选子树。 */
	ret = sysfs_create_group(boot_params_kobj, &boot_params_attr_group);
	if (ret)
		goto out_boot_params_kobj;

	ret = create_setup_data_nodes(boot_params_kobj);
	if (ret)
		goto out_create_group;

	return 0;
out_create_group:
	/* setup_data 构造失败时，先摘除已发布的 version/data 组。 */
	sysfs_remove_group(boot_params_kobj, &boot_params_attr_group);
out_boot_params_kobj:
	/* 释放 boot_params 目录初始引用；父 kernel_kobj 的引用不属于本函数。 */
	kobject_put(boot_params_kobj);
out:
	return ret;
}

arch_initcall(boot_params_ksysfs_init);
