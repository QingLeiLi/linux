# SPDX-License-Identifier: GPL-2.0
#
# kernel/bpf 构建学习导读
#
# kernel/Makefile 仅在 CONFIG_BPF=y 时进入本目录。本文件再把源码分成四个构建层次：
#   1. core.o 随基础 BPF 永远内建，提供解释执行和通用程序生命周期；
#   2. BPF_SYSCALL 打开现代程序、map、BTF、验证器和 iterator 控制面；
#   3. NET、CGROUPS、PERF_EVENTS、MMU/64BIT 等环境能力追加专用实现；
#   4. BPF_JIT、BPF_LSM、DEBUG_INFO_BTF 和 PRELOAD 追加本机执行或上层集成。
# `obj-$(CONFIG_X)` 会在 X=y 时把对象链接进当前 built-in.a，在 X=m 时进入模块对象
# 集合；本目录关键总开关大多是 bool，实际通常为 y 或空。`+=` 允许同一配置按学习主题
# 分行累积，顺序还可能影响链接、DWARF/BTF 扫描结果，不能随意排序。

# core.o 是目录的无条件基础对象：只要上层 CONFIG_BPF 进入本目录，它就提供经典/扩展
# BPF 共用的解释器、程序分配和释放等核心能力，不要求暴露 bpf() 系统调用。
obj-y := core.o

# ALWAYS_ON 会移除解释器；否则 x86+GCC 编译解释器核心时关闭 GCSE，见原注释所列提交。
# `cflags-nogcse-$(A)$(B)` 把两个 y 拼成后缀 yy，最后只在两项同时成立时取到该标志。
ifneq ($(CONFIG_BPF_JIT_ALWAYS_ON),y)
# ___bpf_prog_run() needs GCSE disabled on x86; see 3193c0836f203 for details
# x86 上用 GCC 编译 ___bpf_prog_run() 时必须禁用全局公共子表达式消除；历史原因和错误
# 细节由提交 3193c0836f203 记录。ALWAYS_ON 没有解释器函数，因而无需这个定向规避。
cflags-nogcse-$(CONFIG_X86)$(CONFIG_CC_IS_GCC) := -fno-gcse
endif
# core.o 还关闭“后初始化覆盖”诊断，因为解释器跳转/操作表有意用范围初始化后再覆盖
# 特定 opcode；第二项只在上面的 x86+GCC+非 ALWAYS_ON 组合中展开为 -fno-gcse。
CFLAGS_core.o += -Wno-override-init $(cflags-nogcse-yy)

# BPF_SYSCALL 基础闭包：syscall.o 提供 FD ABI，verifier.o 证明程序安全，inode.o 承载
# bpffs pin，helpers.o 提供 helper 分派；tnum/cnum/liveness/const_fold 构成抽象解释，log
# 输出诊断，token 管理委托权限。关闭 syscall 时这些现代控制面对象不参与链接。
obj-$(CONFIG_BPF_SYSCALL) += syscall.o verifier.o inode.o helpers.o tnum.o cnum.o log.o token.o liveness.o const_fold.o
# iterator 核心及 map/task/prog/link 四种对象迭代器随 syscall 一起构建，用户通过 link/FD
# 建立 seq_file 会话；具体 cgroup、BTF 等迭代器在满足额外依赖时于后文追加。
obj-$(CONFIG_BPF_SYSCALL) += bpf_iter.o map_iter.o task_iter.o prog_iter.o link_iter.o
# 通用 map 实现及其共享分配器：哈希/数组/LRU/LPM/内嵌 map/bloom filter 都依赖 bpf()
# 创建和 FD 生命周期，因此归入 syscall 闭包。
obj-$(CONFIG_BPF_SYSCALL) += hashtab.o arraymap.o percpu_freelist.o bpf_lru_list.o lpm_trie.o map_in_map.o bloom_filter.o
# 本组补齐本地存储、队列/栈、ring buffer 与只读指令数组等 map 或 map 类基础对象。
obj-$(CONFIG_BPF_SYSCALL) += local_storage.o queue_stack_maps.o ringbuf.o bpf_insn_array.o
# bpf_local_storage.o 是通用本地存储核心，task 包装器在 syscall 可用时一并注册对应 map/helper。
obj-$(CONFIG_BPF_SYSCALL) += bpf_local_storage.o bpf_task_storage.o
# fixups/cfg/states/backtrack/check_btf 是验证流水线的重写、控制流、状态集合、精度回溯和
# BTF 前置检查阶段；它们由 verifier.o 消费，不是可独立选择的功能。
obj-$(CONFIG_BPF_SYSCALL) += fixups.o cfg.o states.o backtrack.o check_btf.o
# inode 本地存储只在 BPF_LSM=y 时注册；花括号写法与 `$(...)` 都是 make 变量展开。
obj-${CONFIG_BPF_LSM}	  += bpf_inode_storage.o
# disasm.o 为日志/JIT 等消费者格式化指令，mprog.o 管理一个挂点上的多程序集合。
obj-$(CONFIG_BPF_SYSCALL) += disasm.o mprog.o
# trampoline 生成并管理本机跳板，必须有 JIT 后端；它不单独依赖 syscall 行，是因为
# BPF_JIT 的上层配置关系保证基础 BPF，而实际调用点再受各自配置约束。
obj-$(CONFIG_BPF_JIT) += trampoline.o
# BTF 类型系统、BPF 专用内存分配器、可恢复排队自旋锁和程序流式读取属于现代 syscall 基础层。
obj-$(CONFIG_BPF_SYSCALL) += btf.o memalloc.o rqspinlock.o stream.o
# arena 需要 MMU 建立独立映射，并依赖 64 位地址空间表达 arena 指针；两个 bool 拼成 yy
# 才把 arena 与其范围树同时加入 syscall 构建。
ifeq ($(CONFIG_MMU)$(CONFIG_64BIT),yy)
obj-$(CONFIG_BPF_SYSCALL) += arena.o range_tree.o
endif
# dispatcher 用 JIT 生成面向多个候选程序的高效分派代码，解释器-only 构建不需要它。
obj-$(CONFIG_BPF_JIT) += dispatcher.o
# 网络为内建 bool 时，加入设备/CPU map、硬件卸载、netns 生命周期和 tcx 挂点实现；
# 非网络内核仍保留与网络无关的 BPF syscall/map 能力。
ifeq ($(CONFIG_NET),y)
obj-$(CONFIG_BPF_SYSCALL) += devmap.o
obj-$(CONFIG_BPF_SYSCALL) += cpumap.o
obj-$(CONFIG_BPF_SYSCALL) += offload.o
obj-$(CONFIG_BPF_SYSCALL) += net_namespace.o
obj-$(CONFIG_BPF_SYSCALL) += tcx.o
endif
# stackmap 采集调用栈时依赖 perf 栈遍历/事件基础设施；没有 PERF_EVENTS 就不注册该 map 类型。
ifeq ($(CONFIG_PERF_EVENTS),y)
obj-$(CONFIG_BPF_SYSCALL) += stackmap.o
endif
# cgroup 核心存在时，构建 cgroup 对象迭代器及 cgroup 本地存储包装；二者仍要求 syscall。
ifeq ($(CONFIG_CGROUPS),y)
obj-$(CONFIG_BPF_SYSCALL) += cgroup_iter.o bpf_cgrp_storage.o
endif
# cgroup.o 实现程序附着/运行策略，由更精确的 CONFIG_CGROUP_BPF 控制，而非仅看 CGROUPS。
obj-$(CONFIG_CGROUP_BPF) += cgroup.o
# reuseport array 服务 INET socket 的 SO_REUSEPORT 选择逻辑；无 IPv4/IPv6 Internet 栈时省略。
ifeq ($(CONFIG_INET),y)
obj-$(CONFIG_BPF_SYSCALL) += reuseport_array.o
endif
# sysfs_btf.o 只在 SYSFS 与 DEBUG_INFO_BTF 同时为 y 时发布内核 BTF 文件；嵌套条件防止
# 没有 sysfs 挂载能力的构建留下不可访问的发布代码。
ifeq ($(CONFIG_SYSFS),y)
obj-$(CONFIG_DEBUG_INFO_BTF) += sysfs_btf.o
endif
# struct_ops 和 cpumask kfunc 需要 JIT 承担类型化调用/本机执行，所以即使 syscall 已启用，
# 仍只在 CONFIG_BPF_JIT=y 的外层条件中加入。
ifeq ($(CONFIG_BPF_JIT),y)
obj-$(CONFIG_BPF_SYSCALL) += bpf_struct_ops.o
obj-$(CONFIG_BPF_SYSCALL) += cpumask.o
# bpf_lsm_proto.o must precede bpf_lsm.o. The current pahole logic
# deduplicates function prototypes within
# btf_encoder__add_saved_func() by keeping the first instance seen. We
# need the function prototype(s) in bpf_lsm_proto.o to take precedence
# over those within bpf_lsm.o. Having bpf_lsm_proto.o precede
# bpf_lsm.o ensures its DWARF CU is processed early, forcing the
# generated BTF to contain the overrides.
#
# Notably, this is a temporary workaround whilst the deduplication
# semantics within pahole are revisited accordingly.
# bpf_lsm_proto.o 必须排在 bpf_lsm.o 前。当前 pahole 在
# btf_encoder__add_saved_func() 中对函数原型去重时保留首次看到的实例；需要让
# bpf_lsm_proto.o 中的覆盖原型优先于 bpf_lsm.o 内的版本。按该顺序链接会让前者的
# DWARF 编译单元更早被处理，迫使最终 BTF 保存覆盖版本。这只是 pahole 重新设计去重
# 语义之前的临时规避，因此这里的对象顺序是生成 ABI 的一部分，不能按字母重排。
obj-${CONFIG_BPF_LSM} += bpf_lsm_proto.o bpf_lsm.o
endif
# CRYPTO 为 y 或 m 时变量都非空，crypto.o 才向已启用的 BPF syscall 注册加解密 kfunc；
# 空值表示整个 crypto 框架缺失，不应编译引用其接口的实现。
ifneq ($(CONFIG_CRYPTO),)
obj-$(CONFIG_BPF_SYSCALL) += crypto.o
endif
# preload/ 是 Kbuild 子目录目标；BPF_PRELOAD 打开后再由子目录按 BPF_PRELOAD_UMD 决定
# 是否生成并链接用户模式驱动载荷。
obj-$(CONFIG_BPF_PRELOAD) += preload/

# CO-RE/BTF 后处理层：三个很小的 .c 文件以不同宏模式复用 libbpf 算法源码；btf_iter
# 暴露 BTF 对象迭代，kmem_cache_iter 暴露 slab cache。它们均属于 syscall 控制面。
obj-$(CONFIG_BPF_SYSCALL) += relo_core.o
obj-$(CONFIG_BPF_SYSCALL) += btf_iter.o
obj-$(CONFIG_BPF_SYSCALL) += btf_relocate.o
obj-$(CONFIG_BPF_SYSCALL) += kmem_cache_iter.o
# dma-buf 迭代器只有在 DMA_SHARED_BUFFER 内建时才有可遍历对象和稳定的链接依赖。
ifeq ($(CONFIG_DMA_SHARED_BUFFER),y)
obj-$(CONFIG_BPF_SYSCALL) += dmabuf_iter.o
endif

# 这些低层 map 分配器/容器和 rqspinlock 可由 tracing/BPF 路径自身调用。移除函数跟踪
# 编译标志可避免在内部锁与分配热路径再次进入 ftrace/BPF trampoline，形成递归、改变
# 锁时序或污染本应最小化的基础路径；只影响插桩，不改变优化级别和函数业务语义。
CFLAGS_REMOVE_percpu_freelist.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_bpf_lru_list.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_queue_stack_maps.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_lpm_trie.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_ringbuf.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_rqspinlock.o = $(CC_FLAGS_FTRACE)
