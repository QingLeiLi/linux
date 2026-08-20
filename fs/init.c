// SPDX-License-Identifier: GPL-2.0
/*
 * Routines that mimic syscalls, but don't use the user address space or file
 * descriptors.  Only for init/ and related early init code.
 */
/*
 * 【文件说明】
 * 本文件提供了一组"内核专用的系统调用模拟函数"，供内核初始化阶段（init/）使用。
 *
 * 【为什么需要这些函数？】
 * 通常的系统调用（如 open/mount/chdir 等）要求：
 *   1. 调用者有合法的用户地址空间（user_space）
 *   2. 调用者持有文件描述符（file descriptor，fd）
 *
 * 但在内核 init 阶段（如 kernel_init 线程中），文件系统和进程结构尚未完全建立，
 * 不能使用标准的 sys_xxx 系统调用路径。
 * 这些函数绕过用户空间检查，直接调用 VFS（虚拟文件系统）内核路径。
 *
 * 【使用限制】
 * - 所有函数标记了 __init，只在内核初始化阶段链接，之后会被释放掉
 * - 只能从内核线程（init/）调用，不可在普通进程上下文中使用
 * - 所有路径均为内核路径（不涉及 copy_from_user/copy_to_user）
 *
 * 【常见场景】
 * - 挂载根文件系统（mount rootfs）
 * - 切换根目录（pivot_root / chroot）
 * - 创建初始设备节点（mknod）
 * - 在执行 /sbin/init 之前设置好文件系统环境
 */
#include <linux/init.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/file.h>
#include <linux/init_syscalls.h>
#include <linux/security.h>
#include "internal.h"

/*
 * init_pivot_root - 内核初始化专用的 pivot_root 操作
 * @new_root: 新根文件系统的路径（字符串）
 * @put_old:  旧根文件系统将被移动到的路径（字符串）
 *
 * 【功能说明】
 * pivot_root 用于切换根文件系统（rootfs）。常见于：
 *   1. initramfs 启动后切换到真正的磁盘根分区
 *   2. 容器初始化时切换到容器的根目录
 *
 * 【工作流程】
 *   1. kern_path() 将路径字符串解析为 struct path（dentry + vfsmount）
 *   2. 调用 VFS 核心函数 path_pivot_root() 完成切换
 *   3. __free(path_put) 是 cleanup 属性，自动释放 path 资源
 *
 * 【与 chroot 的区别】
 * - chroot 仅改变当前进程看到的根目录（可被突破）
 * - pivot_root 切换整个系统的根文件系统挂载（更彻底、安全）
 *
 * 【返回值】
 * 成功返回 0，失败返回负的错误码（-ENOENT, -EINVAL 等）
 */
int __init init_pivot_root(const char *new_root, const char *put_old)
{
	/*
	 * __free(path_put) 属性：
	 * GCC cleanup 扩展，函数退出时自动调用 path_put(&new_path/&old_path)。
	 * 等价于在每个 return 前手动调用 path_put()，避免泄漏引用计数。
	 */
	struct path new_path __free(path_put) = {};
	struct path old_path __free(path_put) = {};
	int ret;

	/*
	 * kern_path - 内核路径查找（不走用户空间）
	 * @new_root: 路径字符串（必须是内核可见路径，如 "/mnt/root"）
	 * @flags:    查找标志
	 *   - LOOKUP_FOLLOW: 跟随符号链接（如果最后一级是 symlink，解析到目标）
	 *   - LOOKUP_DIRECTORY: 要求结果必须是目录
	 * @new_path: 输出参数，填充解析后的 path 结构（包含 dentry 和 vfsmount）
	 *
	 * 返回值：0 成功，负值为错误码（如 -ENOENT 路径不存在）
	 */
	ret = kern_path(new_root, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &new_path);
	if (ret)
		return ret;  /* 解析失败，__free 会自动清理 new_path */

	ret = kern_path(put_old, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &old_path);
	if (ret)
		return ret;  /* 解析失败，__free 会清理 old_path 和 new_path */

	/*
	 * path_pivot_root - VFS 层的 pivot_root 核心实现
	 * 将当前进程（通常是 init）的根文件系统从旧根切换到 new_path，
	 * 旧根挂载到 old_path 位置。成功后全局根文件系统被替换。
	 */
	return path_pivot_root(&new_path, &old_path);
	/* 函数退出时 __free 自动调用 path_put 释放两个 path */
}

/*
 * init_mount - 内核初始化专用的 mount 操作
 * @dev_name:   设备名（如 "/dev/sda1"）或特殊文件系统名（如 "proc", "sysfs"）
 * @dir_name:   挂载点路径（如 "/proc", "/mnt"）
 * @type_page:  文件系统类型字符串（如 "ext4", "tmpfs", "proc"）
 * @flags:      挂载标志位（MS_RDONLY, MS_NOEXEC 等，定义在 include/uapi/linux/mount.h）
 * @data_page:  文件系统特定的挂载选项（如 "rw,noatime"），可为 NULL
 *
 * 【功能说明】
 * 在内核初始化阶段挂载文件系统，例如：
 *   - 挂载 rootfs（根文件系统）
 *   - 挂载 /proc, /sys, /dev 等虚拟文件系统
 *
 * 【工作流程】
 *   1. kern_path() 解析挂载点路径
 *   2. path_mount() 执行实际的挂载操作
 *   3. path_put() 释放 path 引用
 *
 * 【常见挂载标志】
 * - MS_RDONLY (0x0001):     只读挂载
 * - MS_NOSUID (0x0002):     禁止 setuid/setgid 位
 * - MS_NODEV  (0x0004):     禁止访问设备文件
 * - MS_NOEXEC (0x0008):     禁止执行程序
 * - MS_REMOUNT (0x0020):    重新挂载（修改挂载选项）
 *
 * 【返回值】
 * 成功返回 0，失败返回负的错误码（如 -ENOENT 挂载点不存在，-EBUSY 已挂载）
 */
int __init init_mount(const char *dev_name, const char *dir_name,
		const char *type_page, unsigned long flags, void *data_page)
{
	struct path path;
	int ret;

	/*
	 * 解析挂载点路径
	 * LOOKUP_FOLLOW: 如果挂载点是符号链接，跟随到实际目录
	 */
	ret = kern_path(dir_name, LOOKUP_FOLLOW, &path);
	if (ret)
		return ret;

	/*
	 * path_mount - VFS 层的挂载核心函数
	 * 将 dev_name 上的 type_page 文件系统挂载到 path 指向的目录上。
	 * data_page 传递文件系统特定的选项（如 "noatime,barrier=1"）。
	 */
	ret = path_mount(dev_name, &path, type_page, flags, data_page);

	/*
	 * path_put - 释放 path 的引用计数
	 * 对应 kern_path 中获取的引用，必须调用以防止泄漏。
	 */
	path_put(&path);
	return ret;
}

/*
 * init_umount - 内核初始化专用的 umount 操作
 * @name:  要卸载的挂载点路径（如 "/mnt"）
 * @flags: 卸载标志（MNT_FORCE, MNT_DETACH 等，定义在 include/linux/mount.h）
 *
 * 【功能说明】
 * 在内核初始化阶段卸载文件系统，例如：
 *   - 切换根文件系统前卸载 initramfs
 *   - 清理临时挂载点
 *
 * 【卸载标志】
 * - UMOUNT_NOFOLLOW (0x00000008): 如果挂载点是符号链接，不要跟随
 * - MNT_FORCE       (0x00000001): 强制卸载（即使有进程在使用）
 * - MNT_DETACH      (0x00000002): 延迟卸载（立即从命名空间分离，但等使用者释放）
 *
 * 【返回值】
 * 成功返回 0，失败返回负的错误码（如 -EBUSY 文件系统正在使用，-EINVAL 不是挂载点）
 */
int __init init_umount(const char *name, int flags)
{
	/*
	 * lookup_flags 的初始值：
	 * LOOKUP_MOUNTPOINT 表示要查找的路径必须是挂载点（否则返回 -EINVAL）
	 */
	int lookup_flags = LOOKUP_MOUNTPOINT;
	struct path path;
	int ret;

	/*
	 * 检查是否需要跟随符号链接
	 * 若用户未指定 UMOUNT_NOFOLLOW，则需要跟随符号链接到实际挂载点
	 */
	if (!(flags & UMOUNT_NOFOLLOW))
		lookup_flags |= LOOKUP_FOLLOW;

	ret = kern_path(name, lookup_flags, &path);
	if (ret)
		return ret;

	/*
	 * path_umount - VFS 层的卸载核心函数
	 * 将 path 指向的挂载点卸载。
	 */
	return path_umount(&path, flags);
	/* 注意：path_umount 内部会调用 path_put，所以这里不需要再调用 */
}

/*
 * init_chdir - 内核初始化专用的 chdir（改变当前工作目录）操作
 * @filename: 目标目录路径（如 "/root", "/mnt"）
 *
 * 【功能说明】
 * 改变当前进程（通常是 kernel_init 线程）的当前工作目录（cwd）。
 * 用于在执行 /sbin/init 之前将工作目录设置到正确的位置。
 *
 * 【与用户态 chdir 的区别】
 * - 用户态 chdir 通过系统调用进入内核，需要 copy_from_user
 * - init_chdir 直接操作内核路径，无需用户空间交互
 *
 * 【工作流程】
 *   1. kern_path() 解析目标目录路径
 *   2. path_permission() 检查是否有权限进入该目录（需要 MAY_EXEC | MAY_CHDIR）
 *   3. set_fs_pwd() 更新 current->fs->pwd（当前工作目录）
 *
 * 【权限检查】
 * MAY_EXEC:   需要对目录有执行权限（即可以 "进入" 该目录）
 * MAY_CHDIR:  需要 chdir 权限（通常与 MAY_EXEC 一起检查）
 *
 * 【返回值】
 * 成功返回 0，失败返回负的错误码（如 -ENOENT 目录不存在，-EACCES 权限不足）
 */
int __init init_chdir(const char *filename)
{
	struct path path;
	int error;

	/*
	 * 解析目标路径
	 * LOOKUP_FOLLOW | LOOKUP_DIRECTORY: 跟随符号链接，且结果必须是目录
	 */
	error = kern_path(filename, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &path);
	if (error)
		return error;

	/*
	 * path_permission - 检查路径权限
	 * @path: 要检查的路径
	 * @mask: 权限掩码（MAY_READ, MAY_WRITE, MAY_EXEC, MAY_CHDIR 等）
	 *
	 * 返回值：0 表示允许访问，负值表示拒绝（如 -EACCES）
	 */
	error = path_permission(&path, MAY_EXEC | MAY_CHDIR);
	if (!error)
		/*
		 * set_fs_pwd - 设置当前进程的工作目录
		 * @fs:   current->fs（进程的文件系统上下文结构）
		 * @path: 新的工作目录路径
		 *
		 * 更新 fs->pwd 指向新目录，同时正确处理引用计数。
		 */
		set_fs_pwd(current->fs, &path);

	path_put(&path);  /* 释放 path 引用 */
	return error;
}

/*
 * init_chroot - 内核初始化专用的 chroot（改变根目录）操作
 * @filename: 新的根目录路径（如 "/newroot"）
 *
 * 【功能说明】
 * 改变当前进程的根目录（root directory），使进程及其子进程看不到该目录外的文件。
 * 常用于容器隔离、沙箱环境、initramfs 切换等场景。
 *
 * 【与 pivot_root 的区别】
 * - chroot 仅改变进程的根目录视图（可通过 fchdir 到旧的 fd 突破）
 * - pivot_root 切换整个系统的根文件系统挂载（更安全，不可突破）
 *
 * 【安全检查】
 *   1. 权限检查：需要对目标目录有 MAY_EXEC | MAY_CHDIR 权限
 *   2. 特权检查：需要 CAP_SYS_CHROOT 能力（通常只有 root 可用）
 *   3. LSM 检查：调用 security_path_chroot() 进行额外的安全模块检查（如 SELinux）
 *
 * 【工作流程】
 *   1. kern_path() 解析目标路径
 *   2. path_permission() 检查基本权限
 *   3. ns_capable() 检查是否有 CAP_SYS_CHROOT 能力
 *   4. security_path_chroot() 进行 LSM（Linux Security Module）检查
 *   5. set_fs_root() 更新 current->fs->root（根目录）
 *
 * 【返回值】
 * 成功返回 0，失败返回负的错误码：
 *   -ENOENT: 目录不存在
 *   -EACCES: 权限不足
 *   -EPERM:  没有 CAP_SYS_CHROOT 能力
 */
int __init init_chroot(const char *filename)
{
	struct path path;
	int error;

	/* 解析目标路径（必须是目录） */
	error = kern_path(filename, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &path);
	if (error)
		return error;

	/* 检查对目标目录的访问权限 */
	error = path_permission(&path, MAY_EXEC | MAY_CHDIR);
	if (error)
		goto dput_and_out;  /* 跳转到清理代码 */

	/*
	 * -EPERM: Operation not permitted（操作不允许）
	 * 设置默认错误码，如果后续检查失败则返回此值
	 */
	error = -EPERM;

	/*
	 * ns_capable - 检查当前进程是否在指定的用户命名空间中拥有某能力
	 * @ns:  用户命名空间（current_user_ns() 返回当前进程的用户命名空间）
	 * @cap: 要检查的能力（CAP_SYS_CHROOT 表示可以改变根目录）
	 *
	 * 返回值：true 表示有能力，false 表示没有
	 *
	 * 【为什么要检查能力？】
	 * chroot 是特权操作，允许进程突破文件系统隔离。普通用户不应能调用，
	 * 否则可能逃逸出沙箱或访问不该访问的文件。
	 */
	if (!ns_capable(current_user_ns(), CAP_SYS_CHROOT))
		goto dput_and_out;

	/*
	 * security_path_chroot - LSM（Linux Security Module）钩子
	 * @path: 要 chroot 到的路径
	 *
	 * 返回值：0 表示允许，负值表示拒绝
	 *
	 * 【LSM 是什么？】
	 * LSM 是 Linux 安全模块框架，允许不同的安全策略（如 SELinux, AppArmor）
	 * 在内核中插入额外的安全检查。security_path_chroot 让这些模块可以
	 * 根据自己的策略决定是否允许 chroot 操作。
	 */
	error = security_path_chroot(&path);
	if (error)
		goto dput_and_out;

	/*
	 * set_fs_root - 设置当前进程的根目录
	 * @fs:   current->fs（进程的文件系统上下文）
	 * @path: 新的根目录路径
	 *
	 * 更新 fs->root 指向新的根目录，同时正确处理引用计数。
	 * 之后该进程及其子进程的所有绝对路径都相对于此新根。
	 */
	set_fs_root(current->fs, &path);

dput_and_out:
	/*
	 * 清理标签：无论成功还是失败都要释放 path 引用
	 * 这是内核常见的错误处理模式（goto error label）
	 */
	path_put(&path);
	return error;
}

/*
 * init_chown - 内核初始化专用的 chown（改变文件所有者）操作
 * @filename: 文件路径（如 "/dev/console"）
 * @user:     新的用户 ID（uid_t 类型）
 * @group:    新的组 ID（gid_t 类型）
 * @flags:    标志位（AT_SYMLINK_NOFOLLOW 等）
 *
 * 【功能说明】
 * 改变文件或目录的所有者（owner）和组（group）。
 * 常用于初始化阶段设置设备节点、配置文件的正确权限。
 *
 * 【标志位说明】
 * - AT_SYMLINK_NOFOLLOW (0x100): 如果文件是符号链接，不跟随，直接修改链接本身
 * - 0: 默认行为，跟随符号链接到实际文件
 *
 * 【工作流程】
 *   1. kern_path() 解析文件路径
 *   2. mnt_want_write() 请求对挂载点的写权限（只读挂载会失败）
 *   3. chown_common() 执行实际的 chown 操作
 *   4. mnt_drop_write() 释放写权限
 *
 * 【为什么需要 mnt_want_write/mnt_drop_write？】
 * 这是挂载点写保护机制：
 *   - 确保挂载点不是只读的（MS_RDONLY）
 *   - 防止在挂载点被标记为只读时进行写操作
 *   - 维护挂载点的写者计数，支持动态重新挂载
 *
 * 【返回值】
 * 成功返回 0，失败返回负的错误码：
 *   -ENOENT: 文件不存在
 *   -EROFS:  文件系统只读
 *   -EPERM:  没有权限修改所有者
 */
int __init init_chown(const char *filename, uid_t user, gid_t group, int flags)
{
	/*
	 * lookup_flags 根据 flags 参数决定是否跟随符号链接
	 * AT_SYMLINK_NOFOLLOW: 不跟随符号链接（lookup_flags = 0）
	 * 否则: 跟随符号链接（lookup_flags = LOOKUP_FOLLOW）
	 */
	int lookup_flags = (flags & AT_SYMLINK_NOFOLLOW) ? 0 : LOOKUP_FOLLOW;
	struct path path;
	int error;

	/* 解析文件路径 */
	error = kern_path(filename, lookup_flags, &path);
	if (error)
		return error;

	/*
	 * mnt_want_write - 请求对挂载点的写权限
	 * @mnt: 挂载点（从 path.mnt 获取）
	 *
	 * 返回值：0 表示获得写权限，-EROFS 表示只读挂载
	 *
	 * 【为什么要单独检查？】
	 * 挂载点可能被标记为只读（mount -o remount,ro），此时任何写操作
	 * （包括 chown）都应该被拒绝。
	 */
	error = mnt_want_write(path.mnt);
	if (!error) {
		/*
		 * chown_common - VFS 层的 chown 核心实现
		 * @path:  要修改的文件路径
		 * @user:  新的 uid（-1 表示不改变）
		 * @group: 新的 gid（-1 表示不改变）
		 *
		 * 会检查权限、调用文件系统的 setattr 方法、更新 inode 信息。
		 */
		error = chown_common(&path, user, group);

		/*
		 * mnt_drop_write - 释放对挂载点的写权限
		 * 必须与 mnt_want_write 配对使用，维护引用计数。
		 */
		mnt_drop_write(path.mnt);
	}

	path_put(&path);  /* 释放 path 引用 */
	return error;
}

/*
 * init_chmod - 内核初始化专用的 chmod（改变文件权限）操作
 * @filename: 文件路径（如 "/dev/null"）
 * @mode:     新的权限模式（umode_t 类型，如 0755, 0644）
 *
 * 【功能说明】
 * 改变文件或目录的访问权限（读、写、执行）。
 * 常用于初始化阶段设置设备节点、脚本的正确权限。
 *
 * 【权限模式说明】
 * mode 是一个 16 位的值，低 12 位表示权限：
 *   - 0o4000 (S_ISUID): setuid 位（执行时以文件所有者身份运行）
 *   - 0o2000 (S_ISGID): setgid 位（执行时以文件所属组身份运行）
 *   - 0o1000 (S_ISVTX): sticky 位（目录中只有所有者可删除文件）
 *   - 0o0700 (S_IRWXU): 所有者的 rwx 权限
 *   - 0o0070 (S_IRWXG): 组的 rwx 权限
 *   - 0o0007 (S_IRWXO): 其他人的 rwx 权限
 *
 * 【工作流程】
 *   1. kern_path() 解析文件路径（跟随符号链接）
 *   2. chmod_common() 执行实际的 chmod 操作
 *
 * 【注意事项】
 * 与 init_chown 不同，这里没有 mnt_want_write/mnt_drop_write，
 * 是因为 chmod_common 内部已经处理了写权限检查。
 *
 * 【返回值】
 * 成功返回 0，失败返回负的错误码：
 *   -ENOENT: 文件不存在
 *   -EROFS:  文件系统只读
 *   -EPERM:  没有权限修改权限
 */
int __init init_chmod(const char *filename, umode_t mode)
{
	struct path path;
	int error;

	/*
	 * 解析文件路径
	 * LOOKUP_FOLLOW: 跟随符号链接到实际文件
	 * （注意：与 init_chown 不同，这里总是跟随符号链接）
	 */
	error = kern_path(filename, LOOKUP_FOLLOW, &path);
	if (error)
		return error;

	/*
	 * chmod_common - VFS 层的 chmod 核心实现
	 * @path: 要修改的文件路径
	 * @mode: 新的权限模式
	 *
	 * 会检查权限、调用文件系统的 setattr 方法、更新 inode 的 i_mode。
	 */
	error = chmod_common(&path, mode);

	path_put(&path);  /* 释放 path 引用 */
	return error;
}

/*
 * init_eaccess - 内核初始化专用的 access（检查文件访问权限）操作
 * @filename: 文件路径（如 "/etc/fstab"）
 *
 * 【功能说明】
 * 检查当前进程是否有权限访问指定文件。
 * eaccess 是 "effective access" 的缩写，表示使用有效用户 ID（euid）而非实际用户 ID（uid）。
 *
 * 【工作流程】
 *   1. kern_path() 解析文件路径（跟随符号链接）
 *   2. path_permission() 检查 MAY_ACCESS 权限
 *
 * 【权限掩码】
 * MAY_ACCESS: 这是一个特殊的权限标志，表示"检查访问权限"。
 * 实际上会检查进程是否有读、写或执行该文件的权限。
 *
 * 【返回值】
 * 成功返回 0（有权限），失败返回负的错误码：
 *   -ENOENT: 文件不存在
 *   -EACCES: 没有访问权限
 */
int __init init_eaccess(const char *filename)
{
	struct path path;
	int error;

	/* 解析文件路径（跟随符号链接） */
	error = kern_path(filename, LOOKUP_FOLLOW, &path);
	if (error)
		return error;

	/*
	 * path_permission - 检查路径权限
	 * @path: 要检查的路径
	 * @mask: 权限掩码（MAY_ACCESS 表示检查基本访问权限）
	 *
	 * 返回值：0 表示有权限，负值表示无权限
	 */
	error = path_permission(&path, MAY_ACCESS);

	path_put(&path);  /* 释放 path 引用 */
	return error;
}

/*
 * init_stat - 内核初始化专用的 stat（获取文件状态）操作
 * @filename: 文件路径（如 "/dev/console"）
 * @stat:     输出参数，填充文件的状态信息（struct kstat *）
 * @flags:    标志位（AT_SYMLINK_NOFOLLOW, AT_NO_AUTOMOUNT 等）
 *
 * 【功能说明】
 * 获取文件或目录的元数据（metadata），如大小、权限、时间戳等。
 * kstat 是内核内部使用的 stat 结构，与用户空间的 struct stat 类似但不同。
 *
 * 【struct kstat 包含的信息】
 * - ino:      inode 号
 * - mode:     文件类型和权限
 * - nlink:    硬链接数
 * - uid/gid:  所有者和组 ID
 * - size:     文件大小（字节）
 * - atime/mtime/ctime: 访问、修改、状态改变时间
 * - blocks:   占用的磁盘块数
 * - blksize:  最优 I/O 块大小
 *
 * 【标志位说明】
 * - AT_SYMLINK_NOFOLLOW (0x100): 如果文件是符号链接，不跟随，返回链接本身的信息
 * - AT_NO_AUTOMOUNT (0x800):     不自动挂载（如果路径是一个自动挂载点）
 * - 0: 默认行为，跟随符号链接
 *
 * 【STATX_BASIC_STATS 说明】
 * 这是 statx 系统调用引入的概念，表示请求"基本统计信息"：
 * - STATX_TYPE:     文件类型（普通文件、目录、符号链接等）
 * - STATX_MODE:     权限位
 * - STATX_NLINK:    硬链接数
 * - STATX_UID/GID:  所有者
 * - STATX_ATIME/MTIME/CTIME: 时间戳
 * - STATX_INO:      inode 号
 * - STATX_SIZE:     文件大小
 * - STATX_BLOCKS:   块数
 *
 * 【返回值】
 * 成功返回 0，失败返回负的错误码：
 *   -ENOENT: 文件不存在
 *   -EACCES: 没有权限访问路径中的某个目录
 */
int __init init_stat(const char *filename, struct kstat *stat, int flags)
{
	/*
	 * lookup_flags 根据 flags 参数决定是否跟随符号链接
	 * AT_SYMLINK_NOFOLLOW: 不跟随符号链接（lookup_flags = 0）
	 * 否则: 跟随符号链接（lookup_flags = LOOKUP_FOLLOW）
	 */
	int lookup_flags = (flags & AT_SYMLINK_NOFOLLOW) ? 0 : LOOKUP_FOLLOW;
	struct path path;
	int error;

	/* 解析文件路径 */
	error = kern_path(filename, lookup_flags, &path);
	if (error)
		return error;

	/*
	 * vfs_getattr - VFS 层的 getattr 核心实现
	 * @path:  要查询的文件路径
	 * @stat:  输出参数，填充文件状态信息
	 * @request_mask: 请求的字段掩码（STATX_BASIC_STATS 表示基本信息）
	 * @flags: 附加标志（如 AT_NO_AUTOMOUNT）
	 *
	 * 返回值：0 成功，负值为错误码
	 *
	 * 【工作流程】
	 * 1. 调用文件系统的 getattr 方法（如 ext4_getattr）
	 * 2. 填充 kstat 结构的各个字段
	 * 3. 处理特殊情况（如块设备、字符设备的 size）
	 */
	error = vfs_getattr(&path, stat, STATX_BASIC_STATS,
			    flags | AT_NO_AUTOMOUNT);

	path_put(&path);  /* 释放 path 引用 */
	return error;
}

/*
 * init_mknod - 内核初始化专用的 mknod（创建设备节点）操作
 * @filename: 要创建的节点路径（如 "/dev/null", "/dev/console"）
 * @mode:     文件类型和权限（S_IFCHR | 0666 表示字符设备，权限 0666）
 * @dev:      设备号（major/minor 编码，通过 MKDEV(major, minor) 生成）
 *
 * 【功能说明】
 * 创建设备节点（device node）、FIFO（命名管道）或普通文件。
 * 在内核初始化阶段用于创建 /dev 下的设备文件。
 *
 * 【mode 参数说明】
 * mode 的高位表示文件类型：
 * - S_IFREG  (0100000): 普通文件
 * - S_IFCHR  (0020000): 字符设备（如 /dev/null, /dev/tty）
 * - S_IFBLK  (0060000): 块设备（如 /dev/sda）
 * - S_IFIFO  (0010000): FIFO（命名管道）
 * - S_IFSOCK (0140000): socket（不能用 mknod 创建）
 *
 * mode 的低位表示权限（0666 = rw-rw-rw-）
 *
 * 【dev 参数说明】
 * dev 是一个 32 位的设备号，包含：
 * - 主设备号（major）：高 12 位，标识设备驱动程序
 * - 次设备号（minor）：低 20 位，标识具体的设备实例
 *
 * 示例：
 *   MKDEV(1, 3) = /dev/null  （字符设备，主设备号 1，次设备号 3）
 *   MKDEV(1, 5) = /dev/zero  （字符设备，主设备号 1，次设备号 5）
 *   MKDEV(5, 1) = /dev/console （字符设备，主设备号 5，次设备号 1）
 *
 * 【CLASS 宏说明】
 * CLASS(filename_kernel, name)(filename):
 * 这是一个 C++ 风格的 RAII（Resource Acquisition Is Initialization）宏，
 * 在 C 中通过 __attribute__((cleanup)) 实现：
 *   - 自动将 filename 字符串转换为 struct filename * 对象
 *   - 函数退出时自动释放资源（类似 C++ 的析构函数）
 *
 * 【返回值】
 * 成功返回 0，失败返回负的错误码：
 *   -EEXIST: 文件已存在
 *   -EACCES: 没有权限在该目录创建文件
 *   -EPERM:  没有权限创建设备节点（需要 CAP_MKNOD 能力）
 */
int __init init_mknod(const char *filename, umode_t mode, unsigned int dev)
{
	/*
	 * CLASS(filename_kernel, name)(filename):
	 * 将 C 字符串转换为内核的 struct filename 对象，
	 * 函数退出时自动释放。name 是局部变量名。
	 */
	CLASS(filename_kernel, name)(filename);

	/*
	 * filename_mknodat - VFS 层的 mknodat 核心实现
	 * @dfd:  目录文件描述符（AT_FDCWD 表示当前工作目录）
	 * @name: 文件名（struct filename *）
	 * @mode: 文件类型和权限
	 * @dev:  设备号（仅对设备节点有效）
	 *
	 * 返回值：0 成功，负值为错误码
	 *
	 * 【为什么用 AT_FDCWD？】
	 * AT_FDCWD 是 *at 系列系统调用的约定，表示"相对于当前工作目录"。
	 * 虽然这里 filename 是绝对路径，但 AT_FDCWD 是标准占位符。
	 */
	return filename_mknodat(AT_FDCWD, name, mode, dev);
	/* CLASS 宏在函数退出时自动释放 name */
}

/*
 * init_link - 内核初始化专用的 link（创建硬链接）操作
 * @oldname: 已存在的文件路径（如 "/bin/busybox"）
 * @newname: 要创建的硬链接路径（如 "/bin/sh"）
 *
 * 【功能说明】
 * 创建硬链接（hard link）：为现有文件创建一个新的目录项（directory entry）。
 * 两个文件名指向同一个 inode，共享数据和元数据。
 *
 * 【硬链接 vs 符号链接】
 * - 硬链接：直接指向 inode，删除原文件不影响硬链接，不能跨文件系统
 * - 符号链接（软链接）：存储目标路径字符串，类似 Windows 快捷方式，可跨文件系统
 *
 * 【使用场景】
 * - BusyBox 风格的多合一工具：一个二进制文件，多个命令名（如 ls, cp, mv 都链接到 busybox）
 * - 共享库的版本管理（如 libc.so.6 链接到 libc-2.31.so）
 *
 * 【限制】
 * - 不能为目录创建硬链接（防止循环）
 * - 源文件和目标文件必须在同一文件系统
 *
 * 【返回值】
 * 成功返回 0，失败返回负的错误码：
 *   -ENOENT: 源文件不存在
 *   -EEXIST: 目标文件已存在
 *   -EXDEV:  跨文件系统（不允许）
 *   -EPERM:  尝试为目录创建硬链接
 */
int __init init_link(const char *oldname, const char *newname)
{
	/* 将两个路径字符串转换为 struct filename 对象 */
	CLASS(filename_kernel, old)(oldname);
	CLASS(filename_kernel, new)(newname);

	/*
	 * filename_linkat - VFS 层的 linkat 核心实现
	 * @olddfd: 旧文件的目录文件描述符（AT_FDCWD 表示当前工作目录）
	 * @old:    旧文件名
	 * @newdfd: 新文件的目录文件描述符（AT_FDCWD）
	 * @new:    新文件名
	 * @flags:  标志位（0 表示默认行为）
	 *
	 * 返回值：0 成功，负值为错误码
	 */
	return filename_linkat(AT_FDCWD, old, AT_FDCWD, new, 0);
	/* CLASS 宏自动释放 old 和 new */
}

/*
 * init_symlink - 内核初始化专用的 symlink（创建符号链接）操作
 * @oldname: 目标路径（可以是任意字符串，不要求文件存在）
 * @newname: 要创建的符号链接路径
 *
 * 【功能说明】
 * 创建符号链接（symbolic link，软链接）：创建一个特殊文件，内容是目标路径字符串。
 *
 * 【与硬链接的区别】
 * - 符号链接存储的是路径字符串，不是 inode
 * - 目标文件可以不存在（悬空链接，dangling symlink）
 * - 可以跨文件系统
 * - 可以为目录创建符号链接
 * - 删除原文件后，符号链接失效
 *
 * 【使用场景】
 * - 跨文件系统的链接（如 /usr/bin -> /mnt/usb/bin）
 * - 创建目录别名（如 /lib64 -> /lib）
 * - 兼容性链接（如 /bin/sh -> /bin/bash）
 *
 * 【返回值】
 * 成功返回 0，失败返回负的错误码：
 *   -EEXIST: 符号链接路径已存在
 *   -EACCES: 没有权限在目录中创建文件
 */
int __init init_symlink(const char *oldname, const char *newname)
{
	CLASS(filename_kernel, old)(oldname);
	CLASS(filename_kernel, new)(newname);

	/*
	 * filename_symlinkat - VFS 层的 symlinkat 核心实现
	 * @old:    目标路径（符号链接的内容）
	 * @newdfd: 符号链接文件的目录文件描述符（AT_FDCWD）
	 * @new:    符号链接文件名
	 *
	 * 返回值：0 成功，负值为错误码
	 */
	return filename_symlinkat(old, AT_FDCWD, new);
}

/*
 * init_unlink - 内核初始化专用的 unlink（删除文件）操作
 * @pathname: 要删除的文件路径（如 "/tmp/test.txt"）
 *
 * 【功能说明】
 * 删除文件（普通文件、符号链接等）。
 * 实际上是删除目录项（directory entry），如果这是最后一个指向该 inode 的链接，
 * 且没有进程打开该文件，则释放 inode 和数据块。
 *
 * 【硬链接与删除】
 * - 如果文件有多个硬链接，unlink 只删除其中一个链接，inode 引用计数减 1
 * - 只有当引用计数降为 0 且没有进程持有文件描述符时，文件才真正删除
 *
 * 【不能删除的文件】
 * - 目录（需要用 rmdir）
 * - 正在执行的二进制文件（返回 -ETXTBSY）
 * - 只读文件系统中的文件
 *
 * 【返回值】
 * 成功返回 0，失败返回负的错误码：
 *   -ENOENT:  文件不存在
 *   -EISDIR:  是目录（不能用 unlink 删除）
 *   -EACCES:  没有权限删除（需要对父目录有写权限）
 *   -ETXTBSY: 文件正在执行中
 */
int __init init_unlink(const char *pathname)
{
	CLASS(filename_kernel, name)(pathname);

	/*
	 * filename_unlinkat - VFS 层的 unlinkat 核心实现
	 * @dfd:  目录文件描述符（AT_FDCWD 表示当前工作目录）
	 * @name: 文件名（struct filename *）
	 *
	 * 返回值：0 成功，负值为错误码
	 */
	return filename_unlinkat(AT_FDCWD, name);
}

/*
 * init_mkdir - 内核初始化专用的 mkdir（创建目录）操作
 * @pathname: 要创建的目录路径（如 "/mnt/tmp"）
 * @mode:     目录权限（如 0755 = rwxr-xr-x）
 *
 * 【功能说明】
 * 创建新目录。
 *
 * 【目录权限说明】
 * - 0755 (rwxr-xr-x): 所有者可读写执行，组和其他人可读执行
 * - 0700 (rwx------): 仅所有者可访问（私有目录）
 * - 0777 (rwxrwxrwx): 所有人可访问（公共临时目录，配合 sticky 位使用）
 *
 * 【执行权限对目录的意义】
 * 对目录来说，执行权限（x）表示可以"进入"该目录，即：
 * - 可以 cd 到该目录
 * - 可以访问该目录中的文件（前提是知道文件名）
 * - 没有执行权限则无法访问目录中的任何文件，即使有读权限
 *
 * 【umask 的影响】
 * 实际创建的权限 = mode & ~umask
 * 例如：mode=0777, umask=0022 => 实际权限=0755
 *
 * 【返回值】
 * 成功返回 0，失败返回负的错误码：
 *   -EEXIST: 目录已存在
 *   -EACCES: 没有权限在父目录中创建目录
 *   -ENOSPC: 文件系统空间不足
 */
int __init init_mkdir(const char *pathname, umode_t mode)
{
	CLASS(filename_kernel, name)(pathname);

	/*
	 * filename_mkdirat - VFS 层的 mkdirat 核心实现
	 * @dfd:  目录文件描述符（AT_FDCWD）
	 * @name: 目录名
	 * @mode: 目录权限
	 *
	 * 返回值：0 成功，负值为错误码
	 */
	return filename_mkdirat(AT_FDCWD, name, mode);
}

/*
 * init_rmdir - 内核初始化专用的 rmdir（删除目录）操作
 * @pathname: 要删除的目录路径（如 "/tmp/olddir"）
 *
 * 【功能说明】
 * 删除空目录。
 *
 * 【限制】
 * - 目录必须为空（除了 . 和 .. 之外没有其他条目）
 * - 不能删除当前工作目录或根目录
 * - 不能删除挂载点
 *
 * 【与 unlink 的区别】
 * - unlink 用于删除文件，rmdir 用于删除目录
 * - rmdir 要求目录为空，unlink 不检查（文件无内容概念）
 *
 * 【返回值】
 * 成功返回 0，失败返回负的错误码：
 *   -ENOENT:    目录不存在
 *   -ENOTDIR:   路径不是目录
 *   -ENOTEMPTY: 目录不为空
 *   -EBUSY:     目录是挂载点或当前工作目录
 *   -EACCES:    没有权限删除（需要对父目录有写权限）
 */
int __init init_rmdir(const char *pathname)
{
	CLASS(filename_kernel, name)(pathname);

	/*
	 * filename_rmdir - VFS 层的 rmdir 核心实现
	 * @dfd:  目录文件描述符（AT_FDCWD）
	 * @name: 目录名
	 *
	 * 返回值：0 成功，负值为错误码
	 */
	return filename_rmdir(AT_FDCWD, name);
}

/*
 * init_utimes - 内核初始化专用的 utimes（修改文件时间戳）操作
 * @filename: 文件路径（如 "/etc/fstab"）
 * @ts:       时间戳数组（struct timespec64[2]）
 *            ts[0] = 访问时间（atime）
 *            ts[1] = 修改时间（mtime）
 *            传 NULL 表示设置为当前时间
 *
 * 【功能说明】
 * 修改文件的访问时间（atime）和修改时间（mtime）。
 * 状态改变时间（ctime）会自动更新为当前时间。
 *
 * 【三种时间戳说明】
 * - atime (Access Time):       最后访问时间（读取文件内容）
 * - mtime (Modification Time): 最后修改时间（写入文件内容）
 * - ctime (Change Time):       最后状态改变时间（修改 inode 元数据，如权限、所有者）
 *
 * 【为什么只能修改 atime 和 mtime？】
 * ctime 由内核维护，用户不能随意修改，防止伪造文件历史。
 *
 * 【使用场景】
 * - 备份/恢复工具保留原始时间戳
 * - touch 命令更新文件时间（不修改内容）
 * - make 等构建工具依赖时间戳判断是否需要重新编译
 *
 * 【返回值】
 * 成功返回 0，失败返回负的错误码：
 *   -ENOENT: 文件不存在
 *   -EACCES: 没有权限修改时间戳（需要是所有者或有 CAP_FOWNER 能力）
 *   -EROFS:  文件系统只读
 */
int __init init_utimes(char *filename, struct timespec64 *ts)
{
	struct path path;
	int error;

	/*
	 * kern_path - 解析文件路径
	 * flags=0: 不跟随符号链接（与 chmod/chown 不同）
	 */
	error = kern_path(filename, 0, &path);
	if (error)
		return error;

	/*
	 * vfs_utimes - VFS 层的 utimes 核心实现
	 * @path: 文件路径
	 * @ts:   时间戳数组（可为 NULL 表示当前时间）
	 *
	 * 返回值：0 成功，负值为错误码
	 *
	 * 【工作流程】
	 * 1. 检查权限（所有者或 CAP_FOWNER）
	 * 2. 调用文件系统的 setattr 方法
	 * 3. 更新 inode 的 atime/mtime，自动更新 ctime
	 */
	error = vfs_utimes(&path, ts);

	path_put(&path);  /* 释放 path 引用 */
	return error;
}

/*
 * init_dup - 内核初始化专用的 dup（复制文件描述符）操作
 * @file: 已打开的文件对象（struct file *）
 *
 * 【功能说明】
 * 为已打开的文件对象分配一个新的文件描述符（fd），新旧 fd 共享同一个文件对象。
 * 常用于重定向标准输入/输出（stdin/stdout/stderr）。
 *
 * 【文件描述符 vs 文件对象】
 * - 文件描述符（fd）：进程内的整数索引（0, 1, 2, ...）
 * - 文件对象（struct file）：内核中的实际文件结构，包含文件位置、打开模式等
 * - 多个 fd 可以指向同一个 file（通过 dup），共享文件位置指针
 * - 不同进程的 fd 可以指向同一个 file（通过 fork 继承）
 *
 * 【使用场景】
 * - 重定向标准输出：dup2(file_fd, 1) 让 stdout 指向文件
 * - shell 管道：dup2(pipe_fd, 1) 让 stdout 写入管道
 * - 保存/恢复文件描述符：old_fd = dup(1); ... dup2(old_fd, 1)
 *
 * 【与 dup2 的区别】
 * - dup:  分配最小的可用 fd
 * - dup2: 指定目标 fd，如果已占用则先关闭
 *
 * 【返回值】
 * 成功返回新的文件描述符（非负整数），失败返回负的错误码：
 *   -EMFILE: 进程打开的文件数达到上限
 *   -ENFILE: 系统打开的文件数达到上限
 */
int __init init_dup(struct file *file)
{
	int fd;

	/*
	 * get_unused_fd_flags - 获取一个未使用的文件描述符
	 * @flags: 标志位（0 表示默认行为，O_CLOEXEC 表示 exec 时关闭）
	 *
	 * 返回值：成功返回 fd（非负整数），失败返回负的错误码
	 *
	 * 【工作流程】
	 * 1. 在 current->files->fd_array 中查找最小的空闲 fd
	 * 2. 如果 fd < RLIMIT_NOFILE（进程文件描述符限制），则分配成功
	 * 3. 如果 fd 数组已满，扩展数组（最多到系统限制）
	 */
	fd = get_unused_fd_flags(0);
	if (fd < 0)
		return fd;  /* 分配失败，返回错误码 */

	/*
	 * fd_install - 将文件对象安装到文件描述符
	 * @fd:   要安装的文件描述符（由 get_unused_fd_flags 分配）
	 * @file: 文件对象（struct file *）
	 *
	 * 【注意】
	 * - fd_install 之后，file 的引用计数已由 get_file 增加
	 * - 这是一个单向操作，之后必须通过 close(fd) 释放
	 * - fd_install 不能失败（因为 fd 已被保留）
	 */
	fd_install(fd, get_file(file));

	/*
	 * get_file - 增加文件对象的引用计数
	 * 返回 file 本身（方便链式调用）
	 *
	 * 【为什么需要 get_file？】
	 * 原始的 file 可能被其他 fd 或进程持有引用，
	 * 新的 fd 也要持有一个独立的引用，防止 file 被过早释放。
	 */

	return 0;  /* 成功，新的 fd 已安装 */
	/*
	 * 注意：这里返回 0 而不是 fd，是因为调用者通常不关心具体的 fd 号，
	 * 只关心操作是否成功。如果需要返回 fd，可以改为 return fd;
	 */
}
