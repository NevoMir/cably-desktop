#!/usr/bin/env bash
# Build the Cably Desktop DMG template (cablytemplate.dmg.tar.bz2) without Finder.
#
# kicad-mac-builder's bin/package.sh does not create the installer image: it
# untars a TEMPLATE image, resizes it, mounts it, rsyncs the app into the
# product folder and converts it to a compressed read-only DMG. Everything the
# user SEES (volume name, folder, background, volume icon, icon layout) comes
# from the template — this script makes ours:
#
#   HFS+ (GUID layout, as package.sh expects "Apple_HFS"), 200 MB read/write
#   image (package.sh grows it with `hdiutil resize`), volume "Cably Desktop":
#     Applications -> /Applications      symlink
#     Cably Desktop/                     package.sh fills it: the six editor
#                                        launcher symlinks + demos/
#     (Cably Desktop.app                 the real bundle, placed by package.sh
#                                        at the top level; only its icon
#                                        position is in the .DS_Store)
#     .background.png                    rendered from cably/icons/src/dmg-background.svg
#     .VolumeIcon.icns                   the Cably mark (kicad/kicad.icns) + the custom-icon flag
#     .DS_Store                          window/icon layout + background alias (make-ds-store.py)
#
# Output: cably/toolchain/dmg/cablytemplate.dmg.tar.bz2 (or --out FILE).
# Needs: hdiutil, mount, SetFile (Xcode CLT), rsvg-convert (Homebrew librsvg), python3.
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
FORK="$(cd "$HERE/../../.." && pwd)"
OUT="$HERE/cablytemplate.dmg.tar.bz2"
[ "${1:-}" = "--out" ] && OUT="$2"
VOLNAME="Cably Desktop"
SIZE_MB=200
SVG="$FORK/cably/icons/src/dmg-background.svg"
ICNS="$FORK/kicad/kicad.icns"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cably-template.XXXXXX")"
DEV=""
cleanup(){
  if [ -n "$DEV" ]; then
    for i in 1 2 3 4 5; do hdiutil detach "$DEV" >/dev/null 2>&1 && break; sleep $i; done
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

for t in hdiutil SetFile rsvg-convert python3; do command -v "$t" >/dev/null || { echo "make-template: $t not found"; exit 1; }; done
[ -f "$SVG" ] || { echo "make-template: $SVG missing"; exit 1; }
[ -f "$ICNS" ] || { echo "make-template: $ICNS missing (run cably/icons/build-icns.sh)"; exit 1; }

rsvg-convert -o "$WORK/background.png" "$SVG"
echo "make-template: background $(sips -g pixelWidth -g pixelHeight "$WORK/background.png" | awk '/pixel/{printf "%s ", $2}')px from $(basename "$SVG")"

IMG="$WORK/cablytemplate.dmg"
hdiutil create -size "${SIZE_MB}m" -fs HFS+ -volname "$VOLNAME" -layout GPTSPUD "$IMG" >/dev/null
DEV="$(hdiutil attach "$IMG" -noautoopen -nobrowse -nomount | awk '/Apple_HFS/ {print $1}')"
[ -n "$DEV" ] || { echo "make-template: no Apple_HFS partition after attach"; exit 1; }
MNT="$WORK/mnt"; mkdir -p "$MNT"
mount -t hfs -o nobrowse "$DEV" "$MNT"
mdutil -i off "$MNT" >/dev/null 2>&1 || true

ln -s /Applications "$MNT/Applications"
mkdir "$MNT/$VOLNAME"
cp "$WORK/background.png" "$MNT/.background.png"
cp "$ICNS" "$MNT/.VolumeIcon.icns"
SetFile -a C "$MNT"                       # custom-icon flag on the volume root
SetFile -a V "$MNT/.VolumeIcon.icns" || true
python3 "$HERE/make-ds-store.py" write --root "$MNT" --volume-name "$VOLNAME" \
  --background .background.png --window 100,100,660,400 --icon-size 96 --text-size 12 \
  --item "$VOLNAME.app=165,170" --item "Applications=495,170" --item "$VOLNAME=330,300"
# .DS_Store.new / .fseventsd etc. are Finder/kernel leftovers; none here (never browsed)
ls -la "$MNT"
echo "make-template: volume '$(diskutil info "$MNT" | awk -F': *' '/Volume Name/{print $2}')', root flags $(GetFileInfo -a "$MNT")"
sync
umount "$MNT" 2>/dev/null || diskutil unmount "$MNT" >/dev/null
for i in 1 2 3 4 5; do hdiutil detach "$DEV" >/dev/null 2>&1 && { DEV=""; break; }; sleep $i; done
[ -z "$DEV" ] || { echo "make-template: could not detach $DEV"; exit 1; }

mkdir -p "$(dirname "$OUT")"
tar -cjf "$OUT" -C "$WORK" cablytemplate.dmg
echo "make-template: wrote $OUT ($(stat -f %z "$OUT") bytes; image $(stat -f %z "$IMG") bytes before compression)"
