# Development inspector capabilities

The Development Inspector capability-control platform is under construction.
Phase 3 deleted the legacy server, raw client, discovery, and standalone
authority before the canonical replacement became a general live product
surface. That sequencing creates an intentional temporary capability reduction:
`pulp inspect` currently exposes static profiles and offline artifact audit
only. Source components, schemas, or a target declaration do not imply live
reachability.

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
| `dev.pulp.state/read@1` (`state.read`) | yes | yes | T0/T1 runtime executor returns bounded parameter catalog/value snapshots through the shared `StateStore` serializer, with explicit sensitive-field redaction; no CLI/MCP adapter yet |
| `dev.pulp.render/offline@1` (`render.offline`) | no | no | T0-only headless executor resolves authority-bound, launcher-trusted in-memory inputs, renders through `OfflineRenderHost`, and publishes broker-owned WAV artifacts; no profile enables it implicitly |
| `dev.pulp.ui/observe@1` (`ui.read`) | yes | yes | Typed contract/components exist; no general product host adapter |
| `dev.pulp.diagnostics/read@1` (`diagnostics.read`) | yes | yes | Typed contract/components exist; no general product host adapter |
| `dev.pulp.logs/read@1` (`logs.read`) | yes | yes | Console component exists; no public live route |
| `dev.pulp.ui/capture@1` (`capture.image`) | yes | yes | Typed contract exists; legacy screenshot routes were removed |
| `dev.pulp.ui/input@1` (`ui.input`) | no | no | Frozen high-risk contract; no current executor or grant path |
| `dev.pulp.trace/control@1` (`trace.control`) | no | yes | Component contract exists; no generic raw Inspector route |
| `dev.pulp.trace/session-control@1` (`trace.session.control`) | no | yes | `pulp trace start/stop` and matching MCP tools use canonical control only |
| `dev.pulp.state/parameter-gesture@1` (`state.write`) | no | yes | Typed executor building block exists; no public general-live route |
| `dev.pulp.test/input@1` (`test.input`) | no | yes | Typed executor building block exists; no public general-live route |
| `dev.pulp.authoring/tweaks@1` (`authoring.tweaks`) | no | yes | In-process authoring components remain; remote wrapper was removed |
| `dev.pulp.telemetry/subscribe@1` (`telemetry.stream`) | no | yes | Bounded telemetry components remain; no public general-live route |
| `dev.pulp.runtime/reload@1` (`runtime.reload`) | no | no | Frozen contract; no current executor or grant path |
| `dev.pulp.runtime/evaluate@1` (`runtime.eval`) | no | no | High-risk component remains separately gated; no public live route |
| `dev.pulp.artifact/read@1` (`artifact.read`) | no | no | Frozen publication-bound contract; broker artifact store is minimal |
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
per-user `LocalSocket` endpoint and currently exposes only a bounded health
probe: the trusted launcher and host foundations are not yet composed into that
service, so authority-bearing session admission fails closed.

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

Trace lifecycle is the first narrow public control adapter. `pulp trace start`
and `pulp trace stop`, plus `pulp_trace_start` and `pulp_trace_stop`, use the
canonical client. They accept no raw host/port or legacy publication selector
and have no legacy Inspector fallback. Offline `trace query --trace`, `doctor`,
`fetch`, and `open` do not require a live target.

## Checked surface matrix

| Area | Present now | Not yet public |
|---|---|---|
| CLI | `pulp inspect profiles`; offline `pulp inspect audit ARTIFACT`; canonical trace start/stop; offline trace analysis | Inspector discovery, live capability query, generic calls, mutation, capture, and Motion |
| MCP | In-process `pulp_inspect_profiles`; canonical `pulp_trace_start` and `pulp_trace_stop` | Inspector list/capabilities/doctor, generic inspect, evaluation, capture, mutation, and Motion wrappers |
| Build/link/install | Separate protocol, control, canonical client, runtime, telemetry, authoring, and high-risk eval components; ordinary targets do not gain authority merely because components are built. A clean-prefix consumer compiles and runs the installed protocol/control/client targets while rejecting direct GPU/render/format/host/CLI/MCP closure | Trusted product host composition, per-target shipped-product declarations, and cross-platform verified-peer parity |
| Shipping | Canonical manifests, registry digest, artifact audit, stripped ordinary targets, marker checks, and the owner-only macOS health-service LaunchAgent | Final trusted-host composition and complete release negative-control proof |

## Replacement roadmap

The temporary gap is owned work, not an invitation to retain two systems.
Phases 4–7 preserve these boundaries:

1. Finish trusted launcher/enrollment and product host adapters without adding a
   second broker, discovery service, transport, session registry, or client.
2. Route selected typed operations through the canonical broker, grants,
   executor slots, schemas, receipts, cancellation, and audit records.
3. Migrate supported installed CLI/MCP workflows to those explicit adapters;
   keep generic raw RPC, filesystem selectors, and shell-string forwarding out.
4. Close shipping composition, platform parity, artifact quota/retention, and
   negative-control evidence before describing the full live platform as
   available.

The deleted legacy TCP server/discovery path is not a compatibility fallback.
There is one centralized authority path, and unavailable operations remain
unavailable until that path owns them end to end rather than falling back to an
Inspector selector.

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
