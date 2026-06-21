+++
date = '2026-04-16'
title = 'BBR (Bottleneck Bandwidth and RTT) 拥塞控制算法原理'
weight = 24
tags = [
    "BBR",
    "拥塞控制",
    "pacing_rate",
    "cwnd",
    "状态机",
]
categories = [
    "网络",
]
+++
# BBR (Bottleneck Bandwidth and RTT) 拥塞控制算法原理

> 基于 Linux 5.15.78 内核源码 `net/ipv4/tcp_bbr.c` 分析

**沉淀说明**：本文档与 `tcp_bbr.c` 顶部长注释、实现常量（`bbr_*`）保持一致；算法论文级推导以 ACM Queue 原文为准，实现细节以本树源码为准。

---

## 一、设计哲学

### 1.1 传统算法的问题

传统 TCP 拥塞控制（Reno、CUBIC 等）基于 **"丢包即拥塞"** 假设：

| 场景 | 问题 |
|------|------|
| 深缓冲网络 (bufferbloat) | 先填满巨大缓冲区再丢包 → 排队延迟极高（数百毫秒） |
| 浅缓冲网络 | 缓冲区小 → 频繁丢包 → 反复降速 → 带宽利用率低 |
| 有随机丢包的链路（如无线网络） | 误将随机丢包当作拥塞信号 → 不必要的降速 |

### 1.2 BBR 的核心思路

BBR 不依赖丢包，而是直接 **建模网络路径的两个物理参数**：

- **BtlBw** (Bottleneck Bandwidth)：瓶颈链路的最大吞吐能力
- **RTprop** (Round-trip propagation time)：链路的最小传播延迟（不含排队）

BBR 的目标是让发送速率精确地"骑在" BtlBw 上，同时保持 inflight 数据量等于 BDP（管道恰好满、队列恰好空），实现 **最大吞吐 + 最低延迟** 的理想工作点。

---

## 二、核心算法公式

### 2.1 网络模型参数估计

#### BtlBw（瓶颈带宽）

```
BtlBw = windowed_max(delivery_rate, 过去 10 个 RTT)
```

- **delivery_rate** = ACK 确认的数据量 / 投递间隔时间
- 使用滑动窗口 **最大值** 滤波器（`win_minmax`）
- **为什么取最大值**：真实带宽是投递速率的上界，各种噪声（排队延迟、ACK 聚合、调度抖动）只会使测量偏低，不会偏高。取最大值过滤向下噪声。
- **窗口长度 = 10 RTT**：覆盖完整的 PROBE_BW gain cycle (8 RTT) + 2 RTT 余量

#### RTprop（传播延迟）

```
RTprop = windowed_min(rtt_sample, 过去 10 秒)
```

- RTT = 传播延迟 + **排队延迟** + 处理延迟
- 使用滑动窗口 **最小值** 滤波器
- **为什么取最小值**：真实传播延迟是 RTT 的下界，排队只会抬高不会降低。取最小值过滤向上噪声。
- **窗口长度 = 10 秒**：适应路由变化等网络条件变化。到期后强制进入 PROBE_RTT 重新测量。

### 2.2 BDP（带宽-延迟积）

```
BDP = BtlBw × RTprop
```

**物理含义**：网络管道中可以容纳的在途数据量（单位：数据包）。这是实现"管道满、队列空"理想状态的精确 inflight 数据量。

### 2.3 发送速率控制（Pacing Rate）

```
pacing_rate = pacing_gain × BtlBw × (1 - 1%)
```

| 组成部分 | 含义 | 源码位置 |
|---------|------|---------|
| `pacing_gain` | 增益因子，不同状态取不同值 | `bbr_update_gains()` |
| `BtlBw` | 瓶颈带宽估计 | `bbr_bw()` |
| `(1 - 1%)` | 边际系数，略低于带宽 | `bbr_pacing_margin_percent` |

**1% 边际的设计原理**：

如果 pacing_rate 精确等于 BtlBw，任何微小的测量误差（哪怕 0.1% 的高估）都会导致持续的队列积累。乘以 0.99 使平均注入速率略低于实际带宽：

- 带宽高估时：0.99 × 高估值 ≈ 真实值，不会积累队列
- 带宽准确时：0.99 × 真实值，管道 99% 利用率，队列为空
- 带宽低估时：本身就不会积累队列

这 1% 的吞吐量代价几乎不可感知，但显著降低了稳态队列占用和延迟。

### 2.4 拥塞窗口控制（cwnd）

```
target_cwnd = cwnd_gain × BDP + extra_acked + quantization_budget
cwnd = max(target_cwnd, 4)
```

| 组成部分 | 含义 | 作用 |
|---------|------|------|
| `cwnd_gain × BDP` | 基础 inflight 上限 | cwnd_gain=2 提供 2 倍 BDP 余量 |
| `extra_acked` | ACK 聚合额外余量 | 补偿 ACK 聚合间隙 |
| `quantization_budget` | TSO/GRO 处理余量 | 3×tso_segs + 取偶 + Phase0 补偿 |
| `min = 4` | 最小 cwnd | 保证 delayed-ACK 下管道畅通 |

**cwnd 不含 1% 边际的原因**：pacing_rate 和 cwnd 职责不同：
- pacing_rate 控制数据 **注入速率**（水龙头流速）
- cwnd 控制在途数据 **上限**（水池容量上限）

cwnd 需要宽松的天花板来容忍 ACK 延迟和聚合波动，而不是精确限制。

---

## 三、状态机运行原理

```
            |
            V
   +---> STARTUP  ----+
   |        |         |
   |        V         |
   |      DRAIN   ----+
   |        |         |
   |        V         |
   +---> PROBE_BW ----+
   |      ^    |      |
   |      |    |      |
   |      +----+      |
   |                  |
   +---- PROBE_RTT <--+
```

### 3.1 STARTUP — 快速探测管道容量

| 参数 | 值 | 说明 |
|------|---|------|
| pacing_gain | 2/ln(2) ≈ 2.885 | 每 RTT 发送速率翻倍 |
| cwnd_gain | 2/ln(2) ≈ 2.885 | 匹配 pacing 增长 |

**high_gain = 2/ln(2) 的数学推导**：

在 paced 发送模式下，设第 n 轮的发送量为 `S(n)`，增益为 `G`：

```
S(n) = G × BtlBw_est(n-1) × RTT
```

因为 BtlBw 估计基于上一轮的投递速率，`BtlBw_est(n) ≈ S(n-1) / RTT`，所以：

```
S(n) = G × S(n-1)
```

要使每轮总发送量翻倍（匹配传统慢启动），需要：

```
S(1) + S(2) + ... + S(n) = 2 × (S(1) + S(2) + ... + S(n-1))
```

即 `S(n) = S(1) + S(2) + ... + S(n-1)`，对等比数列 `G^(n-1)` 求解得 **G = 2/ln(2) ≈ 2.885**。

**退出条件**：连续 3 轮 BtlBw 增长 < 25% → 判定管道已满

为什么要 3 轮：
1. 第 1 轮：接收端 rwin 自动调优可能正在扩大
2. 第 2 轮：用更大 rwin 填充管道
3. 第 3 轮：获得更高的投递速率样本

### 3.2 DRAIN — 排空 STARTUP 建立的队列

| 参数 | 值 | 说明 |
|------|---|------|
| pacing_gain | 1/2.885 ≈ 0.347 | STARTUP 增益的倒数 |
| cwnd_gain | 2.885 | 保持高 cwnd，不触发慢启动 |

**排空原理**：

STARTUP 结束时 inflight ≈ 2.885 × BDP，队列积累 = (2.885 - 1) × BDP ≈ 1.885 × BDP。

```
发送速率 = 0.347 × BtlBw
ACK 返回速率 = BtlBw
净排出速率 = BtlBw × (1 - 0.347) = 0.653 × BtlBw
排空时间 ≈ 1.885 × BDP / (0.653 × BtlBw) ≈ 2.9 × RTprop
```

选择 drain_gain = 1/high_gain 使 STARTUP 多注入和 DRAIN 排出的数据量精确对称。

**DRAIN 期间 cwnd_gain 仍为 2.885 的原因**：如果同时降低 cwnd，cwnd 会小于当前 inflight，TCP 会停止发送直到 inflight 降至 cwnd 以下——相当于突然断流而非渐进排空。保持高 cwnd，让 pacing_rate 自然控制排空节奏。

**退出条件**：inflight ≤ BDP

### 3.3 PROBE_BW — 稳态运行与带宽探测

| 参数 | 值 | 说明 |
|------|---|------|
| pacing_gain 循环 | [5/4, 3/4, 1, 1, 1, 1, 1, 1] | 8 phase 循环 |
| cwnd_gain | 2.0 | 容忍 ACK 波动 |
| 每 phase 持续时间 | ≈ 1 × RTprop | 按 min_rtt 计时 |

**Gain Cycling 详解**：

```
Phase 0 (gain=1.25): 探测 — 以高于 BtlBw 25% 的速率发送
  ├── 如果有新带宽 → BtlBw 滤波器捕获更高样本
  ├── 多注入 0.25 × BDP 数据到队列
  └── 退出条件：时间 ≥ RTprop 且 (丢包 或 inflight ≥ 1.25×BDP)

Phase 1 (gain=0.75): 排空 — 以低于 BtlBw 25% 的速率发送
  ├── 排出 Phase 0 多注入的 0.25 × BDP 数据
  ├── 数学平衡：多注入 = (1.25-1)×BDP = 少注入 = (1-0.75)×BDP = 0.25×BDP
  └── 退出条件：时间 ≥ RTprop 或 inflight ≤ BDP

Phase 2-7 (gain=1.0): 巡航 — 以 BtlBw 速率匀速发送
  ├── 管道满、队列空，高吞吐低延迟
  └── 退出条件：时间 ≥ RTprop
```

**公平性**：多条 BBR 流共享瓶颈时，各流的 gain cycling 异步进行（起始 phase 随机化），短时间交替试探，长时间收敛到公平份额。

### 3.4 PROBE_RTT — 协作测量传播延迟

| 参数 | 值 | 说明 |
|------|---|------|
| pacing_gain | 1.0 | 正常速率 |
| cwnd | 4 包 (强制) | 最小值 |
| 持续时间 | max(200ms, 1 RTT) | 在 inflight ≤ 4 后计时 |
| 触发条件 | min_rtt 10 秒未更新 | 窗口到期 |

**运行流程**：

```
10 秒 min_rtt 窗口到期
  → 保存当前 cwnd
  → 压缩 cwnd 到 4 包
  → 等待 inflight 降到 ≤ 4
  → 启动 200ms 计时器
  → 200ms + 1 RTT 后退出
  → 恢复 cwnd，进入 PROBE_BW 或 STARTUP
```

**性能开销**：200ms / 10s = 2% 吞吐量损失

**协作效果**：所有 BBR 流都会周期性进入 PROBE_RTT，共同排空瓶颈队列，使每个流都能测到准确的 RTprop。

---

## 四、参数变化一览表

| 参数 | STARTUP | DRAIN | PROBE_BW | PROBE_RTT |
|------|---------|-------|----------|-----------|
| pacing_gain | 2.885 | 0.347 | 循环 [5/4, 3/4, 1×6] | 1.0 |
| cwnd_gain | 2.885 | 2.885 | 2.0 | 1.0 |
| inflight 目标 | 递增至饱和 | 降至 BDP | ≈ BDP ± 25% | 4 pkts |
| BtlBw 更新 | 是 | 是 | 是 | 否 (app_limited) |
| RTprop 更新 | 机会性 | 机会性 | 机会性 | 强制更新 |
| 典型持续时间 | ~6 RTT | ~3 RTT | 大部分时间 | 200ms+1RTT |

---

## 五、`bbr_main` 每 ACK 完整执行流程

`bbr_main()` 是 BBR 的核心入口，TCP 栈在每个 ACK 到达时调用。它分为两大阶段：**更新模型**（感知网络）和**设置控制输出**（控制发送）。下面按代码调用顺序逐层展开。

### 5.0 总览：调用树

```
bbr_main(sk, rs)                    ← TCP栈每ACK调用一次
│
├── bbr_update_model(sk, rs)        ← 第1步：感知网络，更新内部模型
│   │
│   ├── bbr_update_bw()             ← ① 带宽采样：轮次检测 + LT限速器 + BtlBw滤波
│   │   ├── 轮次(round)检测          delivered计数切分RTT轮次
│   │   ├── bbr_lt_bw_sampling()     检测token-bucket限速器
│   │   └── minmax_running_max()     滑动窗口最大值更新BtlBw
│   │
│   ├── bbr_update_ack_aggregation() ← ② ACK聚合度追踪
│   │   └── 双缓冲滑动窗口记录extra_acked最大值
│   │
│   ├── bbr_update_cycle_phase()    ← ③ PROBE_BW增益循环推进
│   │   ├── bbr_is_next_cycle_phase()  phase切换条件判定
│   │   └── bbr_advance_cycle_phase()  推进到下一个phase
│   │
│   ├── bbr_check_full_bw_reached() ← ④ STARTUP管道填满检测
│   │   └── 连续3轮BtlBw增长<25% → full_bw_reached=1
│   │
│   ├── bbr_check_drain()           ← ⑤ STARTUP→DRAIN→PROBE_BW转换
│   │   ├── full_bw_reached → 切到DRAIN
│   │   └── inflight≤BDP → 切到PROBE_BW
│   │
│   ├── bbr_update_min_rtt()        ← ⑥ RTprop维护 + PROBE_RTT状态机
│   │   ├── 更新min_rtt_us(取最小/到期强制替换)
│   │   ├── 10s到期 → 进入PROBE_RTT
│   │   └── PROBE_RTT内部：等排空→计时→退出
│   │
│   └── bbr_update_gains()          ← ⑦ 根据当前mode设置pacing_gain/cwnd_gain
│
├── bbr_bw(sk)                      ← 获取有效带宽(BtlBw或lt_bw)
│
├── bbr_set_pacing_rate(sk,bw,gain) ← 第2步输出: pacing_rate = gain × bw × 0.99
│   └── bbr_bw_to_pacing_rate()      定点转bytes/sec，含1%边际
│
└── bbr_set_cwnd(sk,rs,acked,bw,gain) ← 第3步输出: cwnd
    ├── bbr_set_cwnd_to_recover_or_restore()  丢包恢复路径
    ├── bbr_bdp()                     BDP = ceil(bw × min_rtt × gain)
    ├── bbr_ack_aggregation_cwnd()    + ACK聚合余量
    ├── bbr_quantization_budget()     + TSO/GRO量化余量
    └── PROBE_RTT时强制cwnd=4
```

### 5.1 第一步：`bbr_update_model()` — 感知网络

`bbr_update_model()` 按固定顺序调用 7 个子函数来更新 BBR 对网络的认知。**顺序不可打乱**——后面的函数依赖前面的结果（例如 `bbr_check_drain()` 需要 `bbr_check_full_bw_reached()` 设置的 `full_bw_reached` 标志）。

#### ① `bbr_update_bw()` — 带宽采样与 BtlBw 更新

这是模型更新的第一步，也是 BBR 最核心的函数。每个 ACK 都调用。它做三件事：

**A. 轮次(round)检测** — 用 delivered 计数切分 RTT 轮次

```
条件：!before(rs->prior_delivered, bbr->next_rtt_delivered)
含义：本ACK的参考包是在"当前轮次门槛之后"发出的 → 一个RTT已过

触发时：
  next_rtt_delivered = tp->delivered  （画新门槛线）
  rtt_cnt++                           （全局轮次计数器+1）
  round_start = 1                     （通知其他子函数"新轮次到了"）
  packet_conservation = 0             （结束丢包恢复的包守恒）
```

**为什么用 delivered 计数而不用挂钟？** RTT 会随队列深度变化，挂钟切分的"一轮"可能不对齐实际的包往返。delivered 计数与包的实际到达严格绑定，天然适应 RTT 的波动。

**B. LT 限速器采样** — `bbr_lt_bw_sampling()`

检测链路上是否有 token-bucket 限速器（详见§六）。如果检测到，`bbr_bw()` 会返回 `lt_bw` 替代 `bbr_max_bw()`。

**C. BtlBw 滤波器更新**

```c
bw = rs->delivered * BW_UNIT / rs->interval_us   // 本次投递速率
if (!rs->is_app_limited || bw >= bbr_max_bw(sk))  // 过滤app_limited样本
    minmax_running_max(&bbr->bw, 10, rtt_cnt, bw) // 10轮窗口取最大
```

- **为什么取 max**：瓶颈带宽是投递速率的物理上限。排队、ACK 聚合、调度延迟只会让单次样本偏低，不会系统性偏高。取 max 过滤向下噪声。
- **为什么跳过 app_limited**：应用没喂满数据时投递速率偏低，写入 max 滤波器没意义——但如果 bw 仍然 ≥ 当前 BtlBw，说明路径可能变好了，例外纳入。
- **滑动窗口 10 轮**：覆盖 8 phase 的 gain cycle + 2 轮余量，确保即使在 gain=0.75 的低速 phase 也不会把高峰样本过早淘汰。

#### ② `bbr_update_ack_aggregation()` — ACK 聚合度追踪

网络设备（LRO/GRO）和 delayed-ACK 会把多个 ACK 合并成一个大 ACK。在合并间隙中，发送端收不到 ACK → inflight 不减 → cwnd 被"卡住"。如果 cwnd 没有足够余量，这会导致发送端被迫停发。

```
epoch_us = 当前时间 - 纪元起点
expected_acked = BtlBw × epoch_us          // 按带宽应该确认多少
extra_acked = actual_acked - expected_acked // 超出预期的部分 = ACK聚合量
```

使用**双缓冲**实现近似滑动窗口（每 5 RTT 轮换），追踪 `extra_acked` 最大值。这个值后续加到 `target_cwnd` 上，为 ACK 聚合间隙预留 cwnd 余量。

#### ③ `bbr_update_cycle_phase()` — PROBE_BW 增益循环推进

仅在 `mode == BBR_PROBE_BW` 时生效。调用 `bbr_is_next_cycle_phase()` 判断是否切换到下一个 gain phase。

**各 phase 的切换条件不同**：

| Phase | Gain | 切换条件 |
|-------|------|---------|
| Phase 0 | 5/4 | 时间 ≥ RTprop **且** (丢包 或 inflight ≥ 1.25×BDP) |
| Phase 1 | 3/4 | 时间 ≥ RTprop **或** inflight ≤ BDP |
| Phase 2-7 | 1.0 | 时间 ≥ RTprop |

设计逻辑：
- Phase 0（探测）：必须等到数据真正"堆"到队列里（inflight 够大）或出现丢包，才能确认探测完成。时间+inflight 双条件保证探测有效。
- Phase 1（排空）：排空越快越好，要么时间到了，要么 inflight 已降到 BDP，任一满足即停。
- Phase 2-7（巡航）：纯时间驱动，每 RTT 推进一步。

**重要细节**：Phase 0 切换时使用的 inflight 经过 EDT 修正 (`bbr_packets_in_net_at_edt`)——fq 队列规则下，本地排队等待 pacing 的包尚未进入网络，不应算作"在途"。

#### ④ `bbr_check_full_bw_reached()` — STARTUP 管道填满检测

仅在 STARTUP 期间、每个新轮次起点、非 app_limited 时执行：

```
bw_thresh = full_bw × 1.25         // 上次记录的带宽 × 125%
if (bbr_max_bw >= bw_thresh)
    full_bw = bbr_max_bw            // 还在增长，更新高水位
    full_bw_cnt = 0                 // 重置停滞计数
else
    full_bw_cnt++                   // 增长不足25%，停滞+1
    if (full_bw_cnt >= 3)
        full_bw_reached = true      // 连续3轮停滞 → 管道满了
```

**为什么是 25% 和 3 轮？**
- 25%：低于此增幅说明带宽接近饱和（噪声级别的增长）
- 3 轮：排除接收端 rwin 自动调优的干扰——rwin 通常需要 1-2 轮才稳定，第 3 轮的样本才可靠

#### ⑤ `bbr_check_drain()` — STARTUP → DRAIN → PROBE_BW 转换

两步状态转换：

```
if (mode == STARTUP && full_bw_reached)
    mode = DRAIN                    // 管道满了，开始排空
    snd_ssthresh = BDP              // 设置统计值（BBR自身不用）

if (mode == DRAIN && inflight ≤ BDP)
    bbr_reset_probe_bw_mode()       // 排空完成，进入稳态
```

**为什么 DRAIN → PROBE_BW 用 inflight ≤ BDP？** BDP 就是"管道恰好满、队列恰好空"的 inflight 数据量。inflight 降到 BDP 以下说明 STARTUP 建立的队列已排完。

#### ⑥ `bbr_update_min_rtt()` — RTprop 维护 + PROBE_RTT 状态机

**A. 更新 min_rtt_us**

```
if (rtt_us < min_rtt_us)                        // 更小 → 写入
    min_rtt_us = rtt_us
elif (10s到期 && !is_ack_delayed)                // 到期 → 强制刷新
    min_rtt_us = rtt_us                          // 允许变大，跟上网络变化
min_rtt_stamp = now                              // 重置10s计时器
```

为什么排除 `is_ack_delayed`：delayed ACK 会人为增加 RTT（接收端攒 40ms 再回 ACK），用它刷新 min_rtt 会污染 RTprop 估计。

**B. 进入 PROBE_RTT**

```
if (10s到期 && !idle_restart && mode != PROBE_RTT)
    mode = PROBE_RTT
    保存cwnd
```

为什么 `idle_restart` 时不进入：刚从空闲恢复时管道本来就空，不需要再压 cwnd 排队。

**C. PROBE_RTT 内部两步退出**

```
步骤A：inflight 降到 ≤ 4 包
  → 启动200ms计时器
  → 清零round_done
  → 对齐next_rtt_delivered

步骤B：200ms到期 + round_start（至少经过一个RTT）
  → 恢复cwnd
  → 重置min_rtt_stamp
  → 切回PROBE_BW或STARTUP
```

为什么需要等一个 round_start：200ms 是挂钟时间，但必须确保"cwnd=4 的包确实在管道里走了一个完整来回"才能量到排空后的真实 RTT。

**D. 标记 app_limited**

PROBE_RTT 期间故意只发 4 包，`tp->app_limited` 被置位，防止 `bbr_update_bw()` 把低速样本写入 BtlBw 的 max 滤波器。

#### ⑦ `bbr_update_gains()` — 设置增益因子

根据当前 mode 设置 `pacing_gain` 和 `cwnd_gain`：

| Mode | pacing_gain | cwnd_gain | 说明 |
|------|------------|-----------|------|
| STARTUP | 2.885 | 2.885 | 指数探测 |
| DRAIN | 0.347 | 2.885 | 低速排空，高cwnd防停发 |
| PROBE_BW | gain_cycle[idx] 或 1.0(限速时) | 2.0 | 稳态循环 |
| PROBE_RTT | 1.0 | 1.0 | 正常速率，cwnd在bbr_set_cwnd中压到4 |

**PROBE_BW 被限速时**：如果 `lt_use_bw=1`（检测到 token-bucket 限速器），`pacing_gain` 强制为 1.0，不做 gain cycling——被限速时 gain>1 只会超速丢包。

### 5.2 中间步骤：`bbr_bw()` — 获取有效带宽

```c
return bbr->lt_use_bw ? bbr->lt_bw : bbr_max_bw(sk);
```

如果检测到限速器，使用 LT 采样得到的长期平均带宽；否则使用 BtlBw 的 max 滤波器输出。

### 5.3 第二步：`bbr_set_pacing_rate()` — 设置发送速率

```
rate = bbr_bw_to_pacing_rate(bw, gain)
     = bw × gain × mss × (100-1%) / 100     // 含1%边际
     → 转为 bytes/sec
     → min(rate, sk_max_pacing_rate)          // 受用户上限约束
```

**STARTUP 期间的特殊策略**：`full_bw_reached=0` 时只允许速率上升，不下降。因为 STARTUP 在持续探测更高带宽，瞬时波动不应导致降速。

**首次 RTT 初始化**：连接初期还没有投递速率样本时，从 SRTT 估算初始 pacing_rate = `high_gain × init_cwnd / srtt`。

### 5.4 第三步：`bbr_set_cwnd()` — 设置拥塞窗口

这是 BBR 的最后一步输出，分为两条路径：

**路径 A：丢包恢复路径** — `bbr_set_cwnd_to_recover_or_restore()`

```
每个ACK：cwnd -= losses（先扣丢失的包）

刚进入Recovery（第1轮）：
  packet_conservation = 1
  cwnd = inflight + acked             // 裁剪到实际在途量
  → 包守恒：每收P个ACK才允许发P个新包

退出Recovery：
  cwnd = max(cwnd, prior_cwnd)        // 恢复进入前的cwnd
  packet_conservation = 0
```

**路径 B：正常路径** — 三步计算 target_cwnd

```
target_cwnd  = bbr_bdp(bw, cwnd_gain)          // ①基础：gain × BDP
target_cwnd += bbr_ack_aggregation_cwnd()       // ②加：ACK聚合余量
target_cwnd  = bbr_quantization_budget(target)  // ③加：TSO/GRO量化余量
```

**① `bbr_bdp()` — BDP 计算**

```
w = bw × min_rtt_us                    // pkts × 2^24（usec相消）
bdp = ceil(w × gain / BBR_UNIT / BW_UNIT)
```

向上取整的原因：宁可多 0.x 个包（微量排队），也不少 0.x 个包（浪费带宽）。管道必须填满。

**② `bbr_ack_aggregation_cwnd()` — ACK 聚合余量**

```
extra = max(extra_acked[0], extra_acked[1])  // 双缓冲取较大值
return extra_acked_gain × extra              // gain=1，即直接加上
```

**③ `bbr_quantization_budget()` — 量化余量**

```
cwnd += 3 × tso_segs_goal    // Qdisc、TSO引擎、接收端各需1段缓冲
cwnd = 向上取偶数             // 减少delayed-ACK对奇数cwnd的不利影响
if (PROBE_BW Phase 0)
    cwnd += 2                 // 确保小BDP时 gain cycling 也能推动inflight > BDP
```

**cwnd 更新策略**：

```
if (full_bw_reached)
    cwnd = min(cwnd + acked, target_cwnd)   // 受target封顶
else (STARTUP)
    cwnd += acked                           // 不封顶，指数增长
cwnd = max(cwnd, 4)                         // 最小4包
cwnd = min(cwnd, snd_cwnd_clamp)            // 全局上限

if (PROBE_RTT)
    cwnd = min(cwnd, 4)                     // 强制压到4包
```

---

## 六、辅助机制

### 6.1 Token-Bucket 限速器检测（LT 采样）

互联网中广泛部署 token-bucket 流量限速器。限速器允许短期突发（token 充满），但长期限制速率。BBR 的 BtlBw 估计会被突发阶段的高速率"污染"。

**三阶段状态机**：

```
┌──────────────┐         ┌──────────────┐         ┌──────────────┐
│   空闲等待    │  丢包→  │   采样测速    │  确认→  │  使用限速值   │
│ (等token耗尽) │ ─────→ │ (量4~16 RTT)  │ ─────→ │ (跑48 RTT)   │
└──────────────┘         └──────────────┘         └──────┬───────┘
       ↑                        │ 失败                     │ 到期
       └────────────────────────┘                         │
       ↑──────────────────────────────────────────────────┘
```

**检测算法**（`bbr_lt_bw_sampling` + `bbr_lt_bw_interval_done`）：

```
1. 空闲等待(lt_is_sampling=0)：等首次丢包（token耗尽信号）
2. 开始采样(lt_is_sampling=1)：记录起点快照
3. 累计 4-16 RTT 后，等再次丢包结束区间
4. 检查 丢包率 = lost / delivered
5. 如果丢包率 ≥ 20% → 高丢包区间 → 计算平均投递速率
6. bbr_lt_bw_interval_done()：
   ├── 首次(lt_bw=0)：记住bw，等下一区间
   └── 有上一区间(lt_bw≠0)：比较两区间
       ├── 相近(差异≤12.5%或≤4Kbps) → lt_bw取平均，lt_use_bw=1
       └── 不相近 → 覆盖lt_bw，重来
7. 使用中(lt_use_bw=1)：pacing用lt_bw替代BtlBw，48 RTT后重置
```

**关键设计选择**：
- 用"丢包到丢包"而非固定时间做区间——对齐 token-bucket 的自然消耗周期
- 双区间确认——避免偶发拥塞误判
- 相对阈值(12.5%) + 绝对阈值(4Kbps)——高速率和低速率都能正确判定

### 6.2 ACK 聚合补偿

网络设备 (LRO/GRO) 和 delayed-ACK 可能导致 ACK 聚合到达。在 ACK 间隙期间，如果 cwnd 不够大，发送端被迫停发。

```
expected_acked = BtlBw × epoch_duration
extra_acked = actual_acked - expected_acked
target_cwnd += max(extra_acked, 过去 5-10 RTT)
```

使用双缓冲实现近似滑动窗口，每 5 RTT 轮换一次。

### 6.3 丢包恢复（Packet Conservation）

BBR 不因丢包大幅砍 cwnd，而是使用温和的包守恒原则：

| 恢复阶段 | 策略 | 效果 |
|---------|------|------|
| 第 1 轮 | cwnd = max(cwnd, inflight + acked) | 严格一进一出 |
| 后续轮次 | cwnd += acked (受 target_cwnd 封顶) | 允许慢启动增长 |
| 退出恢复 | cwnd = max(cwnd, prior_cwnd) | 恢复之前保存的 cwnd |

---

## 七、完整的一次 ACK 处理时序（例子）

以一个处于 **PROBE_BW Phase 2 (gain=1.0)** 稳态运行的连接为例，一个 ACK 到达时的完整执行流程：

```
ACK到达，TCP栈调用 bbr_main(sk, rs)
│
├── bbr_update_model(sk, rs)
│   │
│   ├── bbr_update_bw()
│   │   ├── round_start=0（默认非轮次起点）
│   │   ├── rs->delivered=5, interval_us=10000 → 有效样本
│   │   ├── prior_delivered=8000, next_rtt_delivered=7800
│   │   │   !before(8000,7800)=true → 新轮次！
│   │   │   next_rtt_delivered=8005, rtt_cnt++, round_start=1
│   │   ├── bbr_lt_bw_sampling() → lt_use_bw=0, 无丢包 → 直接return
│   │   ├── bw = 5 × 2^24 / 10000 = 8388 (pkts/usec定点)
│   │   └── !app_limited → minmax_running_max(bw=8388, window=10, t=rtt_cnt)
│   │
│   ├── bbr_update_ack_aggregation()
│   │   ├── round_start=1 → extra_acked_win_rtts++
│   │   ├── expected_acked=7000, actual=7500 → extra=500
│   │   └── extra_acked[idx] = max(extra_acked[idx], 500)
│   │
│   ├── bbr_update_cycle_phase()
│   │   ├── mode=PROBE_BW → 检查是否切phase
│   │   ├── gain=1.0(巡航) → 纯时间驱动
│   │   └── 时间 > min_rtt → 推进到Phase 3
│   │
│   ├── bbr_check_full_bw_reached()
│   │   └── full_bw_reached=1 → 跳过（已经过了STARTUP）
│   │
│   ├── bbr_check_drain()
│   │   └── mode=PROBE_BW → 跳过（不是STARTUP也不是DRAIN）
│   │
│   ├── bbr_update_min_rtt()
│   │   ├── rtt_us=20100, min_rtt_us=20000 → 不更小，不更新
│   │   ├── filter_expired=false（还没到10s）→ 不进PROBE_RTT
│   │   └── delivered>0 → idle_restart=0
│   │
│   └── bbr_update_gains()
│       ├── mode=PROBE_BW
│       ├── pacing_gain = gain_cycle[3] = BBR_UNIT (1.0)
│       └── cwnd_gain = 2.0
│
├── bw = bbr_bw() = bbr_max_bw() = 8500 (滤波器当前最大值)
│
├── bbr_set_pacing_rate(bw=8500, gain=1.0)
│   ├── rate = 8500 × 1.0 × mss × 0.99 → 转bytes/sec
│   ├── full_bw_reached=1 → 允许上下调整
│   └── sk_pacing_rate = rate
│
└── bbr_set_cwnd(acked=5, bw=8500, gain=2.0)
    ├── bbr_set_cwnd_to_recover_or_restore() → 不在Recovery → false
    ├── target = bbr_bdp(8500, 2.0)
    │         = ceil(8500 × 20000 × 2.0 / 2^24 / 2^8) = 20 pkts
    ├── target += extra_acked = 20 + 2 = 22
    ├── target = quantization_budget(22) → 22 + 3×2 + 取偶 = 28
    ├── cwnd = min(cwnd+5, 28) = min(27, 28) = 27
    ├── cwnd = max(27, 4) = 27
    └── tcp_snd_cwnd_set(27)
```

---

## 八、关键设计选择总结

| 设计选择 | 原因 |
|---------|------|
| 基于带宽而非丢包 | 避免 bufferbloat 和浅缓冲问题 |
| BtlBw 取最大值 | 噪声只向下偏，取 max 过滤 |
| RTprop 取最小值 | 排队只向上偏，取 min 过滤 |
| pacing_rate 含 1% 边际 | 防止带宽高估导致队列持续积累 |
| cwnd 不含 1% 边际 | cwnd 是上限，需要宽松余量 |
| BDP 向上取整 | 宁可多0.x包排队，不让管道欠载 |
| high_gain = 2/ln(2) | 使 paced 慢启动与传统慢启动增长速度匹配 |
| drain_gain = 1/high_gain | 排空数据量与 STARTUP 多注入量对称 |
| DRAIN cwnd_gain 不降 | 防止突然停发，用 pacing 渐进排空 |
| 8 phase gain cycle | 探测+排空+6轮巡航，带宽均衡 |
| 随机起始 phase | 避免多流同步震荡 |
| Phase 0 时间+inflight双条件 | 保证探测有效性 |
| Phase 1 时间或inflight任一 | 排空越快越好 |
| 10 RTT BtlBw 窗口 | 覆盖完整 gain cycle + 余量 |
| 10 秒 RTprop 窗口 | 适应路由变化 |
| PROBE_RTT 200ms/10s | 2% 开销换取准确的 RTprop |
| PROBE_RTT 排除 delayed ACK | 防止人为高 RTT 污染 RTprop |
| PROBE_RTT 等一个 round_start | 确保排空后的包走完一个来回 |
| PROBE_RTT 标记 app_limited | 保护 BtlBw 不被低速样本拉低 |
| cwnd 最小 4 包 | 保证 delayed-ACK 下持续发送 |
| LT 双区间确认 | 避免偶发拥塞误判为限速器 |
| 包守恒(packet conservation) | 丢包恢复期间不向拥塞网络注入额外负载 |

---

## 九、与 Linux TCP 栈的衔接（本树 5.15.78）

### 9.1 拥塞控制入口：每个 ACK 一次

ACK 处理路径在生成 `rate_sample` 后调用 `tcp_cong_control()`。若当前算法实现了 `cong_control`（BBR 如此），则**只调用该回调**，不再走传统的 `tcp_cong_avoid()` / `tcp_cwnd_reduction()`，也**不会**在该路径末尾执行 `tcp_update_pacing_rate()`（BBR 自己在 `bbr_main` 里设置 `sk_pacing_rate`）。

```4228:4246:net/ipv4/tcp_input.c
static void tcp_cong_control(struct sock *sk, u32 ack, u32 acked_sacked,
			     int flag, const struct rate_sample *rs)
{
	const struct inet_connection_sock *icsk = inet_csk(sk);

	if (icsk->icsk_ca_ops->cong_control) {
		icsk->icsk_ca_ops->cong_control(sk, rs);
		return;
	}

	if (tcp_in_cwnd_reduction(sk)) {
		/* Reduce cwnd if state mandates */
		tcp_cwnd_reduction(sk, acked_sacked, rs->losses, flag);
	} else if (tcp_may_raise_cwnd(sk, flag)) {
		/* Advance cwnd if state allows */
		tcp_cong_avoid(sk, ack, acked_sacked);
	}
	tcp_update_pacing_rate(sk);
}
```

BBR 通过 `tcp_congestion_ops` 注册 `.cong_control = bbr_main`（见下文 §十一）。

### 9.2 发送侧：pacing_rate 与 cwnd 如何"生效"

拥塞算法更新 `snd_cwnd` 与 `sk_pacing_rate` 后，**是否发得出去**仍由发送引擎综合判断：`tcp_write_xmit()` 中依次受 **pacing**、**拥塞窗口配额**、**对端接收窗口**、Nagle/TSO 推迟、small-queue 等约束。

```3435:3457:net/ipv4/tcp_output.c
		/* TCP pacing检查：控制发送速率，避免突发流量 */
		if (tcp_pacing_check(sk))
			break;  /* 发送速率受限，暂停发送 */

		/* 初始化TSO段数 */
		tso_segs = tcp_init_tso_segs(skb, mss_now);
		BUG_ON(!tso_segs);

		/* 拥塞窗口测试：检查是否有足够的拥塞窗口配额 */
		cwnd_quota = tcp_cwnd_test(tp, skb);
		if (!cwnd_quota) {
			if (push_one == 2)
				/* 强制发送loss probe包（用于快速丢包检测） */
				cwnd_quota = 1;
			else
				break;  /* 拥塞窗口已满，停止发送 */
		}

		/* 接收窗口测试：检查对端是否有足够的接收空间 */
		if (unlikely(!tcp_snd_wnd_test(tp, skb, mss_now))) {
			is_rwnd_limited = true;  /* 接收窗口限制 */
			break;
		}
```

**要点**：BBR 侧 `pacing_rate` 管注入节奏，`cwnd` 管在途上限；栈侧还有 **rwnd、应用 sndbuf** 等外层限制。

### 9.3 部署提示（与源码注释一致）

`tcp_bbr.c` 文件头建议：**尽量配合 fq qdisc**（`man tc-fq`），以便由队列规则做 pacing；否则栈内为每连接软件 pacing（定时器），CPU 开销更高。见 `net/ipv4/tcp_bbr.c` 顶部注释（约 L178–180）。

---

## 十、内核关键常量速查（`tcp_bbr.c`）

以下符号均为 `static const`，行号对应本仓库 **5.15.78** 版本，便于对照与升级内核时 diff。

| 符号 | 含义 | 典型值 / 说明 |
|------|------|----------------|
| `CYCLE_LEN` | PROBE_BW 下 pacing_gain 循环长度 | 8（L291） |
| `bbr_bw_rtts` | BtlBw 滤波窗口（RTT 轮数） | `CYCLE_LEN + 2` = 10（L303） |
| `bbr_min_rtt_win_sec` | RTprop / min_rtt 过期判据 | 10 秒 → 触发 PROBE_RTT（L308） |
| `bbr_probe_rtt_mode_ms` | PROBE_RTT 最短持续时间 | 200 ms（L313） |
| `bbr_pacing_margin_percent` | pacing 相对 BtlBw 的下修比例 | 1%（L324） |
| `bbr_high_gain` | STARTUP pacing/cwnd 增益 | ≈ 2/ln(2)（定点，L341） |
| `bbr_drain_gain` | DRAIN pacing 增益 | ≈ 1/high_gain（L355） |
| `bbr_cwnd_gain` | PROBE_BW 下 cwnd 增益 | 2.0（L363） |
| `bbr_pacing_gain[]` | PROBE_BW 八相位表 | 5/4, 3/4, 1×6（L380–385） |
| `bbr_cycle_rand` | 进入 PROBE_BW 时起始相位随机上界 | 7（L392） |
| `bbr_cwnd_min_target` | 最小 cwnd（包） | 4（L400） |
| `bbr_full_bw_thresh` | STARTUP 管道满判定增长阈值 | 25%（L411） |
| `bbr_full_bw_cnt` | STARTUP 连续停滞轮数判定 | 3（L418） |
| `bbr_lt_intvl_min_rtts` | LT 采样区间最小 RTT 轮数 | 4（L432） |
| `bbr_lt_loss_thresh` | LT 高丢包率判定阈值 | 50/256 ≈ 20%（L438） |
| `bbr_lt_bw_ratio` | LT 带宽一致性相对阈值 | 1/8 = 12.5%（L442） |
| `bbr_lt_bw_diff` | LT 带宽一致性绝对阈值 | 4Kbps = 500 bytes/sec（L447） |
| `bbr_lt_bw_max_rtts` | LT lt_bw 使用期限 | 48 轮（L451） |

带宽定点：`BW_SCALE`、`BW_UNIT`（约 L199–200）；增益定点：`BBR_SCALE`、`BBR_UNIT`（约 L209–210）。

---

## 十一、源码导航（`tcp_bbr.c`）

| 区域 | 函数 / 符号 | 作用 |
|------|-------------|------|
| 状态与模型 | `enum bbr_mode`，`struct bbr` | 每连接私有状态，存于 `icsk_ca_priv`（约 L216 起） |
| 主循环 | `bbr_main()` → `bbr_update_model()` | 每 ACK：更新模型 → `bbr_set_pacing_rate()` → `bbr_set_cwnd()`（约 L1822–1868） |
| 带宽 | `bbr_update_bw()`，`bbr_bw()` | 投递速率采样 + `minmax` 最大窗（BtlBw） |
| RTT | `bbr_update_min_rtt()` | min RTT 窗、PROBE_RTT 进出 |
| 状态机 | `bbr_check_full_bw_reached()`，`bbr_check_drain()`，`bbr_update_cycle_phase()` | STARTUP/DRAIN/PROBE_BW/PROBE_RTT 迁移 |
| LT 限速器 | `bbr_lt_bw_sampling()`，`bbr_lt_bw_interval_done()` | token-bucket 限速场景（与丢包区间配合） |
| cwnd 控制 | `bbr_set_cwnd()`，`bbr_bdp()`，`bbr_quantization_budget()` | BDP 计算、量化预算、丢包恢复 |
| pacing 控制 | `bbr_set_pacing_rate()`，`bbr_bw_to_pacing_rate()` | 速率设置、定点转换、1% 边际 |
| ACK 聚合 | `bbr_update_ack_aggregation()`，`bbr_ack_aggregation_cwnd()` | 双缓冲追踪、cwnd 补偿 |
| 增益设置 | `bbr_update_gains()` | 根据 mode 设置 pacing_gain / cwnd_gain |
| 栈回调 | `bbr_init`，`bbr_ssthresh`，`bbr_undo_cwnd`，`bbr_cwnd_event`，`bbr_set_state` | 初始化、`snd_ssthresh=∞`、恢复/空闲/RTO 等 |
| 模块注册 | `tcp_bbr_cong_ops`，`bbr_register` | `.name = "bbr"`，`.cong_control = bbr_main`（约 L2104–2127） |

诊断：`bbr_get_info()` → `ss`/INET_DIAG 可见 `bbr_bw*`，`bbr_min_rtt`，`bbr_pacing_gain`，`bbr_cwnd_gain`（约 L2015–2045）。

---

## 十二、维护与核对清单

1. **升级内核版本**：优先 diff `tcp_bbr.c` 中 §十所列常量与 `bbr_main` 调用链，再同步本文档表格与流程描述。
2. **区分论文与实现**：队列规则、ECN、与 CUBIC 共存等以**当前内核行为**为准。
3. **与通用 TCP 文档配合**：拥塞插件接口见 `include/net/tcp.h` 中 `struct tcp_congestion_ops`；ACK 总入口见 `net/ipv4/tcp_input.c` 中 `tcp_ack()` → `tcp_cong_control()`。

---

## 十三、参考文献

- Neal Cardwell, Yuchung Cheng, C. Stephen Gunn, Soheil Hassas Yeganeh, Van Jacobson.
  "BBR: Congestion-Based Congestion Control", ACM Queue, Vol. 14 No. 5, Sep-Oct 2016.
- Linux 内核源码: `net/ipv4/tcp_bbr.c` (v5.15.78)
- BBR 开发讨论邮件列表: https://groups.google.com/forum/#!forum/bbr-dev
