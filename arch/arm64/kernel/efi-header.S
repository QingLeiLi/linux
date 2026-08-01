/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * arm64 EFI PE/COFF 镜像头学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件被 head.S 直接 include，不生成独立运行模块。它用汇编数据指令把 Linux
 * arm64 Image 的开头同时塑造成 UEFI 可识别的 PE32+ 应用：首条机器码提供 DOS
 * “MZ”签名，Image header 中的偏移指向 __EFI_PE_HEADER 展开的 COFF/optional header、
 * section table 和可选 debug payload，入口最终落到 EFI stub。非 EFI 配置保留普通
 * NOP 和零 PE 偏移，使原生 arm64 boot loader 不会误判镜像能力。
 *
 * 这里没有运行期对象、锁或内存 ownership；所有标签、大小和 RVA 都由汇编器/链接器
 * 在构建期求值，固件装载器只读取最终字节。核心不变量是字段宽度、对齐、section
 * 覆盖范围和文件偏移必须彼此一致；错误通常表现为固件拒绝镜像或跳到错误入口，
 * 无运行期回滚机会。条件编译分别控制 EFI 能力、调试目录和 BTI/CFI 元数据。
 */
/*
 * Copyright (C) 2013 - 2017 Linaro, Ltd.
 * Copyright (C) 2013, 2014 Red Hat, Inc.
 */

#include <linux/pe.h>
#include <linux/sizes.h>

	.macro	efi_signature_nop
	/*
	 * efi_signature_nop 在 Image 第一条指令位置展开。无参数、无运行期返回；它只决定
	 * 构建出的 4 字节机器码。CONFIG_EFI 下必须既可作为 AArch64 指令执行，又让低两
	 * 字节呈现 PE/DOS “MZ”；关闭 EFI 时则必须是普通 NOP，向检查 opcode 的加载器明确
	 * 表示本镜像不支持 UEFI 启动。
	 */
#ifdef CONFIG_EFI
.L_head:
	/*
	 * This ccmp instruction has no meaningful effect except that
	 * its opcode forms the magic "MZ" signature required by UEFI.
	 */
	/*
	 * 这条 CCMP 对内核启动状态没有有意义的作用，选择它只因机器码恰好以 UEFI 所需
	 * “MZ”魔数开头。CPU 原生启动会把它当合法指令执行，固件则把同一字节当文件签名。
	 */
	ccmp	x18, #0, #0xd, pl
#else
	/*
	 * Bootloaders may inspect the opcode at the start of the kernel
	 * image to decide if the kernel is capable of booting via UEFI.
	 * So put an ordinary NOP here, not the "MZ.." pseudo-nop above.
	 */
	/*
	 * 部分 boot loader 直接检查镜像首 opcode 判断 UEFI 能力。未配置 EFI 时必须写
	 * 真正 NOP，不能留下伪装成“MZ”的 CCMP，否则加载器会进入并不存在的 EFI 路径。
	 */
	nop
#endif
	.endm

	.macro	__EFI_PE_HEADER
	/*
	 * __EFI_PE_HEADER 在 head.S 的 Image header 中展开完整 PE/COFF 元数据。宏无参数，
	 * 所有 .set/.long/.short/.quad 都在构建期发射固定宽度字段；标签差值形成相对偏移，
	 * 不依赖镜像最终物理装载地址。CONFIG_EFI=n 时只把 .Lpe_header_offset 设为 0。
	 */
#ifdef CONFIG_EFI
	.set	.Lpe_header_offset, . - .L_head
	/*
	 * COFF file header：签名声明 PE，Machine 固定 AArch64；section 数由后面的表反算；
	 * 时间戳、符号表和符号数为 0，表示不携带传统 COFF 符号信息；optional-header
	 * 大小由标签差求得；Characteristics 声明可执行且已剥离调试/行号信息。
	 */
	.long	IMAGE_NT_SIGNATURE
	.short	IMAGE_FILE_MACHINE_ARM64		// Machine
	.short	.Lsection_count				// NumberOfSections
	.long	0 					// TimeDateStamp
	.long	0					// PointerToSymbolTable
	.long	0					// NumberOfSymbols
	.short	.Lsection_table - .Loptional_header	// SizeOfOptionalHeader
	.short	IMAGE_FILE_DEBUG_STRIPPED | \
		IMAGE_FILE_EXECUTABLE_IMAGE | \
		IMAGE_FILE_LINE_NUMS_STRIPPED		// Characteristics

.Loptional_header:
	/*
	 * PE32+ optional header 的第一组字段描述代码/数据布局。LinkerVersion 是格式标识；
	 * SizeOfCode 覆盖 header 末端到 initdata 之前；初始化数据大小由链接脚本计算；
	 * BSS 不占镜像原始文件空间所以 SizeOfUninitializedData 为 0；入口 RVA 指向 EFI
	 * stub，BaseOfCode 指向真正 arm64 Image 内容开始处。
	 */
	.short	IMAGE_NT_OPTIONAL_HDR64_MAGIC		// PE32+ format
	.byte	0x02					// MajorLinkerVersion
	.byte	0x14					// MinorLinkerVersion
	.long	__initdata_begin - .Lefi_header_end	// SizeOfCode
	.long	__pecoff_data_size			// SizeOfInitializedData
	.long	0					// SizeOfUninitializedData
	.long	__efistub_efi_pe_entry - .L_head	// AddressOfEntryPoint
	.long	.Lefi_header_end - .L_head		// BaseOfCode

	/*
	 * 装载/版本字段：ImageBase=0 允许固件按实际地址装载；SectionAlignment 与
	 * FileAlignment 分别约束内存和文件布局；OS/subsystem 版本不提出额外要求；
	 * ImageVersion 暴露 EFI stub 版本；Win32VersionValue 按 PE 约定为 0。
	 */
	.quad	0					// ImageBase
	.long	SEGMENT_ALIGN				// SectionAlignment
	.long	PECOFF_FILE_ALIGNMENT			// FileAlignment
	.short	0					// MajorOperatingSystemVersion
	.short	0					// MinorOperatingSystemVersion
	.short	LINUX_EFISTUB_MAJOR_VERSION		// MajorImageVersion
	.short	LINUX_EFISTUB_MINOR_VERSION		// MinorImageVersion
	.short	0					// MajorSubsystemVersion
	.short	0					// MinorSubsystemVersion
	.long	0					// Win32VersionValue

	/* SizeOfImage 覆盖从 .L_head 到链接器 _end 的完整内存镜像，并受后续段对齐约束。 */
	.long	_end - .L_head				// SizeOfImage

	// Everything before the kernel image is considered part of the header
	// 真正内核 Image 之前的所有字节都计入 PE header，固件据此找到各 section 原始数据。
	/*
	 * 运行属性字段：不要求 PE 校验和；Subsystem 声明 EFI application；NX_COMPAT
	 * 告诉固件镜像支持不可执行数据；栈/堆 reserve/commit 均由内核自身而非 PE loader
	 * 申请；LoaderFlags 为 0；数据目录个数按从当前位置到 section table 的 8 字节项数计算。
	 */
	.long	.Lefi_header_end - .L_head		// SizeOfHeaders
	.long	0					// CheckSum
	.short	IMAGE_SUBSYSTEM_EFI_APPLICATION		// Subsystem
	.short	IMAGE_DLLCHARACTERISTICS_NX_COMPAT	// DllCharacteristics
	.quad	0					// SizeOfStackReserve
	.quad	0					// SizeOfStackCommit
	.quad	0					// SizeOfHeapReserve
	.quad	0					// SizeOfHeapCommit
	.long	0					// LoaderFlags
	.long	(.Lsection_table - .) / 8		// NumberOfRvaAndSizes

	/*
	 * 前六个 PE data directory 均为空：EFI stub 不依赖导出、导入、资源、异常、证书
	 * 或基址重定位目录。每个目录由 32 位 RVA 与 32 位大小组成，合写为一个 .quad 0。
	 */
	.quad	0					// ExportTable
	.quad	0					// ImportTable
	.quad	0					// ResourceTable
	.quad	0					// ExceptionTable
	.quad	0					// CertificationTable
	.quad	0					// BaseRelocationTable

#if defined(CONFIG_DEBUG_EFI) || defined(CONFIG_ARM64_BTI_KERNEL)
	/*
	 * 需要 EFI 调试信息或声明 BTI forward-edge CFI 兼容性时，发布 debug directory 的
	 * RVA 与总大小。表本身稍后放入被 section 覆盖的 init rodata，payload 可仅用文件
	 * 偏移引用，因此能保留在 header 邻近位置。
	 */
	.long	.Lefi_debug_table - .L_head		// DebugTable
	.long	.Lefi_debug_table_size

	/*
	 * The debug table is referenced via its Relative Virtual Address (RVA),
	 * which is only defined for those parts of the image that are covered
	 * by a section declaration. Since this header is not covered by any
	 * section, the debug table must be emitted elsewhere. So stick it in
	 * the .init.rodata section instead.
	 *
	 * Note that the payloads themselves are permitted to have zero RVAs,
	 * which means we can simply put those right after the section headers.
	 */
	/*
	 * debug table 通过 RVA 引用，而 RVA 只对 section 声明覆盖的镜像区域有定义；当前
	 * header 不属于任何 section，所以表不能原地发射，必须临时切到 .init.rodata。
	 * payload 本身允许 RVA=0，于是可以继续放在 section header 后，仅以 FileOffset
	 * 定位。`.previous` 会在表发射完恢复到原来的 header section。
	 */
	__INITRODATA

	.align	2
.Lefi_debug_table:
#ifdef CONFIG_DEBUG_EFI
	// EFI_IMAGE_DEBUG_DIRECTORY_ENTRY
	/*
	 * CodeView debug directory：特征、时间戳和版本留 0；Type 指定 CodeView；大小由
	 * .Lefi_debug_entry_size 回填；RVA=0 表示 payload 不需装入 section，FileOffset
	 * 指向后面的 NB10/VMLINUX_PATH 数据，供 EFI 调试器定位原始 vmlinux。
	 */
	.long	0					// Characteristics
	.long	0					// TimeDateStamp
	.short	0					// MajorVersion
	.short	0					// MinorVersion
	.long	IMAGE_DEBUG_TYPE_CODEVIEW		// Type
	.long	.Lefi_debug_entry_size			// SizeOfData
	.long	0					// RVA
	.long	.Lefi_debug_entry - .L_head		// FileOffset
#endif
#ifdef CONFIG_ARM64_BTI_KERNEL
	/*
	 * 第二个 debug directory 使用 EX_DLLCHARACTERISTICS 类型，payload 固定 4 字节，
	 * 通过文件偏移指向扩展 DLL 特征值，向固件声明镜像兼容 forward-edge CFI/BTI。
	 */
	.long	0					// Characteristics
	.long	0					// TimeDateStamp
	.short	0					// MajorVersion
	.short	0					// MinorVersion
	.long	IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS	// Type
	.long	4					// SizeOfData
	.long	0					// RVA
	.long	.Lefi_dll_characteristics_ex - .L_head	// FileOffset
#endif
	.set	.Lefi_debug_table_size, . - .Lefi_debug_table
	/* 标签差在汇编期得到一个或两个目录项的总字节数，供 optional header 回填。 */
	.previous
#endif

	// Section table
	/*
	 * section table 把同一 Linux Image 描述为两个 PE section。每项严格 40 字节，包含
	 * 8 字节名称、虚拟/原始大小与位置、未使用的重定位/行号字段和访问属性；后面的
	 * .Lsection_count 正是用总字节数除以 40，避免手工维护节数量。
	 */
.Lsection_table:
	.ascii	".text\0\0\0"
	/*
	 * .text 从 EFI header 末端延伸到 __initdata_begin：VirtualSize 与 RawDataSize
	 * 相同，RVA/FileOffset 也同为 header 大小，因为镜像采用平坦文件布局。它没有
	 * COFF relocation/line-number 记录，属性为代码、可读、可执行而不可写。
	 */
	.long	__initdata_begin - .Lefi_header_end	// VirtualSize
	.long	.Lefi_header_end - .L_head		// VirtualAddress
	.long	__initdata_begin - .Lefi_header_end	// SizeOfRawData
	.long	.Lefi_header_end - .L_head		// PointerToRawData

	.long	0					// PointerToRelocations
	.long	0					// PointerToLineNumbers
	.short	0					// NumberOfRelocations
	.short	0					// NumberOfLineNumbers
	.long	IMAGE_SCN_CNT_CODE | \
		IMAGE_SCN_MEM_READ | \
		IMAGE_SCN_MEM_EXECUTE			// Characteristics

	.ascii	".data\0\0\0"
	/*
	 * .data 从 __initdata_begin 起覆盖 PE 数据区。VirtualSize 可包含装载后占用空间，
	 * RawSize 是文件中实际字节；RVA 与 FileOffset 在平坦布局中同值。该 section
	 * 声明 initialized data、可读写、不可执行，落实 W^X 的装载期边界。
	 */
	.long	__pecoff_data_size			// VirtualSize
	.long	__initdata_begin - .L_head		// VirtualAddress
	.long	__pecoff_data_rawsize			// SizeOfRawData
	.long	__initdata_begin - .L_head		// PointerToRawData

	.long	0					// PointerToRelocations
	.long	0					// PointerToLineNumbers
	.short	0					// NumberOfRelocations
	.short	0					// NumberOfLineNumbers
	.long	IMAGE_SCN_CNT_INITIALIZED_DATA | \
		IMAGE_SCN_MEM_READ | \
		IMAGE_SCN_MEM_WRITE			// Characteristics

	.set	.Lsection_count, (. - .Lsection_table) / 40
	/* 两个 40 字节 section header 发射完成后反算 NumberOfSections。 */

#ifdef CONFIG_DEBUG_EFI
.Lefi_debug_entry:
	// EFI_IMAGE_DEBUG_CODEVIEW_NB10_ENTRY
	/*
	 * CodeView NB10 payload：签名后三个历史字段置零，再附以 NUL 结尾 VMLINUX_PATH。
	 * 固件不执行这些字节，调试工具用路径寻找带完整符号的 vmlinux；大小通过末端标签
	 * 计算，保持路径配置变化时 directory 自动一致。
	 */
	.ascii	"NB10"					// Signature
	.long	0					// Unknown
	.long	0					// Unknown2
	.long	0					// Unknown3

	.asciz	VMLINUX_PATH

	.set	.Lefi_debug_entry_size, . - .Lefi_debug_entry
#endif
#ifdef CONFIG_ARM64_BTI_KERNEL
.Lefi_dll_characteristics_ex:
	/* 4 字节扩展特征 payload 声明镜像满足 forward-edge CFI，供支持者启用 BTI 策略。 */
	.long	IMAGE_DLLCHARACTERISTICS_EX_FORWARD_CFI_COMPAT
#endif

	/*
	 * 把 header/payload 总长度补齐到 SEGMENT_ALIGN，确保后续真实 Image 和 PE section
	 * 的 RVA/文件布局满足装载器对齐要求；填充字节不属于运行期业务状态。
	 */
	.balign	SEGMENT_ALIGN
.Lefi_header_end:
#else
	/* 非 EFI 构建没有 PE header，head.S 中对应偏移字段明确写 0。 */
	.set	.Lpe_header_offset, 0x0
#endif
	.endm
