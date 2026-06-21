+++
date = '2026-04-16'
title = 'Linux内核UDP发送路径完整注释总结'
weight = 10
tags = [
    "UDP",
    "udp_sendmsg",
    "ip_make_skb",
    "发送路径",
]
categories = [
    "网络",
]
+++
# Linux内核UDP发送路径完整注释总结

本文档记录了Linux内核中UDP数据包从应用层sendto()调用到网卡驱动发送的完整路径。

## UDP与TCP发送的主要区别

| 特性 | TCP | UDP |
|------|-----|-----|
| 连接性 | 面向连接 | 无连接 |
| 可靠性 | 可靠传输（重传、确认） | 不可靠（尽力而为） |
| 拥塞控制 | 有（cwnd、rwnd等） | 无 |
| 流量控制 | 有 | 无 |
| 头部大小 | 20-60字节 | 8字节 |
| 发送复杂度 | 高 | 低 |
| 适用场景 | 文件传输、网页 | 视频、语音、DNS |

## UDP发送流程概览

```
应用层 sendto()/sendmsg()
    ↓
[1] UDP层: udp_sendmsg()
    ↓
[2] 地址验证: 获取目的地址和端口
    ↓
[3] 路由查找: ip_route_output_flow()
    ↓
[4] 两种发送路径:
    路径A: 快速路径（无锁）
      → ip_make_skb() (构建完整skb)
      → udp_send_skb()
    路径B: Cork路径（有锁）
      → ip_append_data() (追加数据)
      → udp_push_pending_frames()
      → udp_send_skb()
    ↓
[5] UDP头部: udp_send_skb() 构建8字节UDP头
    ↓
[6] UDP校验和: 计算校验和（可选）
    ↓
[7] IP层: ip_send_skb() → ip_local_out()
    ↓
[8] Netfilter: OUTPUT链过滤
    ↓
[9] IP输出: ip_output() → ip_finish_output()
    ↓
[10] 邻居子系统: neigh_output() (ARP解析)
    ↓
[11] 设备层: dev_queue_xmit() (与TCP共享)
    ↓
[12] 驱动层: ndo_start_xmit() (与TCP共享)
    ↓
硬件: DMA → 网卡 → 物理网络
```

## 层级划分与函数对照表

| 层级 | 源文件 | 主要函数 | 功能说明 |
|------|--------|----------|----------|
| **第1层：系统调用层** | net/socket.c | `__sys_sendto()` | 系统调用入口，用户态到内核态的入口 |
| **第2层：BSD Socket层** | net/socket.c | `sock_sendmsg()` | 执行LSM安全检查 |
| | | `sock_sendmsg_nosec()` | 调用协议族的sendmsg |
| **第3层：INET协议族层** | net/ipv4/af_inet.c | `inet_sendmsg()` | AF_INET发送入口，分发到TCP/UDP |
| **第4层：UDP传输层** | net/ipv4/udp.c | `udp_sendmsg()` | UDP发送主入口（1236行） |
| | | `udp_send_skb()` | 构建UDP头部，发送到IP层（966行） |
| | | `udp_push_pending_frames()` | 发送Cork累积的数据（1070行） |
| | | `udp_cmsg_send()` | 处理UDP控制消息 |
| | | `udp4_hwcsum()` | 硬件校验和设置 |
| **第5层：IP网络层** | net/ipv4/ip_output.c | `ip_make_skb()` | 快速路径构建skb（2174行） |
| | | `ip_append_data()` | Cork路径追加数据 |
| | | `ip_finish_skb()` | 完成skb构建 |
| | | `ip_send_skb()` | UDP到IP层的桥梁（2106行） |
| | | `ip_local_out()` | IP层发送入口（148行） |
| | | `__ip_local_out()` | 填充IP长度、计算校验和（113行） |
| | | `ip_output()` | 通用IP输出，通过POST_ROUTING钩子 |
| | | `ip_finish_output()` | IP输出完成，处理GSO |
| | | `ip_finish_output2()` | 进入邻居子系统 |
| | net/ipv4/route.c | `ip_route_output_flow()` | 路由查找 |
| | net/netfilter/ | `nf_hook()` | Netfilter钩子（LOCAL_OUT、POST_ROUTING） |
| **第6层：邻居子系统** | include/net/neighbour.h | `neigh_output()` | 邻居输出入口（快速/慢速路径分发）|
| | | `neigh_hh_output()` | 快速路径：使用缓存的硬件头部 |
| | net/core/neighbour.c | `neigh_resolve_output()` | 慢速路径：需要ARP解析时 |
| | | `neigh_connected_output()` | 已连接但无缓存头时 |
| | include/linux/netdevice.h | `dev_hard_header()` | 构建链路层头部（以太网头）|
| | net/ipv4/arp.c | `arp_solicit()` | 发送ARP请求 |
| **第7层：设备层** | net/core/dev.c | `dev_queue_xmit()` | 设备发送入口 |
| | | `__dev_queue_xmit()` | 队列选择和流量控制 |
| | | `__dev_xmit_skb()` | 通过Qdisc发送 |
| | net/sched/sch_generic.c | `sch_direct_xmit()` | Qdisc直接发送 |
| **第8层：驱动层** | net/core/dev.c | `dev_hard_start_xmit()` | 硬件发送起始 |
| | | `xmit_one()` | 发送单个skb，tcpdump抓包点 |
| | include/linux/netdevice.h | `netdev_start_xmit()` | 调用驱动发送 |
| | | `__netdev_start_xmit()` | 调用ndo_start_xmit |
| | drivers/net/ethernet/* | `ndo_start_xmit()` | 驱动发送回调 |
| | (以e1000为例) | `e1000_xmit_frame()` | Intel e1000发送 |
| | | DMA映射 → 敲门铃 | 将skb映射到DMA并通知网卡 |
| **硬件层** | 网卡硬件 | DMA传输 → PHY发送 | 从内存读取数据，物理层发送 |

## 详细调用流程图

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    第1-3层：系统调用 → Socket → 协议族                      │
├─────────────────────────────────────────────────────────────────────────┤
│  用户态: sendto(sockfd, buf, len, flags, dest_addr, addrlen)            │
│     ↓                                                                    │
│  【第1层】__sys_sendto()           // 系统调用入口                         │
│     ↓                                                                    │
│  【第2层】sock_sendmsg()           // BSD Socket层，LSM安全检查            │
│     ↓     sock_sendmsg_nosec()                                          │
│  【第3层】inet_sendmsg()           // INET协议族，分发到TCP/UDP             │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│                         第4层：UDP传输层                                   │
├─────────────────────────────────────────────────────────────────────────┤
│  udp_sendmsg()  [net/ipv4/udp.c:1236]                                   │
│     │                                                                    │
│     ├─ 参数检查（长度、MSG_OOB）                                           │
│     ├─ 检查pending（Cork机制）                                            │
│     ├─ 获取目的地址（sendto或已connect）                                    │
│     ├─ 初始化ipc（控制消息cookie）                                         │
│     ├─ 处理控制消息（udp_cmsg_send, ip_cmsg_send）                         │
│     ├─ BPF程序处理                                                       │
│     ├─ 源路由处理                                                        │
│     ├─ 多播/广播处理                                                      │
│     ├─ 路由查找 → ip_route_output_flow()                                 │
│     │                                                                    │
│     ├─[快速路径] !corkreq                                                │
│     │    ip_make_skb() → udp_send_skb()                                 │
│     │                                                                    │
│     └─[Cork路径] corkreq                                                 │
│          lock_sock() → ip_append_data()                                 │
│          → udp_push_pending_frames() → ip_finish_skb()                  │
│          → udp_send_skb() → release_sock()                              │
│                                                                          │
│  udp_send_skb()  [net/ipv4/udp.c:966]                                   │
│     ├─ 构建UDP头部（源端口、目的端口、长度、校验和）                           │
│     ├─ 处理GSO（如果启用）                                                │
│     ├─ 计算校验和（UDP-Lite/禁用/硬件/软件）                                │
│     └─ ip_send_skb()                                                    │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│                         第5层：IP网络层                                    │
├─────────────────────────────────────────────────────────────────────────┤
│  ip_send_skb()  [net/ipv4/ip_output.c:2106]                             │
│     └─ ip_local_out()                                                   │
│                                                                          │
│  ip_local_out()  [net/ipv4/ip_output.c:148]                             │
│     └─ __ip_local_out()                                                 │
│                                                                          │
│  __ip_local_out()  [net/ipv4/ip_output.c:113]                            │
│     ├─ iph->tot_len = htons(skb->len)  // 填充IP长度                     │
│     ├─ ip_send_check(iph)               // 计算IP校验和                   │
│     ├─ skb->protocol = htons(ETH_P_IP)                                  │
│     └─ nf_hook(NF_INET_LOCAL_OUT, dst_output)  // Netfilter LOCAL_OUT   │
│                                                                          │
│  dst_output() → ip_output()                                             │
│     └─ nf_hook(NF_INET_POST_ROUTING, ip_finish_output)  // POST_ROUTING │
│                                                                          │
│  ip_finish_output() → ip_finish_output2()                               │
│     ├─ 检查skb headroom是否足够放以太网头                                  │
│     ├─ 获取邻居条目 ip_neigh_for_gw()                                    │
│     └─ neigh_output(neigh, skb)  // 进入邻居子系统                        │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│                         第6层：邻居子系统                                   │
├─────────────────────────────────────────────────────────────────────────┤
│  neigh_output()  [include/net/neighbour.h]                              │
│     │                                                                    │
│     ├─[快速路径] NUD_CONNECTED && hh_len有效                             │
│     │    neigh_hh_output(hh, skb)  // 直接使用缓存的以太网头              │                                       │
│     │       ├─ 拷贝缓存的以太网头到skb                                    │
│     │       └─ dev_queue_xmit(skb)  // 进入设备层                        │
│     │                                                                    │
│     └─[慢速路径] n->output()                                             │
│          neigh_resolve_output()  // 需要ARP解析                          │
│             ├─ neigh_event_send() 检查邻居状态，必要时发ARP                │
│             ├─ dev_hard_header() 填充以太网头                            │
│             └─ dev_queue_xmit(skb)  // 进入设备层                        │
│          neigh_connected_output()  // 已连接但无缓存头                    │
│             ├─ dev_hard_header() 填充以太网头                            │
│             └─ dev_queue_xmit(skb)  // 进入设备层                        │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│                         第7层：设备层                                      │
├─────────────────────────────────────────────────────────────────────────┤
│  dev_queue_xmit()  [net/core/dev.c]                                     │
│     └─ __dev_queue_xmit()                                               │
│                                                                          │
│  __dev_queue_xmit()                                                      │
│     ├─ netdev_core_pick_tx()  // 选择发送队列（多队列网卡）                 │
│     ├─ 获取Qdisc（流量控制队列规则）                                        │
│     │                                                                    │
│     ├─[有Qdisc] __dev_xmit_skb()                                        │
│     │    ├─ q->enqueue()  // 入队                                       │
│     │    └─ __qdisc_run() / sch_direct_xmit()  // 调度发送              │
│     │                                                                    │
│     └─[无Qdisc/直接发送] dev_hard_start_xmit()                          │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│                         第8层：驱动层                                      │
├─────────────────────────────────────────────────────────────────────────┤
│  dev_hard_start_xmit()  [net/core/dev.c]                                │
│     └─ xmit_one()                                                       │
│                                                                          │
│  xmit_one()                                                              │
│     ├─ dev_queue_xmit_nit()  // ★抓包点（tcpdump、wireshark）            │
│     └─ netdev_start_xmit()                                              │
│                                                                          │
│  netdev_start_xmit()  [include/linux/netdevice.h]                       │
│     └─ __netdev_start_xmit()                                            │
│         └─ ops->ndo_start_xmit(skb, dev)  // ★调用驱动发送函数            │
│                                                                          │
│  [以e1000为例: drivers/net/ethernet/intel/e1000/]                       │
│  e1000_xmit_frame()                                                      │
│     ├─ 检查发送队列是否有空间                                              │
│     ├─ 处理TSO/GSO（如果硬件支持）                                         │
│     ├─ 设置发送描述符（TX descriptor）                                     │
│     ├─ dma_map_single()  // DMA映射skb数据                              │
│     ├─ 更新TX ring的tail指针                                             │
│     └─ writel()  // 写寄存器通知网卡（敲门铃）                             │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│                         硬件层：网卡物理发送                                │
├─────────────────────────────────────────────────────────────────────────┤
│  网卡硬件执行：                                                           │
│     ├─ 读取TX描述符                                                      │
│     ├─ DMA读取数据包内容（从内存到网卡）                                    │
│     ├─ 计算校验和（如果启用硬件卸载）                                        │
│     ├─ 添加FCS（帧校验序列，4字节CRC）                                      │
│     ├─ 通过PHY发送到物理介质（以太网线）                                     │
│     ├─ 发送完成后更新描述符状态（设置DD位）                                  │
│     └─ 触发TX完成中断                                                     │
└─────────────────────────────────────────────────────────────────────────┘
                                   ↓
┌─────────────────────────────────────────────────────────────────────────┐
│                     第9层：发送完成通知（skb释放和唤醒）                      │
├─────────────────────────────────────────────────────────────────────────┤
│  e1000_intr()  [硬中断]                                                  │
│     └─ __napi_schedule()                                                │
│         └─ raise NET_RX_SOFTIRQ  // 注意：用RX软中断处理TX完成             │
│                                                                          │
│  net_rx_action()  [NET_RX_SOFTIRQ]                                       │
│     └─ napi_poll()                                                       │
│         └─ e1000_clean()  ← NAPI poll函数                               │
│             ├─ e1000_clean_tx_irq()  ← 处理TX完成                        │
│             │   ├─ 检查TX描述符DD位（Descriptor Done）                    │
│             │   ├─ dev_kfree_skb_any(skb)  → 释放skb                    │
│             │   │   └─ __kfree_skb()                                    │
│             │   │       └─ skb_release_head_state()                     │
│             │   │           └─ skb->destructor()  即 sock_wfree()       │
│             │   │               ├─ 减少sk_wmem_alloc                    │
│             │   │               └─ sk->sk_write_space()                 │
│             │   │                   └─ sock_def_write_space()           │
│             │   │                       └─ wake_up() ★唤醒阻塞进程      │
│             │   └─ netif_wake_queue()  → 唤醒发送队列（如果之前停止）      │
│             │       └─ __netif_schedule()                               │
│             │           └─ raise NET_TX_SOFTIRQ                         │
│             └─ e1000_clean_rx_irq()  ← 处理RX接收                        │
│                                                                          │
│  net_tx_action()  [NET_TX_SOFTIRQ] (如果被触发)                          │
│     ├─ 处理completion_queue: 延迟释放的skb                               │
│     └─ 处理output_queue: 继续发送Qdisc中的数据包                          │
│         └─ qdisc_run()                                                   │
└─────────────────────────────────────────────────────────────────────────┘
```

## 各层函数速查表

### 快速路径函数调用序列（按层级）
```
【第1层-系统调用】sendto() → __sys_sendto()
【第2层-Socket层】→ sock_sendmsg() → sock_sendmsg_nosec()
【第3层-协议族层】→ inet_sendmsg()
【第4层-传输层】  → udp_sendmsg() → ip_make_skb() → udp_send_skb()
【第5层-网络层】  → ip_send_skb() → ip_local_out() → __ip_local_out()
                 → nf_hook(LOCAL_OUT) → dst_output() → ip_output()
                 → nf_hook(POST_ROUTING) → ip_finish_output() → ip_finish_output2()
【第6层-邻居层】  → neigh_output() → neigh_hh_output()
【第7层-设备层】  → dev_queue_xmit() → __dev_queue_xmit() → __dev_xmit_skb()
【第8层-驱动层】  → dev_hard_start_xmit() → xmit_one() → netdev_start_xmit()
                 → __netdev_start_xmit() → ndo_start_xmit()
```

### Cork路径函数调用序列
```
# 第一次sendto(MSG_MORE) - 数据累积
【第1-3层】sendto(MSG_MORE) → ... → inet_sendmsg()
【第4层】  → udp_sendmsg() → lock_sock() → ip_append_data() → release_sock()

# 最后一次sendto(无MSG_MORE) - 发送
【第1-3层】sendto() → ... → inet_sendmsg()
【第4层】  → udp_sendmsg() → lock_sock() → ip_append_data()
          → udp_push_pending_frames() → ip_finish_skb() → udp_send_skb()
【第5-8层】→ [与快速路径相同]
```

## 各层详细说明

### 1. UDP传输层 (net/ipv4/udp.c)

#### udp_sendmsg()
- **位置**: net/ipv4/udp.c:1236
- **功能**: UDP发送的主入口函数，由应用层sendto()/sendmsg()触发
- **完整调用链**:
```
用户态sendto() -> SYSCALL(sendto) -> sock_sendmsg() -> udp_sendmsg()
-> [快速路径] ip_make_skb() + udp_send_skb()
   [cork路径] ip_append_data() + udp_push_pending_frames()
-> ip_send_skb() -> ip_local_out() -> __ip_local_out()
-> nf_hook(NF_INET_LOCAL_OUT) -> dst_output() -> ip_output()
-> nf_hook(NF_INET_POST_ROUTING) -> ip_finish_output() -> ip_finish_output2()
-> neigh_output() -> neigh_hh_output() -> dev_queue_xmit()
-> __dev_queue_xmit() -> __dev_xmit_skb() 或直接 dev_hard_start_xmit()
-> xmit_one() -> netdev_start_xmit() -> __netdev_start_xmit()
-> ops->ndo_start_xmit() (例如 e1000_xmit_frame)
-> 网卡DMA传输 -> 物理发送
```
- **主要流程**:

##### 步骤1: 基本参数检查和初始化
```c
- 检查数据长度（最大64KB-1，因IP头16位长度字段限制）
- 检查MSG_OOB标志（UDP不支持带外数据，返回-EOPNOTSUPP）
- 选择数据拷贝函数: is_udplite ? udplite_getfrag : ip_generic_getfrag
- 计算corkreq: READ_ONCE(up->corkflag) || msg->msg_flags & MSG_MORE
```

##### 步骤2: 检查pending数据（Cork机制）
```c
- UDP cork允许累积多次sendmsg的数据
- Double-checked locking优化：
  * 第一次检查（无锁）：up->pending非0则进入
  * lock_sock(sk)加锁
  * 第二次检查（加锁）：确认up->pending仍为真
  * 验证pending == AF_INET（否则返回-EINVAL）
- 如有pending数据，直接跳转到do_append_data追加
- 若第二次检查pending为0，release_sock(sk)后继续
```

##### 步骤3: 获取目的地址
```c
- 添加UDP头部长度: ulen += sizeof(struct udphdr) // 8字节

两种方式获取目的地址：
方式1: 从msg获取（sendto场景）
  - 验证msg_namelen >= sizeof(struct sockaddr_in)
  - 验证地址族sin_family == AF_INET或AF_UNSPEC
  - 提取IP: daddr = usin->sin_addr.s_addr
  - 提取端口: dport = usin->sin_port（不能为0）

方式2: 从socket获取（已connect的send场景）
  - 检查sk->sk_state == TCP_ESTABLISHED（UDP也用此状态表示已连接）
  - 获取缓存的地址: daddr = inet->inet_daddr
  - 获取缓存的端口: dport = inet->inet_dport
  - 标记connected = 1（可使用缓存路由）
```

##### 步骤4: 初始化IP控制消息cookie
```c
- 调用ipcm_init_sk(&ipc, inet)初始化：
  * 源地址 ipc.addr
  * 输出设备索引 ipc.oif
  * 时间戳选项等
- 设置GSO大小: ipc.gso_size = READ_ONCE(up->gso_size)
```

##### 步骤5: 处理控制消息（辅助数据）
```c
- 如果msg->msg_controllen非0：
  * 调用udp_cmsg_send()处理UDP特定消息（UDP_SEGMENT）
  * 调用ip_cmsg_send()处理IP层消息（IP_PKTINFO、IP_TTL、IP_TOS等）
  * 如果分配了IP选项，标记free = 1
  * 有控制消息时，connected = 0（需要重新路由）
```

##### 步骤6: 获取IP选项
```c
- 如果控制消息中没有IP选项(!ipc.opt)：
  * rcu_read_lock()
  * inet_opt = rcu_dereference(inet->inet_opt)
  * 拷贝IP选项到本地opt_copy（因为要在RCU锁外使用）
  * rcu_read_unlock()
```

##### 步骤7: BPF程序处理
```c
- 条件: cgroup_bpf_enabled(CGROUP_UDP4_SENDMSG) && !connected
- 调用BPF_CGROUP_RUN_PROG_UDP4_SENDMSG_LOCK()
- eBPF程序可以修改目的地址和端口
- BPF修改后需要重新验证dport != 0
```

##### 步骤8: 源路由选项处理
```c
- 条件: ipc.opt && ipc.opt->opt.srr
- IP支持两种源路由：
  * 严格源路由(SSR): 必须严格按指定路径
  * 松散源路由(LSR): 允许经过其他路由器
- 提取第一跳地址: faddr = ipc.opt->opt.faddr
- 标记connected = 0（需要重新路由）
```

##### 步骤9: 本地路由标志检查
```c
- 获取TOS: tos = get_rttos(&ipc, inet)
- 以下情况设置RTO_ONLINK（目的地直连）：
  * sock_flag(sk, SOCK_LOCALROUTE)
  * msg->msg_flags & MSG_DONTROUTE
  * ipc.opt && ipc.opt->opt.is_strictroute
- 上述情况connected = 0
```

##### 步骤10: 多播和广播地址处理
```c
- 多播地址(224.0.0.0 ~ 239.255.255.255):
  * 设置多播设备索引: ipc.oif = inet->mc_index
  * 设置多播源地址: saddr = inet->mc_addr
  * connected = 0
- 单播地址:
  * 使用单播设备索引: ipc.oif = inet->uc_index
- 本地广播(255.255.255.255):
  * 检查L3 master/slave设备关系
```

##### 步骤11: 路由查找
```c
- 已连接socket检查缓存路由: rt = sk_dst_check(sk, 0)
- 如无有效路由，执行路由查找：
  * 初始化流信息: flowi4_init_output()
  * 安全分类: security_sk_classify_flow()
  * 路由查找: rt = ip_route_output_flow(net, fl4, sk)
  * 检查广播权限: (rt->rt_flags & RTCF_BROADCAST) && !sock_flag(sk, SOCK_BROADCAST)
  * 缓存路由: if(connected) sk_dst_set(sk, dst_clone(&rt->dst))
```

##### 步骤12: MSG_CONFIRM处理
```c
- 条件: msg->msg_flags & MSG_CONFIRM
- 跳转到do_confirm标签：
  * MSG_PROBE时: dst_confirm_neigh()仅探测路径
  * 否则: 更新ARP缓存时间戳后继续发送
```

##### 步骤13: 数据发送路径选择

**快速路径（Lockless Fast Path）**
```c
条件: !corkreq (即 !up->corkflag && !(msg->msg_flags & MSG_MORE))
特点:
  1. 无锁操作（高性能）
  2. 使用栈上的struct inet_cork cork（不是socket上的）
  3. 一次性构建skb并立即发送
  4. 适合单个大包或不需要累积的场景

流程:
  skb = ip_make_skb(sk, fl4, getfrag, msg, ulen,
                    sizeof(struct udphdr), &ipc, &rt, &cork, msg->msg_flags)
    ↓  // 构建完整skb: [headroom预留IP头][UDP头8字节][用户数据]
  udp_send_skb(skb, fl4, &cork)   // 添加UDP头并发送
    ↓
  goto out  // 释放资源
```

**Cork路径（需要加锁）**
```c
条件: corkreq (即 up->corkflag || msg->msg_flags & MSG_MORE)
特点:
  1. 需要lock_sock(sk)保护共享状态
  2. 使用socket上的inet->cork（持久存储）
  3. 可累积多次调用的数据
  4. 适合多个小包合并发送

流程:
  lock_sock(sk)
    ↓
  // 再次检查pending（防止并发socket使用）
  if(unlikely(up->pending)) { release_sock(); return -EINVAL; }
    ↓
  // 初始化cork流信息
  fl4 = &inet->cork.fl.u.ip4;
  fl4->daddr = daddr; fl4->saddr = saddr;
  fl4->fl4_dport = dport; fl4->fl4_sport = inet->inet_sport;
    ↓
  up->pending = AF_INET;  // 标记cork开始
    ↓
do_append_data:           // 追加数据标签（首次cork和后续追加都跳到这里）
  up->len += ulen;        // 累加数据长度
    ↓
  err = ip_append_data(sk, fl4, getfrag, msg, ulen,
                       sizeof(struct udphdr), &ipc, &rt,
                       corkreq ? msg->msg_flags|MSG_MORE : msg->msg_flags)
    ↓
  // 错误处理
  if(err) udp_flush_pending_frames(sk);  // 清空队列，复位pending
    ↓
  // 发送判断
  else if(!corkreq)
      err = udp_push_pending_frames(sk); // 立即发送
  else if(skb_queue_empty(&sk->sk_write_queue))
      up->pending = 0;                   // 队列为空，复位pending
    ↓
  release_sock(sk)
```

##### 错误处理和返回
```c
out:
  ip_rt_put(rt);              // 释放路由引用
out_free:
  if(free) kfree(ipc.opt);    // 释放动态分配的IP选项
  
  if(!err) return len;        // 成功返回发送字节数
  
  // 发送缓冲区错误统计
  if(err == -ENOBUFS || test_bit(SOCK_NOSPACE, &sk->sk_socket->flags))
      UDP_INC_STATS(sock_net(sk), UDP_MIB_SNDBUFERRORS, is_udplite);
  
  return err;
```

### 2. UDP数据包构建 (net/ipv4/udp.c)

#### udp_send_skb()
- **位置**: net/ipv4/udp.c:966
- **功能**: 构建UDP头部并发送到IP层
- **原型**: `static int udp_send_skb(struct sk_buff *skb, struct flowi4 *fl4, struct inet_cork *cork)`

##### UDP头部格式（8字节）
```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Source Port          |       Destination Port        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|            Length             |           Checksum            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                             Data                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

##### udp_send_skb详细流程
```c
1. 计算长度:
   int offset = skb_transport_offset(skb);  // 传输层偏移
   int len = skb->len - offset;             // UDP总长度 = 头部8字节 + 数据
   int datalen = len - sizeof(*uh);         // 纯数据长度

2. 构建UDP头部:
   uh = udp_hdr(skb);
   uh->source = inet->inet_sport;   // 源端口
   uh->dest = fl4->fl4_dport;       // 目的端口
   uh->len = htons(len);            // UDP长度（网络字节序）
   uh->check = 0;                   // 校验和先清零

3. 处理GSO（如果启用）

4. 计算校验和（见下文）

5. 发送到IP层:
   err = ip_send_skb(sock_net(sk), skb);

6. 错误处理和统计:
   if(err == -ENOBUFS && !inet->recverr)
       UDP_INC_STATS(..., UDP_MIB_SNDBUFERRORS, ...);
   else
       UDP_INC_STATS(..., UDP_MIB_OUTDATAGRAMS, ...);
```

##### UDP GSO (Generic Segmentation Offload)
```c
作用: 允许发送大于MTU的UDP包
原理: 延迟分段，由网卡或协议栈处理

条件检查 (cork->gso_size非0时):
  1. hlen + cork->gso_size <= cork->fragsize  // 头部+分段大小不超过MTU
  2. datalen <= cork->gso_size * UDP_MAX_SEGMENTS  // 总数据不超过最大段数
  3. !sk->sk_no_check_tx                        // 必须启用校验和
  4. skb->ip_summed == CHECKSUM_PARTIAL         // 必须是部分校验和
  5. !is_udplite                                // 不支持UDP-Lite
  6. !dst_xfrm(skb_dst(skb))                   // 不支持IPsec

参数设置 (datalen > cork->gso_size时):
  skb_shinfo(skb)->gso_size = cork->gso_size;   // 每段大小
  skb_shinfo(skb)->gso_type = SKB_GSO_UDP_L4;   // GSO类型
  skb_shinfo(skb)->gso_segs = DIV_ROUND_UP(datalen, cork->gso_size);  // 段数
```

##### UDP校验和计算
```c
四种模式（按优先级）：

1. UDP GSO模式
   - 跳转到csum_partial标签
   - 使用udp4_hwcsum()设置硬件校验和参数

2. UDP-Lite模式 (is_udplite为真)
   - csum = udplite_csum(skb)
   - 部分校验和，用于容错传输

3. 禁用校验和模式 (sk->sk_no_check_tx)
   - skb->ip_summed = CHECKSUM_NONE
   - 直接跳转到send标签
   - 仅用于可信局域网，不推荐使用

4. 硬件校验和模式 (skb->ip_summed == CHECKSUM_PARTIAL)
   - 调用udp4_hwcsum(skb, fl4->saddr, fl4->daddr)
   - 设置参数让网卡计算，最高效

5. 软件校验和模式 (默认)
   - csum = udp_csum(skb)                       // 计算数据校验和
   - uh->check = csum_tcpudp_magic(             // 添加伪头部
         fl4->saddr, fl4->daddr, len, sk->sk_protocol, csum)
   - if(uh->check == 0) uh->check = CSUM_MANGLED_0  // 0要替换为0xFFFF
```

#### udp_push_pending_frames()
- **位置**: net/ipv4/udp.c:1070
- **功能**: 发送Cork模式下累积的所有数据
- **原型**: `int udp_push_pending_frames(struct sock *sk)`

```c
流程:
1. skb = ip_finish_skb(sk, fl4);   // 完成skb构建，从sk->sk_write_queue取出
2. if(!skb) goto out;              // 无数据则返回
3. err = udp_send_skb(skb, fl4, &inet->cork.base);  // 发送
4. out:
   up->len = 0;                    // 清空累积长度
   up->pending = 0;                // 清除pending标记
   return err;
```

#### udp_flush_pending_frames()
- **功能**: 清空Cork模式下的所有pending数据（出错时调用）
- 调用`ip_flush_pending_frames(sk)`清空sk->sk_write_queue
- 复位up->len和up->pending

#### ip_make_skb() (快速路径关键函数)
- **位置**: net/ipv4/ip_output.c:2174
- **功能**: 一次性构建完整的skb（用于快速路径）
- **原型**: `struct sk_buff *ip_make_skb(struct sock *sk, struct flowi4 *fl4, int (*getfrag)(...), void *from, int length, int transhdrlen, struct ipcm_cookie *ipc, struct rtable **rtp, struct inet_cork *cork, unsigned int flags)`

```c
struct sk_buff *ip_make_skb(...)
{
    struct sk_buff_head queue;
    
    if (flags & MSG_PROBE)
        return NULL;                    // 仅探测，不实际发送
    
    __skb_queue_head_init(&queue);      // 初始化临时skb队列
    
    cork->flags = 0; cork->addr = 0; cork->opt = NULL;
    
    // 步骤1: 设置cork（MTU、路由、IP选项等）
    err = ip_setup_cork(sk, cork, ipc, rtp);
    
    // 步骤2: 分配skb并拷贝用户数据，可能产生多个分片skb
    err = __ip_append_data(sk, fl4, &queue, cork,
                           &current->task_frag, getfrag,
                           from, length, transhdrlen, flags);
    
    // 步骤3: 填充IP头，合并所有分片为一个skb
    return __ip_make_skb(sk, fl4, &queue, cork);
}
```

**快速路径与Cork路径的skb构建区别**:
```
快速路径:
  ip_make_skb() 使用栈上的struct inet_cork和临时队列
    ↓
  ip_setup_cork() + __ip_append_data() + __ip_make_skb()
    ↓
  一次性完成，无需锁保护

Cork路径:
  ip_append_data() 使用socket上的inet->cork和sk->sk_write_queue
    ↓
  多次调用累积数据
    ↓
  ip_finish_skb() (内部调用__ip_make_skb)完成构建
    ↓
  需要lock_sock()保护
```

### 3. IP层发送 (net/ipv4/ip_output.c)

#### ip_send_skb()
- **位置**: net/ipv4/ip_output.c:2106
- **功能**: UDP/ICMP等无连接协议发送数据包的统一入口
- **原型**: `int ip_send_skb(struct net *net, struct sk_buff *skb)`

```c
int ip_send_skb(struct net *net, struct sk_buff *skb)
{
    int err;
    err = ip_local_out(net, skb->sk, skb);  // 启动IP层发送
    if (err) {
        if (err > 0)
            err = net_xmit_errno(err);      // 转换Qdisc返回码
        if (err)
            IP_INC_STATS(net, IPSTATS_MIB_OUTDISCARDS);  // 丢弃统计
    }
    return err;
}
```

与TCP的ip_queue_xmit()的区别：
```
ip_queue_xmit (TCP使用):
  - 需要构建IP头部
  - 需要路由查找
  - 处理IP选项
  - 选择IP ID
  - 复杂度高

ip_send_skb (UDP使用):
  - IP头已由ip_make_skb()或ip_append_data()构建完成
  - 路由已在udp_sendmsg()中查找
  - 直接发送
  - 更简单高效
```

#### ip_local_out()
- **位置**: net/ipv4/ip_output.c:148
- **功能**: IP层向外发送数据包的入口点

```c
int ip_local_out(struct net *net, struct sock *sk, struct sk_buff *skb)
{
    int err;
    err = __ip_local_out(net, sk, skb);   // 设置协议类型、通过netfilter钩子
    if (likely(err == 1))                  // netfilter允许通过
        err = dst_output(net, sk, skb);    // 根据路由调用ip_output()
    return err;
}
```

#### __ip_local_out()
- **位置**: net/ipv4/ip_output.c:113
- **功能**: 填充IP头部长度、计算校验和、通过netfilter

```c
int __ip_local_out(struct net *net, struct sock *sk, struct sk_buff *skb)
{
    struct iphdr *iph = ip_hdr(skb);
    iph->tot_len = htons(skb->len);       // 填充IP报文长度
    ip_send_check(iph);                    // 计算IP校验和
    
    skb = l3mdev_ip_out(sk, skb);         // L3主设备处理
    skb->protocol = htons(ETH_P_IP);
    
    // 通过netfilter LOCAL_OUT钩子，回调函数是dst_output
    return nf_hook(NFPROTO_IPV4, NF_INET_LOCAL_OUT,
                   net, sk, skb, NULL, skb_dst(skb)->dev,
                   dst_output);
}
```

调用链详解：
```
ip_send_skb(net, skb)
  ↓
ip_local_out(net, sk, skb)
  ↓
__ip_local_out(net, sk, skb)
  - iph->tot_len = htons(skb->len)    // 设置IP头部长度
  - ip_send_check(iph)                 // 计算IP校验和
  - skb->protocol = htons(ETH_P_IP)    // 设置协议类型
  ↓
nf_hook(NF_INET_LOCAL_OUT, ..., dst_output)  // Netfilter OUTPUT链
  ↓
dst_output(net, sk, skb)              // 根据路由类型调用输出函数
  ↓
ip_output(net, sk, skb)               // 通用IP输出
  ↓
nf_hook(NF_INET_POST_ROUTING, ..., ip_finish_output)  // POST_ROUTING链
  ↓
ip_finish_output(net, sk, skb)
  ↓
ip_finish_output2(net, sk, skb)       // 处理MTU和分片
  ↓
neigh_output(neigh, skb, hh_len)      // 邻居子系统（ARP解析）
  ↓
neigh_hh_output(hh, skb)              // 使用缓存的硬件头部
  ↓
dev_queue_xmit(skb)                   // 设备层发送
  ↓
网卡驱动 ndo_start_xmit()
```

### 4. 共享层（与TCP共享）

从`dev_queue_xmit()`开始，UDP和TCP共享相同的发送路径：

#### 设备层 (net/core/dev.c)
- **dev_queue_xmit()**: 网络设备发送入口
- **__dev_queue_xmit()**: 队列选择和流量控制
- **dev_hard_start_xmit()**: 硬件发送起始

#### 驱动层 (网卡驱动)
- **ndo_start_xmit()**: 驱动发送函数
- DMA映射
- 网卡通知（敲门铃）

详细注释请参考：`TCP_SEND_PATH_SUMMARY.md`

### 5. 发送完成通知 (TX Completion)

网卡发送完数据包后，需要通知内核释放skb并唤醒阻塞的进程。

#### 为什么需要发送完成通知？

1. **释放skb内存**: 数据包发送完成后，skb占用的内存需要释放
2. **更新发送缓冲区计数**: 减少`sk_wmem_alloc`，允许更多数据发送
3. **唤醒阻塞进程**: 如果send()因缓冲区满而阻塞，需要唤醒

#### 两个软中断的分工

```
┌─────────────────────────────────────────────────────────────────┐
│                    NET_RX_SOFTIRQ (net_rx_action)               │
├─────────────────────────────────────────────────────────────────┤
│  处理NAPI轮询，调用驱动的poll函数                                │
│  - e1000_clean() 同时处理 TX完成 + RX接收                        │
│  - 大部分现代驱动在这里释放已发送的skb                           │
│  注意：虽然名字是RX，但实际同时处理收发！                         │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                    NET_TX_SOFTIRQ (net_tx_action)               │
├─────────────────────────────────────────────────────────────────┤
│  1. completion_queue: 延迟释放硬中断中无法释放的skb              │
│  2. output_queue: 继续发送被netif_wake_queue唤醒的Qdisc          │
└─────────────────────────────────────────────────────────────────┘
```

#### e1000_clean_tx_irq() 核心流程

```c
// drivers/net/ethernet/intel/e1000/e1000_main.c
static bool e1000_clean_tx_irq(struct e1000_adapter *adapter,
                               struct e1000_tx_ring *tx_ring)
{
    // 遍历TX描述符环
    while ((eop_desc->upper.data & cpu_to_le32(E1000_TXD_STAT_DD))) {
        // E1000_TXD_STAT_DD: Descriptor Done，网卡设置此位表示发送完成
        
        // 释放skb: dev_kfree_skb_any() -> sock_wfree() -> wake_up()
        e1000_unmap_and_free_tx_resource(adapter, buffer_info);
    }
    
    // 如果TX队列之前满了，现在有空间了，唤醒队列
    if (netif_queue_stopped(netdev) && E1000_DESC_UNUSED(tx_ring) >= 32) {
        netif_wake_queue(netdev);  // -> 触发NET_TX_SOFTIRQ
    }
}
```

#### sock_wfree() - 唤醒阻塞进程的关键

```c
// net/core/sock.c:2223
void sock_wfree(struct sk_buff *skb)
{
    struct sock *sk = skb->sk;
    unsigned int len = skb->truesize;

    // 减少发送缓冲区使用量
    WARN_ON(refcount_sub_and_test(len - 1, &sk->sk_wmem_alloc));
    
    // 通知socket有空间可写 -> 唤醒阻塞的send()
    sk->sk_write_space(sk);  // 即 sock_def_write_space()
}

// net/core/sock.c:3122
static void sock_def_write_space(struct sock *sk)
{
    // 当发送缓冲区使用量 < 50% 时，唤醒等待的进程
    if ((refcount_read(&sk->sk_wmem_alloc) << 1) <= READ_ONCE(sk->sk_sndbuf)) {
        wake_up_interruptible_sync_poll(&wq->wait, EPOLLOUT);
    }
}
```

#### 完整调用链

```
网卡发送完成，设置TX描述符DD位，触发中断
    │
    ▼
e1000_intr() [硬中断]
    └─> __napi_schedule() → raise NET_RX_SOFTIRQ
    │
    ▼
net_rx_action() [NET_RX_SOFTIRQ]
    └─> napi_poll() → e1000_clean()
        └─> e1000_clean_tx_irq()
            ├─> dev_kfree_skb_any(skb)
            │   └─> __kfree_skb()
            │       └─> skb_release_head_state()
            │           └─> skb->destructor() [sock_wfree]
            │               └─> sk->sk_write_space() [sock_def_write_space]
            │                   └─> wake_up() ★唤醒阻塞在send()的进程
            └─> netif_wake_queue()
                └─> __netif_schedule() → raise NET_TX_SOFTIRQ
    │
    ▼ (如果有Qdisc需要继续发送)
net_tx_action() [NET_TX_SOFTIRQ]
    ├─> 处理completion_queue (延迟释放的skb)
    └─> 处理output_queue (继续发送Qdisc中的包)
```

## UDP Cork机制详解

### 什么是UDP Cork？

UDP Cork是一种优化机制，允许累积多次sendmsg()调用的数据，然后一次性发送。

### 使用场景

```c
// 场景1: 使用setsockopt设置
int on = 1;
setsockopt(sockfd, SOL_UDP, UDP_CORK, &on, sizeof(on));

sendmsg(...);  // 数据被缓存
sendmsg(...);  // 继续累积
sendmsg(...);  // 继续累积

int off = 0;
setsockopt(sockfd, SOL_UDP, UDP_CORK, &off, sizeof(off));  // 一次性发送

// 场景2: 使用MSG_MORE标志
sendmsg(..., MSG_MORE);  // 缓存
sendmsg(..., MSG_MORE);  // 继续
sendmsg(..., 0);         // 发送
```

### Cork的优点

1. **减少系统调用**: 多次write合并为一次网络发送
2. **减少网络包**: 多个小包合并为大包
3. **提高吞吐量**: 减少协议开销
4. **降低CPU占用**: 减少上下文切换

### Cork的缺点

1. **增加延迟**: 数据需要等待累积
2. **需要加锁**: 保护pending状态
3. **占用内存**: 数据在队列中等待

### Cork实现原理

```c
发送流程：

=== 第一次sendmsg() (设置MSG_MORE或UDP_CORK) ===
1. udp_sendmsg()入口
   - corkreq = up->corkflag || (msg->msg_flags & MSG_MORE) = true
   - up->pending == 0，不进入if(up->pending)分支

2. 经过地址验证、路由查找等步骤...

3. 进入Cork路径 (因为corkreq为true)
   - lock_sock(sk)
   - 检查up->pending，此时为0
   - 初始化fl4（保存在inet->cork.fl.u.ip4）
   - 设置up->pending = AF_INET

4. do_append_data标签
   - up->len += ulen
   - ip_append_data()将数据追加到sk->sk_write_queue
   - 因为corkreq为true，不调用udp_push_pending_frames()
   - release_sock(sk)

=== 后续sendmsg() (继续设置MSG_MORE) ===
1. udp_sendmsg()入口
   - up->pending == AF_INET（非0）

2. 进入if(up->pending)分支
   - lock_sock(sk)
   - 再次检查up->pending == AF_INET
   - 直接goto do_append_data

3. do_append_data标签
   - up->len += ulen（累加）
   - ip_append_data()追加数据
   - 因为corkreq为true，继续不发送
   - release_sock(sk)

=== 最后一次sendmsg() (不设置MSG_MORE) 或 解除UDP_CORK ===
1. udp_sendmsg()入口
   - up->pending == AF_INET

2. 进入if(up->pending)分支
   - lock_sock(sk)
   - 直接goto do_append_data

3. do_append_data标签
   - up->len += ulen
   - ip_append_data()追加最后的数据
   - corkreq为false（没有MSG_MORE）
   - 调用udp_push_pending_frames(sk):
     * ip_finish_skb(): 从sk->sk_write_queue取出完整skb
     * udp_send_skb(): 添加UDP头部、计算校验和
     * 发送到IP层
     * 清除up->len = 0, up->pending = 0
   - release_sock(sk)
```

## UDP GSO详解

### 什么是UDP GSO？

UDP GSO（Generic Segmentation Offload）允许应用程序发送大于MTU的UDP数据包，由协议栈或网卡进行分段。

### 传统方式 vs GSO

```
传统方式（应用层分段）:
  App: 发送1000个1KB的UDP包
    ↓
  1000次系统调用
    ↓
  1000次协议栈处理
    ↓
  1000次网卡发送

UDP GSO方式:
  App: 发送1个1000KB的UDP包 + GSO参数
    ↓
  1次系统调用
    ↓
  1次协议栈处理
    ↓
  网卡或协议栈分段为1000个包
    ↓
  效率大幅提升！
```

### GSO使用示例

```c
// 1. 设置GSO大小
int gso_size = 1200;  // 每个段的大小
setsockopt(sockfd, SOL_UDP, UDP_SEGMENT, &gso_size, sizeof(gso_size));

// 2. 或者使用控制消息
struct msghdr msg = {...};
struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
cm->cmsg_level = SOL_UDP;
cm->cmsg_type = UDP_SEGMENT;
cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
*(uint16_t *)CMSG_DATA(cm) = 1200;

// 3. 发送大数据
char buf[100000];  // 100KB数据
sendmsg(sockfd, &msg, 0);  // 会被分成约83个包
```

### GSO的好处

1. **减少系统调用**: 1次vs多次
2. **减少协议栈开销**: 1次处理vs多次
3. **减少CPU占用**: 显著降低
4. **提高吞吐量**: 可达数倍提升
5. **减少应用复杂度**: 不需要手动分包

## 性能对比

### UDP vs TCP

| 指标 | TCP | UDP |
|------|-----|-----|
| 延迟 | 较高（握手+确认） | 很低（无连接） |
| 吞吐量 | 中等（受拥塞控制限制） | 很高（无限制） |
| CPU占用 | 高（复杂状态机） | 低（简单处理） |
| 内存占用 | 高（发送/接收缓冲区） | 低（无需缓冲） |
| 适合场景 | 可靠传输 | 实时传输 |

### 快速路径 vs Cork路径

| 特性 | 快速路径 | Cork路径 |
|------|---------|---------|
| 加锁 | 无 | 有 |
| 延迟 | 极低 | 较高 |
| 吞吐量 | 单包高 | 批量高 |
| CPU | 更低 | 稍高 |
| 适合 | 大包 | 小包合并 |

## 调试和统计

### UDP统计信息

```bash
# 查看UDP统计
cat /proc/net/snmp | grep Udp
netstat -su

# 重要计数器
UDP_MIB_INDATAGRAMS    # 接收的数据报
UDP_MIB_OUTDATAGRAMS   # 发送的数据报
UDP_MIB_NOPORTS        # 无端口错误
UDP_MIB_INERRORS       # 接收错误
UDP_MIB_SNDBUFERRORS   # 发送缓冲区错误
UDP_MIB_RCVBUFERRORS   # 接收缓冲区错误
```

### 追踪点

```bash
# 追踪UDP发送
perf probe udp_sendmsg
perf probe udp_send_skb
perf probe ip_send_skb

# 追踪执行
perf record -e probe:udp_sendmsg -ag
perf report
```

### 常见问题

**1. UDP包丢失**
```
原因:
- 发送缓冲区满（UDP_MIB_SNDBUFERRORS）
- 接收缓冲区满（UDP_MIB_RCVBUFERRORS）
- 中间路由器丢包
- 网卡队列满

解决:
- 增大SO_SNDBUF
- 使用Cork合并小包
- 使用GSO
- 应用层实现重传
```

**2. UDP性能不佳**
```
优化:
- 使用快速路径（避免cork）
- 启用GSO
- 增大发送缓冲区
- 使用多个socket并发
- 绑定CPU亲和性
```

## 总结

UDP发送路径相比TCP简单很多：

1. **无连接管理**: 不需要三次握手
2. **无拥塞控制**: 不需要cwnd、rwnd等
3. **无重传机制**: 不保存已发送数据
4. **无流量控制**: 不限制发送速率
5. **头部简单**: 只有8字节固定头部

但UDP提供了灵活的优化机制：

1. **Cork机制**: 合并多个小包
2. **GSO支持**: 高效发送大数据
3. **快速路径**: 无锁高性能
4. **校验和可选**: 根据需要选择

UDP的设计哲学是"尽力而为"（Best Effort），把可靠性和流量控制交给应用层处理，从而实现最低的延迟和最高的灵活性。

---

**注释完成日期**: 2025年10月31日（更新于2026年1月27日）  
**内核版本**: Linux 5.15.78  
**主要函数行号**:
- `udp_sendmsg()`: net/ipv4/udp.c:1236
- `udp_send_skb()`: net/ipv4/udp.c:966
- `udp_push_pending_frames()`: net/ipv4/udp.c:1070
- `ip_send_skb()`: net/ipv4/ip_output.c:2106
- `ip_local_out()`: net/ipv4/ip_output.c:148
- `__ip_local_out()`: net/ipv4/ip_output.c:113
- `ip_make_skb()`: net/ipv4/ip_output.c:2174

**发送完成通知相关函数**:
- `e1000_intr()`: drivers/net/ethernet/intel/e1000/e1000_main.c (硬中断)
- `e1000_clean()`: drivers/net/ethernet/intel/e1000/e1000_main.c (NAPI poll)
- `e1000_clean_tx_irq()`: drivers/net/ethernet/intel/e1000/e1000_main.c (TX完成处理)
- `net_tx_action()`: net/core/dev.c:5273 (NET_TX_SOFTIRQ处理)
- `__dev_kfree_skb_any()`: net/core/dev.c:3148 (释放skb)
- `netif_tx_wake_queue()`: net/core/dev.c:3088 (唤醒发送队列)
- `sock_wfree()`: net/core/sock.c:2223 (skb destructor，唤醒进程)
- `sock_def_write_space()`: net/core/sock.c:3122 (唤醒阻塞的send)

**注释文件**:
- net/ipv4/udp.c (udp_sendmsg, udp_send_skb, udp_push_pending_frames等)
- net/ipv4/ip_output.c (ip_send_skb, ip_local_out, ip_make_skb等)
- net/core/dev.c (设备层、发送完成通知)
- net/core/sock.c (sock_wfree, sock_def_write_space)
- net/core/skbuff.c (skb_release_head_state, __kfree_skb)
- drivers/net/ethernet/intel/e1000/e1000_main.c (网卡驱动TX完成处理)
- include/linux/netdevice.h (驱动接口)

**参考文档**:
- TCP_SEND_PATH_SUMMARY.md (TCP发送路径注释)
- RFC 768: User Datagram Protocol
- RFC 8085: UDP Usage Guidelines
