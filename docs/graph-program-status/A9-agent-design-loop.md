# A9 — agent design loop

Status: **PARTIAL — bounded installed-SDK consumer and fail-closed action-plan harness implemented**

## Action-plan harness (2026-11-06, source `2f3d36dae75c3dca283c58e47f7731356f088e0d`)

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
completed; the Release cache and installed development marker both report
`source_git_dirty=false` and that exact source SHA. The read-only
`tools/scripts/sdk_development_provenance.py ... check` passed with
`kind=development`, `profile=existing-build-experiment`,
`distribution_eligible=false`, SDK version `1.93.0`, cache SHA-256
`609a14b0295c4d86b34614591649b33f3d3bf8ccdcf5f5617032383e371a6b69`, build GPU
archive SHA-256
`76ca66da4b53ae51dbcc6eb74ea91a70dc8a374d6b20f3af62f45a874436a0e7`, GPU
archive identity SHA-256
`4a0f6bacc290d5e5a4ec4baa0237828253afb64d23c18eed0ba5a7f26694099f`, installed
SDK provenance marker SHA-256
`9b9990e25a81c69a03efa6c11eb060dbf389f73e99b6ed122da007b9ea041cff`,
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
network sandboxing outside the consumer, or A10 routing. The development SDK
marker is non-distributable by design.
