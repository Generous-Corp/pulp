# A10 — routing and hosting environment

Status: **BLOCKED — exact-head installed consumer cannot be configured**

Current `origin/main` is `82a3d1606c715318b69825486b38525077414532`.
The required graph dependencies are present at exact merged heads:

- G-1 named typed ports: PR [#11428](https://github.com/Generous-Corp/pulp/pull/11428), source head `be70e3c44836605ac1279c65a572e833699e1fbb`, merge `e19f556e67129117ef3a4587bff11fdee16ebf44`.
- G-9 process-group cleanup: PR [#11667](https://github.com/Generous-Corp/pulp/pull/11667), source head `433d47e2413450323376cbf0b10e6e5ce28e6a2f`, merge `89c9be945120b9553f2983d265677292d4876caa`.

Both merge commits are ancestors of the current main head. Open graph PRs
#11679 (R2 evidence), #11680 (MPE sidecars), and #11682 (A9) do not own this
A10 lane.

## Public API audit

The current public host API is sufficient to exercise these bounded surfaces in
an installed consumer once an exact-head SDK is available:

| Surface | Public API | Evidence boundary |
| --- | --- | --- |
| Named buses / sidechain | `SignalGraph::connect_sidechain`, `PortHandle`, `PluginInfo::num_inputs`, and `forge_sidechain_catalog.hpp` | A real `PluginSlot` with a declared second input can be routed and inspected; this does not prove every format's bus negotiation. |
| Automation | `SignalGraph::connect_automation` and `ParameterEventQueue` | The host maps source samples into the plugin parameter domain; format-specific event delivery remains separate. |
| PDC | `SignalGraph::prepare`, `latency_samples`, `prepared_latency_snapshot`, and `PluginSlot::latency_query` | A reported latency is accepted only when the slot reports `LatencyQuery::Available`; unsupported/query-failed latency must remain a refusal. |
| Transactional edits | `SignalGraph::begin_swap_edit` / `prepare_swap` and `begin_prepared_topology_edit` / `commit` | Publication and `NeedsEagerPrepare`/stale-base refusals can be checked without claiming realtime continuity. |
| Hosted nodes | public `PluginSlot` lifecycle, `SignalGraph::add_plugin_node`, and typed hosted-editor methods | An in-process test slot can prove host wiring; arbitrary installed plugin loading requires an exact SDK and fixture artifact. |
| Hostile-plugin policy | `GraphLoadOptions` with plugin loading disabled, `IsolatedPluginScanner`, and G-9 process-group helper | The current evidence does **not** prove arbitrary hosted-plugin containment, descendant cleanup on every platform, callback IPC, restart/state recovery, or sibling continuity. Keep this policy HOLD/N/A. |

## Exact blocker and next safe slice

The exact-head installed SDK for `82a3d1606c715318b69825486b38525077414532`
is not present in the local SDK-dev store. A bounded probe from this fresh
worktree also found no `./build/pulp` CLI. The forge-dev SDK installer is known
to refuse a dirty/non-clean checkout, so creating a consumer against an older
SDK would produce stale provenance and is not admissible as A10 evidence.

Next safe slice: run the SDK installer from a clean, exact-`origin/main`
checkout, then add `examples/a10-routing-hosting-consumer` using only the
public APIs above. The consumer must include a positive sidechain/automation/
PDC/transactional-hosted-node path and a planted refusal control for unavailable
latency or hostile containment. Do not promote the hostile fixture to PASS until
platform descendant cleanup, callback safety, recovery, and sibling continuity
are separately proven.
