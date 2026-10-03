# SignalGraph lifecycle contract receipt (Phase 1 / A3)

This receipt records the lifecycle and realtime invariants exercised by the
existing deterministic graph test lanes. It is intentionally a contract map,
not a second implementation description. The tests remain the evidence.

## Scope and ownership

This packet changes only graph tests/manifests and this receipt. It does not
move production code, change public headers, or combine the optimized executor
with `signal_graph_reference_walk.cpp`.

## Invariant map

| Invariant | Contract status | Deterministic evidence |
| --- | --- | --- |
| Mutation is candidate-local until prepare succeeds and publication commits atomically. Failed prepares roll back exactly once; stale edits cannot publish. | Contractual | `test/test_host_signal_graph.cpp`: “prepared edit keeps owned built-in additions candidate-local until commit”, “prepared edit rolls back a failed owned built-in prepare exactly once”, “stale prepared edit releases its owned built-in exactly once”, “prepared edit publishes old or new topology without silence”. |
| A published snapshot remains valid for every in-flight reader; retirement and owned-instance destruction wait for reader drain. | Contractual | `test/test_host_signal_graph.cpp`: “release waits for in-flight snapshot process”, “control-thread snapshot readers pin against retirement”, “owned built-in removal and replacement wait for the old execution snapshot”; `test/test_signal_graph_live_swap_staging.cpp`: “live plugin swap staging retires the old slot only after reader drain”. |
| Live swaps preserve audio continuity where the edit is admissible and reject edits that cannot be staged without disturbing the live graph. | Contractual | `test/test_signal_graph_live_swap_staging.cpp`: warmed identity commit, over-budget refusal, default-off refusal, prepare fallback, and abort; `test/test_signal_graph_prepared_swap_live.cpp`: reinit-free publication and `NeedsEagerPrepare`; `test/test_host_signal_graph.cpp`: old/new topology publication without silence. |
| Sample-region admission is deterministic and fail-closed: proof order, limits, explicit delayed feedback, exact kernel identity, and boundaries are part of the contract. Reset starts fresh; only exact state identities are retained across adoption. | Contractual | `test/test_sample_region_plan.cpp`, `test/test_sample_region_proof.cpp`; `test/test_sample_region_runtime.cpp`: state-bank exact-key retention, changed-kernel rejection, reset hooks, pinned-snapshot reset, quotient prepare/commit, stale-generation rejection, and full-prepare reset. |
| Transport-sensitive nodes receive the declared transport consistently across block, plugin, parallel, anticipation, and custom-node paths. | Contractual | `test/test_signal_graph_transport.cpp`: transport-inert identity, block population, process mode/render hint, opted-in plugin/custom, parallel, non-opt-in byte identity, and anticipation cases. |
| A failed or invalid runtime fails closed to silence and never exposes stale output: failed prepare, oversized blocks, invalid sizes, disconnected outputs, and invalid ports are covered. | Contractual | `test/test_host_signal_graph.cpp`: “prepare failure leaves process output silent”, “process silences oversized blocks”, “process ignores non-positive block sizes”, “disconnected output stays silent”, and invalid-port connection cases. |
| Telemetry is opt-in, bounded, and allocation-free on the audio path; node load/fallback counters describe the selected execution path. | Contractual | `test/test_live_dsp_telemetry_graph.cpp`: disabled default, reference-walk recording, live toggle, and no allocation; `test/test_host_signal_graph.cpp`: `node_loads()` on walk/canonical/parallel paths and routed-walk fallback accounting. |
| Custom-node state and lifecycle callbacks are balanced across prepare, rollback, stale abandonment, publication, and executor translation. | Contractual | `test/test_host_signal_graph.cpp`: custom lifecycle/state round-trip, factory failure, generated-state reprepare, version/shape checks, registration invalidation, rollback and abandonment cases; `test/test_signal_graph_executor_parity.cpp`: stateful, stateless, partial-writing, unresolved, latency-reporting, and event-aware custom-node parity. |
| The canonical executor is an independent translation whose output and event behavior match the reference walk for its admitted subset, including feedback, PDC, MIDI, automation, sidechain, custom nodes, and block partitioning. | Contractual claim; implementation details remain observations | `test/test_signal_graph_executor_parity.cpp` (serial/parallel audio, MIDI, sparse/dense automation, PDC, sidechain, feedback, custom and event-aware nodes, allocation probes, reprepare race); `test/test_signal_graph_audio_parity.cpp`; `test/test_signal_graph_offline_parity.cpp`. |

The exact snapshot/container types, lock-free primitive choices, and callback
ordering beyond these externally observed guarantees are implementation
observations. Future lifecycle extraction must preserve the contractual rows
and re-run the receipt commands below.

## Validation commands

From this worktree, in Release configuration and through the governed Pulp
build path:

```bash
pulp build --target pulp-test-host-signal-graph
pulp build --target pulp-test-signal-graph-executor-parity
pulp build --target pulp-test-sample-region-runtime
pulp build --target pulp-test-sample-region-authoring
pulp build --target pulp-test-signal-graph-transport
pulp build --target pulp-test-live-dsp-telemetry-graph
pulp build --target pulp-test-signal-graph-live-swap-staging
pulp build --target pulp-test-signal-graph-prepared-swap-live
ctest --test-dir build -C Release --output-on-failure -R \
  '^SignalGraph|^Translated routing|^Parallel routing|^Standalone translated routing|^Lowerable Custom|^Routed PDC'
```

Static checks for this packet are:

```bash
git diff --check
ctest --test-dir build -C Release -N > /tmp/pulp-a3-ctest-list.txt
```

The CTest list must contain the same graph test registrations as the base
checkout; this packet intentionally adds no test executable or CTest case.
The receipt is the only new artifact, so any enumeration delta is an
unintentional manifest change.

## Run receipt for this checkout

On 2026-10-02 the fresh protected origin/main worktree rebuilt both packet-owned
executables in Release. CTest discovered 120 SignalGraph cases and 56 routed
parity cases; both focused CTest runs passed. Direct totals were 11,910
assertions in 129 host cases and 48,378 assertions in 62 parity cases. The host binary emitted only the expected negative-path diagnostics
(factory/prepare/limit refusals).

The receipt intentionally adds no CTest case; the enumeration is inherited from origin/main and is recorded above.

Related lane receipts from the same fresh worktree also passed directly in
Release: sample-region runtime (113,184 assertions / 24 cases), sample-region
authoring (4,981 / 14), live-DSP telemetry (57 / 4), and the grouped host/
format/graph lifecycle lanes (46,570 / 98). The packet changes no test source or
manifest, so `git diff --name-only origin/main...HEAD` has no test or manifest
path and the CTest enumeration has no intentional additions.

Release proof: `build/CMakeCache.txt` reports `CMAKE_BUILD_TYPE:STRING=Release`,
and the governed compile commands for both packet-owned sources contain `-O3`
and `-DNDEBUG`. CTest enumeration after all focused targets were built reports
167 SignalGraph-named cases and 31 translated/parallel/custom routed-parity
cases; the direct binary totals above include the complete Catch suites.
