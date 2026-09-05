#!/usr/bin/env bash
# Notarize a signed Cably Desktop deliverable with Apple and staple the ticket
# (F6). Replaces kicad-mac-builder's dead `apple.py notarize` (altool era; the
# subcommand no longer exists) with notarytool and a stored keychain profile,
# so no Apple ID password is ever typed, printed or passed around.
#
#   notarize.sh <KiCad.app | file.dmg | file.zip | file.pkg> [--no-staple]
#
# Environment:
#   CABLY_NOTARY_PROFILE   notarytool keychain profile (default cably-app), created
#                          once, interactively, with `xcrun notarytool store-credentials`
#   CABLY_NOTARY_KEYCHAIN  optional keychain file holding that profile (--keychain)
#   CABLY_NOTARY_POLL      seconds between status polls (default 120)
#   CABLY_NOTARY_TIMEOUT   give up after this many seconds (default 5400)
# An .app is zipped with `ditto -c -k --keepParent` into a temporary archive
# (what Apple's notarization service accepts; the zip is deleted afterwards)
# and the ticket is stapled to the .app itself - Apple's recommendation for an
# app shipped inside a DMG, so a copy dragged out of the image opens offline.
# Any other file is submitted as it is and stapled in place.
# Submission is asynchronous on purpose (no `notarytool submit --wait`): the
# submission id is printed as soon as the upload finishes and `notarytool info`
# is polled every $CABLY_NOTARY_POLL seconds; on "Invalid" the service's log
# (`notarytool log`) is printed and the script fails. Exit 0 only when the
# status is Accepted and (unless --no-staple) `stapler validate` passes.
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
TARGET="${1:-}"; STAPLE=1; [ "${2:-}" = "--no-staple" ] && STAPLE=0
PROFILE="${CABLY_NOTARY_PROFILE:-cably-app}"
POLL="${CABLY_NOTARY_POLL:-120}"; TIMEOUT="${CABLY_NOTARY_TIMEOUT:-5400}"
[ -n "$TARGET" ] && [ -e "$TARGET" ] || { echo "notarize.sh: usage: notarize.sh <App.app|file.dmg|file.zip|file.pkg> [--no-staple]"; exit 2; }
TARGET="$(cd "$(dirname "$TARGET")" && pwd)/$(basename "${TARGET%/}")"
NT=(xcrun notarytool); KC=(--keychain-profile "$PROFILE")
[ -n "${CABLY_NOTARY_KEYCHAIN:-}" ] && KC+=(--keychain "$CABLY_NOTARY_KEYCHAIN")
t0=$(date +%s); step(){ echo "notarize.sh: [$(( $(date +%s) - t0 ))s] $*"; }

# the profile must exist before we spend minutes uploading
if ! "${NT[@]}" history "${KC[@]}" >/dev/null 2>&1; then
  echo "notarize.sh: notarytool profile '$PROFILE' is not usable (create it interactively with"
  echo "  xcrun notarytool store-credentials $PROFILE   - this script never handles Apple passwords)"; exit 2
fi

UPLOAD="$TARGET"; TMPZIP=""
case "$TARGET" in
*.app)
  TMPZIP="$(mktemp -d "${TMPDIR:-/tmp}/cably-notarize.XXXXXX")/$(basename "${TARGET%.app}").zip"
  step "zipping $(basename "$TARGET") for upload"
  ditto -c -k --keepParent "$TARGET" "$TMPZIP"
  UPLOAD="$TMPZIP"; step "zip ready: $(stat -f %z "$TMPZIP") bytes"
  ;;
esac
# (an if, not `[ -n ] &&`: with no zip that list returns 1 and, run from the EXIT trap
# under set -e, turned a successful DMG run into exit status 1 - measured 2026-09-05)
cleanup(){ if [ -n "$TMPZIP" ]; then rm -rf "$(dirname "$TMPZIP")"; fi; }; trap cleanup EXIT

step "submitting $(basename "$UPLOAD") ($(stat -f %z "$UPLOAD") bytes) with profile '$PROFILE' (no --wait)"
out="$("${NT[@]}" submit "$UPLOAD" "${KC[@]}" 2>&1)" || { echo "$out"; echo "notarize.sh: submit failed"; exit 1; }
echo "$out" | sed 's/^/  /'
SUBMISSION_ID="$(echo "$out" | sed -n 's/^ *id: *//p' | head -1)"
[ -n "$SUBMISSION_ID" ] || { echo "notarize.sh: no submission id in notarytool output"; exit 1; }
step "submission id $SUBMISSION_ID (upload done); polling every ${POLL}s"

status=""
while :; do
  info="$("${NT[@]}" info "$SUBMISSION_ID" "${KC[@]}" 2>&1 || true)"
  status="$(echo "$info" | sed -n 's/^ *status: *//p' | head -1)"
  step "status: ${status:-unknown}"
  case "$status" in
    Accepted|Invalid|Rejected) break;;
  esac
  if [ $(( $(date +%s) - t0 )) -ge "$TIMEOUT" ]; then
    echo "notarize.sh: gave up after ${TIMEOUT}s; submission $SUBMISSION_ID still '$status' - check later with:"
    echo "  xcrun notarytool info $SUBMISSION_ID --keychain-profile $PROFILE"; exit 1
  fi
  sleep "$POLL"
done
if [ "$status" != "Accepted" ]; then
  echo "notarize.sh: notarization $status for $SUBMISSION_ID - service log:"
  "${NT[@]}" log "$SUBMISSION_ID" "${KC[@]}" 2>&1 | sed 's/^/  /' || true
  exit 1
fi
step "Accepted"

if [ "$STAPLE" = 1 ]; then
  xcrun stapler staple "$TARGET" | sed 's/^/  /'
  v="$(xcrun stapler validate "$TARGET" 2>&1 | sed -n '$p')"
  case "$v" in *"The validate action worked"*) step "stapled and validated: $TARGET";; *) echo "notarize.sh: stapler validate: $v"; exit 1;; esac
else
  step "not stapled (--no-staple)"
fi
echo "notarize.sh: done - submission $SUBMISSION_ID Accepted for $TARGET"
