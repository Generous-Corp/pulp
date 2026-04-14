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

## Testing against a real plug-in

Integration tests gate on a compile-time path macro:

```cmake
if(TARGET PulpGain_CLAP)
    target_compile_definitions(pulp-test-host PRIVATE
        PULP_TEST_CLAP_PATH="${CMAKE_BINARY_DIR}/CLAP/PulpGain.clap")
    add_dependencies(pulp-test-host PulpGain_CLAP)
endif()
```

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
