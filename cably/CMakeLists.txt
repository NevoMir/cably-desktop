# This program source code file is part of Cably Desktop, based on KiCad,
# a free EDA CAD application.
#
# Copyright (C) 2026 Cably
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

# Cably additions.  This file is include()d ONCE from the top-level CMakeLists.txt after
# every KiCad target exists; each Cably feature appends its own clearly marked block and
# wires its sources with target_sources() so the KiCad CMake files stay untouched.

# ---- F3(d) home: Cably home screen in the manager window (agent "home") ----
target_sources( kicad PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src/cably_home_panel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/cably_home_panel.h
    ${CMAKE_CURRENT_LIST_DIR}/src/cably_home_recent.h
    )
target_include_directories( kicad PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src )
# ---- end F3(d) home ----

# ---- F2 identity (agent "identity") ----
# cably/src/cably_config.h holds the product name ("Cably Desktop", based on KiCad) and the
# GPLv3 s6 corresponding-source URL. Consumers: common/dialog_about (target common) and the
# KiCad Manager (target kicad). Per-editor About titles are plain literals, no include needed.
target_include_directories( common PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src )
target_include_directories( kicad PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src )
# ---- end F2 identity ----
