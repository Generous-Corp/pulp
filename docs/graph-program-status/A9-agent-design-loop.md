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

Next safe slice: qualify the installed SDK consumer at an exact current head.
Do not add a second graph runtime or infer plugin isolation from this fixture.

## Exact current-head installed-SDK qualification (2026-11-09)

The standalone consumer was qualified from a clean Release producer checkout at
the live `origin/main` head
`3fad8b42a44d496361f97eeb469639e3a2b9e3c6`. The producer was configured with
`PULP_PROFILE=release`, `PULP_ENABLE_INSPECTOR=ON`,
`PULP_ENABLE_DESIGN_IMPORT=ON`, `PULP_BUILD_TESTS=OFF`,
`PULP_BUILD_EXAMPLES=OFF`, `PULP_DEV_BRIDGE_EXEC=OFF`, and
`PULP_ENABLE_AUDIO_PROBES=OFF`. `tools/ci/governed-build.sh cmake --build
build-a9-sdk --target install` completed; the Release cache and installed
development marker both report `source_git_dirty=false` and the exact source
SHA above. The read-only
`tools/scripts/sdk_development_provenance.py ... check` passed with
`kind=development`, `profile=existing-build-experiment`,
`distribution_eligible=false`, SDK version `1.93.0`, cache SHA-256
`2ef17dc5a0a9abf4e064b47c3affe24283744291d5542c9f69fd509160eed463`, build GPU
archive SHA-256
`e65cb554490e96ab688de6a9455d2d0452f62915e2a82a6d548b80a3c8f37f1a`, GPU
archive identity SHA-256
`7d8bc59a2233f4993bfc127f37c71e8f5e06dfc8dead1e6bb3b1e1277a398c14`, installed
SDK provenance marker SHA-256
`cd4a21dd1ee66ae354c0c2c68e73cb0ce246b9bf045f553d86ccbfcc3e94f7ae`,
`PulpConfig.cmake` SHA-256
`a66b0b6abdccb89f0671a92626b2c82ed8476864d29c76238cb2635c3527ef46`, and
installed build-info header SHA-256
`c969bc134de078f85ff18ceb44ff908d31af3055ec1723c04f26b1a05c2b4bd4`.

An external copy of `examples/graph-agent-loop` configured solely against that
installed prefix, built in Release, and ran CTest **5/5**. The positive receipt
was `graph-agent-loop PASS checks=258 catalog_nodes=89 baseline_blocks=3
edited_gain=0.25 plan_digest=428768875e834f86 semantic_fingerprint=7368e020052b8aa2
network=denied ambient_plugins=denied`; the action log SHA-256 was
`a6b45c6a3b0cb10aa114d307bd46852200aee07078fa2ea269251ead1e8ec0ad`, and the
consumer executable SHA-256 was
`5062e68a8b818bee0679176d4a0ba09f2418f327aa7f71ee7cb53b17110c5f26`. The
three planted controls each exited 1 before creating output and reported the
expected denial rules: `network-allowed` → `plan-network-allow`,
`plugin-allowed` → `plan-plugin-allow`, and `missing-action` → `action-catalog`.
The checked-in action-plan fixture SHA-256 is
`e2dc1e077d417d79b44609157d1a886367f3f4091bce519b98dd513fa7a92187`.

This is exact current-head installed-SDK evidence for the bounded offline
action-plan path using only public `Pulp::host`. It does not prove model
execution, CLI/MCP parity, arbitrary plugin hosting, realtime behavior,
network sandboxing outside the consumer, or A10 routing. The development SDK
marker is non-distributable by design.

