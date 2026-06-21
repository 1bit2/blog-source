+++
date = '2026-04-16'
title = 'eBPF 开发指导手册（基于 Linux 5.15.78 内核源码）'
weight = 5
tags = [
    "eBPF",
    "开发",
    "SEC",
    "kprobe",
    "tracepoint",
]
categories = [
    "其他",
]
+++
# eBPF 开发指导手册（基于 Linux 5.15.78 内核源码）

本文档是一份面向实际开发的 eBPF 程序编写指南，按照**开发决策顺序**组织：先选挂载方式，再写处理函数，然后用 Map/Helper 完成业务逻辑，最后加载运行。所有内容均基于内核源码树中的实际实现。

建议先阅读 [eBPF 实现原理分析](eBPF实现原理分析.md) 了解底层机制，再按本手册开发。

---

## 目录

- [一、一个 eBPF 程序的完整组成](#一一个-ebpf-程序的完整组成)
- [二、第一步：选择挂载方式](#二第一步选择挂载方式)
  - [2.1 三种主要挂载方式的选择决策](#21-三种主要挂载方式的选择决策)
  - [2.2 SEC 名称速查表](#22-sec-名称速查表)
  - [2.3 SEC() 宏的本质](#23-sec-宏的本质)
- [三、第二步：编写处理函数——获取参数](#三第二步编写处理函数获取参数)
  - [3.1 kprobe 方式：从 CPU 寄存器获取参数](#31-kprobe-方式从-cpu-寄存器获取参数)
  - [3.2 fentry/fexit 方式：通过 BTF 直接获取参数](#32-fentryfexit-方式通过-btf-直接获取参数)
  - [3.3 tracepoint 方式：使用 trace event 结构体](#33-tracepoint-方式使用-trace-event-结构体)
  - [3.4 三种方式对比总结](#34-三种方式对比总结)
- [四、第三步：使用 Map、Helper 和内核数据读取](#四第三步使用-maphelper-和内核数据读取)
  - [4.1 定义 BPF Map](#41-定义-bpf-map)
  - [4.2 BTF Map 的额外能力](#42-btf-map-的额外能力)
  - [4.3 常用 Helper 函数速查](#43-常用-helper-函数速查)
  - [4.4 内核数据读取——三种方式](#44-内核数据读取三种方式)
- [五、第四步：用户空间加载与挂载](#五第四步用户空间加载与挂载)
- [六、第五步：排查验证器错误](#六第五步排查验证器错误)
- [七、完整开发示例](#七完整开发示例)
- [八、开发检查清单](#八开发检查清单)
- [附录 A：x86-64 pt_regs 与寄存器映射详解](#附录-ax86-64-pt_regs-与寄存器映射详解)
- [附录 B：Helper 函数内核实现详解](#附录-bhelper-函数内核实现详解)
- [附录 C：关键源码文件索引](#附录-c关键源码文件索引)

---

## 一、一个 eBPF 程序的完整组成

```c
#include "vmlinux.h"                          // ① 头文件
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

struct {                                       // ② Map 定义（可选）
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u32);
    __type(value, __u64);
} my_map SEC(".maps");

SEC("kprobe/tcp_v4_connect")                   // ③ SEC 名称 → 决定程序类型和挂载目标
int BPF_KPROBE(tcp_connect, struct sock *sk)   // ④ 处理函数 → 宏自动提取参数
{
    __u32 pid = bpf_get_current_pid_tgid() >> 32;  // ⑤ Helper 函数
    __u32 daddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);  // ⑥ 内核数据读取
    bpf_map_update_elem(&my_map, &pid, &daddr, BPF_ANY);     // ⑦ Map 操作
    return 0;
}

char _license[] SEC("license") = "GPL";        // ⑧ License（必须）
```

开发流程对应到后续章节：

```
                你要监控什么？
                    │
        ┌───────────┼───────────┐
        ▼           ▼           ▼
    任意内核函数   稳定跟踪点    高性能追踪
        │           │           │
   ┌────┴────┐      │      ┌────┴────┐
   │ kprobe  │   tracepoint │ fentry  │   ← 第二章：选择挂载方式
   └────┬────┘      │      └────┬────┘
        │           │           │
        ▼           ▼           ▼
    BPF_KPROBE   手动结构体    BPF_PROG      ← 第三章：编写处理函数
    pt_regs提取  或vmlinux.h   ctx[]数组
        │           │           │
        └───────────┼───────────┘
                    ▼
            Map + Helper + 数据读取          ← 第四章：业务逻辑
                    │
                    ▼
            libbpf open → load → attach      ← 第五章：加载运行
                    │
                    ▼
             验证器报错？排查修复            ← 第六章：排错
```

---

## 二、第一步：选择挂载方式

**这是开发 eBPF 程序的第一个也是最关键的决策。** 挂载方式决定了程序类型（SEC 名称）、参数获取方式、可用 Helper 函数集合。

### 2.1 三种主要挂载方式的选择决策

| | kprobe / kretprobe | fentry / fexit | tracepoint |
|---|---|---|---|
| **适用目标** | **任意**内核函数 | BTF 中有记录的内核函数 | 内核预定义的静态跟踪点 |
| **稳定性** | 低——函数可能被重命名/内联 | 中——依赖 BTF | 高——属于内核 ABI |
| **性能开销** | 较高（int3 断点异常） | 低（trampoline 直接跳转） | 中 |
| **参数获取** | 从 `pt_regs` 寄存器提取 | 通过 `ctx[]` 数组直接获取 | 通过 trace event 结构体 |
| **读取内核内存** | 必须用 `bpf_probe_read_kernel` | **可直接读取** | 字段直接可用 |
| **内核版本要求** | 所有版本 | 5.5+，需 BTF | 所有版本 |
| **SEC 前缀** | `kprobe/` / `kretprobe/` | `fentry/` / `fexit/` | `tracepoint/` / `tp_btf/` |
| **推荐宏** | `BPF_KPROBE` / `BPF_KRETPROBE` | `BPF_PROG` | `BPF_PROG`（tp_btf） |

**选择建议**：
- 目标函数在 BTF 中且内核 >= 5.5 → **首选 fentry/fexit**（性能最好，可直接读内存）
- 需要跟踪稳定的系统事件（调度、网络、文件系统） → **用 tracepoint**
- 需要跟踪任意函数或旧内核 → **用 kprobe**

### 2.2 SEC 名称速查表

| SEC 名称 | 程序类型 | 上下文参数 | 用途 |
|----------|---------|-----------|------|
| `kprobe/func` | `KPROBE` | `struct pt_regs *` | 函数入口探针 |
| `kretprobe/func` | `KPROBE` | `struct pt_regs *` | 函数返回探针 |
| `fentry/func` | `TRACING` | `u64 *ctx` | trampoline 函数入口 |
| `fexit/func` | `TRACING` | `u64 *ctx` | trampoline 函数返回 |
| `tracepoint/cat/name` | `TRACEPOINT` | trace event 结构体 | 静态跟踪点 |
| `tp_btf/name` | `TRACING` | `u64 *ctx` | BTF 感知跟踪点 |
| `xdp` | `XDP` | `struct xdp_md *` | 高速数据包处理 |
| `socket` | `SOCKET_FILTER` | `struct __sk_buff *` | 套接字过滤 |
| `tc` / `classifier` | `SCHED_CLS` | `struct __sk_buff *` | 流量控制 |

> **注意**：SEC 名称写错不会有编译错误，但 libbpf 加载时会报 `"failed to find program type"`。
>
> **注意**：`char _license[] SEC("license") = "GPL";` 是必须的。`bpf_probe_read_kernel`、`bpf_perf_event_output` 等常用 helper 都是 GPL-only。

### 2.3 SEC() 宏的本质

`SEC()` 将函数放入 ELF 的指定 section。libbpf 加载时通过 `section_defs[]` 表（`tools/lib/bpf/libbpf.c:7943`）将 section 名匹配到 `bpf_prog_type`，同时确定挂载函数（`attach_fn`）。

```c
// tools/lib/bpf/libbpf.c:7943（摘录）
static const struct bpf_sec_def section_defs[] = {
    SEC_DEF("kprobe/",     KPROBE,     .attach_fn = attach_kprobe),
    SEC_DEF("kretprobe/",  KPROBE,     .attach_fn = attach_kprobe),
    SEC_DEF("fentry/",     TRACING,    .expected_attach_type = BPF_TRACE_FENTRY,
                                       .attach_fn = attach_trace),
    SEC_DEF("fexit/",      TRACING,    .expected_attach_type = BPF_TRACE_FEXIT,
                                       .attach_fn = attach_trace),
    SEC_DEF("tracepoint/", TRACEPOINT, .attach_fn = attach_tp),
    // ...
};
```

---

## 三、第二步：编写处理函数——获取参数

选好挂载方式后，下一步是编写 BPF 处理函数。**不同挂载方式的参数获取机制完全不同**，这是最容易出错的地方。

### 3.1 kprobe 方式：从 CPU 寄存器获取参数

**原理**：kprobe 触发时，内核将 CPU 寄存器快照保存为 `struct pt_regs`，作为 BPF 程序的上下文。函数参数按 C 调用约定存放在寄存器中（x86-64：RDI/RSI/RDX/RCX/R8/R9），通过 `PT_REGS_PARM*` 宏提取。

> 关于 pt_regs 结构体和寄存器映射的详细说明见 [附录 A](#附录-ax86-64-pt_regs-与寄存器映射详解)。

#### 推荐写法：BPF_KPROBE 宏

`BPF_KPROBE`（`tools/lib/bpf/bpf_tracing.h:421`）自动从寄存器按顺序提取参数：

```c
// tcp_v4_connect 内核签名：int tcp_v4_connect(struct sock *sk, struct sockaddr *uaddr, int addr_len)

SEC("kprobe/tcp_v4_connect")
int BPF_KPROBE(tcp_v4_connect, struct sock *sk, struct sockaddr *uaddr, int addr_len)
{
    // sk, uaddr, addr_len 已经由宏自动从寄存器提取好了，直接使用即可

    // 注意：sk 是内核指针，不能直接解引用，必须用 probe_read 或 CO-RE 读取
    __u32 daddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);
    return 0;
}
```

#### BPF_KPROBE 参数列表的含义

`BPF_KPROBE(tcp_v4_connect, struct sock *sk, struct sockaddr *uaddr, int addr_len)` 中的参数列表**不是给内核看的，是给宏用的**。宏会根据参数列表自动生成寄存器提取代码：

```
你写的参数列表                 宏自动生成的提取代码
──────────────                ──────────────────
struct sock *sk           ←   (void *)PT_REGS_PARM1(ctx)  即 ctx->rdi
struct sockaddr *uaddr    ←   (void *)PT_REGS_PARM2(ctx)  即 ctx->rsi
int addr_len              ←   (void *)PT_REGS_PARM3(ctx)  即 ctx->rdx
```

宏展开后实际生成了两层函数：

```c
// 外层：内核实际调用的 BPF 入口，参数是原始的 pt_regs *
int tcp_v4_connect(struct pt_regs *ctx) {
    return ____tcp_v4_connect(ctx,
        (void *)PT_REGS_PARM1(ctx),   // → sk
        (void *)PT_REGS_PARM2(ctx),   // → uaddr
        (void *)PT_REGS_PARM3(ctx));  // → addr_len
}

// 内层：你写代码的地方，参数已经提取好，ctx 也保留可用
static __always_inline int
____tcp_v4_connect(struct pt_regs *ctx, struct sock *sk,
                   struct sockaddr *uaddr, int addr_len) {
    // 你的代码写在这里
    // sk, uaddr, addr_len 可直接使用
    // ctx 也可直接使用
}
```

参数列表的三个作用：

| 作用 | 说明 |
|------|------|
| **告诉宏提取几个参数** | 写 3 个参数 → 宏从 PARM1~PARM3 提取 |
| **提供类型信息** | `struct sock *sk` 让编译器知道类型，后续可以写 `BPF_CORE_READ(sk, ...)` |
| **提供变量名** | 函数体里直接用 `sk` 而不是 `(struct sock *)PT_REGS_PARM1(ctx)` |

等价的不用宏的手动写法（来自 `samples/bpf/tracex3_kern.c:24`）：

```c
SEC("kprobe/blk_mq_start_request")
int bpf_prog1(struct pt_regs *ctx)
{
    long rq = PT_REGS_PARM1(ctx);  // 手动从 RDI 寄存器取第 1 个参数
    // ...
}
```

> **注意**：`BPF_KPROBE` 参数**必须与内核函数签名的顺序和个数匹配**——宏按位置提取（第 1 个参数 → PARM1，第 2 个 → PARM2），写错顺序就会取错寄存器。最多支持 5 个参数（PARM1~PARM5）。
>
> **注意**：虽然参数指针自动提取了，但**拿到的只是指针值本身（一个地址数字）**，指针指向的内核内存**不能直接解引用**，必须用 `bpf_probe_read_kernel` 或 `BPF_CORE_READ` 读取。

#### kretprobe：获取返回值

`BPF_KRETPROBE` 从 RAX 寄存器提取返回值：

```c
SEC("kretprobe/tcp_v4_connect")
int BPF_KRETPROBE(tcp_v4_connect_ret, int ret)
{
    // ret 自动从 PT_REGS_RC(ctx) 提取
    if (ret != 0) return 0;  // 连接失败
    // ...
}
```

> **注意**：kretprobe 触发时**入口参数已丢失**（寄存器被覆盖）。如需同时获取入参和返回值，在 kprobe 中将入参存入 Map，kretprobe 中查 Map：

```c
// kprobe: 保存 sk 到 map（key = pid_tgid）
SEC("kprobe/tcp_v4_connect")
int BPF_KPROBE(tcp_v4_connect, struct sock *sk) {
    u64 pid_tgid = bpf_get_current_pid_tgid();
    bpf_map_update_elem(&sock_map, &pid_tgid, &sk, BPF_ANY);
    return 0;
}

// kretprobe: 从 map 取回 sk，同时获取返回值
SEC("kretprobe/tcp_v4_connect")
int BPF_KRETPROBE(tcp_v4_connect_ret, int ret) {
    u64 pid_tgid = bpf_get_current_pid_tgid();
    struct sock **skp = bpf_map_lookup_elem(&sock_map, &pid_tgid);
    if (!skp) return 0;
    struct sock *sk = *skp;
    bpf_map_delete_elem(&sock_map, &pid_tgid);
    // 现在同时拥有 sk（入参）和 ret（返回值）
    return 0;
}
```

#### x86-64 syscall 包装器的特殊处理

x86-64 内核的系统调用经过包装器（`arch/x86/include/asm/syscall_wrapper.h:14`），**`__x64_sys_xxx` 的 C 签名只有一个参数** `const struct pt_regs *regs`。如果 kprobe 挂在 `__x64_sys_connect` 上，需要两级解引用：

```c
// samples/bpf/test_probe_write_user_kern.c:31
SEC("kprobe/__x64_sys_connect")
int bpf_prog1(struct pt_regs *ctx) {
    struct pt_regs *real_regs = (struct pt_regs *)PT_REGS_PARM1_CORE(ctx);
    void *sockaddr_arg = (void *)PT_REGS_PARM2_CORE(real_regs);
    // ...
}
```

**更简单的方法**：挂在不带包装器的内部函数上（如 `__sys_connect`），参数正常传递。

---

### 3.2 fentry/fexit 方式：通过 BTF 直接获取参数

**原理**：fentry/fexit 使用 BPF Trampoline，参数以 `u64 *ctx` 数组形式传入。BTF 提供类型信息，验证器能跟踪指针类型。

`BPF_PROG`（`tools/lib/bpf/bpf_tracing.h:381`）自动从 `ctx[]` 数组提取参数：

```c
SEC("fentry/tcp_v4_connect")
int BPF_PROG(tcp_v4_connect, struct sock *sk, struct sockaddr *uaddr, int addr_len)
{
    // sk = ctx[0], uaddr = ctx[1], addr_len = ctx[2]（宏自动完成）

    // 与 kprobe 的关键区别：可以直接读取内核内存！
    u32 daddr = sk->__sk_common.skc_daddr;  // 直接解引用，无需 probe_read
    return 0;
}
```

> **注意**：需要内核 >= 5.5 且支持 BTF（`/sys/kernel/btf/vmlinux` 存在）。
>
> **注意**：`BPF_PROG` 和 `BPF_KPROBE` **绝不能混用**——上下文类型完全不同。

---

### 3.3 tracepoint 方式：使用 trace event 结构体

**原理**：tracepoint 的上下文是内核预定义的 trace event 数据结构，不是 `pt_regs`。

#### 方式一：vmlinux.h（推荐，CO-RE 兼容）

```c
#include "vmlinux.h"
SEC("tracepoint/syscalls/sys_enter_openat")
int handle_openat(struct trace_event_raw_sys_enter *ctx) {
    char *filename = (char *)BPF_CORE_READ(ctx, args[1]);
    // ...
}
```

命名规则：`tracepoint 名` → `struct trace_event_raw_<name>`

#### 方式二：手动构造结构体

根据 `/sys/kernel/tracing/events/<cat>/<name>/format` 的偏移量定义：

```c
// 来自 tools/testing/selftests/bpf/progs/test_tracepoint.c
struct sched_switch_args {
    unsigned long long pad;  // 前 8 字节不可访问（trace_entry）
    char prev_comm[16];      // offset:8
    int prev_pid;            // offset:24
    // ...
};

SEC("tracepoint/sched/sched_switch")
int oncpu(struct sched_switch_args *ctx) {
    int pid = ctx->next_pid;  // 直接访问字段
    return 0;
}
```

> **注意**：手动结构体的前 8 字节必须用 `pad` 跳过。偏移量必须与 format 文件精确一致。
>
> **注意**：手动结构体**不可跨内核版本移植**。推荐 vmlinux.h 方式。

---

### 3.4 三种方式对比总结

| | kprobe | fentry/fexit | tracepoint |
|---|---|---|---|
| **获取参数的方式** | 从 `pt_regs` 寄存器提取 | 从 `ctx[]` 数组获取 | 从 event 结构体字段读取 |
| **推荐使用的宏** | `BPF_KPROBE` / `BPF_KRETPROBE` | `BPF_PROG` | `BPF_PROG`(tp_btf) 或直接定义 |
| **读取内核内存** | 必须 `bpf_probe_read_kernel` 或 `BPF_CORE_READ` | **直接解引用** | 字段已填充，直接读取 |
| **获取返回值** | kretprobe（入参丢失，需 Map 配合） | fexit（入参和返回值同时可用） | 不适用 |
| **跨架构** | 需要 `PT_REGS_PARM*` 宏适配 | 自动适配 | 自动适配 |
| **ctx 变量** | `struct pt_regs *ctx` | `unsigned long long *ctx` | event 结构体指针 |

---

## 四、第三步：使用 Map、Helper 和内核数据读取

处理函数拿到参数后，通过 **Map 存储/通信数据**、**Helper 函数执行内核操作**、**安全读取内核内存**来完成业务逻辑。

### 4.1 定义 BPF Map

**推荐使用新式 BTF Map 定义**（`SEC(".maps")`，注意有点号）：

```c
// tools/lib/bpf/bpf_helpers.h:13
#define __uint(name, val) int (*name)[val]
#define __type(name, val) typeof(val) *name

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, __u32);
    __type(value, struct event);
} events SEC(".maps");
```

旧式定义（`SEC("maps")`，无点号）仍可用但不推荐：

```c
struct bpf_map_def SEC("maps") my_map = {
    .type = BPF_MAP_TYPE_HASH,
    .key_size = sizeof(__u32),
    .value_size = sizeof(__u64),
    .max_entries = 1024,
};
```

常用 Map 类型：

| Map 类型 | 用途 |
|----------|------|
| `HASH` | 通用 key-value 存储（最常用） |
| `ARRAY` | 固定大小数组（key 是索引） |
| `PERF_EVENT_ARRAY` | 向用户空间发送事件（配合 `bpf_perf_event_output`） |
| `RINGBUF` | 高性能环形缓冲区（替代 perf_event_array 的新方案） |
| `PERCPU_HASH` / `PERCPU_ARRAY` | 每 CPU 独立副本（避免竞争） |
| `LRU_HASH` | 自动淘汰旧元素的哈希表 |

> **注意**：`max_entries` 不能为 0。`SEC(".maps")` 和 `SEC("maps")` 有无点号含义不同。

### 4.2 BTF Map 的额外能力

新式 `SEC(".maps")` 与旧式 `SEC("maps")` 不仅语法不同——新式会将 key/value 的 **BTF 类型 ID** 传给内核（存入 `struct bpf_map` 的 `btf_key_type_id` / `btf_value_type_id` 字段），启用以下能力：

| 能力 | 说明 | 内核实现位置 |
|------|------|-------------|
| **pretty-print** | `bpftool map dump` 输出结构化数据而非原始字节 | `htab_map_seq_show_elem` → `btf_type_seq_show()` |
| **bpf_spin_lock** | 内核通过 BTF 定位 value 中 spin_lock 字段的偏移 | `map_check_btf()` → `btf_find_spin_lock()`（`syscall.c:787`） |
| **bpf_timer** | 同上，定位 timer 字段偏移 | `btf_find_timer()`（`syscall.c:808`） |
| **类型安全验证** | 验证器检查 Map 操作的类型正确性 | `btf_vmlinux_map_ids_init()`（`btf.c:4495`） |

旧式 Map 只传 `key_size`/`value_size` 两个整数，内核不了解数据内部结构，无法支持上述功能。

### 4.3 常用 Helper 函数速查

**通用 Helper**（所有程序类型可用，`kernel/bpf/helpers.c`）：

| Helper | 原型 | 说明 |
|--------|------|------|
| `bpf_map_lookup_elem` | `void *(map, key)` | 查找 Map 元素，**返回值可能为 NULL，必须检查** |
| `bpf_map_update_elem` | `int(map, key, value, flags)` | 更新 Map 元素（flags: `BPF_ANY`/`BPF_NOEXIST`/`BPF_EXIST`） |
| `bpf_map_delete_elem` | `int(map, key)` | 删除 Map 元素 |
| `bpf_get_current_pid_tgid` | `u64(void)` | 返回 `(tgid << 32) \| pid`，`>> 32` 得到用户空间 PID |
| `bpf_get_current_comm` | `int(buf, size)` | 获取当前进程名 |
| `bpf_ktime_get_ns` | `u64(void)` | 当前时间（纳秒） |

**Tracing 专用 Helper**（kprobe/tracepoint/fentry，`kernel/trace/bpf_trace.c`，**GPL-only**）：

| Helper | 原型 | 说明 |
|--------|------|------|
| `bpf_probe_read_kernel` | `int(dst, size, src)` | 安全读取内核内存 |
| `bpf_probe_read_user` | `int(dst, size, src)` | 安全读取用户空间内存 |
| `bpf_perf_event_output` | `int(ctx, map, flags, data, size)` | 向 perf buffer 发送事件数据 |
| `bpf_get_stackid` | `int(ctx, map, flags)` | 获取调用栈 ID |
| `bpf_override_return` | `int(ctx, rc)` | 覆盖函数返回值（仅 kprobe） |

> **注意**：`bpf_map_lookup_elem` 返回 NULL 时**必须做检查**，否则验证器拒绝。
>
> **注意**：`bpf_probe_read_kernel` 和 `bpf_probe_read_user` 不能混用——内核指针用 `_kernel`，用户空间指针用 `_user`。
>
> **注意**：`bpf_perf_event_output`、`bpf_get_stackid` 等 helper 的第一个参数**必须是原始的 `ctx`**（即 BPF 程序入口收到的上下文指针）。内核验证器要求该参数类型为 `ARG_PTR_TO_CTX`，传其他值会被拒绝。使用 `BPF_KPROBE` 宏时，宏自动保留了 `ctx` 变量（类型为 `struct pt_regs *`），在函数体里始终可用：
>
> ```c
> SEC("kprobe/tcp_v4_connect")
> int BPF_KPROBE(tcp_v4_connect, struct sock *sk) {
>     struct event evt = {};
>     // ...
>     bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU, &evt, sizeof(evt));
>     //                    ^^^
>     //                    必须传 ctx，不能传 sk 或其他变量
>     return 0;
> }
> ```

### 4.4 内核数据读取——三种方式

| 方式 | 适用场景 | 跨内核版本 | 示例 |
|------|---------|-----------|------|
| `bpf_probe_read_kernel` | kprobe，固定内核版本 | 否 | `bpf_probe_read_kernel(&val, 4, &skb->len)` |
| `BPF_CORE_READ` | kprobe，需跨版本 | 是（CO-RE） | `val = BPF_CORE_READ(skb, len)` |
| 直接解引用 | **仅 fentry/fexit** | 是（BTF 验证） | `val = skb->len` |

`BPF_CORE_READ`（`tools/lib/bpf/bpf_core_read.h:402`）支持链式读取：

```c
// 等效于 skb->dev->name，自动处理中间指针
char *name = BPF_CORE_READ(skb, dev, name);
```

> **注意**：kprobe 中**不能直接解引用**内核指针（验证器拒绝），fentry 中**可以**（BTF 保证安全）。这是两者最大的使用区别。

---

## 五、第四步：用户空间加载与挂载

BPF 程序由 libbpf 在用户空间完成加载（`tools/lib/bpf/libbpf.c`）：

```
bpf_object__open_file("prog.bpf.o")     // 解析 ELF，提取程序和 Map
        │
        ▼
bpf_object__load(obj)                    // 核心步骤
  ├── bpf_object__load_vmlinux_btf()     // 加载目标内核 BTF（CO-RE 需要）
  ├── bpf_object__create_maps()          // 创建 Map（传递 BTF 类型 ID）
  ├── bpf_object__relocate()             // CO-RE 重定位：修补字段偏移
  └── bpf_object__load_progs()           // bpf() 系统调用：验证 + JIT
        │
        ▼
bpf_program__attach(prog)               // 挂载到内核
  └── section_defs[].attach_fn()
      // kprobe → attach_kprobe()  → perf_event + ioctl
      // fentry → attach_trace()   → BPF_LINK_CREATE
      // tracepoint → attach_tp()  → perf_event + ioctl
```

> **注意**：顺序必须是 open → load → attach，不能跳过 load。
>
> **注意**：`attach` 依赖 SEC 名称匹配 `section_defs[]` 中的 `attach_fn`，匹配不到返回 `-ESRCH`。

---

## 六、第五步：排查验证器错误

验证器（`kernel/bpf/verifier.c`）是 BPF 程序加载失败的最常见原因：

| 错误信息 | 原因 | 解决方法 |
|---------|------|---------|
| `R0 invalid mem access 'map_value_or_null'` | `bpf_map_lookup_elem` 返回值未 NULL 检查 | 添加 `if (!ptr) return 0;` |
| `invalid indirect read from stack` | 栈上变量未初始化 | 声明时加 `= {}` |
| `back-edge from insn X to Y` | 循环无法证明终止 | 用 `#pragma unroll` 或有界循环(5.3+) |
| `program is too large` | 指令数超限 | 拆分子程序，`static __always_inline` |
| `unknown func bpf_xxx` | 程序类型不支持此 helper | 核对 `xxx_func_proto()` |
| `cannot pass map_type X into func` | Map 类型与 helper 不匹配 | 检查 Map 类型 |

> **注意**：BPF 栈大小限制 **512 字节**。大缓冲区用 `BPF_MAP_TYPE_PERCPU_ARRAY`。
>
> **注意**：5.3 之前完全禁止循环。5.3+ 的循环次数必须能静态确定上限。

---

## 七、完整开发示例

### 示例一：kprobe 追踪 TCP 连接（综合运用第二~六章）

```c
// tcp_connect.bpf.c
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

struct event {
    __u32 pid;
    __u32 daddr;
    __u16 dport;
    char comm[16];
};

// 第四章：BTF Map 定义
struct {
    __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
    __uint(key_size, sizeof(int));
    __uint(value_size, sizeof(int));
} events SEC(".maps");

// 第二章：SEC 名称选择 kprobe
// 第三章：BPF_KPROBE 自动提取参数
SEC("kprobe/tcp_v4_connect")
int BPF_KPROBE(tcp_v4_connect, struct sock *sk)
{
    struct event evt = {};  // 第六章：必须初始化

    // 第四章：Helper 函数
    evt.pid = bpf_get_current_pid_tgid() >> 32;  // >> 32 得到用户空间 PID
    bpf_get_current_comm(&evt.comm, sizeof(evt.comm));

    // 第四章：CO-RE 读取内核数据（kprobe 不能直接解引用）
    evt.daddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);
    evt.dport = BPF_CORE_READ(sk, __sk_common.skc_dport);

    // ctx 是原始的 pt_regs *，perf_event_output 要求
    bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU, &evt, sizeof(evt));
    return 0;
}

char _license[] SEC("license") = "GPL";
```

### 示例二：kprobe + kretprobe 配合

```c
// 参考 samples/bpf/tracex4_kern.c
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, long);
    __type(value, __u64);
} alloc_map SEC(".maps");

SEC("kretprobe/kmem_cache_alloc_node")
int BPF_KRETPROBE(trace_alloc, void *ret) {
    if (ret) {
        __u64 ts = bpf_ktime_get_ns();
        bpf_map_update_elem(&alloc_map, &ret, &ts, BPF_ANY);
    }
    return 0;
}

SEC("kprobe/kmem_cache_free")
int BPF_KPROBE(trace_free, void *cachep, void *objp) {
    bpf_map_delete_elem(&alloc_map, &objp);
    return 0;
}

char _license[] SEC("license") = "GPL";
```

### 示例三：tracepoint 追踪进程调度

```c
// 参考 tools/testing/selftests/bpf/progs/test_tracepoint.c
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

struct sched_switch_args {
    unsigned long long pad;  // 前 8 字节不可访问
    char prev_comm[16];
    int prev_pid;
    int prev_prio;
    long long prev_state;
    char next_comm[16];
    int next_pid;
    int next_prio;
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, int);
    __type(value, __u64);
} start SEC(".maps");

SEC("tracepoint/sched/sched_switch")
int handle_switch(struct sched_switch_args *ctx) {
    int pid = ctx->next_pid;  // tracepoint 直接读字段
    __u64 ts = bpf_ktime_get_ns();
    bpf_map_update_elem(&start, &pid, &ts, BPF_ANY);
    return 0;
}

char _license[] SEC("license") = "GPL";
```

---

## 八、开发检查清单

| 检查项 | 详情 |
|--------|------|
| SEC 名称正确 | 拼写、斜杠、与 `section_defs[]` 匹配 |
| GPL License | `char _license[] SEC("license") = "GPL";` |
| 宏与类型匹配 | kprobe → `BPF_KPROBE`，fentry → `BPF_PROG`，**不能混用** |
| 参数顺序匹配 | `BPF_KPROBE` 参数必须与内核函数签名一致 |
| kretprobe 入参 | 入参已丢失，需 kprobe 配合 Map 保存 |
| syscall wrapper | `__x64_sys_*` 需两级解引用，或改用 `__sys_*` |
| Map lookup NULL 检查 | `bpf_map_lookup_elem` 返回值必须判空 |
| 变量初始化 | 栈上变量 `= {}`，否则验证器拒绝 |
| 内存读取 | kprobe 用 `probe_read`/`BPF_CORE_READ`，fentry 可直接读 |
| 内核/用户空间 | 内核指针 `_kernel`，用户指针 `_user`，不能混用 |
| 栈不超 512 字节 | 大缓冲区用 `PERCPU_ARRAY` Map |
| pid vs tgid | `bpf_get_current_pid_tgid() >> 32` = 用户空间 PID |
| CO-RE 兼容 | 跨内核版本用 `BPF_CORE_READ`，单版本可用 `probe_read` |
| BTF Map | 用 `SEC(".maps")` 新式定义，启用 pretty-print/spin_lock/timer |

---

## 附录 A：x86-64 pt_regs 与寄存器映射详解

kprobe 的 BPF 程序接收 `struct pt_regs *ctx` 作为上下文。以下是完整的底层机制。

### pt_regs 结构体

定义在 `arch/x86/include/asm/ptrace.h`（第 59 行）：

```c
struct pt_regs {
    unsigned long r15, r14, r13, r12;
    unsigned long bp, bx;
    unsigned long r11, r10, r9, r8;
    unsigned long ax, cx, dx, si, di;
    unsigned long orig_ax;
    unsigned long ip, cs, flags, sp, ss;
};
```

### 寄存器到函数参数的映射

x86-64 System V ABI 的参数传递约定，由 `PT_REGS_PARM*` 宏封装（`tools/lib/bpf/bpf_tracing.h:113`）：

| 函数参数 | 寄存器 | pt_regs 字段 | 提取宏 | CO-RE 版本 |
|---------|--------|-------------|--------|-----------|
| 第 1 个 | RDI | `ctx->rdi` | `PT_REGS_PARM1(ctx)` | `PT_REGS_PARM1_CORE(ctx)` |
| 第 2 个 | RSI | `ctx->rsi` | `PT_REGS_PARM2(ctx)` | `PT_REGS_PARM2_CORE(ctx)` |
| 第 3 个 | RDX | `ctx->rdx` | `PT_REGS_PARM3(ctx)` | `PT_REGS_PARM3_CORE(ctx)` |
| 第 4 个 | RCX | `ctx->rcx` | `PT_REGS_PARM4(ctx)` | `PT_REGS_PARM4_CORE(ctx)` |
| 第 5 个 | R8  | `ctx->r8`  | `PT_REGS_PARM5(ctx)` | `PT_REGS_PARM5_CORE(ctx)` |
| 第 6 个 | R9  | `ctx->r9`  | 无宏，手动 `ctx->r9` | — |
| 返回值  | RAX | `ctx->rax` | `PT_REGS_RC(ctx)` | `PT_REGS_RC_CORE(ctx)` |
| 第 7+ 个 | 栈上 | — | `bpf_probe_read_kernel` | — |

### BPF_KPROBE 宏的展开原理

```c
// tools/lib/bpf/bpf_tracing.h:421
#define BPF_KPROBE(name, args...)                       \
name(struct pt_regs *ctx);                              \
static __always_inline typeof(name(0))                  \
____##name(struct pt_regs *ctx, ##args);                \
typeof(name(0)) name(struct pt_regs *ctx)               \
{                                                       \
    return ____##name(___bpf_kprobe_args(args));         \
}                                                       \
static __always_inline typeof(name(0))                  \
____##name(struct pt_regs *ctx, ##args)

// 第 397 行：参数按顺序从寄存器提取
#define ___bpf_kprobe_args0() ctx
#define ___bpf_kprobe_args1(x) ___bpf_kprobe_args0(), (void *)PT_REGS_PARM1(ctx)
#define ___bpf_kprobe_args2(x, args...) ___bpf_kprobe_args1(args), (void *)PT_REGS_PARM2(ctx)
// ... 以此类推到 args5
```

### kprobe 触发的内核调用链

```
内核函数被调用
  └── int3 断点 或 ftrace 回调
        └── kprobe_dispatcher(kp, regs)             // trace_kprobe.c:1665
              └── kprobe_perf_func(tk, regs)        // trace_kprobe.c:1521
                    └── trace_call_bpf(call, regs)  // bpf_trace.c:95
                          └── BPF_PROG_RUN(prog, regs)
                                    ↓
                              你的 BPF 程序(struct pt_regs *ctx)
```

---

## 附录 B：Helper 函数内核实现详解

每种程序类型通过 `xxx_func_proto()` 控制可用 Helper 集合。

### 通用 Helper（`kernel/bpf/helpers.c`）

```c
// bpf_map_lookup_elem
const struct bpf_func_proto bpf_map_lookup_elem_proto = {
    .func       = bpf_map_lookup_elem,
    .gpl_only   = false,
    .ret_type   = RET_PTR_TO_MAP_VALUE_OR_NULL,  // 可能返回 NULL
    .arg1_type  = ARG_CONST_MAP_PTR,
    .arg2_type  = ARG_PTR_TO_MAP_KEY,
};

// bpf_get_current_pid_tgid
const struct bpf_func_proto bpf_get_current_pid_tgid_proto = {
    .func       = bpf_get_current_pid_tgid,
    .gpl_only   = false,
    .ret_type   = RET_INTEGER,  // 返回 (tgid << 32) | pid
};
```

### Tracing Helper 注册（`kernel/trace/bpf_trace.c:1021`）

```c
static const struct bpf_func_proto *
bpf_tracing_func_proto(enum bpf_func_id func_id, const struct bpf_prog *prog)
{
    switch (func_id) {
    case BPF_FUNC_map_lookup_elem:     return &bpf_map_lookup_elem_proto;
    case BPF_FUNC_probe_read_kernel:
        return security_locked_down(LOCKDOWN_BPF_READ_KERNEL) < 0 ?
               NULL : &bpf_probe_read_kernel_proto;
    case BPF_FUNC_get_current_pid_tgid: return &bpf_get_current_pid_tgid_proto;
    // ...
    }
}
```

Kprobe 在此基础上额外支持 `bpf_override_return`、`bpf_get_func_ip` 等（`kprobe_prog_func_proto()`，同文件）。

---

## 附录 C：关键源码文件索引

| 文件 | 内容 |
|------|------|
| `tools/lib/bpf/bpf_helpers.h` | SEC 宏、Map 定义宏（`__uint`/`__type`） |
| `tools/lib/bpf/bpf_tracing.h` | `BPF_KPROBE`/`BPF_PROG` 宏、`PT_REGS_PARM*` 宏 |
| `tools/lib/bpf/bpf_core_read.h` | `BPF_CORE_READ` 等 CO-RE 宏 |
| `tools/lib/bpf/libbpf.c` | `section_defs[]`、open/load/attach 流程 |
| `arch/x86/include/asm/ptrace.h` | x86-64 `struct pt_regs` |
| `arch/x86/include/asm/syscall_wrapper.h` | syscall 包装器说明 |
| `include/uapi/linux/bpf.h` | `bpf_prog_type`、`bpf_map_type` 枚举 |
| `include/linux/bpf.h` | `struct bpf_map`（含 BTF 字段） |
| `kernel/bpf/helpers.c` | 通用 Helper 实现 |
| `kernel/trace/bpf_trace.c` | Tracing Helper 实现、`trace_call_bpf()` |
| `kernel/trace/trace_kprobe.c` | kprobe 回调链 |
| `kernel/bpf/verifier.c` | 验证器 |
| `kernel/bpf/syscall.c` | `map_create()`、`map_check_btf()` |
| `kernel/bpf/btf.c` | `btf_vmlinux_map_ids_init()` |
| `samples/bpf/` | 内核自带 BPF 示例 |
| `tools/testing/selftests/bpf/progs/` | BPF 自测程序 |
