# Scripted-UI runtime inspector

Pulp retains reusable scripted-UI inspection components for QuickJS,
JavaScriptCore, and V8. Phase 3 deleted the legacy TCP server, raw client,
discovery publisher, and standalone session owner. Those components are not a
second live authority path: supported live operations use the canonical
per-user broker, exact-instance control protocol, and generated typed clients.

## Current public surface

`pulp control profiles [--json]` reads static capability metadata.
`pulp inspect profiles` remains a deprecated compatibility alias until Pulp
0.800.0 on 2026-10-01. `pulp inspect audit ARTIFACT [--json]` checks an artifact
and manifest offline without loading or activating it. Neither command reaches
a scripted UI.

Live typed operations use `pulp control` or the generated `pulp_control_*` MCP
family. Runtime evaluation is represented by
`dev.pulp.runtime/evaluate@1` / `pulp_control_runtime_evaluate`, but a typed
client does not make it available. The exact live host must declare and inject
the evaluator, and the broker requires a critical grant backed by separate,
single-use consent. No named grant profile enables evaluation automatically.

There is no public raw `Runtime.evaluate`, `Runtime.interrupt`,
`Runtime.getCapabilities`, or `Console.getMessages` route. There is also no
host/port, discovery-record, generic-method, or custom-fixture compatibility
path. Do not recreate one.

## Retained component contract

The optional runtime-eval component and `RuntimeEvaluator` interface remain
implementation building blocks for a canonical host adapter. Their presence in
source or an SDK is not evidence of runtime reachability.

The engine contract remains deliberately smaller than a step debugger:

- QuickJS has no source-line breakpoint, stepping, suspended-frame, or local
  scope protocol in the Pulp-linked backend.
- An adapter must report `canBreak`, `canStep`, and `canInspectLocals` honestly
  instead of implying DAP/CDP parity.
- Evaluation is arbitrary execution in the UI realm and remains a separate
  high-risk capability, never an implementation shortcut for parameters,
  transport, MIDI, test input, or authoring controls.
- Evaluation is marshaled onto the UI/engine thread at a frame-loop safe point,
  never the audio thread or mid-paint/layout.
- Evaluation remains bounded and single-flight. Code is capped at 64 KiB;
  serialized results and complete encoded responses are capped at 1 MiB;
  result walking enforces cycle and depth limits without first materializing an
  unbounded value.
- A hung evaluation is interrupted within its bounded deadline. An interrupt
  when nothing is running is a no-op and must not abort a later request.
- A completed evaluation obliges the host to rebuild the scripted-UI realm from
  source before the next frame while preserving widget values. Timers,
  animation frames, Promise jobs, callbacks, and global mutations created by
  evaluated code are discarded before a frame can run them.
- Realm reconstruction remains deferred until the host returns to its run loop;
  rebuilding inside the evaluation call would invalidate view-tree pointers
  still held by the caller. Reset failure detaches and destroys the scripted
  engine rather than leaving the evaluated realm active.
- A live scripted realm with any effectful `ReloadCapability` grant refuses
  evaluation. Exec, clipboard, filesystem, storage, AI, runtime-import, or
  network authority cannot be masked in place to make evaluation eligible.

Focused component tests may exercise these contracts directly. That test seam
does not create a shipped server, discovery record, or public raw-protocol
route.

## Centralized replacement boundary

The canonical broker/control composition now owns exact registration, typed
admission, grants, consent, receipts, cancellation, and audit for supported
T0/T1 operations. Runtime evaluation has a critical typed client contract, but
still requires an injected exact-instance evaluator and explicit broker-owned
consent; unavailable hosts fail closed.

New host support must join that centralized composition. It must not revive the
deleted Inspector framing, filesystem discovery records, standalone authority,
or a parallel capability system.
