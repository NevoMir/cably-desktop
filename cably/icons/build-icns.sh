#!/usr/bin/env bash
# Cably Desktop icons: SVG sources -> PNG slices -> .icns (and the in-app logo
# bitmaps / Windows .ico that upstream keeps under resources/bitmaps_png).
#
#   cably/icons/build-icns.sh            # overwrite the icons the build references
#   cably/icons/build-icns.sh --out DIR  # write the same tree under DIR (tests)
#
# Renderer: rsvg-convert (librsvg, Homebrew) when present -- it is
# deterministic, so committed binaries can be checked against a fresh run;
# otherwise macOS' built-in `qlmanage -t` (WebKit; output is valid but may
# differ pixel-wise, so the reproducibility test will not pass with it).
# Packing: Apple's `iconutil` (macOS, no install).
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
FORK="$(cd "$HERE/../.." && pwd)"
SRC="$HERE/src"
OUT="$FORK"
[ "${1:-}" = "--out" ] && OUT="$(mkdir -p "$2" && cd "$2" && pwd)"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

if command -v rsvg-convert >/dev/null 2>&1; then
  RENDERER="rsvg-convert $(rsvg-convert --version | head -1 | awk '{print $NF}')"
  render() { rsvg-convert -w "$2" -h "$2" -o "$3" "$1"; }
else
  RENDERER="qlmanage (WebKit)"
  render() { local t; t="$(mktemp -d)"; qlmanage -t -s "$2" -o "$t" "$1" >/dev/null 2>&1
             mv "$t/$(basename "$1").png" "$3"; rmdir "$t"; }
fi

# dir/name pairs: every .icns a per-app CMakeLists.txt references (cvpcb's are
# no longer built but are kept so nothing in the tree carries the old logo).
ICNS="kicad/kicad kicad/kicad_doc
      pcbnew/pcbnew pcbnew/pcbnew_doc pcbnew/fpedit pcbnew/fpedit_doc
      eeschema/eeschema eeschema/eeschema_doc eeschema/libedit eeschema/libedit_doc
      gerbview/gerbview gerbview/gerbview_doc
      pagelayout_editor/pagelayout_editor pagelayout_editor/pagelayout_editor_doc
      pcb_calculator/pcb_calculator bitmap2component/bitmap2component
      cvpcb/cvpcb cvpcb/cvpcb_doc"

n=0
for rel in $ICNS; do
  name="$(basename "$rel")"; set="$WORK/$name.iconset"; mkdir -p "$set" "$OUT/$(dirname "$rel")"
  for px in 16 32 64 128 256 512 1024; do render "$SRC/$name.svg" "$px" "$set/r$px.png"; done
  mv "$set/r16.png"   "$set/icon_16x16.png";     cp "$set/r32.png"  "$set/icon_16x16@2x.png"
  mv "$set/r32.png"   "$set/icon_32x32.png";     mv "$set/r64.png"  "$set/icon_32x32@2x.png"
  mv "$set/r128.png"  "$set/icon_128x128.png";   cp "$set/r256.png" "$set/icon_128x128@2x.png"
  mv "$set/r256.png"  "$set/icon_256x256.png";   cp "$set/r512.png" "$set/icon_256x256@2x.png"
  mv "$set/r512.png"  "$set/icon_512x512.png";   mv "$set/r1024.png" "$set/icon_512x512@2x.png"
  iconutil -c icns -o "$OUT/$rel.icns" "$set"
  n=$((n+1))
done

# In-app logo bitmaps (About dialog, window icon, project tree). The build
# tars resources/bitmaps_png/png/*.png as-is; the trailing number in each
# file name is its pixel size. icon_kicad* (incl. the _16/_24/_32 small
# designs and the nightly variant) -> the mark; project_kicad* -> the
# project document. Dark-theme variants get the same self-contained image.
PNGDIR="resources/bitmaps_png/png"; mkdir -p "$OUT/$PNGDIR"
for f in "$FORK/$PNGDIR"/icon_kicad*.png "$FORK/$PNGDIR"/project_kicad*.png; do
  b="$(basename "$f")"; px="$(echo "${b%.png}" | grep -oE '[0-9]+$')"
  case "$b" in project_*) s=kicad_doc;; *) s=kicad;; esac
  render "$SRC/$s.svg" "$px" "$OUT/$PNGDIR/$b"; n=$((n+1))
done
# The SVG "sources" the MAINTAIN_PNGS pipeline would regenerate those from.
for theme in light dark; do
  d="resources/bitmaps_png/sources/$theme"; mkdir -p "$OUT/$d"
  for s in icon_kicad icon_kicad_16 icon_kicad_24 icon_kicad_32 icon_kicad_nightly icon_kicad_nightly_16 icon_kicad_nightly_24 icon_kicad_nightly_32; do
    cp "$SRC/kicad.svg" "$OUT/$d/$s.svg"; n=$((n+1)); done
  cp "$SRC/kicad_doc.svg" "$OUT/$d/project_kicad.svg"; n=$((n+1))
done
# Linux/Windows application icon + the Windows resource .ico (PNG-compressed
# entries, accepted by rc.exe/windres since Vista).
ICODIR="resources/bitmaps_png/icons"; mkdir -p "$OUT/$ICODIR" "$WORK/ico"
render "$SRC/kicad.svg" 128 "$OUT/$ICODIR/icon_kicad.png"
render "$SRC/kicad.svg" 64  "$OUT/$ICODIR/icon_kicad_64.png"
for px in 16 24 32 48 64 256; do render "$SRC/kicad.svg" "$px" "$WORK/ico/$px.png"; done
python3 - "$WORK/ico" "$OUT/$ICODIR/icon_kicad.ico" "$OUT/$ICODIR/icon_kicad_nightly.ico" <<'PY'
import struct, sys
d, outs = sys.argv[1], sys.argv[2:]
sizes = [16, 24, 32, 48, 64, 256]
blobs = [open(f"{d}/{s}.png", "rb").read() for s in sizes]
hdr = struct.pack("<HHH", 0, 1, len(sizes)); off = 6 + 16 * len(sizes); ents = b""
for s, b in zip(sizes, blobs):
    ents += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(b), off); off += len(b)
for o in outs:
    open(o, "wb").write(hdr + ents + b"".join(blobs))
PY
n=$((n+4))
echo "build-icns: $n files written under $OUT ($RENDERER + iconutil)"
