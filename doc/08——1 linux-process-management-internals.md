# Linux 进程管理内部实现

> 适用内核版本：v7.2-rc1（基于当前仓库 `a14c021eef14`）  
> 核心源码：`kernel/fork.c`、`kernel/exit.c`、`fs/exec.c`、`kernel/pid.c`、`kernel/signal.c`、`kernel/sched/`

---

## 目录

1. [背景：内核究竟在管理什么](#1-背景内核究竟在管理什么)
2. [设计目标与技术方案](#2-设计目标与技术方案)
3. [总体架构](#3-总体架构)
4. [核心数据结构](#4-核心数据结构)
5. [进程、线程与 PID 的内核语义](#5-进程线程与-pid-的内核语义)
6. [创建路径：fork、vfork 与 clone](#6-创建路径forkvfork-与-clone)
7. [程序替换：execve](#7-程序替换execve)
8. [运行、睡眠与唤醒](#8-运行睡眠与唤醒)
9. [退出、僵尸与回收](#9-退出僵尸与回收)
10. [信号与线程组](#10-信号与线程组)
11. [命名空间、cgroup 与容器](#11-命名空间cgroup-与容器)
12. [并发控制与生命周期安全](#12-并发控制与生命周期安全)
13. [内核线程与特殊任务](#13-内核线程与特殊任务)
14. [观测、调试与常见误区](#14-观测调试与常见误区)
15. [关键函数速查](#15-关键函数速查)

---

## 1. 背景：内核究竟在管理什么

用户通常把“进程”理解为一个正在运行的程序，但内核看到的是若干相互独立、又可按需共享的对象：

- 一条可被调度的执行流；
- 一套虚拟地址空间；
- 文件描述符表、当前目录和根目录；
- 凭证、资源限制、命名空间和 cgroup 身份；
- 信号处理规则、待处理信号和父子关系；
- PID、进程组、会话以及对这些对象的引用。

因此 Linux 没有一个包办所有内容的“大进程对象”。`task_struct` 是执行流的中心索引，
它通过指针连接 `mm_struct`、`files_struct`、`fs_struct`、`cred`、`nsproxy`、
`signal_struct` 等对象。不同 `clone` 标志决定子任务是共享这些对象，还是获得副本。

这种拆分解决了两个看似冲突的需求：

1. `fork()` 要创建“几乎完全一样”的进程，但不能真的复制全部物理内存；
2. 线程要共享地址空间和文件，却必须拥有独立寄存器、内核栈、调度状态和 TID。

Linux 的答案是：**统一任务模型 + 可组合的资源共享 + 写时复制 + 引用计数生命周期**。

---

## 2. 设计目标与技术方案

### 2.1 设计目标

| 目标 | 内核方案 |
|------|----------|
| 快速创建 | 页表复制配合 Copy-on-Write，资源对象按标志共享 |
| 统一进程与线程 | 每条执行流都是 `task_struct`，线程只是共享更多资源的任务 |
| 独立调度 | 每个线程都有调度实体、状态、优先级和 CPU 亲和性 |
| 稳定标识 | 数字 PID 与引用对象 `struct pid` 分离，支持 pidfd |
| 容器隔离 | PID、mount、user、net 等 namespace 按层组合 |
| 可控资源 | cgroup 负责分组、统计、限额和调度控制 |
| 安全回收 | 引用计数、锁和 RCU 配合，退出与最终释放分阶段进行 |
| POSIX 语义 | 线程组、进程组、会话、信号、wait 和 reparenting 协同实现 |

### 2.2 三个关键取舍

**创建不是完整复制。** `fork()` 复制页表并将用户页设为只读共享，父子任一方写入时
才触发缺页异常并复制页面。文件等内核对象通常只增加引用计数。

**退出不等于立即释放。** 子进程退出后要保留 PID、退出码和资源统计，供父进程
`wait()`；任务先成为 zombie，父进程收尸后才进入最终释放。

**数字 PID 不是可靠句柄。** PID 会复用。内核用带引用计数的 `struct pid` 表示身份，
用户空间可用 pidfd 获得不会因数字复用而误指向其他进程的句柄。

---

## 3. 总体架构

```
                    系统调用层
 fork/vfork/clone/clone3  execve  exit  wait  kill/pidfd
          │                 │       │     │       │
          ▼                 ▼       ▼     ▼       ▼
 ┌─────────────────────────────────────────────────────────┐
 │                    进程生命周期层                        │
 │ kernel/fork.c      fs/exec.c       kernel/exit.c         │
 │ 创建/资源复制      装载/替换映像   退出/通知/回收        │
 └───────────┬─────────────┬──────────────┬────────────────┘
             │             │              │
       ┌─────▼─────┐ ┌────▼─────┐  ┌────▼────────┐
       │ PID/关系  │ │ 信号模型 │  │ 调度器      │
       │ pid.c     │ │ signal.c │  │ kernel/sched│
       └─────┬─────┘ └────┬─────┘  └────┬────────┘
             │             │              │
 ┌───────────▼─────────────▼──────────────▼────────────────┐
 │                  被 task_struct 引用的资源               │
 │ mm  files  fs  cred  nsproxy  sighand/signal  cgroup ...│
 └─────────────────────────────────────────────────────────┘
```

职责边界很重要：进程管理决定任务的创建、身份、关系和资源生命周期；调度器只关心
哪些任务可运行、在哪个 CPU 运行以及何时切换。一次 `fork()` 的末尾通过
`wake_up_new_task()` 把新任务交给调度器，两个子系统在这里衔接。

---

## 4. 核心数据结构

### 4.1 `task_struct`：每条执行流的描述符

定义于 `include/linux/sched.h`。它很大，但可以按职责理解：

```
task_struct
├── 执行现场：stack、thread、thread_info
├── 调度状态：__state、on_cpu、on_rq、prio、sched_class、se/rt/dl
├── 身份：pid、tgid、comm、thread_pid
├── 关系：real_parent、parent、children、sibling、group_leader
├── 地址空间：mm、active_mm
├── 资源：files、fs、cred、nsproxy、io_context
├── 信号：signal、sighand、pending、blocked
├── PID 链接：pid_links[]、thread_node
├── 退出：exit_state、exit_code、exit_signal
└── 统计/审计/安全/cgroup/RCU 等扩展字段
```

`task_struct` 自身通常来自专用 slab cache；每个任务还拥有独立内核栈。进入系统调用、
中断或异常后，用户线程在自己的内核栈上执行。上下文切换保存的是 CPU 执行现场，
不是复制整个 `task_struct`。

### 4.2 `mm_struct`：虚拟地址空间

`task->mm` 指向用户地址空间，包含页表、VMA 管理结构、代码/数据/堆栈边界和内存统计。

- 普通进程有自己的 `mm`；
- `CLONE_VM` 线程共享 `mm`；
- 内核线程的 `mm == NULL`，运行时借用前一个用户任务的 `active_mm`，但不会拥有它。

`fork()` 的 `dup_mm()`/`dup_mmap()` 创建新的 `mm_struct` 和映射元数据，匿名页、私有文件
映射通过页表权限实现 COW。`execve()` 则建立新 `mm`，提交成功后替换旧地址空间。

### 4.3 `files_struct` 与 `fs_struct`

二者经常被混淆：

| 结构 | 保存内容 | 共享标志 |
|------|----------|----------|
| `files_struct` | fd → `struct file *` 的描述符表及 close-on-exec 位图 | `CLONE_FILES` |
| `fs_struct` | cwd、root、umask | `CLONE_FS` |

不使用 `CLONE_FILES` 时，`copy_files()` 复制描述符表，但各槽位仍指向同一个 open file
description，因此文件偏移也可能共享。`execve()` 会关闭标有 `FD_CLOEXEC` 的描述符。

### 4.4 `signal_struct` 与 `sighand_struct`

- `signal_struct`：线程组共享状态，如组级 pending、线程数、退出状态、资源统计；
- `sighand_struct`：信号 disposition，即每个信号是默认、忽略还是由哪个 handler 处理；
- `task_struct.pending` 和 `blocked`：线程私有的待处理队列与屏蔽字。

`CLONE_THREAD` 形成线程组；它要求配合 `CLONE_SIGHAND`，而 `CLONE_SIGHAND` 又要求
`CLONE_VM`，内核在 `copy_process()` 中检查这些组合约束。

### 4.5 `struct pid`：可引用的进程身份

定义于 `include/linux/pid.h`：

```
struct pid
├── count                  引用计数
├── level                  所处 PID namespace 层级
├── tasks[PIDTYPE_MAX]     使用该身份的任务链表
├── wait_pidfd             pidfd 退出通知等待队列
└── numbers[]              每一层 namespace 的 (nr, ns)
```

同一个任务同时参与四类 PID 关系：

- `PIDTYPE_PID`：单个线程的 TID；
- `PIDTYPE_TGID`：线程组 ID，用户通常称其为进程 PID；
- `PIDTYPE_PGID`：进程组 ID，用于作业控制；
- `PIDTYPE_SID`：会话 ID。

`task_struct.pid/tgid` 是便于快速访问的数值缓存；跨生命周期持有身份应使用
`struct pid *` 或 pidfd，而不是只保存整数。

---

## 5. 进程、线程与 PID 的内核语义

Linux 调度器不调度“进程”，只调度 task。单线程进程有一个 task，多线程进程有多个 task：

```
进程（线程组，TGID=1000）
├── leader: pid=1000, tgid=1000, group_leader=self
├── thread: pid=1001, tgid=1000, group_leader=leader
└── thread: pid=1002, tgid=1000, group_leader=leader

共享：mm、files、fs、sighand、signal（典型 pthread 配置）
独立：TID、内核栈、寄存器、调度实体、信号 mask、线程私有 pending
```

用户空间的 `getpid()` 返回 TGID，`gettid()` 返回当前 task 的 PID/TID。`ps` 默认按线程组
展示“进程”，`ps -L` 或 `/proc/<tgid>/task/` 才展开每个 task。

`fork()` 与 pthread 创建并非两套机制：它们最终都进入 `kernel_clone()` 和
`copy_process()`，区别主要是 clone flags。典型 pthread 会设置 `CLONE_VM | CLONE_FS |
CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM` 等共享标志。

---

## 6. 创建路径：fork、vfork 与 clone

### 6.1 系统调用汇聚

```
fork()   ── flags=SIGCHLD ───────────────────────────┐
vfork()  ── CLONE_VFORK|CLONE_VM|SIGCHLD ───────────┤
clone()  ── ABI 参数转换 ────────────────────────────┤
clone3() ── copy_clone_args_from_user() ─────────────┤
                                                       ▼
                                                kernel_clone()
                                                       │
                                                copy_process()
                                                       │
                                                wake_up_new_task()
```

`clone3()` 用可扩展的 `struct clone_args` 取代 `clone()` 难以扩展且依体系结构变化的参数
布局，并原生支持 pidfd、set_tid、cgroup fd 等功能。

### 6.2 `copy_process()` 的主要阶段

该函数是创建路径的核心，可概括为：

1. 校验 clone flags、线程组和 namespace 组合；
2. 检查 `RLIMIT_NPROC`，分配 `task_struct` 和内核栈；
3. 复制凭证、调度属性、审计、安全和性能事件状态；
4. 按标志复制或共享 files、fs、sighand、signal、mm、namespace、I/O context；
5. 调用 `sched_fork()` 初始化调度实体，此时任务尚不可运行；
6. 分配 PID，设置 pid/tgid/group leader 和父子关系；
7. 在 `tasklist_lock` 保护下，把任务发布到全局任务、父子、线程组和 PID 链表；
8. 返回一个尚未运行的 task，由 `kernel_clone()` 完成用户指针写回、trace 和唤醒。

伪代码如下：

```c
p = dup_task_struct(current, node);
copy_creds(p, clone_flags);
sched_fork(clone_flags, p);
copy_files(clone_flags, p);
copy_fs(clone_flags, p);
copy_sighand(clone_flags, p);
copy_signal(clone_flags, p);
copy_mm(clone_flags, p);
copy_namespaces(clone_flags, p);
pid = alloc_pid(...);
copy_thread(p, args);          /* 构造子任务首次运行时的寄存器现场 */
/* 持有 tasklist_lock，发布父子关系、线程组和 PID 链接 */
```

实际顺序还穿插 LSM、cgroup、perf、futex、rseq 等子系统。函数尾部大量
`bad_fork_cleanup_*` 标签按初始化的逆序回滚，这是一种内核中常见的事务式资源管理。

### 6.3 为什么子进程从 fork 返回 0

父子不是在 `fork()` 后执行同一条内核路径两遍。体系结构相关的 `copy_thread()` 为子任务
预造寄存器现场，把子任务的返回值寄存器设为 0；父任务则从 `kernel_clone()` 得到子 PID。
子任务第一次被调度时，从 `ret_from_fork` 一类入口恢复该现场，像是刚从系统调用返回。

### 6.4 COW：fork 快的根本原因

```
fork 后：
 parent PTE ─┐
             ├──> 同一物理页（只读/COW）
 child  PTE ─┘

child 写入：写保护异常 → 分配新页 → 复制内容 → 修改 child PTE → 恢复写入
```

COW 延迟了物理页复制，但 fork 仍要复制页表与 VMA 元数据。超大地址空间即使几乎不写，
fork 也可能有明显开销；`MADV_DONTFORK`、`MADV_WIPEONFORK` 可改变特定映射的继承行为。

### 6.5 `vfork()` 的特殊同步

`vfork()` 使用 `CLONE_VM` 临时共享地址空间，并用 `CLONE_VFORK` 让父任务等待。子任务的
`vfork_done` 指向父任务栈上的 completion；子任务 `execve()` 提交新地址空间或退出时调用
`complete_vfork_done()`，父任务才继续。

由于父子共享栈和地址空间，子进程在 `vfork()` 后修改普通数据、从调用它的函数返回或执行
复杂库逻辑都不安全；预期路径是尽快 `_exit()` 或 `execve()`。

### 6.6 发布顺序为何重要

新任务在所有字段、PID 和关系初始化完成前不能进入运行队列，否则另一 CPU 可能运行或查到
一个半初始化对象。因此创建分为“构造”和“发布/唤醒”：

```
不可见、不可运行：分配 → 复制资源 → 初始化调度/PID/寄存器
原子式发布：       挂入各链表
可运行：           wake_up_new_task()
```

---

## 7. 程序替换：execve

`execve()` 不创建新进程，也不会改变成功调用者的 PID。它在当前 task 上替换用户态程序映像：

```
sys_execve()
  → do_execveat_common()
    → 构造 linux_binprm，复制 argv/envp
    → bprm_execve()
      → exec_binprm()
        → search_binary_handler()
          → ELF: load_elf_binary()
          → script: load_script()，解析 #! 后重新查找解释器
```

### 7.1 两阶段提交

exec 必须避免“旧程序已经销毁，新程序又装载失败”的不可恢复状态。因此多数可能失败的检查
先完成：打开文件、权限/LSM 检查、参数复制、格式识别、创建新 mm 和映射。到
`begin_new_exec()` 附近进入不可回滚阶段，随后提交新凭证和地址空间。

成功后的主要变化：

- 旧用户地址空间被替换，建立 ELF 段、动态链接器、栈和 auxv；
- 其他线程被终止，调用线程成为线程组中唯一线程；
- 被捕获的信号 disposition 恢复默认，被忽略的通常保持忽略；
- `FD_CLOEXEC` 文件描述符被关闭；
- setuid/setgid、file capabilities 等可能更新凭证；
- 体系结构寄存器被设置为新程序入口，成功时不会返回旧调用点。

若调用者不是线程组 leader，exec 还要处理 PID/领导者身份转换，这也是 `de_thread()`
复杂的原因之一。

---

## 8. 运行、睡眠与唤醒

### 8.1 状态不是一条简单枚举

`task_struct.__state` 描述运行/睡眠状态，`exit_state` 单独描述 zombie/dead。常见状态：

- `TASK_RUNNING`：正在 CPU 上运行，或已在 runqueue 等待；
- `TASK_INTERRUPTIBLE`：可被信号打断的睡眠；
- `TASK_UNINTERRUPTIBLE`：等待特定内核事件，普通信号不唤醒；
- `TASK_KILLABLE`：只响应致命信号；
- `TASK_STOPPED` / `TASK_TRACED`：作业控制停止或被 ptrace 控制；
- `EXIT_ZOMBIE` / `EXIT_DEAD`：退出阶段，保存在 `exit_state`。

“运行中”和“可运行但在排队”都显示为 `TASK_RUNNING`，需要结合 `on_cpu`、`on_rq`
以及调度器运行队列判断。

### 8.2 睡眠的标准模式

等待队列把“条件检查、设置状态、入队、调度”组织成不会丢失唤醒的协议：

```c
DEFINE_WAIT(wait);

for (;;) {
    prepare_to_wait(&wq, &wait, TASK_INTERRUPTIBLE);
    if (condition)
        break;
    if (signal_pending(current))
        break;
    schedule();
}
finish_wait(&wq, &wait);       /* 恢复 TASK_RUNNING 并从等待队列摘除 */
```

生产代码通常使用 `wait_event*()` 宏。关键点是先把自己变为可唤醒状态并入队，再检查条件；
唤醒方更新条件后调用 `wake_up()`，最终由 `try_to_wake_up()` 把任务放回合适 CPU 的 runqueue。

### 8.3 与调度器的接口

创建：`sched_fork()` 初始化实体，`wake_up_new_task()` 首次入队。  
阻塞：任务设置睡眠状态后调用 `schedule()`，`__schedule()` 将其出队并选择下一个任务。  
唤醒：`try_to_wake_up()` 做状态同步、CPU 选择和入队，必要时向远端 CPU 发 IPI。  
退出：`do_task_dead()` 以 `TASK_DEAD` 状态进行最后一次调度，永不返回。

普通公平任务由 fair scheduling class 管理。当前内核以 EEVDF 思路根据虚拟运行时间、
虚拟截止期和资格选择任务；实时、deadline、idle、sched_ext 等类有各自策略。进程管理只通过
统一的 `sched_class` 接口交接，不依赖某一种具体算法。

---

## 9. 退出、僵尸与回收

### 9.1 退出路径

```
exit()/返回 main/致命信号
          │
          ▼
       do_exit()
          ├── 标记 PF_EXITING，协调线程组退出
          ├── 释放 mm、files、fs、namespace、I/O 等资源
          ├── exit_notify()：重设孤儿父进程、通知父进程/pidfd
          ├── 进入 EXIT_ZOMBIE（通常）
          └── do_task_dead()：最后一次 schedule，永不返回

父进程 wait4()/waitid()
          └── 找到 zombie → 读取退出状态/统计 → release_task()
                                      └── 最终释放 task/PID（含 RCU 延迟）
```

`exit()` 只终止当前线程；`exit_group()` 或致命组信号终止整个线程组。用户态 libc 的
`_exit()` 包装在现代 Linux 上通常使用 `exit_group`，而原始 `SYS_exit` 是单线程语义。

### 9.2 为什么需要 zombie

进程退出后，父进程仍需要获得：

- 正常退出码或导致退出的信号；
- 是否产生 core dump；
- CPU 时间、缺页等资源统计；
- 被等待的子 PID。

所以大部分资源可在 `do_exit()` 中释放，但最小身份和统计必须保留。zombie 不再运行，
通常也不持有用户地址空间；它占用的是内核元数据和 PID，而不是一个完整活进程的资源。

### 9.3 `wait()` 做什么

`wait4`、`waitid`、`waitpid` 最终进入共同等待逻辑，扫描符合 PID/PGID、clone 类型和选项的
子任务。若有 zombie，父进程取得信息并触发回收；若有 stopped/continued 子任务则按选项
报告状态；若暂时无事件，则睡在 `signal->wait_chldexit` 等待队列上。

`SIGCHLD` 被设为 `SIG_IGN` 或使用 `SA_NOCLDWAIT` 时，符合条件的子任务可自动回收，
不长期停留为 zombie。

### 9.4 孤儿进程重设父进程

父进程先退出时，内核把孩子交给最近的 child subreaper，若没有则交给相应 PID namespace
的 child reaper（通常是该 namespace 的 PID 1）。服务管理器通过
`PR_SET_CHILD_SUBREAPER` 接管 double-fork 后代，负责统一收尸。

### 9.5 task 的真正释放

`do_exit()` 不能释放当前正在使用的内核栈和 `task_struct`。任务先切走，由其他上下文完成
`release_task()`/`put_task_struct()`。由于 `/proc`、PID 查找和任务遍历存在 RCU 读者，
最终内存释放还可能通过 RCU callback 延迟，确保旧读者离开。

---

## 10. 信号与线程组

### 10.1 信号投递分三步

1. **产生**：异常、终端、定时器、`kill`、`tgkill` 等产生信号；
2. **排队与选择目标**：进入线程私有或线程组共享 pending，选择一个可接收线程；
3. **递送**：任务返回用户态前检查 pending，执行默认动作或构造用户信号栈帧进入 handler。

普通非实时信号通常只保留“至少发生一次”的状态，多次相同信号可能合并；实时信号可排队
并携带 `siginfo`。信号 mask 是每线程属性，所以进程定向信号可由线程组中未屏蔽它的线程处理。

### 10.2 常见发送接口的差异

| 接口 | 目标 |
|------|------|
| `kill(pid, sig)` | 进程/线程组、进程组或广播（由 pid 取值决定） |
| `tgkill(tgid, tid, sig)` | 指定线程组中的指定线程，避免 TID 复用造成跨组误投 |
| `pidfd_send_signal()` | pidfd 指向的稳定进程身份 |

### 10.3 退出协同

`exit_group()` 设置线程组退出状态并向其他线程发出致命信号；各线程仍需在安全路径进入
`do_exit()`，最后一个活线程负责部分组级资源清理。`signal_struct.live` 等计数用于判断
是否为最后线程。

---

## 11. 命名空间、cgroup 与容器

两者解决不同问题：

```
namespace：这个进程能“看见什么”
  PID、mount、net、IPC、UTS、user、cgroup、time

cgroup：这个进程属于哪个资源与控制组
  CPU、memory、I/O、pids、cpuset、freezer 等
```

创建时，`copy_namespaces()` 按 `CLONE_NEW*` 决定共享还是新建 namespace；PID namespace
有层级关系，同一 `struct pid` 的 `numbers[]` 保存每一层可见数字，因此一个任务在宿主机
可能是 PID 30000，在容器中却是 PID 1。

`CLONE_INTO_CGROUP` 允许 `clone3()` 在创建时直接把子任务放入目标 cgroup，避免“先在父
cgroup 中可见/运行，再迁移”的窗口。pids controller 限制的是 cgroup 中 task 的创建量，
它与 `RLIMIT_NPROC`、系统 PID 上限属于不同层次的限制。

PID namespace 的 PID 1 是 child reaper，承担孤儿回收并具有特殊信号语义；它退出时该
namespace 无法继续正常承载进程，内核会清理其中任务。

---

## 12. 并发控制与生命周期安全

进程管理横跨几乎所有子系统，不存在一把“进程大锁”。常见保护手段如下：

| 机制 | 典型保护对象 |
|------|--------------|
| `tasklist_lock` | 全局任务关系、父子链表、线程组与 PID 链接的结构性修改 |
| `sighand->siglock` | 信号 pending、处理动作、job control 和部分线程组状态 |
| runqueue lock | `on_rq`、调度实体及 CPU 运行队列 |
| `task_lock()` | task 的部分资源指针和统计字段 |
| 引用计数 | task、pid、mm、files、cred 等对象所有权 |
| RCU | 任务/PID 查找、父指针和列表的只读遍历及延迟释放 |

### 12.1 查找与持有不是同一件事

```c
rcu_read_lock();
p = find_task_by_vpid(nr);
if (p)
    get_task_struct(p);       /* 若要在 RCU 临界区外继续使用，取得稳定引用 */
rcu_read_unlock();

if (p) {
    /* 使用 p */
    put_task_struct(p);
}
```

实际代码应使用与场景匹配的 helper 并遵守其锁约定。仅在 RCU 下查到指针，不代表退出
临界区后仍可使用；仅保存整数 PID 更不能阻止 PID 复用。

### 12.2 状态转换需要内存序

睡眠/唤醒同时发生在不同 CPU。等待方必须先发布睡眠状态再检查条件，唤醒方必须先发布
条件再检查任务状态。`set_current_state()`、`try_to_wake_up()` 以及等待队列 API 内含所需
屏障和锁协议。随意写 `current->__state` 容易造成经典的 lost wakeup。

### 12.3 锁顺序与退出竞态

任务可能在被观察时退出，线程组 leader 可能在 exec 时变化，ptrace 还会改变 `parent`
关系。因此应优先使用 `get_task_*()`、`pid_task()`、`lock_task_sighand()`、`get_task_mm()`
等 helper，不应根据结构字段自行拼出锁顺序。

---

## 13. 内核线程与特殊任务

### 13.1 内核线程

内核线程也是 `task_struct`，同样由调度器调度，但具有 `PF_KTHREAD`，没有用户地址空间。
常见创建方式是 `kthread_create()`/`kthread_run()`：先由 kthread 基础设施构造任务，再按需
绑定 CPU、设置名字并唤醒。线程函数返回或调用 `kthread_stop()` 协议后进入退出路径。

### 13.2 idle task

每个 CPU 有一个 `swapper/N` idle task。当 runqueue 没有更合适任务时运行它，通常进入
体系结构低功耗指令。idle task 使用静态/特殊 PID 0 语义，不是用户可见的普通进程，
当前仓库通过 `fork_idle()` 和 `init_idle()` 初始化辅助 CPU 的 idle task。

### 13.3 init task 与 kthreadd

- 启动时的 `init_task` 是静态构造的初始 task（PID 0）；
- 内核启动后创建 PID 1，最终执行用户空间 init；
- `kthreadd` 通常为 PID 2，作为很多内核线程创建请求的管理者。

这些任务并不是通过完整的普通用户 `fork + exec` 路径自然产生，启动阶段有专门初始化逻辑。

---

## 14. 观测、调试与常见误区

### 14.1 用户空间观测

```bash
# 展开线程、状态、等待点和调度信息
ps -eLo pid,tid,tgid,ppid,stat,cls,rtprio,pri,psr,wchan:32,comm

# 查看一个进程的线程、状态、内存和 fd
ls /proc/<pid>/task
sed -n '1,80p' /proc/<pid>/status
sed -n '1,80p' /proc/<pid>/sched
cat /proc/<pid>/maps
ls -l /proc/<pid>/fd

# 观察创建、exec、等待与退出系统调用
strace -f -e trace=clone,clone3,fork,vfork,execve,wait4,waitid,exit,exit_group <cmd>
```

`/proc/<pid>/stat` 字段紧凑且包含带空格的 comm，脚本不应天真地按空格固定列切割；优先使用
`/proc/<pid>/status`、成熟库或明确处理括号字段。

### 14.2 内核观测

- tracepoints：`sched_process_fork`、`sched_process_exec`、`sched_process_exit`、
  `sched_switch`、`sched_wakeup`；
- ftrace/perf：分析创建延迟、调度延迟和调用栈；
- eBPF：关联 TGID/TID、cgroup、namespace 与生命周期事件；
- SysRq：系统卡死时查看任务及 blocked task；
- crash/drgn：转储后遍历 task、mm、files、信号与 PID 关系。

### 14.3 常见误区

**误区一：`fork()` 复制了全部内存。** 主要使用 COW；页表和 VMA 元数据仍需创建。

**误区二：线程不是进程，所以内核结构完全不同。** 调度实体都是 task，差异主要在资源共享。

**误区三：`execve()` 创建新 PID。** 它替换当前 task 的程序映像，成功后 PID 通常不变。

**误区四：`TASK_UNINTERRUPTIBLE` 一定是死锁。** 它表示普通信号不打断等待；可能是正常短暂
I/O，也可能因设备故障长期停留，需要结合 `wchan` 和内核栈判断。

**误区五：kill 后进程就立即消失。** 信号可能被屏蔽/处理，任务可能尚未获得运行机会；退出后
还可能以 zombie 等待父进程回收。

**误区六：保存 PID 整数就能长期指向同一进程。** PID 可复用，应使用 pidfd 或正确持有
`struct pid` 引用。

**误区七：进程状态是严格线性状态机。** `__state`、`exit_state`、`on_rq`、`on_cpu` 和
job-control 标志分属不同维度，仅看一个字符会丢失信息。

---

## 15. 关键函数速查

| 阶段 | 函数 | 位置 | 作用 |
|------|------|------|------|
| 创建入口 | `kernel_clone()` | `kernel/fork.c` | fork/vfork/clone/clone3 的共同入口 |
| 核心复制 | `copy_process()` | `kernel/fork.c` | 构造 task，复制/共享资源并发布关系 |
| task 分配 | `dup_task_struct()` | `kernel/fork.c` | 分配并复制 task_struct 与内核栈 |
| 内存复制 | `copy_mm()` / `dup_mm()` | `kernel/fork.c` | 共享或复制地址空间 |
| PID 分配 | `alloc_pid()` | `kernel/pid.c` | 在 PID namespace 层级中分配身份 |
| 首次入队 | `wake_up_new_task()` | `kernel/sched/core.c` | 把新任务放入运行队列 |
| exec 入口 | `do_execveat_common()` | `fs/exec.c` | 参数复制与 exec 公共入口 |
| 格式分派 | `search_binary_handler()` | `fs/exec.c` | 选择 ELF、脚本等 binary handler |
| ELF 装载 | `load_elf_binary()` | `fs/binfmt_elf.c` | 建立 ELF 映射、栈和入口现场 |
| 睡眠切换 | `schedule()` / `__schedule()` | `kernel/sched/core.c` | 选择并切换到下一个 task |
| 唤醒 | `try_to_wake_up()` | `kernel/sched/core.c` | 状态同步、CPU 选择和重新入队 |
| 退出 | `do_exit()` | `kernel/exit.c` | 释放资源、通知父进程并永久切走 |
| 线程组退出 | `do_group_exit()` | `kernel/exit.c` | 协调整个线程组退出 |
| 等待 | `do_wait()` | `kernel/exit.c` | 查找并报告子进程状态 |
| 最终释放 | `release_task()` | `kernel/exit.c` | 摘链并推进 task/PID 最终回收 |
| 信号发送 | `do_send_sig_info()` 等 | `kernel/signal.c` | 权限检查、排队和目标选择 |

### 一条完整生命周期

```
clone3/fork
  → kernel_clone
  → copy_process
  → wake_up_new_task
  → 调度运行 / 睡眠唤醒
  → execve（可选，替换程序映像）
  → do_exit
  → EXIT_ZOMBIE + 通知父进程
  → wait/do_wait
  → release_task
  → 引用归零并经过必要的 RCU 宽限期
  → task_struct、内核栈和 PID 身份最终释放
```

理解 Linux 进程管理的主线，不是背诵 `task_struct` 的所有字段，而是抓住四个不变量：

1. **task 是调度单位，线程组才接近用户所说的进程；**
2. **资源通过 clone flags 选择共享或复制；**
3. **对象先构造后发布，先退出后回收；**
4. **锁保护关系变化，引用计数保护所有权，RCU 保护无锁读者。**

