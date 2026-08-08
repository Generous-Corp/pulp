# Scripted-UI runtime inspector

Pulp retains reusable scripted-UI inspection components for QuickJS,
JavaScriptCore, and V8, but they are not currently reachable from a shipped
standalone, `pulp inspect`, or MCP client. Phase 3 deleted the legacy server,
raw client, discovery publisher, and standalone session owner. This temporary
capability reduction prevents those components from being mistaken for a live
product surface while the canonical capability-control replacement is built.

## Current public surface

`pulp control profiles [--json]` reads static capability metadata.
`pulp inspect profiles` remains a deprecated compatibility alias until Pulp
0.800.0 on 2026-10-01.
`pulp inspect audit ARTIFACT [--json]` checks an artifact and manifest offline
without loading or activating it. Neither command reaches a scripted UI.

There is no public `Runtime.evaluate`, `Runtime.interrupt`,
`Runtime.getCapabilities`, or `Console.getMessages` route today. There is also
no host/port, discovery-record, generic-method, or custom-fixture compatibility
path. Do not recreate one.

## Retained component contract

The optional runtime-eval component and `RuntimeEvaluator` interface remain as
implementation building blocks for a later canonical host adapter. Their
presence in source or an SDK is not evidence of runtime reachability.

The intended engine contract remains deliberately smaller than a step debugger:

- QuickJS has no source-line breakpoint, stepping, suspended-frame, or local
  scope protocol in the Pulp-linked backend.
- A future adapter must report `canBreak`, `canStep`, and
  `canInspectLocals` honestly instead of implying DAP/CDP parity.
- Evaluation is arbitrary execution in the UI realm and must remain a separate
  high-risk capability, never an implementation shortcut for parameters,
  transport, MIDI, test input, or authoring controls.
- Any future evaluation route must preserve the existing bounded single-flight,
  timeout/interrupt, payload/result-size, cycle/depth, realm-reset, teardown,
  and effectful-`ReloadCapability` denial rules.

## Replacement boundary

Phases 4–7 retain the work needed to make selected live operations reachable:
trusted launcher and host adapters, broker-routed typed execution, supported
client migration, and final shipping/negative-control evidence. The resulting
surface must use the canonical broker/control protocol with explicit grants,
schemas, receipts, cancellation, and audit. It must not revive the deleted TCP
Inspector framing, filesystem discovery records, or standalone authority.

Until that composition lands, source-level component tests may exercise the
runtime bridge directly, but public documentation and tools must report the
operation as unavailable.
