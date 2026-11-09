# A10 installed consumer — exact-main HOLD (2026-11-09)

Status: **HOLD — exact-main installed SDK consumer evidence is unavailable.**

This receipt is from a fresh worktree created from the live `origin/main` and
records the dependency and installation boundary. It does not change the A10
roadmap status or claim a runtime, provider, native-parity, or platform result.

## Exact source and dependency heads

| item | value |
| --- | --- |
| source checkout | `546051b66b44df141b7f0bec1217fa96e971022c` |
| source branch | `codex/a10-installed-consumer-refresh-20261109` |
| base | `origin/main` at `546051b66b44df141b7f0bec1217fa96e971022c` |
| merged dependency PR | `https://github.com/Generous-Corp/pulp/pull/11882` |
| dependency source head | `a5b71687d038ac809edcf9340708547219473187` |
| dependency merge commit | `2f6aa0d90421a24c9c092c1bc96ab2dd34ad8914` |
| observed at | `2026-11-09T22:24:18Z` |

PR #11882 is merged. Its branch was not checked out or modified by this
worktree.

## Source/configuration provenance

The fresh checkout was clean at creation. The public API files audited for the
planned consumer are the exact files below:

| file | SHA-256 |
| --- | --- |
| `core/host/include/pulp/host/signal_graph_runtime.hpp` | `19bd9625c2b4178c78bf55826f09645383e5d86e13b63830ebe58273daf7f412` |
| `core/host/include/pulp/host/signal_graph_prepared_topology_edit.hpp` | `2c4909390c39a83a2360a1dea23bf6ee1449276edf42949742e818dade300c8d` |
| `core/host/include/pulp/host/plugin_slot.hpp` | `69927dee51b495256394c0561f8e31be2abac5cf73dbd462a310565d637a92bb` |
| `core/host/include/pulp/host/signal_graph_connection.hpp` | `c48b57113f6d1b5e5fd76268cf82c8ec08d140c7b04e6b7baa9986dae0cb17c8` |
| `tools/validation/downstream/consumer-validation.json` | `6194e3526e5d323df8fd45b6f456ffe7f07912c38a532a93c7bb910dc0c8055d` |

The partial source configuration was created by the checkout CLI:

```text
build/pulp v1.94.0
CMAKE_BUILD_TYPE=Release
PULP_PROFILE=dev
PULP_ENABLE_GPU=ON
```

`build/CMakeCache.txt` SHA-256 is
`a6a70c4ea423fc6f245e660d5ccc4acc1bb17a38a02a6236a7e60f2382a4e05f` and the
checkout CLI SHA-256 is
`4f91518a28b42ad0b33755bc4853098f3c1da0918b7a9eeb11a7c25f64a4bb0a`.

## SDK state and attempted installation

`pulp sdk status` reported downloaded SDKs through v1.6.0, local SDKs through
v0.399.0, and forge-dev SDKs through v1.93.0. No SDK was identified for the
exact v1.94.0 source head. The newest inspected forge-dev provenance was:

```text
path=/Users/danielraffel/.pulp/sdk-dev/forge-v1/darwin-arm64/ea605ead95c9b38a94c17b5ffd37a030fa72081d/abe9402ee104
version.txt SHA-256=128131bfa30c0f153c93c3caab1b7e6b15de9647496c9bbb6525e2224886c7ff
sdk-provenance.json SHA-256=d1c562ca961bd6c5ea75dbabac583a37fb136f6815e9e469ed93b2d85cb8c4a5
```

The exact-main installation command was attempted with the checkout CLI:

```text
timeout 120s build/pulp sdk install --local --profile forge-dev --print-path
```

It completed source snapshot, dependency, Skia, Node, and SDK configure steps,
then exceeded the 120-second bound while building the SDK. No new
`sdk-provenance.json`, SDK `version.txt`, installed prefix, or consumer binary
was produced. The earlier governed `pulp build` was stopped at 232/1708 after
the build lease was denied (`capacity_exceeded`, fallback `-j2`).

## Public API scope audited for the next run

The current public headers expose the bounded surfaces required for a consumer:

- named audio and sidechain edges through `SignalGraph::connect_sidechain`;
- parameter automation through `SignalGraph::connect_automation` and
  `ParameterEventQueue`;
- prepared latency/PDC through `SignalGraph::prepare`,
  `prepared_latency_snapshot`, `latency_samples`, and `PluginSlot::latency_query`;
- transactional topology publication through
  `begin_prepared_topology_edit`, `prepare`, and `commit`;
- hosted node lifecycle through `PluginSlot` and
  `SignalGraph::add_plugin_node`.

These are source-audit facts only. The installed consumer has not been
configured, linked, run, or mutation-tested against an exact-main SDK.

## Explicit evidence boundaries

| lane | state | reason |
| --- | --- | --- |
| exact-main installed SDK provenance | **BLOCKED** | no completed SDK prefix or provenance receipt for `546051b66…` |
| installed consumer configure/build/run | **NOT CHECKED** | prerequisite SDK unavailable |
| buses/sidechain, automation, PDC, transaction, hosted node | **NOT CHECKED** | no consumer execution receipt |
| arbitrary plugin loading / hostile containment | **HOLD** | no fixture or platform descendant-cleanup proof |
| provider/runtime/native parity | **N/A** | this lane is source + installed SDK only |
| cross-platform / hard-realtime qualification | **N/A** | no platform or timing evidence was collected |

Next safe action: rerun the exact-main SDK installation with a bounded build
window and then configure one standalone consumer using only the public headers
listed above. Until a new prefix has a matching source hash and a clean
consumer run, A10 remains HOLD.
