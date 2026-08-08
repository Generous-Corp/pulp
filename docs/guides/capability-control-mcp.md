# Capability control over MCP

`pulp-mcp` projects Pulp's canonical capability-control registry as typed MCP
tools. The projection does not invoke the `pulp` CLI and does not implement a
second authorization or execution path. Discovery, exact-instance selection,
protocol negotiation, grants, consent, receipts, cancellation, progress, and
artifact ACL checks all remain owned by the installed broker and shared
`ControlClient`.

## Typed tools

Every broker-grantable frozen `dev.pulp.*@1` operation is exposed as a stable
`pulp_control_*` tool generated from the canonical operation registry. For
example, `dev.pulp.state/read@1` becomes `pulp_control_state_read`,
`dev.pulp.state/parameter-gesture@1` becomes
`pulp_control_state_parameter_gesture`, and
`dev.pulp.runtime/evaluate@1` becomes `pulp_control_runtime_evaluate`.
Registry operations whose capability is explicitly non-grantable are not
advertised: no MCP annotation or caller-supplied value can create authority for
them.
Each operation takes:

- `instance_id`: the exact broker-owned live instance;
- optional `request_id`: a caller-visible correlation ID needed when another
  concurrent MCP request may cancel the operation;
- `input`: the operation's closed canonical input schema;
- optional `grant_id`, `profile`, `expected_state_generation`, and bounded
  `timeout_ms` adapter fields.

The tool's `inputSchema` and `outputSchema` are generated from the same frozen
registry used by the CLI and service. Results contain typed structured content,
the correlated request and receipt IDs, terminal state, artifacts, and bounded
progress observations. `pulp_control_cancel` sends the canonical cancellation
envelope. `pulp_control_artifact_read` returns broker-ACL-checked chunks rather
than writing arbitrary local paths.

MCP resources expose exact instance metadata and ACL-checked artifacts under
`pulp-control://`. They are read resources, not lifecycle subscriptions;
broker-backed live telemetry subscriptions use the generated
`pulp_control_telemetry_subscribe` operation. A client that supplies an MCP progress token receives
`notifications/progress`; the final structured result also carries the
validated progress sequence so clients without notification UI support do not
lose evidence.

## Consent is not MCP metadata

Tool annotations and any client-side confirmation UI are advisory. They never
create a grant. Grant requests go to the broker, which obtains consent from its
trusted composition root and rejects absent or replayed decisions. Critical
operations, including runtime evaluation, require an explicit broker-issued
grant backed by a single-use broker-owned consent decision; the MCP adapter
does not auto-grant them and has no argument that can claim approval.
Revocation, expiry, publication changes, and instance teardown remain effective
during a call because the adapter sends the canonical grant and instance
lineage and reports the broker's terminal receipt rather than assuming success.

## Existing-tool break and side-effect inventory

The legacy live Inspector MCP tools are intentionally replaced as follows:

| Previous tool | Disposition | Canonical replacement |
|---|---|---|
| `pulp_inspect_set_param` | Removed; no read-only alias. The old live mutation is now an announced, grant-gated operation. | `pulp_control_state_parameter_gesture` |
| `pulp_inspect_evaluate` | Removed. Its former description was stale: evaluation could execute live code. Runtime evaluation is critical and requires an explicit broker grant backed by single-use consent. | `pulp_control_runtime_evaluate` |
| `pulp_inspect_screenshot` | Removed. It never performed live capture after legacy authority deletion. | `pulp_control_ui_capture` for an authorized live instance; `pulp_screenshot` remains only a demo/script fixture renderer. |

Other agent-reachable side effects stay outside capability control for this
phase:

- `pulp_create` scaffolds source files in a user-selected project directory.
  It is an authoring/workspace operation, not an operation against a live plugin
  instance, so it remains outside the instance capability model.
- `pulp_screenshot`, `pulp_simulate_click`, `pulp_get_view_tree`, and the Motion
  fixture/visual-analysis paths operate on built-in demos, scripts, or explicit
  fixture files. They remain outside live-instance authority. Live capture,
  input, and Motion control use the corresponding generated
  `pulp_control_ui_*`, `pulp_control_trace_control`, or other canonical tools.

This boundary is based on the authority target, not whether a tool happens to
have a side effect: live plugin authority must pass through the broker; local
workspace and fixture workflows retain their existing explicit filesystem or
demo scope.
