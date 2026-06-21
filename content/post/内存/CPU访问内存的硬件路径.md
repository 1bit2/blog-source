+++
date = '2026-06-10'
title = 'CPU 访问内存的硬件路径——地址总线、数据总线与缓存行突发传输'
weight = 1
tags = [
    "地址总线",
    "数据总线",
    "缓存行",
    "Cache Line",
    "DDR",
    "突发传输",
    "MMU",
    "TLB",
    "页表",
    "clflush",
    "prefetch",
]
categories = [
    "内存",
]
+++
# CPU 访问内存的硬件路径

> 基于 Linux 5.15.78 内核源码。
> 本文从**硬件物理路径**角度，解释 CPU 执行一条内存指令时，电信号如何在总线上传输、DRAM 如何响应、缓存行如何搬运。**页表软件遍历、缺页处理、TLB 代际管理等软件路径已移至姊妹篇 §3–§8，本文不再重复。**
> 姊妹篇：[CPU 页表与缓存机制](CPU页表与缓存机制.md) — **软件视角**：页表遍历、TLB、PCID、TLB Shootdown、MESI、Cache Miss 实战。

---

## 目录

- [1. 核心问题：64 位系统每次取多少数据？](#1-核心问题64-位系统每次取多少数据)
- [2. CPU 与内存的物理连接：三种总线](#2-cpu-与内存的物理连接三种总线)
  - [2.1 地址总线——能找多远](#21-地址总线能找多远)
  - [2.2 数据总线——一次搬多宽](#22-数据总线一次搬多宽)
  - [2.3 控制总线——操作命令](#23-控制总线操作命令)
- [3. 缓存行：CPU 与内存之间的搬运单位](#3-缓存行cpu-与内存之间的搬运单位)
  - [3.1 为什么是 64 字节](#31-为什么是-64-字节)
  - [3.2 内核源码中的缓存行定义](#32-内核源码中的缓存行定义)
  - [3.3 clflush_size：CPU 自检缓存行大小](#33-clflush_sizecpu-自检缓存行大小)
- [4. DDR 突发传输：8 拍 × 8 字节 = 64 字节](#4-ddr-突发传输8-拍--8-字节--64-字节)
- [5. DRAM 内部结构：行列二维阵列](#5-dram-内部结构行列二维阵列)
- [6. 多级缓存：每一层之间都搬 64 字节](#6-多级缓存每一层之间都搬-64-字节)
- [7. 完整路径：从 mov rax, [virtual_addr] 到读出数据](#7-完整路径从-mov-rax-virtual_addr-到读出数据)
- [8. 内核中的缓存行操作](#8-内核中的缓存行操作)
  - [8.1 prefetch——预取缓存行](#81-prefetch预取缓存行)
  - [8.2 clflush——刷新缓存行](#82-clflush刷新缓存行)
  - [8.3 clflush_cache_range——按范围刷缓存行](#83-clflush_cache_range按范围刷缓存行)
- [9. 总结：硬件与软件的分工边界](#9-总结硬件与软件的分工边界)

---

## 1. 核心问题：64 位系统每次取多少数据？

**"64 位"指的是什么？**
- CPU 通用寄存器宽度：64 位（rax, rbx, rcx...）
- 虚拟地址宽度：48 位（x86-64 实际使用 48 位，高 16 位为符号扩展）
- 单条指令能处理的整数宽度：64 位

**"64 位"不是指：**
- 不是每次从内存取 64 字节
- 不是数据总线宽度为 64 字节
- 不是缓存行大小为 64 字节（这只是巧合）

**实际上每次从内存搬运数据的最小单位是缓存行（Cache Line）。** 现代 x86 CPU（Intel Core、AMD Ryzen/EPYC）的缓存行恰好是 **64 字节**，这是 CPU 微架构的设计选择，与"64 位"没有因果关系。

即使一条指令只需要读 1 个字节（`mov al, [addr]`），CPU 也会从内存取回整个 64 字节缓存行——这是利用了空间局部性原理（程序接下来大概率访问相邻地址）。

---

## 2. CPU 与内存的物理连接：三种总线

```
         CPU 芯片                              DDR 内存条
   ┌─────────────────────┐              ┌─────────────────────┐
   │                     │              │                     │
   │  寄存器 + ALU       │              │  存储单元阵列        │
   │       ↕             │              │  (行×列 电容矩阵)    │
   │  L1/L2/L3 Cache    │              │                     │
   │       ↕             │              │  行地址锁存器        │
   │  MMU + TLB          │              │  列地址锁存器        │
   │       ↕             │              │  感应放大器          │
   │  内存控制器(IMC)    │              │  I/O 缓冲器          │
   │       ↕             │              │                     │
   └───────┬─────────────┘              └──────────┬──────────┘
           │                                       │
           │  ┌─────────────────────────────────────┐
           ├──┤  地址总线 (Address Bus)              │
           │  │  物理地址线: 40~52 根               │
           │  │  (决定最大可访问物理内存)             │
           │  ├─────────────────────────────────────┤
           ├──┤  数据总线 (Data Bus)                │
           │  │  每通道 64 根数据线                  │
           │  │  DDR: 上升沿+下降沿各传一次           │
           │  ├─────────────────────────────────────┤
           └──┤  控制总线 (Control Bus)              │
              │  RAS/CAS/WE/CS/CKE/CLK             │
              └─────────────────────────────────────┘
```

### 2.1 地址总线——能找多远

地址总线的宽度决定了 CPU 能访问多大的物理内存空间。

| 架构 | 地址总线宽度 | 最大物理内存 |
|------|------------|------------|
| x86-32 | 32 位 (36 位 PAE) | 4 GB (64 GB PAE) |
| x86-64 | 40~52 位 | 1 TB ~ 4 PB |
| 实际主流芯片 | 39~46 位 | 512 GB ~ 64 TB |

**注意：** "64 位系统"的地址总线并非 64 根。x86-64 架构定义的物理地址最大 52 位（`MAXPHYADDR`），实际芯片实现通常是 39~46 位。

> 内核源码验证：`arch/x86/include/asm/pgtable_64_types.h:67` 定义 `MAX_POSSIBLE_PHYSMEM_BITS = 52`。物理地址上限通过 CPUID 叶号 0x80000008 查询，结果存入 `boot_cpu_data.x86_phys_bits`（`arch/x86/mm/physaddr.h:7` 使用该值判断地址是否合法）。

内核中与物理地址宽度相关的定义在 `arch/x86/include/asm/processor.h:126`：

```c
u16    x86_clflush_size;    // CPU 自检的缓存行大小，见第3节
```

物理地址上限通过 `CPUID` 指令查询（叶号 0x80000008 的 EAX[7:0]），内核启动时读取并存入 `boot_cpu_data.x86_phys_bits`。

### 2.2 数据总线——一次搬多宽

数据总线决定了一次总线事务能传输多少位数据。

| 内存类型 | 每通道数据总线宽度 | 说明 |
|---------|-----------------|------|
| DDR3/DDR4/DDR5 | 64 位 (8 字节) | 每通道 64 根数据线 |
| DDR5 双通道 | 2 × 64 位 = 128 位 | 两个独立通道 |

**关键概念：DDR（Double Data Rate）双沿传输**

```
时钟信号:    ──┐   ┌──┐   ┌──┐   ┌──┐   ┌──┐   ┌──
               └───┘  └───┘  └───┘  └───┘  └───┘  └──
               ↑   ↑   ↑   ↑   ↑   ↑   ↑   ↑
传输时机:      上  下  上  下  上  下  上  下
               升  降  升  降  升  降  升  降
               沿  沿  沿  沿  沿  沿  沿  沿

每次传输:     8 字节 (64 位数据总线)
DDR4-3200:    3200 MT/s × 8 字节 = 25.6 GB/s (单通道)
双通道:       51.2 GB/s
```

一次时钟沿传输 8 字节，但 DRAM 不会只传一次就停下来——它用**突发传输**连续传 8 拍，凑成一个完整的缓存行。

### 2.3 控制总线——操作命令

| 信号 | 含义 |
|------|------|
| RAS# (Row Address Strobe) | 行地址选通：锁存行地址 |
| CAS# (Column Address Strobe) | 列地址选通：锁存列地址 |
| WE# (Write Enable) | 写使能 |
| CS# (Chip Select) | 片选 |
| CKE (Clock Enable) | 时钟使能 |

这些控制信号配合地址总线，完成 DRAM 的读/写/预充电/刷新操作。

---

## 3. 缓存行：CPU 与内存之间的搬运单位

### 3.1 为什么是 64 字节

缓存行大小是 CPU 设计时的微架构决策，需要平衡以下因素：

| 因素 | 缓存行大 → 优势 | 缓存行大 → 劣势 |
|------|--------------|--------------|
| 空间局部性 | 相邻数据一次性取到，减少后续 miss | 如果只访问 1 字节，浪费带宽 |
| Tag 存储开销 | 行数少 → Tag SRAM 面积小 | — |
| 缓存命中率 | — | 替换粒度粗，可能把有用的行换出 |
| 伪共享 (False Sharing) | — | 多核共享同一行的概率增大 |

x86 的演进：

| CPU 代际 | 缓存行大小 | 说明 |
|---------|----------|------|
| 486 | 16 字节 | 行缓冲 (line fill buffer) |
| Pentium | 32 字节 | 一级缓存首次引入 |
| Pentium III/II | 32 字节 | — |
| Pentium 4 | 64~128 字节 | L1=64B, L2=128B |
| Core 2 至今 | **64 字节** | 统一标准 |

### 3.2 内核源码中的缓存行定义

`arch/x86/include/asm/cache.h`:

```c
/* L1 cache line size */
#define L1_CACHE_SHIFT      (CONFIG_X86_L1_CACHE_SHIFT)   // 默认值 6
#define L1_CACHE_BYTES      (1 << L1_CACHE_SHIFT)         // = 64 字节
```

`CONFIG_X86_L1_CACHE_SHIFT` 在 Kconfig 中按 CPU 型号设定默认值（`arch/x86/Kconfig.cpu:318`）：

```
config X86_L1_CACHE_SHIFT
    int
    default "7" if MPENTIUM4 || MPSC              // Pentium 4: 128 字节
    default "6" if MK7 || MK8 || MPENTIUMM ||
                    MCORE2 || MATOM || ...          // 现代 CPU: 64 字节
    default "4" if MELAN || M486SX || M486 ...      // 486: 16 字节
    default "5" if MWINCHIP3D || MCRUSOE || ...       // Pentium: 32 字节
```

通用层封装（`include/linux/cache.h:85`）：

```c
#ifndef CONFIG_ARCH_HAS_CACHE_LINE_SIZE
#define cache_line_size()    L1_CACHE_BYTES       // 64
#endif
```

### 3.3 clflush_size：CPU 自检缓存行大小

CPU 通过 CPUID 指令自检缓存行大小，结果存在 `boot_cpu_data.x86_clflush_size`（`arch/x86/include/asm/processor.h:126`）：

```c
u16    x86_clflush_size;    // 由 CPUID 叶号 1 的 EBX[15:8] × 8 得到
```

CPUID 叶号 1，EBX 寄存器（来自 Intel/AMD CPUID 手册，非内核源码）：
- 位 [15:8]：CLFLUSH line size，单位是 8 字节
- 现代 CPU 此字段 = 8 → `8 × 8 = 64` 字节

内核在启动时调用 CPUID 读取该值，后续所有缓存行操作都以此为基准。

---

## 4. DDR 突发传输：8 拍 × 8 字节 = 64 字节

> 本节内容来自 JEDEC DDR 标准规范，非内核源码。

DDR 内存的突发传输（Burst Transfer）是理解"为什么一次取 64 字节"的关键。

```
DDR4 突发传输 (Burst Length = 8):

内存控制器发出: 行地址 + RAS# → 激活行
               列地址 + CAS# + READ

DRAM 连续输出 8 拍数据:

时钟:  ──┐  ┌──┐  ┌──┐  ┌──┐  ┌──┐  ┌──┐  ┌──┐  ┌──┐  ┌──
         └──┘  └──┘  └──┘  └──┘  └──┘  └──┘  └──┘  └──┘  └──
         ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑
数据:    D0 D0 D1 D1 D2 D2 D3 D3 D4 D4 D5 D5 D6 D6 D7 D7
         ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑
         上 下 上 下 上 下 上 下 上 下 上 下 上 下 上 下
         升 降 升 降 升 降 升 降 升 降 升 降 升 降 升 降

每拍 = 1 个时钟沿 = 8 字节 (64 位数据总线)
DDR: 上升沿 + 下降沿各 1 拍 → 每时钟周期 2 拍
BL=8: 共 8 拍 = 4 个时钟周期

总数据量: 8 拍 × 8 字节 = 64 字节 = 1 个缓存行
```

**为什么 BL=8？**

JEDEC 标准规定 DDR 的突发长度为 8，这是经过精确计算的：
- 缓存行 64 字节 / 数据总线 8 字节 = 8 拍 → 恰好匹配
- 如果 BL=4（DDR1 早期），一次突发只有 32 字节，需要 2 次突发才能填满一个缓存行
- BL=8 使一次突发恰好填满一个缓存行，最大化总线利用率

**这就是"64 字节"的真正来源：**
```
缓存行 = DDR 突发长度 × 数据总线宽度 = 8 × 8 = 64 字节
```

不是"64 位系统所以取 64 字节"，而是 CPU 缓存行设计、DRAM 突发长度、数据总线宽度三者协同优化的结果。

---

## 5. DRAM 内部结构：行列二维阵列

> 本节内容来自 DRAM 硬件架构规范，非内核源码。

DRAM（Dynamic RAM）的存储单元是按行列组织的二维电容矩阵：

```
DRAM Bank 内部结构 (以 DDR4 8Gb 为例):

            列地址 (10 位, 1024 列)
            ┌────────────────────────────────────┐
    行地址   │  [0,0] [0,1] [0,2] ... [0,1023]   │  ← 一行 = 1024 × 8bit = 1KB
    (16位    │  [1,0] [1,1] [1,2] ... [1,1023]   │
    65536行) │  [2,0] [2,1] [2,2] ... [2,1023]   │
            │   ...                              │
            │  [65535,0] ... ... ... [65535,1023]│
            └────────────────────────────────────┘

读取过程:
  1. ACTIVATE: RAS# + 行地址 → 整行 (1KB) 读入行缓冲 (Sense Amp)
  2. READ: CAS# + 列地址 → 从行缓冲中选取 64 字节输出
  3. PRECHARGE: 关闭当前行，准备下一次访问
```

**关键洞察：**
- 打开一行（ACTIVATE）是最耗时的操作（~30ns）
- 同一行内的列访问很快（~5ns），因为数据已在行缓冲中
- 突发传输 64 字节只是行缓冲中 1KB 的一小段
- 如果下次访问命中同一行（行命中），不需要重新 ACTIVATE → 性能大幅提升

**这就是空间局部性在硬件层面的体现：** 相邻地址大概率在同一行，连续访问时只需发 CAS#，不需要重复 RAS#。

---

## 6. 多级缓存：每一层之间都搬 64 字节

```
CPU 核心 (寄存器: 每次 8 字节)
    ↕ 1~4 周期
L1 Cache (32~64 KB)
    │  缓存行 = 64 字节
    │  未命中时向 L2 请求一个缓存行
    ↕ 4~12 周期
L2 Cache (256 KB ~ 1 MB)
    │  缓存行 = 64 字节
    │  未命中时向 L3 请求一个缓存行
    ↕ 10~40 周期
L3 Cache (4~64 MB, LLC)
    │  缓存行 = 64 字节
    │  未命中时向内存控制器请求一个缓存行
    ↕ 40~100 周期
内存控制器 (IMC)
    │  发起 DDR 突发传输: 8 拍 × 8 字节 = 64 字节
    ↕ 100~300 周期
DRAM
```

**每一级之间的数据传输单位都是 64 字节缓存行。** 不是只有 L1↔CPU 是 64 字节，而是整个层次结构中每一级间的搬运粒度都是缓存行。

Cache 容量对比：

| 层级 | 可容纳缓存行数 | 说明 |
|------|-------------|------|
| L1 32KB | 512 行 | 32768 / 64 |
| L1 64KB | 1024 行 | 65536 / 64 |
| L2 256KB | 4096 行 | 262144 / 64 |
| L2 1MB | 16384 行 | 1048576 / 64 |
| L3 32MB | 524288 行 | 33554432 / 64 |

**缓存替换的最小单位也是缓存行**：当缓存满了需要替换时，替换的是一整行 64 字节，不是一个字节。

---

## 7. 完整路径：从 mov rax, [virtual_addr] 到读出数据

以 `mov rax, [0x7f3456967890]` 为例，完整路径如下：

### 第一步：虚拟地址 → 物理地址（MMU 硬件页表遍历）

```
虚拟地址: 0x7f3456967890

拆分 (48 位虚拟地址, 4 级页表):
  [47:39] = 0x0FE = PGD 索引 254
  [38:30] = 0x0D1 = PUD 索引 209
  [29:21] = 0x0B4 = PMD 索引 180
  [20:12] = 0x167 = PTE 索引 359
  [11:0]  = 0x890 = 页内偏移 2192

  (为什么是 9-9-9-9-12 的拆分？详见 [CPU 页表与缓存机制 §2](CPU页表与缓存机制.md))

MMU 的 Page Table Walker 硬件自动遍历:
  CR3 寄存器 → PGD 表物理基址
    → PGD[254] → 取出 PUD 表物理基址
    → PUD[209] → 取出 PMD 表物理基址
    → PMD[180] → 取出 PTE 表物理基址
    → PTE[359] → 取出物理页帧号 (PFN)

假设 PTE 内容: PFN = 0x0001_ABCD_EF80, Present=1
物理地址 = (PFN << 12) | 偏移 = 0x1_ABCD_EF80_890
```

如果 PTE 的 Present=0，CPU 触发 #PF 异常，陷入内核（缺页处理的完整软件路径详见 [CPU 页表与缓存机制 §6](CPU页表与缓存机制.md)）。

### 第二步：物理地址 → Cache 查找

```
物理地址: 0x1_ABCD_EF80_890

Cache 控制器用物理地址查缓存 (PIPT: Physically Indexed, Physically Tagged):

  缓存行内偏移 = PA[5:0]  = 0x10 (64 字节行中的第 16 字节)
  Set Index    = PA[中间位] (取决于 Cache 路数和大小)
  Tag          = PA[高位]

L1 Cache 查找:
  → 用 Set Index 定位到 Set
  → 在 Set 内的多路 (Way) 中比较 Tag
  → 匹配 + Valid=1 → 命中!
     从 L1 SRAM 读出 8 字节 → 填入 rax 寄存器
     延迟: 1~4 个时钟周期 (约 0.3~1 ns)
```

### 第三步：Cache Miss → 逐级回源

```
L1 Miss → 查 L2:
  → L2 用同样的 Set Index + Tag 查找
  → 命中: 取出 64 字节缓存行 → 填入 L1 → CPU 读 8 字节
  → 延迟: 4~12 周期 (约 1~3 ns)

L2 Miss → 查 L3:
  → L3 查找 (通常 16 路组相联，容量大，命中率高)
  → 命中: 取出 64 字节缓存行 → 填入 L2 → L1 → CPU
  → 延迟: 10~40 周期 (约 3~10 ns)

L3 Miss → 访问 DRAM:
  → 内存控制器将物理地址解码为 Bank/Row/Column
  → ACTIVATE: 发 RAS# + 行地址，打开行 (~30ns)
  → READ: 发 CAS# + 列地址，从行缓冲读 (~5ns)
  → DDR 突发传输: 8 拍 × 8 字节 = 64 字节
  → 64 字节沿数据总线返回 → 填入 L3 → L2 → L1 → CPU
  → 延迟: 100~300 周期 (约 30~80 ns)
```

### 第四步：数据返回 CPU

```
L1 命中后:
  缓存行内的 64 字节数据已在 L1 SRAM 中
  Cache 控制器按偏移 0x10 选出 8 字节
  通过内部数据通路传送到 rax 寄存器
  指令完成，流水线继续
```

---

## 8. 内核中的缓存行操作

> **灾后重建说明**：本节内容根据内核源码重写（原 §8.1–§8.3 在编辑过程中被误删）。所有代码片段均直接取自 5.15.78 源码，已核对行号。

内核提供两组缓存行级别的操作接口：**预取**（`prefetch`，把数据提前拉进 L1）和**显式失效**（`clflush`，把缓存行从 L1/L2/L3 写回并逐出）。前者是性能优化，后者是正确性保证（DMA 一致性、持久内存落盘、self-modifying code）。

### 8.1 prefetch——预取缓存行

`include/linux/prefetch.h` 封装了三个宏：

```c
// include/linux/prefetch.h:40,44,48
#ifndef ARCH_HAS_PREFETCH
#define prefetch(x)              __builtin_prefetch(x)      // 读预取
#endif
#ifndef ARCH_HAS_PREFETCHW
#define prefetchw(x)             __builtin_prefetch(x,1)    // 写预取
#endif
#ifndef ARCH_HAS_SPINLOCK_PREFETCH
#define spin_lock_prefetch(x)    prefetchw(x)               // 预取锁
#endif

#ifndef PREFETCH_STRIDE
#define PREFETCH_STRIDE          (4*L1_CACHE_BYTES)         // 预取步长：256 字节
#endif
```

| 接口 | x86 指令 | Cache 状态变迁 | 典型场景 |
|------|---------|--------------|---------|
| `prefetch(x)` | `prefetchnta` / `prefetcht2` | Invalid/Shared → Shared | 链表遍历下一个节点 |
| `prefetchw(x)` | `prefetchwt1` / `prefetchw` | Shared → **Exclusive/Modified** | 即将写某字段时（避免 RFO 延迟） |
| `spin_lock_prefetch(x)` | 同 `prefetchw` | 同上 | 抢锁前把 cacheline 拉到独占态 |

`prefetchw` 的关键价值：MESI 协议下，写一个 Shared 态的 cacheline 会触发 **RFO（Request For Ownership）**，需要广播 Invalidate 给所有共享核并等待 ACK，延迟几十到几百周期。提前 `prefetchw` 把这个过程与计算并行掉。

批量预取封装：

```c
// include/linux/prefetch.h:55-64
static inline void prefetch_range(void *addr, size_t len)
{
#ifdef ARCH_HAS_PREFETCH
    char *cp;
    char *end = addr + len;
    for (cp = addr; cp < end; cp += PREFETCH_STRIDE)   // 步长 256 字节
        prefetch(cp);
#endif
}
```

**使用纪律**：prefetch 是 hint 不是命令，CPU 可以无视（地址非法、流水线满、带宽紧张都会丢弃）。`prefetch(0)` 被明确允许，不触发 fault。滥用 prefetch 反而会污染 L1、挤掉有用数据——`perf stat -e cache-misses,cache-references` 是验证手段。

### 8.2 clflush——刷新缓存行

`arch/x86/include/asm/special_insns.h:198-209` 把 x86 的两条刷缓存行指令封装成 C：

```c
// arch/x86/include/asm/special_insns.h:198-209
static inline void clflush(volatile void *__p)
{
    asm volatile("clflush %0" : "+m" (*(volatile char __force *)__p));
}

static inline void clflushopt(volatile void *__p)
{
    // CLFLUSHOPT 是弱排序版本，需要额外 fence
    alternative_io(".byte 0x3e; clflush %P0",       // 旧指令前缀：clflush
                   ".byte 0x66; clflush %P0",       // 0x66 前缀：clflushopt
                   X86_FEATURE_CLFLUSHOPT,
                   "+m" (*(volatile char __force *)__p));
}
```

| 指令 | 排序性 | 吞吐量 | 行为 |
|------|--------|--------|------|
| `clflush` | 强排序（隐式 MFENCE 效果） | 慢（约 100 周期 / 行） | 把地址所在 cacheline 写回内存 + 从所有层级 Cache 逐出 |
| `clflushopt` | **弱排序**（需 `mfence`/`sfence` 包裹） | 快（可流水线） | 同上，但 CPU 可重排多个 clflushopt |

clflush 的语义是**物理地址粒度**的：同一虚拟地址在两个进程里映射到同一物理页时，任一进程执行 clflush 都会让两边同时失效（PIPT 架构下 cacheline 以物理地址为 key）。

适用场景：
- **DMA 一致性**：CPU 写完数据后 clflush，外设从内存读时能看到最新值（非 coherent DMA 平台）
- **持久内存（PMEM）**：写入 NVDIMM 必须 clflush/clflushopt + sfence 才能保证掉电持久
- **Self-modifying code**：内核打补丁（`text_poke`）后刷掉旧指令 cacheline，避免 CPU 取到旧指令

### 8.3 clflush_cache_range——按范围刷缓存行

单条 `clflush` 只能刷一个 cacheline。要刷一段连续内存，内核提供 `clflush_cache_range()`（`arch/x86/mm/pat/set_memory.c:314`）：

```c
// arch/x86/mm/pat/set_memory.c:293-320
static void clflush_cache_range_opt(void *vaddr, unsigned int size)
{
    const unsigned long clflush_size = boot_cpu_data.x86_clflush_size;  // 通常 64
    // 把起始地址向下对齐到 cacheline 边界
    void *p    = (void *)((unsigned long)vaddr & ~(clflush_size - 1));
    void *vend = vaddr + size;

    if (p >= vend)
        return;

    for (; p < vend; p += clflush_size)
        clflushopt(p);            // 用弱排序版本 + 循环
}

/**
 * clflush_cache_range - flush a cache range with clflush
 * @vaddr:  virtual start address
 * @size:   number of bytes to flush
 *
 * CLFLUSHOPT is an unordered instruction which needs fencing with
 * MFENCE or SFENCE to avoid ordering issues.
 */
void clflush_cache_range(void *vaddr, unsigned int size)
{
    mb();                              // 前 fence：确保之前的写都已提交
    clflush_cache_range_opt(vaddr, size);
    mb();                              // 后 fence：确保所有 clflushopt 完成
}
EXPORT_SYMBOL_GPL(clflush_cache_range);
```

**设计要点**：

1. **地址对齐**：用户传入的 `vaddr` 未必在 cacheline 边界，`opt` 里先用 `& ~(clflush_size - 1)` 对齐到 64 字节边界，否则会漏刷首条 line
2. **循环刷**：从对齐后的起点每 64 字节 `clflushopt` 一次，直到覆盖 `[vaddr, vaddr+size)` 全部 cacheline
3. **mb() 包裹**：`clflushopt` 是弱排序，必须前后各一个 `mb()`（x86 上等价于 `mfence`）才能保证"调用返回时，所有 cacheline 都已写回内存"
4. **用 `clflushopt` 而非 `clflush`**：循环内大量刷写时弱排序版本的吞吐量优势显著；强排序的开销由外层一对 `mb()` 承担

调用方：
- `arch_invalidate_pmem()`（持久内存 API，`CONFIG_ARCH_HAS_PMEM_API`）
- `set_memory_np()` / `set_memory_4k()` 等页表属性切换（确保旧 PTE 的 cacheline 失效）
- `text_poke()` 系列（热补丁后刷新指令 cacheline）

## 9. 总结：硬件与软件的分工边界

> **关于页表遍历 / TLB / 缺页处理的软件路径**：本节只给出硬件与软件职责的高层分工图，详细源码分析（CR3、PTE 位、TLB 代际管理、`handle_mm_fault`、`handle_pte_fault` 等）已移至 [CPU 页表与缓存机制](CPU页表与缓存机制.md) 第一部分（§3–§8），避免与本文硬件视角的内容重复。


```
┌─────────────────────────────────────────────────────┐
│                    CPU 硬件自动完成                    │
│                                                      │
│  1. MMU 用 CR3 中的 PGD 物理地址遍历 4 级页表         │
│  2. 取出 PTE 中的物理页帧号，合成物理地址              │
│  3. 用物理地址查 TLB → 命中则直接返回                  │
│  4. TLB Miss → Page Table Walker 自动查表             │
│  5. 得到物理地址后查 Cache（L1→L2→L3）                │
│  6. Cache Miss → 内存控制器发起 DDR 突发传输           │
│  7. 8 拍 × 8 字节 = 64 字节缓存行填入 Cache           │
│  8. CPU 从 Cache 读出需要的字节到寄存器                │
│                                                      │
│  前提：所有页表项 Present=1，权限足够                  │
│  延迟：TLB 命中 ~1ns，全 Miss ~60-100ns              │
└────────────────────┬────────────────────────────────┘
                     │
                     │ PTE Present=0 或权限不足
                     ↓
┌─────────────────────────────────────────────────────┐
│                    内核软件处理                        │
│                                                      │
│  1. CPU 触发 #PF 异常，陷入内核态                     │
│  2. do_user_addr_fault() 解析 error_code              │
│  3. handle_mm_fault() → __handle_mm_fault()           │
│  4. 软件遍历页表，分配缺失的页表页                     │
│  5. handle_pte_fault() 根据 PTE 状态分派:              │
│     → do_anonymous_page: 分配零页                     │
│     → do_read_fault: 从文件/缓存读取                   │
│     → do_cow_fault: COW 写时复制                      │
│     → do_swap_page: 从 swap 换入                      │
│  6. 填写 PTE（设置 Present=1 + 权限位）               │
│  7. 刷新 TLB（invlpg 或全量刷新）                     │
│  8. 返回用户态，重新执行触发缺页的指令                  │
│                                                      │
│  延迟：~1μs（minor fault）到 ~10ms（major fault+磁盘）│
└─────────────────────────────────────────────────────┘
```

**关键数据对照：**

| 项目 | 值 | 来源 |
|------|-----|------|
| 虚拟地址宽度 | 48 位 | x86-64 架构规范 |
| 物理地址宽度 | 39~52 位 | CPUID 叶号 0x80000008 |
| 页大小 | 4 KB (2^12) | `PAGE_SHIFT=12` |
| 每级页表项数 | 512 | `PTRS_PER_xxx=512` |
| PTE 物理帧号位 | 40 位 (位 12~51) | `pgtable_types.h` |
| 最大物理内存 | 4 PB (2^52) | 52 位物理地址 |
| 缓存行大小 | 64 字节 | `L1_CACHE_BYTES` |
| DDR 突发长度 | 8 拍 | JEDEC DDR 标准 |
| DDR 数据总线宽度 | 64 位 (8 字节) | 每通道 |
| 一次突发传输量 | 8 × 8 = 64 字节 = 1 缓存行 | 设计匹配 |
| L1 访问延迟 | ~1 ns | 1~4 周期 |
| L3 访问延迟 | ~10 ns | 10~40 周期 |
| DRAM 访问延迟 | ~60 ns | 100~300 周期 |
| Minor Page Fault | ~1 μs | 软件处理 |
| Major Page Fault | ~10 ms | 含磁盘 I/O |
