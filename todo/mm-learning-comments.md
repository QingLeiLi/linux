# mm 学习注释进度

规则：单文件按 `doc/linux-kernel-source-learning-methodology.md` 第 17 章闭环；机械门禁同时启用 `--require-english-translation`；人工验收必须逐项保存函数使用场景、结构体字段释义和枚举解释三类清单；只加注释；`[~]` 未闭环时不开始下一文件。恢复任务先读本文件和 `todo/mm-learning-comments-baseline.md`，目标未变更时复用探查结论。

## 筛选

- 2026-08-27：扫描 190 个 `.c/.h`；密度门禁通过 0，未通过 190。
- 暂不重复：已有中文学习注释的 18 个文件（后续仅复审，不作为首轮目标）。
  `backing-dev.c`、`early_ioremap.c`、`filemap.c`、`hugetlb.c`、`hugetlb_cma.c`、`init-mm.c`、
  `memcontrol.c`、`mempolicy.c`、`mm_init.c`、`rmap.c`、`slab_common.c`、`slub.c`、`vmalloc.c`、
  `vmstat.c`、`kasan/hw_tags.c`、`kasan/sw_tags.c`、`kfence/core.c`、`kfence/report.c`。

## 固定顺序

1. 闭环 `mm/gup_test.h`。
2. 构建入口：`mm/Makefile` → `mm/Kconfig` → `mm/Kconfig.debug` → `damon/{Makefile,Kconfig}` → `kasan/Makefile` → `kfence/Makefile` → `kmsan/Makefile`。
3. 按基线表序号 2 起处理零中文 `.c/.h`；最后复审标记 `r` 的 18 个文件。
4. 排除 `.DS_Store`、两份 `.kunitconfig`；Rust 测试不套用 C/H 方法论，另行定标。

## 当前检查点（2026-09-21）

- 当前返工：`mm/memory_hotplug.c` 的机械门禁已通过，但人工复审发现多处专属函数头把
  “业务背景、参数、返回、注意事项”压缩或遗漏，尚不满足第 17.2；基线序号 144 恢复为 `[~]`，
  未开始序号 145 `mm/kasan/kasan_test_c.c`。
- `mm/mmap.c`：已完成第 17 章全文件复读与语义补注，源码完整差异 `+193/-0`。
- 机械门禁：`code=1228 comments=996 chinese=443 density=0.361 max_gap=10`，
  `untranslated=0`，退出码 0；命令含 `--require-english-translation`。
- 以下清单中的“通过”均表示专属函数头紧邻声明，四要素、调用场景、锁/ownership、
  返回类别和函数体阶段说明已与当前版本核对；无函数豁免。

### `mm/mmap.c` 函数与使用场景清单

“头/声明/尾”依次为专属中文函数头范围、声明行、函数体右花括号行。

|函数/配置|头/声明/尾|具体入口、触发条件与返回后的下一步|结论|
|---|---|---|:--:|
|`vma_set_page_prot`|100-103/104/118|VMA flag 修改路径重算保护；`WRITE_ONCE` 后页表撤保护者读取新值|通过|
|`check_brk_limits`|128-132/133/145|`brk`/`vm_brk_flags` 扩张前校验 fixed 区间与锁页额度；0 后继续建 VMA，errno 返回|通过|
|`brk` syscall|147-155/156/270|libc/malloc 扩缩传统堆；成功/失败均按 ABI 返回最终 `mm->brk`|通过|
|`round_hint_to_min`|276-279/280/287|`do_mmap` 处理非 fixed hint；规范化后交空闲地址选择器|通过|
|`mlock_future_ok`|289-293/294/311|mmap/brk 在新增锁页前核对 `RLIMIT_MEMLOCK`；允许后才实际建图和锁页|通过|
|`file_mmap_size_max`|313-316/317/337|`file_mmap_ok` 按 inode/驱动能力取得 offset 上限；0 表示无显式上限|通过|
|`file_mmap_ok`|339-345/346/359|`do_mmap` 在文件回调前拒绝偏移回绕/越界；true 才继续权限检查|通过|
|`do_mmap`|418-424/425/689|`vm_mmap_pgoff`/兼容重映射持写锁进入；成功由 `mmap_region` 发布，调用者锁外通知和 populate|通过|
|`ksys_mmap_pgoff`|691-695/696/744|现代/旧 mmap ABI 解析 fd 或匿名 hugetlb；下层返回后释放临时 file 引用|通过|
|`mmap_pgoff` syscall|746-753/754/760|现代页偏移 ABI；把六参数交公共包装并直接返回地址/errno|通过|
|`old_mmap` syscall|779-786/787/799|旧结构体 ABI；复制快照、校验字节 offset 后转页偏移并复用公共包装|通过|
|`stack_guard_placement`|807-813/814/820|上下选址器为 shadow stack 加一页起始 guard；普通 VMA 返回 0|通过|
|`vm_unmapped_area`|831-834/835/846|通用选址器按 TOPDOWN flag 分派 Maple Tree 空洞搜索并发 trace|通过|
|`generic_get_unmapped_area`|859-864/865/899|bottom-up 布局处理 fixed/hint/空洞；候选返回上层继续 LSM 校验|通过|
|`arch_get_unmapped_area` 缺省桩|903-910/911/917|无 arch 覆盖时保持 hook ABI并转发 bottom-up 通用实现|通过|
|`generic_get_unmapped_area_topdown`|924-929/930/984|top-down 布局从栈下搜索；仅 ENOMEM 时退化 bottom-up|通过|
|`arch_get_unmapped_area_topdown` 缺省桩|988-995/996/1003|无 arch 覆盖且 `MMF_TOPDOWN` 时转发通用实现|通过|
|`mm_get_unmapped_area_vmflags`|1006-1012/1013/1022|`__get_unmapped_area` 依据 mm 布局选择 arch hook；结果仍待统一验证|通过|
|`__get_unmapped_area`|1024-1029/1030/1092|mmap 选址总分派：file/shmem/THP/arch 后做范围、对齐和 LSM 检查|通过|
|`mm_get_unmapped_area`|1094-1098/1099/1104|无内部 vm_flags 的导出包装；调用者取得候选地址但尚未建 VMA|通过|
|`find_vma_intersection`|1116-1119/1120/1128|持 mmap_lock 检查 fixed/插入区间冲突；返回锁期借用首个交集|通过|
|`find_vma`|1139-1142/1143/1149|fault/栈扩展等查包含项或后一项；返回后必须再比较边界|通过|
|`find_vma_prev`|1165-1169/1170/1182|选址/栈扩展同时需要当前项与前驱；外部 mmap_lock 代替 RCU|通过|
|`cmdline_parse_stack_guard_gap`|1189-1195/1196/1207|早期 `__setup` 消费页数参数；完整合法值发布为字节全局量|通过|
|`expand_stack_locked` GROWSUP|1212-1218/1219/1222|向上栈 fault 命中前驱后持写锁扩展；0 后重试访问|通过|
|`find_extend_vma_locked` GROWSUP|1224-1227/1228/1246|fault 查不到包含项时只扩前驱向上栈；锁页区同步预 fault|通过|
|`expand_stack_locked` GROWSDOWN|1249-1255/1256/1259|普通配置持写锁向下扩后一栈；返回下层 errno 或 0|通过|
|`find_extend_vma_locked` GROWSDOWN|1261-1264/1265/1285|fault 位于后一 GROWSDOWN VMA 前时扩起点；成功返回锁期借用 VMA|通过|
|`expand_stack`|1313-1317/1318/1348|旧 fault 接口从读锁升级写锁并重查；成功降级为读锁，NULL 时无锁|通过|
|`do_munmap`|1358-1362/1363/1369|已持写锁的旧式包装构造 iterator；返回 `do_vmi_munmap` 结果|通过|
|`vm_munmap`|1371-1377/1378/1382|内核调用者解除规范地址区间；下层自行锁定并完成 userfaultfd|通过|
|`munmap` syscall|1385-1391/1392/1397|用户 ABI 先去地址 tag；返回 0/errno 后区间已拆除或保持失败状态|通过|
|`remap_file_pages` syscall|1403-1407/1408/1540|旧共享文件 ABI：读锁快照、锁外 LSM、写锁重验，再 fixed 重映射|通过|
|`vm_brk_flags`|1542-1546/1547/1596|内核匿名 brk helper 先 unmap 冲突再建图；解锁后通知/锁页 populate|通过|
|`tear_down_vmas`|1598-1602/1603/1627|`exit_mmap` 在树不可达后 close/free 全部 VMA；返回待归还承诺页|通过|
|`exit_mmap`|1630-1634/1635/1695|最后 mm 用户退出：通知、拆页表、RCU 摘树、释放 VMA并归还记账|通过|
|`may_expand_vm`|1701-1705/1706/1732|映射扩张前核对 AS/DATA rlimit；布尔结果决定是否继续，无记账副作用|通过|
|`vm_stat_account`|1734-1738/1739/1750|VMA 发布/撤销事务更新总页数及 exec/stack/data 分类|通过|
|`special_mapping_close`|1760-1766/1767/1773|vmops close 在 munmap/旧 mremap 时通知可选描述符回调|通过|
|`special_mapping_name`|1775-1780/1781/1785|`/proc` 等经 vmops 查询 `[vdso]` 类借用名称|通过|
|`special_mapping_mremap`|1787-1793/1794/1806|mremap 完成特殊 VMA 移动时校验 current mm 并通知描述符|通过|
|`special_mapping_split`|1808-1814/1815/1825|VMA core 拟拆分时恒拒绝，维持页数组与单一 VMA 长度不变量|通过|
|`special_mapping_fault`|1844-1850/1851/1879|缺页核心经 vmops 分派自定义回调或 NULL 终止页数组；页越界 SIGBUS|通过|
|`__install_special_mapping`|1881-1885/1886/1929|持写锁分配并发布特殊 VMA；失败释放未发布对象，成功由 mm 树持有|通过|
|`vma_is_special_mapping`|1931-1936/1937/1943|体系结构清理路径同时比对 spec 与专用 vmops，避免误识别|通过|
|`_install_special_mapping`|1954-1958/1959/1967|vDSO/VVAR 等持写锁安装 `vm_special_mapping`；fault 后续由专用表处理|通过|
|`mmap_init`|2035-2041/2042/2052|MM 启动序列初始化 committed-as、sysctl 和 VMA 状态；失败为 BUG|通过|
|`init_user_reserve`|2064-2067/2068/2076|subsys init/热插拔按空闲内存重算普通用户恢复预留|通过|
|`init_admin_reserve`|2089-2092/2093/2101|subsys init/热插拔按空闲内存重算管理员恢复预留|通过|
|`reserve_mem_notifier`|2122-2126/2127/2170|hotplug 链消费 ONLINE/OFFLINE，尊重显式配置或收缩不可兑现预留|通过|
|`init_reserve_notifier`|2172-2178/2179/2187|subsys init 注册 hotplug 回调；失败只记录日志且仍返回 0|通过|
|`mmap_read_lock_maybe_expand`|2212-2216/2217/2240|ELF 参数页对未发布 VMA 取读锁；必要时写锁扩向下栈再降级|通过|
|`dup_mmap`|2242-2251/2252/2480|`dup_mm` 在非 `CLONE_VM` fork 中复制父地址空间；失败销毁未发布子 mm|通过|

### 结构体字段清单

- `mmap_arg_struct`：`addr`=地址 hint、`len`=字节长度、`prot`=`PROT_*`、
  `flags`=`MAP_*`、`fd`=文件描述符、`offset`=字节偏移；六字段由用户产生、
  `copy_from_user` 一次性复制到栈后只读，生命周期仅覆盖 `old_mmap`，无引用 ownership。
- `vm_unmapped_area_info` 的本文件字段：`flags` 选择上下方向；`length` 为字节需求；
  `low_limit/high_limit` 构成半开搜索边界；`align_mask/align_offset` 构成对齐同余约束；
  `start_gap` 为起始 guard 字节。选址器栈上初始化后只由空洞搜索消费，无跨调用寿命。
- `vm_area_struct` 的本文件字段组：`vm_start/vm_end` 是 mmap_lock 保护的半开字节区间，
  `vm_flags/vm_page_prot` 是策略及硬件保护，`vm_pgoff/vm_file` 是页索引和带引用后端，
  `vm_mm` 指所属地址空间，`vm_ops/vm_private_data` 成对选择特殊映射回调及长期描述符；
  发布前由构造者写，发布后在 mmap_lock/生命周期规则下读，`dup_mmap` 对 file 单独增引用。
- `mm_struct` 的本文件字段组：`start_brk/brk/start_data/end_data` 描述字节边界；
  `total_vm/data_vm/exec_vm/stack_vm/locked_vm` 均以页计；`def_flags` 是新 VMA 缺省策略，
  `map_count` 与 Maple Tree 成员数相符，`mm_mt` 是 VMA 发布树，`mmap_base` 是选址基准，
  `mmap_lock` 保护拓扑。运行中由 mmap 锁同步；未发布的 fork 子 mm 由单一构造者拥有。
- `file/inode/address_space`：`f_op` 选择 get-area/mmap 能力，`f_mode` 表达读写权限，
  `f_mapping` 连接 inode 映射树，`f_path` 提供挂载 noexec，`i_mode` 决定文件类型，
  `i_mmap` 是反向映射树；file 临时/持久引用分别由 `fget/fput` 和 VMA 发布路径维护。
- `vm_special_mapping`：`name` 为长期借用名称，`pages` 为 NULL 终止页数组，`fault` 可替代
  pages，`mremap/close` 是可选生命周期通知；描述符及其指针目标必须长于 VMA。
- `vm_fault`：`vma/pgoff` 是 fault core 提供的借用 VMA 与页索引；`page` 在命中时接收
  `get_page` 后的引用并由 fault core 消费。
- `special_mapping_vmops`：`close/fault/mremap/name/may_split` 分别绑定上述五个回调，
  `access=NULL` 禁止 VVAR 等远程访问；静态只读、内核全寿命，VMA core 按回调上下文读取。
- `mmap_table[]` 每项字段：`procname` 是 `/proc/sys/vm` 名，`data` 指被发布整数，
  `maxlen` 是字节宽度，`mode` 是权限，`proc_handler` 执行整数 min/max 校验，
  `extra1/extra2` 是可选下/上界。四项分别描述 `max_map_count`、`legacy_va_layout`、
  `mmap_rnd_bits`、`mmap_rnd_compat_bits`；表静态只读，条件配置决定成员是否存在。
- `unmap_desc` 本文件仅设置 `mm_wr_locked=true`，表示退出拆页表阶段已持写锁；
  `vma_iterator/mmu_gather` 等其余对象只通过 helper 操作，无本文件直接字段访问。

### 枚举与 switch 项清单

- 本文件定义搜索范围 `^enum`：不存在命名枚举定义，无自有枚举项需要逐项解释。
- 文件映射 `MAP_TYPE`（用户 flags 产生，`do_mmap` 消费）：`MAP_SHARED` 为兼容模式，
  静默裁掉非 legacy 位后 fallthrough；`MAP_SHARED_VALIDATE` 严格拒绝未知位并建立共享语义；
  `MAP_PRIVATE` 建立 COW/私有语义并完成公共文件能力检查；default 返回 `-EINVAL`，mm 未变。
- 匿名 `MAP_TYPE`：`MAP_SHARED` 选择 shmem 且清 pgoff；`MAP_DROPPABLE` 由用户显式请求，
  设置易失、NORESERVE、WIPEONFORK、DONTDUMP 后 fallthrough 私有页索引；`MAP_PRIVATE`
  用虚拟页号建立合并索引；default 返回 `-EINVAL`。
- 热插拔 action（memory notifier 产生，`reserve_mem_notifier` 消费）：`MEM_ONLINE` 只重算
  仍像缺省值的预留；`MEM_OFFLINE` 只收缩超过当前空闲 KiB 的预留并记录日志；default
  不改状态；所有项返回 `NOTIFY_OK`，不改变热插拔主事务结果。
- `VM_FAULT_SIGBUS` 是 fault 结果位而非本文件枚举项：特殊页数组越界时产生，fault core
  消费并向访问者形成总线错误；命中页返回 0 并消费 `vmf->page` 引用。

### 其他实体与局部变量清单

- 全局/静态：`mmap_rnd_bits_{min,max}`/`mmap_rnd_bits` 和 compat 三项分别是 ASLR 下界、
  启动后上界及高频当前值；`ignore_rlimit_data` 由参数写、限额检查读；`stack_guard_gap`
  是字节数且启动期一次写；`sysctl_legacy_va_layout` 是布局选择；`special_mapping_vmops`
  与 `mmap_table` 为静态只读描述。配置宏控制实体是否编译，均无动态 ownership。
- 宏：`arch_mmap_check` 是无 arch hook 的无状态成功桩；`vma_expand_up/down` 按配置把支持
  方向接到真实扩展函数，另一方向固定 `-EFAULT`，使公共升级流程保持同一返回协议。
- 前端局部组：`newbrk/oldbrk/origbrk` 分别是页级新旧边界和精确回滚值；`next/brkvma`
  是锁期借用 VMA；`populate` 是锁外预取字节/布尔后置动作；`uf` 收集待解锁后通知事件。
  `file/inode/maxsize/pgoff/len` 的引用、单位和有效期已在相应函数头及阶段注释中逐项标出。
- 选址局部组：`vma/prev` 是 mmap_lock 期借用邻居；`info` 是栈上搜索约束；`mmap_end`
  是 arch 字节上界；`get_area` 是借用函数指针；`error/addr/index` 分别承载 errno、候选
  字节地址和 Maple Tree 游标，均不越过调用返回。
- 拆除/特殊映射局部组：`ret/error` 承载下层 errno，`file` 跨 remap 解锁窗口由引用稳定；
  `vm_flags` 是重验快照；`nr_accounted/count` 记录页数/条目数；`tlb/unmap/vmi` 仅在退出
  事务有效；`sm/pages/pgoff/page` 由 VMA 生命周期稳定，命中 `page` 引用转交 fault core。
- fork 局部组：`mpnt` 为父借用 VMA，`tmp` 为子独立 VMA；`retval` 是首个失败码，`charge`
  汇总尚需撤销的 overcommit 页，`uf` 在双锁外完成；循环内 `file/mapping` 在 file 引用和
  `i_mmap_rwsem` 下有效，`len/end` 分别是页数和页表复制字节终点。

### 路径、并发与生命周期清单

- mmap：参数/rlimit 快速拒绝 → arch/file 选址 → file/匿名类型验证 → overcommit 策略 →
  `mmap_region` 发布；发布前无 VMA 副作用，发布后 `populate` 和 userfaultfd 在锁外完成。
- brk：精确请求先暂存 → 页级不变快路 → 缩小先发布 brk 再拆 VMA → 增长检查邻居/限额并
  `do_brk_flags`；任一失败恢复 `origbrk`。`mmap_write_lock` 对抗并发映射和 fault 查树。
- munmap/remap：普通拆除由 `do_vmi_munmap` 在失败时保留写锁并重挂 VMA；旧 remap 在读锁
  取得 file 引用和 flags 快照，锁外 LSM，写锁后逐项重验，抵御解锁窗口的 VMA/file 竞态。
- exit：最后用户先 `mmu_notifier_release`，读锁拆 PTE，再设 OOM_SKIP；写锁下 Maple Tree
  对 RCU 读者不可达后释放页表和 VMA，最后归还承诺。不可达发布点后不能回滚为活 mm。
- special mapping：`insert_vm_struct` 前对象仅构造者拥有，失败直接 free；发布后 mm 树拥有，
  spec/ops/pages 必须长寿；fault 命中用 `get_page` 把页引用交 fault core。
- fork：父写锁后嵌套子写锁，防父拓扑变化且子尚不可见；每个 policy/anon_vma/file 引用
  在页表复制前取得。失败标签逆序释放当前项并清已建前缀、撤销 charge、设置
  `MMF_UNSTABLE`；成功解双锁后才完成 userfaultfd，调用者随后发布子 task。
- 未使用 RCU 屏障的查树 helper 均依赖外部 mmap_lock；`WRITE_ONCE(vm_page_prot)` 与无锁
  `remove_protection_ptes` 读取配对，`mt_clear_in_rcu` 是退出时对树读者的摘除边界。

### 英文注释、关联文件与抽查记录

- 英文注释：原文逐字保留；脚本逐单元检查 `untranslated=0`，许可证/作者元数据按规则豁免。
- `mm/vma.c`：读取 `do_vmi_munmap`、`__vm_munmap`、`__mmap_region/mmap_region`、
  `do_brk_flags`（1590-1668、2720-2885、3260-3310），确认拆除锁后置条件、发布/回滚点与
  brk 合并语义；现有学习注释“缺失”（密度 0、英文未翻译），建议按基线序号 155 后续补注。
- `mm/util.c`：读取 `vm_mmap_pgoff`（800-855），确认 LSM/fsnotify、写锁、uf/populate 顺序；
  现有学习注释“部分覆盖”（密度/间隔通过但仍有英文未翻译），建议后续复审英文邻接项。
- `kernel/fork.c`：读取 `dup_mm`（2478-2538），确认 `dup_mmap` 失败后 `mmput` 销毁且 uprobes
  收尾；现有学习注释“部分覆盖”（密度高但 max gap/英文门禁失败），非本 mm 清单范围。
- `include/linux/mm_types.h`：读取 `vm_special_mapping`（1705-1752），确认 pages/fault 互斥和
  回调字段；现有学习注释“缺失”，建议未来对该结构字段补注。
- `include/linux/mm.h`：读取 `vm_unmapped_area_info`（14820-14852），确认搜索字段及单位；
  现有学习注释“部分覆盖”（全文件仍有长空档/英文遗漏），建议相关声明区后续补注。
- 抽查 `do_mmap`：仅凭注释可复述“校验→选址→类型/权限→overcommit→发布→锁外后置”，
  能判断新增错误应在 `mmap_region` 前直接返回，发布后不能按纯校验路径回滚。
- 抽查 `exit_mmap`：可判断 OOM reaper 是竞态方，`MMF_OOM_SKIP` 必须在 PTE 已释放后发布，
  Maple Tree 不可达后才可在保留写锁时逐 VMA close/free。
- 抽查 `dup_mmap`：可判断父/子锁序不能互换、file 引用可跨锁使用、页表复制失败应进入
  当前 VMA cleanup 再清前缀，且 trace/发布只能发生在调用者看到成功之后。

### 修改安全与验证记录

- `git diff --check -- mm/mmap.c`：通过；`git diff --numstat` 为 `193 0`。
- `-U0` 新增行分类：除空行外全部为 `/*`、`*`、`*/` 或 `//` 注释；word-diff 无删除，
  因而零代码改动、零原注释改写。
- 增量 checkpatch：0 errors、79 warnings；warnings 全为中文按字节计宽导致的行长提示。
  全文件 checkpatch 的 4 errors/15 warnings 位于原有代码缩进、宏格式、旧 API 等未修改行，
  按“只加注释”约束不修代码。
- `.config` 不存在，未执行目标编译；这是唯一未执行验证。已按本文第 17 章完成强制验收。

## `mm/memory_hotplug.c` 返工中（2026-09-21）

- 基线：2436 物理行，`code=1451 comments=641 chinese=0 density=0 max_gap=1451`；
  当前返工源码完整差异 `+744/-0`。
- 机械门禁：`code=1451 comments=1385 chinese=604 density=0.416 max_gap=10`，
  `untranslated=0`，退出码 0；命令含 `--require-english-translation`。
- 最终词法索引为 80 个函数定义（78 个名称，两个条件配置各有双定义），修正了初探只按
  非指针返回签名统计所得的 69。下表的调用场景结论可复用，但“结论=通过”暂时撤回：需逐项把
  业务背景、参数、返回和注意事项明确写进声明前专属函数头，再重新生成行号并验收。

### `mm/memory_hotplug.c` 函数与使用场景清单

“头/声明/尾”依次为专属中文函数头范围、声明首行和函数体右花括号行。

|函数/配置|头/声明/尾|具体入口、触发条件与返回后的下一步|结论|
|---|---|---|:--:|
|`memory_block_memmap_size`|62-62/63/66|self-host 能力检查/altmap 规划求单 block 的 `struct page` 字节数；结果继续换算保留页|通过|
|`memory_block_memmap_on_memory_pages`|68-71/72/87|altmap 创建求保留页；FORCE 补齐 pageblock，返回值写 `altmap.free`|通过|
|`set_memmap_mode`|90-94/95/122|模块参数框架解析 Y/N/force；0 发布三态，errno 保持旧值|通过|
|`get_memmap_mode`|124-124/125/132|参数读取把三态编码为文本；返回写入字符数|通过|
|`mhp_memmap_on_memory`/配置启用|145-145/146/149|添加/移除判断运行期开关；ENABLE/FORCE 为 true|通过|
|`mhp_memmap_on_memory`/空桩|151-151/152/155|未配置 self-host 时让公共路径编译为 false|通过|
|`set_online_policy`|171-171/172/180|可写模块参数匹配策略字符串；成功发布数组下标|通过|
|`get_online_policy`|182-182/183/186|参数读取稳定合法下标并输出策略名|通过|
|`get_online_mems`|249-249/250/253|回调注册等轻量读者阻挡全局热插拔写事务；返回时持读锁|通过|
|`put_online_mems`|255-255/256/259|上述读侧退出；释放后写事务可继续|通过|
|`mhp_get_default_online_type`|264-269/270/290|add 后自动上线或 sysfs 默认查询；首次按 Kconfig 缓存 `MMOP_*`|通过|
|`mhp_set_default_online_type`|292-292/293/296|架构/启动管理路径覆盖默认 `MMOP_*`；无返回值|通过|
|`setup_memhp_default_state`|298-298/299/307|early setup 消费 `memhp_default_state=`；合法串覆盖默认值|通过|
|`mem_hotplug_begin`|310-310/311/315|add/remove 事务先稳定 CPU 再独占内存拓扑；必须转 `done`|通过|
|`mem_hotplug_done`|317-317/318/322|按逆序释放 hotplug/CPU 锁并结束事务|通过|
|`register_memory_resource`|326-331/332/371|普通或驱动 add 在建映射前声明字节区间；成功转交 iomem resource，失败 `ERR_PTR`|通过|
|`release_memory_resource`|373-376/377/383|add 失败撤 iomem 所有权并释放对象；NULL 快路|通过|
|`check_pfn_span`|385-388/389/403|底层 arch 增删前验证 subsection 页粒度；0 或无副作用 `-EINVAL`|通过|
|`pfn_to_online_page`|405-412/413/459|PFN walker 查询可安全使用的在线页；普通快路、混合 DEVICE 慢路，返回借用 page/NULL|通过|
|`__add_pages`|462-465/466/513|arch add 逐 section 建 sparse/vmemmap；失败保留成功前缀供上层 remove|通过|
|`find_smallest_section_pfn`|515-519/520/540|zone 首端被移除时找同 nid/zone 的最小在线 PFN；0 表示空|通过|
|`find_biggest_section_pfn`|542-546/547/571|zone 尾端被移除时逆向找最大在线 PFN；0 表示空|通过|
|`shrink_zone_span`|573-576/577/619|offline 后仅在删除首/尾时收缩 zone span；中间洞保持跨度|通过|
|`update_pgdat_span`|621-624/625/654|zone 收缩后汇总节点最外边界；void 且不改 present 计数|通过|
|`remove_pfn_range_from_zone`|656-659/660/697|offline/元数据拆除后 poison page 并收缩普通 zone；DEVICE 仅 poison|通过|
|`__remove_pages`|699-713/714/734|arch remove 在页已 offline 后逐 section 拆 sparse 映射；void/WARN 契约|通过|
|`set_online_page_callback`|736-739/740/757|外部模块仅在 generic 生效时注册长寿回调；0 或 `-EINVAL`|通过|
|`restore_online_page_callback`|760-760/761/778|原注册者按指针匹配恢复 generic；锁序与注册一致|通过|
|`generic_online_page`|781-783/784/787|`online_pages_range` 默认分派，把 order 页所有权交 buddy|通过|
|`online_pages_range`|790-793/794/842|上线提交按最大对齐 order 调回调，最后发布 section online|通过|
|`resize_zone_range`|844-844/845/855|move 前扩展 zone 页跨度；不修改 present/managed|通过|
|`resize_pgdat_range`|857-857/858/869|与 zone 对称扩展节点跨度；调用者持 hotplug 写锁|通过|
|`section_taint_zone_device`/DEVICE|872-872/873/878|非 section 对齐 DEVICE 添加标记混合 section，启用 PFN 查询慢路|通过|
|`section_taint_zone_device`/空桩|880-880/881/883|无 ZONE_DEVICE 配置保持共同调用点且无状态|通过|
|`move_pfn_range_to_zone`|886-896/897/941|online/DEVICE add 先扩 span 再初始化 page/migratetype；返回仍 `PageOffline`|通过|
|`auto_movable_stats_account_zone`|950-950/951/969|策略判断聚合单 zone 的 early-kernel/MOVABLE 页；CMA 归 MOVABLE|通过|
|`auto_movable_stats_account_group`|977-977/978/1006|动态组 walker 计算其他组预算缺口；0 继续遍历|通过|
|`auto_movable_can_online_movable`|1008-1011/1012/1068|全局或单 nid 预测加入候选页后的比例；bool 决定 zone 选择|通过|
|`default_kernel_zone_for_pfn`|1070-1076/1077/1092|显式/回退 kernel 上线继承相交 DMA 类 zone，否则 NORMAL|通过|
|`auto_movable_zone_for_pfn`|1094-1149/1150/1216|`MMOP_ONLINE` 自动策略按整组/单元和两级预算选 MOVABLE，否则 kernel|通过|
|`default_zone_for_pfn`|1218-1218/1219/1243|contig 策略唯一相交时继承，歧义按 `movable_node` 回退|通过|
|`zone_for_pfn_range`|1245-1248/1249/1264|`memory_block_online` 的总分派；显式 mmop 优先，再策略选择|通过|
|`adjust_present_page_count`|1266-1272/1273/1294|block online/offline 同步 zone/node/group 页计数；带符号增量、void|通过|
|`mhp_init_memmap_on_memory`|1296-1299/1300/1332|self-host block 上线先建 shadow/绑定 UNMOVABLE 并发布元数据页；0/errno|通过|
|`mhp_deinit_memmap_on_memory`|1334-1337/1338/1358|普通页下线后撤元数据 section/zone/shadow；无失败返回|通过|
|`online_pages`|1360-1365/1366/1494|device online 在写锁下通知、绑定、发布 buddy/统计/节点；拒绝则取消并撤 zone|通过|
|`hotadd_init_pgdat`|1496-1497/1498/1523|首次添加或重用离线节点时补齐 pgdat/空 zone/zonelist|通过|
|`__try_online_node`|1525-1537/1538/1562|CPU/add 路径初始化离线节点；返回 0 已在线、1 新建、`-ENOMEM`|通过|
|`try_online_node`|1564-1567/1568/1576|需立即注册节点的公共包装自行取得 hotplug 写锁|通过|
|`check_hotplug_memory_range`|1578-1578/1579/1591|用户增删入口校验非零且按 memory block 字节对齐|通过|
|`online_memory_block`|1593-1593/1594/1598|add 成功后的 block walker 写默认类型并触发设备上线|通过|
|`arch_supports_memmap_on_memory`/缺省|1601-1601/1602/1611|无架构 hook 时要求 vmemmap 整 PMD；bool 决定 self-host|通过|
|`mhp_supports_memmap_on_memory`|1614-1614/1615/1667|add 前综合开关、页/PMD/pageblock 对齐和剩余容量|通过|
|`altmap_free`|1670-1670/1671/1675|当前块 arch 映射拆完后校验 alloc 清零并 kfree|通过|
|`remove_memory_blocks_and_altmaps`|1677-1677/1678/1707|self-host remove 逐块转移 altmap、删设备/映射并释放|通过|
|`create_altmaps_and_memory_blocks`|1709-1712/1713/1764|self-host add 逐块建 altmap/arch/device；失败撤当前与成功前缀|通过|
|`add_memory_resource`|1766-1773/1774/1893|持 device 锁执行 node/memblock/arch/device 事务；解写锁后可选 merge/online|通过|
|`__add_memory`|1895-1896/1897/1911|内部普通 RAM 入口先注册 iomem；add 失败释放 resource|通过|
|`add_memory`|1913-1913/1914/1924|导出普通 RAM 入口用 device 锁串行完整添加|通过|
|`add_memory_driver_managed`|1927-1951/1952/1981|驱动 RAM 校验专用名称并添加带 DRIVER_MANAGED 的 resource|通过|
|`arch_get_mappable_range`/弱定义|1984-1999/2000/2007|架构未覆盖时返回全物理闭区间；通用层再钳制|通过|
|`mhp_get_pluggable_range`|2009-2009/2010/2029|按需 direct map 取得最终可插拔闭区间；按值返回|通过|
|`mhp_range_allowed`|2032-2032/2033/2045|resource/sparse add 拒绝回绕或越界半开范围；bool+告警|通过|
|`scan_movable_pages`|2048-2060/2061/2112|offline 扫描下个可迁页；0 写输出、`-ENOENT` 完成、`-EBUSY` 阻断|通过|
|`do_migrate_range`|2114-2117/2118/2220|offline 批量隔离并同步迁移 folio；失败项放回、外层重扫|通过|
|`cmdline_parse_movable_node`|2222-2222/2223/2227|early `movable_node` 发布策略布尔值|通过|
|`count_system_ram_pages_cb`|2230-2230/2231/2238|RAM walker 累加页数，用于下线前排除空洞|通过|
|`offline_pages`|2240-2244/2245/2484|device offline 在写锁下隔离、通知、迁移、摘 buddy/统计；提交前失败全回滚|通过|
|`check_memblock_offlined_cb`|2486-2486/2487/2504|remove walker 记录 nid 并以 `-EBUSY` 拒绝在线 block|通过|
|`count_memory_range_altmaps_cb`|2506-2506/2507/2516|remove 前统计 self-hosted block；不转移 altmap ownership|通过|
|`check_cpu_on_node`|2518-2518/2519/2534|注销 node 前检查 present CPU；0/`-EBUSY`|通过|
|`check_no_memblock_for_node_cb`|2536-2536/2537/2549|发现仍链接 nid 的离线 block 返回 `-EEXIST` 保留节点|通过|
|`try_offline_node`|2551-2561/2562/2595|device 锁下确认无 span/block/CPU 后注销空节点；void 早退表示保留|通过|
|`memory_blocks_have_altmaps`|2598-2598/2599/2619|选择普通或逐块 self-host remove；0/1/混合布局 `-EINVAL`|通过|
|`try_remove_memory`|2621-2624/2625/2684|验证全 OFFLINE 后拆 firmware/device/arch/memblock/iomem 并尝试下线 node|通过|
|`__remove_memory`|2686-2695/2696/2706|已持 device 锁的强契约 void 包装；失败触发 BUG|通过|
|`remove_memory`|2708-2712/2713/2722|可失败导出入口自行持 device 锁；返回 0/errno|通过|
|`try_offline_memory_block`|2725-2728/2729/2760|组合移除第一遍保存原 zone 类型并下线；负 errno 停止|通过|
|`try_reonline_memory_block`|2762-2762/2763/2781|组合移除失败时按保存类型尽力重上线所有已处理块|通过|
|`offline_and_remove_memory`|2783-2790/2791/2852|逻辑拔除设备先逐块下线再整段移除；任一步失败按数组回滚|通过|

### 结构体字段清单

- 本地 `auto_movable_stats`：`kernel_early_pages`、`movable_pages` 均以页计，分别是不可轻易
  热拔的 kernel 分母和 ZONE_MOVABLE+CMA 分子；栈上零初始化，仅在 hotplug 串行期聚合。
- 本地 `auto_movable_group_stats`：`movable_pages` 是其他动态组已用分子，
  `req_kernel_early_pages` 是这些组尚需保留的分母页；walker 写、策略函数读，生命周期仅一次判断。
- `kernel_param/kernel_param_ops`：`kp->arg` 借用静态参数地址；`set/get` 绑定解析与格式化回调，
  参数核心同步调用，回调不取得 ownership。
- `resource`：`start/end` 是包含式字节边界，`name` 是长寿名称，`flags` 含 SYSTEM_RAM、BUSY 和
  可选 DRIVER_MANAGED；注册后 iomem 树拥有，add 失败由包装摘除/free，merge 后旧指针可失效。
- `mhp_params`：`pgprot` 是必需页表保护，`altmap`/`pgmap` 是可空借用；栈对象只覆盖 arch 调用，
  self-host 成功时堆 `altmap` 转给 `memory_block`。
- `vmem_altmap`：`base_pfn/end_pfn` 是包含式页边界，`free` 是头部保留页，`alloc` 是 vmemmap
  已消费页；arch add/remove 修改 `alloc`，block 生命周期末必须回到 0 后释放。
- `zone/pglist_data`：`zone_start_pfn/spanned_pages` 与 `node_start_pfn/node_spanned_pages` 是页跨度；
  `present_pages/present_early_pages/cma_pages/node_present_pages` 是页计数；`nr_isolate_pageblock`
  是 pageblock 个数，`node_zones/node_id/zone_pgdat` 连接层次。拓扑由 hotplug 写锁串行，
  isolate 计数另在 `zone->lock` 下修改。
- `memory_group`：`nid` 定目标节点；`present_kernel_pages/present_movable_pages` 是页计数；
  `is_dynamic` 选择 `s.max_pages`（静态组总上限）或 `d.unit_pages`（动态协调单元页数）。注册表持有，
  add/online 只借用，修改由 device/hotplug 锁串行。
- `memory_block`：`start_section_nr` 定物理起点，`state` 是设备锁串行状态，`online_type` 保存
  `MMOP_*` 请求，`nid/group/zone` 是归属，`dev` 驱动设备状态机，`altmap` 持有 self-host 描述符；
  `memory_block_get/put` 稳定借用，移除在删设备前清并转移 altmap。
- `memory_notify/node_notify`：`start_pfn/nr_pages` 是页单位同步通知快照，`nid` 以
  `NUMA_NO_NODE` 表示非首/末内存事务；均为栈对象，仅 blocking notifier 调用期借用。
- `range`：`start/end` 是包含式物理字节边界；按值返回，无 ownership；`start>end` 不作为有效范围。
- `migration_target_control`：`nmask` 借用栈上节点掩码，`gfp_mask` 要求可移动且允许重试失败，
  `reason=MR_MEMORY_HOTPLUG`，`nid` 是单 zone 源节点；仅 `migrate_pages` 同步调用期有效。
- `page/folio/mem_section`：page 的 Offline/VmemmapSelfHosted/zone 归属描述发布阶段；folio 的
  `lru` 在隔离 source 链表中转移，`page` 是嵌入首页；`section_mem_map` 的 DEVICE taint 使混合
  section 查询走慢路。page/section 受 hotplug 状态保护，folio 另以引用、LRU 隔离和 folio 锁稳定。

### 枚举与状态项清单

- 本地 memmap 三态（参数产生，altmap 规划消费）：`DISABLE` 使用普通元数据；`ENABLE` 仅对齐
  合格时 self-host；`FORCE` 补齐 pageblock 并可能浪费页。后两项使查询为 true。
- 本地 online policy：`CONTIG_ZONES` 继承唯一相交 zone、歧义通常 kernel；`AUTO_MOVABLE`
  启用全局/NUMA 比例决策。参数 set 产生，`zone_for_pfn_range` 消费。
- `enum mmop`：`MMOP_OFFLINE` 只创建设备/也是回滚数组哨兵；`MMOP_ONLINE` 交策略自动选择；
  `MMOP_ONLINE_KERNEL` 强制 kernel zone；`MMOP_ONLINE_MOVABLE` 强制 ZONE_MOVABLE。Kconfig、
  启动参数、sysfs 或回滚代码产生，block online 分派消费并改变是否上线及 zone 归属。
- memory notifier 状态：`MEM_GOING_ONLINE`/`MEM_GOING_OFFLINE` 是可否决准备通知；
  `MEM_CANCEL_ONLINE`/`MEM_CANCEL_OFFLINE` 撤销已发准备通知；`MEM_ONLINE`/`MEM_OFFLINE`
  表示提交完成。产生者是 online/offline 事务，消费者是 blocking notifier 链。
- node notifier 状态：`NODE_ADDING_FIRST_MEMORY` 与 `NODE_REMOVING_LAST_MEMORY` 可否决；
  对应 `NODE_CANCEL_*` 回滚，`NODE_ADDED_FIRST_MEMORY`/`NODE_REMOVED_LAST_MEMORY` 在节点位和
  后台线程稳定后完成通知。
- zone/migration 状态：`ZONE_MOVABLE` 是可移动目标，`ZONE_NORMAL` 及更低项属于 kernel 候选；
  `MIGRATE_MOVABLE` 用于普通热添加页，`MIGRATE_UNMOVABLE` 用于 self-hosted 元数据页；
  `PB_ISOLATE_MODE_MEM_OFFLINE` 阻止新分配进入下线范围，`MR_MEMORY_HOTPLUG` 标记迁移原因。
- memblock 标志：`MEMBLOCK_NONE` 为普通 RAM，`MEMBLOCK_DRIVER_MANAGED` 随 resource 标志产生，
  使保留 memblock 的架构仍能识别驱动管理范围。本文件没有 `switch/case`。

### 其他实体与局部变量清单

- 参数/全局：`memmap_mode` 是只读启动三态；`online_policy`、`auto_movable_ratio` 和 NUMA 下的
  `auto_movable_numa_aware` 是读多写少策略；`movable_node_enabled` 由 early 参数一次发布；
  `mhp_default_online_type=-1` 是未解析哨兵；`max_mem_size` 仅在启动未 RUNNING 时限制 add。
- 锁/回调：`online_page_callback` 是长寿借用函数指针，`online_page_callback_lock` 串行注册者；
  `mem_hotplug_lock` 的 percpu 读侧保护回调更换，写侧串行拓扑事务并与 CPU read lock 固定锁序。
- 静态描述：`online_policy_to_str[]` 的下标与本地枚举相同；两个 `kernel_param_ops` 静态只读；
  `migrate_rs` 是跨调用限速器。条件配置决定空桩/真实函数是否存在。
- 范围局部：`pfn/start_pfn/end_pfn/nr_pages/cur_nr_pages` 均以页计，`start/size/cur_start`
  在 resource API 中以字节计；`end` 先用于回绕检查再转包含式 `end-1`。
- 事务局部：`ret/rc/err` 保存首个下层结果，`reason` 只在 offline 失败诊断前有效；`new_node`
  决定是否撤本事务创建的节点；`need_zonelists_rebuild` 标记原空 zone；`node_arg.nid` 同时是通知哨兵。
- ownership 局部：`res` 在成功时转 iomem 树、失败释放；`params` 是栈上借用载体；`altmap`
  在 self-host 成功后归 block、移除时转回局部释放；`mem` 由 get/put 稳定；`page/zone/group/pgdat`
  都是相应锁期借用。
- 迁移/回滚局部：`source` 拥有已隔离 folio 链表，`nmask/mtc` 只覆盖同步迁移；`online_types`
  每 block 一字节，`tmp` 是 walker 游标，OFFLINE 槽位表示原离线或尚未处理，设备锁外不再有效。

### 路径、并发与生命周期清单

- add：device 锁 → iomem resource → hotplug 写锁/CPU 稳定 → 可选 memblock → node → arch sparse
  映射 → memory_block → node/firmware 链接 → 解写锁 → 可选 resource merge/设备上线；错误标签按反序撤销。
- online：写锁下选 zone → `PageOffline` 绑定 → 首内存/node 与 memory notifier 准备 → isolate 计数/
  pageset → buddy+section+present 发布 → node 位/zonelist → undo isolate（可分配提交点）→ 完成通知。
- offline：写锁下排除洞/多 zone → 禁 PCP/LRU → isolate → 准备通知 → 反复扫描迁移/hugetlb
  溶解/隔离验证 → `__offline_isolated_pages` 不可逆提交 → 扣统计和节点位 → 完成通知/收缩 span；
  提交前失败解除隔离、发 CANCEL 并恢复缓存。
- remove：device 锁先证明所有 block 离线，hotplug 写锁内按是否有 altmap 拆设备和 arch 映射，
  再撤 memblock/iomem 并尝试注销空 node；组合 API 保存每块原 zone 类型，失败尽力重上线已处理前缀。
- self-host：每块堆 altmap 从 add arch 调用转交 memory_block；online 时元数据页先变 UNMOVABLE/self-hosted，
  offline 时普通页先摘除再撤元数据状态；remove 在删设备前清 block 指针、转移 altmap 并最终 free。
- 竞态：`device_hotplug_lock` 对抗 sysfs online/offline 与设备增删；hotplug 写锁对抗 PFN/zone/node
  拓扑读者，且固定 CPU 拓扑；`zone->lock` 保护 isolate 计数；folio 引用/LRU 隔离/folio 锁保护迁移，
  最终 `test_pages_isolated` 容忍无引用 hugetlb 预扫的竞态误判。

### 英文注释、关联文件与抽查记录

- 英文注释：原文逐字保留；脚本逐单元检查 `untranslated=0`，许可证元数据按规则豁免。
- `drivers/base/memory.c`：读取 `memory_block_online/offline`（210-310），确认 zone 分派、self-host
  元数据先后顺序及写锁边界；现有学习注释“部分覆盖”，建议后续补齐设备状态机与 sysfs action。
- `include/linux/memory_hotplug.h`：读取 `enum mmop`、`mhp_params` 及公共声明（20-155、266-317），
  确认枚举/字段契约；现有学习注释“缺失”，建议补字段、flags 和声明使用场景。
- `include/linux/memory.h`：读取 `memory_group`、`memory_block` 和 notifier 字段（27-115），确认单位、
  ownership 与设备锁同步；现有学习注释“部分覆盖”，建议补上述两结构及状态枚举。
- 架构调用点：搜索 `arch/{arm64,loongarch,s390,riscv,powerpc,x86}/mm` 的 `__add_pages/
  __remove_pages`，确认底层 helper 由 arch add/remove 包装消费；arm64 为“部分覆盖”，其余为“缺失”，
  建议未来仅在相应架构专题补调用契约，不扩展本任务。
- `mm/memremap.c`：定位 `__remove_pages` 调用（189），确认 ZONE_DEVICE 撤映射也复用底层 remove；
  现有学习注释“部分覆盖”，建议后续复审该调用的 pgmap/altmap 生命周期。
- `mm/mm_init.c`：定位 hotplug `PageOffline`/present 说明（1770、2805-2852），确认 move 与 online 的
  状态边界；相关区域学习注释“充分”，无需为本文件扩展。
- 抽查 `add_memory_resource`：仅凭注释可复述“资源已声明→memblock/node→arch→device→链接→解锁
  后 merge/online”；新增失败若已建 node 应进 `error`，仅有 memblock 则进 `error_memblock_remove`，
  resource merge 必须晚于不可回滚成功点。
- 抽查 `online_pages`：可判断 `undo_isolate_page_range` 是新页真正可分配的发布点，通知器拒绝发生在
  此前，失败只需 CANCEL 和撤 zone；首内存 node 位、zonelist、kswapd/kcompactd 顺序不能前移。
- 抽查 `offline_pages`：可复述禁 PCP/LRU→isolate→通知→迁移→隔离验证→buddy 摘除→统计/node；
  可判断 `__offline_isolated_pages` 后不能走普通回滚，N_NORMAL_MEMORY 必须先于 N_MEMORY 清除。
- 抽查 `offline_and_remove_memory`：可判断设备锁稳定两遍 block 顺序，保存数组只标记成功下线项，
  remove 失败按原 KERNEL/MOVABLE 类型尽力恢复，数组必须在回调全部结束后释放。

### 修改安全与验证记录

- `git diff --check -- mm/memory_hotplug.c`：通过；`git diff --numstat` 为 `744 0`。
- `-U0` 新增行分类：除空行外全部为 `/*`、`*`、`*/` 或 `//` 注释；新增非注释行 0，
  numstat 删除 0，故零代码改动、零原注释改写。
- 增量 checkpatch：0 errors、378 warnings；全部为中文按字节计宽产生的 LONG_LINE_COMMENT。
  全文件 checkpatch 的 4 errors/7 warnings 位于原有文件名、字符串拆分、global false、缩进和函数名
  字符串等未修改代码，按“只加注释”约束不修代码。
- `.config` 不存在，未执行目标编译。机械门禁已通过；人工函数头验收返工中，尚不能声称
  “已按本文第 17 章完成强制验收”。
