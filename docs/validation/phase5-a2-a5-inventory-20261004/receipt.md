# Phase 5 A2/A5 inventory receipt (2026-10-04)

This is a read-only inventory from a fresh worktree based on `origin/main`.
It records the A2 baseline ledger inputs and A5 deprecation/generated-file
inventory needed before Phase 5 ownership diagrams, test trend checks, and
retirement decisions. No production or test source was edited.

## Provenance and commands

- Worktree: `/Users/danielraffel/Code/pulp-phase5-inventory-20261004`
- Branch: `codex/phase5-inventory-20261004`
- `origin/main` and `HEAD`: `dcee90cce925d42dca420284f22da7d279369c30`
- Lineage: `tools/scripts/worktree_lineage.sh show --path .`; marked active with the Phase 5 inventory note.
- Guidance read: `/Users/danielraffel/Code/pulp-phase5-inventory-20261004/CLAUDE.md`.
- Baseline commands:
  - `python3 tools/scripts/header_fanout_guard.py --top 40`
  - `wc -l` over Forge catalog headers, SignalGraph headers/sources, `tools/cmake/*.cmake`, and test sources.
  - A Python read-only scanner over tracked C/C++/CMake files to count direct textual include users, test macros, deprecation/generated markers, and SHA-256 hashes of key manifests. Its complete output is `metrics.json`.
  - `rg -n "pulp_add_test_suite|add_test\\s*\\(" test/CMakeLists.txt test/cmake forge-seam -g '*.cmake' -g 'CMakeLists.txt'`
  - `python3` JSON shape/count inspection of `test/ctest_script_inputs.json`.

## A2 baseline observations

- Forge catalog headers are large: `forge_lofi_catalog.hpp` 1,465 lines,
  `forge_space_catalog.hpp` 925, `forge_effect_modulation_catalog.hpp` 900,
  `forge_sequencing_catalog.hpp` 855, `forge_modulation_catalog.hpp` 827,
  and `forge_synthesis_catalog.hpp` 768. The generated example module header
  is 1,555 lines and should remain classified as generated/example output.
- SignalGraph production surface totals 10,840 lines across the listed public
  headers and `signal_graph*.cpp` sources. `signal_graph_runtime.hpp` is 1,716
  lines; `signal_graph.cpp` is 2,783; prepare/live-swap/prepared-topology
  sources are 1,528/983/1,476 lines respectively.
- CMake utility files total 19,407 lines. Largest utilities are
  `PulpInstallRules.cmake` (1,065), `PulpDependencies.cmake` (995),
  `PulpTestSuite.cmake` (727), `PulpAuv3.cmake` (647), and
  `PulpPluginFormats.cmake` (568).
- Largest test TUs include `forge-seam/test/test_chrome_no_leak.cpp` (10,000
  lines, 188 Catch macros), `test/test_design_import_cpp_codegen.cpp` (7,789,
  41), `test/test_inspector.cpp` (7,305, 168), `test/test_widget_bridge.cpp`
  (6,679, 176), and `test/test_host_signal_graph.cpp` (5,296, 99). Full list,
  line counts, macro counts, and hashes are in `metrics.json`.
- Static direct include scan reports 98 textual users of
  `signal_graph.hpp`, 8 of `signal_graph_runtime.hpp`, and 3–4 direct users
  for each selected Forge catalog header. The CMake include scan is not a
  transitive target graph; `PulpUtils.cmake` has four textual include users and
  `FindSkia.cmake` one. These counts are a baseline for follow-up measurement,
  not compile-time claims.
- The existing header guard reported 28 tracked headers within their ceilings;
  its top measured candidates are canvas headers at 929–969 TUs and
  `core/audio/include/pulp/audio/buffer.hpp` at 893 TUs. The full command output
  is preserved in the coordinator log; the guard's tracked ledger remains
  `tools/scripts/header_fanout_guard.json`.
- `test/ctest_script_inputs.json` has 389 named test entries, 117 executable
  entries, and 618 scanned executables. Source registration scan found 1,645
  `pulp_add_test_suite`/`add_test` occurrences across the selected CMake files;
  these are registration declarations, not discovered CTest case counts.

## A5 inventory observations

- The marker scan found 38 files containing `deprecated`/`PULP_DEPRECATED`
  markers and 490 files containing generated/codegen/do-not-edit markers. These
  are candidate inventories only; vendor files and generated fixtures are
  included and require ownership classification before any retirement.
- The highest-count deprecation marker file is vendored
  `tools/rack/vendor/zstd-1.5.7/zstddeclib.c` (32); other recurring callers
  include `test/test_cli_import_detect.cpp`, `core/dawproject/include/pulp/dawproject/dawproject_export.hpp`,
  `core/host/include/pulp/host/extensions_visitor.hpp`, and
  `tools/import-design/import_detect.cpp`.
- Generated marker hotspots are design-import codegen tests and generated event
  source/headers. `forge-seam/test/test_chrome_no_leak.cpp` contains generated
  markers and remains a 10,000-line corpus requiring case-identity
  classification before movement.
- Key authority/manifest inputs were hashed: `test/ctest_script_inputs.json`,
  `tools/scripts/header_fanout_guard.json`,
  `docs/status/agent-capability-surface.json`,
  `docs/status/agent-capabilities.json`, `.github/vellum-ownership.json`, and
  `.agents/contract.toml`. Exact byte sizes and SHA-256 values are in
  `metrics.json`.

## Ownership diagram inputs and limitations

The file paths and line counts above identify candidate ownership nodes for
Forge metadata versus executable catalog code, SignalGraph lifecycle seams,
CMake utility families, and giant test behavior partitions. The direct include
scan is intentionally textual and does not resolve generated includes,
compiler include paths, conditional branches, or target-level transitive
closures. No CMake configure was run in this fresh worktree, so no `ctest -N`,
`compile_commands.json`, `-ftime-trace`, or target compile-duration evidence is
claimed. A configured Release build should collect those measurements before
using this receipt to justify API/header changes or test movement.
