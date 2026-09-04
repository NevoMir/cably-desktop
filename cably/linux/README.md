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
    #    theme.sh with the installed kicad-cli, launch under Xvfb)
    docker run --rm --cpus 8 \
        -v "$PWD":/src:ro -v cably-linux-build:/build -v cably-linux-install:/opt/cably-desktop \
        cably-desktop-linux-build:ubuntu24.04 /src/cably/linux/run-tests.sh


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
  `cably/tests/fixtures`).

The macOS-only acceptance scripts (`home.sh`, `identity.sh`, `icons.sh`, `dmg.sh`) are not
run here: they inspect an `.app` bundle.

## Linux specifics

- Secret store (`cably/src/cably_bridge_keychain.cpp`, non-Apple half): the Cably session
  goes to the freedesktop Secret Service through libsecret when one answers on the session
  bus, else to `$XDG_CONFIG_HOME/cably-desktop/session.json` (0600 in a 0700 directory,
  written temp-then-rename). A reachable service that fails is an error, never a silent
  fallback. Unit test: `cably/tests/unit/test_cably_secret_store.cpp` (a fake service
  behind the `CABLY_SECRET_SERVICE` seam); CLI round-trip: `cably/tests/bridge.sh` (e).
