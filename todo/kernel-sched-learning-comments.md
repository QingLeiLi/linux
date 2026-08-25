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
   和关联读取清单，再执行注释密度门禁、追加式安全审计、格式检查及可用的构建检查。密度命令固定为
   `scripts/check-learning-comment-density.py --min-density 0.20 --max-code-gap 10 <目标文件>`；两项阈值
   必须同时通过，不能以总新增行数代替。
6. 只有单文件内容验收全部通过后才标为 `[x]` 并登记验收摘要；一个 `[~]` 文件未闭环前不开始
   下一个文件。
7. 为核对契约而读取的关联源码只登记到当前文件记录；读取关联文件不等于授权修改。
8. 本清单覆盖创建时 `kernel/sched` 下的全部 51 个文件：46 个进入计划，5 个按用户要求排除。
   后续若目录新增文件，先登记到“范围变化”再决定学习顺序，不能静默遗漏。
9. 从 2026-08-25 当前 `[~]` 文件起，后续每个函数的专属中文函数头必须紧邻函数定义并显式包含四项：
   **业务背景**（必须包含理解该函数所需的业务背景知识、上层问题、存在理由和调用链位置）、**入参**（逐参数的含义、单位、范围、可空性、
   输入/输出属性和 ownership）、**出参/返回**（逐输出参数的前后状态与 ownership，以及全部直接返回
   类别）、**注意事项**（作为函数的强制注释事项，覆盖锁/中断/RCU/睡眠、生命周期、失败副作用、配置差异和误用后果中的适用项）。
   无参数、无输出参数或 `void` 返回也必须明确写“无”，不能靠上下文猜测；函数组说明和函数体走读
   不能替代单个函数的四项契约。验收记录必须逐函数核对这四项，密度脚本通过不能替代该人工门禁。
10. 本规则采用向前生效：规则写入前已经标为 `[x]` 的文件不因新增四项格式而重新打开；当前 `[~]`
    和所有 `[ ]` 文件必须严格执行。若已完成文件以后因其他原因重新打开，则该次重新验收也适用新规则。

状态：`[ ]` 未开始；`[~]` 正在处理或验收；`[x]` 已按第 17 章完成全文件闭环；`[-]` 明确排除。

## 文件进度（按建议学习顺序）

### 构建入口、公共数据结构与调度特性

- [x] `kernel/sched/Makefile`
- [ ] `kernel/sched/sched.h`
- [x] `kernel/sched/smp.h`
- [x] `kernel/sched/features.h`
- [x] `kernel/sched/rq-offsets.c`
- [x] `kernel/sched/build_policy.c`
- [x] `kernel/sched/build_utility.c`

### 系统调用、等待与跨 CPU 调度接口

- [x] `kernel/sched/syscalls.c`
- [x] `kernel/sched/wait.c`
- [x] `kernel/sched/wait_bit.c`
- [x] `kernel/sched/membarrier.c`
- [x] `kernel/sched/clock.c`

### 调度类与运行队列策略

- [ ] `kernel/sched/fair.c`
- [~] `kernel/sched/rt.c`
- [ ] `kernel/sched/deadline.c`
- [x] `kernel/sched/stop_task.c`
- [x] `kernel/sched/core_sched.c`
- [x] `kernel/sched/autogroup.h`
- [x] `kernel/sched/autogroup.c`

### CPU 优先级、截止期与拓扑

- [x] `kernel/sched/cpupri.h`
- [x] `kernel/sched/cpupri.c`
- [x] `kernel/sched/cpudeadline.h`
- [x] `kernel/sched/cpudeadline.c`
- [x] `kernel/sched/topology.c`

### PELT、负载、统计与 CPU 时间

- [ ] `kernel/sched/sched-pelt.h`
- [x] `kernel/sched/pelt.h`
- [x] `kernel/sched/pelt.c`
- [x] `kernel/sched/loadavg.c`
- [x] `kernel/sched/cputime.c`
- [x] `kernel/sched/cpuacct.c`
- [x] `kernel/sched/stats.h`
- [x] `kernel/sched/stats.c`
- [x] `kernel/sched/debug.c`
- [x] `kernel/sched/psi.c`

### CPU 频率调节

- [x] `kernel/sched/cpufreq.c`
- [x] `kernel/sched/cpufreq_schedutil.c`

### sched_ext 可扩展调度器

- [x] `kernel/sched/ext/types.h`
- [x] `kernel/sched/ext/internal.h`
- [x] `kernel/sched/ext/ext.h`
- [x] `kernel/sched/ext/arena.h`
- [x] `kernel/sched/ext/arena.c`
- [x] `kernel/sched/ext/cid.h`
- [x] `kernel/sched/ext/cid.c`
- [x] `kernel/sched/ext/idle.h`
- [x] `kernel/sched/ext/idle.c`
- [ ] `kernel/sched/ext/ext.c`

## 明确排除

- [-] `kernel/sched/core.c`（按用户要求直接跳过）
- [-] `kernel/sched/completion.c`（按用户要求直接跳过）
- [-] `kernel/sched/idle.c`（按用户要求直接跳过；原请求重复列出一次，本清单合并为一个排除项）
- [-] `kernel/sched/isolation.c`（按用户要求直接跳过）
- [-] `kernel/sched/swait.c`（按用户要求直接跳过）

## 当前处理文件

- 当前 `[~]` 文件为 `rt.c`。新增密度门禁回溯原 43 个 `[x]` 时有 27 个 C/头文件失败；随后已重新
  处理并关闭其中 17 个。`Makefile` 不属于 C-family 统计范围，按原验收保留 `[x]`。
- 当前通过完整验收的计划项共 40 个：`Makefile`、`smp.h`、`features.h`、`rq-offsets.c`、
  `build_policy.c`、`build_utility.c`、`stop_task.c`、`autogroup.h`、`autogroup.c`、`cpupri.h`、
  `cpupri.c`、`cpudeadline.h`、`cpudeadline.c`、`pelt.h`、`cpuacct.c`、`stats.h`、`stats.c`、
  `cpufreq.c`、`core_sched.c`、`wait_bit.c`、`ext/types.h`、`ext/ext.h`、`ext/arena.h`、
  `ext/arena.c`、`ext/cid.h`、`ext/idle.h`、`loadavg.c`、`wait.c`、`pelt.c`、`clock.c`、`membarrier.c`、
  `ext/cid.c`、`cpufreq_schedutil.c`、`cputime.c`、`debug.c`、`psi.c`、`ext/internal.h`、`ext/idle.c`、
  `topology.c`、`syscalls.c`。
- 回溯和重验统一使用 `--min-density 0.20 --max-code-gap 10`。仍未复验通过的 3 个旧记录目标为：
  `sched.h`（0.030/196）、
  `rt.c`（0.068/89）、
  `deadline.c`（0.039/226）。
  括号内依次为“中文注释行/有效代码行”和“最大连续无中文注释代码行”；任一项不合格即失败。
- `fair.c`（0.009/777）和 `ext/ext.c`（0.012/650）也未达到新标准。上述 6 个文件仍不能使用旧验收
  记录中的“全文件完成”结论；旧记录只保留为当时的语义和追加式安全审计证据。
- 当前计划进度为 40/46 个文件 `[x]`、5/46 个文件 `[ ]`、1/46 个文件 `[~]`，
  5 个用户明确排除文件 `[-]`。工作树无 `.config`，未执行构建或运行时验证。
- `kernel/sched/sched-pelt.h` 标明为自动生成且禁止直接修改，本轮保持 `[ ]`，等待范围决策。

### `kernel/sched/build_policy.c`（2026-08-25 密度复验）

- 文件无函数、结构体或运行时对象；全部有效代码都是头文件或 `.c` 文本聚合指令。已重新顺序核对
  公共依赖、策略实现、`CONFIG_SCHED_CLASS_EXT` 配置分支和最终系统调用入口，并在两个超长区段中
  补充“声明层 → 实现层”的阶段边界以及资源/锁责任仍归具体成员的约束。
- 关联读取：`kernel/sched/Makefile:53-75`，确认两个聚合对象的分支插桩开关和 `build_policy.o`
  链接关系；该范围已有充分中文学习注释，只读未改。
- 修改安全：本次新增 8 行、删除 0 行，diff 中只有独立中文注释；原代码和原注释均未改写。
  `git diff --check` 通过，忽略中文 UTF-8 字节导致的 `LONG_LINE_COMMENT` 后 checkpatch 为
  0 errors/0 warnings。密度命令使用固定阈值，结果为
  `code=48, comments=70, chinese=37, density=0.771, max_gap=8`，退出码 0。无 `.config`，未构建。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。复杂函数复述测试因文件无函数豁免，
  开发者推理改为验证 include 顺序、配置裁剪和重复编译静态符号冲突的后果。

### `kernel/sched/build_utility.c`（2026-08-25 密度复验）

- 文件无函数、结构体或运行时对象；有效代码由公共头和按配置选择的 `.c` 聚合指令组成。已重新核对
  头文件依赖、成员实现顺序及所有配置分支，在原 24 行空白区段中补充策略输入/容器、同步/观测接口
  两个阶段，并明确集中 include 不会合并各成员的锁、RCU、NMI 和资源生命周期契约。
- 关联读取：`kernel/sched/Makefile:53-76`，确认 `DISABLE_BRANCH_PROFILING` 与
  `build_utility.o` 链接关系；该范围已有充分中文学习注释，只读未改。
- 修改安全：本次新增 8 行、删除 0 行，diff 中只有独立中文注释；原代码和原注释均未改写。
  `git diff --check` 通过，忽略 `LONG_LINE_COMMENT` 后 checkpatch 为 0 errors/0 warnings。
  密度结果为 `code=77, comments=62, chinese=39, density=0.506, max_gap=9`，退出码 0。
  无 `.config`，未构建。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。复杂函数复述测试因文件无函数豁免，
  开发者推理改为核对配置裁剪、共享 static 命名空间及单个成员变更触发整体重编的边界。

### `kernel/sched/cpufreq.c`

- 文件职责与主调用链：文件维护每 CPU 唯一的 RCU `update_util_data` 槽，连接
  fair/RT/DL/sched_ext 的 `cpufreq_update_util()` 热路径与 schedutil、传统 governor、
  intel_pstate 回调；本层只注册、摘除和判断发起资格，不计算或提交频率。
- 第 17 章验收：3 个函数均有紧邻声明的专属函数头。`add` 覆盖空参数、槽占用和成功发布；
  `remove` 覆盖摘除与延迟回收；`this_cpu_can_update` 覆盖共享 policy 快速路径、跨 policy
  DVFS 和 CPU 下线拒绝路径。文件无结构体定义和局部变量；全局 per-CPU 槽及全部参数、回调
  参数、返回类别和副作用均已说明。3 个原 kernel-doc 与文件导读原文逐字保留并紧邻补充完整
  中文翻译和机制解释，无 cleanup、分配或可报告错误码路径。
- 并发与生命周期：`data->func` 先初始化，`rcu_assign_pointer()` 后发布；热路径通过
  `rcu_dereference_sched()` 借用，RCU 不转移 ownership，也不串行化两个写者。摘除只阻止
  新读者，旧容器必须存活到 `synchronize_rcu()` 返回或 RCU callback 执行。remote-DVFS
  分支以本 CPU 槽非空作为尚未进入下线撤销的信号。
- 初学者与开发者抽查：文件仅有 3 个函数，已全部抽查。可从注释复述“校验 → 初始化回调 →
  RCU 发布”、 “发布 NULL → 等待外部宽限期”及“同 policy 快速放行 → remote-DVFS/在线门禁”；
  也可推导发布顺序颠倒会暴露未初始化 func、注销后立即释放会 use-after-free、把槽占用检查
  当原子锁会导致双写竞态。
- 关联读取：`kernel/sched/sched.h:3525-3562`（分发器与读侧契约，缺失中文学习覆盖）；
  `include/linux/sched/cpufreq.h:7-36`（接口结构与声明，缺失）；`include/linux/cpufreq.h:53-141`
  （在线 policy 掩码与 remote-DVFS 字段，缺失）；`kernel/sched/cpufreq_schedutil.c:64-105,845-892`
  （资格检查及成对启停，缺失）；`drivers/cpufreq/cpufreq_governor.c:285-363`（传统 governor
  回调及宽限期，缺失）；`drivers/cpufreq/intel_pstate.c:2785-2814`（驱动注册/注销，缺失）；
  `kernel/sched/fair.c:4864-4885,7815-7835,11101-11115,11225-11246`、
  `kernel/sched/deadline.c:213-238`、`kernel/sched/rt.c:548-562,1027-1047`、
  `kernel/sched/ext/ext.c:9995-10023`（各调度类触发位置，均缺失中文学习覆盖）。这些文件只读
  未改；建议在各自计划项中补注，不扩展本文件闭环范围。
- 修改安全：源码新增 114 行、删除 0 行；新增行脚本审计均为注释或空行，故零代码改动、零原
  注释删除或改写。禁用模板前缀扫描无命中，`git diff --check` 通过；原始 checkpatch 仅有
  49 个中文 UTF-8 字节行长告警，忽略 `LONG_LINE_COMMENT` 后为 0 errors/0 warnings。仓库无
  `.config`，未执行目标对象构建或运行时/RCU 热插拔验证。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

### `kernel/sched/ext/ext.h`

- 文件职责与验收：覆盖调度核心到 SCX 的 fork、tick/NO_HZ、CPU 热插拔、策略门禁、idle、
  CPUFreq 和 cgroup 接口。CONFIG 启用声明、关闭态中性 stub 与 3 个有函数体的 inline 共
  50 项均有紧邻契约；参数、返回、状态/ownership、睡眠与配置差异全部核对。原文件仅有顶部
  英文导读及配置标签，均原样保留并补充中文语义。
- 路径与并发抽查：抽查 `scx_fork` 的 pre/fork/post/cancel 门闩闭环、`scx_update_idle` 的
  静态键与“先 idle 位后 BPF 回调”顺序、cgroup prepare/move/cancel 事务；可推导漏放读锁、
  注销配置返回非中性值或跳过迁移回滚会破坏的状态。其余短声明/stub 全量复述验收。
- 关联读取：`kernel/sched/ext/ext.c:3285-3451,3495-3538,3750-3888,4000-4037,4320-4571,
  5070-5140,8490-8566` 与 `kernel/sched/ext/idle.c:733-789` 用于核对实现，均缺失系统中文覆盖；
  `kernel/sched/core.c`、`kernel/sched/syscalls.c`、`kernel/sched/cpufreq_schedutil.c` 仅搜索调用点，
  均缺失中文覆盖，留待各自计划项。
- 修改安全：新增 207 行、删除 0 行，新增行仅注释；禁用模板前缀无命中，`git diff --check`
  和忽略 `LONG_LINE_COMMENT` 后 checkpatch 均为 0 errors/0 warnings。无 `.config`，未构建。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

### `kernel/sched/stop_task.c`

- 文件职责与验收：13 个 stop 类回调及 `stop_sched_class` 操作表全量覆盖；每个函数头均紧邻
  声明，所有 rq/task/flags/时间参数、NULL/true/BUG 出口、nr_running 与 exec_start 副作用、
  空 tick/update stub 均已说明。5 组原英文说明/行尾注释逐字保留并紧邻翻译补充。
- 路径与并发抽查：抽查 `pick_task_stop` 的 runnable/NULL 分支、set/put 的时钟记账配对、
  yield/switching/prio_changed 的不可恢复入口；可推导迁移 stopper、允许 yield 或把 stop section
  排到 DL 以下会破坏 stop_machine 的 CPU 独占窗口。rq 锁保护选择与计数，callback 禁止抢占且
  不得睡眠。
- 关联读取：`kernel/sched/sched.h:2773-2842`（sched_class 链接顺序及 runnable 判定，部分覆盖）、
  `kernel/sched/core.c:4635-4701`（stopper 安装/替换，充分）、`kernel/stop_machine.c:100-165,
  500-565`（工作执行和线程生命周期，部分覆盖）；只读未改，后两者已在各自任务中存在不同程度
  中文注释，剩余英文宜随对应文件补齐。
- 修改安全：新增 118 行、删除 0 行，新增行仅注释；禁用模板前缀无命中，`git diff --check`
  和忽略 `LONG_LINE_COMMENT` 后 checkpatch 均为 0 errors/0 warnings。无 `.config`，未构建。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

### `kernel/sched/ext/arena.c`

- 文件职责与验收：7 个函数及 2 个枚举常量全量覆盖；讲清 BPF arena 页 ownership 与 gen_pool
  子分配元数据的边界、BPF 偏移到 kernel VA 的换算、8 字节粒度、至少 4 页扩容、NULL/errno
  返回及 size 配对约束。4 组英文导读/函数说明和 3 个行尾配置注释均原样保留并紧邻翻译。
- 路径与并发抽查：抽查 grow 的“分配页→地址换算→登记→失败归还”、alloc 的“现有池→扩容→
  重试”和 destroy 的“清占用位→销毁元数据→置 NULL”；可推导 size=0、错 size free、登记失败
  不回滚或 destroy 并发使用的后果。gen_pool 处理活动期子分配同步，实例卸载协议排除销毁竞态。
- 关联读取：`kernel/sched/ext/arena.h:1-72`（公开契约，充分）、`kernel/sched/ext/ext.c:
  4890-5020,7175-7220,7550-7590`（scratch 调用及装载/RCU 销毁，缺失）、`kernel/bpf/arena.c:
  65-110,1030-1105`（映射基址和页 kfunc，缺失）；只读未改，后两者建议随所属文件补注。
- 修改安全：新增 90 行、删除 0 行，新增行仅注释；禁用模板前缀无命中，`git diff --check`
  和忽略 `LONG_LINE_COMMENT` 后 checkpatch 均为 0 errors/0 warnings。无 `.config`，未构建。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

### `kernel/sched/ext/types.h`

- 文件职责与验收：16 个 dispatch/watchdog/诊断/bypass/sub-scheduler 常量、`scx_cid_topo` 六字段、
  `scx_cmask` 四字段及 4 个 function-like 宏全量覆盖。文件无函数；每个实体的单位、哨兵、容量、
  生命周期和配置约束已说明，顶部与 6 组英文注释逐字保留并完整翻译补充。
- 路径与推理抽查：抽查 per-CPU tid chunk 摊销、CID 拓扑首 cid/全局 idx 区分、cmask 的全局
  64-bit 网格与 padding 不变量，以及 DEFINE/SHARD 宏容量。可推导 u32 区间回绕、padding 非零、
  ALLOC_CIDS 不足或 shard 超上限会导致错误 word 运算/越界；本文件无锁和资源回滚，调用者负责
  cmask 存储生命周期与并发。
- 关联读取：`kernel/sched/ext/ext.c:140-180,620-900,2770-2830,3750-3800,4050-4220,
  5250-5420,5680-6710,6810-6880,7150-7220,7540-7960,9870-9930`（常量消费，缺失），
  `kernel/sched/ext/cid.h:1-180`、`kernel/sched/ext/cid.c:20-210,230-260,650-685`（拓扑/cmask
  约束，均缺失）；只读未改，建议在 cid.h/cid.c 计划项中系统补注。
- 修改安全：新增 84 行、删除 0 行，新增行仅注释；禁用模板前缀无命中，`git diff --check`
  和忽略 `LONG_LINE_COMMENT` 后 checkpatch 均为 0 errors/0 warnings。无 `.config`，未构建。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

### `kernel/sched/stats.c`

- 文件职责与验收：8 个函数、`SCHEDSTAT_VERSION` 常量、`schedstat_sops` 操作表和
  `subsys_initcall` 注册点全量覆盖。等待开始/结束、睡眠与阻塞结算、头部/CPU/调度域输出、
  CPU 热插拔感知迭代和初始化失败表现均有紧邻说明；顶部导读、迁移、版本、runqueue/domain
  及迭代器共 6 组原英文注释逐字保留并完整翻译。
- 路径与并发抽查：抽查迁移任务把 `wait_start` 临时改作已等待时长并在目标 rq 重建时间戳、
  sleeper 对跨 CPU 负 delta 夹零并把 block 归入 sleep/I/O 子集，以及 seq token 的“1 为头、
  cpu+2 为数据”协议。可推导迁移时提前结算会重复增加 wait_count，移除负值防护会污染累计值，
  直接按 offset 当 CPU 会在热插拔空洞上输出错误对象。更新侧依赖调用者持有 rq 锁；proc 读取
  不取 rq 锁，只提供跨字段可能变化的诊断快照，RCU 仅保护 sched_domain 链生命周期。
- 关联读取：`kernel/sched/stats.h:1-106`（静态键、更新锁契约和组实体选择，部分覆盖）、
  `include/linux/sched.h:635-680`（统计字段，充分）、`kernel/sched/fair.c:2039-2115`
  （任务/组调用与动态启用防护，缺失）、`Documentation/scheduler/sched-stats.rst:1-205`
  （v17 文本 ABI，文档本身无需源码注释）、`tools/lib/perf/include/perf/schedstat-v17.h:1-190`
  （用户态字段解析，缺失）；只读未改，调度类实现留待各自计划项。
- 修改安全：新增 91 行、删除 0 行，新增行仅注释；禁用模板前缀无命中，`git diff --check`
  和忽略 `LONG_LINE_COMMENT` 后 checkpatch 均为 0 errors/0 warnings。无 `.config`，未执行目标
  对象构建、CPU 热插拔或运行时 proc 输出验证。
- 2026-08-25 密度复验：重新顺序检查 8 个函数、操作表、版本常量、英文注释及热路径/导出路径；
  在 `show_schedstat()` 的 11 字段调度域输出中新增 4 行阶段说明，并在局部审计中恢复了补丁曾
  触及的 6 行既有缩进，最终 diff 为新增 4 行、删除 0 行。固定密度门禁结果为
  `code=154, comments=119, chinese=69, density=0.448, max_gap=10`，退出码 0；
  `git diff --check` 通过，忽略 `LONG_LINE_COMMENT` 后 checkpatch 为 0 errors/0 warnings。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

### `kernel/sched/cpudeadline.c`

- 文件职责与验收：12 个函数全量覆盖，讲清 root_domain 内“空闲 CPU 位图 + 最晚的最早
  deadline 最大堆”两级索引、数组节点/按 CPU 反向 idx 的复用、回绕时间比较及候选而非迁移
  承诺的边界。顶部导读、heap 改编、节点搬移及 5 组 kernel-doc 原文均逐字保留并紧邻翻译。
- 路径与并发抽查：抽查 heapify 上下浮时节点搬移与 cpu→idx 同步、find 的 free CPU/非对称
  容量退化/堆根三条路径，以及 clear 的末节点填洞和 online 位图分流；可推导漏改 idx 会让后续
  set 修改错误节点、把 offline 空 rq 留在 free_cpus 会选中下线 CPU、把 find 成功当迁移承诺会
  遭遇动态索引竞态。更新方先持目标 rq 锁，再 irqsave 获取 cp->lock；查找无锁，只给瞬时候选。
- 关联读取：`kernel/sched/cpudeadline.h:1-130`（公开结构与契约，充分）、
  `kernel/sched/deadline.c:2190-2265,2660-2720,2925-2975,3345-3390`（set/clear/find 调用和
  rq 上下线，缺失）、`kernel/sched/topology.c:430-575`（root_domain 初始化回滚与 RCU 销毁，
  部分覆盖）、`include/linux/sched/deadline.h:27-31`（回绕比较，缺失）；只读未改。
- 修改安全：新增 141 行、删除 0 行，新增行仅注释；禁用模板前缀无命中，`git diff --check`
  和忽略 `LONG_LINE_COMMENT` 后 checkpatch 均为 0 errors/0 warnings。无 `.config`，未构建或执行
  DL push、CPU 热插拔与 root_domain 销毁运行时验证。
- 2026-08-25 密度复验：重新顺序检查 12 个函数、堆/反向索引实体、全部英文注释和更新/查找/
  回收路径；在 `cpudl_heapify_down()` 左右孩子选择阶段新增 4 行，说明必须让右孩子与当前胜者
  而非原节点比较，本次删除 0 行。关联复读 `cpudeadline.h`、`deadline.c` 和 `topology.c` 的
  声明、rq 锁调用及 root_domain 资源配对（覆盖状态沿用上条记录），只读未改。固定密度门禁结果为
  `code=177, comments=200, chinese=105, density=0.593, max_gap=10`，退出码 0；
  `git diff --check` 通过，忽略 `LONG_LINE_COMMENT` 后 checkpatch 为 0 errors/0 warnings。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

### `kernel/sched/wait_bit.c`

- 文件职责与验收：16 个函数、2 个哈希常量及全局 256 桶 waitqueue 表全量覆盖；地址+bit 与
  变量地址两类 key、栈 entry 生命周期、普通/排他/IO/timeout action 和导出边界均有紧邻契约。
  顶部导读、action 返回协议、bit-lock 屏障说明及两组长 kernel-doc 原文逐字保留并完整翻译。
- 路径与并发抽查：抽查普通等待“先入队设状态→检查/action→acquire 复查→finish”、bit-lock
  “排他入队→action→原子 0→1 提交 ownership”，以及 wake 的哈希桶快速跳过/完整 key 与清位
  过滤；可推导提前检查再入队会丢唤醒、去掉地址过滤会误唤醒碰撞对象、缺失清位后全屏障会让
  waitqueue_active 快速路径与条件发布失序。waitqueue 锁保护链表，release/acquire 发布条件状态。
- 关联读取：`include/linux/wait_bit.h:1-340,520-590`（结构、内联快速路径、var 宏及屏障契约，
  缺失）、`kernel/sched/wait.c:250-410`（prepare/finish/autoremove 语义，缺失）、
  `kernel/softirq.c:1540-1560,1870-1920`（tasklet 位锁实例，充分）、`kernel/signal.c:430-460`
  （JOBCTL 清位唤醒屏障，充分）；只读未改。
- 修改安全：新增 135 行、删除 0 行，新增行仅注释；禁用模板前缀无命中，`git diff --check`
  和忽略 `LONG_LINE_COMMENT` 后 checkpatch 均为 0 errors/0 warnings。无 `.config`，未构建或执行
  信号、超时、哈希碰撞和 bit-lock 竞争运行时验证。
- 2026-08-25 密度复验：重新顺序检查 16 个函数、全局哈希表、key/entry 实体、全部英文注释及
  普通/排他/变量等待路径；在 `init_wait_var_entry()` 的复合字面量中新增 4 行，明确 key 固定、
  current/回调绑定和真正入队的阶段边界，删除 0 行。关联复读 `include/linux/wait_bit.h:249-280`
  与 `kernel/sched/wait.c:309` 的宏展开和 prepare 接口（前者缺失、后者已有部分中文覆盖），只读未改。
  固定密度门禁结果为 `code=168, comments=209, chinese=95, density=0.565, max_gap=9`，退出码 0；
  `git diff --check` 通过，忽略 `LONG_LINE_COMMENT` 后 checkpatch 为 0 errors/0 warnings。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

### `kernel/sched/autogroup.c`

- 文件职责与验收：18 个实现函数、SYSCTL 真/假分支、3 个全局实体与 proc 配置边界全量覆盖；
  默认组、动态 ag/tg 双向关系、signal 引用、fork/exit/attach/detach、启动禁用及 nice/path 展示
  均有紧邻契约。顶部及 8 组原英文注释逐字保留并完整翻译。
- 路径与并发抽查：抽查 create 的“ag 分配→tg 分配→RT 重定向→RCU online/失败退化 default”、
  move 的“siglock→新引用发布→逐线程 rq 迁移→旧引用 put”，以及 proc nice 的范围/LSM/capability/
  限速/shares 提交；可推导发布后直接释放失败、先 put 旧组或漏迁移退出线程分别造成可见半成品、
  UAF 或旧组残留。siglock 保护 signal 指针和线程链，rq 锁保护派生调度组，rwsem 串行 nice 展示。
- 关联读取：`kernel/sched/autogroup.h:1-154`（对象和配置无关接口，充分）、
  `kernel/sched/core.c:11830-12040`（task_group 创建/发布/两阶段 RCU 销毁/迁移，充分）、
  `kernel/sched/fair.c:15200-15285`（shares 全局 mutex 与逐 CPU rq 更新，缺失）、
  `kernel/fork.c:1300-1330,2850-2870`、`kernel/exit.c:1145-1160`、`kernel/sys.c:1285-1305`
  （生命周期调用点，充分）；只读未改。
- 修改安全：新增 135 行、删除 0 行，新增行仅注释；禁用模板前缀无命中，`git diff --check`
  和忽略 `LONG_LINE_COMMENT` 后 checkpatch 均为 0 errors/0 warnings。无 `.config`，未构建或执行
  session 创建、并发 cgroup 迁移、proc nice 与 RCU 回收运行时验证。
- 2026-08-25 密度复验：重新顺序检查 18 个实现函数、SYSCTL 配置分支、全局实体及全部英文注释；
  在 sysctl 表和 `autogroup_create()` 分配阶段各补 4 行语义注释，本次新增 8 行、删除 0 行。
  关联复读 `kernel/sysctl.c:867-891` 的 min/max 返回契约及 `kernel/sched/sched.h:623-630` 的
  task_group 生命周期声明（前者缺失中文学习覆盖，后者部分覆盖），只读未改。固定密度门禁结果为
  `code=198, comments=187, chinese=97, density=0.490, max_gap=8`，退出码 0；`git diff --check`
  通过，忽略 `LONG_LINE_COMMENT` 后 checkpatch 为 0 errors/0 warnings。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

### `kernel/sched/ext/cid.h`

- 文件职责与验收：17 个 inline/宏实体、14 个 cid/cmask 外部接口和 4 个全局表/集合声明全量
  覆盖；拓扑稠密编号、无拓扑尾段、双向查表、错误上报、cmask 容量/窗口/padding、racy 快照、
  迭代器及 BPF CPU 参数适配均有紧邻契约。10 组原英文导读/kernel-doc 逐字保留并完整翻译。
- 路径与并发抽查：抽查 cid 校验后稳定查表、reframe 的容量拒绝/首尾清零但 body 垃圾语义，
  以及 for_each 的逐 word READ_ONCE 和 padding 依赖；可推导未验证 cid 会越界、扩窗超过
  alloc_words 会破坏柔性数组、padding 非零会迭代出范围外 cid。表指针首次 WRITE_ONCE 发布且
  不撤销；普通 cmask 写由调用者串行，racy 版本只承诺逐 word 混合快照而非内存序。
- 关联读取：`kernel/sched/ext/cid.c:1-730`（表构建、override、cmask 运算和 BTF 注册，缺失）、
  `kernel/sched/ext/types.h:90-240`（topo/cmask 存储不变量，充分）、`kernel/sched/ext/ext.c:
  4900-4940,7150-7190,9440-9465,9890-9970,10020-10060,10250-10290,10770-10810`
  （初始化及 CPU 参数调用，缺失）；只读未改，cid.c 宜作为后续独立目标补注。
- 修改安全：新增 105 行、删除 0 行，新增行仅注释；禁用模板前缀无命中，`git diff --check`
  和忽略 `LONG_LINE_COMMENT` 后 checkpatch 均为 0 errors/0 warnings。无 `.config`，未构建或执行
  BPF override、CPU 热插拔重启与 racy cmask 并发验证。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

### `kernel/sched/cpupri.c`

- 文件职责与验收：7 个函数全量覆盖，讲清 INVALID/NORMAL/RT1..99/HIGHER 编码、每桶原子
  count+cpumask、CPU 反向桶表、亲和性/active/容量筛选、更新屏障和初始化回滚。顶部复杂度导读、
  转换表、查询竞态、fitness 退化及 3 组 kernel-doc 原文逐字保留并完整翻译。
- 路径与并发抽查：抽查单桶 reader 的“count→rmb→mask→二次非空验证”、fitness 的逐桶容量
  过滤与无 fitness 单层递归，以及 set 的“新 mask→屏障→新 count→跨桶屏障→旧 count→屏障→
  旧 mask”；可推导先删旧桶会短暂漏掉 CPU、count/mask 逆序会让 reader 错过发布、强制容量适配
  会阻止更高优先级 RT task 运行。更新由目标 rq 锁串行，查询无锁，余下偏差由 pull/rebalance 收敛。
- 关联读取：`kernel/sched/cpupri.h:1-124`（结构与公开契约，充分）、`kernel/sched/rt.c:
  1040-1085,1560-1600,1770-1810,2380-2410`（最高优先级更新、抢占和容量调用，缺失）、
  `kernel/sched/deadline.c:2210-2250`（DL HIGHER 覆盖，缺失）、`kernel/sched/topology.c:430-575`
  （root_domain 初始化回滚与 RCU 销毁，部分覆盖）；只读未改。
- 修改安全：新增 109 行、删除 0 行；审计中发现并恢复过一条原有空行，最终新增行仅注释或空行。
  禁用模板前缀无命中，`git diff --check` 和忽略 `LONG_LINE_COMMENT` 后 checkpatch 均为
  0 errors/0 warnings。无 `.config`，未构建或执行 RT push/pull、容量退化和 CPU 热插拔验证。
- 2026-08-25 密度复验：重新顺序检查 7 个函数、桶实体、英文注释、查询/更新屏障和初始化回滚；
  在 `convert_prio()` 的端点编码以及 `cpupri_init()` 的两阶段 ownership 边界各补 4 行注释，
  本次新增 8 行、删除 0 行。关联复读 `cpupri.h`、`rt.c` 和 `topology.c` 的公开契约、rq 锁调用点
  与 root_domain 初始化/销毁配对（覆盖状态沿用上条记录），只读未改。固定密度门禁结果为
  `code=122, comments=274, chinese=79, density=0.648, max_gap=9`，退出码 0；`git diff --check`
  通过，忽略 `LONG_LINE_COMMENT` 后 checkpatch 为 0 errors/0 warnings。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

### `kernel/sched/stats.h`

- 文件职责与验收：schedstats 静态键/宏、PSI enqueue/dequeue/switch 和 sched_info 等待/执行统计，
  含所有配置关闭 stub 全量覆盖；原英文锁、PSI 状态迁移及 sched_info 说明逐字保留并完整翻译。
- 并发抽查：rq 锁保证统计字段更新；PSI 区分睡眠、迁移和 proxy execution；跨 CPU 等待按每个 rq
  分段结算消除时钟偏差。可推导漏掉 switch 合并会重复祖先遍历，重置 last_queued 会少计等待。
- 关联读取：`kernel/sched/stats.c:1-311`（三个统计实现，充分）、`kernel/sched/psi.c`（PSI 状态消费，
  缺失）、`kernel/sched/core.c`（静态键与 sched_move 调用，部分覆盖）；只读未改。
- 修改安全：新增 89 行、删除 0 行，仅注释；`git diff --check` 与忽略 `LONG_LINE_COMMENT` 后
  checkpatch 为 0 errors/0 warnings。无 `.config`，未构建或运行 PSI/统计验证。
- 2026-08-25 密度复验：重新顺序检查 schedstats、PSI、sched_info 三组配置接口及关闭态 stub，
  在未编译 schedstats 的中性读取语义和 `sched_info_arrive()` 的“提交总量→维护极值→汇入 rq”
  阶段新增 5 行，删除 0 行。关联语义继续由 `stats.c`、`psi.c` 和调度核心调用点支撑，覆盖状态
  沿用上条记录。固定密度门禁结果为
  `code=218, comments=182, chinese=68, density=0.312, max_gap=10`，退出码 0；
  `git diff --check` 通过，忽略 `LONG_LINE_COMMENT` 后 checkpatch 为 0 errors/0 warnings。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

### `kernel/sched/cpuacct.c`

- 文件职责与验收：19 个函数、cpuacct/cpustat per-CPU 存储、层级对象和 cgroup v1 文件表全量
  覆盖；创建回滚、root 静态特例、总量/分类/逐 CPU 输出和 reset 路径均已说明。
- 并发抽查：rq 锁保护计费，32 位读写以 rq 锁防 u64 撕裂；跨 CPU 展示/reset 仅是近似快照；
  charge 沿祖先含 root 累计，而 account_field 在 root 前停止以避免全局重复计费。
- 关联读取：`kernel/sched/cputime.c` 与 cgroup core 调用点（缺失）；本文件的 cputime_adjust 契约
  由现有 cputime 注释核对，只读未改。
- 修改安全：新增 64 行、删除 0 行，仅注释；`git diff --check` 与忽略行长后 checkpatch 为
  0 errors/0 warnings。无 `.config`，未构建或运行 cgroup 计费/reset 验证。
- 2026-08-25 密度复验：重新顺序检查 19 个函数、枚举/对象/per-CPU 存储、文件表、全部英文注释
  和分配/读取/reset/计费路径；补充动态资源 ownership、NSTATS 特例、逐 CPU 快照、legacy 调整
  及文件表分组，共新增 20 行、删除 0 行。关联复读 `include/linux/cgroup.h:809-839` 的配置接口
  和计费包装（缺失系统中文覆盖），只读未改。固定密度门禁结果为
  `code=267, comments=125, chinese=62, density=0.232, max_gap=10`，退出码 0；
  `git diff --check` 通过，忽略 `LONG_LINE_COMMENT` 后 checkpatch 为 0 errors/0 warnings。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

### `kernel/sched/loadavg.c`

- 文件职责与验收：14 个函数及全局 active/load 状态全量覆盖；说明分布式运行/不可中断计数折叠、
  固定点指数衰减、NO_HZ 双缓冲 delta 和跨多个采样周期追赶，原英文推导与注释逐字保留并翻译。
- 并发抽查：各 CPU 周期性提交相对 active 变化，`nr_uninterruptible` 只保证全局和正确；NO_HZ
  写端以屏障在索引切换前提交旧槽，读端用 `xchg()` 单次消费，10 tick 窗口容纳远端 CPU 折叠。
- 关联读取：`kernel/sched/sched.h` 的 rq 负载字段和 `include/linux/sched/loadavg.h` 的定点常量
  （部分覆盖）；调用与公式已由本文件注释核对，只读未改。
- 修改安全：新增 68 行、删除 0 行，仅注释；`git diff --check` 与忽略行长后 checkpatch 为
  0 errors/0 warnings。无 `.config`，未构建或运行 NO_HZ/loadavg 周期验证。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

#### 2026-08-25 密度复验

- 在 `calc_load_fold_active()` 的增量基线更新和 `fixed_power_int()` 的二进制平方求幂循环中补充
  3 行机制注释，消除 11/21 行连续代码空窗；未改动原有注释或可执行代码。
- 固定门禁通过：有效代码 149 行、中文注释 49 行、密度 0.329、最大连续无中文注释代码 10 行。
- 本次差异新增 3 行、删除 0 行且均为注释；`git diff --check` 与忽略
  `LONG_LINE_COMMENT` 后 checkpatch 均为 0 errors/0 warnings。

### `kernel/sched/wait.c`

- 文件职责与验收：24 个等待队列入口全量覆盖；说明普通、独占、优先级队列次序，唤醒回调
  返回值与名额消费，prepare/schedule/finish 生命周期，以及信号、pollfree、超时和停止路径。
- 并发抽查：队列锁并关闭本地中断串行链表；先入队后 `set_current_state()` 防止丢失唤醒；
  `wait_woken()` 的 A/B/C 屏障分别与任务唤醒和条件发布配对；careful 链表检查保护栈项释放。
- 关联读取：`include/linux/wait.h:13-219,288-340,1225-1240`（结构、flags、内联入队和宏调用，
  充分）；关联头文件只读未改。
- 修改安全：新增 105 行、删除 0 行，仅注释；`git diff --check` 与忽略行长后 checkpatch 为
  0 errors/0 warnings。无 `.config`，未构建或运行信号/超时/poll 并发验证。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

#### 2026-08-25 新函数契约与密度复验

- 26 个函数逐一增加专属“业务背景、入参、出参/返回、注意事项”契约，四类标签各 26 处且均紧邻
  对应声明；逐参数核对可空性、输入输出属性、单位/范围和 ownership，并显式覆盖 `void`、0、布尔值、
  `-EBUSY`、`-ERESTARTSYS`、剩余 jiffies 与唤醒回调 0/正值等返回类别。
- 语义复核补充 `nr_exclusive=0` 时锁内核心返回负的成功计数、`wake_up_pollfree()` 要求 RCU 延迟释放、
  lockdep 名称/key 的持久生命周期，以及 do_wait_intr 两种入口的锁/中断/可睡眠契约。
- 关联读取：`include/linux/wait.h:211-273,776-825,1225-1250`（唤醒宏、POLLFREE 的 RCU 协议、
  locked wait 宏和等待项定义，部分覆盖）及 `include/linux/lockdep.h:128-176`、
  `kernel/locking/lockdep.c:7744-7834`（名称/key 生命周期，充分）；均只读未改。
- 固定门禁通过：有效代码 249 行、中文注释 181 行、密度 0.727、最大连续无中文注释代码 10 行。
  本次差异新增 176 行、删除 0 行且全部为注释；`git diff --check` 与忽略
  `LONG_LINE_COMMENT` 后 checkpatch 均为 0 errors/0 warnings。无 `.config`，未构建或运行并发验证。

### `kernel/sched/pelt.c`

- 文件职责与验收：13 个函数及 RT、DL、HW pressure、IRQ 配置路径全量覆盖；说明约 1ms
  分段、32ms 半衰期的定点衰减，sum 三段累计、avg 归一化和各调度类输入含义，原公式保留。
- 并发抽查：调用者持 rq 锁并推进同一时钟域；倒退时钟只重置基准；不足 1024ns 留作尾数；
  `WRITE_ONCE(util_avg)` 发布单字段结果，按位 OR 保证四个子系统更新均执行。
- 关联读取：`kernel/sched/pelt.h:1-180`（公开入口、配置 stub、divider 与调用契约，充分）、
  `include/linux/sched.h:609-626`（sched_avg 字段布局，充分）；只读未改。
- 修改安全：新增 79 行、删除 0 行，仅注释；审计中发现并恢复过原英文注释缩进，最终原文
  逐行保留；`git diff --check` 与忽略行长后 checkpatch 为 0 errors/0 warnings。无 `.config`，
  未构建或运行 PELT/IRQ/HW pressure 验证。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

#### 2026-08-25 新函数契约与密度复验

- 13 个函数逐一增加专属“业务背景、入参、出参/返回、注意事项”契约，四类标签各 13 处且紧邻
  对应声明；明确 ns、1024ns 量化单位、PELT 段、jiffies/容量刻度，逐参数记录范围、可空性、
  输入输出属性与 ownership，并覆盖 `void`、0/1、0..2 和布尔返回。
- 语义复核覆盖衰减查表、三段贡献、时钟倒退/残段副作用、sum→avg 发布、RT/DL 二值状态、
  HW 连续容量损失、IRQ 区间尾近似和非 CFS 四路按位 OR；函数体另补 11 个阶段/配置边界说明。
- 关联复读 `kernel/sched/pelt.h:1-180`（入口契约、配置 stub、时间轴和 divider，充分）以及
  `fair.c`、`rt.c`、`deadline.c`、`core.c`、`ext/ext.c` 中的调用点（锁/时钟/状态来源，部分覆盖），
  均只读未改。
- 固定门禁通过：有效代码 179 行、中文注释 110 行、密度 0.615、最大连续无中文注释代码 10 行。
  本次差异新增 92 行、删除 0 行且全部为注释；`git diff --check` 与忽略
  `LONG_LINE_COMMENT` 后 checkpatch 均为 0 errors/0 warnings。无 `.config`，未构建或运行 PELT 验证。

### `kernel/sched/core_sched.c`

- 文件职责与验收：12 个函数与 cookie 引用对象全量覆盖；说明 CREATE/GET/SHARE_TO/
  SHARE_FROM、线程/线程组/进程组 scope、fork/free 配对和 SCHEDSTATS forced-idle 路径。
- 并发抽查：`pi_lock` 稳定 cookie 克隆，`task_rq_lock` 串行树摘插和运行任务重调度；组操作
  在 tasklist 锁下先全员权限预检再修改；cookie 最后引用与全局 core scheduling 引用配对。
- 关联读取：`kernel/sched/sched.h:1356,1508-1605`（任务字段与匹配/配置接口，充分）、
  `kernel/sched/core.c:667-710,7850-8425`（全局启停与选择消费，部分覆盖）、
  `include/uapi/linux/prctl.h:285-293`（UAPI 命令与 scope，充分）；只读未改。
- 修改安全：新增 73 行、删除 0 行，仅注释；`git diff --check` 与忽略行长后 checkpatch 为
  0 errors/0 warnings。无 `.config`，未构建或运行 prctl/SMT forced-idle 验证。
- 2026-08-25 密度复验：重新顺序检查 12 个函数、cookie 引用实体、全部命令/作用域、错误出口及
  forced-idle 统计；在 PID 引用稳定、SHARE_FROM 汇合、CREATE/SHARE_TO 公共路径和统计变量地图
  补充 13 行，删除 0 行。关联复读 `kernel/sys.c:2990`、`kernel/fork.c:1385,4300,4484` 和
  `sched.h:3533-3538` 的入口、fork/free 配对与统计包装（已有充分或部分中文覆盖），只读未改。
  固定密度门禁结果为 `code=196, comments=137, chinese=55, density=0.281, max_gap=10`，退出码 0；
  `git diff --check` 通过，忽略 `LONG_LINE_COMMENT` 后 checkpatch 为 0 errors/0 warnings。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

### `kernel/sched/clock.c`

- 文件职责与验收：不稳定配置下 20 个函数、稳定配置下 2 个替代实现及全局/per-CPU 状态
  全量覆盖；原文件已有中文注释保留，补齐文件模型、static key、远端原子同步和 idle 路径。
- 并发抽查：tick 在关中断下配对采样；NMI/普通上下文以 cmpxchg64 推进单调值；32 位远端读取
  防 u64 撕裂；early/late 稳定性屏障保证 static key 更新不会被双方同时漏掉。
- 关联读取：`include/linux/sched/clock.h:1-105`（公开契约和配置 stub，充分）、
  `kernel/time/sched_clock.c`（generic clock 注册，部分覆盖）、`kernel/sched/core.c:1124-1155`
  （rq 时钟消费，充分）；只读未改。
- 修改安全：新增 72 行、删除 0 行，仅注释；`git diff --check` 与忽略行长后 checkpatch 为
  0 errors/0 warnings。无 `.config`，未构建或运行 TSC 降级、idle/suspend、32 位 NMI 验证。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

#### 2026-08-25 新函数契约与密度复验

- 两个配置分支合计 26 个函数定义逐一增加专属“业务背景、入参、出参/返回、注意事项”契约，
  四类标签各 26 处；明确无参/void、CPU 编号范围、纳秒量纲、借用指针、static-key 副作用、0/非零
  与 initcall 返回，并分别记录 IRQ、抢占、NMI/noinstr、watchdog lock 和工作队列上下文。
- 语义复核覆盖启动期 weak 时钟、GTOD/raw 偏移、稳定性双向切换、u64 回绕、per-CPU 原子裁剪、
  32/64 位远端耦合、tick/idle 配对和稳定架构替代实现；函数体补充 11 个原子重试及发布阶段说明。
- 关联复读 `include/linux/sched/clock.h` 的公共声明、`include/linux/lockdep.h` 的上下文约束及本文件
  两个配置分支；只读未改。固定门禁结果为有效代码 256 行、中文注释 224 行、密度 0.875、
  最大连续无中文注释代码 10 行。
- 本次差异新增 167 行、删除 0 行且全部为注释；`git diff --check` 与忽略
  `LONG_LINE_COMMENT` 后 checkpatch 均为 0 errors/0 warnings。无 `.config`，未构建或运行跨 CPU/idle 验证。

### `kernel/sched/membarrier.c`

- 文件职责与验收：13 个函数、命令/配置掩码、两级 IPI mutex 和 A-E 五类屏障场景全量覆盖；
  说明 global/private、SYNC_CORE、RSEQ、CPU 定向、注册查询、exec 清理与失败返回。
- 并发抽查：作用域首尾 `smp_mb()` 与 rq->curr 修改配对；RCU 稳定 curr，CPU read lock 稳定
  在线集合；注册先置功能位、同步 rq 后置 READY，分配失败保留可重试的非 READY 状态。
- 关联读取：`include/linux/sched/mm.h:516-575`（状态位、switch/配置 stub，充分）、
  `include/uapi/linux/membarrier.h`（命令与 flags，充分）、`kernel/sched/core.c:6960-6990`
  （上下文切换屏障，充分）；只读未改。
- 修改安全：新增 94 行、删除 0 行，仅注释；`git diff --check` 与忽略行长后 checkpatch 为
  0 errors/0 warnings。无 `.config`，未构建或运行 membarrier litmus、RSEQ、CPU hotplug 验证。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

#### 2026-08-25 新函数契约与密度复验

- 14 个函数定义（含 initcall 与 `SYSCALL_DEFINE3` 生成入口）逐一增加专属“业务背景、入参、
  出参/返回、注意事项”契约，四类标签各 14 处；覆盖隐式 `current->mm`、可空 IPI info、flags/cpu_id
  范围、借用对象和 0/掩码/`-EINVAL`/`-EPERM`/`-ENOMEM`/体系结构 `-ENOSYS` 返回类别。
- 语义复核覆盖 A-E 屏障配对、GLOBAL/PRIVATE 目标筛选、SYNC_CORE/RSEQ 当前 CPU 差异、注册功能位
  与 READY 提交点、exec 清理、rq 快照传播、RCU/hotplug/mutex 生命周期和 nohz_full 限制；补充
  32 个函数体阶段与配置掩码说明。
- 关联复读 `include/uapi/linux/membarrier.h` 的命令/标志定义、`sched.h` 的 rq/mm 状态以及调度切换、
  `exit_mm`、kthread use/unuse mm 与 rseq 调用点（部分覆盖），均只读未改。
- 固定门禁通过：有效代码 338 行、中文注释 147 行、密度 0.435、最大连续无中文注释代码 10 行。
  本次差异新增 116 行、删除 0 行且全部为注释；`git diff --check` 与忽略
  `LONG_LINE_COMMENT` 后 checkpatch 均为 0 errors/0 warnings。无 `.config`，未构建或运行 IPI/rseq 验证。

### `kernel/sched/ext/cid.c`

- 文件职责与验收：25 个函数、2 个 op enum、三张永久表和两组 BTF kfunc 集全量覆盖；说明
  node/LLC/core 交集建图、无拓扑尾段、root override、cmask 区间运算及查询失败输出。
- 并发抽查：表首次 WRITE_ONCE 发布后不撤销；默认映射在 ops.init 前完成，override 返回前
  提交；BPF 查询持 RCU；`_racy` 只提供逐 word data_race 混合快照，内存序由调用者承担。
- 关联读取：`kernel/sched/ext/cid.h:1-410`（模型、结构、内联边界与已有中文契约，充分）、
  `kernel/sched/ext/types.h:96-113`（拓扑结果布局，充分）、`kernel/sched/ext/ext.c:7150-7190`
  （初始化调用，充分）；只读未改。
- 修改安全：新增 94 行、删除 0 行，仅注释；`git diff --check` 与忽略行长后 checkpatch 为
  0 errors/0 warnings。无 `.config`，未构建或运行 BPF override、CPU hotplug、cmask 边界测试。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

#### 2026-08-25 新函数契约与密度复验

- 26 个真实函数逐一增加专属“业务背景、入参、出参/返回、注意事项”契约，四类标签各 26 处；
  逐参数覆盖 CPU/CID 范围、字节长度、半开 cmask 窗口、借用/输出对象、BPF 隐式 aux、可睡眠性和
  0/布尔/CPU/CID/首个注册错误/`-EINVAL`/`-ENOMEM` 返回类别。
- 语义复核覆盖 node→LLC→core 连续构图、永久表发布、无拓扑尾段、root override 半成品隔离、
  BPF 输出初始化、双 mask 交集/padding、subset 范围外检查、RACY 混合快照和 BTF kfunc 注册；
  函数体补充 41 个分配、遍历、短路、原子发布与配置阶段说明。
- 关联复读 `kernel/sched/ext/cid.h` 的表/inline/cmask 契约、`ext/types.h` 的结构布局以及 `ext.c`
  的分配与调用点（部分覆盖），均只读未改。
- 固定门禁通过：有效代码 430 行、中文注释 208 行、密度 0.484、最大连续无中文注释代码 10 行。
  本次差异新增 200 行、删除 0 行且全部为注释；`git diff --check` 与忽略
  `LONG_LINE_COMMENT` 后 checkpatch 均为 0 errors/0 warnings。无 `.config`，未构建或加载 BPF scheduler。

### `kernel/sched/cpufreq_schedutil.c`

- 文件职责与验收：35 个函数、policy/CPU/tunables 三类状态、sysfs 属性和 governor 描述符
  全量覆盖；说明 IO-wait boost、util→频率/性能映射、共享 policy 聚合及 fast/slow 路径。
- 并发抽查：目标 rq 锁串行 per-CPU 更新，shared policy/update work 由 raw spinlock 串行；
  慢切换经 irq_work→DL kthread→mutex；limits 标志屏障配对；STOP 摘 hook 后等待 RCU 和 work。
- 关联读取：`kernel/sched/cpufreq.c:1-130`（update-util RCU 槽与 remote DVFS，充分）、
  `include/linux/cpufreq.h:600-680`（driver fast/slow/adjust_perf 接口，充分）、
  `drivers/cpufreq/cpufreq.c:2470-2700`（governor/limits 生命周期，部分覆盖）；只读未改。
- 修改安全：新增 153 行、删除 0 行，仅注释；`git diff --check` 与忽略行长后 checkpatch 为
  0 errors/0 warnings。无 `.config`，未构建或运行 fast/slow driver、CPU offline、sysfs 压测。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

#### 2026-08-25 新函数契约与密度复验

- 两个 `CONFIG_NO_HZ_COMMON` 分支合计 37 个函数定义逐一增加紧邻的“业务背景、入参、
  出参/返回、注意事项”契约，四类标签各 37 处；逐参数记录 policy/CPU/容量/纳秒/kHz/flags 范围、
  借用或输出 ownership、无参/void，以及布尔、频率、字节数、NULL 和全部负 errno 返回类别。
- 语义复核覆盖 util 到频率/性能映射、IO-wait boost 增长与衰减、单 CPU 和共享 policy 聚合、
  rq/update/work 三层串行、fast/slow 提交、limits 屏障、sysfs tunables 引用及
  INIT→START→STOP→EXIT 的发布和逆序回滚；函数体和实体补充 37 个状态阶段说明。
- 关联复读 `kernel/sched/cpufreq.c` 的 update-util RCU 槽与 remote-DVFS 判定、
  `include/linux/cpufreq.h` 的 adjust_perf 和 gov_attr_set 接口；已有覆盖充分，只读未改。
- 固定门禁通过：有效代码 571 行、中文注释 294 行、密度 0.515、最大连续无中文注释代码 10 行。
  本次差异新增 269 行、删除 0 行且全部为注释；`git diff --check` 与忽略
  `LONG_LINE_COMMENT` 后 checkpatch 均为 0 errors/0 warnings。无 `.config`，未构建或运行调频验证。

### `kernel/sched/cputime.c`

- 文件职责与验收：按 ctags 与条件编译分支逐项覆盖 tick、IRQ、steal、NO_HZ idle、native/generic
  vtime、task/thread-group/cgroup/cpustat 计费入口；原英文公式、分类顺序和竞态说明原样保留。
- 路径与并发抽查：抽查 tick 先扣 steal/IRQ 再互斥归类、NO_HZ idle 延迟一轮扣 steal 防读数倒退、
  generic vtime switch 的 INACTIVE 窗口及远端重试；可推导重复归类会双计时间、直接扣本轮 steal
  会让公开 idle 值倒退、无 seqcount 重试会把两代 task 状态拼成无效快照。
- 关联读取：`include/linux/sched/cputime.h:1-180`（公开 task/thread-group API 与 POSIX timer 累计，
  缺失）、`kernel/time/timer.c:3237`（tick 调用，缺失）；只读未改，建议后续独立补注头文件。
- 修改安全：新增 130 行、删除 0 行，仅注释；禁用前缀无命中，`git diff --check` 与忽略中文
  `LONG_LINE_COMMENT` 后 checkpatch 为 0 errors/0 warnings。无 `.config`，未构建或运行 vtime/NO_HZ。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

#### 2026-08-25 新函数契约与密度复验

- 按条件编译展开共 64 个函数定义（57 个唯一名称及 7 个同名配置替代实现）逐一增加紧邻的
  “业务背景、入参、出参/返回、注意事项”契约，四类标签各 64 处；覆盖 task/线程组/CPU/index、
  纳秒与微秒量纲、可空输出、借用 ownership、void/布尔/时间值和 `-EAGAIN` 返回类别。
- 语义复核覆盖 IRQ/steal/tick 互斥扣除、guest 是 user 子集、NO_HZ idle 延迟扣 steal、native 与
  generic vtime 配置分支、prev_cputime 单调校正、task/thread-group 快照、context-switch INACTIVE
  窗口及 RCU+seqcount 远端 cpustat 重试；函数体补充 60 个分类、发布和重试阶段说明。
- 关联复读 `include/linux/sched/cputime.h` 的 task/thread-group 公共接口、`kernel/time/timer.c` 的
  tick 调用点和 `kernel/sched/sched.h` 相关状态；只读未改。
- 固定门禁通过：有效代码 818 行、中文注释 406 行、密度 0.496、最大连续无中文注释代码 10 行。
  本次差异新增 444 行、删除 0 行且全部为注释；`git diff --check` 与忽略
  `LONG_LINE_COMMENT` 后 checkpatch 均为 0 errors/0 warnings。无 `.config`，未构建或运行 vtime/NO_HZ 验证。

### `kernel/sched/debug.c`

- 文件职责与验收：覆盖 feature/static-key、动态抢占/cache/scaling、fair/ext deadline server、
  sched-domain debugfs、rq/class/task/proc/SysRq 展示及 resched latency 限速告警；所有配置 stub 在列。
- 路径与并发抽查：feature 写由 hotplug 读锁+inode 锁维持位图/static-key 一致；server 参数在 rq
  irqsave 锁下 stop→apply→start；CFS 树边界在 rq 锁下复制后解锁打印，其他字段明确是诊断快照。
- 关联读取：`kernel/sched/sched.h:2264,3405`（debugfs/latency 声明，部分覆盖）、
  `kernel/sched/core.c:7371`、`kernel/sched/topology.c:3519`（调用点，缺失）；只读未改。
- 修改安全：新增 93 行、删除 0 行，仅注释；禁用前缀、diff/checkpatch 检查通过；无 `.config`，
  未构建或实际读写 debugfs、触发 SysRq/latency warning。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

#### 2026-08-25 新函数契约与密度复验

- 按条件编译展开共 62 个真实函数定义（ctags 的 77 个唯一条目剔除 17 个打印宏/枚举，再计入
  2 个 jump-label 配置替代实现）逐一增加紧邻的四项契约，四类标签各 62 处；覆盖用户指针、
  文件偏移、seq/inode/file、CPU/rq/domain/task、纳秒参数、可空控制台输出及全部 errno/字节数返回。
- 语义复核覆盖 feature 位图与 static key、dynamic preempt/cache/scaling 控制面、verbose/domain 树
  重建、deadline server stop→apply→start、debugfs 生命周期、rq/class/task 快照、seq CPU 编码、
  SysRq watchdog、schedstats/PELT/uclamp/NUMA 和 resched 告警；补充 137 个阶段说明。
- 关联复读 `kernel/sched/sched.h` 的 domain/debug/server 声明、`kernel/sched/topology.c` 的重建调用、
  `kernel/sched/core.c` 的 latency 告警入口及 `kernel/sched/deadline.c` 的 server 实现；只读未改。
- 固定门禁通过：有效代码 1159 行、中文注释 460 行、密度 0.397、最大连续无中文注释代码 10 行。
  本次差异新增 509 行、删除 0 行且追加式审计仅有注释；`git diff --check` 与忽略
  `LONG_LINE_COMMENT` 后 checkpatch 均为 0 errors/0 warnings。无 `.config`，未构建或操作 debugfs/SysRq。

### `kernel/sched/ext/idle.c`

- 文件职责与验收：覆盖全局/per-node idle 与全闲 SMT mask、LLC/NUMA 选核层次、enable/reset/update、
  BPF kfunc 校验与 BTF 注册；说明 idle 位领取是可竞争候选，非最终迁移或运行承诺。
- 路径与并发抽查：选核禁抢占保护 per-CPU 临时 mask、RCU 保护 topology span；rq 锁下先更新内建
  mask 后回调 BPF，保证 enqueue/update_idle 互锁；KF_ACQUIRE/RELEASE 只建立 verifier trusted
  pointer 生命周期，永久 mask 不实际增减引用。抽查无候选、非法 node、cross-task 锁不覆盖路径。
- 关联读取：`kernel/sched/ext/idle.h:40-100`（公开生命周期与返回契约，充分）、
  `kernel/sched/ext/ext.c:3372,7188`（默认选核/启用调用，缺失）；只读未改。
- 修改安全：新增 71 行、删除 0 行，仅注释；禁用前缀、diff/checkpatch 检查通过；无 `.config`，
  未构建或运行 SMT/NUMA、CPU hotplug、BPF test_run 与 sub-scheduler 验证。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

#### 2026-08-25 新函数契约与密度复验

- 两个 NUMA 配置分支和全部 BPF kfunc 合计 40 个真实函数定义逐一增加紧邻的“业务背景、入参、
  出参/返回、注意事项”契约，四类标签各 40 处；覆盖 mask 借用与 verifier acquire/release、RCU、
  rq/pi 锁、禁抢占 scratch、CPU/node 范围、idle 领取竞态及全部 errno/fallback 类别。
- 关联读取：`kernel/sched/ext/idle.h:46-89`（公开 enable/disable/topology/select 契约）、
  `kernel/sched/ext/ext.c:3420,3479,6245,7260,7294`（默认选核、hotplug 与生命周期调用）；只读未改。
- 修改安全：本轮累计新增 320 行、删除 0 行，追加式正则未发现代码或旧内容变更；
  `git diff --check` 通过，忽略 `LONG_LINE_COMMENT` 后 checkpatch 为 0 errors/0 warnings。密度结果为
  `code=706, comments=1008, chinese=296, density=0.419, max_gap=10`。无 `.config`，未构建或运行
  SMT/NUMA、CPU hotplug、BPF verifier、test_run 与并发 idle 领取验证。
- 最终状态：**全文件完成**；已按新增函数契约门禁和方法论第 17 章完成强制验收。

### `kernel/sched/syscalls.c`

- 文件职责与验收：覆盖 nice、policy/priority/sched_attr、uclamp、affinity、yield 与 RR interval 的
  syscall、内核 wrapper、权限/LSM/admission 及配置 stub；说明旧 ABI 到 sched_attr 的规范化。
- 路径与并发抽查：可睡眠权限/uclamp static-key 检查位于 rq 锁外，锁后 policy 变化走 recheck；
  sched_change scope 负责 dequeue→参数/class 修改→enqueue；affinity 首次提交后重读 cpuset，竞态
  收缩时二次限制并返回 -EINVAL。yield 明确不提供进度保证，yield_to 以 pi_lock+双 rq 锁稳定目标。
- 关联读取：`kernel/sched/core.c:760-825`（pi_lock/rq 锁保护字段，部分覆盖）、
  `kernel/sched/core.c:4235-4280`（affinity 恢复调用，充分）；只读未改。
- 修改安全：新增 88 行、删除 0 行，仅注释；禁用前缀、diff/checkpatch 检查通过；无 `.config`，
  未构建或执行 capability/LSM、DL admission、cpuset 竞态和各 syscall ABI 测试。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

#### 2026-08-25 新函数契约与密度复验

- 两个配置分支及 syscall 宏合计 58 个真实函数定义逐一增加紧邻的“业务背景、入参、出参/返回、
  注意事项”契约，四类标签各 58 处；逐参数记录含义、范围或单位、可空性、输入/输出属性与 ownership，
  无参数、无输出参数和 `void` 返回均显式说明。函数体阶段注释覆盖 ABI 版本化复制、权限/LSM、uclamp
  锁外启用、rq/cpuset/PI 锁序、DL 准入、竞态重试、affinity 所有权及 yield 的非进度语义。
- 关联读取：`kernel/sched/core.c:760-825,4235-4280`（pi_lock/rq 锁保护和 affinity 恢复调用）、
  `kernel/sched/sched.h`（sched_change、rq 与 affinity 内部契约）；均只读未改。
- 修改安全：本轮累计新增 443 行、删除 0 行，追加式正则未发现代码或旧内容变更；
  `git diff --check` 通过，忽略 `LONG_LINE_COMMENT` 后 checkpatch 为 0 errors/0 warnings。密度结果为
  `code=902, comments=979, chinese=399, density=0.442, max_gap=10`。ctags 邻近契约检查唯一报告项为
  `affinity_context` 复合字面量，并非函数定义。无 `.config`，未构建或运行 capability/LSM、DL admission、
  cpuset 竞态和 syscall ABI 测试。
- 最终状态：**全文件完成**；已按新增函数契约门禁和方法论第 17 章完成强制验收。

### `kernel/sched/psi.c`

- 文件职责与验收：覆盖 per-CPU task 状态→SOME/FULL/NONIDLE 派生、加权聚合与衰减平均、普通和
  实时 trigger、poll/proc、IRQ、memstall 与 cgroup 生命周期；顶部数学模型和英文并发推导保留。
- 路径与并发抽查：rq 锁串行 task 状态，per-CPU seqcount 使聚合快照可重试；cgroup move 按旧 flags
  从旧树扣除→RCU 发布 css_set→向新树加回；trigger 摘链/清 RCU task 后等 grace period，再在
  mutex 外 stop psimon 防死锁。rtpoll atomic_xchg 全屏障保证 worker 漏状态时写端必重新调度。
- 关联读取：`kernel/sched/stats.h:200-280`（enqueue/dequeue/switch 合并规则，充分）、
  `include/linux/psi.h:18-65`（公开接口，缺失）、`kernel/cgroup/cgroup.c:5440-5540`（cgroup 文件
  trigger 的 release/acquire 发布，充分）；只读未改，头文件可作为后续独立补注候选。
- 修改安全：新增 95 行、删除 0 行，仅注释；禁用前缀、diff/checkpatch 检查通过；无 `.config`，
  未构建或运行压力负载、短窗 psimon、cgroup 删除/poll 与 IRQ accounting 验证。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

#### 2026-08-25 新函数契约与密度复验

- 54 个真实函数定义逐一增加紧邻的“业务背景、入参、出参/返回、注意事项”专属契约，四类标签各
  54 处；逐参数说明含义、范围或单位、可空性、输入/输出属性与 ownership，无参数、无输出参数和
  `void` 返回均显式记录。函数体阶段注释覆盖 seqcount 快照、分层状态传播、平均/短窗聚合、RCU
  worker 生命周期、cgroup 搬运以及 proc/poll 事件发布与消费。
- 关联读取：`include/linux/psi.h:18-65`（公开接口和禁用配置 stub）、`kernel/sched/core.c:7352,9145`
  （IRQ accounting 调用边界）、`kernel/sched/sched.h`（rq 锁与内部 PSI 声明）；均只读未改。
- 修改安全：本轮累计新增 442 行、删除 0 行，追加式正则未发现代码/旧内容变更；`git diff --check`
  通过，忽略中文 UTF-8 字节导致的 `LONG_LINE_COMMENT` 后 checkpatch 为 0 errors/0 warnings。密度结果
  为 `code=972, comments=1011, chinese=406, density=0.418, max_gap=10`，两项阈值均通过。
  工作树无 `.config`，未构建或运行压力负载、psimon、cgroup 删除竞态与 IRQ accounting 测试。
- 最终状态：**全文件完成**；已按新增函数契约门禁和方法论第 17 章完成强制验收。

### `kernel/sched/ext/internal.h`

- 文件职责与验收：覆盖 SCX 内部 ops ABI、全局/per-CPU 状态、退出诊断、dispatch buffer、子调度器
  指针和关键状态枚举；说明 QUEUEING/QUEUED/DISPATCHING 与 qseq 防 ABA 约束，以及 arena 地址边界。
- 并发与推理抽查：rq 锁保护本地状态，release/acquire 发布队列阶段，RCU 保护 scheduler/parent 借用
  指针；抽查 ops 调用宏、task/scheduler 查找和 bypass 快速路径，可推导遗漏阶段复验会重复 dispatch。
- 关联读取：`kernel/sched/ext/ext.c`、`kernel/sched/ext/types.h`、`kernel/sched/ext/idle.h` 用于核对调用、
  状态布局和 idle 契约；`ext.c` 已在后续批次完成，其余关联文件只读。
- 修改安全：新增 81 行、删除 0 行，仅注释；禁用前缀、`git diff --check` 与忽略行长后 checkpatch
  通过。无 `.config`，未构建或运行 BPF scheduler/热插拔验证。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

#### 2026-08-25 新函数契约与密度复验

- 两个配置分支合计 16 个真实 inline 函数定义逐一增加紧邻的“业务背景、入参、出参/返回、注意事项”
  契约，四类标签各 16 处；逐参数说明锁/RCU 保护、可空性、借用 ownership、配置差异和失败语义。
  同时按 ops 生命周期、cpu/cid ABI 对齐、事件计数、scheduler 资源所有权与调用宏递归上下文补充实体说明。
- 关联读取：`kernel/sched/ext/ext.c`（arena、locked rq、父子 scheduler 和 prog 关联的主要调用点）、
  `kernel/sched/ext/cid.c` 与 `kernel/sched/ext/idle.c`（CID/idle kfunc 的关联查找）；均只读未改。
- 修改安全：本轮累计新增 154 行、删除 0 行，追加式正则未发现代码或旧内容变更；
  `git diff --check` 通过，忽略 `LONG_LINE_COMMENT` 后 checkpatch 为 0 errors/0 warnings。密度结果为
  `code=557, comments=1295, chinese=192, density=0.345, max_gap=10`。其中三个宏内中文注释因维持续行
  反斜杠被统计为有效代码，但宏展开语义不变且两项阈值仍通过。无 `.config`，未构建或运行 BPF
  verifier、sub-scheduler、CPU hotplug 与 bypass 验证。
- 最终状态：**全文件完成**；已按新增函数契约门禁和方法论第 17 章完成强制验收。

### `kernel/sched/rt.c`

- 文件职责与验收：覆盖 FIFO/RR 优先级队列、组实体递归、RT bandwidth throttle/replenish，以及 SMP
  push/pull 与 cpupri 搜索；补齐主要实体、配置分支、失败和回滚路径。
- 并发与推理抽查：rq 锁串行队列，rt_runtime_lock 内嵌于 rq 锁，root-domain overload mask 以屏障
  配对发布；抽查周期 timer、runtime 借用和 find-lock-push，可推导 cpupri 结果只能作为迁移候选。
- 关联读取：`kernel/sched/cpupri.[ch]` 与 `kernel/sched/sched.h` 用于核对索引、锁序和类回调；
  只读未改。
- 修改安全：新增 127 行、删除 0 行，仅注释；禁用前缀、`git diff --check` 与忽略行长后 checkpatch
  通过。无 `.config`，未构建或运行 RT group、CPU hotplug 和 push IPI 验证。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

### `kernel/sched/topology.c`

- 文件职责与验收：全量覆盖 sched_domain/group/root_domain 的分阶段分配、认领、回滚、
  退化与 RCU 发布，以及 EAS/LLC、非对称容量、NUMA 距离层和分区热重建。本次将
  109 个配置分支下的真实函数实现/定义全部按“业务背景、入参、出参/返回、注意事项”逐函数
  核对；`ctags -x` 反查唯一报告为复合字面量被误识别的 `sched_domain` 非函数项。
- 并发与推理抽查：CPU hotplug 锁与 sched_domains_mutex 串行重建，旧 domain/perf-domain/
  NUMA 数组经 RCU 撤销和回收；抽查 overlap group 构造、`sched_init_numa()` 部分分配、
  `cpu_attach_domain()` 退化层所有权转移、`build_sched_domains()` 提交以及 partition 差量复用。可推导
  非 NUMA mask 部分重叠会破坏组环，过早恢复 NUMA 层数会使读者越界，遗漏 claim 会导致 UAF。
- 关联读取：`include/linux/sched/topology.h`、`include/linux/sched/sd_flags.h` 和
  `kernel/sched/sched.h` 用于核对结构、标志、共享引用和负载均衡语义；只读未改。
- 修改安全：相对基线新增 1007 行、删除 0 行，新增行正则审计无可执行内容；
  `git diff --check` 通过，忽略仅由中文 UTF-8 字节引起的 `LONG_LINE_COMMENT` 后 checkpatch 为
  0 errors/0 warnings。密度命令使用固定阈值，结果为
  `code=2116, comments=1995, chinese=924, density=0.437, max_gap=10`，退出码 0。
  无 `.config`，未构建或运行 NUMA/cpuset/CPU hotplug 验证。
- 最终状态：**全文件完成**；已按新增函数契约门禁和方法论第 17 章完成强制验收。

### `kernel/sched/deadline.c`

- 文件职责与验收：覆盖 EDF/CBS、root-domain 带宽准入、0-lag inactive timer、GRUB reclaim、DL
  servers、PI boost 和 SMP push/pull；说明 rq 本地与 root-domain 两级带宽所有权。
- 并发与推理抽查：rq 锁保护实体/timer 状态，dl_bw 锁保护准入总量，sched RCU 稳定 root_domain；
  抽查 replenish、inactive timer、server swap 和迁移复验，可推导立即归还阻塞任务带宽会破坏 GRUB。
- 关联读取：`kernel/sched/cpudeadline.[ch]` 与 `kernel/sched/sched.h` 用于核对候选索引、结构字段
  与策略切换契约；只读未改。
- 修改安全：新增 86 行、删除 0 行，仅注释；禁用前缀、`git diff --check` 与忽略行长后 checkpatch
  通过。无 `.config`，未构建或运行 admission、timer、PI 和 CPU hotplug 验证。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

### `kernel/sched/sched.h`

- 文件职责与验收：覆盖调度器内部 ABI 的 rq、各类子 rq、task_group、root_domain、sched_class、
  uclamp、拓扑组及 MM CID；重点标明对象所有权、动态锁映射和回调锁契约。
- 并发与推理抽查：rq/core 锁、pi_lock、RCU、timer 与引用计数边界分别说明；抽查 rq pin/clock、
  双 rq 全序、on_rq release 交接和 MM CID 转移，可推导无锁提示不能替代锁后复验。
- 关联读取：本批 `rt.c`、`deadline.c`、`topology.c`、`ext/internal.h` 及既有 fair/SCX 相关实现用于
  交叉核对结构消费者；关联文件除本批目标外只读未改。
- 修改安全：新增 90 行、删除 0 行，仅注释；禁用前缀、`git diff --check` 与忽略行长后 checkpatch
  通过。无 `.config`，未执行全调度器构建或并发运行验证。
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

### `kernel/sched/fair.c`

- 文件职责与验收：覆盖 CFS/EEVDF 的虚拟时间、lag、eligibility、virtual deadline 与增广红黑树，
  以及组调度层级、PELT、util_est、CFS bandwidth、NUMA balancing、EAS/idle 选核和 SMP/NOHZ 均衡。
- 并发与推理抽查：rq 锁保护实体树、curr、PELT 和层级计数；cfs_bandwidth 锁保护组全局 runtime，
  timer 与 rq 锁完成 throttle/unthrottle 交接；抽查 EEVDF pick、配额耗尽和双 rq 迁移，可推导虚拟
  时间左偏取整、保存 lag、锁后亲和性复验分别防止饥饿、睡眠获利和错误迁移。
- 关联读取：`kernel/sched/sched.h`、`kernel/sched/pelt.[ch]`、`include/linux/sched/topology.h` 与
  `include/linux/sched/sd_flags.h` 用于核对字段、时间轴和 domain 标志；仅只读未改。
- 修改安全：新增 79 行、删除 0 行，仅注释；禁用前缀、`git diff --check` 与忽略行长后 checkpatch
  均通过。密度门禁结果为 `code=8036, comments=5209, chinese=75, density=0.009, max_gap=777`，退出码
  1，未达到 `density>=0.20 && max_gap<=10`。无 `.config`，未构建或运行相关压测。
- 最终状态：**尚未达到标准**；需按密度报告逐段补充函数体阶段、变量、分支和并发说明后重新验收。

### `kernel/sched/ext/ext.c`

- 文件职责与验收：覆盖 sched_ext 的 BPF ops 调用、task custody/ops_state、local/global/user DSQ、
  buffered/direct dispatch、跨 rq 消费、deferred reenq/kick、watchdog、bypass、cgroup/sub-scheduler、
  enable/disable 回滚、诊断 dump 与主要 BPF kfunc 边界。
- 并发与推理抽查：rq/dsq/scx_sched/scx_tasks 四类锁域、enable mutex、RCU 和 qseq 代际分别说明；
  抽查 DSQ→rq 切锁、finish_dispatch claim、bypass 接管和 disable work，可推导不复验 qseq、持 DSQ
  锁直接取 rq 锁或过早开启静态键会导致重复 dispatch、死锁或暴露半初始化 scheduler。
- 关联读取：`kernel/sched/ext/internal.h`、`types.h`、`ext.h`、`idle.h`、`cid.h` 与 `arena.h` 用于核对
  状态布局、公开入口、选核、CID 和 arena ownership；仅本文件修改。
- 修改安全：新增 79 行、删除 0 行，仅注释；禁用前缀、`git diff --check` 与忽略行长后 checkpatch
  均通过。密度门禁结果为 `code=6326, comments=3060, chinese=75, density=0.012, max_gap=650`，退出码
  1，未达到 `density>=0.20 && max_gap<=10`。无 `.config`，未构建或运行相关验证。
- 最终状态：**尚未达到标准**；需按密度报告逐段补充 ownership、锁转换、回调失败和清理说明。

## 单文件完成记录模板

每个 `[x]` 文件下至少追加以下可复核记录：

- 文件职责与主调用链。
- 第 17 章验收：函数清单、实体清单、英文注释清单、成功/快速/慢速/失败/释放/配置路径清单。
- 并发与生命周期：锁、RCU、引用、屏障、发布、摘除和最终释放。
- 至少三个复杂函数的初学者复述与开发者推理抽查；不足三个时说明实际数量及豁免原因。
- 关联读取：文件、符号或范围、读取原因、结论、现有学习注释覆盖状态；关联文件只读不改。
- 修改安全：零代码改动、零原注释删除或改写、禁用模板前缀扫描、`git diff --check`、checkpatch
  和可用的构建结果；未执行项必须写明原因。另须记录密度脚本命令、固定阈值、
  `code/comments/chinese/density/max_gap` 与退出码，退出码非 0 不得标 `[x]`。
- 最终状态必须准确使用“全文件完成”“主路径检查点完成”或“尚未达到标准”。只有前者可标 `[x]`。

## 范围变化

- 2026-08-20：创建清单。目录快照共 51 个文件，46 个进入计划，5 个明确排除。
- 2026-08-24：按小文件优先的单文件闭环完成上述 29 个目标；计划完成度由 4/46 提升为 33/46。
- 2026-08-25：新增中文学习注释密度硬门禁。回溯原 43 个 `[x]` 后，15 个 C/头文件通过、
  27 个 C/头文件重新打开，`Makefile` 不适用；连同原本未关闭的 3 项，进度调整为 16/46。
- 2026-08-25：重新处理 `build_policy.c`、`build_utility.c`、`autogroup.c`、`cpupri.c` 和
  `cpudeadline.c`，五项均通过内容审计和固定密度门禁，进度调整为 21/46。
- 2026-08-25：继续重新处理 `stats.c`、`core_sched.c`、`wait_bit.c`、`stats.h` 和 `cpuacct.c`，
  五项均通过内容审计和固定密度门禁，进度调整为 26/46。
