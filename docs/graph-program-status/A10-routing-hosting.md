# A10 — routing and hosting environment

Status: **BLOCKED — exact-head installed consumer cannot be configured**

Current `origin/main` is `2c2164d2da39a32a12559d2cc1598aae2628440d`.
The required graph dependencies are present at exact merged heads:

- G-1 named typed ports: PR [#11428](https://github.com/Generous-Corp/pulp/pull/11428), source head `be70e3c44836605ac1279c65a572e833699e1fbb`, merge `e19f556e67129117ef3a4587bff11fdee16ebf44`.
- G-9 process-group cleanup: PR [#11667](https://github.com/Generous-Corp/pulp/pull/11667), source head `433d47e2413450323376cbf0b10e6e5ce28e6a2f`, merge `89c9be945120b9553f2983d265677292d4876caa`.

Both merge commits are ancestors of the current main head. Open graph PRs
#11679 (R2 evidence) is merged into this current main line; #11680 (MPE sidecars)
and #11682 (A9) remain separate open lanes and do not own this A10 lane.

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

The exact-head installed SDK for `2c2164d2da39a32a12559d2cc1598aae2628440d`
is unavailable, so exact-head installed-consumer evidence cannot be produced.
The available local Forge SDK provenance is stale, and the forge-dev SDK
installer is known to refuse a dirty/non-clean checkout. Creating a consumer
against that older SDK would produce stale provenance and is not admissible as
A10 evidence.

Next safe slice: run the SDK installer from a clean, exact-`origin/main`
checkout, then add `examples/a10-routing-hosting-consumer` using only the
public APIs above. The consumer must include a positive sidechain/automation/
PDC/transactional-hosted-node path and a planted refusal control for unavailable
latency or hostile containment. Do not promote the hostile fixture to PASS until
platform descendant cleanup, callback safety, recovery, and sibling continuity
are separately proven.

## Live admission snapshot — 2026-11-10

This snapshot is bound to origin/main=57bf8800d00329314fc3bb5691182808802195cd and the live PR state read on 2026-11-10. It records admission state only; it does not promote any lane beyond its evidence scope.

- Merged evidence heads: PR #11922 (G-7) 3b552aaf35ce206be8ee57300a720c49eaa93965; PR #11923 (G-10/A3) 4aa53af2dc8b72df33e87fe50a58fd12b69d3c21; PR #11933 (G-9) 54df12452b2543341b859ccef35aa57a490697ed; PR #11935 (G-11) 4c6c284658215464496c278aa33495c2ad29c706; PR #11943 (G-4/G-5) 54ce8848a17390f99eef4f910ba7ec418d4eb291.
- Open exact-head evidence: PR #11926 (A-10) aa57fb007af794dfa1f4fa3c1e7ead59842f4bf9; PR #11941 (A-1/G-6) 7bd1dc31c8b805a8a16be1c25308e06594f1f365; PR #11947 (A-9) 2d34234bef31d6e96d6ced052c94167df196f964. These remain unmerged and require current-head review/admission.
- All runtime, realtime, cross-platform, containment, and installed-consumer limits stated above remain PARTIAL/HOLD/NOT CHECKED. Green checks or merge state do not close those requirements.

## Live admission snapshot — 2026-11-10 (ledger refresh)

This snapshot is bound to `origin/main=910501973960060a19d056ec9fb190a283eacf08` and records live admission state without promoting any lane beyond its evidence scope. Merged evidence PRs include #11941 (A1/G-6), #11958 (G-9), and #11963 (G-7). Remaining graph evidence heads are open and unarmed: #11926 (A10) `120352d88d64b79b7ccd7c39899f1e49c74c7cfa`, #11957 (G-6) `df57e1b74f88eea4e4ff4033987162a888147931`, #11964 (G-11) `063675df7246c133d6205be7146f976c9d84641f`, #11965 (G-4) `77d2a27cacea3a34f86095ba5615b102be4c4f2d`, #11966 (G-5) `c38fa39382f5c524fe51110db6caa2446a54353f`, #11969 (G-3) `5030f677fcca09878f7037f06be482d1609a3b90`, #11970 (G-6) `0fd41853575e8a668fb040a5562859455d6754f8`, and #11971 (G-10) `05dcbef351cb37bdbcd844b5439595a2196b49fc`. These heads still require current exact-head review/admission. All runtime, realtime, cross-platform, containment, serialization, and installed-consumer limits remain PARTIAL/HOLD/NOT CHECKED.
