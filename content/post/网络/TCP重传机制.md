+++
date = '2026-04-29'
title = 'TCP 重传机制深度分析'
weight = 20
tags = [
    "TCP",
    "RTO",
    "RACK",
    "TLP",
    "F-RTO",
    "SACK",
    "重传",
    "tcp_retransmit_timer",
    "tcp_retransmit_skb",
]
categories = [
    "网络",
]
+++
# TCP 重传机制深度分析

本文基于 **Linux 5.15.78** 内核源码，从 RTO 估计、重传定时器、RACK/TLP/F-RTO、SACK 记分板到完整调用链，系统梳理发送端如何判断丢包、何时重传、以及如何与拥塞控制协同。文中片段均来自本仓库对应文件，行号与当前树一致。

---

## 第一章：RTO 计算（平滑 RTT 与 Jacobson/Karels）

TCP 重传超时的核心是 **RTO（Retransmission Timeout）**。RFC 6298 建议在获得有效 RTT 样本后用平滑 RTT 与 RTT 方差估计 RTO；Linux 的实现继承了 Van Jacobson 在 SIGCOMM 88 中的思路，并在 `struct tcp_sock` 中以微秒和固定缩放存储中间量。

### 1.1 超时相关的宏定义

`include/net/tcp.h` 定义了 RTO 的上下界与初始值（单位均为 jiffies，随 `HZ` 变化）：

```c
// include/net/tcp.h:141-144
#define TCP_RTO_MAX	((unsigned)(120*HZ))
#define TCP_RTO_MIN	((unsigned)(HZ/5))
#define TCP_TIMEOUT_MIN	(2U) /* Min timeout for TCP timers in jiffies */
#define TCP_TIMEOUT_INIT ((unsigned)(1*HZ))	/* RFC6298 2.1 initial RTO value	*/
```

要点简述：

- **TCP_TIMEOUT_INIT**：无可靠 RTT 时的初始 RTO（RFC 6298 2.1）。
- **TCP_RTO_MIN / TCP_RTO_MAX**：将内核算出的 RTO 限制在工程上可接受的范围；例如 `TCP_RTO_MAX` 与「最长可能的 RTT」这类协议讨论中的 120s 量级相对应，具体以 `HZ` 换算为实际秒数。

### 1.2 将估计值写回 `icsk_rto`：`__tcp_set_rto` 与 `tcp_bound_rto`

内核把「微秒级的 srtt + 4×rttvar」转为 jiffies 赋给 `inet_csk(sk)->icsk_rto`，并对上界做夹紧：

```c
// include/net/tcp.h:669-678
static inline void tcp_bound_rto(const struct sock *sk)
{
	if (inet_csk(sk)->icsk_rto > TCP_RTO_MAX)
		inet_csk(sk)->icsk_rto = TCP_RTO_MAX;
}
```

```c
// include/net/tcp.h:675-678
static inline u32 __tcp_set_rto(const struct tcp_sock *tp)
{
	return usecs_to_jiffies((tp->srtt_us >> 3) + tp->rttvar_us);
}
```

其中 `srtt_us` **左移 3 位存储**（即真实平滑值 = `srtt_us >> 3`），`rttvar_us` 为平滑后的偏差项，二者相加再 `usecs_to_jiffies` 得到本轮「未回溯(backoff)」的 RTO。

`tcp_bound_rto` 只做 **上限** 约束；注释说明下界不必在此再夹到 `TCP_RTO_MIN`，因为当前估计算法本身已保证 RTO 足够大。

### 1.3 `tcp_rtt_estimator()`：Jacobson/Karels 平滑

收到**可用于测量的** RTT 样本后（须满足 Karn 规则，见下节），`tcp_rtt_estimator()` 更新 `srtt_us`、`mdev_us`、`mdev_max_us`、`rttvar_us` 等。核心逻辑如下（节选）：

```c
// tcp_input.c:1174-1240
static void tcp_rtt_estimator(struct sock *sk, long mrtt_us)
{
	struct tcp_sock *tp = tcp_sk(sk);
	long m = mrtt_us;
	u32 srtt = tp->srtt_us;

	if (srtt != 0) {
		m -= (srtt >> 3);
		srtt += m;
		if (m < 0) {
			m = -m;
			m -= (tp->mdev_us >> 2);
			if (m > 0)
				m >>= 3;
		} else {
			m -= (tp->mdev_us >> 2);
		}
		tp->mdev_us += m;
		if (tp->mdev_us > tp->mdev_max_us) {
			tp->mdev_max_us = tp->mdev_us;
			if (tp->mdev_max_us > tp->rttvar_us)
				tp->rttvar_us = tp->mdev_max_us;
		}
		if (after(tp->snd_una, tp->rtt_seq)) {
			if (tp->mdev_max_us < tp->rttvar_us)
				tp->rttvar_us -= (tp->rttvar_us - tp->mdev_max_us) >> 2;
			tp->rtt_seq = tp->snd_nxt;
			tp->mdev_max_us = tcp_rto_min_us(sk);

			tcp_bpf_rtt(sk);
		}
	} else {
		srtt = m << 3;
		tp->mdev_us = m << 1;
		tp->rttvar_us = max(tp->mdev_us, tcp_rto_min_us(sk));
		tp->mdev_max_us = tp->rttvar_us;
		tp->rtt_seq = tp->snd_nxt;

		tcp_bpf_rtt(sk);
	}
	tp->srtt_us = max(1U, srtt);
}
```

解读要点：**首次采样**用大初值稳住 RTO；**`m < 0`** 分支对 RTT 下降时使用更小 `mdev` 增益；**`snd_una` 越过 `rtt_seq`** 时用本窗口 **`mdev_max`** 反哺 **`rttvar`** 并复位 **`mdev_max`**。

### 1.4 `tcp_ack_update_rtt()`：样本来源与 Karn 规则

ACK 路径上，优先使用**基于报文时间的 RTT**，时间戳（TSval/TSecr）作为补充；**若确认范围触及了重传过的数据，则不能用该样本更新 SRTT**（RFC 6298 / Karn）：

```c
// tcp_input.c:3774-3820
static bool tcp_ack_update_rtt(struct sock *sk, const int flag,
			       long seq_rtt_us, long sack_rtt_us,
			       long ca_rtt_us, struct rate_sample *rs)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	if (seq_rtt_us < 0)
		seq_rtt_us = sack_rtt_us;

	if (seq_rtt_us < 0 && tp->rx_opt.saw_tstamp && tp->rx_opt.rcv_tsecr &&
	    flag & FLAG_ACKED) {
		u32 delta = tcp_time_stamp(tp) - tp->rx_opt.rcv_tsecr;

		if (likely(delta < INT_MAX / (USEC_PER_SEC / TCP_TS_HZ))) {
			if (!delta)
				delta = 1;
			seq_rtt_us = delta * (USEC_PER_SEC / TCP_TS_HZ);
			ca_rtt_us = seq_rtt_us;
		}
	}
	rs->rtt_us = ca_rtt_us;
	if (seq_rtt_us < 0)
		return false;

	tcp_update_rtt_min(sk, ca_rtt_us, flag);
	tcp_rtt_estimator(sk, seq_rtt_us);
	tcp_set_rto(sk);

	inet_csk(sk)->icsk_backoff = 0;
	return true;
}
```

另：**`TCP_TIMEOUT_FALLBACK`**（`include/net/tcp.h:145-149`）在无有效 RTT、尤其三次握手因重传弄脏测量时作数据阶段初始 RTO 回退，可与 `TCP_TIMEOUT_INIT` 对照阅读。

### 1.5 `tcp_set_rto()`：综合公式与注释中的工程取舍

```c
// tcp_input.c:1291-1316
static void tcp_set_rto(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	inet_csk(sk)->icsk_rto = __tcp_set_rto(tp);

	tcp_bound_rto(sk);
}
```

函数上方的长注释强调：`rttvar` 过小在真实网络中往往是「幻觉」，因为部分栈 ACK 行为不规则；实现仍采用经典 **RTO = SRTT + 4×RTTVAR** 的精神，但具体微秒/jiffies 换算与 `rttvar` 维护已经集中在 `tcp_rtt_estimator` 与 `__tcp_set_rto`。

---

## 第二章：RTO 定时器与重传

当在途数据未在 `icsk_rto` 内得到足够推进时，`ICSK_TIME_RETRANS` 触发 `tcp_retransmit_timer()`。该路径与「写超时」 policy、`tcp_enter_loss`、指数退回及 **thin stream 线性超时** 交织。

### 2.1 `tcp_write_timeout()`：重试上限与黑洞检测

`tcp_timer.c:231-287`：SYN 与非 SYN 分支分别用 `sysctl_tcp_syn_retries` / `tcp_retries1`+`tcp_retries2` 等界定是否 **`tcp_write_err` 终止**；中间还会触发 **MTU 探测**、**negative dst 建议**、**orphan 资源** 与 **BPF RTO 回调**。返回 1 时 `tcp_retransmit_timer` 直接跳出，不再对本包做 `tcp_retransmit_skb`。

### 2.2 `tcp_retransmit_timer()`：零窗口、`enter_loss`、指数避让

```c
// tcp_timer.c:452-596（主路径摘录）
void tcp_retransmit_timer(struct sock *sk)
{
	// fastopen_rsk / !packets_out 早退略
	tp->tlp_high_seq = 0;

	if (!tp->snd_wnd && !sock_flag(sk, SOCK_DEAD) && /* ... */) {
		// 零窗口：enter_loss + retransmit_skb + 可能 write_err
	}
	// __NET_INC_STATS TCPTIMEOUTS; tcp_write_timeout 失败则 out;

	tcp_enter_loss(sk);
	icsk->icsk_retransmits++;
	if (tcp_retransmit_skb(sk, tcp_rtx_queue_head(sk), 1) > 0) {
		// TCP_RESOURCE_PROBE_INTERVAL 重武装
	}
	icsk->icsk_backoff++;
	// out_reset_timer：thin 流线性 RTO 或 icsk_rto <<= 1
	inet_csk_reset_xmit_timer(sk, ICSK_TIME_RETRANS,
				  tcp_clamp_rto_to_user_timeout(sk), TCP_RTO_MAX);
}
```

要点：

- **零窗口**：不直接把连接判死，而是用重传作为 probe；长时间无对端活动则 `tcp_write_err`。
- **每次 RTO**：调用 `tcp_enter_loss()` 更新记分板与拥塞状态，然后对**重传队列头**做一次 `tcp_retransmit_skb`。
- **本地发送失败**（返回正数/繁忙）：用 `TCP_RESOURCE_PROBE_INTERVAL` 轻度重武装定时器，避免在内存或队列压力下抢资源。
- **指数避让**：`icsk_backoff++` 与 `icsk_rto <<= 1`（实现写成 `min(icsk_rto << 1, TCP_RTO_MAX)`）配套；**thin stream** 在少量重试内改用「按 `__tcp_set_rto` 刷新、不乘二」的线性策略，降低小流量场景的延迟尖刺。

### 2.3 `__tcp_retransmit_skb()` / `tcp_retransmit_skb()`

单段重传的落点在输出路径：

```c
// tcp_output.c:4064-4182 (核心骨架)
int __tcp_retransmit_skb(struct sock *sk, struct sk_buff *skb, int segs)
{
	// MTU 探测、主机队列占用、按 snd_una 修剪、路由 rebuild
	cur_mss = tcp_current_mss(sk);
	avail_wnd = tcp_wnd_end(tp) - TCP_SKB_CB(skb)->seq;

	if (avail_wnd <= 0) {
		if (TCP_SKB_CB(skb)->seq != tp->snd_una)
			return -EAGAIN;
		avail_wnd = cur_mss;
	}
	// 按窗口与 TSO 可能分片、折叠
	// tcp_transmit_skb 发送
	TCP_SKB_CB(skb)->sacked |= TCPCB_EVER_RETRANS;
	// BPF、trace、统计
	return err;
}
```

```c
// tcp_output.c:4198-4222
int tcp_retransmit_skb(struct sock *sk, struct sk_buff *skb, int segs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	int err = __tcp_retransmit_skb(sk, skb, segs);

	if (err == 0) {
		TCP_SKB_CB(skb)->sacked |= TCPCB_RETRANS;
		tp->retrans_out += tcp_skb_pcount(skb);
	}

	if (!tp->retrans_stamp)
		tp->retrans_stamp = tcp_skb_timestamp(skb);

	if (tp->undo_retrans < 0)
		tp->undo_retrans = 0;
	tp->undo_retrans += tcp_skb_pcount(skb);
	return err;
}
```

**TCPCB_EVER_RETRANS** 与 **TCPCB_RETRANS（此处语义上与 SACKED_RETRANS 路径协作）** 共同支撑 Karn、_undo_ 机制与记分板一致性：一旦某 skb 曾作为重传发出，其 ACK 的解释要更谨慎。

---

## 第三章：RACK 丢包检测

**RACK**（Recent ACKnowledgment）用「**时间序**」补充传统 dupthresh/FACK：若某个**更晚发送**的报文已被确认，而较早报文在等待超过 `RACK.RTT + 乱序窗口` 后仍无确认，则判丢。

### 3.1 `struct tcp_rack` 嵌入 `tcp_sock`

```c
// linux/tcp.h:203-213 (位于 struct tcp_sock)
	struct tcp_rack {
		u64 mstamp;
		u32 rtt_us;
		u32 end_seq;
		u32 last_delivered;
		u8 reo_wnd_steps;
#define TCP_RACK_RECOVERY_THRESH 16
		u8 reo_wnd_persist:5,
		   dsack_seen:1,
		   advanced:1;
	} rack;
```

- **`mstamp`/`end_seq`**：当前 RACK 「参考点」——最近被 (S)ACK 且用于推进检测的那段的发送时间与结束序号。
- **`rtt_us`**：与该参考更新相关的一次 RTT 样本，用于 `tcp_rack_skb_timeout`。
- **`reo_wnd_steps` 等**：与乱序容忍、恢复轮次、DSACK 反馈相关（详见 `tcp_rack_update_reo_wnd` 等，文件后半部）。

### 3.2 `tcp_rack_reo_wnd()`：乱序容忍窗口

```c
// tcp_recovery.c:54-73
static u32 tcp_rack_reo_wnd(const struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (!tp->reord_seen) {
		if (inet_csk(sk)->icsk_ca_state >= TCP_CA_Recovery)
			return 0;

		if (tp->sacked_out >= tp->reordering &&
		    !(READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_recovery) &
		      TCP_RACK_NO_DUPTHRESH))
			return 0;
	}

	return min((tcp_min_rtt(tp) >> 2) * tp->rack.reo_wnd_steps,
		   tp->srtt_us >> 3);
}
```

在「未见乱序 / 已等价触发 dupthresh」的条件下将窗口压到 **0**，使 RACK 更激进去匹配快速恢复；否则用 **min_rtt** 缩放步长并以 **srtt/8** 封顶，兼顾路径重排与队列延迟。

### 3.3 `tcp_rack_detect_loss()` / `tcp_rack_mark_lost()`

`tcp_rack_detect_loss()`（`tcp_recovery.c:108-142`）按 **`tsorted_sent_queue`** 时间序.walk：跳过 **已 LOST 且尚未 SACKED_RETRANS** 的节点；对「发送时间早于参考点 `rack.mstamp/end_seq`」的 skb 用 `tcp_rack_skb_timeout()` 比较 **RACK RTT + reo_wnd**；超时则 `tcp_mark_skb_lost` 并脱链，否则累计 **最大 remaining** 供定时器使用。

`tcp_rack_mark_lost()`（`158-176`）仅在 **`rack.advanced`** 为真时清标志并调用检测；若 `remaining` 非零则 **`ICSK_TIME_REO_TIMEOUT`** = `usecs_to_jiffies(remaining) + TCP_TIMEOUT_MIN`。`advanced` 保证仅当 (S)ACK 推进了 RACK 参考点才全表扫描；未到期丢包靠 REO 定时器补判而非干等 RTO。

### 3.4 `tcp_rack_advance()`

```c
// tcp_recovery.c:195-212
void tcp_rack_advance(struct tcp_sock *tp, u8 sacked, u32 end_seq,
		      u64 xmit_time)
{
	u32 rtt_us;

	rtt_us = tcp_stamp_us_delta(tp->tcp_mstamp, xmit_time);
	if (rtt_us < tcp_min_rtt(tp) && (sacked & TCPCB_RETRANS))
		return;
	tp->rack.advanced = 1;
	tp->rack.rtt_us = rtt_us;
	if (tcp_rack_sent_after(xmit_time, tp->rack.mstamp,
				end_seq, tp->rack.end_seq)) {
		tp->rack.mstamp = xmit_time;
		tp->rack.end_seq = end_seq;
	}
}
```

**重传歧义**：若样本 RTT 小于 `min_rtt` 且该 skb 带重传标记，则可能是「原传被查 ACK」而非「重传被承认」，贸然更新参考点会误判，故直接丢弃该次 `advance`。

---

## 第四章：TLP（Tail Loss Probe）

**TLP** 在常规 RTO 之前多发一次 **探测**（新数据优先，否则重传末段），用于改善**尾部单包丢失**场景下的恢复时延。实现与 `sysctl_tcp_early_retrans` 的取值相关（值为 3 或 4 时启用调度逻辑）。

### 4.1 `tcp_schedule_loss_probe()`

```c
// net/ipv4/tcp_output.c:3621-3663
bool tcp_schedule_loss_probe(struct sock *sk, bool advancing_rto)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	u32 timeout, rto_delta_us;
	int early_retrans;

	if (rcu_access_pointer(tp->fastopen_rsk))
		return false;

	early_retrans = READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_early_retrans);
	if ((early_retrans != 3 && early_retrans != 4) ||
	    !tp->packets_out || !tcp_is_sack(tp) ||
	    (icsk->icsk_ca_state != TCP_CA_Open &&
	     icsk->icsk_ca_state != TCP_CA_CWR))
		return false;

	if (tp->srtt_us) {
		timeout = usecs_to_jiffies(tp->srtt_us >> 2);
		if (tp->packets_out == 1)
			timeout += TCP_RTO_MIN;
		else
			timeout += TCP_TIMEOUT_MIN;
	} else {
		timeout = TCP_TIMEOUT_INIT;
	}

	rto_delta_us = advancing_rto ?
			jiffies_to_usecs(inet_csk(sk)->icsk_rto) :
			tcp_rto_delta_us(sk);
	if (rto_delta_us > 0)
		timeout = min_t(u32, timeout, usecs_to_jiffies(rto_delta_us));

	tcp_reset_xmit_timer(sk, ICSK_TIME_LOSS_PROBE, timeout, TCP_RTO_MAX);
	return true;
}
```

注意：**.probe 的基准时间**与 **到下一次 RTO 的剩余时间**取 `min`，保证 TLP 不会晚于原计划 RTO。

### 4.2 `tcp_send_loss_probe()`

```c
// net/ipv4/tcp_output.c:3704-3765
void tcp_send_loss_probe(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *skb;
	int pcount;
	int mss = tcp_current_mss(sk);

	if (tp->tlp_high_seq)
		goto rearm_timer;

	tp->tlp_retrans = 0;
	skb = tcp_send_head(sk);
	if (skb && tcp_snd_wnd_test(tp, skb, mss)) {
		pcount = tp->packets_out;
		tcp_write_xmit(sk, mss, TCP_NAGLE_OFF, 2, GFP_ATOMIC);
		if (tp->packets_out > pcount)
			goto probe_sent;
		goto rearm_timer;
	}
	skb = skb_rb_last(&sk->tcp_rtx_queue);
	if (unlikely(!skb)) {
		WARN_ONCE(tp->packets_out,
			  "invalid inflight: %u state %u cwnd %u mss %d\n",
			  tp->packets_out, sk->sk_state, tcp_snd_cwnd(tp), mss);
		inet_csk(sk)->icsk_pending = 0;
		return;
	}

	if (skb_still_in_host_queue(sk, skb))
		goto rearm_timer;

	pcount = tcp_skb_pcount(skb);
	if (WARN_ON(!pcount))
		goto rearm_timer;

	if ((pcount > 1) && (skb->len > (pcount - 1) * mss)) {
		if (unlikely(tcp_fragment(sk, TCP_FRAG_IN_RTX_QUEUE, skb,
					  (pcount - 1) * mss, mss,
					  GFP_ATOMIC)))
			goto rearm_timer;
		skb = skb_rb_next(skb);
	}

	if (WARN_ON(!skb || !tcp_skb_pcount(skb)))
		goto rearm_timer;

	if (__tcp_retransmit_skb(sk, skb, 1))
		goto rearm_timer;

	tp->tlp_retrans = 1;

probe_sent:
	tp->tlp_high_seq = tp->snd_nxt;

	NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPLOSSPROBES);
	inet_csk(sk)->icsk_pending = 0;
rearm_timer:
	tcp_rearm_rto(sk);
}
```

`tlp_high_seq` 记录探测发出时的 `snd_nxt`，供 **`tcp_process_tlp_ack`** 辨别本轮 TLP 结局。

### 4.3 `tcp_process_tlp_ack()`

```c
// tcp_input.c:4520-4548
static void tcp_process_tlp_ack(struct sock *sk, u32 ack, int flag)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (before(ack, tp->tlp_high_seq))
		return;

	if (!tp->tlp_retrans) {
		tp->tlp_high_seq = 0;
	} else if (flag & FLAG_DSACK_TLP) {
		tp->tlp_high_seq = 0;
	} else if (after(ack, tp->tlp_high_seq)) {
		tcp_init_cwnd_reduction(sk);
		tcp_set_ca_state(sk, TCP_CA_CWR);
		tcp_end_cwnd_reduction(sk);
		tcp_try_keep_open(sk);
		NET_INC_STATS(sock_net(sk),
				LINUX_MIB_TCPLOSSPROBERECOVERY);
	} else if (!(flag & (FLAG_SND_UNA_ADVANCED |
			     FLAG_NOT_DUP | FLAG_DATA_SACKED))) {
		tp->tlp_high_seq = 0;
	}
}
```

分支语义概括：

- **新数据探测被确认**：直接清除 TLP 语境。
- **DSACK 证明探测重复**：无净损失。
- **ACK 跃过 `tlp_high_seq`**：推断**确有丢失**，走一轮 cwnd 减小与统计；细节与 `tcp_init_cwnd_reduction` 等配合。
- **纯 dupack 且无其它进步标志**：说明原数据与探测都到了，**尾部无实质性丢失**。

---

## 第五章：F-RTO（Forward RTO-Recovery）

**F-RTO** 在超时后尝试区分 **真丢包** 与 **伪超时**（例如 suddenly 延迟增大），从而在恢复阶段更谨慎地决定是否立即大量重传。Linux 用 `tp->frto` 布尔嵌入状态机，与 RFC 5682 步骤对应。

### 5.1 标志位定义

```c
// linux/tcp.h:231
		frto        : 1;/* F-RTO (RFC5682) activated in CA_Loss */
```

### 5.2 在 `tcp_enter_loss()` 中置位

`tcp_enter_loss()`（`tcp_input.c:2676-2725`）在 RTO 等路径上调用：先 **`tcp_timeout_mark_lost`** 标记丢失、按条件减半 **ssthresh**、把 **cwnd 收到 in_flight+1**，切换 **`TCP_CA_Loss`** 并记录 **`high_seq`**，最后在尾部置 **`frto`**：

```c
// tcp_input.c:2676-2724（骨架）
void tcp_enter_loss(struct sock *sk)
{
	// tcp_timeout_mark_lost(sk); ssthresh/cwnd 调整略
	tcp_set_ca_state(sk, TCP_CA_Loss);
	tp->high_seq = tp->snd_nxt;
	tcp_ecn_queue_cwr(tp);

	tp->frto = READ_ONCE(net->ipv4.sysctl_tcp_frto) &&
		   (new_recovery || icsk->icsk_retransmits) &&
		   !inet_csk(sk)->icsk_mtup.probe_size;
}
```

**`frto`** 为 `sysctl_tcp_frto` 与 **（首次进入恢复 `new_recovery` 或已有同窗重传计数 `icsk_retransmits`）** 且 **未进行 MTU 探测** 的合取，含义见内核注释 RFC5682 3.1/3.2。

### 5.3 `tcp_process_loss()` 状态机（节选）

`tcp_process_loss()`（`3454-3505`）：先 **`tcp_try_undo_loss`**；`frto` 分支内处理 **ORIG_SACK_ACKED 撤销**、**有 SACK/dupack 时关闭 F-RTO**、**SND.UNA 前进且可发新数据时 `*rexmit=REXMIT_NEW`**；否则若 **recovered** 则 `tcp_try_undo_recovery`；Reno 再维护 `sacked_out`；默认 **`*rexmit=REXMIT_LOST`**。

读者可按 RFC 5682 对照：

- **SACK 路径下** `FLAG_ORIG_SACK_ACKED` 可与 `tcp_try_undo_loss(..., true)` 协作撤销误判。
- **`REXMIT_NEW`**：在 F-RTO 第 2.b 步允许**优先发送新数据**探测路径是否仍能交付，若窗口与写队列条件允许；否则回落传统恢复。
- **`frto` 清零**：一旦出现 **SACK/dupack** 等「证实真丢」的证据，或无法走「发新数据」分支，则终止 F-RTO 试探。

---

## 第六章：SACK 记分板

SACK 将**已选择性确认的字节区间**反映到每个 `sk_buff` 的 `tcp_skb_cb->sacked` 标志上，并维护 `sacked_out`、`lost_out`、`retrans_out` 等聚合量；**快速重传/PRR** 与 **RTO 后的重传队列扫描**都依赖这套记分板。

### 6.1 标志位定义

```c
// include/net/tcp.h:862-865（及紧邻 REPAIRED/EVER_RETRANS）
#define TCPCB_SACKED_ACKED	0x01
#define TCPCB_SACKED_RETRANS	0x02
#define TCPCB_LOST		0x04
```

语义简述：

- **SACKED_ACKED**：该段字节已被对端 SACK 覆盖。
- **SACKED_RETRANS**：该段曾作为「记分板意义上的重传」飞出（与 `TCPCB_RETRANS` 宏组合使用，见同文件）。
- **LOST**：发送端判定该段丢失，等待或正在重传。
- **EVER_RETRANS**：历史上曾经执行过重传发送，用于 Karn/undo/诊断。

### 6.2 `tcp_sacktag_write_queue()`：块解析与队列行走

函数入口与主循环结构如下：

```c
// tcp_input.c:2276-2340 (开头)
static int
tcp_sacktag_write_queue(struct sock *sk, const struct sk_buff *ack_skb,
			u32 prior_snd_una, struct tcp_sacktag_state *state)
{
	struct tcp_sock *tp = tcp_sk(sk);
	// 从 ack_skb 解析 SACK 选项 → sp[]
	state->flag = 0;
	state->reord = tp->snd_nxt;

	if (!tp->sacked_out)
		tcp_highest_sack_reset(sk);

	found_dup_sack = tcp_check_dsack(sk, ack_skb, sp_wire,
					 num_sacks, prior_snd_una, state);

	if (before(TCP_SKB_CB(ack_skb)->ack_seq, prior_snd_una - tp->max_window))
		return 0;

	if (!tp->packets_out)
		goto out;

	// 合法性过滤、排序、与 recv_sack_cache 对齐以跳过已处理前缀
```

后续 `while` 循环调用 `tcp_sacktag_walk` 等，把每个 SACK block 映射到重传树上的 skb，期间更新 `state->reord`、DSACK 撤销 hint、`tcp_highest_sack` 等。尾部做乱序度量与不变式校验：

```c
// tcp_input.c:2457-2470
	if (inet_csk(sk)->icsk_ca_state != TCP_CA_Loss || tp->undo_marker)
		tcp_check_sack_reordering(sk, state->reord, 0);

	tcp_verify_left_out(tp);
out:
	return state->flag;
}
```

### 6.3 `tcp_sacktag_one()`：单段标记与 RACK 协同

`tcp_input.c:1715-1794`：**D-SACK+RETRANS** 路径维护 `undo_retrans` 与 `reord`；若 `end_seq` 未越过 `snd_una` 则返回。对**尚未 SACKED_ACKED** 的区间：`tcp_rack_advance`；区分 **SACKED_RETRANS**（可能与 LOST 一起清计数）与 **首次 SACK 的 hole**（乱序、`FLAG_ORIG_SACK_ACKED`、sack 时间戳）；最后置 **SACKED_ACKED**、`FLAG_DATA_SACKED`、`sacked_out+=pcount`。尾部 **dup_sack+SACKED_RETRANS** 可清除重传标志并减 **`retrans_out`**。

### 6.4 `tcp_update_scoreboard()` 与 `tcp_xmit_retransmit_queue()`

在dupthresh 语义下，根据「有多少段被 SACK 钉在后面」决定是否把**队列头部**标成 `LOST`：

```c
// tcp_input.c:2886-2897
static void tcp_update_scoreboard(struct sock *sk, int fast_rexmit)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (tcp_is_sack(tp)) {
		int sacked_upto = tp->sacked_out - tp->reordering;
		if (sacked_upto >= 0)
			tcp_mark_head_lost(sk, sacked_upto, 0);
		else if (fast_rexmit)
			tcp_mark_head_lost(sk, 1, 1);
	}
}
```

丢包标记完成之后，**真正发出去**常由 `tcp_xmit_retransmit_queue()` 执行：

```c
// tcp_output.c:4232-4304 (骨架)
void tcp_xmit_retransmit_queue(struct sock *sk)
{
	// ...
	skb = tp->retransmit_skb_hint ?: rtx_head;
	max_segs = tcp_tso_segs(sk, tcp_current_mss(sk));
	skb_rbtree_walk_from(skb) {
		// pacing、cwnd 余量 segs
		sacked = TCP_SKB_CB(skb)->sacked;
		// ...
		if (!(sacked & TCPCB_LOST)) {
			// 维护 hole 提示
			continue;
		}

		if (sacked & (TCPCB_SACKED_ACKED|TCPCB_SACKED_RETRANS))
			continue;

		if (tcp_retransmit_skb(sk, skb, segs))
			break;
		// PRR、MIB、重启 RTO 定时器等
	}
}
```

该函数是 **ACK 驱动恢复** 与 **定时器驱动重传** 的交汇：只要在记分板上仍为 `LOST` 且未 `SACKED_ACKED`，就会尝试占用 cwnd 做重传。

---

## 第七章：重传机制完整调用链

本章用 ASCII 图串起两条最常见路径：**纯 ACK 驱动的快速恢复**与 **RTO 定时器驱动**。节点命名采用内核函数名，便于在源码中跳转。

### 7.1 ACK 路径（简化）

```
                    ┌─────────────────┐
                    │   tcp_rcv_*     │
                    │  (established)  │
                    └────────┬────────┘
                             │
                             v
                    ┌─────────────────┐
                    │    tcp_ack()    │
                    └────────┬────────┘
                             │
            ┌────────────────┼────────────────┐
            │                │                │
            v                v                v
   tcp_sacktag_write_queue  RTT 更新     tcp_fastretrans_alert()
   (记分板/SACK/RACK hint)   (tcp_ack_     │
                            update_rtt)    v
                             │      tcp_identify_packet_loss()
                             │      (内含 tcp_rack_mark_lost 等)
                             │                │
                             │                v
                             │      tcp_update_scoreboard()
                             │                │
                             │                v
                             └──────► tcp_xmit_retransmit_queue()
                                    (对 LOST 且未 SACK 段 tcp_retransmit_skb)
```

说明：

- **`tcp_ack`** 内会根据 flag 调用 **SACK 打标签**、**TLP 结束处理**（`tcp_process_tlp_ack`）、**拥塞状态机**（`tcp_fastretrans_alert` 等）。
- **丢包识别**在多种条件下触发：dupack、SACK gap、RACK/REO 定时器等；一旦 `TCPCB_LOST` 置位，**发送侧**由 `tcp_xmit_retransmit_queue` 统一补课。

### 7.2 RTO 路径（简化）

```
          ┌──────────────────────────┐
          │ icsk_retransmit_timer     │
          │ (ICSK_TIME_RETRANS)       │
          └─────────────┬────────────┘
                        v
          ┌──────────────────────────┐
          │ tcp_retransmit_timer()    │
          └─────────────┬────────────┘
                        │
         ┌──────────────┼──────────────┐
         │              │              │
         v              v              v
   零窗口分支    tcp_write_timeout   主路径
   enter_loss+   (策略性终止?)        enter_loss
   retransmit                         retransmit_skb
         │              │              │
         └──────────────┴──────────────┘
                        v
                icsk_backoff++, RTO<<=1
                或 thin 线性 RTO
                        v
                inet_csk_reset_xmit_timer
                (下一轮 RTO)
```

### 7.3 小结表

| 机制 | 主要触发 | 关键函数/文件 |
|------|----------|----------------|
| RTO 估计 | ACK/TS，Karn 过滤 | `tcp_rtt_estimator`, `tcp_ack_update_rtt`, `__tcp_set_rto` |
| RTO 超时 | `ICSK_TIME_RETRANS` | `tcp_retransmit_timer`, `tcp_enter_loss` |
| 单段发送 | 多处 | `__tcp_retransmit_skb`, `tcp_retransmit_skb` |
| RACK | (S)ACK 推进时间参考 | `tcp_recovery.c`：`tcp_rack_*` |
| TLP | Open/CWR + early_retrans | `tcp_schedule_loss_probe`, `tcp_send_loss_probe` |
| F-RTO | Loss 态 + sysctl | `tp->frto`, `tcp_process_loss` |
| SACK 记分 | SACK 选项 | `tcp_sacktag_write_queue`, `tcp_sacktag_one`, `tcp_update_scoreboard` |
| 批量补传 | cwnd 允许 | `tcp_xmit_retransmit_queue` |

---

## 附录：阅读建议

自 **`tcp_ack()`**（`tcp_input.c` 约 4614 行）跟进 SACK 与 **`FLAG_*`**；**`tcp_enter_loss()`** 配合 **`tcp_timeout_mark_lost()`** 理解 RTO 全量标记；**`tcp_recovery.c`** 文件头与 RACK 草案对照。

本文行号均针对本仓库 **Linux 5.15.78** 源码树中对应路径；若你合入其它补丁，请以本地 `grep -n` 为准更新引用。
