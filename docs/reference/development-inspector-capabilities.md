# Development inspector capabilities

The development inspector is an opt-in platform under construction. In a
GPU-enabled desktop build with a compatible window host,
`pulp run --inspect[=PROFILE]` constructs an authenticated network session for
the selected standalone window and displays a visual Cmd+I indicator. Pulp's
built-in macOS standalone window hosts currently provide the required event-loop
exit drain and deferred-close turn. Windows and Linux use external `WindowHost`
factories; an external host must explicitly implement both contracts before
active inspector profiles are accepted. A run without `--inspect`, a host
without those contracts,
a GPU-disabled or mobile build, and every plugin-format launch constructs no
endpoint. The installed Rust `pulp`, sibling `pulp-cpp`, and `pulp-mcp` clients
can discover and authenticate to an explicitly activated endpoint without a
source checkout.

This page records the checked baseline so public descriptions do not confuse
code presence with runtime reachability.

## Capability contract

Every protocol method is assigned exactly one stable capability in
`inspect/include/pulp/inspect/protocol_methods.inc`. Capability IDs, risk,
grantability, and named-profile membership live in
`inspect/include/pulp/inspect/capability_definitions.inc`. The C++ registries
reject duplicate method/capability IDs at compile time and test every ID round
trip.

| Capability | `observe` | `develop` | Current reality |
|---|---:|---:|---|
| `session.describe` | yes | yes | The standalone owner publishes identity, agent context, and authenticated capability reporting |
| `session.control` | no | yes | One-controller lease with expiry, renewal, disconnect release, and structured conflict errors |
| `state.read` | yes | yes | The standalone session exposes its exact `StateStore` parameter catalog and values |
| `ui.read` | yes | yes | The standalone session exposes its live view tree and value-channel catalog |
| `diagnostics.read` | yes | yes | Agent context and audio configuration are attached; individual performance sources may report unavailable |
| `logs.read` | yes | yes | Scripted-UI console capture remains attached across in-place reloads |
| `capture.image` | yes | yes | Advertised only when the initial standalone tree has an honest live or portable whole-window capture route; each request revalidates reload-sensitive native-overlay/GPU requirements; node capture remains unavailable |
| `trace.control` | no | yes | Domain components exist but the standalone owner does not advertise them without a trace binding |
| `trace.session.control` | no | yes | Process-global Trace sessions require a publication-scoped binding |
| `state.write` | no | yes | The `develop` standalone profile applies legal parameter mutations on the main thread after acquiring the same-connection controller lease |
| `test.input` | no | yes | `Test.injectMidi` accepts bounded note-on/off events and `Test.setTransport` applies coherent standalone play/position/tempo updates through the normal host path |
| `authoring.tweaks` | no | yes | Transient tweaks, highlight, bypass, lock, live constants, editor URL templates, and repaint flashing stay in this capability; filesystem and editor-launch methods remain unavailable |
| `telemetry.stream` | no | yes | The standalone owner claims the value-channel telemetry sidecars only when this capability is effective, then provides bounded contextual snapshots and per-client targeted subscriptions |
| `runtime.eval` | no | no | High-risk separate opt-in; `--inspect-runtime-eval` is required in addition to a controller-capable develop/custom selection |
| `unavailable` | no | no | Filesystem-backed tweak/fixture operations and editor launch are classified unavailable for the future policy |

`off` grants nothing. `custom` starts from an empty exact allow-list. These are
enforced policy definitions. `develop` deliberately excludes `runtime.eval`.
The launcher can add it only through the literal `--inspect-runtime-eval`
acknowledgement; custom also has to name `runtime.eval` and `session.control`.
The acknowledgement is one-run state and is not part of standalone persisted
preferences.

## Checked implementation matrix

| Area | Present | Missing |
|---|---|---|
| Constructor/reachability | Explicit `pulp run --inspect[=PROFILE]` activation constructs one authenticated owner for a compatible GPU desktop standalone window; ordinary and plugin-format launches remain endpoint-free | Additional host-format ownership |
| Window host | Built-in macOS standalone hosts keep their owning-thread dispatcher alive after native-loop stop until accepted inspector work retires, and schedule startup-failure close on a later native event turn | Windows/Linux external factories must implement `event_loop_supports_exit_drain()` with `run_event_loop_until()`, plus `supports_deferred_close()` with `request_close_deferred()`, to opt into active profiles |
| Build/link/install | Optional protocol, reader discovery, neutral discovery-path support, publisher/runtime, client, and authoring targets are component-gated and separate from the GPU overlay. Publisher/runtime link closure does not grant reader authority; an installed consumer checks that split, and an ordinary `pulp::format` fixture proves no inspector symbols are present | Per-target shipped-product declaration and final product-manifest proof |
| Threading | The standalone owner uses bounded owning-thread RPC, responds after timely application, cancels queued work during teardown, and fences started timeouts as `mayHaveApplied` while discarding late responses. Reload generations rebind owned channel metadata, the sole telemetry attachment, and scripted inspector sources on the UI tick | Additional host-format ownership |
| Discovery/security | owner-private ephemeral record/token files, exclusive session/instance publication, non-reusable publication generations, exact publication selection, mutual nonce/HMAC transcript proofs, replay rejection, auth/I/O timeouts, teardown, and one-controller lease | None for the explicitly activated standalone path |
| CLI | `pulp inspect profiles/list/capabilities/doctor` and typed parameter/MIDI/transport mutations provide stable JSON; every live operation uses exact session/instance/publication targeting through the shared client | Telemetry subscription lands in the next phase |
| MCP | Installed in-process shared client exposes profiles/list/capabilities/doctor plus typed parameter, MIDI, and transport tools; success carries publication identity and failures carry structured code/message/data | Telemetry subscription lands in the next phase |
| Capture/telemetry | Whole-window in-process capture (live host back-buffer when available, portable view rendering otherwise), owned value-channel metadata, snapshots, and bounded scalar/meter/vector/event subscriptions are attached to the standalone session; delivery is targeted by authenticated client identity and carries explicit source, stale, coalescing, overflow, and transport-loss state | Node capture, external-host compositing, and CLI/MCP watch commands |
| Shipping | The component gate removes inspector targets and CLI commands fail explicitly when disabled; ordinary-format symbol stripping is continuously checked | Per-target declaration, shipped-product manifest, and override proof |

The production server binds loopback only and requires fresh, role-separated
nonce/HMAC proofs from both client and server using an owner-private
per-session credential. Discovery rejects expired or dead publishers,
duplicate live publisher identities, stale publication generations, insecure
mode bits or extended ACLs, path escapes, and ambiguous selection. Newly
created Darwin discovery objects
discard inherited ACLs before any credential material is written; readers
validate the opened object and fail closed on any remaining extended ACL.
Rejected server starts wipe their owned credential before releasing storage.
Capability dispatch is fail-closed before a domain handler runs. The old
unauthenticated direct-handler server exists only as a non-installed test
fixture for transport regression coverage.
Authenticated connections may wait idle for their next frame, but once any
header byte arrives the complete length-prefixed frame must arrive within a
bounded cumulative deadline. Partial headers and payloads are disconnected so
they cannot retain every bounded client slot.
After a complete request frame is sent, a response timeout or disconnect is
explicitly reported as `mayHaveApplied`; timeouts fence the connection so a
late response cannot be mistaken for a safe retry boundary.

Build presence, host wiring, profile allowance, and current enablement are
separate facts. `Session.getCapabilities` reports the available and effective
sets for an authenticated session; no client should infer one from another.

## Typed test input and authoring boundary

`test.input` is deliberately narrow. `Test.injectMidi` accepts only `note_on`
and `note_off`, public channels 1–16, note/velocity bytes 0–127, and no raw
status bytes, SysEx, CC, timestamp, path, or script. Outstanding injected notes
belong to the controller session and are released when its lease is released,
expires, disconnects, or the session tears down. The installed one-shot clients
require a 1–2000 ms hold for note-on and send the matching note-off on the same
controller connection before releasing the lease. `Test.setTransport` accepts an
idempotent partial update containing at least one of `playing`, nonnegative
`position_samples`, or finite `tempo_bpm` from 20 through 400. Both operations
run through the owning thread and normal standalone host/processor path.

Numeric parameter changes remain `state.write`. Transient authoring controls
remain `authoring.tweaks`. Generic preset load/save, filesystem tweak
load/save, source jump, raw MIDI, and arbitrary UI scripting are not test-input
shortcuts; those methods remain unavailable. `Runtime.evaluate` is never an
implementation path for MIDI, transport, parameters, or authoring controls.

## Client evidence loop

A client first runs `pulp inspect list --json` (or
`pulp_inspect_list`) and pins the returned session, instance, and publication
IDs. It authenticates `capabilities` with those exact IDs, reads the typed state,
performs only a capability-authorized typed mutation, rereads, and optionally
captures the selected window. The publication ID is non-reusable; a missing or
changed publication requires rediscovery. `Runtime.evaluate` is never a
parameter or test-input mutation path.

## Live value-channel telemetry

`Telemetry.getSnapshot`, `Telemetry.subscribe`, and `Telemetry.unsubscribe`
are available only when `telemetry.stream` is effective. `observe` and custom
profiles that omit the capability do not claim or drain the exclusive
telemetry reader. Client identity is taken from the authenticated connection,
never from request JSON. Versioned response/event schemas are
`pulp.inspect.telemetry.snapshot.v1`,
`pulp.inspect.telemetry.subscription.v1`, and
`pulp.inspect.telemetry.sample.v1`.

A request may select at most 32 channel names. Subscriptions are limited to one
per client, default to 15 Hz, and are capped at 60 Hz. `maxVectorValues` is
bounded by the broker configuration; event and wire payloads are bounded too.
Slow-client loss is isolated by subscription: attempt sequence advances even
when delivery drops, and the next successful sample reports
`transportDroppedSincePrevious`. Continuous channels report only snapshots the
UI reader actually consumed. Their timestamp is therefore an inspector/UI
snapshot time, not a producer or audio-clock timestamp. Events use the bounded
producer tap, preserve zero-valued occurrences, and report cumulative source
overflow in snapshots plus since-delivery overflow in subscriptions, separately
from transport loss.

Every channel reports availability, source lifetime, publication/sequence,
staleness reason, coalescing, source drops, payload size, and its typed payload.
Non-finite DSP values use the inspector-wide string sentinels `NaN`, `Infinity`,
and `-Infinity` instead of collapsing anomaly evidence to JSON `null`.
Source destruction produces one terminal sample. On a successful processor
reload the subscription ID and requested channel names survive, the source
generation advances, and the first sample is marked `reattached`; removed names
remain explicit unavailable entries. The broker is the sidecars' sole reader
and runs on the serialized UI/control pump. Audio publication remains the
existing allocation-free, lock-free source/tap path and performs no JSON or
network work.
