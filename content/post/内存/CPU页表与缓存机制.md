+++
date = '2026-04-30'
title = 'x86-64 页表遍历与 CPU 缓存机制——从 CR3 到 Cache Miss 优化'
weight = 2
tags = [
    "页表",
    "CR3",
    "TLB",
    "4级页表",
    "MMU",
    "cache",
    "MESI",
    "false sharing",
    "缓存优化",
    "PIPT",
    "TLB Shootdown",
]
categories = [
    "内存",
]
+++
# x86-64 页表遍历与 CPU 缓存机制

> 基于 Linux 5.15.78 内核源码。
> 前置阅读：[内存管理完全指南](内存管理完全指南.md) — VMA、缺页处理、伙伴系统全流程。
> 姊妹篇：[CPU 访问内存的硬件路径](CPU访问内存的硬件路径.md) — **硬件视角**：地址/数据总线、DDR 突发传输、DRAM 行列阵列、prefetch / clflush / clflush_cache_range 内核封装。

---

## 目录

- [第一部分：虚拟地址翻译——从 CPU 取指令到找到物理内存](#第一部分虚拟地址翻译从-cpu-取指令到找到物理内存)
  - [1. 全景：一次内存访问经历了什么](#1-全景一次内存访问经历了什么)
  - [2. 虚拟地址的拆分——为什么是 9-9-9-9-12](#2-虚拟地址的拆分为什么是-9-9-9-9-12)
  - [3. CR3 寄存器与页表遍历——硬件自动完成](#3-cr3-寄存器与页表遍历硬件自动完成)
  - [4. TLB——避免每次都走 4 级页表](#4-tlb避免每次都走-4-级页表)
  - [5. 进程切换时地址翻译如何处理——CR3 + PCID + TLB Shootdown](#5-进程切换时地址翻译如何处理cr3--pcid--tlb-shootdown)
  - [6. 内核软件页表遍历——缺页处理](#6-内核软件页表遍历缺页处理)
  - [7. 各级 offset 宏与函数](#7-各级-offset-宏与函数)
  - [8. 4 级 vs 5 级页表](#8-4-级-vs-5-级页表)
- [第二部分：CPU 缓存——物理地址到数据的高速通路](#第二部分cpu-缓存物理地址到数据的高速通路)
  - [9. Cache 与 TLB 做的事完全不同](#9-cache-与-tlb-做的事完全不同)
  - [10. 缓存层级架构——每层是谁的、存什么](#10-缓存层级架构每层是谁的存什么)
  - [11. Cache Line——为什么一次加载 64 字节](#11-cache-line为什么一次加载-64-字节)
  - [12. 缓存查找过程——Tag / Set Index / Offset](#12-缓存查找过程tag--set-index--offset)
  - [13. MESI 协议——多核如何保证缓存一致性](#13-mesi-协议多核如何保证缓存一致性)
  - [14. 上下文切换时 Cache 会失效吗——PIPT 的设计智慧](#14-上下文切换时-cache-会失效吗pipt-的设计智慧)
- [第三部分：理解 Cache Miss——为什么你的程序变慢了](#第三部分理解-cache-miss为什么你的程序变慢了)
  - [15. Cache Miss 的三种类型](#15-cache-miss-的三种类型)
  - [16. 访问步长与命中率的关系——一个核心心智模型](#16-访问步长与命中率的关系一个核心心智模型)
  - [17. 实战案例一：矩阵遍历——行优先 vs 列优先](#17-实战案例一矩阵遍历行优先-vs-列优先)
  - [18. 实战案例二：数据结构布局——让有用的数据挨在一起](#18-实战案例二数据结构布局让有用的数据挨在一起)
  - [19. 实战案例三：多线程 false sharing——看不见的性能杀手](#19-实战案例三多线程-false-sharing看不见的性能杀手)
  - [20. 实战案例四：链表 vs 数组——指针追逐的代价](#20-实战案例四链表-vs-数组指针追逐的代价)
  - [21. 内核中的缓存优化实践](#21-内核中的缓存优化实践)
  - [22. 度量工具——用 perf 验证你的优化](#22-度量工具用-perf-验证你的优化)

---

# 第一部分：虚拟地址翻译——从 CPU 取指令到找到物理内存

## 1. 全景：一次内存访问经历了什么

**要解决的问题**：每个进程都认为自己独占 0~256TB 的虚拟地址空间，但物理内存只有几 GB。CPU 需要在**每一次**内存访问时，把虚拟地址翻译成物理地址。这个翻译必须极快（纳秒级），否则 CPU 就饿死了。

解决方案是三层协作：

```
CPU 发出虚拟地址
        │
   ①  TLB 查询 (地址翻译缓存，1-2 cycles)
        │
        ├── 命中 → 得到物理地址 ──────────────────────┐
        │                                              │
        └── 未命中 → ② 硬件 Page Walk (~200 cycles)    │
                     CR3 → PGD → PUD → PMD → PTE      │
                     结果填入 TLB                       │
                                                       ▼
                                            ③ Cache 查询 (数据缓存)
                                               用物理地址查找数据
                                               L1(1ns) → L2(5ns) → L3(15ns) → DRAM(80ns)
                                                       │
                                                       ▼
                                                  数据返回 CPU
```

**注意三者的分工**：
- **TLB**：缓存"虚拟地址→物理地址"的翻译结果，按虚拟地址查询，**需要区分进程**
- **页表**：存储完整的翻译映射，TLB 未命中时由硬件遍历
- **Cache**：缓存"物理地址→数据内容"，按物理地址查询，**不需要区分进程**

---

## 2. 虚拟地址的拆分——为什么是 9-9-9-9-12

### 拆分方式

x86-64 Long Mode 下，CPU 硬件把 48 位虚拟地址固定拆成 5 段：

```
63        48  47    39  38    30  29    21  20    12  11       0
 ┌──────────┬─────────┬─────────┬─────────┬─────────┬──────────┐
 │ 符号扩展  │  PGD    │  PUD    │  PMD    │  PTE    │  Offset  │
 │ (16 bit) │ (9 bit) │ (9 bit) │ (9 bit) │ (9 bit) │ (12 bit) │
 └──────────┴─────────┴─────────┴─────────┴─────────┴──────────┘
              索引512项  索引512项  索引512项  索引512项  页内偏移4KB
```

### 设计动机：为什么 Offset 是 12 位，而不是和索引一样 9 位？

因为**页大小 = 4KB = 2^12 字节**是整个设计的起点。12 位 Offset 恰好索引一个 4KB 页内的每个字节。页大小由硬件和操作系统共同决定——太小（如 512B）导致页表项太多、TLB 覆盖范围太小；太大（如 64KB）导致内部碎片严重。4KB 是几十年实践沉淀的平衡点。

### 设计动机：为什么每级索引是 9 位而不是 8 位或 10 位？

这是一个精巧的约束求解——**让每级页表恰好占 1 个物理页**：

```
设计约束：
  页大小        = 4KB
  页表项大小    = 8 字节（64位系统，存物理地址+权限标志）
  虚拟地址      = 48 位

推导：
  页内偏移      = 12 位（由 4KB 页大小决定）
  剩余索引位    = 48 - 12 = 36 位
  分 4 级       → 36 / 4 = 9 位/级
  每级项数      = 2^9 = 512 项
  每级页表大小  = 512 × 8 = 4096 字节 = 恰好 1 个 4KB 页！
```

**"恰好 1 页"不是巧合，这个约束带来三个关键优势**：

| 优势 | 如果不是恰好 1 页会怎样 |
|------|---------------------|
| 页表页的分配/回收直接复用伙伴系统的 `alloc_page` | 需要专门的子页分配器，增加复杂度 |
| 页表页可以独立回收，无碎片 | 可能出现"半页还在用"无法回收的情况 |
| 天然对齐到页边界，简化地址计算 | 需要额外的对齐处理逻辑 |

对比其他位数方案：

| 每级位数 | 每级大小 | 问题 |
|---------|---------|------|
| 8 bit | 256×8=2KB | 浪费半页；4×8+12=44bit 只有 16TB 寻址空间 |
| **9 bit** | **512×8=4KB** | **恰好 1 页，完美** |
| 10 bit | 1024×8=8KB | 需要 2 个连续物理页，分配困难 |

源码中的对应定义：

```c
// arch/x86/include/asm/pgtable_64_types.h:69-84

#define PGDIR_SHIFT     39       // bit[47:39] → 9 位
#define PTRS_PER_PGD    512

#define PUD_SHIFT       30       // bit[38:30] → 9 位
#define PTRS_PER_PUD    512

#define PMD_SHIFT       21       // bit[29:21] → 9 位
#define PTRS_PER_PMD    512

#define PTRS_PER_PTE    512      // bit[20:12] → 9 位
// 每项 8 字节 → 512 × 8 = 4096 = PAGE_SIZE
```

---

## 3. CR3 寄存器与页表遍历——硬件自动完成

### CR3：告诉 CPU "页表在哪里"

CR3 寄存器存放当前地址空间的**顶层页表（PGD/PML4）的物理地址**。

```c
// arch/x86/mm/tlb.c:156-165

static inline unsigned long build_cr3(pgd_t *pgd, u16 asid)
{
    if (static_cpu_has(X86_FEATURE_PCID)) {
        return __sme_pa(pgd) | kern_pcid(asid);  // 物理地址 | PCID
    } else {
        return __sme_pa(pgd);                      // 纯物理地址
    }
}
```

```c
// arch/x86/include/asm/special_insns.h:52-55

static inline void native_write_cr3(unsigned long val)
{
    asm volatile("mov %0,%%cr3": : "r" (val) : "memory");
}
```

### 硬件 Page Walk：MMU 自动逐级查表

当 TLB 未命中时，MMU 硬件电路**自动**执行 4 次内存访问，软件无需参与：

```
                    CR3 (物理地址)
                      │
                      ↓
             ┌────────────────┐
             │  PGD (PML4)    │  512 项 × 8B = 1 页
             └───────┬────────┘
                     │ VA[47:39] 选中 1 项 → 取出下一级页表物理地址
                     ↓
             ┌────────────────┐
             │  PUD (PDPT)    │
             └───────┬────────┘
                     │ VA[38:30] 选中 1 项
                     ↓
             ┌────────────────┐
             │  PMD (PD)      │
             └───────┬────────┘
                     │ VA[29:21] 选中 1 项
                     ↓
             ┌────────────────┐
             │  PTE (PT)      │
             └───────┬────────┘
                     │ VA[20:12] 选中 1 项 → 取出物理页帧号
                     ↓
              物理页帧号 + VA[11:0]（页内偏移）= 最终物理地址
```

**内核的角色**：不参与 page walk 本身，只负责在缺页时**建表**（填充页表项），在进程切换时**切 CR3**（指向新进程的页表）。

---

## 4. TLB——避免每次都走 4 级页表

### 要解决的问题

硬件 page walk 需要 4 次内存访问（~200 cycles）。如果每次取指令、每次读写数据都走一遍，CPU 性能会下降 100 倍。

### 解决方案：TLB 缓存翻译结果

TLB 是 MMU 内部的高速缓存，存储最近的"虚拟页号→物理页帧号"映射。

**TLB 条目结构**：

```
┌──────────────────────┬──────────────────────┬──────────┬────────┐
│ 虚拟页号 (VPN)        │ 物理页帧号 (PFN)      │ 属性标志  │ PCID   │
│ = VA >> 12 (36 bit)  │ = PA >> 12 (40 bit)  │ R/W/X/U  │ (12bit)│
└──────────────────────┴──────────────────────┴──────────┴────────┘
```

**查询过程**：

```
CPU 访问虚拟地址 0x7FFF_DEAD_B234
  VPN = 0x7FFF_DEAD_B  (高 36 位)
  Offset = 0x234        (低 12 位)

TLB 查找 VPN → 命中:  PFN = 0x3_4567_8

物理地址 = (PFN << 12) | Offset = 0x3_4567_8234
```

一个 TLB 条目覆盖一个页的全部范围（4KB 页→4KB，2MB 大页→2MB）。

### TLB 是每个 CPU 核心私有的

```
┌───────────────────┐  ┌───────────────────┐
│    CPU Core 0     │  │    CPU Core 1     │
│ ┌───────────────┐ │  │ ┌───────────────┐ │
│ │ L1 iTLB (128) │ │  │ │ L1 iTLB (128) │ │ ← 核心私有
│ │ L1 dTLB  (64) │ │  │ │ L1 dTLB  (64) │ │ ← 核心私有
│ │ L2 sTLB(1536) │ │  │ │ L2 sTLB(1536) │ │ ← 核心私有
│ └───────────────┘ │  │ └───────────────┘ │
└───────────────────┘  └───────────────────┘
      没有 "共享 TLB" 层
```

**为什么必须核心私有**：
- **速度**：TLB 在流水线最前端，必须 1-2 cycles 完成，跨核共享的仲裁延迟无法接受
- **隔离**：不同核可能运行不同进程，同一虚拟地址指向不同物理地址

### TLB 容量与覆盖能力

| TLB 层 | 容量 | 4KB 页覆盖 | 2MB 大页覆盖 |
|--------|------|-----------|-------------|
| L1 iTLB | ~128 项 | 512 KB | 256 MB |
| L1 dTLB | ~64 项 | 256 KB | 128 MB |
| L2 sTLB | ~1536 项 | 6 MB | 3 GB |

4KB 页时 TLB 最多覆盖 ~6MB；2MB 大页覆盖 ~3GB——这就是数据库推荐 Huge Pages 的原因。

x86 还有专门的 **Page Structure Caches** 缓存中间级页表项（PGD/PUD/PMD），降低 TLB miss 时 page walk 的实际延迟。

---

## 5. 进程切换时地址翻译如何处理——CR3 + PCID + TLB Shootdown

### 问题：不同进程的虚拟→物理映射不同

进程 A 的 `0x400000` 映射到物理 `0x1000_0000`，进程 B 的 `0x400000` 映射到物理 `0x2000_0000`。切换进程后，TLB 中 A 的条目如果被 B 使用就会访问错误的物理地址。

### 解决方案一（无 PCID）：切换时刷新全部 TLB

最简单但代价最大。写入新 CR3 时隐式刷新所有 TLB 条目，切换后 TLB 全冷。

### 解决方案二（有 PCID）：用标签区分不同进程的条目

```c
// arch/x86/mm/tlb.c:86-87
#define CR3_HW_ASID_BITS  12   // CR3 低 12 位存放 PCID
```

每个 TLB 条目附带 PCID 标签，不同进程的条目可以共存。切换逻辑由 `choose_new_asid()` 决定是否刷新：

```c
// arch/x86/mm/tlb.c:213-248 — choose_new_asid

static void choose_new_asid(struct mm_struct *next, u64 next_tlb_gen,
                            u16 *new_asid, bool *need_flush)
{
    if (!static_cpu_has(X86_FEATURE_PCID)) {
        *new_asid = 0;
        *need_flush = true;   // 无 PCID → 必须全刷
        return;
    }

    // 查找该进程是否已有 ASID 槽位
    for (asid = 0; asid < TLB_NR_DYN_ASIDS; asid++) {
        if (cpu_tlbstate.ctxs[asid].ctx_id != next->context.ctx_id)
            continue;
        *new_asid = asid;
        // ASID 在但代数过期（期间有页表修改）→ 需要刷
        *need_flush = (cpu_tlbstate.ctxs[asid].tlb_gen < next_tlb_gen);
        return;
    }

    // 没有匹配槽位 → 分配新 ASID → 必须刷
    *new_asid = this_cpu_add_return(cpu_tlbstate.next_asid, 1) - 1;
    *need_flush = true;
}
```

实际加载 CR3 的两种模式：

```c
// arch/x86/mm/tlb.c:276-292 — load_new_mm_cr3

static void load_new_mm_cr3(pgd_t *pgdir, u16 new_asid, bool need_flush)
{
    if (need_flush) {
        invalidate_user_asid(new_asid);
        new_mm_cr3 = build_cr3(pgdir, new_asid);         // 不带 NOFLUSH → 刷该 ASID
    } else {
        new_mm_cr3 = build_cr3_noflush(pgdir, new_asid); // 带 CR3_NOFLUSH → 不刷
    }
    write_cr3(new_mm_cr3);
}
```

### 进程切换时 TLB 处理的完整决策树

```
┌─────────────────────────────────────────────────────────────┐
│           switch_mm_irqs_off() 中 TLB 的处理逻辑            │
├─────────────────────────────────────────────────────────────┤
│ 同进程线程切换（real_prev == next）                           │
│   → 直接 return，不碰 CR3/TLB                               │
│                                                             │
│ 不同进程切换，有 PCID：                                      │
│   ① ASID 槽位在 + 代数匹配  → CR3_NOFLUSH，TLB 不刷         │
│   ② ASID 槽位在 + 代数过期  → 刷该 ASID 的 TLB 条目         │
│   ③ 无可用 ASID 槽位        → 分配新 ASID，刷旧条目          │
│                                                             │
│ 不同进程切换，无 PCID：                                      │
│   → 写 CR3 时硬件自动全量刷新 TLB                            │
└─────────────────────────────────────────────────────────────┘
```

`TLB_NR_DYN_ASIDS` 在 x86-64 上通常为 6，意味着最近运行的 6 个不同进程可保留各自 TLB 条目不被刷掉——这就是 PCID 减少 TLB miss 的核心收益。

**同一进程的线程切换完全不碰 TLB**（共享 `mm_struct`，`real_prev == next`），这是线程切换比进程切换快的原因之一。

**PCID 的代价**：TLB 条目需要额外存储 PCID 标签，硬件面积增大；内核需要管理 ASID 分配/回收与 tlb_gen 代数追踪。但收益远大于代价。

### TLB Shootdown：修改页表时如何通知其他核

TLB 核心私有带来一个问题：当内核修改页表（`munmap`、COW 缺页、`mprotect`）时，**其他核的 TLB 可能缓存了旧映射**。

```
Core 0 执行 munmap(addr):
  1. 修改页表（清除 PTE）
  2. invlpg 刷新本核 TLB
  3. 发送 IPI（处理器间中断）给运行同一 mm 的其他核
        │
        ├──→ Core 1: 收到 IPI → flush_tlb_func() → 刷新对应条目
        ├──→ Core 2: 收到 IPI → flush_tlb_func() → 刷新对应条目
        └──→ Core 3: (运行不同 mm，跳过)
  4. 等待所有目标核确认
```

```c
// arch/x86/mm/tlb.c:872-878

count_vm_tlb_event(NR_TLB_REMOTE_FLUSH);
// 通过 smp_call_function_many() 发送 IPI
```

**TLB Shootdown 的代价**：IPI 延迟 1-5μs + 中断处理 + 同步等待。这就是为什么在多核上频繁 `mmap`/`munmap`、大量 COW 缺页会导致性能下降。

---

## 6. 内核软件页表遍历——缺页处理

当硬件 page walk 发现某一级页表项为空（或权限不足），触发 **Page Fault (#PF)**。内核在 `__handle_mm_fault()` 中用软件逐级查找/分配页表：

```c
// mm/memory.c:5124-5290

static vm_fault_t __handle_mm_fault(struct vm_area_struct *vma,
        unsigned long address, unsigned int flags)
{
    pgd = pgd_offset(mm, address);              // 从 mm->pgd 定位 PGD 项

    p4d = p4d_alloc(mm, pgd, address);          // P4D（4级时折叠为 PGD）
    if (!p4d) return VM_FAULT_OOM;

    vmf.pud = pud_alloc(mm, p4d, address);      // 按需分配 PUD 页表页
    if (!vmf.pud) return VM_FAULT_OOM;

    vmf.pmd = pmd_alloc(mm, vmf.pud, address);  // 按需分配 PMD 页表页
    if (!vmf.pmd) return VM_FAULT_OOM;

    return handle_pte_fault(&vmf);               // PTE 层：分配物理页、建映射
}
```

`*_alloc` 函数：如果该级页表页不存在，调用 `alloc_page` 分配一个 4KB 物理页作为页表，填入上级页表项。**这正是"每级恰好 1 页"设计的好处——直接用伙伴系统分配**。

---

## 7. 各级 offset 宏与函数

```c
// include/linux/pgtable.h:83-133

#define pgd_offset(mm, address)  pgd_offset_pgd((mm)->pgd, (address))
// pgd_index = (addr >> 39) & 511

static inline pud_t *pud_offset(p4d_t *p4d, unsigned long address)
{   return p4d_pgtable(*p4d) + pud_index(address); }  // (addr >> 30) & 511

static inline pmd_t *pmd_offset(pud_t *pud, unsigned long address)
{   return pud_pgtable(*pud) + pmd_index(address); }  // (addr >> 21) & 511

static inline pte_t *pte_offset_kernel(pmd_t *pmd, unsigned long address)
{   return (pte_t *)pmd_page_vaddr(*pmd) + pte_index(address); }  // (addr >> 12) & 511
```

P4D 在 4 级模式下折叠：

```c
// arch/x86/include/asm/pgtable.h:902-932

static inline p4d_t *p4d_offset(pgd_t *pgd, unsigned long address)
{
    if (!pgtable_l5_enabled())
        return (p4d_t *)pgd;   // 4 级：P4D 等于 PGD 本身
    return (p4d_t *)pgd_page_vaddr(*pgd) + p4d_index(address);
}
```

---

## 8. 4 级 vs 5 级页表

```c
// arch/x86/Kconfig:383-388

config PGTABLE_LEVELS
    default 5 if X86_5LEVEL    // Intel Ice Lake+，需要 LA57 特性
    default 4 if X86_64        // 标准 4 级
```

| 特性 | 4 级 | 5 级 |
|------|------|------|
| 虚拟地址 | 48 位 (256 TB) | 57 位 (128 PB) |
| 页表路径 | PGD→PUD→PMD→PTE | PGD→P4D→PUD→PMD→PTE |
| TLB miss 开销 | 4 次内存访问 | 5 次 |

**设计权衡**：5 级扩大了寻址范围，但每次 TLB miss 多一次内存访问。绝大多数场景 256TB 够用，所以 5 级只在特定 CPU 和内核配置下启用。4 级模式下 P4D 被"折叠"——软件上透明，硬件上不存在。

---

# 第二部分：CPU 缓存——物理地址到数据的高速通路

## 9. Cache 与 TLB 做的事完全不同

一个常见误解是混淆 TLB 和 Cache。它们是流水线上**串联的两个阶段**，解决不同的问题：

```
TLB 解决的问题:   虚拟地址翻译慢 → 缓存翻译结果（VPN → PFN）
Cache 解决的问题: 内存访问慢     → 缓存数据内容（物理地址 → 数据）
```

```
CPU 发出虚拟地址
       │
  ┌────▼────┐
  │  TLB    │  输入: 虚拟页号    输出: 物理页帧号
  └────┬────┘
       │ 物理地址
  ┌────▼────┐
  │  Cache  │  输入: 物理地址    输出: 实际数据
  └────┬────┘
       ▼
  数据到 CPU 寄存器
```

关键区别：
- **TLB** 按虚拟地址索引，**需要区分进程**（PCID 或刷新）
- **Cache** 按物理地址索引（PIPT），**不需要区分进程**——物理地址是全局唯一的

---

## 10. 缓存层级架构——每层是谁的、存什么

```
┌─────────────────────────────────────────────────────────────┐
│                    CPU Core 0                                │
│  ┌────────────────────────────────────────────────────────┐  │
│  │ 寄存器    容量: 几百字节   延迟: ~0.3ns (1 cycle)       │  │
│  └───────────────────────┬────────────────────────────────┘  │
│  ┌───────────────────────▼────────────────────────────────┐  │
│  │ L1 Cache  L1i(指令)+L1d(数据)   32-64KB   ~1ns        │  │ 核心私有
│  └───────────────────────┬────────────────────────────────┘  │
│  ┌───────────────────────▼────────────────────────────────┐  │
│  │ L2 Cache              256KB-1MB           ~3-5ns       │  │ 核心私有
│  └───────────────────────┬────────────────────────────────┘  │
└──────────────────────────┼───────────────────────────────────┘
┌──────────────────────────▼───────────────────────────────────┐
│ L3 Cache (LLC)           8-64MB              ~10-20ns       │  所有核共享
└──────────────────────────┬───────────────────────────────────┘
┌──────────────────────────▼───────────────────────────────────┐
│ DRAM                     GB 级               ~60-100ns      │
└──────────────────────────────────────────────────────────────┘
```

**关键数字**：L1 到 DRAM 延迟差 **100 倍**。一次 cache miss 的等待时间够 L1 命中 100 次。

**为什么 L1/L2 核心私有、L3 共享**：
- L1/L2 私有保证了每核的访问速度（无仲裁开销）
- L3 共享让多核间共享热数据（如共享库代码），且作为最后一道屏障减少 DRAM 访问

**Cache 中存的是什么**（不是地址映射，是实际数据）：

```
L1d Cache 中一条 cache line:
┌──────────────────┬──────────────────────────────────────┐
│ Tag (物理地址高位) │ 64 字节数据（从内存搬来的实际值）       │
├──────────────────┼──────────────────────────────────────┤
│ 0x3_4567_8       │ [42, 0, 17, 255, ...]                │
└──────────────────┴──────────────────────────────────────┘
没有 "进程ID" 字段 —— Cache 只认物理地址
```

---

## 11. Cache Line——为什么一次加载 64 字节

### 要解决的问题

CPU 读 `int x = array[100];` 只需要 4 字节。如果 cache 每次也只从内存搬 4 字节，访问 `array[101]` 时又要搬一次。能不能把附近的数据一起搬来？

### 解决方案：Cache Line = 64 字节批量加载

```c
// arch/x86/include/asm/cache.h:8-9

#define L1_CACHE_SHIFT  (CONFIG_X86_L1_CACHE_SHIFT)  // 通常为 6
#define L1_CACHE_BYTES  (1 << L1_CACHE_SHIFT)         // = 64
```

Cache 的最小传输/存储单位是一条 **cache line = 64 字节**。不管你读 1 字节还是 8 字节，miss 时都从下一级加载**整条 64 字节**。

```
你想要:     array[100] 这 4 字节 (地址 0x190)
实际加载:   0x180 ~ 0x1BF 这 64 字节 (64字节对齐)

内存: ... │0x180│0x184│...│0x190│...│0x1B8│0x1BC│ ...
          ←────────── 一条 cache line (64B) ──────────→
                              ↑
                        你要的 4 字节
```

### 为什么是 64 字节（而不是 32 或 128）

**1. 匹配 DRAM burst**：DDR4/5 的 burst length = 8，每次传 8×8 = 64 字节。传 1 字节和传 64 字节的延迟几乎相同——延迟瓶颈在地址传输和 CAS 等待（~60ns），数据传输本身只有几 ns。

**2. 空间局部性红利**：64 字节装 16 个 `int`，顺序遍历时一次 miss 换来 15 次 hit，miss 率从 100% 降到 6.25%。

**3. 不能太大**：128 字节 cache line 会浪费带宽（如果只用其中 4 字节），也会加剧 false sharing。64 字节是效率和浪费之间的工程平衡。

---

## 12. 缓存查找过程——Tag / Set Index / Offset

以 32KB、8-way 组相联 L1d 为例：

```
物理地址拆分:
63              12  11        6  5      0
┌────────────────┬────────────┬─────────┐
│     Tag        │  Set Index │ Offset  │
│  (高位比较用)   │  (6 bit)   │ (6 bit) │
└────────────────┴────────────┴─────────┘

64 组 (sets) × 8 路 (ways) × 64B (cache line) = 32KB
```

查找过程：
1. **Offset** (6 bit)：cache line 内第几个字节
2. **Set Index** (6 bit)：选中 64 组中的哪一组
3. **Tag**：与该组内 8 条 cache line 的 tag **并行比较**
   - 匹配 → **命中**，返回数据
   - 全不匹配 → **miss**，从下一级加载整条 64B，按 LRU 驱逐一条旧 line

---

## 13. MESI 协议——多核如何保证缓存一致性

### 要解决的问题

L1/L2 核心私有意味着同一物理地址的数据可能在多个核的 cache 中各有一份。如果 Core 0 修改了数据，Core 1 还用旧值就会出错。

### 解决方案：MESI 状态机

每条 cache line 附带一个状态标记：

| 状态 | 含义 | 读 | 写 |
|------|------|----|----|
| **M** (Modified) | 本核独占，已修改，与内存不一致 | 直接读 | 直接写 |
| **E** (Exclusive) | 本核独占，与内存一致 | 直接读 | 改为 M 后写 |
| **S** (Shared) | 多核共享，只读 | 直接读 | 须先使其他核 Invalid |
| **I** (Invalid) | 无效 | 须重新获取 | 须重新获取 |

**关键代价**：当 Core 0 要写一条 Shared 状态的 line 时，必须通过总线广播 Invalidate 消息，等其他核确认失效后才能写。这就是 **cache line bouncing**——理解它是理解后面 false sharing 问题的前提。

---

## 14. 上下文切换时 Cache 会失效吗——PIPT 的设计智慧

### 答案：不会。Cache 用物理地址索引，不需要也不会刷新

```
进程 A: 虚拟地址 0x400000 → 物理地址 0x1_0000_0000 → Cache tag=0x1_0000_0
进程 B: 虚拟地址 0x400000 → 物理地址 0x2_0000_0000 → Cache tag=0x2_0000_0
                  ↑                                       ↑
            虚拟地址相同                              物理地址不同，不冲突
```

**PIPT 的设计智慧**：Cache 完全不知道"进程"的概念，只认物理地址。这让上下文切换时 Cache 的处理极其简单——什么都不用做。

另一个好处：如果两个进程共享同一物理页（如 libc.so），它们在 Cache 中命中同一条 line，避免重复缓存。

### 间接代价：cache warm-up

虽然不刷新，但新进程的工作集不同，旧数据会被 LRU 逐渐淘汰。**频繁切换的真正代价是两个进程反复把对方的热数据挤出 cache**（cache thrashing），不是硬件刷新。

同进程的线程切换开销更小——共享 `mm_struct` 意味着不切 CR3（无 TLB 开销），且线程间共享大量代码和数据，cache 命中率几乎不下降。

### 完整对比总表

| 缓存 | 归属 | 索引方式 | 区分进程 | 切换时刷新 |
|------|------|---------|---------|-----------|
| TLB (L1/L2) | 核心私有 | 虚拟地址+PCID | 是 | 无PCID时全刷 |
| L1i / L1d | 核心私有 | 物理地址 (PIPT) | 不需要 | 不刷 |
| L2 Cache | 核心私有 | 物理地址 (PIPT) | 不需要 | 不刷 |
| L3 Cache | 所有核共享 | 物理地址 (PIPT) | 不需要 | 不刷 |

---

# 第三部分：理解 Cache Miss——为什么你的程序变慢了

## 15. Cache Miss 的三种类型

| 类型 | 原因 | 能否避免 |
|------|------|---------|
| **Compulsory (冷启动)** | 数据首次被访问，不可能在 cache 中 | 不能（但可通过预取隐藏延迟） |
| **Capacity (容量)** | 工作集超过 cache 容量 | 减小工作集，或使用更大 cache 级别 |
| **Conflict (冲突)** | 多个地址映射到同一 cache set | 改善数据对齐/布局 |

还有一种多线程独有的：
| **Coherence (一致性)** | 其他核写入导致本核的 line 被 Invalidate（false sharing） | 消除共享 |

**程序员能优化的主要是后三种**，核心思路只有一条：**让每条 cache line 中的有效数据尽可能多**。

---

## 16. 访问步长与命中率的关系——一个核心心智模型

**这是理解所有缓存优化的关键心智模型**：

一条 cache line = 64 字节。每次 miss 加载 64 字节。你实际用了其中多少字节，就是这条 line 的"有效利用率"。

```
步长 (stride)    每条 line 用了多少    miss 率         有效利用率
──────────────────────────────────────────────────────────────
4 字节 (int)     64/4 = 16 次命中      1/16 = 6.25%    100%
8 字节 (long)    64/8 = 8 次命中       1/8 = 12.5%     100%
16 字节          64/16 = 4 次命中      1/4 = 25%       100%
64 字节          1 次命中              100%             正好用完
128 字节         0.5 次命中            100%             只用了一半
4096 字节        0.016 次命中          100%             只用了 1/64
```

**规律**：步长 ≤ 64 字节时，cache line 总能发挥作用；步长 > 64 字节时，每条加载的 line 大部分浪费了。

把这个规律记住，下面所有案例都是它的应用。

---

## 17. 实战案例一：矩阵遍历——行优先 vs 列优先

C 语言二维数组在内存中按**行**连续存储。`matrix[0][0]` 和 `matrix[0][1]` 是相邻的 4 字节。

```c
int matrix[1024][1024];  // 每行 1024 × 4 = 4096 字节
```

**行优先遍历**（步长 = 4 字节）：

```c
for (int i = 0; i < 1024; i++)
    for (int j = 0; j < 1024; j++)
        sum += matrix[i][j];

// matrix[0][0] miss → 加载 64B → matrix[0][0]~[0][15] 全在 line 里
// matrix[0][1]~[0][15]: 命中！
// matrix[0][16]: miss → 加载 64B
// ...
// miss 率 = 1/16 = 6.25%
```

**列优先遍历**（步长 = 4096 字节）：

```c
for (int j = 0; j < 1024; j++)
    for (int i = 0; i < 1024; i++)
        sum += matrix[i][j];

// matrix[0][0] miss → 加载 64B → matrix[0][0]~[0][15]
// matrix[1][0] miss → 加载 64B → matrix[1][0]~[1][15]
//   跳了 4096 字节，上一条 line 中 15 个 int 全浪费了
// ...
// miss 率 ≈ 100%，有效利用率 = 4/64 = 6.25%
```

**性能差距 10-50 倍**。原因纯粹是步长——行优先步长 4B（<64B，line 充分利用），列优先步长 4096B（>>64B，line 几乎全浪费）。

---

## 18. 实战案例二：数据结构布局——让有用的数据挨在一起

### 问题：结构体太大，热数据被冷数据稀释

```c
struct node_bad {
    char   name[256];    // 冷数据（偶尔用）
    int    key;          // 热数据（每次遍历都用）
    char   desc[512];    // 冷数据
    double value;        // 热数据
};  // sizeof ≈ 780 字节

// 遍历 10000 个 node 时：
// 每个 node 跨 780/64 ≈ 12 条 cache line
// 但只需要 key(4B) + value(8B) = 12B
// 有效利用率 = 12/780 = 1.5%
```

### 解决方案：冷热分离

```c
struct node_hot {
    int    key;
    double value;
};  // sizeof = 12 字节，一条 cache line 放 5 个

struct node_cold {
    char name[256];
    char desc[512];
};  // 只在需要时才访问
```

遍历 `node_hot` 数组时有效利用率接近 100%。

### SoA vs AoS：同样的思路

```c
// AoS: 遍历所有 x 时，y 和 z 浪费 cache 空间
struct Point { float x, y, z; };   // 步长 12B，利用率 4/12=33%
struct Point points[N];

// SoA: 只访问 x 时，整条 cache line 都是 x
struct Points { float x[N]; float y[N]; float z[N]; };
// 步长 4B，利用率 100%
```

### 内核中的实践

`struct sock` 使用 `____cacheline_aligned_in_smp` 将发送路径和接收路径的热字段分组到不同的 cache line，避免收发路径相互干扰。`struct net_device` 同理。

```c
// include/linux/cache.h:42-50
#define ____cacheline_aligned_in_smp ____cacheline_aligned
// 展开为: __attribute__((__aligned__(64)))
```

---

## 19. 实战案例三：多线程 false sharing——看不见的性能杀手

### 问题场景

```c
int counters[4];  // 4 个 int = 16 字节，全在一条 cache line 上

// 线程 0 频繁写 counters[0]
// 线程 1 频繁写 counters[1]
// 它们操作的是不同变量，逻辑上没有共享
```

**但它们在同一条 cache line 上**。每次 Thread 0 写 `counters[0]`，MESI 协议必须 Invalidate Thread 1 核上的那条 line；反之亦然。两个核不停地把同一条 line 抢来抢去（cache line bouncing），实际性能可能比单线程还差。

### 为什么叫"false" sharing

两个线程并没有真正共享数据（各写各的变量），但因为这些变量恰好在同一条 64 字节 cache line 里，硬件一致性协议认为它们在"共享"，产生了不必要的一致性开销。

### 解决方案：让每个被独立写的变量占满一条 cache line

```c
struct padded_counter {
    int value;
    char _pad[60];                              // 填充到 64B
} __attribute__((aligned(64)));

struct padded_counter counters[4];              // 每个 counter 独占一条 line
```

### 怎么发现 false sharing

- `perf c2c record ./program && perf c2c report`：专门检测跨核 cache line 争用
- 症状：多线程性能不升反降，或随核数增加提升极少

---

## 20. 实战案例四：链表 vs 数组——指针追逐的代价

### 问题：链表节点在内存中位置随机

```c
// 链表遍历：每个 node->next 可能在内存任意位置
for (node = head; node; node = node->next)
    process(node);

// node 0 在地址 0x1000 → miss → 加载 64B
// node 1 在地址 0x8000 → miss → 加载 64B (上一条 line 全浪费)
// node 2 在地址 0x3000 → miss → ...
// 步长不可预测 → 硬件预取器无法工作 → 几乎每次都 miss
// 时间 ≈ N × 80ns (DRAM 延迟)
```

```c
// 数组遍历：连续内存，步长固定
for (int i = 0; i < n; i++)
    process(&array[i]);

// 步长 = sizeof(element)，通常 << 64B → cache line 充分利用
// 硬件预取器检测到规律步长，提前加载后续 line
// 时间 ≈ N × 1ns (L1 命中)
```

### 为什么链表对 cache 如此不友好

1. **步长不可预测**：每个 `next` 指向的地址和前一个节点没有空间关系
2. **硬件预取器失效**：预取器依赖规律的步长模式，链表的随机跳转让它无法工作
3. **延迟无法隐藏**：必须拿到 `node->next` 的值才能发出下一次访问请求，访问严格串行

### 内核中的应对策略

链表在内核中无法避免（灵活性需要），但内核用多种手段缓解：
- `prefetch(pos->next)`：在处理当前节点时预取下一个
- 红黑树 + 链表双索引（如 VMA 的 `mm_rb` + `mmap` 链表）：减少链表遍历
- Slab 分配器：同类型对象从同一 slab 分配，节点在物理上相对聚集

---

## 21. 内核中的缓存优化实践

### likely/unlikely——icache 优化

```c
// include/linux/compiler.h
#define likely(x)    __builtin_expect(!!(x), 1)
#define unlikely(x)  __builtin_expect(!!(x), 0)
```

编译器将热路径代码连续排列在 `.text` 段，冷路径移到 `.text.unlikely` 段。热路径紧凑 → 更少的 icache miss。

### prefetch——软件预取

```c
// include/linux/prefetch.h
#define prefetch(x) __builtin_prefetch(x)
```

在网络包处理、链表遍历等热路径中大量使用，提前把即将访问的数据拉入 cache，隐藏内存延迟。

### ____cacheline_aligned——对齐与分组

内核大量使用此宏确保热字段在同一 cache line、不同用途的字段在不同 line：

```c
struct some_struct {
    /* 读多写少的字段组 */
    ...
    ____cacheline_aligned_in_smp
    /* 频繁写的字段组 —— 独占 cache line，避免与上面的读路径 false sharing */
    ...
};
```

---

## 22. 度量工具——用 perf 验证你的优化

不要猜测，用数据说话：

```bash
# cache miss 总览
perf stat -e cache-misses,cache-references,L1-dcache-load-misses ./my_program

# TLB miss（考虑是否需要 Huge Pages）
perf stat -e dTLB-load-misses,dTLB-store-misses ./my_program

# 分支预测失败（考虑 likely/unlikely）
perf stat -e branch-misses,branch-instructions ./my_program

# false sharing 检测
perf c2c record ./my_program && perf c2c report
```

### 优化效果参考

| 场景 | 优化手段 | 本质 | 典型提升 |
|------|---------|------|---------|
| 矩阵遍历 | 行优先 | 步长 4B→利用率100% | 10-50x |
| 大 struct 遍历 | 冷热分离 / SoA | 减少无效数据 | 2-5x |
| 多线程写 | cache line 对齐填充 | 消除 false sharing | 2-10x |
| 容器遍历 | 数组替代链表 | 固定步长+预取 | 3-10x |
| 不规则访问 | 软件 prefetch | 隐藏延迟 | 1.2-2x |
| 高频分支 | likely/unlikely | icache 紧凑 | 1.1-1.5x |

---

> **后续阅读**：
> - [CPU 上下文切换与内核栈](CPU上下文切换与内核栈.md) — 进程切换时 CR3/TLB 的处理、RSP 切换、内核栈布局
> - [内核内存对齐机制](内核内存对齐机制.md) — cache line 对齐在 slab/DMA/原子操作中的实际运用
