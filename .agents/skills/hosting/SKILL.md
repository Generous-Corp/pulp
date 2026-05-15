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

`plugin_slot_clap.cpp` is the one real backend today. Patterns to mirror:

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

### Defensive boundary for entry / factory calls (#812)

`scanner_clap.cpp` wraps `entry->init()` and `entry->get_factory()`
in `try/catch` (commit `70e3545d`). Throws across the dlopen boundary
abort the whole scan otherwise — observed in production with bundles
whose static-init throws C++ exceptions during `dlopen`. The fallback
emits a synthesized `PluginInfo` (filename-derived name, no metadata)
so the scan still surfaces the bundle. Static-init throws that fire
*before* `dlsym` returns can't be caught at this layer; that's the
case `pulp scan --no-load` exists for. When adding new entry-point
calls, wrap them too — the goal is "one bad bundle never crashes a
scan."

### dlerror() must be cached (ASan-only SEGV — pulp #1862 / #1873)

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
already cache via a local — the only offender was the defensive #812
fallback path at `scanner_clap.cpp:90`. When adding a new format
backend that calls `dlopen`, mirror the cache-into-local pattern.

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

- `connect()` returns `false` on cycle — always check. `would_create_cycle`
  lets you preview without mutating.
- `processing_order()` is recomputed each call; cache it in the audio
  thread, don't recompute per block.
- Removing a node invalidates its `NodeId`. Connections referencing a
  removed node are pruned automatically.

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

## Phase 0 contracts (PR #153)

The host exposes an immutable audio-thread snapshot now, not direct
member reads. Anything you write that touches the audio thread (a
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
- **Parameter domain.** `HostParamInfo::min_value` / `max_value` /
  `default_value` are the **plain** parameter domain. VST3-internal
  normalization is hidden behind the loader.
- **Parameter flags.** Consumers must honor
  `HostParamInfo::flags.{automatable, read_only, stepped, is_bypass}`
  before writing. Automation routing (Phase 1E) refuses
  non-automatable edges.
- **ParameterEventQueue.** `PluginSlot::process()` takes a
  `const ParameterEventQueue&`. Phase 1 loaders consume it;
  Phase 0 loaders accept and ignore. Use it — not `set_parameter` —
  for per-block automation.
- **Thread rules doc.** `docs/reference/host-thread-rules.md` is the
  canonical reference.

## Phase 1 per-format depth (PR #156-ish)

Each format loader gained real parameter / state / automation handling
on top of Phase 0 contracts:

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

## Phase 1 P1 follow-ups (PR #159)

Four bugs caught in Codex review of the Phase 0/1 series:

- `connect_automation` rejects cycles via `would_create_cycle` (automation
  edges contribute to topo order so back-edges are invalid).
- `Vst3Slot` dtor only calls `terminate()` once on combined
  IComponent + IEditController objects (FUnknown-pointer equality check).
- `SignalGraph::process()` returns immediately on `num_samples <= 0`
  rather than memset'ing with a wrapped size_t.
- MidiInput nodes' `midi_out` is drained at the END of `process()`, not
  the start. Hosts call `inject_midi()` before each `process()` to refill.

## PluginManagerPanel (issue #494)

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
`[issue-494]` tag on those cases is the canary for regression.

## Phase 3 — `.pulpgraph` save/load

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

## Scanner identity rules (issue #491 P2)

`PluginScanner` produces `PluginInfo::unique_id` values that
`graph_serializer.cpp` keys against on rehydration. Two plugins with
the same *display name* used to collide silently — the identity
contract now is:

- **VST3**: the first audio-effect class's CID from
  `Contents/Resources/moduleinfo.json`, normalized to a 32-char
  lowercase hex string. Read via `scanner_vst3.cpp` — **no dlopen at
  scan time**. This is deliberate: opening random VST3 bundles during
  a bulk scan used to crash on Visage/JUCE-based plugins with
  duplicate ObjC classes and on plugins whose `bundleEntry()` requires
  a real `CFBundleRef`. moduleinfo.json is Steinberg's declarative
  discovery format (VST 3.7+) and lets us read identity without
  running any plugin code.
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
