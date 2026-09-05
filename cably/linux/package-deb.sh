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
# Package the built Cably Desktop as a Debian binary package for Ubuntu 24.04:
#
#   cably-desktop_<upstream>+cably.<yyyymmdd>.<N>.g<rev>[.dirty]_<arch>.deb
#
# Version scheme.  <upstream> is the KiCad tag the build descends from (10.0.6),
# <yyyymmdd> the UTC build day, <N> the number of commits since that tag, <rev> the short
# commit hash and .dirty present when the tree had uncommitted changes.  N, rev and
# dirty are read from the BUILD TREE's record, $CABLY_BUILD_DIR/kicad_build_version.h
# (KICAD_VERSION, KiCad's `git describe --dirty` at the last build - the very string the
# binaries print as `kicad-cli version`), never from the source checkout: a checkout can
# move on after the build, and the package name must say what the binaries are.  Only a
# build tree without that record (a tarball build) falls back to `git describe --dirty`
# in $CABLY_FORK, with a warning.  N counts from the KiCad tag whatever tag describe
# found: once a release tag v10.0.6-cably.<M> is the nearest one, the record reads
# v10.0.6-cably.<M>-<n>-g<rev> and N = M + n (exactly on the tag: v10.0.6-cably.<M>,
# N = M, rev = the record's KICAD_COMMIT_HASH) - cably/linux/deb-version.sh, sourced
# here, lists the four forms; cably/tests/deb-version.sh pins them.
#   Why a commit count and not a time of day: dpkg compares runs of digits numerically and
# everything else as text, so with the bare hex rev after the day a later build of the
# same day sorted as a DOWNGRADE whenever its hash happened to be smaller
# (`dpkg --compare-versions 10.0.6+cably.20260904.1abc0000 lt 10.0.6+cably.20260904.77d2f34d53`
# was true).  N grows with every commit, so the builds of one day order by their source;
# across days <yyyymmdd> orders first; and a rebuild of the same commit gets the same
# version - the version names the source, not the clock (a UTC HHMMSS would also let a
# later build of an OLDER commit win).  .dirty sorts after the clean build of that
# commit; a dirty build is never a release.  cably/tests/deb.sh asserts all of this.
#
# The tree is a fresh, STRIPPED install of the Ninja build (`cmake --install --strip`
# under a DESTDIR) at /opt/cably-desktop - the prefix cably/linux/build.sh configures, so
# the binaries' $ORIGIN RPATH and KiCad's exe-relative data lookup work unchanged - plus
# what a desktop needs to see an /opt application: launchers copied to
# /usr/share/applications with an absolute Exec=, the app icons and the metainfo linked
# into /usr/share, the MIME definitions linked under a cably-desktop-* name (the types are
# KiCad's; a distro KiCad package must not be told it conflicts), /usr/bin/cably-desktop
# and cably-desktop-cli, and /usr/share/doc/cably-desktop/{copyright,NOTICE.md,CHANGES.md.gz}
# (GPLv3 s5/s6: the notice, the change record and the corresponding-source URL).
#
# Depends are derived from the binaries themselves with dpkg-shlibdeps (the way Debian's
# ${shlibs:Depends} is computed for KiCad's own package: every NEEDED library resolved to
# the Ubuntu 24.04 package that ships it), plus libsecret-1-0 (the Linux secret store) and
# python3 (KiCad's scripting).  No maintainer scripts: desktop-file-utils, hicolor-icon-theme
# and shared-mime-info (Recommends) refresh their caches through dpkg triggers.
#
# Environment (the CI contract, .github/workflows/linux.yml):
#   CABLY_FORK       source tree (git rev, NOTICE.md, CHANGES.md)   default /src
#   CABLY_BUILD_DIR  the configured + built Ninja tree               default /build
#   CABLY_PREFIX     where it is installed for the tests (unused: the package always
#                    lands in /opt/cably-desktop)                    default /opt/cably-desktop
#   CABLY_DEB_DIR    where the .deb is written                       default: cwd
#   CABLY_DEB_VERSION  override the version string (tests)
# Prints the .deb path on the last line; exit 1 on any error.
set -euo pipefail
SRC="${CABLY_FORK:-/src}"
BUILD="${CABLY_BUILD_DIR:-/build}"
OUTDIR="${CABLY_DEB_DIR:-$PWD}"
PKG=cably-desktop
DEB_PREFIX=/opt/cably-desktop
MAINT="Cably <dev@cably.dev>"
HOMEPAGE="https://cably.dev"
SOURCE_URL="https://github.com/NevoMir/cably-desktop"

for t in cmake dpkg dpkg-deb dpkg-shlibdeps file gzip; do command -v "$t" >/dev/null || { echo "package-deb.sh: $t not found (apt install dpkg-dev file cmake)"; exit 1; }; done
[ -f "$BUILD/CMakeCache.txt" ] || { echo "package-deb.sh: no configured build at $BUILD (CMakeCache.txt missing): run cably/linux/build.sh first"; exit 1; }
[ -f "$SRC/NOTICE.md" ] && [ -f "$SRC/CHANGES.md" ] || { echo "package-deb.sh: $SRC is not the Cably Desktop tree (NOTICE.md/CHANGES.md missing)"; exit 1; }
mkdir -p "$OUTDIR"

ARCH=$(dpkg --print-architecture)
# What the build tree compiled: KICAD_VERSION in kicad_build_version.h (kicad_build_version.txt
# is the same string) - KiCad's `git describe --dirty` - plus KICAD_COMMIT_HASH from the
# same header for a record that names a tag exactly (no -g<rev> in it).  The parser,
# cably/linux/deb-version.sh, knows the KiCad-tag and the release-tag forms; the source
# checkout is only a fallback.
# shellcheck source=deb-version.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/deb-version.sh"
RECORD=""; RECORD_FROM=""; RECORD_HASH=""
if [ -f "$BUILD/kicad_build_version.h" ]; then
  RECORD=$(sed -n 's/^#define KICAD_VERSION  *"\([^"]*\)".*/\1/p' "$BUILD/kicad_build_version.h" | head -1); RECORD_FROM="$BUILD/kicad_build_version.h"
  RECORD_HASH=$(sed -n 's/^#define KICAD_COMMIT_HASH  *"\([^"]*\)".*/\1/p' "$BUILD/kicad_build_version.h" | head -1)
fi
if [ -z "$RECORD" ] && [ -f "$BUILD/kicad_build_version.txt" ]; then
  RECORD=$(tr -d '\n\r' <"$BUILD/kicad_build_version.txt"); RECORD_FROM="$BUILD/kicad_build_version.txt"
fi
if cably_deb_parse_record "$RECORD" "$SRC" "$RECORD_HASH"; then
  echo "package-deb.sh: build tree records $RECORD ($RECORD_FROM): form $DV_FORM, $DV_COUNT commits since KiCad $DV_UPSTREAM, rev ${DV_REV:-none} from $DV_REV_FROM${DV_DIRTY:+, dirty}"
else
  echo "package-deb.sh: WARNING: no git-describe record in the build tree at $BUILD (found '${RECORD:-nothing}'; want 10.0.6-<N>-g<rev>, v10.0.6-cably.<M>[-<N>-g<rev>] or 10.0.6, each [-dirty]): falling back to git describe in $SRC - the package name is then the checkout's, not necessarily the binaries'"
  RECORD=$(git -C "$SRC" describe --dirty 2>/dev/null || true)
  cably_deb_parse_record "$RECORD" "$SRC" "" || { echo "package-deb.sh: cannot derive the version from '$RECORD' either"; exit 1; }
  echo "package-deb.sh: git describe in $SRC says $RECORD: form $DV_FORM, $DV_COUNT commits since KiCad $DV_UPSTREAM, rev ${DV_REV:-none} from $DV_REV_FROM${DV_DIRTY:+, dirty}"
fi
[ -n "$DV_REV" ] || { echo "package-deb.sh: '$RECORD' names a tag but no commit, and nothing resolves it ($DV_REV_FROM)"; exit 1; }
UPSTREAM=$DV_UPSTREAM; NCOMMITS=$DV_COUNT; REV=$DV_REV; DIRTY=${DV_DIRTY:+.dirty}
SEMVER=$(sed -n 's/^set( *KICAD_SEMANTIC_VERSION *"\([^"]*\)" *)/\1/p' "$SRC/cmake/KiCadVersion.cmake" | head -1)
[ -z "$SEMVER" ] || [ "$SEMVER" = "$UPSTREAM" ] || echo "package-deb.sh: WARNING: the build descends from tag $UPSTREAM but $SRC/cmake/KiCadVersion.cmake says $SEMVER: the checkout is not what was built"
VERSION="${CABLY_DEB_VERSION:-$UPSTREAM+cably.$(date -u +%Y%m%d).$NCOMMITS.g$REV$DIRTY}"
[ -z "$DIRTY" ] || echo "package-deb.sh: WARNING: the binaries were built from a tree with uncommitted changes: version marked .dirty (not a release)"
DEB="$OUTDIR/${PKG}_${VERSION}_${ARCH}.deb"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/$PKG"
mkdir -p "$STAGE"
secs(){ date +%s; }; T_ALL=$(secs)
echo "package-deb.sh: $PKG $VERSION $ARCH from $BUILD (fork $SRC)"

# 1. the stripped install under DESTDIR ------------------------------------------
T0=$(secs)
DESTDIR="$STAGE" cmake --install "$BUILD" --prefix "$DEB_PREFIX" --strip >"$WORK/install.log" 2>&1 \
    || { tail -20 "$WORK/install.log"; echo "package-deb.sh: cmake --install failed"; exit 1; }
OPT="$STAGE$DEB_PREFIX"
[ -x "$OPT/bin/kicad" ] && [ -x "$OPT/bin/kicad-cli" ] || { echo "package-deb.sh: $OPT/bin/kicad{,-cli} missing after install"; exit 1; }
echo "package-deb.sh: staged $(grep -c '^-- Installing' "$WORK/install.log") files, stripped, in $(( $(secs) - T0 )) s ($(du -sh "$OPT" | cut -f1))"

# 2. /usr integration --------------------------------------------------------------
APPS="$STAGE/usr/share/applications"; mkdir -p "$APPS"
n=0
for f in "$OPT"/share/applications/org.cably.desktop*.desktop; do
  [ -f "$f" ] || continue
  # The launcher in /opt is what CMake wrote (Exec=kicad %f, PATH-relative); the copy the
  # desktop actually reads points at the installed binary.
  sed -e "s|^Exec=|Exec=$DEB_PREFIX/bin/|" \
      -e "s|^Exec=\($DEB_PREFIX/bin/[^ ]*\)\(.*\)|Exec=\1\2\nTryExec=\1|" "$f" >"$APPS/$(basename "$f")"
  n=$((n+1))
done
[ "$n" -ge 6 ] || { echo "package-deb.sh: expected >= 6 org.cably.desktop*.desktop launchers in $OPT/share/applications, found $n"; exit 1; }

for d in "$OPT"/share/icons/hicolor/*/apps; do
  rel=${d#"$OPT"/share/icons/}
  mkdir -p "$STAGE/usr/share/icons/$rel"
  for i in "$d"/org.cably.desktop*; do [ -f "$i" ] && ln -s "$DEB_PREFIX/share/icons/$rel/$(basename "$i")" "$STAGE/usr/share/icons/$rel/$(basename "$i")"; done
done
mkdir -p "$STAGE/usr/share/metainfo" "$STAGE/usr/share/mime/packages" "$STAGE/usr/bin" "$STAGE/usr/share/doc/$PKG"
ln -s "$DEB_PREFIX/share/metainfo/org.cably.desktop.metainfo.xml" "$STAGE/usr/share/metainfo/org.cably.desktop.metainfo.xml"
ln -s "$DEB_PREFIX/share/mime/packages/kicad-kicad.xml" "$STAGE/usr/share/mime/packages/$PKG-kicad.xml"
ln -s "$DEB_PREFIX/share/mime/packages/kicad-gerbers.xml" "$STAGE/usr/share/mime/packages/$PKG-gerbers.xml"
ln -s "$DEB_PREFIX/bin/kicad" "$STAGE/usr/bin/$PKG"
ln -s "$DEB_PREFIX/bin/kicad-cli" "$STAGE/usr/bin/$PKG-cli"
for l in "$STAGE/usr/share/metainfo/org.cably.desktop.metainfo.xml" "$STAGE/usr/share/mime/packages/$PKG-kicad.xml" "$STAGE/usr/share/mime/packages/$PKG-gerbers.xml"; do
  [ -e "$STAGE$(readlink "$l")" ] || { echo "package-deb.sh: dangling link $l -> $(readlink "$l")"; exit 1; }
done

# 3. /usr/share/doc: copyright (GPLv3 + the fork's notice), NOTICE.md, CHANGES.md ---
cp "$SRC/NOTICE.md" "$STAGE/usr/share/doc/$PKG/NOTICE.md"
gzip -9n -c "$SRC/CHANGES.md" >"$STAGE/usr/share/doc/$PKG/CHANGES.md.gz"
cat >"$STAGE/usr/share/doc/$PKG/copyright" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Cably Desktop (based on KiCad)
Upstream-Contact: Cably <dev@cably.dev>
Source: $SOURCE_URL
Comment:
 Cably Desktop is a fork of KiCad, the free EDA CAD application
 (https://www.kicad.org/).  The complete corresponding source of this build is
 $SOURCE_URL (the fork), whose CHANGES.md
 lists every modification made to KiCad (GPLv3 section 5(a)); the attribution
 notice is /usr/share/doc/$PKG/NOTICE.md and the change record
 /usr/share/doc/$PKG/CHANGES.md.gz.  KiCad and the KiCad logo are trademarks of
 the KiCad project; this package is not endorsed by the KiCad project.

Files: *
Copyright: The KiCad Developers, see AUTHORS.txt for contributors
           2026 Cably <dev@cably.dev> (modifications, listed in CHANGES.md)
License: GPL-3.0-or-later

License: GPL-3.0-or-later
 This program is free software: you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation, either version 3 of the License, or (at your
 option) any later version.
 .
 This program is distributed in the hope that it will be useful, but
 WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 General Public License for more details.
 .
 On Debian systems, the complete text of the GNU General Public License
 version 3 can be found in /usr/share/common-licenses/GPL-3.
EOF

# 4. Depends from the binaries (dpkg-shlibdeps) ------------------------------------
T0=$(secs)
# Every regular file, by content: the *.kiface modules are installed 0644 and are not
# named *.so, yet they carry most of the NEEDED entries (poppler, odbc, ...).
ELFS=()
while IFS= read -r f; do
  case "$(file -b "$f")" in ELF*) ELFS+=("$f") ;; esac
done < <(find "$OPT/bin" "$OPT/lib" -type f)
[ "${#ELFS[@]}" -gt 0 ] || { echo "package-deb.sh: no ELF binaries under $OPT"; exit 1; }
SHL="$WORK/shlibdeps"; mkdir -p "$SHL/debian"
printf 'Source: %s\nSection: electronics\nPriority: optional\nMaintainer: %s\n\nPackage: %s\nArchitecture: any\nDescription: shlibdeps scratch\n' "$PKG" "$MAINT" "$PKG" >"$SHL/debian/control"
# The package's own shared libraries (libkicommon & co.) have no shlibs entry in any
# installed package; debian/shlibs.local tells dpkg-shlibdeps they come from this one
# (and -x drops the resulting self-dependency), so the strict run passes and a system
# library that really lacks dependency information is still an error.
: >"$SHL/debian/shlibs.local"
for so in "$OPT"/lib/lib*.so.*; do
  [ -f "$so" ] && [ ! -L "$so" ] || continue
  SONAME=$(objdump -p "$so" 2>/dev/null | awk '/SONAME/{print $2}')
  case "$SONAME" in lib*.so.*) printf '%s %s %s\n' "${SONAME%%.so.*}" "${SONAME#*.so.}" "$PKG" >>"$SHL/debian/shlibs.local" ;; esac
done
LIBDIRS=(-l"$OPT/lib" -l"$OPT/lib/kicad/plugins/3d" -l"$OPT/lib/python3/dist-packages")
if ! (cd "$SHL" && dpkg-shlibdeps -O -x"$PKG" "${LIBDIRS[@]}" "${ELFS[@]}" >"$WORK/shlibs.out" 2>"$WORK/shlibs.err"); then
  echo "package-deb.sh: dpkg-shlibdeps failed strictly, retrying with --ignore-missing-info:"; grep -v '^dpkg-shlibdeps: warning' "$WORK/shlibs.err" | head -10 | sed 's/^/    /'
  (cd "$SHL" && dpkg-shlibdeps -O -x"$PKG" --ignore-missing-info "${LIBDIRS[@]}" "${ELFS[@]}" >"$WORK/shlibs.out" 2>"$WORK/shlibs.err") \
    || { cat "$WORK/shlibs.err" | tail -20; echo "package-deb.sh: dpkg-shlibdeps failed"; exit 1; }
fi
SHLIBS=$(sed -n 's/^shlibs:Depends=//p' "$WORK/shlibs.out")
[ -n "$SHLIBS" ] || { echo "package-deb.sh: dpkg-shlibdeps produced no shlibs:Depends"; cat "$WORK/shlibs.err" | tail -10; exit 1; }
# + what the binaries cannot declare: libsecret-1-0 (the Linux secret store; also found
# by shlibdeps, kept explicit), python3 (KiCad's scripting), libngspice0 (the simulator
# dlopens it).  One entry per package name, the versioned one winning; sorted for a
# stable control file.
DEPENDS=$(printf '%s, libsecret-1-0, python3, libngspice0\n' "$SHLIBS" | tr ',' '\n' | sed 's/^ *//;s/ *$//' | grep -v '^$' \
          | awk '{ v = ($0 ~ /\(/) ? 0 : 1; print v "\t" $0 }' | sort | cut -f2- | awk '!seen[$1]++' | sort | paste -sd, - | sed 's/,/, /g')
echo "package-deb.sh: Depends from ${#ELFS[@]} ELF files in $(( $(secs) - T0 )) s: $(echo "$DEPENDS" | tr ',' '\n' | wc -l | tr -d ' ') packages"

# 5. DEBIAN/control + build ----------------------------------------------------------
mkdir -p "$STAGE/DEBIAN"
cat >"$STAGE/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Architecture: $ARCH
Maintainer: $MAINT
Installed-Size: $(du -sk --exclude=DEBIAN "$STAGE" | cut -f1)
Depends: $DEPENDS
Recommends: desktop-file-utils, hicolor-icon-theme, shared-mime-info, xdg-utils
Section: electronics
Priority: optional
Homepage: $HOMEPAGE
Description: Cably Desktop, based on KiCad
 Desktop companion of cably.dev for editing electronic schematics and printed
 circuit boards: schematic capture, circuit simulation, PCB layout, 3D
 rendering and export to manufacturing formats, with sign-in to a Cably
 account to open cloud projects and save them back.
 .
 Cably Desktop is a fork of KiCad (Copyright The KiCad Developers), free
 software under the GNU General Public License version 3 or later; the
 complete corresponding source is $SOURCE_URL
 (see /usr/share/doc/$PKG/copyright and NOTICE.md).  It installs under
 /opt/cably-desktop and does not replace a distribution KiCad.
EOF
chmod 0755 "$STAGE/DEBIAN"
find "$STAGE" -type d -exec chmod 0755 {} +

T0=$(secs)
rm -f "$DEB"
THREADS=""
dpkg-deb --help 2>&1 | grep -q -- '--threads-max' && THREADS="--threads-max=$(nproc)"
# shellcheck disable=SC2086
dpkg-deb --build --root-owner-group -Zxz -z6 $THREADS "$STAGE" "$DEB" >"$WORK/deb.log" 2>&1 || { cat "$WORK/deb.log"; exit 1; }
echo "package-deb.sh: dpkg-deb took $(( $(secs) - T0 )) s; total $(( $(secs) - T_ALL )) s"
echo "package-deb.sh: wrote $(du -h "$DEB" | cut -f1) $(basename "$DEB") ($(dpkg-deb --contents "$DEB" | wc -l | tr -d ' ') entries)"
echo "$DEB"
