# VS Code clangd 配置

本文说明如何让宿主机上的 VS Code/clangd 使用容器内核构建产生的真实编译
参数。目标是让定义跳转、引用查找、补全和诊断与当前 Kconfig、目标架构及
生成头文件保持一致。

## 1. 为什么不能使用旧的 `out/arm64`

当前内核通过 `kernel-build/run` 在 Linux 容器中构建，默认输出位于
`.kernel-build/output/<架构>`。旧的 `out/arm64` 可能只包含 macOS 宿主工具
或少量目标，其 `compile_commands.json` 不覆盖 `mm/`、`kernel/`、`fs/`、
`net/` 等主要内核源码。

clangd 找不到当前文件的编译记录时，会从其他文件推断参数。例如，把
`scripts/dtc/checks.c` 的参数用于 `mm/vmalloc.c` 会丢失内核 include 路径、
架构宏和生成配置，继而产生错误诊断和不可靠跳转。

## 2. 为什么要转换 `/src`

构建脚本把宿主机仓库挂载为容器的 `/src`。Kbuild 的 `.cmd` 文件因此记录：

```text
/src/mm/vmalloc.c
-I/src/include
/src/.kernel-build/output/arm64
```

VS Code 和 clangd 运行在宿主机，实际路径则类似：

```text
/Volumes/linux/linux/mm/vmalloc.c
/Volumes/linux/linux/include
/Volumes/linux/linux/.kernel-build/output/arm64
```

宿主机 clangd 无法直接访问容器路径 `/src`。不仅 `file` 字段需要转换，
`directory`、`command` 中的 `-I`、`-include` 等参数也必须一起转换。

`./kernel-build/run compile_commands` 会先在容器中调用内核自带的
`scripts/clang-tools/gen_compile_commands.py`，再把 JSON 中的 `/src`
统一转换为当前宿主机仓库绝对路径。最终数据库写入当前架构的输出目录。

## 3. 逐步配置

所有命令均在内核仓库根目录执行。

### 第一步：准备完整构建输出

首次使用时执行：

```bash
./kernel-build/run setup
./kernel-build/run defconfig
./kernel-build/run build
```

如果当前输出目录已经存在 `.config`、`vmlinux` 以及各子系统的 `.cmd` 文件，
不需要重复完整构建。修改 Kconfig 或切换架构后，应使用相同的
`KERNEL_ARCH`、`KERNEL_CONTAINER_PLATFORM` 和 `KERNEL_OUTPUT_DIR` 重新构建。

### 第二步：生成并转换编译数据库

默认宿主原生架构：

```bash
./kernel-build/run compile_commands
```

交叉构建时必须沿用构建阶段的环境变量，例如：

```bash
KERNEL_ARCH=arm64 ./kernel-build/run compile_commands
```

检查数据库规模以及是否还残留容器路径：

```bash
jq 'length' .kernel-build/output/arm64/compile_commands.json
rg -n '"/src|[ =]/src/' \
  .kernel-build/output/arm64/compile_commands.json
```

第二条命令没有输出才表示路径转换完整。

### 第三步：配置 clangd

仓库根目录 `.clangd`：

```yaml
CompileFlags:
  CompilationDatabase: .kernel-build/output/arm64

Index:
  Background: Build
```

交叉构建或切换架构时，把 `CompilationDatabase` 改为对应宿主输出目录。

`.vscode/settings.json`：

```json
{
    "C_Cpp.intelliSenseEngine": "disabled",
    "clangd.arguments": [
        "--background-index",
        "-j=4",
        "--header-insertion=never"
    ]
}
```

禁用 Microsoft C/C++ IntelliSense 可以避免两个语言服务同时诊断和索引。
`-j=4` 限制 clangd 后台索引并发；内存充足时可以适当调高。

### 第四步：重启并等待后台索引

在 VS Code 命令面板执行：

```text
clangd: Restart language server
```

首次索引完整内核需要一定时间。可以在 VS Code 的 clangd 输出面板观察
进度；后续只会更新发生变化的编译单元。

### 第五步：命令行验收

先确认主要学习文件存在直接编译记录：

```bash
for file in mm/vmalloc.c net/ipv4/tcp.c kernel/fork.c fs/namei.c; do
    jq -e --arg file "$PWD/$file" \
      '.[] | select(.file == $file)' \
      .kernel-build/output/arm64/compile_commands.json >/dev/null &&
      echo "FOUND $file"
done
```

再让 clangd 使用相同数据库实际解析一个文件：

```bash
clangd \
  --check=mm/vmalloc.c \
  --compile-commands-dir=.kernel-build/output/arm64
```

日志中的编译命令应来自 `mm/vmalloc.c` 本身，不应再出现
`Compile command inferred from ... scripts/dtc/...`，也不应出现
`linux/vmalloc.h file not found`。

## 4. 何时重新生成

以下情况应先完成对应增量构建，再重新执行 `compile_commands`：

- 修改 `.config` 或切换 defconfig；
- 切换目标架构或输出目录；
- 新启用一个此前未编译的子系统或驱动；
- 大范围更新源码导致 Kbuild 编译单元变化。

普通函数实现或注释修改不会改变编译命令，通常无需重新生成数据库。
