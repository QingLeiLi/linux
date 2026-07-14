# Android 内核启动过程完整指南

## 1. 启动链总览

```
按下电源键
    ↓
PBL（片上 ROM，不可修改）
    ↓ 验证签名
XBL（初始化硬件 + 启动 TrustZone）
    ↓ 验证签名
ABL（AVB 验证 + 加载内核）        ← 解锁 bootloader 影响这层
    ↓
Linux Kernel
  head.S（汇编，开启 MMU）
    ↓
  proc.S __cpu_setup（配置 TCR/MAIR）
    ↓
  start_kernel()（C 代码入口）
    ↓
  setup_arch()（解析 DTB，建立内存布局）
    ↓
  smp_init()（启动其他 CPU 核心）
    ↓
  do_initcalls()（所有驱动和子系统初始化）
    ↓
  rest_init()
    ├── PID 1: kernel_init → exec("/init")
    ├── PID 2: kthreadd
    └── PID 0: idle（当前执行流）
    ↓
Android 用户态
  /init（First Stage → Second Stage）
    ↓
  servicemanager / surfaceflinger / zygote
    ↓
  SystemServer（80+ 服务）
    ↓
  Launcher（用户看到桌面）
```

**各阶段执行位置：**

| 阶段 | 执行位置 | 代码来源 |
|---|---|---|
| PBL | SoC 片上 ROM | 烧死在芯片里 |
| XBL/ABL | DRAM，CPU 普通世界 | eMMC 加载 |
| TrustZone | DRAM，CPU 安全世界 | XBL 加载，永久驻留 |
| head.S | DRAM，MMU 开启前后各一段 | boot 分区 |
| start_kernel 之后 | DRAM，虚拟地址空间 | system 分区 |
| /init 之后 | 用户态 EL0 | ramdisk/system 分区 |

---

## 2. 硬件启动阶段（Pre-Kernel）

### 2.1 PBL（Primary Boot Loader）

烧死在高通 SoC 片上 ROM，物理上不可修改。职责：
- 初始化最基本的时钟和总线
- 用硬件公钥验证 XBL 签名（信任根）
- 验证通过才跳转执行 XBL

**这是整个安全启动链的信任根，物理上无法绕过。**

### 2.2 XBL 与 TrustZone

XBL 基于 UEFI/EDK2 框架，做的关键事情：
- 完整初始化 DRAM
- **启动 TrustZone**（切换到 EL3 安全世界）
- 配置 TZASC（划定安全内存范围）
- 加载并验证 ABL

**TrustZone 隔离原理：**

```
每个内存访问请求在总线上携带 NS 位：
  NS=0 → 安全世界发出
  NS=1 → 普通世界发出

TZASC（挂在内存控制器前面的硬件过滤器）：
  安全内存 + NS=1 → 硬件直接拒绝（DECERR）
  安全内存 + NS=0 → 允许
  不是软件权限检查，是电路物理切断信号
```

两个世界的切换只有一个入口：EL3 的 Secure Monitor，通过 SMC 指令触发。普通世界的 Linux 内核无法绕过这个机制直接访问安全内存。

**Pixel 3 的 Titan M** 在 TrustZone 之外再加一层，即使 TrustZone 被攻破，Titan M 里的密钥也无法导出。

### 2.3 ABL 与 AVB

ABL（Android Boot Loader）执行 AVB（Android Verified Boot）验证：

```
读取 boot 分区
    ↓
验证 vbmeta 签名（链式验证）
    ↓
bootloader 已解锁 → 显示橙色警告，继续
bootloader 未解锁 → 签名失败则停止启动
```

AVB 之后，ABL 把内核镜像、ramdisk、dtb 加载到 DRAM，跳转到内核入口。

---

## 3. 内核汇编阶段

### 3.1 head.S 执行流程

**源码：** `arch/arm64/kernel/head.S`

head.S 是从物理世界到虚拟世界的过渡，整个过程只有两个关键目标：**建立页表** 和 **开启 MMU**。

```
primary_entry
  │
  ├── record_mmu_state   记录 bootloader 传入时 MMU 是否开启（存 x19）
  ├── preserve_boot_args 保存 FDT 地址到 x21，保存 x0-x3 到 boot_args[]
  ├── __pi_create_init_idmap  建立恒等映射页表
  │     （虚拟地址 == 物理地址，开启 MMU 的瞬间 PC 不会跳飞）
  ├── init_kernel_el     配置 EL1/EL2，处理字节序
  ├── __cpu_setup        配置 TCR/MAIR（见 proc.S）
  └── __primary_switch
        ├── __enable_mmu  ← MMU 开启！从这里起所有地址都是虚拟地址
        ├── __pi_early_map_kernel  建立完整内核映射
        └── __primary_switched
              ├── 设置 VBAR_EL1（异常向量表）
              ├── 保存 FDT 地址到全局变量
              └── bl start_kernel  ← 跳入 C 世界
```

**关键寄存器分配（callee-saved，跨函数调用保持不变）：**
- x19：进入时 MMU 是否开启
- x20：CPU 启动模式（EL1/EL2）
- x21：FDT 物理地址

**开启 MMU 的机制：**
```
写 SCTLR_EL1 的 M 位（bit0）= 1
    ↓
ISB 指令冲刷流水线
    ↓
之后每条指令的地址都经过 MMU 翻译
```
这是一个 bit 的切换，之前必须保证恒等映射已建立，否则下一条指令地址翻译失败直接崩溃。

### 3.2 proc.S __cpu_setup

**源码：** `arch/arm64/mm/proc.S`

在开启 MMU 之前，必须先配置好 MMU 的工作参数：

**TCR_EL1（地址翻译控制寄存器）关键字段：**

| 字段 | 含义 |
|---|---|
| T0SZ | TTBR0 管理的地址范围（用户空间，低地址） |
| T1SZ | TTBR1 管理的地址范围（内核空间，高地址） |
| TG0/TG1 | 页大小（4KB/16KB/64KB） |
| IRGN/ORGN | 页表遍历的缓存策略（内/外部 Write-Back Write-Allocate） |
| SH | 共享属性（Inner Shareable，多核缓存一致性） |
| IPS | 物理地址位数（运行时从 ID_AA64MMFR0_EL1 查询） |

**MAIR_EL1（内存属性寄存器）：**
定义 8 个内存类型槽，页表条目通过 3 位索引引用：
- 强顺序设备内存（nGnRnE）
- 普通设备内存（nGnRE）
- 普通内存（Write-Back Write-Allocate，缓存友好）
- 非缓存内存

---

## 4. 内核 C 代码启动阶段

### 4.1 start_kernel() 分批初始化

**源码：** `init/main.c`

start_kernel 是内核 C 代码的入口，调用约 50 个初始化函数，按中断状态分批：

**第一批：无任何子系统可用**
```c
set_task_stack_end_magic(&init_task)  // 设置 PID 0 栈溢出检测
smp_setup_processor_id()              // 识别 boot CPU 编号
cgroup_init_early()
local_irq_disable()                   // 显式关中断
early_boot_irqs_disabled = true
```

**第二批：中断禁用，完成必要设置**
```c
boot_cpu_init()     // 把 CPU0 加入 online/present/possible mask
setup_arch()        // ← 最重要的单个调用（见下节）
jump_label_init()   // static key 初始化
setup_command_line()
setup_per_cpu_areas()
```

**第三批：内存管理初始化**
```c
setup_log_buf(0)        // 分配 dmesg 缓冲区
vfs_caches_init_early() // dcache/inode 哈希表（需要大量内存）
mm_core_init()          // buddy system + slab 就绪，kmalloc 可用
```

**第四批：调度器初始化**
```c
sched_init()        // CFS/RT/DL 调度器，必须在中断启动前
rcu_init()          // RCU，几乎所有子系统依赖
workqueue_init_early()
```

**第五批：中断系统初始化**
```c
init_IRQ()          // GIC 中断控制器初始化
timers_init()
hrtimers_init()
timekeeping_init()
time_init()         // 时钟中断就绪
// ── 关键时刻 ──
local_irq_enable()  // 第一次开启中断
```

**第六批：各子系统完整初始化**
```c
fork_init()         // task_struct slab 缓存，进程创建就绪
security_init()     // SELinux 策略加载
vfs_caches_init()   // VFS 完整初始化
cgroup_init()       // 容器基础
rest_init()         // 创建 PID 1/2，自身变 idle
```

### 4.2 setup_arch() 详解

**源码：** `arch/arm64/kernel/setup.c`，第 281 行

setup_arch 是 start_kernel 里最复杂的单个调用，ARM64 特有的硬件初始化全在这里：

```
setup_arch()
  │
  ├── early_fixmap_init()      建立 fixmap（固定虚拟地址，早期 IO 访问用）
  ├── setup_machine_fdt()      解析 DTB
  │     ├── 验证 DTB 魔数
  │     ├── 扫描 /memory 节点 → 注册内存范围到 memblock
  │     ├── 扫描 /chosen 节点 → 提取命令行、initrd 地址
  │     └── 预留 DTB 自身内存
  ├── arm64_memblock_init()    内存布局最终确定
  │     ├── 预留内核镜像区域
  │     ├── 预留 initrd 区域
  │     └── 处理 nomap 内存（不建立线性映射）
  ├── paging_init()            建立最终内核页表
  │     ├── 替换 head.S 的临时页表
  │     ├── 建立所有物理内存的线性映射（PAGE_OFFSET 起）
  │     └── 内核虚拟地址空间最终形态确立
  ├── acpi_table_upgrade()     ACPI 支持（服务器场景）
  └── request_standard_resources()  注册内核内存区域到资源树
```

**内存管理三级演化：**

```
memblock（最早期）
  → 只能分配不能释放
  → setup_arch 阶段使用

buddy system（mm_core_init 之后）
  → 正式的页级分配器
  → 物理内存按 2^n 页管理

slab/slub（kmem_cache_init 之后）
  → 小对象分配器
  → kmalloc() 的底层实现
```

### 4.3 rest_init() 与三个原始进程

```c
rest_init()
  │
  ├── user_mode_thread(kernel_init) → PID 1（阻塞等待 kthreadd 就绪）
  ├── kernel_thread(kthreadd)       → PID 2（内核线程守护进程）
  └── cpu_startup_entry()           → PID 0 变成 idle 进程
```

**PID 0（idle）：** 无任务时执行 WFI 指令让 CPU 休眠，节省电量。

**PID 1（kernel_init）：** 等 kthreadd 就绪后，调用 kernel_init_freeable 完成剩余初始化，最终 exec("/init") 变成 Android 的 init 进程。

**PID 2（kthreadd）：** 所有内核线程的父进程，内核代码通过它创建新线程。

---

## 5. SMP 多核启动

**源码：** `kernel/smp.c`，`kernel/cpu.c`

### 5.1 为什么只有 CPU0 先启动

降低启动复杂性。如果所有核心同时启动：
- 会竞争写内存管理数据结构
- 调度器、中断控制器未就绪就有多核运行
- 调试极其困难

### 5.2 CPU 热插拔状态机

每个 CPU 有约 30 个中间状态，从 OFFLINE 到 ONLINE：

```
关键状态节点：
  CPUHP_BRINGUP_CPU   ← BP（boot CPU）推进到这里，发 IPI 唤醒 AP
  CPUHP_AP_ONLINE_IDLE ← AP 完成低级初始化，等待 BP 放行
  CPUHP_ONLINE        ← 完全就绪，进入调度器
```

**BP/AP 分工：**
- BP 负责推进到 CPUHP_BRINGUP_CPU（含发送 IPI）
- AP 的热插拔线程（cpuhp/N）负责剩余状态

AP 启动入口在 head.S 的 `secondary_holding_pen`，在那里自旋等待，BP 向 `secondary_holding_pen_release` 写入对应 CPU ID 来释放它。

### 5.3 IPI（处理器间中断）机制

```
发送方：
  smp_call_function_single(cpu, func, info, wait)
    ↓
  CSD（call_single_data）= {func, info, flags}
    ↓
  入队到目标 CPU 的 call_single_queue（无锁链表）
    ↓
  arch_send_call_function_single_ipi()  发送硬件 IPI

接收方（IPI 处理函数）：
  generic_smp_call_function_single_interrupt()
    ↓
  从 call_single_queue 取出 CSD
    ↓
  执行 func(info)
    ↓
  同步模式：通知发送方完成
```

`call_single_queue` 使用无锁链表（llist），入队不需要关中断，专为高频 IPI 场景设计。

---

## 6. 进程体系与 Zygote

### 6.1 copy_process() 六个阶段

**源码：** `kernel/fork.c`，通过 kernel_clone() 调用

```
kernel_clone(clone_flags)
    ↓
copy_process()
  │
  ├── 阶段1：安全检查
  │     security_task_create()    SELinux 检查是否允许 fork
  │     copy_creds()              复制权限凭证（uid/gid/capabilities）
  │
  ├── 阶段2：分配资源
  │     dup_task_struct()         分配新 task_struct 和内核栈
  │     copy_files()              复制文件描述符表
  │     copy_fs()                 复制文件系统信息
  │     copy_sighand()            复制信号处理器
  │
  ├── 阶段3：内存空间
  │     copy_mm()                 建立新的地址空间（COW）
  │       CLONE_VM=1 → 共享内存（线程）
  │       CLONE_VM=0 → 独立地址空间（fork，COW 懒拷贝）
  │
  ├── 阶段4：命名空间
  │     copy_namespaces()         复制/共享 7 种命名空间
  │
  ├── 阶段5：CPU 状态
  │     copy_thread()             复制寄存器状态
  │       子进程 x0=0（fork 返回 0 的原因）
  │     alloc_pid()               分配新 PID
  │
  └── 阶段6：安全标签
        security_task_alloc()     LSM 分配安全标签（SELinux domain）
```

### 6.2 COW 写时复制

fork 后父子进程共享物理内存页，页表项标记为只读（Writable=0）：

```
子进程写某个内存地址
    ↓
写操作触发页异常（Writable=0）
    ↓
内核分配新物理页，复制内容
    ↓
更新子进程页表，Writable=1
    ↓
重新执行写操作
```

Zygote 预加载了约 4000 个类，fork App 时所有 App 共享这份内存，仅在各自修改时才触发复制——这是 Android 启动速度快的关键原因。

**历史安全漏洞：** CVE-2016-5195（Dirty COW）就利用了 COW 机制中的竞态条件。

### 6.3 Zygote 启动流程

```
/system/bin/app_process → ZygoteInit.main()
  │
  ├── 启动 ART 虚拟机
  │     初始化 GC、JIT 编译器、堆大小限制
  │
  ├── 预加载（最耗时）
  │     preloadClasses()    ~4000 个常用类
  │     preloadResources()  系统 drawable/color
  │     preloadOpenGL()     EGL 上下文
  │
  ├── fork system_server
  │
  └── 进入 socket 监听循环
        等待 AMS 发来 fork 新 App 的请求
```

---

## 7. Android 核心子系统

### 7.1 Binder IPC

**源码：** `drivers/android/binder.c`

**为什么不用 Linux 原有 IPC：**

| IPC 方式 | 拷贝次数 | 问题 |
|---|---|---|
| 管道/socket | 2次 | 性能差 |
| 共享内存 | 0次 | 需要额外同步机制，复杂 |
| **Binder** | **1次** | 兼顾性能和易用性 |

**1.5 次拷贝原理：**

```
接收方提前 mmap 一块内核内存：
  内核虚拟地址 → 物理页
  接收方用户虚拟地址 → 同一批物理页

发送方调用 BC_TRANSACTION：
  copy_from_user(内核缓冲区, 发送方用户空间, 数据大小)
    ↓ 1次拷贝
  数据已在接收方可见的物理页中

接收方 BR_TRANSACTION 唤醒后：
  直接读自己用户空间地址 → 0次额外拷贝
```

**核心数据结构：**
- `binder_proc`：一个进程的 Binder 状态
- `binder_thread`：一个线程的状态
- `binder_node`：一个服务对象（服务端）
- `binder_ref`：对某个 node 的引用（客户端持有）
- `binder_transaction`：一次 IPC 调用

**所有操作通过 ioctl 进行：**

| ioctl 命令 | 用途 |
|---|---|
| BINDER_WRITE_READ | 核心命令，写入请求/读取回复 |
| BINDER_SET_CONTEXT_MGR | 注册为 servicemanager |
| BINDER_SET_MAX_THREADS | 设置线程池大小 |
| BINDER_FREEZE | 冻结进程的 Binder 通信 |

### 7.2 SELinux

**源码：** `security/selinux/hooks.c`

**MAC vs DAC：**
- DAC（自主访问控制）：文件权限位（rwx），所有者可自由修改
- MAC（强制访问控制）：策略由系统管理员定义，进程无法绕过

**Android 安全标签格式：**
```
user:role:type:sensitivity
例：u:r:untrusted_app:s0:c512,c768
```

**访问决策：**
```
主体（进程 domain）+ 操作 + 客体（文件/进程 type）
→ 查 allow 规则
→ 允许或拒绝（并记录 AVC denial 到 dmesg）
```

**AVC 三级缓存（性能关键）：**
```
快速路径（per-task cache）→ task-level avd cache → 全局 AVC → 策略数据库
```
绝大多数访问在前两级命中，避免每次都查策略数据库。

**Binder 专属 SELinux 钩子（Android 独有）：**

```c
selinux_binder_transaction(caller, target)
  // 检查 caller domain 是否有权调用 target domain
  // 即使 Binder 连接建立了，这里仍可以逐次拒绝具体调用

selinux_binder_transfer_file(caller, target, file)
  // 通过 Binder 传递 fd 时检查文件权限
```

**ptrace 防护（防逆向）：**
```c
selinux_ptrace_access_check(tracer, tracee)
  // Android 非 debuggable App 的 domain 不允许被 attach
  // 即使 root 也受 SELinux 约束
```

### 7.3 Android Init

init 是内核 exec 的第一个用户态进程（/init），分两阶段：

**First Stage Init（在 ramdisk 中）：**
```
挂载 /proc /sys /dev
加载 SELinux 策略（最重要的安全操作）
挂载 system/vendor 分区
exec 切换到 Second Stage Init
```

**Second Stage Init：**
```
解析所有 *.rc 文件（/system/etc/init/ 等）
按 trigger 顺序执行：
  early-init → init → late-init → boot
```

**关键服务启动顺序：**
```
servicemanager   ← Binder 的"电话总机"，必须最先启动
hwservicemanager ← HAL 服务注册中心
surfaceflinger   ← 显示合成，启动后屏幕亮起
bootanim         ← 开机动画
zygote           ← Java 世界起点
```

---

## 8. Android 安全边界总结

```
层次          安全机制              攻击面
─────────────────────────────────────────────────────
应用层        签名验证              APK 解析漏洞
              权限模型              权限提升
              WebView 沙箱          JS 引擎漏洞

框架层        SELinux（用户态）     策略配置错误
              Binder IPC 权限       Binder 接口漏洞
              组件权限              Intent 注入

内核层        SELinux（内核）       内核漏洞提权
              seccomp 过滤          系统调用漏洞
              命名空间隔离          容器逃逸
              Binder 驱动           binder.c 内存漏洞

硬件层        TrustZone             TEE 漏洞（难度最高）
              AVB                   bootloader 解锁
              Titan M               物理攻击
```

**从 App 到 root 的典型提权路径：**
```
App 代码漏洞（JNI/native）
    ↓
绕过 seccomp 或找到允许的系统调用漏洞
    ↓
内核漏洞（内存破坏、UAF、整数溢出）
    ↓
获得内核代码执行
    ↓
绕过 SELinux（修改 task->cred 或 security 指针）
    ↓
root 权限
```

**每层能做什么、不能做什么：**

| 层次 | 普通 App | root | 内核代码执行 |
|---|---|---|---|
| 读其他 App 数据 | 不能（SELinux） | 不能（SELinux） | 能 |
| ptrace 其他 App | 不能（SELinux） | 不能（SELinux） | 能 |
| 修改系统分区 | 不能 | 不能（只读挂载） | 能（重新挂载） |
| 访问 TEE | 只能通过 TA | 只能通过 TA | SMC 调用（受限） |

---

## 9. 延伸学习路径

### 9.1 内核漏洞研究（按历史漏洞密度排序）

**高优先级子系统：**

| 子系统 | 历史漏洞类型 | 关键文件 |
|---|---|---|
| Binder 驱动 | UAF、整数溢出 | `drivers/android/binder.c` |
| ion/dmabuf 内存 | 条件竞争、越界 | `drivers/dma-buf/` |
| 内核内存管理（mm） | UAF、double-free | `mm/page_alloc.c`, `mm/slab.c` |
| 网络协议栈 | 越界读写 | `net/` |
| GPU 驱动 | 厂商定制，漏洞密集 | 高通/Mali 驱动 |

**CVE 复现入门路径：**
```
1. 找 Android 安全公告（monthly bulletin）
2. 搜索对应 CVE 的 patch commit
3. 读 diff 理解漏洞原理
4. 在对应版本内核上复现 PoC
5. 理解利用原语（arbitrary read/write → privilege escalation）
```

**推荐工具：**
- `cs.android.com` — AOSP 代码搜索，支持历史版本对比
- `android.googlesource.com/kernel/common` — Android 内核源码

### 9.2 动态分析工具链

**Frida 能力边界：**

| 层次 | Frida 能力 | 限制 |
|---|---|---|
| Java 层 | Hook 任意方法，修改返回值 | debuggable App 或 root |
| Native（SO）层 | Interceptor，内存读写 | 需要 root + frida-server |
| 系统服务 | Hook SystemServer 方法 | 需要 root |
| 内核层 | 不能直接 hook | 需要内核模块或 GDB |

**内核调试（需要解锁 bootloader + 自编译内核）：**
```
编译开启 KASAN/KCOV 的内核
    ↓
刷入 Pixel 设备
    ↓
USB 连接 + GDB stub
    ↓
可以设断点、检查内核内存
```

**Pixel 3 的价值：** 是 AOSP 官方支持的设备，驱动完整，可以刷自编译内核，是学习 Android 内核安全的最佳真机平台。

### 9.3 关键源码文件索引（已添加注释）

**启动链：**
```
arch/arm64/kernel/head.S       汇编入口，MMU 开启
arch/arm64/mm/proc.S           CPU 配置（TCR/MAIR）
arch/arm64/kernel/setup.c      setup_arch，DTB 解析
init/main.c                    start_kernel，整个内核初始化
kernel/smp.c                   SMP 初始化，IPI 机制
kernel/cpu.c                   CPU 热插拔状态机
kernel/fork.c                  进程创建，copy_process
```

**Android 安全核心：**
```
drivers/android/binder.c       Binder IPC 驱动
security/selinux/hooks.c       SELinux 钩子（含 Binder 专属）
```

**建议的阅读顺序（从整体到细节）：**
```
1. init/main.c       建立整体框架
2. arch/arm64/kernel/setup.c  理解硬件发现
3. kernel/fork.c     理解进程模型
4. drivers/android/binder.c  理解 Android IPC
5. security/selinux/hooks.c  理解 Android 安全模型
```

### 9.4 还未覆盖的重要模块

以下模块尚未添加注释，对 Android 安全研究同样重要：

| 文件 | 重要性 | 说明 |
|---|---|---|
| `kernel/seccomp.c` | ★★★★ | App 沙箱最后防线，系统调用过滤 |
| `mm/mmap.c` | ★★★★ | 内存映射，历史漏洞最集中 |
| `drivers/android/binder_alloc.c` | ★★★★ | Binder 内存分配，独立模块 |
| `mm/page_alloc.c` | ★★★ | buddy system，页分配器 |
| `kernel/sched/core.c` | ★★★ | 调度器，理解进程调度 |
| `net/socket.c` | ★★★ | 网络 IPC，VPN/防火墙基础 |

---

## 10. Linux 内核子系统地图

整个 Linux 内核由五大子系统构成，理解这张地图是读懂任何内核代码的前提。

### 10.1 五大子系统

```
用户程序
    ↓ syscall 接口（内核边界）
┌─────────────────────────────────────────┐
│  VFS（虚拟文件系统）                     │
│  网络协议栈                              │
│  内存管理                                │
│  进程调度                                │
│  设备模型                                │
└─────────────────────────────────────────┘
    ↓
硬件
```

每个子系统都遵循同一个设计模式：
```
子系统 = 抽象接口层 + 注册机制 + N 个具体实现
```

**VFS（Virtual File System）：**
- "Virtual" 的含义：它本身不存储任何数据，是一层抽象接口
- 所有具体文件系统（ext4/tmpfs/procfs）和设备驱动都实现同一套 `file_operations`
- 用户程序永远只和 VFS 对话，不需要知道底下是什么
- Android 的 Binder 驱动（`/dev/binder`）也是通过实现 `file_operations` 挂在 VFS 下的

```c
// 驱动只需填充这个结构体
static struct file_operations binder_fops = {
    .open    = binder_open,
    .mmap    = binder_mmap,
    .ioctl   = binder_ioctl,
    .release = binder_release,
};
```

**网络协议栈：**
- Socket fd 借用 VFS 接口（`read/write/close`），但网络协议栈本身是独立子系统
- 接缝点：`sock->ops->recvmsg` 从 VFS 世界跳入网络世界
- TCP/IP 完全复用，Android 在上面加了 Paranoid Networking（按 uid 控制网络权限）

**内存管理（最重要的子系统之一）：**
```
关键数据结构：
  mm_struct      ← 一个进程的完整内存布局
  vm_area_struct ← 地址空间中的一段区域（VMA）
  page           ← 一个物理页的描述符

分配器层次：
  buddy system → 页级分配（2^n 页）
  slab/slub    → 小对象（kmalloc）
  vmalloc      → 不连续物理内存的虚拟连续映射
```

**进程调度：**
```
sched_class（调度策略的抽象接口）
  stop_sched_class    ← 最高优先级，迁移线程
  dl_sched_class      ← Deadline 调度
  rt_sched_class      ← 实时调度（FIFO/RR）
  fair_sched_class    ← CFS 完全公平调度（普通进程）
  idle_sched_class    ← idle 进程
```

**设备模型：**
- 所有硬件在内核中都表示为 `struct device`
- 通过 `bus_type / device_driver` 抽象与硬件无关的驱动框架
- sysfs（`/sys`）是设备模型的镜像，暴露给用户空间

### 10.2 Android 在五大子系统上加了什么

Android 内核本质是在这五个子系统的钩子上挂了自己的实现：

| 子系统 | Android 新增 | 说明 |
|---|---|---|
| VFS | Binder 驱动（/dev/binder） | Android 专有 IPC |
| VFS | ashmem/memfd | 匿名共享内存 |
| 内存管理 | ion/dmabuf | 跨进程零拷贝共享内存 |
| 进程调度 | Wakelocks | 阻止系统休眠 |
| 进程调度 | Low Memory Killer（LMK） | 按优先级杀进程 |
| 网络协议栈 | Paranoid Networking | 按 uid 控制网络权限 |
| 安全 | SELinux 强化策略 | 更严格的默认策略 |
| 安全 | Cgroups 扩展 | 电源/内存分组管理 |
| 虚拟化 | pKVM | 保护型虚拟机（Pixel 6+） |

Android 替换 Linux 原有实现只有两个原因：
1. **性能不够** → Binder 替换 IPC（减少拷贝次数）
2. **移动场景特有需求** → Wakelocks/LMK（电池和内存限制）

### 10.3 VFS 抽象层的价值

理解 VFS 抽象层，后续看任何驱动代码都有了坐标：

```
看到一个 CVE：先定位在哪个子系统
看到一段代码：先判断是抽象层还是具体实现
遇到不懂的结构体：找它属于哪个子系统的抽象层

抽象层 = 地图
具体实现 = 地图上的某个地点
```

---

## 11. 硬件基础：MMU、TLB、DMA、CPU 流水线

这些硬件概念是理解内核内存管理和安全机制的基础。

### 11.1 MMU 在哪里

**MMU 在 CPU 内部，不在内存控制器。**

```
CPU 芯片内部：
┌──────────────────────────────┐
│  执行单元                     │
│    ↓ 虚拟地址                 │
│  MMU（地址翻译）              │
│    ↓ 物理地址                 │
│  Cache（L1/L2/L3）           │
└──────────────────────────────┘
         ↓ 物理地址（Cache Miss 时）
    内存总线
         ↓
    内存控制器（独立或集成在 SoC）
         ↓
    物理内存（DRAM）
```

MMU 职责：虚拟地址 → 物理地址的翻译 + 权限检查
内存控制器职责：控制 DRAM 时序、多通道调度

**MMU 开启前的状态：**
MMU 是物理上串联在路径里的，没有绕过它的物理路径。
- M=0（禁用）：地址原样输出，虚拟地址 == 物理地址
- M=1（开启）：激活翻译逻辑，经过页表转换

这就是 head.S 必须建立恒等映射才能开启 MMU 的原因——开启前后地址相同，切换瞬间 PC 不会跳飞。

### 11.2 页表映射

**问题起源：** 多进程同时运行，如果直接用物理地址会互相覆盖。

**解决方案：** 每个进程有独立的虚拟地址空间，页表是虚拟地址到物理地址的翻译字典。

```
进程A：虚拟地址 0x1000 → 物理地址 0x5000
进程B：虚拟地址 0x1000 → 物理地址 0x8000
同一虚拟地址，映射到不同物理地址，互不干扰
```

**ARM64 四级页表结构（Pixel 3 使用）：**
```
虚拟地址（48位）：
┌────┬──────┬──────┬──────┬──────┬──────────────┐
│空  │L0索引 │L1索引 │L2索引 │L3索引 │  页内偏移    │
│16位│ 9位  │ 9位  │ 9位  │ 9位  │   12位       │
└────┴──────┴──────┴──────┴──────┴──────────────┘
每级索引 9 位 = 每张页表 512 个条目（512 × 8字节 = 4KB = 一页，页对齐）
```

**页表条目的额外能力（标志位）：**
```
Present=0  → 访问触发缺页异常 → 实现按需分配/swap
Writable=0 → 写入触发异常   → 实现 COW（写时复制）
User=0     → 用户态访问异常  → 实现内核空间保护
```

**TTBR0 vs TTBR1：**
ARM64 把虚拟地址空间从中间一分为二：
- 低地址（0x0000...）→ 用户空间 → TTBR0 指向的页表管理（进程切换时更新）
- 高地址（0xFFFF...）→ 内核空间 → TTBR1 指向的页表管理（全局唯一，不切换）

进程切换时只换 TTBR0，TTBR1 永远不变——内核空间对所有进程相同。

### 11.3 TLB 与 ASID

**TLB（Translation Lookaside Buffer）：** 页表的硬件缓存，在 MMU 内部。

```
MMU 收到虚拟地址
    ↓
查 TLB（SRAM，CAM 结构，全并行比较）
    ↓ 命中（1-4 个时钟周期）
直接输出物理地址

    ↓ 未命中（TLB Miss）
Page Table Walker（硬件电路）遍历内存里的页表
约 400-800 个时钟周期
    ↓
结果填入 TLB，下次命中
```

**TLB 为什么这么快：** 使用 CAM（内容可寻址内存）结构，所有条目同时并行比较，不是顺序查找。

**ASID（Address Space ID）：** 解决进程切换时 TLB 失效的性能问题。

没有 ASID 时：进程切换必须全部刷新 TLB，下一个进程所有访问都是 Miss，性能灾难。

有 ASID 时：
```
TLB 条目格式：[ASID | 虚拟地址 | 物理地址 + 属性]
进程切换 → 只换 TTBR0 + 当前 ASID，不刷 TLB
不同进程的 TLB 条目共存，靠 ASID 区分
```

**TLBI 指令（TLB Invalidate）：**
正常查询全是硬件，但两种情况软件必须主动操作 TLB：
```
TLBI VAE1, x0      → 只清某个虚拟地址的条目（修改页表时）
TLBI ASIDE1, x0    → 清某个 ASID 的所有条目（进程退出时）
TLBI ALLE1         → 清全部条目（极少用）
```

### 11.4 DMA（Direct Memory Access）

**没有 DMA 时：** CPU 收到中断，亲自把数据从设备寄存器搬到内存，期间无法处理其他任务。

**有 DMA 时：**
```
CPU 告诉 DMA 控制器：
  "把设备地址 0x... 的数据搬到内存地址 0x...，长度 1024 字节"
    ↓
CPU 去干别的事（并行）
    ↓
DMA 控制器独立操作内存总线
    ↓
搬完发中断通知 CPU
```

DMA 绕过 Cache 直接写内存，会导致**缓存一致性问题**：CPU 的 Cache 里可能还是旧数据。解决方式：
- 软件：DMA 前 flush，DMA 后 invalidate
- 硬件：Cache Coherent DMA（ARM CCI/CCN 总线互联，手机 SoC 普遍支持）

**DMA 与内存总线争用（Memory Bus Contention）：**
CPU 和 DMA 同时访问内存时会竞争总线。手机 SoC 的缓解机制：
1. CPU Cache（减少实际到达内存总线的请求）
2. 总线仲裁器（按优先级分配访问顺序）
3. 多通道/多 bank 内存（物理上增加带宽）

**拍 4K 视频时 CPU 发热**不只是计算压力——Camera ISP、GPU、视频编码器同时做 DMA，内存总线接近饱和也会导致功耗上升。

### 11.5 零拷贝原理

**传统文件发网络的 4 次拷贝：**
```
磁盘 →(DMA)→ 内核 page cache →(CPU)→ 用户 buffer →(CPU)→ socket buffer →(DMA)→ 网卡
```

**sendfile 零拷贝（Linux 2.4+，网卡支持 SG-DMA）：**
```
磁盘 →(DMA)→ 内核 page cache
                ↓ 只传描述符（在哪、多长）
              socket buffer
                ↓ SG-DMA 直接从 page cache 读
              网卡
```
CPU 参与次数：0。这就是"零拷贝"——用页表映射和 DMA 描述符替代 CPU 搬运数据。

**Binder 是 1.5 次拷贝（不是零拷贝，但比 2 次好）：**
```
接收方提前 mmap：内核 buffer ↔ 接收方用户空间（同一物理页）
发送方 copy_from_user：发送方用户空间 → 内核 buffer（1 次）
接收方直接读自己用户空间（0 次额外拷贝）
```

**ion/dmabuf 才是真正的零拷贝：**
```
Camera ISP → DMA 写入物理页
GPU → 页表映射到同一物理页，直接读
SurfaceFlinger → 页表映射到同一物理页，直接合成
视频编码器 → 页表映射到同一物理页，直接压缩

整个图像处理链路，数据从未被 CPU 拷贝过
各模块通过 dmabuf fd 共享同一块物理内存
```

### 11.6 CPU 流水线与 ISB

**CPU 流水线：** 现代 CPU 不是执行完一条指令再取下一条，而是多条指令同时在不同阶段处理：
```
时钟周期：  1    2    3    4    5
指令A：    取指  译码  执行  访存  写回
指令B：         取指  译码  执行  访存
指令C：               取指  译码  执行
```

**写寄存器如何影响流水线：**
```
软件写 SCTLR_EL1.M=1（开启 MMU）
    ↓ 这只是"扣动扳机"
硬件把寄存器值变化传递给 MMU 电路（电信号传递）
    ↓
MMU 硬件激活
```
这是纯硬件行为，软件只是发出信号，之后一切由电路自动完成。

**ISB 的作用：**
写完 SCTLR_EL1 后，流水线里已经有后续指令在运行，这些指令是按 MMU 开启前的规则取进来的。

```asm
msr sctlr_el1, x0   // 写寄存器，MMU 开启
isb                  // Instruction Synchronization Barrier
```

ISB 的硬件过程：
1. 停止取指单元接收新指令
2. 等待流水线里已有的指令全部完成
3. 丢弃 ISB 之后已经取进来但按旧状态执行的指令
4. 重新从当前 PC 取指，此时 MMU 已完全生效

**ISB 不是软件逻辑，是一条发给硬件流水线控制器的命令**，流水线控制器是一个状态机（数字电路实现），收到 ISB 信号后自动执行排空和重启流程。

### 11.7 CPU 的本质

```
CPU = 100 亿个晶体管
    = 几十亿个逻辑门（AND/OR/NOT/XNOR）
    = 几亿个基本功能单元（比较器/加法器/MUX/触发器）
    = 几千个功能模块（ALU/寄存器文件/Cache/MMU/状态机）
    = 靠时钟信号协调的超大规模数字电路
```

两类电路：
- **组合逻辑**（没有记忆）：输入变了输出立刻变，如比较器、MUX
- **时序逻辑**（有记忆）：只在时钟上升沿更新状态，如寄存器、SRAM

TLB 的 CAM 查找就是：N 个条目各自有一套比较器，输入的虚拟地址**同时**和所有条目比较，匹配信号通过 MUX 选出对应的物理地址。并行比较，几个纳秒，全程无软件参与。

---

## 12. Android 内核与 Linux 主线的关系

### 12.1 演变历史

**2019 年之前：完全碎片化的 Fork**
```
Linux mainline
    ↓ fork
AOSP Common Kernel（Google 改）
    ↓ 各自 fork
高通 BSP 内核 / 联发科 BSP 内核 / 三星 BSP 内核
    ↓ 各自魔改
小米定制 / OPPO 定制 / ...
```
每个厂商自己维护，安全补丁严重滞后，有些设备落后 mainline 数年。

**2019 年之后：GKI（Generic Kernel Image）**
```
Linux mainline
    ↓ 定期 merge
AOSP Common Kernel（android-mainline）
    ↓ 稳定后切出
GKI 发布版（如 android13-5.15）
    ↓
厂商只能加 Vendor Module（内核模块），不能改内核本体
```

**核心变化：** 厂商代码必须以独立的内核模块形式存在，不能直接修改内核源码。

### 12.2 android-mainline 的集成机制

```
Google 维护 android-mainline 分支，持续从 Linux mainline merge：

git merge v6.8   # 从 Linux tag merge
                 # 合并后解决冲突（Android 专有代码 vs Linux 新代码）
    ↓
稳定后切出 LTS 分支：android14-6.1、android15-6.6 等
    ↓
安全补丁通过 backport 机制打入各 LTS 分支
```

**Android 内核与 Linux 的实际差异（GKI 后大幅收窄）：**
- Binder 驱动已于 2015 年 merge 进 Linux mainline
- 主要剩：Wakelocks、LMK、Paranoid Networking、部分安全增强

学 android13-5.15 = 学 Linux 5.15 LTS + 少量 Android 专有补丁，不是两套体系。

### 12.3 Backport 机制

**Backport：** 把新版本的 fix 移植回老版本。

```
Linux mainline（6.8）修复一个漏洞（基于 6.8 的代码结构）
    ↓
Android 用的是 5.15 LTS，代码结构不同，patch 不能直接用
    ↓
手动把这个 fix 的意图重新适配到 5.15 的代码结构
    ↓
这个过程叫 backport
```

为什么不直接升级内核版本？升级意味着所有驱动重新适配、ABI 变化导致模块不兼容，成本极高。

**Backport 引入新漏洞：** 移植时理解有误，或者上下文不同，会产生新的 bug。这是 Android 内核漏洞的重要来源之一。

**漏洞挖掘思路：**
```
找 Linux mainline 的 fix commit
    ↓
对比 Android LTS 分支的 backport
    ↓
如果 backport 有偏差 → 可能是新漏洞
```

### 12.4 Linux 虚拟化与 Android

Linux 内核的虚拟化机制，Android 都有使用：

**KVM → pKVM（Android 特化）：**
- 普通 KVM：Host 内核可以访问 Guest 内存
- pKVM（Pixel 6+）：Host 内核也无法访问 Guest 内存，用来保护 TEE 和敏感计算
- Android 虚拟化框架（AVF）运行 Microdroid（最小化虚拟机）

**Namespaces：**
Android 使用但比容器保守：主要用 mnt_ns 和 user_ns 实现 Scoped Storage（分区存储）。没有像容器那样全量使用，因为 Binder 跨 namespace 很麻烦。

**seccomp：**
```
Android 8+ 强制要求每个 App 进程启用 seccomp 过滤
只允许约 150 个系统调用（Linux 有 300+）
目的：即使沙箱被突破，也限制能执行的内核攻击面
```

这是从 App 提权的最后一道防线：即使有内核漏洞，也必须是 seccomp 允许的系统调用才能触发。

---

## 13. 驱动模型与字符设备

### 13.1 字符设备的本质

Linux 的核心哲学：**一切皆文件**。

```
用户程序 open("/dev/binder")
    ↓ 和读普通文件的系统调用完全一样
VFS
    ↓ 根据主设备号查驱动注册表
调用 Binder 驱动的 open() 函数
```

设备对用户程序来说就是一个文件，读写文件就是操作设备。

**驱动本质就是填充 file_operations：**
```c
static struct file_operations my_fops = {
    .open    = my_open,
    .release = my_close,
    .read    = my_read,
    .write   = my_write,
    .ioctl   = my_ioctl,  // 特殊操作的万能后门
};
// 注册：register_chrdev(240, "mydev", &my_fops);
```

### 13.2 MMIO（Memory-Mapped I/O）

硬件寄存器被映射到物理地址，驱动通过读写这些地址控制硬件：

```c
void __iomem *base = ioremap(0xFF010000, 0x1000);  // 映射硬件寄存器
writel(0x0C, base + UART_BAUD_REG);   // 写地址 = 设置串口波特率
writel('A', base + UART_TX_REG);      // 写地址 = 发送一个字节
```

**没有物理硬件的驱动（虚拟设备）不需要 MMIO：**
- `/dev/null`：write 丢弃，read 返回 EOF，实现只有几行代码
- `/dev/binder`：完全在内核数据结构中操作，没有任何 MMIO
- `/dev/ashmem`：匿名共享内存，纯内核数据结构

**完整调用链：**
```
用户程序 write(fd, data)
    ↓ 系统调用
VFS 根据 fd 找到 file_operations
    ↓
驱动的 my_write()
    ↓（有物理硬件时）
MMIO 写入硬件寄存器
    ↓
电信号传到设备
```

### 13.3 Android 的 CPU 热插拔与驱动

Android 的驱动初始化通过 initcall 机制批量调用：

```c
// 驱动注册自己的初始化函数（编译时放入特殊 ELF 段）
device_initcall(binder_init);   // Binder 驱动
module_init(xxx_init);          // 等价于 device_initcall

// 内核按 level 0→7 顺序调用所有注册函数
// do_initcalls() 遍历这些段，逐个调用
```

这就是为什么添加一个驱动只需要加一行宏，不需要修改任何启动流程代码。

---

## 14. Android 生态与操作系统竞争

### 14.1 新操作系统为何难以成功

Android 和 iOS 各自形成了**双边网络效应飞轮**：
```
用户多 → 开发者愿意来 → App 多 → 用户更多 → ...
```

后来者面对的死锁：没有用户 → 开发者不来 → 没有 App → 用户不来。

历史上的失败案例：

| 系统 | 技术水平 | 死因 |
|---|---|---|
| Windows Phone | 优秀 | App 生态始终追不上，开发者不来 |
| Firefox OS | 设计理念先进 | 硬件太弱，Web App 性能差 |
| Tizen | 三星自研 | 三星自己不敢在旗舰机上用 |
| Sailfish OS | 交互流畅 | 市场太小，融不到足够的钱 |
| webOS | 创意优秀 | HP 战略摇摆，开发者失去信心 |

**技术上更好也没用**——用户换系统的迁移成本（重新购买 App、习惯改变、数据迁移）远大于"更好用一点"的收益。

**唯一的突破路径：** 绑定用户无法拒绝的场景。HarmonyOS 靠华为出货量强制导入，是目前唯一还在挑战格局的变量。

### 14.2 Fuchsia OS：正确但难以落地

Google 2016 年悄悄出现在 GitHub，没有任何公告。

**技术架构（彻底革新）：**
```
Android/Linux：App → Framework → Linux Kernel（宏内核）

Fuchsia：App → Flutter → Framework → Zircon（微内核）
```

**先进设计：**

| 特性 | Linux | Fuchsia |
|---|---|---|
| 权限模型 | 基于身份（uid/gid） | 基于能力（Handle，无 Handle = 资源不存在） |
| 驱动位置 | 内核态（崩溃影响整个系统） | 用户态（崩溃只是进程退出，可自动重启） |
| IPC 方式 | 多种机制混用 | 统一 FIDL（机器可读接口） |
| 内存安全 | 依赖开发者 | Rust 大量使用 |
| 历史包袱 | 极重（POSIX 兼容） | 无（完全不兼容 POSIX） |

**Capability-Based Security（能力安全模型）的精妙之处：**
```
Linux：有权限就能访问这类所有资源（全局）
Fuchsia：持有 Handle 才能访问那个特定资源（Handle 只能由父进程传递）

类比：
Linux = 有钥匙就能开这栋楼所有同类型的门
Fuchsia = 每把锁有独立钥匙，你只有父进程给你的那把
```

**为什么没声音：**
- 2021 年正式发布，但只用在 Nest Hub 智能屏幕（没有第三方 App 生态压力的封闭设备）
- Google 内部利益博弈：Android 团队（30 亿设备）、Play Store 团队、硬件厂商都是既得利益者
- 2023 年大幅裁员，现在是"技术储备项目"——等待一个合适时机，或者永远等不到

**Fuchsia 代表了"如果今天从零设计移动 OS 应该长什么样"**，但自己革自己的命比被别人革还难。

---

## 15. 安全行业就业与技术方向

### 15.1 Android 逆向与内核安全的就业方向

**大陆正规需求：**

| 行业 | 具体岗位 | 代表公司 |
|---|---|---|
| 手机厂商 | 系统安全、红队 | 华为、小米、OPPO、vivo |
| 互联网大厂 | 安全实验室、反外挂 | 腾讯、字节、网易、阿里 |
| 安全厂商 | 移动威胁分析 | 奇安信、安恒、深信服 |
| 游戏公司 | 反作弊、保护 SDK | 腾讯游戏、网易游戏 |
| 金融/支付 | 移动端风控、设备指纹 | 蚂蚁、微信支付、各银行科技 |

**游戏反外挂是大陆需求量最大、最稳定的逆向方向**，腾讯一家就能消化大量人才，技术积累可以平移到其他方向。

**内核/驱动方向的就业：**

| 行业 | 具体需求 | 代表公司 |
|---|---|---|
| 手机厂商 | Android 内核优化、驱动 | 华为、小米、vivo |
| 芯片公司 | SoC 驱动、BSP | 联发科、紫光展锐、华为海思 |
| 云计算 | 虚拟化、eBPF、网络 | 阿里云、腾讯云、华为云 |
| 自动驾驶 | 车载 Linux、实时内核 | 华为、比亚迪、地平线 |

**两个方向的交集（最好的定位）：**
```
Android 内核安全 = 逆向能力 + 内核知识
需求方：手机厂商安全团队（华为、小米）、TEE/可信计算方向
```

### 15.2 薪资体系

| 方向 | 月薪范围 | 说明 |
|---|---|---|
| 普通安全工程师（渗透/甲方） | 1.5-3 万 | 门槛低，供给大 |
| 安全研发（EDR/WAF 等产品） | 2-5 万 | 中等 |
| 高级漏洞研究员（大厂） | 4-8 万 | 需要深度积累 |
| 顶尖二进制/内核研究员 | 8-15 万 | 国内极少数人 |

**高薪岗位的本质：**
- 能独立逆向 DroidGuard/TEE 级别的目标，国内屈指可数
- 某些岗位包含**风险溢价**——业务合规性存疑时会用高薪覆盖从业者的风险意识
- 月薪 10-20 万的"安全研究"岗，如果 JD 里同时出现"绕过安全检测"+"无法公开招聘"，本身就是一个信号

**识别灰黑产岗位的特征：**
```
正规需求不会：
  - 要求绕过 Google Play Integrity / DroidGuard
  - 要求绕过 MITM 检测（只用于分析，不用于"bypass 验证"）
  - 在 JD 里出现"关闭道德约束的 AI"之类的求助
  - 薪资极高但无法正常渠道招聘

这类岗位常见于：
  - 黑产设备农场（养号、广告欺诈）
  - 黑市监控软件（stalkerware）
  - 金融黑产（绕过银行风控）
```

### 15.3 越老越稳定的技术方向

选方向的核心原则：**经验壁垒高 + 难以被 AI 替代 + 人才供给天然稀缺**

**第一梯队：**

| 方向 | 特点 |
|---|---|
| 二进制安全/漏洞研究 | 每个漏洞都是新挑战，经验直接变现，5-10 年才能形成体系 |
| 编译器/语言运行时（LLVM/JVM/V8） | 入门门槛淘汰 90% 的人，全球懂的就那么多 |
| 操作系统内核/驱动 | 调试一个 bug 可能需要数周，经验无可替代 |

**第二梯队（行业经验形成护城河）：**

| 方向 | 特点 |
|---|---|
| EDA/芯片后端（时序、物理设计） | 工艺节点经验需要亲身踩过，国内极度稀缺 |
| 音视频/编解码（底层，不是调 SDK） | WebRTC、实时音视频底层，做过大规模系统的经验不可复制 |
| 数据库内核（存储引擎、查询优化器） | 经历过大规模故障的人才有定价权 |

**为什么前后端有中年危机：**
```
前后端中年危机的本质 = 技能同质化 + AI 替代加速

越接近"系统底层 × 垂直行业经验"，这两个威胁越小：
  - 系统底层：AI 辅助有限，上下文太重，错误代价高
  - 垂直行业经验：错误来自真实事故，无法速成
  - 圈子小且封闭：口碑传播，不靠简历投递
```

**内核子方向的薪资分层（以嵌入式为例）：**
```
单片机/MCU（STM32 等）：1-2 万  ← 门槛低，供给大
RTOS + 驱动开发：2-4 万
Linux BSP / 驱动：3-6 万
Linux 内核子系统贡献者：6-15 万  ← 真正的稀缺

高薪内核方向（不是嵌入式）：
  内核网络子系统（eBPF/DPDK/XDP）← 云厂商抢着要
  文件系统（Btrfs/io_uring）      ← 存储公司核心
  虚拟化（KVM/hypervisor）        ← 云计算基础
```

**嵌入式本身不是好赛道，但嵌入式是进入内核的一条路**。路径建议：
```
驱动开发 → Linux 内核某个子系统深入 → 成为该子系统的专家
```
而不是停留在"会移植 Linux 到开发板"这个层面。

### 15.4 从现有背景入门 Android 安全

**起点评估：** 有 C 基础（学过但手生）+ 了解 bootloader/uboot + 了解 SO 加固概念 + 接触过 ARM 裸机

这个起点比完全空白好很多——有地图但还没走过路。

**热身（2 周）：**
```
写这几个小程序找回 C 手感：
  1. 手写 malloc/free（理解内存布局）
  2. 实现一个简单的链表
  3. 写一个解析 ELF 文件头的小工具（直接和逆向相关）
```

**第一个月：Frida 实战**
```
环境搭建：adb + Frida + jadx
目标：找一个 CTF Android 题，从头跑通完整流程
  静态分析（JADX）→ 动态 hook（Frida）→ 拿到 flag
不要一开始就上加固 App，先跑通基本流程
```

**第二个月：读 bionic/linker 源码**
```
你知道 SO 加固概念，linker 是加固的核心战场
结合 JADX 看一个真实加固 SO 的加载过程
理解 dlopen → linker → 加固 hook 的关系
```

**第三个月：第一个内核模块**
```
写一个 Hello World 内核模块（在 Pixel 3 上运行）
再写一个简单的字符设备驱动
你有 uboot 基础，这步会比别人轻松
```

**Pixel 3 的价值：**
```
刷 Magisk → root → frida-server → 可以 hook 任意进程（包括系统进程）
刷自编译 AOSP → 在系统里加 log → 直接观察 Binder 调用
编译开启 KASAN/KCOV 的内核 → 真机 CVE 复现
有 Titan M → 合法研究 TEE 的真实硬件平台
```

**不建议先碰的方向：**
- TEE 研究：投入产出比极差，一个漏洞可能研究数月，适合大厂安全实验室养着慢研究
- 直接上 DroidGuard 逆向：这是最顶级的目标，先把基础打扎实

---

## 16. 学习资源与工具链

### 16.1 工具链清单

**逆向分析（静态）：**
```
jadx        反编译 APK/DEX → Java 代码
IDA Pro     反汇编 Native（ARM64）代码，工业标准
Ghidra      NSA 开源，免费，功能接近 IDA
apktool     解包/重打包 APK，修改 smali
```

**动态分析：**
```
Frida       动态插桩，hook Java/Native 方法
adb         Android 调试桥，最基础的工具
Magisk      root 方案，frida-server 依赖它
BurpSuite   HTTPS 抓包（需要配合证书安装）
tcpdump     内核级网络抓包（root 后可用）
```

**内核调试：**
```
KASAN       内核地址消毒器，检测内存 bug（需要自编译内核）
KCOV        内核代码覆盖率（用于 fuzzing）
GDB + QEMU  内核调试（模拟器环境）
crash       分析内核崩溃 dump
```

**代码分析：**
```
cs.android.com          AOSP 代码搜索，支持历史版本
android.googlesource.com Android 内核源码
elixir.bootlin.com      Linux 内核交叉引用
```

### 16.2 学习资源

**博客（中文，Android/内核安全方向最好的）：**
- `evilpan` — Android 安全，Binder/so 加固，深度分析
- `bsauce` — Android 内核漏洞分析，CVE 复现
- `天问` — Android 内核安全

**实战练习：**
```
CTFtime.org     → 搜 Android 分类，从简单题开始
AOSP Security Bulletins → 每月安全公告，CVE 学习来源
Google Project Zero → 顶级漏洞分析，目标导向
```

**书籍：**
```
《Android 软件安全权威指南》     逆向入门系统教材
《程序员的自我修养》              ELF/链接/装载，理解 so 加固的基础
《深入理解 Android 内核设计思想》 Binder/ART 等框架原理
```

### 16.3 环境搭建清单

```
必备环境：
  ☐ Android Studio + SDK + NDK
  ☐ adb 可用（手机开启开发者模式）
  ☐ jadx-gui（反编译工具）
  ☐ IDA Free 或 Ghidra
  ☐ Python 3 + frida-tools（pip install frida-tools）
  ☐ frida-server 刷到手机（需要 root）

进阶环境（Pixel 3 解锁后）：
  ☐ 刷 Magisk（root）
  ☐ 编译 AOSP（刷入自定义系统用于研究）
  ☐ 编译开启 KASAN 的内核（用于 CVE 复现）
  ☐ 安装 LSPosed（Xposed 现代版，方便 Java 层 hook）
```

---

## 附：关键概念速查

| 概念 | 一句话解释 |
|---|---|
| DTB | Device Tree Blob，描述硬件拓扑的二进制文件，bootloader 传给内核 |
| memblock | 内核最早期的内存分配器，只能分配不能释放 |
| buddy system | 正式的页分配器，按 2^n 页管理物理内存 |
| slab/slub | 小对象分配器，kmalloc 的底层 |
| MMU | CPU 内部的地址翻译单元，虚拟地址 → 物理地址 |
| TLB | 页表缓存，MMU 内部的 CAM 结构，并行比较所有条目 |
| TTBR0/TTBR1 | 用户/内核页表根地址寄存器，TTBR1 永不切换 |
| ASID | 地址空间 ID，让不同进程的 TLB 条目共存，避免切换时全部失效 |
| TLBI | TLB Invalidate 指令，软件主动让 TLB 条目失效 |
| DMA | Direct Memory Access，硬件搬运数据，CPU 不参与 |
| MMIO | Memory-Mapped I/O，硬件寄存器映射到物理地址空间 |
| ISB | Instruction Synchronization Barrier，冲刷流水线，确保寄存器写入生效 |
| COW | Copy-on-Write，fork 后共享内存页，写时才复制（Dirty COW 漏洞的来源） |
| AVC | Access Vector Cache，SELinux 的三级权限缓存 |
| CSD | Call Single Data，IPI 的数据载体 |
| IPI | Inter-Processor Interrupt，处理器间中断，用于跨 CPU 调用函数 |
| CAM | Content Addressable Memory，按内容查地址，TLB 用它实现并行查找 |
| pKVM | Android 保护型虚拟机，Host 内核也无法访问 Guest 内存（Pixel 6+） |
| AVB | Android Verified Boot，签名验证链 |
| TZASC | TrustZone Address Space Controller，安全内存的硬件守卫 |
| GKI | Generic Kernel Image，统一内核镜像，厂商只能加模块不能改内核 |
| Backport | 把新版本的 fix 移植回旧版本，backport 失误是 Android CVE 的重要来源 |
| initcall | 内核驱动注册初始化函数的机制，do_initcalls() 按 level 批量调用 |
| per-cpu | 每个 CPU 有独立副本的变量，无需加锁，性能极高 |
| RCU | Read-Copy-Update，内核最重要的无锁同步机制，读者无锁 |
| WFI | Wait For Interrupt，ARM 低功耗指令，idle 进程执行它让 CPU 休眠 |
| Zygote | Android Java 世界起点，预加载类后 fork 所有 App 进程 |
| ART | Android Runtime，替代 Dalvik 的虚拟机，支持 AOT/JIT 编译 |
| MAC | Mandatory Access Control，强制访问控制，SELinux 的工作模式 |
| DAC | Discretionary Access Control，自主访问控制，传统 Unix 权限位 |
| domain | SELinux 中进程的类型标签，untrusted_app/system_server 等 |
| neverallow | SELinux 编译期检查规则，违反则无法编译策略 |
| FIDL | Fuchsia Interface Definition Language，Fuchsia 的统一 IPC 接口描述语言 |
| Capability | Fuchsia 的安全模型，持有 Handle 才能访问资源，无 Handle 则资源不可见 |
