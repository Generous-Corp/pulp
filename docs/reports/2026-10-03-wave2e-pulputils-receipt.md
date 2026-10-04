# Wave 2E PulpUtils compatibility-shim receipt

Branch: `codex/wave2e-pulputils-20261003`  
Base: `c3c37bfc05fd58686dcc0dc369e4aaff9f4ccb06`

## Slice

`tools/cmake/PulpUtils.cmake` remains the stable compatibility entry point and
now includes the private `tools/cmake/PulpPlugin.cmake` module. The module owns
`pulp_add_plugin()` and `pulp_add_plugin_bundle()`. Public helper names,
format dispatch, generated target names, install behavior, and plugin runtime
manifest calls are unchanged. `PulpPlugin.cmake` still supports direct legacy
inclusion by bootstrapping through `PulpUtils.cmake` and emits the existing
deprecation message.

`test/cmake/test_au_v2_type_selection.cmake` was updated only so its static
contract check reads the newly private plugin module; the behavior contract is
unchanged.

## Baseline manifest

Captured before edits at commit `c3c37bfc05fd58686dcc0dc369e4aaff9f4ccb06`:

- `PulpUtils.cmake`: 839 lines, SHA-256
  `095b2bf1d57c3a0b7e29222e77510d35eb4102bf3554313ab3122bf9a8b74ba5`.
- Public/private function names: `_pulp_pick_target`,
  `_pulp_apply_view_mac_objc_suffix`, `pulp_add_plugin`,
  `pulp_add_plugin_bundle`.
- Relative helper includes: 15; the compatibility install list already
  contained `PulpPlugin.cmake`.
- Configured CTest inventory: 1,811 discovered cases; the CMake compatibility
  case is `cmake-pulp-utils-compat` with labels `cmake;sdk;compat`.

After the split, `PulpUtils.cmake` is 282 lines (SHA-256
`10a26ffbca110340f3620a5e89f71cc7dea247c8a24dc9de691f8de22660735`) and
`PulpPlugin.cmake` is 548 lines (SHA-256
`350420b47396b882fa4bb432c9a472a088567e7b84a0e886e8f68b86fa548bac`).

## Validation

- Release Ninja configure in `build-wave2e`: passed.
- Direct `PulpUtils.cmake` consumer probe: passed; all five target aliases and
  reload/plugin commands were present.
- Direct `PulpPlugin.cmake` probe: passed; both plugin commands were present and
  the legacy deprecation warning was preserved.
- Clean post-split Release/Ninja configure in `build-wave2e-cleanprobe`: passed;
  generated plugin target declarations and the normalized CMake install helper
  manifest matched the baseline. The install helper set remained
  `PulpUtils.cmake`, `PulpUiAssets.cmake`, `PulpInstall.cmake`,
  `PulpReload.cmake`, and `PulpPlugin.cmake`.
- `cmake-au-v2-type-selection`: passed after moving its static option scan to
  `PulpPlugin.cmake`.
- `cmake-plugin-runtime-manifest-layout`: passed.
- `tools/scripts/gates.sh origin/main`: passed; the two pre-existing
  scene3d-native-slice-handoff selftest failures were classified as base
  failures by the gate.

The installed-SDK smoke test was run once against the untouched baseline and
once against this worktree after contention eased and the missing external
archives/broker were built through `tools/ci/governed-build.sh`. Both runs fail
in the full-tree install before the consumer configure:

- Baseline first failure: missing
  `build-wave2e-baseline/_deps/mbedtls-build/3rdparty/everest/libeverest.a`.
- Worktree rerun first failure: missing
  `build-wave2e/tools/audio/pulp-au-instrument-probe`.

These are pre-consumer install artifacts, so they classify as baseline/build
environment failures rather than compatibility-shim behavior. The exact logs
were captured during validation; no production, CLI, Android, SignalGraph, or
roadmap files were changed.
