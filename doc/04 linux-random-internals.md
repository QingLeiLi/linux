# Linux 内核随机数机制完全解析

> **视角**：从物理熵源到密码学输出，理解 Linux RNG 的每一层——收集什么、如何混合、如何输出、为什么安全。
>
> **代码基础**：`drivers/char/random.c`，`arch/arm64/include/asm/archrandom.h`

---

## 一、整体架构

```
物理世界的不可预测性
  │
  ├── CPU 硬件随机数（TRNG / RNDR / RDRAND）
  ├── 中断时序（网卡/磁盘/键鼠每次中断的纳秒级时间差）
  ├── 磁盘 IO 寻道时序
  ├── 键盘/鼠标输入时序
  ├── 编译时随机字节（LATENT_ENTROPY_PLUGIN）
  ├── 启动时间戳
  └── Bootloader 注入（EFI / 设备树）
        │
        ▼
   input_pool（BLAKE2s 哈希状态，256位）
   ← 所有熵源的汇聚点，不可逆混合
        │  积累到 256 bits 后
        ▼
   extract_entropy()
   → 输出 256 位种子
        │
        ▼
   base_crng（全局 ChaCha20 密钥，256位）
        │ 每 60 秒重播种
        ▼
   per-CPU crng（ChaCha20 密钥副本）
        │ fast key erasure（每次使用后销毁旧密钥）
        ▼
   随机字节输出
   ├── get_random_bytes() / get_random_u32/u64()  内核内部
   ├── /dev/urandom                               用户态（不阻塞）
   ├── /dev/random                                用户态（等待初始化完成）
   └── getrandom(2)                               系统调用（推荐方式）
```

---

## 二、RNG 状态机

整个生命周期由 `crng_init` 枚举控制：

```
CRNG_EMPTY (0)
  状态：刚启动，几乎没有经过认证的熵
  行为：/dev/urandom 输出并打印警告；getrandom(0) 阻塞等待
  转换条件：input_pool.init_bits 达到 128（POOL_EARLY_BITS）

        ↓ _credit_init_bits(bits) 累积到 128

CRNG_EARLY (1)
  状态：已有足够熵用于早期加密操作
  行为：内核内部可使用；用户态 getrandom 仍可能阻塞
  转换条件：init_bits 达到 256（POOL_READY_BITS）

        ↓ _credit_init_bits(bits) 累积到 256

CRNG_READY (2)
  状态：完全初始化
  行为：所有接口正常工作，不再阻塞
  一旦进入此状态，永不退回
  crng_is_ready 静态分支开启 → crng_ready() 变为零开销的 NOP 指令
```

状态转换的实际触发路径：
```c
// _credit_init_bits 内部逻辑
orig = input_pool.init_bits;
new  = min(orig + bits, POOL_READY_BITS);   // 256 封顶
try_cmpxchg(&input_pool.init_bits, &orig, new);

if (new >= POOL_EARLY_BITS && orig < POOL_EARLY_BITS)
    crng_init = CRNG_EARLY;   // 触发第一次 crng_reseed

if (new >= POOL_READY_BITS && orig < POOL_READY_BITS) {
    extract_entropy(base_crng.key, 32);  // 用积累的熵初始化密钥
    crng_init = CRNG_READY;
    queue_work(..., crng_set_ready);     // 开启静态分支
    wake_up_interruptible(&crng_init_wait); // 唤醒所有等待者
}
```

---

## 三、熵收集：各路来源详解

### 3.1 CPU 硬件随机数（质量最高）

ARM64 提供三个层次的硬件随机数接口，按质量从高到低：

**① SMCCC TRNG（最优，需 EL3 固件支持）**

```c
// arch/arm64/include/asm/archrandom.h
if (smccc_trng_available) {
    arm_smccc_1_1_invoke(ARM_SMCCC_TRNG_RND64, max_longs * 64, &res);
    // 通过 SMC 指令陷入 EL3（TrustZone Secure Monitor）
    // EL3 固件从真实硬件 TRNG 读取随机数返回
}
```

物理原理（真随机，TRNG）：
```
热噪声（Thermal Noise）：
  导体中自由电子的布朗运动 → 微小随机电压波动（μV 量级）
  经过放大器放大 → 比较器数字化 → 随机比特流

散粒噪声（Shot Noise）：
  极小电流下单位时间通过的电子数服从泊松分布
  涨落量化后 → 随机比特流

亚稳态采样：
  令两个门控振荡器在频率上极度接近但不同步
  在采样时刻相位差随机 → 输出随机 0/1
  多用于数字工艺友好的 TRNG 实现

特点：
  信息论意义上不可预测（即使有无限算力也无法预测）
  速率较低（通常 < 100Mbps）
  可能偶尔不可用（硬件状态问题），调用者需处理失败情况
```

**② RNDRRS 寄存器（ARMv8.5-RNG，次优）**

```asm
// __arm64_rndrrs
mrs x0, RNDRRS    // SYS_RNDRRS_EL0
// 读取后 DRBG 自动重新从 TRNG 播种
// PSTATE.NZCV: 0b0000 成功，0b0100 失败（硬件忙）
```

**③ RNDR 寄存器（最低级别）**

```asm
// __arm64_rndr
mrs x0, RNDR      // SYS_RNDR_EL0
// 从 DRBG 输出，不重新播种
// 连续读取输出相关性较 RNDRRS 高，但仍满足密码学安全要求
```

x86 对应关系：

| ARM64 | x86 | 质量 |
|-------|-----|------|
| SMCCC TRNG | - | 真随机，最高 |
| RNDRRS | RDSEED | DRBG + 自动重播种 |
| RNDR | RDRAND | DRBG，不重播种 |

`random_init_early` 中的使用策略：

```c
for (i = 0, arch_bits = sizeof(entropy) * 8; i < ARRAY_SIZE(entropy);) {
    // 优先用 seed 级别（TRNG/RNDRRS/RDSEED）
    longs = arch_get_random_seed_longs(entropy + i, ARRAY_SIZE(entropy) - i);
    if (longs) { _mix_pool_bytes(...); i += longs; continue; }

    // 退而用 rand 级别（RNDR/RDRAND）
    longs = arch_get_random_longs(entropy + i, ARRAY_SIZE(entropy) - i);
    if (longs) { _mix_pool_bytes(...); i += longs; continue; }

    // 两者都失败：该槽位不计入熵贡献（不让零填充污染熵池）
    arch_bits -= sizeof(*entropy) * 8;
    ++i;
}
```

---

### 3.2 中断时序熵（运行时最重要的持续来源）

每次硬件中断到来时调用 `add_interrupt_randomness(irq)`：

```c
void add_interrupt_randomness(int irq)
{
    unsigned long entropy = random_get_entropy(); // 读 CPU 周期计数器（CNTPCT_EL0）
    struct fast_pool *fast_pool = this_cpu_ptr(&irq_randomness);
    struct pt_regs *regs = get_irq_regs();

    // 把（时间戳，中断类型，当时的 PC 值）混入 per-CPU fast_pool
    fast_mix(fast_pool->pool,
             entropy,
             (regs ? instruction_pointer(regs) : _RET_IP_) ^ swab(irq));
    new_count = ++fast_pool->count;

    // 积累条件满足才转移到 input_pool：
    // 64 次中断，或距上次超过 1 秒
    if (new_count < 1024 && !time_is_before_jiffies(fast_pool->last + HZ))
        return;

    // 调度 timer 在软中断中批量转移（避免中断上下文直接操作 input_pool）
    fast_pool->count |= MIX_INFLIGHT;
    add_timer_on(&fast_pool->mix, raw_smp_processor_id());
}
```

**fast_pool 的设计原因：**

```
为什么不在每次中断时直接写 input_pool？

input_pool 有自旋锁，且是全局的
中断路径上加锁：
  1. 可能自旋等待（锁被其他 CPU 持有）→ 增加中断延迟
  2. 嵌套中断时死锁风险

fast_pool 是 per-CPU 无锁结构：
  使用 SipHash 快速混合（不需要加密安全，只是临时聚合）
  积累 64 次或等 1 秒后，在软中断上下文批量转移到 input_pool
  → 中断路径零竞争，快速返回
```

**熵计算方式（时序抖动估计）：**

```c
// add_timer_randomness 中的熵估计
delta  = now - last_time;          // 本次与上次的时间差
delta2 = delta - last_delta;       // 一阶差分（速度）
delta3 = delta2 - last_delta2;     // 二阶差分（加速度）

// 取三者绝对值的最小值（最保守估计）
bits = min(fls(min3(|delta|, |delta2|, |delta3|) >> 1), 11)

// 物理含义：
// delta 小且稳定 → 时序可预测 → 熵少
// delta 大且抖动 → 时序不可预测 → 熵多
// 最大计 11 bits（保守，防止高估熵量）
```

---

### 3.3 键盘/鼠标输入时序

```c
void add_input_randomness(unsigned int type, unsigned int code, unsigned int value)
{
    // 忽略自动重复键（连续按键时间间隔太规律，熵很少）
    if (value == last_value)
        return;

    last_value = value;
    // 把（时间戳，按键类型，键码，键值）混入
    add_timer_randomness(&input_timer_state,
                         (type << 4) ^ code ^ (code >> 4) ^ value);
}
```

熵来源：人类按键的时间间隔（毫秒级抖动），不可预测性来自人的行为。
注意：无键盘的服务器这路熵源完全缺失。

---

### 3.4 磁盘 IO 时序

```c
void add_disk_randomness(struct gendisk *disk)
{
    // 混入：IO完成时的时间戳 + 磁盘设备号
    add_timer_randomness(disk->random, 0x100 + disk_devt(disk));
}
```

熵来源：磁盘寻道时间的微小抖动（机械硬盘约 ±1ms，SSD 约 ±10μs）。  
注意：**SSD 因寻道时间极短且一致，熵量远低于 HDD**，代码注释也明确提到这一点。

---

### 3.5 硬件 RNG 驱动（hwrng 子系统）

```c
void add_hwgenerator_randomness(const void *buf, size_t len,
                                size_t entropy, bool sleep_after)
{
    mix_pool_bytes(buf, len);
    credit_init_bits(entropy);

    // 熵池满时限流：等待一个重播种间隔再继续
    // 防止 hwrng 驱动以极高速率填充熵池，浪费带宽
    if (sleep_after)
        schedule_timeout_interruptible(crng_reseed_interval());
}
```

使用此接口的驱动：`/drivers/char/hw_random/` 下的各厂商 TRNG 驱动（如 `bcm2835-rng.c`、`omap-rng.c`）。

---

### 3.6 Bootloader 注入

```c
void __init add_bootloader_randomness(const void *buf, size_t len)
{
    mix_pool_bytes(buf, len);
    if (trust_bootloader)      // 默认 true，可通过 random.trust_bootloader=0 禁用
        credit_init_bits(len * 8);
}
```

EFI 固件和设备树（`/chosen/rng-seed`）都通过此接口注入随机数。
Bootloader 生成的随机数通常质量很高（依赖固件自己的 TRNG），是嵌入式设备早期熵的重要来源。

---

### 3.7 编译时潜在熵（LATENT_ENTROPY_PLUGIN）

```c
// GCC 插件在编译时向此数组填入随机字节
static const u8 compiletime_seed[BLAKE2S_BLOCK_SIZE]
    __initconst __latent_entropy;
_mix_pool_bytes(compiletime_seed, sizeof(compiletime_seed));

// 另有运行时版本：latent_entropy 全局变量
// 内核执行期间，经过特定代码路径时被修改（插件注入 XOR 操作）
// add_latent_entropy() 在 random_init() 中混入
```

作用：让每次编译出的内核镜像初始熵池状态不同，防止攻击者用已知内核镜像推算熵池初始状态。

---

### 3.8 虚拟机 Fork 处理

```c
void add_vmfork_randomness(const void *unique_vm_id, size_t len)
{
    // 注入 VM 唯一 ID（由 hypervisor 提供，克隆时不同）
    // 不计入熵量（unique_id 不一定是随机的，只是"唯一"）
    add_device_randomness(unique_vm_id, len);

    // 立刻强制重播种 crng
    if (crng_ready()) {
        crng_reseed(NULL);
        pr_notice("crng reseeded due to virtual machine fork\n");
    }
}
```

VM clone/snapshot 的安全问题：

```
问题：
  VM A 运行中 → 快照 → 克隆为 VM B、VM C
  三个 VM 的 crng 状态完全相同
  → 生成相同的 TLS 密钥、TCP ISN、随机数
  → 灾难性安全漏洞

检测机制：
  vmgenid（ACPI 设备）：hypervisor 在 VM clone/resume 时
  修改一块 ACPI 共享内存中的 128 位 UUID

  内核驱动（drivers/virt/vmgenid.c）：
    注册 ACPI 通知
    UUID 变化时调用 add_vmfork_randomness(new_uuid, 16)
    → crng 立刻重播种 → 克隆 VM 获得不同的随机数状态
```

---

### 3.9 PM 休眠恢复

```c
static int random_pm_notification(struct notifier_block *nb,
                                   unsigned long action, void *data)
{
    ktime_t stamps[] = {
        ktime_get(),          // MONOTONIC
        ktime_get_boottime(), // BOOTTIME（含 suspend 时长）
        ktime_get_real()      // REALTIME（UTC 时钟）
    };
    // 三个时间戳的差值隐含了休眠时长，每次 suspend 不同
    _mix_pool_bytes(stamps, sizeof(stamps));
    _mix_pool_bytes(&entropy, sizeof(entropy));

    // resume 后重播种（刷新密钥）
    if (crng_ready() && action == PM_POST_SUSPEND)
        crng_reseed(NULL);
}
```

防止 resume 后 crng 状态与 suspend 前相同（时间流逝本身是额外熵）。

---

## 四、熵池：BLAKE2s 哈希状态

### 4.1 数据结构

```c
static struct {
    struct blake2s_ctx hash;    // BLAKE2s 哈希上下文（内部状态 256 位）
    spinlock_t lock;
    unsigned int init_bits;     // 已累积的熵估计量（位数，上限 256）
} input_pool = {
    .hash.h = { BLAKE2S_IV0 ^ (0x01010000 | BLAKE2S_HASH_SIZE),
                BLAKE2S_IV1, ... BLAKE2S_IV7 },  // 带参数化的初始向量
    .hash.outlen = BLAKE2S_HASH_SIZE,            // 32 字节输出
};
```

### 4.2 混入操作

```c
// 所有熵源最终调用此函数
static void _mix_pool_bytes(const void *buf, size_t len)
{
    blake2s_update(&input_pool.hash, buf, len);
    // BLAKE2s 内部做：
    //   将输入分块（64 字节/块）
    //   每块与内部状态做 ChaCha 风格的混合（12 轮 G 函数）
    //   结果不可逆地改变内部状态
}
```

混入是**单向的**：知道混入后的状态，无法推算之前混入的数据。这是 BLAKE2s 单向性的直接应用。

### 4.3 提取操作（HKDF 风格）

```c
static void extract_entropy(void *buf, size_t len)
{
    u8 seed[32], next_key[32];
    struct { unsigned long rdseed[4]; size_t counter; } block;

    // 1. 补充混入额外的硬件随机数（每次提取时都尝试读 RNDR/RDRAND）
    for (i = 0; i < ARRAY_SIZE(block.rdseed); )
        // 尝试 arch_get_random_seed_longs / arch_get_random_longs

    // 2. 终结当前哈希状态，得到 256 位种子
    blake2s_final(&input_pool.hash, seed);       // seed = H(all_inputs)

    // 3. 生成新的熵池密钥（前向保密：旧状态不可从新状态恢复）
    block.counter = 0;
    blake2s(seed, ..., &block, ..., next_key);   // next_key = H(seed || RDSEED || 0)
    blake2s_init_key(&input_pool.hash, 32, next_key, 32);
                                                  // 用新密钥重新初始化熵池
    memzero_explicit(next_key, 32);              // 安全清除中间值

    // 4. 用 seed 展开输出所需长度的随机字节
    while (len) {
        ++block.counter;
        blake2s(seed, ..., &block, ..., buf, i); // output[i] = H(seed || RDSEED || counter)
        len -= i; buf += i;
    }
    memzero_explicit(seed, 32);                  // 安全清除种子
}
```

**前向保密（Forward Secrecy）：**
- 提取后 `input_pool.hash` 用 `next_key` 重新初始化
- 即使攻击者在提取后获得了 `input_pool` 状态，也无法重现此次提取的输出
- 因为 `seed` 和 `next_key` 均已安全清零（`memzero_explicit` 防编译器优化掉清零）

---

## 五、输出层：ChaCha20 流密码（crng）

### 5.1 数据结构

```c
// 全局基础 crng（所有 per-CPU crng 的根）
static struct {
    u8  key[CHACHA_KEY_SIZE];   // 32 字节 ChaCha20 密钥
    unsigned long generation;   // 版本号，per-CPU crng 同步用
    spinlock_t lock;
} base_crng;

// per-CPU crng（实际输出随机数用这个，避免锁竞争）
struct crng {
    u8  key[CHACHA_KEY_SIZE];
    unsigned long generation;   // 与 base_crng.generation 比较判断是否需要同步
    local_lock_t lock;          // per-CPU 本地锁，无需自旋
};
static DEFINE_PER_CPU(struct crng, crngs);
```

### 5.2 fast key erasure 机制

```c
static void crng_fast_key_erasure(u8 key[CHACHA_KEY_SIZE],
                                   struct chacha_state *chacha_state,
                                   u8 *random_data, size_t random_data_len)
{
    // ChaCha20 初始状态：4个常数字 + 8个密钥字 + 计数器字 + nonce字
    chacha_init_consts(chacha_state);           // 填入 "expand 32-byte k"
    memcpy(&chacha_state->x[4], key, 32);       // 填入当前密钥
    memset(&chacha_state->x[12], 0, 16);        // 计数器和 nonce 清零

    // 运行 ChaCha20，生成第一个 64 字节输出块
    chacha20_block(chacha_state, first_block);

    // ★ 关键：立刻用输出的前 32 字节替换旧密钥（销毁旧密钥）
    memcpy(key, first_block, CHACHA_KEY_SIZE);
    // 旧密钥的内存位置现在包含新密钥，不需要额外清零
    // (覆盖即是清除)

    // 输出的后 32 字节（或更多块）作为实际随机数
    memcpy(random_data, first_block + CHACHA_KEY_SIZE,
           min(random_data_len, 32u));
    // 如果需要更多，继续运行 ChaCha20（计数器递增）
}
```

时间线图：

```
t=0: key=K0  → ChaCha20 → [K1 | R0]
                            ↓      ↓
                      新密钥K1  随机数R0（输出给调用者）
                      覆盖K0

t=1: key=K1  → ChaCha20 → [K2 | R1]
t=2: key=K2  → ChaCha20 → [K3 | R2]

攻击者在 t=2 获得了 K2：
  ✓ 可以计算 R2 及之后的输出（后向不安全，这是可接受的）
  ✗ 无法从 K2 推算 K1、K0、R0、R1（前向保密）
    因为 K0→K1 是 ChaCha20 的单向变换，无法逆向
```

### 5.3 per-CPU crng 的同步机制

```c
static void crng_make_state(struct chacha_state *chacha_state,
                             u8 *random_data, size_t random_data_len)
{
    // 获取 per-CPU crng
    crng = raw_cpu_ptr(&crngs);
    local_lock_irqsave(&crngs.lock, flags);

    // 检查是否需要从 base_crng 更新
    if (unlikely(crng->generation != READ_ONCE(base_crng.generation))) {
        // base_crng 已重播种（generation 变了），需要同步
        spin_lock(&base_crng.lock);
        // 用 base_crng 的密钥做 fast key erasure，输出作为 per-CPU 新密钥
        crng_fast_key_erasure(base_crng.key, chacha_state,
                              crng->key, sizeof(crng->key));
        crng->generation = base_crng.generation;
        spin_unlock(&base_crng.lock);
    }

    // 用 per-CPU 密钥生成随机数
    crng_fast_key_erasure(crng->key, chacha_state, random_data, random_data_len);
    local_unlock_irqrestore(&crngs.lock, flags);
}
```

### 5.4 定期重播种

```c
static void crng_reseed(struct work_struct *work)
{
    // 调度下一次重播种（60 秒后，或 crng 刚就绪后 1 秒）
    if (work)
        queue_delayed_work(system_dfl_wq, &next_reseed,
                           crng_reseed_interval());

    // 从 input_pool 提取新的 256 位密钥
    extract_entropy(key, sizeof(key));

    spin_lock_irqsave(&base_crng.lock, flags);
    memcpy(base_crng.key, key, sizeof(key));
    // generation++ → 所有 per-CPU crng 下次使用时检测到版本差异
    //                → 自动拉取新密钥（懒更新）
    WRITE_ONCE(base_crng.generation, base_crng.generation + 1);
    spin_unlock_irqrestore(&base_crng.lock, flags);

    memzero_explicit(key, sizeof(key));  // 安全清零临时密钥
}
```

重播种时间间隔：
- 第一次（`CRNG_EARLY → CRNG_READY` 后）：1 秒后
- 之后：60 秒一次（`CRNG_RESEED_INTERVAL`）

---

## 六、ChaCha20 的密码学安全性

### 6.1 什么是"足够安全"

安全的伪随机数生成器的正式定义是**计算不可区分性**：

```
对于任何运行时间多项式有界的攻击者：
  区分 ChaCha20 输出 和 真随机序列
  的优势不超过可忽略的函数

含义：
  找出 ChaCha20 输出的"规律"
  等价于解决一个计算困难问题
  最优已知算法需要 2^256 次操作
```

与真随机的本质区别：

```
真随机（TRNG）：         信息论安全
  即使有无限算力也无法预测
  每个比特来自独立的物理过程

ChaCha20（CSPRNG）：     计算安全
  知道密钥 → 可完全重现所有输出
  不知道密钥 → 需要 2^256 次穷举
  实践中：计算安全 = 无法攻破
```

### 6.2 ChaCha20 的内部结构与雪崩效应

初始状态（512 位，4×4 矩阵，每格 32 位）：

```
[ "expa" ][ "nd 3" ][ "2-by" ][ "te k" ]  ← 固定常数
[ key[0] ][ key[1] ][ key[2] ][ key[3] ]  ← 256位密钥（前半）
[ key[4] ][ key[5] ][ key[6] ][ key[7] ]  ← 256位密钥（后半）
[ count  ][ nonce0 ][ nonce1 ][ nonce2 ]  ← 64位计数器 + 96位nonce
```

Quarter Round 操作（ARX：Add-Rotate-XOR）：

```c
#define QR(a, b, c, d)        \
    a += b; d ^= a; d = ROL(d, 16); \
    c += d; b ^= c; b = ROL(b, 12); \
    a += b; d ^= a; d = ROL(d,  8); \
    c += d; b ^= c; b = ROL(b,  7);

// 20轮，交替列轮和对角轮
for (i = 0; i < 10; i++) {
    QR(s[0],s[4],s[ 8],s[12]); // 列
    QR(s[1],s[5],s[ 9],s[13]);
    QR(s[2],s[6],s[10],s[14]);
    QR(s[3],s[7],s[11],s[15]);
    QR(s[0],s[5],s[10],s[15]); // 对角
    QR(s[1],s[6],s[11],s[12]);
    QR(s[2],s[7],s[ 8],s[13]);
    QR(s[3],s[4],s[ 9],s[14]);
}
// 结果与初始状态相加 → 输出 512 位密钥流
```

雪崩效应（改变密钥的 1 个 bit）：

```
轮数   影响的 bit 数（共 512 bit）
  1         ~32 bit（局部扩散）
  4         ~128 bit
  8         ~400 bit
 20         ~256 bit（完全随机化，统计上与真随机无法区分）

为什么用 20 轮：
  ChaCha8：有已知区分攻击（不可实用，但存在理论弱点）
  ChaCha12：安全余量充足
  ChaCha20：额外 8 轮安全边际，应对未来可能的密码分析进展
```

### 6.3 旧设计的错误：熵计数是伪概念

```
旧 /dev/random（Linux < 5.6）的错误假设：
  每输出一个随机位 → 消耗一个"熵位"
  熵耗尽 → 阻塞

这个假设在密码学上是错误的：

  正确理解：
    物理熵的作用 = 提供一个不可预测的 256 位初始密钥
    之后的任务 = 用 ChaCha20 无限扩展这个密钥

    读取 1MB 随机数后：
      攻击者得到了 1MB 输出
      但仍需 2^256 次操作才能从输出推算密钥
      ChaCha20 是单向的（不可逆）

    额外读取并没有降低密钥的安全性
    阻塞没有任何密码学意义

类比：
  AES-256 加密了 1GB 数据
  攻击者拿到所有密文
  恢复密钥仍需 2^256 次穷举
  "加密越多越不安全"是错误直觉

真正的唯一安全门槛：
  初始种子是否积累了 256 位不可预测性
  一旦达到 → 可以无限输出
```

### 6.4 256 位为什么物理上无法暴力破解

```
即使对量子计算机（Grover 算法将搜索空间从 2^256 降到 2^128）：

2^128 次量子操作的物理下界（Landauer 原理）：
  每次不可逆操作消耗最小能量 = kT × ln(2)
  T = 3K（接近绝对零度，物理极限）
  每次操作 ≈ 3×10^-23 焦耳

  2^128 次操作需要的能量：
  ≈ 2^128 × 3×10^-23 ≈ 10^16 焦耳
  ≈ 全球电网运行约 3 年的总发电量

结论：
  即使用全人类的能源，对量子计算机破解 256 位密钥
  在物理层面就不可能完成
  不是"很难"，是热力学不允许
```

---

## 七、用户态接口

### 7.1 三种接口的语义区分

```c
// 现代推荐：getrandom(2) 系统调用
SYSCALL_DEFINE3(getrandom, char __user *, ubuf, size_t, len, unsigned int, flags)
{
    // 合法 flags 组合：
    // 0                    ：等待 CRNG_READY 后返回（推荐）
    // GRND_NONBLOCK        ：未就绪时返回 -EAGAIN（非阻塞）
    // GRND_RANDOM          ：同 flags=0（历史遗留，现与 0 等价）
    // GRND_INSECURE        ：立即返回，即使 CRNG_EMPTY（不推荐）
    // GRND_INSECURE|GRND_RANDOM：非法，返回 -EINVAL

    if (!crng_ready() && !(flags & GRND_INSECURE)) {
        if (flags & GRND_NONBLOCK)
            return -EAGAIN;
        wait_for_random_bytes();   // 阻塞直到 CRNG_READY
    }
    return get_random_bytes_user(&iter);
}
```

三种接口对比：

| 接口 | 阻塞行为 | 推荐程度 | 等价 flags |
|------|---------|---------|-----------|
| `getrandom(buf, len, 0)` | 等待 CRNG_READY | ★★★ 推荐 | - |
| `/dev/random` read | 等待 CRNG_READY | ★★ 可用（现代内核与 urandom 相同） | `GRND_RANDOM` |
| `/dev/urandom` read | 不阻塞，可能在 CRNG_EMPTY 时输出 | ★ 不推荐 | `GRND_INSECURE` |

`/dev/random` 历史变化：
```
Linux < 5.6：
  熵估计量 < 阈值时阻塞，导致很多程序卡住
  "cat /dev/random" 会卡住直到有足够硬件中断
  HAVEGED / rng-tools 等工具就是为了解决这个问题

Linux >= 5.6：
  /dev/random 与 /dev/urandom 行为完全相同
  不再基于熵计数阻塞
  只在 CRNG_READY 之前阻塞（一次性，启动后几秒内完成）
  原因：ChaCha20 CRNG 足够安全，熵耗尽的阻塞没有密码学意义
```

### 7.2 向熵池写入

```
写入 /dev/random 或 /dev/urandom：
  mix_pool_bytes(user_data, len)
  → 混入 input_pool（不计入 init_bits！）
  
用途：
  rng-tools 的 rngd 守护进程将硬件 RNG 数据写入 /dev/random
  用户程序可以手动向熵池注入额外的随机数据

  写入只混合，不"提升"熵计数
  防止攻击者通过写入已知数据来"毒化"熵池
```

### 7.3 ioctl 接口

```c
switch (cmd) {
case RNDGETENTCNT:
    // 获取当前熵估计量（已累积的 bits 数，上限 256）
    put_user(input_pool.init_bits, p);
    break;

case RNDADDENTROPY:
    // 向熵池添加数据，并手动声明熵量
    // 需要 CAP_SYS_ADMIN 权限
    // struct rand_pool_info { int entropy_count; int buf_size; __u32 buf[0]; }
    copy_from_user(&rpi, p, sizeof(rpi));
    mix_pool_bytes(rpi.buf, rpi.buf_size);
    credit_init_bits(rpi.entropy_count);
    break;

case RNDZAPENTCNT:
case RNDCLEARPOOL:
    // 清零熵计数（测试用，需要 CAP_SYS_ADMIN）
    // 不清除实际的哈希状态，只是让 crng_ready() 返回 false
    break;

case RNDRESEEDCRNG:
    // 强制立即重播种 crng（需要 CAP_SYS_ADMIN）
    crng_reseed(NULL);
    break;
}
```

### 7.4 内核内部 API

```c
// 通用随机字节（等同于 /dev/urandom）
void get_random_bytes(void *buf, size_t len);

// 单个随机整数（有 per-CPU 批量缓存，比 get_random_bytes 更快）
u8  get_random_u8(void);
u16 get_random_u16(void);
u32 get_random_u32(void);
u64 get_random_u64(void);

// 无偏范围随机数（重要：避免模偏差）
// 普通 rand() % N 当 N 不是 2 的幂时有偏差
// get_random_u32_below 使用拒绝采样保证无偏
u32 get_random_u32_below(u32 ceil);
u32 get_random_u32_above(u32 floor);
u32 get_random_u32_inclusive(u32 floor, u32 ceil);

// 等待 crng 就绪（阻塞，可在驱动 probe 中使用）
int wait_for_random_bytes(void);

// 注册 crng 就绪回调（非阻塞）
int execute_with_initialized_rng(struct notifier_block *nb);
```

---

## 八、使用场景

### 8.1 内核安全机制

```
TCP 初始序列号（ISN）：
  get_random_u32()
  防止 TCP 序列号预测攻击（session hijacking）
  RFC 6528 建议使用 CSPRNG 生成 ISN

地址空间随机化（ASLR）：
  get_random_long()
  内核基址（KASLR）：head.S 极早期，random_init_early 后
  用户进程栈基址：exec 时
  mmap 基址：每次 mmap
  防止 ROP（Return-Oriented Programming）等漏洞利用

栈 Canary（Stack Smashing Protection）：
  fork() 时为每个进程生成随机 canary 值
  写入栈底（低地址）
  函数返回前检查 canary 是否被覆盖
  → 缓冲区溢出攻击检测

哈希表种子（HashDoS 防御）：
  内核各种哈希表初始化时注入随机种子
  防止攻击者构造大量哈希冲突导致 DoS（HashDoS 攻击）
  net_secret（网络相关哈希表）
  inet_peer 哈希表
  ipc_ida 等

文件系统加密（fscrypt）：
  文件加密密钥的密钥派生材料
  /proc/sys/kernel/random/uuid（文件系统 UUID 生成）
```

### 8.2 用户态安全场景

```
TLS/SSH 会话密钥：
  openssl、libsodium 等通过 getrandom(2) 获取密钥材料
  每个 TLS 会话的对称密钥都是新鲜随机生成的

密码哈希的盐（Salt）：
  bcrypt/argon2 等密码哈希算法的盐值
  防止彩虹表攻击

UUID 生成（UUID v4）：
  122 位随机数 + 6 位版本/变体标记
  碰撞概率极低（生成 2^61 个 UUID 才有 50% 碰撞概率）

会话 token / CSRF token：
  Web 框架生成的会话 ID 和防 CSRF token
  必须不可预测，防止会话固定攻击

密钥协商（ECDH/DH）：
  椭圆曲线 Diffie-Hellman 的私钥
  每次密钥协商需要新鲜的随机数
```

### 8.3 不适合使用的场景

```
需要可重现的随机数：
  测试、模拟、游戏地图生成
  → 使用用种子初始化的确定性 PRNG（如 mt19937）
  → getrandom 每次运行结果不同，无法复现

高频简单随机数（不涉及安全）：
  物理模拟中的噪声
  随机算法的随机化（如快速排序的随机pivot）
  → 使用 PRNG（PCG、xoshiro256 等），速度快 100 倍以上
  → getrandom 有 syscall 开销，且杀鸡用牛刀

掷骰子类游戏（不涉及金钱）：
  普通 PRNG 足够
  → 只有涉及金钱的赌博/彩票才必须用 CSPRNG
```

---

## 九、早期启动的熵不足问题

### 9.1 问题场景

```
嵌入式 Linux 的典型困境：

  系统特征：
    无硬件 TRNG（CPU 不支持 RNDR/RDRAND）
    SSD 存储（磁盘寻道时序几乎无熵）
    无键盘鼠标
    每次启动时间、网络接口 MAC 地址相同（批量生产的设备）

  结果：
    启动后很长时间内 crng 处于 CRNG_EMPTY 或 CRNG_EARLY 状态
    getrandom(0) 阻塞，进程卡住
    SSH 守护进程等待随机数 → 系统无法登录

  历史案例（2012 年论文）：
    研究人员扫描 730 万个公开 RSA 密钥
    发现约 27,000 对密钥共享一个质因数
    → 意味着这些密钥的 RNG 初始状态相同
    → 可以直接计算出私钥
    根本原因：嵌入式设备在熵不足时生成了密钥
```

### 9.2 解决方案

```
1. Bootloader 注入（最有效）：
   EFI：EFI_RNG_PROTOCOL → add_bootloader_randomness()
   U-Boot：在 /chosen/rng-seed 写入随机数
           → 内核 early_init_dt_scan_chosen() 读取
           → add_bootloader_randomness()
   随机数来源：固件自己的 TRNG，质量通常很高

2. 种子文件（systemd-random-seed）：
   关机前：dd if=/dev/urandom of=/var/lib/systemd/random-seed bs=512 count=1
   开机时：cat /var/lib/systemd/random-seed > /dev/urandom
   → 把上次关机时的 crng 状态传递给本次启动
   问题：第一次开机无种子文件；flash 损坏丢失种子

3. TPM（可信平台模块）：
   TPM 自带 TRNG 和 NVRAM
   每次启动从 TPM 读取随机数 + 写入新种子
   → 即使无网络也有高质量熵

4. jitterentropy 内核模块（CONFIG_RANDOM_JITTER）：
   通过精确测量 CPU 执行时间的抖动（Cache miss 时序等）
   作为熵源，不依赖任何外部硬件
   缺点：速度慢（约 1KB/s），但对嵌入式设备足够

5. HAVEGE / haveged 用户态守护进程：
   类似 jitterentropy，但在用户态实现
   测量复杂代码路径的执行时间抖动
   向 /dev/random 写入数据提升熵计数
```

---

## 十、全局数据流总结

```
                              启动阶段
                                │
     编译时随机字节 ─────────────┤
     CPU TRNG/RNDRRS ───────────┤
     CPU RNDR ──────────────────┤─→ random_init_early()
     utsname + cmdline ──────────┤                │
                                 │                ▼
     时钟计数器 CNTPCT ───────────┤        input_pool
     墙钟时间 ktime_get ──────────┤─→ random_init()   (BLAKE2s 哈希状态)
     latent_entropy ─────────────┤                │
                                 │         init_bits 累积
                                 │                │
                         运行时持续注入            ▼ 达到 256 bits
     中断时序 ───────────────────→ fast_pool  extract_entropy()
     键鼠输入 ───────────────────→ (per-CPU,      │
     磁盘IO ─────────────────────→  SipHash)      ▼
     hwrng 驱动 ─────────────────→ input_pool  256位种子
     bootloader ─────────────────→           ┌────┘
     VM fork ─────────────────────→           ▼
     PM resume ───────────────────→      base_crng.key
                                           (ChaCha20)
                                                │ 每60秒重播种
                                                ▼
                                         per-CPU crng
                                           fast key erasure
                                                │
                              ┌─────────────────┼─────────────────┐
                              ▼                 ▼                 ▼
                    get_random_u32()     /dev/urandom        getrandom(2)
                    内核哈希表种子        用户态加密库         SSH/TLS 密钥
                    TCP ISN / ASLR       UUID 生成            密码盐值
```