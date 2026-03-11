+++
date = '2026-03-11'
title = 'eBPF 实现原理分析'
tags = [
    "ebpf",
]
categories = [
    "Linux",
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
- [三、BTF（BPF Type Format）](#三btfbpf-type-format)
  - [3.1 问题背景：传统 BPF 的可移植性困境](#31-问题背景传统-bpf-的可移植性困境)
  - [3.2 BTF 数据格式](#32-btf-数据格式)
  - [3.3 BTF 的生成与嵌入](#33-btf-的生成与嵌入)
  - [3.4 BTF 在内核中的解析与使用](#34-btf-在内核中的解析与使用)
- [四、CO-RE（Compile Once - Run Everywhere）](#四corecompile-once---run-everywhere)
  - [4.1 CO-RE 核心思想](#41-core-核心思想)
  - [4.2 重定位记录格式](#42-重定位记录格式)
  - [4.3 libbpf 执行 CO-RE 重定位](#43-libbpf-执行-co-re-重定位)
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

## 三、BTF（BPF Type Format）

### 3.1 问题背景：传统 BPF 的可移植性困境

传统的 BPF tracing 程序（如 BCC 工具）需要访问内核数据结构（如 `task_struct`、`sk_buff`），但这些结构体在不同内核版本间会变化——字段可能被添加、删除、重排或改变大小。传统做法是：

1. 目标机器上安装完整的**内核头文件**（数百MB）
2. 安装 **Clang/LLVM** 编译工具链
3. 在目标机器上**现场编译** BPF 程序，让 Clang 根据当前内核头文件生成正确的字段偏移量

这意味着每台机器都要安装庞大的编译依赖，而且编译过程耗时且脆弱。

BTF 和 CO-RE 技术的引入就是为了解决这一问题。

### 3.2 BTF 数据格式

BTF 是一种紧凑的类型描述格式，定义在 `include/uapi/linux/btf.h`。

**BTF 文件头** 描述了类型段和字符串段的位置：

```c
// include/uapi/linux/btf.h
#define BTF_MAGIC   0xeB9F
#define BTF_VERSION 1

struct btf_header {
    __u16   magic;
    __u8    version;
    __u8    flags;
    __u32   hdr_len;
    __u32   type_off;   // 类型段偏移
    __u32   type_len;   // 类型段长度
    __u32   str_off;    // 字符串段偏移
    __u32   str_len;    // 字符串段长度
};
```

**每个类型** 用 `btf_type` 描述，包含类型名称、种类和大小信息：

```c
// include/uapi/linux/btf.h
struct btf_type {
    __u32 name_off;     // 类型名在字符串段中的偏移
    __u32 info;         // bits 24-27: kind（类型种类）
                        // bits 0-15: vlen（成员数量等）
                        // bit 31: kind_flag
    union {
        __u32 size;     // INT/ENUM/STRUCT/UNION/DATASEC 使用
        __u32 type;     // PTR/TYPEDEF/CONST/VOLATILE 等使用
    };
};
```

BTF 支持完整的 C 类型系统：

| Kind | 编号 | 说明 | 附加数据 |
|------|------|------|----------|
| `BTF_KIND_INT` | 1 | 整数类型 | 编码/偏移/位宽 |
| `BTF_KIND_PTR` | 2 | 指针 | 引用的类型 |
| `BTF_KIND_ARRAY` | 3 | 数组 | `struct btf_array` |
| `BTF_KIND_STRUCT` | 4 | 结构体 | `struct btf_member[]` |
| `BTF_KIND_UNION` | 5 | 联合体 | `struct btf_member[]` |
| `BTF_KIND_ENUM` | 6 | 枚举 | `struct btf_enum[]` |
| `BTF_KIND_FWD` | 7 | 前向声明 | - |
| `BTF_KIND_TYPEDEF` | 8 | 类型别名 | 引用的类型 |
| `BTF_KIND_VOLATILE` | 9 | volatile 修饰 | 引用的类型 |
| `BTF_KIND_CONST` | 10 | const 修饰 | 引用的类型 |
| `BTF_KIND_RESTRICT` | 11 | restrict 修饰 | 引用的类型 |
| `BTF_KIND_FUNC` | 12 | 函数 | 引用 FUNC_PROTO |
| `BTF_KIND_FUNC_PROTO` | 13 | 函数原型 | `struct btf_param[]` |
| `BTF_KIND_VAR` | 14 | 变量 | `struct btf_var` |
| `BTF_KIND_DATASEC` | 15 | 数据段 | `struct btf_var_secinfo[]` |
| `BTF_KIND_FLOAT` | 16 | 浮点类型 | - |

对于结构体，每个成员用 `btf_member` 描述，包含成员名称、类型和**精确的位偏移量**：

```c
// include/uapi/linux/btf.h
struct btf_member {
    __u32   name_off;   // 成员名在字符串段中的偏移
    __u32   type;       // 成员的类型 ID
    __u32   offset;     // 成员的位偏移量（bit offset）
};
```

### 3.3 BTF 的生成与嵌入

BTF 的生成流程是：**DWARF 调试信息 → pahole 工具转换 → 去重的 BTF → 嵌入 vmlinux 内核镜像**

**前提条件**：内核配置启用 `CONFIG_DEBUG_INFO_BTF`（`lib/Kconfig.debug`）：

```
config DEBUG_INFO_BTF
    bool "Generate BTF typeinfo"
    depends on !DEBUG_INFO_SPLIT && !DEBUG_INFO_REDUCED
    help
      Generate deduplicated BTF type information from DWARF debug info.
      Turning this on expects presence of pahole tool, which will convert
      DWARF type info into equivalent deduplicated BTF type info.
```

**第一步：pahole 转换 DWARF 为 BTF**

在 `scripts/link-vmlinux.sh` 的 `gen_btf()` 函数中执行：

```bash
# scripts/link-vmlinux.sh
info "BTF" ${2}
LLVM_OBJCOPY="${OBJCOPY}" ${PAHOLE} -J ${PAHOLE_FLAGS} ${1}

# 提取 .BTF section，设置 SHF_ALLOC 使其成为运行时镜像的一部分
${OBJCOPY} --only-section=.BTF --set-section-flags .BTF=alloc,readonly \
    --strip-all ${1} ${2} 2>/dev/null
```

`pahole -J` 读取 vmlinux 中的 DWARF 调试信息，生成**去重后**的 BTF 类型信息并写入 `.BTF` section。去重后的 BTF 通常只有 **1-5 MB**，而原始 DWARF 可达上百 MB。

**第二步：链接器嵌入 BTF 到 vmlinux**

链接器脚本（`include/asm-generic/vmlinux.lds.h`）将 `.BTF` section 嵌入内核镜像：

```c
// include/asm-generic/vmlinux.lds.h
#ifdef CONFIG_DEBUG_INFO_BTF
#define BTF                             \
    .BTF : AT(ADDR(.BTF) - LOAD_OFFSET) {      \
        __start_BTF = .;                \
        KEEP(*(.BTF))                   \
        __stop_BTF = .;                 \
    }                                   \
    .BTF_ids : AT(ADDR(.BTF_ids) - LOAD_OFFSET) {  \
        *(.BTF_ids)                     \
    }
#endif
```

`__start_BTF` 和 `__stop_BTF` 标记了 BTF 数据在内核镜像中的起止位置。

**第三步：通过 sysfs 暴露给用户空间**

`kernel/bpf/sysfs_btf.c` 将 BTF 数据以 `/sys/kernel/btf/vmlinux` 文件形式暴露：

```c
// kernel/bpf/sysfs_btf.c
extern char __weak __start_BTF[];
extern char __weak __stop_BTF[];

static int __init btf_vmlinux_init(void)
{
    bin_attr_btf_vmlinux.size = __stop_BTF - __start_BTF;

    if (!__start_BTF || bin_attr_btf_vmlinux.size == 0)
        return 0;

    btf_kobj = kobject_create_and_add("btf", kernel_kobj);
    if (!btf_kobj)
        return -ENOMEM;

    return sysfs_create_bin_file(btf_kobj, &bin_attr_btf_vmlinux);
}

subsys_initcall(btf_vmlinux_init);
```

用户空间的 libbpf 可以通过读取 `/sys/kernel/btf/vmlinux` 获取当前运行内核的完整类型信息，无需安装内核头文件。

### 3.4 BTF 在内核中的解析与使用

内核自身也需要解析 BTF 来支持 BPF 程序的验证。`btf_parse_vmlinux()`（`kernel/bpf/btf.c`）负责解析嵌入的 BTF：

```c
// kernel/bpf/btf.c
struct btf *btf_parse_vmlinux(void)
{
    struct btf_verifier_env *env = NULL;
    struct btf *btf = NULL;
    // ...
    btf->data = __start_BTF;
    btf->data_size = __stop_BTF - __start_BTF;
    btf->kernel_btf = true;
    snprintf(btf->name, sizeof(btf->name), "vmlinux");

    err = btf_parse_hdr(env);       // 解析 BTF 头
    btf->nohdr_data = btf->data + btf->hdr.hdr_len;
    err = btf_parse_str_sec(env);   // 解析字符串段
    err = btf_check_all_metas(env); // 验证所有类型元数据
    // ...
}
```

内核中的 BTF 数据结构（`kernel/bpf/btf.c`）：

```c
// kernel/bpf/btf.c
struct btf {
    void *data;                 // 原始 BTF 数据
    struct btf_type **types;    // 类型数组（按 type_id 索引）
    u32 *resolved_ids;          // 解析后的类型 ID
    u32 *resolved_sizes;        // 解析后的类型大小
    const char *strings;        // 字符串段
    struct btf_header hdr;      // BTF 头
    u32 nr_types;               // 类型总数
    refcount_t refcnt;
    u32 id;                     // BTF 对象的全局 ID
    struct btf *base_btf;       // split BTF 的基础 BTF
    bool kernel_btf;            // 是否为内核 BTF
};
```

BTF 在验证器中的关键用途：

1. **`PTR_TO_BTF_ID` 类型跟踪**：验证器跟踪指向内核对象的指针类型，确保只在合法偏移处访问合法字段
2. **`check_ptr_to_btf_access()`**：验证通过 BTF 类型指针的结构体字段访问
3. **`btf_struct_access()`**：根据 BTF 类型信息验证结构体成员访问的合法性
4. **`check_attach_btf_id()`**：验证 fentry/fexit 程序的目标函数 BTF 类型
5. **`check_pseudo_btf_id()`**：解析 `BPF_PSEUDO_BTF_ID`，将 BTF 变量 ID 转换为内核符号地址

---

## 四、CO-RE（Compile Once - Run Everywhere）

### 4.1 CO-RE 核心思想

CO-RE 的核心思想是：**不硬编码字段偏移量，而是记录"要访问哪个结构体的哪个字段"这个语义意图，在 BPF 程序加载时再根据目标内核的 BTF 解析出实际偏移量。**

传统方式（硬编码偏移）：

```c
// 编译时根据头文件确定 pid 在 task_struct 中的偏移量是 X
int pid;
bpf_probe_read(&pid, sizeof(pid), (void *)task + X);
// 如果目标内核中 pid 的偏移量变了，程序就会读错数据
```

CO-RE 方式（记录访问意图）：

```c
int pid;
bpf_core_read(&pid, sizeof(pid), &task->pid);
// Clang 不硬编码偏移量，而是记录："需要 task_struct 的 pid 字段的偏移"
// 加载时 libbpf 查询目标内核 BTF，计算实际偏移并修补指令
```

### 4.2 重定位记录格式

CO-RE 重定位记录定义在 `tools/lib/bpf/relo_core.h`：

```c
// tools/lib/bpf/relo_core.h
struct bpf_core_relo {
    __u32   insn_off;       // 需要修补的 BPF 指令偏移（字节）
    __u32   type_id;        // 根类型的 BTF type ID
    __u32   access_str_off; // 访问路径字符串在 .BTF 字符串段中的偏移
    enum bpf_core_relo_kind kind;  // 重定位种类
};
```

每条重定位记录表达的含义是：**在 `insn_off` 处的 BPF 指令需要根据 `type_id` 类型的 `access_str_off` 访问路径，提取 `kind` 指定的信息来修补指令操作数。**

访问路径用冒号分隔的索引序列编码，源码中有详细的示例：

```c
// tools/lib/bpf/relo_core.h（注释）
//   struct sample {
//       int a;
//       struct {
//           int b[10];
//       };
//   };
//
//   struct sample *s = ...;
//   int x = &s->a;     // encoded as "0:0" (a 是第 0 个字段)
//   int y = &s->b[5];  // encoded as "0:1:0:5"
//                       //   (匿名 struct 是第 1 个字段，
//                       //    b 是匿名 struct 内的第 0 个字段，
//                       //    访问第 5 个元素)
//   int z = &s[10]->b; // encoded as "10:1" (指针当数组用)
```

重定位种类涵盖了字段和类型操作的所有维度：

```c
// tools/lib/bpf/relo_core.h
enum bpf_core_relo_kind {
    BPF_FIELD_BYTE_OFFSET = 0,  // 字段字节偏移
    BPF_FIELD_BYTE_SIZE = 1,    // 字段字节大小
    BPF_FIELD_EXISTS = 2,       // 字段是否存在于目标内核
    BPF_FIELD_SIGNED = 3,       // 字段是否有符号
    BPF_FIELD_LSHIFT_U64 = 4,   // 位域的左移量
    BPF_FIELD_RSHIFT_U64 = 5,   // 位域的右移量
    BPF_TYPE_ID_LOCAL = 6,      // 类型在本地 BPF 对象中的 ID
    BPF_TYPE_ID_TARGET = 7,     // 类型在目标内核中的 ID
    BPF_TYPE_EXISTS = 8,        // 类型是否存在于目标内核
    BPF_TYPE_SIZE = 9,          // 类型在目标内核中的大小
    BPF_ENUMVAL_EXISTS = 10,    // 枚举值是否存在
    BPF_ENUMVAL_VALUE = 11,     // 枚举值的整数值
};
```

Clang 通过一系列 `__builtin_preserve_*` 内建函数在编译时生成这些重定位记录，对应的用户空间宏定义在 `tools/lib/bpf/bpf_core_read.h`：

```c
// tools/lib/bpf/bpf_core_read.h
#define __CORE_RELO(src, field, info) \
    __builtin_preserve_field_info((src)->field, BPF_FIELD_##info)
```

| CO-RE 宏/函数 | Clang 内建函数 | 用途 |
|---------------|---------------|------|
| `BPF_CORE_READ(s, field)` | `__builtin_preserve_access_index()` | 可重定位的字段读取 |
| `bpf_core_field_exists(field)` | `__builtin_preserve_field_info(f, BPF_FIELD_EXISTS)` | 检查字段是否存在 |
| `bpf_core_field_size(field)` | `__builtin_preserve_field_info(f, BPF_FIELD_BYTE_SIZE)` | 获取字段大小 |
| `bpf_core_type_exists(type)` | `__builtin_preserve_type_info(t, BPF_TYPE_EXISTS)` | 检查类型是否存在 |
| `bpf_core_type_size(type)` | `__builtin_preserve_type_info(t, BPF_TYPE_SIZE)` | 获取类型大小 |
| `bpf_core_enum_value_exists(v)` | `__builtin_preserve_enum_value(v, BPF_ENUMVAL_EXISTS)` | 检查枚举值是否存在 |

### 4.3 libbpf 执行 CO-RE 重定位

**CO-RE 重定位在用户空间由 libbpf 执行，内核不参与重定位过程。** 内核收到的是已经完成重定位的 BPF 程序。

libbpf 的 CO-RE 重定位流程（`tools/lib/bpf/`）：

```
bpf_object__load()
  └── bpf_object__relocate_core()          // libbpf.c
        对 .BTF.ext 中的每条 CO-RE 记录:
        ├── bpf_core_apply_relo()
        │     ├── bpf_core_find_cands()    // 在目标 BTF 中查找匹配类型
        │     └── bpf_core_apply_relo_insn()  // relo_core.c
        │           // 根据目标类型信息修补 BPF 指令的 insn->imm
        └── BPF_PROG_LOAD                 // 将已重定位的程序提交给内核
```

libbpf 加载目标内核 BTF 的方式（`tools/lib/bpf/btf.c`）：

```c
// tools/lib/bpf/btf.c
struct btf *btf__load_vmlinux_btf(void)
{
    struct {
        const char *path_fmt;
        bool raw_btf;
    } locations[] = {
        // 优先从 sysfs 读取原始 BTF
        { "/sys/kernel/btf/vmlinux", true },
        // 回退到磁盘上的 vmlinux ELF 文件
        { "/boot/vmlinux-%1$s" },
        { "/lib/modules/%1$s/vmlinux-%1$s" },
        { "/lib/modules/%1$s/build/vmlinux" },
        { "/usr/lib/modules/%1$s/kernel/vmlinux" },
        { "/usr/lib/debug/boot/vmlinux-%1$s" },
        { "/usr/lib/debug/boot/vmlinux-%1$s.debug" },
        { "/usr/lib/debug/lib/modules/%1$s/vmlinux" },
    };

    uname(&buf);
    for (i = 0; i < ARRAY_SIZE(locations); i++) {
        snprintf(path, PATH_MAX, locations[i].path_fmt, buf.release);
        if (access(path, R_OK))
            continue;
        if (locations[i].raw_btf)
            btf = btf__parse_raw(path);   // 直接解析原始 BTF
        else
            btf = btf__parse_elf(path, NULL);  // 从 ELF 中提取 BTF
        if (!err)
            return btf;
    }
    return libbpf_err_ptr(-ESRCH);
}
```

**重定位示例**：假设 BPF 程序要读取 `task_struct->pid`，编译时 `pid` 的偏移是 1224（本地内核），但目标内核中 `pid` 偏移变成了 1240。

1. Clang 编译时生成一条重定位记录：`{insn_off=42, type_id=123(task_struct), access_str="0:15"(pid字段), kind=BPF_FIELD_BYTE_OFFSET}`
2. BPF 指令中 `insn[42].imm = 1224`（本地偏移量，作为默认值）
3. libbpf 加载时读取目标内核的 `/sys/kernel/btf/vmlinux`
4. 在目标 BTF 中找到 `task_struct`，查到 `pid` 字段的偏移是 1240
5. 修补 `insn[42].imm = 1240`
6. 将修补后的程序通过 `BPF_PROG_LOAD` 提交给内核

---

## 五、整体架构总结

### 完整工作流

```
   开发机 (编译一次)                           目标机 (直接运行)
 ┌─────────────────────┐               ┌──────────────────────────────────┐
 │                     │               │           Linux Kernel            │
 │  BPF C 源码         │               │  ┌────────────────────────────┐  │
 │    + CO-RE 宏       │               │  │ 嵌入的 vmlinux BTF        │  │
 │         │           │               │  │ (__start_BTF ~ __stop_BTF) │  │
 │    Clang/LLVM       │               │  └──────────┬─────────────────┘  │
 │         │           │               │             │                    │
 │         ▼           │   ──────→     │             ▼ /sys/kernel/btf/   │
 │  BPF ELF 文件       │   分发到      │               vmlinux            │
 │  ├─ .text (字节码)  │   目标机      │  ┌────────────────────────────┐  │
 │  ├─ .BTF (本地类型) │               │  │      libbpf               │  │
 │  └─ .BTF.ext        │               │  │  1. 读取目标内核 BTF       │  │
 │    (CO-RE重定位记录) │               │  │  2. 对比本地/目标 BTF      │  │
 │                     │               │  │  3. 修补 BPF 指令偏移量    │  │
 └─────────────────────┘               │  │  4. BPF_PROG_LOAD         │  │
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
