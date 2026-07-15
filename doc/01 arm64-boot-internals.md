# ARM64 内核启动：CPU 与内存状态演化

> **视角**：站在 CPU 和内存旁边，逐步观察每一步"寄存器写了什么、内存多了什么、能力边界怎么变"。  
> **模板**：每个步骤固定输出三块 —— ① CPU 寄存器变化 ② 内存数据结构变化 ③ 能力边界（前/后对比）。  
> **约定**：`?` 表示未定义/随机值，`-` 表示无变化，`✗` 表示不可用，`✓` 表示可用。

---

## 模板说明

每个步骤的标准输出格式：

```
### N.N 步骤名

#### CPU 寄存器变化
| 寄存器 | 变化前 | 变化后 | 意义 |

#### 内存变化
| 数据结构 | 位置 | 写入内容 | 用途 |

#### 能力边界
之前：不能做 X（原因）
之后：可以做 X（因为 Y 就绪）
```

---

## 系统资源速查

进入内核时存在的硬件资源，全部启动期间都会被操作：

```
CPU 层
├── 通用寄存器 x0–x30, SP, PC, XZR
├── 系统寄存器（EL1）
│     SCTLR_EL1  总控（MMU/Cache/对齐）
│     MAIR_EL1   内存类型字典（8槽）
│     TCR_EL1    地址翻译控制（位宽/粒度/Cache策略）
│     TTBR0_EL1  用户空间页表根
│     TTBR1_EL1  内核空间页表根
│     VBAR_EL1   异常向量表地址
│     SP_EL0     内核态借用：current 指针
│     TPIDR_EL1  per-CPU 偏移量
├── 系统寄存器（EL2，若从 EL2 启动）
│     HCR_EL2    Hypervisor 配置
│     VBAR_EL2   EL2 异常向量
│     SCTLR_EL2  EL2 系统控制
└── PSTATE        N/Z/C/V 标志 + DAIF 中断屏蔽 + EL + SP选择

内存层
├── 内核镜像（链接脚本划定，物理连续）
│     .head.text / .idmap.text / .text / .rodata / .data / .bss
│     init_pg_dir          早期内核页表区（静态预留）
│     init_idmap_pg_dir    恒等映射页表区（静态预留）
│     init_task            PID 0 的 task_struct（静态编译进来）
│     init_stack           PID 0 的内核栈（静态编译进来）
│     early_init_stack     head.S 专用早期栈
├── DTB（bootloader 放置，x0 指向）
└── 其余 RAM（内核镜像之外，启动过程中逐步被管理）
```

---

## 阶段 A：`primary_entry` — 物理世界（MMU 关闭）

**进入条件**
```
PC  = 内核物理加载地址（_start）
x0  = FDT blob 物理地址
x1/x2/x3 = 0（保留）
MMU = off
D-Cache = off
SP  = ?（未定义）
EL  = ?（EL1 或 EL2，由 firmware 决定）
所有其他寄存器 = ?
```

---

### A.1 `record_mmu_state`

目的：在任何操作前，先弄清楚 firmware 把 CPU 交出来时是什么状态。

#### CPU 寄存器变化

| 寄存器 | 变化前 | 变化后 | 意义 |
|--------|--------|--------|------|
| `x19` | `?` | `0` 或 `SCTLR_ELx_M`(=1) | **全程 MMU 状态标志**。0=MMU关或Cache关，1=两者都开 |
| `SCTLR_EL1/EL2` | firmware 设置值 | 字节序不对时会修正 EE 位并强制清 M 位 | 若 firmware 留下错误字节序，立即修正 |

读取但不写（探测用）：`CurrentEL`、`SCTLR_EL1` 或 `SCTLR_EL2`

#### 内存变化

无。纯寄存器操作。

#### 能力边界

```
之前：不知道 MMU 是开还是关 → 无法安全决定后续 Cache 操作路径
之后：x19 确定，后续所有 Cache 操作以 x19 为分叉条件
```

---

### A.2 `preserve_boot_args`

目的：把 bootloader 传入的寄存器值固化到内存，x0 随后会被各种调用覆盖。

#### CPU 寄存器变化

| 寄存器 | 变化前 | 变化后 | 意义 |
|--------|--------|--------|------|
| `x21` | `?` | FDT 物理地址 | **callee-saved，全程保存 FDT 地址**，直到写入 `__fdt_pointer` |
| `x0` | FDT 物理地址 | `boot_args` 数组地址 | 被覆盖，FDT 地址已转移到 x21 |

#### 内存变化

| 数据结构 | 位置 | 写入内容 | 用途 |
|----------|------|----------|------|
| `boot_args[4]` | `.bss` 段，物理地址 | `[FDT地址, 0, 0, 0]` | 永久记录 bootloader 的入参，`/proc/device-tree` 等路径会用到 |

写入后立刻执行：
- x19=0（MMU off）：`dmb sy` + `dcache_inval_poc(boot_args, boot_args+32)`  
  → 将 boot_args 对应的 Cache 行强制失效，防止 CPU 推测性预取缓存旧数据

#### 能力边界

```
之前：x0 保存 FDT 地址，一旦调用任何函数 x0 就会被覆盖
之后：FDT 地址安全存在 x21 和 boot_args[0] 两处，x0 可自由使用
```

---

### A.3 建立早期栈

目的：SP 有效之前，无法使用局部变量，无法嵌套函数调用。

#### CPU 寄存器变化

| 寄存器 | 变化前 | 变化后 | 意义 |
|--------|--------|--------|------|
| `SP` | `?`（未定义） | `early_init_stack`（物理地址） | **栈指针首次有效**，可以压栈/出栈 |
| `x29`（FP） | `?` | `0` | 帧指针置零 = 标记调用链底部，调试器展开栈时遇到 x29=0 停止 |

#### 内存变化

无新写入。`early_init_stack` 是链接脚本静态预留的内存区，此时只是让 SP 指向它，还没有实际写入。

#### 能力边界

```
之前：SP=? → bl 调用里的被调用者若使用栈 → 写到随机内存地址 → 必然损坏数据
      → 所以 A.1/A.2 只能用寄存器传递所有状态，不能依赖栈
之后：SP 有效 → 可以调用使用局部变量的函数（如接下来的 __pi_create_init_idmap）
```

---

### A.4 `__pi_create_init_idmap` — 构造恒等映射页表

目的：在开 MMU 之前，把"VA==PA"的映射写入内存，让 MMU 开启那一刻 PC 不会 fault。

#### CPU 寄存器变化

| 寄存器 | 变化前 | 变化后 | 意义 |
|--------|--------|--------|------|
| `x0` | `init_idmap_pg_dir` 地址 | 页表占用区域的**结束地址** | 返回值，用于后续 Cache 失效范围计算 |

#### 内存变化

| 数据结构 | 位置 | 写入内容 | 用途 |
|----------|------|----------|------|
| `init_idmap_pg_dir` | 内核镜像内静态区域 | 多级页表项（PGD/PUD/PMD），VA==PA 映射内核物理地址范围 | **MMU 开启过渡专用**，TTBR0_EL1 将指向这里 |

页表项的语义（以 4KB 粒度、内核典型映射为例）：

```
PGD[内核物理地址高位]
  └─ PMD 条目（2MB block 描述符）
       物理地址 = 虚拟地址（恒等）
       属性：Normal 内存，RWX，Inner Shareable
```

写入后立刻执行（根据 x19 分叉）：
- x19=0（MMU off）：`dmb sy` + `dcache_inval_poc(pg_dir_start, pg_dir_end)`  
  → 让页表内存的 Cache 行失效。原因：MMU 硬件遍历页表时直接读内存，若 Cache 中有推测性预取的旧数据，MMU 会解析出错误的页表项
- x19≠0（MMU on）：`dcache_clean_poc(idmap_text_start, idmap_text_end)`  
  → 把 idmap 代码段的 dirty Cache 行写回内存，因为稍后关 MMU 后要直接从内存执行这段代码

#### 能力边界

```
之前：物理内存中没有任何页表 → 无法开启 MMU
之后：init_idmap_pg_dir 就绪 → 可以安全开启 MMU（但 MAIR/TCR 还没配，还不能开）
```

---

### A.5 `init_kernel_el` — 初始化执行级别

目的：把 CPU 配置成内核需要的已知安全状态，明确从哪个 EL 运行。

#### CPU 寄存器变化（EL1 路径）

| 寄存器 | 变化前 | 变化后 | 意义 |
|--------|--------|--------|------|
| `SCTLR_EL1` | firmware 遗留值（未知） | `INIT_SCTLR_EL1_MMU_OFF`（已知安全值） | 关 MMU、关 Cache、正确字节序、关对齐检查 |
| `SPSR_EL1` | `?` | `INIT_PSTATE_EL1`（DAIF 全屏蔽） | `eret` 返回时恢复的 PSTATE |
| `ELR_EL1` | `?` | `lr`（调用方返回地址） | `eret` 返回后继续执行调用方的下一条指令 |
| `x0`（返回值） | - | `BOOT_CPU_MODE_EL1` | 告知调用方从 EL1 启动 |
| `x20` | `?` | `BOOT_CPU_MODE_EL1` | **callee-saved，全程保存启动模式** |

#### CPU 寄存器变化（EL2 路径，更常见）

| 寄存器 | 变化前 | 变化后 | 意义 |
|--------|--------|--------|------|
| `HCR_EL2` | firmware 值 | `HCR_HOST_NVHE_FLAGS \| HCR_ATA` | nVHE 宿主模式；允许 EL1 访问内存标签（MTE） |
| `VBAR_EL2` | `?` | `__hyp_stub_vectors` | 最小化 EL2 向量表，响应从 EL1 发来的 HVC |
| `SCTLR_EL1` | `?` | `INIT_SCTLR_EL1_MMU_OFF` | 为即将降级到的 EL1 准备安全初始值 |
| `SPSR_EL2` | `?` | `INIT_PSTATE_EL1` | `eret` 从 EL2 返回 EL1 时的 PSTATE |
| `ELR_EL2` | `?` | `lr` | `eret` 后跳回调用方，并降级到 EL1 |
| `x0`（返回值） | - | `BOOT_CPU_MODE_EL2 \| [E2H flag]` | 告知调用方从 EL2 启动，是否支持 VHE |
| `x20` | `?` | `x0` 的值 | callee-saved 保存 |

`eret` 指令的副作用：`PC = ELR_EL2`，`PSTATE = SPSR_EL2`，同时 CPU **降级到 EL1**。

#### 内存变化

无。

#### 能力边界

```
之前：SCTLR_EL1 是 firmware 遗留的未知值 → 字节序/对齐行为不确定
      不知道在 EL1 还是 EL2 → 无法判断后续系统寄存器访问是否合法
之后：SCTLR_EL1 = 已知安全值（MMU关、Cache关、正确字节序）
      CPU 确定运行在 EL1，x20 记录启动模式供后续虚拟化代码使用
      若从 EL2：EL2 向量表就绪，后续 HVC 调用可被处理
```

---

### A.6 `__cpu_setup` — 写入内存类型字典与地址翻译参数

目的：告诉 MMU "内存有哪些类型" 以及 "虚拟地址空间怎么划分"，这是开 MMU 前必须配置完的最后一批系统寄存器。

#### CPU 寄存器变化

| 寄存器 | 变化前 | 变化后 | 意义 |
|--------|--------|--------|------|
| `MAIR_EL1` | `?` | 见下方 8槽布局 | **内存类型字典**，页表项 AttrIdx 字段的索引表 |
| `TCR_EL1` | `?` | 见下方字段说明 | 地址翻译的全部参数（位宽/粒度/Cache策略/共享属性） |
| `x0`（返回值） | - | `SCTLR_EL1` 目标值（M 位**未**置1） | 传给 `__primary_switch`，开 MMU 时写入 |

**MAIR_EL1 写入后的内存布局（64位，8个8位槽）：**

```
位 [63:56] 槽7: 未使用（0x00）
位 [55:48] 槽6: 未使用（0x00）
位 [47:40] 槽5: 未使用（0x00）
位 [39:32] 槽4: MT_NORMAL_TAGGED  = 0xFF（MTE预留，初始与Normal相同）
位 [31:24] 槽3: MT_NORMAL         = 0xFF（WBRAWA，所有内核代码/数据）
位 [23:16] 槽2: MT_NORMAL_NC      = 0x44（Non-Cacheable，DMA缓冲区）
位 [15: 8] 槽1: MT_DEVICE_nGnRE   = 0x04（普通MMIO）
位 [ 7: 0] 槽0: MT_DEVICE_nGnRnE  = 0x00（严格设备内存，如GIC）
```

页表项里的 `AttrIdx[2:0]` 就是这里的槽号。例如映射 GIC 寄存器时页表项写 AttrIdx=0，MMU 就会用槽0的属性（禁止推测访问、禁止重排、禁止Cache）处理这段地址。

**TCR_EL1 关键字段：**

```
T1SZ [21:16] = 16  → 内核VA空间 2^(64-16) = 2^48 = 256TB（TTBR1管辖）
T0SZ [ 5: 0] = 16  → 用户VA空间 256TB（TTBR0管辖）
TG1  [31:30]       → 内核页粒度（4KB/16KB/64KB，编译期配置）
TG0  [15:14]       → 用户页粒度
SH1  [29:28] = 0b11 → 内核页表遍历：Inner Shareable（SMP一致性）
SH0  [13:12] = 0b11 → 用户页表遍历：Inner Shareable
IRGN1[25:24] = 0b01 → 内核页表遍历内部Cache：WBWA
ORGN1[27:26] = 0b01 → 内核页表遍历外部Cache：WBWA
```

地址空间因 T0SZ/T1SZ 产生的"洞"：
```
0x0000_0000_0000_0000 ~ 0x0000_FFFF_FFFF_FFFF  ← TTBR0（用户）
        [translation fault zone]
0xFFFF_0000_0000_0000 ~ 0xFFFF_FFFF_FFFF_FFFF  ← TTBR1（内核）
```

#### 内存变化

无。纯系统寄存器操作。

#### 能力边界

```
之前：MAIR/TCR 未配置 → 即使设置 TTBR、置 SCTLR.M=1，MMU 也不知道
      如何解释页表项的 AttrIdx 字段 → 内存访问属性混乱
之后：内存类型字典完备，地址空间划分确定
      x0 持有完整的 SCTLR_EL1 目标值（含正确的 Cache/MMU 控制位，M 位除外）
      具备开 MMU 的全部前提条件（页表+MAIR+TCR+TTBR 四件套）
```

---

## 阶段 B：`__primary_switch` — 开启 MMU，穿越物理/虚拟边界

**这是整个启动过程中最关键的单个时刻。**

---

### B.1 设置 TTBR，写 SCTLR.M=1

#### CPU 寄存器变化

| 寄存器 | 变化前 | 变化后 | 意义 |
|--------|--------|--------|------|
| `TTBR0_EL1` | `?` | `init_idmap_pg_dir` 物理地址 | 用户空间页表根 → 指向恒等映射（VA==PA，覆盖当前 PC） |
| `TTBR1_EL1` | `?` | `init_pg_dir` 物理地址 | 内核空间页表根 → 指向早期内核映射 |
| `SCTLR_EL1` | M=0（MMU 关闭） | **M=1（MMU 开启）** | **这条写指令执行后，CPU 立刻开始翻译所有地址** |

写入 SCTLR_EL1.M=1 之后，紧跟 `isb`（指令同步屏障）：确保流水线中所有在途指令都感知到 MMU 已开启，避免旧的物理地址取指污染。

#### 内存变化（TLB 状态）

| 资源 | 变化前 | 变化后 | 原因 |
|------|--------|--------|------|
| TLB | 空或 firmware 遗留 | `tlbi vmalle1` 全量失效 | 清除任何 firmware 遗留的翻译缓存，强制 MMU 从 TTBR 指向的页表重新遍历 |

#### MMU 开启瞬间的连续性保证

```
MMU 开启前最后一条指令地址（物理）：0x4020_XXXX
↓  SCTLR_EL1.M = 1
MMU 开启后第一条指令地址（虚拟）：  0x4020_XXXX → TTBR0 → init_idmap_pg_dir → 物理 0x4020_XXXX ✓

因为恒等映射 VA==PA，翻译结果不变，PC 继续正常递增。
若没有 init_idmap_pg_dir，此处会立刻触发 translation fault → 死机。
```

然后跳入虚拟地址的 `__primary_switched`（高虚拟地址，TTBR1 管辖）：

```
b __primary_switched  ← 这里已经是 0xFFFF_xxxx_xxxx_xxxx 虚拟地址
```

#### 能力边界

```
之前：所有地址 = 物理地址；无虚拟内存保护；无法区分内核/用户空间
之后：所有地址经 MMU 翻译；内核地址空间（TTBR1）与用户地址空间（TTBR0）独立
      phys_to_virt / virt_to_phys 语义正式成立（但 kimage_voffset 还未写，C 代码还不能用）
      但此刻 TTBR1 指向的 init_pg_dir 只有内核镜像映射，线性映射未建立
```

---

## 阶段 C：`__primary_switched` — 虚拟世界早期

**进入条件**：MMU=on，运行在 `0xFFFF_xxxx` 虚拟地址，但 current 未设置，异常向量未装载，C 运行时未就绪。

---

### C.1 `init_cpu_task init_task` — 建立第一个任务上下文

目的：让 CPU 知道"当前进程是谁"，建立内核栈，让 C 代码的 `current` 宏可用。

#### CPU 寄存器变化

| 寄存器 | 变化前 | 变化后 | 意义 |
|--------|--------|--------|------|
| `SP_EL0` | `?` | `&init_task`（虚拟地址） | **ARM64 内核约定：SP_EL0 = current**。`current` 宏展开为 `read_sysreg(sp_el0)` |
| `SP` | `early_init_stack`（物理时代遗留） | `init_task.stack + THREAD_SIZE - PT_REGS_SIZE` | 切换到 init 进程的正式内核栈 |
| `x29`（FP） | `0`（之前置零） | `SP + S_STACKFRAME` | 帧指针指向栈顶的 stackframe 结构体 |
| `TPIDR_EL1` | `?` | `__per_cpu_offset[0]`（CPU 0 的 per-CPU 区偏移） | per-CPU 变量访问的基地址。`this_cpu_read(v)` 展开为 `*(TPIDR_EL1 + offsetof(v))` |
| `x18` | `?` | Shadow Call Stack 指针（若启用 SCS） | 影子调用栈指针，存真实返回地址，防 ROP 攻击 |

#### 内存变化

| 数据结构 | 位置 | 写入内容 | 用途 |
|----------|------|----------|------|
| `init_task` 内核栈顶的 `pt_regs` 区 | `init_stack + THREAD_SIZE - PT_REGS_SIZE` | `stackframe.fp=0, stackframe.pc=0, stackframe.type=FINAL` | 调用链终止标记，`unwind_frame` 遇到 `type=FINAL` 时停止栈回溯 |

**init_task 内核栈内存布局（写入后）：**

```
init_stack + THREAD_SIZE（高地址）：
┌────────────────────────────────────┐ ← SP（写入后的值）
│  struct pt_regs                    │  大小 = PT_REGS_SIZE
│  ├── [S_STACKFRAME]                │
│  │     fp  = 0                     │ ← 调用链终点
│  │     pc  = 0                     │
│  │     type = FRAME_META_TYPE_FINAL│
│  └── 其余字段（此时为 0）           │
├────────────────────────────────────┤
│                                    │
│  函数调用栈空间（向下增长）          │
│                                    │
├────────────────────────────────────┤
│  struct thread_info                │ ← 栈底，init_task.stack 指向此处
└────────────────────────────────────┘ init_stack（低地址）
```

#### 能力边界

```
之前：current = ? → 任何读取 current 的代码（如加锁、调度器）都会崩溃
      SP = early_init_stack（属于内核镜像里的小块区域，容量有限）
之后：current = init_task → 内核 C 代码可以安全调用 current->xxx
      SP = init 进程内核栈（THREAD_SIZE 大小，通常 16KB 或 32KB）
      per-CPU 变量访问可用（TPIDR_EL1 已设置）
      栈回溯工具（perf/ftrace）可正确终止在此帧
```

---

### C.2 `VBAR_EL1 = vectors` — 装载异常向量表

#### CPU 寄存器变化

| 寄存器 | 变化前 | 变化后 | 意义 |
|--------|--------|--------|------|
| `VBAR_EL1` | `?`（firmware 遗留或未定义） | `vectors`（entry.S 定义的内核向量表虚拟地址） | 所有 EL1 异常的跳转基地址 |

`isb` 紧随其后：确保流水线中已取到的旧向量入口被丢弃，后续异常使用新向量表。

#### 内存变化（向量表本身早已编译进内核，此处无写操作）

向量表结构（`entry.S` 中，写 VBAR 前已在内存中）：

```
vectors（16 个入口，每入口 128 字节 = 32 条指令空间）：
  +0x000  EL1t 同步异常（当前EL，使用 SP_EL0）
  +0x080  EL1t IRQ
  +0x100  EL1t FIQ
  +0x180  EL1t SError
  +0x200  EL1h 同步异常（当前EL，使用 SP_EL1）← 内核正常路径
  +0x280  EL1h IRQ                              ← 内核中断路径
  +0x300  EL1h FIQ
  +0x380  EL1h SError
  +0x400  EL0 64bit 同步（系统调用 SVC 入口）   ← 用户态 syscall
  +0x480  EL0 64bit IRQ
  ...
```

#### 能力边界

```
之前：VBAR_EL1 = firmware 遗留值 → 发生任何异常（包括 IRQ、缺页、除零）→ 跳到未知地址 → 死机
之后：异常有合法处理入口 → 可以安全开中断、可以发生缺页异常（虽然此时还不能处理缺页）
      内核调试器（kgdb）、ftrace、kprobe 的断点机制从此可用（依赖异常向量）
```

---

### C.3 写入全局变量，跳入 `start_kernel`

#### CPU 寄存器变化

无新的寄存器写入。

#### 内存变化

| 数据结构 | 位置 | 写入内容 | 用途 |
|----------|------|----------|------|
| `__fdt_pointer` | `.init.data` 段 | FDT 物理地址（从 x21 来） | C 代码通过此变量访问设备树（`setup_machine_fdt` 会读它） |
| `kimage_voffset` | `.data` 段 | `_text 虚拟地址 - _text 物理地址` | `__pa(va) = va - kimage_voffset`，`__va(pa) = pa + kimage_voffset`，内核物理/虚拟地址互换的基础 |
| `__boot_cpu_mode[0]` | `.data` 段 | `BOOT_CPU_MODE_EL1` 或 `BOOT_CPU_MODE_EL2` | 次级 CPU 启动时读取，决定自己用 EL1 还是 EL2 路径 |

然后 `bl start_kernel`：跳入 C 世界，**永不返回**（`ASM_BUG()` 兜底）。

#### 能力边界

```
之前：kimage_voffset 未写 → __pa/__va 宏结果错误 → 任何物理/虚拟地址转换都不可信
之后：kimage_voffset 就绪 → __pa/__va 全面可用
      C 代码可访问 FDT（通过 __fdt_pointer）
      进入 start_kernel，后续全部是 C 代码
```

---

## 阶段 D：`start_kernel` — 内核子系统初始化

**进入条件**：MMU=on，current=init_task，VBAR 已装载，中断关闭（PSTATE.DAIF.I=1），`alloc_pages`/`kmalloc` 均不可用。

---

### D.1 `set_task_stack_end_magic` — 写入栈溢出哨兵

#### CPU 寄存器变化

无。

#### 内存变化

| 数据结构 | 位置 | 写入内容 | 用途 |
|----------|------|----------|------|
| `init_task` 栈底第一个 `unsigned long` | `init_stack`（最低地址） | `STACK_END_MAGIC = 0x57AC6E9D` | 运行时检测栈溢出：若此值被覆盖，说明栈从底部溢出，触发 `panic` |

#### 能力边界

```
之前：栈溢出是无声的内存损坏，极难定位
之后：栈溢出立刻被检测并 panic，有明确的错误信息
```

---

### D.2 `smp_setup_processor_id` — 建立 CPU 编号映射

#### CPU 寄存器变化

| 寄存器 | 变化前 | 变化后 | 意义 |
|--------|--------|--------|------|
| `MPIDR_EL1` | 硬件值（只读） | 不变（只读） | CPU 拓扑寄存器，格式：`[Aff3][Aff2][Aff1][Aff0]`，标识 cluster/core/thread |

读 `MPIDR_EL1`，写入内存，寄存器本身无变化。

#### 内存变化

| 数据结构 | 位置 | 写入内容 | 用途 |
|----------|------|----------|------|
| `cpu_logical_map[0]` | `.data` 段 | boot CPU 的 MPIDR 值（屏蔽保留位后） | **逻辑 CPU 编号 → 物理 MPIDR 的映射表**。次级 CPU 启动时用此表找到自己的逻辑编号；SMP 调度依赖 |

#### 能力边界

```
之前：只知道"有一个 boot CPU"，不知道它的物理 ID
之后：CPU 0 的物理拓扑 ID 确定，cpu_logical_map[0] 就绪
      次级 CPU 的 MPIDR 注册到此表后，SMP 才能正确识别每个 CPU
```

---

### D.3 `local_irq_disable` + `boot_cpu_init` — 关中断，初始化 CPU 位图

#### CPU 寄存器变化

| 寄存器 | 变化前 | 变化后 | 意义 |
|--------|--------|--------|------|
| `PSTATE.DAIF.I` | firmware/EL 初始化时已设（通常关闭） | 强制置 1（中断屏蔽） | 显式确保后续初始化过程不被中断打断 |

#### 内存变化

| 数据结构 | 位置 | 写入内容 | 用途 |
|----------|------|----------|------|
| `cpu_possible_mask` | `.data` 段（cpumask 位图） | bit 0 置 1 | "系统中存在的 CPU 集合"，循环 `for_each_possible_cpu` 的范围上界 |
| `cpu_present_mask` | `.data` 段 | bit 0 置 1 | "当前插着的 CPU"，热插拔时动态变化 |
| `cpu_online_mask` | `.data` 段 | bit 0 置 1 | "当前在线（可调度）的 CPU"，调度器用 |
| `cpu_active_mask` | `.data` 段 | bit 0 置 1 | "当前活跃（可接收任务迁移）的 CPU" |

这 4 个位图是全系统 CPU 状态管理的基础，所有 `for_each_online_cpu`、`for_each_possible_cpu` 等宏都遍历它们。

#### 能力边界

```
之前：CPU 位图全0 → 任何遍历 CPU 的操作结果为空集 → 调度器无法工作
之后：CPU 0 已注册 → 单核调度路径可用；为后续次级 CPU 注册做好框架
```

---

### D.4 `setup_arch` — ARM64 架构核心初始化

这是 `start_kernel` 中体量最大的单次调用，内部有多个关键子步骤。

---

#### D.4.1 DTB 解析与 memblock 注册

#### CPU 寄存器变化

无（纯内存操作）。

#### 内存变化

| 数据结构 | 位置 | 写入内容 | 用途 |
|----------|------|----------|------|
| `memblock.memory.regions[]` | `.data` 段（memblock 静态结构） | 每条 RAM 物理区间：`{base, size, flags}` | **早期内存分配器的"可用内存清单"**，`memblock_alloc` 从这里找空闲块 |
| `memblock.reserved.regions[]` | `.data` 段 | 内核镜像区间、DTB 区间、crashkernel 等 | 标记已占用，防止 memblock_alloc 把这些区域分配出去 |
| `of_node` 树（DTB展开） | memblock 分配 | 每个设备树节点 → 一个 `struct device_node`，按父子关系链接 | 驱动通过 `of_find_node_by_*` 查找硬件描述；内存、CPU、中断拓扑从这里读取 |

```
DTB 中的内存节点（示例）：
  memory@40000000 { reg = <0x40000000 0x40000000>; }
    ↓ early_init_dt_scan_memory 解析
  memblock_add(0x40000000, 0x40000000)
    ↓ 写入 memblock.memory.regions[0] = {base=0x4000_0000, size=0x4000_0000}
```

#### 能力边界

```
之前：不知道系统有多少 RAM、在哪里 → memblock_alloc 无物可分配
之后：所有 RAM 注册完毕 → memblock_alloc 可用（内核的第一个内存分配器）
      设备树节点树建立 → 后续驱动初始化可通过 OF API 查找硬件参数
```

---

#### D.4.2 `paging_init` — 构造最终内核页表

这是启动期间最大的内存写操作，之后 `init_pg_dir` 被废弃，`swapper_pg_dir` 接管。

#### CPU 寄存器变化

| 寄存器 | 变化前 | 变化后 | 意义 |
|--------|--------|--------|------|
| `TTBR1_EL1` | `init_pg_dir` 物理地址（早期） | `swapper_pg_dir` 物理地址 | 内核页表切换为完整版本 |
| TLB | `init_pg_dir` 产生的缓存 | `tlbi vmalle1is` 全量失效 | 强制 MMU 使用新页表重新遍历 |

#### 内存变化

| 数据结构 | 位置 | 写入内容 | 用途 |
|----------|------|----------|------|
| `swapper_pg_dir` | memblock 分配 | 三/四级页表，覆盖整个内核虚拟地址空间 | 最终内核页表，此后 TTBR1_EL1 永久指向这里 |
| 线性映射页表项 | swapper_pg_dir 内 | 所有 RAM → `PA + PAGE_OFFSET`（大页映射，通常 2MB block） | `phys_to_virt(pa)` 和 `virt_to_phys(va)` 的硬件基础 |
| vmemmap 页表项 | swapper_pg_dir 内 | `vmemmap` 虚拟地址区间 → 为 `struct page` 数组预留的物理页 | 每个物理页帧（PFN）对应一个 `struct page`，`pfn_to_page(pfn)` 直接寻址 |
| vmalloc 区页表 | swapper_pg_dir 内 | 虚拟地址区间预留（PGD 条目存在，PTE 全空） | `vmalloc`/`ioremap` 可以在这里填写新映射，无需修改 PGD |

**虚拟地址空间布局（写入 swapper_pg_dir 后）：**

```
TTBR1 空间（0xFFFF_0000_0000_0000 起）：

[PAGE_OFFSET]
 0xFFFF_0000_0000_0000 ┌──────────────────────────────┐
                       │  线性映射区（Direct Map）      │
                       │  所有 RAM 在此直接映射          │
                       │  va = pa + PAGE_OFFSET         │
                       │  大页（2MB）映射，TLB 压力小    │
 0xFFFF_7FFF_FFFF_FFFF └──────────────────────────────┘

 0xFFFF_8000_0000_0000 ┌──────────────────────────────┐
                       │  vmalloc / ioremap 区          │
                       │  页表框架已建，PTE 按需填写     │
 0xFFFF_FEFF_BFFF_FFFF └──────────────────────────────┘

 0xFFFF_FF00_0000_0000 ┌──────────────────────────────┐
                       │  vmemmap                      │
                       │  struct page[NR_PAGES] 数组   │
                       │  每个物理页对应一个 struct page │
 0xFFFF_FFFF_FFFF_FFFF └──────────────────────────────┘
```

#### 能力边界

```
之前：只有内核镜像自身有虚拟映射；RAM 无法通过虚拟地址访问
之后：所有 RAM 通过线性映射可访问 → phys_to_virt / virt_to_phys 全面可用
      vmemmap 建立 → pfn_to_page / page_to_pfn 可用（buddy allocator 的基础）
      vmalloc/ioremap 区域框架就绪 → 设备驱动可以映射 MMIO
```

---

### D.5 `setup_per_cpu_areas` — 为每个 CPU 分配独立数据区

#### CPU 寄存器变化

| 寄存器 | 变化前 | 变化后 | 意义 |
|--------|--------|--------|------|
| `TPIDR_EL1`（CPU 0） | `__per_cpu_offset[0]`（初始） | 更新为正式分配后的 `__per_cpu_offset[0]` | per-CPU 变量的实际基地址确定 |

#### 内存变化

| 数据结构 | 位置 | 写入内容 | 用途 |
|----------|------|----------|------|
| CPU 0 per-CPU 区域 | memblock 分配（后由 buddy 管理） | 复制 `__per_cpu_start ~ __per_cpu_end` 模板内容 | CPU 0 的所有 per-CPU 变量副本 |
| CPU 1..N per-CPU 区域 | memblock 分配 | 同上，各自独立 | 每个 CPU 的私有变量副本，访问无需加锁 |
| `__per_cpu_offset[0..N]` | `.data` 段数组 | 各 CPU 区域起始地址 - `__per_cpu_start` | per-CPU 变量寻址：`this_cpu_ptr(&var) = &var + __per_cpu_offset[cpu]` |

**per-CPU 变量访问机制：**

```
DEFINE_PER_CPU(int, my_counter);

this_cpu_inc(my_counter);
  ↓ 编译后
  ldr x0, [TPIDR_EL1]           // 读当前 CPU 的偏移量
  add x0, x0, #offsetof(my_counter 在模板中的位置)
  ldr x1, [x0]
  add x1, x1, 1
  str x1, [x0]                  // 无锁！不同 CPU 访问不同物理地址
```

#### 能力边界

```
之前：per-CPU 变量全部指向模板区域（所有 CPU 共享一份） → 读写会相互覆盖
之后：每个 CPU 有独立副本 → this_cpu_read/write 无锁安全
      运行队列、中断计数器、本地定时器等关键 per-CPU 变量正式独立
```

---

### D.6 `mm_init` — 内存管理子系统完整初始化

---

#### D.6.1 `mem_init` — memblock 交接给 buddy allocator

#### CPU 寄存器变化

无。

#### 内存变化

| 数据结构 | 位置 | 写入内容 | 用途 |
|----------|------|----------|------|
| `struct page` 数组（vmemmap） | vmemmap 虚拟地址区 | 每个物理页帧初始化：`order`/`flags`/`_refcount`/链表指针 | buddy allocator 操作的原子单元 |
| `zone->free_area[order]` | 每个 NUMA 节点的 `struct zone` 中 | 各 order（0~10）的空闲页链表头 | buddy 分配器的核心数据：按 2^order 大小组织空闲页块 |
| `node_data[n]->node_zones[]` | NUMA 节点描述符 | DMA / Normal / HighMem 各区统计信息 | 内存分配时按 zone 选择（DMA 设备限制、Normal 普通用途） |

**buddy allocator 的内存视图（写入后）：**

```
free_area[0]  → 4KB  页链表：[page_A] → [page_B] → ...
free_area[1]  → 8KB  页对链表
free_area[2]  → 16KB 页组链表
...
free_area[10] → 4MB  页组链表（2^10 × 4KB）

alloc_pages(GFP_KERNEL, 0) → 从 free_area[0] 取一页
alloc_pages(GFP_KERNEL, 3) → 从 free_area[3] 取 8 页（32KB）
```

#### 能力边界

```
之前：内存分配只能用 memblock（只能分配不能释放，粒度粗）
之后：alloc_pages / free_pages 可用（任意 4KB 整数倍，可释放）
      内核的所有动态内存分配从此建立在 buddy 之上
```

---

#### D.6.2 `kmem_cache_init` — slab 分配器初始化

#### CPU 寄存器变化

无。

#### 内存变化

| 数据结构 | 位置 | 写入内容 | 用途 |
|----------|------|----------|------|
| `kmem_cache` 链表 | buddy 分配的内存 | 各种固定大小的 slab cache：8B/16B/32B/.../4KB | 小对象分配池，避免每次 kmalloc 都调用 buddy（buddy 最小粒度 4KB） |
| per-CPU `kmem_cache_cpu` | per-CPU 区域 | 本地缓存的空闲对象链表（hot cache） | `kmalloc` 首先从本地 CPU 缓存取，命中率极高，无需加锁 |

#### 能力边界

```
之前：没有 slab → 分配 task_struct（~10KB）需要调 buddy，浪费一整页（4KB）
      且频繁 alloc/free 会严重碎片化 buddy
之后：kmalloc(size, flags) 可用 → 任意字节大小的内核内存分配
      task_struct / file / inode / dentry 等核心对象从对应 slab cache 分配
```

---

### D.7 `jump_label_init` — 建立静态分支索引

#### CPU 寄存器变化

无（此时只建立索引，实际 patch 代码发生在后续 `static_branch_enable` 调用时）。

#### 内存变化

| 数据结构 | 位置 | 写入内容 | 用途 |
|----------|------|----------|------|
| `__jump_table` section 排序 | `.rodata` 段（编译进内核） | 按 `key` 地址排序，使二分查找可用 | `static_branch_enable(key)` 时快速找到所有需要 patch 的指令地址 |

**static key 的代码 patch 机制：**

```
编译时：                          运行时 static_branch_enable 后：
  if (static_branch_unlikely(&k)) →  if (static_branch_unlikely(&k))
    NOP                               B target_label   ← 代码被原地 patch
    ...                               ...

__jump_table 中的记录：
  struct jump_entry {
      .code   = NOP 指令的地址   ← patch 目标
      .target = target_label     ← 若开启，patch 成 B target
      .key    = &k               ← 用哪个 key 控制
  }
```

#### 能力边界

```
之前：static_branch_enable/disable 无法找到对应的 NOP/JMP 位置
之后：可以通过 static_branch_enable 在运行时改变代码路径（零性能开销的特性开关）
      tracepoint、KASAN、锁调试等大量基础设施依赖此机制
```

---

### D.8 `sched_init` — 调度器初始化

#### CPU 寄存器变化

无。

#### 内存变化

| 数据结构 | 位置 | 写入内容 | 用途 |
|----------|------|----------|------|
| per-CPU `struct rq` | per-CPU 区域 | 运行队列：CFS/RT/DL 三棵树初始化，`curr=init_task` | 调度器决策的核心数据。每个 CPU 一个运行队列，相互独立 |
| `root_task_group` | `.data` 段 | CFS band-width 控制组的根节点 | cgroup cpu 子系统的根 |
| `init_task` sched 字段 | `init_task.sched_class` | `&idle_sched_class`（idle 调度类） | init_task 将在 `rest_init` 后成为 idle 进程，调度优先级最低 |

**`struct rq`（运行队列）内部结构：**

```
per-CPU struct rq {
  struct cfs_rq cfs;       // 完全公平调度队列（红黑树，普通进程）
  struct rt_rq  rt;        // 实时调度队列（优先级位图，RT进程）
  struct dl_rq  dl;        // 截止时间调度队列（红黑树，Deadline进程）
  struct task_struct *curr; // 当前正在运行的进程
  struct task_struct *idle; // idle 进程（= init_task，后来替换）
  u64 clock;               // 运行队列的单调时钟
  ...
}
```

#### 能力边界

```
之前：没有运行队列 → 无法 fork 进程（fork 后需要 enqueue_task）
之后：调度器基础设施就绪，但只有 init_task 一个进程
      wake_up_process / schedule 可用
      抢占机制就绪（但中断还关着，实际抢占发生在开中断之后）
```

---

### D.9 `rcu_init` — RCU（读-复制-更新）初始化

#### CPU 寄存器变化

无。

#### 内存变化

| 数据结构 | 位置 | 写入内容 | 用途 |
|----------|------|----------|------|
| `rcu_state`（全局） | `.data` 段 | GP（宽限期）状态机：`gp_seq`、节点树根 | 跟踪全系统所有 CPU 是否都已通过静止状态（quiescent state） |
| `rcu_node` 树 | `.data` 段静态数组 | 按 CPU 拓扑组织的层次树，每节点覆盖一组 CPU | 宽限期结束检测：叶节点 CPU 报告 QS → 逐层汇聚到根 |
| per-CPU `rcu_data` | per-CPU 区域 | 本地 RCU 回调链表、QS 状态标志 | 每个 CPU 独立记录待执行的 RCU 回调（`call_rcu` 挂入此处） |

#### 能力边界

```
之前：rcu_read_lock/unlock 语义未定义；call_rcu 无处挂回调
之后：rcu_read_lock/unlock 可用（零开销临界区）
      call_rcu 可用（延迟释放内存，等所有 CPU 离开临界区后执行）
      内核中几乎所有子系统（网络/VFS/进程管理）都依赖 RCU
```

---

### D.10 `init_IRQ` + `softirq_init` — 中断基础设施

#### CPU 寄存器变化

（`local_irq_enable` 在 `start_kernel` 末尾调用，此时仍关闭）

| 寄存器 | 变化前 | 变化后（`local_irq_enable` 后） | 意义 |
|--------|--------|--------|------|
| `PSTATE.DAIF.I` | 1（屏蔽） | 0（允许 IRQ） | 此后 IRQ 可以到达，调度器抢占从此真正工作 |

#### 内存变化

| 数据结构 | 位置 | 写入内容 | 用途 |
|----------|------|----------|------|
| `irq_desc[NR_IRQS]`（或 radix tree） | kmalloc/slab 分配 | 每个中断号一个描述符：handler、action 链表、affinity | `request_irq` 注册的处理函数挂在 `action` 链表上 |
| GIC 寄存器（MMIO） | ioremap 映射后 | 初始化 GIC distributor/CPU interface | ARM64 通用中断控制器，硬件路由中断到 CPU |
| per-CPU `softirq_vec[NR_SOFTIRQS]` | per-CPU 区域 | 各软中断的 action 函数指针 | tasklet、网络 RX/TX、timer 等延迟处理机制 |

#### 能力边界

```
之前：IRQ 被 DAIF.I 屏蔽 → 没有任何中断可以到达 → 定时器不工作 → 调度无法抢占
之后（开中断后）：
  - 时钟中断到达 → jiffies 递增 → 定时器可工作
  - 调度时钟中断 → 触发抢占 → 多任务调度真正开始
  - 设备中断到达 → 驱动可以响应硬件事件
```

---

### D.11 `timekeeping_init` + `time_init` — 时钟初始化

#### CPU 寄存器变化

| 寄存器 | 变化前 | 变化后 | 意义 |
|--------|--------|--------|------|
| `CNTP_CTL_EL0`（物理计时器控制） | 禁用 | 使能 | ARM64 通用定时器（arch timer）开始计数 |
| `CNTV_CTL_EL0`（虚拟计时器控制） | 禁用 | 使能（若使用虚拟计时器） | 虚拟机场景下的时钟 |

#### 内存变化

| 数据结构 | 位置 | 写入内容 | 用途 |
|----------|------|----------|------|
| `timekeeper`（全局） | `.data` 段 | 当前时间基准、clock source 引用、纳秒转换参数 | `ktime_get()` / `gettimeofday()` 的数据来源 |
| `jiffies_64` | `.data` 段 | 初始值（通常对应开机时间） | 整个内核的"心跳"计数，定时器、超时、调度时间片均基于此 |

#### 能力边界

```
之前：没有时钟 → msleep/udelay 不工作；定时器无法触发；调度时间片无法计量
之后：ktime_get() 可用；jiffies 开始递增；mod_timer / schedule_timeout 可用
```

---

## 阶段 E：`rest_init` — 进程化，内核主线退为 idle

**进入条件**：完整内核运行环境就绪，中断已开启，`kmalloc`/`alloc_pages`/调度器全部可用。

---

### E.1 创建 PID 1（`kernel_init`）

#### CPU 寄存器变化

无（fork 是普通函数调用）。

#### 内存变化

| 数据结构 | 位置 | 写入内容 | 用途 |
|----------|------|----------|------|
| PID 1 的 `task_struct` | slab cache（`task_struct` kmem_cache） | 从 init_task 复制，`pid=1`，`comm="init"` | 第一个真正的用户进程前身 |
| PID 1 的内核栈 | buddy allocator（`alloc_thread_stack_node`） | 空（进程刚创建，尚未运行） | kernel_init 函数的执行栈 |
| `init_pid_ns.idr` | slab | PID 1 的 IDR 条目 | PID 分配器，`find_task_by_pid` 通过这里查找 |
| PID 1 加入 `init_task.children` 链表 | task_struct 内 | 链接到 init_task 的子进程链表 | 进程树结构（`pstree` 命令看到的树形结构） |

**PID 1 的最终命运**（在 kernel_init 函数中）：
```
kernel_init()
  → kernel_init_freeable()    释放 init 内存段（__init 标记的代码/数据）
  → try_to_run_init_process("/sbin/init")
      → do_execve("/sbin/init")   ← 用户态 init 进程替换内核 init 线程
```

#### 能力边界

```
之前：系统中只有 PID 0（init_task），没有可调度的其他进程
之后：PID 1 进入运行队列，调度器有了第一个"真正的任务"
      用户态 init 路径开启
```

---

### E.2 创建 PID 2（`kthreadd`）

#### CPU 寄存器变化

无。

#### 内存变化

| 数据结构 | 位置 | 写入内容 | 用途 |
|----------|------|----------|------|
| PID 2 的 `task_struct` | slab | `pid=2`，`comm="kthreadd"` | 内核线程守护进程 |
| `kthreadd_task`（全局指针） | `.data` 段 | 指向 PID 2 的 task_struct | `kthread_create` 内部通过此指针向 kthreadd 发送创建请求 |
| `kthread_create_list`（全局链表） | `.data` 段 | 初始化为空链表 | kthread_create 把创建请求挂入此链表，kthreadd 从链表取任务并 fork |

**kthreadd 的工作模式：**
```
kthreadd() {
  for (;;) {
    if (list_empty(&kthread_create_list))
      schedule();   // 没活干就睡眠
    // 从链表取出创建请求
    task = create_kthread(create);  // fork 出新内核线程
  }
}
// 所有 kthread（kworker, ksoftirqd, kswapd 等）都是 kthreadd 的子进程
```

#### 能力边界

```
之前：kthread_create 无法工作（kthreadd_task = NULL）
之后：kthread_create / kthread_run 可用
      所有驱动的 work queue、内核定时回收线程等都依赖此机制
```

---

### E.3 PID 0 → idle 进程

#### CPU 寄存器变化

| 寄存器 | 变化前 | 变化后 | 意义 |
|--------|--------|--------|------|
| `PSTATE` | 正常执行态 | 调度器切换到 PID 1 时 PSTATE 保存到 init_task 的内核栈 | init_task 的寄存器上下文被保存，等待被切换回来 |

`cpu_startup_entry(CPUHP_ONLINE)` 之后：

```
cpu_startup_entry
  → do_idle()
      for (;;) {
          if (need_resched()) schedule();
          arch_cpu_idle();   // WFI（Wait For Interrupt）
      }
```

#### 内存变化

| 数据结构 | 位置 | 写入内容 | 用途 |
|----------|------|----------|------|
| `init_task.sched_class` | task_struct 字段 | 已在 `sched_init` 时设为 `&idle_sched_class` | idle 调度类：只有运行队列为空时才被选中运行 |

#### 能力边界

```
之前：init_task 是唯一进程，CPU 不会调度到其他任务
之后：init_task 变为 idle，调度器优先运行 PID 1/PID 2
      CPU 空闲时执行 WFI，降低功耗
      系统进入正常的多进程运行状态
```

---

## 全程状态演化时间线

```
时刻  步骤         CPU 寄存器关键变化                  内存新增数据结构
─────────────────────────────────────────────────────────────────────────────
T01   进入内核     x0=FDT_phys, 其余=?                 无
T02   A.1          x19 = MMU状态(0或1)                  无
T03   A.2          x21 = FDT地址                        boot_args[4]（BSS段）
T04   A.3          SP = early_init_stack, x29 = 0       无（SP 首次有效）
T05   A.4          -（x0=页表结束地址，仅临时用）        init_idmap_pg_dir（恒等映射页表）
T06   A.5          SCTLR_EL1 = INIT_SCTLR_EL1_MMU_OFF  无
                   HCR_EL2/VBAR_EL2（若EL2）
                   x20 = 启动模式（EL1/EL2）
T07   A.6          MAIR_EL1 = 内存类型字典（5槽）        无
                   TCR_EL1 = VA位宽/粒度/Cache策略
                   x0 = SCTLR_EL1目标值
T08   B.1          TTBR0_EL1 = init_idmap_pg_dir        init_pg_dir（早期内核页表，此前已填写）
                   TTBR1_EL1 = init_pg_dir              TLB 全量失效
                   SCTLR_EL1.M = 1  ← MMU ON ★
T09   C.1          SP_EL0 = &init_task（current就绪）   init_task 栈帧（stackframe 终止标记）
                   SP = init 内核栈
                   TPIDR_EL1 = CPU0 per-CPU 偏移
T10   C.2          VBAR_EL1 = vectors                   无（向量表已在内存，写寄存器指向它）
T11   C.3          -                                     __fdt_pointer, kimage_voffset,
                                                         __boot_cpu_mode[]
T12   D.1          -                                     init_task 栈底哨兵 0x57AC6E9D
T13   D.2          MPIDR_EL1 读取（只读）                cpu_logical_map[0]
T14   D.3          PSTATE.DAIF.I = 1（显式关中断）       4 个 cpumask 位图（bit 0 置1）
T15   D.4.1        -                                     memblock.memory/reserved 区域表
                                                         of_node 设备树节点树
T16   D.4.2        TTBR1_EL1 = swapper_pg_dir ★         swapper_pg_dir（完整内核页表）
                   TLB 全量失效                           线性映射/vmemmap/vmalloc 页表项
T17   D.5          TPIDR_EL1 更新为正式偏移量            per-CPU 数据区域（每 CPU 一份）
                                                         __per_cpu_offset[] 数组
T18   D.6.1        -                                     struct page 数组（vmemmap 区）
                                                         zone->free_area[] buddy 空闲链表
T19   D.6.2        -                                     kmem_cache 链表（slab）
                                                         per-CPU kmem_cache_cpu（hot cache）
T20   D.7          -                                     __jump_table 排序完成
T21   D.8          -                                     per-CPU struct rq（运行队列）
                                                         init_task → idle_sched_class
T22   D.9          -                                     rcu_state, rcu_node 树
                                                         per-CPU rcu_data
T23   D.10         GIC MMIO 写入                         irq_desc 数组
                                                         per-CPU softirq_vec[]
T24   D.11         CNTP_CTL_EL0 使能                     timekeeper, jiffies_64
T25   D.xx         PSTATE.DAIF.I = 0  ← 中断开启 ★      -
T26   E.1          -                                     PID 1 task_struct + 内核栈
T27   E.2          -                                     PID 2 task_struct（kthreadd）
                                                         kthreadd_task 指针
T28   E.3          PSTATE 保存到 init_task 栈            -（init_task 进入 idle WFI 循环）
```

★ 标记三个最关键的状态突变：MMU 开启、页表切换为最终版本、中断开启。

---

## 能力解锁顺序（依赖树）

```
SP 有效（T04）
  └→ 可调用含局部变量的函数

init_idmap_pg_dir 就绪（T05）+ MAIR/TCR 配置（T07）
  └→ MMU 可以安全开启

MMU 开启（T08）
  └→ 虚拟地址空间激活
  └→ phys_to_virt / virt_to_phys 语义成立（T11 写 kimage_voffset 后可用）

current = init_task（T09）
  └→ 内核 C 代码 current->xxx 可安全访问

VBAR_EL1 = vectors（T10）
  └→ 异常可被处理，可以安全开中断

memblock 就绪（T15）
  └→ memblock_alloc 可用（第一个内存分配器）

swapper_pg_dir + vmemmap（T16）
  └→ pfn_to_page / page_to_pfn 可用
  └→ buddy allocator（T18）可初始化
       └→ alloc_pages 可用
            └→ kmalloc 可用（T19）
                 └→ task_struct fork 可用
                      └→ rest_init 创建 PID 1/2（T26/T27）

调度器 struct rq（T21）
  └→ enqueue_task / wake_up_process 可用
  └→ 中断开启（T25）后，时钟中断触发抢占
       └→ 多任务调度真正开始（T25 之后）
```
