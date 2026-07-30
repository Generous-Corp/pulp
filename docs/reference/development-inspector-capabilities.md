# Development inspector capabilities

The development inspector is an opt-in platform under construction. The
repository currently contains protocol/domain components, a visual Cmd+I
standalone overlay, and experimental CLI/MCP clients. A normal `pulp run`,
standalone, preview, or plugin-format launch does **not** construct a network
inspector session.

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
| `session.describe` | yes | yes | Authenticated session capability reporting is implemented; standalone ownership lands in Phase 2 |
| `session.control` | no | yes | One-controller lease with expiry, renewal, disconnect release, and structured conflict errors |
| `state.read` | yes | yes | Domain component exists; no normal runtime endpoint |
| `ui.read` | yes | yes | Domain component exists; no normal runtime endpoint |
| `diagnostics.read` | yes | yes | Partial domain components; availability depends on custom-host attachments |
| `logs.read` | yes | yes | Capture component exists; no normal runtime endpoint |
| `capture.image` | yes | yes | Protocol methods exist; live host capture returns unavailable |
| `trace.control` | no | yes | Domain components exist; no normal runtime endpoint |
| `state.write` | no | yes | Authenticated dispatch and response-after-main-thread-apply primitive are implemented; standalone attachment lands in Phase 2 |
| `test.input` | no | yes | Reserved capability; typed MIDI/transport methods are not implemented |
| `authoring.tweaks` | no | yes | Partial domain component; filesystem and editor-launch methods are classified unavailable and runtime dispatch enforces that classification |
| `telemetry.stream` | no | yes | Reserved capability; safe multi-consumer fan-out is not implemented |
| `runtime.eval` | no | no | High-risk separate opt-in; not enabled by any profile |
| `unavailable` | no | no | Filesystem-backed tweak/fixture operations and editor launch are classified unavailable for the future policy |

`off` grants nothing. `custom` starts from an empty exact allow-list. These are
enforced policy definitions. `develop` deliberately excludes `runtime.eval`.

## Checked implementation matrix

| Area | Present | Missing |
|---|---|---|
| Constructor/reachability | Authenticated test fixtures construct a least-privilege session; standalone still constructs only the visual overlay | Production standalone session owner and launcher activation |
| Build/link/install | CPU-only protocol, discovery, client, and runtime targets are separate from the GPU overlay | Final installed-consumer and ordinary-target strip proof |
| Threading | Bounded owning-thread RPC responds after application and cancels queued work during teardown | Production standalone attachment |
| Discovery/security | owner-private ephemeral record/token files, exact session identity, nonce/HMAC proof, replay rejection, auth/I/O timeouts, teardown, and one-controller lease | Production standalone activation |
| CLI | Shared typed client performs authenticated exact-session discovery and method requests | Profiles/list/capabilities/doctor stable JSON and installed MCP parity |
| MCP | Source-tree shell wrapper | Direct installed shared client and session-aware schemas |
| Capture/telemetry | Standalone capture seam and value-channel sources exist | Live session attachment and independent bounded telemetry fan-out |
| Shipping | Overlay option exists | Per-target declaration, manifest, binary strip/override proof |

The production server binds loopback only and requires a fresh nonce/HMAC proof
using an owner-private per-session credential. Discovery rejects expired or
dead publishers, insecure files, path escapes, and ambiguous selection.
Capability dispatch is fail-closed before a domain handler runs. The old
unauthenticated direct-handler server exists only as a non-installed test
fixture for transport regression coverage.

Build presence, host wiring, profile allowance, and current enablement are
separate facts. `Session.getCapabilities` reports the available and effective
sets for an authenticated session; no client should infer one from another.
