# Building Cably Desktop (F1) on macOS 26 / Apple Silicon

Toolchain: KiCad's own `kicad-mac-builder` (gitlab.com/kicad/packaging/kicad-mac-builder),
with the patch in this directory applied on top (`git apply kicad-mac-builder-macos26.patch`
inside the kicad-mac-builder checkout). What the patch does and why, verified 2026-09-03:

1. ngspice 45.2: `CXXFLAGS=-Wno-invalid-specialization CFLAGS=...` on its configure line —
   Apple clang 21 hard-errors on cppduals specialising `std::is_compound`.
2. `install-packages3d-into-app` is a no-op — the multi-GB 3D-model clone is packaging
   data (F6), not a build input, and it parked the whole compile.
3. `install-docs-into-app` copies only if the docs tree exists (`--skip-docs-update`).
4. `sign-app` is tolerant — dev builds stay linker ad-hoc-signed; F6 signs with Developer ID.

Homebrew prerequisites beyond the toolchain's own `ci/src/brew_deps.sh`:
`brew install cmake ninja swig python@3.12` (the `cmake` CASK is not enough — the formula
must be on /opt/homebrew/bin).

## The embedded Python (2026-09-05)

The app bundles a complete, self-contained Python.framework — the one the
binaries are linked against and the one `Contents/Frameworks/` ships. It is NOT
Homebrew's (`python@3.12` links five Homebrew dylibs — libmpdec, libcrypto.3,
libssl.3, libsqlite3, liblzma — and a bundle built against it dies on any other
Mac). It is python.org's macOS installer framework, made relocatable by
`cably/toolchain/prepare-python-framework.sh` (arch-thinned, every load command
rewritten to `@rpath/Versions/3.12/...`, ad-hoc re-signed; the same layout the
official KiCad app ships):

    curl -LO https://www.python.org/ftp/python/3.12.10/python-3.12.10-macos11.pkg
    shasum -a 256 python-3.12.10-macos11.pkg
    # 8373e58da4ea146b3eb1c1f9834f19a319440b6b679b06050b1f9ee3237aa8e4  (45,720,356 bytes)
    bash <this repo>/cably/toolchain/prepare-python-framework.sh \
      python-3.12.10-macos11.pkg <...>/build.noindex/python arm64
    # -> <...>/build.noindex/python/Python.framework (86 MB), prints the cmake args

(3.12.11+ are security-only releases without an installer; 3.12.10 is the last
with one.) `-DPYTHON_FRAMEWORK` MUST be the framework ROOT (`.../Python.framework`):
`kicad/CMakeLists.txt` copies that path verbatim into `Contents/Frameworks/`, and
the fork's `CMakeLists.txt` now refuses a `Versions/<X.Y>` directory. `PYTHON_DEST`
(where pcbnew.py/_pcbnew.so are installed) is FORCEd from the Python found by the
current configure; it used to stick from the first configure's PATH python3.

Build command (from the kicad-mac-builder checkout; `PYFW` is the prepared framework):

    PYFW=<...>/build.noindex/python/Python.framework
    FW=$PYFW/Versions/3.12
    PATH=/opt/homebrew/bin:$PATH nice -n 10 ./build.py --arch=arm64 \
      --kicad-source-dir=<this repo> --build-dir=<...>/build.noindex --jobs 6 \
      --target kicad --skip-docs-update --no-retry-failed-build \
      "--extra-kicad-cmake-args=-DPYTHON_EXECUTABLE=$FW/bin/python3.12;-DPYTHON_LIBRARY=$FW/Python;-DPYTHON_INCLUDE_DIR=$FW/include/python3.12;-DPYTHON_FRAMEWORK=$PYFW;-DKICAD_SCRIPTING_WXPYTHON=OFF"

The extra args MUST be one semicolon-separated list passed in `=` form (argparse + CMake list
splicing). Output: `<build-dir>/kicad-dest/KiCad.app` (+ the per-editor .app launchers).
Keep the build dir under a `*.noindex` name so Spotlight ignores ~10 GB of objects.
Acceptance for portability: `cably/tests/portable.sh` (kicad-mac-builder's
`bin/verify-app.sh` rule plus the Python-framework layout and a launch) — run it
after EVERY rebuild; the toolchain never runs verify-app.sh itself.
Changing the Python (or any `-D` in the extra args) is a configure change: delete
`kicad/src/kicad-stamp/kicad-configure` too, and `rm -rf kicad/src/kicad-build
kicad-dest/KiCad.app` — an incremental install never removes a stale
`Contents/Frameworks/<X.Y>` or `Versions/<other>` tree.

Acceptance (F1): the app launches; `KiCad.app/Contents/MacOS/kicad-cli pcb export gerbers`
on the D1 fixture board is byte-identical to the official KiCad's output modulo version/date
headers (20/21 files; the .gbrjob embeds the version), and `sch export netlist` has identical
node membership on all 28 nets.

Incremental rebuild after source changes: kicad-mac-builder drives KiCad as a CMake
ExternalProject WITHOUT `BUILD_ALWAYS`, so once `<build-dir>/kicad/src/kicad-stamp/kicad-build`
exists a second `build.py` run reports "Build complete" in ~20 s without compiling anything.
Delete the stamps of the steps that must rerun, then run the same command:

    rm <build-dir>/kicad/src/kicad-stamp/kicad-{build,install,install-*-into-app,collect-licenses,sign-app,done}

(keep `kicad-configure`: the inner `make` re-runs CMake by itself when a CMakeLists changes).
An incremental install does not remove files it no longer generates — e.g. the pre-F2
`PCB Editor.app` launcher symlinks in `kicad-dest/` — delete those by hand.
The sign-app step is tolerant on purpose: `codesign --sign -` with entitlements fails on
macOS 26 for the embedded Python.framework ("bundle format unrecognized"), the message is
logged and the dev build keeps its linker ad-hoc signature (F6 signs with a Developer ID).

## Unsigned dev DMG (F6-lite, 2026-09-03)

The toolchain's `package-kicad-nightly` target is disabled upstream (its script
exits 1 for `nightly`/`extras`); only `package-kicad-unified` packages. The patch
in this directory trims that target's DEPENDS to `kicad symbols footprints
templates` (no multi-GB 3D models, no docs) for a dev DMG:

    ./build.py … --target package-kicad-unified --jobs 4   (same flags as the app build)

The packaging step is a CMake ExternalProject too: to re-run it after the app
was rebuilt, delete its stamps first:

    rm -f <build-dir>/package-kicad-unified/src/package-kicad-unified-stamp/package-kicad-unified-* \
          <build-dir>/CMakeFiles/package-kicad-unified-complete

Output: `<build-dir>/dmg/cably-desktop-<kicad version>-<date>-<rev>.dmg` (~300 MB
compressed). Top level (2026-09-05): the REAL bundle as `Cably Desktop.app`
(renamed from the toolchain's hard-coded `KiCad.app` only in this packaging
step; identity org.cably.desktop / "Cably Desktop", symbol + footprint libraries,
`kicad-cli` in Contents/MacOS, the embedded Python.framework, no 3D models), the
`Applications` symlink, and the folder `Cably Desktop/` holding the six editor
launcher symlinks re-pointed to `../Cably Desktop.app/Contents/Applications/<x>.app`
plus `demos/`. Nothing in the image is named KiCad.app.
Still to do: 3D models as a separate package. Signing + notarization: next section.

## Developer ID signing, notarization, stapling (F6, 2026-09-05)

Model: KiCad's own signed+notarized 10.0.1 DMG (every code object Developer ID,
hardened runtime, secure timestamp, the four entitlements of
`signing/entitlements.plist`; the image signed, notarized and stapled).
Acceptance: `cably/tests/signed.sh [file.dmg]` (six sections; PASSes on KiCad's
DMG as the positive control, see its header). Two scripts in this directory do
the work, both strict (`set -e`, any codesign/notarytool failure is fatal), GPL:

- `sign-macos.sh <KiCad.app> [identity]` — seal hygiene, then codesign inside-out.
  Hygiene first because the embedded python.org framework breaks its own seal
  twice: the DANGLING `Versions/3.12/bin/python3-intel64` symlink (the x86_64
  launcher was thinned away; `prepare-python-framework.sh` now removes the link
  too) makes every verify fail with "No such file or directory", and any run of
  the bundled Python writes `__pycache__/*.pyc` into the sealed framework ("a
  sealed resource is missing or invalid" — the official KiCad.app in
  /Applications fails `--deep --strict` for exactly this reason once launched).
  So: dangling links removed, `__pycache__` stripped, the stdlib byte-compiled
  with `--invalidation-mode checked-hash` (1807 .pyc, 3 s; valid regardless of
  mtime, Python writes nothing afterwards); stale `*.cstemp` files (codesign's
  own temp file, left behind when a signing run is killed — a Mach-O too, and
  gone by the time the batch reaches it) are deleted. Then every Mach-O regular file (by
  magic, ~290) → `Python.app` → `Python.framework/Versions/3.12` (the root
  resolves to it) → the six editor apps → the app, each with
  `codesign --force --sign <id> --options runtime --timestamp --entitlements
  entitlements.plist` (the copy here is byte-identical to kicad-mac-builder's),
  then `codesign --verify --deep --strict`. Measured 96 s (hygiene + listing
  40 s, 290 Mach-O 53 s, bundles 4 s). The secure timestamp comes from
  `http://timestamp.apple.com/ts01` (port 80): on a network where port 443 of
  that host is refused, `--timestamp` still works — measure with a probe file
  before blaming the network. Why not the toolchain's `bin/apple.py`: it skips Python.framework, never
  passes `--timestamp`, and the fork's install-step signer can only sign ad-hoc
  (`CMakeLists.txt` FORCEs `KICAD_OSX_SIGNING_ID "-"`). Never launch the app or
  run its Python from `kicad-dest` after signing — verify on the DMG mount.
- `notarize.sh <KiCad.app|file.dmg> [--no-staple]` — `xcrun notarytool submit`
  with the stored keychain profile `$CABLY_NOTARY_PROFILE` (default `cably-app`,
  created ONCE interactively with `xcrun notarytool store-credentials cably-app`;
  no password ever passes through the scripts), WITHOUT `--wait`: prints the
  submission id, polls `notarytool info` every 120 s (`CABLY_NOTARY_POLL`), on
  Invalid prints `notarytool log`, on Accepted `stapler staple` + `validate`.
  An .app is zipped with `ditto -c -k --keepParent` for the upload and the ticket
  is stapled to the app itself (Apple's advice for an app inside a DMG, so a
  copy dragged out opens offline; KiCad staples only its DMG).

Patch hunks: `kicad.cmake` — `sign-app` runs `sign-macos.sh` (STRICT) when
`SIGNING_CERTIFICATE_ID` is not `-`, the tolerant apple.py ad-hoc pass only for
dev builds; a `notarize-app` step (notarize.sh, stapled) when the cache var
`CABLY_NOTARY_PROFILE` is non-empty. `bin/package.sh` — the DMG is signed with
`--timestamp` and, with the profile, notarized + stapled by notarize.sh.
`package_kicad_unified.cmake` passes the profile through. The altool-era
notarize steps are dead (apple.py has no such subcommand) and stay untouched.

Release recipe (identity by SHA-1 so no spaces reach cmake; `security
find-identity -v -p codesigning` prints it; build.py has no flag for the
profile, set it in the cache once):

    ID=E692F20010AFEFAECCDBAA06497538ADF6ECDB42     # Developer ID Application: ... (Z9HUH3VGHM)
    cmake -DCABLY_NOTARY_PROFILE=cably-app <build-dir>   # optional: notarize inside the build
    rm -f <build-dir>/kicad/src/kicad-stamp/kicad-{sign-app,done}      # re-sign an existing build
    ./build.py … --target kicad --signing-certificate-id $ID --signing-identity $ID --hardened-runtime
    rm -f <build-dir>/package-kicad-unified/src/package-kicad-unified-stamp/package-kicad-unified-* \
          <build-dir>/CMakeFiles/package-kicad-unified-complete
    ./build.py … --target package-kicad-unified --jobs 4 (same signing flags)
    cably/tests/signed.sh <build-dir>/dmg/<newest>.dmg

Measured 2026-09-05: signing 96 s; app notarization 79 min for the team's FIRST
submission (zip 18 s, upload 86 s), the DMG's 6 min; packaging ~2 min; the whole
release run ≈ 1 h 40 min, all of it waiting on Apple — `notarize.sh` gives up
after `CABLY_NOTARY_TIMEOUT` (default 5400 s) and prints the `notarytool info`
command to resume by hand (the submission keeps running server-side; staple it
yourself and `touch` the `kicad-notarize-app` stamp).

Without the cache profile, notarize by hand between the two build.py runs and
after the second: `cably/toolchain/notarize.sh <build-dir>/kicad-dest/KiCad.app`,
then `cably/toolchain/notarize.sh <build-dir>/dmg/<file>.dmg`. Deleting only the
`kicad-sign-app` + `kicad-done` stamps re-runs just the signing (verified: the
other kicad-* stamps are untouched, nothing is recompiled or re-installed); any
other kicad-* stamp re-copies the framework and undoes the seal hygiene.

## The DMG presents "Cably Desktop" (F6, 2026-09-03)

`bin/package.sh` never designs the installer window: it untars a TEMPLATE image,
grows it (`hdiutil resize`), mounts it, rsyncs the app into the product folder and
converts the result to a compressed read-only DMG. Volume name, folder name,
background, volume icon and icon layout all come from that template — KiCad's
(`unified-packaging/kicadtemplate.dmg.tar.bz2`) says KiCad everywhere. Ours lives
in the fork:

    cably/toolchain/dmg/cablytemplate.dmg.tar.bz2   (~158 KB; a 200 MB HFS+ UDRW image)
    cably/toolchain/dmg/make-template.sh            regenerates it, no Finder needed
    cably/toolchain/dmg/make-ds-store.py            writes/dumps the Finder .DS_Store
    cably/icons/src/dmg-background.svg              the window background (660x400)

Template contents (volume "Cably Desktop"): `Applications -> /Applications`,
`Cably Desktop/` (package.sh fills it with the launchers and demos; the bundle
itself is placed at the top level by package.sh), `.background.png` (rendered
from the SVG with rsvg-convert), `.VolumeIcon.icns` (= `kicad/kicad.icns`, the
Cably mark; custom-icon flag set on the root), and a `.DS_Store` written by
`make-ds-store.py` — the same Buddy-allocator/B-tree layout the ds_store +
mac_alias libraries (dmgbuild) produce: window {{100,100},{660,400}}, no
toolbar/sidebar, icon view 96 px, background = an Alias-v2 record of
`.background.png` on the volume (verified to resolve through
`CFURLCreateBookmarkDataFromAliasRecord` + `URL(resolvingBookmarkData:)` while
the image is mounted at /Volumes/Cably Desktop), icons at Cably Desktop.app
(165,170), Applications (495,170), Cably Desktop (330,300).

Patch hunks (in `kicad-mac-builder-macos26.patch`):

- `package_kicad_unified.cmake`: cache var `CABLY_DMG_TEMPLATE` defaulting to
  `<kicad source dir>/cably/toolchain/dmg/cablytemplate.dmg.tar.bz2` (with
  `--kicad-source-dir` that is this fork), passed to package.sh as an env var.
  build.py has no flag for the toolchain's own cache, so to override it edit the
  cache once — `cmake -DCABLY_DMG_TEMPLATE=/path/to/other.dmg.tar.bz2 <build-dir>` —
  and later build.py runs keep it; the empty string falls back to KiCad's template.
- `bin/package.sh`: when `CABLY_DMG_TEMPLATE` is set, untar it (TEMPLATE becomes
  its basename minus `.tar.bz2`), skip the `PACKAGING_DIR/background.png` copy
  (ours is in the template), `MOUNT_NAME='Cably Desktop'` and the mounted template
  volume is RENAMED to it (`diskutil rename`, in place) so MOUNT_NAME really is the
  volume name Finder shows — mutation check: MOUNT_NAME reverted to the upstream
  value makes `dmg.sh` fail on the volume name —, the product folder is
  `Cably Desktop/` (demos still moved next to it), and the file name is
  `cably-desktop-<KICAD_SEMANTIC_VERSION>-<yyyymmdd-hhmmss>-<git rev>.dmg`
  (version = the built app's CFBundleShortVersionString up to the first `-`,
  i.e. the tag `cmake/KiCadVersion.cmake` derives KICAD_SEMANTIC_VERSION from;
  `git describe --abbrev=0` as fallback). `RELEASE_NAME` gives
  `cably-desktop-<RELEASE_NAME>.dmg`. The unified copy step rsyncs
  `<install dir>/KiCad.app/` to `<mount>/Cably Desktop.app/`, recreates each
  `Cably *.app` launcher symlink of the install dir (skipping the build tree's
  `Cably Desktop.app -> KiCad.app` link) inside `Cably Desktop/` with its target
  rewritten to `../Cably Desktop.app/...`, copies `demos/` there, and fails if
  anything in the image is still named KiCad.app.

Acceptance: `cably/tests/dmg.sh [file.dmg]` (default: newest under
`<build-dir>/dmg`) mounts read-only at a private mountpoint and asserts the file
name, volume name, top level (the real `Cably Desktop.app` directory with
Contents/MacOS/kicad, Applications link, the folder with exactly the six
launchers resolving through `../Cably Desktop.app/...` and demos, nothing named
KiCad at the top level and no `KiCad.app` anywhere in the volume), the bundle id,
that the background is the render of our SVG and not KiCad's, the volume icon,
the .DS_Store names, and that the app launches from the mount (alive after 12 s,
exits on SIGTERM). After a rebuild also run `cably/tests/portable.sh` and
`identity.sh` against the app inside the mounted image. What still says KiCad
inside the image is attribution or a path the toolchain hard-codes:
`kicad-cli`, the `kicad` executable name, and the About/NOTICE texts.
