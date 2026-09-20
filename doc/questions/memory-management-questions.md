# 内存管理源码学习问题

## CMA 多 range 候选容量是否可能被高估

### 所属宏观阶段

`cma_declare_contiguous_multi()` 单 range 失败后的候选筛选与物理区间保留。

### 初始猜测

达到 `CMA_MAX_RANGES` 后，如果新候选小于当前最小入选项，它不会进入 `ranges`，也不应计入
`sizesum`。

### 不理解的代码

当前实现先执行 `sizesum += size`，随后才在 `size < mrp->size` 时 `continue`；该分支没有减回
刚加入的 `size`。容量预检通过后，代码也没有显式检查最终的 `sizeleft == 0`。

### 当前证据与边界

- `sizesum` 的第一次用途是判断最大八个候选是否足以覆盖 `total_size`；
- 被拒绝的小候选不在 `ranges` 链表中，却仍可能留在该计数中；
- 这只能从当前源码证明“计数与链表容量可能不一致”，尚未用启动内存图复现错误发布 area。

### TODO / 验证方法

- 构造九个以上 4GB 之上的 free memblock ranges，使第九段小于当前尾项；
- 记录 `sizesum`、入选链表真实容量、`sizeleft` 和最终 `nranges`；
- 若确认可发布容量不足的 area，再单独提交行为修复，不能混入学习注释补充。

## CMA 物理区间相邻端点的判交语义

### 所属宏观阶段

s390 memory hotplug 下线前通过 `cma_intersects()` 排除包含 CMA 的 memory block。

### 当前证据

range 使用 `[rstart, rend)`；当前代码在 `end < rstart` 时才跳过，因此 `end == rstart` 会返回
相交，而 `start == rend` 返回不相交。s390 调用者构造的 `end` 是排他上界。

### TODO / 验证方法

确认该不对称是有意的保守热插拔策略还是边界条件缺陷；在结论前不得把接口描述成标准半开区间
判交。

## CMA 零长度释放是否应在入口拒绝

### 所属宏观阶段

`cma_release()` / `cma_release_frozen()` 调用 `find_cma_memrange()` 做归属校验。

### 当前证据

校验拒绝 NULL 和 `count > cma->count`，但未显式拒绝 `count == 0`；命中起始 PFN 后可能进入底层
零长度释放与位图清理。公开契约应要求 `count > 0`，但当前实现是否需要防御式校验尚未确认。

### TODO / 验证方法

核对所有树内调用者并用 KUnit 覆盖普通/frozen 两个入口的零长度输入，再决定是否增加参数检查。

## 带额外引用的普通 CMA 释放仍强制回收

### 所属宏观阶段

`cma_release()` 逐页 `put_page_testzero()` 后向底层归还连续区。

### 当前证据

只要页段属于 area，额外引用会累计并触发 `WARN`，但函数仍调用 `__cma_release_frozen()`，使该物理区
重新进入可分配状态。源码注释必须把它描述为调用者错误，而不是安全失败路径。

### TODO / 验证方法

核对 API 历史契约和所有树内调用者是否保证独占引用；行为修改可能影响既有错误恢复策略，需另行评审。

## randomize_page 对过小未对齐范围的处理

### 所属宏观阶段

brk、mmap 等地址空间布局的 ASLR 候选计算。

### 当前证据

`start` 未页对齐时，代码先执行 `range -= PAGE_ALIGN(start) - start`。若 `range` 小于这段对齐消耗，
无符号减法会回绕；随后的溢出截断可能把它变成一个很大的可随机范围，而不是 kernel-doc 所述的
错误时返回 `start`。树内已见调用者都传入远大于一页的固定窗口，但公开 helper 的参数契约未写明
这一前提。

### TODO / 验证方法

用 `start = PAGE_SIZE - 1`、`range = 0/1` 等边界做 KUnit，核对期望 ABI；确认后再决定是补充入口前提
还是在减法前拒绝过小范围。

## memdup_user_nul 的 len 加一溢出边界

### 所属宏观阶段

用户字节块复制到内核并追加终止符。

### 当前证据

函数把 `len + 1` 直接传给 bucket allocator，未在本层使用 `size_add()`；若理论输入为 `SIZE_MAX`，
长度会回绕。多数树内调用来自 write count 或先行限长，但公开声明未表达必须小于 `SIZE_MAX`。

### TODO / 验证方法

核对 bucket allocator 是否在更低层再次防溢出，并审计不受 VFS 最大写入长度约束的调用者；若下层
不能保证，行为修复应单独使用 `size_add()`，不混入注释提交。

## snapshot_page 的 tail index 边界

### 所属宏观阶段

无锁 page/folio 调试快照在 compound split/merge 并发下的一致性降级。

### 当前证据

有效 folio 页索引通常为 `[0, nr_pages)`，当前重试条件是 `ps->idx > nr_pages` 而非 `>=`。若并发读到
`idx == nr_pages`，代码是否仍可能把快照标记为 faithful 需要结合 `compound_info` 编码和允许的瞬态证明。

### TODO / 验证方法

核对 `folio_page_idx()` 与 split/merge 的发布顺序，并用并发 folio split 压测观察等号边界；在证明前只把
它记录为疑点，不宣称存在越界缺陷。
