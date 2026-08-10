# kernel/irq 学习注释任务清单

## 使用规则

本清单是 `kernel/irq` 学习注释长任务的持久化进度源。发生会话中断或上下文丢失时，先读取
本文件，再检查当前工作树；不得仅凭对话记忆推断进度。

采用严格的单文件闭环：

1. 开始文件前，将该文件从 `[ ]` 改为 `[~]`，并确认没有来源不明的重叠改动。
2. 完整读取原文件，按 `doc/linux-kernel-source-learning-methodology.md` 仅添加学习注释。
4. 完成下列文件级验收后，才可把 `[~]` 改为 `[x]`：
   - 按方法论第 17 章通过验收
5. 一个 `[~]` 文件未完成前，不开始下一个文件。完成后立即更新本清单的状态和验收记录。

状态含义：`[ ]` 尚未通过 skill 第 17 章文件级闭环（所在章节区分“已补注待复验”与“未开始”）；
`[~]` 正在处理或复验；`[x]` 已按第 17 章完成独立内容验收与修改安全检查。

## 已完成注释、待 skill 第 17 章复验

- [x] `kernel/irq/settings.h`
- [x] `kernel/irq/internals.h`
- [x] `kernel/irq/irqdesc.c`
- [x] `kernel/irq/handle.c`
- [x] `kernel/irq/chip.c`
- [~] `kernel/irq/manage.c`
- [ ] `kernel/irq/spurious.c`
- [ ] `kernel/irq/resend.c`
- [ ] `kernel/irq/migration.c`
- [ ] `kernel/irq/affinity.c`
- [ ] `kernel/irq/generic-chip.c`

## 待处理文件

- [ ] `kernel/irq/irqdomain.c`
- [ ] `kernel/irq/msi.c`
- [ ] `kernel/irq/cpuhotplug.c`
- [ ] `kernel/irq/pm.c`
- [ ] `kernel/irq/kexec.c`
- [ ] `kernel/irq/ipi.c`
- [ ] `kernel/irq/ipi-mux.c`
- [ ] `kernel/irq/proc.c`
- [ ] `kernel/irq/debugfs.c`
- [ ] `kernel/irq/devres.c`
- [ ] `kernel/irq/autoprobe.c`
- [ ] `kernel/irq/matrix.c`
- [ ] `kernel/irq/irq_sim.c`
- [ ] `kernel/irq/irq_test.c`
- [ ] `kernel/irq/dummychip.c`
- [ ] `kernel/irq/debug.h`
- [ ] `kernel/irq/debugfs.h`
- [ ] `kernel/irq/proc.h`
- [ ] `kernel/irq/Kconfig`
- [ ] `kernel/irq/Makefile`

## 下一步

先从 `kernel/irq/settings.h` 开始，对“已完成注释”文件逐个补做 skill 第 17 章强制验收；每个
文件验收通过后才改为 `[x]`。11 个既有文件全部复验完成前，不开始 `kernel/irq/irqdomain.c`。
