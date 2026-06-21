+++
date = '2026-06-01'
title = 'TCP/UDP 深度掌握问答与实战问题——从内核源码到生产实践'
weight = 27
tags = [
    "TCP",
    "UDP",
    "tcp_ack",
    "tcp_rcv_established",
    "tcp_close",
    "tcp_keepalive",
    "SYN_COOKIES",
    "SACK",
    "SO_REUSEPORT",
    "udp_sendmsg",
    "udp_recvmsg",
    "TIME_WAIT",
    "SO_LINGER",
    "tcp_memory_pressure",
    "GRO",
    "GSO",
    "面试",
    "实战",
]
categories = [
    "网络",
]
+++
# TCP/UDP 深度掌握问答与实战问题

> 基于 Linux 5.15.78 内核源码。每个问题均标注源码位置，可直接对照验证。

---

## 第一部分：证明对 TCP 深度掌握的问题

### Q1. `tcp_rcv_established()` 的快速路径和慢速路径分别在什么条件下走？快速路径到底快在哪里？

**答：** 快速路径由头部预测（header prediction）机制控制，三个条件必须**同时满足**（`tcp_input.c:6900`）：

1. TCP 标志和头部长度匹配 `tp->pred_flags`（没有 SYN/FIN/RST/URG，窗口没变）
2. 序列号 `== rcv_nxt`（数据按序到达，没有乱序）
3. ACK 号 `<= snd_nxt`（合法 ACK）

快速路径快在：**跳过了 OOO 处理、PAWS 校验、窗口边界检查、完整状态机**。纯 ACK 直接走 `tcp_ack(sk, skb, 0)` → `tcp_data_snd_check()`；纯数据直接 `tcp_queue_rcv()` 入接收队列 + `tcp_data_ready()` 唤醒读者。慢速路径则经过 `tcp_validate_incoming()` → 完整 `tcp_ack(FLAG_SLOWPATH)` → `tcp_data_queue()`（含 OOO 红黑树处理）。

**为什么重要：** 在高频短报文场景（如 RPC），绝大部分包走快速路径，理解它意味着理解 TCP 的实际性能瓶颈在哪里。

---

### Q2. 内核如何区分重复 ACK、部分 ACK 和完全 ACK？

**答：** `tcp_ack()`（`tcp_input.c:4598`）通过标志位组合判断：

| ACK 类型 | 内核判断方式 |
|---------|-------------|
| **完全 ACK** | `ack > prior_snd_una` → 设 `FLAG_SND_UNA_ADVANCED`；`tcp_clean_rtx_queue()` 中 `fully_acked = true`，所有覆盖的 skb 从重传树移除 |
| **重复 ACK** | `ack == prior_snd_una`，且不满足 `FLAG_NOT_DUP`（无新数据确认、无窗口更新、无捎带数据）→ `num_dupack++` → 进入 `tcp_fastretrans_alert()` |
| **部分 ACK** | `ack > prior_snd_una` 但 `tcp_clean_rtx_queue()` 中 `fully_acked = false`（重传树队首 skb 未被完全确认）→ 在 Recovery 状态触发 `tcp_try_undo_partial()` |

**实战意义：** 调试丢包/重传时，`ss -ti` 中的 `retrans` 和 `sacked` 字段对应的就是这些标志。理解它们才能判断是真丢包还是乱序触发的误判。

---

### Q3. TCP 的乱序数据在内核中是怎么存储和回填的？

**答：** 乱序数据存在 `tp->out_of_order_queue`（红黑树，非链表），收到 `seq > rcv_nxt` 的数据时调用 `tcp_data_queue_ofo()`（`tcp_input.c:6138`）插入红黑树。

回填发生在 **下一个按序包到达时**：`tcp_data_queue()` 中 `seq == rcv_nxt` 分支在入队后检查 `!RB_EMPTY_ROOT(&tp->out_of_order_queue)`，若非空则调用 `tcp_ofo_queue()`（`tcp_input.c:5637`）将红黑树中 `seq <= rcv_nxt` 的节点依次移入 `sk_receive_queue`。

**关键细节：** `tcp_recvmsg()` 本身**只读 `sk_receive_queue`**，看不到 OOO 队列。应用层感知到的乱序延迟，本质是在等后续按序包到达后触发回填。

---

### Q4. `close()` 和 `shutdown(SHUT_WR)` 在内核中有什么本质区别？

**答：**

| 操作 | 内核函数 | 行为差异 |
|------|---------|---------|
| `shutdown(SHUT_WR)` | `tcp_shutdown()`（`tcp.c:3205`） | 只关发送方向：`tcp_close_state()` + `tcp_send_fin()`。不设 `SHUTDOWN_MASK` 两端关闭、不 orphan socket、不清接收队列、**socket 仍可 `recv()`** |
| `close()` | `__tcp_close()`（`tcp.c:3284`） | 设 `SHUTDOWN_MASK` 双向关闭 → 清空接收队列（统计未读字节）→ 有未读数据则**发 RST**（不走 FIN）→ 否则 `tcp_send_fin()` → orphan socket → 可能进 TIME_WAIT |

**实战陷阱：** 服务端不做 `recv()` 就 `close()`，接收缓冲区有未读数据时，内核发的是 **RST 而非 FIN**，对端看到的是 `ECONNRESET` 而不是优雅关闭。

---

### Q5. `SO_LINGER` 设为 `{1, 0}` 时内核到底做了什么？

**答：** `__tcp_close()`（`tcp.c:3326`）检测到 `SOCK_LINGER && sk_lingertime == 0`，调用 `sk->sk_prot->disconnect()`（即 `tcp_disconnect()`）。`tcp_disconnect()`（`tcp.c:3506`）对 ESTABLISHED/CLOSE_WAIT 等状态发送 **RST**（`tcp_send_active_reset()`），立即转 TCP_CLOSE。

**效果：** 不走四次挥手、不进 TIME_WAIT、立即释放端口。

**实战场景：** 压测工具常用 `SO_LINGER=0` 避免 TIME_WAIT 堆积。但生产环境慎用——RST 会导致对端接收缓冲区未读数据丢失。

---

### Q6. SACK 在内核中是怎么标记和处理的？

**答：** `tcp_sacktag_write_queue()`（`tcp_input.c:2269`）解析 ACK 包中的 SACK 选项，遍历重传红黑树（`tcp_rtx_queue`），对每个被 SACK 覆盖的 skb 调用 `tcp_sacktag_one()`（`tcp_input.c:1729`），设置 `TCPCB_SACKED_ACKED` 标志位 + 增加 `tp->sacked_out`。

累积 ACK 到来时，`tcp_clean_rtx_queue()` 释放已确认的 skb，但 SACK 标记的 skb 即使序列号更大也**不会被释放**，直到 cumulative ACK 覆盖它。

**DSACK 检测：** `tcp_check_dsack()` 识别重复 SACK，可触发 `tcp_try_undo_*()` 系列函数撤销错误的拥塞窗口减半。

---

### Q7. SYN Flood 时内核的防护机制是什么？SYN Cookie 编码了什么信息？

**答：** 当半连接队列满（`inet_csk_reqsk_queue_is_full(sk)`）时，`tcp_input.c:8296` 检查 `sysctl_tcp_syncookies`：

- `0`：直接 drop SYN
- `1`（默认）：队列满时启用 SYN Cookie
- `2`：始终使用 SYN Cookie

SYN Cookie 将 MSS、时间戳等信息**编码到 ISN**（`cookie_init_sequence()`），不分配 request_sock 结构。收到 ACK 时 `cookie_v4_check()`（`syncookies.c:342`）从 ISN 反解 MSS，创建完整连接。

**代价：** SYN Cookie 模式下丢失了 TCP 选项协商（WSCALE、SACK），除非对端支持时间戳选项做 fallback。

---

### Q8. TCP Keepalive 在内核中是如何实现的？为什么有时候检测不到断连？

**答：** `tcp_keepalive_timer()`（`tcp_timer.c:688`）通过 `sk->sk_timer` 定期触发：

1. 连接空闲超过 `keepalive_time`（默认 2 小时）→ 发 keepalive 探测包（`tcp_write_wakeup()`）
2. 每 `keepalive_intvl`（默认 75 秒）重发一次
3. 连续 `keepalive_probes`（默认 9 次）无响应 → 发 RST 断连

**检测不到断连的场景：**
- 有数据在途（`tp->packets_out > 0`）→ keepalive 不触发，由重传定时器接管
- 中间 NAT 设备的连接跟踪超时早于 keepalive 间隔 → 探测包被 NAT 丢弃
- 本地 `tcp_write_wakeup()` 因资源不足返回失败 → 探测间隔退化为 `TCP_RESOURCE_PROBE_INTERVAL`

---

## 第二部分：证明对 UDP 深度掌握的问题

### Q9. UDP `sendmsg()` 和 TCP `sendmsg()` 在内核中的本质区别是什么？

**答：**

| 维度 | TCP（`tcp_sendmsg_locked`） | UDP（`udp_sendmsg`） |
|------|---------------------------|---------------------|
| 数据模型 | 字节流：拷贝到 `sk_write_queue`，多次 `send()` 可能合并成一个 skb | 数据报：一次 `sendmsg()` 产生一个完整数据报 |
| 发送路径 | `tcp_push()` → `tcp_write_xmit()` → 六道关卡（cwnd/rwnd/Nagle/TSO/pacing/TSQ） | 非 cork：`ip_make_skb()` → `udp_send_skb()`，一步到 IP 层 |
| 重传机制 | skb clone 下发，原件留在 `tcp_rtx_queue` 等待 ACK | 无重传队列，发完即忘 |
| cork 机制 | Nagle/autocork/MSG_MORE 在 TCP 层实现 | `UDP_CORK` / `MSG_MORE` 通过 `ip_append_data()` 在 IP 层累积，`udp_push_pending_frames()` 一次性发送 |
| 锁模型 | `lock_sock()` 独占 | 非 cork 路径无需 `lock_sock()`（`ip_make_skb` 无锁快速路径） |

---

### Q10. UDP 接收缓冲区满时，数据包去哪了？应用层能感知到吗？

**答：** `__udp_enqueue_schedule_skb()`（`udp.c:1890`）检查 `sk_rmem_alloc > sk_rcvbuf`，超过则直接 `goto drop` → `atomic_inc(&sk->sk_drops)` → `kfree_skb(skb)`。**没有 ICMP 回复给发送方**，没有 TCP 那样的流控反馈。

应用层感知：
- `recvmsg()` 返回的数据**不连续**（但应用看不到具体丢了什么）
- `SO_RXQ_OVFL` 选项：开启后 `recvmsg()` 的 ancillary data 携带 `sk_drops` 计数
- `/proc/net/udp` 的 `drops` 列
- `ss -u` 或 `netstat -su` 的 `RcvbufErrors` / `InErrors` 计数

---

### Q11. UDP 的 `SO_REUSEPORT` 在内核中是怎么分发数据包的？

**答：** `lookup_reuseport()`（`udp.c:413`）对四元组做 `udp_ehashfn()` 哈希，然后 `reuseport_select_sock()`（`sock_reuseport.c:485`）按 `reciprocal_scale(hash, num_socks)` 选择 socket 索引。

**两种分发模式：**
1. **默认哈希**：`reuseport_select_sock_by_hash()`，同一流（四元组相同）始终落到同一 socket
2. **BPF 程序**：`BPF_PROG_TYPE_SK_REUSEPORT` 可自定义分发逻辑

**实战意义：** 多进程 UDP 服务器（如 DNS）用 `SO_REUSEPORT` 替代 `accept()` 模型，内核级负载均衡。但进程数变化时 hash 结果会变（不像 consistent hash），已有的流可能切换到其它 worker。

---

### Q12. UDP 和 TCP 的 demux（查找目标 socket）有什么区别？

**答：**

| 维度 | TCP | UDP |
|------|-----|-----|
| 查找键 | 完整四元组（`saddr, sport, daddr, dport`）在 `tcp_hashinfo` established 表 | `(daddr, dport)` 哈希 + 可选四元组精确匹配 |
| Early demux | 匹配所有 established 连接 | 只匹配 **connected** UDP socket（`__udp4_lib_demux_lookup`，`udp.c:2898`） |
| 多 socket 场景 | 一个四元组只对应一个 socket | 同一 `(addr, port)` 可有多个 socket（`SO_REUSEPORT`），需 `compute_score()` 选最佳匹配 |

---

## 第三部分：实际应用中的常见问题及内核根因

### 问题 1：TIME_WAIT 堆积导致无法建立新连接

**现象：** 高并发短连接场景，`ss -s` 显示大量 `TIME_WAIT`，`connect()` 返回 `EADDRNOTAVAIL`。

**内核根因：** `__tcp_close()` 调用 `tcp_time_wait()`（`tcp_minisocks.c:309`），创建 `inet_timewait_sock` 轻量结构占用四元组 60 秒。客户端临时端口范围（`ip_local_port_range`，默认 32768–60999）耗尽后 `inet_hash_connect()` 查找失败。

**解决方案层次：**
- 应用层：使用连接池、HTTP keepalive
- 内核参数：`net.ipv4.tcp_tw_reuse=1`（允许复用 TIME_WAIT 连接，仅客户端生效，需对端时间戳支持）
- 架构层：`SO_LINGER=0` 强制 RST（慎用）；扩大 `ip_local_port_range`

---

### 问题 2：服务端 `close()` 后对端收到 RST 而非 FIN

**现象：** 客户端 `recv()` 返回 `ECONNRESET`，抓包看到 RST。

**内核根因：** `__tcp_close()`（`tcp.c:3320`）在清空接收队列时统计未读数据量 `data_was_unread`，如果 > 0 则直接发 RST（不走 FIN）。常见于服务端处理完请求后未 drain 接收缓冲区就 `close()`。

**解决方案：** 在 `close()` 前 `recv()` 直到返回 0（对端 FIN）或错误；或者先 `shutdown(SHUT_WR)` 发 FIN，等对端关闭后再 `close()`。

---

### 问题 3：TCP Keepalive 失效，连接"假死"

**现象：** 服务端以为连接活着，但对端已断网；数据发送卡住。

**内核根因：** Keepalive 只在连接**完全空闲**时触发（`tp->packets_out == 0 && tcp_write_queue_empty`，`tcp_timer.c:735`）。如果应用层一直在发数据（即使对端已不可达），keepalive 不工作，由重传定时器接管。重传指数退避最长可达 `TCP_RTO_MAX`（120 秒），最多重传 `tcp_retries2`（默认 15 次）后才断连——**总共可能等待 15+ 分钟**。

**解决方案：** 设置 `TCP_USER_TIMEOUT`（用户级超时），内核在 `tcp_retransmit_timer()` 和 `tcp_keepalive_timer()` 中均检查此值；应用层心跳机制作为补充。

---

### 问题 4：UDP 丢包但发送端无感知

**现象：** 发送端 `sendto()` 全部返回成功，但接收端只收到部分数据。

**内核根因：** UDP 丢包可能发生在多个层次：
1. **接收端 socket 缓冲区满**：`__udp_enqueue_schedule_skb()` 中 `sk_rmem_alloc > sk_rcvbuf` → 静默丢弃，`sk_drops++`
2. **发送端 Qdisc 队列满**：`__dev_xmit_skb()` 中 `q->enqueue()` 返回 `NET_XMIT_DROP`
3. **网卡 TX ring 满**：`ndo_start_xmit()` 返回 `NETDEV_TX_BUSY` → `dev_requeue_skb()` 或丢弃

`sendto()` 返回成功只代表数据进入了**发送缓冲区/IP 层**，不代表到达对端甚至不代表到达网卡。

**解决方案：** 增大 `SO_RCVBUF` / `net.core.rmem_max`；应用层序号 + 重传；使用 QUIC（用户态可靠 UDP）。

---

### 问题 5：高并发下 TCP 内存压力导致连接被 RST

**现象：** 日志出现 `out of memory -- consider tuning tcp_mem`，连接被 RST 断开。

**内核根因：** `tcp_check_oom()`（`tcp.c:3245`）检查两个条件：
1. `tcp_too_many_orphans()`：orphan 连接数（FIN_WAIT/TIME_WAIT/LAST_ACK 等）超过 `sysctl_tcp_max_orphans`
2. `tcp_out_of_memory()`：`sk_wmem_queued > SOCK_MIN_SNDBUF && sk_memory_allocated > sk_prot_mem_limits[2]`

任一条件满足 → `tcp_send_active_reset()` 强制 RST + `TCPABORTONMEMORY` 统计。

**解决方案：** 调大 `net.ipv4.tcp_mem`（三个阈值：low/pressure/high，单位 page）；调大 `tcp_max_orphans`；优化应用及时 `close()` 减少 orphan。

---

### 问题 6：SYN Flood 攻击下新连接建立失败

**现象：** 正常客户端 `connect()` 超时，`netstat -s` 显示 `SYNs to LISTEN sockets dropped`。

**内核根因：** 半连接队列（`reqsk_queue`）满 + `sysctl_tcp_syncookies=0` → 直接 drop SYN。即使 `syncookies=1`，Cookie 模式下丢失 WSCALE/SACK 协商，可能导致性能下降。

**解决方案：** 确保 `net.ipv4.tcp_syncookies=1`；调大 `net.ipv4.tcp_max_syn_backlog`；调大 `listen()` 的 backlog 参数；考虑 SYN proxy（硬件/中间件）。

---

### 问题 7：多进程 UDP 服务惊群

**现象：** 多个 worker 进程 `recvfrom()` 同一 socket，但负载不均。

**内核根因：** 如果不用 `SO_REUSEPORT`，多进程共享一个 socket 的 `sk_receive_queue`，每次只唤醒一个 waiter（`WQ_FLAG_EXCLUSIVE`），但存在竞争和不均衡。

**用 `SO_REUSEPORT` 后：** 每个进程 `bind()` 独立 socket，内核在 `__udp4_lib_rcv()` 路径直接按哈希选择 socket，**收包时直接入目标 socket 的队列**，无竞争。但进程增减导致 hash slot 重新分配，已有流可能漂移。

**解决方案：** `SO_REUSEPORT` + `BPF_PROG_TYPE_SK_REUSEPORT` 自定义分发逻辑（如 consistent hash）。

---

### 问题 8：TCP 自动调优未生效，发送/接收性能差

**现象：** 长肥管道（高带宽高延迟链路）TCP 吞吐低于预期。

**内核根因：** TCP 自动调优（`tcp_rcv_space_adjust()` / `tcp_sndbuf_expand()`，`tcp_input.c:1013/663`）被以下因素阻止：
1. 应用层调了 `SO_RCVBUF` / `SO_SNDBUF` → `sk_userlocks` 被设置 → 内核跳过自动调优
2. `sysctl_tcp_rmem[2]` / `sysctl_tcp_wmem[2]` 上限太低 → 自动调优有天花板
3. `tcp_moderate_rcvbuf=0` → 接收窗口自适应关闭
4. 全局 `tcp_mem` 压力阈值触发 → `tcp_under_memory_pressure()` 返回 true → `tcp_grow_window()` 跳过窗口增长

**解决方案：** 不要手动设 `SO_RCVBUF`/`SO_SNDBUF`（除非明确知道需要）；调大 `sysctl_tcp_rmem[2]` / `sysctl_tcp_wmem[2]`；调大 `tcp_mem` 上限。

---

## 第四部分：快速自检清单

以下问题如果能**脱离文档**回答出关键源码函数名和行为，说明对 TCP/UDP 内核实现有深度掌握：

| # | 问题 | 关键函数 |
|---|------|---------|
| 1 | 快速路径的三个条件是什么？ | `tcp_rcv_established`：pred_flags + seq==rcv_nxt + ack<=snd_nxt |
| 2 | 重复 ACK 是怎么判断的？ | `tcp_ack`：ack==prior_snd_una 且无 FLAG_NOT_DUP |
| 3 | 乱序数据存在什么数据结构中？ | `tp->out_of_order_queue`（红黑树），`tcp_data_queue_ofo()` 插入 |
| 4 | close() 什么情况下发 RST 而不是 FIN？ | `__tcp_close`：接收队列有未读数据 / SO_LINGER=0 |
| 5 | SACK 标记存在哪个字段？ | `TCP_SKB_CB(skb)->sacked` 的 `TCPCB_SACKED_ACKED` 位 |
| 6 | SYN Cookie 编码在哪？ | ISN（`cookie_init_sequence()`），反解在 `cookie_v4_check()` |
| 7 | Keepalive 不触发的条件是什么？ | `packets_out > 0` 或 `!tcp_write_queue_empty` |
| 8 | UDP sendmsg 的无锁快速路径？ | `ip_make_skb()`（非 cork 路径，不需要 lock_sock） |
| 9 | UDP 收包缓冲区满时内核做什么？ | `__udp_enqueue_schedule_skb`：sk_rmem_alloc > sk_rcvbuf → sk_drops++, kfree_skb |
| 10 | SO_REUSEPORT 默认分发算法？ | `reuseport_select_sock_by_hash`：4-tuple hash → reciprocal_scale 取模 |
| 11 | TCP/UDP early demux 的区别？ | TCP 匹配所有 established；UDP 只匹配 connected socket |
| 12 | orphan 连接过多时内核怎么处理？ | `tcp_check_oom` → `tcp_send_active_reset` 发 RST |
| 13 | tcp_write_xmit 从哪里被调用？ | 五种机制：syscall push / ACK 触发 / TSQ tasklet / 定时器 / release_sock |
| 14 | UDP 的 ICMP port unreachable 对应什么 errno？ | `ECONNREFUSED`（hard error），`__udp4_lib_err` |
| 15 | TCP 内存压力的全局标志在哪？ | `tcp_memory_pressure`（原子变量），`tcp_enter_memory_pressure()` 设置 |
