# A10 — routing and hosting environment

## Close-out (2026-11-13)

Final status: PARTIAL; hostile containment remains HOLD (unchanged).

The bounded installed routing/hosting consumer passes on main (PRs 11926, 11428, 11667, 11679, 11680, 11682). Hostile containment, callback safety and recovery are not qualified.

PR pointers below are historical; all PRs cited in this file that were queried (checked 2026-11-13) are merged, none remain open.

Remaining work is deferred to https://github.com/Generous-Corp/pulp/issues/12177.

Status: **PARTIAL — exact-current-head installed consumer passes bounded routing/hosting controls; hostile containment remains HOLD**

Current source authority for this refreshed receipt is exact live `origin/main=4c69e183387017c47d048b477218a6d0a4947bcc` (2026-11-12). The governed Release install and consumer were rebuilt from this merge head; `source_tree_archive_sha256=7b47eed1d4dcca572452610e439e3343b807d291c198d67b598df85427f73ced` binds the installed source tree.
The required graph dependencies are present at exact merged heads:

- G-1 named typed ports: PR [#11428](https://github.com/Generous-Corp/pulp/pull/11428), source head `be70e3c44836605ac1279c65a572e833699e1fbb`, merge `e19f556e67129117ef3a4587bff11fdee16ebf44`.
- G-9 process-group cleanup: PR [#11667](https://github.com/Generous-Corp/pulp/pull/11667), source head `433d47e2413450323376cbf0b10e6e5ce28e6a2f`, merge `89c9be945120b9553f2983d265677292d4876caa`.

Both merge commits are ancestors of the current main head. PRs
[#11679](https://github.com/Generous-Corp/pulp/pull/11679) (R2 evidence),
[#11680](https://github.com/Generous-Corp/pulp/pull/11680) (MPE sidecars), and
[#11682](https://github.com/Generous-Corp/pulp/pull/11682) (A9) are merged;
these separate graph lanes do not own this A10 evidence.

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

## Exact current-main installed-SDK consumer (2026-11-12)

The former SDK-availability blocker is cleared for a bounded public-host
consumer at the current exact main head. A clean checkout at merge source head
`4c69e183387017c47d048b477218a6d0a4947bcc` was configured as a Pulp
`release` profile and CMake Release source install with GPU audio disabled. The
installed `sdk-provenance.json` reports `kind=source-install`,
`source_git_dirty=false`, that exact source SHA, `platform=darwin-arm64`, and
`build_profile=release`; the source and consumer CMake builds use
`CMAKE_BUILD_TYPE=Release`.

An installed-prefix consumer configured solely with
`Pulp_DIR=<prefix>/lib/cmake/Pulp`, built in Release with the governed build
wrapper, passed CTest 1/1. The executable reports
`a10-routing-hosting PASS sidechain_route_accepted=1 automation_route_accepted=1 pdc=4 transactional_prepare_accepted=1`.
The receipt is [`receipt.json`](../evidence/a10-routing-hosting-installed-20261110/receipt.json)
and is bound to the current exact head. It records accepted named sidechain and
automation route registration, prepared latency introspection, and transactional
topology preparation plus planted refusals for a non-plugin sidechain destination
and an unknown parameter. The consumer does not call `SignalGraph::process`, so
these are graph wiring and preparation acceptance checks rather than audio or
parameter-value behavior.

This refresh closes the exact-head installed-consumer configuration gap for the
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

This snapshot is bound to origin/main=b4fb59b2128b8846c8d4597cad2c972c84c75d57 and the live PR state read on 2026-11-10. It records admission state only; it does not promote any lane beyond its evidence scope.

- Merged evidence heads: PR #11922 (G-7) 3b552aaf35ce206be8ee57300a720c49eaa93965; PR #11923 (G-10/A3) 4aa53af2dc8b72df33e87fe50a58fd12b69d3c21; PR #11933 (G-9) 54df12452b2543341b859ccef35aa57a490697ed; PR #11935 (G-11) 4c6c284658215464496c278aa33495c2ad29c706; PR #11943 (G-4/G-5) 54ce8848a17390f99eef4f910ba7ec418d4eb291.
- Open exact-head evidence: PR #11926 (A-10) 05e23b36f9737bf725ad1cee82afc8dc108cd9c2. It remains open and requires current-head review/admission.
- Historical exact-head rows now closed by the live API: PR #11941 (A-1/G-6) merged at head a3f7959af21214e51efd5c073bc613cb5fee65c4; PR #11947 (A-9) merged at head 2d34234bef31d6e96d6ced052c94167df196f964. They are no longer open admission candidates.
- All runtime, realtime, cross-platform, containment, and installed-consumer limits stated above remain PARTIAL/HOLD/NOT CHECKED. Green checks or merge state do not close those requirements.

## Live admission snapshot — 2026-11-10 (ledger refresh)

This snapshot is bound to `origin/main=910501973960060a19d056ec9fb190a283eacf08` and records live admission state without promoting any lane beyond its evidence scope. Merged evidence PRs include #11941 (A1/G-6), #11958 (G-9), and #11963 (G-7). Remaining graph evidence heads are open and unarmed: #11926 (A10) `120352d88d64b79b7ccd7c39899f1e49c74c7cfa`, #11957 (G-6) `df57e1b74f88eea4e4ff4033987162a888147931`, #11964 (G-11) `063675df7246c133d6205be7146f976c9d84641f`, #11965 (G-4) `77d2a27cacea3a34f86095ba5615b102be4c4f2d`, #11966 (G-5) `c38fa39382f5c524fe51110db6caa2446a54353f`, #11969 (G-3) `5030f677fcca09878f7037f06be482d1609a3b90`, #11970 (G-6) `0fd41853575e8a668fb040a5562859455d6754f8`, and #11971 (G-10) `05dcbef351cb37bdbcd844b5439595a2196b49fc`. These heads still require current exact-head review/admission. All runtime, realtime, cross-platform, containment, serialization, and installed-consumer limits remain PARTIAL/HOLD/NOT CHECKED.


## Live admission snapshot — 2026-11-12

This snapshot is bound to exact `origin/main=4c69e183387017c47d048b477218a6d0a4947bcc`. The A10 bounded installed consumer is PASS at this head; hostile containment, callback safety, recovery, sibling continuity, hard realtime, and cross-platform coverage remain PARTIAL/HOLD/NOT CHECKED. Open graph PRs observed in the live API are #12147 (`codex/p0-layered-receipt-20261112`, head `663b64e9e80055eaeb266c419dd25f364b96faa7`) and #12161 (`feature/midi-port-identity`, head `dc12ccefb71413eea661557070a8c5931bcfe474`); neither owns this A10 receipt refresh.
