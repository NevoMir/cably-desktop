#!/usr/bin/env bash
# F6 acceptance: the DMG presents the product name "Cably Desktop" everywhere the
# user looks; the upstream project's mark appears only as attribution. Written
# BEFORE the packaging changes; it must FAIL on the F6-lite DMG (volume "KiCad",
# folder "KiCad", kicad-unified-*.dmg).
#
# Given a DMG (default: the newest *.dmg under <build dir>/dmg) it mounts it
# read-only at a private mountpoint (never /Volumes, never browsed) and asserts:
#  1. the file name matches ^cably-desktop-.*\.dmg$;
#  2. the volume name is "Cably Desktop" (diskutil info);
#  3. the top level has a "Cably Desktop" folder, an "Applications" symlink to
#     /Applications, nothing whose name contains "KiCad" (hidden entries
#     included), and no visible entry other than {Applications, Cably Desktop,
#     demos};
#  4. "Cably Desktop/Cably Desktop.app" resolves to a bundle whose
#     CFBundleIdentifier is org.cably.desktop;
#  5. the hidden Finder background (.background.png) is OUR image: byte-equal to
#     a fresh rsvg-convert render of cably/icons/src/dmg-background.svg and NOT
#     byte-equal to either of KiCad's background.png files (when the toolchain
#     checkout is present);
#  6. .VolumeIcon.icns is the Cably mark (byte-equal to kicad/kicad.icns) and
#     the volume root carries the custom-icon Finder flag;
#  7. if a .DS_Store is present it names our items (Iloc for "Cably Desktop"
#     and "Applications") and an icvp background record — never a "KiCad" item;
#  8. the app launches FROM THE MOUNT (open -a on the mounted bundle; the live
#     process' executable must live under the mountpoint), is alive after
#     $LAUNCH_SECS s, and exits on SIGTERM;
# then detaches. Every check prints an "ok"/"FAIL" line; exit 1 on any FAIL.
#
# Usage: cably/tests/dmg.sh [path/to/file.dmg]
#   CABLY_BUILD_DIR   build dir (default: <fork>/../build.noindex)
#   CABLY_TOOLCHAIN   kicad-mac-builder checkout (default: <fork>/../kicad-mac-builder)
#   CABLY_DMG_LAUNCH  0 to skip the launch check (default 1)
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
TOOLCHAIN="${CABLY_TOOLCHAIN:-$FORK/../kicad-mac-builder}"
LAUNCH_SECS=12
PRODUCT="Cably Desktop"
fail=0; ok(){ echo "  ok   $1"; }; bad(){ echo "  FAIL $1"; fail=1; }
pl(){ /usr/libexec/PlistBuddy -c "Print $2" "$1" 2>/dev/null || true; }

# 0. Which DMG -------------------------------------------------------------------
if [ -n "${1:-}" ]; then DMG="$1"; else DMG="$(ls -t "$BUILD"/dmg/*.dmg 2>/dev/null | head -1)"; fi
[ -n "$DMG" ] && [ -f "$DMG" ] || { echo "dmg.sh: no DMG (looked in $BUILD/dmg)"; exit 1; }
DMG="$(cd "$(dirname "$DMG")" && pwd)/$(basename "$DMG")"
echo "dmg: checking $DMG ($(du -h "$DMG" | cut -f1 | tr -d ' '), $(stat -f %z "$DMG") bytes)"

# 1. File name -------------------------------------------------------------------
NAME="$(basename "$DMG")"
[[ "$NAME" =~ ^cably-desktop-.*\.dmg$ ]] && ok "file name matches ^cably-desktop-.*\\.dmg\$ ($NAME)" || bad "file name '$NAME' does not match ^cably-desktop-.*\\.dmg\$"

# 2. Mount read-only at a private mountpoint ------------------------------------
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cably-dmg.XXXXXX")"
MP="$WORK/mnt"; mkdir -p "$MP"
detach(){
  for i in 1 2 3 4 5 6; do
    hdiutil detach "$MP" >/dev/null 2>&1 && return 0
    sleep $i
  done
  hdiutil detach -force "$MP" >/dev/null 2>&1 || true
}
cleanup(){ detach; rm -rf "$WORK"; }
trap cleanup EXIT
if ! hdiutil attach -nobrowse -readonly -noautoopen -mountpoint "$MP" "$DMG" >/dev/null 2>&1; then
  bad "hdiutil attach failed for $DMG"; echo "dmg.sh: FAIL"; exit 1
fi
ok "mounted read-only at $MP"

VOL="$(diskutil info "$MP" 2>/dev/null | awk -F': *' '/Volume Name/{print $2}')"
[ "$VOL" = "$PRODUCT" ] && ok "volume name = '$PRODUCT'" || bad "volume name = '$VOL' (want '$PRODUCT')"

# 3. Top level -------------------------------------------------------------------
[ -d "$MP/$PRODUCT" ] && [ ! -L "$MP/$PRODUCT" ] && ok "top-level folder '$PRODUCT' present" || bad "top-level folder '$PRODUCT' missing"
[ -d "$MP/KiCad" ] && bad "top-level folder 'KiCad' still present" || true
if [ -L "$MP/Applications" ] && [ "$(readlink "$MP/Applications")" = "/Applications" ]; then ok "Applications -> /Applications symlink present"; else bad "Applications symlink missing or wrong target ('$(readlink "$MP/Applications" 2>/dev/null)')"; fi
kicad_named=0
while IFS= read -r e; do
  case "$e" in
    .fseventsd|.Trashes|.TemporaryItems|.DocumentRevisions-V100|.Spotlight-V100|.DS_Store|.DS_Store.new|.VolumeIcon.icns|.background|.background.png|.metadata_never_index) continue ;;
  esac
  case "$e" in *[Kk][Ii][Cc][Aa][Dd]*) bad "top-level entry names KiCad: '$e'"; kicad_named=1 ;; esac
  case "$e" in
    .*) ;;   # other hidden entries are tolerated but listed
    "Applications"|"$PRODUCT"|demos) ;;
    *) bad "unexpected visible top-level entry: '$e'" ;;
  esac
done < <(ls -A "$MP")
[ "$kicad_named" = 0 ] && ok "no top-level entry (hidden included) names KiCad: $(ls -A "$MP" | tr '\n' ' ')"

# 4. The app inside the folder ---------------------------------------------------
APP="$MP/$PRODUCT/$PRODUCT.app"
if [ -e "$APP" ] && [ -d "$APP/Contents" ]; then
  ok "'$PRODUCT/$PRODUCT.app' resolves to a bundle$( [ -L "$APP" ] && echo " (symlink -> $(readlink "$APP"))")"
  BID="$(pl "$APP/Contents/Info.plist" CFBundleIdentifier)"
  [ "$BID" = "org.cably.desktop" ] && ok "CFBundleIdentifier = org.cably.desktop" || bad "CFBundleIdentifier = '$BID' (want org.cably.desktop)"
  [ "$(pl "$APP/Contents/Info.plist" CFBundleDisplayName)" = "$PRODUCT" ] && ok "CFBundleDisplayName = '$PRODUCT'" || bad "CFBundleDisplayName = '$(pl "$APP/Contents/Info.plist" CFBundleDisplayName)'"
  [ -x "$APP/Contents/MacOS/kicad" ] && ok "executable Contents/MacOS/kicad present" || bad "Contents/MacOS/kicad missing"
else
  bad "'$PRODUCT/$PRODUCT.app' missing or not a bundle (folder holds: $(ls "$MP/$PRODUCT" 2>/dev/null | tr '\n' ' '))"
fi

# 5. Background is ours ----------------------------------------------------------
BG=""
for c in "$MP/.background.png" "$MP/.background/background.png" "$MP/background.png"; do [ -f "$c" ] && { BG="$c"; break; }; done
if [ -n "$BG" ]; then
  case "$(basename "$BG")" in .*) ok "hidden background present: $(basename "$BG") ($(sips -g pixelWidth -g pixelHeight "$BG" 2>/dev/null | awk '/pixel/{printf "%s ", $2}')px)" ;;
    *) if GetFileInfo -a "$BG" 2>/dev/null | grep -q V; then ok "background present and invisible-flagged: $(basename "$BG")"; else bad "background '$(basename "$BG")' is neither a dotfile nor invisible-flagged"; fi ;;
  esac
  for k in "$TOOLCHAIN/kicad-mac-builder/unified-packaging/background.png" "$TOOLCHAIN/kicad-mac-builder/nightly-packaging/background.png"; do
    [ -f "$k" ] || continue
    if cmp -s "$BG" "$k"; then bad "background is byte-equal to KiCad's $(echo "$k" | sed "s|$TOOLCHAIN/||")"; else ok "background differs from KiCad's $(echo "$k" | sed "s|$TOOLCHAIN/||")"; fi
  done
  SVG="$FORK/cably/icons/src/dmg-background.svg"
  if [ -f "$SVG" ] && command -v rsvg-convert >/dev/null 2>&1; then
    rsvg-convert -o "$WORK/bg.png" "$SVG"
    cmp -s "$BG" "$WORK/bg.png" && ok "background is byte-equal to a fresh render of cably/icons/src/dmg-background.svg" || bad "background is not the render of cably/icons/src/dmg-background.svg"
  else
    bad "cably/icons/src/dmg-background.svg missing (or no rsvg-convert): cannot prove the background is ours"
  fi
  # a second, visible/invisible-flagged KiCad background must not ride along
  for extra in "$MP/background.png"; do
    [ -f "$extra" ] && [ "$extra" != "$BG" ] && bad "a second background file rides along: $(basename "$extra")"
  done
else
  bad "no Finder background found (.background.png / .background/background.png / background.png)"
fi

# 6. Volume icon -----------------------------------------------------------------
if [ -f "$MP/.VolumeIcon.icns" ]; then
  cmp -s "$MP/.VolumeIcon.icns" "$FORK/kicad/kicad.icns" && ok ".VolumeIcon.icns is the Cably mark (== kicad/kicad.icns)" || bad ".VolumeIcon.icns differs from kicad/kicad.icns"
  GetFileInfo -a "$MP" 2>/dev/null | grep -q C && ok "volume root carries the custom-icon flag" || bad "volume root lacks the custom-icon flag (GetFileInfo -a: $(GetFileInfo -a "$MP" 2>/dev/null))"
else
  bad ".VolumeIcon.icns missing"
fi

# 7. Finder layout (.DS_Store), if present ---------------------------------------
if [ -f "$MP/.DS_Store" ]; then
  # record names are UTF-16BE, record codes ASCII: look for both encodings
  dsnames(){ python3 - "$MP/.DS_Store" "$1" <<'PY'
import sys; d=open(sys.argv[1],'rb').read(); n=sys.argv[2]
sys.exit(0 if (n.encode('utf-16-be') in d or n.encode() in d) else 1)
PY
  }
  dsnames "$PRODUCT" && ok ".DS_Store names '$PRODUCT'" || bad ".DS_Store does not name '$PRODUCT'"
  dsnames "Applications" && ok ".DS_Store names 'Applications'" || bad ".DS_Store does not name 'Applications'"
  dsnames "icvp" && dsnames "backgroundImageAlias" && ok ".DS_Store carries icon-view prefs with a background alias (icvp)" || bad ".DS_Store has no icvp/backgroundImageAlias record"
  dsnames ".background.png" && ok ".DS_Store background alias points at .background.png" || bad ".DS_Store background alias does not name .background.png"
  dsnames "KiCad" && bad ".DS_Store names 'KiCad'" || ok ".DS_Store does not name KiCad"
else
  echo "  note .DS_Store absent (Finder will use its default layout; the background will not show)"
fi

# 8. Launch from the mount -------------------------------------------------------
if [ "${CABLY_DMG_LAUNCH:-1}" != 0 ] && [ -d "$APP/Contents" ]; then
  if pgrep -f "MacOS/kicad$" >/dev/null; then
    bad "launch: another kicad manager is already running ($(pgrep -fl 'MacOS/kicad$' | head -1)); close it and rerun"
  else
    open -a "$APP" 2>"$WORK/open.err" || bad "open -a failed: $(cat "$WORK/open.err")"
    # the process reports its real path (/private/var/... for a /var/... mountpoint)
    MPR="$(cd "$MP" && pwd -P)"
    PID=""
    for i in $(seq 1 20); do
      PID="$(pgrep -f "^($MP|$MPR)/.*/Contents/MacOS/kicad$" | head -1)"; [ -n "$PID" ] && break; sleep 0.5
    done
    if [ -n "$PID" ]; then
      ok "launched from the mount (pid $PID: $(ps -o command= -p "$PID" | sed "s|$MPR|<mount>|;s|$MP|<mount>|"))"
      sleep "$LAUNCH_SECS"
      if kill -0 "$PID" 2>/dev/null; then ok "process alive ${LAUNCH_SECS}s after launch"; else bad "process died within ${LAUNCH_SECS}s of launch"; fi
      kill -TERM "$PID" 2>/dev/null || true
      for i in $(seq 1 20); do kill -0 "$PID" 2>/dev/null || break; sleep 0.5; done
      if kill -0 "$PID" 2>/dev/null; then bad "did not exit within 10 s of SIGTERM"; kill -9 "$PID" 2>/dev/null || true; else ok "exited on SIGTERM"; fi
    else
      other="$(pgrep -fl 'MacOS/kicad$' | head -1)"
      bad "no kicad process running from the mount within 10 s${other:+ (LaunchServices started: $other)}"
      pkill -f 'MacOS/kicad$' 2>/dev/null || true
    fi
    sleep 1
  fi
fi

detach && ok "detached" || bad "could not detach $MP"
trap - EXIT; rm -rf "$WORK"
[ "$fail" = 0 ] && echo "dmg.sh: PASS" || echo "dmg.sh: FAIL"
exit $fail
