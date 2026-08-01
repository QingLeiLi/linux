// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 位置无关早期内核映射学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * head.S 先借恒等映射开启 MMU，此时仍只能依赖 idmap 和静态预留内存；随后以
 * 位置无关符号 __pi_early_map_kernel() 进入本文件。这里解析少量 DT 启动信息和 CPU 能力，
 * 决定 VA 位数、KASLR、KPTI/非全局映射及 LPA2 策略，构造 init_pg_dir，必要时
 * 完成重定位或动态 SCS patch，最后把根页表复制到 swapper_pg_dir 并切换 TTBR1。
 * 返回 head.S 后，正式内核虚拟地址、栈和 C 运行环境才可继续建立。
 *
 * 核心对象均为链接期静态区域：init_idmap_pg_dir 保住 MMU 开关过渡代码，
 * init_pg_dir 是可丢弃的构造/临时页表，swapper_pg_dir 是长期内核根页表；FDT 指针
 * 只是启动期借用。此时单个 boot CPU 串行执行，没有普通页表锁或动态分配失败，
 * 但每次把描述符交给硬件前都必须用 DSB/ISB/TLBI 建立可见性与切换顺序。
 * 方案以静态容量和严格前置条件换取“在分配器、异常和调度器均不可用时仍能启动”；
 * 任何容量/格式错误通常没有回滚条件，只会导致极早期启动失败。
 */
// Copyright 2023 Google LLC
// Author: Ard Biesheuvel <ardb@google.com>

#include <linux/init.h>
#include <linux/libfdt.h>
#include <linux/linkage.h>
#include <linux/types.h>
#include <linux/sizes.h>
#include <linux/string.h>

#include <asm/memory.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>

#include "pi.h"

extern const u8 __eh_frame_start[], __eh_frame_end[];
/* 链接器给出的异常展开元数据半开区间；动态 SCS patch 只借用并原地扫描这段镜像。 */

extern void idmap_cpu_replace_ttbr1(phys_addr_t pgdir);
/* 在恒等映射代码中安全替换 TTBR1；参数是静态根页表物理地址，不转移所有权。 */

/*
 * map_segment() - 把内核镜像的一个链接区段映射到目标虚拟偏移。
 *
 * map_kernel() 直接调用，unmap_segment() 也以零权限模板复用它。
 * @pg_dir 是根页表的借用地址；@pgd 是下一空闲页表物理游标，可为 NULL（表示本轮
 * 不允许再创建子表）；@va_offset 把链接/物理地址换算到目标 VA；@start/@end 是
 * 链接器给出的半开区间；@prot、@may_use_cont、@root_level 决定权限、连续项提示和
 * 根级别。函数不能睡眠；返回无直接值，也没有错误码，所有状态变化都发生在静态
 * 页表中，随后由调用者负责屏障和硬件发布。
 */
static void __init map_segment(pgd_t *pg_dir, phys_addr_t *pgd, u64 va_offset,
			       void *start, void *end, pgprot_t prot,
			       bool may_use_cont, int root_level)
{
	/*
	 * 链接符号在此位置无关阶段按当前别名取值：加 va_offset 得目标 VA，再去掉
	 * PAGE_OFFSET 高半区基值；物理起点仍是 start。map_range() 负责逐层填表。
	 */
	map_range(pgd, ((u64)start + va_offset) & ~PAGE_OFFSET,
		  ((u64)end + va_offset) & ~PAGE_OFFSET, (u64)start,
		  prot, root_level, (pte_t *)pg_dir, may_use_cont, 0);
}

/*
 * unmap_segment() - 用零权限模板清除一个已存在区段的叶子描述符。
 *
 * map_kernel() 的两遍文本路径调用。@pg_dir 是借用根页表；@va_offset 把链接地址
 * 换算到当前目标 VA；@start/@end 是待撤销的半开区间；@root_level 是根翻译级别。
 * 内部固定传 NULL 分配游标和 prot=0，要求中间页表已经存在；函数只打断叶子映射，
 * 不回收静态页表页。返回无直接值和错误码；调用者负责紧随其后的屏障和 TLB 失效，
 * 以满足 break-before-make 后半段重新映射的前置条件。
 */
static void __init unmap_segment(pgd_t *pg_dir, u64 va_offset, void *start,
				 void *end, int root_level)
{
	map_segment(pg_dir, NULL, va_offset, start, end, __pgprot(0),
		    false, root_level);
}

/*
 * map_kernel() - 构造并提交启动阶段的正式高地址内核映射。
 *
 * early_map_kernel() 在确定随机化和页表层级后调用。@kaslr_offset 是镜像相对默认
 * KIMAGE_VADDR 的虚拟随机偏移；@va_offset 是目标 VA 与当前镜像物理/链接地址之差；
 * @root_level 是当前 CPU/配置实际使用的根翻译级别。函数串行运行于 boot CPU，
 * MMU 已由 idmap 保住执行流，不能睡眠且没有锁或错误返回。
 *
 * 普通配置一次按最终权限建表；需要重定位或动态 SCS 时，先把文本临时映射为可写，
 * 完成代码修改后打断旧映射、失效 TLB，再按 ROX/BTI 权限重建。最终复制根表到
 * swapper_pg_dir 并切换 TTBR1；静态子页表仍被两张根表引用，不发生页所有权转移。
 * 函数返回时正式根表已对硬件生效，内部没有可恢复的失败出口。
 */
static void __init map_kernel(u64 kaslr_offset, u64 va_offset, int root_level)
{
	/*
	 * 变量地图：enable_scs 表示是否需动态改写返回保护指令；twopass 表示文本要经历
	 * “可写构造 -> patch/relocate -> 最终只读可执行”两遍；pgdp 是根页后一页开始的
	 * 静态页表池游标；text_prot/data_prot 是最终权限，prot 是首遍文本权限。
	 */
	bool enable_scs = IS_ENABLED(CONFIG_UNWIND_PATCH_PAC_INTO_SCS);
	bool twopass = IS_ENABLED(CONFIG_RELOCATABLE);
	phys_addr_t pgdp = (phys_addr_t)init_pg_dir + PAGE_SIZE;
	pgprot_t text_prot = PAGE_KERNEL_ROX;
	pgprot_t data_prot = PAGE_KERNEL;
	pgprot_t prot;

	/*
	 * External debuggers may need to write directly to the text mapping to
	 * install SW breakpoints. Allow this (only) when explicitly requested
	 * with rodata=off.
	 */
	/*
	 * 外部调试器安装软件断点时需要直接改文本。仅显式 rodata=off 才把文本放宽为
	 * 可写可执行；默认坚持只读可执行，避免把调试便利无条件变成攻击面。
	 */
	if (arm64_test_sw_feature_override(ARM64_SW_FEATURE_OVERRIDE_RODATA_OFF))
		text_prot = PAGE_KERNEL_EXEC;

	/*
	 * We only enable the shadow call stack dynamically if we are running
	 * on a system that does not implement PAC or BTI. PAC and SCS provide
	 * roughly the same level of protection, and BTI relies on the PACIASP
	 * instructions serving as landing pads, preventing us from patching
	 * those instructions into something else.
	 */
	/*
	 * 只有 CPU 没有可用 PAC/BTI 时才动态启用 SCS。PAC 与 SCS 提供近似的返回地址
	 * 保护；BTI 又把 PACIASP 当合法 landing pad，若把该指令 patch 成其他操作会
	 * 破坏间接分支目标约束。因此相应硬件能力可用时关闭软件改写路径。
	 */
	if (IS_ENABLED(CONFIG_ARM64_PTR_AUTH_KERNEL) && cpu_has_pac())
		enable_scs = false;

	if (IS_ENABLED(CONFIG_ARM64_BTI_KERNEL) && cpu_has_bti()) {
		enable_scs = false;

		/*
		 * If we have a CPU that supports BTI and a kernel built for
		 * BTI then mark the kernel executable text as guarded pages
		 * now so we don't have to rewrite the page tables later.
		 */
		/*
		 * 内核和 CPU 都支持 BTI 时立即给可执行文本设置 guarded-page 位。这样首次
		 * 发布页表就是最终形式，后续不必为 BTI 单独重写活动描述符和刷新 TLB。
		 */
		text_prot = __pgprot_modify(text_prot, PTE_GP, PTE_GP);
	}

	/* Map all code read-write on the first pass if needed */
	/* 若重定位或 SCS patch 会改指令，首遍必须映射为 RW/NX；否则直接采用最终 ROX。 */
	twopass |= enable_scs;
	prot = twopass ? data_prot : text_prot;

	/*
	 * [_stext, _text) isn't executed after boot and contains some
	 * non-executable, unpredictable data, so map it non-executable.
	 */
	/*
	 * [_stext, _text) 在启动后不执行且夹有不可预测的非指令数据，因此按 RW/NX
	 * 映射。其余区段按文本、init 文本和数据职责拆开，避免给数据执行权限；仅可
	 * 安全形成完整连续组的长期区段允许 contiguous hint。
	 */
	map_segment(init_pg_dir, &pgdp, va_offset, _text, _stext, data_prot,
		    false, root_level);
	map_segment(init_pg_dir, &pgdp, va_offset, _stext, _etext, prot,
		    !twopass, root_level);
	map_segment(init_pg_dir, &pgdp, va_offset, __start_rodata,
		    __inittext_begin, data_prot, false, root_level);
	map_segment(init_pg_dir, &pgdp, va_offset, __inittext_begin,
		    __inittext_end, prot, false, root_level);
	map_segment(init_pg_dir, &pgdp, va_offset, __initdata_begin,
		    __initdata_end, data_prot, false, root_level);
	map_segment(init_pg_dir, &pgdp, va_offset, _data, _end, data_prot,
		    true, root_level);
	/* 页表写先对 inner-shareable 观察者可见，再把新根装入 TTBR1。 */
	dsb(ishst);

	idmap_cpu_replace_ttbr1((phys_addr_t)init_pg_dir);
	/* 从此 CPU 通过 init_pg_dir 的高地址映射执行；静态构造页仍由启动内存持有。 */

	if (twopass) {
		/*
		 * 第二遍前先完成所有会改写镜像的工作。relocate_kernel() 按 KASLR 偏移
		 * 修正重定位项；SCS patch 先 dry-run 校验可信的内核展开元数据，再实际改指令。
		 * 此极早期调用没有错误上报通道，因而忽略返回码依赖“构建产物格式必然有效”
		 * 的启动不变量；随后失效全 inner-shareable I-cache 并发布启用状态。
		 */
		if (IS_ENABLED(CONFIG_RELOCATABLE))
			relocate_kernel(kaslr_offset);

		if (enable_scs) {
			scs_patch(__eh_frame_start + va_offset,
				  __eh_frame_end - __eh_frame_start, false);
			asm("ic ialluis");

			dynamic_scs_is_enabled = true;
		}

		/*
		 * Unmap the text region before remapping it, to avoid
		 * potential TLB conflicts when creating the contiguous
		 * descriptors.
		 */
		/*
		 * 在创建 contiguous 描述符前先清掉旧文本映射，避免同一 VA 的旧普通项与
		 * 新连续项在 TLB 中冲突。DSB 发布“break”，TLBI 清旧翻译，ISB 令后续取指
		 * 只观察到失效后的上下文，这是 break-before-make 的关键边界。
		 */
		unmap_segment(init_pg_dir, va_offset, _stext, _etext,
			      root_level);
		dsb(ishst);
		isb();
		__tlbi(vmalle1);
		isb();

		/*
		 * Remap these segments with different permissions
		 * No new page table allocations should be needed
		 */
		/*
		 * 以最终权限重新映射普通文本与 init 文本；所有中间表已由首遍创建，所以
		 * 分配游标传 NULL，若这里仍需新表就表示首遍布局不变量已被破坏。
		 */
		map_segment(init_pg_dir, NULL, va_offset, _stext, _etext,
			    text_prot, true, root_level);
		map_segment(init_pg_dir, NULL, va_offset, __inittext_begin,
			    __inittext_end, text_prot, false, root_level);
	}

	/* Copy the root page table to its final location */
	/*
	 * 只复制根页；根项仍指向已经构造好的静态下级表。DSB 保证副本先可见，随后
	 * TTBR1 切到长期 swapper_pg_dir。init_pg_dir 自此不再是活动的正式根表。
	 */
	memcpy((void *)swapper_pg_dir + va_offset, init_pg_dir, PAGE_SIZE);
	dsb(ishst);
	idmap_cpu_replace_ttbr1((phys_addr_t)swapper_pg_dir);
}

/*
 * set_ttbr0_for_lpa2() - 在恒等映射中原子化完成 LPA2 的 TTBR0/TCR 格式切换。
 *
 * remap_idmap_for_lpa2() 传入一张已按 LPA2 descriptor 语义构造的根页表物理地址。
 * 函数位于 .idmap.text 且禁止内联，保证临时关闭 MMU 后 PC 仍可按物理地址继续；
 * @ttbr 是借用的静态根页表，不发生所有权变化。函数不能睡眠、无锁、无失败返回。
 *
 * 内联汇编先保存原 SCTLR/TCR 能力，关闭 MMU，安装新 TTBR0 和带 DS/正确 IPS 的
 * TCR，清全部 EL1 stage-1 TLB，再恢复原 SCTLR。返回时 MMU 状态与入口一致，但
 * TTBR0 已指向新根且 LPA2 descriptor 解释生效；屏障顺序不可交换。
 */
static void noinline __section(".idmap.text") set_ttbr0_for_lpa2(phys_addr_t ttbr)
{
	/*
	 * sctlr 保存入口 MMU 状态；tcr 预置 DS 位；mmfr0/parange 给出硬件实际物理地址
	 * 宽度，必须同步写入 IPS，避免 TCR 声明的输出地址范围与 CPU 能力不一致。
	 */
	u64 sctlr = read_sysreg(sctlr_el1);
	u64 tcr = read_sysreg(tcr_el1) | TCR_EL1_DS;
	u64 mmfr0 = read_sysreg(id_aa64mmfr0_el1);
	u64 parange = cpuid_feature_extract_unsigned_field(mmfr0,
							   ID_AA64MMFR0_EL1_PARANGE_SHIFT);

	tcr &= ~TCR_EL1_IPS_MASK;
	tcr |= parange << TCR_EL1_IPS_SHIFT;

	/*
	 * 一个 asm 块固定硬件状态转换顺序：关 MMU并同步 -> 写 TTBR0/TCR并同步 ->
	 * 失效全部 EL1 翻译并等待 -> 恢复 SCTLR并同步。期间不能跳到非 idmap 地址，也
	 * 不能让编译器在序列中插入普通访存；四个输入寄存器只提供计算好的系统寄存器值。
	 */
	asm("	msr	sctlr_el1, %0		;"
	    "	isb				;"
	    "   msr     ttbr0_el1, %1		;"
	    "   msr     tcr_el1, %2		;"
	    "	isb				;"
	    "	tlbi    vmalle1			;"
	    "	dsb     nsh			;"
	    "	isb				;"
	    "	msr     sctlr_el1, %3		;"
	    "	isb				;"
	    ::	"r"(sctlr & ~SCTLR_ELx_M), "r"(ttbr), "r"(tcr), "r"(sctlr));
}

/*
 * remap_idmap_for_lpa2() - 通过临时恒等映射安全重建活动初始 idmap 的描述符格式。
 *
 * early_map_kernel() 仅在内核配置 LPA2、CPU 支持且使用扩展 VA 时调用。此时原 idmap
 * 正被 TTBR0 使用，不能原地改 bits[9:8]；函数先在 init_pg_dir 构造临时兼容表并
 * 切过去，再重建 init_idmap_pg_dir，最后切回并擦除临时表。所有表都是静态借用
 * 内存，无锁、不可睡眠、无可报告失败；返回保证活动 idmap 已采用 LPA2 语义。
 * 入参：无；返回：无直接返回值，硬件可观察副作用是 TTBR0/TCR/TLB 状态发生切换。
 */
static void __init remap_idmap_for_lpa2(void)
{
	/* clear the bits that change meaning once LPA2 is turned on */
	/* 开启 LPA2 后 PTE bits[9:8] 改作物理地址位，因此清除原来的 shared 属性编码。 */
	ptval_t mask = PTE_SHARED;

	/*
	 * We have to clear bits [9:8] in all block or page descriptors in the
	 * initial ID map, as otherwise they will be (mis)interpreted as
	 * physical address bits once we flick the LPA2 switch (TCR.DS). Since
	 * we cannot manipulate live descriptors in that way without creating
	 * potential TLB conflicts, let's create another temporary ID map in a
	 * LPA2 compatible fashion, and update the initial ID map while running
	 * from that.
	 */
	/*
	 * 不能一边通过旧 idmap 执行一边原地改变活动描述符，否则 TLB 可能同时保存两种
	 * 解释。先在 init_pg_dir 建 LPA2 兼容临时 idmap，DSB 发布后切 TTBR0，取得一个
	 * 与待更新 init_idmap_pg_dir 相互独立的执行环境。
	 */
	create_init_idmap(init_pg_dir, mask);
	dsb(ishst);
	set_ttbr0_for_lpa2((phys_addr_t)init_pg_dir);

	/*
	 * Recreate the initial ID map with the same granularity as before.
	 * Don't bother with the FDT, we no longer need it after this.
	 */
	/*
	 * 清空并以原粒度重建正式初始 idmap。此阶段早期参数已解析完，不必再把 FDT
	 * 纳入恒等映射，从而缩小所需表空间和启动期可访问面。
	 */
	memset(init_idmap_pg_dir, 0,
	       (char *)init_idmap_pg_end - (char *)init_idmap_pg_dir);

	create_init_idmap(init_idmap_pg_dir, mask);
	dsb(ishst);

	/* switch back to the updated initial ID map */
	/* 新正式表已可见，切回它；set_ttbr0_for_lpa2() 内部完成 TLB 与同步协议。 */
	set_ttbr0_for_lpa2((phys_addr_t)init_idmap_pg_dir);

	/* wipe the temporary ID map from memory */
	/* 临时表不再被硬件引用，擦除静态区域，防止后续误把残留描述符当有效映射。 */
	memset(init_pg_dir, 0, (char *)init_pg_end - (char *)init_pg_dir);
}

/*
 * map_fdt() - 临时把固件传入的设备树物理地址加入启动恒等映射。
 *
 * @fdt 是 boot loader/固件交付的 FDT 物理起点，函数只借用这段固件内存，不验证
 * blob 内容，也不取得释放责任。静态 @ptes 是本函数专用的 __initdata 页表池，
 * 生命周期仅覆盖启动；调用时处于 idmap，不能睡眠且无并发写者。
 *
 * 返回数值相同的可解引用指针，因为恒等映射令 VA==PA。最多映射 MAX_FDT_SIZE，
 * 若 FDT 位于内核镜像之前则在 _text 截断，避免用 FDT 权限覆盖内核页。函数没有
 * errno；FDT 格式有效性由后续 libfdt 调用判断。
 */
static void *__init map_fdt(phys_addr_t fdt)
{
	/* ptes 归启动代码静态所有；efdt 是允许映射的理论末端，ptep 是池领取游标。 */
	static u8 ptes[INIT_IDMAP_FDT_SIZE] __initdata __aligned(PAGE_SIZE);
	phys_addr_t efdt = fdt + MAX_FDT_SIZE;
	phys_addr_t ptep = (phys_addr_t)ptes; /* We're idmapped when called */
	/* 恒等映射下 ptes 的 C 指针数值也是页表池物理地址。 */

	/*
	 * Map up to MAX_FDT_SIZE bytes, but avoid overlap with
	 * the kernel image.
	 */
	/*
	 * 建立 FDT 的 RW/NX 恒等映射，但当它从内核镜像低地址一侧逼近 _text 时截断；
	 * init_idmap_pg_dir 是既有活动根，map_range() 只补充相关子树。DSB 确保随后
	 * libfdt 的读取能由硬件页表遍历观察到新描述符。
	 */
	map_range(&ptep, fdt, (u64)_text > fdt ? min((u64)_text, efdt) : efdt,
		  fdt, PAGE_KERNEL, IDMAP_ROOT_LEVEL,
		  (pte_t *)init_idmap_pg_dir, false, 0);
	dsb(ishst);

	return (void *)fdt;
}

/*
 * PI version of the Cavium Eratum 27456 detection, which makes it
 * impossible to use non-global mappings.
 */
/*
 * ng_mappings_allowed() - 判断当前 CPU 是否允许 KPTI/KASLR 使用 non-global 映射。
 *
 * 这是 Cavium erratum 27456 的位置无关早期检测版，由 early_map_kernel() 在获得
 * 有效 KASLR seed 且策略需要 KPTI 时调用。函数只读取当前 CPU MIDR 和只读型号表，
 * 不持锁、不能睡眠；返回 false 表示命中受影响 ThunderX 修订版，必须放弃 NG 映射，
 * true 表示本表未发现限制。它只做当前 CPU 启动决策，不拥有或发布任何对象。
 */
static bool __init ng_mappings_allowed(void)
{
	/* 表项描述 model 与允许的 revision/variant 范围；空 model 是线性扫描哨兵。 */
	static const struct midr_range cavium_erratum_27456_cpus[] __initconst = {
		/* Cavium ThunderX, T88 pass 1.x - 2.1 */
		/* ThunderX T88 从 1.x 到 2.1 均受影响，需禁止 non-global 映射。 */
		MIDR_RANGE(MIDR_THUNDERX, 0, 0, 1, 1),
		/* Cavium ThunderX, T81 pass 1.0 */
		/* ThunderX T81 仅列出的 1.0 修订版受影响。 */
		MIDR_REV(MIDR_THUNDERX_81XX, 0, 0),
		{},
	};

	/* r 是对静态表的借用游标；命中任一范围立即拒绝，否则扫描到哨兵后允许。 */
	for (const struct midr_range *r = cavium_erratum_27456_cpus; r->model; r++) {
		if (midr_is_cpu_model_range(read_cpuid_id(), r->model,
					    r->rv_min, r->rv_max))
			return false;
	}

	return true;
}

/*
 * early_map_kernel() - 汇总启动能力与固件信息，建立最终 arm64 内核高地址映射。
 *
 * head.S 的 __primary_switch 在 idmap 和早期栈上调用位置无关别名。@boot_status 是
 * init_kernel_el() 记录的启动 EL/状态位，纯输入；@fdt 是固件交付的设备树物理地址，
 * 仅在本函数早期临时映射和读取，不转移所有权。入口只有 boot CPU 活跃，普通 BSS
 * 尚未清零、分配器和调度器不可用，因此函数不能睡眠、无锁且不得依赖动态内存。
 *
 * 阶段依次为：映射 FDT；清 BSS/临时页表；解析 CPU override；按 LVA/LPA2 能力
 * 收缩 VA/调整根级别；组合满足块对齐的 KASLR 偏移；必要时重建 LPA2 idmap；调用
 * map_kernel() 提交 swapper_pg_dir。返回无直接值；副作用包括 TCR、早期特性状态、
 * arm64_use_ng_mappings、镜像重定位和 TTBR1。内部没有可返回失败，致命条件由极早期
 * 启动机制停机；成功后 head.S 可切到正式虚拟地址继续。
 */
asmlinkage void __init early_map_kernel(u64 boot_status, phys_addr_t fdt)
{
	/*
	 * 变量地图：chosen_str/chosen 定位 FDT /chosen 节点；pa_base 是镜像物理基准；
	 * kaslr_offset 先保留物理放置的低对齐位，再混入 seed 高位；root_level/va_bits
	 * 表示当前硬件可实现的翻译形态；fdt_mapped 是借用的临时恒等映射指针。
	 */
	static char const chosen_str[] __initconst = "/chosen";
	u64 va_base, pa_base = (u64)&_text;
	u64 kaslr_offset = pa_base % MIN_KIMG_ALIGN;
	int root_level = 4 - CONFIG_PGTABLE_LEVELS;
	int va_bits = VA_BITS;
	int chosen;
	void *fdt_mapped = map_fdt(fdt);

	/* Clear BSS and the initial page tables */
	/*
	 * FDT 页表池位于 __initdata，不在此清零范围内；先映射 FDT 才能安全清从 BSS
	 * 到 init_pg_end 的区域。该 memset 同时建立 C 语言要求的零初始化 BSS，并把
	 * 即将重建的 init_pg_dir 清空，不能晚于任何依赖全局零值的 helper。
	 */
	memset(__bss_start, 0, (char *)init_pg_end - (char *)__bss_start);

	/* Parse the command line for CPU feature overrides */
	/*
	 * 查找 /chosen 后解析 early command line 与启动状态中的 CPU feature override；
	 * chosen 可为 libfdt 的负错误值，helper 按“节点缺失”语义处理，不持有新引用。
	 */
	chosen = fdt_path_offset(fdt_mapped, chosen_str);
	init_feature_override(boot_status, fdt_mapped, chosen);

	if (IS_ENABLED(CONFIG_ARM64_64K_PAGES) && !cpu_has_lva()) {
		/* 64K 页内核若无 LVA，只能退回架构最小 VA_BITS，保持页表格式可被 CPU 接受。 */
		va_bits = VA_BITS_MIN;
	} else if (IS_ENABLED(CONFIG_ARM64_LPA2) && !cpu_has_lpa2()) {
		/* 配置允许 LPA2 但 CPU 不支持时缩小 VA，并多用一级根表完成相同映射职责。 */
		va_bits = VA_BITS_MIN;
		root_level++;
	}

	/* 扩展 VA 模式通过减小 T1SZ 发布到 TCR_EL1；最小模式沿用 head.S 的基线值。 */
	if (va_bits > VA_BITS_MIN)
		sysreg_clear_set(tcr_el1, TCR_EL1_T1SZ_MASK, TCR_T1SZ(va_bits));

	/*
	 * The virtual KASLR displacement modulo 2MiB is decided by the
	 * physical placement of the image, as otherwise, we might not be able
	 * to create the early kernel mapping using 2 MiB block descriptors. So
	 * take the low bits of the KASLR offset from the physical address, and
	 * fill in the high bits from the seed.
	 */
	/*
	 * 早期映射希望使用 2 MiB block，虚拟偏移低位必须与镜像物理放置保持同余；否则
	 * VA/PA 无法同时块对齐。故低位取自 pa_base，高位才取随机 seed，在随机化强度
	 * 与无需额外细粒度页表的启动约束之间折中。
	 */
	if (IS_ENABLED(CONFIG_RANDOMIZE_BASE)) {
		/* seed 为 0 表示没有可用随机熵；此时只保留物理对齐决定的低位。 */
		u64 kaslr_seed = kaslr_early_init(fdt_mapped, chosen);

		/* KPTI 需要 NG 映射；受 Cavium 缺陷影响时保守关闭该选择，避免错误翻译。 */
		if (kaslr_seed && kaslr_requires_kpti())
			arm64_use_ng_mappings = ng_mappings_allowed();

		kaslr_offset |= kaslr_seed & ~(MIN_KIMG_ALIGN - 1);
	}

	if (IS_ENABLED(CONFIG_ARM64_LPA2) && va_bits > VA_BITS_MIN)
		/* 开启 DS 前先把正在使用的 idmap 重建为 LPA2 descriptor 语义。 */
		remap_idmap_for_lpa2();

	/* 最终 VA 基址由固定 KIMAGE_VADDR 加随机偏移组成；差值用于各镜像区段换算。 */
	va_base = KIMAGE_VADDR + kaslr_offset;
	map_kernel(kaslr_offset, va_base - pa_base, root_level);
}
