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
# F4 acceptance for the cloud bridge (cably/src/cably_bridge.*). Written BEFORE the
# bridge existed. Hermetic: no real network — a python mock plays Supabase + engine on
# 127.0.0.1, and the loopback sign-in handoff is driven with curl.
#
#  (a) Unit test cably/tests/unit/test_cably_bridge.cpp compiles standalone with
#      clang++ (pure C++17 + the nlohmann/json and picosha2 headers from thirdparty/,
#      no wx, no KiCad libs) and passes.
#  (b) The CLI target `cably-bridge-cli` builds inside the existing KiCad build tree
#      (it links kicommon, so it exercises the REAL KICAD_CURL_EASY transport).
#  (c) Against the mock: list projects; open one (fetch -> export -> write folder);
#      refuse to clobber an edited board, overwrite with --force; an expired bearer is
#      refreshed transparently; every HTTP call carries the documented path + headers.
#  (d) Loopback: the CLI prints port+state, binds 127.0.0.1 only, serves the /callback
#      page with the self-POST script, rejects a wrong state, accepts the right one
#      exactly once, then stops listening.
#  (e) macOS keychain store round-trip on a throw-away service name.
#  (f) F5 sync (cably/src/cably_sync.*): the unit test cably/tests/unit/test_cably_sync.cpp
#      builds standalone and passes; against the mock, with the REAL watcher on a real
#      folder: an in-place save and a write-temp-then-rename save each produce exactly one
#      POST /v1/import + one PATCH /rest/v1/projects (data.project replaced, chat kept);
#      backup/lock/autosave writes produce nothing; our own hand-off write (Expect) is not
#      a save; after the cloud changed (row updated_at newer than the sidecar's) the save
#      is reported cloud-changed with NO PATCH.  Prints the measured save->sync latency.
#  (g) Mutation check: with the content dedupe removed, or the updated_at comparison
#      inverted, the unit test must FAIL (proves the tests hold the two rules).
set -uo pipefail
FORK="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${CABLY_BUILD_DIR:-$FORK/../build.noindex}"
INNER="${CABLY_INNER_BUILD:-$BUILD/kicad/src/kicad-build}"
JOBS="${CABLY_JOBS:-6}"
fail=0; ok(){ echo "  ok   $1"; }; bad(){ echo "  FAIL $1"; fail=1; }

T=$(mktemp -d)
MOCK_PID=""; CLI_PID=""
WATCH_PID=""
cleanup(){ [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null; [ -n "$CLI_PID" ] && kill "$CLI_PID" 2>/dev/null; [ -n "$WATCH_PID" ] && kill "$WATCH_PID" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT

# The publishable key is public (it ships in every web bundle); read it from the header
# so the mock asserts the CLI sends exactly what the app will send.
APIKEY=$(sed -n 's/^#define CABLY_SUPABASE_PUBLISHABLE_KEY "\(.*\)"/\1/p' "$FORK/cably/src/cably_config.h")
[ -n "$APIKEY" ] || bad "CABLY_SUPABASE_PUBLISHABLE_KEY not defined in cably/src/cably_config.h"

# (a) ------------------------------------------------------------------------
if clang++ -std=c++17 -Wall -I"$FORK/cably/src" -I"$FORK/thirdparty/nlohmann_json" -I"$FORK/thirdparty/picosha2" \
      "$FORK/cably/src/cably_bridge.cpp" "$FORK/cably/tests/unit/test_cably_bridge.cpp" \
      -o "$T/test_bridge" 2>"$T/cc.log"; then
  if "$T/test_bridge" >"$T/run.log" 2>&1; then ok "unit test: $(tail -1 "$T/run.log")"
  else bad "unit test failed:"; sed 's/^/       /' "$T/run.log" | tail -20; fi
else bad "unit test did not compile:"; sed 's/^/       /' "$T/cc.log" | head -30; fi

# (f) unit half: the sync watcher + ImportProject, standalone as well.
SYNC_SRCS=("$FORK/cably/src/cably_bridge.cpp" "$FORK/cably/src/cably_sync.cpp")
if clang++ -std=c++17 -Wall -I"$FORK/cably/src" -I"$FORK/thirdparty/nlohmann_json" -I"$FORK/thirdparty/picosha2" \
      "${SYNC_SRCS[@]}" "$FORK/cably/tests/unit/test_cably_sync.cpp" -o "$T/test_sync" 2>"$T/cc2.log"; then
  if "$T/test_sync" >"$T/run2.log" 2>&1; then ok "sync unit test: $(tail -1 "$T/run2.log") [$(grep -o 'latency: [0-9]* ms' "$T/run2.log" | head -1)]"
  else bad "sync unit test failed:"; sed 's/^/       /' "$T/run2.log" | tail -20; fi
else bad "sync unit test did not compile:"; sed 's/^/       /' "$T/cc2.log" | head -30; fi

# (g) mutation check: each mutant must make the sync unit test FAIL.
mutant(){ # $1 label, $2 file, $3 sed expression
  local src="$T/mut-$(basename "$2")"; sed "$3" "$2" >"$src"
  if cmp -s "$src" "$2"; then bad "mutant '$1': sed matched nothing (test surface moved?)"; return; fi
  local srcs=(); local f; for f in "${SYNC_SRCS[@]}"; do if [ "$f" = "$2" ]; then srcs+=("$src"); else srcs+=("$f"); fi; done
  if ! clang++ -std=c++17 -I"$FORK/cably/src" -I"$FORK/thirdparty/nlohmann_json" -I"$FORK/thirdparty/picosha2" \
        "${srcs[@]}" "$FORK/cably/tests/unit/test_cably_sync.cpp" -o "$T/mut_test" 2>"$T/mut.log"; then
    bad "mutant '$1' did not compile: $(head -3 "$T/mut.log")"; return; fi
  if "$T/mut_test" >"$T/mut.out" 2>&1; then bad "mutant '$1' SURVIVED (tests passed with the rule broken)"
  else ok "mutant '$1' killed: $(grep -m1 'CHECK failed' "$T/mut.out")"; fi
}
mutant "content dedupe removed (our own write becomes a save)" "$FORK/cably/src/cably_sync.cpp" \
  's/if( known != m_known.end() \&\& known->second == hash )/if( false )/'
mutant "updated_at comparison inverted (an older row counts as changed)" "$FORK/cably/src/cably_bridge.cpp" \
  's/return row > expected;/return row < expected;/'
mutant "cloud-changed check removed (always patches)" "$FORK/cably/src/cably_bridge.cpp" \
  's/if( !aExpectedUpdatedAt.empty() \&\& cloudIsNewer( rowUpdatedAt, aExpectedUpdatedAt ) )/if( false )/'
mutant "debounce ignored (fires on the first change)" "$FORK/cably/src/cably_sync.cpp" \
  's/const auto deadline = aNow + std::chrono::milliseconds( m_options.debounceMs );/const auto deadline = aNow;/'

# The keychain and KICAD_CURL_EASY adapters at least compile standalone (they need the
# KiCad/wx include tree to link, which (b) covers).
if clang++ -std=c++17 -Wall -c -I"$FORK/cably/src" -I"$FORK/thirdparty/nlohmann_json" \
      "$FORK/cably/src/cably_bridge_keychain.cpp" -o "$T/keychain.o" 2>"$T/kc.log"; then ok "keychain adapter compiles"
else bad "keychain adapter does not compile:"; sed 's/^/       /' "$T/kc.log" | head -20; fi

# (b) ------------------------------------------------------------------------
CLI=""
if [ -f "$INNER/CMakeCache.txt" ]; then
  if pgrep -f "build.py|/ninja|[c]make --build" >/dev/null; then
    bad "another build is running; refusing to touch $INNER (the Mac freezes on two builds)"
  else
    # A target added since the last configure is unknown to the Makefile until CMake re-runs
    # (a Ninja tree — the Linux build, cably/linux/build.sh — re-runs CMake by itself).
    build_cli(){
      if [ -f "$INNER/build.ninja" ]; then ninja -C "$INNER" -j"$JOBS" cably-bridge-cli
      else make -C "$INNER" cmake_check_build_system && make -C "$INNER" -j"$JOBS" cably-bridge-cli; fi
    }
    if build_cli >"$T/make.log" 2>&1; then
      CLI="$INNER/cably/cably-bridge-cli"
      [ -x "$CLI" ] && ok "cably-bridge-cli built: $CLI" || { bad "make succeeded but $CLI missing"; CLI=""; }
    else bad "cably-bridge-cli did not build:"; grep -E "error|Error" "$T/make.log" | head -20 | sed 's/^/       /'; fi
  fi
else bad "no configured KiCad build at $INNER (set CABLY_INNER_BUILD); skipping CLI checks"; fi

[ -n "$CLI" ] || { [ "$fail" = 0 ] && echo "bridge.sh: PASS" || echo "bridge.sh: FAIL"; exit $fail; }

# (c) ------------------------------------------------------------------------
LOG="$T/requests.jsonl"; : >"$LOG"
python3 "$FORK/cably/tests/mock_cloud.py" --apikey "$APIKEY" --log "$LOG" --port-file "$T/mock.port" >"$T/mock.out" 2>&1 &
MOCK_PID=$!; disown "$MOCK_PID" 2>/dev/null
for i in $(seq 1 50); do [ -s "$T/mock.port" ] && break; sleep 0.1; done
MPORT=$(cat "$T/mock.port" 2>/dev/null)
[ -n "$MPORT" ] && ok "mock cloud on 127.0.0.1:$MPORT" || { bad "mock did not start"; cat "$T/mock.out"; exit 1; }
BASE="http://127.0.0.1:$MPORT"
COMMON=(--supabase-url "$BASE" --engine-url "$BASE" --store memory --refresh-token refresh-1 --email dev@example.com)
nreq(){ wc -l <"$LOG" | tr -d ' '; }
req(){ sed -n "${1}p" "$LOG"; }   # 1-based line of the request log
jq_(){ python3 -c "import json,sys; d=json.loads(sys.stdin.read()); print($1)"; }

# list
if OUT=$("$CLI" "${COMMON[@]}" --token good-token list 2>"$T/err"); then
  echo "$OUT" | grep -q '"id": *"p1"' && echo "$OUT" | grep -q '"Blinking LED"' && ok "list prints the fixture project" || bad "list output wrong: $OUT"
  R=$(req 1)
  [ "$(echo "$R" | jq_ 'd["method"]+" "+d["path"]')" = "GET /rest/v1/projects?select=id,name,updated_at&order=updated_at.desc&limit=50" ] && ok "list: exact PostgREST path" || bad "list path: $R"
  [ "$(echo "$R" | jq_ 'd["apikey"]')" = "$APIKEY" ] && ok "list: apikey header is the publishable key" || bad "list: apikey header wrong"
  [ "$(echo "$R" | jq_ 'd["authorization"]')" = "Bearer good-token" ] && ok "list: Authorization: Bearer <access_token>" || bad "list: bearer wrong"
else bad "list failed: $(cat "$T/err")"; fi

# open -> fetch + export + write
ROOT="$T/root"
if OUT=$("$CLI" "${COMMON[@]}" --token good-token --root "$ROOT" open p1 2>"$T/err"); then
  PCB="$ROOT/Blinking_LED/Blinking_LED.kicad_pcb"; SCH="$ROOT/Blinking_LED/Blinking_LED.kicad_sch"; PRO="$ROOT/Blinking_LED/Blinking_LED.kicad_pro"
  [ -f "$PCB" ] && [ -f "$SCH" ] && [ -f "$PRO" ] && ok "open: three files written under <root>/<safe stem>/" || bad "open: files missing under $ROOT/Blinking_LED: $(ls "$ROOT/Blinking_LED" 2>&1)"
  grep -q '(kicad_pcb (version 20240108) (generator "mock")' "$PCB" 2>/dev/null && ok "open: board is the engine's kicadPcb" || bad "open: board content wrong"
  grep -q '(kicad_sch (version 20231120)' "$SCH" 2>/dev/null && ok "open: schematic is the engine's kicadSch" || bad "open: schematic content wrong"
  python3 - "$PRO" <<'EOF' && ok "open: minimal .kicad_pro {meta:{filename,version:3}}" || bad "open: .kicad_pro wrong"
import json,sys; j=json.load(open(sys.argv[1])); assert j=={"meta":{"filename":"Blinking_LED.kicad_pro","version":3}}, j
EOF
  [ -f "$ROOT/Blinking_LED/.cably-export.json" ] && ok "open: sidecar .cably-export.json written" || bad "open: sidecar missing"
  echo "$OUT" | grep -q "\"pcbPath\": *\"$PCB\"" && ok "open: prints the written paths" || bad "open: output: $OUT"
  F=$(req 2); E=$(req 3)
  [ "$(echo "$F" | jq_ 'd["method"]+" "+d["path"]')" = "GET /rest/v1/projects?id=eq.p1&select=data,updated_at" ] && ok "open: fetch path (F5: with the row's version)" || bad "open: fetch: $F"
  [ "$(echo "$F" | jq_ 'd["apikey"]')" = "$APIKEY" ] && [ "$(echo "$F" | jq_ 'd["authorization"]')" = "Bearer good-token" ] && ok "open: fetch carries apikey + bearer" || bad "open: fetch headers"
  [ "$(echo "$E" | jq_ 'd["method"]+" "+d["path"]')" = "POST /v1/export" ] && ok "open: export path" || bad "open: export: $E"
  [ "$(echo "$E" | jq_ 'd["authorization"]')" = "Bearer good-token" ] && [ "$(echo "$E" | jq_ 'd["apikey"]')" = "" ] && ok "open: export carries bearer only (no apikey)" || bad "open: export headers"
  echo "$E" | jq_ 'd["content_type"]' | grep -q "application/json" && ok "open: export Content-Type json" || bad "open: export content-type"
  echo "$E" | jq_ 'sorted(json.loads(d["body"]).keys())==["apiVersion","project"] and json.loads(d["body"])["apiVersion"]==1' | grep -q True && ok "open: export body is exactly {apiVersion:1, project}" || bad "open: export body"
  [ "$(nreq)" = 3 ] && ok "open: exactly 3 requests (list, fetch, export)" || bad "open: $(nreq) requests"
else bad "open failed: $(cat "$T/err") $OUT"; fi

# never clobber an edited file
if [ -f "${PCB:-}" ]; then
  echo "(kicad_pcb edited-in-kicad)" >"$PCB"
  "$CLI" "${COMMON[@]}" --token good-token --root "$ROOT" open p1 >"$T/out2" 2>&1; rc=$?
  [ $rc = 2 ] && grep -q conflict "$T/out2" && ok "open again: conflict (exit 2) on the edited board" || bad "open again: rc=$rc $(cat "$T/out2")"
  [ "$(cat "$PCB")" = "(kicad_pcb edited-in-kicad)" ] && ok "open again: edited board untouched" || bad "open again: board clobbered"
  "$CLI" "${COMMON[@]}" --token good-token --root "$ROOT" --force open p1 >"$T/out3" 2>&1; rc=$?
  [ $rc = 0 ] && grep -q 'generator "mock"' "$PCB" && ok "open --force: overwrote the edited board" || bad "open --force: rc=$rc"
fi

# unknown project
"$CLI" "${COMMON[@]}" --token good-token --root "$ROOT" open nope >"$T/out4" 2>&1 && bad "open nope: should fail" || ok "open nope: fails ($(head -c 80 "$T/out4"))"

# expired bearer -> refresh -> retry
N0=$(nreq)
if OUT=$("$CLI" "${COMMON[@]}" --token expired-token validate 2>"$T/err"); then
  echo "$OUT" | grep -q "email=dev@example.com" && ok "validate with an expired bearer succeeds after refresh" || bad "validate output: $OUT"
  A=$(req $((N0+1))); B=$(req $((N0+2))); C=$(req $((N0+3)))
  [ "$(echo "$A" | jq_ 'd["method"]+" "+d["path"]')" = "GET /auth/v1/user" ] && [ "$(echo "$A" | jq_ 'd["authorization"]')" = "Bearer expired-token" ] && ok "  1: GET /auth/v1/user (expired)" || bad "  1: $A"
  [ "$(echo "$B" | jq_ 'd["method"]+" "+d["path"]')" = "POST /auth/v1/token?grant_type=refresh_token" ] && [ "$(echo "$B" | jq_ 'd["apikey"]')" = "$APIKEY" ] && [ "$(echo "$B" | jq_ 'json.loads(d["body"])')" = "{'refresh_token': 'refresh-1'}" ] && ok "  2: POST /auth/v1/token?grant_type=refresh_token, apikey, {refresh_token}" || bad "  2: $B"
  [ "$(echo "$C" | jq_ 'd["authorization"]')" = "Bearer good-token" ] && ok "  3: retry with the refreshed bearer" || bad "  3: $C"
else bad "validate (expired) failed: $(cat "$T/err")"; fi

# wrong publishable key is refused by the mock -> the CLI reports failure (proves the guard is live)
"$CLI" "${COMMON[@]}" --token good-token --publishable-key wrong list >/dev/null 2>&1 && bad "wrong apikey accepted" || ok "negative control: the mock refuses a wrong apikey"

# (d) ------------------------------------------------------------------------
"$CLI" --auth-url "https://cably.test/desktop/auth" --store memory --timeout 20 loopback >"$T/loop.out" 2>&1 &
CLI_PID=$!; disown "$CLI_PID" 2>/dev/null
for i in $(seq 1 50); do grep -q '^state=' "$T/loop.out" 2>/dev/null && break; sleep 0.1; done
LPORT=$(sed -n 's/^port=//p' "$T/loop.out"); STATE=$(sed -n 's/^state=//p' "$T/loop.out"); AURL=$(sed -n 's/^auth_url=//p' "$T/loop.out")
[ -n "$LPORT" ] && [ ${#STATE} = 64 ] && ok "loopback: port=$LPORT state=<64 hex>" || bad "loopback: no port/state in $(cat "$T/loop.out")"
[ "$AURL" = "https://cably.test/desktop/auth?port=$LPORT&state=$STATE" ] && ok "loopback: auth_url carries port + state" || bad "loopback: auth_url=$AURL"
if command -v lsof >/dev/null; then
  if lsof -nP -a -p "$CLI_PID" -iTCP -sTCP:LISTEN | grep -q "127.0.0.1:$LPORT"; then ok "loopback: bound to 127.0.0.1 only"; else bad "loopback: not bound to 127.0.0.1: $(lsof -nP -a -p "$CLI_PID" -iTCP -sTCP:LISTEN)"; fi
fi
CB=$(curl -s -m 5 "http://127.0.0.1:$LPORT/callback")
echo "$CB" | grep -q "location.hash" && echo "$CB" | grep -q "'/token'" && ok "loopback: /callback serves the self-POST page" || bad "loopback: /callback: $(echo "$CB" | head -c 200)"
S=$(curl -s -m 5 -o /dev/null -w '%{http_code}' -X POST -H 'content-type: application/json' --data "{\"state\":\"deadbeef\",\"access_token\":\"good-token\",\"refresh_token\":\"refresh-1\",\"expires_at\":\"4102444800\",\"email\":\"dev@example.com\"}" "http://127.0.0.1:$LPORT/token")
[ "$S" = 400 ] && ok "loopback: wrong state -> 400" || bad "loopback: wrong state -> $S"
kill -0 "$CLI_PID" 2>/dev/null && ok "loopback: still waiting after a bad token" || bad "loopback: CLI exited after a bad token"
S=$(curl -s -m 5 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$LPORT/nope")
[ "$S" = 404 ] && ok "loopback: unknown path -> 404" || bad "loopback: unknown path -> $S"
S=$(curl -s -m 5 -o "$T/tok.out" -w '%{http_code}' -X POST -H 'content-type: application/json' --data "{\"state\":\"$STATE\",\"access_token\":\"good-token\",\"refresh_token\":\"refresh-1\",\"expires_at\":\"4102444800\",\"email\":\"dev@example.com\"}" "http://127.0.0.1:$LPORT/token")
[ "$S" = 200 ] && ok "loopback: right state -> 200" || bad "loopback: right state -> $S"
for i in $(seq 1 50); do kill -0 "$CLI_PID" 2>/dev/null || break; sleep 0.1; done
# disowned => wait(1) cannot report the status; the CLI prints "exit=<rc>" itself.
if ! kill -0 "$CLI_PID" 2>/dev/null && grep -q '^exit=0$' "$T/loop.out"; then ok "loopback: CLI exited 0 after the handoff"; else bad "loopback: CLI did not exit 0: $(cat "$T/loop.out")"; fi
CLI_PID=""
grep -q '^email=dev@example.com' "$T/loop.out" && ok "loopback: session email received" || bad "loopback: output $(cat "$T/loop.out")"
S=$(curl -s -m 3 -o /dev/null -w '%{http_code}' -X POST --data '{}' "http://127.0.0.1:$LPORT/token" 2>/dev/null || true)
[ "$S" = 000 ] && ok "loopback: listener closed after the handoff (connection refused)" || bad "loopback: still answering ($S) after the handoff"

# timeout path: nobody comes back -> exit 1 within the timeout
"$CLI" --store memory --timeout 1 loopback >"$T/loop2.out" 2>&1; rc=$?
[ $rc = 1 ] && ok "loopback: times out with exit 1 when nobody signs in" || bad "loopback: timeout rc=$rc"

# (e) ------------------------------------------------------------------------
if [ "$(uname)" = Darwin ]; then
  SVC="dev.cably.desktop.test.$$"
  KC=(--store keychain --keychain-service "$SVC")
  "$CLI" "${KC[@]}" --token kc-token --refresh-token kc-refresh --email kc@example.com --expires-at 4102444800 save >"$T/kc1" 2>&1; rc=$?
  if [ $rc = 0 ]; then
    OUT=$("$CLI" "${KC[@]}" show 2>&1)
    echo "$OUT" | grep -q "email=kc@example.com" && echo "$OUT" | grep -q "access_token_len=8" && echo "$OUT" | grep -q "expires_at=4102444800" && ok "keychain: save -> show round-trips" || bad "keychain: show: $OUT"
    security find-generic-password -s "$SVC" >/dev/null 2>&1 && ok "keychain: item exists under service $SVC" || bad "keychain: item not found by security(1)"
    "$CLI" "${KC[@]}" signout >/dev/null 2>&1 && ok "keychain: signout" || bad "keychain: signout failed"
    "$CLI" "${KC[@]}" show >/dev/null 2>&1 && bad "keychain: item survived signout" || ok "keychain: item gone after signout"
    security delete-generic-password -s "$SVC" >/dev/null 2>&1 || true
  else
    grep -q "errSecInteractionNotAllowed\|-25308" "$T/kc1" && echo "  skip keychain (locked / no UI session): $(cat "$T/kc1")" || bad "keychain: save failed: $(cat "$T/kc1")"
  fi
else
  # Linux: the same store is libsecret when a Secret Service answers on the session bus,
  # else the 0600 file under $XDG_CONFIG_HOME/cably-desktop (cably_bridge_keychain.cpp).
  # Pointing XDG_CONFIG_HOME at a scratch dir keeps the test off the real config.  Three
  # bus conditions, each of which must round-trip save -> show -> signout: the machine's
  # own (a Secret Service when one answers, else the file), a session bus address nobody
  # listens on, and - GitHub's ubuntu runners' condition, recreated with dbus-run-session -
  # a real session bus with no Secret Service on it ("The name org.freedesktop.secrets was
  # not provided by any .service files").  The last two MUST use the file, say why in
  # store_note=, and finish well inside the probe timeout (the CLI must never wait on a
  # bus that has nothing to say).
  linux_secret_case(){ # $1 label  $2 want: any|file  $3 store_note must contain ("" = don't care)  $4.. command prefix
    local label="$1" want="$2" notere="$3"; shift 3
    local svc="dev.cably.desktop.test.$$.$RANDOM"
    local kc=(--store keychain --keychain-service "$svc")
    local xdg="$T/xdg-$RANDOM"
    local file="$xdg/cably-desktop/session.$svc.json"
    local t0 ms rc backend note out
    t0=$(date +%s%N)
    XDG_CONFIG_HOME="$xdg" "$@" "$CLI" "${kc[@]}" --token kc-token --refresh-token kc-refresh --email kc@example.com --expires-at 4102444800 save >"$T/kc-save" 2>&1; rc=$?
    ms=$(( ( $(date +%s%N) - t0 ) / 1000000 ))
    if [ $rc != 0 ]; then bad "$label: save failed on $(uname): $(cat "$T/kc-save")"; return; fi
    backend=$(sed -n 's/^store_backend=//p' "$T/kc-save"); note=$(sed -n 's/^store_note=//p' "$T/kc-save")
    case "$want:$backend" in
      any:secret-service) ok "$label: Secret Service reachable, item stored there (store_backend=$backend)" ;;
      *:"file:$file")
        ok "$label: fallback file used (store_backend=$backend)"
        [ "$(stat -c %a "$file")" = 600 ] && ok "$label: session file mode 0600" || bad "$label: session file mode $(stat -c %a "$file") (want 600)"
        [ "$(stat -c %a "$xdg/cably-desktop")" = 700 ] && ok "$label: cably-desktop dir mode 0700" || bad "$label: dir mode $(stat -c %a "$xdg/cably-desktop") (want 700)"
        grep -q "kc-token" "$file" && ok "$label: the file holds the session (tokens never printed by the CLI)" || bad "$label: session file does not hold the token" ;;
      *) bad "$label: unexpected store_backend='$backend' (want $([ "$want" = any ] && echo "secret-service or ")file:$file)" ;;
    esac
    if [ -n "$notere" ]; then
      echo "$note" | grep -qF -- "$notere" && ok "$label: store_note says why: $note" || bad "$label: store_note='$note' (want it to mention '$notere')"
    fi
    [ "$ms" -lt 4000 ] && ok "$label: save took $ms ms (the probe fails fast)" || bad "$label: save took $ms ms (the probe must not wait on a bus with nothing to say)"
    out=$(XDG_CONFIG_HOME="$xdg" "$@" "$CLI" "${kc[@]}" show 2>&1)
    echo "$out" | grep -q "email=kc@example.com" && echo "$out" | grep -q "access_token_len=8" && echo "$out" | grep -q "expires_at=4102444800" && ok "$label: save -> show round-trips" || bad "$label: show: $out"
    echo "$out" | grep -q "^store_backend=$backend$" && ok "$label: show read the same backend" || bad "$label: show backend differs: $(echo "$out" | grep store_backend)"
    XDG_CONFIG_HOME="$xdg" "$@" "$CLI" "${kc[@]}" signout >/dev/null 2>&1 && ok "$label: signout" || bad "$label: signout failed"
    XDG_CONFIG_HOME="$xdg" "$@" "$CLI" "${kc[@]}" show >/dev/null 2>&1 && bad "$label: session survived signout" || ok "$label: session gone after signout"
    [ -e "$file" ] && bad "$label: $file still exists after signout" || ok "$label: no session file left after signout"
  }
  linux_secret_case "secret store" any "" env
  linux_secret_case "secret store (bus nobody listens on)" file "unavailable" env DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent/cably-bus
  if command -v dbus-run-session >/dev/null; then
    linux_secret_case "secret store (bus without a Secret Service)" file "org.freedesktop.secrets" dbus-run-session --
  else bad "dbus-run-session not installed (apt install dbus-daemon): the bus-without-a-Secret-Service case, GitHub's runner condition, cannot run"; fi
fi

# (f) ------------------------------------------------------------------------
# F5 sync against the mock with the REAL watcher (cably-bridge-cli watch).
nowms(){ python3 -c 'import time; print(int(time.time()*1000))'; }
sha(){ python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$1"; }
mockrow(){ curl -s -m 5 "$BASE/__mock/row"; }
scfield(){ python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$SC5" "$1"; }
count_since(){ tail -n +"$(( $1 + 1 ))" "$LOG" | grep -c "\"method\": \"$2\", \"path\": \"$3" || true; }
wait_line(){ # $1 file, $2 regex, $3 timeout seconds -> 0 when seen
  local i; for i in $(seq 1 $(( $3 * 20 ))); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.05; done; return 1; }
start_watch(){ # $1 out file, rest: extra args
  local out="$1"; shift
  "$CLI" "${COMMON[@]}" --token good-token watch "$DIR5" --debounce 300 --poll 50 --watch-timeout 20 "$@" >"$out" 2>&1 &
  WATCH_PID=$!; disown "$WATCH_PID" 2>/dev/null
  wait_line "$out" '^watching=' 5 || { bad "watch did not start: $(cat "$out")"; return 1; }
}
end_watch(){ local i; for i in $(seq 1 100); do kill -0 "$WATCH_PID" 2>/dev/null || break; sleep 0.1; done; kill "$WATCH_PID" 2>/dev/null; WATCH_PID=""; }
import_body(){ # $1 request line -> the import body's keys + kicadPcb/kicadSch, tab-separated
  python3 -c 'import json,sys; d=json.loads(sys.argv[1]); b=json.loads(d["body"]); print(",".join(sorted(b.keys())), b.get("apiVersion"), json.dumps(b.get("kicadPcb")), json.dumps(b.get("kicadSch")), sep="\t")' "$1"; }

ROOT5="$T/root5"
if OUT=$("$CLI" "${COMMON[@]}" --token good-token --root "$ROOT5" open p1 2>"$T/err5"); then
  DIR5="$ROOT5/Blinking_LED"; PCB5="$DIR5/Blinking_LED.kicad_pcb"; SCH5="$DIR5/Blinking_LED.kicad_sch"; SC5="$DIR5/.cably-export.json"
  ROWV=$(mockrow | jq_ 'd["updated_at"]')
  [ "$(scfield projectId)" = p1 ] && [ "$(scfield projectName)" = "Blinking LED" ] && [ "$(scfield engineVersion)" = mock-1 ] && [ "$(scfield cloudUpdatedAt)" = "$ROWV" ] && [ "$(scfield engine)" = cably ] \
    && ok "F5 open: sidecar records projectId, projectName, cloudUpdatedAt (= the row's), engineVersion" || bad "F5 open: sidecar wrong: $(cat "$SC5")"

  # -- in-place save of the board -> one import + one PATCH ------------------------------
  N0=$(nreq)
  if start_watch "$T/w1.out"; then
    sleep 0.3
    T0=$(nowms); printf '(kicad_pcb (version 20240108) (generator "kicad-user")\n  (segment (start 0 0) (end 1 1) (net 1))\n)\n' >"$PCB5"
    if wait_line "$T/w1.out" '^event ' 15; then
      T1=$(nowms); end_watch
      EV=$(grep '^event ' "$T/w1.out" | head -1)
      echo "  info F5 in-place save -> synced latency: $(( T1 - T0 )) ms (debounce 300 ms, poll 50 ms, incl. 4 HTTP calls)"
      echo "$EV" | grep -q 'kind=board ' && echo "$EV" | grep -q 'outcome=synced ' && ok "F5 in-place: event kind=board outcome=synced" || bad "F5 in-place: $EV"
      [ "$(count_since $N0 POST /v1/import)" = 1 ] && [ "$(count_since $N0 PATCH /rest/v1/projects)" = 1 ] && ok "F5 in-place: exactly one POST /v1/import and one PATCH" || bad "F5 in-place: imports=$(count_since $N0 POST /v1/import) patches=$(count_since $N0 PATCH /rest/v1/projects)"
      G=$(req $((N0+1))); I=$(req $((N0+2))); P=$(req $((N0+3))); V=$(req $((N0+4)))
      [ "$(echo "$G" | jq_ 'd["method"]+" "+d["path"]')" = "GET /rest/v1/projects?id=eq.p1&select=data,updated_at" ] && [ "$(echo "$G" | jq_ 'd["apikey"]')" = "$APIKEY" ] && [ "$(echo "$G" | jq_ 'd["authorization"]')" = "Bearer good-token" ] && ok "  1: GET the row with its version (apikey + bearer)" || bad "  1: $G"
      [ "$(echo "$I" | jq_ 'd["method"]+" "+d["path"]')" = "POST /v1/import" ] && [ "$(echo "$I" | jq_ 'd["authorization"]')" = "Bearer good-token" ] && [ "$(echo "$I" | jq_ 'd["apikey"]')" = "" ] && ok "  2: POST /v1/import with the bearer only" || bad "  2: $I"
      IB=$(import_body "$I")
      [ "$IB" = "$(printf 'apiVersion,kicadPcb,project\t1\t%s\tnull' "$(python3 -c 'import json,sys; print(json.dumps(open(sys.argv[1]).read()))' "$PCB5")")" ] \
        && ok "  2: body is exactly {apiVersion:1, project, kicadPcb:<the saved file>} (no kicadSch)" || bad "  2: body: $IB"
      [ "$(echo "$P" | jq_ 'd["method"]+" "+d["path"]')" = "PATCH /rest/v1/projects?id=eq.p1" ] && [ "$(echo "$P" | jq_ 'd["apikey"]')" = "$APIKEY" ] && [ "$(echo "$P" | jq_ 'd["authorization"]')" = "Bearer good-token" ] && ok "  3: PATCH /rest/v1/projects?id=eq.p1 (apikey + bearer)" || bad "  3: $P"
      [ "$(echo "$P" | jq_ 'd["prefer"]')" = "return=minimal" ] && echo "$P" | jq_ 'd["content_type"]' | grep -q "application/json" && ok "  3: Prefer: return=minimal, Content-Type json" || bad "  3: headers: $(echo "$P" | head -c 300)"
      mockrow >"$T/row1.json"
      if python3 -c '
import json,sys
d=json.loads(sys.argv[1]); b=json.loads(d["body"]); row=json.load(open(sys.argv[2]))
assert list(b.keys())==["data"], list(b.keys())
data=b["data"]
assert data["schema"]=="cably-project-session-v2", data.get("schema")
assert data["chat"]==[{"id":"m1","role":"user","content":"make a blinky","createdAt":1}], data["chat"]
assert data["project"]["pcb"]["importedFrom"]=="kicad" and data["project"]["project_name"]=="Blinking LED", data["project"]
assert row["data"]==data, "mock row != PATCH body"
' "$P" "$T/row1.json" 2>"$T/p.err"; then ok "  3: body {data} = fetched data with ONLY .project replaced (schema + chat kept, project = the engine's)"; else bad "  3: PATCH body wrong: $(tail -1 "$T/p.err")"; fi
      [ "$(echo "$V" | jq_ 'd["method"]+" "+d["path"]')" = "GET /rest/v1/projects?id=eq.p1&select=updated_at" ] && ok "  4: GET the trigger-stamped updated_at" || bad "  4: $V"
      NEWV=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["updated_at"])' "$T/row1.json")
      [ "$NEWV" != "$ROWV" ] && grep -q "updated_at=$NEWV " "$T/w1.out" && ok "F5 in-place: the new version ($NEWV) is reported" || bad "F5 in-place: version old=$ROWV new=$NEWV out=$(grep '^event' "$T/w1.out")"
      [ "$(scfield cloudUpdatedAt)" = "$NEWV" ] && [ "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["files"]["Blinking_LED.kicad_pcb"])' "$SC5")" = "$(sha "$PCB5")" ] && [ "$(scfield projectId)" = p1 ] \
        && ok "F5 in-place: sidecar now carries the new cloudUpdatedAt and the saved board's sha256" || bad "F5 in-place: sidecar not updated: $(cat "$SC5")"
    else bad "F5 in-place: no event within 15 s: $(cat "$T/w1.out")"; end_watch; fi
  fi

  # -- atomic save (write temp, rename over) of the schematic ----------------------------
  N1=$(nreq)
  if start_watch "$T/w2.out"; then
    sleep 0.3
    T0=$(nowms); printf '(kicad_sch (version 20231120) (generator "kicad-user")\n  (symbol (lib_id "Device:R"))\n)\n' >"$DIR5/Blinking_LED.kicad_sch.tmp-save"; mv "$DIR5/Blinking_LED.kicad_sch.tmp-save" "$SCH5"
    if wait_line "$T/w2.out" '^event ' 15; then
      T1=$(nowms); end_watch
      echo "  info F5 rename-over save -> synced latency: $(( T1 - T0 )) ms"
      EV=$(grep '^event ' "$T/w2.out" | head -1)
      echo "$EV" | grep -q 'kind=schematic ' && echo "$EV" | grep -q 'outcome=synced ' && ok "F5 rename-over: event kind=schematic outcome=synced" || bad "F5 rename-over: $EV"
      [ "$(count_since $N1 POST /v1/import)" = 1 ] && [ "$(count_since $N1 PATCH /rest/v1/projects)" = 1 ] && ok "F5 rename-over: exactly one POST /v1/import and one PATCH" || bad "F5 rename-over: imports=$(count_since $N1 POST /v1/import) patches=$(count_since $N1 PATCH /rest/v1/projects)"
      IB=$(import_body "$(req $((N1+2)))")
      [ "$IB" = "$(printf 'apiVersion,kicadSch,project\t1\tnull\t%s' "$(python3 -c 'import json,sys; print(json.dumps(open(sys.argv[1]).read()))' "$SCH5")")" ] \
        && ok "F5 rename-over: import body carries kicadSch only (the renamed-in text)" || bad "F5 rename-over: import body: $IB"
      [ "$(sha "$SCH5")" = "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["files"]["Blinking_LED.kicad_sch"])' "$SC5")" ] && ok "F5 rename-over: sidecar baseline updated for the schematic" || bad "F5 rename-over: sidecar sch hash stale"
    else bad "F5 rename-over: no event within 15 s: $(cat "$T/w2.out")"; end_watch; fi
  fi

  # -- backups, locks, autosaves -> nothing ---------------------------------------------
  N2=$(nreq)
  "$CLI" "${COMMON[@]}" --token good-token watch "$DIR5" --debounce 200 --poll 50 --watch-timeout 3 >"$T/w3.out" 2>&1 &
  WATCH_PID=$!; disown "$WATCH_PID" 2>/dev/null
  if wait_line "$T/w3.out" '^watching=' 5; then
    sleep 0.2
    echo "old" >"$DIR5/Blinking_LED.kicad_pcb-bak"; echo "{}" >"$DIR5/~Blinking_LED.kicad_pcb.lck"; echo "(kicad_pcb auto)" >"$DIR5/_autosave-Blinking_LED.kicad_pcb"; echo "(kicad_pcb saved)" >"$DIR5/_saved_Blinking_LED.kicad_pcb"
    mkdir -p "$DIR5/Blinking_LED-backups"; echo "(kicad_pcb backup)" >"$DIR5/Blinking_LED-backups/Blinking_LED.kicad_pcb"
    end_watch
    grep -q '^events=0$' "$T/w3.out" && grep -q '^exit=3$' "$T/w3.out" && ok "F5 backups/locks/autosaves: no event (watch timed out, exit 3)" || bad "F5 backups: $(cat "$T/w3.out")"
    [ "$(nreq)" = "$N2" ] && ok "F5 backups/locks/autosaves: no HTTP call at all" || bad "F5 backups: $(( $(nreq) - N2 )) requests"
  else bad "F5 backups: watch did not start: $(cat "$T/w3.out")"; end_watch; fi

  # -- our own hand-off write (Expect) is not a save; the next real one is --------------
  N3=$(nreq)
  if start_watch "$T/w4.out" --once --self-write "$PCB5"; then
    wait_line "$T/w4.out" '^self-write=done' 5 || bad "F5 self-write: not performed"
    sleep 1.2
    if grep -q '^event ' "$T/w4.out"; then bad "F5 self-write: our own write came back as a save: $(grep '^event' "$T/w4.out")"; else ok "F5 self-write: Expect()ed hand-off write produced no event (1.2 s)"; fi
    [ "$(nreq)" = "$N3" ] && ok "F5 self-write: no HTTP call" || bad "F5 self-write: $(( $(nreq) - N3 )) requests"
    printf '(kicad_pcb (version 20240108) (generator "kicad-user") edited-after-handoff)\n' >"$PCB5"
    if wait_line "$T/w4.out" '^event ' 15; then
      end_watch
      IB=$(import_body "$(req $((N3+2)))")
      case "$IB" in *edited-after-handoff*) case "$IB" in *written-by-cably*) bad "F5 self-write: import carried our own text: $IB";; *) ok "F5 self-write: the next real save is heard and carries the user's text";; esac;; *) bad "F5 self-write: import body: $IB";; esac
      [ "$(count_since $N3 PATCH /rest/v1/projects)" = 1 ] && ok "F5 self-write: exactly one PATCH" || bad "F5 self-write: patches=$(count_since $N3 PATCH /rest/v1/projects)"
    else bad "F5 self-write: real save not heard: $(cat "$T/w4.out")"; end_watch; fi
  fi

  # -- the cloud changed since we exported -> cloud-changed, NO PATCH --------------------
  SCV_BEFORE=$(scfield cloudUpdatedAt)
  TOUCHED=$(curl -s -m 5 -X POST "$BASE/__mock/touch" | jq_ 'd["updated_at"]')
  [ "$TOUCHED" \> "$SCV_BEFORE" ] && ok "F5 cloud-changed: the row was edited elsewhere ($TOUCHED > sidecar $SCV_BEFORE)" || bad "F5 cloud-changed: touch did not bump: $TOUCHED vs $SCV_BEFORE"
  N4=$(nreq)
  if start_watch "$T/w5.out" --once; then
    sleep 0.3
    printf '(kicad_pcb (version 20240108) (generator "kicad-user") edited-while-cloud-moved)\n' >"$PCB5"
    if wait_line "$T/w5.out" '^event ' 15; then
      end_watch
      EV=$(grep '^event ' "$T/w5.out" | head -1)
      echo "$EV" | grep -q 'outcome=cloud-changed ' && echo "$EV" | grep -q "updated_at=$TOUCHED " && ok "F5 cloud-changed: event outcome=cloud-changed with the row's version" || bad "F5 cloud-changed: $EV"
      [ "$(count_since $N4 PATCH /rest/v1/projects)" = 0 ] && [ "$(count_since $N4 POST /v1/import)" = 0 ] && ok "F5 cloud-changed: NO PATCH and no import" || bad "F5 cloud-changed: patches=$(count_since $N4 PATCH /rest/v1/projects) imports=$(count_since $N4 POST /v1/import)"
      [ "$(( $(nreq) - N4 ))" = 1 ] && [ "$(req $((N4+1)) | jq_ 'd["method"]+" "+d["path"]')" = "GET /rest/v1/projects?id=eq.p1&select=data,updated_at" ] && ok "F5 cloud-changed: exactly one request (the GET)" || bad "F5 cloud-changed: $(( $(nreq) - N4 )) requests"
      [ "$(scfield cloudUpdatedAt)" = "$SCV_BEFORE" ] && ok "F5 cloud-changed: sidecar untouched" || bad "F5 cloud-changed: sidecar changed"
      if mockrow | python3 -c "import json,sys; r=json.load(sys.stdin); assert 'edited-while-cloud-moved' not in json.dumps(r) and r['data']['project']['pcb'].get('importedFrom')=='kicad'" 2>/dev/null; then ok "F5 cloud-changed: the cloud row still holds the earlier sync, not this save"; else bad "F5 cloud-changed: row clobbered"; fi
    else bad "F5 cloud-changed: no event: $(cat "$T/w5.out")"; end_watch; fi
  fi
else bad "F5 open failed: $(cat "$T/err5") $OUT"; fi

[ "$fail" = 0 ] && echo "bridge.sh: PASS" || echo "bridge.sh: FAIL"
exit $fail
