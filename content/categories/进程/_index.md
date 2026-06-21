---
title: "进程"
description: "进程管理与调度"
slug: "process"
image: "category-icon.svg"
style:
    background: "#FBBC04"
    color: "#fff"
---

> Linux 进程管理子系统：进程/线程的创建、销毁、调度与上下文切换。
>
> 共 **6 篇**，建议按以下顺序阅读。

## 建议阅读顺序

1. **[进程与线程的本质区别]({{< relref "post/进程/进程与线程的本质区别.md" >}})** — clone flags 共享 vs 复制、PID/TGID、线程栈、max_threads 限制
2. **[进程内核结构关系]({{< relref "post/进程/进程内核结构关系.md" >}})** — task_struct 各子结构（mm/files/fs/cred/signal）与 copy_process 复制逻辑
3. **[系统调用机制]({{< relref "post/进程/系统调用机制.md" >}})** — syscall 指令 vs int 0x80、entry_SYSCALL_64、sys_call_table 分派
4. **[进程线程创建源码分析]({{< relref "post/进程/进程线程创建源码分析.md" >}})** — fork()/clone() → copy_process() 完整调用链
5. **[上下文切换实现原理]({{< relref "post/进程/上下文切换实现原理.md" >}})** — __schedule() → context_switch() → switch_to 汇编级分析
6. **[调度器执行时机深度分析]({{< relref "post/进程/调度器执行时机深度分析.md" >}})** — need_resched 双层标志、五大触发路径、内核抢占
