+++
date = '2026-04-16'
title = 'TCP数据就绪与Epoll唤醒机制分析'
weight = 15
tags = [
    "TCP",
    "epoll",
    "sk_data_ready",
    "tcp_data_queue",
]
categories = [
    "网络",
]
+++
# TCP数据就绪与Epoll唤醒机制分析

## 概述

本文档详细分析Linux内核中TCP数据到达后如何通过`sk->sk_data_ready`回调触发epoll唤醒机制，最终通知用户空间应用程序的完整流程。

---

## 一、核心调用链

```
TCP数据包到达
    ↓
tcp_data_queue()                    // TCP输入处理
    ↓
tcp_data_ready(sk)                  // 检查是否满足唤醒条件
    ↓
sk->sk_data_ready(sk)               // 调用数据就绪回调（默认是sock_def_readable）
    ↓
sock_def_readable(sk)               // 唤醒等待队列
    ↓
wake_up_interruptible_sync_poll()   // 唤醒等待进程
    ↓
ep_poll_callback()                  // epoll的回调函数
    ↓
wake_up(&ep->wq)                    // 唤醒epoll_wait
    ↓
用户空间应用程序被唤醒
```

---

## 二、关键函数详解

### 2.1 tcp_data_ready (net/ipv4/tcp_input.c)

**功能**: TCP数据就绪通知函数

**调用时机**: 
- 当TCP接收到数据并加入接收队列后
- 在`tcp_data_queue()`函数中调用

**源码**:
```c
void tcp_data_ready(struct sock *sk)
{
    /* 检查是否有足够的数据可读，或socket已完成关闭 */
    if (tcp_epollin_ready(sk, sk->sk_rcvlowat) || sock_flag(sk, SOCK_DONE))
        /* 调用数据就绪回调函数（通常是sock_def_readable） */
        sk->sk_data_ready(sk);
}
```

**关键点**:
1. 不是每次收到数据都会调用回调
2. 需要满足唤醒条件（通过`tcp_epollin_ready`检查）
3. `sk->sk_data_ready`是一个函数指针，可以被替换

### 2.2 tcp_epollin_ready (include/net/tcp.h)

**功能**: 检查TCP socket是否有足够数据可读

**判断条件**:
```c
static inline bool tcp_epollin_ready(const struct sock *sk, int target)
{
    const struct tcp_sock *tp = tcp_sk(sk);
    /* 计算可用数据量：接收序号 - 已复制序号 */
    int avail = READ_ONCE(tp->rcv_nxt) - READ_ONCE(tp->copied_seq);

    /* 如果没有可用数据，返回false */
    if (avail <= 0)
        return false;

    /*
     * 满足以下任一条件即可唤醒：
     * 1. 可用数据 >= 目标字节数（通常是sk_rcvlowat）
     * 2. 接收缓冲区有内存压力
     * 3. 接收窗口 <= 一个MSS（避免小窗口综合征）
     */
    return (avail >= target) || tcp_rmem_pressure(sk) ||
           (tcp_receive_window(tp) <= inet_csk(sk)->icsk_ack.rcv_mss);
}
```

**唤醒条件**:
1. **数据量充足**: 可用数据 >= `sk_rcvlowat`（默认1字节）
2. **内存压力**: 接收缓冲区内存不足，需要尽快让应用读取
3. **窗口限制**: 接收窗口很小（≤ MSS），避免小窗口问题

### 2.3 sock_init_data (net/core/sock.c)

**功能**: 初始化socket数据结构

**关键初始化**:
```c
void sock_init_data(struct socket *sock, struct sock *sk)
{
    // ... 其他初始化 ...
    
    /*
     * 初始化回调函数为默认实现
     * 这些回调可以被协议栈或应用层替换
     */
    sk->sk_state_change  = sock_def_wakeup;       /* 状态变化回调 */
    sk->sk_data_ready    = sock_def_readable;     /* 数据就绪回调 */
    sk->sk_write_space   = sock_def_write_space;  /* 写空间可用回调 */
    sk->sk_error_report  = sock_def_error_report; /* 错误报告回调 */
    sk->sk_destruct      = sock_def_destruct;     /* 析构回调 */
    
    // ... 其他初始化 ...
}
```

**重要说明**:
- `sk_data_ready`默认指向`sock_def_readable`
- 某些协议或应用可能会替换这个回调
- 例如：TLS、BPF、RDS等会设置自己的回调

### 2.4 sock_def_readable (net/core/sock.c)

**功能**: socket默认的数据就绪回调函数

**源码**:
```c
void sock_def_readable(struct sock *sk)
{
    struct socket_wq *wq;

    rcu_read_lock();
    /* 获取socket的等待队列 */
    wq = rcu_dereference(sk->sk_wq);
    /* 如果有进程在等待队列上睡眠 */
    if (skwq_has_sleeper(wq))
        /*
         * 唤醒等待队列上的进程，通知以下事件：
         * EPOLLIN: 有数据可读
         * EPOLLPRI: 有紧急数据可读
         * EPOLLRDNORM: 有普通数据可读
         * EPOLLRDBAND: 有带外数据可读
         */
        wake_up_interruptible_sync_poll(&wq->wait, EPOLLIN | EPOLLPRI |
                        EPOLLRDNORM | EPOLLRDBAND);
    /* 异步通知（如SIGIO信号） */
    sk_wake_async(sk, SOCK_WAKE_WAITD, POLL_IN);
    rcu_read_unlock();
}
```

**工作流程**:
1. 获取socket的等待队列（`sk_wq`）
2. 检查是否有进程在等待
3. 调用`wake_up_interruptible_sync_poll()`唤醒等待进程
4. 触发异步通知（SIGIO信号）

**事件类型**:
- `EPOLLIN`: 有数据可读
- `EPOLLPRI`: 有紧急数据
- `EPOLLRDNORM`: 有普通数据
- `EPOLLRDBAND`: 有带外数据

### 2.5 ep_poll_callback (fs/eventpoll.c)

**功能**: epoll的poll回调函数

**注册时机**: 在`ep_insert()`中通过`ep_ptable_queue_proc()`注册

**源码关键部分**:
```c
static int ep_poll_callback(wait_queue_entry_t *wait, unsigned mode, int sync, void *key)
{
    int pwake = 0;
    struct epitem *epi = ep_item_from_wait(wait);
    struct eventpoll *ep = epi->ep;
    __poll_t pollflags = key_to_poll(key);
    unsigned long flags;
    int ewake = 0;

    read_lock_irqsave(&ep->lock, flags);

    /* 设置busy poll的NAPI ID */
    ep_set_busy_poll_napi_id(epi);

    /* 检查事件掩码是否匹配 */
    if (!(epi->event.events & ~EP_PRIVATE_BITS))
        goto out_unlock;

    if (pollflags && !(pollflags & epi->event.events))
        goto out_unlock;

    /*
     * 将事件加入就绪列表或溢出列表
     */
    if (READ_ONCE(ep->ovflist) != EP_UNACTIVE_PTR) {
        /* 扫描期间，将事件加入溢出列表 */
        if (chain_epi_lockless(epi))
            ep_pm_stay_awake_rcu(epi);
    } else if (!ep_is_linked(epi)) {
        /* 通常情况下，将事件添加到就绪列表 */
        if (list_add_tail_lockless(&epi->rdllink, &ep->rdllist))
            ep_pm_stay_awake_rcu(epi);
    }

    /* 唤醒epoll_wait等待的进程 */
    if (waitqueue_active(&ep->wq)) {
        wake_up(&ep->wq);
    }
    
    /* 如果有嵌套epoll在等待，标记需要唤醒 */
    if (waitqueue_active(&ep->poll_wait))
        pwake++;

out_unlock:
    read_unlock_irqrestore(&ep->lock, flags);

    /* 必须在锁外调用此函数（嵌套epoll唤醒） */
    if (pwake)
        ep_poll_safewake(ep, epi);

    return ewake;
}
```

**工作流程**:
1. **获取上下文**: 从等待队列项获取epitem和eventpoll
2. **检查事件**: 验证触发的事件是否匹配监控的事件
3. **加入就绪列表**: 
   - 正常情况：加入`rdllist`
   - 扫描期间：加入`ovflist`（溢出列表）
4. **唤醒进程**: 调用`wake_up(&ep->wq)`唤醒`epoll_wait`
5. **嵌套epoll**: 如果有嵌套epoll，调用`ep_poll_safewake`

**并发控制**:
- 使用读锁保护（`read_lock_irqsave`）
- 无锁操作就绪列表（`list_add_tail_lockless`）
- 使用cmpxchg检测并发

---

## 三、完整流程图

```
┌─────────────────────────────────────────────────────────────┐
│                    TCP数据包到达                              │
└─────────────────────┬───────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────────┐
│  tcp_rcv_established() / tcp_v4_do_rcv()                     │
│  - 处理TCP数据包                                              │
└─────────────────────┬───────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────────┐
│  tcp_data_queue(sk, skb)                                     │
│  - 将数据加入接收队列                                         │
│  - 顺序数据加入sk_receive_queue                               │
│  - 乱序数据加入out_of_order_queue                             │
└─────────────────────┬───────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────────┐
│  tcp_data_ready(sk)                                          │
│  ┌─────────────────────────────────────────────────────┐    │
│  │ if (tcp_epollin_ready(sk, sk->sk_rcvlowat) ||      │    │
│  │     sock_flag(sk, SOCK_DONE))                       │    │
│  │     sk->sk_data_ready(sk);                          │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────┬───────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────────┐
│  tcp_epollin_ready(sk, target)                               │
│  ┌─────────────────────────────────────────────────────┐    │
│  │ avail = rcv_nxt - copied_seq                        │    │
│  │ return (avail >= target) ||                         │    │
│  │        tcp_rmem_pressure(sk) ||                     │    │
│  │        (rcv_window <= mss)                          │    │
│  └─────────────────────────────────────────────────────┘    │
│  条件1: 数据量充足 (>= sk_rcvlowat)                          │
│  条件2: 内存压力                                              │
│  条件3: 窗口限制                                              │
└─────────────────────┬───────────────────────────────────────┘
                      │ 满足条件
                      ▼
┌─────────────────────────────────────────────────────────────┐
│  sk->sk_data_ready(sk)                                       │
│  默认: sock_def_readable(sk)                                 │
│  ┌─────────────────────────────────────────────────────┐    │
│  │ wq = rcu_dereference(sk->sk_wq);                    │    │
│  │ if (skwq_has_sleeper(wq))                           │    │
│  │     wake_up_interruptible_sync_poll(&wq->wait,      │    │
│  │         EPOLLIN | EPOLLPRI | EPOLLRDNORM | ...);    │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────┬───────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────────┐
│  wake_up_interruptible_sync_poll(&wq->wait, events)         │
│  - 遍历等待队列                                               │
│  - 调用每个等待项的回调函数                                   │
└─────────────────────┬───────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────────┐
│  ep_poll_callback(wait, mode, sync, key)                    │
│  ┌─────────────────────────────────────────────────────┐    │
│  │ 1. 检查事件掩码匹配                                  │    │
│  │ 2. 将epi加入就绪列表                                 │    │
│  │    - 正常: rdllist                                   │    │
│  │    - 扫描中: ovflist                                 │    │
│  │ 3. 唤醒epoll_wait                                    │    │
│  │    wake_up(&ep->wq);                                 │    │
│  │ 4. 处理嵌套epoll                                     │    │
│  │    ep_poll_safewake(ep, epi);                        │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────┬───────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────────┐
│  wake_up(&ep->wq)                                            │
│  - 唤醒epoll_wait中睡眠的进程                                 │
└─────────────────────┬───────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────────┐
│  ep_poll() 中的进程被唤醒                                     │
│  ┌─────────────────────────────────────────────────────┐    │
│  │ schedule_hrtimeout_range() 返回                      │    │
│  │ __set_current_state(TASK_RUNNING)                    │    │
│  │ 继续循环，调用 ep_send_events()                       │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────┬───────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────────┐
│  ep_send_events(ep, events, maxevents)                       │
│  - 扫描就绪列表                                               │
│  - 将事件复制到用户空间                                       │
└─────────────────────┬───────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────────┐
│  用户空间应用程序                                             │
│  epoll_wait() 返回                                           │
│  - 获取就绪的文件描述符                                       │
│  - 处理数据（read/recv）                                      │
└─────────────────────────────────────────────────────────────┘
```

---

## 四、关键数据结构

### 4.1 struct sock

```c
struct sock {
    // ... 其他字段 ...
    
    /* 回调函数指针 */
    void (*sk_state_change)(struct sock *sk);
    void (*sk_data_ready)(struct sock *sk);      // 数据就绪回调
    void (*sk_write_space)(struct sock *sk);
    void (*sk_error_report)(struct sock *sk);
    
    /* 等待队列 */
    struct socket_wq __rcu *sk_wq;               // socket等待队列
    
    /* TCP相关 */
    int sk_rcvlowat;                             // 接收低水位标记
    
    // ... 其他字段 ...
};
```

### 4.2 struct socket_wq

```c
struct socket_wq {
    wait_queue_head_t wait;                      // 等待队列头
    struct fasync_struct *fasync_list;           // 异步通知列表
    unsigned long flags;
    struct rcu_head rcu;
} ____cacheline_aligned_in_smp;
```

### 4.3 wait_queue_entry_t

```c
struct wait_queue_entry {
    unsigned int flags;
    void *private;
    wait_queue_func_t func;                      // 唤醒回调函数
    struct list_head entry;
};
```

对于epoll，`func`指向`ep_poll_callback`。

---

## 五、回调函数注册流程

### 5.1 socket初始化

```
socket() 系统调用
    ↓
sock_create()
    ↓
__sock_create()
    ↓
pf->create()  // 例如：inet_create()
    ↓
sk_alloc()
    ↓
sock_init_data(sock, sk)
    ↓
sk->sk_data_ready = sock_def_readable  // 设置默认回调
```

### 5.2 epoll回调注册

```
epoll_ctl(EPOLL_CTL_ADD)
    ↓
do_epoll_ctl()
    ↓
ep_insert()
    ↓
init_poll_funcptr(&epq.pt, ep_ptable_queue_proc)
    ↓
ep_item_poll(epi, &epq.pt, 1)
    ↓
vfs_poll(file, pt)
    ↓
file->f_op->poll(file, pt)  // 例如：tcp_poll()
    ↓
sock_poll_wait(file, whead, pt)
    ↓
poll_wait(file, whead, pt)
    ↓
pt->_qproc(file, whead, pt)
    ↓
ep_ptable_queue_proc()
    ↓
init_waitqueue_func_entry(&pwq->wait, ep_poll_callback)
add_wait_queue(whead, &pwq->wait)
```

**关键点**:
1. 创建`eppoll_entry`结构
2. 设置等待队列项的回调为`ep_poll_callback`
3. 将等待队列项添加到socket的等待队列

---

## 六、性能优化点

### 6.1 唤醒条件优化

**sk_rcvlowat**: 
- 默认值为1字节
- 可通过`setsockopt(SO_RCVLOWAT)`设置
- 较大的值可以减少唤醒次数，提高批处理效率

**内存压力检测**:
- 当接收缓冲区内存不足时，即使数据未达到`sk_rcvlowat`也会唤醒
- 避免因等待而导致的内存浪费

**窗口限制**:
- 当接收窗口很小时提前唤醒
- 避免小窗口综合征，提高吞吐量

### 6.2 无锁操作

**list_add_tail_lockless**:
- 使用原子操作添加到就绪列表
- 减少锁竞争
- 提高并发性能

**READ_ONCE/WRITE_ONCE**:
- 避免编译器优化导致的问题
- 保证内存可见性

### 6.3 Busy Poll

**ep_set_busy_poll_napi_id**:
- 记录网卡的NAPI ID
- 支持busy poll模式
- 减少延迟

---

## 七、特殊场景

### 7.1 嵌套epoll

当一个epoll fd被添加到另一个epoll时：

```
ep_poll_callback()
    ↓
if (waitqueue_active(&ep->poll_wait))
    pwake++;
    ↓
ep_poll_safewake(ep, epi)
    ↓
wake_up_poll(&ep->poll_wait, EPOLLIN)
    ↓
触发父epoll的ep_poll_callback()
```

**深度限制**: 最多4层嵌套（`EP_MAX_NESTS`）

### 7.2 EPOLLONESHOT模式

```c
if (!(epi->event.events & ~EP_PRIVATE_BITS))
    goto out_unlock;
```

当设置了`EPOLLONESHOT`后：
1. 第一次触发后，事件掩码被清除
2. `ep_poll_callback`检测到掩码为空，直接返回
3. 需要通过`EPOLL_CTL_MOD`重新启用

### 7.3 EPOLLEXCLUSIVE模式

```c
if ((epi->event.events & EPOLLEXCLUSIVE) &&
            !(pollflags & POLLFREE)) {
    switch (pollflags & EPOLLINOUT_BITS) {
    case EPOLLIN:
        if (epi->event.events & EPOLLIN)
            ewake = 1;
        break;
    // ...
    }
}
```

**独占唤醒**:
- 只唤醒一个等待进程
- 避免惊群效应
- 提高多进程/多线程场景性能

---

## 八、调试技巧

### 8.1 追踪点

```bash
# 追踪TCP数据接收
trace-cmd record -e tcp:tcp_probe

# 追踪epoll事件
trace-cmd record -e syscalls:sys_enter_epoll_wait \
                 -e syscalls:sys_exit_epoll_wait

# 追踪唤醒事件
trace-cmd record -e sched:sched_wakeup
```

### 8.2 内核参数

```bash
# 查看接收缓冲区大小
cat /proc/sys/net/ipv4/tcp_rmem

# 查看接收低水位标记（通过getsockopt查看）
```

### 8.3 常用工具

- `ss -tnp`: 查看TCP连接状态和接收队列
- `netstat -anp`: 查看网络统计
- `strace -e epoll_wait,read`: 追踪系统调用

---

## 九、总结

### 9.1 关键流程

1. **数据到达**: TCP数据包到达，加入接收队列
2. **条件检查**: `tcp_epollin_ready`检查是否满足唤醒条件
3. **回调触发**: 调用`sk->sk_data_ready`（默认`sock_def_readable`）
4. **唤醒等待队列**: `wake_up_interruptible_sync_poll`遍历等待队列
5. **epoll回调**: `ep_poll_callback`将事件加入就绪列表
6. **唤醒进程**: `wake_up(&ep->wq)`唤醒`epoll_wait`
7. **事件传输**: `ep_send_events`将事件复制到用户空间

### 9.2 设计亮点

1. **回调机制**: 灵活的函数指针设计，支持协议定制
2. **条件唤醒**: 智能的唤醒条件，平衡响应性和效率
3. **无锁优化**: 使用原子操作减少锁竞争
4. **嵌套支持**: 支持epoll嵌套，适应复杂场景
5. **模式多样**: 支持LT/ET、ONESHOT、EXCLUSIVE等多种模式

### 9.3 性能考虑

1. **批处理**: 通过`sk_rcvlowat`控制唤醒粒度
2. **内存管理**: 内存压力时及时唤醒，避免浪费
3. **并发控制**: 读写锁+无锁操作，提高并发性
4. **Busy Poll**: 支持低延迟场景

---

## 十、相关文件

- `net/ipv4/tcp_input.c`: TCP输入处理和`tcp_data_ready`
- `include/net/tcp.h`: `tcp_epollin_ready`定义
- `net/core/sock.c`: `sock_init_data`和`sock_def_readable`
- `fs/eventpoll.c`: epoll实现和`ep_poll_callback`
- `include/net/sock.h`: sock结构定义

所有相关函数都已添加详细的中文注释。
