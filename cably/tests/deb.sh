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
# Acceptance test for the Linux package (the counterpart of cably/tests/dmg.sh): the .deb
# cably/linux/package-deb.sh writes presents the product as Cably Desktop, carries the
# GPL record, and INSTALLS AND RUNS on a fresh Ubuntu 24.04.  Written BEFORE
# package-deb.sh existed (RED: no .deb to test).  Every check prints "  ok   ..." or
# "  FAIL ..."; exit 1 on any FAIL.
#
#  (a) static, with dpkg-deb: file name cably-desktop_<version>_<arch>.deb; control
#      fields Package cably-desktop, Version 10.0.6+cably.<yyyymmdd>.<rev>, Architecture
#      = this dpkg's, Maintainer Cably <dev@cably.dev>, Description first line
#      "Cably Desktop, based on KiCad", Depends naming libsecret-1-0 and the wx/GTK
#      runtime; contents: /opt/cably-desktop/bin/{kicad,kicad-cli}, the org.cably.desktop
#      launchers both under /opt and in /usr/share/applications (absolute Exec=), the
#      icon + metainfo links, /usr/share/doc/cably-desktop/{copyright,NOTICE.md},
#      /usr/bin/cably-desktop; nothing named org.kicad.*.desktop; nothing outside
#      /opt/cably-desktop, /usr/share and /usr/bin; copyright names GPL-3 and NOTICE.md;
#      the binaries are stripped.
#  (b) install: a FRESH `docker run --rm ubuntu:24.04` (or, with CABLY_DEB_INSTALL=local,
#      this very machine as root) does apt-get update; apt-get install -y ./x.deb xvfb
#      (apt resolves Depends - a wrong package name fails right here); then
#      /opt/cably-desktop/bin/kicad-cli version prints 10.0.6, cably-desktop is on PATH,
#      the /usr/share launcher validates, /opt/cably-desktop/bin/kicad launched under
#      xvfb-run is alive after 12 s and exits on SIGTERM, and cably/tests/theme.sh passes
#      with CABLY_KICAD_CLI pointed at the installed kicad-cli.
#
# Usage: cably/tests/deb.sh [file.deb]            (default: newest *.deb in $CABLY_DEB_DIR, else cwd)
#   CABLY_DEB_INSTALL  auto (default) | docker | local | static
#       auto:   docker when the docker CLI works, else local when root (or sudo -n) on a
#               dpkg system, else static only + a WARN line (that is what happens inside
#               the build container: run this script on the Docker host for phase (b)).
#   CABLY_FORK         the source tree (theme.sh + fixtures; default: this script's tree)
#   CABLY_LAUNCH_SECS  seconds the manager must stay alive (default 12)
#
# --inner <deb> is the phase (b) body, run as root inside the fresh system.
set -uo pipefail
FORK="${CABLY_FORK:-$(cd "$(dirname "$0")/../.." && pwd)}"
LAUNCH_SECS="${CABLY_LAUNCH_SECS:-12}"
MODE="${CABLY_DEB_INSTALL:-auto}"
PKG=cably-desktop
fail=0; ok(){ echo "  ok   $1"; }; bad(){ echo "  FAIL $1"; fail=1; }
section(){ echo; echo "== $1"; }

# (a) ------------------------------------------------------------------------------
static_checks(){ # $1 deb
  local DEB="$1" NAME; NAME=$(basename "$DEB")
  section "(a) static: $NAME ($(du -h "$DEB" | cut -f1))"
  [[ "$NAME" =~ ^cably-desktop_10\.0\.6\+cably\.[0-9]{8}\.[0-9a-f]{7,}_(amd64|arm64)\.deb$ ]] && ok "file name matches cably-desktop_10.0.6+cably.<yyyymmdd>.<rev>_<arch>.deb" || bad "file name '$NAME' does not match ^cably-desktop_10.0.6+cably.<yyyymmdd>.<rev>_<arch>.deb$"
  local field; field(){ dpkg-deb --field "$DEB" "$1" 2>/dev/null; }
  [ "$(field Package)" = "$PKG" ] && ok "Package: $PKG" || bad "Package: '$(field Package)'"
  local V; V=$(field Version)
  [[ "$V" =~ ^10\.0\.6\+cably\.[0-9]{8}\.[0-9a-f]{7,}$ ]] && ok "Version: $V" || bad "Version: '$V' (want 10.0.6+cably.<yyyymmdd>.<rev>)"
  local A; A=$(field Architecture)
  if command -v dpkg >/dev/null; then [ "$A" = "$(dpkg --print-architecture)" ] && ok "Architecture: $A (this dpkg's)" || bad "Architecture: '$A' (this dpkg: $(dpkg --print-architecture))"
  else [[ "$A" =~ ^(amd64|arm64)$ ]] && ok "Architecture: $A" || bad "Architecture: '$A'"; fi
  [ "$(field Maintainer)" = "Cably <dev@cably.dev>" ] && ok "Maintainer: Cably <dev@cably.dev>" || bad "Maintainer: '$(field Maintainer)'"
  local D1; D1=$(field Description | head -1)
  [ "$D1" = "Cably Desktop, based on KiCad" ] && ok "Description: $D1" || bad "Description first line: '$D1'"
  field Description | grep -q "GNU General Public License" && ok "Description carries the GPL notice" || bad "Description lacks the GPL notice"
  field Description | grep -q "github.com/NevoMir/cably-desktop" && ok "Description names the corresponding source" || bad "Description lacks the source URL"
  local DEP; DEP=$(field Depends)
  for p in libsecret-1-0 libwxgtk3.2 libgtk-3-0 libc6 python3; do
    grep -qE "(^|, )$p" <<<"$DEP" && ok "Depends: $p" || bad "Depends lacks $p: $DEP"
  done
  grep -qE "\(>= " <<<"$DEP" && ok "Depends carry versions (dpkg-shlibdeps): $(echo "$DEP" | tr ',' '\n' | wc -l | tr -d ' ') packages" || bad "Depends have no versioned entries (not derived from the binaries?)"
  [ "$(field Section)" = electronics ] && ok "Section: electronics" || bad "Section: '$(field Section)'"
  [ "$(field Homepage)" = "https://cably.dev" ] && ok "Homepage: https://cably.dev" || bad "Homepage: '$(field Homepage)'"
  local IS; IS=$(field Installed-Size); [ "${IS:-0}" -gt 100000 ] && ok "Installed-Size: $IS kB" || bad "Installed-Size: '$IS'"

  local C; C=$(dpkg-deb --contents "$DEB" 2>/dev/null)
  local paths; paths=$(echo "$C" | awk '{print $6}')
  has(){ grep -qx "$1" <<<"$paths"; }
  for p in ./opt/cably-desktop/bin/kicad ./opt/cably-desktop/bin/kicad-cli ./opt/cably-desktop/bin/pcbnew ./opt/cably-desktop/bin/eeschema \
           ./opt/cably-desktop/lib/libkicommon.so.10.0.6 \
           ./opt/cably-desktop/share/applications/org.cably.desktop.desktop ./opt/cably-desktop/share/applications/org.cably.desktop.pcbnew.desktop \
           ./opt/cably-desktop/share/metainfo/org.cably.desktop.metainfo.xml ./opt/cably-desktop/share/mime/packages/kicad-kicad.xml \
           ./opt/cably-desktop/share/icons/hicolor/48x48/apps/org.cably.desktop.png ./opt/cably-desktop/share/icons/hicolor/scalable/apps/org.cably.desktop.svg \
           ./usr/share/applications/org.cably.desktop.desktop ./usr/share/applications/org.cably.desktop.eeschema.desktop \
           ./usr/share/icons/hicolor/48x48/apps/org.cably.desktop.png ./usr/share/icons/hicolor/128x128/apps/org.cably.desktop.pcbnew.png \
           ./usr/share/metainfo/org.cably.desktop.metainfo.xml ./usr/share/mime/packages/cably-desktop-kicad.xml \
           ./usr/share/doc/cably-desktop/copyright ./usr/share/doc/cably-desktop/NOTICE.md ./usr/share/doc/cably-desktop/CHANGES.md.gz \
           ./usr/bin/cably-desktop ./usr/bin/cably-desktop-cli; do
    has "$p" && ok "ships $p" || bad "missing $p"
  done
  grep -q "org.kicad.*\.desktop" <<<"$paths" && bad "ships an org.kicad launcher: $(grep 'org.kicad.*\.desktop' <<<"$paths" | tr '\n' ' ')" || ok "no org.kicad.*.desktop launcher"
  grep -qE "^\./usr/share/icons/hicolor/[0-9x]+/apps/(kicad|pcbnew|eeschema)\.png$" <<<"$paths" && bad "links KiCad-named app icons into /usr/share/icons" || ok "no KiCad-named app icon under /usr/share/icons"
  grep -qE "^\./usr/share/(icons/hicolor/[0-9x]+/mimetypes|mime/packages/kicad-)" <<<"$paths" && bad "would collide with a distro kicad package under /usr/share (mimetype icons / kicad-*.xml)" || ok "nothing under /usr/share that a distro kicad package owns"
  local OUTSIDE; OUTSIDE=$(grep -vE '^\./$|^\./opt/$|^\./opt/cably-desktop(/|$)|^\./usr/$|^\./usr/share(/|$)|^\./usr/bin(/|$)' <<<"$paths" || true)
  [ -z "$OUTSIDE" ] && ok "every path is under /opt/cably-desktop, /usr/share or /usr/bin" || bad "paths outside the allowed roots: $(echo "$OUTSIDE" | head -5 | tr '\n' ' ')"
  [ -n "$(grep -E " \./usr/bin/cably-desktop -> " <<<"$C" | grep "/opt/cably-desktop/bin/kicad$")" ] && ok "/usr/bin/cably-desktop -> /opt/cably-desktop/bin/kicad" || bad "/usr/bin/cably-desktop is not a link to /opt/cably-desktop/bin/kicad"
  grep -qE "^-rwxr-xr-x root/root .* \./opt/cably-desktop/bin/kicad$" <<<"$C" && ok "bin/kicad is root-owned 0755" || bad "bin/kicad ownership/mode: $(grep ' \./opt/cably-desktop/bin/kicad$' <<<"$C")"

  local X; X=$(mktemp -d)
  dpkg-deb --fsys-tarfile "$DEB" | tar -x -C "$X" ./usr/share/doc/cably-desktop/copyright ./usr/share/applications/org.cably.desktop.desktop ./opt/cably-desktop/bin/kicad-cli 2>/dev/null
  grep -q "/usr/share/common-licenses/GPL-3" "$X/usr/share/doc/cably-desktop/copyright" 2>/dev/null && ok "copyright points at /usr/share/common-licenses/GPL-3" || bad "copyright does not point at GPL-3"
  grep -q "NOTICE.md" "$X/usr/share/doc/cably-desktop/copyright" 2>/dev/null && ok "copyright points at NOTICE.md" || bad "copyright does not mention NOTICE.md"
  grep -q "The KiCad Developers" "$X/usr/share/doc/cably-desktop/copyright" 2>/dev/null && ok "copyright credits The KiCad Developers" || bad "copyright lacks the KiCad attribution"
  grep -q "^Exec=/opt/cably-desktop/bin/kicad" "$X/usr/share/applications/org.cably.desktop.desktop" 2>/dev/null && ok "/usr/share launcher Exec is absolute (/opt/cably-desktop/bin/kicad)" || bad "/usr/share launcher Exec: $(grep '^Exec=' "$X/usr/share/applications/org.cably.desktop.desktop" 2>/dev/null)"
  grep -q "^Name=Cably Desktop$" "$X/usr/share/applications/org.cably.desktop.desktop" 2>/dev/null && ok "/usr/share launcher Name=Cably Desktop" || bad "/usr/share launcher Name: $(grep '^Name=' "$X/usr/share/applications/org.cably.desktop.desktop" 2>/dev/null)"
  if command -v file >/dev/null; then
    file -b "$X/opt/cably-desktop/bin/kicad-cli" | grep -q "not stripped" && bad "bin/kicad-cli is not stripped" || ok "bin/kicad-cli is stripped ($(file -b "$X/opt/cably-desktop/bin/kicad-cli" | cut -d, -f1))"
  fi
  rm -rf "$X"
}

# (b) body, as root on the fresh system --------------------------------------------
inner(){ # $1 deb (absolute)
  local DEB="$1"
  export DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8 LC_ALL=C.UTF-8
  static_checks "$DEB"
  section "(b) install on $(. /etc/os-release && echo "$PRETTY_NAME") ($(uname -m)), fresh"
  dpkg -s "$PKG" >/dev/null 2>&1 && bad "$PKG already installed here: not a fresh system" || ok "$PKG not installed before"
  local T0; T0=$(date +%s)
  if apt-get update -qq >/tmp/apt-update.log 2>&1; then ok "apt-get update ($(( $(date +%s) - T0 )) s)"; else bad "apt-get update failed:"; tail -5 /tmp/apt-update.log | sed 's/^/       /'; fi
  T0=$(date +%s)
  if apt-get install -y -qq --no-install-recommends "$DEB" xvfb xauth procps desktop-file-utils >/tmp/apt-install.log 2>&1; then
    ok "apt-get install ./$(basename "$DEB") xvfb: Depends resolved ($(( $(date +%s) - T0 )) s, $(grep -cE '^(Get|Unpacking)' /tmp/apt-install.log) packages fetched/unpacked)"
  else bad "apt-get install of the .deb failed (Depends not resolvable on Ubuntu 24.04?):"; grep -vE '^(Get|Unpacking|Selecting|Preparing|Setting up|Processing)' /tmp/apt-install.log | tail -15 | sed 's/^/       /'; echo; echo "deb.sh: FAIL"; exit 1; fi
  grep -q "^Status: install ok installed" <<<"$(dpkg -s "$PKG" 2>/dev/null)" && ok "dpkg: $PKG $(dpkg-query -W -f='${Version}' "$PKG") installed" || bad "dpkg does not report $PKG installed"
  local V; V=$(/opt/cably-desktop/bin/kicad-cli version 2>&1 | head -1)
  case "$V" in 10.0.6*) ok "kicad-cli version: $V" ;; *) bad "kicad-cli: $V" ;; esac
  /opt/cably-desktop/bin/kicad-cli version --format about 2>/dev/null | grep -E "^(Application|Version)" | sed 's/^/       /'
  [ "$(command -v cably-desktop)" = /usr/bin/cably-desktop ] && [ -x /usr/bin/cably-desktop ] && ok "cably-desktop on PATH -> $(readlink -f /usr/bin/cably-desktop)" || bad "cably-desktop not on PATH / not executable"
  grep -q "^10.0.6" <<<"$(cably-desktop-cli version 2>/dev/null)" && ok "cably-desktop-cli runs" || bad "cably-desktop-cli does not run"
  [ -r /usr/share/icons/hicolor/48x48/apps/org.cably.desktop.png ] && ok "icon link resolves: /usr/share/icons/hicolor/48x48/apps/org.cably.desktop.png" || bad "icon link dangling"
  [ -r /usr/share/metainfo/org.cably.desktop.metainfo.xml ] && ok "metainfo link resolves" || bad "metainfo link dangling"
  [ -r /usr/share/mime/packages/cably-desktop-kicad.xml ] && ok "mime link resolves" || bad "mime link dangling"
  if command -v desktop-file-validate >/dev/null; then
    local bad_d=0 f; for f in /usr/share/applications/org.cably.desktop*.desktop; do
      local O; O=$(desktop-file-validate "$f" 2>&1 | grep -v ': warning' || true); [ -n "$O" ] && { bad "desktop-file-validate $(basename "$f"): $O"; bad_d=1; }; done
    [ $bad_d = 0 ] && ok "desktop-file-validate clean on $(ls /usr/share/applications/org.cably.desktop*.desktop | wc -l | tr -d ' ') /usr/share launchers"
    [ -f /usr/share/applications/mimeinfo.cache ] && grep -q "application/x-kicad-pcb=.*org.cably.desktop" /usr/share/applications/mimeinfo.cache && ok "mimeinfo.cache maps application/x-kicad-pcb to a Cably launcher (dpkg trigger ran)" || echo "  note mimeinfo.cache not updated here (no trigger / no cache)"
  fi
  # launch under Xvfb
  section "(b) launch /opt/cably-desktop/bin/kicad under xvfb-run"
  export HOME="${HOME:-/root}"
  xvfb-run -a -s "-screen 0 1280x800x24" /opt/cably-desktop/bin/kicad >/tmp/kicad.log 2>&1 &
  local XPID=$!; sleep "$LAUNCH_SECS"
  local KPID; KPID=$(pgrep -f "^/opt/cably-desktop/bin/kicad" | head -1 || true)
  if kill -0 "$XPID" 2>/dev/null && [ -n "$KPID" ] && kill -0 "$KPID" 2>/dev/null; then
    ok "manager alive ${LAUNCH_SECS}s after launch (pid $KPID)"
    kill -TERM "$KPID"; local i; for i in $(seq 1 30); do kill -0 "$KPID" 2>/dev/null || break; sleep 0.5; done
    kill -0 "$KPID" 2>/dev/null && { bad "manager ignored SIGTERM for 15 s"; kill -9 "$KPID" 2>/dev/null; } || ok "manager exited on SIGTERM"
    for i in $(seq 1 20); do kill -0 "$XPID" 2>/dev/null || break; sleep 0.5; done; kill "$XPID" 2>/dev/null || true
  else bad "manager died within ${LAUNCH_SECS}s:"; tail -15 /tmp/kicad.log | sed 's/^/       /'; fi
  # theme.sh with the installed kicad-cli
  section "(b) cably/tests/theme.sh with the installed kicad-cli"
  if [ -f "$FORK/cably/tests/theme.sh" ]; then
    if CABLY_KICAD_CLI=/opt/cably-desktop/bin/kicad-cli CABLY_FIXTURES="$FORK/cably/tests/fixtures" bash "$FORK/cably/tests/theme.sh" >/tmp/theme.log 2>&1; then
      sed 's/^/     /' /tmp/theme.log; ok "theme.sh: PASS"
    else sed 's/^/     /' /tmp/theme.log; bad "theme.sh: FAIL"; fi
  else bad "no $FORK/cably/tests/theme.sh (mount the fork)"; fi
  echo; [ "$fail" = 0 ] && echo "deb.sh (inner): PASS" || echo "deb.sh (inner): FAIL"
  exit $fail
}

if [ "${1:-}" = "--inner" ]; then inner "$2"; fi

# 0. which .deb ---------------------------------------------------------------------
DEB="${1:-}"
if [ -z "$DEB" ]; then
  DIR="${CABLY_DEB_DIR:-$PWD}"
  DEB=$(ls -t "$DIR"/cably-desktop_*.deb 2>/dev/null | head -1)
fi
[ -n "$DEB" ] && [ -f "$DEB" ] || { echo "deb.sh: no cably-desktop_*.deb (looked in ${CABLY_DEB_DIR:-$PWD}; run cably/linux/package-deb.sh)"; echo "deb.sh: FAIL"; exit 1; }
DEB="$(cd "$(dirname "$DEB")" && pwd)/$(basename "$DEB")"
echo "deb.sh: $DEB ($(du -h "$DEB" | cut -f1)) mode=$MODE"

# resolve auto
if [ "$MODE" = auto ]; then
  if docker info >/dev/null 2>&1; then MODE=docker
  elif command -v apt-get >/dev/null && { [ "$(id -u)" = 0 ] || sudo -n true 2>/dev/null; }; then MODE=local
  else MODE=static; fi
fi

case "$MODE" in
  docker)
    # phase (a) runs inside too (dpkg-deb is there; the host may be macOS without dpkg)
    section "(b) fresh ubuntu:24.04 container (phase (a) runs inside it)"
    docker run --rm -v "$(dirname "$DEB")":/deb:ro -v "$FORK":/src:ro -e CABLY_FORK=/src -e CABLY_LAUNCH_SECS="$LAUNCH_SECS" \
        ubuntu:24.04 bash /src/cably/tests/deb.sh --inner "/deb/$(basename "$DEB")" 2>&1 | tee "${TMPDIR:-/tmp}/cably-deb-inner.log" | sed 's/^/   /'
    rc=${PIPESTATUS[0]}
    grep -q "^deb.sh (inner): PASS" "${TMPDIR:-/tmp}/cably-deb-inner.log" && [ "$rc" = 0 ] && ok "fresh-container install + launch + theme.sh: PASS" || bad "fresh-container phase failed (rc=$rc)" ;;
  local)
    if [ "$(id -u)" = 0 ]; then bash "$0" --inner "$DEB"; rc=$?; else sudo -n env CABLY_FORK="$FORK" CABLY_LAUNCH_SECS="$LAUNCH_SECS" bash "$0" --inner "$DEB"; rc=$?; fi
    [ "$rc" = 0 ] && ok "local install + launch + theme.sh: PASS" || bad "local phase failed (rc=$rc)" ;;
  static)
    if command -v dpkg-deb >/dev/null; then static_checks "$DEB"; else bad "no dpkg-deb here for the static checks and no docker/root for the install phase"; fi
    echo "  WARN install phase (b) not run here (no docker, not root): run  cably/tests/deb.sh $DEB  on a Docker host" ;;
  *) bad "unknown CABLY_DEB_INSTALL=$MODE" ;;
esac

echo
[ "$fail" = 0 ] && echo "deb.sh: PASS" || echo "deb.sh: FAIL"
exit $fail
