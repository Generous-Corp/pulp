# SignalGraph lifecycle Wave 2C post-extraction receipt

This receipt records the refreshed post-extraction ownership map and validation
evidence for Wave 2C. It is intentionally limited to the SignalGraph lifecycle
seam; the public `SignalGraph` source API, RCU/reader-pin discipline, optimized
executor, and `signal_graph_reference_walk.cpp` remain separate contracts.

The branch was refreshed from `origin/main` at
`eabe6871abbe737caf3e05309202917436ef137c`. The post-extraction head is
`3abc36840c19a88e7b302befaac7729f3ca71a4c`. The pre-refresh head
`52d7cd2fe4c9072da5f83bab11eb80997cb44786` is preserved locally as
`backup/signal-graph-wave2c-pre-refresh-20261003`.

## Ownership baseline

| Surface | Current owner | Contract for this extraction |
| --- | --- | --- |
| Authored nodes, connections, registries, and mutation generation | `SignalGraph` authoring state guarded by `graph_mutation_mutex_` | Control-thread only; mutators invalidate the live snapshot except an owner-controlled live-swap transaction. |
| Compile/prepare and generated-graph validation | `SignalGraph::compile_`, `preflight_locked_`, `prepare_impl_`, and `prepare()` in `signal_graph_prepare.cpp` | Build off the live audio snapshot, run lifecycle callbacks under the mutation lock, and publish only a complete candidate. |
| Publication and reader lifetime | `runtime::Slot<CompiledGraph> live_slot_` plus `ExecutionSnapshot` pins | Audio readers pin immutable snapshots; retirement waits for readers; failed preparation leaves the graph silent or preserves the admitted live swap. |
| Live swap | `signal_graph_live_swap.cpp` | Remains a separate transaction seam; extraction does not move or merge it with the optimized executor. |
| Optimized routed execution | `signal_graph_executor_routing.cpp` and `GraphRuntimeExecutor` | Remains independent from the legacy reference walk. |
| Parity oracle | `signal_graph_reference_walk.cpp` | Stays independent and unchanged by this packet. |

The extraction moves only the private compile/prepare/limits implementation into
`signal_graph_prepare.cpp`; it changes no declarations, public headers, ABI, or
test ownership.

## Post-extraction measurements

Fresh post-refresh worktree: `codex/signal-graph-lifecycle-20261003` from the
current `origin/main` above.

| Artifact | Lines / result |
| --- | ---: |
| `origin/main:core/host/src/signal_graph.cpp` | 4,254 |
| `core/host/src/signal_graph.cpp` after extraction | 2,783 |
| `core/host/src/signal_graph_prepare.cpp` | 1,528 |
| Extracted source total | 4,311 |
| `core/host/include/pulp/host/signal_graph_runtime.hpp` | 1,716 |
| `test/test_host_signal_graph.cpp` | 5,295 |
| `test/test_signal_graph_executor_parity.cpp` | 3,303 |
| Focused CTest selection | 178 passed, 0 failed |
| Release host Catch binary | 129 cases, 11,910 assertions, 0 failed |
| Release executor parity Catch binary | 62 cases, 48,378 assertions, 0 failed |

Commands:

```bash
pulp build --target pulp-test-host-signal-graph
pulp build --target pulp-test-signal-graph-executor-parity
ctest --test-dir build -C Release -R \
  'SignalGraph|Translated routing|Parallel routing|Standalone translated routing|Lowerable Custom|Routed PDC' \
  --output-on-failure
build/test/pulp-test-host-signal-graph --reporter compact
build/test/pulp-test-signal-graph-executor-parity --reporter compact
```

The Release build reports `CMAKE_BUILD_TYPE:STRING=Release`; the governed Ninja
compile command for `signal_graph_prepare.cpp` contains `-O3 -DNDEBUG`.

## Dedicated post-extraction TSan evidence

The dedicated TSan tree was configured after the refresh with GPU disabled and
the SignalGraph host target built through the governed build wrapper:

```bash
cmake -S . -B build-tsan-wave2c \
  -DCMAKE_BUILD_TYPE=Debug -DPULP_SANITIZER=thread \
  -DPULP_BUILD_TESTS=ON -DPULP_BUILD_EXAMPLES=OFF -DPULP_ENABLE_GPU=OFF
tools/ci/governed-build.sh cmake --build build-tsan-wave2c \
  --target pulp-test-host-signal-graph
TSAN_OPTIONS="halt_on_error=1:history_size=7:suppressions=$PWD/test/tsan.supp" \
  build-tsan-wave2c/test/pulp-test-host-signal-graph --reporter compact
```

Result: **11,910 assertions in 129 test cases passed; no TSan report**. The
instrumented executable is 32,871,256 bytes with SHA-256
`807188ae04e6efc425753f5a96297a3e7bad39c48e04bd04d47c420274895223`.

## Adversarial scope review

The refreshed diff contains exactly these paths:

```text
core/host/CMakeLists.txt
core/host/src/signal_graph.cpp
core/host/src/signal_graph_prepare.cpp
docs/reports/signal-graph-lifecycle-wave2c-baseline-20261003.md
```

The review found no public-header, test-source, generated-manifest, workflow,
or planning-roadmap changes. `core/host/CMakeLists.txt` only registers the new
private translation unit. The moved definitions are lifecycle/compile/prepare
and generated-graph-limit methods; live-swap, optimized routed execution, and
`signal_graph_reference_walk.cpp` remain in their existing translation units.
`git diff --check` is clean. This packet makes no product-performance or merge
acceptance claim beyond the executable and sanitizer results recorded above.
