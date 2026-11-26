#!/usr/bin/env bash
# One-shot local verification with minimal output: build -> ctest -> UI test. Exit code != 0 on any failure.
#   tools/check_all.sh            (BUILD_DIR=build by default; SKIP_UI=1 to skip the browser test)
set -u
cd "$(dirname "$0")/.."
B="${BUILD_DIR:-build}"
rc=0

[ -f "$B/build.ninja" ] || cmake -S . -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
cmake "$B" >/dev/null 2>&1 || true
if ! cmake --build "$B" -j"$(nproc)" >"$B/.check_build.log" 2>&1; then
  echo "BUILD: FAILED"
  grep -E "error|FAILED" "$B/.check_build.log" | head -15
  exit 1
fi
echo "BUILD: ok ($(grep -c 'warning:' "$B/.check_build.log") warnings)"

out="$(ctest --test-dir "$B" -j4 2>&1)"
echo "$out" | grep -E "tests passed|tests failed" | tail -1
if ! echo "$out" | grep -q "100% tests passed"; then
  echo "$out" | grep -E "\*\*\*Failed|Failed " | head -10
  rc=1
fi

if [ "${SKIP_UI:-0}" != "1" ] && [ -f tools/ui/ui_test.js ] && command -v node >/dev/null 2>&1 && [ -d /opt/node-tools/node_modules/playwright ]; then
  T="$(mktemp -d)"
  mkdir -p "$T/node_modules"
  ln -sfn /opt/node-tools/node_modules/playwright "$T/node_modules/playwright-core"
  ui="$(cd tools/ui && NODE_PATH="$T/node_modules" CHROME="${CHROME:-/opt/pw-browsers/chromium}" node ui_test.js ../../ui/index.html 2>&1)"
  echo "UI: $(echo "$ui" | grep -E 'checks passed' | tail -1)"
  if ! echo "$ui" | grep -q "checks passed" || echo "$ui" | grep -q "FAIL:"; then
    echo "$ui" | grep -E "FAIL:|Timeout" | head -10
    rc=1
  fi
  rm -rf "$T"
fi
exit $rc
