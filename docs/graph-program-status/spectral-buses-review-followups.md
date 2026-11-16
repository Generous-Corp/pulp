# Spectral and bus-layout review follow-ups

Two independent reviews (adversarial and code quality) read the routed spectral
graph, the finite stream rule, spectral persistence, the Processor aux-bus
adapter, the paced-timing and RTSan tooling and the P3/A2/A9 consumers at
`origin/main` `6b072721d25a`. The defects that were safe to fix with a test are
fixed beside this file. The items below need an owner decision or are larger
than a minimal fix. Each names the code it was re-verified against.

## Fixed in the same change

- A Processor with no input bus and 16 output buses failed every block: the
  routed block always adds a main input bus, so the 16th output overflowed
  `BusBufferSet::kMaxBuses`. `prepare()` and `routed_output_channels()` now
  reserve at least one input slot (`core/format/src/processor_node_adapter.cpp`).
- Spectral save returned an empty string for graphs with repeated node names.
  The spectral reader no longer requires unique names; node ids identify nodes.
- Spectral save and load admitted every graph at a 4096-sample block, so a graph
  that fits the 64 MiB bank only at smaller blocks could not be saved. Saving
  now admits at the block the graph is prepared for (or its limits), and loading
  at the destination's `GraphLimits::max_block_size`, both capped at 4096.
- Nodes downstream of any upstream fatal reported `CapacityExceeded`. They now
  report the new `SpectralError::UpstreamFatal` (value 21); the failing node keeps
  its own cause.
- Engine split results are mapped to graph errors by one function,
  `to_spectral_error`, instead of collapsing every analysis failure to
  `ChronologyExhausted` and every synthesis failure to `SynthesisOverflow`.
- The P3 evidence receipt's binding-gap section described the 256/64 mono-only
  asset source. It now states the current rule and the narrower remainder.
- A leftover write to `/tmp/pulp-p4-raw-writer-test.jsonl` in
  `test/test_gpu_audio_trace.cpp` was removed.

## Deferred: owner decision

1. **Typed refusal reason from `add_spectral_asset_morph_v1`.**
   `core/host/src/spectral_graph.cpp` calls `SpectralAssetMorphConfig::make`
   without its `refusal` out-parameter, so a refused geometry reaches the caller
   as `UnsupportedOperation`; a zero source length is `InvalidFrame` and a
   wrong-length asset is `IdentityMismatch`
   (`core/host/src/spectral_asset_morph_internal.hpp:66-77`). Fixing it needs a
   public result shape (a reason field on `SpectralAssetMorphPorts` or a new
   `SpectralError` value such as `InvalidArgument`). That is an API choice.
2. **Unnamed aux buses all become `"aux"`** (`processor_node_adapter.cpp`,
   `prepare()`), so `find_by_name("aux")` reaches only the first one; graph ports
   keep positional `outN` names. Renaming them (`aux1`, `aux2`, ...) changes the
   names a Processor sees and needs an owner call.
3. **Saving a spectral graph has no error channel.** `GraphSerializer::to_json`
   returns only a string, so every spectral refusal is an empty string with no
   reason. An error out-parameter is a public API change.

## Deferred: larger than a minimal fix

4. **One output-bus layout function.** The bus cap, main-first offsets, negative
   channel clamp and the "main declares 0 takes the whole view" rule are derived
   in four places: `ProcessorNode::prepare`, `routed_output_channels`,
   `add_output_buses` and `name_processor_aux_output_ports`
   (`core/host/src/signal_graph_authoring.cpp`). A pure
   `processor_output_bus_layout(descriptor)` in `pulp::format` would replace all
   four. The no-input cap fixed here had to be applied in two of them.
5. **One spectral geometry limits struct.** FFT 256 to 16384, power of two, hop
   at most N/2 and at most 16 channels are written in
   `core/graph/include/pulp/graph/spectral_frame.hpp`,
   `core/signal/include/pulp/signal/spectral_frame_codec.hpp`,
   `core/host/src/spectral_finite_stream_internal.hpp` and
   `SpectralAssetMorphState::max_channels`; `std::array<..., 16>` is hardcoded in
   `core/host/src/spectral_graph_internal.hpp`. Needs a constexpr limits struct
   with `static_assert`s at each use.
6. **Split `SpectralRuntimeBank::Node::process`** (about 145 lines in
   `spectral_graph_internal.hpp`) into per-kind functions, and separate the
   prepare-time bank from the callback logic.
7. **RTSan callee controls prove the report, not the call site.** The malloc,
   node-malloc and voice-malloc controls in
   `test/cmake/p1_event_admission_rtsan_tests.cmake` share one regex. They should
   also require the stack to contain `inject_callee_control` plus a frame under
   `SignalGraph::process` (node) or `VoiceBankBinding`/`render_stereo` (voice).
   The positive gate matches output with `PASS_REGULAR_EXPRESSION`, which ignores
   the exit code; it holds only while the success line is printed last. Both need
   a `PULP_SANITIZER=realtime` build to verify, which this change did not run.
8. **Paced matrix and G-12 qualification tools share rules by copy.**
   `tools/graph_paced_timing_matrix.py` and `tools/graph_performance_qualification.py`
   import each other through `sys.path.insert`, duplicate the threshold,
   nearest-rank percentile and miss rule, and `parse_g12_output` rewrites
   `build_type=`/`build_dirty=` tokens before calling the shared validator. Needs a
   shared timing-rule module and a `check_stamp=False` validator parameter.
9. **Glue-line budgets.** A9 counts non-blank, non-comment lines of `main.cpp`
   and caps its harness header separately; A2's 197-line figure is measured by
   hand and unenforced. `examples/graph-agent-loop/assert-glue-budget.cmake`
   hardcodes `GRAPH_AGENT_LOOP_*` names, and the three-control loop is copied in
   `examples/graph-agent-loop/CMakeLists.txt` and
   `test/cmake/graph_agent_loop_consumer_tests.cmake`. Needs a neutral counter in
   `tools/cmake` and one stated glue definition.
10. **Installed-consumer drivers.** About 30 `test/cmake/test_*consumer*.cmake`
    drivers each re-implement install, configure, build, ctest and an exact
    count. The A2 and A9 drivers forward no instrumentation flags and wipe their
    root with `REMOVE_RECURSE`, unlike the resynthesis and asset-morph drivers.
    Needs one `PulpInstalledConsumerTest.cmake` function.
11. **Program IDs in test names, tags and a wire schema.** Test names and tags
    such as `[g5]`/`[g12]`, the `pulp.a9.action-log/v1` schema and README titles
    carry planning IDs. `docs_noise_lint.py` does not catch `[gN]`/`[pN]`/`[aN]`
    tags. Renames must also update the fixture selection in
    `tools/graph_paced_timing_matrix.py`, which selects by test-case name.
12. **Node-type string tables disagree.** `graph_serializer.cpp` and
    `spectral_graph_persistence.cpp` write `audio_in`/`audio_out`;
    `graph_rt_report.cpp` and `signal_graph_prepared_topology_edit.cpp` write
    `audio_input`/`audio_output`. One table with document and report columns
    would make the split deliberate.

## Not on main

- The P4 GPU admission test (`test/test_graph_gpu_resource_admission.cpp`) and
  `docs/graph-program-status/P-4.md` exist only on an open pull request. Its
  device-free controls are registered only under
  `PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF`, so no CI lane builds them, and the
  device-loss case compares both consumers against the first manifest entry.
  These belong to that pull request's author.

## Program issue text

The program issue's "Remaining P3 gap" line should be restated as: the only
codec source is a two-asset morph; there is no single-stream codec EOF source;
non-dividing hops are refused; and no installed consumer runs the asset source
at a geometry other than FFT 256, hop 64, mono. The issue body also still lists
the merged spectral persistence work as open. Editing the issue was outside this
change.
