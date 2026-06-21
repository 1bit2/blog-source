+++
date = '2026-04-16'
title = 'Linux 5.15.78 TCP 拥塞控制算法实现原理分析'
weight = 23
tags = [
    "TCP",
    "拥塞控制",
    "CUBIC",
    "tcp_congestion_ops",
]
categories = [
    "网络",
]
+++
# Linux 5.15.78 TCP 拥塞控制算法实现原理分析

> 基于 `net/ipv4/tcp_*.c` 源码的深度分析，重点评估**高时延 + 高丢包率**场景下的适用性。

## 一、拥塞控制框架概述

内核通过 `struct tcp_congestion_ops`（定义于 `include/net/tcp.h`）提供统一的拥塞控制接口，所有算法实现为可插拔模块。

核心回调函数：

| 回调 | 调用时机 | 作用 |
|------|---------|------|
| `cong_avoid` | 每个 ACK 到达 | 传统 AIMD 式窗口增长 |
| `cong_control` | 每个 ACK 到达 | 模型驱动的速率+窗口控制（BBR 使用） |
| `ssthresh` | 进入 Recovery/Loss | 计算新的慢启动阈值 |
| `set_state` | CA 状态变化 | 算法状态管理 |
| `pkts_acked` | 处理 ACK 后 | RTT 采样、带宽估计等 |
| `cwnd_event` | 特殊事件 | 空闲重启、CWR 完成等 |

注册要求：必须实现 `ssthresh`、`undo_cwnd`，以及 `cong_avoid` 或 `cong_control` 之一。

---

## 二、各算法实现原理

### 1. Reno（基线算法）

**源码**：`net/ipv4/tcp_cong.c`  
**类型**：纯丢包驱动（Loss-based AIMD）

```c
// tcp_cong.c: Reno 拥塞避免 —— 经典 AIMD
void tcp_reno_cong_avoid(struct sock *sk, u32 ack, u32 acked) {
    // 慢启动：cwnd 每 ACK +1（指数增长）
    // 拥塞避免：每 RTT +1 MSS（线性增长）
    tcp_cong_avoid_ai(tp, tcp_snd_cwnd(tp), acked);
}

// 丢包时的 ssthresh：直接减半
u32 tcp_reno_ssthresh(struct sock *sk) {
    return max(tcp_snd_cwnd(tp) >> 1U, 2U);
}
```

**高时延高丢包评估**：❌ **极差**
- 慢启动后线性增长 +1/RTT，高 RTT 下恢复极慢
- 丢包直接砍半，高丢包率下 cwnd 持续萎缩
- 吞吐量公式：`throughput ≈ MSS / (RTT × √p)`，p 为丢包率，RTT 大时吞吐量极低

---

### 2. CUBIC（默认算法）

**源码**：`net/ipv4/tcp_cubic.c`  
**类型**：丢包驱动 + HyStart 延迟辅助

CUBIC 使用三次函数取代 Reno 的线性增长：

```
W(t) = C × (t - K)³ + Wmax

其中：
  Wmax = 丢包前的窗口大小
  K = ³√(Wmax × β / C)  —— 回到 Wmax 的时间
  C = 0.4（bic_scale=41 对应的缩放值）
  β = 0.7（beta=717/1024）
```

**关键实现**（`bictcp_update`）：
- 丢包后窗口快速恢复到 Wmax 附近（三次函数凸区间）
- 超过 Wmax 后缓慢探测（三次函数凹区间）
- 配合 TCP 友好性检查，保证不劣于 Reno

**ssthresh 计算**（`cubictcp_recalc_ssthresh`）：

```c
// tcp_cubic.c
ssthresh = cwnd * beta / 1024;  // beta=717 → ≈ 0.7 × cwnd
// 快速收敛：如果 cwnd < last_max_cwnd，进一步降低 last_max_cwnd
```

**HyStart**：监测 RTT 变化和 ACK 间距，在慢启动阶段提前退出，避免过冲。

**高时延高丢包评估**：⚠️ **一般**
- 比 Reno 恢复快（三次函数增长），MD 更温和（0.7 vs 0.5）
- 但本质仍是丢包驱动：高丢包率下频繁触发 MD
- 高 RTT 下三次函数的时间轴拉长，窗口恢复仍然慢

---

### 3. BBR（模型驱动算法）

**源码**：`net/ipv4/tcp_bbr.c`  
**类型**：带宽-延迟模型驱动（非丢包触发）

BBR 是唯一使用 `cong_control` 回调（而非 `cong_avoid`）的内核原生算法。它不依赖丢包信号，而是直接建模网络的两个物理参数：

```
BtlBw = max(delivery_rate, 过去 10 RTT)      —— 瓶颈带宽
RTprop = min(rtt_sample, 过去 10 秒)           —— 最小传播延迟
BDP = BtlBw × RTprop                           —— 带宽延迟积
```

**发送速率控制**：

```
pacing_rate = pacing_gain × BtlBw × 0.99
target_cwnd = cwnd_gain × BDP + extra_acked
```

**状态机**：

| 状态 | pacing_gain | cwnd_gain | 目标 |
|------|-------------|-----------|------|
| STARTUP | 2.885 | 2.885 | 指数探测管道容量 |
| DRAIN | 0.347 | 2.885 | 排空 STARTUP 产生的队列 |
| PROBE_BW | 循环 5/4,3/4,1×6 | 2.0 | 稳态带宽探测 |
| PROBE_RTT | 1.0 | — (cwnd=4) | 测量真实传播延迟 |

**丢包处理**（`bbr_set_cwnd_to_recover_or_restore`）：

```c
// tcp_bbr.c: BBR 的丢包恢复策略
// 第一步：扣减 losses（丢失的包不在网络中，释放 cwnd 空间）
if (rs->losses > 0)
    cwnd = max_t(s32, cwnd - rs->losses, 1);

// 进入恢复第一轮：包守恒（一进一出）
if (state == TCP_CA_Recovery && prev_state != TCP_CA_Recovery) {
    bbr->packet_conservation = 1;
    cwnd = tcp_packets_in_flight(tp) + acked;
}

// 退出恢复：取 max(当前cwnd, 恢复前保存的cwnd)
```

**ssthresh**：BBR 将 `snd_ssthresh` 设为 `TCP_INFINITE_SSTHRESH`，**不使用传统 ssthresh 机制**。

**高时延高丢包评估**：✅ **最佳选择**
- **不将丢包等同于拥塞**：丢包不触发窗口减半，而是通过模型估计的 BDP 设置窗口
- **高 RTT 不影响窗口增长速度**：增长由带宽模型驱动，非 +1/RTT
- **恢复策略温和**：包守恒 + cwnd 恢复，不会像 AIMD 算法那样窗口雪崩
- **实际部署验证**：Google 在跨洲际高延迟链路上取得显著吞吐量提升

---

### 4. Hybla（卫星链路专用）

**源码**：`net/ipv4/tcp_hybla.c`  
**类型**：RTT 缩放的 AIMD

Hybla 专为高延迟异构网络（如卫星）设计，核心思想是消除 RTT 对吞吐量的影响：

```
ρ = RTT / RTT₀        (RTT₀ 默认 25ms，参考 RTT)

慢启动增量：  INC = 2^ρ - 1    (每 ACK)
拥塞避免增量：INC = ρ² / cwnd   (每 ACK)
```

```c
// tcp_hybla.c: ρ 参数计算
ca->rho_3ls = max_t(u32, tp->srtt_us / (rtt0 * USEC_PER_MSEC), 8U);
ca->rho = ca->rho_3ls >> 3;  // 整数部分

// 拥塞避免：增量 = ρ²/W，RTT 越大增长越快
increment = ca->rho2_7ls / tcp_snd_cwnd(tp);
```

当 RTT = 250ms（ρ=10）时，Hybla 的拥塞避免增量是 Reno 的 100 倍（ρ²=100）。

**ssthresh**：使用 `tcp_reno_ssthresh` —— **直接减半**。

**高时延高丢包评估**：⚠️ **部分适合**
- ✅ 高时延：ρ 缩放有效消除 RTT 对增长速度的影响
- ❌ 高丢包：ssthresh 仍然使用 Reno 的减半策略，高丢包率下窗口持续萎缩
- 适合"高时延 + 低丢包"场景（如卫星链路）

---

### 5. Westwood+（无线网络优化）

**源码**：`net/ipv4/tcp_westwood.c`  
**类型**：带宽估计辅助的丢包恢复

Westwood+ 的创新在于丢包后的窗口恢复策略——使用带宽估计而非简单减半：

```c
// tcp_westwood.c: 带宽估计设置 ssthresh
static u32 tcp_westwood_bw_rttmin(const struct sock *sk) {
    // ssthresh = BW_est × RTT_min / MSS
    return max_t(u32, (w->bw_est * w->rtt_min) / tp->mss_cache, 2);
}

// 丢包事件处理
static void tcp_westwood_event(struct sock *sk, enum tcp_ca_event event) {
    switch (event) {
    case CA_EVENT_LOSS:
        tp->snd_ssthresh = tcp_westwood_bw_rttmin(sk);  // 基于 BW 估计
        break;
    case CA_EVENT_COMPLETE_CWR:
        tp->snd_ssthresh = tcp_westwood_bw_rttmin(sk);
        tcp_snd_cwnd_set(tp, tp->snd_ssthresh);
        break;
    }
}
```

**带宽估计**使用 7/8 低通滤波器：

```c
// 两级滤波：bw_ns_est（快速），bw_est（平滑）
bw_ns_est = (7 × bw_ns_est + bk/delta) >> 3;
bw_est    = (7 × bw_est + bw_ns_est) >> 3;
```

**拥塞避免**：与 Reno 完全相同（`tcp_reno_cong_avoid`）。

**高时延高丢包评估**：⚠️ **部分适合**
- ✅ 丢包恢复基于 BW 估计，比 Reno 减半更智能
- ❌ 拥塞避免仍是 Reno 的 +1/RTT，高 RTT 下增长慢
- ❌ 带宽估计在高丢包率下可能不准确（采样窗口内有效数据少）
- 适合"低时延 + 随机丢包"场景（如无线局域网）

---

### 6. Veno（无线接入网优化）

**源码**：`net/ipv4/tcp_veno.c`  
**类型**：延迟辅助的区分式丢包响应

Veno 的核心创新是**区分拥塞丢包和随机丢包**：

```c
// tcp_veno.c: 通过 RTT 变化判断网络状态
target_cwnd = cwnd × basertt / rtt;
diff = cwnd - target_cwnd;  // 队列中的估计包数

if (diff < beta) {
    // "非拥塞状态"：丢包可能是随机的
    // 拥塞避免：每 RTT +1（标准速度）
    tcp_cong_avoid_ai(tp, tcp_snd_cwnd(tp), acked);
} else {
    // "拥塞状态"：丢包可能是真正拥塞
    // 拥塞避免：每 2 个 RTT +1（减速）
}
```

**差异化 ssthresh**（关键优势）：

```c
// tcp_veno.c: 根据网络状态区分丢包响应
static u32 tcp_veno_ssthresh(struct sock *sk) {
    if (veno->diff < beta)
        // 非拥塞状态丢包 → 只减 20%（保留 80%）
        return max(tcp_snd_cwnd(tp) * 4 / 5, 2U);
    else
        // 拥塞状态丢包 → 减 50%（标准 Reno）
        return max(tcp_snd_cwnd(tp) >> 1U, 2U);
}
```

**高时延高丢包评估**：⚠️ **部分适合**
- ✅ 能区分随机丢包（只减 20%）和拥塞丢包（减 50%），在随机丢包场景下窗口保持更大
- ❌ 拥塞避免增量仍是 +1/RTT 或 +1/2RTT，高 RTT 下增长仍然慢
- ❌ 丢包率很高时即使只减 20%，频繁触发仍会导致窗口萎缩
- 适合"中低时延 + 中等随机丢包"（如 WiFi）

---

### 7. Illinois（高速网络优化）

**源码**：`net/ipv4/tcp_illinois.c`  
**类型**：延迟自适应 α/β 参数

Illinois 根据排队延迟动态调整 AIMD 的两个核心参数：

```
AI 增量 α：从延迟得出，范围 [0.3, 10]
MD 因子 β：从延迟得出，范围 [0.125, 0.5]

延迟低(队列空) → α 大(快速增长)，β 小(温和减少)
延迟高(队列满) → α 小(缓慢增长)，β 大(大幅减少)
```

```c
// tcp_illinois.c: α/β 自适应
// α 根据平均排队延迟调整
if (dm < d1)      alpha = ALPHA_MAX;     // 延迟很低 → 快速增长
else if (dm >= d2) alpha = ALPHA_MIN;     // 延迟很高 → 慢增长
else               alpha = 线性插值;

// β 根据平均排队延迟调整
if (dm < d1)      beta = BETA_MIN;       // 延迟很低 → 温和减少(0.125)
else if (dm >= d2) beta = BETA_MAX;       // 延迟很高 → 大幅减少(0.5)
else               beta = 线性插值;
```

**高时延高丢包评估**：⚠️ **一般**
- 高丢包时频繁进入 Recovery，但 β 可能较温和（取决于延迟判断）
- 仍然是 AIMD 框架，受限于 +α/cwnd 的增长模式
- 主要设计目标是高速大窗口网络，非高丢包场景

---

### 8. H-TCP（高速长距离网络）

**源码**：`net/ipv4/tcp_htcp.c`  
**类型**：时间自适应 + RTT 缩放

H-TCP 的 α（增长速率）随"距上次丢包的时间"递增，β 由 RTT 比值或带宽估计决定：

```
α(Δt) = 1 + 10×(Δt - 1) + 0.5×(Δt - 1)²    (Δt > 1秒)
β = min(RTTmin/RTTmax, 0.8)                    (use_rtt_scaling=1 时)
```

**高时延高丢包评估**：⚠️ **部分适合**
- α 基于时间而非 RTT 计数，高 RTT 下增长速度不受惩罚
- β 基于 RTT 比值，排队少时温和减少
- 但仍是丢包触发的 AIMD，高丢包率下频繁重置 Δt 和 α

---

### 9. BIC

**源码**：`net/ipv4/tcp_bic.c`  
**类型**：二分搜索式增长（CUBIC 前身）

使用二分搜索策略在 `last_max_cwnd` 附近快速收敛：
- `cwnd < last_max_cwnd` 时：二分搜索逼近
- `cwnd >= last_max_cwnd` 时：线性探测

**ssthresh**：`max(cwnd × 819/1024, 2)` ≈ 0.8 × cwnd

**高时延高丢包评估**：⚠️ 类似 CUBIC，但三次函数增长模式更适合大窗口

---

### 10. Vegas（纯延迟驱动）

**源码**：`net/ipv4/tcp_vegas.c`  
**类型**：纯延迟驱动

通过估计队列中的包数来调整窗口：

```c
// tcp_vegas.c: 延迟驱动的窗口调整
diff = cwnd × (rtt - baseRTT) / baseRTT;  // 队列包数估计

if (diff > beta)  cwnd--;   // 队列太长，减小窗口
if (diff < alpha) cwnd++;   // 队列为空，增大窗口
```

**高时延高丢包评估**：❌ **不适合**
- 延迟驱动算法在高时延变化环境中 baseRTT 测量不准
- 丢包后进入 Recovery 用 Reno ssthresh，仍然减半
- 与 loss-based 流竞争时被挤压

---

### 11. YeAH（混合算法）

**源码**：`net/ipv4/tcp_yeah.c`  
**类型**：Scalable AI + Vegas 延迟检测

混合使用 Scalable TCP 的快速增长和 Vegas 的延迟检测：

```c
// 增长：使用 min(cwnd, 100) 作为 AI 除数（类似 Scalable TCP）
tcp_cong_avoid_ai(tp, min(tcp_snd_cwnd(tp), TCP_SCALABLE_AI_CNT), acked);

// 延迟检测：Vegas 式队列估计
queue = cwnd × (rtt - baseRTT) / rtt;
if (queue > 80)  // 提前减速避免丢包
```

**高时延高丢包评估**：⚠️ 增长快但丢包响应仍基于 AIMD

---

### 12. Scalable TCP / HighSpeed TCP

**源码**：`tcp_scalable.c` / `tcp_highspeed.c`  
**类型**：激进丢包驱动

- Scalable：AI 除数 = `min(cwnd, 100)`，MD = 7/8（只减 12.5%）
- HighSpeed：查表确定 AI/MD，大窗口时 MD 更温和

**高时延高丢包评估**：⚠️ MD 温和但仍是纯丢包驱动，频繁丢包下不断触发

---

### 13. CDG（延迟梯度）

**源码**：`net/ipv4/tcp_cdg.c`  
**类型**：延迟梯度 + 概率退避

通过 RTT 变化梯度（而非绝对值）判断拥塞，概率性触发退避：

```
grad > 0 → 延迟在增加 → 概率性进入 CWR（主动退避）
grad <= 0 → 延迟稳定或下降 → 正常 Reno 增长
```

**高时延高丢包评估**：⚠️ 概率退避在高噪声环境中可能频繁误判

---

### 14. TCP-LP / TCP-NV / DCTCP

**源码**：`tcp_lp.c` / `tcp_nv.c` / `tcp_dctcp.c`

| 算法 | 设计场景 | 高时延高丢包适用性 |
|------|---------|-------------------|
| TCP-LP | 后台低优先级流 | ❌ 主动退让，不追求吞吐量 |
| TCP-NV | 数据中心内部 | ❌ 假设同构环境，不适合异构广域网 |
| DCTCP | 数据中心（需 ECN） | ❌ 依赖 ECN 标记，广域网不支持 |

---

## 三、高时延 + 高丢包场景综合评估

### 评估维度

| 维度 | 说明 |
|------|------|
| 高 RTT 增长能力 | RTT 大时窗口能否快速增长 |
| 丢包容忍度 | 丢包后窗口削减程度 |
| 带宽利用率 | 能否充分利用可用带宽 |
| 恢复速度 | 丢包后回到稳态的速度 |

### 综合评分

| 算法 | 高RTT增长 | 丢包容忍 | 带宽利用 | 恢复速度 | 综合评价 |
|------|-----------|---------|---------|---------|---------|
| **BBR** | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | **最佳** |
| Hybla | ⭐⭐⭐⭐⭐ | ⭐⭐ | ⭐⭐⭐ | ⭐⭐ | 仅适合高时延低丢包 |
| Veno | ⭐⭐ | ⭐⭐⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐ | 适合中低时延随机丢包 |
| Westwood+ | ⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐ | 适合无线网随机丢包 |
| H-TCP | ⭐⭐⭐⭐ | ⭐⭐ | ⭐⭐⭐⭐ | ⭐⭐⭐ | 适合高速长距离 |
| Illinois | ⭐⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐ | 平衡但不突出 |
| CUBIC | ⭐⭐⭐ | ⭐⭐ | ⭐⭐⭐⭐ | ⭐⭐⭐ | 通用默认，非最优 |
| Reno | ⭐ | ⭐ | ⭐⭐ | ⭐ | 基线参考 |

---

## 四、结论：BBR 是兼顾高时延高丢包的最佳选择

### BBR 的核心优势

#### 1. 不将丢包等同于拥塞（根本性差异）

所有传统 AIMD 算法（Reno/CUBIC/Hybla/Veno/Westwood+ 等）都遵循同一范式：

```
检测到丢包 → ssthresh = f(cwnd) → cwnd 大幅减少 → 线性恢复
```

在高丢包率（如 5%+）下，这种范式导致 cwnd 在"减半-恢复-减半"的锯齿中持续低位运行。

BBR 的范式完全不同：

```c
// BBR 不使用传统 ssthresh 机制
// tcp_bbr.c: bbr_ssthresh()
static u32 bbr_ssthresh(struct sock *sk) {
    bbr_save_cwnd(sk);                    // 仅保存 cwnd 供恢复用
    return tcp_sk(sk)->snd_ssthresh;      // 返回无穷大
}

// BBR 的 cwnd 由模型驱动
target_cwnd = cwnd_gain × BtlBw × RTprop + extra_acked
```

丢包只触发温和的包守恒恢复，而不会重置 cwnd 到模型估计值以下。

#### 2. 高 RTT 不影响窗口增长

传统算法的吞吐量受限于 RTT：

```
Reno:  throughput ≈ MSS / (RTT × √p)
CUBIC: throughput ≈ C^(1/4) / (RTT × p^(3/4))  （仍含 RTT 因子）
```

BBR 的吞吐量由带宽模型决定：

```
BBR:   throughput ≈ BtlBw × (1 - pacing_margin)
       与 RTT 和丢包率 p 无关（在合理范围内）
```

#### 3. 稳态发送速率精确

BBR 通过 pacing 控制发送速率为 `BtlBw × 0.99`，在稳态下：
- 不会像 CUBIC 那样产生锯齿波动
- 不会在瓶颈处积累大量队列
- 对丢包的免疫来自于速率控制而非窗口控制

#### 4. 实际验证

Google 在 YouTube 和 Google Cloud 的部署数据显示：
- 跨洲际链路（RTT 200ms+）吞吐量提升 2-25 倍
- 在 1-5% 丢包率下仍能维持接近瓶颈带宽的吞吐量
- CUBIC 在同样条件下吞吐量衰减到理论值的 10% 以下

### BBR 的局限性

| 场景 | 问题 |
|------|------|
| 浅缓冲交换机 | BBR v1 的 STARTUP 可能导致过度丢包 |
| 与 CUBIC 共存 | BBR 可能占用超过公平份额的带宽 |
| 高丢包（>15%） | delivery_rate 采样受损，BtlBw 估计可能偏低 |
| 极端抖动 | RTprop 10s 滤波窗口可能不够灵活 |

### 使用建议

```bash
# 启用 BBR
sysctl -w net.ipv4.tcp_congestion_control=bbr
# BBR 最好配合 fq qdisc 使用以实现精确 pacing
tc qdisc replace dev eth0 root fq

# 如果 BBR 不可用（老内核），次选组合方案：
# 高时延场景用 Hybla，高丢包场景用 Veno/Westwood+
# 但没有单一传统算法能同时兼顾两者
```

### 如果需要兼顾高时延高丢包但不能用 BBR

可考虑组合策略：
1. **Hybla + 调大初始窗口**：解决高 RTT 增长慢的问题，但丢包仍减半
2. **Veno**：对随机丢包只减 20%，但增长速度受 RTT 限制
3. **编写自定义算法**：结合 Hybla 的 ρ 缩放 + Veno 的差异化 ssthresh

---

## 五、源码文件索引

| 文件 | 算法 | 类型 |
|------|------|------|
| `net/ipv4/tcp_cong.c` | Reno + 框架 | 丢包驱动 |
| `net/ipv4/tcp_cubic.c` | CUBIC | 丢包驱动 + HyStart |
| `net/ipv4/tcp_bbr.c` | BBR | 模型驱动 |
| `net/ipv4/tcp_hybla.c` | Hybla | RTT 缩放 AIMD |
| `net/ipv4/tcp_westwood.c` | Westwood+ | 带宽估计辅助 |
| `net/ipv4/tcp_veno.c` | Veno | 延迟辅助区分丢包 |
| `net/ipv4/tcp_illinois.c` | Illinois | 延迟自适应 α/β |
| `net/ipv4/tcp_htcp.c` | H-TCP | 时间自适应 |
| `net/ipv4/tcp_vegas.c` | Vegas | 纯延迟驱动 |
| `net/ipv4/tcp_yeah.c` | YeAH | Scalable + Vegas 混合 |
| `net/ipv4/tcp_bic.c` | BIC | 二分搜索（CUBIC 前身） |
| `net/ipv4/tcp_cdg.c` | CDG | 延迟梯度 + 概率退避 |
| `net/ipv4/tcp_scalable.c` | Scalable | 激进 AIMD |
| `net/ipv4/tcp_highspeed.c` | HighSpeed | 查表 AIMD |
| `net/ipv4/tcp_lp.c` | TCP-LP | 低优先级 |
| `net/ipv4/tcp_nv.c` | TCP-NV | 数据中心延迟驱动 |
| `net/ipv4/tcp_dctcp.c` | DCTCP | ECN 驱动 |
