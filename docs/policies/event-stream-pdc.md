# Event-stream plugin delay compensation

Status: **OPEN**

P0-18 makes device-chain topology durable. A track can declare ordered
pre-fader and post-fader placements, typed event-to-event, event-to-audio, and
audio-to-audio slots, format-neutral bindings, bypass and wet/dry values, and a
content-addressed state reference. Those declarations are sufficient for
editing, persistence, interchange loss accounting, and later host resolution.

They are not a runtime lowering contract. In particular, P0-18 does not:

- instantiate a plugin or select a plugin format;
- lower note effects into the playback event stream;
- define latency discovery for event processors;
- shift events sample-accurately to compensate an event processor; or
- reconcile event latency with downstream audio latency and live monitoring.

Until this decision closes, a playback compiler must not infer that a typed
event slot is executable merely because the document accepts it. Unsupported
event-chain lowering must fail explicitly instead of bypassing a declaration,
dropping events, or applying an unrecorded timing shift.

## Decision required

The eventual design must specify a platform-neutral latency unit and reporting
lifecycle, how latency changes invalidate immutable programs, how event and
audio paths align at an event-to-audio boundary, and whether live input uses the
same compensation policy as scheduled playback. Acceptance must include tempo
changes, block-boundary crossings, dynamic latency changes, offline/realtime
equivalence, and a causal test that fails when the compensating event shift is
removed.

No current Timeline schema or command promises an answer to those questions.
