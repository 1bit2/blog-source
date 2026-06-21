+++
date = '2026-04-16'
title = 'Linux 内核 TCP 协议实现架构分析'
weight = 1
tags = [
    "TCP",
    "架构",
    "tcp_input",
    "tcp_output",
    "tcp_sock",
]
categories = [
    "网络",
]
+++
# Linux 内核 TCP 协议实现架构分析

> 基于 Linux 5.15.78 内核源码

## 目录

- [1. 设计理念](#1-设计理念)
- [2. 整体架构](#2-整体架构)
- [3. 源码文件组织](#3-源码文件组织)
- [4. 核心数据结构](#4-核心数据结构)
- [5. 连接生命周期管理](#5-连接生命周期管理)
- [6. 数据发送路径](#6-数据发送路径)
- [7. 数据接收路径](#7-数据接收路径)
- [8. 拥塞控制框架](#8-拥塞控制框架)
- [9. 定时器体系](#9-定时器体系)
- [10. 关键设计模式](#10-关键设计模式)
- [附录：关键文件索引](#附录关键文件索引)

---

## 1. 设计理念

Linux 内核的 TCP 实现遵循以下核心设计理念：

### 1.1 分层解耦

```
用户空间                    socket()/connect()/send()/recv()/close()
─────────────────────────────────────────────────────────────────────
BSD Socket 层               struct socket + proto_ops (inet_stream_ops)
─────────────────────────────────────────────────────────────────────
传输层 (TCP)                struct sock/tcp_sock + struct proto (tcp_prot)
─────────────────────────────────────────────────────────────────────
网络层 (IP)                 ip_queue_xmit / ip_local_deliver
─────────────────────────────────────────────────────────────────────
链路层                      dev_queue_xmit / netif_receive_skb
```

- **BSD Socket 层**（`net/socket.c`）：提供 POSIX 兼容的系统调用接口
- **INET 层**（`net/ipv4/af_inet.c`）：IPv4 协议族的 socket 适配，通过 `proto_ops` 衔接上层
- **TCP 协议层**：核心状态机、拥塞控制、可靠传输等纯 TCP 逻辑
- **IP 层**：路由、分片、Netfilter 等网络层功能

### 1.2 面向对象的 C 语言设计

通过 **结构体嵌套首成员** 实现类似继承的效果：

```
struct sock → struct inet_sock → struct inet_connection_sock → struct tcp_sock
```

同一块内存可安全地在不同层之间强制类型转换（`(struct tcp_sock *)sk`），每一层只关心自己的字段。

### 1.3 可插拔的拥塞控制

通过 `struct tcp_congestion_ops` 函数指针表实现策略模式，支持运行时动态切换拥塞算法（CUBIC、BBR、Reno 等），甚至通过 BPF 注入自定义算法。

### 1.4 快路径/慢路径分离

收包路径针对最常见场景（按序到达、无特殊选项）设计了 **快路径**（`tcp_rcv_established` 中的 `pred_flags` 匹配），避免不必要的分支和锁开销。

### 1.5 零拷贝与延迟优化

- 发送侧：`MSG_ZEROCOPY`、`sendfile`/`splice` 支持
- 接收侧：page fragment 机制、GRO/LRO 合并
- TSO/GSO 卸载减少 CPU 开销

---

## 2. 整体架构

### 2.1 TCP 子系统架构全景

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           用户空间 (User Space)                              │
│         socket()   connect()   send()   recv()   close()   setsockopt()     │
└─────────────────────────┬───────────────────────────────────────────────────┘
                          │ 系统调用 (syscall)
┌─────────────────────────▼───────────────────────────────────────────────────┐
│                      BSD Socket 层 (net/socket.c)                           │
│                     struct socket + struct proto_ops                         │
│                        inet_stream_ops (TCP)                                │
└─────────────────────────┬───────────────────────────────────────────────────┘
                          │
┌─────────────────────────▼───────────────────────────────────────────────────┐
│                    INET 层 (net/ipv4/af_inet.c)                             │
│              inet_create / inet_stream_connect / inet_release               │
│             ┌───────────────────────────────────────────────┐               │
│             │   struct proto: tcp_prot (tcp_ipv4.c)        │               │
│             │   .connect = tcp_v4_connect                   │               │
│             │   .close   = tcp_close                        │               │
│             │   .sendmsg = tcp_sendmsg                      │               │
│             │   .recvmsg = tcp_recvmsg                      │               │
│             │   .backlog_rcv = tcp_v4_do_rcv                │               │
│             └───────────────────────────────────────────────┘               │
└─────────────────────────┬───────────────────────────────────────────────────┘
                          │
┌─────────────────────────▼───────────────────────────────────────────────────┐
│                         TCP 协议核心                                        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌───────────────┐  │
│  │  连接管理     │  │  数据发送     │  │  数据接收     │  │   定时器       │  │
│  │ tcp_ipv4.c   │  │ tcp_output.c │  │ tcp_input.c  │  │  tcp_timer.c  │  │
│  │ tcp.c        │  │              │  │              │  │               │  │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └───────┬───────┘  │
│         │                 │                 │                   │          │
│  ┌──────▼─────────────────▼─────────────────▼───────────────────▼───────┐  │
│  │                    拥塞控制框架 (tcp_cong.c)                          │  │
│  │  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────────┐  │  │
│  │  │  CUBIC  │ │   BBR   │ │  Reno   │ │  DCTCP  │ │ BPF (自定义) │  │  │
│  │  └─────────┘ └─────────┘ └─────────┘ └─────────┘ └─────────────┘  │  │
│  └────────────────────────────────────────────────────────────────────┘  │
│  ┌────────────────────────┐  ┌────────────────────────────────────────┐  │
│  │  丢包恢复               │  │  速率估计                              │  │
│  │  tcp_recovery.c (RACK) │  │  tcp_rate.c (send/ack rate)           │  │
│  └────────────────────────┘  └────────────────────────────────────────┘  │
│  ┌────────────────────────┐  ┌────────────────────────────────────────┐  │
│  │  轻量状态               │  │  TCP Fast Open                        │  │
│  │  tcp_minisocks.c       │  │  tcp_fastopen.c                       │  │
│  │  TIME_WAIT/SYN_RECV    │  │                                       │  │
│  └────────────────────────┘  └────────────────────────────────────────┘  │
│  ┌────────────────────────┐  ┌────────────────────────────────────────┐  │
│  │  哈希表 & 查找          │  │  SYN Cookie 防护                       │  │
│  │  inet_hashtables.c     │  │  syncookies.c                         │  │
│  │  inet_connection_sock.c│  │                                       │  │
│  └────────────────────────┘  └────────────────────────────────────────┘  │
└─────────────────────────┬───────────────────────────────────────────────────┘
                          │
┌─────────────────────────▼───────────────────────────────────────────────────┐
│                       IP 层 (net/ipv4/ip_output.c 等)                       │
│               ip_queue_xmit (发送) / ip_local_deliver (接收)                │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 Socket 结构体继承层次

```
┌─────────────────────────────────────────────────────────────┐
│  struct socket (BSD层, include/linux/net.h)                  │
│  ├─ state: socket_state (SS_UNCONNECTED/CONNECTED...)       │
│  ├─ type: SOCK_STREAM                                       │
│  ├─ file: 关联的文件描述符                                    │
│  ├─ ops: &inet_stream_ops (proto_ops)                       │
│  └─ sk ──────────────┐  (指针指向传输层控制块)                │
└──────────────────────┼──────────────────────────────────────┘
                       ▼
┌──────────────────────────────────────────────────────────────┐
│  struct sock (传输层基座, include/net/sock.h)                 │
│  ├─ __sk_common: sock_common                                │
│  │   ├─ skc_daddr/skc_rcv_saddr: 目标/本地IP                │
│  │   ├─ skc_dport/skc_num: 目标/本地端口                     │
│  │   ├─ skc_state: TCP状态 (TCP_ESTABLISHED等)              │
│  │   └─ skc_prot: &tcp_prot                                 │
│  ├─ sk_receive_queue: 接收队列                               │
│  ├─ sk_write_queue: 发送队列                                 │
│  ├─ tcp_rtx_queue: 重传队列                                  │
│  ├─ sk_sndbuf/sk_rcvbuf: 缓冲区大小                          │
│  ├─ sk_data_ready(): 数据就绪回调                             │
│  └─ sk_socket: 回指BSD socket                                │
├──────────────────────────────────────────────────────────────┤
│  struct inet_sock (INET层, include/net/inet_sock.h)          │
│  ├─ [包含 struct sock sk]                                    │
│  ├─ inet_saddr/inet_sport: 源IP/端口                         │
│  ├─ inet_opt: IP选项                                         │
│  ├─ tos: 服务类型                                            │
│  └─ cork: 累积发送控制                                        │
├──────────────────────────────────────────────────────────────┤
│  struct inet_connection_sock (连接层, inet_connection_sock.h)│
│  ├─ [包含 struct inet_sock icsk_inet]                        │
│  ├─ icsk_accept_queue: accept队列(全连接)                    │
│  ├─ icsk_retransmit_timer: 重传定时器                        │
│  ├─ icsk_delack_timer: 延迟ACK定时器                         │
│  ├─ icsk_rto: 当前RTO值                                     │
│  ├─ icsk_ca_ops: 拥塞控制算法指针                             │
│  ├─ icsk_ca_state: 拥塞状态(Open/Disorder/CWR/Recovery/Loss)│
│  └─ icsk_ca_priv[]: 拥塞算法私有数据                          │
├──────────────────────────────────────────────────────────────┤
│  struct tcp_sock (TCP层, include/linux/tcp.h)                │
│  ├─ [包含 struct inet_connection_sock inet_conn]             │
│  ├─ 序列号: rcv_nxt, snd_nxt, snd_una, write_seq            │
│  ├─ 窗口: snd_wnd, rcv_wnd, window_clamp, mss_cache         │
│  ├─ RTT: srtt_us, mdev_us, rttvar_us                        │
│  ├─ 拥塞: snd_cwnd, snd_ssthresh, packets_out               │
│  ├─ 丢失: lost_out, sacked_out, retrans_out                 │
│  ├─ 速率: delivered, rate_delivered, rate_interval_us        │
│  ├─ 乱序: out_of_order_queue (红黑树)                        │
│  ├─ SACK: selective_acks[], rack                             │
│  ├─ 定时器: pacing_timer, compressed_ack_timer               │
│  └─ 快路径: pred_flags                                       │
└──────────────────────────────────────────────────────────────┘
```

---

## 3. 源码文件组织

### 3.1 核心实现文件

| 文件 | 行数(约) | 职责 |
|------|----------|------|
| `net/ipv4/tcp.c` | 4882 | TCP 通用逻辑：`tcp_sendmsg`/`tcp_recvmsg`/`tcp_close`、内存管理、poll |
| `net/ipv4/tcp_input.c` | 8528 | **最大文件**：接收路径、ACK 处理、拥塞状态机、快速重传、SACK |
| `net/ipv4/tcp_output.c` | 5185 | 发送引擎：`tcp_write_xmit`/`tcp_transmit_skb`、TSO/GSO、Nagle |
| `net/ipv4/tcp_ipv4.c` | 4094 | IPv4 专用：`tcp_v4_connect`/`tcp_v4_rcv`、`tcp_prot` 定义 |
| `net/ipv4/tcp_timer.c` | 803 | 定时器：RTO 重传、延迟 ACK、keepalive、零窗口探测 |
| `net/ipv4/tcp_minisocks.c` | 968 | 轻量状态：TIME_WAIT minisocket、`tcp_check_req` |

### 3.2 子系统文件

| 文件 | 行数(约) | 职责 |
|------|----------|------|
| `net/ipv4/tcp_cong.c` | 479 | 可插拔拥塞控制框架 + Reno 兜底 |
| `net/ipv4/tcp_recovery.c` | 243 | RACK 丢包检测 |
| `net/ipv4/tcp_rate.c` | 214 | 投递速率估计（BBR 等使用） |
| `net/ipv4/tcp_fastopen.c` | 599 | TCP Fast Open |
| `net/ipv4/tcp_metrics.c` | 1038 | TCP 度量缓存 |
| `net/ipv4/tcp_offload.c` | 696 | GRO/GSO 卸载路径 |
| `net/ipv4/tcp_ulp.c` | 162 | 上层协议框架（TLS 等） |
| `net/ipv4/tcp_diag.c` | 239 | 诊断接口（ss 命令等） |

### 3.3 拥塞控制算法

| 文件 | 算法 | 特点 |
|------|------|------|
| `tcp_cubic.c` | **CUBIC** | Linux 默认，三次函数窗口增长 |
| `tcp_bbr.c` | **BBR** | 基于带宽模型，非丢包驱动 |
| `tcp_dctcp.c` | DCTCP | 数据中心专用，ECN 驱动 |
| `tcp_westwood.c` | Westwood+ | 带宽估计，无线友好 |
| `tcp_vegas.c` | Vegas | 基于延迟 |
| `tcp_htcp.c` | H-TCP | 高速长距网络 |
| `tcp_hybla.c` | Hybla | 卫星链路 |
| `tcp_illinois.c` | Illinois | 延迟+丢包混合 |
| `tcp_nv.c` | NV (New Vegas) | 低延迟数据中心 |
| `tcp_cdg.c` | CDG | CAIA Delay-Gradient |
| `tcp_bic.c` | BIC | CUBIC 前身 |
| `tcp_veno.c` | Veno | Vegas + Reno 混合 |
| `tcp_yeah.c` | YeAH | Yet Another Highspeed |
| `tcp_scalable.c` | Scalable | 简单高速 |
| `tcp_highspeed.c` | HighSpeed | Sally Floyd 提出 |
| `tcp_lp.c` | LP | 低优先级 |
| `bpf_tcp_ca.c` | BPF | eBPF 自定义拥塞控制 |

### 3.4 头文件三层结构

```
include/uapi/linux/tcp.h    ← 用户空间 ABI：tcphdr、socket 选项常量
        ▲
include/linux/tcp.h         ← 内核数据结构：tcp_sock、tcp_options_received
        ▲
include/net/tcp.h           ← TCP 子系统完整视图：函数声明、tcp_congestion_ops、常量
```

### 3.5 INET 基础设施文件

| 文件 | 职责 |
|------|------|
| `net/ipv4/af_inet.c` | PF_INET 协议族注册、`inet_create`、`inet_stream_ops` |
| `net/ipv4/inet_connection_sock.c` | 面向连接 socket 通用逻辑、accept 队列、端口绑定 |
| `net/ipv4/inet_hashtables.c` | 已连接/监听 socket 哈希查找 |
| `net/ipv4/inet_timewait_sock.c` | TIME_WAIT socket 管理 |
| `net/ipv4/syncookies.c` | SYN Cookie 抗洪泛攻击 |

---

## 4. 核心数据结构

### 4.1 `struct tcp_congestion_ops`（拥塞控制回调表）

```c
struct tcp_congestion_ops {
    /* === 快路径回调（热缓存行） === */
    u32 (*ssthresh)(struct sock *sk);                          /* 必须：慢启动阈值 */
    void (*cong_avoid)(struct sock *sk, u32 ack, u32 acked);   /* 窗口增长（与cong_control二选一） */
    void (*set_state)(struct sock *sk, u8 new_state);          /* 拥塞状态变化通知 */
    void (*cwnd_event)(struct sock *sk, enum tcp_ca_event ev); /* 窗口事件通知 */
    void (*in_ack_event)(struct sock *sk, u32 flags);          /* ACK 事件通知 */
    void (*pkts_acked)(struct sock *sk, const struct ack_sample *sample); /* RTT 采样 */
    u32 (*min_tso_segs)(struct sock *sk);                      /* TSO 最小分段 */
    void (*cong_control)(struct sock *sk, const struct rate_sample *rs); /* 新式全权控制 */

    /* === 慢路径回调 === */
    u32  (*undo_cwnd)(struct sock *sk);                        /* 必须：撤销窗口减小 */
    u32 (*sndbuf_expand)(struct sock *sk);                     /* 发送缓冲扩展因子 */
    size_t (*get_info)(...);                                   /* 诊断信息导出 */

    /* === 元数据 === */
    char name[TCP_CA_NAME_MAX];
    struct module *owner;
    struct list_head list;
    u32 key, flags;

    /* === 生命周期 === */
    void (*init)(struct sock *sk);                             /* 初始化私有数据 */
    void (*release)(struct sock *sk);                          /* 清理私有数据 */
};
```

**两种窗口控制模式**：
- **传统模式**（`cong_avoid`）：CUBIC、Reno 等，内核用 PRR 管理恢复阶段窗口
- **新式模式**（`cong_control`）：BBR 等，算法全权控制 cwnd 和 pacing rate

### 4.2 `struct proto tcp_prot`（协议操作表）

定义在 `net/ipv4/tcp_ipv4.c`，将 TCP 操作挂接到 socket 层：

```c
struct proto tcp_prot = {
    .name       = "TCP",
    .close      = tcp_close,
    .connect    = tcp_v4_connect,
    .accept     = inet_csk_accept,
    .sendmsg    = tcp_sendmsg,
    .recvmsg    = tcp_recvmsg,
    .backlog_rcv = tcp_v4_do_rcv,    /* backlog 处理 */
    .init       = tcp_v4_init_sock,
    .destroy    = tcp_v4_destroy_sock,
    .setsockopt = tcp_setsockopt,
    .getsockopt = tcp_getsockopt,
    /* ... */
};
```

---

## 5. 连接生命周期管理

> 三次握手/四次挥手的详细源码分析参见 [TCP 三次握手源码分析](TCP三次握手源码分析.md)、[TCP-listen 操作](TCP-listen操作.md)、[TCP 四次挥手与 close](TCP四次挥手与close.md)。

### 5.1 三次握手（客户端主动连接）

```
用户调用 connect()
     │
     ▼
net/socket.c: __sys_connect()
     │
     ▼
net/ipv4/af_inet.c: inet_stream_connect()
     │ sock->ops->connect → sk->sk_prot->connect
     ▼
net/ipv4/tcp_ipv4.c: tcp_v4_connect()
     │ 1. 路由查找
     │ 2. 选择源端口 (inet_hash_connect)
     │ 3. TCP_CLOSE → TCP_SYN_SENT
     ▼
net/ipv4/tcp_output.c: tcp_connect()
     │ 1. tcp_connect_init()：初始化窗口/MSS
     │ 2. tcp_init_nondata_skb(skb, TCPHDR_SYN)
     │ 3. tcp_transmit_skb()：构造并发送 SYN
     │ 4. inet_csk_reset_xmit_timer()：启动重传定时器
     ▼
[等待 SYN-ACK]
     │
     ▼ 收到 SYN-ACK
net/ipv4/tcp_input.c: tcp_rcv_synsent_state_process()
     │ tcp_finish_connect()
     │ TCP_SYN_SENT → TCP_ESTABLISHED
     ▼
connect() 返回成功
```

### 5.2 三次握手（服务端被动监听）

```
[IP 层收包]
     │
     ▼
net/ipv4/tcp_ipv4.c: tcp_v4_rcv()
     │ __inet_lookup_skb() 查找监听 socket
     ▼
net/ipv4/tcp_ipv4.c: tcp_v4_do_rcv() [TCP_LISTEN]
     │
     ▼
net/ipv4/tcp_input.c: tcp_rcv_state_process() [TCP_LISTEN]
     │ icsk->icsk_af_ops->conn_request
     ▼
net/ipv4/tcp_ipv4.c: tcp_v4_conn_request()
     │
     ▼
net/ipv4/tcp_input.c: tcp_conn_request()
     │ 1. 创建 request_sock（半连接）
     │ 2. af_ops->send_synack() → 发送 SYN-ACK
     │ 3. 半连接以 TCP_NEW_SYN_RECV 加入 ehash
     ▼
[等待第三次 ACK]
     │
     ▼ 收到 ACK
net/ipv4/tcp_ipv4.c: tcp_v4_rcv() [TCP_NEW_SYN_RECV]
     │
     ▼
net/ipv4/tcp_minisocks.c: tcp_check_req()
     │ 1. 校验 ACK 合法性
     │ 2. syn_recv_sock() → tcp_v4_syn_recv_sock()：创建子 socket
     │ 3. inet_csk_complete_hashdance()：加入全连接队列
     ▼
net/ipv4/tcp_minisocks.c: tcp_child_process()
     │ tcp_rcv_state_process() → TCP_SYN_RECV → TCP_ESTABLISHED
     ▼
accept() 返回子 socket
```

### 5.3 连接关闭与 TIME_WAIT

```
                主动关闭方                              被动关闭方
                ─────────                              ─────────
用户调用 close()
     │
af_inet.c: inet_release()
     │ sk->sk_prot->close
     ▼
tcp.c: tcp_close() → __tcp_close()
     │ tcp_close_state():
     │ ESTABLISHED → FIN_WAIT1
     ▼
tcp_output.c: tcp_send_fin()                    收到 FIN
     │ 构造 FIN 包并发送                              │
     │                                          tcp_input.c: tcp_fin()
     │                                              │ ESTABLISHED → CLOSE_WAIT
     │                                              │ 发送 ACK
     ▼                                              ▼
收到 ACK                                      用户调用 close()
     │ FIN_WAIT1 → FIN_WAIT2                        │ CLOSE_WAIT → LAST_ACK
     │                                              │ 发送 FIN
     ▼                                              ▼
收到 FIN                                      收到 ACK
     │ tcp_fin():                                   │ LAST_ACK → CLOSE
     │ FIN_WAIT2 → TIME_WAIT                        │ tcp_done()
     │ 发送 ACK                                      ▼
     ▼                                          socket 释放
tcp_minisocks.c: tcp_time_wait()
     │ 1. 分配 inet_timewait_sock（轻量级）
     │ 2. 设置 TCP_TIMEWAIT_LEN (60s) 超时
     │ 3. inet_twsk_hashdance()：替换原 sock
     │ 4. tcp_done(sk)：释放原 sock
     ▼
[等待 2MSL]
     │
     ▼
超时后释放 timewait sock
```

### 5.4 TCP 状态机全景

```
                              ┌────────────┐
                    ┌────────>│   CLOSED    │<────────┐
                    │         └─────┬──────┘         │
                    │               │                │
              tcp_done()     socket()/bind()     超时/RST
                    │               │                │
                    │         ┌─────▼──────┐         │
                    │    ┌───>│   LISTEN    │         │
                    │    │    └─────┬──────┘         │
                    │    │    收到SYN│                │
                    │    │         │                 │
          ┌────────┴────┴──┐  ┌───▼──────────┐     │
    ┌────>│   LAST_ACK     │  │ SYN_RECEIVED │     │
    │     └────────────────┘  └───┬──────────┘     │
    │          ▲                  │收到ACK         │
    │     发FIN│            ┌─────▼──────┐         │
    │          │    ┌──────>│ESTABLISHED │<────┐    │
    │     ┌────┴────┴──┐    └─────┬──────┘     │    │
    │     │CLOSE_WAIT  │<────────┘│            │    │
    │     └────────────┘  收到FIN  │发FIN       │    │
    │                             │            │    │
    │                       ┌─────▼──────┐     │    │
    │              connect()│ SYN_SENT   │─────┘    │
    │                       └─────┬──────┘ 收到     │
    │                             │      SYN+ACK    │
    │                       ┌─────▼──────┐          │
    │                       │ FIN_WAIT1  │          │
    │                       └─────┬──────┘          │
    │                        收到ACK│                │
    │                       ┌─────▼──────┐          │
    │                       │ FIN_WAIT2  │          │
    │                       └─────┬──────┘          │
    │                        收到FIN│                │
    │                       ┌─────▼──────┐          │
    └───────────────────────│ TIME_WAIT  │──────────┘
                            └────────────┘
                              等待 2MSL
```

---

## 6. 数据发送路径

> 发送路径的完整源码分析（18 章）参见 [TCP 发送全路径](TCP发送全路径.md)。

### 6.1 发送路径调用链

```
tcp_sendmsg()                          ← 系统调用入口
  └─ tcp_sendmsg_locked()
       │
       ├─ [循环] while (msg_data_left(msg))
       │    ├─ sk_stream_alloc_skb()    ← 分配 skb
       │    ├─ tcp_skb_entail()         ← 挂到发送队列
       │    ├─ skb_add_data_nocache()   ← 线性区拷贝
       │    │  或 skb_copy_to_page_nocache() + skb_fill_page_desc()  ← 分页拷贝
       │    └─ [条件] forced_push → __tcp_push_pending_frames()
       │
       └─ tcp_push()                   ← 触发实际发送
            │
            ├─ [检查] tcp_should_autocork()  ← 自动 cork 优化
            └─ __tcp_push_pending_frames()
                 │
                 └─ tcp_write_xmit()         ← 核心发送循环
                      │
                      ├─ [循环] while (skb = tcp_send_head(sk))
                      │    ├─ tcp_pacing_check()     ← pacing 限速
                      │    ├─ tcp_cwnd_test()         ← 拥塞窗口检查
                      │    ├─ tcp_snd_wnd_test()      ← 发送窗口检查
                      │    ├─ tcp_nagle_test()         ← Nagle 算法
                      │    ├─ tcp_tso_should_defer()   ← TSO 延迟
                      │    ├─ tso_fragment()            ← TSO 分段
                      │    ├─ tcp_small_queue_check()   ← 小队列限制
                      │    └─ tcp_transmit_skb()        ← 构造并发送
                      │
                      └─ tcp_transmit_skb()
                           │
                           └─ __tcp_transmit_skb()
                                ├─ 构造 TCP 头 (tcphdr)
                                ├─ 添加 TCP 选项 (timestamp/SACK/etc)
                                ├─ 计算校验和
                                └─ icsk_af_ops->queue_xmit()
                                     │
                                     └─ ip_queue_xmit()  ← 进入 IP 层
```

### 6.2 发送路径关键决策

```
                    ┌──────────────┐
                    │ tcp_push()   │
                    └──────┬───────┘
                           │
                    ┌──────▼───────┐    是
                    │ autocork?    ├──────→ 延迟发送（等更多数据）
                    └──────┬───────┘
                           │ 否
                    ┌──────▼───────────────┐
                    │ tcp_write_xmit 循环   │
                    └──────┬───────────────┘
                           │
              ┌────────────▼────────────┐
              │ 1. pacing 检查           │── 超速 → break (hrtimer 回调后重试)
              │ 2. cwnd 检查             │── 窗口满 → break
              │ 3. rwnd 检查             │── 对端窗口满 → break
              │ 4. Nagle 检查            │── 数据不够 → break
              │ 5. TSO defer 检查        │── 值得等待 → break
              │ 6. small queue 检查      │── 队列过长 → break
              └────────────┬────────────┘
                           │ 全部通过
              ┌────────────▼────────────┐
              │  tcp_transmit_skb()     │
              │  构造 TCP 头 + 发送      │
              └─────────────────────────┘
```

---

## 7. 数据接收路径

> 接收路径的完整源码分析参见 [TCP 接收路径深度分析](TCP接收路径深度分析.md)。

### 7.1 接收路径调用链

```
[网卡中断 → NAPI → GRO]
     │
     ▼
ip_local_deliver()
     │
     ▼
tcp_v4_rcv()                               ← IP 层分发入口
     │
     ├─ __inet_lookup_skb()                ← 四元组哈希查找 socket
     │
     ├─ [TCP_TIME_WAIT] → tcp_timewait_state_process()
     ├─ [TCP_NEW_SYN_RECV] → tcp_check_req()
     │
     └─ [普通 socket]
          ├─ bh_lock_sock()
          ├─ [socket 未被用户锁定]
          │    └─ tcp_v4_do_rcv()
          └─ [socket 被用户锁定]
               └─ tcp_add_backlog()         ← release_sock 时再处理
```

### 7.2 ESTABLISHED 状态收包

```
tcp_v4_do_rcv()
     │ sk_state == TCP_ESTABLISHED
     ▼
tcp_rcv_established()                      ← 核心收包处理
     │
     ├─ [快路径条件满足] pred_flags 匹配 + 按序到达
     │    │
     │    ├─ [纯 ACK (len == header_len)]
     │    │    ├─ tcp_ack()                ← ACK 处理
     │    │    └─ tcp_data_snd_check()     ← 可能触发新发送
     │    │
     │    └─ [带数据]
     │         ├─ 校验和验证
     │         ├─ tcp_queue_rcv()          ← 按序入队
     │         ├─ tcp_ack()
     │         ├─ __tcp_ack_snd_check()   ← 决定是否发 ACK
     │         └─ tcp_data_ready()         ← 唤醒用户进程
     │
     └─ [慢路径]
          ├─ tcp_validate_incoming()       ← 合法性检查
          ├─ tcp_ack()                     ← ACK 处理（含拥塞控制）
          ├─ tcp_urg()                     ← 紧急数据
          ├─ tcp_data_queue()              ← 数据排队
          │    ├─ [按序] tcp_queue_rcv() → tcp_ofo_queue() → tcp_data_ready()
          │    ├─ [重复] D-SACK → drop
          │    ├─ [部分重叠] 裁剪 → 入队
          │    └─ [乱序] tcp_data_queue_ofo()  ← 红黑树管理
          ├─ tcp_data_snd_check()
          └─ tcp_ack_snd_check()
```

### 7.3 ACK 处理与拥塞控制调用

```
tcp_ack()                                  ← ACK 核心处理
     │
     ├─ 更新 snd_una（确认序列号推进）
     ├─ tcp_clean_rtx_queue()              ← 清理重传队列中已确认的包
     │    ├─ tcp_rate_skb_delivered()      ← 速率采样
     │    └─ tcp_ack_update_rtt()          ← RTT 更新
     │
     ├─ tcp_fastretrans_alert()            ← 拥塞状态机
     │    ├─ [TCP_CA_Recovery] → 恢复中处理
     │    ├─ [TCP_CA_Loss] → tcp_process_loss()
     │    ├─ RACK 丢包检测 / Reno 丢包检测
     │    └─ tcp_enter_recovery() / tcp_enter_loss()
     │         └─ tcp_init_cwnd_reduction()
     │              └─ icsk_ca_ops->ssthresh()
     │
     ├─ tcp_rate_gen()                     ← 生成速率样本
     │
     ├─ tcp_cong_control()                 ← 拥塞控制决策
     │    ├─ [有 cong_control] → icsk_ca_ops->cong_control(sk, rs)  ← BBR
     │    ├─ [cwnd reduction 中] → tcp_cwnd_reduction() (PRR)
     │    └─ [正常增长] → tcp_cong_avoid()
     │         └─ icsk_ca_ops->cong_avoid()                        ← CUBIC
     │
     └─ tcp_xmit_recovery()               ← 触发重传
```

### 7.4 用户空间读取

```
tcp_recvmsg()
  └─ tcp_recvmsg_locked()
       │
       ├─ [循环] 按 copied_seq 在 sk_receive_queue 中查找
       │    ├─ skb_copy_datagram_msg()    ← 拷贝到用户缓冲区
       │    ├─ 推进 copied_seq
       │    └─ sk_eat_skb()               ← 释放已读 skb
       │
       ├─ [无数据] sk_wait_data()         ← 阻塞等待
       │
       └─ tcp_cleanup_rbuf()              ← 窗口更新、可能发 ACK
```

---

## 8. 拥塞控制框架

### 8.1 框架架构

```
┌─────────────────────────────────────────────────────────────────┐
│                   拥塞控制框架 (tcp_cong.c)                      │
│                                                                 │
│  tcp_cong_list ──→ [cubic] ──→ [bbr] ──→ [reno] ──→ ...       │
│                                                                 │
│  tcp_register_congestion_control()   注册算法                    │
│  tcp_set_congestion_control()        运行时切换                  │
│  tcp_assign_congestion_control()     新连接分配默认算法           │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │            net->ipv4.tcp_congestion_control             │   │
│  │              （每个网络命名空间的默认算法指针）              │   │
│  │              若模块加载失败 → 回退到 tcp_reno             │   │
│  └─────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│                    回调触发时机                                    │
│                                                                  │
│  ┌────────────┐                                                  │
│  │  tcp_ack() │──→ in_ack_event() ──→ pkts_acked()              │
│  │  (收到ACK) │──→ tcp_fastretrans_alert()                       │
│  │            │     ├─ tcp_enter_recovery() → ssthresh()         │
│  │            │     └─ tcp_enter_loss() → ssthresh() + set_state()│
│  │            │──→ tcp_rate_gen()                                 │
│  │            │──→ tcp_cong_control()                             │
│  │            │     ├─ cong_control(rs)   [BBR 路径]              │
│  │            │     ├─ tcp_cwnd_reduction() [PRR, Recovery 中]    │
│  │            │     └─ cong_avoid()       [CUBIC 路径]            │
│  └────────────┘                                                  │
│                                                                  │
│  ┌──────────────────┐                                            │
│  │ 状态变化          │──→ set_state()                             │
│  │ 窗口事件          │──→ cwnd_event() (CA_EVENT_*)              │
│  │ 连接建立/初始化    │──→ init()                                 │
│  │ 连接关闭          │──→ release()                               │
│  │ 发送恢复后        │──→ cwnd_event(CA_EVENT_TX_START)           │
│  └──────────────────┘                                            │
└──────────────────────────────────────────────────────────────────┘
```

### 8.2 拥塞状态机

```
                   ┌──────────────────┐
                   │  TCP_CA_Open      │ ← 正常传输
                   │  正常增长 cwnd    │
                   └────────┬─────────┘
                            │ DupACK / SACK / ECE
                   ┌────────▼─────────┐
                   │ TCP_CA_Disorder   │ ← 检测到异常
                   │ 可能乱序或丢包    │
                   └────────┬─────────┘
                       ┌────┴────┐
                  ECE确认│        │ 确认丢包
                   ┌────▼────┐  ┌▼───────────┐
                   │TCP_CA_CWR│  │TCP_CA_Recovery│ ← 快速恢复
                   │ECN 窗口  │  │PRR 恢复中      │
                   │减小      │  │重传丢失包       │
                   └────┬────┘  └──┬──────────┘
                        │         │
                        │    RTO超时│
                        │    ┌────▼─────────┐
                        │    │ TCP_CA_Loss    │ ← RTO 超时
                        │    │ cwnd=1, 慢启动 │
                        │    └────┬──────────┘
                        │         │
                   ┌────▼─────────▼──┐
                   │  恢复完成         │
                   │  CA_EVENT_COMPLETE│
                   │  _CWR            │
                   └────────┬─────────┘
                            │
                   ┌────────▼─────────┐
                   │   TCP_CA_Open     │
                   └──────────────────┘
```

### 8.3 CUBIC vs BBR 对比

```
┌──────────────────────────────────────────────────────────────────┐
│                    CUBIC (传统模式)                                │
│                                                                  │
│  注册方式: tcp_register_congestion_control(&cubictcp)             │
│  控制路径: cong_avoid → cubictcp_cong_avoid()                    │
│  窗口增长: W(t) = C*(t-K)³ + W_max  (三次函数)                   │
│  丢包响应: ssthresh → cubictcp_recalc_ssthresh() → β*cwnd       │
│  恢复方式: 由内核 PRR (tcp_cwnd_reduction) 管理                   │
│                                                                  │
│  调用链: tcp_cong_control()                                      │
│          └─ tcp_cong_avoid()                                     │
│               └─ cubictcp_cong_avoid()                           │
│                    ├─ tcp_slow_start()     (cwnd < ssthresh)     │
│                    └─ bictcp_update()      (拥塞避免)             │
│                         └─ tcp_cong_avoid_ai()                   │
└──────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│                    BBR (新式模式)                                  │
│                                                                  │
│  注册方式: tcp_register_congestion_control(&tcp_bbr_cong_ops)     │
│  控制路径: cong_control → bbr_main()                             │
│  带宽模型: BtlBw = max(delivery_rate) over sliding window       │
│  延迟模型: RTprop = min(RTT) over sliding window                 │
│  pacing:  pacing_rate = pacing_gain * BtlBw                     │
│  cwnd:    cwnd = cwnd_gain * BDP                                 │
│  恢复方式: BBR 自主管理，不依赖内核 PRR                            │
│                                                                  │
│  调用链: tcp_cong_control()                                      │
│          └─ bbr_main(sk, rs)                                     │
│               ├─ bbr_update_model()                              │
│               │    ├─ bbr_update_bw()                            │
│               │    ├─ bbr_update_cycle_phase()                   │
│               │    ├─ bbr_check_full_bw_reached()                │
│               │    ├─ bbr_check_drain()                          │
│               │    └─ bbr_update_min_rtt()                       │
│               └─ bbr_update_control_parameters()                 │
│                    ├─ bbr_set_pacing_rate()                       │
│                    ├─ bbr_set_tso_segs_goal()                    │
│                    └─ bbr_set_cwnd()                              │
└──────────────────────────────────────────────────────────────────┘
```

### 8.4 速率估计 (`tcp_rate.c`)

```
┌─────────────────────────────────────────────────────────────────┐
│                   速率估计流程                                    │
│                                                                 │
│  发送时:                                                         │
│  tcp_transmit_skb() → tcp_rate_skb_sent()                       │
│     └─ 记录到 skb: first_tx_mstamp, delivered_mstamp,           │
│                     delivered, app_limited                       │
│                                                                 │
│  确认时:                                                         │
│  tcp_clean_rtx_queue() → tcp_rate_skb_delivered()               │
│     └─ 从 skb 恢复发送时快照，合并到 rate_sample                  │
│                                                                 │
│  ACK 处理末尾:                                                   │
│  tcp_rate_gen()                                                  │
│     ├─ delivered = tp->delivered - rs->prior_delivered           │
│     ├─ interval = max(snd_interval, ack_interval)               │
│     │            ← 缓解 ACK 压缩                                │
│     ├─ bw ≈ delivered / interval                                │
│     └─ 标记 app_limited 样本                                    │
│                                                                 │
│  拥塞控制使用:                                                    │
│  tcp_cong_control(sk, rs) → bbr_main(sk, rs)                   │
│     └─ rs->delivered, rs->interval_us, rs->rtt_us 等            │
└─────────────────────────────────────────────────────────────────┘
```

### 8.5 RACK 丢包检测 (`tcp_recovery.c`)

```
传统方式 (DupACK 计数):
  3 个重复 ACK → 判定丢包 → 快速重传

RACK (Recent ACKnowledgment):
  ┌─────────────────────────────────────────────────────────────┐
  │  核心思想: 基于时间而非计数                                    │
  │                                                             │
  │  如果一个更晚发送的包被确认了，                                │
  │  那么更早发送但仍未确认的包可能已丢失                          │
  │                                                             │
  │  判定条件:                                                    │
  │  当前时间 - skb->发送时间 > rack.rtt_us + 重排序窗口          │
  │                                                             │
  │  优点: 对尾丢失、重传丢失更敏感                               │
  └─────────────────────────────────────────────────────────────┘

  调用路径:
  tcp_ack() → tcp_fastretrans_alert()
       └─ tcp_identify_packet_loss()
            ├─ [RACK] tcp_rack_mark_lost()
            │    └─ tcp_rack_detect_loss()
            │         └─ 扫描 tsorted_sent_queue，按时间标记丢失
            └─ [Reno] tcp_newreno_mark_lost()
```

---

## 9. 定时器体系

### 9.1 TCP 定时器全景

```
┌─────────────────────────────────────────────────────────────────┐
│                   TCP 定时器体系 (tcp_timer.c)                    │
│                                                                 │
│  ┌─────────────────────────┐  ┌────────────────────────────┐   │
│  │  重传定时器 (RTO)         │  │  延迟 ACK 定时器            │   │
│  │  icsk_retransmit_timer  │  │  icsk_delack_timer         │   │
│  │                         │  │                            │   │
│  │  超时后:                 │  │  最多延迟 200ms 后发送 ACK  │   │
│  │  tcp_retransmit_timer() │  │  tcp_delack_timer()        │   │
│  │  → tcp_enter_loss()     │  │  → tcp_send_ack()          │   │
│  │  → tcp_retransmit_skb() │  │                            │   │
│  │  → 指数退避 RTO         │  │                            │   │
│  └─────────────────────────┘  └────────────────────────────┘   │
│                                                                 │
│  ┌─────────────────────────┐  ┌────────────────────────────┐   │
│  │  Keepalive 定时器        │  │  零窗口探测定时器            │   │
│  │  tcp_keepalive_timer()  │  │  tcp_probe_timer()         │   │
│  │                         │  │                            │   │
│  │  空闲连接探活            │  │  对端窗口为 0 时周期性探测   │   │
│  │  默认 2 小时             │  │  发送 1 字节数据            │   │
│  └─────────────────────────┘  └────────────────────────────┘   │
│                                                                 │
│  ┌─────────────────────────┐  ┌────────────────────────────┐   │
│  │  Pacing 定时器 (hrtimer) │  │  SYNACK 重传定时器          │   │
│  │  tcp_pace_kick()        │  │  tcp_fastopen_synack_timer()│   │
│  │                         │  │                            │   │
│  │  控制发包速率(BBR等)     │  │  SYN-ACK 超时重传           │   │
│  │  高精度定时器            │  │                            │   │
│  └─────────────────────────┘  └────────────────────────────┘   │
│                                                                 │
│  ┌─────────────────────────┐  ┌────────────────────────────┐   │
│  │  TLP 定时器              │  │  TIME_WAIT 定时器           │   │
│  │  (Tail Loss Probe)      │  │  inet_twsk_schedule()      │   │
│  │  tcp_send_loss_probe()  │  │                            │   │
│  │                         │  │  60 秒后释放 tw_sock        │   │
│  │  尾部丢包探测            │  │  (TCP_TIMEWAIT_LEN)        │   │
│  └─────────────────────────┘  └────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

---

## 10. 关键设计模式

### 10.1 协议注册机制

```
inet_init() (af_inet.c)
     │
     ├─ proto_register(&tcp_prot)          ← 注册 struct proto（传输层 slab）
     ├─ sock_register(&inet_family_ops)    ← 注册 PF_INET 族（create = inet_create）
     ├─ inet_add_protocol(&tcp_protocol)   ← IP 层 protocol=6 → tcp_v4_rcv
     └─ inet_register_protosw()            ← SOCK_STREAM → tcp_prot + inet_stream_ops
```

### 10.2 Backlog 机制

```
                ┌───────────────────────────────────────────┐
                │        socket 被用户进程持锁时              │
                │                                           │
  tcp_v4_rcv()  │   bh_lock_sock()                          │
       │        │        │                                  │
       ▼        │        ▼                                  │
  sock_owned    │   sock_owned_by_user(sk) == true          │
  _by_user?─────│──→ tcp_add_backlog(sk, skb)               │
       │        │        │ skb 进入 sk_backlog 队列          │
       │ false  │        └─────────────────┐                │
       ▼        │                          │                │
  tcp_v4_do_rcv │                          ▼                │
  (直接处理)     │                    release_sock()         │
                │                    __release_sock()       │
                │                    └─ tcp_v4_do_rcv()     │
                │                       (延迟处理 backlog)   │
                └───────────────────────────────────────────┘
```

### 10.3 内存管理

```
TCP 内存管理三级架构:

1. 全局级: sysctl_tcp_mem[3] (low/pressure/high)
   └─ tcp_memory_pressure 标志控制全局限流

2. Socket 级: sk_sndbuf / sk_rcvbuf
   └─ sk_wmem_queued / sk_rmem_alloc 追踪实际使用量

3. 应用级: SO_SNDBUF / SO_RCVBUF 用户可调
   └─ tcp_sndbuf_expand() / tcp_grow_window() 自动调整
```

### 10.4 锁机制

```
TCP socket 的两把锁:

1. sock_lock (sk_lock.slock)
   └─ 自旋锁，保护底半部（软中断）访问
   └─ bh_lock_sock() / bh_unlock_sock()

2. socket_lock (sk_lock.owned)
   └─ 睡眠锁，保护用户上下文访问
   └─ lock_sock() / release_sock()
   └─ owned=1 时底半部走 backlog

配合机制:
   用户持锁 → 底半部进 backlog → release_sock 时处理 backlog
   避免了用户上下文和软中断的竞争
```

---

## 附录：关键文件索引

| 文件路径 | 行数 | 核心函数 |
|----------|------|----------|
| `net/ipv4/tcp.c` | ~4882 | `tcp_sendmsg`, `tcp_recvmsg`, `tcp_close`, `tcp_init_sock` |
| `net/ipv4/tcp_input.c` | ~8528 | `tcp_rcv_established`, `tcp_rcv_state_process`, `tcp_ack`, `tcp_data_queue`, `tcp_fastretrans_alert` |
| `net/ipv4/tcp_output.c` | ~5185 | `tcp_write_xmit`, `tcp_transmit_skb`, `tcp_connect`, `tcp_send_fin`, `tcp_retransmit_skb` |
| `net/ipv4/tcp_ipv4.c` | ~4094 | `tcp_v4_rcv`, `tcp_v4_connect`, `tcp_v4_do_rcv`, `tcp_prot` |
| `net/ipv4/tcp_timer.c` | ~803 | `tcp_retransmit_timer`, `tcp_delack_timer`, `tcp_keepalive_timer` |
| `net/ipv4/tcp_minisocks.c` | ~968 | `tcp_time_wait`, `tcp_check_req`, `tcp_child_process` |
| `net/ipv4/tcp_cong.c` | ~479 | `tcp_register_congestion_control`, `tcp_set_congestion_control` |
| `net/ipv4/tcp_recovery.c` | ~243 | `tcp_rack_mark_lost`, `tcp_rack_detect_loss` |
| `net/ipv4/tcp_rate.c` | ~214 | `tcp_rate_skb_sent`, `tcp_rate_gen` |
| `net/ipv4/tcp_bbr.c` | ~2285 | `bbr_main`, `bbr_update_model`, `bbr_set_cwnd` |
| `net/ipv4/tcp_cubic.c` | ~533 | `cubictcp_cong_avoid`, `bictcp_update` |
| `net/ipv4/tcp_fastopen.c` | ~599 | `tcp_fastopen_create_child`, `tcp_fastopen_cookie_check` |
| `net/ipv4/af_inet.c` | ~2305 | `inet_create`, `inet_stream_connect`, `inet_init` |
| `net/ipv4/inet_connection_sock.c` | ~1411 | `inet_csk_accept`, `inet_csk_complete_hashdance` |
| `net/ipv4/inet_hashtables.c` | ~900 | `__inet_lookup_established`, `inet_hash_connect` |
| `include/net/tcp.h` | ~2492 | `tcp_congestion_ops`, 函数声明, 内联辅助 |
| `include/linux/tcp.h` | ~531 | `struct tcp_sock` |
| `include/uapi/linux/tcp.h` | ~407 | `struct tcphdr`, socket 选项常量 |

---

> **拥塞控制四大机制的完整源码分析**（慢启动、拥塞避免、丢包检测、PRR 快速恢复）已独立为 **[TCP 拥塞控制四大机制](TCP拥塞控制四大机制.md)**。
