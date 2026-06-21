+++
date = '2026-04-22'
title = 'tcpdump 抓不到包的场景分析'
weight = 28
tags = [
    "tcpdump",
    "AF_PACKET",
    "ptype_all",
    "XDP",
    "NAPI",
    "丢包",
    "收包路径",
]
categories = [
    "网络",
]
+++
# tcpdump 抓不到包的场景分析

> 基于 Linux 5.15.78 源码，分析数据包已到达网口但 tcpdump 却抓不到的所有可能场景。

---

## 一、核心问题

tcpdump 基于 `AF_PACKET` socket，在收包路径的 `ptype_all` 链表上注册 tap。**只有到达 `ptype_all` 遍历点的包才能被抓到**。因此，问题本质是：**哪些丢包发生在 `ptype_all` 之前？**

---

## 二、收包路径与 tcpdump tap 点定位

### 2.1 完整收包调用链

```
网卡硬件 → DMA 写入 Ring Buffer → 硬中断
    ↓
NAPI poll（软中断 NET_RX_SOFTIRQ）
    ↓
napi_gro_receive() / netif_receive_skb()
    ↓
netif_receive_skb_internal()                    [net/core/dev.c:6004]
    ↓
┌─ RPS 开启 ──→ enqueue_to_backlog() ─→ 目标CPU process_backlog()
│                    ↑ ★ 丢包点 A
└─ RPS 关闭 ──→ __netif_receive_skb()
                     ↓
              __netif_receive_skb_one_core()
                     ↓
              __netif_receive_skb_core()         [net/core/dev.c:5616]
                     │
                     ├─ ① do_xdp_generic()      ← ★ 丢包点 B (XDP)
                     ├─ ② skb_vlan_untag()       ← ★ 丢包点 C (VLAN)
                     ├─ ③ pfmemalloc → skip_taps ← ★ 丢包点 D (内存紧急)
                     │
                     ├─ ④ ptype_all 遍历         ← ✅ tcpdump TAP 点
                     │
                     ├─ ⑤ sch_handle_ingress()   ← TC ingress (tap 之后)
                     ├─ ⑥ nf_ingress()           ← netfilter ingress
                     ├─ ⑦ rx_handler (bridge等)
                     └─ ⑧ ptype 协议分发 (ip_rcv)
```

### 2.2 tcpdump 注册的 tap 点

tcpdump 通过 `AF_PACKET` socket 注册 `packet_type` 结构到 `ptype_all` 链表。在 `__netif_receive_skb_core()` 中：

```c
// net/core/dev.c:5670-5680
list_for_each_entry_rcu(ptype, &ptype_all, list) {    // 全局 ptype_all
    if (pt_prev)
        ret = deliver_skb(skb, pt_prev, orig_dev);
    pt_prev = ptype;
}

list_for_each_entry_rcu(ptype, &skb->dev->ptype_all, list) {  // 每设备 ptype_all
    if (pt_prev)
        ret = deliver_skb(skb, pt_prev, orig_dev);
    pt_prev = ptype;
}
```

`deliver_skb()` 会克隆 skb 并调用 `packet_rcv()` 或 `tpacket_rcv()` 将包送给 tcpdump。

---

## 三、tcpdump 抓不到包的 7 大场景

### 场景 1: NIC 硬件层丢弃

**发生位置**：网卡芯片 / 驱动 Ring Buffer 之前

**原因**：
- **Ring Buffer 满（RX overrun）**：NAPI poll 速度跟不上网卡收包速度，Ring Buffer 中的 DMA 描述符用尽
- **网卡硬件 Filter 丢弃**：RSS、Flow Director、VLAN Filter、MAC Filter 等硬件过滤
- **FCS/CRC 校验失败**：物理层错误，网卡直接丢弃
- **硬件限速**：网卡 storm control、hardware rate limiter

**检查方法**：
```bash
ethtool -S eth0 | grep -E 'rx_dropped|rx_missed|rx_fifo|rx_crc|rx_errors'
```

这些计数器由 NIC 驱动维护，数据从未进入内核协议栈，tcpdump 完全无感知。

---

### 场景 2: Native/Offload XDP DROP

**发生位置**：驱动 NAPI poll 内部，`netif_receive_skb()` 之前

**原理**：Native XDP 直接在驱动 poll 函数中运行 eBPF 程序，操作的是 `xdp_buff`（指向 Ring Buffer DMA 区域），不经过 `skb` 分配，完全绕过协议栈。

```
驱动 poll()
    → xdp_buff 指向 ring buffer 中的数据
    → bpf_prog_run_xdp() 执行 XDP 程序
    → 返回 XDP_DROP → 直接回收 buffer，不创建 skb
    → 返回 XDP_PASS → 才进入 napi_gro_receive() → 后续协议栈
```

**关键**：Native XDP 在 skb 分配之前执行，所以 `ptype_all` 永远看不到被 XDP_DROP 的包。

**检查方法**：
```bash
# 查看 XDP 程序
ip link show dev eth0 | grep xdp
bpftool prog list
bpftool net show
```

---

### 场景 3: Generic XDP DROP

**发生位置**：`__netif_receive_skb_core()` 内部，**`ptype_all` 之前**

```c
// net/core/dev.c:5645-5655
if (static_branch_unlikely(&generic_xdp_needed_key)) {
    migrate_disable();
    ret2 = do_xdp_generic(rcu_dereference(skb->dev->xdp_prog), skb);
    migrate_enable();

    if (ret2 != XDP_PASS) {
        ret = NET_RX_DROP;
        goto out;    // 直接退出，不经过 ptype_all
    }
}
```

`do_xdp_generic()` → `netif_receive_generic_xdp()` 中：

```c
// net/core/dev.c:5165-5180
act = bpf_prog_run_generic_xdp(skb, xdp, xdp_prog);
switch (act) {
case XDP_DROP:
do_drop:
    kfree_skb(skb);    // 包被释放，tcpdump 看不到
    break;
}
```

还有一个隐蔽的丢包：`pskb_expand_head()` 或 `skb_linearize()` 失败（内存不足）也会 `goto do_drop`。

---

### 场景 4: RPS 入队 backlog 满

**发生位置**：`enqueue_to_backlog()`，**在 `__netif_receive_skb_core()` 之前**

当开启 RPS（Receive Packet Steering）时，包被调度到目标 CPU 的 `input_pkt_queue`。如果目标 CPU 队列满，包被直接丢弃：

```c
// net/core/dev.c:4988-5019
if (!netif_running(skb->dev))
    goto drop;                    // 设备已 down

qlen = skb_queue_len(&sd->input_pkt_queue);
if (qlen <= READ_ONCE(netdev_max_backlog) && !skb_flow_limit(skb, qlen)) {
    // 正常入队
} else {

drop:
    sd->dropped++;
    atomic_long_inc(&skb->dev->rx_dropped);
    kfree_skb(skb);               // 丢弃，tcpdump 看不到
    return NET_RX_DROP;
}
```

三个丢包条件：
1. **设备已 down**：`!netif_running(skb->dev)`
2. **队列超过 `netdev_max_backlog`**（默认 1000）
3. **flow_limit 触发**：队列超过一半且单流包数超过限制

**检查方法**：
```bash
# 查看每个 CPU 的 backlog 丢包
cat /proc/net/softnet_stat
# 第二列 = dropped 数

# 调大 backlog 上限
sysctl -w net.core.netdev_max_backlog=10000
```

> **注意**：不开 RPS 时不走 `enqueue_to_backlog`，直接 `__netif_receive_skb()`。但使用 `netif_rx()`（旧式驱动）的路径**总是**走 backlog。

---

### 场景 5: VLAN 解包失败

**发生位置**：`__netif_receive_skb_core()` 中，**`ptype_all` 之前**

```c
// net/core/dev.c:5658-5661
if (eth_type_vlan(skb->protocol)) {
    skb = skb_vlan_untag(skb);
    if (unlikely(!skb))
        goto out;    // skb 已被释放，tcpdump 看不到
}
```

`skb_vlan_untag()` 在以下情况可能返回 NULL：
- `skb_share_check()` 失败（内存不足无法 clone）
- 数据不足以解析 VLAN tag

---

### 场景 6: pfmemalloc 跳过 tap

**发生位置**：`__netif_receive_skb_core()` 中，**直接 `goto skip_taps`**

```c
// net/core/dev.c:5667-5668
if (pfmemalloc)
    goto skip_taps;    // 跳过整个 ptype_all 遍历！
```

当系统内存紧张，网卡驱动使用 `PFMEMALLOC` 保留内存分配 skb 时，`pfmemalloc` 标志为 true。此时内核认为这些包只应送给需要它们的 socket（如 swap over NFS），**跳过所有 tap 点**，tcpdump 完全看不到。

后续还有一道过滤：
```c
// net/core/dev.c:5700-5701
if (pfmemalloc && !skb_pfmemalloc_protocol(skb))
    goto drop;    // 非白名单协议直接丢弃
```

---

### 场景 7: AF_PACKET socket 自身丢包

**发生位置**：`ptype_all` 之后，`packet_rcv()` 内部

包到达了 tap 点，但在 `packet_rcv()` 中被丢弃：

```c
// net/packet/af_packet.c:2114-2121
res = run_filter(skb, sk, snaplen);
if (!res)
    goto drop_n_restore;            // BPF filter 不匹配

if (atomic_read(&sk->sk_rmem_alloc) >= sk->sk_rcvbuf)
    goto drop_n_acct;               // socket 接收缓冲区满
```

丢弃时计数：
```c
// net/packet/af_packet.c:2171-2174
drop_n_acct:
    atomic_inc(&po->tp_drops);      // 可通过 /proc 查看
    atomic_inc(&sk->sk_drops);
```

**原因**：
1. **BPF filter 不匹配**：tcpdump 的过滤表达式编译为 BPF，不匹配的包被丢弃
2. **socket 缓冲区满**：tcpdump 处理速度跟不上收包速度（高流量时常见）
3. **skb_clone 失败**：内存不足无法克隆 skb

**检查方法**：
```bash
# tcpdump 退出时会报告
# "N packets dropped by kernel"

# 直接查看
cat /proc/net/packet
```

---

## 四、总结图

```
硬件层面                  软件层面 (ptype_all 之前)         ptype_all 之后
─────────────────────  ─────────────────────────────    ──────────────────

[NIC Ring Buffer 满]   [Native XDP DROP]               [BPF filter 不匹配]
[FCS/CRC 错误]         [Generic XDP DROP]              [socket 缓冲区满]
[硬件 Filter 丢弃]     [RPS backlog 队列满]            [skb_clone 失败]
[MTU 超限]             [VLAN 解包失败]
                       [pfmemalloc skip_taps]

       ↑                        ↑                             ↑
  tcpdump 完全         tcpdump 完全                    包到达了 tap 但
  不可见               不可见                          AF_PACKET 内部丢弃
```

---

## 五、按层分类排查指南

### 第一步：确认包是否到达网卡

```bash
ethtool -S eth0 | grep rx_
# 关注: rx_packets, rx_bytes (正常收包)
# 关注: rx_dropped, rx_missed_errors, rx_fifo_errors (硬件丢包)
```

### 第二步：确认软件层面丢包

```bash
# per-CPU backlog 丢包
cat /proc/net/softnet_stat
# 各列含义: processed, dropped, time_squeeze, ...

# 网络设备统计
cat /sys/class/net/eth0/statistics/rx_dropped

# XDP 丢包
bpftool prog show
ip -s link show dev eth0    # 查看 xdp 项
```

### 第三步：确认 AF_PACKET 层丢包

```bash
cat /proc/net/packet
# tp_drops 列

# 或使用 ss
ss -f packet -p
```

### 第四步：综合排查命令

```bash
# 查看所有丢包原因（需要 dropwatch 或 perf）
perf record -e skb:kfree_skb -a sleep 10
perf script

# 或用 dropwatch 工具
dropwatch -l kas
```

---

## 六、各场景对应源码位置

| 场景 | 源码位置 | 丢包标识 | 统计计数器 |
|------|----------|----------|-----------|
| Ring Buffer 满 | 驱动代码 | 驱动 ethtool stats | `rx_missed_errors` |
| Native XDP DROP | 驱动 NAPI poll | XDP 返回值 | `bpftool prog` 统计 |
| Generic XDP DROP | `net/core/dev.c:5645` | `kfree_skb` | 无专用计数器 |
| RPS backlog 满 | `net/core/dev.c:5011` | `sd->dropped++` | `/proc/net/softnet_stat` |
| VLAN 解包失败 | `net/core/dev.c:5659` | `skb=NULL` | 无专用计数器 |
| pfmemalloc 跳过 | `net/core/dev.c:5667` | `goto skip_taps` | 无 |
| AF_PACKET 缓冲区满 | `net/packet/af_packet.c:2120` | `tp_drops++` | `/proc/net/packet` |

---

## 七、实际案例分析

### 案例 1：高流量下 tcpdump 丢包

**现象**：万兆网卡，tcpdump 报告 "20000 packets dropped by kernel"

**根因**：AF_PACKET socket 缓冲区满（场景 7）

**解决**：
```bash
# 增大 socket 缓冲区
tcpdump -i eth0 -B 8192 -w capture.pcap

# 使用 TPACKET_V3 mmap 模式（tcpdump 默认已使用）
# 减少过滤条件，降低不必要的包进入 socket
```

### 案例 2：开启 XDP 后 tcpdump 看不到某些包

**现象**：XDP 程序挂载后，部分包 tcpdump 抓不到但对端确认已发送

**根因**：XDP_DROP 在 ptype_all 之前（场景 2/3）

**解决**：
```bash
# 临时卸载 XDP 排查
ip link set dev eth0 xdp off

# 或在 XDP 程序中添加统计 map 查看 DROP 数
```

### 案例 3：RPS 开启后偶发丢包

**现象**：开启 RPS 后，tcpdump 偶尔抓不到特定 TCP 连接的包

**根因**：目标 CPU backlog 满（场景 4）

**检查**：
```bash
cat /proc/net/softnet_stat
# 第二列非零 = 有 backlog 丢包

# 增大 backlog
sysctl -w net.core.netdev_max_backlog=10000
```

### 案例 4：ARP 学到错误 MAC 导致回程包走错路径 + 中间设备注入 RST

**现象**：
- 设备 A（双网卡：eth0=192.168.100.82/24, eth1=192.168.20.79/24）与设备 B（192.168.100.123/24）建立 TCP 连接
- ping 始终正常，但 TCP 数据/keepalive ACK 间歇性丢失
- 在中间设备 L0 上 tcpdump 抓不到 B→A 的任何回程包
- 抓包中出现纯 `[R]` 标志的 RST（非本机内核的 `[R.]`），且 RST 后 keepalive 仍在继续

**根因**：`arp_ignore=0`（默认）导致 ARP 响应从错误接口发出

Linux 默认 `arp_ignore=0` 表示**任何接口上的 IP 都会响应 ARP 请求**。当 B 广播请求 192.168.100.82 的 MAC 时：
1. eth0（192.168.100.82）响应自己的 MAC `1c:54:e6:31:c8:27`
2. eth1（192.168.20.79）**也会响应** 192.168.100.82 的请求，返回 MAC `12:26:ec:68:72:b6`
3. B 的 ARP 表记录**先到达的响应**，如果 eth1 的响应先到 → B 学到错误 MAC
4. B 发给 192.168.100.82 的回程包全部走向 eth1 所在网络 → 被中间设备 L0 丢弃或路由到错误路径

**为什么 ping 通但 TCP 断**：
- ICMP echo reply 由内核直接在收包接口生成回复，走对了路
- TCP 数据/ACK 经过路由表查找，从错误的二层路径返回 → 被丢弃

**tcpdump 看不到的原因**：回程包根本没经过 tcpdump 所监听的接口/路径，不属于内核协议栈内丢弃，而是**二层路径就错了**。

**pcap 实证分析（绝对序列号）**：

连接 82.36057→123.57421，建立时的 ISN：
- IC801 ISN = 830744071 → snd_nxt = 830748623（ISN+4552）
- IC204 ISN = 8631213 → snd_nxt = 8633276（ISN+2063）

```
19:17:06  82→123 [.] ack 8633276          123→82 [.] ack 830748623  ✓ 正常
19:17:16  82→123 [.] ack 8633276          123→82 [.] ack 830748623  ✓ 正常
          ──── ARP 表翻转 ────
19:17:26.266  82→123 [.] ack 8633276      IC204 回复 ACK → 走错路径
19:17:26.267  82→123 [R] seq=830748623 win=0   ← ★ seq = IC204 的 ack_seq
19:17:28.403  82→123 [.] ack 8633276      ← IC801 keepalive 继续（socket 未关闭）
19:17:30.533  82→123 [.] ack 8633276      ← 继续
19:17:32.666  82→123 [R.] seq=830748623 ack=8633276 win=65535  ← IC801 超时关闭
```

**RST 的 seq=830748623 精确等于 IC204 回复中的 ack_seq**，这是 `tcp_v4_send_reset()` 第 1052 行 `rep.th.seq = th->ack_seq` 的结果。`win=0` 则来自 `memset(&rep, 0, sizeof(rep))`。

SYN 重连也遵循同一机制（RST.seq = SYN_seq + 1 = IC204 SYN-ACK 中的 ack_seq）：
```
19:17:32.784  82→123 [S]  seq=1108198297
19:17:32.785  82→123 [R]  seq=1108198298 win=0   ← = SYN_seq + 1
```

1.5 小时 pcap 中共出现 4 次 RST 风暴（17:49, 18:22, 18:34, 19:17），间隔不规则，符合 ARP 邻居状态机间歇性翻转特征。

**完整因果链**：

```
1. arp_ignore=0
     → eth1 也响应 192.168.100.82 的 ARP → ARP 表不稳定

2. ARP 表翻转
     → IC204 的回程包（ACK/SYN-ACK）走错二层路径
     → L0 上 tcpdump 抓不到回程包

3. 回程 ACK（带 ack_seq=830748623）到达 IC801 的 eth1（错误接口）
     → ip_rcv: local table 有 82 → RTN_LOCAL → 交给 TCP
     → tcp_v4_rcv: sdif = vrf-mgmt（eth1 所属 VRF）
     → socket lookup: 四元组匹配 ✓，但 VRF 不匹配 ✗（socket 绑定 vrf-data）
       inet_bound_dev_eq: vrf-data ≠ eth1 && vrf-data ≠ vrf-mgmt → false
     → no_tcp_socket → tcp_v4_send_reset(NULL, skb)
     → th->ack 为真 → rep.th.seq = th->ack_seq = 830748623
     → 交换 src/dst → 纯 [R] win=0 发向 IC204

4. IC204 收到 [R] → 关闭连接
   IC801 vrf-data 上的 socket 完全不知情 → keepalive 继续但无响应
     → tcp_keepalive_timer() 超时 → tcp_send_active_reset() → [R.] win=65535
```

**为什么 `tcpdump -i eth0` 看不到回程 ACK 却能看到 RST**：

**pcap MAC 地址分析证据**（已由实际操作人员确认 pcap 确为 eth0 单口抓包）：

对 pcap 做 MAC 地址分析可以判断抓包接口：
- eth0 MAC（`1c:54:e6:31:c8:27`）：出现在大量单播帧的 src/dst → **抓包接口**
- eth1 MAC（`12:26:ec:68:72:b6`）：**仅出现在广播/多播帧**，零个单播帧 → 不是抓包接口

后续补充的**双网口抓包**（`tcpdump -i any` 或分别抓 eth0/eth1）能看到 eth1 上 IC204 的回程 ACK，完整验证了 VRF 理论。

所有纯 `[R]` RST 的 src MAC 都是 eth0：
```
19:17:26.267  1c:54:e6:31:c8:27 > 68:fe:71:3c:b1:77  [R] seq=830748623
              ^^^^^^^^^^^^^^^^^^   ^^^^^^^^^^^^^^^^^^
              eth0 MAC（出接口）    IC204 MAC（目标）
```

看到 192.168.20.x 流量是因为广播帧被交换机转发到所有端口，不代表抓了 eth1。

**交换机单播转发导致 eth0 看不到 IC204 的 ACK**：

ARP 翻转后，IC204 的 ACK 目标 MAC 变成 eth1 的 MAC。交换机是 L2 设备，单播帧**只转发到目标 MAC 所在端口**，不会发到其他端口：

```
IC204 ACK → 交换机查 MAC 表 → dst MAC = 12:26:ec:68:72:b6(eth1)
            → 转发到 eth1 端口 → eth0 端口上完全不可见
```

**RST 却从 eth0 出去的原因**：

`tcp_v4_send_reset(NULL, skb)` 中 `sk=NULL`（无 socket），`ip_send_unicast_reply()` 做路由查找时没有 VRF 绑定，走主路由表 → 目标 192.168.100.123 的出接口是 eth0 → RST 从 eth0 发出。

```
eth1 收到 IC204 ACK → VRF 不匹配 → tcp_v4_send_reset(NULL, skb)
  → ip_send_unicast_reply() → ip_route_output_key(net, &fl4)
  → 主路由表: 192.168.100.0/24 via eth0 → RST 从 eth0 出
```

**eth0 上 tcpdump 看到的视角**：

| 时刻 | eth0 上看到 | 实际发生（eth1 不可见） |
|------|-----------|---------------------|
| 19:17:26.266 | 82→123 keepalive ✓ | IC801 从 eth0 发出 keepalive |
| | （空白，无 IC204 回复） | IC204 ACK → 交换机 → eth1 端口 → IC801 eth1 收 |
| | （空白） | eth1: VRF 不匹配 → tcp_v4_send_reset() |
| 19:17:26.267 | 82→123 [R] seq=830748623 ✓ | RST 路由走主表 → 从 eth0 出 |

从 eth0 的视角看，就是"发了 keepalive → 对方没回 → 突然冒出一个 RST"。

| tcpdump 参数 | IC204 回程 ACK（到达 eth1） | IC801 生成的 RST（从 eth0 出） |
|-------------|---------------------------|-------------------------------|
| `-i eth0` | 看不到（交换机发到了 eth1 端口） | **能看到** ✓（RST 路由走主表从 eth0 出） |
| `-i eth1` | 能看到 ✓ | 看不到（RST 不从 eth1 出） |
| `-i any` | 能看到 ✓ | 能看到 ✓ |

**排查此类问题应始终使用 `tcpdump -i any`**，可以看到跨接口/跨 VRF 的完整流量，并标注每个包的接口名。

**解决**：
```bash
# 设置 arp_ignore=1：仅当目标 IP 配置在入接口上时才响应 ARP
echo 1 > /proc/sys/net/ipv4/conf/all/arp_ignore
echo 1 > /proc/sys/net/ipv4/conf/eth0/arp_ignore

# 清除 B 上的错误 ARP 缓存
# 在 B 上执行：
ip neigh flush 192.168.100.82
```

**关键证据：`ip neighbor` 同一 IP 出现在两个接口**：

```
192.168.100.123 dev eth1 lladdr 68:fe:71:3c:b1:77 STALE
192.168.100.123 dev eth0 lladdr 68:fe:71:3c:b1:77 REACHABLE
```

同一 IP 在两个接口上都有 ARP 条目是问题发生过的直接证据。eth1 上的 `STALE` 说明对端的包**曾经从 eth1 方向到达过**。

**ARP 邻居状态机**（`net/core/neighbour.c`）使问题间歇性发生：

```
REACHABLE ──(base_reachable_time/2 ≈ 15s 超时)──→ STALE
    ↑                                                 │
    │                                          (需要发包时)
    │                                                 ↓
REACHABLE ←──(探测成功)── DELAY ──(探测失败)──→ FAILED
```

当 eth0 的条目退化为 `STALE`，而 eth1 恰好收到对端的 ARP 响应或数据包，eth1 条目可能翻转为 `REACHABLE`，后续流量就走了 eth1 —— 回程路径错误 → TCP 数据丢失。ARP 默认老化时间 20 分钟，故问题呈间歇性。

**排查命令**：
```bash
# 在 A 上检查是否同一 IP 出现在两个接口（关键判定）
ip neigh show | grep 192.168.100.123
# 若两个接口都有条目 → 确认 ARP 路径问题

# 在 B 上检查 ARP 表
ip neigh show 192.168.100.82
# 如果 MAC 不是 eth0 的 → 确认 ARP 问题

# 确认 RST 标志位（区分外部注入 [R] 和本机 [R.]）
tcpdump -r capture.pcap -nn 'tcp[tcpflags] & tcp-rst != 0'

# 在 A 上检查 arp_ignore 设置
cat /proc/sys/net/ipv4/conf/eth0/arp_ignore   # 0 = 有风险

# 删除错误接口上的条目
ip neigh del 192.168.100.123 dev eth1
```
