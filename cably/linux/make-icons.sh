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
# Cably Desktop's Linux icon theme tree: cably/icons/src/*.svg -> cably/linux/icons/hicolor
# (the freedesktop layout resources/CMakeLists.txt installs from resources/linux/icons for
# KiCad; cably/CMakeLists.txt installs this tree next to it).  The PNGs are committed so
# the build needs no renderer; rerun after editing an SVG.  Renderer: rsvg-convert
# (librsvg; `brew install librsvg` on macOS), deterministic, like cably/icons/build-icns.sh.
#
#   cably/linux/make-icons.sh            # rewrite cably/linux/icons/hicolor
#   cably/linux/make-icons.sh --out DIR  # write the same tree under DIR (tests)
#
# Names follow the app-id scheme KiCad uses for its flatpak (cmake/KiCadAppNames.cmake,
# KICAD_DESKTOP_FILE_ICON_KICAD / _PREFIX): the manager is org.cably.desktop, the editors
# org.cably.desktop.<launcher>; MIME type icons keep the names the shared MIME types
# reference (application-x-kicad-*), so a file manager shows Cably's document icons for
# .kicad_pcb & co.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../icons/src"
OUT="$HERE/icons/hicolor"
[ "${1:-}" = "--out" ] && OUT="$(mkdir -p "$2" && cd "$2" && pwd)/hicolor"
command -v rsvg-convert >/dev/null || { echo "make-icons.sh: rsvg-convert not found (brew install librsvg / apt install librsvg2-bin)"; exit 1; }

APPID="org.cably.desktop"
# icon name -> SVG source
APPS="$APPID:kicad $APPID.pcbnew:pcbnew $APPID.eeschema:eeschema $APPID.gerbview:gerbview
      $APPID.pcbcalculator:pcb_calculator $APPID.bitmap2component:bitmap2component"
MIMES="application-x-kicad-project:kicad_doc application-x-kicad-schematic:eeschema_doc
       application-x-kicad-pcb:pcbnew_doc application-x-kicad-footprint:fpedit_doc
       application-x-kicad-symbol:libedit_doc application-x-kicad-worksheet:pagelayout_editor_doc"
SIZES="16 24 32 48 64 128 256"

rm -rf "$OUT"; n=0
for pair in $APPS; do
  name="${pair%%:*}"; svg="$SRC/${pair##*:}.svg"; [ -f "$svg" ] || { echo "missing $svg"; exit 1; }
  for px in $SIZES; do
    mkdir -p "$OUT/${px}x${px}/apps"
    rsvg-convert -w "$px" -h "$px" -o "$OUT/${px}x${px}/apps/$name.png" "$svg"; n=$((n+1))
  done
  mkdir -p "$OUT/scalable/apps"; cp "$svg" "$OUT/scalable/apps/$name.svg"; n=$((n+1))
done
for pair in $MIMES; do
  name="${pair%%:*}"; svg="$SRC/${pair##*:}.svg"; [ -f "$svg" ] || { echo "missing $svg"; exit 1; }
  for px in $SIZES; do
    mkdir -p "$OUT/${px}x${px}/mimetypes"
    rsvg-convert -w "$px" -h "$px" -o "$OUT/${px}x${px}/mimetypes/$name.png" "$svg"; n=$((n+1))
  done
  mkdir -p "$OUT/scalable/mimetypes"; cp "$svg" "$OUT/scalable/mimetypes/$name.svg"; n=$((n+1))
done
echo "make-icons.sh: $n files under $OUT ($(rsvg-convert --version 2>/dev/null))"
