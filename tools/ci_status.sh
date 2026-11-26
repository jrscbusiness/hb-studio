#!/usr/bin/env bash
# Compact CI status for main.
#   tools/ci_status.sh [N]       last N runs, one line each (default 3)
#   tools/ci_status.sh --wait    wait until the run for the CURRENT HEAD finishes (use with run_in_background),
#                                then print one result line plus the failing tests/errors, if any.
set -u
cd "$(dirname "$0")/.."
BR="${BRANCH:-main}"

list() {
  gh run list --branch "$BR" --limit "${1:-3}" --json status,conclusion,displayTitle,createdAt,updatedAt,databaseId \
    --jq '.[] | "\(.databaseId)  \(.status)/\(.conclusion // "-")  \(.displayTitle[0:60])"' 2>&1
}

if [ "${1:-}" = "--wait" ]; then
  sha="$(git rev-parse HEAD)"
  for _ in $(seq 1 120); do                       # up to ~60 min, polling every 30 s
    row="$(gh run list --branch "$BR" --limit 15 --json headSha,status,conclusion,databaseId,displayTitle,workflowName \
      --jq ".[] | select(.headSha==\"$sha\" and .workflowName==\"${WORKFLOW:-ci}\") | \"\(.databaseId) \(.status) \(.conclusion // \"-\") \(.displayTitle[0:50])\"" 2>/dev/null | head -1)"
    if [ -n "$row" ]; then
      set -- $row
      id="$1"; status="$2"; concl="$3"
      if [ "$status" = "completed" ]; then
        echo "CI $concl  (run $id)  $(git log -1 --format=%s | cut -c1-60)"
        if [ "$concl" != "success" ]; then
          gh run view "$id" --log-failed 2>&1 | grep -E "error C[0-9]+|error:|\*\*\*Failed|\[  FAILED  \]|tests failed" | sed 's/^[^\t]*\t[^\t]*\t[^ ]* //' | sort -u | head -15
        fi
        [ "$concl" = "success" ] && exit 0 || exit 1
      fi
    fi
    sleep 30
  done
  echo "CI: timed out waiting for HEAD $sha"
  exit 2
fi

list "${1:-3}"
