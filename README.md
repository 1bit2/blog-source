# 二进制（blog-source）

> 一个基于 **Hugo + GitHub Actions + GitHub Pages** 的个人技术博客，主题为 `hugo-theme-stack`。
> 在线访问：<https://1bit2.github.io>

---

## 项目概览

本仓库是博客的**源码仓库**，通过 GitHub Actions 自动构建后，产物推送到独立的 GitHub Pages 仓库 `1bit2/1bit2.github.io`，最终对外提供服务。

| 仓库角色 | 仓库路径 | 可见性 | 用途 |
|----------|----------|--------|------|
| 源码仓（本仓） | `1bit2/blog-source` | 公开 | 写作、提交、版本管理 |
| 产物仓 | `1bit2/1bit2.github.io` | 公开 | 仅承载 Hugo 构建出的静态文件 |
| 主题仓 | `CaiJimmy/hugo-theme-stack` | 公开 | Git Submodule 引入 |

---

## 部署架构

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

---

## 技术栈

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

## 仓库结构

```
blog-source/
├── archetypes/             # hugo new 模板（自动生成 Front Matter）
├── assets/                 # 需 Hugo Pipes 处理的资源（SCSS、TS）
├── content/                # 所有内容（Markdown）
│   ├── _index.md          # 站点根
│   ├── post/              # 博文（按分类分子目录）
│   │   ├── 内存/
│   │   ├── 网络/
│   │   ├── 股票/
│   │   └── 进程/
│   ├── categories/        # 分类列表页（与 post 同构）
│   │   ├── 内存/
│   │   ├── 文件/
│   │   ├── 网络/
│   │   ├── 股票/
│   │   ├── 设计模式/
│   │   └── 进程/
│   └── page/              # 静态页（about、archives、search）
├── layouts/                # 自定义模板（覆盖主题）
│   └── partials/
│       └── footer/
│           └── footer.html
├── static/                 # 直接拷贝至产物根的静态资源
│   └── favicon.svg
├── themes/
│   └── hugo-theme-stack/  # Git Submodule
├── .github/
│   └── workflows/
│       └── gh-pages.yml   # 部署流水线
├── .trae/
│   └── skills/             # 项目专属 Skill 文档
├── .gitmodules            # Submodule 配置
├── .gitignore
├── .hugo_build.lock
└── hugo.yaml              # 站点主配置（283 行中文注释）
```

---

## 本地开发

### 1. 克隆（含 Submodule）

```bash
git clone --recurse-submodules git@github.com:1bit2/blog-source.git
cd blog-source

# 若已克隆但忘记加 --recurse-submodules
git submodule update --init --recursive
```

### 2. 安装 Hugo

```bash
# macOS
brew install hugo

# Ubuntu / Debian
sudo apt install hugo

# Windows (Chocolatey)
choco install hugo-extended

# 验证版本（需要 extended 版本）
hugo version
# 应显示 hugo v0.X.X+extended ...
```

### 3. 启动本地预览

```bash
hugo server -D
# 默认监听 http://localhost:1313
# -D 包含草稿（draft: true 的文章）
```

### 4. 新建文章

```bash
# 自动应用 archetypes/default.md 模板
hugo new post/<分类>/<文章名>.md
# 例如
hugo new post/内存/指针越界分析.md
```

然后编辑生成的 `.md` 文件：

```markdown
---
title: "你的标题"
date: 2026-06-19
draft: false
tags: ["标签1", "标签2"]
categories: ["分类名"]
---

## 正文开始
```

### 5. 构建生产产物（本地验证）

```bash
hugo --minify
# 产物在 ./public/ 目录
# 推到 GitHub 即可（但本项目通过 Actions 自动构建，无需手动）
```

---

## 部署流程

### 自动触发（推荐）

```bash
git add .
git commit -m "新增：xxx 主题文章"
git push origin main
# → GitHub Actions 自动构建并部署到 1bit2.github.io
```

### 手动验证部署

1. 打开 [Actions 页面](https://github.com/1bit2/blog-source/actions) 查看运行状态
2. 等待 1-2 分钟
3. 访问 <https://1bit2.github.io> 确认

### 部署失败排查

| 现象 | 原因 | 解决 |
|------|------|------|
| Actions 失败：403 Forbidden | Token 权限不足 | 重新生成 PAT 并更新 Secret |
| Actions 失败：submodule not found | Submodule 未拉取 | 检查 `.gitmodules` 与 `submodules: recursive` |
| 网站 404 | 产物仓未启用 Pages | `1bit2/1bit2.github.io` → Settings → Pages → 选 main 分支 |
| 页面样式丢失 | 主题未构建 | Hugo 改用 `extended` 版本 |
| 评论不显示 | Giscus 缺 repoID | 在 [giscus.app](https://giscus.app) 重新生成并填入 `hugo.yaml` |

---

## 关键配置说明

### `hugo.yaml`（核心配置）

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

## 如何修改 GitHub 用户名

详见：`.trae/skills/github-username-rename/SKILL.md`

简要流程：
1. GitHub 网页端 `Settings → Account → Change username`
2. 本地 `git remote set-url origin git@github.com:新用户名/blog-source.git`
3. 全局搜索替换源码中的旧用户名（重点：`hugo.yaml`、`*.yml` 工作流）
4. 提交推送 → Actions 自动重部署

---

## 写作分类

当前共 **6 个分类**（`content/categories/` 与 `content/post/` 共享目录名）：

- 内存
- 文件
- 网络
- 股票
- 设计模式
- 进程

新增分类步骤：
1. `content/post/<新分类>/` 下新建文章目录
2. `content/categories/<新分类>/` 下新建 `_index.md` 与图标
3. （可选）`_index.md` 中定义 `style.background` 与 `style.color` 自定义配色

---

## 维护命令速查

```bash
# 拉取主题最新版本
git submodule update --remote themes/hugo-theme-stack
git add themes/hugo-theme-stack
git commit -m "chore: 升级 hugo-theme-stack"

# 清理构建产物
rm -rf public/ resources/_gen/

# 查看 Hugo 版本要求
cat themes/hugo-theme-stack/theme.toml

# 本地起服务并热重载
hugo server -D --navigateToChanged

# 统计文章数
find content/post -name "*.md" | wc -l
```

---

## 许可

博客文章内容采用 [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/)。

代码与配置：MIT License（继承主题的 [GPL-3.0](https://github.com/CaiJimmy/hugo-theme-stack/blob/master/LICENSE) 兼容性以主题协议为准）。
