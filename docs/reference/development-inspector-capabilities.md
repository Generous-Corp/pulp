# Development inspector capabilities

The Development Inspector capability-control platform is centralized behind
the canonical per-user broker. This registry inventory is paired with the
consolidated [Capability control](capability-control.md) authoring workflow.

The canonical broker evidence, bounded telemetry, and T1/T2a execution
contracts are documented in [Control evidence and telemetry](control-evidence-and-telemetry.md).
Phase 3 deleted the legacy server, raw client, discovery, and standalone
authority. `pulp inspect` remains deliberately static/offline; live typed
operations use `pulp control` and the generated `pulp_control_*` MCP family.
Source components, schemas, or a target declaration do not imply live
reachability.

The earlier temporary capability reduction while Phases 4–7 built that
replacement is now closed for the typed T0/T1 client foundation. It did not
preserve a legacy fallback: operations without a canonical executor or grant
remain unavailable.

This page records the checked baseline so public descriptions do not confuse
implemented building blocks with an activated authority path. The trust
boundaries and explicit non-claims are defined in the
[capability-control threat model](../policies/capability-control-threat-model.md).

## Capability contract

Every protocol method is assigned exactly one stable capability in
`inspect/include/pulp/inspect/protocol_methods.inc`. Capability IDs, risk,
side effect, executor, evidence, grantability, and named-profile membership live
in `inspect/include/pulp/inspect/capability_definitions.inc`. The C++ registries
reject duplicate method/capability IDs and test every ID round trip. The frozen
registry also declares operation schemas and digests, build feature, runtime
contexts, host tiers, activation, policy/grant scope, cancellation/timeout
behavior, and compatibility state.

Operation schemas are closed, versioned, and resource-bounded. A shipped
manifest is an upper bound, never a grant. Effective authority requires all
seven terms: `implemented`, `built`, `host_available`, `activated`,
`policy_eligible`, `client_granted`, and `session_live`. Missing terms deny by
default. Capability dispatch is fail-closed before an executor runs.

The profile columns below are static policy membership, not current runtime
availability.

| Canonical capability (legacy spelling) | `observe` | `develop` | Current reality |
|---|---:|---:|---|
| `dev.pulp.instance/read@1` (`session.describe`) | yes | yes | Broker-owned T0/T1 executor returns the exact active registration, tier, publication generation, build/artifact identity, liveness generation, and declared capabilities after canonical admission |
| `dev.pulp.session/control@1` (`session.control`) | no | yes | Broker lease/grant machinery exists; no general product host adapter |
| `dev.pulp.state/read@1` (`state.read`) | yes | yes | T0/T1 runtime executor returns bounded parameter catalog/value snapshots against the shared `StateStore` mutation generation, with explicit sensitive-field redaction; CLI/MCP use the canonical typed client |
| `dev.pulp.render/offline@1` (`render.offline`) | no | no | T0-only headless executor resolves authority-bound, launcher-trusted in-memory inputs, renders through `OfflineRenderHost`, and publishes broker-owned WAV artifacts; no profile enables it implicitly |
| `dev.pulp.ui/observe@1` (`ui.read`) | yes | yes | Typed contract/components exist; no general product host adapter |
| `dev.pulp.diagnostics/read@1` (`diagnostics.read`) | yes | yes | Typed contract/components exist; no general product host adapter |
| `dev.pulp.logs/read@1` (`logs.read`) | yes | yes | Console component exists; no public live route |
| `dev.pulp.ui/capture@1` (`capture.image`) | yes | yes | The exact-instance main-thread executor reuses `InspectorCaptureSource` for bounded window PNGs and the Pulp-owned exact-target adapter for node PNGs; both publish sensitive ACL-bound broker artifacts |
| `dev.pulp.ui/input@1` (`ui.input`) | no | yes | Grant-controlled ordinary Standalone composition accepts one bounded pointer, keyboard, focus, or UTF-8 text event for an exact registration/view-generation/node target on the fenced main thread; the installed-host seam binds retained state to a broker-projected opaque authority and subscribes exact-owner cleanup to revoke, expiry, disconnect, and teardown |
| `dev.pulp.trace/control@1` (`trace.control`) | no | yes | Injected exact-T1 main-thread Motion executor provides authority-bound geometry/scroll trace ownership, bounded preloaded-fixture scrub/play/pause, and finite redacted cost snapshots; no generic raw Inspector route |
| `dev.pulp.trace/session-control@1` (`trace.session.control`) | no | yes | `pulp trace start/stop` and matching MCP tools use canonical control only; the reusable host observability bundle dispatches the exact admitted instance when an adapter publishes it |
| `dev.pulp.state/parameter-gesture@1` (`state.write`) | no | yes | T1 main-thread exact-slot executor atomically claims the shared `StateStore` generation and rolls back failed brackets without overwriting newer writers; broker grant/consent remains mandatory |
| `dev.pulp.test/input@1` (`test.input`) | no | yes | Typed executor building block exists; no public general-live route |
| `dev.pulp.authoring/tweaks@1` (`authoring.tweaks`) | no | yes | In-process authoring components remain; remote wrapper was removed |
| `dev.pulp.telemetry/subscribe@1` (`telemetry.stream`) | no | yes | The host observability bundle exposes typed `subscribe`, `poll`, and `unsubscribe` actions over the bounded/redacting tap; exact host publication still determines availability |
| `dev.pulp.runtime/reload@1` (`runtime.reload`) | no | no | Frozen contract; no current executor or grant path |
| `dev.pulp.runtime/evaluate@1` (`runtime.eval`) | no | no | Research-unsafe acknowledged manifests may inject the bounded exact-instance evaluator; grants require broker-owned single-use consent, and results/errors are size-bounded and redacted |
| `dev.pulp.artifact/read@1` (`artifact.read`) | no | no | Publication-bound typed client rechecks exact original lineage and broker ACL for every chunk |
| `dev.pulp.unavailable/operation@1` (`unavailable`) | no | no | Filesystem/editor-launch operations remain unavailable by policy |

`off` grants nothing. `custom` starts from an empty exact allow-list. `develop`
deliberately excludes `runtime.eval`; no profile or target declaration implies
that high-risk authority.

## Canonical control foundation

The optional `pulp::inspect-control` component contains broker-owned identity,
registration, grant, typed admission, receipt, cancellation, quota, progress,
artifact-lineage, local carrier, trusted-host inventory, and launcher
foundations. `pulp::inspect-client` is the canonical control client, not the
deleted raw Inspector client. The optional macOS `pulp-control-broker` owns one
per-user `LocalSocket` endpoint and composes enrollment, host routing,
execution, service, and endpoint ownership. Trusted T0/T1 enrollment can
publish exact registrations; unsupported tiers and missing executors fail
closed.

Darwin CLI installs place the broker beside `pulp` and `pulp-cpp` and reconcile
the owner-only `dev.pulp.control-broker` LaunchAgent. A successful
reconciliation proves only `reachable-unverified`; install-time code-signature
validation is an integrity check, not a publisher-trust or authorization
decision. Canonical `~/.pulp/bin` installs activate automatically. A custom
install root requires explicit acceptance on first install, and an upgrade may
reuse it only when the existing owned plist already names that exact broker
path. Ephemeral socket and liveness files remain separate from owner-private
durable receipts and artifacts under `~/.pulp/state/control-broker/v1`; service
stop or removal leaves that durable state intact.

The installed `ControlClient` accepts a typed `ControlClientTransport`
representing one authenticated, connection-bound peer and client identity; its
artifact-read API therefore has no caller-supplied client ID. `ControlService`
accepts a carrier-verified peer and connection-bound client identity, but has no
executor unless a runtime adapter injects one. The deleted legacy
`InspectorSession`/server is not a compatibility transport or a second
capability-control authority path.

The control path validates bounded schemas, exact grants, deadlines,
idempotency, replay, cancellation, operation quotas, and receipt lineage. The
local carrier binds peer process identity and rejects insecure endpoint parents,
unsupported transports, malformed input, dead peers, and identity mismatches.
These protections do not make an owner-private file secret from malicious code
already running as the same OS user.

`pulp control` and the generated `pulp_control_*` MCP family are the general
typed clients. Trace lifecycle is a narrow facade over the same client:
`pulp trace start`/`stop` accept an optional exact broker-owned `--instance ID`;
when omitted, the canonical opener retains its fail-closed unambiguous-selection
behavior. They and `pulp_trace_start`/`stop` accept no raw host/port or legacy
publication selector and have no legacy Inspector fallback. Offline
`trace query --trace`, `doctor`, `fetch`, and `open` do not require a live
target.

## Checked surface matrix

| Area | Present now | Not yet public |
|---|---|---|
| CLI | `pulp control profiles`; offline `pulp inspect audit ARTIFACT`; exact-instance `pulp control` management/call/watch/artifact/revoke; canonical trace start/stop; offline trace analysis. `pulp inspect profiles` is a compatibility alias through Pulp 0.800.0 on 2026-10-01. | Raw Inspector discovery/RPC, host/port selectors, newest-instance selection, and Motion wrappers |
| MCP | In-process `pulp_control_profiles`; generated typed `pulp_control_*` operations and management tools; canonical `pulp_trace_start` and `pulp_trace_stop`. `pulp_inspect_profiles` is a compatibility alias through Pulp 0.800.0 on 2026-10-01. | Generic Inspector RPC, raw selectors, and Motion wrappers |
| Build/link/install | Separate protocol, control, canonical client, runtime, telemetry, authoring, and high-risk eval components; ordinary targets do not gain authority merely because components are built. `ControlInstalledHost` is the explicit T1 composition seam for the authenticated carrier, observability bundle, Motion, exact-target UI, and additional typed host executors, all installed before ready publication. A clean-prefix consumer compiles and runs the installed protocol/control/client targets while rejecting direct GPU/render/format/host/CLI/MCP closure | Per-target shipped-product declarations and cross-platform verified-peer parity |
| Shipping | Canonical manifests, registry digest, artifact audit, stripped ordinary targets, marker checks, the owner-only macOS health-service LaunchAgent, and a Release process E2E for ready-gated exact-host dispatch and authority cleanup | Remaining product declarations and cross-platform release negative controls |

## Centralized replacement boundary

The replacement is the broker, authenticated carrier, exact registration,
typed operation registry, generated clients, grants, receipts, and bounded
artifact/telemetry systems. It is not a compatibility wrapper around the
deleted Inspector authority. New host tiers or operations must join this
composition; they may not add a second broker, discovery service, transport,
session registry, client, generic RPC, or filesystem selector.

The deleted legacy TCP server/discovery path is not a compatibility fallback.
There is one centralized authority path, and unavailable operations remain
unavailable until that path owns them end to end rather than falling back to an
Inspector selector.

Enrolled installed hosts use a two-phase open/ready handshake. The broker holds
the exact registration in a non-discoverable, non-grantable state until the
host has installed its executor and acknowledges readiness. Once published,
the host and broker exchange generation-checked heartbeats. A missed lease,
disconnect, or restart unregisters the exact publication, detaches its router,
and cancels retained opaque authority. No host opens a TCP listener, reads a
discovery file, or becomes a second broker.

## Phase 4 read-only runtime slice

`dev.pulp.instance/read@1` is settled inside `ControlService` from
`ControlBroker`'s live registration after exact grant admission and a final
authority checkpoint. It does not ask a host payload to describe its own
identity. Offline jobs and standalone instances are distinguished explicitly,
and a heartbeat advances only the liveness generation; unregister/restart
mints a new registration identity and revokes the old grants.

`dev.pulp.state/read@1` is an injected runtime executor. T0 compositions may
install it directly; T1 hosts provide it to the canonical authenticated host
connection. Its resolver receives only the admitted registration plan and an
exact runtime `StateStore` selection. The adapter runs on the control/host
worker, never the audio thread, performs no mutation or file I/O, and uses the
shared parameter JSON serializer for catalog and values. Requests are bounded
to 4096 unique parameter IDs. Sensitive parameters are omitted unless the
request explicitly opts in, and the response reports the redacted count.
The resolver snapshots `StateStore::state_generation()` and the executor
rechecks that same store before and after serialization; it has no adapter-side
generation counter.

These adapters do not create a listener, discovery path, CLI command, MCP
tool, capture/eval/reload surface, or legacy Inspector fallback. A
`production-stripped` manifest still cannot contain an endpoint or either
capability; developer/test/support artifacts remain explicit opt-ins whose
manifests are only an upper bound, not a grant.

## High-risk and typed-operation boundaries

`runtime.eval` is arbitrary execution in the UI process. Its retained component
is separately linked and must remain bounded, single-flight, interruptible, and
denied for effectful scripted realms. It is never an implementation path for
MIDI, transport, parameter gestures, authoring controls, or capture.

Typed test input, state changes, telemetry, capture, and Motion components can
still be exercised by focused in-process tests. Public clients must not reach
them through a resurrected raw server or custom fixture wire. Their future
availability depends on the canonical host adapter and its explicit effective
grant.
