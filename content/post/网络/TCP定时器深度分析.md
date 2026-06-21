+++
date = '2026-04-29'
title = 'TCP 定时器体系深度分析'
weight = 21
tags = [
    "TCP",
    "定时器",
    "RTO",
    "延迟ACK",
    "keepalive",
    "pacing",
    "TLP",
    "TIME_WAIT",
    "SYN-ACK重传",
    "压缩ACK",
]
categories = [
    "网络",
]
+++
# TCP 定时器体系深度分析

本文基于 **Linux 5.15.78** 内核源码，梳理 TCP 各类定时器的初始化、内核对象（`timer_list` / `hrtimer`）、回调分发与典型超时来源。所有路径与行号均来自本仓库 `v5.15.78` 树。

---

## 一、定时器初始化

TCP 套接字在协议栈初始化发送路径时会安装：**写定时器**（重传 / 零窗口探测 / TLP / RACK 重排超时共用一根 `icsk_retransmit_timer`）、**延迟 ACK 定时器**（`icsk_delack_timer`）、**保活定时器**（挂在 `sock->sk_timer`），以及两根 **高精度定时器**：**pacing** 与 **压缩 ACK**。

### 1.1 `tcp_init_xmit_timers()`（通用 TCP sock）

```c
// net/ipv4/tcp_timer.c:791-802
void tcp_init_xmit_timers(struct sock *sk)
{
	inet_csk_init_xmit_timers(sk, &tcp_write_timer, &tcp_delack_timer,
				  &tcp_keepalive_timer);
	hrtimer_init(&tcp_sk(sk)->pacing_timer, CLOCK_MONOTONIC,
		     HRTIMER_MODE_ABS_PINNED_SOFT);
	tcp_sk(sk)->pacing_timer.function = tcp_pace_kick;

	hrtimer_init(&tcp_sk(sk)->compressed_ack_timer, CLOCK_MONOTONIC,
		     HRTIMER_MODE_REL_PINNED_SOFT);
	tcp_sk(sk)->compressed_ack_timer.function = tcp_compressed_ack_kick;
}
```

要点：

- 前三类使用 `inet_csk_init_xmit_timers()` 绑定到 `struct inet_connection_sock` / `struct sock` 的 `timer_list`。
- `pacing_timer` 为 `HRTIMER_MODE_ABS_PINNED_SOFT`：绝对到期、与 CPU 亲和的软中断模式回调。
- `compressed_ack_timer` 为 `HRTIMER_MODE_REL_PINNED_SOFT`：相对延迟、`min()` 选取的纳秒量级。

### 1.2 `inet_csk_init_xmit_timers()`（三根基类 `timer_list`）

```c
// net/ipv4/inet_connection_sock.c:649-660
void inet_csk_init_xmit_timers(struct sock *sk,
			       void (*retransmit_handler)(struct timer_list *t),
			       void (*delack_handler)(struct timer_list *t),
			       void (*keepalive_handler)(struct timer_list *t))
{
	struct inet_connection_sock *icsk = inet_csk(sk);

	timer_setup(&icsk->icsk_retransmit_timer, retransmit_handler, 0);
	timer_setup(&icsk->icsk_delack_timer, delack_handler, 0);
	timer_setup(&sk->sk_timer, keepalive_handler, 0);
	icsk->icsk_pending = icsk->icsk_ack.pending = 0;
}
```

TCP 传入的回调分别为 `tcp_write_timer`、`tcp_delack_timer`、`tcp_keepalive_timer`（定义于 `tcp_timer.c`）。保活使用 `sk->sk_timer` 与连接级 `icsk` 结构解耦命名，但语义上仍是「该 sock 的 keepalive」。

### 1.3 `inet_csk_reset_keepalive_timer()`（保活周期重装）

```c
// net/ipv4/inet_connection_sock.c:681-684
void inet_csk_reset_keepalive_timer(struct sock *sk, unsigned long len)
{
	sk_reset_timer(sk, &sk->sk_timer, jiffies + len);
}
```

启用 `SO_KEEPALIVE` 时，`tcp_set_keepalive()` 会首次按 `keepalive_time_when()` 调度；关闭则 `inet_csk_delete_keepalive_timer()` 停止 `sk_timer`（见第五节）。

---

## 二、写定时器分发

**一根** `icsk_retransmit_timer` 通过 `icsk->icsk_pending` 复用，区分 RTO、零窗口探测、TLP、RACK `reo_wnd` 超时。到期后由 `tcp_write_timer()` 在软中断上下文加 `bh_lock_sock`，最终进入 `tcp_write_timer_handler()`。

### 2.1 `tcp_write_timer_handler()`：按 `icsk_pending` 分支

```c
// net/ipv4/tcp_timer.c:600-636
void tcp_write_timer_handler(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	int event;

	if (((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_LISTEN)) ||
	    !icsk->icsk_pending)
		goto out;

	if (time_after(icsk->icsk_timeout, jiffies)) {
		sk_reset_timer(sk, &icsk->icsk_retransmit_timer, icsk->icsk_timeout);
		goto out;
	}

	tcp_mstamp_refresh(tcp_sk(sk));
	event = icsk->icsk_pending;

	switch (event) {
	case ICSK_TIME_REO_TIMEOUT:
		tcp_rack_reo_timeout(sk);
		break;
	case ICSK_TIME_LOSS_PROBE:
		tcp_send_loss_probe(sk);
		break;
	case ICSK_TIME_RETRANS:
		icsk->icsk_pending = 0;
		tcp_retransmit_timer(sk);
		break;
	case ICSK_TIME_PROBE0:
		icsk->icsk_pending = 0;
		tcp_probe_timer(sk);
		break;
	}

out:
	sk_mem_reclaim(sk);
}
```

常量定义在 `include/net/inet_connection_sock.h`：`ICSK_TIME_RETRANS`、`ICSK_TIME_PROBE0`、`ICSK_TIME_LOSS_PROBE`、`ICSK_TIME_REO_TIMEOUT` 等。注意 **RTO 与 PROBE0** 分支在调用具体处理函数前将 `icsk_pending` 清零，避免重复进入；RACK/TLP 由各自实现内部重装定时器。

### 2.2 与用户进程并发：`tcp_write_timer()` 延迟处理

```c
// net/ipv4/tcp_timer.c:638-654
static void tcp_write_timer(struct timer_list *t)
{
	struct inet_connection_sock *icsk =
			from_timer(icsk, t, icsk_retransmit_timer);
	struct sock *sk = &icsk->icsk_inet.sk;

	bh_lock_sock(sk);
	if (!sock_owned_by_user(sk)) {
		tcp_write_timer_handler(sk);
	} else {
		if (!test_and_set_bit(TCP_WRITE_TIMER_DEFERRED, &sk->sk_tsq_flags))
			sock_hold(sk);
	}
	bh_unlock_sock(sk);
	sock_put(sk);
}
```

若 sock 正被用户态持有，则设置 `TCP_WRITE_TIMER_DEFERRED`，稍后在 `tcp_release_cb()` 路径补跑，保证与锁规则一致。

### 2.3 `inet_csk_reset_xmit_timer()`：如何「挂」到写定时器

```c
// include/net/inet_connection_sock.h:245-271
static inline void inet_csk_reset_xmit_timer(struct sock *sk, const int what,
					     unsigned long when,
					     const unsigned long max_when)
{
	struct inet_connection_sock *icsk = inet_csk(sk);

	if (when > max_when) {
		pr_debug("reset_xmit_timer: sk=%p %d when=0x%lx, caller=%p\n",
			 sk, what, when, (void *)_THIS_IP_);
		when = max_when;
	}

	if (what == ICSK_TIME_RETRANS || what == ICSK_TIME_PROBE0 ||
	    what == ICSK_TIME_LOSS_PROBE || what == ICSK_TIME_REO_TIMEOUT) {
		icsk->icsk_pending = what;
		icsk->icsk_timeout = jiffies + when;
		sk_reset_timer(sk, &icsk->icsk_retransmit_timer, icsk->icsk_timeout);
	} else if (what == ICSK_TIME_DACK) {
		icsk->icsk_ack.pending |= ICSK_ACK_TIMER;
		icsk->icsk_ack.timeout = jiffies + when;
		sk_reset_timer(sk, &icsk->icsk_delack_timer, icsk->icsk_ack.timeout);
	} else {
		pr_debug("inet_csk BUG: unknown timer value\n");
	}
}
```

**发送新数据后**重传定时器普遍通过此接口（或 `tcp_reset_xmit_timer()`，见下文 pacing 叠加）以当前 RTO 或探测间隔重装，从而在 ACK 到店时由数据路径取消或再次推远超时点。

---

## 三、重传定时器（RTO）

### 3.1 `tcp_retransmit_timer()` 主流程

```c
// net/ipv4/tcp_timer.c:452-596
void tcp_retransmit_timer(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct net *net = sock_net(sk);
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct request_sock *req;
	struct sk_buff *skb;

	req = rcu_dereference_protected(tp->fastopen_rsk,
					lockdep_sock_is_held(sk));
	if (req) {
		WARN_ON_ONCE(sk->sk_state != TCP_SYN_RECV &&
			     sk->sk_state != TCP_FIN_WAIT1);
		tcp_fastopen_synack_timer(sk, req);
		return;
	}

	if (!tp->packets_out)
		return;

	skb = tcp_rtx_queue_head(sk);
	if (WARN_ON_ONCE(!skb))
		return;

	tp->tlp_high_seq = 0;

	if (!tp->snd_wnd && !sock_flag(sk, SOCK_DEAD) &&
	    !((1 << sk->sk_state) & (TCPF_SYN_SENT | TCPF_SYN_RECV))) {
		// ... 对端窗口缩为 0 的特殊路径：进入 loss、重传、清 dst ...
		if (tcp_jiffies32 - tp->rcv_tstamp > TCP_RTO_MAX) {
			tcp_write_err(sk);
			goto out;
		}
		tcp_enter_loss(sk);
		tcp_retransmit_skb(sk, skb, 1);
		__sk_dst_reset(sk);
		goto out_reset_timer;
	}

	__NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPTIMEOUTS);
	if (tcp_write_timeout(sk))
		goto out;

	// ... 首次重传 MIB 统计 ...

	tcp_enter_loss(sk);

	icsk->icsk_retransmits++;
	if (tcp_retransmit_skb(sk, tcp_rtx_queue_head(sk), 1) > 0) {
		inet_csk_reset_xmit_timer(sk, ICSK_TIME_RETRANS,
					  TCP_RESOURCE_PROBE_INTERVAL,
					  TCP_RTO_MAX);
		goto out;
	}

	icsk->icsk_backoff++;

out_reset_timer:
	if (sk->sk_state == TCP_ESTABLISHED &&
	    (tp->thin_lto || READ_ONCE(net->ipv4.sysctl_tcp_thin_linear_timeouts)) &&
	    tcp_stream_is_thin(tp) &&
	    icsk->icsk_retransmits <= TCP_THIN_LINEAR_RETRIES) {
		icsk->icsk_backoff = 0;
		icsk->icsk_rto = min(__tcp_set_rto(tp), TCP_RTO_MAX);
	} else {
		icsk->icsk_rto = min(icsk->icsk_rto << 1, TCP_RTO_MAX);
	}
	inet_csk_reset_xmit_timer(sk, ICSK_TIME_RETRANS,
				  tcp_clamp_rto_to_user_timeout(sk), TCP_RTO_MAX);
	if (retransmits_timed_out(sk, READ_ONCE(net->ipv4.sysctl_tcp_retries1) + 1, 0))
		__sk_dst_reset(sk);

out:;
}
```

路径要点：

- **Fast Open**：若 `fastopen_rsk` 非空，转 `tcp_fastopen_synack_timer()`，不再走常规数据重传。
- **无在途数据**（`!tp->packets_out`）直接返回，避免空跑。
- **零窗口且非孤儿**：走「窗口探测式」重传分支，与 `ICSK_TIME_PROBE0` 逻辑互补。
- 正常路径：`tcp_write_timeout()` 判是否应放弃连接 → `tcp_enter_loss()` → 递增 `icsk_retransmits` 并重传；本地拥塞导致 `tcp_retransmit_skb` 失败时，用 `TCP_RESOURCE_PROBE_INTERVAL`（`tcp.h`：`HZ/2`）缓和重试。

### 3.2 指数退避：`icsk_rto <<= 1`

在非 thin-linear 模式下，`icsk->icsk_rto = min(icsk->icsk_rto << 1, TCP_RTO_MAX)`（`net/ipv4/tcp_timer.c:587-588`），即每次 **成功安排了一次重传尝试** 后将 RTO 倍增并夹在 `TCP_RTO_MAX`（`include/net/tcp.h`：`120*HZ`）以下。thin 流且满足 `sysctl_tcp_thin_linear_timeouts` 等条件时，则重置 `icsk_backoff` 并用 `__tcp_set_rto(tp)` 线性重算。

### 3.3 `tcp_write_timeout()`：`tcp_retries1` / `tcp_retries2` 与 MTU 黑洞探测

```c
// net/ipv4/tcp_timer.c:231-287
static int tcp_write_timeout(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	struct net *net = sock_net(sk);
	bool expired = false, do_reset;
	int retry_until;

	if ((1 << sk->sk_state) & (TCPF_SYN_SENT | TCPF_SYN_RECV)) {
		if (icsk->icsk_retransmits)
			__dst_negative_advice(sk);
		retry_until = icsk->icsk_syn_retries ? :
			READ_ONCE(net->ipv4.sysctl_tcp_syn_retries);
		expired = icsk->icsk_retransmits >= retry_until;
	} else {
		if (retransmits_timed_out(sk, READ_ONCE(net->ipv4.sysctl_tcp_retries1), 0)) {
			tcp_mtu_probing(icsk, sk);
			__dst_negative_advice(sk);
		}

		retry_until = READ_ONCE(net->ipv4.sysctl_tcp_retries2);
		if (sock_flag(sk, SOCK_DEAD)) {
			const bool alive = icsk->icsk_rto < TCP_RTO_MAX;
			retry_until = tcp_orphan_retries(sk, alive);
			do_reset = alive ||
				!retransmits_timed_out(sk, retry_until, 0);

			if (tcp_out_of_resources(sk, do_reset))
				return 1;
		}
	}
	if (!expired)
		expired = retransmits_timed_out(sk, retry_until,
						icsk->icsk_user_timeout);
	// ... Fast Open 黑洞检测、BPF RTO 回调 ...

	if (expired) {
		tcp_write_err(sk);
		return 1;
	}
	// ... txhash rethink ...
	return 0;
}
```

- **`tcp_retries1`**：与 `retransmits_timed_out()` 结合触发 **路径 MTU 探测**（`tcp_mtu_probing`）与 **负向路由缓存建议**。
- **`tcp_retries2`**：作为 established 等状态 **放弃连接** 的边界（孤儿套接字另有 `tcp_orphan_retries()` 折叠逻辑）。
- **`icsk_user_timeout`**：`TCP_USER_TIMEOUT` 可覆盖/参与「是否 expired」判定。

### 3.4 发送后重装 RTO：`inet_csk_reset_xmit_timer` 与 pacing

封装函数 `tcp_reset_xmit_timer()` 在通用 RTO 上 **叠加 pacing 造成的 jiffies 延迟**，避免排程过早（`include/net/tcp.h`）：

```c
// include/net/tcp.h:1450-1457
static inline void tcp_reset_xmit_timer(struct sock *sk,
					const int what,
					unsigned long when,
					const unsigned long max_when)
{
	inet_csk_reset_xmit_timer(sk, what, when + tcp_pacing_delay(sk),
				  max_when);
}
```

`tcp_pacing_delay()` 由 `tcp_wstamp_ns` 与 `tcp_clock_cache` 推导，保证定时器与节流后的「可发送时刻」一致。

主动打开路径在发出 SYN 后设置首次重传定时器示例：

```c
// net/ipv4/tcp_output.c:4949-4952
	inet_csk_reset_xmit_timer(sk, ICSK_TIME_RETRANS,
				  inet_csk(sk)->icsk_rto, TCP_RTO_MAX);
```

---

## 四、延迟 ACK 定时器

延迟 ACK 的目标是：**合并确认**、**等待反向数据捎带 ACK**。是否延迟由 `__tcp_ack_snd_check()` 决定；一旦决定延迟，`tcp_send_delayed_ack()` 用 `icsk_delack_timer` 调度。

### 4.1 `tcp_delack_timer_handler()`

```c
// net/ipv4/tcp_timer.c:290-325
void tcp_delack_timer_handler(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);

	sk_mem_reclaim_partial(sk);

	if (((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_LISTEN)) ||
	    !(icsk->icsk_ack.pending & ICSK_ACK_TIMER))
		goto out;

	if (time_after(icsk->icsk_ack.timeout, jiffies)) {
		sk_reset_timer(sk, &icsk->icsk_delack_timer, icsk->icsk_ack.timeout);
		goto out;
	}
	icsk->icsk_ack.pending &= ~ICSK_ACK_TIMER;

	if (inet_csk_ack_scheduled(sk)) {
		if (!inet_csk_in_pingpong_mode(sk)) {
			icsk->icsk_ack.ato = min(icsk->icsk_ack.ato << 1, icsk->icsk_rto);
		} else {
			inet_csk_exit_pingpong_mode(sk);
			icsk->icsk_ack.ato      = TCP_ATO_MIN;
		}
		tcp_mstamp_refresh(tcp_sk(sk));
		tcp_send_ack(sk);
		__NET_INC_STATS(sock_net(sk), LINUX_MIB_DELAYEDACKS);
	}

out:
	if (tcp_under_memory_pressure(sk))
		sk_mem_reclaim(sk);
}
```

**ATO 膨胀**：非 pingpong 下定时器「爽约」会将 `ato` 左移一位直至 `icsk_rto`，抑制过于激进的延迟。pingpong 下则退出 pingpong 并 `ato = TCP_ATO_MIN`（`TCP_ATO_MIN` 与 `TCP_DELACK_MIN` 在 `tcp.h` 中随 `HZ` 定义，典型 `HZ=100` 时为 `HZ/25` ≈ 40ms）。

外层 `tcp_delack_timer()` 与写定时器类似，在 `sock_owned_by_user` 时设 `TCP_DELACK_TIMER_DEFERRED`（略）。

### 4.2 `__tcp_ack_snd_check()`：立即 ACK vs 延迟 vs 压缩 ACK

```c
// net/ipv4/tcp_input.c:6571-6628
static void __tcp_ack_snd_check(struct sock *sk, int ofo_possible)
{
	struct tcp_sock *tp = tcp_sk(sk);
	unsigned long rtt, delay;

	    /* More than one full frame received... */
	if (((tp->rcv_nxt - tp->rcv_wup) > inet_csk(sk)->icsk_ack.rcv_mss &&
	     (tp->rcv_nxt - tp->copied_seq < sk->sk_rcvlowat ||
	     __tcp_select_window(sk) >= tp->rcv_wnd)) ||
	    tcp_in_quickack_mode(sk) ||
	    inet_csk(sk)->icsk_ack.pending & ICSK_ACK_NOW) {
send_now:
		tcp_send_ack(sk);
		return;
	}

	if (!ofo_possible || RB_EMPTY_ROOT(&tp->out_of_order_queue)) {
		tcp_send_delayed_ack(sk);
		return;
	}

	if (!tcp_is_sack(tp) ||
	    tp->compressed_ack >= READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_comp_sack_nr))
		goto send_now;

	if (tp->compressed_ack_rcv_nxt != tp->rcv_nxt) {
		tp->compressed_ack_rcv_nxt = tp->rcv_nxt;
		tp->dup_ack_counter = 0;
	}
	if (tp->dup_ack_counter < TCP_FASTRETRANS_THRESH) {
		tp->dup_ack_counter++;
		goto send_now;
	}
	tp->compressed_ack++;
	if (hrtimer_is_queued(&tp->compressed_ack_timer))
		return;

	rtt = tp->rcv_rtt_est.rtt_us;
	if (tp->srtt_us && tp->srtt_us < rtt)
		rtt = tp->srtt_us;

	delay = min_t(unsigned long,
		      READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_comp_sack_delay_ns),
		      rtt * (NSEC_PER_USEC >> 3)/20);
	sock_hold(sk);
	hrtimer_start_range_ns(&tp->compressed_ack_timer, ns_to_ktime(delay),
			       READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_comp_sack_slack_ns),
			       HRTIMER_MODE_REL_PINNED_SOFT);
}
```

- **立即 ACK**：收到的数据量、窗口条件满足、**quickack**、`ICSK_ACK_NOW`。
- **无乱序或调用方不允许 OFO 逻辑**：普通 `tcp_send_delayed_ack()`。
- **SACK + 乱序队列非空**：可能进入 **压缩 ACK** 路径（第九节），否则回到 `send_now`。

### 4.3 `tcp_send_delayed_ack()`：ATO 与上界

```c
// net/ipv4/tcp_output.c:4970-5020
void tcp_send_delayed_ack(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	int ato = icsk->icsk_ack.ato;
	unsigned long timeout;

	if (ato > TCP_DELACK_MIN) {
		const struct tcp_sock *tp = tcp_sk(sk);
		int max_ato = HZ / 2;

		if (inet_csk_in_pingpong_mode(sk) ||
		    (icsk->icsk_ack.pending & ICSK_ACK_PUSHED))
			max_ato = TCP_DELACK_MAX;

		if (tp->srtt_us) {
			int rtt = max_t(int, usecs_to_jiffies(tp->srtt_us >> 3),
					TCP_DELACK_MIN);

			if (rtt < max_ato)
				max_ato = rtt;
		}

		ato = min(ato, max_ato);
	}

	ato = min_t(u32, ato, inet_csk(sk)->icsk_delack_max);
	timeout = jiffies + ato;

	if (icsk->icsk_ack.pending & ICSK_ACK_TIMER) {
		if (time_before_eq(icsk->icsk_ack.timeout, jiffies + (ato >> 2))) {
			tcp_send_ack(sk);
			return;
		}

		if (!time_before(timeout, icsk->icsk_ack.timeout))
			timeout = icsk->icsk_ack.timeout;
	}
	icsk->icsk_ack.pending |= ICSK_ACK_SCHED | ICSK_ACK_TIMER;
	icsk->icsk_ack.timeout = timeout;
	sk_reset_timer(sk, &icsk->icsk_delack_timer, timeout);
}
```

- **pingpong 或 `ICSK_ACK_PUSHED`**：`max_ato` 放宽到 `TCP_DELACK_MAX`（`HZ/5`，典型 200ms）。
- 否则默认 `max_ato = HZ/2`，再用 **平滑 RTT**（`srtt_us >> 3`）收紧。
- 若已有更早的 delack 超时预约，则 **不推迟**已有更早熄火点；若新算出的延迟「几乎到了」也会直接 `tcp_send_ack()`。

### 4.4 `tcp_in_quickack_mode()`：跳过延迟

```c
// net/ipv4/tcp_input.c:492-499
static bool tcp_in_quickack_mode(struct sock *sk)
{
	const struct inet_connection_sock *icsk = inet_csk(sk);
	const struct dst_entry *dst = __sk_dst_get(sk);

	return (dst && dst_metric(dst, RTAX_QUICKACK)) ||
		(icsk->icsk_ack.quick && !inet_csk_in_pingpong_mode(sk));
}
```

路由度量 `RTAX_QUICKACK` 或残留的 **quickack 计数**（且非 pingpong）都会立即触发 ACK。

### 4.5 内存耗尽时的延迟 ACK 退路

`__tcp_send_ack()` 分配 skb 失败时，会退化为 `ICSK_TIME_DACK` 定时器重试（`tcp_output.c:5040-5051`），与 `inet_csk_reset_xmit_timer(..., ICSK_TIME_DACK, ...)` 共用 `icsk_delack_timer`。

---

## 五、Keepalive 定时器

### 5.1 `tcp_set_keepalive()`：用户态开关

```c
// net/ipv4/tcp_timer.c:664-673
void tcp_set_keepalive(struct sock *sk, int val)
{
	if ((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_LISTEN))
		return;

	if (val && !sock_flag(sk, SOCK_KEEPOPEN))
		inet_csk_reset_keepalive_timer(sk, keepalive_time_when(tcp_sk(sk)));
	else if (!val)
		inet_csk_delete_keepalive_timer(sk);
}
```

`SOCK_KEEPOPEN` 与 setsockopt `SO_KEEPALIVE` 对应；开启时首次超时取 **`keepalive_time_when()`**。

### 5.2 时间跨度辅助函数（per-sock 覆盖 sysctl）

```c
// include/net/tcp.h:1642-1664
static inline int keepalive_intvl_when(const struct tcp_sock *tp)
{
	struct net *net = sock_net((struct sock *)tp);

	return tp->keepalive_intvl ? :
		READ_ONCE(net->ipv4.sysctl_tcp_keepalive_intvl);
}

static inline int keepalive_time_when(const struct tcp_sock *tp)
{
	struct net *net = sock_net((struct sock *)tp);

	return tp->keepalive_time ? :
		READ_ONCE(net->ipv4.sysctl_tcp_keepalive_time);
}

static inline int keepalive_probes(const struct tcp_sock *tp)
{
	struct net *net = sock_net((struct sock *)tp);

	return tp->keepalive_probes ? :
		READ_ONCE(net->ipv4.sysctl_tcp_keepalive_probes);
}
```

默认值见 `tcp.h`：`TCP_KEEPALIVE_TIME`（2 小时）、`TCP_KEEPALIVE_PROBES`（9）、`TCP_KEEPALIVE_INTVL`（75s）；命名空间可通过 `ipv4.sysctl_tcp_keepalive_*` 调整。

### 5.3 `tcp_keepalive_timer()` 完整逻辑

```c
// net/ipv4/tcp_timer.c:677-762
static void tcp_keepalive_timer(struct timer_list *t)
{
	struct sock *sk = from_timer(sk, t, sk_timer);
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	u32 elapsed;

	bh_lock_sock(sk);
	if (sock_owned_by_user(sk)) {
		inet_csk_reset_keepalive_timer (sk, HZ/20);
		goto out;
	}

	if (sk->sk_state == TCP_LISTEN) {
		pr_err("Hmm... keepalive on a LISTEN ???\n");
		goto out;
	}

	tcp_mstamp_refresh(tp);
	if (sk->sk_state == TCP_FIN_WAIT2 && sock_flag(sk, SOCK_DEAD)) {
		if (tp->linger2 >= 0) {
			const int tmo = tcp_fin_time(sk) - TCP_TIMEWAIT_LEN;

			if (tmo > 0) {
				tcp_time_wait(sk, TCP_FIN_WAIT2, tmo);
				goto out;
			}
		}
		tcp_send_active_reset(sk, GFP_ATOMIC);
		goto death;
	}

	if (!sock_flag(sk, SOCK_KEEPOPEN) ||
	    ((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_SYN_SENT)))
		goto out;

	elapsed = keepalive_time_when(tp);

	if (tp->packets_out || !tcp_write_queue_empty(sk))
		goto resched;

	elapsed = keepalive_time_elapsed(tp);

	if (elapsed >= keepalive_time_when(tp)) {
		if ((icsk->icsk_user_timeout != 0 &&
		    elapsed >= msecs_to_jiffies(icsk->icsk_user_timeout) &&
		    icsk->icsk_probes_out > 0) ||
		    (icsk->icsk_user_timeout == 0 &&
		    icsk->icsk_probes_out >= keepalive_probes(tp))) {
			tcp_send_active_reset(sk, GFP_ATOMIC);
			tcp_write_err(sk);
			goto out;
		}
		if (tcp_write_wakeup(sk, LINUX_MIB_TCPKEEPALIVE) <= 0) {
			icsk->icsk_probes_out++;
			elapsed = keepalive_intvl_when(tp);
		} else {
			elapsed = TCP_RESOURCE_PROBE_INTERVAL;
		}
	} else {
		elapsed = keepalive_time_when(tp) - elapsed;
	}

	sk_mem_reclaim(sk);

resched:
	inet_csk_reset_keepalive_timer (sk, elapsed);
	goto out;

death:
	tcp_done(sk);

out:
	bh_unlock_sock(sk);
	sock_put(sk);
}
```

要点：

- **FIN_WAIT2 孤儿**：可转 `tcp_time_wait()` 或发 RST `tcp_done()`。
- **空闲判定**：无 `packets_out` 且写队列空才进入 probe 计数逻辑；否则仅 **resched** 完整 idle 周期。
- **探测发送**：`tcp_write_wakeup(sk, LINUX_MIB_TCPKEEPALIVE)`；失败（本地拥塞）时用 `TCP_RESOURCE_PROBE_INTERVAL`。
- **终止条件**：`TCP_USER_TIMEOUT` 与 `icsk_probes_out` 联立，或传统 `keepalive_probes()` 上限。

`keepalive_time_elapsed()`（`tcp.h:1666-1672`）取 `lrcvtime` 与 `rcv_tstamp` 相对 `tcp_jiffies32` 的较小值，避免应用层未读导致误判「有流量」。

### 5.4 `tcp_write_wakeup()` 与探测 skb

Keepalive 与零窗口探测共用发送探测段逻辑，`tcp_write_wakeup()`（`tcp_output.c:5130-5171`）在 **不能发正文** 时调用 `tcp_xmit_probe_skb()`（序列号 `snd_una - 1` 一类技巧，见该文件 5087-5106 行注释）。

---

## 六、Pacing 定时器（`hrtimer`）

### 6.1 `tcp_pace_kick()`：到期后推进发送

```c
// net/ipv4/tcp_output.c:1614-1623
enum hrtimer_restart tcp_pace_kick(struct hrtimer *timer)
{
	struct tcp_sock *tp = container_of(timer, struct tcp_sock, pacing_timer);
	struct sock *sk = (struct sock *)tp;

	tcp_tsq_handler(sk);
	sock_put(sk);

	return HRTIMER_NORESTART;
}
```

回调 **不自动周期重装**；下一次发送窗口由 `tcp_pacing_check()` 在 `tcp_write_xmit()` 中视 `tcp_wstamp_ns` 决定。

### 6.2 `tcp_pacing_check()`：`tcp_write_xmit` 门前的第一道闸

```c
// net/ipv4/tcp_output.c:3254-3274
static bool tcp_pacing_check(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (!tcp_needs_internal_pacing(sk))
		return false;

	if (tp->tcp_wstamp_ns <= tp->tcp_clock_cache)
		return false;

	if (!hrtimer_is_queued(&tp->pacing_timer)) {
		hrtimer_start(&tp->pacing_timer,
			      ns_to_ktime(tp->tcp_wstamp_ns),
			      HRTIMER_MODE_ABS_PINNED_SOFT);
		sock_hold(sk);
	}
	return true;
}
```

`tcp_needs_internal_pacing()`（`tcp.h:1435-1438`）检测 `sk->sk_pacing_status == SK_PACING_NEEDED`；若由 **qdisc fq** 等完全接管，可不走内部 hrtimer。

### 6.3 与 `tcp_write_xmit()` 的交织

```c
// net/ipv4/tcp_output.c:3496-3498 (tcp_write_xmit 内部)
		if (tcp_pacing_check(sk))
			break;
```

发送循环在此处断开，等待 `tcp_pace_kick` → `tcp_tsq_handler` 再次拉起 `tcp_write_xmit`。

### 6.4 BBR 与 `sk_pacing_rate`

拥塞控制算法设置 **字节/秒** 级 `sk->sk_pacing_rate`。例如 BBR 在 `net/ipv4/tcp_bbr.c` 中更新 pacing 速率（如 `sk->sk_pacing_rate = bbr_bw_to_pacing_rate(...)` 等）。**实际包间距** 在 `tcp_update_skb_after_send()`（`tcp_output.c:1638-1661`）按 `skb->len / rate` 累加 `tcp_wstamp_ns`，与定时器到期时间一致。

---

## 七、零窗口探测定时器

对端通告 **零窗口** 且发送方仍有数据想发时，通过 `ICSK_TIME_PROBE0` 路径周期发送窗口探测。

### 7.1 `tcp_probe_timer()`

```c
// net/ipv4/tcp_timer.c:356-400
static void tcp_probe_timer(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct sk_buff *skb = tcp_send_head(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	int max_probes;

	if (tp->packets_out || !skb) {
		icsk->icsk_probes_out = 0;
		icsk->icsk_probes_tstamp = 0;
		return;
	}

	if (!icsk->icsk_probes_tstamp)
		icsk->icsk_probes_tstamp = tcp_jiffies32;
	else if (icsk->icsk_user_timeout &&
		 (s32)(tcp_jiffies32 - icsk->icsk_probes_tstamp) >=
		 msecs_to_jiffies(icsk->icsk_user_timeout))
		goto abort;

	max_probes = READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_retries2);
	if (sock_flag(sk, SOCK_DEAD)) {
		const bool alive = inet_csk_rto_backoff(icsk, TCP_RTO_MAX) < TCP_RTO_MAX;

		max_probes = tcp_orphan_retries(sk, alive);
		if (!alive && icsk->icsk_backoff >= max_probes)
			goto abort;
		if (tcp_out_of_resources(sk, true))
			return;
	}

	if (icsk->icsk_probes_out >= max_probes) {
abort:		tcp_write_err(sk);
	} else {
		tcp_send_probe0(sk);
	}
}
```

注意：若存在 **在途包**（`packets_out`）或无待发 skb（`!tcp_send_head`），探测状态清零并返回，与 RTO 路径分工。

### 7.2 `tcp_send_probe0()`：退避与用户超时钳制

```c
// net/ipv4/tcp_output.c:5182-5214
void tcp_send_probe0(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	struct net *net = sock_net(sk);
	unsigned long timeout;
	int err;

	err = tcp_write_wakeup(sk, LINUX_MIB_TCPWINPROBE);

	if (tp->packets_out || tcp_write_queue_empty(sk)) {
		icsk->icsk_probes_out = 0;
		icsk->icsk_backoff = 0;
		icsk->icsk_probes_tstamp = 0;
		return;
	}

	icsk->icsk_probes_out++;
	if (err <= 0) {
		if (icsk->icsk_backoff < READ_ONCE(net->ipv4.sysctl_tcp_retries2))
			icsk->icsk_backoff++;
		timeout = tcp_probe0_when(sk, TCP_RTO_MAX);
	} else {
		timeout = TCP_RESOURCE_PROBE_INTERVAL;
	}

	timeout = tcp_clamp_probe0_to_user_timeout(sk, timeout);
	tcp_reset_xmit_timer(sk, ICSK_TIME_PROBE0, timeout, TCP_RTO_MAX);
}
```

`tcp_probe0_when()`（`tcp.h:1471-1478`）在 `tcp_probe0_base()`（至少 `TCP_RTO_MIN`）基础上按 `icsk_backoff` 指数放大并封顶 `TCP_RTO_MAX`。`tcp_reset_xmit_timer()` 会加上 **pacing 延迟**，避免与内部 pacing 冲突。

首次.arm 探测的常见入口之一为 `tcp_check_probe_timer()`（`tcp.h:1481-1486`）：在无在途包且无 pending 写定时器事件时 `tcp_reset_xmit_timer(..., ICSK_TIME_PROBE0, tcp_probe0_base(sk), ...)`。

---

## 八、SYN-ACK 重传定时器（半连接 `request_sock`）

监听套接字accept 队列上的 **半连接** 使用 **独立的** `req->rsk_timer`，与 established `tcp_sock` 的 `icsk_retransmit_timer` 不同。

### 8.1 `reqsk_queue_hash_req()`：入 ehash 时启动定时器

```c
// net/ipv4/inet_connection_sock.c:1008-1013
static void reqsk_queue_hash_req(struct request_sock *req,
				 unsigned long timeout)
{
	timer_setup(&req->rsk_timer, reqsk_timer_handler, TIMER_PINNED);
	mod_timer(&req->rsk_timer, jiffies + timeout);
```

### 8.2 `reqsk_timer_handler()` 核心逻辑

```c
// net/ipv4/inet_connection_sock.c:890-1003
static void reqsk_timer_handler(struct timer_list *t)
{
	struct request_sock *req = from_timer(req, t, rsk_timer);
	// ... listener 迁移、max_syn_ack_retries 动态压缩 ...

	max_syn_ack_retries = icsk->icsk_syn_retries ? :
		READ_ONCE(net->ipv4.sysctl_tcp_synack_retries);
	// ... qlen/young 杠杆可能压低 max_syn_ack_retries ...

	syn_ack_recalc(req, max_syn_ack_retries, READ_ONCE(queue->rskq_defer_accept),
		       &expire, &resend);
	req->rsk_ops->syn_ack_timeout(req);
	if (!expire &&
	    (!resend ||
	     !inet_rtx_syn_ack(sk_listener, req) ||
	     inet_rsk(req)->acked)) {
		unsigned long timeo;

		if (req->num_timeout++ == 0)
			atomic_dec(&queue->young);
		timeo = min(TCP_TIMEOUT_INIT << req->num_timeout, TCP_RTO_MAX);
		mod_timer(&req->rsk_timer, jiffies + timeo);

		// ... migrated nreq 成功插入 ehash 的分支 ...
		return;
	}

	// ... 迁移失败清理 ...

drop:
	inet_csk_reqsk_queue_drop_and_put(oreq->rsk_listener, oreq);
}
```

- **`syn_ack_recalc()`**（`inet_connection_sock.c:763` 起）：结合 `tcp_defer_accept` 决定 **仅过期丢弃** 还是 **继续重传**。
- **重传间隔**：`TCP_TIMEOUT_INIT << num_timeout` 封顶 `TCP_RTO_MAX`（与数据 RTO 初始 1s 同性质的指数退避刻度）。
- **`req->num_timeout`**：每次实际进入重传调度递增；首次从 **young** 计数迁出。
- **失败归宿**：`inet_csk_reqsk_queue_drop_and_put()` 释放半连接；`tcp_syn_ack_timeout()` 在 TCP 层可做统计（`tcp_timer.c:656-661` `__NET_INC_STATS(...TCPTIMEOUTS)` 由具体路径触发）。

`define TCP_SYNACK_RETRIES` 在 `tcp.h:116-121` 给出协议注释层面的默认尺度；运行时可由 `sysctl_tcp_synack_retries` 覆盖。

---

## 九、压缩 ACK 定时器（`compressed_ack_timer`）

### 9.1 `tcp_compressed_ack_kick()`

```c
// net/ipv4/tcp_timer.c:764-789
static enum hrtimer_restart tcp_compressed_ack_kick(struct hrtimer *timer)
{
	struct tcp_sock *tp = container_of(timer, struct tcp_sock, compressed_ack_timer);
	struct sock *sk = (struct sock *)tp;

	bh_lock_sock(sk);
	if (!sock_owned_by_user(sk)) {
		if (tp->compressed_ack) {
			tp->compressed_ack--;
			tcp_send_ack(sk);
		}
	} else {
		if (!test_and_set_bit(TCP_DELACK_TIMER_DEFERRED,
				      &sk->sk_tsq_flags))
			sock_hold(sk);
	}
	bh_unlock_sock(sk);

	sock_put(sk);

	return HRTIMER_NORESTART;
}
```

每次到期发送 **一个** ACK，并将 `compressed_ack` 计数递减，用于在 **乱序 + SACK** 场景下合并多次确认触发。

### 9.2 `sysctl_tcp_comp_sack_*` 默认值与语义

初始化（`net/ipv4/tcp_ipv4.c`）：

```c
// net/ipv4/tcp_ipv4.c:3960-3962
	net->ipv4.sysctl_tcp_comp_sack_delay_ns = NSEC_PER_MSEC;
	net->ipv4.sysctl_tcp_comp_sack_slack_ns = 100 * NSEC_PER_USEC;
	net->ipv4.sysctl_tcp_comp_sack_nr = 44;
```

- **`tcp_comp_sack_delay_ns`**：**硬顶** 延迟；默认 **1ms**（`NSEC_PER_MSEC`）。与 `__tcp_ack_snd_check()` 中 **RTT/20**（纳秒换算，`rtt * (NSEC_PER_USEC >> 3)/20`）取 **min**。
- **`tcp_comp_sack_nr`**：**批量阈值**，`compressed_ack` 达到后不再压缩、立即走 `send_now`（`tcp_input.c:6599-6600`）。
- **`tcp_comp_sack_slack_ns`**：传给 `hrtimer_start_range_ns()` 的 **范围 slack**（默认 100µs），允许合并少量调度抖动。

### 9.3 与 `__tcp_ack_snd_check()` 的闭环

乱序路径在 `dup_ack_counter` 达到 `TCP_FASTRETRANS_THRESH`（值为 3，`tcp.h:80`）后才累加 `compressed_ack` 并启动 hrtimer；若定时器已在队列中则直接返回，由 **单次 hrtimer** 聚合多次「本可立即发的 ACK」。定时器触发后 `tcp_compressed_ack_kick` 发送 ACK 并递减计数，从而在 **减少 ACK 数** 与 **时延** 之间折中。

---

## 附：TIME_WAIT 与本文定时器的关系

**TIME_WAIT** 不是 `tcp_sock` 上上述 `timer_list`/`hrtimer` 的延长线，而是迁入 **`inet_timewait_sock`** 后由 **timewait 死亡行**（`tcp_death_row`）管理与定时回收。入口如 `tcp_time_wait()`（`net/ipv4/tcp_minisocks.c:308` 起）将状态、序列号窗口等拷入 `tcp_timewait_sock`；收包路径可对 **已有 TIME_WAIT** 连接 `inet_twsk_reschedule(tw, TCP_TIMEWAIT_LEN)`（同文件 293-294 行附近）刷新约 **60s** 寿命（`TCP_TIMEWAIT_LEN`，`tcp.h:123-124`）。故总结表中 **单独一行** 说明其机制与 `tcp_sock` 定时器列不同。

---

## 总结表：定时器类型与回调

| 定时器（语义） | 内核对象 | 回调 / 处理函数 | 典型超时或刻度 |
|---------------|----------|-----------------|----------------|
| 写定时器（RTO / PROBE0 / TLP / RACK reo） | `icsk->icsk_retransmit_timer` (`timer_list`) | `tcp_write_timer` → `tcp_write_timer_handler` → 各分支 | `icsk_rto`、探测 `tcp_probe0_when`、TLP/RACK 各自设置 |
| 延迟 ACK | `icsk->icsk_delack_timer` | `tcp_delack_timer` → `tcp_delack_timer_handler` | `ato`，上界 `HZ/2` 或 `TCP_DELACK_MAX` + RTT |
| 保活 | `sk->sk_timer` | `tcp_keepalive_timer` | `keepalive_time_when` / `keepalive_intvl_when` |
| Pacing | `tp->pacing_timer` (`hrtimer`) | `tcp_pace_kick` → `tcp_tsq_handler` | `tcp_wstamp_ns`（绝对_ns） |
| 压缩 ACK | `tp->compressed_ack_timer` (`hrtimer`) | `tcp_compressed_ack_kick` | `min(sysctl_tcp_comp_sack_delay_ns, RTT/20)` + slack |
| SYN-ACK 半连接 | `req->rsk_timer` | `reqsk_timer_handler` | `TCP_TIMEOUT_INIT << num_timeout`（cap `TCP_RTO_MAX`） |
| TIME_WAIT 回收 | `inet_timewait_sock` 死亡行定时器 | `inet_twsk_kill` / `inet_twsk_reschedule` 等 | 约 `TCP_TIMEWAIT_LEN`（60s 量级） |

---

## 参考文献（源码文件）

- `net/ipv4/tcp_timer.c` — 写/delack/keepalive/压缩 ACK 主体
- `net/ipv4/tcp_output.c`：`tcp_send_delayed_ack`、`tcp_pace_kick`、`tcp_pacing_check`、`tcp_write_xmit`、`tcp_send_probe0`、`tcp_write_wakeup`
- `net/ipv4/tcp_input.c` — `__tcp_ack_snd_check`、`tcp_in_quickack_mode`
- `net/ipv4/inet_connection_sock.c` — `inet_csk_init_xmit_timers`、`inet_csk_reset_keepalive_timer`、`reqsk_timer_handler`
- `include/net/inet_connection_sock.h` — `inet_csk_reset_xmit_timer`、`ICSK_TIME_*`
- `include/net/tcp.h` — 宏与 `keepalive_*`、`tcp_reset_xmit_timer`、`tcp_probe0_when`、`TCP_TIMEWAIT_LEN` 等
