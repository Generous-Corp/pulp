# Governed importer validation build receipt

Date: 2026-10-07

## Scope

The importer roundtrip harnesses are user-facing validation entry points. Seven
were already routed through `tools/ci/governed-build.sh` on `origin/main`; the
Spectr harness was the remaining raw build emitter in that family. This slice
changes only:

- `tools/import-validation/spectr-roundtrip.sh`

Its standalone Spectr build now invokes the Pulp governed wrapper with the
absolute external build directory:

```text
bash "$PULP/tools/ci/governed-build.sh" cmake --build "$SPECTR/build" --config Release
```

The wrapper supplies the host lease/tier bound through
`CMAKE_BUILD_PARALLEL_LEVEL`; the previous whole-host
`-j$(sysctl -n hw.ncpu)` is gone. Configure remains Spectr's existing CMake
configure command. Both checkout overrides are canonicalized before the script
changes directory, so relative `PULP_DIR`/`SPECTR_DIR` values remain valid. Build
output, failure handling, and artifact paths are unchanged.

## Evidence

- `bash -n tools/import-validation/spectr-roundtrip.sh` — PASS.
- `python3 tools/ci/test_build_governance_paths.py` — 6/6 tests PASS.
- Governed dry-run:
  `PULP_BUILD_JOBS=1 bash tools/ci/governed-build.sh --dry-run cmake --build /Users/danielraffel/Code/spectr/build --config Release`
  — PASS; receipt reports the bounded wrapper command.
- `rg` over `tools/import-validation/*.sh` finds no raw build command carrying
  `-j` or `--parallel`; every actual build command is governed.

A full Spectr build was not run in this receipt because the external Spectr
checkout/build is an integration dependency, not present in this Pulp
worktree. The change is intentionally limited to routing and is covered by the
static governance and shell syntax checks.
