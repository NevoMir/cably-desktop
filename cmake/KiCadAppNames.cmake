#
#  This program source code file is part of KiCad, a free EDA CAD application,
#  as modified for Cably Desktop (based on KiCad); modifications are listed in
#  CHANGES.md as required by GPLv3 s5(a).
#
#  Copyright The KiCad Developers, see AUTHORS.txt for contributors.
#  Copyright (C) 2026 Cably <dev@cably.dev> (modifications only)
#
#  This program is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <https://www.gnu.org/licenses/>.
#

# Derive the Linux/Unix desktop application identifiers shared by the resource
# metadata generation and the runtime Wayland app_id (see PGM_BASE::InitPgm).
#
# We use two different app IDs here for legacy reasons. The org.kicad.KiCad ID is
# from FlatHub Flatpaks, while the org.kicad.kicad ID is used everywhere else.
# Having these two be separate causes problems in Gnome Software because they will
# each appear as their own app, so KiCad gets two entries. To work around this, we
# include a provides statement in the metainfo for the other app ID to link the two
# IDs together.

# Default values for regular builds
set( KICAD_REVERSE_DOMAIN "org.kicad" )
set( KICAD_APP_NAME "${KICAD_REVERSE_DOMAIN}.kicad" )
set( KICAD_PROVIDES_APP_ID "${KICAD_REVERSE_DOMAIN}.KiCad" )
set( KICAD_APP_PREFIX "${KICAD_REVERSE_DOMAIN}" )
set( KICAD_ICON_PREFIX "" )
set( KICAD_DESKTOP_FILE_ICON_PREFIX "" )
set( KICAD_DESKTOP_FILE_ICON_KICAD "kicad" )
set( KICAD_MIME_FILE_PREFIX "kicad" )
set( KICAD_MIME_ICON_PREFIX "" )

# Override default values from above if we are building a flatpak
if( KICAD_BUILD_FLATPAK )
    set( KICAD_APP_NAME "${KICAD_REVERSE_DOMAIN}.KiCad" )
    set( KICAD_PROVIDES_APP_ID "${KICAD_REVERSE_DOMAIN}.kicad" )
    if( KICAD_BUILD_NIGHTLY_FLATPAK )
        set( KICAD_APP_NAME "${KICAD_APP_NAME}.Nightly" )
        set( KICAD_PROVIDES_APP_ID "${KICAD_PROVIDES_APP_ID}.Nightly" )
    endif()
    set( KICAD_APP_PREFIX "${KICAD_APP_NAME}" )
    set( KICAD_ICON_PREFIX "${KICAD_APP_NAME}" )
    set( KICAD_DESKTOP_FILE_ICON_PREFIX "${KICAD_APP_PREFIX}." )
    set( KICAD_DESKTOP_FILE_ICON_KICAD "${KICAD_APP_NAME}" )
    set( KICAD_MIME_FILE_PREFIX "${KICAD_APP_PREFIX}" )
    set( KICAD_MIME_ICON_PREFIX "${KICAD_APP_PREFIX}." )
endif()

# Cably Desktop (based on KiCad): the product's own ids override the defaults above; every
# consumer of these variables includes this file, so the seam is this one line.
include( ${CMAKE_CURRENT_LIST_DIR}/../cably/linux/CablyLinuxNames.cmake )
