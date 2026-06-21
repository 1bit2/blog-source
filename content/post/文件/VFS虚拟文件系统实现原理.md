+++
date = '2026-05-07'
title = 'VFS 虚拟文件系统实现原理'
weight = 1
tags = [
    "VFS",
    "super_block",
    "inode",
    "dentry",
    "file",
    "file_operations",
    "mount",
    "open",
    "read",
    "write",
    "ext4",
    "remount",
]
categories = [
    "文件",
]
+++
# VFS 虚拟文件系统实现原理

> 基于 Linux 5.15.78 源码。本文从"VFS 解决什么问题"出发，剖析四大核心对象的设计动机，追踪 open/read/write 完整调用链，解析 mount 如何将磁盘接入目录树，以及 remount 如何修改读写权限。

## 一、VFS 解决什么问题

### 1.1 没有 VFS 的世界

假设 Linux 没有 VFS 抽象层，每种文件系统（ext4、xfs、btrfs、tmpfs、procfs...）都有自己的系统调用接口：

```
用户程序要读 ext4 文件 → ext4_read()
用户程序要读 xfs 文件  → xfs_read()
用户程序要读 proc 文件 → proc_read()
```

这带来三个严重问题：

1. **应用程序必须知道底层文件系统类型** — 程序不可移植
2. **每新增一种文件系统，所有应用都要修改** — 扩展性为零
3. **不同文件系统无法统一在一棵目录树里** — 用户体验割裂

### 1.2 VFS 的解决方案：面向对象 + 多态

VFS 在用户和具体文件系统之间插入一层抽象，定义**四个通用对象**和**一组操作函数表**：

```
用户程序: open("/mnt/disk/file.txt") → read(fd, buf, 4096) → write(fd, buf, 4096)
                      │                         │                       │
                      ▼                         ▼                       ▼
VFS 层:          path_openat()              vfs_read()              vfs_write()
                      │                         │                       │
                      ▼                         ▼                       ▼
            inode->i_fop->open()    file->f_op->read_iter()  file->f_op->write_iter()
                      │                         │                       │
                      ▼                         ▼                       ▼
ext4 实现:    ext4_file_open()      ext4_file_read_iter()   ext4_file_write_iter()
```

**核心设计思想**：用户只看到统一的 `open/read/write`，VFS 通过函数指针表（类似 C++ 虚函数表）分派给具体文件系统。这就是"Virtual"的含义 — **调用接口是虚的（通用的），实际实现是实的（具体文件系统提供的）**。

### 1.3 四大对象的职责划分

| 对象 | 解决什么问题 | 关键操作函数表 | 生命周期 |
|------|------------|--------------|---------|
| `super_block` | "这个磁盘用什么规则组织数据？" | `super_operations` | mount 时创建，umount 时销毁 |
| `inode` | "这个文件的元数据（大小、权限、数据在哪）是什么？" | `inode_operations` | 首次访问时从磁盘读入缓存 |
| `dentry` | "路径名的每一段怎么找到对应的 inode？" | `dentry_operations` | 路径查找时创建，LRU 淘汰 |
| `file` | "进程打开文件后的读写状态（偏移量、打开模式）是什么？" | `file_operations` | open 时创建，close 时销毁 |

为什么需要四个而不是一个？因为它们的生命周期和共享关系完全不同：

```
进程 A: open("/home/test.txt") → fd=3, file 对象 A (pos=0)
进程 B: open("/home/test.txt") → fd=5, file 对象 B (pos=0)

                file A (pos=100)  ──┐
                                     ├──→ dentry("test.txt") ──→ inode(ino=12345)
                file B (pos=200)  ──┘           │                      │
                                          dentry("home")          super_block
                                                │
                                          dentry("/")
```

- **file** 是进程私有的（每次 open 产生一个新的，各自维护读写偏移）
- **dentry** 和 **inode** 是全局缓存的（多个进程打开同一文件共享同一个 inode）
- **super_block** 是文件系统级的（一次 mount 一个）

## 二、四大核心对象的源码结构

### 2.1 super_block — 已挂载文件系统的描述符

```c
// include/linux/fs.h:1498
struct super_block {
    struct list_head    s_list;      /* 全局超级块链表 */
    dev_t               s_dev;       /* 设备号(主+次) */
    unsigned long       s_blocksize; /* 块大小(字节) */
    loff_t              s_maxbytes;  /* 最大文件大小 */
    struct file_system_type *s_type; /* 文件系统类型 → ext4_fs_type */
    const struct super_operations *s_op; /* 操作函数表 ← VFS多态的核心 */
    unsigned long       s_flags;     /* 挂载标志 SB_RDONLY 等 */
    unsigned long       s_magic;     /* 魔数 (ext4=0xEF53) */
    struct dentry       *s_root;     /* 根目录 dentry ← 文件系统入口 */
    struct block_device *s_bdev;     /* 底层块设备 */
    void                *s_fs_info;  /* 文件系统私有数据 ← ext4: ext4_sb_info */
    // ...
};
```

**设计动机**：`s_fs_info` 是最巧妙的设计 — VFS 层不需要知道 ext4 的块组、日志等细节，ext4 自己把这些信息放在 `ext4_sb_info` 里，通过 `s_fs_info` 指针自取。这实现了**接口与实现的分离**。

`s_op` 指向的 `super_operations` 定义了文件系统级别的操作（`include/linux/fs.h:2165`）：

```c
struct super_operations {
    struct inode *(*alloc_inode)(struct super_block *sb);  /* 分配 inode */
    void (*destroy_inode)(struct inode *);                 /* 销毁 inode */
    void (*dirty_inode)(struct inode *, int flags);        /* 标记 inode 为脏 */
    int (*write_inode)(struct inode *, ...);               /* 回写 inode 到磁盘 */
    int (*sync_fs)(struct super_block *sb, int wait);      /* 同步文件系统 */
    int (*statfs)(struct dentry *, struct kstatfs *);      /* 获取文件系统统计 df 命令 */
    int (*remount_fs)(struct super_block *, int *, char *);/* 重挂载 */
    // ...
};
```

### 2.2 inode — 文件的身份证

```c
// include/linux/fs.h:624
struct inode {
    umode_t             i_mode;      /* 文件类型+权限: S_IFREG|0644 */
    kuid_t              i_uid;       /* 所有者 UID */
    kgid_t              i_gid;       /* 所属组 GID */
    unsigned long       i_ino;       /* inode 号 — 文件的唯一标识 */
    loff_t              i_size;      /* 文件大小 */
    blkcnt_t            i_blocks;    /* 占用磁盘块数 */
    struct timespec64   i_atime;     /* 最后访问时间 */
    struct timespec64   i_mtime;     /* 最后修改时间 */
    struct timespec64   i_ctime;     /* 状态改变时间 */

    const struct inode_operations *i_op;  /* inode 操作: lookup/create/unlink */
    const struct file_operations  *i_fop; /* 默认的文件操作: open 时拷贝给 file */
    struct super_block  *i_sb;       /* 所属超级块 */
    struct address_space *i_mapping; /* 页缓存 ← 文件数据在内存中的缓存 */
    // ...
};
```

**关键设计**：inode 不包含文件名。文件名在 dentry 中。这使得硬链接成为可能 — 多个 dentry（多个文件名）可以指向同一个 inode。

`i_op` 定义目录层面的操作（`include/linux/fs.h:2083`）：

```c
struct inode_operations {
    struct dentry *(*lookup)(struct inode *, struct dentry *, unsigned int);  /* 路径查找 */
    int (*create)(struct user_namespace *, struct inode *, struct dentry *, umode_t, bool);
    int (*link)(struct dentry *, struct inode *, struct dentry *);    /* 硬链接 */
    int (*unlink)(struct inode *, struct dentry *);                   /* 删除文件 */
    int (*mkdir)(struct user_namespace *, struct inode *, struct dentry *, umode_t);
    int (*rmdir)(struct inode *, struct dentry *);
    int (*rename)(...);
    int (*permission)(struct user_namespace *, struct inode *, int);  /* 权限检查 */
    // ...
};
```

### 2.3 dentry — 路径查找的加速器

```c
// include/linux/dcache.h:91
struct dentry {
    unsigned int        d_flags;     /* DCACHE_MOUNTED 等标志 */
    struct hlist_bl_node d_hash;     /* 哈希表节点 ← 加速路径查找 */
    struct dentry       *d_parent;   /* 父目录 dentry */
    struct qstr         d_name;      /* 当前路径段名: "home", "test.txt" */
    struct inode        *d_inode;    /* 指向 inode (NULL=负缓存) */
    struct super_block  *d_sb;       /* 所属超级块 */
    const struct dentry_operations *d_op;
    struct list_head    d_child;     /* 在父目录的子链表中 */
    struct list_head    d_subdirs;   /* 子目录/子文件链表 */
    // ...
};
```

**为什么需要 dentry？** 磁盘上只有 inode 号，没有路径的概念。用户用路径 `/home/test.txt` 访问文件，内核需要逐段解析：

1. `/` → 找到根 inode
2. `home` → 在根 inode 的目录数据中查找名为 "home" 的条目，得到 inode 号
3. `test.txt` → 在 home 的 inode 目录数据中查找，得到目标 inode

如果每次都从磁盘读取目录数据，代价太高。dentry 缓存了**路径名 → inode** 的映射关系，构成一棵内存中的目录树。

**负缓存（negative dentry）**：`d_inode == NULL` 表示"这个路径不存在"也被缓存了。好处：避免反复到磁盘查找不存在的文件（比如动态链接器逐目录搜索 `.so`）。

### 2.4 file — 进程打开文件的会话

```c
// include/linux/fs.h:966
struct file {
    struct path          f_path;     /* { vfsmount, dentry } 定位到哪个文件 */
    struct inode        *f_inode;    /* 快速访问 inode */
    const struct file_operations *f_op; /* 操作函数表 ← 从 inode->i_fop 拷贝 */
    atomic_long_t        f_count;    /* 引用计数 */
    unsigned int         f_flags;    /* O_RDONLY / O_WRONLY / O_APPEND 等 */
    fmode_t              f_mode;     /* FMODE_READ / FMODE_WRITE */
    loff_t               f_pos;      /* 当前读写偏移 ← 进程私有 */
    struct mutex         f_pos_lock; /* 保护 f_pos 的并发访问 */
    const struct cred   *f_cred;     /* 打开时的进程凭证 */
    struct file_ra_state f_ra;       /* 预读状态 */
    void                *private_data; /* 文件系统/驱动私有数据 */
    struct address_space *f_mapping; /* 页缓存映射 */
    // ...
};
```

`f_op` 指向文件级别的操作（`include/linux/fs.h:2041`），这是用户最直接打交道的函数表：

```c
struct file_operations {
    loff_t (*llseek)(struct file *, loff_t, int);                      /* lseek */
    ssize_t (*read)(struct file *, char __user *, size_t, loff_t *);   /* read */
    ssize_t (*write)(struct file *, const char __user *, size_t, loff_t *);
    ssize_t (*read_iter)(struct kiocb *, struct iov_iter *);           /* 新式 read */
    ssize_t (*write_iter)(struct kiocb *, struct iov_iter *);          /* 新式 write */
    __poll_t (*poll)(struct file *, struct poll_table_struct *);        /* select/poll */
    int (*mmap)(struct file *, struct vm_area_struct *);                /* mmap */
    int (*open)(struct inode *, struct file *);                         /* open */
    int (*release)(struct inode *, struct file *);                      /* close 最后引用 */
    int (*fsync)(struct file *, loff_t, loff_t, int datasync);         /* fsync */
    // ...
};
```

### 2.5 四大对象关系图

```
                        进程的文件描述符表
                    fd[0] fd[1] fd[2] fd[3]
                      │     │     │     │
                      ▼     ▼     ▼     ▼
                    ┌─────────────────────┐
                    │    struct file       │  ← 进程私有, 维护 f_pos
                    │  f_op → ext4_fops   │
                    │  f_path.dentry ─────┼──┐
                    └─────────────────────┘  │
                                              ▼
                                   ┌──────────────────┐
                              ┌────│  struct dentry    │  ← 全局缓存
                              │    │  d_name="test.txt"│
                              │    │  d_parent ────────┼──→ dentry("home")
                              │    └──────────────────┘          │
                              ▼                           d_parent │
                    ┌──────────────────┐                         ▼
                    │   struct inode   │              dentry("/") ← rootfs
                    │  i_ino = 12345   │
                    │  i_size = 4096   │
                    │  i_sb ───────────┼──┐
                    └──────────────────┘  │
                                          ▼
                              ┌────────────────────┐
                              │ struct super_block  │  ← 每个已挂载文件系统一个
                              │ s_type → ext4      │
                              │ s_op → ext4_sops   │
                              │ s_root → dentry("/")│
                              │ s_bdev → /dev/sda1 │
                              └────────────────────┘
```

## 三、文件系统注册 — VFS 如何知道 ext4 的存在

在 mount 之前，VFS 需要知道系统支持哪些文件系统类型。每种文件系统通过 `register_filesystem()` 注册自己。

### 3.1 file_system_type — 文件系统的"出生证"

```c
// include/linux/fs.h:2514
struct file_system_type {
    const char *name;        /* "ext4" / "xfs" / "tmpfs" */
    int fs_flags;            /* FS_REQUIRES_DEV: 需要块设备 */

    int (*init_fs_context)(struct fs_context *); /* 新式 mount 接口 */
    struct dentry *(*mount)(struct file_system_type *, int,
                            const char *, void *);  /* 旧式 mount 接口 */
    void (*kill_sb)(struct super_block *);           /* umount 时清理 */

    struct module *owner;                /* 所属内核模块 */
    struct file_system_type *next;       /* 单链表 → 下一个文件系统类型 */
    struct hlist_head fs_supers;         /* 该类型的所有 super_block 实例 */
};
```

### 3.2 ext4 注册自己

```c
// fs/ext4/super.c:6600
static struct file_system_type ext4_fs_type = {
    .owner    = THIS_MODULE,
    .name     = "ext4",
    .mount    = ext4_mount,          /* mount 时调用 */
    .kill_sb  = kill_block_super,    /* umount 时调用 */
    .fs_flags = FS_REQUIRES_DEV | FS_ALLOW_IDMAP,
};

// fs/ext4/super.c:6662 — 模块初始化时注册
err = register_filesystem(&ext4_fs_type);
```

### 3.3 register_filesystem — 加入全局链表

```c
// fs/filesystems.c:72
int register_filesystem(struct file_system_type *fs)
{
    struct file_system_type **p;

    write_lock(&file_systems_lock);
    p = find_filesystem(fs->name, strlen(fs->name));
    if (*p)
        res = -EBUSY;  /* 同名文件系统已注册 */
    else
        *p = fs;        /* 挂到链表末尾 */
    write_unlock(&file_systems_lock);
    return res;
}
```

内核维护一个简单的单链表 `file_systems`，所有注册的文件系统类型串在一起。`mount -t ext4` 时，VFS 调用 `get_fs_type("ext4")` 遍历这个链表找到 `ext4_fs_type`。

可以通过 `/proc/filesystems` 查看当前系统注册的所有文件系统类型。

## 四、mount — 将磁盘接入目录树

> 详细的 mount 调用链分析见 [Mount 机制分析](Mount机制分析.md)，本节侧重 mount 在 VFS 整体架构中的角色。

### 4.1 mount 的本质

你的理解是对的：**mount 就是把磁盘里的文件系统和 VFS 目录树建立联系**。更精确地说：

```
mount("/dev/sda1", "/mnt/disk", "ext4", 0, NULL) 做了三件事:

1. 读磁盘 → 创建内存对象
   ┌──────────────┐         ┌─────────────────────────────┐
   │  /dev/sda1   │ ──读──→ │ super_block + 根inode + 根dentry │
   │  (磁盘字节)   │         │ (内存数据结构)                │
   └──────────────┘         └─────────────────────────────┘

2. 设置操作函数指针
   sb->s_op  = &ext4_sops;
   inode->i_op  = &ext4_dir_inode_operations;
   inode->i_fop = &ext4_dir_operations;

3. 将新文件系统"嫁接"到目录树
   VFS 目录树:    /  ─→  mnt  ─→  disk (DCACHE_MOUNTED)
                                    ↓  vfsmount
   ext4 目录树:                    /  ─→  file1.txt
                                       ─→  dir1/
```

### 4.2 挂载点（mountpoint）的魔法

mount 的关键一步是在挂载点 dentry 上设置 `DCACHE_MOUNTED` 标志。路径查找时：

```c
// 路径查找逻辑 (fs/namei.c 简化)
走到 dentry "disk" 时:
    if (dentry->d_flags & DCACHE_MOUNTED) {
        // 查找挂载在此处的 vfsmount
        vfsmount = lookup_mnt(path);
        // 跨越到新文件系统的根 dentry
        path->dentry = vfsmount->mnt_root;
        path->mnt = vfsmount;
    }
```

这就像"传送门" — 走到挂载点时自动跳到另一个文件系统。

### 4.3 struct mount — 挂载实例的完整信息

`struct vfsmount` 只是对外暴露的精简结构，实际的挂载信息保存在 `struct mount`（`fs/mount.h:39`）中：

```c
struct mount {
    struct hlist_node mnt_hash;       /* 哈希表加速查找 */
    struct mount *mnt_parent;         /* 父挂载 */
    struct dentry *mnt_mountpoint;    /* 挂载点 dentry (disk) */
    struct vfsmount mnt;              /* 嵌入的 vfsmount */
    struct list_head mnt_mounts;      /* 子挂载列表 */
    struct list_head mnt_child;       /* 在父挂载的子列表中 */
    const char *mnt_devname;          /* 设备名 "/dev/sda1" */
    struct mnt_namespace *mnt_ns;     /* 所属 mount namespace */
    int mnt_id;                       /* 唯一挂载 ID */
    // ...
};
```

`struct mount` 和 `struct vfsmount` 的关系类似于 `struct task_struct` 和 `struct thread_info` — 前者是完整的内部表示，后者是精简的对外接口。通过 `real_mount()` 宏可以从 `vfsmount` 反推 `mount`。

### 4.4 mount 的完整调用链

```
用户空间: mount("/dev/sda1", "/mnt/disk", "ext4", 0, NULL)
    │
    ▼
SYSCALL_DEFINE5(mount)                    [fs/namespace.c:3651]
    │ 拷贝用户空间参数到内核
    ▼
do_mount(dev_name, dir_name, type, flags) [fs/namespace.c:3416]
    │ user_path_at() 解析挂载点路径
    ▼
path_mount(dev_name, &path, type, flags)  [fs/namespace.c:3321]
    │ 根据 flags 分发:
    │   MS_REMOUNT → do_remount()
    │   MS_BIND    → do_loopback()
    │   MS_MOVE    → do_move_mount_old()
    │   默认       → do_new_mount()
    ▼
do_new_mount(path, "ext4", sb_flags, ...) [fs/namespace.c:2978]
    │ ① get_fs_type("ext4") → 找到 ext4_fs_type
    │ ② fs_context_for_mount() → 创建文件系统上下文
    │ ③ vfs_get_tree(fc) → 核心！读取磁盘并创建 super_block
    │ ④ do_new_mount_fc() → 将文件系统接入目录树
    ▼
vfs_get_tree(fc)                          [fs/super.c]
    │ 调用 fc->ops->get_tree(fc)
    │ 对于 ext4: ext4_get_tree() → ext4_fill_super()
    ▼
ext4_fill_super(sb, data, silent)         [fs/ext4/super.c]
    ├── 打开块设备
    ├── 读取磁盘超级块 (偏移 1024 字节)
    ├── 验证魔数 0xEF53
    ├── 初始化 ext4_sb_info
    ├── 设置 sb->s_op = &ext4_sops
    ├── 读取根 inode (ino=2): ext4_iget(sb, EXT4_ROOT_INO)
    └── 创建根 dentry: sb->s_root = d_make_root(root_inode)
```

## 五、open — 从路径到 file 对象

### 5.1 调用链总览

```
用户空间: int fd = open("/mnt/disk/file.txt", O_RDONLY)
    │
    ▼
SYSCALL_DEFINE3(open, filename, flags, mode)  [fs/open.c:1231]
    │ force_o_largefile()
    ▼
do_sys_open(AT_FDCWD, filename, flags, mode)  [fs/open.c:1224]
    │ → do_sys_openat2()
    ▼
do_sys_openat2(dfd, filename, &how)           [fs/open.c:1195]
    │ ① build_open_flags() → 构建 open_flags
    │ ② getname(filename) → 从用户空间拷贝文件名
    │ ③ get_unused_fd_flags() → 分配 fd 号
    │ ④ do_filp_open() → 核心：路径查找 + 打开
    │ ⑤ fd_install(fd, file) → 将 file 装入进程 fd 表
    ▼
do_filp_open(dfd, pathname, &op)              [fs/namei.c:3634]
    │ 设置 nameidata，先尝试 RCU 路径查找
    ▼
path_openat(&nd, &op, flags)                  [fs/namei.c:3595]
    │ ① alloc_empty_file() → 分配空 file 对象
    │ ② path_init() → 确定查找起点 (根目录或当前目录)
    │ ③ link_path_walk() → 逐段解析路径 "mnt" → "disk" → "file.txt"
    │ ④ open_last_lookups() → 处理最后一段 (可能需要创建文件)
    │ ⑤ do_open() → 权限检查 + 调用文件系统的 open
    ▼
do_open()
    │ vfs_open() → file->f_op->open()
    │ 对于 ext4: ext4_file_open()
    └── 完成! file 对象已就绪
```

### 5.2 路径查找 — VFS 最复杂的部分

`link_path_walk()` 是路径解析的核心，对 `/mnt/disk/file.txt` 逐段处理：

1. 从当前 dentry 开始（根目录 `/`）
2. 取出下一段路径名 `mnt`
3. 在 dentry 缓存的哈希表中查找 — **这就是 dentry 存在的意义**
4. 如果缓存未命中，调用 `inode->i_op->lookup()` 从磁盘读取
5. 检查是否为挂载点（`DCACHE_MOUNTED`），是则跨越到新文件系统
6. 检查权限（`inode->i_op->permission()`）
7. 重复直到最后一段

路径查找有两种模式：
- **RCU 模式**：无锁快速路径，不增加引用计数，失败回退到 ref 模式
- **REF 模式**：传统加锁路径，逐步增加 dentry/vfsmount 的引用计数

### 5.3 fd_install — file 对象和 fd 的绑定

```c
// fs/open.c:1209-1217 (简化)
fd = get_unused_fd_flags(how->flags);  // 从进程 fd 表找空位
struct file *f = do_filp_open(dfd, tmp, &op);  // 创建 file 对象
fd_install(fd, f);  // current->files->fdt->fd[fd] = f
```

从此，进程通过 `fd` 就能找到 `file → dentry → inode → super_block`，完成对任何文件系统的透明访问。

## 六、read/write — 数据流经 VFS

### 6.1 read 调用链

```c
// fs/read_write.c:631
SYSCALL_DEFINE3(read, unsigned int, fd, char __user *, buf, size_t, count)
{
    return ksys_read(fd, buf, count);
}

// fs/read_write.c:612
ssize_t ksys_read(unsigned int fd, char __user *buf, size_t count)
{
    struct fd f = fdget_pos(fd);       // fd → file 对象
    loff_t pos = file_pos_read(f.file); // 获取当前偏移
    ret = vfs_read(f.file, buf, count, &pos);
    file_pos_write(f.file, pos);       // 更新偏移
    fdput_pos(f);
    return ret;
}
```

### 6.2 vfs_read — VFS 层的分发点

```c
// fs/read_write.c:465
ssize_t vfs_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
    // 权限检查
    if (!(file->f_mode & FMODE_READ))
        return -EBADF;

    // 地址空间验证
    if (unlikely(!access_ok(buf, count)))
        return -EFAULT;

    // 调用具体文件系统实现 ← VFS 多态发生在这里
    if (file->f_op->read)
        ret = file->f_op->read(file, buf, count, pos);
    else if (file->f_op->read_iter)
        ret = new_sync_read(file, buf, count, pos);  // 包装成 iov_iter 调用

    return ret;
}
```

**关键一行**：`file->f_op->read_iter()`。对于 ext4，实际调用的是：

```c
// fs/ext4/file.c:921
const struct file_operations ext4_file_operations = {
    .read_iter  = ext4_file_read_iter,   // ← read 最终到达这里
    .write_iter = ext4_file_write_iter,  // ← write 最终到达这里
    .open       = ext4_file_open,
    .fsync      = ext4_sync_file,
    .mmap       = ext4_file_mmap,
    // ...
};
```

### 6.3 数据流全貌

```
用户空间: read(fd, buf, 4096)
    │
    ▼ 系统调用
ksys_read()                    fd → file 对象
    │
    ▼
vfs_read()                     权限检查 + 分发
    │
    ▼ file->f_op->read_iter()
ext4_file_read_iter()          ext4 具体实现
    │
    ▼
generic_file_read_iter()       通用页缓存读取
    │
    ├──→ 页缓存命中?
    │     是 → copy_to_user()  直接从内存拷贝到用户 buf
    │     否 → ↓
    │
    ▼
a_ops->readahead()             触发磁盘 I/O + 预读
    │
    ▼
submit_bio() → 块设备层 → 磁盘驱动 → 物理磁盘
```

**页缓存（Page Cache）是读性能的关键**：文件数据读取后缓存在 `address_space` 的页缓存中。同一文件的后续读取直接从内存返回，不需要再访问磁盘。这也是为什么 `free -h` 中 `buff/cache` 往往占用很大 — 它在用内存做磁盘缓存。

## 七、remount — 为什么 `mount -o remount,rw /` 能获得写权限

### 7.1 场景

嵌入式系统、recovery 模式、或出错时根文件系统以只读挂载（`SB_RDONLY`）。此时无法修改任何文件。执行：

```bash
mount -o remount,rw /
```

重新挂载为读写模式后，就能修改文件了。为什么？

### 7.2 调用链

```
mount -o remount,rw /
    │
    ▼ SYSCALL_DEFINE5(mount) → do_mount() → path_mount()
    │ 检测到 flags 包含 MS_REMOUNT
    ▼
path_mount()                              [fs/namespace.c:3387]
    │ if (flags & MS_REMOUNT)
    │     return do_remount(path, flags, sb_flags, mnt_flags, data);
    │
    │ 注意: 此时 sb_flags 中 SB_RDONLY 位已被清除
    │      (因为用户指定了 rw，不再是 rdonly)
    ▼
do_remount()                              [fs/namespace.c:2629]
    │ ① 权限检查: ns_capable(sb->s_user_ns, CAP_SYS_ADMIN)
    │ ② 调用 reconfigure_super(fc) 修改超级块标志
    │ ③ 调用 set_mount_attributes(mnt, mnt_flags) 修改挂载标志
    ▼
reconfigure_super(fc)                     [fs/super.c:852]
```

### 7.3 reconfigure_super — 标志位翻转

```c
// fs/super.c:917 — 关键一行
WRITE_ONCE(sb->s_flags,
    ((sb->s_flags & ~fc->sb_flags_mask) |
     (fc->sb_flags & fc->sb_flags_mask)));
```

这行代码的含义：

- `fc->sb_flags_mask` 包含 `SB_RDONLY`（表示"我要修改 RDONLY 这个标志"）
- `fc->sb_flags` 中 `SB_RDONLY` 位为 0（表示"改为非只读"）
- 执行后：`sb->s_flags` 的 `SB_RDONLY` 位被清零

**之后所有写操作的权限检查都会通过**，因为 VFS 层的写操作检查都是：

```c
if (IS_RDONLY(inode))  // 本质是检查 inode->i_sb->s_flags & SB_RDONLY
    return -EROFS;
```

`SB_RDONLY` 被清除后，`IS_RDONLY()` 返回 false，写操作放行。

### 7.4 remount 还做了哪些保护？

`reconfigure_super()` 在修改标志前做了安全检查（`fs/super.c:868-876`）：

```c
if (fc->sb_flags_mask & SB_RDONLY) {
    // 如果要改为读写，但底层块设备本身是只读的（如 CD-ROM），拒绝
    if (!(fc->sb_flags & SB_RDONLY) && sb->s_bdev && bdev_read_only(sb->s_bdev))
        return -EACCES;
}
```

同时，如果要从读写改为只读（`remount_ro = true`），需要：
1. 检查没有文件以写模式打开（`sb_prepare_remount_readonly`）
2. 清理页缓存中的脏数据（`invalidate_bdev`）

## 八、VFS 四大操作函数表的协作

用一个完整例子串联所有知识 — 用户执行 `cat /mnt/disk/hello.txt`：

```
步骤 1: open("/mnt/disk/hello.txt", O_RDONLY)

  VFS 路径查找:
  "/" → dentry 缓存命中 → rootfs 的根 inode
  "mnt" → dentry 缓存命中 → rootfs 下的 mnt 目录 inode
  "disk" → 发现 DCACHE_MOUNTED → lookup_mnt() → 跨到 ext4 的 vfsmount
  "hello.txt" → ext4 inode->i_op->lookup() → 从磁盘读目录项 → 找到 inode

  创建 file 对象:
  file->f_op = inode->i_fop = &ext4_file_operations
  file->f_pos = 0
  fd_install(3, file) → 进程 fd[3] = file

步骤 2: read(3, buf, 4096)

  fd[3] → file 对象
  vfs_read() → file->f_op->read_iter() → ext4_file_read_iter()
  → 检查页缓存 → 未命中 → 向块设备发起 I/O → 数据加载到页缓存
  → copy_to_user(buf, page_data, count)
  → 返回读取字节数

步骤 3: close(3)

  fd[3] → file 对象 → f_count--
  引用计数归零 → file->f_op->release() → ext4_release_file()
  → 释放 file 对象
  → dentry 和 inode 仍在缓存中（可能被其他进程使用）
```

## 九、VFS 的设计总结

### 9.1 设计哲学

| 设计选择 | 为什么 | 带来的代价 |
|---------|-------|----------|
| 函数指针表做多态 | C 语言没有虚函数，函数指针表是最高效的替代 | 间接调用有轻微性能开销；新增操作要改结构体 |
| dentry 缓存 | 路径查找是最频繁的操作，必须避免每次都读磁盘 | 消耗大量内存（大型系统可能缓存数百万 dentry） |
| inode 和 dentry 分离 | 支持硬链接；inode 号才是文件身份，文件名只是别名 | 需要维护两套缓存和它们的映射关系 |
| file 独立于 inode | 同一文件的多次 open 需要独立的读写偏移和打开模式 | 每次 open 都要分配内存 |
| super_block 的 s_fs_info | 让 VFS 不依赖任何具体文件系统的数据结构 | 类型不安全（void *），需要文件系统自己管理 |

### 9.2 VFS 处理的文件系统种类

VFS 不仅仅是为磁盘文件系统设计的：

| 类型 | 示例 | 有磁盘？ | FS_REQUIRES_DEV |
|------|------|---------|-----------------|
| 磁盘文件系统 | ext4, xfs, btrfs | 有 | 是 |
| 网络文件系统 | NFS, CIFS, 9P | 无（网络） | 否 |
| 伪文件系统 | procfs, sysfs, debugfs | 无（内存） | 否 |
| 内存文件系统 | tmpfs, ramfs | 无（内存） | 否 |
| 特殊文件系统 | devtmpfs, sockfs, pipefs | 无 | 否 |

它们都通过相同的 `register_filesystem` 注册，通过相同的 `open/read/write` 接口访问。这就是 Unix "一切皆文件"哲学的实现基础。

### 9.3 一句话总结

**VFS 是 Linux 内核中最典型的"面向对象"设计 — 用 `super_block` / `inode` / `dentry` / `file` 四个对象和三组操作函数表（`super_operations` / `inode_operations` / `file_operations`），将几十种完全不同的文件系统统一在一棵目录树和一套系统调用之下。mount 负责建立连接，open 负责创建会话，read/write 通过函数指针分派到具体实现。**
