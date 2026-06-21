+++
date = '2026-04-29'
title = 'TCP 拥塞控制四大机制源码实现'
weight = 22
tags = [
    "TCP",
    "拥塞控制",
    "慢启动",
    "拥塞避免",
    "PRR",
    "快速恢复",
    "RACK",
    "CUBIC",
    "tcp_enter_recovery",
    "tcp_enter_loss",
]
categories = [
    "网络",
]
+++
# TCP 拥塞控制四大机制源码实现

> 基于 Linux 5.15.78，深入分析慢启动、拥塞避免、拥塞发生（丢包检测）、快速恢复（PRR）的实际代码实现。
> 拥塞控制框架与算法注册机制参见 [TCP 协议栈架构](TCP协议栈架构.md) 第 8 章，CUBIC 详见 [TCP 拥塞控制算法](TCP拥塞控制算法.md)，BBR 详见 [BBR 算法原理](BBR算法原理.md)。

## 一、慢启动 (Slow Start)

### 1.1 设计原理

慢启动的目标是在连接初期快速探测网络可用带宽。每收到一个 ACK，拥塞窗口 `snd_cwnd` 增加 1 个 MSS，实际效果是每个 RTT 窗口翻倍，呈指数增长。当 `cwnd >= ssthresh` 时退出慢启动，进入拥塞避免。

### 1.2 初始化：新连接的慢启动状态

新连接创建时，`tcp_init_sock()` 设置初始慢启动参数：

```c
// net/ipv4/tcp.c: tcp_init_sock()
tcp_snd_cwnd_set(tp, TCP_INIT_CWND);      // 初始 cwnd = 10 (RFC 6928)
tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;  // ssthresh = 0x7fffffff（无穷大）
tp->snd_cwnd_clamp = ~0;                   // cwnd 上限 = 无限制
```

`TCP_INFINITE_SSTHRESH = 0x7fffffff`，意味着新连接**始终从慢启动开始**（因为 `cwnd < ssthresh` 永远为真），直到首次发生丢包才确定真正的 `ssthresh`。

### 1.3 核心判断：是否在慢启动

```c
// include/net/tcp.h
static inline bool tcp_in_slow_start(const struct tcp_sock *tp)
{
    return tcp_snd_cwnd(tp) < tp->snd_ssthresh;
}

static inline bool tcp_in_initial_slowstart(const struct tcp_sock *tp)
{
    return tp->snd_ssthresh >= TCP_INFINITE_SSTHRESH;
}
```

### 1.4 慢启动核心实现

```c
// net/ipv4/tcp_cong.c
u32 tcp_slow_start(struct tcp_sock *tp, u32 acked)
{
    u32 cwnd = min(tcp_snd_cwnd(tp) + acked, tp->snd_ssthresh);

    acked -= cwnd - tcp_snd_cwnd(tp);    // 剩余 acked 留给拥塞避免
    tcp_snd_cwnd_set(tp, min(cwnd, tp->snd_cwnd_clamp));

    return acked;  // 返回未消耗的 acked 计数
}
```

**关键设计点**：

1. **`cwnd += acked`**：每个 ACK 确认 N 个段，cwnd 就增加 N，处理 stretch ACK
2. **`min(..., snd_ssthresh)`**：cwnd 增长被 ssthresh 封顶，防止越界
3. **返回剩余 acked**：如果增长到 ssthresh 后还有多余的 acked 计数，返回给调用方用于拥塞避免阶段的增长。这实现了**慢启动到拥塞避免的平滑过渡**

### 1.5 调用路径：以 Reno 和 CUBIC 为例

**Reno 的调用**（`net/ipv4/tcp_cong.c`）：

```c
void tcp_reno_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
    struct tcp_sock *tp = tcp_sk(sk);

    if (!tcp_is_cwnd_limited(sk))   // 窗口没用满就不增长
        return;

    if (tcp_in_slow_start(tp)) {    // cwnd < ssthresh → 慢启动
        acked = tcp_slow_start(tp, acked);
        if (!acked)
            return;                 // 全部被慢启动消耗
    }
    // 剩余 acked 进入拥塞避免
    tcp_cong_avoid_ai(tp, tcp_snd_cwnd(tp), acked);
}
```

**CUBIC 的调用**（`net/ipv4/tcp_cubic.c`）：

```c
static void cubictcp_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bictcp *ca = inet_csk_ca(sk);

    if (!tcp_is_cwnd_limited(sk))
        return;

    if (tcp_in_slow_start(tp)) {
        acked = tcp_slow_start(tp, acked);
        if (!acked)
            return;
    }
    bictcp_update(ca, tcp_snd_cwnd(tp), acked);  // CUBIC 曲线计算
    tcp_cong_avoid_ai(tp, ca->cnt, acked);        // 按 CUBIC 的 cnt 增长
}
```

**两者在慢启动阶段的行为完全相同**：都调用 `tcp_slow_start()`。差异只在拥塞避免阶段——Reno 线性增长，CUBIC 按三次函数增长。

### 1.6 窗口使用率检查

只有窗口被充分利用时才增长 cwnd，避免应用限速场景下的窗口虚增：

```c
// include/net/tcp.h
static inline bool tcp_is_cwnd_limited(const struct sock *sk)
{
    const struct tcp_sock *tp = tcp_sk(sk);

    if (tp->is_cwnd_limited)
        return true;

    // 慢启动中允许更激进：cwnd 被用了一半就算"受限"
    if (tcp_in_slow_start(tp))
        return tcp_snd_cwnd(tp) < 2 * tp->max_packets_out;

    return false;
}
```

### 1.7 慢启动增长过程图示

```
     cwnd
      ▲
      │                                  ┌── ssthresh
      │                              ╱───┘
  20  │                          ╱──╱  ← 到达 ssthresh，切换到拥塞避免
      │                      ╱──╱      （线性增长：每 RTT 加 1）
  16  │                  ╱──╱
      │              ╱──╱
   8  │          ╱──╱
      │      ╱──╱
   4  │  ╱──╱         慢启动阶段
      │╱╱             （指数增长：每 RTT 翻倍）
   2  │╱
   1  │
      └────────────────────────────────── RTT →
      0    1    2    3    4    5    6    7
```

---

## 二、拥塞避免 (Congestion Avoidance)

### 2.1 设计原理

当 `cwnd >= ssthresh` 后，进入拥塞避免阶段。此阶段谨慎增长窗口：理论上每个 RTT 增加 1 个 MSS（加法增大，Additive Increase）。

### 2.2 核心实现：`tcp_cong_avoid_ai`

```c
// net/ipv4/tcp_cong.c
void tcp_cong_avoid_ai(struct tcp_sock *tp, u32 w, u32 acked)
{
    // 如果之前在更高 w 时积累了 credits，现在温和地应用
    if (tp->snd_cwnd_cnt >= w) {
        tp->snd_cwnd_cnt = 0;
        tcp_snd_cwnd_set(tp, tcp_snd_cwnd(tp) + 1);
    }

    tp->snd_cwnd_cnt += acked;
    if (tp->snd_cwnd_cnt >= w) {
        u32 delta = tp->snd_cwnd_cnt / w;

        tp->snd_cwnd_cnt -= delta * w;
        tcp_snd_cwnd_set(tp, tcp_snd_cwnd(tp) + delta);
    }
    tcp_snd_cwnd_set(tp, min(tcp_snd_cwnd(tp), tp->snd_cwnd_clamp));
}
```

**参数 `w` 的含义**：每收到 `w` 个 ACK，cwnd 增加 1。
- **Reno**：`w = cwnd`，即每 cwnd 个 ACK（约一个 RTT）增加 1
- **CUBIC**：`w = ca->cnt`，由三次函数 `bictcp_update()` 计算得出

**`snd_cwnd_cnt` 的作用**：这是一个分数计数器。因为 cwnd 是整数，无法直接实现 "cwnd += 1/cwnd"，所以用计数器累积 acked，当累积到 `w` 时才加 1。

### 2.3 Reno vs CUBIC 的拥塞避免对比

```
Reno:   tcp_cong_avoid_ai(tp, tcp_snd_cwnd(tp), acked)
        → w = cwnd，每 cwnd 个 ACK 加 1
        → 每 RTT cwnd 线性增 1（经典 AIMD）

CUBIC:  bictcp_update(ca, cwnd, acked)
        → 计算 W(t) = C*(t-K)³ + W_max
        → ca->cnt = cwnd / (W(t) - cwnd)
        tcp_cong_avoid_ai(tp, ca->cnt, acked)
        → cnt 小则增长快，cnt 大则增长慢
        → 三次函数曲线：远离 W_max 时增长快，接近时增长慢
```

### 2.4 何时调用拥塞避免

```c
// net/ipv4/tcp_input.c
static void tcp_cong_control(struct sock *sk, u32 ack, u32 acked_sacked,
                             int flag, const struct rate_sample *rs)
{
    const struct inet_connection_sock *icsk = inet_csk(sk);

    if (icsk->icsk_ca_ops->cong_control) {
        icsk->icsk_ca_ops->cong_control(sk, rs);  // BBR 路径：全权控制
        return;
    }

    if (tcp_in_cwnd_reduction(sk)) {
        tcp_cwnd_reduction(sk, acked_sacked, rs->losses, flag);  // 恢复中：PRR
    } else if (tcp_may_raise_cwnd(sk, flag)) {
        tcp_cong_avoid(sk, ack, acked_sacked);  // 正常：慢启动/拥塞避免
    }
    tcp_update_pacing_rate(sk);
}
```

**三个分支**：
1. **`cong_control` 存在**（BBR）：算法自己管一切，不走以下路径
2. **在 cwnd 减小阶段**（Recovery/CWR）：走 PRR 算法
3. **正常状态**（Open/Disorder）：走 `tcp_cong_avoid()` → 算法的 `cong_avoid` 回调

---

## 三、拥塞发生 (Congestion Event / Loss Detection)

### 3.1 两种丢包检测机制

Linux 内核有两种判定丢包的路径：

```
                    丢包检测
                   ╱        ╲
              快速检测         超时检测
             (ACK驱动)        (定时器驱动)
            ╱        ╲            │
       DupACK计数    RACK       RTO超时
       (Reno模式)  (时间模式)      │
            │          │          │
     tcp_time_to_    tcp_rack_  tcp_retransmit_
     recover()      mark_lost() timer()
            │          │          │
            ▼          ▼          ▼
     tcp_enter_     tcp_enter_  tcp_enter_
     recovery()    recovery()   loss()
       (快速恢复)   (快速恢复)   (RTO恢复)
```

### 3.2 快速检测路径：`tcp_fastretrans_alert`

这是拥塞状态机的核心，在每次 `tcp_ack()` 处理中被调用：

```c
// net/ipv4/tcp_input.c
static void tcp_fastretrans_alert(struct sock *sk, const u32 prior_snd_una,
                                  int num_dupack, int *ack_flag, int *rexmit)
{
    struct inet_connection_sock *icsk = inet_csk(sk);
    struct tcp_sock *tp = tcp_sk(sk);
    int fast_rexmit = 0, flag = *ack_flag;
    bool ece_ack = flag & FLAG_ECE;

    // A. ECE 标记 → 禁止撤销（拥塞确实发生了）
    if (ece_ack)
        tp->prior_ssthresh = 0;

    // B. 检查 SACK reneging（对端撤回已 SACK 的数据）
    if (tcp_check_sack_reneging(sk, flag))
        return;

    // C. 一致性验证
    tcp_verify_left_out(tp);

    // D. 状态退出条件：snd_una 越过 high_seq
    if (!before(tp->snd_una, tp->high_seq)) {
        switch (icsk->icsk_ca_state) {
        case TCP_CA_CWR:
            tcp_end_cwnd_reduction(sk);
            tcp_set_ca_state(sk, TCP_CA_Open);
            break;
        case TCP_CA_Recovery:
            if (tcp_try_undo_recovery(sk))
                return;
            tcp_end_cwnd_reduction(sk);
            break;
        }
    }

    // E. 各状态处理
    switch (icsk->icsk_ca_state) {
    case TCP_CA_Recovery:
        // Recovery 中收到新 ACK：尝试部分撤销或继续恢复
        tcp_identify_packet_loss(sk, ack_flag);
        break;

    case TCP_CA_Loss:
        // Loss 中：F-RTO 检测虚假超时 / 继续重传
        tcp_process_loss(sk, flag, num_dupack, rexmit);
        tcp_identify_packet_loss(sk, ack_flag);
        return;

    default:  // Open 或 Disorder
        tcp_identify_packet_loss(sk, ack_flag);
        if (!tcp_time_to_recover(sk, flag)) {
            tcp_try_to_open(sk, flag);  // 尝试回到 Open
            return;
        }
        // 满足恢复条件 → 进入 Recovery
        tcp_enter_recovery(sk, ece_ack);
        fast_rexmit = 1;
    }

    // 非 RACK 模式下更新记分板
    if (!tcp_is_rack(sk) && do_lost)
        tcp_update_scoreboard(sk, fast_rexmit);
    *rexmit = REXMIT_LOST;
}
```

### 3.3 何时判定需要进入恢复

```c
// net/ipv4/tcp_input.c
static bool tcp_time_to_recover(struct sock *sk, int flag)
{
    struct tcp_sock *tp = tcp_sk(sk);

    // 条件1：已有确认丢包的包（RACK 或 SACK 记分板标记了 LOST）
    if (tp->lost_out)
        return true;

    // 条件2：非 RACK 模式下，DupACK 数超过重排序容忍度
    // tcp_dupack_heuristics = sacked_out + 1（Reno 估计）
    // tp->reordering 初始值 = 3（sysctl_tcp_reordering）
    if (!tcp_is_rack(sk) && tcp_dupack_heuristics(tp) > tp->reordering)
        return true;

    return false;
}
```

**经典的 "3 个重复 ACK" 规则**：`tp->reordering` 默认为 3，当 `sacked_out + 1 > 3` 即 `sacked_out >= 3` 时触发。

### 3.4 进入快速恢复：`tcp_enter_recovery`

```c
// net/ipv4/tcp_input.c
void tcp_enter_recovery(struct sock *sk, bool ece_ack)
{
    struct tcp_sock *tp = tcp_sk(sk);

    tp->prior_ssthresh = 0;
    tcp_init_undo(tp);

    if (!tcp_in_cwnd_reduction(sk)) {
        if (!ece_ack)
            tp->prior_ssthresh = tcp_current_ssthresh(sk);  // 保存旧 ssthresh 用于撤销
        tcp_init_cwnd_reduction(sk);  // 核心：计算新 ssthresh、初始化 PRR
    }
    tcp_set_ca_state(sk, TCP_CA_Recovery);
}
```

**`tcp_init_cwnd_reduction`** 是关键——它调用拥塞算法的 `ssthresh()` 回调：

```c
static void tcp_init_cwnd_reduction(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);

    tp->high_seq = tp->snd_nxt;       // 恢复的目标：ACK 越过此序号即恢复完成
    tp->tlp_high_seq = 0;
    tp->snd_cwnd_cnt = 0;
    tp->prior_cwnd = tcp_snd_cwnd(tp); // 保存进入前的 cwnd（PRR 使用）
    tp->prr_delivered = 0;              // PRR 计数器清零
    tp->prr_out = 0;
    tp->snd_ssthresh = inet_csk(sk)->icsk_ca_ops->ssthresh(sk);  // ★ 调用算法
    tcp_ecn_queue_cwr(tp);
}
```

**CUBIC 的 ssthresh 计算**（`net/ipv4/tcp_cubic.c`）：

```c
static u32 cubictcp_recalc_ssthresh(struct sock *sk)
{
    const struct tcp_sock *tp = tcp_sk(sk);
    struct bictcp *ca = inet_csk_ca(sk);

    ca->epoch_start = 0;  // 重置 CUBIC epoch

    // Fast Convergence：如果 cwnd < 上次最大值，更积极减小
    if (tcp_snd_cwnd(tp) < ca->last_max_cwnd && fast_convergence)
        ca->last_max_cwnd = (tcp_snd_cwnd(tp) * (BICTCP_BETA_SCALE + beta))
                            / (2 * BICTCP_BETA_SCALE);
    else
        ca->last_max_cwnd = tcp_snd_cwnd(tp);

    // 新 ssthresh = cwnd * beta（beta 默认 717/1024 ≈ 0.7）
    return max((tcp_snd_cwnd(tp) * beta) / BICTCP_BETA_SCALE, 2U);
}
```

**Reno 的 ssthresh 计算**：

```c
// net/ipv4/tcp_cong.c
u32 tcp_reno_ssthresh(struct sock *sk)
{
    const struct tcp_sock *tp = tcp_sk(sk);
    // 经典 AIMD：ssthresh = cwnd / 2，最小值 2
    return max(tcp_snd_cwnd(tp) >> 1U, 2U);
}
```

### 3.5 RTO 超时路径：`tcp_enter_loss`

当重传定时器超时时，走更严格的丢包恢复路径：

```c
// net/ipv4/tcp_input.c
void tcp_enter_loss(struct sock *sk)
{
    const struct inet_connection_sock *icsk = inet_csk(sk);
    struct tcp_sock *tp = tcp_sk(sk);

    // 1. 标记所有在途包为 LOST
    tcp_timeout_mark_lost(sk);

    // 2. 减小 ssthresh（仅在当前窗口内首次执行）
    if (icsk->icsk_ca_state <= TCP_CA_Disorder ||
        !after(tp->high_seq, tp->snd_una) ||
        (icsk->icsk_ca_state == TCP_CA_Loss && !icsk->icsk_retransmits)) {
        tp->prior_ssthresh = tcp_current_ssthresh(sk);
        tp->prior_cwnd = tcp_snd_cwnd(tp);
        tp->snd_ssthresh = icsk->icsk_ca_ops->ssthresh(sk);  // 调用算法
        tcp_ca_event(sk, CA_EVENT_LOSS);
        tcp_init_undo(tp);
    }

    // 3. cwnd 设为 in_flight + 1（比快速恢复更激进的减小）
    tcp_snd_cwnd_set(tp, tcp_packets_in_flight(tp) + 1);
    tp->snd_cwnd_cnt = 0;

    // 4. 进入 Loss 状态
    tcp_set_ca_state(sk, TCP_CA_Loss);
    tp->high_seq = tp->snd_nxt;

    // 5. F-RTO 检测虚假超时
    tp->frto = sysctl_tcp_frto && (new_recovery || icsk->icsk_retransmits)
               && !inet_csk(sk)->icsk_mtup.probe_size;
}
```

### 3.6 快速恢复 vs RTO 的对比

```
                          快速恢复 (Recovery)              RTO (Loss)
                          ─────────────────              ──────────
  触发条件                 3 个 DupACK / RACK             重传定时器超时
                          检测到丢包                      所有包可能丢失

  ssthresh                ca_ops->ssthresh()              ca_ops->ssthresh()
                          CUBIC: cwnd*0.7                 CUBIC: cwnd*0.7
                          Reno:  cwnd/2                   Reno:  cwnd/2

  cwnd                    PRR 动态控制                     in_flight + 1
                          (逐步减至 ssthresh)              (接近从 1 重启)

  状态                    TCP_CA_Recovery                  TCP_CA_Loss

  恢复方式                PRR + 选择性重传                 从头重传 + 慢启动
                          (保持数据流)                     (F-RTO 检测虚假超时)

  结束条件                snd_una >= high_seq              snd_una >= high_seq
                          → cwnd = ssthresh                → Open + 慢启动
```

---

## 四、快速恢复 (Fast Recovery / PRR)

### 4.1 设计原理

Linux 使用 **PRR (Proportional Rate Reduction, RFC 6937)** 取代了传统的 RFC 3517 快速恢复。PRR 的核心思想：在恢复阶段**平滑地**将 cwnd 从 `prior_cwnd` 降至 `ssthresh`，而不是一步到位，从而保持数据流的平稳。

### 4.2 PRR 核心实现

```c
// net/ipv4/tcp_input.c
void tcp_cwnd_reduction(struct sock *sk, int newly_acked_sacked,
                        int newly_lost, int flag)
{
    struct tcp_sock *tp = tcp_sk(sk);
    int sndcnt = 0;
    int delta = tp->snd_ssthresh - tcp_packets_in_flight(tp);

    if (newly_acked_sacked <= 0 || WARN_ON_ONCE(!tp->prior_cwnd))
        return;

    tp->prr_delivered += newly_acked_sacked;

    if (delta < 0) {
        // 情况1：in_flight > ssthresh（仍需减小）
        // 按比例减小：sndcnt = ssthresh * prr_delivered / prior_cwnd - prr_out
        u64 dividend = (u64)tp->snd_ssthresh * tp->prr_delivered +
                       tp->prior_cwnd - 1;
        sndcnt = div_u64(dividend, tp->prior_cwnd) - tp->prr_out;
    } else if (flag & FLAG_SND_UNA_ADVANCED && !newly_lost) {
        // 情况2：in_flight <= ssthresh 且有进展无新丢包
        // 包守恒 + 适度加速恢复
        sndcnt = min_t(int, delta,
                       max_t(int, tp->prr_delivered - tp->prr_out,
                             newly_acked_sacked) + 1);
    } else {
        // 情况3：保守策略
        sndcnt = min(delta, newly_acked_sacked);
    }

    // 首次进入恢复时至少允许发 1 个包（触发快速重传）
    sndcnt = max(sndcnt, (tp->prr_out ? 0 : 1));

    // 设置 cwnd = 当前在途 + 允许发送数
    tcp_snd_cwnd_set(tp, tcp_packets_in_flight(tp) + sndcnt);
}
```

### 4.3 PRR 三种情况图示

```
  cwnd
   ▲
   │  prior_cwnd ─────┐
   │                   ╲  情况1: delta < 0
   │                    ╲ in_flight > ssthresh
   │                     ╲ 按比例平滑减小
   │  ssthresh ───────────╲────────────────────
   │                       ╲  情况2: delta >= 0
   │                        ╲ in_flight <= ssthresh
   │                         └─ 包守恒 + 加速恢复
   │
   │              恢复完成 →  cwnd = ssthresh
   └─────────────────────────────────────────── ACK 序号 →
         high_seq 被 ACK 时恢复结束
```

### 4.4 PRR 的关键变量

| 变量 | 初始化 | 更新时机 | 含义 |
|------|--------|----------|------|
| `prior_cwnd` | 进入恢复时的 cwnd | 不变 | PRR 比例计算的分母 |
| `prr_delivered` | 0 | 每次 ACK 累加 `newly_acked_sacked` | 恢复期间确认的总段数 |
| `prr_out` | 0 | 发送时累加 `sent_pkts` | 恢复期间发出的总段数（新数据+重传） |
| `high_seq` | 进入时的 `snd_nxt` | 不变 | 恢复目标：ACK 越过此值即完成 |
| `snd_ssthresh` | `ca_ops->ssthresh()` | 不变 | PRR 的目标 cwnd |

### 4.5 `prr_out` 的更新位置

在实际发送时（不管是新数据还是重传），`prr_out` 才会增加：

```c
// net/ipv4/tcp_output.c: tcp_write_xmit() 末尾
if (tcp_in_cwnd_reduction(sk))
    tp->prr_out += sent_pkts;

// net/ipv4/tcp_output.c: tcp_xmit_retransmit_queue() 中
if (tcp_in_cwnd_reduction(sk))
    tp->prr_out += tcp_skb_pcount(skb);
```

### 4.6 重传队列处理

PRR 设置的 cwnd 直接限制了重传速率：

```c
// net/ipv4/tcp_output.c: tcp_xmit_retransmit_queue()
skb_rbtree_walk_from(skb) {
    segs = tcp_snd_cwnd(tp) - tcp_packets_in_flight(tp);
    if (segs <= 0)
        break;        // cwnd 用满，不能再发

    if (!(sacked & TCPCB_LOST))
        continue;     // 只重传标记为 LOST 的包

    tcp_retransmit_skb(sk, skb, segs);  // 实际重传

    if (tcp_in_cwnd_reduction(sk))
        tp->prr_out += tcp_skb_pcount(skb);
}
```

### 4.7 恢复结束

当 `snd_una` 越过 `high_seq` 时，恢复完成：

```c
// net/ipv4/tcp_input.c: tcp_fastretrans_alert() 中
case TCP_CA_Recovery:
    if (tcp_try_undo_recovery(sk))   // 尝试撤销（虚假恢复）
        return;
    tcp_end_cwnd_reduction(sk);       // 结束：cwnd = ssthresh
    break;
```

```c
static inline void tcp_end_cwnd_reduction(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);

    if (inet_csk(sk)->icsk_ca_ops->cong_control)
        return;    // BBR 等自主管理，不干预

    // 将 cwnd 设为 ssthresh
    if (tp->snd_ssthresh < TCP_INFINITE_SSTHRESH &&
        (inet_csk(sk)->icsk_ca_state == TCP_CA_CWR || tp->undo_marker)) {
        tcp_snd_cwnd_set(tp, tp->snd_ssthresh);
    }
    tcp_ca_event(sk, CA_EVENT_COMPLETE_CWR);
}
```

### 4.8 撤销机制

如果内核发现丢包判断是错误的（如乱序而非真正丢包），可以撤销恢复：

```c
// net/ipv4/tcp_input.c
static void tcp_undo_cwnd_reduction(struct sock *sk, bool unmark_loss)
{
    struct tcp_sock *tp = tcp_sk(sk);

    if (unmark_loss) {
        // 清除所有 LOST 标记
        skb_rbtree_walk(skb, &sk->tcp_rtx_queue) {
            TCP_SKB_CB(skb)->sacked &= ~TCPCB_LOST;
        }
        tp->lost_out = 0;
    }

    if (tp->prior_ssthresh) {
        // 恢复 cwnd（调用算法的 undo_cwnd）
        tcp_snd_cwnd_set(tp, icsk->icsk_ca_ops->undo_cwnd(sk));

        // 恢复 ssthresh
        if (tp->prior_ssthresh > tp->snd_ssthresh)
            tp->snd_ssthresh = tp->prior_ssthresh;
    }
    tp->undo_marker = 0;
}
```

---

## 五、四大机制完整调用链路

```
收到 ACK
  │
  ▼
tcp_ack()                                  ← ACK 处理入口
  │
  ├─ tcp_clean_rtx_queue()                ← 清理已确认的重传队列
  │    └─ tcp_rate_skb_delivered()         ← 速率采样
  │
  ├─ tcp_fastretrans_alert()              ← ★ 拥塞状态机
  │    │
  │    ├─ [Open/Disorder] tcp_identify_packet_loss()
  │    │    ├─ [RACK] tcp_rack_mark_lost() → 标记 LOST
  │    │    └─ [Reno] tcp_newreno_mark_lost()
  │    │
  │    ├─ tcp_time_to_recover()?
  │    │    ├─ lost_out > 0 → true
  │    │    └─ sacked_out+1 > reordering(3) → true
  │    │
  │    ├─ [进入恢复] tcp_enter_recovery()
  │    │    └─ tcp_init_cwnd_reduction()
  │    │         └─ ssthresh = ca_ops->ssthresh()   ← 拥塞发生
  │    │              CUBIC: cwnd * 0.7
  │    │              Reno:  cwnd / 2
  │    │
  │    ├─ [Recovery 中] 继续 PRR + 丢包识别
  │    │
  │    └─ [恢复结束] snd_una >= high_seq
  │         ├─ tcp_try_undo_recovery()    ← 虚假恢复撤销
  │         └─ tcp_end_cwnd_reduction()   ← cwnd = ssthresh
  │
  ├─ tcp_rate_gen()                       ← 生成速率样本
  │
  ├─ tcp_cong_control()                   ← ★ 窗口调整核心
  │    ├─ [有 cong_control] → BBR 自主控制
  │    ├─ [Recovery/CWR 中] → tcp_cwnd_reduction()      ← 快速恢复 PRR
  │    └─ [正常状态] → tcp_cong_avoid()
  │         └─ ca_ops->cong_avoid()
  │              ├─ tcp_in_slow_start()? → tcp_slow_start()    ← 慢启动
  │              └─ else → tcp_cong_avoid_ai()                  ← 拥塞避免
  │
  └─ tcp_xmit_recovery()                 ← 触发重传/新数据发送
       └─ tcp_xmit_retransmit_queue()     ← 按 cwnd 限速重传 LOST 包

RTO 超时（另一条路径）:
  tcp_retransmit_timer()
    └─ tcp_enter_loss()                   ← 拥塞发生（严重）
         ├─ tcp_timeout_mark_lost()       ← 所有在途包标 LOST
         ├─ ssthresh = ca_ops->ssthresh()
         ├─ cwnd = in_flight + 1
         └─ 状态 → TCP_CA_Loss + 慢启动
```

## 六、四大机制对应的 `snd_cwnd` 变化总结

```
cwnd
 ▲
 │     ╱╲ prior_cwnd
 │    ╱  ╲
 │   ╱    ╲  ① 拥塞发生
 │  ╱  ①   ╲──────────╲
 │ ╱        ② PRR 平滑  ╲ ssthresh = cwnd*β
 │╱    ②     下降          ╲
 │─────────────────────────╲───────── ssthresh
 │  ③ 慢启动                ╲③
 │  (指数增长)                ╲
 │                    ④ 拥塞避免
 │                    (线性增长)
 │
 └──────────────────────────────────── 时间 →

 阶段 ①: tcp_init_cwnd_reduction() → ssthresh = ca_ops->ssthresh()
 阶段 ②: tcp_cwnd_reduction() (PRR)  → cwnd 平滑减至 ssthresh
 阶段 ③: tcp_slow_start()            → cwnd 指数增长至 ssthresh
 阶段 ④: tcp_cong_avoid_ai()         → cwnd 线性增长（每 RTT +1）
```
