# G-10 spectral SignalGraph evidence

Current status (2026-11-03): **PARTIAL**. The Wave 2 adapter update below is
implemented and tested; the original evidence verdict records the earlier
2026-11-02 state. Direct routed spectral ports, canonical persisted encoding,
and A3 qualification remain open.

Date: 2026-11-02  
Fixture source branch: `main`

Observed fixture head: `a9197c3505d7ccb7df57f2907504271171552c93`

This evidence predates the status-only disposition commit `34c5c9fb47beb652d84c067136605c77fa213e0b`; the fixture source is the valid `main` parent.

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

## Wave 2 adapter update (2026-11-03)

The later Wave 2 slice adds `format::SpectralFrameProcessor`, an owned in-process
`format::Processor` that SignalGraph can retain and route through its existing
audio ports. It wraps the existing `SpectralFrameEngineT<float>` once and calls a
bounded ordered chain of owned `SpectralFrameStage` objects for each coherent
channel-group frame. A stage receives a borrowed `SpectralFrameView` only during
its callback. The view carries sequence, signed analysis-window origin, sample
rate, FFT size, hop, window, channel count, and positive-bin count. The bins use
the engine's DC..Nyquist positive-frequency convention; DC and Nyquist are real.
The engine fixes the equal-hop WOLA schedule and reports `fft_size + hop` latency.
There is no stage tail or alternate synthesis hop in this adapter.

The stage chain is installed before prepare, runs on the audio thread, and may
rewrite the frame in place. Rejected or non-finite frames are zeroed before
synthesis; the first `SpectralFrameError` is queryable until reset. Reset restarts
sequence and stream origin. The adapter takes the graph's prepared sample rate,
block size and channel count and refuses incompatible geometry. Existing graph
audio edges allow the adapter to compose with other graph nodes and processors.

This bounded adapter does **not** add a routed `spectral{…}` graph port or
inter-node frame transport. Graph edges still carry audio samples. It also does
not define a persisted canonical frame encoding, assets schema, stage parameter
automation, or G-5 granular time-index. These are remaining G-10 contracts; this
slice must not be cited as proof of the full G-10 phase or the A3 flagship.
