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
code presence with runtime reachability. The trust boundaries and explicit
non-claims are defined in the
[capability-control threat model](../policies/capability-control-threat-model.md).

## Capability contract

Every protocol method is assigned exactly one stable capability in
`inspect/include/pulp/inspect/protocol_methods.inc`. Capability IDs, risk,
side effect, executor, evidence, grantability, and named-profile membership live in
`inspect/include/pulp/inspect/capability_definitions.inc`. The C++ registries
reject duplicate method/capability IDs at compile time and test every ID round
trip. The frozen registry also declares per-operation input/output schema IDs
and digests, required build feature, runtime contexts, host tiers, activation,
policy and grant scope, cancellation/timeout behavior, and compatibility and
deprecation state. Its canonical SHA-256 is embedded in each artifact manifest;
changing any of those fields invalidates the prior manifest identity.

Operation schemas are closed, versioned, and resource-bounded. Every string
value has an explicit maximum; required resource and idempotency identifiers
also have a nonzero minimum. Capture has separate closed window/node variants,
state parameter IDs use the complete unsigned 32-bit range, test transport is
limited to the executor's 20–400 BPM domain, and telemetry accepts at most 32
unique nonempty channel IDs. These are registry contract bytes, so relaxing or
narrowing a bound requires a new digest and consent identity.

Trace session control matches the concrete `Trace.startSession`/
`Trace.stopSession` adapter: start accepts unique categories bounded to 128
entries and 128 UTF-8 bytes each, plus a
1–512 MiB ring, the host-main executor enforces the same limits before capture,
and the raw response is the declared evidence. Publication attachment remains
host-owned lifecycle.
The separate trace-control operation uses closed action variants for the
Performance, Audio, and Motion host-main adapters, including bounded motion
metrics; its receipt can carry the motion trace ID needed by a later stop.
The action-discriminated motion-start receipt requires that ID; other action
receipts cannot smuggle one. Integer-valued JSON numbers use Draft 2020-12
numeric semantics, so spellings such as `15.0` are accepted when finite and in
range. Pulp applies a source-owned CHOC compatibility patch so the equivalent
standards-valid `15e+0` spelling reaches the same executor validation.
Authoring bypass and lock changes require a nonempty anchor of at most 256
Unicode codepoints; DOM highlight
requires an exact node, is capped at 256 UTF-8 bytes, and changes overlay selection instead of
returning false success. Runtime evaluation additionally rejects NUL and
enforces a 65,536-byte UTF-8 ceiling (`x-pulp-maxUtf8Bytes`), because JSON
Schema `maxLength` alone counts characters rather than encoded bytes.

The `dev.pulp.*@1` IDs below are the canonical authoring and broker contract.
The shorter Inspector IDs are compatibility spellings only. A shipped manifest
is an upper bound, never a grant. Effective authority requires all seven terms:
`implemented`, `built`, `host_available`, `activated`, `policy_eligible`,
`client_granted`, and `session_live`. Missing terms deny by default and surface
one stable reason such as `not-built`, `client-not-granted`, or
`session-not-live`.

| Canonical capability (legacy spelling) | `observe` | `develop` | Current reality |
|---|---:|---:|---|
| `dev.pulp.instance/read@1` (`session.describe`) | yes | yes | The standalone owner publishes identity, agent context, and authenticated capability reporting |
| `dev.pulp.session/control@1` (`session.control`) | no | yes | One-controller lease with expiry, renewal, disconnect release, and structured conflict errors |
| `dev.pulp.state/read@1` (`state.read`) | yes | yes | The standalone session exposes its exact `StateStore` parameter catalog and values |
| `dev.pulp.render/offline@1` (`render.offline`) | no | no | Frozen Product A contract; no current executor or grant path |
| `dev.pulp.ui/observe@1` (`ui.read`) | yes | yes | The standalone session exposes its live view tree and value-channel catalog |
| `dev.pulp.diagnostics/read@1` (`diagnostics.read`) | yes | yes | Agent context and audio configuration are attached; individual performance sources may report unavailable |
| `dev.pulp.logs/read@1` (`logs.read`) | yes | yes | Scripted-UI console capture remains attached across in-place reloads |
| `dev.pulp.ui/capture@1` (`capture.image`) | yes | yes | Advertised only when the initial standalone tree has an honest live or portable whole-window capture route; each request revalidates reload-sensitive native-overlay/GPU requirements; node capture remains unavailable |
| `dev.pulp.ui/input@1` (`ui.input`) | no | no | Frozen high-risk Product A contract; no current executor or grant path |
| `dev.pulp.trace/control@1` (`trace.control`) | no | yes | Domain components exist but the standalone owner does not advertise them without a trace binding |
| `dev.pulp.trace/session-control@1` (`trace.session.control`) | no | yes | Process-global Trace sessions require a publication-scoped binding |
| `dev.pulp.state/parameter-gesture@1` (`state.write`) | no | yes | The `develop` standalone profile applies legal parameter mutations on the main thread after acquiring the same-connection controller lease |
| `dev.pulp.test/input@1` (`test.input`) | no | yes | `Test.injectMidi` accepts bounded note-on/off events and `Test.setTransport` applies coherent standalone play/position/tempo updates through the normal host path |
| `dev.pulp.authoring/tweaks@1` (`authoring.tweaks`) | no | yes | Transient tweaks, exact-node highlight, anchored bypass/lock, live constants, editor URL templates, and repaint flashing stay in this capability; filesystem and editor-launch methods remain unavailable |
| `dev.pulp.telemetry/subscribe@1` (`telemetry.stream`) | no | yes | The standalone owner claims the value-channel telemetry sidecars only when this capability is effective, then provides bounded contextual snapshots and per-client targeted subscriptions |
| `dev.pulp.runtime/reload@1` (`runtime.reload`) | no | no | Frozen Product A contract; no current executor or grant path |
| `dev.pulp.runtime/evaluate@1` (`runtime.eval`) | no | no | High-risk separate opt-in; `--inspect-runtime-eval` is required in addition to a controller-capable develop/custom selection |
| `dev.pulp.artifact/read@1` (`artifact.read`) | no | no | Frozen publication-bound Product A contract; no current executor or grant path |
| `dev.pulp.unavailable/operation@1` (`unavailable`) | no | no | Filesystem-backed tweak/fixture operations and editor launch are classified unavailable for the future policy |

`off` grants nothing. `custom` starts from an empty exact allow-list. These are
enforced policy definitions. `develop` deliberately excludes `runtime.eval`.
The launcher can add it only through the literal `--inspect-runtime-eval`
acknowledgement; custom also has to name `runtime.eval` and `session.control`.
The acknowledgement is one-run state and is not part of standalone persisted
preferences.

## Dormant broker service and shared client foundation

The optional `pulp::inspect-control` component contains the broker-owned
identity, registration, grant, typed admission, durable receipt, cancellation,
quota, progress, and artifact-lineage state needed by the capability-control
migration. It is deliberately dormant in this phase: it opens no listener, is
not linked into ordinary plugin-format artifacts, and does not replace the
current explicitly activated standalone inspector transport. The installed
`ControlClient` accepts a typed `ControlClientTransport` representing one
authenticated, connection-bound peer and client identity; its artifact-read API
therefore has no caller-supplied client ID. The transitional `InspectorClient`
constructor carries envelope dispatch over the existing path but fails artifact
reads closed. Phase 3c will supply the canonical carrier without adding new
coupling to the legacy `InspectorSession`/server that it replaces.
`ControlService` accepts a carrier-verified peer and connection-bound client
identity, but has no executor unless a later runtime adapter injects one.
Per-user service activation, consent UI, and live Inspector/Remote View
migration remain later work.

The foundation accepts only carrier-observed `VerifiedControlPeerIdentity`
values minted by the broker's peer verifier. Its fingerprint binds the peer
role, UID/SID, PID, process-start generation, executable identity, and verified
publisher. Payload claims and same-user status alone are insufficient. A
launcher bootstrap is single-use, short-lived, bound to that exact fingerprint,
consumed even after a wrong-peer attempt, and wiped on consumption, expiry, or
destruction. The macOS carrier gathers and validates OS peer evidence before
the composition root accepts it. Other platforms remain fail-closed until they
gain an equivalent credential-bearing verifier.

Registration is limited to Pulp-owned T0 offline jobs and T1 standalone hosts.
It validates the complete canonical control manifest and exact artifact digest,
derives the consent identity, and binds an opaque registration to one exact
session, instance, publication, peer generation, and lease. Empty or
"latest" selection is unavailable. Shared plugin hosts, including direct AUv3,
fail with `host-unavailable` until a separately reviewed trusted-host bridge can
attest the exact loaded slot.

Grant issuance requires a live exact client and registration, a capability
subset present in that validated manifest, bounded expiry, and approval from a
trusted Pulp CLI, trusted host UI, or existing user policy. Plugin UI and agent
client assertions cannot approve a grant. Interactive consent decision IDs are
single-use; durable policy IDs may be reused within their policy scope. Broker
restart, client disconnect, registration disappearance, expiry, or explicit
revocation removes authority. The bounded metadata audit records identities,
decisions, and stable reason codes, never bootstrap secrets, consent text, or
operation payload values.

A stored grant establishes only the `client_granted` term. It does not activate
an endpoint, route an operation, or bypass the other six permission terms.

Each service session negotiates its own protocol version and mandatory receipt
support before request or cancellation dispatch; progress is available only
when that session negotiated it. Admission validates JSON parameters against
the resolved operation's input schema before writing an authority-bound
idempotency receipt. Successful executor output is validated against the same
operation's output schema before a completed receipt is persisted. Operations
whose typed result exposes `receipt_id` explicitly bind that field to the
broker-minted durable receipt; a mismatched executor result fails closed and is
persisted as an internal failure. Unsupported or malformed schema keywords fail
closed. Exact replay returns the existing receipt without a second dispatch.

Request parameters, result details, and complete wire envelopes have distinct
bounded budgets (512 KiB, 1,600 KiB, and 2 MiB respectively), including bounded
JSON node counts. Bulk UI-tree, diagnostics, and log results use artifact
handles instead of expanding those receipt budgets; artifact reads retain their
bounded one-mebibyte chunk contract.

The broker checks deadlines and atomically enforces active-operation quotas.
Trusted in-process executors must return within that bound or promptly return a
deferred outcome; the service cannot preempt arbitrary C++ in its own process.
The supplied main-thread adapter enforces the contract with bounded fenced RPC.
If already-started legal-thread work exceeds its response deadline, the response
is `unknown-needs-refresh`, while the durable receipt remains `running` and
retains its quota slot until deferred completion settles it. Cancellation intent
is durable. Progress events are receipt-bound, monotonic, bounded, and subject
to carrier backpressure.

Phase 3b's artifact support is intentionally minimal. Publication is blob-first;
a terminal receipt may name only a stored artifact with matching producer
lineage, and broker-mediated reads reauthorize the original grant, complete
lineage, terminal receipt, metadata, and expiry. Per-blob and read-chunk limits
exist, but aggregate quota, retention collection, deletion audit, redaction, and
generalized ACL policy remain Phase 7. Expired metadata is removed lazily and
orphaned content-addressed blobs may remain. Owner-private filesystem modes
exclude other OS users, not malicious processes running under the same UID;
broker authorization is not an at-rest secrecy boundary against such a process.

## Checked implementation matrix

| Area | Present | Missing |
|---|---|---|
| Constructor/reachability | Explicit `pulp run --inspect[=PROFILE]` activation constructs one authenticated owner for a compatible GPU desktop standalone window; ordinary and plugin-format launches remain endpoint-free | Additional host-format ownership |
| Window host | Built-in macOS standalone hosts keep their owning-thread dispatcher alive after native-loop stop until accepted inspector work retires, and schedule startup-failure close on a later native event turn | Windows/Linux external factories must implement `event_loop_supports_exit_drain()` with `run_event_loop_until()`, plus `supports_deferred_close()` with `request_close_deferred()`, to opt into active profiles |
| Build/link/install | Optional protocol, reader discovery, neutral discovery-path support, publisher/runtime, client, authoring, and dormant `pulp::inspect-control` targets are component-gated and separate from the GPU overlay. Installed protocol/control/client components expose the fail-closed broker, typed service, and shared client but no listener. A mandatory non-slow clean-prefix consumer compiles and runs those installed targets while rejecting direct GPU/render/format/host/CLI/MCP closure. Publisher/runtime link closure does not grant reader authority; an ordinary `pulp::format` fixture proves no inspector symbols are present | Per-target shipped-product declaration and final product-manifest proof |
| Threading | The standalone owner uses bounded owning-thread RPC, responds after timely application, cancels queued work during teardown, and fences started timeouts as `mayHaveApplied` while discarding late responses. Reload generations rebind owned channel metadata, the sole telemetry attachment, and scripted inspector sources on the UI tick | Additional host-format ownership |
| Discovery/security | The explicitly activated standalone path retains owner-private ephemeral record/token files, exact publication selection, mutual nonce/HMAC proofs, replay rejection, timeouts, teardown, and one-controller lease. Separately, the dormant broker composition root owns identity-bound single-use bootstrap, exact T0/T1 registration, trusted-consent grants, lifecycle revocation, per-session negotiated envelopes, per-operation input/output schema enforcement, durable replay receipts/cancellation, active-operation quotas, and original-lineage broker reads from the minimal artifact store. Its macOS local carrier binds accepted-socket UID/GID/PID and audit-token PID generation to a rechecked live code-signing identifier, CDHash, and Team or per-artifact ad-hoc identity; insecure endpoint parents, TCP/FIFO identity, malformed UTF-8/JSON, dead peers, and mismatches fail closed | Canonical per-user service activation, trusted consent surface, non-macOS verified-peer implementations, migration of live operations to that path, and Phase 7 aggregate artifact quota/retention/redaction/deletion policy; owner-private files are not secret from a same-UID process |
| CLI | `pulp inspect profiles/list/capabilities/doctor` and typed parameter/MIDI/transport mutations provide stable JSON; every live operation uses exact session/instance/publication targeting through the shared client | Telemetry subscription lands in the next phase |
| MCP | Installed in-process shared client exposes profiles/list/capabilities/doctor plus typed parameter, MIDI, and transport tools; success carries publication identity and failures carry structured code/message/data | Telemetry subscription lands in the next phase |
| Capture/telemetry | Whole-window in-process capture (live host back-buffer when available, portable view rendering otherwise), owned value-channel metadata, snapshots, and bounded scalar/meter/vector/event subscriptions are attached to the standalone session; delivery is targeted by authenticated client identity and carries explicit source, stale, coalescing, overflow, and transport-loss state | Node capture, external-host compositing, and CLI/MCP watch commands |
| Shipping | The component gate removes live inspector targets and the dormant control core; live CLI commands fail explicitly when disabled, while read-only `inspect audit` remains available. Ordinary-format symbol stripping, per-target declarations, canonical manifests, and manifest-versus-binary checks are continuously tested. The macOS carrier and peer verifier are linked only into the optional headless control target and open no endpoint by themselves | Signed service distribution, broker-owned consent proof, and platform parity beyond the fail-closed macOS v1 verifier |

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

### End-to-end validation boundary

The checked source workflow starts three independent standalone processes in
one discovery directory: an ordinary `develop` session, an `observe` session,
and a deliberately capability-minimal runtime-evaluation session. It selects
each process by the exact session, instance, and publication IDs that process
published. This proves ambiguous selection fails closed, observe reads work
while state mutation is denied, and runtime evaluation is unavailable without
the separate opt-in. The minimal evaluation process proves a successful typed
result plus the 64 KiB request bound without weakening the effectful live
realm used by the ordinary develop process.

The same real-process workflow proves controller acquisition, typed parameter,
transport, and MIDI mutation ordering; compositor-backed PNG capture; scalar,
vector, event, and deliberately stale value-channel snapshots; slow-client
attempt sequencing and explicit source/coalescing loss; generation-changing
reload reattachment of DOM, logs, value telemetry, and runtime-evaluation realm
authority; and independent record, credential, and lock teardown.
Those waits advance from observed process, protocol, or sequence state rather
than assuming a fixed elapsed delay. Processor-owned scripted sessions can
explicitly opt into in-place reload on the stable host root, preserving the
session that owns inspector and GPU-surface attachments instead of replacing it
through `create_view()`. Non-opt-in or replacement-session generations remain
pending and fail closed.

A separate packaged-client workflow starts exact `develop` and `observe`
processes and drives both the installed Rust `pulp inspect` client and the
installed marketplace `pulp-mcp` client. It covers discovery, capability,
context, parameter, DOM, capture, transport, MIDI, and typed mutation reads or
writes without a source-tree client path. An independent source scan rejects
production protocol literals that are absent from
`protocol_methods.inc`; its self-test injects an unmapped method and requires
the check to fail.

These proofs apply to the enabled development build described above. The
ordinary-launch endpoint-free and disabled-component gates remain separate
tests. Final shipped-product manifest, per-target declaration, and shipping
override proof depend on the Phase 7 composition and are not claimed here.

### Live-realm runtime evaluation boundary

`runtime.eval` is refused when the attached `ScriptedUiSession` has any
effectful `ReloadCapability`: `exec`, `clipboard`, `filesystem`, `storage`,
`ai`, `runtime_import`, or `network`. The inspector reads the immutable grant
set installed in the live `WidgetBridge`; it does not mask names in
`globalThis`, because hiding names inside the same reachable realm is not a
security boundary. `Runtime.getCapabilities` reports `canEvaluate:false` and
an exact `evaluateDeniedReason` after an unsafe session is attached.

The framework-owned `build_editor_ui` path retains its historical
`CapabilitySet::all()` posture and therefore rejects `--inspect-runtime-eval`.
A production host or custom processor that needs evaluation must explicitly
construct `ScriptedUiSession` with an empty
`ScriptedUiOptions::granted_capabilities` set. That reviewed set is retained
across hot reloads and checked again whenever the standalone host binds a
replacement scripted-UI session.

The arbitrary-execution adapter is compiled into the separate
`pulp-inspect-runtime-eval` archive and injected through the narrow
`RuntimeEvaluator` interface. The base inspector, protocol, transport, client,
and ordinary format archives do not depend on that component or contain its
high-risk binary marker. Requests are limited to 64 KiB of decoded code, use a
fixed two-second deadline, and reject serialized results or encoded responses
over 1 MiB. Result bytes, nesting depth, and cycles are bounded during QuickJS
traversal. The scripted realm is rebuilt from source after each evaluation,
preserving widget values but discarding deferred callbacks and global
mutations before the next frame pump. That rebuild has a fixed 500 ms cleanup
grace inside a three-second outer RPC fence; a failed rebuild destroys the
engine fail-closed. One owned, bounded server worker keeps the controller's
authenticated connection free to send `Runtime.interrupt` while evaluation is
in flight, and is included in the server's module-unload shutdown fence. These
limits compose with the bridge's single-flight, cooperative interrupt,
engine-detach, and teardown fences.

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
