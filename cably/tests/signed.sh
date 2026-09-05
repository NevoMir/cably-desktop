#!/usr/bin/env bash
# F6 acceptance: the shipped DMG and the app inside it are signed with the Cably
# Developer ID, hardened, timestamped, notarized and stapled - what Gatekeeper
# needs to open the download on another Mac without a warning. Written BEFORE
# the signing step exists; it must FAIL on the unsigned 2026-09-05 DMG (ad-hoc
# linker signatures, no team, no ticket) and PASS, with the team/bundle-id/product
# overridden, on KiCad's own signed+notarized 10.0.1 DMG (the model we match).
#
# Given a DMG (default: the newest *.dmg under <build dir>/dmg) it mounts it
# read-only at a private mountpoint and asserts:
#  (a) the DMG file itself carries a valid Developer ID signature
#      (codesign --verify --verbose=2: "valid on disk" + "satisfies its Designated
#      Requirement"; TeamIdentifier = $TEAM; Authority "Developer ID Application:";
#      a secure Timestamp);
#  (b) the app bundle ("<product>.app" at the top level, else the first real
#      *.app within two levels) passes codesign --verify --deep --strict, its
#      signature is Developer ID (Authority chain "Developer ID Application: ...",
#      "Developer ID Certification Authority", "Apple Root CA"), TeamIdentifier =
#      $TEAM, flags include "runtime" (hardened runtime) and not "adhoc", a secure
#      Timestamp is present and Identifier = $BUNDLE_ID; the main executable
#      Contents/MacOS/<CFBundleExecutable> is signed the same way;
#  (c) EVERY nested code object is signed by the same team with the hardened
#      runtime and a timestamp: every regular file that is a Mach-O (by magic;
#      this covers Contents/MacOS/*, Contents/Frameworks/*.dylib, the Python
#      framework's Versions/<X.Y>/Python, bin/python3*, lib-dynload/*.so and
#      site-packages *.so, Contents/PlugIns/*.kiface and plugin .so, the six
#      editor executables), plus the bundles themselves: Python.framework (root
#      and Versions/<X.Y>), Resources/Python.app (codesign --verify --strict) and
#      Contents/Applications/*.app (codesign --verify: their Contents/Frameworks
#      -> ../../../Frameworks symlink fails --strict on a NESTED bundle even in
#      the official app, while the outer --deep --strict passes). Every unsigned, ad-hoc,
#      other-team, non-hardened or untimestamped object is listed;
#  (d) the entitlements of the main executable, of each editor app, of
#      Contents/MacOS/kicad-cli, of the Python.app helper and of bin/python3.<X>
#      are exactly the reference set of the official KiCad app (= kicad-mac-
#      builder/signing/entitlements.plist: automation.apple-events,
#      cs.allow-dyld-environment-variables, cs.disable-executable-page-protection,
#      cs.disable-library-validation - the last two are what an embedded Python
#      with C extensions needs under the hardened runtime);
#  (e) notarization: "xcrun stapler validate" passes on the DMG; "spctl --assess
#      --type open --context context:primary-signature" on the DMG is "accepted"
#      with "source=Notarized Developer ID"; "spctl --assess --type execute" on
#      the app is "accepted" with "source=Notarized Developer ID"; "xcrun stapler
#      validate" on the app passes (Apple's recommendation for an app shipped in
#      a DMG, so a copy dragged out of the image opens offline; KiCad does not
#      staple its app - CABLY_SIGN_APP_STAPLE=0 turns this one into a note);
#  (f) every launcher symlink in the top-level "<product>/" folder resolves to
#      a bundle INSIDE the signed app whose own signature verifies, and
#      the app launches from the mount (portable.sh's check: Contents/MacOS/kicad
#      with a private KICAD_CONFIG_HOME is alive after $LAUNCH_SECS s, its stderr
#      has none of "Python path configuration", "Fatal Python error", "Unhandled
#      exception", "Library Validation", "code signature", and it exits on
#      SIGTERM);
# then detaches. Every check prints an "ok"/"FAIL" line; exit 1 on any FAIL.
# A path ending in .app instead of .dmg checks that bundle in place: (a), the
# DMG parts of (e) and the launcher part of (f) are skipped.
#
# Usage: cably/tests/signed.sh [path/to/file.dmg | path/to/App.app]
#   CABLY_BUILD_DIR        build dir (default: <fork>/../build.noindex)
#   CABLY_SIGN_TEAM        Team ID (default Z9HUH3VGHM)
#   CABLY_SIGN_BUNDLE_ID   main bundle id (default org.cably.desktop)
#   CABLY_SIGN_PRODUCT     product name (default "Cably Desktop"): <product>.app
#                          and the "<product>/" launchers folder
#   CABLY_SIGN_APP_STAPLE  0: an unstapled app is a note, not a FAIL (default 1)
#   CABLY_SIGN_LAUNCH      0 to skip the launch check (default 1)
# Positive control (must PASS):
#   CABLY_SIGN_TEAM=9FQDHNY6U2 CABLY_SIGN_BUNDLE_ID=org.kicad.kicad \
#   CABLY_SIGN_PRODUCT=KiCad CABLY_SIGN_APP_STAPLE=0 cably/tests/signed.sh "KiCad 10.0.1.dmg"
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
set -uo pipefail
FORK="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${CABLY_BUILD_DIR:-$FORK/../build.noindex}"
TEAM="${CABLY_SIGN_TEAM:-Z9HUH3VGHM}"
BUNDLE_ID="${CABLY_SIGN_BUNDLE_ID:-org.cably.desktop}"
PRODUCT="${CABLY_SIGN_PRODUCT:-Cably Desktop}"
APP_STAPLE="${CABLY_SIGN_APP_STAPLE:-1}"
LAUNCH_SECS=12
REF_ENT="com.apple.security.automation.apple-events com.apple.security.cs.allow-dyld-environment-variables com.apple.security.cs.disable-executable-page-protection com.apple.security.cs.disable-library-validation"
fail=0; ok(){ echo "  ok   $1"; }; bad(){ echo "  FAIL $1"; fail=1; }; note(){ echo "  note $1"; }
pl(){ /usr/libexec/PlistBuddy -c "Print $2" "$1" 2>/dev/null || true; }
rel(){ echo "${1#"$ROOT"/}"; }
csinfo(){ codesign -dvv "$1" 2>&1 </dev/null; }        # signature summary (stderr)
# string tests in pure bash: `echo "$x" | grep -q` under pipefail fails spuriously when grep -q
# exits before echo has written (EPIPE) - that produced a phantom "no timestamp" on the control
has(){ [[ "$1" == *"$2"* ]]; }                                    # substring anywhere
hasline(){ [[ $'\n'"$1"$'\n' == *$'\n'"$2"$'\n'* ]]; }         # a whole line equals $2
hasprefix(){ [[ $'\n'"$1" == *$'\n'"$2"* ]]; }                   # a line starts with $2
RUNTIME_RE='flags=0x[0-9a-f]+\([^)]*runtime'
hasruntime(){ [[ "$1" =~ $RUNTIME_RE ]]; }
csverify(){ codesign --verify --strict --verbose=2 "$1" 2>&1 | grep -v -E '^--(prepared|validated):'; }
# the editor apps carry Contents/Frameworks -> ../../../Frameworks (upstream layout); --strict rejects a
# symlink that leaves a NESTED bundle even on KiCad's official app, whose outer --deep --strict passes,
# so nested editor bundles are verified without --strict (the outer deep strict check covers them)
csverify_nested(){ codesign --verify --verbose=2 "$1" 2>&1 | grep -v -E '^--(prepared|validated):'; }
entkeys(){ codesign -d --entitlements - --xml "$1" 2>/dev/null | grep -o '<key>[^<]*</key>' | sed 's/<[^>]*>//g' | sort | tr '\n' ' ' | sed 's/ $//'; }
is_macho(){ # regular file whose magic is Mach-O (thin, either endianness) or fat
  case "$(head -c 4 "$1" 2>/dev/null | xxd -p)" in cffaedfe|cefaedfe|feedface|feedfacf|cafebabe|bebafeca) return 0;; esac; return 1
}
# sig_check <path> <label>: Developer ID by $TEAM, hardened, timestamped, not ad-hoc
sig_check(){
  local p="$1" label="$2" info; info="$(csinfo "$p")"
  if has "$info" 'not signed'; then bad "$label: not signed at all"; return 1; fi
  local problems=""
  has "$info" 'Signature=adhoc' && problems="$problems ad-hoc"
  hasline "$info" "TeamIdentifier=$TEAM" || problems="$problems team=$(echo "$info" | sed -n 's/^TeamIdentifier=//p')"
  hasruntime "$info" || problems="$problems no-hardened-runtime"
  hasprefix "$info" 'Timestamp=' || problems="$problems no-secure-timestamp"
  hasprefix "$info" 'Authority=Developer ID Application:' || problems="$problems not-Developer-ID"
  if [ -z "$problems" ]; then ok "$label: Developer ID ($TEAM), hardened runtime, timestamped"; return 0
  else bad "$label:$problems"; return 1; fi
}

# 0. Which image / app ----------------------------------------------------------
TARGET="${1:-}"
if [ -z "$TARGET" ]; then TARGET="$(ls -t "$BUILD"/dmg/*.dmg 2>/dev/null | sed -n '1p')"; fi
[ -n "$TARGET" ] && [ -e "$TARGET" ] || { echo "signed.sh: no DMG (looked in $BUILD/dmg)"; exit 1; }
TARGET="$(cd "$(dirname "$TARGET")" && pwd)/$(basename "$TARGET")"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cably-signed.XXXXXX")"
MP=""; DMG=""
detach(){ [ -n "$MP" ] || return 0; for i in 1 2 3 4 5 6; do hdiutil detach "$MP" >/dev/null 2>&1 && return 0; sleep $i; done; hdiutil detach -force "$MP" >/dev/null 2>&1 || true; }
cleanup(){ detach; rm -rf "$WORK"; }
trap cleanup EXIT
echo "signed: team $TEAM, bundle id $BUNDLE_ID, product '$PRODUCT'"

case "$TARGET" in
*.app|*.app/)
  APP="${TARGET%/}"; ROOT="$(dirname "$APP")"
  echo "signed: checking bundle $APP (no DMG: (a), DMG notarization and launchers skipped)"
  ;;
*)
  DMG="$TARGET"
  echo "signed: checking $DMG ($(stat -f %z "$DMG") bytes)"
  # (a) the image's own signature ------------------------------------------------
  v="$(codesign --verify --verbose=2 "$DMG" 2>&1)"
  if has "$v" 'valid on disk' && has "$v" 'satisfies its Designated Requirement'; then ok "(a) DMG signature verifies (valid on disk, satisfies its Designated Requirement)"
  else bad "(a) DMG signature: $(echo "$v" | tr '\n' ' ' | cut -c1-200)"; fi
  # a disk-image signature has no hardened-runtime flag (KiCad's: flags=0x0(none)); team, chain, timestamp only
  info="$(csinfo "$DMG")"
  if hasline "$info" "TeamIdentifier=$TEAM" && hasprefix "$info" 'Authority=Developer ID Application:' \
     && hasprefix "$info" 'Timestamp=' && ! has "$info" 'Signature=adhoc'; then ok "(a) DMG signed by Developer ID Application, TeamIdentifier=$TEAM, timestamped"
  else bad "(a) DMG: $(echo "$info" | grep -E '^(Signature|TeamIdentifier|Authority|Timestamp)|not signed' | tr '\n' ' ' | cut -c1-220)"; fi
  # mount read-only at a private mountpoint
  MP="$WORK/mnt"; mkdir -p "$MP"
  if ! hdiutil attach -nobrowse -readonly -noautoopen -mountpoint "$MP" "$DMG" >/dev/null 2>&1; then
    bad "hdiutil attach failed for $DMG"; echo "signed.sh: FAIL"; exit 1
  fi
  ok "mounted read-only at $MP"
  ROOT="$MP"
  if [ -d "$MP/$PRODUCT.app" ] && [ ! -L "$MP/$PRODUCT.app" ]; then APP="$MP/$PRODUCT.app"
  else APP="$(find "$MP" -mindepth 1 -maxdepth 2 -type d -name '*.app' 2>/dev/null | sed -n '1p')"; fi
  ;;
esac
[ -n "${APP:-}" ] && [ -d "$APP/Contents/MacOS" ] || { bad "no app bundle found ($(ls "$ROOT" 2>/dev/null | tr '\n' ' '))"; echo "signed.sh: FAIL"; exit 1; }
echo "signed: app = $(rel "$APP")"
EXE="$APP/Contents/MacOS/$(pl "$APP/Contents/Info.plist" CFBundleExecutable)"

# (b) the app bundle --------------------------------------------------------------
v="$(codesign --verify --deep --strict --verbose=2 "$APP" 2>&1 | grep -v -E '^--(prepared|validated):')"
if has "$v" 'valid on disk' && has "$v" 'satisfies its Designated Requirement'; then ok "(b) codesign --verify --deep --strict passes"
else bad "(b) codesign --verify --deep --strict: $(echo "$v" | sed -n '1,3p' | tr '\n' ' ' | cut -c1-300)"; fi
info="$(csinfo "$APP")"
id="$(echo "$info" | sed -n 's/^Identifier=//p')"
[ "$id" = "$BUNDLE_ID" ] && ok "(b) Identifier=$BUNDLE_ID" || bad "(b) Identifier='$id' (want $BUNDLE_ID)"
for a in "Developer ID Application:" "Developer ID Certification Authority" "Apple Root CA"; do
  hasprefix "$info" "Authority=$a" && ok "(b) Authority=$a" || bad "(b) Authority chain lacks '$a' (got: $(echo "$info" | sed -n 's/^Authority=//p; s/^Signature=//p' | tr '\n' ';'))"
done
sig_check "$APP" "(b) $(basename "$APP")"
sig_check "$EXE" "(b) main executable $(rel "$EXE")"

# (c) every nested code object ------------------------------------------------------
FW="$APP/Contents/Frameworks/Python.framework"
XY=""; [ -L "$FW/Versions/Current" ] && XY="$(basename "$(readlink "$FW/Versions/Current")")"
# bundles first (their seals cover the files below)
for b in "$FW" "$FW/Versions/$XY" "$FW/Versions/$XY/Resources/Python.app" "$APP"/Contents/Applications/*.app; do
  [ -n "$XY" ] || case "$b" in "$FW/Versions/"*) continue;; esac
  [ -e "$b" ] || { bad "(c) missing bundle $(rel "$b")"; continue; }
  case "$b" in "$APP"/Contents/Applications/*) v="$(csverify_nested "$b")";; *) v="$(csverify "$b")";; esac
  if has "$v" 'valid on disk'; then sig_check "$b" "(c) bundle $(rel "$b")"
  else bad "(c) bundle $(rel "$b"): $(echo "$v" | sed -n '1,2p' | tr '\n' ' ' | cut -c1-200)"; fi
done
# then every Mach-O regular file
find "$APP" -type f -size +1k -print0 2>/dev/null | while IFS= read -r -d '' f; do is_macho "$f" && printf '%s\0' "$f"; done > "$WORK/machos"
n=$(tr -cd '\0' < "$WORK/machos" | wc -c | tr -d ' ')
: > "$WORK/unsigned"; : > "$WORK/adhoc"; : > "$WORK/team"; : > "$WORK/runtime"; : > "$WORK/ts"
nfw=0; nkiface=0; neditor=0; nmacos=0; ndylib=0
while IFS= read -r -d '' f; do
  case "$f" in "$FW"/*) nfw=$((nfw+1));; *.kiface) nkiface=$((nkiface+1));; "$APP"/Contents/Applications/*) neditor=$((neditor+1));; "$APP"/Contents/MacOS/*) nmacos=$((nmacos+1));; "$APP"/Contents/Frameworks/*.dylib) ndylib=$((ndylib+1));; esac
  info="$(csinfo "$f")"
  if has "$info" 'not signed'; then rel "$f" >> "$WORK/unsigned"; continue; fi
  has "$info" 'Signature=adhoc' && rel "$f" >> "$WORK/adhoc"
  hasline "$info" "TeamIdentifier=$TEAM" || echo "$(rel "$f") (TeamIdentifier=$(echo "$info" | sed -n 's/^TeamIdentifier=//p'))" >> "$WORK/team"
  hasruntime "$info" || rel "$f" >> "$WORK/runtime"
  hasprefix "$info" 'Timestamp=' || rel "$f" >> "$WORK/ts"
done < "$WORK/machos"
[ "$n" -gt 0 ] && ok "(c) walked $n Mach-O files: $nmacos in Contents/MacOS, $ndylib Frameworks/*.dylib, $nfw in Python.framework, $nkiface kifaces, $neditor editor executables" \
  || bad "(c) found no Mach-O files under $(rel "$APP")"
[ "$nfw" -ge 3 ] && [ -n "$XY" ] && [ -f "$FW/Versions/$XY/Python" ] && ls "$FW/Versions/$XY/lib/python$XY/lib-dynload/"*.so >/dev/null 2>&1 \
  && ok "(c) Python.framework has Versions/$XY/Python, lib-dynload/*.so and bin/ executables to check" \
  || bad "(c) Python.framework layout incomplete (Current -> '${XY:-?}', $nfw Mach-O files)"
[ "$nkiface" -ge 7 ] && ok "(c) $nkiface kifaces present" || bad "(c) only $nkiface .kiface files (want 7)"
[ "$neditor" -ge 6 ] && ok "(c) $neditor editor executables present" || bad "(c) only $neditor Mach-O files under Contents/Applications (want 6)"
report(){ # $1 file, $2 label
  local c; c=$(wc -l < "$1" | tr -d ' ')
  if [ "$c" = 0 ]; then ok "(c) no Mach-O $2"; else bad "(c) $c Mach-O files $2:"; sed -n '1,12p' "$1" | sed 's/^/         /'; [ "$c" -gt 12 ] && echo "         ... and $((c-12)) more"; fi
}
report "$WORK/unsigned" "unsigned"
report "$WORK/adhoc" "ad-hoc signed"
report "$WORK/team" "signed by a team other than $TEAM"
report "$WORK/runtime" "without the hardened runtime flag"
report "$WORK/ts" "without a secure timestamp"

# (d) entitlements ----------------------------------------------------------------------
ENT_TARGETS=("$EXE" "$APP/Contents/MacOS/kicad-cli")
for e in "$APP"/Contents/Applications/*.app; do ENT_TARGETS+=("$e"); done
[ -n "$XY" ] && ENT_TARGETS+=("$FW/Versions/$XY/Resources/Python.app" "$FW/Versions/$XY/bin/python$XY")
for t in "${ENT_TARGETS[@]}"; do
  [ -e "$t" ] || { bad "(d) missing $(rel "$t")"; continue; }
  got="$(entkeys "$t")"
  [ "$got" = "$REF_ENT" ] && ok "(d) entitlements of $(rel "$t") = reference set" \
    || bad "(d) entitlements of $(rel "$t") = [${got:-none}] (want [$REF_ENT])"
done

# (e) notarization ------------------------------------------------------------------------
if [ -n "$DMG" ]; then
  v="$(xcrun stapler validate "$DMG" 2>&1 | sed -n '$p')"
  has "$v" 'The validate action worked' && ok "(e) stapler validate DMG: ticket stapled" || bad "(e) stapler validate DMG: $v"
  v="$(spctl --assess --type open --context context:primary-signature --verbose=2 "$DMG" 2>&1 | tr '\n' ' ')"
  has "$v" ': accepted' && has "$v" 'source=Notarized Developer ID' && ok "(e) spctl open DMG: accepted, source=Notarized Developer ID" || bad "(e) spctl open DMG: $v"
fi
v="$(spctl --assess --type execute --verbose=2 "$APP" 2>&1 | tr '\n' ' ')"
has "$v" ': accepted' && has "$v" 'source=Notarized Developer ID' && ok "(e) spctl execute app: accepted, source=Notarized Developer ID" || bad "(e) spctl execute app: $v"
v="$(xcrun stapler validate "$APP" 2>&1 | sed -n '$p')"
if has "$v" 'The validate action worked'; then ok "(e) stapler validate app: ticket stapled"
elif [ "$APP_STAPLE" = 0 ]; then note "(e) app has no stapled ticket ($v) - tolerated by CABLY_SIGN_APP_STAPLE=0; Gatekeeper relies on the DMG's ticket or an online lookup"
else bad "(e) stapler validate app: $v"; fi

# (f) launchers + launch ------------------------------------------------------------------
if [ -n "$DMG" ]; then
  APPR="$(cd "$APP" && pwd -P)"; nl=0
  if [ -d "$ROOT/$PRODUCT" ]; then
    for l in "$ROOT/$PRODUCT"/*.app; do
      [ -L "$l" ] || continue; nl=$((nl+1))
      r="$(cd "$l" 2>/dev/null && pwd -P)"
      if [ -n "$r" ] && [ -d "$r/Contents" ] && [ "${r#"$APPR"/}" != "$r" ]; then
        v="$(csverify_nested "$r")"
        has "$v" 'valid on disk' && ok "(f) launcher '$(basename "$l")' -> ${r#"$APPR"/} inside the signed app, signature verifies" \
          || bad "(f) launcher '$(basename "$l")' -> ${r#"$APPR"/}: $(echo "$v" | sed -n '1p' | cut -c1-160)"
      else bad "(f) launcher '$(basename "$l")' -> '$(readlink "$l")' does not resolve into $(rel "$APP")"; fi
    done
  fi
  [ "$nl" -ge 6 ] && ok "(f) $nl launcher symlinks in '$PRODUCT/'" || bad "(f) only $nl launcher symlinks in '$PRODUCT/' (want 6)"
fi
BIN="$APP/Contents/MacOS/kicad"
if [ "${CABLY_SIGN_LAUNCH:-1}" != 1 ]; then echo "  skip (f) launch (CABLY_SIGN_LAUNCH=0)"
elif [ ! -x "$BIN" ]; then bad "(f) no executable at $(rel "$BIN")"
else  # launched directly (pid known), so an unrelated running kicad does not matter
  mkdir -p "$WORK/config" "$WORK/home"
  env KICAD_CONFIG_HOME="$WORK/config" HOME="$WORK/home" "$BIN" </dev/null >/dev/null 2>"$WORK/stderr" &
  PID=$!; disown "$PID" 2>/dev/null; sleep "$LAUNCH_SECS"
  if kill -0 "$PID" 2>/dev/null; then ALIVE=1; ok "(f) Contents/MacOS/kicad alive after ${LAUNCH_SECS}s"; else ALIVE=0; bad "(f) Contents/MacOS/kicad exited within ${LAUNCH_SECS}s (stderr: $(tail -3 "$WORK/stderr" | tr '\n' ' ' | cut -c1-300))"; fi
  for pat in "Python path configuration" "Fatal Python error" "Unhandled exception" "Library Validation" "code signature"; do
    if grep -qi "$pat" "$WORK/stderr"; then bad "(f) stderr contains \"$pat\": $(grep -i -m1 -A2 "$pat" "$WORK/stderr" | tr '\n' ' ' | cut -c1-300)"
    else ok "(f) stderr has no \"$pat\""; fi
  done
  if [ "$ALIVE" = 1 ]; then
    kill -TERM "$PID" 2>/dev/null; i=0
    while kill -0 "$PID" 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
    if kill -0 "$PID" 2>/dev/null; then kill -KILL "$PID" 2>/dev/null; bad "(f) did not exit on SIGTERM within 5 s (killed)"; else ok "(f) exits on SIGTERM"; fi
  fi
  sleep 1
fi

if [ -n "$MP" ]; then detach && ok "detached" || bad "could not detach $MP"; MP=""; fi
trap - EXIT; rm -rf "$WORK"
[ "$fail" = 0 ] && echo "signed.sh: PASS" || echo "signed.sh: FAIL"
exit $fail
