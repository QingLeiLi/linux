# 固定 Debian 发布镜像版本，并通过 Docker Hub 加速地址拉取。
# 该标签是多架构镜像，由 docker build --platform 选择 arm64/amd64。
FROM docker.1ms.run/library/debian:trixie-20260713-slim

# Debian 12 之后的容器镜像使用 DEB822 格式的 debian.sources。基础镜像
# 默认没有 CA 证书，因此首次安装通过 HTTP 访问；Debian Release/InRelease
# 签名仍会验证仓库元数据的真实性和完整性。
#
# 清华 TUNA 文档指出 APT 与 Nginx 的 HTTP pipelining 可能存在兼容问题。
# 关闭 pipelining，避免下载较大的 LLVM 软件包时偶发 Connection reset。
#
# 软件包覆盖 Kbuild 生成器与压缩工具、clang/LLVM binutils、ELF 解析、
# menuconfig、模块签名、BTF 生成以及常用内核镜像目标。
RUN sed -i \
        -e 's|http://deb.debian.org/debian-security|http://mirrors.tuna.tsinghua.edu.cn/debian-security|g' \
        -e 's|http://deb.debian.org/debian|http://mirrors.tuna.tsinghua.edu.cn/debian|g' \
        /etc/apt/sources.list.d/debian.sources \
    && printf 'Acquire::http::Pipeline-Depth "0";\n' \
        > /etc/apt/apt.conf.d/99nopipelining \
    && apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        bc \
        binutils \
        bison \
        build-essential \
        ca-certificates \
        clang \
        cpio \
        file \
        flex \
        git \
        kmod \
        libelf-dev \
        libncurses-dev \
        libssl-dev \
        lld \
        llvm \
        make \
        openssl \
        pahole \
        perl \
        pkg-config \
        python3 \
        rsync \
        xz-utils \
        zstd \
    && rm -rf /var/lib/apt/lists/*

# kernel-build/run 会把仓库绑定挂载到此目录。
WORKDIR /src
