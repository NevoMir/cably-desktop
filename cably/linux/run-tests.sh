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
# Linux test runner for Cably Desktop.  Written BEFORE the Linux build existed (it is
# RED on an empty build volume) and meant to be run after cably/linux/build.sh, inside
# the container built from cably/linux/Dockerfile.build or on the CI runner
# (.github/workflows/linux.yml runs it under xvfb-run):
#
#   docker run --rm -v <fork>:/src:ro -v cably-linux-build:/build \
#       -v cably-linux-install:/opt/cably-desktop \
#       cably-desktop-linux-build:ubuntu24.04 /src/cably/linux/run-tests.sh
#
# What it asserts (every line is "  ok   ..." or "  FAIL ...", exit 1 on any FAIL):
#  (1) The standalone unit tests build and pass on Linux with the system compiler and
#      the distro wxWidgets (wx-config): test_cably_bridge + test_cably_sync (pure C++17
#      + nlohmann/json + picosha2, no wx), test_cably_secret_store (the Linux secret
#      store: libsecret behind a fake Secret Service, the 0600 file fallback),
#      test_cably_home_recent, test_cably_home_cloud, test_cably_sync_status (wxBase).
#      Same recipe as cably/tests/bridge.sh (a)/(f) and cably/tests/home.sh (c) on macOS.
#  (2) cably/tests/bridge.sh against the Ninja build tree in /build: it builds the
#      cably-bridge-cli target there and drives it against the python mock cloud
#      (loopback handoff, list/open/conflict/refresh, the F5 watcher) and, in its (e)
#      section, the persistent secret store: save -> show -> signout through whatever
#      backend this machine has (the 0600 file when no Secret Service answers).
#  (3) cably/tests/theme.sh with the INSTALLED kicad-cli (/opt/cably-desktop) and the
#      fixtures (cably/tests/fixtures by default): key completeness + the render oracle.
#  (4) The installed manager (/opt/cably-desktop/bin/kicad) launches under Xvfb
#      (xvfb-run -a), is still alive after 12 s, and exits on SIGTERM.
#  (5) cably/tests/identity-linux.sh on the installed tree: launchers, icons, metainfo
#      and binaries present the product as Cably Desktop, based on KiCad.
#  (6) cably/linux/package-deb.sh writes the .deb into $CABLY_DEB_DIR (default
#      $CABLY_BUILD_DIR/deb) and cably/tests/deb.sh checks it: the static assertions
#      here, and the fresh-Ubuntu install + launch + theme.sh when a Docker daemon (or
#      root) is available - inside the build container that phase is reported as a WARN
#      with the command to run on the Docker host.
set -uo pipefail
FORK="${CABLY_FORK:-/src}"
BUILD="${CABLY_BUILD_DIR:-/build}"
PREFIX="${CABLY_PREFIX:-/opt/cably-desktop}"
FIX="${CABLY_FIXTURES:-$FORK/cably/tests/fixtures}"
DEBDIR="${CABLY_DEB_DIR:-$BUILD/deb}"
LAUNCH_SECS="${CABLY_LAUNCH_SECS:-12}"
CXX="${CXX:-clang++}"
WXCONFIG="${CABLY_WX_CONFIG:-$(command -v wx-config || true)}"
fail=0; ok(){ echo "  ok   $1"; }; bad(){ echo "  FAIL $1"; fail=1; }
section(){ echo; echo "== $1"; }

T=$(mktemp -d)
KICAD_PID=""; XVFB_PID=""
cleanup(){ [ -n "$KICAD_PID" ] && kill -9 "$KICAD_PID" 2>/dev/null; [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT

# The container user's home may not exist or be writable (CI); KiCad and its cli need
# a writable config dir.
if [ ! -w "${HOME:-/nonexistent}" ]; then export HOME="$T/home"; mkdir -p "$HOME"; fi
export LANG="${LANG:-C.UTF-8}" LC_ALL="${LC_ALL:-C.UTF-8}"
T_ALL=$(date +%s)

echo "run-tests.sh: fork=$FORK build=$BUILD prefix=$PREFIX fixtures=$FIX deb=$DEBDIR ($(uname -sm), $(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME"))"

# (1) ----------------------------------------------------------------------------
section "(1) unit tests"
command -v "$CXX" >/dev/null && ok "compiler: $CXX ($("$CXX" --version | head -1))" || bad "compiler $CXX not found"
INC=(-I"$FORK/cably/src" -I"$FORK/thirdparty/nlohmann_json" -I"$FORK/thirdparty/picosha2")

unit_pure(){ # $1 test name, rest: extra compiler args + sources
  local t="$1"; shift
  if "$CXX" -std=c++17 -Wall "${INC[@]}" "$@" "$FORK/cably/tests/unit/$t.cpp" -o "$T/$t" -pthread 2>"$T/$t.cc.log"; then
    if "$T/$t" >"$T/$t.run.log" 2>&1; then ok "$t: $(tail -1 "$T/$t.run.log")"
    else bad "$t failed:"; sed 's/^/       /' "$T/$t.run.log" | tail -20; fi
  else bad "$t did not compile:"; sed 's/^/       /' "$T/$t.cc.log" | head -30; fi
}
unit_pure test_cably_bridge "$FORK/cably/src/cably_bridge.cpp"
unit_pure test_cably_sync   "$FORK/cably/src/cably_bridge.cpp" "$FORK/cably/src/cably_sync.cpp"
# The secret store with the real libsecret backend compiled in (probed once; the fake
# service drives the rest).  Without libsecret-1-dev the file is the only backend.
if pkg-config --exists libsecret-1 2>/dev/null; then
  ok "libsecret-1 $(pkg-config --modversion libsecret-1) (pkg-config)"
  # shellcheck disable=SC2046
  unit_pure test_cably_secret_store -DCABLY_HAVE_LIBSECRET=1 $(pkg-config --cflags libsecret-1) \
      "$FORK/cably/src/cably_bridge.cpp" "$FORK/cably/src/cably_bridge_keychain.cpp" $(pkg-config --libs libsecret-1)
else
  bad "libsecret-1 not found by pkg-config (apt install libsecret-1-dev); secret store test built without it:"
  unit_pure test_cably_secret_store "$FORK/cably/src/cably_bridge.cpp" "$FORK/cably/src/cably_bridge_keychain.cpp"
fi

if [ -n "$WXCONFIG" ] && [ -x "$WXCONFIG" ]; then
  ok "wx-config: $WXCONFIG (wx $("$WXCONFIG" --version))"
  for t in test_cably_home_recent test_cably_home_cloud test_cably_sync_status; do
    if "$CXX" -std=c++17 "${INC[@]}" $("$WXCONFIG" --cxxflags) "$FORK/cably/tests/unit/$t.cpp" \
          $("$WXCONFIG" --libs base) -o "$T/$t" 2>"$T/$t.cc.log"; then
      if "$T/$t" >"$T/$t.run.log" 2>&1; then ok "$t: $(tail -1 "$T/$t.run.log")"
      else bad "$t failed:"; sed 's/^/       /' "$T/$t.run.log" | tail -20; fi
    else bad "$t did not compile:"; sed 's/^/       /' "$T/$t.cc.log" | head -20; fi
  done
else bad "wx-config not found (set CABLY_WX_CONFIG); wxBase unit tests not run"; fi

# (2) ----------------------------------------------------------------------------
section "(2) cably/tests/bridge.sh against $BUILD"
if [ -f "$BUILD/CMakeCache.txt" ]; then
  if CABLY_INNER_BUILD="$BUILD" CABLY_BUILD_DIR="$BUILD" CABLY_JOBS="${CABLY_JOBS:-6}" bash "$FORK/cably/tests/bridge.sh" >"$T/bridge.log" 2>&1; then
    sed 's/^/     /' "$T/bridge.log"; ok "bridge.sh: PASS ($(grep -c '^  ok' "$T/bridge.log") checks)"
  else sed 's/^/     /' "$T/bridge.log"; bad "bridge.sh: FAIL ($(grep -c '^  FAIL' "$T/bridge.log") failing checks, see above)"; fi
  grep -q "^  ok   secret store: save -> show round-trips" "$T/bridge.log" && ok "bridge.sh exercised the Linux secret store ($(sed -n 's/^  ok   secret store: .*(store_backend=\(.*\))$/\1/p' "$T/bridge.log" | head -1))" || bad "bridge.sh did not run the secret store round-trip"
  # GitHub's ubuntu runners have a session bus and no Secret Service on it; bridge.sh
  # (e) recreates that with dbus-run-session (package dbus-daemon) and must have run it.
  grep -q "^  ok   secret store (bus without a Secret Service): save -> show round-trips" "$T/bridge.log" && ok "bridge.sh exercised the bus-without-a-Secret-Service case (the GitHub runner condition)" || bad "bridge.sh did not run the bus-without-a-Secret-Service secret store case (dbus-run-session missing?)"
else bad "no configured build at $BUILD (CMakeCache.txt missing): run cably/linux/build.sh first"; fi

# (3) ----------------------------------------------------------------------------
section "(3) cably/tests/theme.sh with $PREFIX/bin/kicad-cli"
KCLI="$PREFIX/bin/kicad-cli"
if [ -x "$KCLI" ]; then
  ok "kicad-cli: $("$KCLI" version 2>/dev/null | head -1)"
  [ -f "$FIX/blinking-led.kicad_pcb" ] && [ -f "$FIX/blinking-led.kicad_sch" ] && ok "fixtures present at $FIX" || bad "fixtures missing at $FIX (blinking-led.kicad_pcb/.kicad_sch; set CABLY_FIXTURES)"
  if CABLY_KICAD_CLI="$KCLI" CABLY_FIXTURES="$FIX" bash "$FORK/cably/tests/theme.sh" >"$T/theme.log" 2>&1; then
    sed 's/^/     /' "$T/theme.log"; ok "theme.sh: PASS"
  else sed 's/^/     /' "$T/theme.log"; bad "theme.sh: FAIL"; fi
else bad "installed kicad-cli missing: $KCLI (run cably/linux/build.sh)"; fi

# (4) ----------------------------------------------------------------------------
section "(4) launch under Xvfb"
KBIN="$PREFIX/bin/kicad"
if [ ! -x "$KBIN" ]; then bad "installed manager missing: $KBIN"
elif ! command -v xvfb-run >/dev/null; then bad "xvfb-run not installed"
else
  xvfb-run -a -s "-screen 0 1280x800x24" "$KBIN" >"$T/kicad.log" 2>&1 &
  XVFB_PID=$!
  sleep "$LAUNCH_SECS"
  KICAD_PID=$(pgrep -f "^$KBIN" | head -1 || true)
  if kill -0 "$XVFB_PID" 2>/dev/null && [ -n "$KICAD_PID" ] && kill -0 "$KICAD_PID" 2>/dev/null; then
    ok "manager alive ${LAUNCH_SECS}s after launch (pid $KICAD_PID under xvfb-run $XVFB_PID)"
    kill -TERM "$KICAD_PID"
    for i in $(seq 1 30); do kill -0 "$KICAD_PID" 2>/dev/null || break; sleep 0.5; done
    if kill -0 "$KICAD_PID" 2>/dev/null; then bad "manager ignored SIGTERM for 15 s"; kill -9 "$KICAD_PID" 2>/dev/null
    else ok "manager exited on SIGTERM"; fi
    KICAD_PID=""
    for i in $(seq 1 20); do kill -0 "$XVFB_PID" 2>/dev/null || break; sleep 0.5; done
    kill -0 "$XVFB_PID" 2>/dev/null && { kill "$XVFB_PID" 2>/dev/null; echo "  WARN xvfb-run wrapper still alive after the manager exited; killed"; }
    XVFB_PID=""
  else
    bad "manager died within ${LAUNCH_SECS}s of launch (xvfb-run alive: $(kill -0 "$XVFB_PID" 2>/dev/null && echo yes || echo no)); tail of its output:"
    sed 's/^/       /' "$T/kicad.log" | tail -20
    XVFB_PID=""
  fi
fi

# (5) ----------------------------------------------------------------------------
section "(5) cably/tests/identity-linux.sh on $PREFIX"
if CABLY_FORK="$FORK" CABLY_PREFIX="$PREFIX" bash "$FORK/cably/tests/identity-linux.sh" >"$T/identity.log" 2>&1; then
  ok "identity-linux.sh: PASS ($(grep -c '^  ok' "$T/identity.log") checks)"
else sed 's/^/     /' "$T/identity.log" | grep -vE "^     +ok"; bad "identity-linux.sh: FAIL ($(grep -c '^  FAIL' "$T/identity.log") failing checks, see above)"; fi

# (6) ----------------------------------------------------------------------------
section "(6) cably/linux/package-deb.sh -> $DEBDIR, then cably/tests/deb.sh"
if [ -f "$BUILD/CMakeCache.txt" ] && [ -x "$PREFIX/bin/kicad" ]; then
  T0=$(date +%s)
  if CABLY_FORK="$FORK" CABLY_BUILD_DIR="$BUILD" CABLY_PREFIX="$PREFIX" CABLY_DEB_DIR="$DEBDIR" bash "$FORK/cably/linux/package-deb.sh" >"$T/deb-build.log" 2>&1; then
    DEB=$(tail -1 "$T/deb-build.log")
    grep -E '^package-deb\.sh:' "$T/deb-build.log" | sed 's/^/     /'
    [ -f "$DEB" ] && ok "package-deb.sh wrote $(basename "$DEB") ($(du -h "$DEB" | cut -f1)) in $(( $(date +%s) - T0 )) s" || bad "package-deb.sh printed no .deb path: $(tail -3 "$T/deb-build.log")"
    if CABLY_FORK="$FORK" CABLY_LAUNCH_SECS="$LAUNCH_SECS" bash "$FORK/cably/tests/deb.sh" "$DEB" >"$T/deb.log" 2>&1; then
      sed 's/^/     /' "$T/deb.log"; ok "deb.sh: PASS ($(grep -c '^  ok' "$T/deb.log") checks$(grep -q '^  WARN' "$T/deb.log" && echo '; install phase deferred to a Docker host, see WARN'))"
    else sed 's/^/     /' "$T/deb.log"; bad "deb.sh: FAIL ($(grep -c '^  FAIL' "$T/deb.log") failing checks, see above)"; fi
  else sed 's/^/     /' "$T/deb-build.log" | tail -20; bad "package-deb.sh failed"; fi
else bad "no build/install to package (run cably/linux/build.sh first)"; fi

echo
echo "run-tests.sh: $(( $(date +%s) - T_ALL )) s"
[ "$fail" = 0 ] && echo "run-tests.sh: PASS" || echo "run-tests.sh: FAIL"
exit $fail
