# Linux 块 I/O 内部实现：一次写入如何到达 NVMe

> 适用内核版本：v7.2-rc1（基于当前仓库 `a14c021eef14`）
> 核心源码：`fs/fs-writeback.c`、`mm/page-writeback.c`、`block/`、`drivers/nvme/host/`

---

## 目录

1. [从 write 和 fsync 开始](#1-从-write-和-fsync-开始)
2. [背景与需要解决的问题](#2-背景与需要解决的问题)
3. [总体方案及其优劣](#3-总体方案及其优劣)
4. [宏观地图](#4-宏观地图)
5. [贯穿案例的阶段划分](#5-贯穿案例的阶段划分)
6. [文件偏移如何变成设备扇区](#6-文件偏移如何变成设备扇区)
7. [page cache 与脏页](#7-page-cache-与脏页)
8. [writeback 如何选择和提交脏数据](#8-writeback-如何选择和提交脏数据)
9. [bio：块层的逻辑 I/O 单位](#9-bio块层的逻辑-io-单位)
10. [submit_bio 与块层入口](#10-submit_bio-与块层入口)
11. [bio 拆分、合并与设备限制](#11-bio-拆分合并与设备限制)
12. [request：设备可调度的命令](#12-request设备可调度的命令)
13. [blk-mq 的软件队列和硬件队列](#13-blk-mq-的软件队列和硬件队列)
14. [I/O scheduler 的作用与边界](#14-io-scheduler-的作用与边界)
15. [从 request 到 NVMe SQ](#15-从-request-到-nvme-sq)
16. [中断、CQ 与完成路径](#16-中断cq-与完成路径)
17. [fsync、flush、FUA 与持久化](#17-fsyncflushfua-与持久化)
18. [direct I/O 与 buffered I/O 对比](#18-direct-io-与-buffered-io-对比)
19. [blk-cgroup、限速与延迟控制](#19-blk-cgroup限速与延迟控制)
20. [并发、锁与生命周期](#20-并发锁与生命周期)
21. [错误、超时、重试与设备移除](#21-错误超时重试与设备移除)
22. [观测实验](#22-观测实验)
23. [回到案例与源码索引](#23-回到案例与源码索引)

---

## 1. 从 `write()` 和 `fsync()` 开始

贯穿全文的例子：

```c
int fd = open("data.bin", O_CREAT | O_TRUNC | O_WRONLY, 0644);
write(fd, buffer, 4096);
fsync(fd);
close(fd);
```

应用看到的是三个简单调用，但至少存在三种不同的“完成”：

```text
write 返回： 数据通常已经复制到 page cache
I/O 完成：   控制器已经完成某个写 request
fsync 返回： 文件系统认为数据和必要元数据满足持久化语义
```

三者不能混为一谈。即使 NVMe 完成了写命令，数据也可能只在设备 volatile write cache 中；即使
数据块已经持久化，目录项或 inode size 也可能尚未满足崩溃一致性要求。

本文围绕这个例子回答：

- `write()` 为什么通常没有立刻提交设备 I/O？
- 文件 offset 如何变成磁盘 sector/LBA？
- folio、bio 和 request 分别表示什么？
- 为什么已经有 bio，还需要 request？
- blk-mq 为什么同时有 software context 和 hardware context？
- NVMe 多队列如何与 CPU 对应？
- 完成中断如何一路唤醒等待 `fsync()` 的任务？
- flush 和 FUA 究竟保证什么？

---

## 2. 背景与需要解决的问题

### 2.1 从单队列磁盘到多队列设备

传统旋转磁盘只有少量命令并发能力，寻道位置决定性能。早期块层围绕一条全局 request queue
排序和合并请求，减少磁头移动是主要目标。

现代 NVMe 设备可以提供大量 submission/completion queue，每队列支持成百上千 outstanding
commands。如果所有 CPU 仍争用一把全局 queue lock，软件会先于硬件成为瓶颈。

### 2.2 块层需要同时解决的问题

- **统一设备接口**：文件系统不应理解 NVMe、SCSI、virtio-blk 的寄存器协议；
- **表达离散内存**：文件数据可能分散在多个 folio/page，设备却需要 DMA segment；
- **遵守硬件限制**：最大 sectors、segment 数、边界、对齐和 zone 规则；
- **高并发扩展**：多个 CPU 同时提交，尽量避免共享锁和 cacheline；
- **排序与公平**：吞吐、读延迟、同步写和不同 cgroup 之间需要协调；
- **异步完成**：提交者不能占着 CPU 等设备，完成可能出现在另一个 CPU；
- **持久化语义**：区分进入设备、进入易失缓存和稳定介质；
- **错误恢复**：超时、重试、reset、设备拔出期间不能泄漏或重复完成请求。

### 2.3 最简单方案的问题

最简单的同步方案：每次 `write()` 都把用户 buffer 转成一条设备命令，轮询到完成再返回。

优点是状态简单，应用容易理解。但它会：

- 把小写变成大量设备命令，无法批量合并；
- 让调用线程在微秒到毫秒级设备延迟中空等；
- 无法用 RAM 吸收 burst；
- 同一文件的多次写难以统一回写和元数据事务；
- 页未对齐、scatter-gather、设备限制仍需额外转换；
- 多 CPU 最终仍可能争用控制器提交锁。

---

## 3. 总体方案及其优劣

### 3.1 Linux 的分层方案

```text
VFS/page cache：吸收和合并用户写，维护文件内容视图
filesystem：    把文件 offset 映射到 extent/block，保证元数据一致性
bio：           描述对块设备某段 sector 的一组内存片段
request：       合并 bio，成为可排序、分配 tag 和发给驱动的命令
blk-mq：        per-CPU software ctx → hardware dispatch queue
driver：        request 翻译成 NVMe/SCSI/virtio 命令和 DMA 描述
device：        异步执行，通过中断或 poll 报告 completion
```

### 3.2 优点

- page cache 把应用生命周期与设备延迟解耦；
- filesystem 可独立实现 extent、journal、COW 和 checksum；
- bio 让上层只描述逻辑块 I/O，不关心设备命令格式；
- request 层可合并、排序、限流和统一超时；
- blk-mq 用 per-CPU ctx 和多 hardware queue 降低锁竞争；
- 异步完成允许一个 CPU 同时推动大量 I/O；
- 同一块层支持本地盘、虚拟设备、device mapper 和网络块设备。

### 3.3 代价和边界

- 一次写入跨多个对象，错误与引用必须逐层传播；
- buffered write 成功不表示持久化，应用必须正确使用 fsync；
- 合并、排序和 batching 可提高吞吐，却增加尾延迟；
- per-CPU 队列减少全局争用，却只能获得近似全局排序；
- bio splitting/cloning 让“一次上层 I/O”对应多个底层完成；
- device mapper、RAID、加密会继续克隆/重映射 bio，使追踪关系复杂；
- direct I/O 绕过 page cache，但带来对齐、pin、缓存一致性和短 I/O 问题。

### 3.4 与其他选择对比

| 选择 | 收益 | 代价 |
|------|------|------|
| 每次 write 同步落盘 | 语义直观 | syscall/命令多，吞吐和延迟差 |
| buffered I/O | 合并写、吸收 burst、统一 mmap/read | 需要回写、节流和 fsync 协议 |
| 单全局 request queue | 全局排序容易 | 多核严重争锁 |
| blk-mq | 多 CPU/多硬件队列扩展 | 全局公平和排序更近似 |
| polling completion | 避免中断切换、低延迟 | 持续占 CPU，低负载浪费 |
| interrupt completion | CPU 利用率好 | 中断、跨 CPU 和调度延迟 |

---

## 4. 宏观地图

### 4.1 子系统地图

```
应用
 write()                                      fsync()
    │                                            │
    ▼                                            ▼
VFS / filesystem                          filesystem fsync
    │ buffered write                           │
    ▼                                          ├─ 写数据/元数据
page cache: folio clean → dirty                └─ flush/FUA/barrier
    │
    │ background/explicit writeback
    ▼
filesystem writepages / iomap
    │ file offset → extent → block sector
    ▼
bio { bdev, sector, op, bio_vec[] }
    │
    ▼
generic block layer
 split / merge / cgroup / rq_qos
    │
    ▼
request { bio chain, tag, hctx }
    │
    ▼
blk-mq: per-CPU ctx → scheduler → hardware ctx
    │ queue_rq
    ▼
NVMe driver: request → command + PRP/SGL → submission queue doorbell
    │
    ▼
NVMe controller/media
    │ completion queue + IRQ/poll
    ▼
nvme completion → blk_mq_end_request → bio_endio
    │
    ▼
folio writeback clear / waiter wake / fsync return
```

### 4.2 对象所有权地图

```
inode
 └─ address_space
     └─ XArray → folio(s)
                   │ writeback 构造/持有
                   ▼
                  bio ──可被 split/clone──> child bio(s)
                   │ merge
                   ▼
                request ──tag──> blk_mq_tags
                   │
                   ├─ mq_ctx：提交 CPU 软件上下文
                   └─ mq_hctx：设备硬件队列上下文
```

### 4.3 三条相互独立的线

```text
数据线： user buffer → folio → bio_vec → DMA → device
控制线： writeback → submit → dispatch → interrupt → endio
持久化线：dirty ordering → filesystem transaction → flush/FUA → fsync result
```

数据已经走完某一层，不代表控制完成或持久化完成。后文始终把三条线分开。

---

## 5. 贯穿案例的阶段划分

### 5.1 `write()` 阶段

```text
用户 4096 bytes
→ VFS write_iter
→ filesystem buffered write
→ 找到/分配 page-cache folio
→ copy_from_user
→ folio_mark_dirty
→ 更新 inode size/time
→ write 返回 4096
```

此时数据通常只在 RAM。若系统掉电，它可能丢失。

### 5.2 writeback 阶段

触发来源可能是后台脏页阈值、定时到期、内存压力、`sync/fsync` 或显式回写：

```text
选择 dirty inode/folio
→ 文件系统建立逻辑 offset 到物理块的映射
→ 构造 bio
→ submit_bio
→ blk-mq request
→ NVMe command
```

### 5.3 completion 阶段

```text
NVMe 写完成
→ CQ entry
→ IRQ/poll
→ nvme_pci_complete_rq
→ blk_mq_end_request
→ request 中每个 bio_endio
→ filesystem end_io
→ folio_end_writeback
→ 唤醒等待该 folio/writeback 的任务
```

### 5.4 fsync 阶段

`fsync()` 要求文件系统把脏数据和必要元数据按崩溃一致顺序提交，并处理设备 volatile cache。
它可能复用已经在途的 writeback，也可能新发数据、journal commit、flush 或 FUA 命令。

---

## 6. 文件偏移如何变成设备扇区

### 6.1 VFS 不知道磁盘布局

`write(fd, ..., offset)` 只知道文件字节偏移。文件可能是：

- ext4 extent 映射；
- XFS delayed allocation；
- Btrfs COW extent；
- sparse hole；
- reflink 共享 extent；
- dm-crypt/LVM 上的逻辑块设备；
- 根本没有块设备的 tmpfs/NFS。

因此 VFS/page cache 维护“文件 offset → folio”，具体文件系统在 writeback/direct I/O 时把它
转换为 extent/block，再转换为 `sector_t`。

### 6.2 延迟分配

buffered write 时立即分配磁盘块会把许多小写固化为碎片。ext4/XFS 等可先只标记内存页和逻辑
范围，writeback 时看到更大连续区间再分配 extent。

收益：更好连续性、更少元数据更新、可合并更大 I/O。代价：`write()` 成功时空间可能尚未真正
分配，后续 writeback 才遇到 ENOSPC；文件系统必须预留/记账以降低这种风险。

### 6.3 iomap

许多现代文件系统使用 iomap 描述一段文件 offset 到后端地址的连续映射：mapped、hole、delalloc、
unwritten、inline 等。通用 iomap buffered/direct/writeback 代码据此切分范围和构造 bio。

unwritten extent 已分配物理空间但读取仍返回零；写成功后再转换为 written，可避免崩溃时暴露
旧磁盘内容。这个转换必须与数据 I/O 完成和 journal 顺序协调。

---

## 7. page cache 与脏页

### 7.1 buffered write 的状态变化

```
初始：folio 不存在或 clean
write_begin/iomap_begin：锁定 folio，准备块映射
copy_from_iter：用户数据进入 folio
write_end/iomap_end：提交实际字节，更新 i_size
folio_mark_dirty：clean → dirty
解锁 folio
```

dirty 表示内存内容新于后端。它是“需要回写”的逻辑状态，不表示 I/O 已经提交。

### 7.2 为什么 write 可以很快返回

用户复制到 RAM 后即可返回，设备延迟由后台 writeback 承担。连续小写可聚合为大 folio/bio，
控制器也能保持深队列。

代价是脏数据占用内存且尚未持久化。若产生速度超过设备带宽，`balance_dirty_pages()` 对写入者
节流，使生产速率逐渐接近 writeback 能力，避免脏页吞噬全部内存。

### 7.3 dirty accounting

脏页按 backing device、writeback domain 和 memcg 等维度记账。全局尚有内存不代表某个慢设备
可无限积累 dirty；阈值和带宽估计会把压力反馈给产生脏页的任务。

### 7.4 dirty、writeback 与 error

```
dirty
  → clear dirty + set writeback
  → submit I/O
  ├─ success：clear writeback
  └─ error：mapping_set_error，按策略重新 dirty/结束
```

异步错误用 `errseq_t` 保存在 mapping，并由 file 记录上次观察位置。之后 `fsync()` 能报告自该
file 上次检查以来的 writeback error，而不是因为原 `write()` 已返回就静默丢失错误。

---

## 8. writeback 如何选择和提交脏数据

### 8.1 谁触发 writeback

- `wb_workfn()`：backing device writeback worker；
- 周期性 old-data flush；
- 脏页达到后台阈值；
- 内存回收遇到脏 folio；
- `sync/fsync`、卸载、freeze；
- laptop mode、cgroup writeback 等策略。

后台回写追求吞吐，fsync 回写追求明确完成范围，内存回收更关注尽快形成可回收 clean folio，
三者的 `writeback_control` 参数和同步级别不同。

### 8.2 inode writeback 队列

dirty inode 挂入 backing device 的不同时间/状态链。`wb_writeback()` 根据 work 预算和原因选择，
`writeback_sb_inodes()` 按 superblock/inode 扫描，调用文件系统 `writepages`/iomap 路径。

不能永久持有全局 inode 锁执行设备 I/O。writeback 先隔离/标记工作，释放列表锁，再进入可能
睡眠的文件系统操作；完成后重新归类 inode。

### 8.3 `writeback_control`

它表达：

```text
sync_mode：WB_SYNC_NONE 或 WB_SYNC_ALL
range_start/range_end：目标文件范围
nr_to_write：扫描预算
for_reclaim/for_sync/cyclic：调用原因与扫描方式
```

`WB_SYNC_NONE` 通常提交后不逐页等待；`WB_SYNC_ALL` 用于同步语义，但最终持久化仍要结合文件
系统 journal 和设备 cache flush。

### 8.4 folio writeback 锁定

writeback 在 folio lock 下清 dirty、设置 writeback，防止并发写入和回写状态丢失。用户在 I/O
期间再次写同一 folio，可重新设置 dirty；旧 I/O 完成只清 writeback，不能误清新 dirty 数据。

这正是 dirty 和 writeback 使用不同 bit 的原因。

---

## 9. `bio`：块层的逻辑 I/O 单位

### 9.1 核心字段

`struct bio` 定义于 `include/linux/blk_types.h`：

```c
struct bio {
    struct block_device *bi_bdev;
    blk_opf_t             bi_opf;
    blk_status_t          bi_status;
    struct bio_vec       *bi_io_vec;
    struct bvec_iter      bi_iter;
    bio_end_io_t         *bi_end_io;
    void                 *bi_private;
    unsigned short        bi_vcnt;
    atomic_t              __bi_remaining;
    atomic_t              __bi_cnt;
};
```

它表达：对哪个 block device、从哪个 sector、执行 READ/WRITE/FLUSH/DISCARD 等操作，数据位于
哪些 page fragments，以及完成后调用谁。

### 9.2 `bio_vec`

```c
struct bio_vec {
    struct page *bv_page;
    unsigned int bv_len;
    unsigned int bv_offset;
};
```

bio 的逻辑 sector 范围连续，但内存页可以离散。驱动/DMA 层把 bvec 转成 scatter-gather list，
相邻物理片段若满足限制还可合并为更少 DMA segment。

### 9.3 bio 不是 request

bio 保留上层 I/O 的完成边界和内存描述；request 是块层/驱动的调度命令，可包含多个相邻且兼容
bio：

```
bio A ─┐
bio B ─┼→ request R → 一个 NVMe command
bio C ─┘
```

request 完成后分别推进 A/B/C 的 completion。若只保留 request，就难以把完成结果还给各自上层
folio、direct-I/O kiocb 或 device-mapper clone。

### 9.4 bio chaining

一个上层 bio 被拆成多个 child 时，parent 的 `__bi_remaining` 统计未完成分支。每个 child
`bio_endio()` 递减；只有最后一个完成才调用 parent end_io。第一个错误被合并到 parent status。

这使上层保持“一次逻辑 I/O 完成一次”的语义，同时允许底层按设备限制任意拆分。

---

## 10. `submit_bio()` 与块层入口

### 10.1 调用骨架

```
filesystem 构造 bio
  → submit_bio(bio)
      → 统计、cgroup 关联和通用检查
      → submit_bio_noacct(bio)
          → 设备栈重映射/递归提交保护
          → submit_bio_noacct_nocheck()
              → split/完整性/加密/限速等
              → blk_mq_submit_bio()
```

具体入口会随 block device 类型进入分区重映射、device mapper 或驱动自定义 `submit_bio`。每层
都可修改 `bi_bdev/sector`、clone bio 并设置自己的 end_io，再向下一层提交。

### 10.2 为什么要防递归提交

stacked device 的 `make_request`/submit 回调可能同步提交下一层 bio；若多层映射都直接递归，
深栈或异常配置会耗尽内核栈。块层使用 per-task bio list 等机制把递归转换成迭代处理，并保证
新产生 bio 的顺序。

### 10.3 plugging

`blk_start_plug()` 暂存当前 task 产生的一批 request/bio，在 plug 结束、schedule 或显式 flush 时
批量提交。文件系统写多个相邻块时，plug 给块层看到更多合并机会，也减少 doorbell/锁次数。

代价是增加微小提交延迟；阻塞前必须 flush plug，否则 task 可能等待自己尚未真正提交的 I/O。

---

## 11. bio 拆分、合并与设备限制

### 11.1 queue limits

`request_queue.limits` 来自驱动、控制器和 stacked device 约束，包括：

- 最大 sectors/segment 数；
- logical/physical block size；
- DMA alignment 和 boundary；
- discard/write-zeroes 限制；
- zone size、append、atomic write 单元；
- integrity、crypto 和 IOMMU granule 相关限制。

上层不能假设 1 MiB bio 可被设备原样接受。`bio_split_io_at()`/queue split 按限制产生 child bio，
并用 chaining 保持原完成语义。

### 11.2 merge 的条件

bio 可与 request 前/后合并，通常要求：

```text
同一 queue/partition
相同 op 和关键 flags
sector 连续
合并后不超过 sectors/segments/boundary 限制
write hint、crypto、integrity、zone 语义兼容
```

合并减少命令和中断，提高顺序吞吐；但过度等待合并会增加同步读尾延迟，所以调度器和 plug 的
batching 都有边界。

### 11.3 split 与 merge 的可观测结果

一个 1 MiB 上层 write 可能：

- 被文件系统分成多个 bio；
- 每个 bio 因硬件限制再 split；
- 相邻 child 又和别的 bio merge 到 request；
- NVMe PRP/SGL 再描述多个 DMA segment。

因此 syscall 数、bio 数、request 数和硬件 command 数没有一一对应关系。

---

## 12. `request`：设备可调度的命令

### 12.1 核心状态

`struct request` 定义于 `include/linux/blk-mq.h`：

```text
q / mq_ctx / mq_hctx       queue 与软件/硬件上下文
cmd_flags                  READ/WRITE/FLUSH 等
tag / internal_tag         驱动与 scheduler tag
__sector / __data_len      逻辑设备范围
bio / biotail              bio 链
state                      MQ_RQ_IDLE/IN_FLIGHT/COMPLETE
deadline / timeout         超时管理
end_io / end_io_data       特殊完成回调
```

tag 是 hardware queue 中 outstanding command 的槽位标识。驱动常用 tag 直接索引 command info，
completion queue 返回 command ID 后能 O(1) 找到 request。

### 12.2 request 生命周期

```
分配 tag/request
→ 初始化并附加 bio
→ scheduler/ctx 排队，或 direct issue
→ blk_mq_start_request：开始 timeout/accounting
→ driver queue_rq
→ IN_FLIGHT
→ device completion
→ blk_mq_complete_request
→ blk_mq_end_request
→ 结束 bio、统计、释放 tag/request
```

tag 必须在真正完成后才释放，否则新 request 复用同 tag，而迟到 completion 会完成错误对象。

### 12.3 resource shortage

driver `queue_rq()` 可返回 `BLK_STS_RESOURCE/DEV_RESOURCE`，表示暂时无法接收。块层不能把它当
永久 I/O 错误，而要把 request 放回 dispatch/requeue，等待 tag、queue space 或设备状态恢复。

这给驱动施加约束：返回 resource 前后的 request ownership 和 started 状态必须符合 blk-mq 契约。

---

## 13. blk-mq 的软件队列和硬件队列

### 13.1 三层映射

```
提交 CPU
  └─ per-CPU blk_mq_ctx
       └─ queue map 选择 blk_mq_hw_ctx
            └─ driver hardware submission queue
```

`blk_mq_ctx` 保存每 CPU 提交上下文和统计，避免所有 CPU 修改同一软件链表；`blk_mq_hw_ctx`
聚合映射到同一硬件队列的 CPU，包含 dispatch、tags、cpumask、run work 和 driver data。

### 13.2 `blk_mq_tag_set`

驱动初始化 tag set，描述：

- hardware queue 数；
- queue depth 和 reserved tags；
- 每 request 的 driver private data 大小；
- `blk_mq_ops`；
- queue mapping 和 NUMA node；
- blocking/polling/shared-tag 等 flags。

块层据此分配 tags、request pool、hctx 并创建 `request_queue`。NVMe 通常把 I/O queue 映射到
CPU，使 submission 与 completion 尽量保持局部性。

### 13.3 `blk_mq_submit_bio()`

核心决策：

```text
尝试与已有 request merge
→ 不能 merge：分配 request/tag
→ 初始化 request 并挂 bio
→ 有 scheduler：插入 scheduler queue
→ 无 scheduler且条件允许：try_issue_directly
→ 否则插入 ctx/dispatch 并运行 hctx
```

direct issue 减少排队和调度开销，适合空闲高速设备；资源不足或需要排序时退回排队路径。

### 13.4 dispatch

`blk_mq_sched_dispatch_requests()` 从 scheduler、hctx dispatch list 或 ctx 获取 request，
`blk_mq_dispatch_rq_list()` 逐个调用 driver `queue_rq()`。成功后 request 归驱动/设备；resource
返回则停止或重新排队，避免 busy loop。

### 13.5 优点和代价

优点：提交热路径大多局部于 CPU/hctx，硬件并行度能被充分利用。代价：多个 hctx 间不存在免费
的严格全局排序；CPU hotplug、queue remap、shared tags 和 scheduler 切换需要 freeze/quiesce。

---

## 14. I/O scheduler 的作用与边界

### 14.1 scheduler 位于哪里

I/O scheduler（elevator）作用于 request，而非用户 syscall 或原始 folio：

```
bio → merge/request allocation → elevator insert
                            ↓
                    elevator dispatch
                            ↓
                       hctx/driver
```

它可重排未下发 request；一旦 command 已进入设备内部队列，主机 scheduler 通常不能收回重新排序。

### 14.2 mq-deadline 与 BFQ 的取舍

- mq-deadline：维护读/写排序和 deadline，防止请求无限饥饿，适合需要可控延迟的通用场景；
- BFQ：按进程/组预算服务，强调交互响应和带宽公平，复杂度和 CPU 开销更高；
- none：尽快 dispatch，让 NVMe/虚拟设备自己处理，软件开销最低。

高速 NVMe 上 scheduler 能做的寻道优化很少，但仍可能提供读优先、合并和 cgroup 延迟隔离。
“NVMe 一律用 none”不是普遍真理，应根据 workload、设备和服务目标测量。

### 14.3 scheduler tag 与 driver tag

有 scheduler 时 request 可先持 internal/scheduler tag，在真正 dispatch 时再获得 driver tag；这样
排队 request 不占满有限硬件 command slots。无 scheduler/direct issue 路径可更早取得 driver tag。

---

## 15. 从 request 到 NVMe SQ

### 15.1 `nvme_queue_rq()`

NVMe PCI 驱动通过 `blk_mq_ops.queue_rq` 接收 request。简化过程：

```
nvme_queue_rq(hctx, bd)
  → 检查 controller/namespace 状态
  → nvme_prep_rq()
      → nvme_setup_cmd()：REQ_OP 转 NVMe opcode/LBA/length
      → nvme_map_data()：bio/request segments → PRP 或 SGL
      → nvme_start_request()
  → 持有 nvmeq->sq_lock
  → nvme_sq_copy_cmd()：写 SQ entry 并推进 tail
  → nvme_write_sq_db()：按 batching 条件写 doorbell
```

真正顺序会因 passthrough、metadata、discard、zone append、PI、poll queue 等变化。关键 ownership
边界是 queue_rq 成功后，request 直到 completion 都由设备路径推进。

当前 PCI 驱动的关键发布代码是：

```c
ret = nvme_prep_rq(req);
if (unlikely(ret))
    return ret;

spin_lock(&nvmeq->sq_lock);
nvme_sq_copy_cmd(nvmeq, &iod->cmd);
nvme_write_sq_db(nvmeq, bd->last);
spin_unlock(&nvmeq->sq_lock);
return BLK_STS_OK;
```

`bd->last` 让批量 dispatch 只在合适边界敲 doorbell，减少 MMIO；代价是前面的 command 要等同批
最后一个 request 一起通知控制器。

### 15.2 PRP/SGL 与 DMA

CPU 虚拟地址不能直接交给设备。DMA API 把 request 的物理片段映射成设备可访问 DMA address；
NVMe 使用 PRP entries 或 SGL 描述数据页。

若片段少，可内嵌在 command；片段多则分配 PRP list/SGL buffer。IOMMU 可能让设备地址连续性
不同于 CPU 物理连续性，驱动必须使用 DMA API，不能手算物理地址。

### 15.3 submission queue

SQ 是主机内存中的环形队列。驱动填好 command 后，用适当 DMA write barrier 保证 controller 在
看到 tail doorbell 前已经看到完整 entry，然后写 MMIO doorbell。

缺少屏障时，弱序 CPU/PCIe 可能让 doorbell 先到，controller 读取到半初始化 command。这是
硬件并发协议，不是普通编译器优化问题。

---

## 16. 中断、CQ 与完成路径

### 16.1 controller 如何报告完成

NVMe controller 把 CQE 写入 host completion queue，更新 phase/tag 状态并触发 MSI-X；每个 queue
通常有对应 interrupt vector，也可使用 polling queue。

驱动读取 CQE command ID，根据 tag 找到 request，解析 status/result，并推进 SQ head/CQ head。

### 16.2 完成调用链

```
NVMe IRQ/poll
  → nvme_poll_cq()/process CQ
  → nvme_pci_complete_rq(req)
      → 解除 DMA mapping、释放 PRP/SGL
      → nvme_complete_rq()
      → blk_mq_complete_request(req)
          → 本地或远端 completion
          → blk_mq_end_request(req, status)
              → request bio chain
              → bio_endio(bio)
                  → filesystem end_io
                  → folio_end_writeback()
                  → wake_up waiters
              → 释放 tag/request
```

完成可能在提交 CPU、IRQ CPU 或软件重定向 CPU 执行。上层 end_io 不能依赖 `current` 是原提交者，
也不能在不可睡眠 completion context 做阻塞操作。

### 16.3 为什么有 completion affinity

在提交 CPU 完成可利用 cache locality，但跨 CPU IPI 有成本；在 IRQ CPU 直接完成减少 IPI，却
可能让单 CPU 承担大量 completion。blk-mq 根据 queue mapping、flags 和系统拓扑选择策略。

### 16.4 status 传播

设备 status 转为 `blk_status_t`，request end 再传播到 bio `bi_status`，文件系统记录 mapping
writeback error，最终由 fsync 或后续 I/O 返回 errno。

一个 request 含多个 bio 时，错误必须传播给每个相关逻辑 I/O，但每个 bio 的 completion 仍只
调用一次。

---

## 17. fsync、flush、FUA 与持久化

### 17.1 数据到哪里才算安全

```
CPU cache
→ DRAM page cache
→ controller/drive volatile cache
→ non-volatile media
```

普通 write 只保证前两层中的内核视图。设备报告普通 WRITE 完成，可能表示数据已进入受电池保护
或不受保护的 write cache。持久化需要文件系统与设备 cache contract 配合。

### 17.2 `vfs_fsync_range()`

```
fsync(fd)
  → vfs_fsync_range(file, 0, LLONG_MAX, 0)
  → file->f_op->fsync()
      → filemap_write_and_wait_range()
      → 提交/等待数据 writeback
      → 提交 journal/metadata transaction
      → 必要时 blkdev_issue_flush 或 FUA write
      → 检查 mapping errseq
```

具体顺序由文件系统决定。日志模式、data=ordered、COW tree、checksum 和设备能力会改变命令组合。

### 17.3 preflush 和 FUA

- `REQ_PREFLUSH`：在相关写之前要求设备把更早 volatile writes 持久化；
- `REQ_FUA`：要求该写完成时数据已到稳定介质；
- 单独 FLUSH：清空设备易失写缓存。

典型 barrier transaction 可表达为：

```text
写旧数据/元数据
→ preflush，保证之前写持久化
→ 写 commit record + FUA
```

设备若原生支持 FUA 可减少额外 round trip；不支持时块层/驱动需要组合 flush sequence。

### 17.4 fsync 的边界

`fsync(file)` 主要保证该文件数据和使其可恢复所需元数据。新建或 rename 后，若应用要求目录项
本身跨崩溃持久，通常还要 `fsync(parent_dir)`。设备谎报 flush、掉电保护失效或硬件错误超出
内核软件保证。

---

## 18. direct I/O 与 buffered I/O 对比

### 18.1 direct I/O 主线

```
read/write with O_DIRECT
→ filesystem direct_IO/iomap_dio_rw
→ fault/pin user pages
→ 建 extent mapping
→ bio 直接引用用户页
→ submit_bio/blk-mq
→ completion
→ unpin pages，完成 kiocb
```

它主要绕过 page cache 数据缓存，不绕过 filesystem、bio、blk-mq 和驱动。

### 18.2 优点

- 数据库等自有缓存避免 page cache 双缓存；
- 控制 I/O 提交与完成时机；
- 大型对齐 I/O 可减少一次用户—page-cache copy；
- 减少脏页后台回写的不确定性。

### 18.3 代价

- 对齐和文件系统限制可能导致 EINVAL、短 I/O 或 fallback；
- 用户页 pin 影响 COW、迁移和回收；
- 小 I/O 无 page cache 合并，命令开销更明显；
- 与 mmap/buffered I/O 混用需 invalidate/flush，锁更复杂；
- O_DIRECT 不等于 O_SYNC，完成也不自动等于掉电持久。

### 18.4 选择原则

一般应用优先 buffered I/O；数据库、虚拟机和专用存储引擎在拥有缓存、对齐和 durability 控制时
才更适合 direct I/O。选择应基于 tail latency、CPU copy、cache pressure 和设备 queue depth
测量，而不是只比较顺序吞吐。

---

## 19. blk-cgroup、限速与延迟控制

### 19.1 bio 的 cgroup 关联

bio 可关联 `blkcg_gq`，表示某 blkcg 在某 request_queue 上的状态。I/O 在 merge、clone 和
stacked device 重映射时要保持或重新关联正确身份。

### 19.2 控制机制

- `io.max`：按设备限制 BPS/IOPS；
- `io.weight`：竞争时分配相对权重；
- `io.latency`/iocost 等策略：根据延迟目标或成本模型控制；
- writeback attribution：脏 page cache 最终回写应归因到产生脏数据的 memcg/blkcg。

buffered write 的提交者与真正执行 writeback 的内核线程不同，若只按当前线程归因，所有 I/O 都
会算到 root/worker。cgroup writeback 因此把 inode/backing_dev writeback 实例与 memcg 关联。

### 19.3 `rq_qos`

request queue 的 rq_qos 链可在 bio/request 分配、issue 和 completion 时介入，实现 writeback
throttling、latency、iocost 等。它们可能让 task 在看似“块设备空闲”时等待 token/深度。

排查 I/O 延迟必须区分：page-cache dirty throttling、blkcg throttle、scheduler queue、driver
queue 和设备 service time。

---

## 20. 并发、锁与生命周期

### 20.1 不存在一把块层总锁

| 机制 | 保护内容 |
|------|----------|
| folio lock/flags | dirty、writeback、内容填充 |
| mapping XArray lock | page-cache 索引修改 |
| wb list locks | dirty inode/writeback 队列 |
| per-CPU `blk_mq_ctx` | CPU 本地提交状态 |
| hctx lock/dispatch lock | 硬件队列 dispatch 与 scheduler 状态 |
| sbitmap tags | 并发 tag 分配 |
| request state/ref | issue、complete、timeout 竞态 |
| queue freeze/quiesce | 重配置、驱动移除与在途 I/O 边界 |

### 20.2 bio ownership

submit 后，上层不能继续随意修改 bio；ownership 转给下层，直到 `bi_end_io`。若上层需要保留，
必须 `bio_get()`；clone 只共享数据页，不自动复制所有私有状态。

`bio_endio()` 可能同步发生：某些虚拟设备或错误路径在 submit 调用栈内直接完成。调用者不能
假定“submit 返回后 end_io 才可能运行”。

### 20.3 request completion 与 timeout

设备完成和 timeout handler 可能并发。request state transition 保证只有一条路径最终 end；
timeout 回调可要求 reset、延长、完成或重试，但必须处理此刻硬件 completion 正在到达。

双重 `blk_mq_end_request()` 会重复 bio_endio、释放 tag 并造成 use-after-free，是驱动最严重的
错误类型之一。

### 20.4 queue freeze 与 quiesce

- freeze：阻止新的 queue usage，并等待进入队列的使用者退出，适合重配置/销毁；
- quiesce：停止 dispatch/driver queue_rq，使已进入块层的 request 暂停推进；
- 两者解决的边界不同，经常需要按顺序组合。

切换 I/O scheduler、改变 hardware queue 数、reset/remove 设备时，要先建立稳定边界，再替换
hctx/tags/driver data，最后恢复队列。

### 20.5 memory ordering

NVMe SQ/CQ 是 CPU 和设备共享内存：

```text
提交：写 command fields → DMA write barrier → 写 doorbell
完成：观察 phase/doorbell → DMA read barrier → 读取 CQE 其他字段
```

普通 spinlock 只协调 CPU，不一定替代 DMA barrier。驱动必须使用面向设备一致性的 API。

---

## 21. 错误、超时、重试与设备移除

### 21.1 错误传播链

```
NVMe status
→ nvme/blk status
→ request error
→ bio->bi_status
→ filesystem end_io
→ mapping errseq / inode state
→ fsync/read/write/direct-I/O errno
```

异步 buffered write 的原 syscall 已返回，错误只能延迟到 fsync/close 或后续相关操作。可靠应用
必须检查 fsync 返回值；仅检查 write 不足以确认落盘成功。

### 21.2 timeout 与 reset

request 发出时启动 timeout。超时不证明设备永远不会完成，它可能只是中断丢失或控制器卡顿。
NVMe timeout 路径可能 abort command，进一步失败则 reset controller：停止 queues、禁用控制器、
重新建立 admin/I/O queues，并完成或重试 outstanding requests。

重试必须遵守操作幂等性和状态。普通读易重试；写、zone append、discard 或 passthrough command
可能已经部分执行，不能无条件重复。

### 21.3 设备移除

热拔时要阻止新 I/O、quiesce/freeze queue、让在途 request 以错误完成、撤销 disk/namespace
可见性，最后等待 queue references 和 RCU 读者后释放结构。

和进程退出一样，“用户不再能打开设备”“所有 I/O 已完成”“request_queue 已释放”是不同阶段。

---

## 22. 观测实验

### 22.1 最小程序

```c
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    char *buf;
    if (posix_memalign((void **)&buf, 4096, 4096))
        return 1;
    for (int i = 0; i < 4096; i++)
        buf[i] = (char)i;

    int fd = open("data.bin", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0 || write(fd, buf, 4096) != 4096 || fsync(fd) < 0) {
        perror("write/fsync");
        return 1;
    }
    close(fd);
    free(buf);
    return 0;
}
```

### 22.2 先验证系统调用语义

```bash
strace -T -e trace=openat,write,fsync,close ./io-demo
```

比较 write 与 fsync 延迟。首次执行还可能包含文件创建、extent 分配和 journal commit，不能把
全部 fsync 时间当成纯 NVMe write latency。

### 22.3 block tracepoints

```bash
trace-cmd record \
  -e block:block_bio_queue \
  -e block:block_bio_complete \
  -e block:block_rq_insert \
  -e block:block_rq_issue \
  -e block:block_rq_complete \
  ./io-demo

trace-cmd report
```

关注：

```text
bio_queue → rq_insert：块层排队/合并阶段
rq_insert → rq_issue：scheduler/plug/cgroup 等等待
rq_issue → rq_complete：驱动和设备 service time
bio 与 request 数量不同：split/merge 的证据
```

tracepoint 名称和字段受配置/版本影响，应先查看 tracing `available_events`。

### 22.4 blktrace 与 iostat

```bash
iostat -x 1
cat /sys/block/nvme0n1/queue/scheduler
cat /sys/block/nvme0n1/queue/nr_requests
ls /sys/block/nvme0n1/mq/
```

`iostat await` 包含排队和设备时间的聚合视图，不等于硬件 service latency；`aqu-sz` 反映平均
队列深度。短实验受 page cache 和统计周期影响，需配合 fsync/direct I/O 或足够长 workload。

### 22.5 eBPF/ftrace 关联

可按 `(dev, sector, bytes)` 或 request 指针关联 queue/issue/complete，但 request 会 merge，bio
会 split，指针也会复用。可靠分析需要限定生命周期并记录时间戳、tag/hctx 和 operation。

要关联用户 write 到设备 command，还要同时跟踪 writeback，因为真正提交 I/O 的可能是 worker，
不能只按提交时 `current->pid` 归因。

---

## 23. 回到案例与源码索引

### 23.1 4096 字节写入的完整过程

```
write(fd, 4096)
  → 数据复制到 inode->i_mapping 中的 folio
  → folio dirty
  → write 返回；数据尚未必进入设备

fsync(fd)
  → filesystem fsync
  → filemap_write_and_wait_range
  → writeback 锁定 dirty folio
  → 文件 offset 映射为 extent/sector
  → bio 引用 folio page fragments
  → submit_bio
  → bio 按 queue limits split，或与相邻 bio merge
  → blk_mq_submit_bio
  → request + tag
  → scheduler/dispatch
  → nvme_queue_rq
  → DMA mapping + NVMe command + SQ doorbell
  → controller 写介质/cache
  → CQ + MSI-X
  → nvme completion
  → blk_mq_end_request
  → bio_endio
  → folio writeback 结束
  → filesystem journal/metadata/flush 条件满足
  → 检查 writeback error
  → fsync 返回 0
```

### 23.2 关键函数索引

| 阶段 | 函数 | 位置 | 作用 |
|------|------|------|------|
| 脏页节流 | `balance_dirty_pages()` | `mm/page-writeback.c` | 让写入速度匹配回写能力 |
| 后台回写 | `wb_workfn()` / `wb_writeback()` | `fs/fs-writeback.c` | 选择 dirty inode 并推进 writeback |
| 同步数据 | `filemap_write_and_wait_range()` | `mm/filemap.c` | 提交并等待范围 writeback |
| fsync 入口 | `vfs_fsync_range()` | `fs/sync.c` | 调用具体文件系统 fsync |
| bio 提交 | `submit_bio()` / `submit_bio_noacct()` | `block/blk-core.c` | 块层入口与设备栈处理 |
| bio 拆分 | `bio_split_io_at()` | `block/blk-merge.c` | 按 queue limits 拆分 |
| MQ 提交 | `blk_mq_submit_bio()` | `block/blk-mq.c` | merge、request/tag、排队或直发 |
| MQ 调度 | `blk_mq_sched_dispatch_requests()` | `block/blk-mq-sched.c` | 从 scheduler/ctx 取 request |
| 驱动分发 | `blk_mq_dispatch_rq_list()` | `block/blk-mq.c` | 调用 driver queue_rq |
| 开始计时 | `blk_mq_start_request()` | `block/blk-mq.c` | 标记 in-flight 和启动 timeout |
| NVMe 提交 | `nvme_queue_rq()` | `drivers/nvme/host/pci.c` | 构造命令、DMA 并写 SQ |
| MQ 完成 | `blk_mq_complete_request()` | `block/blk-mq.c` | 调度 request 完成上下文 |
| request 结束 | `blk_mq_end_request()` | `block/blk-mq.c` | 结束 bio 链并释放 request |
| bio 结束 | `bio_endio()` | `block/bio.c` | 聚合 child 并调用上层 end_io |
| flush | `blkdev_issue_flush()` | `block/blk-flush.c` | 提交设备 cache flush |

### 23.3 五个不变量

1. **write 返回、块 I/O 完成和持久化完成是三个不同边界。**
2. **folio 表示缓存内容，bio 表示逻辑块 I/O，request 表示可调度的设备命令。**
3. **split/merge 使 syscall、folio、bio、request 和硬件 command 从来不保证一一对应。**
4. **提交后 ownership 转给下层，完成通过 tag/request/bio 回调逐层归还，且只能完成一次。**
5. **blk-mq 用局部队列换取扩展性，再通过 scheduler、QoS 和 dispatch 做近似全局协调。**

理解这些不变量后，就能把文件系统 writeback、块层调度和 NVMe 驱动看成同一条异步流水线，
并能判断性能问题究竟发生在脏页、映射、排队、设备执行还是完成唤醒阶段。
