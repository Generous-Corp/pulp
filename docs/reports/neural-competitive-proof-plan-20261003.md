# Neural real-time competitive proof plan (2026-10-03)

This document defines the evidence required before Pulp claims that a neural
audio path outperforms an audited real-time engine or service. It is a proof
plan, not a performance claim. A fast synthetic fixture, a GPU execution
counter, or a model that merely produces plausible audio is insufficient.

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
* **MLX:** work is owned by a prepared worker thread. The receipt must prove
  MLX execution, CPU shadow parity, unified-memory residency, stream/queue
  behavior, and the cost of a second instance. A CPU result from an MLX-
  requested configuration is a classified fallback, never a GPU win.
* **Dawn:** GPU work must show provider execution and useful block/batch
  parallelism. Moving serial sample-by-sample work to Dawn is not a win. The
  receipt must include ordered delivery, fixed latency/lead, slot retirement,
  device-loss recovery, and the production node's actual in-flight depth.

## Promotion rules

A Pulp path may claim **outperforms** only when all of the following hold:

1. matched-model output passes the declared quality and state-parity gates;
2. p95 and p99 deadline margin improve, with zero unexplained deadline misses
   in 100,000 blocks;
3. no allocation, lock, blocking, late-result, or fallback counter is hidden
   by aggregation;
4. the result is reproduced on two runs on the named host and the receipt is
   independently replayable from its artifacts; and
5. the comparison names the scope of the win (for example, one model, block
   size, provider, and instance count), rather than generalizing to all audio.

If quality is equal but latency is worse, or latency is better but quality is
outside tolerance, the result is **not a win**. If the competitor cannot be
run with matched state or weights, publish a capability comparison and leave
the performance claim open.

## Smallest decisive campaign

The first campaign should use the in-tree micro TCN and the CPU NAM/TCN oracle,
then the same block-parallel model through MLX and Dawn where available. It
should produce one machine-readable receipt plus a Perfetto trace for one
32-frame and one 128-frame case, then repeat the winning case at two instances.
This is enough to decide whether acceleration helps the persistent node before
investing in larger SSM, Magenta, or generative models.

