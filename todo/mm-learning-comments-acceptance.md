# mm 学习注释第 17 章验收记录

本文件保存 `doc/linux-kernel-source-learning-methodology.md` 第 17 章要求的逐文件内容验收证据。
行号对应该文件完成本轮编辑后的工作区版本；后续改动使行号漂移时，必须重建对应文件记录。

## mm/cma.c

### 函数清单

`函数头行 | 声明及函数体行 | 结论`：

|函数|函数头|声明及函数体|17.2 结论|
|---|---:|---:|---|
|`cma_get_base`|49-55|56-60|通过|
|`cma_get_size`|62-68|69-72|通过|
|`cma_get_name`|74-80|81-84|通过|
|`cma_bitmap_aligned_mask`|87-93|94-100|通过|
|`cma_bitmap_aligned_offset`|106-112|113-119|通过|
|`cma_bitmap_pages_to_bits`|121-127|128-132|通过|
|`cma_clear_bitmap`|134-140|141-155|通过|
|`cma_validate_zones`|162-168|169-210|通过|
|`cma_activate_area`|212-219|220-296|通过|
|`cma_init_reserved_areas`|298-304|305-314|通过|
|`cma_reserve_pages_on_error`|317-323|324-327|通过|
|`cma_new_area`|329-335|336-368|通过|
|`cma_drop_area`|370-376|377-381|通过|
|`cma_init_reserved_mem`|395-402|403-447|通过|
|`revsizecmp`|469-475|476-480|通过|
|`basecmp`|482-488|489-493|通过|
|`list_insert_sorted`|498-504|505-524|通过|
|`cma_fixed_reserve`|526-532|533-557|通过|
|`cma_alloc_mem`|559-565|566-618|通过|
|`__cma_declare_contiguous_nid`|620-627|628-732|通过|
|`cma_declare_contiguous_multi`|743-749|750-967|通过|
|`cma_declare_contiguous_nid`|989-996|997-1021|通过|
|`cma_debug_show_areas`|1023-1029|1030-1061|通过|
|`cma_range_alloc`|1063-1071|1072-1169|通过|
|`__cma_alloc_frozen`|1171-1178|1179-1247|通过|
|`cma_alloc_frozen`|1249-1255|1256-1263|通过|
|`cma_alloc_frozen_compound`|1265-1271|1272-1278|通过|
|`cma_alloc`|1290-1296|1297-1309|通过|
|`find_cma_memrange`|1312-1318|1319-1357|通过|
|`__cma_release_frozen`|1359-1366|1367-1381|通过|
|`cma_release`|1393-1399|1400-1424|通过|
|`cma_release_frozen`|1427-1433|1434-1448|通过|
|`cma_for_each_area`|1450-1456|1457-1471|通过|
|`cma_intersects`|1473-1481|1482-1504|通过|
|`cma_reserve_early`|1528-1536|1537-1582|通过|

结构复核：34 个函数均有位于声明正上方的专属中文函数头，且可直接定位业务背景、入参、
返回/副作用和注意事项。英文 kernel-doc 保持在前，中文契约在后；未发现函数尾部、导出宏附近或
条件编译分支错放的函数头。

### 复杂函数只读复述抽查

- `cma_activate_area()`：先为所有 ranges 保存 `early_pfn` 并分配 bitmap，再验证 zone；把早期取走的
  前缀置位、把剩余 pageblocks 初始化成 CMA，最后初始化运行期锁并发布 `CMA_ACTIVATED`。任一步失败
  都只释放已经分配的 bitmap；未设置保留策略时，从各 range 的真实 early 边界开始把后缀交回 buddy，
  最后撤销 `totalcma_pages` 并把 area 容量清零。
- `cma_declare_contiguous_multi()`：先尝试单段，只有 `-ENOMEM` 才进入多段慢路；慢路筛选 4GB 以上、
  同时满足 pageblock/bitmap 对齐的候选，先按大小保留最多八段，再按地址升序逐段 reserve，最后发布
  `nranges/nid/res_cma`。物理 reserve 中途失败会释放此前完整保留的 ranges 并回滚尾部 area 槽位。
- `cma_range_alloc()`：锁内检查总可用量并认领满足对齐的空 bitmap 区，确认 memmap 连续后扣减
  `available_count`；锁外在 `alloc_mutex` 下迁移并冻结物理页。成功保留 bit 并输出 frozen 首页；失败先
  清 bit/恢复计数，只有 `-EBUSY` 会移动到下一个候选，其它错误直接返回。
- `cma_reserve_early()`：只在未激活、单线程 init 阶段工作；校验两种粒度和总余量后，从能单独容纳
  请求的首个 range 底部推进 `early_pfn`，不跨 range 拼接且不可撤销。调用者接管页结构和失败后的
  生命周期责任。

### 关联文件与语义证据

- `include/linux/cma.h`：核对公开声明及单段包装；学习注释现状为不足，未纳入本文件验收。
- `mm/cma.h`：核对 `struct cma`、`struct cma_memrange`、flags、bitmap 粒度和 sysfs 计账；部分覆盖，
  尚未独立按第 17 章验收。
- `mm/page_alloc.c`：核对 `alloc_contig_frozen_range()` / `free_contig_frozen_range()` 的 frozen ownership、
  单 zone 与 GFP 约束；部分覆盖，尚未独立验收。
- `mm/hugetlb_cma.c`：核对 early reserve 与 frozen compound 的调用配对；已有中文但仅部分覆盖。
- `arch/s390/mm/init.c`：核对 memory-offline 传给 `cma_intersects()` 的排他 `end`；学习注释不足。

### 英文、状态与边界审计

- 已逐段核对原有英文说明，紧邻中文覆盖其条件、因果、失败、锁和 ownership；原英文均保留。
- 已覆盖 `cma_areas/cma_area_count`、`memranges`、候选结构字段、range/area 状态和 bitmap 粒度。
- 已把四个未决行为边界记录到 `doc/questions/memory-management-questions.md`；它们不是本轮行为修改。

### 机械与追加式安全门禁

- 密度：`code=729 comments=676 chinese=358 density=0.491 max_gap=10`，通过。
- `git diff --check -- mm/cma.c doc/questions/memory-management-questions.md`：通过。
- `mm/cma.c` 相对基线：新增 433 行、删除 0 行；未改代码或原有英文注释。
- checkpatch：仅 `LONG_LINE_COMMENT`（245 条），无其它 warning/check/error。
- 构建：工作区无 `.config`，未声称目标对象编译通过。

## mm/hugetlb_cgroup.c

### 函数清单

下表格式为 `函数 | 函数头 | 声明及函数体`，全部通过第 17.2 节：

|函数|函数头|声明及函数体|
|---|---:|---:|
|`__hugetlb_cgroup_counter_from_cgroup`|55-65|66-72|
|`hugetlb_cgroup_counter_from_cgroup`|80-86|87-90|
|`hugetlb_cgroup_counter_from_cgroup_rsvd`|97-104|105-108|
|`hugetlb_cgroup_from_css`|115-122|123-126|
|`hugetlb_cgroup_from_task`|133-141|142-145|
|`hugetlb_cgroup_is_root`|148-153|154-157|
|`parent_hugetlb_cgroup`|160-166|167-170|
|`hugetlb_cgroup_have_usage`|176-184|185-195|
|`hugetlb_cgroup_init`|203-212|213-256|
|`hugetlb_cgroup_free`|263-270|271-279|
|`hugetlb_cgroup_css_alloc`|287-296|297-346|
|`hugetlb_cgroup_css_free`|349-355|356-359|
|`hugetlb_cgroup_move_parent`|372-381|382-426|
|`hugetlb_cgroup_css_offline`|437-445|446-464|
|`hugetlb_event`|472-481|482-494|
|`__hugetlb_cgroup_charge_cgroup`|503-514|515-562|
|`hugetlb_cgroup_charge_cgroup`|565-571|572-576|
|`hugetlb_cgroup_charge_cgroup_rsvd`|579-586|587-591|
|`__hugetlb_cgroup_commit_charge`|598-608|609-634|
|`hugetlb_cgroup_commit_charge`|637-643|644-649|
|`hugetlb_cgroup_commit_charge_rsvd`|652-658|659-664|
|`__hugetlb_cgroup_uncharge_folio`|673-682|683-721|
|`hugetlb_cgroup_uncharge_folio`|724-729|730-734|
|`hugetlb_cgroup_uncharge_folio_rsvd`|737-742|743-747|
|`__hugetlb_cgroup_uncharge_cgroup`|753-762|763-777|
|`hugetlb_cgroup_uncharge_cgroup`|780-785|786-790|
|`hugetlb_cgroup_uncharge_cgroup_rsvd`|793-798|799-803|
|`hugetlb_cgroup_uncharge_counter`|809-818|819-829|
|`hugetlb_cgroup_uncharge_file_region`|835-845|846-871|
|`hugetlb_cgroup_read_numa_stat`|897-906|907-971|
|`hugetlb_cgroup_read_u64`|977-985|986-1027|
|`hugetlb_cgroup_read_u64_max`|1033-1041|1042-1092|
|`hugetlb_cgroup_write`|1102-1112|1113-1162|
|`hugetlb_cgroup_write_legacy`|1165-1170|1171-1175|
|`hugetlb_cgroup_write_dfl`|1178-1183|1184-1188|
|`hugetlb_cgroup_reset`|1194-1203|1204-1242|
|`mem_fmt`|1248-1256|1257-1266|
|`__hugetlb_events_show`|1272-1279|1280-1300|
|`hugetlb_events_show`|1303-1308|1309-1312|
|`hugetlb_events_local_show`|1315-1320|1321-1324|
|`hugetlb_cgroup_cfttypes_init`|1455-1465|1466-1499|
|`__hugetlb_cgroup_file_dfl_init`|1502-1507|1508-1515|
|`__hugetlb_cgroup_file_legacy_init`|1518-1523|1524-1531|
|`__hugetlb_cgroup_file_init`|1534-1539|1540-1544|
|`__hugetlb_cgroup_file_pre_init`|1550-1557|1558-1571|
|`__hugetlb_cgroup_file_post_init`|1574-1580|1581-1587|
|`hugetlb_cgroup_file_init`|1593-1601|1602-1611|
|`hugetlb_cgroup_migrate`|1621-1630|1631-1654|

抽查复述：charge 从 leaf 沿父链逐级试扣并在失败点逆序取消；commit 把 reservation 取消后计入
actual，但 `page_counter_cancel()` 只减本层，不能误解为递归减祖先；css offline 把 actual folio 迁到父组，
reservation 依靠持有的 css 引用延迟释放。文件接口按 hstate/actual-reserved 属性解码，写入与 reset 分别
维护 max/rsvd 限额和 failure 计数。

关联证据：`include/linux/hugetlb_cgroup.h`（结构/inline，部分覆盖）、`mm/hugetlb.c`（reserve、commit、
rollback、migration 调用链，已有中文但未独立验收）、`mm/page_counter.c` 与
`include/linux/page_counter.h`（charge/cancel 语义，前者较充分、后者部分覆盖）。

机械门禁：`code=721 comments=835 chinese=565 density=0.784 max_gap=10`；新增 754 行、删除 0；
`diff --check` 通过；checkpatch 仅 `LONG_LINE_COMMENT`；无 `.config`，未执行对象编译。

## mm/page_owner.c

### 函数清单

下表格式为 `函数 | 函数头 | 声明及函数体`，全部通过第 17.2 节：

|函数|函数头|声明及函数体|
|---|---:|---:|
|`set_current_in_page_owner`|100-107|108-118|
|`unset_current_in_page_owner`|121-126|127-130|
|`early_page_owner_param`|136-143|144-153|
|`need_page_owner`|157-162|163-166|
|`create_dummy_stack`|172-178|179-186|
|`register_dummy_stack`|189-193|194-197|
|`register_failure_stack`|200-204|205-208|
|`register_early_stack`|211-215|216-219|
|`init_page_owner`|226-234|235-256|
|`get_page_owner`|271-277|278-281|
|`save_stack`|288-296|297-316|
|`add_stack_record_to_list`|323-332|333-371|
|`inc_stack_record_count`|378-387|388-415|
|`dec_stack_record_count`|421-429|430-441|
|`__update_page_owner_handle`|448-458|459-493|
|`__update_page_owner_free_handle`|500-510|511-537|
|`__reset_page_owner`|544-552|553-597|
|`__set_page_owner`|604-613|614-627|
|`__folio_set_owner_migrate_reason`|633-642|643-656|
|`__split_page_owner`|662-671|672-686|
|`__folio_copy_owner`|693-704|705-766|
|`pagetypeinfo_showmixedcount_print`|773-782|783-886|
|`print_page_owner_memcg`|895-904|905-946|
|`print_page_owner`|953-963|964-1027|
|`__dump_page_owner`|1034-1042|1043-1104|
|`read_page_owner`|1111-1121|1122-1234|
|`lseek_page_owner`|1240-1247|1248-1265|
|`init_pages_in_zone`|1274-1282|1283-1370|
|`init_early_allocated_pages`|1373-1378|1379-1386|
|`stack_start`|1401-1409|1410-1434|
|`stack_next`|1440-1446|1447-1458|
|`stack_print`|1468-1476|1477-1514|
|`stack_stop`|1517-1522|1523-1525|
|`page_owner_stack_open`|1544-1553|1554-1569|
|`page_owner_threshold_get`|1584-1589|1590-1594|
|`page_owner_threshold_set`|1597-1603|1604-1608|
|`pageowner_init`|1619-1627|1628-1660|

抽查复述：stack 保存用 current 重入哨兵避免 stack depot 再分配递归；每个 page_ext owner 记录与 stack
depot 引用按 set/reset/copy/split 路径配对；debugfs page 顺序文件在 zone/PFN 上迭代并只输出已分配记录；
stack 聚合序列按 threshold 过滤。当前实现的 free-handle 参数 `pid/tgid` 未使用而写 current，copy 路径
在 `page_ext_put()` 后继续读 owner 字段，均已按真实代码和外层序列化前提说明。

关联证据：`include/linux/page_owner.h`（wrapper 边界，部分覆盖）、`include/linux/page_ext.h` 与
`mm/page_ext.c`（RCU get/put 生命周期，部分覆盖）、`include/linux/stackdepot.h` 与 `lib/stackdepot.c`
（stack handle/refcount，部分覆盖）、`mm/migrate.c`、`mm/hugetlb.c`、`mm/huge_memory.c`、
`mm/page_alloc.c`（主要调用点，覆盖程度不一，均未随本文件独立验收）。

机械门禁：`code=727 comments=766 chinese=509 density=0.700 max_gap=10`；新增 660 行、删除 0；
`diff --check` 通过；checkpatch 仅 `LONG_LINE_COMMENT`；无 `.config`，未执行对象编译。

## mm/util.c

### 函数清单

|函数|函数头|声明及函数体|17.2 结论|
|---|---:|---:|---|
|`kfree_const`|52-58|59-64|通过|
|`__kmemdup_nul`|76-82|83-98|通过|
|`kstrdup`|107-114|115-119|通过|
|`kstrdup_const`|133-139|140-147|通过|
|`kstrndup`|160-166|167-171|通过|
|`kmemdup_noprof`|184-190|191-201|通过|
|`kmemdup_array`|215-221|222-226|通过|
|`kvmemdup`|239-245|246-255|通过|
|`kmemdup_nul`|267-273|274-278|通过|
|`init_user_buckets`|284-290|291-297|通过|
|`memdup_user`|309-315|316-333|通过|
|`vmemdup_user`|345-351|352-368|通过|
|`strndup_user`|378-384|385-410|通过|
|`memdup_user_nul`|421-427|428-445|通过|
|`vma_is_stack_for_current`|449-455|456-462|通过|
|`vma_set_file`|467-473|474-481|通过|
|`randomize_stack_top`|489-495|496-514|通过|
|`randomize_page`|530-536|537-556|通过|
|`arch_randomize_brk`|561-567|568-576|通过|
|`arch_mmap_rnd`|578-584|585-598|通过|
|`mmap_is_legacy`|602-608|609-623|通过|
|`mmap_base`|633-639|640-673|通过|
|`arch_pick_mmap_layout` (default topdown)|679-685|686-703|通过|
|`arch_pick_mmap_layout` (fixed fallback)|705-711|712-717|通过|
|`__account_locked_vm`|738-744|745-776|通过|
|`account_locked_vm`|793-799|800-815|通过|
|`vm_mmap_pgoff`|820-826|827-856|通过|
|`vm_mmap`|875-881|882-893|通过|
|`vm_mmap_shadow_stack`|904-910|911-934|通过|
|`__vmalloc_array_noprof`|943-949|950-959|通过|
|`vmalloc_array_noprof`|967-973|974-978|通过|
|`__vcalloc_noprof`|987-993|994-998|通过|
|`vcalloc_noprof`|1006-1012|1013-1017|通过|
|`folio_anon_vma`|1022-1028|1029-1037|通过|
|`folio_mapping`|1051-1057|1058-1077|通过|
|`folio_copy`|1090-1096|1097-1110|通过|
|`folio_mc_copy`|1113-1119|1120-1137|通过|
|`overcommit_ratio_handler`|1159-1165|1166-1176|通过|
|`sync_overcommit_as`|1178-1184|1185-1189|通过|
|`overcommit_policy_handler`|1191-1197|1198-1237|通过|
|`overcommit_kbytes_handler`|1239-1245|1246-1257|通过|
|`init_vm_util_sysctls`|1306-1312|1313-1318|通过|
|`vm_commit_limit`|1327-1333|1334-1349|通过|
|`vm_memory_committed`|1371-1377|1378-1382|通过|
|`__vm_enough_memory`|1403-1409|1410-1462|通过|
|`get_cmdline`|1476-1482|1483-1541|通过|
|`memcmp_pages`|1545-1551|1552-1565|通过|
|`mem_dump_obj`|1579-1585|1586-1613|通过|
|`page_offline_freeze`|1640-1646|1647-1651|通过|
|`page_offline_thaw`|1653-1659|1660-1664|通过|
|`page_offline_begin`|1666-1672|1673-1677|通过|
|`page_offline_end`|1680-1686|1687-1691|通过|
|`flush_dcache_folio`|1695-1701|1702-1711|通过|
|`compat_set_desc_from_vma`|1733-1739|1740-1762|通过|
|`__compat_vma_mmap`|1787-1793|1794-1809|通过|
|`compat_vma_mmap`|1838-1844|1845-1863|通过|
|`set_ps_flags`|1868-1874|1875-1892|通过|
|`snapshot_page`|1909-1915|1916-1979|通过|
|`call_vma_mapped`|1981-1987|1988-2007|通过|
|`mmap_action_finish`|2009-2015|2016-2047|通过|
|`check_mmap_action`|2051-2057|2058-2067|通过|
|`mmap_action_prepare` (`CONFIG_MMU`)|2076-2082|2083-2112|通过|
|`mmap_action_complete` (`CONFIG_MMU`)|2126-2132|2133-2160|通过|
|`mmap_action_prepare` (`!CONFIG_MMU`)|2163-2169|2170-2187|通过|
|`mmap_action_complete` (`!CONFIG_MMU`)|2190-2196|2197-2220|通过|
|`folio_pte_batch`|2250-2256|2257-2263|通过|
|`page_range_contiguous`|2281-2287|2288-2309|通过|

结构复核：67 个条件编译函数定义均有紧邻声明的专属中文函数头和四类显式信息；英文 kernel-doc
保持在前。全局 sysctl、per-CPU 计数、用户复制 bucket、离线 rwsem、布局宏和 action 表也已覆盖。

### 复杂函数只读复述抽查

- `__vm_enough_memory()`：先把请求加入 `vm_committed_as`；ALWAYS 直接批准，GUESS 只拒绝单请求已超过
  RAM+swap 的明显不可能情况。NEVER 计算 commit limit，依权限扣 admin reserve、依 mm 规模扣有限 user
  reserve，再与正的 committed 近似值比较；所有拒绝分支统一打印并回滚最初记账。
- `get_cmdline()`：`get_task_mm` 固定对象，`arg_lock` 只把 argv/env 四边界取成同一快照；跨进程读取可能
  部分成功。若 argv 尾 NUL 被覆盖且缓冲仍有空间，先查已读内容，再按剩余容量拼接 env 并截到首个
  NUL；所有 mm 路径最终 `mmput`，返回长度不承诺尾 NUL。
- `snapshot_page()`：先复制原 page 并解析 compound_info；order-0 直接形成单页快照，tail 则还原 head
  与 index。索引在快照表示范围内时复制 folio head/第二页并取页数；split/merge 竞争导致 index 不匹配时
  最多重试五次，耗尽后清 compound head、降级为单页并清 FAITHFUL。
- mmap action：prepare 在 VMA 发布前按 type 验证/准备资源；complete 在已存在 VMA 上提交映射，再由
  finish 调用 mapped、释放可能持有的 rmap 锁。普通新映射失败只 unmap 本次未合并 VMA，compat 失败留给
  外层 post hook；NOMMU 保持接口但对非 NOTHING action 告警并在 complete 阶段失败。

### 关联文件与语义证据

- `include/linux/mm.h`：核对公开复制、布局、folio、snapshot 和 action helper 契约；已有中文但仅部分覆盖。
- `include/linux/mm_types.h`：核对 `mmap_action`、`vm_area_desc` 可变字段与 action ownership；学习注释不足。
- `mm/vma.c::__mmap_region()` / `call_mmap_prepare()`：确认 prepare 在 merge/新 VMA 前执行，complete 只对
  新建映射提交；部分覆盖，尚未独立验收。
- `mm/memory.c`：核对 PFN、I/O 和 kernel-pages 的 prepare/complete 配对；学习注释不足。
- `mm/migrate.c`：核对 `folio_mc_copy()` 的 -EHWPOISON 调用处理；部分覆盖。
- `fs/proc/kcore.c`：核对 page_offline freeze/thaw 包围随机 PFN 内容访问；学习注释不足。
- `mm/debug.c` / `fs/proc/page.c`：核对 snapshot faithful 标志的消费者；部分覆盖。

### 英文、状态与边界审计

- 原英文 kernel-doc、两阶段 action、PageOffline 协议、overcommit 顺序和 PTE batch 前提均有邻接中文覆盖。
- 六组工具的返回约定已区分 NULL、ERR_PTR、地址/负 errno、借用指针和部分写入。
- 三个尚未证明的边界已记录到 `doc/questions/memory-management-questions.md`，未混入行为修改。

### 机械与追加式安全门禁

- 密度：`code=909 comments=1194 chinese=547 density=0.602 max_gap=10`，通过。
- `git diff --check -- mm/util.c`：通过。
- `mm/util.c` 相对基线：新增 703 行、删除 0 行；未改代码或原有英文注释。
- checkpatch：仅 `LONG_LINE_COMMENT`（219 条），无其它 warning/check/error。
- 构建：工作区无 `.config`，未声称目标对象编译通过。

## mm/oom_kill.c

### 函数清单

|函数|函数头|声明及函数体|17.2 结论|
|---|---:|---:|---|
|`is_memcg_oom`|93-99|100-103|通过|
|`oom_cpuset_eligible` (`CONFIG_NUMA`)|118-124|125-161|通过|
|`oom_cpuset_eligible` (`!CONFIG_NUMA`)|163-169|170-173|通过|
|`find_lock_task_mm`|182-189|190-205|通过|
|`is_sysrq_oom`|211-217|218-221|通过|
|`oom_unkillable_task`|224-230|231-238|通过|
|`should_dump_unreclaim_slab`|246-252|253-269|通过|
|`oom_badness`|280-286|287-333|通过|
|`constrained_alloc`|346-352|353-417|通过|
|`oom_evaluate_task`|419-425|426-486|通过|
|`select_bad_process`|492-498|499-518|通过|
|`dump_task`|520-526|527-563|通过|
|`dump_tasks`|575-581|582-606|通过|
|`dump_oom_victim`|608-614|615-626|通过|
|`dump_header`|628-634|635-658|通过|
|`process_shares_mm`|677-683|684-697|通过|
|`__oom_reap_task_mm`|713-719|720-766|通过|
|`oom_reap_task_mm`|774-780|781-826|通过|
|`oom_reap_task`|829-835|836-868|通过|
|`oom_reaper`|870-876|877-901|通过|
|`wake_oom_reaper`|903-909|910-934|通过|
|`queue_oom_reaper` (`CONFIG_MMU`)|949-954|955-969|通过|
|`oom_init`|1003-1008|1009-1017|通过|
|`queue_oom_reaper` (`!CONFIG_MMU`)|1020-1026|1027-1030|通过|
|`mark_oom_victim`|1043-1049|1050-1080|通过|
|`exit_oom_victim`|1085-1090|1091-1099|通过|
|`oom_killer_enable`|1104-1109|1110-1115|通过|
|`oom_killer_disable`|1132-1137|1138-1165|通过|
|`__task_will_free_mem`|1167-1172|1173-1197|通过|
|`task_will_free_mem`|1206-1211|1212-1266|通过|
|`__oom_kill_process`|1268-1273|1274-1370|通过|
|`oom_kill_memcg_member`|1376-1381|1382-1392|通过|
|`oom_kill_process`|1394-1399|1400-1456|通过|
|`check_panic_on_oom`|1461-1466|1467-1490|通过|
|`register_oom_notifier`|1495-1500|1501-1506|通过|
|`unregister_oom_notifier`|1509-1514|1515-1519|通过|
|`out_of_memory`|1531-1536|1537-1618|通过|
|`pagefault_out_of_memory`|1626-1631|1632-1649|通过|
|`process_mrelease`|1651-1656|1657-1730|通过|

结构复核：两个条件编译双实现均单独验收，共 39 个定义；所有中文函数头紧邻声明并显式覆盖四类
信息。原 kernel-doc 位于中文契约之前，`EXPORT_SYMBOL*()` 与下一函数之间未出现错属函数头。

### 复杂函数只读复述抽查

- `out_of_memory()`：禁用时唯一返回 false；全局域先给 notifier 回收机会，已有退出的 current 则直接
  标记 victim/排 reaper。随后处理 NOFS、分类约束、panic 策略和“杀 current”sysctl；常规扫描可能得到
  NULL、`-1` 哨兵或持引用 task，只有最后一种进入 kill。全局真实分配若无任何可杀者会 panic。
- `__oom_kill_process()`：先用带 `task_lock` 的活 mm 线程替换可能已退出的 leader，并转移 chosen 引用；
  `mmgrab` 后先记录事件、发 SIGKILL，再标记 MEMDIE，随后解 task 锁。RCU 下向其它共享同一 mm 的用户
  线程组发信号；若 global init 共享该 mm，则置 OOM_SKIP 并禁止 reaper。末尾严格 `mmdrop` 和 put victim。
- reaper 链：`queue_oom_reaper()` 用 mm flag 去重、取 task 引用并启动延迟 timer；timer 若发现 OOM_SKIP
  就消费引用，否则锁下入队并唤醒 worker；worker 锁下摘链、锁外有限次 trylock/zap，最终置 OOM_SKIP、
  清节点并消费引用。`MMF_UNSTABLE` 在实际遍历前发布，使后续私有 fault 拒绝不可靠内容。
- `process_mrelease()`：pidfd 先取得 task 引用，再找带锁的活 mm 线程并 `mmgrab`；只有所有 mm 共享者都会
  退出才允许 reap。解 task 锁后以 killable mmap 读锁与 exit_mmap 串行，锁内再次检查 OOM_SKIP；所有
  标签路径都按 mm 引用、task 引用逆序释放，非 MMU 配置返回 `-ENOSYS`。

### 关联文件与语义证据

- `include/linux/oom.h`：核对 `struct oom_control`、chosen 字段和公开 API；学习注释不足。
- `mm/page_alloc.c::__alloc_pages_may_oom()`：确认全局入口以 `oom_lock` 串行，并把 true 当作前进条件；
  已有中文但仅部分覆盖。
- `mm/memcontrol.c::mem_cgroup_out_of_memory()`：确认 memcg 入口也在 `oom_lock` 下调用并稳定 memcg 域；
  部分覆盖，尚未独立验收。
- `kernel/exit.c::exit_mm()` 与 `kernel/fork.c::free_signal_struct()`：确认 MEMDIE 计数退出配对，以及
  `signal->oom_mm` 的异步 mmdrop；部分覆盖。
- `mm/mmap.c::exit_mmap()`：确认 OOM_SKIP 在已解除映射后发布，并通过 mmap 写锁周期与 reaper 同步；
  学习注释不足。
- `kernel/power/process.c::freeze_processes()`：确认冻结用户态后调用 disable，并以超时失败回退；
  学习注释不足。

### 英文、状态与边界审计

- 原有英文说明均保留；已覆盖其域资格、选择哨兵、task/mm 引用、锁、reaper 延迟原因与失败边界。
- 已覆盖 sysctl、两把全局 mutex、victim 计数/等待队列、reaper 队列、notifier 链和约束名表。
- 条件编译分别说明 `CONFIG_NUMA`、`CONFIG_MMU` 和 `CONFIG_SYSCTL` 的行为不变量。

### 机械与追加式安全门禁

- 密度：`code=745 comments=833 chinese=389 density=0.522 max_gap=10`，通过。
- `git diff --check -- mm/oom_kill.c`：通过。
- `mm/oom_kill.c` 相对基线：新增 473 行、删除 0 行；未改代码或原有英文注释。
- checkpatch：仅 `LONG_LINE_COMMENT`（278 条），无其它 warning/check/error。
- 构建：工作区无 `.config`，未声称目标对象编译通过。
