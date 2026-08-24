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
- [ ] `kernel/sched/sched.h`
- [x] `kernel/sched/smp.h`
- [x] `kernel/sched/features.h`
- [x] `kernel/sched/rq-offsets.c`
- [x] `kernel/sched/build_policy.c`
- [x] `kernel/sched/build_utility.c`

### 系统调用、等待与跨 CPU 调度接口

- [ ] `kernel/sched/syscalls.c`
- [x] `kernel/sched/wait.c`
- [x] `kernel/sched/wait_bit.c`
- [ ] `kernel/sched/membarrier.c`
- [~] `kernel/sched/clock.c`

### 调度类与运行队列策略

- [ ] `kernel/sched/fair.c`
- [ ] `kernel/sched/rt.c`
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
- [ ] `kernel/sched/topology.c`

### PELT、负载、统计与 CPU 时间

- [ ] `kernel/sched/sched-pelt.h`
- [x] `kernel/sched/pelt.h`
- [x] `kernel/sched/pelt.c`
- [x] `kernel/sched/loadavg.c`
- [ ] `kernel/sched/cputime.c`
- [x] `kernel/sched/cpuacct.c`
- [x] `kernel/sched/stats.h`
- [x] `kernel/sched/stats.c`
- [ ] `kernel/sched/debug.c`
- [ ] `kernel/sched/psi.c`

### CPU 频率调节

- [x] `kernel/sched/cpufreq.c`
- [ ] `kernel/sched/cpufreq_schedutil.c`

### sched_ext 可扩展调度器

- [x] `kernel/sched/ext/types.h`
- [ ] `kernel/sched/ext/internal.h`
- [x] `kernel/sched/ext/ext.h`
- [x] `kernel/sched/ext/arena.h`
- [x] `kernel/sched/ext/arena.c`
- [x] `kernel/sched/ext/cid.h`
- [ ] `kernel/sched/ext/cid.c`
- [x] `kernel/sched/ext/idle.h`
- [ ] `kernel/sched/ext/idle.c`
- [ ] `kernel/sched/ext/ext.c`

## 明确排除

- [-] `kernel/sched/core.c`（按用户要求直接跳过）
- [-] `kernel/sched/completion.c`（按用户要求直接跳过）
- [-] `kernel/sched/idle.c`（按用户要求直接跳过；原请求重复列出一次，本清单合并为一个排除项）
- [-] `kernel/sched/isolation.c`（按用户要求直接跳过）
- [-] `kernel/sched/swait.c`（按用户要求直接跳过）

## 当前处理文件

- `[~] kernel/sched/clock.c`：第四批五文件任务的第 2 个目标，正在建立
  sched_clock 稳定性、每 CPU 时间同步和 suspend/idle 生命周期。
- 本轮已闭环 25 个文件 `cpupri.h`、`ext/arena.h`、`ext/idle.h`、`autogroup.h`、
  `build_policy.c`、`build_utility.c`、`features.h`、`pelt.h`、`cpufreq.c`、`ext/ext.h`、
  `stop_task.c`、`ext/arena.c`、`ext/types.h`、`stats.c`、`cpudeadline.c`、`wait_bit.c`、
  `autogroup.c`、`ext/cid.h`、`cpupri.c`、`stats.h`、`cpuacct.c`、`loadavg.c`、`wait.c`、
  `pelt.c`、`core_sched.c`
  均已完成整体审计。
- 本轮源文件合计新增 2482 行、删除 0 行；逐文件新增行均仅为注释或空行，原代码、声明、宏、条件
  编译和原注释逐行保留。25 个目标的 `git diff --check` 与忽略中文 UTF-8 字节行长后的 checkpatch
  均为 0 errors/0 warnings；工作树无 `.config`，未执行构建或运行时验证。
- 当前计划进度为 29/46 个文件 `[x]`、16/46 个文件 `[ ]`、1/46 个文件 `[~]`，
  5 个用户明确排除文件 `[-]`。
- `kernel/sched/sched.h` 只做过局部只读建图，未修改源码，保持 `[ ]`。
- `kernel/sched/sched-pelt.h` 标明为自动生成且禁止直接修改，本轮保持 `[ ]`，等待范围决策。

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
- 最终状态：**全文件完成**；已按方法论第 17 章完成强制验收。

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
- 2026-08-24：按小文件优先的单文件闭环完成上述 24 个目标；计划完成度由 4/46 提升为 28/46。
