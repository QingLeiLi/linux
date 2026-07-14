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
