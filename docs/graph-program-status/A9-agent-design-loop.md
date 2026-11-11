# A9 — agent design loop

Status: **PARTIAL — bounded installed-SDK consumer and fail-closed action-plan harness implemented**

## Action-plan harness (2026-11-06, source `3fad8b42a44d496361f97eeb469639e3a2b9e3c6`)

The existing consumer now reads the checked-in
[`fixtures/action-plan.json`](../../examples/graph-agent-loop/fixtures/action-plan.json),
which pins a deterministic offline action model, tool version, and this exact
source SHA. The executable decodes escaped and Unicode JSON keys before
duplicate-key rejection, validates the complete root/action/assertion/mutation
schema and 64-hex model digest, then executes the ordered steps from the plan
and records each completed step. It refuses case-insensitive URI schemes,
protocol-relative and UNC authorities, `network_policy: allow`,
`plugin_policy: allow-ambient`, unknown policy URL fields, missing or mistyped
actions, and incorrect assertion or mutation relations before creating an
output directory, action log, or graph. Its JSONL action log records a plan
digest, ordered action results, explicit network/ambient-plugin denials, and a
topology-only semantic fingerprint. A gain edit must change samples without
changing that fingerprint; saved-input replay must restore the original
samples and fingerprint.

The standalone CTest suite adds three independent planted-negative plans:
network opt-in, ambient plugin opt-in, and missing action schema. Each is a
full copy of the positive plan with one field or step changed, and CTest asserts
the named denial rule (`plan-network-allow`, `plan-plugin-allow`, or
`action-catalog`) while the restored positive plan passes.
This is evidence for the local action-plan boundary only. It does not prove
model execution, CLI/MCP parity, arbitrary plugin hosting, network sandboxing
outside the consumer, realtime behavior, A10 routing, or flagship completion.

This slice closes one executable, public-API path for catalog discovery,
notation, validation, offline render, edge probe/compare, one bounded edit, and
reproduction from saved inputs. It is intentionally separate from the live G-4
nested-macro PR and G-12 performance PR.

The consumer is in [`examples/graph-agent-loop`](../../examples/graph-agent-loop/)
and uses only `Pulp::host`. It disables plugin loading and default plugin roots
for replay, so the action plan cannot execute ambient plugin code. Its positive
receipt is `graph-agent-loop PASS`; its expected edit changes the render while
reloading the saved baseline reproduces the original samples exactly.

The focused consumer gate has a planted edit-oracle control: replacing the expected render change with equality fails with `graph-agent-loop FAIL rule=edit-changes-render` (rc 1), while the restored fixture passes. The public serializer include is intentionally scoped to this standalone consumer; its measured header fan-out is declared in the change trailer without widening public headers.

This does not complete A9. Model execution, CLI/MCP parity, arbitrary hosted
plugins, and a full external fixture remain open. It also
does not claim A10 routing, arbitrary hosted plugins, realtime qualification,
or G-3 prepared-plan identity.

The installed SDK consumer qualification below is current-head evidence. Do not
add a second graph runtime or infer plugin isolation from this fixture.

## Exact current-head installed-SDK qualification (2026-11-09)

The checked-in action plan and all three planted mutation copies pin the exact
producer/source head `3fad8b42a44d496361f97eeb469639e3a2b9e3c6`; the consumer
source-pin guard carries the same value. The standalone consumer was qualified
from a clean Release producer checkout at that live `origin/main` head. The
producer was configured with `PULP_PROFILE=release`,
`PULP_ENABLE_INSPECTOR=ON`, `PULP_ENABLE_DESIGN_IMPORT=ON`,
`PULP_BUILD_TESTS=OFF`, `PULP_BUILD_EXAMPLES=OFF`,
`PULP_DEV_BRIDGE_EXEC=OFF`, and `PULP_ENABLE_AUDIO_PROBES=OFF`.
`tools/ci/governed-build.sh cmake --build build-a9-sdk --target install`
completed; the Release cache and installed source-install marker both report
`source_git_dirty=false` and that exact source SHA. The read-only
`tools/scripts/sdk_development_provenance.py ... check` passed for the installed
package identity. The installed `sdk-provenance.json` marker reports
`kind=source-install`, `build_profile=release`, and
`distribution_eligible=false`; SDK version `1.93.0`, cache SHA-256
`609a14b0295c4d86b34614591649b33f3d3bf8ccdcf5f5617032383e371a6b69`, build GPU
archive SHA-256
`76ca66da4b53ae51dbcc6eb74ea91a70dc8a374d6b20f3af62f45a874436a0e7`, GPU
archive identity SHA-256
`4a0f6bacc290d5e5a4ec4baa0237828253afb64d23c18eed0ba5a7f26694099f`, installed
SDK provenance marker SHA-256
`cd4a21dd1ee66ae354c0c2c68e73cb0ce246b9bf045f553d86ccbfcc3e94f7ae`,
`PulpConfig.cmake` SHA-256
`a66b0b6abdccb89f0671a92626b2c82ed8476864d29c76238cb2635c3527ef46`, and
installed build-info header SHA-256
`241d347a76f76fc74db59a1b5c44052a5e85e347b8531a32e50364343748ccaa`.

An external copy of `examples/graph-agent-loop` configured solely against that
installed prefix, built in Release, and ran CTest **5/5**. The positive receipt
was `graph-agent-loop PASS checks=258 catalog_nodes=89 baseline_blocks=3
edited_gain=0.25 plan_digest=40848371d259a634 semantic_fingerprint=7368e020052b8aa2
network=denied ambient_plugins=denied`; the action log SHA-256 was
`f05c5de56f70d0b3988c1c3f2f90177e02e043a8497a27cf34e8482fd5b419f9`, and the
consumer executable SHA-256 was
`fc31d86d4e520be3ef2defa6facfe4fbfbc32180b7cb1c2c61f4ac6eaffe7c14`. The
three planted controls each exited 1 before creating output and reported the
expected denial rules: `network-allowed` → `plan-network-allow`,
`plugin-allowed` → `plan-plugin-allow`, and `missing-action` → `action-catalog`.
The checked-in action-plan fixture SHA-256 is
`9b0ee0b77c09e09bfe99c0d09f674e8729f13146ea00dbea98c46d2c3f8a7eca`.

This is exact current-head installed-SDK evidence for the bounded offline
action-plan path using only public `Pulp::host`. It does not prove model
execution, CLI/MCP parity, arbitrary plugin hosting, realtime behavior,
network sandboxing outside the consumer, or A10 routing. The source-install
marker is non-distributable by design (`distribution_eligible=false`).

## Live admission snapshot — 2026-11-10

This snapshot is bound to origin/main=57bf8800d00329314fc3bb5691182808802195cd and the live PR state read on 2026-11-10. It records admission state only; it does not promote any lane beyond its evidence scope.

- Merged evidence heads: PR #11922 (G-7) 3b552aaf35ce206be8ee57300a720c49eaa93965; PR #11923 (G-10/A3) 4aa53af2dc8b72df33e87fe50a58fd12b69d3c21; PR #11933 (G-9) 54df12452b2543341b859ccef35aa57a490697ed; PR #11935 (G-11) 4c6c284658215464496c278aa33495c2ad29c706; PR #11943 (G-4/G-5) 54ce8848a17390f99eef4f910ba7ec418d4eb291.
- Open exact-head evidence: PR #11926 (A-10) aa57fb007af794dfa1f4fa3c1e7ead59842f4bf9; PR #11941 (A-1/G-6) 7bd1dc31c8b805a8a16be1c25308e06594f1f365; PR #11947 (A-9) 2d34234bef31d6e96d6ced052c94167df196f964. These remain unmerged and require current-head review/admission.
- All runtime, realtime, cross-platform, containment, and installed-consumer limits stated above remain PARTIAL/HOLD/NOT CHECKED. Green checks or merge state do not close those requirements.

## Live admission snapshot — 2026-11-10 (ledger refresh)

This snapshot is bound to `origin/main=910501973960060a19d056ec9fb190a283eacf08` and records live admission state without promoting any lane beyond its evidence scope. Merged evidence PRs include #11941 (A1/G-6), #11958 (G-9), and #11963 (G-7). Remaining graph evidence heads are open and unarmed: #11926 (A10) `120352d88d64b79b7ccd7c39899f1e49c74c7cfa`, #11957 (G-6) `df57e1b74f88eea4e4ff4033987162a888147931`, #11964 (G-11) `063675df7246c133d6205be7146f976c9d84641f`, #11965 (G-4) `77d2a27cacea3a34f86095ba5615b102be4c4f2d`, #11966 (G-5) `c38fa39382f5c524fe51110db6caa2446a54353f`, #11969 (G-3) `5030f677fcca09878f7037f06be482d1609a3b90`, #11970 (G-6) `0fd41853575e8a668fb040a5562859455d6754f8`, and #11971 (G-10) `05dcbef351cb37bdbcd844b5439595a2196b49fc`. These heads still require current exact-head review/admission. All runtime, realtime, cross-platform, containment, serialization, and installed-consumer limits remain PARTIAL/HOLD/NOT CHECKED.
