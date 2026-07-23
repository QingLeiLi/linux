# Linux 内核容器构建环境

## 1. 目标

这套环境用于在 arm64 或 x86_64 的 macOS/Linux 主机上编译 Linux 内核。
脚本会根据宿主 CPU 自动选择原生容器平台和默认内核架构；在 macOS 上还可
避免 SDK、Homebrew 路径和 Linux/glibc 宿主头文件不兼容的问题。

内核构建不仅会编译目标架构代码，还会先为“构建宿主机”生成 `fixdep`、
`sorttable`、`modpost`、`file2alias` 等工具。直接在 macOS 上构建会陆续遇到：

- Homebrew 的 `clang` 与 `ld.lld` 不在同一个前缀目录；
- macOS 不提供 Linux/glibc 的 `elf.h`、`byteswap.h`；
- macOS SDK 的 `uuid_t` 与 `scripts/mod/file2alias.c` 中类型重名；
- BSD 与 GNU 工具、系统头文件的语义差异。

本方案把这些宿主工具也放到 Linux 容器中编译，避免逐个维护兼容补丁。

## 2. 目录结构

```text
kernel-build/
├── Dockerfile       # Debian/LLVM 内核构建镜像定义
├── .dockerignore    # 只允许 Dockerfile 进入镜像构建上下文
├── CLANGD.md        # VS Code/clangd 编译数据库与路径转换
├── run              # 日常构建入口，已包含完整中文注释
└── README.md        # 本文档

.kernel-build/       # 本地构建缓存，继续由 Git 忽略
└── output/
    ├── arm64/               # arm64 容器原生构建 arm64 内核
    ├── x86_64/              # amd64 容器原生构建 x86_64 内核
    └── arm64-amd64/         # 示例：amd64 容器交叉构建 arm64 内核
```

`kernel-build/` 中的环境定义、脚本和文档可以提交到 Git。仓库根目录的
`.gitignore` 仅继续忽略 `.kernel-build/` 本地缓存，因此构建结果不会污染
`git status`，也不会误提交数 GiB 的目标文件。

不要再复用旧的 `out/arm64`。其中可能混有 macOS 编译的宿主工具对象，与
Linux 容器生成的对象不兼容。

## 3. 镜像组成和国内加速

基础镜像固定为：

```text
docker.1ms.run/library/debian:trixie-20260713-slim
```

容器内 APT 使用：

```text
http://mirrors.tuna.tsinghua.edu.cn/debian
http://mirrors.tuna.tsinghua.edu.cn/debian-security
```

首次 APT 事务使用 HTTP，是因为 Debian slim 基础镜像尚未安装 CA 证书。
仓库元数据和软件包仍由 Debian Archive Key 签名验证。安装过程中会补齐
`ca-certificates`。

镜像包含以下主要工具：

- Clang/LLVM 19 和 `ld.lld`；
- GNU make、GCC、binutils、flex、bison、bc；
- `libelf-dev`、`libssl-dev`、`libncurses-dev`；
- pahole 1.30；
- cpio、kmod、rsync、xz、zstd 及常用构建工具。

只需构建镜像一次：

```bash
./kernel-build/run setup
```

日常内核构建不会再次运行 `apt-get`。只有修改 `Dockerfile`、主动执行
`setup`，或本地镜像不存在时，才会重建镜像。

## 4. 首次构建宿主原生架构内核

启动 Docker Engine 或 Docker Desktop 后，在仓库根目录执行：

```bash
./kernel-build/run help
./kernel-build/run setup
./kernel-build/run defconfig
./kernel-build/run build
```

脚本会自动选择与宿主 CPU 一致的默认组合：

| 宿主 CPU | 默认目标 | 输出目录 | 启动镜像 |
|---|---|---|---|
| arm64 | arm64 | `.kernel-build/output/arm64` | `arch/arm64/boot/Image` |
| x86_64 | x86_64 | `.kernel-build/output/x86_64` | `arch/x86/boot/bzImage` |

只构建目标架构的启动镜像：

```bash
# arm64 主机
./kernel-build/run build Image

# x86_64 主机
./kernel-build/run build bzImage
```

构建是增量的。源码修复后重新执行相同命令即可，不需要重新运行
`defconfig`，也不需要删除整个输出目录。

## 5. 常用命令

```bash
# 查看脚本当前解析出的架构、镜像、输出目录和并行度
./kernel-build/run help

# 生成默认配置
./kernel-build/run defconfig

# 更新旧配置
./kernel-build/run olddefconfig

# 交互式配置
./kernel-build/run menuconfig

# 完整并行构建
./kernel-build/run build

# 生成供宿主机 VS Code/clangd 使用的编译数据库
./kernel-build/run compile_commands

# 构建指定目标
./kernel-build/run build Image
./kernel-build/run build modules
./kernel-build/run build dtbs

# 进入与构建命令相同的 Linux 环境
./kernel-build/run shell
```

向 `make` 传递额外参数：

```bash
./kernel-build/run build Image V=1
```

VS Code/clangd 的完整配置、容器 `/src` 路径转换和验收步骤见
[`CLANGD.md`](CLANGD.md)。

## 6. 目录如何挂载

脚本的核心挂载参数是：

```bash
--volume "$repo_root:/src"
--workdir /src
```

含义如下：

```text
宿主机源码目录：<repo_root>（例如 /Volumes/linux/linux 或 /work/linux）
                         │
                         │ Docker bind mount（读写）
                         ▼
容器源码目录：  /src
```

因此：

- 容器直接读取宿主机上的源码，不需要复制仓库；
- 容器写入 `/src/.kernel-build/output/...` 后，文件立即出现在宿主机；
- 容器退出不会丢失编译结果；
- 源码挂载是读写的，容器内命令有能力修改源码，交互式 shell 中应谨慎操作。

容器使用宿主机的 UID/GID：

```bash
--user "$(id -u):$(id -g)"
```

这样生成物仍属于当前宿主用户，不会产生必须用 `sudo` 删除的 root 文件。
`HOME=/tmp` 则避免工具尝试写入容器中不存在的宿主用户主目录。

## 7. 容器架构与内核架构

两个架构变量不要混淆：

| 变量 | 控制对象 | 示例 |
|---|---|---|
| `KERNEL_CONTAINER_PLATFORM` | Docker 用户态和宿主构建工具架构 | `linux/arm64`、`linux/amd64` |
| `KERNEL_ARCH` | Kbuild 的目标内核架构 | `arm64`、`x86_64` |

默认值由 `uname -m` 自动确定：

| 宿主机 `uname -m` | 默认容器平台 | 默认内核目标 | 默认输出目录 |
|---|---|---|---|
| `arm64` / `aarch64` | `linux/arm64` | `ARCH=arm64` | `.kernel-build/output/arm64` |
| `x86_64` / `amd64` | `linux/amd64` | `ARCH=x86_64` | `.kernel-build/output/x86_64` |

这两种默认组合都是原生执行路径。执行以下命令可先确认脚本检测结果，不会
启动构建：

```bash
./kernel-build/run help
```

LLVM 自带多目标后端，因此容器架构和内核目标架构可以不同。不过跨容器架构
还可能引入 Docker 指令模拟开销。通常应让容器平台与宿主硬件一致，只修改
`KERNEL_ARCH` 来交叉构建目标内核。

## 8. x86 平台使用方法

### 8.1 x86_64 Linux 或 Intel Mac：原生构建 x86_64

在 Linux 上安装并启动 Docker Engine，或在 Intel Mac 上启动 Docker
Desktop。

构建环境位于已纳入 Git 的 `kernel-build/`。提交这些文件后，x86 主机通过
正常的 `git clone` 或 `git pull` 即可获得脚本，无需复制
`.kernel-build/output/` 本地缓存。

在 x86 主机进入内核源码根目录，确认执行权限并直接运行：

```bash
chmod +x kernel-build/run
./kernel-build/run help
./kernel-build/run setup
./kernel-build/run defconfig
./kernel-build/run build
```

x86_64 主机不需要设置 `KERNEL_CONTAINER_PLATFORM` 或 `KERNEL_ARCH`。
脚本自动使用：

```text
宿主机架构： x86_64
容器平台：   linux/amd64
内核架构：   ARCH=x86_64
Docker 镜像：linux-kernel-build:trixie-amd64
输出目录：   .kernel-build/output/x86_64
```

只构建 x86 启动镜像：

```bash
./kernel-build/run build bzImage
```

产物通常位于：

```text
.kernel-build/output/x86_64/arch/x86/boot/bzImage
```

`setup` 只需在首次使用、镜像被删除或 `Dockerfile` 修改后执行。已有镜像时，
后续通常只需执行 `defconfig` 和 `build`。

### 8.2 x86_64 主机交叉构建 arm64

保持默认的原生 `linux/amd64` 容器，只覆盖内核目标架构：

```bash
KERNEL_ARCH=arm64 ./kernel-build/run defconfig
KERNEL_ARCH=arm64 ./kernel-build/run build Image
```

脚本会使用独立输出目录，避免与 x86_64 构建对象混用：

```text
.kernel-build/output/arm64-amd64
```

arm64 内核镜像通常位于：

```text
.kernel-build/output/arm64-amd64/arch/arm64/boot/Image
```

每次命令都要带相同的 `KERNEL_ARCH=arm64`。也可以在当前 shell 中临时导出：

```bash
export KERNEL_ARCH=arm64
./kernel-build/run defconfig
./kernel-build/run build Image
unset KERNEL_ARCH
```

### 8.3 Apple Silicon 上使用 amd64 容器构建 x86_64

先创建 amd64 工具链镜像：

```bash
KERNEL_CONTAINER_PLATFORM=linux/amd64 \
KERNEL_ARCH=x86_64 \
./kernel-build/run setup
```

生成 x86_64 配置并构建：

```bash
KERNEL_CONTAINER_PLATFORM=linux/amd64 \
KERNEL_ARCH=x86_64 \
./kernel-build/run defconfig

KERNEL_CONTAINER_PLATFORM=linux/amd64 \
KERNEL_ARCH=x86_64 \
./kernel-build/run build
```

自动使用：

```text
Docker 镜像：linux-kernel-build:trixie-amd64
输出目录：  .kernel-build/output/x86_64
```

在 Apple Silicon 上，`linux/amd64` 容器需要 Docker Desktop 的指令模拟，
速度会显著慢于原生 arm64 容器。

### 8.4 arm64 容器交叉构建 x86_64 内核

如果只需要 x86_64 内核结果、不要求验证 amd64 用户态宿主工具，可保留原生
arm64 容器，仅切换 Kbuild 目标：

```bash
KERNEL_ARCH=x86_64 ./kernel-build/run defconfig
KERNEL_ARCH=x86_64 ./kernel-build/run build
```

默认输出目录为：

```text
.kernel-build/output/x86_64-arm64
```

无论哪种交叉构建方式，`defconfig`、配置调整和 `build` 都必须使用同一组架构
变量及同一个输出目录，不能把不同容器架构生成的宿主工具对象混在一起。

## 9. 可配置变量

| 变量 | 默认值 | 用途 |
|---|---|---|
| `KERNEL_CONTAINER_PLATFORM` | 按宿主 CPU 自动检测 | Docker 镜像/容器架构 |
| `KERNEL_ARCH` | 按宿主 CPU 自动检测 | 传给 Kbuild 的 `ARCH` |
| `KERNEL_BUILD_IMAGE` | 按容器架构自动生成 | 覆盖 Docker 镜像名 |
| `KERNEL_OUTPUT_DIR` | 按架构自动生成 | 覆盖容器内 `O=` 路径 |
| `KERNEL_JOBS` | 宿主 CPU 数，最多自动取 8 | 控制 `make -j` |

自动检测映射为：

```text
arm64 / aarch64 -> KERNEL_CONTAINER_PLATFORM=linux/arm64, KERNEL_ARCH=arm64
x86_64 / amd64 -> KERNEL_CONTAINER_PLATFORM=linux/amd64, KERNEL_ARCH=x86_64
```

降低并行度：

```bash
KERNEL_JOBS=4 ./kernel-build/run build Image
```

自定义输出目录必须位于容器 `/src` 挂载下：

```bash
KERNEL_OUTPUT_DIR=/src/.kernel-build/output/my-arm64 \
./kernel-build/run build Image
```

## 10. 内存和性能注意事项

### `cannot allocate memory` / `ResourceExhausted`

常见原因：

- 同时启动了多个 `docker build` 或内核构建；
- Docker Desktop 分配内存过低；
- 并行任务数过高；
- 在上一次 `setup` 尚未结束时重复执行 `setup`。

处理顺序：

1. 确认镜像是否已经存在：

   ```bash
   docker image inspect linux-kernel-build:trixie-arm64
   ```

2. 镜像存在时不要再次执行 `setup`，直接执行：

   ```bash
   KERNEL_JOBS=4 ./kernel-build/run build
   ```

3. 在 Docker Desktop 设置中增加可用内存。完整 `defconfig` 建议至少预留
   6～8 GiB；启用调试信息、LTO 或更高并行度时需要更多。

4. 查看是否有重复容器：

   ```bash
   docker ps
   ```

脚本自动并行度最多为 8，以降低默认 OOM 风险。

### `No space left on device`

内核 `defconfig` 的完整输出可能达到数 GiB。本次构建在 9.8 GiB 的卷上生成
约 2.6 GiB 输出，并在最终 `OBJCOPY vmlinux` 阶段耗尽磁盘。

建议在开始完整构建前至少预留 6～10 GiB；启用调试信息、LTO、模块或多个
架构输出时需要更多。空间不足时优先移动输出目录或扩容，不要在不确认目标的
情况下删除整个源码树。

### macOS 绑定挂载较慢

Linux 内核源码包含大量小文件，Docker Desktop 的 bind mount 元数据访问
明显慢于原生 Linux 文件系统。这是预期现象，不是构建卡死。

当前方案选择 bind mount，是为了让源码修改和输出文件在 macOS 上立即可见。
若未来追求极致性能，可以把输出目录改为 Docker named volume，但需要额外
导出 `Image`、模块和日志。

## 11. 构建环境与源码错误的边界

环境问题通常表现为：

- 宿主头文件缺失；
- 编译器、链接器或生成工具找不到；
- `modpost`、`sorttable` 等宿主工具无法编译；
- `libelf`、OpenSSL、pahole 缺失。

源码问题通常带有明确的源文件和行号，例如 C 语法错误、类型错误或配置不一致。
容器环境解决的是前一类问题，不会掩盖真实的源码错误。

本次验证中：

- `defconfig` 成功；
- `prepare` 成功；
- `sorttable`、`elf-parse`、`modpost`、`file2alias` 均成功；
- `init/main.c` 注释中的 `/proc/*/ns/` 因包含 `*/` 导致语法错误，已改为
  `/proc/<pid>/ns/`，随后 `init/main.o` 编译成功；
- 完整构建已完成链接、`MODPOST`、kallsyms、`SORTTAB`，最后因磁盘空间
  不足在生成最终 `vmlinux` 时停止。

## 12. 维护和复现

Docker 基础镜像使用带日期的固定标签，避免基础文件系统无意漂移：

```text
debian:trixie-20260713-slim
```

APT 仓库没有固定到快照，因此未来主动重建镜像时，安全更新和软件包小版本
可能变化。若需要字节级可复现，应进一步固定 Debian snapshot 时间和包版本。

修改 `Dockerfile` 后执行：

```bash
./kernel-build/run setup
```

仅修改脚本或 README 不需要重建镜像。

`kernel-build/.dockerignore` 只允许 `Dockerfile` 进入镜像构建上下文。
`.kernel-build/output` 位于该上下文之外，并继续由 Git 忽略。
