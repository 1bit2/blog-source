+++
date = '2026-04-28'
title = 'TCP Listen 操作深度源码分析——backlog、半连接队列、全连接队列'
weight = 6
tags = [
    "TCP",
    "listen",
    "inet_listen",
    "backlog",
    "accept队列",
    "SYN队列",
    "半连接",
    "全连接",
    "somaxconn",
    "tcp_max_syn_backlog",
    "SYN Cookie",
    "tcp_abort_on_overflow",
    "ListenOverflows",
    "ListenDrops",
]
categories = [
    "网络",
]
+++
# TCP Listen 操作深度源码分析——backlog、半连接队列、全连接队列

> 基于 Linux 5.15.78，全面分析 `listen()` 系统调用、backlog 的真实含义、半连接队列（SYN 队列）和全连接队列（Accept 队列）的数据结构、参数限制、溢出处理、SYN Cookie 机制、监控方法。

---

## 目录

- [一、全景调用链](#一全景调用链)
- [二、`listen()` 做了什么](#二listen-做了什么)
- [三、准备知识：数据结构与 ehash 生命周期](#三准备知识数据结构与-ehash-生命周期)
- [四、两种工作模式（心智模型）](#四两种工作模式心智模型)
- [五、正常模式：SYN → req → ehash → SYN-ACK](#五正常模式syn--req--ehash--syn-ack)
- [六、Cookie 模式：状态编码进 ISN](#六cookie-模式状态编码进-isn)
- [七、ACK 处理与全连接队列](#七ack-处理与全连接队列)
- [八、溢出、排查与调优（一站式）](#八溢出排查与调优一站式)
- [九、历史演进](#九历史演进)


---

## 一、全景调用链

```
用户态: listen(sockfd, backlog)
    │
    ▼
SYSCALL_DEFINE2(listen)
    └── __sys_listen()                          [net/socket.c:1813]
        ├── somaxconn = sysctl_somaxconn        // 系统上限
        ├── backlog = min(backlog, somaxconn)    // 截断
        ├── security_socket_listen()            // LSM
        └── sock->ops->listen()                 // → inet_listen()
            │
            ▼
        inet_listen()                           [net/ipv4/af_inet.c:211]
        ├── sk_max_ack_backlog = backlog         // 设置队列上限
        ├── [首次] fastopen_queue_tune()         // TFO 队列
        └── [首次] inet_csk_listen_start()       [inet_connection_sock.c:1145]
            ├── reqsk_queue_alloc()              // 初始化 accept 队列
            ├── inet_sk_state_store(TCP_LISTEN)  // 状态转换
            ├── get_port()                       // 端口校验
            └── hash()                           // 加入 listening_hash
```

---

## 二、`listen()` 做了什么

> 本章一次讲完 `listen()` 系统调用的完整行为：入口、backlog 截断、状态切换、零内存分配、注册监听哈希表。读者读完本章应该能回答"listen 到底做了什么、不做什么"。


### 2.1 `__sys_listen()`：入口与 backlog 截断

```c
// net/socket.c:1813-1837
int __sys_listen(int fd, int backlog)
{
    struct socket *sock;
    int err, fput_needed;
    int somaxconn;

    sock = sockfd_lookup_light(fd, &err, &fput_needed);
    if (sock) {
        somaxconn = READ_ONCE(sock_net(sock->sk)->core.sysctl_somaxconn);
        if ((unsigned int)backlog > somaxconn)
            backlog = somaxconn;   // ★ backlog 被 somaxconn 截断

        err = security_socket_listen(sock, backlog);
        if (!err)
            err = sock->ops->listen(sock, backlog);
        fput_light(sock->file, fput_needed);
    }
    return err;
}
```

**关键点**：用户传入的 `backlog` 被 `(unsigned int)` 转换后与 `somaxconn` 比较。如果用户传入负数（如 -1），`(unsigned int)-1 = 4294967295`，会被截断为 `somaxconn`。

---

### 2.2 `inet_listen()`：状态切换与 backlog 落盘

#### `inet_listen()` 源码

```c
// net/ipv4/af_inet.c:211-258
int inet_listen(struct socket *sock, int backlog)
{
    struct sock *sk = sock->sk;
    unsigned char old_state;

    lock_sock(sk);

    // ① 前置检查
    if (sock->state != SS_UNCONNECTED || sock->type != SOCK_STREAM)
        goto out;  // -EINVAL
    if (!((1 << old_state) & (TCPF_CLOSE | TCPF_LISTEN)))
        goto out;  // 只有 CLOSE 或 LISTEN 可以调用 listen

    // ② 更新 backlog 上限（即使已在 LISTEN 也可动态调整）
    WRITE_ONCE(sk->sk_max_ack_backlog, backlog);

    // ③ 首次 listen（CLOSE → LISTEN）
    if (old_state != TCP_LISTEN) {
        // TFO 初始化
        tcp_fastopen = READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_fastopen);
        if ((tcp_fastopen & TFO_SERVER_WO_SOCKOPT1) &&
            (tcp_fastopen & TFO_SERVER_ENABLE) &&
            !inet_csk(sk)->icsk_accept_queue.fastopenq.max_qlen) {
            fastopen_queue_tune(sk, backlog);
            tcp_fastopen_init_key_once(sock_net(sk));
        }

        err = inet_csk_listen_start(sk, backlog);
    }

    release_sock(sk);
    return err;
}
```

#### 重复调用 listen() 的行为

`inet_listen()` 允许对**已处于 TCP_LISTEN 状态**的 socket 再次调用 `listen()`。此时：

- `WRITE_ONCE(sk->sk_max_ack_backlog, backlog)` **无条件执行**——动态调整队列上限
- `if (old_state != TCP_LISTEN)` 为 false——跳过 `inet_csk_listen_start()`

**重复 listen() 只更新 backlog 阈值**，已有的队列内容、连接状态、ehash 注册全部保持不变。

### 2.3 `inet_csk_listen_start()`：注册监听 + 零内存分配

```c
// net/ipv4/inet_connection_sock.c:1162-1198
int inet_csk_listen_start(struct sock *sk, int backlog)
{
    struct inet_connection_sock *icsk = inet_csk(sk);
    struct inet_sock *inet = inet_sk(sk);
    int err = -EADDRINUSE;

    // ① 初始化 accept 队列（注意：只是 init，不分配内存）
    reqsk_queue_alloc(&icsk->icsk_accept_queue);

    sk->sk_ack_backlog = 0;       // 当前全连接队列长度清零
    inet_csk_delack_init(sk);     // 初始化延迟 ACK

    // ② 状态转换
    inet_sk_state_store(sk, TCP_LISTEN);

    // ③ 端口校验 + 加入监听哈希表
    if (!sk->sk_prot->get_port(sk, inet->inet_num)) {
        inet->inet_sport = htons(inet->inet_num);
        sk_dst_reset(sk);
        err = sk->sk_prot->hash(sk);    // inet_hash() → listening_hash + lhash2
        if (likely(!err))
            return 0;
    }

    // 失败回滚
    inet_sk_set_state(sk, TCP_CLOSE);
    return err;
}
```

#### 关键认知：listen 是"声明式"操作，零内存分配

**listen 全过程不分配任何新的内存对象**。`icsk_accept_queue` 是 `inet_connection_sock` 的**内嵌结构体**（不是指针），随 `tcp_sock` 一起早已分配好。所有"新对象"都要等到握手阶段按需发生：

| 对象 | listen 时分配？ | 真正分配时机 | 分配函数 |
|------|----------------|--------------|----------|
| `inet_bind_bucket` | ✗ | bind() 时 | `inet_bind_bucket_create` (slab) |
| `request_sock`（半连接） | ✗ | 收到 SYN 时 | `inet_reqsk_alloc` (slab) |
| 子 `sock`（全连接） | ✗ | 收到 ACK 时 | `tcp_create_openreq_child` (slab) |
| accept 队列节点 | ✗ | 收到 ACK 时 | 复用 `request_sock` 的 `dl_next` 链入 |

这是典型的 **lazy allocation** 设计：listen 只声明容量上限、切换状态、注册哈希表，把所有真实内存分配推迟到握手路径按需发生 —— 避免 idle listener 占用额外内存。

#### `reqsk_queue_alloc` 名不副实

函数名带 `alloc` 但实际只做 init —— 历史命名遗留：

```c
// net/core/request_sock.c:36
void reqsk_queue_alloc(struct request_sock_queue *queue)
{
    spin_lock_init(&queue->rskq_lock);          // accept 队列锁
    spin_lock_init(&queue->fastopenq.lock);     // TFO 子队列锁
    queue->fastopenq.rskq_rst_head = NULL;
    queue->fastopenq.rskq_rst_tail = NULL;
    queue->fastopenq.qlen = 0;
    queue->rskq_accept_head = NULL;             // 全连接队列初始为空
}
```

**没有 `kmalloc` / `kmem_cache_alloc` 调用**。5.15 内核也不再为 SYN 队列分配独立哈希表（旧内核曾有 `listen_sock` 私有哈希），半连接直接插入全局 ehash。

#### 加入的是 **两张** 监听哈希表，不是一张

`sk->sk_prot->hash()` 实际指向 `inet_hash()`，其内部对 LISTEN 状态同时写入两张表：

```c
// net/ipv4/inet_hashtables.c:837  __inet_hash()
if (sk->sk_state != TCP_LISTEN) {
    inet_ehash_nolisten(sk, osk, NULL);   // 非 LISTEN → ehash
    return 0;
}
ilb = &hashinfo->listening_hash[inet_sk_listen_hashfn(sk)];
spin_lock(&ilb->lock);
if (sk->sk_reuseport) {
    err = inet_reuseport_add_sock(sk, ilb);   // REUSEPORT 注册
    if (err) goto unlock;
}
__sk_nulls_add_node_rcu(sk, &ilb->nulls_head);   // ① listening_hash（旧表）
inet_hash2(hashinfo, sk);                        // ② lhash2（新表）
ilb->count++;
sock_set_flag(sk, SOCK_RCU_FREE);
```

| 表 | 哈希键 | 桶数 | 作用 |
|----|--------|------|------|
| `listening_hash[32]` | 仅本地端口 | **固定 32**（内联） | 兼容 + 兜底查找 |
| `lhash2` | 本地 IP + 端口 | 动态 2 的幂 | 精确查找，虚拟主机场景性能差 10x+ |

**收 SYN 时的查找顺序**：先查 `lhash2[hash(dst_ip, dst_port)]`（精确）→ 没命中再查 `lhash2[hash(0.0.0.0, dst_port)]`（通配兜底）→ 最后才走 `listening_hash`。`listening_hash` 主要是历史兼容，新场景都靠 `lhash2`。

### 2.4 backlog 落盘位置（速记）

`listen(fd, backlog)` 中的 `backlog` 经过 `somaxconn` 截断后，最终写入 `sk->sk_max_ack_backlog`。它**同时**作为半连接队列（`>=` 判满）和全连接队列（`>` 判满，实容 `backlog+1`）的上限。详细的判满对比见 [§8.4 队列限制对比](#84-队列限制对比)。

---

## 三、准备知识：数据结构与 ehash 生命周期

> 在进入 SYN 处理流程之前，先认识几个关键数据结构。读完本章，你会知道"半连接放在哪、全连接放在哪、accept() 取的是什么"。

### 3.1 `struct request_sock_queue`

```c
// include/net/request_sock.h:173-188
struct request_sock_queue {
    spinlock_t          rskq_lock;
    u8                  rskq_defer_accept;   // TCP_DEFER_ACCEPT

    u32                 synflood_warned;      // SYN flood 警告标志（只告警一次）
    atomic_t            qlen;                 // ★ 半连接队列长度
    atomic_t            young;                // 未重传过 SYN-ACK 的半连接数

    struct request_sock *rskq_accept_head;    // ★ 全连接队列头
    struct request_sock *rskq_accept_tail;    // ★ 全连接队列尾
    struct fastopen_queue fastopenq;          // TFO 队列
};
```

### 3.2 全连接与半连接的存储

```
                     ┌────────────────────────┐
                     │   LISTEN socket (sk)    │
                     │ icsk_accept_queue:      │
                     │   qlen = 半连接数        │  ← atomic 计数器
                     │   rskq_accept_head ──→  │  ← 全连接 FIFO 链表
                     │   sk_ack_backlog = N    │  ← 全连接队列长度
                     │   sk_max_ack_backlog    │  ← backlog 上限
                     └────────────┬───────────┘
                                  │
              ┌───────────────────┼───────────────────┐
              ▼                   │                   ▼
    ┌─────────────────┐           │         ┌─────────────────┐
    │ 半连接 req_sock  │           │         │ 全连接 req_sock  │
    │ TCP_NEW_SYN_RECV │           │         │ req->sk = child │
    │ 存储在 ehash 中   │           │         │ 链入 accept FIFO │
    │ qlen++            │           │         │ sk_ack_backlog++ │
    └─────────────────┘           │         └─────────────────┘
                                  │
                            ┌─────┴─────┐
                            │  ehash    │
                            │ (全局)     │
                            └───────────┘
```

- **全连接队列**：单链表 FIFO（`rskq_accept_head → ... → rskq_accept_tail`），长度通过 `sk->sk_ack_backlog` 跟踪
- **半连接**：不在 `request_sock_queue` 内单独存储，以 `TCP_NEW_SYN_RECV` 状态的 mini socket 插入全局 ehash，通过 `qlen`（`atomic_t`）跟踪

### 3.3 为什么半连接直接放 ehash

旧版本内核（≤ 4.x）半连接存储在 listener 私有哈希表。5.x 改为全局 ehash，原因：

**① 收包路径统一**：`tcp_v4_rcv()` 对所有入站包只做一次 `__inet_lookup_skb()`，通过 `sk->sk_state` 区分 `ESTABLISHED` / `NEW_SYN_RECV` / `TIME_WAIT`。旧版本需要先查 ehash → 查 listening_hash → 查私有半连接表，多两次查找。

**② 锁粒度更细**：ehash 分桶加锁（`inet_ehash_lockp()` 每桶独立 spinlock），不同四元组天然分布在不同桶。旧版本所有半连接操作都竞争 listener 的锁。

**③ 代码复用**：`request_sock` 通过 `req_to_sk()` 伪装成 `struct sock *`，插入 ehash 与普通 socket 完全一样。

### 3.4 Socket 在 ehash 中的完整生命周期

```
阶段一: 收到 SYN（半连接创建）
    reqsk_queue_hash_req()                [inet_connection_sock.c:1002]
    └── inet_ehash_insert(req_to_sk(req), NULL, NULL)
        → request_sock 以 TCP_NEW_SYN_RECV 插入 ehash
        → qlen++

阶段二: 收到 ACK（三次握手完成，原子替换）
    tcp_v4_syn_recv_sock()                [tcp_ipv4.c:2151]
    └── inet_ehash_nolisten(newsk, req_to_sk(req_unhash), ...)
        → 同一个 hash 桶、同一把锁下：
          ① sk_nulls_del_node_init_rcu(osk)   删除 req（TCP_NEW_SYN_RECV）
          ② __sk_nulls_add_node_rcu(sk, list)  插入 newsk（TCP_ESTABLISHED）

    inet_csk_complete_hashdance()          [inet_connection_sock.c:1233]
    └── inet_csk_reqsk_queue_add()
        → req 挂入 listener 的 accept FIFO（req->sk = child）
        → sk_ack_backlog++

    *** 此时 newsk 同时存在于两处 ***
      ● ehash  — 用于后续数据包的收包路由
      ● accept FIFO — 等待用户态 accept() 取走

阶段三: accept() 取走（仅操作 accept FIFO，不涉及 ehash）
    inet_csk_accept()                     [inet_connection_sock.c:560]
    └── reqsk_queue_remove()
        → 从 accept FIFO 取出 req，返回 req->sk 给用户态
        → sk_ack_backlog--
        → reqsk_put(req) 释放 request_sock

    *** newsk 只存在于 ehash（直到 close） ***

阶段四: close() 关闭
    tcp_close() → ... → inet_unhash()
    └── sk_nulls_del_node_init_rcu(sk)
        → 从 ehash 中删除
```

**关键理解**：accept FIFO 只是临时暂存区，存的是 `request_sock`（内含 `req->sk` 指针），不是 socket 本身。子 socket 的"家"始终是 ehash。


---

## 四、两种工作模式（心智模型）

> **先建立一个核心认知**：半连接队列和 SYN Cookie 不是"两个并存的机制"，而是**同一功能（处理 SYN）的两种互斥工作模式**。任何一个 SYN 都只会走其中一种模式，由"队列是否满 + `sysctl_tcp_syncookies` 取值"共同决定。

> **阅读指引**：读完本章后，§5 讲"正常模式"的完整链路，§6 讲"Cookie 模式"的完整链路。

### 4.1 模式选择决策矩阵

| `sysctl_tcp_syncookies` | 半连接队列未满 | 半连接队列已满 |
|-------------------------|---------------|---------------|
| **`0`（禁用）** | 正常模式：建 req、入 ehash、`qlen++` | **直接 DROP SYN**（ListenDrops++） |
| **`1`（默认）** | 正常模式：建 req、入 ehash、`qlen++` | **Cookie 模式**：不建队列、状态编码进 ISN |
| **`2`（强制）** | **强制 Cookie 模式**（测试用） | **强制 Cookie 模式** |

### 4.2 两种模式的端到端对比

```
┌──────────────────────────────────────────────────────────────────┐
│                    正常模式（半连接队列未满）                      │
├──────────────────────────────────────────────────────────────────┤
│ Client → SYN → Server                                          │
│   ① inet_reqsk_alloc()         分配 request_sock（约 256 字节） │
│   ② inet_ehash_insert()        req 伪装成 mini sock 入 ehash    │
│   ③ qlen++                     半连接队列计数器 +1              │
│   ④ send_synack(NORMAL)        SYN-ACK 用正常 ISN              │
│   ⑤ 启动 rsk_timer             SYN-ACK 丢失可重传               │
│                                                                        │
│ Client ← SYN-ACK ← Server                                            │
│ Client → ACK → Server                                                │
│   ⑥ 查 ehash 找到 req          凭四元组匹配                       │
│   ⑦ 用 req 重建完整 sock       tcp_create_openreq_child          │
│   ⑧ 加入 accept 队列           sk_ack_backlog++                  │
│                                                                        │
│ 服务端状态：有 req（ehash）+ 子 sock（accept 队列）              │
└──────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│                    Cookie 模式（半连接队列满 + syncookies>0）     │
├──────────────────────────────────────────────────────────────────┤
│ Client → SYN → Server                                          │
│   ① 不分配 req                   ★ 没有 request_sock            │
│   ② 不入 ehash                   ★ 没有队列条目                 │
│   ③ qlen 不变                    ★ 计数器不动                   │
│   ④ 把状态编码进 ISN             32 位 ISN = cookie            │
│   ⑤ send_synack(COOKIE)          SYN-ACK 的 seq = cookie       │
│   ⑥ reqsk_free(req)              临时 req 立即释放              │
│                                                                        │
│ Client ← SYN-ACK(seq=cookie) ← Server                               │
│ Client → ACK(ack=cookie+1) → Server                                 │
│   ⑦ 从 ack-1 反解 cookie         凭算法还原状态                 │
│   ⑧ 校验哈希 + 提取 MSS/WScale   验证合法性、恢复选项           │
│   ⑨ 凭空重建 req + sock          tcp_create_openreq_child        │
│   ⑩ 加入 accept 队列             sk_ack_backlog++                │
│                                                                        │
│ 服务端状态：只有子 sock（accept 队列），无 req 残留             │
└──────────────────────────────────────────────────────────────────┘
```

### 4.3 三种模式的本质差异（一句话）

| 模式 | 一句话本质 |
|------|-----------|
| **正常模式** | 服务端**保存**连接状态（req 在 ehash 里），等 ACK 来了从队列里取出来 |
| **Cookie 模式** | 服务端**不保存**任何状态，把状态**塞进 SYN-ACK 的 seq 字段**让客户端带回 ACK，凭 ACK 反解 |
| **DROP 模式** | 服务端**拒绝**连接（syncookies=0 时的最后手段） |

### 4.4 Cookie 模式的代价（为什么平时不用）

| 维度 | 正常模式 | Cookie 模式 |
|------|---------|------------|
| 内存 | 每个半连接 256 字节 | 0 字节 |
| TCP 选项 | 全部保留 | **MSS 只能 4 档**（536/1300/1440/1460） |
| WScale/SACK/ECN | 完整 | **靠时间戳捎带**（对端不支持则全丢） |
| SYN-ACK 重传 | 支持（rsk_timer） | **不支持**（没 req 没法重传，丢包由客户端重发 SYN） |
| CPU | 低（查表） | 高（SipHash 加解密） |

> **结论**：Cookie 模式是**抗 SYN Flood 的应急兜底**，不是常规运行模式。`sysctl_tcp_syncookies=1`（默认）的意义是"平时走正常模式，队列被打爆时自动切换"。

---

## 五、正常模式：SYN → req → ehash → SYN-ACK

> 本章覆盖 §4 决策矩阵中"正常模式"那一列的完整链路：从 SYN 到达，到 req 入 ehash，到 SYN-ACK 发出。**Cookie 分支全部剥出到 §6**。

### 5.1 SYN 到达处理路径

```
tcp_v4_rcv() → tcp_v4_do_rcv() → tcp_rcv_state_process()
  [TCP_LISTEN + SYN] → icsk->icsk_af_ops->conn_request()
                        → tcp_v4_conn_request()
                          → tcp_conn_request()
```

### 5.2 `tcp_conn_request()` 核心流程

```c
// net/ipv4/tcp_input.c:8296-8490
int tcp_conn_request(struct request_sock_ops *rsk_ops, ...)
{
    // ======== 检查一：半连接队列是否已满 ========
    if ((syncookies == 2 || inet_csk_reqsk_queue_is_full(sk)) && !isn) {
        want_cookie = tcp_syn_flood_action(sk, rsk_ops->slab_name);
        if (!want_cookie)
            goto drop;
    }

    // ======== 检查二：全连接队列是否已满 ========
    if (sk_acceptq_is_full(sk)) {
        NET_INC_STATS(sock_net(sk), LINUX_MIB_LISTENOVERFLOWS);
        goto drop;
    }

    // ======== 分配 request_sock ========
    req = inet_reqsk_alloc(rsk_ops, sk, !want_cookie);

    // ======== 检查三：max_syn_backlog 75% 阈值（仅 syncookies=0 时）========
    // max_syn_backlog 不是队列上限，仅是压力控制阈值
    if (!want_cookie && !isn) {
        int max_syn_backlog = READ_ONCE(net->ipv4.sysctl_max_syn_backlog);
        if (!syncookies &&
            (max_syn_backlog - inet_csk_reqsk_queue_len(sk) <
             (max_syn_backlog >> 2)) &&
            !tcp_peer_is_proven(req, dst)) {
            goto drop_and_release;
        }
    }

    // ======== 正常路径：加入 ehash + 发送 SYN-ACK ========
    if (!want_cookie)
        inet_csk_reqsk_queue_hash_add(sk, req,
            tcp_timeout_init((struct sock *)req));

    af_ops->send_synack(sk, dst, &fl, req, &foc, ...);


    // （Cookie 分支已剥出到 §6.1，避免打断正常路径的叙事）

drop:
    tcp_listendrop(sk);    // LINUX_MIB_LISTENDROPS++
    return 0;
}
```

### 5.3 三道防线

> **配合 §4 心智模型看**：下面这三道防线决定 SYN 走哪种模式。防线一触发 → 切到 **Cookie 模式**或 **DROP 模式**；防线二、三只在 **正常模式**下生效（Cookie 模式跳过）。

```
SYN 到达
    │
    ▼
┌─ 防线一：inet_csk_reqsk_queue_is_full() ─┐
│ qlen >= sk_max_ack_backlog?               │
│ 是 → SYN Cookie 模式（syncookies>0）       │
│     或直接 DROP（syncookies=0）            │
└──────────────────────────┬────────────────┘
                           │ 通过（继续走正常模式）
                           ▼
┌─ 防线二：sk_acceptq_is_full() ────────────┐
│ 全连接队列满？（sk_ack_backlog > backlog）│
│   → 实容 backlog+1 个（用 > 不用 >=）      │
│ 是 → DROP + LINUX_MIB_LISTENOVERFLOWS     │
│   → 无 SYN Cookie 兜底，与防线一不同       │
└──────────────────────────┬────────────────┘
                           │ 通过
                           ▼
┌─ 防线三：max_syn_backlog 75% 阈值 ────────┐
│ syncookies=0 且队列 > 75% 且对端未验证？    │
│ 是 → DROP（仅放行 tcp_metrics 中有记录的对端）│
└──────────────────────────┬────────────────┘
                           │ 通过
                           ▼
              正常模式：分配 req → 加入 ehash → 发送 SYN-ACK
```

### 5.4 半连接加入 ehash

```c
// net/ipv4/inet_connection_sock.c:1012-1028
static void reqsk_queue_hash_req(struct request_sock *req, unsigned long timeout)
{
    timer_setup(&req->rsk_timer, reqsk_timer_handler, TIMER_PINNED);
    mod_timer(&req->rsk_timer, jiffies + timeout);  // SYN-ACK 重传定时器
    inet_ehash_insert(req_to_sk(req), NULL, NULL);   // 插入全局 ehash
    smp_wmb();
    refcount_set(&req->rsk_refcnt, 2 + 1);
}

void inet_csk_reqsk_queue_hash_add(struct sock *sk, struct request_sock *req,
                                   unsigned long timeout)
{
    reqsk_queue_hash_req(req, timeout);
    inet_csk_reqsk_queue_added(sk);  // qlen++, young++
}
```

---

## 六、Cookie 模式：状态编码进 ISN

> 本章覆盖 §4 决策矩阵中"Cookie 模式"那一列：当半连接队列满且 `syncookies>0` 时，服务端**不保存任何状态**，把连接参数编码进 SYN-ACK 的 seq 字段（ISN），凭客户端回传的 ACK 反解。

### 6.1 `tcp_conn_request()` 中的 Cookie 分支

接 §5.2 的正常路径代码，下面是被剥出的 Cookie 分支（原 `if (want_cookie)` 块）：

```c
    // ======== Cookie 路径：发完 SYN-ACK 立即释放 req ========
    if (want_cookie) {
        reqsk_free(req);
        return 0;
    }
```

**触发条件**（即 §5.3 的"防线一"）：

```c
if ((syncookies == 2 || inet_csk_reqsk_queue_is_full(sk)) && !isn) {
    want_cookie = tcp_syn_flood_action(sk, rsk_ops->slab_name);
    if (!want_cookie)
        goto drop;    // syncookies=0 时直接 DROP（无兜底）
}
```

**三态决策**（syncookies 取值 × 队列状态 → 模式）：

| `sysctl_tcp_syncookies` | 队列未满 | 队列已满 |
|-------------------------|---------|----------|
| `0` | 正常模式 | **DROP**（ListenDrops++）|
| `1`（默认）| 正常模式 | **Cookie 模式** |
| `2` | **强制 Cookie** | **强制 Cookie** |

### 6.2 SYN Cookie 机制

半连接队列满时（`inet_csk_reqsk_queue_is_full` 且 `syncookies>0`），`tcp_syn_flood_action()` 返回 `want_cookie=true`，启用无状态 SYN-ACK：

```
正常三次握手:                        SYN Cookie:
Client → SYN → Server               Client → SYN → Server
  [Server 创建 req, 存入 ehash]        [Server 不保存任何状态]
Server → SYN-ACK → Client           Server → SYN-ACK(ISN=cookie) → Client
  [Server 等待 ACK]                    [Server 忘记此连接]
Client → ACK → Server               Client → ACK(cookie+1) → Server
  [Server 查 ehash 找到 req]           [Server 从 ACK 中反解 cookie, 重建 req]
```

#### ISN 32 位编码

Cookie 将连接状态编码到 32 位 ISN 中。源码（`net/ipv4/syncookies.c`）定义：

```c
#define COOKIEBITS 24                  // 低位固定 24 bit
#define COOKIEMASK (((__u32)1 << COOKIEBITS) - 1)   // 0x00FFFFFF

// 生成 cookie（secure_tcp_syn_cookie）：
return cookie_hash(..., 0, 0) + sseq            // 第一层哈希 + 服务端 ISN
     + (count << COOKIEBITS)                     // 时间戳（8 bit）放到高 8 位
     + ((cookie_hash(..., count, 1) + data) & COOKIEMASK);  // 第二层哈希 + mssind，截断到低 24 位
```

| 字段 | 实际位数 | 在 ISN 中的位置 | 说明 |
|------|---------|----------------|------|
| `count`（时间戳） | **8 位** | 高 8 位（bit 24–31） | 分钟级，`count << 24`，与当前时间比较做过期校验（`MAX_SYNCOOKIE_AGE = 2` 分钟） |
| 第二层哈希 + `data` | **24 位** | 低 24 位（bit 0–23） | `siphash(..., count, 1) + mssind`，**mssind 混入哈希而非独立位域** |
| `mssind`（MSS 索引） | **2 位**（非独立位） | 嵌在低 24 位内 | `msstab[]` 只有 4 项 → 索引值域 0–3 → 2 bit；通过 `count` 反算剥离 |

**解码路径**（`__cookie_v4_check`）：

```c
// 1. 剥去第一层哈希和服务端 ISN
cookie -= cookie_hash(saddr, daddr, sport, dport, 0, 0) + sseq;
// 现在 cookie = (count << 24) | ((hash2 + mssind) & 0xFFFFFF)

// 2. 提取 count（高 8 位）
diff = (current_count - (cookie >> 24)) & 0xFF;
if (diff >= MAX_SYNCOOKIE_AGE) return -1;   // 过期

// 3. 用恢复的 count 重算第二层哈希，剥离出 mssind
data = cookie - cookie_hash(saddr, daddr, sport, dport, count - diff, 1);
mssind = data & 0x3;      // 2 位索引
mss = msstab[mssind];     // 量化后的 MSS
```

> **注意**：网上不少文章把位布局写成"时间戳 5 位 / MSS 索引 3 位 / 哈希 24 位"——这是早期 2.4/2.6 内核的写法。5.15 中 `COOKIEBITS = 24`（`syncookies.c:33`），`count` 占 8 位、`mssind` 占 2 位并嵌入 24 位低位区。

```c
// net/ipv4/syncookies.c:150 — MSS 只有 4 个档位
static __u16 const msstab[] = {
    536,
    1300,
    1440,   /* PPPoE */
    1460,
};

// 将客户端通告的 MSS 向下量化到预定义表中最近的值
for (mssind = ARRAY_SIZE(msstab) - 1; mssind ; mssind--)
    if (mss >= msstab[mssind])
        break;
*mssp = msstab[mssind];  // 例如客户端通告 MSS=1400，量化后变为 1300
```

#### WScale/SACK/ECN 的编码：借用时间戳

ISN 没有空间存 WScale/SACK/ECN，内核将它们编码到 SYN-ACK 时间戳选项的 TSecr 低 6 位中：

```c
// net/ipv4/syncookies.c:38-40
#define TS_OPT_WSCALE_MASK  0xf     // 低4位: 窗口缩放因子(0~14), 0xf=不支持
#define TS_OPT_SACK         BIT(4)  // 第5位: SACK Permitted
#define TS_OPT_ECN          BIT(5)  // 第6位: ECN

// 编码（发送 SYN-ACK 时）:
options = ireq->wscale_ok ? ireq->snd_wscale : TS_OPT_WSCALE_MASK;
if (ireq->sack_ok)  options |= TS_OPT_SACK;
if (ireq->ecn_ok)   options |= TS_OPT_ECN;
ts = ts_now & ~TSMASK;
ts |= options;  // 选项编码到时间戳低位

// 解码（收到 ACK 时, cookie_timestamp_decode）:
tcp_opt->sack_ok = (options & TS_OPT_SACK) ? TCP_SACK_SEEN : 0;
tcp_opt->wscale_ok = 1;
tcp_opt->snd_wscale = options & TS_OPT_WSCALE_MASK;
```

#### Cookie 的代价

| 对端是否支持时间戳 | 丢失的信息 | 性能影响 |
|---|---|---|
| **支持** | MSS 精度降低（4 个档位） | 较小，WScale/SACK/ECN 通过时间戳保留 |
| **不支持** | MSS 精度降低 + WScale/SACK/ECN **全部丢失** | 窗口最大 65535，丢包只能全部重传 |

```c
// net/ipv4/tcp_input.c:8406 — 对端不支持时间戳时清除所有选项
if (want_cookie && !tmp_opt.saw_tstamp)
    tcp_clear_options(&tmp_opt);
```

**`tcp_syn_flood_action()`**：

```c
// net/ipv4/tcp_input.c:8182-8210
static bool tcp_syn_flood_action(const struct sock *sk, const char *proto)
{
    struct request_sock_queue *queue = &inet_csk(sk)->icsk_accept_queue;
    bool want_cookie = false;
    u8 syncookies = READ_ONCE(net->ipv4.sysctl_tcp_syncookies);

#ifdef CONFIG_SYN_COOKIES
    if (syncookies) {
        want_cookie = true;
        __NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPREQQFULLDOCOOKIES);
    } else
#endif
        __NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPREQQFULLDROP);

    // ★ 每 listener 只告警一次（xchg 原子置位 synflood_warned）
    // syncookies=2（管理员强制 Cookie 模式）不告警：非异常
    if (!queue->synflood_warned && syncookies != 2 &&
        xchg(&queue->synflood_warned, 1) == 0)
        net_info_ratelimited("%s: Possible SYN flooding on port %d. %s. Check SNMP counters.\n",
                             proto, sk->sk_num, msg);
    return want_cookie;
}
```

---

## 七、ACK 处理与全连接队列

### 7.1 三次握手完成：ACK → 创建子 socket → 入队

```
客户端 ACK 到达
    │
    ▼
tcp_v4_rcv(): sk->sk_state == TCP_NEW_SYN_RECV
    │
    ▼
tcp_check_req()                              [net/ipv4/tcp_minisocks.c]
    ├── syn_recv_sock() → tcp_v4_syn_recv_sock()
    │   ├── sk_acceptq_is_full(sk)?          // ★ 全连接队列满检查
    │   │   ├── 是 → LINUX_MIB_LISTENOVERFLOWS + LISTENDROPS → drop
    │   │   └── 否 → tcp_create_openreq_child() → 创建子 sock
    │   └── inet_csk_complete_hashdance()
    │       ├── inet_csk_reqsk_queue_drop()  // 从 ehash 移除 req
    │       ├── reqsk_queue_removed()        // qlen--
    │       └── inet_csk_reqsk_queue_add()   // 加入 accept FIFO
    │           └── sk_acceptq_added(sk)     // sk_ack_backlog++
    └── [溢出] listen_overflow 标签
        ├── tcp_abort_on_overflow=0: acked=1, return NULL（静默丢弃 ACK）
        └── tcp_abort_on_overflow=1: send_reset() → 发 RST
```

### 7.2 加入全连接队列

```c
// net/ipv4/inet_connection_sock.c:1229-1255
struct sock *inet_csk_reqsk_queue_add(struct sock *sk,
                                      struct request_sock *req,
                                      struct sock *child)
{
    struct request_sock_queue *queue = &inet_csk(sk)->icsk_accept_queue;

    spin_lock(&queue->rskq_lock);
    if (unlikely(sk->sk_state != TCP_LISTEN)) {
        inet_child_forget(sk, req, child);
        child = NULL;
    } else {
        req->sk = child;           // req 关联到子 socket
        req->dl_next = NULL;
        if (queue->rskq_accept_head == NULL)
            WRITE_ONCE(queue->rskq_accept_head, req);
        else
            queue->rskq_accept_tail->dl_next = req;
        queue->rskq_accept_tail = req;
        sk_acceptq_added(sk);      // ★ sk_ack_backlog++
    }
    spin_unlock(&queue->rskq_lock);
    return child;
}
```

### 7.3 `accept()` 取出连接

```c
// include/net/request_sock.h:198-212
static inline struct request_sock *reqsk_queue_remove(struct request_sock_queue *queue,
                                                      struct sock *parent)
{
    struct request_sock *req;

    spin_lock_bh(&queue->rskq_lock);
    req = queue->rskq_accept_head;
    if (req) {
        sk_acceptq_removed(parent);                          // ★ sk_ack_backlog--
        WRITE_ONCE(queue->rskq_accept_head, req->dl_next);
        if (queue->rskq_accept_head == NULL)
            queue->rskq_accept_tail = NULL;
    }
    spin_unlock_bh(&queue->rskq_lock);
    return req;
}
```

`inet_csk_accept()` 调用 `reqsk_queue_remove()` 取出连接后，返回 `req->sk`（子 socket）。

---

## 八、溢出、排查与调优（一站式）

> 把散落在 §5.6 / §6.4 / §7 的溢出处理、参数对比、排查命令合并到一章，读者遇到队列问题直接翻这里。

### 8.1 半连接溢出的后果

| 条件 | 结果 |
|------|------|
| `syncookies=1`（默认） | 启用 SYN Cookie，SYN-ACK 正常发送但不保存状态；连接可建立但丢失部分 TCP 选项 |
| `syncookies=0` | **直接丢弃 SYN**，客户端 connect() 超时（~75s） |
| `syncookies=2` | **所有 SYN** 都用 Cookie（不管队列是否满） |
| 队列 > 75% 且 `syncookies=0` | 只接受 `tcp_metrics` 中有记录的对端 |

---

### 8.2 全连接溢出与 `tcp_abort_on_overflow`

全连接队列溢出发生在**两个检查点**：

**检查点一**：`tcp_conn_request()` 中（收 SYN 时提前检查）——丢弃 SYN，递增 `ListenOverflows`

**检查点二**：`tcp_v4_syn_recv_sock()` 中（创建子 socket 时）——递增 `ListenOverflows` + `ListenDrops`

> **与半连接溢出的关键差异**：半连接满时还能切 SYN Cookie 兜底（syncookies>0）；**全连接满时没有任何兜底机制**，直接丢包。原因是 SYN Cookie 只能推迟"建 req"，无法推迟"建完整 sock 入 accept 队列"——accept 队列真没地方放了。

溢出后的行为由 `tcp_abort_on_overflow` 控制（`tcp_check_req()` 的 `listen_overflow` 标签）：

```c
// net/ipv4/tcp_minisocks.c:902-918
listen_overflow:
    if (!READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_abort_on_overflow)) {
        inet_rsk(req)->acked = 1;    // 标记已收到 ACK
        return NULL;                  // 静默丢弃（等 SYN-ACK 重传）
    }

embryonic_reset:
    if (!(flg & TCP_FLAG_RST))
        req->rsk_ops->send_reset(sk, skb);   // ★ 发 RST
    inet_csk_reqsk_queue_drop(sk, req);
```

| `tcp_abort_on_overflow` | 行为 | 客户端表现 |
|-------------------------|------|-----------|
| `0`（默认） | 静默丢弃 ACK，服务端重传 SYN-ACK → 客户端重发 ACK → 循环直到有空间或超时 | connect() 成功但 send() 后长时间无响应 |
| `1` | 发 RST | connect() 返回 `ECONNRESET` |

---

### 8.3 参数总览

| 参数 | 默认值 | 作用 | 所在函数 |
|------|--------|------|---------|
| `listen(fd, backlog)` | 应用指定 | 全连接+半连接队列上限 | `__sys_listen` |
| `net.core.somaxconn` | 4096 | `backlog` 上界 | `__sys_listen` |
| `net.ipv4.tcp_max_syn_backlog` | `max(128, ehash/128)` | `syncookies=0` 时 75% 丢弃阈值 | `tcp_conn_request` |
| `net.ipv4.tcp_syncookies` | 1 | SYN Cookie 开关 | `tcp_conn_request` |
| `net.ipv4.tcp_synack_retries` | 5 | SYN-ACK 重传次数 | `reqsk_timer_handler` |
| `net.ipv4.tcp_abort_on_overflow` | 0 | 全连接溢出时是否发 RST | `tcp_check_req` |

### 8.4 队列限制对比

| | 半连接队列 | 全连接队列 |
|---|---|---|
| **上限** | `min(backlog, somaxconn)` | `min(backlog, somaxconn)` |
| **判满** | `qlen >= sk_max_ack_backlog` | `sk_ack_backlog > sk_max_ack_backlog` |
| **可容纳** | `backlog` 个 | `backlog + 1` 个 |
| **存储** | 全局 ehash | listener 的 accept FIFO 链表 |
| **跟踪** | `atomic_t qlen` | `sk->sk_ack_backlog` |

### 8.5 SNMP 计数器与队列对应

```bash
nstat -az | grep -E 'Listen|SynFlood|Cookie'
```

| 计数器                    | 递增位置                                            | 说明                                 |
| ---------------------- | ----------------------------------------------- | ---------------------------------- |
| `TCPReqQFullDrop`      | `tcp_syn_flood_action()` 且 `syncookies=0`       | **半连接**满 + Cookie 关闭 → SYN 被丢弃     |
| `TCPReqQFullDoCookies` | `tcp_syn_flood_action()` 且 `syncookies>0`       | **半连接**满 + Cookie 开启 → 走 Cookie 路径 |
| `SyncookiesSent`       | `cookie_init_sequence()`                        | 实际发出的 SYN Cookie 数                 |
| `ListenOverflows`      | `tcp_conn_request()` + `tcp_v4_syn_recv_sock()` | **全连接**队列满                         |
| `ListenDrops`          | `tcp_listendrop()`                              | 所有 drop 路径末尾调用（半连接+全连接溢出均递增）       |

**判断技巧**：

- `ListenDrops` 增长但 `ListenOverflows` 不增长 → **半连接**溢出
- `ListenOverflows` 增长 → **全连接**溢出（`ListenDrops` 也会同步增长）
- `TCPReqQFullDoCookies` 增长 → **半连接**溢出且 Cookie 在工作

### 8.6 排查决策树

```
客户端连接异常
    │
    ▼
nstat -az | grep -E 'Listen|TCPReqQ|Cookie'
    │
    ├── ListenOverflows 持续增长？
    │   │
    │   └── 是 ──→ ★ 全连接队列溢出
    │             确认: ss -ltn → Recv-Q 接近 Send-Q？
    │             根因: 应用 accept() 太慢
    │             解决: ① 优化 accept 速度
    │                   ② 增大 backlog 和 somaxconn（治标）
    │                   ③ 考虑 tcp_abort_on_overflow=1
    │
    ├── TCPReqQFullDoCookies 增长？
    │   │
    │   └── 是 ──→ ★ 半连接溢出（Cookie 兜底中）
    │             确认: ss -tn state syn-recv | wc -l
    │             解决: ① 增大 backlog 和 somaxconn
    │                   ② 配合 iptables rate limit
    │                   ③ 降低 tcp_synack_retries
    │
    ├── TCPReqQFullDrop 增长？
    │   │
    │   └── 是 ──→ ★ 半连接溢出（无 Cookie！）
    │             解决: 开启 syncookies=1
    │
    └── 均无增长 → 非队列问题，检查路由/防火墙/应用层
```

### 8.7 监控命令

```bash
# ss -ltn 解读: Recv-Q=当前全连接数, Send-Q=backlog上限
ss -ltn

# 查看半连接
ss -tn state syn-recv

# 全连接队列使用率
watch -n1 'ss -ltn | awk "NR>1 {usage=\$2/\$3*100; printf \"%-20s %s/%s (%.0f%%)\n\", \$4, \$2, \$3, usage}"'

# 溢出增量
watch -n1 'nstat -az | grep -E "ListenOverflows|ListenDrops|TCPReqQFull"'
```

SYN flood 被检测到时，内核输出（每 listener 一次）：

```
TCP: Possible SYN flooding on port 80. Sending cookies. Check SNMP counters.
```

### 8.8 生产环境建议

| 场景 | 建议配置 |
|------|----------|
| 高并发 Web 服务器 | `somaxconn=65535`，`backlog=65535`，`syncookies=1` |
| 低延迟服务 | `tcp_abort_on_overflow=1`（快速失败），客户端有重试 |
| 被 SYN Flood 攻击 | `syncookies=1`（默认已开启），配合 `iptables` rate limit |
| 排查连接超时 | 先看 `nstat` 的 `ListenOverflows`/`ListenDrops`，再看 `ss -ltn` 的 `Recv-Q` |


---

## 九、历史演进

> 给从旧内核迁移过来的读者：5.15 与 4.x 及更早版本的关键差异。


| 特性 | 旧版本（≤ 4.x） | 5.15 |
|------|-----------------|------|
| 半连接存储 | listener 私有哈希表（`listen_sock`） | 全局 `ehash`（`TCP_NEW_SYN_RECV` mini socket） |
| 半连接队列大小 | `min(backlog, somaxconn, tcp_max_syn_backlog) + 1` 上取整到 2 的幂次，最小 16 | `sk_max_ack_backlog`（= `min(backlog, somaxconn)`），原子计数器判满 |
| `max_qlen_log` 字段 | 存在，存储 log2(队列大小) | **不存在**（5.15 内核树中搜索为零） |
| `reqsk_queue_alloc` 参数 | 接受 `nr_table_entries`，分配哈希表 | 无参数，仅初始化锁和指针 |
| `tcp_max_syn_backlog` 作用 | 参与队列大小计算 | **仅**在 `syncookies=0` 时作为 75% 压力丢弃阈值 |
