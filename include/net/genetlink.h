/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __NET_GENERIC_NETLINK_H
#define __NET_GENERIC_NETLINK_H

#include <linux/net.h>
#include <net/netlink.h>
#include <net/net_namespace.h>
#include <uapi/linux/genetlink.h>

/*
 * Generic Netlink 在 nlmsghdr 后增加 genlmsghdr，以 family/command 分派请求，
 * 以 nla_policy 校验 TLV。典型发送生命期：genlmsg_new() -> genlmsg_put*() ->
 * nla_put*() -> genlmsg_end() -> unicast/multicast；失败则 cancel/free。发送函数
 * 通常消费 skb，调用后无论成功失败都不可复用。
 */

#define GENLMSG_DEFAULT_SIZE (NLMSG_DEFAULT_SIZE - GENL_HDRLEN)

/* Non-parallel generic netlink requests are serialized by a global lock. */
/* 未声明 parallel_ops 的 family 请求由这把全局锁串行化。 */
void genl_lock(void);
void genl_unlock(void);

#define MODULE_ALIAS_GENL_FAMILY(family) \
 MODULE_ALIAS_NET_PF_PROTO_NAME(PF_NETLINK, NETLINK_GENERIC, "-family-" family)

/* Binding to multicast group requires %CAP_NET_ADMIN */
/* 订阅该组要求目标网络命名空间中的 CAP_NET_ADMIN。 */
#define GENL_MCAST_CAP_NET_ADMIN	BIT(0)
/* Binding to multicast group requires %CAP_SYS_ADMIN */
/* 订阅该组要求更强的 CAP_SYS_ADMIN。 */
#define GENL_MCAST_CAP_SYS_ADMIN	BIT(1)

/**
 * struct genl_multicast_group - generic netlink multicast group
 * @name: name of the multicast group, names are per-family
 * @flags: GENL_MCAST_* flags
 * 组名只需在 family 内唯一，flags 定义订阅者所需能力。
 */
struct genl_multicast_group {
	char			name[GENL_NAMSIZ];
	u8			flags;
};

struct genl_split_ops;
struct genl_info;

/**
 * struct genl_family - generic netlink family
 * @hdrsize: length of user specific header in bytes
 * @name: name of family
 * @version: protocol version
 * @maxattr: maximum number of attributes supported
 * @policy: netlink policy
 * @netnsok: set to true if the family can handle network
 *	namespaces and should be presented in all of them
 * @parallel_ops: operations can be called in parallel and aren't
 *	synchronized by the core genetlink code
 * @pre_doit: called before an operation's doit callback, it may
 *	do additional, common, filtering and return an error
 * @post_doit: called after an operation's doit callback, it may
 *	undo operations done by pre_doit, for example release locks
 * @bind: called when family multicast group is added to a netlink socket
 * @unbind: called when family multicast group is removed from a netlink socket
 * @module: pointer to the owning module (set to THIS_MODULE)
 * @mcgrps: multicast groups used by this family
 * @n_mcgrps: number of multicast groups
 * @resv_start_op: first operation for which reserved fields of the header
 *	can be validated and policies are required (see below);
 *	new families should leave this field at zero
 * @ops: the operations supported by this family
 * @n_ops: number of operations supported by this family
 * @small_ops: the small-struct operations supported by this family
 * @n_small_ops: number of small-struct operations supported by this family
 * @split_ops: the split do/dump form of operation definition
 * @n_split_ops: number of entries in @split_ops, note that with split do/dump
 *	ops the number of entries is not the same as number of commands
 * @sock_priv_size: the size of per-socket private memory
 * @sock_priv_init: the per-socket private memory initializer
 * @sock_priv_destroy: the per-socket private memory destructor
 *
 * Attribute policies (the combination of @policy and @maxattr fields)
 * can be attached at the family level or at the operation level.
 * If both are present the per-operation policy takes precedence.
 * For operations before @resv_start_op lack of policy means that the core
 * will perform no attribute parsing or validation. For newer operations
 * if policy is not provided core will reject all TLV attributes.
 *
 * family 描述名称、版本、策略、命令、组播组、netns 能力及每 socket
 * 私有数据。parallel_ops 决定 core 是否串行回调；单命令策略覆盖 family 策略。
 * resv_start_op 之前保留旧 ABI 的宽松行为，之后无 policy 就拒绝全部属性。
 */
struct genl_family {
	unsigned int		hdrsize;
	char			name[GENL_NAMSIZ];
	unsigned int		version;
	unsigned int		maxattr;
	u8			netnsok:1;
	u8			parallel_ops:1;
	u8			n_ops;
	u8			n_small_ops;
	u8			n_split_ops;
	u8			n_mcgrps;
	u8			resv_start_op;
	const struct nla_policy *policy;
	int			(*pre_doit)(const struct genl_split_ops *ops,
					    struct sk_buff *skb,
					    struct genl_info *info);
	void			(*post_doit)(const struct genl_split_ops *ops,
					     struct sk_buff *skb,
					     struct genl_info *info);
	int			(*bind)(int mcgrp);
	void			(*unbind)(int mcgrp);
	const struct genl_ops *	ops;
	const struct genl_small_ops *small_ops;
	const struct genl_split_ops *split_ops;
	const struct genl_multicast_group *mcgrps;
	struct module		*module;

	size_t			sock_priv_size;
	void			(*sock_priv_init)(void *priv);
	void			(*sock_priv_destroy)(void *priv);

/* private: internal use only */
	/* 以下字段由 core 注册时填写，family 实现不得自行修改。 */
	/* protocol family identifier */
	int			id;
	/* starting number of multicast group IDs in this family */
	unsigned int		mcgrp_offset;
	/* list of per-socket privs */
	struct xarray		*sock_privs;
};

/**
 * struct genl_info - receiving information
 * @snd_seq: sending sequence number
 * @snd_portid: netlink portid of sender
 * @family: generic netlink family
 * @nlhdr: netlink message header
 * @genlhdr: generic netlink message header
 * @attrs: netlink attributes
 * @_net: network namespace
 * @ctx: storage space for the use by the family
 * @user_ptr: user pointers (deprecated, use ctx instead)
 * @extack: extended ACK report struct
 *
 * genl_info 是一次请求的借用上下文，包含发送端、已解析属性、netns
 * 和 extack。ctx 供回调链共享固定大小临时状态，只在本次回调生命期有效。
 */
struct genl_info {
	u32			snd_seq;
	u32			snd_portid;
	const struct genl_family *family;
	const struct nlmsghdr *	nlhdr;
	struct genlmsghdr *	genlhdr;
	struct nlattr **	attrs;
	possible_net_t		_net;
	union {
		u8		ctx[NETLINK_CTX_SIZE];
		void *		user_ptr[2];
	};
	struct netlink_ext_ack *extack;
};

static inline struct net *genl_info_net(const struct genl_info *info)
{
	/* 从 possible_net_t 读取请求 netns，返回借用指针。 */
	return read_pnet(&info->_net);
}

static inline void genl_info_net_set(struct genl_info *info, struct net *net)
{
	/* 设置本地上下文 netns，不改变 net 引用计数。 */
	write_pnet(&info->_net, net);
}

static inline void *genl_info_userhdr(const struct genl_info *info)
{
	/* 跳过固定 genlmsghdr，返回 family 自定义头的借用地址。 */
	return (u8 *)info->genlhdr + GENL_HDRLEN;
}

#define GENL_SET_ERR_MSG(info, msg) NL_SET_ERR_MSG((info)->extack, msg)

#define GENL_SET_ERR_MSG_FMT(info, msg, args...) \
	NL_SET_ERR_MSG_FMT((info)->extack, msg, ##args)

/* Report that a root attribute is missing */
#define GENL_REQ_ATTR_CHECK(info, attr) ({				\
	const struct genl_info *__info = (info);			\
									\
	NL_REQ_ATTR_CHECK(__info->extack, NULL, __info->attrs, (attr)); \
})

enum genl_validate_flags {
	/* 兼容标志可关闭严格/转储验证，新协议不应依赖宽松解析。 */
	GENL_DONT_VALIDATE_STRICT		= BIT(0),
	GENL_DONT_VALIDATE_DUMP			= BIT(1),
	GENL_DONT_VALIDATE_DUMP_STRICT		= BIT(2),
};

/**
 * struct genl_small_ops - generic netlink operations (small version)
 * @cmd: command identifier
 * @internal_flags: flags used by the family
 * @flags: GENL_* flags (%GENL_ADMIN_PERM or %GENL_UNS_ADMIN_PERM)
 * @validate: validation flags from enum genl_validate_flags
 * @doit: standard command callback
 * @dumpit: callback for dumpers
 *
 * This is a cut-down version of struct genl_ops for users who don't need
 * most of the ancillary infra and want to save space.
 * small_ops 省略 per-op policy 和 dump 生命周期钩子，适合命令少且
 * 共用 family 策略的协议，以减小只读操作表体积。
 */
struct genl_small_ops {
	int	(*doit)(struct sk_buff *skb, struct genl_info *info);
	int	(*dumpit)(struct sk_buff *skb, struct netlink_callback *cb);
	u8	cmd;
	u8	internal_flags;
	u8	flags;
	u8	validate;
};

/**
 * struct genl_ops - generic netlink operations
 * @cmd: command identifier
 * @internal_flags: flags used by the family
 * @flags: GENL_* flags (%GENL_ADMIN_PERM or %GENL_UNS_ADMIN_PERM)
 * @maxattr: maximum number of attributes supported
 * @policy: netlink policy (takes precedence over family policy)
 * @validate: validation flags from enum genl_validate_flags
 * @doit: standard command callback
 * @start: start callback for dumps
 * @dumpit: callback for dumpers
 * @done: completion callback for dumps
 * doit 处理单次请求；start/dumpit/done 可跨多个 skb 迭代，状态存于
 * netlink_callback；per-op policy 优先于 family policy。
 */
struct genl_ops {
	int		       (*doit)(struct sk_buff *skb,
				       struct genl_info *info);
	int		       (*start)(struct netlink_callback *cb);
	int		       (*dumpit)(struct sk_buff *skb,
					 struct netlink_callback *cb);
	int		       (*done)(struct netlink_callback *cb);
	const struct nla_policy *policy;
	unsigned int		maxattr;
	u8			cmd;
	u8			internal_flags;
	u8			flags;
	u8			validate;
};

/**
 * struct genl_split_ops - generic netlink operations (do/dump split version)
 * @cmd: command identifier
 * @internal_flags: flags used by the family
 * @flags: GENL_* flags (%GENL_ADMIN_PERM or %GENL_UNS_ADMIN_PERM)
 * @validate: validation flags from enum genl_validate_flags
 * @policy: netlink policy (takes precedence over family policy)
 * @maxattr: maximum number of attributes supported
 *
 * Do callbacks:
 * @pre_doit: called before an operation's @doit callback, it may
 *	do additional, common, filtering and return an error
 * @doit: standard command callback
 * @post_doit: called after an operation's @doit callback, it may
 *	undo operations done by pre_doit, for example release locks
 *
 * Dump callbacks:
 * @start: start callback for dumps
 * @dumpit: callback for dumpers
 * @done: completion callback for dumps
 *
 * Do callbacks can be used if %GENL_CMD_CAP_DO is set in @flags.
 * Dump callbacks can be used if %GENL_CMD_CAP_DUMP is set in @flags.
 * Exactly one of those flags must be set.
 * split_ops 将同一 cmd 的 doit 与 dump 拆成两项，使两条路径可有不同
 * 策略和权限；每项必须且只能声明 DO 或 DUMP 能力之一。
 */
struct genl_split_ops {
	union {
		struct {
			int (*pre_doit)(const struct genl_split_ops *ops,
					struct sk_buff *skb,
					struct genl_info *info);
			int (*doit)(struct sk_buff *skb,
				    struct genl_info *info);
			void (*post_doit)(const struct genl_split_ops *ops,
					  struct sk_buff *skb,
					  struct genl_info *info);
		};
		struct {
			int (*start)(struct netlink_callback *cb);
			int (*dumpit)(struct sk_buff *skb,
				      struct netlink_callback *cb);
			int (*done)(struct netlink_callback *cb);
		};
	};
	const struct nla_policy *policy;
	unsigned int		maxattr;
	u8			cmd;
	u8			internal_flags;
	u8			flags;
	u8			validate;
};

/**
 * struct genl_dumpit_info - info that is available during dumpit op call
 * @op: generic netlink ops - for internal genl code usage
 * @attrs: netlink attributes
 * @info: struct genl_info describing the request
 * core 为一次 dump 保存解析后的请求与操作，cb->data 在 dump 期间借用它。
 */
struct genl_dumpit_info {
	struct genl_split_ops op;
	struct genl_info info;
};

static inline const struct genl_dumpit_info *
genl_dumpit_info(struct netlink_callback *cb)
{
	/* cb->data 由 Generic Netlink core 初始化为 genl_dumpit_info。 */
	return cb->data;
}

static inline const struct genl_info *
genl_info_dump(struct netlink_callback *cb)
{
	/* 返回 dump 原请求的只读上下文，不得越过 dump 生命周期保存。 */
	return &genl_dumpit_info(cb)->info;
}

/**
 * genl_info_init_ntf() - initialize genl_info for notifications
 * @info:   genl_info struct to set up
 * @family: pointer to the genetlink family
 * @cmd:    command to be used in the notification
 *
 * Initialize a locally declared struct genl_info to pass to various APIs.
 * Intended to be used when creating notifications.
 * 清零 info，并借用 user_ptr 存储仅含 cmd 的 genlmsghdr；nlhdr 保持
 * NULL，使 genl_info_is_ntf() 能区分通知与真实入站请求。
 */
static inline void
genl_info_init_ntf(struct genl_info *info, const struct genl_family *family,
		   u8 cmd)
{
	struct genlmsghdr *hdr = (void *) &info->user_ptr[0];

	memset(info, 0, sizeof(*info));
	info->family = family;
	info->genlhdr = hdr;
	hdr->cmd = cmd;
}

static inline bool genl_info_is_ntf(const struct genl_info *info)
{
	/* 本地通知没有入站 nlmsghdr；真实请求始终设置该指针。 */
	return !info->nlhdr;
}

void *__genl_sk_priv_get(struct genl_family *family, struct sock *sk);
void *genl_sk_priv_get(struct genl_family *family, struct sock *sk);
int genl_register_family(struct genl_family *family);
int genl_unregister_family(const struct genl_family *family);
void genl_notify(const struct genl_family *family, struct sk_buff *skb,
		 struct genl_info *info, u32 group, gfp_t flags);

void *genlmsg_put(struct sk_buff *skb, u32 portid, u32 seq,
		  const struct genl_family *family, int flags, u8 cmd);

static inline void *
__genlmsg_iput(struct sk_buff *skb, const struct genl_info *info, int flags)
{
	/* 复用上下文的 family/cmd/seq/portid 开始构造消息。 */
	return genlmsg_put(skb, info->snd_portid, info->snd_seq, info->family,
			   flags, info->genlhdr->cmd);
}

/**
 * genlmsg_iput - start genetlink message based on genl_info
 * @skb: skb in which message header will be placed
 * @info: genl_info as provided to do/dump handlers
 *
 * Convenience wrapper which starts a genetlink message based on
 * information in user request. @info should be either the struct passed
 * by genetlink core to do/dump handlers (when constructing replies to
 * such requests) or a struct initialized by genl_info_init_ntf()
 * when constructing notifications.
 *
 * Returns: pointer to new genetlink header.
 * 成功返回 family 自定义头起点，失败返回 NULL；随后应填属性并 end，
 * 或取消并释放 skb。
 */
static inline void *
genlmsg_iput(struct sk_buff *skb, const struct genl_info *info)
{
	return __genlmsg_iput(skb, info, 0);
}

/**
 * genlmsg_nlhdr - Obtain netlink header from user specified header
 * @user_hdr: user header as returned from genlmsg_put()
 *
 * Returns: pointer to netlink header.
 * genlmsg_put 返回自定义头地址，这里按固定两层头长度反推 nlmsghdr。
 */
static inline struct nlmsghdr *genlmsg_nlhdr(void *user_hdr)
{
	return (struct nlmsghdr *)((char *)user_hdr -
				   GENL_HDRLEN -
				   NLMSG_HDRLEN);
}

/**
 * genlmsg_parse_deprecated - parse attributes of a genetlink message
 * @nlh: netlink message header
 * @family: genetlink message family
 * @tb: destination array with maxtype+1 elements
 * @maxtype: maximum attribute type to be expected
 * @policy: validation policy
 * @extack: extended ACK report struct
 * 兼容接口跳过自定义头后采用 LIBERAL 模式；新代码应优先严格解析。
 */
static inline int genlmsg_parse_deprecated(const struct nlmsghdr *nlh,
					   const struct genl_family *family,
					   struct nlattr *tb[], int maxtype,
					   const struct nla_policy *policy,
					   struct netlink_ext_ack *extack)
{
	return __nlmsg_parse(nlh, family->hdrsize + GENL_HDRLEN, tb, maxtype,
			     policy, NL_VALIDATE_LIBERAL, extack);
}

/**
 * genlmsg_parse - parse attributes of a genetlink message
 * @nlh: netlink message header
 * @family: genetlink message family
 * @tb: destination array with maxtype+1 elements
 * @maxtype: maximum attribute type to be expected
 * @policy: validation policy
 * @extack: extended ACK report struct
 * STRICT 模式按 policy 校验属性类型、长度与布局，详情写入 extack。
 */
static inline int genlmsg_parse(const struct nlmsghdr *nlh,
				const struct genl_family *family,
				struct nlattr *tb[], int maxtype,
				const struct nla_policy *policy,
				struct netlink_ext_ack *extack)
{
	return __nlmsg_parse(nlh, family->hdrsize + GENL_HDRLEN, tb, maxtype,
			     policy, NL_VALIDATE_STRICT, extack);
}

/**
 * genl_dump_check_consistent - check if sequence is consistent and advertise if not
 * @cb: netlink callback structure that stores the sequence number
 * @user_hdr: user header as returned from genlmsg_put()
 *
 * Cf. nl_dump_check_consistent(), this just provides a wrapper to make it
 * simpler to use with generic netlink.
 * 数据集序列变化时标记 NLM_F_DUMP_INTR，提示用户重试一致快照。
 */
static inline void genl_dump_check_consistent(struct netlink_callback *cb,
					      void *user_hdr)
{
	nl_dump_check_consistent(cb, genlmsg_nlhdr(user_hdr));
}

/**
 * genlmsg_put_reply - Add generic netlink header to a reply message
 * @skb: socket buffer holding the message
 * @info: receiver info
 * @family: generic netlink family
 * @flags: netlink message flags
 * @cmd: generic netlink command
 *
 * Returns: pointer to user specific header
 * 沿用请求端 portid/序列号写回复头；NULL 表示 skb 空间不足。
 */
static inline void *genlmsg_put_reply(struct sk_buff *skb,
				      struct genl_info *info,
				      const struct genl_family *family,
				      int flags, u8 cmd)
{
	return genlmsg_put(skb, info->snd_portid, info->snd_seq, family,
			   flags, cmd);
}

/**
 * genlmsg_end - Finalize a generic netlink message
 * @skb: socket buffer the message is stored in
 * @hdr: user specific header
 * 以起始头位置计算最终 nlmsg_len；封口后才能发送。
 */
static inline void genlmsg_end(struct sk_buff *skb, void *hdr)
{
	nlmsg_end(skb, hdr - GENL_HDRLEN - NLMSG_HDRLEN);
}

/**
 * genlmsg_cancel - Cancel construction of a generic netlink message
 * @skb: socket buffer the message is stored in
 * @hdr: generic netlink message header
 * 属性构造失败时回退 skb 尾部；NULL 允许调用者统一失败清理。
 */
static inline void genlmsg_cancel(struct sk_buff *skb, void *hdr)
{
	if (hdr)
		nlmsg_cancel(skb, hdr - GENL_HDRLEN - NLMSG_HDRLEN);
}

/**
 * genlmsg_multicast_netns_filtered - multicast a netlink message
 *				      to a specific netns with filter
 *				      function
 * @family: the generic netlink family
 * @net: the net namespace
 * @skb: netlink message as socket buffer
 * @portid: own netlink portid to avoid sending to yourself
 * @group: offset of multicast group in groups array
 * @flags: allocation flags
 * @filter: filter function
 * @filter_data: filter function private data
 *
 * Return: 0 on success, negative error code for failure.
 * group 是 family 内下标，发送前换算为全局组号。越界与正常发送
 * 都会消费 skb 所有权，调用后不得再次释放。
 */
static inline int
genlmsg_multicast_netns_filtered(const struct genl_family *family,
				 struct net *net, struct sk_buff *skb,
				 u32 portid, unsigned int group, gfp_t flags,
				 netlink_filter_fn filter,
				 void *filter_data)
{
	if (WARN_ON_ONCE(group >= family->n_mcgrps)) {
		nlmsg_free(skb);
		return -EINVAL;
	}
	group = family->mcgrp_offset + group;
	return nlmsg_multicast_filtered(net->genl_sock, skb, portid, group,
					flags, filter, filter_data);
}

/**
 * genlmsg_multicast_netns - multicast a netlink message to a specific netns
 * @family: the generic netlink family
 * @net: the net namespace
 * @skb: netlink message as socket buffer
 * @portid: own netlink portid to avoid sending to yourself
 * @group: offset of multicast group in groups array
 * @flags: allocation flags
 * 无过滤器的指定 netns 组播，是 filtered 版本的直接封装。
 */
static inline int genlmsg_multicast_netns(const struct genl_family *family,
					  struct net *net, struct sk_buff *skb,
					  u32 portid, unsigned int group, gfp_t flags)
{
	return genlmsg_multicast_netns_filtered(family, net, skb, portid,
						group, flags, NULL, NULL);
}

/**
 * genlmsg_multicast - multicast a netlink message to the default netns
 * @family: the generic netlink family
 * @skb: netlink message as socket buffer
 * @portid: own netlink portid to avoid sending to yourself
 * @group: offset of multicast group in groups array
 * @flags: allocation flags
 * 便捷接口固定发送到 init_net；容器协议通常应显式选择 netns 版本。
 */
static inline int genlmsg_multicast(const struct genl_family *family,
				    struct sk_buff *skb, u32 portid,
				    unsigned int group, gfp_t flags)
{
	return genlmsg_multicast_netns(family, &init_net, skb,
				       portid, group, flags);
}

/**
 * genlmsg_multicast_allns - multicast a netlink message to all net namespaces
 * @family: the generic netlink family
 * @skb: netlink message as socket buffer
 * @portid: own netlink portid to avoid sending to yourself
 * @group: offset of multicast group in groups array
 *
 * This function must hold the RTNL or rcu_read_lock().
 * 遍历所有 netns，调用者须以 RTNL 或 RCU 固定命名空间链表。
 */
int genlmsg_multicast_allns(const struct genl_family *family,
			    struct sk_buff *skb, u32 portid,
			    unsigned int group);

/**
 * genlmsg_unicast - unicast a netlink message
 * @net: network namespace to look up @portid in
 * @skb: netlink message as socket buffer
 * @portid: netlink portid of the destination socket
 */
/*
 * genlmsg_unicast() - 把一条完整 Generic Netlink skb 单播到指定端口。
 *
 * @net: 查找目标 Netlink socket 的网络命名空间，借用至调用结束。
 * @skb: 待发送消息；调用后无论成功失败都由 Netlink 路径消费所有权。
 * @portid: 目标 socket 的 Netlink port ID，不是进程 PID 引用。
 *
 * 返回 nlmsg_unicast() 的 0 或负 errno。taskstats 退出通知固定传 init_net，
 * 因而其 listener 注册也限制到初始命名空间，避免标识视图不一致。
 */
static inline int genlmsg_unicast(struct net *net, struct sk_buff *skb, u32 portid)
{
	return nlmsg_unicast(net->genl_sock, skb, portid);
}

/**
 * genlmsg_reply - reply to a request
 * @skb: netlink message to be sent back
 * @info: receiver information
 */
/*
 * genlmsg_reply() - 使用请求上下文把 skb 回送给原 Generic Netlink 端口。
 *
 * @skb: 已封口的回复消息，所有权转交 genlmsg_unicast()。
 * @info: 借用的请求元数据；genl_info_net() 选择请求所在 netns，snd_portid
 *        选择发送者端口，因此调用者无需再次解析 nlmsghdr。
 *
 * 返回单播结果。taskstats 的同步 PID/TGID/cgroup 查询在填完属性后使用
 * 此 helper；返回后不得再次释放或访问 @skb。
 */
static inline int genlmsg_reply(struct sk_buff *skb, struct genl_info *info)
{
	return genlmsg_unicast(genl_info_net(info), skb, info->snd_portid);
}

/**
 * genlmsg_data - head of message payload
 * @gnlh: genetlink message header
 */
/*
 * genlmsg_data() 返回 Generic Netlink 专用头之后的载荷起点，是指向原 skb
 * 存储的借用指针，不增加引用也不复制数据。taskstats 用该地址作为
 * genlmsg_end() 的消息起始标记；skb 释放或重排后指针立即失效。
 */
static inline void *genlmsg_data(const struct genlmsghdr *gnlh)
{
	return ((unsigned char *) gnlh + GENL_HDRLEN);
}

/**
 * genlmsg_len - length of message payload
 * @gnlh: genetlink message header
 * 从 genl 头反推 nlmsghdr，总长度扣除两层固定头即为自定义头和属性载荷。
 */
static inline int genlmsg_len(const struct genlmsghdr *gnlh)
{
	struct nlmsghdr *nlh = (struct nlmsghdr *)((unsigned char *)gnlh -
							NLMSG_HDRLEN);
	return (nlh->nlmsg_len - GENL_HDRLEN - NLMSG_HDRLEN);
}

/**
 * genlmsg_msg_size - length of genetlink message not including padding
 * @payload: length of message payload
 * 返回 genl 固定头加 payload 的未对齐长度，不包含 nlmsghdr。
 */
static inline int genlmsg_msg_size(int payload)
{
	return GENL_HDRLEN + payload;
}

/**
 * genlmsg_total_size - length of genetlink message including padding
 * @payload: length of message payload
 * 把 genl 消息体按 Netlink 对齐，供 nlmsg_new 计算所需尾部空间。
 */
static inline int genlmsg_total_size(int payload)
{
	return NLMSG_ALIGN(genlmsg_msg_size(payload));
}

/**
 * genlmsg_new - Allocate a new generic netlink message
 * @payload: size of the message payload
 * @flags: the type of memory to allocate.
 * 按完整对齐尺寸分配 skb，失败返回 NULL；flags 必须匹配调用上下文。
 */
static inline struct sk_buff *genlmsg_new(size_t payload, gfp_t flags)
{
	return nlmsg_new(genlmsg_total_size(payload), flags);
}

/**
 * genl_set_err - report error to genetlink broadcast listeners
 * @family: the generic netlink family
 * @net: the network namespace to report the error to
 * @portid: the PORTID of a process that we want to skip (if any)
 * @group: the broadcast group that will notice the error
 * 	(this is the offset of the multicast group in the groups array)
 * @code: error code, must be negative (as usual in kernelspace)
 *
 * This function returns the number of broadcast listeners that have set the
 * NETLINK_RECV_NO_ENOBUFS socket option.
 * 把 family 内组下标换成全局组号，向订阅者报告负 errno；越界返回
 * EINVAL，正常返回值是选择忽略 ENOBUFS 通知的监听者数量。
 */
static inline int genl_set_err(const struct genl_family *family,
			       struct net *net, u32 portid,
			       u32 group, int code)
{
	if (WARN_ON_ONCE(group >= family->n_mcgrps))
		return -EINVAL;
	group = family->mcgrp_offset + group;
	return netlink_set_err(net->genl_sock, portid, group, code);
}

static inline int genl_has_listeners(const struct genl_family *family,
				     struct net *net, unsigned int group)
{
	/* 检查指定 netns 的组播组是否存在监听者；group 仍是 family 内下标。 */
	if (WARN_ON_ONCE(group >= family->n_mcgrps))
		return -EINVAL;
	group = family->mcgrp_offset + group;
	return netlink_has_listeners(net->genl_sock, group);
}
#endif	/* __NET_GENERIC_NETLINK_H */
