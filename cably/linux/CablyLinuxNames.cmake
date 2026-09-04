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
# Cably Desktop's Linux/Unix desktop identifiers.  Included at the END of
# cmake/KiCadAppNames.cmake (the one-line seam in that upstream file), so every consumer
# of those variables sees the product's ids without further edits: the Wayland app_id
# definition common/CMakeLists.txt puts on pgm_base.cpp, and the resources/CMakeLists.txt
# rules that name the .desktop launchers, the metainfo and the icons.  The text inside the
# launchers and the metainfo (Name=, <name>, <description>) is edited in place in
# resources/linux (see CHANGES.md): those lines hardly move upstream, so a merge conflict
# there is small and visible, whereas Cably copies of the files would drift silently.
#
#   launchers   share/applications/org.cably.desktop.desktop, org.cably.desktop.<app>.desktop
#   metainfo    share/metainfo/org.cably.desktop.metainfo.xml, <id>org.cably.desktop</id>
#   icons       share/icons/hicolor/<size>/apps/org.cably.desktop{,.<app>}.png (+ scalable),
#               installed by cably/CMakeLists.txt from cably/linux/icons (rendered from
#               cably/icons/src by cably/linux/make-icons.sh).  KICAD_ICON_PREFIX names
#               them too, and since resources/linux/icons has no file of that name KiCad's
#               own app icons (kicad.png, pcbnew.png ...) are not installed.
#   MIME        unchanged: the file formats are KiCad's (application/x-kicad-*, files
#               kicad-kicad.xml / kicad-gerbers.xml, icons application-x-kicad-*), and a
#               Cably Desktop installed next to a distro KiCad must not fight over them.
#
# KICAD_PROVIDES_APP_ID links KiCad's two historical ids in the metainfo; Cably Desktop has
# no legacy id, so it stays empty and the metainfo carries no <provides><id>.

set( KICAD_REVERSE_DOMAIN "org.cably" )
set( KICAD_APP_NAME "org.cably.desktop" )
set( KICAD_PROVIDES_APP_ID "" )
set( KICAD_APP_PREFIX "org.cably.desktop" )
set( KICAD_ICON_PREFIX "org.cably.desktop" )
set( KICAD_DESKTOP_FILE_ICON_PREFIX "org.cably.desktop." )
set( KICAD_DESKTOP_FILE_ICON_KICAD "org.cably.desktop" )
set( KICAD_MIME_FILE_PREFIX "kicad" )
set( KICAD_MIME_ICON_PREFIX "" )
