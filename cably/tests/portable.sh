#!/usr/bin/env bash
# macOS portability test: the app bundle must run on a Mac that has none of the
# build machine's Homebrew, /usr/local or /Library/Frameworks installs, and its
# embedded Python must be a complete, self-contained framework. Written BEFORE
# the fix; it must FAIL on the 2026-09-05 build (Homebrew Python references,
# stdlib-less Python.framework, "failed to get the Python codec of the filesystem
# encoding" at launch) and PASS on the official KiCad build in /Applications/KiCad.
#
# Given a .app (default: <build dir>/kicad-dest/KiCad.app) it asserts:
#  (a) every Mach-O in the bundle references only /usr/, /System/,
#      @executable_path/, @rpath/ and @loader_path/ libraries — the rule of
#      kicad-mac-builder/bin/verify-app.sh — and lists every offender;
#  (b) Contents/Frameworks/Python.framework/Versions/Current is a symlink to a
#      Versions/<X.Y> directory that holds lib/python<X.Y>/encodings/__init__.py
#      (the module whose absence is the fatal-init message) and bin/python3;
#  (c) with the environment KiCad sets (scripting/python_scripting.cpp:
#      PYTHONHOME=<Current>, PYTHONPATH=<SharedSupport>/scripting:<site-packages
#      under Current>) that bin/python3 runs "import encodings, pcbnew; print(
#      pcbnew.GetBuildVersion())" and prints the bundle's own major.minor.patch
#      (CFBundleShortVersionString up to the first "-");
#  (d) the site-packages holding pcbnew.py lives under the SAME Versions/<X.Y>
#      as Current, there is no other Versions/<n> directory, _pcbnew.so's load
#      command names that same Versions/<X.Y>/Python, and no stray
#      Contents/Frameworks/<n.n>/ directory exists (a framework copied from its
#      Versions/<n.n> directory instead of its root);
#  (e) Contents/MacOS/kicad, launched with a private KICAD_CONFIG_HOME, is
#      still alive after $LAUNCH_SECS s and its stderr contains none of
#      "Python path configuration", "Fatal Python error", "Unhandled exception";
#      then it exits on SIGTERM;
#  (f) launched again with DYLD_PRINT_LIBRARIES=1 for $DYLD_SECS s, no loaded
#      library outside the bundle itself starts with /opt/homebrew, /usr/local,
#      /Users or /Library/Frameworks. dyld prints nothing for a binary with the
#      hardened runtime (DYLD_* is dropped): that case is reported and passes
#      only when codesign confirms the runtime flag.
# Every check prints an "ok"/"FAIL" line; exit 1 on any FAIL.
#
# Usage: cably/tests/portable.sh [path/to/App.app]
#   CABLY_BUILD_DIR       build dir (default: <fork>/../build.noindex)
#   CABLY_PORTABLE_LAUNCH 0 to skip the launch checks (e)(f) (default 1)
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
set -uo pipefail
FORK="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${CABLY_BUILD_DIR:-$FORK/../build.noindex}"
LAUNCH_SECS=12
DYLD_SECS=8
fail=0; ok(){ echo "  ok   $1"; }; bad(){ echo "  FAIL $1"; fail=1; }
pl(){ /usr/libexec/PlistBuddy -c "Print $2" "$1" 2>/dev/null || true; }

APP="${1:-$BUILD/kicad-dest/KiCad.app}"
[ -d "$APP/Contents" ] || { echo "portable.sh: no app bundle at $APP"; exit 1; }
APP="$(cd "$APP" && pwd -P)"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/cably-portable.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
echo "portable: checking $APP"

# (a) non-relative, non-system references --------------------------------------
# Same rule as kicad-mac-builder/bin/verify-app.sh, over every regular file
# (symlinks skipped so a library is reported once, under its real name).
find "$APP" -type f -print0 | xargs -0 otool -L 2>/dev/null \
  | awk '/^[^\t]/{f=$0; sub(/:$/,"",f)} /^\t/{ if ($1 !~ /^(\/usr\/|\/System\/|@executable_path\/|@rpath\/|@loader_path\/)/) print f " -> " $1 }' \
  | sort -u > "$TMP/offenders"
n=$(wc -l < "$TMP/offenders" | tr -d ' ')
if [ "$n" = 0 ]; then ok "(a) no non-relative, non-system dylib references"
else
  bad "(a) $n non-relative, non-system dylib references in $(cut -d' ' -f1 "$TMP/offenders" | sort -u | wc -l | tr -d ' ') Mach-O files:"
  sed "s|^$APP/||; s|^|         |" "$TMP/offenders"
fi

# (b) Current -> a complete Python -----------------------------------------------
FW="$APP/Contents/Frameworks/Python.framework"
CUR="$FW/Versions/Current"
if [ -L "$CUR" ] && [ -d "$CUR" ]; then
  XY="$(basename "$(readlink "$CUR")")"
  ok "(b) Versions/Current -> $XY"
else
  XY=""; bad "(b) $CUR is not a symlink to a directory ($(ls "$FW/Versions" 2>/dev/null | tr '\n' ' '))"
fi
if [ -n "$XY" ]; then
  [ -f "$CUR/lib/python$XY/encodings/__init__.py" ] && ok "(b) Current has lib/python$XY/encodings/__init__.py" \
    || bad "(b) Current lacks lib/python$XY/encodings/__init__.py (no standard library: the 'Python codec of the filesystem encoding' fatal init)"
  [ -x "$CUR/bin/python3" ] && ok "(b) Current has bin/python3" || bad "(b) Current lacks bin/python3"
fi

# (c) KiCad's own PYTHONHOME/PYTHONPATH must import pcbnew ---------------------
WANT="$(pl "$APP/Contents/Info.plist" CFBundleShortVersionString | cut -d- -f1)"
if [ -n "$XY" ] && [ -x "$CUR/bin/python3" ]; then
  SP="$CUR/lib/python$XY/site-packages"
  got="$(cd "$TMP" && PYTHONHOME="$CUR" PYTHONPATH="$APP/Contents/SharedSupport/scripting:$SP" \
        "$CUR/bin/python3" -c 'import encodings, pcbnew; print(pcbnew.GetBuildVersion())' 2>"$TMP/pyerr")"
  case "$got" in
    "$WANT"*) ok "(c) Current/bin/python3 imports encodings+pcbnew; GetBuildVersion()=$got (bundle $WANT)" ;;
    *) bad "(c) python3 -c 'import encodings, pcbnew' under KiCad's PYTHONHOME/PYTHONPATH: got '$got', want $WANT*; stderr: $(tail -2 "$TMP/pyerr" | tr '\n' ' ')" ;;
  esac
else
  bad "(c) skipped: no runnable Current/bin/python3"
fi

# (d) pcbnew.py under the same Versions/<X.Y> ----------------------------------
PYS=(); while IFS= read -r f; do [ -n "$f" ] && PYS+=("$f"); done < <(find "$FW/Versions" -maxdepth 5 -name pcbnew.py -path '*/site-packages/*' 2>/dev/null | grep -v '/Versions/Current/')
VERS=(); while IFS= read -r d; do [ -n "$d" ] && VERS+=("$d"); done < <(cd "$FW/Versions" 2>/dev/null && find . -mindepth 1 -maxdepth 1 -type d | sed 's|^\./||')
if [ "${#PYS[@]}" = 1 ]; then
  PYXY="$(echo "${PYS[0]}" | sed -E 's|.*/Versions/([^/]+)/.*|\1|')"
  [ -n "$XY" ] && [ "$PYXY" = "$XY" ] && ok "(d) pcbnew.py is under Versions/$PYXY = Current" \
    || bad "(d) pcbnew.py is under Versions/$PYXY but Current -> '${XY:-?}'"
else
  bad "(d) expected exactly one pcbnew.py under Python.framework/Versions/*/lib/python*/site-packages, found ${#PYS[@]}"
fi
[ "${#VERS[@]}" = 1 ] && [ "${VERS[0]:-}" = "${XY:-}" ] && ok "(d) Versions/ holds only $XY" \
  || bad "(d) Versions/ holds [${VERS[*]:-}], expected only '${XY:-?}'"
# The Python that _pcbnew.so was linked against (its load command names Versions/<X.Y>/Python)
# must be the one Current points at; a framework copied to the wrong place shows up as a
# stray Contents/Frameworks/<n.n>/ directory.
SO="$(find "$FW/Versions" -maxdepth 5 -name _pcbnew.so -path '*/site-packages/*' 2>/dev/null | grep -v '/Versions/Current/' | head -1)"
if [ -n "$SO" ]; then
  LINKXY="$(otool -L "$SO" 2>/dev/null | grep -E 'Python\.framework/Versions/[0-9.]+/Python|@rpath/Versions/[0-9.]+/Python' | head -1 | sed -E 's|.*/Versions/([0-9.]+)/Python.*|\1|')"
  [ -n "$LINKXY" ] && [ "$LINKXY" = "${XY:-}" ] && ok "(d) _pcbnew.so links Python $LINKXY = Current" \
    || bad "(d) _pcbnew.so links Python '${LINKXY:-?}' but Current -> '${XY:-?}'"
else bad "(d) no _pcbnew.so under Python.framework/Versions/*/lib/python*/site-packages"; fi
STRAY="$(cd "$APP/Contents/Frameworks" && ls -d [0-9]*.[0-9]* 2>/dev/null | tr '\n' ' ')"
[ -z "$STRAY" ] && ok "(d) no stray Contents/Frameworks/<n.n>/ directory" \
  || bad "(d) stray Contents/Frameworks/$STRAY(a Python Versions/<n.n> directory copied as the framework root)"

# (e)(f) launch ------------------------------------------------------------------
BIN="$APP/Contents/MacOS/kicad"
if [ "${CABLY_PORTABLE_LAUNCH:-1}" != 1 ]; then echo "  skip (e)(f) launch (CABLY_PORTABLE_LAUNCH=0)"
elif [ ! -x "$BIN" ]; then bad "(e) no executable at $BIN"
else
  launch(){ # $1 = seconds, $2 = stderr file, rest = extra env; sets PID, ALIVE
    local secs="$1" err="$2"; shift 2
    env "$@" KICAD_CONFIG_HOME="$TMP/config" HOME="$TMP/home" "$BIN" </dev/null >/dev/null 2>"$err" &
    PID=$!; disown "$PID" 2>/dev/null; sleep "$secs"
    if kill -0 "$PID" 2>/dev/null; then ALIVE=1; else ALIVE=0; fi
  }
  stop(){ # $1 = pid; returns 0 if it exited on SIGTERM within 5 s
    kill -TERM "$1" 2>/dev/null; local i=0
    while kill -0 "$1" 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
    if kill -0 "$1" 2>/dev/null; then kill -KILL "$1" 2>/dev/null; return 1; fi
    return 0
  }
  mkdir -p "$TMP/config" "$TMP/home"
  launch "$LAUNCH_SECS" "$TMP/stderr"
  [ "$ALIVE" = 1 ] && ok "(e) Contents/MacOS/kicad alive after ${LAUNCH_SECS}s" || bad "(e) Contents/MacOS/kicad exited within ${LAUNCH_SECS}s (stderr: $(tail -3 "$TMP/stderr" | tr '\n' ' '))"
  for pat in "Python path configuration" "Fatal Python error" "Unhandled exception"; do
    if grep -q "$pat" "$TMP/stderr"; then bad "(e) stderr contains \"$pat\": $(grep -m1 -A3 "$pat" "$TMP/stderr" | tr '\n' ' ' | cut -c1-300)"
    else ok "(e) stderr has no \"$pat\""; fi
  done
  if [ "$ALIVE" = 1 ]; then stop "$PID" && ok "(e) exits on SIGTERM" || bad "(e) did not exit on SIGTERM within 5 s (killed)"; fi

  launch "$DYLD_SECS" "$TMP/dyld" DYLD_PRINT_LIBRARIES=1
  [ "$ALIVE" = 1 ] && stop "$PID" >/dev/null
  # dyld prints "dyld[<pid>]: <UUID> /path" (the UUID token is absent for unsigned images)
  grep -E '^dyld\[[0-9]+\]: ' "$TMP/dyld" | sed -E 's/^dyld\[[0-9]+\]: (<[0-9A-Fa-f-]+> )?//' | sort -u > "$TMP/loaded"
  nl=$(wc -l < "$TMP/loaded" | tr -d ' ')
  grep -v "^$APP/" "$TMP/loaded" | grep -E '^(/opt/homebrew|/usr/local|/Users|/Library/Frameworks)' > "$TMP/badload"
  if [ "$nl" = 0 ]; then
    if codesign -dv "$BIN" 2>&1 | grep -q 'flags=.*runtime'; then ok "(f) dyld printed nothing: hardened runtime drops DYLD_PRINT_LIBRARIES (signed release build)"
    else bad "(f) DYLD_PRINT_LIBRARIES=1 produced no dyld output and the binary has no hardened runtime"; fi
  elif [ -s "$TMP/badload" ]; then
    bad "(f) $(wc -l < "$TMP/badload" | tr -d ' ') libraries loaded from outside the bundle ($nl loaded):"; sed 's/^/         /' "$TMP/badload"
  else ok "(f) $nl libraries loaded, none from /opt/homebrew, /usr/local, /Users or /Library/Frameworks"; fi
fi

[ "$fail" = 0 ] && echo "portable: PASS" || echo "portable: FAIL"
exit $fail
