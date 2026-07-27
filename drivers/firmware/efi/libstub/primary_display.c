// SPDX-License-Identifier: GPL-2.0
/*
 * EFI stub 主显示信息交接缓冲区学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * EFI stub 在正式内核启动前通过 GOP 收集 framebuffer 的 screen_info，
 * 并在启用 CONFIG_FIRMWARE_EDID 时一并保存显示器 EDID。本文件实现其中的
 * “EFI configuration table”交接方式：分配一个能跨越 ExitBootServices
 * 交给内核的 sysfb_display_info，把它注册到 Linux 私有 GUID 下，并在
 * 启动失败时撤销发布、释放 EFI pool。
 *
 * 主调用链：
 *
 *   setup_primary_display()
 *       -> alloc_primary_display()
 *           -> __alloc_primary_display()        配置表方案
 *       -> efi_setup_graphics()                  填充 screen/EDID
 *       -> efi_boot_kernel()
 *           -> EFI init 按 GUID 找到物理表
 *           -> init_primary_display() 复制到 sysfb_primary_display
 *
 * 成功跳入内核后 EFI stub 不再返回，内核复制内容并由 EFI 内存生命周期回收
 * EFI_ACPI_RECLAIM_MEMORY。若图形探测或启动失败而返回 stub，
 * free_primary_display() 先从 configuration table 摘除指针，再释放 pool。
 *
 * 另一方案由 efi-stub-entry.c 直接取得已重定位内核镜像中的
 * sysfb_primary_display，省去配置表复制并让 earlycon 更早可用；这种构建
 * 使用 efi-stub.c 的 weak 空释放回调。本文件的强分配/释放实现由 zboot 和
 * 需要独立交接缓冲区的构建按链接依赖拉入。
 *
 * 本协议运行在 EFI Boot Services 尚有效的单线程 stub 阶段，没有 Linux
 * 锁、RCU 或引用计数。configuration table 的安装是跨 stub/内核的发布边界；
 * 其代价是额外 pool 分配、GUID 查找和内核侧复制。
 */

#include <linux/efi.h>
#include <linux/sysfb.h>

#include <asm/efi.h>

#include "efistub.h"

/*
 * There are two ways of populating the core kernel's sysfb_primary_display
 * via the stub:
 *
 *   - using a configuration table, which relies on the EFI init code to
 *     locate the table and copy the contents; or
 *
 *   - by linking directly to the core kernel's copy of the global symbol.
 *
 * The latter is preferred because it makes the EFIFB earlycon available very
 * early, but it only works if the EFI stub is part of the core kernel image
 * itself. The zboot decompressor can only use the configuration table
 * approach.
 */
/*
 * EFI stub 有两种方式填充核心内核的 sysfb_primary_display：
 *
 * 一是注册 configuration table，由内核 EFI 初始化代码按 GUID 找到表并
 * 复制内容；二是链接到核心内核中全局对象的实际副本并直接写入。直接写入
 * 能让 EFIFB earlycon 在更早阶段使用显示信息，因此优先采用，但要求 stub
 * 本身位于同一核心内核镜像中。zboot 解压器与被解压内核是不同镜像，只能
 * 通过 configuration table 完成交接。
 *
 * 两种方案向后续 efi_setup_graphics() 暴露相同的
 * struct sysfb_display_info *，差异集中在对象的分配、发布和释放责任。
 */

/*
 * Linux 私有 primary-display configuration table 的稳定类型标识。
 * stub 用它发布缓冲区，内核侧 efi_config_parse_tables() 用同一 GUID 把
 * 表地址写入 primary_display_table。变量在整个 stub 生命周期只读；未声明
 * const 是因为 EFI Boot Services 接口接收可写 GUID 指针。
 */
static efi_guid_t primary_display_guid = LINUX_EFI_PRIMARY_DISPLAY_TABLE_GUID;

/*
 * __alloc_primary_display() - 分配并发布配置表方式的主显示交接对象。
 *
 * 调用关系：zboot 的 alloc_primary_display() 无条件进入这里；普通 generic
 * stub 在不能直接链接内核全局对象的配置（例如 ARM32）下进入这里。返回的
 * dpy 随后由 setup_primary_display() 借给 efi_setup_graphics() 填充。
 *
 * 入参：无。入口要求 EFI Boot Services 仍有效；不持 Linux 锁，运行于 EFI
 * stub 的串行启动上下文。allocate_pool()/install_configuration_table()
 * 都是固件调用，不能在 ExitBootServices 之后使用。
 *
 * 成功返回非 NULL 的可写借用指针：底层 pool ownership 仍属于本交接协议，
 * 调用者必须在启动失败返回时交给 free_primary_display()，不能直接
 * free_pool。对象已以 primary_display_guid 发布，但此时内容仅完成清零，
 * efi_setup_graphics() 随后填充；内核读者直到 stub 完成并跳转后才会出现，
 * 因此不会并发观察半初始化内容。
 *
 * pool 分配失败或配置表安装失败均返回 NULL。前者没有资源，后者在返回前
 * 释放刚取得的 pool；失败不会留下可发现的 configuration table。
 */
struct sysfb_display_info *__alloc_primary_display(void)
{
	/*
	 * 变量地图：
	 *   dpy    EFI pool 中的交接对象；allocate_pool 成功后由本函数负责回滚。
	 *   status 最近一次 Boot Services 调用的 EFI 状态，只以 EFI_SUCCESS
	 *          作为提交条件，其他状态统一收敛为 NULL 接口。
	 */
	struct sysfb_display_info *dpy;
	efi_status_t status;

	/*
	 * 阶段 1：取得能存活到内核早期解析阶段的物理存储。
	 *
	 * EFI_ACPI_RECLAIM_MEMORY 在退出 Boot Services 后不会立即像普通 Boot
	 * Services Data 那样失去交接保证，内核可先按 configuration table 地址
	 * 映射、复制，再在后续内存初始化中回收。void ** 是 EFI ABI 的通用输出
	 * 参数；成功前不得读取 dpy。
	 */
	status = efi_bs_call(allocate_pool, EFI_ACPI_RECLAIM_MEMORY,
			     sizeof(*dpy), (void **)&dpy);

	/* 分配失败时 dpy 没有有效 ownership，也没有 configuration table。 */
	if (status != EFI_SUCCESS)
		return NULL;

	/*
	 * 阶段 2：先建立确定的空快照。这样即使某些 GOP/EDID 字段未被后续探测
	 * 填充，内核也只会看到零值“未知/不可用”，不会泄露 pool 中的旧内容。
	 */
	memset(dpy, 0, sizeof(*dpy));

	/*
	 * 阶段 3：把指针安装进 EFI System Table 的 configuration table 数组。
	 *
	 * 成功是本函数的发布边界：固件表开始记录到 dpy 的可发现指针，但 pool
	 * 的释放责任并未转给固件。后续不能只 free_pool，必须先用同一 GUID
	 * 安装 NULL 将其摘除。
	 */
	status = efi_bs_call(install_configuration_table,
			     &primary_display_guid, dpy);
	if (status == EFI_SUCCESS)
		return dpy;

	/*
	 * 安装失败说明指针从未成功发布；当前函数仍独占 pool 的释放责任，可直接
	 * 回滚。free_pool 的状态在这里无法进一步恢复，接口仍以 NULL 告知上层
	 * 放弃主显示信息，不阻止整个内核启动。
	 */
	efi_bs_call(free_pool, dpy);
	return NULL;
}

/*
 * free_primary_display() - 回滚配置表交接对象的发布和存储。
 *
 * @dpy：__alloc_primary_display() 成功返回的 pool 指针，或 NULL。它是输入
 * ownership token：非 NULL 时调用者交还撤销发布与释放责任；函数返回后
 * 指针失效，调用者不得再访问或重复释放。
 *
 * 调用者是 setup_primary_display() 的图形初始化失败路径，以及
 * efi_stub_common() 在 efi_boot_kernel() 失败返回后的统一清理。正常成功
 * 跳入内核不会返回到这条路径；内核侧已经自行复制 configuration table
 * 内容。
 *
 * 入口必须仍处于 EFI Boot Services 可调用阶段，不持 Linux 锁。返回类型为
 * void：NULL 是幂等空操作；非 NULL 时先摘除全局可发现指针，再释放 pool。
 * 固件调用的错误无法向当前接口上传播，因此该函数是 best-effort cleanup。
 */
void free_primary_display(struct sysfb_display_info *dpy)
{
	/* alloc 失败或当前构建没有主显示对象时，统一清理路径无需另设分支。 */
	if (!dpy)
		return;

	/*
	 * 释放顺序不可交换：先以同一 GUID 安装 NULL，从 EFI configuration
	 * table 摘除 dpy；若先 free_pool，表中会暂时或永久保留悬空地址，后续
	 * EFI 初始化代码可能把已释放内存当作 sysfb_display_info。
	 */
	efi_bs_call(install_configuration_table, &primary_display_guid, NULL);
	efi_bs_call(free_pool, dpy);
}
