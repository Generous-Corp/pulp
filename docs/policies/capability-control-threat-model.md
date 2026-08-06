# Capability control threat model

This document defines the security boundary for Pulp's local capability-control
platform. It applies to Product A control sessions and the shared primitives a
future Product B collaboration router may reuse. It does not claim that an
arbitrary native audio plugin is sandboxed merely because its Pulp control
surface is capability-controlled.

## Security goals

- Loading, discovering, or inspecting a plugin grants no authority by itself.
- A build manifest is an upper bound, not a runtime grant.
- Every operation is bound to an exact artifact publication, process generation,
  plugin instance, client, live session, capability, and typed operation.
- Consent is keyed by a broker-derived identity over the verified canonical
  manifest digest and the SHA-256 of the exact artifact bytes; a mutable build
  label or publisher assertion is never sufficient.
- One Pulp-owned per-user broker owns discovery, grants, revocation, consent,
  receipts, artifact access, quotas, and audit records.
- Plugins and clients receive neither broker credentials for other principals
  nor raw peer addresses, sockets, or generic message escape hatches.
- Production-stripped artifacts contain no Product A endpoint or legacy Remote
  View parameter authority.
- Denial is explainable without logging secret values, parameter payloads,
  captured content, or collaboration message bodies.

An operation is allowed only when all seven terms are true:

```text
implemented ∩ built ∩ host_available ∩ activated
∩ policy_eligible ∩ client_granted ∩ session_live
```

Every term defaults false. Unknown fields, capabilities, operations, schema
versions, executors, publishers, publications, and instances fail closed.

## Protected assets

- plugin state, parameters, presets, authoring state, and host automation;
- audio/MIDI fixtures, render output, screenshots, logs, traces, and telemetry;
- user consent decisions, publisher policy, grants, receipts, and audit records;
- per-session and bootstrap credentials;
- exact instance, slot, process-generation, and publication identity;
- host stability, audio-thread deadlines, storage, CPU, memory, and connection
  capacity.

## Trust boundaries

The broker and its owner-private persistent state are trusted. The CLI and MCP
adapter are separate clients; neither may self-issue grants. A Pulp-owned host
bridge is trusted only for the exact host process and slots it attests. Plugin
code, plugin UI code, imported content, remote renderers, MCP request text,
project files, presets, collaboration payloads, and third-party host processes
are untrusted inputs.

The v1 broker is local-only. Loopback is defense in depth, not identity. OS peer
credentials, owner-private bootstrap material, mutual authentication, exact
publication identity, and fresh nonces still apply. A discovery record proves
only that something is discoverable; registration and policy must independently
establish what it is and what it may do.

Shared-process plugins are not meaningfully identified by process signing
alone. A trusted host bridge must attest the verified artifact publisher and
exact loaded slot. If a host tier cannot supply that evidence, capability
control and collaboration are unavailable there.

## Implemented foundation and current boundary

The optional control foundation now composes broker identity, exact T0/T1
registration, and client-scoped grants behind one dormant `ControlBroker`
owner without opening a listener or routing an operation. Verified peer identity binds UID/SID, PID, process generation,
executable identity, publisher, and role; same-UID membership and payload claims
are not proof. Launcher bootstrap material is single-use, short-lived,
peer-bound, and wiped. Registrations validate a canonical manifest plus the
exact artifact digest and expose only that manifest's bounded capability set.
Interactive trusted-consent decisions reject replay, while an existing user
policy may be deliberately reusable. All stores have explicit capacity and
expiry limits, and their security audit contains metadata and reason codes only.

The first broker-carrier slice extends the existing length-prefixed IPC stack
with an OS-local stream; it does not introduce a second framing protocol. Its
filesystem endpoint is confined to an absolute owner-owned private directory,
rejects group/other access and extended ACLs, never replaces an existing path,
uses mode `0600`, and is unlinked by the listener owner. On macOS the accepted
socket yields kernel UID/GID/PID plus the audit-token PID generation. The peer
verifier then validates the live dynamic code signature and binds its signing
identifier, CDHash, and Team ID (or a per-artifact ad-hoc CDHash fallback) to
that carrier evidence. It rechecks process start and ownership across signature
inspection. TCP, POSIX FIFO, client payload identity, dead/zombie processes,
missing PID generation, signature failure, and any expectation mismatch fail
closed. Other platforms do not yet mint a verified control peer.

This remains a dormant composition root, not a running broker. The installed
per-user service activation, trusted consent UI, operation dispatcher, receipts, and
artifact ACLs are still required. Only Pulp-owned T0 offline jobs and T1
standalone hosts are admissible at this stage. Shared-host slots and direct
AUv3 access fail closed as `host-unavailable`; plugin-rendered consent and
environment-delivered bootstrap credentials remain rejected.

## Threats and required controls

| Threat | Required control |
|---|---|
| Local attacker reads discovery or replays credentials | Owner-private files, no inherited ACLs, mutual challenge/proof, fresh nonces, bounded expiry, credential wipe, peer identity checks, replay rejection |
| Confused deputy asks a trusted client to control another instance | Exact session/instance/publication selection; client-scoped grants; capability and operation binding; no newest-instance fallback |
| PID reuse, restart, stale project intent, or stale grant | Kernel-origin process generation where available, process-start recheck across code-signature inspection, opaque publication IDs, heartbeat/expiry, disconnect revocation; restore creates fresh grants only after revalidation |
| Malicious plugin claims another publisher, slot, service, or capability | Signed artifact/declaration verification plus trusted host slot attestation; manifest is only an upper bound; broker rejects self-asserted identity |
| Compromised or over-broad client | Least-privilege client grants, controller leases, expiry, explicit revocation, typed schemas, idempotency keys, bounded receipts, artifact ACLs |
| Plugin or client bypasses policy with raw transport | No generic message or peer socket SDK; one broker transport; generated typed bindings; legacy Remote View mutation removed; raw host/port authority deleted by the broker migration |
| Artifact or private-data exfiltration | Grant-checked artifact handles, provenance, retention/quota limits, redaction classes, no ambient filesystem paths, no payload values in policy audit logs |
| Runtime evaluation becomes a mutation shortcut | Separate high-risk component and capability, `research-unsafe` profile, exact acknowledgement, dedicated evaluator, realm and size/time limits; never an implementation path for typed operations |
| Denial of service against broker, host, or audio thread | Bounded clients, frames, queues, rates, subscriptions, jobs, artifacts, timeouts, cancellation and expiry; no JSON/network work on the audio thread; host-main execution is bounded and fenced |
| Grant revoked while work is queued or executing | Admission and pre-apply revalidation, cancellable staged operations, truthful `mayHaveApplied`/receipt state, no automatic retry of ambiguous mutation |
| Schema downgrade or scope smuggling | Namespaced versioned IDs, canonical serialization and digest, unknown-field rejection, no permissive downgrade, explicit manifest changes for new fields/directions/rates |
| Removed build authority survives reconfiguration | Per-target profile, capability, and unsafe-evaluation declarations force-refresh on every configure; a two-configure regression proves critical authority is withdrawn without deleting the build tree |
| Artifact changes between discovery and verification | One cached byte snapshot per candidate drives selection, surface detection, marker verification, hashing, and consent identity; sidecar-derived names must remain safe basenames beside the sidecar; artifact and sidecar symlinks are rejected rather than followed |
| Empty identity or oversized typed payload bypasses policy accounting | Required identity and idempotency strings are nonempty; every schema string and collection is bounded; discriminated operations use closed request variants; executor-specific limits are frozen in the registry, including byte-based UTF-8 limits that JSON Schema character counts cannot express |
| Update installs a second or untrusted broker | One active Pulp-owned per-user service, one signed update/bootstrap path, version negotiation, old service drain and credential invalidation |
| Cross-vendor route appears from compatibility alone | Compatibility only makes a route offer possible; user/publisher policy and a visible integration action admit it; paired revocable grants are issued only after both exact endpoints are revalidated |
| A blocked publisher communicates through another allowed route | Publisher deny policy wins before route creation and on restore/reconnect; active routes are inspectable and revoked when policy changes |

## Consent and policy defaults

Product behavior should be permissive enough for useful integrations without
making installation consent. Cross-vendor support may be enabled globally, but
no actual route exists until a visible integration action. Publisher policy has
three simple states: `use-default`, `always-ask`, and `block-integrations`.
Blocks always win. A user may allow same-publisher integrations, allow most
cross-vendor integrations while blocking one publisher, inspect active routes,
and revoke them at any time.

Routine messages, reconnects, and compatible patch versions do not prompt.
Publisher changes, service/schema incompatibility, new data fields or direction,
higher rate/budget, or any other material scope expansion require a new decision.
Project files store inert route intent, never credentials or durable grants.

## Explicit non-claims

- Product A does not prevent arbitrary native plugin code from accessing files,
  the network, or other process resources allowed by its host and OS sandbox.
- OSC UDP is a separate opt-in network control product and is reported, not
  silently claimed as protected by Product A.
- Loopback does not make an unauthenticated endpoint safe.
- Code signing alone does not identify one plugin slot in a shared DAW process.
- An MCP tool list, a build type, an environment variable, a compatible schema,
  or a discovered peer is not authorization.
- Product B is not approved merely because the shared identity and policy
  primitives exist; its design and pilot gates remain separate.

## Verification gates

Changes to this boundary require tests for unknown fields and versions,
permission-term denial, identity forgery and reuse, replay, grant expiry and
revocation, cancellation races, artifact ACLs, queue/rate limits, broker
restart/update, incremental reconfiguration, path traversal, immutable artifact
snapshot use, schema boundary values, and negative binary scans. The dedicated
platform-sandbox review is accepted with binding T0/T1-only restrictions. It
rejects direct AUv3 access, self-attested shared-host slots, plugin-rendered
consent, and environment bootstrap credentials. Any later host tier must close
its own reachability, trusted-consent ownership, legal completion,
bootstrap-delivery, and missing-attestation gates before gaining authority.

Security reviews and host feasibility decisions are durable planning records.
User-facing artifact checks use `pulp inspect audit ARTIFACT`; that command is
read-only and never activates the target.
