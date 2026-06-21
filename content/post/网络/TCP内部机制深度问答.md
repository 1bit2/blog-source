+++
date = '2026-04-22'
title = 'TCP 内部机制深度问答：速率采样、RACK、重传定时器、CUBIC、BBR'
weight = 26
tags = [
    "TCP",
    "tcp_rate",
    "RACK",
    "tcp_timer",
    "CUBIC",
    "BBR",
    "源码分析",
]
categories = [
    "网络",
]
+++
# TCP 内部机制深度问答：速率采样、RACK、重传定时器、CUBIC、BBR

> 基于 Linux 5.15.78 内核源码的 Agent 对抗式深度分析。由两个独立 Agent 分别出题、答题、评判，交叉验证确保结论与源码一致。

---

## 一、速率采样：`tcp_rate_gen()` 中 `snd_us` 与 `ack_us` 的计算

### 问题

在 `tcp_rate_gen()` 中，发送阶段时长 `snd_us` 与 ACK 阶段时长 `ack_us` 如何由已有字段算出来？为何最终将 `rs->interval_us` 取为 `max(snd_us, ack_us)` 而不是二者之和或平均值？

### 源码路径

`net/ipv4/tcp_rate.c` → `tcp_rate_gen()` (L156–168)

### 分析

#### 两个阶段的计算来源

速率采样将数据传输建模为**两阶段管线**：

1. **发送阶段（send phase）**：`snd_us` 直接取 `rs->interval_us`，该值在 `tcp_rate_skb_delivered()` 中已由发送时间戳差值算出：

```c
// tcp_rate.c: tcp_rate_skb_delivered() L109-113
tp->first_tx_mstamp  = tx_tstamp;
rs->interval_us = tcp_stamp_us_delta(tp->first_tx_mstamp,
                                     scb->tx.first_tx_mstamp);
```

即"采样窗口内最后一个包的发送时刻"减去"第一个包的发送时刻"。

2. **ACK 阶段（ack phase）**：`ack_us` 用当前 ACK 处理时刻减去采样窗口起始时的 ACK 时间戳快照：

```c
// tcp_rate.c: tcp_rate_gen() L161-163
snd_us = rs->interval_us;                          /* send phase */
ack_us = tcp_stamp_us_delta(tp->tcp_mstamp,
                            rs->prior_mstamp);      /* ack phase */
rs->interval_us = max(snd_us, ack_us);
```

#### 为什么取 max 而不是和或平均

源码注释直接给出了原因：

```c
/* Model sending data and receiving ACKs as separate pipeline phases
 * for a window. Usually the ACK phase is longer, but with ACK
 * compression the send phase can be longer. To be safe we use the
 * longer phase.
 */
```

核心逻辑：

- 投递速率 = `delivered / interval_us`。`interval_us` 是分母，分母越小则速率越大。
- **ACK 压缩/聚合**时，多个 ACK 合并到达，`ack_us` 被人为缩短——此时若用 `ack_us` 做分母，会**高估**带宽。
- **发送被延迟**（应用层写入间隔、pacing 等），`snd_us` 更大——此时 `snd_us` 才能反映真实间隔。
- 取 `max` 即**取较长的阶段**作为分母，是保守估计，避免任一阶段被"压缩"后导致带宽高估。
- 求和会重复计算时间序列重叠的两段，平均值在一侧被主导时无法取到保守的"有效瓶颈间隔"。

#### 数据流图

```
发送端                                           接收端
  |------- snd_us -------|                          |
  S1     S2     S3      S4  ──────────────────>   ACK
  |                      |                          |
  |                      |<──── ack_us ────>        |
  |                      prior_mstamp    tcp_mstamp |

interval_us = max(snd_us, ack_us)
delivery_rate = delivered / interval_us
```

---

## 二、RACK 丢包检测：`tcp_rack_advance()` 的重传二义性过滤

### 问题

`tcp_rack_advance()` 在何种条件下会直接 `return`，不更新 RACK 的 `advanced` 标志位和参考点时间戳/序号？这对应哪种歧义？

### 源码路径

`net/ipv4/tcp_recovery.c` → `tcp_rack_advance()` (L195–212)

### 分析

#### 早退条件

```c
// tcp_recovery.c: tcp_rack_advance() L200-203
rtt_us = tcp_stamp_us_delta(tp->tcp_mstamp, xmit_time);
if (rtt_us < tcp_min_rtt(tp) && (sacked & TCPCB_RETRANS))
    return;
```

当且仅当**两个条件同时满足**时跳过更新：

| 条件 | 含义 |
|------|------|
| `rtt_us < tcp_min_rtt(tp)` | 本次测得的 RTT 小于连接观测到的最小 RTT |
| `sacked & TCPCB_RETRANS` | 被确认的段是重传段 |

#### 对应的歧义

这是经典的**重传二义性（Retransmission Ambiguity）**：

当发送端重传了一个包，随后收到对该序号的 ACK 时，**无法区分是哪个副本被确认**：

```
时间线：
  t0: 发送原始包 P（seq=1000）
  t1: 超时，重传包 P'（seq=1000）
  t2: 收到 ACK（seq=1001）

  可能情况 A：ACK 是对 P（原始包）的确认，RTT = t2-t0
  可能情况 B：ACK 是对 P'（重传包）的确认，RTT = t2-t1
```

RACK 利用 `xmit_time`（取重传时刻 t1）计算 `rtt_us = t2 - t1`。如果结果**小于 `min_rtt`**，说明这个"RTT"不合理地短——很可能是情况 A（原始包被确认），而 RACK 误用了 t1 做起点。

若此时仍更新 RACK 参考点（`rack.mstamp`、`rack.end_seq`）和 `rack.rtt_us`，会导致：
- 参考时间被错误推进到重传时刻
- 后续其他包被错误判定为"超过 RACK 参考点一个 reo_wnd"的丢包

因此直接丢弃该样本，**不设置 `tp->rack.advanced = 1`**。

#### 正常更新路径

通过过滤后，正常执行：

```c
// tcp_recovery.c L204-211
tp->rack.advanced = 1;
tp->rack.rtt_us = rtt_us;
if (tcp_rack_sent_after(xmit_time, tp->rack.mstamp,
                        end_seq, tp->rack.end_seq)) {
    tp->rack.mstamp = xmit_time;
    tp->rack.end_seq = end_seq;
}
```

`tcp_rack_sent_after()` 确保参考点在"时间+序号"上**单调递进**。

---

## 三、重传定时器：零窗口分支的特殊行为

### 问题

`tcp_retransmit_timer()` 在零窗口分支（`!tp->snd_wnd`）中，`goto out_reset_timer` 跳过了正常 RTO 路径。此时 `icsk->icsk_retransmits` 是否递增？`out_reset_timer` 处如何设置下一次重传定时器？

### 源码路径

`net/ipv4/tcp_timer.c` → `tcp_retransmit_timer()` (L481–595)

### 分析

#### 零窗口分支的控制流

```c
// tcp_timer.c L481-513
if (!tp->snd_wnd && !sock_flag(sk, SOCK_DEAD) &&
    !((1 << sk->sk_state) & (TCPF_SYN_SENT | TCPF_SYN_RECV))) {
    // 对端恶意缩小窗口，重传变为零窗探测

    if (tcp_jiffies32 - tp->rcv_tstamp > TCP_RTO_MAX) {
        tcp_write_err(sk);          // 超过最大容忍时间，报错退出
        goto out;
    }
    tcp_enter_loss(sk);             // 进入 Loss 状态
    tcp_retransmit_skb(sk, skb, 1); // 重传（零窗探测）
    __sk_dst_reset(sk);
    goto out_reset_timer;           // ← 关键：跳过正常 RTO 路径
}
```

`goto out_reset_timer` 直接跳过了以下代码：

```c
// tcp_timer.c L542
icsk->icsk_retransmits++;   // ← 被跳过：重传计数不递增

// tcp_timer.c L568
icsk->icsk_backoff++;       // ← 被跳过：退避指数不递增
```

#### `out_reset_timer` 处的定时器设置

**关键发现**：`out_reset_timer` 标签处仍然会更新 `icsk_rto`：

```c
// tcp_timer.c L570-591
out_reset_timer:
    if (sk->sk_state == TCP_ESTABLISHED &&
        (tp->thin_lto || ...) &&
        tcp_stream_is_thin(tp) &&
        icsk->icsk_retransmits <= TCP_THIN_LINEAR_RETRIES) {
        icsk->icsk_backoff = 0;
        icsk->icsk_rto = min(__tcp_set_rto(tp), TCP_RTO_MAX);
    } else {
        /* Use normal (exponential) backoff */
        icsk->icsk_rto = min(icsk->icsk_rto << 1, TCP_RTO_MAX);
    }
    inet_csk_reset_xmit_timer(sk, ICSK_TIME_RETRANS,
                              tcp_clamp_rto_to_user_timeout(sk), TCP_RTO_MAX);
```

#### 完整行为总结

| 属性 | 零窗口分支 | 正常 RTO 路径 |
|------|-----------|-------------|
| `icsk_retransmits++` | **不执行** | 执行 |
| `icsk_backoff++` | **不执行** | 执行 |
| `icsk_rto <<= 1`（在 `out_reset_timer`） | **执行**（非 thin 路径） | 执行 |
| 定时器重武装 | 执行 | 执行 |

**设计意图**：零窗口探测不计入正式重传次数（`icsk_retransmits` 不增），避免在持续零窗口时因重传计数过高而提前报错杀连接。但 `icsk_rto` 仍在 `out_reset_timer` 中加倍，防止对零窗口对端的探测过于频繁。

> **交叉验证注**：本结论在 Agent 对抗评判中被特别标注——出题方标准答案原认为"RTO 不加倍"，但答题方指出 `out_reset_timer` 中的 `icsk->icsk_rto <<= 1` 对所有经此出口的路径（含零窗口分支）均生效，**以源码为准，RTO 仍会加倍**。

---

## 四、CUBIC epoch 初始化：`bic_K` 与 `bic_origin_point` 的分支逻辑

### 问题

在 CUBIC 的 `bictcp_update()` 中，新 epoch 初始化时（`ca->epoch_start == 0`），`ca->last_max_cwnd <= cwnd` 与 `ca->last_max_cwnd > cwnd` 两分支对 `bic_K`、`bic_origin_point` 的赋值分别是什么？CUBIC 曲线在 t=0 附近对应哪种几何形状？

### 源码路径

`net/ipv4/tcp_cubic.c` → `bictcp_update()` (L287–303)

### 分析

#### 两分支赋值

```c
// tcp_cubic.c L288-303
if (ca->epoch_start == 0) {
    ca->epoch_start = tcp_jiffies32;
    ca->ack_cnt = acked;
    ca->tcp_cwnd = cwnd;

    if (ca->last_max_cwnd <= cwnd) {
        ca->bic_K = 0;
        ca->bic_origin_point = cwnd;
    } else {
        ca->bic_K = cubic_root(cube_factor
                               * (ca->last_max_cwnd - cwnd));
        ca->bic_origin_point = ca->last_max_cwnd;
    }
}
```

| 条件 | `bic_K` | `bic_origin_point` | 含义 |
|------|---------|-------------------|------|
| `last_max_cwnd <= cwnd` | 0 | 当前 `cwnd` | cwnd 已达/超 Wmax，无需凹段回升 |
| `last_max_cwnd > cwnd` | `cubic_root(...)` | `last_max_cwnd` | cwnd < Wmax，需要凹段爬回 |

#### 几何形状解读

CUBIC 的窗口函数为 `W(t) = C × (t - K)³ + origin_point`，实现中用 `|t-K|` 分凹/凸两段：

```c
// tcp_cubic.c L313-323
if (t < ca->bic_K)
    offs = ca->bic_K - t;      // 凹段
else
    offs = t - ca->bic_K;      // 凸段

delta = (cube_rtt_scale * offs * offs * offs) >> (10+3*BICTCP_HZ);

if (t < ca->bic_K)
    bic_target = ca->bic_origin_point - delta;   // 凹：从下往上趋近 Wmax
else
    bic_target = ca->bic_origin_point + delta;   // 凸：超越 Wmax 探新带宽
```

**情况一：K=0, origin=cwnd（cwnd ≥ Wmax）**

```
W(t)
 ^
 |              ╱ (凸段探测)
 |           ╱
 |        ╱
 |------⨯-------- cwnd = origin_point
 |    t=0=K
 +─────────────────> t

曲线从 cwnd 出发，立即进入凸段向上探测新带宽。
K=0 使凹段长度为零，因为当前窗口已经不低于 Wmax，不需要"回到"任何目标。
```

**情况二：K>0, origin=Wmax（cwnd < Wmax）**

```
W(t)
 ^
 |              Wmax = origin_point
 |  ............⨯.................╱ (凸段探测)
 |  ╱(凹段回升)  |              ╱
 | ╱              |           ╱
 |⨯                |        ╱
 cwnd              t=K
 +─────────────────────────────> t

曲线从 cwnd 出发，沿凹段上升，在 t=K 时回到 Wmax，之后进入凸段探新带宽。
K = ∛(cube_factor × (Wmax - cwnd)) 精确控制回到 Wmax 的时间。
```

---

## 五、BBR `bbr_is_next_cycle_phase()`：三种 pacing_gain 的切换条件

### 问题

在 BBR 的 `bbr_is_next_cycle_phase()` 中，三种 pacing_gain 值（`== BBR_UNIT`、`> BBR_UNIT`、`< BBR_UNIT`）分别对应怎样的切换条件？drain 段为何用 `bbr_inflight(sk, bw, BBR_UNIT)` 而非当前 `pacing_gain`？

### 源码路径

`net/ipv4/tcp_bbr.c` → `bbr_is_next_cycle_phase()` (L1095–1143)

### 分析

#### 公共时间条件

所有三种 gain 都依赖 `is_full_length`——当前 phase 已持续至少一个 `min_rtt_us`：

```c
// tcp_bbr.c L1103-1105
bool is_full_length =
    tcp_stamp_us_delta(tp->delivered_mstamp, bbr->cycle_mstamp) >
    bbr->min_rtt_us;
```

用 `delivered_mstamp`（投递时间戳）而非挂钟，确保空闲期不被误计。

#### 三种 gain 的切换条件对比

| Phase | Gain | 切换条件 | 设计意图 |
|-------|------|---------|---------|
| 2–7 (巡航) | `== BBR_UNIT` (1.0) | `is_full_length` | 纯时间驱动，不改变队列 |
| 0 (探测) | `> BBR_UNIT` (5/4) | `is_full_length && (losses \|\| inflight >= 1.25×BDP)` | 确保数据真正"堆"进管道 |
| 1 (排空) | `< BBR_UNIT` (3/4) | `is_full_length \|\| inflight <= 1×BDP` | 排空越快越好 |

源码：

```c
// tcp_bbr.c L1110-1142
if (bbr->pacing_gain == BBR_UNIT)
    return is_full_length;                           // 巡航：纯时间

if (bbr->pacing_gain > BBR_UNIT)
    return is_full_length &&                         // 探测：时间 AND
        (rs->losses ||                               //   (丢包 OR
         inflight >= bbr_inflight(sk, bw,
                                  bbr->pacing_gain));//    inflight足够大)

return is_full_length ||                             // 排空：时间 OR
    inflight <= bbr_inflight(sk, bw, BBR_UNIT);      //   inflight已够低
```

#### 探测阶段（gain > 1）的额外条件

`rs->losses` 条件允许在**浅缓冲链路**上提前退出探测——如果已经发生丢包，说明缓冲已满，继续以 1.25× 速率注入只会增加丢失而无法探测更多带宽。

#### drain 段为何用 `BBR_UNIT` 而非 `pacing_gain`

drain 的 `pacing_gain = 3/4`，但退出判定用 `bbr_inflight(sk, bw, BBR_UNIT)` 即 **1.0 × BDP**：

```
目标：将 inflight 降到"管道恰好满、队列恰好空"
对应的 inflight = 1.0 × BDP（BBR_UNIT = 1.0）

若用 pacing_gain(3/4)计算：
  目标 inflight = 0.75 × BDP
  → 管道只填了 75%，造成不必要的欠载
  → 浪费 25% 的带宽直到巡航阶段恢复

所以用 BBR_UNIT(1.0)：排空到"恰好填满管道"就够了。
```

#### 逻辑运算符的对称设计

```
探测：AND（保守——必须同时满足时间和数据量，确保探测有效）
排空：OR （激进——任一满足即停，尽快恢复正常发送）
巡航：仅时间（中性——稳态运行，不需额外判断）
```

---

## 六、交叉验证总结

本文的 5 个专题均经过以下流程验证：

1. **Agent A（出题者）** 深入阅读源码，提出需要理解代码逻辑才能回答的问题，并准备标准答案
2. **Agent B（答题者）** 独立阅读源码回答，不可见标准答案
3. **Agent A（裁判）** 对照源码逐题评判

### 评判结果

| 专题 | 评分 | 备注 |
|------|------|------|
| 速率采样 `tcp_rate_gen()` | 正确 | Agent B 准确描述了两阶段管线模型 |
| RACK 重传二义性 | 正确 | 条件与歧义类型均准确 |
| 零窗口重传定时器 | 部分正确 | Agent B 发现标准答案中关于 RTO 加倍的描述不够精确，**以源码为准修正** |
| CUBIC epoch 初始化 | 正确 | 两分支赋值及几何意义均准确 |
| BBR phase 切换条件 | 正确 | 三种 gain 的 AND/OR 逻辑均准确 |

> **特别标注**：第三题（零窗口重传定时器）中，答题方发现 `out_reset_timer` 处的 `icsk->icsk_rto <<= 1` 对零窗口分支同样生效，纠正了出题方标准答案中"RTO 不加倍"的表述。这体现了交叉验证对内核源码分析的价值。

---

## 七、源码文件快速索引

| 文件 | 核心函数 | 本文涉及专题 |
|------|---------|-------------|
| `net/ipv4/tcp_rate.c` | `tcp_rate_gen()`, `tcp_rate_skb_delivered()` | §一 速率采样 |
| `net/ipv4/tcp_recovery.c` | `tcp_rack_advance()` | §二 RACK 丢包检测 |
| `net/ipv4/tcp_timer.c` | `tcp_retransmit_timer()` | §三 重传定时器 |
| `net/ipv4/tcp_cubic.c` | `bictcp_update()` | §四 CUBIC epoch |
| `net/ipv4/tcp_bbr.c` | `bbr_is_next_cycle_phase()` | §五 BBR phase 切换 |
