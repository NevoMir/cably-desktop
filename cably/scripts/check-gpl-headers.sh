#!/usr/bin/env bash
# F0 test: every source file we ADD or MODIFY relative to the upstream tag
# must carry the GPL notice, and nothing we add may embed a Cably secret.
# Exit 1 on any violation. Run from the repo root.
#
# The secret scan (git grep, i.e. what is committed) matches, literally and without
# decoding anything: the Supabase service-role key names (the env var, the bare role
# name, and the role claim of a service-role JWT as it appears base64-encoded in the
# payload at each of its three alignments - the publishable key in cably/src/cably_config.h
# is a JWT whose payload says role=anon and does not match), Supabase secret keys
# (sb_secret_ prefix), Stripe secret keys, PEM private-key blocks (RSA, OpenSSH, EC, DSA
# and PKCS#8 "BEGIN PRIVATE KEY"), and AWS access key ids (AKIA + 16 upper-case/digits,
# not inside a longer base64 run).  Scope: every tracked file outside thirdparty/ (vendored
# upstream code we do not edit; crashpad's test key ships there at the base tag) plus any
# file under thirdparty/ that differs from the base tag - what we add there is ours.
# Test: cably/tests/secret-scan.sh.
set -euo pipefail
BASE="${1:-10.0.6}"
fail=0
while IFS= read -r f; do
  [ -f "$f" ] || continue
  case "$f" in *.cpp|*.h|*.hpp|*.c|*.cc|*.mm|*.py|*.cmake|*.sh|CMakeLists.txt) ;; *) continue ;; esac
  if ! grep -q "GNU General Public License" "$f"; then
    echo "MISSING GPL HEADER: $f"; fail=1
  fi
done < <(git diff --name-only "$BASE" -- . ':!CHANGES.md' ':!NOTICE.md' ':!cably/scripts')

SECRET_RE='SUPABASE_SERVICE_ROLE|service_role|InJvbGUiOiJzZXJ2aWNl|Jyb2xlIjoic2VydmljZV|icm9sZSI6InNlcnZpY2V|sb_secret_|sk_live_|sk_test_|BEGIN (RSA |OPENSSH |EC |DSA )?PRIVATE KEY|(^|[^A-Za-z0-9/+])AKIA[0-9A-Z]{16}([^A-Za-z0-9/+]|$)'
SELF=cably/scripts/check-gpl-headers.sh
# git grep: 0 = hits, 1 = none; anything else (a regex the local engine rejects, say) must
# fail the check loudly, never read as "no hits".
scan(){ # pathspecs...
  local out="" rc=0
  out=$(git grep -nE "$SECRET_RE" -- "$@") || rc=$?
  [ "$rc" -le 1 ] || { echo "check-gpl-headers.sh: git grep failed (rc=$rc); the secret scan did not run"; exit 2; }
  [ -z "$out" ] || echo "$out"
}
secret_hits(){
  scan . ':!thirdparty' ":!$SELF"
  local changed=()
  while IFS= read -r f; do [ -f "$f" ] && changed+=("$f"); done < <(git diff --name-only "$BASE" -- thirdparty)
  [ "${#changed[@]}" -eq 0 ] || scan "${changed[@]}"
}
HITS=$(secret_hits) || exit $?
if [ -n "$HITS" ]; then
  echo "SECRET-LIKE STRING COMMITTED"; echo "$HITS" | head -5; fail=1
fi
[ "$fail" = 0 ] && echo "gpl-headers: OK ($(git diff --name-only "$BASE" | wc -l | tr -d ' ') files differ from $BASE)"
exit $fail
