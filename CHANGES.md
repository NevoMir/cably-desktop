# Changes from upstream KiCad (GPLv3 §5(a) record)

Base: KiCad tag 10.0.6 (gitlab.com/kicad/code/kicad).

| date | area | change |
|---|---|---|
| 2026-09-02 | repo | Forked at 10.0.6; added NOTICE.md, CHANGES.md, `cably/` for additions; upstream remote kept for rebasing. No source files modified yet. |
| 2026-09-03 | theme | Added `cably/themes/Cably.json` and `Cably Dark.json` (181 colour keys, schema 5, generated from cably.dev's palette) and `cably/tests/theme.sh` (key completeness against color_settings.cpp + kicad-cli render oracle with negative control). Data only; no source changes. |
| 2026-09-03 | toolchain | `cably/toolchain/`: the kicad-mac-builder patch (ngspice clang flag, 3D-models step no-op, conditional docs copy, tolerant ad-hoc signing) and the exact macOS 26 build recipe. F1 acceptance passed: app launches; Gerbers/netlist match the official 10.0.1 build. No KiCad source changes. |
