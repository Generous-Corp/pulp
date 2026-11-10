# A10 — routing and hosting environment

Status: **PARTIAL — exact-current-head installed consumer passes bounded routing/hosting controls; hostile containment remains HOLD**

Current source authority for this refreshed receipt is merge head `1b41de3f9314b36a928938d8b1fdf634bc60aa72`, based on live `origin/main=fb0ca2601cab3f082d72e4bba9f2aaf5675eb3a8` at build time. The governed Release install and consumer were rebuilt from this merge head; `source_tree_archive_sha256=564f65ea259a3591aeabfb417c765f8755a071100b41b6b176426681d8a8b2c9` binds the installed source tree.
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

## Exact current-main installed-SDK consumer (2026-11-10)

The former SDK-availability blocker is cleared for a bounded public-host
consumer. A clean checkout at merge source head `1b41de3f9314b36a928938d8b1fdf634bc60aa72` (based on live `origin/main=fb0ca2601cab3f082d72e4bba9f2aaf5675eb3a8`)
was configured as a Pulp `release` profile and CMake Release source install with GPU disabled (the consumer
does not exercise rendering), and `tools/ci/governed-build.sh cmake --build
build-a10-sdk-release --target install` completed. The installed
`sdk-provenance.json` reports `kind=source-install`, `source_git_dirty=false`,
that exact source SHA, `platform=darwin-arm64`, and `build_profile=release`; the
source and consumer CMake builds use `CMAKE_BUILD_TYPE=Release`.

An installed-prefix consumer configured solely with
`Pulp_DIR=<prefix>/lib/cmake/Pulp`, built in Release with the governed build
wrapper, and passed CTest 1/1. The executable reports
`a10-routing-hosting PASS sidechain_route_accepted=1 automation_route_accepted=1 pdc=4 transactional_prepare_accepted=1`.
The receipt is [`receipt.json`](../evidence/a10-routing-hosting-installed-20261110/receipt.json)
(`sha256=fec24df91fb3e5c495714ce2f08d70e2d7502653aa39f9d6174c72e563341e33`).
It records accepted named sidechain and automation route registration, prepared
latency introspection, and transactional topology preparation plus planted
refusals for a non-plugin sidechain destination and an unknown parameter. The
consumer does not call `SignalGraph::process`, so these are graph wiring and
preparation acceptance checks rather than audio or parameter-value behavior.

This closes the exact-head installed-consumer configuration gap for the
bounded host API. The fixture uses a caller-owned deterministic in-process
`PluginSlot`; it intentionally does not claim arbitrary third-party plugin
execution, process isolation, descendant cleanup, callback safety, recovery,
sibling continuity, hard realtime, or Windows/Linux parity. G-9 production
containment and those platform gates remain HOLD.

## Historical blocker and remaining safe boundary

The older `2c2164d2…` exact-head SDK blocker is superseded by the current-main
source install and receipt above. No stale SDK is used for the current claim.
The remaining safe boundary is unchanged: do not promote the hostile fixture to
PASS until platform descendant cleanup, callback safety, recovery, and sibling
continuity are separately proven. A future slice may add a real installed
third-party plugin artifact only when its provenance and containment evidence
are available.

## Live admission snapshot — 2026-11-10

This snapshot is bound to origin/main=57bf8800d00329314fc3bb5691182808802195cd and the live PR state read on 2026-11-10. It records admission state only; it does not promote any lane beyond its evidence scope.

- Merged evidence heads: PR #11922 (G-7) 3b552aaf35ce206be8ee57300a720c49eaa93965; PR #11923 (G-10/A3) 4aa53af2dc8b72df33e87fe50a58fd12b69d3c21; PR #11933 (G-9) 54df12452b2543341b859ccef35aa57a490697ed; PR #11935 (G-11) 4c6c284658215464496c278aa33495c2ad29c706; PR #11943 (G-4/G-5) 54ce8848a17390f99eef4f910ba7ec418d4eb291.
- Open exact-head evidence: PR #11926 (A-10) aa57fb007af794dfa1f4fa3c1e7ead59842f4bf9; PR #11941 (A-1/G-6) 7bd1dc31c8b805a8a16be1c25308e06594f1f365; PR #11947 (A-9) 2d34234bef31d6e96d6ced052c94167df196f964. These remain unmerged and require current-head review/admission.
- All runtime, realtime, cross-platform, containment, and installed-consumer limits stated above remain PARTIAL/HOLD/NOT CHECKED. Green checks or merge state do not close those requirements.
