# Linux 进程管理内部实现：从 shell 启动命令到进程回收

> 适用内核版本：v7.2-rc1（基于当前仓库 `a14c021eef14`）
> 核心源码：`kernel/fork.c`、`fs/exec.c`、`kernel/exit.c`、`kernel/pid.c`、`kernel/signal.c`、`kernel/sched/core.c`

---

## 目录

1. [从一个常见命令开始](#1-从一个常见命令开始)
2. [背景与需要解决的问题](#2-背景与需要解决的问题)
3. [总体方案及其优劣](#3-总体方案及其优劣)
4. [宏观地图与案例总流程](#4-宏观地图与案例总流程)
5. [task_struct 不是进程的全部](#5-task_struct-不是进程的全部)
6. [PID、TID 与线程组](#6-pidtid-与线程组)
7. [创建入口：fork、vfork、clone 和 clone3](#7-创建入口forkvforkclone-和-clone3)
8. [copy_process：先构造，后发布](#8-copy_process先构造后发布)
9. [资源究竟是复制还是共享](#9-资源究竟是复制还是共享)
10. [写时复制如何让 fork 足够快](#10-写时复制如何让-fork-足够快)
11. [子进程为什么从 fork 返回 0](#11-子进程为什么从-fork-返回-0)
12. [新任务如何进入调度器](#12-新任务如何进入调度器)
13. [execve：在原 task 上替换程序](#13-execve在原-task-上替换程序)
14. [多线程 exec 为什么复杂](#14-多线程-exec-为什么复杂)
15. [exit：释放资源但保留死亡信息](#15-exit释放资源但保留死亡信息)
16. [wait：父进程如何取得结果并收尸](#16-wait父进程如何取得结果并收尸)
17. [孤儿进程、subreaper 与 PID namespace](#17-孤儿进程subreaper-与-pid-namespace)
18. [信号如何贯穿生命周期](#18-信号如何贯穿生命周期)
19. [并发、锁、引用计数与 RCU](#19-并发锁引用计数与-rcu)
20. [线程、vfork、pidfd 与容器分支](#20-线程vforkpidfd-与容器分支)
21. [失败路径与方案优劣](#21-失败路径与方案优劣)
22. [用实验验证完整过程](#22-用实验验证完整过程)
23. [回到案例与源码索引](#23-回到案例与源码索引)

---

## 1. 从一个常见命令开始

在交互式 shell 中执行：

```bash
/bin/echo hello
```

表面上只有一行输出，背后通常经历：

1. shell 创建一个子进程；
2. 子进程把自己的程序替换成 `/bin/echo`；
3. 新程序运行并向标准输出写入 `hello`；
4. 子进程退出；
5. shell 收到通知，读取退出状态并回收子进程；
6. shell 再次显示提示符。

可以用一个简化的 C 程序表达 shell 的核心逻辑：

```c
pid_t pid = fork();

if (pid == 0) {
    char *argv[] = { "/bin/echo", "hello", NULL };
    execve(argv[0], argv, environ);
    _exit(127);                    /* execve 失败才会执行 */
}

int status;
waitpid(pid, &status, 0);
```

本文将一直跟踪这个例子。每个数据结构和函数都要回答一个具体问题：

- `fork()` 为什么没有复制所有物理内存？
- 父子为什么能从同一个调用得到不同返回值？
- 子进程在完全初始化之前为什么不会被其他 CPU 运行？
- `execve()` 为什么成功后不返回，PID 却没有改变？
- `_exit()` 为什么不能直接释放当前内核栈？
- 子进程已经退出，为什么还需要 zombie？
- 父进程怎样避免等待到 PID 复用后的另一个进程？

---

## 2. 背景与需要解决的问题

### 2.1 历史背景：程序、进程和线程逐渐分离

早期批处理系统可以把“正在运行的程序”和“一块固定内存”近似看成同一对象。分时、多用户、
虚拟内存和多核出现后，这个模型不再成立：同一程序可以被多个用户同时运行；一个进程可以有
多条并发执行流；共享库和文件页应被多个进程复用；进程还要被暂停、调试、迁移和隔离。

Unix 又形成了 `fork + exec` 的组合语义：

```text
fork：保留当前执行环境，派生一个几乎相同的 child
exec：保留进程身份，在原位置替换程序映像
wait：父进程取得 child 的最终结果
```

这套接口简洁而强大：shell 可在 fork 与 exec 之间重定向 fd、修改环境和凭证，再启动任意程序。
但它要求内核同时提供高效复制、灵活共享、身份稳定和延迟回收。

### 2.2 需要解决的具体问题

进程管理至少要同时满足：

- **执行隔离**：一个进程不能随意读写另一个进程的地址空间和寄存器；
- **快速派生**：fork 后立即 exec 的常见路径不能复制全部用户内存；
- **线程共享**：同一进程的线程共享资源，同时仍能独立调度和阻塞；
- **稳定身份**：信号、wait、ptrace、procfs 和 pidfd 能定位正确对象；
- **关系语义**：维护父子、线程组、进程组、会话和 namespace 层级；
- **退出可观测**：child 退出后，parent 仍能取得退出码和资源统计；
- **并发安全**：其他 CPU 查找 task 时，创建和退出不能暴露半初始化或已释放对象。

这些目标之间存在冲突：复制越彻底，隔离越直观但 fork 越慢；共享越多，线程越高效但生命周期
越复杂；退出越早释放越省资源，但父进程越可能丢失死亡信息。

### 2.3 最直观方案为什么不够

最容易想到的实现，是为每个进程分配一个大对象，其中包含：

```text
寄存器 + 地址空间 + 文件表 + 信号 + 权限 + PID + 父子关系 + 调度状态
```

`fork()` 时完整复制这个对象和所有内存，`exit()` 时全部释放。

它的优点是概念简单：进程拥有全部资源，复制和销毁边界清晰。但它无法满足真实系统：

- 一个占用数 GB 内存的进程，fork 后通常立即 exec；完整复制绝大部分是浪费；
- 线程需要共享地址空间和文件表，却要独立调度和接收部分信号；
- 文件、凭证、namespace 等对象的复制成本和共享语义不同；
- 进程退出时父进程仍需读取退出码和统计，不能立即销毁所有身份；
- `/proc`、ptrace、pidfd 和信号发送者可能正在并发查找 task。

---

## 3. 总体方案及其优劣

### 3.1 Linux 的技术方案

Linux 把问题拆成四层：

```text
执行流：task_struct + 内核栈 + 寄存器现场
资源对象：mm/files/fs/cred/nsproxy/sighand/signal...
身份关系：struct pid + 父子/线程组/进程组/会话
生命周期：构造 → 发布 → 运行 → 退出 → zombie → reap → RCU 释放
```

创建时，由 clone flags 决定每类资源是复制还是共享；昂贵用户内存使用 COW；退出时先撤销
运行能力并释放大资源，再保留最小死亡记录等待父进程。

### 3.2 方案的优点

- fork 快，线程也能复用同一创建框架；
- 不同资源可以独立共享，支持 pthread、容器和内核 worker；
- task 是统一调度单位，调度器无需理解“进程还是线程”；
- PID 身份可独立持有，pidfd 能抵抗数字复用；
- 退出与回收分离，POSIX wait 语义自然实现。

### 3.3 方案的代价和边界

- `task_struct` 只是中心索引，生命周期分散在很多引用对象中；
- clone flags 有组合约束，错误组合必须拒绝；
- fork、exec、exit 的失败回滚和并发锁顺序复杂；
- “已经退出”“已经从 PID 索引消失”“内存已经释放”是不同时间点；
- task 查找同时依赖锁、引用计数和 RCU，不能只保存裸指针或整数 PID。

### 3.4 与简单方案的对比

| 设计问题 | 简单方案 | Linux 方案 | Linux 为此付出的代价 |
|----------|----------|------------|----------------------|
| fork 内存 | 复制全部页 | VMA/页表复制，物理页 COW | 写 fault、页表成本、TLB 同步 |
| 线程实现 | 单独线程对象体系 | 统一 task + clone flags | flag 组合和共享生命周期复杂 |
| PID 身份 | 保存整数 | `struct pid`/pidfd | 引用、namespace 层级和索引维护 |
| 退出 | 立即销毁 | 先释放资源，保留 zombie | 需要 wait/reparent/autoreap |
| 并发查找 | 一把全局锁 | tasklist lock + refcount + RCU | 正确使用协议的门槛更高 |

### 3.5 Linux 的统一任务模型

#### 内核调度的是 task

Linux 中每条可调度执行流都是一个 `task_struct`。所谓“进程”和“线程”主要区别在资源共享：

```
单线程进程：
  一个 task + 一套独立资源对象

多线程进程：
  多个 task
  ├── 各自 TID、内核栈、寄存器、调度实体、signal mask
  └── 共享 mm、files、fs、sighand、signal 等
```

因此 pthread 创建与 fork 并不是完全独立的内核机制。二者最终都进入 `kernel_clone()` 和
`copy_process()`；区别是 pthread 设置更多 `CLONE_*` 共享标志。

#### 为什么统一模型优于两套实现

若“进程”和“线程”使用两套执行对象，调度、信号、ptrace、CPU affinity、cgroup 和退出统计
都要处理两种实体。统一 task 模型让这些子系统只面对一种调度单位。

代价是用户术语容易混淆：

- 内核 `pid` 字段通常是线程 ID；
- `tgid` 才是用户通常所说的进程 PID；
- `/proc/<tgid>/task/<tid>` 展开线程；
- `kill(pid, sig)` 通常面向线程组，`tgkill(tgid, tid, sig)` 精确面向线程。

---

## 4. 宏观地图与案例总流程

### 4.1 子系统宏观地图

在进入 `copy_process()` 等细节之前，先确定各子系统边界：

```
用户接口
fork/clone3       execve            exit             wait/pidfd
     │               │                │                 │
     ▼               ▼                ▼                 ▼
┌──────────┐    ┌──────────┐    ┌──────────┐      ┌──────────┐
│ fork.c   │    │ exec.c   │    │ exit.c   │      │ exit.c  │
│ 构造task │    │ 替换映像 │    │ 释放资源 │      │ 消费状态 │
└────┬─────┘    └────┬─────┘    └────┬─────┘      └────┬─────┘
     │               │                │                 │
     ├──── PID/父子/线程组 ────────────┴─────────────────┤
     │         kernel/pid.c、signal.c、tasklist_lock     │
     │                                                   │
     ├──── 资源对象 ─────────────────────────────────────┤
     │   mm、files、fs、cred、nsproxy、signal/sighand   │
     │                                                   │
     └──── 调度器 ───────────────────────────────────────┘
         sched_fork → wake_up_new_task → do_task_dead
```

宏观上有三条相互正交的主线：

```text
执行线：  构造寄存器/栈 → runnable → 运行 → 永久切走
资源线：  复制或共享 → exec 替换部分资源 → exit 减引用/销毁
身份线：  分配 PID/建立关系 → 保留 zombie → wait 后摘除 → RCU 释放
```

后文的每个函数都可以放回这三条线。这样即使进入某个子系统的慢速路径，也不会失去整体位置。

### 4.2 贯穿案例的总体过程

先看完整骨架，后续逐段证明：

```
shell task（父）
  │
  │ fork()
  ▼
kernel_clone()
  → copy_process()
      分配 child task/stack
      复制或共享资源
      建立 PID、父子关系
      构造 child 寄存器现场
      发布到任务/PID 链表
  → wake_up_new_task(child)
  │
  ├── 父：fork 返回 child PID → waitpid() 睡眠
  │
  └── 子：首次被调度 → fork 返回 0
         → execve("/bin/echo")
             建立 linux_binprm
             识别 ELF
             杀掉同组其他线程（若有）
             提交新 mm/cred
             构造用户栈与入口寄存器
         → 返回用户态从 ELF entry 开始
         → main/exit_group
         → do_exit()
             释放 mm/files/fs 等
             保存 exit_code
             通知父进程和 pidfd
             EXIT_ZOMBIE
             do_task_dead()
  │
  └── 父被唤醒
      → do_wait()
      → wait_task_zombie()
      → 复制 exit status
      → release_task()
      → task/PID 引用归零后延迟释放
```

这条主线包含三个发布边界：

1. `wake_up_new_task()`：新 task 开始可能执行；
2. `begin_new_exec()` 附近：exec 从可回滚准备进入不可回滚提交；
3. `exit_notify()`/zombie：任务失去执行能力，但死亡记录仍可被 wait 查找。

---

## 5. `task_struct` 不是进程的全部

### 5.1 按案例需要理解字段

`include/linux/sched.h` 中的 `task_struct` 很大，但本例主要使用：

```
task_struct
├── stack / thread               内核栈与体系结构寄存器现场
├── __state / on_rq / on_cpu     睡眠、排队和执行状态
├── sched_class / se/rt/dl       调度实体
├── pid / tgid / thread_pid      数字缓存和稳定 PID 对象
├── real_parent / parent         自然父进程与 SIGCHLD/wait 关系
├── children / sibling           父子链表
├── group_leader / thread_node   线程组关系
├── mm / active_mm               用户地址空间
├── files / fs                   fdtable 与 cwd/root/umask
├── cred / real_cred             权限凭证
├── nsproxy                      namespace 集合
├── signal / sighand             线程组状态与信号动作
├── pending / blocked            线程私有信号状态
├── exit_state / exit_code       zombie/dead 与 wait 结果
└── usage / rcu_users            生命周期引用
```

### 5.2 为什么资源使用独立对象

考虑 `files_struct`：fork 需要复制 fdtable，但每个槽位仍指向同一 `struct file`；pthread 则要
共享整张 fdtable，使一个线程 close 立即影响其他线程。若 fdtable 内嵌在 task 中，这两种语义
都要特殊处理。

独立对象配合引用计数后：

```text
fork：    新 files_struct + 每个 file 增加引用
pthread：files_struct.count++，共享同一张表
exit：    put_files_struct()，最后引用关闭全部 fd
```

同样的设计应用于 `mm_struct`、`fs_struct`、`sighand_struct`、`cred` 和 `nsproxy`。

### 5.3 task 和内核栈

每个 task 有独立内核栈。系统调用、中断或异常代表该 task 执行内核代码时，使用它自己的栈。
上下文切换保存少量体系结构现场并切换栈指针，而不是复制整个 `task_struct`。

这也解释了退出时为何不能立即释放当前栈：`do_exit()` 正运行在要释放的栈上，必须先永久切换
到其他 task，之后由其他上下文完成最终释放。

---

## 6. PID、TID 与线程组

### 6.1 一个三线程进程

```
thread-group TGID=1000
├── leader: pid=1000, tgid=1000, group_leader=self
├── worker: pid=1001, tgid=1000, group_leader=leader
└── worker: pid=1002, tgid=1000, group_leader=leader
```

`getpid()` 返回 TGID，`gettid()` 返回当前 task 的 PID/TID。线程组 leader 的 TID 等于 TGID。

### 6.2 为什么不能只用整数 PID

进程退出并被 wait 回收后，数字 PID 会重新分配。如果内核子系统或用户程序长期保存整数：

```
保存 pid=1234
旧进程退出并释放 PID
新进程获得 pid=1234
向 1234 发信号 → 误伤新进程
```

内核用带引用计数的 `struct pid` 表示稳定身份：

```
struct pid
├── count
├── level
├── tasks[PIDTYPE_MAX]
├── wait_pidfd
└── numbers[]：每层 PID namespace 的 (nr, ns)
```

`PIDTYPE_PID/TGID/PGID/SID` 分别连接线程、线程组、进程组和会话。`get_pid()` 保持身份对象；
`get_pid_task()` 再取得当前关联 task 引用。pidfd 把这种稳定身份暴露给用户空间。

### 6.3 PID namespace

同一个 `struct pid` 的 `numbers[]` 保存从内层到外层的数字：

```text
宿主 PID namespace：PID 24831
容器 PID namespace：PID 37
更内层 namespace：  PID 1
```

`pid_vnr()` 返回调用者视图中的数字。新 PID namespace 的第一个进程成为 PID 1/child reaper，
承担孤儿回收并具有特殊信号语义。

---

## 7. 创建入口：fork、vfork、clone 和 clone3

### 7.1 四个接口汇聚到同一路径

```
fork()   → flags = SIGCHLD
vfork()  → CLONE_VFORK | CLONE_VM | SIGCHLD
clone()  → 旧 ABI 参数转换
clone3() → struct clone_args 校验与转换
              │
              ▼
       struct kernel_clone_args
              │
              ▼
         kernel_clone()
              │
              ▼
         copy_process()
```

`clone3()` 使用有 size 的可扩展结构，能清晰表达 pidfd、set_tid、cgroup fd 等字段；旧 `clone()`
参数顺序依体系结构变化，扩展困难。

### 7.2 clone flags 是资源共享协议

关键组合：

| 标志 | 含义 |
|------|------|
| `CLONE_VM` | 共享 `mm_struct` |
| `CLONE_FILES` | 共享 fdtable |
| `CLONE_FS` | 共享 cwd/root/umask |
| `CLONE_SIGHAND` | 共享信号 disposition |
| `CLONE_THREAD` | 加入同一线程组，共享 `signal_struct` |
| `CLONE_VFORK` | 父等待子 exec/exit |
| `CLONE_NEW*` | 创建相应 namespace |
| `CLONE_PIDFD` | 返回引用新进程的 pidfd |
| `CLONE_CHILD_CLEARTID` | 线程退出时清 TID 并 futex wake |

并非任意组合都合法。例如共享 sighand 而不共享 VM 会让信号 handler 地址在两个地址空间含义
不同，因此 `CLONE_SIGHAND` 要求 `CLONE_VM`；`CLONE_THREAD` 又要求 `CLONE_SIGHAND`。

### 7.3 `kernel_clone()` 的职责

它不负责逐项复制资源，而是处理创建操作外围语义：

1. 规范和校验参数；
2. 选择 ptrace fork/clone/vfork 事件；
3. 调用 `copy_process()` 构造未运行 task；
4. 在唤醒前上报 `sched_process_fork` 并取得 PID 引用；
5. 写回 parent_tid/pidfd；
6. 建立 vfork completion；
7. `wake_up_new_task()`；
8. vfork 父进程等待 completion；
9. 上报 ptrace 事件并返回虚拟 PID。

唤醒前完成对裸 child 指针的关键访问，因为唤醒后子进程可在另一 CPU 立即退出。

当前源码把这个边界写得很直接：

```c
trace_sched_process_fork(current, p);

pid = get_task_pid(p, PIDTYPE_PID);
nr = pid_vnr(pid);

/* ... parent_tid、vfork、MGLRU 等唤醒前准备 ... */

wake_up_new_task(p);

if (unlikely(trace))
    ptrace_event_pid(trace, pid);
```

这里 ptrace 在唤醒后使用的是已持有引用的 `struct pid`，而不是假定裸 `p` 仍然有效。这段代码
同时证明了“trace fork 事件发生在可运行之前”和“发布后必须依靠稳定引用”两个结论。

---

## 8. `copy_process()`：先构造，后发布

### 8.1 为什么不能边复制边让别人看到

若 task 刚分配就进入全局 PID/任务表：

```text
CPU0 正在 copy_mm，p->mm 尚未初始化
CPU1 根据 PID 找到 p，发送信号或读取 /proc/pid/maps
→ 观察半初始化对象
```

所以创建采用事务式结构：

```text
私有构造阶段 → 一次性发布关系 → 最后交给调度器
```

### 8.2 构造阶段

按当前源码主线简化：

```c
p = dup_task_struct(current, node);
copy_creds(p, clone_flags);
sched_fork(clone_flags, p);
copy_files(clone_flags, p, args->fd_range);
copy_fs(clone_flags, p);
copy_sighand(clone_flags, p);
copy_signal(clone_flags, p);
copy_mm(clone_flags, p);
copy_namespaces(clone_flags, p);
copy_io(clone_flags, p);
pid = alloc_pid(...);
copy_thread(p, args);
```

真实函数还穿插 seccomp、audit、perf、cgroup、futex、rseq、uprobe、LSM、NUMA policy 等。
每一步都可能失败，因此顺序同时决定回滚顺序。

### 8.3 发布阶段

在持有 `tasklist_lock` 和相关信号锁时，内核：

- 设置 `real_parent/parent`；
- 把 child 接入父进程 `children`；
- 建立 group leader/thread group 关系；
- `attach_pid()` 接入 PIDTYPE 链表；
- 加入全局任务/线程遍历结构；
- 更新进程和线程计数。

从这一点起，PID lookup、`/proc`、信号和 ptrace 可以发现 child。但它仍未 runnable，直到
`kernel_clone()` 调用 `wake_up_new_task()`。

### 8.4 三种“可见性”不要混淆

```text
对象已分配：copy_process 内部可用，外界不可安全查找
关系已发布：PID/父子遍历可发现，但 task 尚未运行
已唤醒：     task 在 runqueue，可与父进程真正并发
```

这种分阶段发布减少了必须在大锁中完成的工作，同时保持观察者看不到半初始化状态。

---

## 9. 资源究竟是复制还是共享

### 9.1 `copy_mm()`

```text
内核线程或特殊 worker：按专门语义处理 mm
CLONE_VM：mmget(oldmm)，父子共享地址空间
普通 fork：dup_mm() 创建新 mm，并 dup_mmap() 复制 VMA/页表
```

普通 fork 的 `mm_struct` 不共享，所以之后 `mmap()` 或 `brk()` 只改变一方；物理用户页则暂时
COW 共享。

### 9.2 `copy_files()`

```text
CLONE_FILES：files_struct.count++
普通 fork：dup_fd()
  → 新 fdtable
  → 复制 open_fds/close_on_exec
  → 每个 struct file 增加引用
```

因此 fork 后父子 close 某个 fd 不会从对方 fdtable 移除，但两边 fd 仍指向同一 open file
description，共享 `f_pos` 和 file status flags。

### 9.3 `copy_fs()`

`fs_struct` 保存 cwd、root 和 umask。`CLONE_FS` 共享，使一个线程 `chdir()` 影响同组其他线程；
普通 fork 复制 path 引用，之后父子可独立改变 cwd。

### 9.4 信号对象

```text
sighand_struct：信号 disposition 表
signal_struct：线程组级 pending、线程计数、退出和统计
task fields：  blocked mask、线程私有 pending
```

`CLONE_SIGHAND` 共享 disposition；`CLONE_THREAD` 共享 `signal_struct`。signal mask 必须线程私有，
否则一个线程临时屏蔽信号会意外修改全组行为。

### 9.5 凭证和 namespace

凭证对象通常以不可变对象 + RCU 指针方式共享；修改 UID/capability 时 prepare 新 cred，再提交
替换，而不是原地修改共享对象。namespace 通过 `nsproxy` 聚合，`CLONE_NEW*` 决定某一维创建
新实例还是继承引用。

### 9.6 方案代价

“按资源选择复制/共享”非常灵活，但 clone flags 形成依赖图。`copy_process()` 必须先验证组合，
否则错误可能到复制一半才暴露，使回滚更复杂，甚至产生无法表达的语义。

---

## 10. 写时复制如何让 fork 足够快

### 10.1 不做 COW 会怎样

shell 自身可能映射动态库、堆和大量缓存，但 child 马上 exec。这些页若 fork 时全部复制，复制
结果几乎立即被 exec 丢弃。

COW 把成本推迟到实际写入：

```
fork 前：
parent writable PTE ─→ folio X

fork 后：
parent read-only/COW PTE ─┐
                          ├→ folio X
child  read-only/COW PTE ─┘

child 写入：
write fault → do_wp_page()
            → 分配 folio Y
            → 复制 X 内容
            → child writable PTE → Y
```

### 10.2 源码如何建立 COW

`copy_mm()` → `dup_mm()` → `dup_mmap()` 复制 VMA，页表复制进入 `copy_page_range()` 等路径。
对私有可写映射，父子 PTE 都被写保护，并增加 folio 映射/引用关系。之后任何一方写入都触发
fault，由内存管理判断复制还是在已独占时直接恢复可写。

### 10.3 COW 的优点

- fork 后 exec 的常见路径几乎不复制用户数据页；
- 父子只读访问长期共享物理页；
- 成本与实际修改的页数相关，而非完整地址空间大小。

### 10.4 COW 的代价

- fork 仍要复制 VMA 和页表，超大地址空间依然可能很慢；
- 写 fault 增加延迟、内存分配和复制；
- 父 PTE 也要写保护，可能触发 TLB shootdown；
- THP、大 folio、KSM、GUP pin 和 userfaultfd 会让 COW 判断复杂；
- 多线程父进程 fork 时，其他线程的内存写入与页表复制要正确同步。

这说明“fork 是 O(1)”是错误说法。它避免了按用户数据量完整复制，但页表成本仍随映射规模增长。

---

## 11. 子进程为什么从 fork 返回 0

父子并不是把 `kernel_clone()` 后半段各执行一次。child 从未执行父进程的内核调用栈；创建时
体系结构代码为它预造第一次运行现场。

以 ARM64 为例，`copy_thread()` 位于 `arch/arm64/kernel/process.c`，根据用户 task、内核线程
等情况复制/初始化 `cpu_context`、用户寄存器、TLS 等，并把 child 的返回值寄存器设置为 0。

```
父 task：
  kernel_clone() 正常返回 pid_vnr(child_pid)

子 task：
  第一次被调度
  → switch_to() 切入预造 context
  → ret_from_fork
  → 返回用户态
  → 用户 ABI 返回值寄存器为 0
```

这个设计的优点是 child 看起来像从同一个 `fork()` 返回，实际上只复用了用户态程序计数器和
寄存器语义。代价是每个体系结构必须正确实现 `copy_thread()` 与 `ret_from_fork` 契约。

---

## 12. 新任务如何进入调度器

### 12.1 `sched_fork()` 与 `wake_up_new_task()` 分工

`sched_fork()` 在构造阶段初始化 priority、policy、调度实体和 CPU，但不 enqueue。这样后续
copy 失败时，不需要从并发运行队列中追回一个可能已经执行的 task。

成功后：

```
wake_up_new_task(p)
  → p->__state = TASK_RUNNING
  → 锁 p->pi_lock 和目标 rq
  → update_rq_clock()
  → activate_task()/enqueue_task()
  → class->wakeup_preempt(... WF_FORK)
  → 必要时给目标 CPU reschedule IPI
```

### 12.2 父子谁先运行没有固定保证

child 入队后，调度类根据当前 CPU、deadline/vruntime、负载和特性决定是否抢占 parent。即使
配置倾向 child-runs-first，也不是用户 ABI。程序必须通过 pipe、futex、wait 等同步，不能依赖
fork 后父或子必然先执行。

### 12.3 shell 案例中的状态变化

```
copy_process 返回：
  child 已有 PID/资源
  __state 不是可自由运行状态
  on_rq = 0, on_cpu = 0

wake_up_new_task：
  __state = TASK_RUNNING
  on_rq = 1

调度选中 child：
  on_rq = 1
  on_cpu = 1
  从 ret_from_fork 返回用户态
```

`TASK_RUNNING` 同时包含“在队列等待”和“正在 CPU 上”，必须结合 `on_cpu` 理解。

---

## 13. `execve()`：在原 task 上替换程序

### 13.1 为什么 exec 不创建新进程

shell 的 child 调用 exec 后，父子关系、PID、很多 fd 和调度身份仍应保留；变化的是用户程序、
地址空间和部分进程属性。若 exec 创建新 task，再搬迁这些关系，会产生 PID 变化和观察窗口。

Linux 选择在当前 task 上替换：

```
同一个 task_struct / PID
旧 mm、程序入口、用户栈、部分 cred
                 ↓ exec
新 mm、ELF 映射、入口、用户栈、提交后的 cred
```

### 13.2 调用链

```
sys_execve
  → do_execveat_common()
      → 构造 linux_binprm
      → 统计并复制 argv/envp
      → bprm_stack_limits()
      → bprm_execve()
          → prepare_binprm()/LSM
          → exec_binprm()
              → search_binary_handler()
                  ├── load_elf_binary()
                  └── load_script()：#! 解释器后重新分派
          → audit/trace_sched_process_exec
```

`linux_binprm` 是一次 exec transaction：保存目标 file、文件头 buffer、argv/envp、待提交 cred、
新栈位置、解释器和安全状态。

### 13.3 为什么需要两阶段提交

最简单实现“先销毁旧 mm，再打开新 ELF”有致命问题：若文件格式错误或内存不足，当前 task 已
没有可返回的旧程序。

exec 因此尽量先完成可失败准备：

- 路径查找和打开 executable；
- execute 权限、noexec mount、LSM、setid/capability 检查；
- argv/envp 计数与复制；
- binary handler 识别；
- 新 mm、栈和映射准备。

到 `begin_new_exec()` 附近进入不可回滚阶段：协调线程组、提交新凭证/地址空间、关闭 CLOEXEC
fd、重置信号等。之后严重失败通常只能终止 task，不能回到旧程序。

源码用一个显式标志划出边界：

```c
trace_sched_prepare_exec(current, bprm);

/* Ensure all future errors are fatal. */
bprm->point_of_no_return = true;

retval = de_thread(current);
if (retval)
    goto out;

/* ... */
retval = exec_mmap(bprm);
if (retval)
    goto out;
bprm->mm = NULL;
```

`point_of_no_return` 之后仍可能出现错误，但错误不再返回旧用户程序；`exec_mmap()` 提交新 mm，
随后把 `bprm->mm` 清空表示所有权已经转移给 current。

### 13.4 ELF 装载过程

`load_elf_binary()` 验证 ELF header/program headers，建立：

```text
PT_LOAD 映射：代码、只读数据、可写数据、BSS
PT_INTERP：动态链接器（如 ld-linux）
用户栈：argc、argv[]、envp[]、auxv
入口寄存器：解释器入口或 ELF entry
```

大部分文件内容通过 file-backed VMA demand paging，不在 exec 时全部读入。首次执行/访问页面才
触发 page fault，从 page cache 或存储加载。因此 exec 成功的成本也不等于读取完整二进制。

### 13.5 exec 后哪些东西保留

通常保留：PID/父子关系、cwd/root、未标 CLOEXEC 的 fd、部分调度属性、资源限制。主要变化：

- 地址空间和用户寄存器；
- caught signal disposition 恢复默认；
- CLOEXEC fd 关闭；
- setid/file capabilities 可能改变 cred；
- alt signal stack、健壮 futex、rseq 等按各自规则重置；
- comm/exe_file 和 dumpability 更新。

成功 exec 不返回原调用点；下一次返回用户态时 PC 已指向新程序入口。

---

## 14. 多线程 exec 为什么复杂

只有调用 exec 的线程能进入新程序，其他线程必须消失。否则它们会继续使用即将销毁的旧 mm、
旧 handler 和用户栈。

`de_thread()` 协调：

1. 设置线程组 exec 状态，阻止冲突操作；
2. 终止同组其他线程并等待它们退出；
3. 若调用者不是 leader，处理 leader PID/TID 和 task 关系转换；
4. 保证 ptrace、parent/wait 和 `/proc/<tgid>` 仍符合线程组语义；
5. 最终让当前 task 成为唯一线程。

### 14.1 为什么不能直接“保留调用线程的 TID”

用户期望 exec 后进程 PID/TGID 不变，而线程组 TGID 由 leader 身份代表。如果非 leader 调用
exec，仅保留其原 TID 会让进程 PID 改变。内核需要交换/转移 PID 链接和 leader 角色，并与已经
退出但可能尚未 release 的旧 leader 协调。

### 14.2 方案代价

多线程 exec 使 exec 不再只是 mm 替换，还与线程退出、信号、ptrace 和 PID 表同时交互。
它需要 `cred_guard_mutex`、signal locks、tasklist lock 等协议，避免两个线程同时 exec、ptrace
修改 cred 或父进程 wait 到中间状态。

---

## 15. `exit()`：释放资源但保留死亡信息

### 15.1 shell 子进程的退出入口

`main()` 返回通常经 libc 调用 `exit_group()`；原始 `SYS_exit` 只退出当前线程。两者最终使相应
task 进入 `do_exit()`：

```
exit_group(status)
  → do_group_exit()
      → 设置 group_exit_code
      → 终止同组其他线程
      → do_exit(code)
```

### 15.2 `do_exit()` 的主线

按当前源码归纳：

```text
标记 PF_EXITING / 协调 group exit
→ ptrace、user events、io_uring、seccomp
→ signal->live--，判断是否最后线程
→ 保存 exit_code，汇总 accounting/perf/taskstats
→ exit_mm()
→ exit_sem/shm/files/fs
→ namespace、task_work、arch thread、cgroup
→ exit_tasks_rcu_start()
→ exit_notify()：reparent + SIGCHLD/pidfd + zombie/autoreap
→ 清剩余 task 私有资源
→ preempt_disable()
→ exit_rcu()
→ do_task_dead()
```

实际顺序是正确性的一部分。例如 perf 需要在 mm 销毁前停止访问；父进程被 SIGCHLD 唤醒前，
退出统计必须已经汇总；`exit_notify()` 操作父子关系时 task 仍需保持可引用。

### 15.3 `exit_mm()`

它先让 task 与用户 mm 脱离，再 `mmput()`。若是最后 `mm_users`，内存管理 `exit_mmap()` 撤销
VMA/页表，减少 folio 引用并释放页表。多线程共享 mm 时，单线程退出只减少引用，不能销毁
其他线程仍使用的地址空间。

### 15.4 为什么不能直接 free current

`do_exit()` 当前仍在：

- 使用自己的 task_struct；
- 使用自己的内核栈；
- 作为 `rq->curr` 在 CPU 上运行；
- 可能被 RCU task/PID 读者观察。

`do_task_dead()` 把 task 置为不可再运行并最后一次 schedule，永不返回。后续由其他 task 的
`finish_task_switch()`、父进程 reap 和引用/RCU 回调完成分阶段释放。

`do_exit()` 的结尾证明退出者只能“切走自己”：

```c
check_stack_usage();
preempt_disable();
exit_rcu();
exit_tasks_rcu_finish();

lockdep_free_task(tsk);
do_task_dead();
```

函数具有 `__noreturn` 属性。任何把 `kfree(current)` 放在这里的方案都会释放正在执行的栈和
调度器仍引用的当前 task。

---

## 16. `wait()`：父进程如何取得结果并收尸

### 16.1 为什么需要 zombie

若 child 一退出就删除 task/PID，shell 调用 wait 时无法得到：

- 哪个 child 退出；
- 正常退出码还是信号终止；
- 是否产生 core dump；
- 用户/系统 CPU 时间、缺页和 I/O 统计。

所以 `do_exit()` 释放 mm、files 等大资源，但保留 task 的身份、退出码和统计，设置
`EXIT_ZOMBIE`。zombie 不运行、通常也没有用户地址空间；它不是一个仍占 CPU 的“活进程”。

### 16.2 wait 调用链

```
waitpid(pid, &status, 0)
  → sys_wait4
  → kernel_wait4()/do_wait()
      → 根据 pid/pgid、clone 类型和 options 扫描子任务
      → wait_consider_task()
          ├── wait_task_zombie()
          ├── wait_task_stopped()
          └── wait_task_continued()
      → 无事件：睡在 signal->wait_chldexit
      → 被 SIGCHLD/子状态变化唤醒后重扫
```

等待必须循环，因为唤醒可能属于另一个 child，事件也可能被另一个同线程组 waiter 消费。

### 16.3 `wait_task_zombie()` 的状态领取

多个线程可能同时 wait 同一个 child。内核不能让两者都成功收尸。领取方原子地把
`EXIT_ZOMBIE` 推进到 `EXIT_DEAD`（或 ptrace 相关中间语义），从而获得唯一回收权。

之后它：

- 读取并编码 exit status；
- 汇总 child resource usage 到父进程；
- 向用户空间复制 `status/rusage/siginfo`；
- 调用 `release_task()` 摘除并推进最终释放。

唯一领取权由原子状态转换保证：

```c
state = (ptrace_reparented(p) && thread_group_leader(p)) ?
        EXIT_TRACE : EXIT_DEAD;

if (cmpxchg(&p->exit_state, EXIT_ZOMBIE, state) != EXIT_ZOMBIE)
    return 0;

/* We own this thread, nobody else can reap it. */
read_unlock(&tasklist_lock);
```

成功的 waiter 在释放 `tasklist_lock` 前已经把状态从 ZOMBIE 改走，因此其他 waiter 即使随后找到
同一个 task，也无法第二次取得退出统计或调用 `release_task()`。

### 16.4 SIGCHLD 与 wait 不是同一机制

SIGCHLD 是通知，wait 是读取并消费子状态。信号可能合并，不能把“收到几次 SIGCHLD”等同于
“退出了几个 child”。正确程序应在 handler/event loop 中循环 `waitpid(-1, ..., WNOHANG)`，直到
没有更多可领取状态。

若 SIGCHLD disposition 为 `SIG_IGN` 或设置 `SA_NOCLDWAIT`，符合语义的 child 可自动 reap，
不长期保留 zombie。

---

## 17. 孤儿进程、subreaper 与 PID namespace

父进程退出时，child 不能继续指向即将释放的 `real_parent`。`forget_original_parent()` 为其选择
新父进程：

```text
同线程组中合适的存活 task
→ 最近的 child subreaper
→ 当前 PID namespace 的 child reaper（通常 PID 1）
```

### 17.1 为什么需要 subreaper

传统 daemon double-fork 后会成为 init 的后代。容器/service manager 希望负责其服务树中所有
后代的退出状态，于是通过 `PR_SET_CHILD_SUBREAPER` 成为中间收养者。

### 17.2 reparent 的并发要求

父指针由 RCU 读者访问，父子链表与 wait/ptrace 又需要结构性一致。退出路径在 tasklist lock
和 signal locks 协议下移动 child、更新 parent/real_parent、决定是否发送死亡信号，并将可自动
回收对象收集到临时 dead list，出锁后再 `release_task()`。

不能在持有全局 tasklist write lock 时执行所有慢速释放，否则会长时间阻塞 fork、exit、信号
和 `/proc` 任务遍历。

### 17.3 namespace PID 1 退出

PID namespace 的 child reaper 是该空间的生命周期支点。它退出时内核需要终止/清理 namespace
中的其他进程，之后不能再正常创建新成员。全局 init 若退出，内核通常 panic，因为系统失去
最终 orphan reaper 和用户空间管理根。

---

## 18. 信号如何贯穿生命周期

### 18.1 产生、排队、选择与递送

```
产生：kill/tgkill、异常、终端、timer、child exit
  → 权限与目标查找
  → 线程私有 pending 或 signal->shared_pending
  → 选择可接收线程并 signal_wake_up()
  → task 返回用户态前 get_signal()
  → 默认动作、忽略或构造用户 signal frame
```

普通非实时信号可能合并，实时信号可以排队。线程 mask 私有，所以线程组信号需要选择一个未
屏蔽它的线程；fatal group signal 则协调全组退出。

### 18.2 fork 期间的信号窗口

`copy_process()` 构造 child 时，父线程组可能同时收到 multiprocess signal。如果只在复制某个
瞬间抓取 pending，信号可能落在“复制完 signal 状态、child 尚未发布”的窗口，导致 POSIX
语义不一致。

当前 fork 路径使用 delayed multiprocess signal 节点和 `sighand->siglock` 协调，直到 child
正确继承/发布后移除。失败路径也必须 `hlist_del_init()`，因为该节点通常来自调用栈，遗留在
链表会形成悬挂指针。

### 18.3 SIGCHLD 的生成

`exit_notify()` 根据 exit_signal、ptrace、线程组 leader、SIGCHLD disposition 和 autoreap
规则调用 `do_notify_parent()`，填充 child exit siginfo，唤醒 `wait_chldexit`，并通知 pidfd。

通知发生前必须写好 `exit_code` 和统计；否则父进程被唤醒后可能看到不完整死亡记录。

---

## 19. 并发、锁、引用计数与 RCU

### 19.1 没有一把“进程总锁”

| 机制 | 主要职责 |
|------|----------|
| `tasklist_lock` | 父子、线程组、PID 链接等结构性修改 |
| `sighand->siglock` | pending、signal action、job control、组退出状态 |
| `p->pi_lock` + rq lock | task 状态、CPU 归属、调度队列 |
| `task_lock()` | task 部分资源指针和状态 |
| `cred_guard_mutex` | exec/凭证/ptrace 关键转换 |
| 引用计数 | task、pid、mm、files、cred 等对象所有权 |
| RCU | task/PID/parent 等读多写少查找和延迟释放 |

### 19.2 查到指针不等于持有对象

典型模式：

```c
rcu_read_lock();
p = pid_task(pid, PIDTYPE_PID);
if (p)
    get_task_struct(p);
rcu_read_unlock();

if (p) {
    /* 可以跨越 RCU 临界区使用 p */
    put_task_struct(p);
}
```

RCU 只保证临界区内对象内存不释放；若退出临界区后继续用，必须取得引用。反之，引用保证对象
存在，不自动保证它仍是某 PID 的 active task，也不冻结 parent/mm 等可变字段。

### 19.3 task 释放链

概念上：

```
do_exit()
  → 不再运行
wait/autoreap
  → release_task()
      → 从 PID/父子/线程组结构摘除
      → put_task_struct_rcu_user()
          → call_rcu(delayed_put_task_struct)
              → put_task_struct()
                  → 最后 usage 引用
                  → free_task()
                      → 释放内核栈与 task_struct
```

具体引用可能让任一步延迟。RCU 回调保护仍在无锁遍历的读者；`usage` 引用保护明确持有者。

### 19.4 锁保护状态，不替代生命周期

持有 tasklist lock 时可安全修改父子链，但出锁后裸 task 指针仍可能释放；需要 task 引用。
持有 task 引用时对象不会释放，但若要读取 `sighand`，还要通过 `lock_task_sighand()` 应对它在
exit 中变 NULL。不同机制解决不同问题。

### 19.5 发布后的裸指针风险

`kernel_clone()` 在 `wake_up_new_task()` 前完成 trace 和 PID 引用获取，是因为 child 一旦运行，
可能立即 `_exit()` 并被其他线程 autoreap。发布点之后，调用者若要继续使用 child，必须先持有
正确引用，而不能依赖“它刚刚创建，不可能退出”。

---

## 20. 线程、vfork、pidfd 与容器分支

### 20.1 pthread 创建

典型线程 clone flags 包括：

```text
CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS |
CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID
```

内核仍创建独立 task、内核栈、TID 和调度实体。`CLONE_CHILD_CLEARTID` 让线程退出时清用户态
TID 地址并 futex wake，pthread join 可据此等待内核侧线程真正退出。

优点是线程与进程共用创建/调度/信号基础设施；代价是共享对象让 exec、exit_group 和资源修改
需要线程组级协调。

### 20.2 vfork

vfork child 临时与 parent 共享 mm，避免页表复制；parent 在 completion 上等待 child exec 或
exit：

```
parent 建栈上 completion
→ child->vfork_done = &completion
→ wake child
→ parent wait_for_vfork_done()

child exec/exit
→ complete_vfork_done()
→ parent 继续
```

收益是创建后立即 exec 的路径更轻；代价是 child 与暂停的 parent 共享地址空间甚至用户栈，
child 在 exec/_exit 前修改普通数据、返回调用者或执行复杂库代码都可能破坏 parent。

### 20.3 pidfd

`CLONE_PIDFD` 或 `pidfd_open()` 返回 fd，fd 内持有 `struct pid`。可用于：

- `pidfd_send_signal()` 精确发送信号；
- poll/epoll 等待退出；
- 部分系统调用以 pidfd 获取目标资源；
- 避免“检查 `/proc/PID` 后 PID 被复用”的 TOCTOU。

pidfd 保证身份不误指向复用后的进程，但不保证 task 永远存活；退出后操作应返回相应状态。

### 20.4 容器创建

`clone3()` 配合 `CLONE_NEWUSER/NEWPID/NEWNS/NEWNET/...` 创建隔离视图；`CLONE_INTO_CGROUP`
可让 child 从首次可运行开始就在目标 cgroup，避免先在父 cgroup 短暂运行再迁移的窗口。

namespace 决定“看见什么”，cgroup 决定“如何计费和限制”。它们都挂在 task 生命周期上，但不是
同一机制。

---

## 21. 失败路径与方案优劣

### 21.1 fork 为什么有大量 cleanup 标签

构造过程中资源依次获得：

```text
task/stack → cred → sched → files → fs → sighand → signal
→ mm → namespace → io → PID → thread context → published relations
```

任一步失败，必须按相反顺序撤销已经成功的步骤。`copy_process()` 的
`bad_fork_cleanup_*` 链是显式 unwind stack：

```text
copy_mm 之后失败
→ mmput
→ cleanup signal
→ cleanup sighand
→ exit_fs
→ exit_files
→ sched_cancel_fork
→ exit_creds
→ put_task_stack
→ delayed_free_task
```

顺序错误会造成引用泄漏或回调访问已释放依赖。例如 signal 共享与非共享路径不同，不能无条件
free；task 内存还要考虑 RCU 读者。

### 21.2 fork 方案评价

| 方案 | 优点 | 代价/边界 |
|------|------|-----------|
| 完整复制 | 语义简单，写入无 fault | 创建成本与全部内存成正比 |
| COW fork | fork+exec 快，只复制写过的页 | 页表复制、写 fault、TLB 和 pin 复杂 |
| vfork | 几乎不复制 mm/页表 | parent 停止，child 可安全操作范围极小 |
| posix_spawn 类接口 | libc/内核可选择更合适路径 | API 表达能力与平台实现有差异 |

### 21.3 统一 clone 模型评价

优点：一种机制覆盖进程、线程、namespace task、内核 worker 的大部分创建需求。资源共享可以
组合，新增能力可通过 `clone3` 扩展。

代价：flags 语义复杂，非法组合多；安全审计需要同时理解共享边界；某些内部 worker 看似线程，
却有特殊信号、mm 或退出规则。

### 21.4 zombie/wait 方案评价

优点：父进程可靠获得退出码和统计；父子生命周期解耦；child 可先释放昂贵资源。

代价：不 wait 会积累 zombie 和 PID；reparent/subreaper 增加关系维护；父进程多线程 wait 与
ptrace 需要原子领取状态。

替代做法“退出时把结果放进无界消息队列”仍需要结果对象的容量、所有权和消费协议，本质上只是
把 zombie 换成另一种死亡记录。Linux 选择复用 task/PID 关系直到 wait。

---

## 22. 用实验验证完整过程

### 22.1 最小实验程序

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int main(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        char *argv[] = { "/bin/echo", "hello", NULL };
        execve(argv[0], argv, environ);
        perror("execve");
        _exit(127);
    }

    printf("parent=%d child=%d\n", getpid(), pid);
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    printf("exit=%d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return 0;
}
```

### 22.2 用 strace 观察语义

```bash
strace -f -yy \
  -e trace=clone,clone3,fork,vfork,execve,wait4,waitid,exit,exit_group \
  ./demo
```

预期观察：

- libc/体系结构可能使用 `clone` 而不是直接显示 `fork`；
- parent 得到 child PID，child 看到返回 0；
- child `execve` 成功后不返回旧程序；
- parent 的 wait 在 child 退出前阻塞；
- `/bin/echo` 通常以 `exit_group(0)` 结束；
- wait 返回编码后的 status。

### 22.3 用 tracepoint 对照源码发布点

```bash
trace-cmd record \
  -e sched:sched_process_fork \
  -e sched:sched_process_exec \
  -e sched:sched_process_exit \
  -e sched:sched_wakeup \
  -e sched:sched_switch \
  ./demo

trace-cmd report
```

将事件映射回源码：

```text
sched_process_fork：kernel_clone() 中，wake_up_new_task() 之前
sched_wakeup：      child 首次/后续变为 runnable
sched_switch：      child 真正获得 CPU
sched_process_exec：exec 新映像提交后
sched_process_exit：do_exit() 资源释放阶段
```

注意 `sched_process_exit` 不等于 `release_task` 或内存已经 free。要验证 zombie，可让 parent 在
wait 前 sleep：

```c
sleep(10);
waitpid(pid, &status, 0);
```

期间执行：

```bash
ps -o pid,ppid,stat,wchan,cmd -p <child-pid>
cat /proc/<child-pid>/status
```

应看到 `Z` 状态；parent wait 后 `/proc/<child-pid>` 消失。PID 数字未来仍可能复用。

### 22.4 观察父子共享与独立资源

在 fork 后、exec 前暂停 child，可比较：

```bash
cat /proc/<parent>/maps
cat /proc/<child>/maps
ls -l /proc/<parent>/fd
ls -l /proc/<child>/fd
cat /proc/<parent>/status | grep -E 'Pid|Tgid|PPid|Threads'
cat /proc/<child>/status  | grep -E 'Pid|Tgid|PPid|Threads'
```

maps 虚拟布局初始相似但属于不同 mm；fdtable 是副本，但对应 fd symlink/open file 可指向同一
对象；parent/child 的 TGID 不同。对 pthread，则 `/proc/<tgid>/task/` 中多个 TID 共享 TGID。

---

## 23. 回到案例与源码索引

### 23.1 `/bin/echo hello` 每一步的内核含义

```
shell 调用 fork
  → kernel_clone/copy_process
  → 新 task、PID、mm 元数据和资源引用
  → 用户页 COW，共享 open file descriptions
  → 构造 child 返回值为 0 的寄存器现场
  → 发布父子/PID 关系
  → wake_up_new_task

child 被调度
  → ret_from_fork 返回用户态
  → execve("/bin/echo")
  → linux_binprm + ELF handler
  → 新 mm 和用户栈提交，PID 保持
  → ELF/解释器入口运行

parent 调用 waitpid
  → 暂无 child 状态，进入 wait_chldexit 睡眠

echo 退出
  → do_group_exit/do_exit
  → 释放 mm/files 等引用
  → 保存 exit_code=0
  → exit_notify：SIGCHLD + 唤醒 parent + pidfd notification
  → EXIT_ZOMBIE
  → do_task_dead，永不再运行

parent 醒来
  → wait_task_zombie 原子领取状态
  → 拷贝 status、累计统计
  → release_task 摘除 PID/父子/线程组关系
  → 引用归零和 RCU 宽限期后释放 task/stack/PID
```

### 23.2 关键函数索引

| 阶段 | 函数 | 位置 | 在案例中的作用 |
|------|------|------|----------------|
| 创建入口 | `kernel_clone()` | `kernel/fork.c` | 组织 fork 外围语义并最终唤醒 |
| 核心构造 | `copy_process()` | `kernel/fork.c` | 复制/共享资源，建立身份关系 |
| task/栈 | `dup_task_struct()` | `kernel/fork.c` | 分配 child task 与内核栈 |
| 地址空间 | `copy_mm()` / `dup_mm()` | `kernel/fork.c` | 共享或复制 mm |
| VMA/页表 | `dup_mmap()` | `mm/mmap.c` | 复制映射并建立 COW 页表 |
| 文件表 | `copy_files()` / `dup_fd()` | `kernel/fork.c`、`fs/file.c` | 共享或复制 fdtable |
| PID | `alloc_pid()` / `attach_pid()` | `kernel/pid.c` | 分配 namespace PID 并发布链接 |
| 寄存器 | `copy_thread()` | `arch/*/kernel/process.c` | 构造 child 首次运行现场 |
| 首次入队 | `wake_up_new_task()` | `kernel/sched/core.c` | child 变为 runnable |
| exec 入口 | `do_execveat_common()` | `fs/exec.c` | 参数复制与 exec transaction |
| exec 提交 | `begin_new_exec()` | `fs/exec.c` | 进入不可回滚的新映像提交阶段 |
| 多线程 exec | `de_thread()` | `fs/exec.c` | 终止其他线程并处理 leader |
| ELF | `load_elf_binary()` | `fs/binfmt_elf.c` | 建映射、栈和入口 |
| 线程组退出 | `do_group_exit()` | `kernel/exit.c` | 协调全组退出 |
| task 退出 | `do_exit()` | `kernel/exit.c` | 释放资源、保存状态并永久切走 |
| 通知/reparent | `exit_notify()` | `kernel/exit.c` | 收养 child、通知 parent/pidfd |
| 等待 | `do_wait()` | `kernel/exit.c` | 扫描符合条件的子状态 |
| zombie 领取 | `wait_task_zombie()` | `kernel/exit.c` | 原子消费死亡状态和统计 |
| 最终摘除 | `release_task()` | `kernel/exit.c` | 移除关系并推进引用释放 |

### 23.3 五个不变量

1. **task 是执行与调度单位；用户所说的进程更接近共享 `signal_struct` 的线程组。**
2. **clone flags 决定资源共享边界；普通 fork 的用户页采用 COW，而非完整复制。**
3. **创建必须先完成私有构造和关系发布，最后才能让调度器运行 child。**
4. **退出、zombie、wait/reap 和最终释放是不同阶段，分别服务通知、统计和并发安全。**
5. **锁保护关系变化，引用计数保护明确持有者，RCU 保护无锁读者；三者不能互相替代。**

理解这五个不变量后，fork、pthread、exec、exit、wait、pidfd 和 PID namespace 不再是分散功能，
而是同一套任务模型在不同资源共享方式和生命周期阶段上的表现。
