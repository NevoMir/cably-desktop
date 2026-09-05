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
# Unit test for the .deb version parser, cably/linux/deb-version.sh (sourced by
# cably/linux/package-deb.sh and by cably/tests/deb.sh).  Written BEFORE the parser
# existed (RED: nothing to source), after the Linux CI run actions/runs/33969108226
# failed in package-deb.sh: the release tag v10.0.6-cably.26 (on 1f248de678) had become
# the nearest tag, so the build tree's record (KICAD_VERSION, KiCad's `git describe
# --dirty`) read "v10.0.6-cably.26-2-gec55de20a0", and the parser, which knew only
# "<kicad tag>-<N>-g<rev>[-dirty]", rejected it ("cannot derive <tag>-<N>-g<rev> ...").
#
# The contract (cably/linux/README.md, "The version"): the package Version stays
# <kicad>+cably.<yyyymmdd>.<COUNT>.g<rev>[.dirty] with COUNT = commits since the KiCad
# tag, WHATEVER tag git describe found, so versions stay monotonic across a release tag.
# `cably_deb_parse_record <record> [<checkout>] [<commit hash>]` accepts
#   (1) 10.0.6-<N>-g<rev>[-dirty]             COUNT = N
#   (2) v10.0.6-cably.<M>-<N>-g<rev>[-dirty]  COUNT = M + N  (M = the release tag's commits
#       since 10.0.6, N = commits after the release tag: v10.0.6-cably.26-2-g... -> 28)
#   (3) v10.0.6-cably.<M>[-dirty]             COUNT = M; no rev in the record, so the rev
#       is the record's own commit hash when one is given (kicad_build_version.h carries
#       KICAD_COMMIT_HASH next to KICAD_VERSION), else the tag's commit in the checkout,
#       else the checkout's HEAD; DV_REV_FROM says which
#   (4) 10.0.6[-dirty]                        COUNT = 0, rev as in (3)
#   anything else                             non-zero return
# and sets DV_UPSTREAM DV_COUNT DV_REV DV_DIRTY DV_FORM DV_REV_FROM; `cably_deb_version
# <yyyymmdd>` prints the Version built from them (1 when there is no rev).  Where dpkg
# exists, `dpkg --compare-versions` proves the ordering: a build before the release tag
# sorts below a build after it (across days AND on the same day), .dirty sorts after the
# clean build, and a rebuild of the tagged commit keeps its version.
#
# Usage: cably/tests/deb-version.sh        CABLY_FORK: the checkout (default: this tree)
set -uo pipefail
FORK="${CABLY_FORK:-$(cd "$(dirname "$0")/../.." && pwd)}"
LIB="$FORK/cably/linux/deb-version.sh"
fail=0; ok(){ echo "  ok   $1"; }; bad(){ echo "  FAIL $1"; fail=1; }
section(){ echo; echo "== $1"; }

echo "deb-version.sh: parser $LIB"
section "(a) the library"
if [ -f "$LIB" ]; then ok "cably/linux/deb-version.sh exists"; else bad "no $LIB"; echo; echo "deb-version.sh: FAIL"; exit 1; fi
grep -q "GNU General Public License" "$LIB" && ok "carries the GPL header" || bad "no GPL header"
# shellcheck disable=SC1090
. "$LIB"
declare -F cably_deb_parse_record >/dev/null && ok "defines cably_deb_parse_record" || { bad "cably_deb_parse_record not defined by $LIB"; echo; echo "deb-version.sh: FAIL"; exit 1; }
declare -F cably_deb_version >/dev/null && ok "defines cably_deb_version" || bad "cably_deb_version not defined by $LIB"

# The checkout's revs the no-rev forms resolve to.
HEADREV=$(git -C "$FORK" rev-parse --short=10 HEAD 2>/dev/null || true)
TAGREV=$(git -C "$FORK" rev-parse --short=10 'v10.0.6-cably.26^{commit}' 2>/dev/null || true)
KTAGREV=$(git -C "$FORK" rev-parse --short=10 '10.0.6^{commit}' 2>/dev/null || true)
[ -n "$HEADREV" ] && ok "checkout $FORK: HEAD $HEADREV, tag v10.0.6-cably.26 -> ${TAGREV:-absent}, tag 10.0.6 -> ${KTAGREV:-absent}" || echo "  note $FORK is not a git checkout (or no git): the git-resolved cases are skipped"

reset(){ DV_UPSTREAM=; DV_COUNT=; DV_REV=; DV_DIRTY=; DV_FORM=; DV_REV_FROM=; }
expect(){ # label record checkout hash want_upstream want_count want_rev want_dirty(yes|no) [rev-from substring]
  local label="$1" rec="$2" src="$3" hash="$4" wu="$5" wc="$6" wr="$7" wd="$8" from="${9:-}"
  reset
  if ! cably_deb_parse_record "$rec" "$src" "$hash"; then bad "$label: '$rec' rejected"; return; fi
  local gd; [ -n "$DV_DIRTY" ] && gd=yes || gd=no
  local got="$DV_UPSTREAM/$DV_COUNT/${DV_REV:-<none>}/$gd" want="$wu/$wc/${wr:-<none>}/$wd"
  if [ "$got" = "$want" ]; then ok "$label: '$rec' -> upstream/COUNT/rev/dirty $got (form $DV_FORM, rev from $DV_REV_FROM)"
  else bad "$label: '$rec' -> $got, want $want (form ${DV_FORM:-?}, rev from ${DV_REV_FROM:-?})"; fi
  [ -z "$from" ] || { grep -qi "$from" <<<"$DV_REV_FROM" && ok "$label: DV_REV_FROM names '$from': $DV_REV_FROM" || bad "$label: DV_REV_FROM '$DV_REV_FROM' does not say '$from'"; }
}
reject(){ # label record
  reset
  if cably_deb_parse_record "$2" "$FORK" ""; then bad "$1: '$2' accepted as $DV_UPSTREAM/$DV_COUNT/$DV_REV (form $DV_FORM)"; else ok "$1: '$2' rejected (non-zero)"; fi
}

section "(b) the record forms"
expect "(1) KiCad tag, N commits"        "10.0.6-24-g77d2f34d53"           "" "" 10.0.6 24 77d2f34d53 no  record
expect "(1) KiCad tag, dirty"            "10.0.6-25-gf5db84df12-dirty"     "" "" 10.0.6 25 f5db84df12 yes record
expect "(2) release tag + N (the CI case)" "v10.0.6-cably.26-2-gec55de20a0" "" "" 10.0.6 28 ec55de20a0 no record
expect "(2) release tag + N, dirty"      "v10.0.6-cably.26-3-g0123abcdef-dirty" "" "" 10.0.6 29 0123abcdef yes
expect "(2) larger counts add"           "v10.0.6-cably.120-15-gabcdef0123" "" "" 10.0.6 135 abcdef0123 no
expect "(3) exact release tag, rev from the record's commit hash" "v10.0.6-cably.26" "" "1f248de678add7c922149bf158ad83a0f0f70475" 10.0.6 26 1f248de678 no hash
expect "(3) exact release tag, dirty, hash" "v10.0.6-cably.26-dirty" "" "1f248de678add7c922149bf158ad83a0f0f70475" 10.0.6 26 1f248de678 yes hash
expect "(4) exactly the KiCad tag, hash" "10.0.6" "" "caf7377e9c000000000000000000000000000000" 10.0.6 0 caf7377e9c no hash
expect "(4) exactly the KiCad tag, dirty, hash" "10.0.6-dirty" "" "caf7377e9c000000000000000000000000000000" 10.0.6 0 caf7377e9c yes hash
expect "(1) with a hash: the record's own rev wins" "10.0.6-24-g77d2f34d53" "" "1f248de678add7c922149bf158ad83a0f0f70475" 10.0.6 24 77d2f34d53 no record
expect "(2) other KiCad versions"        "v10.1.0-cably.3-1-g1234567890"   "" "" 10.1.0 4 1234567890 no
expect "(1) other KiCad versions"        "9.0.4-7-gabcdef1234"             "" "" 9.0.4 7 abcdef1234 no
if [ -n "$HEADREV" ]; then
  if [ -n "$TAGREV" ]; then
    expect "(3) exact release tag, no hash: the tag's commit in the checkout" "v10.0.6-cably.26" "$FORK" "" 10.0.6 26 "$TAGREV" no "tag v10.0.6-cably.26"
    expect "(3) exact release tag, dirty, no hash" "v10.0.6-cably.26-dirty" "$FORK" "" 10.0.6 26 "$TAGREV" yes "tag"
  else
    expect "(3) exact release tag, no hash, tag absent here: HEAD" "v10.0.6-cably.26" "$FORK" "" 10.0.6 26 "$HEADREV" no HEAD
  fi
  expect "(3) a release tag the checkout does not have: HEAD, and says so" "v10.0.6-cably.999" "$FORK" "" 10.0.6 999 "$HEADREV" no HEAD
  if [ -n "$KTAGREV" ]; then expect "(4) exactly the KiCad tag, no hash: the tag's commit" "10.0.6" "$FORK" "" 10.0.6 0 "$KTAGREV" no "tag 10.0.6"
  else expect "(4) exactly the KiCad tag, no hash, tag absent: HEAD" "10.0.6" "$FORK" "" 10.0.6 0 "$HEADREV" no HEAD; fi
  expect "(3) a bad hash is ignored, git answers" "v10.0.6-cably.26" "$FORK" "not-a-hash" 10.0.6 26 "${TAGREV:-$HEADREV}" no
fi
expect "(3) exact release tag, no checkout, no hash: parsed, rev unavailable" "v10.0.6-cably.26" "" "" 10.0.6 26 "" no unavailable
expect "(3) exact release tag, a checkout that is not git: parsed, rev unavailable" "v10.0.6-cably.26" "/nonexistent-cably-checkout" "" 10.0.6 26 "" no unavailable

section "(c) rejected records"
reject "garbage" "garbage"
reject "empty" ""
reject "trailing junk" "10.0.6-24-g77d2f34d53-extra"
reject "no g before the rev" "10.0.6-24-77d2f34d53"
reject "non-numeric M" "v10.0.6-cably.x-2-gec55de20a0"
reject "a foreign tag" "v10.0.6-other.26-2-gec55de20a0"
reject "no v before the release tag" "10.0.6-cably.26-2-gec55de20a0"
reject "uppercase hex" "10.0.6-24-g77D2F34D53"

section "(d) the Version string and its ordering"
reset; cably_deb_parse_record "10.0.6-26-g1f248de678" "" ""; A=$(cably_deb_version 20260904)
[ "$A" = "10.0.6+cably.20260904.26.g1f248de678" ] && ok "10.0.6-26-g1f248de678 on 20260904 -> $A" || bad "10.0.6-26-g1f248de678 on 20260904 -> '$A'"
reset; cably_deb_parse_record "v10.0.6-cably.26-2-gec55de20a0" "" ""; B=$(cably_deb_version 20260905); B0=$(cably_deb_version 20260904)
[ "$B" = "10.0.6+cably.20260905.28.gec55de20a0" ] && ok "v10.0.6-cably.26-2-gec55de20a0 on 20260905 -> $B" || bad "v10.0.6-cably.26-2-gec55de20a0 on 20260905 -> '$B'"
reset; cably_deb_parse_record "v10.0.6-cably.26-2-gec55de20a0-dirty" "" ""; BD=$(cably_deb_version 20260905)
[ "$BD" = "10.0.6+cably.20260905.28.gec55de20a0.dirty" ] && ok "dirty -> $BD" || bad "dirty -> '$BD'"
reset; cably_deb_parse_record "v10.0.6-cably.26" "" "1f248de678add7c922149bf158ad83a0f0f70475"; T=$(cably_deb_version 20260904)
[ "$T" = "$A" ] && ok "a rebuild of the tagged commit after tagging keeps its version: v10.0.6-cably.26 (hash 1f248de678...) -> $T = $A" || bad "v10.0.6-cably.26 with the hash -> '$T', want $A"
reset; cably_deb_parse_record "v10.0.6-cably.26" "" ""
if cably_deb_version 20260904 >/dev/null 2>&1; then bad "cably_deb_version succeeded without a rev"; else ok "cably_deb_version fails (non-zero) when the rev is unavailable"; fi
if command -v dpkg >/dev/null; then
  dpkg --compare-versions "$A" lt "$B"  && ok "dpkg: $A lt $B (the release tag does not reset the count: monotonic across days)" || bad "dpkg: $A is not lt $B"
  dpkg --compare-versions "$A" lt "$B0" && ok "dpkg: $A lt $B0 (same day, 26 lt 28)" || bad "dpkg: $A is not lt $B0"
  dpkg --compare-versions "$B" lt "$BD" && ok "dpkg: $B lt $BD (.dirty sorts after the clean build)" || bad "dpkg: $B is not lt $BD"
  dpkg --compare-versions "$B0" lt "$B" && ok "dpkg: $B0 lt $B (a later day sorts greater)" || bad "dpkg: $B0 is not lt $B"
  dpkg --compare-versions "10.0.6+cably.20260904.26.gX" lt "10.0.6+cably.20260905.28.gY" && ok "dpkg: 10.0.6+cably.20260904.26.gX lt 10.0.6+cably.20260905.28.gY" || bad "dpkg: 10.0.6+cably.20260904.26.gX is not lt 10.0.6+cably.20260905.28.gY"
  dpkg --compare-versions "10.0.6+cably.20260905.26.gX" lt "10.0.6+cably.20260905.28.gY" && ok "dpkg: same day, 10.0.6+cably.20260905.26.gX lt 10.0.6+cably.20260905.28.gY" || bad "dpkg: same day 26 is not lt 28"
else echo "  note no dpkg here: the dpkg --compare-versions ordering is not checked (run in the build container)"; fi

echo
[ "$fail" = 0 ] && echo "deb-version.sh: PASS" || echo "deb-version.sh: FAIL"
exit $fail
