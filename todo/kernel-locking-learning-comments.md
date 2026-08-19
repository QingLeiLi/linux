# kernel/locking 学习注释任务清单

## 使用规则

本清单是 `kernel/locking` 目录学习注释长任务的持久化进度源。发生会话中断或上下文压缩后，先读取
本文件并核对当前工作树，不凭对话记忆推断进度。

严格采用单文件闭环：

1. 开始前把目标文件标为 `[~]`，完整读取并建立函数、实体、英文注释和语义索引。
2. 仅追加中文学习注释；不改代码，不删除、改写或移动原有注释。
3. 按源码顺序以 1～5 个函数为一批修改，每批复读当前窗口并检查局部 diff。
4. 完整复读修改后的文件，按方法论第 17 章完成独立内容验收和修改安全检查。
5. 只有验收全部通过后才标为 `[x]`；一个 `[~]` 文件未闭环前不开始下一个文件。
6. 为核对契约而读取的关联源码只登记到当前文件记录；读取不等于授权修改关联文件。

状态：`[ ]` 未开始；`[~]` 正在处理或验收；`[x]` 已通过第 17 章单文件闭环。

## 文件进度（按建议学习顺序）

### 构建入口与 lockdep 状态模型

- [x] `kernel/locking/Makefile`
- [x] `kernel/locking/lockdep_states.h`
- [x] `kernel/locking/lock_events_list.h`
- [x] `kernel/locking/lock_events.h`
- [x] `kernel/locking/lock_events.c`
- [x] `kernel/locking/lockdep_internals.h`
- [x] `kernel/locking/lockdep.c`
- [x] `kernel/locking/lockdep_proc.c`
- [x] `kernel/locking/irqflag-debug.c`

### 自旋锁与排队自旋锁

- [x] `kernel/locking/mcs_spinlock.h`
- [x] `kernel/locking/qspinlock.h`
- [x] `kernel/locking/qspinlock.c`
- [x] `kernel/locking/qspinlock_paravirt.h`
- [x] `kernel/locking/qspinlock_stat.h`
- [x] `kernel/locking/qrwlock.c`
- [x] `kernel/locking/spinlock.c`
- [x] `kernel/locking/spinlock_debug.c`
- [x] `kernel/locking/spinlock_rt.c`

### mutex、乐观自旋与 wound/wait

- [x] `kernel/locking/mutex.h`
- [x] `kernel/locking/mutex.c`
- [x] `kernel/locking/mutex-debug.c`
- [x] `kernel/locking/osq_lock.c`
- [x] `kernel/locking/ww_mutex.h`
- [x] `kernel/locking/ww_rt_mutex.c`
- [x] `kernel/locking/test-ww_mutex.c`

### rtmutex 与 PREEMPT_RT 公共层

- [x] `kernel/locking/rtmutex_common.h`
- [x] `kernel/locking/rtmutex.c`
- [x] `kernel/locking/rtmutex_api.c`
- [x] `kernel/locking/rwbase_rt.c`

### 睡眠读写同步与验证工具

- [x] `kernel/locking/rwsem.c`
- [x] `kernel/locking/percpu-rwsem.c`
  - 文件职责：以每 CPU `read_count` 承载无写者时的低争用读快路径，以 `rcu_sync` 在写者到来时
    强制后续读者切换到慢路径，再由集中式 `block` 同时完成写者互斥和新读者门控；慢路径通过
    `rcuwait writer` 等待旧读者排空，并用 `waiters` FIFO 队列在释放时批量交付读者或单独交付写者。
  - 第 17 章验收：完整复读最终 500 行。13 个函数定义 13/13 均有紧邻专属契约，另为
    `per_cpu_sum` 语句表达式宏补齐适用前提；覆盖动态初始化/兼容销毁、读写 trylock、统一等待队列
    取锁、唤醒回调、读写阻塞入口、读状态查询、读者归零检查和读写释放。契约逐项说明参数、返回、
    可睡眠性、禁止抢占前提、队列锁/任务引用/锁所有权交付及失败保证，未发现错位契约。
  - 实体与英文注释验收：实体清单覆盖 `rss`、`read_count`、`writer`、`waiters`、`block`、
    `dep_map`，等待项的 `WQ_FLAG_EXCLUSIVE`/`WQ_FLAG_CUSTOM`、`private` 交付信号，以及 A/D、B/C
    两组屏障。原文件 319 行与 27 处块/行尾注释标记全部按序保留，非豁免英文均有紧邻翻译和机制
    补充；初始化字段定义虽位于头文件，也已结合公开读侧包装器解释其生命周期与快慢路径作用。
  - 路径与并发验收：抽查读路径，可复述快速每 CPU 计数、写者活跃后的先增计数再查 `block`、
    失败撤销及 FIFO 入队；抽查写路径，可复述 `rcu_sync_enter()` 关闭快路径并等待宽限期、原子设置
    `block`、求和等待读者清空；抽查释放路径，可复述 release 清 `block`、回调替任务取锁、连续放行
    readers/单个 writer 和宽限期后恢复快路径。并发清单覆盖 A/D 的“看见门闩或看见计数”保证、
    B/C 的“看见递减也看见读临界区”保证、waiters.lock 防丢唤醒、private release/acquire 交付，
    以及禁止抢占保证同一 CPU 增减配对。
  - 关联读取：`include/linux/percpu-rwsem.h` 的结构、静态初始化器、公开读侧快/慢路径包装与 lockdep
    接口，`kernel/rcu/sync.c` 的 enter/exit/回调状态机和宽限期语义，`include/linux/rcuwait.h` 的单任务
    等待接口与隐含屏障；这些区域学习注释缺失，关联文件均未修改。
  - 修改安全：新增 181 行、删除 0 行，新增非注释行 0；禁用模板前缀扫描无命中，`git diff --check`
    通过。完整文件 checkpatch 为 0 errors/0 warnings（另有 49 个检查项：48 个中文 UTF-8 视觉行宽
    及原有宏参数复用提示）；增量为 0 errors/48 个中文 UTF-8 视觉行宽 warnings。工作树无 `.config`，
    未执行目标内核配置构建。已按方法论第 17 章完成强制验收，状态为“全文件完成”。
- [x] `kernel/locking/semaphore.c`
  - 文件职责：实现无严格 owner 的计数信号量。`count` 表示仍可直接分配的许可数，`lock` 串行保护
    许可和以 `first_waiter` 锚定的环形 FIFO；争用时 `up()` 不增加 count，而是把许可直接交给队首，
    在锁内加入本地 wake_q、锁外真正唤醒，兼顾中断上下文可调用性和任务生命周期。
  - 第 17 章验收：完整复读最终 473 行。按条件编译展开共 21 个函数定义，21/21 均有紧邻专属契约；
    覆盖 hung-task 最近持有者及关闭配置 stubs、许可扣减、六个公开 down/up 入口、waiter 删除、统一
    睡眠/跟踪状态机、四种 down 慢路径适配器和队首直接交付。契约说明返回值、TASK 状态、超时单位、
    中断上下文边界、内部锁/IRQ 前后条件、许可归属、栈上 waiter 与 task 引用责任，未发现错位契约。
  - 实体与英文注释验收：实体清单覆盖 `lock`、`count`、`first_waiter`、可选 `last_holder`，
    `semaphore_waiter` 的环节点/task/up 三字段，四种等待状态/期限以及 wake_q。原文件 354 行与 10 处
    块注释起点全部按序保留，版权作者豁免，其余英文均有紧邻翻译和机制补充；特别强调
    `down_trylock()` 的 0 成功/1 失败反向约定及 semaphore 与 mutex 的 owner 差异。
  - 路径与并发验收：抽查快路径，可复述 irqsave 内 count>0 扣减；抽查 `___down_common()`，可复述
    栈上 waiter 尾插、锁内信号/超时仲裁、设状态后开 IRQ 调度、重取锁后检查直接交付；抽查 `up()`，
    可复述无等待者加 count、有等待者摘队置 up、wake_q 持引用、解锁后唤醒。并发清单覆盖 sem->lock
    对 count/队列/up 标志的串行保护，状态设置与 wake_q 唤醒屏障，信号/超时失败不消耗许可，
    `last_holder` 仅为可能过时的诊断线索而非所有权协议。
  - 关联读取：`include/linux/semaphore.h` 的对象布局、初始化器、无 owner/中断上下文契约；
    `include/linux/sched/wake_q.h` 与 `kernel/sched/core.c` 的 wake_q 去重、引用转移和锁外消费；
    `include/linux/hung_task.h`、`kernel/hung_task.c` 的 blocker 编码与 semaphore 最近持有者报告。
    关联文件均未修改，其中调度器 wake_q 实现已有充分学习注释，其余所读区域注释不足。
  - 修改安全：新增 119 行、删除 0 行，新增非注释行 0；禁用模板前缀扫描无命中，`git diff --check`
    通过。完整文件 checkpatch 为 0 errors/0 warnings（另有 29 个检查项：28 个中文 UTF-8 视觉行宽
    及原有函数参数对齐提示）；增量为 0 errors/28 个中文 UTF-8 视觉行宽 warnings。工作树无 `.config`，
    未执行目标内核配置构建。已按方法论第 17 章完成强制验收，状态为“全文件完成”。
- [x] `kernel/locking/locktorture.c`
  - 文件职责：以 `lock_torture_ops` 将 spin/raw-spin/rqspin/rwlock、mutex/ww_mutex/rtmutex、rwsem 和
    percpu-rwsem 适配到统一 writer/reader 压力线程；通过临界区延迟、嵌套锁链、RT 优先级扰动、
    CPU 热插拔/shuffle/stutter、RCU 自续回调链和互斥断言组合制造并观测高强度并发交错。
  - 第 17 章验收：完整复读最终 1681 行。71 个函数定义 71/71 均有紧邻专属契约；覆盖 cpumask 参数、
    busted 对照、各锁类型获取/延迟/释放适配器、嵌套 mutex/rtmutex、ww_mutex 伤口等待回退、writer/
    reader/stats kthread、RCU 链、幂等 cleanup 和 init/unwind 主状态机。契约说明 tid/lockset/随机状态、
    返回与失败语义、持锁/IRQ/调度状态、类型资源生命周期、任务/回调 ownership 和停止条件。
  - 实体与英文注释验收：实体清单覆盖 19 个模块参数、reader/writer CPU 掩码、共享读写持有标志、
    `lock_stress_stats`、`call_rcu_chain`、`lock_torture_ops` 全回调/flags/name、全局 cxt 及每种锁对象/
    ops 表。原文件 1450 行与 62 处块/行尾注释标记全部按序保留，SPDX/版权作者豁免，其余英文均有
    紧邻翻译和机制补充；busted 的故意错误、try 型返回、常见路径、优先级复位和线程创建策略均明确。
  - 路径与并发验收：抽查 writer/reader，可复述随机嵌套、偶发跳过中心锁、重复 writer/读写重叠
    检测、延迟和逆序释放；抽查 ww_mutex，可复述 -EDEADLK 后释放已持锁、slow 获取冲突锁、重排再
    继续；抽查 init/unwind，可复述操作表选择、默认线程数、类型 init、各通用压力子系统、交错建线程
    及任一点失败统一 cleanup。并发清单覆盖正确锁原语对共享验证标志的保护、data_race 统计近似快照、
    crc_stop release/acquire 停链、rcu_barrier 回调排空、先停 stats 再最终打印及 cleanup 唯一执行权。
  - 关联读取：`include/linux/torture.h` 的参数宏、随机状态、线程创建/停止、热插拔/shuffle/stutter/
    shutdown 接口，`kernel/torture.c` 的 init/cleanup fullstop 状态机、停止条件和 kthread 退出协议；
    各 ops 所调用的锁实现均已在本清单前序文件完成学习注释，rqspinlock 也已结合 BPF 适配路径复核。
    关联文件均未修改；torture 公共框架所读区域学习注释不足，建议另立目录任务。
  - 修改安全：新增 231 行、删除 0 行，新增非注释行 0；禁用模板前缀扫描无命中，`git diff --check`
    通过。增量 checkpatch 为 0 errors/46 个中文 UTF-8 视觉行宽 warnings；完整文件为 1 error/9 warnings，
    均落在未改原代码（既有三元运算符空格、C 文件 extern、函数名字符串、OOM 日志和缩进标签），
    另有 62 个检查项。工作树无 `.config`，未执行目标内核配置构建。已按方法论第 17 章完成强制验收，
    状态为“全文件完成”。

## 已完成文件

- [x] `kernel/locking/lockdep_internals.h`
  - 文件职责：定义 lockdep 内部 usage 位编号/掩码、IRQ 聚合集合、静态池容量、chain 上下文、跨文件
    统计接口，以及 `DEBUG_LOCKDEP` 启用/关闭时的 percpu 统计布局和 helper。文件无 include guard，
    当前由 `lockdep.c`、`lockdep_proc.c` 各自单次包含。
  - 第 17 章验收：完整复读修改后的 524 行。函数/声明清单覆盖 `get_usage_chars()`（226～227）、
    `__get_key_name()`（237～238）、`lock_chain_get_class()`（248）、chain 遍历/计数（272、280）、
    正反依赖计数声明（325、333）与关闭证明时的 stub（357～360、369～372）、两个 stack trace
    计数声明（340、347），以及 `debug_class_ops_inc/read()`（482～489、499～510）；专属契约均紧邻
    并覆盖参数、范围、上下文、返回、ownership 和配置差异。实体清单覆盖两个枚举组、全部 X-macro
    展开、容量/上下文宏、extern 池与统计、`lockdep_stats` 全字段、percpu 声明及启用/禁用调试宏。
    所有非豁免英文注释均逐字保留并有紧邻翻译和机制补充。
  - 抽查记录：仅凭注释可复述 usage bit0/read、bit1/direction 与高位 state 的编码及冲突掩码位移；
    可解释 small/常规容量如何映射直接依赖、chain、hlock 和 trace 静态池；可解释 IRQ-off
    `__this_cpu_*` 写、特殊 `this_cpu_inc()` 写、跨 CPU 非快照读取及关闭调试后的零开销语义。
  - 关联读取：`lockdep_states.h` 的状态顺序与四位展开（学习注释充分）；`lockdep.c` 的 usage 解码/
    冲突掩码、BFS 计数、chain 池、统计更新和 class ops 调用（缺失或零散）；`lockdep_proc.c` 的 chain
    与统计消费者（缺失）；`include/linux/lockdep_types.h` 的 trace 容量、`lock_class`/`held_lock`
    布局（缺失）；`lib/Kconfig.debug` 和 `arch/sparc/Kconfig` 的容量范围与 small 选择（缺失）。目录内
    文件按清单继续补注，目录外关联区域建议另立任务处理。
  - 修改安全：新增 259 行、删除 0 行，原代码和原注释逐行保留；禁用模板前缀扫描无命中，
    `git diff --check` 通过；checkpatch 为 0 errors，37 个 warnings 均为中文 UTF-8 行计宽；工作树
    无 `.config`，未执行构建。

- [x] `kernel/locking/lock_events.c`
  - 文件职责：定义 X-macro 生成的事件名称表和每 CPU 计数存储，通过 debugfs 发布逐事件只读文件
    与全量重置文件；默认弱读取实现汇总普通计数，PV qspinlock 构建可用强实现替换并计算平均值。
  - 第 17 章验收：完整复读 332 行；函数清单为弱 `lockevent_read()`（111～142）、
    `lockevent_write()`（167～194）、PV `skip_lockevent()`（224～243）、非 PV stub（253～256）和
    `init_lockevent_counts()`（281～329），所有专属函数头均紧邻声明并覆盖参数、上下文、返回、
    ownership、阶段、失败和并发边界。实体清单覆盖 `LOCK_EVENT` 名称展开、目录名、名称表、percpu
    数组和 fops；全部非豁免英文注释均有紧邻完整翻译与学习补充。抽查读取、重置、初始化三个复杂
    路径后，可仅凭注释解释非快照汇总、reset 与 raw percpu 更新竞态、PV 裸机过滤、发布点及递归
    回滚；条件编译两侧的同名 helper 均有独立契约。
  - 关联读取：`lock_events.h` 与 `lock_events_list.h` 的编号/名称/更新契约（学习注释充分）；
    `qspinlock_stat.h` 的 PV 强读取实现（缺失）；`include/linux/debugfs.h` 的创建返回与清理契约、
    `arch/Kconfig` 的 `LOCK_EVENT_COUNTS depends on DEBUG_FS`、`arch/x86/kernel/paravirt-spinlocks.c` 的
    `pv_is_native_spin_unlock()` 判定（均缺失）。目录内文件按清单后续补注，目录外关联区域建议另立
    任务处理。
  - 修改安全：新增 153 行、删除 0 行，原代码和原注释逐行保留；禁用模板前缀扫描无命中，
    `git diff --check` 通过；checkpatch 为 0 errors，30 个 warnings 均为中文 UTF-8 行计宽；工作树
    无 `.config`，未执行构建。

- [x] `kernel/locking/lock_events.h`
  - 文件职责：将 X-macro 事件目录展开为 `enum lock_events`，声明每 CPU 计数数组，并按
    `CONFIG_LOCK_EVENT_COUNTS` 提供真实或空操作更新接口；同时声明供 debugfs 使用、可由 PV
    qspinlock 统计层覆盖的读取函数。
  - 第 17 章验收：完整复读 140 行；函数清单为 `__lockevent_inc()`（72～78）、
    `__lockevent_add()`（99～103）与外部 `lockevent_read()` 声明（137～138），专属契约均紧邻且
    覆盖全部参数、上下文、返回、ownership、percpu 近似统计副作用和后续去向。实体清单覆盖枚举、
    `lockevent_num`/reset 伪编号、percpu 数组及启用/禁用配置下的六个宏定义；英文总数、percpu 与
    低开销递增说明均有紧邻翻译和补充，并修正解释原文 `loose` 为 `lose`。两个内联函数均属短小
    单阶段 helper，无三个复杂函数可抽查；配置求值、条件 false、累加、普通/PV 读取路径已复述。
  - 关联读取：`lock_events_list.h` 的事件枚举来源（学习注释充分）；`lock_events.c` 的弱读取实现、
    per CPU 定义和 debugfs 回调（学习注释缺失）；`qspinlock.c` 对 `qspinlock_stat.h` 的包含以及
    `qspinlock_stat.h` 的强读取实现（学习注释缺失）。后两者建议按本清单对应文件继续补注。
  - 修改安全：新增 76 行、删除 0 行，全部原代码与原注释逐行保留；禁用模板前缀扫描无命中，
    `git diff --check` 通过；checkpatch 为 0 errors，16 个 warnings 均为中文 UTF-8 行计宽；工作树
    无 `.config`，未执行构建。

- [x] `kernel/locking/lock_events_list.h`
  - 文件职责：作为无 include guard 的 X-macro 事件目录，按配置声明 49 个锁观测槽；默认展开生成
    枚举，`lock_events.c` 重定义宏后重复包含以生成同序 debugfs 名称，调用点使用相同编号更新 percpu
    计数。PV 的跳数/延迟槽先累计总量，读取时才除以 kick 样本数形成平均值。
  - 第 17 章验收：完整复读 199 行；许可证与作者信息按豁免保留，所有事件组和 46 条原有事件行尾
    英文说明均有紧邻完整翻译与调用点语义补充，其余 8 个无行尾英文的事件也逐项解释。文件无函数、
    结构体、局部变量、控制流或资源生命周期，函数清单和三个复杂函数抽查不适用；宏实体、49 个
    事件实体、配置分支、编号关系和观测边界均已核对。当前源码还表明精确推导 node1 使用次数时，
    除 node2～node4 外还须从 lock_slowpath 中减去节点耗尽的 lock_no_node，已在保留原文后追加修正。
  - 关联读取：`lock_events.h` 的枚举与更新宏；`lock_events.c` 的名称表、percpu 聚合和 debugfs；
    `qspinlock_stat.h` 的 PV 总量/平均值换算；`qspinlock.c`、`qspinlock_paravirt.h` 与
    `kernel/bpf/rqspinlock.c` 的 pending、MCS 节点、PV 和超时更新点；`rwsem.c` 的睡眠、唤醒、
    乐观自旋、偷锁与 handoff；`rtmutex.c` 的 rtlock/rtmutex 慢路径；`lockdep.c` 的三个更新点。
    这些关联区域现有学习注释均缺失或仅有零散覆盖；目录内文件按本清单后续补注，目录外 BPF 文件
    建议另立任务处理。
  - 修改安全：新增 97 行、删除 0 行；49 个 `LOCK_EVENT()` 代码条目及顺序不变；禁用模板前缀扫描
    无命中，`git diff --check` 通过；checkpatch 为 0 errors，38 个 warnings 均为中文 UTF-8 行计宽；
    工作树无 `.config`，未执行构建。

- [x] `kernel/locking/lockdep_states.h`
  - 文件职责：作为无 include guard 的 X-macro 数据表，集中列出 lockdep 跟踪的 IRQ 上下文类别；
    不同包含点通过重新定义 `LOCKDEP_STATE()`，从同一顺序生成枚举、位掩码、名称表和检查分派表。
  - 第 17 章验收：完整复读后确认唯一英文注释已有紧邻翻译和当前版本修正，两个宏实体均解释了
    业务意义、四状态展开和编号不变量；本文件没有函数、局部变量、分支或资源生命周期，函数清单
    与三复杂函数抽查不适用。初学者可以仅凭注释复述重复包含协议及增删状态的同步维护要求。
  - 关联读取：`kernel/locking/lockdep_internals.h` 的 usage-bit/bitmask 展开；
    `kernel/locking/lockdep.c` 的字符串、名称与 verbose 分派展开；`include/linux/lockdep_types.h` 的
    `XXX_LOCK_USAGE_STATES`/`LOCK_TRACE_STATES` 容量契约；`include/linux/lockdep.h` 的公开接口附近。
    这些关联区域现有学习注释均缺失，建议随本目录对应文件及后续头文件任务补注。
  - 修改安全：新增 23 行注释、0 行删除，两个 `LOCKDEP_STATE()` 代码 token 及顺序不变；
    `git diff --check` 通过；checkpatch 为 0 errors，剩余仅中文 UTF-8 计宽 warnings；工作树无
    `.config`，未执行构建。

- [x] `kernel/locking/Makefile`
  - 文件职责：始终链接 mutex/semaphore/rwsem/percpu-rwsem 基础对象，按配置选择 lockdep、各类
    自旋锁、PREEMPT_RT 包装和测试/观测对象，并对 lockdep 热路径关闭会递归或放大噪声的插桩。
  - 第 17 章验收：完整复读全部 40 条原始构建语句；许可证行无需翻译，两个英文说明均有紧邻
    完整翻译与机制补充，每个变量/条件块/对象组均说明选择条件、产物和副作用。本文件没有 C
    函数、数据对象生命周期或运行时失败路径，函数清单和三个复杂函数抽查不适用；可仅凭注释
    复述普通、调试、lockdep、排队锁、RT 和测试组件的构建关系。
  - 关联读取：`kernel/Makefile` 的 `obj-y += locking/`（确认目录始终进入 Kbuild）；
    `scripts/Makefile.lib` 与 `scripts/Makefile.context-analysis` 的 KCOV/KCSAN/context-analysis 标志
    生成规则；`kernel/Kconfig.locks`、`lib/Kconfig.debug`、`arch/Kconfig` 的锁配置依赖；
    `rtmutex_api.c`、`ww_rt_mutex.c`、`spinlock_rt.c`、`rwsem.c` 的 `#include "rtmutex.c"`/
    `#include "rwbase_rt.c"`（确认核心实现按构建宏文本复用）。这些区域现有学习注释为缺失或
    部分覆盖，目录内文件将按本清单处理，目录外 Kbuild/Kconfig 建议另立任务补注。
  - 修改安全：新增 26 行注释、0 行删除；过滤注释和空行后全部 Kbuild 语句逐行一致；
    `git diff --check` 与 checkpatch 均通过（0 errors、0 warnings）；工作树无 `.config`，未执行构建。

## 最近完成文件

- [x] `kernel/locking/lockdep.c`
  - 首次建图已按源码顺序完整读取原文件 1～6880 行；前序为核对头文件而读取的局部片段未冒充
    本次覆盖。已完成原文件 1～215 行的首个注释分区：文件设计总览、运行开关与 sysctl 表、
    `kernel_lockdep_sysctls_init()`、percpu 递归门、`lockdep_enabled()`、内部 raw spinlock/owner、
    `lockdep_lock()`、`lockdep_unlock()`、`lockdep_assert_locked()`、selftest 任务指针、`graph_lock()`、
    `graph_unlock()` 和 `debug_locks_off_graph_unlock()`。随后已顺序完成原文件 216～531 行：直接依赖
    与 class 静态池、`hlock_class()`、LOCK_STAT 启用/禁用两侧全部 helper、class/chain hash、RCU
    双缓冲延迟回收实体、held-lock/chain key 编码、task 初始化/递归门/selftest helper，以及 verbose
    过滤和关闭提示。各批均已复读；当前 `lockdep.c` 累计新增 422 行、删除 0 行，`git diff --check`
    通过，禁用模板前缀无命中；checkpatch 为 0 errors，148 个 warnings 均为中文 UTF-8 行计宽。
    已继续完成原文件 534～1394 行：`lock_trace`/trace 静态池、去重保存和统计，chain 上下文计数、
    usage 字符串生成与锁诊断打印，静态 key 地址分类、无锁 RCU class 查询和 key 推导，
    `CONFIG_DEBUG_LOCKDEP` 全局结构一致性谓词/扫描及关闭配置 stub，一次性静态池初始化，动态 key
    注册/查询，以及 `register_lock_class()` 的双重查找、静态槽初始化、RCU 发布和 map cache 路径。
    当前 `lockdep.c` 累计新增 936 行、删除 0 行；`git diff --check` 通过，禁用模板前缀无命中；
    checkpatch 为 0 errors，353 个 warnings 均为中文 UTF-8 行计宽。已继续完成原文件 1396～2179
    行：直接依赖边静态池分配/RCU 发布，压缩环形队列，generation/parent/字段偏移 helper，
    SR/ER/SN/EN 正反向编码，正反向 BFS 根与强路径主循环，环路路径/两 CPU 场景/BFS 错误终止报告，
    正反向可达计数，以及 `check_path()`/`check_noncircular()` 的返回值与图锁所有权。审计中发现并
    恢复了两行包装器缩进的意外改写，当前 `lockdep.c` 累计新增 1439 行、删除 0 行；
    `git diff --check` 通过，禁用模板前缀无命中；checkpatch 为 0 errors，465 个 warnings 均为中文
    UTF-8 行计宽。已继续完成原文件 2181～2879 行：IRQ safe/unsafe 强环判据，usage 累计/匹配/特殊
    节点剪枝，正反向 usage 搜索，锁类与最短依赖路径打印，IRQ 两 CPU 场景和终止报告，状态名称与
    usage bit 编码、direction/read 掩码变换、精确冲突位配对，以及 `check_irq_usage()` 四阶段证明与
    关闭配置 stub。注释保留并明确指出原文“only consider _READ usage”与当前 `& LOCKF_IRQ` 实际去掉
    `_READ` 位的差异。当前 `lockdep.c` 累计新增 1772 行、删除 0 行；`git diff --check` 通过，禁用
    模板前缀无命中；checkpatch 为 0 errors，556 个 warnings 均为中文 UTF-8 行计宽。下一分区从原
    文件 2881 行的 `CONFIG_LOCKDEP_SMALL` 冗余边证明开始。现已继续完成原文件 2881～3934 行：
    小内存配置的冗余强路径判定及关闭 stub、chain 计数、同类递归死锁检查、相关前驱选择与依赖边
    双向提交；chain_hlocks 空闲块元数据、精确尺寸桶与 worst-fit 分配器；chain key 调试重放与碰撞
    核验；chain 描述符分配、RCU hash 查找/双重检查插入，以及 `validate_chain()` 启用/关闭实现。
    注释明确记录 `check_prev_add()` 各返回码并不对应统一的图锁状态，尤其已有正向边却缺少反向镜像
    的内部不一致分支仍持有 `graph_lock`。当前 `lockdep.c` 累计新增 2364 行、删除 0 行（使用
    `minimal` diff 消除重复英文注释的配对歧义）；`git diff --check` 通过，禁用模板前缀无命中；对
    增量 diff 执行 checkpatch 为 0 errors、709 个 warnings，均为中文 UTF-8 行计宽。下一分区从原
    文件 3936 行的 `check_chain_key()` 增量 key 自校验开始。现已继续顺序覆盖原文件 3936～6880
    行：增量 chain key 自校验、usage 状态冲突与 IRQ 反转报告、hardirq/softirq 开关跟踪、wait-type
    约束、map 初始化与特殊 key、完整 `__lock_acquire()`/release 重放/pin 路径、LOCK_STAT 竞争统计、
    testsuite reset、class/chain 的 RCU 双缓冲延迟回收、动态 key 注销、内存释放与任务/系统持锁检查，
    以及最终 RCU suspicious 报告。审计中发现并删除了 189 行被补丁工具误追加到文件末尾的游离
    注释；当前文件重新以原有 `EXPORT_SYMBOL_GPL(lockdep_rcu_suspicious);` 结束。当前 `lockdep.c`
    累计新增 3685 行、删除 0 行（`minimal` diff），`git diff --check` 通过，禁用模板前缀无命中；
    增量 checkpatch 为 0 errors、969 个 warnings，均是新增中文注释的 UTF-8 视觉行宽。源码内容已
    覆盖到原文件末尾，但尚未完成最终全文件复读和第 17 章验收，因此仍保持 `[~]`；工作树无
    `.config`，不能执行目标内核配置的构建验证。
  - 函数索引：配置/图锁入口位于 105～215 行；lockstat 位于 252～363 行；trace、打印与 class key
    注册位于 429～1399 行；依赖边、BFS 和环路诊断位于 1401～2219 行；IRQ usage 图检查位于
    2224～2899 行；冗余/死锁检查和依赖边提交位于 2908～3318 行；chain hlock 分配器、chain cache
    与校验位于 3326～3980 行；usage/IRQ 状态迁移和 wait context 位于 3989～4927 行；map 初始化、
    `__lock_acquire()`、释放栈重放和公开 acquire/release/pin 接口位于 4937～5998 行；lockstat 公开
    回调、reset、RCU 延迟回收、key 注销和最终诊断位于 6004～6880 行。条件编译两侧的同名实现与
    stub 均已单独登记，后续不能因同名而漏注。
  - 实体索引：运行开关与 percpu 递归计数；内部 raw spinlock、owner 和 generation；直接依赖
    `list_entries[]`/bitmap；class 池、free/all/zapped 链表及 key/class hash；chain 池/hash/bitmap、
    紧凑 `chain_hlocks[]` 与空闲块桶；stack trace 池/hash；每 CPU lockstat；BFS 环形队列；两组
    `pending_free`/`delayed_free` 缓冲及 `rcu_head`。后续逐实体补充容量、所有权、发布/回收时点和
    并发保护，不能只解释字段字面含义。
  - 主调用链：公开 acquire 包装在本 CPU 关中断且 `lockdep_recursion` 隔离后进入
    `__lock_acquire()`；后者构造未发布的 `held_lock`，更新 usage/wait context，增量计算 chain key，
    命中 chain cache 时跳过图校验，未命中时在 graph lock 下做环路、IRQ 反转、wait context 与前驱
    依赖检查并发布 chain；release 在非栈顶匹配时截断并重放后继锁以重建 chain。IRQ 入口/出口负责
    把当前持锁集合标为对应 hardirq/softirq usage。
  - 生命周期与并发边界：IRQ-off 保护本 CPU held-lock/recursion 状态，内部 `__lock` 保护图、hash、
    class/list/chain 分配器；class 通过 RCU hash 发布并允许 IRQ-off 读侧查找。zap 在 graph lock 下
    摘除边、class 和 chain，清除对外 key/name 后切换 delayed-free 缓冲，RCU grace period 之后才把
    class 与 chain 槽归还静态池；模块/动态 key 注销路径另有同步 RCU 边界。诊断路径会先永久关闭
    lockdep，并在 printk 前释放 graph lock，避免递归进入自身。
  - 英文注释索引：文件头设计约束、内部锁与静态池、class key/RCU 发布、BFS 强依赖、IRQ usage、
    chain cache、held-lock 栈、pin cookie、reset 与延迟回收、syscall/RCU 诊断等连续区域均已在首次
    通读中定位；后续必须保留原文并在紧邻位置逐条翻译、补足上下文，不以本索引代替逐条覆盖。
  - 第 17 章最终验收：按修改后文件顺序以无截断窗口完整复读 1～10565 行。函数/声明清单共 251 份
    专属契约，自动核对契约名与紧随声明名为 0 处错位；条件编译两侧的同名实现、stub 和前置声明
    均分别覆盖。函数头及函数体注释说明调用位置、参数/返回、入口上下文、阶段、快速/慢速/失败
    路径、图锁所有权和配置差异。实体清单覆盖运行开关、percpu 递归门、图锁、class/依赖边/trace/
    chain 固定池与 hash、BFS 队列、held-lock 栈、usage 状态、chain_hlocks 分桶分配器、lockstat、
    dynamic key 和 RCU 双缓冲延迟回收；参数与重要局部变量均在契约或首次使用处解释有效期和角色。
  - 英文注释验收：逐项映射并核对原文件 346 组含英文的非许可证候选注释；除 9 个 `#else/#endif`
    配置标识和 3 个 `/* 1 */`～`/* 3 */` 位号数字属于结构/标识符豁免外，其余均有紧邻完整中文翻译
    与机制补充。6880 条原始源码行全部按序保留，未改写或移动原有英文注释。
  - 路径与并发验收：成功、cache hit、class/chain/trace/list 池耗尽、BFS 错误、冲突报告、非 LIFO
    release 重放、配置 stub、key/class zap 和 RCU 最终复用路径均已复读。IRQ-off 稳定本 CPU 状态，
    graph lock 串行图和池，RCU 负责无锁查询与摘除后的生命周期；诊断前冻结 lockdep 并释放图锁。
  - 抽查记录：仅读注释可复述 `__bfs()` 的 generation 去重、强路径过滤、首兄弟入队和四类结果；
    可复述 `check_prev_add()` 的 key 生命周期、成环/IRQ/冗余证明、双向镜像发布及非统一图锁返回；
    可复述 `__lock_acquire()` 的 no-track/no-validate 分流、候选栈项、wait/usage、chain cache、nest、
    sync 和最终提交阶段。开发者视角下可判断三者的调用顺序不可交换、错误后由哪个 helper 解锁、
    哪些发布不可回滚，以及 `CONFIG_PROVE_LOCKING`/`CONFIG_TRACE_IRQFLAGS`/`CONFIG_LOCKDEP_SMALL`
    关闭后的替代分支。
  - 关联读取：`kernel/locking/lockdep_internals.h` 的 usage 位、容量、统计和跨文件接口（学习注释充分）；
    `kernel/locking/lockdep_states.h` 的状态顺序与 X-macro 展开（学习注释充分）；前序完成这些文件时
    已核对 `include/linux/lockdep_types.h` 的 class/held-lock/trace 布局和 `include/linux/lockdep.h`
    的公开接口附近（现有学习注释缺失，建议另立头文件任务补注，不在本目录清单内扩改）。
  - 修改安全：最终为新增 3685 行、删除 0 行，新增非空行全部为注释；`git diff --check` 通过，
    禁用模板前缀扫描无命中；增量 checkpatch 为 0 errors、969 个 warnings，全部是新增中文注释的
    UTF-8 视觉行宽。工作树无 `.config`，未执行目标内核配置构建。已按本文第 17 章完成强制验收，
    本文件状态为“全文件完成”。

## 最近完成文件（续）

- [x] `kernel/locking/lockdep_proc.c`
  - 首次建图已按源码顺序完整读取原文件 1～732 行；当前源码尚未修改。文件职责是通过 seq_file 和
    procfs 发布活动 lock class、缓存 chain、全局 lockdep 统计与可选 lockstat 快照，并允许向
    `lock_stat` 写入字符 `0` 清零每个活动 class 的竞争统计。
  - 函数索引：`/proc/lockdep` 的 `l_next()`、`l_start()`、`l_stop()`、`print_name()`、`l_show()` 位于
    38～118 行；`CONFIG_PROVE_LOCKING` 的 chain 迭代与输出 `lc_start()`、`lc_next()`、`lc_stop()`、
    `lc_show()` 位于 128～183 行；统计输出 `lockdep_stats_debug_show()`、`lockdep_stats_show()` 位于
    193～394 行；`CONFIG_LOCK_STAT` 的排序、格式化、快照 seq 迭代、open/write/release 位于 411～713
    行；`lockdep_proc_init()` 位于 717～730 行。共 26 个函数，三个 seq_operations 表和一个 proc_ops
    表；条件编译区域必须分别说明启用与缺席时的 proc 可见性。
  - 实体索引：`iterate_lock_classes` 数组扫描宏；`lockdep_ops`、`lockdep_chains_ops`、`lockstat_ops`、
    `lock_stat_proc_ops` 回调表；`lock_stat_data` 的 class 借用指针与统计快照；一次 open 独占的
    `lock_stat_seq` vmalloc 缓冲及 `iter_end` 有效区间；IRQ context 名称表和各类局部累计计数。
  - 路径索引：普通 class 输出跳过 bitmap 空洞并只列 distance==1 的直接后继；chain 输出用
    `lockdep_next_lockchain()` 跨过空闲槽；stats 全池聚合 usage 与依赖；lockstat open 分配、收集、
    排序并挂到 seq_file，失败释放；write 只把首字符 `0` 解释为清零命令；release 释放私有快照；
    init 按配置创建四个 proc 节点。需分别覆盖成功、空池、inactive/zapped class、用户拷贝失败、
    vmalloc/seq_open 失败和配置关闭路径。
  - 并发与生命周期索引：class 链可能在无图锁迭代时移到 free/zapped 链，因此主扫描必须使用固定
    `lock_classes[]`、`lock_classes_in_use` 与 `max_lock_class_idx`；结果是并发变化中的诊断视图而非
    一致快照。lockstat open 把计数复制进每文件快照，`seq_stats()` 在 sched-RCU 下借用 class 的
    name/key，快照由 release 释放；清零与更新可并发，统计只能按近似观测解释。
  - 英文注释索引：文件头职责、无图锁 class 迭代原因、总依赖估算、zapped/缓冲复用、contention
    排序、显示舍入和版本截断提示共七组；许可证/版权豁免，其余必须逐字保留并紧邻翻译与补充。
  - 关联读取计划：`lockdep_internals.h` 的池/统计声明和 `lockdep.c` 的生产、回收、计数 helper 已有
    充分学习注释；追加前按需核对 `include/linux/seq_file.h`、`include/linux/proc_fs.h` 与排序接口
    的当前契约，只读不扩改。
  - 已按原文件顺序完成 1～732 行注释：固定 class 数组和 chain bitmap 的 seq_file 迭代，class/
    chain/stats 输出，LOCK_STAT 数据结构、排序与时间格式化，sched-RCU 名称复制，私有快照的
    vmalloc→seq_open→汇总→排序→release 生命周期，字符 `0` 清零协议，以及按配置创建 proc 节点。
    当前修改后文件 1099 行，累计新增 367 行、删除 0 行；各批均已复读，`git diff --check` 通过。
    关联核对了 `include/linux/seq_file.h` 的回调/私有数据接口（现有学习注释部分覆盖）、
    `fs/seq_file.c` 的 `seq_open()` 返回与所有权（该符号附近学习注释充分）、`include/linux/proc_fs.h`
    的节点创建接口和关闭 PROC_FS 时的 NULL stub（学习注释缺失），以及 `include/linux/sort.h` 的
    比较器接口（学习注释缺失）；后两者建议另立公共基础设施任务补注。
  - 第 17 章最终验收：以两个无截断连续窗口完整复读修改后 1～1099 行。26 份函数契约逐项核对
    名称、参数、返回、调用位置、上下文、阶段和所有权，自动检查契约名与紧随声明名为 0 处错位；
    三个 seq_operations 表、proc_ops、两个快照结构及字段、数组扫描宏和 IRQ 字符串表均有角色、
    生命周期与配置边界说明。成功、inactive/zapped 跳过、空池、用户拷贝失败、vmalloc/seq_open
    失败释放、非 `0` 写入、清零竞态、release 和配置关闭路径均已覆盖。
  - 英文注释验收：文件头与 7 组机制性英文说明逐字保留并有紧邻完整翻译/补充；自动扫描剩余两个
    候选仅为 `#endif /* CONFIG_* */` 结构标签，属于标识符豁免。732 条原始源码行全部按序保留。
  - 抽查记录：仅读注释可复述 `l_show()` 的固定数组、bitmap 空洞、BFS 计数和直接边输出；可复述
    `lockdep_stats_show()` 的 usage 分类、非事务快照、包含根的 forward 计数及历史估算公式；可复述
    `lock_stat_open()`/`seq_stats()` 的 vmalloc 所有权、seq_open 失败清理、快照排序、sched-RCU 名称
    复制与 release 配对。开发者视角下可判断不能改成无锁链表遍历、RCU 解锁前必须复制名称、错误
    返回应在哪一层释放 data，以及 PROVE_LOCKING/LOCK_STAT 关闭时哪些节点和数据路径消失。
  - 修改安全：新增 367 行、删除 0 行，新增非空行全部为注释；禁用模板前缀无命中，
    `git diff --check` 通过；增量 checkpatch 为 0 errors、123 个 warnings，全部是新增中文注释的
    UTF-8 视觉行宽。工作树无 `.config`，未执行目标内核配置构建。已按本文第 17 章完成强制验收，
    本文件状态为“全文件完成”。

## 最近完成文件（再续）

- [x] `kernel/locking/irqflag-debug.c`
  - 文件职责：仅在 `CONFIG_DEBUG_IRQFLAGS` 构建中提供 `warn_bogus_irq_restore()` 错误报告慢路径；
    `raw_local_irq_restore()` 发现 restore 前真实硬件 IRQ 已经开启时调用它，告警后仍继续执行架构
    restore。基线 39 行已经具备完整中文学习注释，本轮未重复追加。
  - 第 17 章验收：完整复读 1～39 行。唯一函数契约位于声明前，覆盖无参数/无返回、调用者、入口
    IRQ 状态、`noinstr` 限制、instrumentation_begin/end 配对、WARN_ONCE 限流、无资源 ownership
    与返回后去向；函数体两个阶段均有紧邻说明。唯一导出宏说明了外部消费者和静态符号生命周期。
    文件无结构体、全局可变状态、局部变量、循环、错误返回或资源回滚；三个复杂函数抽查不适用，
    仅读注释仍可复述“检查→开放插桩窗口→一次告警→关闭窗口→调用者继续 restore”的完整路径。
  - 英文注释验收：除 SPDX、内核标识符和模型归属文本外没有待翻译的非豁免英文说明；现有原注释
    和全部代码逐行保持不变。现有 `中文学习注释模型：` 是基线注释，按“不改写既有前缀”规则保留。
  - 关联读取：`include/linux/irqflags.h` 的 `raw_check_bogus_irq_restore()`、
    `raw_local_irq_restore()` 与 `local_irq_restore()`（学习注释充分）；`include/linux/instrumentation.h`
    的 begin/end 宏及嵌套验证规则（学习注释部分覆盖，建议另立 noinstr 基础设施任务补齐）。
  - 修改安全：本文件源码零改动，`git diff --check` 通过，禁用模板前缀无命中；对完整文件执行
    checkpatch 为 0 errors、0 warnings、39 lines checked。工作树无 `.config`，未执行目标内核配置
    构建。已按本文第 17 章完成强制验收，本文件状态为“全文件完成”。

## 当前文件

- [x] `kernel/locking/rtmutex_api.c`
  - 文件职责：把 `rtmutex.c` 的内部 PI 锁核心包装成公开 rt_mutex API、PI-futex proxy API、调度器
    优先级变化后的 PI 链重检入口，以及 PREEMPT_RT 下由 rtmutex 支撑的普通 mutex API。
  - 首次建图：已完整读取原文件 1～683 行；显式函数定义 40 个。条件编译分为
    `CONFIG_DEBUG_LOCK_ALLOC` 的嵌套 lockdep 包装、`CONFIG_DEBUG_RT_MUTEXES` 的任务释放检查，以及
    `CONFIG_PREEMPT_RT` 的 mutex 替代接口。基线 checkpatch 为 0 errors、0 warnings。
  - 第 17 章验收：完整复读最终 1052 行；40/40 个函数均有紧邻中文契约，覆盖全部参数、返回类别、
    锁/中断/睡眠上下文、task/waiter/wake_q ownership、lockdep 与条件编译差异。实体清单覆盖
    `max_lock_depth`、sysctl 表、PI-futex lock class key、各类 wake_q、flags/token/ret 等重要局部量。
    可仅凭注释复述普通 rt_mutex fast/slow 包装、proxy start→wait→cleanup 竞态闭环、分段 futex
    unlock、调度优先级变化后的 PI 重排，以及 PREEMPT_RT mutex 的 lockdep/I/O-wait 包装。
  - 英文注释与修改安全：全部既有英文注释保持原样并有紧邻独立中文翻译/学习补充；683 条原始行
    逐行保留，新增 369 行、删除 0 行，新增非注释行 0。禁用模板前缀无命中，`git diff --check`
    通过；完整 checkpatch 为 0 errors、0 warnings，增量为 0 errors、212 个 warnings，全部为中文
    UTF-8 行计宽。工作树无 `.config`，未执行目标内核配置构建。状态为“全文件完成”。
  - 关联读取：`kernel/futex/pi.c` 的 lock handoff/start/wait/cleanup（现有学习注释缺失）、
    `kernel/futex/requeue.c` 的 proxy start 返回处理（缺失）、`kernel/sched/syscalls.c` 的
    `rt_mutex_adjust_pi()` 调用点（部分覆盖）、`kernel/sched/core.c` 的 `io_schedule_prepare/finish()`
    （充分）、`include/linux/mutex.h` 的 PREEMPT_RT API 映射（部分覆盖）、`kernel/rcu/tree_plugin.h`
    的人工 rtmutex boost 用法（缺失），以及 `rtmutex_common.h` 的公开声明（充分）；关联文件均未修改。
  - 阶段边界：本文件闭环后按用户要求立即暂停；尚未开始 `rwbase_rt.c`。

## 上一阶段完成文件

- [x] `kernel/locking/rtmutex.c`
  - 文件职责：实现 RT mutex 的 owner 低位协议、waiter 与 owner PI 双红黑树、优先级继承链重排与
    死锁检测、快速/慢速获取释放、owner spinning，并按构建宏生成普通 rtmutex/WW-RT 与 PREEMPT_RT
    sleeping spin/rwlock 共用核心。
  - 首次建图：已完整读取原文件 1～1926 行；显式函数定义共 57 个，含无 WW 的 4 个 stub、
    DEBUG_RT_MUTEXES 开关下两套 5 个 owner fastpath helper、SMP/UP 两套 owner-spin，以及公共排序、
    入出树、PI chain、慢速锁和 RT lock 路径。基线 checkpatch 为 2 errors、7 warnings，均来自既有
    复杂复合字面量宏、重复词、注释对齐、printk/字符串换行和 data_race 提示。
  - 语义地图：owner bit0 把 fast cmpxchg 与 wait_lock 慢路径互斥；每把锁的 waiters 树选 top waiter，
    每个 owner 的 pi_waiters 树只挂各锁 top waiter并决定有效优先级；chainwalk 每步最多持 pi_lock 与
    下一把 wait_lock，并以 task 引用跨越解锁窗口；解锁先降优先级再延迟唤醒，防止 donor 唤醒前反转。
  - 构建地图：`rtmutex_api.c` 定义 RT_MUTEX_BUILD_MUTEX；`ww_rt_mutex.c` 额外定义 WW_RT；
    `spinlock_rt.c` 定义 RT_MUTEX_BUILD_SPINLOCKS。关联文件只读未修改。
  - 第 17 章验收：完整复读最终 2797 行；57 个条件化函数定义与 1 个前置声明均有紧邻中文契约，覆盖
    参数/返回、锁与中断上下文、睡眠性、task/waiter 引用 ownership、配置分支和后续去向。可仅凭注释
    复述 owner tagged pointer、双红黑树、PI chainwalk、WW 假环抑制、等待/超时/信号撤销、deboost 后
    延迟唤醒、自适应 owner spinning，以及普通 rtmutex 与 PREEMPT_RT sleeping lock 两套生成路径。
  - 英文注释与修改安全：全部既有英文注释保持原样，新增译文已拆成相邻独立注释块；1926 条原始行
    逐行保留，新增 871 行、删除 0 行，新增非注释行 0。禁用模板前缀无命中，`git diff --check`
    通过。完整 checkpatch 与基线同为 2 errors、7 warnings，均为既有复合字面量宏、未用宏参数、
    重复词、原始注释对齐、printk/字符串换行和 data_race 提示；增量为 0 errors、358 个 warnings，
    均是中文 UTF-8 行计宽。工作树无 `.config`，未执行目标内核配置构建。状态为“全文件完成”。
  - 阶段边界：本文件闭环后曾按用户要求暂停；本轮已接续并完成 `rtmutex_api.c`。

## 最近完成文件（四续）

- [x] `kernel/locking/rtmutex_common.h`
  - 最终 358 行，13/13 个内联函数及 11 个外部声明均有契约；双树、proxy/futex、chainwalk 与配置
    stub 均已覆盖。新增 121 行、删除 0 行；完整 checkpatch 与基线同为 0 errors/1 warning，增量
    0 errors/45 个中文计宽 warnings；无 `.config`，未构建。状态为“全文件完成”。

- [x] `kernel/locking/test-ww_mutex.c`
  - 最终 1018 行，21/21 个函数及测试实体均有契约；可复述八种基础组合、递归、ABBA/环形恢复和
    三类压力模型。新增 241 行、删除 0 行，777 条原始行保留且无新增非注释行；完整 checkpatch
    0 errors/0 warnings，增量 0 errors/116 个中文计宽 warnings；无 `.config`，未构建。状态为
    “全文件完成”。

- [x] `kernel/locking/ww_rt_mutex.c`
  - 最终 146 行，5/5 个显式函数均有契约；可复述 trylock 的 ctx 分流、共享 PI slowlock、任务状态
    包装与 ww/lockdep/RT owner 解锁顺序。新增 44 行、删除 0 行，102 条原始行保留且无新增非注释行；
    完整 checkpatch 0 errors/0 warnings，增量 0 errors/11 个中文计宽 warnings；无 `.config`，未构建。
    状态为“全文件完成”。

- [x] `kernel/locking/ww_mutex.h`
  - 最终 944 行，普通/RT 各 10 个适配 helper 与 10 个公共算法函数共 30/30 契约齐全；可复述
    Wait-Die/Wound-Wait、优先级/stamp、PROXY_WAKING、双 MB 与插入/解锁。新增 309 行、删除 0 行，
    635 条原始行保留且无新增非注释行；完整 checkpatch 与基线同为 0 errors/4 warnings，增量
    0 errors/74 个中文计宽 warnings；无 `.config`，未构建。状态为“全文件完成”。

- [x] `kernel/locking/osq_lock.c`
  - 最终 344 行，6/6 个函数及节点/percpu 实体均有契约；可复述 CPU 编码、MCS 发布、取消 A/B/C 和
    三类解锁交接。新增 110 行、删除 0 行，234 条原始行保留且无新增非注释行；完整 checkpatch
    0 errors/0 warnings，增量 0 errors/35 个中文计宽 warnings；无 `.config`，未构建。状态为
    “全文件完成”。

- [x] `kernel/locking/mutex-debug.c`
  - 最终 192 行，10/10 个函数均有契约；waiter poison、magic、blocked_on 诊断和 devres reset/
    destroy 生命周期均已覆盖。新增 84 行、删除 0 行，108 条原始行保留且无新增非注释行；完整
    checkpatch 与基线同为 0 errors/3 warnings，增量 0 errors/19 个中文计宽 warnings；无 `.config`，
    未构建。状态为“全文件完成”。

- [x] `kernel/locking/mutex.c`
  - 最终 1872 行，51/51 份函数定义及 3 份慢路径前置声明均有契约；可复述 owner 三位协议、OSQ/
    waiter 两类自旋、共享睡眠慢路径、proxy donor 交接、ww 死锁路径及归零带锁 helper。新增 575 行、
    删除 0 行，1297 条原始行保留且无新增非注释行；完整 checkpatch 与基线同为 0 errors/4 warnings，
    增量 0 errors/146 个中文计宽 warnings；无 `.config`，未构建。状态为“全文件完成”。

- [x] `kernel/locking/mutex.h`
  - 最终 114 行，2/2 个内联函数均有紧邻契约；waiter 全字段、owner 三位协议、7 个调试声明及关闭
    配置空宏均已覆盖。新增 35 行、删除 0 行，79 条原始行逐行保留且无新增非注释行；完整
    checkpatch 与基线同为 0 errors/14 个既有 warnings，增量 0 errors/22 个中文计宽 warnings；
    无 `.config`，未构建。状态为“全文件完成”。

- [x] `kernel/locking/spinlock_rt.c`
  - 最终 447 行，本体 23/23 函数均有契约；可复述 saved_state、RCU-last 防 UAF、trylock 回滚、
    rwbase 偏置和 PI/公平边界。新增 139 行、删除 0 行，308 条原始行保留且无新增非注释行；完整
    checkpatch 与基线同为 0 errors/3 warnings，增量 0 errors/68 个中文计宽 warnings；无 `.config`，
    未构建。状态为“全文件完成”。

- [x] `kernel/locking/spinlock_debug.c`
  - 最终 353 行，20/20 函数均有契约；可复述首错门控、无引用 owner 诊断、两类递归、阻塞/
    trylock 非对称、MMIOWB 释放次序及 writer-only rwlock owner。新增 117 行、删除 0 行，236 条
    原始行保留且无新增非注释行；完整 checkpatch 与基线同为 2 errors/7 warnings，增量 0 errors/
    61 个中文计宽 warnings；无 `.config`，未构建。状态为“全文件完成”。

- [x] `kernel/locking/spinlock.c`
  - 文件职责：提供未内联 raw spinlock/rwlock API、可重新调度的争用循环、lockdep 嵌套入口、
    MMIOWB 默认 percpu 状态、锁文本地址判定和 PREEMPT_RT softirq 断言桥接。
  - 第 17 章验收：完整复读最终 625 行；34 个显式函数均有紧邻契约，4 个 `BUILD_LOCK_OPS`
    模板由生成器契约覆盖。实体说明覆盖 percpu 状态、relax 回退、nested-write 回退和配置门；
    非豁免英文注释均有邻接翻译。可仅凭注释复述四种获取/释放上下文责任、失败轮次恢复、lockdep
    登记、MMIOWB 状态和锁文本边界；softirq 桥接的引入提交 `c5bcab755822` 也已核对。
  - 修改安全：新增 198 行、删除 0 行，427 条原始行逐行保留，新增非注释行 0；禁用前缀无命中，
    `git diff --check` 通过。基线 checkpatch 为 0/0，增量为 0 errors、67 个中文计宽 warnings；
    完整文件工具受函数生成宏前 UTF-8 注释影响，在未改原行产生 4 个 spacing errors/4 warnings，
    已如实保留该工具边界。无 `.config`，未构建。状态为“全文件完成”。

- [x] `kernel/locking/qrwlock.c`
  - 文件职责：实现通用排队 rwlock 的读写慢路径，以原子 cnts 编码读计数和 writer 状态，并用公平
    wait_lock 排列普通上下文；中断 reader 对仅 WAITING 的 writer 采用防自死锁旁路。
  - 第 17 章验收：完整复读最终 148 行。两个函数均有紧邻契约，参数、上下文、返回、内部锁
    ownership、acquire/release 和出口责任齐全；12 个原英文注释块全部紧邻翻译。可仅凭注释复述
    普通 reader 撤销 bias→排队→重加→等 writer→传队首，中断 reader 只等 LOCKED，以及 writer
    直接获取或 WAITING→等仅读者清空→LOCKED 的三条路径。
  - 关联读取：通用 qrwlock 类型/快路径/解锁和 trace 标志（学习注释缺失）；qspinlock 公平内部锁
    背景（学习注释充分）。关联文件未修改。
  - 修改安全：新增 56 行、删除 0 行，92 条原始行逐行保留，新增非注释行 0；禁用模板前缀无命中，
    `git diff --check` 通过。完整文件 checkpatch 与基线一致，均仅 1 个既有“条件内赋值”error、
    0 warnings；增量为 0 errors、18 warnings，均为中文 UTF-8 行计宽。工作树无 `.config`，未执行
    目标内核配置构建。内容与修改安全验收完成，状态为“全文件完成”。

- [x] `kernel/locking/qspinlock_stat.h`
  - 文件职责：在 PV 与锁事件计数同时启用时提供强 `lockevent_read()`，把哈希步数、kick 调用耗时和
    kick-to-wake 延迟写入 percpu 事件槽，并利用定义顺序把后续 PV wait/kick 调用包装到统计层。
  - 第 17 章验收：完整复读最终 227 行。函数清单为强读取、hop、kick、wait 四个实现和关闭计数时
    的独立 hop stub，共 5 个配置后定义，5/5 均有紧邻契约。实体清单覆盖两层配置、EVENT_COUNT、
    percpu 时间戳及两个包装宏；15 个原注释块中 4 个许可证/结构尾标豁免，其余英文均完整配对。
  - 抽查记录：可仅凭注释复述事件 ID 校验、possible CPU 非快照汇总、三类平均值分母与 X.XX 格式；
    kicker 跨 CPU 写时间戳、发起侧 kick latency、waiter 清零/消费 wake latency；以及“包装函数先
    定义、宏后生效、后续 PV 文件被包装而自身不递归”的预处理路径。关闭配置 stub 仍按 C 规则求值
    实参，未误写成空宏语义。
  - 关联读取：已完成的 lockevent 与 qspinlock 前序文件（学习注释充分）；fs 用户缓冲 helper 与
    sched_clock 跨 CPU 边界（学习注释缺失，统计只作观测）。关联文件未修改。
  - 修改安全：新增 85 行、删除 0 行，142 条原始行逐行保留，新增非注释行 0；禁用模板前缀无
    命中，`git diff --check` 通过。完整文件 checkpatch 为 0 errors、0 warnings、227 lines checked；
    增量为 0 errors、20 warnings，均为中文 UTF-8 行计宽。工作树无 `.config`，未执行目标内核配置
    构建。内容与修改安全验收完成，状态为“全文件完成”。

- [x] `kernel/locking/qspinlock_paravirt.h`
  - 文件职责：为 PV qspinlock 实现受 pending 约束的混合偷锁、vCPU 自适应 halt/kick、锁到队头节点
    的早期开放寻址哈希、SLOW 发布协议，以及必须先 unhash 再 release 的 PV 解锁快慢路径。
  - 第 17 章验收：完整复读最终 907 行。函数清单为 15 个配置后定义，覆盖混合 trylock、pending
    位宽两侧四个 helper、哈希初始化/登记/移除、自适应判定、四个真实 PV callback 和两个 unlock
    路径；15/15 均有紧邻中文契约。实体清单覆盖生成入口约束、SLOW 值、状态枚举、`pv_node`、
    哈希 entry/容量/全局表/遍历宏和体系结构 thunk。46 个原注释块中 4 个许可证/结构尾标豁免，
    其余全部有紧邻翻译与机制补充；原文 `nozero` 和把实际 xchg 称为 cmpxchg 的差异均明确记录。
  - 抽查记录：可仅凭注释复述非队头 HALTED 宣布与前驱 HASHED 推进如何避免丢失唤醒；队头如何
    pending 禁偷锁、短暂 acquire、自建或复用哈希、发布 SLOW、挂起和处理重复等待；unlock 如何从
    失败回填值识别 SLOW，按 RMB→unhash→store-release→kick 顺序释放。两个 pending 位宽分支与
    哈希无槽/无条目的 BUG 不变量也已分别复述。
  - 生命周期验收：哈希表由早期内存永久分配；每把阻塞锁只占一个条目，owner 在 release 前移除。
    store-release 后锁对象可立刻释放/复用，慢路径不再解引用它，但 percpu `pv_node` 仍可读 CPU 并
    kick。所有锁、节点参数均为借用，只有早期哈希初始化取得永久存储。
  - 关联读取：已完成的 qspinlock 三个前序文件；x86 callee-save thunk、paravirt ops/wait/kick、KVM
    安装路径和 memblock 大哈希声明（学习注释缺失）；`qspinlock_stat.h` 统计包装（下一文件处理）。
  - 修改安全：新增 350 行、删除 0 行，557 条原始行逐行保留，新增非注释行 0；禁用模板前缀无
    命中，`git diff --check` 通过。完整文件 checkpatch 与基线一致，均为 0 errors、2 个既有 warnings
    （函数名与左括号间空格、声明后缺空行）；增量为 0 errors、93 warnings，均为中文 UTF-8 计宽。
    工作树无 `.config`，未执行目标内核配置构建。内容与修改安全验收完成，状态为“全文件完成”。

- [x] `kernel/locking/qspinlock.c`
  - 文件职责：实现 qspinlock 的 pending 优化与 MCS 排队慢路径，管理每 CPU 四个嵌套节点，并通过
    宏重命名和受控自包含从同一函数体生成 native/PV 两个版本；同时登记 `nopvspin` 早期参数。
  - 第 17 章验收：完整复读最终 643 行。源码函数清单为四个原生 PV stub、核心慢路径和参数回调，
    共 6 个定义，全部有紧邻专属契约；核心函数在 PV 构建下生成两个编译后符号。实体清单覆盖
    percpu `qnodes`、嵌套 count、全部 callback/生成宏、`nopvspin` 和 early_param。32 个原注释块中
    仅 `_GEN_PV_LOCK_SLOWPATH` 结构尾标豁免，其余英文说明均有紧邻翻译和机制补充；两处原文误称
    `queued_spin_unlock_slowpath()` 均保留并明确当前代码实际生成 lock slowpath。
  - 抽查记录：以同一复杂函数的三条路径完成内容验收，可仅凭注释复述 pending 获取/撤销与 acquire
    交接、四槽耗尽的直接 trylock 退化与统一 count/trace 清理、完整 MCS 发布/链接/队头等待/清尾或
    后继交接。另可复述首次包含、PV callback 替换、自包含第二轮和递归终止，以及参数出现到体系
    结构消费 `nopvspin` 的生命周期。
  - 并发验收：注释明确 count 先增与编译器 barrier 防 IRQ 覆盖、节点初始化到 relaxed 尾发布间的
    `smp_wmb()`、前驱/后继 MCS release-acquire、队头锁字 acquire、并发 pending 使无竞争摘尾失败，
    以及所有预留节点出口必须归还 count。pending 快路径不预留节点，故不误入 release 清理段。
  - 关联读取：已完成的 `qspinlock.h`、`mcs_spinlock.h`（学习注释充分）；通用 qspinlock 公共快路径、
    x86 pending 覆盖、trace 标志（学习注释缺失）；`qspinlock_paravirt.h` 的真实 callback 与哈希/
    休眠路径（下一文件处理）。关联文件未修改。
  - 修改安全：新增 233 行、删除 0 行，410 条原始行逐行保留，新增非注释行 0；禁用模板前缀无
    命中，`git diff --check` 通过。完整文件 checkpatch 与未修改基线一致，均为 1 error（既有条件
    内赋值）和 1 warning（既有声明后缺空行）；增量 checkpatch 为 0 errors、82 warnings，均为中文
    UTF-8 行计宽。工作树无 `.config`，未执行目标内核配置构建。内容与修改安全验收完成，本文件
    状态为“全文件完成”。

- [x] `kernel/locking/qspinlock.h`
  - 文件职责：定义不同 qspinlock 慢路径共享的尾码编解码、percpu MCS 节点定位、pending 状态转换、
    队尾原子替换和最终 locked 写入 helper，并按 pending 位宽选择混合宽度或整字 RMW 实现。
  - 第 17 章验收：完整复读修改后的 375 行。函数清单为 11 个配置后定义：三个尾码/节点 helper，
    `_Q_PENDING_BITS == 8` 两侧各三个独立状态 helper，默认 pending acquire 和 `set_locked()`；所有
    函数均有紧邻中文契约，覆盖参数、返回、状态前置条件、内存序、ownership 和调用后去向。实体
    清单覆盖 include guard、四节点上限、pending 轮询值、`qnode` 及 PV 填充、组合掩码、配置覆盖点
    和全部局部变量。19 个原注释块中 SPDX 与三个结构尾标豁免，其余英文均有紧邻完整翻译和补充。
  - 抽查记录：可仅凭注释复述 CPU+1/两位索引的编码与 percpu 反解，解释 8 位 pending 分支为何能
    分别写 byte/halfword，以及 1 位分支如何用 cmpxchg 失败回填的最新 old 保留并发低位。还明确
    记录整字 `xchg_tail()` 实际可能返回 locked/pending，而 16 位分支返回值低位为零；公共调用者只
    解释 tail。`set_locked()` 三元组中的 pending=0 是调用前置条件，并非代码主动清除 pending。
  - 关联读取：`include/asm-generic/qspinlock_types.h` 的锁字联合体与位布局、
    `include/asm-generic/qspinlock.h` 的公共快路径/解锁（均缺失学习注释）；`qspinlock.c` 的 percpu
    节点和全部调用点（下一文件处理）；`arch/x86/include/asm/qspinlock.h` 的 pending 覆盖实现
    （学习注释缺失）。关联文件未修改。
  - 修改安全：新增 174 行、删除 0 行，201 条原始行逐行保留，新增非注释行 0；禁用模板前缀无
    命中，`git diff --check` 通过。完整文件 checkpatch 为 0 errors、0 warnings、375 lines checked；
    增量 checkpatch 为 0 errors、34 warnings，均为中文 UTF-8 行计宽。工作树无 `.config`，未执行
    目标内核配置构建。内容验收和修改安全检查均完成，本文件状态为“全文件完成”。

- [x] `kernel/locking/mcs_spinlock.h`
  - 文件职责：定义基础 MCS 排队自旋锁的获取/释放接口和可由体系结构覆盖的等待、交接宏；竞争者以
    自有节点串成 FIFO 队列，并在本地 `locked` 字段上等待，减少共享缓存行反复抖动。
  - 第 17 章验收：完整复读修改后的 215 行。函数清单仅 `mcs_spin_lock()` 与 `mcs_spin_unlock()`，
    两个紧邻契约均覆盖参数、调用上下文、返回、节点 ownership/生命周期、路径、失败边界和后续责任；
    实体清单覆盖 include guard、两个体系结构覆盖点、关联结构体的队尾/`next`/`locked` 关系和局部
    `prev`/`next` 快照。原有 15 个注释块中，除 SPDX 与 include-guard 尾标外，全部英文说明都有
    紧邻完整翻译和机制补充。文件只有两个函数，三个复杂函数抽查不适用；已分别复述无竞争获取、
    竞争入队与本地等待、空队列释放、后继链接竞态等待和 release/acquire 直接交接路径。
  - 并发验收：注释明确区分共享队尾发布与节点链接发布，解释 `xchg()` 的 acquire/全局传递排序、
    `WRITE_ONCE`/`READ_ONCE` 闭合的入队窗口、`cmpxchg_release()` 成功与失败含义，以及弱内存序架构上
    release-acquire 链不自动形成跨 CPU 全屏障的边界。调用者必须为每次获取独占节点，并保持节点
    有效到配对解锁返回；接口不分配、不回收且不睡眠。
  - 关联读取：`include/asm-generic/mcs_spinlock.h` 的节点布局与覆盖协议、
    `arch/arm/include/asm/mcs_spinlock.h` 的 `wfe()`/`dsb_sev()` 实现（均缺失学习注释）；
    `include/asm-generic/barrier.h` 的条件 acquire/release 契约和 `include/linux/rcupdate.h` 的
    `smp_mb__after_unlock_lock()` 契约（学习注释充分）；`Documentation/memory-barriers.txt` 的
    release-acquire 链边界（英文原始文档）。关联文件未修改。
  - 修改安全：新增 102 行、删除 0 行，113 条原始行逐行保留，新增非注释行 0；禁用模板前缀扫描
    无命中，`git diff --check` 通过。完整文件 checkpatch 为 0 errors、1 warning，该“memory barrier
    without comment”在未修改基线中同样存在；增量 checkpatch 为 0 errors、40 warnings，其中同一
    基线屏障告警 1 条，其余 39 条均为中文 UTF-8 行计宽。工作树无 `.config`，未执行目标内核配置
    构建。已完成内容验收和修改安全检查，本文件状态为“全文件完成”。

- [x] `kernel/locking/rwbase_rt.c`
  - 文件职责：作为被 `rwsem.c` 与 `spinlock_rt.c` 文本包含的公共状态机，以 `readers` 中的
    `READER_BIAS`/活跃 reader 数/`WRITER_BIAS` 编码读写状态；reader 通过 acquire CAS 走无锁快路径，
    writer 先取得支持 PI/DL 的 rtmutex、移除 bias，再等待读者清空。算法刻意不保证 writer 公平，
    但保持 RT waiter 对 rtmutex owner 的优先级继承。
  - 第 17 章验收：完整复读最终 530 行。11/11 个函数均有紧邻专属契约：`rwbase_read_trylock()`
    （82～100）、`__rwbase_read_lock()`（118～209）、`rwbase_read_lock()`（220～230）、
    `__rwbase_read_unlock()`（246～275）、`rwbase_read_unlock()`（287～305）、
    `__rwbase_write_unlock()`（320～338）、`rwbase_write_unlock()`（349～359）、
    `rwbase_write_downgrade()`（371～382）、`__rwbase_write_trylock()`（394～414）、
    `rwbase_write_lock()`（431～496）和 `rwbase_write_trylock()`（509～530）。参数、返回、睡眠上下文、
    内部锁 ownership、rwsem/RT rwlock 配置差异及后续配对均已逐项覆盖。
  - 实体与英文注释验收：文件不定义结构体或全局变量；契约/变量地图覆盖借用 `rwb`、内嵌 `rtm`、
    状态字 `readers`、等待状态 `state`、IRQ `flags`、CAS 快照 `r`、结果 `ret`、owner 快照及两类
    wake queue。原文件 15 处注释中 SPDX 豁免，其余 14 处英文均逐字保留并有紧邻翻译与机制补充；
    310 条原始行全部按序保留。
  - 路径与并发验收：抽查读慢路径，可复述持 `wait_lock` 入 rtmutex 慢锁如何闭合 reader/writer 竞态；
    抽查阻塞写路径，可复述 rtmutex→移除 bias→等待读者归零→信号加回 bias 的完整成功/回滚流程；
    抽查 trylock/降级路径，可复述“失败等同未尝试”和写转读计数。并发清单覆盖读 CAS acquire 与写
    恢复 bias release、最后 reader release 与 writer acquire、wait_lock 下 owner/wake_q 稳定、
    preempt disable/enable 唤醒配对及 writer 非公平边界。
  - 关联读取：`include/linux/rwbase_rt.h` 的 bias、结构体、初始化与状态查询（学习注释缺失，建议后续
    补注该短头文件）；`rwsem.c` 1451～1565 行的宏适配与公开包装（学习注释缺失，下一文件处理）；
    `spinlock_rt.c` 229～432 行的 RT rwlock 适配与包装、`rtmutex.c` 的 wake_q、普通/RT 慢锁，
    `kernel/sched/core.c` 的 pre/schedule/post hook 及 `include/linux/sched.h` 的保存任务状态宏（所读
    区域学习注释充分）。关联文件均未修改。
  - 修改安全：新增 220 行、删除 0 行，新增非注释行 0；禁用模板前缀扫描无命中，`git diff --check`
    通过。完整文件 checkpatch 为 0 errors/0 warnings；增量为 0 errors/134 个中文 UTF-8 视觉行宽
    warnings。工作树无 `.config`，未执行目标内核配置构建。已按方法论第 17 章完成强制验收，状态为
    “全文件完成”。

- [x] `kernel/locking/rwsem.c`
  - 文件职责：实现可睡眠读写信号量。非 RT 分支以 `count` 位域编码 writer/waiter/handoff/reader，
    以 `wait_lock` 维护环形 waiter 队列、phase-fair reader 批授予和 writer 强制 handoff，并可通过
    OSQ 对 owner 乐观自旋；PREEMPT_RT 分支把相同公开 API 适配到 `rwbase_rt`/rtmutex。
  - 第 17 章验收：完整复读最终 2365 行。按条件编译展开共 78 个函数定义，78/78 均有紧邻专属契约；
    覆盖 owner/count helper、waiter 删除/遍历/唤醒、writer handoff、OSQ 自旋及关闭配置 stubs、读写
    慢路径、非 RT/RT 两套内部包装、公开/trylock/nested/non-owner API。函数头逐项说明参数、返回、
    睡眠状态、锁与引用 ownership、成功/信号失败保证和后续配对，未发现错位契约。
  - 实体与英文注释验收：实体清单覆盖 owner/count 全部位域宏、调试宏、`rwsem_waiter_type`、
    `rwsem_waiter` 五字段、三种 wake type、等待超时/reader 批量上限、四种 owner state、OSQ 与 RT
    adapter 宏；参数和重要局部快照/临时链表/wake_q 均在契约或阶段块解释。原文件 1786 行与 99 处
    注释起点/行尾说明全部按序保留，非豁免英文均有紧邻翻译和机制补充；明确修正原文不存在的
    `RWSEM_READ_OWNED` 宏名及方向相反的 `#else /* !CONFIG_PREEMPT_RT */` 尾注释。
  - 路径与并发验收：抽查 `rwsem_mark_wake()`，可复述首 reader 预授予、phase-fair 两遍批处理、先
    task 引用后 release 清 NULL、锁外唤醒；抽查 `rwsem_down_read_slowpath()`，可复述预加 bias、偷锁、
    入队撤销、signal 与授予的 wait_lock 仲裁；抽查 `rwsem_down_write_slowpath()`，可复述 OSQ、入队、
    RT/DL/超时 handoff、自旋交接与信号删队。并发清单覆盖 count acquire/release、owner 裸指针的
    preempt-disabled RCU 生命周期、barrier 后解引用、wait_lock 队列保护及 downgrade 发布边界。
  - 关联读取：`include/linux/rwsem.h` 的两种结构布局、初始化/查询/公开声明（学习注释缺失，建议另立
    头文件任务）；`include/linux/osq_lock.h` 接口与 `osq_lock.c` 获取/取消/解锁协议（实现学习注释
    充分，头文件缺失）；`rwbase_rt.c`、`rtmutex.c` 的 RT adapter 目标（学习注释充分）；已完成的
    lock events 文件提供统计编号语义。关联文件均未修改。
  - 修改安全：新增 579 行、删除 0 行，新增非注释行 0；禁用模板前缀扫描无命中，`git diff --check`
    通过。完整文件 checkpatch 为 0 errors/2 warnings，均落在未改原代码（调试宏缩进、既有 acquire
    屏障提示）；增量为 0 errors/226 个中文 UTF-8 视觉行宽 warnings。工作树无 `.config`，未执行
    目标配置构建。已按方法论第 17 章完成强制验收，状态为“全文件完成”。

## 目录状态

- [x] `kernel/locking` 清单 33/33 个文件均已完成单文件闭环；目录级复核未发现 `[ ]` 或 `[~]` 遗留，
  所有目标文件均已按方法论第 17 章记录内容验收、修改安全边界和不可用的构建验证，状态为“全文件完成”。
