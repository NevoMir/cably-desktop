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
# F3(a) tests for the Cably colour themes. Written BEFORE the themes existed.
#  1. Both theme files are valid JSON with meta.name/meta.version.
#  2. Every colour key KiCad declares (regenerated from color_settings.cpp, so
#     an upstream bump that adds a key fails this) is present, and no key is
#     unknown to KiCad.
#  3. ORACLE: kicad-cli renders the D1 fixture board and schematic with
#     --theme Cably from a throwaway KICAD_CONFIG_HOME and the SVG carries the
#     theme's own F.Cu / wire colours; WITHOUT --theme those colours are absent
#     (negative control), so the positive assertion is not vacuous.
set -euo pipefail
FORK="$(cd "$(dirname "$0")/../.." && pwd)"
CLI="${CABLY_KICAD_CLI:-/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli}"
FIX="${CABLY_FIXTURES:-/Users/nevomirzaihamadani/Documents/Cably_Fritzing/Cably-pcb/desktop/test-fixtures}"
fail=0; ok(){ echo "  ok   $1"; }; bad(){ echo "  FAIL $1"; fail=1; }

# 1 + 2 ---------------------------------------------------------------------
grep -oE 'CLR\( *"[a-z0-9_.]+"' "$FORK/common/settings/color_settings.cpp" | grep -oE '"[^"]+"' | tr -d '"' | sort -u > /tmp/cably-theme-keys.txt
NKEYS=$(wc -l < /tmp/cably-theme-keys.txt | tr -d ' ')
for name in "Cably" "Cably Dark"; do
  f="$FORK/cably/themes/$name.json"
  [ -f "$f" ] || { bad "$name.json missing"; continue; }
  python3 - "$f" "$name" /tmp/cably-theme-keys.txt <<'PY' && ok "$name: valid, meta ok, all $NKEYS keys present, none unknown" || bad "$name: schema/keys"
import json,sys
f,name,keys=sys.argv[1],sys.argv[2],[l.strip() for l in open(sys.argv[3]) if l.strip()]
d=json.load(open(f))
assert d['meta']['name']==name, d['meta']
assert isinstance(d['meta']['version'],int)
flat={}
def walk(o,p):
    for k,v in o.items():
        if k=='meta': continue
        if isinstance(v,dict): walk(v,p+k+'.')
        else: flat[p+k]=v
walk(d,'')
missing=[k for k in keys if k not in flat]
unknown=[k for k in flat if k not in keys and not k.endswith('override_item_colors')]
assert not missing, ('missing',missing[:8])
assert not unknown, ('unknown',unknown[:8])
import re
for k,v in flat.items():
    if isinstance(v,str): assert re.match(r'^rgba?\(\s*\d+,\s*\d+,\s*\d+(,\s*[0-9.]+)?\s*\)$',v), (k,v)
PY
done

# 3 ---------------------------------------------------------------------------
[ -x "$CLI" ] || { echo "  skip oracle: kicad-cli not found"; exit $fail; }
hexof(){ python3 -c "import json,re,sys; d=json.load(open('$1')); v=d['$2']['$3']; r,g,b=map(int,re.findall(r'\d+',v)[:3]); print('#%02X%02X%02X'%(r,g,b))"; }
# KICAD_CONFIG_HOME is the ROOT: KiCad appends <major.minor>/ and looks in colors/ under that.
VER=$("$CLI" version | grep -oE '^[0-9]+\.[0-9]+')
T=$(mktemp -d); mkdir -p "$T/$VER/colors"; cp "$FORK/cably/themes/Cably.json" "$T/$VER/colors/Cably.json"
FCU=$(hexof "$FORK/cably/themes/Cably.json" board copper.f 2>/dev/null || python3 -c "import json,re; d=json.load(open('$FORK/cably/themes/Cably.json')); v=d['board']['copper']['f']; r,g,b=map(int,re.findall(r'\d+',v)[:3]); print('#%02X%02X%02X'%(r,g,b))")
WIRE=$(python3 -c "import json,re; d=json.load(open('$FORK/cably/themes/Cably.json')); v=d['schematic']['wire']; r,g,b=map(int,re.findall(r'\d+',v)[:3]); print('#%02X%02X%02X'%(r,g,b))")
mkdir -p "$T/with" "$T/without"
# --mode-single writes ONE file: -o is a file path here (a directory is a silent failure).
OUT=$(KICAD_CONFIG_HOME="$T" "$CLI" pcb export svg --theme Cably --layers F.Cu,Edge.Cuts --mode-single -o "$T/with/board.svg" "$FIX/blinking-led.kicad_pcb" 2>&1) || bad "pcb export svg --theme Cably exited non-zero"
echo "$OUT" | grep -qi "not found" && bad "kicad-cli did not find theme 'Cably' in $T/$VER/colors"
KICAD_CONFIG_HOME="$T" "$CLI" pcb export svg --layers F.Cu,Edge.Cuts --mode-single -o "$T/without/board.svg" "$FIX/blinking-led.kicad_pcb" >/dev/null 2>&1 || true
grep -qi "$FCU" "$T"/with/*.svg && ok "board SVG carries Cably F.Cu $FCU" || bad "board SVG lacks Cably F.Cu $FCU (theme not applied?)"
grep -qi "$FCU" "$T"/without/*.svg && bad "negative control: default render already contains $FCU — assertion vacuous" || ok "negative control: default render lacks $FCU"
mkdir -p "$T/swith" "$T/swithout"
KICAD_CONFIG_HOME="$T" "$CLI" sch export svg --theme Cably -o "$T/swith" "$FIX/blinking-led.kicad_sch" >/dev/null 2>&1 || bad "sch export svg --theme Cably exited non-zero"
KICAD_CONFIG_HOME="$T" "$CLI" sch export svg -o "$T/swithout" "$FIX/blinking-led.kicad_sch" >/dev/null 2>&1 || true
grep -qi "$WIRE" "$T"/swith/*.svg && ok "schematic SVG carries Cably wire $WIRE" || bad "schematic SVG lacks Cably wire $WIRE"
grep -qi "$WIRE" "$T"/swithout/*.svg && bad "negative control: default schematic already contains $WIRE" || ok "negative control: default schematic lacks $WIRE"
rm -rf "$T"
[ $fail = 0 ] && echo "theme tests: ALL OK" || echo "theme tests: FAILURES"
exit $fail
