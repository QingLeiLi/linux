# `kernel/bpf` 学习注释任务清单

## 使用规则与验收口径

本文件是 `kernel/bpf` 全目录学习注释长任务的持久化进度源。会话中断或上下文压缩后，先读取本文件并
核对工作树；不得凭对话记忆推断进度。主标准为
`doc/linux-kernel-source-learning-methodology.md`，尤其是第 5～10、17 和 19 章。

严格采用单文件闭环：

1. 开始前把唯一目标文件标为 `[~]`，完整读取并建立函数、实体、英文注释、路径及并发/生命周期索引。
2. 只追加中文学习注释；不修改代码，不删除、改写、移动或合并原有注释。
3. 按源码顺序以 1～5 个函数为一批修改；每批先重读当前窗口，修改后复读并检查局部 diff。
4. 完整复读最终文件，按方法论第 17 章记录函数/实体/英文注释/路径/并发与生命周期/关联读取清单，
   并对至少三个复杂函数做初学者复述和开发者推理抽查（不足三个复杂函数时全部抽查）。
5. C/H 文件必须通过 `scripts/check-learning-comment-density.py --min-density 0.20 --max-code-gap 10`；
   再执行追加式 diff 审计、`git diff --check`、增量 checkpatch，并在可用配置下做相关构建检查。
6. 只有全部门禁通过后才标为 `[x]`；存在缺口只能记录“检查点”，不得声称全文件完成。
7. 自动生成且标明不可修改的文件不直接补注；记录生成器、生成命令、生成源覆盖情况和豁免理由。
   纯转发/占位文件仍需核对其构建作用，再决定补注或记录有证据的豁免。
8. 为核对契约读取的目录外源码仅登记，不擅自扩大修改范围。

状态：`[ ]` 未开始；`[~]` 正在处理或验收；`[x]` 已通过完整验收；`[g]` 自动生成文件，经证据审计豁免。

## 处理顺序

顺序按“构建入口 → 抽象域与验证器工具 → 程序/对象生命周期 → map 基础设施与实现 → BTF/CO-RE →
附着与程序类型 → 迭代器 → preload 产物”组织。这样先建立术语和数据模型，再阅读大文件中的消费者，
避免按字母序造成反复跨文件追踪。

### 1. 构建入口与目录地图

- [x] `kernel/bpf/Kconfig`
- [x] `kernel/bpf/Makefile`

### 2. 验证器抽象域、控制流与诊断基础

- [x] `kernel/bpf/tnum.c`
- [x] `kernel/bpf/cnum_defs.h`
- [x] `kernel/bpf/cnum.c`
- [x] `kernel/bpf/range_tree.h`
- [x] `kernel/bpf/range_tree.c`
- [x] `kernel/bpf/cfg.c`
- [x] `kernel/bpf/states.c`
- [x] `kernel/bpf/liveness.c`
- [x] `kernel/bpf/backtrack.c`
- [x] `kernel/bpf/const_fold.c`
- [~] `kernel/bpf/fixups.c`
- [ ] `kernel/bpf/disasm.h`
- [ ] `kernel/bpf/disasm.c`
- [ ] `kernel/bpf/log.c`
- [ ] `kernel/bpf/check_btf.c`
- [ ] `kernel/bpf/verifier.c`

### 3. 程序、链接与系统调用生命周期

- [ ] `kernel/bpf/bpf_insn_array.c`
- [ ] `kernel/bpf/core.c`
- [ ] `kernel/bpf/memalloc.c`
- [ ] `kernel/bpf/token.c`
- [ ] `kernel/bpf/stream.c`
- [ ] `kernel/bpf/dispatcher.c`
- [ ] `kernel/bpf/trampoline.c`
- [ ] `kernel/bpf/helpers.c`
- [ ] `kernel/bpf/syscall.c`

### 4. map 公共设施与具体实现

- [ ] `kernel/bpf/percpu_freelist.h`
- [ ] `kernel/bpf/percpu_freelist.c`
- [ ] `kernel/bpf/bpf_lru_list.h`
- [ ] `kernel/bpf/bpf_lru_list.c`
- [ ] `kernel/bpf/map_in_map.h`
- [ ] `kernel/bpf/map_in_map.c`
- [ ] `kernel/bpf/mmap_unlock_work.h`
- [ ] `kernel/bpf/arraymap.c`
- [ ] `kernel/bpf/hashtab.c`
- [ ] `kernel/bpf/lpm_trie.c`
- [ ] `kernel/bpf/queue_stack_maps.c`
- [ ] `kernel/bpf/stackmap.c`
- [ ] `kernel/bpf/ringbuf.c`
- [ ] `kernel/bpf/bloom_filter.c`
- [ ] `kernel/bpf/reuseport_array.c`
- [ ] `kernel/bpf/devmap.c`
- [ ] `kernel/bpf/cpumap.c`
- [ ] `kernel/bpf/arena.c`
- [ ] `kernel/bpf/cpumask.c`
- [ ] `kernel/bpf/crypto.c`

### 5. 本地存储基础设施与专用包装

- [ ] `kernel/bpf/bpf_local_storage.c`
- [ ] `kernel/bpf/local_storage.c`
- [ ] `kernel/bpf/bpf_task_storage.c`
- [ ] `kernel/bpf/bpf_inode_storage.c`
- [ ] `kernel/bpf/bpf_cgrp_storage.c`

### 6. BTF、重定位与类型驱动能力

- [ ] `kernel/bpf/btf.c`
- [ ] `kernel/bpf/btf_relocate.c`
- [ ] `kernel/bpf/btf_iter.c`
- [ ] `kernel/bpf/relo_core.c`
- [ ] `kernel/bpf/bpf_struct_ops.c`
- [ ] `kernel/bpf/sysfs_btf.c`

### 7. 附着点、命名空间与程序类型

- [ ] `kernel/bpf/cgroup.c`
- [ ] `kernel/bpf/inode.c`
- [ ] `kernel/bpf/offload.c`
- [ ] `kernel/bpf/net_namespace.c`
- [ ] `kernel/bpf/mprog.c`
- [ ] `kernel/bpf/tcx.c`
- [ ] `kernel/bpf/rqspinlock.h`
- [ ] `kernel/bpf/rqspinlock.c`
- [ ] `kernel/bpf/bpf_lsm_proto.c`
- [ ] `kernel/bpf/bpf_lsm.c`

### 8. BPF iterator 框架与具体迭代器

- [ ] `kernel/bpf/bpf_iter.c`
- [ ] `kernel/bpf/link_iter.c`
- [ ] `kernel/bpf/prog_iter.c`
- [ ] `kernel/bpf/map_iter.c`
- [ ] `kernel/bpf/btf_iter.c`
- [ ] `kernel/bpf/task_iter.c`
- [ ] `kernel/bpf/cgroup_iter.c`
- [ ] `kernel/bpf/kmem_cache_iter.c`
- [ ] `kernel/bpf/dmabuf_iter.c`

### 9. preload 构建、加载器与生成物

- [ ] `kernel/bpf/preload/Kconfig`
- [ ] `kernel/bpf/preload/Makefile`
- [ ] `kernel/bpf/preload/.gitignore`
- [ ] `kernel/bpf/preload/bpf_preload.h`
- [ ] `kernel/bpf/preload/bpf_preload_kern.c`
- [ ] `kernel/bpf/preload/iterators/Makefile`
- [ ] `kernel/bpf/preload/iterators/README`
- [ ] `kernel/bpf/preload/iterators/.gitignore`
- [ ] `kernel/bpf/preload/iterators/iterators.bpf.c`
- [ ] `kernel/bpf/preload/iterators/iterators.lskel-little-endian.h`
- [ ] `kernel/bpf/preload/iterators/iterators.lskel-big-endian.h`

## 当前文件记录

### `kernel/bpf/Kconfig`

- 状态：全文件完成；已按方法论第 17 章完成强制验收。
- 文件职责与实体清单：完整复读最终 198 行。9 个配置实体 9/9 均有紧邻学习说明；覆盖隐藏基础
  `BPF`、两类架构 JIT 能力、架构默认策略、syscall 控制面、JIT/强制 JIT/默认 JIT 三层策略、非特权
  默认策略与 BPF LSM。另解释 menu/source/endmenu、`depends on`、条件 `select`、`def_bool` 和 sysctl
  与编译期开关的边界。Kconfig 无函数、结构体、局部变量，函数清单和复杂函数抽查不适用。
- 英文注释与路径验收：SPDX 豁免；其余 7 组英文 `#` 注释和 5 组英文 help 均逐字保留，并在紧邻
  位置完整翻译和补充机制。仅凭注释可复述 `BPF_SYSCALL → BPF/RCU/网络设施`、`HAVE_* → BPF_JIT
  → EXECMEM`、`ALWAYS_ON → 移除解释器`、非特权 sysctl 的 0/1/2 状态和 `BPF_LSM` 四项依赖；关闭
  syscall、JIT、NET 或 LSM 时的退化边界均已说明。该配置文件不实现运行期锁、RCU 或对象回收，
  相关并发/生命周期只登记为被选基础设施及 FD ownership，避免把 Kconfig 写成实现契约。
- 关联读取：`kernel/bpf/preload/Kconfig`（source 落点，注释缺失，已列入后序）；`kernel/trace/Kconfig`
  的 `BPF_EVENTS`（LSM tracing 依赖，注释缺失）；`kernel/rcu/Kconfig` 的 `NEED_TASKS_RCU`/
  `TASKS_TRACE_RCU`（syscall 选择的宽限期能力，注释缺失）；`mm/Kconfig` 的 `EXECMEM`（JIT 可执行
  内存依赖，该符号有简短学习注释）；`net/Kconfig` 的 `NET_XGRESS`/`NET_SOCK_MSG`/`PAGE_POOL`
  （条件网络依赖，部分覆盖）；`lib/Kconfig` 的 `BINARY_PRINTF`（格式化依赖，注释缺失）。目录内目标
  将按本清单继续处理；目录外仅作契约核对，不扩大修改范围。
- 修改安全：新增 94 行、删除 0 行，原配置和原注释零改动；`git diff --check` 通过。完整文件
  checkpatch 为 0 errors/2 warnings，两项均是原有 `BPF_SYSCALL`/`BPF_LSM` help 不足四行；学习注释
  不能改写原 help。Kconfig 不适用 C/H 密度脚本。尝试以独立 `/tmp` 输出目录运行 `make allnoconfig`
  做解析验证，但宿主 GNU Make 3.81 低于源码要求的 4.0，未能进入 Kconfig 解析，已明确记录。

### `kernel/bpf/Makefile`

- 状态：全文件完成；已按方法论第 17 章完成强制验收。
- 文件职责与实体清单：完整复读最终 141 行。逐项覆盖 `core.o` 基础层、6 组 syscall 验证/map/
  iterator 对象、JIT trampoline/dispatcher、MMU+64 位 arena、NET/PERF/CGROUP/INET/SYSFS/CRYPTO/
  DMA-BUF 条件对象、BPF LSM 顺序、preload 子目录、CO-RE 转发对象和 6 个移除 ftrace 标志的底层对象。
  解释 `obj-*`、条件后缀拼接、`ifeq/ifneq`、对象顺序与定向 CFLAGS；Makefile 无函数/结构体/局部变量，
  函数清单与复杂函数抽查不适用。
- 英文注释与路径验收：SPDX 豁免；GCSE 和 pahole 两组英文注释均逐字保留、紧邻完整翻译并补充
  构建机制。仅凭注释可从 `kernel/Makefile: CONFIG_BPF` 进入 core，再沿 syscall、环境能力和上层集成
  四层恢复对象闭包；可解释 x86+GCC+解释器为何得到 `-fno-gcse`，以及为何 LSM 两对象不能重排。
  构建描述文件无运行期锁或对象 ownership；ftrace 移除项只说明防递归/锁时序的构建边界。
- 关联读取：`kernel/Makefile` 的 `obj-$(CONFIG_BPF) += bpf/`（目录入口，部分学习注释）；
  `kernel/bpf/preload/Makefile` 和 `preload/iterators/Makefile`（子构建落点，注释缺失，已列入后序）；
  `kernel/bpf/percpu_freelist.c` 的 resilient lock 热路径与 `kernel/bpf/rqspinlock.c` 的低层锁实现
  （核对移除 ftrace 的递归边界，注释缺失，均已列入后序）；顶层 `Makefile` 的 `CC_FLAGS_FTRACE`
  定义（核对移除变量含义，注释缺失）。
- 修改安全：新增 65 行、删除 0 行，原构建语句和原注释零改动；`git diff --check` 通过，完整文件
  checkpatch 为 0 errors/0 warnings。Makefile 不适用 C/H 密度脚本。GNU Make 3.81 的 `-pn` 解析探针
  读完整文件后仅因故意请求的不存在目标退出 2，未报告语法错误；完整 Kbuild 仍受 GNU Make 4.0
  最低版本限制而未执行。

### `kernel/bpf/tnum.c`

- 状态：全文件完成；已按方法论第 17 章完成强制验收。
- 文件职责与函数清单：完整复读最终 579 行。27 个函数定义 27/27 均有紧邻专属契约；覆盖常量/
  范围构造，逻辑/算术移位，加减负、AND/OR/XOR、长乘法，集合 overlap/intersect/union/包含，窄化、
  对齐、二进制格式化，四个 32 位子寄存器 helper，三种字节交换和离散成员 step。每个契约均明确
  业务背景、全部入参、返回/副作用、调用者前提与不睡眠属性；复杂 mul、intersect、step 另有阶段
  走读。实体清单覆盖 `TNUM` 复合字面量宏、`tnum_unknown` 全局常量及全部重要局部中间量。
- 英文注释与路径验收：SPDX 豁免；文件导读、unknown、range 特例/示例、arshift、mul 全算法/行内
  分支、intersect 警告、union、step 全算法/边界等 11 组英文说明均逐字保留，并有紧邻完整翻译与
  机制补充。仅凭注释可复述 `(value, mask)` 规范不变量和具体集合、加减进位/借位传播、长乘未知位
  两分支合并、range/union 的安全过近似、intersect 的冲突前提、状态包含方向及 step 的掩码内进位。
- 并发/生命周期与抽查：tnum 是无引用的按值对象，全部函数纯计算、不睡眠且无锁/RCU/发布/回滚。
  抽查 `tnum_mul()` 可恢复按 LSB 分列、未知位 union 与移位终止；抽查 `tnum_intersect()` 可指出它
  不能表示空集及冲突位会取已知 1；抽查 `tnum_step()` 可推导 tmin/tmax 饱和和 carry_mask 穿过
  非 mask 空洞。5 位缩小模型穷举全部 243 个规范状态，验证 add/sub/mul、AND/OR/XOR 的结果覆盖、
  union 覆盖、overlap/包含等价关系和 step 下一成员，全部通过。
- 关联读取：`include/linux/tnum.h` 全部结构与声明（确认 value/mask、对齐/包含/格式化公开契约，
  注释缺失）；`kernel/bpf/verifier.c` 的 var_off 构造、ALU、JMP、bounds 同步、coerce/子寄存器和
  step 调用点（核对参数范围与上下游，注释缺失，已列入后序）；`kernel/bpf/states.c` 的
  `regsafe()`/`tnum_in()` 剪枝方向（注释缺失，已列入后序）。目录外头文件建议另立任务补注。
- 修改安全：新增 260 行、删除 0 行，原代码和原英文注释零改动；密度门禁
  `code=193, comments=342, chinese=194, density=1.005, max_gap=10` 通过；`git diff --check` 通过。
  增量 checkpatch 为 0 errors/96 个中文 UTF-8 视觉行宽 checks；完整文件另有 1 error，落在未改的
  原有 `TNUM` 复合字面量宏 `COMPLEX_MACRO`，并有相同视觉行宽 checks。工作树无 `.config`，且宿主
  GNU Make 版本不足，未执行目标对象构建；注释修改不改变预处理结果。

### `kernel/bpf/cnum_defs.h`

- 状态：全文件完成；已按方法论第 17 章完成强制验收。
- 文件职责与函数清单：完整复读最终 420 行。19 个模板函数定义 19/19 均有紧邻专属契约，经
  `T=32/64` 两次包含实际生成 38 个符号；覆盖有/无符号构造与 min/max、两类断点判定、交集及三个
  原地包装、满圆规范化、加法/取负、空/常量/成员/子集判断。所有契约列出模板展开名、全部参数、
  返回/原地副作用、不睡眠和前置条件。实体清单覆盖 T、类型/极值/EMPTY/FN 拼接宏、base/size 圆弧
  不变量、局部旋转坐标 b1/dbase 及宏撤销协议。
- 英文注释与路径验收：SPDX/版权和纯 ASCII 图豁免；无符号/有符号双区间、无溢出改写、四类交集
  布局、旋转参考系和 subset 旋转等全部英文文字逐字保留、紧邻翻译并补充机制。仅凭注释可复述
  “base 起点+size 跨度”的模圆集合、U/S 两个线性断点、双弧交集为何过近似、加法跨度溢出为何
  退化满圆、取负为何从旧末端开始，以及状态剪枝的超集/子集参数方向。
- 并发/生命周期与抽查：按值纯计算，无锁、RCU、分配、引用、发布或失败回滚；三个 `*_with`
  包装只在调用者保证 dst 稳定时原地覆盖。抽查 intersect 可沿四种 ASCII 布局恢复 EMPTY/单弧/
  双弧过近似；抽查 add/negate 可推导模运算和 EMPTY 传播；抽查 is_subset 可证明旋转后 bigger
  不环绕并避免线性 min/max 空洞误判。5 位缩小模型穷举 995 个规范/EMPTY 编码的全组合，验证
  intersect 覆盖真实交集、add 覆盖真实和集、negate 精确以及 subset 与具体集合包含等价，全部通过。
- 关联读取：`kernel/bpf/cnum.c` 的两次模板包含与跨位宽函数（注释缺失，紧接后序）；
  `include/linux/cnum.h` 的结构、UNBOUNDED/EMPTY 编码和公开声明（注释缺失）；
  `include/linux/bpf_verifier.h` 的 r32/r64 字段与 accessor（注释缺失）；`kernel/bpf/verifier.c` 的
  bounds 同步、ALU、条件分支收紧和 tnum 转换调用（注释缺失，已列入后序）；`kernel/bpf/states.c`
  的 cnum subset 剪枝（注释缺失，已列入后序）。另读取引入提交 `256f0071f9b6` 与修复提交
  `cd5b460ed1ec` 说明，核对 CBMC 验证范围和线性 min/max 误判背景；目录外建议另立任务补注。
- 修改安全：新增 173 行、删除 0 行，原代码/宏/英文注释零改动；密度门禁
  `code=154, comments=238, chinese=128, density=0.831, max_gap=8` 通过；`git diff --check` 通过，
  增量 checkpatch 0 errors/43 个中文 UTF-8 视觉行宽 warnings。工作树无可用 `.config` 且 GNU Make
  版本不足，未构建对象；缩小位宽的独立集合模型已补充算法验证。

### `kernel/bpf/cnum.c`

- 状态：全文件完成；已按方法论第 17 章完成强制验收。
- 文件职责与函数清单：完整复读最终 176 行。两个显式函数 2/2 均有紧邻专属契约；另完整解释
  `T=32/64` 两次包含如何生成前文件验收过的 38 个模板符号。`cnum32_from_cnum64()` 覆盖 EMPTY、
  满低位周期和短弧精确投影；`cnum64_cnum32_intersect()` 覆盖空输入、b1 环绕的尾裁/不可调整、
  b1 非环绕的空交/首裁/两类尾裁及最终返回。参数、按值 ownership、返回过近似和 u32 溢出前提齐全。
- 英文注释与实体验收：版权豁免；总布局、旋转参考系、必要 u32 溢出、五组 ASCII 分支图及
  “No adjustments”文字全部原样保留并紧邻翻译补充。实体清单覆盖 b1 旋转低位圆弧、t 候选结果、
  d 尾裁长度和 b1_max 回绕端点。仅凭注释可说明 b 为何每 2^32 重复、结果为何只裁首尾而可能保留
  中间空洞、size>=U32_MAX 为何投影全覆盖，以及每一幅图如何改变 t。
- 并发/生命周期与抽查：两个函数均为按值纯计算，无锁/RCU/引用/发布/回滚和睡眠。复杂函数不足
  三个，已全部抽查：投影函数可推导闭弧成员数 size+1；跨位宽函数可从 b1 环绕与否恢复所有返回。
  以 6 位主圆/3 位低圆缩小模型穷举 238,065 对规范/EMPTY 输入，验证投影与收紧结果均覆盖真实
  `{v in a | low(v) in b}`，全部通过。
- 关联读取：`kernel/bpf/cnum_defs.h` 模板（已完成学习注释）；`include/linux/cnum.h` 公开结构/哨兵
  （注释缺失）；`kernel/bpf/verifier.c` 的 `deduce_bounds_32_from_64()`/
  `deduce_bounds_64_from_32()`（确认调用位置，注释缺失，已列入后序）；引入提交 `256f0071f9b6`
  的说明与布局（确认本函数补足旧推导 A/B/C 场景）。目录外头文件建议另立任务补注。
- 修改安全：首次局部检查发现原 ASCII 图缩进被补丁上下文改变，已立即按 HEAD 精确恢复；最终 diff
  为新增 56 行、删除 0 行，原代码和原注释零改动。密度门禁
  `code=47, comments=122, chinese=44, density=0.936, max_gap=4` 通过；`git diff --check` 通过，
  增量 checkpatch 0 errors/27 个中文 UTF-8 视觉行宽 warnings。无 `.config` 且 GNU Make 版本不足，
  未执行对象构建；缩小位宽穷举已验证算法陈述。

### `kernel/bpf/range_tree.h`

- 状态：全文件完成；已按方法论第 17 章完成强制验收。
- 文件职责与验收：完整复读最终 75 行。5 个公开函数声明 5/5 具有紧邻契约，明确业务背景、全部
  参数单位/ownership、0/-ENOENT/-ESRCH/-ENOMEM/-EFAULT 返回类别和外部锁前提；`struct range_tree`
  及两个根字段说明地址 interval tree 与长度 best-fit tree 的同节点不变量。英文两条 root 注释原样
  保留、紧邻翻译；头文件无函数体，复杂函数抽查不适用。
- 路径与并发：仅凭接口注释可复述 init→set 全 free→find+clear 分配→set 释放→destroy；明确 find
  不预留，必须与 clear 同处 `bpf_arena.spinlock` 临界区，结构自身不加锁。节点由实现分配/释放，
  rt 本体始终归 arena；clear/set 的部分失败保证已标注。
- 关联读取：`kernel/bpf/range_tree.c` 全实现（注释缺失，紧接后序）；`kernel/bpf/arena.c` 的结构、
  map 构造/回滚、页分配/free/guard/worker 调用区域（确认单位、rqspinlock 与调用序列，注释缺失，
  已列入后序）。
- 修改安全：新增 56 行、删除 0 行，原声明与原注释零改动；密度门禁
  `code=13, comments=60, chinese=40, density=3.077, max_gap=2` 通过；`git diff --check` 通过，
  增量 checkpatch 0 errors/8 个中文 UTF-8 视觉行宽 warnings。头文件未单独构建，原因同前。

### `kernel/bpf/range_tree.c`

- 状态：全文件完成；已按方法论第 17 章完成强制验收。
- 文件职责与函数清单：完整复读最终 419 行。11 个显式函数定义 11/11 均有紧邻专属契约；覆盖
  rb 节点还原、闭区间长度、内部/公开 best-fit、长度树插入、双索引插入/摘除、区间查询、clear、
  is_set、set、destroy/init。`INTERVAL_TREE_DEFINE` 生成组单独解释参数、增强字段和外锁；START/LAST
  宏及 `range_node` 两链接、闭区间端点、subtree_last 字段全部覆盖。
- 英文注释与路径验收：版权和纯 ASCII 图豁免；文件总览、best-fit、长度树插入、clear/set/is_set
  及其 11 条分支说明全部原样保留并紧邻翻译补充。发现总览称 best-fit 找“小于等于请求大小”与
  `len <= rn_size()` 相反，保留原文后追加修正：长度树大值在左，合格后向右，实际寻找最小的
  `size>=len` 节点。仅凭注释可复述 init/set 全空闲、find+clear 分配、set 释放、destroy 回收主线。
- 并发/生命周期与抽查：所有树操作依赖 arena rqspinlock，双索引始终共享节点集合；range_node 由
  kmalloc_nolock 创建、摘除后 kfree_nolock，find 返回借用且不预留。抽查 clear 可恢复严格包含拆分、
  左裁、右裁、整段删除及拆分 -ENOMEM 的“右半容量丢失但不重复分配”；抽查 set 可恢复已覆盖快路、
  先清重叠、左右邻接诊断、四种合并/新建和失败非事务性；抽查长度树可推导大值在左、合格向右的
  best-fit 方向。关键端点与 ownership 变化均有阶段注释。
- 关联读取：`kernel/bpf/range_tree.h`（已完成学习注释）；`kernel/bpf/arena.c` 的 rt 字段、map 构造/
  回滚、fault、alloc/free/guard/worker 调用（确认槽单位、rqspinlock、find+clear 原子性和失败处理，
  注释缺失，已列入后序）；`include/linux/slab.h` 的 kmalloc_nolock 契约及 `mm/slub.c` 的 nolock
  限制/实现（确认只允许 GFP 子集、NULL=-ENOMEM、不可期待重试及上下文边界；前者缺失，后者部分覆盖）。
- 修改安全：新增 157 行、删除 0 行，原代码与原英文/ASCII 注释零改动；密度门禁
  `code=182, comments=206, chinese=120, density=0.659, max_gap=8` 通过；`git diff --check` 通过，
  增量 checkpatch 0 errors/65 个中文 UTF-8 视觉行宽 warnings。无可用构建环境，未编译对象。

### `kernel/bpf/cfg.c`

- 状态：全文件完成；已按方法论第 17 章完成强制验收。
- 文件职责与函数清单：完整复读最终 1211 行。20 个函数定义 20/20 均有紧邻专属契约；覆盖三类
  子程序副作用标记与调用图合并、DFS 边推进、调用/普通跳转/gotox/隐藏退出分派、跳转表分配/读取/
  排序/合并/边界验证、整图结构检查、逐子程序后序和 Tarjan 强连通分量。两个枚举的 DFS 颜色、
  边进度和驱动返回态逐项说明；宏、柔性数组、错误指针、显式栈及重要局部变量均已登记。
- 英文注释与路径验收：SPDX/版权和论文题名豁免；非递归 DFS 伪代码、callee 属性稳定条件、map
  查询保证、gotox 表筛选限制、异常退出、回调/迭代器收敛、后序切片、Tarjan 不变量与递归伪代码等
  英文说明均逐字保留并有紧邻中文翻译与机制补充。仅凭注释可复述主入口/异常入口 DFS、树边/回边/
  交叉边分类、ldimm64 第二槽禁跳、跳转表发布、后序“后继先于前驱”以及 SCC 根的判定与弹栈。
- 并发/生命周期与抽查：验证线程独占 env，本文件无锁/RCU；局部 DFS/后序/Tarjan 工作数组由当前
  函数持有并在失败或完成时释放，jt/postorder/scc_info 成功后转交 env 统一销毁。抽查
  `visit_insn()` 可按指令类别恢复全部后继和检查点副作用；抽查 `bpf_check_cfg()` 可恢复两根 DFS、
  不可达与 ldimm64 检查及统一回滚；抽查 `bpf_compute_scc()` 可用 pre/low/活动栈推导多顶点、自环和
  隐式回调循环编号，且能指出 scc_info 分配失败后 aux.scc 非事务性写入由整体验证失败兜底。
- 关联读取：`include/linux/bpf_verifier.h` 中 `bpf_verifier_env.cfg`、`bpf_subprog_info`、
  `bpf_insn_aux_data.jt/scc` 及 `bpf_iarray`（确认字段 ownership 与消费者，现有学习注释部分覆盖）；
  `kernel/bpf/verifier.c` 中 CFG 三入口调用和环境清理（确认调用顺序与最终释放，学习注释缺失，已列入
  后序）；`kernel/bpf/liveness.c` 的 `bpf_insn_successors()`（确认后继表的借用/复用语义，学习注释缺失，
  紧接后序）；`kernel/bpf/bpf_insn_array.c` 的 `insn_array_lookup_elem()`/init（确认 array map 槽位与
  xlated_off 稳定条件，学习注释缺失，已列入后序）。目录外头文件建议另立任务补足公开数据结构契约。
- 修改安全：本轮新增 98 行、删除 0 行，原代码和原注释零改动；密度门禁
  `code=559, comments=563, chinese=254, density=0.454, max_gap=10` 通过；`git diff --check` 通过，
  增量 checkpatch 为 0 errors/52 个中文 UTF-8 视觉行宽 warnings。宿主 GNU Make 3.81 低于源码要求
  4.0，且工作树无可用 `.config`，未执行目标对象构建；纯注释追加不改变预处理结果。

### `kernel/bpf/states.c`

- 状态：全文件完成；已按方法论第 17 章完成强制验收。
- 文件职责与函数清单：完整复读最终 2090 行。37 个函数定义 37/37 及 2 个内部前置声明均有紧邻
  专属契约；覆盖状态父链和延迟释放、SCC callchain/visit/backedge、ID 重命名、死值清洗、寄存器/
  栈/出栈参数/引用安全包含、精度固定点、循环候选和状态缓存主入口。`exact_level` 三种模式、静态
  `unbound_reg`、ID scratch、各类栈槽和关键统计字段均已登记。
- 英文注释与路径验收：SPDX/版权豁免；父状态容器、SCC 进入/退出、ID 映射、标量链接、死半槽、
  类型严格性、包指针 range、栈包含、迭代器 depth、推测状态、精度传播以及主入口各启发式说明均
  原样保留并有紧邻中文翻译与机制补充。仅凭注释可复述“清死值→扫描缓存→区分未完成循环与已
  完成安全状态→回灌精度/SCC 回边→淘汰低收益状态→保存新检查点”的完整主线。
- 并发/生命周期与抽查：验证线程独占 env，无运行期锁/RCU；explored/free 节点以 branches 和未完成
  SCC 标记延迟释放，visit 按 callchain 隔离 parent 链寿命，backedge 登记后转移 ownership，固定点
  完成或 free_states() 统一释放。抽查 `regsafe()` 可按标量、内存/包/栈/arena/insn 指针恢复安全
  包含方向和 ID 约束；抽查 `stacksafe()` 可恢复 POISON 归一化、半槽合成和特殊资源槽比较；抽查
  `bpf_is_state_visited()` 可恢复普通死循环、iterator/may_goto/callback 收敛及缓存发布/回滚；抽查
  `propagate_backedges()` 可恢复 64 轮上限和保守精度退化。审计还记录当前 `scc_visit_lookup()` 在
  NULL 检查前形成 `info->visits` 成员地址的源码前提，未在注释任务中擅改代码。
- 关联读取：`include/linux/bpf_verifier.h` 的 ID/SCC/环境字段（确认容量、柔性数组和所有权）；
  `kernel/bpf/verifier.c` 的 prune-point 调用、路径退出、`bpf_free_backedges()` 与 `free_states()`（确认
  发布和最终回收）；`kernel/bpf/liveness.c` 的 live-stack 查询（确认跨帧查询输入，紧接后序）；
  `kernel/bpf/backtrack.c` 的 `bpf_mark_chain_precision()`（确认当前状态精度可延后、parent 才是持久
  传播目标，紧接后序）。目录外头文件仍建议另立任务补足公开结构契约。
- 修改安全：最终新增 491 行、删除 0 行，原代码和原英文注释零改动；密度门禁
  `code=910, comments=1034, chinese=387, density=0.425, max_gap=10` 通过；`git diff --check` 通过，
  增量 checkpatch 为 0 errors/202 个中文 UTF-8 视觉行宽 warnings。宿主 GNU Make 3.81 低于源码要求
  4.0，且工作树无可用 `.config`，未执行目标对象构建；纯注释追加不改变预处理结果。

### `kernel/bpf/liveness.c`

- 状态：全文件完成；已按方法论第 17 章完成强制验收。
- 文件职责与函数清单：完整复读最终 3083 行。63 个函数定义 63/63 均有紧邻专属契约；覆盖函数实例
  建模、逐帧活跃栈位图、指令后继/固定点、跨子程序参数反向追踪、实例合并，以及逐指令活跃寄存器
  发布。`per_frame_masks`、`func_instance`、`live_stack_query`、`bpf_liveness`、`arg_track` 联合体及其
  状态枚举、`subprog_at_info`、`insn_live_regs`、容量宏和关键工作数组均已登记。
- 英文注释与路径验收：SPDX/版权豁免；实例散列、相对指令号、栈位读写、后继枚举、参数偏移集合、
  spill/fill、回调子程序、固定点和活跃寄存器等英文说明均逐字保留并有紧邻中文翻译与机制补充。
  仅凭注释可复述“建立调用实例→反向传播栈需求→计算子程序参数摘要→合并等价实例→正向发布栈
  活跃区间→反向发布寄存器活跃集”的主线；并注明上游旧英文中的 `live_regs` 在当前结构实际对应
  `live_regs_before`，避免照抄过时字段名。
- 并发/生命周期与抽查：验证线程独占 env，无运行期锁/RCU；临时状态、参数追踪数组和偏移集合由
  当前分析持有并沿错误路径释放，成功实例挂入 `env->liveness`，最终由 `bpf_stack_liveness_free()`
  统一回收，查询对象只借用位图。初学者抽查 `arg_track_xfer()` 可按 load/store/ALU/call 恢复“本条
  指令怎样把出口需求搬回入口”；开发者抽查可推导不精确状态的保守合并、重叠 spill 清除和偏移溢出
  退化。抽查 `compute_subprog_args()` 可复述参数摘要固定点，并推导递归调用实例、回调入口及失败回滚；
  抽查 `analyze_subprog()` 可复述后序遍历和逐帧位图传播，并推导等价实例合并的单调性；另抽查
  `bpf_compute_live_registers()` 可恢复反向数据流、调用 clobber 与 `live_regs_before` 的发布边界。
- 关联读取：`kernel/bpf/states.c` 的 `__clean_func_state()`/`clean_verifier_state()`（确认栈位查询和
  `live_regs_before` 消费者，现有学习注释充分）；`kernel/bpf/cfg.c` 的 `bpf_insn_successors()` 相关
  gotox/后序语义（现有学习注释充分）；`kernel/bpf/verifier.c` 的 CFG→活跃性→`do_check` 调用顺序、
  helper/kfunc 栈访问和清理路径（契约已核对，学习注释缺失，已列入后序）；
  `include/linux/bpf_verifier.h` 的活跃寄存器、调用栈和 CFG 字段（部分覆盖，目录外建议另立任务补足）。
- 修改安全：最终新增 787 行、删除 0 行，原代码和原英文注释零改动；密度门禁
  `code=1736, comments=1124, chinese=632, density=0.364, max_gap=10` 通过；`git diff --check` 通过，
  增量 checkpatch 为 0 errors/325 个中文 UTF-8 视觉行宽 warnings。全文件 checkpatch 的 3 errors/
  1 warning 分别来自原有同一行 `case` 语句和 `kvzalloc(sizeof(*f))`，均可在 HEAD 原文定位。本机
  GNU Make 3.81 低于源码要求 4.0，且无 `.config`，目标对象构建不可用；纯注释追加不改变编译结果。

### `kernel/bpf/backtrack.c`

- 状态：全文件完成；已按方法论第 17 章完成强制验收。
- 文件职责与函数清单：完整复读最终 1148 行。26 个函数定义 26/26 均有紧邻专属契约；覆盖跳转历史
  追加/合并、真实前驱恢复、逐帧寄存器/普通栈/出栈参数位图、日志格式化、单指令反向传递、全标精确
  兜底及 parent 状态链主循环。`verbose` 宏、`backtrack_state` 三类位图的本文件用法、history 字段、
  指令解码字段、槽号/帧号和临时 bitmap 均已登记。
- 英文注释与路径验收：SPDX/版权豁免；跳转历史循环边界、逐指令回溯、标量精度算法、跨帧示例、
  当前状态无需 precise 的长篇论证、状态剪枝路径无关性及保守兜底说明均逐字保留并有紧邻中文翻译与
  机制补充。仅凭注释可复述“精度使用点置需求→沿真实历史逆行→在 MOV/ALU/load/store/call/exit
  转移位图→跨 parent 提交 precise→定义点清空或全标兜底”的完整主线；旧注释所称 spi 入参在当前
  接口实际由调用者预置 `env->bt`，已在入口契约澄清。
- 并发/生命周期与抽查：验证线程独占 env，无运行期锁/RCU；`jmp_history` 由 state 持有，krealloc
  失败保留旧指针，`hist` 只借用；`env->bt` 是不分配内存的可复用工作区，成功时需求清空，未支持
  指令走全标精确并 reset，致命一致性错误交由验证失败清理。初学者抽查 `backtrack_insn()` 可按
  指令类型复述出口需求怎样变成入口需求；开发者抽查可推导 linked_regs 两次同步、栈比较 flags、
  静态/全局子程序与同步回调的不同帧语义。抽查 `bpf_mark_chain_precision()` 可复述双层“状态链×真实
  指令路径”循环，并推导当前短命状态不置 precise、父检查点固定点和全局入口边界；抽查
  `bpf_mark_all_scalars_precise()` 可说明为何跳过当前状态、覆盖全部父帧且只处理标量/spill。
- 关联读取：`include/linux/bpf_verifier.h` 的 `backtrack_state`、三类置位/查询助手和公开原型（确认
  位宽、帧容量与 env 内嵌 ownership，学习注释缺失，目录外建议另立任务）；`kernel/bpf/verifier.c` 的
  `bpf_bt_sync_linked_regs()`、精度触发点、栈访问/条件分支 history 生产者（确认等价寄存器和 flags
  语义，学习注释缺失，已列入后序）；`kernel/bpf/states.c` 的 `propagate_precision()`（确认批量预置
  env->bt 及 changed 消费，现有学习注释充分）。
- 修改安全：最终新增 164 行、删除 0 行，原代码和原英文注释零改动；密度门禁
  `code=583, comments=491, chinese=138, density=0.237, max_gap=10` 通过；`git diff --check` 通过，
  增量 checkpatch 为 0 errors/114 个中文 UTF-8 视觉行宽 warnings。全文件 checkpatch 的 1 error/
  2 warnings 来自 HEAD 原有续行空格缩进、return 后 else 和字符串换行前空格。本机 GNU Make 3.81
  低于源码要求 4.0，且无 `.config`，目标对象构建不可用；纯注释追加不改变编译结果。

### `kernel/bpf/const_fold.c`

- 状态：全文件完成；已按方法论第 17 章完成强制验收。
- 文件职责与函数清单：完整复读最终 503 行。10 个函数定义 10/10 均有紧邻专属契约；覆盖常量格
  判定、单指令传递、后继汇合、逆后序固定点、条件求值和死分支改写。`const_arg_state` 六个枚举项、
  `const_arg_info` 三字段、三类 aux 掩码、二维工作矩阵、入口/出口快照及 CFG 后继均已登记。
- 英文注释与路径验收：SPDX/版权豁免；文件级前向分析说明、枚举逐项语义、transfer/join、清零初态、
  子程序入口、32 位发布限制、分支改写目的及 JMP32 两段有序映射均逐字保留并有紧邻中文翻译与机制
  补充。仅凭注释可复述“入口设 UNKNOWN→逆后序传递/汇合到固定点→发布 R0-R9 常量/逻辑指针→
  恒真/恒假分支改写→CFG 后序重算”的主线。
- 并发/生命周期与抽查：验证线程独占 env；只读 map 必须同时满足程序侧只读、冻结且无活动写者，
  才由 direct-read 借用稳定 value 内容，无额外锁/RCU。`ci_in` 由计算入口分配并在发布按值副本后释放；
  prune 改写程序指令并在有变化时先释放旧 postorder，重算失败由整体验证失败清理且不局部回滚。
  初学者抽查 `const_reg_xfer()` 可按 MOV/ADD/SUB/AND/LD/LDX/call/atomic 复述定义和 clobber；开发者
  抽查可推导 map-value 直接读取的不可变前提、ALU32 零扩展及 UNKNOWN 保守性。抽查
  `bpf_compute_const_regs()` 可复述逆后序固定点，并推导格的单调收敛及多子程序入口；抽查
  `bpf_prune_dead_branches()` 可复述恒真/恒假 offset，推导 may_goto 豁免、JMP32 有符号归一化和 CFG
  ownership 更新。
- 关联读取：`kernel/bpf/verifier.c` 的 `bpf_map_is_rdonly()`/`bpf_map_direct_read()`、helper/kfunc 栈
  大小消费者、`do_check` 常量一致性断言和顶层 pass 顺序（确认冻结/无并发写前提及 aux 消费，学习
  注释缺失，已列入后序）；`kernel/bpf/liveness.c` 的回调函数指针消费者（现有学习注释充分）；
  `kernel/bpf/cfg.c` 的后继/后序生产者（现有学习注释充分）；`include/linux/bpf_verifier.h` 的三个
  mask 与十项 vals 契约（英文契约充分但中文缺失，目录外建议另立任务）。
- 修改安全：最终新增 99 行、删除 0 行，原代码和原英文注释零改动；密度门禁
  `code=337, comments=135, chinese=87, density=0.258, max_gap=10` 通过；`git diff --check` 通过，
  增量 checkpatch 为 0 errors/49 个中文 UTF-8 视觉行宽 warnings。全文件 checkpatch 的 4 errors 均
  来自 HEAD 原有 switch 同行语句。本机 GNU Make 3.81 低于源码要求 4.0，且无 `.config`，目标对象
  构建不可用；纯注释部分不改变编译结果，分支改写代码保持原样。

### `kernel/bpf/fixups.c`

- 状态：首次建图中；当前仅切换唯一目标，尚未声称完成。
