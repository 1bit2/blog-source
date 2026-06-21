+++
date = '2026-04-16'
title = 'eBPF 实现原理分析（基于 Linux 5.15.78 内核源码）'
weight = 4
tags = [
    "eBPF",
    "BPF",
    "verifier",
    "JIT",
    "BTF",
]
categories = [
    "其他",
]
+++
# eBPF 实现原理分析（基于 Linux 5.15.78 内核源码）

## 目录

- [一、eBPF 程序的加载流程](#一ebpf-程序的加载流程)
  - [1.1 BPF 系统调用入口](#11-bpf-系统调用入口)
  - [1.2 程序类型与 attach 类型](#12-程序类型与-attach-类型)
  - [1.3 bpf_prog_load 加载流程](#13-bpf_prog_load-加载流程)
  - [1.4 验证器 bpf_check](#14-验证器-bpf_check)
  - [1.5 JIT 编译](#15-jit-编译)
- [二、eBPF 程序的挂载与执行机制](#二ebpf-程序的挂载与执行机制)
  - [2.1 Kprobe 挂载机制](#21-kprobe-挂载机制)
  - [2.2 Tracepoint 挂载机制](#22-tracepoint-挂载机制)
  - [2.3 fentry/fexit：BPF Trampoline 机制](#23-fentryfexit-bpf-trampoline-机制)
  - [2.4 XDP 挂载机制](#24-xdp-挂载机制)
  - [2.5 BPF 程序的执行路径](#25-bpf-程序的执行路径)
- [三、BTF 与 CO-RE：从问题到实现的完整追踪](#三btf-与-core从问题到实现的完整追踪)
  - [3.1 先理解问题：为什么传统 BPF 不可移植？](#31-先理解问题为什么传统-bpf-不可移植)
  - [3.2 解决思路：把"偏移量"变成"字段名"](#32-解决思路把偏移量变成字段名)
  - [3.3 第一步：BTF 是什么——内核的"类型字典"](#33-第一步btf-是什么内核的类型字典)
  - [3.4 第二步：CO-RE 是什么——"查字典改指令"的机制](#34-第二步core-是什么查字典改指令的机制)
  - [3.5 完整端到端流程图](#35-完整端到端流程图)
  - [3.6 BTF 与 CO-RE 的关系总结](#36-btf-与-core-的关系总结)
- [四、BTF 的其他用途（不只是 CO-RE）](#四btf-的其他用途不只是-core)
- [五、整体架构总结](#五整体架构总结)

---

## 一、eBPF 程序的加载流程

### 1.1 BPF 系统调用入口

所有 BPF 操作通过统一的 `bpf()` 系统调用进入内核，入口在 `kernel/bpf/syscall.c` 中的 `__sys_bpf()` 函数，通过 `cmd` 参数分派到不同子命令：

```c
// kernel/bpf/syscall.c
case BPF_PROG_LOAD:
    err = bpf_prog_load(&attr, uattr);
```

### 1.2 程序类型与 attach 类型

内核在 `include/uapi/linux/bpf.h` 中定义了所有 BPF 程序类型：

```c
// include/uapi/linux/bpf.h
enum bpf_prog_type {
    BPF_PROG_TYPE_UNSPEC,
    BPF_PROG_TYPE_SOCKET_FILTER,    // socket 过滤器
    BPF_PROG_TYPE_KPROBE,           // kprobe/kretprobe 探针
    BPF_PROG_TYPE_SCHED_CLS,        // TC 分类器
    BPF_PROG_TYPE_SCHED_ACT,        // TC action
    BPF_PROG_TYPE_TRACEPOINT,       // 静态 tracepoint
    BPF_PROG_TYPE_XDP,              // XDP 数据面
    BPF_PROG_TYPE_PERF_EVENT,       // perf 事件
    BPF_PROG_TYPE_CGROUP_SKB,       // cgroup skb 过滤
    BPF_PROG_TYPE_CGROUP_SOCK,      // cgroup socket
    BPF_PROG_TYPE_LWT_IN,           // 轻量级隧道入
    BPF_PROG_TYPE_LWT_OUT,          // 轻量级隧道出
    BPF_PROG_TYPE_LWT_XMIT,        // 轻量级隧道发送
    BPF_PROG_TYPE_SOCK_OPS,         // socket 操作回调
    BPF_PROG_TYPE_SK_SKB,           // socket skb 重定向
    BPF_PROG_TYPE_CGROUP_DEVICE,    // cgroup 设备访问控制
    BPF_PROG_TYPE_SK_MSG,           // socket 消息重定向
    BPF_PROG_TYPE_RAW_TRACEPOINT,   // 原始 tracepoint
    BPF_PROG_TYPE_CGROUP_SOCK_ADDR, // cgroup socket 地址
    BPF_PROG_TYPE_LWT_SEG6LOCAL,    // SRv6 本地处理
    BPF_PROG_TYPE_LIRC_MODE2,       // 红外遥控器
    BPF_PROG_TYPE_SK_REUSEPORT,     // SO_REUSEPORT 选择
    BPF_PROG_TYPE_FLOW_DISSECTOR,   // 流解析
    BPF_PROG_TYPE_CGROUP_SYSCTL,    // cgroup sysctl
    BPF_PROG_TYPE_RAW_TRACEPOINT_WRITABLE,
    BPF_PROG_TYPE_CGROUP_SOCKOPT,   // cgroup socket 选项
    BPF_PROG_TYPE_TRACING,          // fentry/fexit/fmod_ret（基于 BTF）
    BPF_PROG_TYPE_STRUCT_OPS,       // 替换内核结构体操作
    BPF_PROG_TYPE_EXT,              // 扩展其他 BPF 程序
    BPF_PROG_TYPE_LSM,              // Linux 安全模块
    BPF_PROG_TYPE_SK_LOOKUP,        // socket 查找
    BPF_PROG_TYPE_SYSCALL,          // 可执行系统调用的 BPF 程序
};
```

每种程序类型对应一组 `bpf_verifier_ops` 和 `bpf_prog_ops`，定义了该类型程序可调用的辅助函数和访问权限。这些对应关系通过 `bpf_prog_types[]` 数组建立：

```c
// kernel/bpf/syscall.c
static int find_prog_type(enum bpf_prog_type type, struct bpf_prog *prog)
{
    const struct bpf_prog_ops *ops;

    if (type >= ARRAY_SIZE(bpf_prog_types))
        return -EINVAL;
    type = array_index_nospec(type, ARRAY_SIZE(bpf_prog_types));
    ops = bpf_prog_types[type];
    if (!ops)
        return -EINVAL;

    if (!bpf_prog_is_dev_bound(prog->aux))
        prog->aux->ops = ops;
    else
        prog->aux->ops = &bpf_offload_prog_ops;
    prog->type = type;
    return 0;
}
```

### 1.3 bpf_prog_load 加载流程

`bpf_prog_load()` 是 BPF 程序加载的核心函数（`kernel/bpf/syscall.c`），完整流程为：

```
bpf_prog_load()
  ├── bpf_prog_alloc()              // 分配 bpf_prog 结构体和指令空间
  ├── copy_from_bpfptr(prog->insns) // 从用户空间拷贝 BPF 字节码
  ├── find_prog_type()              // 根据 prog_type 查找对应的 ops
  ├── bpf_check()                   // 验证器：静态分析确保安全性
  ├── bpf_prog_select_runtime()     // 选择运行时：JIT 或解释器
  │     └── bpf_int_jit_compile()   // JIT 编译为本机指令
  ├── bpf_prog_alloc_id()           // 分配全局唯一 ID
  ├── bpf_prog_kallsyms_add()       // 添加到 kallsyms（便于 perf/ftrace 识别）
  └── bpf_prog_new_fd()             // 创建文件描述符返回给用户空间
```

`struct bpf_prog` 是内核中 BPF 程序的核心数据结构（`include/linux/filter.h`）：

```c
// include/linux/filter.h
struct bpf_prog {
    u16         pages;
    u16         jited:1,        // 是否已 JIT 编译
                jit_requested:1,
                gpl_compatible:1,
                kprobe_override:1,
                // ... 其他标志位
    enum bpf_prog_type  type;
    enum bpf_attach_type expected_attach_type;
    u32         len;            // BPF 指令数
    u32         jited_len;      // JIT 后的本机指令大小
    struct bpf_prog_stats __percpu *stats;
    unsigned int (*bpf_func)(const void *ctx,
                 const struct bpf_insn *insn);  // 执行入口
    struct bpf_prog_aux *aux;
    struct bpf_insn     insnsi[];               // BPF 指令数组
};
```

关键字段 `bpf_func` 是程序的执行入口函数指针：JIT 成功后指向 JIT 生成的本机代码，否则指向解释器。

### 1.4 验证器 bpf_check

BPF 验证器（`kernel/bpf/verifier.c`）是 eBPF 安全性的核心保障，`bpf_check()` 是其入口函数。验证器对 BPF 程序进行静态分析，确保程序不会危害内核：

```c
// kernel/bpf/verifier.c（函数 bpf_check 的核心流程）
int bpf_check(struct bpf_prog **prog, union bpf_attr *attr, bpfptr_t uattr)
{
    // ...
    ret = add_subprog_and_kfunc(env);   // 识别子程序和内核函数
    ret = check_subprogs(env);          // 检查子程序边界
    ret = check_btf_info(env, attr, uattr); // 验证 BTF 信息
    ret = check_attach_btf_id(env);     // 验证 attach 目标的 BTF ID
    ret = resolve_pseudo_ldimm64(env);  // 解析 map fd / BTF 伪指令
    ret = check_cfg(env);              // 控制流图检查（DAG，无无限循环）
    ret = do_check_subprogs(env);      // 验证子程序
    ret = do_check_main(env);          // 验证主程序
    ret = check_max_stack_depth(env);  // 栈深度检查
    ret = convert_ctx_accesses(env);   // 转换上下文访问
    ret = do_misc_fixups(env);         // 杂项修复（helper 调用等）
    // ...
}
```

验证器的核心设计理念（摘自源码注释）：

> bpf_check() is a static code analyzer that walks eBPF program instruction by instruction and updates register/stack state. All paths of conditional branches are analyzed until 'bpf_exit' insn.
>
> The first pass is depth-first-search to check that the program is a DAG. It rejects programs with loops, unreachable insns, out of bounds or malformed jumps.
>
> The second pass is all possible path descent from the 1st insn. On entry to each instruction, each register has a type, and the instruction changes the types of the registers depending on instruction semantics.

验证器通过跟踪每条指令执行后的**寄存器状态**和**栈状态**，确保：
- 不存在越界内存访问
- 不存在未初始化变量使用
- 所有 helper 函数调用参数类型正确
- 程序一定会终止（禁止后向跳转形成的无限循环）

每种程序类型有自己的 `bpf_verifier_ops`，定义了可调用的辅助函数和合法的上下文访问范围：

```c
// kernel/bpf/verifier.c
static const struct bpf_verifier_ops * const bpf_verifier_ops[] = {
#define BPF_PROG_TYPE(_id, _name, prog_ctx_type, kern_ctx_type) \
    [_id] = & _name ## _verifier_ops,
#include <linux/bpf_types.h>
};
```

### 1.5 JIT 编译

验证通过后，`bpf_prog_select_runtime()` 会尝试 JIT 编译（`kernel/bpf/core.c`）：

```c
// kernel/bpf/core.c
struct bpf_prog *bpf_prog_select_runtime(struct bpf_prog *fp, int *err)
{
    bpf_prog_select_func(fp);

    if (!bpf_prog_is_dev_bound(fp->aux)) {
        fp = bpf_int_jit_compile(fp);   // 调用架构相关的 JIT
        if (!fp->jited && jit_needed) {
            *err = -ENOTSUPP;
            return fp;
        }
    }
    // ...
}
```

以 x86-64 为例，JIT 编译器在 `arch/x86/net/bpf_jit_comp.c` 中实现：

```c
// arch/x86/net/bpf_jit_comp.c
struct bpf_prog *bpf_int_jit_compile(struct bpf_prog *prog)
{
    // ...
    for (pass = 0; pass < MAX_PASSES || image; pass++) {
        proglen = do_jit(prog, addrs, image, oldproglen, &ctx, padding);
        // ...
    }
    prog->bpf_func = (void *)image;  // 执行入口指向 JIT 生成的本机代码
    prog->jited = 1;
    prog->jited_len = proglen;
    return prog;
}
```

`do_jit()` 函数逐条将 BPF 指令翻译为 x86-64 本机指令，BPF 寄存器映射到 x86 物理寄存器（R0→RAX, R1→RDI, R2→RSI 等）。

如果 JIT 不可用，则使用解释器 `___bpf_prog_run()`（`kernel/bpf/core.c`），通过 computed goto 跳转表逐条解释执行 BPF 指令。

---

## 二、eBPF 程序的挂载与执行机制

eBPF 程序加载到内核后，需要**挂载（attach）**到特定的内核挂载点才能被触发执行。不同类型的 BPF 程序有不同的挂载方式。

### 2.1 Kprobe 挂载机制

**Kprobe** 允许在任意内核函数入口/返回处动态插入探针。BPF kprobe 程序通过 **perf_event** 机制挂载。

**挂载流程：**

```
用户空间                                    内核
  │
  ├── perf_event_open(PERF_TYPE_TRACEPOINT  ──→  创建 perf_event
  │     或 kprobe PMU)                            关联到 kprobe
  │
  ├── bpf(BPF_PROG_LOAD, ...)              ──→  加载 BPF 程序
  │                                               验证 + JIT
  │
  └── ioctl(perf_fd, PERF_EVENT_IOC_SET_BPF,──→  perf_event_set_bpf_prog()
            bpf_fd)                                 └── perf_event_attach_bpf_prog()
                                                         将 prog 加入 tp_event->prog_array
```

`ioctl(PERF_EVENT_IOC_SET_BPF)` 的内核处理路径（`kernel/events/core.c`）：

```c
// kernel/events/core.c
case PERF_EVENT_IOC_SET_BPF:
{
    struct bpf_prog *prog;

    prog = bpf_prog_get(arg);        // 通过 fd 获取 bpf_prog
    if (IS_ERR(prog))
        return PTR_ERR(prog);

    err = perf_event_set_bpf_prog(event, prog, 0);
    if (err) {
        bpf_prog_put(prog);
        return err;
    }
    return 0;
}
```

`perf_event_set_bpf_prog()` 执行类型匹配检查——确保 kprobe 事件只接受 `BPF_PROG_TYPE_KPROBE` 类型的程序：

```c
// kernel/events/core.c
int perf_event_set_bpf_prog(struct perf_event *event, struct bpf_prog *prog,
                u64 bpf_cookie)
{
    // ...
    if ((is_kprobe && prog->type != BPF_PROG_TYPE_KPROBE) ||
        (is_tracepoint && prog->type != BPF_PROG_TYPE_TRACEPOINT) ||
        (is_syscall_tp && prog->type != BPF_PROG_TYPE_TRACEPOINT))
        return -EINVAL;
    // ...
    return perf_event_attach_bpf_prog(event, prog, bpf_cookie);
}
```

最终通过 `perf_event_attach_bpf_prog()`（`kernel/trace/bpf_trace.c`）将 BPF 程序加入事件的 `prog_array`：

```c
// kernel/trace/bpf_trace.c
int perf_event_attach_bpf_prog(struct perf_event *event,
                   struct bpf_prog *prog, u64 bpf_cookie)
{
    struct bpf_prog_array *old_array;
    struct bpf_prog_array *new_array;
    // ...
    old_array = bpf_event_rcu_dereference(event->tp_event->prog_array);
    ret = bpf_prog_array_copy(old_array, NULL, prog, bpf_cookie, &new_array);
    // ...
    event->prog = prog;
    event->bpf_cookie = bpf_cookie;
    rcu_assign_pointer(event->tp_event->prog_array, new_array);
    // ...
}
```

**触发执行流程：**

当被探测的内核函数执行时，kprobe 机制触发回调链：

```
内核函数被调用
  └── kprobe 断点异常
        └── kprobe_dispatcher()                  // kernel/trace/trace_kprobe.c
              └── kprobe_perf_func()
                    └── trace_call_bpf(call, regs)  // kernel/trace/bpf_trace.c
                          └── BPF_PROG_RUN_ARRAY(call->prog_array, ctx, bpf_prog_run)
                                └── bpf_prog_run(prog, ctx)
                                      └── prog->bpf_func(ctx, insn)  // JIT 代码或解释器
```

`kprobe_dispatcher()` 是 kprobe 的回调入口（`kernel/trace/trace_kprobe.c`）：

```c
// kernel/trace/trace_kprobe.c
static int kprobe_dispatcher(struct kprobe *kp, struct pt_regs *regs)
{
    struct trace_kprobe *tk = container_of(kp, struct trace_kprobe, rp.kp);
    int ret = 0;

    raw_cpu_inc(*tk->nhit);

    if (trace_probe_test_flag(&tk->tp, TP_FLAG_TRACE))
        kprobe_trace_func(tk, regs);
#ifdef CONFIG_PERF_EVENTS
    if (trace_probe_test_flag(&tk->tp, TP_FLAG_PROFILE))
        ret = kprobe_perf_func(tk, regs);  // 这里会调用 BPF 程序
#endif
    return ret;
}
```

`kprobe_perf_func()` 中检查是否有 BPF 程序挂载，有则调用 `trace_call_bpf()`：

```c
// kernel/trace/trace_kprobe.c
static int kprobe_perf_func(struct trace_kprobe *tk, struct pt_regs *regs)
{
    struct trace_event_call *call = trace_probe_event_call(&tk->tp);
    // ...
    if (bpf_prog_array_valid(call)) {
        unsigned long orig_ip = instruction_pointer(regs);
        int ret;

        ret = trace_call_bpf(call, regs);
        if (orig_ip != instruction_pointer(regs))
            return 1;
        if (!ret)
            return 0;
    }
    // ...
}
```

`trace_call_bpf()` 是实际执行 BPF 程序的核心函数（`kernel/trace/bpf_trace.c`）：

```c
// kernel/trace/bpf_trace.c
unsigned int trace_call_bpf(struct trace_event_call *call, void *ctx)
{
    unsigned int ret;

    cant_sleep();

    if (unlikely(__this_cpu_inc_return(bpf_prog_active) != 1)) {
        // 防止 BPF 程序递归调用
        ret = 0;
        goto out;
    }

    ret = BPF_PROG_RUN_ARRAY(call->prog_array, ctx, bpf_prog_run);

 out:
    __this_cpu_dec(bpf_prog_active);
    return ret;
}
```

Kprobe BPF 程序接收 `struct pt_regs *` 作为上下文，可以访问 CPU 寄存器来获取函数参数。其可用辅助函数在 `kprobe_prog_func_proto()` 中定义（`kernel/trace/bpf_trace.c`）：

```c
// kernel/trace/bpf_trace.c
static const struct bpf_func_proto *
kprobe_prog_func_proto(enum bpf_func_id func_id, const struct bpf_prog *prog)
{
    switch (func_id) {
    case BPF_FUNC_perf_event_output:
        return &bpf_perf_event_output_proto;
    case BPF_FUNC_get_stackid:
        return &bpf_get_stackid_proto;
    case BPF_FUNC_get_stack:
        return &bpf_get_stack_proto;
    case BPF_FUNC_override_return:      // 可覆盖函数返回值
        return &bpf_override_return_proto;
    case BPF_FUNC_get_func_ip:
        return &bpf_get_func_ip_proto_kprobe;
    default:
        return bpf_tracing_func_proto(func_id, prog);
    }
}
```

### 2.2 Tracepoint 挂载机制

Tracepoint 是内核中预定义的**静态探测点**（如 `sched:sched_switch`, `net:net_dev_xmit`），比 kprobe 稳定（不随内核版本变化而改变地址）。

挂载方式与 kprobe 类似，也通过 `perf_event_open` + `ioctl(PERF_EVENT_IOC_SET_BPF)` 完成。

**触发路径：**

tracepoint 触发时通过 `perf_trace_##call()` 宏展开的函数进入，最终调用 `perf_trace_run_bpf_submit()`（`kernel/events/core.c`）：

```c
// kernel/events/core.c
void perf_trace_run_bpf_submit(void *raw_data, int size, int rctx,
                   struct trace_event_call *call, u64 count,
                   struct pt_regs *regs, struct hlist_head *head,
                   struct task_struct *task)
{
    if (bpf_prog_array_valid(call)) {
        *(struct pt_regs **)raw_data = regs;
        if (!trace_call_bpf(call, raw_data) || hlist_empty(head)) {
            perf_swevent_put_recursion_context(rctx);
            return;
        }
    }
    perf_tp_event(call->event.type, count, raw_data, size, regs, head,
              rctx, task);
}
```

Tracepoint BPF 程序可用的辅助函数由 `tp_prog_func_proto()` 定义：

```c
// kernel/trace/bpf_trace.c
static const struct bpf_func_proto *
tp_prog_func_proto(enum bpf_func_id func_id, const struct bpf_prog *prog)
{
    switch (func_id) {
    case BPF_FUNC_perf_event_output:
        return &bpf_perf_event_output_proto_tp;
    case BPF_FUNC_get_stackid:
        return &bpf_get_stackid_proto_tp;
    case BPF_FUNC_get_stack:
        return &bpf_get_stack_proto_tp;
    default:
        return bpf_tracing_func_proto(func_id, prog);
    }
}
```

### 2.3 fentry/fexit：BPF Trampoline 机制

Linux 5.5+ 引入了基于 BTF 的 **BPF Trampoline** 机制，提供了比 kprobe 更高效的函数挂钩方式。它直接在目标函数的入口/出口处插入跳板代码，**无需断点异常的开销**。

**挂载方式：** 使用 `BPF_PROG_TYPE_TRACING` 类型的程序，通过 `attach_btf_id` 指定目标函数在 vmlinux BTF 中的类型 ID。

```c
// kernel/bpf/syscall.c（加载时绑定 BTF 目标）
prog->aux->attach_btf = attach_btf;
prog->aux->attach_btf_id = attr->attach_btf_id;
```

**`struct bpf_trampoline`**（`include/linux/bpf.h`）：

```c
// include/linux/bpf.h
struct bpf_trampoline {
    struct hlist_node hlist;
    struct mutex mutex;
    refcount_t refcnt;
    u64 key;
    struct {
        struct btf_func_model model;  // 函数原型模型（参数个数、大小）
        void *addr;                   // 目标函数地址
        bool ftrace_managed;          // 是否通过 ftrace 管理
    } func;
    struct bpf_prog *extension_prog;  // BPF_PROG_TYPE_EXT 扩展程序
    struct hlist_head progs_hlist[BPF_TRAMP_MAX];  // fentry/fexit/fmod_ret 程序列表
    int progs_cnt[BPF_TRAMP_MAX];
    struct bpf_tramp_image *cur_image;  // 当前跳板代码镜像
    u64 selector;
    struct module *mod;
};
```

**挂载流程** (`kernel/bpf/trampoline.c`)：

```
bpf_trampoline_link_prog(prog, tr)
  ├── kind = bpf_attach_type_to_tramp(prog)  // 确定是 fentry/fexit/fmod_ret
  ├── hlist_add_head(&prog->aux->tramp_hlist, &tr->progs_hlist[kind])
  ├── tr->progs_cnt[kind]++
  └── bpf_trampoline_update(tr)              // 重新生成跳板代码
        ├── bpf_trampoline_get_progs(tr)     // 收集所有挂载的程序
        ├── arch_prepare_bpf_trampoline()    // 架构相关：生成跳板本机代码
        └── register_fentry(tr, image)       // 修补目标函数
              ├── 若 ftrace_managed:
              │     register_ftrace_direct(ip, new_addr)  // 通过 ftrace 注册
              └── 否则:
                    bpf_arch_text_poke(ip, BPF_MOD_CALL, NULL, new_addr)
                    // 直接修改目标函数入口的 call 指令
```

`arch_prepare_bpf_trampoline()` 的 x86-64 实现（`arch/x86/net/bpf_jit_comp.c`）会生成一段跳板代码，该代码：
1. 保存所有函数参数寄存器
2. 依次调用所有 fentry BPF 程序
3. 调用原始函数
4. 依次调用所有 fexit BPF 程序
5. 恢复寄存器并返回

```c
// arch/x86/net/bpf_jit_comp.c
int arch_prepare_bpf_trampoline(struct bpf_tramp_image *im, void *image,
                void *image_end, const struct btf_func_model *m,
                u32 flags, struct bpf_tramp_progs *tprogs,
                void *orig_call)
{
    // ... 生成 push rbp; mov rbp, rsp 等序言
    save_regs(m, &prog, nr_args, stack_size);

    if (fentry->nr_progs)
        invoke_bpf(m, &prog, fentry, stack_size, ...);  // 调用 fentry 程序

    if (flags & BPF_TRAMP_F_CALL_ORIG)
        // ... 调用原始函数

    if (fexit->nr_progs)
        invoke_bpf(m, &prog, fexit, stack_size, ...);   // 调用 fexit 程序
    // ...
}
```

**Trampoline vs Kprobe 的性能差异**：kprobe 通过 `int3` 断点异常触发，有异常处理的开销；而 trampoline 直接修改目标函数的 call 指令或通过 ftrace 的 nop/call 替换，避免了异常的开销。

### 2.4 XDP 挂载机制

XDP（eXpress Data Path）程序挂载到网络设备的收包路径上，在驱动收到数据包后、分配 `sk_buff` 之前执行，实现高性能数据包处理。

挂载通过 `dev_xdp_attach()`（`net/core/dev.c`）完成：

```c
// net/core/dev.c
static int dev_xdp_attach(struct net_device *dev, struct netlink_ext_ack *extack,
              struct bpf_xdp_link *link, struct bpf_prog *new_prog,
              struct bpf_prog *old_prog, u32 flags)
{
    // ...
    mode = dev_xdp_mode(dev, flags);    // native/generic/offload 模式
    cur_prog = dev_xdp_prog(dev, mode);
    // ...
    err = dev_xdp_install(dev, mode, bpf_op, extack, flags, new_prog);
    // ...
}
```

`dev_xdp_install()` 通过网络设备驱动的 `ndo_bpf` 回调安装 XDP 程序：

```c
// net/core/dev.c
static int dev_xdp_install(struct net_device *dev, enum bpf_xdp_mode mode,
               bpf_op_t bpf_op, struct netlink_ext_ack *extack,
               u32 flags, struct bpf_prog *prog)
{
    struct netdev_bpf xdp;

    memset(&xdp, 0, sizeof(xdp));
    xdp.command = mode == XDP_MODE_HW ? XDP_SETUP_PROG_HW : XDP_SETUP_PROG;
    xdp.prog = prog;

    err = bpf_op(dev, &xdp);  // 调用驱动的 ndo_bpf 回调
    // ...
}
```

**XDP 执行路径**（`include/linux/filter.h`）：

```c
// include/linux/filter.h
static __always_inline u32 bpf_prog_run_xdp(const struct bpf_prog *prog,
                        struct xdp_buff *xdp)
{
    u32 act = __bpf_prog_run(prog, xdp, BPF_DISPATCHER_FUNC(xdp));

    if (static_branch_unlikely(&bpf_master_redirect_enabled_key)) {
        if (act == XDP_TX && netif_is_bond_slave(xdp->rxq->dev))
            act = xdp_master_redirect(xdp);
    }
    return act;
}
```

XDP 程序返回动作码：`XDP_PASS`（继续协议栈处理）、`XDP_DROP`（丢弃）、`XDP_TX`（从同端口发出）、`XDP_REDIRECT`（重定向到其他端口/CPU）。

### 2.5 BPF 程序的执行路径

所有 BPF 程序最终都通过统一的执行框架运行。

**单程序执行**（`include/linux/filter.h`）：

```c
// include/linux/filter.h
static __always_inline u32 bpf_prog_run(const struct bpf_prog *prog,
                    const void *ctx)
{
    return __bpf_prog_run(prog, ctx, bpf_dispatcher_nop_func);
}

static __always_inline u32 __bpf_prog_run(const struct bpf_prog *prog,
                      const void *ctx,
                      bpf_dispatcher_fn dfunc)
{
    u32 ret;
    cant_migrate();

    if (static_branch_unlikely(&bpf_stats_enabled_key)) {
        // 统计模式：记录执行时间和次数
        struct bpf_prog_stats *stats;
        u64 start = sched_clock();
        ret = dfunc(ctx, prog->insnsi, prog->bpf_func);
        stats = this_cpu_ptr(prog->stats);
        u64_stats_inc(&stats->cnt);
        u64_stats_add(&stats->nsecs, sched_clock() - start);
    } else {
        ret = dfunc(ctx, prog->insnsi, prog->bpf_func);
    }
    return ret;
}
```

关键调用 `dfunc(ctx, prog->insnsi, prog->bpf_func)`：
- `ctx`：上下文指针（如 `struct pt_regs *`、`struct xdp_buff *` 等）
- `prog->insnsi`：BPF 指令数组（解释器使用）
- `prog->bpf_func`：JIT 编译后的本机函数指针，或解释器入口

**程序数组批量执行**（`include/linux/bpf.h`）：

```c
// include/linux/bpf.h
static __always_inline u32
BPF_PROG_RUN_ARRAY(const struct bpf_prog_array __rcu *array_rcu,
           const void *ctx, bpf_prog_run_fn run_prog)
{
    const struct bpf_prog_array_item *item;
    const struct bpf_prog *prog;
    u32 ret = 1;

    migrate_disable();
    rcu_read_lock();
    array = rcu_dereference(array_rcu);
    item = &array->items[0];
    while ((prog = READ_ONCE(item->prog))) {
        run_ctx.bpf_cookie = item->bpf_cookie;
        ret &= run_prog(prog, ctx);
        item++;
    }
    rcu_read_unlock();
    migrate_enable();
    return ret;
}
```

同一挂载点可以关联多个 BPF 程序，通过 `bpf_prog_array` 管理，RCU 保护实现无锁读取。

---

## 三、BTF 与 CO-RE：从问题到实现的完整追踪

### 3.1 先理解问题：为什么传统 BPF 不可移植？

假设你写了一个 BPF 程序想读取当前进程的 PID：

```c
struct task_struct *task = (void *)bpf_get_current_task();
int pid;
bpf_probe_read_kernel(&pid, sizeof(pid), (void *)task + 1224);
```

这里的 `1224` 是什么？是 `pid` 字段在你**编译时**那个内核版本的 `task_struct` 中的字节偏移量。问题在于：

- **5.4 内核**的 `task_struct` 中 `pid` 可能在偏移 1224
- **5.10 内核**添加了几个新字段后，`pid` 变成了偏移 1312
- **5.15 内核**又改了布局，`pid` 变成了偏移 1296

**如果你把在 5.4 上编译的程序拿到 5.15 上跑，`task + 1224` 读到的不是 `pid`，而是别的字段的数据。** 程序产生错误结果或者直接崩溃。

传统的解决方案是在每台目标机器上都安装 Clang + 内核头文件，现场编译。

**BTF 和 CO-RE 解决的就是这个问题：让编译好的 BPF 程序能在不同内核版本上正确运行，而不需要重新编译。**

### 3.2 解决思路：把"偏移量"变成"字段名"

CO-RE 的核心思想非常简单：

> **不硬编码字段偏移量 `1224`，而是记录"我要访问 `task_struct` 的第 15 个成员 `pid`"这个语义信息。加载时再根据目标内核查出 `pid` 实际在哪个偏移，把指令里的数字改掉。**

但要实现这个思路，需要两样东西：

1. **BTF** — 提供"字典"：目标内核的 `task_struct` 长什么样，每个字段在哪个偏移
2. **CO-RE** — 提供"查字典并改指令"的机制

**BTF 是数据，CO-RE 是算法。BTF 告诉你 `pid` 在偏移 1296，CO-RE 负责把 BPF 指令中的 `1224` 改成 `1296`。**

下面从源码层面追踪这整个过程。

---

### 3.3 第一步：BTF 是什么——内核的"类型字典"

BTF（BPF Type Format）是一种**紧凑的类型描述格式**，它把整个内核的结构体定义（数万个）用几 MB 的二进制数据描述清楚。

#### 3.3.1 BTF 的数据格式

定义在 `include/uapi/linux/btf.h`。

**文件头**（第 11 行）指明了类型段和字符串段的位置：

```c
// include/uapi/linux/btf.h:11
struct btf_header {
    __u16   magic;       // 0xeB9F
    __u8    version;     // 1
    __u8    flags;
    __u32   hdr_len;
    __u32   type_off;    // 类型段在数据中的偏移
    __u32   type_len;    // 类型段长度
    __u32   str_off;     // 字符串段偏移
    __u32   str_len;     // 字符串段长度
};
```

**每个类型**用 `btf_type` 描述（第 30 行）：

```c
// include/uapi/linux/btf.h:30
struct btf_type {
    __u32 name_off;     // 类型名在字符串段中的偏移（如 "task_struct"）
    __u32 info;         // bits 24-27: kind（INT/PTR/STRUCT/...）
                        // bits 0-15: vlen（成员数量）
    union {
        __u32 size;     // STRUCT/UNION/INT 用：类型的总字节大小
        __u32 type;     // PTR/TYPEDEF 用：指向的类型 ID
    };
};
```

BTF 支持的类型种类（第 59 行）：

```c
// include/uapi/linux/btf.h:59
#define BTF_KIND_INT        1    // 整数（int, char, bool...）
#define BTF_KIND_PTR        2    // 指针
#define BTF_KIND_ARRAY      3    // 数组
#define BTF_KIND_STRUCT     4    // 结构体  ← 这是 CO-RE 最关心的
#define BTF_KIND_UNION      5    // 联合体
#define BTF_KIND_ENUM       6    // 枚举
#define BTF_KIND_FWD        7    // 前向声明
#define BTF_KIND_TYPEDEF    8    // typedef
#define BTF_KIND_VOLATILE   9
#define BTF_KIND_CONST      10
#define BTF_KIND_RESTRICT   11
#define BTF_KIND_FUNC       12   // 函数
#define BTF_KIND_FUNC_PROTO 13   // 函数原型
#define BTF_KIND_VAR        14
#define BTF_KIND_DATASEC    15
#define BTF_KIND_FLOAT      16
```

**最关键的部分**：对于 `BTF_KIND_STRUCT`，每个成员用 `btf_member` 描述（第 114 行），它记录了**成员名、类型和精确的位偏移**：

```c
// include/uapi/linux/btf.h:114
struct btf_member {
    __u32   name_off;   // 成员名（如 "pid"）在字符串段中的偏移
    __u32   type;       // 成员的类型 ID
    __u32   offset;     // 成员的位偏移（bit offset）
};
```

#### 3.3.2 具体例子：BTF 如何描述 task_struct

假设 `task_struct` 在某个内核中定义为（极度简化）：

```c
struct task_struct {    // BTF type_id = 123, kind = STRUCT, size = 9024
    // ... 前面省略很多字段 ...
    pid_t  pid;         // btf_member: name="pid", type=INT, offset=10368 bits (= 1296 bytes)
    pid_t  tgid;        // btf_member: name="tgid", type=INT, offset=10400 bits (= 1300 bytes)
    // ... 后面省略很多字段 ...
};
```

在 BTF 二进制数据中，这被编码为：

```
btf_type {
    name_off → 指向字符串 "task_struct"
    info → kind=STRUCT, vlen=200（假设有 200 个成员）
    size → 9024
}
后跟 200 个 btf_member:
    ...
    btf_member { name_off→"pid",  type→INT_type_id, offset→10368 }  // 第 15 个成员
    btf_member { name_off→"tgid", type→INT_type_id, offset→10400 }  // 第 16 个成员
    ...
```

**这就是 BTF 的本质：它是一个把"task_struct 的 pid 字段在偏移 1296 字节处"这样的信息编码成二进制的格式。**

#### 3.3.3 BTF 从哪里来——构建时生成

BTF 的生成流程：**DWARF → pahole 转换 → .BTF section → 嵌入 vmlinux**

在 `scripts/link-vmlinux.sh` 第 211 行的 `gen_btf()` 函数中：

```bash
# scripts/link-vmlinux.sh:228
LLVM_OBJCOPY="${OBJCOPY}" ${PAHOLE} -J ${PAHOLE_FLAGS} ${1}
# pahole -J: 读取 vmlinux 中的 DWARF 调试信息，生成去重的 BTF 并写入 .BTF section

# 提取 .BTF section
${OBJCOPY} --only-section=.BTF --set-section-flags .BTF=alloc,readonly \
    --strip-all ${1} ${2} 2>/dev/null
```

链接器脚本（`include/asm-generic/vmlinux.lds.h` 第 664 行）将 `.BTF` 嵌入内核镜像：

```c
// include/asm-generic/vmlinux.lds.h:664
.BTF : AT(ADDR(.BTF) - LOAD_OFFSET) {
    __start_BTF = .;    // BTF 数据起始地址
    KEEP(*(.BTF))
    __stop_BTF = .;     // BTF 数据结束地址
}
```

#### 3.3.4 BTF 如何暴露给用户空间

`kernel/bpf/sysfs_btf.c` 将内核内嵌的 BTF 通过 sysfs 暴露为文件：

```c
// kernel/bpf/sysfs_btf.c:12
extern char __weak __start_BTF[];
extern char __weak __stop_BTF[];

// 第 32 行
static int __init btf_vmlinux_init(void)
{
    bin_attr_btf_vmlinux.size = __stop_BTF - __start_BTF;
    btf_kobj = kobject_create_and_add("btf", kernel_kobj);
    return sysfs_create_bin_file(btf_kobj, &bin_attr_btf_vmlinux);
}
```

结果：用户空间可以直接读取 `/sys/kernel/btf/vmlinux` 获取当前运行内核的全部类型信息。**这就是 BTF 替代内核头文件的原理——类型信息随内核一起分发，不需要额外安装。**

#### 3.3.5 BTF 在内核中的解析

内核自身也解析 BTF 来支持 BPF 验证。`btf_parse_vmlinux()`（`kernel/bpf/btf.c` 第 4538 行）：

```c
// kernel/bpf/btf.c:4538
struct btf *btf_parse_vmlinux(void)
{
    btf->data = __start_BTF;                         // 指向内嵌的 BTF 数据
    btf->data_size = __stop_BTF - __start_BTF;
    btf->kernel_btf = true;
    snprintf(btf->name, sizeof(btf->name), "vmlinux");

    err = btf_parse_hdr(env);        // 解析文件头
    err = btf_parse_str_sec(env);    // 解析字符串段
    err = btf_check_all_metas(env);  // 验证所有类型元数据
}
```

通过 `btf_type_by_id()` 可以按 ID 查找任意类型（第 709 行）：

```c
// kernel/bpf/btf.c:709
const struct btf_type *btf_type_by_id(const struct btf *btf, u32 type_id)
{
    while (type_id < btf->start_id)
        btf = btf->base_btf;       // 支持 split BTF（模块 BTF 基于 vmlinux BTF）
    type_id -= btf->start_id;
    if (type_id >= btf->nr_types)
        return NULL;
    return btf->types[type_id];     // 直接数组索引，O(1) 查找
}
```

验证器通过 `btf_struct_access()`（第 5113 行）利用 BTF 验证 BPF 程序对结构体字段的访问：

```c
// kernel/bpf/btf.c:5113
int btf_struct_access(struct bpf_verifier_log *log, const struct btf *btf,
                      const struct btf_type *t, int off, int size,
                      enum bpf_access_type atype, u32 *next_btf_id)
{
    do {
        err = btf_struct_walk(log, btf, t, off, size, &id);
        switch (err) {
        case WALK_PTR:
            *next_btf_id = id;
            return PTR_TO_BTF_ID;    // 访问的是指针字段
        case WALK_SCALAR:
            return SCALAR_VALUE;      // 访问的是标量字段
        case WALK_STRUCT:
            t = btf_type_by_id(btf, id);  // 访问的是嵌套结构体，继续深入
            off = 0;
            break;
        }
    } while (t);
}
```

---

### 3.4 第二步：CO-RE 是什么——"查字典改指令"的机制

现在我们知道 BTF 提供了目标内核的"类型字典"，但还需要一个机制来：

1. **记录** BPF 程序中哪些指令依赖了结构体布局
2. **查询**目标内核 BTF 得到正确的偏移量
3. **修补**那些指令

这就是 CO-RE 做的事。

#### 3.4.1 Clang 做了什么——生成重定位记录

当你在 BPF 程序中写：

```c
#include "vmlinux.h"  // 从开发机的 BTF 生成的头文件
#include <bpf/bpf_core_read.h>

SEC("kprobe/do_fork")
int trace_fork(struct pt_regs *ctx)
{
    struct task_struct *task = (void *)bpf_get_current_task();
    pid_t pid = BPF_CORE_READ(task, pid);   // ← CO-RE 读取
    return 0;
}
```

`BPF_CORE_READ` 宏展开后（`tools/lib/bpf/bpf_core_read.h` 第 402 行）：

```c
// tools/lib/bpf/bpf_core_read.h:402
#define BPF_CORE_READ(src, a, ...) ({
    ___type((src), a, ##__VA_ARGS__) __r;
    BPF_CORE_READ_INTO(&__r, (src), a, ##__VA_ARGS__);
    __r;
})
```

底层的 `bpf_core_read` 宏（第 205 行）使用了 Clang 内建函数：

```c
// tools/lib/bpf/bpf_core_read.h:205
#define bpf_core_read(dst, sz, src) \
    bpf_probe_read_kernel(dst, sz, \
        (const void *)__builtin_preserve_access_index(src))
```

**关键在 `__builtin_preserve_access_index()`**。这是 Clang 的一个特殊内建函数，它告诉 Clang：

> "这个表达式 `&task->pid` 不要只生成一个固定偏移的指令，还要在 ELF 的 `.BTF.ext` section 中记录一条重定位信息，说明这条指令依赖于 `task_struct` 的 `pid` 字段偏移。"

Clang 编译后，BPF ELF 文件中包含：

```
.text section:      BPF 字节码（其中 task->pid 的偏移用编译时的值填充，如 1224）
.BTF section:       本地 BTF（编译时用的内核类型信息）
.BTF.ext section:   CO-RE 重定位记录（哪条指令要修补、关联哪个类型的哪个字段）
```

#### 3.4.2 重定位记录长什么样

每条 CO-RE 重定位记录的结构（`tools/lib/bpf/relo_core.h` 第 71 行）：

```c
// tools/lib/bpf/relo_core.h:71
struct bpf_core_relo {
    __u32   insn_off;       // 需要修补的 BPF 指令在程序中的字节偏移
    __u32   type_id;        // 本地 BTF 中的类型 ID（如 task_struct = 123）
    __u32   access_str_off; // 访问路径字符串在 BTF 字符串段中的偏移
    enum bpf_core_relo_kind kind;  // 要什么信息（偏移？大小？是否存在？）
};
```

**`access_str_off` 指向的字符串** 编码了字段访问路径，例如：

```
"0:15"  →  第 0 层（解引用指针）的第 15 个成员（pid）
"0:1:0:5" → 嵌套访问：第 1 个成员是匿名 struct，其中第 0 个字段是数组，取第 5 个元素
```

**`kind`** 说明要提取什么信息（第 10 行）：

```c
// tools/lib/bpf/relo_core.h:10
enum bpf_core_relo_kind {
    BPF_FIELD_BYTE_OFFSET = 0,  // 要的是字段的字节偏移量
    BPF_FIELD_BYTE_SIZE = 1,    // 要的是字段的字节大小
    BPF_FIELD_EXISTS = 2,       // 只想知道目标内核有没有这个字段
    BPF_FIELD_SIGNED = 3,       // 字段是否有符号
    BPF_FIELD_LSHIFT_U64 = 4,   // 位域左移量
    BPF_FIELD_RSHIFT_U64 = 5,   // 位域右移量
    BPF_TYPE_ID_LOCAL = 6,      // 类型在本地 BTF 中的 ID
    BPF_TYPE_ID_TARGET = 7,     // 类型在目标 BTF 中的 ID
    BPF_TYPE_EXISTS = 8,        // 类型在目标内核中是否存在
    BPF_TYPE_SIZE = 9,          // 类型在目标内核中的大小
    BPF_ENUMVAL_EXISTS = 10,    // 枚举值是否存在
    BPF_ENUMVAL_VALUE = 11,     // 枚举值的整数值
};
```

#### 3.4.3 libbpf 加载时做了什么——完整重定位流程

**CO-RE 重定位完全在用户空间由 libbpf 执行，内核不参与。** 内核收到的是已经修补好的指令。

加载顺序在 `tools/lib/bpf/libbpf.c` 第 6905 行：

```c
// tools/lib/bpf/libbpf.c:6905
err = bpf_object__probe_loading(obj);
err = err ? : bpf_object__load_vmlinux_btf(obj, false);   // ← 加载目标内核 BTF
err = err ? : bpf_object__resolve_externs(obj, obj->kconfig);
err = err ? : bpf_object__sanitize_and_load_btf(obj);
err = err ? : bpf_object__sanitize_maps(obj);
err = err ? : bpf_object__init_kern_struct_ops_maps(obj);
err = err ? : bpf_object__create_maps(obj);
err = err ? : bpf_object__relocate(obj, ...);              // ← CO-RE 重定位在这里
```

**第一步：加载目标内核的 BTF**

`btf__load_vmlinux_btf()`（`tools/lib/bpf/btf.c` 第 4456 行）按优先级从多个位置查找：

```c
// tools/lib/bpf/btf.c:4456
struct btf *btf__load_vmlinux_btf(void)
{
    locations[] = {
        { "/sys/kernel/btf/vmlinux", true },    // 优先：sysfs 原始 BTF
        { "/boot/vmlinux-%1$s" },                // 回退：磁盘上的 vmlinux
        { "/lib/modules/%1$s/vmlinux-%1$s" },
        { "/lib/modules/%1$s/build/vmlinux" },
        // ...
    };

    uname(&buf);  // 获取当前内核版本
    for (i = 0; i < ARRAY_SIZE(locations); i++) {
        snprintf(path, PATH_MAX, locations[i].path_fmt, buf.release);
        if (locations[i].raw_btf)
            btf = btf__parse_raw(path);     // 解析原始 BTF 二进制
        else
            btf = btf__parse_elf(path, NULL); // 从 ELF 提取 BTF
        if (!err) return btf;
    }
}
```

**第二步：执行 CO-RE 重定位**

`bpf_object__relocate_core()`（第 5185 行）遍历所有 CO-RE 记录：

```c
// tools/lib/bpf/libbpf.c:5185
bpf_object__relocate_core(struct bpf_object *obj, const char *targ_btf_path)
{
    // 遍历 .BTF.ext 中的每条 CO-RE 重定位记录
    for_each_btf_ext_sec(seg, sec) {
        err = bpf_core_apply_relo(prog, rec, i, obj->btf, cand_cache);
    }
}
```

**第三步：单条重定位的处理**

核心函数 `bpf_core_apply_relo_insn()`（`tools/lib/bpf/relo_core.c` 第 1145 行）的完整流程：

```c
// tools/lib/bpf/relo_core.c:1145
int bpf_core_apply_relo_insn(const char *prog_name, struct bpf_insn *insn,
                             int insn_idx,
                             const struct bpf_core_relo *relo,
                             int relo_idx,
                             const struct btf *local_btf,
                             struct bpf_core_cand_list *cands)
{
    // ① 从本地 BTF 获取类型名
    local_id = relo->type_id;
    local_type = btf__type_by_id(local_btf, local_id);
    local_name = btf__name_by_offset(local_btf, local_type->name_off);
    // local_name = "task_struct"

    // ② 解析访问路径字符串
    spec_str = btf__name_by_offset(local_btf, relo->access_str_off);
    // spec_str = "0:15" (第 15 个成员)
    err = bpf_core_parse_spec(local_btf, local_id, spec_str,
                              relo->kind, &local_spec);
    // local_spec.bit_offset = 1224*8 = 9792（编译时的偏移）

    // ③ 在目标内核 BTF 中查找匹配的类型
    for (i = 0; i < cands->len; i++) {
        // 对每个候选类型（名字匹配 "task_struct" 的类型）
        err = bpf_core_spec_match(&local_spec,
                                  cands->cands[i].btf,    // 目标 BTF
                                  cands->cands[i].id,      // 目标类型 ID
                                  &cand_spec);
        // bpf_core_spec_match 做的事：
        //   在目标 BTF 的 task_struct 中按名字找 "pid" 字段
        //   找到后记录 cand_spec.bit_offset = 1296*8 = 10368

        // ④ 计算原始值和新值
        err = bpf_core_calc_relo(prog_name, relo, relo_idx,
                                 &local_spec, &cand_spec, &cand_res);
        // cand_res.orig_val = 1224  (本地偏移)
        // cand_res.new_val  = 1296  (目标偏移)
    }

patch_insn:
    // ⑤ 修补 BPF 指令
    return bpf_core_patch_insn(prog_name, insn, insn_idx,
                               relo, relo_idx, &targ_res);
}
```

**第四步：修补指令的具体细节**

`bpf_core_patch_insn()`（第 919 行）根据指令类型修补不同的字段：

```c
// tools/lib/bpf/relo_core.c:919
static int bpf_core_patch_insn(const char *prog_name, struct bpf_insn *insn,
                               int insn_idx, const struct bpf_core_relo *relo,
                               int relo_idx, const struct bpf_core_relo_res *res)
{
    orig_val = res->orig_val;   // 1224（编译时偏移）
    new_val = res->new_val;     // 1296（目标内核偏移）

    switch (BPF_CLASS(insn->code)) {
    case BPF_ALU:
    case BPF_ALU64:
        // 算术指令：rX += <imm>
        // 修补 insn->imm
        insn->imm = new_val;    // 1224 → 1296
        break;

    case BPF_LDX:
    case BPF_ST:
    case BPF_STX:
        // 内存访问指令：rX = *(u32 *)(rY + <off>)
        // 修补 insn->off
        insn->off = new_val;    // 1224 → 1296
        // 如果字段大小也变了（如 u32 → u64），还要修补访问宽度
        if (res->new_sz != res->orig_sz) {
            insn->code = BPF_MODE(insn->code)
                       | insn_bytes_to_bpf_size(res->new_sz)
                       | BPF_CLASS(insn->code);
        }
        break;

    case BPF_LD:
        // 64 位立即数加载：rX = <imm64>
        insn[0].imm = new_val;
        insn[1].imm = 0;
        break;
    }
}
```

如果目标内核中**找不到该字段**（`BPF_FIELD_EXISTS` 返回 0），CO-RE 会"毒化"该指令（第 864 行）：

```c
// tools/lib/bpf/relo_core.c:864
static void bpf_core_poison_insn(...)
{
    insn->code = BPF_JMP | BPF_CALL;
    insn->imm = 195896080;  // 0xbad2310 = "bad relo"
    // 如果这条指令可达，验证器会报错 "invalid func unknown#195896080"
    // 如果不可达（被 if 条件跳过），程序正常加载
}
```

这使得 BPF 程序可以写防御性代码：

```c
if (bpf_core_field_exists(task->new_field_in_5_15)) {
    // 只在 5.15+ 内核上执行
    val = BPF_CORE_READ(task, new_field_in_5_15);
} else {
    // 旧内核的回退路径
    val = 0;
}
```

#### 3.4.4 类型匹配是怎么做的

`bpf_core_spec_match()`（`relo_core.c` 第 447 行）在目标 BTF 中匹配本地类型的核心逻辑：

1. **按名字匹配**：在目标 BTF 中搜索所有名为 `"task_struct"` 的 `STRUCT` 类型
2. **按访问路径匹配**：按照 `"0:15"` 路径，在目标 `task_struct` 中**按字段名**逐级查找
   - 先取本地类型的第 15 个成员，得到名字 `"pid"`
   - 在目标 `task_struct` 的所有成员中搜索名为 `"pid"` 的字段
   - 找到后记录目标的 `bit_offset`
3. **兼容性检查**：`bpf_core_fields_are_compat()`（第 301 行）确保本地和目标字段的类型兼容（都是 int、都是指针等）

**注意：匹配用的是字段名而不是字段序号。** 如果内核在 `pid` 前面添加了新字段导致 `pid` 从第 15 个变成第 20 个，CO-RE 照样能找到它，因为它搜的是 `"pid"` 这个名字。

---

### 3.5 完整端到端流程图

```
 ┌─────────────────────── 开发机（编译一次）─────────────────────────┐
 │                                                                  │
 │   BPF C 源码:                                                     │
 │     pid = BPF_CORE_READ(task, pid);                               │
 │              │                                                    │
 │              ▼                                                    │
 │   Clang + __builtin_preserve_access_index()                       │
 │              │                                                    │
 │              ▼ 产生                                                │
 │   BPF ELF 文件:                                                   │
 │   ┌──────────────────────────────────────────────────────────┐    │
 │   │ .text:     r1 = *(u32 *)(r6 + 1224)  ← 编译时偏移       │    │
 │   │ .BTF:      本地类型 {task_struct: pid at offset 1224}    │    │
 │   │ .BTF.ext:  CO-RE relo {insn=42, type=task_struct,        │    │
 │   │             access="0:15"(pid), kind=BYTE_OFFSET}        │    │
 │   └──────────────────────────────────────────────────────────┘    │
 │                                                                  │
 └──────────────────────────┬───────────────────────────────────────┘
                            │ 分发到目标机
                            ▼
 ┌─────────────────────── 目标机（运行时）─────────────────────────┐
 │                                                                  │
 │   Linux Kernel 内嵌 BTF:                                          │
 │     /sys/kernel/btf/vmlinux                                       │
 │     → task_struct: pid at offset 1296  ← 目标内核的偏移不同！      │
 │                                                                  │
 │   libbpf 加载过程:                                                │
 │                                                                  │
 │   ① bpf_object__load_vmlinux_btf()                               │
 │      读取 /sys/kernel/btf/vmlinux → 得到目标 BTF                  │
 │                                                                  │
 │   ② bpf_object__relocate_core()                                  │
 │      遍历 .BTF.ext 中的每条 CO-RE 记录                             │
 │                                                                  │
 │   ③ bpf_core_apply_relo_insn():                                  │
 │      ├─ 解析本地 spec: task_struct 第 15 个成员 "pid"              │
 │      ├─ 目标 BTF 搜索: task_struct 中名为 "pid" 的成员             │
 │      ├─ 找到: pid at offset 1296（目标偏移）                       │
 │      └─ 计算: orig=1224, new=1296                                 │
 │                                                                  │
 │   ④ bpf_core_patch_insn():                                       │
 │      insn->off = 1296  （修补指令：1224 → 1296）                   │
 │                                                                  │
 │   ⑤ 修补后的指令:                                                 │
 │      r1 = *(u32 *)(r6 + 1296)  ← 正确的目标偏移！                 │
 │                                                                  │
 │   ⑥ bpf(BPF_PROG_LOAD, ...) → 内核验证 + JIT → 正确执行           │
 │                                                                  │
 └──────────────────────────────────────────────────────────────────┘
```

### 3.6 BTF 与 CO-RE 的关系总结

| 概念 | 角色 | 类比 |
|------|------|------|
| **BTF** | 类型信息数据库 | 字典 |
| **本地 BTF**（.BTF section） | 编译时的内核类型信息 | 你查过的旧版字典 |
| **目标 BTF**（/sys/kernel/btf/vmlinux） | 运行时内核的类型信息 | 目标环境的新版字典 |
| **CO-RE 重定位记录**（.BTF.ext） | "哪条指令要查什么词" | 标注了需要查字典的位置 |
| **libbpf CO-RE 引擎**（relo_core.c） | 查字典 + 改指令 | 翻译员 |
| **Clang 内建函数** | 编译时标记需要重定位的访问 | 在文本中划出需要翻译的词 |

**核心关系**：

1. **没有 BTF，CO-RE 无法工作** — CO-RE 需要查询目标内核的类型信息来计算正确偏移
2. **没有 CO-RE，BTF 只是数据** — BTF 仅提供类型描述，不会自动修改 BPF 指令
3. **两者配合** — Clang 编译时记录访问意图（CO-RE 记录） + BTF 提供目标信息 → libbpf 在加载时完成指令修补

---

## 四、BTF 的其他用途（不只是 CO-RE）

BTF 除了服务 CO-RE 外，在内核中还有重要用途：

### 4.1 验证器类型安全检查

BPF 验证器利用 BTF 跟踪 `PTR_TO_BTF_ID` 类型的指针，确保 BPF 程序只能在合法偏移处访问合法字段：

```c
// kernel/bpf/btf.c:5113
int btf_struct_access(log, btf, t, off, size, atype, next_btf_id)
// 验证 BPF 程序对 off 偏移的访问是否合法
// btf_struct_walk() 沿着 BTF 类型信息检查：
//   - off 是否落在某个合法成员的范围内
//   - 访问大小是否匹配成员类型
//   - 如果成员是指针，返回 PTR_TO_BTF_ID 以便继续跟踪
```

### 4.2 fentry/fexit 函数参数描述

`BPF_PROG_TYPE_TRACING` 程序通过 BTF 获取目标内核函数的参数信息，实现**类型安全**的参数访问（不需要像 kprobe 那样手动从 `pt_regs` 中提取）。

### 4.3 BPF map 的 pretty-print

`bpftool map dump` 命令利用 BTF 将 map 的 key/value 以人类可读格式显示，而非原始字节。

### 4.4 生成 vmlinux.h

`bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h` 从 BTF 反向生成 C 头文件，供 BPF 程序使用。这使得开发者不需要安装内核头文件包。

---

## 五、整体架构总结

### 完整工作流

```
   开发机 (编译一次)                           目标机 (直接运行)
 ┌──────────────────────┐              ┌──────────────────────────────────┐
 │                      │              │           Linux Kernel            │
 │  BPF C 源码          │              │  ┌────────────────────────────┐  │
 │  + CO-RE 宏          │              │  │ 嵌入的 vmlinux BTF        │  │
 │  + vmlinux.h         │              │  │ (__start_BTF ~ __stop_BTF) │  │
 │         │            │              │  └──────────┬─────────────────┘  │
 │    Clang/LLVM        │              │             │                    │
 │  __builtin_preserve  │              │             ▼ /sys/kernel/btf/   │
 │  _access_index()     │   ──────→    │               vmlinux            │
 │         │            │   分发到     │                                   │
 │         ▼            │   目标机     │  ┌────────────────────────────┐  │
 │  BPF ELF 文件        │              │  │      libbpf               │  │
 │  ├ .text:            │              │  │                            │  │
 │  │  rX=*(rY+1224)    │              │  │  1. 读取目标 BTF           │  │
 │  │  (编译时偏移)     │              │  │     btf__load_vmlinux_btf()│  │
 │  ├ .BTF:             │              │  │                            │  │
 │  │  本地类型信息     │              │  │  2. CO-RE 重定位           │  │
 │  └ .BTF.ext:         │              │  │     bpf_core_apply_relo_   │  │
 │    CO-RE 记录:       │              │  │     insn(): 按名字匹配     │  │
 │    "task_struct.pid   │              │  │     字段，计算新偏移       │  │
 │     → insn #42"      │              │  │                            │  │
 │                      │              │  │  3. 修补指令               │  │
 └──────────────────────┘              │  │     bpf_core_patch_insn(): │  │
                                       │  │     1224 → 1296            │  │
                                       │  │                            │  │
                                       │  │  4. BPF_PROG_LOAD         │  │
                                       │  └──────────┬─────────────────┘  │
                                       │             ▼                    │
                                       │  ┌────────────────────────────┐  │
                                       │  │     BPF 验证器 (bpf_check) │  │
                                       │  │  - BTF 类型安全检查        │  │
                                       │  │  - 指令安全性验证          │  │
                                       │  └──────────┬─────────────────┘  │
                                       │             ▼                    │
                                       │  ┌────────────────────────────┐  │
                                       │  │     JIT 编译              │  │
                                       │  │  BPF 字节码 → 本机指令    │  │
                                       │  └──────────┬─────────────────┘  │
                                       │             ▼                    │
                                       │  ┌────────────────────────────┐  │
                                       │  │  挂载到内核挂载点         │  │
                                       │  │  - kprobe: perf_event     │  │
                                       │  │  - tracepoint: perf_event │  │
                                       │  │  - fentry: trampoline     │  │
                                       │  │  - XDP: ndo_bpf           │  │
                                       │  └──────────┬─────────────────┘  │
                                       │             ▼                    │
                                       │    内核事件触发 → BPF 程序执行  │
                                       └──────────────────────────────────┘
```

### 挂载方式对比

| 挂载方式 | 程序类型 | 挂载目标 | 挂载 API | 触发机制 | 上下文 |
|---------|---------|---------|---------|---------|-------|
| kprobe | `BPF_PROG_TYPE_KPROBE` | 任意内核函数 | perf_event + ioctl | int3 断点异常 | `struct pt_regs *` |
| tracepoint | `BPF_PROG_TYPE_TRACEPOINT` | 预定义静态探测点 | perf_event + ioctl | trace event 回调 | trace event 数据 |
| fentry/fexit | `BPF_PROG_TYPE_TRACING` | BTF 已知的内核函数 | BPF_LINK_CREATE | trampoline 跳板 | 函数参数（类型安全） |
| XDP | `BPF_PROG_TYPE_XDP` | 网络设备 | netlink / bpf_link | 驱动收包路径 | `struct xdp_buff *` |
| socket filter | `BPF_PROG_TYPE_SOCKET_FILTER` | socket | setsockopt | 协议栈收包 | `struct sk_buff *` |
| TC | `BPF_PROG_TYPE_SCHED_CLS` | 网络设备 qdisc | tc 命令 | TC 入口/出口 | `struct sk_buff *` |
| LSM | `BPF_PROG_TYPE_LSM` | 安全钩子 | BPF_LINK_CREATE | LSM hook 点 | hook 参数 |

### 传统方式 vs BTF + CO-RE 对比

| 对比项 | 传统方式 (BCC) | BTF + CO-RE |
|--------|---------------|-------------|
| 目标机依赖 | Clang + LLVM + 内核头文件 (~数百MB) | 仅 libbpf (~几百KB) |
| 编译时机 | 目标机上现场编译 | 开发机上编译一次 |
| 内核结构信息来源 | 内核头文件 (`.h` 文件) | 内嵌的 BTF (~1-5MB，随内核分发) |
| 可移植性 | 仅适用于编译时的那个内核版本 | 跨内核版本可重定位 |
| 字段偏移处理 | 编译时硬编码 | 加载时根据目标 BTF 动态修补 |

### 关键源码文件索引

| 组件 | 源码路径 |
|------|---------|
| BPF 系统调用 | `kernel/bpf/syscall.c` |
| BPF 验证器 | `kernel/bpf/verifier.c` |
| BPF 核心（解释器、JIT选择） | `kernel/bpf/core.c` |
| BPF 执行框架 | `include/linux/filter.h` |
| BPF Trampoline | `kernel/bpf/trampoline.c` |
| x86 JIT 编译器 | `arch/x86/net/bpf_jit_comp.c` |
| Kprobe/Tracepoint BPF | `kernel/trace/bpf_trace.c` |
| Kprobe 回调 | `kernel/trace/trace_kprobe.c` |
| Perf 事件 BPF 绑定 | `kernel/events/core.c` |
| XDP 挂载 | `net/core/dev.c` |
| BTF UAPI 定义 | `include/uapi/linux/btf.h` |
| BTF 内核解析 | `kernel/bpf/btf.c` |
| BTF sysfs 导出 | `kernel/bpf/sysfs_btf.c` |
| BTF 嵌入（链接器脚本） | `include/asm-generic/vmlinux.lds.h` |
| BTF 生成脚本 | `scripts/link-vmlinux.sh` |
| CO-RE 重定位核心 | `tools/lib/bpf/relo_core.c`, `tools/lib/bpf/relo_core.h` |
| CO-RE 用户空间宏 | `tools/lib/bpf/bpf_core_read.h` |
| libbpf BTF 加载 | `tools/lib/bpf/btf.c` |
| BPF 程序/类型定义 | `include/uapi/linux/bpf.h` |
| BPF 内核数据结构 | `include/linux/bpf.h` |
| BPF 类型注册表 | `include/linux/bpf_types.h` |
