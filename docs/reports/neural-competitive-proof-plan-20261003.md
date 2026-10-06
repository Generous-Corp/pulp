# Neural real-time competitive proof plan (2026-10-06 protected-head successor)

This document defines the evidence required before Pulp claims that a neural
audio path outperforms an audited real-time engine or service. It is a proof
plan, not a performance claim. A fast synthetic fixture, a GPU execution
counter, or a model that merely produces plausible audio is insufficient.

## Current source boundary

The source snapshot for this plan is the fetched protected `origin/main` at
[`ba1a212f481cadb659fccf58d41616b1b25be360`](https://github.com/Generous-Corp/pulp/commit/ba1a212f481cadb659fccf58d41616b1b25be360). It includes the private CPU
neural lifecycle facade, manifest/installed-asset admission, serialized NAM/TCN
CPU bridge, the tools-only `tools/validation/mlx_worker_harness.py` probe,
the Apple host-tier planning matrix, the minimum-lead GPU proxy (PR9582),
transactional NAM/TCN lifecycle (PR9622), hardened MLX named-model harness
(PR9624), Apple Silicon serviceability record (PR9625), and the later
CI/pluginval, host-scheduling, GPU-batching, and Windows portability merges.
The MLX probe is
default-off synthetic scheduling evidence; it is not wired into a plugin build
and is not a shipped product/provider. The processor facade still executes CPU
and records non-CPU requests as fallback or unavailable, so every MLX, Dawn,
competitive, and Phase 3 gate below remains open until its declared receipt
exists.

## Comparison contract

Every comparison must use the same serialized weights, tensor precision,
sample rate, channel count, host block size, warm-up policy, reset/epoch
semantics, and output tolerance. The reference and Pulp runs must receive the
same input corpus and control events. If an engine cannot implement the same
causal state contract, it is reported as incomparable rather than losing by
definition.

The receipt records the exact commit, model digest, compiler/toolchain, host
chip and OS, provider identity, power/thermal state, input corpus digest, and
benchmark harness digest. It also records whether the result is callback,
paced producer, or offline; these lanes are never mixed in one ranking.

## Required measurements

For each lane, run at least 100,000 paced blocks after warm-up and report
p50/p95/p99/max processing time, real-time ratio against the block deadline,
deadline misses, queue depth and lead, late-result count, fallback count,
device-loss/reprepare count, allocations, locks, blocking calls, and audio
output checksum/error against an independent CPU oracle. Report cold-start and
steady-state separately. Repeat at 32, 64, and 128 frames, 44.1/48/96 kHz,
1/2/4 channels, and one and two instances. A claim must include resident
weights, peak working memory, and the incremental cost of instance two.

The Pulp harness uses the existing audio test harness for paced input and
xrun accounting, the audio-quality lab for output metrics, the proxy-first
evaluation workflow for matched third-party runs, and optional Perfetto
captures for causal attribution. Perfetto is diagnostic evidence: a trace can
explain a miss but cannot replace the receipt counters or prove GPU work.

The benchmark reports two timing domains. **Inference time** measures the
provider kernel or worker service in isolation. **Delivered-block time** starts
when a host block is admitted and ends when the correctly ordered output is
available to the audio node, including queueing, copies, synchronization,
fallback, and slot retirement. A GPU result is useful only if delivered-block
deadline margin improves; a faster kernel with a slower delivery path is a
regression. Every row also reports CPU time and peak resident/working bytes.

Each comparison has a paired baseline and candidate run in alternating order,
five warm-up blocks, and at least five independent cold starts plus five
steady-state runs of the 100,000-block campaign. Each run is segmented into
fixed windows so p50/p95/p99, median, IQR, and coefficient of variation are
available per window and per run. Report a 10,000-sample paired bootstrap 95%
confidence interval for each candidate-minus-baseline percentile delta. A
coefficient of variation above 5% is inconclusive unless a follow-up explains
and removes the source of variance. Treat a difference smaller than the
instrument's measured timer-noise floor as indistinguishable. A single run, an
unpaired host, or a wall-clock total without per-block samples is insufficient
evidence.

For the initial 48 kHz/64-frame callback cell (1.333 ms deadline), the proposed
practical-effect floor is at least a 10% paired reduction in both p95 and p99
delivered-block time and at least 0.10 ms more deadline margin, with no matrix
cell regressing by more than 5%. The bootstrap interval must exclude zero and
the practical floor. These thresholds are deliberately explicit proposals to
ratify before the first competitive run; a weaker result is reported as
inconclusive or a capability improvement, never as “outperforms”.

The harness carries a planted-defect control set in every campaign: (1) a
deliberately slower reference configuration that must be measurably slower, (2)
one injected allocation, (3) one injected lock/block event, (4) a stale or
wrong-sequence completion, (5) a dropped completion that forces fallback, (6)
device loss followed by reprepare, (7) a generation/epoch mismatch, and (8) a
single-sample corruption. The deadline overrun must increment the miss counter
and fail the deadline gate. Each defect must be detected exactly once, produce
the declared counter/retirement event, and leave no contaminated output; the
campaign must have zero false acceptance. Unix builds may use allocator and
lock traps, while every platform must also expose explicit allocation, lock,
blocking, retirement, fallback, and checksum probes so portability does not
depend on interposition. If any control is silent, the instrument is invalid
and no performance verdict is issued.

Every receipt also records the exact engine and model-conversion commit,
compiler and optimization flags, thread-pool size, SIMD/quantization mode,
worker affinity, warm-up and power-state policy, serialized model/weights
digest, state-byte count, receptive field, and precision. This metadata is
mandatory for every provider row and makes a later replay or regression audit
possible.

Quality gates are model-class appropriate. Amp/effect models require bounded
sample error and spectral distance against the oracle; denoisers and
separators additionally report SI-SDR or the task metric on a fixed corpus;
generators report time-to-first-audio, control latency, interruption behavior,
and human/listening-lab scores. A performance win is valid only when the
quality metric is within the predeclared tolerance.

## Provider lanes

* **CPU:** the allocation-free callback path is the portability oracle. It must
  remain buildable on every Pulp CI OS and is the fallback for every optional
  accelerator.
* **MLX:** the landed worker probe is only a synthetic thread-ownership and
  scheduling instrument. A product lane still requires work owned by a prepared
  worker thread plus proof of MLX execution, CPU shadow parity, unified-memory
  residency, stream/queue behavior, and the cost of a second instance. A CPU
  result from an MLX-requested configuration is a classified fallback, never a
  GPU win.
* **Dawn:** GPU work must show provider execution and useful block/batch
  parallelism. Moving serial sample-by-sample work to Dawn is not a win. The
  receipt must include ordered delivery, fixed latency/lead, slot retirement,
  device-loss recovery, and the production node's actual in-flight depth.

## Model coverage ladder

## Apple Silicon service matrix

M1, M3, and M5 are valid experiment hosts, but a control-path measurement is
not a neural-provider support claim. The historical source anchor below is retained for provenance; current protected source is [`ba1a212f481cadb659fccf58d41616b1b25be360`](https://github.com/Generous-Corp/pulp/commit/ba1a212f481cadb659fccf58d41616b1b25be360). The historical source anchor is
[`b038559ab2d019566c4964d42c0786310c139860`](https://github.com/Generous-Corp/pulp/commit/b038559ab2d019566c4964d42c0786310c139860),
which includes the named-model MLX, host-evidence, GPU lifecycle, and provenance
merges ([PR 9577](https://github.com/Generous-Corp/pulp/pull/9577), [PR 9602](https://github.com/Generous-Corp/pulp/pull/9602), [PR 9525](https://github.com/Generous-Corp/pulp/pull/9525), and [PR 9605](https://github.com/Generous-Corp/pulp/pull/9605)). Existing native-control observations
reported GPU p50 values of 10.250 us on M1 Max, 6.792 us on M3 Ultra, and
7.292 us on M5 Max. Those runs had observer and allocation confounders, so
they prove cross-generation availability only. Each provider and model still
needs its own receipt:

| Host tier | Permitted use before a real provider receipt | Promotion evidence required |
| --- | --- | --- |
| M1 Max | CPU/reference work and Dawn feasibility experiments | host-bound named-model receipt with provider/executable identity, CPU shadow parity and quality, 100,000 paced blocks, the full block/rate/channel/instance matrix, zero unexplained fallback, p95/p99 deadline margin, model/license/package hashes, and reset/device-loss evidence before any host claim |
| M3 Ultra | block-parallel MLX/Dawn experiments and multi-instance profiling | the same host-bound receipt plus measured accelerator parallelism, instance-scaling evidence, reset/device-loss recovery, and proof that GPU work is useful at production block sizes |
| M5 Max/Studio | primary sustained GPU-audio campaign candidate | the same host-bound receipt at production block sizes, with independent-process cold starts, signed package identity, serialized model/license hashes, and no cross-host promotion |

Results remain host-specific. A pass on M5 does not promote M1 or M3, and a
control that runs on all three does not prove that a persistent audio node
benefits from GPU execution.

The first receipt is deliberately small, but the program must expand coverage
before claiming a general engine advantage:

1. **Callback effects:** the in-tree micro TCN is feasibility/profiling-only;
   competitive evidence starts with one real serialized causal NAM/TCN artifact
   (including WaveNet A1/A1-Standard where available), with CPU-oracle parity
   and mono/stereo state-reset replay.
2. **Stateful recurrence:** one LSTM or GRU artifact with identical hidden-state
   initialization and reset, measured CPU-first. This proves whether serial
   recurrence is a useful persistent node and whether acceleration merely moves
   serial work to a worker.
3. **Long-context/block-parallel:** one compact SSM and one block-parallel TCN,
   where the benchmark must show useful parallel work before MLX or Dawn is
   promoted.
4. **Paced generation:** a named Magenta RT2 workload measured as a producer
   with time-to-first-audio, control latency, cancellation, queue lead, and
   underruns. It is not ranked against callback DSP.
5. **Offline/near-real-time generation:** diffusion or flow models are measured
   as cancellable background renders with quality and throughput receipts, not
   advertised as callback-capable until a separate causal model exists.

Transformers, selective SSMs, and other large-context models enter this ladder
only with a named workload and an execution plan that exposes block or batch
parallelism. Model names alone do not establish that a GPU is worthwhile.

## Promotion rules

A Pulp path may claim **outperforms** only when all of the following hold:

1. matched-model output passes the declared quality and state-parity gates;
2. p95 and p99 deadline margin meet the ratified practical-effect floor, with
   zero unexplained deadline misses in 100,000 blocks;
3. no allocation, lock, blocking, late-result, or fallback counter is hidden
   by aggregation;
4. the result is reproduced across the required cold and steady runs on the
   named host, passes the variance and bootstrap rules, and the receipt is
   independently replayable from its artifacts; and
5. the comparison names the scope of the win (for example, one model, block
   size, provider, and instance count), rather than generalizing to all audio.

If quality is equal but latency is worse, or latency is better but quality is
outside tolerance, the result is **not a win**. If the competitor cannot be
run with matched state or weights, publish a capability comparison and leave
the performance claim open.

## Smallest decisive campaign

The first campaign should use the in-tree micro TCN only as a feasibility
instrument, then a matched serialized CPU NAM/TCN artifact as the first
competitive workload. The landed MLX worker probe may be used to validate
thread/watchdog instrumentation, but it cannot substitute for the named-model
campaign. Run the same block-parallel model through MLX and Dawn where
available. Produce one machine-readable receipt plus a Perfetto trace for one
32-frame and one 128-frame case, then repeat the winning case at two instances.
This decides whether acceleration helps the persistent node before investing in
larger SSM, Magenta, or generative models; it cannot establish a general engine
ranking until the real-model ladder is complete.

The campaign's output is a versioned JSON receipt plus raw timing samples,
audio-quality report, and (when tracing is enabled) a Perfetto trace. The
receipt names the proxy, data source, detection floor, sample size, controls,
and verdict. It may say **better for this matched workload**; it must not say
“faster neural audio” or “outperforms everyone” until the model ladder and
matched-quality rules above have been satisfied.
