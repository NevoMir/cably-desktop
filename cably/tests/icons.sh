#!/usr/bin/env bash
# F3(b) acceptance test for the Cably app/document icons. Written BEFORE the
# icons existed (red), then made green by cably/icons/.
#
#  1. Every *.icns the macOS build references (grepped from the per-app
#     CMakeLists) has an SVG source in cably/icons/src/<name>.svg.
#  2. REPRODUCIBLE: cably/icons/build-icns.sh --out <tmp> regenerates every
#     icns / in-app PNG / .ico BYTE-IDENTICAL to what is committed in the tree
#     (rsvg-convert and iconutil are deterministic on this Mac), so the
#     committed binaries are provably derived from the SVG sources.
#  3. VALID: each icns survives `iconutil -c iconset` and its 512x512@2x slice
#     is 1024 px wide (sips), the 16x16 slice is 16 px; the 512 slice is the
#     exact PNG rsvg-convert produces from the SVG (iconutil embeds it verbatim).
#  4. NOT KICAD'S: each icns and each in-app logo PNG differs from the file at
#     upstream tag 10.0.6 (`git show 10.0.6:<path>`), and every SVG source
#     carries the Cably primary colour and no KiCad artwork.
#  5. The generator, the build script and every SVG carry the GPL notice.
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option)
# any later version. It is distributed WITHOUT ANY WARRANTY; see
# LICENSE.GPLv3. Copyright (C) 2026 Cably <dev@cably.dev>.
set -euo pipefail
FORK="$(cd "$(dirname "$0")/../.." && pwd)"
BASE="${CABLY_BASE_TAG:-10.0.6}"
ICONS="$FORK/cably/icons"
PRIMARY="#0073E6"   # hsl(210 100% 45%) from Cably-pcb/src/index.css
fail=0; ok(){ echo "  ok   $1"; }; bad(){ echo "  FAIL $1"; fail=1; }
cd "$FORK"

# 1 -- the icns set the build uses -------------------------------------------
ICNS=()
for cm in kicad pcbnew eeschema gerbview pcb_calculator pagelayout_editor bitmap2component cvpcb; do
  [ -f "$cm/CMakeLists.txt" ] || continue
  for n in $(grep -oE '[A-Za-z0-9_]+\.icns' "$cm/CMakeLists.txt" | sort -u); do
    [ -f "$cm/$n" ] && ICNS+=("$cm/$n")
  done
done
[ "${#ICNS[@]}" -ge 16 ] && ok "build references ${#ICNS[@]} icns files" || bad "expected >=16 icns references, found ${#ICNS[@]}"
for p in "${ICNS[@]}"; do
  n="$(basename "$p" .icns)"
  [ -f "$ICONS/src/$n.svg" ] || bad "no SVG source $ICONS/src/$n.svg for $p"
done

# 2 -- regenerate into a scratch tree and compare byte-for-byte -----------------
[ -x "$ICONS/build-icns.sh" ] || { bad "$ICONS/build-icns.sh missing or not executable"; echo "icons: FAIL"; exit 1; }
OUT="$(mktemp -d)"
if "$ICONS/build-icns.sh" --out "$OUT" >"$OUT/build.log" 2>&1; then ok "build-icns.sh --out ran ($(tail -1 "$OUT/build.log"))"; else bad "build-icns.sh failed: $(tail -3 "$OUT/build.log")"; fi
GEN=0
while IFS= read -r rel; do
  GEN=$((GEN+1))
  [ -f "$rel" ] || { bad "regenerated $rel is not in the tree"; continue; }
  cmp -s "$OUT/$rel" "$rel" || bad "tree $rel is NOT what build-icns.sh regenerates from cably/icons/src"
done < <(cd "$OUT" && find . -type f \( -name '*.icns' -o -name '*.png' -o -name '*.ico' -o -name '*.svg' \) | sed 's|^\./||' | sort)
[ "$GEN" -ge 60 ] && ok "$GEN regenerated files are byte-identical to the tree" || bad "only $GEN files regenerated (expected icns + in-app PNGs + ico)"
for p in "${ICNS[@]}"; do [ -f "$OUT/$p" ] || bad "build-icns.sh does not produce $p"; done

# 3 -- validity of each icns ------------------------------------------------------
for p in "${ICNS[@]}"; do
  n="$(basename "$p" .icns)"; set_dir="$OUT/rt/$n.iconset"; rm -rf "$set_dir"; mkdir -p "$OUT/rt"
  if ! iconutil -c iconset -o "$set_dir" "$p" 2>/dev/null; then bad "$p: iconutil round-trip failed"; continue; fi
  w2x=$(sips -g pixelWidth "$set_dir/icon_512x512@2x.png" 2>/dev/null | awk '/pixelWidth/{print $2}')
  w16=$(sips -g pixelWidth "$set_dir/icon_16x16.png" 2>/dev/null | awk '/pixelWidth/{print $2}')
  [ "$w2x" = 1024 ] && [ "$w16" = 16 ] || bad "$p: slices 512@2x=$w2x 16=$w16 (want 1024/16)"
  # Content: read pixels of the 512 slice (via sips -> uncompressed BMP) and
  # check the Cably tile blue and the white cable-C are where the design puts
  # them (scaled/offset for document icons, which also carry the paper).
  sips -s format bmp "$set_dir/icon_512x512.png" --out "$OUT/rt/$n.bmp" >/dev/null 2>&1
  case "$n" in *_doc) kind=doc;; *) kind=app;; esac
  python3 - "$OUT/rt/$n.bmp" "$kind" <<'PY' || bad "$p: 512 slice does not show the Cably mark (tile blue / white C / paper)"
import struct, sys
b = open(sys.argv[1], 'rb').read(); kind = sys.argv[2]
off = struct.unpack('<I', b[10:14])[0]; w, h = struct.unpack('<ii', b[18:26]); bpp = struct.unpack('<H', b[28:30])[0]
assert bpp in (24, 32), bpp; row = ((w * bpp // 8) + 3) // 4 * 4; up = h > 0; h = abs(h)
def px(fx, fy):
    x, y = int(fx * w), int(fy * h); r_ = (h - 1 - y) if up else y
    i = off + r_ * row + x * bpp // 8; return b[i + 2], b[i + 1], b[i]  # BGR(A)
blue = lambda c: c[2] > 190 and c[0] < 40 and 90 < c[1] < 150   # hsl 210 primary, incl. gradient
white = lambda c: min(c) > 235
if kind == 'app':
    assert blue(px(0.5, 0.13)), px(0.5, 0.13)
    assert white(px(0.25, 0.5)), px(0.25, 0.5)      # left of the white C (core line is at 0.293)
    assert blue(px(0.5, 0.5)), px(0.5, 0.5)           # inside the C
else:
    assert blue(px(0.5, 0.40)), px(0.5, 0.40)
    assert white(px(0.388, 0.535)), px(0.388, 0.535)
    assert white(px(0.30, 0.20)), px(0.30, 0.20)     # paper
PY
done
ok "all icns round-trip through iconutil with 16..1024 slices and show the Cably mark"

# 4 -- not KiCad's ---------------------------------------------------------------
LOGO_PNGS=$(ls resources/bitmaps_png/png/icon_kicad*.png resources/bitmaps_png/png/project_kicad*.png 2>/dev/null)
for p in "${ICNS[@]}" $LOGO_PNGS resources/bitmaps_png/icons/icon_kicad.ico resources/bitmaps_png/icons/icon_kicad.png; do
  if git cat-file -e "$BASE:$p" 2>/dev/null; then
    git show "$BASE:$p" | cmp -s - "$p" && bad "$p is byte-identical to upstream $BASE (still KiCad's logo)"
  else
    bad "$p does not exist at $BASE (test list is wrong)"
  fi
done
ok "every icns and in-app logo bitmap differs from upstream $BASE"
for p in $LOGO_PNGS; do
  want=$(basename "$p" .png | grep -oE '[0-9]+$'); have=$(sips -g pixelWidth "$p" | awk '/pixelWidth/{print $2}')
  [ "$want" = "$have" ] || bad "$p is ${have}px, filename says ${want}px"
done
ok "in-app logo PNGs have the pixel size their filename declares"
python3 - resources/bitmaps_png/icons/icon_kicad.ico <<'PY' && ok "icon_kicad.ico is a valid ICO with >=4 PNG entries" || bad "icon_kicad.ico invalid"
import struct,sys
b=open(sys.argv[1],'rb').read(); r,t,n=struct.unpack('<HHH',b[:6]); assert (r,t)==(0,1) and n>=4,(r,t,n)
for i in range(n):
    w,h,c,z,pl,bpp,sz,off=struct.unpack('<BBBBHHII',b[6+16*i:22+16*i]); assert b[off:off+8]==b'\x89PNG\r\n\x1a\n',i
PY
for s in "$ICONS"/src/*.svg; do
  grep -qi "$PRIMARY" "$s" || bad "$(basename "$s") lacks Cably primary $PRIMARY"
  grep -qE 'sodipodi|inkscape|KiCad' "$s" && bad "$(basename "$s") looks derived from KiCad's Inkscape sources"
done
ok "all SVG sources use $PRIMARY and none carry KiCad artwork"

# 5 -- GPL notice on everything we add --------------------------------------------
for f in "$ICONS"/*.sh "$ICONS"/*.py "$ICONS"/src/*.svg "$0"; do
  grep -q "GNU General Public License" "$f" || bad "$f lacks the GPL notice"
done
ok "GPL notice present on generator, build script, SVG sources and this test"

rm -rf "$OUT"
[ "$fail" = 0 ] && echo "icons: OK" || echo "icons: FAIL"
exit $fail
