+++
date = '2026-06-11'
title = 'CPU 上下文切换与内核栈——进程是怎么被 CPU 执行的'
weight = 3
tags = [
    "上下文切换",
    "内核栈",
    "task_struct",
    "switch_to",
    "RIP",
    "RSP",
    "PC寄存器",
    "copy_thread",
]
categories = [
    "内存",
]
+++
# CPU 上下文切换与内核栈

> 基于 Linux 5.15.78 内核源码。
> 前置阅读：[内存管理完全指南](内存管理完全指南.md) — 了解 task_struct 和 VMA 的基本结构。
> 姊妹篇：[CPU 访问内存的硬件路径](CPU访问内存的硬件路径.md) — CPU 与内存的物理总线交互。

本文回答一个根本问题：**每个 CPU 核心有自己的 PC 寄存器吗？当一个 task_struct 被调度运行时，CPU 是怎么知道该干什么的？**

---

## 目录

- [1. 每个 CPU 核心是独立的执行单元](#1-每个-cpu-核心是独立的执行单元)
- [2. task_struct 和 thread_struct](#2-task_struct-和-thread_struct)
- [3. 内核栈：每个进程独立的 16KB](#3-内核栈每个进程独立的-16kb)
  - [3.1 内核栈的用途](#31-内核栈的用途)
  - [3.2 内核栈的分配](#32-内核栈的分配)
  - [3.3 内核栈的布局](#33-内核栈的布局)
- [4. 上下文切换的完整过程](#4-上下文切换的完整过程)
  - [4.1 调度器调用 context_switch()](#41-调度器调用-context_switch)
  - [4.2 switch_to 宏](#42-switch_to-宏)
  - [4.3 __switch_to_asm 汇编实现](#43-__switch_to_asm-汇编实现)
- [5. 新进程的第一次运行](#5-新进程的第一次运行)
  - [5.1 copy_thread() 设置初始状态](#51-copy_thread-设置初始状态)
  - [5.2 ret_from_fork 入口](#52-ret_from_fork-入口)
- [6. RSP 在各阶段的值](#6-rsp-在各阶段的值)
- [7. 总结：CPU 只认 RIP 和 RSP](#7-总结cpu-只认-rip-和-rsp)

---

## 1. 每个 CPU 核心是独立的执行单元

每个 CPU 核心本质上是一个**独立的处理器**，拥有自己完整的一套寄存器：

```
Core 0:  RIP(指令指针) + RSP(栈指针) + RAX/RBX/RCX/... + L1/L2 Cache(私有)
Core 1:  RIP(指令指针) + RSP(栈指针) + RAX/RBX/RCX/... + L1/L2 Cache(私有)
Core 2:  RIP(指令指针) + RSP(栈指针) + RAX/RBX/RCX/... + L1/L2 Cache(私有)
Core 3:  RIP(指令指针) + RSP(栈指针) + RAX/RBX/RCX/... + L1/L2 Cache(私有)
```

每个核心独立执行指令循环：**取 RIP 指向的指令 → 解码 → 执行 → RIP 前进**。

- **RIP**（Instruction Pointer）= 其他架构中的 **PC**（Program Counter），指向下一条要执行的指令
- **RSP**（Stack Pointer）= 当前栈顶地址
- 核心之间共享 L3 Cache 和物理内存，但 L1/L2 和所有寄存器都是私有的

所以，4 核 CPU 可以同时执行 4 条不同的指令流，各自维护自己的 RIP 和 RSP。

---

## 2. task_struct 和 thread_struct

每个进程/线程在内核中由一个 `task_struct` 描述。其中嵌入了一个 `thread_struct`，保存该进程被切换走时的 CPU 状态。

`arch/x86/include/asm/processor.h:469`：

```c
struct thread_struct {
    /* Cached TLS descriptors: */
    struct desc_struct    tls_array[GDT_ENTRY_TLS_ENTRIES];
    unsigned long         sp;          // 保存的内核栈指针 (RSP)
#ifdef CONFIG_X86_64
    unsigned short        es;
    unsigned short        ds;
    unsigned short        fsindex;
    unsigned short        gsindex;
    unsigned long         fsbase;
    unsigned long         gsbase;
#endif
    unsigned long         cr2;         // 缺页时的地址
    unsigned long         trap_nr;     // 异常号
    unsigned long         error_code;  // 异常错误码
    // ...
};
```

**关键字段是 `sp`**：保存该进程被切走时的内核栈指针（RSP）。RIP 不需要单独保存——它在栈上的返回地址里。

---

## 3. 内核栈：每个进程独立的 16KB

### 3.1 内核栈的用途

用户程序调用 `read()`、`write()`、`mmap()` 等系统调用时，CPU 进入内核态。内核代码需要栈来运行——**内核栈就是内核代码为这个进程工作时的运行栈**。

| | 用户栈 | 内核栈 |
|---|---|---|
| **用途** | 用户代码运行时用 | 内核代码为该进程工作时用 |
| **何时使用** | 正常执行用户程序 | 系统调用、中断、异常 |
| **地址范围** | 0x7fff_xxxx（用户空间） | 0xffff_c900_xxxx（内核空间） |
| **大小** | 通常 8MB（可增长） | 固定 16KB |
| **每个进程独立** | 是 | 是 |

内核栈上存的是：
- 内核函数的局部变量和调用链（`vfs_read() → ext4_read() → bio_submit()`）
- 系统调用入口保存的用户态寄存器（`pt_regs`）
- 上下文切换时保存的 callee-saved 寄存器

内核栈只有 16KB，所以内核代码**不能用大的局部变量**、不能有太深的递归。

### 3.2 内核栈的分配

每个进程的内核栈是**独立分配**的，不是共享的。

`kernel/fork.c:1067`（`dup_task_struct()` 中）：

```c
stack = alloc_thread_stack_node(tsk, node);   // 为新进程分配一块独立内存
if (!stack)
    goto free_tsk;

tsk->stack = stack;    // 挂到新进程的 task_struct 上
```

分配实现（`kernel/fork.c:433`）：

```c
static unsigned long *alloc_thread_stack_node(struct task_struct *tsk, int node)
{
    unsigned long *stack;
    stack = kmem_cache_alloc_node(thread_stack_cache, THREADINFO_GFP, node);
    stack = kasan_reset_tag(stack);
    tsk->stack = stack;
    return stack;
}
```

从 slab 缓存分配一块 `THREAD_SIZE` 大小的内存。

大小定义（`arch/x86/include/asm/page_64_types.h:15`）：

```c
#define THREAD_SIZE_ORDER    (2 + KASAN_STACK_ORDER)
#define THREAD_SIZE          (PAGE_SIZE << THREAD_SIZE_ORDER)
// THREAD_SIZE_ORDER = 2, 所以 THREAD_SIZE = 4096 × 4 = 16KB
```

### 3.3 内核栈的布局

```
高地址 (栈底)
┌─────────────────────────────────────────┐
│ pt_regs                                 │ ← 用户态寄存器快照
│   rip = 用户被中断处的指令                 │    (iret 返回用户态时用)
│   rsp = 用户的栈指针                      │
│   rax, rbx, rcx... 用户态的值             │
├─────────────────────────────────────────┤
│ entry_SYSCALL_64() 的栈帧               │ ← 系统调用入口
│   保存的寄存器, 局部变量                    │
├─────────────────────────────────────────┤
│ sys_read() / vfs_read() 的栈帧          │ ← VFS 层
├─────────────────────────────────────────┤
│ ext4_read() 的栈帧                      │ ← 文件系统层
├─────────────────────────────────────────┤
│ bio_submit() / 驱动函数的栈帧            │ ← 块设备层
├─────────────────────────────────────────┤
│ ... 更深的内核调用链 ...                   │
│                                         │
│ RSP → 当前栈顶                           │
低地址 (栈顶，向低地址增长)
└─────────────────────────────────────────┘
```

---

## 4. 上下文切换的完整过程

### 4.1 调度器调用 context_switch()

`kernel/sched/core.c:5015`：

```c
static __always_inline struct rq *
context_switch(struct rq *rq, struct task_struct *prev,
               struct task_struct *next, struct rq_flags *rf)
{
    prepare_task_switch(rq, prev, next);

    arch_start_context_switch(prev);

    // 1. 切换地址空间（页表/CR3）
    if (!next->mm) {                    // 切到内核线程
        enter_lazy_tlb(prev->active_mm, next);
        next->active_mm = prev->active_mm;
    } else {                            // 切到用户进程
        switch_mm_irqs_off(prev->active_mm, next->mm, next);
    }

    // 2. 切换寄存器（栈 + 指令指针）—— 这是核心！
    switch_to(prev, next, last);

    // 注意：switch_to 返回后，CPU 已经在 next 进程中了！
    // prev 变量实际上可能指向另一个进程（因为栈已经换了）

    return finish_task_switch(rq, prev);
}
```

### 4.2 switch_to 宏

`arch/x86/include/asm/switch_to.h:47`：

```c
#define switch_to(prev, next, last)                    \
do {                                                   \
    ((last) = __switch_to_asm((prev), (next)));        \
} while (0)
```

`switch_to` 展开后调用 `__switch_to_asm`，这是一个汇编函数。参数通过寄存器传递：
- `%rdi` = prev（当前进程的 task_struct 指针）
- `%rsi` = next（新进程的 task_struct 指针）

### 4.3 __switch_to_asm 汇编实现

`arch/x86/entry/entry_64.S:230`：

```asm
SYM_FUNC_START(__switch_to_asm)
    /*
     * 1. 保存当前进程（prev）的 callee-saved 寄存器到当前栈
     *    这些寄存器按 C 调用约定，被调用者必须保存
     */
    pushq    %rbp
    pushq    %rbx
    pushq    %r12
    pushq    %r13
    pushq    %r14
    pushq    %r15

    /*
     * 2. 切换栈 —— 这是最关键的两行！
     *    将当前 RSP 保存到 prev 的 thread.sp
     *    然后将 next 的 thread.sp 加载到 RSP
     */
    movq    %rsp, TASK_threadsp(%rdi)    /* prev->thread.sp = 当前 RSP */
    movq    TASK_threadsp(%rsi), %rsp    /* 当前 RSP = next->thread.sp */

    /*
     * 从此刻起，CPU 使用的是 next 进程的内核栈！
     * 下面的 pop 操作从 next 的栈上恢复寄存器
     * （这些值是 next 上次被切走时 push 进去的）
     */

    /* 3. 恢复 next 进程的 callee-saved 寄存器 */
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp

    /*
     * 4. 跳到 C 函数 __switch_to 做剩余工作
     *    (FPU 状态、FS/GS base 等)
     *    __switch_to 返回后，ret 指令从 next 的栈上取返回地址
     *    → 跳回 next 进程上次正在执行的代码位置
     */
    jmp    __switch_to
SYM_FUNC_END(__switch_to_asm)
```

**核心就两步**：`movq %rsp, prev->thread.sp` 保存旧栈，`movq next->thread.sp, %rsp` 切换栈。RSP 一换，后续所有 `pop` 和 `ret` 都从新进程的栈上取数据。

---

## 5. 新进程的第一次运行

### 5.1 copy_thread() 设置初始状态

当一个新进程通过 `fork()`/`clone()` 创建时，它从未运行过，没有"上次被切走"的状态。`copy_thread()` 负责为它构造一个**假的切换现场**：

`arch/x86/kernel/process.c:194`：

```c
int copy_thread(unsigned long clone_flags, unsigned long sp,
                unsigned long arg, struct task_struct *p, unsigned long tls)
{
    struct inactive_task_frame *frame;
    struct fork_frame *fork_frame;
    struct pt_regs *childregs;

    /* 获取子进程内核栈顶部的 pt_regs 区域 */
    childregs = task_pt_regs(p);
    fork_frame = container_of(childregs, struct fork_frame, regs);
    frame = &fork_frame->frame;

    /* 设置栈帧基址指针 */
    frame->bp = encode_frame_pointer(childregs);

    /*
     * 关键！设置返回地址为 ret_from_fork
     * 子进程首次被 context_switch() 切换到时，
     * __switch_to_asm 最后的 ret 指令会跳转到这里
     */
    frame->ret_addr = (unsigned long)ret_from_fork;

    /* 设置子进程的内核栈指针 */
    p->thread.sp = (unsigned long)fork_frame;

    /* 复制父进程的用户态寄存器到子进程的 pt_regs */
    *childregs = *current_pt_regs();

    /* 子进程 fork() 返回值为 0 */
    childregs->ax = 0;

    /* 如果 clone 指定了用户栈 */
    if (sp)
        childregs->sp = sp;

    // ... TLS 处理等
}
```

### 5.2 新进程内核栈的布局

```
子进程内核栈 (16KB)：

  高地址 (栈底)
  ┌─────────────────────────────────────┐
  │ pt_regs                             │ ← 从父进程复制的用户态寄存器
  │   rip = 父进程 fork() 的下一条指令    │    (iret 返回用户态时用)
  │   rsp = 用户栈指针（或 clone 指定的）  │
  │   ax  = 0 (子进程 fork 返回 0)       │
  ├─────────────────────────────────────┤
  │ fork_frame                          │
  │   frame.bp = 编码的帧指针            │
  │   frame.ret_addr = ret_from_fork    │ ← __switch_to_asm 的 ret 跳到这
  │   (callee-saved 寄存器位: 全0)       │
  └─────────────────────────────────────┘
  p->thread.sp → 指向 fork_frame

  低地址 (栈顶)
```

### 5.3 首次调度执行流程

```
调度器选中子进程
  └→ context_switch()
      └→ switch_to(prev, child, last)
          └→ __switch_to_asm()
              movq child->thread.sp → RSP    ← 栈切到子进程
              pop r15...rbp                   ← 从 fork_frame 恢复（全是0）
              jmp __switch_to                 ← C 代码做 FPU 等初始化
              ret                             ← 从 fork_frame.ret_addr 取地址
                                               → 跳到 ret_from_fork!

ret_from_fork:
  └→ 恢复 pt_regs 中的用户态寄存器
  └→ iret 返回用户态
  └→ CPU 执行 pt_regs.rip 指向的指令
  └→ 子进程在 fork() 的下一条指令处继续执行，返回值=0
```

---

## 6. RSP 在各阶段的值

RSP 保存的是**当前栈顶的地址**，在不同阶段指向不同的栈：

### 用户态

```
RSP = 0x7fff_aabb_ccd0    ← 指向用户栈顶
```

用户栈存放应用程序的局部变量、函数返回地址等。

### 系统调用陷入内核

```
用户态 RSP = 0x7fff_xxxx  (用户栈)
    ↓ syscall 指令
    ↓ CPU 从 TSS 加载内核栈指针
内核态 RSP = 0xffff_c900_0012_3ff0  (当前进程的内核栈顶)
```

CPU 硬件自动完成栈切换，通过 MSR/TSS 机制找到当前进程的内核栈。

### 在内核中执行

RSP 在当前进程的内核栈上移动，随着函数调用深入而减小（栈向低地址增长）。

### 上下文切换时

```
切换前 (进程 A):
  RSP = A 的内核栈某处 (0xffff_c900_0012_3f00)

__switch_to_asm:
  push r15...rbp                  ← 压入 A 的内核栈
  movq %rsp, A->thread.sp        ← A.thread.sp = 0xffff_c900_0012_3ec8
  movq B->thread.sp, %rsp        ← RSP = B 的内核栈 (0xffff_c900_0014_5ec8)
  pop rbp...r15                   ← 从 B 的内核栈恢复
  ret                             ← 从 B 的栈上取返回地址

切换后 (进程 B):
  RSP = B 的内核栈某处 (0xffff_c900_0014_5ef8)
```

### 返回用户态

```
内核态 RSP = 当前进程的内核栈
    ↓ iret 指令
    ↓ 从 pt_regs 恢复用户态 RSP
用户态 RSP = pt_regs.sp (0x7fff_xxxx)
```

---

## 7. 总结：CPU 只认 RIP 和 RSP

CPU 硬件不知道什么是 `task_struct`、什么是进程、什么是操作系统。它只做一件事：

> **从 RIP 指向的地址取指令，用 RSP 指向的栈来运行。**

上下文切换的本质：

1. **换栈**（RSP）→ CPU 开始用新进程的栈
2. **换 RIP**（通过栈上的返回地址）→ CPU 开始执行新进程的代码
3. **换页表**（CR3）→ CPU 看到新进程的地址空间

这三步完成后，CPU 就完全"变成"了新进程——它执行的每一条指令、访问的每一个地址，都是新进程的。

```
                    ┌──────────────────┐
    context_switch  │  保存 prev 状态   │  push 寄存器, 保存 RSP
                    │  切换地址空间     │  写 CR3 (新页表)
                    │  切换栈          │  RSP = next->thread.sp
                    │  恢复 next 状态  │  pop 寄存器, ret 恢复 RIP
                    └──────────────────┘
                              ↓
                    CPU 继续执行 next 进程的代码
                    对 CPU 来说，什么都没变
                    只是 RIP/RSP/CR3 指向了不同的地方
```

---

> **相关文档**：
> - [内存管理完全指南](内存管理完全指南.md) — task_struct、VMA、页表管理
> - [CPU 页表与缓存机制](CPU页表与缓存机制.md) — CR3 切换后的 TLB 处理、PCID 优化
> - [CPU 访问内存的硬件路径](CPU访问内存的硬件路径.md) — CPU 与内存的物理总线交互
> - [x86_64 虚拟内存布局](x86_64虚拟内存布局.md) — 用户/内核地址空间划分
