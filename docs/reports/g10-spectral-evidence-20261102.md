# G-10 spectral SignalGraph evidence

Date: 2026-11-02  
Branch: `codex/g5-g10-impl-20261102`  
Observed head: `0474c1d9ebbba2ba69d86b1bbf3b9fa502d9a5e1`  
Platform scope: macOS arm64 source checkout; no build directory was present.

## Bounded verdict

**BLOCKED / implementation contract missing.** The evidence below remains valid,
but Wave 2 implementation cannot proceed until an owner specifies the public
typed-frame adapter contract. No SignalGraph or spectral production code was
invented in this lane.

**PARTIAL / evidence-only.** The existing public APIs prove that a graph can retain an
in-process `format::Processor` through `SignalGraph::add_processor_node()`, but the
existing spectral processors are `pulp::signal` classes, not `format::Processor`
subclasses. Their public contracts consume/emit typed planar sample blocks or
complex spectral frame groups. `ProcessorNode` only adapts the `format::Processor`
`ProcessBlock` ABI and has no public typed-frame adapter. Wiring these classes into a
SignalGraph therefore requires a new adapter/runtime contract, which is outside this
fixture/report-only slice and is intentionally not invented here.

## Positive controls (current public API)

1. `core/host/include/pulp/host/signal_graph_runtime.hpp` declares
   `SignalGraph::add_processor_node()` overloads for `std::unique_ptr<format::Processor>`
   and `std::shared_ptr<format::ProcessorNodeInstance>`.
2. `core/format/include/pulp/format/processor_node_adapter.hpp` documents and exposes
   `ProcessorNode` around a non-owning `format::Processor`, with `prepare()` and the
   routed `ProcessBlock` binding.
3. `core/signal/include/pulp/signal/spectral_mask_processor.hpp` exposes
   `SpectralMaskProcessorT::process(const SampleType* const* input,
   SampleType* const* output, int num_samples)` and `process_frame(...)`; it is not a
   `format::Processor` subclass.
4. Existing spectral tests cover standalone behavior, including identity reconstruction,
   exact mute, frame publication, latency alignment, and allocation-free processing.

## Planted-negative controls

1. A direct `add_processor_node(std::make_unique<pulp::signal::SpectralMaskProcessor>())`
   cannot type-check because the graph overload requires `std::unique_ptr<format::Processor>`.
2. `ProcessorNode` cannot be constructed from `SpectralMaskProcessor`; its constructor
   parameter is `format::Processor&` and its binding forwards `ProcessBlock`, while the
   spectral processor's frame callback uses `std::complex<SampleType>` channel frames.
3. The `.pulpgraph`/`.pulpbake` serialization paths intentionally reject runtime-only
   `ProcessorNode` instances; serialization is not a workaround for the typed-frame gap.

## Frame-flow contract draft (no runtime claim)

```
SignalGraph audio block (planar samples, graph block size)
  -> [missing public typed-frame adapter]
  -> SpectralFrameEngine analyze() / coherent channel-group frame
  -> spectral stage (mask, delay, morph, cross-synthesis, etc.)
  -> SpectralFrameEngine synthesize_frame() / WOLA output
  -> graph output block
```

The missing adapter must define, before implementation is proposed: frame ownership and
borrowing lifetime, analysis-hop and synthesis-hop scheduling, channel-group shape,
latency/tail reporting, block-to-frame chronology, failure/back-pressure behavior, and
whether frame stages may be composed with graph automation. None of those semantics are
added by this evidence slice.

## Exact evidence commands

```sh
rg -n "add_processor_node" core/host/include/pulp/host/signal_graph_runtime.hpp
rg -n "class ProcessorNode|Processor& processor|process_binding" \
  core/format/include/pulp/format/processor_node_adapter.hpp
rg -n "class SpectralMaskProcessorT|process\(|process_frame" \
  core/signal/include/pulp/signal/spectral_mask_processor.hpp
rg -n "SpectralMaskProcessor|SignalGraph" test docs/reference
```

Expected positive controls are the declarations listed above. The planted-negative
controls are compile/type-boundary checks and are recorded as expected failures; no
speculative compile probe or runtime adapter was added.

## Gates and next action

No code or build-system files changed, so no build gate was run. The worktree had no
`build/` directory. The next safe action for `surface:1000120036` is to choose an
explicit owner for the typed-frame adapter contract and define its public API before
any SignalGraph wiring is attempted. Implementation status: **BLOCKED**, no PR opened for speculative runtime wiring. The prior evidence PR remains the source receipt: https://github.com/Generous-Corp/pulp/pull/11427
