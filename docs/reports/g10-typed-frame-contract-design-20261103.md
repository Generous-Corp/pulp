# G-10 typed-frame adapter contract design

Date: 2026-11-03  
Design scope: contract and evidence only; no runtime API or SignalGraph wiring

## Decision boundary

G-10 can advance from evidence to a design review, but not to implementation.
The current graph seam accepts `format::Processor` nodes whose unit of work is a
planar `ProcessBlock`. The spectral seam accepts coherent channel groups of
positive-frequency complex bins. These are different ownership and scheduling
domains. This document fixes the facts an eventual adapter owner must preserve;
it deliberately does not name a C++ type, add a method, or imply that a runtime
adapter exists.

The existing evidence remains the implementation gate: until an owner accepts
this contract and chooses the public API, G-10 is **BLOCKED for runtime work**.

## Source-backed facts

* `SpectralFrameEngineT` is one instance per stream and is not thread-safe. Its
  `prepare()` allocates all FFT, window, ring, and frame storage; after prepare,
  `process`, `analyze`, `synthesize_frame`, `available_output`, `read_output`,
  `reset`, and accessors are allocation-free within configured capacities
  (`core/signal/include/pulp/signal/spectral_frame_engine.hpp:200-212`).
* Geometry is explicit: power-of-two FFT size 256..16384, analysis hop no larger
  than half the FFT, a fixed channel-group count, a maximum host block, and a
  maximum synthesis hop (`spectral_frame_engine.hpp:53-74,91-97`).
* The frame callback receives all channels for one frame and only the positive
  bins DC..Nyquist. The callback may modify those bins in place. The frame
  storage belongs to the engine and is reused (`spectral_frame_engine.hpp:288-315,
  342-371`; `spectral_mask_processor.hpp:61-71`).
* Frame completion is independent of host block chunking. The split API emits
  at `fft_size + first_frame_start + k * analysis_hop`; `samples_until_next_frame()`
  exposes the remaining input distance (`spectral_frame_engine.hpp:332-347`).
* Equal-hop processing reports a fixed delay of `fft_size + analysis_hop` and
  writes exactly the requested block. Split synthesis advances an explicit
  synthesis position and exposes only fully overlapped output through
  `available_output()`/`read_output()` (`spectral_frame_engine.hpp:268-273,
  375-407`).
* `ProcessorNode` is a non-owning adapter around `format::Processor`; its
  prepare-before-process and allocation-free routed `ProcessBlock` contract is
  explicit (`core/format/include/pulp/format/processor_node_adapter.hpp:18-35,
  43-72`). `SignalGraph::add_processor_node()` accepts only
  `format::Processor`/`ProcessorNodeInstance`, and processor nodes are runtime
  only (`core/host/include/pulp/host/signal_graph_runtime.hpp:159-167`).

## Contract to accept before API work

The eventual adapter specification MUST answer each item below in terms that a
host and a frame stage can test. These are requirements on behavior, not a
proposed runtime class.

### Frame value and borrowing

1. A frame is a coherent group of `channels` spectra, each with `num_bins`
   complex values in DC..Nyquist order. Channel order is the graph bus order.
2. A callback receives a borrowed view. The view is valid only for that callback
   invocation; a stage that retains a frame must copy into storage it owns and
   has bounded during preparation. No callback may retain an engine pointer,
   enqueue a raw pointer, or mutate the frame after returning.
3. The adapter owns the engine lifetime for the stream, while a published graph
   snapshot owns the adapter binding through audio-reader quiescence. Destruction,
   replacement, and re-prepare happen off the audio thread after the last routed
   block has retired.

### Chronology and hop scheduling

1. Input blocks are fed in host order. Frame ordinal `k` is tied to the engine's
   `first_frame_start + k * analysis_hop` grid, not to host callback count.
2. A block that crosses one or more frame boundaries emits callbacks in order;
   the adapter must not coalesce, drop, or reorder complete frame groups.
3. Equal-hop mode synthesizes each emitted frame at `analysis_hop` and reports
   the engine's fixed latency. A variable-hop mode must make the synthesis hop
   an explicit, bounded decision for every emitted frame and must expose output
   availability rather than silently changing delay.
4. Reset/seek starts a new stream epoch. The frame ordinal restarts at zero and
   the pre-stream-silence/full-overlap policy is part of that epoch's prepared
   configuration.

### Ownership, back-pressure, and failure

1. `prepare` validates geometry and reserves every buffer needed by the largest
   configured block/frame/hop. Re-prepare is transactional: failure leaves the
   previous prepared state intact.
2. The audio path cannot allocate, lock, wait, throw, or call a control-thread
   operation. A stage that cannot accept a frame must fail closed using a
   predeclared bounded policy (for example, bypass or silence); it may not block
   the graph or retain an unbounded queue.
3. Split output is pull-based. The adapter reports the number of finalized
   samples; a read beyond that count returns the documented fill value and does
   not advance chronology. No implicit buffering may create a variable delay.
4. Any rejected frame, invalid hop, or failed stage decision is observable in a
   control-side diagnostic/receipt. The audio path still has deterministic output
   under the selected failure policy.

### Composition and automation

1. Stages run in a deterministic order on one coherent frame group. The owner
   must state whether source replacement, pre-mask processing, mask application,
   and synthesis are the complete order; a graph edge must not create a second
   hidden frame path.
2. Automation is sampled against frame chronology, not the host block end. If a
   parameter changes inside a block, the contract must define the frame's sample
   position and whether the stage sees the value at frame completion, frame start,
   or an explicitly interpolated trajectory.
3. Sparse parameter events and dense lanes may be translated into a frame-stage
   control snapshot only at a declared boundary. The translation is bounded and
   allocation-free, and stale events cannot attach to a reset or replacement
   stream epoch.
4. Serialization/baking remains unavailable for runtime-only typed-frame state
   unless a separate, versioned value representation is specified. A serialized
   graph must never imply that a borrowed frame callback or live stage pointer is
   portable.

## Positive and planted-negative controls

These controls are sufficient to review the design without adding an adapter.

**Positive controls (must stay true):**

* A standalone spectral identity stage reconstructs the input within the
  existing WOLA test tolerance for multiple host block partitions.
* A frame observer sees one callback per completed coherent frame, with the same
  ordinal and channel/bin shape when the same input is partitioned differently.
* Equal-hop output has the documented constant `fft_size + analysis_hop`
  latency, and split output exposes only finalized samples.
* A prepared stage performs no allocation or blocking on the callback path.

**Planted negatives (must fail closed):**

* Passing `SpectralMaskProcessor` directly to `SignalGraph::add_processor_node`
  is ill-typed: it is a `pulp::signal` class, not a `format::Processor`.
* Constructing `ProcessorNode` from a spectral stage is ill-typed: its constructor
  requires `format::Processor&`, while a spectral stage consumes complex frame
  groups rather than `ProcessBlock` buses.
* Retaining a callback frame pointer past callback return is invalid and must be
  rejected by the contract review; no test or implementation may bless it.
* Serializing a graph containing a runtime-only processor node cannot be used as
  a substitute for the missing typed-frame lifetime and scheduling contract.

## Evidence commands

```sh
rg -n "allocation-free|latency_samples|samples_until_next_frame|analyze\(|synthesize_frame\(|available_output" \
  core/signal/include/pulp/signal/spectral_frame_engine.hpp
rg -n "process_frames|borrowed|audio thread|process\(" \
  core/signal/include/pulp/signal/spectral_mask_processor.hpp
rg -n "ProcessorNode|add_processor_node|runtime-only|serialization" \
  core/format/include/pulp/format/processor_node_adapter.hpp \
  core/host/include/pulp/host/signal_graph_runtime.hpp
```

No build gate is required for this document-only design. The next owner action
is to accept or amend the four contract sections above, then specify a public
adapter API and its graph integration tests. Until that decision is recorded,
the G-10 implementation verdict remains **BLOCKED**.
