# Matched Dawn-vs-Metal comparator (comparator v2)

`pulp-test-native-metal-audio-comparator` is the measurement behind one
decision: whether to build a native Metal GPU-audio provider. Dawn stays the
default provider. A Metal provider gets built only if this comparator shows,
on a quiet host, that tuned Dawn misses the thresholds below. The comparator
is test-only. It never runs from an audio callback and changes no provider.

## What "matched" means

Both sides run the production shared-I/O convolution program. That program
does log2(N) radix-2 Stockham forward passes, one complex multiply against a
prepared IR spectrum, and log2(N) inverse passes. Each pass is its own compute
encoder, with the same workgroup size and dispatch counts on both sides.

- **Dawn** runs `DawnSharedIoProvider::make_convolution_program` through
  `SharedIoComputePlan`, the production path.
- **Metal** runs an MSL port of the provider's WGSL (`kSharedIoFftWgsl` and
  `kSharedIoMulWgsl`), over no-copy buffers on page-aligned host memory.

Both sides use two channels, a two-slot flight depth, and prebuilt pipelines
and buffers. Inputs and outputs live in page-aligned host memory: imported on
Dawn, no-copy on Metal.

### Robustness: like with like

Dawn always compiles WGSL through Tint, which adds safety code to the kernel:

- it clamps every storage-buffer index to the bound length;
- it guards unsigned `/` and `%` against a zero divisor.

A hand-written MSL kernel has neither, which would quietly favour Metal. The
gating Metal lanes therefore run the MSL with equivalent guards, selected by a
function constant. These guards follow what Tint does; they are not Tint's
literal output.

The same MSL without the guards runs only in the labelled robustness-cost
probe, `metal_shared_event_unguarded_robustness_probe`. That lane never gates.
The verifier reports its difference from `metal_shared_event` as a
**robustness cost** row, a Metal-side estimate of what the guards cost.

Dawn's equivalent row would disable robustness with a device toggle on the
audio device. The provider sets its device toggles internally and offers no
switch for this. Adding one is a WP-T tuning knob, a provider change outside
this measurement-only lane. Until it lands, the Metal-side row is the only
robustness-cost number. Either way it is a tuning input, not a reason to
switch providers.

### Kernel digest

Each cell records a **kernel digest**: the SHA-256 of a canonical
description of the program. The description covers:

- the program and its pass shape;
- the hashes of the provider WGSL behind it;
- the hash of the MSL port;
- the robustness mode.

The canonical form matches Python's `json.dumps(sort_keys=True,
separators=(",", ":"))`, so the verifier recomputes every digest rather than
trusting it.

- **Dawn.** The comparator reads the WGSL out of the provider source at run
  time. CMake also hashes the same WGSL at configure time, so the binary knows
  which WGSL it was built with. If the two differ, the binary exits 1 and the
  verifier refuses the receipt with `provider_wgsl_differs_from_build`.
- **Metal.** The digest uses the WGSL hashes the port was written against, and
  the hash of the MSL actually compiled. On the Dawn side, the digest carries
  the MSL hash pinned in the source as the reviewed port for that WGSL. So
  editing the MSL without updating the pinned hash makes the digests differ.

In short, if the WGSL changes, the MSL changes, or a different kernel runs,
the digests differ and the verifier refuses the receipt.

Two output checks back this up:

- Every lane's output for eight deterministic input patterns must match a
  Dawn production reference lane within the verifier's own tolerance of 1e-4.
- Every block is checked against a double-precision direct-convolution
  oracle.

## Workload and lanes

The comparator runs 64, 128 and 256 frames at 48 kHz. Each cell runs 10,000
blocks after a 512-block warm-up. Blocks are paced at the block period, not
back to back. Cells run in a seeded random order.

There are two load variants:

- **idle**: no other work.
- **render**: a concurrent Skia/Graphite render on Pulp's own UI Dawn device.
  It draws 1280x800 at 60 fps with a full readback per frame, which is ADR
  Measurement C. The receipt records the UI frame times and dropped frames.

| Lane | Side | Role | Notes |
|---|---|---|---|
| `metal_shared_event` | Metal | gating | `MTLSharedEvent` signal, listener wakes a semaphore |
| `metal_completed_handler` | Metal | gating | `addCompletedHandler` wakes a semaphore |
| `metal_shared_event_unguarded_robustness_probe` | Metal | robustness probe | no Tint-equivalent guards; never gates |
| `dawn_imported_timed_wait_any` | Dawn | gating | production storage; event-driven wait (below) |
| `dawn_imported_process_events_poll50us` | Dawn | gating | production storage, `ProcessEvents` plus the transport's 50 µs poll |
| `dawn_imported_wait_any_poll50us` | Dawn | gating | production storage, non-blocking `WaitAny` plus a 50 µs poll |
| `dawn_staged_timestamps_timed_wait_any` | Dawn | diagnostic | GPU timestamps on; not production-eligible |

**Dawn's event-driven wait.** In `dawn_imported_timed_wait_any`, the worker
blocks in `TimedWaitAny` on the queue futures, the way the provider's
dispatcher does. Sometimes no queue future is pending: the queue work is done
but scope or map callbacks are still due. Then `WaitAny` cannot block, and the
worker falls back to the transport's 50 µs poll quantum rather than spinning.
The receipt records this mode as `wait_mode`, along with the number of
fallback sleeps. The CPU it measures is therefore the CPU of the wait the
provider actually performs, with no extra spinning and nothing hidden.

**Dawn timestamps.** The provider refuses timestamps on imported storage
(`timestamps_require_staged_storage`), so Dawn's GPU busy time comes from the
staged diagnostic lane.

**Dawn wake latency.** The provider exposes no host-domain completion time, so
Dawn wake latency cannot be measured. The staged lane's submit→visible minus
GPU busy is reported only as a non-production proxy.

**WP-T switches.** The Dawn tuning switches planned in WP-T are not on this
base. The receipt records `wp_t_switches`, and each switch becomes a lane once
it lands.

## Metrics per cell

The comparator records raw per-block samples. The verifier computes every
statistic from them, and refuses a cell whose timeline is impossible. A valid
timeline satisfies `release ≤ submit ≤ submit_end ≤ visible`, has the GPU end
between submit and visible (within a 50 µs clock-conversion margin), and has
GPU busy time no longer than the latency.

- **Submit→visible**: p50, p99, p99.9 and max, on the host clock.
- **GPU busy time**: Metal measures command-buffer start to end; Dawn measures
  first pass to last pass, from timestamp queries.
- **CPU per block**, which the gate uses: process CPU over the cell's driven
  window, minus the render thread and the oracle check, divided by the blocks
  driven.
  - This counts completion work wherever it runs: Dawn's callbacks on the
    worker, and Metal's on its own dispatch threads.
  - The cell also reports worker-thread CPU and the CPU spent inside each
    Metal completion block. Worker CPU alone omits Metal's completion work and
    never decides the gate.
  - In render cells, Dawn/Metal driver threads serving the UI device also land
    in process CPU. That noise affects both sides alike.
- **Wake latency**: measured on Metal only, from GPU end to the moment the
  worker observes it.
- **Misses**: a block misses when it is not visible within a two-block lead
  of its release, or when it fails the oracle.

## Running a campaign

```bash
python3 tools/scripts/gpu_audio_comparator_v2.py run \
  --binary build/test/pulp-test-native-metal-audio-comparator \
  --out /tmp/comparator-v2/campaign-1.json
python3 tools/scripts/gpu_audio_comparator_v2.py verify \
  --binary build/test/pulp-test-native-metal-audio-comparator \
  /tmp/comparator-v2/campaign-1.json /tmp/comparator-v2/campaign-2.json
```

`run` takes the P2 host preflight
([gpu-audio-p2-host-preflight.md](gpu-audio-p2-host-preflight.md)) before and
after the binary runs. It then binds these to a `pulp.gpu-audio.comparator.v2`
receipt:

- both preflights;
- the source revision and the clean or dirty state of the tree;
- the binary digest;
- a receipt digest.

The receipt digest is an integrity check, not authentication: anyone who can
edit a receipt can recompute it. `verify --binary` refuses receipts that the
given binary did not produce.

A receipt counts only when all of the following hold:

- Both preflights passed, at the measured revision, and the tree was clean.
- Every cell has 10,000 blocks, and no cell's worker started blocks more than
  a period late (`worker_starved`).
- Every default lane ran every block size under both loads.
- Dawn GPU timestamps were available.

Anything else is evaluated and printed, but marked `INCONCLUSIVE`. A noisy
agent host can therefore still produce a dry-run receipt. Lowering
`--min-blocks` is for exploration only and can never produce a gate verdict.

Git provenance failures are fail-closed: a missing or invalid `HEAD`, a failed
Git status query, or any dirty/untracked source tree produces a `REFUSED`
verdict (`source_revision_invalid`, `source_git_error`, or
`source_tree_dirty`) rather than an inconclusive campaign. Receipts must carry
explicit boolean `false` values for both `source_git_error` and `source_dirty`;
missing or non-boolean provenance fields are refused as malformed provenance
(`source_git_error_invalid` or `source_dirty_invalid`). Empty or malformed
source revisions are refused as `source_revision_invalid`.

The ctest runs `gpu_audio_comparator_v2.py smoke`, which makes two runs:

- A short matched run. It must not be refused, and it must be marked
  undersampled.
- A planted run where the Metal lane uses the old unmatched `complex_multiply`
  kernel. The verifier must refuse it with `kernel_digest_mismatch`.

## Gate thresholds (PROPOSED, not ratified)

The thresholds are evaluated at 64 frames (a 1.333 ms period). The verifier
needs two counted campaigns that are distinct and time-ordered, and that come
from the same binary, source revision, host and Dawn revision. Each criterion
compares the best production-eligible gating Dawn lane against the best
gating Metal lane, under each load. The gate fires if any criterion fires in
both campaigns:

1. Dawn p99.9 submit→visible is above 50% of the period, while Metal is
   below 25%.
2. Dawn CPU per block is above 2x Metal's and above 10% of the period, using
   the process-CPU measure above.
3. Dawn wake p99 is above 300 µs, while Metal is below 100 µs. Dawn's wake
   time is unobservable, so this criterion stays `not_evaluable` unless
   Metal's own wake p99 is at least 100 µs, in which case it cannot fire.
   The staged-lane proxy is reported but never decides it. As a result,
   `GATE_DOES_NOT_FIRE` cannot be reached until the provider exposes a
   completion time (WP-T).
4. Under the render load, the Dawn miss rate against a two-block lead is
   above 0.01%, while Metal's is 0.

The verifier reports one of these verdicts:

- `GATE_FIRES`
- `GATE_DOES_NOT_FIRE`
- `INCONCLUSIVE`
- `REFUSED`: an unmatched kernel, a parity or oracle failure, an impossible
  timeline, a tampered or duplicated receipt, or a source tree that differs
  from the build.

Until the owner ratifies the thresholds, every verdict is advisory.
