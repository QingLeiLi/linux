# Linux 静态分支（Static Key / Jump Label）机制详解

## 目录

1. [背景：为什么需要静态分支](#1-背景为什么需要静态分支)
2. [整体架构](#2-整体架构)
3. [开发者如何使用](#3-开发者如何使用)
4. [jump_entry 的形成过程](#4-jump_entry-的形成过程)
5. [汇编代码逐行解析](#5-汇编代码逐行解析)
6. [链接阶段](#6-链接阶段)
7. [jump_label_init：建立索引](#7-jump_label_init建立索引)
8. [运行时 enable/disable](#8-运行时-enabledisable)
9. [相对偏移的设计原因](#9-相对偏移的设计原因)
10. [各架构的差异](#10-各架构的差异)
11. [相关源文件](#11-相关源文件)

---

## 1. 背景：为什么需要静态分支

内核热路径（如网络收包、调度器、系统调用入口）中大量存在特性开关：

```c
void do_something(void) {
    if (tracing_enabled)   // 每秒可能执行数百万次
        trace();
    // ... 真正的工作
}
```

普通条件跳转的 CPU 开销：
- 读取内存变量 `tracing_enabled`
- 比较值
- 条件跳转（分支预测，预测错误则流水线清空）

在 `tracing_enabled` 绝大多数时候为 `false` 的场景下，每次都承担这些开销是浪费。

**理想状态**：特性关闭时，这个 `if` 在机器码层面完全不存在；特性开启时，才变成跳转指令。

这就是静态分支（Static Key / Jump Label）机制的目标。

---

## 2. 整体架构

```
开发者代码
    static_branch_unlikely(&tracing_key)
            ↓ 宏展开
    arch_static_branch()  [内核架构代码]
            ↓ 内联汇编
    .text:   1: nop              ← 可被 patch 的指令
    __jump_table: [code][target][key]  ← jump_entry 记录
            ↓ 汇编器计算相对偏移
    目标文件 .o
            ↓ 链接器合并所有 __jump_table
    内核镜像：__start___jump_table ~ __stop___jump_table
            ↓ jump_label_init()
    建立索引：key->entries 链表
            ↓ static_branch_enable/disable()
    原地 patch 机器码：NOP ↔ JMP
```

---

## 3. 开发者如何使用

### 第一步：声明 static_key

```c
// 声明一个默认关闭的 key（对应初始 NOP）
DEFINE_STATIC_KEY_FALSE(tracing_key);

// 声明一个默认开启的 key（对应初始 JMP）
DEFINE_STATIC_KEY_TRUE(rcu_expedited_key);
```

### 第二步：用专用宏做条件判断

```c
// 用这个宏 → 生成 jump_entry，走 patch 机制
if (static_branch_unlikely(&tracing_key))
    trace();

if (static_branch_likely(&rcu_expedited_key))
    do_fast_path();

// 用普通 if → 普通条件跳转，不生成 jump_entry，不走 patch 机制
if (tracing_enabled)
    trace();
```

`likely` 和 `unlikely` 的语义：

| 宏 | 含义 | 初始指令 |
|----|------|----------|
| `static_branch_unlikely(&key)` | 特性默认关闭，走分支是少数情况 | NOP |
| `static_branch_likely(&key)` | 特性默认开启，走分支是多数情况 | JMP |

### 第三步：运行时开关

```c
static_branch_enable(&tracing_key);   // NOP → JMP，之后每次执行直接跳转
static_branch_disable(&tracing_key);  // JMP → NOP，之后每次执行直接跳过
```

---

## 4. jump_entry 的形成过程

### 数据结构

```c
// include/linux/jump_label.h
struct jump_entry {
    s32 code;    // NOP/JMP 指令相对本字段的偏移
    s32 target;  // 跳转目标（分支代码入口）相对本字段的偏移
    long key;    // static_key 变量相对本字段的偏移
};
```

所有字段都是**相对偏移**而非绝对地址，原因见第 9 节。

### 形成流程

```
1. 开发者写 static_branch_unlikely(&key)

2. 宏展开为 arch_static_branch(&key, false)
   （由 include/linux/jump_label.h 中的 static_branch_unlikely 宏决定）

3. arch_static_branch 是架构相关内联函数，展开为内联汇编
   （由 arch/arm64/include/asm/jump_label.h 或对应架构实现）

4. 汇编器处理内联汇编：
   - 在 .text section 生成一条 NOP 指令，标记为 "1:"
   - 切换到 __jump_table section
   - 计算三个相对偏移并写入（此时文件内地址已知，直接计算）
   - 切回 .text section

5. 链接器将所有目标文件的 __jump_table section 合并
```

---

## 5. 汇编代码逐行解析

### arm64 实现（arch/arm64/include/asm/jump_label.h）

```c
#define JUMP_TABLE_ENTRY(key, label)
    ".pushsection __jump_table, \"aw\"\n\t"
    ".align       3\n\t"
    ".long        1b - ., " label " - .\n\t"
    ".quad        " key " - .\n\t"
    ".popsection\n\t"

#define ARCH_STATIC_BRANCH_ASM(key, label)
    "1:   nop\n\t"
    JUMP_TABLE_ENTRY(key, label)

static __always_inline bool arch_static_branch(struct static_key *key, bool branch)
{
    asm goto(
        ARCH_STATIC_BRANCH_ASM("%c0", "%l[l_yes]")
        :: "i"(key) :: l_yes
    );
    return false;   // NOP 时执行到这里
l_yes:
    return true;    // 被 patch 成 JMP 后执行到这里
}
```

逐行解析：

```asm
1:  nop
```
在 `.text` section 生成一条 NOP 指令（4 字节，arm64 固定指令长度）。
`1:` 是**数字标签**——可以重复定义，`1b` 引用"向上最近的 1"，`1f` 引用"向下最近的 1"。
同一文件里可以有任意多个 `1:` 标签，互不干扰，正适合每个 `static_branch_xxx()` 调用处都需要一个标签的场景。

```asm
.pushsection __jump_table, "aw"
```
将后续汇编内容写入 `__jump_table` section 而非当前 `.text` section。
`"aw"` 是 section 属性：`a`=allocatable（链接进内存映像），`w`=writable（`jump_label_init` 需要修改内容）。
`.popsection` 之前的所有内容都进入 `__jump_table`，不影响 `.text`。

```asm
.align 3
```
将当前写入位置对齐到 `2^3 = 8` 字节边界。
arm64 上 `struct jump_entry` 包含一个 `long key` 字段（8 字节），整体需要 8 字节对齐，否则 CPU 访问会跨 cache line 或触发对齐异常。

```asm
.long 1b - .
```
`1b` = 上方 NOP 指令的地址。
`.` = 当前写入位置（即这个 `.long` 字段自身的地址）。
`1b - .` = NOP 相对本字段的有符号偏移，汇编器在编译时直接计算，写入 4 字节（`s32`）。
这个值对应 `struct jump_entry.code`。

```asm
label " - .
```
`label` 是 `asm goto` 的跳转标签（`l_yes`）的地址，即分支代码的入口。
同样计算相对偏移写入 4 字节，对应 `struct jump_entry.target`。
注意：`1b - .` 和 `label - .` 在同一行用逗号分隔，汇编器依次写入两个 `.long`，效果等同于分两行写。

```asm
.quad key - .
```
`key` 是 `static_key` 变量的地址（由 `asm goto` 的 `"i"(key)` 约束传入）。
`.quad` = 8 字节，arm64 是 64 位架构，指针 8 字节。
对应 `struct jump_entry.key`。

```asm
.popsection
```
切回之前的 section（`.text`），后续代码继续写入 `.text`。

---

### OpenRISC 实现（arch/openrisc/include/asm/jump_label.h）

```c
#define JUMP_TABLE_ENTRY(key, label)
    ".pushsection __jump_table, \"aw\"\n\t"
    ".align       4\n\t"
    ".long        1b - ., " label " - .\n\t"
    ".long        " key " - .\n\t"
    ".popsection\n\t"
```

与 arm64 的差异：
- `.align 4`：对齐到 `2^4 = 16` 字节（OpenRISC 的对齐要求不同）
- `key` 字段用 `.long`（4 字节）而非 `.quad`（8 字节）：OpenRISC 是 32 位架构，指针 4 字节

---

## 6. 链接阶段

链接器将所有目标文件的 `__jump_table` section 合并为一个连续数组：

```
目标文件 a.o: [entry_a0][entry_a1]
目标文件 b.o: [entry_b0]
目标文件 c.o: [entry_c0][entry_c1][entry_c2]
                    ↓ 链接
内核镜像 __jump_table: [entry_a0][entry_a1][entry_b0][entry_c0][entry_c1][entry_c2]
                        ↑                                                           ↑
              __start___jump_table                                    __stop___jump_table
```

`__start___jump_table` 和 `__stop___jump_table` 是链接脚本自动生成的首尾符号，
`jump_label_init` 通过这两个符号遍历整张表。

**相对偏移在链接后仍然正确**：
链接时 section 重新排列，符号绝对地址改变，但同一目标文件内两个符号之间的相对距离不变：
```
链接前：NOP 在偏移 0x10，entry 在偏移 0x30，code = 0x10 - 0x30 = -0x20
链接后：NOP 在地址 0x1010，entry 在地址 0x1030，code = 0x1010 - 0x1030 = -0x20 ✓
```

---

## 7. jump_label_init：建立索引

`kernel/jump_label.c`，在 `start_kernel()` 极早期调用。

### 第一步：排序

```c
jump_label_sort_entries(iter_start, iter_stop);
```

按 `key` 地址升序排序，使同一个 `static_key` 的所有 `jump_entry` 连续排列。
目的：建立 `key→entries` 索引时只需线性扫描，遇到新 key 才更新指针。

**相对偏移架构的特殊处理**：
排序时交换两个 entry 的位置，它们的偏移值（相对自身地址计算）会因位置变化而失效。
`jump_label_swap()` 在交换时修正偏移值：
```c
jea->code = jeb->code - delta;  // delta = 两个 entry 的地址差
```
确保交换后每个 entry 的偏移值仍然指向正确的 NOP 指令位置。

### 第二步：修正初始机器码

```c
if (jump_label_type(iter) == JUMP_LABEL_NOP)
    arch_jump_label_transform_static(iter, JUMP_LABEL_NOP);
```

编译器生成的 NOP 位置，实际机器码可能不是标准 NOP（依赖编译器行为）。
此处强制将应为 NOP 的位置写入正确的 NOP 机器码，确保与 `key->enabled` 的初始值一致。

还原运行时地址：
```c
// jump_entry_code() 的实现
return (unsigned long)&entry->code + entry->code;
// &entry->code：这个字段自身的运行时地址（KASLR 后的真实地址）
// entry->code：存储的相对偏移
// 两者相加 = NOP 指令的真实运行时地址
```

### 第三步：标记 init section

```c
in_init = init_section_contains((void *)jump_entry_code(iter), 1);
jump_entry_set_init(iter, in_init);
```

`__init` section 的代码在内核初始化完成后被释放。
若 NOP 指令位于 `__init` section，之后禁止再对该地址 patch，否则写入已释放内存会崩溃。

### 第四步：建立 key→entries 链表

```c
iterk = jump_entry_key(iter);
if (iterk == key)
    continue;
key = iterk;
static_key_set_entries(key, iter);  // key->entries 指向该 key 的第一个 entry
```

同一 key 的 entry 因已排序而连续，`key->entries` 只需指向第一个，
后续通过 `jump_entry_key(entry) == key` 判断是否到达末尾。

---

## 8. 运行时 enable/disable

```c
static_branch_enable(&tracing_key);
```

内部调用链：
```
static_branch_enable()
    → static_key_enable()
        → static_key_enable_cpuslocked()
            → jump_label_update(key)
                → 遍历 key->entries 链表
                    → arch_jump_label_transform(entry, JUMP_LABEL_JMP)
                        → 生成 B <target> 指令机器码
                        → aarch64_insn_patch_text_nosync(addr, insn)
                            → 原地改写内核 .text section 的机器码
    → kick_all_cpus_sync()
        → 向所有 CPU 发送 IPI，刷新指令缓存
        → 确保所有 CPU 看到新指令
```

**为什么 enable/disable 代价昂贵**：
- 修改内核代码段（需要临时修改页表权限或使用特殊写入接口）
- 向所有 CPU 发 IPI（中断所有核）
- 等待所有 CPU 确认指令缓存刷新完成

这是有意为之的权衡：**enable/disable 极少发生，执行路径极频繁**。
每次 enable/disable 的高昂代价换取热路径上永久的零开销。

---

## 9. 相对偏移的设计原因

`jump_entry` 的三个字段全部存储相对偏移而非绝对地址，原因：

**KASLR（内核地址随机化）**：
内核启动时基地址随机，所有符号的绝对地址都会变化。
相对偏移不受基地址影响，链接后正确，KASLR 后仍然正确。

**节省空间（64 位架构）**：
绝对地址需要 8 字节，`s32` 相对偏移只需 4 字节。
`code` 和 `target` 字段从 8 字节缩小到 4 字节，整个 `struct jump_entry` 更紧凑，
大型内核的 `__jump_table` 可能包含数万条 entry，节省空间可观。

**使用时还原**：
```c
// jump_entry_code()
(unsigned long)&entry->code + entry->code

// jump_entry_target()
(unsigned long)&entry->target + entry->target

// jump_entry_key()
(unsigned long)&entry->key + (entry->key & ~3UL)  // 低2位用于存标志位
```

---

## 10. 各架构的差异

| 架构 | key 字段大小 | .align | 特殊处理 |
|------|-------------|--------|----------|
| arm64 | `.quad`（8字节） | 3（8字节对齐） | `HAVE_ARCH_JUMP_LABEL_RELATIVE`，排序时需修正偏移 |
| OpenRISC | `.long`（4字节） | 4（16字节对齐） | 32位架构，指针4字节 |
| x86_64 | `.quad`（8字节） | 3（8字节对齐） | 类似 arm64 |

不支持 `CONFIG_JUMP_LABEL` 时（无 `asm goto` 支持）：
```c
// 退化为普通条件跳转，没有 __jump_table，没有 patch 机制
#define static_branch_unlikely(x)  unlikely_notrace(static_key_enabled(&(x)->key))
```

---

## 11. 相关源文件

| 文件 | 内容 |
|------|------|
| `include/linux/jump_label.h` | `static_branch_likely/unlikely` 宏、`struct jump_entry`、`struct static_key` |
| `kernel/jump_label.c` | `jump_label_init()`、`static_key_enable/disable()`、排序和索引建立 |
| `arch/arm64/include/asm/jump_label.h` | arm64 的 `arch_static_branch()`、`JUMP_TABLE_ENTRY` 宏 |
| `arch/arm64/kernel/jump_label.c` | arm64 的 `arch_jump_label_transform_queue()`，生成并写入 NOP/JMP 机器码 |
| `arch/openrisc/include/asm/jump_label.h` | OpenRISC（32位）的 `JUMP_TABLE_ENTRY` 宏，key 字段用 `.long` |
