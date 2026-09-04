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
# Linux identity acceptance (the counterpart of cably/tests/identity.sh, which inspects
# the macOS .app bundle): the INSTALLED tree presents the product as "Cably Desktop"
# and names KiCad only as attribution.  Written BEFORE the Linux identity changes; it
# is RED on an install made from unmodified resources/linux (org.kicad.*.desktop,
# Name=KiCad, KiCad's icons).
#
# Measured on $CABLY_PREFIX (default /opt/cably-desktop), i.e. after cably/linux/build.sh:
#  1. share/applications: every launcher is org.cably.desktop*.desktop, no org.kicad.*;
#     Name= starts with "Cably Desktop"; no Name/GenericName/Comment carries the KiCad
#     mark as the product; desktop-file-validate (when installed) accepts each file;
#     Exec= names the KiCad binaries that exist in bin/; MimeType= entries are kept.
#  2. Icon= of every launcher names a Cably icon (org.cably.desktop*) that exists in
#     share/icons/hicolor at 16..128 px + scalable, and every one of those PNGs differs
#     byte-wise from KiCad's icon of the same role in resources/linux/icons.
#  3. share/metainfo: one org.cably.desktop.metainfo.xml with <id>org.cably.desktop,
#     <name>Cably Desktop, launchable org.cably.desktop.desktop, a description that
#     says "based on KiCad", the MIME types still provided; no org.kicad.*.metainfo.xml.
#  4. share/mime/packages keeps the application/x-kicad-* types (files unchanged in
#     name: kicad-kicad.xml, kicad-gerbers.xml).
#  5. `strings bin/kicad` carries "Cably Desktop" and "based on KiCad" (the About text
#     is linked in), and bin/kicad-cli runs from the installed tree (RPATH) and prints
#     a version.
# Every check prints "  ok   ..." or "  FAIL ..."; exit 1 on any FAIL.
#
# Usage: cably/tests/identity-linux.sh [prefix]
#   CABLY_PREFIX  the install prefix (default /opt/cably-desktop)
#   CABLY_FORK    the source tree, for KiCad's original icons (default: this script's tree)
set -uo pipefail
FORK="${CABLY_FORK:-$(cd "$(dirname "$0")/../.." && pwd)}"
PREFIX="${1:-${CABLY_PREFIX:-/opt/cably-desktop}}"
APPID="org.cably.desktop"
PRODUCT="Cably Desktop"
fail=0; ok(){ echo "  ok   $1"; }; bad(){ echo "  FAIL $1"; fail=1; }
section(){ echo; echo "== $1"; }
val(){ sed -n "s/^$2=//p" "$1" | head -1; }   # val <desktop file> <key>

[ -d "$PREFIX/share" ] && [ -d "$PREFIX/bin" ] || { echo "identity-linux.sh: no installed tree at $PREFIX (run cably/linux/build.sh)"; echo "identity-linux.sh: FAIL"; exit 1; }
echo "identity-linux.sh: prefix=$PREFIX fork=$FORK"
APPS="$PREFIX/share/applications"; ICONS="$PREFIX/share/icons/hicolor"; META="$PREFIX/share/metainfo"; MIME="$PREFIX/share/mime/packages"

# 1. launchers ---------------------------------------------------------------------
section "(1) share/applications"
shopt -s nullglob
CABLY_DESKTOPS=("$APPS"/$APPID*.desktop); KICAD_DESKTOPS=("$APPS"/org.kicad.*.desktop); ALL_DESKTOPS=("$APPS"/*.desktop)
[ "${#CABLY_DESKTOPS[@]}" -ge 6 ] && ok "${#CABLY_DESKTOPS[@]} launchers named $APPID*.desktop" || bad "expected >= 6 $APPID*.desktop launchers, found ${#CABLY_DESKTOPS[@]}: $(ls "$APPS" 2>/dev/null | tr '\n' ' ')"
[ "${#KICAD_DESKTOPS[@]}" = 0 ] && ok "no org.kicad.*.desktop launcher" || bad "org.kicad launchers still installed: $(basename -a "${KICAD_DESKTOPS[@]}" | tr '\n' ' ')"
[ "${#ALL_DESKTOPS[@]}" = "${#CABLY_DESKTOPS[@]}" ] && ok "every launcher is a Cably one" || bad "launchers not under the Cably id: $(basename -a "${ALL_DESKTOPS[@]}" | grep -v "^$APPID" | tr '\n' ' ')"
[ -f "$APPS/$APPID.desktop" ] && ok "manager launcher $APPID.desktop present" || bad "manager launcher $APPS/$APPID.desktop missing"
for f in "${CABLY_DESKTOPS[@]}"; do
  b=$(basename "$f")
  name=$(val "$f" Name)
  case "$name" in "$PRODUCT"|"$PRODUCT "*) ok "$b: Name=$name" ;; *) bad "$b: Name='$name' (want '$PRODUCT...')" ;; esac
  for k in Name GenericName Comment; do
    v=$(val "$f" "$k")
    case "$v" in *[Kk]i[Cc]ad*) bad "$b: $k names KiCad as the product: '$v'" ;; esac
  done
  exe=$(val "$f" Exec | awk '{print $1}')
  [ -n "$exe" ] && [ -x "$PREFIX/bin/$(basename "$exe")" ] && ok "$b: Exec=$exe exists in bin/" || bad "$b: Exec='$exe' not an installed binary"
  if command -v desktop-file-validate >/dev/null; then
    OUT=$(desktop-file-validate "$f" 2>&1 | grep -v "^$f: warning" || true)
    [ -z "$OUT" ] && ok "$b: desktop-file-validate clean" || bad "$b: desktop-file-validate: $OUT"
  fi
done
# The MIME associations the upstream launchers carry must survive the rename.
grep -q "^MimeType=application/x-kicad-project" "$APPS/$APPID.desktop" 2>/dev/null && ok "manager launcher keeps MimeType=application/x-kicad-project" || bad "manager launcher lost its MimeType"
grep -q "^MimeType=application/x-kicad-pcb" "$APPS/$APPID.pcbnew.desktop" 2>/dev/null && ok "pcbnew launcher keeps MimeType=application/x-kicad-pcb" || bad "pcbnew launcher missing or lost its MimeType"
grep -q "^MimeType=application/x-kicad-schematic" "$APPS/$APPID.eeschema.desktop" 2>/dev/null && ok "eeschema launcher keeps MimeType=application/x-kicad-schematic" || bad "eeschema launcher missing or lost its MimeType"
grep -q "^MimeType=.*application/x-gerber" "$APPS/$APPID.gerbview.desktop" 2>/dev/null && ok "gerbview launcher keeps MimeType=application/x-gerber" || bad "gerbview launcher missing or lost its MimeType"

# 2. icons ---------------------------------------------------------------------------
section "(2) icons"
# launcher basename suffix -> KiCad's icon of the same role (for the byte-wise comparison)
kicad_icon_for(){ case "$1" in "$APPID") echo kicad ;; *) echo "${1#$APPID.}" ;; esac; }
for f in "${CABLY_DESKTOPS[@]}"; do
  b=$(basename "$f" .desktop); icon=$(val "$f" Icon)
  case "$icon" in "$APPID"|"$APPID".*) ok "$b: Icon=$icon" ;; *) bad "$b: Icon='$icon' (want $APPID or $APPID.<app>)"; continue ;; esac
  kicon=$(kicad_icon_for "$b")
  for px in 16 24 32 48 64 128; do
    p="$ICONS/${px}x${px}/apps/$icon.png"
    if [ ! -f "$p" ]; then bad "$icon: no ${px}x${px} PNG at $p"; continue; fi
    file "$p" 2>/dev/null | grep -q "PNG image data, $px x $px" || bad "$icon: $p is not a $px x $px PNG ($(file -b "$p" | cut -c1-60))"
    k="$FORK/resources/linux/icons/hicolor/${px}x${px}/apps/$kicon.png"
    if [ -f "$k" ]; then
      cmp -s "$p" "$k" && bad "$icon ${px}px is byte-equal to KiCad's $kicon.png" || true
    fi
  done
  ok "$icon: 16..128 px PNGs present$( [ -f "$FORK/resources/linux/icons/hicolor/48x48/apps/$kicon.png" ] && echo ", none byte-equal to KiCad's $kicon.png")"
  [ -f "$ICONS/scalable/apps/$icon.svg" ] && ok "$icon: scalable SVG present" || bad "$icon: no scalable SVG at $ICONS/scalable/apps/$icon.svg"
done
KICAD_ICONS=("$ICONS"/*/apps/kicad.png "$ICONS"/*/apps/pcbnew.png "$ICONS"/*/apps/eeschema.png)
[ "${#KICAD_ICONS[@]}" = 0 ] && ok "KiCad's own app icons (kicad.png, pcbnew.png, eeschema.png) are not installed" || bad "KiCad's app icons still installed: ${KICAD_ICONS[*]}"
MIMEICONS=("$ICONS"/48x48/mimetypes/application-x-kicad-*.png)
[ "${#MIMEICONS[@]}" -ge 6 ] && ok "${#MIMEICONS[@]} MIME type icons at 48x48 (application-x-kicad-*)" || bad "MIME type icons missing at $ICONS/48x48/mimetypes (found ${#MIMEICONS[@]})"

# 3. metainfo ------------------------------------------------------------------------
section "(3) share/metainfo"
MI="$META/$APPID.metainfo.xml"
KMI=("$META"/org.kicad.*.metainfo.xml)
[ "${#KMI[@]}" = 0 ] && ok "no org.kicad.*.metainfo.xml" || bad "KiCad metainfo still installed: $(basename -a "${KMI[@]}" | tr '\n' ' ')"
if [ -f "$MI" ]; then
  ok "$(basename "$MI") present"
  python3 - "$MI" "$APPID" "$PRODUCT" <<'PY' | while IFS= read -r line; do case "$line" in ok*) ok "${line#ok }" ;; *) bad "${line#FAIL }" ;; esac; done
import sys, xml.etree.ElementTree as ET
path, appid, product = sys.argv[1:4]
try:
    root = ET.parse(path).getroot()
except Exception as e:
    print(f"FAIL metainfo does not parse: {e}"); sys.exit(0)
def t(tag):
    el = root.find(tag); return (el.text or "").strip() if el is not None else ""
print(("ok " if t("id") == appid else "FAIL ") + f"<id> = '{t('id')}' (want {appid})")
print(("ok " if t("name") == product else "FAIL ") + f"<name> = '{t('name')}' (want {product})")
la = root.find("launchable"); la = (la.text or "").strip() if la is not None else ""
print(("ok " if la == appid + ".desktop" else "FAIL ") + f"<launchable> = '{la}'")
print(("ok " if t("project_license") == "GPL-3.0-or-later" else "FAIL ") + f"<project_license> = '{t('project_license')}'")
desc = " ".join(" ".join(" ".join(p.itertext()).split()) for p in root.findall("description/p"))
print(("ok " if "based on KiCad" in desc else "FAIL ") + "description says 'based on KiCad' (attribution)")
print(("ok " if product in desc else "FAIL ") + f"description names {product}")
summ = t("summary")
print(("FAIL " if "KiCad" in summ else "ok ") + f"<summary> = '{summ}'")
mts = sorted(m.text.strip() for m in root.findall("provides/mediatype"))
need = ["application/x-kicad-pcb", "application/x-kicad-project", "application/x-kicad-schematic"]
print(("ok " if all(n in mts for n in need) else "FAIL ") + f"provides the KiCad MIME types ({len(mts)} mediatypes)")
bins = sorted(b.text.strip() for b in root.findall("provides/binary"))
print(("ok " if "kicad" in bins and "pcbnew" in bins else "FAIL ") + f"provides binaries {' '.join(bins)}")
dev = t("developer_name") or t("developer/name")
print(("ok " if "Cably" in dev else "FAIL ") + f"developer = '{dev}'")
rel = root.find("releases/release")
print(("ok " if rel is not None and rel.get("version", "").startswith("10.0.6") else "FAIL ") + f"release version = '{rel.get('version') if rel is not None else ''}'")
for s in root.findall("screenshots/screenshot/image"):
    if "kicad.org" in (s.text or ""):
        print("FAIL a screenshot is KiCad's own (kicad.org image) presented as this product's"); break
else:
    print("ok no KiCad screenshot presented as this product's")
PY
else bad "$MI missing"; fi

# 4. MIME --------------------------------------------------------------------------
section "(4) share/mime/packages"
for x in kicad-kicad.xml kicad-gerbers.xml; do
  [ -f "$MIME/$x" ] && ok "$x installed" || bad "$MIME/$x missing"
done
grep -q 'type="application/x-kicad-pcb"' "$MIME/kicad-kicad.xml" 2>/dev/null && ok "application/x-kicad-pcb still declared" || bad "application/x-kicad-pcb not declared in kicad-kicad.xml"
grep -q 'glob pattern="\*.kicad_pro"' "$MIME/kicad-kicad.xml" 2>/dev/null && ok "*.kicad_pro glob kept" || bad "*.kicad_pro glob missing"

# 5. binaries ------------------------------------------------------------------------
section "(5) bin/"
for bin in kicad _pcbnew.kiface _eeschema.kiface; do
  p="$PREFIX/bin/$bin"
  [ -f "$p" ] || { bad "missing $p"; continue; }
  S=$(strings "$p")
  grep -qF "$PRODUCT" <<<"$S" && ok "$bin: '$PRODUCT' linked in" || bad "$bin: no '$PRODUCT' in binary"
  grep -qF "based on KiCad" <<<"$S" && ok "$bin: 'based on KiCad' linked in" || bad "$bin: no 'based on KiCad' in binary"
done
V=$("$PREFIX/bin/kicad-cli" version 2>&1 | head -1)
case "$V" in 10.0.6*) ok "kicad-cli runs from the installed tree (no LD_LIBRARY_PATH): version $V" ;; *) bad "kicad-cli does not run from $PREFIX/bin (RPATH?): $V" ;; esac
if command -v readelf >/dev/null; then
  RP=$(readelf -d "$PREFIX/bin/kicad" 2>/dev/null | grep -E 'RUNPATH|RPATH' | grep -o '\[.*\]')
  case "$RP" in *'$ORIGIN/../lib'*) ok "bin/kicad RUNPATH $RP" ;; *) bad "bin/kicad has no \$ORIGIN/../lib RUNPATH ($RP)" ;; esac
fi

echo
[ "$fail" = 0 ] && echo "identity-linux.sh: PASS" || echo "identity-linux.sh: FAIL"
exit $fail
