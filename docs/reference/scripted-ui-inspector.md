# Scripted-UI runtime inspector

Pulp's scripted UIs (JS via `@pulp/react` on QuickJS / JavaScriptCore / V8) can
be inspected at runtime over the existing inspector protocol: evaluate
expressions against the live engine, read honest debug capabilities, tail device
logs, and abort a runaway evaluation. This is a **runtime inspector / debug
console**, not a step debugger — see [runtime capabilities](#runtimegetcapabilities)
for why.

## Why not a step debugger (yet)

Pulp's default engine is **mainline QuickJS** (bundled header-only via CHOC). It
exposes no source-line debugger protocol — breakpoints, stepping, suspended call
frames, and local-scope inspection are all absent from the runtime Pulp links.
Those live only in QuickJS *forks* (quickjs-ng, koush) that Pulp does not vendor.
So rather than pretend, the inspector reports exactly what the engine can do and
offers the honest subset: evaluate, capability reporting, device logs, interrupt.
Source-line debugging is a future engine-capability milestone (a debugger-enabled
backend, or the Chrome DevTools inspector that JSC/V8 expose), gated behind the
`canBreak` / `canStep` capability flags below.

## Protocol methods

All methods speak the standard inspector JSON-RPC (`Domain.method` + `params`),
reachable via `pulp inspect --command … --params …` or any inspector client.

### `Runtime.getCapabilities`

Returns the engine's honest debug capabilities. Always safe to call (reports an
empty/`attached:false` engine when no scripted UI is wired).

```json
{
  "engine": "QuickJS",
  "attached": true,
  "canEvaluate": true,
  "canInterrupt": true,
  "canBreak": false,
  "canStep": false,
  "canInspectLocals": false
}
```

A client MUST branch on these flags rather than assuming DAP-shaped features.

### `Runtime.evaluate`

```json
// params
{ "code": "widgets.length" }      // "expression" is accepted as a CDP alias
// result
{ "result": <typed JSON value> }
```

Evaluates against `globalThis` on the engine thread. The request is **marshaled
from the inspector's background thread onto the UI/engine thread** and runs at a
safe point in the frame loop (never mid-paint/layout). One evaluation is in
flight at a time — a concurrent request returns a `busy` error. A JS exception
returns an error carrying the thrown message. A hung evaluation times out (~2 s)
and is interrupted automatically. The result is always valid JSON — a non-finite
number (`1/0`) is reported as `null` rather than a bare `NaN`/`Infinity` token.

**Opt-in required.** Evaluate is arbitrary code execution in the plugin's JS
context, so it is **off by default** even when the debug console is wired: a host
must call `DomainHandler::set_runtime_eval_enabled(true)` for a trusted dev
session. Until then, `Runtime.evaluate` / `Runtime.interrupt` return a "disabled"
error and `getCapabilities` reports `canEvaluate:false`.

### `Runtime.interrupt`

```json
{ "interrupted": true }   // true only if an evaluation was actually aborted
```

Cooperatively aborts the in-flight evaluation (QuickJS host interrupt). A no-op
(`interrupted:false`) when nothing is running, so it can never spuriously abort
the *next* evaluation. Requires the same `set_runtime_eval_enabled(true)` opt-in
as evaluate. Because the inspector handles requests synchronously on one
connection's reader thread, a client that is blocked in `Runtime.evaluate`
cannot send `Runtime.interrupt` on the *same* connection — use a second
connection, or rely on the automatic ~2 s timeout-interrupt for a runaway eval.

### `Console.getMessages`

Device-log cursor poll. Returns entries newer than the client's cursor plus the
next cursor to page forward without duplicates.

```json
// params
{ "sinceSeq": 0 }
// result
{ "messages": [ { "level": "log", "message": "…", "seq": 1 } ], "nextSeq": 1 }
```

`Console.enable` still returns the full retained ring buffer (last 200 entries),
now each carrying its `seq`.

## Wiring a host

The runtime inspector reaches the live UI only when a host connects its
`ScriptedUiSession` to the inspector's `DomainHandler`:

```cpp
// The session owns the bridge and pumps it once per poll() on the UI thread.
handler.set_script_inspector(session.script_inspector());
handler.set_console_capture(&console);            // for Console.getMessages
handler.set_runtime_eval_enabled(true);           // opt in to evaluate (dev/loopback only)
```

**Teardown ordering:** the bridge lives as long as the `ScriptedUiSession`, but
its methods are called from the inspector's background reader thread. Before
destroying the session, stop the `InspectorServer` reader thread and call
`handler.set_script_inspector(nullptr)` so no background call outlives the bridge.

`ScriptInspectorBridge` re-attaches to the engine across hot reloads, so the
debug console survives a reload. Without this wiring, `Runtime.evaluate` /
`Runtime.interrupt` report the engine as unavailable and `getCapabilities`
returns `attached:false`.

## Security

Evaluate is remote code execution against the plugin UI's JS context, and the
inspector transport is unauthenticated. Two consequences:

- Evaluate/interrupt are **off by default** — a host must explicitly
  `set_runtime_eval_enabled(true)`, and should only do so for a trusted, local
  dev session. Read-only surfaces (logs, DOM, state) are unaffected.
- The inspector server currently binds all interfaces; do not enable eval on a
  host reachable from an untrusted network. Evaluation is serialized and never
  runs on the audio thread.
