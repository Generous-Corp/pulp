# SignalGraph authoring extraction and D1 TSan refresh (2026-10-15)

This packet moves control-thread node and connection authoring methods from
`signal_graph.cpp` to the private `signal_graph_authoring.cpp` translation unit.
It does not change the public headers, serialization, prepare/live-swap policy,
optimized executor, or independent reference walk. The moved methods retain
their `GraphMutationLock` and `invalidate_live_locked_()` ordering.

## Provenance and ownership

- Original Release and TSan validation base: `origin/main` at
  [`bcde1146d8d82c932d71b47037cfc4e7fe83a019`](https://github.com/Generous-Corp/pulp/commit/bcde1146d8d82c932d71b47037cfc4e7fe83a019).
- PR rebase base: `origin/main` at
  [`666157bda2acaee99fa7c667c20af774040ad0a3`](https://github.com/Generous-Corp/pulp/commit/666157bda2acaee99fa7c667c20af774040ad0a3).
- Validation used an isolated worktree at the protected base.
- `signal_graph.cpp` shrank from 2,413 to 1,737 lines; the new private TU is
  725 lines after repository formatting.
- The existing `signal_graph_prepare.cpp`, `signal_graph_live_swap.cpp`,
  `signal_graph_executor_routing.cpp`, and `signal_graph_reference_walk.cpp`
  remain separate and unchanged.

## Release validation

The focused host, parity, and grouped graph targets built through
`tools/ci/governed-build.sh cmake --build build --target ...` in Release.
`pulp-test-host-signal-graph` passed 11,977 assertions in 136 cases;
`pulp-test-signal-graph-executor-parity` passed 48,742 assertions in 63 cases.
Focused CTest discovery and execution for `SignalGraph|Translated routing|Parallel
routing|Standalone translated routing|Lowerable Custom|Routed PDC` both found
207 cases; all 207 passed. This includes the prepared-edit and live-swap cases
registered in the grouped graph binary.

After rebasing onto the PR base above, the three focused targets rebuilt in
Release through the governed wrapper. The full host and parity binaries passed
the same 11,977/136 and 48,742/63 assertion/case totals; the sample-region
sequencer binary passed 250 assertions in 10 cases, including its source
authority check against the moved authoring method.

## Dedicated Debug/TSan refresh

The TSan tree used `Debug`, `PULP_SANITIZER=thread`, tests ON, examples OFF, and
GPU OFF. Both binaries built through the governed wrapper. Each run used
`TSAN_OPTIONS=halt_on_error=1:history_size=7:suppressions=$PWD/test/tsan.supp`.
No ThreadSanitizer report was emitted.

| Selection | Result |
| --- | ---: |
| Full host graph binary | 11,977 assertions / 136 cases passed |
| Full executor parity binary | 48,742 assertions / 63 cases passed |
| Host `[race]` | 65 assertions / 9 cases passed |
| Host `[live-swap]` | 129 assertions / 4 cases passed |
| Parity `[threads]` | 416 assertions / 3 cases passed |

The refreshed suites include the later scalar-status, pinned routed-status,
and standalone routing-publication regression tests added after the earlier D1
receipt. This is deterministic sanitizer evidence on the listed base and diff,
not a claim that all possible thread schedules were explored.

| Binary | Size | SHA-256 |
| --- | ---: | --- |
| `build-tsan-d1-current/test/pulp-test-host-signal-graph` | 33,446,584 bytes | `f46d9234bab4cce2c2245d88eb711e93d1022af6103c29e7cffaec3ef259fbb8` |
| `build-tsan-d1-current/test/pulp-test-signal-graph-executor-parity` | 27,465,432 bytes | `3e1aa08a187a7b347405bc891bf3145b0d447ac9fe77fdd76a4ddf2811dabf84` |
