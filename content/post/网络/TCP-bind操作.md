+++
date = '2026-04-28'
title = 'TCP Bind 操作源码分析'
weight = 5
tags = [
    "TCP",
    "bind",
    "inet_bind",
    "SO_REUSEADDR",
    "SO_REUSEPORT",
    "socket",
]
categories = [
    "网络",
]
+++
# TCP Bind 操作源码分析

> 基于 Linux 5.15.78，分析 `bind()` 系统调用从用户态到内核的完整路径：系统调用入口、地址校验、权限检查、BPF 钩子、状态机约束。
>
> 端口分配算法（`inet_csk_get_port`、`inet_csk_find_open_port`）、bhash 哈希表结构、冲突检测（`inet_csk_bind_conflict`）的详细分析参见 [TCP 端口分配与哈希表](TCP端口分配与哈希表.md)。

---

## 目录

- [一、全景调用链](#一全景调用链)
- [二、系统调用入口：`__sys_bind()`](#二系统调用入口__sys_bind)
- [三、协议层入口：`inet_bind()`](#三协议层入口inet_bind)
- [四、核心绑定逻辑：`__inet_bind()`](#四核心绑定逻辑__inet_bind)
- [五、SO_REUSEADDR 与 SO_REUSEPORT](#五so_reuseaddr-与-so_reuseport)
- [六、SO_BINDTODEVICE 与 bind 的交互](#六so_bindtodevice-与-bind-的交互)
- [七、常见错误与排查](#七常见错误与排查)

---

## 一、全景调用链

```
用户态: bind(sockfd, addr, addrlen)
    │
    ▼
__sys_bind()                            [net/socket.c:1757]
    ├── sockfd_lookup_light()            // fd → struct socket
    ├── move_addr_to_kernel()            // 用户地址拷贝到内核
    ├── security_socket_bind()           // LSM 安全检查
    └── sock->ops->bind()                // → inet_bind()
        │
        ▼
    inet_bind()                         [net/ipv4/af_inet.c:495]
        ├── BPF_CGROUP_RUN_PROG_INET_BIND_LOCK()   // cgroup-BPF pre-bind
        └── __inet_bind()                          // 核心：地址校验 + 端口绑定
            │
            ▼
        sk->sk_prot->get_port()         // → inet_csk_get_port()
            │
            ├─ 快路径: tb->fastreuse / sk_reuseport_match → success
            │          (SO_REUSEADDR / SO_REUSEPORT 命中)
            │
            └─ 慢路径: inet_csk_bind_conflict()   // 逐一检查 owners 链
                       · sk_bound_dev_if 分域 (§六.1)
                       · sk_reuse + sk_state != TCP_LISTEN
                       · sk_reuseport + uid_eq
        │
        ▼
    BPF_CGROUP_RUN_PROG_INET4_POST_BIND()          // cgroup-BPF post-bind
```

> `__inet_bind()` 内部 10 步细节（地址族验证、FIB 查类型、特权端口、`lock_sock`/`release_sock`、状态检查、saddr 设置）详见 [§四](#四核心绑定逻辑__inet_bind)。
>
> `sk_bound_dev_if` 在 bind 生命周期的 4 处读取（端口冲突分域 / VRF FIB / 路由 oif / 收包匹配）详见 [§六](#六so_bindtodevice-与-bind-的交互)。
>
> 端口获取的深层细节（bhash 查找 / 冲突检测 / 自动分配 / TIME_WAIT 复用）详见 [TCP 端口分配与哈希表](TCP端口分配与哈希表.md)。

---

## 二、系统调用入口：`__sys_bind()`

```c
// net/socket.c:1757-1782
int __sys_bind(int fd, struct sockaddr __user *umyaddr, int addrlen)
{
    struct socket *sock;
    struct sockaddr_storage address;
    int err, fput_needed;

    sock = sockfd_lookup_light(fd, &err, &fput_needed);
    if (sock) {
        err = move_addr_to_kernel(umyaddr, addrlen, &address);
        if (!err) {
            err = security_socket_bind(sock, (struct sockaddr *)&address, addrlen);
            if (!err)
                err = sock->ops->bind(sock, (struct sockaddr *)&address, addrlen);
        }
        fput_light(sock->file, fput_needed);
    }
    return err;
}
```

流程：fd 查找 → 地址拷贝 → LSM 检查 → 协议族 bind。

---

## 三、协议层入口：`inet_bind()`

```c
// net/ipv4/af_inet.c:495-515
int inet_bind(struct socket *sock, struct sockaddr *uaddr, int addr_len)
{
    struct sock *sk = sock->sk;
    u32 flags = BIND_WITH_LOCK;
    int err;

    // RAW socket 有自己的 bind 实现
    if (sk->sk_prot->bind)
        return sk->sk_prot->bind(sk, uaddr, addr_len);

    if (addr_len < sizeof(struct sockaddr_in))
        return -EINVAL;

    // cgroup-BPF pre-bind 钩子：容器场景可修改绑定行为
    // 可设置 BIND_NO_CAP_NET_BIND_SERVICE 旁路特权端口检查
    err = BPF_CGROUP_RUN_PROG_INET_BIND_LOCK(sk, uaddr,
                                              CGROUP_INET4_BIND, &flags);
    if (err)
        return err;

    return __inet_bind(sk, uaddr, addr_len, flags);
}
```

---

## 四、核心绑定逻辑：`__inet_bind()`

```c
// net/ipv4/af_inet.c:530-624
int __inet_bind(struct sock *sk, struct sockaddr *uaddr, int addr_len, u32 flags)
{
    struct sockaddr_in *addr = (struct sockaddr_in *)uaddr;
    struct inet_sock *inet = inet_sk(sk);
    struct net *net = sock_net(sk);
    unsigned short snum;
    int chk_addr_ret;
    u32 tb_id = RT_TABLE_LOCAL;
    int err;

    // ① 地址族验证：必须 AF_INET（兼容 AF_UNSPEC + INADDR_ANY）
    if (addr->sin_family != AF_INET) {
        err = -EAFNOSUPPORT;
        if (addr->sin_family != AF_UNSPEC ||
            addr->sin_addr.s_addr != htonl(INADDR_ANY))
            goto out;
    }

    // ② 地址类型检查：通过 FIB 路由表判断是否为本地地址
    tb_id = l3mdev_fib_table_by_index(net, sk->sk_bound_dev_if) ? : tb_id;
    chk_addr_ret = inet_addr_type_table(net, addr->sin_addr.s_addr, tb_id);

    // ③ 非本地地址绑定控制
    // 默认只能绑定本机 IP，IP_FREEBIND/IP_TRANSPARENT 可绑定任意 IP
    err = -EADDRNOTAVAIL;
    if (!inet_can_nonlocal_bind(net, inet) &&
        addr->sin_addr.s_addr != htonl(INADDR_ANY) &&
        chk_addr_ret != RTN_LOCAL &&
        chk_addr_ret != RTN_MULTICAST &&
        chk_addr_ret != RTN_BROADCAST)
        goto out;

    // ④ 特权端口检查：< 1024 需要 CAP_NET_BIND_SERVICE
    snum = ntohs(addr->sin_port);
    err = -EACCES;
    if (!(flags & BIND_NO_CAP_NET_BIND_SERVICE) &&
        snum && inet_port_requires_bind_service(net, snum) &&
        !ns_capable(net->user_ns, CAP_NET_BIND_SERVICE))
        goto out;

    // ★ lock_sock 受 BIND_WITH_LOCK 标志控制，避免 BPF/内核调用路径重复加锁
    if (flags & BIND_WITH_LOCK)
        lock_sock(sk);

    // ⑤ 状态检查：必须 TCP_CLOSE 且未绑定过
    err = -EINVAL;
    if (sk->sk_state != TCP_CLOSE || inet->inet_num)
        goto out_release_sock;

    // ⑥ 设置本地地址
    // inet_rcv_saddr: 用于收包哈希匹配
    // inet_saddr: 用于发包源地址
    inet->inet_rcv_saddr = inet->inet_saddr = addr->sin_addr.s_addr;
    if (chk_addr_ret == RTN_MULTICAST || chk_addr_ret == RTN_BROADCAST)
        inet->inet_saddr = 0;  // 多播/广播使用出口设备地址

    // ⑦ 端口绑定：调用 inet_csk_get_port()
    // 详见 [TCP 端口分配与哈希表](TCP端口分配与哈希表.md)
    if (snum || !(inet->bind_address_no_port ||
                  (flags & BIND_FORCE_ADDRESS_NO_PORT))) {
        if (sk->sk_prot->get_port(sk, snum)) {
            inet->inet_saddr = inet->inet_rcv_saddr = 0;
            err = -EADDRINUSE;
            goto out_release_sock;
        }
        // post-bind BPF 钩子（BPF 调用路径跳过，避免递归）
        if (!(flags & BIND_FROM_BPF)) {
            err = BPF_CGROUP_RUN_PROG_INET4_POST_BIND(sk);
            if (err) {
                inet->inet_saddr = inet->inet_rcv_saddr = 0;
                goto out_release_sock;
            }
        }
    }

    // ⑧ 设置用户锁定标志，防止系统自动修改地址/端口
    if (inet->inet_rcv_saddr)
        sk->sk_userlocks |= SOCK_BINDADDR_LOCK;
    if (snum)
        sk->sk_userlocks |= SOCK_BINDPORT_LOCK;
    inet->inet_sport = htons(inet->inet_num);
    inet->inet_daddr = 0;
    inet->inet_dport = 0;
    sk_dst_reset(sk);
    err = 0;

out_release_sock:
    // ★ 与上方 lock_sock 对称：只有 BIND_WITH_LOCK 路径才释放
    if (flags & BIND_WITH_LOCK)
        release_sock(sk);
out:
    return err;
}
```

**为什么 `lock_sock` / `release_sock` 必须受 `BIND_WITH_LOCK` 控制？**

`__inet_bind` 同时被两条路径调用：
- **用户态 `bind()`**：`inet_bind` 传 `BIND_WITH_LOCK`，需要自己加锁
- **BPF post-bind / 内核内部**：传 `BIND_FROM_BPF` 等不带锁标志，调用方已持锁或无需加锁

如果忽略这个标志直接 `lock_sock(sk)`，BPF 路径会在已持锁状态下重复加锁 → 死锁；而 `release_sock` 不对称释放则会破坏锁计数。

### 关键分支：port=0 vs port≠0

- **port≠0（指定端口）**：`get_port(sk, snum)` 在 bhash 中查找冲突
- **port=0（自动分配）**：`get_port(sk, 0)` 调用 `inet_csk_find_open_port()` 从临时端口范围分配
- **port=0 + `bind_address_no_port`**：跳过 `get_port`，只绑定地址，端口延迟到 `connect()` 时分配

### 地址类型检查：`inet_addr_type_table()`

通过 FIB 路由表查询地址类型。`inet_addr_type_table` 定义在 `net/ipv4/fib_frontend.c:236`，内部转调 `__inet_dev_addr_type`（`fib_frontend.c:205`），核心查找逻辑如下：

```c
// net/ipv4/fib_frontend.c:221-228 (在 __inet_dev_addr_type 内)
if (!fib_table_lookup(table, &fl4, &res, FIB_LOOKUP_NOREF)) {
    struct fib_nh_common *nhc = fib_info_nhc(res.fi, 0);
    if (!dev || dev == nhc->nhc_dev)
        ret = res.type;   // RTN_LOCAL / RTN_BROADCAST / RTN_UNICAST 等
}
```

---

## 五、SO_REUSEADDR 与 SO_REUSEPORT

> 这两个选项影响 `inet_csk_get_port()` 中的冲突检测逻辑，冲突检测的完整代码分析参见 [TCP 端口分配与哈希表](TCP端口分配与哈希表.md)。本节仅总结使用层面的行为差异。

### SO_REUSEADDR

| 场景 | 效果 |
|------|------|
| 对端 socket 在 `TIME_WAIT` 状态 | **允许绑定**（最常见用途：服务器重启） |
| 对端 socket 在 `ESTABLISHED` 状态 | **允许绑定**（不同地址对不冲突） |
| 对端 socket 在 `LISTEN` 状态 | **不允许**（`sk2->sk_state == TCP_LISTEN` 时检查失败） |
| 双方绑定 `0.0.0.0:80` 且都 LISTEN | **冲突**，即使都设置 REUSEADDR |

核心限制：`reuse && sk2->sk_reuse && sk2->sk_state != TCP_LISTEN`。LISTEN 状态的 socket 无法被 REUSEADDR 绕过。

### SO_REUSEPORT

| 条件 | 要求 |
|------|------|
| 双方都设置 `SO_REUSEPORT` | 必须 |
| 同一 UID（`uid_eq`） | 必须（安全隔离） |
| 地址相同 | 允许（这是 REUSEPORT 的用途） |
| 不同进程 LISTEN 同一端口 | 允许（内核做负载均衡分发 SYN） |

REUSEPORT 的核心优势：**允许多个 LISTEN socket 绑定同一 `addr:port`**，解决了 REUSEADDR 无法做到的场景。

### 具体场景示例

#### 场景 1：服务器重启（SO_REUSEADDR 最常用场景）

**问题**：HTTP 服务器 crash 或被 `kill`，重启时旧 socket 仍残留在 `TIME_WAIT` / `ESTABLISHED`，`bind()` 同一端口返回 `EADDRINUSE`。

```c
// server.c
int sk = socket(AF_INET, SOCK_STREAM, 0);
int opt = 1;
setsockopt(sk, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));  // ★ 必加

struct sockaddr_in addr = {
    .sin_family = AF_INET,
    .sin_port   = htons(8080),
    .sin_addr.s_addr = htonl(INADDR_ANY),
};
if (bind(sk, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    exit(1);
}
listen(sk, 128);
```

**行为**：

| 旧 socket 状态 | 新 socket 设 SO_REUSEADDR | bind 结果 |
|---------------|-------------------------|----------|
| `TIME_WAIT`（活跃连接已关闭） | 是 | ✓ 成功 |
| `ESTABLISHED`（仍有活跃连接） | 是 | ✓ 成功（不同四元组不冲突） |
| `FIN_WAIT_2` / `CLOSE_WAIT` | 是 | ✓ 成功 |
| 旧 listener 仍在 `LISTEN` 状态 | 是 | ✗ `EADDRINUSE`（见场景 2） |
| 任意状态 | 否 | ✗ `EADDRINUSE` |

**源码路径**：`inet_csk_bind_conflict()`（`inet_connection_sock.c:133-183`）

```c
if (reuse && sk2->sk_reuse && sk2->sk_state != TCP_LISTEN) {
    // 双方都 REUSEADDR 且对方不在 LISTEN → 不 break，允许复用
    if (inet_rcv_saddr_equal(sk, sk2, true))
        break;  // relax=true 时这里不会执行到
}
```

#### 场景 2：旧 LISTEN socket 仍在（SO_REUSEADDR 失败）

**问题**：父进程 fork 出 worker 后自己保持 LISTEN，worker 重启时想绑定同一端口。

```c
// 父进程
int parent_sk = socket(...);
bind(parent_sk, &addr, ...);    // 绑 0.0.0.0:8080
listen(parent_sk, 128);         // 状态 = TCP_LISTEN
fork();                         // 子进程继承 parent_sk

// 子进程（另起一个 socket）
int child_sk = socket(...);
setsockopt(child_sk, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
bind(child_sk, &addr, ...);     // 同样地址
// → EADDRINUSE！因为 parent_sk 仍在 TCP_LISTEN 状态
```

**源码解释**：`inet_csk_bind_conflict()` 对 `sk2->sk_state == TCP_LISTEN` 显式短路，即使双方都设了 `SO_REUSEADDR` 也会 `break`（冲突）。

**解决方法**：改用 `SO_REUSEPORT`（见场景 4），或确保旧 listener 先 `close()`。

#### 场景 3：只有一方设 SO_REUSEADDR（失败）

**问题**：

```c
// 进程 A（未设 SO_REUSEADDR）
int sk_a = socket(...);
bind(sk_a, &addr, ...);
listen(sk_a, 128);
// ... A 接受连接，某活跃连接进入 ESTABLISHED
close(sk_a);                    // listener 关闭，但子连接仍在 ESTABLISHED

// 进程 B（设了 SO_REUSEADDR）
int sk_b = socket(...);
setsockopt(sk_b, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
bind(sk_b, &addr, ...);
// → EADDRINUSE！因为 A 的 ESTABLISHED 子连接 sk_reuse=0
```

**源码解释**：`reuse && sk2->sk_reuse && sk2->sk_state != TCP_LISTEN` 三个条件**全部为真**才放行。B 设了 `reuse=1`，但 A 的 ESTABLISHED socket `sk2->sk_reuse=0`，整体为假 → 走 `else` 分支，地址相同 → `break` 冲突。

**关键教训**：`SO_REUSEADDR` 必须**双方都设**才生效。服务器启动第一件事就该 `setsockopt(SO_REUSEADDR)`，否则 crash 后下次启动无法复用端口。

#### 场景 4：多 worker 监听同一端口（SO_REUSEPORT 典型用法）

**问题**：Nginx / Redis 多 worker 进程都想 `listen()` 同一端口，由内核分发 SYN 实现负载均衡。

```c
// worker.c — 每个 worker 进程独立运行同一份代码
int sk = socket(AF_INET, SOCK_STREAM, 0);
int opt = 1;
setsockopt(sk, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));  // ★ 必加

struct sockaddr_in addr = {
    .sin_family = AF_INET,
    .sin_port   = htons(80),
    .sin_addr.s_addr = htonl(INADDR_ANY),
};
bind(sk, (struct sockaddr *)&addr, sizeof(addr));
listen(sk, 1024);

// accept 循环：每个 worker 各自 accept()，内核按 hash 分发 SYN
while (1) {
    int client = accept(sk, NULL, NULL);
    handle(client);
}
```

**启动方式**：

```bash
# 父进程 fork 4 个 worker，每个都独立 bind+listen 80 端口
for i in 1 2 3 4; do ./worker & done
```

**验证**：

```bash
$ ss -ltnp | grep :80
LISTEN  0  1024  0.0.0.0:80  users:(("worker",pid=1234,fd=3))
LISTEN  0  1024  0.0.0.0:80  users:(("worker",pid=1235,fd=3))
LISTEN  0  1024  0.0.0.0:80  users:(("worker",pid=1236,fd=3))
LISTEN  0  1024  0.0.0.0:80  users:(("worker",pid=1237,fd=3))
```

4 个 listener 同时存在，不报 `EADDRINUSE`。

**源码路径**：`inet_csk_get_port()`（`inet_connection_sock.c:461-476`）

```c
if (!hlist_empty(&tb->owners)) {
    if (sk->sk_reuse == SK_FORCE_REUSE)
        goto success;
    if ((tb->fastreuse > 0 && reuse) ||
        sk_reuseport_match(tb, sk))     // ★ SO_REUSEPORT 走这里
        goto success;
    if (inet_csk_bind_conflict(sk, tb, true, true))
        goto fail_unlock;
}
```

`sk_reuseport_match()`（`inet_connection_sock.c:316-347`）依次校验：
1. `tb->fastreuseport > 0` — 桶内所有 socket 都支持 REUSEPORT
2. `sk->sk_reuseport` — 新 socket 已设 REUSEPORT
3. `!rcu_access_pointer(sk->sk_reuseport_cb)` — 无 BPF 程序干扰
4. `uid_eq(tb->fastuid, uid)` — **同一用户**（安全隔离）

#### 场景 5：UID 不同 / 单方未设（SO_REUSEPORT 失败）

**问题 A：双方 UID 不同**

```bash
# 进程 A 以 root 运行
sudo ./worker     # uid=0, 绑定 80 端口成功

# 进程 B 以普通用户运行
./worker          # uid=1000, 绑定同一端口
# → EADDRINUSE（sk_reuseport_match 在 uid_eq 处返回 0）
```

**问题 B：只有一方设 SO_REUSEPORT**

```c
// 进程 A（未设 SO_REUSEPORT）
int sk_a = socket(...);
bind(sk_a, &addr, ...);
listen(sk_a, 128);

// 进程 B（设了 SO_REUSEPORT）
int sk_b = socket(...);
setsockopt(sk_b, SOL_SOCKET, SO_REUSEPORT, &(int){1}, sizeof(int));
bind(sk_b, &addr, ...);
// → EADDRINUSE（sk_reuseport_match 在 !sk->sk_reuseport 处返回 0）
// 随后进入 inet_csk_bind_conflict，地址相同 → 冲突
```

**关键教训**：
- `SO_REUSEPORT` 同样要求**双方都设**（与 REUSEADDR 一致）
- `SO_REUSEPORT` 额外要求**同一 UID**，root 进程与用户进程不能"共享端口"——这是多租户隔离的安全防线

#### 场景选择决策树

```
我要让新 socket bind() 已被占用的端口
    │
    ├─ 旧 socket 是 TIME_WAIT / ESTABLISHED 等"非 LISTEN" 状态？
    │   └─ 是 → SO_REUSEADDR（双方都设）
    │
    ├─ 我要多个进程同时 LISTEN 同一端口做负载均衡？
    │   └─ 是 → SO_REUSEPORT（双方都设 + 同 UID）
    │
    ├─ 我要新 listener 在旧 listener 仍在运行时接管端口？
    │   └─ 不可能（REUSEADDR 被 LISTEN 状态短路，REUSEPORT 让两个 listener 共存而非接管）
    │       → 解决：让旧进程先 close()，或用 systemd socket activation
    │
    └─ 我要 connect() 复用 TIME_WAIT 的四元组？
        └─ 不是 bind 层问题，改用 sysctl tcp_tw_reuse（见 TCP端口分配与哈希表.md §5）
```

---

## 六、SO_BINDTODEVICE 与 bind 的交互

> 进阶主题：分析先 `setsockopt(SO_BINDTODEVICE)` 再 `bind()` 在 socket 生命周期（bind → connect/发包 → 收包）中对 4 处行为的影响。所有源码引用均来自 Linux 5.15.78，每处标有文件:行号，末尾附"源码对照表"供复核。
>
> **一句话机制**：`SO_BINDTODEVICE` 的内核动作**只是** `sk->sk_bound_dev_if = ifindex`（`net/core/sock.c:619`），然后 `sk_bound_dev_if` 在后续 4 处被读取。TCP 的 `tcp_prot` 未实现 `.rehash` 回调（`tcp_ipv4.c:3836-3881` 的结构体无 `.rehash` 字段），所以**改绑设备不会 rehash、不会重做冲突检测**。
>
> 下面按 **bind 阶段 → connect/发包阶段 → 收包阶段** 的 socket 生命周期展开，从最常用到最深入。

### 6.1 bind 阶段（一）：端口冲突按设备分域 — 最常用

这是 `SO_BINDTODEVICE + bind` 最常被利用的能力。

```c
// net/ipv4/inet_connection_sock.c:133-183  inet_csk_bind_conflict()
static int inet_csk_bind_conflict(const struct sock *sk,
                                  const struct inet_bind_bucket *tb,
                                  bool relax, bool reuseport_ok)
{
    ...
    sk_for_each_bound(sk2, &tb->owners) {
        if (sk != sk2 &&
            (!sk->sk_bound_dev_if ||         // ★ 双方任一未绑设备
             !sk2->sk_bound_dev_if ||
             sk->sk_bound_dev_if == sk2->sk_bound_dev_if)) {  // ★ 或设备相同
            // 才进入 IP/端口/REUSEADDR 冲突判断
            ...
        }
    }
    return sk2 != NULL;
}
```

**解读**：

- 只有"双方 `sk_bound_dev_if` 都为 0，或相等"才继续判断 IP+port 冲突
- 只要**任一方绑了不同设备**，外层 `if` 为假，直接跳过，视为"不冲突"

**行为矩阵**（已存在一个 listener，新 socket 试图 bind 同一 IP:port）：

| 已存在 listener | 新 socket | 结果 |
|----------------|-----------|------|
| `0.0.0.0:80`，未绑设备 | `bind 0.0.0.0:80` 绑 eth0 | **EADDRINUSE**（任一未绑 → 判冲突） |
| `0.0.0.0:80`，绑 eth0 | `bind 0.0.0.0:80` 绑 eth1 | **成功** ✓（不同设备） |
| `0.0.0.0:80`，绑 eth0 | `bind 0.0.0.0:80` 绑 eth0 | **EADDRINUSE**（同设备） |
| `10.0.0.5:80`，绑 eth0 | `bind 10.0.0.5:80` 绑 eth1 | **成功** ✓ |

**典型场景**：多网卡服务器在每张网卡独立监听同一端口（双网卡负载均衡、多租户 VRF 隔离、DPDK/XDP 与内核栈共存）。

**陷阱**：必须**双方都绑设备**才能"并行"监听；只要任一方 `sk_bound_dev_if == 0`，仍然冲突。

### 6.2 bind 阶段（二）：VRF 场景下地址校验走设备对应的 FIB 表

`bind(ip)` 要验证 `ip` 是本机本地地址，否则会返回 `EADDRNOTAVAIL`（见 §四 步骤 ③）。这一检查的 FIB 表受 `sk_bound_dev_if` 影响：

```c
// net/ipv4/af_inet.c:550-562  __inet_bind()
/* 步骤2: 检查地址类型(本地/多播/广播) */
tb_id = l3mdev_fib_table_by_index(net, sk->sk_bound_dev_if) ? : tb_id;
chk_addr_ret = inet_addr_type_table(net, addr->sin_addr.s_addr, tb_id);

/* 非本地地址绑定检查 */
err = -EADDRNOTAVAIL;
if (!inet_can_nonlocal_bind(net, inet) &&
    addr->sin_addr.s_addr != htonl(INADDR_ANY) &&
    chk_addr_ret != RTN_LOCAL &&
    chk_addr_ret != RTN_MULTICAST &&
    chk_addr_ret != RTN_BROADCAST)
    goto out;
```

**解读**：

- `l3mdev_fib_table_by_index()` 在设备属于某 VRF（L3 master device）时返回该 VRF 的独立 FIB 表 ID；否则返回 0，走默认 `RT_TABLE_LOCAL`
- `inet_addr_type_table` 在**该 FIB 表内**判断 `addr` 是否为 `RTN_LOCAL`

**行为差异**：

| 场景 | 不绑设备 | 绑 VRF 设备 |
|------|---------|------------|
| bind `10.0.0.5`（该 IP 只在 VRF-A 内是 local） | `-EADDRNOTAVAIL`（主表查不到） | **成功**（VRF-A 表命中） |
| bind `192.168.1.10`（主表 local） | **成功** | 绑非主表 VRF → `-EADDRNOTAVAIL` |

**用途**：VRF 多租户隔离 —— 每个 VRF 内的 socket 只能 bind 到该 VRF 路由表可见的 IP，跨 VRF 同名 IP 互不干扰。

### 6.3 connect / 发包阶段：路由 oif 被钉死

```c
// net/ipv4/tcp_ipv4.c:445-448  tcp_v4_connect()
rt = ip_route_connect(fl4, nexthop, inet->inet_saddr,
                      RT_CONN_FLAGS(sk), sk->sk_bound_dev_if,  // ★ 路由 oif 被钉死
                      IPPROTO_TCP, orig_sport, orig_dport, sk);
```

**解读**：`connect()` 发起的路由查询把 `sk_bound_dev_if` 当作 output interface 传给 `ip_route_connect`，FIB 只在**该设备可达**的路由里选择。

**行为差异**：

| 场景 | 不绑设备 | 绑 eth0 |
|------|---------|--------|
| 路由表中 eth0 / eth1 都能到目标 | 内核按 metric 选最优 | **强制走 eth0**，即使 eth1 metric 更优 |
| 目标只能从 eth1 到 | 走 eth1 | `-ENETUNREACH` |

**用途**：强制流量从指定网卡出（如管理口 vs 业务口分离）。

### 6.4 收包阶段：按设备匹配 listener

客户端 SYN 到达时，内核在 `tcp_hashinfo.ehash`（已建立连接哈希表）查找匹配的 listener。查找函数按 `sk_bound_dev_if` 匹配入站设备：

```c
// net/ipv4/inet_hashtables.c:243-245  遍历 listener 时的匹配
if (!inet_sk_bound_dev_eq(net, sk->sk_bound_dev_if, dif, sdif))
    return -1;                   // ★ 入站设备与绑定的设备不匹配 → 跳过
score = sk->sk_bound_dev_if ? 2 : 1;   // 绑设备的 listener 优先级更高
```

`inet_sk_bound_dev_eq` 是个 wrapper（`include/net/inet_sock.h:152-161`），它读取 `sysctl_tcp_l3mdev_accept` 后转调底层 `inet_bound_dev_eq`：

```c
// include/net/inet_sock.h:144-150
static inline bool inet_bound_dev_eq(bool l3mdev_accept, int bound_dev_if,
                                     int dif, int sdif)
{
    if (!bound_dev_if)
        return !sdif || l3mdev_accept;   // 未绑：允许 slave 设备入站（受 sysctl 控制）
    return bound_dev_if == dif || bound_dev_if == sdif;  // 绑：必须匹配 dif 或其 master/slave
}

// include/net/inet_sock.h:152-161
static inline bool inet_sk_bound_dev_eq(struct net *net, int bound_dev_if,
                                        int dif, int sdif)
{
#if IS_ENABLED(CONFIG_NET_L3_MASTER_DEV)
    return inet_bound_dev_eq(!!READ_ONCE(net->ipv4.sysctl_tcp_l3mdev_accept),
                             bound_dev_if, dif, sdif);
#else
    return inet_bound_dev_eq(true, bound_dev_if, dif, sdif);
#endif
}
```

**解读**：

- 绑了 eth0 的 listener 只接收从 eth0（或其 L3 master/slave）入站的 SYN
- 从 eth1 来的 SYN 会匹配绑 eth1 的另一 listener，互不串扰
- 未绑设备的 listener 默认接收所有设备（除非 `sysctl_l3mdev_accept=0`）

**用途**：这是"同一端口多设备独立监听能正常工作"的根本保证 —— 否则两个 listener 同时存在时，入站 SYN 会被随机命中。

### 6.5 推荐顺序与代码示例

```c
int sk = socket(AF_INET, SOCK_STREAM, 0);

// ✅ 推荐：先绑设备，再 bind
setsockopt(sk, SOL_SOCKET, SO_BINDTODEVICE, "eth0", sizeof("eth0"));
bind(sk, (struct sockaddr *)&addr, sizeof(addr));
listen(sk, backlog);

// ⚠️ 反例：先 bind 再改设备
bind(sk, ...);
setsockopt(sk, SOL_SOCKET, SO_BINDTODEVICE, "eth1", ...);
// 问题 1：需要 CAP_NET_RAW 才能改已绑的 sk_bound_dev_if
// 问题 2：TCP 无 .rehash，不会在 bhash 桶里重新校验冲突
//         如果 eth1 上已有同 IP:port listener，socket 会进入"已 bind 但收包错乱"状态
```

**工程规范**：把 `SO_BINDTODEVICE` 放在 socket() 之后、bind() 之前，让 bind 一次性完成设备感知的冲突检测。

### 6.6 源码对照表（供读者复核）

| 论断 | 文件:行号 |
|------|---------|
| `SO_BINDTODEVICE` 写 `sk->sk_bound_dev_if` | `net/core/sock.c:619` |
| 重复绑设备需 `CAP_NET_RAW` | `net/core/sock.c:611-613` |
| TCP `tcp_prot` 未实现 `.rehash` | `net/ipv4/tcp_ipv4.c:3836-3881`（结构体无 `.rehash` 字段） |
| bind 冲突按 `sk_bound_dev_if` 分域 | `net/ipv4/inet_connection_sock.c:157-161` |
| bind 时 FIB 查表受设备影响 | `net/ipv4/af_inet.c:551-552` |
| connect 路由 oif 被钉死 | `net/ipv4/tcp_ipv4.c:445-448` |
| 收包按设备匹配 listener | `net/ipv4/inet_hashtables.c:243-245` |
| `inet_sk_bound_dev_eq` / `inet_bound_dev_eq` 处理 L3 master/slave | `include/net/inet_sock.h:144-161` |

### 6.7 排查陷阱：SO_BINDTODEVICE 下的 EADDRINUSE 误判

常规 `EADDRINUSE` 排查只看 IP+port，但如果 socket 设置了 `SO_BINDTODEVICE`，冲突判定还会考虑设备（见 上文）。**同一 IP:port 在不同网卡上可以被两个 listener 同时占用**，此时 `ss -tlnp` 会列出两条记录，不是故障。

---


---

## 七、常见错误与排查


| 错误码 | 场景 | 排查方法 |
|--------|------|----------|
| `EACCES` | 非特权进程绑定端口 < 1024 | `sysctl net.ipv4.ip_unprivileged_port_start` 或 `CAP_NET_BIND_SERVICE` |
| `EADDRINUSE` | 端口冲突 | `ss -tlnp \| grep :PORT`；检查 TIME_WAIT 残留；设 `SO_REUSEADDR` |
| `EADDRNOTAVAIL` | 绑定的 IP 非本机地址 | `ip addr show`；设 `IP_FREEBIND` 或 `IP_TRANSPARENT` |
| `EINVAL` | socket 已绑定（重复 bind）或状态不对 | 检查是否多次 bind |
| `EAFNOSUPPORT` | `sin_family` 不是 `AF_INET` | 检查 sockaddr_in 初始化 |

### 排查端口占用

```bash
# 查看端口绑定情况
ss -tlnp | grep :8080

# 查看 TIME_WAIT 连接
ss -tan state time-wait | grep :8080

# 查看 bhash 统计
cat /proc/net/sockstat
# TCP: inuse X orphan Y tw Z alloc W mem M
```

### 延伸阅读

bind 失败时，除了上表的通用排查，还要结合本文其他章节定位根因：

| 错误码 | 触发场景 | 详见 |
|--------|---------|------|
| `EADDRINUSE` | 端口已被 bind / listen / TIME_WAIT 占用 | [§五 SO_REUSEADDR / SO_REUSEPORT](#五so_reuseaddr-与-so_reuseport) 场景 1-5 |
| `EADDRINUSE` | 同一 IP:port 在不同网卡上看似冲突 | [§六 SO_BINDTODEVICE](#六so_bindtodevice-与-bind-的交互) §6.7 |
| `EADDRNOTAVAIL` | bind 的 IP 在 VRF 路由表内不可见 | [§六 §6.2](#六so_bindtodevice-与-bind-的交互) VRF FIB 表 |
| `EACCES` | 端口 < 1024 且无 CAP_NET_BIND_SERVICE | [§四 步骤 ④](#四核心绑定逻辑__inet_bind) |

---
