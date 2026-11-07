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

