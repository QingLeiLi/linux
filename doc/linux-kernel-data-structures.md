# Linux 内核核心数据结构

> 面向对象视角：每个结构体视为一个"类"，字段是"属性"，`ops` 指针是"虚函数表"。

---

## 一、基础设施（所有子系统的底层）

### `list_head` — 侵入式双向链表节点

```c
struct list_head {
    struct list_head *next, *prev;
};
```

- 嵌入到其他结构体内部，通过 `container_of` 反向拿到宿主
- Linux 几乎所有链表都用这个，不是"链表节点持有数据"而是"数据持有链表节点"
- **注意**：操作必须在锁保护下，或确认是 per-CPU 场景

---

### `rb_node` / `rb_root` — 红黑树

```c
struct rb_node {
    unsigned long __rb_parent_color;  // 父节点指针 + 颜色bit复用
    struct rb_node *rb_right, *rb_left;
};
```

- VMA、定时器、进程 pid 都用红黑树
- **注意**：颜色和父指针打包在同一个字段，直接读没有意义

---

### `kref` — 引用计数基类

```c
struct kref {
    refcount_t refcount;
};
```

- 类比 C++ `shared_ptr` 的控制块
- `kref_get` / `kref_put`，put 到 0 时调用 release 回调
- **注意**：`refcount_t` 有溢出保护，不要直接用 `atomic_t` 替代

---

## 二、进程管理

### `task_struct` — 进程/线程描述符（最核心的结构体）

```c
struct task_struct {
    // 调度相关
    volatile long        state;          // TASK_RUNNING / TASK_INTERRUPTIBLE 等
    int                  prio;           // 动态优先级
    int                  static_prio;    // 静态优先级（nice值换算）
    struct sched_entity  se;             // 挂入CFS红黑树的节点

    // 身份
    pid_t                pid;            // 进程id
    pid_t                tgid;           // 线程组id（getpid()返回这个）
    struct task_struct  *group_leader;   // 主线程指针
    struct list_head     thread_group;   // 同线程组的链表

    // 内存
    struct mm_struct    *mm;             // 用户态内存描述符
    struct mm_struct    *active_mm;      // 内核线程借用的mm

    // 文件系统
    struct fs_struct    *fs;             // 根目录/当前目录
    struct files_struct *files;          // 打开文件表

    // 信号
    struct signal_struct    *signal;     // 线程组共享的信号
    struct sighand_struct   *sighand;    // 信号处理函数表
    sigset_t                 blocked;    // 被屏蔽的信号掩码

    // 权限
    const struct cred       *real_cred;  // 真实uid/gid
    const struct cred       *cred;       // 有效uid/gid（sudo后变化）

    // 亲属关系
    struct task_struct  *real_parent;    // 真实父进程
    struct list_head     children;       // 子进程链表
    struct list_head     sibling;        // 兄弟进程链表节点

    // namespace
    struct nsproxy      *nsproxy;        // 指向各种namespace

    // cgroup
    struct css_set      *cgroups;        // 所属cgroup

    // 栈
    void                *stack;          // 内核栈指针（指向 thread_info）
};
```

**注意：**
- `mm == NULL` 表示内核线程
- `pid != tgid` 表示这是一个非主线程
- `cred` 是 RCU 保护的，读取需要 `rcu_read_lock()` 或 `get_cred()`

---

### `cred` — 权限凭证

```c
struct cred {
    kuid_t   uid, euid, suid, fsuid;   // real/effective/saved/fs uid
    kgid_t   gid, egid, sgid, fsgid;
    struct group_info *group_info;      // 附加组
    kernel_cap_t cap_inheritable;       // 可继承capability
    kernel_cap_t cap_permitted;         // 允许集合
    kernel_cap_t cap_effective;         // 当前生效集合
    struct user_struct *user;           // 指向uid对应的user对象
};
```

**注意：**
- `cred` 是不可变的，修改权限必须 `prepare_creds()` 复制一份再 `commit_creds()`
- 提权漏洞的终点通常是覆盖 `cred` 里的 uid 为 0

---

### `sched_entity` — CFS 调度实体

```c
struct sched_entity {
    struct load_weight   load;       // 权重（由nice值决定）
    struct rb_node       run_node;   // 挂入CFS红黑树的节点
    u64                  vruntime;   // 虚拟运行时间（CFS排序key）
    struct cfs_rq       *cfs_rq;    // 所在的运行队列
};
```

**注意：**
- CFS 选最小 `vruntime` 的实体运行
- `vruntime` 增长速度和权重成反比，nice 值低的增长慢

---

## 三、内存管理

### `mm_struct` — 进程虚拟内存空间

```c
struct mm_struct {
    struct maple_tree   mm_mt;        // 存储所有VMA的maple tree（新内核）
    // 老内核是 struct rb_root mm_rb; 红黑树

    unsigned long       mmap_base;    // mmap区域起始地址
    unsigned long       task_size;    // 用户空间上限

    pgd_t              *pgd;          // 页全局目录（页表根）

    // 各段地址范围
    unsigned long       start_code, end_code;
    unsigned long       start_data, end_data;
    unsigned long       start_brk, brk;   // 堆的起止
    unsigned long       start_stack;

    atomic_t            mm_users;     // 共享此mm的线程数
    atomic_t            mm_count;     // mm_struct自身引用数

    struct rw_semaphore mmap_lock;    // 保护VMA树的读写锁
};
```

**注意：**
- `mm_users` 和 `mm_count` 的区别：`mm_users` 是使用者（线程），`mm_count` 包含内核借用者
- 操作 VMA 必须持有 `mmap_lock`，读操作用 `mmap_read_lock`，写用 `mmap_write_lock`
- **这是内核漏洞里竞争条件的高频战场**

---

### `vm_area_struct` (VMA) — 虚拟内存区域

```c
struct vm_area_struct {
    unsigned long       vm_start, vm_end;   // 虚拟地址范围 [start, end)
    struct mm_struct   *vm_mm;              // 所属mm
    pgprot_t            vm_page_prot;       // 页保护标志
    unsigned long       vm_flags;           // VM_READ/VM_WRITE/VM_EXEC/VM_SHARED

    struct file        *vm_file;            // 映射的文件（匿名映射为NULL）
    unsigned long       vm_pgoff;           // 文件映射偏移（单位页）

    const struct vm_operations_struct *vm_ops;  // 虚函数表
};

// 虚函数表
struct vm_operations_struct {
    void       (*open)(struct vm_area_struct *area);
    void       (*close)(struct vm_area_struct *area);
    vm_fault_t (*fault)(struct vm_fault *vmf);   // 缺页中断处理
};
```

**注意：**
- VMA 是 `mmap()` 的直接产物，每次 mmap 创建一个 VMA
- `vm_flags` 里的 `VM_WRITE` 和页表里的 `_PAGE_RW` 是两层，VMA 是逻辑权限，页表是硬件权限
- COW 的原理：fork 后子进程的 VMA `VM_WRITE` 仍然置位，但页表 `_PAGE_RW` 清零，写时触发缺页中断完成复制

---

### `page` / `folio` — 物理页描述符

```c
struct page {
    unsigned long       flags;        // PG_locked/PG_dirty/PG_uptodate 等
    union {
        struct address_space *mapping; // 文件页指向address_space，匿名页指向anon_vma
        void *s_mem;                   // slab使用
    };
    union {
        pgoff_t         index;         // 在文件/交换区的偏移
        unsigned long   private;       // 各子系统自用
    };
    atomic_t            _refcount;    // 引用计数
    atomic_t            _mapcount;    // 被页表映射的次数
    struct list_head    lru;          // LRU链表节点
};
```

**注意：**
- 每个物理页都有一个 `page` 结构体，存放在 `mem_map` 数组里
- `_mapcount == -1` 表示没有页表映射
- `folio` 是新内核对复合页的封装，逐渐替代直接使用 `page`
- **这个结构体字段有大量 union 复用，不同场景含义完全不同**

---

### `address_space` — 页缓存（文件的内存映射管理）

```c
struct address_space {
    struct inode            *host;       // 所属inode
    struct xarray           i_pages;     // 页缓存基树（存储文件的物理页）
    const struct address_space_operations *a_ops;  // 虚函数表
    unsigned long           nrpages;     // 缓存页数量
    struct rw_semaphore     invalidate_lock;
};

struct address_space_operations {
    int (*writepage)(struct page*, struct writeback_control*);
    int (*read_folio)(struct file*, struct folio*);
    // ...
};
```

**注意：**
- 文件的所有缓存页都通过 `address_space` 管理，VMA 的文件映射缺页中断最终查这里
- 同一文件多次 mmap 共享同一个 `address_space`

---

### `kmem_cache` — slab 分配器缓存

```c
struct kmem_cache {
    unsigned int    object_size;    // 对象实际大小
    unsigned int    size;           // 分配大小（含对齐填充）
    unsigned int    align;
    const char     *name;           // 显示在 /proc/slabinfo
    struct list_head list;          // 全局slab_caches链表节点
    // per-CPU 缓存、partial链表等（简化）
};
```

**注意：**
- `kmalloc` 背后是一组按大小预建的 `kmem_cache`
- 漏洞利用中的 heap grooming 就是操控特定 cache 的空闲对象布局

---

## 四、VFS

### `super_block` — 文件系统实例

```c
struct super_block {
    dev_t               s_dev;          // 设备号
    unsigned long       s_blocksize;
    unsigned char       s_blocksize_bits;
    struct file_system_type *s_type;    // 文件系统类型（ext4/xfs等）
    const struct super_operations *s_op; // 虚函数表
    struct dentry       *s_root;        // 根dentry
    struct list_head    s_inodes;       // 所有inode链表
};
```

---

### `inode` — 文件元数据

```c
struct inode {
    umode_t             i_mode;     // 文件类型 + 权限位
    kuid_t              i_uid;
    kgid_t              i_gid;
    unsigned long       i_ino;      // inode号
    loff_t              i_size;
    struct timespec64   i_atime, i_mtime, i_ctime;

    const struct inode_operations   *i_op;   // 虚函数表（mkdir/link/lookup等）
    const struct file_operations    *i_fop;  // 文件操作虚函数表
    struct address_space            *i_mapping; // 页缓存
    struct address_space            i_data;     // inode自己的页缓存

    atomic_t            i_count;    // 引用计数
};
```

**注意：**
- `inode` 是文件的唯一身份，`dentry` 是名字，两者分离
- 硬链接 = 多个 `dentry` 指向同一个 `inode`
- `i_mapping` 通常指向 `i_data`，特殊情况（块设备文件）指向别处

---

### `dentry` — 目录项缓存

```c
struct dentry {
    unsigned int        d_flags;
    struct dentry      *d_parent;   // 父目录dentry
    struct qstr         d_name;     // 文件名（含hash）
    struct inode       *d_inode;    // 对应的inode（负dentry此字段为NULL）
    const struct dentry_operations *d_op;
    struct super_block *d_sb;
    struct list_head    d_child;    // 在父目录下的兄弟链表
    struct list_head    d_subdirs;  // 子目录链表
    struct hlist_bl_node d_hash;    // 全局dentry hash表节点
};
```

**注意：**
- `dentry` 是**缓存**，不一定对应磁盘结构，目的是加速路径解析
- 负 dentry（`d_inode == NULL`）缓存"文件不存在"的查找结果，也是一种优化
- 路径解析 `/a/b/c` 就是沿 dentry 树走三步

---

### `file` — 进程打开文件的实例

```c
struct file {
    struct path         f_path;         // 包含dentry和vfsmount
    struct inode       *f_inode;        // 对应inode（快捷指针）
    const struct file_operations *f_op; // 虚函数表
    loff_t              f_pos;          // 当前读写偏移（每个fd独立）
    unsigned int        f_flags;        // O_RDONLY/O_WRONLY/O_NONBLOCK等
    fmode_t             f_mode;
    struct fown_struct  f_owner;        // 异步通知owner
    atomic_long_t       f_count;        // 引用计数
    void               *private_data;  // 驱动/文件系统自用
};
```

**注意：**
- `dup()` 后两个 fd 共享同一个 `file`，共享 `f_pos`
- `fork()` 后父子共享同一个 `file`，这就是为什么父子写同一 fd 会互相影响
- `file_operations` 是真正的"多态"实现，ext4/socket/设备文件的 read/write 行为完全不同

---

## 五、同步原语

### 选择关系总览

| 场景 | 选用 |
|------|------|
| 中断上下文 / 不能睡眠 | spinlock |
| 可以睡眠 / 用户态等待 | mutex |
| 读多写少 | rwsemaphore / RCU |
| 引用计数 | kref / refcount_t |
| 等待某个事件 | wait_queue_head_t |
| 等待某个任务完成 | completion |

### `spinlock_t`

```c
// 核心约束：
// 1. 持有期间不能睡眠
// 2. 中断上下文必须用 spin_lock_irqsave 版本
// 3. 持有期间禁止抢占
```

### `mutex`

```c
struct mutex {
    atomic_long_t   owner;    // 持有者task_struct指针（含标志位）
    struct list_head wait_list;
};
// 约束：只能在进程上下文用，不能在中断上下文用
// 同一进程不能重复加锁（会死锁）
```

### `rw_semaphore`

```c
// mmap_lock 就是 rw_semaphore
// 读锁可以并发，写锁独占
// mmap_read_lock / mmap_write_lock
```

---

## 六、结构体关系图

```
task_struct
  ├── mm_struct ──────── VMA树 ──── vm_area_struct ──── vm_operations_struct
  │      └── pgd ─────── 页表 ──── page
  ├── files_struct
  │      └── fd数组 ──── file ──── file_operations
  │                        └── dentry ── inode ── address_space ── page
  ├── cred
  ├── signal_struct
  ├── nsproxy ─── pid_ns / net_ns / mnt_ns / ...
  └── sched_entity ── cfs_rq ── rq (per-CPU)
```

---

## 七、per-CPU 机制

### 背景：为什么需要 per-CPU

多核系统下，共享变量的每次读写都要经过缓存一致性协议（MESI）：

```
CPU0 写共享变量
  → 通知所有其他CPU：你的缓存行无效
  → 其他CPU下次读时必须重新从内存/其他CPU缓存拉取
  → 高频操作下总线竞争成为瓶颈
```

per-CPU 变量的思路是**彻底消除共享**：每个 CPU 有自己独立的副本，互不干扰，既不需要锁，也不产生缓存行竞争（false sharing）。

典型场景：
- 网络收包计数器（每个 CPU 独立统计，查询时汇总）
- 内存分配器的本地缓存（slab 的 per-CPU freelist）
- 调度器的运行队列 `rq`（每个 CPU 一个，是最重要的 per-CPU 结构体）
- 中断处理的临时缓冲区

---

### 静态 per-CPU 变量

```c
// 定义（编译期确定，存放在特殊的 .data..percpu section）
DEFINE_PER_CPU(int, my_counter);
DEFINE_PER_CPU(struct my_struct, my_data);

// 声明（跨文件引用）
DECLARE_PER_CPU(int, my_counter);

// 读写（需要禁止抢占）
int val = get_cpu_var(my_counter);   // 读，同时禁止抢占+禁止当前CPU被迁移
put_cpu_var(my_counter);             // 释放，重新开启抢占

// 或者手动控制抢占
preempt_disable();
int val = __this_cpu_read(my_counter);    // 读当前CPU的副本
__this_cpu_write(my_counter, val + 1);   // 写当前CPU的副本
preempt_enable();

// 不关心抢占的原子操作（适合统计计数器）
this_cpu_inc(my_counter);       // 原子递增当前CPU副本
this_cpu_add(my_counter, n);    // 原子加n

// 访问指定CPU的副本（用于汇总统计，不需要禁抢占）
int cpu0_val = per_cpu(my_counter, 0);
```

---

### 动态 per-CPU 变量

```c
// 运行时分配（模块常用，因为无法在编译期放入 percpu section）
int __percpu *dyn_counter = alloc_percpu(int);
if (!dyn_counter)
    return -ENOMEM;

// 读写
preempt_disable();
*this_cpu_ptr(dyn_counter) += 1;
preempt_enable();

// 访问指定CPU
int *ptr = per_cpu_ptr(dyn_counter, cpu);

// 释放
free_percpu(dyn_counter);
```

---

### 内存布局

```
物理内存中 per-CPU 区域的布局：

[CPU0 percpu区域]  [CPU1 percpu区域]  [CPU2 percpu区域] ...
   my_counter=0      my_counter=0       my_counter=0
   my_data=...       my_data=...        my_data=...
   rq=...            rq=...             rq=...

每个CPU的基地址存在 __per_cpu_offset[cpu] 数组里
访问时：变量地址 + __per_cpu_offset[当前CPU编号]
```

静态 per-CPU 变量在链接时放入 `.data..percpu` section，内核启动时为每个 CPU 复制一份并记录偏移量。

---

### 最重要的 per-CPU 结构体：`runqueue (rq)`

调度器的核心数据结构，每个 CPU 一个：

```c
struct rq {
    unsigned int        nr_running;     // 当前可运行任务数
    u64                 nr_switches;    // 上下文切换次数

    struct cfs_rq       cfs;            // CFS调度类的运行队列（红黑树）
    struct rt_rq        rt;             // 实时调度类的运行队列
    struct dl_rq        dl;             // deadline调度类的运行队列

    struct task_struct *curr;           // 当前正在运行的任务
    struct task_struct *idle;           // idle线程（CPU无事可做时运行）
    struct task_struct *stop;           // 最高优先级的停止任务

    u64                 clock;          // 本CPU的时钟
    u64                 clock_task;     // 任务时钟（不含中断时间）

    raw_spinlock_t      lock;           // 保护此rq的自旋锁

    // 负载均衡相关
    struct lb_env      *sd;             // 调度域
    unsigned long       cpu_load;       // CPU负载
};

// 访问当前CPU的rq
struct rq *rq = this_rq();             // 等价于 &per_cpu(runqueues, smp_processor_id())
```

---

### per-CPU 与 slab 分配器的关系

slab 分配器用 per-CPU freelist 作为快速路径：

```
kmalloc(size) 调用路径：

1. 查当前CPU的 kmem_cache_cpu.freelist（per-CPU，无锁）
        ↓ 命中
   直接返回，极快

        ↓ 未命中
2. 查当前CPU的 partial slab（per-CPU，需要短暂关中断）

        ↓ 未命中
3. 从 node 级别的 partial 链表补充（需要获取 node 锁）

        ↓ 未命中
4. 向 buddy allocator 申请新页
```

这就是 slab 分配在多核下依然高效的原因——大多数分配命中第一步，完全无锁。

```c
struct kmem_cache_cpu {
    void      **freelist;   // 当前CPU的空闲对象链表（指针数组）
    unsigned long tid;      // 事务id，用于检测CPU迁移
    struct page *page;      // 当前使用的slab页
    struct page *partial;   // 本CPU的partial slab链表
};
```

---

### 注意事项

| 问题 | 说明 |
|------|------|
| **必须禁止抢占** | 读写 per-CPU 变量期间如果被抢占并迁移到另一个 CPU，操作的就不是"当前CPU"的副本了。`get_cpu_var` 自动处理，裸用 `__this_cpu_*` 需要手动 `preempt_disable()` |
| **不能在中断上下文直接用 `get_cpu_var`** | `get_cpu_var` 会调用 `preempt_disable`，中断上下文本就不可抢占，用 `__this_cpu_*` 系列即可 |
| **汇总时数据不一致是正常的** | 读各 CPU 副本并求和时，没有全局快照，结果是近似值，对统计计数器可以接受 |
| **`this_cpu_*` vs `__this_cpu_*`** | 带双下划线的版本不检查抢占状态，性能略高但使用者自己保证安全 |
| **避免 false sharing** | 如果 per-CPU 变量和其他频繁修改的变量在同一缓存行（64字节），会导致 false sharing，用 `____cacheline_aligned` 对齐 |
| **动态分配必须检查返回值** | `alloc_percpu` 可能返回 NULL，尤其内存紧张时 |

---

## 八、全局约定

| 约定 | 说明 |
|------|------|
| `container_of` | 从嵌入的子结构指针反推宿主指针，阅读链表/红黑树操作时随处可见 |
| RCU 保护 | 读不加锁，写复制再替换，`cred`/网络路由表等都用这个 |
| `_count` vs `_refcount` | 前者可能是 atomic_t，后者是带溢出保护的 refcount_t，不能混用 |
| ops 指针即多态 | 所有 `xxx_operations` 结构体就是虚函数表，是 Linux 面向对象的核心手法 |
| per-CPU 变量 | `DEFINE_PER_CPU`，每个 CPU 独立副本，无锁访问，读写期间必须禁止抢占 |
