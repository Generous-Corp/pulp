# Scripted-UI runtime inspector

Pulp contains reusable scripted-UI inspector components for QuickJS,
JavaScriptCore, and V8. A standalone launched with an explicit Development
Inspector profile attaches those components to its authenticated local session.
Inspector-off standalones and every normal plugin-format launch construct no
endpoint. The surface is a **runtime inspector / debug console**, not a step
debugger.

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

The component methods use the inspector JSON message shape (`Domain.method` +
`params`). They are reachable only after `pulp run --inspect` (or another
explicit profile) in a GPU-enabled desktop build constructs and wires the
standalone server; `pulp inspect`
is the experimental low-level client.

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
flight at a time — a concurrent direct dispatch returns `busy`, while the
standalone transport's single-slot worker rejects another queued evaluation.
A JS exception
returns an error carrying the thrown message. A hung evaluation times out (~2 s)
and is interrupted automatically. The result is always valid JSON — a non-finite
number (`1/0`) is reported as `null` rather than a bare `NaN`/`Infinity` token.
Decoded code is capped at 64 KiB. Serialized results and complete encoded
responses are each capped at 1 MiB. QuickJS enforces the result byte, depth,
and cycle limits while walking the value rather than materializing an
unbounded intermediate object.

Every evaluation obliges the host to rebuild the scripted-UI realm from its
source, preserving widget values, **before the next frame** — that is, at the
top of the host's next `ScriptedUiSession::poll()`, ahead of the frame pump and
the next request pump. Global mutations, timers, animation frames, Promise jobs,
and event callbacks created by evaluated code are therefore discarded before any
frame pump can execute them outside the request deadline. Evaluations that share
a realm between two frames owe one rebuild between them, not one each.

Sharing a realm is observable, and deliberately so: a global that one evaluation
plants is visible to another evaluation issued before the next frame. What the
reset guarantees is that evaluated code never reaches a **frame** — the deferred
timer, animation frame, Promise job, or patched callback is discarded with its
realm before any frame pump could run it. It does not, and never did, isolate
one evaluation from the next within a single frame; an inspector client that
can evaluate at all could put the same statements in one request.

The rebuild is deliberately **not** run inside `Runtime.evaluate`: replacing the
view tree there would leave every widget pointer the caller held across the
request pointing into a detached tree, so a panel would render and stop
responding. The response describes the live realm at the evaluation safe point,
and that realm stays the caller's until it returns to its run loop.

Realm reconstruction replaces every script-owned `View` and its callback
chain. A host that adds native observers to those views must install
`ScriptedUiSession::set_post_evaluation_reset_callback()` and resolve the new
views by stable widget id there. The callback runs after the replacement bridge
is live and before its first frame pump. It must use metadata retained from the
original inspection; evaluating the registry again from that callback would
owe another reset and create a reset/rebind loop.

Realm reconstruction gets a fixed 500 ms cleanup grace, measured from the frame
boundary that runs it; the enclosing main-thread RPC still bounds the evaluation
at three seconds. Because the rebuild is deferred, a reset failure is reported by
`poll()` — which returns false with `evaluated realm reset failed: …` — rather
than by the evaluate response. The fail-closed behavior is unchanged: on failure
the host detaches and destroys the scripted engine instead of leaving the
evaluated realm active. A session destroyed before it ever polls forces the owed
reset during teardown so the failure is still logged.

**Opt-in required.** Evaluate is arbitrary code execution in the plugin's JS
context, so it is **off by default** even when the debug console is wired: a host
must separately enable it for a trusted dev session. The live
`ScriptedUiSession` must also have an empty effectful `ReloadCapability` grant
set. Any `exec`, clipboard, filesystem, storage, AI, runtime-import, or network
grant refuses evaluation; `getCapabilities` reports `canEvaluate:false` plus
`evaluateDeniedReason`. Pulp does not mask those globals in place. A custom
production host can explicitly pass an empty
`ScriptedUiOptions::granted_capabilities`; the stock `build_editor_ui` path
retains its historical all-capabilities posture and is therefore ineligible.

### `Runtime.interrupt`

```json
{ "interrupted": true }   // true only if an evaluation was actually aborted
```

Cooperatively aborts the in-flight evaluation (QuickJS host interrupt). A no-op
(`interrupted:false`) when nothing is running, so it can never spuriously abort
the *next* evaluation. Requires the same `set_runtime_eval_enabled(true)` opt-in
as evaluate. The server runs `Runtime.evaluate` through one bounded asynchronous
worker, leaving the authenticated connection reader free to deliver
`Runtime.interrupt` from that same controller connection. The session still
performs its normal capability, controller-lease, and audit checks on both
requests.

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

The runtime inspector reaches the live UI only when a host links the optional
runtime-eval component and connects its `ScriptedUiSession` to the inspector's
`DomainHandler`:

```cpp
#include <pulp/inspect/runtime_eval_component.hpp>

// Retain this adapter while DomainHandler borrows its pointer. The session owns
// the bridge and pumps it once per poll() on the UI thread.
auto evaluator = pulp::inspect::make_script_runtime_evaluator(
    session.script_inspector());
handler.set_runtime_evaluator(evaluator.get());
handler.set_console_capture(&console);            // for Console.getMessages
handler.set_runtime_eval_enabled(true);           // opt in to evaluate (dev/loopback only)
```

**Teardown ordering:** the bridge lives as long as the `ScriptedUiSession`, but
its methods are called from inspector worker threads. Ordinary owner-thread
teardown destroys `InspectorServer` synchronously, then calls
`handler.set_runtime_evaluator(nullptr)` and destroys `evaluator` before
destroying the session. If a
publication or domain callback may destroy the server wrapper, capture
`server.shutdown_fence()` first. Retain the bridge and other attached sources,
then wait on that fence from a non-callback thread before clearing them or
unloading the module. The fence remains closed through any causal server
callback still unwinding after deferred teardown and until the dispatcher
releases every accepted main-thread RPC callable. A custom dispatcher must
therefore destroy cancelled queued callables even though they are inert.
Waiting on either owned server worker itself returns `false` instead of
deadlocking.

`ScriptInspectorBridge` re-attaches to the engine across hot reloads, so the
debug console survives a reload. Without this wiring, `Runtime.evaluate` /
`Runtime.interrupt` report the engine as unavailable and `getCapabilities`
returns `attached:false`.
This guarantee covers reloads within one `ScriptedUiSession`. A processor that
replaces its entire editor/session currently causes standalone inspector startup
to fail closed until all borrowed sources can be reattached atomically.

## Security

Evaluate is remote code execution against the plugin UI's JS context, and the
authenticated inspector transport therefore treats it as a separately gated,
high-risk capability. Two consequences:

- Evaluate/interrupt are **off by default** — a host must explicitly
  `set_runtime_eval_enabled(true)`, and should only do so for a trusted, local
  dev session. Read-only surfaces (logs, DOM, state) are unaffected.
- The production TCP server binds loopback only and requires a fresh
  nonce/HMAC proof in each direction, with role-separated transcripts using
  an owner-private per-session credential discovered through an ephemeral
  record/token pair. Do not enable eval outside a controlled custom-host
  fixture. Evaluation is serialized and never runs on the audio thread.
