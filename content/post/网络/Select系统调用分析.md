+++
date = '2026-04-16'
title = 'Select系统调用源码流程分析'
weight = 16
tags = [
    "select",
    "do_select",
    "poll",
    "fd_set",
]
categories = [
    "网络",
]
+++
# Select系统调用源码流程分析

## 一、核心调用链

```
用户态: select(nfds, readfds, writefds, exceptfds, timeout)
    ↓
SYSCALL_DEFINE5(select)          // fs/select.c:871
    ↓
kern_select()                    // fs/select.c:837
    ↓
core_sys_select()                // fs/select.c:749
    ↓
do_select()                      // fs/select.c:578
    ├─→ poll_initwait()          // 初始化，设置_qproc = __pollwait
    ├─→ vfs_poll()               // 调用file->f_op->poll()
    │   └─→ sock_poll()          // socket文件的poll
    │       └─→ tcp_poll()       // TCP协议的poll
    │           └─→ sock_poll_wait()
    │               └─→ poll_wait()
    │                   └─→ __pollwait()  // 注册到等待队列
    ├─→ poll_schedule_timeout()  // 睡眠等待
    └─→ poll_freewait()          // 清理
```

---

## 二、系统调用入口

```c
// fs/select.c:871-874
SYSCALL_DEFINE5(select, int, n, fd_set __user *, inp, fd_set __user *, outp,
        fd_set __user *, exp, struct __kernel_old_timeval __user *, tvp)
{
    return kern_select(n, inp, outp, exp, tvp);
}

// fs/select.c:837-859
static int kern_select(int n, fd_set __user *inp, fd_set __user *outp,
               fd_set __user *exp, struct __kernel_old_timeval __user *tvp)
{
    struct timespec64 end_time, *to = NULL;
    struct __kernel_old_timeval tv;
    int ret;

    if (tvp) {
        if (copy_from_user(&tv, tvp, sizeof(tv)))  // 复制超时时间到内核
            return -EFAULT;

        to = &end_time;
        // 转换为绝对超时时间
        if (poll_select_set_timeout(to,
                tv.tv_sec + (tv.tv_usec / USEC_PER_SEC),
                (tv.tv_usec % USEC_PER_SEC) * NSEC_PER_USEC))
            return -EINVAL;
    }

    ret = core_sys_select(n, inp, outp, exp, to);
    return poll_select_finish(&end_time, tvp, PT_TIMEVAL, ret);
}
```

---

## 三、核心实现 - core_sys_select

```c
// fs/select.c:749-835
int core_sys_select(int n, fd_set __user *inp, fd_set __user *outp,
               fd_set __user *exp, struct timespec64 *end_time)
{
    fd_set_bits fds;
    void *bits;
    int ret, max_fds;
    size_t size, alloc_size;
    struct fdtable *fdt;
    long stack_fds[SELECT_STACK_ALLOC/sizeof(long)];  // 栈上预分配空间

    ret = -EINVAL;
    if (n < 0)
        goto out_nofds;

    // 获取当前进程的最大fd数，避免竞争
    rcu_read_lock();
    fdt = files_fdtable(current->files);
    max_fds = fdt->max_fds;
    rcu_read_unlock();
    if (n > max_fds)
        n = max_fds;

    // 计算需要的字节数，需要6个位图（输入3个 + 输出3个）
    size = FDS_BYTES(n);
    bits = stack_fds;  // 优先使用栈上内存

    // 栈上空间不足时使用堆内存
    if (size > sizeof(stack_fds) / 6) {
        ret = -ENOMEM;
        if (size > (SIZE_MAX / 6))
            goto out_nofds;

        alloc_size = 6 * size;
        bits = kvmalloc(alloc_size, GFP_KERNEL);
        if (!bits)
            goto out_nofds;
    }

    // 设置6个位图的指针
    fds.in      = bits;              // 输入：监控可读
    fds.out     = bits +   size;     // 输入：监控可写
    fds.ex      = bits + 2*size;     // 输入：监控异常
    fds.res_in  = bits + 3*size;     // 输出：就绪可读
    fds.res_out = bits + 4*size;     // 输出：就绪可写
    fds.res_ex  = bits + 5*size;     // 输出：就绪异常

    // 从用户空间复制输入位图
    if ((ret = get_fd_set(n, inp, fds.in)) ||
        (ret = get_fd_set(n, outp, fds.out)) ||
        (ret = get_fd_set(n, exp, fds.ex)))
        goto out;

    // 清零输出位图
    zero_fd_set(n, fds.res_in);
    zero_fd_set(n, fds.res_out);
    zero_fd_set(n, fds.res_ex);

    // 执行实际的select操作
    ret = do_select(n, &fds, end_time);

    if (ret < 0)
        goto out;
    if (!ret) {
        ret = -ERESTARTNOHAND;
        if (signal_pending(current))
            goto out;
        ret = 0;
    }

    // 将结果位图复制回用户空间
    if (set_fd_set(n, inp, fds.res_in) ||
        set_fd_set(n, outp, fds.res_out) ||
        set_fd_set(n, exp, fds.res_ex))
        ret = -EFAULT;

out:
    if (bits != stack_fds)
        kvfree(bits);
out_nofds:
    return ret;
}
```

---

## 四、核心轮询逻辑 - do_select

```c
// fs/select.c:578-734
static int do_select(int n, fd_set_bits *fds, struct timespec64 *end_time)
{
    ktime_t expire, *to = NULL;
    struct poll_wqueues table;
    poll_table *wait;
    int retval, i, timed_out = 0;
    u64 slack = 0;
    __poll_t busy_flag = net_busy_loop_on() ? POLL_BUSY_LOOP : 0;
    unsigned long busy_start = 0;

    // 获取实际需要检查的最大fd编号
    rcu_read_lock();
    retval = max_select_fd(n, fds);
    rcu_read_unlock();
    if (retval < 0)
        return retval;
    n = retval;

    // 初始化poll等待队列，设置_qproc = __pollwait
    poll_initwait(&table);
    wait = &table.pt;

    // 超时时间为0表示非阻塞模式
    if (end_time && !end_time->tv_sec && !end_time->tv_nsec) {
        wait->_qproc = NULL;  // 不注册回调
        timed_out = 1;
    }

    if (end_time && !timed_out)
        slack = select_estimate_accuracy(end_time);

    retval = 0;

    // 主循环
    for (;;) {
        unsigned long *rinp, *routp, *rexp, *inp, *outp, *exp;
        bool can_busy_loop = false;

        inp = fds->in; outp = fds->out; exp = fds->ex;
        rinp = fds->res_in; routp = fds->res_out; rexp = fds->res_ex;

        // 遍历所有位图字（每个long包含BITS_PER_LONG个fd）
        for (i = 0; i < n; ++rinp, ++routp, ++rexp) {
            unsigned long in, out, ex, all_bits, bit = 1, j;
            unsigned long res_in = 0, res_out = 0, res_ex = 0;
            __poll_t mask;

            in = *inp++; out = *outp++; ex = *exp++;
            all_bits = in | out | ex;

            // 如果这个long中没有任何监控的fd，跳过
            if (all_bits == 0) {
                i += BITS_PER_LONG;
                continue;
            }

            // 遍历long中的每一位
            for (j = 0; j < BITS_PER_LONG; ++j, ++i, bit <<= 1) {
                struct fd f;
                if (i >= n)
                    break;
                if (!(bit & all_bits))
                    continue;

                mask = EPOLLNVAL;
                f = fdget(i);
                if (f.file) {
                    // 设置等待的事件类型
                    wait_key_set(wait, in, out, bit, busy_flag);
                    // 调用文件的poll函数
                    mask = vfs_poll(f.file, wait);
                    fdput(f);
                }

                // 检查可读事件
                if ((mask & POLLIN_SET) && (in & bit)) {
                    res_in |= bit;
                    retval++;
                    wait->_qproc = NULL;  // 有就绪fd，不再注册回调
                }
                // 检查可写事件
                if ((mask & POLLOUT_SET) && (out & bit)) {
                    res_out |= bit;
                    retval++;
                    wait->_qproc = NULL;
                }
                // 检查异常事件
                if ((mask & POLLEX_SET) && (ex & bit)) {
                    res_ex |= bit;
                    retval++;
                    wait->_qproc = NULL;
                }

                if (retval) {
                    can_busy_loop = false;
                    busy_flag = 0;
                } else if (busy_flag & mask)
                    can_busy_loop = true;
            }

            // 保存本轮结果
            if (res_in)  *rinp = res_in;
            if (res_out) *routp = res_out;
            if (res_ex)  *rexp = res_ex;
            cond_resched();
        }

        // 第一轮后不再注册回调
        wait->_qproc = NULL;

        // 退出条件：有就绪fd、超时、或收到信号
        if (retval || timed_out || signal_pending(current))
            break;
        if (table.error) {
            retval = table.error;
            break;
        }

        // busy loop优化
        if (can_busy_loop && !need_resched()) {
            if (!busy_start) {
                busy_start = busy_loop_current_time();
                continue;
            }
            if (!busy_loop_timeout(busy_start))
                continue;
        }
        busy_flag = 0;

        // 设置超时
        if (end_time && !to) {
            expire = timespec64_to_ktime(*end_time);
            to = &expire;
        }

        // 进入睡眠等待
        if (!poll_schedule_timeout(&table, TASK_INTERRUPTIBLE, to, slack))
            timed_out = 1;
    }

    // 清理等待队列
    poll_freewait(&table);
    return retval;
}
```

---

## 五、等待队列机制

### 5.1 poll_initwait

```c
// fs/select.c:125-134
void poll_initwait(struct poll_wqueues *pwq)
{
    init_poll_funcptr(&pwq->pt, __pollwait);  // 设置回调函数
    pwq->polling_task = current;               // 记录当前任务
    pwq->triggered = 0;                        // 未触发
    pwq->error = 0;
    pwq->table = NULL;
    pwq->inline_index = 0;
}
```

### 5.2 __pollwait - 注册到文件的等待队列

```c
// fs/select.c:283-299
static void __pollwait(struct file *filp, wait_queue_head_t *wait_address,
                poll_table *p)
{
    struct poll_wqueues *pwq = container_of(p, struct poll_wqueues, pt);
    struct poll_table_entry *entry = poll_get_entry(pwq);  // 分配条目
    if (!entry)
        return;

    entry->filp = get_file(filp);           // 增加文件引用
    entry->wait_address = wait_address;     // 记录等待队列头
    entry->key = p->_key;                   // 记录感兴趣的事件

    // 设置唤醒回调为pollwake
    init_waitqueue_func_entry(&entry->wait, pollwake);
    entry->wait.private = pwq;

    // 将等待队列项添加到文件的等待队列
    add_wait_queue(wait_address, &entry->wait);
}
```

### 5.3 pollwake - 唤醒回调

```c
// fs/select.c:260-269
static int pollwake(wait_queue_entry_t *wait, unsigned mode, int sync, void *key)
{
    struct poll_table_entry *entry;
    entry = container_of(wait, struct poll_table_entry, wait);

    // 事件不匹配则不唤醒
    if (key && !(key_to_poll(key) & entry->key))
        return 0;

    return __pollwake(wait, mode, sync, key);
}

// fs/select.c:227-248
static int __pollwake(wait_queue_entry_t *wait, unsigned mode, int sync, void *key)
{
    struct poll_wqueues *pwq = wait->private;
    DECLARE_WAITQUEUE(dummy_wait, pwq->polling_task);

    smp_wmb();
    pwq->triggered = 1;  // 标记已触发

    // 唤醒select进程
    return default_wake_function(&dummy_wait, mode, sync, key);
}
```

### 5.4 poll_freewait - 清理资源

```c
// fs/select.c:155-177
void poll_freewait(struct poll_wqueues *pwq)
{
    struct poll_table_page *p = pwq->table;
    int i;

    // 释放内联条目
    for (i = 0; i < pwq->inline_index; i++)
        free_poll_entry(pwq->inline_entries + i);

    // 释放页表链表
    while (p) {
        struct poll_table_entry *entry;
        struct poll_table_page *old;

        entry = p->entry;
        do {
            entry--;
            free_poll_entry(entry);  // 从等待队列移除，释放文件引用
        } while (entry > p->entries);

        old = p;
        p = p->next;
        free_page((unsigned long) old);
    }
}
```

---

## 六、vfs_poll调用链（以TCP socket为例）

```
vfs_poll(file, wait)                           // include/linux/poll.h:86
    ↓
file->f_op->poll(file, pt)                     // file->f_op = socket_file_ops
    ↓
sock_poll(file, wait)                          // net/socket.c:1293
    ↓
sock->ops->poll(file, sock, wait)              // sock->ops = inet_stream_ops
    ↓
tcp_poll(file, sock, wait)                     // net/ipv4/tcp.c:499
    ↓
sock_poll_wait(file, sock, wait)               // include/net/sock.h:2263
    ↓
poll_wait(file, &sock->wq.wait, wait)          // include/linux/poll.h:48
    ↓
wait->_qproc(file, &sock->wq.wait, wait)       // 对于select: _qproc = __pollwait
    ↓
__pollwait()                                   // fs/select.c:283
    ↓
add_wait_queue(&sock->wq.wait, &entry->wait)   // kernel/sched/wait.c:18
```

---

## 七、唤醒流程（数据到达socket时）

```
TCP数据到达
    ↓
tcp_data_ready(sk)                             // net/ipv4/tcp_input.c:4992
    ↓
sk->sk_data_ready(sk)                          // = sock_def_readable
    ↓
sock_def_readable(sk)                          // net/core/sock.c:3051
    ↓
wake_up_interruptible_sync_poll(&wq->wait, EPOLLIN | ...)
    ↓
__wake_up_common()                             // kernel/sched/wait.c:103
    ↓
curr->func()                                   // = pollwake
    ↓
pollwake()                                     // fs/select.c:260
    ↓
__pollwake()                                   // fs/select.c:227
    ├─→ pwq->triggered = 1
    └─→ default_wake_function()                // kernel/sched/core.c:6753
            ↓
        try_to_wake_up()                       // 将进程加入运行队列
```

---

## 八、关键数据结构

```c
// 文件描述符位图集合
typedef struct {
    unsigned long *in;       // 输入：监控可读
    unsigned long *out;      // 输入：监控可写
    unsigned long *ex;       // 输入：监控异常
    unsigned long *res_in;   // 输出：就绪可读
    unsigned long *res_out;  // 输出：就绪可写
    unsigned long *res_ex;   // 输出：就绪异常
} fd_set_bits;

// poll等待队列（include/linux/poll.h:103-111）
struct poll_wqueues {
    poll_table pt;           // 包含_qproc回调函数指针
    struct poll_table_page *table;
    struct task_struct *polling_task;
    int triggered;           // 是否被唤醒
    int error;
    int inline_index;
    struct poll_table_entry inline_entries[N_INLINE_POLL_ENTRIES];
};

// poll table条目（include/linux/poll.h:93-98）
struct poll_table_entry {
    struct file *filp;
    __poll_t key;                    // 感兴趣的事件
    wait_queue_entry_t wait;         // 等待队列项
    wait_queue_head_t *wait_address; // 等待队列头
};

// poll table（include/linux/poll.h:43-46）
typedef struct poll_table_struct {
    poll_queue_proc _qproc;  // 回调函数（select时为__pollwait）
    __poll_t _key;           // 事件掩码
} poll_table;
```

---

## 九、事件掩码定义

```c
// fs/select.c:67-77
#define POLLIN_SET  (EPOLLRDNORM | EPOLLRDBAND | EPOLLIN | EPOLLHUP | EPOLLERR)
#define POLLOUT_SET (EPOLLWRBAND | EPOLLWRNORM | EPOLLOUT | EPOLLERR)
#define POLLEX_SET  (EPOLLPRI)
```

| 事件类型 | 含义 |
|---------|------|
| EPOLLIN | 有数据可读 |
| EPOLLOUT | 可以写入数据 |
| EPOLLPRI | 有紧急数据 |
| EPOLLHUP | 连接挂断 |
| EPOLLERR | 发生错误 |

---

## 十、select vs epoll

| 特性 | select | epoll |
|-----|--------|-------|
| 数据结构 | 位图 | 红黑树 + 就绪链表 |
| 注册方式 | 每次都重新注册 | 只注册一次 |
| 就绪通知 | 遍历所有fd | 只返回就绪fd |
| 时间复杂度 | O(n) | O(活跃连接数) |
| fd数量限制 | FD_SETSIZE (1024) | 无限制 |

---

## 十一、相关源码文件

| 文件 | 功能 |
|-----|------|
| fs/select.c | select/poll实现 |
| include/linux/poll.h | poll数据结构和内联函数 |
| net/socket.c | sock_poll实现 |
| net/ipv4/tcp.c | tcp_poll实现 |
| include/net/sock.h | sock_poll_wait实现 |
| kernel/sched/wait.c | 等待队列操作 |
| kernel/sched/core.c | try_to_wake_up实现 |
