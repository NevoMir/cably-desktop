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
# F3(d) acceptance for the Cably home screen. Written BEFORE the panel existed.
#
#  (a) The built KiCad.app main binary carries the home panel's own UI strings —
#      i.e. cably/src/cably_home_panel.cpp was compiled AND linked into `kicad`
#      (a stale build, or a missing include(cably/CMakeLists.txt), fails here).
#      Negative control: the pre-fork KiCad main binary lacks them, so the
#      positive grep is not vacuous.
#  (b) The app launches (open -a), is still alive after 12 s (a home panel that
#      crashes the manager on construction fails here), and quits cleanly via
#      osascript addressed by the bundle id read from the built Info.plist.
#  (c) The pure helpers (cably/src/cably_home_recent.h, and since F4
#      cably/src/cably_home_cloud.h: project stem, ISO timestamps, "updated"
#      wording, generate URL) pass their unit tests, compiled standalone
#      against the toolchain's wxWidgets (KICAD_BUILD_QA_TESTS is OFF in the
#      kicad-mac-builder build, so a qa/ Boost target would never run; this is
#      the cheap equivalent).
#  F4 (home panel <-> bridge) extends (a): the binary must carry the sign-in,
#      projects-list, waiting, conflict and generate-on-web strings, and must
#      NOT carry the F3 placeholder "coming in the next phase" any more.
#  F5 (sync UI) extends (a) with the save-to-cloud strings ("Synced to Cably",
#      the cloud-changed dialog "changed on cably.dev since it was opened here" and
#      its three answers, the error/Retry wording) and (c) with
#      cably/src/cably_sync_status.h (report summariser, status line, clock),
#      compiled with the nlohmann/json header from thirdparty/ like the bridge test.
set -uo pipefail
FORK="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${CABLY_BUILD_DIR:-$FORK/../build.noindex}"
APP="${CABLY_APP:-$BUILD/kicad-dest/KiCad.app}"
OFFICIAL="${CABLY_OFFICIAL_APP:-/Applications/KiCad/KiCad.app}"
WXCONFIG="${CABLY_WX_CONFIG:-$BUILD/wxwidgets-dest/bin/wx-config}"
LAUNCH_SECS="${CABLY_LAUNCH_SECS:-12}"
fail=0; ok(){ echo "  ok   $1"; }; bad(){ echo "  FAIL $1"; fail=1; }

BIN="$APP/Contents/MacOS/kicad"
[ -x "$BIN" ] || { bad "built binary missing: $BIN"; exit 1; }

# (a) ------------------------------------------------------------------------
for s in "Describe the circuit you want" "Recent projects" "Cably Desktop" \
         "Sign in to Cably" "Your Cably projects" "Generates on cably.dev" \
         "Waiting for the browser" "Signed in as" \
         "This project was edited here since Cably last exported it" \
         "Synced to Cably" "changed on cably.dev since it was opened here" \
         "Keep my KiCad edits (overwrite cloud)" "Discard my edits (reload from Cably)" \
         "Couldn't sync to Cably" "Retry"; do
  # grep -F without -q: with pipefail, grep -q exiting early gives strings SIGPIPE and
  # the pipeline a non-zero status even on a match (measured 2026-09-03: false FAILs).
  if strings - "$BIN" | grep -F "$s" >/dev/null; then ok "binary carries \"$s\""; else bad "binary lacks \"$s\""; fi
done
# F4 retired the F3 placeholder: a binary that still carries it is a stale build.
if strings - "$BIN" | grep -F "coming in the next phase" >/dev/null; then bad "binary still carries the F3 placeholder \"coming in the next phase\" (stale build?)"; else ok "F3 placeholder gone"; fi
if [ -x "$OFFICIAL/Contents/MacOS/kicad" ]; then
  if strings - "$OFFICIAL/Contents/MacOS/kicad" | grep -F "Describe the circuit you want" >/dev/null; then
    bad "negative control: official KiCad already contains the prompt string — assertion vacuous"
  else ok "negative control: official KiCad lacks the prompt string"; fi
else echo "  skip negative control: no official KiCad at $OFFICIAL"; fi

# (c) ------------------------------------------------------------------------
if [ -x "$WXCONFIG" ]; then
  T=$(mktemp -d)
  WXLIB="$("$WXCONFIG" --prefix)/lib"
  for t in test_cably_home_recent test_cably_home_cloud test_cably_sync_status; do
    if clang++ -std=c++17 -I"$FORK/cably/src" -I"$FORK/thirdparty/nlohmann_json" $("$WXCONFIG" --cxxflags) \
          "$FORK/cably/tests/unit/$t.cpp" \
          $("$WXCONFIG" --libs base) -Wl,-rpath,"$WXLIB" -o "$T/$t" 2>"$T/cc.log"; then
      if "$T/$t" >"$T/run.log" 2>&1; then ok "unit test: $(tail -1 "$T/run.log")"
      else bad "unit test $t failed:"; sed 's/^/       /' "$T/run.log" | tail -20; fi
    else bad "unit test $t did not compile:"; sed 's/^/       /' "$T/cc.log" | head -20; fi
  done
else bad "wx-config not found at $WXCONFIG (set CABLY_WX_CONFIG)"; fi

# (b) ------------------------------------------------------------------------
# Measured 2026-09-03: BOTH `open -a <path>` and `open <path>` resolve through
# LaunchServices and launch the OFFICIAL /Applications KiCad while the bundle id is
# still org.kicad.kicad, so the built binary is exec'd directly (the only way to be
# sure it is ours).  Also measured: KiCad (the official 10.0.1 too) does not answer
# the `quit` AppleEvent for a while after launch (-1712 timed out), so a failed
# osascript quit is a WARN with a SIGTERM fallback, not a FAIL; the assertion that
# matters is that the manager (home panel included) is alive after $LAUNCH_SECS s.
BUNDLE_ID=$(/usr/libexec/PlistBuddy -c "Print CFBundleIdentifier" "$APP/Contents/Info.plist" 2>/dev/null || true)
[ -n "$BUNDLE_ID" ] || bad "no CFBundleIdentifier in $APP/Contents/Info.plist"
if pgrep -f "MacOS/kicad$" >/dev/null; then
  bad "launch: another kicad manager is already running ($(pgrep -fl 'MacOS/kicad$' | head -1)); close it and rerun"
else
  "$BIN" >/dev/null 2>&1 &
  PID=$!
  sleep "$LAUNCH_SECS"
  if kill -0 "$PID" 2>/dev/null; then ok "process alive ${LAUNCH_SECS}s after launch (pid $PID, $BIN)"; else bad "process died within ${LAUNCH_SECS}s of launch"; fi
  if kill -0 "$PID" 2>/dev/null; then
    osascript -e "with timeout of 5 seconds
tell application id \"$BUNDLE_ID\" to quit
end timeout" >/dev/null 2>&1 || true
    for i in $(seq 1 20); do kill -0 "$PID" 2>/dev/null || break; sleep 0.5; done
    if kill -0 "$PID" 2>/dev/null; then
      echo "  WARN osascript quit (bundle id $BUNDLE_ID) not honoured within 10 s; sending SIGTERM"
      kill "$PID" 2>/dev/null || true; sleep 1; kill -9 "$PID" 2>/dev/null || true
    else ok "quit cleanly via osascript (bundle id $BUNDLE_ID)"; fi
  fi
  # never leave an official KiCad behind if LaunchServices substituted it for the quit target
  pkill -f "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad$" 2>/dev/null || true
fi

[ "$fail" = 0 ] && echo "home.sh: PASS" || echo "home.sh: FAIL"
exit $fail
