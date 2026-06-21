+++
date = '2026-04-16'
title = 'TCP 三次握手源码分析'
weight = 7
tags = [
    "TCP",
    "三次握手",
    "connect",
    "SYN",
    "SYN-ACK",
]
categories = [
    "网络",
]
+++
# TCP 三次握手源码分析

> 基于 Linux 5.15.78，专注分析 `connect()` 触发的 TCP 三次握手全过程。
> Socket 创建参见 [socket创建与fd分配](socket创建与fd分配.md)，Bind 参见 [TCP-bind操作](TCP-bind操作.md)，Listen 参见 [TCP-listen操作](TCP-listen操作.md)。
> 半连接/全连接队列、SYN Cookie、溢出排查详见 [TCP-listen操作](TCP-listen操作.md) 五~七节。

---

## 三次握手时序图（点击跳转）

```
时间   客户端                                   服务器
 │
 │     connect()
 │     ├─> __sys_connect() ─────────── ① Connect入口
 │     ├─> tcp_v4_connect()
 │     │   ├─> 路由查找/选端口/生成ISN
 │     │   └─> tcp_connect()
 │     │
 │     │   ─── [SYN, seq=x] ──────────> ② 客户端发送SYN
 │     │                                     tcp_v4_rcv()
 │     │                                     └─> tcp_rcv_state_process()
 │     │                                         ├─> tcp_v4_conn_request()
 │     │                                         ├─> tcp_conn_request()
 │     │                                         │   ├─> 创建request_sock
 │     │                                         │   ├─> 加入半连接队列
 │     │                                         │   └─> tcp_v4_send_synack()
 │     │
 │     │   <──── [SYN-ACK, seq=y, ack=x+1] ──── ③ 服务端收SYN发SYN-ACK
 │     │
 │     tcp_v4_rcv()
 │     └─> tcp_rcv_state_process()
 │         ├─> tcp_rcv_synsent_state_process()
 │         ├─> tcp_finish_connect()
 │         │   └─> 状态: ESTABLISHED
 │         └─> tcp_send_ack()
 │
 │         ─── [ACK, ack=y+1] ──────────────> ④ 客户端收SYN-ACK发ACK
 │                                             tcp_v4_rcv()
 │                                             └─> tcp_check_req()
 │                                                 ├─> tcp_v4_syn_recv_sock()
 │                                                 │   └─> 创建完整sock
 │                                                 ├─> 加入accept队列  ── ⑤ 服务端收ACK
 │                                                 │   └─> 状态: ESTABLISHED
 │                                                 └─> sk->sk_data_ready()
 │
 │     连接建立完成，双方状态: ESTABLISHED
```

| 步骤                   | 方向              | 详细分析（点击跳转）                                                                                                                         |
| -------------------- | --------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| ① Connect 入口         | —               | [一、Connect 入口](#sec-connect)                                                                                                       |
| ② 客户端发送 SYN          | Client → Server | [三、客户端发送 SYN](#sec-syn)（[`tcp_v4_connect`](#sec-tcp_v4_connect) → [`tcp_connect`](#sec-tcp_connect)）                               |
| ③ 服务端收 SYN 发 SYN-ACK | Server → Client | [四、服务端收 SYN 发 SYN-ACK](#sec-synack)（[`tcp_conn_request`](#sec-tcp_conn_request) → [`tcp_v4_send_synack`](#sec-tcp_v4_send_synack)） |
| ④ 客户端收 SYN-ACK 发 ACK | Client → Server | [五、客户端收 SYN-ACK 发 ACK](#sec-ack)（[`tcp_rcv_synsent_state_process`](#sec-tcp_rcv_synsent)）                                          |
| ⑤ 服务端收 ACK 完成握手      | Server          | [5.3 服务端收最终 ACK](#sec-tcp_check_req)（[`tcp_check_req`](#sec-tcp_check_req)）                                                        |
| 安全机制                 | —               | [八、安全机制](#sec-security)（SYN Cookie / ISN 防预测）                                                                                      |
| 选项协商                 | —               | [七、TCP 选项协商](#sec-options)（MSS / WScale / SACK / Timestamps）                                                                       |

---

<a id="sec-connect"></a>

## 一、Connect 入口

### 1.1 调用链

```
用户态: connect(sockfd, addr, addrlen)
    ↓
SYSCALL_DEFINE3(connect)
    ↓
__sys_connect()                        [net/socket.c:2034]
    ↓
__sys_connect_file()
    ↓
sock->ops->connect()
    ↓
inet_stream_connect()                  [net/ipv4/af_inet.c]
    ↓
tcp_v4_connect()                       [net/ipv4/tcp_ipv4.c:402]
    ↓
tcp_connect()                          [net/ipv4/tcp_output.c:4895]  → 发送 SYN
```

### 1.2 `__sys_connect()`

```c
// net/socket.c:2034
int __sys_connect(int fd, struct sockaddr __user *uservaddr, int addrlen)
{
    int ret = -EBADF;
    struct fd f;

    /* 通过fd获取file结构（同时增加引用计数） */
    f = fdget(fd);
    if (f.file) {
        struct sockaddr_storage address;

        /* 将用户空间地址复制到内核空间（防止用户态竞态修改） */
        ret = move_addr_to_kernel(uservaddr, addrlen, &address);
        if (!ret)
            /* 最终调用 sock->ops->connect()，TCP对应inet_stream_connect()
             * → tcp_v4_connect() */
            ret = __sys_connect_file(f.file, &address, addrlen, 0);
        fdput(f);  /* 释放file引用 */
    }

    return ret;
}
```

### 1.3 Connect 的前置条件：阻塞 socket 状态机与路由绑定

#### 阻塞 socket 能否同时 connect 两个地址？

`__inet_stream_connect()` (`net/ipv4/af_inet.c:698`) 通过 `sock->state` 状态机控制：

```c
// net/ipv4/af_inet.c:720
switch (sock->state) {
case SS_CONNECTED:
    err = -EISCONN;        // 已建立连接，拒绝重复 connect
    goto out;
case SS_CONNECTING:
    err = -EALREADY;       // 正在连接中（阻塞等待 SYN-ACK）
    break;
case SS_UNCONNECTED:
    // 唯一允许发起 connect 的状态
    if (sk->sk_state != TCP_CLOSE)
        goto out;          // 内核状态非 CLOSE，拒绝
    err = sk->sk_prot->connect(sk, uaddr, addr_len);  // → tcp_v4_connect()
    sock->state = SS_CONNECTING;
    break;
}
// 阻塞等待
timeo = sock_sndtimeo(sk, flags & O_NONBLOCK);  // 阻塞socket: timeo > 0
inet_wait_for_connect(sk, timeo, writebias);     // 睡眠直到 SYN-ACK 或超时
```

**执行序列**：

```
第一次 connect(addr1):
  SS_UNCONNECTED → tcp_v4_connect() → 发 SYN → SYN_SENT
  → sock->state = SS_CONNECTING → inet_wait_for_connect() 阻塞

第二次 connect(addr2):  // 第一次仍在阻塞
  sock->state == SS_CONNECTING → 直接返回 -EALREADY
```

**结论**：阻塞 socket 不可能同时向两个地址发起连接。第二次 `connect()` 在状态检查阶段就被拒绝（`-EALREADY`）。多地址并行连接必须用非阻塞 socket + epoll/select。

#### bind 不绑定路由

`__inet_bind()` (`net/ipv4/af_inet.c:530`) 只做四件事，**不涉及任何路由函数**：

```c
// net/ipv4/af_inet.c:530-599
1. inet_addr_type_table()          // 验证地址类型（必须本机地址，除非 IP_FREEBIND）
2. inet->inet_saddr = addr         // 设置源地址（发送用）
3. inet->inet_rcv_saddr = addr     // 设置收包匹配地址
4. sk->sk_prot->get_port()         // 绑定端口，加入 bhash
```

没有调用 `ip_route_*` 系列函数。bind 只设置本地地址和端口，不查路由、不选出口设备。这也是为什么 `bind("0.0.0.0")` 能正常工作 — 源地址和路由都推迟到 connect 时确定。

#### 路由绑定的时机

| 时机 | 调用路径 | 说明 |
|------|---------|------|
| **connect()** | `tcp_v4_connect()` → `ip_route_connect()` → `ip_route_newports()` → `sk_setup_caps()` | 主动连接时确定并缓存路由（最常见） |
| **发送前检查** | `tcp_transmit_skb()` → `sk_dst_check()` | 每次发送验证路由缓存有效性 |
| **PMTU 变化** | `tcp_v4_mtu_reduced()` → `sk_dst_reset()` | MTU 减小时缓存失效，下次发送重新查找 |
| **服务端收 SYN** | `tcp_conn_request()` → `af_ops->route_req()` | 为 SYN-ACK 查找回客户端的路由（临时） |
| **服务端 accept** | `tcp_v4_syn_recv_sock()` → `inet_csk_route_child_sock()` | 为子 socket 绑定持久路由 |

路由是 **connect 时绑定、send 时验证** 的懒缓存模型。`tcp_v4_connect()` 中的关键代码：

```c
// net/ipv4/tcp_ipv4.c:204
rt = ip_route_connect(fl4, nexthop, inet->inet_saddr,
                      RT_CONN_FLAGS(sk), sk->sk_bound_dev_if,
                      IPPROTO_TCP, orig_sport, orig_dport, sk);
// ...
// 端口确定后重新验证路由（某些路由策略基于端口号）
rt = ip_route_newports(fl4, rt, orig_sport, orig_dport,
                       inet->inet_sport, inet->inet_dport, sk);
// 缓存到 socket
sk_setup_caps(sk, &rt->dst);
```

---

## 二、三次握手报文格式

| | 第一个包 (SYN) | 第二个包 (SYN-ACK) | 第三个包 (ACK) |
|---|---|---|---|
| 方向 | Client → Server | Server → Client | Client → Server |
| SYN | 1 | 1 | 0 |
| ACK | 0 | 1 | 1 |
| seq | ISN_client | ISN_server | ISN_client+1 |
| ack | 0 | ISN_client+1 | ISN_server+1 |
| 选项 | MSS, WScale, SACK, TS | MSS, WScale, SACK, TS | Timestamps |

---

<a id="sec-syn"></a>

## 三、第一步：客户端发送 SYN

### 3.1 调用链

```
用户空间: connect(sockfd, addr, addrlen)
    ↓
__sys_connect()                                [net/socket.c:2034]
    ├── sockfd_lookup_light()   // fd → struct socket
    ├── move_addr_to_kernel()   // 用户地址拷贝到内核
    └── sock->ops->connect()    // TCP 对应 inet_stream_connect()
        ↓
inet_stream_connect()                            [net/ipv4/af_inet.c]
    └── __inet_stream_connect()
        └── tcp_v4_connect()                     [net/ipv4/tcp_ipv4.c:402]
            ├── ip_route_connect()   // 路由查找 + 选源 IP
            ├── inet_hash_connect() // 选临时端口 + 加入 ehash
            ├── secure_tcp_seq()    // 生成不可预测 ISN (RFC6528)
            └── tcp_connect()                    [net/ipv4/tcp_output.c:4895]
                ├── tcp_connect_init()         // 设置 MSS/窗口/RTO
                ├── tcp_init_nondata_skb()     // SYN 占 1 个 seq
                ├── tcp_transmit_skb()         // 发送 SYN 包
                └── inet_csk_reset_xmit_timer()// 启动 SYN 重传定时器
```

<a id="sec-tcp_v4_connect"></a>

### 3.2 `tcp_v4_connect()`

```c
// net/ipv4/tcp_ipv4.c:402
int tcp_v4_connect(struct sock *sk, struct sockaddr *uaddr, int addr_len)
{
    struct sockaddr_in *usin = (struct sockaddr_in *)uaddr;
    struct inet_sock *inet = inet_sk(sk);
    struct tcp_sock *tp = tcp_sk(sk);
    __be16 orig_sport, orig_dport;
    __be32 daddr, nexthop;
    struct flowi4 *fl4;
    struct rtable *rt;
    int err;
    struct ip_options_rcu *inet_opt;
    struct inet_timewait_death_row *tcp_death_row = &sock_net(sk)->ipv4.tcp_death_row;

    /* 验证地址长度和地址族 */
    if (addr_len < sizeof(struct sockaddr_in))
        return -EINVAL;
    if (usin->sin_family != AF_INET)
        return -EAFNOSUPPORT;

    nexthop = daddr = usin->sin_addr.s_addr;
    inet_opt = rcu_dereference_protected(inet->inet_opt,
                                         lockdep_sock_is_held(sk));
    /* 处理源路由选项：如果设置了Loose/Strict Source Route，
     * 第一跳是opt.faddr而非最终目的地 */
    if (inet_opt && inet_opt->opt.srr) {
        if (!daddr)
            return -EINVAL;
        nexthop = inet_opt->opt.faddr;
    }

    orig_sport = inet->inet_sport;   /* 可能为0（未bind） */
    orig_dport = usin->sin_port;
    fl4 = &inet->cork.fl.u.ip4;

    /* 查找路由：确定出口设备和源IP地址 */
    rt = ip_route_connect(fl4, nexthop, inet->inet_saddr,
                          RT_CONN_FLAGS(sk), sk->sk_bound_dev_if,
                          IPPROTO_TCP, orig_sport, orig_dport, sk);
    if (IS_ERR(rt)) {
        err = PTR_ERR(rt);
        if (err == -ENETUNREACH)
            IP_INC_STATS(sock_net(sk), IPSTATS_MIB_OUTNOROUTES);
        return err;
    }

    /* TCP是点对点协议，不能连接到多播或广播地址 */
    if (rt->rt_flags & (RTCF_MULTICAST | RTCF_BROADCAST)) {
        ip_rt_put(rt);
        return -ENETUNREACH;
    }

    if (!inet_opt || !inet_opt->opt.srr)
        daddr = fl4->daddr;

    /* 设置本地地址（如果还未bind，使用路由选择的源IP） */
    if (!inet->inet_saddr)
        inet->inet_saddr = fl4->saddr;
    sk_rcv_saddr_set(sk, inet->inet_saddr);

    /* 目标地址改变时，重置时间戳状态（socket复用场景） */
    if (tp->rx_opt.ts_recent_stamp && inet->inet_daddr != daddr) {
        tp->rx_opt.ts_recent       = 0;
        tp->rx_opt.ts_recent_stamp = 0;
        if (likely(!tp->repair))
            WRITE_ONCE(tp->write_seq, 0);
    }

    inet->inet_dport = usin->sin_port;
    sk_daddr_set(sk, daddr);

    inet_csk(sk)->icsk_ext_hdr_len = 0;
    if (inet_opt)
        inet_csk(sk)->icsk_ext_hdr_len = inet_opt->opt.optlen;

    /* MSS上限默认536，后续SYN-ACK协商可能调大 */
    tp->rx_opt.mss_clamp = TCP_MSS_DEFAULT;

    /* 核心状态转换：CLOSE → SYN_SENT */
    tcp_set_state(sk, TCP_SYN_SENT);

    /* 选择本地端口并加入ehash（用于收包时查找socket）
     * 如果inet_sport为0，从临时端口范围自动选择，
     * 同时检查端口冲突（考虑TIME_WAIT重用） */
    err = inet_hash_connect(tcp_death_row, sk);
    if (err)
        goto failure;

    sk_set_txhash(sk);

    /* 端口确定后重新验证路由（某些路由策略基于端口号） */
    rt = ip_route_newports(fl4, rt, orig_sport, orig_dport,
                           inet->inet_sport, inet->inet_dport, sk);
    if (IS_ERR(rt)) { ... goto failure; }

    sk->sk_gso_type = SKB_GSO_TCPV4;
    sk_setup_caps(sk, &rt->dst);

    /* 生成初始序列号（ISN）:
     * secure_tcp_seq()基于SipHash(saddr, daddr, sport, dport, secret)，
     * 不可预测，防止TCP劫持（RFC6528）
     * 同时生成时间戳偏移，防止通过时间戳推断系统启动时间 */
    if (likely(!tp->repair)) {
        WRITE_ONCE(tp->write_seq,
                   secure_tcp_seq(inet->inet_saddr, inet->inet_daddr,
                                  inet->inet_sport, usin->sin_port));
        tp->tsoffset = secure_tcp_ts_off(sock_net(sk),
                                         inet->inet_saddr, inet->inet_daddr);
    }

    inet->inet_id = get_random_u16();

    /* 构建并发送SYN包 */
    err = tcp_connect(sk);
    // ...
}
```

<a id="sec-tcp_connect"></a>

### 3.3 `tcp_connect()`

```c
// net/ipv4/tcp_output.c:4895
int tcp_connect(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct sk_buff *buff;
    int err;

    /* BPF钩子：通知eBPF程序有新连接（可修改拥塞算法、设置选项等） */
    tcp_call_bpf(sk, BPF_SOCK_OPS_TCP_CONNECT_CB, 0, NULL);

    /* 验证路由缓存是否仍然有效，失效则重新查找 */
    if (inet_csk(sk)->icsk_af_ops->rebuild_header(sk))
        return -EHOSTUNREACH;

    /* 初始化TCP连接参数：头部长度/MSS/窗口/序列号/RTO */
    tcp_connect_init(sk);

    /* repair模式(CRIU容器迁移)：TCP状态已由用户空间设置，跳过SYN发送 */
    if (unlikely(tp->repair)) {
        tcp_finish_connect(sk, NULL);
        return 0;
    }

    /* 分配SYN包：size=0(无载荷)，force_schedule=true(SYN必须发送) */
    buff = sk_stream_alloc_skb(sk, 0, sk->sk_allocation, true);
    if (unlikely(!buff))
        return -ENOBUFS;

    /* 设置SYN标志和序列号。write_seq++：SYN占用1个序列号空间
     * （TCP协议规定SYN和FIN各占1个序列号） */
    tcp_init_nondata_skb(buff, tp->write_seq++, TCPHDR_SYN);

    tcp_mstamp_refresh(tp);
    /* retrans_stamp：记录SYN首次发送时间，用于RTT计算和超时判断 */
    tp->retrans_stamp = tcp_time_stamp(tp);

    /* 加入发送队列：更新内存计费和packets_out */
    tcp_connect_queue_skb(sk, buff);
    /* ECN协商：如果sysctl_tcp_ecn开启，在SYN中设置ECE+CWR标志 */
    tcp_ecn_send_syn(sk, buff);
    /* 加入重传红黑树(tcp_rtx_queue)，超时后从树中取出重传 */
    tcp_rbtree_insert(&sk->tcp_rtx_queue, buff);

    /* 发送SYN包：
     * TFO路径：tcp_send_syn_data() — SYN携带数据+cookie，减少1个RTT
     * 普通路径：tcp_transmit_skb() — 纯SYN包
     * clone_it=1：克隆skb发送，原始保留在重传队列中 */
    err = tp->fastopen_req ? tcp_send_syn_data(sk, buff) :
          tcp_transmit_skb(sk, buff, 1, sk->sk_allocation);
    if (err == -ECONNREFUSED)
        return err;

    WRITE_ONCE(tp->snd_nxt, tp->write_seq);
    tp->pushed_seq = tp->write_seq;

    /* 启动SYN重传定时器：初始超时icsk_rto（默认1秒），
     * 指数退避重传，最多tcp_syn_retries次（默认6次，总约63秒） */
    tcp_send_head(sk);
    inet_csk_reset_xmit_timer(sk, ICSK_TIME_RETRANS,
                              inet_csk(sk)->icsk_rto, TCP_RTO_MAX);
    return 0;
}
```

**SYN 包格式**:
- `SYN=1, ACK=0`
- `seq=ISN`
- `options`: MSS, Window Scale, SACK Permitted, Timestamp

---

<a id="sec-synack"></a>

## 四、第二步：服务端接收 SYN 并发送 SYN-ACK

### 4.1 调用链

```
tcp_v4_rcv()
    ↓
tcp_v4_do_rcv()
    ↓
tcp_rcv_state_process()                // TCP_LISTEN 状态
    ↓
icsk->icsk_af_ops->conn_request()
    ↓
tcp_v4_conn_request()
    ↓
tcp_conn_request()                     [net/ipv4/tcp_input.c:8296-8490]
    1. 检查 SYN Cookie（防洪水攻击）
    2. 创建 request_sock（半连接）
    3. 生成服务器 ISN
    4. 加入 SYN 队列
    ↓
tcp_v4_send_synack()
    5. 构建 SYN-ACK 包
    6. 发送
```

### 4.2 `tcp_v4_conn_request()`

```c
// net/ipv4/tcp_ipv4.c:1992
int tcp_v4_conn_request(struct sock *sk, struct sk_buff *skb)
{
    /* TCP是点对点协议，不响应发送到广播或多播地址的SYN
     * （也是防止放大攻击的措施） */
    if (skb_rtable(skb)->rt_flags & (RTCF_BROADCAST | RTCF_MULTICAST))
        goto drop;

    /* 调用通用的TCP连接请求处理函数
     * tcp_request_sock_ops: request_sock操作函数表
     * tcp_request_sock_ipv4_ops: IPv4专用操作（路由、发送SYN-ACK等） */
    return tcp_conn_request(&tcp_request_sock_ops,
                            &tcp_request_sock_ipv4_ops, sk, skb);
drop:
    tcp_listendrop(sk);
    return 0;
}
```

<a id="sec-tcp_conn_request"></a>

### 4.3 `tcp_conn_request()`

```c
// net/ipv4/tcp_input.c:8296
int tcp_conn_request(struct request_sock_ops *rsk_ops,
                     const struct tcp_request_sock_ops *af_ops,
                     struct sock *sk, struct sk_buff *skb)
{
    __u32 isn = TCP_SKB_CB(skb)->tcp_tw_isn;  /* TIME_WAIT复用场景的预设ISN */
    struct tcp_options_received tmp_opt;
    struct tcp_sock *tp = tcp_sk(sk);
    struct request_sock *req;
    bool want_cookie = false;
    struct dst_entry *dst;

    /* ===== 1. 半连接队列容量检查 =====
     * 队列满 或 管理员强制Cookie(syncookies==2) 时启用SYN Cookie。
     * isn!=0(TIME_WAIT复用)跳过检查——旧连接已验证过对端合法性 */
    if ((syncookies == 2 || inet_csk_reqsk_queue_is_full(sk)) && !isn) {
        want_cookie = tcp_syn_flood_action(sk, rsk_ops->slab_name);
        if (!want_cookie)
            goto drop;
    }

    /* ===== 2. 全连接队列(accept queue)满检查 =====
     * 应用来不及accept()消费，丢弃SYN比完成握手后再丢弃更高效 */
    if (sk_acceptq_is_full(sk)) {
        NET_INC_STATS(sock_net(sk), LINUX_MIB_LISTENOVERFLOWS);
        goto drop;
    }

    /* ===== 3. 创建request_sock(半连接socket) =====
     * request_sock约256B，远小于完整sock(~2KB)，是抵御SYN洪水的关键设计。
     * !want_cookie: 非Cookie模式才关联到监听socket(增加半连接计数)，
     *   Cookie模式下req只是临时使用(构造SYN-ACK)，发送后立即释放 */
    req = inet_reqsk_alloc(rsk_ops, sk, !want_cookie);
    if (!req)
        goto drop;

    /* ===== 4. 解析SYN包中的TCP选项 =====
     * Cookie模式不解析Fast Open cookie(传NULL)，因为无法在ACK到达时验证 */
    tcp_clear_options(&tmp_opt);
    tmp_opt.mss_clamp = af_ops->mss_clamp;
    tmp_opt.user_mss  = tp->rx_opt.user_mss;
    tcp_parse_options(sock_net(sk), skb, &tmp_opt, 0,
                      want_cookie ? NULL : &foc);

    /* SYN Cookie + 无时间戳 = 丢失所有选项信息:
     * Cookie通过时间戳的TSecr字段编码窗口缩放和SACK等信息，
     * 对端不支持时间戳则只能清除所有选项 */
    if (want_cookie && !tmp_opt.saw_tstamp)
        tcp_clear_options(&tmp_opt);

    tcp_openreq_init(req, &tmp_opt, skb, sk);

    /* ===== 5. 查找SYN-ACK的发送路由 ===== */
    dst = af_ops->route_req(sk, skb, &fl, req);
    if (!dst)
        goto drop_and_free;

    /* ===== 6. 生成服务端ISN =====
     * SYN Cookie场景: ISN由cookie_init_sequence生成(编码连接状态到32位)
     * 普通场景: secure_tcp_seq()基于SipHash生成不可预测的ISN(RFC6528) */
    if (!want_cookie && !isn)
        isn = af_ops->init_seq(skb);
    else if (want_cookie)
        isn = cookie_init_sequence(af_ops, sk, skb, &req->mss);

    tcp_rsk(req)->snt_isn = isn;  /* 记录ISN，收到ACK时验证: ack_seq == isn+1 */

    /* ===== 7. 将req加入半连接哈希表(非Cookie模式) =====
     * req伪装成mini socket(TCP_NEW_SYN_RECV)插入ehash，
     * 后续客户端ACK通过ehash查找到这个req。
     * 同时启动SYN-ACK重传定时器(默认1秒) */
    if (!want_cookie)
        inet_csk_reqsk_queue_hash_add(sk, req,
            tcp_timeout_init((struct sock *)req));

    /* ===== 8. 发送SYN-ACK ===== */
    af_ops->send_synack(sk, dst, &fl, req, &foc,
                        !want_cookie ? TCP_SYNACK_NORMAL : TCP_SYNACK_COOKIE,
                        skb);

    /* Cookie模式: SYN-ACK已发送，状态编码在ISN中，不需要保存req，
     * 直接释放。客户端ACK到达时从ack_seq-1解码重建连接 */
    if (want_cookie) {
        reqsk_free(req);
        return 0;
    }
    reqsk_put(req);
    return 0;
}
```

<a id="sec-tcp_v4_send_synack"></a>

### 4.4 `tcp_v4_send_synack()`

```c
// net/ipv4/tcp_ipv4.c:1402
static int tcp_v4_send_synack(const struct sock *sk, struct dst_entry *dst,
                              struct flowi *fl, struct request_sock *req,
                              struct tcp_fastopen_cookie *foc,
                              enum tcp_synack_type synack_type,
                              struct sk_buff *syn_skb)
{
    const struct inet_request_sock *ireq = inet_rsk(req);
    struct flowi4 fl4;
    int err = -1;
    struct sk_buff *skb;
    u8 tos;   /* ★ 必须在调用 ip_build_and_send_pkt 前声明并初始化 */

    /* 1. 获取回复路由（如果调用者未提供） */
    if (!dst && (dst = inet_csk_route_req(sk, &fl4, req)) == NULL)
        return -1;

    /* 2. 构建SYN-ACK数据包:
     * - 分配skb
     * - 填充TCP头(seq=服务端ISN, ack=客户端ISN+1, SYN=1, ACK=1)
     * - 添加TCP选项(MSS, WScale, SACK, Timestamps)
     * - TFO模式下可能携带cookie或数据 */
    skb = tcp_make_synack(sk, dst, req, foc, synack_type, syn_skb);

    if (skb) {
        /* 3. 计算TCP校验和（伪首部校验和） */
        __tcp_v4_send_check(skb, ireq->ir_loc_addr, ireq->ir_rmt_addr);

        /* 4. 设置TOS（Type of Service）
         * sysctl_tcp_reflect_tos=1: 反射客户端SYN中的TOS(剥ECN位) + 本端ECN位
         * sysctl_tcp_reflect_tos=0: 直接用本端socket的tos */
        tos = READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_reflect_tos) ?
                (tcp_rsk(req)->syn_tos & ~INET_ECN_MASK) |
                (inet_sk(sk)->tos & INET_ECN_MASK) :
                inet_sk(sk)->tos;

        /* BPF拥塞控制可能需要ECN(例如DCTCP) */
        if (!INET_ECN_is_capable(tos) &&
            tcp_bpf_ca_needs_ecn((struct sock *)req))
            tos |= INET_ECN_ECT_0;

        /* 5. 构建IP头并发送（RCU读锁保护ireq_opt解引用） */
        rcu_read_lock();
        err = ip_build_and_send_pkt(skb, sk, ireq->ir_loc_addr,
                                     ireq->ir_rmt_addr,
                                     rcu_dereference(ireq->ireq_opt), tos);
        rcu_read_unlock();
        err = net_xmit_eval(err);
    }
    return err;
}
```

**SYN-ACK 包格式**:
- `SYN=1, ACK=1`
- `seq=ISN_server`
- `ack=ISN_client+1`（确认客户端的 SYN）
- `options`: 与 SYN 协商的选项

---

<a id="sec-ack"></a>

## 五、第三步：客户端接收 SYN-ACK 并发送 ACK

### 5.1 调用链

```
tcp_v4_rcv()
    ↓
tcp_v4_do_rcv()
    ↓
tcp_rcv_state_process()                // TCP_SYN_SENT 状态
    ↓
tcp_rcv_synsent_state_process()
    1. 验证 ACK 合法性
    2. 处理 SYN-ACK 选项
    3. 更新 TCP 状态 → ESTABLISHED
    ↓
tcp_finish_connect()
    4. 通知应用层（sk->sk_state_change）
    ↓
tcp_send_ack()
    5. 发送最终 ACK
```

<a id="sec-tcp_rcv_synsent"></a>

### 5.2 `tcp_rcv_synsent_state_process()`

```c
// net/ipv4/tcp_input.c:7299
static int tcp_rcv_synsent_state_process(struct sock *sk, struct sk_buff *skb,
                                         const struct tcphdr *th)
{
    struct inet_connection_sock *icsk = inet_csk(sk);
    struct tcp_sock *tp = tcp_sk(sk);
    struct tcp_fastopen_cookie foc = { .len = -1 };
    /* 保存原始mss_clamp：如果后续验证失败需要恢复，
     * 避免错误的选项值影响SYN重传 */
    int saved_clamp = tp->rx_opt.mss_clamp;
    bool fastopen_fail;

    /* 解析SYN-ACK中的TCP选项 */
    tcp_parse_options(sock_net(sk), skb, &tp->rx_opt, 0, &foc);
    /* 对端在SYN-ACK中原样回显了我方时间戳，减去偏移还原为本地真实时间 */
    if (tp->rx_opt.saw_tstamp && tp->rx_opt.rcv_tsecr)
        tp->rx_opt.rcv_tsecr -= tp->tsoffset;

    if (th->ack) {
        /* ===== 收到SYN-ACK（正常三次握手） ===== */

        /* RFC793 ACK验证：SYN_SENT状态下 ack_seq 只能是 ISS+1 */
        if (!after(TCP_SKB_CB(skb)->ack_seq, tp->snd_una) ||
            after(TCP_SKB_CB(skb)->ack_seq, tp->snd_nxt)) {
            /* ACK无效，加速SYN重传 */
            if (icsk->icsk_retransmits == 0)
                inet_csk_reset_xmit_timer(sk, ICSK_TIME_RETRANS,
                                          TCP_TIMEOUT_MIN, TCP_RTO_MAX);
            goto reset_and_undo;
        }

        /* PAWS检查：回显时间戳必须在[retrans_stamp, now]范围内，
         * 否则是对过期SYN的响应 */
        if (tp->rx_opt.saw_tstamp && tp->rx_opt.rcv_tsecr &&
            !between(tp->rx_opt.rcv_tsecr, tp->retrans_stamp,
                     tcp_time_stamp(tp))) {
            NET_INC_STATS(sock_net(sk), LINUX_MIB_PAWSACTIVEREJECTED);
            goto reset_and_undo;
        }

        /* RST+ACK：对端明确拒绝连接（目标端口未监听） */
        if (th->rst) {
            tcp_reset(sk, skb);
            goto discard;
        }

        /* ACK有效但没有SYN标志：不是合法的SYN-ACK，丢弃 */
        if (!th->syn)
            goto discard_and_undo;

        /* ===== SYN-ACK合法，开始建立连接 ===== */

        tcp_ecn_rcv_synack(tp, th);           /* ECN协商结果 */
        tcp_init_wl(tp, TCP_SKB_CB(skb)->seq); /* 窗口更新基准 */
        tcp_try_undo_spurious_syn(sk);         /* 检查虚假超时 */
        tcp_ack(sk, skb, FLAG_SLOWPATH);       /* 处理ACK：更新snd_una，停止重传定时器 */

        /* 更新接收序列号：SYN占1个序列号，期望下一个数据seq+1 */
        WRITE_ONCE(tp->rcv_nxt, TCP_SKB_CB(skb)->seq + 1);
        tp->rcv_wup = TCP_SKB_CB(skb)->seq + 1;

        /* SYN和SYN-ACK中的窗口值是未缩放的原始值 */
        tp->snd_wnd = ntohs(th->window);

        if (!tp->rx_opt.wscale_ok) {
            /* 对端不支持窗口缩放：双方都不使用，窗口限制65535 */
            tp->rx_opt.snd_wscale = tp->rx_opt.rcv_wscale = 0;
            tp->window_clamp = min(tp->window_clamp, 65535U);
        }

        if (tp->rx_opt.saw_tstamp) {
            tp->rx_opt.tstamp_ok = 1;
            /* 时间戳选项占12字节，需要从advmss中扣除 */
            tp->tcp_header_len =
                sizeof(struct tcphdr) + TCPOLEN_TSTAMP_ALIGNED;
            tp->advmss -= TCPOLEN_TSTAMP_ALIGNED;
            tcp_store_ts_recent(tp);  /* 保存对端时间戳用于后续PAWS */
        } else {
            tp->tcp_header_len = sizeof(struct tcphdr);
        }

        /* 根据PMTU重新计算MSS */
        tcp_sync_mss(sk, icsk->icsk_pmtu_cookie);
        tcp_initialize_rcv_mss(sk);

        /* 必须先设copied_seq再改状态，防止tcp_poll()
         * 看到ESTABLISHED但copied_seq=0导致误判 */
        WRITE_ONCE(tp->copied_seq, tp->rcv_nxt);
        smp_mb();  /* 写屏障：确保上述字段对其他CPU可见 */

        /* 完成连接：SYN_SENT → ESTABLISHED
         * 内部初始化拥塞窗口、拥塞控制算法、启动keepalive定时器 */
        tcp_finish_connect(sk, skb);

        /* 通知应用层：唤醒在connect()中阻塞等待的进程 */
        if (!sock_flag(sk, SOCK_DEAD)) {
            sk->sk_state_change(sk);
            sk_wake_async(sk, SOCK_WAKE_IO, POLL_OUT);
        }

        /* 发送三次握手的最终ACK */
        tcp_send_ack(sk);
        return -1;
    }

    /* ===== 分支B: 收到纯SYN(无ACK) — 同时打开(simultaneous open) ===== */
    // ... SYN_SENT → SYN_RECV，此处省略
}
```

**最终 ACK 包格式**:
- `SYN=0, ACK=1`
- `seq=ISN_client+1`
- `ack=ISN_server+1`

<a id="sec-tcp_check_req"></a>

### 5.3 服务端接收最终 ACK — `tcp_check_req()`

```
tcp_v4_rcv()  → 在ehash中查找到request_sock(TCP_NEW_SYN_RECV状态)
    ↓
tcp_check_req()  → 验证ACK → 创建完整子sock → 加入accept队列
    ↓
sk->sk_data_ready()  → 唤醒accept()阻塞
```

```c
// net/ipv4/tcp_minisocks.c:691
struct sock *tcp_check_req(struct sock *sk, struct sk_buff *skb,
                           struct request_sock *req,
                           bool fastopen, bool *req_stolen)
{
    struct tcp_options_received tmp_opt;
    struct sock *child;
    const struct tcphdr *th = tcp_hdr(skb);
    __be32 flg = tcp_flag_word(th) & (TCP_FLAG_RST|TCP_FLAG_SYN|TCP_FLAG_ACK);
    bool paws_reject = false;
    bool own_req;   /* ★ 标识本CPU是否拥有此req（用于inet_csk_complete_hashdance） */

    /* 解析TCP选项，进行PAWS检查 */
    tmp_opt.saw_tstamp = 0;   /* ★ 必须初始化：后续 if (tmp_opt.saw_tstamp) 依赖 */
    if (th->doff > (sizeof(struct tcphdr)>>2)) {
        tcp_parse_options(sock_net(sk), skb, &tmp_opt, 0, NULL);
        if (tmp_opt.saw_tstamp) {
            tmp_opt.ts_recent = req->ts_recent;
            if (tmp_opt.rcv_tsecr)
                tmp_opt.rcv_tsecr -= tcp_rsk(req)->ts_off;
            paws_reject = tcp_paws_reject(&tmp_opt, th->rst);
        }
    }

    /* 纯SYN重传：客户端没收到SYN-ACK，重新发送了SYN。
     * 重传SYN-ACK作为响应，重置定时器 */
    if (TCP_SKB_CB(skb)->seq == tcp_rsk(req)->rcv_isn &&
        flg == TCP_FLAG_SYN && !paws_reject) {
        if (!inet_rtx_syn_ack(sk, req))
            mod_timer_pending(&req->rsk_timer, ...);
        return NULL;
    }

    /* ACK验证：ack_seq必须等于snt_isn+1（确认我方SYN-ACK） */
    if ((flg & TCP_FLAG_ACK) && !fastopen &&
        (TCP_SKB_CB(skb)->ack_seq != tcp_rsk(req)->snt_isn + 1))
        return sk;  /* ACK无效，调用者发送RST */

    /* PAWS拒绝或窗口外的包：发送ACK并丢弃 */
    if (paws_reject || !tcp_in_window(...))
        // ...

    /* RST或SYN：终止半连接 */
    if (flg & (TCP_FLAG_RST|TCP_FLAG_SYN))
        goto embryonic_reset;

    /* 必须有ACK标志 */
    if (!(flg & TCP_FLAG_ACK))
        return NULL;

    /* TCP_DEFER_ACCEPT：设置了defer_accept时，纯ACK(无数据)被暂时忽略，
     * 等待客户端发送数据后再创建完整socket（减少空闲连接） */
    if (req->num_timeout < inet_csk(sk)->icsk_accept_queue.rskq_defer_accept &&
        TCP_SKB_CB(skb)->end_seq == tcp_rsk(req)->rcv_isn + 1) {
        inet_rsk(req)->acked = 1;
        return NULL;
    }

    /* ===== ACK有效，创建完整的子socket =====
     * syn_recv_sock回调（IPv4是tcp_v4_syn_recv_sock）:
     * 1. 创建新的struct sock（~2KB）
     * 2. 初始化TCP状态（拷贝选项、窗口、MSS等）
     * 3. 将新socket加入ehash（已建立连接哈希表）
     * 状态变为 TCP_ESTABLISHED */
    child = inet_csk(sk)->icsk_af_ops->syn_recv_sock(sk, skb, req, NULL,
                                                     req, &own_req);
    if (!child)
        goto listen_overflow;  /* accept队列满 */

    /* 完成握手：从半连接哈希表移到全连接accept队列，
     * 唤醒阻塞在accept()的进程 */
    return inet_csk_complete_hashdance(sk, child, req, own_req);
}
```

此时服务端从 `request_sock`（~256B）转为完整 `struct sock`（~2KB），状态变为 `TCP_ESTABLISHED`。`accept()` 返回新连接的文件描述符。

---

<a id="sec-options"></a>

## 六、TCP 选项协商

三次握手期间，双方通过 SYN/SYN-ACK 中的 TCP 选项完成参数协商。

### 6.1 MSS (Maximum Segment Size)

- 客户端在 SYN 中发送 MSS（通常 1460 = 1500 MTU - 20 IP - 20 TCP）
- 服务器在 SYN-ACK 中发送自己的 MSS
- 双方取较小值

```c
// tcp_v4_connect() 中（tcp_ipv4.c:221）：MSS 上限默认 536（RFC1122/RFC2581）
// 收到 SYN-ACK 时由 tcp_parse_options() 重新 clamp 到对端通告值
tp->rx_opt.mss_clamp = TCP_MSS_DEFAULT;

// tcp_connect_init() 中（tcp_output.c:4696）：advmss 从路由 metric 计算
// 受用户 user_mss (setsockopt TCP_MAXSEG) 与 TCP_MSS_DEFAULT 下限约束
tp->advmss = tcp_mss_clamp(tp, dst_metric_advmss(dst));
```

### 6.2 Window Scale

- 缩放因子范围: 0-14，允许窗口超过 65535 字节
- 只有双方都在 SYN 中发送才启用

```c
// tcp_select_initial_window()
if (sysctl_tcp_window_scaling) {
    tp->rx_opt.rcv_wscale = rcv_wscale;
}
```

### 6.3 SACK (Selective Acknowledgment)

- 允许接收方确认非连续数据块，提高丢包恢复效率
- 双方在 SYN 中发送 SACK Permitted 选项

```c
// tcp_parse_options()
if (opcode == TCPOPT_SACK_PERM) {
    tp->rx_opt.sack_ok = 1;
}
```

### 6.4 Timestamps

- 用于 RTT 精确测量和 PAWS（Protection Against Wrapped Sequence）
- 双方在 SYN 中发送

```c
// tcp_parse_options()
if (opcode == TCPOPT_TIMESTAMP) {
    tp->rx_opt.tstamp_ok = 1;
    tp->rx_opt.rcv_tsval = get_unaligned_be32(ptr);
    tp->rx_opt.rcv_tsecr = get_unaligned_be32(ptr + 4);
}
```

---

<a id="sec-security"></a>

## 七、安全机制

### 7.1 SYN Cookie

**目的**：防止 SYN 洪水攻击耗尽服务器内存。

**原理**：
1. 不分配 `request_sock`，避免内存耗尽
2. 将连接信息编码到 ISN 中（加密哈希）
3. 收到第三次 ACK 时，从 `ack_seq - 1` 反向恢复连接参数

```c
// tcp_conn_request()
/* syncookies 取值：0=禁用, 1=按需(默认), 2=强制
 * syncookies==2: 管理员强制所有SYN都用Cookie(测试/极端防护),
 *                不受队列状态影响
 * isn!=0: TIME_WAIT复用场景, 旧连接已验证对端合法性,
 *         跳过Cookie检查直接用预设ISN */
if ((syncookies == 2 || inet_csk_reqsk_queue_is_full(sk)) && !isn) {
    want_cookie = tcp_syn_flood_action(sk, rsk_ops->slab_name);
    if (!want_cookie)
        goto drop;   // syncookies=0 时直接丢 SYN（无兜底）
}

/* 后续: ISN 生成阶段才会调用 cookie_init_sequence()
 * 见 tcp_input.c:8441
 *     isn = cookie_init_sequence(af_ops, sk, skb, &req->mss); */
```

### 7.2 半连接队列与全连接队列

半连接/全连接队列的数据结构、计数器、溢出判断、溢出排查详见 [TCP-listen操作](TCP-listen操作.md) 五~七节，此处仅列要点：

- 半连接计数器 `qlen`（`atomic_t`）与全连接计数器 `sk_ack_backlog`（`u32`）**完全独立**，互不影响
- 两者共享同一上限 `sk_max_ack_backlog`（= `min(backlog, somaxconn)`）
- 半连接满判断用 `>=`，全连接满判断用 `>`（全连接实际可容纳 `backlog + 1` 个）
- `ListenOverflows` 仅在全连接满时递增；`ListenDrops` 在所有监听路径 drop 时递增（粗粒度）

### 7.3 ISN 防预测

```c
WRITE_ONCE(tp->write_seq,
           secure_tcp_seq(inet->inet_saddr, inet->inet_daddr,
                          inet->inet_sport, usin->sin_port));
```

`secure_tcp_seq()` 基于 SipHash + 时钟确保 ISN 不可预测，防止 TCP 劫持。

---

## 八、TCP Fast Open

在 SYN 中携带数据，减少一个 RTT。

1. 首次连接：客户端获取 Fast Open Cookie
2. 后续连接：SYN 中包含 Cookie + 数据
3. 服务器验证 Cookie 后立即处理数据

```c
// tcp_connect()
err = tp->fastopen_req ? tcp_send_syn_data(sk, buff) :
      tcp_transmit_skb(sk, buff, 1, sk->sk_allocation);
```

---

## 九、错误处理

### 9.1 SYN 超时重传

- 默认重传次数：`tcp_syn_retries = 6`
- 超时时间指数增长：1s → 2s → 4s → 8s → 16s → 32s
- 总超时约 63 秒

```c
inet_csk_reset_xmit_timer(sk, ICSK_TIME_RETRANS,
                          inet_csk(sk)->icsk_rto, TCP_RTO_MAX);
```

### 9.2 连接拒绝（RST）

目标端口未监听时，服务器发送 RST：

```c
if (th->rst) {
    tcp_reset(sk, skb);
    goto discard;
}
```

### 9.3 网络不可达

路由查找失败时返回 `ENETUNREACH`：

```c
rt = ip_route_connect(...);
if (IS_ERR(rt)) {
    err = PTR_ERR(rt);
    if (err == -ENETUNREACH)
        IP_INC_STATS(sock_net(sk), IPSTATS_MIB_OUTNOROUTES);
    return err;
}
```

---

## 十、相关系统参数

```bash
# SYN 重传次数
net.ipv4.tcp_syn_retries = 6

# SYN-ACK 重传次数
net.ipv4.tcp_synack_retries = 5

# SYN 队列上限
net.ipv4.tcp_max_syn_backlog = 1024

# SYN Cookie 启用
net.ipv4.tcp_syncookies = 1

# TCP Fast Open
net.ipv4.tcp_fastopen = 1

# TCP 选项
net.ipv4.tcp_timestamps = 1
net.ipv4.tcp_window_scaling = 1
net.ipv4.tcp_sack = 1

# Accept 队列上限
net.core.somaxconn = 4096
```

---

## 十一、调试工具

```bash
# 抓取三次握手包
tcpdump -i eth0 -nn 'port 8080 and (tcp[tcpflags] & (tcp-syn|tcp-ack) != 0)'

# 查看半连接/全连接队列
ss -tan state syn-recv
ss -ltn     # Recv-Q = accept 队列中连接数, Send-Q = backlog 上限

# 监控队列溢出
nstat -az | grep -i listen

# 查看 TCP 统计
netstat -st | grep -i syn
```

---

## 十二、状态转换总结

```
客户端:
CLOSED → [connect()/发送SYN] → SYN_SENT → [收到SYN-ACK/发送ACK] → ESTABLISHED

服务器:
CLOSED → [listen()] → LISTEN → [收到SYN/发送SYN-ACK] → SYN_RECV → [收到ACK] → ESTABLISHED
```
