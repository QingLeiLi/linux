# Linux cgroup 系统入门：从资源分组到内核生命周期

> 适用内核版本：v7.2-rc1（基于当前仓库 `f8f7ac7435bf`）
>
> 核心源码：`kernel/cgroup/`、`include/linux/cgroup*.h`、`include/linux/cgroup_subsys.h`
>
> 用户接口：以 cgroup v2 为主；v1 只用于解释历史设计与兼容路径

---

## 目录

1. [从一个受限服务开始](#1-从一个受限服务开始)
2. [cgroup 解决什么问题](#2-cgroup-解决什么问题)
3. [Linux 的总体方案](#3-linux-的总体方案)
4. [宏观地图](#4-宏观地图)
5. [先理解 cgroup v2 的用户模型](#5-先理解-cgroup-v2-的用户模型)
6. [四个核心对象](#6-四个核心对象)
7. [目录和接口文件从哪里来](#7-目录和接口文件从哪里来)
8. [创建 cgroup：先构造，最后激活](#8-创建-cgroup先构造最后激活)
9. [启用 controller：一次层级事务](#9-启用-controller一次层级事务)
10. [把进程迁入 cgroup](#10-把进程迁入-cgroup)
11. [fork、clone3 与退出路径](#11-forkclone3-与退出路径)
12. [controller 如何接入 core](#12-controller-如何接入-core)
13. [资源控制的四种基本模型](#13-资源控制的四种基本模型)
14. [domain、threaded 与两条 v2 约束](#14-domainthreaded-与两条-v2-约束)
15. [删除为什么不是立即释放](#15-删除为什么不是立即释放)
16. [并发、锁、引用和 RCU](#16-并发锁引用和-rcu)
17. [namespace、delegation 与容器](#17-namespacedelegation-与容器)
18. [可运行的观察实验](#18-可运行的观察实验)
19. [常见误区与故障排查](#19-常见误区与故障排查)
20. [回到开头案例](#20-回到开头案例)
21. [源码阅读路线与关键函数索引](#21-源码阅读路线与关键函数索引)
22. [快速参考](#22-快速参考)

---

## 1. 从一个受限服务开始

假设机器上运行一个图片处理服务。它会派生 worker，偶尔因异常图片占用大量内存，并与
其他服务竞争 CPU 和存储 I/O。我们希望满足：

- 服务及其后代最多使用 1 GiB 内存；
- CPU 繁忙时，它只获得相对较低的份额；
- 最多创建 128 个进程；
- 能观察 CPU、内存和 I/O 压力；
- 服务退出后，可以判断整个进程树是否已经清空。

在 cgroup v2 中，这些要求大致表现为：

```bash
CG=/sys/fs/cgroup/image-worker

mkdir "$CG"
echo 1G  > "$CG/memory.max"
echo 50  > "$CG/cpu.weight"
echo 128 > "$CG/pids.max"
echo <pid> > "$CG/cgroup.procs"

cat "$CG/memory.current"
cat "$CG/cpu.stat"
cat "$CG/memory.pressure"
cat "$CG/cgroup.events"
```

这些命令表面上只是在一个特殊文件系统中创建目录、读写文件，内核却必须解决一组更深的问题：

1. 一个多线程进程怎样作为整体迁移，避免线程散落到不同资源域？
2. 新 fork 的 child 怎样从出生起继承正确的资源归属？
3. memory、CPU、I/O 等 controller 怎样共享同一棵任务树，又保存各自状态？
4. 修改 controller 时，如果第六个节点分配失败，前五个节点怎样恢复？
5. 删除目录后，正在进行的 I/O、统计读取或 RCU 遍历怎样安全结束？

本文会一直使用这个服务作为主线，把用户可见文件映射到内核对象和状态变化。

---

## 2. cgroup 解决什么问题

### 2.1 namespace 与 cgroup 不是同一件事

namespace 主要改变进程能看见的系统视图：

```text
PID namespace      看见哪些 PID
mount namespace    看见哪棵挂载树
network namespace  看见哪些网络设备和协议栈
user namespace     UID/GID 和能力的解释方式
```

cgroup 主要回答另一组问题：

```text
进程属于哪个资源组？
资源消耗记到哪里？
同级工作负载怎样分配资源？
到达限制后应该节流、回收、拒绝还是触发事件？
```

容器通常同时使用 namespace 和 cgroup，但二者可以独立存在。namespace 不会自动限制资源，
cgroup 也不会让进程看见一套独立的 PID 或网络设备。

### 2.2 只给每个进程保存一组限额为什么不够

最直观的方案是在 `task_struct` 中保存：

```text
memory_limit
cpu_weight
io_limit
pids_limit
```

这个方案很快遇到问题：

- 服务由很多进程和线程组成，需要按整体统计和控制；
- child 应继承 parent 的资源归属，而不是重新配置全部限制；
- 组织结构天然有层级：机器 → 租户 → 服务 → worker；
- CPU、内存和 I/O 的资源模型不同，不能共用一种限额算法；
- 每个 task 保存所有 controller 状态会浪费内存，fork/exit 也会反复维护相同数据；
- 管理程序需要通过稳定接口动态创建、迁移和观察资源组。

因此 Linux 把“任务组织”和“具体资源策略”分开：

```text
cgroup core
  维护层级、任务归属、迁移、文件接口和生命周期

controllers
  CPU、memory、I/O、pids、cpuset 等各自实现记账和策略
```

### 2.3 cgroup 的核心矛盾

cgroup 主要在以下矛盾之间折中：

- **统一组织与多种资源语义**：所有 controller 共用任务树，但各自状态独立；
- **迁移灵活性与热路径成本**：允许运行时迁移，但不能让每次 fork、exit、记账都取得全局重锁；
- **精确层级统计与扩展性**：父节点需要聚合后代，但每次资源事件不能逐级争锁；
- **立即删除与并发读者安全**：用户希望 `rmdir` 立即返回，旧引用和 RCU 读者却可能仍在使用对象；
- **委托管理与不可逃逸**：容器可管理自己的子树，但不能把任务移出上级限制。

---

## 3. Linux 的总体方案

### 3.1 一棵任务组织树，多组 controller 状态

cgroup v2 使用一棵统一 hierarchy。每个进程在普通 domain 模式下属于唯一 cgroup；每个
controller 在树上为需要的节点创建自己的状态对象。

```text
root
├── system
│   ├── ssh.service
│   └── database.service
└── workloads
    └── image-worker
        ├── request-A
        └── request-B
```

同一个 `image-worker` 节点可以同时对应：

```text
struct cgroup                 core 的层级节点
struct mem_cgroup             memory controller 状态
struct task_group             CPU controller 状态
struct blkcg                  I/O controller 状态
struct pids_cgroup            pids controller 状态
```

controller 对象都嵌入一个共同基类 `struct cgroup_subsys_state`，简称 **css**。core 只依赖
css 的通用生命周期，controller 再通过 `container_of()` 找回自己的完整对象。

### 3.2 task 不直接拥有整套 controller 对象

任务通过 `task_struct::cgroups` 指向一个 `struct css_set`。`css_set` 保存每个 controller
对应的 css 指针。拥有相同组合的任务共享同一个 `css_set`。

```text
task A ─┐
task B ─┼──> css_set
task C ─┘      ├── subsys[memory] ──> mem_cgroup.css
               ├── subsys[cpu]    ──> task_group.css
               ├── subsys[io]     ──> blkcg.css
               └── dfl_cgrp       ──> image-worker cgroup
```

收益是 fork 和 exit 通常只需增减一份 `css_set` 引用，而不是逐 controller 操作。代价是迁移
必须查找或创建目标 css 组合，并维护 `cgroup ↔ css_set` 的反向链接。

### 3.3 用户 ABI 建在 kernfs 上

cgroup2 是一种文件系统。目录表示 cgroup，文件表示 core 或 controller 操作：

```text
mkdir/rmdir        创建或销毁 cgroup
cgroup.procs       查询或迁移进程
cgroup.controllers 查询可下放的 controller
cgroup.subtree_control
                   向 child 下放 controller
memory.max         memory controller 的限制
cpu.weight         CPU controller 的权重
```

这些不是磁盘文件。kernfs 把 VFS 操作转发到 `struct cftype` 中注册的回调。

### 3.4 方案收益与代价

| 选择 | 收益 | 代价 |
|------|------|------|
| core/controller 分离 | 一棵树可服务多种资源策略 | css 生命周期和依赖关系复杂 |
| 共享 `css_set` | task 占用小，fork/exit 快 | 迁移需哈希查找、预分配和批量提交 |
| kernfs 文件 ABI | 可脚本化、可轮询、与目录权限结合 | 文件创建/隐藏必须和 css 发布同步 |
| 层级限制 | 上级约束不能被下级绕过 | controller 配置受 top-down 规则限制 |
| RCU + 延迟释放 | 高频读者可以低成本查找 | “目录消失”和“对象释放”不在同一时刻 |

---

## 4. 宏观地图

### 4.1 子系统边界

```text
用户空间
mkdir / echo PID / 写 controller 文件 / rmdir / poll
       │
       ▼
VFS + cgroup2 filesystem + kernfs
       │
       ├── core 文件回调 ───────> kernel/cgroup/cgroup.c
       │                          层级、迁移、css_set、生命周期
       │
       └── controller 文件回调 ─> mm/memcontrol.c
                                  kernel/sched/
                                  block/blk-cgroup.c
                                  kernel/cgroup/pids.c ...
```

### 4.2 对象关系

```text
cgroup_root
└── cgroup root
    └── cgroup child
        ├── self                         通用层级 css
        ├── subsys[memory] ─────────┐
        ├── subsys[cpu] ────────────┼── controller css
        ├── kn ──> kernfs directory │
        └── cset_links              │
                                    │
task_struct ──> css_set ──> subsys[]┘
                     └── dfl_cgrp ──> cgroup child
```

### 4.3 主过程地图

```text
启动：
  cgroup_init_early()
    → cgroup_init()
      → cgroup_setup_root()
        → 注册 controller 和接口文件

创建：
  mkdir()
    → kernfs
      → cgroup_mkdir()
        → cgroup_create()
        → css_populate_dir()
        → cgroup_apply_control_enable()
        → kernfs_activate()

迁移：
  write(cgroup.procs, pid)
    → __cgroup_procs_write()
      → 权限与目标检查
      → 预装载 source/destination css_set
      → controller can_attach()
      → cgroup_migrate()
      → controller attach()

删除：
  rmdir()
    → cgroup_rmdir()
      → cgroup_destroy_locked()
        → kill_css_sync()
        → percpu_ref kill
        → offline
        → refcount zero
        → RCU grace period
        → controller css_free()
```

后续每条调用链都可以放回这三张图中：文件系统负责入口，core 负责一致性，controller 负责资源语义。

---

## 5. 先理解 cgroup v2 的用户模型

### 5.1 一个进程属于哪里

查看当前 shell 的 cgroup：

```bash
cat /proc/self/cgroup
```

在纯 v2 hierarchy 中通常看到：

```text
0::/user.slice/user-1000.slice/session-3.scope
```

`0` 是统一 hierarchy 的标识，第二列 controller 列表为空，第三列是相对 cgroup 路径。

查看挂载：

```bash
findmnt -t cgroup2
```

### 5.2 `cgroup.controllers` 与 `cgroup.subtree_control`

这两个文件最容易混淆：

```text
cgroup.controllers
  当前节点可以下放给 child 的 controller 集合

cgroup.subtree_control
  当前节点已经决定下放给 child 的 controller 集合
```

启用 memory：

```bash
echo +memory > cgroup.subtree_control
```

含义不是“限制当前 cgroup 的内存”，而是“让当前节点的直接 children 获得 memory controller
接口，由当前节点在 children 之间分配内存”。所以 controller 文件从所有权角度属于 parent
对 child 的资源分配界面。

### 5.3 top-down constraint

child 只能继续下放 parent 已经提供的 controller：

```text
root.subtree_control     = +memory
A.subtree_control        可以 +memory
A/B.subtree_control      只有 A 已 +memory 后才可以 +memory
```

上级限制不会被下级覆盖。child 的 `memory.max` 可以比 parent 更紧，但不能让自己突破 parent
已经施加的总限制。

### 5.4 no-internal-process constraint

非 root 的普通 domain cgroup 若要把 domain controller 下放给 children，自身不能同时包含
进程。典型布局应把组织节点与运行节点分开：

```text
错误直觉：
service/               有 manager 进程，又向 child 下放 memory
└── workers/

常见布局：
service/               只负责向下分配资源，自身无进程
├── manager/           放 manager
└── workers/           放 workers
```

这样 controller 在任一有效子树中只需比较 sibling cgroups，不必同时比较“parent 中的裸进程”
和 child cgroups 两种不同实体。

### 5.5 核心接口速览

| 文件 | 作用 |
|------|------|
| `cgroup.type` | domain/threaded 类型 |
| `cgroup.procs` | 进程 TGID 列表；写入时迁移整个进程 |
| `cgroup.threads` | 线程 TID 列表；受 threaded domain 约束 |
| `cgroup.controllers` | 当前可下放 controller |
| `cgroup.subtree_control` | 向直接 children 启用/禁用 controller |
| `cgroup.events` | `populated`、`frozen` 等可轮询状态 |
| `cgroup.stat` | live/dying cgroup 与 css 统计 |
| `cgroup.freeze` | 异步冻结/解冻子树 |
| `cgroup.kill` | 向整个子树的用户进程发送 `SIGKILL` |
| `cpu.stat` | core 聚合的 CPU 时间与节流统计 |
| `*.pressure` | PSI 压力统计和 trigger |

---

## 6. 四个核心对象

### 6.1 `struct cgroup_root`：一棵 hierarchy

`struct cgroup_root` 定义于 `include/linux/cgroup-defs.h`。重要关系是：

```c
struct cgroup_root {
	struct kernfs_root *kf_root;
	unsigned int subsys_mask;
	int hierarchy_id;
	struct list_head root_list;
	unsigned int flags;
	struct cgroup cgrp;
};
```

- `kf_root` 把 hierarchy 接到 kernfs；
- `subsys_mask` 表示绑定到这棵 hierarchy 的 controller；
- `hierarchy_id` 用于 legacy hierarchy 标识；
- `cgrp` 内嵌根 cgroup，保证 root 和 hierarchy 生命周期相连。

v2 只有一棵默认统一 hierarchy，即 `cgrp_dfl_root`。v1 兼容代码仍允许 controller 绑定到其他
hierarchy，因此 core 中仍保留 `cgroup_root` 抽象。

### 6.2 `struct cgroup`：树上的组织节点

关键字段按用途分组：

```text
层级：
  self
  level
  ancestors[]
  root

用户接口：
  kn
  procs_file
  events_file
  psi_files[]

controller：
  subtree_control
  subtree_ss_mask
  subsys[]
  dom_cgrp

任务反向关系：
  cset_links
  e_csets[]

生命周期和统计：
  nr_descendants
  nr_dying_descendants
  nr_dying_subsys[]
  offline_waitq
  rstat_base_cpu
```

`self` 是 `ss == NULL` 的通用 css，它让 cgroup core 自己也复用 css 的父子遍历、引用和延迟
释放框架。

`ancestors[]` 缓存从 root 到 self 的祖先指针。判断祖先关系时可按 `level` 直接索引，而不必
每次向上遍历整条链。代价是创建节点时需要按深度分配柔性数组并填充祖先。

`subtree_control` 是用户显式请求的位图，`subtree_ss_mask` 是加入隐式 controller 和依赖后
真正需要物化的位图。二者不能混为一谈。

### 6.3 `struct cgroup_subsys_state`：controller 状态的共同基类

css 的核心职责是给不同 controller 提供统一生命周期：

```text
cgroup / ss       属于哪个节点、哪个 controller
parent/children   controller 自己的 css 层级
refcnt            online 引用和最终释放
id                controller 内唯一 ID
flags             ONLINE、DYING 等状态
online_cnt        保证 child 先于 parent offline
rstat_cpu         分层统计
destroy_work      可睡眠的延迟销毁阶段
destroy_rwork     RCU 宽限期后的阶段
```

controller 常把 css 嵌入自己的对象：

```c
struct example_cgroup {
	struct cgroup_subsys_state css;
	/* controller 私有状态 */
};
```

core 调用 `ss->css_alloc()` 得到 `css`，controller 回调再使用 `container_of()` 访问私有字段。

### 6.4 `struct css_set`：task 的资源身份

`css_set` 不是树节点，而是一个不可变的 controller 状态组合：

```text
subsys[]          各 controller 的 effective css
dfl_cgrp          v2 对应的组织节点
dom_cset          threaded 模式的 domain 归属
refcount          共享该组合的 task 和临时持有者
tasks             正常成员
mg_tasks          迁移事务中的成员
dying_tasks       退出但仍可能被迭代器观察的成员
cgrp_links        指向相关 cgroup 的反向链接
hlist             全局哈希表中的同组合查找
```

为什么 `subsys[]` 创建后基本不可变？如果原地修改一个共享 `css_set`，所有使用它的 task 会在
同一瞬间被无意迁移。正确做法是找到或创建新的目标组合，再把选定 task 从旧 `css_set` 链到
新 `css_set`。

---

## 7. 目录和接口文件从哪里来

### 7.1 `struct cftype`

core 和 controller 都用 `struct cftype` 描述文件：

```c
struct cftype {
	char name[MAX_CFTYPE_NAME];
	unsigned int flags;
	int (*seq_show)(struct seq_file *, void *);
	ssize_t (*write)(struct kernfs_open_file *,
			 char *, size_t, loff_t);
	/* read_u64、write_u64、poll、open、release 等 */
};
```

`kernel/cgroup/cgroup.c` 中的 `cgroup_base_files[]` 注册 `cgroup.procs`、
`cgroup.subtree_control`、`cgroup.events` 等 core 文件。controller 的
`dfl_cftypes` 注册 `memory.*`、`cpu.*`、`io.*` 等文件。

### 7.2 文件回调怎样定位对象

kernfs node 的私有数据连接目录和 `struct cgroup`，cftype 连接具体操作。打开文件后，
`of_css()`、`seq_css()` 等 helper 得到关联 css。

```text
VFS file
  → kernfs_open_file
    → kernfs_node
      → cgroup / cftype
        → cgroup_subsys_state
          → controller 私有对象
```

回调运行期间 kernfs active reference 保证节点不会在脚下失效；需要跨回调保存对象时仍必须
按 css/cgroup 引用协议显式取引用。

### 7.3 为什么创建后要 `kernfs_activate`

创建目录和文件时使用未激活节点。只有 cgroup、css、controller 文件和反向关系全部成功后，
`kernfs_activate()` 才让普通用户访问。

这和其他内核“先构造、后发布”协议相同：

```text
未激活：
  可以回滚，外部读者看不见半初始化对象

激活：
  用户可打开文件，后续失败不能再简单 kfree
```

---

## 8. 创建 cgroup：先构造，最后激活

创建 `image-worker` 的调用骨架：

```text
mkdir("/sys/fs/cgroup/image-worker")
  → VFS
    → kernfs mkdir
      → cgroup_mkdir()
        → cgroup_kn_lock_live(parent)
        → cgroup_check_hierarchy_limits()
        → cgroup_create()
        → css_populate_dir(&cgrp->self)
        → cgroup_apply_control_enable()
        → TRACE_CGROUP_PATH(mkdir, ...)
        → kernfs_activate(cgrp->kn)
```

### 8.1 `cgroup_create()` 的构造阶段

状态变化可以概括为：

```text
阶段 1：分配 cgroup + ancestors[]，初始化 self percpu_ref
阶段 2：创建未激活 kernfs 目录
阶段 3：初始化锁、链表、rstat、PSI、freezer 和 ancestor 缓存
阶段 4：通知 lifetime notifier
阶段 5：提交到 parent children 链和祖先统计，取得 parent 引用
阶段 6：传播 effective controller mask
```

提交点在所有可能失败的基础初始化之后。提交前失败按相反顺序释放：

```text
psi_cgroup_free()
  → css_rstat_exit()
    → kernfs_remove()
      → percpu_ref_exit()
        → kfree(cgrp)
```

一旦接入 parent 的 RCU children 链，销毁就必须走统一的 cgroup/css 生命周期，不能直接释放。

### 8.2 controller css 与文件发布

`cgroup_apply_control_enable()` 自顶向下遍历新节点相关子树：

```text
需要 css 且尚不存在
  → css_create()
    → controller css_alloc()
    → 初始化 css 引用、ID、rstat
    → 接入 parent css children
    → online_css()

css 对用户可见
  → css_populate_dir()
```

自顶向下保证 controller 创建 child 状态时，parent css 已经存在并 online。

### 8.3 创建失败为什么可以不暴露残缺目录

`cgroup_mkdir()` 最后才 `kernfs_activate()`。若 controller 的 `css_alloc()` 或文件创建失败，
路径进入 `cgroup_destroy_locked()`，用户从未获得一个可操作的半成品目录。

---

## 9. 启用 controller：一次层级事务

执行：

```bash
echo "+memory +pids" > cgroup.subtree_control
```

不是简单设置两个 bit。controller 的有效范围会影响整棵后代子树，需要创建/隐藏 css、增删
接口文件，并更新 task 的 effective css 组合。

### 9.1 事务阶段

`cgroup_subtree_control_write()` 的骨架：

```text
解析所有 +controller / -controller
  → 锁定 live cgroup 并排空旧 offline
  → 去除重复和无效操作
  → 检查 controller availability
  → 检查 child 是否仍在下放待关闭 controller
  → 检查 domain/threaded/no-internal-process 规则
  → cgroup_save_control()
  → 修改 subtree_control
  → cgroup_apply_control()
  → cgroup_finalize_control(ret)
  → 成功后 kernfs_activate()
```

同一次写入中的多项修改要么全部成功，要么全部失败。

### 9.2 逻辑状态与物理状态

事务同时维护：

```text
逻辑状态：
  subtree_control
  subtree_ss_mask
  dom_cgrp

物理状态：
  controller css 是否存在/online
  controller 文件是否可见
  task 的 css_set 指向哪些 effective css
```

`cgroup_save_control()` 先保存逻辑快照。`cgroup_propagate_control()` 自顶向下计算 effective
mask；`cgroup_apply_control_enable()` 创建或显示新 css；之后
`cgroup_update_dfl_csses()` 才更新 task 的 css 组合。

顺序不能颠倒：若先迁移 task，再发布 css，目标组合只能退回到 ancestor 的旧 effective css。

### 9.3 失败回滚

如果中途创建某个 css 失败：

```text
cgroup_finalize_control(ret != 0)
  → cgroup_restore_control()
  → cgroup_propagate_control()
  → cgroup_apply_control_disable()
```

回滚不是“撤销每一步函数调用”，而是恢复目标逻辑状态，再让物理对象向该状态收敛。多创建的
css 进入异步销毁；不应可见的文件被隐藏。

这是内核常见的事务方式：**快照目标状态 + 分阶段应用 + 失败后重新收敛**。

---

## 10. 把进程迁入 cgroup

### 10.1 用户入口

```bash
echo 1234 > image-worker/cgroup.procs
```

`cgroup.procs` 表示进程粒度，写入任一线程的 PID 时迁移整个 thread group。
`cgroup.threads` 表示线程粒度，只能在允许的 threaded resource domain 内使用。

### 10.2 为什么迁移不能逐 task 立即修改

假设一个多线程进程有 100 个线程。如果逐个修改：

```text
线程 1 已进入新 memory cgroup
线程 2 仍在旧 cgroup
同时发生 fork / exit / controller attach 回调
```

controller 会观察到进程处于撕裂状态，fork 的 child 也可能继承不确定归属。

因此迁移是预装载后提交的事务。

### 10.3 调用骨架

```text
__cgroup_procs_write()
  → cgroup_procs_write_start()
      解析 PID、稳定 task、取得 threadgroup 排他锁
  → cgroup_attach_lock()
      锁定 cgroup 拓扑和目标
  → cgroup_attach_permissions()
      检查目标与共同祖先写权限、namespace 可达性
  → cgroup_attach_task()
      → cgroup_migrate_add_src()
      → cgroup_migrate_prepare_dst()
          查找或创建目标 css_set
      → cgroup_migrate()
          controller can_attach()
          批量移动 task 链接
          controller attach()
      → cgroup_migrate_finish()
  → cgroup_attach_unlock()
  → cgroup_procs_write_finish()
```

### 10.4 迁移的状态变化

```text
迁移前：
  task->cgroups = old_cset
  task 位于 old_cset->tasks

准备阶段：
  old/new css_set 都有事务引用
  task 暂移到 mg_tasks
  controller can_attach() 仍可拒绝

提交阶段：
  rcu_assign_pointer(task->cgroups, new_cset)
  task 链入 new_cset
  更新 populated、freezer 和反向链接状态

完成：
  controller attach() 获知迁移
  释放预装载引用和迁移链
```

`can_attach()` 失败时，task 仍属于旧 cgroup；已分配的新 `css_set` 只丢弃临时引用，不会留下
部分迁移。

### 10.5 迁移不搬走历史资源

移动进程主要改变未来记账和控制归属，不保证把所有历史资源一起迁移：

- 已分配内存通常仍记在原 memory cgroup，直到释放或特定机制重新归属；
- 已提交 I/O 可能保留原 blkcg 关联；
- CPU 时间历史统计不会从源组扣除再加到目标组。

因此 cgroup v2 文档建议“organize once and control”：工作负载启动时放到正确 cgroup，之后
主要修改限制，而不是高频来回迁移。

---

## 11. fork、clone3 与退出路径

### 11.1 普通 fork 如何继承 cgroup

进程创建穿插三个 cgroup 阶段：

```text
copy_process()
  → cgroup_fork(child)
      child 临时指向 init_css_set，初始化 task 链
  → cgroup_can_fork(kargs)
      找到或创建 child 应使用的 css_set
      controller can_fork() 逐项预检
  → 发布 task 的其他核心关系
  → cgroup_post_fork(child, kargs)
      把 child 接入目标 css_set
      controller fork() 回调
```

普通 fork 的目标来自 parent 当前 `css_set`。cgroup 身份与 task 其他关系一起在 child 可运行前
发布，因此 child 不会先在 root cgroup 运行一段时间再“补上”归属。

### 11.2 `CLONE_INTO_CGROUP`

`clone3()` 可通过 cgroup fd 指定 child 的初始目标：

```text
用户打开目标 cgroup 目录 fd
  → clone3(CLONE_INTO_CGROUP, cgroup = fd)
    → cgroup_css_set_fork()
      校验 fd、写权限、目标状态和 controller can_fork
    → child 直接发布到目标 css_set
```

它避免传统做法的竞态窗口：

```text
fork child
child 已可能运行/分配资源
parent 再写 cgroup.procs
```

### 11.3 fork 与迁移怎样互斥

`cgroup_threadgroup_rwsem` 以及可选的 per-threadgroup rwsem 协调 fork、exit 与整进程迁移。
默认策略偏向 fork/exit 热路径；`favordynmods` 可降低动态迁移和 controller 开关延迟，代价是
让 fork/exit 承担更多同步成本。

这是明确的工程权衡，而不是一个总能同时优化的开关。

### 11.4 退出与最终 task 释放

```text
cgroup_task_exit()
  → task 从正常 tasks 链转到 dying_tasks
  → controller exit()
  → 更新 populated/freezer 等状态

cgroup_task_dead()
  → 允许迭代器跳过死亡 task

cgroup_task_free()
  → 从剩余链表摘除
  → put_css_set()
```

退出时不能立刻让所有迭代器持有的 task 指针失效。`dying_tasks` 和 iterator 协议把“任务不再
运行”与“所有 cgroup 遍历都不再访问它”分开。

---

## 12. controller 如何接入 core

### 12.1 `struct cgroup_subsys`

每个 controller 提供一组生命周期和迁移回调：

```text
css_alloc / css_online / css_offline / css_free
  每 cgroup 状态的构造、发布、停用和释放

can_attach / cancel_attach / attach
  task 迁移事务

can_fork / cancel_fork / fork
  child 创建事务

exit / release
  task 退出和最终释放

dfl_cftypes
  cgroup v2 文件接口
```

controller 列表来自 `include/linux/cgroup_subsys.h`，受内核配置控制。当前源码包含 cpuset、
cpu、io、memory、devices、freezer、perf_event、hugetlb、pids、rdma、misc、dmem 等条目；
并非每个构建都会启用，也并非所有条目都以 v2 可见 controller 形式出现。

### 12.2 memory controller 示例

memory controller 的对象大致是：

```text
struct mem_cgroup
└── struct cgroup_subsys_state css
```

内存分配或 page fault 发生 charge 时，代码从 task 当前 css 找到 `mem_cgroup`，检查层级限制，
更新 per-CPU/层级统计；达到 `memory.high` 可触发回收和节流，无法满足 `memory.max` 时可能
进入 memcg OOM。

core 不理解 folio、LRU 或 reclaim。它只保证 task 与 `mem_cgroup.css` 的关联和生命周期正确。

### 12.3 CPU controller 示例

CPU controller 把 css 嵌入 `struct task_group`。调度器为层级中的 task group 建立对应调度
实体和 per-CPU `cfs_rq`，通过 `cpu.weight` 分配竞争份额，通过 `cpu.max` 实现带宽上限。

core 不决定某个 task 下一次何时运行；它只组织 task group，并在迁移/fork 时通知调度器更新
关联。

### 12.4 I/O controller 示例

I/O controller 使用 `struct blkcg`，并针对具体 block device/queue 建立 `blkcg_gq`。bio
保留 blkcg 关联，使异步下发和完成阶段仍能归属原工作负载。

这说明 controller 状态不总是一个简单计数器：它还可能按 CPU、设备或 NUMA 节点派生更多
对象。css 只提供共同的树位置和生命周期锚点。

---

## 13. 资源控制的四种基本模型

cgroup v2 不把所有资源都强行表达为 hard limit。

### 13.1 weight：竞争时按比例分配

例：`cpu.weight`。

```text
A.weight = 100
B.weight = 50

A、B 都持续 runnable 时，A 倾向获得 B 两倍的 CPU 份额。
只有 A 活跃时，A 可以使用空闲 CPU，不会因 weight 自行闲置资源。
```

weight 通常是 work-conserving 的相对控制，不是“最多使用百分之多少”。

### 13.2 limit：不能超过上限

例：`memory.max`、`pids.max`、`io.max`。

child 的 limits 总和可以超过 parent 实际资源，即允许 overcommit；真正竞争或消耗到达边界时
才执行拒绝、节流或回收。

### 13.3 protection：资源紧张时优先保护

例：`memory.low`、`memory.min`。

它们不是预留一块永远不能被别人使用的物理内存，而是在 reclaim 决策中给受保护 cgroup 更高
生存优先级。`memory.min` 语义更强，过度承诺时仍需按层级规则折算。

### 13.4 allocation：独占分配

某些资源天然需要独占集合或明确分配，例如 cpuset 对 CPU/NUMA memory node 的选择。它不像
weight 那样只在竞争时按比例共享。

### 13.5 同名“限制”的后果并不相同

```text
memory.max 到达：
  reclaim，仍失败时 memcg OOM 或 charge 失败

pids.max 到达：
  fork/clone 返回 -EAGAIN，并更新 pids.events

cpu.max 到达：
  调度器 throttle，周期补充 runtime 后继续运行

io.max 到达：
  I/O 请求被延迟，不代表 write syscall 一定同步阻塞到设备完成
```

理解 controller 时必须继续阅读它自己的资源语义，不能只看 core。

---

## 14. domain、threaded 与两条 v2 约束

### 14.1 为什么默认按进程组织

memory、pids 等 domain controller 往往面向整个进程。若同一地址空间的线程被随意分散到不同
memory cgroup，匿名页、共享页表和进程级操作的归属会变得含糊。

因此普通 v2 hierarchy 强调 process granularity：同一进程的线程属于同一 domain cgroup。

### 14.2 threaded subtree

某些 controller，如 CPU，能够支持线程粒度控制。cgroup v2 提供 threaded subtree：

```text
domain thread root
├── threaded cgroup A
└── threaded cgroup B
```

`cgroup.type` 可能显示：

- `domain`
- `domain threaded`
- `domain invalid`
- `threaded`

threaded cgroup 的 `dom_cgrp` 指向最近的 domain ancestor。domain 级资源仍归到共同 resource
domain，支持 threaded 的 controller 才在其中区分线程。

### 14.3 为什么会出现 `domain invalid`

从普通 domain 布局转为 threaded 布局是状态转换，不是给每个节点独立设置一个布尔值。
某些节点在 parent 类型改变后暂时不满足有效 domain 约束，会成为 `domain invalid`；它不能
接收进程或启用普通 domain controller，但可能继续转换为 threaded。

入门阶段不应为了“更灵活”默认使用 threaded。只有确实需要同一进程内线程级 CPU 等控制，
并理解 controller 支持边界时才使用。

---

## 15. 删除为什么不是立即释放

### 15.1 用户看到的删除

删除前 cgroup 必须满足 core 的可删除条件，例如不能仍包含不能迁出的进程。`rmdir` 成功后：

- 路径从 kernfs hierarchy 消失；
- 新用户查找不能再进入该 cgroup；
- 对象可能仍处于 dying 状态；
- controller 资源和内存可能稍后才真正释放。

`cgroup.stat` 的 `nr_dying_descendants` 正是为了观察这种状态。

### 15.2 css 四阶段销毁

当前源码把 css 销毁分为四个核心阶段：

```text
1. kill
   设置 CSS_DYING，隐藏接口文件，阻止恢复为 live

2. offline
   kill percpu_ref 在所有 CPU 可见后
   css_tryget_online() 保证失败
   调 controller css_offline()

3. release
   percpu_ref 归零
   此时只可能剩 RCU 读侧访问

4. free
   等待 RCU grace period
   调 controller css_free()
   释放 core 资源
```

其中 offline 和 free 回调可能睡眠，因此还要借助 workqueue 把原子/RCU 回调转到进程上下文。

### 15.3 为什么 parent 必须晚于 child

child css 持有 parent 关系，controller 的 `css_free()` 也可能读取 parent 状态。`online_cnt`
以及父子引用保证：

```text
child offline/free
  → 最后一个 child 放弃 parent 依赖
    → parent 才能继续 offline/free
```

若反过来，child 清理过程会解引用已经释放的 parent。

### 15.4 `rmdir` 成功不等于资源瞬间归零

历史 charge、异步 I/O、controller 私有引用或 RCU reader 都可能延长 dying 对象生命周期。
这不是泄漏的充分证据。应结合：

```bash
cat /sys/fs/cgroup/cgroup.stat
```

观察 `nr_dying_descendants` 和 `nr_dying_subsys_*` 是否最终收敛，并检查是否有长期引用来源。

---

## 16. 并发、锁、引用和 RCU

### 16.1 主要同步工具

| 机制 | 主要保护内容 |
|------|--------------|
| `cgroup_mutex` | hierarchy、controller mask、css 创建/销毁和大部分慢速事务 |
| `css_set_lock` | task/css_set 链、反向链接、populated/freezer 等高频关系 |
| `cgroup_threadgroup_rwsem` | fork/exit 与进程级迁移的排他关系 |
| `percpu_ref` | css online 快速引用、kill 后禁止新引用、最终归零 |
| 普通 `refcount_t` | `css_set` 等共享对象生命周期 |
| RCU | task 当前 css_set、children/sibling、controller css 等无锁读取 |
| kernfs active ref | 文件回调期间目录节点和关联对象稳定 |

锁、引用和 RCU 解决不同问题，不能互相替代：

```text
锁          防止状态转换交错
引用        保证持有期间对象不释放
RCU         允许读者看到旧对象，并推迟物理回收
percpu_ref  兼顾高频 get/put 与可关闭生命周期
```

### 16.2 一个具体竞态：迁移与 fork

没有排他协议时：

```text
迁移 CPU                             fork CPU
收集 thread group 的旧成员
                                     复制 parent 的旧 css_set
提交现有线程到新 cgroup
                                     发布 child 到旧 cgroup
```

结果是迁移“整个进程”后，新 child 却留在旧 cgroup。threadgroup rwsem 让 fork 与进程级迁移
形成确定顺序：要么 child 被迁移事务看见，要么 child 直接继承迁移后的归属。

### 16.3 一个具体竞态：css dying 与新引用

若销毁只检查普通引用计数：

```text
销毁 CPU                         查找 CPU
看见 refcount 接近零
                                 刚读取 css 指针，准备加引用
释放 css
                                 对已释放内存加引用
```

percpu_ref kill 先发布“禁止新 online 引用”的状态，并等待该状态对所有 CPU 可见。之后
`css_tryget_online()` 必然失败，旧引用归零后再经过 RCU 宽限期，才允许 free。

### 16.4 一个具体竞态：DYING 与 populated 归零

css 销毁可能等待其 task/populated 状态清空，而最后一个 task 离开也可能并发发生。源码用
`smp_mb()` 配对，保证至少一方观察到另一方：

```text
kill 路径                         populated 更新路径
发布 CSS_DYING                    递减到零
memory barrier                    memory barrier
检查 populated                    检查 CSS_DYING

至少一侧负责触发 kill_css_finish()
```

否则双方都可能认为“对方稍后会处理”，css 永久停在 dying。

### 16.5 锁顺序

涉及多把锁的主要慢路径遵循固定顺序，例如：

```text
cgroup_mutex
  → cgroup_threadgroup_rwsem
    → css_set_lock
```

具体 helper 可能因 `favordynmods` 和操作类型选择不同锁模式。阅读源码时应从
`cgroup_attach_lock()`、`cgroup_procs_write_start()` 等统一入口理解顺序，不要在 controller
中自行拼装锁协议。

---

## 17. namespace、delegation 与容器

### 17.1 cgroup namespace 改变路径视图

cgroup namespace 可以让某个子树在 namespace 内显示为 `/`。它改变进程看到的 cgroup 路径，
并不复制一棵新 hierarchy，也不会消除外部 ancestor 已施加的限制。

```text
宿主机： /tenant-A/container-1
容器内： /
```

容器内即使看不见 `/tenant-A`，仍无法突破其 `memory.max` 或 `cpu.max`。

### 17.2 delegation 的目标

上级管理者把某个 cgroup 子树委托给低权限管理者后，后者应能：

- 在子树中创建/删除 child；
- 在子树内部迁移自己的进程；
- 把上级提供的 controller 继续分配给 descendants；
- 不能拉入子树外的进程，也不能把进程推出委托边界；
- 不能修改由 parent 控制当前节点资源的文件。

### 17.3 为什么不能简单 `chown -R`

当前目录中的 `memory.max`、`cpu.max` 等文件表达 parent 对该 cgroup 的限制。若被委托者可写，
就能放宽上级施加的约束。

委托通常只授权目录管理和 core 委托文件，例如：

```text
cgroup.procs
cgroup.threads
cgroup.subtree_control
```

实际允许集合可查看：

```bash
cat /sys/kernel/cgroup/delegate
```

systemd 系统上优先使用 systemd 的 delegation/transient unit 接口，不要和 PID 1 同时直接修改
其管理的层级。

### 17.4 迁移的共同祖先权限

低权限写入目标 `cgroup.procs` 时，不仅需要目标文件可写，还需要源与目标共同祖先的
`cgroup.procs` 写权限。这条规则保证被委托者只能在自己的子树内重排任务，不能跨越委托边界。

---

## 18. 可运行的观察实验

> 以下实验要求 cgroup v2，并通常需要 root。systemd 管理的生产机器不应直接修改顶层；
> 建议在测试 VM、容器委托子树或临时 systemd scope 中执行。

### 18.1 确认环境

```bash
findmnt -t cgroup2
cat /proc/self/cgroup
cat /sys/fs/cgroup/cgroup.controllers
```

观察：

- 是否存在 cgroup2 挂载；
- 当前 shell 位于哪个路径；
- 内核配置、v1 占用和 parent 下放后，哪些 controller 当前可用。

### 18.2 创建一个最小实验 cgroup

以下命令以已获得写权限的 cgroup v2 子树为 `BASE`。在测试 VM 的 root hierarchy 中可设置：

```bash
BASE=/sys/fs/cgroup
CG="$BASE/cgroup-learning"

sudo mkdir "$CG"
cat "$CG/cgroup.controllers"
cat "$CG/cgroup.events"
```

如果 `memory.max`、`pids.max` 不存在，需要在 parent 的
`cgroup.subtree_control` 中启用对应 controller：

```bash
printf '+memory +pids\n' |
	sudo tee "$BASE/cgroup.subtree_control"
```

若返回 `EBUSY`，优先检查 no-internal-process constraint 和 systemd 管理边界，不要反复强写。

### 18.3 设置限制并迁移一个任务

```bash
echo 128M | sudo tee "$CG/memory.max"
echo 16   | sudo tee "$CG/pids.max"

sleep 600 &
worker=$!
echo "$worker" | sudo tee "$CG/cgroup.procs"

cat "$CG/cgroup.procs"
cat "/proc/$worker/cgroup"
cat "$CG/memory.current"
cat "$CG/pids.current"
```

预期：

- `worker` 出现在 `cgroup.procs`；
- `/proc/$worker/cgroup` 路径指向实验组；
- 当前内存和 task 计数反映该任务；
- 未来 fork 的 child 自动继承该 cgroup。

这条路径对应 `__cgroup_procs_write()` → `cgroup_attach_task()` → `cgroup_migrate()`。

### 18.4 观察 populated 事件

另开终端：

```bash
inotifywait -m "$CG/cgroup.events"
```

然后结束并回收 worker：

```bash
kill "$worker"
wait "$worker"
cat "$CG/cgroup.events"
```

当 cgroup 及其 descendants 不再有 live process，`populated` 从 1 变为 0，并触发文件修改通知。
这比循环读取 `cgroup.procs` 更适合服务管理器判断整个子树是否清空。

### 18.5 观察压力而不只看用量

```bash
cat "$CG/cpu.pressure"
cat "$CG/memory.pressure"
cat "$CG/io.pressure"
```

用量回答“消耗了多少”，PSI 回答“任务因资源不足停顿了多久”。高 `memory.current` 不一定代表
应用正在受压；显著的 `memory.pressure some/full` 才说明 reclaim 或内存竞争影响了执行。

### 18.6 清理并观察 dying

确保没有 task 后：

```bash
sudo rmdir "$CG"
cat "$BASE/cgroup.stat"
```

`rmdir` 后路径立即消失；`nr_dying_descendants` 可能短暂非零，随后随 css 引用和 RCU 回收收敛。

### 18.7 tracepoint

当前内核配置启用相应 tracepoint 时：

```bash
sudo trace-cmd record \
	-e cgroup:cgroup_mkdir \
	-e cgroup:cgroup_attach_task \
	-e cgroup:cgroup_rmdir \
	sleep 10
sudo trace-cmd report
```

在记录窗口内执行创建、迁移和删除，可把用户命令与 core 生命周期事件对齐。若 tracepoint 名称
因配置或版本不可用，先查看：

```bash
find /sys/kernel/tracing/events/cgroup -maxdepth 2 -type f -name enable
```

---

## 19. 常见误区与故障排查

### 19.1 “写了 `memory.max`，进程内存应该立刻降下来”

限制改变后，memory controller 可能触发 reclaim 或阻塞新 charge，但并不承诺同步把 usage
降到新值。检查：

```bash
cat memory.current
cat memory.events
cat memory.stat
cat memory.pressure
```

区分 usage、high 事件、max 事件、OOM 和 reclaim 压力。

### 19.2 “`cpu.weight=50` 就是最多使用 50% CPU”

错误。weight 是 sibling 竞争时的相对份额。硬带宽上限使用 `cpu.max`，例如：

```bash
echo "50000 100000" > cpu.max
```

表示每 100 ms 周期最多使用 50 ms CPU runtime；在多核和具体调度配置下还需结合
`cpu.stat` 判断节流。

### 19.3 “进程移走后，原 cgroup 的 memory.current 应立即为零”

迁移不自动搬走所有历史 memory charge。内存页可能继续归属原 memcg，直到释放或重新归属。
这也是不应使用高频迁移模拟资源策略切换的原因。

### 19.4 “目录为空，为什么 `rmdir` 仍失败”

依次检查：

```bash
cat cgroup.procs
cat cgroup.threads
cat cgroup.events
find . -mindepth 1 -maxdepth 1 -type d
```

可能原因包括仍有 child cgroup、仍有 task、threaded/domain 约束、挂载或委托权限，以及内核
认为节点已死亡或正被其他操作锁定。

### 19.5 “为什么看不到某个 controller 文件”

检查路径：

```text
内核是否编译该 controller？
  ↓
是否被 cgroup_disable= 或 cgroup_no_v1= 等启动参数影响？
  ↓
是否仍绑定在 v1 hierarchy？
  ↓
parent 的 cgroup.controllers 是否列出？
  ↓
parent 是否写入 +controller 到 subtree_control？
  ↓
当前节点是否满足 domain/threaded 和 no-internal-process 规则？
```

不要只检查当前目录的 `cgroup.subtree_control`。

### 19.6 “可以直接管理 systemd 创建的 cgroup 目录吗”

技术上某些写入可能成功，但会和 systemd 的 unit 生命周期、controller enablement 和进程迁移
冲突。应使用 `systemd-run`、unit 的 `MemoryMax=`/`CPUWeight=`/`TasksMax=` 或 delegation
机制，让单一管理者维护拓扑。

### 19.7 “删掉目录后 `nr_dying_descendants` 长时间不归零”

短暂非零是正常异步销毁。长期不归零时再检查：

- controller 是否持有 css 引用；
- 是否有未完成 I/O、writeback 或 BPF 关联；
- workqueue 是否阻塞；
- RCU stall 或 CPU hotplug 是否影响回收；
- controller 的 offline/free 回调是否等待外部条件。

---

## 20. 回到开头案例

现在可以把 `image-worker` 的用户操作完整映射到内核状态：

```text
mkdir image-worker
  → 分配 cgroup/self css/ancestors
  → 创建未激活 kernfs 目录
  → 创建 controller css 和接口文件
  → 最后 activate

写 memory.max / cpu.weight / pids.max
  → kernfs 定位 cftype 和 css
  → controller 校验并更新私有层级状态

写 PID 到 cgroup.procs
  → 稳定 thread group
  → 预建目标 css_set
  → 所有 controller can_attach
  → 一次提交 task->cgroups

worker fork
  → can_fork 确认限制和目标 css_set
  → child 在首次运行前发布到同一 cgroup

读取 *.pressure / *.stat
  → controller 刷新或聚合 per-CPU 层级统计
  → 用户区分用量、节流、回收和真实 stall

服务退出
  → task 进入 dying_tasks，再最终 put css_set
  → cgroup.events populated 变为 0

rmdir
  → 用户路径立即消失
  → css 依次 DYING、offline、引用归零、经过 RCU 后 free
```

由此可以得到最重要的入门模型：

> cgroup 不是一组散落的限额文件，而是一棵任务组织树；core 用 cgroup、css 和 css_set
> 维护层级与生命周期，controller 把具体资源语义挂到这棵树上，kernfs 再把这些状态暴露为
> 可组合、可委托、可观察的用户接口。

---

## 21. 源码阅读路线与关键函数索引

### 21.1 推荐阅读顺序

第一轮只建立对象和用户 ABI：

```text
Documentation/admin-guide/cgroup-v2.rst
  → include/linux/cgroup-defs.h
  → kernel/cgroup/cgroup.c 中 cgroup_base_files[]
```

第二轮跟踪主路径：

```text
cgroup_mkdir()
cgroup_subtree_control_write()
__cgroup_procs_write()
cgroup_fork()/cgroup_post_fork()
cgroup_destroy_locked()
```

第三轮选择一个 controller：

```text
memory：mm/memcontrol.c
CPU：   kernel/sched/core.c、kernel/sched/fair.c、kernel/sched/sched.h
I/O：   block/blk-cgroup.c
pids：  kernel/cgroup/pids.c
cpuset：kernel/cgroup/cpuset.c
```

### 21.2 core 函数索引

| 函数 | 阅读目标 |
|------|----------|
| `cgroup_init_early()` | fork 等早期路径使用前必须具备的最小状态 |
| `cgroup_init()` | root、controller、core 文件的正式初始化 |
| `cgroup_setup_root()` | hierarchy 与 kernfs root 的构造发布 |
| `cgroup_mkdir()` | 用户 mkdir 的事务入口 |
| `cgroup_create()` | cgroup 对象构造和提交点 |
| `css_create()` | controller 状态的分配、ID、online 与失败释放 |
| `online_css()` / `offline_css()` | controller 状态发布与停用 |
| `cgroup_subtree_control_write()` | v2 controller enable/disable 事务 |
| `cgroup_apply_control()` | mask 传播、css 创建与 task effective css 更新 |
| `__cgroup_procs_write()` | PID/TID 迁移入口 |
| `cgroup_attach_task()` | source/destination css_set 预装载 |
| `cgroup_migrate()` | controller 预检和 task 迁移提交 |
| `cgroup_fork()` | child cgroup 字段的最早初始化 |
| `cgroup_css_set_fork()` | fork 目标 css_set 与 `CLONE_INTO_CGROUP` |
| `cgroup_post_fork()` | child cgroup 身份最终发布 |
| `cgroup_task_exit()` | task 从 live 成员状态退出 |
| `cgroup_destroy_locked()` | cgroup 摘除和 css kill 起点 |
| `kill_css_sync()` | 发布 DYING、隐藏文件和启动销毁 |
| `css_killed_work_fn()` | percpu_ref killed 后执行 offline |
| `css_release()` | 引用归零后进入 RCU 回收 |
| `css_free_rwork_fn()` | 宽限期后 controller/core 最终释放 |

### 21.3 核心结构索引

| 结构 | 位置 | 作用 |
|------|------|------|
| `struct cgroup_root` | `include/linux/cgroup-defs.h` | hierarchy |
| `struct cgroup` | `include/linux/cgroup-defs.h` | core 树节点 |
| `struct cgroup_subsys_state` | `include/linux/cgroup-defs.h` | controller 状态基类 |
| `struct css_set` | `include/linux/cgroup-defs.h` | task 的 controller 状态组合 |
| `struct cgroup_subsys` | `include/linux/cgroup-defs.h` | controller 回调表 |
| `struct cftype` | `include/linux/cgroup-defs.h` | cgroup 文件接口描述 |
| `struct cgroup_taskset` | `include/linux/cgroup-defs.h` | controller 观察迁移 task 集合 |

### 21.4 继续学习时应回答的问题

读完一个 controller 后，尝试回答：

1. 它的私有对象怎样嵌入 css？
2. 哪个热路径取得 task 当前 css？
3. 统计是立即向 parent 传播，还是 per-CPU 延迟聚合？
4. 达到限制时是拒绝、回收、节流还是只记录事件？
5. task 迁移时，历史资源是否一起迁移？
6. `css_offline()` 后还有哪些对象可能引用它？
7. 哪些接口是层级语义，哪些只统计 local 状态？

如果这些问题能用源码回答，就不再只是“会使用 cgroup 文件”，而是已经建立了 controller
实现模型。

---

## 22. 快速参考

### 22.1 一句话对象关系

```text
cgroup_root 包含一棵 cgroup 树；
每个 controller 为节点创建 css；
css_set 把一组 effective css 组合起来；
task_struct 通过 css_set 获得资源身份；
kernfs/cftype 把对象操作暴露为文件。
```

### 22.2 一句话主路径

```text
创建：构造 cgroup/css/files → 最后 activate
配置：保存 mask → 传播 → 创建/隐藏 css → 更新 task → 提交或收敛回滚
迁移：稳定 thread group → 预建 css_set → controller 预检 → 一次提交
fork：预选 css_set → child 可运行前发布
删除：路径摘除 → DYING → offline → ref zero → RCU → free
```

### 22.3 v1 与 v2 的入门对比

| 项目 | cgroup v1 | cgroup v2 |
|------|-----------|-----------|
| hierarchy | controller 可组成多棵 hierarchy | 单一统一 hierarchy |
| 任务组织 | 不同 controller 可看到不同树 | controller 共享同一组织树 |
| controller 下放 | legacy mount/bind 模型 | `cgroup.subtree_control` |
| 内部进程 | 允许，资源竞争模型容易含糊 | domain controller 遵守 no-internal-process |
| 线程粒度 | 多种历史接口 | 显式 threaded subtree |
| 推荐用途 | 兼容旧系统 | 新设计与新部署优先 |

### 22.4 最重要的三个边界

```text
目录消失 != css 已释放
task 迁移 != 历史资源全部迁移
controller available != 已经下放给 child
```

### 22.5 参考资料

- `Documentation/admin-guide/cgroup-v2.rst`：当前源码的 v2 用户 ABI 权威说明；
- `Documentation/admin-guide/cgroup-v1/`：v1 历史接口；
- `Documentation/accounting/psi.rst`：压力统计；
- `doc/08 linux-process-management-internals.md`：fork/exit 与 task 生命周期；
- `doc/09 linux-memory-management-internals.md`：memcg、回收和 OOM；
- `doc/11 linux-scheduler-internals.md`：CPU controller 与调度层级；
- `doc/12 linux-block-io-internals.md`：blk-cgroup 与 I/O 控制。
