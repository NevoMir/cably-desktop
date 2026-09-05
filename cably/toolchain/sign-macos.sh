#!/usr/bin/env bash
# Developer ID signing of the built Cably Desktop bundle (F6). Signs EVERY code
# object inside-out with the hardened runtime, a secure timestamp and the
# entitlements the official KiCad app ships, then verifies the result. Strict:
# any codesign failure fails the script (and the build step that runs it).
#
#   sign-macos.sh <KiCad.app> [identity]
#
# identity: the name ("Developer ID Application: ... (TEAMID)") or the 40-hex
# SHA-1 of a code-signing identity in the keychain (security find-identity -v
# -p codesigning); default $CABLY_SIGNING_IDENTITY. The private key is never
# touched by this script - codesign uses it through the keychain. "-" (ad-hoc)
# is refused: the hardened runtime and timestamps need a real identity, and the
# dev build's linker ad-hoc signature is fine as it is.
# Entitlements: $CABLY_ENTITLEMENTS, default entitlements.plist next to this
# script (= kicad-mac-builder/signing/entitlements.plist, the four keys every
# code object of the official KiCad app carries).
#
# Why not kicad-mac-builder's bin/apple.py: it skips Python.framework entirely,
# never passes --timestamp, and the fork's own install-step signer can only sign
# ad-hoc (CMakeLists.txt FORCEs KICAD_OSX_SIGNING_ID "-"). Two things break the
# seal of an embedded python.org framework and are fixed here first ("seal
# hygiene", measured 2026-09-05):
#  1. a DANGLING symlink (Versions/X.Y/bin/python3-intel64 -> the x86_64 launcher
#     that arch-thinning removed): codesign seals it and every later verify of
#     the framework - and so of the app - fails with "No such file or directory";
#  2. __pycache__/*.pyc written by any run of the bundled Python after signing
#     ("a sealed resource is missing or invalid ... file added"); the official
#     KiCad.app in /Applications fails --deep --strict for exactly this reason
#     once it has been launched. Fix: strip __pycache__ and byte-compile the
#     stdlib with --invalidation-mode checked-hash BEFORE signing - Python then
#     finds every .pyc valid and writes nothing.
# Order (Apple: inside-out, each object before the bundle sealing it): every
# Mach-O regular file (by magic) -> Python.app helper -> the versioned framework
# bundle Versions/X.Y (the framework root resolves to it) -> the editor apps
# under Contents/Applications -> the app. Then codesign --verify --deep --strict.
# Never run the bundled Python or launch the app from the build tree after
# this (verify on the DMG mount instead) - cably/tests/signed.sh checks it all.
#
# Copyright (C) 2026 Cably <dev@cably.dev>
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
# more details. You should have received a copy of the GNU General Public
# License along with this program; see LICENSE.GPLv3 in this repository.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="${1:-}"; ID="${2:-${CABLY_SIGNING_IDENTITY:-}}"
ENT="${CABLY_ENTITLEMENTS:-$HERE/entitlements.plist}"
[ -n "$APP" ] && [ -d "$APP/Contents/MacOS" ] || { echo "sign-macos.sh: usage: sign-macos.sh <App.app> [identity]"; exit 2; }
[ -n "$ID" ] || { echo "sign-macos.sh: no signing identity (argument 2 or CABLY_SIGNING_IDENTITY)"; exit 2; }
[ "$ID" != "-" ] || { echo "sign-macos.sh: ad-hoc ('-') refused: Developer ID signing needs a real identity"; exit 2; }
[ -f "$ENT" ] || { echo "sign-macos.sh: no entitlements file at $ENT"; exit 2; }
APP="$(cd "$APP" && pwd)"
if ! security find-identity -v -p codesigning | grep -q -F "$ID"; then
  echo "sign-macos.sh: identity '$ID' not found by 'security find-identity -v -p codesigning'"; exit 2
fi
t0=$(date +%s); step(){ echo "sign-macos.sh: [$(( $(date +%s) - t0 ))s] $*"; }
SIGN=(codesign --force --sign "$ID" --options runtime --timestamp --entitlements "$ENT")
is_macho(){ case "$(head -c 4 "$1" 2>/dev/null | xxd -p)" in cffaedfe|cefaedfe|feedface|feedfacf|cafebabe|bebafeca) return 0;; esac; return 1; }

FW="$APP/Contents/Frameworks/Python.framework"; V=""; XY=""
if [ -L "$FW/Versions/Current" ]; then XY="$(basename "$(readlink "$FW/Versions/Current")")"; V="$FW/Versions/$XY"; fi

# 1. seal hygiene ---------------------------------------------------------------------------
step "seal hygiene in $APP"
n=0
while IFS= read -r l; do echo "  removing dangling symlink ${l#"$APP"/}"; rm -f "$l"; n=$((n+1)); done \
  < <(find "$APP" -type l ! -exec test -e {} \; -print)
step "removed $n dangling symlink(s)"
n=$(find "$APP/Contents" -name __pycache__ -type d -prune -print | wc -l | tr -d ' ')
find "$APP/Contents" -name __pycache__ -type d -prune -exec rm -rf {} +
step "removed $n __pycache__ dir(s)"
if [ -n "$V" ] && [ -x "$V/bin/python$XY" ]; then
  # checked-hash pyc: valid regardless of mtime, so a run after signing writes nothing.
  # Exit status ignored on purpose: the stdlib test suite ships files that must not compile.
  dirs=("$V/lib/python$XY"); [ -d "$APP/Contents/SharedSupport/scripting" ] && dirs+=("$APP/Contents/SharedSupport/scripting")
  PYTHONHOME="$V" "$V/bin/python$XY" -E -s -B -m compileall -q -f --invalidation-mode checked-hash "${dirs[@]}" >/dev/null 2>&1 \
    || echo "  compileall: some files did not compile (stdlib test fixtures - expected)"
  step "byte-compiled $(find "${dirs[@]}" -name '*.pyc' | wc -l | tr -d ' ') .pyc (checked-hash) under ${dirs[*]#"$APP"/}"
else
  echo "  no embedded Python.framework with Versions/Current -> skipping the .pyc step"
fi
find "$APP" -name '.DS_Store' -type f -delete
# leftovers of an interrupted codesign (it writes <file>.cstemp, then renames): a stale one is a
# Mach-O too, but signing its original removes it mid-batch -> "No such file or directory"
n=$(find "$APP" -name '*.cstemp' -type f -print -delete | wc -l | tr -d ' '); [ "$n" = 0 ] || step "removed $n stale *.cstemp file(s)"

# 2. sign inside-out ---------------------------------------------------------------------------
LIST="$(mktemp "${TMPDIR:-/tmp}/cably-sign.XXXXXX")"; trap 'rm -f "$LIST"' EXIT
# (the loop's status is that of its last command: a non-Mach-O last file must not trip set -e/pipefail)
find "$APP" -type f -size +1k ! -path '*/_CodeSignature/*' ! -name '*.cstemp' -print0 | while IFS= read -r -d '' f; do if is_macho "$f"; then printf '%s\0' "$f"; fi; done > "$LIST"
nm=$(tr -cd '\0' < "$LIST" | wc -c | tr -d ' ')
[ "$nm" -gt 0 ] || { echo "sign-macos.sh: no Mach-O files under $APP"; exit 1; }
step "signing $nm Mach-O files (hardened runtime, timestamp, entitlements) with '$ID'"
xargs -0 -n 25 "${SIGN[@]}" < "$LIST"
step "Mach-O files signed"
if [ -n "$V" ]; then
  [ -d "$V/Resources/Python.app" ] && { "${SIGN[@]}" "$V/Resources/Python.app"; step "signed Python.app helper"; }
  "${SIGN[@]}" "$V"; step "signed Python.framework/Versions/$XY (the framework root resolves to it)"
fi
for e in "$APP"/Contents/Applications/*.app; do [ -d "$e" ] || continue; "${SIGN[@]}" "$e"; step "signed $(basename "$e")"; done
"${SIGN[@]}" "$APP"; step "signed $(basename "$APP")"

# 3. verify -----------------------------------------------------------------------------------
codesign --verify --deep --strict --verbose=2 "$APP" 2>&1 | grep -v -E '^--(prepared|validated):' | sed 's/^/  /'
info="$(codesign -dvv "$APP" 2>&1)"
echo "$info" | grep -E '^(Identifier|TeamIdentifier|Authority=Developer ID Application|Timestamp|CodeDirectory)' | sed 's/^/  /'
case "$info" in *"flags=0x"*"runtime"*) ;; *) echo "sign-macos.sh: hardened runtime flag missing on $APP"; exit 1;; esac
case "$info" in *$'\nTimestamp='*) ;; *) echo "sign-macos.sh: no secure timestamp on $APP"; exit 1;; esac
step "done: $APP signed and verified (--deep --strict)"
