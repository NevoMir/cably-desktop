#!/usr/bin/env bash
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
set -uo pipefail
FORK="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${CABLY_BUILD_DIR:-$FORK/../build.noindex}"
INNER="${CABLY_INNER_BUILD:-$BUILD/kicad/src/kicad-build}"
JOBS="${CABLY_JOBS:-6}"
fail=0; ok(){ echo "  ok   $1"; }; bad(){ echo "  FAIL $1"; fail=1; }

T=$(mktemp -d)
MOCK_PID=""; CLI_PID=""
cleanup(){ [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null; [ -n "$CLI_PID" ] && kill "$CLI_PID" 2>/dev/null; rm -rf "$T"; }
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
    # A target added since the last configure is unknown to the Makefile until CMake re-runs.
    if make -C "$INNER" cmake_check_build_system >"$T/make.log" 2>&1 && make -C "$INNER" -j"$JOBS" cably-bridge-cli >>"$T/make.log" 2>&1; then
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
  [ "$(echo "$F" | jq_ 'd["method"]+" "+d["path"]')" = "GET /rest/v1/projects?id=eq.p1&select=data" ] && ok "open: fetch path" || bad "open: fetch: $F"
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
    echo "$OUT" | grep -q "email=kc@example.com" && echo "$OUT" | grep -q "access_token=kc-token" && echo "$OUT" | grep -q "expires_at=4102444800" && ok "keychain: save -> show round-trips" || bad "keychain: show: $OUT"
    security find-generic-password -s "$SVC" >/dev/null 2>&1 && ok "keychain: item exists under service $SVC" || bad "keychain: item not found by security(1)"
    "$CLI" "${KC[@]}" signout >/dev/null 2>&1 && ok "keychain: signout" || bad "keychain: signout failed"
    "$CLI" "${KC[@]}" show >/dev/null 2>&1 && bad "keychain: item survived signout" || ok "keychain: item gone after signout"
    security delete-generic-password -s "$SVC" >/dev/null 2>&1 || true
  else
    grep -q "errSecInteractionNotAllowed\|-25308" "$T/kc1" && echo "  skip keychain (locked / no UI session): $(cat "$T/kc1")" || bad "keychain: save failed: $(cat "$T/kc1")"
  fi
fi

[ "$fail" = 0 ] && echo "bridge.sh: PASS" || echo "bridge.sh: FAIL"
exit $fail
