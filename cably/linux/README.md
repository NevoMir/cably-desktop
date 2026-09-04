# Building, testing and packaging Cably Desktop on Linux (Docker)

Everything here runs in a container built from `Dockerfile.build` (Ubuntu 24.04,
native on arm64 and amd64) or on the GitHub Actions runner (`.github/workflows/linux.yml`
sequences the same scripts; `apt-packages.txt` is the shared package list, kept equal to
the Dockerfile's by `cably/tests/linux-ci.sh`). The fork itself is never copied into the
image.

    # 1. image (~2 GB; a few minutes)
    docker build -t cably-desktop-linux-build:ubuntu24.04 -f cably/linux/Dockerfile.build cably/linux

    # 2. volumes: the Ninja tree (incremental across containers) and the install prefix
    docker volume create cably-linux-build
    docker volume create cably-linux-install

    # 3. build + install (first full compile: ~2 h 10 min at -j6 on an M-series Mac
    #    with 8 CPUs / 28 GB; later runs are incremental, seconds to minutes)
    docker run --rm --cpus 8 --memory 28g \
        -v "$PWD":/src:ro -v cably-linux-build:/build -v cably-linux-install:/opt/cably-desktop \
        cably-desktop-linux-build:ubuntu24.04 /src/cably/linux/build.sh

    # 4. tests (unit tests, bridge.sh against the build tree incl. the secret store,
    #    theme.sh with the installed kicad-cli, launch under Xvfb, identity-linux.sh,
    #    package-deb.sh + deb.sh's static checks; the .deb lands in /build/deb)
    docker run --rm --cpus 8 \
        -v "$PWD":/src:ro -v cably-linux-build:/build -v cably-linux-install:/opt/cably-desktop \
        cably-desktop-linux-build:ubuntu24.04 /src/cably/linux/run-tests.sh

    # 5. the package's install test needs a FRESH Ubuntu, i.e. a Docker host: copy the
    #    .deb out of the volume and run deb.sh here (it starts `docker run --rm ubuntu:24.04`,
    #    apt-installs the .deb, launches the manager under Xvfb and runs theme.sh)
    docker run --rm -v cably-linux-build:/build -v "$PWD/dist":/dist ubuntu:24.04 cp /build/deb/cably-desktop_*.deb /dist/
    cably/tests/deb.sh dist/cably-desktop_*.deb

## The scripts

- `build.sh` configures with `-DKICAD_USE_CMAKE_FINDPROTOBUF=ON -DKICAD_SCRIPTING_WXPYTHON=OFF
  -DKICAD_BUILD_QA_TESTS=OFF -DKICAD_BUILD_I18N=OFF` (RelWithDebInfo, prefix
  `/opt/cably-desktop`, `CMAKE_INSTALL_RPATH` `$ORIGIN`-relative so the installed binaries
  find `lib/libkicommon.so` & co. without `LD_LIBRARY_PATH`), builds `all` plus the
  `cably-bridge-cli` test target, installs into an emptied prefix (only one this script
  populated before is wiped), and prints the elapsed time of each step and the installed
  `kicad-cli version`. Knobs: `CABLY_FORK`, `CABLY_BUILD_DIR`, `CABLY_PREFIX`, `CABLY_JOBS`
  (default 6), `CABLY_CMAKE_EXTRA`.
- `run-tests.sh` prints one `ok`/`FAIL` line per check and exits 1 on any FAIL; it is red
  on an empty build volume by design. Knobs: the same plus `CABLY_FIXTURES` (default
  `cably/tests/fixtures`) and `CABLY_DEB_DIR` (default `$CABLY_BUILD_DIR/deb`).
- `package-deb.sh` writes `cably-desktop_10.0.6+cably.<yyyymmdd>.<N>.g<rev>[.dirty]_<arch>.deb`
  into `$CABLY_DEB_DIR` (default: cwd): a stripped `cmake --install` of the build under
  `/opt/cably-desktop`, launchers copied to `/usr/share/applications` with an absolute
  `Exec=`, icons/metainfo/MIME linked into `/usr/share`, `/usr/bin/cably-desktop{,-cli}`,
  `/usr/share/doc/cably-desktop/{copyright,NOTICE.md,CHANGES.md.gz}`; `Depends` come from
  `dpkg-shlibdeps` over the binaries plus `libsecret-1-0` and `python3`. Test:
  `cably/tests/deb.sh`.
  The version: `<yyyymmdd>` is the UTC build day; `<N>` (commits since the 10.0.6 tag),
  `<rev>` (short hash) and `.dirty` (uncommitted changes) are KiCad's `git describe --dirty`
  as recorded in the BUILD tree (`$CABLY_BUILD_DIR/kicad_build_version.h`, the string
  `kicad-cli version` prints), never the checkout's HEAD, so the name says what the
  binaries are (`git describe` in the checkout is only a warned fallback for a tree without
  the record). `N` sits before the rev because dpkg compares a hex rev as text, so a later
  build of the same day could sort as a downgrade; `N` grows with every commit, the day
  orders first across days, and a rebuild of the same commit keeps its version (a time of
  day would let a later build of an older commit win). `deb.sh` checks the ordering with
  `dpkg --compare-versions` and that the packaged `kicad-cli version --format about`
  reports the same `<N>-g<rev>[-dirty]`.
- `make-icons.sh` renders `cably/icons/src/*.svg` into `icons/hicolor` (the PNGs are
  committed; rerun after editing an SVG; needs `rsvg-convert`).
- `CablyLinuxNames.cmake` is the product's desktop identity (`org.cably.desktop*`
  launchers, metainfo id, icon names), included at the end of `cmake/KiCadAppNames.cmake`
  so both consumers of those variables (the Wayland app_id in `common/` and the
  `resources/` metadata rules) see it. The launcher/metainfo text lives, edited in place,
  in `resources/linux` (see CHANGES.md).

The macOS-only acceptance scripts (`home.sh`, `identity.sh`, `icons.sh`, `dmg.sh`) are not
run here: they inspect an `.app` bundle; `identity-linux.sh` and `deb.sh` are their Linux
counterparts.

## Linux specifics

- Secret store (`cably/src/cably_bridge_keychain.cpp`, non-Apple half): the Cably session
  goes to the freedesktop Secret Service through libsecret when one answers on the session
  bus, else to `$XDG_CONFIG_HOME/cably-desktop/session.json` (0600 in a 0700 directory,
  written temp-then-rename). A reachable service that fails is an error, never a silent
  fallback. Unit test: `cably/tests/unit/test_cably_secret_store.cpp` (a fake service
  behind the `CABLY_SECRET_SERVICE` seam); CLI round-trip: `cably/tests/bridge.sh` (e).
- The package installs under `/opt/cably-desktop` and never owns a path a distribution
  `kicad` package ships (no `kicad-*.xml` MIME files or `application-x-kicad-*` icons
  under `/usr/share`), so both can be installed side by side.
