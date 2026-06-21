+++
date = '2026-04-29'
title = 'TCP 端口分配与哈希表存储机制'
weight = 8
tags = [
    "TCP",
    "端口",
    "bhash",
    "ehash",
    "inet_bind_bucket",
]
categories = [
    "网络",
]
+++
# TCP 端口分配与哈希表存储机制

> 基于 Linux 5.15.78 源码

本文从数据结构出发，先介绍 TCP 哈希表的整体架构和 socket 类型体系，再沿 `connect()` 调用链分析端口分配的完整流程，最后说明 TIME_WAIT 复用机制。每个环节紧密衔接，对应源码中的实际实现。

---

## 1. TCP 哈希表架构

> 🎨 **可视化**：[tcp_hashinfo 四表存储结构](tcp_hashinfo四表可视化.html)（交互式 SVG：ehash / bhash / lhash2 / listening_hash 的数组-链表层级对比 + nulls 哨兵演示）

TCP 协议通过全局变量 `tcp_hashinfo`（类型 `struct inet_hashinfo`）管理所有 socket，其中包含 4 个哈希表，分别服务于不同的查找场景。

### 1.1 为什么需要四个哈希表？——从问题出发

内核需要在不同场景下快速查找 socket，每个场景的"搜索条件"不同：

```
场景1：收到一个数据包 [SrcIP:SrcPort → DstIP:DstPort]
  → 需要找到哪个 socket 在处理这条连接 → 按四元组查找 → ehash

场景2：用户调用 bind(port=80)
  → 需要检查端口80是否已被占用 → 按端口查找 → bhash

场景3：收到一个 SYN 包 [→ DstIP:DstPort]
  → 需要找到哪个 socket 在 listen 这个端口 → 按IP+端口查找 → lhash2 / listening_hash
```

一个哈希表无法同时满足这三种查询模式，所以内核用四个哈希表各司其职：

| 哈希表 | 解决什么问题 | 哈希键 | 类比 |
|---|---|---|---|
| **ehash** | 数据包进来，找到对应连接 | 四元组(本地IP:端口+远端IP:端口) | 通讯录：按"姓名+电话"精确查人 |
| **bhash** | bind/connect时，检查端口冲突 | 本地端口 | 车位登记簿：按"车位号"查谁在用 |
| **listening_hash** | SYN包进来，找监听socket(旧) | 本地端口 | 前台总机：按"分机号"转接 |
| **lhash2** | SYN包进来，找监听socket(新) | 本地IP+端口 | 前台总机升级版：按"楼层+分机号"精确转接 |

### 1.2 四个哈希表的物理结构（内存布局）

下图展示 `tcp_hashinfo` 的完整内存布局：

```
                        tcp_hashinfo (struct inet_hashinfo)
                        ┌───────────────────────────────────────┐
                        │                                       │
  ┌─────────────────────┤  *ehash ──→ 动态分配的桶数组          │
  │                     │  *ehash_locks ──→ 分离锁数组          │
  │                     │   ehash_mask = 桶数 - 1               │
  │                     │   ehash_locks_mask = 锁数 - 1         │
  │                     │                                       │
  │  ┌──────────────────┤  *bhash ──→ 动态分配的桶数组          │
  │  │                  │   bhash_size = 桶数                   │
  │  │                  │  *bind_bucket_cachep ──→ slab缓存     │
  │  │                  │                                       │
  │  │  ┌───────────────┤  *lhash2 ──→ 动态分配的桶数组         │
  │  │  │               │   lhash2_mask = 桶数 - 1              │
  │  │  │               │                                       │
  │  │  │  ┌────────────┤   listening_hash[32] (内联数组)       │
  │  │  │  │            └───────────────────────────────────────┘
  │  │  │  │
  ▼  ▼  ▼  ▼
四个哈希表的桶数组 (下文逐一展开)
```

#### ehash：已建立连接查找表

收到数据包时，内核需要在纳秒级找到对应的 socket。ehash 通过四元组哈希实现 O(1) 查找。

```
ehash 桶数组                        ehash_locks 锁数组
(inet_ehash_bucket[])               (spinlock_t[])
                                    锁和桶是 多对一 关系
桶[0]  ─chain→ sk ──→ tw ──→ NULLS(0)     ┐
桶[1]  ─chain→ NULLS(1)                     ├─ 共享 lock[0]
桶[2]  ─chain→ sk ──→ NULLS(2)             ┘
桶[3]  ─chain→ sk ──→ sk ──→ NULLS(3)     ─── lock[1]
...
桶[N-1]─chain→ NULLS(N-1)                  ─── lock[M-1]

桶索引 = inet_ehashfn(net, 本地IP, 本地端口, 远端IP, 远端端口) & ehash_mask
锁索引 = 同一个hash值 & ehash_locks_mask
```

**关键设计细节**：

- **nulls 链表**：链表尾部不是 NULL，而是编码了桶号的特殊值。RCU 无锁遍历时，如果发现尾部桶号和预期不符，说明节点在遍历过程中被移到了别的桶，需要重新查找。这是一种无锁并发安全保证。
- **分离锁**：锁的数量远少于桶的数量（比如 1024 把锁管理 65536 个桶），每把锁保护 64 个桶。这样锁数组只占几 KB，而不是几百 KB。代价是不同桶可能竞争同一把锁，但由于四元组哈希分布均匀，实际冲突率很低。
- **桶中混合存放** sock 和 inet_timewait_sock，两者都通过 `sock_common` 里的 `skc_nulls_node` 链入。

#### bhash：端口绑定管理表

bind/connect 时需要检查端口是否可用。bhash 是**两级索引**：先按端口找桶，桶内按端口找 `bind_bucket`，每个 `bind_bucket` 挂所有使用该端口的 socket。

```
bhash 桶数组 (inet_bind_hashbucket[])
┌──────────────────────────────────────────────────────────┐
│ 桶[0]: lock + chain                                      │
│        chain → bind_bucket(port=80) → bind_bucket(port=8192) → NULL │
│                  │ fastreuse=1            │ fastreuse=-1            │
│                  │ owners:                │ owners:                 │
│                  │  sk_A(LISTEN) → sk_B   │  sk_C(ESTAB)           │
│                  │  (两个进程都listen:80)  │  (connect用了8192)     │
│                  │                        │                         │
│ 注意: port=80 和 port=8192 之所以在同一个桶, 是因为哈希冲突,     │
│       不是因为它们有关系                                           │
├──────────────────────────────────────────────────────────┤
│ 桶[1]: lock + chain → ...                               │
└──────────────────────────────────────────────────────────┘

桶索引 = (port + net_hash_mix(net)) & (bhash_size - 1)

查找路径:
  1. 计算桶索引 → 定位到桶
  2. 遍历桶内 chain 链表 → 逐个比较 (net, l3mdev, port)
  3. 找到 bind_bucket → 检查 owners 链表中是否有冲突的socket
```

**核心不变量**：每个 `(net, l3mdev, port)` 三元组最多一个 `bind_bucket`。所有创建路径（`inet_csk_get_port`、`__inet_hash_connect`）都遵循"先查找、不存在才创建"。

#### listening_hash 与 lhash2：监听查找表

```
listening_hash (固定32桶, 仅按端口哈希, 旧机制)
┌──────────────────────────────────────────────────────────────┐
│ 桶[80%32=16]: sk(LISTEN 0.0.0.0:80) → sk(LISTEN 10.0.0.1:80)│
│               同端口不同地址全在一桶, 需遍历                     │
│ 缺点: 同一端口大量虚拟主机时, 链表很长                          │
└──────────────────────────────────────────────────────────────┘

lhash2 (动态桶数, 按IP+端口哈希, 新机制)
┌─────────────────────────────────┬─────────────────────────────────┐
│ 桶[hash(0.0.0.0, 80)]          │ 桶[hash(10.0.0.1, 80)]          │
│  → sk(LISTEN 0.0.0.0:80)       │  → sk(LISTEN 10.0.0.1:80)       │
│  (通配地址单独一桶)             │  (具体地址单独一桶, O(1)查找)    │
└─────────────────────────────────┴─────────────────────────────────┘

收包查找策略: 先查 lhash2[hash(具体IP, 端口)] → 没找到 → 再查 lhash2[hash(0.0.0.0, 端口)]
```

### 1.3 socket 在各哈希表中的存在关系

```
状态              bhash    ehash    listening_hash + lhash2
─────────────────────────────────────────────────────────
bind()后            ✓        ✗              ✗
listen()后          ✓        ✗              ✓
connect()后         ✓        ✓              ✗
ESTABLISHED         ✓        ✓              ✗
TIME_WAIT           ✓        ✓              ✗
close()/超时后     全部移除
```

---

### 1.4 socket 类型体系与强转机制

> 理解端口分配流程前，需要先了解 `struct sock` 和 `inet_timewait_sock` 的关系，因为它们共存于 ehash 和 bhash 中。

#### sock_common 伪继承

Linux 内核中不同类型的 socket 结构都以 `struct sock_common` 作为第一个成员（偏移量 0），形成 C 语言的"伪继承"：

```
sock_common (公共基类: 地址、端口、哈希值、链表节点)
    │
    ├── struct sock → inet_sock → inet_connection_sock → tcp_sock
    │   (完整socket, ~2KB+, 用于活跃连接)
    │
    └── struct inet_timewait_sock → tcp_timewait_sock
        (轻量socket, 仅保留哈希表+定时器字段, 用于TIME_WAIT状态)
```

`sock_common` 中包含了哈希表操作所需的全部字段，两种 socket 通过不同别名访问同一偏移：

| sock_common 字段 | sock 别名 | timewait 别名 | 用途 |
|---|---|---|---|
| `skc_addrpair` | `sk_addrpair` | — | 地址对(64位打包, 快速比较) |
| `skc_portpair` | `sk_portpair` | — | 端口对(32位打包, 快速比较) |
| `skc_hash` | `sk_hash` | `tw_hash` | ehash 哈希值 |
| `skc_nulls_node` | `sk_nulls_node` | `tw_node` | ehash 链表节点 |
| `skc_bind_node` | `sk_bind_node` | `tw_bind_node` | bhash 链表节点 |

因此 `inet_twsk()` 可以安全地将 `struct sock *` 强转为 `inet_timewait_sock *`，`INET_MATCH` 宏可以同时匹配两种类型。源码中有明确的互相约束注释，禁止在 `sock_common` 之前添加任何字段。

#### inet_timewait_sock 存在的意义

TIME_WAIT 状态需要持续 2MSL（通常 60 秒），但不需要完整 `tcp_sock` 的全部字段。`inet_timewait_sock` 只保留最小字段集，专有字段包括：

- `tw_timer`：2MSL 定时器
- `tw_tb`：指向 bhash 中的 `inet_bind_bucket`（反向指针）
- `tw_dr`：指向 `inet_timewait_death_row`（TIME_WAIT 全局管理器）

#### inet_timewait_death_row

`inet_timewait_death_row` 管理一个协议中所有 TIME_WAIT socket 的生命周期。TCP 的实例是每个网络命名空间一个（`net->ipv4.tcp_death_row`）。

```c
// include/net/netns/ipv4.h
struct inet_timewait_death_row {
    atomic_t         tw_count;              // 当前TIME_WAIT总数(高频原子操作)
    char             tw_pad[...];           // 缓存行填充, 隔离高频写和低频读
    struct inet_hashinfo *hashinfo;         // 指向tcp_hashinfo, 用于访问ehash/bhash
    int              sysctl_max_tw_buckets; // 上限, 默认=ehash桶数/2
};
```

当 `tw_count >= sysctl_max_tw_buckets` 时，`inet_twsk_alloc()` 拒绝创建新的 TIME_WAIT，直接关闭连接。

---


---

## 2. connect() 端口分配流程

### 2.1 调用链

```
用户空间: connect(sockfd, addr, addrlen)
  → __sys_connect()
    → tcp_v4_connect(sk, uaddr, addr_len)
      // 设置 sk->inet_daddr, sk->inet_dport (远端地址和端口)
      // 计算 port_offset = secure_ipv4_port_ephemeral(rcv_saddr, daddr, dport)
      → inet_hash_connect(&net->ipv4.tcp_death_row, sk)
        → __inet_hash_connect(death_row, sk, port_offset, __inet_check_established)
```

### 2.2 __inet_hash_connect 完整流程

`__inet_hash_connect()` 是端口分配的核心函数（`net/ipv4/inet_hashtables.c`），分两种情况处理：

**情况一：已通过 bind() 绑定端口**

```c
int __inet_hash_connect(struct inet_timewait_death_row *death_row,
        struct sock *sk, u64 port_offset,
        int (*check_established)(...))
{
    int port = inet_sk(sk)->inet_num;  // 已绑定的端口(0表示未绑定)

    if (port) {
        // 定位 bhash 桶, 获取之前 bind() 时关联的 bind_bucket
        head = &hinfo->bhash[inet_bhashfn(net, port, hinfo->bhash_size)];
        tb = inet_csk(sk)->icsk_bind_hash;
        spin_lock_bh(&head->lock);

        // 快速路径: bind_bucket 中只有自己, 无需冲突检查
        if (sk_head(&tb->owners) == sk && !sk->sk_bind_node.next) {
            inet_ehash_nolisten(sk, NULL, NULL);  // 直接加入 ehash
            spin_unlock_bh(&head->lock);
            return 0;
        }
        spin_unlock(&head->lock);
        // 慢速路径: 需要检查四元组是否与已建立连接冲突
        ret = check_established(death_row, sk, port, NULL);
        local_bh_enable();
        return ret;
    }
    // ... 情况二: 自动分配端口
```

**情况二：自动分配端口（核心路径）**

```c
    // 获取临时端口范围, 默认 [32768, 60999]
    inet_get_local_port_range(net, &low, &high);
    high++;                    // 转为左闭右开 [32768, 61000)
    remaining = high - low;
    if (likely(remaining > 1))
        remaining &= ~1U;     // 确保偶数(便于奇偶分离)

    // 计算起始偏移: 结合扰动表和四元组哈希, 实现端口随机化(RFC 6056)
    index = port_offset & (INET_TABLE_PERTURB_SIZE - 1);
    offset = READ_ONCE(table_perturb[index]) + (port_offset >> 32);
    offset %= remaining;
    offset &= ~1U;            // connect()优先选偶数端口, bind()优先选奇数, 减少竞争

other_parity_scan:
    port = low + offset;
    for (i = 0; i < remaining; i += 2, port += 2) {
        if (unlikely(port >= high))
            port -= remaining;             // 环绕处理
        if (inet_is_local_reserved_port(net, port))
            continue;                      // 跳过保留端口

        head = &hinfo->bhash[inet_bhashfn(net, port, hinfo->bhash_size)];
        spin_lock_bh(&head->lock);

        // 在 bhash 桶中查找该端口的 bind_bucket
        inet_bind_bucket_for_each(tb, &head->chain) {
            if (net_eq(ib_net(tb), net) && tb->l3mdev == l3mdev &&
                tb->port == port) {
                // 找到了: 该端口已被使用
                // fastreuse >= 0 表示被 bind() 占用, connect 不能复用
                if (tb->fastreuse >= 0 || tb->fastreuseport >= 0)
                    goto next_port;
                // fastreuse == -1: 被其他 connect() 使用, 检查四元组是否冲突
                if (!check_established(death_row, sk, port, &tw))
                    goto ok;   // 四元组不同, 可以复用同一端口
                goto next_port;
            }
        }
        // 循环结束未找到: 端口全新未使用, 创建新 bind_bucket
        tb = inet_bind_bucket_create(hinfo->bind_bucket_cachep, net, head, port, l3mdev);
        if (!tb) { spin_unlock_bh(&head->lock); return -ENOMEM; }
        tb->fastreuse = -1;        // 标记为 connect() 专用
        tb->fastreuseport = -1;
        goto ok;

    next_port:
        spin_unlock_bh(&head->lock);
        cond_resched();
    }

    // 偶数端口搜完, 尝试奇数端口
    offset++;
    if ((offset & 1) && remaining > 1)
        goto other_parity_scan;
    return -EADDRNOTAVAIL;     // 端口耗尽

ok:
    // 更新扰动表, 增加下次选择的随机性
    i = max_t(int, i, (prandom_u32() & 7) * 2);
    WRITE_ONCE(table_perturb[index], READ_ONCE(table_perturb[index]) + i + 2);

    inet_bind_hash(sk, tb, port);              // 加入 bhash
    if (sk_unhashed(sk)) {
        inet_sk(sk)->inet_sport = htons(port);
        inet_ehash_nolisten(sk, (struct sock *)tw, NULL);  // 加入 ehash
    }
    if (tw) inet_twsk_bind_unhash(tw, hinfo);  // 复用了TIME_WAIT, 解除其bhash绑定
    spin_unlock(&head->lock);
    if (tw) inet_twsk_deschedule_put(tw);       // 释放TIME_WAIT socket
    local_bh_enable();
    return 0;
}
```

#### 图形化流程：自动分配端口全过程

```
输入: sk (未 bind)  +  port_offset (四元组哈希)
  │
  ▼
┌──────────────────────────────────────────────────────────────┐
│ Step 1: 取临时端口范围                                         │
│   inet_get_local_port_range(net, &low, &high)                │
│   默认: low=32768, high=61000 (左闭右开), remaining=28232    │
│   remaining &= ~1U  → 强制偶数 (28232)                        │
└──────────────────────────────────────────────────────────────┘
  │
  ▼
┌──────────────────────────────────────────────────────────────┐
│ Step 2: 扰动表计算起始偏移 (RFC 6056)                          │
│                                                              │
│   table_perturb[65536] (u32 数组, 256 KB)                    │
│   ┌────────────────────────────────────────────────────────┐ │
│   │ [0] [1] [2] ... [index] ... [65535]                    │ │
│   │                  ↓                                      │ │
│   └────────────────────────────────────────────────────────┘ │
│   index = port_offset & 0xFFFF  (相同目的地共享 index)       │
│   offset = table_perturb[index] + (port_offset >> 32)       │
│   offset %= remaining                                        │
│   offset &= ~1U          ← 强制偶数 (connect 用偶数端口)     │
└──────────────────────────────────────────────────────────────┘
  │
  ▼
┌──────────────────────────────────────────────────────────────┐
│ Step 3: 偶数端口扫描循环 (步长 = 2)                            │
│   for i in [0, 2, 4, ..., remaining-2]:                      │
│     port = low + offset;  if port >= high: port -= remaining │
│                                                              │
│   临时端口范围 (仅示意偶数位)                                  │
│   32768  32770  32772  32774  32776  ...  60998              │
│    │      │      │      │      │              │               │
│    ▼      ▼      ▼      ▼      ▼              ▼               │
│   [空闲][占用][保留][占用][空闲]          [空闲]              │
│              ↑                                                  │
│           inet_is_local_reserved_port 跳过                     │
└──────────────────────────────────────────────────────────────┘
  │
  ▼
┌──────────────────────────────────────────────────────────────┐
│ Step 4: 查 bhash 桶 → 三种分支                                │
│   head = &bhash[inet_bhashfn(net, port, bhash_size)]         │
│   在 head->chain 中找 (net, l3mdev, port) 的 bind_bucket     │
└──────────────────────────────────────────────────────────────┘
  │
  ├─── 分支 A: 没找到 tb ──────────────────────────────────────┐
  │    端口全新未用                                              │
  │    tb = inet_bind_bucket_create(...)                         │
  │    tb->fastreuse = -1      ← 标记为 connect() 专用          │
  │    tb->fastreuseport = -1                                    │
  │    ────────────────────────────→ 跳到 ok                    │
  │                                                              │
  ├─── 分支 B: 找到 tb, 但 fastreuse ≥ 0 或 fastreuseport ≥ 0 │
  │    该端口被 bind() 占用 → connect 不能复用                   │
  │    goto next_port          ← 释放锁, 尝试下一个偶数端口    │
  │                                                              │
  └─── 分支 C: 找到 tb, fastreuse == -1                        │
       该端口被其他 connect() 使用, 查 ehash 看四元组冲突       │
       │                                                         │
       ├── check_established() == 0 (无冲突 / TIME_WAIT 可复用) │
       │   ───────────────────────→ 跳到 ok                     │
       │                                                         │
       └── check_established() != 0 (四元组冲突)                │
           goto next_port          ← 尝试下一个偶数端口        │
  │
  ▼ (循环结束仍未找到可用端口)
┌──────────────────────────────────────────────────────────────┐
│ Step 5: 奇偶切换                                              │
│   offset++   (偶数 → 奇数)                                    │
│   if (offset & 1) && remaining > 1:                          │
│       goto other_parity_scan    ← 重新扫描奇数端口            │
│                                                              │
│   偶数轮: 32768, 32770, 32772, ... (已扫尽)                  │
│   奇数轮: 32769, 32771, 32773, ... (再扫一遍)                │
└──────────────────────────────────────────────────────────────┘
  │
  ├─── 仍失败 → return -EADDRNOTAVAIL (端口耗尽)
  │
  ▼ (ok 标签)
┌──────────────────────────────────────────────────────────────┐
│ Step 6: 成功收尾                                              │
│   ① 更新扰动表:                                                │
│      i = max(i, (prandom_u32() & 7) * 2)                     │
│      table_perturb[index] += i + 2   ← 让下次起点偏移         │
│                                                              │
│   ② 加入 bhash:                                                │
│      inet_bind_hash(sk, tb, port)                              │
│                                                              │
│   ③ 加入 ehash:                                                │
│      inet_sk(sk)->inet_sport = htons(port)                     │
│      inet_ehash_nolisten(sk, tw, NULL)                         │
│                                                              │
│   ④ 若复用了 TIME_WAIT:                                        │
│      inet_twsk_bind_unhash(tw)         ← 解除 tw 的 bhash 绑定│
│      inet_twsk_deschedule_put(tw)      ← 释放 tw              │
│                                                              │
│   ⑤ return 0  → connect() 成功                                │
└──────────────────────────────────────────────────────────────┘
```

#### 关键设计意图

| 设计点 | 目的 |
|--------|------|
| **`table_perturb[65536]` 扰动表** | 相同目的地的连续连接共享 index → 端口递增式分配（减少冲突搜索）；不同目的地用不同 index → 端口完全独立（防预测，RFC 6056 §3.3.4） |
| **`offset &= ~1U`（connect 优先偶数）** | connect() 用偶数端口，bind() 优先奇数端口 → 两条路径的端口空间物理隔离，减少扫描冲突 |
| **`fastreuse` 三值 (-1/0/1)** | 让 bhash 在 O(1) 内判断"该端口是否允许 connect 复用"：`-1` 表示 connect 专用（可再查 ehash 复用）；`≥0` 表示 bind 占用（直接跳过） |
| **先扫偶数、再扫奇数（而非交替）** | 偶数端口空间连续，缓存行友好；奇数轮是兜底，多数情况下偶数轮就能找到 |
| **`i = max(i, prandom*2)` 更新扰动表** | 低竞争时 `i` 小，扰动增量大（随机性高）；高竞争时 `i` 大，扰动增量大（避免反复撞同一端口） |

**关于遍历 bhash 桶时只比较一次端口的问题**：找到第一个匹配的 `inet_bind_bucket` 后直接处理而不继续遍历，这不是 bug。因为每个 `(net, l3mdev, port)` 三元组在 bhash 中最多只有一个 `inet_bind_bucket`——所有创建路径（`inet_csk_get_port`、`__inet_hash_connect`、`__inet_inherit_port`）都遵循"先查找、不存在才创建"的模式，保证了唯一性。同一桶中的多个 `inet_bind_bucket` 要么端口不同（哈希冲突），要么网络命名空间不同。

### 2.3 端口选择的安全性设计

`table_perturb` 是一个 65536 条目的 u32 数组（256KB），用于防止攻击者预测端口号（RFC 6056 Section 3.3.4）：

- 相同目的地的连续连接共享同一扰动表索引，端口递增式分配（减少冲突搜索）
- 不同目的地使用不同索引，端口完全独立（攻击者无法从一个连接推断另一个）
- 每次成功分配后更新扰动值，低竞争时随机性高，高竞争时递增


---

## 3. __inet_check_established：四元组冲突检查

### 3.1 这个函数解决什么问题？

TCP 连接由四元组唯一标识：`(本地IP, 本地端口, 远端IP, 远端端口)`。**同一个本地端口可以被多个连接共享**，只要四元组不同。例如：

```
连接1: 192.168.1.100:45678 → 10.0.0.1:8080    ← 四元组A
连接2: 192.168.1.100:45678 → 10.0.0.2:8080    ← 四元组B（远端IP不同）
连接3: 192.168.1.100:45678 → 10.0.0.1:9090    ← 四元组C（远端端口不同）

三条连接共享端口45678, 完全合法!
```

当 `__inet_hash_connect` 发现端口已被其他 `connect()` 使用（`bind_bucket->fastreuse == -1`）时，调用 `__inet_check_established` 到 ehash 中检查四元组是否真的冲突。

### 3.2 执行流程图

```
__inet_check_established(death_row, sk, lport, twp)
│
│  ① 计算四元组哈希 → 定位 ehash 桶
│     hash = inet_ehashfn(net, 本地IP, lport, 远端IP, 远端端口)
│     head = ehash[hash & ehash_mask]
│     lock = ehash_locks[hash & ehash_locks_mask]
│
│  ② spin_lock(lock)  (内层锁, softirq已被外层禁用)
│
│  ③ 遍历 ehash 桶中的链表
│     for (sk2 in head->chain):
│       ├── sk2->sk_hash != hash ?  → 跳过 (快速过滤, 哈希值都不同肯定不匹配)
│       │
│       ├── INET_MATCH(sk2) ?  → 四元组匹配!
│       │     │
│       │     ├── sk2 是 ESTABLISHED/SYN_SENT 等活跃连接
│       │     │     → goto not_unique (冲突, 返回 -EADDRNOTAVAIL)
│       │     │
│       │     └── sk2 是 TIME_WAIT
│       │           → twsk_unique() 检查能否复用
│       │              ├── 可复用 → tw = sk2, break
│       │              └── 不可复用 → goto not_unique
│       │
│       └── 不匹配 → 继续下一个
│
│  ④ 循环正常结束 (无冲突) 或 break (TIME_WAIT复用)
│     设置端口号: inet_num = lport, inet_sport = htons(lport)
│     设置哈希值: sk->sk_hash = hash
│     加入 ehash: __sk_nulls_add_node_rcu(sk, &head->chain)
│     如果复用了 TIME_WAIT:
│       从 ehash 删除旧 tw: sk_nulls_del_node_init_rcu(tw)
│       (先加后删, 保证任何时刻ehash中都有该四元组的有效条目)
│
│  ⑤ spin_unlock(lock)
│     通过 twp 指针返回 tw (调用者负责后续销毁)
│     return 0
```

### 3.3 完整源码

```c
// net/ipv4/inet_hashtables.c
static int __inet_check_established(struct inet_timewait_death_row *death_row,
                                    struct sock *sk, __u16 lport,
                                    struct inet_timewait_sock **twp)
{
    struct inet_hashinfo *hinfo = death_row->hashinfo;
    struct inet_sock *inet = inet_sk(sk);
    __be32 daddr = inet->inet_rcv_saddr;  // 本地IP (ehash命名中叫dest)
    __be32 saddr = inet->inet_daddr;      // 远端IP (ehash命名中叫source)
    int dif = sk->sk_bound_dev_if;
    struct net *net = sock_net(sk);
    int sdif = l3mdev_master_ifindex_by_index(net, dif);

    // 将地址打包为64位、端口打包为32位, INET_MATCH用一条指令比较而非逐字段比4次
    INET_ADDR_COOKIE(acookie, saddr, daddr);
    const __portpair ports = INET_COMBINED_PORTS(inet->inet_dport, lport);

    // 计算四元组哈希 → 定位 ehash 桶和对应的锁
    unsigned int hash = inet_ehashfn(net, daddr, lport, saddr, inet->inet_dport);
    struct inet_ehash_bucket *head = inet_ehash_bucket(hinfo, hash);
    spinlock_t *lock = inet_ehash_lockp(hinfo, hash);
    struct sock *sk2;
    const struct hlist_nulls_node *node;
    struct inet_timewait_sock *tw = NULL;

    // 内层锁: 外层__inet_hash_connect已通过spin_lock_bh禁用softirq, 此处无需再禁
    spin_lock(lock);

    // 遍历 ehash 桶中的 nulls 链表, 查找四元组冲突
    sk_nulls_for_each(sk2, node, &head->chain) {
        if (sk2->sk_hash != hash)       // 哈希值不同 → 四元组必不同, 快速跳过
            continue;
        if (likely(INET_MATCH(net, sk2, acookie, ports, dif, sdif))) {
            // INET_MATCH: 比较 net + portpair(32位) + addrpair(64位) + 接口绑定
            // 四元组完全匹配! 检查对方状态:
            if (sk2->sk_state == TCP_TIME_WAIT) {
                tw = inet_twsk(sk2);    // 强转为 inet_timewait_sock (sock_common偏移相同)
                if (twsk_unique(sk, sk2, twp))
                    break;              // TIME_WAIT 可复用 → 带着 tw 跳出循环
            }
            goto not_unique;            // 活跃连接冲突, 或 TIME_WAIT 不可复用
        }
    }

    // 到达此处: 循环自然结束(四元组唯一) 或 break(TIME_WAIT复用成功)
    // 必须先设端口再加入ehash: 加入后其他CPU立即可通过RCU看到, 端口未设会导致INET_MATCH误判
    inet->inet_num = lport;
    inet->inet_sport = htons(lport);
    sk->sk_hash = hash;
    WARN_ON(!sk_unhashed(sk));
    __sk_nulls_add_node_rcu(sk, &head->chain);  // RCU方式插入ehash

    if (tw) {
        // 原子替换: 先插入新sk再删除旧tw, 确保并发查找不会看到"两者都不在"的空窗口
        sk_nulls_del_node_init_rcu((struct sock *)tw);
        __NET_INC_STATS(net, LINUX_MIB_TIMEWAITRECYCLED);
    }
    spin_unlock(lock);
    sock_prot_inuse_add(sock_net(sk), sk->sk_prot, 1);

    // twp: 调用者传入的输出参数, 用于延迟销毁TIME_WAIT
    // 调用者__inet_hash_connect还持有bhash锁, 不能在持锁时销毁tw(销毁需操作bhash)
    // 所以通过twp传回去, 调用者释放bhash锁后再调用 inet_twsk_bind_unhash + deschedule_put
    if (twp) {
        *twp = tw;
    } else if (tw) {
        inet_twsk_deschedule_put(tw);   // 直接调用场景, 没有外层锁, 立即销毁
    }
    return 0;

not_unique:
    spin_unlock(lock);
    return -EADDRNOTAVAIL;
}
```

### 3.4 举例说明

```
场景: 客户端 192.168.1.100 要连接 10.0.0.1:8080, 被分配端口 45678

ehash 桶 [hash(192.168.1.100, 45678, 10.0.0.1, 8080) & mask] 中当前有:

  sk_A: 192.168.1.100:45678 → 10.0.0.2:8080  (ESTABLISHED)
        → sk_hash 不同(远端IP不同, 哈希值不同), 跳过

  sk_B: 192.168.1.100:45678 → 10.0.0.1:8080  (TIME_WAIT)
        → sk_hash 相同, INET_MATCH 匹配!
        → 状态是 TIME_WAIT, 调用 twsk_unique() 检查
        → tcp_tw_reuse=1, TIME_WAIT已存在3秒 > 1秒, 双方有时间戳
        → 可复用! tw = sk_B, break

  结果: 新 socket 加入 ehash, sk_B(TIME_WAIT) 从 ehash 删除, 端口 45678 复用成功
```

---


---

## 4. TIME_WAIT 复用机制

### 4.1 为什么 TIME_WAIT 需要复用？

TIME_WAIT 存在的目的是防止旧连接的延迟报文被新连接错误接收（详见 `TCP四次挥手与close.md`）。但 TIME_WAIT 持续 60 秒，在高并发场景会产生问题：

```
场景: 反向代理(如HAProxy)连接后端服务器

HAProxy (192.168.1.100)  →  Backend (10.0.0.1:8080)

每秒 1000 个短连接, 每个连接完成后进入 TIME_WAIT (60秒)
→ 同时存在的 TIME_WAIT = 1000 × 60 = 60,000 个
→ 临时端口范围 [32768, 60999] 只有 28,232 个端口
→ 端口耗尽! connect() 返回 -EADDRNOTAVAIL

而且: 这些 TIME_WAIT 的四元组都是 (192.168.1.100:X → 10.0.0.1:8080)
目的地相同, 端口不能在 bhash 层面复用, 必须在 ehash 层面复用四元组
```

### 4.2 TIME_WAIT 的创建与销毁

当正常连接进入 TIME_WAIT 状态时，`inet_twsk_hashdance()` 将轻量级的 `inet_timewait_sock` 替换原始 `sock` 加入哈希表：

```c
// net/ipv4/inet_timewait_sock.c
void inet_twsk_hashdance(struct inet_timewait_sock *tw, struct sock *sk,
                         struct inet_hashinfo *hashinfo)
{
    // Step 1: 加入 bhash (复用原socket的bind_bucket)
    tw->tw_tb = icsk->icsk_bind_hash;
    inet_twsk_add_bind_node(tw, &tw->tw_tb->owners);

    // Step 2: 加入 ehash
    inet_twsk_add_node_rcu(tw, &ehead->chain);

    // Step 3: 从 ehash 删除原始 socket
    __sk_nulls_del_node_init_rcu(sk);

    // 引用计数 = 3: bhash(1) + ehash(1) + timer(1)
    refcount_set(&tw->tw_refcnt, 3);
}
```

定时器到期后，`inet_twsk_kill()` 从 ehash 和 bhash 中删除 tw，递减 `death_row->tw_count`，释放内存。

---

### 4.3 复用的安全性问题——为什么不能无条件复用？

TIME_WAIT 的核心作用是防止"旧报文污染新连接"：

```
时间线:
────────────────────────────────────────────────────
旧连接: A:45678 → B:8080, 序列号空间 [1000, 50000]
  │  发送了一个数据包 seq=30000, 但它在网络中延迟了
  │  连接正常关闭, 进入 TIME_WAIT
  │
如果立即复用同一四元组建立新连接:
  │
新连接: A:45678 → B:8080, 序列号空间 [1, 60000]
  │  延迟的旧包 seq=30000 到达!
  │  新连接的接收窗口包含 30000 → 接收了旧数据!
  │  → 数据损坏 ✗
```

**安全复用的两个条件**：

1. **双方支持 TCP 时间戳**：时间戳单调递增，新连接的时间戳必然大于旧连接。接收方通过 PAWS (Protection Against Wrapped Sequences) 机制，会丢弃时间戳小于当前记录的报文。这样旧连接的延迟报文（时间戳旧）会被新连接自动丢弃。

2. **TIME_WAIT 已存在超过 1 秒**：确保对端也观察到了时间戳的前进。如果复用太快（比如 0.1 秒），新旧连接的时间戳可能相同（时间戳精度为秒级），PAWS 无法区分。

### 4.4 tcp_twsk_unique 复用条件源码

```c
// net/ipv4/tcp_ipv4.c
int tcp_twsk_unique(struct sock *sk, struct sock *sktw, void *twp)
{
    int reuse = READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_tw_reuse);
    const struct tcp_timewait_sock *tcptw = tcp_twsk(sktw);

    // sysctl开关: 0=禁止(默认), 1=允许, 2=仅loopback
    // loopback没有物理网络, 不存在"延迟报文在路由器缓冲"的问题, 所以reuse=2更安全
    if (reuse == 2) {
        if (!loopback) reuse = 0;  // 非loopback → 降级为禁止
    }

    // 核心条件:
    //   tw_ts_recent_stamp != 0 → 旧连接双方都支持TCP时间戳 (PAWS可以工作)
    //   time_after32(now, stamp) → TIME_WAIT已存在超过1秒 (确保对端时间戳前进)
    if (tcptw->tw_ts_recent_stamp &&
        (!twp || (reuse && time_after32(ktime_get_seconds(),
                                        tcptw->tw_ts_recent_stamp)))) {
        if (likely(!tp->repair)) {
            // 继承序列号: 旧连接最后序列号 + 65535 + 2, 跳过一个完整窗口
            // 确保新序列号远大于旧连接的任何在途报文
            tp->write_seq = tcptw->tw_snd_nxt + 65535 + 2;

            // 继承对端时间戳: 作为新连接PAWS检查的起始基线
            // 新连接知道"对端的时间戳至少是X", 小于X的旧报文一律丢弃
            tp->rx_opt.ts_recent = tcptw->tw_ts_recent;
            tp->rx_opt.ts_recent_stamp = tcptw->tw_ts_recent_stamp;
        }
        sock_hold(sktw);  // 增加tw引用计数, 调用者负责后续销毁
        return 1;          // 可复用
    }
    return 0;              // 不可复用
}
```

### 4.5 复用的完整流程时序

```
                  __inet_hash_connect
                  │
                  │  发现端口45678的bind_bucket, fastreuse=-1(被connect占用)
                  │  调用 __inet_check_established(death_row, sk, 45678, &tw)
                  │
                  ▼
              __inet_check_established
              │
              │  ehash中找到匹配四元组, 状态=TIME_WAIT
              │  调用 twsk_unique(sk, tw_sk, twp)
              │    → tcp_twsk_unique()
              │       tcp_tw_reuse=1 ✓
              │       tw存在3秒 > 1秒 ✓
              │       双方有时间戳 ✓
              │       → 复用成功, 继承序列号和时间戳
              │
              │  新sk加入ehash → 旧tw从ehash删除 (原子替换)
              │  return 0, *twp = tw
              │
              ▼
          回到 __inet_hash_connect
              │
              │  inet_bind_hash(sk, tb, 45678)    → sk加入bhash
              │  inet_ehash_nolisten(sk, tw, ...)  → (已在check_established中加入)
              │
              │  释放 bhash 桶锁
              │
              │  inet_twsk_bind_unhash(tw, hinfo)  → tw从bhash解除
              │  inet_twsk_deschedule_put(tw)       → 取消tw的2MSL定时器 + 释放内存
              │
              │  return 0  → connect() 成功!
```

`sysctl_tcp_tw_reuse` 取值：0=禁止复用（默认）、1=允许、2=仅loopback。


---

## 5. 端口复用三机制对比：SO_REUSEADDR / SO_REUSEPORT / tcp_tw_reuse

`SO_REUSEADDR`、`SO_REUSEPORT`、`tcp_tw_reuse` 三者名字相似，但**作用层面、生效时机、解决的问题完全不同**。理解它们的区别是理解 TCP 端口管理的关键：

- `SO_REUSEADDR` / `SO_REUSEPORT`：在 **bind() 路径**（bhash 层）工作，是**端口级**复用，由 socket 自身通过 `setsockopt` 设置
- `tcp_tw_reuse`：在 **connect() 路径**（ehash 层）工作，是**四元组级**复用，由 sysctl 全局控制

> 源码路径分别在 `inet_csk_get_port()`（bind 路径，详见 [TCP-bind操作.md](TCP-bind操作.md) §五）和 `__inet_check_established()`（connect 路径，详见本文 §3）。本节聚焦**机制对比**，不重复源码细节。

### 5.1 三者对比

| 维度 | SO_REUSEADDR | SO_REUSEPORT | tcp_tw_reuse |
|---|---|---|---|
| **设置方式** | `setsockopt(SO_REUSEADDR)` | `setsockopt(SO_REUSEPORT)` | `sysctl net.ipv4.tcp_tw_reuse` |
| **内核字段** | `sk->sk_reuse` | `sk->sk_reuseport` | `net->ipv4.sysctl_tcp_tw_reuse` |
| **生效路径** | `bind()` | `bind()` | `connect()` |
| **检查位置** | bhash（端口绑定表） | bhash（端口绑定表） | ehash（已建立连接表） |
| **比较粒度** | 端口 | 地址 + 端口 | 四元组 |
| **对端要求** | 新旧 socket 都需设置 | 新旧 socket 都需设置 | 单方 sysctl，无需对端配合 |
| **状态限制** | 旧 socket 不能是 LISTEN | 无限制（含 LISTEN） | 旧连接必须是 TIME_WAIT |
| **安全机制** | 无 | 同 UID 检查 | PAWS 时间戳 + 1秒延迟 |
| **典型场景** | 服务器重启快速绑定旧端口 | 多进程监听同一端口负载均衡 | 高并发客户端临时端口耗尽 |

### 5.2 SO_REUSEADDR 的源码路径

用户调用 `setsockopt(SO_REUSEADDR)` 时，内核设置 `sk->sk_reuse = SK_CAN_REUSE`（`net/core/sock.c`）。

`bind()` 路径中，`inet_csk_get_port()` 检查端口是否可复用：

```c
// net/ipv4/inet_connection_sock.c - inet_csk_get_port()
bool reuse = sk->sk_reuse && sk->sk_state != TCP_LISTEN;  // LISTEN状态不参与reuse

if (!hlist_empty(&tb->owners)) {
    if (sk->sk_reuse == SK_FORCE_REUSE)   // 内核强制复用(如TCP repair)
        goto success;
    if (tb->fastreuse > 0 && reuse)       // 快速路径: 桶内所有socket都可reuse
        goto success;
    if (inet_csk_bind_conflict(sk, tb, true, true))  // 慢速路径: 逐一检查冲突
        goto fail_unlock;
}
```

`inet_csk_bind_conflict()` 的冲突判定逻辑：

```c
// net/ipv4/inet_connection_sock.c - inet_csk_bind_conflict()
sk_for_each_bound(sk2, &tb->owners) {
    if (sk != sk2 && 设备匹配) {
        if (reuse && sk2->sk_reuse && sk2->sk_state != TCP_LISTEN) {
            // 双方都设置了REUSEADDR且都不在LISTEN → 允许(不break, 继续检查)
            // 但如果地址完全相同(inet_rcv_saddr_equal)，在非relax模式下仍冲突
        } else {
            // 有一方未设置REUSEADDR → 地址相同则冲突
            if (inet_rcv_saddr_equal(sk, sk2, true))
                break;  // 冲突
        }
    }
}
```

核心规则：**双方都设置了 `SO_REUSEADDR`，且旧 socket 不在 LISTEN 状态，且绑定地址不完全相同时，允许复用端口**。典型场景是服务器 crash 后重启，旧连接处于 TIME_WAIT/FIN_WAIT 等状态，新服务器进程可以立即 `bind()` 同一端口。

### 5.3 SO_REUSEPORT 的源码路径

用户调用 `setsockopt(SO_REUSEPORT)` 时，内核设置 `sk->sk_reuseport = 1`（`net/core/sock.c`）。

`bind()` 路径中，`inet_csk_get_port()` 通过 `sk_reuseport_match()` 快速检查：

```c
// net/ipv4/inet_connection_sock.c - sk_reuseport_match()
static inline int sk_reuseport_match(struct inet_bind_bucket *tb, struct sock *sk)
{
    if (tb->fastreuseport <= 0)   return 0;  // 桶内有不支持reuseport的socket
    if (!sk->sk_reuseport)        return 0;  // 新socket未设置reuseport
    if (rcu_access_pointer(sk->sk_reuseport_cb))  return 0;  // 已有BPF程序
    if (!uid_eq(tb->fastuid, uid))  return 0;  // UID不同(安全检查)
    // STRICT模式还需比较地址
    return 1;
}
```

`inet_csk_bind_conflict()` 中的 reuseport 逻辑：

```c
// 双方都设置了reuseport，且UID相同(或对端是TIME_WAIT) → 允许
if (!reuseport_ok || !reuseport || !sk2->sk_reuseport || !reuseport_cb_ok ||
    (sk2->sk_state != TCP_TIME_WAIT && !uid_eq(uid, sock_i_uid(sk2)))) {
    if (inet_rcv_saddr_equal(sk, sk2, true))
        break;  // 冲突
}
// 否则: 不break, 允许复用
```

核心规则：**双方都设置了 `SO_REUSEPORT`，且属于同一用户（UID 相同），允许绑定完全相同的地址+端口，包括 LISTEN 状态**。典型场景是 Nginx 多 worker 进程各自 `bind()` + `listen()` 同一个 80 端口，内核通过 `reuseport_cb` 实现连接的负载均衡分发。

### 5.4 tcp_tw_reuse 的源码路径

管理员通过 `sysctl net.ipv4.tcp_tw_reuse=1` 设置，内核存储在 `net->ipv4.sysctl_tcp_tw_reuse`。

`connect()` 路径中，`__inet_hash_connect()` → `__inet_check_established()` → `twsk_unique()` → `tcp_twsk_unique()`：

```c
// net/ipv4/tcp_ipv4.c - tcp_twsk_unique()
int reuse = READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_tw_reuse);
// 检查条件: 双方支持时间戳 && (reuse开启 && TIME_WAIT已存在>1秒)
if (tcptw->tw_ts_recent_stamp &&
    (!twp || (reuse && time_after32(now, tcptw->tw_ts_recent_stamp)))) {
    tp->write_seq = tcptw->tw_snd_nxt + 65535 + 2;  // 继承序列号
    return 1;  // 可复用
}
```

核心规则：**sysctl 开启，TIME_WAIT 已存在超过 1 秒，双方支持 TCP 时间戳（PAWS 保证安全），允许 `connect()` 复用 TIME_WAIT 的四元组**。典型场景是反向代理（如 HAProxy）大量短连接到后端，临时端口被 TIME_WAIT 耗尽。

### 5.5 三者互不干扰的原因

三个机制在代码中完全独立，不存在交叉影响：

1. **`connect()` 路径不受 SO_REUSEADDR/SO_REUSEPORT 影响**：`__inet_hash_connect()` 创建 `bind_bucket` 时将 `fastreuse` 和 `fastreuseport` 设为 `-1`（connect 专用标记）。遍历 bhash 时，如果发现 `fastreuse >= 0`（被 `bind()` 占用），直接跳过该端口（`goto next_port`），不检查 `sk_reuse` 或 `sk_reuseport`。

2. **`bind()` 路径不受 tcp_tw_reuse 影响**：`inet_csk_get_port()` 在 bhash 层面检查端口冲突，不调用 `tcp_twsk_unique()`。即使 `tcp_tw_reuse=1`，`bind()` 到一个被 TIME_WAIT 占用的端口仍需要 `SO_REUSEADDR`。

3. **tcp_tw_reuse 在 ehash 层面工作**：它判断的是四元组级别的 TIME_WAIT 复用，而 `SO_REUSEADDR/SO_REUSEPORT` 在 bhash 层面判断端口级别的绑定冲突。


---

## 6. socket 在哈希表中的生命周期


以客户端 `192.168.1.100` 连接 `10.0.0.1:8080`，分配端口 `45678` 为例：

```
分配完成后, socket 同时存在于 bhash 和 ehash:

bhash[inet_bhashfn(net, 45678, bhash_size)]:
    └── inet_bind_bucket (port=45678, fastreuse=-1)
            └── owners: sk

ehash[inet_ehashfn(net, 192.168.1.100, 45678, 10.0.0.1, 8080) & ehash_mask]:
    └── sk (sk_hash=hash, state=TCP_SYN_SENT)
```

同一客户端再连接 `10.0.0.2:8080`，四元组不同，可以复用端口 45678：

```
bhash 中同一个 bind_bucket:
    └── inet_bind_bucket (port=45678)
            └── owners: sk1 → sk2    (两个socket共享端口)

ehash 中不同的桶(四元组不同, 哈希值不同):
    ehash[hash1]: sk1 (192.168.1.100:45678 → 10.0.0.1:8080)
    ehash[hash2]: sk2 (192.168.1.100:45678 → 10.0.0.2:8080)
```

---



### 6.1 单个 socket 的双哈希表链接

一个已建立连接的 socket 通过 `sock_common` 中的不同字段同时链入 ehash 和 bhash：

```
                struct sock
                ┌────────────────────────────────────┐
                │ sock_common:                       │
                │   skc_nulls_node ────────────────┐ │ → ehash_bucket.chain
                │   skc_bind_node  ──────────────┐ │ │ → bind_bucket.owners
                │   skc_hash, skc_addrpair, ...  │ │ │
                │                                │ │ │
                │ inet_connection_sock:           │ │ │
                │   icsk_bind_hash ──→ bind_bucket│ │ │ (反向指针)
                └────────────────────────────────┼─┼─┘
                                                 │ │
                     bhash                       │ │       ehash
                ┌────────────┐                   │ │  ┌────────────┐
                │bind_bucket │                   │ │  │ehash_bucket│
                │ owners: ───┼───────────────────┘ └──┼── chain    │
                │ port: 45678│                        └────────────┘
                └────────────┘
```

### 6.2 连接生命周期中的哈希表迁移

```
bind()         → bhash: [bind_bucket] → owners: [sk]
                 ehash: (不在)

connect()成功  → bhash: [bind_bucket] → owners: [sk]
                 ehash: [ehash_bucket] → chain: [sk]

ESTABLISHED    → (同上, 状态变为ESTABLISHED)

→ TIME_WAIT    → bhash: [bind_bucket] → owners: [tw]  (sk已从owners删除)
(hashdance)      ehash: [ehash_bucket] → chain: [tw]  (sk已从chain删除)

→ 2MSL超时     → bhash: (tw已删除, bind_bucket可能被销毁)
(twsk_kill)      ehash: (tw已删除)
```

---


---

## 附录 A. spin_lock_bh 与 spin_lock 的使用

> 本节是并发背景知识，从主线 §2 移出，避免打断 connect 端口分配的叙事。


### 先理解背景：谁会访问哈希表？

Linux 中有两种执行上下文会操作 TCP 哈希表：

```
执行上下文1：进程上下文（可被抢占）
  用户调用 connect() → 内核在进程上下文中执行 __inet_hash_connect()
  → 操作 bhash 和 ehash

执行上下文2：软中断上下文（softirq，优先级更高）
  网卡收到数据包 → 触发 NET_RX_SOFTIRQ → tcp_v4_rcv()
  → __inet_lookup_established() 读取 ehash
  → 软中断可以打断进程上下文！
```

### 如果只用普通 spin_lock 会怎样？——死锁场景

```
            CPU 0 时间线
            ─────────────────────────────────────────────
时刻1       进程上下文: connect()
            │  spin_lock(&bhash_bucket->lock)  ← 成功获取锁
            │  正在操作 bhash...
            │
时刻2       ↓ 这时网卡收到一个包，触发软中断！
            ┌─ 软中断: tcp_v4_rcv()
            │  需要查 ehash，如果恰好也要获取同一把锁...
            │  spin_lock(&bhash_bucket->lock)  ← 永远拿不到！
            │  ┌──────────────────────────────────────┐
            │  │ 死锁！                                │
            │  │ 软中断等锁 → 但锁被进程持有            │
            │  │ 进程等软中断结束 → 但软中断在等锁       │
            │  │ 同一个 CPU 上，谁也无法前进             │
            │  └──────────────────────────────────────┘
            └─ (永远不会返回)
```

关键：**软中断可以在同一个 CPU 上抢占进程上下文**。如果进程持锁时被软中断打断，而软中断又要获取同一把锁，就形成了单 CPU 上的自死锁。

### spin_lock_bh 的解决方案

`spin_lock_bh` = `local_bh_disable()` + `spin_lock()`，即**先禁止本 CPU 的软中断，再获取锁**：

```c
// include/linux/spinlock_api_smp.h
static inline void __raw_spin_lock_bh(raw_spinlock_t *lock)
{
    __local_bh_disable_ip(_RET_IP_, SOFTIRQ_LOCK_OFFSET);  // 禁止本CPU软中断
    spin_acquire(&lock->dep_map, 0, 0, _RET_IP_);
    LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);  // 获取锁
}
```

加了这层保护后，时序变为：

```
            CPU 0 时间线
            ─────────────────────────────────────────────
时刻1       进程上下文: connect()
            │  spin_lock_bh(&bhash_bucket->lock)
            │    ├── local_bh_disable()  ← 本CPU软中断被屏蔽
            │    └── spin_lock()  ← 获取锁成功
            │  正在操作 bhash...
            │
时刻2       ↓ 网卡收到包，想触发软中断
            ✗ 软中断被禁止了，暂时挂起，不会执行
            │
时刻3       │  spin_unlock_bh(&bhash_bucket->lock)
            │    ├── spin_unlock()  ← 释放锁
            │    └── local_bh_enable()  ← 恢复软中断
            │
时刻4       ┌─ 软中断: tcp_v4_rcv()  ← 现在才执行
            │  spin_lock(&ehash_lock)  ← 成功，没有冲突
            └─ 正常完成
```

### 为什么 __inet_check_established 内部只用 spin_lock？

```
调用链:
  __inet_hash_connect()
    spin_lock_bh(&bhash_bucket->lock)  ← 外层锁，已禁用softirq
    │
    ├── __inet_check_established()
    │     spin_lock(&ehash_lock)       ← 内层锁，不需要再禁用softirq
    │     spin_unlock(&ehash_lock)     （因为外层已经禁用了，这里重复禁用没意义）
    │
    spin_unlock_bh(&bhash_bucket->lock) ← 释放外层锁 + 恢复softirq
```

`local_bh_disable` 是**嵌套计数**的（引用计数机制），但 `__inet_check_established` 作为内层调用，外层已经保证了 softirq 被禁用，所以用普通 `spin_lock` 即可，避免不必要的 `bh_disable/enable` 开销。

---


---

## 附录 B. 源码文件索引


| 文件 | 关键内容 |
|---|---|
| `include/net/inet_hashtables.h` | `inet_hashinfo`、`inet_bind_bucket`、`inet_bhashfn`、`INET_MATCH` |
| `net/ipv4/inet_hashtables.c` | `__inet_hash_connect`、`__inet_check_established`、`inet_bind_hash` |
| `include/net/inet_sock.h` | `__inet_ehashfn` (Jenkins hash) |
| `include/net/sock.h` | `sock_common`、`struct sock` |
| `include/net/inet_timewait_sock.h` | `inet_timewait_sock` |
| `net/ipv4/inet_timewait_sock.c` | `inet_twsk_hashdance`、`inet_twsk_kill` |
| `include/net/netns/ipv4.h` | `inet_timewait_death_row` |
| `net/ipv4/tcp_ipv4.c` | `tcp_twsk_unique` (TIME_WAIT复用条件, tcp_tw_reuse实现) |
| `net/core/sock.c` | `setsockopt` 处理 `SO_REUSEADDR`→`sk_reuse`、`SO_REUSEPORT`→`sk_reuseport` |
| `include/net/timewait_sock.h` | `twsk_unique` (间接调用层) |
| `include/linux/jhash.h` | `jhash_3words` (Jenkins哈希算法) |
| `net/ipv4/inet_connection_sock.c` | `inet_csk_get_port` (bind路径端口分配) |
