---
title: "内存"
description: "内存管理"
slug: "mem"
image: "category-icon.svg"
style:
    background: "#4285F4"
    color: "#fff"
---

> Linux 5.15.78 内核内存子系统：从硬件总线到用户态 malloc。
> 共 **7 篇**，分 4 层递进阅读。

## 推荐阅读路径

### 第 1 层：硬件基础
1. **[CPU 访问内存的硬件路径]({{< relref "post/内存/CPU访问内存的硬件路径.md" >}})** — 地址总线/数据总线/DDR 突发/DRAM 阵列

### 第 2 层：内核软件机制
2. **[CPU 页表与缓存机制]({{< relref "post/内存/CPU页表与缓存机制.md" >}})** — 4 级页表/TLB/PCID/Cache/MESI
3. **[CPU 上下文切换与内核栈]({{< relref "post/内存/CPU上下文切换与内核栈.md" >}})** — RIP/RSP 切换、__switch_to_asm、内核栈布局

### 第 3 层：内存管理全局
4. **[内存管理完全指南]({{< relref "post/内存/内存管理完全指南.md" >}})** — VMA/缺页/伙伴系统/slab/LRU/OOM 总览
5. **[x86_64 虚拟内存布局]({{< relref "post/内存/x86_64虚拟内存布局.md" >}})** — 用户/内核地址空间各区域划分

### 第 4 层：专题
6. **[内核内存对齐机制]({{< relref "post/内存/内核内存对齐机制.md" >}})** — slab 对齐/DMA 约束/False Sharing 防护
7. **[共享库加载与内存共享机制]({{< relref "post/内存/共享库加载与内存共享机制.md" >}})** — PT_LOAD 按需加载、page cache 多进程共享、COW
