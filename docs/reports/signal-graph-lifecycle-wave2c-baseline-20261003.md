# SignalGraph lifecycle Wave 2C baseline and ownership contract

This receipt records the pre-extraction ownership map and validation baseline for
Wave 2C. It is intentionally limited to the SignalGraph lifecycle seam; the
public `SignalGraph` source API, RCU/reader-pin discipline, optimized executor,
and `signal_graph_reference_walk.cpp` remain separate contracts.

## Ownership baseline

| Surface | Current owner | Contract for this extraction |
| --- | --- | --- |
| Authored nodes, connections, registries, and mutation generation | `SignalGraph` authoring state guarded by `graph_mutation_mutex_` | Control-thread only; mutators invalidate the live snapshot except an owner-controlled live-swap transaction. |
| Compile/prepare and generated-graph validation | `SignalGraph::compile_`, `preflight_locked_`, `prepare_impl_`, and `prepare()` | Build off the live audio snapshot, run lifecycle callbacks under the mutation lock, and publish only a complete candidate. |
| Publication and reader lifetime | `runtime::Slot<CompiledGraph> live_slot_` plus `ExecutionSnapshot` pins | Audio readers pin immutable snapshots; retirement waits for readers; failed preparation leaves the graph silent or preserves the admitted live swap. |
| Live swap | `signal_graph_live_swap.cpp` | Remains a separate transaction seam; extraction must not move or merge it with the optimized executor. |
| Optimized routed execution | `signal_graph_executor_routing.cpp` and `GraphRuntimeExecutor` | Remains independent from the legacy reference walk. |
| Parity oracle | `signal_graph_reference_walk.cpp` | Stays independent and unchanged by this packet. |

The first bounded extraction moves only the private compile/prepare/limits
implementation into `signal_graph_prepare.cpp`; it does not change declarations,
public headers, ABI, or test ownership.

## Baseline measurements

Fresh worktree: `codex/signal-graph-lifecycle-20261003` from
`origin/main` at `c3c37bfc05fd58686dcc0dc369e4aaff9f4ccb06`.

| Artifact | Baseline |
| --- | ---: |
| `core/host/src/signal_graph.cpp` | 4,254 lines |
| `core/host/include/pulp/host/signal_graph_runtime.hpp` | 1,716 lines |
| `test/test_host_signal_graph.cpp` | 5,295 lines |
| `test/test_signal_graph_executor_parity.cpp` | 3,303 lines |
| Focused CTest selection | 178 passed, 0 failed |
| Host Catch binary | 129 cases, 11,910 assertions, 0 failed |
| Executor parity Catch binary | 62 cases, 48,378 assertions, 0 failed |

Commands:

```bash
pulp build --target pulp-test-host-signal-graph
pulp build --target pulp-test-signal-graph-executor-parity
ctest --test-dir build -C Release -R \
  'SignalGraph|Translated routing|Parallel routing|Standalone translated routing|Lowerable Custom|Routed PDC' \
  --output-on-failure
./build/test/pulp-test-host-signal-graph
./build/test/pulp-test-signal-graph-executor-parity
```

No TSAN run is claimed in this receipt; the existing lifecycle characterization
contains race-focused cases, but a dedicated post-extraction TSAN lane remains
required before merge.
