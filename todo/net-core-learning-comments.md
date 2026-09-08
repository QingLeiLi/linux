# net 核心源码学习注释进度

## 任务目标

从 `net/` 中选取 10 个能串起 Linux 网络栈主干的文件，按固定顺序追加详细中文学习注释。目标是让初学者能够沿着“构建入口 → socket API → 通用 sock → skb → 设备收发 → IP/路由”的路径理解关键对象、调用链、所有权、并发与失败处理。

## 执行规则

- 主标准：`doc/linux-kernel-source-learning-methodology.md`（本任务启动时已完整阅读 1474 行）。
- 只新增中文学习注释；不修改代码，不删除、改写或移动原有注释。
- 严格单文件闭环：一个 `[~]` 文件完成最终复读和第 17 章验收前，不开始下一个文件。
- C/头文件必须通过密度门禁：`--min-density 0.20 --max-code-gap 10`。
- 每个文件记录函数/实体、英文注释、路径、并发与生命周期、关联读取、抽查和修改安全结果。
- 构建文件按其语法、选择关系和构建副作用验收；不机械套用 C 函数门禁。
- 会话中断或上下文压缩后，先读取本文件并核对工作树，以本文件状态恢复进度。

状态：`[ ]` 未开始；`[~]` 正在处理或验收；`[x]` 已完成单文件闭环；`[-]` 经核对后跳过。

## 已确认处理顺序

- [x] 1. `net/Makefile`：建立网络栈目录、内建对象和按配置展开模块的构建地图。
- [x] 2. `net/Kconfig`：理解网络总开关、协议族和核心功能的配置依赖。
- [x] 3. `net/sysctl_net.c`：理解每网络命名空间 sysctl 根目录的注册与生命周期。
- [~] 4. `net/socket.c`：从 socket 系统调用/VFS 入口进入协议族实现。
- [ ] 5. `net/core/sock.c`：理解通用 `struct sock` 的分配、记账、锁和生命周期。
- [ ] 6. `net/core/skbuff.c`：理解 `sk_buff` 的分配、克隆、线性化和释放所有权。
- [ ] 7. `net/core/dev.c`：理解网络设备注册、收包、发包和 softirq/NAPI 主路径。
- [ ] 8. `net/ipv4/af_inet.c`：理解 IPv4 socket 创建、bind/connect 和协议分派。
- [ ] 9. `net/ipv4/route.c`：理解 IPv4 路由缓存、输入/输出查找与异常路径。
- [ ] 10. `net/ipv6/af_inet6.c`：对照 IPv4 理解 IPv6 协议族注册和 socket 生命周期。

## 选取说明

- `net/ipv4/tcp.c` 已有历史提交 `fae7447751ae` 的系统学习注释，本批不重复处理。
- `net/core/net_namespace.c` 当前已有高密度中文学习注释，本批不重复处理。
- `net/core/devres.c`、`net/devres.c`、诊断/proc/sysfs 等辅助文件留待主干闭环后再决定是否扩展。

## 当前文件

### [~] `net/socket.c`

- 当前阶段：首次顺序读取并建立函数/实体/英文注释索引。
- 首次建图：已完整顺序读取原始 3870 行；BSD ctags 识别约 100 个具名函数/宏入口（SYSCALL_DEFINE 重名只保留首项），另有约 18 个 syscall wrapper 需逐项人工纳入。
- 区域进度：已处理文件历史/头文件/全局分派实体、`sock_show_fdinfo()`、协议族名称与 RCU 表、地址跨用户边界，以及 sockfs 的 inode 缓存/联合对象、`SOCKFS_I()`、分配、逐出、最终释放和 SLAB ctor；下一批从 `init_inodecache()` 与 sockfs 操作表/xattr 继续。
- 待完成：sockfs 与 fd 映射、socket/VFS 对象生命周期、时间戳/cmsg、消息收发、ioctl/compat、协议族创建、系统调用主线、协议注册/初始化、kernel socket API、英文注释翻译、密度门禁和第 17 章验收。
- 关联读取：待记录。
- 验收结果：待完成。

## 已完成文件

### [x] `net/Makefile`

- 文件职责：作为 `net/` 顶层 Kbuild 入口，把 socket/core 公共底座、二层设施、协议族和可选网络功能映射为内建对象、模块或递归子目录。
- 路径与语义验收：已逐行复读最终 114 行；覆盖 `obj-y`、`obj-$(CONFIG_*)`、`:=`/`+=`、目录递归及 y/m/n 三态，并解释 LLC 链接顺序、IPv6/DSA 无条件进入和 VLAN 两阶段构建三个特殊点。
- 关联读取：`Documentation/kbuild/makefiles.rst` 的 obj-y/obj-m 与递归目录规则、`net/8021q/Makefile` 的 vlan_core/8021q 拆分、`net/ipv6/Makefile` 的公共 core 与完整 ipv6.o 分派、`net/dsa/Makefile` 的 built-in stubs 与模块核心；均为只读核对，后 3 个文件现有中文学习注释缺失，建议在对应主题扩展时补注。
- 修改安全：新增 37 行、删除 0 行；原 77 行代码与注释逐字保留，`git diff --check` 通过。该文件是 Kbuild 元数据，不适用 C 函数与中文注释密度脚本；未运行完整内核构建，因为工作树没有为本任务准备的配置且纯注释不改变 Kbuild 求值。
- 验收结论：已按方法论第 17 章中适用于构建元数据的项目完成全文件闭环。

### [x] `net/Kconfig`

- 文件职责：定义 NET 总闸门、共享隐藏能力、网络主菜单及各协议子菜单的拼装顺序，并用 depends/select/default 表达可见性和构建依赖。
- 路径与语义验收：逐行复读最终 674 行；核对 126 个 config/menu/source/条件条目，覆盖 bool/tristate、隐藏符号、NET/INET/NETFILTER/WIRELESS 条件域、RPS/RFS/XPS、BQL、page_pool、lwtunnel、failover 和测试配置；27 个英文 help 均有邻接中文翻译与学习补充。
- 关联读取：沿用上一文件对 `Documentation/kbuild/makefiles.rst` 的 Kbuild 三态核对；本文件主要描述配置关系，未额外展开各 source 子文件。
- 修改安全：新增 131 行、删除 0 行，原配置与英文帮助逐字保留；`git diff --check` 通过。checkpatch 为 0 errors/4 warnings，均是原文件已有的短 help/无 help 样式（WIRELESS、LWTUNNEL_BPF、两个 KUnit 项），本次没有改写上游配置来消警告。未运行会生成 `.config` 的配置目标，避免污染工作树。
- 验收结论：已按方法论第 17 章中适用于 Kconfig 元数据的项目完成全文件闭环。

### [x] `net/sysctl_net.c`

- 文件职责：为共享 `/proc/sys/net` 根建立按 current netns 动态选择的 ctl_table_set，映射容器管理员权限/属主，并在非 init netns 注册表前阻断可写全局 data 泄漏。
- 第 17 章验收：最终完整复读 324 行；10 个函数均有紧邻专属契约，两个常驻操作实体和全部参数/关键局部量已说明。英文文件历史、权限、安全校验及降权注释均原样保留并邻接翻译补充。
- 路径与语义验收：可仅凭注释复述 current netns lookup/is_seen、CAP_NET_ADMIN 权限映射、userns uid/gid 映射、顶层空目录加 pernet ops 的两阶段构造/回滚，以及非 init 表逐项只读/堆/全局地址三分支。
- 并发与生命周期：net->sysctls 内嵌并随 netns 初始化/retire；ctl_table 在注销前由注册者保持有效；表发布前降权避免并发改 mode；header 摘除与最终引用/RCU 回收委托通用 sysctl 层。
- 关联读取：`include/linux/sysctl.h` 的 ctl_table_header/set/root 布局（现有中文注释充分），`fs/proc/proc_sysctl.c` 的 lookup、register、setup、retire/unregister 实现（中文注释部分覆盖，建议后续补齐核心函数），`include/net/net_namespace.h` 的 net->sysctls 生命周期（部分覆盖），以及若干 register_net_sysctl_sz 调用点用于确认表 ownership（多数缺失，不纳入本批修改）。
- 修改安全：新增 145 行、删除 0 行，新增非注释语句 0；密度门禁 PASS（code=121、comments=174、chinese=105、density=0.868、max_gap=7），`git diff --check` 通过。checkpatch 为 0 errors/5 warnings，均定位到未改动的原代码声明/可变 ctl_table 设计；忽略 LONG_LINE_COMMENT 后结论相同。工作树无任务专用 `.config`，未执行对象构建或 netns/proc 运行测试。
- 验收结论：已按方法论第 17 章完成强制验收，状态为“全文件完成”。
