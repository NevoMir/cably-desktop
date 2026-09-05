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
# The .deb version parser: SOURCED by cably/linux/package-deb.sh (the package Version)
# and cably/tests/deb.sh (what the packaged kicad-cli reports against that Version);
# tested by cably/tests/deb-version.sh.  Run directly it parses its arguments and
# prints the fields (a debugging aid).
#
# The record is KiCad's `git describe --dirty` as the BUILD tree stored it (KICAD_VERSION
# in kicad_build_version.h, what `kicad-cli version` prints).  The package Version is
# <kicad>+cably.<yyyymmdd>.<COUNT>.g<rev>[.dirty] with COUNT = commits since the KiCad
# tag WHATEVER tag git describe found - since the first release tag (v10.0.6-cably.26,
# on the 26th commit after 10.0.6) describe names that tag instead of KiCad's, and the
# count must not restart at 0 there or dpkg would see a downgrade.  The forms:
#
#   (1) 10.0.6-<N>-g<rev>[-dirty]             the KiCad tag is nearest:  COUNT = N
#   (2) v10.0.6-cably.<M>-<N>-g<rev>[-dirty]  a release tag is nearest:  COUNT = M + N
#       (M = the release tag's own count, N = commits after it: v10.0.6-cably.26-2-g.. -> 28)
#   (3) v10.0.6-cably.<M>[-dirty]             exactly ON a release tag:  COUNT = M
#       git describe writes no -<N>-g<rev> on a tagged commit, so the rev comes from
#       the record's own KICAD_COMMIT_HASH when the caller passes it, else from the tag
#       resolved in the checkout, else from the checkout's HEAD (DV_REV_FROM says which)
#   (4) 10.0.6[-dirty]                        exactly on the KiCad tag:  COUNT = 0, rev as (3)
#
# cably_deb_parse_record <record> [<checkout>] [<commit hash>]
#   sets DV_UPSTREAM (10.0.6)  DV_COUNT  DV_REV (10 hex chars; "" when nothing resolves it)
#        DV_DIRTY ("dirty" or "")  DV_FORM (one of the four above)  DV_REV_FROM (prose)
#   returns 1, all six cleared, for any other string.
# cably_deb_version <yyyymmdd>
#   prints <upstream>+cably.<yyyymmdd>.<count>.g<rev>[.dirty] from the last parse;
#   returns 1 without printing when there is no rev (or no parse, or no 8-digit day).
#
# Safe under `set -euo pipefail`: every test is an if, nothing exits.

cably_deb_parse_record() {
  local rec="${1:-}" src="${2:-}" hash="${3:-}" tag=""
  local ver='[0-9]+(\.[0-9]+)*'
  DV_UPSTREAM=""; DV_COUNT=""; DV_REV=""; DV_DIRTY=""; DV_FORM=""; DV_REV_FROM=""
  if [[ "$rec" =~ ^($ver)-([0-9]+)-g([0-9a-f]+)(-dirty)?$ ]]; then
    DV_FORM="<kicad>-<N>-g<rev>"; DV_UPSTREAM=${BASH_REMATCH[1]}; DV_COUNT=$(( 10#${BASH_REMATCH[3]} ))
    DV_REV=${BASH_REMATCH[4]}; DV_DIRTY=${BASH_REMATCH[5]:+dirty}; DV_REV_FROM="the record"
    return 0
  elif [[ "$rec" =~ ^v($ver)-cably\.([0-9]+)-([0-9]+)-g([0-9a-f]+)(-dirty)?$ ]]; then
    DV_FORM="v<kicad>-cably.<M>-<N>-g<rev>"; DV_UPSTREAM=${BASH_REMATCH[1]}
    DV_COUNT=$(( 10#${BASH_REMATCH[3]} + 10#${BASH_REMATCH[4]} ))
    DV_REV=${BASH_REMATCH[5]}; DV_DIRTY=${BASH_REMATCH[6]:+dirty}; DV_REV_FROM="the record"
    return 0
  elif [[ "$rec" =~ ^v($ver)-cably\.([0-9]+)(-dirty)?$ ]]; then
    DV_FORM="v<kicad>-cably.<M>"; DV_UPSTREAM=${BASH_REMATCH[1]}; DV_COUNT=$(( 10#${BASH_REMATCH[3]} ))
    DV_DIRTY=${BASH_REMATCH[4]:+dirty}; tag="v$DV_UPSTREAM-cably.${BASH_REMATCH[3]}"
  elif [[ "$rec" =~ ^($ver)(-dirty)?$ ]]; then
    DV_FORM="<kicad>"; DV_UPSTREAM=${BASH_REMATCH[1]}; DV_COUNT=0
    DV_DIRTY=${BASH_REMATCH[3]:+dirty}; tag="$DV_UPSTREAM"
  else
    return 1
  fi
  # (3)/(4): a tag exactly, no rev in the record.
  if [[ "$hash" =~ ^[0-9a-f]{10,40}$ ]]; then
    DV_REV=${hash:0:10}; DV_REV_FROM="the record's commit hash"
  elif [ -n "$src" ] && DV_REV=$(git -C "$src" rev-parse --short=10 --verify -q "refs/tags/$tag^{commit}" 2>/dev/null) && [ -n "$DV_REV" ]; then
    DV_REV_FROM="git: tag $tag in $src"
  elif [ -n "$src" ] && DV_REV=$(git -C "$src" rev-parse --short=10 --verify -q HEAD 2>/dev/null) && [ -n "$DV_REV" ]; then
    DV_REV_FROM="git: HEAD of $src (tag $tag is not in that checkout)"
  else
    DV_REV=""; DV_REV_FROM="unavailable: the record names tag $tag without a rev, and no commit hash or git checkout resolves it"
  fi
  return 0
}

cably_deb_version() {
  local day="${1:-}"
  [ -n "${DV_UPSTREAM:-}" ] && [ -n "${DV_COUNT:-}" ] && [ -n "${DV_REV:-}" ] && [[ "$day" =~ ^[0-9]{8}$ ]] || return 1
  printf '%s+cably.%s.%s.g%s%s\n' "$DV_UPSTREAM" "$day" "$DV_COUNT" "$DV_REV" "${DV_DIRTY:+.dirty}"
}

# Run, not sourced: parse the arguments and show the fields.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  if cably_deb_parse_record "${1:-}" "${2:-}" "${3:-}"; then
    printf 'form=%s\nupstream=%s\ncount=%s\nrev=%s\ndirty=%s\nrev_from=%s\nversion=%s\n' \
      "$DV_FORM" "$DV_UPSTREAM" "$DV_COUNT" "$DV_REV" "${DV_DIRTY:-no}" "$DV_REV_FROM" "$(cably_deb_version "$(date -u +%Y%m%d)" || echo '(no rev)')"
  else
    echo "deb-version.sh: '${1:-}' is not a record this parser understands (10.0.6-<N>-g<rev>, v10.0.6-cably.<M>[-<N>-g<rev>] or 10.0.6, each [-dirty])" >&2
    exit 1
  fi
fi
