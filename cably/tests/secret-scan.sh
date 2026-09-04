#!/usr/bin/env bash
# This file is part of Cably Desktop, a fork of KiCad, a free EDA CAD application.
#
# Copyright (C) 2026 Cably
# Copyright (C) The KiCad Developers, see AUTHORS.txt for contributors.
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation, either version 3 of the License, or (at your
# option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# Test for the secret scan in cably/scripts/check-gpl-headers.sh (the F0 rule: nothing we
# add may embed a Cably secret).  Written BEFORE the scan was widened - RED: it matched only
# the service-role env var name, the Stripe secret-key prefixes and RSA/OpenSSH key blocks.
# Every planted string is assembled at run time from fragments, so this file never contains
# one (the scan covers committed files: this file is scanned too, once tracked).
#
#  (a) the real tree: exit 0.  The publishable Supabase key in cably/src/cably_config.h is
#      a JWT whose payload says role=anon and must NOT be flagged; crashpad's test key
#      under thirdparty/ is upstream's at the base tag, not ours.
#  (b) each planted secret alone, in a throw-away git repo whose tagged 10.0.6 commit
#      holds it (the script diffs the tree against that tag and git-greps it, the way CI
#      sees the fork): the Supabase secret-key prefix (sb_ + secret_), a service-role JWT
#      (the role claim, base64 in the payload, at each of its three alignments - no
#      decoding) and the bare role name, an EC private key block, a PKCS#8 (BEGIN +
#      PRIVATE KEY) block, an AWS access key id (AKIA + 16 upper-case/digits): exit 1,
#      "SECRET-LIKE STRING COMMITTED", and the planted path named.
#  (c) the anon key alone (a copy of cably_config.h): exit 0.
#  (d) thirdparty/: a key block present at the base tag is upstream's (exit 0); one added
#      in a later commit is ours (exit 1).
#
# Usage: cably/tests/secret-scan.sh        CABLY_FORK: the tree (default: this script's)
set -uo pipefail
FORK="${CABLY_FORK:-$(cd "$(dirname "$0")/../.." && pwd)}"
SCRIPT="$FORK/cably/scripts/check-gpl-headers.sh"
fail=0; ok(){ echo "  ok   $1"; }; bad(){ echo "  FAIL $1"; fail=1; }
section(){ echo; echo "== $1"; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
G=(git -c user.name=test -c user.email=test@example.invalid -c commit.gpgsign=false -c init.defaultBranch=main)
[ -f "$SCRIPT" ] || { echo "secret-scan.sh: no $SCRIPT"; echo "secret-scan.sh: FAIL"; exit 1; }
echo "secret-scan.sh: $SCRIPT (tree $FORK)"

# the plants ---------------------------------------------------------------------------
SB="sb_"; SB+="secret_9f3a7c1e2b4d6081"
ROLE="service_"; ROLE+="role"
jwt(){ # $1 pad chars in the ref claim: shifts the base64 alignment of the role claim
  local payload; payload=$(printf '{"iss":"supabase","ref":"bhuzwogxxeyolpadhisl%s","role":"%s","iat":1771418769}' "$(printf '%*s' "$1" '' | tr ' ' y)" "$ROLE")
  printf 'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.%s.c2ln' "$(printf '%s' "$payload" | base64 | tr -d '\n=' | tr '+/' '-_')"
}
EC="-----BEGIN "; EC+="EC PRIVATE KEY-----"
P8="-----BEGIN "; P8+="PRIVATE KEY-----"
AWS="AKIA"; AWS+="IOSFODNN7EXAMPLE"
ANON=$(sed -n 's/^#define CABLY_SUPABASE_PUBLISHABLE_KEY *"\([^"]*\)".*/\1/p' "$FORK/cably/src/cably_config.h" | head -1)
[ -n "$ANON" ] && ok "anon key read from cably/src/cably_config.h (${#ANON} chars)" || bad "no CABLY_SUPABASE_PUBLISHABLE_KEY in cably/src/cably_config.h"

# helpers ------------------------------------------------------------------------------
# repo <name>: an empty dir; tag_base <dir>: commit everything as the tagged 10.0.6 base;
# commit <dir> <msg>; run <dir>: the script's output on stdout, its rc in $rc.
repo(){ mkdir -p "$T/$1"; (cd "$T/$1" && "${G[@]}" init -q); echo "$T/$1"; }
tag_base(){ (cd "$1" && "${G[@]}" add -A && "${G[@]}" commit -q --allow-empty -m base && "${G[@]}" tag 10.0.6); }
commit(){ (cd "$1" && "${G[@]}" add -A && "${G[@]}" commit -q -m "$2"); }
rc=0; OUT=""
run(){ OUT=$(cd "$1" && bash "$SCRIPT" 2>&1); rc=$?; }
expect_flagged(){ # $1 dir  $2 planted path  $3 label
  run "$1"
  if [ "$rc" != 0 ] && grep -q "SECRET-LIKE STRING COMMITTED" <<<"$OUT" && grep -q "^$2:" <<<"$OUT"; then ok "$3: flagged, $2 named (rc=$rc)"
  else bad "$3: NOT flagged (rc=$rc): $(grep -v '^gpl-headers: OK' <<<"$OUT" | head -3 | tr '\n' ' ')"; fi
}
expect_clean(){ # $1 dir  $2 label
  run "$1"
  if [ "$rc" = 0 ] && ! grep -q "SECRET-LIKE STRING COMMITTED" <<<"$OUT"; then ok "$2: clean (rc=0)"
  else bad "$2: rc=$rc: $(head -3 <<<"$OUT" | tr '\n' ' ')"; fi
}

# (a) -----------------------------------------------------------------------------------
section "(a) the real tree"
if [ -d "$FORK/.git" ] || git -C "$FORK" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  OUT=$(cd "$FORK" && bash "$SCRIPT" 2>&1); rc=$?
  [ "$rc" = 0 ] && ok "check-gpl-headers.sh on $FORK: $(tail -1 <<<"$OUT")" || bad "check-gpl-headers.sh on $FORK failed (rc=$rc): $(head -5 <<<"$OUT" | tr '\n' ' ')"
else bad "$FORK is not a git tree"; fi

# (b) -----------------------------------------------------------------------------------
section "(b) planted secrets, one per throw-away repo tagged 10.0.6"
D=$(repo sb); mkdir -p "$D/cably/src"; printf '#define CABLY_SUPABASE_SECRET_KEY "%s"\n' "$SB" >"$D/cably/src/keys.h"
tag_base "$D"; expect_flagged "$D" cably/src/keys.h "Supabase secret key (sb_ + secret_ prefix)"
for pad in 0 1 2; do
  D=$(repo "jwt$pad"); mkdir -p "$D/cably/src"; printf '#define CABLY_SUPABASE_KEY "%s"\n' "$(jwt "$pad")" >"$D/cably/src/keys.h"
  tag_base "$D"; expect_flagged "$D" cably/src/keys.h "service-role JWT, payload alignment $pad (role claim base64, no decoding)"
done
D=$(repo role); mkdir -p "$D/cably/src"; printf 'const char* kRole = "%s";\n' "$ROLE" >"$D/cably/src/role.cpp"
tag_base "$D"; expect_flagged "$D" cably/src/role.cpp "bare service-role name"
D=$(repo ec); mkdir -p "$D/cably/keys"; printf '%s\nMHQCAQEEIBkS7hK5x1pRYc0e8dWZq2aP0Yb3fUoJjM1nHc5tL9vXoAcGBSuBBAAK\n-----END EC PRIVATE KEY-----\n' "$EC" >"$D/cably/keys/signing.pem"
tag_base "$D"; expect_flagged "$D" cably/keys/signing.pem "EC private key block"
D=$(repo p8); mkdir -p "$D/cably/keys"; printf '%s\nMIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQg\n-----END PRIVATE KEY-----\n' "$P8" >"$D/cably/keys/server.pem"
tag_base "$D"; expect_flagged "$D" cably/keys/server.pem "PKCS#8 private key block"
D=$(repo aws); printf '[default]\naws_access_key_id = %s\naws_secret_access_key = wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY\n' "$AWS" >"$D/credentials"
tag_base "$D"; expect_flagged "$D" credentials "AWS access key id (AKIA + 16)"

# (c) -----------------------------------------------------------------------------------
section "(c) the publishable anon key alone must pass"
D=$(repo anon); mkdir -p "$D/cably/src"; cp "$FORK/cably/src/cably_config.h" "$D/cably/src/cably_config.h"
tag_base "$D"; expect_clean "$D" "cably_config.h (anon JWT)"
D=$(repo anon2); printf 'key=%s\n' "$ANON" >"$D/.env.public"
tag_base "$D"; expect_clean "$D" "the anon JWT in a bare file"

# (d) -----------------------------------------------------------------------------------
section "(d) thirdparty/: upstream's at the base tag vs added by us"
D=$(repo tp); mkdir -p "$D/thirdparty/crashpad/testdata"; printf '%s\nMHQCAQEEIBkS7hK5x1pRYc0e8dWZq2aP0Yb3fUoJjM1nHc5tL9vXoAcGBSuBBAAK\n-----END EC PRIVATE KEY-----\n' "$EC" >"$D/thirdparty/crashpad/testdata/test_key.pem"
tag_base "$D"; expect_clean "$D" "key block under thirdparty/ at the base tag (upstream's)"
mkdir -p "$D/thirdparty/ours"; printf '%s\nMHQCAQEEIBkS7hK5x1pRYc0e8dWZq2aP0Yb3fUoJjM1nHc5tL9vXoAcGBSuBBAAK\n-----END EC PRIVATE KEY-----\n' "$EC" >"$D/thirdparty/ours/deploy.pem"
commit "$D" "add a key under thirdparty"; expect_flagged "$D" thirdparty/ours/deploy.pem "key block added under thirdparty/ after the base tag (ours)"

echo
[ "$fail" = 0 ] && echo "secret-scan.sh: PASS" || echo "secret-scan.sh: FAIL"
exit $fail
