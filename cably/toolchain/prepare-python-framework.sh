#!/usr/bin/env bash
# Prepare a relocatable Python.framework for Cably Desktop from python.org's
# official macOS installer package (no system install: pkgutil --expand-full).
# The result is used BOTH to build (headers, import library) and to bundle:
# kicad/CMakeLists.txt:280 copies ${PYTHON_FRAMEWORK} (the framework ROOT)
# into Contents/Frameworks/ with cp -RP, so every Mach-O inside must already
# reference @rpath/Versions/<X.Y>/... and nothing under /Library/Frameworks.
#
#   prepare-python-framework.sh <python-X.Y.Z-macos11.pkg> <dest dir> [arch]
#
# writes <dest dir>/Python.framework (arch-thinned, default arm64) and prints
# the cmake arguments to pass. Idempotent: an existing dest is replaced.
# The pkg is python.org's installer (URL + sha256 in cably/toolchain/README.md);
# its framework is self-contained (OpenSSL, sqlite, lzma, mpdecimal built in),
# unlike Homebrew's, which links five Homebrew dylibs.
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
PKG="$1"; DEST="$2"; ARCH="${3:-arm64}"
[ -f "$PKG" ] || { echo "no pkg at $PKG"; exit 1; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cably-pyfw.XXXXXX")"; trap 'rm -rf "$WORK"' EXIT
pkgutil --expand-full "$PKG" "$WORK/x"
PAYLOAD="$WORK/x/Python_Framework.pkg/Payload"          # == Python.framework root
XY="$(ls "$PAYLOAD/Versions" | grep -E '^[0-9]+\.[0-9]+$' | head -1)"
[ -n "$XY" ] || { echo "no Versions/X.Y in payload"; exit 1; }
mkdir -p "$DEST"; rm -rf "$DEST/Python.framework"
ditto --arch "$ARCH" "$PAYLOAD" "$DEST/Python.framework"  # thin: 113 MB universal2 -> ~60 MB
FW="$DEST/Python.framework"; V="$FW/Versions/$XY"
OLD="/Library/Frameworks/Python.framework/Versions/$XY"
rm -f "$V/bin/python$XY-intel64"                        # x86_64-only launcher, gone after thinning
rm -f "$V/bin/python3-intel64"                           # its symlink: dangling, and codesign seals it ->
                                                        # every later verify of the framework fails (2026-09-05)
rm -rf "$V/_CodeSignature" "$V/Resources/Python.app/Contents/_CodeSignature"
# Current must exist for KiCad's PYTHONHOME (RefixupMacOS.cmake:82-84 recreates it anyway)
ln -sfn "$XY" "$FW/Versions/Current"
n=0
while IFS= read -r -d '' f; do
  file -b "$f" | grep -q 'Mach-O' || continue
  args=()
  # install name of the dylibs themselves
  case "$f" in
    "$V/Python") args+=(-id "@rpath/Versions/$XY/Python") ;;
    "$V"/lib/*.dylib) args+=(-id "@rpath/Versions/$XY/lib/$(basename "$f")") ;;
  esac
  # every absolute reference into the framework -> @rpath
  while IFS= read -r ref; do
    [ -n "$ref" ] && args+=(-change "$ref" "@rpath/Versions/$XY/${ref#$OLD/}")
  done < <(otool -L "$f" | awk 'NR>1{print $1}' | grep "^$OLD/" || true)
  # executables need an rpath that reaches the framework root. @loader_path (same
  # meaning as @executable_path for a main executable) on purpose: KiCad's
  # cmake/InstallSteps/RefixupMacOS.cmake later add_rpath's the @executable_path
  # form to these two binaries and aborts the install on a duplicate LC_RPATH.
  case "$f" in
    "$V"/bin/*) args+=(-add_rpath "@loader_path/../../..") ;;
    "$V"/Resources/Python.app/Contents/MacOS/*) args+=(-add_rpath "@loader_path/../../../../../..") ;;
  esac
  [ "${#args[@]}" -gt 0 ] || continue
  install_name_tool "${args[@]}" "$f" 2>&1 | grep -v 'invalidate the code signature' || true
  codesign --force --sign - "$f" 2>/dev/null   # arm64 refuses to run an invalidated signature
  n=$((n+1))
done < <(find "$FW" -type f -print0)
echo "rewrote $n Mach-O files"
# proof: nothing absolute outside /usr and /System, and the interpreter runs from here
left="$(find "$FW" -type f -print0 | xargs -0 otool -L 2>/dev/null | grep '^\t' | awk '{print $1}' | grep -v '^/usr/lib\|^/System/\|^@' || true)"
[ -z "$left" ] || { echo "still absolute:"; echo "$left"; exit 1; }
"$V/bin/python3" -c "import sys, encodings, ssl, sqlite3, lzma, decimal, hashlib, zlib, bz2, ctypes; print(sys.version.split()[0], sys.prefix); print('ssl', ssl.OPENSSL_VERSION)"
PYTHONHOME="$V" "$V/bin/python3" -c "import encodings, ssl; print('PYTHONHOME ok')"
cat <<EOT
PYTHON_FRAMEWORK=$FW
cmake args:
  -DPYTHON_EXECUTABLE=$V/bin/python$XY
  -DPYTHON_LIBRARY=$V/Python
  -DPYTHON_INCLUDE_DIR=$V/include/python$XY
  -DPYTHON_FRAMEWORK=$FW
EOT
