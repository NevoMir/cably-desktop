#!/usr/bin/env bash
# F2 acceptance: the BUILT product identifies as "Cably Desktop, based on KiCad".
# Written BEFORE the identity changes; it must fail on an unmodified 10.0.6 build.
#
# Everything is measured on the built app under the build dir (Info.plists via
# PlistBuddy, the linked binaries via `strings`), plus two source-level checks
# for things that only show at run time (window title, About text). Optional:
# CABLY_IDENTITY_LAUNCH=1 launches the app and asserts a live process with the
# Cably bundle id, then kills it.
#
# Usage: cably/tests/identity.sh [kicad-dest dir]
#   CABLY_BUILD_DEST overrides the default build output directory.
set -uo pipefail
FORK="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="${1:-${CABLY_BUILD_DEST:-/Users/nevomirzaihamadani/Documents/Cably_Fritzing/kicad-fork.noindex/build.noindex/kicad-dest}}"
PB=/usr/libexec/PlistBuddy
fail=0; ok(){ echo "  ok   $1"; }; bad(){ echo "  FAIL $1"; fail=1; }
pl(){ "$PB" -c "Print $2" "$1" 2>/dev/null || true; }   # pl <plist> <key>

# The on-disk bundle directory stays KiCad.app (kicad-mac-builder hard-codes that
# path in 48 places); the product identity lives in the bundle metadata, and a
# "Cably Desktop.app" symlink gives Finder the product name.
APP="$DEST/KiCad.app"
[ -d "$APP/Contents" ] || { echo "no built app at $APP"; exit 1; }
echo "identity: checking $APP"

# 1. Main bundle -----------------------------------------------------------------
P="$APP/Contents/Info.plist"
[ "$(pl "$P" CFBundleIdentifier)" = "org.cably.desktop" ]  && ok "KiCad.app CFBundleIdentifier = org.cably.desktop" || bad "KiCad.app CFBundleIdentifier = '$(pl "$P" CFBundleIdentifier)' (want org.cably.desktop)"
[ "$(pl "$P" CFBundleName)" = "Cably Desktop" ]            && ok "KiCad.app CFBundleName = Cably Desktop"              || bad "KiCad.app CFBundleName = '$(pl "$P" CFBundleName)'"
[ "$(pl "$P" CFBundleDisplayName)" = "Cably Desktop" ]     && ok "KiCad.app CFBundleDisplayName = Cably Desktop"       || bad "KiCad.app CFBundleDisplayName = '$(pl "$P" CFBundleDisplayName)'"
case "$(pl "$P" NSHumanReadableCopyright)" in
  *"KiCad Developers"*"Cably"*|*"Cably"*"KiCad Developers"*) ok "KiCad.app copyright names both The KiCad Developers and Cably" ;;
  *) bad "KiCad.app NSHumanReadableCopyright = '$(pl "$P" NSHumanReadableCopyright)'" ;;
esac
[ -L "$DEST/Cably Desktop.app" ] && [ -d "$DEST/Cably Desktop.app/Contents" ] && ok "'Cably Desktop.app' symlink resolves to the main bundle" || bad "'Cably Desktop.app' symlink missing/broken in $DEST"

# 2. Editor launchers (inner bundles + the top-level symlinks CMake creates) ------
# inner-bundle-name|bundle id suffix|display name|top-level symlink
while IFS='|' read -r inner suffix name link; do
  P="$APP/Contents/Applications/$inner.app/Contents/Info.plist"
  [ -f "$P" ] || { bad "$inner.app missing"; continue; }
  [ "$(pl "$P" CFBundleIdentifier)" = "org.cably.desktop.$suffix" ] && ok "$inner.app id = org.cably.desktop.$suffix" || bad "$inner.app id = '$(pl "$P" CFBundleIdentifier)' (want org.cably.desktop.$suffix)"
  [ "$(pl "$P" CFBundleName)" = "$name" ]        && ok "$inner.app CFBundleName = $name"        || bad "$inner.app CFBundleName = '$(pl "$P" CFBundleName)' (want $name)"
  [ "$(pl "$P" CFBundleDisplayName)" = "$name" ] && ok "$inner.app CFBundleDisplayName = $name" || bad "$inner.app CFBundleDisplayName = '$(pl "$P" CFBundleDisplayName)'"
  [ -L "$DEST/$link.app" ] && [ -d "$DEST/$link.app/Contents" ] && ok "'$link.app' launcher symlink resolves" || bad "'$link.app' launcher symlink missing/broken in $DEST"
done <<'EOF'
pcbnew|pcbnew|Cably PCB Editor|Cably PCB Editor
eeschema|eeschema|Cably Schematic Editor|Cably Schematic Editor
gerbview|gerbview|Cably Gerber Viewer|Cably Gerber Viewer
pcb_calculator|pcb-calculator|Cably PCB Calculator|Cably PCB Calculator
pl_editor|pl-editor|Cably Drawing Sheet Editor|Cably Drawing Sheet Editor
bitmap2component|bitmap2component|Cably Image Converter|Cably Image Converter
EOF

# 3. Nothing under our control still names KiCad as the PRODUCT ------------------
for P in "$APP/Contents/Info.plist" "$APP"/Contents/Applications/*.app/Contents/Info.plist; do
  for k in CFBundleIdentifier CFBundleName CFBundleDisplayName; do
    v="$(pl "$P" "$k")"
    case "$v" in *[Kk][Ii][Cc][Aa][Dd]*) bad "$(basename "$(dirname "$(dirname "$P")")") $k still says KiCad: '$v'" ;; esac
  done
done
for a in "$DEST"/*.app; do
  case "$(basename "$a")" in KiCad.app) ;; *[Kk][Ii][Cc][Aa][Dd]*) bad "top-level app name still says KiCad: $(basename "$a")" ;; esac
  case "$(basename "$a")" in "Cably "*|KiCad.app) ;; *) bad "top-level launcher not renamed: $(basename "$a")" ;; esac
done
ok "no bundle id/name under our control names KiCad as the product (checked $(ls -d "$APP"/Contents/Applications/*.app | wc -l | tr -d ' ') launchers + main)"

# 4. About text, as linked into the binaries (common is static: in kicad + kifaces)
for bin in "$APP/Contents/MacOS/kicad" "$APP/Contents/PlugIns/_pcbnew.kiface" "$APP/Contents/PlugIns/_eeschema.kiface"; do
  [ -f "$bin" ] || { bad "missing binary $bin"; continue; }
  S="$(strings "$bin")"
  b="$(basename "$bin")"
  grep -qF "based on KiCad" <<<"$S"                    && ok "$b: About text says 'based on KiCad'"                 || bad "$b: no 'based on KiCad' in binary"
  grep -qF "GNU General Public License" <<<"$S"        && ok "$b: GPL notice present"                               || bad "$b: no 'GNU General Public License' in binary"
  grep -qF "Cably Desktop" <<<"$S"                     && ok "$b: product name 'Cably Desktop' linked in"          || bad "$b: no 'Cably Desktop' in binary"
  grep -qF "The complete KiCad EDA Suite is released" <<<"$S" && bad "$b: still carries KiCad's own About license sentence" || ok "$b: upstream About license sentence replaced"
  # GPLv3 s6: the About dialog must carry a corresponding-source URL
  grep -qE "https?://[^ ]*cably[^ ]*" <<<"$S"          && ok "$b: a cably source URL is linked in"                 || bad "$b: no https://...cably... URL in binary"
done

# 5. Source-level checks for run-time-only strings --------------------------------
CFG="$FORK/cably/src/cably_config.h"
[ -f "$CFG" ] && grep -qE '^#define CABLY_SOURCE_URL +"https?://' "$CFG" && ok "cably_config.h defines CABLY_SOURCE_URL" || bad "cably/src/cably_config.h missing or no CABLY_SOURCE_URL"
[ -f "$CFG" ] && grep -qE '^#define CABLY_PRODUCT_NAME +"Cably Desktop"' "$CFG" && ok "cably_config.h defines CABLY_PRODUCT_NAME \"Cably Desktop\"" || bad "cably_config.h has no CABLY_PRODUCT_NAME \"Cably Desktop\""
MF="$FORK/kicad/kicad_manager_frame.cpp"
if grep -q 'CABLY_PRODUCT_NAME' "$MF" && ! grep -qE 'SetTitle\( *wxT\( *"KiCad"|wxS\( *"KiCad ?"|wxString\( *"KiCad ' "$MF"; then
  ok "manager window title source uses CABLY_PRODUCT_NAME (Cably Desktop), no literal KiCad title"
else
  bad "kicad_manager_frame.cpp still titles the window \"KiCad\""
fi
AB="$FORK/common/dialog_about/AboutDialog_main.cpp"
grep -q 'based on KiCad' "$AB" && grep -q 'GNU General Public License' "$AB" && grep -q 'CABLY_SOURCE_URL' "$AB" && ok "About source: based on KiCad + GPL + CABLY_SOURCE_URL" || bad "AboutDialog_main.cpp lacks 'based on KiCad' / GPL notice / CABLY_SOURCE_URL"
grep -qE '_HKI\( *"KiCad ' "$FORK"/pcbnew/*.cpp "$FORK"/eeschema/*.cpp "$FORK"/eeschema/symbol_editor/*.cpp 2>/dev/null && bad "an editor About title still says 'KiCad ...'" || ok "editor About titles no longer say 'KiCad ...'"

# 6. Optional live check: launched process carries the Cably bundle id ------------
if [ "${CABLY_IDENTITY_LAUNCH:-0}" = "1" ]; then
  open -n -a "$APP" >/dev/null 2>&1
  found=0
  for _ in $(seq 1 40); do
    if lsappinfo find bundleid=org.cably.desktop 2>/dev/null | grep -q ASN; then found=1; break; fi
    sleep 0.5
  done
  [ "$found" = 1 ] && ok "launched process registered with LaunchServices as org.cably.desktop" || bad "no running process with bundle id org.cably.desktop after 20s"
  pkill -f "$APP/Contents/MacOS/kicad" >/dev/null 2>&1 || true
fi

if [ "$fail" = 0 ]; then echo "identity: OK"; else echo "identity: FAILED"; fi
exit $fail
