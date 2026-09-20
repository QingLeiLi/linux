# mm 学习注释进度

规则：单文件按 `doc/linux-kernel-source-learning-methodology.md` 第 17 章闭环；机械门禁同时启用 `--require-english-translation`；人工验收必须逐项保存函数使用场景、结构体字段释义和枚举解释三类清单；只加注释；`[~]` 未闭环时不开始下一文件。恢复任务先读本文件和 `todo/mm-learning-comments-baseline.md`，目标未变更时复用探查结论。

## 筛选

- 2026-08-27：扫描 190 个 `.c/.h`；密度门禁通过 0，未通过 190。
- 暂不重复：已有中文学习注释的 18 个文件（后续仅复审，不作为首轮目标）。
  `backing-dev.c`、`early_ioremap.c`、`filemap.c`、`hugetlb.c`、`hugetlb_cma.c`、`init-mm.c`、
  `memcontrol.c`、`mempolicy.c`、`mm_init.c`、`rmap.c`、`slab_common.c`、`slub.c`、`vmalloc.c`、
  `vmstat.c`、`kasan/hw_tags.c`、`kasan/sw_tags.c`、`kfence/core.c`、`kfence/report.c`。

## 进度

- [x] `mm/nommu.c`
  - 2026-09-20 恢复检查：源码 1903 行、有效代码 1201 行；初查简单正则得到 59 个函数，最终按
    配置展开、宏入口和多行签名校正为 76 个定义。当前尚无中文注释，机械门禁为
    `density=0.000`、`max_gap=1201`、
    120 组英文注释待翻译。已完成方法论复读和首次全文件建图，开始按“文件总览与 vmalloc 兼容层
    → NOMMU region/VMA → mmap → munmap/mremap → 远程访问与收尾初始化”分区补注，状态仍为未闭环。
  - 2026-09-20 检查点 1：已完成文件总览、全局实体、vmalloc 兼容/不支持空桩、`brk`、region 树与
    VMA 基础协议，并补到 `validate_mmap_request()`、`determine_vm_flags()`、共享/私有映射后端。
    当前源码仅新增注释 462 行、删除 0 行，`git diff --check` 通过；机械扫描已达
    `density=0.286`，但未处理区使 `max_gap=604`，尚有 29 组英文注释待翻译，故保持 `[~]`。
  - 2026-09-20 第 17 章函数清单（`H` 为紧邻中文契约、`D` 为声明至函数体末行，均逐项核对
    业务背景/入参/出参返回/注意事项并通过）：
    - vmalloc 兼容层：`kobjsize H93-103/D104-151`、`vfree H153-159/D160-163`、
      `__vmalloc_noprof H166-172/D173-184`、`vrealloc_node_align_noprof H187-194/D195-199`、
      `__vmalloc_node_range_noprof H201-208/D209-215`、`__vmalloc_node_noprof H217-223/D224-228`、
      `__vmalloc_user_flags H230-237/D238-256`、`vmalloc_user_noprof H258-263/D264-267`、
      `vmalloc_to_page H270-275/D276-279`、`vmalloc_to_pfn H282-287/D288-291`、
      `vread_iter H294-300/D301-309`、`vmalloc_noprof H322-328/D329-332`、
      `vmalloc_huge_node_noprof H348-354/D355-358`、`vzalloc_noprof H372-377/D378-381`、
      `vmalloc_node_noprof H395-401/D402-405`、`vzalloc_node_noprof H420-425/D426-429`、
      `vmalloc_32_noprof H439-444/D445-448`、`vmalloc_32_user_noprof H461-468/D469-477`。
    - MMU 专用接口空桩：`vmap H480-485/D486-490`、`vunmap H493-497/D498-501`、
      `vm_map_ram H504-508/D509-513`、`vm_unmap_ram H516-520/D521-524`、
      `vm_unmap_aliases H527-531/D532-534`、`free_vm_area H537-541/D542-545`、
      `vm_insert_page H548-552/D553-557`、`vm_insert_pages H560-564/D565-569`、
      `vm_map_pages H572-576/D577-581`、`vm_map_pages_zero H584-588/D589-593`。
    - 初始化与 region/VMA：`sys_brk H603-613/D614-641`、`mmap_init H666-672/D673-684`、
      debug `validate_nommu_regions H690-695/D697-723`、非 debug 空桩
      `validate_nommu_regions H725-729/D730-732`、`add_nommu_region H738-745/D746-776`、
      `delete_nommu_region H781-786/D787-794`、`free_page_series H799-805/D806-815`、
      `__put_nommu_region H823-830/D831-859`、`put_nommu_region H864-869/D870-874`、
      `setup_vma_to_mm H876-882/D883-898`、`cleanup_vma_from_mm H900-905/D906-921`、
      `delete_vma_from_mm H926-932/D933-950`、`delete_vma H954-960/D961-969`、
      `find_vma_intersection H971-977/D978-986`、`find_vma H993-1000/D1001-1006`、
      `expand_stack_locked H1013-1018/D1019-1022`、`expand_stack H1024-1030/D1031-1035`、
      `find_vma_exact H1041-1047/D1048-1066`。
    - mmap 创建：`validate_mmap_request H1072-1083/D1084-1264`、
      `determine_vm_flags H1270-1277/D1278-1325`、`do_mmap_shared_file H1331-1338/D1339-1357`、
      `do_mmap_private H1362-1370/D1371-1477`、`do_mmap H1482-1493/D1494-1764`、
      `ksys_mmap_pgoff H1766-1772/D1773-1797`、`sys_mmap_pgoff H1799-1805/D1806-1811`、
      条件入口 `sys_old_mmap H1828-1834/D1835-1847`。
    - 解除/调整映射：`split_vma H1854-1862/D1863-1947`、
      `vmi_shrink_vma H1953-1959/D1960-1999`、`do_munmap H2006-2013/D2014-2082`、
      `vm_munmap H2084-2088/D2089-2098`、`sys_munmap H2101-2105/D2106-2109`、
      `exit_mmap H2114-2120/D2121-2145`、`do_mremap H2157-2165/D2166-2204`、
      `sys_mremap H2206-2211/D2212-2222`、`remap_pfn_range H2224-2230/D2231-2239`、
      `vm_iomap_memory H2242-2248/D2249-2257`、`remap_vmalloc_range H2260-2266/D2267-2280`、
      `filemap_fault H2283-2287/D2288-2292`、`filemap_map_pages H2295-2299/D2300-2305`。
    - 远程访问与收尾：`__access_remote_vm H2308-2315/D2316-2354`、
      `access_remote_vm H2366-2373/D2374-2378`、`access_process_vm H2384-2391/D2392-2411`、
      `__copy_remote_vm_str H2419-2425/D2426-2467`、`copy_remote_vm_str H2483-2491/D2492-2514`、
      `nommu_shrink_inode_mappings H2529-2538/D2539-2599`、
      `init_user_reserve H2611-2617/D2618-2627`、`init_admin_reserve H2640-2646/D2647-2656`、
      `dup_mmap H2659-2665/D2666-2672`。
  - 函数使用场景清单（逐函数，分号后为触发场景与返回后的下一步）：
    - `kobjsize`：容量诊断区分 slab/VMA/folio，调用者得到近似上界；`vfree`：释放 NOMMU vmalloc
      缓冲区后地址失效；`__vmalloc_noprof`：带 GFP 的通用后端返回连续对象；
      `vrealloc_node_align_noprof`：调整已有对象后沿 krealloc 成败契约继续；
      `__vmalloc_node_range_noprof`：区间版调用在 NOMMU 忽略选址后取得对象；
      `__vmalloc_node_noprof`：节点版调用忽略节点后取得对象；`__vmalloc_user_flags`：用户映射缓冲区
      分配后标记 `VM_USERMAP`；`vmalloc_user_noprof`：驱动申请零填充用户缓冲区后交给 remap；
      `vmalloc_to_page`：地址转借用 page 后由查询者继续；`vmalloc_to_pfn`：地址转 PFN 后交给物理接口；
      `vread_iter`：proc/kcore 类读取把线性区复制并推进迭代器；`vmalloc_noprof`：普通可睡眠分配；
      `vmalloc_huge_node_noprof`：大页/节点 API 在 NOMMU 退化；`vzalloc_noprof`：取得零填充对象；
      `vmalloc_node_noprof`/`vzalloc_node_noprof`：节点调用分别取得普通/零填充对象；
      `vmalloc_32_noprof`/`vmalloc_32_user_noprof`：旧 32 位寻址调用分别取得内核/用户映射对象。
    - `vmap`/`vunmap`/`vm_map_ram`/`vm_unmap_ram`/`free_vm_area`/`filemap_fault`/
      `filemap_map_pages`：MMU 专属调用误入时 BUG，要求调用者修正配置路径；`vm_unmap_aliases`：
      通用刷新入口在 NOMMU 安全空操作后返回；`vm_insert_page`/`vm_insert_pages`/`vm_map_pages`/
      `vm_map_pages_zero`：页表型驱动映射固定 `-EINVAL`，调用者改走直接映射。
    - `sys_brk`：用户 malloc 扩缩固定堆区后获得实际 break；`mmap_init`：启动期建立计数/slab/sysctl/VMA；
      两个 `validate_nommu_regions`：region 修改前后按 debug 配置校验或空操作；`add_nommu_region`：
      新 mmap 后发布共享 region；`delete_nommu_region`：末引用/改键前摘除；`free_page_series`：
      私有映射回收页并更新统计；`__put_nommu_region`/`put_nommu_region`：VMA 销毁消费 region 引用，
      末引用释放 file/页/slab；`setup_vma_to_mm`：发布前绑定 mm/i_mmap；`cleanup_vma_from_mm`：
      删除前撤销 map_count/i_mmap；`delete_vma_from_mm`：munmap 预分配后从 Maple Tree 摘除；
      `delete_vma`：摘除后 close 并最终释放；`find_vma_intersection`：区间冲突查找；`find_vma`：
      锁内按地址装载实际包含它的 VMA；`expand_stack_locked`/`expand_stack`：NOMMU fault 扩栈分别
      返回 `-ENOMEM`/解锁后 NULL；`find_vma_exact`：mremap 校验完整单一 VMA。
    - `validate_mmap_request`：mmap 分配前筛选类型/权限/能力/LSM，成功交给 flags 计算；
      `determine_vm_flags`：将验证结果固化为 VMA 权限后交给构造；`do_mmap_shared_file`：
      MAP_SHARED 让文件回调固定后备，失败不私有化；`do_mmap_private`：私有文件/匿名映射优先 overlay，
      否则复制并填充连续页；`do_mmap`：在 `vm_mmap_pgoff` 写锁内共享/构造/publish VMA，成功返回地址；
      `ksys_mmap_pgoff`：系统调用审计并稳定 fd，随后归还 file 引用；`sys_mmap_pgoff`：现代 ABI 转包装；
      `sys_old_mmap`：旧结构 ABI 复制快照、校验字节偏移后转页偏移包装。
    - `split_vma`：匿名中段 munmap 前切边界并发布第二 VMA；`vmi_shrink_vma`：端部 munmap 收缩索引/
      region 并释放页；`do_munmap`：写锁下整体删除文件 VMA或拆/缩匿名 VMA；`vm_munmap`：内核调用
      加锁包装；`sys_munmap`：用户 ABI 转包装；`exit_mmap`：mm teardown 清空全部映射；`do_mremap`：
      锁内只改私有 VMA 可见长度；`sys_mremap`：用户 ABI 加锁包装；`remap_pfn_range`：驱动声明
      地址/PFN 直接对应并标志 VMA；`vm_iomap_memory`：设备资源叠加 VMA 偏移后进入 io remap；
      `remap_vmalloc_range`：仅授权 `VM_USERMAP` 缓冲区并重设 VMA 地址。
    - `__access_remote_vm`/`access_remote_vm`：已有 mm 引用时按单 VMA 权限复制并返回短长度；
      `access_process_vm`：只有 task 时临时取得 mm、复制后 mmput；`__copy_remote_vm_str`：BPF 路径
      在单 VMA 内复制并保证 NUL；`copy_remote_vm_str`：task 包装取得/归还 mm；
      `nommu_shrink_inode_mappings`：ramfs truncate 缩小时先拒绝活跃共享死区，再收紧 region；
      `init_user_reserve`/`init_admin_reserve`：subsys init 根据空闲 KiB 发布普通用户/管理员恢复额度；
      `dup_mmap`：fork `dup_mm()` 在 NOMMU 仅复制 exe file 引用，随后建立新 mm 高水位。
  - 结构体字段清单：目标文件唯一结构定义 `mmap_arg_struct` 的 `addr/len`（字节区间）、`prot/flags`
    （权限/策略）、`fd`（调用期描述符）、`offset`（须页对齐的字节偏移）均只在 old_mmap 栈快照期有效；
    `nommu_table` 显式字段 `procname/data/maxlen/mode/proc_handler/extra1` 分别给出路径、整数地址/宽度、
    0644 权限、minmax 处理器与零下界；空 `generic_file_vm_ops` 的静态生命周期和 ramfs 使用场景已说明。
    关联 `vm_region` 的 `vm_rb/vm_flags/vm_start/vm_end/vm_top/vm_pgoff/vm_file/vm_usage/
    vm_icache_flushed` 已逐项在树、共享、页 ownership、file 引用和 I-cache 发布处解释；所用 VMA 的
    `vm_mm/vm_start/vm_end/vm_pgoff/vm_flags/vm_file/vm_region/vm_ops/vm_page_prot`，mm 的
    `start_brk/context.end_brk/brk/total_vm/map_count/mm_mt`，file/inode/address_space 的
    `f_op/f_mapping/f_mode/f_path/i_mode/i_mapping/i_mmap` 均有单位、有效锁域和使用场景。
  - 枚举清单：以 `rg '^enum|typedef enum' mm/nommu.c` 搜索，目标文件不定义 enum；不得省略的命名
    选择值已覆盖：`S_IFREG/S_IFBLK` 产生 COPY、`S_IFCHR` 产生 DIRECT+READ+WRITE、default 拒绝；
    `NOMMU_MAP_COPY/DIRECT/READ/WRITE/EXEC` 的产生者为文件回调/默认推导、消费者为验证与后端选择，
    典型 MAP_PRIVATE/SHARED/EXEC 场景及对复制、直接映射和返回 errno 的影响均已说明。
  - 其他实体清单：`highest_memmap_pfn`（最高 PFN）、`heap_stack_gap`（额外间隔，初值 0）、
    `mmap_pages_allocated`（私有映射页原子统计）、`vm_region_jar`（启动期至关机 slab）、
    `nommu_region_tree`（地址键共享树）、`nommu_region_sem`（树/usage/字段同步）、
    `sysctl_nr_trim_pages`（尾页浪费阈值）、`nommu_table`、`generic_file_vm_ops` 均已说明；函数参数和
    重要局部量由契约/变量地图逐项覆盖，简单迭代量在所属阶段解释。
  - 英文注释与路径清单：SPDX、版权名单豁免；其余文件说明、vmalloc kernel-doc、region/VMA
    协议、mmap/munmap、远程访问、truncate 和 reserve 共 120 组候选英文注释均原样保留并有紧邻
    完整翻译/学习补充，脚本 `untranslated=0`。路径覆盖匿名/文件、PRIVATE/SHARED、DIRECT/COPY、
    已有 region 复用、新 region 发布、零填充、I-cache 首次刷新；快速包装/安全空桩、私有页慢分配与
    文件读取；能力/权限/LSM 拒绝、元数据 OOM、驱动 errno、共享冲突及逆序 cleanup；munmap 整体/
    首尾/中段分裂、mremap 容量内调整、exit 全释放和 truncate 冲突/收紧。
  - 并发与生命周期清单：`mmap_lock` 保护 mm Maple Tree、VMA 边界和 map_count；
    `nommu_region_sem` 写侧保护 region 树、地址键、`vm_usage`、`vm_icache_flushed` 和末引用领取；
    `i_mmap_rwsem` 保护 address_space 反向区间树，truncate 锁序为 region→i_mmap；file 引用在 region
    与 VMA 各持一份并在失败/销毁路径配对 fput；私有页从 `alloc_pages_exact` 到 VM_MAPPED_COPY
    最后引用或 shrink 的 `free_page_series`；VMA 先从 i_mmap/Maple Tree 摘除再 close/free；远程访问
    以 task→mm 引用跨越退出，并只在 mmap 读锁内借用 VMA；`mmap_pages_allocated` 原子统计允许并发；
    `do_munmap` 日志 limit 的竞态只影响限报次数，不影响映射状态。
  - 关联文件读取清单：`mm/util.c:vm_mmap_pgoff()`（充分）确认 LSM/fsnotify→mmap 写锁→`do_mmap`→
    userfaultfd/populate；`include/linux/mm_types.h:struct vm_region`（学习注释缺失）核对全部字段与
    usage 锁；`include/linux/mm.h:is_nommu_shared_mapping()`（充分）核对 MAYSHARE/MAYOVERLAY 判定及
    do_mmap/do_munmap 声明；`include/linux/fs.h:NOMMU_MAP_*`（学习注释缺失）核对 COPY/DIRECT/R/W/X
    位；`fs/ramfs/file-nommu.c:ramfs_nommu_resize(),ramfs_nommu_mmap_prepare()`（缺失）确认 truncate
    调用者与空 vm_ops 注册；`kernel/trace/bpf_trace.c:copy_user_str_sleepable()`、
    `kernel/bpf/helpers.c:bpf_copy_from_user_task()`（两处相关区域缺失）确认 BPF task 字符串场景；
    `kernel/fork.c:dup_mm()`（部分覆盖）确认 `dup_mmap` 前后 uprobe 与高水位步骤。建议后续分别在其
    所属目录任务补注，当前仅核对契约，未越界修改。
  - 三函数抽查：只读 `validate_mmap_request()` 注释可复述 fd 能力产生→共享/私有删减→执行/noexec→
    LSM 及各 errno；只读 `do_mmap()` 注释可复述两个 file 引用、region 复用/驱动选址/私有副本、
    Maple 发布点和四层失败回滚；只读 `do_munmap()`/`split_vma()`/`vmi_shrink_vma()` 注释可推导
    文件 VMA 不可切、匿名中段先分裂、索引摘除早于页释放，以及新增失败应落到哪个 cleanup。
    开发者推理抽查亦可回答锁序、裸 region/VMA 解锁后不可继续使用、DIRECT 回调可睡眠、debug
    配置空桩差异和 I-cache 必须在可执行映射发布前完成。
  - 修改安全与机械门禁：完整复读最终 2672 行；配置展开共 76 个函数定义，76/76 契约归属正确。
    `scripts/check-learning-comment-density.py --min-density 0.20 --max-code-gap 10
    --require-english-translation --json mm/nommu.c` 通过：`code=1201 comments=1192 chinese=587
    density=0.489 max_gap=10 untranslated=0`。`git diff --numstat` 为新增 769、删除 0，证明代码和原注释
    零删除/零改写；`git diff --check` 通过。增量 checkpatch 为 0 errors、0 checks、314 warnings，全部
    是中文 UTF-8 计宽产生的 101～136 列行宽提示。工作树无 `.config`，未执行目标配置对象构建。
    已按方法论第 17 章完成强制验收，`mm/nommu.c` 状态为“全文件完成”。
- [x] `mm/damon/tests/core-kunit.h`
  - 2026-09-20 建图与实施：原文件 1503 行、有效代码 1211 行、英文注释候选 38 组，初始
    `density=0.000/max_gap=1211`。按 region/target 基础、属性换算、quota/filter/scheme commit、
    ctx/filter/反馈与最小 region 五区完成补注；最终仅新增学习注释 484 行、删除 0 行。
  - 2026-09-20 第 17 章函数清单（`H` 为紧邻契约范围，`D` 为声明行，`E` 为函数体末行；42/42
    均逐项核对业务背景、全部入参、返回/副作用、同步/ownership、失败出口和实际调用场景并通过）：
    - 基础对象与 region：`damon_test_regions H24-31/D32/E62`、
      `nr_damon_targets H64-69/D70/E79`、`damon_test_target H81-86/D87/E112`、
      `damon_test_aggregate H124-132/D133/E194`、`damon_test_split_at H196-201/D202/E240`、
      `damon_test_merge_two H242-247/D248/E297`、`__nth_region_of H299-305/D306/E317`、
      `damon_test_merge_regions_of H319-324/D325/E367`、
      `damon_test_split_regions_of H369-374/D375/E452`。
    - operations、范围与监控属性：`damon_test_ops_registration H454-460/D461/E518`、
      `damon_test_set_regions_for H520-527/D528/E568`、`damon_test_set_regions H570-575/D576/E681`、
      `damon_test_nr_accesses_to_accesses_bp H683-688/D689/E714`、
      `damon_test_update_monitoring_result H716-721/D722/E759`、
      `damon_test_set_attrs H761-766/D767/E797`、`damon_test_moving_sum H799-804/D805/E819`。
    - quota 与迁移目的：`damos_test_new_filter H821-826/D827/E840`、
      `damos_test_commit_quota_goal_for H842-849/D850/E884`、
      `damos_test_commit_quota_goal H886-891/D892/E950`、
      `damos_test_commit_quota_goals_for H952-959/D960/E1008`、
      `damos_test_commit_quota_goals H1010-1015/D1016/E1053`、
      `damos_test_commit_quota H1055-1061/D1062/E1108`、
      `damos_test_help_dests_setup H1110-1116/D1117/E1141`、
      `damos_test_help_dests_free H1143-1148/D1149/E1153`、
      `damos_test_commit_dests_for H1155-1161/D1162/E1202`、
      `damos_test_commit_dests H1204-1209/D1210/E1237`。
    - filter 与 scheme：`damos_test_commit_filter_for H1239-1245/D1246/E1279`、
      `damos_test_commit_filter H1281-1286/D1287/E1348`、
      `damos_test_help_initailize_scheme H1350-1355/D1356/E1361`、
      `damos_test_commit_for H1363-1369/D1370/E1419`、
      `damos_test_commit_pageout H1421-1426/D1427/E1450`、
      `damos_test_commit_migrate_hot H1452-1457/D1458/E1479`。
    - target/ctx 与收尾：`damon_test_help_setup_target H1481-1486/D1487/E1510`、
      `damon_test_commit_target_regions_for H1512-1518/D1519/E1553`、
      `damon_test_commit_target_regions H1555-1560/D1561/E1573`、
      `damon_test_commit_ctx H1575-1580/D1581/E1608`、
      `damos_test_filter_out H1610-1616/D1617/E1699`、
      `damon_test_feed_loop_next_input H1701-1706/D1707/E1739`、
      `damon_test_set_filters_default_reject H1741-1746/D1747/E1828`、
      `damon_test_apply_min_nr_regions_for H1830-1836/D1837/E1876`、
      `damon_test_apply_min_nr_regions H1878-1883/D1884/E1900`、
      `damon_test_is_last_region H1902-1907/D1908/E1931`。
  - 函数使用场景清单（逐函数；分号后为触发条件与返回后的下一步）：
    - `damon_test_regions`/`target`/`aggregate`：KUnit 分别触发 region、target 和 3×3 聚合生命周期，
      断言后销毁对象；`nr_damon_targets`：target 用例在增删前后借用遍历 ctx；`split_at`/`merge_two`：
      构造相邻段后验证统计复制或长度加权，再由 target 统一释放；`__nth_region_of`：合并结果按索引
      返回借用节点；`merge_regions_of`：阈值 9/上限 9999 下核对六段 oracle；`split_regions_of`：三轮
      随机拆分核对数量预算，且明确原有无符号余数断言不能证明 5 字节对齐。
    - `ops_registration`：VADDR 可选预注册状态下选择、拒绝重复/哨兵 id、锁内恢复全局槽；
      `set_regions_for`/`set_regions`：五个表驱动场景构造旧范围并核对删除、补洞、追加、插入；
      `nr_accesses_to_accesses_bp`：大周期换算在 32 位回绕时跳过，否则核对截断为零；
      `update_monitoring_result`：三组新旧周期换算次数与 age；`set_attrs`：合法基线后逐字段制造
      `-EINVAL`；`moving_sum`：十个样本依次更新伪移动和。
    - `new_filter`：构造 ANON filter 后核对 type/matching/list，明确现有用例未断言 allow；
      `commit_quota_goal_for`/`commit_quota_goal`：六种 metric 提交通用/联合字段，PSI 保留断言仅在
      helper 前置条件满足时生效；`commit_quota_goals_for`/`commit_quota_goals`：空→一、等长覆盖、
      一→空并在 `out` 回收动态 dst；`commit_quota`：复制额度、tuner、失败记账和三权重；
      `help_dests_setup`/`free`：建立或消费并行拥有数组；`commit_dests_for`/`commit_dests`：3→3、
      2→3、0→3、3→2、3→0 后核对数组并统一释放。
    - `commit_filter_for`/`commit_filter`：七类 src 触发通用字段及四种 union 载荷比较；
      `help_initailize_scheme`：提交前初始化 goals/core/ops 三链表；`commit_for`：提交 pattern/action/
      interval/watermarks 并按迁移动作核对 nid；`commit_pageout`/`commit_migrate_hot`：分别触发通用
      PAGEOUT 覆盖和 PAGEOUT→MIGRATE_HOT 专属字段复制。
    - `help_setup_target`：二维区间转为拥有 region 的 target，失败递归清理；
      `commit_target_regions_for`/`commit_target_regions`：非空 src 重塑 dst、空 src 保留旧布局；
      `commit_ctx`：4096 成功、4095 拒绝、恢复 4096 后提交 pause；`filter_out`：ADDR filter 覆盖
      内/前/后/跨两边界并验证分裂；`feed_loop_next_input`：分数低/高及误差大小触发负反馈；
      `set_filters_default_reject`：无 filter、core allow/reject、再叠加 ops allow 推导两层缺省策略；
      `apply_min_nr_regions_for`/`apply_min_nr_regions`：四个大小/数量组合核对上限与节点数；
      `is_last_region`：四次尾插后立即核对最后节点。所有 28 个注册入口返回 KUnit，helper 返回其用例。
  - 结构体字段清单：目标文件以 `rg '^struct |^union '` 确认不定义结构/union，以下为逐项访问的外部字段，
    测试私有对象除 `damon_ops_lock` 场景外无并发读写：`damon_addr_range.start/end` 与
    `damon_region.ar.start/ar.end` 是字节半开区间、构造至摘链有效；`nr_accesses` 是聚合期采样命中数、
    `nr_accesses_bp` 是万分比、`last_nr_accesses` 是上一聚合值、`age` 是稳定聚合轮数，分裂复制、合并
    加权、属性换算时读写。`damon_attrs.sample_interval/aggr_interval` 为微秒，`min_nr_regions/
    max_nr_regions` 为数量边界；`damon_ctx.attrs/min_region_sz/pause` 分别承载监控配置、字节对齐下限和
    kdamond 暂停配置。`damon_operations.id` 选择全局注册槽，`NR_DAMON_OPS` 仅为越界哨兵。
  - 结构体字段清单（续）：`damos_quota_goal.metric/target_value/current_value` 为目标类型、目标/当前值，
    union 的 `last_psi_total` 是运行期 PSI 累计，`nid/memcg_id` 定位节点/控制组，`list` 只在临时 src
    借链或动态 dst 拥有期有效；`damos_quota.reset_interval/ms/sz` 为毫秒周期、时间/字节额度，`goals`
    为拥有链表，`goal_tuner` 选调谐策略，`fail_charge_num/fail_charge_denom` 为失败记账比例，
    `weight_sz/weight_nr_accesses/weight_age` 为三类优先权重。`damos_migrate_dests.node_id_arr/weight_arr/
    nr_dests` 是同索引并行拥有数组和元素数，发布数量前必须填完数组。
  - 结构体字段清单（续）：`damos_filter.type/matching/allow` 决定类型、正反匹配和允许/拒绝；union 的
    `memcg_id`、`addr_range.start/end`、`target_idx`、`sz_range.min/max` 仅在对应 type 有效，`list.prev/
    list.next` 构造为空环。`damos_access_pattern.min_sz_region/max_sz_region` 为字节范围，
    `min_nr_accesses/max_nr_accesses` 为次数范围，`min_age_region/max_age_region` 为聚合 age；
    `damos_watermarks.metric/interval/high/mid/low` 为指标、微秒检查周期和三阈值。`damos.pattern/action/
    apply_interval_us/quota/wmarks/target_nid` 是 scheme 配置，`core_filters/ops_filters` 是分层拥有链表，
    `core_filters_default_reject/ops_filters_default_reject` 是末项 allow 推导的缺省策略。
    `kunit_case` 表由 `KUNIT_CASE` 生成函数/名称并以空项终止；`kunit_suite.name/test_cases` 分别是
    报告名和静态用例表指针，注册期有效。target/ctx/region 的链表计数由 DAMON helper 封装访问。
  - 枚举清单：目标文件以 `rg '^enum |typedef enum'` 确认不定义 enum；使用值逐项核对如下。
    `DAMON_OPS_VADDR` 由测试/配置选择、注册表与 ctx 消费以启用虚拟地址监控，`NR_DAMON_OPS` 是生产者
    枚举尾哨兵、select 消费后返回 `-EINVAL`。quota metric：`USER_INPUT` 复制 current，`SOME_MEM_PSI_US`
    保留运行期累计，`NODE_MEM_{USED,FREE}_BP` 消费 nid，`NODE_MEMCG_{USED,FREE}_BP` 消费 nid+memcg_id，
    均由控制配置产生、commit/反馈采样消费；`GOAL_TUNER_CONSIST/TEMPORAL` 分别追求长期一致和尽快归零，
    src 设置、quota 调谐消费。filter：`ACTIVE` 仅作 dst 初始哨兵，`ANON/MEMCG` 由 ops 层处理，`YOUNG/
    HUGEPAGE_SIZE/UNMAPPED/ADDR/TARGET` 由 core 分支消费，其中四个载荷类型分别改变 union 读取成员；
    commit 后 type/matching/allow 决定下一过滤阶段。action：`PAGEOUT` 走非迁移分支，`MIGRATE_HOT` 在
    用例中产生并消费 target_nid，`MIGRATE_COLD` 是相同专属字段的 switch 配对值；
    `DAMOS_WMARK_FREE_MEM_RATE` 由 scheme 配置产生、水位检查消费以决定启停。
  - 其他实体与英文清单：文件静态实体仅 `damon_test_cases[]`（28 个入口加空哨兵）和
    `damon_test_suite`（name/test_cases），生命周期至测试注册结束；42 个函数的参数、返回值和重要
    局部量均在各自契约/阶段注释中逐项覆盖，循环索引与 `skip/err/need_cleanup` 的有效阶段和回滚用途
    已说明。SPDX、版权/作者元数据豁免；其余 38 组英文候选原样保留并有紧邻完整中文翻译/补充，
    `untranslated=0`。
  - 路径清单：成功路径覆盖对象增删、统计换算、全量 commit 与 KUnit 注册；快速路径覆盖空 src、
    空 filter、等长数组和反馈目标相等邻域，慢路径覆盖 region 随机拆分、范围补洞及列表/数组重分配；
    失败路径覆盖每级 OOM 的已得对象清理、`-EINVAL` 属性/id/min_region_sz、commit OOM 的 `out`；
    释放路径覆盖 ctx→target→region 递归销毁、显式摘链、quota goal 和 migrate 数组；配置分支覆盖
    `CONFIG_DAMON_VADDR` 注册初态、32 位聚合周期回绕以及 core/ops filter 分层。明确记录两个原用例
    盲区：split 的无符号余数断言恒真，new_filter 未断言 allow；只加注释，不擅改测试行为。
  - 并发与生命周期清单：绝大多数 KUnit 用例只操作未发布的私有栈/堆对象，无 RCU、屏障或引用并发；
    唯一全局竞态是 `damon_registered_ops[DAMON_OPS_VADDR]`，测试与生产 register/select 均以
    `damon_ops_lock` 配对，锁外不得借用可变槽。region 加入 target、target 加入 ctx 后 ownership 逐层
    转移，destroy 先摘链再释放；quota dst goal 必须动态分配以允许被测 kfree，src 复合字面量只借链；
    filter 链表在栈 scheme 存活期借用动态节点，末尾显式释放后不再遍历；迁移并行数组始终由 setup/
    free 成对消费；复合字面量只在完整调用表达式内有效。
  - 关联文件读取清单：`mm/damon/core.c` 的 ops 注册、region 增删/范围设置、访问换算、attrs、quota/
    filter/dests/scheme/ctx commit、filter match/default reject、反馈环、分裂/合并、最小 region 实现
    （相关区域中文学习注释缺失）用于核对实际复制字段、错误码、锁和 ownership；`include/linux/damon.h`
    的 `damon_region/target/attrs/ctx/operations`、quota goal/quota/filter/access pattern/migrate dests/
    watermarks/scheme 及全部相关 enum（中文学习注释缺失）用于核对单位、union 有效条件和枚举语义。
    建议在对应后续基线任务补注，当前只读核对且未越界修改。
  - 三组复杂抽查：只读 `damon_test_ops_registration` 注释可复述可选预注册→锁内摘槽→重注册→恢复和
    cleanup；只读 `damos_test_commit_quota_goals_for` 可推导为何 dst 必须堆分配、src 只能借链及 OOM
    应落 `out`；只读 `damos_test_filter_out` 可推导四种相交关系、哪个节点被分裂/销毁及借用指针何时
    失效。开发者视角可回答：只有 ops 全局槽需要锁、私有对象可睡眠分配、commit 新增错误应沿现有
    cleanup、复合字面量不得逃逸、过滤分层由 type 决定；无 RCU/屏障可删除问题。
  - 修改安全与机械门禁：最终顺序复读 1987 行；42/42 函数契约归属正确，目标内无结构/enum 定义且
    外部字段/枚举逐项审计。`scripts/check-learning-comment-density.py --min-density 0.20 --max-code-gap 10
    --require-english-translation --json mm/damon/tests/core-kunit.h` 通过：`code=1211 comments=611
    chinese=390 density=0.322 max_gap=10 untranslated=0`。`git diff --numstat` 为新增 484、删除 0，证明
    代码和原注释零删除/零改写；`git diff --check` 通过。增量 checkpatch 为 0 errors、0 checks、127
    warnings，全部为中文 UTF-8 计宽产生的 101～134 列行宽提示。工作树无 `.config`，未执行 DAMON
    KUnit 构建。已按方法论第 17 章完成强制验收，状态为“全文件完成”。
- [~] 当前文件：`mm/mmap.c`
  - 2026-09-20 建图检查点：当前版本 1921 行、有效代码 1228 行、已有注释 437 行但中文为 0；机械
    基线 `density=0.000/max_gap=1228`，非豁免英文待翻译 98 组。已完成首次顺序通读，索引出约 45 个
    普通/配置互斥函数定义、`mmap_arg_struct`、`special_mapping_vmops`、`mmap_table` 及随机化/guard/
    reserve 全局实体。拟按“brk 与 mmap 参数验证 → unmapped-area/VMA 查询/栈扩展 → munmap/remap/
    exit → special mapping/初始化与热插拔 → dup_mmap”分区补注；尚未达到任何验收结论。
  - 2026-09-20 检查点 1：已完成文件总览、`vma_set_page_prot`、brk 事务、mmap 参数/文件与匿名分支、
    hugetlb 包装、bottom-up/top-down/THP 空闲地址选择、VMA 查询和两种栈增长配置的首轮补注。
    当前源码新增注释 148 行、删除 0 行，`git diff --check` 通过；机械扫描为 `density=0.099`、
    `max_gap=597`，剩余 46 组英文注释待翻译，故继续保持 `[~]`。

## 固定顺序

1. 闭环 `mm/gup_test.h`。
2. 构建入口：`mm/Makefile` → `mm/Kconfig` → `mm/Kconfig.debug` → `damon/{Makefile,Kconfig}` → `kasan/Makefile` → `kfence/Makefile` → `kmsan/Makefile`。
3. 按基线表序号 2 起处理零中文 `.c/.h`；最后复审标记 `r` 的 18 个文件。
4. 排除 `.DS_Store`、两份 `.kunitconfig`；Rust 测试不套用 C/H 方法论，另行定标。
