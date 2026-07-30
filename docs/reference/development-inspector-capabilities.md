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
| `session.describe` | yes | yes | Registry only; authenticated session description is not wired |
| `state.read` | yes | yes | Domain component exists; no normal runtime endpoint |
| `ui.read` | yes | yes | Domain component exists; no normal runtime endpoint |
| `diagnostics.read` | yes | yes | Partial domain components; availability depends on custom-host attachments |
| `logs.read` | yes | yes | Capture component exists; no normal runtime endpoint |
| `capture.image` | yes | yes | Protocol methods exist; live host capture returns unavailable |
| `trace.control` | no | yes | Domain components exist; no normal runtime endpoint |
| `state.write` | no | yes | Component exists but lacks authenticated, response-after-main-thread-apply dispatch |
| `test.input` | no | yes | Reserved capability; typed MIDI/transport methods are not implemented |
| `authoring.tweaks` | no | yes | Partial legacy custom-host component; filesystem and editor-launch methods are classified unavailable for future policy but remain executable because current dispatch does not enforce the registry |
| `telemetry.stream` | no | yes | Reserved capability; safe multi-consumer fan-out is not implemented |
| `runtime.eval` | no | no | High-risk separate opt-in; not enabled by any profile |
| `unavailable` | no | no | Filesystem-backed tweak/fixture operations and editor launch are classified unavailable for the future policy |

`off` grants nothing. `custom` starts from an empty exact allow-list. These are
policy definitions only until the authenticated session runtime lands.
`develop` deliberately excludes `runtime.eval`.

## Checked implementation matrix

| Area | Present | Missing |
|---|---|---|
| Constructor/reachability | Test fixtures construct server/domain components; standalone constructs the visual overlay | Production session owner and normal launcher activation |
| Build/link/install | Monolithic desktop-GPU inspector archive is exported | CPU-only client/runtime split, installed public inspector headers, ordinary-target non-linkage proof |
| Threading | Existing main-thread and lifetime primitives can be reused | Bounded inspector RPC handoff with response-after-apply semantics |
| Discovery/security | TCP binds loopback | Ephemeral endpoint, exact session identity, token proof, secure records, replay defense, controller lease |
| CLI | Experimental raw method/params client | Profiles, session list/capabilities/doctor, typed shared installed client |
| MCP | Source-tree shell wrapper | Direct installed shared client and session-aware schemas |
| Capture/telemetry | Standalone capture seam and value-channel sources exist | Live session attachment and independent bounded telemetry fan-out |
| Shipping | Overlay option exists | Per-target declaration, manifest, binary strip/override proof |

The current TCP fixture is loopback-only but unauthenticated. Its newest-file
discovery hint does not validate process identity or select an exact instance.
It does not yet enforce the registry, so custom-host fixtures can still dispatch
methods classified unavailable. Do not use it for privileged mutation, file
operations, editor launch, or runtime evaluation.

Build presence, host wiring, profile allowance, and current enablement are
separate facts. A future session capability response will report all four; no
client should infer one from another.
