+++
date = '2026-04-16'
title = '为什么磁盘需要 Mount 才能使用 - 内核源码分析'
weight = 2
tags = [
    "mount",
    "VFS",
    "super_block",
    "文件系统",
]
categories = [
    "文件",
]
+++
# 为什么磁盘需要 Mount 才能使用 - 内核源码分析

## 一、核心原因概述

磁盘需要 mount 的根本原因是 **Linux VFS（虚拟文件系统）架构设计**。VFS 是用户程序和具体文件系统之间的抽象层，它需要通过 mount 来：

1. **读取并解析磁盘上的文件系统元数据**（超级块、inode 表等）
2. **建立内存中的数据结构**（super_block、dentry、inode 缓存）
3. **将磁盘文件系统"嫁接"到统一的目录树上**
4. **提供文件操作的函数指针**（read、write、lookup 等）

```
┌─────────────────────────────────────────────────────────────┐
│                    用户空间                                  │
│    open("/mnt/disk/file.txt", O_RDONLY)                     │
└─────────────────────────┬───────────────────────────────────┘
                          │ 系统调用
┌─────────────────────────▼───────────────────────────────────┐
│                       VFS 层                                 │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐         │
│  │ super_block │  │   dentry    │  │    inode    │         │
│  │  (文件系统)  │  │  (目录项)   │  │  (文件信息) │         │
│  └─────────────┘  └─────────────┘  └─────────────┘         │
│         ↓ s_op          ↓ d_op          ↓ i_op             │
│    [文件系统特定的操作函数指针]                               │
└─────────────────────────┬───────────────────────────────────┘
                          │ Mount 建立的关联
┌─────────────────────────▼───────────────────────────────────┐
│              具体文件系统 (ext4/xfs/btrfs...)               │
│  ┌─────────────────────────────────────────────────────┐   │
│  │  磁盘布局: [超级块][块组描述符][inode表][数据块...]   │   │
│  └─────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

## 二、Mount 系统调用流程

### 2.1 系统调用入口

```c
// fs/namespace.c:3522

SYSCALL_DEFINE5(mount, char __user *, dev_name, char __user *, dir_name,
                char __user *, type, unsigned long, flags, void __user *, data)
{
    // 参数说明:
    // dev_name: 设备名 (如 "/dev/sda1")
    // dir_name: 挂载点 (如 "/mnt/disk")
    // type:     文件系统类型 (如 "ext4")
    // flags:    挂载选项 (如 MS_RDONLY)
    // data:     文件系统特定选项
    
    // 1. 从用户空间拷贝参数
    kernel_type = copy_mount_string(type);
    kernel_dev = copy_mount_string(dev_name);
    options = copy_mount_options(data);
    
    // 2. 执行挂载
    ret = do_mount(kernel_dev, dir_name, kernel_type, flags, options);
    
    return ret;
}
```

### 2.2 do_mount - 主要流程

```c
// fs/namespace.c:3328

long do_mount(const char *dev_name, const char __user *dir_name,
              const char *type_page, unsigned long flags, void *data_page)
{
    struct path path;
    
    // 1. 解析挂载点路径，获取 path 结构
    // 这个 path 必须已经存在于某个已挂载的文件系统中
    ret = user_path_at(AT_FDCWD, dir_name, LOOKUP_FOLLOW, &path);
    
    // 2. 执行实际挂载
    ret = path_mount(dev_name, &path, type_page, flags, data_page);
    
    return ret;
}
```

### 2.3 path_mount - 根据类型分发

```c
// fs/namespace.c:3249

int path_mount(const char *dev_name, struct path *path,
               const char *type_page, unsigned long flags, void *data_page)
{
    // 安全检查
    ret = security_sb_mount(dev_name, path, type_page, flags, data_page);
    
    // 根据标志选择不同操作
    if (flags & MS_REMOUNT)
        return do_remount(path, flags, ...);      // 重新挂载
    if (flags & MS_BIND)
        return do_loopback(path, dev_name, ...);  // 绑定挂载
    if (flags & MS_MOVE)
        return do_move_mount_old(path, dev_name); // 移动挂载点
    
    // 新挂载 - 最重要的路径
    return do_new_mount(path, type_page, sb_flags, mnt_flags, 
                        dev_name, data_page);
}
```

### 2.4 do_new_mount - 创建新挂载

```c
// fs/namespace.c:2953

static int do_new_mount(struct path *path, const char *fstype, int sb_flags,
                        int mnt_flags, const char *name, void *data)
{
    struct file_system_type *type;
    struct fs_context *fc;
    
    /*
     * 步骤1: 查找文件系统类型
     * 内核维护一个已注册文件系统的链表
     * ext4、xfs 等在模块加载时注册
     */
    type = get_fs_type(fstype);
    if (!type)
        return -ENODEV;  // 未知文件系统类型
    
    /*
     * 步骤2: 创建文件系统上下文
     * fs_context 是 mount 过程的"工作区"
     */
    fc = fs_context_for_mount(type, sb_flags);
    
    /*
     * 步骤3: 解析挂载选项
     */
    vfs_parse_fs_string(fc, "source", name, strlen(name));
    parse_monolithic_mount_data(fc, data);
    
    /*
     * 步骤4: 获取超级块 - 核心步骤！
     * 这里会读取磁盘上的文件系统元数据
     */
    err = vfs_get_tree(fc);
    
    /*
     * 步骤5: 完成挂载，将文件系统接入目录树
     */
    err = do_new_mount_fc(fc, path, mnt_flags);
    
    return err;
}
```

## 三、vfs_get_tree - 读取文件系统元数据

这是 mount 最核心的步骤，负责读取磁盘数据并初始化内存结构。

```c
// fs/super.c:1488

int vfs_get_tree(struct fs_context *fc)
{
    struct super_block *sb;
    
    /*
     * 调用具体文件系统的 get_tree 方法
     * 例如 ext4 的 ext4_get_tree -> ext4_fill_super
     * 
     * 这个函数会:
     * 1. 打开块设备
     * 2. 读取磁盘上的超级块
     * 3. 验证文件系统魔数
     * 4. 初始化 super_block 结构
     * 5. 读取根目录 inode
     * 6. 创建根 dentry
     */
    error = fc->ops->get_tree(fc);
    
    sb = fc->root->d_sb;
    
    // 标记超级块已初始化完成
    sb->s_flags |= SB_BORN;
    
    return 0;
}
```

## 四、ext4_fill_super - 以 ext4 为例

```c
// fs/ext4/super.c:3883

static int ext4_fill_super(struct super_block *sb, void *data, int silent)
{
    struct buffer_head *bh;
    struct ext4_super_block *es;  // 磁盘上的超级块结构
    struct ext4_sb_info *sbi;     // 内存中的超级块信息
    struct inode *root;
    
    /* 分配内存中的超级块信息结构 */
    sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
    sb->s_fs_info = sbi;
    
    /* 设置块大小 */
    blocksize = sb_min_blocksize(sb, EXT4_MIN_BLOCK_SIZE);
    
    /*
     * 从磁盘读取超级块
     * ext4 超级块位于偏移 1024 字节处
     */
    bh = ext4_sb_bread_unmovable(sb, logical_sb_block);
    es = (struct ext4_super_block *) (bh->b_data + offset);
    sbi->s_es = es;
    
    /*
     * 验证文件系统魔数
     * 如果魔数不匹配，说明不是 ext4 文件系统
     */
    sb->s_magic = le16_to_cpu(es->s_magic);
    if (sb->s_magic != EXT4_SUPER_MAGIC)
        goto cantfind_ext4;
    
    /*
     * 从超级块读取文件系统参数:
     * - 块大小
     * - inode 大小
     * - 块组数量
     * - 特性标志
     */
    sbi->s_inode_size = le16_to_cpu(es->s_inode_size);
    sbi->s_blocks_per_group = le32_to_cpu(es->s_blocks_per_group);
    sbi->s_inodes_per_group = le32_to_cpu(es->s_inodes_per_group);
    
    /* 设置文件系统操作函数 */
    sb->s_op = &ext4_sops;
    
    /*
     * 读取根目录 inode (inode 号 = 2)
     * 这是整个文件系统的入口点
     */
    root = ext4_iget(sb, EXT4_ROOT_INO, EXT4_IGET_SPECIAL);
    
    /*
     * 创建根目录的 dentry
     * 这个 dentry 将成为挂载点的连接点
     */
    sb->s_root = d_make_root(root);
    
    return 0;
}
```

### 4.1 磁盘超级块结构 (ext4)

```c
// fs/ext4/ext4.h

struct ext4_super_block {
    __le32  s_inodes_count;      /* inode 总数 */
    __le32  s_blocks_count_lo;   /* 块总数 */
    __le32  s_free_blocks_count; /* 空闲块数 */
    __le32  s_free_inodes_count; /* 空闲 inode 数 */
    __le32  s_first_data_block;  /* 第一个数据块 */
    __le32  s_log_block_size;    /* 块大小 = 1024 << s_log_block_size */
    __le32  s_blocks_per_group;  /* 每组块数 */
    __le32  s_inodes_per_group;  /* 每组 inode 数 */
    __le32  s_mtime;             /* 最后挂载时间 */
    __le32  s_wtime;             /* 最后写入时间 */
    __le16  s_mnt_count;         /* 挂载次数 */
    __le16  s_magic;             /* 魔数 0xEF53 */
    __le16  s_state;             /* 文件系统状态 */
    __le16  s_inode_size;        /* inode 大小 */
    // ... 更多字段
};
```

## 五、核心数据结构详解

### 5.1 super_block - 已挂载文件系统的描述符

```c
// include/linux/fs.h:1466

struct super_block {
    /*
     * 这个结构是 VFS 和具体文件系统之间的桥梁
     * 每个挂载的文件系统都有一个 super_block
     */
    
    dev_t               s_dev;           /* 设备号 */
    unsigned long       s_blocksize;     /* 块大小 */
    loff_t              s_maxbytes;      /* 最大文件大小 */
    
    struct file_system_type *s_type;     /* 文件系统类型 */
    
    /*
     * 文件系统操作函数指针
     * 这就是为什么不同文件系统可以用相同的系统调用
     */
    const struct super_operations *s_op;
    
    struct dentry       *s_root;         /* 根目录 dentry */
    
    struct block_device *s_bdev;         /* 块设备 */
    
    void                *s_fs_info;      /* 文件系统私有数据 */
                                         /* ext4: struct ext4_sb_info */
    
    unsigned long       s_magic;         /* 文件系统魔数 */
};
```

### 5.2 vfsmount - 挂载实例

```c
// include/linux/mount.h:71

struct vfsmount {
    /*
     * 描述一次具体的挂载
     * 同一个文件系统可以挂载到多个位置
     */
    struct dentry *mnt_root;      /* 该文件系统的根 dentry */
    struct super_block *mnt_sb;   /* 指向超级块 */
    int mnt_flags;                /* 挂载标志 */
};
```

### 5.3 dentry - 目录项（路径组件）

```c
// include/linux/dcache.h:91

struct dentry {
    /*
     * dentry 是路径查找的核心
     * 将文件名映射到 inode
     */
    struct dentry *d_parent;      /* 父目录 */
    struct qstr d_name;           /* 文件名 */
    struct inode *d_inode;        /* 关联的 inode */
    struct super_block *d_sb;     /* 所属超级块 */
    
    const struct dentry_operations *d_op;  /* 操作函数 */
    
    struct list_head d_subdirs;   /* 子目录链表 */
};
```

### 5.4 inode - 文件元数据

```c
// include/linux/fs.h:624

struct inode {
    /*
     * inode 描述文件的元数据
     * 不包含文件名（文件名在 dentry 中）
     */
    umode_t         i_mode;       /* 文件类型和权限 */
    kuid_t          i_uid;        /* 所有者 */
    kgid_t          i_gid;        /* 所属组 */
    unsigned long   i_ino;        /* inode 号 */
    loff_t          i_size;       /* 文件大小 */
    
    struct timespec64 i_atime;    /* 访问时间 */
    struct timespec64 i_mtime;    /* 修改时间 */
    struct timespec64 i_ctime;    /* 状态改变时间 */
    
    blkcnt_t        i_blocks;     /* 占用块数 */
    
    struct super_block *i_sb;     /* 所属超级块 */
    
    /*
     * 操作函数指针
     * 不同文件系统实现不同
     */
    const struct inode_operations *i_op;
    const struct file_operations *i_fop;
    
    struct address_space *i_mapping;  /* 页缓存 */
};
```

## 六、路径查找如何依赖挂载点

当用户访问 `/mnt/disk/file.txt` 时：

```c
// fs/namei.c (简化)

/*
 * 路径查找过程中，每进入一个目录都要检查是否是挂载点
 */
static int handle_mounts(struct path *path, ...)
{
    unsigned flags = path->dentry->d_flags;
    
    /*
     * DCACHE_MOUNTED 标志表示这个 dentry 上有文件系统挂载
     * 这个标志在 mount 时设置
     */
    if (flags & DCACHE_MOUNTED) {
        /*
         * lookup_mnt 查找挂载在这个点上的文件系统
         * 如果找到，就"跨越"到新文件系统
         */
        struct vfsmount *mounted = lookup_mnt(path);
        if (mounted) {
            /* 释放旧的 dentry */
            dput(path->dentry);
            
            /* 切换到新文件系统的根 */
            path->mnt = mounted;
            path->dentry = dget(mounted->mnt_root);
            
            /* 继续在新文件系统中查找 */
            continue;
        }
    }
}
```

### 6.1 路径查找示例

```
访问路径: /mnt/disk/file.txt

步骤 1: 从根目录 "/" 开始
        ┌──────────────┐
        │  / (rootfs)  │
        └──────┬───────┘
               │
步骤 2: 查找 "mnt"
        ┌──────▼───────┐
        │     mnt      │
        └──────┬───────┘
               │
步骤 3: 查找 "disk" - 发现这是挂载点！
        ┌──────▼───────┐
        │    disk      │ ← DCACHE_MOUNTED
        └──────┬───────┘
               │ lookup_mnt() 返回 ext4 的 vfsmount
               │
        ═══════╪═══════ 跨越文件系统边界
               │
步骤 4: 进入 ext4 文件系统的根目录
        ┌──────▼───────┐
        │  / (ext4)    │ ← vfsmount->mnt_root
        └──────┬───────┘
               │
步骤 5: 在 ext4 中查找 "file.txt"
        ┌──────▼───────┐
        │  file.txt    │
        └──────────────┘
```

## 七、为什么不能直接访问裸磁盘

### 7.1 磁盘只是字节序列

```
裸磁盘 (/dev/sda1):
┌────────────────────────────────────────────────────────┐
│ 0x00 0x00 0x00 0x00 ... (原始字节)                      │
│ 没有文件概念、没有目录概念                               │
│ 只是一系列扇区                                          │
└────────────────────────────────────────────────────────┘

Mount 后:
┌────────────────────────────────────────────────────────┐
│              VFS 提供的抽象                             │
│  ┌──────┐   ┌──────┐   ┌──────┐                       │
│  │ 文件 │   │ 目录 │   │ 权限 │                       │
│  └──────┘   └──────┘   └──────┘                       │
│       ↑         ↑         ↑                           │
│       └─────────┴─────────┴── Mount 建立的映射        │
│                 ↓                                      │
│  ┌─────────────────────────────────────────────────┐  │
│  │ 超级块 → 块组 → inode表 → 数据块                  │  │
│  │ (文件系统结构化数据)                              │  │
│  └─────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────┘
```

### 7.2 Mount 解决的问题

| 问题 | Mount 如何解决 |
|------|---------------|
| 如何定位文件数据块？ | 读取 inode 表，获取块指针 |
| 如何找到文件名？ | 读取目录项 (dirent) |
| 如何知道块大小？ | 从超级块读取 |
| 如何处理空闲空间？ | 读取块位图、inode 位图 |
| 如何加入目录树？ | 创建 vfsmount，设置挂载点 |
| 如何调用正确的读写函数？ | 设置 file_operations 指针 |

### 7.3 如果没有 Mount

```c
// 假设直接读取 /dev/sda1

int fd = open("/dev/sda1", O_RDONLY);
char buf[512];
read(fd, buf, 512);  // 读到什么？

/*
 * 你读到的是原始扇区数据
 * 可能是超级块、可能是某个文件的数据块
 * 你无法知道:
 *   1. 这是什么数据
 *   2. 文件在哪里
 *   3. 如何解释这些字节
 * 
 * 你需要自己实现整个文件系统解析逻辑！
 */
```

## 八、file_system_type - 文件系统注册

```c
// include/linux/fs.h:2438

struct file_system_type {
    const char *name;              /* 文件系统名称 "ext4" */
    int fs_flags;                  /* 特性标志 */
    
    /*
     * 初始化函数 - mount 时调用
     */
    int (*init_fs_context)(struct fs_context *);
    
    /*
     * 旧式 mount 接口
     */
    struct dentry *(*mount)(struct file_system_type *, int,
                            const char *, void *);
    
    /*
     * 卸载时调用
     */
    void (*kill_sb)(struct super_block *);
    
    struct module *owner;          /* 所属内核模块 */
    struct file_system_type *next; /* 链表 */
    struct hlist_head fs_supers;   /* 所有 super_block */
};

/*
 * 文件系统注册示例 (ext4)
 */
static struct file_system_type ext4_fs_type = {
    .owner      = THIS_MODULE,
    .name       = "ext4",
    .mount      = ext4_mount,
    .kill_sb    = kill_block_super,
    .fs_flags   = FS_REQUIRES_DEV,
};

// 模块加载时注册
register_filesystem(&ext4_fs_type);
```

## 九、总结

### 9.1 Mount 的本质作用

```
┌─────────────────────────────────────────────────────────────┐
│                     Mount 做了什么                          │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  1. 读取磁盘元数据                                          │
│     ┌──────────────────────────────────────────────────┐   │
│     │ 磁盘: [超级块] [块位图] [inode位图] [inode表] ... │   │
│     └────────────────────┬─────────────────────────────┘   │
│                          ↓ 读取                             │
│     ┌──────────────────────────────────────────────────┐   │
│     │ 内存: super_block, ext4_sb_info                   │   │
│     └──────────────────────────────────────────────────┘   │
│                                                             │
│  2. 初始化操作函数                                          │
│     ┌──────────────────────────────────────────────────┐   │
│     │ sb->s_op = &ext4_sops;                            │   │
│     │ inode->i_op = &ext4_file_inode_operations;        │   │
│     │ inode->i_fop = &ext4_file_operations;             │   │
│     └──────────────────────────────────────────────────┘   │
│                                                             │
│  3. 建立目录树连接                                          │
│     ┌──────────────────────────────────────────────────┐   │
│     │ 根目录树:  /  →  mnt  →  disk (挂载点)            │   │
│     │                          ↓ vfsmount               │   │
│     │ ext4 树:              /  →  file.txt              │   │
│     └──────────────────────────────────────────────────┘   │
│                                                             │
│  4. 使 VFS 系统调用可以工作                                 │
│     ┌──────────────────────────────────────────────────┐   │
│     │ open() → VFS → inode->i_fop->open()              │   │
│     │ read() → VFS → file->f_op->read()                │   │
│     │ write() → VFS → file->f_op->write()              │   │
│     └──────────────────────────────────────────────────┘   │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 9.2 一句话总结

**Mount 是将磁盘上的"死数据"转化为内核可操作的"活对象"的过程。没有 Mount，VFS 不知道如何解释磁盘上的字节，也没有函数指针来执行文件操作，更无法将文件系统接入统一的目录树让用户访问。**
