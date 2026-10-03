# MLX packaging and unified-control gate receipt

Date: 2026-10-02
Work package: D (MLX Apple Silicon provider)
Related landed receipt: `docs/reports/neural-mlx-execution-receipt-20261002.md`
The worker-seam design receipt remains feature-lineage-only; see the provenance
audit in `docs/reports/neural-mlx-provenance-audit-20261003.md`.
Status: design and evidence requirements only; no measurements claimed

## Gate comparison

The existing worker receipt correctly leaves implementation blocked. This addendum
closes the evidence gap against the current planning baseline:

- Phase 3 requires a named model and host, 100,000 paced blocks, stream count,
  callback period, GPU delivery rate, transport misses, fallback, worker CPU,
  shadow CPU, memory, thermal state, contention, and a forced-late completion
  classified as `LateRejected` plus the declared fallback.
- Package D remains blocked on a one-owner-thread, N=2, thermal, and
  hardened-runtime receipt. Presence of an MLX class or a passing synthetic
  kernel does not promote the package.
- Packaging evidence must prove a signed/notarized bundle contains the MLX
  runtime and required `.metallib`, then loads under hardened runtime in a DAW.
  A Magenta example build or unsigned local plugin is insufficient.

The separate execution receipt now records a synthetic Apple Silicon N=2/
100,000-block probe. It is comparison evidence only: it has no named model,
Pulp transport, callback, CPU shadow, thermal/contended-load, or parity result
and cannot be relabeled as the Phase 3 gate.

## Unified controls: required contract and proof

Until a real consumer requires a public enum, MLX stays an optional, default-OFF
internal `StreamingBackend`/worker identity. The existing neural control surface
remains the single selection input:

- `CpuOnly` must select CPU and report no MLX execution.
- `PreferMlx` may select MLX only after worker preparation succeeds; absence,
  incompatibility, or a worker failure must publish CPU with an explicit fallback
  reason.
- `PreferDawn` and `Auto` retain their existing semantics and must not silently
  reinterpret a CPU-only build as MLX-capable.

A prepared snapshot/receipt must expose only provider-neutral facts: stable
`model_id`, artifact hash, requested preference, actual selected provider,
fixed latency/lead, generation, worker/callback timing scope, execution count,
miss/fallback counters, and receipt hash. It must not expose MLX streams, device
handles, checkpoint paths, or arbitrary filesystem paths. Spectr/Forge controls
must use the same validated model ID and safe control schema; hidden state and
provider selection remain read-only telemetry.

Required negative proof: run a CPU-only build with `PreferMlx`, a rejected model
hash, and a forced worker-start failure. Each case must select CPU, record the
reason, produce aligned audio, and show zero MLX executions. A positive proof
may claim MLX only when the receipt's execution count and provider identity are
bound to the prepared model hash.

## Apple Silicon execution runbook

Run on a named Apple Silicon host (chip, OS, Xcode/Metal toolchain, MLX revision,
thermal/power mode) with a fixed model/artifact hash and precision. Capture the
command line, environment, binary hash, and receipt hash.

1. **N=1 baseline:** prepare one instance on its dedicated worker; record owner
   thread ID, load/eval/release thread checks, resident weights, allocator bytes,
   callback period, worker p50/p95/p99, queue/lead, GPU delivery, misses,
   fallback, worker CPU, shadow CPU, and callback maximum.
2. **N=2 cost:** prepare two identical instances in one host process; record
   incremental resident weight/allocator bytes, worker count, service-time and
   deadline/fallback deltas. Repeat with representative contention. This decides
   shared process worker versus one worker per instance; do not infer the answer
   from N=1.
3. **Phase 3 campaign:** run the named workload for 100,000 paced blocks and
   report stream count, callback period, GPU-delivery percentage, transport
   misses, parity failures, fallback blocks, worker/shadow CPU, memory, thermal
   state, and contention. Pass requires exactly 100,000 admitted blocks,
   exactly one disposition for every admitted block, zero transport or parity
   misses, and a separately reported accelerated-provider rate. Never relabel
   provider execution as GPU execution without provider identity and
   timestamps. Also report total process CPU against the CPU-only control for
   the claimed stream count, or name the workload the CPU cannot sustain.
4. **Adversarial late completion:** inject a completion after the prepared lead;
   assert `LateRejected`, declared CPU fallback, no callback wait, and no late
   audio. Also exercise reset/swap generation fencing and a stalled worker.
5. **Magenta RT2 (separate paced lane):** run a two-minute session with a named
   model, record time to first audio, control latency, sustained real-time ratio,
   queue depth, and classified underruns. Do not use the planning reference of
   approximately 200 ms as a measured result.

## Packaging and release proof

On the same Apple Silicon toolchain, and for each shipped format (`.app`, AU,
VST3, CLAP) that embeds MLX:

- prove the exact MLX runtime revision and license/notice inventory are present;
- locate the required `.metallib` inside the final bundle and bind its hash to the
  package receipt;
- sign nested code and the outer bundle with hardened runtime and timestamp;
- run the real launch/load smoke in a DAW or equivalent host, then verify the
  process is using the bundled runtime rather than a developer checkout;
- notarize/staple and run `spctl --assess` on the final artifact;
- repeat the load test after stripping developer paths and with no MLX installed
  globally, proving the bundle is self-contained;
- build the CPU-only configuration on macOS, Linux, and Windows to prove MLX is
  optional and default-off.

The receipt must include bundle paths, code-signing and notarization status,
MLX/metallib hashes, host/load result, and the CPU-only cross-platform result.
No release claim is valid from an unsigned local build.

## Blocker status

The package is still blocked. This checkout has no Pulp-pinned MLX dependency,
worker adapter, public receipt schema, product-valid Apple Silicon campaign, or
signed hardened-runtime MLX bundle. The private harness and synthetic receipt
exist, but only a named-model, transport-backed, machine-readable receipt
should drive a later provider/control or packaging implementation.
