+++
date = '2026-04-27'
title = 'RCU 与锁机制源码深度分析'
weight = 6
tags = [
    "RCU",
    "spinlock",
    "mutex",
    "rwlock",
    "rw_semaphore",
    "qspinlock",
    "内存屏障",
    "原子操作",
    "并发同步",
]
categories = [
    "其他",
]
+++
# RCU 与锁机制源码深度分析

> 基于 Linux 5.15.78 内核源码分析

---

## 目录

- [一、锁的本质](#一锁的本质)
- [二、原子操作与内存屏障——锁的基石](#二原子操作与内存屏障锁的基石)
- [三、Spinlock（自旋锁）](#三spinlock自旋锁)
- [四、Mutex（互斥锁）](#四mutex互斥锁)
- [五、读写锁与读写信号量](#五读写锁与读写信号量)
- [六、RCU——Read-Copy-Update](#六rcuread-copy-update)
- [七、RCU 核心数据结构](#七rcu-核心数据结构)
- [八、RCU 读侧 API](#八rcu-读侧-api)
- [九、RCU 写侧 API](#九rcu-写侧-api)
- [十、宽限期（Grace Period）机制](#十宽限期grace-period机制)
- [十一、RCU 回调处理](#十一rcu-回调处理)
- [十二、锁与 RCU 全景对比](#十二锁与-rcu-全景对比)
- [十三、总结](#十三总结)

---

## 一、锁的本质

### 1.1 一句话定义

**锁 = 原子操作（建立互斥） + 等待策略（处理争用） + 内存屏障（保证可见性）**

所有锁的核心都是在一个共享变量上执行**原子的"读-修改-写"（RMW）操作**，保证同一时刻只有一个执行单元能成功获取锁。不同锁的区别仅在于获取失败后**如何等待**。

### 1.2 分类总览

```
                    ┌─────────────────────────────────────┐
                    │          Linux 内核同步原语           │
                    └──────────────┬──────────────────────┘
           ┌───────────────────────┼───────────────────────┐
           │                      │                       │
    ┌──────┴──────┐      ┌───────┴───────┐      ┌───────┴────────┐
    │   忙等待类   │      │   睡眠等待类   │      │   无锁类        │
    ├─────────────┤      ├───────────────┤      ├────────────────┤
    │ spinlock    │      │ mutex         │      │ RCU            │
    │ rwlock      │      │ rw_semaphore  │      │ per-CPU 变量   │
    │ raw_spinlock│      │ semaphore     │      │ 原子变量       │
    └─────────────┘      └───────────────┘      └────────────────┘
```

---

## 二、原子操作与内存屏障——锁的基石

### 2.1 `atomic_t` 定义

```c
// include/linux/types.h:166
typedef struct {
    int counter;
} atomic_t;
```

### 2.2 x86 原子操作：`LOCK` 前缀

x86 上所有原子 RMW 操作依赖 `LOCK` 前缀指令，锁定缓存行保证原子性：

```c
// arch/x86/include/asm/atomic.h:51
static __always_inline void arch_atomic_add(int i, atomic_t *v)
{
    asm volatile(LOCK_PREFIX "addl %1,%0"
             : "+m" (v->counter)
             : "ir" (i) : "memory");
}
```

### 2.3 `cmpxchg`——锁的核心指令

`cmpxchg`（Compare-and-Exchange）是几乎所有锁快路径的底层指令。x86 的 `xchg` 即使无 `lock` 前缀也隐含串行化语义：

```c
// arch/x86/include/asm/cmpxchg.h:72
/*
 * Note: no "lock" prefix even on SMP: xchg always implies lock anyway.
 */
#define arch_xchg(ptr, v)    __xchg_op((ptr), (v), xchg, "")
```

### 2.4 内存屏障

```c
// arch/x86/include/asm/barrier.h:57
#define __smp_mb() asm volatile("lock; addl $0,-4(%%" _ASM_SP ")" \
                                ::: "memory", "cc")

// x86 原子操作已自带串行化，前后屏障可为空
#define __smp_mb__before_atomic()    do { } while (0)
#define __smp_mb__after_atomic()     do { } while (0)
```

UP 下退化为编译屏障：

```c
// include/asm-generic/barrier.h:62
#ifdef CONFIG_SMP
#define smp_mb()    __smp_mb()
#else
#define smp_mb()    barrier()
#endif
```

**要点**：锁的实现依赖**原子 RMW** 建立互斥，再依赖 **acquire/release** 保证临界区与外部数据访问的可见性顺序。

---

## 三、Spinlock（自旋锁）

### 3.1 数据结构

```c
// include/linux/spinlock_types_raw.h:14
typedef struct raw_spinlock {
    arch_spinlock_t raw_lock;
#ifdef CONFIG_DEBUG_SPINLOCK
    unsigned int magic, owner_cpu;
    void *owner;
#endif
} raw_spinlock_t;

// include/linux/spinlock_types.h:16
typedef struct spinlock {
    union {
        struct raw_spinlock rlock;
    };
} spinlock_t;
```

### 3.2 调用链

```
spin_lock(lock)
  → raw_spin_lock(&lock->rlock)          // include/linux/spinlock.h:361
    → _raw_spin_lock()                   // SMP 路径
      → __raw_spin_lock()               // include/linux/spinlock_api_smp.h:139
        → preempt_disable()             // 关抢占
        → do_raw_spin_lock()
          → arch_spin_lock(&lock->raw_lock)
            → queued_spin_lock()         // qspinlock 快路径
```

### 3.3 qspinlock 快路径——一次 CAS 搞定

```c
// include/asm-generic/qspinlock.h:75
static __always_inline void queued_spin_lock(struct qspinlock *lock)
{
    int val = 0;
    if (likely(atomic_try_cmpxchg_acquire(&lock->val, &val, _Q_LOCKED_VAL)))
        return;
    queued_spin_lock_slowpath(lock, val);
}
```

### 3.4 qspinlock 数据编码

```c
// include/asm-generic/qspinlock_types.h:14
typedef struct qspinlock {
    union {
        atomic_t val;
        struct {
            u8  locked;     // 锁状态位
            u8  pending;    // 等待位
        };
        struct {
            u16 locked_pending;
            u16 tail;       // MCS 队列尾指针
        };
    };
} arch_spinlock_t;
```

4 字节内编码三段信息：**locked**（是否被持有）、**pending**（第一个等待者标记）、**tail**（MCS 队列尾部 CPU+idx 编码）。争用时走 `queued_spin_lock_slowpath`（`kernel/locking/qspinlock.c`），用 MCS 风格的 per-CPU 节点排队，避免经典 TAS 的缓存行风暴。

### 3.5 释放——单字节 store-release

```c
// include/asm-generic/qspinlock.h:93
static __always_inline void queued_spin_unlock(struct qspinlock *lock)
{
    smp_store_release(&lock->locked, 0);
}
```

### 3.6 UP 下的退化

UP 且无 DEBUG 时，`arch_spinlock_t` 是**空结构体**，`spin_lock` 仅为编译屏障：

```c
// include/linux/spinlock_up.h:62
#define arch_spin_lock(lock)    do { barrier(); (void)(lock); } while (0)
#define arch_spin_unlock(lock)  do { barrier(); (void)(lock); } while (0)
```

**本质**：锁是为解决**多处理器并发**而存在的。UP 上真正需要的只是关抢占/关中断。

---

## 四、Mutex（互斥锁）

### 4.1 数据结构

```c
// include/linux/mutex.h:63
struct mutex {
    atomic_long_t       owner;      // 持有者 task 指针 + 低位标志
    raw_spinlock_t      wait_lock;  // 保护 wait_list
#ifdef CONFIG_MUTEX_SPIN_ON_OWNER
    struct optimistic_spin_queue osq; // 乐观自旋 MCS 队列
#endif
    struct list_head    wait_list;  // 等待者链表
};
```

`owner` 低位复用为标志位：`MUTEX_FLAG_WAITERS`（有等待者）、`MUTEX_FLAG_HANDOFF`（锁传递）、`MUTEX_FLAG_PICKUP`（已指定下一持有者）。

### 4.2 三级递进获取策略

```
mutex_lock(lock)                           // kernel/locking/mutex.c:275
  ├─ 快路径: __mutex_trylock_fast()        // cmpxchg owner 0→current
  │   → 成功: 零等待直接返回
  │   → 失败: ↓
  ├─ __mutex_lock_slowpath()
  │   ├─ 乐观自旋: mutex_optimistic_spin() // owner 在 CPU 上运行时忙等
  │   │   → OSQ lock 保证只有一个自旋者
  │   │   → 检查 owner->on_cpu / need_resched
  │   │   → 成功: 拿到锁返回
  │   │   → 失败: ↓
  │   └─ 睡眠等待:
  │       → raw_spin_lock(&wait_lock)
  │       → __mutex_add_waiter() 挂到 wait_list
  │       → set_current_state(TASK_UNINTERRUPTIBLE)
  │       → schedule_preempt_disabled()    // 让出 CPU
  │       → 被唤醒后再 trylock / handoff
  └─ 返回
```

### 4.3 快路径源码

```c
// kernel/locking/mutex.c:160
static __always_inline bool __mutex_trylock_fast(struct mutex *lock)
{
    unsigned long curr = (unsigned long)current;
    unsigned long zero = 0UL;

    if (atomic_long_try_cmpxchg_acquire(&lock->owner, &zero, curr))
        return true;
    return false;
}
```

### 4.4 乐观自旋

思想：若 owner 正在其它 CPU 上运行，短暂自旋可能比立刻睡眠更高效。用 **OSQ（MCS 队列）** 保证只有一个"头号自旋者"在转：

```c
// kernel/locking/mutex.c:412
static __always_inline bool
mutex_optimistic_spin(struct mutex *lock, ...)
{
    if (!waiter) {
        if (!mutex_can_spin_on_owner(lock))
            goto fail;
        if (!osq_lock(&lock->osq))
            goto fail;
    }
    for (;;) {
        struct task_struct *owner;
        owner = __mutex_trylock_or_owner(lock);
        if (!owner)
            break;  // 拿到锁
        if (!mutex_spin_on_owner(lock, owner, ...))
            goto fail_unlock;  // owner 被调度走，放弃自旋
        cpu_relax();
    }
    // ...
}
```

### 4.5 解锁与唤醒

快路径：`cmpxchg` 把 `owner` 从 `current` 清 0。慢路径：从 `wait_list` 取第一个 waiter，`wake_q_add` 唤醒：

```c
// kernel/locking/mutex.c:860
raw_spin_lock(&lock->wait_lock);
if (!list_empty(&lock->wait_list)) {
    struct mutex_waiter *waiter =
        list_first_entry(&lock->wait_list, struct mutex_waiter, list);
    next = waiter->task;
    wake_q_add(&wake_q, next);
}
if (owner & MUTEX_FLAG_HANDOFF)
    __mutex_handoff(lock, next);
raw_spin_unlock(&lock->wait_lock);
wake_up_q(&wake_q);
```

---

## 五、读写锁与读写信号量

### 5.1 读写自旋锁（rwlock）——qrwlock

**数据编码**：`cnts` 低 9 位表示 writer 状态，高位用 `_QR_BIAS` 累加读者计数：

```c
// include/asm-generic/qrwlock.h:23
#define _QW_WAITING  0x100       // 写者等待
#define _QW_LOCKED   0x0ff       // 写者持有
#define _QW_WMASK    0x1ff       // 写者掩码
#define _QR_SHIFT    9
#define _QR_BIAS     (1U << _QR_SHIFT)
```

**读锁**：原子加法叠加读者计数

```c
// include/asm-generic/qrwlock.h:74
static inline void queued_read_lock(struct qrwlock *lock)
{
    int cnts;
    cnts = atomic_add_return_acquire(_QR_BIAS, &lock->cnts);
    if (likely(!(cnts & _QW_WMASK)))
        return;  // 无写者，直接成功
    queued_read_lock_slowpath(lock);
}
```

**写锁**：cmpxchg 独占 writer 位

```c
// include/asm-generic/qrwlock.h:90
static inline void queued_write_lock(struct qrwlock *lock)
{
    int cnts = 0;
    if (likely(atomic_try_cmpxchg_acquire(&lock->cnts, &cnts, _QW_LOCKED)))
        return;
    queued_write_lock_slowpath(lock);
}
```

### 5.2 读写信号量（rw_semaphore）

```c
// include/linux/rwsem.h:48
struct rw_semaphore {
    atomic_long_t count;       // 读者计数 + writer/waiters/handoff 位
    atomic_long_t owner;
    struct optimistic_spin_queue osq;
    raw_spinlock_t wait_lock;
    struct list_head wait_list;
};
```

与 rwlock 的核心区别：**rwsem 等待时可睡眠**（`might_sleep`），适合长临界区。

```c
// kernel/locking/rwsem.c:236
static inline bool rwsem_read_trylock(struct rw_semaphore *sem, long *cntp)
{
    *cntp = atomic_long_add_return_acquire(RWSEM_READER_BIAS, &sem->count);
    if (!(*cntp & RWSEM_READ_FAILED_MASK)) {
        rwsem_set_reader_owned(sem);
        return true;
    }
    return false;
}
```

### 5.3 rwlock vs rw_semaphore

| 维度 | `rwlock_t` | `rw_semaphore` |
|------|------------|----------------|
| 等待方式 | 忙等（自旋） | 可睡眠 |
| 适用上下文 | 中断/进程均可 | 仅进程上下文 |
| 适用临界区 | 极短 | 可较长 |
| 乐观自旋 | 无 | 有（`CONFIG_RWSEM_SPIN_ON_OWNER`） |

---

## 六、RCU——Read-Copy-Update

### 6.1 核心思想

RCU 不是传统意义上的"锁"，它是一种**无锁并发机制**：

- **读者**：几乎零开销（无原子操作、无缓存行争用）
- **写者**：先 Copy 副本 → 修改副本 → 原子指针替换发布 → 等待所有旧读者结束（宽限期） → 释放旧数据

```
     Writer 视角                              Reader 视角
  ┌──────────────┐                        ┌──────────────┐
  │ 分配新节点    │                        │ rcu_read_lock │
  │ 拷贝旧数据    │                        │   p = rcu_dereference(gp)
  │ 修改新节点    │                        │   使用 p->field
  │ rcu_assign_pointer(gp, new) ──────→   │   ...
  │ synchronize_rcu() / call_rcu()        │ rcu_read_unlock
  │   等待所有旧读者退出...                │
  │ kfree(old)    │                        └──────────────┘
  └──────────────┘
```

### 6.2 与传统锁的根本区别

传统锁回答"如何让多个执行单元安全地访问同一份数据"的方式是：**排队轮流来**。

RCU 的回答是：**读的人随便读，改的人悄悄换一份新的上去，等老读者都走了再把旧的扔掉**。

---

## 七、RCU 核心数据结构

### 7.1 `rcu_head`——回调挂载点

```c
// include/linux/types.h:200
struct callback_head {
    struct callback_head *next;
    void (*func)(struct callback_head *head);
} __attribute__((aligned(sizeof(void *))));
#define rcu_head callback_head
```

`call_rcu()` 把回调函数和 `next` 指针存入此结构，挂到 per-CPU 链表上等待宽限期结束后执行。

### 7.2 `rcu_node`——宽限期在树上的聚合

```c
// kernel/rcu/tree.h:40
struct rcu_node {
    raw_spinlock_t __private lock;
    unsigned long gp_seq;          // 本节点看到的宽限期序号
    unsigned long gp_seq_needed;
    unsigned long qsmask;          // 位图：哪些 CPU 尚未通过 QS
    unsigned long qsmaskinit;      // 初始化时的 qsmask
    struct rcu_node *parent;
    struct list_head blkd_tasks;   // 抢占 RCU 时被阻塞的任务
    struct list_head *gp_tasks;
    // ...
};
```

### 7.3 `rcu_data`——per-CPU 数据

```c
// kernel/rcu/tree.h:152
struct rcu_data {
    unsigned long   gp_seq;
    unsigned long   gp_seq_needed;
    union rcu_noqs  cpu_no_qs;     // 本 CPU 是否还需报告 QS
    bool            core_needs_qs;
    struct rcu_node *mynode;       // 所属叶节点
    unsigned long   grpmask;       // 在 mynode->qsmask 中的位
    struct rcu_segcblist cblist;   // 分段回调链表
    atomic_t        dynticks;      // dyntick 计数器
    int             cpu;
    // ...
};
```

### 7.4 `rcu_state`——全局状态

```c
// kernel/rcu/tree.h:298
struct rcu_state {
    struct rcu_node node[NUM_RCU_NODES];  // rcu_node 树数组
    struct rcu_node *level[RCU_NUM_LVLS + 1];
    unsigned long gp_seq;                  // 全局宽限期序号
    struct task_struct *gp_kthread;        // 宽限期内核线程
    struct swait_queue_head gp_wq;         // 唤醒 gp_kthread 的等待队列
    short gp_flags;                        // RCU_GP_FLAG_INIT / _FQS
    short gp_state;                        // RCU_GP_IDLE / _INIT / ...
    // ...
};
```

### 7.5 层次化树结构

```
                    ┌──────────┐
                    │ rcu_state│  全局单例，包含 gp_seq / gp_kthread
                    │ .node[0] │  ← 根 rcu_node
                    └────┬─────┘
               ┌─────────┼─────────┐
          ┌────┴────┐         ┌────┴────┐
          │ node[1] │         │ node[2] │     中间层
          └────┬────┘         └────┬────┘
        ┌──────┼──────┐     ┌──────┼──────┐
      ┌─┴─┐ ┌─┴─┐ ┌──┴┐ ┌─┴─┐ ┌─┴─┐ ┌──┴┐
      │n3 │ │n4 │ │n5 │ │n6 │ │n7 │ │n8 │  叶节点
      └─┬─┘ └─┬─┘ └──┬┘ └─┬─┘ └─┬─┘ └──┬┘
        │      │      │    │      │      │
      CPU0  CPU1   CPU2  CPU3  CPU4   CPU5   per-CPU rcu_data
```

每个 CPU 一个 `rcu_data`，指向所属叶 `rcu_node`。QS 报告沿树**自底向上**传播，根节点 `qsmask` 全清时表示宽限期可结束。

---

## 八、RCU 读侧 API

### 8.1 `rcu_read_lock()` / `rcu_read_unlock()`

```c
// include/linux/rcupdate.h:683
static __always_inline void rcu_read_lock(void)
{
    __rcu_read_lock();
    __acquire(RCU);
    rcu_lock_acquire(&rcu_lock_map);
}

static inline void rcu_read_unlock(void)
{
    __release(RCU);
    __rcu_read_unlock();
    rcu_lock_release(&rcu_lock_map);
}
```

### 8.2 非抢占式 RCU——`preempt_disable`

```c
// include/linux/rcupdate.h:66
static inline void __rcu_read_lock(void)
{
    preempt_disable();
}
static inline void __rcu_read_unlock(void)
{
    preempt_enable();
}
```

**零原子操作**：仅修改 per-CPU 的 `preempt_count`，无缓存行争用。

### 8.3 抢占式 RCU——nesting 计数

```c
// kernel/rcu/tree_plugin.h:396
void __rcu_read_lock(void)
{
    rcu_preempt_read_enter();  // current->rcu_read_lock_nesting++
    barrier();
}

void __rcu_read_unlock(void)
{
    struct task_struct *t = current;
    barrier();
    if (rcu_preempt_read_exit() == 0) {
        barrier();
        if (unlikely(READ_ONCE(t->rcu_read_unlock_special.s)))
            rcu_read_unlock_special(t);
    }
}
```

抢占式 RCU **不关抢占**，允许在读侧临界区内被调度走。被抢占的任务会被挂到 `rcu_node->blkd_tasks` 链表，阻止宽限期结束。

### 8.4 抢占 vs 非抢占对比

| 维度 | 非 PREEMPT_RCU | PREEMPT_RCU |
|------|---------------|-------------|
| 核心机制 | `preempt_disable()` | nesting 计数 |
| 读侧可否被抢占 | 不可 | 可以 |
| 实时性 | 较差（关抢占） | 较好（不关抢占） |
| 复杂度 | 极简 | 需处理阻塞任务协作 |

---

## 九、RCU 写侧 API

### 9.1 发布——`rcu_assign_pointer()`

```c
// include/linux/rcupdate.h:444
#define rcu_assign_pointer(p, v)                          \
do {                                                      \
    uintptr_t _r_a_p__v = (uintptr_t)(v);                \
    if (__builtin_constant_p(v) && (_r_a_p__v) == (uintptr_t)NULL) \
        WRITE_ONCE((p), (typeof(p))(_r_a_p__v));          \
    else                                                  \
        smp_store_release(&p, RCU_INITIALIZER((typeof(p))_r_a_p__v)); \
} while (0)
```

使用 `smp_store_release` 保证：**读者看到新指针之前，新数据的初始化必须已经完成**。

### 9.2 订阅——`rcu_dereference()`

```c
// include/linux/rcupdate.h:527
#define rcu_dereference(p) rcu_dereference_check(p, 0)

#define rcu_dereference_check(p, c) \
    __rcu_dereference_check((p), (c) || rcu_read_lock_held(), __rcu)
```

使用 `READ_ONCE` + lockdep 检查，确保在读侧临界区内访问。

### 9.3 `call_rcu()`——异步延迟回调

```c
// kernel/rcu/tree.c:2967
static void __call_rcu(struct rcu_head *head, rcu_callback_t func)
{
    head->func = func;
    head->next = NULL;
    local_irq_save(flags);
    rdp = this_cpu_ptr(&rcu_data);
    // ...
    rcu_segcblist_enqueue(&rdp->cblist, head);  // 入队到 per-CPU 分段链表
    // ...
    __call_rcu_core(rdp, head, flags);          // 可能触发新 GP
    local_irq_restore(flags);
}

void call_rcu(struct rcu_head *head, rcu_callback_t func)
{
    __call_rcu(head, func);
}
```

### 9.4 `synchronize_rcu()`——同步等待宽限期

```c
// kernel/rcu/tree.c:3744
void synchronize_rcu(void)
{
    if (rcu_blocking_is_gp())
        return;                      // 单 CPU 在线，直接返回
    if (rcu_gp_is_expedited())
        synchronize_rcu_expedited(); // 加速模式
    else
        wait_rcu_gp(call_rcu);       // 常规路径
}
```

常规路径等价于：注册一个 `call_rcu` 回调 → 在 `completion` 上睡眠 → 宽限期结束后回调唤醒。

```c
// kernel/rcu/update.c:371
void __wait_rcu_gp(bool checktiny, int n, call_rcu_func_t *crcu_array,
                   struct rcu_synchronize *rs_array)
{
    for (i = 0; i < n; i++) {
        init_completion(&rs_array[i].completion);
        (crcu_array[i])(&rs_array[i].head, wakeme_after_rcu);
    }
    for (i = 0; i < n; i++) {
        wait_for_completion(&rs_array[i].completion);
    }
}
```

---

## 十、宽限期（Grace Period）机制

### 10.1 GP 内核线程主循环

```c
// kernel/rcu/tree.c:2105
static int __noreturn rcu_gp_kthread(void *unused)
{
    rcu_bind_gp_kthread();
    for (;;) {
        /* 等待 GP 启动信号 */
        swait_event_idle_exclusive(rcu_state.gp_wq,
                 READ_ONCE(rcu_state.gp_flags) & RCU_GP_FLAG_INIT);
        if (rcu_gp_init())           // ① 初始化宽限期
            break;

        rcu_gp_fqs_loop();           // ② 强制静止状态循环

        rcu_gp_cleanup();            // ③ 清理，结束宽限期
    }
}
```

创建时机：`early_initcall(rcu_spawn_gp_kthread)` — 用 `kthread_create` 创建。

### 10.2 GP 启动——`rcu_gp_init()`

```
rcu_gp_init()
  ├─ 检查 gp_flags，若无需求则返回 false
  ├─ WRITE_ONCE(rcu_state.gp_flags, 0)      // 清标志
  ├─ rcu_seq_start(&rcu_state.gp_seq)        // 推进全局序号
  └─ rcu_for_each_node_breadth_first(rnp):
       ├─ rnp->qsmask = rnp->qsmaskinit     // 设置位图：哪些 CPU 需要报告 QS
       └─ 离线 CPU → rcu_report_qs_rnp() 直接清位
```

### 10.3 静止状态（Quiescent State）检测

**QS 的含义**：当一个 CPU 发生了上下文切换、进入空闲、或从内核态返回用户态，就说明它不可能还持有旧的 RCU 读侧引用。

**显式 QS 上报**（正常路径）：

```c
// kernel/rcu/tree.c:2328
static void rcu_check_quiescent_state(struct rcu_data *rdp)
{
    note_gp_changes(rdp);       // 同步本 CPU 与当前 GP
    if (!rdp->core_needs_qs)
        return;
    if (rdp->cpu_no_qs.b.norm)
        return;                 // 尚未达到 QS
    rcu_report_qs_rdp(rdp);     // 上报
}
```

**QS 沿树向上传播**：

```c
// kernel/rcu/tree.c:2174
static void rcu_report_qs_rnp(unsigned long mask, struct rcu_node *rnp, ...)
{
    for (;;) {
        WRITE_ONCE(rnp->qsmask, rnp->qsmask & ~mask);  // 清除本 CPU 的位
        if (rnp->qsmask != 0 || rcu_preempt_blocked_readers_cgp(rnp))
            return;          // 本节点还有未完成的 CPU/任务
        mask = rnp->grpmask;
        if (rnp->parent == NULL)
            break;           // 到达根节点
        rnp = rnp->parent;   // 向上传播
    }
    rcu_report_qs_rsp(flags); // 根节点全清 → 唤醒 GP 线程
}
```

**强制 QS**（`rcu_gp_fqs_loop` 周期性执行）：

```c
// kernel/rcu/tree.c:1906
static void rcu_gp_fqs(bool first_time)
{
    if (first_time)
        force_qs_rnp(dyntick_save_progress_counter);  // 记录快照
    else
        force_qs_rnp(rcu_implicit_dynticks_qs);       // 检测隐式 QS
}
```

隐式 QS 检测利用 **dynticks 计数器**：CPU 进出空闲/用户态时递增 dynticks，若两次检查间计数器变化，说明该 CPU 已通过 QS。

### 10.4 GP 结束——`rcu_gp_cleanup()`

```
rcu_gp_cleanup()
  ├─ rcu_seq_end(&new_gp_seq)               // 推进序号到"已结束"
  ├─ rcu_for_each_node_breadth_first(rnp):
  │    ├─ WRITE_ONCE(rnp->gp_seq, new_gp_seq)
  │    └─ __note_gp_changes() → 推进回调到 DONE 段
  ├─ rcu_seq_end(&rcu_state.gp_seq)
  ├─ WRITE_ONCE(rcu_state.gp_state, RCU_GP_IDLE)
  └─ 若仍有未满足的 gp_seq_needed:
       → WRITE_ONCE(rcu_state.gp_flags, RCU_GP_FLAG_INIT)  // 请求下一轮
```

### 10.5 完整宽限期时序

```
时间轴 →

CPU 0:  [rcu_read_lock ... rcu_read_unlock]         ← 旧读者
CPU 1:        [ctx_switch]                           ← QS 点
CPU 2:              [idle]                           ← QS 点
Writer:   call_rcu(old, kfree)
                    |
          ┌────────┴─────────────────────────────┐
          │        Grace Period（宽限期）           │
          │  rcu_gp_init → fqs_loop → gp_cleanup  │
          └─────────────────────────────┬─────────┘
                                        |
                                   kfree(old)      ← 回调执行
```

---

## 十一、RCU 回调处理

### 11.1 分段回调链表——`rcu_segcblist`

```c
// include/linux/rcu_segcblist.h:183
struct rcu_segcblist {
    struct rcu_head *head;
    struct rcu_head **tails[RCU_CBLIST_NSEGS];  // 四段尾指针
    unsigned long gp_seq[RCU_CBLIST_NSEGS];     // 每段对应的 GP 序号
    long seglen[RCU_CBLIST_NSEGS];              // 每段长度
};
```

四段含义：

```
DONE          WAIT          NEXT_READY      NEXT
(已完成GP)    (等待当前GP)  (等待下一GP)    (新入队)
  ├─────────────┼──────────────┼───────────────┤
  head                                      tail
```

随着宽限期推进，`rcu_segcblist_advance()` 把已完成 GP 的回调向 DONE 段移动。

### 11.2 `rcu_do_batch()`——批量执行回调

```c
// kernel/rcu/tree.c:2445
static void rcu_do_batch(struct rcu_data *rdp)
{
    if (!rcu_segcblist_ready_cbs(&rdp->cblist))
        return;

    rcu_segcblist_extract_done_cbs(&rdp->cblist, &rcl);  // 摘下 DONE 段

    rhp = rcu_cblist_dequeue(&rcl);
    for (; rhp; rhp = rcu_cblist_dequeue(&rcl)) {
        f = rhp->func;
        f(rhp);                    // 执行回调（通常是 kfree）
    }

    if (!offloaded && rcu_segcblist_ready_cbs(&rdp->cblist))
        invoke_rcu_core();         // 仍有就绪回调，继续调度执行
}
```

在 softirq 路径有 `blimit` / 时间上限，防止单次执行过久。

---

## 十二、锁与 RCU 全景对比

### 12.1 特性对比

| 维度 | spinlock | mutex | rwlock | rw_semaphore | RCU |
|------|----------|-------|--------|-------------|-----|
| 互斥方式 | 原子 CAS | 原子 CAS | 原子 add/CAS | 原子 add/CAS | **不互斥** |
| 读侧开销 | 原子操作 | 原子操作 | 原子加法 | 原子加法 | **≈0** |
| 写侧开销 | 与读对称 | 与读对称 | 独占 CAS | 独占 CAS | **重**（拷贝+GP） |
| 等待策略 | 忙等 | 睡眠+乐观自旋 | 忙等 | 睡眠+乐观自旋 | 写者等 GP |
| 缓存行争用 | 有 | 有 | 有 | 有 | 读侧**无** |
| 中断上下文 | 可用 | **不可** | 可用 | **不可** | 读侧可用 |
| 适用场景 | 极短临界区 | 长临界区 | 多读少写短 | 多读少写长 | **读极多写极少** |

### 12.2 选择决策树

```
需要保护共享数据？
  │
  ├─ 写操作极少（路由表/模块列表/...） → RCU
  │
  ├─ 读多写少，临界区较长 → rw_semaphore
  │
  ├─ 读多写少，临界区极短 → rwlock
  │
  ├─ 临界区可能睡眠 → mutex
  │
  └─ 极短临界区，不可睡眠 → spinlock
       │
       ├─ 可能在中断中访问 → spin_lock_irqsave
       └─ 可能在软中断中访问 → spin_lock_bh
```

---

## 十三、总结

### 锁的本质

锁是用**硬件原子指令**（x86 `LOCK cmpxchg`）在共享变量上建立**互斥**，再配合**等待策略**（忙等或睡眠）处理争用，用**内存屏障**（acquire/release）保证临界区内外的可见性顺序。

从 UP 下 spinlock 退化为空操作可以看出：**锁是为多处理器并发而存在的**。

### RCU 的本质

RCU 是**用空间换时间**——放弃写侧效率，换取读侧零开销。它不依赖互斥，而是利用 **CPU 调度的天然同步点**（上下文切换、空闲态）作为"所有旧读者已退出"的证据，安全地延迟释放旧数据。

### 最终类比

- **传统锁**：排队轮流来，谁拿到锁谁进入
- **RCU**：读的人随便读，改的人悄悄换一份新的上去，等老读者都走了再把旧的扔掉
