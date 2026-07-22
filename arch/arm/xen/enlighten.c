// SPDX-License-Identifier: GPL-2.0-only
#include <xen/xen.h>
#include <xen/events.h>
#include <xen/grant_table.h>
#include <xen/hvm.h>
#include <xen/interface/vcpu.h>
#include <xen/interface/xen.h>
#include <xen/interface/memory.h>
#include <xen/interface/hvm/params.h>
#include <xen/features.h>
#include <xen/platform_pci.h>
#include <xen/xenbus.h>
#include <xen/page.h>
#include <xen/interface/sched.h>
#include <xen/xen-ops.h>
#include <asm/xen/hypervisor.h>
#include <asm/xen/hypercall.h>
#include <asm/system_misc.h>
#include <asm/efi.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/cpuidle.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/console.h>
#include <linux/pvclock_gtod.h>
#include <linux/reboot.h>
#include <linux/time64.h>
#include <linux/timekeeping.h>
#include <linux/timekeeper_internal.h>
#include <linux/acpi.h>
#include <linux/virtio_anchor.h>

#include <linux/mm.h>

/*
 * 本文件是 ARM Xen guest 的体系结构接入层，而不是完整的 Xen 驱动。
 *
 * 宏观启动链如下：
 *
 *   xen_early_init()
 *     由扁平设备树确认 Xen/版本，建立“当前是 Xen HVM 域”的身份
 *          |
 *   xen_guest_init()                 [early_initcall]
 *     发现事件 IRQ 和 grant 区域，映射 shared_info，分配每 CPU vcpu_info，
 *     初始化 grant table/event channel，并登记 CPU hotplug 回调
 *          |
 *   xen_starting_cpu()
 *     每个 CPU 上线时向 hypervisor 注册自己的 vcpu_info 并开放事件 IRQ
 *          |
 *   xen_late_init()                  [late_initcall]
 *     接入关机/重启、guest wallclock、runstate 与 Xen 时钟
 *
 * Xen 不直接调用 guest 的普通 C 函数。双方以 hypercall（guest 主动请求）、
 * shared_info/vcpu_info（共享状态）和 event channel（异步通知）协作。这样
 * 避免模拟完整物理设备，开销低；代价是共享结构的内存顺序、每 vCPU
 * 生命周期和失败回滚必须严格遵守 Xen ABI。
 */

/*
 * ARM guest 没有 x86 PV 启动时由 hypervisor 直接传入的 start_info 页，
 * 因而提供一个内核自有的兼容对象。xen_start_info 是通用 Xen 代码使用
 * 的稳定指针，导出后模块只能借用，不能替换或释放其目标。
 */
static struct start_info _xen_start_info;
struct start_info *xen_start_info = &_xen_start_info;
EXPORT_SYMBOL(xen_start_info);

enum xen_domain_type xen_domain_type = XEN_NATIVE;
/*
 * 域类型在早期默认为“裸机”，xen_early_init() 确认设备树后切为 HVM。
 * 大量 xen_domain()/xen_hvm_domain() 判断依赖它，所以只能先发现后发布。
 */
EXPORT_SYMBOL(xen_domain_type);

/*
 * 真 shared_info 映射前让全局指针指向零填充 dummy，避免极早期通用代码
 * 解引用 NULL。xen_guest_init() 成功执行 XENMEM_add_to_physmap 后一次性
 * 改指针；映射页此后由 Xen ABI 与 guest 共同使用，不能当普通页释放。
 */
struct shared_info xen_dummy_shared_info;
struct shared_info *HYPERVISOR_shared_info = (void *)&xen_dummy_shared_info;

/* xen_vcpu：每个 Linux CPU 当前注册给 Xen 的 vcpu_info 地址。 */
DEFINE_PER_CPU(struct vcpu_info *, xen_vcpu);
/* xen_vcpu_info：guest 分配的整块 percpu 存储，是上述各指针的所有者。 */
static struct vcpu_info __percpu *xen_vcpu_info;

/* Linux <-> Xen vCPU id mapping */
DEFINE_PER_CPU(uint32_t, xen_vcpu_id);
EXPORT_PER_CPU_SYMBOL(xen_vcpu_id);

/* These are unused until we support booting "pre-ballooned" */
unsigned long xen_released_pages;
/* 预 balloon 启动将用它记录 hypervisor 额外提供的 guest 物理内存区域。 */
struct xen_memory_region xen_extra_mem[XEN_EXTRA_MEM_MAX_REGIONS] __initdata;

/* 事件通道汇聚到的每 CPU PPI IRQ；初始化后只读，故标为 read-mostly。 */
static __read_mostly unsigned int xen_events_irq;
/* DT 提供的 grant-table guest 物理起点；0 表示需由 balloon 页自动映射。 */
static __read_mostly phys_addr_t xen_grant_frames;

/* DT reg[0] 固定为 grant table，reg[1...] 是可作 Xen scratch 的扩展区域。 */
#define GRANT_TABLE_INDEX   0
#define EXT_REGION_INDEX    1

/* 启动域/特权域等 SIF_* 位，供通用 Xen 层和模块查询。 */
uint32_t xen_start_flags;
EXPORT_SYMBOL(xen_start_flags);

int xen_unmap_domain_gfn_range(struct vm_area_struct *vma,
			       int nr, struct page **pages)
{
	/*
	 * ARM 的体系结构导出包装：从 vma 撤销此前映射的 nr 个 domain GFN，
	 * pages 是对应的 Linux page 数组。实际 PTE/GFN 解除工作由自动转换
	 * 层完成；返回 0 或其负 errno，调用者仍负责自己的 VMA 生命周期。
	 */
	return xen_xlate_unmap_gfn_range(vma, nr, pages);
}
EXPORT_SYMBOL_GPL(xen_unmap_domain_gfn_range);

static void xen_read_wallclock(struct timespec64 *ts)
{
	/* version 是 Xen 类 seqlock 序号；奇数表示 hypervisor 正在更新。 */
	u32 version;
	/* now=启动时墙钟，ts_monotonic=guest 自启动以来的单调时间。 */
	struct timespec64 now, ts_monotonic;
	/* s/wall_clock 均指向 Xen 映射的共享页，只借用、不拥有。 */
	struct shared_info *s = HYPERVISOR_shared_info;
	struct pvclock_wall_clock *wall_clock = &(s->wc);

	/* get wallclock at system boot */
	do {
		version = wall_clock->version;
		rmb();		/* fetch version before time */
		now.tv_sec  = ((uint64_t)wall_clock->sec_hi << 32) | wall_clock->sec;
		now.tv_nsec = wall_clock->nsec;
		rmb();		/* fetch time before checking version */
	} while ((wall_clock->version & 1) || (version != wall_clock->version));
	/*
	 * 两次 rmb 与版本复核保证 sec_hi/sec/nsec 来自同一次 hypervisor 更新。
	 * 若写入跨越读取，版本变奇或前后不等便重试；无需 guest 自旋锁，
	 * 因为写端在 hypervisor，普通 Linux 锁无法保护它。
	 */

	/* time since system boot */
	ktime_get_ts64(&ts_monotonic);
	*ts = timespec64_add(now, ts_monotonic);
	/* 输出的是当前 UTC 墙钟，而非单纯的 boot timestamp。 */
}

/*
 * timekeeping 更新通知：把初始域的 Linux 墙钟同步回 Xen 平台时钟。
 * nb 是 notifier 自身、was_set 表示发生显式 settimeofday、priv 指向由
 * timekeeping 核心串行保护的 timekeeper。回调不另加锁，依赖调用核心
 * 的 serialization；hypercall 失败被刻意忽略，不能阻断本地时间更新。
 */
static int xen_pvclock_gtod_notify(struct notifier_block *nb,
				   unsigned long was_set, void *priv)
{
	/* Protected by the calling core code serialization */
	static struct timespec64 next_sync;

	/* op 是传给 XENPF_settime64 的 ABI 请求；now/system_time 是两个时基。 */
	struct xen_platform_op op;
	struct timespec64 now, system_time;
	struct timekeeper *tk = priv;

	now.tv_sec = tk->xtime_sec;
	now.tv_nsec = (long)(tk->tkr_mono.xtime_nsec >> tk->tkr_mono.shift);
	system_time = timespec64_add(now, tk->wall_to_monotonic);
	/* system_time 对应 Xen 所需的单调时基纳秒值，now 是当前墙钟。 */

	/*
	 * We only take the expensive HV call when the clock was set
	 * or when the 11 minutes RTC synchronization time elapsed.
	 */
	if (!was_set && timespec64_compare(&now, &next_sync) < 0)
		return NOTIFY_OK;

	op.cmd = XENPF_settime64;
	op.u.settime64.mbz = 0;
	op.u.settime64.secs = now.tv_sec;
	op.u.settime64.nsecs = now.tv_nsec;
	op.u.settime64.system_time = timespec64_to_ns(&system_time);
	(void)HYPERVISOR_platform_op(&op);
	/* best-effort 同步：平台时钟不可写不应让 notifier 链失败。 */

	/*
	 * Move the next drift compensation time 11 minutes
	 * ahead. That's emulating the sync_cmos_clock() update for
	 * the hardware RTC.
	 */
	next_sync = now;
	next_sync.tv_sec += 11 * 60;

	return NOTIFY_OK;
}

static struct notifier_block xen_pvclock_gtod_notifier = {
	/* 注册在 pvclock GTOD 链上，只在 initial domain 中启用。 */
	.notifier_call = xen_pvclock_gtod_notify,
};

/*
 * CPU hotplug STARTING 回调，为 Linux CPU cpu 建立 Xen vCPU 快速共享区。
 * vcpu_info 保存 event-channel pending/upcall mask 及 pvclock 状态，处在
 * 高频路径；把 guest 自有 percpu 地址注册给 Xen，避免每次读 shared_info
 * 中仅为 boot CPU 保留的槽位或使用 hypercall。
 */
static int xen_starting_cpu(unsigned int cpu)
{
	/* info 是 hypercall ABI 的“机器帧号+页内偏移”；vcpup 是内核虚址。 */
	struct vcpu_register_vcpu_info info;
	struct vcpu_info *vcpup;
	int err;

	/* 
	 * VCPUOP_register_vcpu_info cannot be called twice for the same
	 * vcpu, so if vcpu_info is already registered, just get out. This
	 * can happen with cpu-hotplug.
	 */
	if (per_cpu(xen_vcpu, cpu) != NULL)
		goto after_register_vcpu_info;

	pr_info("Xen: initializing cpu%d\n", cpu);
	vcpup = per_cpu_ptr(xen_vcpu_info, cpu);

	/* percpu 对象可能不页对齐，故必须同时传 GFN/MFN 和精确页内 offset。 */
	info.mfn = percpu_to_gfn(vcpup);
	info.offset = xen_offset_in_page(vcpup);

	err = HYPERVISOR_vcpu_op(VCPUOP_register_vcpu_info, xen_vcpu_nr(cpu),
				 &info);
	BUG_ON(err);
	/* 注册失败意味着 event/pvclock 基础 ABI 无法成立，当前实现不可降级。 */
	per_cpu(xen_vcpu, cpu) = vcpup;

after_register_vcpu_info:
	/* vcpu_info 可见之后才开放 PPI，避免回调读到未注册的 percpu 指针。 */
	enable_percpu_irq(xen_events_irq, 0);
	return 0;
}

static int xen_dying_cpu(unsigned int cpu)
{
	/*
	 * CPU 下线先关闭本 CPU event PPI。Xen 不允许重复 register，所以不
	 * 注销/清空 xen_vcpu；同一 CPU 再上线会直接复用已经注册的共享区。
	 */
	disable_percpu_irq(xen_events_irq);
	return 0;
}

void xen_reboot(int reason)
{
	/* reason 是 Xen SHUTDOWN_* ABI 值；sched_shutdown 请求不会正常返回。 */
	struct sched_shutdown r = { .reason = reason };
	int rc;

	rc = HYPERVISOR_sched_op(SCHEDOP_shutdown, &r);
	BUG_ON(rc);
	/* 若 hypervisor 拒绝关机，guest 无可靠继续运行语义，因此 BUG。 */
}

/* Linux restart-handler 适配层，忽略 notifier 参数并请求 Xen 重启域。 */
static int xen_restart(struct notifier_block *nb, unsigned long action,
		       void *data)
{
	xen_reboot(SHUTDOWN_reboot);

	return NOTIFY_DONE;
}

static struct notifier_block xen_restart_nb = {
	.notifier_call = xen_restart,
	/* 较高优先级确保 Xen 接管重启，而不是尝试不存在的板级复位设备。 */
	.priority = 192,
};

/* platform_power_off 的无参数适配函数。 */
static void xen_power_off(void)
{
	xen_reboot(SHUTDOWN_poweroff);
}

static irqreturn_t xen_arm_callback(int irq, void *arg)
{
	/*
	 * 物理 PPI 只是“有事件待处理”的门铃；真正事件位在 shared_info 的
	 * event-channel bitmap。upcall 分发所有 pending channel 到绑定 handler。
	 * irq/arg 由通用 IRQ 框架传入，本适配层无需读取。
	 */
	xen_evtchn_do_upcall();
	return IRQ_HANDLED;
}

static __initdata struct {
	/* 无版本 compatible、带版本字符串前缀、解析出的版本子串、命中标志。 */
	const char *compat;
	const char *prefix;
	const char *version;
	bool found;
} hyper_node = {"xen,xen", "xen,xen-", NULL, false};
/* __initdata 表示设备树早期发现结束后可回收；后续以 xen_domain_type 为准。 */

/*
 * of_scan_flat_dt() 回调，只检查深度 1 的 /hypervisor 节点。node 是扁平
 * DT 节点偏移，uname 是单元名，depth 是树深，data 当前未使用。它既
 * 接受通用 "xen,xen"，也从形如 "xen,xen-4.17" 中借用版本字符串。
 */
static int __init fdt_find_hyper_node(unsigned long node, const char *uname,
				      int depth, void *data)
{
	const char *s = NULL;
	/* len 是 compatible 属性缓冲区长度，prefix_len 是 "xen,xen-" 长度。 */
	int len;
	size_t prefix_len = strlen(hyper_node.prefix);

	if (depth != 1 || strcmp(uname, "hypervisor") != 0)
		return 0;

	if (of_flat_dt_is_compatible(node, hyper_node.compat))
		hyper_node.found = true;

	s = of_get_flat_dt_prop(node, "compatible", &len);
	if (s && len > 0 && strnlen(s, len) < len &&
	    len > prefix_len + 3 &&
	    !strncmp(hyper_node.prefix, s, prefix_len))
		hyper_node.version = s + prefix_len;
	/*
	 * strnlen < len 验证属性内存在 NUL，len > prefix+3 拒绝过短版本；
	 * version 指向 FDT 内存，不复制，所以只允许在 init 阶段使用。
	 */

	/*
	 * Check if Xen supports EFI by checking whether there is the
	 * "/hypervisor/uefi" node in DT. If so, runtime services are available
	 * through proxy functions (e.g. in case of Xen dom0 EFI implementation
	 * they call special hypercall which executes relevant EFI functions)
	 * and that is why they are always enabled.
	 */
	if (IS_ENABLED(CONFIG_XEN_EFI)) {
		if ((of_get_flat_dt_subnode_by_name(node, "uefi") > 0) &&
		    !efi_runtime_disabled())
			set_bit(EFI_RUNTIME_SERVICES, &efi.flags);
	}

	return 0;
}

/*
 * see Documentation/devicetree/bindings/arm/xen.txt for the
 * documentation of the Xen Device Tree format.
 */
void __init xen_early_init(void)
{
	/* 第一阶段只做身份发现，不分配页、注册 IRQ 或发依赖完整内核的请求。 */
	of_scan_flat_dt(fdt_find_hyper_node, NULL);
	if (!hyper_node.found) {
		pr_debug("No Xen support\n");
		return;
	}

	if (hyper_node.version == NULL) {
		pr_debug("Xen version not found\n");
		return;
	}

	pr_info("Xen %s support found\n", hyper_node.version);

	xen_domain_type = XEN_HVM_DOMAIN;
	/* 从此 xen_domain() 为真，后续 early_initcall 才会进入完整初始化。 */

	/* 读取 hypervisor feature bitmap，供 grant、callback 等路径选择能力。 */
	xen_setup_features();

	if (xen_feature(XENFEAT_dom0))
		/* dom0 同时是初始域和特权域，可以管理平台资源与其他 domain。 */
		xen_start_flags |= SIF_INITDOMAIN|SIF_PRIVILEGED;

	if (!console_set_on_cmdline && !xen_initial_domain())
		/* 普通 guest 无显式 console 参数时优先使用 Xen hvc0。 */
		add_preferred_console("hvc", 0, NULL);
}

/*
 * ACPI 启动的 guest 从 HVM_PARAM_CALLBACK_IRQ 查询 event-channel 门铃 PPI。
 * Xen 把类型、IRQ 号、触发方式和极性打包在 value 中；解析后交 ACPI GSI
 * 层建立 Linux IRQ 映射。查询失败或类型不是 PPI 时以 0 表示未发现。
 */
static void __init xen_acpi_guest_init(void)
{
#ifdef CONFIG_ACPI
	struct xen_hvm_param a;
	/* interrupt 是固件 GSI/PPI 号，trigger/polarity 是 ACPI 编码属性。 */
	int interrupt, trigger, polarity;

	a.domid = DOMID_SELF;
	a.index = HVM_PARAM_CALLBACK_IRQ;

	if (HYPERVISOR_hvm_op(HVMOP_get_param, &a)
	    || (a.value >> 56) != HVM_PARAM_CALLBACK_TYPE_PPI) {
		xen_events_irq = 0;
		return;
	}

	interrupt = a.value & 0xff;
	trigger = ((a.value >> 8) & 0x1) ? ACPI_EDGE_SENSITIVE
					 : ACPI_LEVEL_SENSITIVE;
	polarity = ((a.value >> 8) & 0x2) ? ACPI_ACTIVE_LOW
					  : ACPI_ACTIVE_HIGH;
	xen_events_irq = acpi_register_gsi(NULL, interrupt, trigger, polarity);
#endif
}

#ifdef CONFIG_XEN_UNPOPULATED_ALLOC
/*
 * A type-less specific Xen resource which contains extended regions
 * (unused regions of guest physical address space provided by the hypervisor).
 */
static struct resource xen_resource = {
	.name = "Xen unused space",
};
/* 父 resource 覆盖所有扩展区的最小到最大范围，洞由 child resource 占住。 */

/*
 * 把 DT reg[1...] 描述的未填充 guest 物理地址空间转成 resource allocator
 * 可用的 Xen scratch 区。输出 *res 借用静态 xen_resource，不得释放；
 * 仅 Xen+DT（非 ACPI）路径支持。成功返回 0，失败为负 errno。
 */
int __init arch_xen_unpopulated_init(struct resource **res)
{
	/* np 是需 put 的 DT 引用；regs 是临时区域数组；tmp_res 表示永久洞。 */
	struct device_node *np;
	struct resource *regs, *tmp_res;
	uint64_t min_gpaddr = -1, max_gpaddr = 0;
	/* nr_reg 是扩展区数量，i 是遍历下标，rc 同时承载最终返回码。 */
	unsigned int i, nr_reg = 0;
	int rc;

	if (!xen_domain())
		return -ENODEV;

	if (!acpi_disabled)
		return -ENODEV;

	np = of_find_compatible_node(NULL, NULL, "xen,xen");
	if (WARN_ON(!np))
		return -ENODEV;

	/* Skip region 0 which is reserved for grant table space */
	while (of_get_address(np, nr_reg + EXT_REGION_INDEX, NULL, NULL))
		nr_reg++;
	/* 只计数属性，不取得所有权；index 0 的 grant 区绝不能当空闲区。 */

	if (!nr_reg) {
		pr_err("No extended regions are found\n");
		of_node_put(np);
		return -EINVAL;
	}

	regs = kzalloc_objs(*regs, nr_reg);
	if (!regs) {
		of_node_put(np);
		return -ENOMEM;
	}

	/*
	 * Create resource from extended regions provided by the hypervisor to be
	 * used as unused address space for Xen scratch pages.
	 */
	for (i = 0; i < nr_reg; i++) {
		rc = of_address_to_resource(np, i + EXT_REGION_INDEX, &regs[i]);
		if (rc)
			goto err;

		if (max_gpaddr < regs[i].end)
			max_gpaddr = regs[i].end;
		if (min_gpaddr > regs[i].start)
			min_gpaddr = regs[i].start;
	}
	/* 假定 DT 区域按地址递增；下面用相邻项检测重叠并构造空洞。 */

	xen_resource.start = min_gpaddr;
	xen_resource.end = max_gpaddr;

	/*
	 * Mark holes between extended regions as unavailable. The rest of that
	 * address space will be available for the allocation.
	 */
	for (i = 1; i < nr_reg; i++) {
		resource_size_t start, end;
		/* start/end 是两个可用扩展区之间不可分配的闭区间。 */

		/* There is an overlap between regions */
		if (regs[i - 1].end + 1 > regs[i].start) {
			rc = -EINVAL;
			goto err;
		}

		/* There is no hole between regions */
		if (regs[i - 1].end + 1 == regs[i].start)
			continue;

		start = regs[i - 1].end + 1;
		end = regs[i].start - 1;

		tmp_res = kzalloc_obj(*tmp_res);
		if (!tmp_res) {
			rc = -ENOMEM;
			goto err;
		}

		tmp_res->name = "Unavailable space";
		tmp_res->start = start;
		tmp_res->end = end;

		rc = insert_resource(&xen_resource, tmp_res);
		if (rc) {
			pr_err("Cannot insert resource %pR (%d)\n", tmp_res, rc);
			kfree(tmp_res);
			goto err;
		}
	}

	/* 只有所有区域和洞均建立成功后才向调用者发布父 resource。 */
	*res = &xen_resource;

err:
	/* np/regs 是本函数临时所有权；已插入的 child resource 归资源树管理。 */
	of_node_put(np);
	kfree(regs);
	return rc;
}
#endif

static void __init xen_dt_guest_init(void)
{
	/* xen_node 持有 DT 引用；res 暂存 reg[0] grant-table 固件资源。 */
	struct device_node *xen_node;
	struct resource res;

	xen_node = of_find_compatible_node(NULL, NULL, "xen,xen");
	if (!xen_node) {
		pr_err("Xen support was detected before, but it has disappeared\n");
		return;
	}

	xen_events_irq = irq_of_parse_and_map(xen_node, 0);
	/* interrupt[0] 映射为 Linux IRQ，稍后 request_percpu_irq() 申请。 */

	if (of_address_to_resource(xen_node, GRANT_TABLE_INDEX, &res)) {
		pr_err("Xen grant table region is not found\n");
		of_node_put(xen_node);
		return;
	}
	of_node_put(xen_node);
	xen_grant_frames = res.start;
	/* 这里只保存 guest 物理起点，grant-table 通用层稍后完成实际映射。 */
}

/*
 * ARM Xen guest 的主体初始化，成功后最少建立三条通信通道：
 *
 * 1. shared_info/vcpu_info：共享事件和时间状态；
 * 2. event PPI：hypervisor 通知 guest 处理 event channel；
 * 3. grant table：domain 间安全共享页帧。
 *
 * 函数在 early_initcall 执行，此时可分配内存/IRQ，但各 secondary CPU
 * 尚通过后面的 CPUHP 回调逐个注册。任一步失败返回负 errno；若关键
 * shared_info 映射 hypercall 失败则 BUG，因为 guest 身份已确认且无法
 * 在缺失基础 ABI 的情况下安全退回裸机模式。
 */
static int __init xen_guest_init(void)
{
	/* xatp 描述 shared_info 的 physmap 映射；shared_info_page 是 guest 页。 */
	struct xen_add_to_physmap xatp;
	struct shared_info *shared_info_page = NULL;
	int rc, cpu;
	/* rc 传递各子系统错误，cpu 用于初始化 possible CPU 的直映射 ID。 */

	if (!xen_domain())
		return 0;

	if (IS_ENABLED(CONFIG_XEN_VIRTIO))
		/* 让受限 virtio DMA 通过 Xen grant/共享内存策略检查 guest 页访问。 */
		virtio_set_mem_acc_cb(xen_virtio_restricted_mem_acc);

	if (!acpi_disabled)
		xen_acpi_guest_init();
	else
		xen_dt_guest_init();

	if (!xen_events_irq) {
		pr_err("Xen event channel interrupt not found\n");
		return -ENODEV;
	}

	/*
	 * The fdt parsing codes have set EFI_RUNTIME_SERVICES if Xen EFI
	 * parameters are found. Force enable runtime services.
	 */
	if (efi_enabled(EFI_RUNTIME_SERVICES))
		xen_efi_runtime_setup();

	shared_info_page = (struct shared_info *)get_zeroed_page(GFP_KERNEL);
	/* 页对齐且清零，满足 XENMAPSPACE_shared_info 的 ABI 与信息泄漏要求。 */

	if (!shared_info_page) {
		pr_err("not enough memory\n");
		return -ENOMEM;
	}
	xatp.domid = DOMID_SELF;
	xatp.idx = 0;
	xatp.space = XENMAPSPACE_shared_info;
	xatp.gpfn = virt_to_gfn(shared_info_page);
	/* DOMID_SELF 的 index 0 shared_info 映射到刚分配 guest PFN。 */
	if (HYPERVISOR_memory_op(XENMEM_add_to_physmap, &xatp))
		BUG();

	HYPERVISOR_shared_info = (struct shared_info *)shared_info_page;
	/* hypercall 成功后发布真共享页，替换只用于早期防空指针的 dummy。 */

	/* xen_vcpu is a pointer to the vcpu_info struct in the shared_info
	 * page, we use it in the event channel upcall and in some pvclock
	 * related functions. 
	 * The shared info contains exactly 1 CPU (the boot CPU). The guest
	 * is required to use VCPUOP_register_vcpu_info to place vcpu info
	 * for secondary CPUs as they are brought up.
	 * For uniformity we use VCPUOP_register_vcpu_info even on cpu0.
	 */
	xen_vcpu_info = __alloc_percpu(sizeof(struct vcpu_info),
				       1 << fls(sizeof(struct vcpu_info) - 1));
	/*
	 * 对齐取不小于结构大小的 2 次幂，保证任一 vcpu_info 不跨页；否则
	 * VCPUOP_register_vcpu_info 的单个 mfn+offset 无法完整描述它。
	 */
	if (xen_vcpu_info == NULL)
		return -ENOMEM;

	/* Direct vCPU id mapping for ARM guests. */
	for_each_possible_cpu(cpu)
		per_cpu(xen_vcpu_id, cpu) = cpu;
	/* ARM 固件/CPU 拓扑约定 Linux CPU 号与 Xen vCPU 号直接对应。 */

	if (!xen_grant_frames) {
		/* ACPI/无固定 DT 区域：balloon 若干页并建立自动转换 grant 映射。 */
		xen_auto_xlat_grant_frames.count = gnttab_max_grant_frames();
		rc = xen_xlate_map_ballooned_pages(&xen_auto_xlat_grant_frames.pfn,
										   &xen_auto_xlat_grant_frames.vaddr,
										   xen_auto_xlat_grant_frames.count);
	} else
		/* DT 路径：使用 hypervisor 预留的连续 guest 物理 grant 区。 */
		rc = gnttab_setup_auto_xlat_frames(xen_grant_frames);
	if (rc) {
		free_percpu(xen_vcpu_info);
		return rc;
	}
	gnttab_init();
	/* 映射页准备完成后，通用 grant-table 层才可发布分配/映射接口。 */

	/*
	 * Making sure board specific code will not set up ops for
	 * cpu idle and cpu freq.
	 */
	disable_cpuidle();
	disable_cpufreq();

	xen_init_IRQ();
	/* 初始化 event-channel 到 Linux IRQ 的通用绑定/分发层。 */

	if (request_percpu_irq(xen_events_irq, xen_arm_callback,
			       "events", &xen_vcpu)) {
		pr_err("Error request IRQ %d\n", xen_events_irq);
		return -EINVAL;
	}

	if (xen_initial_domain())
		/* 只有 dom0 负责把内核墙钟反向同步给 hypervisor 平台时钟。 */
		pvclock_gtod_register_notifier(&xen_pvclock_gtod_notifier);

	/*
	 * 注册动态 CPU 生命周期；框架会为已在线 CPU 调 starting，并在以后
	 * 上/下线时保持 vcpu_info 注册与 event PPI 启用状态一致。
	 */
	return cpuhp_setup_state(CPUHP_AP_ARM_XEN_STARTING,
				 "arm/xen:starting", xen_starting_cpu,
				 xen_dying_cpu);
}
early_initcall(xen_guest_init);

static int xen_starting_runstate_cpu(unsigned int cpu)
{
	/* 为 cpu 注册 Xen runstate 共享区，供 steal/runnable/blocked 时间统计。 */
	xen_setup_runstate_info(cpu);
	return 0;
}

static int __init xen_late_init(void)
{
	/*
	 * 晚期阶段接管平台生命周期和时间。此时 early init 已确认域并建立
	 * hypercall/event 基础；非 Xen 启动返回 -ENODEV 只表示本 initcall
	 * 不适用，不影响普通 ARM 启动。
	 */
	if (!xen_domain())
		return -ENODEV;

	register_platform_power_off(xen_power_off);
	register_restart_handler(&xen_restart_nb);
	if (!xen_initial_domain()) {
		/* 普通 guest 从 Xen 共享墙钟初始化 Linux CLOCK_REALTIME。 */
		struct timespec64 ts;
		xen_read_wallclock(&ts);
		do_settimeofday64(&ts);
	}

	if (xen_kernel_unmapped_at_usr())
		/* KPTI 类用户页表隔离模式不建立此 runstate/time 快速映射路径。 */
		return 0;

	xen_time_setup_guest();
	/* 接入 Xen pvclock clocksource/clockevent，随后逐 CPU 注册 runstate。 */

	return cpuhp_setup_state(CPUHP_AP_ARM_XEN_RUNSTATE_STARTING,
				 "arm/xen_runstate:starting",
				 xen_starting_runstate_cpu, NULL);
}
late_initcall(xen_late_init);


/* empty stubs */
/*
 * 通用 Xen suspend/resume 层要求各架构提供这些钩子；当前 ARM HVM 没有
 * 额外的页表、计时器或 hypercall-page 切换工作，故实现为空。参数
 * suspend_cancelled 表示暂停被取消，但同样无需处理。保留真实符号可让
 * 通用代码不包含 ARM 条件编译，也为未来补充架构恢复序列留下 ABI 点。
 */
void xen_arch_pre_suspend(void) { }
void xen_arch_post_suspend(int suspend_cancelled) { }
void xen_timer_resume(void) { }
void xen_arch_resume(void) { }
void xen_arch_suspend(void) { }


/* In the hypercall.S file. */
/*
 * 下列符号的实现是 hypercall.S 中的架构调用桩：它们按 Xen ARM ABI
 * 布置寄存器并执行 HVC。此处只把接口以 GPL 符号导出给 Xen 子模块；
 * 导出不代表普通驱动可绕过相应 Xen 子系统的锁、引用和参数校验。
 */
EXPORT_SYMBOL_GPL(HYPERVISOR_event_channel_op);
EXPORT_SYMBOL_GPL(HYPERVISOR_grant_table_op);
EXPORT_SYMBOL_GPL(HYPERVISOR_xen_version);
EXPORT_SYMBOL_GPL(HYPERVISOR_console_io);
EXPORT_SYMBOL_GPL(HYPERVISOR_sched_op);
EXPORT_SYMBOL_GPL(HYPERVISOR_hvm_op);
EXPORT_SYMBOL_GPL(HYPERVISOR_memory_op);
EXPORT_SYMBOL_GPL(HYPERVISOR_physdev_op);
EXPORT_SYMBOL_GPL(HYPERVISOR_vcpu_op);
EXPORT_SYMBOL_GPL(HYPERVISOR_platform_op_raw);
EXPORT_SYMBOL_GPL(HYPERVISOR_multicall);
EXPORT_SYMBOL_GPL(HYPERVISOR_vm_assist);
EXPORT_SYMBOL_GPL(HYPERVISOR_dm_op);
EXPORT_SYMBOL_GPL(privcmd_call);
