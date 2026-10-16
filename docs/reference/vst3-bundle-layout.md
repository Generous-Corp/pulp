# Windows VST3 bundle layout

Pulp's raw Windows VST3 target is assembled as
`VST3/<name>.vst3/Contents/<arch>-win/`. The architecture directory contains
the loadable `<name>.vst3`, `pulp-build-info.json`, and every file listed in
`bundled_runtime_libraries` (including Skia's `icudtl.dat` when the editor
uses Skia). x64 uses `x86_64-win`; ARM64EC uses `arm64ec-win`.

The raw target directory is the build-system contract. A later ZIP or installer
step may copy missing files and therefore cannot prove that the direct Pulp
target was complete. `tools/cmake/scripts/check_vst3_bundle_layout.py` checks
the raw layout and refuses missing, malformed, or wrongly typed build-info
records and declared runtime sidecars.
