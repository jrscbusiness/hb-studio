#!/usr/bin/env bash
# Merge a branch (typically an agent's worktree branch) and auto-resolve additive FEATURE-block conflicts.
#   tools/merge_branch.sh worktree-agent-xxxx
# Does NOT commit: run tools/check_all.sh, then commit. Prints only what needs a human look.
set -u
cd "$(dirname "$0")/.."
[ $# -eq 1 ] || { echo "usage: $0 <branch>"; exit 2; }
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then echo "working tree has uncommitted changes; commit or stash first"; exit 2; fi

if git merge --no-edit "$1" >/tmp/merge_out.$$ 2>&1; then
  echo "merged cleanly: $(git log -1 --format=%s | cut -c1-70)"
  rm -f /tmp/merge_out.$$
  exit 0
fi
files="$(git diff --name-only --diff-filter=U)"
if [ -z "$files" ]; then cat /tmp/merge_out.$$ | tail -5; rm -f /tmp/merge_out.$$; exit 1; fi
rm -f /tmp/merge_out.$$
echo "conflicts in: $(echo $files | tr '\n' ' ')"
python3 -I tools/keep_both.py $files
rc=$?
git add $files
[ $rc -eq 0 ] && echo "all hunks additive; now run tools/check_all.sh and commit" || echo "REVIEW the flagged hunks/balance above, then tools/check_all.sh and commit"
exit $rc
