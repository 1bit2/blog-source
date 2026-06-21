+++
date = '2026-04-16'
title = 'TCP RST（Reset）发送场景完整分析'
weight = 25
tags = [
    "TCP",
    "RST",
    "tcp_send_active_reset",
    "RFC5961",
]
categories = [
    "网络",
]
+++
# TCP RST（Reset）发送场景完整分析

## 概述

TCP RST（Reset）是TCP协议中用于立即终止连接或拒绝连接请求的机制。与正常的四次挥手不同，RST可以立即关闭连接，不需要等待确认。

### RST的基本特性

| 特性 | 说明 |
|------|------|
| 立即生效 | 收到RST后连接立即终止 |
| 无需确认 | RST不需要对端ACK |
| 单向通知 | 发送方通知接收方连接无效 |
| 释放资源 | 双方立即释放连接资源 |

### 核心原则

1. **永远不要用RST回复RST**（防止RST风暴）
2. **校验和错误的包不发送RST**（可能是网络传输错误）
3. **RFC5961增强安全性**：RST序列号必须精确匹配

---

## 发送RST的场景分类

### 场景1：端口未监听/连接不存在（最常见）

**代码位置**: `net/ipv4/tcp_ipv4.c` → `tcp_v4_rcv()` → `no_tcp_socket`

```c
no_tcp_socket:
    drop_reason = SKB_DROP_REASON_NO_SOCKET;
    if (!xfrm4_policy_check(NULL, XFRM_POLICY_IN, skb))
        goto discard_it;
    tcp_v4_fill_cb(skb, iph, th);
    if (tcp_checksum_complete(skb)) {
        // 校验和错误，不发送RST
        goto csum_error;
    } else {
        // ★ 发送RST
        tcp_v4_send_reset(NULL, skb);
    }
```

**触发场景**:
- 客户端连接未监听的端口 → 返回 `ECONNREFUSED`
- 服务器重启后收到旧连接的数据包
- NAT表项超时后对端继续发送
- 连接已正常关闭但对端还在发送

**典型抓包示例**:
```
Client -> Server: SYN (dst port 8080)
Server -> Client: RST, ACK          # 端口8080未监听
```

---

### 场景2：LISTEN状态收到带ACK的包

**代码位置**: `net/ipv4/tcp_input.c` → `tcp_rcv_state_process()`

```c
case TCP_LISTEN:
    if (th->ack)
        return 1;  // ★ 触发RST
```

**原因**: 正常的SYN包不应该带ACK标志，这违反TCP协议规范（RFC793）

**触发场景**:
- 网络设备故障导致包损坏
- 恶意构造的异常包
- 协议栈实现错误

---

### 场景3：LISTEN状态的conn_request()失败

**代码位置**: `net/ipv4/tcp_input.c` → `tcp_rcv_state_process()`

```c
case TCP_LISTEN:
    if (th->syn) {
        acceptable = icsk->icsk_af_ops->conn_request(sk, skb) >= 0;
        if (!acceptable)
            return 1;  // ★ 触发RST
    }
```

**触发场景**:

| 原因 | 说明 |
|------|------|
| 半连接队列满 | `tcp_max_syn_backlog` 已达上限 |
| 内存不足 | 无法分配 `request_sock` |
| 安全策略拒绝 | SELinux/AppArmor拒绝连接 |
| 资源限制 | 文件描述符耗尽 |

**相关内核参数**:
```bash
# 查看半连接队列大小
sysctl net.ipv4.tcp_max_syn_backlog

# 查看SYN Cookie状态（队列满时的备用方案）
sysctl net.ipv4.tcp_syncookies
```

---

### 场景4：SYN_SENT状态收到无效的SYN-ACK

**代码位置**: `net/ipv4/tcp_input.c` → `tcp_rcv_synsent_state_process()`

```c
// 检查ACK序列号
if (!after(TCP_SKB_CB(skb)->ack_seq, tp->snd_una) ||
    after(TCP_SKB_CB(skb)->ack_seq, tp->snd_nxt)) {
    goto reset_and_undo;  // ★ return 1 触发RST
}

// PAWS时间戳检查
if (tp->rx_opt.saw_tstamp && tp->rx_opt.rcv_tsecr &&
    !between(tp->rx_opt.rcv_tsecr, tp->retrans_stamp, tcp_time_stamp(tp))) {
    goto reset_and_undo;  // ★ return 1 触发RST
}
```

**触发场景**:

| 条件 | 说明 |
|------|------|
| ACK ≤ ISS | ACK序列号小于等于初始序列号 |
| ACK > SND.NXT | ACK序列号超过发送的最大序列号 |
| PAWS失败 | 时间戳不在合理范围内 |

**调用链**:
```
tcp_v4_do_rcv()
  → tcp_rcv_state_process()
    → tcp_rcv_synsent_state_process()
      → reset_and_undo: return 1
  → goto reset
    → tcp_v4_send_reset()
```

---

### 场景5：SYN_RECV状态的ACK验证失败

**代码位置**: `net/ipv4/tcp_input.c` → `tcp_rcv_state_process()`

```c
// step 5: check the ACK field
acceptable = tcp_ack(sk, skb, FLAG_SLOWPATH | ...) > 0;

if (!acceptable) {
    if (sk->sk_state == TCP_SYN_RECV)
        return 1;  // ★ 触发RST
    tcp_send_challenge_ack(sk, skb);
    goto discard;
}
```

**触发场景**: 三次握手最后一步收到的ACK序列号不匹配

**典型抓包示例**:
```
Client -> Server: SYN
Server -> Client: SYN-ACK
Client -> Server: ACK (ack_seq错误)
Server -> Client: RST
```

---

### 场景6：子socket处理失败

**代码位置**: `net/ipv4/tcp_ipv4.c` → `tcp_v4_do_rcv()`

```c
if (sk->sk_state == TCP_LISTEN) {
    struct sock *nsk = tcp_v4_cookie_check(sk, skb);
    if (nsk != sk) {
        if (tcp_child_process(sk, nsk, skb)) {
            rsk = nsk;
            goto reset;  // ★ tcp_v4_send_reset(nsk, skb)
        }
    }
}
```

**触发场景**:
- SYN Cookie创建的子socket处理失败
- 子socket状态转换错误
- 内存不足导致处理失败

---

### 场景7：FIN_WAIT1状态的异常处理

**代码位置**: `net/ipv4/tcp_input.c` → `tcp_rcv_state_process()`

```c
case TCP_FIN_WAIT1:
    // 场景7.1: SO_LINGER设置为0
    if (tp->linger2 < 0) {
        tcp_done(sk);
        return 1;  // ★ 触发RST
    }
    
    // 场景7.2: close()后收到乱序数据
    if (after(TCP_SKB_CB(skb)->end_seq - th->fin, tp->rcv_nxt)) {
        tcp_done(sk);
        return 1;  // ★ 触发RST
    }
```

**场景7.1: SO_LINGER=0**

应用程序设置：
```c
struct linger sl;
sl.l_onoff = 1;
sl.l_linger = 0;
setsockopt(sockfd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
close(sockfd);  // 立即发送RST，不进行四次挥手
```

**场景7.2: close()后收到数据**

```
应用程序调用 close()
  ↓
内核发送 FIN，进入 FIN_WAIT1
  ↓
收到对端的数据（不是FIN）
  ↓
发送 RST 终止连接
```

---

### 场景8：TIME_WAIT状态的异常处理

**代码位置**: `net/ipv4/tcp_minisocks.c` → `tcp_timewait_state_process()`

```c
// TIME_WAIT状态收到SYN
if (th->syn && !th->rst && !th->ack && !paws_reject) {
    // 检查新连接的ISN是否有效
    if (after(TCP_SKB_CB(skb)->seq, tcptw->tw_rcv_nxt)) {
        return TCP_TW_SYN;  // 允许新连接
    }
}

// 其他情况
return TCP_TW_RST;  // ★ 发送RST
```

**触发场景**:
- TIME_WAIT期间收到非法包
- 收到的SYN序列号不合法（可能是旧连接的重传）
- 收到非SYN的异常包

**TIME_WAIT存在的意义**:
1. 确保最后的ACK被对端收到
2. 防止旧连接的包干扰新连接

---

### 场景9：收到RST后被动重置

**代码位置**: `net/ipv4/tcp_input.c` → `tcp_reset()`

```c
void tcp_reset(struct sock *sk, struct sk_buff *skb)
{
    switch (sk->sk_state) {
    case TCP_SYN_SENT:
        sk->sk_err = ECONNREFUSED;  // 连接被拒绝
        break;
    case TCP_CLOSE_WAIT:
        sk->sk_err = EPIPE;         // 管道破裂
        break;
    case TCP_CLOSE:
        return;                      // 已关闭，忽略
    default:
        sk->sk_err = ECONNRESET;    // 连接重置
    }
    tcp_done(sk);
}
```

**注意**: 这是收到RST的处理，不是发送RST。

---

## tcp_v4_send_reset() 核心实现

**代码位置**: `net/ipv4/tcp_ipv4.c`

```c
static void tcp_v4_send_reset(const struct sock *sk, struct sk_buff *skb)
{
    const struct tcphdr *th = tcp_hdr(skb);
    
    // 1. 永远不要用RST回复RST
    if (th->rst)
        return;
    
    // 2. 非本地目的地址不回复
    if (!sk && skb_rtable(skb)->rt_type != RTN_LOCAL)
        return;
    
    // 3. 构建RST包
    memset(&rep, 0, sizeof(rep));
    rep.th.dest   = th->source;   // 交换源和目的
    rep.th.source = th->dest;
    rep.th.rst    = 1;            // 设置RST标志
    
    // 4. 设置序列号
    if (th->ack) {
        rep.th.seq = th->ack_seq;
    } else {
        rep.th.ack = 1;
        rep.th.ack_seq = htonl(ntohl(th->seq) + th->syn + th->fin +
                               skb->len - (th->doff << 2));
    }
    
    // 5. 发送RST
    ip_send_unicast_reply(...);
    
    // 6. 统计
    __TCP_INC_STATS(net, TCP_MIB_OUTRSTS);
}
```

---

### 场景N：Keepalive 探测超时

**代码位置**: `net/ipv4/tcp_timer.c` → `tcp_keepalive_timer()`

```c
// net/ipv4/tcp_timer.c:723-734
if (elapsed >= keepalive_time_when(tp)) {
    if ((icsk->icsk_user_timeout != 0 &&
        elapsed >= msecs_to_jiffies(icsk->icsk_user_timeout) &&
        icsk->icsk_probes_out > 0) ||
        (icsk->icsk_user_timeout == 0 &&
        icsk->icsk_probes_out >= keepalive_probes(tp))) {
        tcp_send_active_reset(sk, GFP_ATOMIC);   // ★ 发送 RST
        tcp_write_err(sk);                         // 设置 ETIMEDOUT
        goto out;
    }
    if (tcp_write_wakeup(sk, LINUX_MIB_TCPKEEPALIVE) <= 0) {
        icsk->icsk_probes_out++;
        elapsed = keepalive_intvl_when(tp);
    }
}
```

**触发条件**: `icsk_probes_out >= keepalive_probes`（默认 9 次探测均无 ACK 响应）

**执行顺序**（关键）:
1. 定时器触发 → **先检查** `probes_out >= 9` → 未超限 → 发送 keepalive 探测 → `probes_out++`
2. 75 秒后定时器再次触发 → **先检查** `probes_out >= 9` → **已超限** → **发 RST** → 连接终止

这意味着**RST 和最后一次 keepalive 探测之间隔了一个 `keepalive_intvl`（默认 75s）**，在抓包中看起来是"先 keepalive 后 RST"。

**默认参数**:

```c
// include/net/tcp.h:155-157
#define TCP_KEEPALIVE_TIME    (120*60*HZ)    // 空闲 2 小时后开始探测
#define TCP_KEEPALIVE_PROBES  9              // 最多探测 9 次
#define TCP_KEEPALIVE_INTVL   (75*HZ)        // 每次间隔 75 秒
```

从首次探测失败到 RST 的总时间：9 × 75 = 675 秒 ≈ 11 分钟。

**典型场景**: 网络路径中断（如 ARP 学到错误 MAC 导致回程包丢失），对端实际存活但 ACK 无法送达，keepalive 探测次数耗尽后发 RST。

---

## RST场景总结表

| 场景 | 状态 | 触发条件 | 错误码 |
|------|------|----------|--------|
| 端口未监听 | - | 找不到socket | ECONNREFUSED |
| LISTEN收到ACK | LISTEN | th->ack为真 | - |
| conn_request失败 | LISTEN | 队列满/内存不足 | - |
| 无效SYN-ACK | SYN_SENT | ACK序列号错误 | - |
| 无效ACK | SYN_RECV | ACK验证失败 | - |
| 子socket失败 | LISTEN | tcp_child_process返回非0 | - |
| SO_LINGER=0 | FIN_WAIT1 | linger2 < 0 | - |
| close后收数据 | FIN_WAIT1 | 收到乱序数据 | - |
| TIME_WAIT异常 | TIME_WAIT | 非法SYN或其他包 | - |
| **Keepalive 超时** | **ESTABLISHED** | **probes_out >= keepalive_probes** | **ETIMEDOUT** |

---

## 调试与监控

### 1. 查看RST统计

```bash
# 查看发送的RST数量
netstat -s | grep -i reset
# 或
cat /proc/net/snmp | grep Tcp
```

**关键指标**:
- `OutRsts`: 发送的RST数量
- `AttemptFails`: 连接尝试失败数
- `EstabResets`: 已建立连接被重置数

### 2. 抓包分析

```bash
# 抓取所有RST包
tcpdump -i any 'tcp[tcpflags] & tcp-rst != 0'

# 抓取特定端口的RST
tcpdump -i any 'port 8080 and tcp[tcpflags] & tcp-rst != 0'
```

### 3. 使用eBPF跟踪

```bash
# 使用bpftrace跟踪RST发送
bpftrace -e 'kprobe:tcp_v4_send_reset { 
    printf("RST sent from %s\n", comm); 
}'
```

### 4. 内核跟踪点

```bash
# 启用tcp_send_reset跟踪点
echo 1 > /sys/kernel/debug/tracing/events/tcp/tcp_send_reset/enable
cat /sys/kernel/debug/tracing/trace_pipe
```

---

## 常见问题排查

### 问题1: 大量Connection Refused

**可能原因**:
1. 服务未启动
2. 服务监听地址/端口错误
3. 防火墙规则拒绝

**排查步骤**:
```bash
# 检查服务是否监听
ss -tlnp | grep <port>
netstat -tlnp | grep <port>

# 检查防火墙
iptables -L -n
```

### 问题2: 连接频繁被RST

**可能原因**:
1. 应用程序bug导致异常关闭
2. 中间设备（NAT/防火墙）超时
3. 网络不稳定导致重传

**排查步骤**:
```bash
# 查看连接状态
ss -tn state established | head

# 查看超时设置
sysctl net.ipv4.tcp_keepalive_time
sysctl net.ipv4.tcp_keepalive_intvl
sysctl net.ipv4.tcp_keepalive_probes
```

### 问题3: TIME_WAIT过多导致新连接RST

**解决方案**:
```bash
# 允许TIME_WAIT socket重用
sysctl -w net.ipv4.tcp_tw_reuse=1

# 减少TIME_WAIT时间（不推荐）
# 注意：tcp_tw_recycle已在Linux 4.12移除
```

### 问题4: Keepalive 正常但突然收到 RST —— 从源码理解纯 `[R]` 的生成机制

**场景**：抓包中看到 keepalive 探测和 ACK 一直正常交互，突然出现纯 `[R]`（不带 ACK）的 RST。

#### 内核中 RST 的两条路径与标志位差异

Linux 内核发送 RST 有两条完全不同的代码路径，产生不同的标志位：

**路径一：`tcp_send_active_reset()` —— 主动关闭时发 RST**

```c
// net/ipv4/tcp_output.c:4450-4451
tcp_init_nondata_skb(skb, tcp_acceptable_seq(sk),
                     TCPHDR_ACK | TCPHDR_RST);  // 硬编码 [R.] = RST + ACK
```

调用场景：keepalive 超时、应用 `close()` 时缓冲区有未读数据、`SO_LINGER=0`。**固定产生 `[R.]`**。

**路径二：`tcp_v4_send_reset()` —— 被动回复无 socket 匹配的包**

```c
// net/ipv4/tcp_ipv4.c:1052-1058
if (th->ack) {
    rep.th.seq = th->ack_seq;     // 仅设 RST → 纯 [R]
} else {
    rep.th.ack = 1;               // 设 RST + ACK → [R.]
    rep.th.ack_seq = htonl(ntohl(th->seq) + th->syn + th->fin +
                           skb->len - (th->doff << 2));
}
```

标志位**取决于收到的包**：收到带 ACK 的包（如 keepalive `[.]`）→ 回复纯 `[R]`；收到不带 ACK 的包（如 SYN）→ 回复 `[R.]`。

**关键约束 —— `RTN_LOCAL` 检查**：

```c
// net/ipv4/tcp_ipv4.c:1039
if (!sk && skb_rtable(skb)->rt_type != RTN_LOCAL)
    return;  // 包的目标 IP 不是本机地址 → 不发 RST
```

这意味着：**只有当包的目标 IP 是本机某个接口地址时，才会在无 socket 情况下发 RST**。普通交换机/路由器如果不持有该 IP，不会通过此路径发 RST。

#### 纯 `[R]` vs `[R.]` 总结

| 生成函数 | 触发条件 | 标志位 | 是否需要 socket |
|----------|----------|--------|----------------|
| `tcp_send_active_reset()` | 主动关闭（keepalive超时/close/abort） | **`[R.]`** 固定 | **是**（操作已有 socket） |
| `tcp_v4_send_reset()` + 收到 ACK 包 | 收到包但无匹配 socket | **`[R]`** 纯 RST | **否**（走 `no_tcp_socket` 路径） |
| `tcp_v4_send_reset()` + 收到非 ACK 包 | 收到包但无匹配 socket | **`[R.]`** | **否** |

#### 实际 pcap 案例：用绝对序列号证明 RST 来源

双网卡设备 IC801（eth0=192.168.100.82/24, eth1=192.168.20.79/24）因 `arp_ignore=0` 导致 ARP 响应竞争，L0 学到错误 MAC，IC204（192.168.100.123）的回程包走错路径。

**连接建立（绝对序列号）**：

```
IC801 SYN:      82.36057→123.57421 [S]  seq=830744071          ← IC801 ISN
IC204 SYN-ACK:  123.57421→82.36057 [S.] seq=8631213 ack=830744072  ← IC204 ISN
```

数据交换后进入 keepalive：
- IC801 snd_nxt = **830748623**（ISN 830744071 + 4552）
- IC204 snd_nxt = **8633276**（ISN 8631213 + 2063）

**证明一：keepalive RST 的 seq 来自 IC204 的 ack 回复**

```
19:17:06  82→123 [.] ack 8633276             IC204 回复 → 123→82 [.] ack 830748623  ✓
19:17:16  82→123 [.] ack 8633276             IC204 回复 → 123→82 [.] ack 830748623  ✓
          ─── ARP 表翻转，IC204 的回复开始走错路径 ───
19:17:26.266  82→123 [.] ack 8633276         IC204 回复 → 走错路径 → 到达无 socket 设备
19:17:26.267  82→123 [R] seq=830748623 win=0 ← ★ seq 恰好 = IC204 ack 中的 830748623
```

数学关系：**RST.seq = IC204回复.ack_seq = IC801.ISN + 4552 = 830744071 + 4552 = 830748623**

这正是 `tcp_v4_send_reset()` 第 1052 行 `rep.th.seq = th->ack_seq` 的结果：某设备收到 IC204 的 ACK（`ack 830748623`），没有匹配 socket，取其 ack_seq 作为 RST 的 seq。

**证明二：SYN 重连的 RST 遵循同一机制**

连接断开后 IC801 尝试重连，每个 SYN 都被 RST：

```
19:17:32.784  82.34139→123.57421 [S]  seq=1108198297
19:17:32.785  82.34139→123.57421 [R]  seq=1108198298 win=0  ← seq = SYN_seq + 1
19:17:33.786  82.34139→123.57421 [S]  seq=1108198297        ← SYN 重试（1s 退避）
19:17:33.787  82.34139→123.57421 [R]  seq=1108198298 win=0
19:17:35.866  82.34139→123.57421 [S]  seq=1108198297        ← SYN 重试（2s 退避）
19:17:35.867  82.34139→123.57421 [R]  seq=1108198298 win=0
19:17:39.919  82.34139→123.57421 [S]  seq=1108198297        ← SYN 重试（4s 退避）
19:17:39.921  82.34139→123.57421 [R]  seq=1108198298 win=0
```

IC204 收到 SYN 后发 SYN-ACK（`ack = SYN_seq + 1 = 1108198298`），SYN-ACK 带 ACK 标志，走错路径后被 `tcp_v4_send_reset()` 处理：`rep.th.seq = th->ack_seq = 1108198298`。

**证明三：`win` 字段区分两条代码路径**

| 字段 | 纯 `[R]`（19:17:26.267） | `[R.]`（19:17:32.666） |
|------|--------------------------|------------------------|
| seq | 830748623 | 830748623 |
| ack | 无 | 8633276 |
| **win** | **0** | **65535** |
| 来源 | `tcp_v4_send_reset()` | `tcp_send_active_reset()` |

- `tcp_v4_send_reset()` 用 `memset(&rep, 0, sizeof(rep))` 初始化 → **win 固定为 0**
- `tcp_send_active_reset()` 使用已有 socket，经 `tcp_transmit_skb()` 设置 `th->window = min(tp->rcv_wnd, 65535)` → **win = 65535**（IC801 的接收窗口）

整个 pcap 中所有 RST 完美符合此规律：

| 类型 | win | 来源确认 |
|------|-----|---------|
| 30 个纯 `[R]`（82→123） | 全部 = 0 | `tcp_v4_send_reset()`（无 socket） |
| 3 个 `[R.]`（82→123, win=65535） | 65535 | IC801 `tcp_send_active_reset()`（keepalive 超时） |
| 6 个 `[R.]`（123→82, win=28694） | 28694 | IC204 端的 RST |

#### 完整时间线

```
19:17:06  keepalive 正常，IC204 ACK 回程路径正确
19:17:16  keepalive 正常，IC204 ACK 回程路径正确

          ──── ARP 表翻转，IC204 回复开始走错路径 ────

19:17:26.266  IC801 发 keepalive
              IC204 回复 ACK → 走错路径 → 到达无 socket 设备
19:17:26.267  无 socket 设备 tcp_v4_send_reset() → 纯 [R] seq=830748623 win=0
              IC204 收到 [R] → 关闭连接

19:17:28.403  IC801 keepalive（不知 IC204 已关闭，probes_out++）
19:17:30.533  IC801 keepalive（probes_out++，interval ≈ 2s）
19:17:32.666  IC801 probes_out 达到上限 → tcp_send_active_reset() → [R.] win=65535

19:17:32.784  IC801 应用层重连 SYN seq=1108198297
              IC204 SYN-ACK → 走错路径 → 被 RST（seq=1108198298）
19:17:33~39   SYN 重试 3 次（1s→2s→4s 退避），每次都被 RST

19:17:48.897  IC801 换端口重连 SYN seq=1220203279
              同样被 RST（seq=1220203280），重试 4 次
```

1.5 小时的 pcap 中共出现 **4 次 RST 风暴**（17:49, 18:22, 18:34, 19:17），间隔不规则（33min, 12min, 43min），符合 ARP 邻居状态机（`REACHABLE → STALE → DELAY → REACHABLE/FAILED`）的间歇性翻转特征。

#### VRF 导致 IC801 自身生成纯 `[R]` —— 完整源码追踪

生成纯 `[R]` 的设备就是 **IC801 自身**。IC801 收到了包、82 是本机地址，但因为 VRF 隔离导致 socket lookup 失败。

IC801 的网络配置（VRF 隔离）：
- eth0（192.168.100.82）→ enslaved to **vrf-data**
- eth1（192.168.20.79）→ enslaved to **vrf-mgmt**（或主路由表）
- TCP socket 在 vrf-data 上下文中创建 → `sk->sk_bound_dev_if = vrf-data_ifindex`

**`tcp_v4_rcv()` 处理 eth1 上收到的包**：

```c
// net/ipv4/tcp_ipv4.c:2661-2714
int tcp_v4_rcv(struct sk_buff *skb) {
    int sdif = inet_sdif(skb);  // eth1 在 vrf-mgmt → sdif = vrf-mgmt_ifindex
    int dif = inet_iif(skb);    // dif = eth1_ifindex

    // 路由查找：local table(255) 有 192.168.100.82 → RTN_LOCAL ✓
    // 包被交给 TCP 处理

    sk = __inet_lookup_skb(&tcp_hashinfo, skb, ..., sdif, &refcounted);
    // → 内部调用 INET_MATCH → inet_sk_bound_dev_eq()
    if (!sk)
        goto no_tcp_socket;  // ← VRF 不匹配走这里
}
```

**`INET_MATCH` 中四元组匹配但 VRF 检查失败**：

```c
// include/net/inet_hashtables.h:525-553
static inline bool INET_MATCH(..., int dif, int sdif) {
    if (!net_eq(...) || sk->sk_portpair != ports || sk->sk_addrpair != cookie)
        return false;   // 四元组匹配 ✓
    return inet_sk_bound_dev_eq(net, sk->sk_bound_dev_if, dif, sdif);
    //                               ^^^^^^^^^^^^^^^^      ^^^  ^^^^
    //                               vrf-data_ifindex   eth1  vrf-mgmt
}
```

**VRF 匹配的核心判定**：

```c
// include/net/inet_sock.h:144-150
static inline bool inet_bound_dev_eq(bool l3mdev_accept,
                                     int bound_dev_if, int dif, int sdif)
{
    if (!bound_dev_if)                     // socket 未绑定 VRF
        return !sdif || l3mdev_accept;     // VRF 包且 l3mdev_accept=0 → false
    return bound_dev_if == dif || bound_dev_if == sdif;
    //     vrf-data ≠ eth1   &&   vrf-data ≠ vrf-mgmt → false!
}
```

两种场景都失败：

| socket 状态 | `bound_dev_if` | 判断 | 结果 |
|-------------|---------------|------|------|
| 绑定 vrf-data | vrf-data_idx | `vrf-data ≠ eth1 && vrf-data ≠ vrf-mgmt` | **false** |
| 未绑定（通用 socket） | 0 | `sdif(≠0) && l3mdev_accept(=0)` | **false** |

注：`sysctl_tcp_l3mdev_accept` 默认为 0，不允许跨 VRF 匹配 socket。

**关键：RTN_LOCAL 通过但 socket 查找失败**

```
eth1 收到 dst=192.168.100.82 的包
    → ip_rcv → local table (255, 共享) 有 82 → RTN_LOCAL ✓ → 交给 TCP
    → tcp_v4_rcv → socket lookup → VRF 不匹配 → no_tcp_socket
    → tcp_v4_send_reset(NULL, skb) → 纯 [R]
```

local table 是所有 VRF 共享的，所以 RTN_LOCAL 检查通过；但 socket 表的查找带了 VRF 过滤，所以找不到 socket。两者的不一致正是产生纯 `[R]` 的原因。

#### Namespace vs VRF 对比

| 特性 | Network Namespace | VRF |
|------|------------------|-----|
| 内核结构 | `struct net`（独立网络栈） | `struct net_device`（L3 master 设备） |
| socket 表 | **完全隔离**（`net_eq()` 拦截） | **共享**（通过 `sdif` 参数过滤） |
| 路由表 | 完全隔离 | 分离（每个 VRF 一个 table，local table 共享） |
| ARP 表 | 完全隔离 | 共享 |
| 本案例的差异 | namespace 隔离下包到 eth1 不会进入 TCP → 不发 RST | VRF 下包到 eth1 进入 TCP 但找不到 socket → **发 RST** |

#### 完整闭合的因果链

```
1. IC801: eth0(vrf-data, 82), eth1(vrf-mgmt, 79), arp_ignore=0
   同一交换机连接 IC801 两个网口和 IC204

2. IC204 ARP "who has 82?" → 交换机广播
   → eth0 响应 MAC=1c:54:e6:31:c8:27 ✓
   → eth1 也响应 MAC=12:26:ec:68:72:b6 ✗（arp_ignore=0）
   → 交换机 MAC 表可能记录 eth1 的 MAC

3. IC204 发 TCP 包给 82 → 交换机查 MAC 表 → 转发到 eth1 端口
   → IC801 eth1(vrf-mgmt) 收包

4. IC801 内核：
   ip_rcv → local table 有 82 → RTN_LOCAL → 交给 tcp_v4_rcv
   sdif = vrf-mgmt_ifindex
   socket lookup: 四元组匹配 ✓，VRF 匹配 ✗ → no_tcp_socket
   → tcp_v4_send_reset(NULL, skb)
   → th->ack → rep.th.seq = th->ack_seq → 纯 [R] win=0
   → ip_send_unicast_reply(sk=NULL) → 路由查找无 VRF 绑定
   → 主路由表: 192.168.100.123 via eth0 → RST 从 eth0 发出
   （pcap 证据: RST src MAC = 1c:54:e6:31:c8:27 即 eth0 MAC）

5. IC204 收到 [R] → 关闭连接
   IC801 vrf-data 上的 socket 完全不知情 → keepalive 继续
   → 最终 keepalive 超时 → tcp_send_active_reset → [R.] win=65535

注: eth0-only 的 tcpdump 看到"keepalive → 无回复 → 突然 RST"：
   - keepalive 从 eth0 出 → 可见
   - IC204 ACK 到达 eth1（交换机单播只发到目标 MAC 端口）→ eth0 不可见
   - RST 路由走主表从 eth0 出 → 可见
   使用 tcpdump -i any 可同时看到 eth1 上的 IC204 ACK
```

**根本解决**：设置 `arp_ignore=1`，eth1 不再响应 82 的 ARP → 交换机只学 eth0 MAC → 回程包走 eth0 → VRF 匹配 → socket 找到 → 正常处理：
```bash
echo 1 > /proc/sys/net/ipv4/conf/all/arp_ignore
```

---

## 参考资料

1. RFC 793 - Transmission Control Protocol
2. RFC 5961 - Improving TCP's Robustness to Blind In-Window Attacks
3. Linux内核源码: `net/ipv4/tcp_*.c`
4. Stevens, W. Richard. "TCP/IP Illustrated, Volume 1"
