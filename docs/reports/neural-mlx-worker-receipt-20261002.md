# MLX Apple Silicon worker seam receipt

Date: 2026-10-02
Work package: D (MLX Apple Silicon provider)
Branch: `codex/dsp-next-20261001`

## Decision

This package records a design and measurement receipt. No MLX implementation is
safe in this checkout yet. The current neural seam is intentionally CPU-only:
`core/gpu_audio/src/detail/neural_processor.hpp` accepts a provider preference and
capability mask, but `prepare()` refuses a non-CPU capability and publishes only
`NeuralProvider::Cpu`. There is no MLX dependency, worker interface, provider
identity in the public SDK, or production second-instance behavior to justify
changing that contract. This receipt does not edit the planning submodule,
CPU model headers, or dependency manifests.

The source paths named below are feature-lineage references. They are absent
from the audited `origin/main` ref as of the provenance audit and therefore do
not constitute a landed public implementation.

The later private synthetic probe and its limitations are recorded in
`neural-mlx-execution-receipt-20261002.md`; that evidence does not change this
CPU-only decision.

## What exists today

- `NeuralProvider::{Cpu,Mlx,Dawn}` and `NeuralProviderCapabilities` are private
  control-plane vocabulary. `PreferMlx` is currently an honest request that
  records `fell_back_to_cpu`; it never claims MLX execution.
- `NeuralProcessor::prepare()` and `publish()` are control-thread operations.
  `process_cpu()` is a bounded callback read and CPU model call. No callback
  path may inspect provider availability, allocate, load a model, or call MLX.
- `NeuralModelManifest` validates immutable identity, SHA-256, license,
  redistribution permission, and retained-state schema. `ModelStore` remains the
  asset/provenance owner; it does not select an inference backend.
- Existing GPU transport/status contracts already separate selected engine,
  misses, fallback, resync, and dropped input. A future MLX adapter must reuse
  those accounting rules or provide an equivalent provider-neutral receipt.

## MLX facts and required measurements

The reviewed Magenta/MLX example and Pulp research record MLX lazy evaluation,
blocking `eval()`, thread-local streams, and thread-local Metal command
encoders. Therefore one prepared runtime must have exactly one owning worker
thread for model load, array creation, evaluation, and hot reload. Cross-thread
array use has already failed with `There is no Stream(gpu, N) in current thread`.
The audio callback must never touch MLX objects.

Before implementation, run a real Apple Silicon harness with a fixed model hash,
precision, sample rate, block size, and thermal state. Record at least:

| Axis | Required receipt |
|---|---|
| Thread ownership | owner thread id; proof that load, `eval()`, and release stay on it; callback has zero MLX calls/allocations/waits |
| Pacing | callback period, worker service time p50/p95/p99, queue depth, lead/lookahead, underruns/misses, and delivery timestamps |
| Second instance | N=1 and N=2 in one host process; incremental resident weight/allocator bytes, worker count, service time, and deadline/fallback rate |
| CPU fallback | continuously primed CPU shadow cost, fallback blocks, miss disposition, output alignment, reset/swap behavior, and max callback time |
| Provider truth | requested preference, selected provider, actual provider execution count, fallback reason, model/artifact hash, generation, and receipt hash |

The first implementation gate is a matched CPU oracle and an MLX worker for one
block-parallel TCN or compact SSM. It must pass tolerance-based output parity,
reset determinism, and block-size matrix tests before any public provider value
or SDK target is added. N=2 must be measured before deciding between one
process-wide worker with shared immutable weights and one worker per instance.

## Unified controls and client observability

The unified control remains the existing preference/capability path:
`CpuOnly`, `PreferMlx`, `PreferDawn`, or `Auto`, plus a capability report derived
from the prepared snapshot. A future MLX adapter should publish a pending
snapshot only after worker preparation succeeds; selection is then visible as
`selected_provider=Mlx`, while an unavailable/failed worker publishes
`selected_provider=Cpu` with `fell_back_to_cpu=true` and a stable reason code.
No client should receive a provider handle or checkpoint path.

A plugin/SDK client observes the result through the provider-neutral status and
receipt surfaces: stable `model_id` and artifact hash, requested preference,
actual selected provider, fixed latency/lead, callback and worker latency
percentiles, miss/fallback counts, reset/swap generation, and receipt hash.
The client can therefore distinguish “MLX requested” from “MLX actually ran”.
The receipt must state whether timing is callback-only or includes provider/device
timestamps; initialization time and a claimed speedup that omits CPU shadow work
are not performance evidence.

For Magenta RT2, this is a paced producer/ring contract rather than callback DSP:
lookahead, time-to-first-audio, sustained real-time ratio, queue depth, and
underrun policy are reported separately. The existing example's roughly 200 ms
generative latency is a planning datum, not a Pulp acceptance result.

## Adversarial risks and tests

- **False capability:** request `PreferMlx` with MLX absent or worker failure;
  assert CPU selection, fallback reason, and no MLX execution count.
- **Thread violation:** create/evaluate/release arrays from a foreign thread;
  test fails unless the adapter rejects it before publication.
- **Late worker:** delay `eval()` beyond the lead; assert one terminal
  disposition per block, aligned continuously primed CPU output, and no callback
  wait.
- **Second-instance amplification:** prepare two identical instances; assert
  measured weight/allocator delta and that deadline/fallback rates are recorded,
  not inferred from N=1.
- **Stale delivery:** reset or swap while work is in flight; reject old
  generation outputs and retain deterministic CPU behavior.
- **Receipt spoofing:** mismatch model/artifact hash or provider execution count;
  SDK validation must refuse the receipt.
- **Packaging:** a signed Apple bundle must contain the MLX runtime and required
  metallib; a CPU-only build must remain cross-platform and default-on.

## Validation performed

- Inspected `neural_processor.hpp`, `neural_model_manifest.hpp`, and
  `test/test_neural_model_manifest.cpp`.
- Inspected the MLX/Magenta execution findings in
  `planning/research/2026-10-01-neural-audio-mlx-first-program-plan.md` and
  existing GPU-NAM paced receipts. Those records establish the worker/fallback
  requirements; the later synthetic N=2/100,000-block probe is documented in
  `neural-mlx-execution-receipt-20261002.md` and is explicitly not a product
  gate.
- Existing manifest tests are the applicable focused test surface; no source or
  manifest change was made, so no build was required for this receipt.

## Blockers

1. No pinned/optional MLX dependency or worker adapter exists in this checkout.
2. Public provider identity and receipt schema are intentionally undecided.
3. No product-valid Apple Silicon N=2 measurement exists for weight residency,
   allocator growth, Pulp pacing, or fallback cost. The synthetic probe has no
   model, transport, CPU shadow, or allocator-residency evidence.
4. Magenta RT2 example receipts do not yet report sustained pacing/underruns.
5. Signed/notarized bundle handling for MLX metallib is unproven.

Next safe step: adapt the private, default-OFF harness to a named model and the
existing Pulp paced transport, collect the missing CPU-shadow, fallback,
thermal, contention, and package evidence, then review that receipt before
changing capability publication or SDK surfaces.
