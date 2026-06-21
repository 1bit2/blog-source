+++
date = '2026-04-16'
title = 'close(socket) 的 fd 归还时机与 TIME_WAIT 资源分析'
weight = 19
tags = [
    "close",
    "fd归还",
    "TIME_WAIT",
    "tcp_close",
    "sock_orphan",
    "inet_timewait_sock",
    "fput",
]
categories = [
    "网络",
]
+++
> **本文已扩充并迁移至 [TCP 四次挥手与 close 深度源码分析](TCP四次挥手与close.md)**，请阅读新版。

# close(socket) 的 fd 归还时机与 TIME_WAIT 资源分析

> 基于 Linux 5.15.78，追踪 `close(fd)` 从 fd 表到协议栈到 TIME_WAIT 的完整路径。

## 概述

`close(fd)` 对 socket 而言涉及两个独立的"释放"：
- **fd 号归还**：在 `close` 入口处**立即完成**，fd 可被下次 `open`/`socket` 复用
- **协议栈资源释放**：通过 `fput` → `sock_close` → `tcp_close` 异步完成，可能经历 FIN_WAIT → TIME_WAIT 等漫长状态

这两者的时间差是理解 TIME_WAIT "不占 fd 但占端口"的关键。

## 调用链总览

```
close(fd)
  │
  ▼
close_fd(fd)                               [fs/file.c:663]
  ├── pick_file(files, fd)                 ← fd 号立即归还
  │    ├── fdt->fd[fd] = NULL
  │    └── __put_unused_fd(files, fd)      (清 open_fds 位图)
  │
  └── filp_close(file, files)
       └── fput(file)
            └── 引用归零时 → file->f_op->release
                 │
                 ▼
            sock_close(inode, filp)         [net/socket.c:1361]
              └── __sock_release(socket, inode)
                   └── socket->ops->release = inet_release  [af_inet.c:457]
                        └── sk->sk_prot->close = tcp_close  [tcp.c:3417]
                             │
                             ├── 正常关闭: tcp_send_fin → FIN_WAIT1
                             ├── 有未读数据: tcp_send_active_reset → RST
                             ├── SO_LINGER=0: disconnect → RST
                             │
                             ├── sock_orphan(sk)     ← sk 与 socket/file 断开
                             │
                             └── 可能进入:
                                  ├── TCP_CLOSE → inet_csk_destroy_sock
                                  ├── TCP_FIN_WAIT2 → tcp_time_wait()
                                  └── TCP_TIME_WAIT (由对端 FIN 触发)
```

## 详细分析

### 阶段 1：fd 号立即回收（fs/file.c）

`close_fd` 调用 `pick_file`，在持有 `files->file_lock` 的情况下：

```c
rcu_assign_pointer(fdt->fd[fd], NULL);   // 清空槽位
__put_unused_fd(files, fd);              // 清 open_fds 位图 + 更新 next_fd
```

**此后该 fd 号立即可被 `get_unused_fd_flags` 再次分配**。返回的 `file` 指针交给 `filp_close`。

### 阶段 2：file 引用归零触发协议栈释放

`filp_close` → `fput` 递减 `file` 的引用计数。当引用归零时（可能延迟到 `task_work` 或 RCU 回调），调用 `file->f_op->release`，对 socket 即 `sock_close`。

### 阶段 3：tcp_close — 协议层关闭决策

`tcp_close` 根据当前状态和数据情况选择关闭方式：

| 条件 | 行为 | 发送 |
|------|------|------|
| 接收缓冲区有未读数据 | 丢弃数据，强制关闭 | RST |
| `SO_LINGER` 且 `linger_time=0` | 立即断开 | RST |
| 正常关闭 | 优雅关闭 | FIN |
| 已在 `TCP_CLOSE` | 直接销毁 | 无 |

正常情况下 `tcp_close_state()` 执行状态转换：
- `ESTABLISHED → FIN_WAIT1`（发 FIN）
- `CLOSE_WAIT → LAST_ACK`（发 FIN）

### 阶段 4：sock_orphan — 断开用户态关联

```c
sock_orphan(sk);
```

这一步至关重要：
- `sk->sk_socket = NULL` — sock 不再关联 BSD socket
- `sk->sk_wq = NULL` — 没有进程在等待
- 设置 `SOCK_DEAD` 标志

**从此刻起，sock 成为"孤儿"**——没有 fd、没有 file、没有进程可以操作它，但 TCP 状态机仍在内核中运行（等待对端 FIN、重传等）。

### 阶段 5：TIME_WAIT — 轻量替身

当连接进入 TIME_WAIT 状态时（通常由 `tcp_time_wait()` 触发），内核做了一个关键优化：

**用 `inet_timewait_sock`（约 168 字节）替换完整的 `tcp_sock`（约 2KB）**

```
tcp_time_wait(sk, TCP_FIN_WAIT2/TCP_TIME_WAIT, tmo)
  ├── 分配 inet_timewait_sock (从 tcp_death_row 的 slab 分配)
  ├── 拷贝必要字段（地址/端口/序列号等）
  ├── 注册到 TIME_WAIT 定时器（默认 60 秒）
  └── 释放原 tcp_sock
```

TIME_WAIT 定时器到期后（2MSL = 60s），`inet_timewait_sock` 被释放，端口号归还。

## 各阶段资源状态一览

```
时间轴 ───────────────────────────────────────────────►

close()     fput归零     sock_orphan    进入TW      TW超时
  │            │             │            │           │
  ▼            ▼             ▼            ▼           ▼
┌──────┐  ┌──────┐     ┌──────┐    ┌──────┐    ┌──────┐
│fd: 回收│  │file: │     │sock: │    │tw_sock│    │全部  │
│file:存在│  │释放  │     │orphan│    │替代   │    │释放  │
│sock:存在│  │sock: │     │无fd  │    │~168B  │    │端口  │
│        │  │存在  │     │无file│    │占端口 │    │归还  │
└──────┘  └──────┘     └──────┘    └──────┘    └──────┘
```

## 关键数据结构

### inet_timewait_sock

```c
struct inet_timewait_sock {
    struct sock_common  __tw_common;    // 地址/端口/哈希（复用 sock 的公共头）
    volatile unsigned char tw_substate; // TIME_WAIT 子状态
    unsigned char       tw_rcv_wscale;  // 窗口缩放（验证迟到包用）
    __be16              tw_sport;       // 源端口
    struct inet_bind_bucket *tw_tb;     // 绑定桶（占用端口号）
    struct hlist_node   tw_death_node;  // TIME_WAIT 定时器链表
    // ... 约 168 字节
};
```

相比完整 `tcp_sock` 的 2KB+，节省了拥塞窗口、重传队列、乱序队列、发送/接收缓冲区等所有活跃连接才需要的字段。

## 常见问题

### Q: TIME_WAIT 会消耗 fd 吗？

**不会。** fd 在 `close` 入口处就已归还，TIME_WAIT 时连 `struct file` 和 `struct socket` 都已释放。`inet_timewait_sock` 只占用**端口号**和约 168 字节内存。

### Q: 大量 TIME_WAIT 的影响是什么？

不影响 fd 配额（`ulimit -n`），但会：
- 占用端口号（影响 `connect` 时可用的源端口范围）
- 占用 `ehash` 哈希表槽位
- 每条约 168 字节内存（1 万条 ≈ 1.6MB，一般不是瓶颈）

缓解方式：`net.ipv4.tcp_tw_reuse=1`、`SO_REUSEADDR`、扩大 `ip_local_port_range`。

### Q: close 后 fd 号能立即被复用吗？

**能。** `pick_file` 在 `close_fd` 一开始就清空 `fdt->fd[fd]` 并归还位图。即使 `fput` 尚未执行（file 引用未归零），fd 号已经可以被下次 `open`/`socket`/`accept` 分配。这也是为什么以下代码可能出现问题：

```c
close(fd);
// 另一个线程同时 accept()，恰好拿到同一个 fd 号
// 如果此线程还在用"旧 fd"做事，就会操作到错误的连接
```

### Q: SO_LINGER 设置为非零超时时会怎样？

`close` 会在 `sk_stream_wait_close` 中**阻塞**当前进程，等待数据发送完成或超时。但 fd 号**仍然在阻塞之前就已归还**（`pick_file` 先于 `filp_close` 执行）。
