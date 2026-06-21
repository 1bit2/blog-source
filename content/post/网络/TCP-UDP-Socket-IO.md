+++
date = '2026-04-16'
title = 'TCP/UDP Socket I/O 机制详解'
weight = 9
tags = [
    "TCP",
    "UDP",
    "sendmsg",
    "recvmsg",
    "Socket-IO",
]
categories = [
    "网络",
]
+++
# TCP/UDP Socket I/O 机制详解

> 基于 Linux 5.15.78 内核源码分析，所有内容均来自实际源码实现。

---

## 1. 阻塞 UDP socket 与非阻塞 UDP socket 发送数据的区别

### 1.1 发送路径

```
用户空间 sendto() / sendmsg()
  → __sys_sendto()                          [net/socket.c]
    → sock->ops->sendmsg()
      → udp_sendmsg()                       [net/ipv4/udp.c]
        → ip_make_skb()                     [net/ipv4/ip_output.c]
          → __ip_append_data()
            → sock_alloc_send_skb()         [net/core/sock.c]
              → sock_alloc_send_pskb()      ← 阻塞/非阻塞的分歧点
```

### 1.2 非阻塞标志的传递

```c
// net/socket.c: __sys_sendto()
if (sock->file->f_flags & O_NONBLOCK)   // socket创建时设置了SOCK_NONBLOCK
    flags |= MSG_DONTWAIT;               // 转换为MSG_DONTWAIT标志
msg.msg_flags = flags;
```

`O_NONBLOCK`（文件级）和 `MSG_DONTWAIT`（消息级）最终都通过 `MSG_DONTWAIT` 标志传递到底层。

### 1.3 核心分歧点：sock_alloc_send_pskb()

```c
// net/core/sock.c: sock_alloc_send_pskb()
timeo = sock_sndtimeo(sk, noblock);   // noblock = flags & MSG_DONTWAIT
                                        // 非阻塞: timeo=0, 阻塞: timeo=sk->sk_sndtimeo

for (;;) {
    if (sk_wmem_alloc_get(sk) < READ_ONCE(sk->sk_sndbuf))
        break;                          // 发送缓冲区有空间 → 分配skb

    err = -EAGAIN;
    if (!timeo)
        goto failure;                   // ★ 非阻塞: 立即返回-EAGAIN

    timeo = sock_wait_for_wmem(sk, timeo);  // ★ 阻塞: 睡眠等待空间释放
}
```

### 1.4 对比总结

| 场景 | 阻塞 UDP | 非阻塞 UDP |
|------|----------|------------|
| 缓冲区有空间 | 分配skb → 拷贝数据 → `udp_send_skb()` → 立即返回发送字节数 | 完全相同 |
| 缓冲区满 (`sk_wmem_alloc >= sk_sndbuf`) | 进程睡眠在 `sock_wait_for_wmem()`，等待已发送skb被网卡释放后唤醒 | 立即返回 `-EAGAIN`（errno=EAGAIN） |
| 超时控制 | `SO_SNDTIMEO` 设置超时，默认0（无限等待） | 不适用 |

**关键区别**：UDP 没有发送队列（不像 TCP 有重传队列），每次 `sendmsg` 是"一次性"操作——分配 skb、拷贝数据、发到 IP 层。阻塞只发生在**分配 skb 时发送缓冲区内存不足**，而非"等待对端确认"。

---

## 2. 阻塞 TCP socket 与非阻塞 TCP socket 发送数据的区别

### 2.1 发送路径

```
用户空间 send() / write()
  → tcp_sendmsg()                           [net/ipv4/tcp.c]
    → tcp_sendmsg_locked()
      → 循环: 将用户数据拷贝到 sk_write_queue 中的 skb
        → sk_stream_memory_free(sk) 检查缓冲区
          → 有空间: 拷贝数据
          → 无空间: goto wait_for_space
      → tcp_push() 触发实际发送
```

### 2.2 核心分歧点：sk_stream_wait_memory()

```c
// net/ipv4/tcp.c: tcp_sendmsg_locked()
timeo = sock_sndtimeo(sk, flags & MSG_DONTWAIT);

// 主循环: 将用户数据拷贝到sk_write_queue
while (msg_data_left(msg)) {
    if (!sk_stream_memory_free(sk))    // sk_wmem_queued >= sk_sndbuf ?
        goto wait_for_space;
    // ... 拷贝数据到skb ...
}

wait_for_space:
    err = sk_stream_wait_memory(sk, &timeo);
```

```c
// net/core/stream.c: sk_stream_wait_memory()
while (1) {
    if (!*timeo_p)
        goto do_eagain;                 // ★ 非阻塞: 返回-EAGAIN

    sk_wait_event(sk, &current_timeo,
        sk_stream_memory_free(sk), &wait);  // ★ 阻塞: 睡眠等待ACK释放缓冲区
}
```

### 2.3 对比总结

| 场景 | 阻塞 TCP | 非阻塞 TCP |
|------|----------|------------|
| 缓冲区有空间 | 拷贝数据到 `sk_write_queue`，`tcp_push()` 触发发送，返回已拷贝字节数 | 完全相同 |
| 缓冲区满 (`sk_wmem_queued >= sk_sndbuf`) | 睡眠在 `sk_stream_wait_memory()`，等待对端 ACK 释放缓冲区后继续拷贝 | 返回**已拷贝的字节数**（部分写入），或 `-EAGAIN`（一个字节都没拷贝） |
| 数据完整性 | 保证写完所有数据才返回（除非出错/信号中断） | 可能只写入部分数据（返回值 < 请求长度） |

### 2.4 TCP 与 UDP 发送的本质区别

```
UDP: sendmsg → 分配skb → 拷贝数据 → 立即发到IP层 → 返回
     (一次性操作，不保留副本，不重传)

TCP: sendmsg → 拷贝数据到sk_write_queue → 返回
     (数据进入内核缓冲区后，由TCP状态机异步发送、重传、确认)
```

**TCP `sendmsg` 返回成功不代表数据已发送到对端**，只代表数据已拷贝到内核发送缓冲区。

---

## 3. TCP 调用 connect 怎么知道三次握手成功了

### 3.1 connect 系统调用路径

```c
// net/ipv4/af_inet.c: __inet_stream_connect()

// 第1步: 发送SYN
err = sk->sk_prot->connect(sk, uaddr, addr_len);  // → tcp_v4_connect()
// 此时 sk->sk_state = TCP_SYN_SENT

// 第2步: 计算等待超时
timeo = sock_sndtimeo(sk, flags & O_NONBLOCK);
// 阻塞: timeo = sk->sk_sndtimeo (默认很大)
// 非阻塞: timeo = 0

// 第3步: 等待状态变化
if ((1 << sk->sk_state) & (TCPF_SYN_SENT | TCPF_SYN_RECV)) {
    if (!timeo || !inet_wait_for_connect(sk, timeo, writebias))
        goto out;  // 非阻塞: timeo=0, 返回-EINPROGRESS
}

// 第4步: 醒来后检查状态
if (sk->sk_state == TCP_ESTABLISHED)
    // 三次握手成功
```

### 3.2 阻塞等待的实现

```c
// net/ipv4/af_inet.c: inet_wait_for_connect()
static long inet_wait_for_connect(struct sock *sk, long timeo, int writebias)
{
    DEFINE_WAIT_FUNC(wait, woken_wake_function);
    add_wait_queue(sk_sleep(sk), &wait);       // 注册到socket等待队列

    while ((1 << sk->sk_state) & (TCPF_SYN_SENT | TCPF_SYN_RECV)) {
        release_sock(sk);                       // 释放锁，让softirq能处理收包
        timeo = wait_woken(&wait, TASK_INTERRUPTIBLE, timeo);  // 睡眠
        lock_sock(sk);
        if (signal_pending(current) || !timeo)
            break;                              // 信号中断或超时
    }
    remove_wait_queue(sk_sleep(sk), &wait);
    return timeo;
}
```

### 3.3 唤醒时机：softirq 收到 SYN-ACK

```c
// net/ipv4/tcp_input.c: tcp_rcv_synsent_state_process()
// softirq上下文中处理收到的SYN-ACK:

tcp_finish_connect(sk, skb);       // 状态 → TCP_ESTABLISHED
                                    // 初始化拥塞控制、路由缓存等

if (!sock_flag(sk, SOCK_DEAD)) {
    sk->sk_state_change(sk);       // ★ 唤醒connect()中睡眠的进程
    sk_wake_async(sk, SOCK_WAKE_IO, POLL_OUT);  // 发送SIGIO(O_ASYNC)
}
```

`sk->sk_state_change` 默认指向 `sock_def_wakeup()`：

```c
// net/core/sock.c: sock_def_wakeup()
static void sock_def_wakeup(struct sock *sk)
{
    struct socket_wq *wq;
    rcu_read_lock();
    wq = rcu_dereference(sk->sk_wq);
    if (skwq_has_sleeper(wq))
        wake_up_interruptible_all(&wq->wait);  // 唤醒所有等待者
    rcu_read_unlock();
}
```

### 3.4 非阻塞 connect 的流程

```
应用调用 connect()
  → tcp_v4_connect() 发送SYN
  → timeo = 0 (O_NONBLOCK)
  → 不调用 inet_wait_for_connect()
  → 返回 -EINPROGRESS

应用通过 epoll_wait() 等待
  → softirq收到SYN-ACK → sk_state_change → 唤醒epoll
  → epoll返回EPOLLOUT → 连接成功
  → 或返回EPOLLERR → 连接失败，getsockopt(SO_ERROR)获取错误码
```

---

## 4. 三次握手成功后怎么通知 epoll / select

### 4.1 通知链

```
softirq 收到 SYN-ACK
  → tcp_rcv_synsent_state_process()
    → tcp_finish_connect(sk, skb)           // sk_state = ESTABLISHED
    → sk->sk_state_change(sk)               // = sock_def_wakeup()
      → wake_up_interruptible_all(&wq->wait)
        → 唤醒 wq->wait 上的所有等待者
          → epoll: ep_poll_callback()
          → select/poll: 进程被唤醒
```

### 4.2 epoll 的注册机制

```c
// net/ipv4/tcp.c: tcp_poll()
__poll_t tcp_poll(struct file *file, struct socket *sock, poll_table *wait)
{
    sock_poll_wait(file, sock, wait);
    // ★ 将epoll的等待条目(epitem)注册到 sk->sk_wq->wait 上
    // 后续 sk_state_change → wake_up 时，ep_poll_callback被调用
    ...
}
```

### 4.3 epoll 唤醒后的状态判断

当 `sock_def_wakeup()` 唤醒 `wq->wait` 上的等待者时：

- **epoll**：`ep_poll_callback()` 被调用 → 将就绪的 epitem 加入 `rdllist`（就绪链表）→ 唤醒 `epoll_wait()` 阻塞的进程
- **select/poll**：进程被唤醒后重新扫描所有 fd，调用 `tcp_poll()` 检查状态

```c
// net/ipv4/tcp.c: tcp_poll() 中的关键判断逻辑:

if (sk->sk_state == TCP_ESTABLISHED || ...) {
    if (sk_stream_is_writeable(sk))
        mask |= EPOLLOUT | EPOLLWRNORM;    // ★ 连接完成 = 可写
}

if (sk->sk_err || !skb_queue_empty(&sk->sk_error_queue))
    mask |= EPOLLERR;                      // 连接失败 = 错误
```

### 4.4 服务端 accept() 的通知机制

服务端三次握手完成后的通知链不同：

```
softirq 收到客户端 ACK (三次握手最后一步)
  → tcp_check_req()                         // 验证ACK，创建子socket
    → inet_csk_reqsk_queue_add()            // 子socket加入全连接队列
    → sk->sk_data_ready(sk)                 // = sock_def_readable()
      → wake_up_interruptible_sync_poll(&wq->wait, EPOLLIN)
        → 唤醒 accept() 或 epoll_wait()
```

服务端监听 `EPOLLIN` 表示有新连接可 accept。

---

## 5. TCP/UDP 的发送、接收缓冲区与 sk_buff 的关系

### 5.1 缓冲区不是 sk_buff 的数量，而是字节数

```c
// include/net/sock.h: struct sock 中的关键字段

int sk_sndbuf;              // 发送缓冲区上限（字节），SO_SNDBUF设置
int sk_rcvbuf;              // 接收缓冲区上限（字节），SO_RCVBUF设置

refcount_t sk_wmem_alloc;   // 发送方向已使用内存（字节）
atomic_t   sk_rmem_alloc;   // 接收方向已使用内存（字节）

int sk_wmem_queued;         // TCP发送队列中已排队内存（字节）
```

### 5.2 每个 sk_buff 的内存计费

```c
// net/core/sock.c: skb_set_owner_w() — 发送方向
refcount_add(skb->truesize, &sk->sk_wmem_alloc);
// skb释放时(sock_wfree)自动减回

// include/net/sock.h: skb_set_owner_r() — 接收方向
atomic_add(skb->truesize, &sk->sk_rmem_alloc);
// skb释放时(sock_rfree)自动减回
```

### 5.3 truesize：skb 的真实内存占用

`skb->truesize` 不等于数据长度，它包括：

```
truesize = sizeof(struct sk_buff)           // ~240字节(结构体本身)
         + (skb->end - skb->head)           // 线性数据区
         + 分片页面内存(frags[])             // 如果有分片
```

### 5.4 缓冲区"满"的判断

```c
// TCP 发送缓冲区满:
sk->sk_wmem_queued >= sk->sk_sndbuf         // sk_stream_memory_free()

// UDP 发送缓冲区满:
sk_wmem_alloc_get(sk) >= sk->sk_sndbuf      // sock_alloc_send_pskb()

// TCP 接收缓冲区满:
atomic_read(&sk->sk_rmem_alloc) > sk->sk_rcvbuf  // tcp_try_rmem_schedule()
```

### 5.5 缓冲区大小与 skb 数量的关系

缓冲区容量以**字节**为单位，skb 数量取决于每个 skb 的 `truesize`：

```
假设 sk_sndbuf = 87380 (默认值)
假设每个 skb 的 truesize ≈ 2048 (1个MSS的数据 + 元数据开销)
则发送缓冲区大约能容纳 ≈ 42 个 skb

但这不是固定数量：
- 小包(如ACK): truesize ≈ 768字节 → 能放更多
- 大包(如TSO/GSO): truesize可能数万字节 → 能放更少
```

### 5.6 sk_wmem_alloc 与 sk_wmem_queued 的区别（TCP特有）

```
sk_wmem_queued: sk_write_queue中排队的skb内存总和
               (数据还在TCP发送缓冲区中，未发送或等待ACK确认)

sk_wmem_alloc:  所有关联到socket的skb内存总和
               (包括已交给IP层/网卡但尚未释放的skb)

关系: sk_wmem_alloc >= sk_wmem_queued
     (因为sk_wmem_alloc还包含已发出但未释放的skb)
```

TCP 使用 `sk_wmem_queued` 判断发送缓冲区是否满（因为已发出的包不应阻止新数据进入缓冲区），而 UDP 使用 `sk_wmem_alloc`（因为 UDP 没有发送队列）。

---

## 源码文件索引

| 文件 | 关键函数/结构 |
|------|--------------|
| `net/socket.c` | `__sys_sendto()` — O_NONBLOCK → MSG_DONTWAIT 转换 |
| `net/ipv4/udp.c` | `udp_sendmsg()` — UDP 发送入口 |
| `net/ipv4/ip_output.c` | `__ip_append_data()` → `sock_alloc_send_skb()` |
| `net/core/sock.c` | `sock_alloc_send_pskb()` — UDP 阻塞/非阻塞分歧点 |
| `net/core/sock.c` | `sock_def_wakeup()` — 默认状态变化回调 |
| `net/core/sock.c` | `skb_set_owner_w()` — 发送方向内存计费 |
| `net/ipv4/tcp.c` | `tcp_sendmsg_locked()` — TCP 发送入口 |
| `net/ipv4/tcp.c` | `tcp_poll()` — TCP poll/epoll 状态判断 |
| `net/core/stream.c` | `sk_stream_wait_memory()` — TCP 阻塞/非阻塞分歧点 |
| `net/ipv4/af_inet.c` | `__inet_stream_connect()` — connect 系统调用 |
| `net/ipv4/af_inet.c` | `inet_wait_for_connect()` — connect 阻塞等待 |
| `net/ipv4/tcp_input.c` | `tcp_rcv_synsent_state_process()` — SYN-ACK 处理 |
| `net/ipv4/tcp_input.c` | `tcp_finish_connect()` — 连接建立完成 |
| `include/net/sock.h` | `struct sock` — sk_sndbuf/sk_rcvbuf/sk_wmem_alloc 等 |
