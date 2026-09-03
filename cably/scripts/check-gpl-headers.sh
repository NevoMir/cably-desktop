#!/usr/bin/env bash
# F0 test: every source file we ADD or MODIFY relative to the upstream tag
# must carry the GPL notice, and nothing we add may embed a Cably secret.
# Exit 1 on any violation. Run from the repo root.
set -euo pipefail
BASE="${1:-10.0.6}"
fail=0
while IFS= read -r f; do
  [ -f "$f" ] || continue
  case "$f" in *.cpp|*.h|*.hpp|*.c|*.cc|*.mm|*.py|*.cmake|CMakeLists.txt) ;; *) continue ;; esac
  if ! grep -q "GNU General Public License" "$f"; then
    echo "MISSING GPL HEADER: $f"; fail=1
  fi
done < <(git diff --name-only "$BASE" -- . ':!CHANGES.md' ':!NOTICE.md' ':!cably/scripts')
if git grep -nE "SUPABASE_SERVICE_ROLE|sk_live_|sk_test_|BEGIN (RSA|OPENSSH) PRIVATE KEY" -- . ':!cably/scripts/check-gpl-headers.sh' >/dev/null; then
  echo "SECRET-LIKE STRING COMMITTED"; git grep -nE "SUPABASE_SERVICE_ROLE|sk_live_|sk_test_|BEGIN (RSA|OPENSSH) PRIVATE KEY" -- . ':!cably/scripts/check-gpl-headers.sh' | head -5; fail=1
fi
[ "$fail" = 0 ] && echo "gpl-headers: OK ($(git diff --name-only "$BASE" | wc -l | tr -d ' ') files differ from $BASE)"
exit $fail
