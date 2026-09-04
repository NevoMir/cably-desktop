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
# Configure + build + install Cably Desktop on Linux.  Idempotent and incremental: the
# Ninja tree lives in $CABLY_BUILD_DIR (a named volume in Docker, so a second run only
# recompiles what changed); CMake is re-run every time (cheap, and it is what picks up a
# changed CMakeLists).  Prints the elapsed time of each step and the installed
# kicad-cli's version at the end.
#
#   CABLY_FORK       source tree (read-only is fine)         default /src
#   CABLY_BUILD_DIR  build tree                              default /build
#   CABLY_PREFIX     install prefix                          default /opt/cably-desktop
#   CABLY_JOBS       ninja -j                                default 6
#   CABLY_CMAKE_EXTRA  extra -D flags (space separated)      default empty
set -euo pipefail
SRC="${CABLY_FORK:-/src}"
BUILD="${CABLY_BUILD_DIR:-/build}"
PREFIX="${CABLY_PREFIX:-/opt/cably-desktop}"
JOBS="${CABLY_JOBS:-6}"
EXTRA="${CABLY_CMAKE_EXTRA:-}"

[ -f "$SRC/CMakeLists.txt" ] && [ -f "$SRC/cably/CMakeLists.txt" ] || { echo "build.sh: no Cably Desktop source at $SRC (set CABLY_FORK)"; exit 1; }
mkdir -p "$BUILD" "$PREFIX"

secs(){ date +%s; }
T_ALL=$(secs)
echo "build.sh: src=$SRC build=$BUILD prefix=$PREFIX jobs=$JOBS ($(nproc) cpus, $(uname -sm))"

# 1. configure ----------------------------------------------------------------
T0=$(secs)
# shellcheck disable=SC2086
cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DKICAD_USE_CMAKE_FINDPROTOBUF=ON \
    -DKICAD_SCRIPTING_WXPYTHON=OFF \
    -DKICAD_BUILD_QA_TESTS=OFF \
    -DKICAD_BUILD_I18N=OFF \
    -DCMAKE_INSTALL_RPATH='$ORIGIN/../lib;$ORIGIN/../../..;$ORIGIN/../../../lib' \
    $EXTRA
echo "build.sh: configure took $(( $(secs) - T0 )) s"

# 2. build (all + the test CLI, which is EXCLUDE_FROM_ALL) ----------------------
T0=$(secs)
ninja -C "$BUILD" -j"$JOBS"
ninja -C "$BUILD" -j"$JOBS" cably-bridge-cli
echo "build.sh: ninja took $(( $(secs) - T0 )) s"

# 3. install --------------------------------------------------------------------
# Into an EMPTY prefix: a file an earlier install put there (a renamed launcher, an icon
# no longer shipped) must not linger, since the tests and the .deb take the tree as it
# is.  Only a prefix this script populated before (marker file) is wiped.
T0=$(secs)
MARK="$PREFIX/.cably-desktop-install"
if [ -e "$MARK" ]; then rm -rf "${PREFIX:?}"/* "$MARK"; fi
ninja -C "$BUILD" install >"$BUILD/install.log" 2>&1 || { tail -30 "$BUILD/install.log"; exit 1; }
date -u +%FT%TZ >"$MARK"
echo "build.sh: install took $(( $(secs) - T0 )) s ($(grep -c '^-- Installing' "$BUILD/install.log") files into $PREFIX)"

echo "build.sh: total $(( $(secs) - T_ALL )) s"
echo "build.sh: kicad-cli version: $("$PREFIX/bin/kicad-cli" version 2>&1 | head -1)"
"$PREFIX/bin/kicad-cli" version --format about 2>/dev/null | head -5 || true
