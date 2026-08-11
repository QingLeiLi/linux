# kernel/irq 学习注释任务清单

## 使用规则

本清单是 `kernel/irq` 学习注释长任务的持久化进度源。发生会话中断或上下文丢失时，先读取
本文件，再检查当前工作树；不得仅凭对话记忆推断进度。

采用严格的单文件闭环：

1. 开始文件前，将该文件从 `[ ]` 改为 `[~]`，并确认没有来源不明的重叠改动。
2. 完整读取原文件，按 `doc/linux-kernel-source-learning-methodology.md` 仅添加学习注释。
4. 完成下列文件级验收后，才可把 `[~]` 改为 `[x]`：
   - 按方法论第 17 章通过验收
5. 一个 `[~]` 文件未完成前，不开始下一个文件。完成后立即更新本清单的状态和验收记录。

状态含义：`[ ]` 尚未通过 skill 第 17 章文件级闭环（所在章节区分“已补注待复验”与“未开始”）；
`[~]` 正在处理或复验；`[x]` 已按第 17 章完成独立内容验收与修改安全检查。

## 已完成注释、待 skill 第 17 章复验

- [x] `kernel/irq/settings.h`
- [x] `kernel/irq/internals.h`
- [x] `kernel/irq/irqdesc.c`
- [x] `kernel/irq/handle.c`
- [x] `kernel/irq/chip.c`
- [x] `kernel/irq/manage.c`
- [x] `kernel/irq/spurious.c`
- [x] `kernel/irq/resend.c`
- [x] `kernel/irq/migration.c`
- [x] `kernel/irq/affinity.c`
- [x] `kernel/irq/generic-chip.c`

## 待处理文件

- [x] `kernel/irq/irqdomain.c`
- [x] `kernel/irq/msi.c`
- [x] `kernel/irq/cpuhotplug.c`
- [x] `kernel/irq/pm.c`
- [x] `kernel/irq/kexec.c`
- [x] `kernel/irq/ipi.c`
- [x] `kernel/irq/ipi-mux.c`
- [x] `kernel/irq/proc.c`
- [x] `kernel/irq/debugfs.c`
- [x] `kernel/irq/devres.c`
- [x] `kernel/irq/autoprobe.c`
- [x] `kernel/irq/matrix.c`
- [x] `kernel/irq/irq_sim.c`
- [x] `kernel/irq/irq_test.c`
- [x] `kernel/irq/dummychip.c`
- [x] `kernel/irq/debug.h`
- [x] `kernel/irq/debugfs.h`
- [x] `kernel/irq/proc.h`
- [x] `kernel/irq/Kconfig`
- [x] `kernel/irq/Makefile`

## 下一步

11 个既有文件以及 `irqdomain.c`、`msi.c`、`cpuhotplug.c`、`pm.c`、`kexec.c`、`ipi.c`、
`ipi-mux.c`、`proc.c`、`debugfs.c`、`devres.c`、`autoprobe.c`、`matrix.c`、`irq_sim.c`、
清单内全部 31 个文件均已通过第 17 章单文件验收和目录级总审计；仅注释改动、diff hygiene、
关联读取披露与验收记录完整性均已复核。本长任务已完成，后续若扩展范围需另建 TODO。

## 第 17 章验收记录

### 2026-08-11：`kernel/irq/manage.c`

- 函数与实体：按源码顺序核对 94 个函数定义（含条件编译变体）、全局/静态状态、参数和重要
  局部变量；函数头归属、声明签名、返回类别、上下文与 ownership 均通过。对函数体阶段偏疏的
  affinity、trigger、IRQ thread、request/free、per-CPU 与 irqchip state 路径追加走读注释。
- 英文注释：许可证和版权按规则豁免；其余英文块/行尾注释均保留原文，并有紧邻中文翻译与
  学习补充，自动扫描未发现漏配。
- 路径与并发：复核 immediate/pending affinity、oneshot 唤醒与收尾、action 发布/摘除、
  request 失败回滚、NMI/per-CPU 生命周期，以及 request_mutex -> chip bus lock -> desc lock
  锁序和 threads_active/thread_mask 的不同职责。
- 复杂函数抽查：`__setup_irq()`、`__free_irq()`、`irq_finalize_oneshot()` 均能仅沿注释复述
  入口状态、发布点、失败/释放路径、竞态双方和返回保证。
- 关联读取：`kernel/irq/handle.c::__irq_wake_thread()`、`kernel/irq/chip.c::{irq_activate,
  irq_shutdown_and_deactivate,irq_percpu_disable,handle_percpu_devid_irq}`、`kernel/irq/internals.h`
  的 pending-affinity helpers 已有充分学习注释；`kernel/irq/pm.c::{irq_pm_install_action,
  irq_pm_remove_action}` 缺少学习注释，保留在本清单后续 `pm.c` 任务中处理。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`，新增非注释行 0，原有行删除/改写 0；
  `git diff --check` 通过，checkpatch 为 0 error、0 warning。仓库缺少 `.config`，未执行目标编译。

### 2026-08-11：`kernel/irq/spurious.c`

- 函数与实体：按源码顺序核对 11 个函数、`irqfixup`/`noirqdebug` 模式变量、全局轮询定时器、
  `irq_poll_cpu` 和 `irq_poll_active`；函数头归属、参数、返回语义与局部变量说明均通过。
- 英文注释：保留全部原文；为 `Already tried` 行尾说明补齐紧邻中文对应，自动扫描无漏配。
- 路径与并发：复核误路由全局互斥扫描、POLL_INPROGRESS/PENDING 协议、定时恢复、线程结果
  延迟一轮结算、100000 次检测窗口，以及 SPURIOUS_DISABLED 的 disable-depth 生命周期。
- 复杂函数抽查：`try_one_irq()`、`misrouted_irq()`、`note_interrupt()` 能仅沿注释复述正常、
  误路由、延迟线程结果、诊断、禁用与恢复路径，并解释 desc 锁和原子扫描门禁不可删除的原因。
- 关联读取：`kernel/irq/handle.c::{handle_irq_event,handle_irq_event_percpu}` 与
  `kernel/irq/chip.c` 的 flow-handler 调用点已有充分学习注释；`kernel/irq/manage.c::__enable_irq()`
  已在上一项完成，用于核对安装新共享 handler 后撤销 spurious disable depth 的恢复语义。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`，仅新增 1 行中文注释，原有行删除/改写 0；
  `git diff --check` 通过，合并源码补丁 checkpatch 为 0 error、0 warning。仓库缺少 `.config`，
  未执行目标编译。

### 2026-08-11：`kernel/irq/resend.c`

- 函数清单：逐项核对 10 个条件编译函数定义；函数头/声明/函数体结束行分别为
  `resend_irqs` 40-50/51/80、软件配置打开时 `irq_sw_resend` 86-100/101/142、
  `clear_irq_resend` 144-154/155/159、`irq_resend_init` 161-171/172/175，配置关闭时
  `clear_irq_resend` 178-184/185/185、`irq_resend_init` 186-192/193/193、`irq_sw_resend`
  195-204/205/208，以及 `try_retrigger` 211-223/224/234、`check_irq_resend`
  236-250/251/298、`irq_inject_interrupt` 301-332/333/359（导出宏 360）；职责、上下文、
  参数、返回类别、ownership 与条件配置均通过。
- 实体与英文注释：核对 `irq_resend_list`、`irq_resend_lock`、`resend_tasklet` 及全部参数和
  重要局部变量；许可证/版权按规则豁免，其余英文块、单行和行尾注释均保留原文并有紧邻中文
  翻译与学习补充。为两个配置关闭 stub、scope guard 和结果变量补齐专属说明。
- 路径与并发：复核硬件 retrigger -> tasklet 软件退化、nested IRQ 转交 parent、PENDING
  领取、REPLAY 去重/flow-handler 清除、level IRQ 快速拒绝、shutdown 摘链，以及
  `irq_resend_lock` 下摘节点后保持本地 IRQ 关闭、锁外调用 flow handler 的锁序和重新入链协议。
- 复杂函数抽查：`resend_irqs()`、`irq_sw_resend()`、`check_irq_resend()` 均能仅沿注释复述
  排队/领取/提交阶段、状态发布点、失败保证、竞态双方，以及为何不能持全局重发锁调用 handler。
- 关联读取：`kernel/irq/chip.c::{irq_startup,irq_shutdown,irq_can_handle_actions,
  handle_fasteoi_irq,irq_chip_retrigger_hierarchy}`、`kernel/irq/internals.h` 的 IRQS 状态和 scoped
  irqdesc guard、`kernel/irq/irqdesc.c::init_desc()` 均有充分学习注释；
  `drivers/irqchip/irq-al-fic.c::al_fic_irq_retrigger()`、`arch/x86/kernel/apic/vector.c::
  apic_retrigger_irq()`、`drivers/irqchip/irq-gic-v3.c::gic_retrigger()` 用于核对 retrigger 的
  0/1 契约，相关局部学习注释缺失，不扩展本 TODO 的 IRQ 核心目录范围。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`，新增 76 行且删除 0 行，原有代码和注释无改写；
  `git diff --check` 通过，checkpatch 为 0 error、30 个 UTF-8 中文注释行宽 warning。仓库缺少
  `.config`，未执行目标编译。

### 2026-08-11：`kernel/irq/migration.c`

- 函数清单：逐项核对 5 个函数；函数头/声明/函数体结束行分别为
  `irq_fixup_move_pending` 17-39/40/63、`irq_force_complete_move` 65-77/78/87、
  `irq_move_masked_irq` 89-108/109/186、`__irq_move_irq` 188-201/202/238、
  `irq_can_move_in_process_context` 240-251/252/261；调用位置、参数、返回类别、锁/屏蔽
  前提、ownership、局部变量与配置差异均通过，文件无自定义全局变量或结构体。
- 英文注释：逐项保留并紧邻覆盖 dying CPU、per-CPU 防御检查、edge/RTE 重编程、-EBUSY
  重试、ONESHOT mask 和 hierarchy 规范化等全部英文注释；未发现原文删除、改写或漏配。
- 路径与并发：复核 CPU hotplug 对 pending 目标的复用/放弃、层级首个 force-complete 回调、
  deferred move 的领取/提交/重试、临时 mask 的精确恢复，以及 pending 位与 pending_mask 内容
  的独立生命周期。追加修正说明：per-CPU、空 mask、缺 affinity 回调的早退不会统一经过末尾
  clear，不能把残留 mask 内容误判为仍置 pending 的可执行请求。
- 复杂函数抽查：`irq_fixup_move_pending()`、`irq_move_masked_irq()`、`__irq_move_irq()` 能仅沿
  注释复述入口状态、领取点、硬件安全窗口、-EBUSY 重试、永久结束与 ONESHOT 竞态后果。
- 关联读取：`kernel/irq/manage.c::{irq_set_affinity_pending,irq_set_affinity_locked,
  irq_do_set_affinity}` 与 `kernel/irq/internals.h` 的 pending helpers/条件配置已有充分注释；
  `kernel/irq/cpuhotplug.c::migrate_one_irq()`、`include/linux/irq.h::irq_move_irq()`、
  `arch/x86/kernel/apic/io_apic.c` 的 ack 后迁移窗口，以及
  `drivers/irqchip/irq-riscv-imsic-{platform,state}.c` 的查询调用点均缺少或仅部分覆盖学习注释；
  `cpuhotplug.c` 保留在本 TODO 后续项，其余关联文件不扩展本目录任务范围。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`，新增 48 行且删除 0 行，原有代码和注释无改写；
  `git diff --check` 通过，checkpatch 为 0 error、24 个 UTF-8 中文注释行宽 warning。仓库缺少
  `.config`，未执行目标编译。

### 2026-08-11：`kernel/irq/affinity.c`

- 函数清单：逐项核对 3 个函数；函数头/声明/函数体结束行分别为 `default_calc_sets`
  21-33/34/38、`irq_create_affinity_masks` 40-68/69-70/165、
  `irq_calc_affinity_vectors` 167-185/186-187/202；参数单位、输入输出属性、calc_sets 回调、
  数组 ownership、NULL/数值返回类别、可睡眠分配上下文与局部变量均通过，文件无自定义全局
  变量或结构体定义。
- 英文注释：许可证/版权按规则豁免；其余 kernel-doc 和阶段注释均逐项保留并有紧邻中文翻译与
  学习补充。追加修正说明：`irq_create_affinity_masks()` 的英文 kernel-doc 将 NULL 仅归因为
  分配失败并不完整，当前源码还在 `nr_sets` 越界和 affinity 中段为 0 时返回 NULL。
- 路径与生命周期：复核 pre/mid/post 三段向量、默认/驱动集合计算、present/possible CPU
  分摊、临时 result 释放、失败回滚、默认 mask 回填、managed 标记，以及返回数组由调用者
  `kfree` 的所有权转移。无共享状态锁协议；CPU 集合是调用时快照，函数明确不持 hotplug 锁。
- 函数抽查：覆盖文件全部 3 个函数；`irq_create_affinity_masks()` 可仅沿注释复述容量退化、
  回调发布、三类 NULL、分配/逐集合复制/回滚、尾部回填和 managed 标记；两个短直线函数也能
  复述输入输出状态，并解释默认集合为什么仍处理 0 向量、规模计算为何不等于实际 IRQ 发布。
- 关联读取：`include/linux/interrupt.h` 的 `irq_affinity`/`irq_affinity_desc` 定义及 SMP stub、
  `drivers/pci/msi/msi.c` 和 `drivers/base/platform.c` 的创建/规模计算调用与释放路径用于核对返回
  ownership；这些关联区域均缺少学习注释，且不在本 IRQ 核心目录 TODO 的修改授权范围内。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`，新增 19 行且删除 0 行，原有代码和注释无改写；
  `git diff --check` 通过，checkpatch 为 0 error、11 个 UTF-8 中文注释行宽 warning。仓库缺少
  `.config`，未执行目标编译。

### 2026-08-11：`kernel/irq/generic-chip.c`

- 函数清单（函数头/声明/函数体结束）：按源码顺序核对 30 个函数：`irq_gc_noop`
  33-44/45/47、`irq_gc_mask_disable_reg` 50-65/66/75、`irq_gc_mask_set_bit`
  78-93/94/103、`irq_gc_mask_clr_bit` 106-120/121/130、`irq_gc_unmask_enable_reg`
  133-148/149/158、`irq_gc_ack_set_bit` 161-172/173/181、`irq_gc_ack_clr_bit`
  184-196/197/205、`irq_gc_mask_disable_and_ack_set` 207-228/229/239、`irq_gc_eoi`
  242-253/254/262、`irq_gc_set_wake` 264-282/283/297、`irq_readl_be` 300-308/309/312、
  `irq_writel_be` 314-322/323/326、`irq_init_generic_chip` 328-344/345-347/359、
  `irq_alloc_generic_chip` 361-382/383-385/395、`irq_gc_init_mask_cache`
  398-410/411-412/427。
- 函数清单（续）：`irq_domain_alloc_generic_chips` 429-454/455-456/546、
  `irq_domain_remove_generic_chips` 549-564/565/583、`__irq_alloc_domain_generic_chips`
  586-607/608-612/625、`__irq_get_domain_generic_chip` 628-639/640-641/652、
  `irq_get_domain_generic_chip` 654-666/667-668/673、`irq_map_generic_chip`
  688-703/704-705/758、`irq_unmap_generic_chip` 760-771/772/792、
  `irq_setup_generic_chip` 805-831/832-834/870、`irq_setup_alt_chip`
  873-890/891/905、`irq_remove_generic_chip` 908-931/932-933/970、
  `irq_gc_get_irq_data` 973-985/986/1003、`irq_gc_suspend` 1006-1017/1018/1039、
  `irq_gc_resume` 1041-1051/1052/1072、`irq_gc_shutdown` 1079-1089/1090/1105、
  `irq_gc_init_ops` 1119-1128/1129/1133（initcall 1135）；函数名、参数、返回、调用上下文、
  ownership、配置分支和导出宏归属均通过。
- 实体与英文注释：核对 `gc_list`/`gc_lock`、两个 nested lock class、
  `irq_generic_chip_ops`、`irq_gc_syscore_ops`、`irq_gc_syscore` 的角色、生命周期和同步；许可证/
  版权按规则豁免，其余 kernel-doc、块注释和行注释全部保留原文，并有紧邻完整翻译与学习补充。
- 路径与生命周期：复核传统 alloc -> type 配置 -> setup -> remove -> free、domain 连续块分配 ->
  per-chip init/入 PM 链 -> map/unmap -> exit/remove/free、mask cache 共享/私有与大小端访问、
  installed/unused 位图、type0/备选 flow 切换、失败时仅逆序撤销已提交 chip，以及 syscore
  suspend/resume 的单 CPU/关中断约束和关机遍历。
- 复杂函数抽查：`irq_domain_alloc_generic_chips()`、`irq_map_generic_chip()`、
  `irq_setup_generic_chip()`、`irq_remove_generic_chip()` 均能仅沿注释复述入口 ownership、阶段、
  发布点、硬件/缓存副作用、失败回滚与并发前提；特别区分 `gc_lock` 的全局链职责与 `gc->lock`
  的寄存器/缓存职责，禁止把未知回调放在全局 raw 锁内。
- 关联读取：`include/linux/irq.h` 的 `irq_chip_type`、`irq_chip_generic`、domain generic 容器、
  flags、访问器和释放 helper，以及 `include/linux/syscore_ops.h`、`drivers/base/syscore.c`、
  `kernel/power/suspend.c` 的 syscore 调用上下文用于核对字段 ownership、释放和 PM 不可睡眠约束；
  这些关联区域缺少学习注释且不在当前 `kernel/irq` 文件修改授权范围内。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`，新增 168 行且删除 0 行，原有代码和注释无改写；
  `git diff --check` 通过，checkpatch 为 0 error、84 个 UTF-8 中文注释行宽 warning。仓库缺少
  `.config`，未执行目标编译。

## `kernel/irq/irqdomain.c` 验收记录（2026-08-11）

- 已按主方法论第 17 章完成修改后整文件强制验收；当前文件 2826 行，按源码顺序复核 89 个
  条件编译后函数定义、7 个文件级实体以及全部非豁免英文注释，函数头均紧邻对应声明，未落在
  前一函数结尾、导出宏或错误配置分支。
- 函数清单（函数头/声明/函数体结束，1）：`debugfs_add_domain_dir` 75-75/76/76、
  `debugfs_remove_domain_dir` 77-77/78/78、`irqchip_fwnode_get_name` 81-84/85/90、
  `irqchip_fwnode_get_parent` 92-95/96/101、`__irq_domain_alloc_fwnode` 127-143/144/178、
  `irq_domain_free_fwnode` 187-194/195/205、`alloc_name` 208-214/215/226、
  `alloc_fwnode_name` 228-235/236/262、`alloc_unknown_name` 264-268/269/283、
  `irq_domain_set_name` 285-297/298/342、`__irq_domain_create` 344-355/356/413、
  `__irq_domain_publish` 415-420/421/429、`irq_domain_free` 431-436/437/444、
  `irq_domain_instantiate_descs` 446-451/452/462、`__irq_domain_instantiate`
  464-474/475/536、`irq_domain_instantiate` 544-548/549/552、`irq_domain_remove`
  563-572/573/599、`irq_domain_update_bus_token` 602-609/610/639。
- 函数清单（2）：`irq_domain_create_simple` 660-668/669/686、`irq_domain_create_legacy`
  689-694/695/714、`irq_find_matching_fwspec` 722-731/732/772、`irq_set_default_domain`
  784-789/790/795、`irq_get_default_domain` 807-811/812/815、`irq_domain_is_nomap`
  818-822/823/827、`irq_domain_clear_mapping` 829-834/835/847、`irq_domain_set_mapping`
  849-853/854/875、`irq_domain_disassociate` 877-885/886/924、
  `irq_domain_associate_locked` 926-933/934/978、`irq_domain_associate` 980-984/985/995、
  `irq_domain_associate_many` 998-1002/1003/1015、`irq_create_direct_mapping`
  1029-1036/1037/1065、`irq_create_mapping_affinity_locked` 1069-1073/1074/1101、
  `irq_create_mapping_affinity` 1114-1122/1123/1153、`irq_domain_translate`
  1156-1161/1162/1179、`of_phandle_args_to_fwspec` 1181-1185/1186/1196、
  `fwspec_to_domain` 1199-1203/1204/1217、`irq_populate_fwspec_info` 1220-1224/1225/1235。
- 函数清单（3）：`irq_create_fwspec_mapping` 1238-1249/1250/1349、
  `irq_create_of_mapping` 1352-1356/1357/1365、`irq_dispose_mapping` 1372-1377/1378/1397、
  `__irq_resolve_mapping` 1408-1416/1417/1459、`irq_domain_xlate_onecell`
  1474-1477/1478/1487、`irq_domain_xlate_twocell` 1503-1507/1508/1516、
  `irq_domain_xlate_twothreecell` 1532-1536/1537/1546、`irq_domain_xlate_onetwocell`
  1566-1571/1572/1585、`irq_domain_translate_onecell` 1602-1605/1606/1616、
  `irq_domain_translate_twocell` 1631-1635/1636/1646、`irq_domain_translate_twothreecell`
  1661-1665/1666/1682、`irq_domain_alloc_descs` 1685-1690/1691/1712、
  `irq_domain_reset_irq_data` 1718-1722/1723/1728、`irq_domain_insert_irq`
  1732-1737/1738/1750、`irq_domain_remove_irq` 1752-1757/1758/1774、
  `irq_domain_insert_irq_data` 1776-1781/1782/1797、`__irq_domain_free_hierarchy`
  1799-1803/1804/1813、`irq_domain_free_irq_data` 1815-1819/1820/1833。
- 函数清单（4）：`irq_domain_disconnect_hierarchy` 1848-1853/1854/1865、
  `irq_domain_trim_hierarchy` 1868-1877/1878/1932、`irq_domain_alloc_irq_data`
  1934-1939/1940/1963、`irq_domain_get_irq_data` 1970-1974/1975/1986、
  `irq_domain_set_hwirq_and_chip` 1997-2002/2003/2019、`irq_domain_set_info`
  2033-2038/2039/2047、`irq_domain_free_irqs_common` 2056-2060/2061/2073、
  `irq_domain_free_irqs_top` 2082-2087/2088/2098、`irq_domain_free_irqs_hierarchy`
  2101-2105/2106/2119、`irq_domain_alloc_irqs_hierarchy` 2121-2125/2126/2135、
  `irq_domain_alloc_irqs_locked` 2137-2147/2148/2192、`__irq_domain_alloc_irqs`
  2216-2226/2227/2245、`irq_domain_fix_revmap` 2249-2253/2254/2274、
  `irq_domain_push_irq` 2287-2300/2301/2383、`irq_domain_pop_irq` 2394-2402/2403/2467。
- 函数清单（5）：`irq_domain_free_irqs` 2475-2480/2481/2501、`irq_domain_free_one_irq`
  2504-2508/2509/2515、`irq_domain_alloc_irqs_parent` 2524-2528/2529/2538、
  `irq_domain_free_irqs_parent` 2547-2551/2552/2559、`__irq_domain_deactivate_irq`
  2562-2566/2567/2577、`__irq_domain_activate_irq` 2579-2584/2585/2604、
  `irq_domain_activate_irq` 2615-2620/2621/2630、`irq_domain_deactivate_irq`
  2640-2644/2645/2651、`irq_domain_check_hierarchy` 2653-2657/2658/2664、
  `irq_domain_debug_show_one` 2745-2749/2750/2765、`irq_domain_debug_show`
  2767-2771/2772/2785、`irq_domain_debugfs_init` 2807-2812/2813/2825。
- 配置替代定义：非层级分支 `irq_domain_get_irq_data` 2670-2673/2674/2680、
  `irq_domain_set_info` 2694-2698/2699/2707、`irq_domain_alloc_irqs_locked` 2709/2710/2715、
  `irq_domain_check_hierarchy` 2717/2718/2718、`irq_domain_free_one_irq` 2719/2720/2720；
  debugfs 实现分支 `debugfs_add_domain_dir` 2787-2791/2792/2798、
  `debugfs_remove_domain_dir` 2800/2801/2804。函数名、参数、返回、调用上下文、ownership、
  配置边界和导出宏归属均通过。
- 实体与英文注释：核对 `irq_domain_list`、`irq_domain_mutex`、`irq_default_domain`、
  `irqchip_fwid`、`irqchip_fwnode_ops`、`irq_domain_simple_ops`、`domain_dir`、
  `irqdomain_flags` 的角色、借用/拥有关系和同步；许可证按规则豁免，其余 kernel-doc、块注释和
  行注释逐字保留，并有紧邻完整中文翻译与学习补充。
- 路径与生命周期：复核 create -> generic chip/init -> publish -> 可选预关联 -> remove，普通
  associate/disassociate，线性 RCU/radix/nomap 三类反向映射，fwspec 查找/翻译/触发类型冲突，
  层级 desc -> irq_data 链 -> 驱动 alloc -> trim -> revmap 发布，以及 deactivate/free/父链/desc
  逆序释放；另核对 push/pop 移动内嵌 irq_data 后修正 revmap、激活内到外与停用外到内的顺序。
- 复杂函数只读抽查：`__irq_domain_instantiate()` 可仅沿注释复述未发布对象的资源取得、发布边界和
  init 失败逆序回滚；`irq_create_fwspec_mapping()` 可复述 domain 选择、触发类型一致性、MSI 解锁
  分配和唯一映射提交；`irq_domain_alloc_irqs_locked()` 可复述 desc/父链/硬件资源/trim/revmap 五
  阶段及失败责任；`irq_domain_push_irq()`/`irq_domain_pop_irq()` 可推理移动对象后为何必须修复旧
  revmap、为何 action 检查不能替代生命周期排他。第 17.3/17.4 的入口 ownership、发布点、竞态、
  cleanup label、配置分支和后续动作均可由注释回答。
- 关联读取：`include/linux/irqdomain.h` 的 `irq_domain_ops`、`irq_domain`、`irq_domain_info`、
  `irq_find_mapping()` 与层级 API（现有学习注释部分覆盖），用于核对回调、字段和 RCU 查询契约；
  `include/linux/irq.h` 的 `irq_data`（部分覆盖）用于核对内嵌/父链字段所有权；
  `include/linux/irqdesc.h` 的 `irq_desc`（部分覆盖）及 `kernel/irq/irqdesc.c` 的 `free_desc()`、
  `irq_desc_free_rcu()`、`irq_free_descs()`（学习注释充分）用于核对 desc 摘除后的 RCU 延迟释放；
  `kernel/irq/msi.c` 的激活调用区和 `kernel/irq/manage.c` 的 managed affinity 更新区（部分覆盖）
  用于核对 reserve 两阶段激活及 deactivate/restore 配对。关联文件不在本文件修改授权范围内。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`，新增 664 行且删除 0 行，原有代码和注释无改写；
  `git diff --check` 通过，checkpatch 为 0 error、139 个仅由 UTF-8 中文注释显示宽度触发的行宽
  warning。仓库缺少 `.config`（`NO_CONFIG`），未执行目标编译。

## `kernel/irq/msi.c` 验收记录（2026-08-11）

- 已按主方法论第 17 章完成修改后整文件强制验收；当前文件 2346 行，按源码顺序复核 77 个
  条件编译函数定义以及 10 个文件级实体。专属中文函数头均直接位于对应声明之前；英文
  kernel-doc 与中文函数头严格分块，未落在前一函数尾部、导出宏或错误配置分支。
- 函数清单（函数头/声明/函数体结束，1）：`msi_alloc_desc` 102-107/108/128、
  `msi_free_desc` 130-134/135/139、`msi_insert_desc` 141-147/148/188、
  `msi_domain_insert_msi_desc` 200-205/206/222、`msi_desc_match` 224-228/229/241、
  `msi_ctrl_valid` 243-248/249/264、`msi_domain_free_descs` 266-271/272/293、
  `msi_domain_free_msi_descs_range` 302-306/307/317、`msi_domain_add_simple_msi_descs`
  326-331/332/360、`__get_cached_msi_msg` 362-366/367/370、`get_cached_msi_msg`
  372-376/377/382、`msi_device_data_release` 385-390/391/402、`msi_setup_device_data`
  414-420/421/459、`__msi_lock_descs` 467-471/472/475、`__msi_unlock_descs`
  484-488/489/495、`msi_find_desc` 498-502/503/515、`msi_domain_first_desc`
  529-534/535/547、`msi_next_desc` 564-568/569/584、`msi_domain_get_virq`
  595-600/601/637。
- 函数清单（2）：SYSFS 打开分支 `msi_sysfs_create_group` 652-655/656/659、
  `msi_mode_show` 661-665/666/674、`msi_sysfs_remove_desc` 676-681/682/697、
  `msi_sysfs_populate_desc` 699-704/705/740、`msi_device_populate_sysfs`
  747-751/752/765、`msi_device_destroy_sysfs` 772-776/777/783；SYSFS 关闭替代定义
  `msi_sysfs_create_group` 786/787/787、`msi_sysfs_populate_desc` 788/789/789、
  `msi_sysfs_remove_desc` 790/791/791。其后为 `msi_get_device_domain` 794-798/799/816、
  `msi_domain_get_hwsize` 818-822/823/836、`irq_chip_write_msi_msg` 838-842/843/847、
  `msi_check_level` 849-853/854/866、`msi_domain_set_affinity` 879-884/885/900、
  `msi_domain_activate` 902-907/908/917、`msi_domain_deactivate` 919-923/924/931、
  `msi_domain_alloc` 933-941/942/975、`msi_domain_free` 977-981/982/993、
  `msi_domain_translate` 995-1000/1001/1014、`msi_domain_debug_show` 1017-1020/1021/1032。
- 函数清单（3）：`msi_domain_ops_get_hwirq` 1047/1048/1052、`msi_domain_ops_prepare`
  1054-1057/1058/1063、`msi_domain_ops_teardown` 1065/1066/1068、
  `msi_domain_ops_set_desc` 1070/1071/1075、`msi_domain_ops_init` 1077-1082/1083/1096、
  `msi_domain_update_dom_ops` 1107-1112/1113/1137、`msi_domain_update_chip_ops`
  1139-1143/1144/1151、`__msi_create_irq_domain` 1153-1161/1162/1196、
  `msi_create_irq_domain` 1206-1210/1211/1216、`msi_create_parent_irq_domain`
  1225-1229/1230/1246、`msi_parent_init_dev_msi_info` 1276-1286/1287/1299、
  `msi_create_device_irq_domain` 1348-1368/1369/1456、`msi_remove_device_irq_domain`
  1463-1474/1475/1496、`msi_match_device_irq_domain` 1506-1510/1511/1524、
  `msi_domain_prepare_irqs` 1526-1530/1531/1538、`msi_check_reservation_mode`
  1551-1560/1561/1590、`msi_handle_pci_fail` 1592-1597/1598/1621、`msi_init_virq`
  1628-1637/1638/1680、`populate_alloc_info` 1682-1686/1687/1707。
- 函数清单（4）：`__msi_domain_alloc_irqs` 1709-1726/1727/1795、
  `msi_domain_alloc_simple_msi_descs` 1797-1801/1802/1810、`__msi_domain_alloc_locked`
  1812-1817/1818/1845、`msi_domain_alloc_locked` 1847-1851/1852/1859、
  `msi_domain_alloc_irqs_range_locked` 1875-1879/1880/1891、
  `msi_domain_alloc_irqs_range` 1903-1906/1907/1913、`msi_domain_alloc_irqs_all_locked`
  1930-1934/1935/1945、`__msi_domain_alloc_irq_at` 1947-1955/1956/2002、
  `msi_domain_alloc_irq_at` 2029-2040/2041/2047、`msi_device_domain_alloc_wired`
  2070-2079/2080/2099、`__msi_domain_free_irqs` 2101-2110/2111/2140、
  `msi_domain_free_locked` 2142-2149/2150/2175、`msi_domain_free_irqs_range_locked`
  2186-2190/2191/2200、`msi_domain_free_irqs_range` 2211-2215/2216/2221、
  `msi_domain_free_irqs_all_locked` 2235-2240/2241/2245、`msi_domain_free_irqs_all`
  2254-2257/2258/2262、`msi_device_domain_free_wired` 2272-2277/2278/2291、
  `msi_get_domain_info` 2299-2303/2304/2307、`msi_device_has_isolated_msi`
  2325-2337/2338/2346。
- 实体与英文注释：核对 `msi_device_data`、`msi_ctrl`、两个 XArray 容量宏、SYSFS 属性数组/
  属性组、`msi_domain_ops`、`msi_domain_ops_default` 和两个 VIRQ 状态宏的角色、配置归属、
  ownership 与同步；许可证/版权及三个条件编译尾注按规则豁免，其余 61 个英文块/行注释均
  逐字保留并有紧邻中文翻译，自动结构扫描为 `PAIRED=61 EXEMPT=4 UNPAIRED=0`。
- 路径与生命周期：复核设备 devres 容器、domid/XArray 描述符、virq/irq_data 三层 ownership；
  descriptor 插入 -> irqdomain 分配 -> early/reservation 激活 -> SYSFS 发布及其逆序回滚；普通
  PCI multi-MSI、每项 MSI-X、核心创建简单描述符和 provider 整域覆盖四种分配模型；释放时先
  deactivate、再释放 irqdomain、撤销 SYSFS、清 desc->irq，最后按标志擦除描述符。设备 MSI
  mutex 同时保护 domain 槽、XArray、关联状态和共享迭代游标；root domain mutex 与设备锁的
  wired-to-MSI 交接依据 `irq_create_fwspec_mapping()` 的解锁调用点核对。
- 复杂函数只读抽查：`msi_create_device_irq_domain()` 可仅沿注释复述模板深拷贝、内部指针重定向、
  fwnode/bundle 的 `__free` 临时所有权、domain 槽发布、prepare 失败回滚和最终 ownership 转移；
  `__msi_domain_alloc_irqs()` 可复述 prepare、early/reservation 决策、逐描述符 irqdomain 分配、
  irq_data 绑定、SYSFS 发布和外层统一回滚；`__msi_domain_free_irqs()` 可复述逐向量停用、整段
  virq 释放、SYSFS 撤销、关联清零及描述符保留/销毁分界。三项的入口状态、发布点、竞态双方、
  错误返回和 cleanup 责任均可由注释回答。
- 关联读取：`include/linux/msi.h` 的 `msi_desc`、`msi_dev_domain`、`msi_domain_ops`、
  `msi_domain_info`、`msi_domain_template` 与 flags 用于核对字段 ownership 和 provider 契约；
  `kernel/irq/irqdomain.c::{irq_domain_free_fwnode,__irq_domain_create,
  irq_create_fwspec_mapping}` 用于核对 fwnode 引用/释放和 wired-to-MSI 锁交接；
  `drivers/pci/msi/irqdomain.c` 的 PCI/MSI[-X] 模板、批量 setup/teardown，
  `drivers/pci/msi/api.c::{pci_msix_alloc_irq_at,pci_msix_free_irq}` 及
  `drivers/irqchip/irq-mbigen.c` 的 wired template/cookie 恢复用于核对真实调用与返回契约。
  关联区域不在本文件修改授权范围内；其中真实设备 fwnode 传给只接受 irqchip fwnode 的释放
  helper 会触发现有告警，学习注释按当前代码如实说明，未越权修改实现。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`，新增 570 行且删除 0 行，原有代码和注释无改写；
  `git diff --check` 通过，checkpatch 为 0 error、145 个仅由 UTF-8 中文注释显示宽度触发的行宽
  warning。仓库缺少 `.config`（`NO_CONFIG`），未执行目标编译。

## `kernel/irq/cpuhotplug.c` 验收记录（2026-08-11）

- 函数清单（函数头/声明/函数体结束）：`irq_needs_fixup` 34-41/42/82、
  `migrate_one_irq` 84-99/100/219、`irq_migrate_all_off_this_cpu` 231-240/241/263、
  `hk_should_isolate` 265-271/272/284、`irq_restore_affinity_of_irq` 286-295/296/322、
  `irq_affinity_online_cpu` 328-334/335/351。六个函数的专属中文函数头均紧邻对应声明；参数、
  返回类别、调用上下文、锁前提、局部变量和条件编译语义均通过。文件无自定义全局状态或结构体。
- 英文注释：许可证/版权按规则豁免，其余 16 个英文 kernel-doc、块注释和单行注释均逐字保留，
  且有紧邻中文翻译与学习补充；自动扫描为 `PAIRED=16 EXEMPT=1 UNPAIRED=0`。
- 路径与并发：复核 dying CPU 已移出 online mask 后的 effective/general affinity 选择、旧 move
  cleanup、pending mask 复用、进程上下文不可安全移动时的临时 mask、普通 IRQ 打破 affinity 与
  `-ENOSPC` 二次兜底、managed IRQ 保留 affinity 后 shutdown/deactivate，以及 CPU 上线后的
  managed depth 恢复和 housekeeping 单目标再隔离。下线逐项持 `desc->lock`，上线按
  `sparse_irq_lock -> desc->lock + local IRQ off` 锁序，通知 work 在 desc 锁内取得引用、日志在锁外。
- 复杂函数只读抽查：`migrate_one_irq()` 可仅沿注释复述 chip 消失、无需迁移、pending move、
  managed shutdown、普通兜底、两次 set_affinity、错误和 unmask 全路径；
  `irq_migrate_all_off_this_cpu()` 可解释为何 chained IRQ 不能按 action 过滤、何时通知以及锁内外
  分界；`irq_restore_affinity_of_irq()` 可解释 managed/action/chip/原 affinity 四项门禁、
  shutdown depth 配对及单目标 IRQ 只在 housekeeping 要求下重编程。
- 关联读取：`kernel/irq/migration.c::{irq_force_complete_move,irq_fixup_move_pending}`、
  `kernel/irq/chip.c::{__irq_startup_managed,irq_startup_managed,irq_shutdown_and_deactivate}`、
  `kernel/irq/manage.c::{irq_do_set_affinity,irq_set_affinity_locked,
  irq_affinity_schedule_notify_work}` 已有充分学习注释；`kernel/irq/irqdesc.c::{irq_to_desc,
  irq_lock_sparse,irq_unlock_sparse}` 与 `include/linux/irqnr.h::for_each_active_irq` 用于核对拓扑寿命；
  `kernel/cpu.c` 的 `CPUHP_AP_IRQ_AFFINITY_ONLINE` 注册和 `arch/x86/kernel/irq.c::fixup_irqs()`
  用于核对上下线调用时序。后两个关联区域缺少本任务学习注释且不在当前文件修改授权范围内。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`，新增 104 行且删除 0 行，原有代码和注释无改写；
  `git diff --check` 通过，checkpatch 为 0 error、32 个仅由 UTF-8 中文注释显示宽度触发的行宽
  warning。仓库缺少 `.config`（`NO_CONFIG`），未执行目标编译。

## `kernel/irq/pm.c` 验收记录（2026-08-11）

- 函数清单（函数头/声明/函数体结束）：`irq_pm_handle_wakeup` 27-33/34/41、
  `irq_pm_install_action` 48-54/55/72、`irq_pm_remove_action` 79-84/85/96、
  `suspend_device_irq` 98-108/109/160、`suspend_device_irqs` 178-187/188/206、
  `resume_irq` 208-217/218/252、`resume_irqs` 254-260/261/278、`rearm_wake_irq`
  284-290/291/304、`irq_pm_syscore_resume` 312-315/316/319、`irq_pm_init_ops`
  331-335/336/340、`resume_device_irqs` 351-355/356/359；函数头归属、参数、返回类别、
  desc 锁/本地 IRQ 上下文和 scope guard 均通过。
- 实体与英文注释：核对静态 `irq_pm_syscore_ops` 与 `irq_pm_syscore` 的长期借用关系、注册时机和
  resume 回调；许可证/版权按规则豁免，其余 12 个英文 kernel-doc、块注释和单行注释均逐字保留，
  且有紧邻中文翻译与学习补充，自动扫描为 `PAIRED=12 EXEMPT=1 UNPAIRED=0`。
- 路径与并发：复核 action 安装/删除后的 FORCE_RESUME、NO_SUSPEND、COND_SUSPEND 聚合计数；
  suspend 对普通 IRQ 设置 SUSPENDED/disable/mask，对 wake IRQ 设置 ARMED 并按 chip 要求临时
  enable；外层在 desc 锁外 synchronize，flow handler 消费 wake 后设置 PENDING、增加 depth、
  disable 并通知 PM；rearm 或 resume 对称撤销。恢复区分 syscore early 与设备常规阶段，恢复
  临时 enabled-on-suspend 原状态，并用人工 disabled/masked 状态实现 FORCE_RESUME。
- 复杂函数只读抽查：`suspend_device_irq()` 可仅沿注释复述三类早退、wake armed、临时 enable、
  普通 suspend、chip mask 和同步返回；`resume_irq()` 可复述 armed 清理、临时 enable 还原、
  SUSPENDED 与 FORCE_RESUME 两个入口及共同 depth 恢复；`rearm_wake_irq()` 可解释 desc/bus 锁、
  状态二次验证、armed 重新发布和 wake-handler 增加 depth 的配对。
- 关联读取：`kernel/irq/chip.c::irq_can_handle_pm()` 用于核对 ARMED 唤醒事件的消费入口；
  `kernel/irq/manage.c::{__enable_irq,irq_pm_install_action 调用点,irq_pm_remove_action 调用点}`、
  `include/linux/interrupt.h` 的 IRQF PM 标志和 `include/linux/irqdesc.h` 的聚合字段用于核对 depth
  与共享线约束；`drivers/base/power/main.c::{dpm_suspend_noirq,dpm_resume_noirq}` 用于核对设备
  noirq 阶段顺序；`drivers/acpi/sleep.c::acpi_s2idle_wake()` 用于核对 SCI 检查后重武装场景；
  syscore 注册/调用顺序沿用 `generic-chip.c` 验收时已读的 `drivers/base/syscore.c`。关联区域不在
  当前文件修改授权范围内。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`，新增 110 行且删除 0 行，原有代码和注释无改写；
  `git diff --check` 通过，checkpatch 为 0 error、28 个仅由 UTF-8 中文注释显示宽度触发的行宽
  warning。仓库缺少 `.config`（`NO_CONFIG`），未执行目标编译。

## `kernel/irq/kexec.c` 验收记录（2026-08-11）

- 函数清单（函数头/声明/函数体结束）：`machine_kexec_mask_interrupts` 17-28/29/63；文件无
  自定义全局状态或结构体。函数头紧邻声明，覆盖静止上下文、描述符/chip 借用、无返回值、
  配置分支与单项失败不阻断全局关停的契约；函数体阶段注释覆盖筛选、ACTIVE、EOI 和 shutdown。
- 英文注释：唯一原英文块完整保留并有紧邻中文翻译，自动扫描为 `PAIRED=1 UNPAIRED=0`；
  SPDX 行按规则保留。只读抽查可由注释复述“未启动跳过 -> 可选清 VM forward ACTIVE ->
  in-progress EOI 退化 -> 无条件 shutdown”，并解释 `check_eoi` 的 0/非 0 含义。
- 关联读取：`kernel/irq/manage.c::irq_set_irqchip_state()` 用于核对从叶到父层寻找回调及错误返回；
  `kernel/irq/Kconfig::GENERIC_IRQ_KEXEC_CLEAR_VM_FORWARD` 用于核对配置目的；
  `arch/{arm,arm64,riscv}/kernel/machine_kexec.c::machine_crash_shutdown()` 用于核对本地 IRQ 已关闭、
  其他 CPU 已停止的调用前提；`kernel/irq/chip.c::irq_shutdown()` 已有充分学习注释。架构调用点
  缺少本任务学习注释且不在当前文件修改授权范围内。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`，新增 27 行且删除 0 行，原有代码和注释无改写；
  `git diff --check` 通过，checkpatch 为 0 error、9 个仅由 UTF-8 中文注释显示宽度触发的行宽
  warning。仓库缺少 `.config`（`NO_CONFIG`），未执行目标编译。

## `kernel/irq/ipi.c` 验收记录（2026-08-11）

- 函数清单（函数头/声明/函数体结束）：`irq_reserve_ipi` 34-47/48/136、`irq_destroy_ipi`
  148-157/158/196、`ipi_get_hwirq` 208-214/215/238、`ipi_send_verify` 241-247/248/274、
  `__ipi_send_single` 287-293/294/327、`__ipi_send_mask` 340-346/347/384、
  `ipi_send_single` 394-399/400/410、`ipi_send_mask` 421-426/427/437。函数头紧邻声明，
  参数/返回、DEBUG 配置、导出宏、对象借用和发送热路径上下文均通过；文件级实体只有 `pr_fmt`。
- 英文注释：许可证/版权按规则豁免，其余 16 个英文 kernel-doc、块注释和 FIXME 均逐字保留并有
  紧邻中文翻译与学习补充，自动扫描为 `PAIRED=16 EXEMPT=1 UNPAIRED=0`。
- 路径与生命周期：复核 single domain 的单 virq/有洞 dest 与 per-CPU domain 的连续 dest/连续
  virq 模型；desc 预分配 -> irqdomain realloc 分配 -> affinity/ipi_offset/NO_BALANCING 发布；
  destroy 的 CPU 到 virq 换算；查询 hwirq；公开入口全量验证与双下划线入口仅 DEBUG 验证；
  provider mask 优先和 single 逐 CPU 退化。发送不持锁，调用者必须与 destroy 串行化。
- 边界审计：`__irq_domain_alloc_irqs()` 失败时已在内层用原正 virq 基址释放预分配 desc，外层
  `free_descs` 标签拿负错误码后的调用只是无效重复清理，不构成泄漏；per-CPU destroy 只验证
  dest 为原 affinity 子集，却按 `first + weight` 释放连续区间，因此调用者还必须隐式保证子集
  连续。学习注释按当前实现记录这两个事实，未越权修改代码。
- 复杂函数只读抽查：`irq_reserve_ipi()` 可复述验证、两种模型、连续性检测、两阶段资源取得、
  发布和失败责任；`irq_destroy_ipi()` 可复述身份/subset 门禁及 single/per-CPU 释放换算；
  `__ipi_send_mask()` 可复述 DEBUG 验证、原生 mask、per-CPU 独立 data 和 single 共享 data 路径。
- 关联读取：`kernel/irq/irqdomain.c::{irq_domain_alloc_descs,__irq_domain_alloc_irqs,
  irq_domain_alloc_irqs_locked}` 与 `kernel/irq/irqdesc.c::irq_free_descs()` 用于核对失败回滚；
  `arch/mips/kernel/smp.c` 的 allocate/free/send 调用用于核对 possible-mask 生命周期；
  `drivers/irqchip/irq-mips-gic.c` 的 IPI_PER_CPU domain 和 `drivers/irqchip/irq-mips-cpu.c` 的
  IPI_SINGLE domain 用于核对两种 provider 模型。这些关联区域不在当前文件修改授权范围内。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`，新增 93 行且删除 0 行，原有代码和注释无改写；
  `git diff --check` 通过，checkpatch 为 0 error、32 个仅由 UTF-8 中文注释显示宽度触发的行宽
  warning。仓库缺少 `.config`（`NO_CONFIG`），未执行目标编译。

## `kernel/irq/ipi-mux.c` 验收记录（2026-08-11）

- 函数清单（函数头/声明/函数体结束）：`ipi_mux_mask` 49-53/54/59、`ipi_mux_unmask`
  61-66/67/85、`ipi_mux_send_mask` 87-96/97/138、`ipi_mux_domain_alloc`
  148-153/154/166、`ipi_mux_process` 177-186/187/212、`ipi_mux_create` 224-236/237/296。
  函数头紧邻声明，参数、返回、hardirq/per-CPU 上下文、原子状态和失败 ownership 均通过。
- 实体与英文注释：核对 `ipi_mux_cpu`、三个单例全局、`ipi_mux_chip`、
  `ipi_mux_domain_ops` 和 `pr_fmt` 的角色、永久生命周期与借用关系；许可证/版权按规则豁免，其余
  9 个英文 kernel-doc/块/行注释均逐字保留并有紧邻中文对应，自动扫描为
  `PAIRED=9 EXEMPT=1 UNPAIRED=0`。
- 路径与并发：复核发送方“release 置 pending -> 屏障 -> 读 enable”和 unmask 方“置 enable ->
  屏障 -> 读 pending”的镜像协议，保证竞态双方至少一方触发父 IPI；旧 pending 抑制重复父通知，
  masked pending 留待 unmask；接收端只领取 enabled 位并在 domain 中逐项分派。per-CPU enable
  只由本 CPU 修改，bits 允许跨 CPU 并发 RMW；成功创建后无 destroy API。
- 复杂函数只读抽查：`ipi_mux_send_mask()` 可解释共享数据 release、屏障、旧 pending 和 enable
  四项决策；`ipi_mux_process()` 可解释为何 enable 可无序读取、fetch_andnot 只领取 enabled 位及
  与发送发布的配对；`ipi_mux_create()` 可复述 per-CPU/fwnode/domain/virq 四阶段取得、单例发布点和
  domain -> fwnode -> per-CPU 逆序失败清理。
- 关联读取：`Documentation/memory-barriers.txt` 与 `include/asm-generic/barrier.h` 的
  `smp_mb__after_atomic()` 说明用于核对补充完整屏障；`kernel/irq/irqdomain.c` 的创建/分配/失败
  回滚和 `kernel/irq/handle.c::handle_percpu_devid_irq()` 已有充分学习注释；
  `drivers/clocksource/timer-clint.c`、`arch/riscv/kernel/sbi-ipi.c`、
  `drivers/irqchip/irq-apple-aic.c` 的 create/process 调用用于核对返回后才发布 virq、父 hardirq
  调用上下文。这些调用点不在当前文件修改授权范围内。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`，新增 90 行且删除 0 行，原有代码和注释无改写；
  `git diff --check` 通过，checkpatch 为 0 error、26 个仅由 UTF-8 中文注释显示宽度触发的行宽
  warning。仓库缺少 `.config`（`NO_CONFIG`），未执行目标编译。

## `kernel/irq/proc.c` 验收记录（2026-08-11）

- 已按主方法论第 17 章完成修改后整文件强制复读；当前文件 899 行，共核对 38 个条件编译函数
  定义。函数头均直接位于对应声明之前，未落到前一函数尾部、`proc_ops` 实体或条件分支之外。
- 函数清单（1，函数头/声明/函数体结束）：`show_irq_affinity` 70-76/77/114、
  `irq_affinity_hint_proc_show` 116-121/122/138、`irq_affinity_proc_show` 142/143/146、
  `irq_affinity_list_proc_show` 148/149/152、非自动分支 `irq_select_affinity_usr`
  155-159/160/173、自动分支同名函数 176-179/180/183、`write_irq_affinity`
  186-194/195/237、`irq_affinity_proc_write` 239/240/244、`irq_affinity_list_proc_write`
  246/247/251、`irq_affinity_proc_open` 253/254/257、`irq_affinity_list_proc_open`
  259/260/263。
- 函数清单（2）：`irq_effective_aff_proc_show` 284/285/288、
  `irq_effective_aff_list_proc_show` 290/291/294、`default_affinity_show` 297/298/302、
  `default_affinity_write` 304-309/310/340、`default_affinity_open` 342/343/346、
  `irq_node_proc_show` 357/358/364、`irq_spurious_proc_show` 367-371/372/380、
  `name_unique` 385-389/390/402、`register_handler_proc` 404-409/410/423、
  `register_irq_proc` 430-438/439/497、`unregister_irq_proc` 499-504/505/525、
  `unregister_handler_proc` 529-532/533/536、`register_default_affinity_proc`
  538-541/542/548、`init_irq_proc` 550-554/555/574、`irq_proc_update_valid`
  576-580/581/589。
- 函数清单（3）：`arch_show_interrupts` 596/597/600、`irq_proc_calc_prec`
  624-628/629/639、`irq_proc_update_chip` 641-645/646/658、
  `irq_proc_emit_zero_counts` 672-675/676/683、`irq_proc_emit_count`
  685-688/689/698、`irq_proc_emit_counts` 700-704/705/712、`irq_seq_show`
  714-724/725/800、`irq_seq_next_desc` 802-810/811/839、`irq_seq_start`
  841-845/846/856、`irq_seq_next` 858-861/862/869、`irq_seq_stop` 871/872/876、
  `irq_proc_init` 886-890/891/896。
- 实体与英文注释：核对 `root_irq_dir`、affinity 类型枚举、`no_irq_affinity`、三组
  `proc_ops`、两个 `MAX_NAMELEN` 语境、架构哨兵、显示约束 raw lock/结构体、零计数字符串宏和
  `irq_seq_ops` 的角色、配置归属与生命周期。许可证/版权按规则豁免，其余 20 个英文块/行注释
  均逐字保留并有紧邻中文翻译与学习补充，审计结果为 `PAIRED=20 EXEMPT=2 UNPAIRED=0`；其中
  Access rules 关于 radix tree/sparse_irq_lock 的旧描述已明确标为历史信息，并补充当前
  Maple Tree + RCU + rcuref 协议。
- 路径与并发：复核配置/pending/effective affinity 的锁内选择，bitmap/list 写入的权限、在线
  CPU 门禁、架构自动选择和核心 setter；handler 名唯一性、首次目录注册互斥、子文件部分创建、
  proc 撤销等待在途 file op 后再释放 desc/action；`/proc/interrupts` 的单调格式快照、连续零
  批量输出、desc 锁内行尾字段，以及 RCU 查 Maple Tree -> 资格位筛选 -> rcuref ->
  show/next/stop 精确归还。spurious 与计数显示明确是并发诊断快照，不误称事务一致。
- 复杂路径只读抽查：`write_irq_affinity()` 可仅沿注释复述权限、两种解析、在线交集、自动选择、
  setter 和所有错误/释放出口；`register_irq_proc()`/`unregister_irq_proc()` 可复述并发首次发布、
  可写权限、部分创建语义以及 proc rundown 与 desc 销毁顺序；`irq_seq_next_desc()` 配合
  `irq_seq_{start,next,stop,show}()` 可复述稀疏查找、无效/死亡对象跳过、引用配对、架构哨兵恰好
  一次及中断读取时的清理。第 17.3/17.4 要求的入口状态、发布点、竞态、错误责任和后续动作均
  可由注释回答。
- 关联读取：`fs/proc/generic.c::{remove_proc_entry,remove_proc_subtree,proc_remove,
  proc_create_seq_private}` 与 `include/linux/proc_fs.h` 用于核对 rundown、NULL 删除和私有 seq
  状态；`kernel/irq/manage.c::{irq_can_set_affinity_usr,irq_set_affinity,irq_setup_affinity}` 及
  action 安装/释放调用点用于核对 affinity 和 proc 生命周期；`kernel/irq/irqdesc.c::{
  irq_find_desc_at_or_after,free_desc}`、`kernel/irq/internals.h::{irq_desc_get_ref,
  irq_desc_put_ref}`、`kernel/irq/settings.h` 的 PROC_VALID helpers 和 `kernel/irq/chip.c` 的更新
  调用用于核对 Maple Tree/RCU/rcuref/资格位协议；另索引 `arch/alpha/{kernel/irq.c,
  include/asm/hw_irq.h}` 的 `ACTUAL_NR_IRQS` 覆盖。关联区域不在本文件修改授权范围内。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`（去注释词法流 SHA-256 均为
  `6c6808ca4b1e73042e52c88a290f486f8c5a57eb4935f90e0e11af2ebafbfb1c`），新增 221 行且
  删除 0 行，原有代码和注释无改写；`git diff --check` 通过，补丁 checkpatch 为 0 error、
  66 个仅由 UTF-8 中文注释显示宽度触发的行宽 warning。仓库缺少 `.config`（`NO_CONFIG`），
  未执行目标编译。

## `kernel/irq/debugfs.c` 验收记录（2026-08-11）

- 已按主方法论第 17 章完成修改后整文件强制复读；当前文件 340 行，共核对 11 个条件编译函数
  定义。函数头均直接位于对应声明之前，分行书写的 `static void` 与函数名归属正确。
- 函数清单（函数头/声明/函数体结束）：`irq_debug_show_bits` 23-27/28/37、SMP 分支
  `irq_debug_show_masks` 40-44/45/60、UP 替代定义同名函数 62/63/63、
  `irq_debug_show_chip` 82-86/87/104、`irq_debug_show_data` 106-110/111/126、
  `irq_debug_show` 194-203/204/228、`irq_debug_open` 230-234/235/238、
  `irq_debug_write` 240-247/248/266、`irq_debugfs_copy_devname` 277-284/285/292、
  `irq_add_debugfs_entry` 294-301/302/312、`irq_debugfs_init` 314-321/322/339。
- 实体与英文注释：核对 `irq_dir`、`irqchip_flags`、`irqdata_states`、`irqdesc_states`、
  `irqdesc_istates`、`dfs_irq_ops` 以及 `__initcall` 的角色、条件配置、长期借用和失败表示；原文件
  只有 SPDX 与版权两项英文注释，均按规则逐字保留并豁免，审计结果为
  `PAIRED=0 EXEMPT=2 UNPAIRED=0`。
- 路径与并发：复核 desc raw lock 下从 flow handler/desc 状态到叶层/父层 irq_data、chip/domain
  回调的完整输出；位表只展开已知项而原始十六进制保留未知位；SMP affinity 的配置分支；安全
  `debugfs_create_file()` 对 open/read/write/llseek 的 active-user 保护，以及 `free_desc()` 先
  `debugfs_remove()`、等待访问结束、再释放 desc/dev_name 的顺序。明确记录 dev_name 建立期写入
  不持 desc 锁、只提供 best-effort 显示，重复调用会覆盖并泄漏旧分配这一当前实现边界。
- 复杂路径只读抽查：`irq_debug_show()`/`irq_debug_show_data()` 可仅沿注释复述锁定、状态展开、
  provider 回调和层级递归；`irq_debug_write()` 可复述 7 字节截断、宽松前缀（含零长度）匹配、
  注入错误与未知文本消费；`irq_debugfs_init()`/`irq_add_debugfs_entry()` 可复述已有/新增 desc
  两条注册路径、dentry/ERR_PTR 哨兵、best-effort 失败和最终删除责任。入口、返回、ownership、
  竞态双方与 cleanup 均可由注释回答。
- 关联读取：`fs/debugfs/{file.c,inode.c}` 与 `include/linux/debugfs.h` 用于核对 full-fops proxy、
  active-user/remove 屏障、ERR_PTR/NULL 删除和 best-effort 创建；`kernel/irq/debugfs.h` 与
  `kernel/irq/irqdesc.c::{alloc_descs,free_desc}` 用于核对 dentry/dev_name 所有权及发布/撤销顺序；
  `kernel/irq/msi.c::__msi_domain_alloc_irqs` 用于核对设备名复制时机；`kernel/irq/resend.c::
  irq_inject_interrupt` 用于核对注入前提/错误；`kernel/irq/irqdomain.c::irq_domain_debugfs_init`
  用于核对共享根与 domain 补注册。这些关联区域不在当前文件修改授权范围内。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`（去注释词法流 SHA-256 均为
  `4053868fe94534ae801715d0938b49b41f807fe4147487d07718f6d9b3b09af3`），新增 84 行且删除
  0 行，原有代码、注释和空行无改写；`git diff --check` 通过，补丁 checkpatch 为 0 error、
  30 个仅由 UTF-8 中文注释显示宽度触发的行宽 warning。仓库缺少 `.config`（`NO_CONFIG`），
  未执行目标编译。

## `kernel/irq/devres.c` 验收记录（2026-08-11）

- 已按主方法论第 17 章完成修改后整文件强制复读；当前文件 449 行，共核对 15 个条件编译函数
  定义。全部中文函数头紧邻对应声明，公开 kernel-doc 与中文学习契约分块清晰。
- 函数清单（函数头/声明/函数体结束）：`devm_irq_release` 26/27/32、`devm_irq_match`
  34/35/40、`devm_request_result` 42-46/47/56、`__devm_request_threaded_irq`
  58-63/64/93、`devm_request_threaded_irq` 120-124/125/134、
  `__devm_request_any_context_irq` 137-141/142/169、`devm_request_any_context_irq`
  195-199/200/208、`devm_free_irq` 222-226/227/233、`devm_irq_desc_release`
  242/243/248、`__devm_irq_alloc_descs` 267-275/276/298、
  `devm_irq_alloc_generic_chip` 315-320/321/334、`devm_irq_remove_generic_chip`
  345/346/351、`devm_irq_setup_generic_chip` 368-376/377/397、
  `devm_irq_domain_remove` 402/403/408、`devm_irq_domain_instantiate`
  419-427/428/447。
- 实体与英文注释：核对 `irq_devres`、`irq_desc_devres`、`irq_generic_chip_devres` 三类逆操作
  凭据的字段 ownership、发布时机和释放回调。原文件 8 个说明/kernel-doc 块逐字保留并有紧邻
  中文翻译与补充；SPDX 和两个条件编译尾注按规则豁免，审计结果为
  `PAIRED=8 EXEMPT=3 UNPAIRED=0`。
- 路径与并发：复核共同模式“先分配未入栈 devres -> 取得底层资源 -> 填逆操作参数 ->
  devres_add 发布”，以及底层失败时只释放孤立记录；设备 devres_lock 保护入栈/摘除，release 在
  锁外按 LIFO 执行。threaded 与 any-context 成功返回类别差异、统一 dev_err_probe 错误日志、
  `devm_free_irq` 的 irq+dev_id 精确匹配与找不到 WARN 均已覆盖；desc/domain/generic chip 明确只
  管自身资源，依赖 action/mapping 必须更晚登记或更早撤销。
- 复杂路径只读抽查：`__devm_request_threaded_irq()` 可复述记录分配、名字回退、IRQ 申请、失败
  回滚与成功发布；`__devm_request_any_context_irq()` 可解释为何 0/正值均为成功且必须保留
  HARDIRQ/NESTED 类别；`devm_irq_setup_generic_chip()` 可解释 setup 记录为何后于 gc 内存记录、
  LIFO 如何保证先 remove 后 free；`devm_irq_domain_instantiate()` 可复述 ERR_PTR 原样传播及只有
  成功 domain 才进入 devres 栈。入口、返回、ownership、发布点和 cleanup 均可由注释回答。
- 关联读取：`drivers/base/devres.c::{devres_free,devres_add,devres_release,
  devres_release_all}` 与 `include/linux/device/devres.h` 用于核对设备锁、最新匹配、锁外 release
  和逆序释放；`kernel/irq/manage.c::{request_any_context_irq,request_threaded_irq,free_irq}` 用于
  核对成功类别与同步释放；已验收的 `kernel/irq/{irqdesc.c,generic-chip.c,irqdomain.c}` 分别用于
  核对描述符区间、generic setup/remove 和 domain instantiate/remove 的底层契约。另索引
  `include/linux/{interrupt.h,irqdomain.h}`、`drivers/irqchip/irq-lan966x-oic.c` 与后续 TODO 中的
  `kernel/irq/irq_sim.c` 调用声明/实例；关联区域不在本文件修改授权范围内。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`（去注释词法流 SHA-256 均为
  `4556ae8e3c26623e7687fd5ee6017ccd8feedd150c6bfe7c27ffd88c94f00969`），新增 78 行且删除
  0 行，原有代码和注释无改写；`git diff --check` 通过，补丁 checkpatch 为 0 error、39 个仅由
  UTF-8 中文注释显示宽度触发的行宽 warning。仓库缺少 `.config`（`NO_CONFIG`），未执行目标编译。

## `kernel/irq/autoprobe.c` 验收记录（2026-08-11）

- 已按主方法论第 17 章完成修改后整文件强制复读；当前文件 233 行，共核对 3 个函数定义：
  `probe_irq_on` 函数头/声明/函数体结束为 44-53/54/132、`probe_irq_mask`
  147-155/156/175、`probe_irq_off` 195-206/207/230；三个函数头均紧邻声明，导出宏归属正确。
- 实体与英文注释：核对跨 API 持有的 `probing_active` mutex、局部候选 mask/计数与 desc 借用；
  SPDX/版权文件头按规则豁免，其余 12 个英文依赖说明、kernel-doc 和阶段注释逐字保留并有紧邻
  中文翻译与补充，审计结果为 `PAIRED=12 EXEMPT=2 UNPAIRED=0`。原 BUGS 的“两个探测者可
  重叠”已明确标为与当前 mutex 实现不符的历史描述。
- 路径与并发：复核 async 全局静止、第一轮启动与 20ms 旧事件冲洗、第二轮
  AUTODETECT|WAITING 发布/重新 startup、100ms 杂散观察、预触发项过滤，以及驱动主动触发后
  mask/off 两种全量 cleanup。probe mutex 由 on 获取、结束 API 在同一任务释放；返回 0 也仍需
  配对，遗漏结束会永久阻塞后来者。每项状态/硬件变化持 desc raw lock，但全局遍历不取得
  sparse 生命周期锁，因此该 legacy API 只适合文档所述初始化探测，不误称热插拔安全。
- 复杂路径只读抽查：`probe_irq_on()` 可仅沿注释复述两轮 startup、两个等待窗口、
  WAITING 的事件证据与 quiet 候选保留；`probe_irq_mask()` 可解释为何 @val=0 仍必须全量扫描、
  为何仅返回低 16 位；`probe_irq_off()` 可解释 0/正/负结果、@val 未使用、多个候选和全局清理。
  另明确记录 on 只编码低 32 位但武装全部 IRQ、off 又不按 @val 过滤，故高编号触发仍可能影响
  唯一/多候选结果这一当前实现边界。
- 关联读取：`kernel/irq/chip.c::irq_can_handle_actions()` 用于核对事件入口清 WAITING/置
  PENDING，`kernel/irq/handle.c::handle_bad_irq()` 用于区分默认坏中断入口，`kernel/irq/
  internals.h` 与 `settings.h::irq_settings_can_probe()` 用于核对 istate/策略位；
  `include/linux/{interrupt.h,irqnr.h}` 用于核对旧式八步调用协议、非配置 stub 与无引用全空间
  迭代；`drivers/mfd/ucb1x00-core.c` 和 `drivers/pcmcia/yenta_socket.c` 用于核对 off/mask 两种真实
  调用及设备主动触发时序。关联区域不在当前文件修改授权范围内。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`（去注释词法流 SHA-256 均为
  `03d9dd17c3499a3b63417d31d16d388ecb34f32be4b7e4f139eb4a9ed3842a5c`），新增 57 行且删除
  0 行，原有代码、注释和空行无改写；`git diff --check` 通过，补丁 checkpatch 为 0 error、
  31 个仅由 UTF-8 中文注释显示宽度触发的行宽 warning。仓库缺少 `.config`（`NO_CONFIG`），
  未执行目标编译。

## `kernel/irq/matrix.c` 验收记录（2026-08-11）

- 已按主方法论第 17 章完成修改后整文件强制复读；当前文件 666 行，共核对 19 个函数定义，
  全部函数头紧邻声明，条件编译 debug 函数归属正确。
- 函数清单（1，函数头/声明/函数体结束）：`irq_alloc_matrix` 63-71/72/103、
  `irq_matrix_online` 109-114/115/130、`irq_matrix_offline` 136-140/141/151、
  `matrix_alloc_area` 153-158/159/175、`matrix_find_best_cpu` 178-182/183/201、
  `matrix_find_best_cpu_managed` 204-208/209/227、`irq_matrix_assign_system`
  240-249/250/268、`irq_matrix_reserve_managed` 279-286/287/314、
  `irq_matrix_remove_managed` 328-335/336/364、`irq_matrix_alloc_managed`
  372-380/381/409。
- 函数清单（2）：`irq_matrix_assign` 418-425/426/439、`irq_matrix_reserve`
  450-455/456/463、`irq_matrix_remove_reserved` 474-478/479/483、`irq_matrix_alloc`
  492-500/501/533、`irq_matrix_free` 543-552/553/577、`irq_matrix_available`
  585-590/591/598、`irq_matrix_reserved` 604/605/608、`irq_matrix_allocated`
  616-620/621/626、`irq_matrix_debug_show` 637-642/643/665。
- 实体与英文注释：完整核对 `cpumap` 的 available/allocated/managed/managed_allocated、
  online/initialized 与双位图关系，以及 `irq_matrix` 的搜索区间、三张共享/每 CPU 位图、全局
  计数和 per-CPU ownership；SPDX/版权按规则豁免，其余 22 个 kernel-doc/块/行注释逐字保留并
  有紧邻中文翻译与学习补充，审计为 `PAIRED=22 EXEMPT=2 UNPAIRED=0`。
- 路径与并发：复核 system_map、managed_map、alloc_map 三集合并集查空，managed 预留与实际
  占用交集，普通 available 在预留/分配/释放时的不同计数点；online 首次初始化与再次上线保留
  dormant 位图，offline 只移除全局 available 贡献；普通 global reservation 不绑定具体资源，
  必须由 reserved alloc 或 remove 精确消费。分配器无内部锁且共享 scratch_map，所有修改必须由
  provider 外部锁串行；debugfs 仅用 CPU read lock 稳定枚举，明确是非事务快照。
- 边界与只读状态推演：以两个 CPU、一个 system 位、每 CPU 一个 managed 预留推演 managed
  alloc、普通 reserved alloc、CPU offline、离线普通 free、再 online、managed free/remove，逐项
  核对 local/global available 与四类计数。确认离线 dormant 位释放会修正本地 bitmap/available，
  但按现代码不减少 `total_allocated`，故该 trace/debug 字段不能在此场景当精确位图权重；
  `irq_matrix_assign_system()` 的现有 BUG_ON 只拒绝 bit>matrix_bits，真实调用前提仍是严格 `<`；
  reserved remove/consume 和 managed 类型参数也无下溢/错类防护，均需调用协议保证。
- 复杂函数抽查：`irq_matrix_reserve_managed()` 可复述跨 CPU 逐项取得与失败回滚；
  `irq_matrix_remove_managed()` 可解释为何只能删除 managed_map & ~alloc_map、全部仍占用时为何只
  WARN；`irq_matrix_alloc()` 可复述选 CPU、共享 scratch 提交、四项计数和 reservation 消费；
  `irq_matrix_free()` 可复述 online/offline 与 managed/regular 四象限的不同账目。
- 关联读取：`arch/x86/kernel/apic/vector.c` 的 `vector_lock`、system/legacy 初始化、managed/
  reserved 分配、迁移释放与 CPU 下线容量检查用于核对外部串行和真实生命周期；
  `include/trace/events/irq_matrix.h` 用于核对 trace 字段；本地历史提交 `651ca2c00405` 用于核对
  offline dormant 分配保留修复的原始意图；`drivers/irqchip/{irq-loongarch-avec.c,
  irq-riscv-imsic-state.c}` 与 `include/linux/irq.h` 用于索引其他 provider/声明。关联区域不在当前
  文件修改授权范围内。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`（去注释词法流 SHA-256 均为
  `17c4894781d2021648edca7e0f0abd0de3d21a08d082aa5700c8048ec0b5292d`），新增 147 行且删除
  0 行，原有代码和注释无改写；`git diff --check` 通过，补丁 checkpatch 为 0 error、63 个仅由
  UTF-8 中文注释显示宽度触发的行宽 warning。仓库缺少 `.config`（`NO_CONFIG`），未执行目标编译。

## `kernel/irq/irq_sim.c` 验收记录（2026-08-11）

- 已按主方法论第 17 章完成修改后整文件强制复读；当前文件 391 行，共核对 16 个函数定义，
  全部函数头紧邻声明，分行返回类型和四个导出入口归属正确。
- 函数清单（1，函数头/声明/函数体结束）：`irq_sim_irqmask` 34/35/40、
  `irq_sim_irqunmask` 42/43/48、`irq_sim_set_type` 50-53/54/64、
  `irq_sim_get_irqchip_state` 66-70/71/87、`irq_sim_set_irqchip_state`
  89-93/94/113、`irq_sim_request_resources` 115-119/120/132、
  `irq_sim_release_resources` 134-138/139/148、`irq_sim_handle_irq`
  162-172/173/188、`irq_sim_domain_map` 190-195/196/213、`irq_sim_domain_unmap`
  215-219/220/231。
- 函数清单（2）：`irq_domain_create_sim` 249-254/255/259、`irq_domain_create_sim_full`
  262-270/271/301、`irq_domain_remove_sim` 310-318/319/328、
  `devm_irq_domain_remove_sim` 331/332/337、`devm_irq_domain_create_sim`
  350-353/354/360、`devm_irq_domain_create_sim_full` 363-370/371/390。
- 实体与英文注释：核对共享 `irq_sim_work_ctx` 的 domain/pending/HARD irq_work/ops/user_data
  ownership、每映射 `irq_sim_irq_ctx`、共享 `irq_sim_irqchip` 与 `irq_sim_domain_ops`；SPDX/版权
  按规则豁免，其余 4 个英文行注释/kernel-doc 均逐字保留并有紧邻中文翻译与补充，审计为
  `PAIRED=4 EXEMPT=2 UNPAIRED=0`。
- 路径与并发：复核 map 分配 irq_ctx -> 发布 chip/data/simple handler -> 清 NOREQUEST/NOAUTOEN、
  置 NOPROBE，以及 dispose mapping 时 handler/reset/free 逆序；mask/unmask 的 enabled 由 desc
  锁域维护，pending bitmap 由 state callback 与 HARD irq_work 用原子 bitops 协作。request/release
  provider callback 仅在首 action/末 action 配对，并按值复制 ops、借用 user_data；创建用
  `__free`/`no_free_ptr` 明确临时所有权转移，managed 创建用 add_action_or_reset 保证登记失败立即
  回滚。
- 边界与复杂路径抽查：masked get PENDING 返回 0 但不写输出、masked set 静默忽略；set_type
  实际接受 NONE 与 edge 组合、拒绝 level。`irq_sim_handle_irq()` 可复述低到高领取、映射和 simple
  dispatch；同位/高位并发可在本轮观察，低于当前 offset 的并发置位依赖重新排队，而当前全图
  while + 无回绕 find_next 缺少末尾防线，已如实标注现有边界。`irq_domain_remove_sim()` 必须先由
  调用者 dispose mappings/actions：它先 sync work、释放 pending/work_ctx，再 remove domain；若
  映射仍活跃会形成悬空 chip_data/host_data。原文“一次性分配 dummy interrupts”也按当前线性
  domain 修正为只建立 hwirq 容量、virq/desc 按 mapping 动态取得。
- 复杂只读场景：验证 enabled mapping 注入 -> 原子 pending -> HARD work -> mapping ->
  handle_simple_irq，masked 注入/查询，多个 pending 位有序消费；验证 work_ctx/pending/domain 三阶段
  创建失败和 managed action 登记失败的 ownership；验证 gpio-sim 先 managed 创建 domain、后登记
  dispose mappings，LIFO 在 detach 时先拆映射再 remove_sim。
- 关联读取：`include/linux/irq_sim.h` 的 ops/API，`kernel/irq_work.c::{irq_work_queue,
  irq_work_single,irq_work_sync}`，`kernel/irq/manage.c` 的 irqchip state 与 request/release resources，
  `kernel/irq/chip.c::handle_simple_irq`、`kernel/irq/irqdomain.c::{irq_domain_create_linear,
  irq_domain_remove}` 用于核对上下文与生命周期；`drivers/gpio/{gpio-sim.c,gpio-mockup.c}` 用于核对
  真实注入、pin lock callbacks 和 devres 顺序；本地历史提交 `06459901d55e`、`011f583781fa` 用于
  核对 bitmap 并发设计与扩展 callbacks 意图。关联区域不在当前文件修改授权范围内。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`（去注释词法流 SHA-256 均为
  `130ca3475bd360e1da0dccc7916d592444bc71c6985225a8960ad272f34d4683`），新增 94 行且删除
  0 行，原有代码和注释无改写；`git diff --check` 通过，补丁 checkpatch 为 0 error、42 个仅由
  UTF-8 中文注释显示宽度触发的行宽 warning。仓库缺少 `.config`（`NO_CONFIG`），未执行目标编译。

## `kernel/irq/irq_test.c` 验收记录（2026-08-11）

- 已按主方法论第 17 章完成修改后整文件强制复读；当前文件 290 行，共核对 9 个函数定义：
  `noop_handler` 22/23/26、`noop` 28/29/29、`noop_ret` 30/31/31、`noop_affinity`
  33/34/40、`irq_test_setup_fake_irq` 59-68/69/87、`irq_disable_depth_test`
  89-93/94/116、`irq_free_disabled_test` 118-122/123/149、`irq_shutdown_depth_test`
  151-156/157/207、`irq_cpuhotplug_test` 209-217/218/270；函数头均紧邻声明。
- 实体与英文注释：核对永久静态 `fake_irq_chip`、四项 `irq_test_cases`、suite 注册与模块元数据；
  SPDX 按规则豁免，唯一原英文架构默认状态行逐字保留并有紧邻中文翻译，审计为
  `PAIRED=1 EXEMPT=1 UNPAIRED=0`。
- 路径与状态：fake chip 的 startup/enable/disable/mask/ack 均不失败，仅 affinity 回调维护
  effective mask，使测试隔离观察 IRQ 核心 depth/activated/started/managed 状态。基本用例覆盖
  request 自动启动 -> disable depth 1 -> enable depth 0；disabled free 后 depth 保留及 re-request
  规范化；managed IRQ 人工 shutdown/deactivate 后 activate/startup 不吞掉软件 disable depth；
  CPU1 下线/上线后同样保留 depth，最终 enable 恢复硬件生命周期。
- KUnit 与环境边界：`kunit_skip()` 已核对会终止当前用例，CPU hotplug 测试只在 SMP、CPU1
  存在/可热插拔/在线时运行；但 remove/add 用 EXPECT 而非 ASSERT，add 失败可能把 CPU1 留在
  offline，故仅适合隔离测试环境。setup 分配动态 desc 后各用例只 `free_irq()`，没有
  `irq_free_descs()` 配对；`IRQ_KUNIT_TEST` 为依赖 `KUNIT=y`/`SPARSE_IRQ` 的 bool 内建测试，当前
  描述符会保留到测试系统结束，已在注释如实记录。
- 复杂只读抽查：`irq_free_disabled_test()` 可解释 action 生命周期与 depth 为什么分离；
  `irq_shutdown_depth_test()` 可复述 desc 锁内 shutdown/deactivate、锁外 reactivate/managed startup
  和最终 enable；`irq_cpuhotplug_test()` 可复述四项 skip 门禁、真实 CPU 状态改变、失败继续与
  恢复断言。ASSERT/EXPECT 的中止/继续差异和 cleanup 边界均可由注释回答。
- 关联读取：已验收 `kernel/irq/{manage.c,chip.c,cpuhotplug.c,irqdesc.c}` 的 request/free、
  depth、managed startup/shutdown、CPU online/offline 与 desc 分配契约；`include/kunit/test.h` 用于
  核对 skip/ASSERT 中止语义；`kernel/irq/{Kconfig,Makefile}` 用于核对 IRQ_KUNIT_TEST 的 bool、
  KUNIT/SPARSE_IRQ 依赖与 built-in 编译方式。关联区域除后续 TODO 中 Kconfig/Makefile 外不在当前
  文件修改授权范围内。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`（去注释词法流 SHA-256 均为
  `139914442fd5d4b0b3d88878f5f51e4bc903e44d1b7c28bfc0473e514bedf953`），新增 54 行且删除
  0 行，原有代码和注释无改写；`git diff --check` 通过，补丁 checkpatch 为 0 error、18 个仅由
  UTF-8 中文注释显示宽度触发的行宽 warning。仓库缺少 `.config`（`NO_CONFIG`），未运行 KUnit。

## `kernel/irq/dummychip.c` 验收记录（2026-08-11）

- 已按主方法论第 17 章完成修改后整文件强制复读；当前文件 103 行，三个函数定义
  `ack_bad` 24-29/30/36、`noop` 43-46/47/47、`noop_ret` 49-52/53/56 的函数头均紧邻声明。
- 实体与英文注释：核对静态哨兵 `no_irq_chip` 和导出的静态最小实现 `dummy_irq_chip`；许可证/
  版权按规则豁免，其余文件说明、非法向量、NOP、no-controller 与 dummy-controller 原英文均逐字
  保留并有紧邻中文翻译或说明，审计为 `PAIRED=4 EXEMPT=2 UNPAIRED=0`。
- 语义与边界：`no_irq_chip` 表示描述符尚无有效控制器，`irq_set_chip(..., NULL)` 会恢复到它，
  `__setup_irq()` 会以 `-ENOSYS` 拒绝注册；其 NOP 槽位不能被理解为可工作的虚拟硬件，异常 ack
  先打印 desc 再交给体系结构 `ack_bad_irq()`。`dummy_irq_chip` 则是可正常申请的最小合法控制器，
  startup/ack/mask/unmask 均不访问硬件，调用者必须保证中断源确实不需要这些控制动作。
- 复杂只读抽查：可由注释区分“缺失控制器哨兵”和“真实简单中断源”两条路径，并复述 bad-vector
  诊断顺序、无硬件副作用、静态对象 ownership、模块导出以及 `IRQCHIP_SKIP_SET_WAKE` 的含义。
- 关联读取：`kernel/irq/manage.c::__setup_irq`、`kernel/irq/chip.c::irq_set_chip`、
  `kernel/irq/irqdesc.c` 的默认初始化/重置、`kernel/irq/proc.c` 的哨兵过滤和体系结构
  `ack_bad_irq()` 声明/实现用于核对 no-controller 路径；Aspeed I2C 等实际使用点用于确认
  `dummy_irq_chip` 服务于无需硬件控制的真实/解复用中断源。关联区域不在当前文件修改授权范围内。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`（去注释词法流 SHA-256 均为
  `28a037ead3a9c196fea1046ebdac1ac12ec99914e66340a6715a615058353f5a`），新增 39 行且删除
  0 行，原有代码和注释无改写；`git diff --check` 通过，checkpatch 为 0 error、0 warning。
  仓库缺少 `.config`（`NO_CONFIG`），未执行目标编译。

## `kernel/irq/debug.h` 验收记录（2026-08-11）

- 已按主方法论第 17 章完成修改后整文件强制复读；当前文件 77 行，唯一函数
  `print_irq_desc` 23-33/34/72 的函数头紧邻声明，三个局部调试宏在使用后均被 `#undef`。
- 实体与英文注释：核对 `___P`/`___PS`/`___PD`、函数内静态 ratelimit 及分组输出；SPDX
  按规则豁免，原有 “Debugging printout” 和 `FIXME` 均逐字保留并有紧邻中文说明，审计为
  `PAIRED=2 EXEMPT=1 UNPAIRED=0`。
- 路径与并发：函数从坏中断 hardirq 路径输出只读诊断快照，不统一要求 desc 锁，所以不能把多行
  当成一致性快照；先经过限速才读取/打印 desc，action 非空才解引用 handler。由于这是头文件中的
  `static inline`，每个包含它的编译单元各有一份 ratelimit，同一编译单元内所有 IRQ 共享每 5 秒
  5 次额度，而非整个内核只有一个全局桶。
- 旧状态边界：历史提交 `32f4125ebffe` 将 INPROGRESS/MASKED/DISABLED 移到 irq_data 后留下
  `___PD` FIXME；其替换文本不使用参数，故三个已不存在的 `IRQS_*` token 不会展开或求值，当前
  也不会输出。`___P` 读取持久策略位，`___PS` 读取仍在 istate 的内部运行时位。
- 关联读取：`kernel/irq/{handle.c,dummychip.c}` 的两个调用点、`internals.h` 的包含位置、
  `settings.h`/`include/linux/irq.h` 的状态位定义以及本地历史提交 `11bca0a83f83` 的坏中断风暴限速
  意图用于核对上下文与历史边界；关联区域已有注释或不在当前文件修改授权范围内。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`（去注释词法流 SHA-256 均为
  `9b9ce7b1a5fde64c0dd1413f105fee027918f7eca2ec8c849a6500e14876260c`），新增 28 行且删除
  0 行，原有代码和注释无改写；`git diff --check` 通过，补丁 checkpatch 为 0 error、13 个仅由
  UTF-8 中文注释显示宽度触发的行宽 warning。整文件 checkpatch 另报 2 error、8 warning，均来自
  原有裸 `if` 宏、未使用宏参数和无日志级别的 printk，未在仅注释任务中改代码。
  仓库缺少 `.config`（`NO_CONFIG`），未执行目标编译。

## `kernel/irq/debugfs.h` 验收记录（2026-08-11）

- 已按主方法论第 17 章完成修改后整文件强制复读；当前文件 71 行。启用 debugfs 时核对
  `irq_remove_debugfs_entry` 31-35/36/40，启用 debugfs 但关闭 IRQ_DOMAIN 时核对
  `irq_domain_debugfs_init` 47/48/50；关闭 debugfs 时核对 `irq_add_debugfs_entry`
  53-57/58/60、`irq_remove_debugfs_entry` 61/62/64、`irq_debugfs_copy_devname` 65/66/68，
  所有 inline 函数头均直接归属对应声明。
- 实体与配置矩阵：核对 `irq_bit_descr`、`BIT_MASK_DESCR`、四个真实接口声明和两层配置分支。
  GENERIC_IRQ_DEBUGFS 关闭时三个类型安全 stub 不读取对象、不取得 ownership；实参仍按普通函数
  规则求值。debugfs 开启但 IRQ_DOMAIN 关闭时只裁掉 domain 视图，单 IRQ 视图仍正常构建。
- 生命周期：单 IRQ 文件借用 irq_desc，销毁必须先 `debugfs_remove()` 阻止新操作并等待安全
  file operation 退出，再释放 show 可能读取的复制 `dev_name`；两者随 desc 一起终止。建档与名称
  复制是 best-effort，失败不得影响 IRQ 主功能；位描述表的 name 借用宏生成的静态字符串。
- 英文注释：SPDX 与两个条件编译尾注按许可证/结构标签规则豁免，无其他原英文自然语言注释，
  审计为 `PAIRED=0 EXEMPT=3 UNPAIRED=0`。
- 关联读取：`kernel/irq/debugfs.c` 的真实实现和 full-fops、`irqdesc.c::{free_desc,
  __irq_alloc_descs}` 的删除/创建顺序、`msi.c` 的设备名复制调用以及 `irqdomain.c` 的 domain
  初始化与文件删除路径用于核对配置、ownership 和并发；关联区域已有注释或不在当前文件授权范围。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`（去注释词法流 SHA-256 均为
  `da047a01b03102347ad39bdbfa938d4ec59e15c985346da36b8fb4b755921e25`），新增 27 行且删除
  0 行，原有代码和注释无改写；`git diff --check` 与整文件 checkpatch 均通过，补丁 checkpatch
  为 0 error、15 个仅由 UTF-8 中文注释显示宽度触发的行宽 warning。仓库缺少 `.config`
  （`NO_CONFIG`），未执行目标编译。

## `kernel/irq/proc.h` 验收记录（2026-08-11）

- 已按主方法论第 17 章完成修改后整文件强制复读；当前文件 25 行。真实配置下两个声明均有
  直接契约；裁剪配置下 `irq_proc_calc_prec` 19/20/20、`irq_proc_update_chip` 21/22/22 两个
  inline 空实现的函数头均紧邻声明。
- 配置与语义：只有 `CONFIG_PROC_FS && CONFIG_GENERIC_IRQ_SHOW` 同时成立才维护
  `/proc/interrupts` 的全局格式约束。`irq_proc_calc_prec()` 随 total_nr_irqs 单调扩大编号列宽；
  `irq_proc_update_chip()` 立即读取可选 chip 名长度、但不保存借用指针，并允许从已关中断上下文
  进入。关闭展示时 stub 保留类型检查，实参仍会求值，但无状态、副作用或 ownership。
- 英文注释：除 SPDX 许可证外无原英文自然语言注释，审计为
  `PAIRED=0 EXEMPT=1 UNPAIRED=0`。
- 关联读取：`kernel/irq/proc.c::{irq_proc_calc_prec,irq_proc_update_chip}` 的单调约束和 irqsave
  raw lock、`irqdesc.c` 的初始化/扩容调用、`chip.c::irq_set_chip` 与
  `irqdomain.c::irq_domain_set_hwirq_and_chip` 的芯片安装调用用于核对上下文；关联区域均已有学习注释。
- 修改安全：`CODE_TOKENS_UNCHANGED=true`（去注释词法流 SHA-256 均为
  `38ab453a0f62efe0f8da59b7f79f890e0d2b9c10b15e65908d448b6dada403a4`），新增 12 行且删除
  0 行，原有代码和注释无改写；`git diff --check` 与整文件 checkpatch 均通过，补丁 checkpatch
  为 0 error、6 个仅由 UTF-8 中文注释显示宽度触发的行宽 warning。仓库缺少 `.config`
  （`NO_CONFIG`），未执行目标编译。

## `kernel/irq/Kconfig` 验收记录（2026-08-11）

- 已按主方法论第 17 章完成修改后整文件强制复读；当前文件 199 行，按源码顺序核对 30 个
  `config`：从体系结构可选择的隐藏能力位，到 SPARSE_IRQ/debugfs/KUnit 用户选项，再到 menu
  外的根 handler 与 Octeon 兼容项；每项均有紧邻中文语义或成组说明。
- 配置关系：复核 SMP/DEBUG_FS/KUNIT/SPARSE_IRQ/CAVIUM_OCTEON_SOC 的 `depends on`，
  IRQ_WORK/IRQ_DOMAIN/IRQ_DOMAIN_HIERARCHY/GENERIC_IRQ_INJECTION 的 `select`，SMP 的 `imply`，
  以及 KUNIT_ALL_TESTS/SoC 的默认值；明确 capability bool 多由架构/irqchip 选择，而非普通用户开关。
- 关键路径：SPARSE_IRQ 的可见性受 MAY_HAVE_SPARSE_IRQ 门控；IRQ_SIM 组合 irq_work 与线性 domain；
  debugfs 同时启用软件注入但仍是默认关闭的诊断面；KUnit 测试要求内建 KUNIT 与动态 desc，真实
  CPU 热插拔副作用不适用于常规内核；reservation mode 会限制 affinity management 的运行期更新。
- 英文注释与 help：SPDX 按许可证规则豁免，25 组原英文 `#` 注释均逐字保留并有紧邻中文翻译/
  补充，审计为 `PAIRED=25 EXEMPT=1 UNPAIRED=0`；SPARSE_IRQ、GENERIC_IRQ_DEBUGFS、
  IRQ_KUNIT_TEST、GENERIC_IRQ_MULTI_HANDLER 四段英文 help 未改写，其含义已在对应 config 前用
  中文注释完整覆盖。
- 关联读取：已验收的 `autoprobe.c`、`proc.c`、`resend.c`、`generic-chip.c`、`irqdomain.c`、
  `irq_sim.c`、`ipi*.c`、`msi.c`、`matrix.c`、`irq_test.c`、`handle.c`、`chip.c` 和相关头文件用于
  核对每个配置分支的真实实现与裁剪边界；未扩大任何关联文件修改范围。
- 修改安全：去除 `#` 注释后的 Kconfig 语义流 SHA-256 前后均为
  `27ea0fe9b015fdc9704c2d0b22d4c330a609fcdac011ee459af1edd90b23132e`，新增 34 行且删除
  0 行，所有 `config`、类型、依赖、选择、默认值、提示与 help 原文均未改写；`git diff --check`
  与补丁 checkpatch 均为 0 error、0 warning。仓库缺少 `.config`（`NO_CONFIG`），未运行配置生成或编译。

## `kernel/irq/Makefile` 验收记录（2026-08-11）

- 已按主方法论第 17 章完成修改后整文件强制复读；当前文件 35 行，核对 1 组无条件基础对象和
  15 条配置条件对象映射，每条 `obj-*` 赋值均有紧邻中文职责说明。
- 构建边界：基础对象始终进入 IRQ 核心，内部再按配置裁剪；区分 GENERIC_PENDING_IRQ 的单 IRQ
  延迟 affinity 提交与 GENERIC_IRQ_MIGRATION 的 CPU 下线迁移；proc.o 只受 PROC_FS 门控，
  GENERIC_IRQ_SHOW 在源码内部继续裁剪；PM、MSI、IPI/IPI mux、SMP affinity、debugfs、matrix 与
  KUnit 对象均和 Kconfig 能力位一一对应。
- 英文注释：除 SPDX 许可证外原文件无自然语言注释，审计为
  `PAIRED=0 EXEMPT=1 UNPAIRED=0`；新增注释覆盖全部 16 组对象规则，无未说明的构建项。
- 关联读取：`kernel/irq/Kconfig` 的 30 个配置项及清单内各实现文件用于核对对象职责；特别复核
  `proc.c`、`migration.c`、`cpuhotplug.c`、`pm.c`、`ipi*.c` 的条件编译边界，未修改关联代码。
- 修改安全：去除 `#` 注释后的 Make 语义流 SHA-256 前后均为
  `871f46cc23381dcfa155e1222e7576eb313f04373db55b0d689767fed1d670bf`，新增 17 行且删除
  0 行，原有对象列表、顺序与条件均未改写；`git diff --check` 与补丁 checkpatch 均为
  0 error、0 warning。仓库缺少 `.config`（`NO_CONFIG`），未执行目标编译。

## `kernel/irq` 目录级总审计（2026-08-11）

- 清单闭环：31 个目标全部为 `[x]`，没有残留 `[~]` 或 `[ ]`；20 个本轮/后续文件使用独立
  `kernel/irq/<file>` 验收记录，11 个既有文件使用前置逐文件记录，均包含函数/实体、英文注释、
  生命周期与并发、复杂路径、关联读取和修改安全结论。
- 仅注释门禁：逐一将 29 个 C/头文件去除 C 注释、将 Kconfig/Makefile 去除 `#` 注释后与
  `HEAD` 比较，31/31 语义流 SHA-256 一致；当前工作树中 26 个 IRQ 源文件共新增 2923 行、删除
  0 行，另 5 个已验收文件（settings.h、internals.h、irqdesc.c、handle.c、chip.c）相对 HEAD
  无未提交差异。新增行形态扫描未发现注释之外的源码/构建语句。
- 整批卫生：`git diff --check -- kernel/irq todo/kernel-irq-learning-comments.md` 通过；聚合
  `kernel/irq` 补丁 checkpatch 为 0 error、938 个 warning，warning 类型全部是 UTF-8 中文注释
  显示宽度超过 100 列，没有其他警告类别。Kconfig 与 Makefile 的补丁单独检查均为 0/0。
- 范围与环境：工作树状态只包含本任务的 IRQ 学习注释文件和本 TODO，没有不明重叠文件；各文件
  记录已逐项披露为核对语义而读取、但未获修改授权的关联源码。仓库仍无 `.config`
  （`NO_CONFIG`），因此未声称目标编译、KUnit 或配置组合构建通过。
- 结论：已按主方法论第 17 章完成全部目标的强制验收，并在单文件门禁之后额外完成目录级一致性
  审计；TODO 定义的 IRQ 学习注释范围已完成。
