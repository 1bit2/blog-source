---
title: "其他"
description: "其他内核子系统"
slug: "other"
image: "category-icon.svg"
style:
    background: "#607D8B"
    color: "#fff"
---

> 内核架构总览、eBPF、设备驱动等不属于上述分类的主题。
> 共 **6 篇**，建议按以下顺序阅读。

## 文档列表（建议阅读顺序）

1. **[内核整体架构]({{< relref "post/其他/内核整体架构.md" >}})** — 宏内核分层、start_kernel 启动链、各子系统定位
2. **[内核如何接管 CPU 与内存]({{< relref "post/其他/内核如何接管CPU与内存.md" >}})** — start_kernel → setup_arch → mm_init → sched_init → rest_init
3. **[Namespace、Cgroup、VRF 机制分析]({{< relref "post/其他/Namespace-Cgroup-VRF机制分析.md" >}})** — 资源隔离（Namespace）、资源限额（Cgroup）、路由隔离（VRF）
4. **[eBPF 实现原理]({{< relref "post/其他/eBPF实现原理.md" >}})** — 从加载到验证到 JIT：BPF 程序在内核中的完整生命周期
5. **[eBPF 开发指导手册]({{< relref "post/其他/eBPF开发指导手册.md" >}})** — SEC 宏、attach 类型选择、kprobe/tracepoint 开发指南
6. **[RCU 与锁机制源码分析]({{< relref "post/其他/RCU与锁机制源码分析.md" >}})** — spinlock/mutex/rwlock/rwsem、RCU 读写 API、宽限期、全景对比
