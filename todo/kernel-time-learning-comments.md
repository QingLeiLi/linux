# kernel/time 学习注释任务清单

## 使用规则

本清单是 `kernel/time` 目录学习注释长任务的持久化进度源。会话中断或上下文压缩后，先读取本文件
并核对当前工作树，不凭对话记忆推断进度。

严格采用单文件闭环：

1. 开始前把目标文件标为 `[~]`，完整读取并建立函数、实体、英文注释和语义索引。
2. 仅追加中文学习注释；不改代码，不删除、改写或移动原有注释。
3. 按源码顺序以 1～5 个函数为一批修改，每批复读当前窗口并检查局部 diff。
4. 完整复读修改后的文件，按方法论第 17 章完成独立内容验收和修改安全检查。
5. 只有验收全部通过后才标为 `[x]`；一个 `[~]` 文件未闭环前不开始下一个文件。
6. 为核对契约而读取的关联源码只登记到当前文件记录；读取不等于授权修改关联文件。

状态：`[ ]` 未开始；`[~]` 正在处理或验收；`[x]` 已通过第 17 章单文件闭环；`[-]` 已处理跳过
或不属于学习注释目标。

## 规则与范围

- 主标准：`doc/linux-kernel-source-learning-methodology.md`，本任务开始时已完整读取 1413 行。
- 硬门禁：函数专属契约、阶段走读、实体与变量、英文注释翻译补充、并发/生命周期、失败路径、
  关联读取、三函数抽查和零代码/零原注释改动审计。
- 纳入：目录中的 C、头文件、Kconfig、Makefile 和 `timeconst.bc`；构建元数据按其语法和副作用验收。
- 排除：`.kunitconfig` 是供 KUnit 使用的两行机器配置片段，不适合插入学习注释。
- 已处理判据：提交历史显示文件曾集中新增数百行系统学习注释，当前函数契约和阶段说明覆盖密集；
  这类文件按用户要求直接跳过，不在本任务重复改写或重新验收。

## 文件进度（按建议学习顺序）

### 构建入口与基础换算

- [x] `kernel/time/Makefile`
- [x] `kernel/time/Kconfig`
- [x] `kernel/time/timeconst.bc`
- [x] `kernel/time/timeconv.c`
- [x] `kernel/time/timecounter.c`
- [x] `kernel/time/jiffies.c`
- [~] `kernel/time/time.c`
- [ ] `kernel/time/time_test.c`
- [ ] `kernel/time/test_udelay.c`

### clocksource、clockevents 与调度时钟

- [ ] `kernel/time/clocksource.c`
- [ ] `kernel/time/clocksource-wdtest.c`
- [ ] `kernel/time/clockevents.c`
- [ ] `kernel/time/sched_clock.c`
- [ ] `kernel/time/vsyscall.c`

### timekeeping 与 NTP

- [-] `kernel/time/timekeeping.c`（历史提交已系统处理，直接跳过）
- [ ] `kernel/time/timekeeping.h`
- [ ] `kernel/time/timekeeping_internal.h`
- [ ] `kernel/time/timekeeping_debug.c`
- [ ] `kernel/time/ntp.c`
- [ ] `kernel/time/ntp_internal.h`

### 低精度与高精度定时器

- [-] `kernel/time/timer.c`（历史提交已系统处理，直接跳过）
- [ ] `kernel/time/timer_list.c`
- [-] `kernel/time/hrtimer.c`（历史提交已系统处理，直接跳过）
- [ ] `kernel/time/sleep_timeout.c`
- [ ] `kernel/time/itimer.c`
- [ ] `kernel/time/alarmtimer.c`

### POSIX 时钟与定时器

- [ ] `kernel/time/posix-clock.c`
- [ ] `kernel/time/posix-timers.c`
- [ ] `kernel/time/posix-timers.h`
- [ ] `kernel/time/posix-cpu-timers.c`
- [ ] `kernel/time/posix-stubs.c`

### tick、NO_HZ 与广播

- [-] `kernel/time/tick-common.c`（历史提交已系统处理，直接跳过）
- [ ] `kernel/time/tick-internal.h`
- [ ] `kernel/time/tick-legacy.c`
- [ ] `kernel/time/tick-oneshot.c`
- [ ] `kernel/time/tick-sched.c`（仅有局部历史注释，仍需全文件处理）
- [ ] `kernel/time/tick-sched.h`
- [ ] `kernel/time/tick-broadcast.c`
- [ ] `kernel/time/tick-broadcast-hrtimer.c`

### 时间命名空间

- [ ] `kernel/time/namespace.c`
- [ ] `kernel/time/namespace_internal.h`
- [ ] `kernel/time/namespace_vdso.c`

### timer migration

- [ ] `kernel/time/timer_migration.c`（仅有局部历史注释，仍需全文件处理）
- [ ] `kernel/time/timer_migration.h`

### 排除项

- [-] `kernel/time/.kunitconfig`（机器配置片段，不插入学习注释）

## 当前文件

- [~] `kernel/time/time.c`
  - 待建立时间系统调用、timespec/timeval 转换与旧 ABI 兼容索引。

## 已完成文件

- [x] `kernel/time/jiffies.c`
  - 文件职责：提供最低 rating 的 32 位 jiffies clocksource、32 位平台 seqcount 保护的 jiffies_64
    快照、架构频率校准的 refined-jiffies，以及秒/USER_HZ/毫秒和内核 jiffy 的 sysctl 适配。
  - 第 17 章验收：完整复读最终 497 行。按配置展开共 27 个物理函数定义，27/27 均有紧邻专属
    契约；覆盖 clocksource 永久对象、jiffies_lock/jiffies_seq、启动期标志、refined 静态对象、
    所有转换回调和五个公共 sysctl 入口。全部非豁免英文注释逐字保留并有邻接翻译补充。
  - 路径与并发验收：抽查 jiffies read/get64、refined 注册、sysctl 双向转换，可复述 32 位回绕、
    seqcount 写侧配对、Q8 频率校准、READ/WRITE_ONCE 和配置关闭 -ENOSYS 桩。当前 int 型
    `proc_dointvec_ms_jiffies_minmax` 回调向 `proc_int_conv()` 传 false，extra1/extra2 不生效，已按
    当前源码如实标注而未越权修代码；ulong minmax 路径在换算后检查范围。
  - 关联读取：已处理的 `kernel/time/tick-common.c` jiffies 写段和 `timekeeping.c:do_timer()`；
    `include/linux/clocksource.h` 的 max_cycles 契约、`kernel/time/clocksource.c:cycles_to_nsec_safe()`；
    `kernel/sysctl.c` 的 int/ulong 转换与范围检查；`include/linux/jiffies.h` 声明；x86/s390 默认与
    refined 调用点。除已处理 time 文件外，这些区域学习注释缺失或部分覆盖，均仅核对未修改。
  - 修改安全：新增 181 行、删除 0 行，禁用模板前缀无命中，`git diff --check` 通过。完整文件
    checkpatch 为 0 errors/7 warnings，均落在原有宏声明、EXPORT 距定义位置和 -ENOSYS 桩；新增
    注释无告警。工作树无 `.config`，未执行目标构建。已按方法论第 17 章完成强制验收，状态为
    “全文件完成”。

- [x] `kernel/time/timecounter.c`
  - 文件职责：在无状态、会回绕的硬件 `cyclecounter` 上维护 `cycle_last/nsec/frac`，把周期差按
    mult/shift 转为带小数余量的连续纳秒时间轴，供 PTP、NIC、CAN 等设备时钟使用。
  - 第 17 章验收：完整复读最终 110 行。`timecounter_init()`、静态 `timecounter_read_delta()`、
    `timecounter_read()` 3/3 均有紧邻专属契约；覆盖参数、局部量、回调上下文、调用者串行化、
    单次回绕模减、定点余量、状态提交和回绕过多的不可检测失败。全部英文注释逐字保留并邻接翻译；
    对“首次 delta 调用会初始化”的旧说明追加当前实现修正，明确必须先 init。
  - 抽查记录：三个函数均已抽查，可仅凭注释复述初始化基点、周期读取/换算/提交和绝对纳秒累计；
    并发清单确认本层无锁，具体 cc 回调和同一 tc 的 read/adjtime/reinit 串行化均由调用者负责。
  - 关联读取：`include/linux/timecounter.h` 的两个结构体、`cyclecounter_cyc2ns()`、adjtime 和
    `timecounter_cyc2time()`（学习注释缺失）；`drivers/net/ethernet/freescale/fec_ptp.c:fec_time_keep()`
    的每秒读与双锁调用示例（学习注释缺失）。关联文件均未修改，建议随头文件/驱动子系统补注。
  - 修改安全：新增 46 行、删除 0 行，禁用模板前缀无命中；`git diff --check` 通过，checkpatch
    为 0 errors/0 warnings。工作树无 `.config`，未执行目标对象构建。已按方法论第 17 章完成
    强制验收，状态为“全文件完成”。

- [x] `kernel/time/timeconv.c`
  - 文件职责：把带固定秒偏移的 64 位 Unix 秒数拆成内核 `struct tm` 八个字段；先规范化负余数与
    跨日 offset，再用 March-based Gregorian 仿射算法常数时间求公历日期。
  - 第 17 章验收：完整复读最终 205 行。唯一 `time64_to_tm()` 有紧邻专属契约，覆盖 3 个参数、
    15 个局部中间量、输出 ownership、可调用上下文和 6 个阶段；全部非许可证英文注释逐字保留并
    有邻接完整翻译，文件来源/许可证块按规范豁免。负时间、星期、闰年、计算历和最终提交均已覆盖。
  - 抽查记录：文件仅一个复杂函数；可仅凭注释复述 -1 秒及大 offset 的跨日归一化、1970-01-01
    星期四基准、400 年/世纪拆分、一二月计算年修正和 `tm_year/tm_mon/tm_yday` 编码。
  - 关联读取：`include/linux/time.h` 的 `struct tm` 字段与公开声明（学习注释缺失）；
    `kernel/time/time_test.c:time64_to_tm_test_date_range()` 的正负 80000 年逐日测试（学习注释缺失，
    已在本清单排队）；`lib/vsprintf.c:time64_str()` 和 `fs/fat/misc.c:fat_time_unix2fat()` 的消费者
    （学习注释缺失）。关联文件均仅核对未修改，目录外文件建议随各自子系统任务处理。
  - 修改安全：新增 64 行、删除 0 行，禁用模板前缀无命中；`git diff --check` 通过，checkpatch
    为 0 errors/0 warnings。工作树无 `.config`，未运行 KUnit 或目标对象构建。已按方法论第 17 章
    完成强制验收，状态为“全文件完成”。

- [x] `kernel/time/timeconst.bc`
  - 文件职责：构建期读取 HZ，用任意精度整数计算 jiffy 与毫秒/微秒/纳秒换算所需的 32 位定点
    MUL/ADJ/SHR 三元组及最简 NUM/DEN 比例，并生成带 HZ 一致性检查的 `timeconst.h`。
  - 第 17 章验收：完整复读最终 205 行。`gcd()`、`fmul()`、`fadj()`、`fmuls()`、`timeconst()`
    5/5 均有紧邻专属契约；覆盖全部参数、bc 局部/全局变量、整数截断、向上取整补偿、shift 搜索、
    输出进制切换、非法 HZ 和五阶段生成流程。原有 3 个英文注释块逐字保留并有紧邻翻译与机制补充。
  - 路径抽查：可仅凭注释复述 gcd 约分、定点倒数三元组和 timeconst 头文件生成；本文件为单线程
    构建主机脚本，没有内核锁、RCU、引用或对象 ownership。hz < 2 的失败通过生成 `#error` 表达。
  - 关联读取：未读取目标之外的源码；生成宏消费者将在后续 timeconv/time 相关文件中按需核对。
  - 修改安全：新增 88 行、删除 0 行，禁用模板前缀无命中，`git diff --check` 通过；以 HZ=100
    分别运行 HEAD 脚本和当前脚本，输出逐字一致。checkpatch 为 0 errors/1 warning，唯一 warning
    来自原有生成字符串包含文件名的规则误报。已按方法论第 17 章完成强制验收，状态为“全文件完成”。

- [x] `kernel/time/Kconfig`
  - 文件职责：区分由架构/子系统选择的隐藏时间能力位与用户可选策略；构造 periodic、idle dynticks、
    full dynticks 三选一的 tick 模式，并声明 high-res timer、context tracking、辅助 POSIX clock 与
    KUnit 测试的依赖和 select 副作用。
  - 第 17 章验收：完整复读最终 281 行。28 个 config、1 个 choice、1 个 menu 均有邻接中文职责、
    依赖与运行期影响说明；全部原英文标题、注释和 help 文本逐字保留，并有紧邻完整翻译与学习补充。
    Kconfig 无 C 函数/结构体/ownership/cleanup，三复杂函数抽查不适用；抽查 broadcast 能力链、
    NO_HZ_FULL 依赖链和 POSIX_AUX_CLOCKS，可仅凭注释说明能力与策略边界及关闭后的构建结果。
  - 关联读取：`kernel/time/Makefile` 的 CONFIG 到对象映射，用于确认 GENERIC_CLOCKEVENTS、
    TICK_ONESHOT、NO_HZ_COMMON 等符号对实际编译单元的影响；该文件已完成学习注释，未再次修改。
  - 修改安全：首次审计发现 16 条原行缩进被意外改变，已按 HEAD 精确恢复并重新审计；最终新增
    61 行、删除 0 行，新增非注释语句 0，`git diff --check` 通过，禁用模板前缀无命中。
    checkpatch 为 0 errors/1 warning，warning 是 choice 没有四行 help 的既有结构规则，不为消警
    增加会改变配置界面的 help。`scripts/kconfig/conf` 未构建，未执行解析。已按方法论第 17 章完成
    强制验收，状态为“全文件完成”。

- [x] `kernel/time/Makefile`
  - 文件职责：把基础 timekeeping、timer/hrtimer、NTP、clocksource 与换算对象无条件编入
    `built-in.a`，再按 POSIX timers、clockevents/oneshot、SMP NO_HZ、时间命名空间和测试配置
    选择互斥实现或附加对象；单独为 `sched_clock.o` 关闭不适合 noinstr 路径的分支插桩。
  - 第 17 章验收：完整复读最终 62 行。文件没有函数、结构体、局部变量或运行期控制流，函数清单
    与三个复杂函数抽查不适用；构建实体清单覆盖唯一对象专属 CFLAGS、3 个无条件 obj-y 组、
    POSIX 完整实现/ABI stub 互斥分支、broadcast 与 SMP 双层门控，以及 10 项独立能力/测试对象。
  - 英文与路径验收：唯一非豁免英文注释逐字保留并有紧邻完整翻译及 noinstr/递归风险补充；
    可仅靠注释复述 built-in 链接、POSIX 关闭时的符号替代、oneshot/broadcast 组合和测试对象边界。
    本文件不获取资源、不参与运行期锁/RCU/引用协议，也无失败回滚路径。
  - 关联读取：`kernel/time/Kconfig` 中相关配置符号的本目录定义位置，用于确认对象选择来自布尔能力
    或测试开关；该文件当时学习注释缺失，已转为下一目标，未在本文件闭环中修改。
  - 修改安全：新增 27 行、删除 0 行，新增非注释行 0；禁用模板前缀扫描无命中，
    `git diff --check` 通过；完整文件 checkpatch 为 0 errors/0 warnings。工作树无 `.config`，
    未执行目标内核配置构建。已按方法论第 17 章完成强制验收，状态为“全文件完成”。

## 目录状态

- 已处理跳过：4 个。
- 排除：1 个。
- 已完成闭环：6 个。
- 正在处理：1 个。
- 待处理：34 个。
