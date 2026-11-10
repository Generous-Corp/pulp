# A10 — routing and hosting environment

Status: **PARTIAL — exact-current-head installed consumer passes bounded routing/hosting controls; hostile containment remains HOLD**

Current source authority for this refreshed receipt is merge head `eaf7de2788cd68021686f1eac2a2e9b9f0256ead`, which contains live `origin/main=68c471e93e46f57517fac64f4ece31148a6f0d93`.
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
consumer. A clean checkout at refreshed source head `eaf7de2788cd68021686f1eac2a2e9b9f0256ead` (live `origin/main=68c471e93e46f57517fac64f4ece31148a6f0d93`)
was configured as a Release source install with GPU disabled (the consumer
does not exercise rendering), and `tools/ci/governed-build.sh cmake --build
build-a10-sdk-release --target install` completed. The installed
`sdk-provenance.json` reports `kind=source-install`, `source_git_dirty=false`,
that exact source SHA, `platform=darwin-arm64`, and `build_profile=release`.

An installed-prefix consumer configured solely with
`Pulp_DIR=<prefix>/lib/cmake/Pulp`, built in Release with the governed build
wrapper, and passed CTest 1/1. The executable reports
`a10-routing-hosting PASS sidechain=1 automation=1 pdc=4 transactional_swap=1`.
The receipt is [`receipt.json`](../evidence/a10-routing-hosting-installed-20261110/receipt.json)
(`sha256=b5b6c74168738474a70acac124d633bcdac49b8641a315017f89d72f4d74f29d).
It records positive named sidechain, parameter automation, prepared latency,
and transactional swap controls plus planted refusals for a non-plugin
sidechain destination and an unknown parameter.

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
