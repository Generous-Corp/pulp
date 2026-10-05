# Private recurrent CPU-family implementation packet

Date: 2026-10-05
Worktree: `/Users/danielraffel/Code/pulp-neural-cpu-family-20261004`
Branch: `codex/neural-cpu-family-packet-20261004`
Source baseline: `origin/main` at `9cc65128d0680b55ffeeafc351e2ce59b73c85cb`
Scope: bounded private CPU fallback mechanics for one causal LSTM/GRU family; no
public ABI, provider capability, product model, GPU, MLX, or Dawn claim.

## Disposition

**GO, narrowly:** implementation may start for a private, default-off recurrent
adapter contract and synthetic lifecycle tests. **NO-GO:** do not wire it into a
shipped processor, advertise LSTM/GRU model support, or claim a named model until
a licensed serialized fixture, an independent CPU oracle, and the gates below
have receipts. No PR should be opened from this lane without coordinator review.

The baseline has a suitable private seam but no recurrent implementation. The
existing LSTM parity statement in
`docs/status/neural-nam-cpu-oracle-parity-receipt-20261002.md` is external
receipt evidence, not a Pulp loader or product capability.

## Existing seams and boundaries

- `core/gpu_audio/src/detail/streaming_model.hpp` owns the private
  `StreamingModel` contract. `prepare()` is control-only; `process_cpu()` is
  `AudioCallbackSafeAfterPrepare`; `quiesce()`, `reset()`, and `release()` are
  host/quiescent lifecycle calls. `StreamingModelSpec` already carries immutable
  identity, sample/block shape, state bytes, schema, and determinism.
- `core/gpu_audio/src/detail/neural_processor.hpp` performs prepare into a
  pending snapshot and exposes it only through `publish()`. Its reset path first
  quiesces, advances the generation, then resets the model. Its CPU lane is the
  only executable provider today; non-CPU preferences are recorded as CPU
  fallback, not as provider execution.
- `core/gpu_audio/src/detail/nam_tcn_adapter.hpp` is the smallest existing
  callback-table adapter. `nam_tcn_artifact.hpp` shows that parsing, weight
  layout, and state allocation belong to preparation while callback processing
  only advances fixed state.
- `test/harness/scoped_rt_process_probe.hpp` and
  `test/harness/rt_contract_probe.hpp` provide allocation/lock/blocking/stale/
  alias instrumentation. `test/test_streaming_model_contract.cpp` is the
  lifecycle and negative-control home.
- `core/gpu_audio/src/detail/neural_model_manifest.hpp` can persist state schema
  and byte count, but it does not define recurrent tensor layout or authorize a
  model. Keep those private until a reviewed artifact contract exists.

No public header, `NeuralProvider` enum, capability projection, or generic
`ModelStore` API should change in this packet.

## Smallest adapter

Add one private header, proposed as
`core/gpu_audio/src/detail/recurrent_cpu_adapter.hpp`, and a test-only fixture
implementation. Keep it model-format-neutral and parallel to
`NamTcnStreamingAdapter`:

```cpp
enum class RecurrentFamily : uint8_t { Lstm, Gru };

struct RecurrentCpuShape {
    RecurrentFamily family;
    uint32_t input_size;       // first packet: 1
    uint32_t hidden_size;      // bounded, nonzero
    uint32_t layers;           // first packet: 1
    uint32_t directions;       // first packet: 1; bidirectional is not causal
    uint32_t output_size;      // first packet: 1
};

struct RecurrentCpuKernel {
    void* state;
    bool (*prepare)(void*, const StreamingPrepareContext&) noexcept;
    void (*process)(void*, const float*, float*, uint32_t) noexcept;
    void (*reset)(void*) noexcept;
    bool (*quiesce)(void*) noexcept;
    bool (*release)(void*) noexcept;
};
```

`RecurrentCpuAdapter` owns the immutable `StreamingModelSpec`, validates the
shape and context, delegates control-thread preparation once, and delegates
only fixed-size audio blocks after preparation. It rejects non-causal shapes,
zero/oversized dimensions, channel mismatches, frames above `max_frames`, and
invalid buffers by clearing the output without touching recurrent state. The
callback path must contain no filesystem access, model parsing, allocation,
lock, wait, provider selection, or dynamic dispatch that can allocate.

The first fixture kernel should use one layer, one direction, one input and one
output. Its state and scratch are allocated/reserved during `prepare()` and
never resized in `process_cpu()`:

| family | persistent state | per-step gate scratch | state schema |
| --- | --- | --- | --- |
| LSTM | `h[hidden] + c[hidden]` | `4 * hidden` floats | `recurrent-lstm-v1` |
| GRU | `h[hidden]` | `3 * hidden` floats | `recurrent-gru-v1` |

For the first packet, `state_bytes` is the persistent state plus fixed scratch
bytes, with checked multiplication and an explicit admission ceiling. If a
future implementation uses layer/direction banks, the formula must be
`layers * directions * (family_state_floats * hidden + gate_floats * hidden)`;
the first packet must reject every value other than one layer and one direction.
Weights are immutable prepared data and are not counted as mutable causal state;
the manifest must record their byte count separately when a real artifact is
admitted.

A kernel reset must zero every hidden/cell element and scratch value and restore
its cursor/epoch-independent state. `StreamingBlockStamp` is telemetry and
ordering metadata; it must never implicitly reset a recurrent state.

## Lifecycle and reprepare gates

1. **Admission:** `StreamingModelSpec` identity, shape, sample rate, block size,
   state schema/version, and checked state bytes validate before any model is
   exposed. Unsupported family/shape fails closed.
2. **Prepare transaction:** parsing/weight validation and all allocations happen
   before `NeuralProcessor::publish()`. A failed prepare must release partial
   state, be retryable, and leave no pending or active snapshot.
3. **Publish:** only a fully prepared adapter is published. The callback sees a
   stable model pointer and immutable spec; no mutable registry lookup occurs.
4. **Reset:** owner stops admission and calls `quiesce()`; then
   `NeuralProcessor::reset()` advances generation and invokes adapter reset.
   Reset replay must match fresh preparation for the same weights and input.
5. **Reprepare:** sample-rate, block-size, device-loss, or model changes use a
   quiescent release/reprepare transaction. Old and new state must never execute
   concurrently. A failed replacement keeps the old active snapshot only if the
   coordinator explicitly chooses that transaction policy; the first packet
   should fail closed and retain no half-replaced state.
6. **Release:** idempotent and retryable after a failed release. Destruction or
   state reuse is forbidden until `release()` succeeds.

## RT and CPU-oracle gates

The implementation gate is closed until all of these are green for both family
fixtures (synthetic kernels first):

- Direct `process_cpu()` under `ScopedRtProcessProbe`: zero allocations for
  32/64/128-frame blocks, including reset replay. Add planted allocation, lock,
  and blocking controls through `RtContractProbe`; each control must be detected
  exactly once. Keep the noinline planted-allocation helper so optimization
  cannot erase the negative control.
- Output-buffer shape, alias, and oversized-frame rejection clears output and
  leaves hidden/cell state unchanged. The adapter must not write past the
  declared frame count.
- CPU oracle uses the same weights, float precision, sample rate, input corpus,
  initial zero state, block partitions, and reset epoch. Compare complete output
  vectors, not a few samples; require a predeclared residual (initial proposal
  `max_abs_error <= 1e-6`) and a nonzero `parity_failures` counter on a planted
  one-sample corruption.
- Matrix: 32, 64, and 128 frames; 44.1, 48, and 96 kHz metadata; mono first;
  one and two prepared instances; five reset/replay repetitions. Sample rate is
  `host_sample_rate_metadata` unless the recurrent computation was actually
  measured at that rate.
- Lifecycle: prepare failure cleanup/retry, publish atomicity, reset generation
  increment, release idempotence, and reprepare after a changed shape or rate.
- A real artifact gate additionally requires immutable fixture path, SHA-256,
  architecture/family, license/redistribution evidence, tensor layout, state
  schema, and an independent oracle receipt. Until then all results are
  synthetic contract evidence and must remain labelled that way.

Suggested future focused test names are exact discovered names to be established
when code lands, for example `recurrent CPU adapter LSTM callback safety`,
`recurrent CPU adapter GRU reset replay`, and `recurrent CPU adapter rejects
bidirectional shape`. Do not cite these names as existing tests yet.

## Smallest implementation packet

1. Add the private shape/kernel/adapter header with checked bounds and no public
   include exposure.
2. Add one deterministic synthetic LSTM fixture and one GRU fixture in the
   existing private GPU-audio test group; no external model files yet.
3. Add lifecycle, reset/reprepare, RT-probe, invalid-shape, alias, and CPU-oracle
   tests. Regenerate any prescribed test-input manifest rather than editing it.
4. Run the exact discovered CTest names plus the direct private test binary and
   `git diff --check`. A full product/provider build is not implied by this
   contract-only packet.
5. Produce a receipt stating baseline SHA, adapter/test paths, matrix,
   `max_abs_error`, allocation/lock/blocking counters, and explicit
   `model_support=false` / `provider_execution=false` fields.

## Decision and blockers

Safe implementation can start now only at step 1–3 above. The following remain
blocking for model or product claims: a reviewed serialized LSTM/GRU format and
licensed fixture; independent oracle vectors and exact reset/prewarm semantics;
state/weight hash persistence; a product host integration; and cross-platform
CPU receipts. The existing synthetic CPU seam and external parity note do not
close those gates. Coordinator review is required before any branch publication
or PR.
