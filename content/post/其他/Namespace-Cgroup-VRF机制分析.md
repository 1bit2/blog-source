+++
date = '2026-04-30'
title = 'Namespace、Cgroup、VRF 机制分析'
weight = 3
tags = [
    "namespace",
    "cgroup",
    "VRF",
    "nsproxy",
    "css_set",
    "l3mdev",
    "容器",
    "资源隔离",
    "ip-rule",
    "policy-routing",
    "sock_net",
    "dev_net",
]
categories = [
    "其他",
]
+++
# Namespace、Cgroup、VRF 机制分析

> 基于 Linux 5.15.78，分析三大资源管控机制的作用、生效时机、进程归属方式，
> 以及三者在网络场景下的协作关系。

---

## 一、总体定位

```
┌──────────────────────────────────────────────────────────────────┐
│                        Linux 内核资源管控体系                      │
├──────────────┬───────────────────┬───────────────────────────────┤
│  Namespace   │      Cgroup       │            VRF                │
│  ─────────── │  ─────────────── │  ───────────────────────────  │
│  "看到什么"  │   "能用多少"      │   "走哪条路"                  │
│  资源隔离    │   资源限制/计量   │   路由隔离                    │
│  (视图层面)  │   (配额层面)      │   (同一netns内FIB隔离)        │
├──────────────┼───────────────────┼───────────────────────────────┤
│ 层级: 内核   │ 层级: 内核        │ 层级: L3路由                  │
│ 粒度: 进程   │ 粒度: 进程        │ 粒度: 网络接口                │
│ 附着: nsproxy│ 附着: css_set     │ 附着: net_device(l3mdev_ops)  │
└──────────────┴───────────────────┴───────────────────────────────┘
```

| | Namespace | Cgroup | VRF |
|---|-----------|--------|-----|
| **解决问题** | 资源隔离(视图) | 资源限制(配额) | 路由隔离 |
| **绑定对象** | 进程(`nsproxy`) | 进程(`css_set`) | 网络接口(`net_device`) |
| **继承方式** | fork时继承/`CLONE_NEW*`新建 | fork时继承父进程cgroup | Socket绑定设备 |
| **网络影响** | 完全独立的网络栈 | 给socket打标签/设优先级 | 同一网络栈内路由隔离 |
| **隔离粒度** | 全栈隔离(设备/IP/路由/iptables) | 仅限制/标记 | 仅路由表隔离 |
| **创建方式** | `clone(CLONE_NEWNET)`/`ip netns add` | `mkdir /sys/fs/cgroup/XXX` | `ip link add type vrf` |
| **典型场景** | 容器(Docker/K8s) | 资源限制(内存/CPU) | 多租户路由(运营商/企业网) |

---

## 二、Namespace — "进程看到什么"

### 2.1 八种 Namespace

Linux 5.15 支持 8 种命名空间，每种隔离一类系统资源的 **可见性**：

| Namespace | CLONE标志 | 隔离内容 | 内核结构 |
|-----------|----------|---------|---------|
| Mount | `CLONE_NEWNS` | 文件系统挂载点 | `mnt_namespace` |
| UTS | `CLONE_NEWUTS` | 主机名、域名 | `uts_namespace` |
| IPC | `CLONE_NEWIPC` | SystemV IPC、POSIX消息队列 | `ipc_namespace` |
| PID | `CLONE_NEWPID` | 进程ID编号空间 | `pid_namespace` |
| Network | `CLONE_NEWNET` | 网络栈(设备、路由、iptables、socket) | `struct net` |
| User | `CLONE_NEWUSER` | UID/GID映射 | `user_namespace` |
| Cgroup | `CLONE_NEWCGROUP` | cgroup根视图 | `cgroup_namespace` |
| Time | `CLONE_NEWTIME` | 单调时钟/启动时钟偏移 | `time_namespace` |

### 2.2 核心数据结构 — `nsproxy`（代理模式）

每个进程通过 `task_struct->nsproxy` 指向自己的命名空间集合。`nsproxy` 本质上是**代理模式（Proxy Pattern）**——不同隔离域的进程持有不同的 `nsproxy` 实例，每个实例内的指针指向各自的 namespace 对象：

```c
// include/linux/nsproxy.h:31
struct nsproxy {
    atomic_t count;                          // 引用计数
    struct uts_namespace  *uts_ns;            // 主机名隔离
    struct ipc_namespace  *ipc_ns;            // IPC隔离
    struct mnt_namespace  *mnt_ns;            // 挂载点隔离
    struct pid_namespace  *pid_ns_for_children; // 子进程的PID空间
    struct net            *net_ns;            // ★ 网络命名空间
    struct time_namespace *time_ns;           // 当前时间空间
    struct time_namespace *time_ns_for_children; // 子进程的时间空间
    struct cgroup_namespace *cgroup_ns;       // cgroup根视图
};
```

```
进程A (宿主机)                             进程B (容器)
  │                                          │
  └→ nsproxy_1                               └→ nsproxy_2
       ├── net_ns → init_net                      ├── net_ns → container_net
       ├── uts_ns → 宿主机hostname                ├── uts_ns → "container-1"
       └── pid_ns → 全局PID空间                   └── pid_ns → 容器PID空间
```

`struct net` 是 Network Namespace 的实体，包含独立的设备列表、路由表、iptables 规则、socket 哈希表等完整网络栈。

### 2.3 进程创建时的 Namespace 归属

```
clone()/fork()
  → kernel_clone()         // kernel/fork.c
    → copy_process()
      → copy_namespaces(clone_flags, p)   // kernel/nsproxy.c:151
```

#### `copy_namespaces()` 核心逻辑

```c
// kernel/nsproxy.c:151
int copy_namespaces(unsigned long flags, struct task_struct *tsk)
{
    struct nsproxy *old_ns = tsk->nsproxy;

    // ★ 快速路径：没有任何 CLONE_NEW* 标志
    //   → 子进程共享父进程的 nsproxy（引用计数+1）
    if (likely(!(flags & (CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC |
                          CLONE_NEWPID | CLONE_NEWNET |
                          CLONE_NEWCGROUP | CLONE_NEWTIME)))) {
        get_nsproxy(old_ns);
        return 0;
    }

    // 需要 CAP_SYS_ADMIN 权限
    if (!ns_capable(user_ns, CAP_SYS_ADMIN))
        return -EPERM;

    // ★ 慢速路径：创建新 nsproxy
    new_ns = create_new_namespaces(flags, tsk, user_ns, tsk->fs);
    tsk->nsproxy = new_ns;
    return 0;
}
```

#### `create_new_namespaces()` — 逐个检查标志

```c
// kernel/nsproxy.c:67
static struct nsproxy *create_new_namespaces(unsigned long flags, ...)
{
    new_nsp = create_nsproxy();  // 分配新 nsproxy

    // 每个 copy_xxx 检查对应的 CLONE_NEW* 标志：
    //   有标志 → 创建全新的namespace
    //   无标志 → 引用父进程的namespace（引用计数+1）
    new_nsp->mnt_ns  = copy_mnt_ns(flags, ...);     // CLONE_NEWNS?
    new_nsp->uts_ns  = copy_utsname(flags, ...);     // CLONE_NEWUTS?
    new_nsp->ipc_ns  = copy_ipcs(flags, ...);        // CLONE_NEWIPC?
    new_nsp->pid_ns_for_children = copy_pid_ns(flags, ...); // CLONE_NEWPID?
    new_nsp->cgroup_ns = copy_cgroup_ns(flags, ...); // CLONE_NEWCGROUP?
    new_nsp->net_ns  = copy_net_ns(flags, ...);      // CLONE_NEWNET?
    new_nsp->time_ns_for_children = copy_time_ns(flags, ...); // CLONE_NEWTIME?
    return new_nsp;
}
```

### 2.4 三种进入 Namespace 的方式

| 方式 | 系统调用 | 时机 | 内核入口 |
|-----|---------|------|---------|
| **继承** | `clone(CLONE_NEW*)` | 子进程创建时 | `copy_namespaces()` |
| **脱离** | `unshare(CLONE_NEW*)` | 当前进程运行中 | `ksys_unshare()` → `unshare_nsproxy_namespaces()` |
| **加入** | `setns(fd, nstype)` | 加入已有namespace | `SYSCALL_DEFINE2(setns)` → `commit_nsset()` |

#### `unshare()` 调用链

```
unshare(CLONE_NEWNET)
  → ksys_unshare()                    // kernel/fork.c:3621
    → unshare_nsproxy_namespaces()    // kernel/nsproxy.c
      → create_new_namespaces(flags, ...)
    → switch_task_namespaces(current, new_nsproxy)
```

#### `setns()` 调用链

```
setns(fd, CLONE_NEWNET)
  → SYSCALL_DEFINE2(setns)            // kernel/nsproxy.c:527
    → prepare_nsset()                 // 克隆当前 nsproxy
    → validate_ns(&nsset, ns)         // 安装目标 namespace
    → commit_nsset()                  // 原子切换
      → switch_task_namespaces(current, new_nsproxy)
```

### 2.5 nsproxy 何时起作用——每次系统调用

nsproxy 不是在某个特殊时刻才起作用。**进程每次做系统调用时，内核都通过 `current->nsproxy` 决定"这个进程看到哪个世界"**。

关键示例——创建 socket（`net/socket.c:1558`）：

```c
int sock_create(int family, int type, int protocol, struct socket **res)
{
    return __sock_create(current->nsproxy->net_ns, family, type, protocol, res, 0);
}
```

各 namespace 的典型引用位置：

| 进程操作 | 内核引用路径 | 源码位置 |
|---------|------------|---------|
| `socket()` | `current->nsproxy->net_ns` → 在对应网络栈中创建 | `net/socket.c:1558` |
| `gethostname()` | `current->nsproxy->uts_ns` → 返回对应 hostname | `kernel/sys.c` |
| `mount()` | `current->nsproxy->mnt_ns` → 在对应挂载空间操作 | `fs/namespace.c` |
| `fork()` | `current->nsproxy->pid_ns_for_children` → 分配 PID | `kernel/pid.c` |
| `shmget()` | `current->nsproxy->ipc_ns` → 查找 IPC 对象 | `ipc/shm.c` |

内核对象创建后会把 `net` 缓存到自身，后续通过 `sock_net(sk)` / `dev_net(dev)` 访问，不再每次查 nsproxy：

```c
// include/net/sock.h:2684
static inline struct net *sock_net(const struct sock *sk)
{
    return read_pnet(&sk->sk_net);
}

// include/linux/netdevice.h:2460
static inline struct net *dev_net(const struct net_device *dev)
{
    return read_pnet(&dev->nd_net);
}
```

### 2.6 Network Namespace 对网络栈的影响

```
struct net 包含：
├── dev_base_head        — 独立的网络设备列表
├── ipv4.fib_table_hash  — 独立的IPv4路由表
├── ipv4.rules_ops       — 独立的路由策略规则
├── ipv4.iptable_filter  — 独立的iptables规则
├── ipv4.tcp_death_row   — 独立的TIME_WAIT管理
├── ipv4.tcp_sk          — 独立的TCP控制socket
└── loopback_dev         — 独立的lo设备
```

每个 Network Namespace 是一个 **完全独立的网络栈实例**。

---

## 三、Cgroup — "进程能用多少"

### 3.1 控制器列表

Cgroup 不隔离资源可见性，而是对进程组施加 **资源限制和统计**：

```c
// include/linux/cgroup_subsys.h — 控制器注册
SUBSYS(cpuset)    // 绑定CPU核
SUBSYS(cpu)       // CPU时间配额(CFS带宽)
SUBSYS(cpuacct)   // CPU使用统计
SUBSYS(io)        // 块设备I/O带宽
SUBSYS(memory)    // 内存用量上限(OOM)
SUBSYS(devices)   // 设备访问白名单
SUBSYS(freezer)   // 冻结/恢复进程组
SUBSYS(net_cls)   // 给socket打classid标签
SUBSYS(net_prio)  // 给socket设优先级
SUBSYS(perf_event)// perf事件
SUBSYS(hugetlb)   // 大页限制
SUBSYS(pids)      // 最大进程数
SUBSYS(rdma)      // RDMA资源限制
SUBSYS(misc)      // 杂项资源
```

### 3.2 核心数据结构

```
task_struct
  └── cgroups (RCU指针) ────→ struct css_set
                                ├── subsys[CGROUP_SUBSYS_COUNT]
                                │    ├── [cpu_cgrp_id]    → cpu 的 cgroup_subsys_state
                                │    ├── [memory_cgrp_id] → memory 的 css
                                │    ├── [net_cls_cgrp_id]→ net_cls 的 classid
                                │    └── ...
                                ├── tasks             — 使用此css_set的任务列表
                                ├── dfl_cgrp          — 默认层级的cgroup
                                └── nr_tasks          — 任务计数
```

```c
// include/linux/sched.h:1203 — task_struct 中的 cgroup 字段
#ifdef CONFIG_CGROUPS
    struct css_set __rcu  *cgroups;   // 指向当前cgroup成员关系
    struct list_head      cg_list;    // 链接到css_set->tasks
#endif
```

```c
// include/linux/cgroup-defs.h:199 — css_set核心字段
struct css_set {
    struct cgroup_subsys_state *subsys[CGROUP_SUBSYS_COUNT]; // 每个控制器的状态
    refcount_t refcount;
    struct css_set *dom_cset;     // 域cgroup的css_set
    struct cgroup *dfl_cgrp;      // 关联的默认cgroup
    int nr_tasks;                 // 内部任务计数
    struct list_head tasks;       // 使用此cset的任务链表
    struct list_head mg_tasks;    // 正在迁移的任务
};
```

### 3.3 进程创建时的 Cgroup 归属

```
copy_process()
  │
  ├── cgroup_fork(p)              // kernel/cgroup/cgroup.c:6123
  │     // 初始化：暂时指向 init_css_set
  │     RCU_INIT_POINTER(child->cgroups, &init_css_set);
  │     INIT_LIST_HEAD(&child->cg_list);
  │
  ├── cgroup_can_fork(p, args)    // kernel/cgroup/cgroup.c:6282
  │     // 1. cgroup_css_set_fork(): 查找父进程的css_set
  │     // 2. 各控制器的 can_fork() 回调（如pids检查进程数上限）
  │     // 失败则fork被拒绝
  │
  ├── sched_cgroup_fork(p)        // 放到正确的cgroup运行队列
  │
  └── cgroup_post_fork(p, args)   // kernel/cgroup/cgroup.c:6344
        // ★ 最终归属：将子进程移入父进程的css_set
        css_set_move_task(child, NULL, cset, false);
        // cset 来自 cgroup_css_set_fork()，即父进程的cgroup
```

**核心规则**：子进程 **默认继承父进程所在的 cgroup**。

### 3.4 运行时迁移

```bash
echo $PID > /sys/fs/cgroup/memory/container1/cgroup.procs
```

```
→ cgroup_attach_task(dst_cgrp, leader, threadgroup)  // kernel/cgroup/cgroup.c:2824
  → cgroup_migrate_add_src()     // 收集源css_set
  → cgroup_migrate_prepare_dst() // 准备目标css_set
  → cgroup_migrate()             // 原子迁移所有线程
```

### 3.5 Cgroup 对网络的影响

Cgroup 不隔离网络栈，但通过两个控制器给 socket **打标签**：

#### net_cls — classid 标签

```c
// net/core/netclassid_cgroup.c
// 给进程组的所有socket打上classid
// tc filter / iptables -m cgroup --cgroup 可以匹配

static void cgrp_attach(struct cgroup_taskset *tset)
{
    cgroup_taskset_for_each(p, css, tset) {
        // 遍历进程的所有fd，给socket设置classid
        update_classid_task(p, css_cls_state(css)->classid);
    }
}
```

#### net_prio — 优先级

```c
// net/core/netprio_cgroup.c
// 给每个(cgroup, 网卡)组合设定发送优先级

static void net_prio_attach(struct cgroup_taskset *tset)
{
    cgroup_taskset_for_each(p, css, tset) {
        iterate_fd(p->files, 0, update_netprio, (void *)css->id);
        // 遍历进程的所有fd，给socket设置prioidx
    }
}
```

---

## 四、VRF — "同一网络空间内走哪条路"

> VRF = **Virtual Routing and Forwarding**（虚拟路由与转发）。
> 概念来自传统路由器厂商（Cisco/Juniper），Linux 4.3 内核通过 L3 master device 机制引入支持（`drivers/net/vrf.c`）。

**本质定位**：VRF 是 **ip rule + 多路由表 + SO_BINDTODEVICE 的自动化封装**，不是新机制。VRF 创建时自动注册一条 `FRA_L3MDEV` 类型的 fib rule，底层仍依赖 ip rule 机制：

```c
// drivers/net/vrf.c:1564 — vrf_fib_rule()
// VRF 设备 up 时自动调用，为当前 netns 注册 l3mdev 策略规则
static int vrf_fib_rule(const struct net_device *dev, __u8 family, bool add_it)
{
    // ...
    frh->action = FR_ACT_TO_TBL;
    nla_put_u8(skb, FRA_L3MDEV, 1);             // ★ 自动创建的 rule
    nla_put_u32(skb, FRA_PRIORITY, FIB_RULE_PREF); // 优先级 1000
    // ...
}
```

这条 rule 等效于用户手动执行 `ip rule add l3mdev lookup table ...`，区别在于 VRF 让内核自动完成 rule 注册、路由表选择和 socket 绑定。

### 4.1 设计动机——ip rule 已经能做多路由域，为什么还要 VRF？

Linux 内核本身就支持多路由表 + `ip rule` 策略路由，**可以实现多个独立路由域**：

```bash
# 创建两张路由表，ip rule 按接口选表——这不需要 VRF
ip route add 10.0.0.0/24 via 10.0.0.254 table 10
ip route add 10.0.0.0/24 via 10.0.0.254 table 20
ip rule add iif eth1 lookup 10
ip rule add iif eth2 lookup 20
```

**转发流量没问题**。那 VRF 到底多做了什么？不同方案解决不同程度的问题：

#### 方案一：ip rule (iif) — 只覆盖转发流量

`iif` 匹配"包从哪个接口进来"。对路由器转发的流量有效。但本机发起的包（如 `ping`）没有入接口，`iif` 规则不匹配——内核不知道该查哪张表。

#### 方案二：bind 真实网口 + oif rule — 覆盖本机发包

```bash
ip rule add oif eth1 lookup 10    # 本机发包时按出口接口选表
ip rule add oif eth2 lookup 20
setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, "eth1", 4);
```

**大部分场景确实可以工作**，包括转发和本机发包。

需要注意的是：ehash **桶索引**仅由四元组计算（`inet_ehashfn`），但 `INET_MATCH()` 的完整匹配**会检查** `sk_bound_dev_if`（通过 `inet_sk_bound_dev_eq`）：

```c
// include/net/inet_hashtables.h — INET_MATCH()
static inline bool INET_MATCH(struct net *net, const struct sock *sk,
                              const __addrpair cookie, const __portpair ports,
                              int dif, int sdif)
{
    if (!net_eq(sock_net(sk), net) ||
        sk->sk_portpair != ports ||
        sk->sk_addrpair != cookie)
        return false;

    // ★ 会检查接口绑定
    return inet_sk_bound_dev_eq(net, READ_ONCE(sk->sk_bound_dev_if), dif, sdif);
}
```

所以 **bind 到不同物理网口的 socket，在 ehash 中可以通过接口区分**。这意味着方案二在大部分场景下是可行的。但管理负担较重：每个应用都要 `SO_BINDTODEVICE`，每对接口都要维护 iif/oif rule。

#### 方案三：VRF — 自动化 + 简化管理

VRF 相比方案二的主要优势：

1. **自动路由表选择**：VRF 创建时自动注册 `FRA_L3MDEV` 策略规则，slave 接口的流量自动走对应路由表，不需要手写 iif/oif rule
2. **对应用透明**：`ip vrf exec` 自动设置 `SO_BINDTODEVICE`，应用无需修改代码
3. **收包路径一致性**：`vrf_ip_rcv()` 统一将 `skb->dev` 改为 VRF master，收包时 `dif` 和 `sdif` 都能正确匹配 socket

#### 如何选择？

| 场景 | 推荐方案 |
|---|---|
| 简单的多路由表，网段不重叠 | ip rule 足够 |
| 网段不重叠，但想简化管理 | VRF（省去 rule 维护） |
| 网段重叠，只有转发流量 | ip rule (iif) 足够 |
| 网段重叠，有本机发起流量 | bind + oif rule 可行，VRF 管理更简单 |
| 大规模多租户/多接口隔离 | VRF（rule/bind 管理成本过高） |

### 4.2 VRF 与路由表的关系——严格一对一

**一个 VRF 只绑定一张路由表**。创建 VRF 时通过 `table` 参数指定，之后不可更改：

```bash
ip link add vrf-red type vrf table 10    # vrf-red ↔ table 10，一对一
```

内核中，`vrf_fib_table()` 直接返回创建时绑定的 `tb_id`（`drivers/net/vrf.c:1225`）：

```c
static u32 vrf_fib_table(const struct net_device *dev)
{
    struct net_vrf *vrf = netdev_priv(dev);
    return vrf->tb_id;
}
```

**但一个 VRF 可以挂多个网口**。多个物理接口可以作为同一个 VRF 的 slave，共享同一张路由表：

```bash
ip link set eth1 master vrf-red    # eth1 加入 vrf-red，走 table 10
ip link set eth2 master vrf-red    # eth2 也加入 vrf-red，也走 table 10
```

```
┌─── Network Namespace (struct net) ──────────────────────────┐
│                                                              │
│  ┌── VRF "red" (tb_id=10) ──┐  ┌── VRF "blue" (tb_id=20) ─┐│
│  │ eth1 ──→ FIB表 10        │  │ eth3 ──→ FIB表 20         ││
│  │ eth2 ──→ FIB表 10        │  │ 10.0.0.0/8 → gateway_b   ││
│  │ 10.0.0.0/8 → gateway_a   │  └───────────────────────────┘│
│  └───────────────────────────┘                               │
│                                                              │
│  ┌── default (tb_id=254) ───┐                               │
│  │ eth0 ──→ main FIB表      │                               │
│  └───────────────────────────┘                               │
└──────────────────────────────────────────────────────────────┘
```

### 4.3 核心数据结构

VRF 是一个 **L3 master 虚拟网络设备**，通过 `l3mdev_ops` 介入路由：

```c
// drivers/net/vrf.c
struct net_vrf {
    struct rtable __rcu  *rth;    // IPv4路由缓存
    struct rt6_info __rcu *rt6;   // IPv6路由缓存
    u32                  tb_id;   // ★ 关联的FIB路由表ID（一对一）
    int                  ifindex; // VRF设备的ifindex
};
```

```c
// include/net/l3mdev.h — L3 master device 操作表
struct l3mdev_ops {
    u32 (*l3mdev_fib_table)(const struct net_device *dev); // 返回FIB表ID
    struct sk_buff *(*l3mdev_l3_rcv)(...);   // 收包钩子
    struct sk_buff *(*l3mdev_l3_out)(...);   // 发包钩子
};

// drivers/net/vrf.c — VRF注册的ops
static const struct l3mdev_ops vrf_l3mdev_ops = {
    .l3mdev_fib_table  = vrf_fib_table,
    .l3mdev_l3_rcv     = vrf_l3_rcv,
    .l3mdev_l3_out     = vrf_l3_out,
};
```

### 4.4 VRF 的创建与绑定

VRF **不绑定到进程**，而是绑定到 **网络接口**：

```bash
# 创建VRF设备并关联路由表
ip link add vrf-red type vrf table 10
ip link set vrf-red up

# 将物理接口加入VRF
ip link set eth1 master vrf-red
# → eth1 成为 vrf-red 的 slave，路由查找走 table 10
```

### 4.5 应用程序如何使用 VRF

| 方式 | 应用程序是否需要修改 | 适用场景 |
|---|---|---|
| `ip vrf exec vrf-a ./app` | **不需要** | 整个进程的所有流量走同一个 VRF |
| `setsockopt(SO_BINDTODEVICE, "vrf-a")` | 需要知道 VRF 设备名 | 一个进程内不同 socket 走不同 VRF |
| 不做任何处理 | 不需要 | 流量走默认路由表（不属于任何 VRF） |

`ip vrf exec` 在启动进程前设置 `SO_BINDTODEVICE`，对应用程序完全透明：

```bash
# 同一个 nginx，不改代码，分别服务两个路由域
ip vrf exec vrf-a nginx -c /etc/nginx/customer_a.conf
ip vrf exec vrf-b nginx -c /etc/nginx/customer_b.conf
```

一个进程内需要同时访问多个路由域时，用 `setsockopt`：

```c
int fd_a = socket(AF_INET, SOCK_STREAM, 0);
setsockopt(fd_a, SOL_SOCKET, SO_BINDTODEVICE, "vrf-a", 6);

int fd_b = socket(AF_INET, SOCK_STREAM, 0);
setsockopt(fd_b, SOL_SOCKET, SO_BINDTODEVICE, "vrf-b", 6);
// fd_a connect(10.0.0.50) → table 10 → 客户A
// fd_b connect(10.0.0.50) → table 20 → 客户B
```

### 4.6 VRF 在内核中的生效路径

#### 发送路径 — Socket 绑定 VRF

```c
// 用户态
setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, "vrf-red", 8);

// 内核 net/core/sock.c
sk->sk_bound_dev_if = ifindex;  // 保存VRF设备的ifindex
```

路由查找时，`sk_bound_dev_if` 决定走哪个 FIB 表：

```c
// net/ipv4/fib_rules.c:90 — __fib_lookup()
l3mdev_update_flow(net, flowi4_to_flowi(flp));
// → 如果 flowi4_oif 指向 VRF slave 设备，
//   替换为 VRF master 的 ifindex
err = fib_rules_lookup(net->ipv4.rules_ops, ...);
// → 匹配到 VRF 的路由表
```

```c
// net/l3mdev/l3mdev.c:271 — l3mdev_update_flow()
void l3mdev_update_flow(struct net *net, struct flowi *fl)
{
    if (fl->flowi_oif) {
        dev = dev_get_by_index_rcu(net, fl->flowi_oif);
        ifindex = l3mdev_master_ifindex_rcu(dev);
        if (ifindex) {
            fl->flowi_oif = ifindex;  // 替换为VRF master
            fl->flowi_flags |= FLOWI_FLAG_SKIP_NH_OIF;
        }
    }
}
```

#### 收包路径 — VRF 在 L3 层介入

调用链：`ip_rcv` → `ip_rcv_core`（`IPCB(skb)->iif = skb->skb_iif`，保存原始 slave ifindex）→ PREROUTING → `ip_rcv_finish` → `l3mdev_ip_rcv` → `vrf_ip_rcv`。

```c
// drivers/net/vrf.c:1444 — vrf_ip_rcv()
static struct sk_buff *vrf_ip_rcv(struct net_device *vrf_dev,
                                  struct sk_buff *skb)
{
    skb->dev = vrf_dev;                     // 入接口改为VRF master
    skb->skb_iif = vrf_dev->ifindex;        // skb_iif 改为 VRF master
    IPCB(skb)->flags |= IPSKB_L3SLAVE;     // 标记来自L3 slave
    // ★ 注意：IPCB(skb)->iif 保持不变（仍是原始 slave ifindex）
    // 后续 inet_sdif(skb) 依赖 IPSKB_L3SLAVE 标记返回原始 slave ifindex
    skb = vrf_rcv_nfhook(NFPROTO_IPV4, NF_INET_PRE_ROUTING, skb, vrf_dev);
    return skb;
}
```

`vrf_ip_rcv()` **不设置路由表 ID**。路由表选择在后续的 `__fib_lookup()` 中由 `l3mdev_update_flow()` + `l3mdev_fib_rule_match()` 配合完成（见发送路径）。

#### Socket 匹配 — inet_sk_bound_dev_eq（非 sk_dev_equal_l3scope）

TCP/UDP 收包时的 socket 匹配通过 `INET_MATCH()` → `inet_sk_bound_dev_eq()` 完成：

```c
// include/net/inet_sock.h:144
static inline bool inet_bound_dev_eq(bool l3mdev_accept, int bound_dev_if,
                                     int dif, int sdif)
{
    if (!bound_dev_if)
        return !sdif || l3mdev_accept;      // 未绑定：非 VRF 包或允许跨域
    return bound_dev_if == dif || bound_dev_if == sdif;
    // 已绑定：匹配 VRF master ifindex(dif) 或原始 slave ifindex(sdif)
}
```

收包时参数来源（`tcp_v4_rcv` 为例）：
- `dif = inet_iif(skb)` → `skb->skb_iif`（已被 `vrf_ip_rcv` 改为 **VRF master ifindex**）
- `sdif = inet_sdif(skb)` → 检查 `IPSKB_L3SLAVE` 后返回 `IPCB(skb)->iif`（**原始 slave ifindex**）

所以 socket 绑定 VRF master 设备名时 `bound_dev_if == dif` 匹配；绑定物理网口名时 `bound_dev_if == sdif` 匹配。

> **注意**：`sk_dev_equal_l3scope()` 是另一个辅助函数，仅用于 **IPv6 connect/setsockopt/sendmsg** 中校验出接口与 socket 绑定设备的 L3 域一致性，**不参与** ehash/listen hash/UDP hash 的收包匹配。

### 4.7 全景：本机如何区分相同网段两个网口的流量

VRF 的价值在于让路由表选择渗透到内核网络栈的每一个环节，形成完整的隔离链条：

```
               eth1 (master=vrf-a)          eth2 (master=vrf-b)
                 │                            │
                 ▼                            ▼
收包      skb->dev = vrf-a              skb->dev = vrf-b
                 │                            │
路由      table 10                       table 20
                 │                            │
socket    匹配绑定 vrf-a 的 socket       匹配绑定 vrf-b 的 socket
                 │                            │
应用      app_a (ip vrf exec vrf-a)     app_b (ip vrf exec vrf-b)
                 │                            │
发包      flowi_oif = vrf-a              flowi_oif = vrf-b
                 │                            │
出口      table 10 → eth1               table 20 → eth2
```

- **收包**：`vrf_ip_rcv()` 将 `skb->dev`/`skb_iif` 改为 VRF master，`IPCB->iif` 保持原始 slave → 后续 `__fib_lookup()` 中 `l3mdev_update_flow()` + `l3mdev_fib_rule_match()` 选对路由表
- **socket 匹配**：`inet_sk_bound_dev_eq()` 比较 `sk_bound_dev_if` 与 `dif`（VRF master）或 `sdif`（原始 slave），让绑定 VRF 的 socket 只接收对应接口的包
- **发包**：`l3mdev_update_flow()` 将 `flowi_oif` 替换为 VRF master 的 ifindex → `l3mdev_fib_rule_match()` 自动选对路由表

三条路径每一步都通过 VRF 设备来区分流量。VRF 相比手动 ip rule 的核心优势在于：**自动注册 `FRA_L3MDEV` 策略规则**，将路由表选择从需要人工维护的 iif/oif rule 变成了内核自动完成的逻辑。

### 4.8 VRF 与 Network Namespace 的关系

- VRF 的 per-netns 数据通过 `register_pernet_subsys(&vrf_net_ops)` 注册
- 每个 Network Namespace 有独立的 VRF 映射表 `struct netns_vrf`
- VRF **不替代** Namespace，而是在 Namespace **内部** 细分路由

```c
// drivers/net/vrf.c:102
struct netns_vrf {
    bool add_fib_rules;        // 是否已添加默认FIB规则
    struct vrf_map vmap;       // VRF table_id → ifindex 映射
};
```

### 4.9 三种方案对比

| 能力 | ip rule (iif) | bind 真实网口 + oif rule | VRF |
|------|--------------|------------------------|-----|
| 转发流量选路由表 | `iif` rule 可做 | `iif` rule 可做 | 自动（`FRA_L3MDEV` rule） |
| 本机发包选路由表 | **不行**（iif 无效） | `oif` rule + bind 可做 | 自动 |
| listener socket 区分 | 不行 | `sk_bound_dev_if` 可做 | 自动 |
| 已建立连接 socket 区分 | 不行 | `inet_sk_bound_dev_eq` 可做 | 自动 |
| 管理复杂度 | N 组 iif rule | N 组 iif+oif rule + 每个 app bind | N 个 VRF 设备 |
| 应用程序改动 | 不需要 | 每个 app 需要 `SO_BINDTODEVICE` | 不需要（`ip vrf exec` 透明） |

**说明**：经源码验证，`INET_MATCH()` 在 ehash 查找时通过 `inet_sk_bound_dev_eq()` **会检查** `sk_bound_dev_if`，所以 bind 到不同接口的 socket 在 ehash 中可以区分。VRF 相比方案二的核心优势不在于"解决 ehash 冲突"（方案二已经能做），而在于**自动化和管理成本**——无需手写 rule、无需每个应用都改代码。

### 4.10 实战场景：两个网口连相同网段但链路不通的路由器

```
         ┌──────────┐
    eth1──┤  本机    ├──eth2
         └──────────┘
           │              │
     ┌─────┴─────┐  ┌─────┴─────┐
     │ 路由器 A   │  │ 路由器 B   │
     │10.0.0.0/24│  │10.0.0.0/24│
     └───────────┘  └───────────┘
         (链路不通)
```

不用 VRF 时，两条 `10.0.0.0/24` 路由在 main 表中冲突。VRF 方案：

```bash
# 1. 创建两个 VRF
ip link add vrf-a type vrf table 10
ip link add vrf-b type vrf table 20
ip link set vrf-a up
ip link set vrf-b up

# 2. 分别绑定网口
ip link set eth1 master vrf-a
ip link set eth2 master vrf-b

# 3. 各自配置 IP 和路由（可以用相同网段）
ip addr add 10.0.0.1/24 dev eth1
ip addr add 10.0.0.1/24 dev eth2
ip route add default via 10.0.0.254 table 10
ip route add default via 10.0.0.254 table 20

# 4. 本机分别访问两个路由器
ip vrf exec vrf-a ping 10.0.0.100    # → 走 eth1 → 路由器A
ip vrf exec vrf-b ping 10.0.0.100    # → 走 eth2 → 路由器B

# 5. 应用程序绑定 VRF
ip vrf exec vrf-a ./my_app           # my_app 所有流量走路由器A
ip vrf exec vrf-b ./my_app           # my_app 所有流量走路由器B
```

路由器 A 的链路故障 **不影响** 通过 vrf-b 到路由器 B 的通信，因为两者走完全不同的路由表。

---

## 五、层级关系与协作

```
Network Namespace (最大的网络隔离边界)
  ├── 独立的设备列表、路由表、iptables规则
  │
  └── VRF (Namespace内部的路由细分)
        ├── VRF "red" → FIB表10（可挂多个网口）
        └── VRF "blue" → FIB表20（可挂多个网口）

Cgroup (正交于上面两者，控制"量"而非"可见性")
  ├── net_cls: 给socket打classid → tc/iptables按组限速
  └── net_prio: 给socket设优先级 → 网卡按优先级调度
```

### 5.1 在收包路径中的生效位置

```
网卡 → 硬中断 → NAPI poll
  │
  ├── Network Namespace: dev_net(dev) 确定 struct net
  │     → 决定进入哪个网络栈(路由表、iptables、socket哈希表)
  │
  ├── VRF: l3mdev_l3_rcv() 在 ip_rcv() 之后
  │     → 替换 skb->dev 为 VRF master
  │     → 路由查找使用 VRF 关联的 FIB 表
  │
  └── Cgroup: socket 匹配后
        → sk->sk_cgrp_data 包含 classid/prioidx
        → tc/netfilter 使用这些标签做流量控制
```

### 5.2 典型使用场景

| 场景 | 使用的机制 | 原因 |
|------|-----------|------|
| Docker容器网络 | Network Namespace | 每个容器需要独立IP、路由、端口空间 |
| K8s Pod资源限制 | Cgroup (memory/cpu/pids) | 限制Pod的CPU/内存/进程数 |
| 容器网络带宽限制 | Cgroup (net_cls) + tc | classid标记 + tc限速 |
| 简单多路由表（网段不重叠） | ip rule + 多路由表 | 足够简单，不需要 VRF |
| 运营商多租户路由（网段重叠） | VRF | 自动路由表选择 + socket L3 scope 隔离 |
| 管理网/业务网分离 | VRF 或 ip rule | VRF 管理更简单（`ip vrf exec`） |
| 双网口连同网段设备 | VRF | 相同IP网段 + 本机发包 + 可能的四元组冲突 |

### 5.3 类比总结

- **Namespace** = 不同的房子（看不到彼此的东西）
- **Cgroup** = 同一栋楼的水电配额（限制每户用多少）
- **VRF** = 同一座城市的不同道路系统（同一个地址走不同的路）

---

## 六、相关系统参数

| 参数 | 说明 |
|------|------|
| `/proc/$PID/ns/` | 查看进程的各namespace文件描述符 |
| `/proc/$PID/cgroup` | 查看进程的cgroup归属 |
| `/sys/fs/cgroup/` | cgroup 文件系统挂载点 |
| `ip netns list` | 列出所有Network Namespace |
| `ip vrf show` | 列出所有VRF及其路由表 |
| `ip link show type vrf` | 列出VRF设备 |
| `/proc/sys/net/ipv4/tcp_l3mdev_accept` | 允许跨VRF接受TCP连接 |

---

## 七、调试工具

```bash
# Namespace
nsenter -t $PID -n            # 进入进程的网络namespace
lsns                          # 列出所有namespace
unshare --net bash            # 创建新网络namespace

# Cgroup
cat /proc/$PID/cgroup         # 查看进程cgroup
systemd-cgls                  # 树形显示cgroup层级
cat /sys/fs/cgroup/memory/XXX/memory.usage_in_bytes

# VRF
ip vrf exec vrf-red ping 10.0.0.1  # 在VRF中执行命令
ip route show table 10              # 查看VRF路由表
ip rule show                        # 查看路由策略规则
```
