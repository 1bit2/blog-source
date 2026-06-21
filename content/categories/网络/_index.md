---
title: "网络"
description: "网络协议和工具"
slug: "net"
image: "category-icon.svg"
style:
    background: "#EA4335"
    color: "#fff"
---

> Linux 5.15.78 网络协议栈。共 **29 篇**，按 13 个主题分组。
> 主线：架构鸟瞰 → TCP 连接生命周期（建立 → 传输 → 断连 → 回收）。
> 支线：重传定时器、拥塞控制、常见陷阱。

## 文档列表

### 一、TCP 协议栈整体架构
1. **[TCP 协议栈架构]({{< relref "post/网络/TCP协议栈架构.md" >}})** — 状态机、收发路径全景、拥塞控制框架、定时器体系
2. **[网络子系统初始化全路径]({{< relref "post/网络/网络子系统初始化全路径.md" >}})** — start_kernel() → sock_init / net_dev_init / inet_init
3. **[网络核心数据结构]({{< relref "post/网络/网络核心数据结构.md" >}})** — sk_buff / sock / socket / net_device

### 二、Socket 创建
4. **[Socket 创建与 fd 分配]({{< relref "post/网络/socket创建与fd分配.md" >}})** — socket() 全路径、fd 位图分配、5 跳访问链

### 三、TCP 连接建立
5. **[TCP-bind 操作]({{< relref "post/网络/TCP-bind操作.md" >}})** — __inet_bind、SO_REUSEADDR/SO_REUSEPORT、SO_BINDTODEVICE
6. **[TCP-listen 操作]({{< relref "post/网络/TCP-listen操作.md" >}})** — backlog、SYN/Accept 队列、SYN Cookie、溢出排查
7. **[TCP 三次握手源码分析]({{< relref "post/网络/TCP三次握手源码分析.md" >}})** — SYN/SYN-ACK/ACK 全链路、选项协商、TFO

### 四、TCP 端口分配
8. **[TCP 端口分配与哈希表]({{< relref "post/网络/TCP端口分配与哈希表.md" >}})** — 4 张哈希表、__inet_check_established、TIME_WAIT 复用

### 五、数据发送
9. **[TCP/UDP Socket I/O]({{< relref "post/网络/TCP-UDP-Socket-IO.md" >}})** — sendmsg/recvmsg TCP vs UDP 对比
10. **[UDP 发送路径]({{< relref "post/网络/UDP发送路径.md" >}})** — udp_sendmsg → ip_make_skb
11. **[TCP 发送全路径]({{< relref "post/网络/TCP发送全路径.md" >}})** — send() → TCP/IP/邻居/Qdisc/驱动、TSO/GSO

### 六、数据接收
12. **[网络数据包接收全路径]({{< relref "post/网络/网络数据包接收全路径.md" >}})** — 网卡 → NAPI → GRO → IP → TCP/UDP → recv
13. **[TCP 接收路径深度分析]({{< relref "post/网络/TCP接收路径深度分析.md" >}})** — 快/慢路径、乱序红黑树、D-SACK、GRO

### 七、数据就绪与多路复用
14. **[Epoll 实现原理深度分析]({{< relref "post/网络/Epoll实现原理深度分析.md" >}})** — 红黑树、ep_poll_callback、ET/LT/ONESHOT
15. **[TCP 数据就绪与 Epoll 唤醒]({{< relref "post/网络/TCP数据就绪与Epoll唤醒.md" >}})** — sk_data_ready → epoll_wait 返回
16. **[Select 系统调用分析]({{< relref "post/网络/Select系统调用分析.md" >}})** — fd 扫描与等待机制

### 八、TCP 内存管理与流量控制
17. **[TCP 内存管理与流量控制]({{< relref "post/网络/TCP内存管理与流量控制.md" >}})** — sndbuf/rcvbuf 自动调整、滑动窗口、零窗口探测

### 九、TCP 断连
18. **[TCP 四次挥手与 close]({{< relref "post/网络/TCP四次挥手与close.md" >}})** — FIN 收发、SO_LINGER、TIME_WAIT、orphan

### 十、TCP 重传与定时器
19. **[TCP 重传机制]({{< relref "post/网络/TCP重传机制.md" >}})** — RTO/RACK/TLP/F-RTO/SACK
20. **[TCP 定时器深度分析]({{< relref "post/网络/TCP定时器深度分析.md" >}})** — 九大定时器详解

### 十一、TCP 拥塞控制
21. **[TCP 拥塞控制四大机制]({{< relref "post/网络/TCP拥塞控制四大机制.md" >}})** — 慢启动/拥塞避免/丢包检测/PRR
22. **[TCP 拥塞控制算法]({{< relref "post/网络/TCP拥塞控制算法.md" >}})** — tcp_congestion_ops 框架
23. **[BBR 算法原理]({{< relref "post/网络/BBR算法原理.md" >}})** — 带宽/RTT 探测模型

### 十二、常见问题与深度分析
24. **[TCP-RST 发送场景]({{< relref "post/网络/TCP-RST发送场景.md" >}})** — 各种 RST 触发场景
25. **[TCP 内部机制深度问答]({{< relref "post/网络/TCP内部机制深度问答.md" >}})** — 速率采样、RACK、重传定时器
26. **[TCP/UDP 深度掌握问答与实战问题]({{< relref "post/网络/TCP-UDP深度掌握问答与实战问题.md" >}})** — 面试级实战问答
27. **[tcpdump 抓不到包的场景分析]({{< relref "post/网络/tcpdump抓不到包的场景分析.md" >}})** — 7 大丢包场景源码定位

### 十三、Netfilter 与网络框架
28. **[Netfilter 框架与钩子机制]({{< relref "post/网络/Netfilter框架与钩子机制.md" >}})** — 五钩子架构、nf_hook、iptables/nftables
