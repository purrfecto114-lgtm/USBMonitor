#!/usr/bin/env bash
# USB Monitor — 一键发布脚本 (v1.1.0)
#
# 前置条件:
#   1. gh CLI 已安装 (本机 ~/.local/bin/gh)
#   2. 已完成 GitHub 认证 (gh auth login)
#   3. 或设置环境变量 GH_TOKEN / GITHUB_TOKEN
#
# 用法:
#   bash scripts/push_release.sh                    # 交互式(会问仓库名)
#   REPO=youruser/usb-monitor bash scripts/push_release.sh  # 指定仓库
#
# 本脚本完成:
#   - 创建 GitHub 仓库(如不存在)
#   - 配置 remote 并推送 main + tag v1.1.0
#   - 触发 "Build, tag and release" workflow
#   - 持续监测 workflow 运行直到 release 发布成功

set -euo pipefail

cd "$(dirname "$0")/.."
export PATH="$HOME/.local/bin:$PATH"

# 检查 gh 认证
if ! gh auth status >/dev/null 2>&1; then
  echo "❌ 未认证 GitHub。请先运行: gh auth login --hostname github.com --git-protocol https --web"
  exit 1
fi

# 读取 release 元数据
META=$(python scripts/release_meta.py check 2>&1)
TAG=$(echo "$META" | python -c "import sys,json; print(json.load(sys.stdin)['tag'])")
VERSION=$(echo "$META" | python -c "import sys,json; print(json.load(sys.stdin)['version'])")
echo "📋 Release: version=$VERSION tag=$TAG"

# 确定仓库名
REPO="${REPO:-}"
if [[ -z "$REPO" ]]; then
  USER=$(gh api user --jq .login 2>/dev/null || echo "")
  read -rp "输入目标仓库 (如 ${USER}/usb-monitor): " REPO
fi
echo "📦 目标仓库: $REPO"

# 创建仓库(如不存在)
if ! gh repo view "$REPO" >/dev/null 2>&1; then
  echo "🔧 仓库不存在,创建中..."
  REPO_NAME="${REPO#*/}"
  gh repo create "$REPO" --public --description "Windows USB storage tray monitor — v$VERSION" 2>/dev/null || \
    gh repo create "$REPO_NAME" --public --description "Windows USB storage tray monitor — v$VERSION"
  REPO=$(gh repo view --json nameWithOwner --jq .nameWithOwner)
fi

# 配置 remote
REMOTE_URL="https://github.com/${REPO}.git"
git remote remove origin 2>/dev/null || true
git remote add origin "$REMOTE_URL"
echo "🔗 remote: $REMOTE_URL"

# 推送 main + tag
echo "📤 推送 main 分支..."
git push -u origin main
echo "📤 推送 tag $TAG..."
git push origin "$TAG"

# 触发/监测 workflow
echo "🚀 workflow 'Build, tag and release' 将由 push 自动触发"
echo "⏳ 持续监测 workflow 运行..."

# 获取最新 workflow run
sleep 5
RUN_ID=""
for i in $(seq 1 60); do
  RUN_ID=$(gh run list --repo "$REPO" --workflow="release.yml" --limit 1 --json databaseId,status --jq '.[0].databaseId' 2>/dev/null || echo "")
  if [[ -n "$RUN_ID" ]]; then break; fi
  sleep 5
done

if [[ -z "$RUN_ID" ]]; then
  echo "⚠️  未找到 workflow run,请检查 Actions 页面"
  exit 1
fi

echo "🎯 监测 run #$RUN_ID (最多 90 分钟)"
gh run watch "$RUN_ID" --repo "$REPO" --exit-status || true

# 验证 release
echo "🔍 验证 release..."
if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
  echo ""
  echo "✅✅✅ Release $TAG 发布成功! ✅✅✅"
  gh release view "$TAG" --repo "$REPO" --json url,tagName,name,assets --jq '. | "URL: \(.url)\nTag: \(.tagName)\nName: \(.name)\nAssets: \(.assets | length) 个"'
  exit 0
else
  echo "❌ Release $TAG 未找到,请检查 Actions 日志"
  gh run view "$RUN_ID" --repo "$REPO" --log-failed 2>/dev/null | tail -30
  exit 1
fi
