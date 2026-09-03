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

    ./build.py … --target package-kicad-unified   (same flags as the app build)

Output: `<build-dir>/dmg/kicad-unified-<date>-<rev>.dmg` (272 MB compressed):
a `KiCad/` folder holding `KiCad.app` and the `Cably *.app` launcher symlinks,
`org.cably.desktop`, `kicad-cli` 10.0.6, 224 symbol files, 155 footprint dirs,
no 3D models. Verified by mounting, launching from the mount, and running
`kicad-cli version` from it. Still to do in F6 proper: the DMG's volume name,
folder name, background and file name come from KiCad's template
(`nightly-packaging/kicadtemplate.dmg.tar.bz2`) and still say KiCad; Developer
ID signing + notarization (the ad-hoc sign step is tolerated); 3D models as a
separate package.
