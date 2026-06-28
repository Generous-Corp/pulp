# Node ABI

Pulp exposes two SDK-facing C++ node surfaces:

- `pulp::format::Processor`, implemented by plug-ins
- `pulp::host::PluginSlot`, implemented by host-side format loaders

Both surfaces include `pulp/runtime/node_abi.hpp`, which defines
`PULP_NODE_ABI_VERSION` and `pulp_node_abi_version()`. Version `1` is the
current node ABI generation.

## Compatibility Contract

Pulp's established `Processor` and `PluginSlot` node surfaces are
source-compatibility contracts. Plug-ins and host nodes are expected to be
recompiled with the Pulp SDK they ship against. The version constant lets code,
tests, and generated artifacts state the node interface generation they were
built for.

This is not a claim of stable C++ binary ABI across arbitrary compilers,
standard libraries, compiler flags, or struct layouts. A truly stable binary
node ABI would need a dedicated C shim. Pulp does not expose that shim today.

Pulp's current custom-node decision is:

> Pulp supports source-compatible custom nodes (`CustomNodeType`) and also ships
> the language-neutral `pulp_node_v1` C ABI (experimental) for nodes written
> against a stable binary contract, plus runtime loading of *binary* node packs
> from disk (Ed25519 signing / trust / packaging) via `core/host/node_pack.hpp`
> on desktop and Android. See "`pulp_node_v1` C ABI (shipped, experimental)" and
> "Signed node packs" below.

Here, "compatible with a future C ABI boundary" is a design constraint, not a
binary compatibility promise. New node-facing APIs should be shaped so they can
later be represented as POD data, opaque handles, explicit sizes, status-code
errors, and host-owned lifetimes. They should not expose STL types, exceptions,
templates, RTTI, virtual inheritance, or ambiguous ownership as if those were a
stable binary contract.

For now, custom node development is explicitly experimental and source-rebuild
oriented:

- rebuilds with the target SDK are expected;
- source/API breakage is allowed between releases;
- binary compatibility is not guaranteed;
- the API exists to validate ergonomics and pressure-test the future
  `pulp_node_v1` direction.

## Compatibility Terms

| Term | Meaning in Pulp today |
|------|-----------------------|
| Source compatibility | Code rebuilds with the SDK version it targets. This is the current model for established SDK node surfaces; experimental custom-node APIs may still break between releases. |
| ABI-shaped API | An internal or experimental surface follows C-boundary discipline so it can be tested and adapted later. It may still change. |
| Stable binary compatibility | A precompiled third-party node binary loads across supported Pulp builds without recompilation. Pulp does not promise this today. |
| Frozen ABI | A stable binary contract that cannot remove or reinterpret existing fields, callbacks, symbols, or behavior within the supported compatibility window. Nothing in `pulp_node_v1.draft.0` is frozen. |

## `pulp_node_v1` C ABI (shipped, experimental)

`pulp_node_v1` is the language-neutral C ABI for custom `SignalGraph` nodes. It
ships as a real header —
`core/native-components/include/pulp/native_components/pulp_node_v1.h` (module
`pulp::native-components`) — derived from the source-level `CustomNodeType`
lifecycle/state contract. Its `PULP_NODE_V1_ABI_MAJOR` tracks
`PULP_NODE_ABI_VERSION` (the cross-module equality is asserted in
`test/test_pulp_node_v1.cpp`).

Scope is **custom `SignalGraph` nodes only** — not the Processor-level FFI
(`native_core.h`), not a format-adapter replacement. The contract:

- POD only: leading `uint32_t size` + `uint32_t abi_major`; opaque
  `pulp_node_instance_v1` / host handles; status codes; capability flags; no STL,
  templates, exceptions, RTTI, virtuals, `std::function`, or references.
- One exported symbol, `pulp_node_v1_entry()`, returning a `pulp_node_entry_v1*`
  vtable (descriptor / create / prepare / reset / process / release / save_state /
  load_state / report_latency). The host checks `abi_major` + `size` and refuses
  on mismatch.
- Host owns audio buffers (borrowed planar float32 for the call); state crosses
  via a host writer (save) + a byte span (load), validate-before-commit; a
  host-services struct carries alloc/free/log/now_ns, each labelled RT-callable
  or NON-RT-only.
- **Same-major compatibility:** within major 1 the contract is append-only —
  fields grow via `size`, behaviour via capability bits; a host accepts any node
  with a matching major and at least its minimum `size`, ignoring trailing
  fields. A different major is rejected.

It is **experimental**: the contract may still gain capability bits / trailing
fields (additively) before it is declared frozen. A C node and a Rust node are
proven to load through the identical contract (`test_pulp_node_v1.cpp` +
`test_pulp_node_v1_rust.cpp`). Runtime loading of *binary* node packs from disk
(signing / trust / per-platform packaging) now ships as a separate layer — see
"Signed node packs" below.

The ABI test suite pins the public struct contract with compile-time POD /
trivial-copy assertions, leading `size` + `abi_major` offset checks for every
boundary struct, same-major old/new entry negotiation, and rejection of entries
that truncate the first required callback. The source-level compatibility path is
covered separately by graph serializer tests: registered custom nodes resolve by
exact `(type_id, version)`, newer registered versions do not replace saved older
nodes, mismatched shapes remain unresolved placeholders, and unresolved nodes
survive save-load-save-load cycles with identity, ports, connections, and opaque
state intact.

## Signed node packs

A precompiled `pulp_node_v1` node can be distributed as a **signed node pack**: a
dynamic library (`.dylib` / `.so` / `.dll`) exporting `pulp_node_v1_entry`, plus a
JSON manifest declaring the pack identity, ABI major, the binary's SHA-256,
declared node type-ids/capabilities, resource declarations, runtime
requirements, and an **Ed25519 signature**. The host loader
`core/host/node_pack.hpp` (`load_node_pack(dir, manifest, trust, policy)`)
verifies trust and host policy *before* it `dlopen`s anything:

1. the signer key must be in the host's trust set (drop a key to revoke it);
2. the signature over `pack_id + abi_major + binary-hash + declared node
   type-ids/capabilities + resources + runtime requirements` must be authentic;
3. declared capabilities, realtime requirements, audio-thread allocation policy,
   block size, and memory ceilings must fit the host's `NodePackHostPolicy`;
4. required resource declarations must have stable IDs, kinds, and hashes;
5. the on-disk binary's SHA-256 must match the signed hash;
6. the entry's `abi_major` must match the host's `pulp_node_v1` major;
7. the loaded descriptor's stable ID and capability flags must match one of the
   signed node declarations.

Any failure rejects the pack and loads no code. This is the host-level integrity
gate; OS code-signing / notarization (Gatekeeper, Authenticode) is an additional,
separate distribution step. **Desktop and Android only** — `core/host` is compiled
out on iOS / AUv3 / sandboxed targets, where native components are static-bundled
and signed with the app. Still experimental (the manifest/loader may evolve
additively); see [`native-components.md`](native-components.md) for the
platform-policy rationale.

## API Layers

| Layer | Audience | Compatibility promise | Examples |
|-------|----------|-----------------------|----------|
| Internal/private APIs | Pulp runtime implementation | May change at any time. Not an extension contract. | `CompiledGraph`, graph scratch buffers, serializer internals. |
| Experimental public APIs | SDK users willing to rebuild and track changes | Source-oriented, explicitly experimental, breakage allowed between releases. | `CustomNodeType`, custom node registration, graph serialization of custom `type_id` + `version`. |
| Experimental binary ABI boundary | Precompiled third-party node binaries | Shipped but experimental: same-major append-only within `pulp_node_v1`; not yet a *frozen* cross-release promise. | `pulp_node_v1` header + `pulp_node_v1_entry`; signed node packs via `core/host/node_pack.hpp`. |

Experimental public APIs should remain small. The current minimum useful
surface is:

1. register a custom node type with stable `type_id`, integer `version`, port
   counts, display name, and process callback;
2. instantiate a node by `type_id` and optional exact `version`;
3. preserve unresolved custom node identity across graph serialization;
4. process audio only when the registered type and saved node shape match;
5. treat parameter, MIDI, state, reset, latency, and dynamic loading as future
   extensions unless separately designed and tested.

This gives developers something real to build against today while preserving a
clear line between experimental source APIs and a future frozen binary ABI.

## Draft Future Shape

**Draft id:** `pulp_node_v1.draft.0`

**Status:** historical, **superseded**. This sketch predates the shipped
`pulp_node_v1.h` header and `core/host/node_pack.hpp` loader; it is retained as a
design record of the review target. For the real, current contract see
"`pulp_node_v1` C ABI (shipped, experimental)" and "Signed node packs" above. The
shipped header is still not a *frozen* stable ABI.

**Scope:** custom `SignalGraph` nodes only. Full `Processor` binaries are out of
scope because they overlap with VST3, AU, CLAP, and Pulp's format adapters.

The smallest useful future node lifecycle is:

1. `descriptor`
2. `create`
3. `prepare`
4. `process`
5. `set_param` / event delivery
6. `save_state`
7. `load_state`
8. `release`

The shape is intentionally close to CLAP-style host/plugin separation, LV2-style
extension layering, LLVM-style opaque handles, and OBS-style module lifecycle,
while preserving Pulp's current source-rebuild posture.

An illustrative C boundary would look roughly like this:

```c
/* Draft-only sketch. Not a Pulp header. Not a stable ABI. */

#define PULP_NODE_V1_ABI_MAJOR 1u
#define PULP_NODE_V1_ABI_MINOR_DRAFT 0u

typedef struct pulp_node_host_v1 pulp_node_host_v1;
typedef struct pulp_node_instance_v1 pulp_node_instance_v1;

typedef enum pulp_node_status_v1 {
    PULP_NODE_OK = 0,
    PULP_NODE_UNSUPPORTED = 1,
    PULP_NODE_INVALID_ARGUMENT = 2,
    PULP_NODE_OUT_OF_MEMORY = 3,
    PULP_NODE_INTERNAL_ERROR = 4
} pulp_node_status_v1;

typedef struct pulp_node_audio_bus_v1 {
    unsigned size;
    unsigned channel_count;
    float** channels;
} pulp_node_audio_bus_v1;

typedef struct pulp_node_process_v1 {
    unsigned size;
    unsigned frame_count;
    const pulp_node_audio_bus_v1* audio_inputs;
    unsigned audio_input_count;
    pulp_node_audio_bus_v1* audio_outputs;
    unsigned audio_output_count;
    const void* events;
    unsigned event_count;
} pulp_node_process_v1;

typedef struct pulp_node_descriptor_v1 {
    unsigned size;
    unsigned abi_major;
    unsigned abi_minor;
    const char* stable_id;
    const char* display_name;
    unsigned version;
    unsigned capability_flags;
    unsigned audio_input_count;
    unsigned audio_output_count;
} pulp_node_descriptor_v1;

typedef struct pulp_node_entry_v1 {
    unsigned size;
    const pulp_node_descriptor_v1* (*descriptor)(void);
    pulp_node_status_v1 (*create)(const pulp_node_host_v1* host,
                                  pulp_node_instance_v1** out);
    pulp_node_status_v1 (*prepare)(pulp_node_instance_v1* node,
                                   double sample_rate,
                                   unsigned max_block_size);
    pulp_node_status_v1 (*process)(pulp_node_instance_v1* node,
                                   const pulp_node_process_v1* process);
    pulp_node_status_v1 (*save_state)(pulp_node_instance_v1* node,
                                      void* writer);
    pulp_node_status_v1 (*load_state)(pulp_node_instance_v1* node,
                                      const void* reader);
    void (*release)(pulp_node_instance_v1* node);
} pulp_node_entry_v1;
```

That sketch exists to make future review concrete. The actual ABI must be
designed in a separate PR and may choose different names or split optional
surfaces into extensions.

## Boundary Rules for Future Drafts

Any experimental surface that might become binary-facing should follow these
rules:

- Host-owned audio buffers; the node borrows them only for the current call.
- Opaque handles for host and node objects.
- POD structs with `size` and version fields.
- Explicit counts and byte sizes for arrays, strings, and state blobs.
- Status-code errors, not exceptions.
- No STL, templates, references, lambdas, exceptions, RTTI, or C++ ownership
  across the boundary.
- No allocation, locks, blocking I/O, logging allocation, or host callbacks
  that allocate from `process`.
- Capability flags and extension queries for optional features instead of
  widening required structs for every feature.
- Same-major compatibility is the first realistic promise if this ever ships.

## Architecture Assessment

| Area | Helps a future C ABI | Complicates or blocks a future C ABI |
|------|----------------------|---------------------------------------|
| `SignalGraph` lifecycle | Graph editing and `prepare()` are UI-thread operations; `process()` runs on an immutable snapshot. | `CustomNodeType` has no independent create/prepare/release lifecycle yet. |
| Custom node identity | `type_id` plus `version` are serialized, and unresolved custom nodes survive reload. | The current callback is metadata plus process only; parameter, MIDI, state, reset, and latency surfaces are not modeled. |
| Processor/plugin boundary | `PluginSlot::process()` already separates audio-thread processing from UI-thread load/state/editor work. | `Processor` and `PluginSlot` are C++ virtual interfaces and must not be treated as binary-stable. |
| Buffers/audio I/O | `BufferView` is a clear borrowed-buffer model. | `BufferView` is a C++ template type; a C ABI needs explicit pointer/count/channel structs. |
| Params/events | `ParameterEventQueue` gives per-block event transport and sample offsets. | Custom nodes do not yet have a C-shaped param/event stream or capability query. |
| MIDI/events | Graph has MIDI nodes and block-scoped MIDI routing. | MIDI uses C++/library-owned representations today; a C ABI needs a POD event stream and ordering rules. |
| State serialization | Graph serializer stores versions and preserves unresolved custom node identity. | Custom nodes have no save/load state callbacks yet. |
| RT safety | Host thread rules clearly forbid allocation, locks, and blocking in audio callbacks. | The custom node callback type cannot enforce RT safety by itself; tests/lints would be needed for ABI-shaped experiments. |
| Versioning | `PULP_NODE_ABI_VERSION`, graph format versions, node capabilities, and custom node versions already exist. | These are source/serialization markers, not a binary negotiation protocol. |
| Dynamic loading | Signed node packs ship via `core/host/node_pack.hpp` (Ed25519 trust verified before `dlopen`) on desktop + Android. | iOS / AUv3 / sandboxed targets compile `core/host` out entirely — static-bundled, app-signed native code only. OS notarization/Gatekeeper/Authenticode remain a separate distribution step. |

## Recommendation Tiers

| Tier | Recommendation |
|------|----------------|
| Safe to implement now | Keep public docs explicit that Pulp is source-compatible, experimental, and rebuild-oriented. Keep custom node IDs and versions stable. Prefer additive capability bits and POD-shaped internal DTOs where useful. Add tests for serialization, unresolved reload, and no audio-thread allocation around new graph/runtime work. |
| Experimental only | Grow the source-level custom node API in small steps: lifecycle hooks, param/event descriptors, state callbacks, and latency reporting. Prototype ABI-shaped structs behind internal or test-only headers. Add static assertions for POD/trivial layout. Build adapters from C-shaped structs into current C++ types without shipping a loader. |
| Shipped (experimental) | Dynamic loading, manifest format, the exported `pulp_node_v1_entry` symbol, Ed25519 signing/trust policy, accept/reject loader tests, and the same-major append-only contract now ship via `pulp_node_v1` + `core/host/node_pack.hpp`. |
| Must defer until ABI freeze | A *frozen* cross-release binary guarantee and a formal extension registry. Until then, packs are rebuilt against the SDK major they target. |
| Dangerous to expose publicly | Current `CustomNodeType`, `CustomNodeProcessFn`, `Processor`, `PluginSlot`, `audio::BufferView`, `MidiBuffer`, STL containers, exceptions, virtual tables, or `std::function` as a stable binary ABI. |

## Draft Changelog

| Draft | Date | Notes |
|-------|------|-------|
| `pulp_node_v1.draft.0` | 2026-05-21 | Initial non-binding future shape. Records experimental source-compatible SDK rebuilds as the current stance, defers stable binary ABI, and defines the descriptor/create/prepare/process/events/state/release review target. |

Future edits to this section must update the draft id and changelog. Before any
draft becomes a public ABI, it must have cross-platform loader tests, RT-safety
tests, version-negotiation tests, and a migration story for older drafts.

## Design References

- [CLAP](https://cleveraudio.org/) for a modern audio C ABI, extension
  negotiation, event ordering, and host/plugin separation.
- [LV2](https://lv2plug.in/) for a small core plus extension model.
- [Cmajor](https://cmajor.dev/) and [Faust](https://faust.grame.fr/) for
  graph-oriented and generated DSP module thinking.
- [LLVM C API](https://llvm.org/doxygen/group__LLVMC.html) for an opaque C
  facade over a larger C++ implementation.
- [WebAssembly Component Model](https://component-model.bytecodealliance.org/)
  for explicit interface contracts and canonical ABI design.
- [OBS Plugin API](https://docs.obsproject.com/plugins) and
  [Rust `libloading`](https://docs.rs/libloading/latest/libloading/) for
  practical dynamic module loading lessons.

## Virtual Method Policy

Within a node ABI generation, virtual methods on `Processor` and `PluginSlot`
are append-only:

- do not reorder existing virtual methods
- do not remove existing virtual methods
- do not change an existing virtual method signature
- add new virtual methods only after the current virtual-method tail

`tools/scripts/node_abi_gate.py` enforces this in CI by comparing the current
virtual declarations against the PR base. A middle insert, removal, reorder, or
signature change fails; appending at the tail passes.

## Optional Capabilities

New optional node behavior should prefer additive capability bits over new
virtual methods. `PluginDescriptor::node_capabilities` carries the forward
compatible capability field, while legacy `supports_mpe` and `supports_ump`
remain valid for source compatibility.

Use `PluginDescriptor::effective_capabilities()` when adapter or host code
needs the effective value. It ORs the legacy flags with the node capability
field so both declaration styles behave the same.

## Custom Host Graph Nodes

`SignalGraph` supports string-keyed custom host nodes through
`CustomNodeType`. Register a type on the graph with a stable `type_id`, a
positive integer `version`, its input/output port shape, and an optional
process callback, then instantiate it with `add_custom_node(type_id)` or
`add_custom_node(type_id, version)`. A callback is attached only when the
registered `(type_id, version)` and port shape match the node; mismatched or
unresolved nodes use placeholder passthrough behavior.

The node type enum only appends `NodeType::Custom`; built-in enum values stay
stable. Serialized graphs store the custom `type_id` and `version`, and loads
preserve that identity even when the target graph has not registered a matching
factory. Multiple versions of the same custom `type_id` can be registered at
once; deserialization resolves by exact `(type_id, version)`.

### Stateful custom nodes

`CustomNodeType` is additively extended with an optional **stateful lifecycle**.
When `create` is set, the graph owns one opaque instance per node (RAII via
`destroy`); `process_instance` runs instead of the stateless `process`, and
`prepare` / `release` / `reset` / `save_state` / `load_state` operate on that
instance. All callbacks default-empty, so a process-only node behaves exactly as
before (no instance, no serialized state).

The instance is created and prepared on the UI/main thread inside
`SignalGraph::prepare()` — mirroring `PluginSlot` — and captured into each
immutable `CompiledGraph` snapshot by `shared_ptr`, so the audio thread never
allocates and an in-flight snapshot keeps its instance alive. `process_instance`
runs on the audio thread and must be real-time-safe; `save_state` / `load_state`
are non-RT control-path calls (use them while the graph is not live, or after an
invalidate + re-prepare), the same expectation as plugin state.
Generated or scripted graph flows must not expose `save_state`, `load_state`,
graph import/serialization, registration, or `prepare`/`release` as audio-thread
operations. Those APIs are explicitly denied from RT paths; changing custom
state invalidates the live graph and requires a later successful `prepare()`
before processing resumes.

Opaque state is a `std::vector<uint8_t>` (mirroring `PluginSlot::save_state`),
reachable via `SignalGraph::custom_node_state(NodeId)` /
`set_custom_node_state(NodeId, bytes)`. `GraphSerializer` persists it as
`state_b64` inside the node's `custom` object, and **preserves the blob even for
unresolved nodes** so a save → load (missing type) → save cycle keeps the state.
This is the source-level C++ API; the binary `pulp_node_v1` C ABI (above) was
derived from this lifecycle/state experience and now ships experimentally — not
yet frozen.
