---
name: "github-username-rename"
description: "Guides post-migration steps after a GitHub username change for Hugo + GitHub Actions projects. Invoke when user has just changed their GitHub username or asks how to migrate an existing repo to a new username."
---

# GitHub Username Rename Migration

This skill documents the **post-migration steps required after changing a GitHub username**, especially for projects using **Hugo + GitHub Actions → GitHub Pages** deployment.

---

## When to Invoke This Skill

Invoke this skill when **any** of the following is true:

- User has just changed their GitHub username
- User is asking what to do after a GitHub rename
- User notices their `<username>.github.io` URL is broken (404 or wrong redirect)
- User wants to migrate an existing project from one GitHub account to another
- User asks: "how do I update my repo after renaming my GitHub user?"

---

## Background: What GitHub Auto-Handles vs. What You Must Manually Update

GitHub automatically:
- Renames the `<username>.github.io` repository to match the new username
- Sets up **301 redirects** from old URLs to new URLs
- Renames ordinary repositories (URL changes, but repo identity preserved)
- Transfers stars / watches / issues to the new URL

GitHub does **NOT** auto-update:
- Code references to the old username (URLs in source, comments, docs)
- Git remote URLs on local clones
- CI/CD workflow files that hardcode the old `external_repository` path
- Personal Access Token scope (for fine-grained tokens)
- External links, social profiles, READMEs, blog posts

---

## Migration Checklist (Execute in Order)

### Phase 1: Pre-flight Verification

```bash
# 1. Confirm working tree is clean
cd <project-root>
git status

# 2. Confirm current remote
git remote -v
# Expected old: git@github.com:OLD_USER/<repo>.git
```

### Phase 2: Update GitHub-Side Web Settings

1. Open GitHub → Settings → Account → **Change username** → set to `NEW_USER`
2. Wait for the rename to complete (usually instant)
3. Verify new URL works: `https://github.com/NEW_USER`

### Phase 3: Update Local Git Remote

```bash
# Replace remote URL to use the new username
git remote set-url origin git@github.com:NEW_USER/<repo>.git

# Verify
git remote -v
# Should now show: git@github.com:NEW_USER/<repo>.git
```

### Phase 4: Update Project Source Code

Search the project for any hardcoded references to the old username:

```bash
# Search the entire project
grep -rn "OLD_USER" --exclude-dir=.git --exclude-dir=public --exclude-dir=resources
```

Common locations to update:

| File | What to update |
|------|----------------|
| `hugo.yaml` (or `config.toml`) | `baseurl: "https://NEW_USER.github.io/"` |
| `hugo.yaml` | `params.comments.giscus.repo: NEW_USER/<repo>` |
| `.github/workflows/*.yml` | `external_repository: NEW_USER/NEW_USER.github.io` |
| `README.md` | Any demo URLs, badges, social links |
| `content/**/*.md` | Inline links to GitHub user / repo |
| `archetypes/*.md` | Template frontmatter with hardcoded author URL |
| `data/*.yaml` | Any social profile / author metadata |

### Phase 5: Token Verification (For CI/CD)

| Token Type | Action Required |
|------------|-----------------|
| **Classic PAT** (account-scoped) | ✅ No change needed |
| **Fine-grained PAT** (repo-scoped) | ⚠️ Must be re-issued for `NEW_USER/NEW_USER.github.io` |

Update the token at:
`Repo → Settings → Secrets and variables → Actions → <SECRET_NAME>`

### Phase 6: Commit and Push

```bash
git add .
git status                # Review the staged changes carefully
git commit -m "rename: migrate to new GitHub username NEW_USER"
git push origin main
```

### Phase 7: Verify Deployment

1. Open `https://github.com/NEW_USER/<repo>/actions` → confirm workflow ran green
2. Wait 1-2 minutes for Pages to propagate
3. Visit `https://NEW_USER.github.io` → site should load
4. Visit the old URL `https://OLD_USER.github.io` → should 301-redirect to new URL
5. Check browser dev tools Network tab to confirm correct final URL

---

## Project-Specific Notes: Hugo + hugo-theme-stack

This project's deployment chain:

```
[Local Hugo source]
    ↓ git push to NEW_USER/blog-source
[GitHub Actions: peaceiris/actions-hugo@v2]
    ↓ hugo --minify → ./public
[GitHub Actions: peaceiris/actions-gh-pages@v3]
    ↓ cross-repo push with ${{ secrets.PERSONAL_TOKEN }}
[NEW_USER/NEW_USER.github.io] ← public Pages serve
```

**Three files MUST be updated for the chain to keep working:**

1. `hugo.yaml` → `baseurl`
2. `hugo.yaml` → `params.comments.giscus.repo`
3. `.github/workflows/gh-pages.yml` → `external_repository`

**Token in use:** Classic PAT (workflow scope) → **no re-issue required**.

---

## Edge Cases & Common Pitfalls

1. **Submodule URLs**: If a project uses Git submodules pointing to GitHub, check `.gitmodules` and update the `url = https://github.com/OLD_USER/...` lines
2. **Custom domain**: If using a `CNAME` file for custom domain, the domain is unaffected by username change (Pages settings stay)
3. **Giscus / GitHub OAuth apps**: Comment system callbacks may need re-registration if they hardcode the old repo URL
4. **Search engine cache**: Old URLs may linger in search results for weeks; consider submitting a sitemap to Google Search Console for the new URL
5. **Already-renamed but not yet migrated**: If you changed username on the GitHub side but not locally, all pushes will fail with "repository not found" — execute Phase 3+ immediately

---

## Rollback Strategy

GitHub allows re-renaming back to the original username **only if**:
- No one else has claimed the old username in the meantime
- The redirect chains have not been broken

**If rename fails partway:** GitHub will auto-restore the previous state, but verify the `<username>.github.io` repo name matches your current username.

---

## Quick-Reference One-Liner (Recovery)

If you ever land in a broken state, run this from project root (replace `OLD_USER`/`NEW_USER`):

```bash
OLD_USER=zhangquanhua1
NEW_USER=1bit2
git remote set-url origin "git@github.com:${NEW_USER}/$(basename $(git rev-parse --show-toplevel)).git"
grep -rl "$OLD_USER" . --exclude-dir=.git --exclude-dir=public --exclude-dir=resources \
  | xargs sed -i "s|$OLD_USER|$NEW_USER|g"
grep -rl "latteratter-coder" . --exclude-dir=.git --exclude-dir=public --exclude-dir=resources \
  | xargs sed -i "s|latteratter-coder|$NEW_USER|g"
git add . && git commit -m "rename: migrate to $NEW_USER" && git push
```
