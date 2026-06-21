+++
date = '2026-04-28'
title = 'Epoll 实现原理深度源码分析'
weight = 14
tags = [
    "epoll",
    "epoll_create",
    "epoll_ctl",
    "epoll_wait",
    "eventpoll",
    "epitem",
    "rdllist",
    "红黑树",
    "ET",
    "LT",
    "EPOLLONESHOT",
    "EPOLLEXCLUSIVE",
    "ovflist",
    "ep_poll_callback",
]
categories = [
    "网络",
]
+++
# Epoll 实现原理深度源码分析

> 基于 Linux 5.15.78，全面分析 epoll 三个核心系统调用的实现：`epoll_create`（创建）、`epoll_ctl`（添加/修改/删除 fd）、`epoll_wait`（等待事件）。包括核心数据结构（`eventpoll`/`epitem`/`eppoll_entry`）、红黑树管理、就绪链表机制、回调注册、ET/LT 模式差异、ovflist 溢出缓冲、嵌套 epoll 环检测。
>
> 数据到达后触发 epoll 唤醒的具体路径（`sk_data_ready` → `ep_poll_callback`）参见 [TCP 数据就绪与 Epoll 唤醒](TCP数据就绪与Epoll唤醒.md)。

---

## 目录

- [一、全景架构](#一全景架构)
- [二、核心数据结构](#二核心数据结构)
- [三、epoll_create：创建 epoll 实例](#三epoll_create创建-epoll-实例)
- [四、epoll_ctl：管理监控的 fd](#四epoll_ctl管理监控的-fd)
- [五、epoll_wait：等待事件](#五epoll_wait等待事件)
- [六、ET 与 LT 模式差异](#六et-与-lt-模式差异)
- [七、EPOLLONESHOT 与 EPOLLEXCLUSIVE](#七epolloneshot-与-epollexclusive)
- [八、ovflist 溢出缓冲机制](#八ovflist-溢出缓冲机制)
- [九、嵌套 epoll 与环检测](#九嵌套-epoll-与环检测)

---

## 一、全景架构

```
用户态                          内核态 (fs/eventpoll.c)
─────                          ──────────────────────

epoll_create1(flags)   ──→    do_epoll_create()
                               ├── ep_alloc()         → 分配 struct eventpoll
                               ├── get_unused_fd_flags()
                               ├── anon_inode_getfile("[eventpoll]")
                               └── fd_install()

epoll_ctl(epfd, op, fd, event) ──→ do_epoll_ctl()
                               ├── EPOLL_CTL_ADD → ep_insert()
                               │   ├── 红黑树插入 epitem
                               │   ├── ep_ptable_queue_proc() → 注册回调
                               │   └── 检查初始就绪 → 加入 rdllist
                               ├── EPOLL_CTL_MOD → ep_modify()
                               └── EPOLL_CTL_DEL → ep_remove()

epoll_wait(epfd, events, max, timeout) ──→ ep_poll()
                               ├── ep_events_available()? → ep_send_events()
                               │   ├── ep_start_scan() → splice rdllist
                               │   ├── 遍历: ep_item_poll() + __put_user()
                               │   ├── LT 模式: 重新加入 rdllist
                               │   └── ep_done_scan() → 合并 ovflist
                               └── 无事件 → __add_wait_queue_exclusive() → sleep
```

```
                    ┌─────────────────────────────────────┐
                    │         struct eventpoll             │
                    │                                     │
                    │  rbr (红黑树根)                       │
                    │    ├── epitem (fd=3, EPOLLIN)        │
                    │    ├── epitem (fd=5, EPOLLOUT)       │
                    │    └── epitem (fd=7, EPOLLIN|ET)     │
                    │                                     │
                    │  rdllist (就绪链表) ──→ epi_A ──→ epi_B │
                    │                                     │
                    │  wq (等待队列)      ←── epoll_wait 线程 │
                    │                                     │
                    │  ovflist           ←── 扫描期间的新事件  │
                    └─────────────────────────────────────┘
                              ↑ ep_poll_callback()
                              │ (目标 fd 就绪时回调)
                    ┌─────────┴───────────┐
                    │  目标 fd 的等待队列   │
                    │  (如 socket->wq)     │
                    └─────────────────────┘
```

---

## 二、核心数据结构

### 2.1 `struct eventpoll`

```c
// fs/eventpoll.c:208-253
struct eventpoll {
    struct mutex mtx;              // 保护红黑树和 ctl 操作
    wait_queue_head_t wq;          // epoll_wait 阻塞等待队列
    wait_queue_head_t poll_wait;   // 嵌套 epoll 时的 poll 等待队列
    struct list_head rdllist;      // ★ 就绪文件描述符链表
    rwlock_t lock;                 // 保护 rdllist 和 ovflist
    struct rb_root_cached rbr;     // ★ 红黑树根（存储所有监控的 fd）
    struct epitem *ovflist;        // ★ 溢出链表（扫描期间缓冲新事件）
    struct wakeup_source *ws;      // PM 唤醒源
    struct user_struct *user;      // 创建者用户（资源计数）
    struct file *file;             // 关联的 file 结构
    u64 gen;                       // 循环检测代数
    struct hlist_head refs;        // 引用哈希表头
};
```

### 2.2 `struct epitem`

每个被监控的 fd 对应一个 `epitem`：

```c
// fs/eventpoll.c:155-186
struct epitem {
    union {
        struct rb_node rbn;        // 红黑树节点（在 ep->rbr 中）
        struct rcu_head rcu;       // RCU 释放（复用 rbn 空间）
    };
    struct list_head rdllink;      // 就绪链表节点（在 ep->rdllist 中）
    struct epitem *next;           // ovflist 单链表指针
    struct epoll_filefd ffd;       // 文件+fd 组合键
    struct eppoll_entry *pwqlist;  // poll 等待队列项链表
    struct eventpoll *ep;          // 所属 eventpoll
    struct hlist_node fllink;      // 链入 file->f_ep
    struct wakeup_source __rcu *ws;// EPOLLWAKEUP 唤醒源
    struct epoll_event event;      // 用户关注的事件掩码和 data
};
```

### 2.3 `struct eppoll_entry`

将 epitem 链入目标 fd 等待队列的桥梁：

```c
// fs/eventpoll.c:126-138
struct eppoll_entry {
    struct eppoll_entry *next;     // 链入 epitem->pwqlist
    struct epitem *base;           // 指回所属 epitem
    wait_queue_entry_t wait;       // 等待队列项（func = ep_poll_callback）
    wait_queue_head_t *whead;      // 目标 fd 的等待队列头
};
```

### 2.4 三者关系

```
eventpoll
  ├── rbr (红黑树) ──→ epitem_A ──→ epitem_B ──→ ...
  │                      │
  │                      ├── ffd = {file, fd}
  │                      ├── event = {EPOLLIN, data}
  │                      └── pwqlist ──→ eppoll_entry
  │                                         ├── wait.func = ep_poll_callback
  │                                         └── whead → socket->wq
  │
  ├── rdllist ──→ epitem_A.rdllink ──→ epitem_C.rdllink
  │
  ├── wq ──→ epoll_wait 线程
  │
  └── ovflist ──→ epitem_X → epitem_Y → NULL (扫描期间)
```

---

## 三、epoll_create：创建 epoll 实例

### 3.1 系统调用入口

```c
// fs/eventpoll.c:2220-2223
SYSCALL_DEFINE1(epoll_create1, int, flags)
{
    return do_epoll_create(flags);
}

// fs/eventpoll.c:2232-2240
SYSCALL_DEFINE1(epoll_create, int, size)
{
    if (size <= 0)
        return -EINVAL;     // 历史遗留：size 必须 > 0（实际被忽略）
    return do_epoll_create(0);
}
```

### 3.2 `do_epoll_create()`

```c
// fs/eventpoll.c:2159-2212
static int do_epoll_create(int flags)
{
    int error, fd;
    struct eventpoll *ep = NULL;
    struct file *file;

    if (flags & ~EPOLL_CLOEXEC)
        return -EINVAL;

    // ① 分配并初始化 eventpoll
    error = ep_alloc(&ep);

    // ② 分配 fd
    fd = get_unused_fd_flags(O_RDWR | (flags & O_CLOEXEC));

    // ③ 创建匿名 inode 文件
    file = anon_inode_getfile("[eventpoll]", &eventpoll_fops, ep,
                              O_RDWR | (flags & O_CLOEXEC));

    // ④ 关联并安装
    ep->file = file;
    fd_install(fd, file);

    return fd;
}
```

### 3.3 `ep_alloc()`

```c
// fs/eventpoll.c:1052-1091
static int ep_alloc(struct eventpoll **pep)
{
    struct eventpoll *ep;

    ep = kzalloc(sizeof(*ep), GFP_KERNEL);

    mutex_init(&ep->mtx);                // 互斥锁
    rwlock_init(&ep->lock);              // 读写锁
    init_waitqueue_head(&ep->wq);        // epoll_wait 等待队列
    init_waitqueue_head(&ep->poll_wait); // 嵌套 epoll 等待队列
    INIT_LIST_HEAD(&ep->rdllist);        // 就绪链表
    ep->rbr = RB_ROOT_CACHED;           // 红黑树
    ep->ovflist = EP_UNACTIVE_PTR;       // 溢出链表（未激活标记）
    ep->user = get_current_user();

    *pep = ep;
    return 0;
}
```

### 3.4 `eventpoll_fops`

```c
// fs/eventpoll.c:998-1006
static const struct file_operations eventpoll_fops = {
    .release    = ep_eventpoll_release,  // close() 时释放资源
    .poll       = ep_eventpoll_poll,     // 支持嵌套 epoll
    .llseek     = noop_llseek,
};
```

---

## 四、epoll_ctl：管理监控的 fd

### 4.1 系统调用入口

```c
// fs/eventpoll.c:2422-2434
SYSCALL_DEFINE4(epoll_ctl, int, epfd, int, op, int, fd,
                struct epoll_event __user *, event)
{
    struct epoll_event epds;
    if (ep_op_has_event(op) &&
        copy_from_user(&epds, event, sizeof(struct epoll_event)))
        return -EFAULT;
    return do_epoll_ctl(epfd, op, fd, &epds, false);
}
```

### 4.2 `do_epoll_ctl()` 分发逻辑

```c
// fs/eventpoll.c:2264-2410（简化）
int do_epoll_ctl(int epfd, int op, int fd, struct epoll_event *epds, bool nonblock)
{
    // ① fd 查找与校验
    f = fdget(epfd);       // epoll fd
    tf = fdget(fd);        // 目标 fd
    if (!file_can_poll(tf.file))  return -EPERM;   // 必须支持 poll
    if (f.file == tf.file)        return -EINVAL;   // 不能监控自己
    if (!is_file_epoll(f.file))   return -EINVAL;   // epfd 必须是 epoll

    ep = f.file->private_data;

    // ② EPOLL_CTL_ADD 时的嵌套 epoll 环检测
    if (op == EPOLL_CTL_ADD && is_file_epoll(tf.file)) {
        tep = tf.file->private_data;
        if (ep_loop_check(ep, tep) != 0)
            return -ELOOP;
    }

    // ③ 红黑树查找
    epi = ep_find(ep, tf.file, fd);

    // ④ 分发操作
    switch (op) {
    case EPOLL_CTL_ADD:
        if (!epi) {
            epds->events |= EPOLLERR | EPOLLHUP;  // 强制监控错误和挂断
            error = ep_insert(ep, epds, tf.file, fd, full_check);
        } else error = -EEXIST;
        break;
    case EPOLL_CTL_DEL:
        if (epi) error = ep_remove(ep, epi);
        else error = -ENOENT;
        break;
    case EPOLL_CTL_MOD:
        if (epi) {
            epds->events |= EPOLLERR | EPOLLHUP;
            error = ep_modify(ep, epi, epds);
        } else error = -ENOENT;
        break;
    }
    return error;
}
```

### 4.3 `ep_insert()`：EPOLL_CTL_ADD 的核心

```c
// fs/eventpoll.c:1605-1724（简化关键路径）
static int ep_insert(struct eventpoll *ep, const struct epoll_event *event,
                     struct file *tfile, int fd, int full_check)
{
    // ① 资源限制检查
    if (percpu_counter_compare(&ep->user->epoll_watches, max_user_watches) >= 0)
        return -ENOSPC;

    // ② 分配 epitem
    epi = kmem_cache_zalloc(epi_cache, GFP_KERNEL);
    INIT_LIST_HEAD(&epi->rdllink);
    epi->ep = ep;
    ep_set_ffd(&epi->ffd, tfile, fd);
    epi->event = *event;
    epi->next = EP_UNACTIVE_PTR;

    // ③ 挂入 file->f_ep
    attach_epitem(tfile, epi);

    // ④ 插入红黑树
    ep_rbtree_insert(ep, epi);

    // ⑤ 注册回调并检查初始就绪状态
    epq.epi = epi;
    init_poll_funcptr(&epq.pt, ep_ptable_queue_proc);
    revents = ep_item_poll(epi, &epq.pt, 1);
    //         ↑ 这一步做两件事:
    //         a) 调用 vfs_poll() → 目标 fd 的 poll → ep_ptable_queue_proc → 注册回调
    //         b) 返回当前就绪事件

    // ⑥ 如果已经就绪，加入 rdllist
    write_lock_irq(&ep->lock);
    if (revents && !ep_is_linked(epi)) {
        list_add_tail(&epi->rdllink, &ep->rdllist);
        if (waitqueue_active(&ep->wq))
            wake_up(&ep->wq);       // 唤醒 epoll_wait 线程
    }
    write_unlock_irq(&ep->lock);

    return 0;
}
```

### 4.4 `ep_ptable_queue_proc()`：回调注册

当 `ep_item_poll()` 调用 `vfs_poll()` 时，目标 fd 的 `poll` 实现会调用 `poll_wait()`，最终触发 `ep_ptable_queue_proc()`：

```c
// fs/eventpoll.c:1380-1410
static void ep_ptable_queue_proc(struct file *file, wait_queue_head_t *whead,
                                 poll_table *pt)
{
    struct epitem *epi = container_of(pt, struct ep_pqueue, pt)->epi;

    // 分配 eppoll_entry
    pwq = kmem_cache_alloc(pwq_cache, GFP_KERNEL);

    // 设置回调函数为 ep_poll_callback
    init_waitqueue_func_entry(&pwq->wait, ep_poll_callback);
    pwq->whead = whead;
    pwq->base = epi;

    // 加入目标 fd 的等待队列
    if (epi->event.events & EPOLLEXCLUSIVE)
        add_wait_queue_exclusive(whead, &pwq->wait);
    else
        add_wait_queue(whead, &pwq->wait);

    // 链入 epitem 的 pwqlist
    pwq->next = epi->pwqlist;
    epi->pwqlist = pwq;
}
```

### 4.5 `ep_poll_callback()`：事件就绪回调

当目标 fd 的状态变化时（如 socket 收到数据），其等待队列唤醒所有 waiter，`ep_poll_callback` 被调用：

```c
// fs/eventpoll.c:1271-1372（简化关键路径）
static int ep_poll_callback(wait_queue_entry_t *wait, unsigned mode, int sync, void *key)
{
    struct epitem *epi = ep_item_from_wait(wait);
    struct eventpoll *ep = epi->ep;
    __poll_t pollflags = key_to_poll(key);

    read_lock_irqsave(&ep->lock, flags);

    // ① 检查事件掩码是否匹配
    if (!(epi->event.events & ~EP_PRIVATE_BITS))
        goto out_unlock;  // EPOLLONESHOT 已禁用
    if (pollflags && !(pollflags & epi->event.events))
        goto out_unlock;  // 事件不匹配

    // ② 加入就绪链表
    if (READ_ONCE(ep->ovflist) != EP_UNACTIVE_PTR) {
        // 正在扫描中，加入 ovflist 缓冲
        chain_epi_lockless(epi);
    } else if (!ep_is_linked(epi)) {
        // 正常路径，加入 rdllist
        list_add_tail_lockless(&epi->rdllink, &ep->rdllist);
    }

    // ③ 唤醒 epoll_wait 等待者
    if (waitqueue_active(&ep->wq))
        wake_up(&ep->wq);

    // ④ 唤醒嵌套 epoll
    if (waitqueue_active(&ep->poll_wait))
        pwake++;  // 锁外调用 ep_poll_safewake

    read_unlock_irqrestore(&ep->lock, flags);

    if (pwake)
        ep_poll_safewake(ep, epi);

    return ewake;
}
```

### 4.6 `ep_modify()`：EPOLL_CTL_MOD

```c
// fs/eventpoll.c:1736-1800（简化）
static int ep_modify(struct eventpoll *ep, struct epitem *epi,
                     const struct epoll_event *event)
{
    // ① 更新事件掩码
    epi->event.events = event->events;
    epi->event.data = event->data;

    // ② 内存屏障：确保 ep_poll_callback 看到新掩码
    smp_mb();

    // ③ 重新检查就绪状态
    if (ep_item_poll(epi, &pt, 1)) {
        write_lock_irq(&ep->lock);
        if (!ep_is_linked(epi)) {
            list_add_tail(&epi->rdllink, &ep->rdllist);
            if (waitqueue_active(&ep->wq))
                wake_up(&ep->wq);
        }
        write_unlock_irq(&ep->lock);
    }
    return 0;
}
```

### 4.7 `ep_remove()`：EPOLL_CTL_DEL

```c
// fs/eventpoll.c:756-808（简化）
static int ep_remove(struct eventpoll *ep, struct epitem *epi)
{
    // ① 从目标 fd 的等待队列中摘除回调
    ep_unregister_pollwait(ep, epi);  // remove_wait_queue + free eppoll_entry

    // ② 从 file->f_ep 哈希链删除
    hlist_del_rcu(&epi->fllink);

    // ③ 从红黑树删除
    rb_erase_cached(&epi->rbn, &ep->rbr);

    // ④ 从就绪链表删除
    write_lock_irq(&ep->lock);
    if (ep_is_linked(epi))
        list_del_init(&epi->rdllink);
    write_unlock_irq(&ep->lock);

    // ⑤ RCU 延迟释放
    call_rcu(&epi->rcu, epi_rcu_free);
    percpu_counter_dec(&ep->user->epoll_watches);
    return 0;
}
```

---

## 五、epoll_wait：等待事件

### 5.1 系统调用入口

```c
// fs/eventpoll.c:2490-2498
SYSCALL_DEFINE4(epoll_wait, int, epfd, struct epoll_event __user *, events,
                int, maxevents, int, timeout)
{
    struct timespec64 to;
    return do_epoll_wait(epfd, events, maxevents,
                         ep_timeout_to_timespec(&to, timeout));
}
```

超时转换：`timeout < 0` → `NULL`（永久阻塞）；`timeout == 0` → `{0,0}`（非阻塞）；`timeout > 0` → 绝对时间点。

### 5.2 `do_epoll_wait()`

```c
// fs/eventpoll.c:2445-2479
static int do_epoll_wait(int epfd, struct epoll_event __user *events,
                         int maxevents, struct timespec64 *to)
{
    // 参数校验: maxevents > 0, access_ok, is_file_epoll
    ep = f.file->private_data;
    error = ep_poll(ep, events, maxevents, to);
    return error;
}
```

### 5.3 `ep_poll()`：核心等待循环

```c
// fs/eventpoll.c:1960-2069（简化关键路径）
static int ep_poll(struct eventpoll *ep, struct epoll_event __user *events,
                   int maxevents, struct timespec64 *timeout)
{
    int res, eavail, timed_out = 0;
    wait_queue_entry_t wait;

    // 超时处理
    if (timeout && (timeout->tv_sec | timeout->tv_nsec)) {
        slack = select_estimate_accuracy(timeout);
        *to = timespec64_to_ktime(*timeout);
    } else if (timeout) {
        timed_out = 1;  // 非阻塞
    }

    // 初始检查
    eavail = ep_events_available(ep);

    while (1) {
        // ====== 有事件：收集并返回 ======
        if (eavail) {
            res = ep_send_events(ep, events, maxevents);
            if (res)
                return res;
        }

        if (timed_out)
            return 0;

        // busy poll 尝试
        eavail = ep_busy_loop(ep, timed_out);
        if (eavail)
            continue;

        if (signal_pending(current))
            return -EINTR;

        // ====== 无事件：进入睡眠 ======
        init_wait(&wait);
        wait.func = ep_autoremove_wake_function;  // 唤醒后自动从队列移除

        write_lock_irq(&ep->lock);
        __set_current_state(TASK_INTERRUPTIBLE);

        eavail = ep_events_available(ep);  // 锁下最终检查
        if (!eavail)
            __add_wait_queue_exclusive(&ep->wq, &wait);

        write_unlock_irq(&ep->lock);

        if (!eavail)
            timed_out = !schedule_hrtimeout_range(to, slack, HRTIMER_MODE_ABS);

        __set_current_state(TASK_RUNNING);

        // 唤醒后回到循环顶部收集事件
        eavail = 1;
        // ... 超时边界情况处理 ...
    }
}
```

### 5.4 `ep_events_available()`

```c
// fs/eventpoll.c:423-428
static inline int ep_events_available(struct eventpoll *ep)
{
    return !list_empty_careful(&ep->rdllist) ||
        READ_ONCE(ep->ovflist) != EP_UNACTIVE_PTR;
}
```

### 5.5 `ep_send_events()`：事件收集与用户态拷贝

```c
// fs/eventpoll.c:1810-1900（简化关键路径）
static int ep_send_events(struct eventpoll *ep,
                          struct epoll_event __user *events, int maxevents)
{
    struct epitem *epi, *tmp;
    LIST_HEAD(txlist);
    poll_table pt;
    int res = 0;

    init_poll_funcptr(&pt, NULL);  // 不再注册新回调，只查询就绪状态

    mutex_lock(&ep->mtx);
    ep_start_scan(ep, &txlist);   // splice rdllist → txlist, 激活 ovflist

    list_for_each_entry_safe(epi, tmp, &txlist, rdllink) {
        if (res >= maxevents)
            break;

        list_del_init(&epi->rdllink);

        // ① 重新验证就绪状态（可能已变化）
        revents = ep_item_poll(epi, &pt, 1);
        if (!revents)
            continue;

        // ② 拷贝到用户空间
        events = epoll_put_uevent(revents, epi->event.data, events);
        if (!events) {
            list_add(&epi->rdllink, &txlist);
            res = res ? res : -EFAULT;
            break;
        }
        res++;

        // ③ ★ ET/LT/ONESHOT 处理（关键差异点）
        if (epi->event.events & EPOLLONESHOT)
            epi->event.events &= EP_PRIVATE_BITS;   // 清除事件掩码
        else if (!(epi->event.events & EPOLLET))
            list_add_tail(&epi->rdllink, &ep->rdllist);  // LT: 重新加入
        // ET: 不重新加入（就是什么都不做）
    }

    ep_done_scan(ep, &txlist);    // 合并 ovflist，恢复 rdllist
    mutex_unlock(&ep->mtx);
    return res;
}
```

用户态拷贝：

```c
// include/linux/eventpoll.h:77-86
static inline struct epoll_event __user *
epoll_put_uevent(__poll_t revents, __u64 data, struct epoll_event __user *uevent)
{
    if (__put_user(revents, &uevent->events) ||
        __put_user(data, &uevent->data))
        return NULL;
    return uevent + 1;
}
```

---

## 六、ET 与 LT 模式差异

差异集中在 `ep_send_events()` 中事件拷贝后的处理：

```
ep_send_events() 拷贝事件到用户空间后:
    │
    ├── EPOLLONESHOT: epi->event.events &= EP_PRIVATE_BITS
    │   → 清除所有用户事件位，后续 ep_poll_callback 会跳过此 epi
    │   → 需要 EPOLL_CTL_MOD 重新激活
    │
    ├── EPOLLET (边沿触发): 什么都不做
    │   → epi 不回 rdllist → 下次 epoll_wait 不会返回此 epi
    │   → 只有 ep_poll_callback 被再次调用（新事件到达）才重新加入 rdllist
    │
    └── 默认 LT (水平触发): list_add_tail(&epi->rdllink, &ep->rdllist)
        → epi 重新回到 rdllist → 下次 epoll_wait 会再次检查
        → 如果 fd 仍然就绪，继续返回；不就绪则 ep_item_poll 返回 0，跳过
```

| 特性 | LT（默认） | ET（EPOLLET） |
|------|-----------|---------------|
| 拷贝后处理 | 重新加入 rdllist | 不加入 |
| 重复通知 | fd 就绪就持续通知 | 仅在状态变化边沿通知一次 |
| 使用模式 | 可以不一次读完 | 必须一次读完（否则丢事件） |
| 性能 | 每次 epoll_wait 都重新 poll | 减少不必要的 poll |

---

## 七、EPOLLONESHOT 与 EPOLLEXCLUSIVE

### EPOLLONESHOT

```
事件触发一次后:
  ep_send_events(): epi->event.events &= EP_PRIVATE_BITS
      ↓
  后续 ep_poll_callback():
      if (!(epi->event.events & ~EP_PRIVATE_BITS))  goto out_unlock;
      → 直接跳过，不再通知
      ↓
  必须 EPOLL_CTL_MOD 重新设置 events 才能收到下一次事件
```

用途：多线程 epoll，防止多个线程同时被同一 fd 唤醒。

### EPOLLEXCLUSIVE

在 `ep_ptable_queue_proc()` 中注册回调时：

```c
if (epi->event.events & EPOLLEXCLUSIVE)
    add_wait_queue_exclusive(whead, &pwq->wait);
else
    add_wait_queue(whead, &pwq->wait);
```

效果：多个 epoll 实例监控同一 fd 时，事件只唤醒一个 epoll（避免惊群）。

限制：
- 仅 `EPOLL_CTL_ADD` 时可设置（不能 MOD）
- 不能用于嵌套 epoll

---

## 八、ovflist 溢出缓冲机制

### 问题

`ep_send_events()` 在遍历 `rdllist` 拷贝事件到用户空间时，不能持有 `ep->lock`（因为要访问用户内存）。此时如果有新事件到达，`ep_poll_callback()` 不能直接操作 `rdllist`。

### 解决方案

```
ep_start_scan():
  write_lock_irq(&ep->lock)
  list_splice_init(&ep->rdllist, &txlist)  // 转移到私有列表
  WRITE_ONCE(ep->ovflist, NULL)            // ★ 激活 ovflist
  write_unlock_irq(&ep->lock)

↓ 扫描期间，ep_poll_callback():
  if (READ_ONCE(ep->ovflist) != EP_UNACTIVE_PTR)
      chain_epi_lockless(epi)              // 加入 ovflist（无锁 CAS）

↓ 扫描完成

ep_done_scan():
  write_lock_irq(&ep->lock)
  for (epi = ep->ovflist; epi != NULL; epi = epi->next)
      list_add(&epi->rdllink, &ep->rdllist)  // 合并回 rdllist
  WRITE_ONCE(ep->ovflist, EP_UNACTIVE_PTR)    // ★ 关闭 ovflist
  list_splice(&txlist, &ep->rdllist)          // 未处理完的也放回
  if (!list_empty(&ep->rdllist))
      wake_up(&ep->wq)                        // 唤醒下一轮 wait
  write_unlock_irq(&ep->lock)
```

`chain_epi_lockless()` 使用 `cmpxchg` + `xchg` 实现无锁 LIFO 入队：

```c
// fs/eventpoll.c:1231-1246
static inline bool chain_epi_lockless(struct epitem *epi)
{
    if (cmpxchg(&epi->next, EP_UNACTIVE_PTR, NULL) != EP_UNACTIVE_PTR)
        return false;              // 已在链上
    epi->next = xchg(&ep->ovflist, epi);  // 原子交换头指针
    return true;
}
```

---

## 九、嵌套 epoll 与环检测

epoll fd 本身可以被另一个 epoll 监控（嵌套）。为防止形成环（A 监控 B，B 监控 A），`EPOLL_CTL_ADD` 添加 epoll 类型的 fd 时执行环检测。

### 检测逻辑

```c
// fs/eventpoll.c:2079-2134
static int ep_loop_check_proc(struct eventpoll *ep, int depth)
{
    ep->gen = loop_check_gen;  // 标记已访问
    for (rbp = rb_first_cached(&ep->rbr); rbp; rbp = rb_next(rbp)) {
        epi = rb_entry(rbp, struct epitem, rbn);
        if (is_file_epoll(epi->ffd.file)) {
            ep_tovisit = epi->ffd.file->private_data;
            if (ep_tovisit->gen == loop_check_gen)
                continue;                  // 已访问，跳过
            if (ep_tovisit == inserting_into || depth > EP_MAX_NESTS)
                return -1;                 // ★ 检测到环或超过最大嵌套深度
            error = ep_loop_check_proc(ep_tovisit, depth + 1);  // 递归
            if (error != 0)
                break;
        }
    }
    return error;
}
```

### 嵌套 epoll 的 poll

当嵌套 epoll fd 被 poll 时，调用 `__ep_eventpoll_poll()`（`fs/eventpoll.c:894`），递归检查内层 epoll 的 `rdllist` 是否有就绪事件。`depth` 参数限制递归深度。

```
外层 epoll A 监控内层 epoll B:
  ep_item_poll(epi_for_B, &pt, depth)
    → is_file_epoll(B) ? __ep_eventpoll_poll(B, &pt, depth+1)
      → 检查 B 的 rdllist 是否有就绪事件
      → 有 → 返回 EPOLLIN | EPOLLRDNORM
      → 无 → 返回 0
```
