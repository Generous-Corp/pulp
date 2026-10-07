# Forge Wave 2B space baseline

Captured from `origin/main` at `c3c37bfc05fd58686dcc0dc369e4aaff9f4ccb06` before the space-family extraction.

The extraction was replayed onto current `origin/main` at
`62a063a2c6b6a300ada92ecc645c40b73d835b8e` before this receipt was revalidated.

- Public header: `core/host/include/pulp/host/forge_space_catalog.hpp`
- Header size: 52,728 bytes, 925 lines.
- Direct include consumers: 3 source/test files.
- Export snapshot: `forge-wave2b-space-baseline-catalog.json` (415,732 bytes).
- Export SHA-256: `350567a6c9ca5a6b981f0efe8354207479f3341765871d7af4a821fc094c3e46`.
- The snapshot is copied byte-for-byte from `docs/status/forge-catalog.json`; it is the comparison artifact for the post-extraction export.
- Space catalog rows present in the snapshot: convolution reverb, nonlin ambience, and speaker cabinet.

## Post-extraction evidence

- Public header after extraction: 6,716 bytes, 160 lines; implementation: 42,396 bytes, 799 lines; private descriptor: 438 bytes, 14 lines.
- Direct include consumers remain 5 across source and tests; the implementation is now compiled as `core/host/src/forge_space_catalog.cpp` through `core/host/CMakeLists.txt`.
- Governed `pulp-host` compile passed, followed by governed `pulp-test-forge-space-catalog` link.
- Direct focused binary `build/test/pulp-test-forge-space-catalog --reporter compact`: 19 cases, 3,374 assertions, all passed.
- Export after extraction: 415,732 bytes, SHA-256 `350567a6c9ca5a6b981f0efe8354207479f3341765871d7af4a821fc094c3e46`; `cmp` against the baseline returned 0.
- `dsp_capability_registry.py --check` and `consumption_census.py --check` passed.
- No compile-time improvement claim is made yet; a before/after include-time
  measurement remains a follow-up rather than inferred from the line-count
  reduction.
- The new out-of-line implementation translation unit reaches the existing
  `load_measurer.hpp` closure once; this is an intentional implementation-TU
  edge, not a public-header include.
- The compatibility surface retains the previously nameable `Instance` and
  `GpuInstance` types, `gpu_internal_block_size`, and
  `nonlin_ambience::forward_if_changed`; the dedicated compatibility suite
  covers those declarations and helper semantics.
