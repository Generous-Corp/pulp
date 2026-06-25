---
name: hosting
description: |
  Load, run, and test VST3 / AU / CLAP / LV2 plugins from Pulp code. Use
  when working on `core/host/` (scanner, plugin_slot, signal_graph), when
  adding a new format backend, when wiring a plugin into a SignalGraph, or
  when writing an integration test that needs a real plug-in binary.
---

# hosting

## When this skill applies

- Adding or modifying a format backend under `core/host/src/plugin_slot_<format>.cpp`.
- Extending `PluginSlot::load()` to handle a new format in `core/host/src/plugin_slot.cpp`.
- Building or routing nodes in `SignalGraph`.
- Writing tests that need to load a real plug-in binary.

## Mental model

`PluginSlot` is the uniform interface. Each format backend is a single
free function — `load_<format>_plugin(info)` — that returns a
`std::unique_ptr<PluginSlot>` or `nullptr`. `PluginSlot::load()` in
`plugin_slot.cpp` is a small compile-time dispatcher. There is no dynamic
registry and no plug-in-per-file hooks; adding a format means:

1. Write `core/host/src/plugin_slot_<fmt>.cpp` that defines
   `std::unique_ptr<PluginSlot> load_<fmt>_plugin(const PluginInfo&)`.
2. Add the file to `core/host/CMakeLists.txt` under a `if(PULP_HAS_<FMT>)`
   guard. Link the format's SDK. Define `PULP_HOST_HAS_<FMT>=1`.
3. Forward-declare the loader and add a `case PluginFormat::<FMT>:` to
   the dispatcher in `plugin_slot.cpp`, guarded by the same macro.

Everything else — tests, scanner, graph wiring — is format-agnostic.

## CLAP reference backend

`plugin_slot_clap.cpp` is the simplest backend to study for dlopen,
factory lifetime, parameter metadata, automation, MIDI, and state patterns.
VST3 / AU / LV2 also have real loaders, so treat CLAP as a reference for
its ABI shape rather than as the only implemented backend. Patterns to mirror:

- `dlopen(RTLD_LAZY | RTLD_LOCAL)`; on macOS resolve
  `<bundle>.clap/Contents/MacOS/<name>` before `dlopen`.
- `dlsym("clap_entry")`; call `entry->init(path)` exactly once before
  `entry->get_factory(...)`, and `entry->deinit()` + `dlclose()` in the
  slot's destructor.
- Pick factory descriptor by `info.unique_id` when set, else first
  available. Then fill the returned `PluginInfo` with any missing
  name / vendor / version / id fields from the descriptor.
- The slot must own the `clap_host_t` it exposes to the plug-in; the
  plug-in stores the pointer and will deref it later.
- After a successful `CLAP_EXT_STATE` restore, clear any cached host
  parameter edits in the slot. Otherwise `get_parameter()` can report a
  stale host-side value even though the plug-in restored its own state.

### Defensive boundary for entry / factory calls

`scanner_clap.cpp` wraps `entry->init()` and `entry->get_factory()` in
`try/catch`. Throws across the dlopen boundary abort the whole scan
otherwise — observed in production with bundles whose static-init throws
C++ exceptions during `dlopen`. The fallback
emits a synthesized `PluginInfo` (filename-derived name, no metadata)
so the scan still surfaces the bundle. Static-init throws that fire
*before* `dlsym` returns can't be caught at this layer; that's the
case `pulp scan --no-load` exists for. When adding new entry-point
calls, wrap them too — the goal is "one bad bundle never crashes a
scan."

### dlerror() must be cached

**Never** call `dlerror()` more than once per failure log line. POSIX
clears dlerror's internal buffer after every call, so a ternary like:

```cpp
// WRONG — second dlerror() returns nullptr
runtime::log_warn("dlopen failed: {}",
                  dlerror() ? dlerror() : "unknown");
```

calls dlerror() twice and the SECOND call returns `nullptr`.
`std::format`'s `string_view(char const*)` ctor then runs
`strlen(nullptr)` and ASan flags a SEGV in `libsystem_platform`'s
`_platform_strlen`. **The bug is invisible on non-ASan builds** —
release builds happen to survive the near-null strlen because the
zero page is read-protected one access deep. The fixed behavior caches
`dlerror()` before formatting so the null-returning second call cannot
reach `std::format`.

Correct idiom:

```cpp
const char* err = dlerror();
runtime::log_warn("dlopen failed: {}", err ? err : "unknown");
```

The other format backends (`plugin_slot_vst3.cpp`, `plugin_slot_lv2.cpp`,
`plugin_slot_clap.cpp`, `core/runtime/src/dynamic_library.cpp`) all
already cache via a local. When adding a new format backend that calls
`dlopen`, mirror the cache-into-local pattern.

## Testing against a real plug-in

Integration tests gate on a compile-time path macro:

```cmake
if(PULP_BUILD_TESTS AND NOT ANDROID AND TARGET PulpGain_CLAP)
    foreach(_pulp_clap_host_test IN ITEMS pulp-test-host pulp-test-host-regression)
        if(TARGET ${_pulp_clap_host_test})
            target_compile_definitions(${_pulp_clap_host_test} PRIVATE
                PULP_TEST_CLAP_PATH="${CMAKE_BINARY_DIR}/CLAP/PulpGain.clap")
            add_dependencies(${_pulp_clap_host_test} PulpGain_CLAP)
        endif()
    endforeach()
endif()
```

Keep this wiring after `add_subdirectory(examples)`: the top-level build
registers `test/` before `examples/`, so `test/CMakeLists.txt` cannot
reliably see `PulpGain_CLAP` at configure time.

Tests check `fs::exists(PULP_TEST_CLAP_PATH)` and `WARN` + return if the
plug-in isn't built, so the suite still passes on configurations that
skip the plug-in builds (Android, CI without GPU examples, etc.).

Pattern for a process test: load, `prepare(48000, 256)`, fill an input
buffer with non-zero samples, call `process`, assert the output buffer
has non-zero energy. A gain plug-in is the cheapest target — one param,
predictable output, no MIDI.

## Signal graph gotchas

- `SignalGraph` dispatches plugin nodes through the additive
  `PluginSlot::process(format::ProcessBuffers&, ...)` overload. The default
  implementation projects the active main input/output bus back to the legacy
  `process(output, input, ...)` callback, and still calls legacy processing with
  empty audio views for MIDI-only slots. Override the `ProcessBuffers` overload
  when a hosted format or fixture needs direct bus metadata for sidechains,
  auxes, surround, or multi-output products.
- **Canonical-executor routing (opt-in, default OFF).** An eligible graph —
  nodes only AudioInput / AudioOutput / Gain / Plugin (every Plugin node must
  carry a LIVE slot), connections only plain audio (feedforward or one-block
  feedback; no MIDI / automation / audio-rate-mod / sidechain) — can be driven
  through the canonical `GraphRuntimeExecutor` instead of the legacy walk via
  `set_canonical_executor_routing_enabled(true)`. Output is bit-identical to the
  legacy walk (`signal_graph_executor_routing.{hpp,cpp}` translates the graph;
  `test_signal_graph_executor_parity` is the guard). Plugin output slots are
  pinned *persistent* in the buffer assignment (the `persistent_output` spec
  flag), so a plugin that does not fully overwrite its output carries the same
  stale tail across blocks that SignalGraph's per-node buffer does — the reason a
  Plugin node needs a live slot to be eligible (a null-slot placeholder would
  take the legacy pass-through-or-zero branch, which the executor does not
  reproduce). A latency-reporting plugin IS eligible: the routed gather applies
  the same per-connection plug-in delay compensation as the legacy walk
  (per-node latency is propagated through the topology in the buffer assignment,
  and each feedforward connection that needs it gets a delay ring sized in the
  `GraphRuntimeBufferPool`), so fan-in paths of differing latency time-align
  identically. MIDI / automation / sidechain graphs stay on the legacy walk.
- `connect()` returns `false` on cycle — always check. `would_create_cycle`
  lets you preview without mutating.
- `processing_order()` is recomputed each call; cache it in the audio
  thread, don't recompute per block.
- Removing a node invalidates its `NodeId`. Connections referencing a
  removed node are pruned automatically.
- Per-node CPU load: `process()` wraps each node's work in a persistent
  per-node `audio::AudioProcessLoadMeasurer` (keyed by `NodeId` in
  `node_load_`), read via `node_loads()`. The measurers live on the
  SignalGraph (not the snapshot) and `compile_()` only ever ADDS to the map —
  never erase while a snapshot may be live, or the audio thread's raw
  `NodeRuntime::load` pointer dangles. `begin()/end()` are relaxed-atomic and
  RT-safe (proven under the no-alloc trap in test_signal_graph_rt_safety).
- `.pulpgraph` schema changes must go through the graph serializer migration
  path. Bump the graph format version, add a deterministic migrator for older
  fixtures, and keep future-version loads fail-closed instead of silently
  accepting fields the current reader does not understand.
- Use `connect_automation()` for sparse two-point-per-block control events.
  Use `connect_audio_rate_modulation()` only for continuous, automatable
  `HostParamInfo::rate == AudioRate` params; do not route dense CV into
  stepped/read-only/control-rate parameters.
- MIDI graph edges carry one block with three parallel payloads: short MIDI
  events, SysEx, and optional UMP sidecars. When copying or clearing graph MIDI
  scratch, handle all three together. If a `MidiBuffer` attaches a `UmpBuffer`
  owned by `NodeRuntime`, attach it only after the runtime object is in its
  final `CompiledGraph` storage; attaching before a move leaves a stale sidecar
  pointer.
- `SignalGraph::inject_midi()` and `extract_midi()` cross the
  control/audio-thread boundary through per-node mailboxes, not by mutating
  audio-thread scratch directly. Keep mailbox snapshots and writer scratch
  preallocated by `prepare()`; constructing a fresh MIDI snapshot in
  `inject_midi()` reintroduces realtime-path allocation.
- Keep plugin automation scratch preallocated by `SignalGraph::prepare()`.
  The audio-thread `process()` path must not create per-block containers for
  input pointer casts, sparse automation accumulation, or dense audio-rate
  modulation accumulation.
- Custom graph nodes are registered per `SignalGraph` with `CustomNodeType`
  (`type_id`, `version`, port counts, default name, optional process
  callback), then instantiated with `add_custom_node(type_id)` or
  `add_custom_node(type_id, version)`. `GraphSerializer` resolves exact
  `(type_id, version)` matches with the saved port shape, preserves unresolved
  custom identities as placeholder `NodeType::Custom` nodes, and reports them in
  `LoadResult::missing_custom_node_types`, so do not coerce unknown node
  strings to a built-in type. Runtime callbacks are attached only when the
  registered version and shape match the node.
- **Stateful custom nodes.** `CustomNodeType` has an
  optional lifecycle: set `create` and the graph owns one opaque instance per
  node (RAII via `destroy`); `process_instance` runs instead of the stateless
  `process`, and `prepare`/`release`/`reset`/`save_state`/`load_state` operate on
  it. Empty callbacks = today's stateless node (no instance, no serialized
  state). The instance is created/prepared on the UI thread inside
  `SignalGraph::prepare()` (mirroring `PluginSlot`) and captured into each
  `CompiledGraph` snapshot by `shared_ptr` — never allocate or create instances
  on the audio thread, and never store a raw `GraphNode` pointer in the snapshot.
  `process_instance` must be RT-safe; call `save_state`/`load_state` only on the
  control path (graph not live, or after invalidate + re-prepare). Opaque state
  is `std::vector<uint8_t>` via `SignalGraph::custom_node_state` /
  `set_custom_node_state`; `GraphSerializer` persists it as `state_b64` and keeps
  the blob even for **unresolved** nodes (save → load-missing-type → save keeps
  state). Do not pull the `pulp_native_state_*` C ABI into `CustomNodeType`;
  that belongs to the `pulp_node_v1` ABI.
- **Signed node-pack loader (`core/host/node_pack.{hpp,cpp}`).**
  `load_node_pack(dir, manifest, trust)` loads a precompiled `pulp_node_v1` node
  pack (a `.dylib`/`.so`/`.dll` exporting `pulp_node_v1_entry` + a JSON manifest).
  It verifies trust BEFORE any `dlopen`: the signer key must be in the
  `NodePackTrust` set, the Ed25519 signature over `node_pack_signed_message()`
  (pack_id + abi_major + binary SHA-256 + declared nodes/resources/requirements)
  must be authentic, the on-disk binary's SHA-256 must match the signed hash,
  and the entry's `abi_major` must match — any failure returns a
  `NodePackError` and loads nothing. Revocation = drop a key from the trust set.
  Desktop + Android only; `pulp-host` (and this loader) is compiled out on iOS,
  where native components are static-bundled + signed with the app. The crypto
  comes from `pulp::runtime` (`ed25519_verify`, `sha256_hex`); OS
  codesign/notarization is a separate, additional distribution step on top of
  the manifest signature. Registry/package discovery metadata still needs its
  own signed canonical manifest; do not treat screenshots, validation reports,
  licenses, or provenance as covered by the node-pack loader signature.
- **Routing a `SignalGraph` through the canonical executor
  (`core/host/signal_graph_executor_routing.{hpp,cpp}`).** A prepared graph in
  the eligible subset — nodes only `AudioInput`/`AudioOutput`/`Gain`, connections
  only plain audio (feedforward or feedback; no MIDI/automation/audio-rate-mod/
  sidechain) — can be translated by `build_signal_graph_executor_routing()` into a
  `format::GraphRuntimeSnapshot` + pre-sized `GraphRuntimeBufferPool` and driven
  via `GraphRuntimeExecutor::process_routed()`, producing output bit-identical to
  `SignalGraph::process()`'s own walk. Gotchas: gate with
  `signal_graph_executor_eligible()` first (Plugin/Custom/MIDI/PDC/sidechain/
  automation stay on the legacy walk); the routing holds the live compiled
  snapshot alive via `SignalGraph::live_snapshot_handle()` and the Gain bindings
  read the live `live_gain_atomic()`, so **rebuild the routing after any
  re-prepare** (the atomics belong to the snapshot it was built from). Eligibility
  depends on the node-type restriction guaranteeing zero latency — only Plugin
  nodes add latency, so the executor (which has no per-connection delay
  compensation yet) stays correct for the subset.
  `SignalGraph::set_canonical_executor_routing_enabled(true)` requests that the
  live `process()` callback use this path for eligible graphs (default OFF →
  legacy walk; ineligible graphs always fall back). The routing snapshot AND its
  scratch pool are built in
  `compile_()` and embedded per-snapshot in the published `CompiledGraph` (NOT a
  shared `SignalGraph` member), so a re-prepare builds them on a fresh snapshot
  and never resizes a buffer an in-flight audio reader holds — the same
  RCU/retire discipline the legacy per-node scratch relies on. The long-lived
  `GraphRuntimeExecutor` is a `SignalGraph` member (prepare never mutates it; the
  single audio thread is its only writer).

## Common tripwires

- Building `pulp-host` without adding a new `.cpp` to `target_sources` —
  the file sits on disk but isn't compiled; link errors fire only in the
  dispatcher's `case`. **Always** update `core/host/CMakeLists.txt`
  alongside adding a backend.
- Missing `PULP_HOST_HAS_<FMT>` define — dispatcher silently returns
  `nullptr`. Verify `grep PULP_HOST_HAS_ build/CMakeCache.txt` after
  configure.
- CLAP bundles on macOS: don't `dlopen` the `.clap` directory; resolve to
  the executable inside `Contents/MacOS/` first.
- LV2 manifest URI extraction must only use subject-position `<URI>` tokens.
  A manifest stanza like `<plugin> rdfs:seeAlso <plugin.ttl> ; a lv2:Plugin`
  should identify `<plugin>`, not the `seeAlso` object. Keep parser coverage
  in `test/test_plugin_info_metadata.cpp` or `test/test_lv2_host_discovery.cpp`
  when changing `core/host/src/scanner.cpp`.
- LV2 invalid-bundle tests deliberately use placeholder `.so` / `.dylib` files.
  Keep the loader's magic-byte preflight before `dlopen` / `LoadLibrary` so
  invalid modules fail quickly and consistently on Windows instead of waiting
  on the platform loader.
- A slot's per-block scratch must be reserved in `prepare()`, not grown in
  `process()`. The CLAP slot fills `in_ptrs_`/`out_ptrs_` and emplaces into
  `in_event_storage_` each block; on a default-constructed vector the first
  `resize`/`emplace_back` allocates on the audio thread. Reserve the channel
  vectors from `PluginInfo::num_inputs/num_outputs` (the graph sizes node
  buffers from these, floored at stereo) and the event scratch for
  `params_.size() + ParameterEventQueue::kCapacity +` the realtime MIDI cap.
  Guard with a `PULP_DBG_ASSERT(capacity >= needed)` tripwire (debug-only).
  This holds for the graph-driven path; a direct caller passing more channels
  or an un-capacity-limited `MidiBuffer` is outside the contract. No-alloc
  coverage lives in `test_host.cpp` ("ClapSlot::process is allocation-free
  after prepare() reserves"), gated on `PULP_TEST_CLAP_PATH`.
- The VST3 slot has the same channel-vector issue *plus* extra per-block
  allocation inside the Steinberg helper containers it builds each block
  (`Vst::ParameterChanges` / `EventList` from `public.sdk/.../hosting`), so
  reserving `in_ptrs_`/`out_ptrs_` alone does NOT make `Vst3Slot::process`
  allocation-free — a `PULP_TEST_VST3_PATH` no-alloc test against
  `PulpGain.vst3` still trips. Making the VST3 slot RT-safe needs those SDK
  containers pre-sized too; tracked as a follow-up, not yet done.
- Fixture wiring (`PULP_TEST_CLAP_PATH`, future `PULP_TEST_VST3_PATH`) lives in
  the ROOT `CMakeLists.txt` block *after* `add_subdirectory(examples)` — NOT in
  `test/CMakeLists.txt`, which is registered before `examples/` so it cannot
  see the `PulpGain_*` targets at configure time. A guard placed in `test/`
  silently never runs (its define just appears stale in an incremental build).

## Audio-thread snapshot contracts

The host exposes an immutable audio-thread snapshot, not direct member
reads. Anything you write that touches the audio thread (a
graph editor, an MCP bridge, a preset loader) must account for these
rules:

- **Mutation protocol.** Every UI-thread `SignalGraph` mutator
  (`add_*`, `connect*`, `disconnect`, `remove_node`, `clear`)
  invalidates the live snapshot. `process()` returns silence until
  the next `prepare()` call republishes. Batch edits: mutate, then
  `prepare()`, not the other way around.
- **Plugin ownership.** `GraphNode::plugin` is a `std::shared_ptr<PluginSlot>`.
  The published snapshot copies the shared_ptr, so a plugin survives
  past the removal of its GraphNode until the audio thread's stale
  snapshot reference drops. Do not stash raw plugin pointers.
- **Release ordering.** `SignalGraph::release()` must unpublish the live
  snapshot and wait for in-flight snapshot readers before calling
  `PluginSlot::release()` or custom-node release callbacks. Do not move
  release callbacks ahead of snapshot retirement.
- **Live control scalars.** If a control-path setter updates state inside the
  already-published `CompiledGraph`, the audio-thread field must be RT-safe.
  `set_node_gain()` stores into a per-runtime `std::atomic<float>`; do not
  reintroduce plain mutable snapshot fields for values read by `process()`.
- **Parameter domain.** `HostParamInfo::min_value` / `max_value` /
  `default_value` are the **plain** parameter domain. VST3-internal
  normalization is hidden behind the loader.
- **Parameter flags.** Consumers must honor
  `HostParamInfo::flags.{automatable, read_only, stepped, is_bypass}`
  before writing. Automation routing refuses non-automatable edges.
- **ParameterEventQueue.** `PluginSlot::process()` takes a
  `const ParameterEventQueue&`. The queue type now lives in
  `pulp::state` and `pulp::host` re-exports it for compatibility, so
  format and graph code can share the event ABI without depending on
  `core/host`. Current loaders consume it for per-block automation where
  the format supports sample offsets. Use it — not `set_parameter` — for
  per-block automation.
- **Node ABI surface.** `PluginSlot` includes
  `pulp/runtime/node_abi.hpp` and participates in the node ABI
  virtual-order gate. Existing virtual methods may not be inserted,
  removed, or reordered; add new virtual methods only after the current
  tail and let `tools/scripts/node_abi_gate.py --mode=report` verify
  the diff against the PR base.
- **Thread rules doc.** `docs/reference/host-thread-rules.md` is the
  canonical reference.

## Per-format depth

Each format loader has parameter / state / automation handling on top of the
audio-thread snapshot contracts:

- **CLAP**: real `clap_input_events_t` (param_value + midi events
  sorted by time), `clap_output_events_t` harvests MIDI to
  `midi_out`, `CLAP_EXT_STATE` save/load via vector-backed
  `clap_ostream` / `clap_istream`.
- **VST3**: `IEditController` queryInterface (combined or separate
  with controller initialize), full parameter enumeration with
  ParameterInfo flags mapped onto HostParamInfo, plain-domain
  get/set via `normalizedParamToPlain` / `plainParamToNormalized`,
  state save/load via a `VectorStream` IBStream implementation.
- **AU**: `AudioUnitScheduleParameters` per block from
  ParameterEventQueue — sample-accurate AUv2 automation.
- **LV2**: control-port discovery extended into the regex TTL parser
  (lv2:ControlPort + name/default/min/max), per-port float scratch in
  `control_values_`, `connect_port` wired at process() block start,
  param_events apply last-write-wins.
- LV2 bundle discovery has a private test seam in
  `core/host/src/lv2_discovery.hpp`; keep TTL port/binary parsing tests in
  `test/test_lv2_host_discovery.cpp` rather than reaching through real
  plug-in binaries for deterministic coverage.

Param domain: **plain values** at the PluginSlot boundary (not
normalized). Loaders convert internally if they natively normalize
(VST3). Don't normalize host-side.

`connect_automation(src, port, dest, param, lo, hi, ...)` delivers
two control points per block (sample 0 + N-1) via the queue. Loaders
that interpolate sample-accurately (CLAP, VST3, AU via
ScheduleParameters) get smooth automation; LV2 control ports are
sample-at-block-start so the offset-(N-1) value wins.

MixMode::Replace is the default; second Replace edge to the same
(node, param) is rejected. MixMode::Add sums then clamps.

## Review-found host graph invariants

Keep these host graph invariants covered by tests:

- `connect_automation` rejects cycles via `would_create_cycle` (automation
  edges contribute to topo order so back-edges are invalid).
- `Vst3Slot` dtor only calls `terminate()` once on combined
  IComponent + IEditController objects (FUnknown-pointer equality check).
- `SignalGraph::process()` returns immediately on `num_samples <= 0`
  rather than memset'ing with a wrapped size_t.
- MidiInput nodes' `midi_out` is drained at the END of `process()`, not
  the start. Hosts call `inject_midi()` before each `process()` to refill.

## PluginManagerPanel

`pulp::view::PluginManagerPanel` sits on top of the scanner backend and
gives host apps a ready-made "manage plugins" UI. The widget is
header-only (`core/view/include/pulp/view/plugin_manager_panel.hpp`)
and drives everything through `PluginManagerModel`:

- Tests use `InMemoryPluginManagerModel` — pre-populate `scanned_rows`,
  `failed_rows`, and `paths_by_format`, then assert on `visible_count`,
  `rows`, and context-menu activations. The model exposes
  `rescan_count`, `single_rescan_count`, `last_reveal_path` counters
  for verifying the widget wired through.
- Real hosts subclass `PluginManagerModel` and back `start_rescan()`
  with either `PluginScanner::scan()` on a worker thread or the
  out-of-process `pulp-scan-worker` binary. `examples/plugin-host-demo
  --manage` shows the threaded-scanner pattern end-to-end.
- Blacklist persistence goes through `pulp::host::ScanBlacklist::save_to
  /load_from`; the widget itself is stateless beyond the filter string.
  `set_blacklisted(path, true)` must save to disk so the row stays
  blacklisted across sessions.
- The widget does not render a native popup for right-click; it exposes
  `context_menu_path()`, `context_menu_items()`, `context_menu_label()`,
  and `activate_context_item()` so hosts can wire their own popup
  (or tests can drive menu activation directly).

When adding new context-menu items or bucket semantics, remember to
extend `test_plugin_manager_panel.cpp` in the same commit — the
`[issue-494]` Catch2 tag on those cases is the canary for regression.

## `.pulpgraph` save/load

`pulp::host::GraphSerializer::to_json(graph, layout)` /
`from_json(graph, json)` round-trips topology + per-node plugin state
+ editor layout. Plugin entries store identity (format, unique_id,
manufacturer, name, version, last_path) plus a base64 state blob from
`PluginSlot::save_state()`. **Plugin binaries are never embedded.**

Two-pass deserialize: instantiate every node (mapping old → new
NodeId), then walk connections and replay `connect / connect_midi /
connect_feedback / connect_automation`. Plugin re-resolution is
scanner-identity-first; missing plugins surface in
`LoadResult::missing_plugins` and the corresponding nodes are still
created with null slots so connection ids stay stable. `GraphNode`
gained a `plugin_info` member that survives a failed slot load so
re-saving an unresolved-plugin node preserves its identity.

## Crash-isolated scanning

`pulp::host::IsolatedPluginScanner` (in
`core/host/include/pulp/host/isolated_scanner.hpp`) is the high-level
wrapper around the long-standing `pulp-scan-worker` binary. Construct
it with a path to the worker, then call `scan(bundle_path, timeout_ms)`
to scan a single bundle in a child process via
`pulp::platform::ChildProcess::run()`. A crash, hang, or malformed
descriptor is reported as a `ScanResult { status, descriptor,
exit_code, error_message }` instead of taking down the host.

`ScanStatus` classifications:

| Status          | Trigger                                          | Caller action                |
|-----------------|--------------------------------------------------|------------------------------|
| `Ok`            | exit 0 + parseable JSON descriptor on stdout     | use `result.descriptor`      |
| `Crash`         | exit ≠ 0,2,3 OR exit 0 with unparseable stdout   | `ScanBlacklist::blacklist()` |
| `Timeout`       | worker exceeded `timeout_ms`                      | blacklist as soft crash      |
| `FormatError`   | worker exit 3 (unsupported bundle extension)     | skip (not a plugin format)   |
| `NotPlugin`     | worker exit 0 with empty stdout                  | skip                          |
| `WorkerMissing` | configured `worker_path` doesn't exist           | operational error — surface  |

Gotchas:

- The worker's exit-code surface is frozen: 0 = success, 2 = usage
  error, 3 = unsupported extension. Anything else is treated as a
  crash. If you grow the worker's exit semantics, update the parent's
  classifier in `core/host/src/isolated_scanner.cpp` AND the test
  matrix in `test/test_isolated_scanner.cpp` in the same commit.
- The descriptor parser is a flat string-search, not a full JSON
  parser — it relies on the worker's `write_json_descriptor()` schema
  being stable. If you add nested objects to the worker output, swap
  to `choc::json` here instead of extending the string search.
- `ChildProcess::exec_code` is `-1` for any signal-kill on POSIX (the
  Crash branch covers this) and an OS exception code on Windows
  (also Crash). Don't try to disambiguate further; the only signal
  the parent has is "did the worker exit 0 with valid JSON or not".
- Tests use a small `fixtures/isolated_scanner_crash_helper.cpp`
  binary that segfaults / hangs / emits garbage on demand. Pattern is
  reusable for any future ChildProcess-based isolation work — give
  the helper a mode argv and exec it from the parent.

## PluginManagerPanel drag-add

Users can drag a row out of `PluginManagerPanel` and drop it onto a
graph editor surface to add a plugin node. The panel itself stays
surface-agnostic — it emits `on_row_drag_start` / `on_row_drag_end`
callbacks with the row payload, and hosts wire the drop into whichever
graph the cursor landed on.

Panel contract:

- `PluginManagerRow` gained the identity fields needed to round-trip
  into a `PluginInfo` (`manufacturer`, `version`, `unique_id`,
  `num_inputs`, `num_outputs`, `is_instrument`, `is_effect`) plus a
  `to_plugin_info()` helper. All defaulted so older models keep
  working.
- The panel tracks press → drag-threshold → drag start, emits drag
  callbacks only for `scanned` bucket rows (failed and blacklisted
  rows are suppressed because they cannot load anyway), and exposes
  `simulate_row_drag()` so tests and hosts can drive the callback
  without synthesising motion events.

Host-side: `pulp::host::add_plugin_node_from_drop(graph, info,
&loaded)` attempts a live `PluginSlot::load` via `add_plugin_node`
and falls back to `add_unresolved_plugin_node` when the bundle can't
load — preserving user intent across save/reload. Don't bypass this
helper and call `add_plugin_node` directly; the unresolved-fallback
path is what keeps `.pulpgraph` round-trips honest when a plugin
binary disappears between sessions.

## ExtensionsVisitor — typed access to format-specific extensions

Hosts that need to reach a format-specific extension (CLAP note ports,
VST3 IMidiMapping, AU AudioUnit property, LV2 instance) subclass
`ExtensionsVisitor`, override the `visit_*` methods they care about,
and call `slot.accept(visitor)`. Default `visit_*` fall through to
`visit_unknown` so a visitor that only cares about one format ignores
the rest automatically. The base `PluginSlot` dispatches to
`visit_unknown` for placeholder / unresolved slots so they degrade
gracefully.

Format-specific `*Extension` structs deliberately expose handles as
`void*` so they don't pull SDK headers into client code. Callers that
*do* link the SDK `static_cast` back to the concrete type. Wired in
`ClapSlot`, `Vst3Slot`, `AuSlot`, `Lv2Slot`. Tests in
`pulp-test-extensions-visitor` pin the visit dispatch + the
`ExtensionFormat` enumerator values (the latter is a reorder-detector).

## Scanner identity rules

`PluginScanner` produces `PluginInfo::unique_id` values that
`graph_serializer.cpp` keys against on rehydration. The identity
contract is:

- **VST3**: the first audio-effect class's CID from
  `Contents/Resources/moduleinfo.json`, normalized to a 32-char
  lowercase hex string. Read via `scanner_vst3.cpp` — **no dlopen at
  scan time**. This is deliberate: opening random VST3 bundles during
  a bulk scan used to crash on plugins with duplicate ObjC classes and on
  plugins whose `bundleEntry()` requires a real `CFBundleRef`. moduleinfo.json
  is Steinberg's declarative discovery format (VST 3.7+) and lets us read
  identity without running any plugin code.
- **LV2**: the plugin URI from `manifest.ttl` (the same URI
  `plugin_slot_lv2.cpp` uses at load time to pick a descriptor).
  Parsed via a tiny regex — we deliberately don't pull in
  lilv/serd/sord for a single-field read.
- **CLAP**: `desc->id` from `clap_plugin_descriptor_t`, extracted by
  briefly loading the bundle in `scanner_clap.cpp`. CLAP bundles are
  the only format where scan-time dlopen is safe — the CLAP ABI is
  designed for cheap metadata reads and the bundles don't ship ObjC.
- **AU**: scanned by `AudioComponent` API, not file-system walk. The
  AU component's type/subtype/manu four-char codes serve as identity.

Bundles that don't expose their identity through the safe path (e.g.
VST3 without moduleinfo.json) fall back to the directory stem. The
graph_serializer rehydration handles stem IDs the same way it always
did — best-effort — so the scanner stays safe across a user's entire
plugin folder.

**Placeholder plugin node**: when graph_serializer can't resolve a
saved plugin at load time, it creates a Plugin node with a null
PluginSlot so topology survives. `SignalGraph::process()` treats
null-slot nodes as deterministic input→output pass-through (or
zero-fill on channel-count mismatch). Don't assume a Plugin node
always has a live slot — always null-check `plugins[id]` before
dereferencing.
