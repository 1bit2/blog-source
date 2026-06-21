+++
date = '2026-04-22'
title = 'Socket 创建与 fd 分配全流程'
weight = 4
tags = [
    "socket",
    "syscall",
    "fd分配",
    "VFS",
    "inet_create",
    "sock_alloc",
    "fdtable",
    "socket_alloc",
    "container_of",
]
categories = [
    "网络",
]
+++
# Socket 创建与 fd 分配全流程

> 基于 Linux 5.15.78，以 `socket(AF_INET, SOCK_STREAM, 0)` 为例，追踪从用户态到内核、从协议族到 fd 表的每一步。重点剖析对象之间的内存布局与关联关系。

---

## 一、全局视角：一次 socket() 调用创建了哪些对象？

用户态一句 `int fd = socket(AF_INET, SOCK_STREAM, 0)` 看似只返回一个整数，但内核实际分配了**五个核心对象**：

| 对象 | 类型 | 来源 | 分配方式 |
|------|------|------|---------|
| socket_alloc | `struct socket_alloc` | sockfs inode slab | `kmem_cache_alloc(sock_inode_cachep)` |
| ↳ socket | `struct socket` | 嵌在 socket_alloc 内 | 与 inode 一体分配 |
| ↳ vfs_inode | `struct inode` | 嵌在 socket_alloc 内 | 与 socket 一体分配 |
| sock (tcp_sock) | `struct tcp_sock` | tcp_prot.slab | `sk_alloc()` |
| file | `struct file` | slab | `alloc_file_pseudo()` |

这五个对象通过指针互相关联，形成从 fd 到协议栈的完整链路：

```
fd (整数)
 └─→ fdt->fd[fd] ──→ struct file
                       ├── f_op = socket_file_ops
                       ├── private_data ──→ struct socket ─┐
                       └── f_path.dentry──→ dentry         │
                                             └──→ inode ←──┘ (同一 socket_alloc 内)
                                                              
struct socket                    struct sock (tcp_sock)
 ├── sk ────────────────────────→ ├── sk_prot = tcp_prot
 ├── ops = inet_stream_ops        ├── sk_socket ──→ (回指 socket)
 ├── file ──→ (回指 file)         ├── sk_state = TCP_CLOSE
 └── wq (等待队列)                └── (cwnd/定时器/队列...)
```

---

## 二、核心谜题：为什么 socket 和 inode 捆绑在一起？

### 2.1 `struct socket_alloc`：一次分配、两个身份

```c
// include/net/sock.h L1470-1473
struct socket_alloc {
    struct socket socket;     // BSD socket 层对象
    struct inode vfs_inode;   // VFS 层 inode
};
```

**这不是"先创建 inode 再创建 socket"，而是一次 `kmem_cache_alloc` 同时获得两者。** 它们共享同一块内存（从 `sock_inode_cache` slab 分配），layout 如下：

```
           socket_alloc 内存布局（slab 分配的一整块）
           ┌──────────────────────────────────────┐ 低地址
           │ struct socket                         │
           │   ├── state (SS_UNCONNECTED)          │
           │   ├── type                            │
           │   ├── flags                           │
           │   ├── *ops    → inet_stream_ops       │
           │   ├── *sk     → tcp_sock              │
           │   ├── *file   → struct file           │
           │   └── wq                              │
           │       └── wait (等待队列头)            │
  偏移 ──→ ├──────────────────────────────────────┤
           │ struct inode (vfs_inode)               │
           │   ├── i_ino (inode 号)                │
           │   ├── i_mode = S_IFSOCK | 0777        │
           │   ├── i_uid / i_gid                   │
           │   └── i_op = sockfs_inode_ops         │
           └──────────────────────────────────────┘ 高地址
```

### 2.2 为什么必须有 inode？— 设计思考

Linux 内核所有 I/O 对象必须挂在某个 `super_block` 上才能被 VFS 识别为"文件"。VFS 的**身份链**是：

```
fd (整数) ──→ file ──→ dentry ──→ inode (唯一标识文件身份)
```

`alloc_file_pseudo()` 要求**必须传入 inode**：没 inode → 没 dentry → 没 `struct file` → `fd_install` 装不进去。这是最根本的硬约束。

围绕这条硬约束，inode 在 socket 场景下还承担四个维度的设计职责：

| 维度 | 为什么需要 inode | 设计取舍 |
|------|-----------------|---------|
| **VFS 统一入口** | `read`/`write`/`poll`/`close` 复用 VFS 路径，无需为 socket 另建系统调用分发 | 统一 > 性能：多一层间接跳转换取一套框架管所有 I/O |
| **`alloc_file_pseudo()` 硬约束** | 创建 `struct file` 必须传入 inode 构建 `dentry → inode` | 无法绕过，是 socket 必须建 inode 的直接原因 |
| **安全框架载体** | LSM（SELinux/SMACK）通过 `inode->i_security` 给 socket 打 sid 标签 | 复用 inode 已有 LSM 钩子，避免给 socket 新造一套安全框架 |
| **身份标识** | `/proc/<pid>/fd/` 展示 `socket:[ino]`，`fstat` 通过 `S_IFSOCK` 识别类型 | 让 `ss`/`lsof`/`netstat` 能用 ino 关联 fd 与 `/proc/net/tcp` |
| **sockfs 伪文件系统** | inode 挂在 `sockfs` 上，使 socket 形式上是"文件" | 遵循"一切皆文件"哲学，复用 dcache/icache 基础设施 |

### 2.3 inode 的 7 个使用时刻：从创建到释放的生命周期

上一节讲"为什么需要"，本节讲"何时真正被用到"。inode 平时**不承载字节流数据**（socket 数据走 `sk_buff` + `sk_receive_queue`/`sk_write_queue`），但在**身份识别、生命周期、安全审计**三个维度被反复引用。下面按 socket 生命周期的时间轴，列出 inode 被内核访问的 7 个真实时刻。

| # | 时刻 | 触发函数 / 源码位置 | 使用的 inode 字段 | 用户可观察现象 |
|---|------|---------------------|-------------------|----------------|
| ① | **创建** | `sock_alloc` [net/socket.c:626] | `i_ino` / `i_mode` / `i_uid` / `i_gid` / `i_op` | 分配唯一 ino；打上 `S_IFSOCK \| 0777` 类型位 |
| ② | **绑定 file** | `sock_alloc_file` [net/socket.c:455, 468] | 整个 inode 作为 `alloc_file_pseudo` 入参 | file/dentry 得以构建，fd 才能安装 |
| ③ | **关闭** | 正常：`close() → fput → sock_close → __sock_release(sock, inode)`；回滚：`sock_release(sock)` 中 `!sock->file` 分支调用 `iput(SOCK_INODE(sock))` [net/socket.c:673] | `i_count` 引用计数 | `i_count` 归零 → `sock_free_inode` 回收 slab |
| ④ | **stat 查询** | `vfs_fstat → vfs_getattr_nosec` | `i_mode` / `i_ino` / `i_uid` / `i_gid` | `fstat()` 返回 `S_ISSOCK(st.st_mode)=1` |
| ⑤ | **`/proc/<pid>/fd/` 展示** | `sockfs_dname` [net/socket.c:355] | `d_inode(dentry)->i_ino` | `readlink /proc/pid/fd/3` → `socket:[12345]` |
| ⑥ | **修改权限/所有者** | `sockfs_setattr` [net/socket.c:601] | `i_uid`/`i_gid`/`i_mode`，并经 `SOCKET_I` 同步 `sk->sk_uid` | `fchown(fd, ...)` 能改 socket 所有者 |
| ⑦ | **LSM 安全审计** | `security_inode_alloc` + `sockfs_security_xattr_set` [net/socket.c:386] | `i_security`（LSM sid）；`security.*` xattr | SELinux 对 socket 操作做权限检查 |

**① 创建：`sock_alloc` 给 inode 上"户口"**

```c
// net/socket.c sock_alloc()
inode = new_inode_pseudo(sock_mnt->mnt_sb);   // 触发 sock_alloc_inode() 分配 socket_alloc
sock = SOCKET_I(inode);
inode->i_ino = get_next_ino();                 // 唯一身份编号（全局递增）
inode->i_mode = S_IFSOCK | S_IRWXUGO;          // 类型位 S_IFSOCK，权限 0777
inode->i_uid = current_fsuid();                // 所有者
inode->i_gid = current_fsgid();
inode->i_op  = &sockfs_inode_ops;              // 挂 sockfs 的 listxattr / setattr
```

这一刻 inode 拿到编号、类型、权限。同时 LSM 通过 `security_inode_alloc()` 在 `i_security` 挂上安全上下文（SELinux 会给每个 socket inode 分配 sid）。

**② 绑定 file：inode 是 `alloc_file_pseudo` 的硬入参**

```c
// net/socket.c sock_alloc_file()
file = alloc_file_pseudo(SOCK_INODE(sock), sock_mnt, dname,
                         O_RDWR | (flags & O_NONBLOCK),
                         &socket_file_ops);
...
stream_open(SOCK_INODE(sock), file);
```

`alloc_file_pseudo` 内部：`d_alloc_pseudo(sock_mnt->mnt_sb, dname)` 建 dentry → 把 inode 挂到 `dentry->d_inode` → 让 `file->f_path = {mnt, dentry}`。**这是 inode 最关键的用途**：没它整条 VFS 链路断掉。

**③ 关闭：`close()` 触发 `sock_close`；inode 由 VFS 的 `iput` 链路回收**

正常路径 `close(fd)`：

```c
// 关闭链路
close(fd) → filp_close → fput
         → file->f_op->release  (= sock_close)
         → __sock_release(SOCKET_I(inode), inode)   // inode 非 NULL
              sock->ops->release(sock);             // 协议栈释放（tcp_close 等）
              sock->sk = NULL;
              sock->ops = NULL;
              // sock->file 仍然非 NULL → 跳过 iput 分支
```

注意 `__sock_release` 在 `sock->file != NULL` 时**不主动调用 `iput`**（[net/socket.c:671-677](../../../net/socket.c)）。inode 的释放由 VFS 链路接管：`fput` 引用归零 → `__fput` → `iput` → `i_callback` → `sock_free_inode` → `kmem_cache_free(sock_inode_cachep, ei)`。

**回滚路径**（socket 创建过程中途失败，file 未关联成功）：

```c
// net/socket.c __sock_release
if (!sock->file) {
    /* 创建失败回滚：无 file 关联，直接 iput 释放 inode+socket */
    iput(SOCK_INODE(sock));
    return;
}
```

socket 与 inode 同生共死：`sock_free_inode()` 把整块 `socket_alloc`（含 socket + inode）还给 slab，这正是 2.1 节"一次分配、两个身份"的逆过程。

**④ `stat/fstat` 系统调用：`S_ISSOCK` 靠 `i_mode` 判断**

`fstat(fd, &st)` 走 `vfs_fstat → vfs_getattr_nosec → inode->i_op->getattr`，读到的字段：

| `stat` 字段 | 来源 | 用户看到的值 |
|------------|------|--------------|
| `st_ino`   | `i_ino` | 唯一编号 |
| `st_mode`  | `i_mode` | `S_IFSOCK \| 0777` |
| `st_uid/gid` | `i_uid/i_gid` | 所有者 |
| `st_size`  | 0 | socket 无大小 |

`S_ISSOCK(st.st_mode)` 宏就靠 `i_mode` 中的 `S_IFSOCK` 位判断"这是一个 socket"。

**⑤ `/proc/<pid>/fd/` 展示：`socket:[ino]` 的来历**

```c
// net/socket.c sockfs_dname()
static char *sockfs_dname(struct dentry *dentry, char *buffer, int buflen)
{
    return dynamic_dname(dentry, buffer, buflen, "socket:[%lu]",
                         d_inode(dentry)->i_ino);
}
```

执行 `ls -l /proc/<pid>/fd/` 或 `readlink` 时：

```
lrwx------ 1 user user 64 Jun 11 10:00 3 -> socket:[12345]
```

`socket:[12345]` 里的数字就是 `inode->i_ino`。这就是为什么 `ss -tnp`、`lsof` 能用 inode 号把 fd 和 `/proc/net/tcp` 里的 socket 对应起来 —— **ino 是用户态排查网络问题的关键锚点**。

**⑥ `setattr` 修改权限：`sockfs_setattr` 反向找 socket**

```c
// net/socket.c sockfs_setattr()
static int sockfs_setattr(struct user_namespace *mnt_userns,
                          struct dentry *dentry, struct iattr *iattr)
{
    int err = simple_setattr(&init_user_ns, dentry, iattr);
    if (!err && (iattr->ia_valid & ATTR_UID)) {
        struct socket *sock = SOCKET_I(d_inode(dentry));  // 从 inode 反查 socket
        if (sock->sk)
            sock->sk->sk_uid = iattr->ia_uid;   // 同步更新 sk_uid，供 TCP 内存记账
    }
    ...
}
```

`chown` 改 socket 所有者时，inode 是入口；改完再通过 `SOCKET_I` 反查 socket 同步 `sk_uid`。这正是 2.4 节 `SOCKET_I` 双向转换能力的真实应用。

**⑦ LSM 安全框架：inode 是 socket 的"安全身份证"**

- `sock_alloc_inode` 分配时 → `security_inode_alloc()` 分配安全上下文
- `security_socket_create` 创建时 → 给 inode 打 SELinux sid 标签
- 后续 `sendmsg`/`recvmsg` 路径上 LSM 通过 `inode->i_security` 校验权限
- `sockfs_security_xattr_set` [net/socket.c:386] 拒绝用户直接写 `security.*` xattr（"Handled by LSM"），把控制权交给 LSM 模块

socket 类的 SELinux 权限检查（`socket/send`、`socket/recv`、`tcp_socket/bind` 等）全部挂在 inode 的 sid 上，没有 inode 就无法接入 SELinux 框架。

**小结**：inode 是 socket 在 VFS 世界的**身份证**。它平时不承载字节流数据，只在身份识别、生命周期、安全审计三个维度发挥作用。从 VFS 统一入口（②）到 sockfs 命名（⑤）、从 fstat（④）到关闭回收（③）、从权限修改（⑥）到 LSM 审计（⑦），inode 串起了 7 个跨子系统的触点 —— 这正是"一切皆文件"设计哲学的具体落地。

### 2.4 `SOCKET_I` 与 `SOCK_INODE`：双向零开销转换

```c
// include/net/sock.h L1475-1483

// inode → socket：已知 inode 地址，反推 socket_alloc 起始地址，取 socket 成员
static inline struct socket *SOCKET_I(struct inode *inode)
{
    return &container_of(inode, struct socket_alloc, vfs_inode)->socket;
}

// socket → inode：已知 socket 地址，反推 socket_alloc 起始地址，取 vfs_inode 成员
static inline struct inode *SOCK_INODE(struct socket *socket)
{
    return &container_of(socket, struct socket_alloc, socket)->vfs_inode;
}
```

`container_of` 的数学本质：

```
SOCKET_I(inode):
  socket_alloc_addr = inode_addr - offsetof(socket_alloc, vfs_inode)
  return &socket_alloc_addr->socket   // 偏移 0，即 socket_alloc 自身

SOCK_INODE(socket):
  socket_alloc_addr = socket_addr - offsetof(socket_alloc, socket)  // 偏移 0
  return &socket_alloc_addr->vfs_inode
```

**零开销**：编译期确定偏移量，运行时只做一次指针减法，无任何查表或间接调用。

---

## 三、调用链全景

```
用户态 socket(AF_INET, SOCK_STREAM, 0)
  │
  ▼ syscall
SYSCALL_DEFINE3(socket)                            [net/socket.c:1631]
  └── __sys_socket(family, type, protocol)         [net/socket.c:1591]
       │
       ├── 【步骤 A】sock_create()                  ← 创建 socket + sock
       │    └── __sock_create()                    [net/socket.c:1439]
       │         │
       │         ├── security_socket_create()      (LSM 安全检查)
       │         │
       │         ├── sock_alloc()                  [net/socket.c:626]
       │         │    ├── new_inode_pseudo(sock_mnt->mnt_sb)
       │         │    │    └── sock_alloc_inode()  ← 分配 socket_alloc（含socket+inode）
       │         │    │         └── kmem_cache_alloc(sock_inode_cachep)
       │         │    └── SOCKET_I(inode)          ← container_of 取回 socket
       │         │
       │         └── pf->create()                  (协议族分发)
       │              └── inet_create()            [net/ipv4/af_inet.c:277]
       │                   ├── inetsw 表匹配       → 找到 tcp_prot + inet_stream_ops
       │                   ├── sock->ops = inet_stream_ops
       │                   ├── sk_alloc()          ← 从 tcp_prot.slab 分配 tcp_sock
       │                   ├── sock_init_data()    ← 双向关联 socket↔sock
       │                   └── sk->sk_prot->init()
       │                        └── tcp_v4_init_sock() → tcp_init_sock()
       │
       └── 【步骤 B】sock_map_fd()                  ← 绑定 file 与 fd
            │
            ├── get_unused_fd_flags()              [fs/file.c:559]
            │    └── alloc_fd()                    (位图查找 + 扩容)
            │
            ├── sock_alloc_file()                  [net/socket.c:446]
            │    ├── SOCK_INODE(sock)              ← 取 inode（同一 socket_alloc 内）
            │    ├── alloc_file_pseudo(inode, sock_mnt, "TCP", ...)
            │    │    └── 创建 file + dentry，绑定 socket_file_ops
            │    ├── sock->file = file             ← 双向关联
            │    └── file->private_data = sock
            │
            └── fd_install(fd, file)               [fs/file.c:606]
                 └── rcu_assign_pointer(fdt->fd[fd], file)  ← 用户态可见
```

---

## 四、分阶段详细分析

### 阶段 1：系统调用入口

```c
// net/socket.c
SYSCALL_DEFINE3(socket, int, family, int, type, int, protocol)
{
    return __sys_socket(family, type, protocol);
}
```

`__sys_socket` 解析 `type` 高位的 `SOCK_CLOEXEC`（exec 时自动关闭）和 `SOCK_NONBLOCK`（非阻塞），然后执行两步：`sock_create` + `sock_map_fd`。

### 阶段 2：sock_alloc — 一体分配 socket + inode

```c
// net/socket.c L626-646
struct socket *sock_alloc(void)
{
    struct inode *inode;
    struct socket *sock;

    inode = new_inode_pseudo(sock_mnt->mnt_sb);  // 触发 sock_alloc_inode 回调
    if (!inode)
        return NULL;

    sock = SOCKET_I(inode);  // container_of 从 inode 取回同块内存中的 socket

    inode->i_ino = get_next_ino();
    inode->i_mode = S_IFSOCK | S_IRWXUGO;
    inode->i_uid = current_fsuid();
    inode->i_gid = current_fsgid();
    inode->i_op = &sockfs_inode_ops;

    return sock;
}
```

`new_inode_pseudo()` 调用 sockfs 超级块注册的 `sock_alloc_inode()`：

```c
// net/socket.c L299-317
static struct inode *sock_alloc_inode(struct super_block *sb)
{
    struct socket_alloc *ei;
    ei = kmem_cache_alloc(sock_inode_cachep, GFP_KERNEL);  // 一次分配完整 socket_alloc
    if (!ei)
        return NULL;
    init_waitqueue_head(&ei->socket.wq.wait);
    ei->socket.state = SS_UNCONNECTED;
    ei->socket.ops = NULL;
    ei->socket.sk = NULL;
    ei->socket.file = NULL;
    return &ei->vfs_inode;  // 返回 inode 给 VFS 框架
}
```

slab 缓存在启动时创建：

```c
// net/socket.c L334-343
sock_inode_cachep = kmem_cache_create("sock_inode_cache",
                                      sizeof(struct socket_alloc), ...);
```

**此时的 socket 是空壳**：`ops = NULL`、`sk = NULL`、`file = NULL`。

### 阶段 3：inet_create — 协议族对接

`__sock_create` 通过 `net_families[AF_INET]->create` 分发到 `inet_create`。

**3.1 inetsw 表匹配**

`inetsw[SOCK_STREAM]` 链表中查找匹配项：
- `ops = inet_stream_ops`（BSD socket 操作表：accept/bind/connect/listen 等）
- `prot = tcp_prot`（传输层操作表 + slab 信息）

**3.2 sk_alloc — 分配传输层控制块**

```c
sk = sk_alloc(net, PF_INET, GFP_KERNEL, answer_prot, kern);
```

从 `tcp_prot.slab` 高速缓存分配零初始化的 `tcp_sock`（约 2KB）。`tcp_sock` 的嵌套继承链：

```
struct tcp_sock
  └── struct inet_connection_sock
       └── struct inet_sock
            └── struct sock
                 └── struct sock_common
```

通过 `tcp_sk(sk)`、`inet_sk(sk)` 等向下转型宏访问各层字段。

**3.3 sock_init_data — 双向关联建立**

```c
// net/core/sock.c:sock_init_data()
sk->sk_socket = sock;   // sock → socket（上层）
sock->sk = sk;           // socket → sock（下层）
```

同时初始化：`sk_state = TCP_CLOSE`、收发队列、默认缓冲区、回调函数（`sk_data_ready`、`sk_write_space` 等）。

**3.4 tcp_init_sock — TCP 特有初始化**

- 三个定时器：重传、延迟 ACK、keepalive
- 初始 RTO = 1s，最小 RTO = 200ms
- 初始拥塞窗口 `cwnd = 10`（RFC 6928）
- `ssthresh = 0x7fffffff`（无穷大）
- 默认拥塞控制算法（通常 CUBIC）

### 阶段 4：sock_map_fd — 分配 fd 并安装 file

**4.1 fd 表数据结构全景**

每个进程通过 `task_struct->files` 指向 `files_struct`，其核心是 `fdtable`：

```c
// include/linux/fdtable.h
struct fdtable {
    unsigned int max_fds;           // 当前容量
    struct file __rcu **fd;         // file 指针数组，按 fd 索引
    unsigned long *close_on_exec;   // exec 时自动关闭的 fd 位图
    unsigned long *open_fds;        // 一级位图：每 bit 对应一个 fd 是否已占用
    unsigned long *full_fds_bits;   // 二级位图：每 bit 概括 BITS_PER_LONG 个 fd 是否全满
    struct rcu_head rcu;
};
```

`files_struct` 内嵌了初始 fdtable 和小数组，避免短生命周期进程做额外内存分配：

```c
struct files_struct {
    atomic_t count;                                // 引用计数（CLONE_FILES 共享时 > 1）
    bool resize_in_progress;                       // 扩容互斥标记
    wait_queue_head_t resize_wait;                 // 并发扩容等待队列
    struct fdtable __rcu *fdt;                     // 当前活跃 fdtable（RCU 切换）
    struct fdtable fdtab;                          // 内嵌初始 fdtable
    spinlock_t file_lock;
    unsigned int next_fd;                          // 启发式起始搜索位置
    unsigned long close_on_exec_init[1];           // 内嵌初始位图
    unsigned long open_fds_init[1];
    unsigned long full_fds_bits_init[1];
    struct file __rcu *fd_array[NR_OPEN_DEFAULT];  // 初始 fd 数组（64 槽）
};
```

初始状态下 `fdt` 指向内嵌的 `fdtab`，fd 数组为 64 槽（`NR_OPEN_DEFAULT = BITS_PER_LONG = 64`）。当 fd 超过 64 时才触发扩容。

内存布局如下：

<style>
.fdt-layout{max-width:750px;display:flex;gap:20px;background:#fff;border:1px solid #e0e0e0;border-radius:6px;padding:16px;margin:12px 0;font-family:Consolas,'Source Code Pro',monospace;font-size:12px}
.fdt-layout .col{flex:1}
.fdt-layout .struct-box{border:1px solid #bbb;border-radius:4px;padding:8px;margin-bottom:8px;background:#f8f8f8}
.fdt-layout .struct-box .sh{font-weight:700;font-size:13px;color:#333;margin-bottom:6px;border-bottom:1px solid #ddd;padding-bottom:3px}
.fdt-layout .row{display:flex;align-items:center;margin:3px 0;padding:3px 6px;border:1px solid #ddd;border-radius:3px;background:#fff;font-size:11px;transition:background .12s}
.fdt-layout .row:hover{background:#f0f7ff;border-color:#a0c4e8}
.fdt-layout .row .fn{flex:1;color:#333}
.fdt-layout .row .vl{color:#888;font-size:10px;text-align:right}
.fdt-layout .inner{border:1px solid #d5d0a0;border-radius:4px;padding:6px;margin-top:6px;background:#fffef5}
.fdt-layout .inner .sh{font-weight:700;font-size:12px;color:#665}
.fdt-layout .arrow{color:#2a7ae2;font-size:18px;text-align:center;line-height:28px}
.fdt-layout .target{border:1px solid #555;border-radius:3px;padding:4px 8px;margin:3px 0;background:#fff;font-size:11px;transition:background .12s}
.fdt-layout .target:hover{background:#f0f7ff;border-color:#2a7ae2}
.fdt-layout .target .tn{font-weight:600;color:#333}
.fdt-layout .target .td{color:#888;font-size:10px;margin-left:8px}
.fdt-layout .note{font-size:10px;color:#999;margin-top:6px;padding:4px;background:#f5f5f5;border-radius:3px}
</style>
<div class="fdt-layout">
  <div class="col">
    <div class="struct-box">
      <div class="sh">files_struct</div>
      <div class="row"><span class="fn"><b>fdt</b> ──────→</span><span class="vl">初始指向 &amp;fdtab</span></div>
      <div class="row"><span class="fn"><b>next_fd</b></span><span class="vl">= 0</span></div>
      <div class="row"><span class="fn"><b>file_lock</b></span><span class="vl">spinlock</span></div>
      <div class="row"><span class="fn"><b>count</b></span><span class="vl">引用计数 (CLONE_FILES)</span></div>
      <div class="row"><span class="fn"><b>resize_in_progress</b></span><span class="vl">扩容互斥标记</span></div>
      <div class="inner">
        <div class="sh">fdtab（内嵌 fdtable）</div>
        <div class="row"><span class="fn"><b>max_fds</b></span><span class="vl">= 64</span></div>
        <div class="row"><span class="fn"><b>fd</b> ──→</span><span class="vl">→ fd_array</span></div>
        <div class="row"><span class="fn"><b>open_fds</b> ──→</span><span class="vl">→ open_fds_init</span></div>
        <div class="row"><span class="fn"><b>close_on_exec</b> ──→</span><span class="vl">→ close_on_exec_init</span></div>
        <div class="row"><span class="fn"><b>full_fds_bits</b> ──→</span><span class="vl">→ full_fds_bits_init</span></div>
        <div class="row"><span class="fn">rcu_head</span><span class="vl">延迟释放</span></div>
      </div>
    </div>
  </div>
  <div class="col" style="padding-top:90px">
    <div class="arrow">→ → →</div>
    <div class="target"><span class="tn">fd_array[64]</span><span class="td">file 指针数组 · 内嵌 · 无 kmalloc</span></div>
    <div class="target"><span class="tn">open_fds_init[1]</span><span class="td">1 × unsigned long = 64 bits</span></div>
    <div class="target"><span class="tn">close_on_exec_init[1]</span><span class="td">exec 时自动关闭标记</span></div>
    <div class="target"><span class="tn">full_fds_bits_init[1]</span><span class="td">1 bit 概括全部 64 fd</span></div>
    <div class="note">
      <b>初始状态</b>：所有数组内嵌在 files_struct 中<br>
      无额外堆分配 · fd 超 64 时触发 expand_fdtable 扩容<br>
      扩容后 fdt 指向新 kmalloc 的 fdtable，旧表 call_rcu 释放
    </div>
  </div>
</div>

**fdtable 四类数组对照**

`fdtable` 内并不是单一数组，而是**1 个指针数组 + 3 块 unsigned long 位图**的组合。四类数组的元素类型、用途、粒度完全不同：

| 字段 | 元素类型 | 每元素覆盖 | 用途 |
|------|---------|-----------|------|
| `fd[]` | `struct file __rcu *` | **1 个 fd**（8 字节槽位） | fd 号 → file 指针的索引表 |
| `open_fds` | `unsigned long` | **64 个 fd**（每 bit 1 个） | 一级占用位图 |
| `close_on_exec` | `unsigned long` | **64 个 fd** | exec 时自动关闭的位图 |
| `full_fds_bits` | `unsigned long` | **64 × 64 = 4096 个 fd** | 二级摘要位图（每 bit 概括一个 ulong 宽） |

以 `fd = 20` 为例，它同时落入 4 个数组的下列位置：

```
fdt->fd[20]                        : 第 20 个槽位（8 字节指针）
fdt->open_fds[0]      的第 20 bit  : 1 bit 标记占用
fdt->close_on_exec[0] 的第 20 bit  : 1 bit 标记 exec 关闭
fdt->full_fds_bits[0] 的第 0  bit  : 概括 open_fds[0] 全 64 bit 是否全满
```

**位图 vs 指针数组的本质区别**：
- `fd[]` 是**定长槽位数组**，每槽 8 字节存 `file *`，用 `fdt->fd[N] = file` 直接写入
- 三块位图是**位压缩数组**，每 bit 表 1 个 fd，用 `__set_bit(N, fdt->open_fds)` 置位（x86_64 little-endian，`open_fds[N/64]` 的第 `N%64` 位）
- 两者**下标语义对齐**：fd=N 同时对应 `fd[N]` 槽位 + `open_fds` 第 N bit

**扩容时的分配差异**（`fs/file.c` `alloc_fdtable`）：
- `fd[]` 单独一次 `kvmalloc_array(nr, sizeof(struct file *))`
- 三块位图**一次 kvmalloc 连续内存**，首尾相接：`open_fds | close_on_exec | full_fds_bits`
- 这样 4 块内存（fdt 本体 + fd[] + 三块位图）构成一个 fdtable 的完整存储

**4.2 两级位图查找算法**

**为什么用位图而不是直接遍历 `fd[]`？** 这是经典的"空间换时间 + 指令级并行"模式：

| 方案 | 查找"第一个空闲 fd"的做法 | 复杂度 | 实际使用 |
|------|--------------------------|--------|---------|
| 逐槽扫 `fd[]` | `for (i=0; i<max_fds; i++) if (!fd[i]) return i;` | O(N) 次指针比较 | ✗ |
| 单级位图 `open_fds` | 每 64 个 fd 压成 1 个 `unsigned long`，用 CPU 位扫描指令找第一个 0 | O(N/64) | 中等规模可用 |
| **两级位图（Linux 实际方案）** | 第一级 `full_fds_bits` 跳过全满的 64-fd 块，第二级 `open_fds` 在块内精确定位 | **O(N/64) + 硬件位扫描** | ✓ |

位图之所以快，有三个本质原因：

1. **空间压缩 64 倍**：64 个 fd 的占用状态压缩成 1 个 `unsigned long`（8 字节），原本需要 64 次指针比较的工作量被压进 1 次字读取。
2. **硬件位扫描指令**：x86_64 的 `BSF`/`TZCNT`、ARM 的 `RBIT+CLZ` 能在单条指令内从一个 64-bit 字里找到第一个 0 位 —— 这是 `find_next_zero_bit` 的底层实现。
3. **二级摘要把"跨块扫描"降到常数**：当 `max_fds = 1048576`（默认 `sysctl_nr_open`）时，`open_fds` 有 16384 个 ulong；再加一层 `full_fds_bits`（256 个 ulong），最多 2 次位扫描就能定位空闲块。

| `max_fds` | 逐槽扫 `fd[]` | 单级位图 | 两级位图 |
|-----------|---------------|---------|---------|
| 64        | 64 次比较    | 1 次 BSF | 1 次 BSF |
| 1024      | 1024 次比较  | 16 次 BSF | 1–2 次 BSF |
| 65536     | 65536 次比较 | 1024 次 BSF | 1–2 次 BSF |
| 1048576   | 10⁶ 次比较   | 16384 次 BSF | 2–3 次 BSF |

> 这个模式在内核中随处可见：`page buddy allocator` 的 `free_area` 位图、`block bitmap` 管理磁盘空闲块、`cpumask` 选 CPU —— 思路完全一致。

`alloc_fd` 的核心是 `find_next_fd`，使用两级位图快速跳过全满区域：

```c
// fs/file.c
static unsigned int find_next_fd(struct fdtable *fdt, unsigned int start)
{
    unsigned int maxfd = fdt->max_fds;
    unsigned int maxbit = maxfd / BITS_PER_LONG;
    unsigned int bitbit = start / BITS_PER_LONG;

    // 第一级：在 full_fds_bits 中跳过全满的 64-fd 块
    bitbit = find_next_zero_bit(fdt->full_fds_bits, maxbit, bitbit) * BITS_PER_LONG;
    if (bitbit > maxfd)
        return maxfd;
    if (bitbit > start)
        start = bitbit;
    // 第二级：在 open_fds 中精确定位该块内第一个空闲位
    return find_next_zero_bit(fdt->open_fds, maxfd, start);
}
```

图示（64 位系统，`BITS_PER_LONG = 64`）：

<style>
.bitmap-demo{max-width:780px;background:#fff;border:1px solid #e0e0e0;border-radius:6px;padding:16px;margin:12px 0;font-family:Consolas,'Source Code Pro',monospace;font-size:12px}
.bitmap-demo h4{margin:0 0 6px;font-size:13px;color:#333}
.bitmap-demo .sub{font-size:10px;color:#888;margin-bottom:8px}
.bitmap-demo .bits{display:flex;gap:4px;flex-wrap:wrap;margin-bottom:6px}
.bitmap-demo .bit{width:52px;height:36px;display:flex;flex-direction:column;align-items:center;justify-content:center;border-radius:3px;font-size:12px;font-weight:700;border:1.5px solid;cursor:default;transition:transform .1s}
.bitmap-demo .bit:hover{transform:scale(1.08)}
.bitmap-demo .bit.full{background:#fce4e4;border-color:#e57373;color:#c62828}
.bitmap-demo .bit.free{background:#e8f5e9;border-color:#66bb6a;color:#2e7d32}
.bitmap-demo .bit .idx{font-size:9px;font-weight:400;color:#999;margin-top:1px}
.bitmap-demo .bit.free .idx{color:#558b2f}
.bitmap-demo .expand-arrow{font-size:18px;color:#66bb6a;margin:4px 0 4px 20px}
.bitmap-demo .bit-detail{display:flex;gap:3px;flex-wrap:wrap;margin-bottom:6px}
.bitmap-demo .bd{width:34px;height:30px;display:flex;flex-direction:column;align-items:center;justify-content:center;border-radius:2px;font-size:10px;border:1px solid #ddd}
.bitmap-demo .bd.occ{background:#fff0f0;color:#aaa}
.bitmap-demo .bd.hit{background:#e8f5e9;border:2px solid #66bb6a;color:#2e7d32;font-weight:700}
.bitmap-demo .bd .bi{font-size:8px;color:#bbb}
.bitmap-demo .bd.hit .bi{color:#558b2f;font-weight:600}
.bitmap-demo .result{display:inline-block;background:#e8f5e9;border:1.5px solid #66bb6a;border-radius:4px;padding:4px 14px;font-weight:700;color:#2e7d32;font-size:13px;margin:6px 0}
.bitmap-demo .steps{background:#f8f9fa;border:1px solid #e8e8e8;border-radius:4px;padding:8px 12px;margin-top:10px}
.bitmap-demo .steps .st{margin:3px 0;font-size:11px;color:#444;line-height:1.6}
.bitmap-demo .steps .st b{color:#2e7d32}
.bitmap-demo .perf{font-size:10px;color:#666;margin-top:8px;padding:4px 8px;background:#f0f4f8;border-radius:3px;border:1px solid #d5dee8}
</style>
<div class="bitmap-demo">
  <h4>第一级：full_fds_bits（摘要位图）</h4>
  <div class="sub">每 bit 概括 64 个 fd 是否全满 · find_next_zero_bit 跳过全满块</div>
  <div class="bits">
    <div class="bit full">1<span class="idx">bit 0</span></div>
    <div class="bit free" style="border-width:2.5px">0 ✓<span class="idx">bit 1</span></div>
    <div class="bit full">1<span class="idx">bit 2</span></div>
    <div class="bit free">0<span class="idx">bit 3</span></div>
    <div style="display:flex;align-items:center;color:#999;font-size:11px;padding:0 6px">…</div>
  </div>
  <div class="expand-arrow">↓ bit 1 = 0 → 展开 open_fds[1]</div>

  <h4>第二级：open_fds[1]（fd 64–127，逐位扫描）</h4>
  <div class="sub">在该 64-bit 块内找第一个 0 · 精确定位空闲 fd</div>
  <div class="bit-detail">
    <div class="bd occ">1<span class="bi">64</span></div>
    <div class="bd occ">1<span class="bi">65</span></div>
    <div class="bd occ">1<span class="bi">66</span></div>
    <div class="bd hit">0<span class="bi">67 ✓</span></div>
    <div class="bd occ">1<span class="bi">68</span></div>
    <div class="bd occ">1<span class="bi">69</span></div>
    <div class="bd occ">1<span class="bi">70</span></div>
    <div class="bd occ">1<span class="bi">71</span></div>
    <div style="display:flex;align-items:center;color:#999;font-size:10px;padding:0 4px">… 共 64 bits</div>
  </div>
  <div class="result">→ 返回 fd = 67</div>

  <div class="steps">
    <div class="st">❶ 扫描 <code>full_fds_bits</code>：<b>跳过</b> bit0（全满）→ 命中 bit1（有空闲），对应 fd [64, 127]</div>
    <div class="st">❷ 进入 <code>open_fds[1]</code>：<code>find_next_zero_bit(start=64)</code> → 跳过 64,65,66 → <b>命中 bit 67</b></div>
    <div class="st">❸ 返回 fd = 67，<code>alloc_fd</code> 调用 <code>__set_open_fd(67, fdt)</code> 标记占用</div>
  </div>
  <div class="perf">复杂度 O(N / BITS_PER_LONG) — 数千 fd 时仍可一次跳过 64 个全满位，远优于逐位 O(N) 扫描</div>
</div>

**查找过程**：
1. 从 `start / 64` 位开始扫描 `full_fds_bits`，找到第一个为 0 的 bit（说明该 64-fd 块有空闲）
2. 跳入该块对应的 `open_fds` 区域，找第一个为 0 的 bit
3. 该 bit 的全局索引即为空闲 fd 号

**4.3 alloc_fd 完整流程**

```c
// fs/file.c（简化注释版）
static int alloc_fd(unsigned start, unsigned end, unsigned flags)
{
    struct files_struct *files = current->files;
    struct fdtable *fdt;

    spin_lock(&files->file_lock);
repeat:
    fdt = files_fdtable(files);
    fd = start;
    if (fd < files->next_fd)
        fd = files->next_fd;        // 启发式：跳过已知占满的低位

    if (fd < fdt->max_fds)
        fd = find_next_fd(fdt, fd); // 两级位图查找

    if (fd >= end)                  // 超过 RLIMIT_NOFILE → EMFILE
        goto out;

    error = expand_files(files, fd); // 需要时扩容 fdtable
    if (error < 0) goto out;
    if (error) goto repeat;          // 扩容中途可能释放锁，位图变化须重查

    if (start <= files->next_fd)
        files->next_fd = fd + 1;    // 更新启发式起点

    __set_open_fd(fd, fdt);          // 置 open_fds 位 + 可能更新 full_fds_bits
    if (flags & O_CLOEXEC)
        __set_close_on_exec(fd, fdt); // 置 close_on_exec 位

    spin_unlock(&files->file_lock);
    return fd;                       // 返回 fd 号，此时 fdt->fd[fd] 仍为 NULL
}
```

关键点：
- **位图标记与指针安装分离**：`alloc_fd` 只标记 `open_fds`，`fdt->fd[fd]` 仍为 NULL，直到后续 `fd_install` 才写入 file 指针
- **`next_fd` 启发式**：避免每次从 fd=0 开始扫描，加速连续分配场景
- **失败回滚**：如果后续 `sock_alloc_file` 失败，必须调 `put_unused_fd` 归还位图

**4.4 __set_open_fd — 位图标记与摘要更新**

```c
// fs/file.c
static inline void __set_open_fd(unsigned int fd, struct fdtable *fdt)
{
    __set_bit(fd, fdt->open_fds);
    fd /= BITS_PER_LONG;
    if (!~fdt->open_fds[fd])           // 该 ulong 全为 1？
        __set_bit(fd, fdt->full_fds_bits); // 标记二级摘要为"满"
}
```

`!~fdt->open_fds[fd]`：对 `open_fds[fd]` 取反，若全 1 则取反后为 0，`!0` 为 true。

**4.5 fdtable 扩容机制**

当 `fd >= fdt->max_fds` 时触发扩容：

```c
// fs/file.c alloc_fdtable（简化）
static struct fdtable *alloc_fdtable(unsigned int nr)
{
    // 容量计算：归一化到 1024B 粒度 → 向上取 2 的幂 → 还原为 fd 数
    nr /= (1024 / sizeof(struct file *));  // 64 位系统：nr / 128
    nr = roundup_pow_of_two(nr + 1);       // 2 的幂对齐
    nr *= (1024 / sizeof(struct file *));   // 还原
    nr = ALIGN(nr, BITS_PER_LONG);         // 64 对齐

    fdt = kmalloc(sizeof(struct fdtable), ...);
    fdt->max_fds = nr;
    fdt->fd = kvmalloc_array(nr, sizeof(struct file *), ...);

    // open_fds + close_on_exec + full_fds_bits 一次分配，首尾相接
    data = kvmalloc(2 * nr / BITS_PER_BYTE + BITBIT_SIZE(nr), ...);
    fdt->open_fds = data;
    fdt->close_on_exec = data + nr / BITS_PER_BYTE;
    fdt->full_fds_bits = data + 2 * nr / BITS_PER_BYTE;
    return fdt;
}
```

扩容序列（64 位系统）：`64 → 128 → 256 → 512 → 1024 → 2048 → ...`（2 的幂增长）。

`expand_fdtable` 执行切换：

```
1. spin_unlock → alloc_fdtable（可能睡眠）
2. 如果 files->count > 1（CLONE_FILES 共享）→ synchronize_rcu 等待所有读者退出
3. spin_lock → copy_fdtable（旧→新）→ rcu_assign_pointer(files->fdt, new_fdt)
4. 旧表通过 call_rcu 延迟释放（保护并发 fd_install 读者）
```

**4.6 sock_alloc_file — 创建伪文件**

```c
// net/socket.c L446-470
struct file *sock_alloc_file(struct socket *sock, int flags, const char *dname)
{
    if (!dname)
        dname = sock->sk ? sock->sk->sk_prot_creator->name : "";

    file = alloc_file_pseudo(SOCK_INODE(sock), sock_mnt, dname,
                             O_RDWR | (flags & O_NONBLOCK),
                             &socket_file_ops);
    sock->file = file;
    file->private_data = sock;
    stream_open(SOCK_INODE(sock), file);
    return file;
}
```

`SOCK_INODE(sock)` 取同一 `socket_alloc` 中的 `vfs_inode`，传给 `alloc_file_pseudo` 构建 dentry→inode 链路。

**4.7 fd_install — 写入 fd 表**

```c
// fs/file.c
void fd_install(unsigned int fd, struct file *file)
{
    struct files_struct *files = current->files;

    rcu_read_lock_sched();
    if (unlikely(files->resize_in_progress)) {
        rcu_read_unlock_sched();
        spin_lock(&files->file_lock);       // 慢路径：扩容中持锁写入
        rcu_assign_pointer(fdt->fd[fd], file);
        spin_unlock(&files->file_lock);
        return;
    }
    smp_rmb();                               // 与 expand_fdtable 的 smp_wmb 配对
    fdt = rcu_dereference_sched(files->fdt);
    rcu_assign_pointer(fdt->fd[fd], file);   // 快路径：无锁写入
    rcu_read_unlock_sched();
}
```

`fd_install` 有两条路径：
- **快路径**（常见）：无扩容进行中，RCU 读侧临界区内直接写指针
- **慢路径**（罕见）：扩容进行中，退化为持 `file_lock` 写入

此后 fd 对用户态可见。`read`/`write`/`close` 等通过 VFS → `socket_file_ops` → `inet_stream_ops` / `tcp_prot` 到达 TCP 协议栈。

**4.8 fd 分配与安装时序图**

<style>
.fd-seq{max-width:780px;background:#fff;border:1px solid #e0e0e0;border-radius:6px;padding:0;margin:12px 0;font-family:Consolas,'Source Code Pro',monospace;font-size:12px;overflow:hidden}
.fd-seq .phase{padding:12px 16px;border-bottom:1px solid #eee;position:relative}
.fd-seq .phase:last-child{border-bottom:none}
.fd-seq .phase:hover{background:#fafcff}
.fd-seq .ph{display:flex;align-items:center;margin-bottom:8px}
.fd-seq .ph .num{display:inline-flex;width:24px;height:24px;align-items:center;justify-content:center;border-radius:50%;background:#555;color:#fff;font-size:12px;font-weight:700;margin-right:8px;flex-shrink:0}
.fd-seq .ph .fn{font-weight:700;font-size:13px;color:#333}
.fd-seq .ph .src{font-size:10px;color:#888;margin-left:8px}
.fd-seq .op{margin:3px 0 3px 36px;padding:3px 8px;font-size:11px;color:#444;border-left:2px solid #ddd;line-height:1.6}
.fd-seq .op.key{border-left-color:#66bb6a;background:#f6fdf6}
.fd-seq .op .tag{display:inline-block;font-size:9px;padding:1px 6px;border-radius:3px;margin-left:8px;vertical-align:middle}
.fd-seq .op .tag.warn{background:#fff3cd;color:#856404;border:1px solid #ffc107}
.fd-seq .op .tag.ok{background:#e8f5e9;color:#2e7d32;border:1px solid #66bb6a}
.fd-seq .op .tag.err{background:#fce4e4;color:#c62828;border:1px solid #e57373}
.fd-seq .summary{padding:10px 16px;background:#f0f4f8;font-size:11px;color:#555;border-top:1px solid #d5dee8}
</style>
<div class="fd-seq">
  <div class="phase">
    <div class="ph">
      <span class="num">1</span>
      <span class="fn">get_unused_fd_flags(flags)</span>
      <span class="src">fs/file.c → alloc_fd(0, RLIMIT_NOFILE, flags)</span>
    </div>
    <div class="op"><code>spin_lock(&files->file_lock)</code></div>
    <div class="op"><code>find_next_fd(fdt, fd)</code> — 两级位图查找空闲 fd</div>
    <div class="op"><code>expand_files(files, fd)</code> — 容量不足时扩容 fdtable</div>
    <div class="op key"><code>__set_open_fd(fd, fdt)</code> — 标记 open_fds + 更新 full_fds_bits</div>
    <div class="op"><code>__set_close_on_exec(fd, fdt)</code> — O_CLOEXEC 时设置</div>
    <div class="op"><code>spin_unlock</code> → return fd <span class="tag warn">位图已标记，fdt->fd[fd] 仍为 NULL</span></div>
  </div>
  <div class="phase">
    <div class="ph">
      <span class="num">2</span>
      <span class="fn">sock_alloc_file(sock, flags, dname)</span>
      <span class="src">net/socket.c</span>
    </div>
    <div class="op"><code>alloc_file_pseudo(SOCK_INODE(sock), sock_mnt, "TCP", ...)</code></div>
    <div class="op key"><code>sock->file = file; file->private_data = sock</code> — 双向关联</div>
    <div class="op"><code>stream_open()</code> → return file <span class="tag err">失败时 → put_unused_fd(fd) 归还位图</span></div>
  </div>
  <div class="phase">
    <div class="ph">
      <span class="num">3</span>
      <span class="fn">fd_install(fd, file)</span>
      <span class="src">fs/file.c</span>
    </div>
    <div class="op"><code>rcu_read_lock_sched()</code></div>
    <div class="op">检查 <code>resize_in_progress</code> → 快路径(无锁) / 慢路径(持锁)</div>
    <div class="op key"><code>rcu_assign_pointer(fdt->fd[fd], file)</code> <span class="tag ok">fd 对用户态可见！</span></div>
    <div class="op"><code>rcu_read_unlock_sched()</code></div>
  </div>
  <div class="summary">
    <b>设计要点</b>：位图标记（alloc_fd）与指针写入（fd_install）分离 — 两步之间若 sock_alloc_file 失败，put_unused_fd 可安全回滚位图，不产生悬挂 fd
  </div>
</div>

---

## 五、对象关系图

> **交互版**：浏览器打开 [socket创建对象关系图.html](socket创建对象关系图.html)（悬停对象查看源码级详情：ops 成员表、继承链、初始化参数、current 访问链）

### 5.1 创建顺序与关联建立

| 步骤 | 函数 | 分配来源 | 关联建立 |
|------|------|---------|---------|
| ① | `sock_alloc()` → `sock_alloc_inode()` | `sock_inode_cache` slab | socket 空壳（ops/sk/file 均 NULL） |
| ② | `inet_create()` → `sk_alloc()` | `tcp_prot.slab` | `socket.sk ↔ sock.sk_socket` 双向关联；`socket.ops = inet_stream_ops` |
| ③ | `sock_alloc_file()` + `fd_install()` | file slab | `socket.file ↔ file.private_data` 双向关联；`fdt->fd[fd] → file` |

### 5.2 运行时访问链：从 `current` 到 `tcp_sock`

系统调用（`read`/`write`/`poll`/`close`/`getsockopt`/`epoll_ctl`）从 `current` 出发走 5 跳到达协议栈，每跳都是单指针解引用（O(1)）：

| 跳 | 起点 → 终点 | 解引用方式 | 并发保护 |
|----|------------|-----------|---------|
| ① | `current` → `task_struct` | per-cpu 变量（`this_cpu_read_stable`） | 调度器切换时更新 |
| ② | `task_struct` → `files_struct` | `current->files` 单指针 | — |
| ③ | `files_struct` → `fdtable` | `rcu_dereference_check(files->fdt)` | RCU 读侧 / `file_lock` |
| ④ | `fdtable` → `file` | `rcu_dereference(fdt->fd[fd])` | 配对 `rcu_assign_pointer` |
| ⑤ | `file` → `socket` → `sock` | `file->private_data` / `sock->sk` | `sock_from_file` 校验 `f_op` |

**三个设计要点**：

- **CLONE_FILES 共享语义**：`clone(CLONE_FILES)` 让多线程共享 `files_struct`（`count > 1`），扩容时须 `synchronize_rcu` 等其他核退出 RCU 临界区后再切换 `fdt` 指针
- **RCU 贯穿始终**：`fdt` 切换、`fd[]` 写入都用 RCU，让无锁读者（`fd_install` 快路径）与并发扩容（`expand_fdtable`）共存
- **身份链 vs 数据链分离**：身份链（fd→file→dentry→inode）走 VFS 框架；数据链（socket→sock→sk_buff 队列）走协议栈；两者通过 `file->private_data` 桥接

---

## 六、VFS 回调路径：从 fd 到协议栈

用户态 `read(fd)` 的完整路径：

```
read(fd)
  → vfs_read()
    → file->f_op->read_iter()              // socket_file_ops.read_iter
      → sock_read_iter()                    // net/socket.c
        → sock->ops->recvmsg()             // inet_stream_ops.recvmsg
          → inet_recvmsg()
            → sk->sk_prot->recvmsg()       // tcp_prot.recvmsg
              → tcp_recvmsg()
```

每个箭头对应一个函数指针跳转，分别经过：**VFS → socket_file_ops → inet_stream_ops → tcp_prot**。

---

## 七、常见问题

### Q1: alloc_fd 和 fd_install 之间如果失败会怎样？

`sock_alloc_file` 失败时内部已调 `sock_release` 释放 socket，调用方用 `put_unused_fd` 归还位图上的 fd 号。两者必须成对——分配了位图没装 file 就得归还，否则 fd 号泄漏。

### Q2: SOCK_CLOEXEC 在哪里生效？它和 O_CLOEXEC 是什么关系？

**定义**（`include/linux/net.h:74-79`）：

```c
#define SOCK_TYPE_MASK  0xf          // 低 4 位 = 真实 socket 类型
#define SOCK_CLOEXEC    O_CLOEXEC    // 与 O_CLOEXEC 同值
#define SOCK_NONBLOCK   O_NONBLOCK   // 与 O_NONBLOCK 同值
```

**三个作用**：

① **类型与标志复用同一参数**：用户写 `socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)`，一个 `type` 参数同时传类型和标志。`__sys_socket` 用位掩码拆分：

```c
// net/socket.c __sys_socket
flags = type & ~SOCK_TYPE_MASK;         // 高位 = 标志
if (flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK))
    return -EINVAL;                     // 只允许这两个标志
type  &= SOCK_TYPE_MASK;                // 低 4 位 = 真实类型
```

② **与 `O_CLOEXEC` 同值 → 免翻译直接传**：`net/socket.c:1598` 用 `BUILD_BUG_ON(SOCK_CLOEXEC != O_CLOEXEC)` 编译期断言两者相等，`sock_map_fd(sock, flags)` 把 `flags` 当 `O_CLOEXEC` 传给 `get_unused_fd_flags`，无需位转换。

③ **运行时语义**：`alloc_fd` 中 `__set_close_on_exec(fd, fdt)` 置位图（`fs/file.c:530`）。进程调用 `execve()` 时，`fs/exec.c` 的 `do_close_on_exec()` 扫 `close_on_exec` 位图，凡置位的 fd 自动 `close()`，防止子进程继承敏感 fd（监听 socket、密钥文件、数据库连接）。

**设计思考（POSIX.1-2008 引入）**：`socket()` + `fcntl(F_SETFD, FD_CLOEXEC)` 两步操作之间有竞态窗口 —— signal handler 或并发线程可能插入 `fork+exec`，导致子进程意外继承 fd。一步到位的 `SOCK_CLOEXEC` 消除该竞态。同一设计模式也用于 `epoll_create1(EPOLL_CLOEXEC)`、`eventfd(EFD_CLOEXEC)`、`timerfd_create(TFD_CLOEXEC)` 等所有返回 fd 的系统调用。

### Q3: 为什么 socket 要挂在 sockfs 上？

内核通过 VFS 统一管理所有 I/O 对象。socket 挂在 sockfs 上后，`read`/`write`/`poll`/`close` 等系统调用可以复用 VFS 的统一入口（`vfs_read` 等），而不需要为 socket 另建一套系统调用分发机制。

### Q4: `container_of` 转换的安全性？

`SOCKET_I` / `SOCK_INODE` 的安全性取决于传入的 inode/socket 确实属于 `socket_alloc`。在内核中，只有 sockfs 的 inode 会被传给 `SOCKET_I`——调用方通常先检查 `inode->i_sb->s_magic == SOCKFS_MAGIC` 或 `S_ISSOCK(inode->i_mode)`。

### Q5: 一个 socket 对象在整个生命周期中被几个 slab 缓存管理？

两个独立的 slab：
- `sock_inode_cache`：管理 `socket_alloc`（含 socket + inode）
- `TCP` slab（`tcp_prot.slab`）：管理 `tcp_sock`

关闭时释放顺序相反：先 `tcp_close` 释放 tcp_sock，再 `sock_free_inode` 释放 socket_alloc。

### Q6: 为什么 fd 是从 `next_fd` 而非 0 开始搜索？`next_fd` 是 fd 值还是数组下标？

`files->next_fd` 是**下一个建议使用的 fd 数值**（语义上等于候选 fd 号，恰好也等于数组下标，但本质是 fd 值），是启发式优化。连续调用 `socket()`/`open()` 时，低位 fd 通常已被占满。

源码三处体现"fd 值"语义（`fs/file.c`）：

```c
// L504 alloc_fd：把候选 fd 号抬到 next_fd（不是"从下标 next_fd 读"）
if (fd < files->next_fd)
    fd = files->next_fd;

// L526 alloc_fd：设为"已分配 fd 值 + 1"
if (start <= files->next_fd)
    files->next_fd = fd + 1;

// L570 __put_unused_fd：设为"刚释放的 fd 值"，下次优先复用低号
if (fd < files->next_fd)
    files->next_fd = fd;
```

若是"数组下标偏移"，`fd + 1` 的语义无从解释。`close(fd)` 释放一个较小的 fd 时，`__put_unused_fd` 把 `next_fd` 拉回该 fd 值，下次 `alloc_fd` 优先复用它，保证低号不被长期闲置。

**推演示例**：用户先后分配 fd 18–23，再 close(20)，再分配一次。假设初始 `next_fd = 18`（即 0–17 已被占用）。

| 操作 | `alloc_fd` / `__put_unused_fd` 内部 | `next_fd` 结果 |
|------|-------------------------------------|----------------|
| `socket()` | `fd = max(18, 18) = 18` → 命中 18 → `next_fd = 18 + 1` | **19** |
| `socket()` | `fd = 19` → 命中 → `next_fd = 20` | **20** |
| `socket()` | `fd = 20` → `next_fd = 21` | **21** |
| `socket()` | `fd = 21` → `next_fd = 22` | **22** |
| `socket()` | `fd = 22` → `next_fd = 23` | **23** |
| `socket()` | `fd = 23` → `next_fd = 24` | **24** |
| `close(20)` | 清 `open_fds[20]`；`if (20 < 24) next_fd = 20` | **20** |
| `socket()` | `fd = max(0, 20) = 20` → 命中 20（刚释放）→ `next_fd = 21` | **21** |

**关键观察**：
- 连续分配阶段 `next_fd` 单调递增，跳过已满低位
- `close(20)` 后 `next_fd` 被拉回到 20，下次分配**立即复用**刚释放的低号
- 若此时 20 被其他线程抢先占用，`find_next_fd` 会继续往后扫到下一个空闲位，但 `next_fd` 已记录"这里可能有机会"，避免从 0 起步扫描

### Q7: fdtable 扩容时如何保证并发安全？

三层保护协作：
1. **`file_lock` 自旋锁**：`alloc_fd` / `fd_install`（慢路径）/ `expand_fdtable` 持锁访问
2. **`resize_in_progress` 标志**：`fd_install` 快路径无锁时检查此标志，若扩容中则退化为持锁慢路径
3. **RCU**：`rcu_assign_pointer(files->fdt, new_fdt)` 切换表指针，旧表通过 `call_rcu` 延迟释放，保证并发读者不触及已释放内存

### Q8: close(fd) 时 fd 号如何回收？

`close(fd)` → `pick_file()` → `__put_unused_fd()`：

```c
static inline void __clear_open_fd(unsigned int fd, struct fdtable *fdt)
{
    __clear_bit(fd, fdt->open_fds);
    __clear_bit(fd / BITS_PER_LONG, fdt->full_fds_bits);
}
```

清 `open_fds` 对应位，同时无条件清 `full_fds_bits` 对应位（保守策略：即使该块未变空也标记为"有空闲"，下次查找会进入该块）。然后 `next_fd = min(next_fd, fd)` 保证低号优先复用。
