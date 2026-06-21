+++
date = '2026-04-27'
title = 'TCP 发送全路径深度分析——从 send() 到网卡'
weight = 11
tags = [
    "TCP",
    "tcp_sendmsg",
    "tcp_write_xmit",
    "tcp_transmit_skb",
    "ip_queue_xmit",
    "dev_queue_xmit",
    "qdisc",
    "ndo_start_xmit",
    "TSO",
    "GSO",
    "Nagle",
    "拥塞控制",
    "Pacing",
    "EDT",
    "TSQ",
    "ECN",
    "CHECKSUM_PARTIAL",
    "PMTU",
    "SWS",
    "XPS",
    "Scatter-Gather",
    "MSG_ZEROCOPY",
    "重传",
    "发送触发机制",
    "softirq",
    "ksoftirqd",
    "NET_TX_SOFTIRQ",
    "tcp_wfree",
    "tcp_tasklet_func",
    "tcp_release_cb",
    "tcp_data_snd_check",
    "tcp_retransmit_timer",
    "tcp_pace_kick",
]
categories = [
    "网络",
]
+++
# TCP 发送全路径深度分析——从 send() 到网卡

> 基于 Linux 5.15.78 内核源码分析

---

## 目录

- [一、全景调用链](#一全景调用链)
- [二、系统调用入口](#二系统调用入口)
- [三、Socket 层到 TCP 层](#三socket-层到-tcp-层)
- [四、tcp_sendmsg：加锁与入口](#四tcp_sendmsg加锁与入口)
- [五、tcp_sendmsg_locked：数据拷贝与缓冲管理](#五tcp_sendmsg_locked数据拷贝与缓冲管理)
- [六、tcp_push：触发发送](#六tcp_push触发发送)
- [七、tcp_write_xmit：发送主循环与六道关卡](#七tcp_write_xmit发送主循环与六道关卡)
- [八、tcp_transmit_skb：TCP 头构建与下送 IP](#八tcp_transmit_skb-tcp-头构建与下送-ip)
- [九、IP 层处理](#九ip-层处理)
- [十、邻居子系统与 ARP](#十邻居子系统与-arp)
- [十一、设备层：dev_queue_xmit 与 Qdisc](#十一设备层dev_queue_xmit-与-qdisc)
- [十二、驱动层：ndo_start_xmit 到硬件](#十二驱动层ndo_start_xmit-到硬件)
- [十三、GSO/TSO 全路径](#十三gsotso-全路径)
- [十四、发送完成与资源回收](#十四发送完成与资源回收)
- [十五、总结](#十五总结)
- [十六、阻塞与非阻塞 fd 的 TCP 发送差异](#十六阻塞与非阻塞-fd-的-tcp-发送差异)
- [十七、TSO/GSO 与小包聚合发送机制详解](#十七tsogso-与小包聚合发送机制详解)
- [十八、发送路径关键技术点深入补充](#十八发送路径关键技术点深入补充)
- [十九、数据从发送缓冲区到网卡的五种触发机制](#十九数据从发送缓冲区到网卡的五种触发机制)

---

## 一、全景调用链

```
用户空间
  send() / sendto() / sendmsg() / write()
    │
    ▼
════════════════════ 系统调用边界 ════════════════════
    │
    ▼
Socket 层（net/socket.c）
  __sys_sendto() / ____sys_sendmsg() / sock_write_iter()
    → sock_sendmsg()
      → security_socket_sendmsg()（LSM 安全检查）
      → sock_sendmsg_nosec()
        → sock->ops->sendmsg()         ← inet_sendmsg
    │
    ▼
传输层（net/ipv4/tcp.c + tcp_output.c）
  inet_sendmsg()
    → sk->sk_prot->sendmsg()           ← tcp_sendmsg
      → lock_sock()
      → tcp_sendmsg_locked()           ← 数据拷贝到 skb
        → tcp_push()
          → __tcp_push_pending_frames()
            → tcp_write_xmit()         ← 发送主循环（cwnd/rwnd/Nagle/TSO/pacing）
              → tcp_transmit_skb()     ← 构建 TCP 头
                → icsk->icsk_af_ops->send_check()  ← tcp_v4_send_check
                → icsk->icsk_af_ops->queue_xmit()  ← ip_queue_xmit
    │
    ▼
网络层（net/ipv4/ip_output.c）
  ip_queue_xmit() / __ip_queue_xmit()
    → 路由查找（__sk_dst_check / ip_route_output_ports）
    → 构建 IP 头
    → ip_local_out()
      → __ip_local_out()               ← 填 tot_len + 校验和
        → nf_hook(NF_INET_LOCAL_OUT)    ← Netfilter LOCAL_OUT
      → dst_output()                   ← ip_output
    → ip_output()
      → NF_HOOK(NF_INET_POST_ROUTING)  ← Netfilter POST_ROUTING（SNAT）
      → ip_finish_output()
        → __ip_finish_output()         ← GSO/分片判断
          → ip_finish_output2()        ← 邻居子系统
    │
    ▼
邻居子系统（net/core/neighbour.c）
  ip_neigh_for_gw()                    ← ARP 表查找/创建
    → neigh_output()
      → neigh_hh_output()             ← 已缓存 MAC 头
      → neigh_resolve_output()        ← 需要 ARP 解析
    → dev_queue_xmit()
    │
    ▼
设备层（net/core/dev.c + net/sched/sch_generic.c）
  dev_queue_xmit() → __dev_queue_xmit()
    → netdev_core_pick_tx()            ← 选择 TX 队列
    → validate_xmit_skb()             ← GSO 软件分段（如需要）
    ├─ 有 Qdisc：__dev_xmit_skb()
    │   → q->enqueue()                ← 入队
    │   → __qdisc_run() → qdisc_restart() → sch_direct_xmit()
    │     → dev_hard_start_xmit()
    └─ 无 Qdisc：直接 dev_hard_start_xmit()
    │
    ▼
驱动层
  dev_hard_start_xmit()
    → xmit_one()
      → netdev_start_xmit()
        → ops->ndo_start_xmit()       ← 驱动入口
          → DMA 映射 + 填写描述符环
          → 写 doorbell 寄存器          ← 通知硬件发送
```

---

## 二、系统调用入口

### 2.1 send / sendto → `__sys_sendto()`

`send()` 只是 `addr == NULL` 的 `sendto()`：

```c
// net/socket.c:2214
SYSCALL_DEFINE4(send, int, fd, void __user *, buff, size_t, len,
        unsigned int, flags)
{
    return __sys_sendto(fd, buff, len, flags, NULL, 0);
}
```

`__sys_sendto()` 核心流程：

```c
// net/socket.c:2160
int __sys_sendto(int fd, void __user *buff, size_t len, unsigned int flags,
         struct sockaddr __user *addr, int addr_len)
{
    // ① 构建 iov_iter：将用户缓冲区地址/长度填入 iov
    err = import_single_range(WRITE, buff, len, &iov, &msg.msg_iter);

    // ② 通过 fd 获取 struct socket*
    sock = sockfd_lookup_light(fd, &err, &fput_needed);

    // ③ 可选：拷贝目标地址到内核
    if (addr)
        err = move_addr_to_kernel(addr, addr_len, &address);

    // ④ 非阻塞标志合并
    if (sock->file->f_flags & O_NONBLOCK)
        flags |= MSG_DONTWAIT;

    // ⑤ 统一入口
    err = sock_sendmsg(sock, &msg);
}
```

### 2.2 write → VFS → `sock_write_iter()`

`write()` 经 `vfs_write()` → `new_sync_write()` → `call_write_iter()` 到达 socket 的 `write_iter`：

```c
// net/socket.c:150
static const struct file_operations socket_file_ops = {
    .write_iter = sock_write_iter,
    // ...
};
```

`sock_write_iter()` 构造 `msghdr` 后调用 `sock_sendmsg()`：

```c
// net/socket.c:1061
static ssize_t sock_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct socket *sock = file->private_data;
    struct msghdr msg = {.msg_iter = *from, .msg_iocb = iocb};
    // ...
    res = sock_sendmsg(sock, &msg);
}
```

### 2.3 sendmsg → `____sys_sendmsg()`

经 `__sys_sendmsg()` → `___sys_sendmsg()`（拷贝 msghdr、import_iovec）→ `____sys_sendmsg()` → `sock_sendmsg()`。

### 2.4 `sock_sendmsg()`：安全检查 + 协议分派

```c
// net/socket.c:720
static inline int sock_sendmsg_nosec(struct socket *sock, struct msghdr *msg)
{
    int ret = INDIRECT_CALL_INET(sock->ops->sendmsg, inet6_sendmsg,
                     inet_sendmsg, sock, msg, msg_data_left(msg));
    return ret;
}

int sock_sendmsg(struct socket *sock, struct msghdr *msg)
{
    int err = security_socket_sendmsg(sock, msg, msg_data_left(msg));
    return err ?: sock_sendmsg_nosec(sock, msg);
}
```

---

## 三、Socket 层到 TCP 层

### 3.1 `inet_sendmsg()`

```c
// net/ipv4/af_inet.c:913
int inet_sendmsg(struct socket *sock, struct msghdr *msg, size_t size)
{
    struct sock *sk = sock->sk;
    if (unlikely(inet_send_prepare(sk)))  // 自动绑定临时端口
        return -EAGAIN;
    return INDIRECT_CALL_2(sk->sk_prot->sendmsg, tcp_sendmsg, udp_sendmsg,
                   sk, msg, size);
}
```

TCP 的 `struct proto tcp_prot` 绑定：

```c
// net/ipv4/tcp_ipv4.c:3835
struct proto tcp_prot = {
    .sendmsg    = tcp_sendmsg,
    // ...
};
```

---

## 四、tcp_sendmsg：加锁与入口

```c
// net/ipv4/tcp.c:1896
int tcp_sendmsg(struct sock *sk, struct msghdr *msg, size_t size)
{
    lock_sock(sk);                    // 独占 socket
    ret = tcp_sendmsg_locked(sk, msg, size);
    release_sock(sk);
    return ret;
}
```

`lock_sock` 保证 `tcp_sendmsg_locked` 对写队列、`tp->write_seq` 等的独占访问。

---

## 五、tcp_sendmsg_locked：数据拷贝与缓冲管理

### 5.1 整体流程

```
tcp_sendmsg_locked()
  ├─ 连接状态检查（非 ESTABLISHED/CLOSE_WAIT 则等待）
  ├─ FastOpen / repair 分支
  ├─ while (msg_data_left(msg))         ← 主循环
  │   ├─ 获取/分配 skb
  │   │   ├─ tcp_write_queue_tail()     ← 复用队尾 skb
  │   │   └─ sk_stream_alloc_skb()     ← 分配新 skb
  │   │       → tcp_skb_entail()       ← 挂入 sk_write_queue
  │   ├─ 拷贝用户数据到 skb
  │   │   ├─ skb_add_data_nocache()    ← 线性区
  │   │   ├─ skb_copy_to_page_nocache() ← page frags
  │   │   └─ skb_zerocopy_iter_stream() ← 零拷贝
  │   ├─ 更新 tp->write_seq / TCP_SKB_CB(skb)->end_seq
  │   └─ 条件触发 __tcp_push_pending_frames / tcp_push_one
  ├─ out: tcp_push()                    ← 循环结束，推送数据
  └─ wait_for_space:                    ← 缓冲区满
      → tcp_push()（腾出空间）
      → sk_stream_wait_memory()（睡眠等待）
```

### 5.2 skb 分配与入队

```c
// net/ipv4/tcp.c:1615
new_segment:
    if (!sk_stream_memory_free(sk))
        goto wait_for_space;
    skb = sk_stream_alloc_skb(sk, 0, sk->sk_allocation, first_skb);
    if (!skb)
        goto wait_for_space;
    skb->ip_summed = CHECKSUM_PARTIAL;
    tcp_skb_entail(sk, skb);
    copy = size_goal;
```

`tcp_skb_entail()` 将 skb 挂入写队列并做内存记账：

```c
// net/ipv4/tcp.c:706
void tcp_skb_entail(struct sock *sk, struct sk_buff *skb)
{
    struct tcp_skb_cb *tcb = TCP_SKB_CB(skb);
    tcb->seq = tcb->end_seq = tp->write_seq;
    tcb->tcp_flags = TCPHDR_ACK;
    tcp_add_write_queue_tail(sk, skb);
    sk_wmem_queued_add(sk, skb->truesize);
    sk_mem_charge(sk, skb->truesize);
}
```

### 5.3 三种数据拷贝路径

```c
// net/ipv4/tcp.c:1701
if (skb_availroom(skb) > 0 && !zc) {
    // 路径 1：skb 线性区有空间
    err = skb_add_data_nocache(sk, skb, &msg->msg_iter, copy);
} else if (!zc) {
    // 路径 2：page frags（最常见路径，alloc_skb(0) 线性区为空）
    err = skb_copy_to_page_nocache(sk, &msg->msg_iter, skb,
                       pfrag->page, pfrag->offset, copy);
} else {
    // 路径 3：零拷贝（MSG_ZEROCOPY）
    err = skb_zerocopy_iter_stream(sk, skb, msg, copy, uarg);
}
```

### 5.4 循环内触发发送

当 skb 填满 `size_goal` 且 `forced_push()` 成立时，循环内即时发送：

```c
// net/ipv4/tcp.c:1820
if (forced_push(tp)) {
    tcp_mark_push(tp, skb);
    __tcp_push_pending_frames(sk, mss_now, TCP_NAGLE_PUSH);
} else if (skb == tcp_send_head(sk))
    tcp_push_one(sk, mss_now);
```

`forced_push()`：已积累超过半个接收窗口的数据未推送：

```c
// net/ipv4/tcp.c:687
static inline bool forced_push(const struct tcp_sock *tp)
{
    return after(tp->write_seq, tp->pushed_seq + (tp->max_window >> 1));
}
```

### 5.5 发送缓冲区管理

`sk_stream_memory_free()` 检查 `sk_wmem_queued` vs `sk_sndbuf`：

```c
// include/net/sock.h:1316
static inline bool __sk_stream_memory_free(const struct sock *sk, int wake)
{
    if (READ_ONCE(sk->sk_wmem_queued) >= READ_ONCE(sk->sk_sndbuf))
        return false;
    return sk->sk_prot->stream_memory_free ?
        sk->sk_prot->stream_memory_free(sk, wake) : true;
}
```

缓冲区满时先 `tcp_push()` 尝试腾空间，再 `sk_stream_wait_memory()` 睡眠等待：

```c
// net/ipv4/tcp.c:1833
wait_for_space:
    set_bit(SOCK_NOSPACE, &sk->sk_socket->flags);
    if (copied)
        tcp_push(sk, flags & ~MSG_MORE, mss_now, TCP_NAGLE_PUSH, size_goal);
    err = sk_stream_wait_memory(sk, &timeo);
```

---

## 六、tcp_push：触发发送

```c
// net/ipv4/tcp.c:821
void tcp_push(struct sock *sk, int flags, int mss_now,
          int nonagle, int size_goal)
{
    struct sk_buff *skb = tcp_write_queue_tail(sk);
    if (!skb)
        return;

    // ① PSH 标志：无 MSG_MORE 或 forced_push 时设置
    if (!(flags & MSG_MORE) || forced_push(tp))
        tcp_mark_push(tp, skb);

    // ② URG 标志
    tcp_mark_urg(tp, flags);

    // ③ Autocork：发送缓冲区还有其它数据在途，延迟发送以聚合
    if (tcp_should_autocork(sk, skb, size_goal)) {
        if (refcount_read(&sk->sk_wmem_alloc) > skb->truesize)
            return;
    }

    // ④ MSG_MORE → 临时启用 Nagle cork
    if (flags & MSG_MORE)
        nonagle = TCP_NAGLE_CORK;

    // ⑤ 真正推送
    __tcp_push_pending_frames(sk, mss_now, nonagle);
}
```

`__tcp_push_pending_frames()` 调用 `tcp_write_xmit()`：

```c
// net/ipv4/tcp_output.c:3814
void __tcp_push_pending_frames(struct sock *sk, unsigned int cur_mss, int nonagle)
{
    if (unlikely(sk->sk_state == TCP_CLOSE))
        return;
    if (tcp_write_xmit(sk, cur_mss, nonagle, 0, sk_gfp_mask(sk, GFP_ATOMIC)))
        tcp_check_probe_timer(sk);
}
```

---

## 七、tcp_write_xmit：发送主循环与六道关卡

这是 TCP 发送路径中**最核心**的函数，决定哪些 skb 可以发出去。

### 7.1 整体结构

```
tcp_write_xmit()
  ├─ tcp_mstamp_refresh()               ← 刷新时间基准
  ├─ tcp_mtu_probe()                    ← MTU 探测（可选）
  ├─ max_segs = tcp_tso_segs()          ← TSO 最大段数
  └─ while ((skb = tcp_send_head(sk)))   ← 遍历写队列
       ├─ 关卡①: tcp_pacing_check()     ← 发送速率限制
       ├─ 关卡②: tcp_cwnd_test()        ← 拥塞窗口
       ├─ 关卡③: tcp_snd_wnd_test()     ← 接收窗口
       ├─ 关卡④: tcp_nagle_test()       ← Nagle 算法
       ├─ 关卡⑤: tcp_tso_should_defer() ← TSO 延迟聚合
       ├─ 关卡⑥: tcp_small_queue_check() ← 小队列限制
       ├─ tcp_mss_split_point() + tso_fragment()  ← 分段
       ├─ tcp_transmit_skb()            ← 实际发送
       └─ tcp_event_new_data_sent()     ← 移入重传树 + RTO 定时器
```

### 7.2 关卡详解

**关卡① Pacing**（速率限制）：

```c
// net/ipv4/tcp_output.c:3292
static bool tcp_pacing_check(struct sock *sk)
{
    if (!tcp_needs_internal_pacing(sk))
        return false;
    if (tp->tcp_wstamp_ns <= tp->tcp_clock_cache)
        return false;
    // 启动 hrtimer，到时间再发
    hrtimer_start(&tp->pacing_timer, ns_to_ktime(tp->tcp_wstamp_ns), ...);
    return true;
}
```

**关卡② 拥塞窗口**：

```c
// net/ipv4/tcp_output.c:2781
static inline unsigned int tcp_cwnd_test(const struct tcp_sock *tp,
                     const struct sk_buff *skb)
{
    u32 in_flight, cwnd;
    in_flight = tcp_packets_in_flight(tp);  // packets_out - left_out + retrans_out
    cwnd = tcp_snd_cwnd(tp);                // tp->snd_cwnd
    if (in_flight >= cwnd)
        return 0;                           // 拥塞窗口已满
    halfcwnd = max(cwnd >> 1, 1U);
    return min(halfcwnd, cwnd - in_flight);
}
```

**关卡③ 接收窗口**：

```c
// net/ipv4/tcp_output.c:2878
static bool tcp_snd_wnd_test(const struct tcp_sock *tp,
                 const struct sk_buff *skb, unsigned int cur_mss)
{
    u32 end_seq = TCP_SKB_CB(skb)->end_seq;
    if (skb->len > cur_mss)
        end_seq = TCP_SKB_CB(skb)->seq + cur_mss;
    return !after(end_seq, tcp_wnd_end(tp));  // snd_una + snd_wnd
}
```

**关卡④ Nagle 算法**：小包且有在途数据时延迟发送

```c
// net/ipv4/tcp_output.c:2651
static bool tcp_nagle_check(bool partial, const struct tcp_sock *tp, int nonagle)
{
    return partial &&
        ((nonagle & TCP_NAGLE_CORK) ||
         (!nonagle && tp->packets_out && tcp_minshall_check(tp)));
}
```

**关卡⑤ TSO 延迟**：等待更多数据以构建更大的 TSO 段

**关卡⑥ Small Queue Check**：限制每 socket 在设备层的排队量

### 7.3 发送后处理

```c
// net/ipv4/tcp_output.c:100
static void tcp_event_new_data_sent(struct sock *sk, struct sk_buff *skb)
{
    WRITE_ONCE(tp->snd_nxt, TCP_SKB_CB(skb)->end_seq);
    __skb_unlink(skb, &sk->sk_write_queue);       // 从写队列摘下
    tcp_rbtree_insert(&sk->tcp_rtx_queue, skb);    // 插入重传红黑树
    tp->packets_out += tcp_skb_pcount(skb);
    if (!prior_packets || icsk->icsk_pending == ICSK_TIME_LOSS_PROBE)
        tcp_rearm_rto(sk);                         // 设置/重置 RTO 定时器
}
```

---

## 八、tcp_transmit_skb：TCP 头构建与下送 IP

### 8.1 入口

```c
// net/ipv4/tcp_output.c:1981
static int tcp_transmit_skb(struct sock *sk, struct sk_buff *skb,
                int clone_it, gfp_t gfp_mask)
{
    return __tcp_transmit_skb(sk, skb, clone_it, gfp_mask, tcp_sk(sk)->rcv_nxt);
}
```

### 8.2 `__tcp_transmit_skb` 核心流程

```
__tcp_transmit_skb()
  ├─ ① skb_clone / pskb_copy          ← 克隆体下送，原件留在重传树
  ├─ ② 计算 TCP 选项大小
  │   ├─ SYN: tcp_syn_options()
  │   └─ 非 SYN: tcp_established_options()（时间戳/SACK/窗口缩放）
  ├─ ③ skb_push(tcp_header_size)       ← 预留 TCP 头空间
  ├─ ④ 填充 TCP 头字段
  │   ├─ th->source / th->dest        ← 端口
  │   ├─ th->seq / th->ack_seq        ← 序列号/确认号
  │   ├─ th->window                   ← 接收窗口
  │   ├─ tcp_flags                    ← SYN/ACK/FIN/RST/PSH
  │   └─ tcp_options_write()          ← 写入选项字节
  ├─ ⑤ 校验和: tcp_v4_send_check()
  ├─ ⑥ skb_orphan + 设置 destructor  ← 内存记账转移
  └─ ⑦ icsk->icsk_af_ops->queue_xmit() ← ip_queue_xmit
```

关键代码（节选）：

```c
// net/ipv4/tcp_output.c:1755（节选）
static int __tcp_transmit_skb(struct sock *sk, struct sk_buff *skb,
                  int clone_it, gfp_t gfp_mask, u32 rcv_nxt)
{
    // ① 克隆
    if (clone_it) {
        oskb = skb;
        skb = skb_clone(oskb, gfp_mask);
    }

    // ② TCP 头大小
    tcp_header_size = tcp_options_size + sizeof(struct tcphdr);

    // ③ 预留头空间
    skb_push(skb, tcp_header_size);
    skb_reset_transport_header(skb);

    // ④ 填 TCP 头
    th = (struct tcphdr *)skb->data;
    th->source      = inet->inet_sport;
    th->dest        = inet->inet_dport;
    th->seq         = htonl(tcb->seq);
    th->ack_seq     = htonl(rcv_nxt);
    th->window      = htons(min(tp->rcv_wnd, 65535U));

    tcp_options_write((__be32 *)(th + 1), tp, &opts);

    // ⑤ 校验和
    icsk->icsk_af_ops->send_check(sk, skb);  // tcp_v4_send_check

    // ⑦ 下送 IP 层
    err = icsk->icsk_af_ops->queue_xmit(sk, skb, &inet->cork.fl);
    // → ip_queue_xmit()
}
```

IPv4 绑定：

```c
// net/ipv4/tcp_ipv4.c:2982
const struct inet_connection_sock_af_ops ipv4_specific = {
    .queue_xmit    = ip_queue_xmit,
    .send_check    = tcp_v4_send_check,
};
```

---

## 九、IP 层处理

### 9.1 `ip_queue_xmit` → `__ip_queue_xmit`

```c
// net/ipv4/ip_output.c:700（核心路径）
int __ip_queue_xmit(struct sock *sk, struct sk_buff *skb, struct flowi *fl, __u8 tos)
{
    // ① 路由查找
    rt = __sk_dst_check(sk, 0);        // 检查 socket 缓存的路由
    if (!rt) {
        rt = ip_route_output_ports(...);  // 重新查路由
        sk_setup_caps(sk, &rt->dst);     // 更新 socket 能力（TSO/GSO）
    }

    // ② 设置路由到 skb
    skb_dst_set_noref(skb, &rt->dst);

    // ③ 构建 IP 头
    skb_push(skb, sizeof(struct iphdr) + (inet_opt ? inet_opt->opt.optlen : 0));
    skb_reset_network_header(skb);
    iph = ip_hdr(skb);
    *((__be16 *)iph) = htons((4 << 12) | (5 << 8) | (tos & 0xff));
    iph->frag_off = ip_dont_fragment(sk, &rt->dst) ? htons(IP_DF) : 0;
    iph->ttl      = ip_select_ttl(inet, &rt->dst);
    iph->protocol = sk->sk_protocol;
    ip_copy_addrs(iph, fl4);
    ip_select_ident_segs(net, skb, sk, skb_shinfo(skb)->gso_segs ?: 1);

    // ④ 进入 ip_local_out
    res = ip_local_out(net, sk, skb);
}
```

### 9.2 `ip_local_out` → `__ip_local_out`

```c
// net/ipv4/ip_output.c:113
int __ip_local_out(struct net *net, struct sock *sk, struct sk_buff *skb)
{
    iph->tot_len = htons(skb->len);     // 填 IP 总长度（此处才写）
    ip_send_check(iph);                  // IP 头校验和
    skb->protocol = htons(ETH_P_IP);

    // Netfilter LOCAL_OUT 钩子
    return nf_hook(NFPROTO_IPV4, NF_INET_LOCAL_OUT,
               net, sk, skb, NULL, skb_dst(skb)->dev, dst_output);
}
```

### 9.3 `ip_output`：POST_ROUTING

```c
// net/ipv4/ip_output.c:631
int ip_output(struct net *net, struct sock *sk, struct sk_buff *skb)
{
    skb->dev = skb_dst(skb)->dev;
    skb->protocol = htons(ETH_P_IP);

    // Netfilter POST_ROUTING 钩子（SNAT 在此处理）
    return NF_HOOK_COND(NFPROTO_IPV4, NF_INET_POST_ROUTING,
                net, sk, skb, indev, dev,
                ip_finish_output,
                !(IPCB(skb)->flags & IPSKB_REROUTED));
}
```

### 9.4 `ip_finish_output` → `__ip_finish_output`

```c
// net/ipv4/ip_output.c:470
static int __ip_finish_output(struct net *net, struct sock *sk, struct sk_buff *skb)
{
    unsigned int mtu = ip_skb_dst_mtu(sk, skb);

    if (skb_is_gso(skb))
        return ip_finish_output_gso(net, sk, skb, mtu);  // GSO 大包

    if (skb->len > mtu || IPCB(skb)->frag_max_size)
        return ip_fragment(net, sk, skb, mtu, ip_finish_output2);  // 需要分片

    return ip_finish_output2(net, sk, skb);  // 直接发送
}
```

### 9.5 `ip_finish_output2`：进入邻居子系统

```c
// net/ipv4/ip_output.c:241
static int ip_finish_output2(struct net *net, struct sock *sk, struct sk_buff *skb)
{
    // 确保 headroom 足够放以太网头
    if (unlikely(skb_headroom(skb) < hh_len && dev->header_ops))
        skb = skb_expand_head(skb, hh_len);

    rcu_read_lock_bh();
    neigh = ip_neigh_for_gw(rt, skb, &is_v6gw);  // ARP 表查找
    if (!IS_ERR(neigh)) {
        sock_confirm_neigh(skb, neigh);
        res = neigh_output(neigh, skb, is_v6gw);  // 邻居输出
    }
    rcu_read_unlock_bh();
    return res;
}
```

### 9.6 Netfilter 钩子位置总览

| 钩子 | 所在函数 | 后续路径 |
|------|----------|---------|
| `NF_INET_LOCAL_OUT` | `__ip_local_out()` | → `dst_output()` → `ip_output()` |
| `NF_INET_POST_ROUTING` | `ip_output()` | → `ip_finish_output()` → `ip_finish_output2()` |

---

## 十、邻居子系统与 ARP

### 10.1 `ip_neigh_for_gw`：查找邻居

```c
// include/net/route.h:485
static inline struct neighbour *ip_neigh_for_gw(struct rtable *rt,
                        struct sk_buff *skb, bool *is_v6gw)
{
    if (likely(rt->rt_gw_family == AF_INET))
        neigh = ip_neigh_gw4(dev, rt->rt_gw4);   // 有网关：查网关 MAC
    else
        neigh = ip_neigh_gw4(dev, ip_hdr(skb)->daddr);  // 直连：查目标 MAC
    return neigh;
}
```

`ip_neigh_gw4()` 先 `__ipv4_neigh_lookup_noref()` 查 ARP 缓存，未命中则 `__neigh_create(&arp_tbl, ...)` 创建新条目并触发 ARP 请求。

### 10.2 `neigh_output`：三条路径

```c
// include/net/neighbour.h:549
static inline int neigh_output(struct neighbour *n, struct sk_buff *skb, bool skip_cache)
{
    if (!skip_cache && (n->nud_state & NUD_CONNECTED) && hh->hh_len)
        return neigh_hh_output(hh, skb);     // 路径 A：MAC 已缓存，直接发
    return n->output(n, skb);                // 路径 B/C
}
```

| 路径 | 条件 | 函数 | 行为 |
|------|------|------|------|
| A | 已连接 + 有 hh_cache | `neigh_hh_output()` | 推入缓存的以太网头 → `dev_queue_xmit()` |
| B | MAC 已知 | `neigh_connected_output()` | `dev_hard_header()` → `dev_queue_xmit()` |
| C | MAC 未知 | `neigh_resolve_output()` | `neigh_event_send()` 触发 ARP → 解析完成后 `dev_queue_xmit()` |

---

## 十一、设备层：dev_queue_xmit 与 Qdisc

### 11.1 `__dev_queue_xmit` 主流程

```c
// net/core/dev.c:4436
static int __dev_queue_xmit(struct sk_buff *skb, struct net_device *sb_dev)
{
    // ① 选择 TX 队列
    txq = netdev_core_pick_tx(dev, skb, sb_dev);
    q = rcu_dereference_bh(txq->qdisc);

    // ② 有 Qdisc：走排队路径
    if (q->enqueue) {
        rc = __dev_xmit_skb(skb, q, dev, txq);
        goto out;
    }

    // ③ 无 Qdisc（noqueue）：直接发送
    skb = validate_xmit_skb(skb, dev, &again);
    HARD_TX_LOCK(dev, txq, cpu);
    skb = dev_hard_start_xmit(skb, dev, txq, &rc);
    HARD_TX_UNLOCK(dev, txq);
}
```

### 11.2 Qdisc 路径

```
__dev_xmit_skb()
  ├─ 空队列 bypass：sch_direct_xmit() → dev_hard_start_xmit()
  ├─ 否则：q->enqueue(skb) 入队
  └─ __qdisc_run()
       └─ while(quota) qdisc_restart()
            └─ dequeue_skb() → sch_direct_xmit()
                 └─ validate_xmit_skb_list()
                 └─ HARD_TX_LOCK
                 └─ dev_hard_start_xmit()
                 └─ HARD_TX_UNLOCK
```

`__qdisc_run()` 循环 `qdisc_restart`，配额用完则 `__netif_schedule` 延迟到软中断：

```c
// net/sched/sch_generic.c:472
void __qdisc_run(struct Qdisc *q)
{
    int quota = READ_ONCE(dev_tx_weight);
    while (qdisc_restart(q, &packets)) {
        quota -= packets;
        if (quota <= 0) {
            __netif_schedule(q);
            break;
        }
    }
}
```

### 11.3 TX 队列选择

```c
// net/core/dev.c:4358
struct netdev_queue *netdev_core_pick_tx(struct net_device *dev,
                     struct sk_buff *skb, struct net_device *sb_dev)
{
    if (dev->real_num_tx_queues != 1) {
        if (ops->ndo_select_queue)
            queue_index = ops->ndo_select_queue(dev, skb, sb_dev);
        else
            queue_index = netdev_pick_tx(dev, skb, sb_dev);  // XPS / skb_tx_hash
    }
    skb_set_queue_mapping(skb, queue_index);
    return netdev_get_tx_queue(dev, queue_index);
}
```

---

## 十二、驱动层：ndo_start_xmit 到硬件

### 12.1 调用链

```
dev_hard_start_xmit(skb, dev, txq, &rc)
  └─ while(skb) xmit_one(skb, dev, txq, more)
       ├─ dev_queue_xmit_nit()        ← tcpdump AF_PACKET 抓包点
       └─ netdev_start_xmit(skb, dev, txq, more)
            └─ __netdev_start_xmit(ops, skb, dev, more)
                 └─ ops->ndo_start_xmit(skb, dev)  ← 驱动入口
```

```c
// include/linux/netdevice.h:5051
static inline netdev_tx_t __netdev_start_xmit(const struct net_device_ops *ops,
                          struct sk_buff *skb, struct net_device *dev, bool more)
{
    __this_cpu_write(softnet_data.xmit.more, more);
    return ops->ndo_start_xmit(skb, dev);
}
```

### 12.2 virtio-net 驱动示例

```c
// drivers/net/virtio_net.c:1727
static netdev_tx_t start_xmit(struct sk_buff *skb, struct net_device *dev)
{
    // ① 回收已完成的旧 skb
    free_old_xmit_skbs(sq, false);

    // ② 将 skb 转为 scatterlist，提交到 TX virtqueue
    err = xmit_skb(sq, skb);
    //   → skb_to_sgvec() + virtqueue_add_outbuf()

    // ③ 队列空间不足，停止上层发送
    if (sq->vq->num_free < 2 + MAX_SKB_FRAGS)
        netif_stop_subqueue(dev, qnum);

    // ④ 通知 hypervisor（doorbell）
    if (kick || netif_xmit_stopped(txq)) {
        virtqueue_kick_prepare(sq->vq);
        virtqueue_notify(sq->vq);
    }
    return NETDEV_TX_OK;
}
```

### 12.3 e1000 驱动示例

```
e1000_xmit_frame()
  ├─ e1000_tx_map()
  │   ├─ dma_map_single()         ← 线性数据 DMA 映射
  │   └─ skb_frag_dma_map()       ← frags DMA 映射
  ├─ e1000_tx_queue()              ← 填写 TX 描述符
  ├─ netdev_sent_queue()           ← BQL：记录发送字节数
  └─ writel(tx_ring->next_to_use, hw->hw_addr + tx_ring->tdt)
     ↑ 写 TDT 寄存器 = doorbell：通知网卡 DMA 引擎读取描述符并发送
```

### 12.4 BQL（Byte Queue Limits）

BQL 跟踪"已交给硬件但未完成"的字节量，控制设备层队列深度：

```c
// include/linux/netdevice.h:3615
static inline void netdev_tx_sent_queue(struct netdev_queue *dev_queue,
                    unsigned int bytes)
{
    dql_queued(&dev_queue->dql, bytes);
    if (likely(dql_avail(&dev_queue->dql) >= 0))
        return;
    set_bit(__QUEUE_STATE_STACK_XOFF, &dev_queue->state);  // 暂停上层发送
}
```

发送完成（TX 中断）时对称调用 `netdev_tx_completed_queue()` 释放配额。

---

## 十三、GSO/TSO 全路径

### 13.1 TSO（硬件分段）

如果网卡支持 TSO（`NETIF_F_TSO`），TCP 层构建大于 MSS 的 GSO skb，一路不分段直到硬件：

```
tcp_sendmsg_locked: size_goal = tp->xmit_size_goal（可能 > MSS）
  → tcp_write_xmit: tcp_init_tso_segs() 计算 GSO 段数
    → tcp_transmit_skb: skb_shinfo(skb)->gso_size = MSS
      → ip_queue_xmit → ip_output → ip_finish_output
        → __ip_finish_output: skb_gso_validate_network_len(mtu) → 直接过
          → ip_finish_output2 → dev_queue_xmit
            → validate_xmit_skb: netif_needs_gso() = false（硬件支持）
              → ndo_start_xmit: 硬件按 gso_size 切分
```

### 13.2 GSO 软件分段

如果硬件不支持当前 GSO 类型，在 `validate_xmit_skb()` 中软件分段：

```c
// net/core/dev.c:3766
static struct sk_buff *validate_xmit_skb(struct sk_buff *skb, struct net_device *dev, ...)
{
    features = netif_skb_features(skb);
    if (netif_needs_gso(skb, features)) {
        segs = skb_gso_segment(skb, features);  // 软件分段
        consume_skb(skb);
        skb = segs;
    }
}
```

### 13.3 IP 层 GSO 处理

```c
// net/ipv4/ip_output.c:389
static int ip_finish_output_gso(struct net *net, struct sock *sk,
                struct sk_buff *skb, unsigned int mtu)
{
    if (skb_gso_validate_network_len(skb, mtu))
        return ip_finish_output2(net, sk, skb);  // 快路径：GSO 段 <= MTU

    segs = skb_gso_segment(skb, features);       // 软件分段
    skb_list_walk_safe(segs, segs, nskb) {
        err = ip_fragment(net, sk, segs, mtu, ip_finish_output2);
    }
}
```

---

## 十四、发送完成与资源回收

### 14.1 返回值语义

```c
// include/linux/netdevice.h:116
enum netdev_tx {
    NETDEV_TX_OK    = 0x00,    // 已消费/已发送
    NETDEV_TX_BUSY  = 0x10,    // 队列满，请重试
};
```

### 14.2 TX 完成中断回收

以 e1000 为例，`e1000_clean_tx_irq()` 在 NAPI poll 中回收已完成的描述符：

```
e1000_clean_tx_irq()
  ├─ 检查描述符 DD 位（DMA 完成）
  ├─ e1000_unmap_and_free_tx_resource()   ← DMA unmap + kfree_skb
  ├─ netdev_completed_queue()             ← BQL：释放配额
  └─ netif_wake_queue()                   ← 如果队列曾停止，重新启动
```

### 14.3 skb 生命周期总结

```
                    tcp_sendmsg_locked
                         │
     ┌───────────────────┼───────────────────┐
     │ sk_stream_alloc_skb()                 │
     │ → tcp_skb_entail()                    │
     │   挂入 sk_write_queue                  │
     │                                        │
     │ tcp_write_xmit:                        │
     │   tcp_transmit_skb:                    │
     │     skb_clone() ─────→ clone_skb       │
     │                         │              │
     │   tcp_event_new_data_sent:             │
     │     原件 → tcp_rtx_queue（重传树）       │
     │                                        │
     │                    clone_skb            │
     │                    → ip_queue_xmit      │
     │                    → dev_queue_xmit     │
     │                    → ndo_start_xmit     │
     │                    → DMA → 网卡发送      │
     │                    → TX 中断回收 clone   │
     │                                        │
     │ ACK 到达:                               │
     │   tcp_clean_rtx_queue()                │
     │   → 从重传树删除原件                     │
     │   → kfree_skb(原件)                    │
     └────────────────────────────────────────┘
```

---

## 十五、总结

### 各层职责

| 层次 | 核心函数 | 关键职责 |
|------|----------|---------|
| **系统调用** | `__sys_sendto` / `sock_write_iter` | fd→socket、构建 msghdr、安全检查 |
| **Socket 层** | `sock_sendmsg` → `inet_sendmsg` | 协议族分派、自动绑定端口 |
| **TCP 传输层** | `tcp_sendmsg_locked` | 数据拷贝到 skb、写序列号管理、缓冲区控制 |
| **TCP 输出** | `tcp_write_xmit` | 六道关卡（pacing/cwnd/rwnd/Nagle/TSO/SQ）|
| **TCP 封装** | `__tcp_transmit_skb` | 构建 TCP 头、选项、校验和、skb 克隆 |
| **IP 层** | `__ip_queue_xmit` → `ip_local_out` → `ip_output` | 路由查找、IP 头、Netfilter 钩子 |
| **IP 输出** | `ip_finish_output` → `ip_finish_output2` | GSO/分片决策、进入邻居子系统 |
| **邻居子系统** | `neigh_output` / `neigh_resolve_output` | ARP 解析、填以太网头 |
| **设备层** | `__dev_queue_xmit` | 选 TX 队列、Qdisc 入队/出队、GSO 软分段 |
| **驱动层** | `ndo_start_xmit` | DMA 映射、描述符填写、doorbell 通知硬件 |

### 关键设计思想

1. **延迟发送**：数据先拷贝到 skb 缓冲区，积累到合适大小再发（Nagle、autocork、TSO defer）
2. **流量控制层层把关**：拥塞窗口 → 接收窗口 → pacing → 小队列 → Qdisc → BQL
3. **零拷贝优化**：skb clone 避免重传树与发送路径的数据拷贝；MSG_ZEROCOPY 避免用户态拷贝
4. **硬件卸载**：TSO/GSO 让大段数据尽量晚分段，减少 per-packet 开销
5. **skb 两份生命**：clone 给网卡发送后由 TX 中断回收；原件留在重传树直到 ACK 确认

---

## 十六、阻塞与非阻塞 fd 的 TCP 发送差异

阻塞和非阻塞 fd 调用 `send()`/`write()` 发送 TCP 数据时，走的是**同一条代码路径**，差异仅体现在一个变量 `timeo` 上。`timeo` 决定了在**连接未就绪**和**发送缓冲区满**两个等待点是立即返回还是睡眠等待。

### 16.1 O_NONBLOCK 标志的传播路径

```
用户态 fd 的 O_NONBLOCK
    │
    ▼
┌─────────────────────────────────────────────┐
│ __sys_sendto() / sock_write_iter()          │
│ if (sock->file->f_flags & O_NONBLOCK)       │
│     flags |= MSG_DONTWAIT;                  │
│ msg.msg_flags = flags;                      │
└──────────────────────┬──────────────────────┘
                       │ msg_flags 携带 MSG_DONTWAIT
                       ▼
┌─────────────────────────────────────────────┐
│ tcp_sendmsg_locked()                        │
│ timeo = sock_sndtimeo(sk, flags & MSG_DONTWAIT) │
└──────────────────────┬──────────────────────┘
                       │
        ┌──────────────┴──────────────┐
        ▼                             ▼
  MSG_DONTWAIT=1                MSG_DONTWAIT=0
  timeo = 0                    timeo = sk->sk_sndtimeo
  (非阻塞)                     (默认 MAX_SCHEDULE_TIMEOUT)
```

**`__sys_sendto()` 中的标志转换**（`net/socket.c:2191`）：

```c
// net/socket.c:2191
if (sock->file->f_flags & O_NONBLOCK)
    flags |= MSG_DONTWAIT;
msg.msg_flags = flags;
```

**`sock_write_iter()` 中的标志转换**（`net/socket.c:1072`）：

```c
// net/socket.c:1072
if (file->f_flags & O_NONBLOCK || (iocb->ki_flags & IOCB_NOWAIT))
    msg.msg_flags = MSG_DONTWAIT;
```

**`sock_sndtimeo()` 的实现**（`include/net/sock.h:2502`）：

```c
// include/net/sock.h:2502
static inline long sock_sndtimeo(const struct sock *sk, bool noblock)
{
    return noblock ? 0 : sk->sk_sndtimeo;
}
```

### 16.2 两个等待点的行为差异

TCP 发送路径中有两个地方会根据 `timeo` 决定是阻塞还是返回错误：

#### 等待点一：连接未建立 — `sk_stream_wait_connect()`

在 `tcp_sendmsg_locked()` 开头检查连接状态，如果 socket 还在 `SYN_SENT` / `SYN_RECV` 状态（三次握手未完成），需要等待。

```c
// net/core/stream.c:68
if (!*timeo_p)
    return -EAGAIN;     // 非阻塞：立即返回
// 阻塞：通过 sk_wait_event() 睡眠，直到连接建立或超时
add_wait_queue(sk_sleep(sk), &wait);
sk->sk_write_pending++;
done = sk_wait_event(sk, timeo_p,
         !sk->sk_err &&
         !((1 << sk->sk_state) &
           ~(TCPF_ESTABLISHED | TCPF_CLOSE_WAIT)), &wait);
```

#### 等待点二：发送缓冲区满 — `sk_stream_wait_memory()`

当发送队列 `sk_wmem_queued` 超过 `sk_sndbuf` 限制时，无法再分配 skb，进入等待。

```c
// net/core/stream.c:136-137
if (!*timeo_p)
    goto do_eagain;     // 非阻塞：跳转到 EAGAIN 处理
```

**`do_eagain` 的处理**（`net/core/stream.c:170-179`）：

```c
// net/core/stream.c:170-179
do_eagain:
    /* 设置 SOCK_NOSPACE 标志，确保后续缓冲区有空间时
     * 能触发 EPOLLOUT 事件通知应用层
     * tcp_check_space() 收到 ACK 释放空间时，检查此标志
     * 来决定是否调用 tcp_new_space() */
    set_bit(SOCK_NOSPACE, &sk->sk_socket->flags);
    err = -EAGAIN;
    goto out;
```

**阻塞模式**下，`sk_stream_wait_memory()` 通过 `sk_wait_event()` 挂起进程，等待条件：

```c
// net/core/stream.c:147-151
sk_wait_event(sk, &current_timeo, sk->sk_err ||
              (sk->sk_shutdown & SEND_SHUTDOWN) ||
              (sk_stream_memory_free(sk) &&
              !vm_wait), &wait);
```

唤醒时机：ACK 到达 → `tcp_data_snd_check()` → `tcp_check_space()` → 释放已确认 skb 的内存 → `sk_stream_memory_free()` 返回 true → 唤醒等待队列。

### 16.3 `tcp_sendmsg_locked()` 中的 wait_for_space 跳转

```c
// net/ipv4/tcp.c:1833-1847
wait_for_space:
    set_bit(SOCK_NOSPACE, &sk->sk_socket->flags);
    if (copied)
        tcp_push(sk, flags & ~MSG_MORE, mss_now,
                 TCP_NAGLE_PUSH, size_goal);

    err = sk_stream_wait_memory(sk, &timeo);
    if (err != 0)
        goto do_error;

    mss_now = tcp_send_mss(sk, &size_goal, flags);
```

关键逻辑：
- 进入 `wait_for_space` 前先设 `SOCK_NOSPACE`
- 如果已有部分数据拷贝（`copied > 0`），先调 `tcp_push()` 推送出去（释放部分缓冲区）
- 然后调 `sk_stream_wait_memory()` — 这里 `timeo` 决定了阻塞还是 `-EAGAIN`

### 16.4 返回值差异对比

| 场景 | 阻塞 fd | 非阻塞 fd |
|------|---------|-----------|
| **连接未建立** | 睡眠等待连接完成，超时返回 `-ERESTARTSYS` | 立即返回 `-EAGAIN` |
| **缓冲区满，已拷贝部分数据** | 睡眠等待空间，继续拷贝剩余数据 | 返回已拷贝字节数（**短写**） |
| **缓冲区满，未拷贝任何数据** | 睡眠等待空间 | 返回 `-EAGAIN`（`errno=EAGAIN`） |
| **信号中断** | 若已拷贝部分数据返回 `copied`，否则返回 `-ERESTARTSYS` 或 `-EINTR` | 同阻塞情况 |
| **对端关闭** | 返回 `-EPIPE`，进程收到 `SIGPIPE` | 同阻塞情况 |
| **正常完成** | 返回 `size`（全部数据） | 返回 `size` 或短写 |

**短写（short write）的处理**：

```c
// net/ipv4/tcp.c:1860-1867
out:
    if (copied) {
        tcp_tx_timestamp(sk, sockc.tsflags);
        tcp_push(sk, flags, mss_now, tp->nonagle, size_goal);
    }
out_nopush:
    release_sock(sk);
    return copied;    // 返回已拷贝的字节数，可能 < size
```

当非阻塞 fd 遇到缓冲区满时，如果已经拷贝了部分数据（`copied > 0`），`sk_stream_wait_memory()` 返回 `-EAGAIN`，走 `do_error` → 但 `copied > 0` 所以跳到 `out`，返回 `copied`。这就是**短写**——应用层需要检查返回值并重试剩余部分。

### 16.5 SO_SNDTIMEO 的影响

用户可通过 `setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, ...)` 设置 `sk->sk_sndtimeo`：

- **未设置**：`sk->sk_sndtimeo = MAX_SCHEDULE_TIMEOUT`（无限等待）
- **设置超时**：阻塞模式下等待指定时间后返回 `-EAGAIN`

```
                    ┌──────────────────────────────────────┐
                    │         timeo 的来源决策树            │
                    └──────────────────────┬───────────────┘
                                          │
                            fd 是否设置 O_NONBLOCK?
                           ┌──────────────┴──────────────┐
                          yes                           no
                           │                             │
                    timeo = 0              是否设置 SO_SNDTIMEO?
                    (永不阻塞)            ┌──────────┴──────────┐
                                        yes                   no
                                         │                     │
                                  timeo = 用户设定值    timeo = MAX_SCHEDULE_TIMEOUT
                                  (有限阻塞)           (无限阻塞)
```

---

## 十七、TSO/GSO 与小包聚合发送机制详解

TCP 发送路径有一套精密的**延迟发送 + 聚合发送**机制，核心目标是减少 per-packet 开销，让数据尽量积攒成大包再发出。整体分为四个层次：

```
┌────────────────────────────────────────────────────────────────────┐
│                    TCP 发送聚合的四个层次                           │
├──────────────┬─────────────────────────────────────────────────────┤
│ 层次一       │ 小包抑制：Nagle / Autocork / MSG_MORE              │
│              │ 在 tcp_sendmsg_locked / tcp_push 层面延迟推送       │
├──────────────┼─────────────────────────────────────────────────────┤
│ 层次二       │ TSO SKB 构建：size_goal > MSS                      │
│              │ 在 tcp_sendmsg_locked 中构建超大 skb                │
├──────────────┼─────────────────────────────────────────────────────┤
│ 层次三       │ TSO 发送控制：TSO defer / autosize / split          │
│              │ 在 tcp_write_xmit 中控制发送时机和分片               │
├──────────────┼─────────────────────────────────────────────────────┤
│ 层次四       │ GSO 分段执行：validate_xmit_skb → skb_gso_segment  │
│              │ 在设备层将大 skb 拆成 MSS 大小的真实包               │
└──────────────┴─────────────────────────────────────────────────────┘
```

### 17.1 层次一：小包抑制机制

#### 17.1.1 Nagle 算法

Nagle 算法的核心思想：**如果有已发送但未确认的数据（`packets_out > 0`），且当前包不满一个 MSS，则不发送**。

**判定函数 `tcp_nagle_check()`**（`net/ipv4/tcp_output.c:2651`）：

```c
// net/ipv4/tcp_output.c:2651-2657
static bool tcp_nagle_check(bool partial, const struct tcp_sock *tp,
                            int nonagle)
{
    return partial &&
        ((nonagle & TCP_NAGLE_CORK) ||
         (!nonagle && tp->packets_out && tcp_minshall_check(tp)));
}
```

- `partial`：`skb->len < cur_mss`，即当前包不满一个 MSS
- `nonagle & TCP_NAGLE_CORK`：用户设置了 `TCP_CORK`，强制不发小包
- `!nonagle && tp->packets_out`：Nagle 模式且有在途包 → 等待 ACK
- `tcp_minshall_check(tp)`：检查最近的 ACK 是否确认了所有数据（Minshall 优化）

**`tcp_nagle_test()` 包装**（`net/ipv4/tcp_output.c:2846-2866`）：

```c
// net/ipv4/tcp_output.c:2846-2866
static inline bool tcp_nagle_test(const struct tcp_sock *tp,
                                  const struct sk_buff *skb,
                                  unsigned int cur_mss, int nonagle)
{
    // 队列中间的包强制发出
    if (nonagle & TCP_NAGLE_PUSH)
        return true;
    // 紧急数据或 FIN 包不受 Nagle 限制
    if (tcp_urg_mode(tp) || (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN))
        return true;
    // 核心判断：不满 MSS 的小包是否被 Nagle 阻止
    if (!tcp_nagle_check(skb->len < cur_mss, tp, nonagle))
        return true;
    return false;
}
```

**控制开关**：

| 选项 | `tp->nonagle` 值 | 效果 |
|------|-------------------|------|
| 默认 | `0` | 启用 Nagle，有在途包时不发小包 |
| `TCP_NODELAY` | `TCP_NAGLE_OFF (1)` | 禁用 Nagle，小包立即发送 |
| `TCP_CORK` | `TCP_NAGLE_CORK (2)` | 强制 cork，所有小包都不发 |

#### 17.1.2 Autocork 机制

Autocork 是内核自动的 cork 机制，在 `tcp_push()` 中决定是否延迟推送。

**`tcp_should_autocork()`**（`net/ipv4/tcp.c:789-796`）：

```c
// net/ipv4/tcp.c:789-796
static bool tcp_should_autocork(struct sock *sk, struct sk_buff *skb,
                                int size_goal)
{
    return skb->len < size_goal &&                             // 当前 skb 未满
           READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_autocorking) && // 系统开关开启
           !tcp_rtx_queue_empty(sk) &&                          // 有在途数据（等 ACK）
           refcount_read(&sk->sk_wmem_alloc) > skb->truesize;  // 设备层有积压
}
```

四个条件**全部满足**时，`tcp_push()` 跳过 `__tcp_push_pending_frames()`，不触发实际发送。当 ACK 到达后，`tcp_data_snd_check()` → `tcp_push_pending_frames()` 会推送积攒的数据。

#### 17.1.3 MSG_MORE 标志

应用层通过 `send(fd, buf, len, MSG_MORE)` 显式声明"后面还有数据"。

**`tcp_push()` 中的处理**（`net/ipv4/tcp.c:878`）：

```c
// net/ipv4/tcp.c:878
if (flags & MSG_MORE)
    nonagle = TCP_NAGLE_CORK;
```

`MSG_MORE` 把本次调用的 `nonagle` 参数临时设为 `TCP_NAGLE_CORK`，但**不修改** `tp->nonagle`，仅影响当前这次 `tcp_push()` 调用。后续不带 `MSG_MORE` 的 `send()` 恢复正常行为。

#### 17.1.4 TSO defer

`tcp_tso_should_defer()` 在 `tcp_write_xmit()` 的发送循环中判断**是否延迟发送不满的 TSO 包**（`net/ipv4/tcp_output.c:2974-3074`）。

延迟条件（全部满足时 defer）：
1. 不在 Recovery/Loss 状态
2. 距离上次写入不超过 1ms
3. 窗口不足以发满一个完整 TSO 段（`limit < max_segs * mss`）
4. 不满 `tcp_tso_win_divisor` 比例窗口
5. 距离上次发包的时间不超过 `srtt/2`

```c
// net/ipv4/tcp_output.c:2990-2992
delta = tp->tcp_clock_cache - tp->tcp_wstamp_ns - NSEC_PER_MSEC;
if (delta > 0)
    goto send_now;   // 超过 1ms 未写入新数据，立即发送
```

### 17.2 层次二：TSO SKB 构建

#### 17.2.1 size_goal 的计算

`tcp_sendmsg_locked()` 中，每个 skb 的目标大小不是 MSS，而是 `size_goal`：

**`tcp_xmit_size_goal()`**（`net/ipv4/tcp.c:1125-1149`）：

```c
// net/ipv4/tcp.c:1125-1149
static unsigned int tcp_xmit_size_goal(struct sock *sk, u32 mss_now,
                                       int large_allowed)
{
    struct tcp_sock *tp = tcp_sk(sk);
    u32 new_size_goal, size_goal;

    if (!large_allowed)
        return mss_now;

    // GSO 载荷预算：网卡 GSO 上限 - TCP 头空间
    new_size_goal = sk->sk_gso_max_size - 1 - MAX_TCP_HEADER;
    // 不超过接收窗口的一半（避免 SWS）
    new_size_goal = tcp_bound_to_half_wnd(tp, new_size_goal);

    // 按 MSS 对齐
    size_goal = tp->gso_segs * mss_now;
    if (unlikely(new_size_goal < size_goal ||
                 new_size_goal >= size_goal + mss_now)) {
        tp->gso_segs = min_t(u16, new_size_goal / mss_now,
                             sk->sk_gso_max_segs);
        size_goal = tp->gso_segs * mss_now;
    }

    return max(size_goal, mss_now);
}
```

- `sk->sk_gso_max_size`：网卡支持的最大 GSO 大小（典型值 65536 字节）
- `sk->sk_gso_max_segs`：网卡支持的最大 GSO 段数
- 最终 `size_goal = gso_segs × MSS`，典型值约 64KB（约 44 个 1460 字节的段）

**`tcp_send_mss()` 包装**（`net/ipv4/tcp.c:1162`）：

```c
// net/ipv4/tcp.c:1162-1170
int tcp_send_mss(struct sock *sk, int *size_goal, int flags)
{
    int mss_now;
    mss_now = tcp_current_mss(sk);
    *size_goal = tcp_xmit_size_goal(sk, mss_now, !(flags & MSG_OOB));
    return mss_now;
}
```

#### 17.2.2 skb 的聚合过程

在 `tcp_sendmsg_locked()` 的主循环中，用户数据被拷贝到 skb 直到达到 `size_goal`：

```
用户调用 send(fd, buf, 128KB)
    │
    ▼  size_goal = 44 × 1460 = 64240 字节
┌─────────────────────────────────────┐
│ skb_1: 拷贝 64240 字节              │  ← 一个大 skb，内含 44 段数据
│   线性区：部分数据                   │
│   page frag：大部分数据              │
├─────────────────────────────────────┤
│ skb_2: 拷贝 64240 字节              │  ← 第二个大 skb
│   ...                               │
├─────────────────────────────────────┤
│ skb_3: 拷贝剩余字节 (< size_goal)   │  ← 尾部 skb
└─────────────────────────────────────┘
```

### 17.3 层次三：TSO 发送控制（`tcp_write_xmit`）

#### 17.3.1 TSO 段数自适应 — `tcp_tso_autosize()`

`tcp_write_xmit()` 调用 `tcp_tso_segs()` 动态计算每次发送的 TSO 段数：

**`tcp_tso_autosize()`**（`net/ipv4/tcp_output.c:2670-2687`）：

```c
// net/ipv4/tcp_output.c:2670-2687
static u32 tcp_tso_autosize(const struct sock *sk, unsigned int mss_now,
                            int min_tso_segs)
{
    u32 bytes, segs;

    bytes = min_t(unsigned long,
                  sk->sk_pacing_rate >> READ_ONCE(sk->sk_pacing_shift),
                  sk->sk_gso_max_size - 1 - MAX_TCP_HEADER);

    // 目标：每 ms 发一个包，而不是每 100ms 发一个大 TSO 包
    segs = max_t(u32, bytes / mss_now, min_tso_segs);

    return segs;
}
```

- `sk_pacing_rate >> sk_pacing_shift`：每个 pacing 周期可发送的字节数
- 目标是每毫秒发一个 TSO 包（保持 ACK 时钟）

**`tcp_tso_segs()` 封装**（`net/ipv4/tcp_output.c:2713-2727`）：

```c
// net/ipv4/tcp_output.c:2713-2727
static u32 tcp_tso_segs(struct sock *sk, unsigned int mss_now)
{
    const struct tcp_congestion_ops *ca_ops = inet_csk(sk)->icsk_ca_ops;
    u32 min_tso, tso_segs;

    // 拥塞算法可自定义最小 TSO 段数
    min_tso = ca_ops->min_tso_segs ?
            ca_ops->min_tso_segs(sk) :
            READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_min_tso_segs);

    tso_segs = tcp_tso_autosize(sk, mss_now, min_tso);
    return min_t(u32, tso_segs, sk->sk_gso_max_segs);
}
```

#### 17.3.2 TSO 分片 — `tso_fragment()`

如果 skb 数据量超过窗口或 TSO 段数限制，`tcp_write_xmit()` 调用 `tcp_mss_split_point()` 计算切割点，再调用 `tso_fragment()` 将 skb 一分为二：

```c
// net/ipv4/tcp_output.c:2741-2770 (tcp_mss_split_point 核心逻辑)
window = tcp_wnd_end(tp) - TCP_SKB_CB(skb)->seq;
max_len = mss_now * max_segs;
// 取窗口和 max_len 的较小值
needed = min(skb->len, window);
if (max_len <= needed)
    return max_len;
// 尾部不满 MSS 时用 Nagle 判断是否包含
partial = needed % mss_now;
if (tcp_nagle_check(partial != 0, tp, nonagle))
    return needed - partial;   // 切掉尾部不满的部分
return needed;
```

`tso_fragment()` 执行实际分割（`net/ipv4/tcp_output.c:2900-2951`）：
- 分配新 skb（`buff`），将 `skb` 尾部数据移到 `buff`
- 调整两个 skb 的序列号、标志位（PSH/FIN 移到后段）
- 对两个 skb 分别调用 `tcp_set_skb_tso_segs()` 重新计算 GSO 元数据

#### 17.3.3 GSO 元数据的设置

**`tcp_set_skb_tso_segs()`**（`net/ipv4/tcp_output.c:2017-2028`）：

```c
// net/ipv4/tcp_output.c:2017-2028
static void tcp_set_skb_tso_segs(struct sk_buff *skb, unsigned int mss_now)
{
    if (skb->len <= mss_now) {
        tcp_skb_pcount_set(skb, 1);         // 单段，不需要 TSO
        TCP_SKB_CB(skb)->tcp_gso_size = 0;
    } else {
        tcp_skb_pcount_set(skb, DIV_ROUND_UP(skb->len, mss_now));
        TCP_SKB_CB(skb)->tcp_gso_size = mss_now;
    }
}
```

**`__tcp_transmit_skb()` 中复制到 skb_shinfo**（`net/ipv4/tcp_output.c:1937-1938`）：

```c
// net/ipv4/tcp_output.c:1937-1938
skb_shinfo(skb)->gso_segs = tcp_skb_pcount(skb);   // TCP_SKB_CB→skb_shinfo
skb_shinfo(skb)->gso_size = tcp_skb_mss(skb);      // TCP_SKB_CB→skb_shinfo
```

这样，skb 携带 GSO 元数据进入 IP 层和设备层。

### 17.4 层次四：GSO 分段执行

#### 17.4.1 硬件 TSO vs 软件 GSO 的决策

在 `net/core/dev.c` 的 `validate_xmit_skb()` 中，设备层检查网卡是否支持硬件 TSO：

```c
// net/core/dev.c:3792-3813
if (netif_needs_gso(skb, features)) {
    struct sk_buff *segs;
    // 网卡不支持此 GSO 类型 → 软件分段
    segs = skb_gso_segment(skb, features);
    if (IS_ERR(segs)) {
        goto out_kfree_skb;
    } else if (segs) {
        consume_skb(skb);   // 释放原始大 skb
        skb = segs;         // 替换为分段后的链表
    }
}
```

- `netif_needs_gso()` 检查 `skb_shinfo(skb)->gso_size > 0` 且网卡不支持对应的 `gso_type`
- 如果网卡支持 `NETIF_F_TSO`（对应 `SKB_GSO_TCPV4`），则直接下发给硬件分段

#### 17.4.2 软件 GSO 分段路径

```
skb_gso_segment()
  └── tcp4_gso_segment()        // net/ipv4/tcp_offload.c:88
        └── tcp_gso_segment()   // net/ipv4/tcp_offload.c:158
              └── skb_segment() // 真正的内存分割
```

**`tcp_gso_segment()` 核心逻辑**（`net/ipv4/tcp_offload.c:192-227`）：

```c
// net/ipv4/tcp_offload.c:192-227
mss = skb_shinfo(skb)->gso_size;
if (unlikely(skb->len <= mss))
    goto out;   // 不需要分段

// 检查硬件是否可以处理
if (skb_gso_ok(skb, features | NETIF_F_GSO_ROBUST)) {
    skb_shinfo(skb)->gso_segs = DIV_ROUND_UP(skb->len, mss);
    segs = NULL;    // NULL → 硬件处理
    goto out;
}

// 软件分段
segs = skb_segment(skb, features);
```

软件分段后，`tcp_gso_segment()` 遍历所有分段，逐个调整 TCP 头：
- **序列号**：每段递增 `mss` 字节
- **标志位**：中间段清除 `FIN`/`PSH`/`CWR`，最后一段保留
- **校验和**：每段重新计算

#### 17.4.3 全路径示意

```
用户态 send(fd, 128KB)
    │
    ▼ tcp_sendmsg_locked: 按 size_goal=64KB 构建 skb
┌─────────┐  ┌─────────┐
│ skb 64KB │  │ skb 64KB │
└────┬─────┘  └────┬─────┘
     │              │
     ▼ tcp_write_xmit: tso_autosize → 决定一次发多少段
     │ tso_fragment: 若窗口不够，分割 skb
     │
     ▼ __tcp_transmit_skb: 写 TCP 头，设 gso_segs/gso_size
     │
     ▼ ip_queue_xmit → ip_output
     │
     ▼ dev_queue_xmit → validate_xmit_skb
     │
     ├── 网卡支持 TSO ──→ 大 skb 直接下发给网卡硬件分段
     │                     ndo_start_xmit(skb_64KB)
     │                     网卡将 64KB 按 MSS 切成 ~44 个以太网帧
     │
     └── 网卡不支持 TSO ──→ skb_gso_segment() 软件分段
                            tcp_gso_segment() → skb_segment()
                            生成 ~44 个 MSS 大小的 skb
                            逐个调用 ndo_start_xmit()
```

### 17.5 各机制协作关系

```
┌─────────────────────────────────────────────────────┐
│              tcp_sendmsg_locked()                    │
│  用户数据 → skb (size_goal ≈ 64KB)                  │
│                    │                                 │
│                    ▼                                 │
│              tcp_push()                              │
│  ┌──────────────────────────────────────┐            │
│  │ MSG_MORE?   → nonagle = CORK        │            │
│  │ autocork?   → 跳过推送              │            │
│  │ 否则        → __tcp_push_pending    │            │
│  └──────────────────┬───────────────────┘            │
│                     │                                │
│                     ▼                                │
│             tcp_write_xmit()                         │
│  ┌──────────────────────────────────────┐            │
│  │ Nagle check   → 小包且有在途？阻止  │            │
│  │ TSO defer     → 不满 TSO？延迟      │            │
│  │ TSO autosize  → 计算段数            │            │
│  │ tso_fragment  → 按窗口切割          │            │
│  │ cwnd/rwnd     → 流控               │            │
│  └──────────────────┬───────────────────┘            │
│                     │                                │
│                     ▼                                │
│          __tcp_transmit_skb()                        │
│  设置 skb_shinfo->gso_segs/gso_size                 │
└─────────────────────┬───────────────────────────────┘
                      │
                      ▼
              IP → 设备层 → validate_xmit_skb
                      │
            ┌─────────┴──────────┐
            ▼                    ▼
       硬件 TSO              软件 GSO
   网卡按 MSS 分段      tcp_gso_segment()
                        skb_segment()
```

---

## 十八、发送路径关键技术点深入补充

### 18.1 Pacing 与 EDT（Earliest Departure Time）

TCP pacing 的目标是将突发发送平滑化，避免瞬间打满网络导致排队和丢包。Linux 实现了两种 pacing 模式：**内部 pacing**（hrtimer）和**外部 pacing**（fq qdisc）。

#### 18.1.1 内部 Pacing：hrtimer

`tcp_pacing_check()` 在 `tcp_write_xmit()` 循环的第一道关卡（`net/ipv4/tcp_output.c:3292`）：

```c
// net/ipv4/tcp_output.c:3292-3312
static bool tcp_pacing_check(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);

    if (!tcp_needs_internal_pacing(sk))
        return false;                              // 使用 fq qdisc，不需要内部 pacing

    if (tp->tcp_wstamp_ns <= tp->tcp_clock_cache)
        return false;                              // 允许发送时间已到

    if (!hrtimer_is_queued(&tp->pacing_timer)) {
        hrtimer_start(&tp->pacing_timer,
                      ns_to_ktime(tp->tcp_wstamp_ns),
                      HRTIMER_MODE_ABS_PINNED_SOFT);
        sock_hold(sk);
    }
    return true;                                   // 需要等待
}
```

#### 18.1.2 EDT 时间戳机制

每个 skb 携带一个"最早离开时间"（EDT），存储在 `skb->skb_mstamp_ns`（与 `skb->tstamp` 是 union）：

```c
// include/linux/skbuff.h:827
union {
    ktime_t     tstamp;           // 接收/发送时间戳
    u64         skb_mstamp_ns;    // 最早离开时间（EDT，用于 pacing）
};
```

`__tcp_transmit_skb()` 中设置 EDT（`net/ipv4/tcp_output.c:1775`）：

```c
// net/ipv4/tcp_output.c:1773-1776
prior_wstamp = tp->tcp_wstamp_ns;
tp->tcp_wstamp_ns = max(tp->tcp_wstamp_ns, tp->tcp_clock_cache);
skb->skb_mstamp_ns = tp->tcp_wstamp_ns;
```

发送成功后，`tcp_update_skb_after_send()` 根据 pacing rate 递增 `tcp_wstamp_ns`（`net/ipv4/tcp_output.c:1676`）：

```c
// net/ipv4/tcp_output.c:1688-1697
if (rate != ~0UL && rate && tp->data_segs_out >= 10) {
    // 按当前 pacing rate 计算发送此 skb 所需时间
    u64 len_ns = div64_ul((u64)skb->len * NSEC_PER_SEC, rate);
    // credit: 调度延迟的补偿（最多抵消一半）
    u64 credit = tp->tcp_wstamp_ns - prior_wstamp;
    len_ns -= min_t(u64, len_ns / 2, credit);
    tp->tcp_wstamp_ns += len_ns;
}
```

#### 18.1.3 外部 Pacing：fq qdisc

当使用 `fq`（Fair Queue）qdisc 时，`tcp_needs_internal_pacing()` 返回 false，TCP 不启动 hrtimer，而是让 fq 根据 `skb->tstamp` 控制发送时机。

`fq_enqueue()` 中读取 EDT（`net/sched/sch_fq.c:453`）：

```c
// net/sched/sch_fq.c:453-458
if (!skb->tstamp) {
    fq_skb_cb(skb)->time_to_send = q->ktime_cache = ktime_get_ns();
} else {
    // ... horizon 检查 ...
    fq_skb_cb(skb)->time_to_send = skb->tstamp;  // 使用 TCP 设置的 EDT
}
```

`fq_dequeue()` 中等待 EDT 到达（`net/sched/sch_fq.c:568`）：

```c
// net/sched/sch_fq.c:568-577
u64 time_next_packet = max_t(u64, fq_skb_cb(skb)->time_to_send,
                             f->time_next_packet);
if (now < time_next_packet) {
    fq_flow_set_throttled(q, f);   // 时间未到，流被限速
    goto begin;
}
```

```
TCP 层                            Qdisc 层
┌─────────────────────┐           ┌──────────────────────┐
│ tcp_wstamp_ns += Δt  │           │ fq_enqueue:          │
│ skb->tstamp = EDT    │ ─────→   │   time_to_send = EDT │
│                      │           │ fq_dequeue:          │
│ 内部 pacing 不启用    │           │   now < EDT? 等待    │
└─────────────────────┘           └──────────────────────┘
```

### 18.2 TCP Small Queues (TSQ)

TSQ 限制每个 socket 在设备层的排队字节量，防止单条 TCP 流占满 Qdisc 和驱动 TX ring。

#### 18.2.1 `tcp_small_queue_check()`

这是 `tcp_write_xmit()` 的第六道关卡（`net/ipv4/tcp_output.c:3331`）：

```c
// net/ipv4/tcp_output.c:3331-3374
static bool tcp_small_queue_check(struct sock *sk, const struct sk_buff *skb,
                                  unsigned int factor)
{
    unsigned long limit;

    // 限制 = max(2 × skb 真实大小, pacing_rate >> pacing_shift)
    limit = max_t(unsigned long,
                  2 * skb->truesize,
                  sk->sk_pacing_rate >> READ_ONCE(sk->sk_pacing_shift));
    // 无 pacing 时额外受 sysctl 限制
    if (sk->sk_pacing_status == SK_PACING_NONE)
        limit = min_t(unsigned long, limit,
                      READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_limit_output_bytes));
    limit <<= factor;

    // 检查：设备层排队字节 > 限制？
    if (refcount_read(&sk->sk_wmem_alloc) > limit) {
        if (tcp_rtx_queue_empty(sk))
            return false;               // 重传队列空时不限制
        set_bit(TSQ_THROTTLED, &sk->sk_tsq_flags);
        smp_mb__after_atomic();
        if (refcount_read(&sk->sk_wmem_alloc) > limit)
            return true;                // 确认被限速
    }
    return false;
}
```

#### 18.2.2 TSQ 延迟发送回调

当 skb 被网卡消费（TX 完成中断）后，`sk_wmem_alloc` 减小，TSQ tasklet 被唤醒来继续发送：

```c
// net/ipv4/tcp_output.c:1446-1486
static void tcp_tsq_handler(struct sock *sk)
{
    bh_lock_sock(sk);
    if (!sock_owned_by_user(sk))
        tcp_tsq_write(sk);               // 直接发送
    else if (!test_and_set_bit(TCP_TSQ_DEFERRED, &sk->sk_tsq_flags))
        sock_hold(sk);                    // 延迟到 release_sock
    bh_unlock_sock(sk);
}

static void tcp_tasklet_func(struct tasklet_struct *t)
{
    struct tsq_tasklet *tsq = from_tasklet(tsq, t, tasklet);
    LIST_HEAD(list);
    // ... 从每 CPU 的 tsq->head 取出待处理 socket ...
    list_for_each_safe(q, n, &list) {
        tp = list_entry(q, struct tcp_sock, tsq_node);
        list_del(&tp->tsq_node);
        sk = (struct sock *)tp;
        clear_bit(TSQ_QUEUED, &sk->sk_tsq_flags);
        tcp_tsq_handler(sk);
        sk_free(sk);
    }
}
```

### 18.3 ECN（显式拥塞通知）

ECN 允许路由器在拥塞时标记 IP 头的 ECT/CE 位，而不是直接丢包。TCP 通过 ECE/CWR 标志位回馈拥塞信息。

#### 18.3.1 `tcp_ecn_send()`

在 `__tcp_transmit_skb()` 构建 TCP 头后调用（`net/ipv4/tcp_output.c:1889`）：

```c
// net/ipv4/tcp_output.c:527-550
static void tcp_ecn_send(struct sock *sk, struct sk_buff *skb,
                         struct tcphdr *th, int tcp_header_len)
{
    struct tcp_sock *tp = tcp_sk(sk);

    if (tp->ecn_flags & TCP_ECN_OK) {
        // 新数据段（非重传）：设置 IP 头的 ECT 位
        if (skb->len != tcp_header_len &&
            !before(TCP_SKB_CB(skb)->seq, tp->snd_nxt)) {
            INET_ECN_xmit(sk);                     // 设 IP 头 ECN 位
            if (tp->ecn_flags & TCP_ECN_QUEUE_CWR) {
                tp->ecn_flags &= ~TCP_ECN_QUEUE_CWR;
                th->cwr = 1;                        // 告诉对端已减小 cwnd
                skb_shinfo(skb)->gso_type |= SKB_GSO_TCP_ECN;
            }
        } else if (!tcp_ca_needs_ecn(sk)) {
            INET_ECN_dontxmit(sk);                  // ACK/重传段清 ECT
        }
        // 收到路由器 CE 标记 → 用 ECE 通知对端
        if (tp->ecn_flags & TCP_ECN_DEMAND_CWR)
            th->ece = 1;
    }
}
```

工作流程：

```
发送端                   路由器                   接收端
  │ IP.ECT=1              │                        │
  ├───────────────────────→│ 拥塞！标记 CE          │
  │                        ├───────────────────────→│
  │                        │                        │ 收到 CE
  │                        │      TCP.ECE=1         │
  │←────────────────────────────────────────────────┤
  │ 收到 ECE                                        │
  │ 减小 cwnd                                       │
  │ TCP.CWR=1                                       │
  ├───────────────────────→│                        │
  │                        ├───────────────────────→│
  │                        │                        │ 收到 CWR，停止发 ECE
```

### 18.4 校验和卸载（Checksum Offload）

#### 18.4.1 `CHECKSUM_PARTIAL` 机制

TCP 发送路径使用 `CHECKSUM_PARTIAL` 模式——内核只计算伪头部校验和，真正的校验和由网卡硬件完成。

`ip_summed` 的取值（`include/linux/skbuff.h:220`）：

| 值 | 含义 |
|----|------|
| `CHECKSUM_NONE (0)` | 无校验和信息 |
| `CHECKSUM_UNNECESSARY (1)` | 接收侧：硬件已验证 |
| `CHECKSUM_COMPLETE (2)` | 接收侧：硬件提供了原始和 |
| `CHECKSUM_PARTIAL (3)` | **发送侧：内核填伪头部和，硬件补完** |

#### 18.4.2 `tcp_v4_send_check()` 伪头部校验和

```c
// net/ipv4/tcp_ipv4.c:921-934
void __tcp_v4_send_check(struct sk_buff *skb, __be32 saddr, __be32 daddr)
{
    struct tcphdr *th = tcp_hdr(skb);
    // 伪头部校验和 = 源IP + 目的IP + 协议号 + TCP长度，取反存入 th->check
    th->check = ~tcp_v4_check(skb->len, saddr, daddr, 0);
    // 告诉网卡：从 TCP 头开始算校验和，结果写入 th->check 位置
    skb->csum_start = skb_transport_header(skb) - skb->head;
    skb->csum_offset = offsetof(struct tcphdr, check);
}
```

```
┌──────────────────────────────────────────┐
│ 内核（CHECKSUM_PARTIAL）                  │
│ th->check = ~伪头部校验和                 │
│ csum_start = TCP 头偏移                   │
│ csum_offset = check 字段偏移              │
└─────────────────┬────────────────────────┘
                  │ skb 下发给网卡
                  ▼
┌──────────────────────────────────────────┐
│ 网卡硬件                                  │
│ 从 csum_start 到包尾计算校验和             │
│ 结果写入 csum_start + csum_offset         │
│ （即 th->check 字段）                     │
└──────────────────────────────────────────┘
```

### 18.5 PMTU 发现（Path MTU Discovery）

#### 18.5.1 IP_DF 标志

TCP 默认设置 IP 头的 DF（Don't Fragment）位，禁止中间路由器分片（`net/ipv4/ip_output.c:746`）：

```c
// net/ipv4/ip_output.c:746-748
if (ip_dont_fragment(sk, &rt->dst) && !skb->ignore_df)
    iph->frag_off = htons(IP_DF);      // 不分片
else
    iph->frag_off = 0;                 // 允许分片
```

#### 18.5.2 ICMP 触发 PMTU 更新

当路径上的路由器 MTU 小于包大小时，返回 ICMP "Fragmentation Needed"。`tcp_v4_err()` 处理此消息（`net/ipv4/tcp_ipv4.c:818`）：

```c
// net/ipv4/tcp_ipv4.c:826-837
case ICMP_FRAG_NEEDED:              // PMTU discovery (RFC1191)
    if (sk->sk_state == TCP_LISTEN)
        goto out;
    WRITE_ONCE(tp->mtu_info, info); // 保存新 MTU
    if (!sock_owned_by_user(sk)) {
        tcp_v4_mtu_reduced(sk);     // 立即处理
    } else {
        // 延迟到 release_sock 时处理
        if (!test_and_set_bit(TCP_MTU_REDUCED_DEFERRED, &sk->sk_tsq_flags))
            sock_hold(sk);
    }
```

`tcp_v4_mtu_reduced()` 更新路由 MTU、调整 MSS、触发重传（`net/ipv4/tcp_ipv4.c:600`）：

```c
// net/ipv4/tcp_ipv4.c:622-632
if (inet->pmtudisc != IP_PMTUDISC_DONT &&
    ip_sk_accept_pmtu(sk) &&
    inet_csk(sk)->icsk_pmtu_cookie > mtu) {
    tcp_sync_mss(sk, mtu);          // 根据新 MTU 重算 MSS
    tcp_simple_retransmit(sk);       // 用新 MSS 重传在途包
}
```

#### 18.5.3 MTU 探测（RFC 4821）

`tcp_mtu_probe()` 使用二分法主动探测更大的 MTU（`net/ipv4/tcp_output.c:3142`）。在 `tcp_write_xmit()` 循环开始前调用：

```c
// net/ipv4/tcp_output.c:3155-3163 (进入条件)
if (likely(!icsk->icsk_mtup.enabled ||
           icsk->icsk_mtup.probe_size ||
           inet_csk(sk)->icsk_ca_state != TCP_CA_Open ||
           tcp_snd_cwnd(tp) < 11 ||
           tp->rx_opt.num_sacks || tp->rx_opt.dsack))
    return -1;
```

条件满足时，构建一个大于当前 MSS 的探测 skb，将写队列中的多个小 skb 合并进去：

```c
// net/ipv4/tcp_output.c:3184-3192
probe_size = tcp_mtu_to_mss(sk, (icsk->icsk_mtup.search_high +
                            icsk->icsk_mtup.search_low) >> 1);
// ...
nskb = sk_stream_alloc_skb(sk, probe_size, GFP_ATOMIC, false);
// ... 将写队列中的 skb 数据拷贝合并到 nskb ...
```

如果探测包被 ACK，搜索范围上调；如果丢失，搜索范围下调。

### 18.6 SWS 避免（Silly Window Syndrome）

SWS 是指发送端发送极小的数据段或接收端通告极小的窗口，导致效率极低。

#### 18.6.1 发送端 SWS 避免

`tcp_bound_to_half_wnd()` 限制 `size_goal` 不超过对端接收窗口的一半（`include/net/tcp.h:633`）：

```c
// include/net/tcp.h:633-654
static inline int tcp_bound_to_half_wnd(struct tcp_sock *tp, int pktsize)
{
    int cutoff;

    // 窗口较大时，cutoff = 窗口/2；窗口极小时，cutoff = 全窗口
    if (tp->max_window > TCP_MSS_DEFAULT)
        cutoff = (tp->max_window >> 1);
    else
        cutoff = tp->max_window;

    if (cutoff && pktsize > cutoff)
        return max_t(int, cutoff, 68U - tp->tcp_header_len);
    else
        return pktsize;
}
```

该函数在 `tcp_xmit_size_goal()` 中调用，确保 TSO skb 的 `size_goal` 不会超过接收窗口一半：

```c
// net/ipv4/tcp.c:1137
new_size_goal = tcp_bound_to_half_wnd(tp, new_size_goal);
```

### 18.7 TCP_CORK / TCP_NODELAY 的 setsockopt 实现

#### 18.7.1 TCP_CORK

```c
// net/ipv4/tcp.c:3753-3768
static void __tcp_sock_set_cork(struct sock *sk, bool on)
{
    struct tcp_sock *tp = tcp_sk(sk);
    if (on) {
        tp->nonagle |= TCP_NAGLE_CORK;        // 设置 CORK 标志
    } else {
        tp->nonagle &= ~TCP_NAGLE_CORK;       // 清除 CORK 标志
        if (tp->nonagle & TCP_NAGLE_OFF)
            tp->nonagle |= TCP_NAGLE_PUSH;
        tcp_push_pending_frames(sk);           // 取消 CORK 后立即推送
    }
}
```

#### 18.7.2 TCP_NODELAY

```c
// net/ipv4/tcp.c:3785-3792
static void __tcp_sock_set_nodelay(struct sock *sk, bool on)
{
    if (on) {
        tcp_sk(sk)->nonagle |= TCP_NAGLE_OFF|TCP_NAGLE_PUSH;
        tcp_push_pending_frames(sk);           // 立即推送积压数据
    } else {
        tcp_sk(sk)->nonagle &= ~TCP_NAGLE_OFF;
    }
}
```

**对比**：

| 选项 | 设置时效果 | 清除时效果 |
|------|-----------|-----------|
| `TCP_CORK` | `nonagle |= CORK`，抑制所有小包 | 清 CORK + 立即 push |
| `TCP_NODELAY` | `nonagle |= OFF|PUSH`，立即 push | 清 OFF，恢复 Nagle |

### 18.8 重传机制在发送路径中的角色

#### 18.8.1 重传队列与 RTO 定时器

`tcp_event_new_data_sent()` 将发出的 skb 从写队列移到重传红黑树，并启动/重置 RTO 定时器（见第七章 7.3）。

#### 18.8.2 RTO 超时重传

`tcp_retransmit_timer()` 是 RTO 超时处理函数（`net/ipv4/tcp_timer.c:452`），取重传队列头部 skb 重发：

```c
// net/ipv4/tcp_timer.c:475-478
skb = tcp_rtx_queue_head(sk);
// ...
tcp_enter_loss(sk);
tcp_retransmit_skb(sk, skb, 1);
```

`tcp_retransmit_skb()` 标记 `TCPCB_RETRANS` 并调用 `__tcp_retransmit_skb()` 实际发送（`net/ipv4/tcp_output.c:4236`）：

```c
// net/ipv4/tcp_output.c:4236-4259
int tcp_retransmit_skb(struct sock *sk, struct sk_buff *skb, int segs)
{
    int err = __tcp_retransmit_skb(sk, skb, segs);
    if (err == 0) {
        TCP_SKB_CB(skb)->sacked |= TCPCB_RETRANS;
        tp->retrans_out += tcp_skb_pcount(skb);
    }
    if (!tp->retrans_stamp)
        tp->retrans_stamp = tcp_skb_timestamp(skb);
    tp->undo_retrans += tcp_skb_pcount(skb);
    return err;
}
```

#### 18.8.3 `tcp_rearm_rto()`

每次新数据发出或 ACK 到达时重置 RTO 定时器（`net/ipv4/tcp_input.c:3864`）：

```c
// net/ipv4/tcp_input.c:3864-3890
void tcp_rearm_rto(struct sock *sk)
{
    if (!tp->packets_out) {
        inet_csk_clear_xmit_timer(sk, ICSK_TIME_RETRANS);  // 无在途包，清定时器
    } else {
        u32 rto = inet_csk(sk)->icsk_rto;
        // ... loss probe 时间修正 ...
        tcp_reset_xmit_timer(sk, ICSK_TIME_RETRANS, rto, TCP_RTO_MAX);
    }
}
```

### 18.9 Scatter-Gather（SG）

网卡支持 `NETIF_F_SG` 时，驱动可以直接 DMA 发送 skb 的多个分散的 page frag，无需将 skb 线性化（合并到连续内存）。

#### 18.9.1 e1000 驱动示例

`e1000_xmit_frame()` 遍历 `skb_shinfo(skb)->frags[]`，每个 frag 独立做 DMA 映射（`drivers/net/ethernet/intel/e1000/e1000_main.c:2892`）：

```c
// drivers/net/ethernet/intel/e1000/e1000_main.c:2892-2936 (e1000_tx_map)
for (f = 0; f < nr_frags; f++) {
    const skb_frag_t *frag = &skb_shinfo(skb)->frags[f];
    // ... 按描述符大小拆分 ...
    buffer_info->dma = skb_frag_dma_map(&pdev->dev, frag,
                        offset, size, DMA_TO_DEVICE);
    // 每个 frag → 一个或多个 TX descriptor
}
```

```
skb 内存布局：
┌──────────────┐  ┌──────────┐  ┌──────────┐
│ 线性区 (head) │  │ frag[0]  │  │ frag[1]  │  ...
│ TCP/IP 头     │  │ page 数据 │  │ page 数据 │
└──────┬───────┘  └────┬─────┘  └────┬─────┘
       │               │              │
  dma_map_single   skb_frag_dma_map  skb_frag_dma_map
       │               │              │
       ▼               ▼              ▼
  TX desc[0]       TX desc[1]     TX desc[2]   → 网卡 DMA 引擎按序发送
```

### 18.10 XPS（Transmit Packet Steering）

多队列网卡需要选择 TX 队列。XPS 允许管理员将 CPU 绑定到特定 TX 队列，减少锁竞争和缓存失效。

`netdev_pick_tx()` 的选择逻辑（`net/core/dev.c:4324`）：

```c
// net/core/dev.c:4324-4355
u16 netdev_pick_tx(struct net_device *dev, struct sk_buff *skb,
                   struct net_device *sb_dev)
{
    int queue_index = sk_tx_queue_get(sk);           // ① 尝试复用 socket 缓存的队列

    if (queue_index < 0 || skb->ooo_okay ||
        queue_index >= dev->real_num_tx_queues) {
        int new_index = get_xps_queue(dev, sb_dev, skb); // ② XPS 映射
        if (new_index < 0)
            new_index = skb_tx_hash(dev, sb_dev, skb);   // ③ 哈希后备

        if (queue_index != new_index && sk && sk_fullsock(sk))
            sk_tx_queue_set(sk, new_index);              // ④ 缓存到 socket
        queue_index = new_index;
    }
    return queue_index;
}
```

XPS CPU 映射查找（`net/core/dev.c:4268`）：

```c
// net/core/dev.c:4290-4296
dev_maps = rcu_dereference(sb_dev->xps_maps[XPS_CPUS]);
if (dev_maps) {
    unsigned int tci = skb->sender_cpu - 1;
    queue_index = __get_xps_queue_idx(dev, skb, dev_maps, tci);
}
```

```
TX 队列选择优先级：
  ① sk_tx_queue_get() — socket 缓存的上次选择
  ② get_xps_queue()   — XPS CPU/RX 映射
  ③ skb_tx_hash()     — skb 哈希值取模
```

### 18.11 零拷贝完成通知

`MSG_ZEROCOPY` 模式下，用户态页面被 pin 住供网卡 DMA，发送完成后需通知应用层可以复用缓冲区。

#### 18.11.1 完成回调

TX 完成中断后，`__msg_zerocopy_callback()` 将通知排入 `sk->sk_error_queue`（`net/core/skbuff.c:1292`）：

```c
// net/core/skbuff.c:1303-1310
serr->ee.ee_errno = 0;
serr->ee.ee_origin = SO_EE_ORIGIN_ZEROCOPY;
serr->ee.ee_data = hi;       // 完成范围上界
serr->ee.ee_info = lo;       // 完成范围下界
// ...
q = &sk->sk_error_queue;
__skb_queue_tail(q, skb);
sk_error_report(sk);          // 唤醒等待者（epoll EPOLLERR）
```

#### 18.11.2 用户态读取

应用通过 `recvmsg(fd, &msg, MSG_ERRQUEUE)` 读取完成通知：

```c
// net/ipv4/tcp.c:3060
if (unlikely(flags & MSG_ERRQUEUE))
    return inet_recv_error(sk, msg, len, addr_len);
// → ip_recv_error() → sock_dequeue_err_skb(sk)
```

```
网卡 TX 完成中断
    │
    ▼
__msg_zerocopy_callback()
    → sk->sk_error_queue 入队
    → sk_error_report() 唤醒 epoll
    │
用户态 recvmsg(fd, &msg, MSG_ERRQUEUE)
    → 解析 sock_extended_err
    → ee_info/ee_data 获取可复用的缓冲区范围
```

### 18.12 技术点全景索引

| 技术点 | 所在章节 | 核心函数/源文件 |
|--------|---------|----------------|
| 系统调用入口 | 二 | `__sys_sendto`, `sock_write_iter` |
| 协议分派 | 三 | `inet_sendmsg` |
| Socket 锁 | 四 | `lock_sock` / `release_sock` |
| skb 分配/数据拷贝 | 五 | `sk_stream_alloc_skb`, `skb_copy_to_page_nocache` |
| 零拷贝发送 | 五 | `skb_zerocopy_iter_stream` |
| 发送缓冲区控制 | 五 | `sk_stream_memory_free`, `sk_stream_wait_memory` |
| 阻塞/非阻塞 | 十六 | `sock_sndtimeo`, `timeo` |
| PSH/URG 标志 | 六 | `tcp_mark_push`, `tcp_mark_urg` |
| Autocork | 六/十七 | `tcp_should_autocork` |
| MSG_MORE | 六/十七 | `tcp_push` |
| Pacing / EDT | 七/十八 | `tcp_pacing_check`, `fq_enqueue` |
| 拥塞窗口 cwnd | 七 | `tcp_cwnd_test` |
| 接收窗口 rwnd | 七 | `tcp_snd_wnd_test` |
| Nagle 算法 | 七/十七 | `tcp_nagle_test`, `tcp_nagle_check` |
| TSO defer | 七/十七 | `tcp_tso_should_defer` |
| TSQ | 七/十八 | `tcp_small_queue_check`, `tcp_tasklet_func` |
| TSO autosize | 十七 | `tcp_tso_autosize` |
| TSO fragment | 十七 | `tso_fragment`, `tcp_mss_split_point` |
| GSO 元数据 | 十七 | `tcp_set_skb_tso_segs` |
| skb clone | 八 | `skb_clone` in `__tcp_transmit_skb` |
| TCP 头构建 | 八 | `__tcp_transmit_skb` |
| ECN | 八/十八 | `tcp_ecn_send` |
| 校验和卸载 | 八/十八 | `tcp_v4_send_check`, `CHECKSUM_PARTIAL` |
| 路由查找 | 九 | `__sk_dst_check`, `ip_route_output_ports` |
| IP 头构建 | 九 | `__ip_queue_xmit` |
| Netfilter 钩子 | 九 | `NF_INET_LOCAL_OUT`, `NF_INET_POST_ROUTING` |
| IP 分片 | 九 | `ip_fragment` |
| PMTU 发现 | 九/十八 | `tcp_v4_mtu_reduced`, `tcp_mtu_probe` |
| SWS 避免 | 十七/十八 | `tcp_bound_to_half_wnd` |
| ARP 解析 | 十 | `neigh_resolve_output` |
| 邻居状态机 | 十 | NUD_REACHABLE → NUD_STALE → NUD_PROBE |
| TX 队列选择 | 十一 | `netdev_core_pick_tx` |
| XPS | 十一/十八 | `netdev_pick_tx`, `get_xps_queue` |
| Qdisc 调度 | 十一 | `__qdisc_run`, `qdisc_restart` |
| BQL | 十二 | `dql_queued`, `dql_avail` |
| SG (Scatter-Gather) | 十二/十八 | `skb_frag_dma_map`, `NETIF_F_SG` |
| DMA 映射 | 十二 | `dma_map_single`, `dma_map_page` |
| 硬件 TSO | 十三 | `NETIF_F_TSO` |
| 软件 GSO | 十三/十七 | `skb_gso_segment`, `tcp_gso_segment` |
| TX 完成/资源回收 | 十四 | `e1000_clean_tx_irq`, `napi_consume_skb` |
| 重传 | 十四/十八 | `tcp_retransmit_skb`, `tcp_rearm_rto` |
| 零拷贝完成通知 | 十八 | `__msg_zerocopy_callback`, `MSG_ERRQUEUE` |
| TCP_CORK/TCP_NODELAY | 十七/十八 | `__tcp_sock_set_cork`, `__tcp_sock_set_nodelay` |
| 发送触发机制 | 十九 | `tcp_push`, `tcp_data_snd_check`, `tcp_wfree`, `tcp_retransmit_timer`, `tcp_pace_kick`, `tcp_release_cb` |
| softirq/ksoftirqd | 十九 | `net_tx_action`, `run_ksoftirqd`, `spawn_ksoftirqd` |

---

## 十九、数据从发送缓冲区到网卡的五种触发机制

前面章节分析了数据**如何进入**发送缓冲区（`tcp_sendmsg_locked` → `sk_write_queue`），以及 `tcp_write_xmit` 如何从队列取 skb 并下发到网卡。本章回答一个关键问题：**是谁、在什么时机调用 `tcp_write_xmit()` 把 `sk_write_queue` 中的数据真正发出去？**

**核心结论：Linux 内核没有专门的 "TCP 发送线程"**。数据发送由多种上下文协作完成——用户进程上下文、软中断、tasklet、定时器、ksoftirqd 内核线程，它们在不同时机触发 `tcp_write_xmit()`。

### 19.1 全景概览

```
┌─────────────────────────────────────────────────────────────────────┐
│                  tcp_write_xmit() 的五种入口                         │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ① 用户进程上下文（系统调用路径，最常见）                               │
│     send() → tcp_sendmsg → tcp_push → tcp_write_xmit               │
│                                                                     │
│  ② ACK 触发（收包软中断中）                                           │
│     网卡收到 ACK → NET_RX_SOFTIRQ → tcp_ack()                      │
│       → tcp_data_snd_check() → tcp_push_pending_frames()           │
│       → tcp_write_xmit()                                           │
│                                                                     │
│  ③ TSQ tasklet（TX 完成后延迟发送）                                   │
│     网卡 DMA 完成 → tcp_wfree() → tasklet_schedule()               │
│       → tcp_tasklet_func() → tcp_tsq_write()                       │
│       → tcp_write_xmit()                                           │
│                                                                     │
│  ④ 定时器（重传 / pacing）                                           │
│     tcp_write_timer → tcp_retransmit_timer() → tcp_retransmit_skb()│
│     pacing hrtimer  → tcp_pace_kick() → tcp_tsq_handler()          │
│       → tcp_write_xmit()                                           │
│                                                                     │
│  ⑤ release_sock 延迟处理                                             │
│     用户持锁时 timer/TSQ 设 deferred 标志                             │
│     release_sock() → tcp_release_cb()                               │
│       → tcp_tsq_write() → tcp_write_xmit()                         │
│                                                                     │
│  附: NET_TX_SOFTIRQ（设备层继续发送 + skb 生命周期管理）                │
│     net_tx_action() → qdisc_run() / completion_queue 释放           │
│                                                                     │
│  附: ksoftirqd（每 CPU 软中断后备线程，执行上述所有 softirq handler）    │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 19.2 触发机制①：用户进程上下文直接推送

这是最常见的路径，已在第四至六章详细分析。应用线程调用 `send()`，在**自己的进程上下文**中完成数据拷贝和首次发送尝试：

```
send() → __sys_sendto() → sock_sendmsg() → inet_sendmsg()
  → tcp_sendmsg()
    → lock_sock(sk)
    → tcp_sendmsg_locked()
      → 拷贝数据到 skb → tcp_skb_entail() → sk_write_queue
      → tcp_push()                    ← 第六章
        → __tcp_push_pending_frames()
          → tcp_write_xmit()          ← 第七章：六道关卡
            → tcp_transmit_skb()      ← 同步发送，用户线程直接推
    → release_sock(sk)
      → tcp_release_cb()             ← 可能处理延迟的 TSQ/定时器
```

**关键特征**：

- 执行上下文是调用 `send()` 的用户线程
- 持有 `lock_sock(sk)`，独占 socket
- `tcp_push()` 可能因 autocork（第十七章 17.1.2）而跳过实际推送，留给后续触发机制

### 19.3 触发机制②：ACK 到达触发发送新数据

当对端 ACK 到达时，确认号（`snd_una`）前进，释放了拥塞窗口和接收窗口的空间。内核在收包软中断（`NET_RX_SOFTIRQ`）中处理 ACK，并检查是否可以继续发送 `sk_write_queue` 中的待发数据。

#### 19.3.1 触发入口：`tcp_data_snd_check()`

```c
// net/ipv4/tcp_input.c:6522
static inline void tcp_data_snd_check(struct sock *sk)
{
    tcp_push_pending_frames(sk);    // → __tcp_push_pending_frames → tcp_write_xmit
    tcp_check_space(sk);            // 检查缓冲区空间，可能唤醒写者
}
```

#### 19.3.2 调用点

`tcp_data_snd_check()` 在多个 ACK 处理路径上被调用：

- **快速路径**（纯 ACK）：`tcp_rcv_established()` 中 `tcp_ack()` 处理后直接调用
- **慢速路径**：`tcp_rcv_state_process()` 中状态机处理后调用

#### 19.3.3 `tcp_check_space()`：唤醒阻塞写者

ACK 确认了已发送的 skb，`tcp_ack()` 内部调用 `tcp_clean_rtx_queue()` 释放重传队列中的 skb，减小 `sk_wmem_queued`。随后 `tcp_check_space()` 检查是否需要唤醒因缓冲区满而等待的写者：

```c
// net/ipv4/tcp_input.c:6510
void tcp_check_space(struct sock *sk)
{
    smp_mb();
    if (sk->sk_socket &&
        test_bit(SOCK_NOSPACE, &sk->sk_socket->flags)) {
        tcp_new_space(sk);    // → sk_stream_write_space() → 唤醒 epoll/阻塞写者
    }
}
```

`tcp_new_space()` 最终调用 `sk_stream_write_space()`（`net/core/stream.c:31`），清除 `SOCK_NOSPACE` 标志并唤醒等待队列：

```c
// net/core/stream.c:31
void sk_stream_write_space(struct sock *sk)
{
    struct socket *sock = sk->sk_socket;
    if (__sk_stream_is_writeable(sk, 1) && sock) {
        clear_bit(SOCK_NOSPACE, &sock->flags);
        wake_up_interruptible_poll(&wq->wait, EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND);
    }
}
```

**此机制包含两条互补路径**：

| 路径 | 作用 | 受益者 |
|------|------|--------|
| `tcp_push_pending_frames()` | 内核主动将 `sk_write_queue` 中积压的数据发出 | 已在缓冲区的待发数据 |
| `sk_stream_write_space()` | 唤醒阻塞写者或产生 `EPOLLOUT` 事件 | 因缓冲区满而等待的应用进程 |

### 19.4 触发机制③：TSQ tasklet（TX 完成后延迟发送）

TSQ（TCP Small Queues）机制在第十八章 18.2 已介绍其**限流**功能。本节聚焦其另一面——**解除限流后如何触发继续发送**。

#### 19.4.1 触发链路

```
网卡 DMA 完成 → 驱动释放克隆体 skb → kfree_skb
  → skb->destructor = tcp_wfree()          ← net/ipv4/tcp_output.c:1554
    → sk_wmem_alloc 减小
    → 检查 TSQ_THROTTLED 标志
    → 将 socket 挂到 per-CPU tsq_tasklet 链表
    → tasklet_schedule()                    ← 触发 TASKLET_SOFTIRQ
      → tcp_tasklet_func()                  ← net/ipv4/tcp_output.c:1425
        → tcp_tsq_handler()                 ← net/ipv4/tcp_output.c:1408
          → tcp_tsq_write()                 ← net/ipv4/tcp_output.c:1379
            → tcp_xmit_retransmit_queue()   ← 优先处理丢包重传
            → tcp_write_xmit()              ← 继续发送新数据
```

#### 19.4.2 `tcp_wfree()`：TX 完成回调

```c
// net/ipv4/tcp_output.c:1554
void tcp_wfree(struct sk_buff *skb)
{
    struct sock *sk = skb->sk;
    struct tcp_sock *tp = tcp_sk(sk);

    // 减小 sk_wmem_alloc（设备层排队字节计数）
    WARN_ON(refcount_sub_and_test(skb->truesize - 1, &sk->sk_wmem_alloc));

    // ksoftirqd 压力保护：设备层积压仍多时跳过唤醒，避免频繁触发
    if (refcount_read(&sk->sk_wmem_alloc) >= SKB_TRUESIZE(1) && this_cpu_ksoftirqd() == current)
        goto out;

    for (oval = READ_ONCE(sk->sk_tsq_flags);; oval = nval) {
        // 未被 throttle 或已入队：不重复触发
        if (!(oval & TSQF_THROTTLED) || (oval & TSQF_QUEUED))
            goto out;
        // 原子操作：清 THROTTLED + 设 QUEUED
        nval = (oval & ~TSQF_THROTTLED) | TSQF_QUEUED;
        nval = cmpxchg(&sk->sk_tsq_flags, oval, nval);
        if (nval != oval)
            continue;
        // 挂到 per-CPU tasklet 链表，调度软中断
        tsq = this_cpu_ptr(&tsq_tasklet);
        list_add(&tp->tsq_node, &tsq->head);
        if (empty)
            tasklet_schedule(&tsq->tasklet);
        return;
    }
out:
    sk_free(sk);
}
```

#### 19.4.3 `tcp_tsq_write()`：TSQ 恢复后的发送

```c
// net/ipv4/tcp_output.c:1379
static void tcp_tsq_write(struct sock *sk)
{
    if ((1 << sk->sk_state) &
        (TCPF_ESTABLISHED | TCPF_FIN_WAIT1 | TCPF_CLOSING |
         TCPF_CLOSE_WAIT  | TCPF_LAST_ACK)) {
        struct tcp_sock *tp = tcp_sk(sk);
        // 优先处理丢包重传
        if (tp->lost_out > tp->retrans_out &&
            tcp_snd_cwnd(tp) > tcp_packets_in_flight(tp)) {
            tcp_mstamp_refresh(tp);
            tcp_xmit_retransmit_queue(sk);
        }
        // 继续发送新数据
        tcp_write_xmit(sk, tcp_current_mss(sk), tp->nonagle, 0, GFP_ATOMIC);
    }
}
```

#### 19.4.4 TSQ tasklet 的初始化

TSQ tasklet 在 TCP 子系统初始化时为每个 CPU 创建：

```c
// net/ipv4/tcp_output.c:1510
void __init tcp_tasklet_init(void)
{
    int i;
    for_each_possible_cpu(i) {
        struct tsq_tasklet *tsq = &per_cpu(tsq_tasklet, i);
        INIT_LIST_HEAD(&tsq->head);
        tasklet_setup(&tsq->tasklet, tcp_tasklet_func);
    }
}
```

调用路径：`tcp_init()` → `tcp_tasklet_init()`，在内核启动 `inet_init()` 阶段完成。

### 19.5 触发机制④：定时器触发

#### 19.5.1 RTO 重传定时器

当发送的数据包在 RTO 超时内未收到 ACK，写定时器到期触发重传。注意重传发送的是**重传队列**（`tcp_rtx_queue`）中的数据，而非 `sk_write_queue` 中的新数据。

```
timer softirq → tcp_write_timer()                  ← net/ipv4/tcp_timer.c:647
  → bh_lock_sock(sk)
  → sock_owned_by_user(sk)?
    ├─ 否 → tcp_write_timer_handler()               ← net/ipv4/tcp_timer.c:614
    │         → ICSK_TIME_RETRANS 分支
    │           → tcp_retransmit_timer()             ← net/ipv4/tcp_timer.c:459
    │             → tcp_enter_loss(sk)
    │             → tcp_retransmit_skb(sk, skb, 1)   ← 重传队首 skb
    └─ 是 → set TCP_WRITE_TIMER_DEFERRED            ← 延迟到 release_sock
```

```c
// net/ipv4/tcp_timer.c:647
static void tcp_write_timer(struct timer_list *t)
{
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

#### 19.5.2 Pacing 高精度定时器

TCP pacing 机制（第十八章 18.1）通过 hrtimer 控制发送速率。当 `tcp_pacing_check()` 判断发送时间未到时，启动 hrtimer；定时器到期后通过 TSQ handler 继续发送：

```c
// net/ipv4/tcp_output.c:1614
enum hrtimer_restart tcp_pace_kick(struct hrtimer *timer)
{
    struct tcp_sock *tp = container_of(timer, struct tcp_sock, pacing_timer);
    struct sock *sk = (struct sock *)tp;

    tcp_tsq_handler(sk);    // → tcp_tsq_write() → tcp_write_xmit()
    sock_put(sk);

    return HRTIMER_NORESTART;
}
```

### 19.6 触发机制⑤：`release_sock` 延迟处理

当 timer/TSQ 触发时，如果 socket 正被用户态线程 `lock_sock()` 占用，这些处理不能立即执行（会死锁）。内核通过 `sk_tsq_flags` 的 deferred 标志位记录待处理事项，等用户态线程 `release_sock()` 时统一处理：

```c
// net/ipv4/tcp_output.c:1462
void tcp_release_cb(struct sock *sk)
{
    unsigned long flags, nflags;

    do {
        flags = sk->sk_tsq_flags;
        if (!(flags & TCP_DEFERRED_ALL))
            return;
        nflags = flags & ~TCP_DEFERRED_ALL;
    } while (cmpxchg(&sk->sk_tsq_flags, flags, nflags) != flags);

    if (flags & TCPF_TSQ_DEFERRED) {
        tcp_tsq_write(sk);                  // TSQ 延迟：继续发送
        __sock_put(sk);
    }

    sock_release_ownership(sk);

    if (flags & TCPF_WRITE_TIMER_DEFERRED) {
        tcp_write_timer_handler(sk);        // 写定时器延迟：重传处理
        __sock_put(sk);
    }
    if (flags & TCPF_DELACK_TIMER_DEFERRED) {
        tcp_delack_timer_handler(sk);       // 延迟 ACK 定时器
        __sock_put(sk);
    }
    if (flags & TCPF_MTU_REDUCED_DEFERRED) {
        inet_csk(sk)->icsk_af_ops->mtu_reduced(sk);  // PMTU 减小处理
        __sock_put(sk);
    }
}
```

四种 deferred 标志的含义：

| 标志 | 设置场景 | 延迟执行的操作 |
|------|---------|---------------|
| `TCPF_TSQ_DEFERRED` | `tcp_tsq_handler()` 发现 socket 被锁 | `tcp_tsq_write()` → `tcp_write_xmit()` |
| `TCPF_WRITE_TIMER_DEFERRED` | `tcp_write_timer()` 发现 socket 被锁 | `tcp_write_timer_handler()` → 重传 |
| `TCPF_DELACK_TIMER_DEFERRED` | `tcp_delack_timer()` 发现 socket 被锁 | `tcp_delack_timer_handler()` → 发 ACK |
| `TCPF_MTU_REDUCED_DEFERRED` | `tcp_v4_err()` 收到 ICMP 时 socket 被锁 | `mtu_reduced()` → PMTU 更新 |

### 19.7 附属机制：NET_TX_SOFTIRQ 与 ksoftirqd

以上五种机制负责在 TCP 层触发 `tcp_write_xmit()`。包交给 IP 层和设备层后，还有两个底层机制参与实际的网卡发送调度。

#### 19.7.1 NET_TX_SOFTIRQ：设备层发送软中断

在 `net_dev_init()` 中注册（`net/core/dev.c:12269`）：

```c
// net/core/dev.c:12269
open_softirq(NET_TX_SOFTIRQ, net_tx_action);
```

`NET_TX_SOFTIRQ` **不直接调用 `tcp_write_xmit()`**，它负责两件事：

| 职责 | 处理对象 | 间接效果 |
|------|---------|---------|
| `completion_queue` 释放 | 已完成 TX 的 skb | `skb->destructor`（`tcp_wfree`）→ 可能触发 TSQ tasklet → `tcp_write_xmit()` |
| `output_queue` 继续发送 | 被调度的 Qdisc | `qdisc_run()` → `dev_hard_start_xmit()` → 网卡驱动 |

触发时机：
- Qdisc 配额用完：`__qdisc_run()` → `__netif_schedule()` → `raise_softirq_irqoff(NET_TX_SOFTIRQ)`
- 网卡 TX 完成：驱动调用 `netif_wake_queue()` → `__netif_schedule()` → `raise_softirq_irqoff(NET_TX_SOFTIRQ)`
- 硬中断中延迟释放 skb：`__dev_kfree_skb_irq()` → `raise_softirq_irqoff(NET_TX_SOFTIRQ)`

#### 19.7.2 ksoftirqd：每 CPU 软中断后备线程

`ksoftirqd` 不是 TCP 专用发送线程，而是**所有软中断的后备执行体**。当软中断在中断返回路径处理不完（超时或次数超限），由 `ksoftirqd` 内核线程接管。

**创建时机**：boot 早期通过 `early_initcall` 创建，每个 CPU 一个：

```c
// kernel/softirq.c:958
static struct smp_hotplug_thread softirq_threads = {
    .store          = &ksoftirqd,
    .thread_should_run  = ksoftirqd_should_run,
    .thread_fn      = run_ksoftirqd,
    .thread_comm        = "ksoftirqd/%u",
};

static __init int spawn_ksoftirqd(void)
{
    cpuhp_setup_state_nocalls(CPUHP_SOFTIRQ_DEAD, "softirq:dead", NULL,
                  takeover_tasklets);
    BUG_ON(smpboot_register_percpu_thread(&softirq_threads));
    return 0;
}
early_initcall(spawn_ksoftirqd);
```

**执行逻辑**：有 pending 的 softirq 时，调用 `__do_softirq()` 遍历所有 pending handler：

```c
// kernel/softirq.c:912
static void run_ksoftirqd(unsigned int cpu)
{
    ksoftirqd_run_begin();
    if (local_softirq_pending()) {
        __do_softirq();
        ksoftirqd_run_end();
        cond_resched();
        return;
    }
    ksoftirqd_run_end();
}
```

因此 TSQ tasklet（`TASKLET_SOFTIRQ`）、`NET_TX_SOFTIRQ`、timer softirq 等都可能在 `ksoftirqd` 上下文中执行。

### 19.8 五种机制对照表

| 机制 | 触发者 | 执行上下文 | 关键函数（源文件:行号） | 发送对象 |
|------|--------|-----------|----------------------|---------|
| ① syscall push | 应用调用 `send()` | 用户进程，持 sock 锁 | `tcp_push`（`tcp.c:821`）→ `tcp_write_xmit`（`tcp_output.c:3456`） | `sk_write_queue` 新数据 |
| ② ACK 触发 | 收到对端 ACK | softirq（NET_RX 处理） | `tcp_data_snd_check`（`tcp_input.c:6522`）→ `tcp_write_xmit` | `sk_write_queue` 待发数据 |
| ③ TSQ tasklet | 网卡 TX 完成释放 skb | TASKLET_SOFTIRQ | `tcp_wfree`（`tcp_output.c:1554`）→ `tcp_tasklet_func`（`1425`）→ `tcp_tsq_write`（`1379`） | 新数据 + 重传 |
| ④a 写定时器 | RTO 超时 | timer softirq | `tcp_write_timer`（`tcp_timer.c:647`）→ `tcp_retransmit_timer`（`459`） | 重传队列队首 |
| ④b pacing 定时器 | pacing 时间到 | hrtimer softirq | `tcp_pace_kick`（`tcp_output.c:1614`）→ `tcp_tsq_handler` → `tcp_write_xmit` | `sk_write_queue` |
| ⑤ release_sock | 用户 `release_sock()` | 用户进程 | `tcp_release_cb`（`tcp_output.c:1462`）→ `tcp_tsq_write` / `tcp_write_timer_handler` | 上述延迟项 |
| 附: NET_TX_SOFTIRQ | Qdisc 调度 / TX 完成 | softirq / ksoftirqd | `net_tx_action`（`dev.c:5346`） | Qdisc 排队包 + skb 释放 |
| 附: ksoftirqd | softirq 积压 | 每 CPU 内核线程 | `run_ksoftirqd`（`softirq.c:912`） | 执行上述 softirq handler |

### 19.9 典型场景时序图

以非阻塞 socket 发送大量数据为例，展示各机制的协作：

```
时间轴 →

用户线程                    软中断/tasklet              ksoftirqd
   │                           │                          │
   ├─ send(128KB)              │                          │
   │  tcp_sendmsg_locked:      │                          │
   │  拷贝数据 → sk_write_queue │                          │
   │  tcp_push → tcp_write_xmit│                          │
   │  ├─ 发送 skb_1 (cwnd 允许) │                          │
   │  ├─ 发送 skb_2             │                          │
   │  ├─ TSQ check: 设备层积压  │                          │
   │  │  set TSQ_THROTTLED      │                          │
   │  └─ 停止发送，返回 copied  │                          │
   │                           │                          │
   │  return copied (< 128KB)  │                          │
   │  = 短写，用户需重试        │                          │
   │                           │                          │
   │                    ┌──────┤                          │
   │                    │ 网卡 TX 完成中断                 │
   │                    │ → tcp_wfree()                   │
   │                    │   清 THROTTLED, 设 QUEUED        │
   │                    │   tasklet_schedule()             │
   │                    │                                 │
   │                    │ TASKLET_SOFTIRQ:                 │
   │                    │ → tcp_tasklet_func()             │
   │                    │   → tcp_tsq_write()              │
   │                    │     → tcp_write_xmit()           │
   │                    │       发送 skb_3, skb_4 ...      │
   │                    └──────┤                          │
   │                           │                          │
   │                    ┌──────┤                          │
   │                    │ 收到 ACK (NET_RX_SOFTIRQ)        │
   │                    │ → tcp_ack()                      │
   │                    │   → tcp_clean_rtx_queue()        │
   │                    │     释放已确认 skb               │
   │                    │   → tcp_data_snd_check()         │
   │                    │     → tcp_write_xmit()           │
   │                    │       继续发 sk_write_queue 数据  │
   │                    │     → tcp_check_space()           │
   │                    │       → sk_stream_write_space()   │
   │                    │         EPOLLOUT 事件 → 用户线程  │
   │                    └──────┤                          │
   │                           │                          │
   ├─ epoll 返回 EPOLLOUT      │                          │
   │  再次 send() 剩余数据     │                          │
   │  ...                      │                          │
```

### 19.10 小结

| 问题 | 答案 |
|------|------|
| 用户线程 `send()` 后数据在哪？ | `sk_write_queue`（发送缓冲区），同时在 syscall 上下文尝试首次推送 |
| 内核从发送缓冲区取数据的"线程"是谁？ | **没有专门线程**，由用户进程、softirq、tasklet、定时器等多种上下文事件驱动 |
| 数据可能被发送的时机有哪些？ | ① syscall 直接 push ② ACK 到达 ③ TSQ tasklet ④ 重传/pacing 定时器 ⑤ release_sock 延迟处理 |
| `ksoftirqd` 是什么？谁创建的？ | boot 时 `early_initcall(spawn_ksoftirqd)` 创建的每 CPU 内核线程，作为所有 softirq 的后备执行者，不是 TCP 专用 |
| 为什么不用专门的发送线程？ | 事件驱动模型更高效——只在有事件（ACK/TX完成/定时器）时才执行，避免轮询开销和线程调度延迟 |
