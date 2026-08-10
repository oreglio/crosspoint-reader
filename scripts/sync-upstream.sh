#!/usr/bin/env bash
#
# Pull upstream CrossInk into this fork, on a dedicated branch.
#
# Two things about this repo make a naive sync go wrong, and this script exists
# to get both right every time:
#
#   1. upstream/main carries only squashed release commits. They share no
#      history with this fork, so merging main replays code already present
#      here as a conflict. Measured on the v1.5.0 sync: 90 conflicting files
#      from main, 2 from development. Always merge development.
#
#   2. The merge moves the freeink-sdk submodule pointer, but leaves the
#      checked-out copy behind. A build right after the merge compiles the OLD
#      SDK and still reports success. The submodule must be updated first.
#
# Usage:  scripts/sync-upstream.sh [branch-name]

set -euo pipefail

UPSTREAM_REMOTE=upstream
UPSTREAM_BRANCH=development
BRANCH="${1:-sync/upstream-$(date +%Y%m%d)}"

cd "$(dirname "$0")/.."

if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
  echo "error: working tree has uncommitted changes. Commit or stash first." >&2
  exit 1
fi

echo "==> Fetching $UPSTREAM_REMOTE"
git fetch "$UPSTREAM_REMOTE" --tags

BASE="$(git merge-base HEAD "$UPSTREAM_REMOTE/$UPSTREAM_BRANCH")"
COUNT="$(git rev-list --count --no-merges "$BASE..$UPSTREAM_REMOTE/$UPSTREAM_BRANCH")"

if [[ "$COUNT" -eq 0 ]]; then
  echo "Already up to date with $UPSTREAM_REMOTE/$UPSTREAM_BRANCH."
  exit 0
fi

echo
echo "==> $COUNT incoming commits, by type:"
git log --format='%s' --no-merges "$BASE..$UPSTREAM_REMOTE/$UPSTREAM_BRANCH" \
  | sed 's/[(:].*//' | sort | uniq -c | sort -rn

# merge-tree exits non-zero when it finds conflicts, which is the case we most
# want to report. Capture first, judge after, so the exit status cannot be
# mistaken for "no conflicts".
CONFLICTS="$(git merge-tree --write-tree --name-only HEAD \
  "$UPSTREAM_REMOTE/$UPSTREAM_BRANCH" 2>/dev/null | grep '^CONFLICT' || true)"

echo
echo "==> Conflicts this merge will produce:"
if [[ -n "$CONFLICTS" ]]; then
  echo "$CONFLICTS" | sed 's/^/  /'
else
  echo "  none"
fi

echo
echo "==> Creating $BRANCH and merging"
git checkout -b "$BRANCH"

if ! git merge "$UPSTREAM_REMOTE/$UPSTREAM_BRANCH"; then
  echo
  echo "Conflicts to resolve, then 'git commit':"
  git diff --name-only --diff-filter=U | sed 's/^/  /'
  echo
  echo "Resolve them by preserving upstream's intent alongside this fork's own"
  echo "behaviour -- see the Git Workflow section of AGENTS.md. When done, run"
  echo "this script's tail by hand:"
  echo "  git submodule update --recursive && pio run -e default"
  exit 1
fi

echo
echo "==> Updating submodules to the merged pointers"
git submodule update --init --recursive

echo
echo "==> Building"
pio run -e default

cat <<'EOF'

Merged and built. The build proves compilation only.

Before publishing an OTA you must flash and confirm on the device:
  pio run -e default -t upload
EOF
