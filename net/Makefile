# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the linux networking.
# 本文件是网络子系统的顶层 Kbuild 清单：它不描述协议运行时逻辑，而是决定进入哪些子目录、
# 哪些对象链接进 vmlinux，以及哪些对象生成可装卸模块。阅读网络源码时可把它当作目录地图。
#
# 2 Sep 2000, Christoph Hellwig <hch@infradead.org>
# Rewritten to use lists instead of if-statements.
# 2000 年 9 月 2 日改写为列表式 Kbuild 描述，不再为每个配置项编写命令式 if；这样 y/m/n
# 三态由 obj-$(CONFIG_*) 统一映射，新增协议通常只需向对应列表追加目标。
#

# obj-y 表示只要进入 net/ 就参与内建链接。devres.o 管理网络设备的 devres 辅助资源，
# socket.o 提供 socket 系统调用与 VFS 桥接，core/ 则递归构建 skb、sock、net_device 等公共底座。
# “:=”在此立即建立首组清单；后面的“+=”继续按链接顺序追加，目录尾部“/”表示递归进入其 Makefile。
obj-y				:= devres.o socket.o core/

# 兼容层仅在 CONFIG_COMPAT=y 时内建、为 m 时生成模块；它负责 32 位用户 ABI 与本机 ABI 的转换。
obj-$(CONFIG_COMPAT)		+= compat.o

# LLC has to be linked before the files in net/802/
# LLC 必须先于 net/802/ 中的对象链接；obj-y 列表顺序会传递到 built-in.a，因而这里的先后是符号
# 初始化/解析约束，不是仅为排版。随后这组目录构成始终需要访问的二层、调度、netlink、BPF 与 ethtool 基础设施。
obj-$(CONFIG_LLC)		+= llc/
obj-y				+= ethernet/ 802/ sched/ netlink/ bpf/ ethtool/

# 第一组按功能开关进入的目录：包过滤、IPv4、TLS、IPsec/XFRM、Unix socket 和 PSP。
# CONFIG 值为 y 时子目录的内建对象汇入 vmlinux，为 m 时只收集其模块目标，为空/n 时完全跳过。
obj-$(CONFIG_NETFILTER)		+= netfilter/
obj-$(CONFIG_INET)		+= ipv4/
obj-$(CONFIG_TLS)		+= tls/
obj-$(CONFIG_XFRM)		+= xfrm/
obj-$(CONFIG_UNIX)		+= unix/
obj-$(CONFIG_INET_PSP)		+= psp/

# 即使 CONFIG_IPV6=n 也进入 ipv6/，因为该目录还提供 IPv4 会复用的校验、扩展头和协议公共对象；
# 真正完整的 ipv6.o 仍由子目录中的 obj-$(CONFIG_IPV6) 控制，不会因此无条件启用 IPv6 协议栈。
obj-y				+= ipv6/

# 这一组覆盖原始 packet socket、密钥管理、桥接、devlink 与 DSA。dsa/ 无条件进入是为了让其
# 子 Makefile 在 DSA=y/m 时把内建 stubs 和核心/标签模块按正确方式拆分，而不是无条件生成 DSA 功能。
obj-$(CONFIG_PACKET)		+= packet/
obj-$(CONFIG_NET_KEY)		+= key/
obj-$(CONFIG_BRIDGE)		+= bridge/
obj-$(CONFIG_NET_DEVLINK)	+= devlink/
obj-y				+= dsa/

# 较独立的协议族和传输基础设施各自保持 Kconfig 三态：传统 WAN、CAN、Bluetooth、RPC、RxRPC、
# KCM、流解析器、ATM、L2TP 与 Phonet 只有在被选择时才进入，避免关闭功能时产生孤立 built-in 对象。
obj-$(CONFIG_X25)		+= x25/
obj-$(CONFIG_LAPB)		+= lapb/
obj-$(CONFIG_CAN)		+= can/
obj-$(CONFIG_BT)		+= bluetooth/
obj-$(CONFIG_SUNRPC)		+= sunrpc/
obj-$(CONFIG_AF_RXRPC)		+= rxrpc/
obj-$(CONFIG_AF_KCM)		+= kcm/
obj-$(CONFIG_STREAM_PARSER)	+= strparser/
obj-$(CONFIG_ATM)		+= atm/
obj-$(CONFIG_L2TP)		+= l2tp/
obj-$(CONFIG_PHONET)		+= phonet/

# VLAN 的子目录同时含必须内建的 vlan_core.o 和可模块化的 8021q.o。只要配置不是空值（y 或 m），
# 顶层就以 obj-y 进入目录；子 Makefile 再用 subst m,y 保证公共 core 内建，并按原三态生成 8021q.o。
# 若直接写 obj-$(CONFIG_VLAN_8021Q) += 8021q/，配置为 m 时子目录内的 obj-y 会成为孤立对象。
ifneq ($(CONFIG_VLAN_8021Q),)
obj-y				+= 8021q/
endif

# SCTP、RDS、无线配置与 MAC80211、TIPC、NetLabel、IUCV、SMC、RFKill、9P 和 DCB 是建立在前述
# 公共底座上的可选上层；这里仅决定构建可见性，具体依赖关系与用户可选性由各自 Kconfig 保证。
obj-$(CONFIG_IP_SCTP)		+= sctp/
obj-$(CONFIG_RDS)		+= rds/
obj-$(CONFIG_WIRELESS)		+= wireless/
obj-$(CONFIG_MAC80211)		+= mac80211/
obj-$(CONFIG_TIPC)		+= tipc/
obj-$(CONFIG_NETLABEL)		+= netlabel/
obj-$(CONFIG_IUCV)		+= iucv/
obj-$(CONFIG_SMC)		+= smc/
obj-$(CONFIG_RFKILL)		+= rfkill/
obj-$(CONFIG_NET_9P)		+= 9p/
obj-$(CONFIG_DCB)		+= dcb/

# 低功耗无线协议分两层：6LoWPAN 提供适配层，IEEE 802.15.4 提供协议核心，MAC802154 提供 MAC；
# 三个独立配置项允许其他承载复用 6LoWPAN，也允许只构建 802.15.4 的部分层次。
obj-$(CONFIG_6LOWPAN)		+= 6lowpan/
obj-$(CONFIG_IEEE802154)	+= ieee802154/
obj-$(CONFIG_MAC802154)		+= mac802154/

# sysctl_net.o 是本批下一阶段的全局控制面入口，仅在 sysctl 框架存在时编译。其后的解析器、
# 分布式存储/网状网络、NFC、采样、IFE、虚拟交换机与 vsock 都以各自配置三态决定链接形态。
obj-$(CONFIG_SYSCTL)		+= sysctl_net.o
obj-$(CONFIG_DNS_RESOLVER)	+= dns_resolver/
obj-$(CONFIG_CEPH_LIB)		+= ceph/
obj-$(CONFIG_BATMAN_ADV)	+= batman-adv/
obj-$(CONFIG_NFC)		+= nfc/
obj-$(CONFIG_PSAMPLE)		+= psample/
obj-$(CONFIG_NET_IFE)		+= ife/
obj-$(CONFIG_OPENVSWITCH)	+= openvswitch/
obj-$(CONFIG_VSOCKETS)	+= vmw_vsock/

# 最后一组是较新的转发、隧道与控制设施：MPLS/NSH、冗余以太网、switchdev/L3 master、QRTR、
# NCSI、AF_XDP、MPTCP、MCTP、握手卸载和整形。它们共享 core 层，但不会因列在这里而自动选择依赖。
obj-$(CONFIG_MPLS)		+= mpls/
obj-$(CONFIG_NET_NSH)		+= nsh/
obj-$(CONFIG_HSR)		+= hsr/
obj-$(CONFIG_NET_SWITCHDEV)	+= switchdev/
obj-$(CONFIG_NET_L3_MASTER_DEV)	+= l3mdev/
obj-$(CONFIG_QRTR)		+= qrtr/
obj-$(CONFIG_NET_NCSI)		+= ncsi/
obj-$(CONFIG_XDP_SOCKETS)	+= xdp/
obj-$(CONFIG_MPTCP)		+= mptcp/
obj-$(CONFIG_MCTP)		+= mctp/
obj-$(CONFIG_NET_HANDSHAKE)	+= handshake/
obj-$(CONFIG_NET_SHAPER)	+= shaper/
