# Linux 网络栈内部实现：一次 TCP Echo 如何穿过内核

> 适用内核版本：v7.2-rc1（基于当前仓库 `a14c021eef14`）
>
> 分析范围：IPv4、TCP、普通 Ethernet 网卡、已建立连接的数据收发；同时补足建连、关闭和异常路径
>
> 核心源码：`net/socket.c`、`net/ipv4/`、`net/core/`、`include/net/`、`include/linux/skbuff.h`

---

## 目录

1. [从一个 TCP Echo 开始](#1-从一个-tcp-echo-开始)
2. [网络栈要解决什么问题](#2-网络栈要解决什么问题)
3. [从直观方案推导 Linux 方案](#3-从直观方案推导-linux-方案)
4. [总体方案的收益、代价与边界](#4-总体方案的收益代价与边界)
5. [进入源码前的宏观地图](#5-进入源码前的宏观地图)
6. [核心对象：同一批字节为何需要多种表示](#6-核心对象同一批字节为何需要多种表示)
7. [socket 创建：一次两级函数指针分派](#7-socket-创建一次两级函数指针分派)
8. [connect：从四元组到 ESTABLISHED](#8-connect从四元组到-established)
9. [send：用户字节进入 TCP write queue](#9-send用户字节进入-tcp-write-queue)
10. [TCP 为什么不等于“一次 send 一个包”](#10-tcp-为什么不等于一次-send-一个包)
11. [TCP 输出：序列号、窗口与拥塞窗口共同放行](#11-tcp-输出序列号窗口与拥塞窗口共同放行)
12. [IP 输出：路由缓存、IP 头与 Netfilter](#12-ip-输出路由缓存ip-头与-netfilter)
13. [邻居子系统：下一跳 IP 如何变成 MAC](#13-邻居子系统下一跳-ip-如何变成-mac)
14. [qdisc、多队列与驱动 ownership](#14-qdisc多队列与驱动-ownership)
15. [接收入口：中断为什么不直接跑完整协议栈](#15-接收入口中断为什么不直接跑完整协议栈)
16. [NAPI 与 GRO：限额轮询和批量摊销](#16-napi-与-gro限额轮询和批量摊销)
17. [从 Ethernet 分派到本机 IPv4](#17-从-ethernet-分派到本机-ipv4)
18. [TCP 查找 socket：四元组如何命中连接](#18-tcp-查找-socket四元组如何命中连接)
19. [socket 正被用户占用时：backlog 协议](#19-socket-正被用户占用时backlog-协议)
20. [TCP 接收：顺序队列、乱序队列与唤醒](#20-tcp-接收顺序队列乱序队列与唤醒)
21. [recv：从 receive queue 复制回用户空间](#21-recv从-receive-queue-复制回用户空间)
22. [ACK、重传、流控和拥塞控制](#22-ack重传流控和拥塞控制)
23. [并发、锁、引用、RCU 与内存序](#23-并发锁引用rcu-与内存序)
24. [失败、内存压力和慢速路径](#24-失败内存压力和慢速路径)
25. [close、FIN 与 TIME_WAIT](#25-closefin-与-time_wait)
26. [offload 和旁路为何不改变协议语义](#26-offload-和旁路为何不改变协议语义)
27. [观测实验：把 Echo 映射回源码](#27-观测实验把-echo-映射回源码)
28. [回到案例：两次用户态返回究竟承诺什么](#28-回到案例两次用户态返回究竟承诺什么)
29. [源码阅读路线与不变量](#29-源码阅读路线与不变量)

---

## 1. 从一个 TCP Echo 开始

服务端已经 `listen()`，客户端执行：

```c
int fd = socket(AF_INET, SOCK_STREAM, 0);
connect(fd, &server, sizeof(server));

send(fd, "hello", 5, 0);
recv(fd, buf, 5, MSG_WAITALL);
close(fd);
```

服务端 `accept()` 后执行：

```c
ssize_t n = recv(cfd, buf, sizeof(buf), 0);
send(cfd, buf, n, 0);
```

初学者很容易形成下面这幅图：

```text
send("hello") → 网卡发送一个包 → 对端 recv("hello")
```

它省略了几乎所有真正决定正确性和性能的环节：

- `send()` 交付给内核的是字节流，不是一个网络包；
- TCP 可能合并多次 `send()`，也可能把一次 `send()` 分成多个 segment；
- 发送队列、qdisc 和网卡 ring 是三个不同的排队位置；
- 本机返回 `send() == 5` 时，对端可能尚未收到任何字节；
- 网卡接收后，硬中断只调度 NAPI，完整协议处理通常发生在软中断；
- 到达顺序、序列号顺序和应用读取顺序不是同一个概念；
- 包到 socket 后，用户线程还要被唤醒并获得 CPU；
- ACK 只确认 TCP 字节，不确认服务端应用已经调用 `recv()`；
- 丢包不会让应用直接看到“少一个 packet”，TCP 会重传并维持字节流语义。

本文要回答的是：这 5 个字节在每个阶段由什么对象表示、由谁拥有、何时变得可见、可能在哪里
等待，以及 Linux 用什么协议关闭并发竞态。

---

## 2. 网络栈要解决什么问题

### 2.1 应用需要稳定语义，网络只能提供不稳定数据报

IP 网络可能丢包、重复、乱序、分片或改变路径。TCP 却向应用提供：

```text
有序、无重复、全双工的字节流
```

所以内核必须保存未确认数据、接收乱序数据、计算序列号、安排重传，并在两端速度不一致时实施
流控。

### 2.2 单个 CPU 的简单实现无法扩展到现代网卡

现代 NIC 有多条 TX/RX queue，一个 CPU 上又可能同时处理成千上万条连接。如果所有包都经过一把
全局锁：

- 每次发送和接收都会造成 cacheline bouncing；
- 多队列硬件会被软件串行化；
- 慢连接会阻塞无关连接；
- 中断过多会把 CPU 消耗在上下文切换，而不是有效处理上。

Linux 因而把高频状态拆成 socket 局部状态、per-CPU softnet/NAPI 状态和设备队列局部状态。

### 2.3 网络栈面对的核心矛盾

| 矛盾 | 不能只选择一端的原因 |
|------|----------------------|
| 低延迟 vs 批处理 | 每包立刻中断延迟低但吞吐差；批量太大又饿死其他工作 |
| 零拷贝 vs 生命周期简单 | 引用用户页可少复制，但完成通知、pin 和错误回收更复杂 |
| 流局部性 vs 负载均衡 | 固定 CPU 缓存好；CPU 过载时又需要迁移 |
| 大包处理 vs MTU | 大 skb 可摊销协议开销；线上仍需满足 MTU |
| 无锁查找 vs 更新安全 | 每包都加全局锁不可扩展；设备、路由和 socket 又会并发删除 |
| 吞吐 vs 公平/尾延迟 | qdisc batching 提高吞吐，却可能让短流排队 |
| 发送成功语义 vs 可靠交付 | syscall 不能等待远端应用，否则阻塞时间不可控 |

---

## 3. 从直观方案推导 Linux 方案

### 3.1 方案 A：send 中同步完成所有工作

```text
复制用户数据 → 构造一个包 → 网卡发送 → 等待对端 ACK → send 返回
```

优点是容易解释。问题是 RTT 可能是微秒、毫秒甚至超时级，调用线程会长期阻塞；每个小写入产生
一个包，带宽利用率也很差。对端故障时，`send()` 甚至可能等数分钟。

### 3.2 方案 B：只做异步队列

```text
send → 无限制地放入内核队列 → 立即返回
```

它解决了等待 RTT 的问题，却允许快速生产者耗尽内核内存。还无法决定何时合并、何时发送、丢包
后保存哪些数据。

### 3.3 Linux 的组合方案

```text
socket send buffer
  限制应用可提前交给内核的字节
        ↓
TCP write/retransmit queue
  保存序列号和未确认数据，受 rwnd/cwnd/pacing 控制
        ↓
IP + route + neighbour
  决定路径、下一跳和二层地址
        ↓
qdisc + netdev TX queue
  分类、整形、排队并向多队列 NIC 提交
        ↓
driver/NIC completion
  释放“设备仍在使用”的 skb 引用，但 TCP 原始未确认数据仍可保留
```

接收端则采用：

```text
短硬中断只屏蔽/确认事件并调度 NAPI
→ NAPI 按 budget 轮询 RX ring
→ GRO 合并可合并报文
→ RCU 分派 Ethernet/IP/TCP
→ 按四元组找到 socket
→ socket 锁空闲则立即处理，否则进入 backlog
→ 有序数据进入 receive queue
→ 唤醒 recv/epoll waiter
```

这不是单个“最优算法”，而是一组分阶段队列、局部锁和快速/慢速路径的组合。

---

## 4. 总体方案的收益、代价与边界

### 4.1 收益

- syscall 与 RTT、网卡完成解耦；
- TCP 可跨多次写入进行 MSS/GSO 聚合；
- NAPI 把中断风暴变成有预算的轮询；
- GRO/GSO/TSO 减少每个线速 packet 的软件固定成本；
- RCU 允许接收热路径读取协议表、设备配置和 socket hash；
- 多 TX/RX queue 与 CPU affinity 提供流局部性；
- socket、TCP、IP、qdisc 和驱动各自保持清晰契约。

### 4.2 具体代价

- 同一数据可能同时关联用户页、多个 skb clone、重传队列和设备 DMA；
- `send()`、NIC completion、ACK、对端 `recv()` 是不同完成点；
- batching、GRO 和 qdisc 会隐藏单个线速包，增加 trace 解释难度；
- per-CPU 与 per-queue 设计只提供局部视图，全局公平是近似的；
- fast path 需要大量前置条件，任一异常会进入更长的慢路径；
- route、neighbour、socket 和 netdevice 的并发销毁需要引用与 RCU 配合。

### 4.3 本文边界

主线不展开 IPv6、UDP、MPTCP、IPsec、桥接、容器 veth、隧道和具体 NIC 描述符格式。它们会在
宏观地图的某个分派点插入或替换路径，但不改变本文用于理解普通 TCP/IPv4 的核心模型。

---

## 5. 进入源码前的宏观地图

### 5.1 子系统边界

```text
用户进程
 send/recv/connect
      │
      ▼
socket syscall 层                     net/socket.c
 fd → struct file → struct socket
      │ proto_ops
      ▼
INET socket 层                         net/ipv4/af_inet.c
 地址族分派、autobind、通用 stream 操作
      │ struct proto
      ▼
TCP                                    net/ipv4/tcp*.c
 字节流、seq/ACK、重传、rwnd/cwnd、socket queues
      │
      ▼
IPv4 + route + Netfilter               net/ipv4/ip_*.c
 IP 头、FIB/dst、PRE/LOCAL/POST hooks
      │
      ▼
neighbour                              net/core/neighbour.c
 next-hop IP → MAC，ARP 状态机
      │
      ▼
qdisc / netdevice core                 net/sched/ + net/core/dev.c
 分类、排队、选 TX queue、驱动分派
      │
      ▼
NIC driver / hardware                  drivers/net/ethernet/...
 DMA ring、doorbell、IRQ、NAPI
```

### 5.2 发送和接收主路径

```text
发送：
send
 → __sys_sendto
 → sock_sendmsg / inet_sendmsg
 → tcp_sendmsg / tcp_sendmsg_locked
 → tcp_push / tcp_write_xmit
 → __tcp_transmit_skb
 → ip_queue_xmit / ip_local_out
 → ip_output / ip_finish_output
 → neighbour output
 → dev_queue_xmit / __dev_queue_xmit
 → qdisc → ndo_start_xmit → NIC TX ring

接收：
NIC RX ring
 → IRQ schedules NAPI
 → NET_RX_SOFTIRQ / net_rx_action
 → driver napi->poll
 → napi_gro_receive
 → __netif_receive_skb_core
 → ip_rcv → ip_local_deliver
 → tcp_v4_rcv → tcp_v4_do_rcv
 → tcp_rcv_established
 → sk_receive_queue / sk_data_ready
 → tcp_recvmsg → copy_to_user
```

### 5.3 四条不要混在一起的线

```text
数据线： user iterator → skb data/frags → DMA → peer skb → user buffer
控制线： syscall → protocol → qdisc → IRQ/NAPI → protocol → wakeup
可靠性线： write_seq/snd_nxt → snd_una → ACK/SACK → retransmit
ownership 线： socket → TCP queue → clone/driver → completion/free
```

---

## 6. 核心对象：同一批字节为何需要多种表示

### 6.1 `struct socket` 与 `struct sock`

`struct socket` 是 VFS/系统调用视角的对象，关联 `struct file`、等待队列和 `proto_ops`。
`struct sock` 是协议栈视角的通用 socket，保存状态、收发内存记账、队列、回调和协议操作。

```text
fd table
 └─ file
     └─ private_data → socket
                        ├─ ops → inet_stream_ops
                        └─ sk  → sock
                                  └─ 实际分配对象是 tcp_sock
```

这里体现 C 的“首成员嵌入”式继承：`tcp_sock` 包含更通用的 `inet_connection_sock`/`inet_sock`/
`sock`，`tcp_sk(sk)` 根据布局取回 TCP 专有对象。它不是创建另一个对象，也不是运行时复制。

### 6.2 `struct sk_buff`

skb 不是简单的“一个包数组”。它把元数据和数据存储分开：

```text
sk_buff 元数据
  next/prev       队列链接
  sk              关联 socket（并不总存在）
  dev             当前网络设备
  len/data_len    总长度与非线性长度
  protocol        L3 协议
  headers offsets MAC/network/transport header 位置
  dst             路由结果
  destructor      最后释放时的记账回调
  cb[48]           各协议在不同阶段复用的控制块

数据区域
  linear head + page frags + 可选 frag_list
```

协议通过 push/pull 改变 `data`，并用 header offset 记录各层头部，而不必每经过一层都搬动整个
payload。clone 可以共享数据区，仅复制元数据；写共享头部前必须 copy-on-write。

### 6.3 三组 TCP 队列

```text
write queue / retransmit queue：已交给 TCP、尚需发送或确认的数据
receive queue：                已按序、可交给应用的数据
out_of_order_queue：           序列号在洞之后，暂不能交给应用的数据
socket backlog：               软中断到达时 socket 正被用户上下文占用，延后处理的包
```

backlog 不是 TCP 乱序队列。前者解决执行上下文串行化，后者解决网络报文序列号乱序。

### 6.4 `dst_entry`、`neighbour`、`net_device` 和 qdisc

- `dst_entry/rtable`：一次路由查询结果及输出函数；
- `neighbour`：下一跳协议地址、链路地址和可达状态；
- `net_device`：内核看到的网络接口及其 `netdev_ops`；
- `netdev_queue/Qdisc`：某条发送队列及排队规则；
- `napi_struct`：驱动的一组 RX/TX completion 轮询工作。

它们分离的原因是：路由、邻居可达性、设备生命周期、排队策略和硬件 ring 的变化频率及锁域完全
不同，用一个巨型对象会使每包热路径争用同一状态。

---

## 7. socket 创建：一次两级函数指针分派

`socket(AF_INET, SOCK_STREAM, 0)` 最终进入 `__sock_create()`。它先根据 `AF_INET` 找到
`net_proto_family.create`，由 `inet_create()` 创建 `sock`，再根据 type/protocol 选择 TCP。

两个 operation table 分工不同：

```text
socket->ops = &inet_stream_ops
  面向 socket/VFS：connect、accept、sendmsg、recvmsg、poll...

sk->sk_prot = &tcp_prot
  面向传输协议：sendmsg、recvmsg、close、hash、memory pressure...
```

当前源码中，`inet_stream_ops.sendmsg = inet_sendmsg`；`inet_sendmsg()` 再读取
`sk->sk_prot->sendmsg`，对常见 TCP/UDP 目标使用 `INDIRECT_CALL_2` 优化：

```c
prot = READ_ONCE(sk->sk_prot);
return INDIRECT_CALL_2(prot->sendmsg, tcp_sendmsg, udp_sendmsg,
                       sk, msg, size);
```

这段代码证明：

1. syscall 层不直接硬编码 TCP；
2. 第一级根据 socket 地址族/类型分派，第二级根据具体传输协议分派；
3. `READ_ONCE` 防止编译器把可能并发变化的指针读取拆分或重复；
4. `INDIRECT_CALL_2` 不改变函数指针语义，只给常见目标更易优化的直接分支。

代价是初学者无法仅搜索 `send()` 找到完整调用链，必须同时查看 operation table 的初始化。

---

## 8. connect：从四元组到 ESTABLISHED

### 8.1 用户地址先复制到内核

`__sys_connect()` 使用 `move_addr_to_kernel()` 复制并校验用户地址，再经：

```text
socket->ops->connect
 → inet_stream_connect
 → __inet_stream_connect
 → sk->sk_prot->connect
 → tcp_v4_connect
```

不能在后续协议处理中反复直接解引用用户指针，否则另一个线程可在校验后修改地址，形成
time-of-check/time-of-use 问题。

### 8.2 `tcp_v4_connect()` 建立本地发送上下文

它完成的关键工作不是“发送 SYN”这么简单：

- 校验目标地址并执行路由查询；
- 选择源地址和可能的临时源端口；
- 建立目的地址、端口和 route cache；
- 初始化序列号相关状态；
- 把 socket 插入 established/ehash 查找体系；
- 进入 `TCP_SYN_SENT` 并发送 SYN。

状态可概括为：

```text
TCP_CLOSE
  无远端四元组，无可用连接
      │ route + autobind + hash + SYN
      ▼
TCP_SYN_SENT
  四元组已占用，SYN 需要 ACK，connect 可能睡眠
      │ 收到合法 SYN+ACK，发 ACK
      ▼
TCP_ESTABLISHED
  send/recv 可使用正常快速路径
```

阻塞 `connect()` 等待的是状态变化，不是持锁自旋。非阻塞 fd 通常返回 `-EINPROGRESS`，完成后由
poll/epoll 的可写事件和 `SO_ERROR` 表达结果。

### 8.3 服务端为何需要两级队列

监听 socket 不会为每个初始 SYN 立刻创建完整 `tcp_sock`。典型过程是：

```text
SYN → request_sock / SYN queue
第三次握手 ACK → 创建完整 child socket → accept queue
accept() → 从 accept queue 取走 child，并安装新 fd
```

`request_sock` 降低半连接的内存成本；SYN cookie 在队列压力下把部分状态编码进序列号，代价是可
保存的信息有限且只是一种退化保护，不是正常建连模型。

---

## 9. send：用户字节进入 TCP write queue

### 9.1 syscall 只建立一个 iterator

`send(fd, buf, len, flags)` 复用 `__sys_sendto()`。当前源码先执行：

```c
err = import_ubuf(ITER_SOURCE, buff, len, &msg.msg_iter);
...
return __sock_sendmsg(sock, &msg);
```

`iov_iter` 描述“数据从哪里来、剩多少”，让 TCP 的循环既可处理普通 buffer，也能复用 vectored I/O
逻辑。它没有在 syscall 入口就把全部数据复制一遍。

### 9.2 `tcp_sendmsg()` 先取得 socket 用户锁

```c
lock_sock(sk);
ret = tcp_sendmsg_locked(sk, msg, size);
release_sock(sk);
```

这把锁串行化会改变 TCP 连接状态的进程上下文操作。它可以睡眠，因此与接收软中断使用的
`bh_lock_sock()` 不是同一种取得方式；二者如何协调将在 backlog 一节展开。

### 9.3 普通 copy 发送的关键过程

`tcp_sendmsg_locked()` 的主循环可压缩为：

```text
等待连接建立
→ 计算当前 MSS 和 size_goal
→ 尝试追加到 write queue 尾 skb
→ 空间不足则分配新 skb
→ 从用户 iterator 复制到 page fragment
→ 增加 write_seq 和 skb.end_seq
→ 达到 push 条件时 tcp_push
→ send buffer 满时 sk_stream_wait_memory
```

当前代码用以下两次更新提交字节的 TCP 序列号范围：

```c
WRITE_ONCE(tp->write_seq, tp->write_seq + copy);
TCP_SKB_CB(skb)->end_seq += copy;
```

`write_seq` 表示下一待分配序列号；skb 控制块的 `seq/end_seq` 表示该 skb 覆盖的字节区间。这就是
后续 ACK、SACK 和重传能讨论“字节范围”而不是用户调用次数的基础。

### 9.4 `send()` 返回点的承诺

普通成功返回 `n` 只承诺：

```text
前 n 个用户字节已经被 TCP 接受并记入本 socket 的发送状态；
应用可以复用原 buffer（MSG_ZEROCOPY 等特殊模式另有完成协议）。
```

它不承诺：已经形成线速包、已经进入 NIC、已经被 ACK，或已经被对端应用读取。

### 9.5 send buffer 满时为什么睡眠

如果不限制 write queue，恶意或失速连接可以吃光系统内存。`sk_stream_memory_free()` 失败后，
阻塞 socket 经 `sk_stream_wait_memory()` 等待 ACK/释放产生空间；非阻塞 socket 返回 `-EAGAIN`。

如果调用已经复制了一部分数据，POSIX 风格接口通常优先返回短写，而不是丢掉进度后只返回错误。
因此应用必须循环处理 short write。

---

## 10. TCP 为什么不等于“一次 send 一个包”

TCP 对应用暴露字节流，调用边界没有协议意义：

```text
send("he", 2); send("llo", 3)
```

对端可能一次 `recv()` 得到 5 字节，也可能多次得到 1、2、2 字节。反向也成立：一次 128 KiB
`send()` 会覆盖许多 MSS。

影响形成/发送 skb 的因素包括：

- MSS 与 path MTU；
- `MSG_MORE`、`TCP_CORK` 和 Nagle；
- 当前 send buffer 与 page frag；
- GSO/TSO 能否把大 skb 留到更低层分段；
- 拥塞窗口、接收窗口和 pacing；
- retransmit queue 中已有的数据。

大 skb 的收益是只执行一次较多的 TCP/IP/qdisc 逻辑，最后由软件 GSO 或 NIC TSO 产生多个线速
segment。代价是 trace 中的一个 skb 不再等于一个 wire packet，且设备限制可能导致后续分段。

---

## 11. TCP 输出：序列号、窗口与拥塞窗口共同放行

### 11.1 `tcp_push()` 不保证立刻交给网卡

`tcp_push()` 标记 push 条件并进入 `__tcp_push_pending_frames()`，后者调用 `tcp_write_xmit()`。
`tcp_write_xmit()` 扫描可发送 skb，但至少受以下边界限制：

```text
发送窗口：对端公告的 rwnd，不得覆盖其不可接收序列号
拥塞窗口：cwnd，不得让网络中未确认数据过多
Nagle/CORK：小包是否应等待合并
pacing：即使窗口允许，也可能延后到计划发送时间
TSQ：限制单 socket 在 qdisc/device 中积压的字节
```

所以“write queue 有数据”与“现在允许发送”是两个状态。

### 11.2 发送时为何 clone skb

`__tcp_transmit_skb()` 的注释明确指出，它处理原 skb 的 clone 或重传构造的新副本。TCP 必须保留
未确认数据以便重传，而低层一旦接收 skb 就可能异步消费并释放它。因此常见 ownership 是：

```text
TCP 原始 skb：留在重传相关队列，直到累计 ACK/SACK 允许释放
发送 clone：  交给 IP/qdisc/driver，设备完成后释放
共享数据区：  通过引用计数保证两者存活，写入前遵守 COW
```

### 11.3 构造 TCP header 是一个发布前步骤

`__tcp_transmit_skb()` 执行：

- 根据 `seq/end_seq` 写 TCP 序列号；
- 写 ACK、窗口和 flags；
- 编码 timestamp/SACK 等 options；
- 设置 checksum 或 checksum offload 元数据；
- 给发送 clone 安装 `destructor`，计入 `sk_wmem_alloc`；
- 再调用 address-family 的 queue_xmit。

低层看到 skb 时，TCP 头和所有 offload 元数据必须已经完整。否则驱动可能 DMA 一个半初始化报文。

---

## 12. IP 输出：路由缓存、IP 头与 Netfilter

### 12.1 route cache 是常见快速路径

`__ip_queue_xmit()` 先检查 skb 或 socket 的缓存路由：

```c
rt = skb_rtable(skb);
if (rt)
        goto packet_routed;

rt = dst_rtable(__sk_dst_check(sk, 0));
if (!rt) {
        ...
        rt = ip_route_output_flow(net, fl4, sk);
}
```

同一 TCP flow 的目标通常不变，每包完整查 FIB 浪费巨大；但 route cache 必须带有效性检查，路由
变化后不能无限使用旧结果。

### 12.2 IP 层如何扩展 skb

找到 route 后，IP 使用 `skb_push()` 在现有 TCP 头前暴露 headroom，设置 version、IHL、TOS、
DF、TTL、protocol、源/目的地址和 ID。payload 没有因为“增加 IP 头”被整体复制。

随后：

```text
ip_local_out
 → NF_INET_LOCAL_OUT
 → dst_output
 → ip_output
 → NF_INET_POST_ROUTING
 → ip_finish_output
```

Netfilter hook 可能接受、丢弃、修改、排队或重定向 skb，因此主路径必须把它视为语义扩展点，而
不是一个无副作用日志回调。

### 12.3 MTU 与分片/分段

若 skb 大于设备 MTU，可能由 GSO 在较低层分段；普通 IPv4 数据报也可能分片，但 TCP 通常通过
MSS/PMTU discovery 和 DF 避免依赖网络分片。PMTU 下降会使缓存 MSS 变化，并可能触发重传重分段。

---

## 13. 邻居子系统：下一跳 IP 如何变成 MAC

路由回答“从哪个设备、经哪个下一跳发送”，Ethernet 仍需要目标 MAC。`ip_finish_output2()` 根据
route 找 neighbour，并调用其 output：已解析时通常走 `neigh_connected_output()`；未知或状态需
确认时走 `neigh_resolve_output()`。

状态演进可简化为：

```text
无 neighbour/INCOMPLETE
 → 发 ARP request，待发送 skb 暂存在 neighbour 队列
 → 收到 ARP reply，填 ha(MAC)
 → NUD_REACHABLE，可用缓存 L2 header 快速发送
 → 超时后 STALE/DELAY/PROBE，边发送边重新确认
 → 解析失败，丢弃排队 skb 并传播可达性错误
```

最直观的“每个包都 ARP”没有陈旧缓存问题，但会制造广播风暴和额外延迟。缓存解决热路径成本，
NUD 状态机则承担缓存可能过期的代价。

这里还有一个重要完成点：TCP 数据即使已获发送许可，也可能因为 ARP 尚未完成而停在 neighbour
队列，并未进入 qdisc。

---

## 14. qdisc、多队列与驱动 ownership

### 14.1 `__dev_queue_xmit()` 的契约

当前函数注释明确规定：无论返回值如何，传入 skb 都被消费。调用者不能在返回错误后随意再次使用
该指针。这是 ownership 转移，不只是“调用一个发送函数”。

函数主要完成：

```text
egress Netfilter/tc/BPF
→ 选择 netdev TX queue（缓存映射、XPS 或 flow hash）
→ 取得该 queue 的 qdisc
→ 有 enqueue 方法：进入 qdisc 并由 qdisc run 发送
→ noqueue 设备：满足条件时直接调用驱动
```

### 14.2 为什么需要 qdisc

NIC ring 只知道“还有没有 descriptor”，不知道不同 flow/cgroup 的公平性、整形速率或 AQM。
qdisc 提供排队策略。无 qdisc 直接发送更短，但设备 busy 时缺少受控的软件排队。

```text
直接路径：qdisc 空且可运行 → dev_hard_start_xmit
排队路径：enqueue → later dequeue/run → dev_hard_start_xmit
拥塞路径：队列达到限制/AQM 决策 → drop/ECN
```

### 14.3 到 `ndo_start_xmit` 的 ownership

`dev_hard_start_xmit()` 最终经 `netdev_start_xmit()` 调用：

```c
return ops->ndo_start_xmit(skb, dev);
```

典型结果：

- `NETDEV_TX_OK`：驱动接管 skb，映射 DMA 并挂到 TX ring；完成前不能释放数据；
- `NETDEV_TX_BUSY`：驱动未接管，队列层必须保留/重试；正常驱动应先 stop queue，避免频繁 busy。

驱动收到硬件 TX completion 后解除 DMA mapping、完成 BQL 记账并释放 skb。这个 completion 表示
本机设备不再使用该发送副本，不表示 TCP 已收到 ACK。

### 14.4 XPS、flow hash 和乱序

多队列提高并发，但同一 TCP flow 若随意切换 TX queue，不同 ring 的排队差异可能制造自发乱序。
因此 socket 缓存 queue mapping，skb 的 `ooo_okay` 只在没有旧 payload 积压等条件下允许重新选队。

---

## 15. 接收入口：中断为什么不直接跑完整协议栈

### 15.1 纯中断方案的问题

如果每来一个 packet 都硬中断并处理到 TCP：

- 高包速率会形成 interrupt livelock；
- 硬中断上下文不能睡眠，执行时间又不可控；
- 多个包无法批量回收 descriptor、更新 doorbell 或聚合；
- 用户进程和其他软中断可能长期得不到 CPU。

### 15.2 NAPI 的组合方案

典型驱动 IRQ handler：

```text
确认设备中断原因
→ 屏蔽/抑制该 RX queue 的进一步中断
→ napi_schedule()
→ 返回硬中断
```

`NET_RX_SOFTIRQ` 中的 `net_rx_action()` 取本 CPU `softnet_data.poll_list`，依次调用 NAPI poll，并同时
受 packet budget 和时间 budget 限制。工作未清空则重新调度；清空后 `napi_complete_done()` 允许驱动
重新打开中断。

收益是低负载时仍由中断快速响应，高负载时自动转为批量轮询。代价是 budget 太小会增加轮次，太
大则增加其他任务尾延迟。

---

## 16. NAPI 与 GRO：限额轮询和批量摊销

### 16.1 driver poll 的 ownership

驱动 poll 通常循环：

```text
检查 RX completion descriptor
→ 解除/同步 DMA
→ 构造 skb 或 xdp_buff
→ 补充 checksum/hash/VLAN 等硬件元数据
→ 交给 XDP 或 napi_gro_receive
→ refill RX descriptor
→ work_done 达 budget 时让 NAPI 继续保持 scheduled
```

只有驱动确认硬件已放弃 descriptor 后，CPU 才能安全读取对应 buffer；只有 refill 完成后，NIC 才能
再次 DMA 到新 buffer。这是硬件与内核之间的 ownership 协议。

### 16.2 GRO 解决什么问题

在线上收到多个 MSS-sized TCP segment 时，逐包执行 IP/TCP/socket 路径固定成本很高。GRO 在满足
flow、header、序列号等条件时合并 skb，让上层一次处理更多 payload。

快速路径收益：少做 hash、协议分派、ACK 决策和队列操作。代价与边界：

- 乱序、不同 options、校验异常等条件会 flush；
- 合并后的 skb 仍要保留 segment 数等信息；
- packet capture 的观察点不同，看到的包大小可能不同；
- GRO 是接收侧软件聚合，不等于发送侧 TSO。

---

## 17. 从 Ethernet 分派到本机 IPv4

### 17.1 `__netif_receive_skb_core()` 是 L2 总分派点

该函数在 RCU 读侧临界区语义下依次处理可能的：generic XDP、VLAN、packet taps、tc ingress、
Netfilter ingress、RX handler，然后根据 `skb->protocol` 查 `packet_type`。

对普通 Ethernet IPv4，最终命中注册的 `ip_packet_type.func = ip_rcv`。packet socket/tcpdump、bridge
或 VLAN 会在这里引入额外消费者或重定向。

### 17.2 `ip_rcv()` 先验证，再决定本机还是转发

```text
ip_rcv_core
  检查长度、版本、IHL、checksum，裁剪 padding
→ NF_INET_PRE_ROUTING
→ ip_rcv_finish
  route input lookup
→ dst_input
  本机路由：ip_local_deliver
  转发路由：ip_forward
```

因此“目的 IP 是本机”不是由 TCP 判断，而是 route input 的结果。网络 namespace 也在这一层决定
应使用哪套 FIB、Netfilter 和协议状态。

### 17.3 本机交付

`ip_local_deliver()` 必要时先做 IP fragment reassembly，再经过 `NF_INET_LOCAL_IN`，随后
`ip_protocol_deliver_rcu()` 根据 IP protocol 号找到 TCP 的 handler：`tcp_v4_rcv()`。

---

## 18. TCP 查找 socket：四元组如何命中连接

`tcp_v4_rcv()` 先检查目标包类型、TCP header 长度和 checksum 初始化，再调用 `__inet_lookup_skb()`。

对 established flow，查找键至少涉及：

```text
local IP + local port + remote IP + remote port
以及 network namespace、输入设备/绑定约束
```

查找结果可能是：

- established `sock`：普通数据/ACK；
- `TCP_TIME_WAIT` 对象：旧连接尾部报文；
- `TCP_NEW_SYN_RECV` request：第三次握手附近；
- listening socket：新的 SYN；
- 无 socket：可能发送 RST 或丢弃。

hash 表让每包不必扫描所有 socket；代价是 bind/connect/close 必须维护 hash 可见性和对象生命周期，
查找读侧需要 RCU/引用防止命中后对象立即释放。

---

## 19. socket 正被用户占用时：backlog 协议

这是理解 Linux TCP 并发最关键的一段之一。

### 19.1 具体竞态

```text
进程上下文                         NET_RX softirq
tcp_recvmsg()
lock_sock(sk)
修改 receive queue / rcv_nxt      tcp_v4_rcv() 找到同一 sk
                                  也需要修改 TCP 状态
```

软中断不能等待一把可睡眠锁。若它和进程同时直接修改队列，链表、序列号和内存记账会损坏。

### 19.2 当前代码的选择

`tcp_v4_rcv()` 在取得 BH socket spinlock 后检查：

```c
if (!sock_owned_by_user(sk)) {
        ret = tcp_v4_do_rcv(sk, skb);
} else {
        drop_reason = tcp_add_backlog(sk, skb);
}
```

含义是：

```text
用户未持有 socket：softirq 立即执行完整 TCP receive
用户持有 socket：  softirq 只把 skb 加入 sk_backlog，然后尽快返回
用户 release_sock：在释放 ownership 的协议中处理 backlog
```

这比让软中断自旋等待用户线程更安全，也避免关闭 BH 很长时间。代价是 backlog 需要独立内存上限，
并使“包到达”到“进入 receive queue”之间多一个排队点。

### 19.3 backlog 与 receive queue 再次区分

进入 backlog 的 skb 尚未完成 TCP sequence/window 处理；进入 receive queue 的 skb 已被 TCP 接受为
按序数据，可由用户读取。二者不能通过同一个 queue length 解释。

---

## 20. TCP 接收：顺序队列、乱序队列与唤醒

### 20.1 established header prediction 快速路径

`tcp_rcv_established()` 首先尝试 header prediction。当前源码注释列出的退化条件包括零窗口、乱序、
urgent data、buffer 不足、异常 flags/window/header、双向数据模式变化和异常 option。

快速路径要求典型条件成立：

```text
header 形态与 pred_flags 匹配
seq == rcv_nxt
ACK 不超过 snd_nxt
checksum 正确
数据落在 receive window 内
socket 有可用接收内存
```

满足后直接 `tcp_queue_rcv()`、处理 ACK、安排 ACK，并调用 `tcp_data_ready()`。

### 20.2 顺序数据的状态变化

```text
到达前：rcv_nxt = N
报文：  seq=N, end_seq=N+5, payload="hello"

验证并计入接收内存
→ skb 进入 sk_receive_queue
→ rcv_nxt = N+5
→ ACK 可确认到 N+5
→ sk_data_ready 唤醒等待者
```

`rcv_nxt` 表示下一个期望序列号。只有连续字节才能推进它。

### 20.3 乱序数据为什么不能直接交付

若先收到 `[N+5,N+10)`：

```text
receive queue:      仍只到 N
out_of_order_queue: [N+5,N+10)
rcv_nxt:            N
ACK/SACK:           累计 ACK=N，可用 SACK 告诉发送端已收到后块
```

随后 `[N,N+5)` 到达，`tcp_ofo_queue()` 才能把连续区间并入 receive queue 并推进 `rcv_nxt`。如果先
把后块交给应用，就破坏 TCP 有序字节流语义。

### 20.4 唤醒不等于立即运行

`sock_def_readable()` 在 RCU 下取得 `sk_wq`，对 sleeper 执行：

```c
wake_up_interruptible_sync_poll(&wq->wait,
        EPOLLIN | EPOLLPRI | EPOLLRDNORM | EPOLLRDBAND);
```

这只把等待任务变为 runnable 或通知 epoll；调度器何时让它运行取决于 CPU、优先级和调度状态。
因此接收延迟至少可拆成 NIC→NAPI、协议处理、wakeup→run、recv copy 四段。

---

## 21. recv：从 receive queue 复制回用户空间

接收系统调用的主路径与发送对称：

```text
recvfrom/recvmsg
→ sock_recvmsg
→ inet_recvmsg
→ sk->sk_prot->recvmsg
→ tcp_recvmsg
→ tcp_recvmsg_locked
```

`tcp_recvmsg()` 可在队列为空且配置允许时先 busy poll NAPI，以 CPU 换更低延迟；普通路径随后
`lock_sock()`，从 `sk_receive_queue` 找覆盖当前 `copied_seq` 的 skb，把 payload 复制到用户 iterator。

关键状态不是简单地“删除一个包”：

```text
copied_seq 前进 n
→ skb 全消费则从 receive queue 移除并释放接收内存
→ 部分消费则保留剩余字节
→ tcp_cleanup_rbuf 评估是否应发送 window update/ACK
```

如果没有足够数据：阻塞 recv 将发布等待状态并睡眠；`MSG_DONTWAIT` 返回 `-EAGAIN`；信号可能导致
中断；`MSG_WAITALL` 也不意味着永不短读，EOF、错误或信号仍可提前结束。

应用读取释放接收 buffer 后，本端公告窗口可能扩大。也就是说，对端能否继续发送不仅取决于网络
拥塞，也取决于本应用是否及时 `recv()`。

---

## 22. ACK、重传、流控和拥塞控制

### 22.1 四个容易混淆的序列变量

```text
write_seq：应用数据已分配到的末端
snd_nxt：  已经发送过的下一个序列号
snd_una：  尚未累计确认的最早序列号
rcv_nxt：  本端下一个期望接收序列号
```

典型发送状态：

```text
snd_una <= snd_nxt <= write_seq

[snd_una, snd_nxt)    已发送、未累计确认
[snd_nxt, write_seq)  在 TCP 中排队、尚未发送
```

### 22.2 ACK 做了什么

合法 ACK 推进 `snd_una` 后，TCP 可以：

- 从重传队列释放已确认范围；
- 降低 socket write-memory 压力并唤醒阻塞 writer；
- 更新 RTT/RTO；
- 更新 delivery rate 与拥塞控制状态；
- 允许 cwnd 中出现新的发送空间。

ACK 表示对端 TCP 已接收对应字节，不表示对端应用已经消费。

### 22.3 rwnd 与 cwnd 解决不同问题

| 窗口 | 防止什么 | 由谁反馈 |
|------|----------|----------|
| receive window, rwnd | 压垮对端 socket 接收缓存 | 接收端 TCP header window |
| congestion window, cwnd | 向网络注入过多在途数据 | 发送端根据 ACK/loss/ECN 算法维护 |

实际可发送上限受二者较小者以及 pacing 等约束。把 cwnd 解释为“对端 buffer”会导致错误诊断。

### 22.4 丢包如何变成重传

TCP 可从重复 ACK/SACK、RACK 等信号推断丢失并快速重传；没有足够反馈时由 retransmission timer
超时处理。重传再次 clone/构造 skb 并走 IP/设备路径，但使用相同字节序列范围。

退化代价是 head-of-line blocking：即使后续字节已经到达，缺失洞之前的数据未补齐，普通 TCP
字节流也不能把后续部分交给应用。

---

## 23. 并发、锁、引用、RCU 与内存序

### 23.1 不存在一把“网络栈总锁”

| 机制 | 主要保护/保证 | 不能替代什么 |
|------|---------------|--------------|
| `lock_sock` | 进程上下文串行修改一个 socket，可睡眠 | 不能让 softirq 睡眠等待 |
| `bh_lock_sock` | softirq 与 socket 状态短临界区 | 不能跨长时间用户复制持有 |
| backlog | 用户持锁时延后 softirq 工作 | 不是网络乱序队列 |
| skb refcount | clone 共享数据区的生命周期 | 不保护 TCP 状态机 |
| sock refcount | 查找者使用期间 socket 不释放 | 不串行字段修改 |
| RCU | 协议表、设备/handler、hash 查找等读侧生命周期 | 不自动保证字段一致快照 |
| qdisc/txq lock | 某条发送队列的不变量 | 不保护别的 socket/queue |
| DMA barrier/API | CPU 与设备观察 descriptor/data 的顺序 | 不提供 TCP 可靠性 |

### 23.2 socket lookup 的生命周期窗口

```text
CPU A: tcp_v4_rcv 从 hash 找到 sk
CPU B: close 正在 unhash 并减少最后引用
```

仅使用 RCU 可以防止底层内存立刻回收，但处理包可能越过 RCU 临界区，所以查找路径按结果类型和
场景取得 socket 引用。close 必须先阻止新查找，再等待已有引用/RCU reader 离开，最后才能释放。

### 23.3 `sk_wq` 为什么使用 RCU

接收软中断可能正执行 `sk_data_ready`，另一个线程同时 close/替换等待队列关联。读者通过
`rcu_dereference(sk->sk_wq)` 在 RCU 临界区内使用对象，更新/销毁侧不能立即释放旧对象。

### 23.4 锁、引用和内存屏障各回答不同问题

```text
锁：      谁能同时改变状态？
引用：    使用期间对象会不会消失？
RCU：     无锁读者看到旧指针时，旧对象多久仍有效？
屏障：    CPU/NIC 以什么先后顺序观察 descriptor 与数据？
```

写网络代码时只说“这里线程安全”是不够的，必须分别回答四个问题。

---

## 24. 失败、内存压力和慢速路径

### 24.1 发送侧失败不是都能同步返回给 send

| 发生位置 | 典型表现 |
|----------|----------|
| 用户参数/fd/状态检查 | 当前 syscall 直接 `-EFAULT/-EBADF/-EPIPE` 等 |
| send buffer 暂满 | 阻塞等待或 `-EAGAIN`，也可能短写 |
| 同步 route 查询失败 | 可能返回 `-EHOSTUNREACH` 等 |
| 数据已排队后的异步错误 | 写入 socket error，后续 syscall/poll/`SO_ERROR` 观察 |
| qdisc/NIC drop | TCP 通常由 ACK 缺失推断并重传，原 send 已经返回 |

这解释了为什么不能把“某次 send 返回成功”与“该报文没有在本机稍后丢弃”画等号。

### 24.2 接收内存压力

`tcp_data_queue()` 在顺序数据进入队列前执行 receive-memory 调度。空间不足时可能：

- 收缩公告窗口，甚至通告 zero window；
- prune/collapse 乱序队列；
- 丢弃新报文并等待发送端重传；
- 增加协议内存压力统计；
- 唤醒应用尽快读取。

丢包在这里仍不能让应用看到字节流空洞；可靠性由发送端重传恢复，代价是延迟和吞吐下降。

### 24.3 快速路径常见退化表

| 快速路径 | 退化条件 | 慢速路径代价 |
|----------|----------|--------------|
| cached route | 路由失效/策略变化 | FIB rule/table 查询 |
| neighbour connected output | MAC 未知/NUD 需确认 | ARP、排队、probe |
| TCP header prediction | 乱序、options、窗口异常等 | RFC 状态机完整验证 |
| immediate socket receive | 用户正持 `lock_sock` | backlog 排队 |
| qdisc direct run | queue busy/整形 | enqueue 和稍后调度 |
| interrupt moderation + NAPI complete | 持续满载 | 多轮 softirq/ksoftirqd |
| GRO merge | flow/header/seq 不兼容 | flush 并逐 skb 处理 |

---

## 25. close、FIN 与 TIME_WAIT

### 25.1 close 不是“立即删除连接”

主动正常关闭大致经历：

```text
应用 close
→ fd/file 引用释放
→ TCP 排队/发送 FIN
→ FIN_WAIT1（等待 FIN 被确认）
→ FIN_WAIT2（等待对端 FIN）
→ TIME_WAIT（吸收旧报文并允许重发最后 ACK）
→ 从 hash/定时器体系移除并最终释放
```

对端收到 FIN 后，`recv()` 在已排队数据读完后返回 0；这表示有序字节流 EOF，不是错误。

### 25.2 为什么 TIME_WAIT 通常属于主动关闭方

最后 ACK 可能丢失，对端会重发 FIN；TIME_WAIT 让本端还能重发 ACK，并防止旧连接的延迟报文被
相同四元组的新连接误收。代价是临时占用四元组和小型 timewait 状态。

### 25.3 RST 与 FIN 不同

FIN 是有序关闭，占一个序列号，FIN 前数据仍可正常交付。RST 是异常终止，可能让未读数据对应的
应用操作得到 `ECONNRESET`。使用 `SO_LINGER` 的特定配置可能让 close 走 abort/RST 语义。

---

## 26. offload 和旁路为何不改变协议语义

### 26.1 checksum offload、TSO 与 GRO

```text
checksum offload：内核提供伪头/offset 元数据，由 NIC 计算或验证校验和
TSO：              大 TCP skb 由 NIC 分成 MTU-sized segments
GSO：              软件在较低层执行类似分段
GRO：              接收侧把可合并 segments 组合成大 skb
```

它们改变“每次软件处理多少线速包”，不改变序列号、ACK、重传和应用字节流语义。

### 26.2 XDP 的位置与边界

native XDP 可在驱动刚取得 RX buffer、构造完整 skb 之前执行 DROP/PASS/TX/REDIRECT。它减少 skb
分配和协议栈成本，适合过滤、负载均衡等，但 XDP_PASS 后仍需走本文主线；若程序直接 redirect，
则不会进入本机 TCP socket。

### 26.3 `MSG_ZEROCOPY` 不代表“完全没有复制”

它主要避免 payload 从用户页复制到内核发送页，但仍需构造 skb 元数据和协议头，并 pin 用户页直到
异步完成。应用必须从 error queue 接收 completion 后才能安全复用相应内存，失败还可能回退复制。

---

## 27. 观测实验：把 Echo 映射回源码

### 27.1 准备服务端和客户端

可先使用系统自带工具，避免测试程序掩盖主线：

```bash
# 终端 1
nc -l 127.0.0.1 9090

# 终端 2
printf hello | nc 127.0.0.1 9090
```

loopback 不经过物理 NIC/ARP，适合先验证 TCP/socket，再换成另一台主机的地址观察 Ethernet/NAPI。

### 27.2 观察连接状态和队列

```bash
ss -tinp '( sport = :9090 or dport = :9090 )'
```

重点观察：

- `ESTAB`/`TIME-WAIT` 状态；
- Send-Q/Recv-Q；
- `cwnd`、`rtt`、`rto`、retrans；
- pacing/delivery rate（系统和版本支持时）。

在服务端暂不读取、客户端持续发送，可以观察 Recv-Q 增长、接收窗口收缩和客户端 Send-Q/阻塞。

### 27.3 用 tracepoint 区分阶段

先查看本机可用事件，避免假设配置：

```bash
sudo trace-cmd list -e | grep -E '^(sock|tcp|net|napi):'
```

常见可组合观察点包括：

```text
sock:inet_sock_set_state      connect/close 的 TCP 状态变化
tcp:tcp_probe                 TCP 接收/拥塞相关状态
tcp:tcp_retransmit_skb        重传发生点
net:net_dev_queue             skb 进入设备发送路径
net:net_dev_start_xmit        交给驱动
net:net_dev_xmit              驱动发送调用返回
napi:napi_poll                NAPI poll 批次
net:netif_receive_skb         进入通用接收层
skb:skb_copy_datagram_iovec   接收数据复制到用户（若当前内核提供）
```

事件名受配置影响，应以 `trace-cmd list` 为准。实验的目标不是“收集越多越好”，而是把同一 flow 的
时间线分成 syscall/TCP、设备发送、NAPI 接收、socket 唤醒几段。

### 27.4 观察线速包与 offload 差异

```bash
sudo ethtool -k eth0
sudo tcpdump -ni eth0 tcp port 9090
```

若抓包看到超过 MTU 的 skb 或校验和看似错误，先检查抓包点位于 TSO/checksum offload 之前；不要
立刻得出“线上发送了超大包/坏校验和”的结论。可在隔离测试环境临时关闭相关 offload 对比，但不应
在生产接口随意修改。

### 27.5 构造丢包和延迟验证重传

在独立 network namespace/veth 实验环境中使用 `tc netem` 注入 delay/loss，比修改真实网卡安全。
预期关系是：

```text
注入 loss
→ tcp_retransmit_skb 增加
→ snd_una 停顿、Send-Q 可能增长
→ cwnd/发送速率下降
→ 应用仍收到连续字节流，但完成时间增加
```

### 27.6 验证“唤醒不等于运行”

同时记录网络接收事件和调度事件：

```text
netif_receive_skb / tcp receive trace
→ sched_wakeup（服务端线程）
→ sched_switch（服务端真正运行）
→ recv 返回
```

两事件时间差就是 wake-to-run 延迟的一部分，它属于调度而不是 TCP 处理耗时。

---

## 28. 回到案例：两次用户态返回究竟承诺什么

### 28.1 客户端 `send("hello")`

```text
用户 buffer
→ iov_iter
→ TCP write skb/page frag
→ write_seq 增加 5
→ tcp_push 尝试发送
→ send 返回 5
```

返回时最低保证是 TCP 接受了 5 字节。随后它可能因 cwnd、rwnd、pacing、ARP、qdisc 或 NIC ring
等待；丢包后还可能重传。

### 28.2 服务端网卡收到数据

```text
RX DMA complete
→ IRQ schedules NAPI
→ NAPI/GRO
→ Ethernet → IPv4 → TCP
→ 四元组查找 child socket
→ 若用户锁空闲，立即 TCP 处理；否则 backlog
→ 顺序数据进入 receive queue，rcv_nxt += 5
→ sk_data_ready 唤醒服务端
```

### 28.3 服务端 `recv()` 与 echo `send()`

服务端获得 CPU 后，从 receive queue 复制 5 字节；读取可能扩大 rwnd。echo 的 `send()` 创建反向
TCP 序列空间的数据，它与原方向的 ACK 是不同语义，尽管实现可能把 ACK piggyback 到数据报上。

### 28.4 客户端 `recv()` 返回

反向数据经历同样过程，客户端 `tcp_recvmsg()` 复制后返回 5。此时能确认的是客户端应用已读取 echo；
不能仅从这个返回反推原方向每一层具体用了多少 skb 或 wire packet，因为合并、GSO/GRO 都可能改变
表示粒度。

---

## 29. 源码阅读路线与不变量

### 29.1 建议按一条纵向路径阅读

第一遍只恢复控制骨架：

```text
net/socket.c
  __sys_sendto / sock_sendmsg
net/ipv4/af_inet.c
  inet_sendmsg / inet_recvmsg
net/ipv4/tcp.c
  tcp_sendmsg_locked / tcp_recvmsg_locked
net/ipv4/tcp_output.c
  tcp_write_xmit / __tcp_transmit_skb
net/ipv4/ip_output.c
  __ip_queue_xmit / ip_output / ip_finish_output2
net/core/neighbour.c
  neigh_resolve_output / neigh_connected_output
net/core/dev.c
  __dev_queue_xmit / net_rx_action / __netif_receive_skb_core
net/ipv4/ip_input.c
  ip_rcv / ip_local_deliver
net/ipv4/tcp_ipv4.c
  tcp_v4_rcv / tcp_v4_do_rcv
net/ipv4/tcp_input.c
  tcp_rcv_established / tcp_data_queue
```

第二遍追踪对象和 ownership：`sk`、write/receive/backlog queue、skb clone、dst/neighbour、qdisc、
driver completion。第三遍才集中分析 socket 锁、RCU、refcount、softirq 和 DMA 顺序。

### 29.2 阅读时必须保持的十个不变量

1. TCP 序列号描述字节，不描述 syscall 或 skb；
2. `send()` 成功不等于对端收到，更不等于对端应用读取；
3. 未确认 TCP 数据必须在某种可重传形式下继续存活；
4. 低层接管 skb 后，上层不能继续无引用地使用该发送副本；
5. 只有连续且在窗口内的数据才能推进 `rcv_nxt` 并交付字节流；
6. backlog 解决执行上下文冲突，out-of-order queue 解决序列空间空洞；
7. 唤醒只改变任务可运行性，不保证用户线程已经运行；
8. route、neighbour、qdisc 和 NIC ring 是不同的等待与失败点；
9. 锁、引用、RCU 和内存屏障提供不同保证，不能互相替代；
10. GSO/TSO/GRO 改变处理粒度，但不能改变 TCP 对应用承诺的语义。

### 29.3 用开发者视角检验理解

读完后应能推理：

- 若删除 socket backlog 协议，用户 `recv()` 与 softirq 会破坏哪些字段？
- 若驱动在 TX completion 前释放 skb/page，DMA 可能读到什么？
- 为什么 ACK 到达会唤醒 send-buffer 满的 writer？
- 为什么后到的连续段可以进入 receive queue，先到的乱序段却不行？
- 为什么 qdisc drop 通常不能让早已返回的 `send()` 同步报错？
- 为什么同一 flow 随意迁移 TX queue 可能降低 TCP 性能？
- 为什么 close 后仍可能在 hash/RCU/timewait 路径看到连接相关对象？

如果只能背出调用链，不能回答这些“删除一项机制会怎样”的问题，说明还没有真正恢复代码背后的
不变量。

最终可以把完整过程压缩为：

```text
应用交付字节
→ TCP 为字节分配序列空间并保留重传能力
→ IP/邻居选择路径和下一跳
→ qdisc/驱动异步交给硬件
→ 对端 NAPI 批量收包
→ IP/TCP 验证、查找和重排
→ socket queue 发布可读状态
→ 调度器让应用运行并取走字节
→ ACK、窗口和拥塞控制反向约束下一轮发送
```

这条闭环同时解释了可靠性、吞吐、并发和用户可见语义，也是继续阅读 Linux 网络源码的最小稳定
认知地图。
