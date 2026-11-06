# A9 — agent design loop

Status: **PARTIAL — bounded installed-SDK consumer implemented**

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

This does not complete A9. Model/tool pinning, a declarative agent action-log
schema, network denial, CLI/MCP parity, semantic fingerprint/diff integration,
and a full external fixture with planted mutation controls remain open. It also
does not claim A10 routing, arbitrary hosted plugins, realtime qualification,
or G-3 prepared-plan identity.

Next safe slice: add the action-plan and mutation-control harness around this
consumer after this PR's exact-head installed-SDK receipt is reviewed. Do not
add a second graph runtime or infer plugin isolation from this fixture.

