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

Build command (from the kicad-mac-builder checkout; `FW` is Homebrew's framework Python):

    FW=/opt/homebrew/opt/python@3.12/Frameworks/Python.framework/Versions/3.12
    PATH=/opt/homebrew/bin:$PATH nice -n 10 ./build.py --arch=arm64 \
      --kicad-source-dir=<this repo> --build-dir=<...>/build.noindex --jobs 6 \
      --target kicad --skip-docs-update --no-retry-failed-build \
      "--extra-kicad-cmake-args=-DPYTHON_EXECUTABLE=$FW/bin/python3.12;-DPYTHON_LIBRARY=$FW/Python;-DPYTHON_INCLUDE_DIR=$FW/include/python3.12;-DPYTHON_FRAMEWORK=$FW;-DKICAD_SCRIPTING_WXPYTHON=OFF"

The extra args MUST be one semicolon-separated list passed in `=` form (argparse + CMake list
splicing). Output: `<build-dir>/kicad-dest/KiCad.app` (+ the per-editor .app launchers).
Keep the build dir under a `*.noindex` name so Spotlight ignores ~10 GB of objects.

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

Output: `<build-dir>/dmg/cably-desktop-<kicad version>-<date>-<rev>.dmg` (~270 MB
compressed): the product folder holding `KiCad.app` (the bundle directory the
toolchain addresses by that path; its identity is org.cably.desktop / "Cably
Desktop"), `kicad-cli` 10.0.6, the `Cably Desktop.app` and `Cably *.app` launcher
symlinks, the symbol and footprint libraries, no 3D models; `demos/` next to it.
Still to do: Developer ID signing + notarization (the ad-hoc sign step is
tolerated); 3D models as a separate package.

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
`Cably Desktop/` (package.sh fills it), `demos/`, `.background.png` (rendered
from the SVG with rsvg-convert), `.VolumeIcon.icns` (= `kicad/kicad.icns`, the
Cably mark; custom-icon flag set on the root), and a `.DS_Store` written by
`make-ds-store.py` — the same Buddy-allocator/B-tree layout the ds_store +
mac_alias libraries (dmgbuild) produce: window {{100,100},{660,400}}, no
toolbar/sidebar, icon view 96 px, background = an Alias-v2 record of
`.background.png` on the volume (verified to resolve through
`CFURLCreateBookmarkDataFromAliasRecord` + `URL(resolvingBookmarkData:)` while
the image is mounted at /Volumes/Cably Desktop), icons at Cably Desktop (165,170),
Applications (495,170), demos (330,300).

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
  `cably-desktop-<RELEASE_NAME>.dmg`.

Acceptance: `cably/tests/dmg.sh [file.dmg]` (default: newest under
`<build-dir>/dmg`) mounts read-only at a private mountpoint and asserts the file
name, volume name, top level (folder, Applications link, nothing named KiCad),
the bundle id inside, that the background is the render of our SVG and not
KiCad's, the volume icon, the .DS_Store names, and that the app launches from
the mount (alive after 12 s, exits on SIGTERM). What still says KiCad inside
the image is attribution or a path the toolchain hard-codes: the `KiCad.app`
bundle directory inside the product folder, `kicad-cli`, and the About/NOTICE
texts.
