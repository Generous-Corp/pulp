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

Each cell records a **kernel digest**: the SHA-256 of a canonical description
of the program and its pass shape, plus the SHA-256 of the WGSL behind it.

- On the Dawn side, the comparator reads the WGSL out of the provider source
  at run time.
- On the Metal side, the digest uses the WGSL hash that the port was written
  against.

If the provider WGSL changes and the port does not, the digests differ. The
same happens if a different kernel runs on either side. In both cases the
verifier refuses the receipt. A second check compares outputs across sides:
every lane's output for eight deterministic input patterns must match a Dawn
reference lane within 1e-4. Every block is also checked against a
double-precision direct-convolution oracle.

## Workload and lanes

The comparator runs 64, 128 and 256 frames at 48 kHz. Each cell runs 10,000
blocks after a 512-block warm-up. Blocks are paced at the block period, not
back to back. Cells run in a seeded random order.

There are two load variants:

- **idle**: no other work.
- **render**: a concurrent Skia/Graphite render on Pulp's own UI Dawn device.
  It draws 1280x800 at 60 fps with a full readback per frame, which is ADR
  Measurement C. The receipt records the UI frame times and dropped frames.

| Lane | Side | Notes |
|---|---|---|
| `metal_shared_event` | Metal | `MTLSharedEvent` signal, listener wakes a semaphore |
| `metal_completed_handler` | Metal | `addCompletedHandler` wakes a semaphore |
| `dawn_imported_timed_wait_any` | Dawn | production storage, `TimedWaitAny` |
| `dawn_imported_process_events_poll50us` | Dawn | production storage, `ProcessEvents` plus the transport's 50 µs poll |
| `dawn_imported_wait_any_poll50us` | Dawn | production storage, non-blocking `WaitAny` plus a 50 µs poll |
| `dawn_staged_timestamps_timed_wait_any` | Dawn | GPU timestamps on; diagnostic only |

The provider refuses timestamps on imported storage
(`timestamps_require_staged_storage`). Dawn's GPU busy time therefore comes
from a staged lane, and the gate never uses that lane as the production Dawn
configuration. The provider also exposes no host-domain completion time. Dawn
wake latency is therefore reported only as an upper bound: submit→visible
minus GPU busy, from the timestamp lane. The Dawn tuning switches planned in
WP-T are not on this base; the receipt records `wp_t_switches` and gains a
lane per switch once they land.

## Metrics per cell

The comparator records raw per-block samples. The verifier computes every
statistic from them.

- **Submit→visible**: p50, p99, p99.9 and max, on the host clock.
- **GPU busy time**: Metal command-buffer start to end; Dawn first pass to
  last pass, from timestamp queries.
- **Worker CPU per block**: thread CPU time, minus the oracle check. Dawn
  runs its completion callbacks on the worker, so they count. Metal runs its
  handlers on its own threads, so they do not. Each cell also records process
  CPU over the whole cell, so the gap can be checked.
- **Wake latency**: Metal measures GPU end → worker observes directly. Dawn
  reports only the upper bound described above.
- **Misses**: a block misses when it is not visible within a two-block lead
  of its release, or when it fails the oracle.

## Running a campaign

```bash
python3 tools/scripts/gpu_audio_comparator_v2.py run \
  --binary build/test/pulp-test-native-metal-audio-comparator \
  --out /tmp/comparator-v2/campaign-1.json
python3 tools/scripts/gpu_audio_comparator_v2.py verify \
  /tmp/comparator-v2/campaign-1.json /tmp/comparator-v2/campaign-2.json
```

`run` takes the P2 host preflight
([gpu-audio-p2-host-preflight.md](gpu-audio-p2-host-preflight.md)) before and
after the binary runs. It then binds four things to a
`pulp.gpu-audio.comparator.v2` receipt: both preflights, the source revision,
the binary digest, and a receipt digest. A receipt counts only when all of the
following hold:

- Both preflights passed at the measured revision.
- Every cell has 10,000 blocks.
- Both loads and all three block sizes ran on both sides.
- Dawn GPU timestamps were available.

Anything else is evaluated and printed, but marked `INCONCLUSIVE`. A noisy
agent host can therefore still produce a dry-run receipt.

The ctest runs `gpu_audio_comparator_v2.py smoke`, which makes two runs:

- A short matched run. It must not be refused, and it must be marked
  undersampled.
- A planted run where the Metal lane uses the old unmatched `complex_multiply`
  kernel. The verifier must refuse it with `kernel_digest_mismatch`.

## Gate thresholds (PROPOSED, not ratified)

The thresholds are evaluated at 64 frames (a 1.333 ms period), on two
consecutive counted campaigns from the same host and Dawn revision. Each
criterion compares the best production-eligible Dawn lane against the best
Metal lane, under each load. The gate fires if any criterion fires in both
campaigns:

1. Dawn p99.9 submit→visible is above 50% of the period, while Metal is
   below 25%.
2. Dawn worker CPU per block is above 2x Metal's and above 10% of the
   period.
3. Dawn wake p99 is above 300 µs, while Metal is below 100 µs. Dawn wake time
   is only bounded from above, so this criterion can be shown not to fire. It
   cannot be shown to fire until the provider exposes a completion time.
4. Under the render load, the Dawn miss rate against a two-block lead is
   above 0.01%, while Metal's is 0.

The verifier reports one of these verdicts:

- `GATE_FIRES`
- `GATE_DOES_NOT_FIRE`
- `INCONCLUSIVE`
- `REFUSED`: an unmatched kernel, a parity or oracle failure, or a tampered
  receipt.

Until the owner ratifies the thresholds, every verdict is advisory.
