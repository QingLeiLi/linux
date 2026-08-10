// SPDX-License-Identifier: GPL-2.0
/*
 * kobject.h - generic kernel object infrastructure.
 *
 * Copyright (c) 2002-2003 Patrick Mochel
 * Copyright (c) 2002-2003 Open Source Development Labs
 * Copyright (c) 2006-2008 Greg Kroah-Hartman <greg@kroah.com>
 * Copyright (c) 2006-2008 Novell Inc.
 *
 * Please read Documentation/core-api/kobject.rst before using the kobject
 * interface, ESPECIALLY the parts about reference counts and object
 * destructors.
 */
/*
 * kobject 是 sysfs 目录、引用计数和对象层级的共同基础。使用接口前尤其要理解
 * 引用计数与 release 析构：把对象挂入 sysfs 只建立可见性，最后一次 kobject_put()
 * 才沿 ktype->release 结束内存生命周期；kernel_kobj 等全局根则通常贯穿运行期。
 */

#ifndef _KOBJECT_H_
#define _KOBJECT_H_

#include <linux/types.h>
#include <linux/list.h>
#include <linux/sysfs.h>
#include <linux/compiler.h>
#include <linux/container_of.h>
#include <linux/spinlock.h>
#include <linux/kref.h>
#include <linux/kobject_ns.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>
#include <linux/uidgid.h>

#define UEVENT_HELPER_PATH_LEN		256
#define UEVENT_NUM_ENVP			64	/* number of env pointers */
#define UEVENT_BUFFER_SIZE		2048	/* buffer for the variables */
/*
 * 每个 uevent 环境最多保存 64 个字符串指针，所有字符串共享 2048 字节缓冲区；
 * helper 路径另有 256 字节上限。add_uevent_var() 同时检查指针槽和字符缓冲区，
 * 任一耗尽都使本次事件构造失败，而不是截断出一个语义不完整的事件。
 */

#ifdef CONFIG_UEVENT_HELPER
/* path to the userspace helper executed on an event */
/*
 * 事件发生时可执行的用户态 helper 路径。存储定义在 lib/kobject_uevent.c，初值
 * 来自 CONFIG_UEVENT_HELPER_PATH；kernel/ksysfs.c 与 /proc/sys/kernel/hotplug
 * 都可改写它。调用方只借用全局数组，不拥有或释放其内存。
 */
extern char uevent_helper[];
#endif

/* counter to tag the uevent, read only except for the kobject core */
/*
 * 为 uevent 分配全局单调序号的原子计数器。只有 kobject 事件核心递增；
 * /sys/kernel/uevent_seqnum 只读取快照。原子性避免并发发送得到相同或撕裂值，
 * 但序号代表“已开始构造/发送事件”的顺序，不保证用户态跨通道接收顺序。
 */
extern atomic64_t uevent_seqnum;

/*
 * The actions here must match the index to the string array
 * in lib/kobject_uevent.c
 *
 * Do not add new actions here without checking with the driver-core
 * maintainers. Action strings are not meant to express subsystem
 * or device specific properties. In most cases you want to send a
 * kobject_uevent_env(kobj, KOBJ_CHANGE, env) with additional event
 * specific variables added to the event environment.
 */
/*
 * 枚举下标必须与 lib/kobject_uevent.c 的字符串表严格一致。动作只描述对象通用
 * 生命周期变化；设备/子系统细节应作为 KOBJ_CHANGE 的环境变量携带，避免把
 * 稳定 ABI 扩展成无法维护的设备专用动作集合。
 */
enum kobject_action {
	KOBJ_ADD,
	KOBJ_REMOVE,
	KOBJ_CHANGE,
	KOBJ_MOVE,
	KOBJ_ONLINE,
	KOBJ_OFFLINE,
	KOBJ_BIND,
	KOBJ_UNBIND,
};

struct kobject {
	const char		*name;
	struct list_head	entry;
	struct kobject		*parent;
	struct kset		*kset;
	const struct kobj_type	*ktype;
	struct kernfs_node	*sd; /* sysfs directory entry */
	struct kref		kref;

	unsigned int state_initialized:1;
	unsigned int state_in_sysfs:1;
	unsigned int state_add_uevent_sent:1;
	unsigned int state_remove_uevent_sent:1;
	unsigned int uevent_suppress:1;

#ifdef CONFIG_DEBUG_KOBJECT_RELEASE
	struct delayed_work	release;
#endif
};

__printf(2, 3) int kobject_set_name(struct kobject *kobj, const char *name, ...);
__printf(2, 0) int kobject_set_name_vargs(struct kobject *kobj, const char *fmt, va_list vargs);

static inline const char *kobject_name(const struct kobject *kobj)
{
	return kobj->name;
}

void kobject_init(struct kobject *kobj, const struct kobj_type *ktype);
__printf(3, 4) __must_check int kobject_add(struct kobject *kobj,
					    struct kobject *parent,
					    const char *fmt, ...);
__printf(4, 5) __must_check int kobject_init_and_add(struct kobject *kobj,
						     const struct kobj_type *ktype,
						     struct kobject *parent,
						     const char *fmt, ...);

void kobject_del(struct kobject *kobj);

struct kobject * __must_check kobject_create_and_add(const char *name, struct kobject *parent);

int __must_check kobject_rename(struct kobject *, const char *new_name);
int __must_check kobject_move(struct kobject *, struct kobject *);

struct kobject *kobject_get(struct kobject *kobj);
struct kobject * __must_check kobject_get_unless_zero(struct kobject *kobj);
void kobject_put(struct kobject *kobj);

const struct ns_common *kobject_namespace(const struct kobject *kobj);
void kobject_get_ownership(const struct kobject *kobj, kuid_t *uid, kgid_t *gid);
char *kobject_get_path(const struct kobject *kobj, gfp_t flag);

struct kobj_type {
	void (*release)(struct kobject *kobj);
	const struct sysfs_ops *sysfs_ops;
	const struct attribute_group **default_groups;
	const struct kobj_ns_type_operations *(*child_ns_type)(const struct kobject *kobj);
	const struct ns_common *(*namespace)(const struct kobject *kobj);
	void (*get_ownership)(const struct kobject *kobj, kuid_t *uid, kgid_t *gid);
};

struct kobj_uevent_env {
	char *argv[3];
	char *envp[UEVENT_NUM_ENVP];
	int envp_idx;
	char buf[UEVENT_BUFFER_SIZE];
	int buflen;
};

struct kset_uevent_ops {
	int (* const filter)(const struct kobject *kobj);
	const char *(* const name)(const struct kobject *kobj);
	int (* const uevent)(const struct kobject *kobj, struct kobj_uevent_env *env);
};

struct kobj_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf);
	ssize_t (*store)(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count);
};

extern const struct sysfs_ops kobj_sysfs_ops;

struct sock;

/**
 * struct kset - a set of kobjects of a specific type, belonging to a specific subsystem.
 *
 * A kset defines a group of kobjects.  They can be individually
 * different "types" but overall these kobjects all want to be grouped
 * together and operated on in the same manner.  ksets are used to
 * define the attribute callbacks and other common events that happen to
 * a kobject.
 *
 * @list: the list of all kobjects for this kset
 * @list_lock: a lock for iterating over the kobjects
 * @kobj: the embedded kobject for this kset (recursion, isn't it fun...)
 * @uevent_ops: the set of uevent operations for this kset.  These are
 * called whenever a kobject has something happen to it so that the kset
 * can add new environment variables, or filter out the uevents if so
 * desired.
 */
struct kset {
	struct list_head list;
	spinlock_t list_lock;
	struct kobject kobj;
	const struct kset_uevent_ops *uevent_ops;
} __randomize_layout;

void kset_init(struct kset *kset);
int __must_check kset_register(struct kset *kset);
void kset_unregister(struct kset *kset);
struct kset * __must_check kset_create_and_add(const char *name, const struct kset_uevent_ops *u,
					       struct kobject *parent_kobj);

static inline struct kset *to_kset(struct kobject *kobj)
{
	return kobj ? container_of(kobj, struct kset, kobj) : NULL;
}

static inline struct kset *kset_get(struct kset *k)
{
	return k ? to_kset(kobject_get(&k->kobj)) : NULL;
}

static inline void kset_put(struct kset *k)
{
	kobject_put(&k->kobj);
}

static inline const struct kobj_type *get_ktype(const struct kobject *kobj)
{
	return kobj->ktype;
}

struct kobject *kset_find_obj(struct kset *, const char *);

/* The global /sys/kernel/ kobject for people to chain off of */
/*
 * 全局 /sys/kernel 根，定义并由 kernel/ksysfs.c 在 do_basic_setup() 阶段创建。
 * 后续子系统借用该指针作为 kobject_create_and_add() 的父对象；声明本身不授予
 * 额外引用，调用方也不得对这个长寿命全局根执行匹配 kobject_put()。
 */
extern struct kobject *kernel_kobj;
/* The global /sys/kernel/mm/ kobject for people to chain off of */
extern struct kobject *mm_kobj;
/* The global /sys/hypervisor/ kobject for people to chain off of */
extern struct kobject *hypervisor_kobj;
/* The global /sys/power/ kobject for people to chain off of */
extern struct kobject *power_kobj;
/* The global /sys/firmware/ kobject for people to chain off of */
extern struct kobject *firmware_kobj;

int kobject_uevent(struct kobject *kobj, enum kobject_action action);
int kobject_uevent_env(struct kobject *kobj, enum kobject_action action,
			char *envp[]);
int kobject_synth_uevent(struct kobject *kobj, const char *buf, size_t count);

__printf(2, 3)
int add_uevent_var(struct kobj_uevent_env *env, const char *format, ...);

#endif /* _KOBJECT_H_ */
