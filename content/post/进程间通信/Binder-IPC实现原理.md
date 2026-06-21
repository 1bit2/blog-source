+++
date = '2026-04-16'
title = 'Binder IPC 实现原理分析（基于 Linux 5.15.78 内核源码）'
weight = 1
tags = [
    "Binder",
    "IPC",
    "binder_transaction",
    "mmap",
    "buffer_exhaustion",
    "oneway_spam",
]
categories = [
    "进程间通信",
]
+++
# Binder IPC 实现原理分析（基于 Linux 5.15.78 内核源码）

## 目录

- [一、Binder 核心数据结构](#一binder-核心数据结构)
- [二、Binder 发送数据流程](#二binder-发送数据流程)
- [三、Binder 接收数据流程](#三binder-接收数据流程)
- [四、同步 IPC 与异步 IPC 的区别](#四同步-ipc-与异步-ipc-的区别)
- [五、BINDER_TYPE_BINDER 消息与 binder_node 创建](#五binder_type_binder-消息与-binder_node-创建)
- [六、Binder 内存申请与大小限制](#六binder-内存申请与大小限制)
- [七、Buffer 空间耗尽时的行为分析](#七buffer-空间耗尽时的行为分析)
- [八、Todo 列表容量与请求数量限制](#八todo-列表容量与请求数量限制)
- [关键源码文件索引](#关键源码文件索引)

---

## 一、Binder 核心数据结构

### 1.1 进程描述：struct binder_proc

每个打开 `/dev/binder` 的进程对应一个 `binder_proc`（`drivers/android/binder_internal.h` 第 291 行）：

```c
struct binder_proc {
    struct hlist_node proc_node;        // 全局 binder_procs 链表节点
    struct rb_root threads;             // 该进程的所有 binder 线程（红黑树）
    struct rb_root nodes;               // 该进程拥有的所有 binder_node（红黑树，按 ptr 索引）
    struct rb_root refs_by_desc;        // 该进程持有的所有引用（按 desc 索引）
    struct rb_root refs_by_node;        // 该进程持有的所有引用（按 node 索引）
    struct list_head waiting_threads;   // 空闲等待中的线程列表
    int pid;
    struct task_struct *tsk;
    struct list_head todo;              // 【关键】进程级待处理工作队列
    struct binder_alloc alloc;          // 该进程的 binder 内存分配器
    // ...
};
```

### 1.2 线程描述：struct binder_thread

每个参与 binder 通信的线程对应一个 `binder_thread`（第 355 行）：

```c
struct binder_thread {
    struct binder_proc *proc;                       // 所属进程
    struct rb_node rb_node;                         // 在 proc->threads 红黑树中的节点
    struct list_head waiting_thread_node;            // 在 proc->waiting_threads 中的节点
    int pid;
    int looper;                                     // 线程状态标志
    struct binder_transaction *transaction_stack;    // 【关键】事务栈（同步调用链）
    struct list_head todo;                          // 【关键】线程私有待处理工作队列
    wait_queue_head_t wait;                         // 等待队列头（用于睡眠/唤醒）
    // ...
};
```

### 1.3 服务实体：struct binder_node

每个注册到 binder 的服务对象对应一个 `binder_node`（第 215 行）：

```c
struct binder_node {
    struct binder_work work;
    struct binder_proc *proc;           // 拥有此 node 的进程
    struct hlist_head refs;             // 所有指向此 node 的 binder_ref 链表
    binder_uintptr_t ptr;              // 【关键】用户空间 BBinder 对象地址
    binder_uintptr_t cookie;           // 用户空间 cookie
    bool has_async_transaction;         // 【关键】是否有异步事务正在处理
    struct list_head async_todo;        // 【关键】等待处理的异步事务队列
    // ... 引用计数、优先级等字段
};
```

### 1.4 服务引用：struct binder_ref

客户端进程通过 `binder_ref` 引用远程 `binder_node`（第 303 行）：

```c
struct binder_ref {
    struct binder_ref_data data;        // desc（句柄号）、引用计数
    struct rb_node rb_node_desc;        // 在 proc->refs_by_desc 红黑树中
    struct rb_node rb_node_node;        // 在 proc->refs_by_node 红黑树中
    struct hlist_node node_entry;       // 在 node->refs 链表中
    struct binder_proc *proc;           // 持有此引用的进程
    struct binder_node *node;           // 指向的目标 node
};
```

### 1.5 事务描述：struct binder_transaction

每次 binder 调用产生一个 `binder_transaction`（第 387 行）：

```c
struct binder_transaction {
    struct binder_work work;            // 工作项（类型为 BINDER_WORK_TRANSACTION）
    struct binder_thread *from;         // 发送方线程（异步 IPC 时为 NULL）
    struct binder_transaction *from_parent;  // 发送方事务栈上的父事务
    struct binder_proc *to_proc;        // 目标进程
    struct binder_thread *to_thread;    // 目标线程（异步时可能为 NULL）
    struct binder_transaction *to_parent;    // 目标侧事务栈上的父事务
    unsigned need_reply:1;              // 【关键】是否需要回复（同步 IPC = 1）
    struct binder_buffer *buffer;       // 在目标进程中分配的数据缓冲区
    unsigned int code;                  // 事务 code（如 PING_TRANSACTION）
    unsigned int flags;                 // 标志（TF_ONE_WAY 表示异步）
};
```

### 1.6 工作项：struct binder_work

所有排队到 todo 列表的工作都是 `binder_work`（第 148 行）：

```c
struct binder_work {
    struct list_head entry;
    enum binder_work_type {
        BINDER_WORK_TRANSACTION = 1,          // 收到事务
        BINDER_WORK_TRANSACTION_COMPLETE,      // 事务发送完成确认
        BINDER_WORK_RETURN_ERROR,
        BINDER_WORK_NODE,                      // node 引用计数变化通知
        BINDER_WORK_DEAD_BINDER,
        BINDER_WORK_DEAD_BINDER_AND_CLEAR,
        BINDER_WORK_CLEAR_DEATH_NOTIFICATION,
    } type;
};
```

### 1.7 数据结构关系图

```
  进程 A (Client)                              进程 B (Server)
 ┌─────────────────┐                        ┌─────────────────┐
 │  binder_proc     │                        │  binder_proc     │
 │  ├── threads     │                        │  ├── threads     │
 │  │   └── thread  │                        │  │   └── thread  │
 │  │      ├── todo │                        │  │      ├── todo │
 │  │      └── wait │                        │  │      └── wait │
 │  ├── todo        │                        │  ├── todo        │
 │  ├── refs_by_desc│                        │  ├── nodes       │
 │  │   └── ref ────┼─── desc:0 ──────────→  │  │   └── node   │
 │  │      (handle) │                        │  │     ├── ptr   │
 │  └── alloc       │                        │  │     ├── refs  │
 │                  │                        │  │     └── async │
 │                  │                        │  │        _todo   │
 └─────────────────┘                        │  └── alloc       │
                                            └─────────────────┘

  Client 通过 handle (desc) ──→ binder_ref ──→ binder_node ──→ Server
```

---

## 二、Binder 发送数据流程

### 2.1 完整调用链

```
用户空间: ioctl(fd, BINDER_WRITE_READ, &bwr)
    │
    ▼
binder_ioctl()                              // drivers/android/binder.c:5175
    └── case BINDER_WRITE_READ:
        └── binder_ioctl_write_read()       // binder.c:4871
            ├── binder_thread_write()       // binder.c:3545  (处理 write_buffer)
            │   └── case BC_TRANSACTION:
            │       └── binder_transaction()// binder.c:2695  【核心函数】
            │           ├── 查找目标进程/线程
            │           ├── binder_alloc_new_buf()  // 在目标进程分配缓冲区
            │           ├── 拷贝数据到目标缓冲区
            │           ├── 翻译 flat_binder_object
            │           ├── 入队到目标 todo 列表
            │           └── 唤醒目标线程
            │
            └── binder_thread_read()        // binder.c:4181  (处理 read_buffer)
                └── 等待并读取回复
```

### 2.2 ioctl 入口

用户空间通过 `ioctl(fd, BINDER_WRITE_READ, &bwr)` 发起通信（`binder.c` 第 5200 行）：

```c
// drivers/android/binder.c:5200
switch (cmd) {
case BINDER_WRITE_READ:
    ret = binder_ioctl_write_read(filp, cmd, arg, thread);
    break;
```

`binder_ioctl_write_read()` 从用户空间拷贝 `struct binder_write_read`，然后先调用 `binder_thread_write()` 处理写缓冲区，再调用 `binder_thread_read()` 处理读缓冲区：

```c
// drivers/android/binder.c:4871
static int binder_ioctl_write_read(...)
{
    struct binder_write_read bwr;
    copy_from_user(&bwr, ubuf, sizeof(bwr));

    if (bwr.write_size > 0) {
        ret = binder_thread_write(proc, thread,
                      bwr.write_buffer, bwr.write_size,
                      &bwr.write_consumed);
    }
    if (bwr.read_size > 0) {
        ret = binder_thread_read(proc, thread, bwr.read_buffer,
                     bwr.read_size, &bwr.read_consumed,
                     filp->f_flags & O_NONBLOCK);
    }
}
```

### 2.3 binder_thread_write() 处理 BC_TRANSACTION

`binder_thread_write()` 解析写缓冲区中的命令（第 3545 行），遇到 `BC_TRANSACTION` 时调用 `binder_transaction()`：

```c
// drivers/android/binder.c:3756
case BC_TRANSACTION:
case BC_REPLY: {
    struct binder_transaction_data tr;
    copy_from_user(&tr, ptr, sizeof(tr));
    ptr += sizeof(tr);
    binder_transaction(proc, thread, &tr,
                       cmd == BC_REPLY, 0);
    break;
}
```

### 2.4 binder_transaction() 核心流程

这是 binder 驱动中最核心的函数（第 2695 行），负责查找目标、分配缓冲区、拷贝数据、入队事务：

```c
// drivers/android/binder.c:2695 (省略错误处理和非关键分支)
static void binder_transaction(struct binder_proc *proc,
                               struct binder_thread *thread,
                               struct binder_transaction_data *tr, int reply, ...)
{
    // 1. 查找目标进程和线程
    if (tr->target.handle) {
        struct binder_ref *ref;
        ref = binder_get_ref_olocked(proc, tr->target.handle, true);
        target_node = binder_get_node_refs_for_txn(ref->node, &target_proc, ...);
    } else {
        target_node = context->binder_context_mgr_node;  // handle=0 → servicemanager
    }

    // 2. 在目标进程的地址空间中分配缓冲区
    // 这是 binder "一次拷贝" 的关键: 数据从发送方用户空间拷贝到目标进程的内核/用户共享映射区
    t->buffer = binder_alloc_new_buf(&target_proc->alloc, tr->data_size,
        tr->offsets_size, extra_buffers_size,
        !reply && (t->flags & TF_ONE_WAY), current->tgid);

    // 3. 拷贝数据并翻译 binder 对象
    // 将用户空间的 binder_transaction_data 拷贝到目标缓冲区
    // 翻译 flat_binder_object (如 BINDER_TYPE_BINDER → BINDER_TYPE_HANDLE, 详见第五节)
    // ... 省略拷贝和翻译代码 ...

    // 4. 设置事务的发送方信息
    if (!reply && !(tr->flags & TF_ONE_WAY))
        t->from = thread;      // 同步IPC: 记录发送方线程(等待回复时需要)
    else
        t->from = NULL;         // 异步IPC或回复: 不记录发送方

    // 5. 根据同步/异步选择不同的入队策略 (详见第四节)
    // ... 省略入队逻辑 ...
}
```

---

## 三、Binder 接收数据流程

### 3.1 线程等待机制

当 `binder_thread_read()` 发现没有待处理的工作时，调用 `binder_wait_for_work()` 进入睡眠（第 4054 行）：

```c
// drivers/android/binder.c:4054
static int binder_wait_for_work(struct binder_thread *thread, bool do_proc_work)
{
    DEFINE_WAIT(wait);
    struct binder_proc *proc = thread->proc;

    for (;;) {
        prepare_to_wait(&thread->wait, &wait, TASK_INTERRUPTIBLE);

        // 检查是否有工作
        if (binder_has_work_ilocked(thread, do_proc_work))
            break;

        // 将自己加入空闲线程列表，等待被选中
        if (do_proc_work)
            list_add(&thread->waiting_thread_node, &proc->waiting_threads);

        schedule();  // 【阻塞点】线程在此睡眠

        list_del_init(&thread->waiting_thread_node);
        if (signal_pending(current)) {
            ret = -EINTR;
            break;
        }
    }
    finish_wait(&thread->wait, &wait);
    return ret;
}
```

### 3.2 工作队列优先级

被唤醒后，`binder_thread_read()` 按优先级从 todo 队列取出工作项（第 4255 行）：

```c
// drivers/android/binder.c:4255
// 【优先级规则】
// 1. 先检查线程私有队列 thread->todo
// 2. 再检查进程公共队列 proc->todo
if (!binder_worklist_empty_ilocked(&thread->todo))
    list = &thread->todo;
else if (!binder_worklist_empty_ilocked(&proc->todo) && wait_for_proc_work)
    list = &proc->todo;
else {
    // 没有工作，重试或退出
    break;
}

w = binder_dequeue_work_head_ilocked(list);
```

### 3.3 处理不同类型的工作项

```c
// drivers/android/binder.c:4281
switch (w->type) {
case BINDER_WORK_TRANSACTION: {
    // 收到事务，生成 BR_TRANSACTION 或 BR_REPLY
    struct binder_transaction *t = container_of(w, struct binder_transaction, work);
    // ...
}
case BINDER_WORK_TRANSACTION_COMPLETE: {
    // 发送完成确认，生成 BR_TRANSACTION_COMPLETE
    cmd = BR_TRANSACTION_COMPLETE;
    // ...
}
```

### 3.4 区分 BR_TRANSACTION 和 BR_REPLY

```c
// drivers/android/binder.c:4465
if (t->buffer->target_node) {
    // target_node 非空 → 这是一个新的传入事务
    trd->target.ptr = target_node->ptr;
    trd->cookie = target_node->cookie;
    cmd = BR_TRANSACTION;
} else {
    // target_node 为空 → 这是对同步调用的回复
    trd->target.ptr = 0;
    trd->cookie = 0;
    cmd = BR_REPLY;
}
```

### 3.5 完整的一次同步 IPC 流程

```
Client (进程A)                              Server (进程B)
    │                                            │
    │  ioctl(BINDER_WRITE_READ)                  │  ioctl(BINDER_WRITE_READ)
    │  write: BC_TRANSACTION                     │  read: (等待中...)
    │     │                                      │     │
    │     ▼                                      │     │
    │  binder_transaction()                      │     │
    │  ├─ 分配目标缓冲区                          │     │
    │  ├─ 拷贝数据                               │     │
    │  ├─ tcomplete → thread_A->todo             │     │
    │  │   (BINDER_WORK_TRANSACTION_COMPLETE)    │     │
    │  ├─ t→work → thread_B->todo 或 proc_B->todo│    │
    │  │   (BINDER_WORK_TRANSACTION)             │     │
    │  ├─ t->need_reply = 1                      │     │
    │  ├─ thread_A->transaction_stack = t        │     │
    │  └─ wake_up(thread_B)                      │     │
    │     │                                      │  ◄──┘ 被唤醒
    │     │                                      │
    │  read: BR_TRANSACTION_COMPLETE             │  read: BR_TRANSACTION
    │  (从 thread_A->todo 取出 tcomplete)         │  (从 todo 取出 t->work)
    │     │                                      │     │
    │  read: (继续等待回复...)                     │  处理请求...
    │  binder_wait_for_work() ← 阻塞             │     │
    │     │                                      │  write: BC_REPLY
    │     │                                      │     │
    │     │                                      │  binder_transaction(reply=true)
    │     │                                      │  ├─ 弹出发送方事务栈
    │     │                                      │  ├─ reply → thread_A->todo
    │     │                                      │  └─ wake_up(thread_A)
    │  ◄──┘ 被唤醒                                │
    │                                            │  read: BR_TRANSACTION_COMPLETE
    │  read: BR_REPLY                            │
    │  (从 thread_A->todo 取出回复事务)            │
    │                                            │
    ▼                                            ▼
  返回用户空间                                   返回用户空间
```

---

## 四、同步 IPC 与异步 IPC 的区别

### 4.1 核心区别

| 特性 | 同步 IPC | 异步 IPC (TF_ONE_WAY) |
|------|---------|----------------------|
| `t->flags` | 无 `TF_ONE_WAY` | 有 `TF_ONE_WAY` |
| `t->from` | 发送方线程指针 | NULL |
| `t->need_reply` | 1 | 0 |
| 发送方行为 | 阻塞等待 BR_REPLY | 立即收到 BR_TRANSACTION_COMPLETE 后返回 |
| `transaction_stack` | 压栈（建立调用链） | 不压栈 |
| 目标线程选择 | 可能指定特定线程 | 传 NULL，由 binder 选择 |
| 目标队列 | `thread->todo` 或 `proc->todo` | `proc->todo` 或 `node->async_todo` |
| 串行化保证 | 无（并发处理） | 同一 node 的异步事务串行处理 |

### 4.2 同步 IPC 的入队逻辑

```c
// drivers/android/binder.c:3344
} else if (!(t->flags & TF_ONE_WAY)) {
    // 【同步 IPC】
    binder_enqueue_deferred_thread_work_ilocked(thread, tcomplete);
    t->need_reply = 1;
    t->from_parent = thread->transaction_stack;
    thread->transaction_stack = t;    // 事务压栈
    return_error = binder_proc_transaction(t, target_proc, target_thread);
```

同步 IPC 使用 **deferred** 方式入队 `TRANSACTION_COMPLETE`，这意味着发送方线程不会立即返回用户空间处理 COMPLETE，而是继续在内核中阻塞等待回复。这减少了一次上下文切换，降低延迟。

### 4.3 异步 IPC 的入队逻辑

```c
// drivers/android/binder.c:3369
} else {
    // 【异步 IPC (TF_ONE_WAY)】
    BUG_ON(target_node == NULL);
    BUG_ON(t->buffer->async_transaction != 1);
    binder_enqueue_thread_work(thread, tcomplete);  // 立即返回 COMPLETE
    // 传 NULL 作为 target_thread → 不指定目标线程
    return_error = binder_proc_transaction(t, target_proc, NULL);
}
```

异步 IPC 立即将 `TRANSACTION_COMPLETE` 入队到发送方线程，发送方可以立即返回用户空间。

### 4.4 todo 队列的三级入队策略

`binder_proc_transaction()` 中的入队逻辑（第 2605 行）是理解 binder 调度的关键：

```c
// drivers/android/binder.c:2605
// 【异步事务串行化检查】
if (oneway) {
    BUG_ON(thread);  // 异步 IPC 不指定目标线程
    if (node->has_async_transaction)
        pending_async = true;       // 该 node 已有异步事务在处理
    else
        node->has_async_transaction = true;  // 标记自己是第一个
}

// 【三级入队策略】
if (!thread && !pending_async)
    thread = binder_select_thread_ilocked(proc);  // 从空闲线程中选一个

if (thread)
    // 情况 1：入队到线程私有队列（同步回复 / 被选中的空闲线程）
    binder_enqueue_thread_work_ilocked(thread, &t->work);
else if (!pending_async)
    // 情况 2：入队到进程公共队列（没有空闲线程）
    binder_enqueue_work_ilocked(&t->work, &proc->todo);
else
    // 情况 3：入队到 node 异步等待队列（该 node 已有异步事务在处理）
    binder_enqueue_work_ilocked(&t->work, &node->async_todo);
```

### 4.5 异步事务的串行化机制

**为什么异步事务需要串行化？** 因为异步调用没有返回值，发送方不等待完成，可能短时间内发送大量异步事务。如果全部并发处理会耗尽目标进程的资源。

**串行化机制**：同一个 `binder_node` 的异步事务**一次只处理一个**。当前异步事务完成（缓冲区被释放）后，才从 `node->async_todo` 取出下一个：

```c
// drivers/android/binder.c:3510 — binder_free_buf() 中
if (buffer->async_transaction && buffer->target_node) {
    struct binder_node *buf_node = buffer->target_node;

    w = binder_dequeue_work_head_ilocked(&buf_node->async_todo);

    if (!w) {
        // 没有更多异步事务等待
        buf_node->has_async_transaction = false;
    } else {
        // 将下一个异步事务移到 proc->todo，唤醒空闲线程处理
        binder_enqueue_work_ilocked(w, &proc->todo);
        binder_wakeup_proc_ilocked(proc);
    }
}
```

### 4.6 多进程向 A 进程发送大量 IPC 时的行为

**场景：进程 B、C、D 同时向进程 A 的服务 node_X 发送大量 IPC**

**同步 IPC 场景：**

```
B → A (sync): t1 → proc_A->todo 或 thread_A1->todo
C → A (sync): t2 → proc_A->todo 或 thread_A2->todo
D → A (sync): t3 → proc_A->todo 或 thread_A3->todo

- 所有事务直接入队 proc_A->todo 或空闲线程的 todo
- A 进程的多个 binder 线程可以并发处理这些事务
- B、C、D 各自阻塞等待回复
- 不存在串行化约束
```

**异步 IPC 场景（都发给同一个 node_X）：**

```
B → A (async): t1 → node_X 没有异步事务 → 标记 has_async = true → proc_A->todo
C → A (async): t2 → node_X 已有异步事务 → node_X->async_todo（排队等待）
D → A (async): t3 → node_X 已有异步事务 → node_X->async_todo（排队等待）

- t1 被 A 的某个线程取走处理
- t2、t3 在 node_X->async_todo 中等待
- t1 处理完毕，A 释放缓冲区时：
  → 取出 t2 → proc_A->todo → 某线程处理
- t2 处理完毕：
  → 取出 t3 → proc_A->todo → 某线程处理
```

**异步 IPC 场景（发给不同的 node）：**

```
B → A 的 node_X (async): t1 → proc_A->todo
C → A 的 node_Y (async): t2 → proc_A->todo
D → A 的 node_Z (async): t3 → proc_A->todo

- 不同 node 的异步事务之间没有串行化约束
- 它们可以被 A 的不同线程并发处理
- 串行化是 per-node 的，不是 per-process 的
```

---

## 五、BINDER_TYPE_BINDER 消息与 binder_node 创建

### 5.1 Binder 对象类型

用户空间通过 `flat_binder_object` 在事务中传递 binder 对象（`include/uapi/linux/android/binder.h` 第 31 行）：

```c
enum {
    BINDER_TYPE_BINDER      = B_PACK_CHARS('s', 'b', '*', B_TYPE_LARGE),  // 本地强引用
    BINDER_TYPE_WEAK_BINDER = B_PACK_CHARS('w', 'b', '*', B_TYPE_LARGE),  // 本地弱引用
    BINDER_TYPE_HANDLE      = B_PACK_CHARS('s', 'h', '*', B_TYPE_LARGE),  // 远程强引用
    BINDER_TYPE_WEAK_HANDLE = B_PACK_CHARS('w', 'h', '*', B_TYPE_LARGE),  // 远程弱引用
    BINDER_TYPE_FD          = B_PACK_CHARS('f', 'd', '*', B_TYPE_LARGE),  // 文件描述符
};
```

`flat_binder_object` 结构（第 77 行）：

```c
struct flat_binder_object {
    struct binder_object_header hdr;    // type 字段
    __u32 flags;
    union {
        binder_uintptr_t binder;        // BINDER_TYPE_BINDER 时：本地对象地址
        __u32 handle;                   // BINDER_TYPE_HANDLE 时：远程句柄号
    };
    binder_uintptr_t cookie;
};
```

### 5.2 翻译过程：BINDER_TYPE_BINDER → BINDER_TYPE_HANDLE

当 `binder_transaction()` 处理事务数据时，遍历所有 `flat_binder_object` 并翻译（第 3131 行）：

```c
// drivers/android/binder.c:3131
switch (hdr->type) {
case BINDER_TYPE_BINDER:
case BINDER_TYPE_WEAK_BINDER: {
    struct flat_binder_object *fp;
    fp = to_flat_binder_object(hdr);
    ret = binder_translate_binder(fp, t, thread);
    // ...
} break;
```

`binder_translate_binder()` 是核心翻译函数（第 2232 行）：

```c
// drivers/android/binder.c:2232
static int binder_translate_binder(struct flat_binder_object *fp,
                   struct binder_transaction *t,
                   struct binder_thread *thread)
{
    struct binder_node *node;
    struct binder_proc *proc = thread->proc;         // 发送方进程
    struct binder_proc *target_proc = t->to_proc;    // 接收方进程

    // 步骤 1：在发送方进程的 nodes 红黑树中查找
    node = binder_get_node(proc, fp->binder);
    if (!node) {
        // 步骤 2：【首次传递】该 BBinder 没有对应的 node，创建一个
        node = binder_new_node(proc, fp);
    }

    // 步骤 3：在接收方进程中创建/查找对应的 binder_ref
    ret = binder_inc_ref_for_node(target_proc, node,
            fp->hdr.type == BINDER_TYPE_BINDER, &thread->todo, &rdata);

    // 步骤 4：【关键翻译】将类型从 BINDER 改为 HANDLE
    if (fp->hdr.type == BINDER_TYPE_BINDER)
        fp->hdr.type = BINDER_TYPE_HANDLE;
    else
        fp->hdr.type = BINDER_TYPE_WEAK_HANDLE;
    fp->binder = 0;          // 清除本地地址（接收方不应看到）
    fp->handle = rdata.desc;  // 设置为接收方的句柄号
    fp->cookie = 0;
}
```

### 5.3 binder_node 的创建过程

`binder_new_node()`（第 874 行）→ `binder_init_node_ilocked()`（第 782 行）：

```c
// drivers/android/binder.c:782
static struct binder_node *binder_init_node_ilocked(
                        struct binder_proc *proc,
                        struct binder_node *new_node,
                        struct flat_binder_object *fp)
{
    struct rb_node **p = &proc->nodes.rb_node;  // 红黑树根

    binder_uintptr_t ptr = fp ? fp->binder : 0;    // 用户空间 BBinder 地址
    binder_uintptr_t cookie = fp ? fp->cookie : 0;

    // 在红黑树中查找是否已存在
    while (*p) {
        parent = *p;
        node = rb_entry(parent, struct binder_node, rb_node);
        if (ptr < node->ptr)
            p = &(*p)->rb_left;
        else if (ptr > node->ptr)
            p = &(*p)->rb_right;
        else
            return node;  // 已存在，直接返回
    }

    // 不存在，初始化新 node
    node = new_node;
    rb_link_node(&node->rb_node, parent, p);
    rb_insert_color(&node->rb_node, &proc->nodes);

    node->proc = proc;
    node->ptr = ptr;                     // 用户空间 BBinder 地址
    node->cookie = cookie;
    node->work.type = BINDER_WORK_NODE;
    node->min_priority = flags & FLAT_BINDER_FLAG_PRIORITY_MASK;
    node->accept_fds = !!(flags & FLAT_BINDER_FLAG_ACCEPTS_FDS);
    INIT_LIST_HEAD(&node->async_todo);   // 初始化异步事务等待队列

    return node;
}
```

### 5.4 binder_ref 的创建过程

`binder_get_ref_for_node_olocked()`（第 1150 行）在接收方进程中为 node 创建引用：

```c
// drivers/android/binder.c:1150
static struct binder_ref *binder_get_ref_for_node_olocked(
                    struct binder_proc *proc,
                    struct binder_node *node,
                    struct binder_ref *new_ref)
{
    // 在 refs_by_node 红黑树中查找
    while (*p) {
        parent = *p;
        ref = rb_entry(parent, struct binder_ref, rb_node_node);
        if (node < ref->node)
            p = &(*p)->rb_left;
        else if (node > ref->node)
            p = &(*p)->rb_right;
        else
            return ref;  // 已存在引用，直接返回
    }

    // 不存在，创建新的 ref
    // 分配句柄号 desc（context manager 固定为 0，其他从 1 递增）
    new_ref->data.desc = (node == context->binder_context_mgr_node) ? 0 : 1;
    for (n = rb_first(&proc->refs_by_desc); n != NULL; n = rb_next(n)) {
        ref = rb_entry(n, struct binder_ref, rb_node_desc);
        if (ref->data.desc > new_ref->data.desc)
            break;
        new_ref->data.desc = ref->data.desc + 1;
    }

    // 插入两棵红黑树
    rb_insert_color(&new_ref->rb_node_node, &proc->refs_by_node);
    rb_insert_color(&new_ref->rb_node_desc, &proc->refs_by_desc);

    // 加入 node 的引用链表
    hlist_add_head(&new_ref->node_entry, &node->refs);
    return new_ref;
}
```

### 5.5 翻译流程图

```
  发送方进程 (Client)                       binder 驱动                        接收方进程 (Server)
 ┌──────────────────┐                ┌─────────────────────┐               ┌──────────────────┐
 │ flat_binder_object│                │                     │               │ flat_binder_object│
 │ type: BINDER      │ ──────────→   │ binder_translate_   │ ──────────→   │ type: HANDLE      │
 │ binder: 0x1234    │                │   binder()          │               │ handle: 1         │
 │ cookie: 0x1234    │                │                     │               │ cookie: 0         │
 └──────────────────┘                │ 1. 查找/创建 node   │               └──────────────────┘
                                     │    ptr=0x1234        │
                                     │    proc=发送方       │
                                     │                     │
                                     │ 2. 在接收方创建 ref  │
                                     │    desc=1           │
                                     │    node→上面的node  │
                                     │                     │
                                     │ 3. 翻译 type:       │
                                     │    BINDER → HANDLE  │
                                     │    binder → 0       │
                                     │    handle = desc     │
                                     └─────────────────────┘
```

---

## 六、Binder 内存申请与大小限制

### 6.1 mmap 初始化

进程打开 `/dev/binder` 后，通过 `mmap` 建立内核与用户空间的共享映射区。`binder_mmap()` 最终调用 `binder_alloc_mmap_handler()`（`drivers/android/binder_alloc.c` 第 749 行）：

```c
// drivers/android/binder_alloc.c:749
int binder_alloc_mmap_handler(struct binder_alloc *alloc, struct vm_area_struct *vma)
{
    struct binder_buffer *buffer;

    // 【关键限制】缓冲区大小不超过 4MB
    alloc->buffer_size = min_t(unsigned long, vma->vm_end - vma->vm_start, SZ_4M);

    alloc->buffer = (void __user *)vma->vm_start;

    // 分配页描述符数组（但不分配物理页）
    alloc->pages = kcalloc(alloc->buffer_size / PAGE_SIZE,
                           sizeof(alloc->pages[0]), GFP_KERNEL);

    // 创建初始空闲缓冲区（覆盖整个映射区）
    buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
    buffer->user_data = alloc->buffer;
    list_add(&buffer->entry, &alloc->buffers);
    buffer->free = 1;
    binder_insert_free_buffer(alloc, buffer);

    // 【关键】异步空间限制为总空间的一半
    alloc->free_async_space = alloc->buffer_size / 2;
}
```

### 6.2 内存大小限制

**注意区分两个层面**：内核驱动的硬上限 vs Android 用户空间的实际请求值。4MB 只是内核允许的**上限**，不是默认值；实际 buffer 大小由用户空间 `mmap()` 时传入的 size 决定。

| 限制项 | 内核硬上限 | Android 实际值 | 来源 |
|--------|-----------|---------------|------|
| 总 buffer 大小 | `min(用户请求, SZ_4M)` = **最大 4MB** | `(1*1024*1024) - 2*PAGE_SIZE` ≈ **~1MB** | 内核: `binder_alloc.c:915`；用户空间: `ProcessState.cpp` 的 `BINDER_VM_SIZE` |
| 异步空间配额 | `buffer_size / 2` = 最大 **2MB** | ≈ **~512KB** | `binder_alloc.c:941` |
| 异步空间低水位告警 | `buffer_size / 10` | ≈ **~104KB** | `binder_alloc.c:641` |

Android 用户空间 `ProcessState` 中的定义：

```cpp
// frameworks/native/libs/binder/ProcessState.cpp
#define BINDER_VM_SIZE ((1 * 1024 * 1024) - sysconf(_SC_PAGE_SIZE) * 2)
//                      = 1MB - 2*PAGE_SIZE ≈ 1,040,384 字节（32位）
```

绝大多数 Android 进程的 binder buffer 只有 ~1MB，只有当进程自行调用 `mmap(/dev/binder, 4MB)` 时才能拿到 4MB buffer（此时异步配额才是 2MB）。

### 6.3 缓冲区按需分配物理页

binder 不会在 mmap 时分配所有物理页，而是在 `binder_alloc_new_buf_locked()` 中**按需分配**（`binder_update_page_range()`，第 181 行）：

```c
// drivers/android/binder_alloc.c:181
static int binder_update_page_range(struct binder_alloc *alloc, int allocate,
                    void __user *start, void __user *end)
{
    for (page_addr = start; page_addr < end; page_addr += PAGE_SIZE) {
        index = (page_addr - alloc->buffer) / PAGE_SIZE;
        page = &alloc->pages[index];

        if (page->page_ptr) {
            // 页已存在（在 LRU 中），从 LRU 移出
            list_lru_del(&binder_alloc_lru, &page->lru);
            continue;
        }

        // 分配新的物理页
        page->page_ptr = alloc_page(GFP_KERNEL | __GFP_HIGHMEM | __GFP_ZERO);

        // 插入到用户空间 VMA
        ret = vm_insert_page(vma, user_page_addr, page[0].page_ptr);
    }
}
```

这意味着：
- mmap 时只分配页表结构，不分配物理页
- 第一次使用某个区域时才分配物理页并映射到用户空间
- 空闲页可以被放入 LRU，在内存压力下被回收

### 6.4 缓冲区分配过程

`binder_alloc_new_buf_locked()` 使用**最佳匹配**算法从空闲缓冲区红黑树中找到大小最合适的块（第 380 行）：

```c
// drivers/android/binder_alloc.c:380
static struct binder_buffer *binder_alloc_new_buf_locked(
                struct binder_alloc *alloc,
                size_t data_size, size_t offsets_size,
                size_t extra_buffers_size, int is_async, int pid)
{
    struct rb_node *n = alloc->free_buffers.rb_node;
    size_t size = ALIGN(data_size, sizeof(void *))
                + ALIGN(offsets_size, sizeof(void *))
                + ALIGN(extra_buffers_size, sizeof(void *));

    // 【异步空间检查】
    if (is_async && alloc->free_async_space < size + sizeof(struct binder_buffer)) {
        return ERR_PTR(-ENOSPC);  // 异步空间不足
    }

    // 最佳匹配查找
    while (n) {
        buffer = rb_entry(n, struct binder_buffer, rb_node);
        buffer_size = binder_alloc_buffer_size(alloc, buffer);
        if (size < buffer_size) {
            best_fit = n;
            n = n->rb_left;      // 还有更小的？
        } else if (size > buffer_size)
            n = n->rb_right;     // 太小了
        else {
            best_fit = n;         // 精确匹配
            break;
        }
    }
    // ... 分配物理页、分割缓冲区
}
```

### 6.5 异步空间计数

异步事务分配时扣减，释放时归还：

```c
// 分配时（binder_alloc.c:534-535）
if (is_async) {
    alloc->free_async_space -= size + sizeof(struct binder_buffer);
}

// 释放时（binder_alloc.c:672-673）
if (buffer->async_transaction) {
    alloc->free_async_space += buffer_size + sizeof(struct binder_buffer);
}
```

### 6.6 binder 缓冲区结构

```c
// drivers/android/binder_alloc.h:43
struct binder_buffer {
    struct list_head entry;          // 按地址排序的链表
    struct rb_node rb_node;          // 按大小排序的红黑树（空闲）/ 按地址（已分配）
    unsigned free:1;                 // 是否空闲
    unsigned async_transaction:1;    // 是否为异步事务的缓冲区
    struct binder_transaction *transaction;  // 关联的事务
    struct binder_node *target_node;        // 目标 node
    size_t data_size;                // 数据大小
    size_t offsets_size;             // 偏移数组大小
    void __user *user_data;          // 用户空间地址
};
```

### 6.7 内存管理图示

```
  binder_alloc 管理的缓冲区空间（最大 4MB）
 ┌────────────────────────────────────────────────┐
 │                                                │
 │  ┌────────┐ ┌──────┐ ┌─────────────┐ ┌──────┐ │
 │  │已分配  │ │ 空闲 │ │   已分配    │ │ 空闲 │ │
 │  │buffer  │ │      │ │   buffer    │ │      │ │
 │  │(sync)  │ │      │ │  (async)    │ │      │ │
 │  └────────┘ └──────┘ └─────────────┘ └──────┘ │
 │                                                │
 │  总空间 = buffer_size（≤ 4MB）                    │
 │  异步可用 = free_async_space（初始 = 总空间/2）    │
 │                                                │
 │  空闲缓冲区：按大小排序的红黑树 (free_buffers)     │
 │  已分配缓冲区：按地址排序的红黑树 (allocated_buffers)│
 │  所有缓冲区：按地址排序的链表 (buffers)            │
 └────────────────────────────────────────────────┘
```

---

## 七、Buffer 空间耗尽时的行为分析

当目标进程的 binder mmap 缓冲区空间耗尽时，同步 IPC 和异步 IPC 的失败路径不同。本节从源码角度分析两种情况。

### 7.1 Buffer 分配的入口

所有事务的 buffer 分配发生在 `binder_transaction()` 中，调用 `binder_alloc_new_buf()` 在**目标进程**的 mmap 空间中分配：

```c
// drivers/android/binder.c:3186
t->buffer = binder_alloc_new_buf(&target_proc->alloc, tr->data_size,
    tr->offsets_size, extra_buffers_size,
    !reply && (t->flags & TF_ONE_WAY), current->tgid);
if (IS_ERR(t->buffer)) {
    return_error_param = PTR_ERR(t->buffer);
    return_error = return_error_param == -ESRCH ?
        BR_DEAD_REPLY : BR_FAILED_REPLY;
    t->buffer = NULL;
    goto err_binder_alloc_buf_failed;
}
```

Buffer 分配在事务入队之前完成，因此无论同步还是异步，分配失败都不会产生残留的 todo 队列项。

### 7.2 两种空间不足的场景

`binder_alloc_new_buf_locked()` 中有两个失败点：

**场景 A：异步配额不足（仅影响异步 IPC）**

```c
// drivers/android/binder_alloc.c:525
if (is_async &&
    alloc->free_async_space < size + sizeof(struct binder_buffer)) {
    binder_alloc_debug(BINDER_DEBUG_BUFFER_ALLOC,
         "%d: binder_alloc_buf size %zd failed, no async space left\n",
          alloc->pid, size);
    return ERR_PTR(-ENOSPC);
}
```

异步事务有独立配额（`free_async_space`，初始为 `buffer_size / 2`）。即使总空间还有剩余，异步配额耗尽也会拒绝分配。同步 IPC 不受此检查约束，可以继续使用剩余的总空间。

**场景 B：总空间不足（同步/异步都受影响）**

```c
// drivers/android/binder_alloc.c:551
if (best_fit == NULL) {
    // ... 打印诊断日志 ...
    binder_alloc_debug(BINDER_DEBUG_USER_ERROR,
               "%d: binder_alloc_buf size %zd failed, no address space\n",
               alloc->pid, size);
    return ERR_PTR(-ENOSPC);
}
```

空闲红黑树中找不到容纳请求大小的空闲块。此时同步和异步 IPC 都会失败。

### 7.3 同步 IPC 空间不足的表现

Buffer 分配失败后，`binder_transaction()` 跳转到错误路径：

```c
// drivers/android/binder.c:3654
BUG_ON(thread->return_error.cmd != BR_OK);
if (in_reply_to) {
    // 回复场景：向发送方返回 COMPLETE，向原始调用方发送 failed_reply
    thread->return_error.cmd = BR_TRANSACTION_COMPLETE;
    binder_enqueue_thread_work(thread, &thread->return_error.work);
    binder_send_failed_reply(in_reply_to, return_error);
} else {
    // 发起新事务场景：直接返回错误给发送方
    thread->return_error.cmd = return_error;  // BR_FAILED_REPLY
    binder_enqueue_thread_work(thread, &thread->return_error.work);
}
```

| 步骤 | 行为 |
|------|------|
| 1 | `binder_alloc_new_buf()` 返回 `ERR_PTR(-ENOSPC)` |
| 2 | `return_error` 设为 `BR_FAILED_REPLY`（因为 `-ENOSPC != -ESRCH`） |
| 3 | 事务对象 `t` 和 `tcomplete` 被释放（`kfree`） |
| 4 | 发送方线程收到 `BR_FAILED_REPLY` |
| 5 | 用户空间 `IPCThreadState::waitForResponse()` 返回 `FAILED_TRANSACTION` |

**发送方不会阻塞**——因为事务根本没有成功入队，`transaction_stack` 未被压栈，直接返回错误。

### 7.4 异步 IPC 空间不足的表现

异步 IPC 失败路径与同步相同。关键区别在于：buffer 分配发生在 `TRANSACTION_COMPLETE` 入队**之前**：

```c
// drivers/android/binder.c 的执行顺序
// 第 3186 行: binder_alloc_new_buf()  ← 先分配
// 第 3565 行: binder_enqueue_thread_work(thread, tcomplete)  ← 后入队 COMPLETE
```

如果分配失败，`tcomplete` 尚未入队，走 `err_binder_alloc_buf_failed` 路径直接释放。发送方同样收到 `BR_FAILED_REPLY`，而非 `BR_TRANSACTION_COMPLETE`。

| 场景 | 异步配额不足 | 总空间不足 |
|------|-------------|-----------|
| 同步 IPC 影响 | **无影响**（不检查异步配额） | **失败**，返回 `BR_FAILED_REPLY` |
| 异步 IPC 影响 | **失败**，返回 `BR_FAILED_REPLY` | **失败**，返回 `BR_FAILED_REPLY` |

### 7.5 Oneway Spam 检测机制

当异步空间剩余不足 `buffer_size / 10`（即异步配额的 20%、总空间的 10%）时，驱动启动 spam 嫌疑检测：

```c
// drivers/android/binder_alloc.c:641
if (alloc->free_async_space < alloc->buffer_size / 10) {
    buffer->oneway_spam_suspect = debug_low_async_space_locked(alloc, pid);
}
```

`debug_low_async_space_locked()` 遍历所有已分配的异步 buffer，统计当前发送方 pid 的使用情况：

```c
// drivers/android/binder_alloc.c:421
// 阈值：某 pid 占用 >50 个异步 buffer，或 >25% 的总 buffer 空间
if (num_buffers > 50 || total_alloc_size > alloc->buffer_size / 4) {
    binder_alloc_debug(BINDER_DEBUG_USER_ERROR,
         "%d: pid %d spamming oneway? %zd buffers allocated for ...\n",
          alloc->pid, pid, num_buffers, total_alloc_size);
    if (!alloc->oneway_spam_detected) {
        alloc->oneway_spam_detected = true;
        return true;
    }
}
```

标记为 spam 嫌疑后，`binder_thread_read()` 会将 `BINDER_WORK_TRANSACTION_ONEWAY_SPAM_SUSPECT` 转换为 `BR_ONEWAY_SPAM_SUSPECT` 返回给发送方：

```c
// drivers/android/binder.c:4572
case BINDER_WORK_TRANSACTION_ONEWAY_SPAM_SUSPECT: {
    if (proc->oneway_spam_detection_enabled &&
           w->type == BINDER_WORK_TRANSACTION_ONEWAY_SPAM_SUSPECT)
        cmd = BR_ONEWAY_SPAM_SUSPECT;
    else
        cmd = BR_TRANSACTION_COMPLETE;
```

注意：`BR_ONEWAY_SPAM_SUSPECT` 只是**警告**，事务仍然会成功发送。只有当异步配额**完全耗尽**时才会返回 `BR_FAILED_REPLY` 拒绝事务。

### 7.6 冻结进程对 IPC 的影响

当目标进程被冻结（`proc->is_frozen = true`）时，`binder_proc_transaction()` 的处理策略不同于 buffer 耗尽：

```c
// drivers/android/binder.c:2694
if (proc->is_frozen) {
    proc->sync_recv |= !oneway;   // 记录冻结期间收到的同步事务
    proc->async_recv |= oneway;   // 记录冻结期间收到的异步事务
}

if ((proc->is_frozen && !oneway) || proc->is_dead ||
        (thread && thread->is_dead)) {
    binder_inner_proc_unlock(proc);
    binder_node_unlock(node);
    return proc->is_frozen ? BR_FROZEN_REPLY : BR_DEAD_REPLY;
}
```

| 事务类型 | 冻结进程行为 |
|----------|-------------|
| 同步 IPC | **拒绝**，返回 `BR_FROZEN_REPLY` |
| 异步 IPC | **接受入队**，在进程解冻后处理 |

### 7.7 完整失败路径流程图

```
binder_transaction()
    │
    ├─ binder_alloc_new_buf() 在目标进程 mmap 空间分配 buffer
    │     │
    │     ├─ [异步?] 检查 free_async_space
    │     │     └─ 不足 → ERR_PTR(-ENOSPC)
    │     │
    │     ├─ 空闲红黑树查找 best_fit
    │     │     └─ 找不到 → ERR_PTR(-ENOSPC)
    │     │
    │     └─ VMA 已销毁 → ERR_PTR(-ESRCH)
    │                          │
    │     ┌─────────────────────┘
    │     ▼
    │  IS_ERR(t->buffer) == true
    │     │
    │     ├─ -ESRCH → return_error = BR_DEAD_REPLY   （目标进程正在退出）
    │     └─ 其他   → return_error = BR_FAILED_REPLY  （空间不足等）
    │                          │
    │     goto err_binder_alloc_buf_failed
    │                          │
    │     ┌─────────────────────┘
    │     ▼
    │  错误清理: kfree(tcomplete), kfree(t)
    │     │
    │     ├─ [回复场景] 向原始调用方发送 binder_send_failed_reply()
    │     └─ [发起事务] thread->return_error.cmd = BR_FAILED_REPLY
    │                   发送方用户空间收到 FAILED_TRANSACTION
```

---

## 八、Todo 列表容量与请求数量限制

### 8.1 Todo 列表没有显式数量上限

从源码看，`proc->todo`、`thread->todo`、`node->async_todo` 都是 `struct list_head` 链表，**没有任何计数器或长度限制**：

```c
// drivers/android/binder_internal.h
struct binder_proc {
    // ...
    struct list_head todo;              // 进程级待处理工作队列
};

struct binder_thread {
    // ...
    struct list_head todo;              // 线程私有工作队列
};

struct binder_node {
    // ...
    struct list_head async_todo;        // 异步事务等待队列
};
```

`binder_proc_transaction()` 在入队时不检查队列长度，`outstanding_txns` 只做统计计数，不做限流。

### 8.2 实际限制来自 Buffer 空间

每个待处理的事务都需要在目标进程的 mmap 空间中持有一块 `binder_buffer`。Buffer 空间是 todo 列表深度的**硬约束**。

**空间布局与配额**（以 Android 典型的 ~1MB mmap 为例）：

| 参数 | 值 | 说明 |
|------|-----|------|
| `buffer_size` | ~1,040,384 字节（1MB - 8KB） | `ProcessState` 请求的 mmap 大小 |
| `free_async_space` | ~520,192 字节 | `buffer_size / 2`，异步专用配额 |
| 最小事务开销 | `sizeof(void *) + sizeof(struct binder_buffer)` | 数据最小 8 字节 + 元数据 ~104 字节 |

**每个事务在 mmap 空间中的占用**：

```c
// binder_alloc.c:502
size = ALIGN(data_size, sizeof(void *))
     + ALIGN(offsets_size, sizeof(void *))
     + ALIGN(extra_buffers_size, sizeof(void *));
size = max(size, sizeof(void *));  // 最小 8 字节

// 异步配额扣减量（binder_alloc.c:637）
alloc->free_async_space -= size + sizeof(struct binder_buffer);
```

### 8.3 同步 IPC 的理论最大并发数

同步 IPC 可使用整个 `buffer_size`，理论上限：

```
max_sync ≈ buffer_size / (sizeof(void *) + sizeof(struct binder_buffer))
         ≈ 1,040,384 / (8 + ~104)
         ≈ ~9,289 个
```

但同步 IPC 有天然的**自限流**机制：

1. **发送方阻塞**：每个同步调用的发送方线程在等到 `BR_REPLY` 之前无法发起新事务
2. **线程池限制**：目标进程的 `max_threads` 限制了并发处理能力（Android 应用通常为 15）
3. **栈深限制**：同步嵌套调用通过 `transaction_stack` 链式压栈，不会无限增长

实际中同时 pending 的同步事务数 ≈ 当前调用该进程的外部线程总数，远达不到 buffer 上限。

### 8.4 异步 IPC 的理论最大并发数

异步 IPC 使用独立配额 `free_async_space`（`buffer_size / 2`），理论上限：

```
max_async ≈ free_async_space / (sizeof(void *) + sizeof(struct binder_buffer))
          ≈ 520,192 / (8 + ~104)
          ≈ ~4,644 个
```

异步 IPC 缺少自限流（发送方不阻塞），因此需要额外的保护机制：

1. **per-node 串行化**：同一 `binder_node` 同一时刻只有 1 个异步事务在 `proc->todo` 中，其余排队在 `node->async_todo`。但排队的事务**已经分配了 buffer**
2. **Spam 检测**：异步配额低于 20% 时触发，超阈值的 pid 会收到 `BR_ONEWAY_SPAM_SUSPECT`
3. **配额硬限制**：异步配额耗尽后返回 `BR_FAILED_REPLY`

### 8.5 同步与异步共存时的空间隔离

```
 ┌──────────────────── buffer_size (~1MB) ────────────────────┐
 │                                                            │
 │   同步 IPC 使用整个空间（包括异步配额未使用的部分）            │
 │   ┌──────────────────────────────────────────────────┐     │
 │   │                                                  │     │
 │   └──────────────────────────────────────────────────┘     │
 │                                                            │
 │   异步 IPC 只能使用前半部分（free_async_space 配额）         │
 │   ┌────────────────────────┐                               │
 │   │  free_async_space      │                               │
 │   │  = buffer_size / 2     │                               │
 │   └────────────────────────┘                               │
 └────────────────────────────────────────────────────────────┘

 注意：异步配额是"逻辑配额"，不是物理分区。
 同步 IPC 可以分配到任何空闲块（不管它"属于"哪个半区）。
 异步 IPC 则受 free_async_space 计数器限制。
```

这个设计确保了：**异步 IPC 无论多拥挤，都不能占用超过总空间一半的 buffer，同步 IPC 始终有至少一半的空间可用**。

### 8.6 总结对照表

| 维度 | 同步 IPC | 异步 IPC (TF_ONE_WAY) |
|------|---------|----------------------|
| Buffer 配额 | 整个 `buffer_size`（~1MB） | `buffer_size / 2`（~512KB） |
| 理论最大事务数 | ~9,289（最小载荷） | ~4,644（最小载荷） |
| 实际限制因素 | 发送方阻塞 + 线程池 `max_threads` | Buffer 配额 + per-node 串行化 |
| 空间不足返回 | `BR_FAILED_REPLY` | `BR_FAILED_REPLY` |
| 配额不足返回 | 无独立配额 | `BR_FAILED_REPLY`（异步配额耗尽） |
| Spam 警告 | 不适用 | `BR_ONEWAY_SPAM_SUSPECT`（异步空间 < 20%） |
| 冻结进程 | `BR_FROZEN_REPLY`（拒绝） | 正常入队（解冻后处理） |
| 自限流 | 有（发送方阻塞等待回复） | 无（发送方立即返回） |

---

## 关键源码文件索引

| 文件 | 内容 |
|------|------|
| `drivers/android/binder.c` | binder 驱动核心：ioctl、transaction、translate、read/write |
| `drivers/android/binder_internal.h` | 内核内部数据结构：binder_proc、binder_thread、binder_node、binder_transaction |
| `drivers/android/binder_alloc.c` | 缓冲区管理：mmap、alloc_new_buf、free_buf、spam 检测 |
| `drivers/android/binder_alloc.h` | 内存管理数据结构：binder_alloc、binder_buffer |
| `drivers/android/binderfs.c` | binderfs 文件系统（含 oneway_spam_detection 特性开关） |
| `include/uapi/linux/android/binder.h` | 用户空间 API：命令协议（BC_/BR_）、数据结构、binder 类型 |

| 核心函数 | 文件:行号 | 作用 |
|---------|----------|------|
| `binder_open()` | `binder.c:5460` | 打开 /dev/binder，创建 binder_proc |
| `binder_mmap()` | `binder.c:5433` | 建立共享内存映射 |
| `binder_ioctl()` | `binder.c:5175` | ioctl 入口 |
| `binder_ioctl_write_read()` | `binder.c:4871` | 处理 BINDER_WRITE_READ |
| `binder_thread_write()` | `binder.c:3545` | 处理写命令（BC_TRANSACTION 等） |
| `binder_transaction()` | `binder.c:2883` | 核心事务处理（含 buffer 分配失败路径） |
| `binder_translate_binder()` | `binder.c:2232` | BINDER_TYPE_BINDER → HANDLE 翻译 |
| `binder_init_node_ilocked()` | `binder.c:782` | 创建 binder_node |
| `binder_get_ref_for_node_olocked()` | `binder.c:1150` | 创建 binder_ref |
| `binder_proc_transaction()` | `binder.c:2665` | 入队目标 todo / 唤醒线程 / 冻结检查 |
| `binder_thread_read()` | `binder.c:4181` | 读取并生成 BR 命令 |
| `binder_wait_for_work()` | `binder.c:4054` | 线程阻塞等待 |
| `binder_free_buf()` | `binder.c:3683` | 释放 buffer / 触发异步串行化 |
| `binder_alloc_mmap_handler()` | `binder_alloc.c:896` | mmap 初始化缓冲区 / 设置异步配额 |
| `binder_alloc_new_buf_locked()` | `binder_alloc.c:473` | 分配事务缓冲区（含异步配额检查） |
| `binder_alloc_free_buf_locked()` | `binder_alloc.c:761` | 释放缓冲区（归还异步配额） |
| `debug_low_async_space_locked()` | `binder_alloc.c:395` | Oneway spam 检测（>50 buffer 或 >25% 空间） |
| `binder_update_page_range()` | `binder_alloc.c:181` | 按需分配/释放物理页 |
