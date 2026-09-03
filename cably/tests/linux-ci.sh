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
# Acceptance test for the Linux CI workflow (.github/workflows/linux.yml) and the shared
# apt list (cably/linux/apt-packages.txt).  Static: it never builds anything.  Written
# BEFORE the workflow existed (RED: both files missing).  Every line is "  ok   ..." or
# "  FAIL ...", exit 1 on any FAIL.  Run from anywhere; needs python3, and actionlint on
# PATH (brew install actionlint / the download script in the workflow's lint job).
#
#  (a) apt-packages.txt: one package per line (# comments, blank lines), valid Debian
#      names, no duplicates, and the SAME set the Dockerfile.build RUN apt-get install
#      line names — the two must not drift (the Dockerfile is the port agent's file; the
#      workflow consumes the list, so equality is the contract).
#  (b) linux.yml: GPL notice at the top, parses as YAML, actionlint clean.
#  (c) linux.yml meaning: ubuntu-24.04, timeout-minutes 330, cancel-in-progress on the
#      same ref, checkout without submodules, apt install from apt-packages.txt, ccache
#      restored/saved with actions/cache keyed on the compiler AND the CMakeLists hashes,
#      builds through cably/linux/build.sh with -j$(nproc) (no cmake/ninja invocations of
#      its own — the scripts own the recipe), runs cably/linux/run-tests.sh under xvfb-run,
#      packages through cably/linux/package-deb.sh, uploads the .deb as an artifact, and
#      on a v* tag attaches it to a GitHub Release whose body carries the required sentence.
#      No secrets beyond the job's own GITHUB_TOKEN.  Trademark: the product is
#      "Cably Desktop"; KiCad is named only as attribution.
set -uo pipefail
FORK="${CABLY_FORK:-$(cd "$(dirname "$0")/../.." && pwd)}"
WF="$FORK/.github/workflows/linux.yml"
APT="$FORK/cably/linux/apt-packages.txt"
DOCKERFILE="$FORK/cably/linux/Dockerfile.build"
fail=0; ok(){ echo "  ok   $1"; }; bad(){ echo "  FAIL $1"; fail=1; }
section(){ echo; echo "== $1"; }
has(){ grep -qE -- "$1" "$WF"; }   # regex present in the workflow text

echo "linux-ci.sh: fork=$FORK"

# (a) ----------------------------------------------------------------------------
section "(a) cably/linux/apt-packages.txt"
if [ -f "$APT" ]; then
  ok "apt-packages.txt exists"
  PKGS=$(grep -vE '^\s*(#|$)' "$APT" | tr -d '\r' | sed 's/[[:space:]]*$//')
  N=$(echo "$PKGS" | grep -c .)
  [ "$N" -ge 40 ] && ok "$N packages listed" || bad "only $N packages listed (KiCad needs dozens: wx, occt, boost, ngspice, nng, protobuf, ...)"
  BADNAME=$(echo "$PKGS" | grep -vE '^[a-z0-9][a-z0-9+.-]+$' || true)
  [ -z "$BADNAME" ] && ok "every line is a single Debian package name" || bad "not package names: $(echo "$BADNAME" | head -3 | tr '\n' ' ')"
  DUP=$(echo "$PKGS" | sort | uniq -d)
  [ -z "$DUP" ] && ok "no duplicates" || bad "duplicates: $(echo "$DUP" | tr '\n' ' ')"
  for p in libwxgtk3.2-dev libngspice0-dev libnng-dev libprotobuf-dev protobuf-compiler libgit2-dev libsecret-1-dev libocct-foundation-dev xvfb dpkg-dev clang cmake ninja-build; do
    echo "$PKGS" | grep -qx "$p" && ok "lists $p" || bad "missing $p"
  done
  if [ -f "$DOCKERFILE" ]; then
    DIFF=$(python3 - "$DOCKERFILE" "$APT" <<'PY'
import re, sys
docker, apt = open(sys.argv[1]).read(), open(sys.argv[2]).read()
m = re.search(r'RUN\s+apt-get\s+update\s*&&\s*apt-get\s+install(.*?)&&\s*rm\s+-rf', docker, re.S)
if not m:
    print("could not find the RUN apt-get install ... && rm -rf block in the Dockerfile"); sys.exit(0)
words = []
for line in m.group(1).splitlines():
    line = line.strip()
    if not line or line.startswith('#'): continue          # docker drops # lines inside a RUN continuation
    words += [w for w in line.replace('\\', ' ').split() if not w.startswith('-')]
dpk = set(words)
apk = set(l.strip() for l in apt.splitlines() if l.strip() and not l.lstrip().startswith('#'))
only_docker = sorted(dpk - apk); only_apt = sorted(apk - dpk)
if only_docker: print("in Dockerfile.build but not apt-packages.txt: " + " ".join(only_docker))
if only_apt: print("in apt-packages.txt but not Dockerfile.build: " + " ".join(only_apt))
PY
)
    [ -z "$DIFF" ] && ok "apt-packages.txt == Dockerfile.build's apt-get install set (no drift)" || bad "drift: $DIFF"
  else echo "  skip drift check: $DOCKERFILE not present"; fi
else bad "missing $APT (factor the Dockerfile.build apt list into it)"; fi

# (b) ----------------------------------------------------------------------------
section "(b) .github/workflows/linux.yml exists, GPL notice, YAML, actionlint"
if [ ! -f "$WF" ]; then
  bad "missing $WF"
  echo; echo "linux-ci.sh: FAIL"; exit 1
fi
ok "linux.yml exists"
head -20 "$WF" | grep -q "GNU General Public License" && ok "GPL notice in the first 20 lines" || bad "no GPL notice at the top"
head -20 "$WF" | grep -q "Copyright (C) 2026 Cably" && ok "Cably copyright line" || bad "no Cably copyright line at the top"
head -20 "$WF" | grep -q "The KiCad Developers" && ok "KiCad attribution kept in the header" || bad "header drops the KiCad attribution"
if python3 -c 'import yaml' 2>/dev/null; then
  python3 -c "import yaml,sys; d=yaml.safe_load(open(sys.argv[1])); assert isinstance(d.get('jobs'),dict) and d['jobs'], 'no jobs'" "$WF" 2>"$FORK/.yaml.err" \
    && ok "parses as YAML (python) with a jobs map" || bad "YAML parse (python): $(head -3 "$FORK/.yaml.err")"; rm -f "$FORK/.yaml.err"
elif command -v ruby >/dev/null && ruby -ryaml -e '' 2>/dev/null; then
  ruby -ryaml -e 'd=YAML.load_file(ARGV[0]); abort("no jobs") unless d["jobs"].is_a?(Hash) && !d["jobs"].empty?' "$WF" 2>"$FORK/.yaml.err" \
    && ok "parses as YAML (ruby) with a jobs map" || bad "YAML parse (ruby): $(head -3 "$FORK/.yaml.err")"; rm -f "$FORK/.yaml.err"
else bad "no YAML parser (python3-yaml or ruby) available"; fi
if command -v actionlint >/dev/null; then
  OUT=$(cd "$FORK" && actionlint -no-color .github/workflows/linux.yml 2>&1)
  [ -z "$OUT" ] && ok "actionlint clean ($(actionlint -version 2>/dev/null | head -1))" || { bad "actionlint:"; echo "$OUT" | sed 's/^/       /' | head -30; }
else bad "actionlint not on PATH (brew install actionlint)"; fi

# (c) ----------------------------------------------------------------------------
section "(c) what the workflow does"
has '^name:\s*.*Linux' && ok "named as the Linux workflow" || bad "workflow name should mention Linux"
has 'runs-on:\s*ubuntu-24\.04' && ok "runner ubuntu-24.04" || bad "runner must be ubuntu-24.04 (the .deb targets it)"
has 'runs-on:\s*ubuntu-latest' && bad "ubuntu-latest used somewhere (pin 24.04)" || ok "no floating ubuntu-latest"
has 'timeout-minutes:\s*330' && ok "timeout-minutes 330 on the build" || bad "timeout-minutes 330 missing"
has '^concurrency:' && has 'cancel-in-progress:\s*true' && has 'github\.ref' && ok "concurrency: cancel in-progress runs on the same ref" || bad "concurrency group on github.ref with cancel-in-progress missing"
has '^\s+tags:' && has "'?v\*'?" && ok "push on tags v* triggers" || bad "tags v* trigger missing"
has 'actions/checkout@' && ok "actions/checkout used" || bad "no checkout"
has 'submodules:\s*(true|recursive)' && bad "submodules fetched (none needed)" || ok "no submodule checkout"
has 'cably/linux/apt-packages\.txt' && has 'apt-get install' && ok "apt install reads cably/linux/apt-packages.txt" || bad "apt install must come from cably/linux/apt-packages.txt"
has 'actions/cache' && ok "actions/cache used" || bad "actions/cache missing"
has 'ccache' && ok "ccache wired" || bad "ccache missing"
has 'hashFiles\(' && grep -E 'hashFiles\(' "$WF" | grep -q 'CMakeLists' && ok "cache key hashes CMakeLists" || bad "cache key must hash the CMakeLists files"
grep -A3 -E '^\s+key:' "$WF" | grep -qE 'compiler|gcc|cxx' && ok "cache key includes the compiler" || bad "cache key must include the compiler (version)"
has 'CMAKE_CXX_COMPILER_LAUNCHER=ccache' && ok "compiler launcher = ccache handed to build.sh (CABLY_CMAKE_EXTRA)" || bad "build must go through ccache (CMAKE_CXX_COMPILER_LAUNCHER=ccache via CABLY_CMAKE_EXTRA)"
has 'cably/linux/build\.sh' && ok "builds via cably/linux/build.sh" || bad "must call cably/linux/build.sh"
has 'CABLY_JOBS[=:]\s*"?\$\(nproc\)' && ok "CABLY_JOBS = \$(nproc)  (-j\$(nproc))" || bad "build must run with -j\$(nproc) (CABLY_JOBS=\$(nproc))"
has '\bcmake -S|ninja -C|-DKICAD_' && bad "the workflow duplicates build logic (cmake/ninja/-DKICAD_ flags belong to build.sh)" || ok "no build logic duplicated in the workflow"
has 'cably/linux/run-tests\.sh' && ok "tests via cably/linux/run-tests.sh" || bad "must call cably/linux/run-tests.sh"
grep -E 'run-tests\.sh' "$WF" | grep -q 'xvfb-run' && ok "run-tests.sh runs under xvfb-run" || bad "run-tests.sh must be run under xvfb-run"
has 'cably/linux/package-deb\.sh' && ok "packages via cably/linux/package-deb.sh" || bad "must call cably/linux/package-deb.sh"
has 'actions/upload-artifact@' && grep -A6 'actions/upload-artifact@' "$WF" | grep -q '\.deb' && ok "the .deb is uploaded as an artifact" || bad "upload-artifact of the .deb missing"
has 'softprops/action-gh-release@|gh release create' && ok "GitHub Release step present" || bad "no release step (softprops/action-gh-release or gh release create)"
has "startsWith\(github\.ref, 'refs/tags/v'\)" && ok "release gated on refs/tags/v*" || bad "release must be conditioned on startsWith(github.ref, 'refs/tags/v')"
grep -qF 'Linux x86_64 .deb for Ubuntu 24.04; based on KiCad; GPL-3.0-or-later; source = this repository' "$WF" && ok "release body carries the required sentence" || bad "release body must say: Linux x86_64 .deb for Ubuntu 24.04; based on KiCad; GPL-3.0-or-later; source = this repository"
OTHER=$(grep -oE 'secrets\.[A-Za-z0-9_]+' "$WF" | grep -v 'secrets\.GITHUB_TOKEN' || true)
[ -z "$OTHER" ] && ok "no secrets beyond GITHUB_TOKEN" || bad "unexpected secrets: $OTHER"
has 'permissions:' && has 'contents:\s*write' && ok "contents: write granted (release upload)" || bad "release upload needs permissions: contents: write"
has 'Cably Desktop' && ok "product name Cably Desktop appears" || bad "product name Cably Desktop missing"
MARK="Cably K"; MARK="${MARK}iCad"   # assembled so this file itself never carries the phrase
(cd "$FORK" && git grep -q "$MARK" -- .) && bad "trademark: the product name followed by the KiCad mark appears in the tree" || ok "trademark grep empty"
grep -qiE 'kicad desktop|kicad cably' "$WF" && bad "workflow names the product with the KiCad mark" || ok "workflow never names the product with the KiCad mark"

echo
[ "$fail" = 0 ] && echo "linux-ci.sh: PASS" || echo "linux-ci.sh: FAIL"
exit $fail
