# kernel/sched 学习注释任务清单

## 使用规则

本清单是 `kernel/sched` 目录详尽学习注释长任务的持久化进度源。发生会话中断或上下文压缩后，
先读取本文件、`doc/linux-kernel-source-learning-methodology.md` 并核对当前工作树，不凭对话记忆推断
进度。

严格采用单文件闭环：

1. 开始前把目标文件标为 `[~]`，完整读取并建立函数、实体、英文注释和语义索引。
2. 仅追加中文学习注释；不改代码，不删除、改写或移动任何原有代码与原有注释。
3. 非豁免英文注释必须保留原文，并在紧邻位置补充完整翻译、上下文和机制说明；新增注释不使用
   `中文说明：`、`背景知识：` 等只标识语言或用途的模板前缀。
4. 按源码顺序以 1～5 个函数为一批修改；每批修改前读取最新连续窗口，修改后复读窗口并检查局部
   diff，避免函数头错位或补丁覆盖原内容。
5. 完整复读修改后的目标文件，按方法论第 17 章核对函数、实体、英文注释、控制路径、并发、生命周期
   和关联读取清单，再执行追加式安全审计、格式检查及可用的构建检查。
6. 只有单文件内容验收全部通过后才标为 `[x]` 并登记验收摘要；一个 `[~]` 文件未闭环前不开始
   下一个文件。
7. 为核对契约而读取的关联源码只登记到当前文件记录；读取关联文件不等于授权修改。
8. 本清单覆盖创建时 `kernel/sched` 下的全部 51 个文件：46 个进入计划，5 个按用户要求排除。
   后续若目录新增文件，先登记到“范围变化”再决定学习顺序，不能静默遗漏。

状态：`[ ]` 未开始；`[~]` 正在处理或验收；`[x]` 已按第 17 章完成全文件闭环；`[-]` 明确排除。

## 文件进度（按建议学习顺序）

### 构建入口、公共数据结构与调度特性

- [x] `kernel/sched/Makefile`
  - 文件职责：定义调度目录的编译期上下文分析、KCOV/KCSAN、帧指针和分支跟踪边界，并把调度器
    组织为 `core.o`、`fair.o`、`build_policy.o`、`build_utility.o` 四个规模接近的内建编译单元；后两者
    通过包含多份 `.c` 摊薄头文件解析成本，父级再把 `kernel/sched/built-in.a` 链入 `vmlinux`。
  - 第 17 章验收：完整复读最终 76 行。文件没有函数、结构体、运行时控制流、ownership 或资源释放，
    函数清单和三个复杂函数抽查不适用；实体清单覆盖 2 个上下文分析目标、目录级告警/KCOV/KCSAN
    变量、2 个配置分支、3 组目标级 CFLAGS 和 4 个 `obj-y` 条目。所有非豁免英文注释均逐字保留，
    并有紧邻完整翻译及当前 Kbuild 语义补充。
  - 路径与配置验收：可仅凭注释复述 KCOV 为何排除异步覆盖噪声、KCSAN 为何关闭访存插桩却保留
    屏障建模、`SCHED_OMIT_FRAME_POINTER` 两侧如何决定 `core.o` 的帧指针参数、启用分支分析时两个
    聚合单元如何通过 `DISABLE_BRANCH_PROFILING` 避免 noinstr 不安全插桩，以及四目标的归档去向。
  - 关联读取：`scripts/Makefile.lib` 的 KCOV/KCSAN/context-analysis flag 选择，
    `scripts/Makefile.context-analysis` 与 `scripts/Makefile.compiler` 的具体编译选项和告警探测，
    `Documentation/kbuild/makefiles.rst` 与 `scripts/Makefile.build` 的 `ccflags-y`/目标 CFLAGS/`obj-y` 规则，
    `kernel/Makefile` 的 `sched/` 入口，`build_policy.c`/`build_utility.c` 的聚合内容，六个体系结构 Kconfig
    中的帧指针配置，以及 `include/linux/compiler.h` 的分支跟踪门控；均只读未修改。`core.c` 已有充分
    学习导读，`compiler.h` 所读区域部分覆盖，其余关联区域缺失或仅有上游英文；计划内聚合文件后续
    继续补注，目录外 Kbuild/Kconfig 文档不在本任务修改范围。
  - 修改安全：新增 34 行、删除 0 行；过滤注释和空行后全部 Kbuild 语句与 `HEAD` 逐行一致；禁用
    模板前缀扫描无命中，`git diff --check` 通过，完整文件 checkpatch 为 0 errors/0 warnings。工作树
    无 `.config`，未执行目标内核配置构建。已按方法论第 17 章完成强制验收，状态为“全文件完成”。
- [ ] `kernel/sched/sched.h`
- [x] `kernel/sched/smp.h`
  - 文件职责：定义通用 SMP call-single 队列与调度器之间的内部接口，使普通跨 CPU 回调和远端 task
    唤醒可以共用每 CPU `call_single_queue`，并约定目标 CPU 通过 IPI、polling idle 或 migration
    stopper 消费队列。
  - 第 17 章验收：完整复读最终 70 行。函数/声明清单覆盖 `sched_ttwu_pending(void *)`、
    `call_function_single_prep_ipi(int)`、SMP 版 `flush_smp_call_function_queue()` 外部实现和 UP 版空
    stub；4/4 均有紧邻专属契约，逐项说明参数、可空性、返回类别、中断/任务上下文、可睡眠性、
    队列节点与 task/CSD ownership、可观察副作用和配置差异。文件无结构体、全局变量或复杂函数体，
    三个复杂函数抽查不适用。
  - 英文与路径验收：许可证豁免；唯一英文总览逐字保留，并有紧邻翻译和调用链补充。仅凭注释可
    复述 TTWU 链表在目标 CPU 批量激活及延后清 pending、polling idle 通过 need_resched 省 IPI、
    非 polling 目标继续发硬件 IPI、SMP 主动 flush 的关中断/恢复与 SYNC→ASYNC/IRQ_WORK→TTWU
    次序，以及 UP 配置为何安全退化为空操作。
  - 关联读取：`kernel/sched/core.c` 的 `sched_ttwu_pending()`、
    `call_function_single_prep_ipi()` 和 migration stopper 调用点，`kernel/smp.c` 的发送、IPI 接收、
    分阶段队列消费及公开 flush 实现，`kernel/sched/idle.c` 的 polling idle 承诺和主动 flush 调用；
    均只读未修改，所读实现区域已有充分中文学习注释。`idle.c` 仍按用户要求排除，不因关联读取
    重新纳入修改范围。
  - 修改安全：新增 48 行、删除 0 行，diff 中全部新增行均位于注释，原 22 行逐行保留；禁用模板
    前缀扫描无命中，`git diff --check` 通过，完整文件 checkpatch 为 0 errors/0 warnings。工作树无
    `.config`，未执行配置构建。已按方法论第 17 章完成强制验收，状态为“全文件完成”。
- [x] `kernel/sched/features.h`
  - 文件职责：作为无 include guard 的 X-macro 特性表，被 sched.h/core.c/debug.c 以不同
    `SCHED_FEAT(name, default)` 定义重复展开，生成 29 个逻辑特性的枚举、默认位图、名称、查询 helper
    与 static-key 初值；它声明全局实验策略及启动默认值，不直接执行调度或提供任务级配置。
  - 第 17 章验收：完整复读最终 235 行。33 个物理宏调用（含 HRTICK、TTWU_QUEUE、RT_PUSH_IPI 的
    配置互斥默认值）全部有用途、收益/代价或诊断边界说明；覆盖 EEVDF 放置/抢占、buddy/cache、延迟
    出队、精确 tick、容量/远端唤醒、RT、负载均衡/wake-affine、util_est 和 newidle。文件无函数体、
    结构体或变量定义，三个复杂函数抽查不适用；改为抽查 PLACE_LAG/DELAY_DEQUEUE、TTWU_QUEUE/
    RT_PUSH_IPI、NI_RANDOM/NI_RATE 三组，可仅凭注释复述状态影响、配置分支和退化路径。
  - 英文与配置验收：全部 17 个非许可证英文说明逐字保留，并有紧邻完整翻译和当前实现补充。
    `CONFIG_HRTIMER_REARM_DEFERRED` 改变 HRTICK 两项默认值，`CONFIG_PREEMPT_RT` 改变 TTWU_QUEUE 和
    RT_PUSH_IPI 默认值，`HAVE_RT_PUSH_IPI` 决定后者是否进入枚举；关闭条件不会留下无实现条目。
  - 并发与生命周期：debug 控制面以 NAME/NO_NAME 修改全局 feature 位，并在 jump-label 配置下同步
    static key；热路径只消费随后观察到的策略，不获得 task/rq ownership，也没有跨多项事务回滚。
    无 jump label 时直接读取全局位图。X-macro 的文本顺序固定枚举/位号，不能随意重排或插入代码。
  - 关联读取：`kernel/sched/sched.h` 的枚举、static helper 和 `sched_feat()` 两种实现；
    `kernel/sched/core.c` 的默认位图与 NONTASK_CAPACITY/TTWU_QUEUE/WARN/LATENCY 消费；
    `kernel/sched/debug.c` 的名称、static-key 数组和写入同步；`kernel/sched/fair.c`、`rt.c`、`pelt.h`
    的各特性消费点。均只读未修改；core.c 相关入口已有部分/充分学习注释，其余区域后续按清单闭环。
  - 修改安全：新增 91 行、删除 0 行，原 144 行逐行保留，新增行均为注释或空行；禁用模板前缀扫描
    无命中，`git diff --check` 通过。原始 checkpatch 仅有 47 个中文 UTF-8 字节长度导致的
    `LONG_LINE_COMMENT`，忽略后为 0 errors/0 warnings。工作树无 `.config`，未编译依赖该表的对象，
    静态检查不能证明所有配置组合的宏展开。已按方法论第 17 章完成强制验收，状态为“全文件完成”。
- [x] `kernel/sched/rq-offsets.c`
  - 文件职责：作为构建期类型布局转接器，使用目标编译器把完整内部 `struct rq` 的 `nr_pinned`
    字段偏移编码进 `rq-offsets.s`，再由 Kbuild 提取为 `include/generated/rq-offsets.h`；公共
    `include/linux/sched.h` 因只持有 `struct rq` 前置声明，借此偏移访问当前 CPU 的该字段。
  - 第 17 章验收：完整复读最终 53 行。唯一函数 `main(void)` 有紧邻专属契约，明确无入参、构建期
    上下文、不会链接或运行、无锁/睡眠/ownership/运行时失败路径、返回 0 不被消费，以及真正输出是
    `DEFINE()` 汇编标记；函数体两阶段说明覆盖 `offsetof()` 常量求值、标记发布和形式返回。
  - 实体与路径验收：覆盖 `COMPILE_OFFSETS` 打破生成头循环依赖的作用、`linux/kbuild.h` 中
    `DEFINE()` 的 `.ascii` 编码、目标 ABI 类型来源、内部 `sched.h` 的完整结构布局，以及
    `rq-offsets.c → rq-offsets.s → rq-offsets.h → this_rq_pinned()` 全链路。原文件除 SPDX 外没有英文
    注释，许可证按规则豁免；文件没有结构体定义、运行时快速/慢速/回滚路径或三个复杂函数可抽查。
  - 关联读取：顶层 `Kbuild` 的生成目标和 prepare 依赖，`scripts/Makefile.lib` 的 offsets sed/filechk
    规则，`include/linux/kbuild.h` 的 `DEFINE()` 实现，`include/linux/sched.h` 的生成头门控、
    `this_rq_raw()`/`this_rq_pinned()` 与 offsets 编译 stubs，以及 `kernel/sched/sched.h` 中 `struct rq` 的
    `nr_pinned` 字段；均只读未修改。公共 sched 头所读区域已有部分学习注释，内部 `sched.h` 缺失，
    后者仍保留在本清单待处理。
  - 修改安全：新增 41 行、删除 0 行，diff 中全部新增行均位于注释；原 12 行源码逐行保留，禁用模板
    前缀扫描无命中，`git diff --check` 通过，完整文件 checkpatch 为 0 errors/0 warnings。工作树无
    `.config`，未执行目标架构的 offsets 重新生成。已按方法论第 17 章完成强制验收，状态为
    “全文件完成”。
- [x] `kernel/sched/build_policy.c`
  - 文件职责：把 idle、RT、DL、PELT、CPU 时间、sched_ext 和调度系统调用实现文本合并为
    `build_policy.o`，与独立的 core.o/fair.o 形成规模接近的并行构建单元，摊薄公共头文件解析成本；
    它是构建组织边界而非运行时调用顺序或调度类优先级。
  - 第 17 章验收：完整复读最终 122 行。文件没有自身函数、结构体、变量或运行时控制流，函数清单
    和三个复杂函数抽查不适用；实体清单覆盖 6 个 sched 专用头、15 个通用基础设施头、UAPI、2 个
    核心内部头、3 个局部契约头、7 个常驻 `.c` 成员及 sched_ext 条件分支中的 3 个公共/5 个局部头
    和 4 个实现成员。仅凭注释可复述每组依赖职责、文本合并语义、静态符号命名空间和重编译代价。
  - 英文与配置验收：顶部聚合说明、`Headers`、`Source code modules` 原文逐字保留，均有紧邻完整
    翻译和机制补充；许可证豁免。路径覆盖 `CONFIG_SCHED_CLASS_EXT` 开/关两种预处理结果、成员展开
    次序、最终 build_policy.o→sched/built-in.a 链接去向，以及 Makefile 的
    `DISABLE_BRANCH_PROFILING` noinstr 边界；文件无运行时成功/失败/回滚/释放路径。
  - 关联读取：`kernel/sched/Makefile` 的对象列表、聚合构建说明与目标级 CFLAGS；各 include 文件名及
    `kernel/sched/ext` 成员边界。Makefile 已有充分学习注释；成员实现仅为确认职责/顺序而搜索，
    未修改，后续仍按清单各自闭环，`idle.c` 保持用户明确排除。
  - 修改安全：新增 47 行、删除 0 行，原 75 行逐行保留，新增行均为注释；禁用模板前缀扫描无命中，
    `git diff --check` 通过。原始 checkpatch 仅有 26 个中文 UTF-8 字节长度导致的
    `LONG_LINE_COMMENT`，忽略后为 0 errors/0 warnings。工作树无 `.config`，未编译 build_policy.o；
    静态注释检查不能证明所有配置下聚合对象可构建。已按方法论第 17 章完成强制验收，状态为
    “全文件完成”。
- [x] `kernel/sched/build_utility.c`
  - 文件职责：把调度时钟、CPU/调度统计、等待协议、RT CPU 优先级、拓扑、PSI、membarrier、CPU
    隔离与自动分组等横向设施文本合并为 `build_utility.o`，与其他大型调度对象均衡并行构建并摊薄
    公共头解析成本；文本顺序不是运行时调用链或初始化承诺。
  - 第 17 章验收：完整复读最终 150 行。文件无自身函数、结构体、变量和运行时路径，函数清单及
    三个复杂函数抽查不适用；实体清单覆盖 9 个 sched 专用头、24 个通用基础设施头、2 个 UAPI、
    1 个架构头、4 个内部头、10 个常驻实现成员及 9 个条件编译实现成员。仅凭注释可说明各组职责、
    `.c` 文本合并、共享 static 命名空间、成员重编译范围和目标对象链接边界。
  - 英文与配置验收：顶部聚合说明逐字保留并有紧邻完整翻译和机制补充；许可证豁免。配置路径覆盖
    CPUACCT、CPU_FREQ、SCHEDUTIL、SCHEDSTATS、SCHED_CORE、PSI、MEMBARRIER、CPU_ISOLATION、
    SCHED_AUTOGROUP 的开/关结果，以及 Makefile 对对象施加的 `DISABLE_BRANCH_PROFILING`；无运行时
    成功/失败/回滚/释放路径。
  - 关联读取：`kernel/sched/Makefile` 的聚合目标、构建均衡说明和目标级 CFLAGS；本文件所有 include
    路径及配置边界。Makefile 已有充分学习注释，各成员仅按文件职责定向核对、均未修改；
    `completion.c`、`swait.c`、`isolation.c` 继续保持用户明确排除，其余成员按清单后续闭环。
  - 修改安全：新增 44 行、删除 0 行，原 106 行逐行保留，新增行均为注释或空行；禁用模板前缀扫描
    无命中，`git diff --check` 通过。原始 checkpatch 仅有 24 个中文 UTF-8 字节长度导致的
    `LONG_LINE_COMMENT`，忽略后为 0 errors/0 warnings。工作树无 `.config`，未编译 build_utility.o；
    静态注释检查不能证明所有配置组合可构建。已按方法论第 17 章完成强制验收，状态为“全文件完成”。

### 系统调用、等待与跨 CPU 调度接口

- [ ] `kernel/sched/syscalls.c`
- [ ] `kernel/sched/wait.c`
- [ ] `kernel/sched/wait_bit.c`
- [ ] `kernel/sched/membarrier.c`
- [ ] `kernel/sched/clock.c`

### 调度类与运行队列策略

- [ ] `kernel/sched/fair.c`
- [ ] `kernel/sched/rt.c`
- [ ] `kernel/sched/deadline.c`
- [ ] `kernel/sched/stop_task.c`
- [ ] `kernel/sched/core_sched.c`
- [x] `kernel/sched/autogroup.h`
  - 文件职责：定义自动分组对象和配置无关内部调用面；对仍在 root CPU cgroup 且未退出的线程组，
    把有效 CFS `task_group` 映射为 `signal->autogroup->tg`，而真实 cgroup、RT 带宽及实际调度实体迁移
    分别由 cgroup/root RT 组和 `sched_move_task()` 负责。
  - 第 17 章验收：完整复读最终 151 行。`struct autogroup` 的 kref/tg/lock/id/nice 字段组、5 个启用
    配置接口和 5 个关闭配置 stub 均有紧邻契约，覆盖全部参数、借用/引用 ownership、锁、睡眠性、
    返回类别及副作用。文件只有两个极短 inline 查询和机械 stub，没有复杂函数可供三个抽查；改为
    复述 `task_wants_autogroup()`、`autogroup_task_group()` 与配置关闭回退，能够解释真实 cgroup 优先、
    PF_EXITING 边界、READ_ONCE 限度以及借用返回值的有效期。
  - 英文与路径验收：许可证及条件编译行尾标记豁免；唯一结构体英文注释逐字保留，并有紧邻完整翻译
    和 kref 生命周期补充。路径覆盖默认组启动发布、普通组最终释放、sysctl 开/关、root/非 root、
    task 退出、路径缓冲区截断语义，以及 CONFIG 关闭后的无副作用/原 tg 返回。
  - 并发与生命周期：`signal->autogroup` 的替换由 siglock 稳定线程列表并以 kref 保活，移动方在放弃
    旧引用前逐线程再次 `sched_move_task()`；每个 task 的 rq 锁保护 `sched_task_group` 及调度实体链接
    更新。最后一个引用触发 tg 从 RCU 索引摘除和延迟销毁，proc nice/read 只由对象 rwsem 串行化；
    `READ_ONCE(sysctl)` 不为 signal 指针提供一致快照。
  - 关联读取：`kernel/sched/autogroup.c` 全部实现；`kernel/sched/core.c` 的 `sched_cgroup_fork()`、
    `sched_change_group()`、`sched_move_task()` 和 task_group 两阶段释放；`kernel/sched/sched.h` 的
    `task_group.autogroup`；`include/linux/sched/autogroup.h` 的公开生命周期入口。均只读未修改；core.c
    所读区域已有充分/部分学习注释，其余相关区域缺失，autogroup.c 和 sched.h 已在后续计划中。
  - 修改安全：新增 83 行、删除 0 行，原 68 行逐行保留，新增行均为注释或空行；禁用模板前缀扫描
    无命中，`git diff --check` 通过。原始 checkpatch 仅有 30 个中文 UTF-8 字节长度导致的
    `LONG_LINE_COMMENT`，忽略后为 0 errors/0 warnings。工作树无 `.config`，头文件不是独立构建目标，
    未执行配置构建。已按方法论第 17 章完成强制验收，状态为“全文件完成”。
- [ ] `kernel/sched/autogroup.c`

### CPU 优先级、截止期与拓扑

- [x] `kernel/sched/cpupri.h`
  - 文件职责：定义每个 `root_domain` 的 RT CPU 优先级候选索引、101 个优先级桶和 CPU→桶反向表，
    供 RT 唤醒选核、push/pull 与同优先级抢占路径寻找“当前运行更低优先级实体”的 CPU；查询只
    生成可过期候选，不负责迁移、拓扑择优或目标 rq 最终复核。
  - 第 17 章验收：完整复读最终 124 行。两个结构体及全部字段、4 个特殊值/桶数宏和 5 个声明均有
    紧邻说明；函数头分别覆盖 `cpupri_find()`、`cpupri_find_fitness()`、`cpupri_set()`、
    `cpupri_init()`、`cpupri_cleanup()` 的全部参数、可空性、借用/输出 ownership、上下文、睡眠性、
    返回类别和副作用。头文件没有函数体，三个复杂函数抽查不适用；结合实现复述 find/fitness/set
    三份契约，能够说明分桶扫描、容量过滤后回退、先加入新桶再摘除旧桶及并发结果复核边界。
  - 英文与路径验收：许可证豁免；唯一英文行尾注释逐字保留并有紧邻完整翻译和编码补充。路径覆盖
    空桶跳过、亲和性/active 位图求交、fitness 全部不适配时优先级优先回退、同桶快速返回、
    INVALID 上下线、init 的逐桶分配与 `-ENOMEM` 逆序回滚，以及 cleanup 最终释放。
  - 并发与生命周期：查询方先读原子计数、读屏障后读位图但不获得一致快照；更新方在目标 rq 锁下
    先发布新桶，再通过成对屏障摘除旧桶，使优先级提高期间 reader 至少看到一个桶，余下竞态由
    RT pull/rebalance 和目标 rq 加锁复核收敛。对象随 root_domain 初始化，最后一个引用消失并经过
    RCU 宽限期后释放；生命周期保护、rq 状态锁和无锁候选索引的职责已区分。
  - 关联读取：`kernel/sched/cpupri.c` 全部实现；`kernel/sched/rt.c` 的
    `inc_rt_prio_smp()`/`dec_rt_prio_smp()`、`check_preempt_equal_prio()`、`find_lowest_rq()`、
    `rq_online_rt()`/`rq_offline_rt()`；`kernel/sched/topology.c` 的 `init_rootdomain()` 和
    `free_rootdomain()`；均只读未修改，相关区域学习注释缺失，`cpupri.c`、`rt.c`、`topology.c`
    已在本清单后续计划中。
  - 修改安全：新增 94 行、删除 0 行，原 30 行逐行保留，新增行均为注释或空行；禁用模板前缀扫描
    无命中，`git diff --check` 通过。原始 checkpatch 仅有 38 个中文 UTF-8 字节长度导致的
    `LONG_LINE_COMMENT`，忽略该类型后为 0 errors/0 warnings。工作树无 `.config`，头文件不是独立
    构建目标，未执行配置构建。已按方法论第 17 章完成强制验收，状态为“全文件完成”。
- [ ] `kernel/sched/cpupri.c`
- [x] `kernel/sched/cpudeadline.h`
  - 文件职责：定义每个 `root_domain` 的 SCHED_DEADLINE CPU 候选索引、最大堆节点/反向索引
    布局及五个跨文件接口；以 `free_cpus` 优先表示“rq 在线且无可运行 DL 实体”的 CPU，繁忙时
    由堆根提供“最晚的最早截止期”候选，但不负责真正迁移、拓扑择优或目标 rq 最终复核。
  - 第 17 章验收：完整复读最终 130 行。五个声明的专属函数头分别为 `cpudl_find()` 64～81 行、
    `cpudl_set()` 84～94 行、`cpudl_clear()` 97～107 行、`cpudl_init()` 110～119 行和
    `cpudl_cleanup()` 122～129 行，声明位于 82、95、108、120、130 行；逐项覆盖参数可空性、
    借用/输出 ownership、调用上下文、rq/cpudl 锁边界、睡眠性、返回类别和副作用。该头文件只有
    声明而无函数体，三个复杂函数体抽查不适用；改为结合实现复述 `find/set/clear` 三份接口契约，
    可说明候选产生、插入/改键、删除/填洞、在线位同步及调用方复核边界。
  - 实体与路径验收：覆盖 `IDX_INVALID`、`cpudl_item` 三字段双视图、`cpudl` 四字段、最大堆与
    CPU→idx 不变量；路径清单覆盖空闲位图快速路径、非对称容量全不适配退化、繁忙堆根和无候选，
    set 的首次插入/已有节点更新，clear 的节点存在/缺失与 online/offline，init 的两级分配成功及
    `-ENOMEM` 回滚，以及经过 RCU 后 cleanup 的最终释放。除 SPDX 许可证豁免外没有英文注释，
    条件编译、函数体 cleanup 标签和体系结构分支均不适用。
  - 并发与生命周期：更新方先持目标 `rq->lock`，再由 `cpudl.lock` 以 irqsave 串行化堆、反向索引
    和位图写入；查找不取该锁，只生成可能变化的候选，迁移路径随后锁住并复核目标 rq。对象随
    `root_domain` 初始化，成功后拥有 `nr_cpu_ids` 元素数组和动态 cpumask；最后一个 root-domain
    引用消失并经过 RCU 宽限期后释放，引用保护、状态锁和候选复核的职责已明确区分。
  - 关联读取：`kernel/sched/cpudeadline.c` 全部实现；`kernel/sched/deadline.c` 的
    `inc_dl_deadline()`/`dec_dl_deadline()`、`check_preempt_equal_dl()`、`find_later_rq()`、
    `find_lock_later_rq()`、`rq_online_dl()`/`rq_offline_dl()`；`kernel/sched/topology.c` 的
    `init_rootdomain()`/`free_rootdomain()`/`rq_attach_root()`；`kernel/sched/sched.h` 的
    `struct root_domain`。均只读未修改，目标相关区域缺少完整中文学习注释；`cpudeadline.c` 已在
    本清单后续计划中，其他文件按各自条目后续闭环。另核对提交 `382748c05e58`，确认离线 rq 必须
    清除 free 位的当前语义。
  - 修改安全：新增 106 行、删除 0 行，原 24 行逐行保留；新增行均为注释或空行，禁用模板前缀
    扫描无命中，`git diff --check` 通过。原始 checkpatch 仅有 39 个中文 UTF-8 字节长度导致的
    `LONG_LINE_COMMENT`，忽略该类型后为 0 errors/0 warnings。工作树无 `.config`，头文件也不是
    独立构建目标，未执行配置构建。已按方法论第 17 章完成强制验收，状态为“全文件完成”。
- [ ] `kernel/sched/cpudeadline.c`
- [ ] `kernel/sched/topology.c`

### PELT、负载、统计与 CPU 时间

- [ ] `kernel/sched/sched-pelt.h`
- [x] `kernel/sched/pelt.h`
  - 文件职责：连接 pelt.c 的实体/CFS/RT/DL/IRQ/硬件压力更新实现与调度器消费路径，并维护按 CPU
    原始容量和当前频率缩放的 rq PELT 时间轴、idle 快照、lost-idle 修正及 CFS bandwidth 停钟；
    它输出指数衰减估计，不选择任务/CPU，也不拥有或分配调度对象。
  - 第 17 章验收：完整复读最终 383 行。6 个跨成员 PELT 入口、HW/IRQ 配置两侧接口、
    `PELT_MIN_DIVIDER`、`get_pelt_divider()`、`cfs_se_util_change()`、4 个 rq 时钟 inline 和 CFS bandwidth
    两侧接口均有紧邻专属契约；全部参数、ns/容量/贡献单位、借用 ownership、rq 锁/时钟前置条件、
    返回类别和副作用均覆盖。局部 enqueued、divider、util_sum、throttled 的哨兵/有效期也已说明。
  - 路径与复杂函数抽查：可仅凭注释复述 `cfs_se_util_change()` 的 feature/标志双快速路径与 WRITE_ONCE
    发布；`update_rq_clock_pelt()` 的 idle 同步和非 idle 两级容量缩放；`update_idle_rq_clock_pelt()` 的
    CFS+RT+DL 满载阈值、lost_idle_time 结算和无条件快照发布。另覆盖 HW/IRQ 缺失 stub、CFS bandwidth
    停钟/正常时钟和关闭配置直接 rq 时钟。
  - 英文与并发验收：全部 10 个非许可证英文块/单行注释逐字保留并有紧邻完整翻译和机制补充；ASCII
    时钟示意图原样保留并解释。`_update_idle_rq_clock_pelt()` 先写 clock_idle、wmb、再写
    clock_pelt_idle，与 `migrate_se_pelt_lag()` 先读 PELT、rmb、再读 clock 配对，使混合代际最坏低估
    而非过度衰减；u64_u32_store/load 保证 32 位快照不撕裂，rq 锁串行化写者。
  - 生命周期：PELT avg 随 task/cfs_rq/rq 内嵌存在；本文件不取得引用或释放对象。CFS bandwidth 把
    throttle 区间从组时间轴扣除，正在停钟以 U64_MAX 阻止 NO_HZ 迁移估算；解除限流后累计停钟时间。
    lost_idle_time 累计排除低容量满载造成的虚假 idle，不与普通 idle 或 IRQ/HW 压力混算。
  - 关联读取：`kernel/sched/pelt.c` 全部实现；`kernel/sched/fair.c` 的 `migrate_se_pelt_lag()`、实体迁移、
    CFS throttle/unthrottle、enqueue/dequeue 和各 inline 调用点；`kernel/sched/sched.h` 的 rq 时钟字段；
    `kernel/sched/features.h` 的 UTIL_EST。均只读未修改；features.h 本轮已充分覆盖，pelt.c/fair.c/sched.h
    相关区域缺失或部分覆盖，仍按清单后续闭环。
  - 修改安全：新增 194 行、删除 0 行，原 189 行逐行保留，新增行均为注释或空行；禁用模板前缀扫描
    无命中，`git diff --check` 通过。原始 checkpatch 仅有 79 个中文 UTF-8 字节长度导致的
    `LONG_LINE_COMMENT`，忽略后为 0 errors/0 warnings。工作树无 `.config`，头文件不是独立构建目标，
    未执行配置构建。已按方法论第 17 章完成强制验收，状态为“全文件完成”。
- [ ] `kernel/sched/pelt.c`
- [ ] `kernel/sched/loadavg.c`
- [ ] `kernel/sched/cputime.c`
- [ ] `kernel/sched/cpuacct.c`
- [ ] `kernel/sched/stats.h`
- [ ] `kernel/sched/stats.c`
- [ ] `kernel/sched/debug.c`
- [ ] `kernel/sched/psi.c`

### CPU 频率调节

- [ ] `kernel/sched/cpufreq.c`
- [ ] `kernel/sched/cpufreq_schedutil.c`

### sched_ext 可扩展调度器

- [ ] `kernel/sched/ext/types.h`
- [ ] `kernel/sched/ext/internal.h`
- [ ] `kernel/sched/ext/ext.h`
- [x] `kernel/sched/ext/arena.h`
  - 文件职责：公开每个 `scx_sched` 实例的 BPF arena 内核侧 `gen_pool` 初始化、销毁、按需扩容分配
    和子区间归还接口；返回的是内核映射地址，BPF 地址转换与底层 arena map 页生命周期不由本头
    文件承担。
  - 第 17 章验收：完整复读最终 72 行。前置声明、重复包含保护和 4 个函数声明均有用途说明；
    `scx_arena_pool_init()`、`scx_arena_pool_destroy()`、`scx_arena_alloc()`、`scx_arena_free()` 的专属
    函数头逐项覆盖参数、size 单位与配对要求、借用 ownership、睡眠性、返回类别和副作用。文件无
    函数体或结构体字段，三个复杂函数抽查不适用；结合实现可复述建池、扩容登记失败归还新页、
    子分配归还、销毁清除未归还区间与 arena map 最终回收的完整生命周期。
  - 英文与路径验收：原总览、版权及 `#endif` 行尾注释逐字保留；许可证/版权豁免，总览和保护宏有
    紧邻中文解释。路径覆盖无 arena 配置的成功空操作、建池 `-ENOMEM`、无池/扩容失败返回 NULL、
    扩容至少 4 页、空指针 free、销毁空池及清除 outstanding 子分配。
  - 并发与生命周期：接口自身不串行化 alloc/free；调用者须用调度器装载/卸载阶段阻止与 destroy
    并发。gen_pool 元数据在 destroy 释放并置 NULL，底层页随 arena map 拆除；子区间地址归还后
    立即失效，不能并发使用、重复释放或用不匹配的 size 归还。
  - 关联读取：`kernel/sched/ext/arena.c` 全部实现；`kernel/sched/ext/ext.c` 的
    `scx_set_cmask_scratch_alloc()`/`scx_set_cmask_scratch_free()` 及 arena pool 初始化/销毁调用点；
    `kernel/sched/ext/internal.h` 的 `scx_sched.arena_map/arena_pool` 字段。均只读未修改，arena.c 与
    ext.c 相关区域缺少完整学习注释，internal.h 字段只有上游英文；三者均在本清单后续计划中。
  - 修改安全：新增 52 行、删除 0 行，原 20 行逐行保留，新增行均为注释；禁用模板前缀扫描无命中，
    `git diff --check` 通过。原始 checkpatch 仅有 22 个中文 UTF-8 字节长度导致的
    `LONG_LINE_COMMENT`，忽略后为 0 errors/0 warnings。工作树无 `.config`，头文件不是独立构建
    目标，未执行配置构建。已按方法论第 17 章完成强制验收，状态为“全文件完成”。
- [ ] `kernel/sched/ext/arena.c`
- [ ] `kernel/sched/ext/cid.h`
- [ ] `kernel/sched/ext/cid.c`
- [x] `kernel/sched/ext/idle.h`
  - 文件职责：连接 sched_ext 核心与 idle 实现，公开默认拓扑感知选核、全局/逐 NUMA 节点 idle 掩码
    初始化和启停接口，并导出普通 idle helper 与 select_cpu helper 两组 BTF kfunc ID 集合；它只提供
    接口和候选，不实现掩码更新或承诺返回 CPU 仍空闲。
  - 第 17 章验收：完整复读最终 101 行。3 个前置声明、2 个 BTF 集合以及 6 个函数声明均有角色说明；
    每个函数头覆盖全部参数、可空性、借用 ownership、CPU 热插拔/RCU/禁抢占上下文、睡眠性、返回
    类别和副作用。头文件没有函数体，三个复杂函数抽查不适用；结合实现复述拓扑静态键更新、默认
    选核、内建追踪启停三组契约，可说明 LLC/NUMA 层次、idle 位原子领取和 BPF 接管边界。
  - 英文与路径验收：原总览、版权及 `#endif` 行尾注释逐字保留；许可证/版权豁免，其余英文有紧邻
    中文翻译与机制补充。路径覆盖额外允许掩码无交集、同步唤醒、完整空闲 SMT core、prev CPU/兄弟、
    LLC/NUMA/任意 idle 回退，ops.update_idle 接管与 KEEP 内建模式，以及 5 次 kfunc 注册的首错返回。
  - 并发与生命周期：rq busy/idle 路径先更新内建位图再通知 BPF，以便和 enqueue 形成交锁；选核在
    禁抢占及 RCU 读侧原子领取 idle 位，但结果仍可能过期。enable/topology 更新由 CPU 热插拔锁稳定
    在线集合并修改 static branch；disable 不释放掩码或等待读者，调度器卸载序列负责阻止新调用。
  - 关联读取：`kernel/sched/ext/idle.c` 的拓扑检测、`scx_select_cpu_dfl()`、掩码初始化、
    `scx_idle_enable()`/`disable()`、kfunc 集合和 `scx_idle_init()`；`kernel/sched/ext/ext.c` 的选核、
    enable/disable 和启动调用点；`kernel/sched/ext/internal.h` 的 select_cpu/update_idle 契约与标志。
    均只读未修改；idle.c/ext.c/internal.h 相关区域只有上游英文或部分学习注释，均在后续计划中。
  - 修改安全：新增 70 行、删除 0 行，原 31 行逐行保留，新增行均为注释；禁用模板前缀扫描无命中，
    `git diff --check` 通过。原始 checkpatch 仅有 28 个中文 UTF-8 字节长度导致的
    `LONG_LINE_COMMENT`，忽略后为 0 errors/0 warnings。工作树无 `.config`，头文件不是独立构建
    目标，未执行配置构建。已按方法论第 17 章完成强制验收，状态为“全文件完成”。
- [ ] `kernel/sched/ext/idle.c`
- [ ] `kernel/sched/ext/ext.c`

## 明确排除

- [-] `kernel/sched/core.c`（按用户要求直接跳过）
- [-] `kernel/sched/completion.c`（按用户要求直接跳过）
- [-] `kernel/sched/idle.c`（按用户要求直接跳过；原请求重复列出一次，本清单合并为一个排除项）
- [-] `kernel/sched/isolation.c`（按用户要求直接跳过）
- [-] `kernel/sched/swait.c`（按用户要求直接跳过）

## 当前处理文件

- 无；本轮 8 个文件 `cpupri.h`、`ext/arena.h`、`ext/idle.h`、`autogroup.h`、
  `build_policy.c`、`build_utility.c`、`features.h`、`pelt.h` 均已完成整体审计。
- 本轮源文件合计新增 675 行、删除 0 行；逐文件新增行均仅为注释或空行，原代码、声明、宏、条件
  编译和原注释逐行保留。8 个目标的 `git diff --check` 与忽略中文 UTF-8 字节行长后的 checkpatch
  均为 0 errors/0 warnings；工作树无 `.config`，未执行构建或运行时验证。
- 当前计划进度为 12/46 个文件 `[x]`、34/46 个文件 `[ ]`，5 个用户明确排除文件 `[-]`。
- `kernel/sched/sched.h` 只做过局部只读建图，未修改源码，保持 `[ ]`。
- `kernel/sched/sched-pelt.h` 标明为自动生成且禁止直接修改，本轮保持 `[ ]`，等待范围决策。

## 单文件完成记录模板

每个 `[x]` 文件下至少追加以下可复核记录：

- 文件职责与主调用链。
- 第 17 章验收：函数清单、实体清单、英文注释清单、成功/快速/慢速/失败/释放/配置路径清单。
- 并发与生命周期：锁、RCU、引用、屏障、发布、摘除和最终释放。
- 至少三个复杂函数的初学者复述与开发者推理抽查；不足三个时说明实际数量及豁免原因。
- 关联读取：文件、符号或范围、读取原因、结论、现有学习注释覆盖状态；关联文件只读不改。
- 修改安全：零代码改动、零原注释删除或改写、禁用模板前缀扫描、`git diff --check`、checkpatch
  和可用的构建结果；未执行项必须写明原因。
- 最终状态必须准确使用“全文件完成”“主路径检查点完成”或“尚未达到标准”。只有前者可标 `[x]`。

## 范围变化

- 2026-08-20：创建清单。目录快照共 51 个文件，46 个进入计划，5 个明确排除。
- 2026-08-24：按小文件优先的单文件闭环完成上述 8 个目标；计划完成度由 4/46 提升为 12/46。
