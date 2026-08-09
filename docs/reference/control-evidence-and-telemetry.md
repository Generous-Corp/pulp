# Control evidence and telemetry

The capability-control evidence path is broker-owned and disabled unless a
control-enabled host has passed build, activation, policy, grant, and session
admission. It does not revive the retired Inspector server or create a network
listener.

## Evidence contracts

Evidence-producing executors return an opaque artifact ID and metadata. They do
not return temporary paths. The broker binds every publication to the exact
operation receipt, client, registration, session, instance, grant, manifest,
and producer digest. Reads recheck that lineage and grant; knowing an artifact
ID is not authorization.

| Evidence | Content type | Storage rule |
|---|---|---|
| Screenshot | `image/png` | sensitive or restricted; redact metadata before publication |
| Offline render | `audio/wav` | never public; original audio is allowed when the grant permits it |
| State snapshot | `application/vnd.pulp.state-snapshot+json` | sensitive or restricted; redact fields before publication |
| Perfetto trace | `application/vnd.pulp.perfetto-trace` | sensitive or restricted; redact metadata before publication |

The store verifies SHA-256 on every read, bounds blob and chunk sizes, and
enforces aggregate, publication-count, and per-client quotas. Expired metadata
is deleted before its bytes can be read. A background collection pass removes
orphan blobs and interrupted private-publish files. Deletion audit records are
bounded and contain only opaque artifact ID, hash, size, time, and reason; they
never persist plugin text, consent text, paths, or tokens.

## T1 and T2a mutation

`dev.pulp.state/parameter-gesture@1` resolves one exact admitted registration,
checks the expected state generation, and runs begin, normalized write, and end
on the host main thread. Completion is reported only after the host advances
its state generation. Host automation winning the generation race returns
`state_conflict` without starting a gesture.

The generation belongs to `StateStore`, not the host adapter. Restore, base
UI/audio writes, modulation, trigger reset, state reads, and canonical control
all observe that same monotonic authority. The gesture claims its expected
generation atomically; callback failure or a writer arriving inside the bracket
uses a versioned value compare and compare-only rollback so newer parameter
state is never overwritten. Reads also reject while any store writer is active,
so a reserved generation cannot certify a pre-write value.

For a Pulp-hosted T2a slot, the host router binds the registration to both the
slot instance ID and its process/slot generation. Unload detaches the route;
recreating a similarly named slot cannot inherit an old admission or grant.

## Bounded telemetry

`ControlTelemetryTap` is a transport-free broker component and is disabled by
default. An explicitly enabled developer/test host transfers the one exclusive
`ValueChannelTelemetryAttachment` to it. The tap reads each sidecar once and
fans out copied frames into bounded per-subscription queues; it never adds a
reader to a value channel's existing triple buffer.

Requests are capped by client, total subscription count, channel count, sample
rate, vector width, and queued frames. Sampling is downsampled to policy. A slow
subscriber loses the oldest copied frame and receives a loss counter; it never
blocks a producer, render thread, or audio callback. Subscription reads require
the same client, registration, instance, and grant authority used at creation.
Sensitive channel names and values are redacted unless that authority permits
them.

`ControlHostObservabilityBundle` composes that tap with the canonical trace
session executor at the registration ownership boundary. The adapter installs
the bundle executor before publishing its registration. Dispatch rechecks the
broker-minted client, grant, registration, session, instance, publication,
generation, operation, version, and deadline binding; none of those values can
be replaced by request parameters. An opaque host-connection authentication
token refreshes a monotonic bounded lease. A missed heartbeat, explicit
disconnect, or process restart drops trace ownership and detaches the tap,
which destroys all subscriptions.

The telemetry operation uses one versioned action union. `subscribe` returns a
stream ID, `poll` returns either no frame or one bounded typed frame with loss
accounting and redaction fields, and `unsubscribe` closes that exact stream.
Every poll and unsubscribe must present the same admitted client,
registration, instance, and grant that created it. The remote host carrier
keeps client and grant identities broker-private. The broker projects one
random connection-local authority ID plus the exact broker, session,
registration, instance, publication, manifest, artifact, operation, and
generation binding. Explicit revocation, grant expiry, disconnect, or restart
sends an authority-end event and destroys retained trace and telemetry
ownership. The opaque ID is not accepted on the client carrier and cannot mint
authority.
