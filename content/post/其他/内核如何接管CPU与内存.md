+++
date = '2026-06-01'
title = 'Linux 内核如何接管 CPU 与内存——从上电到完全掌控硬件'
weight = 2
tags = [
    "启动",
    "CPU接管",
    "内存接管",
    "CR0",
    "CR3",
    "CR4",
    "GDT",
    "IDT",
    "e820",
    "memblock",
    "伙伴系统",
    "SMP",
    "页表",
]
categories = [
    "其他",
]
+++
# Linux 内核如何接管 CPU 与内存

> 基于 Linux 5.15.78 内核源码，以 x86_64 架构为主线。
>
> 前置阅读：[内核整体架构](内核整体架构.md) — `start_kernel()` 各阶段概览

---

## 一、为什么需要"接管"——理解问题的本质

电源按下后，CPU 从一个**极其原始**的状态开始运行：

- **16 位实模式**（Real Mode）：只能寻址 1MB 内存，没有内存保护，没有虚拟地址
- **没有页表**：CPU 直接用物理地址访问内存
- **没有中断表**：CPU 不知道出错了该找谁处理
- **没有多任务能力**：谈不上进程调度
- **不知道物理内存有多大**：CPU 只知道自己能发出地址信号，不知道哪些地址有 DRAM 芯片在响应

**内核的"接管"就是把 CPU 从这个原始状态，一步步配置成现代操作系统需要的状态**：

```
上电时的 CPU 状态                              内核接管后的 CPU 状态
────────────────                              ────────────────────
16位实模式                                     64位长模式（Long Mode）
物理地址直接访问                                虚拟地址 + 4级页表翻译
没有内存保护                                   Ring 0/Ring 3 特权级隔离
没有中断处理表                                  完整的 IDT，每种异常/中断有专门处理函数
不知道有多少内存                                已探测物理内存布局，建立完整分配管理体系
单个 CPU 核心在跑                               所有 CPU 核心启动，各有独立调度队列
```

**这不是"一步到位"的事，而是一个渐进的过程**——内核像搭积木一样，先搭好最底层的（CPU 模式切换），才能搭上面的（内存管理），再搭更上面的（进程调度）。

---

## 二、CPU 接管：从 16 位实模式到 64 位长模式

### 2.1 接管的核心：配置 CPU 的控制寄存器

CPU 的行为由几个**控制寄存器**决定。配置这些寄存器就是告诉 CPU "你现在该用什么模式工作"。

```
┌─────────────────────────────────────────────────────────────────┐
│                    CPU 控制寄存器（x86_64）                       │
├──────────┬──────────────────────────────────────────────────────┤
│  CR0     │ 控制 CPU 基本工作模式                                 │
│          │   PE 位 = 1 → 进入保护模式（有特权级，能隔离用户/内核）│
│          │   PG 位 = 1 → 启用分页（虚拟地址→物理地址翻译）       │
│          │   WP 位 = 1 → 内核也不能随意写只读页（COW 的基础）    │
├──────────┼──────────────────────────────────────────────────────┤
│  CR3     │ 存放顶级页表（PGD）的物理地址                         │
│          │ CPU 的 MMU 从这里开始遍历页表做地址翻译               │
│          │ 进程切换时，切换 CR3 = 切换地址空间                    │
├──────────┼──────────────────────────────────────────────────────┤
│  CR4     │ 控制 CPU 扩展特性                                     │
│          │   PAE 位 = 1 → 启用物理地址扩展（64位必需）           │
│          │   PGE 位 = 1 → 启用全局页（内核页表项不随 CR3 切换失效）│
│          │   LA57 位     → 启用 5 级页表（可选）                  │
├──────────┼──────────────────────────────────────────────────────┤
│  MSR_EFER│ 扩展功能寄存器                                        │
│          │   LME 位 = 1 → 启用长模式（64位模式的前提）           │
│          │   SCE 位 = 1 → 启用 syscall/sysret 指令              │
├──────────┼──────────────────────────────────────────────────────┤
│  GDTR    │ 指向 GDT（段描述符表），定义内核段/用户段的基址和权限   │
├──────────┼──────────────────────────────────────────────────────┤
│  IDTR    │ 指向 IDT（中断描述符表），定义每种中断/异常的处理函数   │
└──────────┴──────────────────────────────────────────────────────┘
```

### 2.2 接管过程分三步走

```
┌─────────────────────────────────────────────────────────────────────┐
│  第一步：BIOS/引导加载器阶段（内核被加载到内存）                       │
│                                                                     │
│  BIOS 上电自检 → 从磁盘加载 GRUB → GRUB 加载内核映像（bzImage）       │
│  此时 CPU 还在实模式或 32 位保护模式                                  │
│                                                                     │
│  关键点：BIOS 已经通过 int 15h/e820 中断，探测出了物理内存布局         │
│         这个信息存在 boot_params 结构中，后面内核会用到                │
└─────────────────────────────────┬───────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  第二步：内核解压缩阶段（第一次启用分页和长模式）                       │
│                                                                     │
│  arch/x86/boot/compressed/head_64.S                                │
│                                                                     │
│  ① CR4.PAE = 1        启用物理地址扩展                               │
│  ② 建立临时恒等映射     让虚拟地址 = 物理地址（刚开分页时必须这样）    │
│  ③ CR3 = 临时页表       告诉 MMU 页表在哪                            │
│  ④ EFER.LME = 1       启用长模式                                    │
│  ⑤ CR0 = PG | PE      同时开启分页和保护模式                         │
│  → CPU 进入 64 位长模式，开始执行解压代码，解压内核                   │
└─────────────────────────────────┬───────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  第三步：解压后的内核正式启动                                         │
│                                                                     │
│  arch/x86/kernel/head_64.S → head64.c → start_kernel()             │
│                                                                     │
│  ① 修正页表（恒等映射 + 内核高地址映射）                               │
│  ② 重新配置 CR4（PAE + PGE）/ CR3 / GDT / IDT                      │
│  ③ 配置 CR0 的最终值（加上 WP 写保护等位）                           │
│  ④ 跳转到 C 语言入口 x86_64_start_kernel()                         │
│  → CPU 现在工作在完整的 64 位保护模式下，内核已经接管了 CPU            │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.3 源码实证：内核怎么配置 CPU

**第一次启用分页和长模式**（解压缩阶段）：

```
arch/x86/boot/compressed/head_64.S 做了什么：

    /* ① 启用 PAE（物理地址扩展，64位必需） */
    movl    %cr4, %eax
    orl     $X86_CR4_PAE, %eax        ← 把 CR4 的 PAE 位置 1
    movl    %eax, %cr4

    /* ② 加载临时页表 */
    leal    rva(pgtable)(%ebx), %eax  ← 临时页表的物理地址
    movl    %eax, %cr3                ← 写入 CR3，MMU 从此查这张表

    /* ③ 启用长模式 */
    movl    $MSR_EFER, %ecx
    rdmsr
    btsl    $_EFER_LME, %eax          ← EFER 的 LME 位置 1
    wrmsr

    /* ④ 同时开启分页和保护模式 → CPU 进入 64 位 */
    movl    $(X86_CR0_PG | X86_CR0_PE), %eax
    movl    %eax, %cr0                ← 这一条指令后，CPU 进入长模式
```

**内核正式接管 CPU**（`startup_64` → `secondary_startup_64`）：

```
arch/x86/kernel/head_64.S 做了什么：

    /* ① 配置 CR4：PAE + 全局页 */
    movl    $(X86_CR4_PAE | X86_CR4_PGE), %ecx
    movq    %rcx, %cr4

    /* ② 加载内核页表 */
    movq    %rax, %cr3                ← 切换到内核自己的页表

    /* ③ 加载 GDT（段描述符表） */
    lgdt    early_gdt_descr(%rip)     ← 告诉 CPU 代码段/数据段的权限定义

    /* ④ 加载 IDT（中断描述符表） */
    call    early_setup_idt           ← 告诉 CPU 每种异常/中断由谁处理

    /* ⑤ 配置 EFER（启用 syscall 指令） */
    movl    $MSR_EFER, %ecx
    rdmsr
    btsl    $_EFER_SCE, %eax          ← 启用 syscall/sysret 快速系统调用
    wrmsr

    /* ⑥ 配置 CR0 最终值 */
    movl    $CR0_STATE, %eax
    movq    %rax, %cr0
```

其中 `CR0_STATE` 是内核定义的 CR0 最终配置：

```
CR0_STATE = PE | MP | ET | NE | WP | AM | PG

PE = 保护模式          MP = 监控协处理器
ET = 扩展类型          NE = 数值异常
WP = 写保护（内核也不能写只读页 → COW 的硬件基础）
AM = 对齐检查          PG = 分页
```

**进入 C 语言后**（`arch/x86/kernel/head64.c`）：

```
x86_64_start_kernel() 做了什么：

    cr4_init_shadow()           ← 读取 CR4 到 per-CPU 变量（供后续查询）
    reset_early_page_tables()   ← write_cr3(early_top_pgt)
    clear_bss()                 ← 清零 BSS 段
    idt_setup_early_handler()   ← 为所有异常向量安装临时处理函数
    → x86_64_start_reservations()
        → start_kernel()        ← 进入内核主初始化流程
```

### 2.4 GDT 和 IDT：CPU 的"规则手册"

**GDT（全局描述符表）**告诉 CPU "内存段的边界和权限"。x86_64 下段寄存器的作用已弱化（flat 模型），但 GDT 仍然必须设置，因为：
- CPU 需要区分 Ring 0（内核）和 Ring 3（用户态）的代码段/数据段
- `syscall`/`sysret` 指令依赖 GDT 中的段描述符

```
内核启动时的 GDT 条目（arch/x86/kernel/head64.c）：

static struct desc_struct startup_gdt[] = {
    [GDT_ENTRY_KERNEL32_CS] = GDT_ENTRY_INIT(0xc09b, 0, 0xfffff),   // 32位内核代码段
    [GDT_ENTRY_KERNEL_CS]   = GDT_ENTRY_INIT(0xa09b, 0, 0xfffff),   // 64位内核代码段
    [GDT_ENTRY_KERNEL_DS]   = GDT_ENTRY_INIT(0xc093, 0, 0xfffff),   // 内核数据段
};

加载方式：lgdt early_gdt_descr(%rip)
→ 把 GDT 的地址和大小告诉 CPU 的 GDTR 寄存器
→ CPU 此后根据段选择子在这张表里查权限
```

**IDT（中断描述符表）**告诉 CPU "每种异常/中断发生时跳转到哪个函数"：

```
内核的 IDT 初始化分多个阶段（arch/x86/kernel/idt.c）：

阶段 1: idt_setup_early_handler()     ← x86_64_start_kernel 中
         为所有 256 个向量安装 early_idt_handler_array 中的临时处理函数

阶段 2: idt_setup_early_traps()       ← setup_arch 中
         安装真正的异常处理函数（如 #DE 除零、#GP 一般保护）

阶段 3: idt_setup_early_pf()          ← init_mem_mapping 之后
         安装缺页异常（#PF）处理函数（在页表初始化完成后才能安装）

阶段 4: idt_setup_apic_and_irq_gates() ← 完整 IDT
         安装所有硬件中断和 APIC 相关的处理函数

加载方式：lidt idt_descr
→ 把 IDT 的地址和大小告诉 CPU 的 IDTR 寄存器
→ CPU 此后遇到异常/中断就查这张表找处理函数
```

### 2.5 "接管 CPU"到底意味着什么

总结一下，**内核接管 CPU 的本质就是配置 5 样东西**：

```
┌─────────────────────────────────────────────────────────────────┐
│  1. CR0：启用保护模式 + 分页 + 写保护                             │
│     → CPU 从"裸奔"变成"有规矩"——区分内核态/用户态，使用虚拟地址   │
│                                                                 │
│  2. CR3：指向内核页表                                             │
│     → CPU 的 MMU 知道了"虚拟地址→物理地址"的翻译规则              │
│                                                                 │
│  3. CR4 + EFER：启用 64 位长模式和各种扩展特性                     │
│     → CPU 能使用完整的 64 位地址空间和指令集                       │
│                                                                 │
│  4. GDT（通过 GDTR 加载）：定义内核/用户代码段和数据段              │
│     → CPU 知道了特权级的划分规则                                  │
│                                                                 │
│  5. IDT（通过 IDTR 加载）：定义异常和中断的处理函数                 │
│     → CPU 知道了出错/来中断时该跳到哪里执行内核代码                │
│                                                                 │
│  这 5 样配置好后，CPU 就完全按照内核的规则运行了。                  │
│  用户程序试图做越权操作 → CPU 硬件自动触发异常 → 跳转到内核代码     │
│  这就是"内核控制了 CPU"的含义。                                   │
└─────────────────────────────────────────────────────────────────┘
```

---

## 三、内存接管：从"不知道有多少内存"到"每一页都在管理之下"

### 3.1 接管的核心问题

内核面临的内存接管挑战：

```
问题 1：物理内存有多大？哪些地址范围是 RAM？哪些是 MMIO 设备空间？
         → 需要从 BIOS 获取 E820 内存布局图

问题 2：内核自己加载在哪里？不能把自己覆盖了
         → 需要预留（reserve）内核代码/数据占用的内存

问题 3：虚拟地址系统还没建好，怎么分配内存？
         → 需要一个极简的早期分配器（memblock）先顶着

问题 4：最终的内存管理体系（伙伴系统）什么时候建好？
         → 在 memblock 完成历史使命后，把所有空闲内存移交给伙伴系统
```

### 3.2 完整的接管时间线

```
时间轴 ──────────────────────────────────────────────────────────────→

BIOS 阶段         内核启动阶段              start_kernel()
│                 │                         │
│ ① BIOS 探测    │ ② 建临时页表            │ ③ 读取 E820
│   物理内存布局   │    启用分页              │   填充 memblock
│   (int 15h/e820)│                         │
│   存入          │                         │ ④ 预留内核自身
│   boot_params   │                         │   占用的内存
│                 │                         │
│                 │                         │ ⑤ 建立内核页表
│                 │                         │   映射全部物理内存
│                 │                         │   切换到 swapper_pg_dir
│                 │                         │
│                 │                         │ ⑥ 初始化 Zone 元数据
│                 │                         │   (ZONE_DMA, ZONE_NORMAL...)
│                 │                         │
│                 │                         │ ⑦ memblock_free_all()
│                 │                         │   把所有空闲页移交伙伴系统
│                 │                         │   内存管理体系正式就绪
│                 │                         │
│                 │                         │ ⑧ 初始化 Slab 分配器
│                 │                         │   kmalloc 可以用了
│                 │                         │
│                 │                         │ ⑨ 初始化 vmalloc 区域
│                 │                         │   vmalloc 可以用了
│                 │                         │
│─── 没有内存管理 ─│── memblock 早期分配器 ──│── 伙伴系统 + Slab ──→
```

### 3.3 第一步：获取物理内存布局（E820）

在 x86 上，BIOS 通过 `int 15h, eax=E820h` 中断提供物理内存布局。引导加载器调用这个中断，结果存在 `boot_params.e820_table` 中。

**E820 表告诉内核：哪段物理地址是 RAM，哪段是设备空间，哪段被保留**：

```
典型的 E820 表（每台机器不同）：

物理地址范围                    类型              含义
──────────────────────────   ─────────────    ──────────────
0x0000_0000 ~ 0x0009_FBFF    E820_RAM         低 640KB 常规内存
0x0009_FC00 ~ 0x0009_FFFF    E820_RESERVED    BIOS 扩展数据区
0x000A_0000 ~ 0x000F_FFFF    E820_RESERVED    显存 + ROM + BIOS
0x0010_0000 ~ 0x7FEF_FFFF    E820_RAM         ★ 主要内存区域（~2GB）
0x7FF0_0000 ~ 0x7FFF_FFFF    E820_ACPI/NVS    ACPI 表
0xFEC0_0000 ~ 0xFEC0_0FFF    E820_RESERVED    I/O APIC
0xFEE0_0000 ~ 0xFEE0_0FFF    E820_RESERVED    Local APIC
0x1_0000_0000 ~ 0x2_7FFF_FFFF E820_RAM        ★ 4GB 以上的内存

注意：物理地址空间并不连续！中间有很多"空洞"是给设备用的。
内核必须知道这个布局，才能只管理真正的 RAM，不去碰设备空间。
```

内核读取 E820 的源码（`arch/x86/kernel/e820.c`）：

```
e820__memory_setup_default() 做了什么：

    char *who = "BIOS-e820";

    // 从 boot_params 中复制 E820 表
    append_e820_table(boot_params.e820_table, boot_params.e820_entries);

    // 排序、去重、合并
    e820__update_table(e820_table);

    // 打印内存布局到 dmesg（你在 dmesg 中看到的 "BIOS-e820" 行就来自这里）
```

### 3.4 第二步：memblock——内核的早期内存分配器

E820 告诉了内核有多少内存，但此时**伙伴系统还没有初始化**。内核需要一个极简的分配器来临时管理内存，这就是 `memblock`。

```
memblock 的设计极其简单：两个区域数组

┌─────────────────────────────────────────────────────────────┐
│  memblock.memory[]     ← 记录所有可用 RAM 区域               │
│                                                             │
│  [0] 0x0000_0000 ~ 0x0009_FBFF  (640KB)                    │
│  [1] 0x0010_0000 ~ 0x7FEF_FFFF  (~2GB)                     │
│  [2] 0x1_0000_0000 ~ 0x2_7FFF_FFFF  (~6GB)                 │
│                                                             │
├─────────────────────────────────────────────────────────────┤
│  memblock.reserved[]   ← 记录已被占用/预留的区域             │
│                                                             │
│  [0] 0x0000_0000 ~ 0x0000_FFFF  (低 64KB，实模式数据)       │
│  [1] 0x0100_0000 ~ 0x02FF_FFFF  (内核代码和数据)             │
│  [2] 0x0300_0000 ~ 0x031F_FFFF  (initrd)                    │
│  ... (还有页表、早期 per-CPU 等)                              │
│                                                             │
└─────────────────────────────────────────────────────────────┘

分配方式：从 memory[] 中找一段不在 reserved[] 中的区域，标记为 reserved
释放方式：从 reserved[] 中移除

就这么简单。不需要复杂的数据结构，因为启动阶段的分配次数很少。
```

E820 到 memblock 的转换（`arch/x86/kernel/e820.c`）：

```
e820__memblock_setup() 做了什么：

    // 遍历 E820 表，把 RAM 类型的区域添加到 memblock
    for (i = 0; i < e820_table->nr_entries; i++) {
        if (entry->type != E820_TYPE_RAM)
            continue;
        memblock_add(entry->addr, entry->size);  ← 告诉 memblock "这段是 RAM"
    }
```

内核自身占用的内存被预留（`arch/x86/kernel/setup.c`）：

```
early_reserve_memory() 做了什么：

    // 预留内核代码和数据段（不能被分配出去覆盖掉）
    memblock_reserve(__pa_symbol(_text),
                     __end_of_kernel_reserve - _text);

    // 预留低 64KB（实模式中断向量表等）
    memblock_reserve(0, SZ_64K);

    // 预留 initrd（如果有）
    early_reserve_initrd();
```

### 3.5 第三步：建立内核页表，映射全部物理内存

此时内核知道了物理内存布局，下一步是**建立页表，把所有物理 RAM 映射到内核的虚拟地址空间**。

```
为什么需要映射全部物理内存？

因为 CPU 启用分页后，所有代码（包括内核自己）都通过虚拟地址访问内存。
如果某段物理内存没有对应的页表映射，内核就没法访问那段内存。

x86_64 的内核地址空间布局（简化）：

  0xFFFF_FFFF_FFFF_FFFF  ┌──────────────────────────┐
                         │  ...                      │
  0xFFFF_8880_0000_0000  ├──────────────────────────┤
                         │  直接映射区               │ ← 这里映射全部物理 RAM
                         │  虚拟地址 = PAGE_OFFSET    │    虚拟地址 = 物理地址 + PAGE_OFFSET
                         │        + 物理地址          │    内核通过这个区域访问任意物理内存
  0xFFFF_8800_0000_0000  ├──────────────────────────┤ ← PAGE_OFFSET
                         │  vmalloc 区域             │
                         │  ...                      │
```

这个映射由 `init_mem_mapping()` 完成（`arch/x86/mm/init.c`）：

```
init_mem_mapping() 做了什么：

    // 逐段映射物理内存到内核虚拟地址空间
    // 先映射低 1MB（ISA 区域）
    init_memory_mapping(0, ISA_END_ADDRESS, PAGE_KERNEL);

    // 再映射所有 E820 标记为 RAM 的区域
    // 使用 2MB 或 1GB 大页来减少页表项数量

    // 最后，切换到内核自己的顶级页表
    load_cr3(swapper_pg_dir);    ← 把 CR3 指向内核页表
    __flush_tlb_all();           ← 刷新 TLB，使新页表生效

    // 从此时起，内核完全通过自己的页表访问内存
```

**`swapper_pg_dir` 就是内核的顶级页表（PGD）**。它映射了所有物理内存，在整个内核生命期中一直存在。每个用户进程的页表中，高半部分（内核地址空间）都共享这套映射。

### 3.6 第四步：初始化 Zone 和伙伴系统

物理内存被划分为多个 **Zone**（区域），因为不同的硬件有不同的内存需求：

```
物理内存地址空间（x86_64）：

  0                     16MB              4GB                    最大
  ├────── ZONE_DMA ──────┤                │                      │
  │  ISA DMA 设备只能用   │                │                      │
  │  这个范围的内存       │                │                      │
  ├──────────────────────┼── ZONE_DMA32 ──┤                      │
  │                      │  32位 DMA 设备  │                      │
  │                      │  能用的范围      │                      │
  ├──────────────────────┼────────────────┼──── ZONE_NORMAL ────┤
  │                      │                │  内核正常使用的内存   │
  │                      │                │  x86_64 上大部分 RAM │
  │                      │                │  都在这个 zone       │
  └──────────────────────┴────────────────┴──────────────────────┘
```

Zone 元数据的初始化由 `paging_init()` → `zone_sizes_init()` → `free_area_init()` 完成：

```
这一步做了什么：

① zone_sizes_init()  ← 计算每个 zone 的边界 PFN
② free_area_init()   ← 为每个 zone 初始化伙伴系统的 free_area[] 数组
                        此时 free_area 是空的——物理页还在 memblock 手里

所以这一步只是"搭好了伙伴系统的骨架"，还没有实际的空闲页。
```

### 3.7 第五步：memblock → 伙伴系统（权力交接）

这是内存接管中**最关键的一步**：`memblock_free_all()` 把所有空闲物理页从 memblock 移交给伙伴系统。

```
memblock_free_all() 的工作：

① 遍历 memblock.memory[] 中的每个 RAM 区域
② 对于每个区域中不在 memblock.reserved[] 里的部分
③ 按伙伴系统的 order 拆分，调用 __free_pages_core() 放入伙伴系统的 free_list

    memblock_free_all()                          [mm/memblock.c]
        → free_low_memory_core_early()
            → __free_memory_core(start, end)
                → __free_pages_memory(start_pfn, end_pfn)
                    → memblock_free_pages(pfn, order)
                        → __free_pages_core()    [mm/page_alloc.c]
                            → 放入 zone->free_area[order].free_list

完成后：
  - memblock 的使命结束，后续不再使用（标记为 __init 的内存会被回收）
  - 伙伴系统的 free_area[] 中有了实际的空闲页
  - alloc_pages() 可以正常工作了
  - totalram_pages 被更新为实际可用页数
```

这就好比公司交接：memblock 是创业时的临时会计，把所有账目移交给正式的财务部门（伙伴系统）后就退休了。

### 3.8 第六步：初始化 Slab 和 vmalloc

伙伴系统只能以"整页"为单位分配内存，内核还需要：

```
mm_init() 做了什么（init/main.c）：

    mem_init()           ← 调用 memblock_free_all()，完成上述交接
    kmem_cache_init()    ← 初始化 Slab 分配器
                            从此 kmalloc/kfree 可用
                            小对象（8B~8KB）从 Slab 缓存分配
    vmalloc_init()       ← 初始化 vmalloc 区域
                            从此 vmalloc/vfree 可用
                            大块虚拟连续（物理不连续）的内存

至此，内核的完整内存管理体系就绪：
  ┌────────────────────────────────────────────────────────┐
  │ 用户态 malloc → 系统调用 brk/mmap → VMA → 缺页 → 伙伴  │
  │ 内核 kmalloc → Slab → 伙伴                              │
  │ 内核 vmalloc → 页表映射 → 伙伴                           │
  │ 内核 alloc_pages → 伙伴系统                              │
  └────────────────────────────────────────────────────────┘
```

### 3.9 "接管内存"到底意味着什么

```
┌─────────────────────────────────────────────────────────────────┐
│  1. 知道有多少内存（E820）                                        │
│     → 从 BIOS 获取物理内存布局图                                  │
│                                                                 │
│  2. 预留关键区域（memblock_reserve）                               │
│     → 内核自身代码/数据/页表/initrd 不会被分配出去                 │
│                                                                 │
│  3. 建立页表映射（init_mem_mapping）                               │
│     → 内核能通过虚拟地址访问所有物理内存                           │
│                                                                 │
│  4. 建立分配管理体系（伙伴系统 + Slab + vmalloc）                  │
│     → 每一个物理页帧都被 struct page 描述                          │
│     → 所有空闲页在伙伴系统的 free_list 中管理                      │
│     → 分配和释放有严格的记账和追踪                                 │
│                                                                 │
│  从此，没有任何一个物理页是"无主"的——要么在使用中，要么在空闲链表中。│
│  任何人（包括用户程序）要用内存，都必须通过内核的分配接口来申请。    │
│  这就是"内核控制了内存"的含义。                                    │
└─────────────────────────────────────────────────────────────────┘
```

---

## 四、多 CPU 接管：把所有核心都叫醒

到目前为止，所有工作都在**一个 CPU 核心**（BSP，Bootstrap Processor）上完成。其他核心（AP，Application Processor）还在睡觉。

### 4.1 多核启动流程

```
┌─────────────────────────────────────────────────────────────────┐
│  BSP（Boot CPU）执行 start_kernel() 全部初始化                    │
│       │                                                         │
│       ├─ setup_arch() 中：                                      │
│       │   find_smp_config()     ← 扫描 ACPI/MP 表，发现有多少核  │
│       │                                                         │
│       ├─ kernel_init_freeable() 中：                             │
│       │   smp_prepare_cpus()    ← 准备工作：APIC 初始化           │
│       │   smp_init()            ← 正式启动所有 AP                 │
│       │     │                                                    │
│       │     └─ bringup_nonboot_cpus()                            │
│       │          └─ 对每个 AP 调用 cpu_up()                      │
│       │               └─ do_boot_cpu()                           │
│       │                    │                                     │
│       │                    ├─ 设置 AP 的初始代码地址              │
│       │                    │  initial_code = start_secondary     │
│       │                    │                                     │
│       │                    └─ 发送 INIT + SIPI 中断给目标 AP     │
│       │                                                         │
└───────┼─────────────────────────────────────────────────────────┘
        │
        │  INIT/SIPI 中断
        ▼
┌─────────────────────────────────────────────────────────────────┐
│  AP（被唤醒的 CPU 核心）                                         │
│                                                                 │
│  trampoline_start          ← 实模式（16位）入口                  │
│    │  cli                  ← 关中断                              │
│    │  lgdtl tr_gdt         ← 加载临时 GDT                       │
│    │  movl $X86_CR0_PE, %cr0  ← 进入保护模式                    │
│    │  ljmpl $__KERNEL32_CS, $pa_startup_32  ← 跳到 32 位代码     │
│    │                                                             │
│    └─→ secondary_startup_64                                     │
│         │  和 BSP 相同的 CR4/CR3/GDT/IDT/CR0 配置               │
│         │                                                       │
│         └─→ start_secondary()   ← C 语言入口                    │
│              ├─ cr4_init()                                       │
│              ├─ cpu_init_secondary()                              │
│              ├─ set_cpu_online(true)       ← 标记自己为在线       │
│              ├─ local_irq_enable()         ← 开中断               │
│              └─ cpu_startup_entry()        ← 进入 idle 循环       │
│                                                                  │
│  每个 AP 和 BSP 经历相同的 CPU 配置过程（CR0/CR3/CR4/GDT/IDT）    │
│  区别只是 AP 不执行 start_kernel()，而是直接进入 idle             │
└─────────────────────────────────────────────────────────────────┘
```

### 4.2 AP 的 CPU 配置

AP 复用 BSP 相同的 `secondary_startup_64` 代码（`arch/x86/kernel/head_64.S`），所以每个 AP 都经历：

```
同一段汇编代码（head_64.S 的 secondary_startup_64）：

1. CR4 = PAE | PGE            ← 和 BSP 一样，启用物理地址扩展 + 全局页
2. CR3 = init_top_pgt         ← 使用和 BSP 相同的内核页表
3. lgdt early_gdt_descr       ← 使用和 BSP 相同的 GDT
4. 加载 IDT                    ← 使用和 BSP 相同的 IDT
5. CR0 = CR0_STATE            ← 和 BSP 相同的 CR0 配置
6. 跳转到 initial_code        ← BSP 跳到 x86_64_start_kernel
                                  AP 跳到 start_secondary
```

每个 AP 进入 `start_secondary()` 后：
- 初始化自己的 per-CPU 变量
- 初始化本地 APIC
- 标记自己为 online
- 进入 idle 循环，等待调度器分配任务

**至此，所有 CPU 核心都被内核接管，各自有独立的运行队列，调度器可以在所有核心上分配进程。**

---

## 五、完整时间线总结

```
时间线 ──────────────────────────────────────────────────────────────────→

① BIOS/EFI         ② 引导加载器       ③ 内核解压缩        ④ start_kernel
│                   │                  │                   │
│ 上电自检          │ GRUB 加载        │ head_64.S:        │ setup_arch():
│ 探测硬件          │ bzImage 到内存   │   CR4.PAE=1       │   读 E820
│ E820 探测内存     │                  │   CR3=临时页表     │   memblock 建立
│                   │                  │   EFER.LME=1      │   预留内核内存
│                   │                  │   CR0=PG|PE       │   建内核页表
│                   │                  │   → 64位长模式     │   CR3=swapper_pg_dir
│                   │                  │                   │   安装异常处理 IDT
│                   │                  │ head64.c:         │
│                   │                  │   GDT/IDT 加载    │ mm_init():
│                   │                  │   → start_kernel  │   memblock→伙伴系统
│                   │                  │                   │   Slab 初始化
│                   │                  │                   │   vmalloc 初始化
│                   │                  │                   │
│                   │                  │                   │ sched_init():
│                   │                  │                   │   调度器运行队列
│                   │                  │                   │
│─── 硬件原始状态 ──│─── 加载中 ────── │─── CPU 被接管 ─── │── 内存被接管 ──→
│                   │                  │                   │
│                   │                  │                   │ smp_init():
│                   │                  │                   │   唤醒所有 AP
│                   │                  │                   │   每个 AP 重复
│                   │                  │                   │   CPU 接管过程
│                   │                  │                   │
│                   │                  │                   │── 所有 CPU 接管 ─→
│                   │                  │                   │
│                   │                  │                   │ kernel_execve:
│                   │                  │                   │   /sbin/init
│                   │                  │                   │── 用户空间启动 ─→
```

---

## 六、一个类比：理解"接管"的本质

如果把计算机比作一栋新建好的办公大楼，"内核接管硬件"就像物业公司入驻：

```
硬件状态（空楼）              内核接管后（物业入驻）
────────────                ────────────────────
大楼建好了，但：              物业公司做了这些事：

没有门禁系统                  安装门禁 = 设置 GDT（区分内核态/用户态）
没有消防预案                  贴消防预案 = 设置 IDT（异常/中断处理）
不知道有多少房间              清点房间 = E820 探测物理内存
没有房间分配制度              建立分配制度 = 伙伴系统 + Slab
没有租户登记                  租户登记系统 = 进程管理（task_struct）
只有一个电梯在运行            启动所有电梯 = SMP 启动所有 CPU 核心

入驻后，每间房间都有编号，     启动后，每个物理页都有 struct page，
每个租户都有登记，             每个进程都有 task_struct，
所有设施都有使用规则。         所有硬件访问都通过内核接口。

没有人能绕过物业直接使用设施。  没有程序能绕过内核直接访问硬件。
```

---

## 七、关键源码文件索引

| 阶段 | 文件 | 关键函数 |
|------|------|---------|
| 解压缩阶段 CPU 配置 | `arch/x86/boot/compressed/head_64.S` | CR4/CR3/EFER/CR0 配置 |
| 内核入口 CPU 配置 | `arch/x86/kernel/head_64.S` | `startup_64`, `secondary_startup_64` |
| C 语言入口 | `arch/x86/kernel/head64.c` | `x86_64_start_kernel()`, `startup_64_setup_env()` |
| 主初始化 | `init/main.c` | `start_kernel()`, `mm_init()` |
| 体系结构初始化 | `arch/x86/kernel/setup.c` | `setup_arch()`, `early_reserve_memory()` |
| E820 内存探测 | `arch/x86/kernel/e820.c` | `e820__memory_setup_default()`, `e820__memblock_setup()` |
| 早期内存分配器 | `mm/memblock.c` | `memblock_add()`, `memblock_reserve()`, `memblock_free_all()` |
| 内核页表建立 | `arch/x86/mm/init.c` | `init_mem_mapping()`, `zone_sizes_init()` |
| 伙伴系统初始化 | `mm/page_alloc.c` | `free_area_init()`, `memblock_free_pages()` |
| GDT/IDT | `arch/x86/kernel/head64.c`, `arch/x86/kernel/idt.c` | `startup_64_setup_env()`, `idt_setup_*()` |
| 控制寄存器定义 | `arch/x86/include/uapi/asm/processor-flags.h` | `CR0_STATE`, `X86_CR4_*` |
| SMP 启动 | `arch/x86/kernel/smpboot.c` | `do_boot_cpu()`, `start_secondary()` |
| AP 实模式入口 | `arch/x86/realmode/rm/trampoline_64.S` | `trampoline_start` |

---

## 八、回答最初的问题

> 内核是怎么接管 CPU 的？

通过配置 CPU 的控制寄存器（CR0/CR3/CR4/EFER）和描述符表寄存器（GDTR/IDTR），把 CPU 从 16 位实模式切换到 64 位长模式，启用分页、特权级隔离和中断处理。**配置完成后，CPU 就按照内核的规则运行——用户程序的每次系统调用、每次内存访问、每次异常，都由内核代码处理。**

> 内核是怎么接管内存的？

分五步：
1. 从 BIOS 获取物理内存布局（E820）
2. 用 memblock 做早期内存管理，预留内核自身占用的区域
3. 建立内核页表，映射全部物理 RAM
4. 初始化伙伴系统的 Zone 元数据
5. 把所有空闲页从 memblock 移交给伙伴系统，再初始化 Slab 和 vmalloc

**完成后，每一个物理页帧都被 `struct page` 描述，要么在使用中，要么在伙伴系统的空闲链表中。任何人要用内存，都必须通过内核的接口分配。**
