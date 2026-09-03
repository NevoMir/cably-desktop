#!/usr/bin/env bash
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
#  (c) The pure recent-projects helper (cably/src/cably_home_recent.h) passes
#      its unit test, compiled standalone against the toolchain's wxWidgets
#      (KICAD_BUILD_QA_TESTS is OFF in the kicad-mac-builder build, so a qa/
#      Boost target would never run; this is the cheap equivalent).
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
         "Sign in to Cably to generate (coming in the next phase)"; do
  if strings - "$BIN" | grep -qF "$s"; then ok "binary carries \"$s\""; else bad "binary lacks \"$s\""; fi
done
if [ -x "$OFFICIAL/Contents/MacOS/kicad" ]; then
  if strings - "$OFFICIAL/Contents/MacOS/kicad" | grep -qF "Describe the circuit you want"; then
    bad "negative control: official KiCad already contains the prompt string — assertion vacuous"
  else ok "negative control: official KiCad lacks the prompt string"; fi
else echo "  skip negative control: no official KiCad at $OFFICIAL"; fi

# (c) ------------------------------------------------------------------------
if [ -x "$WXCONFIG" ]; then
  T=$(mktemp -d)
  WXLIB="$("$WXCONFIG" --prefix)/lib"
  if clang++ -std=c++17 -I"$FORK/cably/src" $("$WXCONFIG" --cxxflags) \
        "$FORK/cably/tests/unit/test_cably_home_recent.cpp" \
        $("$WXCONFIG" --libs base) -Wl,-rpath,"$WXLIB" -o "$T/test_recent" 2>"$T/cc.log"; then
    if "$T/test_recent" >"$T/run.log" 2>&1; then ok "unit test: $(tail -1 "$T/run.log")"
    else bad "unit test failed:"; sed 's/^/       /' "$T/run.log" | tail -20; fi
  else bad "unit test did not compile:"; sed 's/^/       /' "$T/cc.log" | head -20; fi
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
