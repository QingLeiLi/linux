// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ACPI 总线核心学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件把 ACPICA 提供的固件命名空间和 AML 执行能力接入 Linux 设备模型。
 * 它负责读取 _STA、执行 _OSC 能力/控制权协商、把固件 Notify 转换为热插拔
 * 或驱动回调、完成 ACPI 设备与驱动匹配，并按“早期核心 -> 解释器 -> 总线与
 * 扫描”的顺序初始化 ACPI。具体设备枚举、资源解析和 acpi_device 创建主要
 * 位于 scan.c；电池、处理器、EC、PCI 等功能由各自驱动负责。
 *
 * 主路径：start_kernel() -> acpi_early_init() -> acpi_subsystem_init()；随后
 * subsys_initcall(acpi_init) -> acpi_bus_init() -> acpi_scan_init()，最终由驱动
 * 核心的 match/probe/remove 回调完成绑定与解绑。
 *
 * acpi_handle 是 ACPICA 命名空间节点句柄；struct acpi_device 是扫描层发布到
 * acpi_bus_type 的 Linux 包装，普通 struct device 可通过 companion 借用它。
 * ACPICA 以 ACPI_ALLOCATE_BUFFER 返回的对象必须用 ACPI_FREE() 回收；驱动绑定
 * 成功后总线额外持有一次 device 引用，remove 时对称释放。
 *
 * 并发上，驱动核心串行化同一设备的 probe/remove，物理伴生节点链表由
 * physical_node_lock 保护；ACPICA 通知可异步到达，移除 handler 后必须等待
 * 已排队事件结束。热插拔被投递给工作队列，避免在固件通知上下文直接重建
 * 设备树。分层和保守协商避免 OS 接管未获固件授权的功能，代价是初始化次序、
 * 通知生命周期和固件缺陷回退路径较复杂。
 */
/*
 *  acpi_bus.c - ACPI Bus Driver ($Revision: 80 $)
 *
 *  Copyright (C) 2001, 2002 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 */

/* 让本文件所有 pr_* 日志自动带 "ACPI: " 前缀，便于从启动日志定位子系统。 */
#define pr_fmt(fmt) "ACPI: " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/pm.h>
#include <linux/device.h>
#include <linux/proc_fs.h>
#include <linux/acpi.h>
#include <linux/slab.h>
#include <linux/regulator/machine.h>
#include <linux/workqueue.h>
#include <linux/reboot.h>
#include <linux/delay.h>
#ifdef CONFIG_X86
#include <asm/mpspec.h>
#include <linux/dmi.h>
#endif
#include <linux/acpi_viot.h>
#include <linux/pci.h>
#include <acpi/apei.h>
#include <linux/suspend.h>
#include <linux/prmt.h>

#include "internal.h"

/*
 * acpi_root 是命名空间根节点的 acpi_device，由扫描层创建和维护；这里提供
 * 全局入口。acpi_root_dir 是 acpi_bus_init() 创建的兼容性 procfs 根，供仍
 * 使用 /proc/acpi 的子模块挂接。两者在初始化后长期存在，运行期不反复替换。
 */
struct acpi_device *acpi_root;
struct proc_dir_entry *acpi_root_dir;
EXPORT_SYMBOL(acpi_root_dir);

#ifdef CONFIG_X86
#ifdef CONFIG_ACPI_CUSTOM_DSDT
/*
 * 已编入自定义 DSDT 时无需“复制固件 DSDT”规避。@id 是 DMI 框架传入的借用
 * 指针，函数不保存它；返回 0 允许框架继续匹配。
 */
static inline int set_copy_dsdt(const struct dmi_system_id *id)
{
	return 0;
}
#else
/*
 * DMI 命中已知机型后要求 ACPICA 在解析前复制 DSDT，避免固件表所在内存随后
 * 被改写。@id 只在回调期间有效；启动期写入全局开关时尚无并发解释器读者。
 */
static int set_copy_dsdt(const struct dmi_system_id *id)
{
	pr_notice("%s detected - force copy of DSDT to local memory\n", id->ident);
	acpi_gbl_copy_dsdt_locally = 1;
	return 0;
}
#endif

static const struct dmi_system_id dsdt_dmi_table[] __initconst = {
	/*
	 * Invoke DSDT corruption work-around on all Toshiba Satellite.
	 * https://bugzilla.kernel.org/show_bug.cgi?id=14679
	 */
	/*
	 * 对所有 Toshiba Satellite 启用 DSDT 损坏规避。该表带 __initconst，
	 * 启动匹配结束后可随 init 段一起回收。
	 */
	{
	 .callback = set_copy_dsdt,
	 .ident = "TOSHIBA Satellite",
	 .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "TOSHIBA"),
		DMI_MATCH(DMI_PRODUCT_NAME, "Satellite"),
		},
	},
	{}
};
#endif

/* --------------------------------------------------------------------------
                                Device Management
   -------------------------------------------------------------------------- */

/*
 * acpi_bus_get_status_handle() - 读取命名空间对象的标准 _STA 位图。
 *
 * @handle 是调用期间有效的 ACPICA 句柄；@sta 是必需输出，成功时写入设备的
 * PRESENT/ENABLED/UI/FUNCTIONING 等位。函数可能执行 AML，调用环境必须允许
 * 睡眠且解释器已经可用；本函数不取得 handle 的所有权。
 *
 * _STA 不存在按规范等价于设备存在、启用、可见且正常，因此合成默认位图并
 * 返回 AE_OK。其他 ACPICA 错误原样返回，此时调用者不得使用 @sta。
 */
acpi_status acpi_bus_get_status_handle(acpi_handle handle,
				       unsigned long long *sta)
{
	acpi_status status;

	status = acpi_evaluate_integer(handle, "_STA", NULL, sta);
	if (ACPI_SUCCESS(status))
		return AE_OK;

	if (status == AE_NOT_FOUND) {
		*sta = ACPI_STA_DEVICE_PRESENT | ACPI_STA_DEVICE_ENABLED |
		       ACPI_STA_DEVICE_UI      | ACPI_STA_DEVICE_FUNCTIONING;
		return AE_OK;
	}
	return status;
}
EXPORT_SYMBOL_GPL(acpi_bus_get_status_handle);

/*
 * acpi_bus_get_status() - 刷新 acpi_device 中缓存的固件状态。
 *
 * @device 是调用者保证存活的借用指针；函数可能执行 _STA，因而可以睡眠。
 * quirk 覆盖优先于固件求值，电池还需先满足 _DEP 依赖。成功返回 0，并通过
 * acpi_set_device_status() 更新 device->status；求值失败返回 -ENODEV。
 */
int acpi_bus_get_status(struct acpi_device *device)
{
	acpi_status status;
	unsigned long long sta;

	if (acpi_device_override_status(device, &sta)) {
		/* 平台规则已给出可信覆盖值，不再执行可能有缺陷的固件 _STA。 */
		acpi_set_device_status(device, sta);
		return 0;
	}

	/* Battery devices must have their deps met before calling _STA */
	/*
	 * 电池 _STA 可能访问 _DEP 指定的控制器；依赖未就绪时执行 AML 可能失败
	 * 或访问未初始化硬件，因此暂时发布“不可用”，待依赖满足后再刷新。
	 */
	if (acpi_device_is_battery(device) && device->dep_unmet) {
		acpi_set_device_status(device, 0);
		return 0;
	}

	status = acpi_bus_get_status_handle(device->handle, &sta);
	if (ACPI_FAILURE(status))
		return -ENODEV;

	if (!device->status.present && device->status.enabled) {
		/*
		 * 此判断读取的是调用前已经缓存于 device->status 的状态，并以本次
		 * _STA 值 sta 作为诊断输出；命中表示缓存曾出现“未存在但已启用”。
		 */
		pr_info(FW_BUG "Device [%s] status [%08x]: not present and enabled\n",
			device->pnp.bus_id, (u32)sta);
		device->status.enabled = 0;
		/*
		 * The status is clearly invalid, so clear the functional bit as
		 * well to avoid attempting to use the device.
		 */
		/*
		 * 修正说明：原注释描述了清除 functional 的设计目的，但按当前源码顺序，
		 * 这里对旧缓存的两次位清除紧接着会被 acpi_set_device_status(device, sta)
		 * 整体覆盖；最终可见状态仍来自本次 sta。因而不能把这两行解释成对新
		 * _STA 值的持久修正，当前明确效果是记录固件/缓存异常诊断。
		 */
		device->status.functional = 0;
	}

	acpi_set_device_status(device, sta);

	if (device->status.functional && !device->status.present) {
		pr_debug("Device [%s] status [%08x]: functional but not present\n",
			 device->pnp.bus_id, (u32)sta);
	}

	pr_debug("Device [%s] status [%08x]\n", device->pnp.bus_id, (u32)sta);
	return 0;
}
EXPORT_SYMBOL(acpi_bus_get_status);

/*
 * 这是 ACPICA attach-data API 的身份键，不是实际事件回调。ACPICA 以
 * “handler 地址 + handle”区分数据槽，所以函数体故意为空；两个参数均借用，
 * 无返回值和副作用。
 */
void acpi_bus_private_data_handler(acpi_handle handle,
				   void *context)
{
	return;
}
EXPORT_SYMBOL(acpi_bus_private_data_handler);

/*
 * acpi_bus_attach_private_data() - 将调用者数据关联到命名空间节点。
 *
 * @data 的所有权仍归调用者，ACPICA 只保存裸指针；调用者必须在释放 data 前
 * detach，并自行同步并发读取者。成功返回 0，失败返回 -ENODEV，不释放 data。
 */
int acpi_bus_attach_private_data(acpi_handle handle, void *data)
{
	acpi_status status;

	status = acpi_attach_data(handle,
			acpi_bus_private_data_handler, data);
	if (ACPI_FAILURE(status)) {
		acpi_handle_debug(handle, "Error attaching device data\n");
		return -ENODEV;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(acpi_bus_attach_private_data);

/*
 * acpi_bus_get_private_data() - 取回上述槽中的借用指针。
 *
 * @data 为必需输出；成功时 *data 不增加引用，其有效期由 attach/detach 一方
 * 保证。NULL 输出参数返回 -EINVAL，无关联或查询失败返回 -ENODEV。
 */
int acpi_bus_get_private_data(acpi_handle handle, void **data)
{
	acpi_status status;

	if (!data)
		return -EINVAL;

	status = acpi_get_data(handle, acpi_bus_private_data_handler, data);
	if (ACPI_FAILURE(status)) {
		acpi_handle_debug(handle, "No context for object\n");
		return -ENODEV;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(acpi_bus_get_private_data);

/*
 * acpi_bus_detach_private_data() 仅移除关联，不释放 data。调用者应先阻止新读者
 * 并等待既有借用结束；@handle 为借用句柄，函数无直接返回值。
 */
void acpi_bus_detach_private_data(acpi_handle handle)
{
	acpi_detach_data(handle, acpi_bus_private_data_handler);
}
EXPORT_SYMBOL_GPL(acpi_bus_detach_private_data);

/*
 * acpi_dump_osc_data() 只打印一次 _OSC 请求的 UUID、版本和 DWORD 能力数组。
 * @cap->pointer 属于调用者，本函数只读；i 以 DWORD 为单位遍历，不分配内存，
 * 也不改变协商结果。
 */
static void acpi_dump_osc_data(acpi_handle handle, const guid_t *guid, int rev,
			       struct acpi_buffer *cap)
{
	u32 *capbuf = cap->pointer;
	int i;

	acpi_handle_debug(handle, "_OSC: UUID: %pUL, rev: %d\n", guid, rev);
	for (i = 0; i < cap->length / sizeof(u32); i++)
		acpi_handle_debug(handle, "_OSC: capabilities DWORD %i: [%08x]\n",
				  i, capbuf[i]);
}

/* _OSC 返回查询 DWORD 中仅这四位是规范错误；其他保留位不得影响协商。 */
#define OSC_ERROR_MASK 	(OSC_REQUEST_ERROR | OSC_INVALID_UUID_ERROR | \
			 OSC_INVALID_REVISION_ERROR | \
			 OSC_CAPABILITIES_MASK_ERROR)

/*
 * acpi_eval_osc() - 组装并执行一次标准 _OSC AML 调用。
 *
 * @handle 是方法节点；@guid/@rev 指定能力集合和协议版本；@cap 是调用者拥有的
 * DWORD 缓冲区；@in_params 至少四项，成功后仍引用 guid/cap，供握手第二阶段
 * 复用；@output 接收 ACPICA 分配的返回对象。函数会执行 AML并可睡眠。
 *
 * 成功返回 0，output->pointer 的释放责任交给调用者。求值失败或固件没有返回
 * 与输入等长的 BUFFER 时返回 -ENODATA；若已取得无效对象，本函数自行释放。
 */
static int acpi_eval_osc(acpi_handle handle, guid_t *guid, int rev,
			 struct acpi_buffer *cap,
			 union acpi_object in_params[at_least 4],
			 struct acpi_buffer *output)
{
	struct acpi_object_list input;
	union acpi_object *out_obj;
	acpi_status status;

	/* _OSC 四个位置参数依次为 UUID、revision、DWORD 数和能力 buffer。 */
	in_params[0].type = ACPI_TYPE_BUFFER;
	in_params[0].buffer.length = sizeof(*guid);
	in_params[0].buffer.pointer = (u8 *)guid;
	in_params[1].type = ACPI_TYPE_INTEGER;
	in_params[1].integer.value = rev;
	in_params[2].type = ACPI_TYPE_INTEGER;
	in_params[2].integer.value = cap->length / sizeof(u32);
	in_params[3].type = ACPI_TYPE_BUFFER;
	in_params[3].buffer.length = cap->length;
	in_params[3].buffer.pointer = cap->pointer;
	input.pointer = in_params;
	input.count = 4;

	/* 让 ACPICA 按固件实际返回对象的大小分配内存。 */
	output->length = ACPI_ALLOCATE_BUFFER;
	output->pointer = NULL;

	status = acpi_evaluate_object(handle, "_OSC", &input, output);
	if (ACPI_FAILURE(status) || !output->length)
		return -ENODATA;

	/*
	 * 只有同长度 BUFFER 才能按输入的 DWORD 布局解释错误位和能力位；形状
	 * 不符时继续强转为 u32 数组会造成越界或错误协商。
	 */
	out_obj = output->pointer;
	if (out_obj->type != ACPI_TYPE_BUFFER ||
	    out_obj->buffer.length != cap->length) {
		acpi_handle_debug(handle, "Invalid _OSC return buffer\n");
		acpi_dump_osc_data(handle, guid, rev, cap);
		ACPI_FREE(out_obj);
		return -ENODATA;
	}

	return 0;
}

/*
 * acpi_osc_error_check() - 解释返回首 DWORD 中规范定义的 _OSC 错误位。
 *
 * @cap 描述请求，@retbuf 是固件返回数组的借用指针。true 表示本阶段必须
 * 失败；false 表示调用者仍可采用固件收窄后的能力掩码。函数只读缓冲区并
 * 输出诊断，不取得所有权。
 */
static bool acpi_osc_error_check(acpi_handle handle, guid_t *guid, int rev,
				 struct acpi_buffer *cap, u32 *retbuf)
{
	/* Only take defined error bits into account. */
	/* 保留位不能被当作协议错误，故先用规范掩码过滤。 */
	u32 errors = retbuf[OSC_QUERY_DWORD] & OSC_ERROR_MASK;
	u32 *capbuf = cap->pointer;
	bool fail;

	/*
	 * If OSC_QUERY_ENABLE is set, ignore the "capabilities masked"
	 * bit because it merely means that some features have not been
	 * acknowledged which is not unexpected.
	 */
	/* 查询的目的就是获知交集，固件屏蔽部分能力属于正常回答。 */
	if (capbuf[OSC_QUERY_DWORD] & OSC_QUERY_ENABLE)
		errors &= ~OSC_CAPABILITIES_MASK_ERROR;

	if (!errors)
		return false;

	acpi_dump_osc_data(handle, guid, rev, cap);
	/*
	 * As a rule, fail only if OSC_QUERY_ENABLE is set because otherwise the
	 * acknowledged features need to be controlled.
	 */
	/*
	 * 正式控制阶段即使部分位被拒，已确认的位仍应继续使用；查询阶段若请求
	 * 本身不可处理，则不能进入控制阶段。
	 */
	fail = !!(capbuf[OSC_QUERY_DWORD] & OSC_QUERY_ENABLE);

	if (errors & OSC_REQUEST_ERROR)
		acpi_handle_debug(handle, "_OSC: request failed\n");

	if (errors & OSC_INVALID_UUID_ERROR) {
		acpi_handle_debug(handle, "_OSC: invalid UUID\n");
		/*
		 * Always fail if this bit is set because it means that the
		 * request could not be processed.
		 */
		/* UUID 无效表示固件不认识整个能力集合，两种阶段都无法继续。 */
		fail = true;
	}

	if (errors & OSC_INVALID_REVISION_ERROR)
		acpi_handle_debug(handle, "_OSC: invalid revision\n");

	if (errors & OSC_CAPABILITIES_MASK_ERROR)
		acpi_handle_debug(handle, "_OSC: capability bits masked\n");

	return fail;
}

/*
 * acpi_run_osc() - 对外执行单次 _OSC 并返回独立结果副本。
 *
 * @context 是输入输出对象：uuid_str、rev、cap 由调用者提供且保持所有权；
 * 成功时 ret 被填为 kmemdup() 的缓冲区，调用者负责释放。cap 至少包含查询
 * DWORD 和一个能力 DWORD。函数执行 AML并用 GFP_KERNEL 分配，因此可睡眠。
 *
 * 返回 AE_OK、AE_BAD_PARAMETER、AE_ERROR 或 AE_NO_MEMORY。ACPICA 临时返回
 * 对象在所有完成路径中均由本函数 ACPI_FREE()，不会泄漏给调用者。
 */
acpi_status acpi_run_osc(acpi_handle handle, struct acpi_osc_context *context)
{
	union acpi_object in_params[4], *out_obj;
	struct acpi_buffer output;
	acpi_status status = AE_OK;
	guid_t guid;
	u32 *retbuf;
	int ret;

	if (!context || !context->cap.pointer ||
	    context->cap.length < 2 * sizeof(u32) ||
	    guid_parse(context->uuid_str, &guid))
		return AE_BAD_PARAMETER;

	/* 阶段 1：同步执行固件方法，取得 ACPICA 管理的临时返回对象。 */
	ret = acpi_eval_osc(handle, &guid, context->rev, &context->cap,
			    in_params, &output);
	if (ret)
		return AE_ERROR;

	out_obj = output.pointer;
	retbuf = (u32 *)out_obj->buffer.pointer;

	/* 阶段 2：验证协议状态，再复制为调用者可越过本函数持有的内存。 */
	if (acpi_osc_error_check(handle, &guid, context->rev, &context->cap, retbuf)) {
		status = AE_ERROR;
		goto out;
	}

	context->ret.length = out_obj->buffer.length;
	context->ret.pointer = kmemdup(retbuf, context->ret.length, GFP_KERNEL);
	if (!context->ret.pointer) {
		status =  AE_NO_MEMORY;
		goto out;
	}
	status =  AE_OK;

out:
	/* 无论协议或复制是否成功，临时 ACPICA 对象都在此统一回收。 */
	ACPI_FREE(out_obj);
	return status;
}
EXPORT_SYMBOL(acpi_run_osc);

/*
 * acpi_osc_handshake() - 完成“查询支持 -> 正式请求控制权”的两阶段握手。
 *
 * @capbuf 是输入输出 DWORD 数组，@bufsize 以 DWORD 而非字节计。调用者写入
 * 期望位，成功后数组被原地收窄为固件最终确认的位。函数可执行 AML并睡眠，
 * 临时 ACPICA 对象始终在函数内释放。
 *
 * 返回 0 表示握手完成（最终零能力也算完成），-EINVAL 表示参数无效，
 * -ENODATA 表示固件或协议失败。取双方交集避免 OS 使用未获授权的功能。
 */
static int acpi_osc_handshake(acpi_handle handle, const char *uuid_str,
			      int rev, u32 *capbuf, size_t bufsize)
{
	union acpi_object in_params[4], *out_obj;
	struct acpi_object_list input;
	struct acpi_buffer cap = {
		.pointer = capbuf,
		.length = bufsize * sizeof(u32),
	};
	struct acpi_buffer output;
	u32 *retbuf, test;
	guid_t guid;
	int ret, i;

	if (!capbuf || bufsize < 2 || guid_parse(uuid_str, &guid))
		return -EINVAL;

	/* First evaluate _OSC with OSC_QUERY_ENABLE set. */
	/* 第一遍只查询支持集合，不改变平台控制权。 */
	capbuf[OSC_QUERY_DWORD] = OSC_QUERY_ENABLE;

	ret = acpi_eval_osc(handle, &guid, rev, &cap, in_params, &output);
	if (ret)
		return ret;

	out_obj = output.pointer;
	retbuf = (u32 *)out_obj->buffer.pointer;

	if (acpi_osc_error_check(handle, &guid, rev, &cap, retbuf)) {
		ret = -ENODATA;
		goto out;
	}

	/*
	 * Clear the feature bits in the capabilities buffer that have not been
	 * acknowledged and clear the return buffer.
	 */
	/*
	 * 每个能力 DWORD 求请求值与确认值的交集；test 汇总是否还有任何获准位。
	 * 同时清零返回区，为第二次原地复用做准备。
	 */
	for (i = OSC_QUERY_DWORD + 1, test = 0; i < bufsize; i++) {
		capbuf[i] &= retbuf[i];
		test |= capbuf[i];
		retbuf[i] = 0;
	}
	/*
	 * If none of the feature bits have been acknowledged, there's nothing
	 * more to do.  capbuf[] contains a feature mask of all zeros.
	 */
	/* 全零交集无需再请求控制，capbuf 已是最终降级结果。 */
	if (!test)
		goto out;

	retbuf[OSC_QUERY_DWORD] = 0;
	/*
	 * Now evaluate _OSC again (directly) with OSC_QUERY_ENABLE clear and
	 * the updated input and output buffers used before.  Since the feature
	 * bits that were clear in the return buffer from the previous _OSC
	 * evaluation are also clear in the capabilities buffer now, this _OSC
	 * evaluation is not expected to fail.
	 */
	/*
	 * 第二遍清 QUERY 位并正式提交第一遍确认的交集。in_params 中的指针仍
	 * 指向同一 capbuf，output 内存也按 ACPICA 接口复用到统一清理点。
	 */
	capbuf[OSC_QUERY_DWORD] = 0;
	/* Reuse in_params[] populated by acpi_eval_osc(). */
	/* 四参数布局不变，直接复用第一次求值填好的对象。 */
	input.pointer = in_params;
	input.count = 4;

	if (ACPI_FAILURE(acpi_evaluate_object(handle, "_OSC", &input, &output))) {
		ret = -ENODATA;
		goto out;
	}

	/*
	 * Clear the feature bits in capbuf[] that have not been acknowledged.
	 * After that, capbuf[] contains the resultant feature mask.
	 */
	/* 再取一次交集，形成 OS 后续真正可以依赖的最终控制掩码。 */
	for (i = OSC_QUERY_DWORD + 1; i < bufsize; i++)
		capbuf[i] &= retbuf[i];

	if (retbuf[OSC_QUERY_DWORD] & OSC_ERROR_MASK) {
		/*
		 * Complain about the unexpected errors and print diagnostic
		 * information related to them.
		 */
		/* 查询已成功而提交失败说明固件行为不一致，记录错误并保留降级掩码。 */
		acpi_handle_err(handle, "_OSC: errors while processing control request\n");
		acpi_handle_err(handle, "_OSC: some features may be missing\n");
		acpi_osc_error_check(handle, &guid, rev, &cap, retbuf);
	}

out:
	/* out_obj 是 ACPICA 分配并在两次求值间复用的临时对象。 */
	ACPI_FREE(out_obj);
	return ret;
}

/*
 * 以下全局布尔量保存平台级 _OSC 的最终确认结果，均在单线程启动阶段写入，
 * 此后由对应子系统只读。它们不是“内核编译支持”本身，而是“内核声明支持且
 * 固件确认”的交集；未协商或失败时保持 false，调用方必须走兼容路径。
 */
bool osc_sb_apei_support_acked;

/*
 * ACPI 6.0 Section 8.4.4.2 Idle State Coordination
 * OSPM supports platform coordinated low power idle(LPI) states
 */
/*
 * 表示 OSPM 已获准使用平台协调的 LPI 空闲状态；CPU idle 代码据此决定能否
 * 采用需要平台共同参与的低功耗方案。
 */
bool osc_pc_lpi_support_confirmed;
EXPORT_SYMBOL_GPL(osc_pc_lpi_support_confirmed);

/*
 * ACPI 6.2 Section 6.2.11.2 'Platform-Wide OSPM Capabilities':
 *   Starting with ACPI Specification 6.2, all _CPC registers can be in
 *   PCC, System Memory, System IO, or Functional Fixed Hardware address
 *   spaces. OSPM support for this more flexible register space scheme is
 *   indicated by the “Flexible Address Space for CPPC Registers” _OSC bit.
 *
 * Otherwise (cf ACPI 6.1, s8.4.7.1.1.X), _CPC registers must be in:
 * - PCC or Functional Fixed Hardware address space if defined
 * - SystemMemory address space (NULL register) if not defined
 */
/*
 * 固件确认后，CPPC 的 _CPC 寄存器可位于更灵活的地址空间；未确认时仍必须
 * 遵守旧规范的地址空间限制，不能仅因内核驱动支持就访问任意 GAS 类型。
 */
bool osc_cpc_flexible_adr_space_confirmed;
EXPORT_SYMBOL_GPL(osc_cpc_flexible_adr_space_confirmed);

/*
 * ACPI 6.4 Operating System Capabilities for USB.
 */
/* 表示平台接受 OS 原生 USB4 能力协议，后续才可继续协商具体隧道控制位。 */
bool osc_sb_native_usb4_support_confirmed;
EXPORT_SYMBOL_GPL(osc_sb_native_usb4_support_confirmed);

/* 表示平台确认第二版 CPPC 能力，供处理器性能控制路径选择接口版本。 */
bool osc_sb_cppc2_support_acked;

/*
 * acpi_bus_osc_negotiate_platform_control() - 协商平台范围的 OSPM 能力。
 *
 * 无参数、无直接返回值；启动期从内核配置和运行时开关构造支持位，在 \_SB
 * 上执行平台 UUID 的 _OSC，最后更新上述只读全局结果。方法缺失或握手失败时
 * 安全返回并保留 false。函数执行 AML，可睡眠，且必须晚于完整对象初始化。
 */
static void acpi_bus_osc_negotiate_platform_control(void)
{
	static const u8 sb_uuid_str[] = "0811B06E-4A27-44F9-8D60-3CBBC22E7B48";
	u32 capbuf[2], feature_mask;
	acpi_handle handle;

	/* 阶段 1：先声明本文件及核心 ACPI 代码无条件实现的基础能力。 */
	feature_mask = OSC_SB_PR3_SUPPORT | OSC_SB_HOTPLUG_OST_SUPPORT |
			OSC_SB_PCLPI_SUPPORT | OSC_SB_OVER_16_PSTATES_SUPPORT |
			OSC_SB_GED_SUPPORT | OSC_SB_IRQ_RESOURCE_SOURCE_SUPPORT;

	/* 阶段 2：只为已编译且实际可用的子系统增加能力，避免过度承诺。 */
	if (IS_ENABLED(CONFIG_ARM64) || IS_ENABLED(CONFIG_X86))
		feature_mask |= OSC_SB_GENERIC_INITIATOR_SUPPORT;

	if (IS_ENABLED(CONFIG_ACPI_CPPC_LIB)) {
		feature_mask |= OSC_SB_CPC_SUPPORT | OSC_SB_CPCV2_SUPPORT |
				OSC_SB_CPC_FLEXIBLE_ADR_SPACE;
		if (IS_ENABLED(CONFIG_SCHED_MC_PRIO))
			feature_mask |= OSC_SB_CPC_DIVERSE_HIGH_SUPPORT;
	}

	if (IS_ENABLED(CONFIG_ACPI_PROCESSOR_AGGREGATOR))
		feature_mask |= OSC_SB_PAD_SUPPORT;

	if (IS_ENABLED(CONFIG_ACPI_PROCESSOR))
		feature_mask |= OSC_SB_PPC_OST_SUPPORT;

	if (IS_ENABLED(CONFIG_ACPI_THERMAL))
		feature_mask |= OSC_SB_FAST_THERMAL_SAMPLING_SUPPORT;

	if (IS_ENABLED(CONFIG_ACPI_BATTERY))
		feature_mask |= OSC_SB_BATTERY_CHARGE_LIMITING_SUPPORT;

	if (IS_ENABLED(CONFIG_ACPI_PRMT))
		feature_mask |= OSC_SB_PRM_SUPPORT;

	if (IS_ENABLED(CONFIG_ACPI_FFH))
		feature_mask |= OSC_SB_FFH_OPR_SUPPORT;

	if (IS_ENABLED(CONFIG_USB4))
		feature_mask |= OSC_SB_NATIVE_USB4_SUPPORT;

	if (!ghes_disable)
		feature_mask |= OSC_SB_APEI_SUPPORT;

	/* _OSC 属于系统总线对象；找不到 \_SB 时平台级协商无法进行。 */
	if (ACPI_FAILURE(acpi_get_handle(NULL, "\\_SB", &handle)))
		return;

	capbuf[OSC_SUPPORT_DWORD] = feature_mask;

	acpi_handle_info(handle, "platform _OSC: OS support mask [%08x]\n", feature_mask);

	/* 阶段 3：握手会把 capbuf 原地收窄为固件确认的最终交集。 */
	if (acpi_osc_handshake(handle, sb_uuid_str, 1, capbuf, ARRAY_SIZE(capbuf)))
		return;

	feature_mask = capbuf[OSC_SUPPORT_DWORD];

	acpi_handle_info(handle, "platform _OSC: OS control mask [%08x]\n", feature_mask);

	/* 阶段 4：一次性发布只读确认位，供各功能子系统选择安全路径。 */
	osc_sb_cppc2_support_acked = feature_mask & OSC_SB_CPCV2_SUPPORT;
	osc_sb_apei_support_acked = feature_mask & OSC_SB_APEI_SUPPORT;
	osc_pc_lpi_support_confirmed = feature_mask & OSC_SB_PCLPI_SUPPORT;
	osc_sb_native_usb4_support_confirmed = feature_mask & OSC_SB_NATIVE_USB4_SUPPORT;
	osc_cpc_flexible_adr_space_confirmed = feature_mask & OSC_SB_CPC_FLEXIBLE_ADR_SPACE;
}

/*
 * Native control of USB4 capabilities. If any of the tunneling bits is
 * set it means OS is in control and we use software based connection
 * manager.
 */
/*
 * 任一隧道位获准即表示该连接类型由 OS 软件连接管理器控制；未置位的类型仍
 * 归固件。该 u32 在启动协商时写一次，Thunderbolt/USB4 路径随后只读。
 */
u32 osc_sb_native_usb4_control;
EXPORT_SYMBOL_GPL(osc_sb_native_usb4_control);

/*
 * acpi_bus_decode_usb_osc() 把 USB4 四类隧道位格式化为启动日志。@msg 为借用
 * 字符串，@bits 为值快照；函数只读全局状态、不可失败且无所有权变化。
 */
static void acpi_bus_decode_usb_osc(const char *msg, u32 bits)
{
	pr_info("%s USB3%c DisplayPort%c PCIe%c XDomain%c\n", msg,
	       (bits & OSC_USB_USB3_TUNNELING) ? '+' : '-',
	       (bits & OSC_USB_DP_TUNNELING) ? '+' : '-',
	       (bits & OSC_USB_PCIE_TUNNELING) ? '+' : '-',
	       (bits & OSC_USB_XDOMAIN) ? '+' : '-');
}

/*
 * acpi_bus_osc_negotiate_usb_control() - 在平台能力确认后请求具体 USB4 控制权。
 *
 * 无参数和直接返回值；只有第一阶段平台 _OSC 已确认原生 USB4 支持才在 \_SB
 * 上用 USB UUID 握手。成功后发布实际获准的隧道位并打印“请求/获准”对照；
 * 任何失败均保持零掩码，让固件继续管理。启动期执行 AML，允许睡眠。
 */
static void acpi_bus_osc_negotiate_usb_control(void)
{
	static const u8 sb_usb_uuid_str[] = "23A0D13A-26AB-486C-9C5F-0FFA525A575A";
	u32 capbuf[3], control;
	acpi_handle handle;

	/* 平台级先决能力未确认时，不能直接尝试接管具体隧道。 */
	if (!osc_sb_native_usb4_support_confirmed)
		return;

	if (ACPI_FAILURE(acpi_get_handle(NULL, "\\_SB", &handle)))
		return;

	/* 请求所有内核连接管理器能够处理的隧道类型。 */
	control = OSC_USB_USB3_TUNNELING | OSC_USB_DP_TUNNELING |
		  OSC_USB_PCIE_TUNNELING | OSC_USB_XDOMAIN;

	capbuf[OSC_SUPPORT_DWORD] = 0;
	capbuf[OSC_CONTROL_DWORD] = control;

	if (acpi_osc_handshake(handle, sb_usb_uuid_str, 1, capbuf, ARRAY_SIZE(capbuf)))
		return;

	/* 握手后 capbuf 已是双方交集，此写入是对其他子系统的发布点。 */
	osc_sb_native_usb4_control = capbuf[OSC_CONTROL_DWORD];

	acpi_bus_decode_usb_osc("USB4 _OSC: OS supports", control);
	acpi_bus_decode_usb_osc("USB4 _OSC: OS controls", osc_sb_native_usb4_control);
}

/* --------------------------------------------------------------------------
                             Notification Handling
   -------------------------------------------------------------------------- */

/**
 * acpi_bus_notify - Global system-level (0x00-0x7F) notifications handler
 * @handle: Target ACPI object.
 * @type: Notification type.
 * @data: Ignored.
 *
 * This only handles notifications related to device hotplug.
 */
/*
 * 处理 ACPICA 分发的系统级 0x00～0x7f Notify，本文件只消费与设备重新扫描、
 * 弹出有关的事件。@handle 是目标节点的借用句柄，@type 是事件码，@data 在
 * 此全局注册中为 NULL 且不使用。回调可能来自 ACPICA 异步事件执行路径，不能
 * 在此直接重建命名空间；可处理事件被交给 acpi_hotplug_schedule()。
 *
 * 调度成功时工作项接管 adev 引用；调度失败或没有 acpi_device 时，本函数
 * 释放临时引用并以 _OST 向固件报告失败。纯状态/故障通知只记录后返回。
 */
static void acpi_bus_notify(acpi_handle handle, u32 type, void *data)
{
	struct acpi_device *adev;

	switch (type) {
	case ACPI_NOTIFY_BUS_CHECK:
		acpi_handle_debug(handle, "ACPI_NOTIFY_BUS_CHECK event\n");
		break;

	case ACPI_NOTIFY_DEVICE_CHECK:
		acpi_handle_debug(handle, "ACPI_NOTIFY_DEVICE_CHECK event\n");
		break;

	case ACPI_NOTIFY_DEVICE_WAKE:
		acpi_handle_debug(handle, "ACPI_NOTIFY_DEVICE_WAKE event\n");
		return;

	case ACPI_NOTIFY_EJECT_REQUEST:
		acpi_handle_debug(handle, "ACPI_NOTIFY_EJECT_REQUEST event\n");
		break;

	case ACPI_NOTIFY_DEVICE_CHECK_LIGHT:
		acpi_handle_debug(handle, "ACPI_NOTIFY_DEVICE_CHECK_LIGHT event\n");
		/* TBD: Exactly what does 'light' mean? */
		/*
		 * 上游尚未确定“轻量检查”的精确重枚举语义，因此保守地只记录，
		 * 不把它等同于完整 DEVICE_CHECK 以免错误增删设备。
		 */
		return;

	case ACPI_NOTIFY_FREQUENCY_MISMATCH:
		acpi_handle_err(handle, "Device cannot be configured due "
				"to a frequency mismatch\n");
		return;

	case ACPI_NOTIFY_BUS_MODE_MISMATCH:
		acpi_handle_err(handle, "Device cannot be configured due "
				"to a bus mode mismatch\n");
		return;

	case ACPI_NOTIFY_POWER_FAULT:
		acpi_handle_err(handle, "Device has suffered a power fault\n");
		return;

	default:
		acpi_handle_debug(handle, "Unknown event type 0x%x\n", type);
		return;
	}

	/*
	 * 将 ACPICA 句柄转换为带引用的 acpi_device，跨越异步调度边界必须稳定
	 * 生命周期，不能只使用命名空间裸指针。
	 */
	adev = acpi_get_acpi_dev(handle);

	/* 成功调度时热插拔工作取得该引用，本路径不再 put。 */
	if (adev && ACPI_SUCCESS(acpi_hotplug_schedule(adev, type)))
		return;

	/* 未转移给工作项的引用在报告失败前由本函数释放。NULL 可安全传入。 */
	acpi_put_acpi_dev(adev);

	acpi_evaluate_ost(handle, type, ACPI_OST_SC_NON_SPECIFIC_FAILURE, NULL);
}

/*
 * acpi_notify_device() 是传统 acpi_driver 通知适配器。@data 是安装 handler
 * 时传入且由驱动核心保证仍绑定的 acpi_device；从当前 dev.driver 取得
 * acpi_driver 后同步调用 ops.notify。它不持有新引用，卸载端必须等待事件排空。
 */
static void acpi_notify_device(acpi_handle handle, u32 event, void *data)
{
	struct acpi_device *device = data;
	struct acpi_driver *acpi_drv = to_acpi_driver(device->dev.driver);

	acpi_drv->ops.notify(device, event);
}

/*
 * acpi_device_install_notify_handler() - 为已成功 probe 的 acpi_driver 安装回调。
 *
 * @device/@acpi_drv 均为绑定期借用对象。驱动标记 ALL_NOTIFY_EVENTS 时同时接收
 * 系统和设备通知，否则只接收 0x80～0xff 设备通知。成功返回 0；ACPICA 注册
 * 失败返回 -EINVAL，此时 probe 会调用 remove 回滚已建立的驱动状态。
 */
static int acpi_device_install_notify_handler(struct acpi_device *device,
					      struct acpi_driver *acpi_drv)
{
	u32 type = acpi_drv->flags & ACPI_DRIVER_ALL_NOTIFY_EVENTS ?
				ACPI_ALL_NOTIFY : ACPI_DEVICE_NOTIFY;
	acpi_status status;

	status = acpi_install_notify_handler(device->handle, type,
					     acpi_notify_device, device);
	if (ACPI_FAILURE(status))
		return -EINVAL;

	return 0;
}

/*
 * acpi_device_remove_notify_handler() - 解绑前撤销传统驱动通知并建立静默边界。
 *
 * 移除注册只阻止新事件入队，acpi_os_wait_events_complete() 还要等待已排队回调
 * 完成；因此返回后 ops.remove 才可释放 notify 所用资源。函数可睡眠，无返回值。
 */
static void acpi_device_remove_notify_handler(struct acpi_device *device,
					      struct acpi_driver *acpi_drv)
{
	u32 type = acpi_drv->flags & ACPI_DRIVER_ALL_NOTIFY_EVENTS ?
				ACPI_ALL_NOTIFY : ACPI_DEVICE_NOTIFY;

	acpi_remove_notify_handler(device->handle, type,
				   acpi_notify_device);

	acpi_os_wait_events_complete();
}

/*
 * acpi_dev_install_notify_handler() - 公共的非托管 ACPICA 通知注册接口。
 *
 * @adev 为调用者保证存活的 ACPI companion；@handler_type 指定系统/设备/全部
 * 事件；@handler 与 @context 由 ACPICA 保存为裸函数/数据指针，所有权不转移。
 * 成功返回 0，失败返回 -ENODEV。调用者必须在相关代码或 context 释放前移除。
 */
int acpi_dev_install_notify_handler(struct acpi_device *adev,
				    u32 handler_type,
				    acpi_notify_handler handler, void *context)
{
	acpi_status status;

	status = acpi_install_notify_handler(adev->handle, handler_type,
					     handler, context);
	if (ACPI_FAILURE(status))
		return -ENODEV;

	return 0;
}
EXPORT_SYMBOL_GPL(acpi_dev_install_notify_handler);

/*
 * acpi_dev_remove_notify_handler() 撤销与 adev/type/handler 精确匹配的注册，并
 * 等待所有 ACPICA 异步事件完成。返回后调用者才可释放 handler 代码和 context；
 * 函数无直接返回值且可能睡眠。
 */
void acpi_dev_remove_notify_handler(struct acpi_device *adev,
				    u32 handler_type,
				    acpi_notify_handler handler)
{
	acpi_remove_notify_handler(adev->handle, handler_type, handler);
	acpi_os_wait_events_complete();
}
EXPORT_SYMBOL_GPL(acpi_dev_remove_notify_handler);

/*
 * devres 记录 handler 的完整卸载键。它由 devres_alloc() 创建、绑定到物理
 * struct device，设备解绑时调用 release；adev 只是借用 companion，handler
 * 与 type 用来精确撤销注册。
 */
struct acpi_notify_handler_devres {
	struct acpi_device *adev;
	acpi_notify_handler handler;
	u32 handler_type;
};

/*
 * devm_acpi_notify_handler_release() 是 devres 析构器。@res 指向框架拥有的记录，
 * 本函数通过公共移除接口同步排空回调；@dev 本身无需使用，记录内存随后由
 * devres 核心释放。
 */
static void devm_acpi_notify_handler_release(struct device *dev, void *res)
{
	struct acpi_notify_handler_devres *dr = res;

	acpi_dev_remove_notify_handler(dr->adev, dr->handler_type, dr->handler);
}

/**
 * devm_acpi_install_notify_handler - Install an ACPI notify handler for a
 *				      managed device
 * @dev: Device to install a notify handler for
 * @handler_type: Type of the notify handler
 * @handler: Handler function to install
 * @context: Data passed back to the handler function
 *
 * This function performs the same function as acpi_dev_install_notify_handler()
 * called for the ACPI companion of @dev with the same @handler_type, @handler,
 * and @context arguments, but the ACPI notify handler installed by it will be
 * automatically removed on driver detach.
 *
 * Callers should ensure that all resources used by @handler have been allocated
 * prior to invoking this function, in which case those resources should be
 * devres-managed so that they won't be released before the notify handler
 * removal.  Otherwise, special synchronization between @handler and the
 * management of those resources is required.
 *
 * When the request fails, an error message is printed.  Don't add extra error
 * messages at the call sites.
 *
 * Return: 0 on success or a negative error number.
 */
/*
 * 为 @dev 的 ACPI companion 安装托管通知。@handler_type/@handler/@context 的
 * 协议与非托管接口相同，但成功后 devres 自动保证“先移除并排空 handler，后
 * 释放更早注册的托管资源”的逆序解绑。调用者应在本调用前创建 handler 所依赖
 * 的 devres；否则必须自行同步其生命周期。
 *
 * 函数处于 probe 等可睡眠上下文，以 GFP_KERNEL 分配记录。无 companion 返回
 * -ENODEV，分配失败返回 -ENOMEM，ACPICA 注册失败返回其负 errno；错误日志已
 * 在此统一输出。成功时记录所有权转交 devres 核心。
 */
int devm_acpi_install_notify_handler(struct device *dev, u32 handler_type,
				     acpi_notify_handler handler, void *context)
{
	struct acpi_notify_handler_devres *dr;
	struct acpi_device *adev;
	int ret;

	/* 阶段 1：取得借用 companion；物理 device 生命周期覆盖整个 devres。 */
	adev = ACPI_COMPANION(dev);
	if (!adev)
		return dev_err_probe(dev, -ENODEV, "No ACPI companion\n");

	/* 阶段 2：先准备回滚记录，尚未发布给 devres。 */
	dr = devres_alloc(devm_acpi_notify_handler_release, sizeof(*dr), GFP_KERNEL);
	if (!dr)
		return -ENOMEM;

	/* 阶段 3：注册是外界可见点；失败时仅需释放尚未发布的记录。 */
	ret = acpi_dev_install_notify_handler(adev, handler_type, handler, context);
	if (ret) {
		devres_free(dr);
		return dev_err_probe(dev, ret, "Failed to install an ACPI notify handler\n");
	}

	/* 阶段 4：填好卸载键后发布到 devres，之后由设备解绑自动清理。 */
	dr->adev = adev;
	dr->handler = handler;
	dr->handler_type = handler_type;
	devres_add(dev, dr);

	return 0;
}
EXPORT_SYMBOL_GPL(devm_acpi_install_notify_handler);

/* Handle events targeting \_SB device (at present only graceful shutdown) */
/* 当前只处理发给 \_SB 的优雅关机请求，其他设备通知由上面的通用路径负责。 */

/* 0x81 是 \_SB 的 OSPM 关机请求；状态上报间隔单位为毫秒，即规范要求的 10 秒。 */
#define ACPI_SB_NOTIFY_SHUTDOWN_REQUEST 0x81
#define ACPI_SB_INDICATE_INTERVAL	10000

/*
 * sb_notify_work() 在进程上下文发起强制的 orderly_poweroff，并按 ACPI 规范每
 * 10 秒用 _OST 告知固件“OS 关机进行中”。@dummy 是工作队列统一签名参数。
 * orderly_poweroff() 和 AML 都可睡眠，因此不能直接放在 Notify 回调中。
 *
 * 循环有意不退出：正常关机最终终止系统；若用户态关机迟迟未完成，持续状态
 * 上报让固件知道请求没有丢失。该静态工作项至多同时执行一次。
 */
static void sb_notify_work(struct work_struct *dummy)
{
	acpi_handle sb_handle;

	orderly_poweroff(true);

	/*
	 * After initiating graceful shutdown, the ACPI spec requires OSPM
	 * to evaluate _OST method once every 10seconds to indicate that
	 * the shutdown is in progress
	 */
	/*
	 * 发起优雅关机后，OSPM 必须每 10 秒求值一次 _OST 表示仍在处理中；这里
	 * 无超时回收，因为 orderly_poweroff(true) 已允许必要时强制关机。
	 */
	acpi_get_handle(NULL, "\\_SB", &sb_handle);
	while (1) {
		pr_info("Graceful shutdown in progress.\n");
		acpi_evaluate_ost(sb_handle, ACPI_OST_EC_OSPM_SHUTDOWN,
				ACPI_OST_SC_OS_SHUTDOWN_IN_PROGRESS, NULL);
		msleep(ACPI_SB_INDICATE_INTERVAL);
	}
}

/*
 * acpi_sb_notify() 是 \_SB 设备级 Notify 的轻量入口。@handle/@data 不需保存，
 * @event 为固件事件码。匹配 0x81 时只调度静态工作；work_busy() 防止重复事件
 * 并发启动多个无限上报循环。未知事件只告警。回调本身不睡眠。
 */
static void acpi_sb_notify(acpi_handle handle, u32 event, void *data)
{
	static DECLARE_WORK(acpi_sb_work, sb_notify_work);

	if (event == ACPI_SB_NOTIFY_SHUTDOWN_REQUEST) {
		if (!work_busy(&acpi_sb_work))
			schedule_work(&acpi_sb_work);
	} else {
		pr_warn("event %x is not supported by \\_SB device\n", event);
	}
}

/*
 * acpi_setup_sb_notify_handler() - 启动期为 \_SB 注册优雅关机事件。
 *
 * 无参数；成功后 handler 持续到系统结束，不设常规卸载路径。找不到 \_SB 返回
 * -ENXIO，注册失败返回 -EINVAL，成功返回 0。调用者当前忽略失败，使 ACPI 其余
 * 功能仍可用；函数只在 init 阶段执行。
 */
static int __init acpi_setup_sb_notify_handler(void)
{
	acpi_handle sb_handle;

	if (ACPI_FAILURE(acpi_get_handle(NULL, "\\_SB", &sb_handle)))
		return -ENXIO;

	if (ACPI_FAILURE(acpi_install_notify_handler(sb_handle, ACPI_DEVICE_NOTIFY,
						acpi_sb_notify, NULL)))
		return -EINVAL;

	return 0;
}

/* --------------------------------------------------------------------------
                             Device Matching
   -------------------------------------------------------------------------- */

/**
 * acpi_get_first_physical_node - Get first physical node of an ACPI device
 * @adev:	ACPI device in question
 *
 * Return: First physical node of ACPI device @adev
 */
/*
 * 返回 @adev 关联物理节点链表中的首个 struct device，若链表为空则为 NULL。
 * @adev 必须有效；返回值是未增加引用的借用指针，只适合在调用者已保证关联
 * 生命周期的范围内使用。physical_node_lock 与并发 companion 添加/删除竞争，
 * 保证链表遍历结构安全；解锁后不会冻结 phys_dev 的其他字段。
 */
struct device *acpi_get_first_physical_node(struct acpi_device *adev)
{
	struct mutex *physical_node_lock = &adev->physical_node_lock;
	struct device *phys_dev;

	/* 互斥锁保护 physical_node_list 的链接关系和“第一个”这一顺序不变量。 */
	mutex_lock(physical_node_lock);
	if (list_empty(&adev->physical_node_list)) {
		phys_dev = NULL;
	} else {
		const struct acpi_device_physical_node *node;

		node = list_first_entry(&adev->physical_node_list,
					struct acpi_device_physical_node, node);

		phys_dev = node->dev;
	}
	mutex_unlock(physical_node_lock);
	return phys_dev;
}
EXPORT_SYMBOL_GPL(acpi_get_first_physical_node);

/*
 * acpi_primary_dev_companion() 只在 @dev 恰为 @adev 的首个物理节点时返回借用的
 * adev，否则返回 NULL。该策略防止多个物理子设备同时用共享 companion 的
 * PNP ID 匹配同一个 ACPI 驱动。
 */
static struct acpi_device *acpi_primary_dev_companion(struct acpi_device *adev,
						      const struct device *dev)
{
	const struct device *phys_dev = acpi_get_first_physical_node(adev);

	return phys_dev && phys_dev == dev ? adev : NULL;
}

/**
 * acpi_device_is_first_physical_node - Is given dev first physical node
 * @adev: ACPI companion device
 * @dev: Physical device to check
 *
 * Function checks if given @dev is the first physical devices attached to
 * the ACPI companion device. This distinction is needed in some cases
 * where the same companion device is shared between many physical devices.
 *
 * Note that the caller have to provide valid @adev pointer.
 */
/*
 * 判断 @dev 是否为共享 ACPI companion 的主物理节点。两个参数均为借用指针，
 * 调用者必须保证 @adev 非 NULL；函数不取得引用，只返回布尔快照。区分主节点
 * 使共享固件资源的其他物理设备仍可通过自身总线规则匹配，而不会重复套用 PNP ID。
 */
bool acpi_device_is_first_physical_node(struct acpi_device *adev,
					const struct device *dev)
{
	return !!acpi_primary_dev_companion(adev, dev);
}

/*
 * acpi_companion_match() - Can we match via ACPI companion device
 * @dev: Device in question
 *
 * Check if the given device has an ACPI companion and if that companion has
 * a valid list of PNP IDs, and if the device is the first (primary) physical
 * device associated with it.  Return the companion pointer if that's the case
 * or NULL otherwise.
 *
 * If multiple physical devices are attached to a single ACPI companion, we need
 * to be careful.  The usage scenario for this kind of relationship is that all
 * of the physical devices in question use resources provided by the ACPI
 * companion.  A typical case is an MFD device where all the sub-devices share
 * the parent's ACPI companion.  In such cases we can only allow the primary
 * (first) physical device to be matched with the help of the companion's PNP
 * IDs.
 *
 * Additional physical devices sharing the ACPI companion can still use
 * resources available from it but they will be matched normally using functions
 * provided by their bus types (and analogously for their modalias).
 */
/*
 * 若 @dev 有 companion、该 companion 有可匹配 ID，且 @dev 是首个物理节点，
 * 返回未增加引用的 companion；否则返回 NULL。共享 companion 的场景常见于
 * MFD 子设备：只有主节点可借固件 PNP ID 匹配，其余节点仍可借用资源，但必须
 * 使用各自总线的 ID/modalias，避免多个驱动错误绑定同一逻辑 ACPI 设备。
 */
const struct acpi_device *acpi_companion_match(const struct device *dev)
{
	struct acpi_device *adev;

	adev = ACPI_COMPANION(dev);
	if (!adev)
		return NULL;

	if (list_empty(&adev->pnp.ids))
		return NULL;

	return acpi_primary_dev_companion(adev, dev);
}

/**
 * acpi_of_match_device - Match device object using the "compatible" property.
 * @adev: ACPI device object to match.
 * @of_match_table: List of device IDs to match against.
 * @of_id: OF ID if matched
 *
 * If @dev has an ACPI companion which has ACPI_DT_NAMESPACE_HID in its list of
 * identifiers and a _DSD object with the "compatible" property, use that
 * property to match against the given list of identifiers.
 */
/*
 * 用 ACPI _DSD 缓存的 compatible 属性匹配 OF ID 表。@adev/@of_match_table 均为
 * 借用只读对象；@of_id 可为 NULL，非 NULL 且匹配时写入表内元素的借用指针。
 * 属性既可为单个 STRING，也可为 PACKAGE；比较不区分大小写。成功 true，输入
 * 缺失或无匹配 false，不分配内存、不会改变设备状态。
 */
bool acpi_of_match_device(const struct acpi_device *adev,
			  const struct of_device_id *of_match_table,
			  const struct of_device_id **of_id)
{
	const union acpi_object *of_compatible, *obj;
	int i, nval;

	if (!adev)
		return false;

	of_compatible = adev->data.of_compatible;
	if (!of_match_table || !of_compatible)
		return false;

	/* 把 STRING 规范化为一个元素，把 PACKAGE 视作连续候选数组。 */
	if (of_compatible->type == ACPI_TYPE_PACKAGE) {
		nval = of_compatible->package.count;
		obj = of_compatible->package.elements;
	} else { /* Must be ACPI_TYPE_STRING. */
		/* 扫描层已验证非 PACKAGE 形式必须是 STRING，这里依赖该缓存不变量。 */
		nval = 1;
		obj = of_compatible;
	}
	/* Now we can look for the driver DT compatible strings */
	/* 对每个固件字符串按表顺序查找；首个匹配决定驱动数据。 */
	for (i = 0; i < nval; i++, obj++) {
		const struct of_device_id *id;

		for (id = of_match_table; id->compatible[0]; id++)
			if (!strcasecmp(obj->string.pointer, id->compatible)) {
				if (of_id)
					*of_id = id;
				return true;
			}
	}

	return false;
}

/*
 * acpi_of_modalias() 从第一个 compatible 字符串生成简化 modalias。@modalias
 * 是长度 @len 的调用者输出缓冲区；有属性时复制逗号后的型号部分（无逗号则
 * 复制全串），始终以 strscpy() 限长。成功 true，无属性 false；不返回引用。
 */
static bool acpi_of_modalias(struct acpi_device *adev,
			     char *modalias, size_t len)
{
	const union acpi_object *of_compatible;
	const union acpi_object *obj;
	const char *str, *chr;

	of_compatible = adev->data.of_compatible;
	if (!of_compatible)
		return false;

	if (of_compatible->type == ACPI_TYPE_PACKAGE)
		obj = of_compatible->package.elements;
	else /* Must be ACPI_TYPE_STRING. */
		/* 非 PACKAGE 的缓存对象已由属性解析阶段保证为 STRING。 */
		obj = of_compatible;

	str = obj->string.pointer;
	chr = strchr(str, ',');
	strscpy(modalias, chr ? chr + 1 : str, len);

	return true;
}

/**
 * acpi_set_modalias - Set modalias using "compatible" property or supplied ID
 * @adev:	ACPI device object to match
 * @default_id:	ID string to use as default if no compatible string found
 * @modalias:   Pointer to buffer that modalias value will be copied into
 * @len:	Length of modalias buffer
 *
 * This is a counterpart of of_alias_from_compatible() for struct acpi_device
 * objects. If there is a compatible string for @adev, it will be copied to
 * @modalias with the vendor prefix stripped; otherwise, @default_id will be
 * used.
 */
/*
 * 为 @adev 生成用户空间模块自动加载使用的 modalias。优先采用 compatible 的
 * 无厂商前缀形式，否则把 @default_id 复制到 @modalias；所有输入均借用，输出
 * 缓冲区长度为 @len。函数无错误返回，过长字符串按 strscpy() 规则截断。
 */
void acpi_set_modalias(struct acpi_device *adev, const char *default_id,
		       char *modalias, size_t len)
{
	if (!acpi_of_modalias(adev, modalias, len))
		strscpy(modalias, default_id, len);
}
EXPORT_SYMBOL_GPL(acpi_set_modalias);

/*
 * __acpi_match_device_cls() - 将驱动 ID 的三字节类码掩码与硬件 ID 文本比较。
 *
 * @id/@hwid 均为借用对象。cls_msk 中为零的字节是通配位；其余字节先对驱动
 * 类码取掩码，再转为两位十六进制文本，与 hwid 对应位置比较。全部受约束
 * 字节相等返回 true；未提供类码或任一字节不符返回 false。
 */
static bool __acpi_match_device_cls(const struct acpi_device_id *id,
				    struct acpi_hardware_id *hwid)
{
	int i, msk, byte_shift;
	char buf[3];

	if (!id->cls)
		return false;

	/* Apply class-code bitmask, before checking each class-code byte */
	/* 每轮处理从高到低的一个类码字节，文本 ID 每字节占两个字符。 */
	for (i = 1; i <= 3; i++) {
		byte_shift = 8 * (3 - i);
		msk = (id->cls_msk >> byte_shift) & 0xFF;
		if (!msk)
			continue;

		sprintf(buf, "%02x", (id->cls >> byte_shift) & msk);
		if (strncmp(buf, &hwid->id[(i - 1) * 2], 2))
			return false;
	}
	return true;
}

/*
 * __acpi_match_device() - ACPI/PNP ID 与 DT compatible 的统一匹配核心。
 *
 * @device 必须存在且 status.present；@acpi_ids/@of_ids 可分别为 NULL；@acpi_id
 * 和 @of_id 是可选输出，成功时指向驱动静态表中的借用元素。函数按设备硬件
 * ID 顺序扫描，先试精确 ID/类码；遇到 ACPI_DT_NAMESPACE_HID 才转向 _DSD
 * compatible。返回值只表示匹配，不取得设备或表项引用。
 */
static bool __acpi_match_device(const struct acpi_device *device,
				const struct acpi_device_id *acpi_ids,
				const struct of_device_id *of_ids,
				const struct acpi_device_id **acpi_id,
				const struct of_device_id **of_id)
{
	const struct acpi_device_id *id;
	struct acpi_hardware_id *hwid;

	/*
	 * If the device is not present, it is unnecessary to load device
	 * driver for it.
	 */
	/* 不为固件报告 absent 的节点触发模块加载或 probe。 */
	if (!device || !device->status.present)
		return false;

	list_for_each_entry(hwid, &device->pnp.ids, list) {
		/* First, check the ACPI/PNP IDs provided by the caller. */
		/* 先匹配传统 HID/CID 或类码，保持 ACPI 驱动表的既有优先级。 */
		if (acpi_ids) {
			for (id = acpi_ids; id->id[0] || id->cls; id++) {
				if (id->id[0] && !strcmp((char *)id->id, hwid->id))
					goto out_acpi_match;
				if (id->cls && __acpi_match_device_cls(id, hwid))
					goto out_acpi_match;
			}
		}

		/*
		 * Next, check ACPI_DT_NAMESPACE_HID and try to match the
		 * "compatible" property if found.
		 */
		/* 特殊 HID 表示该节点采用 DT 风格绑定，此时改查 _DSD compatible。 */
		if (!strcmp(ACPI_DT_NAMESPACE_HID, hwid->id))
			return acpi_of_match_device(device, of_ids, of_id);
	}
	return false;

out_acpi_match:
	/* id 指向驱动静态终止数组，输出仅借用，不需要释放。 */
	if (acpi_id)
		*acpi_id = id;
	return true;
}

/**
 * acpi_match_acpi_device - Match an ACPI device against a given list of ACPI IDs
 * @ids: Array of struct acpi_device_id objects to match against.
 * @adev: The ACPI device pointer to match.
 *
 * Match the ACPI device @adev against a given list of ACPI IDs @ids.
 *
 * Return:
 * a pointer to the first matching ACPI ID on success or %NULL on failure.
 */
/*
 * 直接将 @adev 与 NULL 结尾的 @ids 表匹配，返回首个匹配表项的借用指针或
 * NULL。两个输入均不改变，设备必须 present 才可能成功；常供 ACPI 功能驱动
 * 在已有 acpi_device 上复用统一匹配规则。
 */
const struct acpi_device_id *acpi_match_acpi_device(const struct acpi_device_id *ids,
						    const struct acpi_device *adev)
{
	const struct acpi_device_id *id = NULL;

	__acpi_match_device(adev, ids, NULL, &id, NULL);
	return id;
}
EXPORT_SYMBOL_GPL(acpi_match_acpi_device);

/**
 * acpi_match_device - Match a struct device against a given list of ACPI IDs
 * @ids: Array of struct acpi_device_id object to match against.
 * @dev: The device structure to match.
 *
 * Check if @dev has a valid ACPI handle and if there is a struct acpi_device
 * object for that handle and use that object to match against a given list of
 * device IDs.
 *
 * Return a pointer to the first matching ID on success or %NULL on failure.
 */
/*
 * 物理 @dev 的便捷匹配接口：先通过 acpi_companion_match() 限定为共享 companion
 * 的主物理节点，再按 @ids 查找。返回驱动表中借用元素或 NULL，不增加 companion
 * 或表项引用。
 */
const struct acpi_device_id *acpi_match_device(const struct acpi_device_id *ids,
					       const struct device *dev)
{
	return acpi_match_acpi_device(ids, acpi_companion_match(dev));
}
EXPORT_SYMBOL_GPL(acpi_match_device);

/*
 * acpi_device_get_match_data() - 返回当前驱动匹配表随 ID 携带的私有数据。
 *
 * @dev 必须已有关联 driver；函数同时支持 acpi_match_table 和 of_match_table，
 * 并遵守主 companion 限制。返回值为驱动静态数据的借用指针，未匹配或表项未
 * 携带数据时为 NULL；不增加引用、不转移所有权。
 */
const void *acpi_device_get_match_data(const struct device *dev)
{
	const struct acpi_device_id *acpi_ids = dev->driver->acpi_match_table;
	const struct of_device_id *of_ids = dev->driver->of_match_table;
	const struct acpi_device *adev = acpi_companion_match(dev);
	const struct acpi_device_id *acpi_id = NULL;
	const struct of_device_id *of_id = NULL;

	if (!__acpi_match_device(adev, acpi_ids, of_ids, &acpi_id, &of_id))
		return NULL;

	if (acpi_id)
		return (const void *)acpi_id->driver_data;

	if (of_id)
		return of_id->data;

	return NULL;
}
EXPORT_SYMBOL_GPL(acpi_device_get_match_data);

/*
 * acpi_match_device_ids() 为旧调用者把布尔匹配转换为 errno：@device 与 @ids
 * 均借用，匹配返回 0，否则 -ENOENT。它不返回具体表项，也不修改设备。
 */
int acpi_match_device_ids(struct acpi_device *device,
			  const struct acpi_device_id *ids)
{
	return __acpi_match_device(device, ids, NULL, NULL, NULL) ? 0 : -ENOENT;
}
EXPORT_SYMBOL(acpi_match_device_ids);

/*
 * acpi_driver_match_device() - 判断通用 @dev 是否满足 @drv 的固件匹配表。
 *
 * 若驱动没有 ACPI ID 表，允许直接用 companion 的 OF compatible 表；有 ACPI
 * 表时使用统一匹配核心并执行主物理节点约束。两个参数均由驱动核心持有，
 * 函数只读并返回布尔值。
 */
bool acpi_driver_match_device(struct device *dev,
			      const struct device_driver *drv)
{
	const struct acpi_device_id *acpi_ids = drv->acpi_match_table;
	const struct of_device_id *of_ids = drv->of_match_table;

	if (!acpi_ids)
		return acpi_of_match_device(ACPI_COMPANION(dev), of_ids, NULL);

	return __acpi_match_device(acpi_companion_match(dev), acpi_ids, of_ids, NULL, NULL);
}
EXPORT_SYMBOL_GPL(acpi_driver_match_device);

/* --------------------------------------------------------------------------
                              ACPI Driver Management
   -------------------------------------------------------------------------- */

/**
 * __acpi_bus_register_driver - register a driver with the ACPI bus
 * @driver: driver being registered
 * @owner: owning module/driver
 *
 * Registers a driver with the ACPI bus.  Searches the namespace for all
 * devices that match the driver's criteria and binds.  Returns zero for
 * success or a negative error status for failure.
 */
/*
 * 将 acpi_driver 嵌入的通用 device_driver 发布到驱动核心。@driver 在注销完成
 * 前必须保持存活；@owner 由核心用于模块引用保护。函数填充 name/bus/owner 后
 * 调用 driver_register()，其成功会触发对既有 ACPI 设备的 match/probe。
 * ACPI 被禁用返回 -ENODEV，其余负 errno 来自驱动核心；失败不取得长期所有权。
 */
int __acpi_bus_register_driver(struct acpi_driver *driver, struct module *owner)
{
	if (acpi_disabled)
		return -ENODEV;
	driver->drv.name = driver->name;
	driver->drv.bus = &acpi_bus_type;
	driver->drv.owner = owner;

	return driver_register(&driver->drv);
}

EXPORT_SYMBOL(__acpi_bus_register_driver);

/**
 * acpi_bus_unregister_driver - unregisters a driver with the ACPI bus
 * @driver: driver to unregister
 *
 * Unregisters a driver with the ACPI bus.  Searches the namespace for all
 * devices that match the driver's criteria and unbinds.
 */
/*
 * 从 ACPI 总线注销 @driver。driver_unregister() 会阻止新绑定并同步解除既有
 * 绑定，期间 acpi_device_remove() 排空通知、调用驱动 remove 并释放绑定引用。
 * 返回后调用者才可释放驱动对象/模块代码；函数可能睡眠，无直接返回值。
 */
void acpi_bus_unregister_driver(struct acpi_driver *driver)
{
	driver_unregister(&driver->drv);
}

EXPORT_SYMBOL(acpi_bus_unregister_driver);

/* --------------------------------------------------------------------------
                              ACPI Bus operations
   -------------------------------------------------------------------------- */

/*
 * acpi_bus_match() 是 acpi_bus_type 的匹配回调。驱动核心保证 @dev/@drv 存活；
 * 只有扫描层允许该节点参与驱动匹配（match_driver）且驱动 ID 命中时返回 1。
 * acpi_match_device_ids() 的 0 表示匹配，因此这里取逻辑非；无状态副作用。
 */
static int acpi_bus_match(struct device *dev, const struct device_driver *drv)
{
	struct acpi_device *acpi_dev = to_acpi_device(dev);
	const struct acpi_driver *acpi_drv = to_acpi_driver(drv);

	return acpi_dev->flags.match_driver
		&& !acpi_match_device_ids(acpi_dev, acpi_drv->ids);
}

/*
 * acpi_device_uevent() 为用户空间热插拔事件追加 ACPI modalias。@dev 和 @env
 * 由 kobject 核心持有，helper 在有限环境缓冲区中写入变量；返回 0 或其负 errno。
 */
static int acpi_device_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	return __acpi_device_uevent_modalias(to_acpi_device(dev), env);
}

/*
 * acpi_device_probe() - 把已匹配的 acpi_device 绑定到传统 acpi_driver。
 *
 * 驱动核心已设置 dev->driver 并串行化同一设备的 probe/remove。阶段为：验证
 * handler 冲突 -> 调用 ops.add 建立驱动状态 -> 可选安装 Notify -> 额外持有
 * device 引用。add/通知注册可以睡眠。
 *
 * 成功返回 0，driver_data 和可选通知均有效，额外 get_device() 由 remove 对称
 * put。失败返回 -EINVAL/-ENOSYS 或驱动 errno；若通知注册失败会先调用 remove
 * 回滚 add，再清空 driver_data，不留下绑定引用。
 */
static int acpi_device_probe(struct device *dev)
{
	struct acpi_device *acpi_dev = to_acpi_device(dev);
	struct acpi_driver *acpi_drv = to_acpi_driver(dev->driver);
	int ret;

	/* 非 PNP 设备已有扫描 handler 接管时，传统 acpi_driver 不能重复绑定。 */
	if (acpi_dev->handler && !acpi_is_pnp_device(acpi_dev))
		return -EINVAL;

	if (!acpi_drv->ops.add)
		return -ENOSYS;

	/* add 是驱动资源构造边界；失败时驱动仍负责其内部部分初始化回滚。 */
	ret = acpi_drv->ops.add(acpi_dev);
	if (ret) {
		acpi_dev->driver_data = NULL;
		return ret;
	}

	pr_debug("Driver [%s] successfully bound to device [%s]\n",
		 acpi_drv->name, acpi_dev->pnp.bus_id);

	/*
	 * 只有 add 完全成功后才发布通知回调，确保回调看到初始化完成的 driver_data。
	 */
	if (acpi_drv->ops.notify) {
		ret = acpi_device_install_notify_handler(acpi_dev, acpi_drv);
		if (ret) {
			/* 通知无法建立时按 add 的逆序调用 remove，恢复未绑定状态。 */
			if (acpi_drv->ops.remove)
				acpi_drv->ops.remove(acpi_dev);

			acpi_dev->driver_data = NULL;
			return ret;
		}
	}

	pr_debug("Found driver [%s] for device [%s]\n", acpi_drv->name,
		 acpi_dev->pnp.bus_id);

	/*
	 * 绑定期额外引用允许 ACPI 驱动的异步活动依赖 dev 存活；remove 最后释放。
	 */
	get_device(dev);
	return 0;
}

/*
 * acpi_device_remove() - 解除传统 ACPI 驱动绑定并逆序撤销 probe 状态。
 *
 * 驱动核心保证 @dev 和 dev->driver 存活并与 probe 串行。先移除通知并等待回调
 * 排空，随后 ops.remove 才能安全释放回调资源；再清空 driver_data，最后释放
 * probe 持有的 device 引用。函数可睡眠，无直接返回值。
 */
static void acpi_device_remove(struct device *dev)
{
	struct acpi_device *acpi_dev = to_acpi_device(dev);
	struct acpi_driver *acpi_drv = to_acpi_driver(dev->driver);

	/* 先建立“不会再进入驱动 notify”的边界，避免 remove 与回调 UAF。 */
	if (acpi_drv->ops.notify)
		acpi_device_remove_notify_handler(acpi_dev, acpi_drv);

	if (acpi_drv->ops.remove)
		acpi_drv->ops.remove(acpi_dev);

	acpi_dev->driver_data = NULL;

	/* 与 probe 成功出口的 get_device() 严格配对。 */
	put_device(dev);
}

/*
 * ACPI 设备总线的静态操作表。bus_register() 发布后，驱动核心通过这些回调
 * 完成匹配、绑定、解绑和 modalias 事件生成；对象具有全局静态生命周期。
 */
const struct bus_type acpi_bus_type = {
	.name		= "acpi",
	.match		= acpi_bus_match,
	.probe		= acpi_device_probe,
	.remove		= acpi_device_remove,
	.uevent		= acpi_device_uevent,
};

/*
 * acpi_bus_for_each_dev() 对 ACPI 总线上每个设备同步调用 @fn(dev, data)。@data
 * 原样透传，所有权不变；总线核心在回调期间稳定当前 device。返回 0 表示遍历
 * 完成，非零为回调要求提前停止的值。回调可能睡眠，调用者不得制造锁反转。
 */
int acpi_bus_for_each_dev(int (*fn)(struct device *, void *), void *data)
{
	return bus_for_each_dev(&acpi_bus_type, NULL, data, fn);
}
EXPORT_SYMBOL_GPL(acpi_bus_for_each_dev);

/**
 * acpi_bus_find_device_by_name() - Locate an ACPI device by its name
 * @name: Name of the device to match
 *
 * The caller is responsible for calling put_device() on the returned object.
 *
 * Returns:
 * New reference to the matched device or NULL if the device can't be found.
 */
/*
 * 按设备核心名称在 ACPI 总线查找 @name。成功返回持有新引用的 struct device，
 * 调用者必须 put_device()；失败返回 NULL。@name 只在同步查找期间借用。
 */
struct device *acpi_bus_find_device_by_name(const char *name)
{
	return bus_find_device_by_name(&acpi_bus_type, NULL, name);
}
EXPORT_SYMBOL_GPL(acpi_bus_find_device_by_name);

/*
 * 子设备遍历适配上下文：fn 是调用者针对 acpi_device 的回调，data 为原样透传
 * 的私有指针。该栈对象只在同步 device_for_each_child*() 调用期间有效。
 */
struct acpi_dev_walk_context {
	int (*fn)(struct acpi_device *, void *);
	void *data;
};

/*
 * acpi_dev_for_one_check() 把通用 struct device 子节点过滤并转换为 acpi_device。
 * 非 ACPI 总线子节点返回 0 继续遍历；命中时调用用户 fn，并把其非零返回值
 * 原样用于提前停止。@context 指向外层栈对象，不可被回调保存。
 */
static int acpi_dev_for_one_check(struct device *dev, void *context)
{
	struct acpi_dev_walk_context *adwc = context;

	if (dev->bus != &acpi_bus_type)
		return 0;

	return adwc->fn(to_acpi_device(dev), adwc->data);
}
EXPORT_SYMBOL_GPL(acpi_dev_for_each_child);

/*
 * acpi_dev_for_each_child() 按正向设备子链遍历 @adev 的直接 ACPI 子设备。
 * @fn/@data 均借用且调用同步完成；非 ACPI 子节点被跳过。返回 0 表示走完，
 * 非零为 fn 的提前终止值。遍历时设备核心稳定当前子对象。
 */
int acpi_dev_for_each_child(struct acpi_device *adev,
			    int (*fn)(struct acpi_device *, void *), void *data)
{
	struct acpi_dev_walk_context adwc = {
		.fn = fn,
		.data = data,
	};

	return device_for_each_child(&adev->dev, &adwc, acpi_dev_for_one_check);
}

/*
 * 与 acpi_dev_for_each_child() 契约相同，但按反向子链调用 @fn，适合按创建顺序
 * 的逆序拆除依赖。只遍历直接子节点，不递归；返回语义和所有权不变。
 */
int acpi_dev_for_each_child_reverse(struct acpi_device *adev,
				    int (*fn)(struct acpi_device *, void *),
				    void *data)
{
	struct acpi_dev_walk_context adwc = {
		.fn = fn,
		.data = data,
	};

	return device_for_each_child_reverse(&adev->dev, &adwc, acpi_dev_for_one_check);
}

/* --------------------------------------------------------------------------
                             Initialization/Cleanup
   -------------------------------------------------------------------------- */

/*
 * acpi_bus_init_irq() - 告知固件 OSPM 当前采用的系统中断路由模型。
 *
 * 无参数；读取启动期已确定的 acpi_irq_model，将其映射为日志名称并求值根对象
 * 的 \_PIC。函数在 init 进程上下文执行 AML，可睡眠。\_PIC 不存在是允许的，
 * 表示固件无需按模式切换；未知模型或其他求值失败返回 -ENODEV，成功返回 0。
 */
static int __init acpi_bus_init_irq(void)
{
	acpi_status status;
	char *message = NULL;


	/*
	 * Let the system know what interrupt model we are using by
	 * evaluating the \_PIC object, if exists.
	 */
	/*
	 * 若 \_PIC 存在，将 Linux 已选择的 PIC/IOAPIC/GIC 等模型通知 AML，使后续
	 * _PRT 等方法返回与实际中断控制器一致的路由；方法缺失则保持固件默认值。
	 */

	switch (acpi_irq_model) {
	case ACPI_IRQ_MODEL_PIC:
		message = "PIC";
		break;
	case ACPI_IRQ_MODEL_IOAPIC:
		message = "IOAPIC";
		break;
	case ACPI_IRQ_MODEL_IOSAPIC:
		message = "IOSAPIC";
		break;
	case ACPI_IRQ_MODEL_GIC:
		message = "GIC";
		break;
	case ACPI_IRQ_MODEL_GIC_V5:
		message = "GICv5";
		break;
	case ACPI_IRQ_MODEL_PLATFORM:
		message = "platform specific model";
		break;
	case ACPI_IRQ_MODEL_LPIC:
		message = "LPIC";
		break;
	case ACPI_IRQ_MODEL_RINTC:
		message = "RINTC";
		break;
	default:
		pr_info("Unknown interrupt routing model\n");
		return -ENODEV;
	}

	pr_info("Using %s for interrupt routing\n", message);

	/* 这里才产生固件可观察副作用；此前 switch 仅验证并生成诊断名称。 */
	status = acpi_execute_simple_method(NULL, "\\_PIC", acpi_irq_model);
	if (ACPI_FAILURE(status) && (status != AE_NOT_FOUND)) {
		pr_info("_PIC evaluation failed: %s\n", acpi_format_exception(status));
		return -ENODEV;
	}

	return 0;
}

/**
 * acpi_early_init - Initialize ACPICA and populate the ACPI namespace.
 *
 * The ACPI tables are accessible after this, but the handling of events has not
 * been initialized and the global lock is not available yet, so AML should not
 * be executed at this point.
 *
 * Doing this before switching the EFI runtime services to virtual mode allows
 * the EfiBootServices memory to be freed slightly earlier on boot.
 *
 * 这是 ACPI（Advanced Configuration and Power Interface，高级配置与电源接口）
 * 子系统启动的第一阶段，在 start_kernel() 的早期被调用。
 * 完成后 ACPI 表可读，但事件处理和全局锁尚未就绪，因此此时不能执行
 * AML（ACPI Machine Language，ACPI 机器语言，固件用来描述硬件行为的字节码）。
 * 第二阶段由 acpi_subsystem_init() 完成，负责真正切换到 ACPI 模式。
 *
 * 在 EFI（Extensible Firmware Interface，可扩展固件接口）runtime services
 * 切换到虚拟地址模式之前完成此初始化，可以让 EfiBootServices（EFI 启动期
 * 服务占用的内存，启动完成后应归还给 OS）内存稍早释放，减少启动时内存占用。
 *
 * 常见问题：
 * - 固件 ACPI 表损坏或不合规：acpi_reallocate_root_table() 或
 *   acpi_initialize_subsystem() 失败，整个 ACPI 子系统被禁用，
 *   导致电源管理、热插拔等功能不可用，内核降级为遗留模式运行。
 * - x86 上 DSDT（Differentiated System Description Table，差异化系统描述表，
 *   描述主板设备及其控制方法的核心 AML 表）有 bug：通过 DMI（Desktop Management
 *   Interface，桌面管理接口，存储主板厂商/型号等标识信息的固件表）匹配表将有
 *   问题的 DSDT 替换为内核自带的修正版本（需要内核编译时包含对应的 quirk）。
 * - SCI（System Control Interrupt，系统控制中断，ACPI 用于通知 OS 发生电源/
 *   热管理事件的专用中断）触发方式错误：在 PIC（Programmable Interrupt
 *   Controller，可编程中断控制器，传统 x86 中断管理芯片）模式下若固件未指定
 *   触发类型，默认强制为电平触发（level），避免中断风暴。
 */
/*
 * 入参：无。调用位置是 start_kernel() 的 ACPI 第一阶段，处于单线程启动期，
 * 不要求调用者持锁且允许启动期分配；返回：无直接返回值。成功后 ACPICA 核心
 * 与根表可用，但事件、全局锁和完整 AML 对象尚未就绪；失败会调用 disable_acpi()
 * 发布全局禁用状态，后续各阶段据此直接跳过。该函数带 __init，代码启动后回收。
 */
void __init acpi_early_init(void)
{
	acpi_status status;

	/* acpi=off 内核参数或固件标记禁用时直接跳过 */
	if (acpi_disabled)
		return;

	pr_info("Core revision %08x\n", ACPI_CA_VERSION);

	/* enable workarounds, unless strict ACPI spec. compliance */
	/*
	 * 非严格模式下开启 AML 解释器的容错（slack）模式。
	 * 现实中大量固件的 AML 代码不完全符合 ACPI 规范，
	 * slack 模式会对常见的轻微违规行为进行宽容处理，
	 * 而不是直接返回错误导致设备无法使用。
	 * acpi_strict 由内核参数 acpi=strict 控制。
	 */
	/* 上游原注释意为：除非请求严格规范模式，否则开启固件兼容规避。 */
	if (!acpi_strict)
		acpi_gbl_enable_interpreter_slack = TRUE;

	/*
	 * 标记 ACPI 表的内存映射为永久映射，防止后续被 unmap。
	 * 必须在任何 ACPI 表访问之前设置，否则后期 ioremap（将物理地址映射到
	 * 内核虚拟地址空间的接口）释放后访问表内容会导致内核 oops（空指针或
	 * 非法地址访问引发的内核错误）。
	 */
	acpi_permanent_mmap = true;

#ifdef CONFIG_X86
	/*
	 * If the machine falls into the DMI check table,
	 * DSDT will be copied to memory.
	 * Note that calling dmi_check_system() here on other architectures
	 * would not be OK because only x86 initializes dmi early enough.
	 * Thankfully only x86 systems need such quirks for now.
	 */
	/*
	 * 对已知有缺陷的机型，将固件 DSDT 替换为内核内置的修正版本。
	 * DMI 表中记录了这些机型的特征字符串（厂商、产品名等）。
	 * 仅限 x86：其他架构的 DMI 初始化时机晚于此处，无法在这里调用。
	 */
	/*
	 * 修正说明：就本文件的 dsdt_dmi_table 而言，普通配置下回调只是设置
	 * acpi_gbl_copy_dsdt_locally，让 ACPICA 复制固件原表，并不替换为内核内置
	 * DSDT；CONFIG_ACPI_CUSTOM_DSDT 分支反而让该 DMI 回调保持空操作。这里的
	 * 真实规避目标是防止运行期固件内存中的 DSDT 被破坏。
	 */
	dmi_check_system(dsdt_dmi_table);
#endif

	/*
	 * 将固件在物理内存中的 RSDP（Root System Description Pointer，根系统描述
	 * 指针，ACPI 表的入口，由固件放置在特定内存区域供 OS 搜索）/
	 * XSDT（Extended System Description Table，扩展系统描述表，64位根表）/
	 * RSDT（Root System Description Table，根系统描述表，32位根表）等根表
	 * 重新分配到内核可长期管理的内存区域。固件原始表所在的 EfiBootServices
	 * 内存在 EFI 初始化完成后会被释放，若不提前拷贝则后续访问会越界。
	 */
	status = acpi_reallocate_root_table();
	if (ACPI_FAILURE(status)) {
		pr_err("Unable to reallocate ACPI tables\n");
		goto error0;
	}

	/*
	 * 初始化 ACPICA（ACPI Component Architecture，由 Intel 主导开发的开源
	 * ACPI 参考实现，Linux 内核直接集成其核心层）核心子系统：
	 * 分配内部数据结构、初始化 AML 解释器、解析 ACPI 命名空间（一棵描述
	 * 系统中所有硬件对象及其方法的树形结构）。
	 * 完成后可通过 acpi_get_table() 等接口访问各 ACPI 表。
	 * 注意：此时仅完成解析，事件和操作区域（Operation Region，AML 访问
	 * 硬件寄存器的抽象层）处理器尚未注册，不能调用 AML 方法（如 _INI、_STA）。
	 */
	status = acpi_initialize_subsystem();
	if (ACPI_FAILURE(status)) {
		pr_err("Unable to initialize the ACPI Interpreter\n");
		goto error0;
	}

#ifdef CONFIG_X86
	if (!acpi_ioapic) {
		/* compatible (0) means level (3) */
		/*
		 * PIC 模式下：若 MADT（Multiple APIC Description Table，多 APIC
		 * 描述表，描述系统中断控制器拓扑的 ACPI 表）中 SCI 中断的触发
		 * 类型字段为 0（compatible，含义模糊），则强制设为电平触发（level）。
		 * 边沿触发的 SCI 在 PIC 模式下容易丢中断，导致电源事件
		 * （如按电源键、电池状态变化）无响应。
		 */
		if (!(acpi_sci_flags & ACPI_MADT_TRIGGER_MASK)) {
			acpi_sci_flags &= ~ACPI_MADT_TRIGGER_MASK;
			acpi_sci_flags |= ACPI_MADT_TRIGGER_LEVEL;
		}
		/* Set PIC-mode SCI trigger type */
		/*
		 * 将确定后的触发类型写入 PIC 的 ELCR（Edge/Level Control Register，
		 * 边沿/电平控制寄存器，控制每个 IRQ 的触发方式）寄存器。
		 */
		acpi_pic_sci_set_trigger(acpi_gbl_FADT.sci_interrupt,
					 (acpi_sci_flags & ACPI_MADT_TRIGGER_MASK) >> 2);
	} else {
		/*
		 * now that acpi_gbl_FADT is initialized,
		 * update it with result from INT_SRC_OVR parsing
		 * IOAPIC（I/O Advanced Programmable Interrupt Controller，I/O 高级
		 * 可编程中断控制器，SMP 系统中替代传统 PIC 的中断路由芯片）模式下：
		 * 用 INT_SRC_OVR（Interrupt Source Override，中断源覆盖，MADT 中的
		 * 一种表项，描述 ISA IRQ 到 GSI 的重映射关系）解析出的
		 * GSI（Global System Interrupt，全局系统中断号，IOAPIC 体系下统一
		 * 编号的中断标识）覆盖 FADT（Fixed ACPI Description Table，固定 ACPI
		 * 描述表，包含电源管理寄存器地址、SCI 中断号等核心硬件参数）中的
		 * sci_interrupt 字段。
		 * 部分固件在 FADT 里填写的是 ISA IRQ 号而非 GSI，
		 * INT_SRC_OVR 提供了从 ISA IRQ 到 GSI 的重映射信息。
		 */
		acpi_gbl_FADT.sci_interrupt = acpi_sci_override_gsi;
	}
#endif
	return;

 error0:
	/* 任一步骤失败则完全禁用 ACPI，内核继续以无 ACPI 模式启动 */
	disable_acpi();
}

/**
 * acpi_subsystem_init - Finalize the early initialization of ACPI.
 *
 * Switch over the platform to the ACPI mode (if possible).
 *
 * Doing this too early is generally unsafe, but at the same time it needs to be
 * done before all things that really depend on ACPI.  The right spot appears to
 * be before finalizing the EFI initialization.
 */
/*
 * ACPI 第二阶段：在表/解释器基础已建立后切换平台到 ACPI 模式。无参数和直接
 * 返回值，单线程 init 上下文可睡眠。成功会通知 regulator 核心固件约束是完整
 * 的；失败打印错误并全局禁用 ACPI。它必须晚于 acpi_early_init()，又早于真正
 * 依赖 ACPI 的设备初始化及 EFI 最终化。
 */
void __init acpi_subsystem_init(void)
{
	acpi_status status;

	if (acpi_disabled)
		return;

	/*
	 * acpi_enable_subsystem() 的置位参数表示“跳过该阶段”。取反后只有
	 * ACPI_NO_ACPI_ENABLE 未置位，因此这里仅执行平台 ACPI 模式切换，其余
	 * 地址空间、事件和 handler 初始化留给后面的 acpi_bus_init()。
	 */
	status = acpi_enable_subsystem(~ACPI_NO_ACPI_ENABLE);
	if (ACPI_FAILURE(status)) {
		pr_err("Unable to enable ACPI\n");
		disable_acpi();
	} else {
		/*
		 * If the system is using ACPI then we can be reasonably
		 * confident that any regulators are managed by the firmware
		 * so tell the regulator core it has everything it needs to
		 * know.
		 */
		/*
		 * 使用 ACPI 的平台通常由固件完整描述电源约束，因此告知 regulator 核心
		 * 不要把未显式声明的供电关系无限期当作未知约束。
		 */
		regulator_has_full_constraints();
	}
}

/*
 * acpi_bus_table_handler() - 动态 ACPI 表事件的扇出适配器。
 *
 * @event 指示加载/卸载，@table 是 ACPICA 表对象的借用指针，@context 为注册时
 * 的 NULL。加载时先通知扫描层发现新命名空间内容，再把所有事件交给 sysfs
 * 维护表文件。返回 ACPICA 状态码；不取得 table 所有权。
 */
static acpi_status acpi_bus_table_handler(u32 event, void *table, void *context)
{
	if (event == ACPI_TABLE_EVENT_LOAD)
		acpi_scan_table_notify();

	return acpi_sysfs_table_handler(event, table, context);
}

/*
 * acpi_bus_init() - 建立可执行 AML、接收通知并参与驱动核心的 ACPI 总线基础。
 *
 * 无参数；由 acpi_init() 在 subsys_initcall 阶段调用，单线程且允许睡眠。阶段：
 * 加载表 -> 提前建立 EC -> 启用解释器并初始化对象 -> _OSC 协商 -> 注册动态表
 * 处理与 sysfs -> 建立 EC/睡眠/中断/Notify -> 创建 proc 根 -> 发布 bus_type。
 *
 * 成功返回 0，此后 acpi_scan_init() 可发布设备；任何关键失败统一 acpi_terminate()
 * 并返回 -ENODEV。部分无返回值的辅助初始化按现有设计视为可选，不能仅凭其
 * 局部失败撤销整个 ACPI。该函数带 __init，完成后代码可回收。
 */
static int __init acpi_bus_init(void)
{
	int result;
	acpi_status status;

	/* 阶段 1：建立 ACPICA OS 层运行期设施，随后才能加载并执行表。 */
	acpi_os_initialize1();

	status = acpi_load_tables();
	if (ACPI_FAILURE(status)) {
		pr_err("Unable to load the System Description Tables\n");
		goto error1;
	}

	/*
	 * ACPI 2.0 requires the EC driver to be loaded and work before the EC
	 * device is found in the namespace.
	 *
	 * This is accomplished by looking for the ECDT table and getting the EC
	 * parameters out of that.
	 *
	 * Do that before calling acpi_initialize_objects() which may trigger EC
	 * address space accesses.
	 */
	/*
	 * ACPI 2.0 要求命名空间发现 EC 设备前，EC 驱动已能工作。ECDT 可在不执行
	 * AML 的情况下给出早期 EC 参数；这必须先于 acpi_initialize_objects()，
	 * 因为对象初始化可能立即访问 EC OperationRegion。
	 */
	acpi_ec_ecdt_probe();

	/* 阶段 2：平台模式已切换，此处启用除 ACPI_ENABLE 动作外的解释器设施。 */
	status = acpi_enable_subsystem(ACPI_NO_ACPI_ENABLE);
	if (ACPI_FAILURE(status)) {
		pr_err("Unable to start the ACPI Interpreter\n");
		goto error1;
	}

	/* 完整初始化会执行规定的模块级代码/_INI，并建立可供后续 AML 使用的对象。 */
	status = acpi_initialize_objects(ACPI_FULL_INITIALIZATION);
	if (ACPI_FAILURE(status)) {
		pr_err("Unable to initialize ACPI objects\n");
		goto error1;
	}

	/*
	 * _OSC method may exist in module level code,
	 * so it must be run after ACPI_FULL_INITIALIZATION
	 */
	/* _OSC 可能由模块级 AML 动态定义，所以只能在完整对象初始化之后协商。 */
	acpi_bus_osc_negotiate_platform_control();
	acpi_bus_osc_negotiate_usb_control();

	/*
	 * _PDC control method may load dynamic SSDT tables,
	 * and we need to install the table handler before that.
	 */
	/*
	 * _PDC 可能加载 SSDT；先装 table handler，才能让扫描层和 sysfs 观察到这些
	 * 动态表。status 当前未作为致命结果使用，保留 ACPICA 的兼容启动策略。
	 */
	status = acpi_install_table_handler(acpi_bus_table_handler, NULL);

	acpi_sysfs_init();

	acpi_early_processor_control_setup();

	/*
	 * Maybe EC region is required at bus_scan/acpi_get_devices. So it
	 * is necessary to enable it as early as possible.
	 */
	/*
	 * 后续总线扫描/设备查询也可能访问 EC OperationRegion，因此此时根据 DSDT
	 * 建立常规 EC，使扫描路径不会在 handler 尚未就绪时执行 EC AML。
	 */
	acpi_ec_dsdt_probe();

	pr_info("Interpreter enabled\n");

	/* Initialize sleep structures */
	/* 初始化 ACPI 睡眠状态数据，供之后的 suspend/hibernate 路径使用。 */
	acpi_sleep_init();

	/*
	 * Get the system interrupt model and evaluate \_PIC.
	 */
	/* 固件中断路由必须在设备枚举读取 _PRT 等对象前与内核模型对齐。 */
	result = acpi_bus_init_irq();
	if (result)
		goto error1;

	/*
	 * Register for all standard device notifications.
	 */
	/*
	 * 根对象系统通知注册是热插拔事件的发布边界；成功后固件可异步进入
	 * acpi_bus_notify()，因此其依赖的解释器和事件设施必须已经完成。
	 */
	status =
	    acpi_install_notify_handler(ACPI_ROOT_OBJECT, ACPI_SYSTEM_NOTIFY,
					&acpi_bus_notify, NULL);
	if (ACPI_FAILURE(status)) {
		pr_err("Unable to register for system notifications\n");
		goto error1;
	}

	/*
	 * Create the top ACPI proc directory
	 */
	/* procfs 兼容目录创建失败允许继续；新接口主要位于 sysfs。 */
	acpi_root_dir = proc_mkdir(ACPI_BUS_FILE_ROOT, NULL);

	/* 最后发布 bus_type，之后驱动注册和设备扫描才能观察到完整回调集合。 */
	result = bus_register(&acpi_bus_type);
	if (!result)
		return 0;

	/* Mimic structured exception handling */
	/*
	 * 所有关键初始化失败汇合到同一终止点。ACPICA 负责撤销已经建立的解释器、
	 * 事件和表等内部设施；bus_register() 未成功，因此无需 bus_unregister()。
	 * 根据失败时机，proc/sysfs 辅助节点可能已经创建，本标签不逐项删除它们：
	 * 外层会释放 acpi_kobj 并禁用 ACPI，而 proc 兼容根可能保留为空目录。因此
	 * 这是“终止 ACPICA 并禁止继续使用”的失败收束，不是所有外部副作用的完整
	 * 事务式回滚。
	 */
      error1:
	acpi_terminate();
	return -ENODEV;
}

/*
 * /sys/firmware/acpi 对应的 kobject 根。acpi_init() 创建并持有其初始引用，
 * 后续 ACPI 子模块借用该全局挂接属性组；导出后运行期只读。
 */
struct kobject *acpi_kobj;
EXPORT_SYMBOL_GPL(acpi_kobj);

/*
 * 体系结构可覆盖的 ACPI 晚期初始化钩子。默认弱实现无参数、无返回和副作用；
 * 被覆盖实现运行在 init 进程上下文，必须在 acpi_scan_init() 前完成架构准备。
 */
void __weak __init acpi_arch_init(void) { }

/*
 * acpi_init() - subsys_initcall 阶段的 ACPI 总入口。
 *
 * 无参数；串行启动上下文可睡眠。先创建 sysfs 根并初始化 PRMT/PCC，再调用
 * acpi_bus_init() 建立共同基础；成功后按依赖顺序初始化 FFH、PCI 配置、VIOT、
 * APEI/GHES、架构钩子、命名空间扫描、EC、调试、睡眠/唤醒和 \_SB 通知。
 *
 * ACPI 禁用返回 -ENODEV，kobject 分配失败返回 -ENOMEM，总线失败原样返回并
 * put kobject、disable_acpi。成功返回 0；后续可选子系统 helper 的局部失败按
 * 各自接口降级，不回滚已经发布的 ACPI 总线。
 */
static int __init acpi_init(void)
{
	int result;

	if (acpi_disabled) {
		pr_info("Interpreter disabled.\n");
		return -ENODEV;
	}

	/* 阶段 1：发布固件 sysfs 根；失败时尚无其他本阶段资源。 */
	acpi_kobj = kobject_create_and_add("acpi", firmware_kobj);
	if (!acpi_kobj) {
		pr_err("Failed to register kobject\n");
		return -ENOMEM;
	}

	/* 阶段 2：建立总线初始化前可能依赖的固件运行时机制。 */
	init_prmt();
	acpi_init_pcc();
	/* acpi_bus_init() 是共同提交点；失败时撤销 sysfs 根并全局禁用 ACPI。 */
	result = acpi_bus_init();
	if (result) {
		kobject_put(acpi_kobj);
		disable_acpi();
		return result;
	}
	/*
	 * 阶段 3：基础总线已发布，按“架构/错误处理 -> 扫描 -> 设备服务”顺序启动
	 * 可选组件。它们依赖 ACPI，但局部不可用不应让整个平台退回无 ACPI 模式。
	 */
	acpi_init_ffh();

	pci_mmcfg_late_init();
	acpi_viot_early_init();
	acpi_hest_init();
	acpi_ghes_init();
	acpi_arch_init();
	acpi_scan_init();
	acpi_ec_init();
	acpi_debugfs_init();
	acpi_sleep_proc_init();
	acpi_wakeup_device_init();
	acpi_debugger_init();
	acpi_setup_sb_notify_handler();
	acpi_viot_init();
	return 0;
}

/* 在设备驱动普遍注册前建立 ACPI 总线和命名空间设备。 */
subsys_initcall(acpi_init);
