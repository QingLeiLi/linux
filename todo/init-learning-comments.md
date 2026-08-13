# init 学习注释任务清单

## 使用规则

本清单是 `init` 目录学习注释长任务的持久化进度源。发生会话中断或上下文压缩后，先读取
本文件并核对当前工作树，不凭对话记忆推断进度。

严格采用单文件闭环：

1. 开始前把目标文件标为 `[~]`，完整读取并建立函数、实体、英文注释和语义索引。
2. 仅追加中文学习注释；不改代码，不删除、改写或移动原有注释。
3. 按源码顺序以 1～5 个函数或一个连续配置/构建区域为一批修改，每批复读当前窗口并检查局部 diff。
4. 完整复读修改后的文件，按方法论第 17 章完成独立内容验收和修改安全检查。
5. 只有验收全部通过后才标为 `[x]`；一个 `[~]` 文件未闭环前不开始下一个文件。
6. 为核对契约而读取的跨目录源码只登记到关联读取记录，不在本任务中顺带修改。

状态：`[ ]` 未开始；`[~]` 正在处理或验收；`[x]` 已通过第 17 章单文件闭环；
`[-]` 已有学习注释并按用户要求直接跳过，本任务不重新验收或修改。

## 文件进度

- [x] `init/Kconfig`
- [x] `init/Makefile`
- [-] `init/calibrate.c`
- [x] `init/do_mounts.c`
- [x] `init/do_mounts.h`
- [x] `init/do_mounts_initrd.c`
- [x] `init/do_mounts_rd.c`
- [x] `init/init_task.c`
- [x] `init/initramfs.c`
- [x] `init/initramfs_internal.h`
- [x] `init/initramfs_test.c`
- [-] `init/main.c`
- [x] `init/noinitramfs.c`
- [-] `init/version-timestamp.c`
- [-] `init/version.c`

不纳入学习注释源码清单：`.gitignore` 是 Git 元数据，`.kunitconfig` 是三行测试配置片段；二者不含
需要按源码方法论展开的实现或构建协议。若后续扩大范围，再单独登记。

## 最近完成文件

- [x] `init/Makefile`
  - 构建职责：覆盖目录级编译标志、内建/配置对象选择、`mounts.o` 复合对象，以及临时和最终
    `UTS_VERSION` 的两阶段生成；明确 `version-timestamp.o` 由最终链接脚本单独构建和加入。
  - 配置与增量构建：说明 `CONFIG_BLK_DEV_INITRD` 的实现/桩二选一、可选校准与测试对象、
    target-specific auto 值、`FORCE + filechk` 的“每次计算、内容变化才更新时间戳”协议。
  - 第 17 章验收：完整复读 126 行文件；逐项核对所有变量、配置分支、生成目标、依赖边和原英文
    标题，能够仅靠注释复述对象选择、复合链接以及最终构建身份替换占位定义的过程。
  - 关联读取：`scripts/Kbuild.include` 的 `filechk`（现有学习注释缺失），确认临时文件比较与按内容
    更新；`scripts/link-vmlinux.sh` 的 `vmlinux_link()` 及最终对象构建入口（现有学习注释缺失），
    确认 `version-timestamp.o` 不经 `obj-y` 而在最终链接时加入；`scripts/build-version` 全文件
    （现有学习注释缺失），确认 `.version` 的读取、递增和回写；顶层 `Makefile` 的生成头规则
    （现有学习注释缺失），用于对照通用 `filechk` 调用方式。以上文件仅核对，均未修改。
  - 修改安全：新增 64 行、删除 0 行；剥离注释和空行后与 `HEAD` 完全一致，新增非注释行 0；
    `git diff --check` 通过；checkpatch 为 0 errors、0 warnings；工作树没有 `.config`，未执行构建。

## 最近完成文件

- [x] `init/Kconfig`
  - 全文件索引：已完整读取 2318 行并按物理顺序建立工具链探测、General setup、CPU/任务统计、
    调度、cgroup、namespace、initramfs、专家裁剪、perf/Rust 和尾部架构能力分区。
  - 已补注区域：文件总览；`CC_VERSION_TEXT` 至 `THREAD_INFO_IN_TASK` 的编译器、汇编器、链接器、
    Rust/pahole 能力与缺陷门槛；General setup 中 `BROKEN` 至 `BUILD_SALT` 的构建策略；
    `HAVE_KERNEL_GZIP` 至 `KERNEL_UNCOMPRESSED` 的架构能力与镜像压缩取舍；`DEFAULT_INIT`、
    `DEFAULT_HOSTNAME`、System V/POSIX IPC、watch queue、跨进程内存访问和审计入口；完整
    CPU/任务时间与统计菜单；CPU isolation、内核配置/头文件自描述、printk 缓冲区和
    `UCLAMP_TASK` 至 `SCHED_PROXY_EXEC` 的调度器特性；NUMA/TLB/编译器告警能力；完整
    cgroup 核心、调度及资源控制器；namespace、checkpoint/restore、relay、initramfs、
    bootconfig 与优化级别；链接器 section 回收/orphan 检查、sysctl 入口，以及 EXPERT
    基础裁剪项至 `BASE_SMALL`。
  - 后续已补注：`FUTEX` 至 `DEBUG_RSEQ` 的 futex、fd 事件接口、shmem、异步 I/O、
    membarrier/kcmp/rseq；cachestat、kallsyms、perf、系统数据验签、Rust、tracepoint，
    以及文件尾部所有 source 汇入和架构能力。
  - 第 17 章验收：完整文件现为 2850 行，252 个 config/menuconfig、3 个 choice、4 个 menu、
    13 个 source 均有邻接中文职责/依赖/副作用说明；61 行上游 `#` 注释逐字保留并有邻接中文
    翻译补充。抽查 `CGROUPS/MEMCG`、`BOOT_CONFIG`、`RUST`、`PERF_EVENTS`，可仅靠中文注释
    复述能力前提、下游 select、构建/运行期边界和失败性配置排除。
  - 修改安全：新增 532 行、删除 0 行；剥离注释和空行后与 `HEAD` 完全一致，新增非注释行 0；
    无重复 config；`git diff --check` 通过；checkpatch 为 0 errors、0 warnings；工作树没有
    `.config` 且 `scripts/kconfig/conf` 未构建，未执行 Kconfig 解析或目标构建。

- [x] `init/do_mounts.c`
  - 全文件索引：原文件 520 行、25 个物理函数声明（含 3 个配置关闭 stub）；状态分为启动参数、文件系统
    名单/单次挂载、NFS/CIFS 重试、nodev/block 分派、根设备等待/解析、namespace pivot 和
    rootfs ramfs/tmpfs 选择。实体包括 7 个全局/静态状态、NFS/CIFS 重试常量和 rootfs 类型表。
  - 已补注区域：文件级生命周期地图；`root_mountflags`、`saved_root_name`、`root_wait`、
    `ROOT_DEV`；所有启动参数、名单切分、单次/通用挂载、NFS/CIFS 与配置桩、nodev/block
    分派、等待/解析、`prepare_namespace()` pivot 和 rootfs ramfs/tmpfs 选择均已覆盖。
  - 第 17 章验收：完整复读当前 844 行；25 个物理函数声明均有紧邻中文函数头，7 个启动
    状态和 `rootfs_fs_type` 的生命周期/ownership 已说明；所有上游英文注释逐字保留并邻接
    翻译补充。抽查 `mount_root_generic()`、`wait_for_root()`、`prepare_namespace()`，可仅靠
    中文注释复述候选重试、设备发现竞态、挂载提交与不可事务回滚边界。
  - 关联读取：`init/do_mounts.h` 的声明、`create_dev()`、`initrd_load()` 和 `init_flush_fput()`
    （学习注释缺失，已转为下一目标）；`fs/init.c` 的 `init_mount()`、`init_chdir()`、
    `init_pivot_root()`、`init_umount()`（学习注释缺失）；`drivers/base/dd.c` 的
    `driver_probe_done()`/`wait_for_device_probe()`、`drivers/base/devtmpfs.c` 的
    `devtmpfs_mount()`、`drivers/md/md-autodetect.c` 的 `md_run_setup()`（所读区域学习注释缺失）。
    关联文件仅核对，除已纳入目录清单的 do_mounts.h 外均未修改。
  - 修改安全：新增 324 行、删除 0 行；去除注释和空白后与 `HEAD` 完全一致；
    `git diff --check` 通过；checkpatch 0 errors，74 项均为中文 UTF-8 计宽行宽 warning；
    工作树无 `.config`，未执行目标对象构建。

## 最近完成文件

- [x] `init/do_mounts.h`
  - 第 17 章验收：完整复读当前 100 行；两个公共声明、共享标志、`create_dev()`、RAM disk
    和 initrd 启用声明/关闭桩、`init_flush_fput()` 均有紧邻中文契约；唯一英文注释逐字保留
    并邻接翻译。配置桩的返回/副作用和 unlink→mknod、delayed_fput→task_work 顺序已覆盖。
  - 关联读取：`init/do_mounts_rd.c:rd_load_image()`、`init/do_mounts_initrd.c:initrd_load()`，
    用于核对返回值、资源和调用边界；两者当时学习注释缺失，均已在目录清单中排队。
  - 修改安全：新增 58 行、删除 0 行；去除注释和空白后与 `HEAD` 完全一致；
    `git diff --check` 通过；checkpatch 0 errors，7 项均为中文 UTF-8 计宽行宽 warning；
    工作树无 `.config`，未执行目标对象构建。

## 最近完成文件

- [x] `init/do_mounts_initrd.c`
  - 第 17 章验收：完整复读当前 111 行；4 个函数、映射后/物理 initrd 边界、布局许可和
    mount 开关均有紧邻中文说明；`initrdmem=` 解析提交、`initrd=` wrapper、ram0 装载和
    无条件 unlink 的一次性语义已覆盖。唯一英文块注释逐字保留并邻接翻译。
  - 关联读取：`include/linux/initrd.h` 的全局声明与 `init/initramfs.c` 的物理地址转换写入点，
    用于确认 `initrd_start/end` 与 `phys_initrd_start/size` 的边界；所读区域学习注释缺失，未修改。
  - 修改安全：新增 48 行、删除 0 行；去除注释和空白后与 `HEAD` 完全一致；
    `git diff --check` 通过；checkpatch 0 errors，9 项均为中文 UTF-8 计宽行宽 warning；
    工作树无 `.config`，未执行目标对象构建。

## 最近完成文件

- [x] `init/do_mounts_rd.c`
  - 第 17 章验收：完整复读当前 441 行；8 个函数均有紧邻中文契约，格式探测复用缓冲区、
    `nblocks` 实为 KiB、原始复制/同步解压分支、全局 file/offset 有效期、短读写边界及
    `done`/`noclose_input`/`out` 资源栈均已覆盖；所有英文注释逐字保留并邻接翻译补充。
  - 抽查 `identify_ramdisk_image()`、`rd_load_image()`、`crd_load()`，可仅靠中文注释复述
    magic 探测顺序、压缩哨兵、容量检查、回调 ownership 和不可恢复的缺解压器 panic。
  - 关联读取：`init/do_mounts_initrd.c:initrd_load()` 和 `init/do_mounts.h:rd_load_image()`，
    用于确认调用者对 0/1 返回值的解释和 `/dev/ram` 临时节点生命周期；两文件现已完成注释。
  - 修改安全：新增 126 行、删除 0 行；去除注释和空白后与 `HEAD` 完全一致；
    `git diff --check` 通过；checkpatch 0 errors，31 项均为中文 UTF-8 计宽行宽 warning；
    工作树无 `.config`，未执行目标对象构建。

## 最近完成文件

- [x] `init/init_task.c`
  - 第 17 章验收：按静态实体而非函数建立完整索引；`init_signals`、`init_sighand`、
    `init_task_exec_state`、shadow call stack、`init_groups`、`init_cred`、`init_task` 和外置
    `init_thread_info` 均有邻接中文说明。初始 idle task 的永久生命周期、自指拓扑、引用钉住、
    `active_mm` 借用、RCU 凭据、signal/sighand、PID 与各配置字段的空表/哨兵协议已覆盖。
  - 原英文注释：4 处英文块注释和 2 处英文行尾注释均逐字保留，并由邻接中文解释其启动期含义。
  - 修改安全：新增 107 行、删除 0 行；去除注释和空白后与 `HEAD` 的 SHA-256 完全一致；
    `git diff --check` 通过；checkpatch 0 errors，27 项均为中文 UTF-8 计宽行宽 warning；
    工作树无 `.config`，未执行目标对象构建。

## 最近完成文件

- [x] `init/initramfs.c`
  - 第 17 章验收：完整复读当前 1002 行；30 个唯一函数名、40 个配置分支物理声明均有
    紧邻中文契约。内置/外部输入顺序、newc/crc 头和校验、跨解压窗口收集、8 状态 cpio
    机、硬链接哈希、目录 mtime 逆序回放、各 VFS 对象创建、首错传播和资源释放均已覆盖。
  - 启动后半程：物理 initrd 页对齐预留、虚拟地址发布、retain sysfs、crashkernel 重叠区
    部分释放、旧式 RAM-disk 降级、LSM 通知、专属 async domain 和 cookie 等待边界已说明。
  - 原英文注释：内核写入上限、状态机标题、cpio 名字检查、TRAILER 缺失清理、initrd 预留/
    转换、crashkernel 和过早等待等原注释逐字保留，并有邻接中文对照。
  - 关联读取：`init/initramfs_internal.h` 的声明与 `CPIO_HDRLEN`（下一目标）；
    `include/linux/async.h`、`kernel/async.c:async_synchronize_cookie_domain()`，确认 cookie 上界；
    `include/linux/decompress/generic.h`，确认 flush/posp ownership；
    `security/security.c:security_initramfs_populated()`，确认 LSM 通知时点。以上均仅核对未修改。
  - 修改安全：新增 210 行、删除 0 行；去除注释和空白后与 `HEAD` 的 SHA-256 完全一致；
    `git diff --check` 通过；checkpatch 0 errors，88 项均为中文 UTF-8 计宽行宽 warning；
    工作树无 `.config`，未执行目标对象构建。

## 最近完成文件

- [x] `init/initramfs_internal.h`
  - 第 17 章验收：完整复读当前 16 行；唯一公共声明明确输入只读且不转移所有权、成功/失败
    返回值、静态错误字符串生命周期、`__initdata` 单实例不可重入和 init 段回收后禁止调用；
    `CPIO_HDRLEN` 说明 6 字节 magic 与 13 个八位十六进制字段的 110 字节构成。
  - 修改安全：新增 9 行、删除 0 行；去除注释和空白后与 `HEAD` 的 SHA-256 完全一致；
    `git diff --check` 通过；checkpatch 0 errors，6 项均为中文 UTF-8 计宽行宽 warning；
    工作树无 `.config`，未执行目标对象构建。

## 最近完成文件

- [x] `init/initramfs_test.c`
  - 第 17 章验收：完整复读当前 676 行；11 个测试/辅助函数均有紧邻中文契约，归档字段模型、
    标准/畸形头模板、名字和 body 四字节对齐、测试缓冲 ownership 与 rootfs 清理路径已覆盖。
  - 测试协议：基线元数据/mtime、未终止名字、普通文件内容、crc 成败与无回滚副作用、缺少
    TRAILER 的硬链接、1000 项连续状态、4 KiB 名字填充、PATH_MAX 超限跳过、0x 字段拒绝，
    以及 suite 与异步启动解包的串行化边界均已说明；原英文回归背景逐字保留并邻接翻译。
  - 修改安全：新增 91 行、删除 0 行；去除注释和空白后与 `HEAD` 的 SHA-256 完全一致；
    `git diff --check` 通过；checkpatch 0 errors，41 项均为中文 UTF-8 计宽行宽 warning；
    工作树无 `.config`，未执行 KUnit 构建或运行。

## 最近完成文件

- [x] `init/noinitramfs.c`
  - 第 17 章验收：完整复读当前 55 行；唯一 `default_rootfs()` 说明在无 initrd 支持时按顺序
    开放 usermode helper、创建 `/dev`、5:1 的 `/dev/console` 和 `/root`，覆盖权限、返回值、
    首错传播、VFS ownership 与失败不回滚边界；原英文说明逐字保留并邻接翻译。
  - 关联复核：纠正 `init/Makefile` 早先新增注释中的事实错误，明确本对象提供最小 rootfs
    initcall 而非 `unpack_to_rootfs()` 桩；与 `initramfs.o` 的互斥选择保持不变且未改构建代码。
  - 修改安全：新增 13 行、删除 0 行；去除注释和空白后与 `HEAD` 的 SHA-256 完全一致；
    `git diff --check` 通过；checkpatch 0 errors，5 项均为中文 UTF-8 计宽行宽 warning；
    工作树无 `.config`，未执行目标对象构建。

## 当前文件

- [x] `init` 目录总验收
  - 15 个登记文件均已归档：11 个完成第 17 章闭环，4 个按用户要求或因已有学习注释跳过，
    没有处理中或待处理源码；`.gitignore` 与 `.kunitconfig` 的排除边界已复核。
  - 11 个修改文件逐一剥离注释/空白后与 `HEAD` 代码令牌哈希一致，合计新增 1582 行、
    删除 0 行；因此原代码和原英文注释均未删除、改写或移动。
  - `git diff --check` 通过；合并 checkpatch 为 0 errors、264 项均为中文 UTF-8 计宽行宽
    warning；新增 diff 中不存在 `中文说明：`、`背景知识：` 等禁止的语言标签。
  - 配置互斥复核确认 `initramfs.o` 与 `noinitramfs.o` 分别承担完整归档展开和最小 rootfs
    创建；工作树无 `.config`，目录所有对象构建与 KUnit 运行统一记录为 `NO_CONFIG`。
  - 工作树中的 `kernel/dma/swiotlb.c` 与 `todo/kernel-dma-learning-comments.md` 是既有无关修改，
    本任务未触碰；本清单仅记录 `init` 目录工作。

## 目录状态

- [x] `init` 共登记 15 个源码/构建文件：4 个按用户要求或既有学习注释跳过，11 个已完成
  第 17 章单文件闭环并通过目录级独立总验收；目录已达到标准。
