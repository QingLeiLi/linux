# ARM64 中断模型与关中断机制

## 目录

1. [中断信号线：IRQ 与 FIQ](#1-中断信号线irq-与-fiq)
2. [DAIF 寄存器](#2-daif-寄存器)
3. [DAIF 路径：传统关中断](#3-daif-路径传统关中断)
4. [GICv3 PMR 寄存器](#4-gicv3-pmr-寄存器)
5. [PMR 路径：支持伪 NMI 的关中断](#5-pmr-路径支持伪-nmi-的关中断)
6. [伪 NMI](#6-伪-nmi)
7. [两条路径对比](#7-两条路径对比)
8. [编译器屏障 barrier()](#8-编译器屏障-barrier)
9. [相关源文件](#9-相关源文件)

---

## 1. 中断信号线：IRQ 与 FIQ

ARM 处理器对外暴露两条独立的中断信号线：

### IRQ（Interrupt Request，普通中断）

外设（网卡、定时器、键盘等）触发的常规中断。Linux 绝大多数中断处理走此路径。可被 DAIF 寄存器的 I 位屏蔽。

### FIQ（Fast Interrupt Request，快速中断）

最初设计目的是提供比 IRQ 更低的响应延迟（独立寄存器组，无需保存现场）。在 AArch64 中 FIQ 的"快速"优势已基本消失，其主要价值在于提供一条与 IRQ **物理独立**的信号通道。内核利用这一特性实现伪 NMI（见第 6 节）。可被 DAIF 寄存器的 F 位屏蔽。

---

## 2. DAIF 寄存器

`DAIF` 是 AArch64 处理器状态寄存器（`PSTATE`）中的四个异常屏蔽位，每位控制一类异常。**置 1 = 屏蔽，清 0 = 允许**：

```
bit3  D — Debug exception        调试断点/单步异常
bit2  A — SError (Async abort)   异步外部中止（总线错误等）
bit1  I — IRQ                    普通中断
bit0  F — FIQ                    快速中断 / 伪 NMI
```

屏蔽存在优先级约束（屏蔽高级别会连带屏蔽低级别）：

```
屏蔽 D → 其余三类全部被屏蔽
屏蔽 A → IRQ 和 FIQ 也被屏蔽，但 D 不受影响
屏蔽 I/F → 只影响各自，互不干扰
```

这一顺序使 `entry.S` 在异常返回时能准确判断哪些异常应当重新开启。

### 操作指令

| 指令 | 作用 |
|------|------|
| `msr daifset, #N` | 将指定位置 1（屏蔽），`#3 = 0b0011` 同时屏蔽 I 和 F |
| `msr daifclr, #N` | 将指定位清 0（开启），`#3 = 0b0011` 同时开启 I 和 F |

这两条指令是专用写寄存器，比读-改-写 PSTATE 更快，对当前 CPU 立即生效，无需额外的 ISB。

---

## 3. DAIF 路径：传统关中断

不启用 `CONFIG_ARM64_PSEUDO_NMI` 时，`local_irq_disable()` 直接操作 DAIF 寄存器：

```c
barrier();
asm volatile("msr daifset, #3");  // 同时屏蔽 IRQ(I) 和 FIQ(F)
barrier();
```

两侧的 `barrier()` 是**编译器屏障**，不生成任何机器指令，但禁止编译器将临界区内外的内存访问跨越屏障重排（详见第 8 节）。

> 这里只需要编译器屏障，不需要 DSB/ISB 等硬件内存屏障，因为 `msr daifset` 对当前 CPU 立即生效，无需等待其他 CPU 或设备确认。

---

## 4. GICv3 PMR 寄存器

`ICC_PMR_EL1`（Priority Mask Register）是 GICv3 规范定义的硬件寄存器，始终存在于所有 GICv3/v4 系统中，功能单一：

> **优先级数值低于此阈值（即优先级高于阈值）的中断，GIC 才向 CPU 投递。**

> GICv3 优先级数值**越小优先级越高**。例如优先级 `0x10` 高于 `0x80`。

内核在 PMR 路径下只使用两个阈值：

| 值 | 含义 |
|----|------|
| `GIC_PRIO_IRQON` | 阈值低，普通 IRQ 和 FIQ 均可送达 CPU（开中断） |
| `GIC_PRIO_IRQOFF` | 阈值高，普通中断被 GIC 拦截（关中断） |

### 未投递的中断不会丢弃

每个中断在 GIC 内部维护状态机：

```
Inactive → Pending → Active → Active+Pending
```

PMR 拦截时，中断停在 `Pending` 状态，等 PMR 阈值降低后 GIC 会重新投递。边沿触发的中断会被 GIC 锁存，即使信号已撤销也不丢失。

---

## 5. PMR 路径：支持伪 NMI 的关中断

启用 `CONFIG_ARM64_PSEUDO_NMI` 时，`local_irq_disable()` 改为操作 `ICC_PMR_EL1`：

```c
barrier();
write_sysreg_s(GIC_PRIO_IRQOFF, SYS_ICC_PMR_EL1);  // 提高 GIC 优先级阈值
barrier();
```

与 DAIF 路径的关键区别：

- **DAIF.F（FIQ 通道）保持开启**，伪 NMI 使用的高优先级 FIQ 可以穿透 `GIC_PRIO_IRQOFF` 阈值，抢占关中断临界区
- `GIC_PRIO_IRQOFF` 只拦截普通优先级中断，不影响优先级数值更小的伪 NMI

### 开中断时必须调用 pmr_sync()

```c
write_sysreg_s(GIC_PRIO_IRQON, SYS_ICC_PMR_EL1);
pmr_sync();   // 等待 GIC 确认 PMR 写入已生效
```

开中断时若不等 GIC 确认，已积压的 Pending 中断可能被漏送。**关中断路径无需 `pmr_sync()`**：收紧阈值即使有短暂窗口 GIC 尚未感知，也不会导致额外中断提前到达。

### 运行时路径选择

由 `system_uses_irq_prio_masking()`（`arch/arm64/include/asm/cpufeature.h`）决定，该函数使用**静态分支**（alternative patching）实现：内核启动时检测到 `ARM64_HAS_GIC_PRIO_MASKING` capability 后，将代码中的 `nop` 原地 patch 为跳转指令，之后每次调用只执行一条指令，热路径开销接近零。

---

## 6. 伪 NMI

### 背景

x86 有硬件 NMI 引脚，触发后无论 CPU 是否关中断都必须响应，用于：

- **perf 性能采样**：在任意代码路径采样，包括关中断临界区
- **看门狗**：检测持有锁超时的死锁
- **硬件错误（RAS）**：机器检查错误必须立即响应

ARM64 没有硬件 NMI，`daifset #3` 一关所有中断都进不来，导致关中断临界区对 perf 不可见、死锁无法被看门狗检测。

### 实现原理

利用 GICv3 优先级机制，将特定 FIQ 配置为极高优先级，使其优先级数值低于 `GIC_PRIO_IRQOFF` 阈值，从而在 PMR 关中断时仍能穿透：

```
普通中断优先级：  0x80 ~ 0xFF
GIC_PRIO_IRQOFF   （拦截普通中断）
伪 NMI 优先级：   0x10  （数值更小 = 优先级更高，穿透 IRQOFF）
```

关中断后的中断通路：

```
local_irq_disable()（PMR 路径）
    ├── IRQ 通道：DAIF.I 屏蔽 → 所有 IRQ 进不来
    └── FIQ 通道：DAIF.F 开着 → GIC PMR 过滤
                普通 FIQ → 优先级低，被 PMR 拦截，停在 Pending
                伪 NMI   → 优先级高，穿透 PMR，到达 CPU
```

### 使用者

| 使用者 | 作用 |
|--------|------|
| perf | 在关中断临界区内采样，否则该段代码对性能分析永远不可见 |
| 看门狗 | 检测持有锁超时的死锁，关中断的死锁用普通定时器永远检测不到 |
| 硬件错误 RAS | 机器检查错误必须立即响应，不能等到开中断 |

---

## 7. 两条路径对比

| | DAIF 路径 | PMR 路径 |
|---|---|---|
| 操作对象 | PSTATE.DAIF 寄存器 | ICC_PMR_EL1 寄存器 |
| 关中断指令 | `msr daifset, #3` | `write ICC_PMR_EL1 = IRQOFF` |
| FIQ 通道 | 一并屏蔽 | 保持开启 |
| 伪 NMI 支持 | 不支持 | 支持（高优先级 FIQ 穿透） |
| 开中断需同步 | 否（MSR 立即生效） | 是（需 `pmr_sync()`） |
| 启用条件 | 默认 | `CONFIG_ARM64_PSEUDO_NMI` |
| 适用场景 | 普通系统 | 需要 perf/watchdog 穿透临界区 |

---

## 8. 编译器屏障 barrier()

```c
static inline void barrier(void)
{
    asm volatile("" : : : "memory");
}
```

拆解：

| 部分 | 含义 |
|------|------|
| `""` | 汇编模板为空，不生成任何机器指令，运行时零开销 |
| `volatile` | 禁止编译器删除或移动这条 asm |
| `"memory"` clobber | 告诉编译器此处可能读写任意内存，必须放弃所有寄存器缓存的内存值 |

**效果**：

- barrier 之前：将所有"脏"寄存器值刷回内存，不能假设任何内存值已缓存在寄存器中
- barrier 之后：后续内存读取必须重新生成 load 指令，不能复用 barrier 之前读到的旧值

**关键限制**：只约束编译器，不约束 CPU。CPU 在运行时仍可乱序执行。

```
防编译器重排  →  barrier()        （零指令）
防 CPU 乱序   →  硬件屏障          （如 PowerPC 的 sync，ARM 的 DSB）
两者都防      →  硬件屏障（自带 "memory" clobber）
```

---

## 9. 相关源文件

| 文件 | 内容 |
|------|------|
| `arch/arm64/include/asm/irqflags.h` | `arch_local_irq_disable/enable/save/restore` 完整实现 |
| `arch/arm64/include/asm/cpufeature.h` | `system_uses_irq_prio_masking()` 静态分支实现 |
| `arch/arm64/kernel/setup.c` | `smp_setup_processor_id()`，boot CPU MPIDR 初始化 |
| `arch/arm64/include/asm/smp.h` | `__cpu_logical_map[]`，逻辑 CPU 到 MPIDR 映射表 |
| `arch/powerpc/boot/io.h` | PowerPC MMIO 读写函数，`barrier()`/`sync()`/`eieio()` 实现 |
