+++
date = '2026-05-07'
title = 'Netfilter 框架与钩子机制'
weight = 29
tags = [
    "netfilter",
    "iptables",
    "nftables",
    "NF_HOOK",
    "PRE_ROUTING",
    "POST_ROUTING",
    "FORWARD",
    "INPUT",
    "OUTPUT",
    "conntrack",
    "NAT",
    "DNAT",
    "SNAT",
]
categories = [
    "网络",
]
+++
# Netfilter 框架与钩子机制

> 基于 Linux 5.15.78 源码。从"为什么在 IP 层设钩子"出发，分析 5 个钩子点的精确位置、触发时序、优先级链、NF_HOOK 宏的工作机制，以及 conntrack/NAT 如何挂载到钩子上。

## 一、Netfilter 解决什么问题

内核需要在数据包经过网络协议栈的过程中**插入检查点**，让防火墙规则（iptables/nftables）、NAT、连接跟踪等模块能够检查、修改或丢弃数据包，而不需要修改协议栈本身的代码。

Netfilter 的方案：在 IP 层的关键路径上埋入 **5 个钩子点（hook point）**，任何内核模块都可以向这些钩子注册回调函数。数据包经过钩子时，依次调用所有注册的回调。

## 二、为什么钩子在 IP 层

### 2.1 IP 层是唯一的流量汇聚点

```
      TCP   UDP   ICMP   SCTP   ...       ← 传输层（种类繁多）
        \    |    /     /
         ▼   ▼  ▼    ▼
      ┌──────────────────┐
      │      IP 层        │  ← 所有包的必经之路
      └──────────────────┘
         /    |     \
        ▼    ▼      ▼
      eth0  wlan0  ppp0  ...              ← 链路层（种类繁多）
```

传输层种类太多（TCP/UDP/ICMP/SCTP...），逐个加钩子不现实且无法统一管理。链路层只看 MAC 地址，无法基于 IP 地址/端口做过滤。**IP 层是唯一能同时看到网络地址信息、覆盖所有协议流量的位置。**

### 2.2 IP 层有路由决策

防火墙需要区分三种流量：进入本机的、本机发出的、经过本机转发的。这个三岔路口的判断发生在 **IP 层的路由查找** — 只有 IP 层有路由表，只有路由查找后才能确定包是"给本机"还是"要转发"。

### 2.3 NAT 必须配合路由

- DNAT（改目的地址）必须在路由之前做 — 改了目的地址，路由结果才会变
- SNAT（改源地址）必须在路由之后做 — 需要知道从哪个网卡出去才能决定伪装成什么地址

这种与路由的紧密配合只有在 IP 层才能实现。

## 三、5 个钩子点的定义

```c
// include/uapi/linux/netfilter.h:42
enum nf_inet_hooks {
    NF_INET_PRE_ROUTING,   // 0 — 收到包，路由之前
    NF_INET_LOCAL_IN,      // 1 — 路由判定"给本机"之后
    NF_INET_FORWARD,       // 2 — 路由判定"要转发"之后
    NF_INET_LOCAL_OUT,     // 3 — 本机发出包，路由之前
    NF_INET_POST_ROUTING,  // 4 — 路由之后，发往网卡之前
    NF_INET_NUMHOOKS,      // 5 — 计数
};
```

## 四、5 个钩子的精确位置与流量路径

### 4.1 全景图

```
                              网卡收到数据包
                                    │
                                    ▼
                              ┌───────────┐
                              │  ip_rcv() │  校验 IP 头
                              └─────┬─────┘
                                    │
                  ① NF_INET_PRE_ROUTING ──── DNAT 在这里做（改目的地址）
                                    │          conntrack 开始跟踪
                                    ▼
                            ┌───────────────┐
                            │   路由决策     │  查路由表
                            └───┬───────┬───┘
                  目的是本机 │           │ 目的是其他机器
                              │           │
              ② NF_INET_LOCAL_IN    ③ NF_INET_FORWARD
                (iptables INPUT)      (iptables FORWARD)
                              │           │
                              ▼           │
                       ┌──────────┐       │
                       │  传输层   │       │
                       │ TCP/UDP  │       │
                       └────┬─────┘       │
                            │             │
                    本机进程发出的包       │
                            │             │
              ④ NF_INET_LOCAL_OUT         │
                (iptables OUTPUT)         │
                            │             │
                            ▼             ▼
                            ┌───────────────┐
                            │   路由决策     │
                            └───────┬───────┘
                                    │
                  ⑤ NF_INET_POST_ROUTING ── SNAT 在这里做（改源地址）
                                    │         conntrack 确认
                                    ▼
                              ┌───────────┐
                              │  网卡发送  │
                              └───────────┘
```

### 4.2 每个钩子的源码位置

| # | 钩子 | 函数 | 源码位置 | 触发时机 |
|---|------|------|---------|---------|
| ① | `NF_INET_PRE_ROUTING` | `ip_rcv()` | `net/ipv4/ip_input.c:599` | IP 头校验通过，路由查找之前 |
| ② | `NF_INET_LOCAL_IN` | `ip_local_deliver()` | `net/ipv4/ip_input.c:307` | 路由判定目的是本机，分片重组之后 |
| ③ | `NF_INET_FORWARD` | `ip_forward()` | `net/ipv4/ip_forward.c:157` | 路由判定需要转发，TTL 递减之后 |
| ④ | `NF_INET_LOCAL_OUT` | `__ip_local_out()` | `net/ipv4/ip_output.c:131` | 本机生成的包，IP 头填充完毕，路由之前 |
| ⑤ | `NF_INET_POST_ROUTING` | `ip_output()` | `net/ipv4/ip_output.c:643` | 路由完成，即将交给网卡发送 |

### 4.3 三种流量经过的钩子

| 流量类型 | 经过的钩子 | 示例 |
|---------|-----------|------|
| 外部 → 本机 | ① PRE_ROUTING → ② LOCAL_IN | SSH 连接本机 |
| 本机 → 外部 | ④ LOCAL_OUT → ⑤ POST_ROUTING | 本机 curl 外部 |
| 转发（穿越） | ① PRE_ROUTING → ③ FORWARD → ⑤ POST_ROUTING | 本机作网关 |

转发流量**不经过** LOCAL_IN 和 LOCAL_OUT — 包不"进入"传输层，只在 IP 层转弯就从另一个网卡出去。

### 4.4 源码验证：PRE_ROUTING

```c
// net/ipv4/ip_input.c:589-601
int ip_rcv(struct sk_buff *skb, struct net_device *dev, ...)
{
    struct net *net = dev_net(dev);

    skb = ip_rcv_core(skb, net);  // IP 头校验
    if (skb == NULL)
        return NET_RX_DROP;

    // PRE_ROUTING 钩子：通过后调用 ip_rcv_finish 做路由查找
    return NF_HOOK(NFPROTO_IPV4, NF_INET_PRE_ROUTING,
                   net, NULL, skb, dev, NULL,
                   ip_rcv_finish);
}
```

### 4.5 源码验证：LOCAL_OUT vs POST_ROUTING

这两个钩子看似都在"发出去"的路径上，但**中间隔着路由决策**：

```c
// net/ipv4/ip_output.c:113 — LOCAL_OUT: 路由之前
int __ip_local_out(struct net *net, struct sock *sk, struct sk_buff *skb)
{
    iph->tot_len = htons(skb->len);
    ip_send_check(iph);
    // LOCAL_OUT → 通过后调用 dst_output() 执行路由
    return nf_hook(NFPROTO_IPV4, NF_INET_LOCAL_OUT,
                   net, sk, skb, NULL, skb_dst(skb)->dev,
                   dst_output);
}

// net/ipv4/ip_output.c:630 — POST_ROUTING: 路由之后
int ip_output(struct net *net, struct sock *sk, struct sk_buff *skb)
{
    skb->dev = skb_dst(skb)->dev;  // 出口设备已确定
    // POST_ROUTING → 通过后调用 ip_finish_output() 发往网卡
    return NF_HOOK_COND(NFPROTO_IPV4, NF_INET_POST_ROUTING,
                        net, sk, skb, indev, dev,
                        ip_finish_output, ...);
}
```

**为什么不能合并？** 因为 LOCAL_OUT 能通过 DNAT 改变目的地址从而影响路由结果，POST_ROUTING 做不到（路由已完成）。而 POST_ROUTING 同时覆盖本机发出和转发的包（两条路径汇合于此），LOCAL_OUT 只覆盖本机发出的包。

## 五、NF_HOOK 宏的工作机制

### 5.1 宏定义

```c
// include/linux/netfilter.h:300
static inline int
NF_HOOK(uint8_t pf, unsigned int hook, struct net *net, struct sock *sk,
        struct sk_buff *skb, struct net_device *in, struct net_device *out,
        int (*okfn)(struct net *, struct sock *, struct sk_buff *))
{
    int ret = nf_hook(pf, hook, net, sk, skb, in, out, okfn);
    if (ret == 1)         // 所有钩子函数返回 NF_ACCEPT
        ret = okfn(...);  // 调用正常处理函数（如 ip_rcv_finish）
    return ret;
}
```

### 5.2 钩子函数的返回值

```c
// include/uapi/linux/netfilter.h:11
#define NF_DROP   0  // 丢弃包
#define NF_ACCEPT 1  // 放行，继续下一个钩子函数
#define NF_STOLEN 2  // 钩子函数接管了包（不调用 okfn，也不释放）
#define NF_QUEUE  3  // 送到用户空间队列（如 NFQUEUE 目标）
#define NF_REPEAT 4  // 再调用一次当前钩子函数
```

### 5.3 执行流程

```
NF_HOOK(NFPROTO_IPV4, NF_INET_PRE_ROUTING, ..., ip_rcv_finish)
    │
    ▼
nf_hook() 遍历该钩子点注册的所有函数，按优先级排序：
    │
    ├─ conntrack 回调 (优先级 -200)  → 返回 NF_ACCEPT
    ├─ mangle 回调   (优先级 -150)  → 返回 NF_ACCEPT
    ├─ DNAT 回调     (优先级 -100)  → 修改目的IP，返回 NF_ACCEPT
    ├─ filter 回调   (优先级 0)     → 检查规则，返回 NF_ACCEPT 或 NF_DROP
    │
    ▼
全部 ACCEPT → 调用 okfn (ip_rcv_finish)
任一 DROP   → kfree_skb()，包被丢弃
```

## 六、钩子优先级 — 同一钩子上的执行顺序

每个钩子点上可以注册多个回调函数，按 `priority` 字段排序（数值小的先执行）：

```c
// include/uapi/linux/netfilter_ipv4.h:31
enum nf_ip_hook_priorities {
    NF_IP_PRI_FIRST            = INT_MIN,   // 最先执行
    NF_IP_PRI_RAW_BEFORE_DEFRAG = -450,     // raw 表（分片重组前）
    NF_IP_PRI_CONNTRACK_DEFRAG = -400,      // conntrack 分片重组
    NF_IP_PRI_RAW              = -300,      // raw 表
    NF_IP_PRI_SELINUX_FIRST    = -225,      // SELinux
    NF_IP_PRI_CONNTRACK        = -200,      // conntrack 连接跟踪 ← 最先知道包属于哪个连接
    NF_IP_PRI_MANGLE           = -150,      // mangle 表（修改包头）
    NF_IP_PRI_NAT_DST          = -100,      // DNAT（目的地址转换）
    NF_IP_PRI_FILTER           = 0,         // filter 表（防火墙规则）← 用户最常用
    NF_IP_PRI_SECURITY         = 50,        // security 表
    NF_IP_PRI_NAT_SRC          = 100,       // SNAT（源地址转换）
    NF_IP_PRI_SELINUX_LAST     = 225,       // SELinux 收尾
    NF_IP_PRI_CONNTRACK_HELPER = 300,       // conntrack helper（FTP 等）
    NF_IP_PRI_CONNTRACK_CONFIRM = INT_MAX,  // conntrack 确认 ← 最后执行
};
```

**设计逻辑**：conntrack 最先（需要知道包属于哪个连接），NAT 在 filter 之前（DNAT 改了地址后 filter 才能正确匹配），filter 在中间（用户规则），conntrack confirm 最后（确认连接状态）。

### 6.1 以 PRE_ROUTING 为例

```
包到达 PRE_ROUTING 钩子:
    │
    ├─ [-400] conntrack_defrag  — 分片重组
    ├─ [-300] raw 表            — 跳过 conntrack (NOTRACK)
    ├─ [-200] conntrack         — 连接跟踪，确定包属于哪个连接
    ├─ [-150] mangle 表         — 修改 TOS/TTL 等
    ├─ [-100] nat (DNAT)        — 目的地址转换
    ├─ [  0 ] filter 表         — 不在 PRE_ROUTING（filter 只在 INPUT/FORWARD/OUTPUT）
    ├─ [ 300] conntrack helper  — 应用层网关（FTP 等）
    └─ [MAX ] conntrack confirm — 确认连接
    │
    ▼
    ip_rcv_finish() → 路由查找
```

## 七、钩子注册机制

### 7.1 注册结构体

```c
// include/linux/netfilter.h:85
struct nf_hook_ops {
    nf_hookfn           *hook;     // 回调函数
    struct net_device   *dev;      // 绑定的网络设备（NULL=所有设备）
    void                *priv;     // 私有数据
    u8                  pf;        // 协议族: NFPROTO_IPV4
    enum nf_hook_ops_type hook_ops_type:8;
    unsigned int        hooknum;   // 钩子点: NF_INET_PRE_ROUTING 等
    int                 priority;  // 优先级
};
```

### 7.2 注册示例（conntrack）

```c
// net/netfilter/nf_conntrack_proto.c:466
err = nf_register_net_hooks(net, ipv4_conntrack_ops,
                            ARRAY_SIZE(ipv4_conntrack_ops));
```

conntrack 在 PRE_ROUTING 和 LOCAL_OUT 注册回调（入向和出向各跟踪一次），优先级 `NF_IP_PRI_CONNTRACK`（-200），确保在 NAT 和 filter 之前执行。

## 八、iptables 四表五链与钩子的映射

iptables 的"表"和"链"本质上就是注册在不同钩子、不同优先级上的回调函数：

```
                 ① PRE_ROUTING         ② INPUT          ③ FORWARD
                 ┌─────────────┐    ┌─────────────┐  ┌─────────────┐
  conntrack(-200)│ conntrack   │    │ conntrack   │  │             │
  raw     (-300) │ raw 表      │    │             │  │             │
  mangle  (-150) │ mangle 表   │    │ mangle 表   │  │ mangle 表   │
  nat     (-100) │ nat(DNAT)   │    │             │  │             │
  filter  (  0)  │             │    │ filter 表   │  │ filter 表   │
  security( 50)  │             │    │ security 表 │  │ security 表 │
  nat_src ( 100) │             │    │ nat 表      │  │             │
                 └─────────────┘    └─────────────┘  └─────────────┘

                 ④ OUTPUT              ⑤ POST_ROUTING
                 ┌─────────────┐    ┌─────────────┐
  conntrack(-200)│ conntrack   │    │             │
  raw     (-300) │ raw 表      │    │             │
  mangle  (-150) │ mangle 表   │    │ mangle 表   │
  nat     (-100) │ nat(DNAT)   │    │             │
  filter  (  0)  │ filter 表   │    │             │
  security( 50)  │ security 表 │    │             │
  nat_src ( 100) │             │    │ nat(SNAT)   │
                 └─────────────┘    └─────────────┘
```

**filter 表只出现在 INPUT/FORWARD/OUTPUT** — 这三个链是防火墙过滤的正确位置。PRE_ROUTING 和 POST_ROUTING 的 filter 没有意义（还没决定包的走向 / 已经要出去了）。

## 九、网络调试工具速查

> 除了 eBPF、SNMP、netstat、tcpdump、iptables、ip route、ip rule 之外的实用工具。

| 场景 | 工具 | 用法示例 | 优势 |
|------|------|---------|------|
| 替代 netstat | **ss** | `ss -tiepm` | 直接读 netlink，速度快两个量级；显示拥塞算法、RTT、重传数 |
| 丢包定位 | **dropwatch** | `dropwatch -l kas` | 精确报告包在哪个内核函数被丢弃 |
| 丢包定位 | **perf trace** | `perf trace -e skb:kfree_skb` | 追踪所有内核丢包事件 |
| 内核函数追踪 | **bpftrace** | `bpftrace -e 'kprobe:tcp_retransmit_skb { printf("retx\n"); }'` | 一行脚本追踪任意内核函数 |
| 路由模拟 | **ip route get** | `ip route get 8.8.8.8` | 模拟一个包的路由查找，显示走哪条路、哪个网卡 |
| 连接跟踪 | **conntrack** | `conntrack -L` / `conntrack -E` | 查看/监听 conntrack 表（NAT 映射） |
| nftables | **nft** | `nft list ruleset` | iptables 继任者，语法统一、性能更好 |
| 深度抓包 | **tshark** | `tshark -i eth0 -Y "tcp.analysis.retransmission"` | Wireshark CLI，协议解析更强 |
| 内核网络事件 | **trace-cmd** | `trace-cmd record -e 'net:*'` | 追踪所有网络子系统 tracepoint |
| TCP 诊断 | **ss -tiepm** | 显示每个 TCP 连接的 cwnd/rtt/retrans/拥塞算法 | 比 `/proc/net/tcp` 信息丰富 |
| XDP 高性能 | **xdp-tools** | `xdpdump -i eth0` | 网卡驱动层抓包/处理 |
| 网络命名空间 | **ip netns** | `ip netns exec test ping 10.0.0.1` | 隔离环境调试网络问题 |
| BPF 工具集 | **bcc-tools** | `tcplife` / `tcpretrans` / `tcpdrop` | 即装即用的 TCP 诊断脚本 |
