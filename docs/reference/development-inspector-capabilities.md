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
| `capture.image` | yes | yes | Whole-window compositor capture is available when the selected standalone host supports it; node capture remains unavailable |
| `trace.control` | no | yes | Domain components exist but the standalone owner does not advertise them without a trace binding |
| `trace.session.control` | no | yes | Process-global Trace sessions require a publication-scoped binding |
| `state.write` | no | yes | The `develop` standalone profile applies legal parameter mutations on the main thread after acquiring the same-connection controller lease |
| `test.input` | no | yes | Reserved capability; typed MIDI/transport methods are not implemented |
| `authoring.tweaks` | no | yes | The standalone session exposes its tweak store; filesystem and editor-launch methods remain classified unavailable |
| `telemetry.stream` | no | yes | Registered events are policy-filtered before authenticated fan-out, but the standalone owner does not yet attach independent live telemetry sources |
| `runtime.eval` | no | no | High-risk separate opt-in; no standalone profile enables it |
| `unavailable` | no | no | Filesystem-backed tweak/fixture operations and editor launch are classified unavailable for the future policy |

`off` grants nothing. `custom` starts from an empty exact allow-list. These are
enforced policy definitions. `develop` deliberately excludes `runtime.eval`.

## Checked implementation matrix

| Area | Present | Missing |
|---|---|---|
| Constructor/reachability | Explicit `pulp run --inspect[=PROFILE]` activation constructs one authenticated owner for a compatible GPU desktop standalone window; ordinary and plugin-format launches remain endpoint-free | Additional host-format ownership |
| Window host | Built-in macOS standalone hosts keep their owning-thread dispatcher alive after native-loop stop until accepted inspector work retires, and schedule startup-failure close on a later native event turn | Windows/Linux external factories must implement `event_loop_supports_exit_drain()` with `run_event_loop_until()`, plus `supports_deferred_close()` with `request_close_deferred()`, to opt into active profiles |
| Build/link/install | Optional protocol, reader discovery, neutral discovery-path support, publisher/runtime, client, and authoring targets are component-gated and separate from the GPU overlay. Publisher/runtime link closure does not grant reader authority; an installed consumer checks that split, and an ordinary `pulp::format` fixture proves no inspector symbols are present | Per-target shipped-product declaration and final product-manifest proof |
| Threading | The standalone owner uses bounded owning-thread RPC, responds after timely application, cancels queued work during teardown, and fences started timeouts as `mayHaveApplied` while discarding late responses | Processor-level editor replacement remains fail-closed |
| Discovery/security | owner-private ephemeral record/token files, exclusive session/instance publication, non-reusable publication generations, exact publication selection, mutual nonce/HMAC transcript proofs, replay rejection, auth/I/O timeouts, teardown, and one-controller lease | None for the explicitly activated standalone path |
| CLI | `pulp inspect profiles/list/capabilities/doctor` provide schema-versioned human/JSON orientation; every live operation accepts an exact session/instance/publication selector; the shared client authenticates and owns bounded request/controller-lease lifetimes | Higher-level task-specific commands remain phase-owned |
| MCP | Installed in-process shared client exposes profiles/list/capabilities/doctor plus exact typed operations; success carries publication identity and failures carry structured code/message/data | Higher-level task-specific tools remain phase-owned |
| Capture/telemetry | Whole-window compositor capture and the value-channel catalog are attached to the standalone session | Node capture and independent bounded live telemetry fan-out |
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

## Client evidence loop

A client first runs `pulp inspect list --json` (or
`pulp_inspect_list`) and pins the returned session, instance, and publication
IDs. It authenticates `capabilities` with those exact IDs, reads the typed state,
performs only a capability-authorized typed mutation, rereads, and optionally
captures the selected window. The publication ID is non-reusable; a missing or
changed publication requires rediscovery. `Runtime.evaluate` is never a
parameter or test-input mutation path.
