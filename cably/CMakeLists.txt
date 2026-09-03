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

# ---- F4 home-f4: home panel <-> bridge (agent "home-f4") ----
# cably/src/cably_home_cloud.h: pure helpers (project stem, Supabase timestamps, "updated"
# wording, generate URL) behind the cloud side of the home panel; unit test
# cably/tests/unit/test_cably_home_cloud.cpp is built standalone by cably/tests/home.sh.
# The panel reaches the bridge (cably/src/cably_bridge.h, agent "bridge", whose sources are
# wired by its own block) only through the adapter section at the top of cably_home_panel.cpp.
target_sources( kicad PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src/cably_home_cloud.h
    )
# ---- end F4 home-f4 ----

# ---- F4 bridge: cloud bridge (agent "bridge") ----
# cably/src/cably_bridge.{h,cpp} (pure C++17 core: loopback sign-in, Supabase + engine
# calls, project-folder writer), cably_bridge_keychain.cpp (macOS Security.framework
# secret store), cably_bridge_http_kicad.cpp (KICAD_CURL_EASY transport).  The core needs
# only the nlohmann_json and picosha2 header targets; the transport needs kicommon.
set( CABLY_BRIDGE_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/src/cably_bridge.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/cably_bridge_keychain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/cably_bridge_http_kicad.cpp
    )
target_sources( kicad PRIVATE ${CABLY_BRIDGE_SRCS} ${CMAKE_CURRENT_LIST_DIR}/src/cably_bridge.h )
target_link_libraries( kicad picosha2 )
if( APPLE )
    target_link_libraries( kicad "-framework Security" "-framework CoreFoundation" )
endif()

# Test/diagnostic CLI driven by cably/tests/bridge.sh against a mock cloud.  Not part of
# `all` (never packaged); build with `make cably-bridge-cli` in the KiCad build dir.
add_executable( cably-bridge-cli EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_LIST_DIR}/src/cably_bridge_cli.cpp
    ${CABLY_BRIDGE_SRCS}
    )
target_include_directories( cably-bridge-cli PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src )
target_link_libraries( cably-bridge-cli
    kicommon
    core
    nlohmann_json
    picosha2
    ${wxWidgets_LIBRARIES}
    )
if( APPLE )
    target_link_libraries( cably-bridge-cli "-framework Security" "-framework CoreFoundation" )
endif()
set_target_properties( cably-bridge-cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/cably )
# ---- end F4 bridge ----

# ---- F5 sync-home: sync UI in the home panel (agent "sync-home") ----
# cably/src/cably_sync_status.h: pure helpers (import-report summariser, "Synced to Cably ·
# 12:04 · …" status line, clock, sidecar/report paths); unit test
# cably/tests/unit/test_cably_sync_status.cpp is built standalone by cably/tests/home.sh with
# the nlohmann/json header from thirdparty/.  The panel reaches the sync-bridge
# (cably/src/cably_sync.h + cably_sync.cpp, agent "sync-bridge", wired by its own block:
# CABLY_PROJECT_WATCH, CablySyncSave, CABLY_EXPORT_SIDECAR) only through the sync-adapter
# section at the top of cably_home_panel.cpp.
target_sources( kicad PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src/cably_sync_status.h
    )
target_link_libraries( kicad nlohmann_json )
# ---- end F5 sync-home ----

# ---- F5 sync-bridge: KiCad save -> cloud project (agent "sync-bridge") ----
# cably/src/cably_sync.{h,cpp}: the save watcher (polling, wx-free: see the header for the
# measured reasons) and CablySyncSave(); CABLY_BRIDGE::ImportProject lives in the bridge.
# Unit test cably/tests/unit/test_cably_sync.cpp is built standalone by cably/tests/bridge.sh,
# which also drives the CLI's `watch` command against the mock cloud.
target_sources( kicad PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src/cably_sync.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/cably_sync.h
    )
target_sources( cably-bridge-cli PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/cably_sync.cpp )
# ---- end F5 sync-bridge ----
