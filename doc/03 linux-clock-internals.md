# Linux 时钟机制完全解析

> **视角**：从硬件电路原理出发，逐层向上到 Linux 内核抽象，理解"时钟"在不同语境下的真实含义和实现机制。

---

## 一、消除歧义：Linux 里"时钟"的三种含义

Linux 内核文档、驱动代码、内核参数里"clock"这个词指三件完全不同的事，必须先分开：

```
时钟（clock）在 Linux 里的三种含义：

┌─────────────────────┬──────────────────────────────┬──────────────────────┐
│ 含义                │ 硬件基础                      │ Linux 子系统         │
├─────────────────────┼──────────────────────────────┼──────────────────────┤
│ 1. 外设时钟信号      │ 晶振→PLL→Divider→Gate→外设   │ CCF（Common Clock    │
│    "给 UART 提供     │ 真实的电信号，MHz 量级         │  Framework）         │
│     工作频率"        │                              │                      │
├─────────────────────┼──────────────────────────────┼──────────────────────┤
│ 2. 计时时钟          │ 硬件单调递增计数器            │ clocksource          │
│    "现在是几点？     │ ARM64: CNTPCT_EL0            │ timekeeping          │
│     过了多少纳秒？"  │                              │                      │
├─────────────────────┼──────────────────────────────┼──────────────────────┤
│ 3. 定时器时钟        │ 可编程比较器，计数到目标值    │ clock_event_device   │
│    "N 毫秒后叫醒我"  │ 触发中断                     │ hrtimer / jiffies    │
│                     │ ARM64: CNTP_TVAL_EL0         │                      │
└─────────────────────┴──────────────────────────────┴──────────────────────┘

本文按此顺序展开，第一种（硬件时钟树）篇幅最大，因为它是另外两种的物理基础。
```

---

## 二、硬件时钟树

### 2.1 为什么需要时钟树

SoC 芯片内部几十个模块各自需要不同的工作频率，但芯片外部只有一颗晶振：

```
需求：
  CPU 核心         1.8GHz
  内存控制器       800MHz
  USB 控制器       480MHz（USB 协议硬性要求，必须精确）
  UART             48MHz（波特率计算基准）
  I2C              400KHz
  SD 卡控制器      200MHz

供给：
  外部晶振（XTAL） 24MHz 或 25MHz（石英晶体，精度极高但只有一个）

问题：一个 24MHz 信号如何变成上面所有频率？
答：时钟树——由 PLL/Divider/Mux/Gate 组成的信号变换网络
```

### 2.2 四种基础硬件模块

#### PLL（Phase-Locked Loop，锁相环）— 倍频

```
输入：24MHz 参考时钟
输出：数百 MHz 到数 GHz 的高频时钟

核心公式：F_out = F_ref × N / M
  N = 反馈分频系数（整数，软件可写寄存器）
  M = 前分频系数（整数，软件可写寄存器）

示例：
  N=50, M=1 → F_out = 24 × 50 = 1200MHz
  N=40, M=1 → F_out = 24 × 40 = 960MHz
  N=20, M=1 → F_out = 24 × 20 = 480MHz（USB 需要的精确频率）
```

#### Divider（分频器）— 整数降频

```
输入：高频时钟（如 1200MHz）
输出：整除后的低频时钟

1200MHz ÷1  = 1200MHz
1200MHz ÷2  = 600MHz
1200MHz ÷4  = 300MHz
1200MHz ÷6  = 200MHz
1200MHz ÷8  = 150MHz
1200MHz ÷12 = 100MHz
1200MHz ÷25 = 48MHz   ← UART 时钟

限制：只能整除，241~299MHz 这样的值取不到（离散性的根源）
```

#### Mux（多路选择器）— 切换时钟源

```
多个时钟源输入 → 软件控制选择哪一路输出

典型用途：
  输入A：PLL 高频输出（高性能）
  输入B：晶振直出 24MHz（低功耗）
  
  CPU 高负载时 → 选 A
  CPU 待机时   → 选 B（不需要 PLL，节省 PLL 自身功耗）

Glitch-Free Mux（无毛刺切换）：
  普通 MUX 切换时可能产生极短的异常脉冲（毛刺）
  → 下游时序电路误触发
  Glitch-Free MUX 保证：
    切换只在两路信号都处于低电平时发生
    → 切换过程中不产生任何额外时钟沿
    → 对下游完全透明
```

#### Gate（时钟门控）— 开关时钟

```
输入：时钟信号
控制：寄存器中的一个 bit

bit=1 → 输出时钟信号（外设正常工作）
bit=0 → 输出恒定低电平（外设停止工作，节省动态功耗）

功耗意义：
  P_动态 ∝ C × V² × F（动态功耗与频率成正比）
  Gate 关闭 → F=0 → 该模块动态功耗为零
  这是最细粒度的节电手段
```

### 2.3 时钟树拓扑示例

```
XTAL 24MHz（树根，外部晶振）
  │
  ├── PLL_CPU（N=75） → 1800MHz
  │     │
  │     ├── [Glitch-Free MUX] ←── 也可切换回 XTAL（待机用）
  │     │         │
  │     │         └── CPU Gate ──→ CPU Core（可独立开关）
  │     │
  │     └── ÷3 → 600MHz
  │           │
  │           └── [MUX] ←── 也可选其他源
  │                 │
  │                 └── AHB Bus Gate → 系统总线
  │                       │
  │                       ├── ÷2 → 300MHz → APB1 Bus
  │                       │     ├── UART Gate  → UART0/1/2
  │                       │     ├── I2C Gate   → I2C0/1
  │                       │     └── SPI Gate   → SPI0
  │                       │
  │                       └── ÷4 → 150MHz → APB2 Bus
  │                             └── Timer Gate → 定时器
  │
  ├── PLL_USB（N=20） → 480MHz（USB 协议精确要求）
  │     └── USB Gate → USB 控制器
  │
  ├── PLL_DDR（N=33） → 800MHz（×2 = LPDDR4 1600MT/s）
  │     └── DDR Gate → 内存控制器
  │
  └── PLL_AUDIO（分数N） → 22.5792MHz（44.1kHz × 512）
        └── I2S Gate → 音频接口

树上每个节点 = 一个硬件寄存器可控的电路模块
节点数量在芯片流片时固定，软件无法增减
```

---

## 三、PLL 内部硬件原理

PLL 是时钟树中最复杂的模块，理解它需要从每个子电路开始。

### 3.1 整体结构

```
                    ┌──────────────────────────────────────────────┐
                    │                  PLL 内部                     │
                    │                                              │
F_ref（晶振）        │  ┌───┐    ┌─────┐    ┌────┐    ┌─────┐      │
24MHz ──────────────┼─→│ ÷M │──→│ PFD │──→│ CP │──→│ LPF │      │
                    │  └───┘    └──┬──┘    └────┘    └──┬──┘      │
                    │  前分频       │  UP/DOWN 脉冲        │         │
                    │  寄存器       │  （误差信号）         │ 控制电压 │
                    │              └────────────          │         │
                    │                           ↓         │         │
                    │                        ┌─────┐      │         │
                    │              F_out ←───│ VCO │←─────┘         │
                    │                │       └─────┘                 │
                    │                │       压控振荡器               │
                    │                │                               │
                    │             ┌──┴──┐                            │
                    │             │ ÷N  │  反馈分频器                 │
                    │             └──┬──┘  寄存器（改这里改频率）      │
                    │                │                               │
                    │  PFD 第二输入 ←─┘  FB 信号                     │
                    └──────────────────────────────────────────────┘

锁定时：F_out/N = F_ref/M  →  F_out = F_ref × N/M
改 N 值寄存器 → 打破平衡 → 反馈回路自动调整 VCO → 锁定到新频率
```

### 3.2 VCO（压控振荡器）— 频率生成核心

VCO 是整个 PLL 里唯一真正产生振荡的模块，其余模块都是控制和测量。

**LC 振荡器物理原理：**

```
电路结构：

        Vdd
         │
    ┌────┴────┐
    │  电感 L  │    L 和 C 组成谐振回路
    └────┬────┘    类比：弹簧（L 存储磁能）+ 重锤（C 存储电能）
         │         能量在两者之间不断转换 → 产生持续振荡
    ┌────┴────┐
    │  变容管  │  ← 核心：这是一个"电压可调的电容"
    │  C(V)   │    本质是反向偏置的 PN 结二极管：
    └────┬────┘    反向电压高 → 耗尽层宽 → 结电容小
         │         反向电压低 → 耗尽层窄 → 结电容大
        GND

谐振频率公式：
  f = 1 / (2π × √(L × C(V)))

  V 升高 → C(V) 减小 → f 升高
  V 降低 → C(V) 增大 → f 降低

这就是"压控"含义：控制电压直接决定输出频率

典型 VCO 特性曲线（示意）：
  Vctrl  0.3V → 600MHz
  Vctrl  0.7V → 900MHz
  Vctrl  1.1V → 1200MHz
  Vctrl  1.5V → 1500MHz
  （近似线性，实际有非线性，PLL 闭环后不影响精度）

VCO 的致命缺点：
  温度每升高 10°C → 频率漂移 ~0.1%（对 1GHz 就是 1MHz）
  电源电压波动 1% → 频率漂移 ~0.5%
  → VCO 单独使用频率极不稳定，必须加 PLL 闭环控制
```

**环形振荡器 VCO（数字工艺友好）：**

```
奇数个反相器首尾相连：

  ┌──→[NOT]──→[NOT]──→[NOT]──┐
  └───────────────────────────┘

  信号绕一圈的时间 = 3 × t_pd（每个反相器的传播延迟）
  振荡频率 = 1 / (2 × 3 × t_pd)

  调频方法：改变每个反相器的偏置电流
    电流大 → t_pd 小 → 频率高
    电流小 → t_pd 大 → 频率低

  优点：纯数字工艺，面积小，易集成
  缺点：相位噪声比 LC VCO 差（频率稳定性差）
  用途：片内低要求时钟，如测试电路、低速外设
```

### 3.3 ÷M / ÷N 分频器 — 把频率拉到可比较范围

```
为什么需要分频器：

  VCO 输出 1200MHz，参考晶振 24MHz
  频率差 50 倍，PFD 无法直接比较

解决：把 VCO 输出 ÷50 → 24MHz，然后与参考 24MHz 比较

÷N 分频器实现（模 N 计数器）：

  状态：0 → 1 → 2 → ... → N-1 → 0 → 1 → ...
  每个时钟沿状态+1，到 N-1 时下一个沿回到 0 并输出一个脉冲

  VCO 每 N 个周期 → 输出 1 个脉冲
  输出频率 = VCO频率 / N

÷M（前分频器）的作用：

  当目标频率很高时，N 值需要很大
  大 N 值的计数器：
    工作速度必须与 VCO 同步（最高速度路径）
    功耗大，时序难收敛

  引入 ÷M 先把参考频率降低：
    F_ref_effective = F_ref / M = 24MHz / 2 = 12MHz
    则 N 值减半，计数器压力减半

  更重要的用途：调整频率分辨率
    F_step = F_ref / M
    M 越大，可选频率步进越细
```

### 3.4 PFD（鉴频鉴相器）— 测量误差信号

PFD 同时检测两个输入信号的频率差和相位差，是模拟/数字混合电路。

```
两个输入：
  REF：参考信号（晶振 ÷M 后，频率已知、相位稳定）
  FB： 反馈信号（VCO ÷N 后，需要与 REF 同步）

输出：UP 和 DOWN 两路脉冲信号

工作原理（D 触发器实现）：

  REF 上升沿 → 触发器A置1 → UP=1
  FB  上升沿 → 触发器B置1 → DOWN=1
  两者都为1 → 复位信号 → 两个触发器同时清零

  情况1：FB 比 REF 慢（VCO 频率偏低）
    REF 上升沿先到 → UP=1
    延迟一段时间 FB 上升沿才到 → UP=0, DOWN 短暂=1 后立刻复位
    净效果：UP 有宽脉冲，DOWN 只有极短脉冲
    → "需要加速"信号

  情况2：FB 比 REF 快（VCO 频率偏高）
    FB 上升沿先到 → DOWN 有宽脉冲
    → "需要减速"信号

  情况3：完全锁定（同频同相）
    两个上升沿几乎同时到 → 两路都只有极短脉冲（死区内）
    → CP 输出平均电流为零 → VCO 频率不变

死区（Dead Zone）的意义：
  完全锁定时 UP/DOWN 各有一个极短的同步脉冲
  若这些脉冲导致 CP 微小动作 → VCO 微小抖动 → 相位噪声
  死区：当脉冲宽度小于阈值时，CP 不响应
  → 消除锁定时的微小抖振
  但死区也引入非线性 → 设计权衡
```

### 3.5 CP（电荷泵）— 数字脉冲转模拟电压

```
电路结构：

  Vdd
   │
  [PMOS] ←── UP 信号（高有效时 PMOS 导通）
   │
   ├──────→ 输出（接 LPF）
   │
  [NMOS] ←── DOWN 信号（高有效时 NMOS 导通）
   │
  GND

工作逻辑：
  UP 脉冲  → PMOS 导通 → 向 LPF 电容注入正电荷 → 电压↑ → VCO 频率↑
  DOWN 脉冲 → NMOS 导通 → 从 LPF 电容抽走电荷  → 电压↓ → VCO 频率↓
  无脉冲   → 两管截止  → 电容保持电荷           → 电压不变

电压变化量：
  ΔV = I_pump × t_pulse / C_filter

其中 I_pump（泵电流）是关键设计参数，部分 SoC 可通过寄存器调整：
  I_pump 大 → 每个脉冲电压变化大 → 响应快（锁定时间短）→ 相位噪声大
  I_pump 小 → 每个脉冲电压变化小 → 响应慢（锁定时间长）→ 相位噪声小

这是 PLL 带宽设计的核心权衡。
```

### 3.6 LPF（低通滤波器）— 平滑控制电压

```
为什么需要滤波：

  PFD/CP 每个参考时钟周期都产生脉冲（即使已锁定，也有死区脉冲）
  这些周期性脉冲 → 控制电压有纹波（频率 = F_ref）
  → VCO 频率在目标值附近周期性抖动
  → 输出时钟有相位噪声（jitter）

LPF 的任务：
  只让直流分量（真正的控制电压）通过
  滤掉 F_ref 频率的纹波

典型实现（二阶无源滤波器）：
     ┌───R1───┬───→ 到 VCO
     │        │
  CP │       C1        C2
  输出        │        │
              └────R2──┘
              │
             GND

  转移函数：H(s) = (1 + sR2C2) / (s(C1+C2)(1 + sR1C1/(C1+C2)))

参数选择约束：
  截止频率 f_c = 1/(2π√(R1C1)) << F_ref（滤除参考纹波）
  f_c 越低 → 纹波越小 → 但 PLL 响应越慢（闭环带宽窄）
  f_c 越高 → 响应快   → 但纹波大，相位噪声差

  经验法则：f_c ≈ F_ref / 10 ~ F_ref / 20
  典型值：F_ref=24MHz → f_c ≈ 1~2MHz
```

### 3.7 完整的锁定动态过程

```
初始状态：PLL 锁定在 600MHz（N=25）

软件写寄存器：N 从 25 改为 50（目标 1200MHz）

─────────────────────────────────────────────
T=0μs：N 值更新

  ÷N 分频器改为 ÷50
  VCO 还是 600MHz → FB = 600/50 = 12MHz
  REF = 24/1 = 24MHz
  PFD：FB(12MHz) < REF(24MHz)
  → 持续输出宽 UP 脉冲（几乎每个参考周期都是 UP）

─────────────────────────────────────────────
T=0~50μs：追频阶段

  CP 持续向 LPF 注入电荷
  LPF 电容电压缓慢上升（速度由 RC 决定）
  VCO 控制电压上升 → 变容管电容减小 → 频率上升

  VCO 频率轨迹：
  600MHz → 700MHz → 900MHz → 1100MHz → 1180MHz → 1200MHz

  随着 VCO 频率上升，FB 频率也上升
  UP 脉冲越来越窄（误差越来越小）

─────────────────────────────────────────────
T≈50μs：接近锁定

  VCO ≈ 1200MHz → FB = 1200/50 = 24MHz ≈ REF
  PFD：UP 和 DOWN 脉冲宽度接近（微小相位误差）
  LPF 电容电压趋于稳定

─────────────────────────────────────────────
T≈80μs：完全锁定

  VCO = 1200.000MHz（精确）
  FB = 24.000MHz = REF（频率相同，相位对齐）
  PFD：只有死区内的极短同步脉冲
  CP 平均输出电流 = 0
  LPF 电容电压稳定 = V_ctrl_new
  
  LOCK 状态位（只读寄存器）置 1
  内核驱动退出 while(!LOCK) 等待循环

─────────────────────────────────────────────
稳态负反馈维持：

  温度升高 → VCO 频率漂移到 1201MHz
  → FB = 24.02MHz > REF = 24MHz
  → PFD 输出 DOWN 脉冲
  → CP 抽走电荷 → 控制电压略降 → VCO 频率回到 1200MHz
  → 误差消除

  任何扰动（温度/电源/老化）都被这个负反馈回路自动消除
  这就是 PLL 比裸 VCO 频率精度高得多的原因
```

### 3.8 分数 N PLL（Fractional-N PLL）

普通整数 PLL 的频率分辨率 = F_ref（24MHz 步进太大），分数 N PLL 突破这一限制。

```
目标：产生 22.5792MHz（44.1kHz 音频采样 × 512，整数 N 凑不出来）

基本思想：让 N 在两个整数之间高速交替，时间平均值等于目标小数

Sigma-Delta 调制实现：

  目标：N_eff = 49.25 = 49 + 1/4

  硬件实际序列：
    参考周期：1    2    3    4    5    6    7    8  ...
    N 取值：  49   49   49   50   49   49   49   50 ...
              └────────────────┘
              4个周期内：3×49 + 1×50 = 197 个 VCO 周期
              等效 N = 197/4 = 49.25  ✓

  更高精度（24位分辨率）：
    N_eff = 49 + k/2^24，k 可取 0 ~ 2^24-1
    频率分辨率：24MHz / 2^24 ≈ 1.43Hz（远优于整数 PLL 的 24MHz 步进）

Sigma-Delta 的代价：
  N 值高速抖动 → 每个参考周期内误差不同
  → 误差信号有高频噪声分量（Fractional Spurs，分数杂散）
  → 输出时钟有周期性相位抖动

  表现在频谱上：
    目标频率 f_out 两侧出现杂散：f_out ± n × F_ref/k（n=1,2,3...）

应用选择：
  音频时钟：✓（必须用分数N，44.1kHz/48kHz无法整数凑出；人耳感知不到杂散）
  CPU 时钟：谨慎（杂散可能影响某些高速接口的时序余量）
  PCIe/USB：通常用整数PLL（协议对时钟纯度要求高）
  HDMI：✓（像素时钟千奇百怪，必须分数N）
```

### 3.9 寄存器与硬件的对应关系

```
典型 PLL 控制寄存器布局（以全志 H6 SoC 为例）：

PLL_CPU_CTRL_REG（地址 0x000）
  bit[31]     PLL_EN      → 1=PLL 上电，0=PLL 关闭（节省 PLL 自身功耗 ~5mW）
  bit[28]     LOCK_EN     → 1=使能锁定检测
  bit[27]     LOCK        → 只读，1=已锁定（轮询此位等待 set_rate 完成）
  bit[24]     PLL_LDO_EN  → PLL 内部 LDO 使能
  bit[21:20]  PLL_OUT_DIV → 输出后分频（÷1/÷2/÷4）
  bit[16:8]   PLL_N[8:0]  → 反馈分频值（9位，1~511）← 改频率改这里
  bit[1:0]    PLL_M[1:0]  → 前分频值（0=÷1, 1=÷2, 2=÷4）

内核驱动 set_rate 的实际操作：

  int clk_pll_cpu_set_rate(struct clk_hw *hw, unsigned long rate,
                           unsigned long parent_rate)
  {
      struct clk_pll *pll = hw_to_clk_pll(hw);
      u32 reg;
      int n;

      // Step 1：计算新 N 值
      // rate = parent_rate × N / M
      // N = rate × M / parent_rate
      n = DIV_ROUND_CLOSEST(rate, parent_rate / pll->m);

      // Step 2：读取当前寄存器值
      reg = readl(pll->reg);

      // Step 3：更新 N 字段（不改其他位）
      reg &= ~GENMASK(16, 8);          // 清除 N 字段
      reg |= (n << 8) & GENMASK(16,8); // 写新 N 值

      // Step 4：写回寄存器 → 硬件立刻开始重新锁定
      writel(reg, pll->reg);

      // Step 5：等待锁定完成（轮询 LOCK 位，典型 20~100μs）
      return readl_poll_timeout(pll->reg, reg,
                                reg & BIT(28),    // LOCK 位
                                100,              // 轮询间隔 100ns
                                2000);            // 超时 2ms
  }

注意：
  写 N 值到 LOCK 位置1 之间，硬件经历完整的锁定动态过程（见 3.7）
  内核驱动的 readl_poll_timeout 就是在等待这个模拟电路过程完成
  这是纯硬件行为，软件无法加速，只能等待
```

---

## 四、Linux CCF（Common Clock Framework）

### 4.1 为什么需要统一框架

```
不同 SoC 厂商的时钟树结构各异：
  高通 SDM845：200+ 个时钟节点
  全志 H6：    100+ 个时钟节点
  NXP i.MX8：  150+ 个时钟节点
  
但操作模式完全相同：写寄存器改分频比、写寄存器开关 Gate

没有 CCF 时：
  每个驱动直接操作 SoC 特定寄存器
  → 代码重复，无法复用
  → 没有共享管理（两个驱动可能同时操作同一个 PLL，互相覆盖）
  → 没有引用计数（谁还在用这个时钟？可以关掉它吗？）

CCF 提供：
  统一的 clk_hw + clk_ops 抽象接口
  引用计数（use_count），防止在用的时钟被意外关闭
  父子关系管理，set_rate 时自动向上传播
  CLK_SET_RATE_PARENT：子节点 set_rate 时自动更新父节点频率
  全局时钟树 debugfs：/sys/kernel/debug/clk/clk_summary
```

### 4.2 核心数据结构

```c
// 每个时钟节点的硬件抽象
struct clk_hw {
    struct clk_core *core;    // 内部管理对象（引用计数、父子关系在这里）
    struct clk *clk;          // 面向驱动的句柄
    const struct clk_init_data *init;
};

// 驱动实现的操作集（类似 file_operations）
struct clk_ops {
    // 开关（Gate 类节点实现）
    int      (*enable)(struct clk_hw *hw);
    void     (*disable)(struct clk_hw *hw);
    int      (*is_enabled)(struct clk_hw *hw);

    // 频率（Divider / PLL 类节点实现）
    unsigned long (*recalc_rate)(struct clk_hw *hw,
                                 unsigned long parent_rate);
    long     (*round_rate)(struct clk_hw *hw, unsigned long rate,
                           unsigned long *parent_rate);
    int      (*set_rate)(struct clk_hw *hw, unsigned long rate,
                         unsigned long parent_rate);

    // 父节点选择（Mux 类节点实现）
    u8       (*get_parent)(struct clk_hw *hw);
    int      (*set_parent)(struct clk_hw *hw, u8 index);

    // 特殊操作
    int      (*set_rate_and_parent)(struct clk_hw *hw,
                                    unsigned long rate,
                                    unsigned long parent_rate,
                                    u8 index);
    void     (*init)(struct clk_hw *hw);     // 注册时初始化
    void     (*debug_init)(struct clk_hw *hw,
                           struct dentry *dentry); // debugfs 支持
};

// 时钟节点内部管理结构（用户不直接访问）
struct clk_core {
    const char          *name;
    const struct clk_ops *ops;
    struct clk_hw       *hw;
    struct clk_core     *parent;          // 父节点
    struct hlist_head    children;        // 子节点链表
    unsigned int         enable_count;    // Gate 引用计数
    unsigned int         prepare_count;   // prepare 引用计数
    unsigned long        rate;            // 当前频率（缓存值）
    unsigned long        req_rate;        // 请求的频率
    struct clk_notifier_data notifier;   // 频率变化通知链
};
```

### 4.3 CCF 内置的通用节点类型

```c
// CCF 提供了最常见类型的通用实现，厂商驱动直接使用，无需重写

// 1. 固定频率时钟（晶振）
struct clk_hw *clk_hw_register_fixed_rate(struct device *dev,
    const char *name, const char *parent_name,
    unsigned long flags, unsigned long fixed_rate);

// 2. 门控时钟（一个寄存器 bit 控制开关）
struct clk_hw *clk_hw_register_gate(struct device *dev,
    const char *name, const char *parent_name,
    unsigned long flags, void __iomem *reg,
    u8 bit_idx, u8 clk_gate_flags, spinlock_t *lock);

// 3. 分频器时钟
struct clk_hw *clk_hw_register_divider(struct device *dev,
    const char *name, const char *parent_name,
    unsigned long flags, void __iomem *reg,
    u8 shift, u8 width,        // 寄存器中分频值的位位置和宽度
    u8 clk_divider_flags, spinlock_t *lock);

// 4. 多路选择器
struct clk_hw *clk_hw_register_mux(struct device *dev,
    const char *name, const char * const *parent_names,
    u8 num_parents, unsigned long flags,
    void __iomem *reg, u8 shift, u8 width,
    u8 clk_mux_flags, spinlock_t *lock);

// 5. 复合时钟（Gate + Divider + Mux 合一，很常见）
struct clk_hw *clk_hw_register_composite(struct device *dev,
    const char *name, const char * const *parent_names,
    int num_parents,
    struct clk_hw *mux_hw,   struct clk_ops *mux_ops,
    struct clk_hw *rate_hw,  struct clk_ops *rate_ops,
    struct clk_hw *gate_hw,  struct clk_ops *gate_ops,
    unsigned long flags);
```

### 4.4 面向驱动的使用接口

```c
// 驱动获取时钟（从设备树 clocks 属性解析）
struct clk *clk_get(struct device *dev, const char *id);
struct clk *devm_clk_get(struct device *dev, const char *id);
// devm_ 版本：设备卸载时自动 clk_put，避免泄漏

// 准备阶段（可能睡眠，在 probe 时调用）
int clk_prepare(struct clk *clk);
void clk_unprepare(struct clk *clk);

// 使能/禁用（不能睡眠，可在中断上下文调用）
int clk_enable(struct clk *clk);    // use_count++，为 0→1 时硬件开 Gate
void clk_disable(struct clk *clk);  // use_count--，为 1→0 时硬件关 Gate

// 组合操作（等于 prepare + enable）
int clk_prepare_enable(struct clk *clk);
void clk_disable_unprepare(struct clk *clk);

// 频率操作
unsigned long clk_get_rate(struct clk *clk);   // 读当前频率
long clk_round_rate(struct clk *clk, unsigned long rate); // 查最近可达频率
int clk_set_rate(struct clk *clk, unsigned long rate);    // 设置频率

// 父节点操作
int clk_set_parent(struct clk *clk, struct clk *parent); // 切换 Mux

// 典型驱动 probe 流程
static int uart_probe(struct platform_device *pdev)
{
    struct uart_priv *priv;

    // 获取时钟（从 DT clocks 属性）
    priv->clk = devm_clk_get(&pdev->dev, "uart");
    if (IS_ERR(priv->clk))
        return PTR_ERR(priv->clk);

    // 设置频率（CCF 内部自动计算分频比）
    clk_set_rate(priv->clk, 48000000);  // 请求 48MHz

    // 使能时钟
    clk_prepare_enable(priv->clk);

    // ... 初始化 UART 硬件 ...
    return 0;
}

static int uart_remove(struct platform_device *pdev)
{
    clk_disable_unprepare(priv->clk);  // 关闭时钟，节省功耗
    return 0;
}
```

### 4.5 时钟树在 Device Tree 中的描述

```dts
/* SoC 厂商的 .dtsi 文件（描述时钟树拓扑）*/

/ {
    clocks {
        /* 树根：外部晶振 */
        osc24m: oscillator {
            compatible = "fixed-clock";
            #clock-cells = <0>;
            clock-frequency = <24000000>;
            clock-output-names = "osc24M";
        };

        /* PLL（倍频器）*/
        pll_cpu: pll@01c20000 {
            compatible = "allwinner,sun50i-h6-pll-cpu-clk";
            reg = <0x01c20000 0x4>;     /* PLL 控制寄存器地址 */
            #clock-cells = <0>;
            clocks = <&osc24m>;         /* 父节点：从晶振输入 */
            clock-output-names = "pll-cpu";
        };

        /* 分频器 */
        ahb1: ahb1@01c20054 {
            compatible = "allwinner,sun50i-h6-ahb1-clk";
            reg = <0x01c20054 0x4>;
            #clock-cells = <0>;
            clocks = <&pll_cpu>, <&osc24m>;  /* 两个可选父节点（Mux）*/
            clock-output-names = "ahb1";
        };

        /* 带 Gate 的外设时钟 */
        uart_clk: uart@01c2006c {
            compatible = "allwinner,sun50i-h6-gate-clk";
            reg = <0x01c2006c 0x4>;
            #clock-cells = <1>;          /* 1 表示需要一个索引参数 */
            clocks = <&ahb1>;
            clock-output-names = "uart0", "uart1", "uart2", "uart3";
        };
    };

    /* 外设设备引用时钟 */
    uart0: serial@5000000 {
        compatible = "snps,dw-apb-uart";
        reg = <0x5000000 0x400>;
        clocks = <&uart_clk 0>;    /* 引用 uart_clk 的第 0 个输出 */
        clock-names = "baudclk";   /* 驱动中 clk_get(dev, "baudclk") 用此名 */
    };
};
```

### 4.6 debugfs 时钟树查看

```bash
# 查看完整时钟树（频率、使能状态、引用计数）
cat /sys/kernel/debug/clk/clk_summary

   clock                         enable_cnt  prepare_cnt  rate
   ─────────────────────────────────────────────────────────────
   osc24m                              1           1    24000000
   └── pll-cpu                         1           1  1800000000
       └── ahb1                        1           1   600000000
           ├── uart0                   1           1    48000000
           ├── uart1                   0           0    48000000
           └── spi0                    1           1   100000000

# 查看单个时钟节点
cat /sys/kernel/debug/clk/pll-cpu/clk_rate       # 当前频率
cat /sys/kernel/debug/clk/pll-cpu/clk_enable_count
cat /sys/kernel/debug/clk/pll-cpu/clk_flags
```

---

## 五、计时时钟（clocksource）

计时时钟与硬件时钟树无关，是内核用来回答"现在是几点/过了多少纳秒"的机制。

### 5.1 硬件基础：ARM64 Generic Timer

```
ARM64 Generic Timer 寄存器（系统寄存器，EL1 直接访问）：

CNTFRQ_EL0  ：计数器频率（Hz），bootloader 写入，内核只读
              典型值：24000000（24MHz）或 100000000（100MHz）

CNTPCT_EL0  ：物理计数器当前值（64位，单调递增，永不回绕）
              上电后开始计数，每个 tick +1
              读取一次约 20ns（直接寄存器访问）

CNTVCT_EL0  ：虚拟计数器（= 物理计数器 - CNTVOFF_EL2 偏移）
              虚拟机使用，屏蔽宿主机暂停时间

计数器频率与 CPU 频率无关：
  CPU 从 1GHz 变频到 600MHz，CNTPCT 仍以 24MHz 计数
  这是关键设计：即使 DVFS 大幅变频，时间仍然精确
```

### 5.2 clocksource 抽象

```c
struct clocksource {
    u64  (*read)(struct clocksource *cs);  // 读取计数器当前值
    u64  mask;             // 有效位掩码（64位计数器 = 0xFFFFFFFFFFFFFFFF）
    u32  mult;             // 乘法因子（cycles → ns 转换）
    u32  shift;            // 移位因子
    u64  max_idle_ns;      // 最长允许的空闲时间（防止计数器回绕）
    int  rating;           // 精度评分（越高越好，400=最优）
    const char *name;
};

// ARM64 arch timer 的注册（arch/arm64/kernel/time.c）
static struct clocksource clocksource_counter = {
    .name   = "arch_sys_counter",
    .rating = 400,                     // 最高等级
    .read   = arch_counter_read,       // 直接读 CNTPCT_EL0
    .mask   = CLOCKSOURCE_MASK(56),    // 56位有效（硬件保证）
    .flags  = CLOCK_SOURCE_IS_CONTINUOUS,
};

static u64 arch_counter_read(struct clocksource *cs)
{
    return arch_timer_read_counter();  // mrs x0, CNTPCT_EL0
}
```

### 5.3 cycles → 纳秒的转换

```
直接做除法太慢（除法在 ARM64 需要几十个周期），
内核用"乘法+移位"替代除法：

  ns = (cycles × mult) >> shift

mult 和 shift 在注册时预计算：
  mult = (10^9 << shift) / freq
  shift 选择使 mult 尽量大但不溢出 32位

例：freq = 24MHz，shift = 24
  mult = (1_000_000_000 × 2^24) / 24_000_000
       = 16_777_216_000_000_000 / 24_000_000
       = 699_050_667  ≈ 0x29AAA_A6B（未溢出 32位 ✓）

验证：
  cycles = 24（经过 1μs）
  ns = (24 × 699_050_667) >> 24
     = 16_777_216_008 >> 24
     = 1000  ✓（1μs = 1000ns）

ktime_get() 的完整路径：
  ktime_get()
    → timekeeping_get_ns()
    → clocksource.read()          读 CNTPCT_EL0
    → cycles_to_ns(delta, cs)     乘法+移位
    → 加上 timekeeper.base_mono   基准时间
    → 返回 ktime_t（纳秒）
```

---

## 六、定时器时钟（clock_event_device）

### 6.1 硬件基础：Generic Timer 的比较器

```
ARM64 Generic Timer 还有一组比较寄存器，与计数器一起实现定时中断：

CNTP_CVAL_EL0：比较值寄存器（绝对时间）
  当 CNTPCT_EL0 >= CNTP_CVAL_EL0 时，触发中断

CNTP_TVAL_EL0：定时值寄存器（相对时间，写入后自动计算 CVAL）
  写入 N → CNTP_CVAL = CNTPCT + N
  即"N 个 tick 后触发中断"

CNTP_CTL_EL0：控制寄存器
  bit[0] ENABLE  = 1 启用定时器
  bit[1] IMASK   = 0 不屏蔽中断（= 到期时发出中断）
  bit[2] ISTATUS = 只读，1=已到期
```

### 6.2 clock_event_device 抽象

```c
struct clock_event_device {
    void (*event_handler)(struct clock_event_device *);  // 中断处理回调
    int  (*set_next_event)(unsigned long evt,
                           struct clock_event_device *); // 设置下次触发
    int  (*set_next_ktime)(ktime_t expires,
                           struct clock_event_device *);
    int  (*set_state_periodic)(struct clock_event_device *);  // 周期模式
    int  (*set_state_oneshot)(struct clock_event_device *);   // 单次模式
    int  (*set_state_shutdown)(struct clock_event_device *);  // 关闭

    ktime_t next_event;     // 下次触发时间
    u64     max_delta_ns;   // 最大可设定间隔
    u32     mult, shift;    // ns → cycles 转换参数
    int     rating;         // 精度评分
    int     irq;            // 中断号
    const char *name;
};

// ARM64 arch timer 的 set_next_event 实现
static int arch_timer_set_next_event_phys(unsigned long evt,
                                          struct clock_event_device *clk)
{
    unsigned long ctrl = arch_timer_reg_read(ARCH_TIMER_PHYS_ACCESS,
                                             ARCH_TIMER_REG_CTRL);
    ctrl |= ARCH_TIMER_CTRL_ENABLE;
    ctrl &= ~ARCH_TIMER_CTRL_IT_MASK;
    arch_timer_reg_write(ARCH_TIMER_PHYS_ACCESS,
                         ARCH_TIMER_REG_TVAL, evt);  // 写 CNTP_TVAL_EL0
    arch_timer_reg_write(ARCH_TIMER_PHYS_ACCESS,
                         ARCH_TIMER_REG_CTRL, ctrl); // 启用定时器
    return 0;
}
```

### 6.3 jiffies、hrtimer 与调度器的关系

```
时钟中断触发后的处理链：

Generic Timer IRQ
      │
      ▼
  tick_handle_periodic()   或   hrtimer_interrupt()
      │                              │
      ├── jiffies++                  └── 遍历到期的 hrtimer 回调
      │   （低精度，1/HZ 秒）              （纳秒精度）
      │
      ├── update_process_times()
      │     └── scheduler_tick()     ← 调度器时间片检查
      │           └── curr->sched_class->task_tick()
      │                 └── 若时间片耗尽 → 设置 TIF_NEED_RESCHED 标志
      │
      └── run_local_timers()
            └── 触发 TIMER_SOFTIRQ → 执行到期的低精度定时器回调

两种定时器对比：
  jiffies 定时器（timer_list）：
    精度 = 1/HZ（通常 4ms 或 1ms）
    实现：红黑树 + 时间轮，到期触发软中断
    用途：网络超时、设备轮询等低精度场景

  hrtimer（高精度定时器）：
    精度：纳秒（受硬件 jitter 限制，实际 ~1μs）
    实现：per-CPU 红黑树，按 ktime_t 排序
    用途：nanosleep、itimer、调度器抢占点
    要求：CONFIG_HIGH_RES_TIMERS=y，硬件支持单次模式
```

---

## 七、DVFS（动态电压频率调节）

DVFS 是 CPU 变频的核心机制，频率与电压必须联动调整。

### 7.1 频率与电压的物理约束

```
数字电路工作的基本约束：

  触发器（Flip-Flop）的建立时间：
    输入信号必须在时钟上升沿前 t_setup 就稳定
    否则触发器进入亚稳态 → 输出随机 → 逻辑错误

  信号传播延迟 t_pd（关键路径延迟）：
    从上一级触发器输出 → 经过组合逻辑 → 到下一级触发器输入
    这段路径上所有门电路延迟之和

  时序约束：
    时钟周期 T > t_pd + t_setup + t_hold
    即：频率 F < 1 / (t_pd + t_setup + t_hold)

  t_pd 与电压的关系：
    晶体管（MOSFET）的开关速度 ∝ (Vgs - Vth) / 负载电容
    Vdd 高 → Vgs 高 → 电流大 → 充放电快 → t_pd 小 → 可以更高频率

  结论（物理定律，非软件规定）：
    要提高频率 → 必须提高电压（减小 t_pd）
    要降低电压（省电）→ 必须降低频率（增大 T）
    电压不足时强行提频 → 时序违例 → 数据损坏/死机（不是软件崩溃，是硬件错误）
```

### 7.2 OPP（Operating Performance Point）表

```
每个 OPP 是一个（频率，电压）对，由芯片厂商实测后确定

实测流程：
  1. 将芯片置于最差工况（高温、低电压角、工艺角慢）
  2. 在目标频率下逐步降低电压
  3. 找到仍能稳定工作的最低电压 + 裕量（通常 25~50mV）
  4. 这对（频率，电压）就是一个 OPP

设备树中的 OPP 表：
  cpu0_opp_table: opp-table-0 {
      compatible = "operating-points-v2";
      opp-shared;               /* 同 cluster 的 CPU 共享此表 */

      opp-300000000 {
          opp-hz    = /bits/ 64 <300000000>;   /* 300MHz */
          opp-microvolt = <825000>;             /* 0.825V */
          clock-latency-ns = <200000>;          /* 切换延迟 200μs */
      };
      opp-600000000 {
          opp-hz    = /bits/ 64 <600000000>;
          opp-microvolt = <875000>;             /* 0.875V */
      };
      opp-1200000000 {
          opp-hz    = /bits/ 64 <1200000000>;
          opp-microvolt = <1000000>;            /* 1.0V */
      };
      opp-1800000000 {
          opp-hz    = /bits/ 64 <1800000000>;
          opp-microvolt = <1150000>;            /* 1.15V */
          turbo-mode;                           /* 标记为 Boost 档位 */
      };
  };
```

### 7.3 升频/降频的操作顺序

```
升频（低→高）：必须先升压，再升频

  当前：600MHz @ 0.875V
  目标：1200MHz @ 1.0V

  Step 1: 通知 PMIC 升压 0.875V → 1.0V
    内核写 PMIC 寄存器（通过 I2C/SPI 总线，约 100μs）
    PMIC DC-DC 变换器调整占空比，输出电压上升
    等待电压稳定（regulator 驱动的 ramp_delay）

  Step 2: 电压已满足 1200MHz 需求
    写 PLL N 值寄存器
    等待 PLL 重新锁定（约 50μs）

  Step 3: PLL 锁定完成，切换 Glitch-Free MUX 到 PLL 输出
    完成，CPU 跑在 1200MHz @ 1.0V

  若违反顺序（先升频）：
    1200MHz 在 0.875V 下运行 → t_pd 超过时钟周期 → 时序违例
    → 计算结果错误，通常表现为莫名 kernel panic 或内存损坏

─────────────────────────────────────────────

降频（高→低）：必须先降频，再降压

  当前：1200MHz @ 1.0V
  目标：600MHz @ 0.875V

  Step 1: 写 PLL N 值寄存器，降到 600MHz
    等待 PLL 锁定

  Step 2: 降低 PMIC 输出电压 1.0V → 0.875V

  若违反顺序（先降压）：
    1200MHz 在 0.875V 下运行 → 同上，时序违例
```

### 7.4 PLL 切换期间的 Glitch-Free MUX 保护

```
PLL 重锁期间输出频率不稳定，CPU 不能使用：

  正常运行：
    晶振 24MHz → [PLL 1200MHz] → [Glitch-Free MUX] → CPU

  PLL 开始重锁（写 N 值后）：
    [PLL 频率不稳定]
    硬件自动切换：晶振 24MHz → [Glitch-Free MUX] → CPU
    CPU 此时跑 24MHz（很慢，但稳定正确）

  PLL 重锁完成：
    [PLL 新频率稳定] → [Glitch-Free MUX] → CPU
    切换回 PLL 输出

  "Glitch-Free" 的保证：
    切换只在两路时钟都处于低电平时才执行
    → 不产生任何宽度异常的时钟脉冲
    → CPU 看不到切换过程，流水线不受影响
```

### 7.5 Linux cpufreq 框架

```
软件层次：

  用户空间
    │ echo 1200000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed
    ▼
  cpufreq 核心层
    │  管理 OPP 表，维护当前频率状态
    │  调用 governor 的决策函数
    ▼
  cpufreq governor（决策者）
    │  根据负载决定目标频率
    │  schedutil / ondemand / performance / powersave
    ▼
  cpufreq driver（执行者）
    │  SoC 特定实现，知道如何操作 PLL/PMIC 寄存器
    │  qcom-cpufreq / mediatek-cpufreq / arm_big_little 等
    ▼
  regulator 框架（电压）+ CCF（频率）
    │
    ▼
  PMIC 寄存器 + PLL 寄存器（硬件）
```

### 7.6 schedutil governor：调度器感知的变频

```
schedutil 是 Linux 4.7 引入的最现代 governor，
直接从调度器的负载信息决定频率，无需独立采样。

PELT（Per-Entity Load Tracking，进程负载跟踪）：

  每个 CPU 维护一个利用率信号 util（0~1024）
  
  信号更新（指数移动平均）：
    util_new = util_old × y + running_time × (1-y)
    y ≈ 0.5^(1/32)（半衰期约 32ms）
  
  含义：最近活跃的进程权重高，久远的权重低
  时间越新越重要，历史自然衰减

schedutil 的频率决策（每次调度事件后触发）：

  target_freq = max_freq × util / 1024

  向上取整到最近的 OPP 档位

  例：
    util=512（CPU 使用率 50%）
    max_freq=1800MHz
    target_freq = 1800 × 512/1024 = 900MHz
    最近的 OPP：1000MHz（上取整）
    → 调用 cpufreq_driver->target(1000MHz)

  与 ondemand 对比：
    ondemand：独立定时器每 10~50ms 采样一次 CPU 利用率
              采样周期内的负载变化看不到 → 响应慢
    schedutil：每个调度事件（唤醒/睡眠/时钟中断）都重新评估
               进程唤醒时立刻提频 → 响应延迟 < 1ms
```

---

## 八、big.LITTLE / DynamIQ 多簇变频

### 8.1 硬件拓扑与供电域隔离

```
DynamIQ Shared Unit (DSU) 拓扑：

┌─────────────────────────────────────────────────┐
│                  SoC                             │
│  ┌──────────────────────┐                       │
│  │ Cluster 0（小核）     │ 独立供电域 A           │
│  │ 4× Cortex-A55        │ 独立 PLL_LITTLE        │
│  │ OPP：300/600/800MHz  │ 独立 DCDC 电源轨       │
│  └──────────────────────┘                       │
│  ┌──────────────────────┐                       │
│  │ Cluster 1（大核）     │ 独立供电域 B           │
│  │ 4× Cortex-A78        │ 独立 PLL_BIG           │
│  │ OPP：600/1000/1800MHz│ 独立 DCDC 电源轨       │
│  └──────────────────────┘                       │
│  ┌──────────────────────────────────────────┐   │
│  │         DSU（共享 L3 Cache 8MB）          │   │
│  └──────────────────────────────────────────┘   │
│  ┌───────────────────┐                          │
│  │   内存控制器       │ 独立供电域 C              │
│  └───────────────────┘ 独立 PLL_DDR             │
└─────────────────────────────────────────────────┘

供电域隔离的节能效果：
  功耗 P ∝ C × V² × F

  小核：600MHz @ 0.75V → P ∝ C × 0.5625 × 600M = C × 337.5M
  若用大核电压（1.15V）：P ∝ C × 1.3225 × 600M = C × 793.5M
  节省：57%

  这就是为什么移动 SoC 要费力设计多供电域
```

### 8.2 进程迁移的硬件保证

```
进程从大核迁移到小核的过程（对进程完全透明）：

  Step 1: 调度器决定迁移
    load_balancer 发现大核负载低，小核有空闲
    选择目标小核

  Step 2: 上下文保存（在大核上执行）
    调度器 context_switch()
    → __switch_to() 保存寄存器到 task_struct.thread.cpu_context：
      x19-x28（callee-saved）
      sp, pc, fp

  Step 3: Cache 一致性保证（硬件完成）
    大核写的数据在大核 L1/L2 Cache 中
    DSU 的 L3 Cache 是两个 cluster 共享的
    MESI 协议保证：大核把数据写到 L3 后，小核读 L3 能看到最新数据
    无需软件干预，硬件自动维护

  Step 4: 小核恢复上下文
    从 task_struct.thread.cpu_context 恢复寄存器
    继续执行

  对进程的透明性：
    进程的 PC、寄存器状态、内存数据完全一致
    进程无法感知自己换了一个物理核心
    （除非通过 sched_getaffinity 主动查询）
```

---

## 九、Turbo Boost（短时超频）

### 9.1 热设计功耗（TDP）与热容量

```
TDP（Thermal Design Power）：
  散热系统能持续带走的最大功耗
  例如 15W TDP = 风扇+散热片必须能持续处理 15W 热量

"持续"是关键词：
  硅芯片本身有热容量（比热 × 质量）
  就像一块铁：短时间加热 30W，温度上升但不会立刻超限
                持续加热 30W，温度持续上升直到超限

Turbo 的逻辑：
  正常功耗 = TDP = 15W，跑在基础频率 1.8GHz
  短时峰值 = 30W，允许跑更高频率 2.4GHz
  持续时间由温度限制（几秒到几十秒，视散热条件）
```

### 9.2 硬件热管理单元（TMU）

```
芯片内部每个温度敏感区域都有热敏二极管：

  热敏二极管原理：
    PN 结正向电压 V_be 随温度线性变化：约 -2mV/°C
    精确测量 V_be → 换算出温度
    精度：±1°C（远优于外部热敏电阻）

  典型传感器布局（8核 SoC）：
    大核 Cluster × 4个传感器（每核一个）
    小核 Cluster × 4个传感器
    GPU × 2个传感器
    内存控制器 × 1个传感器
    共 11 个温度采样点

TMU 的硬件动作（无需软件干预）：
  温度阈值寄存器：
    TRIP0 = 85°C → 触发软件中断，通知 Linux thermal 框架
    TRIP1 = 95°C → 硬件自动触发 CPU 降频（不等软件）
    TRIP2 = 105°C → 硬件强制关闭部分 CPU 核心
    TRIP3 = 115°C → 硬件紧急关机（写 PMIC 寄存器断电）

Turbo 的时间维度（示意）：

  时间轴：  0     100ms    500ms   2000ms
  功率：    ████████████████                 ← Boost（~25W）
                            ████████████████ ← 降回 TDP（15W）
  温度：         ╱‾‾‾‾╲
                ╱      ╲________________________
  频率：   ─────────────╲
                          ╲____________________ ← TMU 触发降频
```

### 9.3 Linux thermal 框架与 cpufreq 的联动

```
Linux thermal 框架的角色：

  /sys/class/thermal/thermal_zone0/temp   ← 读取温度（单位：毫度）
  /sys/class/thermal/cooling_device0/     ← 冷却设备（CPU 频率限制）

  thermal governor（如 step_wise）：
    温度 > TRIP0(85°C) → 请求 cpufreq 限制最大频率
    温度每超出 1°C → 再降一档
    温度回落后 → 逐步解除限制

  cpufreq 的 thermal 限制机制：
    struct cpufreq_policy {
        unsigned int cpuinfo_max_freq;  // 硬件支持的最高频率（不变）
        unsigned int max;               // 当前允许的最高频率（thermal 可降低此值）
        unsigned int min;               // 当前允许的最低频率
    };

    thermal 框架通过 cpufreq_update_policy() 修改 max 字段
    schedutil 选频时：target_freq = min(schedutil_freq, policy->max)

Linux 感知 Boost 的接口：
  /sys/devices/system/cpu/cpufreq/boost
    echo 1 → 允许调度器请求超过基础频率的 OPP 档位
    echo 0 → 禁止 Boost，封顶在基础频率
```

---

## 十、内存/总线变频（devfreq）

### 10.1 内存子系统的独立时钟域

```
内存访问路径上的三个独立时钟域：

  CPU @ 1.8GHz（独立 PLL_CPU）
      │
      │ 读写请求（经过 L1/L2 Cache）
      ▼ Cache Miss
  内存控制器 @ 400MHz（独立 PLL_BUS）
      │ DDR 命令（RAS/CAS/DATA）
      ▼
  LPDDR5 SDRAM @ 3200MHz（等效，DDR = 双倍速率，时钟 1600MHz）

三个时钟完全独立：
  CPU 变频不影响内存时钟
  内存时钟单独由 devfreq 管理
  但它们通过"内存带宽利用率"间接关联
```

### 10.2 devfreq 框架

```c
// 类似 cpufreq 对 CPU，devfreq 对内存/GPU/总线等外设做变频管理

struct devfreq {
    struct device        *dev;
    struct devfreq_dev_profile *profile;  // 设备特定的 OPP 和操作
    const struct devfreq_governor *governor;
    struct notifier_block nb;             // OPP 变化通知
    unsigned long previous_freq;
    unsigned long min_freq, max_freq;
};

// simple_ondemand governor 的决策逻辑
static int devfreq_simple_ondemand_func(struct devfreq *df,
                                         unsigned long *freq)
{
    struct devfreq_dev_status *stat = &df->last_status;

    // stat->busy_time：采样周期内内存控制器忙碌时间
    // stat->total_time：采样周期总时间
    // 利用率 = busy_time / total_time

    unsigned long target = stat->current_frequency;
    unsigned long numerator = stat->busy_time;
    unsigned long denominator = stat->total_time;

    // 利用率 > 90%：升到最高频
    if (numerator * 10 > denominator * 9)
        *freq = df->max_freq;
    // 利用率 < 30%：降到最低频
    else if (numerator * 10 < denominator * 3)
        *freq = df->min_freq;
    // 中间：按比例选择
    else
        *freq = df->max_freq * numerator / denominator;

    return 0;
}
```

### 10.3 内存带宽与 CPU 性能的关系

```
内存墙（Memory Wall）问题：

  CPU 每个 Cache Miss 需要等待内存：
    L1 命中：4个 CPU 周期（~2ns @ 2GHz）
    L2 命中：12个 CPU 周期
    L3 命中：40个 CPU 周期
    主内存：100ns = 200个 CPU 周期 ← 等待时间占主导

  内存频率与延迟：
    LPDDR4  1600MHz：延迟 ~60ns
    LPDDR4X 2133MHz：延迟 ~50ns
    LPDDR5  3200MHz：延迟 ~40ns

  内存频率过低时：
    Cache Miss 等待时间增加
    CPU 流水线停顿（stall）增多
    即使 CPU 频率很高，实际性能也上不去

  最优策略（ARM 的 DSU 文档建议）：
    内存带宽利用率 > 50%：内存频率应与 CPU 频率同步提升
    内存带宽利用率 < 20%：可以大幅降低内存频率（省电优先）
```

---

## 十一、频率可调节性：离散而非连续

### 11.1 为什么不支持无极调整

```
"无极调整"意味着连续可调（任意频率）。
数字电路的 PLL 为什么只能离散调节：

  核心公式：F_out = F_ref × N / M
  N 和 M 只能取整数（计数器只能计整数个脉冲）
  → 输出频率是离散的有限集合

  两个相邻可选频率之间有间隔（步进）：
    F_step = F_ref / M = 24MHz / M
    M=1 时步进 = 24MHz（很粗）
    M=4 时步进 = 6MHz（稍细）
    分数N时步进 ≈ Hz 级别（极细，但仍是离散）

真正连续调频需要纯模拟 VCO（无 PLL 约束）：
  → 频率随温度/电压大幅漂移（不可接受的不稳定性）
  → 数字系统必须使用 PLL 锁定频率，代价是离散性

实际影响：
  cpufreq 的 available_frequencies 列出所有可选档位
  请求一个不在列表中的频率 → 自动 round 到最近档位
  这是 CCF 中 round_rate 操作的作用
```

### 11.2 频率数量是 SoC 级别固定的

```
时钟树节点数量（PLL 个数、Divider 个数、Gate 个数）
在芯片流片时固定进硅里，软件无法增减。

典型 SoC 规模：
  Qualcomm SDM845：~250 个 CCF 时钟节点
  MediaTek MT6983：~200 个时钟节点
  Apple M2（推测）：50+ 个可见时钟域（大量细节不公开）
  树莓派 BCM2712：~80 个时钟节点

Linux CCF 只是给这些固定硬件节点建立软件模型：
  每个节点一个 struct clk_core
  节点间父子关系 = 时钟信号的物理连接关系
  软件操作 = 写对应的寄存器

不能通过软件"创造"新的时钟节点，
就像不能通过软件创造新的 CPU 核心一样。
```

---

## 十二、各时钟机制关系总览

```
硬件时钟树（CCF 管理）
│
│  XTAL 24MHz → PLL → Divider → Gate → 各外设时钟信号
│
├── 其中一路 → ARM Generic Timer 硬件
│               │
│               ├── CNTPCT_EL0（计数器）→ clocksource
│               │     └── ktime_get() / gettimeofday()
│               │
│               └── CNTP_TVAL_EL0（比较器）→ clock_event_device
│                     └── hrtimer / jiffies / 调度器 tick
│
├── 其中一路 → CPU Core
│               └── cpufreq（DVFS）
│                     ├── OPP 表（频率+电压对）
│                     ├── schedutil governor
│                     └── PLL N 值 + PMIC 电压联动调整
│
├── 其中一路 → DDR 内存控制器
│               └── devfreq（内存变频）
│                     └── 按带宽利用率动态调整
│
└── 其他路 → GPU / ISP / NPU / USB / UART / ...
              └── 各设备驱动通过 CCF 接口管理

变频触发层次（响应速度从快到慢）：
  硬件 TMU（热保护）      ~1ms    全自动，软件不可见
  schedutil governor     ~1ms    调度事件触发
  devfreq ondemand       ~20ms   定时采样触发
  thermal 框架           ~100ms  温度超阈值触发
  用户空间 cpufreq        用户写文件  手动

底层物理约束（所有机制的共同根因）：
  功耗 P = α × C × V² × F + I_leak × V
  V 是平方项 → 降压比降频节电效果更显著
  降压依赖降频（时序约束）→ DVFS 必须联动
  PLL 输出离散 → 变频只能在 OPP 档位间跳跃
```

---

## 附录：常用命令速查

```bash
# 查看 CPU 频率
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

# 手动设置频率（需要 performance 或 userspace governor）
echo userspace > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo 1200000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed

# 查看完整时钟树
cat /sys/kernel/debug/clk/clk_summary

# 查看特定时钟节点
cat /sys/kernel/debug/clk/pll-cpu/clk_rate
cat /sys/kernel/debug/clk/pll-cpu/clk_enable_count

# 查看内存频率（devfreq）
cat /sys/class/devfreq/*/cur_freq
cat /sys/class/devfreq/*/available_frequencies

# 查看 CPU 温度
cat /sys/class/thermal/thermal_zone0/temp    # 单位：毫摄氏度，除以1000是°C

# 查看 OPP 表
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies

# 开启/关闭 Turbo Boost
cat /sys/devices/system/cpu/cpufreq/boost
echo 1 > /sys/devices/system/cpu/cpufreq/boost

# 查看 cpufreq 统计（每个频率档位停留时间）
cat /sys/devices/system/cpu/cpu0/cpufreq/stats/time_in_state
```

---

## 十三、频率无法满足时的处理机制

CCF 的频率协商分四个层次，从"尽力逼近"到"明确报错"，**判断误差是否可接受始终是驱动自己的职责，CCF 只负责找最近可达值**。

### 13.1 第一层：`round_rate` — 找最近可达频率

驱动调用 `clk_set_rate(clk, 48_000_000)` 时，CCF 不会直接写寄存器，先走协商：

```
clk_set_rate(uart_clk, 48_000_000)
      │
      ▼
clk_round_rate(uart_clk, 48_000_000)
      │  沿时钟树向上传播，每个节点计算自己能提供的最近值
      │
      └── uart_clk（Divider 节点）：
            parent_rate = 600MHz（ahb_clk 当前频率）

            可选除数：
              ÷12 → 50.000MHz   误差 +4.2%
              ÷13 → 46.154MHz   误差 -3.8%

            取哪个由 Divider 的标志位决定：
              默认（无标志）：    向下取整 → 46.154MHz
              CLK_DIVIDER_ROUND_CLOSEST：取最近 → 50.000MHz
              CLK_DIVIDER_CEILING：向上取整 → 50.000MHz

            round_rate 返回实际会被设置的频率
            驱动可在 set_rate 前先调 round_rate 预判结果
```

round_rate 沿树向上传播的完整路径：

```
uart_clk.round_rate(48MHz)
  → 问父节点 ahb_clk：当前 600MHz 能否整除出 48MHz？
    → 不能（600/48=12.5）
    → 如果 uart_clk 设了 CLK_SET_RATE_PARENT：
        继续问 ahb_clk.round_rate：
        "你能换一个频率，让我整除出 48MHz 吗？"
          → ahb_clk 问上游 PLL：
              48MHz × 12 = 576MHz，PLL 能输出 576MHz 吗？
              N=24 → 24×24=576MHz ✓
          → 全链路最优解：PLL→576MHz, ahb÷12→48MHz 精确
    → 如果没有 CLK_SET_RATE_PARENT：
        只能在 600MHz 基础上取最近整除 → 返回 50MHz 或 46.154MHz
```

### 13.2 第二层：驱动的容忍策略

不同外设对频率误差的容忍度截然不同，这决定了驱动拿到"近似值"后的行为：

```
UART（容忍率高，~5%）：
  请求 48MHz，实际得到 46.154MHz
  波特率误差 = |48 - 46.154| / 48 = 3.8%
  UART 协议允许最大 ±5% → 可以接受

  正确做法：用实际频率重新计算分频寄存器
    actual = clk_get_rate(priv->clk);     // 读回 46.154MHz
    divisor = actual / (baudrate × 16);   // 用实际值计算，不假设精确
    writel(divisor, UART_DLL);
  → 波特率自动校正，通信正常

I2C（容忍率中等，~10%）：
  标准 100kHz / 快速 400kHz，有一定误差余量
  驱动通常也用实际频率重新计算 SCL 分频

USB（零容忍，±500ppm = 0.05%）：
  协议硬性要求 480MHz ± 0.05%
  偏差超限 → 设备枚举失败，完全无法工作
  → SoC 设计时必须给 USB 专用 PLL（PLL_USB）
    选择 N/M 使得 F_ref × N / M = 480.000000MHz 精确
  → USB 驱动拿不到精确值 → 直接返回 -EINVAL，不凑合

PCIe（零容忍）：
  Gen3 参考时钟 250MHz ± 300ppm
  通常用专用 PLL 或外部晶振直接提供

HDMI 像素时钟（需要分数N PLL）：
  1080p60：148.5MHz（无法由整数 PLL 精确产生）
  4K60：594MHz
  → 必须用分数N PLL 凑出精确像素时钟
  → 否则画面出现帧率漂移或色彩条纹
```

### 13.3 第三层：`determine_rate` — 全树协商

比 `round_rate` 更强大，允许通过修改父节点频率来精确满足子节点需求：

```c
// clk_ops 中的 determine_rate 钩子
int (*determine_rate)(struct clk_hw *hw,
                      struct clk_rate_request *req);

// clk_rate_request 包含：
struct clk_rate_request {
    unsigned long rate;           // 请求的目标频率
    unsigned long min_rate;       // 可接受的最低频率
    unsigned long max_rate;       // 可接受的最高频率
    unsigned long best_parent_rate; // 协商出的父节点最优频率
    struct clk_hw *best_parent_hw;  // 最优父节点（Mux 情况下可能切换）
};
```

协商过程示例：

```
场景：uart_clk 要求精确 48MHz
      ahb_clk 当前 600MHz，÷12.5 无法整除

CLK_SET_RATE_PARENT 触发向上传播：

  uart_clk.determine_rate(48MHz)：
    计算：需要父节点提供 48×N（N 为整数）
    最优：48 × 12 = 576MHz（÷12 精确）
    → req.best_parent_rate = 576MHz

  ahb_clk.determine_rate(576MHz)：
    ahb_clk 自身也是 Divider，继续向上问 PLL
    576MHz = 24MHz × 24 → N=24，PLL 支持
    → req.best_parent_rate = 576MHz（告诉 PLL）

  PLL.set_rate(576MHz)：
    写 N=24 寄存器，等待锁定

  ahb_clk.set_rate(576MHz)：
    ÷1（或保持不变，取决于 ahb 自身 Divider）

  uart_clk.set_rate(48MHz)：
    576MHz ÷ 12 = 48.000MHz ✓ 精确！

代价与风险：
  修改 ahb_clk（总线时钟）会影响挂在同一总线上的所有外设
  所有依赖 ahb_clk 的设备时钟都会变化
  → 生产代码中总线时钟通常不带 CLK_SET_RATE_PARENT
  → 只有叶节点（外设私有时钟）才允许向上传播

频率变化通知链（保护依赖此时钟的其他驱动）：
  PLL 频率变化前 → 触发 PRE_RATE_CHANGE 通知
    依赖此 PLL 的驱动收到通知 → 暂停操作（如 DDR 控制器刷写缓冲）
  PLL 频率变化后 → 触发 POST_RATE_CHANGE 通知
    驱动恢复操作，用新频率重新计算自己的参数
```

### 13.4 第四层：明确报错

```c
int ret = clk_set_rate(clk, rate);

// 以下情况 CCF 直接返回错误，不做任何硬件操作：

// 1. 时钟节点标记为不可变频
if (clk->flags & CLK_IS_FIXED)
    return -EINVAL;   // 晶振、固定输出 PLL 等

// 2. 请求频率超出硬件物理范围
if (rate > clk->max_rate || rate < clk->min_rate)
    return -EINVAL;

// 3. 时钟正在使用且标记了 CLK_SET_RATE_GATE
//    只允许在 disable 状态下改频（某些 PLL 要求）
if ((clk->flags & CLK_SET_RATE_GATE) && clk->enable_count)
    return -EBUSY;

// 4. 父节点的 notifier 回调拒绝了频率变更
//    驱动可通过 clk_notifier_register 注册回调
//    回调返回 NOTIFY_BAD → set_rate 中止，返回 -EBUSY

// 驱动的完整防御写法：
ret = clk_set_rate(priv->clk, desired_rate);
if (ret) {
    dev_err(dev, "clk_set_rate(%lu) failed: %d\n", desired_rate, ret);
    return ret;
}

// 无论 set_rate 成功与否，都应读回实际值
actual_rate = clk_get_rate(priv->clk);

// 驱动自己判断误差是否可接受
error_ppm = abs((long)actual_rate - (long)desired_rate) * 1000000
            / desired_rate;

if (error_ppm > MAX_ALLOWED_PPM) {
    dev_err(dev, "频率误差 %lu ppm 超出允许范围 %d ppm\n",
            error_ppm, MAX_ALLOWED_PPM);
    clk_disable_unprepare(priv->clk);
    return -EINVAL;
}

// 用实际频率重新校正硬件参数
recalculate_dividers(priv, actual_rate);
```

### 13.5 职责边界总结

```
CCF 的职责（机制层）：
  ✓ round_rate：找硬件上最接近的可达频率
  ✓ determine_rate：全树协商最优解
  ✓ set_rate：执行实际的寄存器写入
  ✓ clk_get_rate：返回实际被设置的频率
  ✗ 不判断误差是否可接受（不知道外设协议的容忍范围）
  ✗ 不知道 UART 能容忍 5% 但 USB 不能容忍 0.1%

驱动的职责（策略层）：
  ✓ 调用 clk_round_rate 预查询，决定是否继续
  ✓ 调用 clk_get_rate 读回实际值
  ✓ 用实际频率重新计算自己的分频寄存器
  ✓ 根据外设协议规范判断误差是否可接受
  ✓ 误差超限时返回错误，而不是带病运行

SoC 设计的职责（硬件层，最根本）：
  ✓ 为零容忍外设（USB/PCIe/HDMI）配备专用 PLL
  ✓ 选择 N/M 参数使专用 PLL 能精确输出所需频率
  ✓ 为音频配备分数N PLL（44.1kHz/48kHz 系列）
  这些是流片前就必须解决的问题，软件无法在运行时弥补
```

---

## 十四、数字电路为什么需要时钟

理解时钟的根本原因，比知道 API 更重要。

### 14.1 组合逻辑与时序逻辑

```
组合逻辑（无状态）：
  输入 → 若干逻辑门 → 输出
  输入变化后，输出经过传播延迟 t_pd 后跟着变化
  没有"记忆"，没有"当前状态"，不需要时钟

  A ──→ [AND] ──→ Y（Y = A AND B，A 变化后 ~1ns 内 Y 跟着变）
  B ──→

时序逻辑（有状态）：
  需要"记住"某个时刻的值，在确定的时刻才更新状态
  基本单元：D 触发器（Flip-Flop）

  D 触发器行为：
    时钟上升沿到来时：Q = D（锁存当时 D 端的值）
    其余时刻：Q 保持不变，无论 D 怎么变化

        ┌─────┐
  D ───→│     ├──→ Q
        │  FF │
  CLK ─→│ ▲   │
        └─────┘

CPU 寄存器、Cache 存储单元、状态机的每个状态位，
底层全部是 D 触发器组成的阵列。
```

### 14.2 为什么必须有统一时钟

多级逻辑链中，每一级的输出是下一级的输入。若没有统一的"采样时刻"：

```
问题场景（两级加法器，无时钟）：

  A, B ──→ [加法器1] ──→ S1 ──→ [加法器2] ──→ S2

  t=0：    A=0, B=0，S1=0，S2=0（稳定）
  t=1ns：  A 变为 1
  t=3ns：  S1 开始从 0 变化（传播延迟）
  t=4ns：  S1 处于中间状态（既不是 0 也不是 1）
  t=5ns：  S1 稳定为新值
  t=8ns：  S2 才稳定（还需要 3ns 传播）

  如果在 t=4ns 时读取 S2：读到的是垃圾值

时钟的解决方案：
  在每一级之间插入触发器，规定统一采样时刻：

  A, B ──→ [加法器1] ──→ [FF] ──→ [加法器2] ──→ [FF] ──→ 输出
                          CLK↑                    CLK↑

  时钟周期 T 设置为 > 单级最长传播延迟（如 6ns）
  每个 CLK 上升沿：
    所有 FF 同时采样各自的输入（此时信号已稳定）
    同时更新各自的输出

  结果：
    第1个 CLK↑：FF1 锁存 S1（加法器1的结果）
    第2个 CLK↑：FF2 锁存 S2（加法器2用已稳定的 S1 计算的结果）
    每一步都是确定性的，不存在采样到中间态

这就是时钟的本质：
  不是给电路"提供能量"
  而是给所有触发器规定"统一的快照时刻"
  在这个时刻之前：各自完成组合逻辑计算
  在这个时刻：所有人同时锁存结果，进入下一个状态
```

### 14.3 建立时间与保持时间

```
触发器对输入信号有严格的时间要求：

         建立时间        保持时间
         t_setup         t_hold
           │←──────→│←──→│
  D: ──────XXXXXXXX─────────────
                    ↑
                  CLK 上升沿

  t_setup（建立时间）：
    CLK 上升沿前，D 必须保持稳定的最短时间
    违反 → 触发器进入亚稳态（输出不确定）

  t_hold（保持时间）：
    CLK 上升沿后，D 必须继续保持稳定的最短时间
    违反 → 触发器采到错误值

时序约束公式：
  T_clk > t_pd（组合逻辑延迟） + t_setup + t_clock_skew

  t_clock_skew：时钟信号到达不同触发器的时间差
    同一块芯片上时钟树有精心设计的布线，使 skew < 50ps
    这是时钟树综合（CTS，Clock Tree Synthesis）的核心工作

最高工作频率：
  F_max = 1 / (t_pd_max + t_setup + t_skew)
  t_pd_max 是整个电路中最长的组合逻辑路径（关键路径）
  提高 F_max 的方法：
    提高电源电压（加快晶体管开关速度，减小 t_pd）→ DVFS 的物理基础
    优化布局布线（缩短关键路径）→ 芯片设计工程师的工作
    流水线化（把长路径切短）→ CPU 流水线加深的原因
```

### 14.4 亚稳态（Metastability）

```
当 D 在 t_setup 窗口内发生变化，触发器进入亚稳态：

  Q 输出既不是稳定的 0 也不是稳定的 1
  是一个中间电压（VDD/2 附近）
  会随机向 0 或 1 收敛，收敛时间理论上无界（指数分布）

              亚稳态
  Q: ────────/\/\/\/────→ 随机收敛到 0 或 1
                 ↑
             持续时间不确定（通常 < 1ns，极少超过几ns）

亚稳态的危害：
  若下一级触发器在亚稳态期间采样 → 又产生亚稳态
  亚稳态沿流水线扩散 → 整个电路状态不确定

发生场景：
  1. 跨时钟域（最常见）：两个异步时钟域之间传数据
  2. 异步输入：外部按钮、传感器信号进入同步电路
  3. 时钟抖动过大：CLK 沿偏移到 D 变化附近

解决：双触发器同步器（Double-FF Synchronizer）

  异步信号 ──→ [FF1] ──→ [FF2] ──→ 安全使用
                CLK↑      CLK↑

  FF1 可能进入亚稳态，但给它整整一个时钟周期恢复
  对于 1GHz 时钟（1ns 周期），亚稳态在 1ns 内未收敛的概率：
    P ≈ exp(-1ns / τ)，τ ≈ 20ps
    P ≈ exp(-50) ≈ 10^-22（每次传输）
    即使每秒传输 10^9 次，平均 10^13 秒（~300万年）才出错一次
  FF2 采到的已经是稳定值

高速接口的处理（FIFO）：
  双 FF 同步器适用于单比特、低速信号
  多比特数据跨时钟域 → 使用异步 FIFO
  写端：写时钟域写指针
  读端：读时钟域读指针
  指针用格雷码编码（相邻值只有 1 bit 变化）后跨域同步
  → 避免多比特同时变化导致的亚稳态
```

### 14.5 时钟在典型硬件中的具体使用

#### CPU 流水线

```
5 级流水线，每级之间有流水线寄存器（触发器组）：

        IF         ID         EX         MEM        WB
  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐
  │  取指    │ │  译码    │ │  执行    │ │  访存    │ │  写回    │
  └────┬─────┘ └────┬─────┘ └────┬─────┘ └────┬─────┘ └──────────┘
       │  [FF]      │  [FF]      │  [FF]      │  [FF]
       └────────────┘────────────┘────────────┘

每个时钟沿：所有流水线寄存器同时更新
  CLK↑ 时刻：
    IF 阶段结果 → 存入 IF/ID 流水线寄存器
    ID 阶段结果 → 存入 ID/EX 流水线寄存器
    EX 阶段结果 → 存入 EX/MEM 流水线寄存器
    ...

时钟频率 = 1 / 最慢那一级的传播延迟
现代 CPU 流水线深达 15~20 级就是为了让每级更短，频率更高
代价：分支预测错误时需要清空更多级的流水线（性能损失更大）
```

#### DRAM 的时序参数

```
DDR4 读操作完整时序（以 DDR4-3200 为例，时钟 1600MHz，周期 0.625ns）：

CLK: ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑
     1  2  3  4  5  6  7  8  9 10 11 12 13 14

周期1：    发出 ACTIVATE 命令（打开行，电容电荷转移到位线）
周期1~4：  等待 tRCD = 4（Row to Column Delay，行列间延迟）
周期5：    发出 READ 命令（选列地址）
周期5~9：  等待 CL = 4（CAS Latency，列地址选通延迟）
           内部读放大器工作，数据驱动到 DQ 引脚
周期10：   第一个数据字出现在 DQ 引脚

总延迟：tRCD + CL = 4 + 4 = 8 个时钟周期
实际时间：8 × 0.625ns = 5ns

所有时序参数（tRCD/CL/tRP/tRAS...）都以时钟周期为单位：
  同样的 DDR4-3200 参数 4-4-4
  换到 DDR4-1600（800MHz）：4 周期 × 1.25ns = 5ns（绝对时间相同）
  这就是为什么 DDR 频率升级需要同时考虑时序参数
```

#### SPI / I2C 通信接口

```
SPI（有时钟线 SCK，同步通信）：

  主设备产生 SCK，从设备跟随节拍采样 MOSI

  MOSI: ─[D7]──[D6]──[D5]──[D4]──[D3]──[D2]──[D1]──[D0]─
  SCK:  ─┐ └─┐ └─┐ └─┐ └─┐ └─┐ └─┐ └─┐ └─
          上升沿 = "此时 MOSI 稳定，请采样"

  SCK 频率由主设备控制：
    外设时钟（CCF 提供）÷ 分频比 = SCK 频率
    从设备有最高 SCK 频率限制（datasheet 规定）

I2C（有时钟线 SCL，同步通信，支持多主多从）：

  SCL 高：SDA 必须稳定（主设备在此采样）
  SCL 低：SDA 允许变化（准备下一个 bit）

  Clock Stretching（时钟拉伸）：
    从设备处理太慢时，可以拉低 SCL 阻止主设备继续
    主设备检测到 SCL 被拉低 → 等待从设备准备好
    → 硬件层面的流控机制，无需软件干预

UART（无时钟线，异步通信）：
  用"波特率"约定采样间隔，两端各自用本地时钟计时

  发送方：每隔 1/baudrate 秒发送一位
  接收方：检测起始位下降沿后，以相同间隔采样

  为什么允许 ±5% 误差：
    一帧 10 bit（1起始+8数据+1停止）
    到第 10 bit 时累计时钟偏差 = 10 × 5% = 50% 的位宽
    采样点在位中间，有 50% 余量 → 不出错
    超过 5% 误差 → 累计偏移超过采样窗口 → 采到错误位
```

### 14.6 时钟失效的后果

```
情况1：时钟 Gate 关闭
  所有触发器停止更新，电路"冻结"
  输出保持最后一次时钟沿锁存的值，不响应任何输入
  → Gate 节电的原理：电路停止计算，动态功耗归零
  → 状态保留，重新开启 Gate 后从冻结点继续

情况2：频率过高（超过 t_pd + t_setup）
  触发器 D 端在 CLK 上升沿到来时还在变化
  触发器采到不确定值，进入亚稳态
  → 状态机跳到未预期状态
  → CPU：执行随机指令 / 内存数据损坏
  → 不是软件崩溃，是硬件级别的错误，无法 catch
  → DVFS 降压超频必然导致此问题

情况3：时钟毛刺（Glitch，多余的窄脉冲）
  触发器意外更新一次，状态机跳到错误状态
  来源：Mux 切换时产生的窄脉冲（Glitch-Free MUX 解决此问题）
  后果与情况2类似，且极难调试（偶发性）

情况4：时钟抖动（Jitter）
  每个 CLK 上升沿不在精确时间点，有 ±Δt 偏差
  有效建立时间 = T - t_pd - 2×Δt（两端各扣一个 Δt）
  Δt 越大 → 可用频率越低
  PLL 的相位噪声指标就是在量化 Δt
  高速 SerDes（PCIe/USB3/DDR）对 jitter 极敏感：
    PCIe Gen4：总允许 jitter < 3ps（！）
    这就是为什么高速接口用专用低噪声 PLL
```

---

## 十五、用户态时间接口

### 15.1 时钟类型（POSIX clock_id）

Linux 向用户态暴露多种时钟，语义各不相同：

```
CLOCK_REALTIME（墙钟时间）：
  含义：Unix 纪元（1970-01-01 00:00:00 UTC）以来的秒/纳秒
  特点：可以被 NTP/adjtime 向前或向后跳变
  使用场景：记录事件发生的绝对时间（日志时间戳）
  不适合：测量时间间隔（NTP 跳变会导致负数间隔）

CLOCK_MONOTONIC（单调时间）：
  含义：系统启动后经过的时间（单调递增，不跳变）
  特点：NTP 调整时只改变走速，不跳变；suspend 期间暂停
  使用场景：测量代码执行耗时、超时计算
  注意：系统从 suspend 恢复后，时间没有包含睡眠时长

CLOCK_BOOTTIME（启动时间）：
  含义：系统启动（含 suspend）经过的总时间
  特点：suspend 期间时间继续累加
  使用场景：移动设备上的定时任务（alarm 闹钟必须用这个）
  对比 MONOTONIC：如果进程 sleep(10) 期间手机 suspend 5秒，
    MONOTONIC 经过 5秒（suspend 的 5秒不算）
    BOOTTIME 经过 10秒（更符合用户感知）

CLOCK_TAI（国际原子时）：
  含义：不受闰秒影响的连续时间
  与 REALTIME 的差：当前累计闰秒数（2024年为 37秒）
  使用场景：金融交易、电信计费（需要精确无跳变的绝对时间）
  闰秒问题：2016年闰秒时，REALTIME 在 23:59:60 跳变
    使用 REALTIME 计算的 1秒间隔可能变成 2秒或 0秒
    CLOCK_TAI 不受影响

CLOCK_PROCESS_CPUTIME_ID：
  含义：进程实际占用 CPU 的时间（不含 sleep / IO 等待）
  使用场景：性能分析、CPU 配额

CLOCK_THREAD_CPUTIME_ID：
  含义：当前线程实际占用 CPU 的时间
```

### 15.2 vDSO（虚拟动态共享对象）

`clock_gettime()` 本是系统调用，但读时间过于频繁，内核用 vDSO 优化：

```
传统系统调用路径：
  用户程序调用 clock_gettime()
  → CPU 切换到内核态（~100ns 开销）
  → 读取 timekeeper 数据
  → 返回用户态
  总耗时：~100ns

vDSO 优化：
  内核在每个进程的地址空间映射一个只读页
  该页包含：
    1. 一小段代码（clock_gettime 的实现）
    2. timekeeper 的关键数据（当前时间基准、mult/shift 参数）
  
  用户程序调用 clock_gettime()
  → glibc 检测到 vDSO 存在，直接调用 vDSO 中的代码
  → 在用户态直接读内存中的 timekeeper 数据 + 读 CNTPCT_EL0
  → 计算当前时间并返回
  → 不需要陷入内核
  总耗时：~20ns（5倍提升）

内核如何保证 vDSO 数据的一致性（seqlock）：
  timekeeper 更新时：
    seq++ （奇数，表示正在更新）
    更新 vDSO 页中的数据
    seq++ （偶数，表示更新完成）

  用户读取时：
    读 seq（必须是偶数才继续）
    读数据
    再次读 seq（必须与之前相同）
    若不同 → 读到了更新中的数据 → 重试

  这个 seqlock 在用户态纯内存操作，不需要系统调用
```

### 15.3 时间精度的实际限制

```
理论精度 vs 实际精度：

  ARM64 Generic Timer：24MHz → 分辨率 ~42ns（理论）
  hrtimer 精度：受中断响应延迟影响，实际 ~1μs

  主要误差来源：
  1. 中断延迟：
     定时器到期 → 硬件产生中断
     → CPU 从当前状态（可能在关中断的临界区）响应
     → 延迟 0~几十 μs

  2. C-state 唤醒延迟（见第十六章）：
     CPU 处于深度睡眠 → 被定时器中断唤醒
     → 需要几十到几百 μs 恢复现场
     → 定时器回调实际执行时间比预期晚

  3. 调度延迟：
     定时器回调在软中断上下文执行
     若当前 CPU 在执行高优先级任务 → 延迟更长

实时性要求场景的解决方案：
  PREEMPT_RT 补丁：把大量中断处理线程化，减少调度延迟
  CPU 隔离（isolcpus）：专用 CPU 核心，不参与普通调度
  关闭 C-state：echo 0 > /sys/devices/system/cpu/cpu*/cpuidle/state*/disable
  NO_HZ_FULL：进一步减少定时器中断对专用核心的干扰
```

---

## 十六、NO_HZ（无滴答内核）

### 16.1 传统周期性 tick 的问题

```
传统 Linux（HZ=250 时）：
  每 4ms 触发一次时钟中断（tick）
  无论 CPU 是否有任务需要运行

  问题：
  1. 空闲 CPU 每 4ms 被强制唤醒一次
     → 无法进入 C3/C6 等深度睡眠（需要几十ms才值得进入）
     → 移动设备待机功耗高

  2. 频繁唤醒破坏 CPU cache warm 状态
     → 唤醒后重新加载数据，性能损失

  3. HPC/实时场景：
     每 4ms 的中断干扰精确计算
     → 高性能计算中造成周期性抖动
```

### 16.2 NO_HZ_IDLE（空闲时停止 tick）

```
CONFIG_NO_HZ_IDLE（Linux 默认开启）：

  当 CPU 进入 idle 时：
    停止周期性 tick
    只在下一个最近的 hrtimer 到期时设置单次中断

  CPU idle 期间的时间推进：
    重新唤醒时，读取硬件计数器（CNTPCT_EL0）
    计算 idle 期间经过的时间
    批量更新 jiffies、统计信息

  效果：
    CPU idle 时可以进入更深的 C-state
    移动设备续航显著提升（减少 20-30% 待机功耗）

  代价：
    jiffies 在 idle 期间不实时更新
    唤醒时批量补偿（对大多数用途无影响）
```

### 16.3 NO_HZ_FULL（完全无滴答）

```
CONFIG_NO_HZ_FULL（需要显式配置，用于 HPC 和实时场景）：

  在 NO_HZ_IDLE 基础上更进一步：
  当 CPU 上只有一个可运行进程时，也停止 tick

  不只是 idle，连运行中的单任务 CPU 也不产生周期性中断

  配置方式：
    内核参数：nohz_full=2-7（指定完全无滴答的 CPU）
    通常 CPU 0 保留为管理核心，其余 CPU 设为 nohz_full

  需要配合：
    RCU_NOCB（RCU 回调卸载到专用线程）
    isolcpus（隔离 CPU，不参与普通调度）

  效果：
    运行单任务的 CPU 每秒中断次数从 250 次降到接近 0
    HPC 场景：消除周期性抖动，进程运行时间更稳定
    延迟从毫秒级降到微秒级

  代价：
    jiffies 精度降低（需要从其他 CPU 的 tick 推进）
    不适合需要频繁交互的通用场景
```

### 16.4 C-state 与定时器精度的关联

```
ARM CPU C-state（功耗状态）：

  C0：活跃执行（最高功耗）
  C1：时钟门控（CPU 停止取指，但 L1 Cache 保持上电）  恢复 ~1μs
  C2：浅度睡眠（部分逻辑断电）                       恢复 ~10μs
  C3：深度睡眠（更多逻辑断电，LLC 可能断电）           恢复 ~100μs
  C6/C7：最深睡眠（CPU 完全断电，状态保存到专用 SRAM） 恢复 ~500μs

定时器精度与 C-state 的矛盾：
  C-state 越深 → 省电越多 → 唤醒延迟越长
  hrtimer 设置 100μs 后到期 → CPU 在 C3 → 唤醒需要 100μs
  → 实际延迟 200μs（比预期晚 100μs = 100% 误差）

解决机制：

  1. 本地定时器断电时的 Broadcast Timer：
     进入深度 C-state 时，本地 Generic Timer 可能断电
     → 由全局广播定时器（始终上电的外部定时器）代劳
     → 到期时发 IPI（处理器间中断）唤醒目标 CPU
     代价：IPI 本身有延迟，且唤醒所有 CPU（浪费）

  2. cpuidle governor 的权衡：
     menu governor：预测 idle 时长，选择合适的 C-state
     如果预测下次唤醒在 50μs 后 → 不进入 C3（唤醒太慢）
     如果预测下次唤醒在 10ms 后  → 进入 C3（值得）

  3. 实时系统的做法：
     完全禁用 C-state（始终在 C0）
     echo 0 > /sys/devices/system/cpu/cpu0/cpuidle/state2/disable
     代价：功耗大幅增加，只适合服务器/工控场景
```

---

## 十七、时钟精度：晶振与同步

### 17.1 晶振精度与温漂

```
精度单位：ppm（parts per million，百万分之一）
  1ppm = 频率误差 1/1,000,000
  24MHz 晶振 1ppm 误差 = 24Hz 偏差
  每天时间误差 = 1ppm × 86400秒 = 0.086秒/天

常见晶振类型：

  普通晶振（XO/XTAL）：
    精度：±20~50ppm（初始精度）
    温漂：约 ±0.5ppm/°C
    0°C ~ 70°C 范围内总误差可达 ±100ppm
    每天误差：±8.6秒
    成本：几分钱
    用途：普通微控制器、低精度外设

  TCXO（温度补偿晶振）：
    精度：±0.5~2ppm（全温度范围）
    原理：内置热敏电阻网络，根据温度调整负载电容补偿频漂
    每天误差：±0.17秒
    成本：几元到几十元
    用途：手机基带、GPS 接收机、蓝牙/Wi-Fi

  VCTCXO（电压控制温补晶振）：
    在 TCXO 基础上增加电压控制输入
    外部 DAC 提供微调电压，配合 GNSS 信号实现精密同步
    精度：受外部参考源限制（< 0.1ppm）

  OCXO（恒温晶振）：
    原理：将晶振置于恒温炉中（通常 80°C），消除温度影响
    精度：±0.001~0.01ppm
    每天误差：< 1毫秒
    预热时间：5~10分钟（恒温炉需要时间稳定）
    功耗：0.5~2W（持续加热）
    成本：几百元到几千元
    用途：电信基站、测量仪器、GPS 参考源

扩频时钟（Spread Spectrum Clocking, SSC）：
  故意让 PLL 输出在中心频率 ±0.5% 范围内做三角波调制（约 30kHz）

  目的：
    把本来集中在单一频率的电磁辐射分散到更宽频带
    峰值辐射降低 10~15dB
    更容易通过 FCC Part 15 / CE EMC 认证

  实现：
    PLL 的参考时钟经过 Sigma-Delta 调制器做微小的频率抖动

  必须禁用 SSC 的接口：
    PCIe（参考时钟扩频破坏链路训练）
    USB 3.0+（发送时钟有严格的 SSC 规范，不是任意扩频）
    DDR（内存控制器和 DIMM 需要精确时序配合）
    SATA（有自己的扩频规范）
```

### 17.2 RTC（实时时钟）

```
RTC 的角色：
  独立于 SoC 主电源，由纽扣电池（CR2032）供电
  SoC 断电、重启期间继续走时
  内核启动时从 RTC 读取初始时间

RTC 的精度问题：
  通常使用廉价晶振（±20ppm）
  一个月误差：20ppm × 2592000秒 ≈ 52秒
  → 精度很差，只能提供"大概的时间"

内核启动时的 RTC 读取流程：
  start_kernel()
    → timekeeping_init()        初始化 timekeeper（时间从 0 开始）
    → rtc_hctosys()             从 RTC 硬件读取时间，写入 timekeeper
      → 此后 clock_gettime(CLOCK_REALTIME) 返回正确的年/月/日

RTC 的时区问题：
  RTC 只存储计数值，不知道时区
  存 UTC 时间（Linux 默认）：
    内核读 RTC → CLOCK_REALTIME = UTC 时间
    用户空间根据 /etc/localtime 转换为本地时间
  存本地时间（Windows 默认，双系统常见问题根源）：
    会导致 Linux 时间错误，需要 timedatectl set-local-rtc 1
```

### 17.3 NTP（网络时间协议）

```
NTP 工作原理：

  客户端 → 服务器：发送请求，记录发出时间 T1
  服务器收到请求：记录时间 T2
  服务器 → 客户端：发送响应，记录发出时间 T3
  客户端收到响应：记录时间 T4

  往返延迟：RTT = (T4 - T1) - (T3 - T2)
  单向延迟：delay = RTT / 2（假设网络对称）
  时钟偏差：offset = ((T2 - T1) + (T3 - T4)) / 2

NTP 的调整方式（关键：不跳变时间）：
  直接跳变时间的问题：
    时间突然后退 → 文件时间戳倒退 → make 认为目标文件比源文件新 → 不重新编译
    时间突然前进 → 定时任务可能被跳过或重复执行
    数据库事务 ID 混乱

  Linux 的正确做法：adjtimex() 系统调用
    调整时钟走速（frequency），不跳变时间值
    NTP 发现本地时钟偏快 → 让时钟走慢一点（频率 -0.1ppm）
    NTP 发现本地时钟偏慢 → 让时钟走快一点（频率 +0.1ppm）
    → 时间缓慢收敛，不产生跳变

  误差较大时（> 128ms，默认值）：
    ntpd 直接调用 settimeofday() 跳变（接受这一次不连续）
    然后切换回 slew 模式

精度：
  互联网 NTP：~10ms（网络延迟不对称）
  局域网 NTP：~1ms
  GPS PPS 参考源 + NTP：~1μs
```

### 17.4 PTP（IEEE 1588 精确时间协议）

```
NTP 精度受限于软件时间戳的抖动（几十到几百μs）
PTP 通过硬件时间戳消除这一误差：

硬件时间戳的原理：
  普通 NTP：
    [应用层发包] → [内核协议栈] → [网卡驱动] → [物理层发出]
    时间戳在应用层打，包含了协议栈处理时间（不确定，几十μs）

  PTP：
    时间戳在物理层（PHY）打，即报文实际离开/到达的时刻
    误差来源消除 → 时间戳精度 ~10ns

PTP 主从同步过程：
  主时钟（Grandmaster）广播 Sync 报文（带硬件时间戳 T1）
  从时钟收到后记录到达时间 T2（硬件时间戳）
  从时钟发送 Delay_Req（带时间戳 T3）
  主时钟回复 Delay_Resp（带 T4）
  从时钟计算：offset = ((T2-T1) - (T4-T3)) / 2

调整方式：
  PHC（PTP Hardware Clock）调整（硬件时钟寄存器）
  通过 phc2sys 把 PHC 同步到系统时钟（CLOCK_REALTIME）

精度：
  数据中心以太网：< 100ns
  电信网络（G.8275.1）：< 100ns
  工业以太网（TSN）：< 1μs

使用场景：
  5G 基站（需要 < 1μs 同步用于频分双工）
  金融交易所（MiFID II 要求 100μs 内时间戳精度）
  工业控制（运动控制需要微秒级多轴同步）
  分布式存储（Spanner 用 GPS+PTP 实现全球一致性事务）
```

---

## 十八、多核时钟同步

### 18.1 ARM64 Generic Timer 的跨核一致性

```
问题：SMP 系统中每个 CPU 核心有独立的本地定时器
     两个核心同时读 CNTPCT_EL0，结果是否一致？

ARM 架构规范保证：
  所有 CPU 核心读同一个物理计数器
  计数器是共享硬件，位于 DSU（或 SoC 的 Always-On 域）
  读取结果在硬件保证的误差范围内一致

实现细节（ARM Cortex-A 系列）：
  CNTPCT_EL0 读取通过总线访问共享计数器寄存器
  时钟分发网络保证所有核心的时钟沿对齐（skew < 几十ps）
  → 两个核心同时读 CNTPCT_EL0，结果相差 < 1个计数单位（< 42ns @ 24MHz）

与 x86 TSC 的对比：
  x86 早期 TSC（Time Stamp Counter）问题：
    每个 CPU 有独立的 TSC，上电时从 0 开始
    多核 TSC 不同步（相差可达几百个 cycle）
    CPU 变频时 TSC 频率跟着变 → 时间计算错误

  现代 x86 Invariant TSC（CPUID.80000007H:EDX[8]=1）：
    TSC 以固定频率运行（不随 DVFS 变化）
    多 socket 系统：BIOS 负责同步各 socket 的 TSC
    精度：通常 < 1ns

  结论：ARM64 Generic Timer 从设计上就避免了 x86 TSC 的历史问题
```

### 18.2 Linux 的 clocksource 选择机制

```
系统可能有多个可用的 clocksource：

  arch_sys_counter   rating=400   （Generic Timer，推荐）
  mmio_timer         rating=200   （某些 SoC 的 MMIO 定时器，备用）
  jiffies            rating=1     （最低精度，最后兜底）

选择规则：
  内核选择 rating 最高的 clocksource 作为当前时间源

  clocksource_select()：
    遍历所有已注册的 clocksource
    选 rating 最高且通过 watchdog 验证的

clocksource watchdog（防止硬件缺陷）：
  用参考 clocksource（通常是 HPET 或 TSC）交叉验证主 clocksource
  若两者读数偏差过大 → 标记有问题的 clocksource 为 unstable
  → 自动切换到下一个 clocksource
  → dmesg 输出警告

查看当前 clocksource：
  cat /sys/devices/system/clocksource/clocksource0/current_clocksource
  cat /sys/devices/system/clocksource/clocksource0/available_clocksource
```
