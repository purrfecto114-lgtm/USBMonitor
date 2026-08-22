# USB Monitor · 自动发布指南

仓库使用 `.github/workflows/release.yml` 在默认分支上自动完成版本校验、测试、Windows 构建、打 Tag 和发布 GitHub Release。

## 发布一个新版本

1. 按 `MAJOR.MINOR.PATCH` 格式更新以下四处版本号，四者必须完全一致：
   - `pyproject.toml` 的 `[project].version`；
   - `usb_monitor/__init__.py` 的 `__version__`；
   - `usb_monitor/app.py` 的 `APP_VERSION`；
   - `build/windows_nuitka.bat` 的 `APP_VERSION`。
2. 在 `CHANGELOG.md` 顶部增加对应章节，例如：

   ```markdown
   ## [1.1.0] — 2026-07-23

   ### Added
   - 新功能说明。
   ```

3. 将这些修改推送或合并到 GitHub 默认分支。只要 `v1.1.0` Release 尚不存在，workflow 就会开始发布。

## Workflow 做了什么

- 校验四处版本号和 Changelog 章节；
- 如果同名 Release 已存在则安全跳过；
- 如果同名 Tag 指向其他提交则失败，避免覆盖历史版本；
- 在 Linux 和 Windows 上运行测试；
- 在 Windows x64 上用 Nuitka 构建 onefile EXE；
- 对打包后的 EXE 执行 `--startup-status` 烟测；
- 生成独立 EXE、便携 ZIP 和 `SHA256SUMS.txt`；
- 用内置 `GITHUB_TOKEN` 创建 `vX.Y.Z` Tag 并发布 GitHub Release；
- 保留 Nuitka dependency report 作为 14 天的 Actions artifact。

发布构建默认关闭 UPX，以降低杀毒软件误报并提高 CI 构建稳定性；本地仍可按 `docs/BUILD.md` 使用 UPX。

## 手动重试

在 GitHub 的 **Actions → Build, tag and release → Run workflow** 中从默认分支运行即可。若先前失败时已经创建了 Tag，只要该 Tag 仍指向同一提交且 Release 尚未存在，workflow 会继续完成发布。

不需要额外创建 PAT 或仓库 Secret。发布 Job 仅申请 `contents: write`，其余 Job 保持只读权限。
