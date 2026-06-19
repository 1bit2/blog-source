# 二进制（blog-source）

> 一个基于 **Hugo + GitHub Actions + GitHub Pages** 的个人技术博客，主题为 `hugo-theme-stack`。
> 在线访问：<https://1bit2.github.io>

---

## 一、项目概览

本仓库是博客的**源码仓**，通过 GitHub Actions 自动构建后，产物推送到独立的 GitHub Pages 仓库 `1bit2/1bit2.github.io`，最终对外提供服务。

| 仓库角色 | 仓库路径 | 可见性 | 用途 |
|----------|----------|--------|------|
| 源码仓（本仓） | `1bit2/blog-source` | 公开 | 写作、提交、版本管理 |
| 产物仓 | `1bit2/1bit2.github.io` | 公开 | 仅承载 Hugo 构建出的静态文件 |
| 主题仓 | `CaiJimmy/hugo-theme-stack` | 公开 | Git Submodule 引入 |

如果你只是想**写文章**，只看 **"四、内容创作指南"** 即可；其余章节按需查阅。

---

## 二、快速开始

5 分钟把博客跑起来。

```bash
# 1. 克隆（含 Submodule）
git clone --recurse-submodules git@github.com:1bit2/blog-source.git
cd blog-source
# 若已克隆但忘记 --recurse-submodules，补一次：
git submodule update --init --recursive

# 2. 安装 Hugo（需要 extended 版本）
brew install hugo                # macOS
sudo apt install hugo            # Ubuntu / Debian
choco install hugo-extended      # Windows

# 3. 启动本地预览
hugo server -D
# 浏览器打开 http://localhost:1313
# -D 表示包含 draft: true 的草稿

# 4. 新建第一篇文章
hugo new post/内存/我的第一篇文章.md
# 编辑 content/post/内存/我的第一篇文章.md 写正文

# 5. 提交部署
git add .
git commit -m "新增：我的第一篇文章"
git push origin main
# → GitHub Actions 自动构建并部署到 https://1bit2.github.io
```

---

## 三、部署架构

理解一次 push 是怎么变成全球可访问的网站。

```
本地编辑 Markdown
       ↓
   git push origin main
       ↓
GitHub Actions 触发（.github/workflows/gh-pages.yml）
       ↓
   ① actions/checkout@v3 ─── 拉取源码 + Submodule 主题
       ↓
   ② peaceiris/actions-hugo@v2 ─── 安装 Hugo extended
       ↓
   ③ hugo --minify ─── 构建产物 → ./public
       ↓
   ④ peaceiris/actions-gh-pages@v3 ─── 跨仓推送
       ↓
   1bit2/1bit2.github.io 仓的 main 分支
       ↓
   GitHub Pages 服务（1-2 分钟全球生效）
       ↓
https://1bit2.github.io
```

### 自动部署触发条件

| 事件 | 是否触发 | 备注 |
|------|----------|------|
| push 到 `main` 分支 | ✅ | 日常写作 |
| Pull Request 到 `main` | ✅ | 预览但不部署（`actions-gh-pages` 自动跳过 PR） |
| push 到其他分支 | ❌ | 不会触发 |
| 网页直接编辑 | ✅ | 等同于 push |

### 部署失败排查

| 现象 | 原因 | 解决 |
|------|------|------|
| Actions 失败：403 Forbidden | Token 权限不足 | 重新生成 PAT 并更新 Secret |
| Actions 失败：submodule not found | Submodule 未拉取 | 检查 `.gitmodules` 与 `submodules: recursive` |
| 网站 404 | 产物仓未启用 Pages | `1bit2/1bit2.github.io` → Settings → Pages → 选 main 分支 |
| 页面样式丢失 | 主题未构建 | Hugo 改用 `extended` 版本 |
| 评论不显示 | Giscus 缺 repoID | 在 [giscus.app](https://giscus.app) 重新生成并填入 `hugo.yaml` |

---

## 四、内容创作指南

> 本节是**最常用的部分**。写文章只需看懂 "动作 1"，新增分类只需看懂 "动作 2"。

### 当前分类（6 个）

`content/categories/` 与 `content/post/` 共享目录名：

- 内存
- 文件
- 网络
- 股票
- 设计模式
- 进程

### 动作 1：在已有分类下新增文章

**路径**：
```
content/post/<已存在分类>/<文章名>.md
```

**完整模板**（TOML 格式）：

```toml
+++
date = '2026-06-19'
title = '你的文章标题'
tags = [
    "内存",
    "指针",
]
categories = [
    "内存",
]
+++

# 文章正文从这里开始

## 小标题

正文内容...
```

**3 个必填字段**：

| 字段 | 必填 | 作用 |
|------|------|------|
| `date` | ✅ | Hugo 按此排序；缺失则文章不会出现在首页 |
| `title` | ✅ | 显示标题；缺失则用文件名 |
| `categories` | ✅ | 必须与所在目录名**完全一致**（包括中文） |

> 💡 `tags` 选填；`draft` 默认为 false，不写即发布。

**GitHub 网页端操作**：

1. 打开仓库 `1bit2/blog-source`
2. 路径填 `content/post/<分类>/<文章名>.md`
3. 复制上面的模板 + 写正文
4. 滚到底部 → `Commit changes`
5. 等 1-2 分钟，访问 `https://1bit2.github.io/p/<文章名>/` 验证

### 动作 2：新增一个分类

需要**两个文件同时建**：

| 文件 | 路径 |
|------|------|
| 分类索引页 | `content/categories/<新分类>/_index.md` |
| 文章目录占位 | `content/post/<新分类>/.gitkeep` |

**文件 1 模板**（YAML 格式）：

```yaml
---
title: "算法"
description: "算法与数据结构"
slug: "algo"
style:
    background: "#34A853"
    color: "#fff"
---
```

> 📌 `style.background` 与 `style.color` 可选，不填使用主题默认色。
> 📌 `slug` 控制 URL 短名，不填则用目录名（中文 URL 较长）。

**文件 2**（占位文件）：写一行任意内容即可。

### 草稿与发布

```toml
+++
date = '2026-06-19'
title = '草稿示例'
draft = true        # ← 改 false 或删除即发布
+++

# 仅本地 hugo server -D 可见，线上不显示
```

### 最小速记卡

```
新文章 →  content/post/<分类>/<名>.md       (TOML)
新分类 →  content/categories/<名>/_index.md  (YAML)
       + content/post/<名>/.gitkeep
```

---

## 五、本地开发

### 环境要求

| 工具 | 版本 | 说明 |
|------|------|------|
| Hugo | ≥ 0.110.0 extended | 主题依赖 SCSS 编译 |
| Git | 任意 | 含 Submodule 支持 |

### 常用命令

```bash
# 启动本地预览（含草稿、热重载）
hugo server -D --navigateToChanged

# 仅看线上版本（不含草稿）
hugo server

# 构建生产产物（产物在 ./public/）
hugo --minify

# 清理构建产物
rm -rf public/ resources/_gen/

# 统计文章数
find content/post -name "*.md" | wc -l

# 查看主题要求的 Hugo 版本
cat themes/hugo-theme-stack/theme.toml
```

### 本地新建文章（命令行方式）

```bash
# 自动应用 archetypes/default.md 模板
hugo new post/内存/指针越界分析.md

# 之后编辑生成的 .md 文件
```

> 💡 `archetypes/default.md` 控制 `hugo new` 生成的 Front Matter 模板，按需修改。

---

## 六、关键配置

### `hugo.yaml`（站点主配置，283 行中文注释）

最重要的几个字段：

```yaml
baseurl: "https://1bit2.github.io/"  # ← 必须与 GitHub 用户名一致
theme: hugo-theme-stack
title: 二进制
DefaultContentLanguage: en
disableLanguages: ["zh-cn", "ar", "ja", "ko", "fr", "de", "es"]
hasCJKLanguage: false                # ← 中文内容建议改为 true
```

### `.github/workflows/gh-pages.yml`（部署流水线）

```yaml
- name: Setup Hugo
  uses: peaceiris/actions-hugo@v2
  with:
    hugo-version: 'latest'
    extended: true                    # ← SCSS 编译必需

- name: Deploy
  uses: peaceiris/actions-gh-pages@v3
  with:
    personal_token: ${{ secrets.PERSONAL_TOKEN }}
    external_repository: 1bit2/1bit2.github.io   # ← 必须更新
    publish_dir: ./public
    keep_files: false
    publish_branch: main
```

### GitHub Personal Access Token

本项目使用 **Classic PAT**（账号级权限），用户名变更后**无需重新生成**。

> 若改用 Fine-grained Token（仓库级），改名后必须重新签发并更新 `PERSONAL_TOKEN` Secret。

---

## 七、进阶主题

### 7.1 修改 GitHub 用户名

详见：`.trae/skills/github-username-rename/SKILL.md`

简要流程：
1. GitHub 网页端 `Settings → Account → Change username`
2. 本地 `git remote set-url origin git@github.com:新用户名/blog-source.git`
3. 全局搜索替换源码中的旧用户名（重点：`hugo.yaml`、`*.yml` 工作流）
4. 提交推送 → Actions 自动重部署

### 7.2 升级主题

```bash
git submodule update --remote themes/hugo-theme-stack
git add themes/hugo-theme-stack
git commit -m "chore: 升级 hugo-theme-stack"
git push
```

升级前先看 [主题更新日志](https://github.com/CaiJimmy/hugo-theme-stack/releases)，留意 breaking changes。

### 7.3 自定义样式 / 模板

主题已就绪的情况下，**不要直接修改 `themes/`**（会被 Submodule 覆盖）。正确做法：

- **样式覆盖**：在 `assets/` 下创建同名 SCSS 文件（优先级高于主题）
- **模板覆盖**：在 `layouts/` 下创建同名 HTML 文件
- **静态资源**：放到 `static/` 目录

本项目已通过 `layouts/partials/footer/footer.html` 覆盖主题页脚。

---

## 八、参考信息

### 8.1 仓库结构

```
blog-source/
├── archetypes/             # hugo new 模板（自动生成 Front Matter）
├── assets/                 # 需 Hugo Pipes 处理的资源（SCSS、TS）
├── content/                # 所有内容（Markdown）
│   ├── _index.md          # 站点根
│   ├── post/              # 博文（按分类分子目录）
│   ├── categories/        # 分类列表页（与 post 同构）
│   └── page/              # 静态页（about、archives、search）
├── layouts/                # 自定义模板（覆盖主题）
│   └── partials/footer/footer.html
├── static/                 # 直接拷贝至产物根的静态资源
│   └── favicon.svg
├── themes/
│   └── hugo-theme-stack/  # Git Submodule
├── .github/
│   └── workflows/gh-pages.yml
├── .trae/
│   └── skills/             # 项目专属 Skill 文档
├── .gitmodules            # Submodule 配置
└── hugo.yaml              # 站点主配置
```

### 8.2 技术栈

| 组件 | 选型 | 说明 |
|------|------|------|
| 静态站点生成器 | Hugo (extended) | Go 编译、构建秒级 |
| 主题 | hugo-theme-stack | 卡片式、响应式、支持暗色模式 |
| 主题引入方式 | Git Submodule | 主题升级不影响主仓 |
| CI/CD | GitHub Actions | 免费、与 GitHub 原生集成 |
| 部署 Action | peaceiris/actions-gh-pages@v3 | 支持跨仓库部署 |
| Markdown 解析 | Goldmark | Hugo 默认、支持 LaTeX passthrough |
| 评论系统 | Giscus | 基于 GitHub Discussions（需自行开启） |
| 静态资源优化 | Hugo Pipes + `--minify` | 自动压缩 HTML/CSS/JS |

---

## 九、维护命令速查

```bash
# 拉取主题最新版本
git submodule update --remote themes/hugo-theme-stack

# 清理构建产物
rm -rf public/ resources/_gen/

# 本地起服务并热重载
hugo server -D --navigateToChanged

# 统计文章数
find content/post -name "*.md" | wc -l

# 同步远程
git fetch origin && git pull --rebase

# 推送
git push origin main
```

---

## 十、许可

博客文章内容采用 [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/)。

代码与配置：MIT License（继承主题的 [GPL-3.0](https://github.com/CaiJimmy/hugo-theme-stack/blob/master/LICENSE) 兼容性以主题协议为准）。
