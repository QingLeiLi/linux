# Linux VFS 内部实现深度解析

> 适用内核版本：v7.2-rc1（基于当前仓库 `a14c021eef14`）
> 核心源码：`fs/namei.c`、`fs/open.c`、`fs/read_write.c`、`fs/file.c`、`fs/file_table.c`、`fs/dcache.c`、`fs/inode.c`、`fs/super.c`、`fs/namespace.c`、`mm/filemap.c`

---

## 目录

1. [VFS 要解决的根本问题](#1-vfs-要解决的根本问题)
2. [总体架构与设计原则](#2-总体架构与设计原则)
3. [核心对象及其关系](#3-核心对象及其关系)
4. [文件系统注册与挂载](#4-文件系统注册与挂载)
5. [路径名解析总览](#5-路径名解析总览)
6. [RCU-walk 与 REF-walk](#6-rcu-walk-与-ref-walk)
7. [路径分量、符号链接和挂载点](#7-路径分量符号链接和挂载点)
8. [openat2 完整调用链](#8-openat2-完整调用链)
9. [文件描述符表与 fd 发布](#9-文件描述符表与-fd-发布)
10. [read/write 与迭代 I/O](#10-readwrite-与迭代-io)
11. [页缓存与 buffered I/O](#11-页缓存与-buffered-io)
12. [mmap、direct I/O 与一致性](#12-mmapdirect-io-与一致性)
13. [目录项修改：create、link、unlink、rename](#13-目录项修改createlinkunlinkrename)
14. [inode、dentry 与 file 的生命周期](#14-inodedentry-与-file-的生命周期)
15. [mount tree 与 mount namespace](#15-mount-tree-与-mount-namespace)
16. [权限、安全与 idmapped mount](#16-权限安全与-idmapped-mount)
17. [并发控制与锁顺序](#17-并发控制与锁顺序)
18. [缓存回收、writeback 与卸载](#18-缓存回收writeback-与卸载)
19. [伪文件系统与叠加文件系统](#19-伪文件系统与叠加文件系统)
20. [观测、调试与常见误区](#20-观测调试与常见误区)
21. [关键函数与完整调用链速查](#21-关键函数与完整调用链速查)

---

## 1. VFS 要解决的根本问题

Linux 同时支持 ext4、XFS、Btrfs、NFS、tmpfs、procfs、sysfs、overlayfs 等文件系统。
它们的磁盘格式、目录索引、一致性模型甚至是否有磁盘都不同，但用户空间希望统一使用：

```c
open(path, flags);
read(fd, buf, len);
write(fd, buf, len);
mmap(..., fd, ...);
rename(old, new);
```

VFS（Virtual Filesystem）不是一个具体文件系统，而是一套对象模型、缓存和调用协议：

- 把路径名解析为由 mount 和 dentry 组成的位置；
- 把“文件身份”“目录中的名字”“一次打开”拆成不同对象；
- 用 operation tables 把通用系统调用分派给具体文件系统；
- 统一 page cache、权限、文件描述符和生命周期；
- 在并发 lookup、rename、mount、unlink、回收时维持路径语义。

最关键的认知是：**路径、目录项、inode、打开文件和 fd 不是同一个东西。**

```
路径字符串 "/a/b"
    │ 路径遍历
    ▼
struct path = { mount, dentry }
    │ dentry 指向
    ▼
inode（文件身份）
    │ open 创建
    ▼
file（一次打开实例）
    │ 安装进进程 fdtable
    ▼
整数 fd
```

这层拆分让 hard link、bind mount、打开后 unlink、共享文件偏移和 mount namespace 都能自然表达。

---

## 2. 总体架构与设计原则

### 2.1 分层调用模型

```
用户态 libc / 系统调用 ABI
        │
        ▼
open/read/write/stat/rename/mount...
        │
        ▼
┌────────────────────────────────────────────────────────────┐
│ VFS 通用层                                                 │
│ fdtable、namei、dcache、inode cache、permission、locks     │
├────────────────────────────────────────────────────────────┤
│ operation tables                                          │
│ super_operations / inode_operations / file_operations     │
│ address_space_operations / dentry_operations              │
├──────────────────────┬─────────────────────────────────────┤
│ 本地文件系统          │ 网络/内存/伪/叠加文件系统           │
│ ext4, XFS, Btrfs     │ NFS, tmpfs, procfs, overlayfs      │
├──────────────────────┴─────────────────────────────────────┤
│ page cache / writeback / iomap / buffer_head              │
├────────────────────────────────────────────────────────────┤
│ block layer / network transport / memory objects           │
└────────────────────────────────────────────────────────────┘
```

### 2.2 关键设计原则

**对象与名字分离。** inode 表示对象，dentry 表示某个父目录下的名字。一个 inode 可有多个
hard link，也就可以对应多个 positive dentry。

**打开实例与对象分离。** 每次 `open()` 通常产生一个 `struct file`，其中 `f_pos`、open flags
和私有状态属于这次打开，而 inode 元数据由所有打开实例共享。

**查找先缓存后后端。** dcache 命中时无需调用具体文件系统；negative dentry 还能缓存
“这个名字不存在”，避免反复访问磁盘或网络。

**读路径乐观无锁。** 路径遍历首先尝试 RCU-walk，用 seqcount 验证没有并发变化；只有
缓存未命中、需要睡眠或观察到变化时才退化为取得引用的 REF-walk。

**先保留 fd 槽位，最后发布 file。** open 可能失败，内核先占用 fd bitmap，再完成路径查找
和文件打开，最后用 RCU publish 指针，避免其他线程看见半初始化 `struct file`。

---

## 3. 核心对象及其关系

### 3.1 `struct super_block`：一次文件系统实例

定义于 `include/linux/fs/super_types.h`。一次 mount 通常关联一个 superblock，但 bind mount
或同一 superblock 的多个挂载视图可共享它。

```
super_block
├── s_type             file_system_type，例如 ext4_fs_type
├── s_op               super_operations
├── s_root             文件系统根 dentry
├── s_bdev             后端块设备（若存在）
├── s_fs_info          具体文件系统私有信息
├── s_inodes           该 superblock 的 inode 集合
├── s_mounts           引用它的 mount
├── s_flags / s_iflags 只读、同步等标志
├── s_blocksize        文件系统块大小
├── s_active/s_count   生命周期引用
└── s_umount           冻结/卸载相关 rwsem
```

superblock 是“已装载文件系统”的内存表示，不等于磁盘 superblock。tmpfs/procfs 也有
`struct super_block`，即使不存在块设备上的超级块。

### 3.2 `struct inode`：文件身份和元数据缓存

定义于 `include/linux/fs.h`：

```
inode
├── i_ino / i_generation      文件系统内身份
├── i_mode / i_uid / i_gid    类型、权限和所有者
├── i_nlink                   hard-link 数
├── i_size                    文件逻辑大小
├── i_atime/i_mtime/i_ctime   时间戳
├── i_sb                      所属 superblock
├── i_op                      inode_operations
├── i_fop                     默认 file_operations
├── i_mapping                 address_space（页缓存入口）
├── i_rwsem                   目录/文件结构性操作串行化
├── i_lock                    状态、计数等短临界区
├── i_count                   VFS 引用
└── i_state                   I_NEW/I_DIRTY/I_FREEING 等状态
```

inode 缓存的是文件系统对象元数据，不保存“这个文件在目录中叫什么”。hard link 共享 inode，
但拥有不同 dentry。文件系统 inode number 通常只在 superblock 范围内唯一，跨文件系统比较
必须连同设备/文件系统身份。

### 3.3 `struct dentry`：名字到 inode 的缓存边

定义于 `include/linux/dcache.h`：

```
dentry
├── d_name / d_parent       名字与父 dentry
├── d_inode                 positive 时指向 inode；negative 时 NULL
├── d_hash                  dcache 哈希链
├── d_children / d_sib      dentry 树关系
├── d_alias                 同一 inode 的别名链
├── d_seq                   RCU-walk 验证并发变化
├── d_lockref               自旋锁 + 引用计数
├── d_op                    文件系统定制 revalidate/hash/compare...
├── d_sb                    所属 superblock
└── d_lru                   未使用 dentry 的回收链
```

positive dentry 的 `d_inode != NULL`；negative dentry 表示某父目录下查过该名字且不存在。
网络文件系统或需要外部一致性的文件系统可通过 `d_revalidate` 判定缓存结论是否仍有效。

dentry 是 VFS 缓存对象，不保证与磁盘目录项一一对应。它可能因缓存压力被回收，也可能表示
已经 unlink 但仍被引用的 disconnected name。

### 3.4 `struct path`：真正的 VFS 位置

```c
struct path {
    struct vfsmount *mnt;
    struct dentry *dentry;
};
```

仅有 dentry 不足以确定一个路径位置：同一 dentry tree 可经 bind mount 出现在多个位置，
同一个挂载点在不同 mount namespace 中视图也不同。权限检查、`..`、路径打印和 mount crossing
通常都需要完整 `struct path`。

### 3.5 `struct file`：一次 open file description

定义于 `include/linux/fs.h`：

```
file
├── f_path             打开的 path，持有 mount+dentry 引用
├── f_inode            缓存的 inode 指针
├── f_op               本次打开使用的 file_operations
├── f_flags / f_mode   O_* 标志与 FMODE_* 内核能力
├── f_pos              当前文件偏移
├── f_pos_lock         共享 f_pos 的串行化
├── f_cred              open 时凭证快照
├── private_data        驱动/文件系统的打开实例状态
├── f_mapping           通常指向 inode->i_mapping
└── f_ref               `file_ref_t` 引用状态
```

`dup()` 和 fork 后的 fd 可以指向同一 `struct file`，因此共享 `f_pos`。分别调用两次 `open()`
通常得到两个 file，即使 inode 相同，偏移也独立。

### 3.6 `struct address_space`：文件内容缓存

它并不是 CPU 虚拟地址空间，而是“可分页对象的缓存映射”：

```
address_space
├── host               通常为所属 inode
├── i_pages            XArray：文件页索引 → folio/特殊 entry
├── a_ops              read_folio、writepages、dirty_folio、direct_IO...
├── i_mmap             映射该文件的 VMA interval tree
├── nrpages            缓存 folio 页数
├── writeback_index    回写扫描位置
└── invalidate_lock    truncate、invalidate、DIO 等协调
```

buffered read/write 和 file-backed mmap 共享这一个 page cache，这正是两种访问方式能够一致
观察文件内容的基础。

### 3.7 对象关系总图

```
task_struct
  └── files_struct
      └── fdtable.fd[3] ─────────────┐
                                     ▼
                                  struct file
                     ┌───────────────┼───────────────┐
                     ▼               ▼               ▼
                  f_path          f_inode          f_mapping
                ┌────┴────┐          │               │
                ▼         ▼          │               ▼
             mount      dentry ──────┘          address_space
               │          │                         │
               ▼          └── parent/name           └── XArray → folios
          super_block
               │
               └── concrete filesystem private state
```

---

## 4. 文件系统注册与挂载

### 4.1 `file_system_type`

具体文件系统通过 `register_filesystem()` 注册 `struct file_system_type`：

```c
struct file_system_type {
    const char *name;
    int fs_flags;
    int (*init_fs_context)(struct fs_context *);
    const struct fs_parameter_spec *parameters;
    struct dentry *(*mount)(...);       /* legacy compatibility path */
    void (*kill_sb)(struct super_block *);
    ...
};
```

现代 mount API 以 `fs_context` 分阶段完成参数解析、创建/复用 superblock、取得文件系统根和
安装 mount。用户态新的 `fsopen/fsconfig/fsmount/move_mount` 可以显式操作这些阶段；传统
`mount(2)` 在内核中转换为同一套上下文。

### 4.2 从类型到 superblock

典型流程为：

```
文件系统类型查找
  → fs_context 初始化与参数解析
  → get_tree 回调
     ├── get_tree_bdev：按块设备查找/创建 superblock
     ├── get_tree_nodev：无块设备文件系统
     └── sget_fc：匹配已有或分配新 superblock
  → fill_super：读取/构造文件系统元数据和根 inode/dentry
  → 得到 fc->root
  → 创建 struct mount 并接入 mount tree
```

`sget_fc()` 必须处理并发 mount 同一后端的竞态。新 superblock 以 `SB_BORN` 等状态发布前，
其他查找者不能把它当成完整实例；失败路径必须撤销设备、模块和 security 引用。

### 4.3 operation tables 的职责边界

| 操作表 | 面向对象 | 典型回调 |
|--------|----------|----------|
| `super_operations` | superblock | `alloc_inode`、`write_inode`、`evict_inode`、`statfs` |
| `inode_operations` | 命名与元数据 | `lookup`、`create`、`unlink`、`rename`、`permission`、`getattr` |
| `file_operations` | 已打开实例 | `open`、`release`、`read_iter`、`write_iter`、`mmap`、`ioctl` |
| `address_space_operations` | 文件内容缓存 | `read_folio`、`readahead`、`writepages`、`dirty_folio` |
| `dentry_operations` | dcache 策略 | `d_revalidate`、`d_hash`、`d_compare`、`d_delete` |

VFS helper 不是无条件转发：通常先做权限、锁、freeze、security、fsnotify、审计和通用状态更新，
再调用具体回调。直接调用某个 `i_op` 往往会绕过必要协议。

---

## 5. 路径名解析总览

路径解析的输入不只是字符串，还包括：

- 起点：当前目录、根目录、`dirfd` 或预设 root；
- lookup flags：是否跟随最终 symlink、是否找目录、是否创建；
- `openat2()` resolve policy：禁止越界、mount crossing、symlink 等；
- 当前 task 的 mount namespace、凭证和 fs root；
- 并发 rename、mount、unlink 与 dcache 回收状态。

### 5.1 `nameidata`

`fs/namei.c` 的 `struct nameidata` 是一次路径遍历状态机：

```
nameidata
├── path                  当前 mount+dentry
├── inode                 当前 dentry 的 inode 快照
├── root                  本次解析不可越过的根
├── last / last_type      最后一个分量及 DOT/DOTDOT/NORM/ROOT
├── flags / state         LOOKUP_* 与内部状态
├── seq / next_seq        当前/下一 dentry 的 d_seq 快照
├── m_seq / r_seq         mount tree、rename 序列快照
├── depth                 symlink 栈深度
├── stack[]               延迟处理的符号链接状态
└── dfd/name/pathname     原始入口信息
```

它不是长期对象，而是一次 lookup 的栈上/临时控制结构。嵌套路径查找通过 `current->nameidata`
保存与恢复上下文，并累计 symlink 深度限制。

### 5.2 普通查找调用链

```
filename_lookup()/filename_parentat()
  → set_nameidata()
  → path_lookupat()
    → path_init()             选择 /、cwd、dirfd 或 preset root
    → link_path_walk()        遍历除最后分量之外的部分
      → walk_component()
        → lookup_fast()
        → lookup_slow()       dcache miss/需 revalidate
        → step_into()         更新 path、跨 mount、处理 symlink
    → walk_component(...TRAILING)
    → complete_walk()         最终合法性和弱 revalidate
  → terminate_walk()
  → restore_nameidata()
```

open 与 create 对最后一个分量有更复杂的锁和创建语义，使用 `open_last_lookups()` +
`do_open()`，不能简单复用普通 lookup 的最后一步。

### 5.3 起点选择

- 绝对路径从 task root 开始，而非必然从全局文件系统根开始；
- 相对路径且 `dfd == AT_FDCWD` 时从 cwd 开始；
- 其他相对路径从 dirfd 指向的目录开始；
- `LOOKUP_IN_ROOT` 可把 dirfd 临时视为 `/`；
- 进程的 root/cwd 是 `fs_struct` 中的 `struct path`，读取时需要相应 seq/lock 协议。

---

## 6. RCU-walk 与 REF-walk

### 6.1 为什么路径遍历不能每层都加引用

解析 `/usr/lib/libc.so` 需要逐个查 dentry。如果每个分量都 `dget()`、`mntget()` 并修改共享
引用计数，多核上 cacheline 会频繁争用。RCU-walk 的目标是：在 dcache 命中且没有并发结构
变化时，仅用 RCU、seqcount 和普通读取完成整个路径遍历。

### 6.2 RCU-walk 的验证模型

RCU 保证对象内存不会立即释放，但不能保证名字、parent、inode 或 mount 关系没变化。因此读取
前后要验证 sequence：

```
rcu_read_lock()
读取 mount_lock 序列 m_seq
读取 parent dentry 的 d_seq
  → 哈希查找 child
  → 读取 child name/parent/inode
  → read_seqcount_retry(child->d_seq, seq) ?
  → mount_lock/rename_lock 是否变化？
全部稳定：继续下一分量
否则：退出乐观路径并重试
```

`d_seq` 保护 dentry 的 RCU 可见字段一致性；mount sequence 检测挂载树变化；rename sequence
帮助检测 `..` 遍历和祖先关系在 rename 下是否失效。

### 6.3 `-ECHILD` 的内部含义

路径 lookup 内部的 `-ECHILD` 通常不是“没有子进程”，而是“RCU-walk 无法安全完成，请用
REF-walk 重试”。例如：

- dentry revalidate 回调需要睡眠；
- symlink 获取目标需要阻塞；
- seqcount 验证失败；
- 某操作无法在只持 RCU 的状态完成。

典型调用者：

```c
file = path_openat(..., flags | LOOKUP_RCU);
if (unlikely(file == ERR_PTR(-ECHILD)))
    file = path_openat(..., flags);          /* REF-walk */
if (unlikely(file == ERR_PTR(-ESTALE)))
    file = path_openat(..., flags | LOOKUP_REVAL);
```

因此用户空间通常看不到这个 `ECHILD`。

### 6.4 从 RCU-walk 升级为 REF-walk

当前源码的 `try_to_unlazy()`/`try_to_unlazy_next()` 尝试把 RCU 下观察到的 path 转换成真实引用：

1. 对 mount/dentry 取得引用；
2. 再验证 `d_seq`、mount sequence 等快照；
3. 验证成功才退出 RCU 模式；
4. 失败则丢弃引用并让整个 lookup 重走。

顺序不能颠倒。只先 `dget()` 而不验证，可能拿到了已被 rename 到别处的同一个对象，结果不是
原来观察的路径。

### 6.5 REF-walk

REF-walk 对当前 path 持有 `d_lockref`/mount 引用，允许调用可能睡眠的文件系统回调。
`lookup_fast()` 仍先查 dcache；未命中时 `lookup_slow()` 在父目录 `i_rwsem` 保护下分配
in-lookup dentry，并调用 `dir->i_op->lookup()`。

并发线程查询同一未缓存名字时，dcache 的 parallel lookup 协议让一个线程执行后端 lookup，
其他线程等待该 in-lookup dentry 完成，避免重复 I/O 和产生两个同名 dentry。

---

## 7. 路径分量、符号链接和挂载点

### 7.1 每个普通分量

`walk_component()` 的核心逻辑：

```
解析下一个 qstr（name/hash/len）
  → lookup_fast(nd)
      ├── dcache 命中且有效：返回 dentry
      ├── negative 命中：按调用语义返回不存在
      └── miss/需慢速验证：NULL
  → lookup_slow(name, parent, flags)
      → 锁父 inode
      → d_alloc_parallel()
      → inode->i_op->lookup()
      → d_add/d_splice_alias 完成实例化
  → step_into()
      → handle_mounts()
      → 若 symlink，pick_link()
      → 更新 nd->path/inode/seq
```

文件系统 lookup 的职责通常是根据父 inode 和名字查找后端 inode，再用 `d_add()` 把结果与
dentry 关联；找不到时也应完成 negative dentry。

### 7.2 `.` 与 `..`

`.` 保持当前位置。`..` 远比读取 `d_parent` 复杂：

- 到达当前 mount 根时要跳到父 mount 的挂载点；
- 不能越过 task root、chroot 或 `LOOKUP_IN_ROOT` 指定根；
- `LOOKUP_BENEATH` 下任何可能逃出起点的行为都要拒绝；
- bind mount 的根可能不是 superblock 根；
- 并发 rename 可能改变祖先关系，必须借助 sequence 验证。

这也是仅靠字符串规范化删除 `a/../` 无法安全替代内核路径解析的原因。

### 7.3 符号链接

symlink 的 inode `get_link()` 返回目标字符串，可能同时返回 delayed callback 用于释放临时
资源。目标为绝对路径时重新从 root 开始，相对路径则从链接所在目录继续。

内核用 `nameidata.stack` 保存嵌套链接的剩余路径和清理回调，而不是递归消耗无限内核栈。
`MAXSYMLINKS` 限制一次解析中的链接总数，防止环与拒绝服务。

最终分量是否跟随 symlink 取决于 syscall 和 flags：普通 `open()` 通常跟随；`lstat()`、
`O_NOFOLLOW` 或 `openat2(RESOLVE_NO_SYMLINKS)` 改变行为。中间分量若要继续遍历则必须是目录，
且通常会跟随 symlink，除非 resolve policy 禁止。

### 7.4 mount crossing

当 dentry 是挂载点，`handle_mounts()` 在当前 namespace 的 mount hash/tree 中寻找覆盖它的
child mount，并把 `path` 切换到 child mount root。向上走 `..` 时执行反向 crossing。

mount 不是修改 dentry tree，而是在 `(parent mount, mountpoint dentry)` 上覆盖另一个
`mount->mnt_root`。因此被覆盖目录原内容仍存在，只是在该 namespace 路径视图中不可见。

### 7.5 openat2 的解析边界

`openat2()` 把过去容易产生 TOCTOU 的用户态“先检查再 open”约束放进同一次内核遍历：

- `RESOLVE_BENEATH`：不允许逃到 dirfd 之上，绝对路径/绝对 symlink 也受限；
- `RESOLVE_IN_ROOT`：把 dirfd 当作本次解析的临时根；
- `RESOLVE_NO_SYMLINKS`：拒绝任何 symlink；
- `RESOLVE_NO_MAGICLINKS`：拒绝 procfs magic link；
- `RESOLVE_NO_XDEV`：禁止跨 mount，包括 bind mount；
- `RESOLVE_CACHED`：只允许可在缓存/RCU 路径完成的查找，否则返回 `EAGAIN`。

这些保证与路径遍历同步，强于用户态对字符串做前缀检查或先 `realpath()` 再打开。

---

## 8. openat2 完整调用链

### 8.1 从系统调用到 `path_openat()`

```
sys_openat2
  → copy_struct_from_user(struct open_how)
  → build_open_how()/build_open_flags()
  → do_sys_openat2(dfd, filename, how)
    → build_open_flags()
    → CLASS(filename_flags, name)        复制路径并绑定作用域清理
    → FD_ADD(flags, do_file_open(...))   封装“保留 fd + 打开 + 发布/回滚”
      → get_unused_fd_flags()            先保留 fd 槽位
      → do_file_open(dfd, name, &op)
      → set_nameidata()
      → path_openat(... LOOKUP_RCU)
        → alloc_empty_file()
        → path_init()
        → link_path_walk()
        → open_last_lookups()
        → do_open()
      → 必要时 REF/REVAL 重试
      → fd_install(fd, file)             成功时最后发布
    → 返回 fd
```

当前源码用 `CLASS(filename_flags, ...)` 和 `FD_ADD()`/`fd_prepare` cleanup class 表达所有权。
失败时作用域析构自动释放 `filename`、归还已保留 fd bit，并 `fput()` 已分配 file。这里仍是
两个独立资源：fd number reservation 和 `struct file`；cleanup class 只是把逆序回滚结构化，
并没有把二者合成同一个对象。

### 8.2 为什么最后分量特殊

对于 `/a/b/c`，前面的 `/a/b` 只需查找并确认是目录，`c` 却可能：

- 已存在并被打开；
- 不存在且因 `O_CREAT` 被创建；
- 已存在但 `O_EXCL|O_CREAT` 必须失败；
- 是 symlink，需要决定是否跟随；
- 是目录但调用者要求普通写打开；
- 在查找和创建间被另一线程创建/删除/rename。

`open_last_lookups()` 先尝试 fast lookup；需要创建时锁父目录 inode，重新确认 negative dentry，
调用 `vfs_create()`/atomic_open，并保持创建与 lookup 原子性。`do_open()` 完成最终 path 验证、
权限检查、truncate 和真正 file open。

### 8.3 `atomic_open`

若文件系统提供 `i_op->atomic_open`，可把 lookup、可能的 create 和 open 合并。网络文件系统
尤其需要它避免多次 RPC 之间的竞态和往返。VFS 仍负责 flags 规范、父目录锁、security hook
与返回状态协议，文件系统通过 `FILE_OPENED`、`FILE_CREATED` 等结果说明完成了哪些阶段。

### 8.4 `do_dentry_open()`

当 path 已确定，`vfs_open()` 最终进入 `do_dentry_open()`：

1. 把 path/inode/mapping 安装到尚未发布的 file；
2. 设置读写能力和 append 等 `f_mode`；
3. 检查只读文件系统、write access 和特殊文件限制；
4. 调用 `security_file_open()` 等 LSM hook；
5. 选择/固定 `f_op`，调用 `f_op->open()`；
6. 初始化 readahead、版本等状态，完成 fsnotify/open accounting。

设备文件的 inode 来自设备节点，但 `f_op->open()` 可能把 file operations 替换成具体字符/块
设备驱动的操作表。之后 read/ioctl 不再走普通文件实现。

### 8.5 `O_TRUNC` 的位置

打开已有普通文件并请求 `O_TRUNC` 时，内核必须在确认写权限、mount 可写且目标类型允许之后，
通过 truncate 协议修改 `i_size`、清 page cache 和文件系统块映射。它不能在路径查找尚可能失败
时过早发生，也必须与 mmap、writeback 和 direct I/O 协调。

---

## 9. 文件描述符表与 fd 发布

### 9.1 `files_struct` 与 `fdtable`

每个 task 通过 `task->files` 引用 `files_struct`。线程使用 `CLONE_FILES` 时共享整张表；fork
默认由 `dup_fd()` 复制表，但其中每个非空槽位对同一 file 增加引用。

```
files_struct
├── count                 共享该表的任务数
├── file_lock             修改 fd、bitmaps、扩容状态
├── next_fd               下次搜索提示
├── fdt ───────────────┐  RCU 指向当前 fdtable
└── 内嵌小表           │
                       ▼
fdtable
├── max_fds
├── fd[]                  RCU file 指针数组
├── open_fds              已占用槽位 bitmap
├── close_on_exec         FD_CLOEXEC bitmap
└── full_fds_bits         加速跳过已满 bitmap word
```

小 fd 表内嵌在 `files_struct`，避免常见进程额外分配；扩容时分配新 fdtable/数组，复制内容后
RCU 替换 `files->fdt`，旧表经过宽限期释放。

### 9.2 fd 分配与安装是两阶段

`get_unused_fd_flags()` 在 `file_lock` 下寻找空 bit，必要时扩容，设置 `open_fds` 和
`close_on_exec`，但 `fd[fd]` 仍为 NULL。完成 open 后：

```c
rcu_assign_pointer(fdt->fd[fd], file);   /* fd_install() */
```

这个 store-release 发布点保证其他线程看到非 NULL file 时，file 的 path、f_op、mode 等初始化
已经可见。fd 被预留期间，其他分配者不会重复使用它，但 lookup 也不会获得半成品 file。

### 9.3 无锁 fd lookup

读写系统调用通过 `fdget()`/`files_lookup_fd_rcu()` 一类 helper 在 RCU 下读取 fdt 和 file，
并用 `get_file_rcu()` 尝试稳定增加 `file->f_ref`。竞态模型包括：

```
reader                         close/reuse
读取 files->fdt                清 fd[old]
读取 file 指针                 fput(old)
尝试增加 f_ref                 fd number 可安装 new file
再次验证槽位仍指向同一 file
```

只增加旧 file 引用还不够：fd 可能已关闭并复用，reader 必须验证槽位没有变化。helper 把这套
协议封装起来，调用者不能裸读 `files->fdt->fd[n]`。

### 9.4 close 与 file 引用

`close_fd()` 在 `file_lock` 下先把 fd 槽位置 NULL、清 bitmap，使新查找不再取得 file，然后
在锁外执行 `filp_close()`/`fput()`。最后引用消失时才运行 `f_op->release` 和 path/inode 清理。

从 fdtable 移除不等于 file 立即销毁：dup fd、fork 子进程、epoll、异步 I/O 或内核引用都可能
仍持有它。反过来，数字 fd 一旦被清空就可很快复用，所以并发程序不能假定 close 后该数字
仍代表原对象。

### 9.5 `fput()` 为什么可能延迟

最终 `fput()` 会执行可能睡眠或递归进入文件系统的清理，不能在所有上下文直接运行。
`fput()` 根据上下文把 `____fput()` 安排到 task_work 或 delayed work。`____fput()` 依次处理
eventpoll、锁、`release`、fsnotify、write access、path 和 cred 等引用。

task 退出时会运行 task_work 并释放 fdtable；内核还需处理“最后引用恰好在中断/退出边界消失”
的情况，防止在非法上下文调用文件系统 release。

### 9.6 dup、fork 与 exec

- `dup/dup2/dup3`：新 fd 指向同一 file，共享 `f_pos` 和 file status flags；
- fork：默认复制 fdtable，每个槽仍指向同一 file；
- pthread：通常共享 `files_struct`，一个线程 close 会立刻影响其他线程的 fd namespace；
- exec：保留 fdtable，但 `do_close_on_exec()` 关闭 `close_on_exec` bitmap 中的 fd；
- `FD_CLOEXEC` 是 fd flag，`O_APPEND/O_NONBLOCK` 等通常是 file status flags。


---

## 10. read/write 与迭代 I/O

### 10.1 同步 read 调用链

```
sys_read(fd, user_buf, count)
  → fdget_pos(fd)
      → RCU 查 fdtable，稳定 file 引用
      → 若共享 f_pos 需要，取得 file->f_pos_lock
  → vfs_read(file, buf, count, &file->f_pos)
      → access_ok() / rw_verify_area()
      → security_file_permission()
      → file->f_op->read 或 new_sync_read()
          → init_sync_kiocb()
          → iov_iter_ubuf()
          → file->f_op->read_iter(&kiocb, &iter)
      → fsnotify_access / 记账
  → fdput_pos()
```

现代文件系统主要实现 `read_iter/write_iter`，旧的 `read/write` 回调用于兼容。iter 接口把
用户 buffer、内核 buffer、iovec、pipe、bvec、xarray 等来源统一成 `iov_iter`，从而让
readv/writev、splice、io_uring 和同步 I/O 复用底层逻辑。

### 10.2 `kiocb`

`struct kiocb` 描述一次 I/O，而 `struct file` 描述一次打开：

```
kiocb
├── ki_filp          目标 file
├── ki_pos           本次 I/O 起始偏移
├── ki_flags         NOWAIT、DIRECT、DSYNC、HIPRI 等
├── ki_complete      异步完成回调
└── private          实现私有状态
```

同步包装创建栈上 kiocb，并要求 read_iter/write_iter 在返回前完成或按同步协议等待；异步路径可
返回 `-EIOCBQUEUED`，完成回调稍后报告结果。不能把“调用返回”与“设备 I/O 已完成”混为一谈。

### 10.3 文件偏移的并发语义

不显式传偏移的 `read/write` 使用 `file->f_pos`；`pread/pwrite` 使用调用者给定位置，不修改
共享偏移。多个 fd 若通过 dup/fork 指向同一 file，必须原子更新共享 f_pos，因此
`fdget_pos()` 可能取得 `f_pos_lock`。

两个独立 open 得到不同 file，其 `f_pos` 不共享。`O_APPEND` 写不是简单在用户态读取
`i_size` 再写：内核/文件系统必须让“选择文件尾 + 写入”满足并发追加的原子语义。

### 10.4 VFS 通用检查

`rw_verify_area()`、file mode 和 LSM 路径共同检查：

- file 是否以相应模式打开；
- offset/count 是否溢出，是否超过最大文件大小与 `RLIMIT_FSIZE`；
- mandatory/lease 等冲突；
- security policy 是否允许；
- superblock freeze 与 write accounting 是否允许写入。

具体文件系统回调不能假定所有状态都永远不变，仍需在自己的锁和 transaction 中验证块映射、
文件大小、DAX、压缩或网络状态。

### 10.5 splice 与 zero-copy 的边界

`splice()` 把数据在 file 和 pipe buffers 之间移动，`sendfile()` 通常建立在 splice 类路径上。
“zero-copy”表示尽量避免用户态往返复制，不保证整个路径没有任何复制；文件系统、网络栈、
页对齐、加密、校验和或设备能力都可能触发复制或 fallback。

---

## 11. 页缓存与 buffered I/O

### 11.1 buffered read

普通文件未自定义特殊读路径时常进入 `generic_file_read_iter()`/filemap read：

```
read_iter
  → filemap_read()
    → 根据 ki_pos 计算 page index + offset
    → filemap_get_folio(mapping, index)
       ├── XArray 命中 uptodate folio
       └── miss：触发 synchronous readahead
             → 分配 folio，加入 mapping->i_pages
             → a_ops->readahead/read_folio
             → 等待 folio uptodate
    → copy_folio_to_iter()
    → 更新 ki_pos、accessed 标志和 readahead 状态
```

XArray 中先插入 locked folio，再发起 I/O，能让并发 reader 找到同一个 in-flight folio 并等待，
避免为同一文件区间提交重复读取。`uptodate` 表示内容有效，folio lock 协调填充、truncate 和
错误处理。

### 11.2 readahead

`file_ra_state` 保存在 file 中，因为顺序访问模式属于打开实例。内核根据连续命中/缺页扩大
窗口，预取后续 folio；随机访问则缩小或禁用窗口。异步 readahead 常在命中窗口中的 marker
folio 时扩展下一窗口。

预读页进入 page cache，但应用尚未消费；内存压力下可以回收。过度预读会挤压工作集，所以
算法需要在 I/O 吞吐和缓存污染之间平衡。

### 11.3 buffered write

通用写路径通常分为 prepare/copy/commit 循环：

```
generic_file_write_iter()
  → file_start_write()
  → generic_write_checks()
  → iomap_file_buffered_write() 或 generic_perform_write()
      → 找/分配 page-cache folio
      → a_ops->write_begin() / iomap_begin()
          建立或预留文件块映射
      → copy_page_from_iter_atomic()
      → a_ops->write_end() / iomap_end()
          提交长度、更新 i_size、标脏
      → balance_dirty_pages_ratelimited()
  → 按 O_SYNC/O_DSYNC 执行同步
  → file_end_write()
```

`write()` 返回通常只表示数据进入 page cache 并被标脏，不表示已到稳定存储。具体文件系统可用
iomap 替代传统 buffer_head；VFS 层不假设其磁盘块布局。

### 11.4 为什么需要 write_begin/write_end

写入可能只覆盖一个 filesystem block 的部分内容。文件系统需要先确保未覆盖部分有正确旧值、
为 hole 分配/延迟分配块、启动 journal transaction；复制失败或短写后又要只提交实际字节。

`write_begin` 到 `write_end` 期间 folio 通常被锁定。若用户源地址恰好 mmap 同一文件，复制
可能再次 fault 进入文件系统，因此实现必须遵守锁顺序并避免持有会导致递归死锁的锁。

### 11.5 dirty 与 writeback 状态机

```
clean + uptodate
  → 用户写 / mmap page_mkwrite
dirty
  → writeback 扫描并锁定
dirty cleared + writeback set
  → 提交 I/O
  ├── 成功：writeback clear，保持 clean
  └── 失败：记录 mapping error，可能重新 dirty
```

写回错误不仅通过当次 write 报告，还保存在 mapping 的 errseq 状态。`fsync()` 和后续相关
调用使用 file 中的采样位置检查“自上次检查以后”的错误，避免一个错误永久污染所有调用，也
避免静默丢失异步 writeback 错误。

### 11.6 truncate 与 page cache

缩小文件必须让 `i_size`、page cache、已有 PTE、正在进行的 buffered/direct I/O 和文件系统
块映射一致。典型协议会：

1. 在 inode/文件系统锁下更新或准备更新大小；
2. 通过 `unmap_mapping_range()` 撤销 mmap PTE；
3. `truncate_inode_pages*()` 从 XArray 删除超出新 EOF 的 folio；
4. 等待/处理 writeback，并清最后一个部分 folio 的尾部；
5. 释放后端块并提交元数据事务。

仅修改 `inode->i_size` 会留下旧缓存数据甚至造成越界数据重新可见。

---

## 12. mmap、direct I/O 与一致性

### 12.1 file-backed mmap fault

文件 mmap 建立 VMA 时，`file->f_op->mmap()` 安装 `vm_ops`。首次访问通常：

```
page fault
  → handle_mm_fault()
  → VMA vm_ops->fault
  → filemap_fault()
      → 按 vm_pgoff + address 计算文件 index
      → page cache lookup
      → 必要时 readahead/read_folio
      → 锁定并验证 folio、i_size、mapping
      → vmf->page/folio 返回给通用 fault
  → 建立 PTE，并加入文件 rmap
```

fault 中等待 I/O 时 VMA 或文件大小可能变化，因此返回前必须重新验证 folio 仍属于该 mapping、
offset 未越过 EOF。超出文件末尾的访问通常产生 `SIGBUS`，而不是匿名零页。

### 12.2 shared writable mmap

`MAP_SHARED` 的首次写 fault 进入 `page_mkwrite`：文件系统在 PTE 变为 writable 之前获得写入
许可、分配块、处理 freeze/journal 并锁定 folio。通用层随后标脏。这个回调防止用户 CPU store
绕开文件系统的空间分配与一致性协议。

`MAP_PRIVATE` 写 fault 则走内存管理 COW，生成匿名 folio，原 page cache 不变。

### 12.3 direct I/O

`O_DIRECT` 主要绕过 page cache 数据路径，把用户页与设备 I/O 直接关联。它仍可能：

- 因对齐、文件系统特性或尾部写而 fallback；
- pin 用户页并建立 bio/iomap；
- 与 page cache 中已有脏/有效 folio 协调；
- 与 truncate、hole punch、mmap fault 通过 inode/invalidate locks 串行化；
- 受设备 DMA、IOMMU 和 cache coherency 约束。

同一范围混用 buffered I/O、mmap 和 direct I/O 会增加 invalidate/flush 成本。应用不应把
`O_DIRECT` 理解为“必然更快”或“自动持久化”；持久化仍取决于同步标志、flush/FUA 和文件系统。

### 12.4 DAX

DAX 文件可把持久内存直接映射进进程页表，绕过普通 page cache 和块 I/O。VFS 接口仍存在，
但 fault、writeback 和持久化语义走 DAX/iomap 专门路径。因为 CPU 可直接写介质映射，cache
flush、日志顺序、reflink/COW 和 truncate 同步更加关键。

---

## 13. 目录项修改：create、link、unlink、rename

### 13.1 create/mkdir

创建操作通常先解析父目录，最后分量保留为 `last`：

```
filename_create()
  → path_parentat() 得到父 path + last
  → 锁 parent inode->i_rwsem
  → lookup/hash last，得到 negative dentry
  → may_create()：权限、只读、sticky、idmap 等检查
  → security_inode_create/mkdir()
  → dir->i_op->create/mkdir()
  → 文件系统分配 inode、写目录项、更新日志
  → d_instantiate()/d_add() 把 dentry 变 positive
  → fsnotify
```

父 inode 锁把“确认名字不存在”和“插入名字”放进同一临界区。negative dentry 是创建目标的
占位对象，不是错误对象；成功后同一个 dentry 可被实例化为 positive。

### 13.2 hard link

`link(old, new)` 为同一非目录 inode 增加一个新名字：锁目标父目录，检查权限与 link count，
调用 `i_op->link`，增加 `i_nlink` 并把新 dentry 加入 inode alias 链。它不复制文件内容，
两个名字的 inode、page cache 和元数据完全相同。

VFS 通常禁止用户创建目录 hard link，避免目录图出现环并破坏 `..`、遍历和引用语义。

### 13.3 unlink 后为什么还能读

`vfs_unlink()` 的概念顺序：

1. 锁父目录并取得 victim inode；
2. 检查目录写/执行权限、sticky bit、lease、immutable 等；
3. 调用 security 和 `dir->i_op->unlink()`；
4. 文件系统删除目录项并减少 `i_nlink`；
5. `d_delete()` 使 dentry negative/disconnected，发 fsnotify；
6. 已打开 file 继续持有 path/inode，内容仍可访问；
7. link count 为 0 且最后引用消失后，`evict_inode()` 才最终回收后端空间。

因此 unlink 删除的是“名字到 inode 的链接”，不是强制关闭所有打开实例。这也是临时文件可以
open 后立即 unlink，并在进程退出时自动消失的基础。

### 13.4 rmdir

rmdir 除 unlink 检查外还要求目标为目录且为空，必须处理当前工作目录、mountpoint、NFS silly
rename 和并发创建。目录 inode 的 parent/child 锁顺序比普通文件更严格，防止环和 ABBA 死锁。

### 13.5 rename 是 VFS 最复杂的操作之一

rename 可能跨目录、覆盖目标、交换两个名字或使用 `RENAME_NOREPLACE/WHITEOUT/EXCHANGE`。
它必须保证观察者不会看到中间状态，并维持目录树无环。

```
do_renameat2()
  → 解析 old/new parent + last
  → lock_rename(old_dir, new_dir)
      ├── 同目录：一把 i_rwsem
      └── 跨目录：按祖先和规定顺序锁两边，检查 trap
  → lookup old/new dentry
  → vfs_rename(struct renamedata)
      → 类型、权限、mount、sticky、lease、delegation 检查
      → security_inode_rename()
      → filesystem i_op->rename()
      → fsnotify / d_move 或 d_exchange 更新 dcache
  → unlock_rename()
```

`rename_lock` seqcount 让 RCU path walk 检测全局 rename 干扰；具体 dentry 的 `d_seq` 让读取者
检测 parent/name 改变。写侧的锁保证结构更新，读侧的 sequence 保证乐观快照自洽。

### 13.6 跨文件系统 rename

普通 rename 要求源和目标属于同一 mounted filesystem，跨 superblock 返回 `EXDEV`。用户态
工具的“跨文件系统 mv”实际是 copy + metadata + unlink，失去同文件系统 rename 的原子性。

---

## 14. inode、dentry 与 file 的生命周期

### 14.1 三套引用不能互相替代

```
file:   get_file()/fput()      最后引用 → ____fput()
path:   path_get()/path_put()  分别持有 dentry + mount
dentry: dget()/dput()          最后活跃引用 → LRU 或 kill/RCU free
inode:  igrab()/iput()         最后引用 → 缓存或 evict_inode
```

file 持有 path，dentry 通常持有 inode 关联，但 dentry refcount 与 inode refcount 并非简单相等。
dcache/inode cache 可在无外部活跃引用时保留对象，shrinker 再根据压力回收。

### 14.2 inode cache 状态机

`iget*()` 先按 `(superblock, inode key)` 查 inode hash：

- 已存在稳定 inode：增加引用返回；
- 不存在：分配 inode，以 `I_NEW` 插入 hash；
- 当前线程从磁盘/后端填充字段，调用 `unlock_new_inode()` 发布；
- 并发查找同一 inode 的线程等待 `I_NEW` 清除；
- 初始化失败用 `iget_failed()` 撤销并唤醒等待者。

这保证同一文件系统对象在内存中通常只有一个 canonical inode，从而让锁、page cache 和元数据
状态集中在同一对象。

### 14.3 inode 回收与删除

`iput()` 让最后活跃引用消失后，inode 可能保留在缓存，也可能进入 eviction：设置
`I_FREEING`，从 hash/list 隔离，等待 writeback，调用 `s_op->evict_inode()` 清 page cache
与文件系统私有资源，最后通过 `destroy_inode`/RCU 释放。

若 `i_nlink == 0`，eviction 常是文件系统真正释放数据块和磁盘 inode 的最后机会；具体事务
可能早在 unlink 开始、也可能在 orphan list/journal recovery 中完成。

### 14.4 dcache 回收

`dput()` 不一定立刻释放 dentry。未使用对象可进入 superblock 的 dentry LRU，保留 positive
和 negative lookup 结果。shrinker 在内存压力下扫描：

1. 隔离 refcount 为 0 的候选；
2. 从父子树、hash 和 inode alias 链摘除；
3. 调用 `d_release` 等回调；
4. 经过 RCU 宽限期释放，保护仍在 RCU-walk 的读者。

父 dentry 因 child 关系和引用形成级联，回收子树需要严格锁顺序，不能简单递归 `kfree`。

### 14.5 file 的凭证快照

`file->f_cred` 保存 open 时凭证。很多 I/O 操作使用这个 opener credential，而不是调用操作时
线程当前凭证，这对 NFS、SCM_RIGHTS 传递 fd、setuid 后继续使用已打开文件很重要。并非所有
权限只在 open 检查一次；ioctl、写入、文件系统后端仍可能做额外检查。

---

## 15. mount tree 与 mount namespace

### 15.1 `struct mount`

VFS 对外常暴露 `struct vfsmount`，内部 `struct mount` 包含：

```
mount
├── mnt                  内嵌 vfsmount：root + superblock
├── mnt_parent           父 mount
├── mnt_mountpoint       在父 mount 上覆盖的 dentry
├── mnt_mounts           子 mount 列表
├── mnt_child            挂入父列表
├── mnt_ns               所属 mount namespace
├── mnt_id / unique_id   标识
├── mnt_count            per-CPU 引用计数体系
└── propagation links    shared/slave/peer group
```

mount tree 与 dentry tree 正交。每个 mount 内部沿 dentry 走，遇到挂载点再跳到另一个 mount。

### 15.2 mount namespace 的复制

`CLONE_NEWNS`/`unshare(CLONE_NEWNS)` 创建 mount namespace。内核复制 mount tree 结构和传播
关系，但通常共享 superblock、inode 和 dentry cache；隔离的是“挂载拓扑视图”，不是复制
文件内容。

修改某 namespace 的 private mount 不影响其他 namespace；shared propagation 则可把 mount/
umount 事件传播给 peer group，并按 master/slave 方向传递。

### 15.3 bind mount

bind mount 以现有 path 为新 mount 的 root，不创建新 superblock。源 dentry 可不是原文件系统
根，所以 `..`、路径连通性和 rename 后的 bind root 都需要 `path_connected()` 等验证。

bind mount 说明 `st_dev + st_ino` 相同的对象可在路径树多处出现，路径字符串不是稳定身份。

### 15.4 lazy unmount

普通 umount 要求 mount 不 busy；`MNT_DETACH` 把 mount 从 namespace 可达树断开，但已有 path/
file 引用仍可继续访问，直到引用耗尽才真正销毁。它和 unlink-open file 是同一种核心模式：

> 先从名字空间撤销可达性，再由引用计数决定对象何时最终释放。

### 15.5 pivot_root 与 chroot

`chroot()` 改 task 的 fs root，但不改变 mount topology，也不是完整安全沙箱；已有 fd、cwd、
其他线程、mount 能力等都可能提供逃逸路径。`pivot_root()` 在 mount namespace 中替换根 mount，
容器通常配合独立 mount namespace、权限下降和其他 namespace 使用。

---

## 16. 权限、安全与 idmapped mount

### 16.1 目录遍历与最终对象权限

访问 `/a/b/file` 至少涉及：

- 对每个目录分量的 search/execute 权限；
- 最终 inode 的 read/write/execute 权限；
- 创建/删除时父目录 write + execute 权限；
- sticky directory 规则；
- mount 的 readonly、noexec、nodev、nosuid 标志；
- POSIX ACL、capabilities、LSM、fscrypt、verity 等附加规则。

“对文件有写权限”不代表能 unlink；unlink 修改的是父目录。反之，能 unlink 一个文件也不一定
能打开读取它。

### 16.2 DAC 与 capabilities

`inode_permission()` 先处理通用 DAC、ACL 和 mount/idmap 语义，再进入文件系统/LSM hook。
root 也不是无条件绕过一切：capability 被 user namespace 限定，LSM、只读 mount、immutable
flag 和文件系统状态可继续拒绝。

### 16.3 idmapped mount

idmapped mount 给某个 mount 附加 UID/GID 映射，使同一 inode 在该挂载视图下以映射后的
ownership 参与权限和创建。它不递归 chown 磁盘 inode，也不同于单纯 user namespace。

VFS 修改接口传递 `struct mnt_idmap *`，具体文件系统必须用 `vfsuid/vfsgid` helper 进行正确
比较和写回。忽略 idmap 直接比较裸 `i_uid` 会导致权限或所有权错误。

### 16.4 LSM hook 的位置

LSM 同时在 path/inode/file 多层插入 hook，例如：

- `security_path_*`：基于完整 path 的操作；
- `security_inode_permission/create/unlink/rename`：对象与命名操作；
- `security_file_open/permission/ioctl`：打开实例和后续使用。

多层不是重复：open 后 fd 可能经 SCM_RIGHTS 传给另一进程，path 也可能 rename；不同安全模型
需要在不同稳定对象上做决策。

---

## 17. 并发控制与锁顺序

### 17.1 常见同步机制

| 机制 | 保护内容 |
|------|----------|
| RCU + `d_seq` | dentry RCU-walk 字段与对象延迟释放 |
| `d_lockref` | dentry 引用、flags 和结构修改 |
| `rename_lock` | rename 全局 sequence，辅助祖先/路径验证 |
| inode `i_rwsem` | lookup/create/unlink/rename 与文件数据操作的规定部分 |
| inode `i_lock` | inode flags、计数、短状态更新 |
| `file_lock` | fdtable bitmap、槽位与扩容 |
| `f_pos_lock` | 共享 file position |
| `invalidate_lock` | page cache invalidate、truncate、DIO 协调 |
| folio lock | 单个缓存 folio 的填充、截断、写入状态 |
| `s_umount` | superblock mount/unmount/freeze 生命周期 |
| `mount_lock` sequence | mount tree 读验证与写侧更新 |

### 17.2 目录锁顺序

同目录操作通常锁该目录 inode。跨目录 rename 可能需要锁两个目录和被移动目录，必须按 VFS
定义的祖先/地址顺序执行；`lock_rename()` 同时返回 trap dentry 用于拒绝把目录移入自己的
子树。文件系统 locking 文档是实现 `i_op` 的强制契约。

自行按“先 old 后 new”加两把 `i_rwsem` 会在反向 rename 时死锁。

### 17.3 dentry 锁与 inode 锁的职责

dentry lock 保护缓存拓扑和名字关联，inode lock 保护文件系统对象状态。rename 同时改变两者，
所以写侧在规定锁下更新，再通过 seqcount 让无锁读者发现变化。只持 inode 锁不能裸改
`d_parent/d_name`，只持 dentry 锁也不能提交文件系统目录事务。

### 17.4 freeze 协议

文件系统 freeze 需要阻止新的写操作并等待不同阶段的在途写者、page fault、内部事务和
writeback 排空。`file_start_write()/file_end_write()`、`mnt_want_write()` 等 helper 把操作
纳入 freeze/write accounting。

绕过 VFS helper 直接调用写回调，可能在 freeze 已进入下一阶段后启动新事务，导致快照或卸载
一致性破坏。

### 17.5 TOCTOU 与稳定句柄

路径名会被 rename、symlink 和 mount 并发改变。“检查 path，再用同一字符串 open”不是原子
操作。更可靠的方式包括：

- 用 `openat2` resolve flags 在同一次 lookup 中限制路径；
- 逐级打开目录 fd，并用 `*at` 接口相对操作；
- 得到 fd 后用 `fstat` 检查已打开对象，而不是重新 stat 路径；
- 需要空路径操作时使用支持的 `AT_EMPTY_PATH`/fd-based API。

---

## 18. 缓存回收、writeback 与卸载

### 18.1 dentry/inode shrinker

VFS 注册 shrinker，使内存回收器可扫描未使用 dentry 和 inode。回收必须区分：

- 有外部引用：不能回收；
- 无引用但缓存有价值：留在 LRU；
- dirty inode：需要先 writeback；
- inode 仍有 page cache/writeback：按 eviction 协议清理；
- 网络文件系统对象：可能需要 `d_revalidate` 或远程释放。

negative dentry 可显著加速不存在路径查询，但大量随机不存在名字也可能占据内存；shrinker 会在
压力下回收它们。

### 18.2 writeback 的归属

inode 的 `address_space` 连接 page cache 与 backing device/writeback domain。脏 inode 被加入
相应 writeback list，flusher 按时间、阈值、sync 请求或内存压力调用文件系统 `writepages`。

数据写出和元数据提交可能是不同阶段。日志文件系统需要保证 journal、data block、inode size
和目录项之间的崩溃一致性；VFS 只提供通用 writeback/fsync 框架，不替文件系统决定磁盘顺序。

### 18.3 fsync 的层次

`vfs_fsync_range()` 调用 `file->f_op->fsync()`。文件系统需要：

1. 写出指定范围的脏数据；
2. 提交使这些数据可达所需的 inode/extent 元数据；
3. 按语义处理父目录项持久化（通常创建/rename 后还需 fsync 目录）；
4. 向块层发出需要的 flush/FUA；
5. 报告之前积累的 writeback error。

仅 `fsync(file)` 是否保证 rename/create 的目录项持久化依赖操作和文件系统语义，要求崩溃一致
的应用通常还要 fsync 所在目录。

### 18.4 卸载为何会 busy

mount 上的 cwd/root、打开 file、内核 path、子 mount 等都可使其 busy。卸载过程先从 namespace
隔离或确认无引用，再 shutdown superblock：停止新操作、同步/丢弃缓存、调用 `put_super` 和
`kill_sb`、释放设备与模块引用。

superblock 生命周期不只由 mount 数决定：active references、打开对象和内部工作也参与。
强行把 mount 从树上摘除不允许提前释放仍被 file->f_path 引用的 superblock。

---

## 19. 伪文件系统与叠加文件系统

### 19.1 procfs/sysfs/debugfs

VFS 对象模型不要求持久磁盘：

- procfs 根据进程和内核状态动态生成目录/inode/内容；
- sysfs 把 kobject 层次投影为文件树；
- debugfs 为调试接口提供低约束文件模型；
- tmpfs 用内存/swap 作为数据后端，仍完整使用 inode、dentry 和 page cache/folios。

读取这些文件可能执行内核代码并生成快照，不应把它们当普通磁盘文件。`st_size` 可能为 0，
但 read 仍返回数据；seek、mmap、poll 语义也由各自 `file_operations` 决定。

### 19.2 seq_file

大量虚拟文本文件使用 seq_file，把“从某位置开始迭代对象”和“格式化输出”分离：

```
start(pos) → show(item) → next(item, pos) ... → stop(item)
```

seq_file 管理缓冲区扩展、部分 read、seek 和跨多次系统调用位置。实现者仍必须在迭代期间正确
使用锁/RCU，不能把已经失效的内核对象指针保存在用户可控的文件位置里。

### 19.3 overlayfs

overlayfs 把 upper、lower 和 workdir 组合成单一 VFS 视图。其 dentry/inode/file 是 overlay
对象，内部再引用 real path：

- 读可来自 lower；
- 首次修改可能触发 copy-up 到 upper；
- 删除 lower 对象通过 whiteout 隐藏；
- rename、hard link、xattr 和 file handle 需要跨层一致性；
- 打开前后 real file 选择必须保持正确生命周期。

它说明 VFS operation table 不只“连接磁盘驱动”，也能实现栈式文件系统。路径解析先到 overlay
dentry，overlay 回调内部再操作下层 VFS 对象，锁和凭证覆盖因此格外复杂。

---

## 20. 观测、调试与常见误区

### 20.1 用户空间观测

```bash
# 查看 fd、mount 和 namespace
ls -l /proc/<pid>/fd
cat /proc/<pid>/fdinfo/<fd>
findmnt -o TARGET,SOURCE,FSTYPE,OPTIONS,PROPAGATION
cat /proc/<pid>/mountinfo
readlink /proc/<pid>/ns/mnt

# 跟踪 VFS 相关系统调用
strace -ff -yy -e trace=%file,%desc <command>

# 缓存与写回
cat /proc/meminfo
cat /proc/sys/fs/dentry-state
cat /proc/sys/fs/inode-state
cat /proc/vmstat | grep -E 'dirty|writeback|pgscan|pgsteal'

# 打开文件和持有者
lsof -p <pid>
fuser -vm <mountpoint>
```

`/proc/<pid>/fd/N` 的 symlink 文本只是展示；真正稳定对象是已打开 fd。显示 `(deleted)` 表示
dentry 名字已删除但 file 仍持有 inode/path，不表示 fd 失效。

### 20.2 内核跟踪

- tracepoints：`filemap`、`writeback`、`ext4/xfs`、系统调用 enter/exit；
- ftrace function graph：跟踪 `path_openat`、`do_dentry_open`、`vfs_*`；
- eBPF：kprobe/fentry 统计 lookup miss、open 延迟、writeback error；
- perf lock：分析 inode/dentry 锁竞争；
- drgn/crash：遍历 files_struct、mount tree、dentry/inode 和 page cache；
- lockdep：验证文件系统回调的目录锁与 reclaim 上下文锁顺序。

生产环境动态跟踪 path 字符串要注意 fault、并发 rename 和隐私；优先在稳定 hook 获取已有
`struct path`，并限制复制长度。

### 20.3 常见误区

**误区一：fd 就是 inode。** fd 指向 file；file 再指向 path/inode。dup 共享 file，两次 open
通常不共享 file。

**误区二：dentry 是磁盘目录项。** dentry 是内存缓存对象，也可以是 negative 或 disconnected。

**误区三：一个 inode 只有一个路径。** hard link、bind mount、namespace 和 overlay 都会让
路径与 inode 形成多对多视图。

**误区四：close 一定立即调用驱动 release。** 只有最后 file 引用消失才 release，而且 fput
可能因上下文而延迟。

**误区五：unlink 会让已打开 fd 失效。** 它删除名字；file 引用让 inode 和内容继续存在。

**误区六：read/write 总会访问磁盘。** buffered I/O 命中 page cache 时不需要设备 I/O；写入
也通常先变 dirty，稍后回写。

**误区七：fsync 等同于 fflush。** fflush 把用户态 libc buffer 交给内核；fsync 要求内核与
存储设备完成更强的持久化协议。

**误区八：`O_DIRECT` 就是完全零拷贝且持久化。** 它主要绕过 page cache，仍可能 pin、映射、
fallback，并且不自动等价于 durable write。

**误区九：chroot 是安全容器。** 它只改变路径解析 root，不能单独隔离 mount、fd、权限和内核
攻击面。

**误区十：先 realpath 再 open 可以防 symlink 竞态。** 两次路径遍历之间可被 rename；应使用
fd-relative API 和 `openat2` 原子解析约束。

---

## 21. 关键函数与完整调用链速查

### 21.1 关键函数

| 阶段 | 函数 | 位置 | 核心作用 |
|------|------|------|----------|
| open syscall | `do_sys_openat2()` | `fs/open.c` | 分配 fd、打开 file、最终安装 |
| open 主路径 | `do_file_open()` / `path_openat()` | `fs/namei.c` | RCU/REF/REVAL 三阶段尝试 |
| 路径初始化 | `path_init()` | `fs/namei.c` | 选择 root/cwd/dirfd 起点 |
| 分量遍历 | `link_path_walk()` / `walk_component()` | `fs/namei.c` | 逐级查找与进入 |
| dcache 快路 | `lookup_fast()` | `fs/namei.c` | RCU/引用模式下缓存查找 |
| 后端慢路 | `lookup_slow()` | `fs/namei.c` | 锁父 inode 并调用文件系统 lookup |
| RCU 升级 | `try_to_unlazy()` / `try_to_unlazy_next()` | `fs/namei.c` | 把乐观快照转换成稳定 path 引用 |
| 最终分量 | `open_last_lookups()` / `do_open()` | `fs/namei.c` | create/exclusive/symlink/open 状态机 |
| file 打开 | `do_dentry_open()` | `fs/open.c` | 初始化 file 并调用 `f_op->open` |
| fd 分配 | `get_unused_fd_flags()` | `fs/file.c` | 保留槽位并设置 fd flags |
| fd 发布 | `fd_install()` | `fs/file.c` | RCU 发布完整 file |
| fd 关闭 | `close_fd()` | `fs/file.c` | 摘除槽位并启动 file close |
| file 终结 | `fput()` / `____fput()` | `fs/file_table.c` | 最后引用的 release 与资源清理 |
| 同步读取 | `vfs_read()` / `new_sync_read()` | `fs/read_write.c` | 通用检查并转 read_iter |
| 同步写入 | `vfs_write()` / `new_sync_write()` | `fs/read_write.c` | 通用检查并转 write_iter |
| 缓存读取 | `filemap_read()` | `mm/filemap.c` | page cache lookup、预读和复制 |
| mmap fault | `filemap_fault()` | `mm/filemap.c` | 取得文件 folio 供 PTE 映射 |
| 创建 | `vfs_create()` / `vfs_mkdir()` | `fs/namei.c` | 权限/LSM 后调用 inode operation |
| 删除 | `vfs_unlink()` / `vfs_rmdir()` | `fs/namei.c` | 删除命名链接并更新 dcache |
| 改名 | `vfs_rename()` | `fs/namei.c` | 原子目录树修改协议 |
| inode 获取 | `iget_locked()` 等 | `fs/inode.c` | inode cache 查找与 I_NEW 发布 |
| dentry 释放 | `dput()` | `fs/dcache.c` | 减引用、LRU 或 kill |
| mount | `do_new_mount_fc()` 等 | `fs/namespace.c` | 把新 mount 接入 namespace |
| superblock | `sget_fc()` / `deactivate_locked_super()` | `fs/super.c` | superblock 查找、创建与停用 |

### 21.2 一次缓存命中的 open + read + close

```
openat2("/a/b", O_RDONLY)
  → reserve fd=3
  → RCU path walk
      root → dcache hit "a" → dcache hit "b"
      每层验证 d_seq/mount sequence
  → alloc/init file → do_dentry_open → filesystem open
  → fd_install(3, file)

read(3, buf, len)
  → RCU fd lookup + get file ref
  → vfs_read → read_iter → filemap_read
  → XArray page-cache hit → copy_to_iter
  → 更新共享 f_pos → put file ref

close(3)
  → fdtable 槽位置空，fd=3 可复用
  → fput
  → 若最后引用：release → path_put → dput/mntput → file free
```

该路径在理想情况下既不访问磁盘，也不在每个路径分量修改引用计数；性能来自 RCU-walk、
dcache、page cache 和 fd RCU lookup 四层缓存/乐观读取。

### 21.3 一次 O_CREAT 缓存未命中的 open

```
openat2("dir/new", O_CREAT|O_EXCL|O_WRONLY)
  → reserve fd
  → 查到 parent "dir"
  → last component "new" lookup miss
  → 退出 RCU，进入 REF-walk
  → 锁 dir inode i_rwsem
  → d_alloc_parallel 建 in-lookup dentry
  → 文件系统确认不存在，形成 negative dentry
  → may_create + LSM + mount write/freeze accounting
  → filesystem create/atomic_open
      → 分配并初始化 inode
      → 提交目录项/日志
      → d_instantiate：negative → positive
  → 初始化 file，解父目录锁
  → fd_install
```

这里“查无此名”本身是创建事务的必要输入；父目录锁和 parallel lookup 防止两个并发
`O_EXCL` 调用都成功。

### 21.4 一次 unlink-open-file 的完整生命周期

```
fd → file → path → dentry → inode (i_nlink=1)

unlink(path)
  → 锁 parent
  → 文件系统删除目录项，i_nlink: 1 → 0
  → dentry 从命名树断开/变 negative
  → path lookup 再也找不到该名字

已有 fd read/write
  → file 仍持有 path/inode
  → page cache 与后端块仍有效

close(fd)
  → 最后 file/path/inode 引用下降
  → iput → evict_inode
  → 清 page cache、释放后端块和磁盘 inode
  → dentry/inode 经缓存或 RCU 最终释放
```

### 21.5 阅读 VFS 源码时应持续检查的六个问题

1. 当前持有的是路径字符串、RCU 快照、`struct path` 引用，还是 file 引用？
2. 操作的是名字关系（dentry/父目录），还是文件身份与内容（inode/address_space）？
3. 当前在 RCU-walk 还是 REF-walk，回调能否睡眠？
4. 哪把锁使“检查条件 + 修改”成为原子操作，锁顺序由谁规定？
5. 对象何时从 namespace 不可达，何时最后引用归零，何时经过 RCU 真正释放？
6. 通用 VFS helper 在具体文件系统回调前后补充了哪些权限、LSM、freeze、fsnotify 和记账？

掌握这些不变量后，VFS 不再是一长串函数指针：它是一套围绕**名字解析、稳定引用、并发发布、
缓存一致性和延迟回收**建立的对象协议。
