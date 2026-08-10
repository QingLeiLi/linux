# Linux 内核源码学习注释待办

本文件记录在关联源码核对中发现、但尚未纳入完整学习注释范围的文件。

## 待处理文件

- [ ] `kernel/sched/syscalls.c`
  - 当前状态：部分覆盖。
  - 建议范围：`yield_to()`、`sched_setaffinity()`、`sched_getaffinity()` 及相关调度策略系统调用。
  - 补注重点：返回值类别、权限检查、task 生命周期、亲和性掩码与 runqueue 锁协议。

- [ ] `fs/exec.c`
  - 当前状态：`__set_task_comm()` 相关学习注释缺失。
  - 建议范围：`__set_task_comm()` 以及 exec 过程中 task 名称更新的调用链。
  - 补注重点：无锁读写、NUL 终止与零填充、trace/perf 通知时序。

- [ ] `kernel/fork.c`
  - 当前状态：与 `task_struct` 创建和 `vfork_done` 有关的区域部分覆盖。
  - 建议范围：task 创建、vfork completion 建立、失败回滚与新 task 发布路径。
  - 补注重点：引用获取、父子同步、发布边界以及失败清理顺序。

- [ ] `kernel/exit.c`
  - 当前状态：与 `vfork_done`、task 退出和最终释放有关的区域部分覆盖。
  - 建议范围：vfork completion 唤醒、退出状态发布、父进程通知和 task 最终释放路径。
  - 补注重点：锁、RCU、引用计数、等待者竞争及不可回滚边界。

## 验收要求

处理每个文件时，遵循 `doc/linux-kernel-source-learning-methodology.md`，仅添加学习注释，
保留原有代码和英文注释，并按第 17 章完成全文件内容验收与修改安全检查。
