# G-9 checkpoint recovery wired into a running graph — 2026-11-13

Scope: one bounded slice of issue 12177 (item 11). A terminal isolated CLAP
node in a prepared `SignalGraph` returns to service from its last accepted
checkpoint while the callback keeps running. Base: `origin/main`
`250be0889974`. Platform: arm64 macOS (MacClapStereo64V1 only).

Status for this item: **IMPLEMENTED, functional evidence only.** No realtime
qualification, Windows/Linux, mid-stream checkpoint or DSP-history carry is
claimed.

## Before this slice

- A killed worker latches its node to terminal silence
  (`core/host/src/isolated_plugin_slot.cpp`, `IsolatedSlot::process_request`).
- `recover_isolated_clap()` built a fresh generation, but nothing could put it
  back into a running graph:
  - every live-swap entry point refuses a graph holding an isolated proxy
    (`core/host/src/signal_graph_live_swap.cpp:77`, `:442`, `:484` at base);
  - prepared topology edits refuse too
    (`core/host/src/signal_graph_prepared_topology_edit.cpp:815`);
  - eager `prepare()` refuses an isolated graph that already has a live
    snapshot (`core/host/src/signal_graph_prepare.cpp:1588`).

## Design decision: a dedicated recovery call that reuses the publication tail

Extending `stage_plugin_replacement()`/`prepare_swap()` by lifting the
isolated refusal was rejected as the larger change. That path would need an
isolated-specific exception at each of these points (base line numbers):

- it loads the replacement in-process from the scan catalog
  (`signal_graph_live_swap.cpp:270`, `PluginSlot::load`);
- it reads `save_state()` from the old instance (`:161`) and restores into the
  new one (`:206`), which isolated proxies refuse by design and latch terminal
  (`isolated_plugin_slot.cpp:260`, `:264`);
- it loads a second throwaway probe instance for warm-up (`:196`), and the
  isolated registry holds only two reservations (`isolated_plugin_slot.cpp:125`);
- warm-up drives 16 back-to-back control-thread blocks through the proxy,
  which the paced endpoint would treat as deadline faults;
- admission requires load history for the staged node (`:536`);
- every failure path invalidates the live snapshot (`:425`), which silences
  the siblings that recovery must keep running.

The chosen shape keeps the live-swap path unchanged for ordinary plugins:

- `recover_isolated_graph_node()` (`core/host/include/pulp/host/isolated_plugin_slot.hpp`,
  implemented in `core/host/src/isolated_plugin_slot.cpp`) owns the isolation
  checks and the worker spawn. It runs on the control thread, pins the live
  snapshot, and returns a typed `IsolatedNodeRecovery`.
- `SignalGraph::publish_isolated_recovery()` (`signal_graph_runtime.hpp:584`,
  `signal_graph_live_swap.cpp:793`) prepares the fresh proxy and publishes
  it. It reuses the live-swap reinit-free predicate, the `SwapNoAnticipation`
  compile and one `live_slot_.publish()`. Every refusal only records
  `last_swap_diagnostics()` and leaves the live snapshot published.
- The post-compile latency/feedback/routed-domain/PDC adoption checks moved out
  of `prepare_swap()` into `adopt_live_snapshot_state()`
  (`signal_graph_live_swap.cpp:60`, file-local). Both paths call it. It is a
  template over the private snapshot type, so `signal_graph_runtime.hpp` gains
  no private declaration and stays line-neutral (2133 lines before and after;
  one public declaration, with comments reflowed to make room).
- Orchestration lives in `isolated_plugin_slot.cpp` because the web
  sample-region closure does not compile that file. The graph-side method calls
  only `is_isolated_plugin_slot()`, which already has a web definition.
- A recovered generation records the checkpoint it was restored from as its
  accepted checkpoint, so a node that faults again can recover again. Without
  this, each recovery would need a fresh checkpoint, and `pause()` is terminal
  for a node the graph is still rendering.

Typed refusals, in evaluation order: `not_isolated_node`, `control_mismatch`,
`not_terminal` (never preempts a healthy worker), `identity_mismatch` (plugin
descriptor or plugin SHA-256), `no_checkpoint`, `cleanup_incomplete`,
`worker_failed` (carrying the worker `IsolationReason`, for example
`launch_failed`), and `publication_refused`. In every one of them the old
silent node stays published.

Callback behavior: no new audio-thread work. The callback pins either the old
snapshot, where the terminal proxy renders silence without IPC, or the new one.
The displaced snapshot is reclaimed on the control thread by the existing
`runtime::Slot` retire path.

## Tests (`test/test_isolated_clap_worker.cpp`)

| Test | Proves |
|---|---|
| `Isolated CLAP terminal graph node recovers from its checkpoint` | The state fixture renders at 0.5 and checkpoints 0.75. After SIGKILL the node is silent. After recovery it gives one latency block of silence, then exactly 0.75 x the previous input, so the parameter state came from the checkpoint. The built-in gain sibling (PDC 64) and a healthy isolated sibling are bit-exact on every block. New generation and PID; the old control stays `worker_exited`. |
| `Isolated CLAP graph recovery refusals keep the silent node published` | `not_isolated_node`, `control_mismatch`, `not_terminal` (the node keeps rendering 0.5 afterwards), `identity_mismatch` for both descriptor and SHA, and `worker_failed`/`launch_failed` from a mismatched worker. Graph-level refusals: not terminal, and a different-identity proxy (`UntrustedIdentity`). After every refusal the same slot stays published and silent with the sibling exact, and a correct recovery still succeeds afterwards. |
| `Isolated CLAP graph recovery without a checkpoint refuses` | `no_checkpoint`; the old node stays published. |
| `Isolated CLAP graph recovery races a running callback` | Three kill and recover cycles while a separate thread runs paced `graph.process()` under `ScopedRtProcessProbe`. Zero sibling mismatches; every node block is silence, 0.5 x prior or 0.75 x prior; recovered output resumes after each cycle (at least 9 recovered blocks); retired controls are terminal. The run is bounded to at most 800 blocks. |

Results on this host (Release): `pulp-test-isolated-clap-worker` passed all
24 cases in each of 3 full runs. The four recovery cases passed 25 of 25
repeated runs. The live-swap suites in `pulp-test-group-sampler-host-graph`,
which cover the refactored `prepare_swap()`, passed all 131 cases.

### confirm_failure.sh verdicts

| Guard broken | Test | Verdict |
|---|---|---|
| `recover_isolated_graph_node` not-terminal check (`if (false)`) | refusals | CONFIRMED |
| `publish_isolated_recovery` `terminal.is_loaded()` check | refusals | CONFIRMED |
| `publish_isolated_recovery` `live_slot_.publish(next)` removed | recovers-from-checkpoint | CONFIRMED |
| `publish_isolated_recovery` PDC adoption skipped | recovers-from-checkpoint | CONFIRMED |

## Not established

- Checkpointing a node while the graph is rendering it: `pause()` still
  latches the node terminal, so the checkpoint has to be taken before the node
  renders. A recovered generation carries its restore checkpoint forward.
- Carrying DSP history: recovery restores preset state only.
- Fading the recovered node in: it switches instantly after its latency
  block, and the node was already silent.
- Recovery as an automatic supervisor policy: the host must call
  `recover_isolated_graph_node`.
- Realtime qualification, Windows/Linux process-tree parity, host death.
