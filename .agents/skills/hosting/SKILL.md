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

For unattended, scriptable interrogation prefer the isolated CLI/MCP surfaces:

```bash
pulp audio plugin-inspect --plugin /path/to/plugin.component --format au
pulp audio render --plugin /path/to/plugin.component --format au \
  --input-signal noise:7 --duration-ms 1000 --warmup-ms 1000 \
  --initial-param 12=0.5 --settle-ms 250 --wav-format float32 --out /tmp/out.wav
```

`plugin-inspect` reports the complete host-visible parameter API. Both commands
instantiate vendor code in disposable child processes with timeouts. That is
crash/hang containment, not a security sandbox. The rich `pulp audio compare`
step is an optional Audio Quality Lab add-on; inspection, rendering, metrics, and
`pulp audio validate compare` are stock Pulp.

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

For a quick manual load/inspect of one installed plug-in, `examples/plugin-host-demo`
doubles as a format-neutral analyzer. `--path <bundle>` loads a CLAP / VST3 / LV2
by inferring the format from the bundle extension; an Audio Unit has no bundle
path, so select it with `--id TYPE:SUBT:MANU` (the OSType triplet printed by
`--list`). `--path` deliberately skips the full installed-plugin scan — a bulk
scan runs third-party discovery code across the machine, which is unrelated to a
trusted, user-named probe. Two extension gotchas when inferring format from a
path: shell tab-completion appends a trailing `/` to a bundle *directory*, which
empties `std::filesystem::path::extension()` (step up via `parent_path()` when
`filename()` is empty), and a plain `dlopen` of a relative bundle path triggers
the `@rpath` search dance — pass an absolute path.

## Headless Audio Unit event-loop servicing

Licensed Audio Units may complete initialization asynchronously via XPC,
timers, or dispatch-main callbacks. GUI hosts service these naturally; an
offline analyzer may otherwise produce plausible but incorrect audio or ignore
early parameter writes without reporting an error.

Use `pulp::events::MessageLoopIntegration::pump_main_loop_for()` from the
process main/control thread to service bounded slices before parameter access
and between offline render blocks. Never call it from the audio callback. The
result reports event-loop progress only, not license or plug-in readiness, so a
tool must own a configurable warm-up and post-write/render-settle policy. Put
the entire instantiate/process probe in a child process; isolated scanning alone
does not contain crashes in deeper plug-in code.

## Signal graph gotchas

- `SignalGraph` dispatches plugin nodes through the additive
  `PluginSlot::process(format::ProcessBuffers&, ...)` overload. The default
  implementation projects the active main input/output bus back to the legacy
  `process(output, input, ...)` callback, and still calls legacy processing with
  empty audio views for MIDI-only slots. Override the `ProcessBuffers` overload
  when a hosted format or fixture needs direct bus metadata for sidechains,
  auxes, surround, or multi-output products.
- **Canonical-executor routing (DEFAULT ON; `set_canonical_executor_routing_enabled`
  toggles it).** The routed executor is the primary inter-node backend for every
  eligible graph; it is bit-identical to the legacy walk for that subset AND reports
  the same per-node `node_loads()` telemetry (the executor times each node's work via
  a per-binding `AudioProcessLoadMeasurer` wired from the host's persistent node-load
  map), so the default-ON flip is behaviour-preserving where it takes effect. Force it
  OFF to render the walk — the routed-vs-walk parity oracles (`run_legacy`,
  `signal_graph_block`) do that so the walk stays an independent reference. **EVERY
  node kind SignalGraph produces is now eligible** — the only remaining walk triggers
  are an unprepared graph or a routed snapshot/pool BUILD failure (e.g. a topology past
  `GraphRuntimeLimits`); the walk is the deliberate reference/fallback for those, the
  independent parity oracle, and is NOT slated for deletion. **`routed_walk_fallbacks()`**
  counts blocks where a routed path was ELIGIBLE but its dispatch returned failure so
  process() silently fell back to the walk — that degradation is invisible to the parity
  test (the walk is both oracle and fallback), so this counter (plus a once-per-graph
  debug warning) is the only signal an eligible graph stopped routing. It stays 0 in
  healthy operation; a normal fallback (routing disabled / ineligible / walk-by-choice)
  does NOT increment it. An eligible graph —
  nodes AudioInput / AudioOutput / Gain / Plugin (a Plugin with NO live slot routes as
  pass-through-or-zero via `custom_binding(nullptr)`, exactly matching the walk's
  missing-plugin behavior) / MidiInput / MidiOutput / **Custom** (`CustomNodeType`,
  stateless `process` or stateful `process_instance`; routed via `custom_binding`,
  an unresolved/shape-mismatch custom node pass-through-or-zeros exactly as the
  walk does; custom output regions are pinned `persistent_output` like plugins so
  a partial writer keeps its stale tail), connections audio (feedforward,
  one-block feedback, or sidechain — a sidechain edge routes as plain audio into
  a higher input port of the destination plugin), MIDI (connect_midi event
  edges), or parameter automation — sparse (connect_automation, two control
  points) and dense (connect_audio_rate_modulation, per-sample) — can be driven
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
  identically. Each pool ring's mutable samples + write cursor live in a
  shareable state object while the RT lookup still returns raw pointers; this
  is the seam `prepare_swap` uses off-RT to adopt identity-matched PDC history
  without reading or copying live ring contents. MIDI edges route through
  per-node MIDI scratch buffers owned by
  the executor (`GraphRuntimeMidiScratch`); SignalGraph bridges its MIDI
  mailboxes (inject_midi / extract_midi) around the routed call. External
  per-block parameter events cross the same boundary through a per-node
  `inject_parameter_events` mailbox. The routed call appends that publication
  after executor-generated automation and commits its sequence only after the
  whole dispatch succeeds, matching serial fallback and anticipation behavior.
  Parameter automation routes through a `GraphRuntimeAutomationScratch`
  (per-node parameter event queue + per-connection slew state + per-node dense
  buffers): sparse edges sample the source at the block edges, map/slew/mix per
  the connection's resolved bounds, and emit two control points; dense
  audio-rate edges map every sample (through the same per-connection PDC delay
  ring as audio), mix into a per-node buffer, and emit one event per sample —
  both bit-identical to the walk and built into the same per-node event queue.
  A node exceeding
  `kMaxParamsPerNode` (64) distinct sparse OR dense params is kept on the legacy
  walk.
  - **Where the walk lives.** The legacy serial reference walk is no longer
    inline in `process_impl`; it lives in
    `core/host/src/signal_graph_reference_walk.cpp` and is entered via
    `SignalGraph::run_reference_walk_` when no routed path takes a block. It is
    kept deliberately INDEPENDENT of `signal_graph_executor_routing.{hpp,cpp}`
    — do not share or merge its gather / PDC / feedback / MIDI / automation
    execution with the executor. The MIDI-block helpers shared by both the
    routed dispatch and the walk (`clear_midi_block`, `midi_block_has_drops`,
    `copy_midi_block`) live in the shared header
    `core/host/src/signal_graph_internal.hpp`. The dual-maintenance rule still
    applies: any audio-output-affecting edit to the walk must be mirrored in the
    executor (and vice versa), guarded by `test_graph_routing_differential_parity`,
    `test_signal_graph_executor_parity`, and `test_signal_graph_offline_parity`.
  - **Where the live-swap engine lives.** The no-silence topology-edit machinery —
    the swap policy and scanned-plugin catalog, the staged-replacement pipeline,
    the `begin_swap_edit` / `prepare_swap` / `abort_swap_edit` transaction with its
    crossfade publish and rollback, `snapshot_is_plugin_reinit_free_locked_`, and
    the load-admission gate — lives in `core/host/src/signal_graph_live_swap.cpp`,
    so `signal_graph.cpp` keeps the topology/compile/process spine. The file
    arrangement mirrors `signal_graph_reference_walk.cpp`, but the *reason* does
    NOT: these are still ordinary `SignalGraph` members that name its private
    nested types, only their definitions moved. **The reference walk's
    independence rule does not transfer here** — there is no second
    implementation to stay bit-exact against, and factoring shared code out of
    live-swap into `signal_graph.cpp` (or `signal_graph_internal.hpp`, where
    `prepare_midi_block_storage` already single-sources every graph MIDI block's
    real-time capacities for both the compile path and the swap warm-up probe) is
    a fix, not a violation. Everything in the live-swap TU runs on the CONTROL
    thread.
  - **Gap-free PDC carry is identity-based and conservative.** `connections_`
    has a private parallel vector of monotonic identities; every insertion and
    erasure must update both vectors. `CompiledGraph` snapshots those identities
    beside `connections`. During `prepare_swap`, the old and candidate delayed
    edge sets must form an identity-keyed bijection with equal delay sizes and
    equal total graph latency. The candidate then shares, never copies, each old
    domain's audio-thread-owned ring state: legacy `ConnectionDelay`,
    `routed.serial.pool`, and `routed.parallel.pool`. Disconnect+reconnect mints a
    new identity and is refused even when the public `Connection` values compare
    equal. Because those domains keep independent histories, a PDC-active
    `CompiledGraph` pins the execution domain chosen during `prepare()`; relaxed
    routing toggles remain dynamic only for zero-PDC snapshots, and a live swap
    that would change the pinned domain is refused. Feedback graphs,
    routed-validity changes, and latency/delay-structure changes are also refused.
    Tests: `test_signal_graph_pdc_swap_continuity.cpp`
    uses D=97 with 64-frame blocks across all three execution domains and includes
    the reconnect negative plus a concurrent swap hammer.
  - **`_locked_` is a contract, and it is now asserted.** A `SignalGraph` helper
    suffixed `_locked_` requires the caller to already hold
    `graph_mutation_mutex_`; the convention now spans `signal_graph.cpp` and
    `signal_graph_live_swap.cpp`, so it is easier to violate from the far side than
    it was when one TU held every caller. Helpers whose call graph is entirely
    internal open with `assert_graph_mutation_locked_()`, which reads a debug-only
    owner record — so **take the mutex through `GraphMutationLock`, never a bare
    `std::lock_guard`/`unique_lock`**. A bare guard locks correctly but leaves the
    owner record empty, and the next `_locked_` helper you call asserts as though
    you forgot the lock entirely (a debug-only false failure that reads like a real
    one). Only `prepare_swap` calls `GraphMutationLock::unlock()` early, to drop
    the lock before invoking user callbacks. Two `_locked_` helpers cannot assert —
    `has_path_locked_` (reached via `would_create_cycle`) and
    `total_declared_ports_locked_` (via `validate_generated_graph` /
    `estimate_generated_graph_work_units`) — because those public entry points do
    not lock; their suffix documents the internal contract only. (`node_load_mu_`
    is a different mutex and is correctly taken with a plain `lock_guard`.)
  - **`CompiledGraph::routed` groups what is only ever valid together.** Each
    `RoutedPath` (`routed.serial`, `routed.parallel`) owns its own `snapshot`,
    `pool`, `plugin_ctx`, `custom_ctx`, and `valid` flag — driving one path's
    snapshot against the other path's pool is the bug the grouping exists to make
    hard, since the parallel path's assignment is reuse-free and the serial path's
    is compact. The MIDI scratch, automation scratch, and MidiInput/MidiOutput node
    lists sit on `routed` itself and are **deliberately SHARED** by both paths: the
    plans are identical and only ONE path runs per block. That sharing is load-
    bearing — anything that makes those structs carry per-path state (or that runs
    both paths in a block) breaks it silently, because the MIDI mailbox bridge and
    the automation queues would then interleave across paths.
  - **`build_executor_snapshot` prefers the `ExecutorSnapshotBinders` struct.** The
    positional overload still exists as a forwarder for unmigrated call sites, and
    it is exactly where a resolver can go wrong quietly: several of its resolvers
    are same-shaped `std::function<T*(NodeId)>`, so swapping two by argument
    position compiles and mis-binds. Every binder field is optional and has a
    documented fallback (`plugin_latency_for` / `plugin_params_for` empty means
    "fall back to the live slot", which is fine for baked/anticipation callers but
    NOT on the swap path, where the point of the cached accessors is that a
    swap-time build makes no live `PluginSlot` metadata call).
  - **Multi-path routing has an arithmetic guard.**
    `test/test_signal_graph_audio_parity.cpp` (target
    `pulp-test-signal-graph-audio-parity`) renders a fan-out/fan-in topology whose
    paths carry distinct non-commutative transfer functions, and checks the output
    bit-exactly against expectations derived by hand from the stimulus — on the
    walk, the routed serial path, and the routed parallel path. Nothing in it is
    captured from a build, so a routing change cannot move the bar with it: a
    dropped, swapped, or reordered path fails. It also asserts
    `routed_walk_fallbacks()` / `routing_executor_stats()`, since a routed case
    that quietly degraded into the walk would otherwise pass vacuously (the walk is
    both oracle and fallback).
  - **Connection lane CLASSIFICATION is single-sourced** (distinct from the
    execution-independence rule above). Which lane a host `Connection` carries —
    audio / event(MIDI) / automation, plus the orthogonal feedback flag and the
    dense-vs-sparse audio-rate split — is decided in ONE place: `classify()` in
    `signal_graph_executor_routing.{hpp,cpp}`, returning a `ConnectionClass`. The
    runtime structs carry this as a typed `graph::GraphRuntimeConnectionKind`
    discriminator (`Audio`/`Event`/`Automation`, default `Audio`) instead of the
    old independent `event`/`is_automation` bools; read it via the
    `pulp::graph::is_event` / `is_automation_conn` / `carries_audio` accessors.
    BOTH classification surfaces route through `classify()`: the executor-routing
    gather (building `GraphRuntimeConnectionSpec`s) AND the compile-time
    reference-walk edge bucketer in `SignalGraph::compile_` (audio / MIDI /
    sparse-automation / dense-audio-rate / feedback buckets). The PDC/latency
    passes share the matching `connection_affects_latency()` predicate. A
    sidechain edge deliberately classifies as `Audio` (it is plain audio into a
    higher dest port). This is single-sourced CLASSIFICATION only — the gather
    math, PDC delay rings, and MIDI/automation evaluation stay independent and
    dual-maintained per the rule above. New lane mappings are pinned by
    `test_connection_classify`.
- **Transport-aware `process()`.** Alongside the no-transport
  `process(out, in, n)` there is an additive
  `process(out, in, n, const format::ProcessContext& transport)` overload. Both
  delegate to one private `process_impl(..., const format::ProcessContext*)`; the
  3-arg form passes `nullptr` and is bit-identical to its prior behaviour. When a
  transport is supplied it populates the routed `ProcessBlock`
  (`block.transport = &ctx`, `block.mode = ctx.process_mode`) so nodes that consume
  it (e.g. a `ProcessorNode`, via `context_for_block`) see the host playhead, mode,
  and render-speed hint. `block.render_speed` stays the numeric `1.0`: the
  render-speed hint is categorical and travels through `*block.transport`, never the
  multiplier. Nodes that ignore `block.transport` are bit-identical to the
  no-transport path, so routed-vs-walk parity is unaffected. Under active
  anticipation the transport stays LIVE: transport-sensitive nodes are excluded
  from the ahead-rendered interior (see the per-node opt-in below and the
  anticipation gotchas), so every ahead-rendered node is transport-insensitive by
  construction and the forwarding is inert for it.
- **Per-node transport opt-in (`PluginSlot::wants_transport()` + transport
  `process()` overload / a transport-aware custom callback).** A routed plugin or
  custom node OPTS INTO the host transport: a `PluginSlot` overrides
  `wants_transport()` to return `true` and overrides the appended
  `process(ProcessBuffers&, midi_in, midi_out, param_events, n, const
  format::ProcessContext&)` overload; a custom node's type sets
  `process_transport` (stateless) or `process_instance_transport` (stateful).
  `compile_` resolves the capability ONCE into the cached, prepare-stable
  `GraphNode::transport_sensitive` (Plugin: from `slot->wants_transport()`;
  Custom: from a non-empty transport callback registration), resolved BEFORE the
  anticipation eligibility analysis. That ONE cached bit is read by BOTH the
  routed binding (`PluginBindingContext::wants_transport` /
  `CustomBindingContext::process_transport`, which forward the live transport when
  the block carries one) AND the anticipation analyzer (which seeds
  `AnticipationExclusion::TransportSensitive`), so the partition and the bindings
  can never disagree. INVARIANT: never call a live `slot->wants_transport()` per
  block on the audio thread — the bit is cached at compile and a capability change
  requires a re-prepare. A node that does not opt in is byte-for-byte unchanged.
- **Parallel-executor routing (opt-in, default OFF, independent of the serial
  opt-in).** `set_parallel_routing_enabled(true)` drives the SAME eligible subset
  through `GraphRuntimeExecutor::process_parallel` — a levelized fork-join over a
  persistent `GraphRuntimeWorkerPool` (the audio thread is participant 0). Output
  is bit-identical to the serial executor and the legacy walk; the per-node body
  (`run_routed_node`) is shared. Dispatch order in `process()`: parallel (if
  enabled + valid + pool running + fits) → serial executor (if its toggle on) →
  legacy walk. The two routed branches share one `dispatch_routed` bridge, and
  every executor zeroes the output bus + the MIDI ingress is idempotent (consumed
  mailbox sequences aren't committed until `run()` succeeds), so a failed parallel
  attempt re-renders the block on a lower tier with no doubled output or
  double-consumed MIDI. `SignalGraph::set_parallel_min_work_units(n)` forwards
  to the executor's channel-sample break-even gate; default `0` preserves the
  original "parallelize every eligible level" behavior, while a positive value
  keeps low-cost levels serial to avoid fork/join overhead on small graphs. Use
  `routing_executor_stats()` to verify the live path when testing the threshold.
  GOTCHAS: (1) the parallel snapshot uses a REUSE-FREE
  buffer assignment (`parallel_safe=true`) — concurrent same-level nodes must not
  alias a recycled scratch slot; `process_parallel` refuses a non-parallel-safe
  snapshot. (2) Levels containing an AudioOutput node run SERIALLY in topo order
  (AudioOutput `+=` accumulates into the shared output bus; float add is
  non-associative, so order is load-bearing for ≥3 sinks). (3) WORKER-POOL
  LIFECYCLE is load-bearing: the pool is started ONCE (size = clamped hardware
  concurrency, guarded by `worker_count() == 0`) and NEVER stopped/resized on a
  re-prepare — `start()`/`stop()` join threads + reset the epoch/completion
  counters, a UAF if run against an in-flight audio `run()`. The only legal stop
  is `~GraphRuntimeWorkerPool` at SignalGraph destruction. Don't make the thread
  count runtime-variable without a drain handshake. The pool's completion barrier
  counts PARTICIPANTS finished (not tasks): an empty-range participant must still
  register done, or it can race the next batch's published state.
- **Anticipative-rendering eligibility (`anticipation_eligibility.{hpp,cpp}`).**
  `analyze_anticipation_eligibility(nodes, connections)` is the static SAFETY
  contract for rendering a latent subgraph ahead of the audio deadline: it
  classifies each node `None` (passed) or a hard-exclusion reason — seeds live
  AudioInput/MidiInput nodes, both endpoints of every feedback edge, any node with
  a sidechain inbound edge, and any node with `GraphNode::transport_sensitive` set
  (the per-node host-transport opt-in), then propagates each exclusion forward
  along feedforward (non-feedback) edges to a fixpoint so anything downstream of an
  excluded node is excluded too. It's deliberately conservative: a false exclusion
  only forfeits a speed-up, but a false inclusion would render an unsafe node
  ahead. Host-clock sensitivity is handled by the `TransportSensitive` seed: a
  host-clock-dependent node opts in via `wants_transport()` / a transport-aware
  custom callback (resolved into `transport_sensitive` at compile), so it — and
  its downstream cone — is kept out of the ahead-rendered interior and runs live.
  `passes_static_exclusions(i)` true therefore IS sufficient for the partition to
  treat node i as ahead-renderable. The `SignalGraph` anticipative splice gates on
  this analysis when `set_anticipation_enabled(true)` is prepared.
- **Anticipation partition (`anticipation_partition.{hpp,cpp}`).**
  `build_anticipation_partition(nodes, connections, eligibility)` carves the
  renderable eligible INTERIOR (eligible nodes minus the live AudioOutput/MidiOutput
  sinks, which are consumed at the real deadline and must never be written ahead)
  and the BOUNDARY edges (interior-source -> outside-the-interior), which are the
  splice points the renderer pre-computes and the live graph reads. `cost_weight`
  (the same coarse max(in,out) proxy the parallel cost gate uses) +
  `worth_anticipating()` gate out trivial/no-boundary partitions. Still pure static
  analysis — no rendering, no RT path. Builds on the 6a eligibility result and is
  rejected (ok=false) if that result isn't ok or doesn't match the node span.
- **Anticipation sub-graph (`anticipation_subgraph.{hpp,cpp}`).**
  `build_anticipation_subgraph(nodes, connections, partition)` turns a partition
  into a standalone renderable graph: it copies the interior nodes verbatim (plugin
  slots/gain/ports preserved) and the internal edges, and synthesizes ONE
  `AudioOutput` sink whose input ports correspond to the DISTINCT boundary output
  ports (fresh id above every existing node id, so no collision), fed so boundary
  output `i` lands on sink input/output-bus channel `i` — so the sub-graph renders
  through the ordinary `build_executor_snapshot` + `process_routed` and its output
  bus carries exactly the boundary signals without summing them together.
  `outputs[]` maps each output-bus channel back to the `(source_node, source_port)`
  it captures. GOTCHA: the interior plugin
  GraphNodes are copied by value, so the SAME plugin instances render here — which
  means a live splice (a later slice) must NOT also process those instances, or
  their state double-advances. This slice does extraction only; it neither renders
  nor changes any RT path.
- **Anticipation lane (`anticipation_lane.{hpp,cpp}`).** `AnticipationLane` renders
  an eligible sub-graph AHEAD of the deadline into a `PlanarAudioRingBuffer`:
  `prepare()` (off-RT, quiescent) builds the executor snapshot + sizes the ring for
  a FIXED block size; `render_ahead()` (single background producer) advances the
  interior's plugin state and pushes whole blocks; `consume()` (audio thread,
  RT-safe, no-alloc) pops a pre-rendered block or reports underrun so the caller
  falls back to a synchronous render. The block size is PINNED at prepare (before
  any thread exists) so producer/consumer stay in lockstep and there's no
  cross-thread block-size field — the consumed sequence is bit-identical to a
  block-by-block synchronous render. GOTCHAS: (1) the interior plugins are advanced
  ONLY by render_ahead — a live splice that uses a lane must not also process those
  nodes or their state double-advances; (2) render_ahead is SINGLE-producer (all
  calls, including priming, must be serialized — they share unsynchronized
  executor/pool/scratch); only the ring mediates against the consumer.
- **Anticipation splice (`set_anticipation_enabled`, default OFF; runs on the
  canonical executor path).** When enabled + the routed snapshot is eligible + the
  graph has an eligible latent interior, `compile_` builds an `AnticipationLane` +
  a `skip_mask` over the routed plan (the interior nodes) + a prefill map (each
  lane output channel → the interior boundary-source's `exec_pool` output slot).
  The host drives `pump_anticipation()` from ONE background thread (the producer);
  `process()` consumes a pre-rendered block, copies it into the prefill slots (or
  zeros them on underrun / block-size mismatch), and runs `process_routed` with the
  interior masked — bit-identical to the canonical interior-live render. GOTCHAS:
  (1) the branch is TERMINAL once entered — on a routed failure it zeros the output
  and returns rather than falling through to a path that would re-run (double-
  advance) the producer-owned interior. (2) `pump_anticipation` pins the live
  snapshot (RCU object-lifetime only) and is single-producer-guarded, but the host
  MUST stop/join the pump before any `prepare()`/mutation — prepare reinitializes
  the SAME plugin instances the pump renders (a data race otherwise; same rule as
  "no `process()` during prepare"). (3) Host-clock-sensitive nodes opt in via the
  per-node transport capability (`wants_transport()` / a transport-aware custom
  callback → cached `GraphNode::transport_sensitive`). The eligibility pass seeds
  `AnticipationExclusion::TransportSensitive` on that bit, so a transport-sensitive
  node — and its downstream cone — is EXCLUDED from the ahead-rendered interior and
  always runs live/exterior, where it receives the live transport. The former
  blanket transport suppression under anticipation is therefore RETIRED: the
  transport stays populated on every block (inert for the transport-insensitive
  interior). `transport_suppressed_for_anticipation()` is repurposed to count the
  transport-sensitive nodes anticipation forced exterior (resolved at compile),
  not per-block drops.
  (A masked node must not be an `AudioOutput` or a feedback endpoint; the
  partition guarantees this and `process_routed` debug-asserts it.) (4) The lane
  uses a FIXED block size (the prepared max). A block of a different size — or a
  ring underrun — silences the interior for that block (the interior is never
  re-rendered live, so bit-identical-to-canonical holds only for fixed-size,
  kept-up blocks); and an interior param/gain edit takes effect at render-ahead
  time, a lead earlier than a live render. The anticipation branch is
  STRUCTURALLY terminal once `anticipation_valid` — it never falls through to the
  parallel/legacy paths (which would run the producer-owned interior live), even
  if the pool can't fit the block (then: silence).
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
- Per-node live-DSP telemetry (`audio::LiveDspTelemetryStore`): richer than the
  load measurer — fixed-slot p50/p95/p99 + jitter + over-budget attribution.
  Disabled by default (`set_live_dsp_telemetry_enabled()`; the audio path is one
  predicted-not-taken branch when off); drain + read a snapshot copy via
  `poll_live_dsp_telemetry()` (single non-RT poller). Unlike `node_load_`, the
  store is PER-`CompiledGraph` (not a SignalGraph member): it rides the RCU
  snapshot lifetime, so telemetry resets on a topology recompile (a new topology
  is a new timing baseline) and no re-prepare races the audio thread. Recording
  is PATH-AGNOSTIC and lives at ONE site: a guard at the top of `process_impl`
  destructs after the block (reverse-order vs the graph-load end guard) and reads
  the values BOTH the routed serial executor and the legacy walk already stamped
  into each node's persistent `AudioProcessLoadMeasurer` + `graph_load_`, then
  pushes one fixed-slot record via `inject_block()` over a pre-sized scratch
  (`external_record_scratch()`). This is why there is NO per-node hook in the
  executor or the walk — both already time per node, so the store just harvests
  those measurements once per block. `canonical_executor_routing_enabled_`
  defaults TRUE, so the serial executor is the common path; harvesting the
  measurers (not hooking the walk) is what makes telemetry work by default.
  Per-node slot == the node's index in `ordered_runtime`, matching the store
  metadata built at compile.
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
- `SignalGraph::inject_parameter_events()` uses a separate prepared per-node
  mailbox with one control-side writer. Publications are latest-wins and
  one-shot: the next successful serial, routed, parallel, or anticipation
  block consumes the newest sequence once. Append injected events after graph
  automation before the stable sample-offset sort; this preserves graph
  automation when the fixed queue is full and lets injected events win a
  same-offset tie. Reuse the mailbox across gap-free snapshots for the same
  plugin node so an edit cannot discard a publication made before the swap.
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
  (`core/host/signal_graph_executor_routing.{hpp,cpp}`).** The eligible subset is
  described above under "Canonical-executor routing" and enforced by
  `signal_graph_topology_executor_eligible()` /
  `signal_graph_executor_eligible()`. The builder fails closed for unsupported
  Custom nodes, placeholder Plugin nodes, and per-node automation counts above
  the fixed scratch caps. `build_signal_graph_executor_routing()` translates an
  eligible prepared graph into a `format::GraphRuntimeSnapshot` + pre-sized
  `GraphRuntimeBufferPool`; the live `process()` path embeds that snapshot and
  its scratch pool per `CompiledGraph`, so a re-prepare rebuilds fresh routing
  state without resizing buffers an in-flight audio reader holds. The routing
  keeps the live compiled snapshot alive, reads live gain atomics, and invokes
  the snapshot's live PluginSlots, so **rebuild routing after any re-prepare**
  and keep this section aligned with `test_signal_graph_executor_parity`.

## Offline graph rendering (`OfflineSignalGraphHost`)

`core/host/offline_signal_graph_host.{hpp,cpp}` renders a prepared `SignalGraph`
offline by stepping a fixed block size across a frame range through the **public**
`SignalGraph::process()` — no live audio device, deterministic, allocation-free per
block (staging + output buffers are sized in `prepare()`). It is a control-thread
host, not a routing path: it adds no walk of its own and never touches graph
internals, so it stays clear of the in-flight routing/anticipation churn.

Gotchas:
- **Block-size silence clamp.** `SignalGraph::process()` zero-fills any block larger
  than the prepared `max_block_size` (`prepared_max_block_size()`). `prepare()` refuses
  if the configured `block_frames` exceeds the graph's prepared max — otherwise an
  offline "one big block" render would silently drop to silence. To render one big
  block, re-`prepare()` the graph at that block size first.
- **What "offline equals online" actually means here.** A `SignalGraph` carries no
  `ProcessMode`/transport into its nodes, so an offline render is NOT distinguishable
  from an online one by render mode — the only variable is the block partitioning. For
  deterministic nodes, output is therefore block-size invariant: same input at any
  block size → bit-exact for pure gain/sum, within ~1e-6 across re-partitioning. A node
  whose output legitimately depends on block size (the exempt path) is declared
  EXEMPT as harness-side metadata today (no per-node `ProcessMode` opt-out exists yet);
  the equivalence harness flags and excludes it rather than failing.
- Keep the executor/parallel/anticipation opt-ins OFF for partition-invariance
  fixtures — anticipation in particular is intentionally not block-size invariant.

## Baking a graph to a `Processor` (`BakedGraphProcessor`)

`core/host/baked_graph_processor.{hpp,cpp}` — `bake(const SignalGraph&)` freezes a
prepared, fully-lowerable graph into one `pulp::format::Processor` that runs a frozen
`GraphRuntimeSnapshot` through the SAME `GraphRuntimeExecutor::process_routed()` the
live graph uses, so baked output is bit-identical to the live graph for the lowerable
subset. The artifact is a *serialized fused plan* (data), not generated code — it
reuses the one backend, so the baked Processor only CALLS `process_routed`, never
defines a routing entry point.

Gotchas:
- **Lowerable subset is narrow by design.** Today: `AudioInput`/`AudioOutput`/`Gain`
  only. `bake()` REFUSES loudly (null processor + a `LowerRejectReason`) for an
  unprepared or executor-ineligible graph, a hosted `Plugin` node (opaque external
  state — not self-contained), or a `Custom` node (lowering is a follow-up). The
  node-kind refusals are checked BEFORE the eligibility predicate so a Plugin/Custom
  graph reports its specific reason instead of a generic `NotExecutorEligible`.
- **The baked Processor owns its Gain values.** `bake()` copies each Gain's value into
  the Processor; `prepare()` seeds one heap-stable `atomic<float>` per Gain (a
  `unique_ptr` vector, never a value vector) and resolves the routed Gain bindings to
  those owned atomics — so the baked Processor is independent of the source graph's
  live snapshot lifetime. A second `prepare()` clears the old snapshot/pool/atomics
  before rebuilding, so binding pointers never dangle.
- **Sizing mirrors live routing.** `prepare()` builds the snapshot via the same
  `build_executor_snapshot()` the live routing uses and sizes the pool from
  `buffer_slot_count()` × `max_buffer_size` plus the per-connection PDC rings, so
  `process()` is allocation-free.
- **bake() captures topology + gain values, not hot runtime state.** The baked
  Processor builds fresh feedback/delay/scratch in `prepare()` and starts from zero;
  a source graph that has already processed blocks does not transfer its feedback
  history. The parity proof covers both directions — baked output is bit-exact to the
  live graph's legacy WALK and to its routed executor (the test asserts the walk case
  explicitly by forcing routing OFF, since canonical-executor routing is now ON by
  default).

## Common tripwires

- **Instruments have no input bus — never address input element 0 blind.**
  An AU instrument (`aumu`) and a generator (`augn`) expose **zero input
  elements**; so does a MIDI processor (`aumi`, which despite the name is
  `kAudioUnitType_MIDIProcessor`, not an instrument — and it may expose no
  *output* element either). A MIDI effect (`aumf`) *does* have audio input and
  is unaffected. Setting a per-element input property on an input-less AU —
  `kAudioUnitProperty_StreamFormat`, `kAudioUnitProperty_SetRenderCallback` —
  returns `kAudioUnitErr_InvalidElement` (**-10877**), *not* a format error.
  Treating that as fatal rejects **every instrument on the system** while every
  effect keeps working, so the failure is invisible to effect-only tests. Ask
  `kAudioUnitProperty_ElementCount` on the scope first and skip the input-side
  setup when it is 0 (`scope_element_count()` in
  `core/host/src/plugin_slot_au.mm`). When the AU does not answer the query,
  assume an input bus **exists**: a non-answering effect then behaves as it
  always did, and a non-answering instrument fails loudly at the property set
  with a named scope+status. Assuming *none* would skip input setup on that
  effect and leave every `AudioUnitRender` failing while the caller's buffer
  keeps stale contents — **silent wrong audio, the worst of the four outcomes.**
  The general rule for any backend: derive the bus layout from what the plug-in
  **reports**, never from the assumption that an input side exists. The other
  slots already do this and are the pattern to copy — VST3 loops
  `component_->getBusCount(kAudio, kInput)` and ignores `setBusArrangements`'
  status ("missing buses degrade gracefully"); CLAP and VST3 both size
  `ProcessData` from the caller's view. AU was the outlier.
- **The LV2 slot cannot safely host an instrument — and it fails as UB, not
  cleanly.** Port discovery keeps only `lv2:AudioPort`/`lv2:ControlPort` stanzas
  (`plugin_slot_lv2.cpp`, `if (!is_audio && !is_control) continue;`), so atom /
  event / CV ports are never seen and never `connect_port`'d — yet `run()` is
  called anyway. The LV2 spec requires **every** port be connected before
  `run()` unless it is `lv2:connectionOptional`; running with unconnected ports
  is undefined behavior and commonly segfaults. Every LV2 instrument has an atom
  MIDI input port, and many effects carry atom ports for transport. There is
  also no MIDI delivery path for LV2 at all. Unlike the AU trap above this does
  not fail loudly — so **do not claim "Pulp hosts instruments" unqualified**:
  AU yes, VST3/CLAP plausibly, LV2 no. Fixing it means an atom-sequence input
  buffer + MIDI mapping; the minimum stopgap is to detect non-audio/non-control
  input ports at discovery and refuse `prepare()` loudly unless
  `connectionOptional`.
- **Test hosts against a real *instrument*, not just a real effect.** The
  effect-only integration test in `test/test_plugin_slot_au.mm` passed happily
  through the bug above. Apple's bundled `DLSMusicDevice`
  (`kAudioUnitType_MusicDevice` + `kAudioUnitManufacturer_Apple`) ships on every
  Mac, so an instrument fixture costs nothing —
  `first_apple_instrument_unique_id()` is there for this. Any host change that
  touches bus/format negotiation needs both shapes, or half the plug-in universe
  goes untested. (These tests WARN-and-return when no system AU is registered;
  a headless VM may not surface Apple's AUs, so treat a green run in CI as
  "not disproven" rather than "covered" — a **skip is never a pass**.)
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
- The AU slot (`plugin_slot_au.mm`) has the same rule for its output
  `AudioBufferList`: `AuSlot::process` builds an ABL pointing at the caller's
  channels every block. Size the backing `abl_storage_` once in `prepare()`
  (`num_channels_`) via `au_internal::reserve_audio_buffer_list` and only
  *refill* it per block (`fill_output_audio_buffer_list`) — never allocate a
  fresh `std::vector` in `process()`. The ABL build lives in
  `plugin_slot_au_internal.hpp` so its no-alloc invariant is unit-tested
  (pointer-stable across thousands of refills) without a live AU;
  `test_plugin_slot_au.mm` additionally drives a real system Apple effect AU
  through `process()` (skips honestly when none is registered — headless CI
  may surface no AUs). Do NOT assert `allocs==0` over `AudioUnitRender` itself
  (Apple allocates internally); assert the reuse invariant on our buffer.
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

The host exposes a reader-pinned audio-thread snapshot, not direct member
reads. Anything you write that touches the audio thread (a
graph editor, an MCP bridge, a preset loader) must account for these
rules:

- **The snapshot lives in `runtime::Slot<CompiledGraph>`** (`live_slot_`), the
  shared reader-pinned RCU primitive in `core/runtime/include/pulp/runtime/slot.hpp`.
  It owns the atomic pointer the audio thread loads, the seq_cst reader count,
  and the retire list. `SignalGraph` no longer hand-rolls any of that; the old
  `live_` / `live_raw_` / `retired_snapshots_` / `active_process_readers_` /
  `ProcessReadGuard` / `retire_snapshot_` / `prune_retired_snapshots_` /
  `wait_for_retired_snapshots_` are gone. Don't reintroduce them.
- **A pin guarantees LIFETIME, not constness.** This is the single most
  misread part of the contract. `CompiledGraph` is *not* immutable — the audio
  thread writes every node's scratch buffer through the pin on every block,
  `inject_midi` writes mailboxes, `drain()` consumes telemetry, `set_node_gain`
  writes a gain. What is immutable is the *topology*. `Slot::ReadGuard::get()`
  therefore hands back a mutable `T*`; a genuinely read-only publication says so
  in the type (`Slot<const T>`).
- **Read it, don't reach for it.** Anything that dereferences the snapshot off
  the prepare/release thread must hold a pin for the whole dereference:
  `auto pin = live_slot_.read(); if (auto* cg = pin.get()) { ... }`. That
  includes control-thread readers (`inject_midi`, `extract_midi`,
  `node_latency_samples`, `set_node_gain`, `pump_anticipation`) — without the
  pin a concurrent `prepare()`/`release()` can retire and free the snapshot
  mid-dereference.
- **The `live_*()` getters are NOT pinned.** `is_prepared()`,
  `prepared_max_block_size()`, and friends read `live_slot_.live()` directly.
  They are control-thread-only by contract, and `Slot` will not save a caller
  who uses them concurrently with `prepare()`.
- **`unpublish()`, not `publish(nullptr)`.** The latter is ambiguous between
  Slot's `shared_ptr` and `unique_ptr` overloads.

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
- **External parameter-event mailbox.** Hosts publish per-block events with
  `SignalGraph::inject_parameter_events()` before `process()`. The API is
  additive on `SignalGraph`; do not add a `Processor` or `PluginSlot` virtual.
  Sample offsets are block-relative. A `false` return reports an invalid or
  unavailable node, or a source queue that already overflowed; a retained
  source prefix can still be published and consumed. Destination overflow is
  observed later when the audio-thread merge fills the fixed queue.
- **Node ABI surface.** `PluginSlot` includes
  `pulp/runtime/node_abi.hpp` and participates in the node ABI
  virtual-order gate. Existing virtual methods may not be inserted,
  removed, or reordered; add new virtual methods only after the current
  tail and let `tools/scripts/node_abi_gate.py --mode=report` verify
  the diff against the PR base.
- **Thread rules doc.** `docs/reference/host-thread-rules.md` is the
  canonical reference.

## Hosted plugin editors

`EditorAttachment::create(slot, window)` embeds a hosted plugin's own GUI.
Wired for **CLAP on macOS**; VST3 / AU / LV2 slots still report no editor. The
non-obvious parts:

- **The APIs are inverted from `HostedEditor`.** CLAP `set_parent` and VST3
  `IPlugView::attached` CONSUME a parent view — the plugin inserts its own view
  into what you hand it and never hands one back. `HostedEditor` is the other
  way round (the slot returns a handle the host embeds). The slots reconcile
  this with a host-owned container `NSView` (`hosted_editor_container.hpp`)
  reported as `native_handle`. AU v2's CocoaUI does return a view, but wraps it
  in the same container so teardown is uniform. Do not "simplify" this away.
- **The container is inserted into the parent window at CREATION**, not at
  attach. `EditorAttachment::create` calls `create_hosted_editor()` BEFORE
  `attach_native_child_view()`, so a container that deferred insertion would
  have the plugin run `set_parent`/`attached` against a windowless view —
  editors that bring up Metal/OpenGL layers misbehave there. The later attach is
  an `addSubview:` move within the same window.
- **`core/host` is compiled WITHOUT ARC.** Container views are retained and
  released by hand. Check `flags.make` before assuming ARC in any `core/host`
  `.mm`.
- **`clap_host_gui` must be served from `host_get_extension`.** It returned
  nullptr for every extension before editors existed, so a plugin could not call
  back at all. `request_show`/`request_hide` are denied — Pulp only creates
  embedded editors. Deny rather than pretend.
- **The thread rules are ASYMMETRIC, and getting this backwards is the easy
  mistake.** Every `clap_plugin_gui` call (`create`, `hide`, `destroy`,
  `can_resize`, `set_size`…) is `[main-thread]`. But the `clap_host_gui`
  callbacks a plugin invokes are `[thread-safe]` — `request_resize` and
  `resize_hints_changed` are `[thread-safe & !floating]`, and
  `request_show`/`request_hide`/`closed` are `[thread-safe]`. A plugin may call
  them from a render thread. So a host callback must NEVER walk straight into a
  `clap_plugin_gui` call or a native view — that runs a main-thread-only API off
  the editor's thread. The slot records the thread that opened the editor and
  refuses (`request_resize` → false, which the spec allows) or defers (`closed`
  → pay the destroy at teardown) off-thread. Any `bool` a `[thread-safe]`
  callback touches must be atomic: two callers can otherwise both pass a
  test-then-set and double-destroy the plugin's gui.
- **Do not call `set_scale()` on cocoa/uikit.** `clap/ext/gui.h` documents them
  as logical-size APIs that must not receive it; win32/x11 are physical-size and
  do want it.
- **The gui and the container have independent lifetimes.** A plugin can report
  its gui destroyed (`clap_host_gui::closed(was_destroyed=true)`) while the
  caller still holds a `HostedEditor` pointing at the container. Freeing the
  container there dangles that handle, because `EditorAttachment` detaches by
  pointer on release. Acknowledge with `destroy()` only; let the caller's
  teardown release the container.
- **A native child ALWAYS composites above Pulp's GPU layer** — you cannot paint
  Pulp chrome over an embedded editor. This is a fixed OS behavior, not a bug to
  fix, and it constrains node-editor designs.
- **Pulp's OWN plugins withdraw `clap.gui` under `CI=1` / `PULP_HEADLESS=1` /
  `PULP_TEST_MODE=1`** (`format/detail/editor_environment.hpp`). A dogfood test
  asserting a fixed `has_editor()` will pass locally and fail in CI; read the
  same flag instead.
- **Testing without a bundle:** `make_clap_slot(info, creator)` in
  `core/host/src/plugin_slot_clap_internal.hpp` builds a slot around a
  caller-created `clap_plugin_t`, skipping dlopen. The creator receives the
  slot's real `clap_host_t`, so a fake can call back into `clap_host_gui`. See
  `test/test_clap_hosted_editor.mm`.

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

## NativeHandleVisitor — typed access to a plugin's native handle

Hosts that need to reach a format-specific handle (CLAP note ports,
VST3 IMidiMapping, AU AudioUnit property, LV2 instance) subclass
`NativeHandleVisitor` (`<pulp/host/native_handle_visitor.hpp>`),
override the `visit_*` methods they care about, and call
`slot.accept(visitor)`. Default `visit_*` fall through to
`visit_unknown` so a visitor that only cares about one format ignores
the rest automatically. The base `PluginSlot` dispatches to
`visit_unknown` for placeholder / unresolved slots so they degrade
gracefully.

Format-specific `*NativeHandle` structs deliberately expose handles as
`void*` so they don't pull SDK headers into client code. Callers that
*do* link the SDK `static_cast` back to the concrete type. Wired in
`ClapSlot`, `Vst3Slot`, `AuSlot`, `Lv2Slot`. Tests in
`pulp-test-native-handle-visitor` pin the visit dispatch + the
`NativeHandleFormat` enumerator values (the latter is a
reorder-detector).

The pre-rename spellings (`ExtensionsVisitor`, `*Extension`,
`ExtensionFormat`, and the `<pulp/host/extensions_visitor.hpp>` include
path) still resolve via deprecated aliases, so existing host code keeps
compiling — but new code should use the `NativeHandle*` names.

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

## `latency_samples()` alone is not a fact — ask `latency_query()`

`PluginSlot::latency_samples()` returns an `int` whether or not the backend can
actually ask the plugin. On its own it cannot distinguish **"this plugin reports
zero latency"** from **"this backend has no way to ask"** — and collapsing those
turns an unanswered question into a confident zero, which anything downstream
will happily treat as verified.

`PluginSlot::latency_query()` is the fact:

| | |
|---|---|
| `Available` | The value is the plugin's own report. Zero means zero. |
| `Unsupported` | This backend cannot ask. The value is meaningless. |
| `QueryFailed` | The backend asked and the plugin errored. |

It defaults to `Available` (CLAP, VST3, and AU all read a real value). **The LV2
slot overrides it to `Unsupported`**: it does not read the plugin's
`lv2:reportsLatency` control port, so its `latency_samples() == 0` is a
placeholder, not a claim. Wiring that port is the fix; until then, anything that
reports or gates on hosted latency must branch on `latency_query()` first.

`pulp audio render --latency-report` does exactly that, which is why an LV2 plugin
comes back `unsupported` rather than being falsely certified as zero-latency.
